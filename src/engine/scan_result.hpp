// Снимок результата сканирования и статистика прогона (SPEC §6.4, §6.3).
//
// Зачем модуль. Скан идёт в фоновом пуле, UI читает результат параллельно с ним,
// и §6.4 требует трёх вещей, которых не даёт «просто структура»:
//   1) результат не мутируется после публикации — наружу отдаётся только
//      shared_ptr<const ScanResult>. shared_ptr<ScanResult> отдавать нельзя:
//      любой держатель допишет в снимок, пока UI его рисует;
//   2) счётчики файлов и байт публикуются атомарно, UI читает их раз в 100 мс —
//      прогресс не ходит через мьютекс, который в горячем цикле обхода стоил бы
//      дороже самой работы;
//   3) отмена кооперативная (std::stop_source / std::stop_token, C++20) и
//      проверяется каждый 256-й элемент, а не на каждом.
//
// Что модуль делает:
//   * ScanProgress — счётчики прогона для UI (фаза, файлы, байты, текущий путь);
//   * CancellationToken / CancellationSource — отмена с шагом проверки из §6.4;
//   * ScanResultBuilder — единственный способ собрать ScanResult: наполнение,
//     пересчёт агрегатов, проверка согласованности и публикация;
//   * ScanResultSlot — место публикации: «последний снимок» для UI и CLI;
//   * validateScanResult — инварианты §6.3 на готовом снимке.
//
// Разделение слоёв (SPEC §6.1, ADR-004). Модуль переносимый: ни windows.h, ни
// COM, ни ввода-вывода. Числа добывает платформа, провайдеры категорий —
// scanner::*, а здесь только снимок, агрегаты и статистика. Время приходит двумя
// часами: system_clock — для отчёта (FR-8), steady_clock — для длительности
// (часы прыгают при переводе времени, а «скан занял 60 с» должно быть правдой).
//
// Чего модуль НЕ делает (границы, чтобы работа не расползлась):
//   * не запускает скан и не знает провайдеров категорий — это
//     scanner::* и engine::ScanCoordinator;
//   * не строит план очистки — это core::plan, он получает готовый список
//     кандидатов и опирается на их порядок;
//   * не печатает JSON отчёта — это core::report_json;
//   * не ходит по диску и ничего не удаляет.
//
// Детерминированность (SPEC §11.4, golden-тесты). Кандидаты остаются в порядке
// сбора: индексы в них — это ссылки плана (core::plan), и перестановка внутри
// снимка сломала бы соответствие «план ↔ кандидат». Для показа крупных
// элементов есть largestCandidateIndexes(), которая сортирует индексы, но не
// трогает снимок. Агрегаты по категориям отсортированы по убыванию
// освобождаемого места, затем по имени.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model.hpp"

namespace mrproper::engine {

// ---------------------------------------------------------------------------
// Константы §6.4
// ---------------------------------------------------------------------------

// Проверять отмену каждый 256-й элемент — так требует §6.4. Проверка на каждом
// элементе съела бы заметную долю времени обхода, а один раз на 256 элементов
// добавляет к задержке отмены единицы миллисекунд даже на самом длинном IO.
inline constexpr std::size_t kCancelCheckStride = 256;

// Период чтения прогресса UI-потоком (§6.4: «счётчик файлов/байт публикуется
// атомарно, UI читает раз в 100 мс»). Чаще читать незачем: цифры на экране не
// меняются быстрее, чем человек их видит, а чаще — значит чаще брать мьютекс пути.
inline constexpr std::chrono::milliseconds kProgressPollInterval{100};

// Больше этого числа замечаний в снимок не попадает. Снимок не должен расти из-за
// тысяч однотипных отказов (SPEC §5: «Память ≤ 150 МБ»), поэтому счётчик
// потерянных замечаний хранится отдельно — в ScanStats::issuesDropped.
inline constexpr std::size_t kMaxScanIssues = 512;

// ---------------------------------------------------------------------------
// Отмена (SPEC §6.4)
// ---------------------------------------------------------------------------

// Тонкая обёртка над std::stop_token. Существует ради двух вещей: чтобы движок
// не зависел от того, как устроена отмена внутри std, и ради шага проверки
// kCancelCheckStride, который в §6.4 задан числом, а не «раз в столько-то».
//
// Обёртка ничего не добавляет к состоянию: пустой токен (конструктор по
// умолчанию) — это токен, который никогда не остановится. Поэтому вызывающий
// может не заводить источник отмены вообще, а проверки будут честно давать false.
class CancellationToken {
public:
    CancellationToken() = default;
    explicit CancellationToken(std::stop_token token) noexcept : token_(std::move(token)) {}

    // Отмена запрошена прямо сейчас.
    [[nodiscard]] bool stopRequested() const noexcept { return token_.stop_requested(); }

    // Проверка отмены в горячем цикле по номеру элемента: проверяется элемент 0,
    // kCancelCheckStride, 2*kCancelCheckStride… Ровно то, что описано в §6.4.
    // indexOf — то, чем считают цикл: filesDone, индекс в векторе, общий счётчик
    // обхода (нужно одно число на все пути, иначе проверка «сбросится» на каждом).
    [[nodiscard]] bool stopRequestedAt(std::uint64_t indexOf) const noexcept {
        if ((indexOf % static_cast<std::uint64_t>(kCancelCheckStride)) != 0) return false;
        return token_.stop_requested();
    }

    // Может ли этот токен вообще когда-нибудь остановиться. false — источника
    // отмены нет, и координатору незачем создавать CancellationSource.
    [[nodiscard]] bool stopPossible() const noexcept { return token_.stop_possible(); }

    // Токен для модулей платформы, которые принимают std::stop_token напрямую
    // (platform::vfs_delete, platform::process_control).
    [[nodiscard]] std::stop_token native() const noexcept { return token_; }

private:
    std::stop_token token_{};
};

// Владелец отмены: его создаёт ScanCoordinator на прогон и отдаёт токен пулу.
// Копируем и перемещаем (stop_source это позволяет), но источник один на прогон:
// сбросить stop_source нельзя, а «погоняем отменённый прогон» — не то поведение,
// которое нужно приложению. Новый прогон = новый источник.
class CancellationSource {
public:
    CancellationSource() = default;
    explicit CancellationSource(std::stop_source source) noexcept : source_(std::move(source)) {}

    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(source_.get_token()); }
    [[nodiscard]] std::stop_token nativeToken() const noexcept { return source_.get_token(); }

    // Просить отмену. true — запрос был новым, false — уже просили (кнопка
    // «Отмена» может быть нажата повторно, и это не ошибка).
    bool requestCancel() noexcept { return source_.request_stop(); }

    [[nodiscard]] bool cancelRequested() const noexcept { return source_.stop_requested(); }

private:
    std::stop_source source_{};
};

// ---------------------------------------------------------------------------
// Прогресс прогона
// ---------------------------------------------------------------------------

// Фаза прогона: UI показывает одну строку «сейчас делаем…», а конец прогона
// отличается от отмены отдельным значением (FR-6: отмена оставляет систему в
// согласованном состоянии, и это должно быть видно, а не «нулевой результат»).
enum class ScanPhase : std::uint8_t {
    Idle,        // прогон не начат
    Inventory,   // карта дисков (§6.4: обход IOCTL в пуле)
    Candidates,  // обход файловых систем по правилам
    Scoring,     // оценка кандидатов (core::scoring)
    Finalizing,  // пересчёт агрегатов и сборка снимка
    Done,        // снимок опубликован целиком
    Cancelled,   // пользователь нажал «Отмена» (§6.4)
    Failed,      // прогон не удался и снимок неполон
};

const char* scanPhaseName(ScanPhase phase) noexcept;

// Согласованный набор счётчиков, который UI читает раз в kProgressPollInterval.
// Это значение, а не ссылка на живые счётчики: UI рисует то, что прочитал, и
// не должен блокировать пул на каждом кадре.
struct ScanProgressSnapshot {
    ScanPhase phase{ScanPhase::Idle};
    std::uint64_t generation{};
    std::uint64_t filesScanned{};
    std::uint64_t directoriesScanned{};
    std::uint64_t bytesScanned{};         // аллоцированных — по ним считается освобождаемое место (FR-4)
    std::uint64_t logicalBytesScanned{};  // логических — для объяснения разницы
    std::uint64_t candidatesFound{};
    std::uint64_t errors{};
    std::uint64_t skipped{};  // не совпало с правилом, reparse, защищённый путь
    std::uint64_t cancelRequests{};
    bool cancelRequested{};
    std::string currentPath;  // где идёт обход прямо сейчас
    std::uint64_t totalBytes{};  // сколько байт ожидается; 0 — объём неизвестен заранее
    std::chrono::milliseconds elapsed{};
    std::int64_t startedAtUnix{};  // 0 — платформа не передала время
    std::int64_t nowUnix{};        // 0 — то же

    // Доля пройденного по известному объёму: 0.0 при неизвестном объёме (нет
    // смысла показывать 0 % и думать, что скан стоит). UI обязан различать
    // «не знаем» и «ноль» — иначе индикатор мигает в обе стороны.
    [[nodiscard]] double fraction() const noexcept;
};

// Живые счётчики прогона. Один объект на прогон, пишут все потоки пула, читает
// UI. Числа — атомарные с relaxed-упорядочиванием: счётчик не публикует данные,
// требующие синхронизации с другим счётчиком, а лишние барьеры в горячем цикле
// обхода (SPEC §5: «не более 8 потоков», «потоковая обработка») недопустимы.
//
// Чего счётчики не гарантируют: что пара filesScanned/bytesScanned относится к
// одному моменту времени. Каждое значение настоящее, просто обновляются они
// по одному; для индикатора с периодом 100 мс это неразличимо, а «строгий» снимок
// пары потребовал бы блокировки в горячем цикле. Если профилировщик однажды
// покажет ложное разделение строк между потоками пула — это повод добавить
// padding, но не раньше: alignas в структуре под /W4 даёт C4324, а с ним /WX
// роняет сборку.
//
// currentPath — единственное исключение из «без блокировок»: это std::string,
// а менять её на каждом файле расточительно. Поэтому путь меняет только
// координатор (не чаще раза на смену каталога) под коротким мьютексом, а читает
// UI — раз в kProgressPollInterval. Горячий цикл мьютекс не трогает.
class ScanProgress {
public:
    ScanProgress() = default;

    ScanProgress(const ScanProgress&) = delete;
    ScanProgress& operator=(const ScanProgress&) = delete;
    ScanProgress(ScanProgress&&) = delete;
    ScanProgress& operator=(ScanProgress&&) = delete;
    ~ScanProgress() = default;

    // Начать прогон: обнуляет счётчики, запоминает номер прогона и момент
    // старта. Вызывается до старта потоков, поэтому write/read happens-before
    // тут не нужен, а поднятый флаг begun_ позволяет читателю не смотреть на
    // start_ до инициализации.
    void begin(std::uint64_t generation, std::int64_t startedAtUnix = 0) noexcept;

    // Ход прогона. Счётчики только растут, поэтому при отмене снимок остаётся
    // осмысленным: «сколько успели до отмены» — тоже ответ.
    void setPhase(ScanPhase phase) noexcept;
    void addFile(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept;
    void addDirectory() noexcept;
    void addCandidate() noexcept;
    void addError() noexcept;
    void addSkipped() noexcept;

    // Ожидаемый объём: сколько байт впереди, если провайдер умеет его назвать
    // заранее (обход каталога сначала меряет, потом идёт). Ноль означает
    // «неизвестно», и это НЕ «пусто»: индикатор без знаменателя показывает
    // бесконечную полосу, поэтому fraction() честно отдаёт 0.0.
    void setTotalBytes(std::uint64_t totalBytes) noexcept;
    void addTotalBytes(std::uint64_t totalBytes) noexcept;

    // Где идёт обход. Мьютекс; вызывающий обязан звать её не на каждый файл, а
    // на смену каталога.
    void setCurrentPath(std::string path);

    // Отмена запрошена (кнопка «Отмена» или выход из приложения). Только для
    // показа: саму отмену делает CancellationSource, иначе счётчик и источник
    // могли бы разойтись.
    void noteCancelRequest() noexcept;
    void noteCancelRequests(std::uint64_t count) noexcept;

    [[nodiscard]] ScanPhase phase() const noexcept;
    [[nodiscard]] bool cancelRequested() const noexcept;

    // Чтение UI. Не помечено noexcept: внутри мьютекс и выделение под currentPath.
    [[nodiscard]] ScanProgressSnapshot snapshot() const;

private:
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> files_{0};
    std::atomic<std::uint64_t> directories_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> logicalBytes_{0};
    std::atomic<std::uint64_t> candidates_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> skipped_{0};
    std::atomic<std::uint64_t> cancelRequests_{0};
    std::atomic<std::uint64_t> totalBytes_{0};
    std::atomic<std::uint8_t> phase_{static_cast<std::uint8_t>(ScanPhase::Idle)};
    std::atomic<std::int64_t> startedAtUnix_{0};
    std::atomic<bool> begun_{false};

    std::chrono::steady_clock::time_point start_{};

    mutable std::mutex pathMutex_{};
    std::string path_;
};

// ---------------------------------------------------------------------------
// Статистика прогона
// ---------------------------------------------------------------------------

// Что измерял прогон. Не то же самое, что ScanTotals: totals — свойства
// результата (что нашли), stats — свойства процесса (чем он обошёлся). Разделение
// нужно, чтобы «скан занял 4 минуты на 12 потоках с 30 ошибками» нельзя было
// спутать с «нашли 1,2 ГБ».
struct ScanStats {
    std::int64_t startedAtUnix{};   // 0 — платформа не передала время (FR-8)
    std::int64_t finishedAtUnix{};  // 0 — то же
    std::chrono::milliseconds duration{};  // по steady_clock: часы могли прыгнуть

    std::uint64_t filesScanned{};
    std::uint64_t directoriesScanned{};
    std::uint64_t bytesScanned{};         // аллоцированных
    std::uint64_t logicalBytesScanned{};  // логических

    std::size_t candidateCount{};
    std::size_t ruleCount{};       // правил, по которым шёл скан
    std::size_t workerThreads{};   // потоков в пуле (SPEC §5: не более 8)
    std::size_t errorCount{};
    std::size_t issuesDropped{};   // замечаний не поместилось в список (kMaxScanIssues)
    std::size_t skippedCount{};    // не совпало с правилом / reparse / защищённый путь
    std::size_t lockedCandidateCount{};  // файлы держат приложения (FR-4 LockedBy)

    // Отменён ли прогон по требованию пользователя (§6.4). Такой результат
    // показывается с пометкой: он неполон, и неполнота здесь ожидаемая.
    bool cancelled{};
    // Прогон закончился с потерями: отказ диска, таймаут, битое правило (FR-6:
    // «ошибки не фатальны: собираются в отчёт»). Результат показывается целиком
    // и с пояснением, а не выбрасывается.
    bool degraded{};

    std::string cancelReason;    // по-русски, одно предложение
    std::string degradedReason;  // то же про потери
};

// Агрегат по уровню безопасности (SPEC §4 FR-4). Risky сюда попадает, но в
// интерфейсе скрыт по умолчанию — считать его всё равно нужно, иначе «сколько
// всего можно освободить» посчитано по неполному списку.
struct SafetyTotals {
    std::size_t candidateCount{};
    std::uint64_t allocatedBytes{};
    std::uint64_t logicalBytes{};
    std::size_t lockedCount{};
};

// Строка дерева на экране «Очистка» (FR-5).
struct CategoryTotals {
    std::string category;
    std::size_t candidateCount{};
    std::uint64_t allocatedBytes{};  // освобождаемое место — по аллоцированному (FR-4)
    std::uint64_t logicalBytes{};
    std::uint64_t fileCount{};
    std::size_t lockedCount{};
    SafetyTotals safe{};
    SafetyTotals review{};
    SafetyTotals risky{};
};

// Агрегаты снимка. Считаются один раз при публикации: UI перерисовывает дерево
// часто, а пересчёт по сотням кандидатов на каждый кадр — лишняя работа
// (SPEC §5 «Память/производительность»).
struct ScanTotals {
    std::size_t candidateCount{};
    std::uint64_t allocatedBytes{};  // «сколько освободим» — большая цифра на экране
    std::uint64_t logicalBytes{};
    std::uint64_t fileCount{};
    SafetyTotals safe{};
    SafetyTotals review{};
    SafetyTotals risky{};

    std::size_t diskCount{};
    std::size_t partitionCount{};
    std::size_t volumeCount{};
    std::uint64_t diskBytes{};
    std::uint64_t volumeTotalBytes{};
    std::uint64_t freeBytes{};

    // По убыванию allocatedBytes, при равенстве — по имени категории.
    std::vector<CategoryTotals> categories;

    // Доля освобождаемого от занятого места по томам: «свободно станет на X %».
    // 0.0, если тома не опрошены, — UI обязан это отличать от «свободно 0 %».
    [[nodiscard]] double reclaimableFraction() const noexcept;

    [[nodiscard]] const CategoryTotals* category(std::string_view name) const noexcept;
};

// ---------------------------------------------------------------------------
// Замечания прогона
// ---------------------------------------------------------------------------

enum class ScanIssueLevel : std::uint8_t { Info, Warning, Error };
const char* scanIssueLevelName(ScanIssueLevel level) noexcept;

// Этап, на котором что-то не сработало. Нужен не для красоты: «диск не ответил»
// (FR-1) и «набор правил не загрузился» (ADR-008) — разные проблемы с разными
// последствиями, и по одному тексту их не различить.
enum class ScanStage : std::uint8_t {
    Inventory,   // карта дисков
    RuleSet,     // набор правил (загрузка, версия, подпись)
    Candidates,  // находка кандидатов
    Walking,     // обход файловой системы
    Scoring,     // оценка кандидатов
    Publishing,  // сборка и публикация снимка
};

const char* scanStageName(ScanStage stage) noexcept;

// Замечание прогона. Не фатально: SPEC §5 и §12 требуют, чтобы ни один отказ не
// ронял процесс, поэтому неудача — это запись в списке, а не исключение.
struct ScanIssue {
    ScanStage stage{ScanStage::Walking};
    ScanIssueLevel level{ScanIssueLevel::Warning};
    std::string subject;   // путь, идентификатор правила, номер тома
    std::string message;   // по-русски, одно предложение
    std::uint32_t code{};  // HRESULT/Win32, если отказ системный; 0 — «без кода»
    std::int64_t atUnix{}; // когда заметили; 0 — время не передано
};

// ---------------------------------------------------------------------------
// Снимок
// ---------------------------------------------------------------------------

// Снимок результата сканирования. Состав полей — §6.3 (disks, candidates,
// stats), остальное — агрегаты и признаки прогона, без которых UI и отчёт
// считали бы одно и то же заново.
//
// Иммутабельность обеспечивается не «договоримся», а типом: наружу отдаётся
// только shared_ptr<const ScanResult> (см. ScanResultPtr и ScanResultBuilder).
// Поля публичные — как у всех структур модели в core (§6.3), — но до константной
// ссылки добраться нельзя.
struct ScanResult {
    std::vector<core::PhysicalDisk> disks;
    std::vector<core::CleanupCandidate> candidates;
    ScanStats stats;
    ScanTotals totals;
    std::vector<ScanIssue> issues;  // не больше kMaxScanIssues

    std::string rulesVersion;  // версия набора правил, по которым шёл скан (FR-8)
    std::uint64_t generation{};  // монотонный номер прогона: «снимок новый» без сравнения содержимого
    std::chrono::system_clock::time_point finishedAt{};
    // Прогон не прерывался пользователем. Не то же, что «всё хорошо»: при
    // degraded == true результат показывается целиком, но с замечаниями
    // (FR-6: «ошибки не фатальны: собираются в отчёт»). У UI три состояния:
    // complete && !degraded — полный результат; complete && degraded — полный
    // с замечаниями; !complete — прерван пользователем.
    bool complete{};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const core::PhysicalDisk* diskByNumber(int number) const noexcept;
    [[nodiscard]] const CategoryTotals* category(std::string_view name) const noexcept;

    // Кандидаты по убыванию освобождаемого места — крупные сверху (FR-5). Снимок
    // не переставляется: индексы в нём — это ссылки плана (core::plan).
    [[nodiscard]] std::vector<std::size_t> largestCandidateIndexes(std::size_t limit) const;

    // Одна строка для UI и для отчёта: «Освободится 1,2 ГБ — 214 элементов
    // в 8 категориях».
    [[nodiscard]] std::string headline() const;

    // Статистика прогона текстом: журнал и баг-репорт (FR-8 — «время, версии,
    // ошибки»). Ошибки перечисляются ограниченным числом строк, остальное
    // считается: список на 512 записей в баг-репорте бесполезен.
    [[nodiscard]] std::string toText() const;

    // Дешёвый отпечаток снимка: состав и ключевые числа. UI использует его, чтобы
    // понять «другой результат» без сравнения векторов, журнал — чтобы связать
    // два сообщения об одном прогоне. Не является криптографической суммой.
    [[nodiscard]] std::uint64_t signature() const noexcept;
};

// Единственный тип, который имеет право ходить по снимку наружу (SPEC §6.4).
using ScanResultPtr = std::shared_ptr<const ScanResult>;

// ---------------------------------------------------------------------------
// Сборщик
// ---------------------------------------------------------------------------

// Параметры прогона, которые знает координатор, а не счётчики.
struct ScanBuildOptions {
    std::uint64_t generation{1};      // номер прогона
    std::string rulesVersion;         // FR-8: по какому набору правил шли
    std::size_t workerThreads{1};     // сколько потоков в пуле на самом деле
    std::size_t ruleCount{};          // правил в наборе
};

// Единственный способ создать ScanResult. Логика простая и проверяемая: пока
// publish() не вызван, снимка не существует; после publish() сборщик запечатан и
// больше не может ни принять элемент, ни изменить уже опубликованное.
//
// Сборщик не потокобезопасен и не должен быть: его ведёт один поток (пул
// отдаёт кандидатов через очередь), а публикует он снимок ровно один раз. Публикация
// снимка — точка, где «последние» данные уже никто не изменит, и единственное
// место, где нужно пережить гонку, — до него.
class ScanResultBuilder {
public:
    explicit ScanResultBuilder(ScanBuildOptions options = {});

    ScanResultBuilder(const ScanResultBuilder&) = delete;
    ScanResultBuilder& operator=(const ScanResultBuilder&) = delete;
    ScanResultBuilder(ScanResultBuilder&&) = default;
    ScanResultBuilder& operator=(ScanResultBuilder&&) = default;
    ~ScanResultBuilder() = default;

    const ScanBuildOptions& options() const noexcept { return options_; }

    // Наполнение. false — элемент не принят: сборщик запечатан (publish() уже
    // вызван) или кандидат без пути. Молча проглоченный элемент — это «цифры в
    // UI не сходятся с отчётом», поэтому отказ всегда можно посчитать:
    // rejected().
    bool addDisk(core::PhysicalDisk disk);
    bool addCandidate(core::CleanupCandidate candidate);
    // false — список замечаний переполнен (kMaxScanIssues); счётчик потерянных
    // попадёт в ScanStats::issuesDropped.
    bool addIssue(ScanIssue issue);
    // Отказ без подробностей: счётчик ошибок растёт, запись в список не делается.
    void addError();

    // Итог прогона словами: «пользователь отменил», «диск не ответил» (FR-6:
    // ошибки не фатальны, но должны быть видны). Причина складывается, а не
    // затирается: отмену часто сопровождают отказы дисков, и обе причины нужны.
    void noteCancelled(std::string reason);
    void noteDegraded(std::string reason);

    [[nodiscard]] bool sealed() const noexcept { return sealed_; }
    [[nodiscard]] std::size_t rejected() const noexcept { return rejected_; }
    [[nodiscard]] std::size_t errorCount() const noexcept { return errors_; }
    [[nodiscard]] const std::vector<ScanIssue>& issues() const noexcept { return issues_; }

    // Опубликовать снимок: пересчитать агрегаты, перенести счётчики прогона в
    // ScanStats, запечатать сборщик и отдать иммутабельный указатель. Повторный
    // вызов возвращает тот же снимок (сборщик не пересобирает его второй раз) —
    // это позволяет вызвать publish() из двух мест, не задумываясь о порядке.
    [[nodiscard]] ScanResultPtr publish(const ScanProgressSnapshot& progress);
    [[nodiscard]] ScanResultPtr publish(const ScanProgress& progress);

    // Инварианты §6.3 на собранном, но ещё не опубликованном составе. Публикует
    // ли что-то — вызывающий решает сам: результат с замечаниями в issues лучше,
    // чем ничего (SPEC §12), но публиковать надо с пониманием, что именно не так.
    [[nodiscard]] std::vector<std::string> validate() const;

private:
    ScanBuildOptions options_;
    std::vector<core::PhysicalDisk> disks_;
    std::vector<core::CleanupCandidate> candidates_;
    std::vector<ScanIssue> issues_;
    std::size_t errors_{};
    std::size_t issuesDropped_{};
    std::size_t rejected_{};
    bool cancelled_{};
    bool degraded_{};
    std::string cancelReason_;
    std::string degradedReason_;
    bool sealed_{};
    ScanResultPtr published_;
};

// ---------------------------------------------------------------------------
// Публикация
// ---------------------------------------------------------------------------

// «Последний снимок» для UI, CLI и отчёта: чтение не блокирует сбор, потому что
// снимок иммутабелен и его достаточно удержать shared_ptr.
//
// Указатель защищён коротким мьютексом, а не std::atomic<std::shared_ptr>: в
// тулчейне проекта (MSVC 19.29, v142) этого типа ещё нет, а критическая секция —
// одно копирование указателя, в горячий цикл она не входит.
class ScanResultSlot {
public:
    ScanResultSlot() = default;

    ScanResultSlot(const ScanResultSlot&) = delete;
    ScanResultSlot& operator=(const ScanResultSlot&) = delete;
    ScanResultSlot(ScanResultSlot&&) = delete;
    ScanResultSlot& operator=(ScanResultSlot&&) = delete;
    ~ScanResultSlot() = default;

    // Текущий снимок; nullptr, пока не было ни одной публикации.
    [[nodiscard]] ScanResultPtr get() const;

    // Опубликовать новый снимок. nullptr — очистить слот (сброс прогресса при
    // выходе из приложения). Старый снимок у держателей не исчезает: его
    // shared_ptr живёт, пока на него смотрят.
    void publish(ScanResultPtr result);

    void clear();

    [[nodiscard]] bool hasValue() const;
    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] std::size_t publishCount() const;

private:
    mutable std::mutex mutex_;
    ScanResultPtr current_;
    std::size_t publishCount_{};
};

// Нарушения инвариантов §6.3 и внутренних соглашений снимка. Пустой список —
// снимок согласован. Возвращаются строки по-русски: они идут в журнал и в
// баг-репорт, где английский текст ничего не объясняет пользователю.
//
// Чего здесь нет и почему. Пара allocatedBytes / logicalBytes НЕ проверяется на
// «allocated ≤ logical» (§6.3 упоминает это как инвариант, но с оговоркой):
// core::sizing::classifySize считает нормальным случай, когда аллоцированный
// БОЛЬШЕ логического — выравнивание по кластеру и сжатие. Ошибкой это делается
// только на фикстуре со sparse-файлами, то есть в тестах размера, а не в общем
// валидаторе. Привязка тома к разделу и прочие свойства карты дисков —
// зона core::validateInventory, здесь они не дублируются.
[[nodiscard]] std::vector<std::string> validateScanResult(const ScanResult& result);

}  // namespace mrproper::engine
