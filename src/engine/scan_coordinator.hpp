// Оркестрация сканирования: пул рабочих потоков, прогресс, кооперативная отмена
// (SPEC §6.4 «Потоки и отмена», §4 FR-2/FR-3, §8 Этап 2).
//
// Место в архитектуре (SPEC §6.1): слой Engine. Сканер отдаёт координатору
// задачи («обойти локаторы правил temp.user»), координатор решает, сколько
// потоков работает одновременно, публикует прогресс для UI и обеспечивает
// отмену. Содержимое правил, обход ФС и скоринг — чужие модули (scanner::*,
// core::*, platform::*); здесь только оркестрация, поэтому файл переносимый
// (без windows.h) и собирается на любом хосте — как core (SPEC §6.1, ADR-004).
//
// Три правила §6.4, ради которых модуль существует:
//   1. UI-поток один и не блокируется: прогон идёт в фоновых потоках, счётчики
//      публикуются атомарно, UI читает снимок раз в kProgressInterval (100 мс);
//   2. отмена кооперативная: std::stop_source у координатора, std::stop_token в
//      рабочих потоках и в горячих циклах обхода, проверка раз в
//      kCancelCheckItems (256) элементов;
//   3. результат после публикации не мутируется: отчёт отдаётся вызывающему как
//      std::shared_ptr<const ScanRunReport>, а снимок прогресса — как значение.
//
// Чего модуль НЕ делает намеренно: не знает про диски, правила и кандидатов
// (сборка ScanResult — задача scanner::*/engine::scan_result), не ходит в ФС и
// не трогает UI. Его публичный API — три словаря: задачи на входе, прогресс и
// отчёт на выходе.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mrproper::engine {

// Проверка отмены раз в 256 элементов обхода (SPEC §6.4 — «отмена кооперативная,
// проверка каждые 256 элементов»). Число взято из спеки, а не подобрано: слишком
// часто — лишние атомарные загрузки в самом горячем цикле движка, слишком редко —
// отмена ощущается как зависание на большом каталоге.
inline constexpr std::uint32_t kCancelCheckItems = 256;

// Периодичность публикации прогресса: UI читает счётчики раз в 100 мс (SPEC §6.4).
inline constexpr std::chrono::milliseconds kProgressInterval{100};

// Потолок пула. Скан — это файловый и дисковый ввод-вывод, а не счёт: смысла в
// 64 потоках на 16 ядрах нет, они только накладку добавят. 16 — потолок, ниже
// которого берётся hardware_concurrency.
inline constexpr std::size_t kDefaultMaxWorkers = 16;

// ---------------------------------------------------------------------------
// Отмена (§6.4): CancellationToken = std::stop_token поверх std::stop_source
// ---------------------------------------------------------------------------

// Имена с префиксом Scan — не из вкуса, а потому что `scan_result.hpp` (чужой
// файл, задача scanner::*/engine::scan_result) объявляет в том же самом
// namespace `mrproper::engine` классы с именами ровно `CancellationToken` и
// `CancellationSource`. Одинаковое имя класса в одном namespace — это
// переопределение: любой файл, который включил бы оба заголовка, не собрался бы.
// Переименование здесь снимает мину на будущее, пока у обоих типов ещё нет
// потребителей; слияние двух обёрток (эта умеет кооперативный счётчик
// shouldStop(), та — stopRequestedAt(index)) — задача того, кто пишет
// потребителя прогона, а не молчаливое «победил кто позже».
//
// Токен отмены, который можно носить в IO-цикле.
//
// Два способа спросить про отмену — и они не взаимозаменяемы:
//   * stopRequested() — точная проверка, годится вне горячего цикла;
//   * shouldStop()    — кооперативная: раз в kCancelCheckItems обращений сама
//                       отвечает «работаем дальше», не трогая состояние
//                       остановки. Именно её зовёт обход файлов.
class ScanCancellationToken {
public:
    ScanCancellationToken() = default;
    explicit ScanCancellationToken(std::stop_token token) noexcept;

    [[nodiscard]] std::stop_token native() const noexcept { return token_; }

    // Отмена уже запрошена? Точная проверка (состояние stop_state).
    [[nodiscard]] bool stopRequested() const noexcept;

    // Можно ли этот токен остановить в принципе: пустой stop_token из чужого
    // кода (например, токен по умолчанию из std::jthread без источника).
    [[nodiscard]] bool stoppable() const noexcept;

    // Кооперативная проверка горячего цикла: возвращает true, когда пора
    // прекращать работу. Счётчик обнуляется каждые kCancelCheckItems вызовов,
    // поэтому в оставшихся 255 случаях ответ берётся из обычного счётчика.
    bool shouldStop() noexcept;

    // Сбросить счётчик кооперативных проверок. Нужен перед длинным участком,
    // где проверять отмену не собирались: иначе она случится с задержкой в
    // kCancelCheckItems итераций после последней проверки.
    void resetCancelCountdown() noexcept { countdown_ = kCancelCheckItems; }

private:
    std::stop_token token_;
    std::uint32_t countdown_{kCancelCheckItems};
};

// Источник отмены. Владелец один — ScanCoordinator; копии (ScanCancellationToken)
// раздаются рабочим потокам и IO-циклам.
//
// Именно stop_source, а не собственная обёртка на atomic<bool>: стандартная
// семантика (поток, который ждёт, просыпается по запросу остановки) уже
// реализована в std::jthread и std::condition_variable_any, а своя обёртка
// означала бы вторую, всегда отстающую реализацию (SPEC §6.4, ADR-001).
class ScanCancellationSource {
public:
    ScanCancellationSource() = default;

    [[nodiscard]] ScanCancellationToken token() const noexcept { return ScanCancellationToken(source_.get_token()); }
    [[nodiscard]] std::stop_token native() const noexcept { return source_.get_token(); }

    // Запросить остановку. Идемпотентен и безопасен из любого потока — так
    // отменяет UI-поток по кнопке «Отмена». Возвращает false, если остановка
    // уже была запрошена (информация для журнала, не ошибка).
    bool requestStop() noexcept;

    [[nodiscard]] bool stopRequested() const noexcept { return source_.stop_requested(); }

    // Заменить источник на новый (свежий прогон). Прежний токен у уже идущих
    // рабочих потоков при этом не меняется — он остаётся действительным.
    void reset() noexcept;

private:
    // Источник создаётся именно так: дефолтный stop_source сразу владеет
    // stop_state (MSVC 14.29: stop_source() : _State{new _Stop_state}), тогда
    // как конструктор std::nostopstate дал бы источник, который нельзя
    // остановить, — request_stop() на нём ничего не делает.
    std::stop_source source_;
};

// ---------------------------------------------------------------------------
// Прогресс (§6.4: «счётчик файлов/байт публикуется атомарно»)
// ---------------------------------------------------------------------------

// Снимок прогресса — ровно то, что читает UI. Значение, а не ссылка: полем
// класса ProgressCounter оно быть не может (там атомарные счётчики), а
// ссылка на внутренности заставила бы UI знать про реализацию счётчика.
struct ProgressSnapshot {
    std::uint64_t itemsDone{};   // обработано элементов (файлов, каталогов, локаторов)
    std::uint64_t itemsTotal{};  // 0 — заранее неизвестно: обход ФС не знает счётчика заранее
    std::uint64_t bytesFound{};  // найдено байт (аллоцированных — оценка освобождения, §4 FR-4)
    std::size_t tasksDone{};
    std::size_t tasksTotal{};
    std::size_t workersActive{};
    std::size_t workersTotal{};
    std::chrono::milliseconds elapsed{};
    bool finished{};
    bool cancelled{};

    // Доля готовности 0.0…1.0 для полосы прогресса. 0 — размер неизвестен
    // заранее, интерфейс обязан показать неопределённый индикатор, а не 0 %:
    // «сканирование ничего не нашло» и «ещё не начато» — разные состояния.
    [[nodiscard]] double fraction() const noexcept;
    [[nodiscard]] std::string toText() const;
};

// Единственная точка записи прогресса. Потокобезопасна без мьютекса: счётчики
// атомарные, пишут рабочие потоки, читает UI-поток. Согласованности полей между
// собой снимок не гарантирует (это не транзакция), но значения не «уезжают
// назад»: монотонность важнее попарной согласованности для полосы прогресса.
class ProgressCounter {
public:
    // Новый прогон: обнулить счётчики и начать отсчёт времени. Вызывается до
    // старта рабочих потоков, поэтому гонок нет.
    void begin() noexcept;
    void addItems(std::uint64_t count) noexcept;
    void addBytes(std::uint64_t count) noexcept;
    void setItemsTotal(std::uint64_t total) noexcept;
    void setTasksTotal(std::size_t total) noexcept;
    void setWorkersTotal(std::size_t total) noexcept;
    void taskFinished() noexcept;
    void workerStarted() noexcept;
    void workerFinished() noexcept;
    // Прогон закончен: зафиксировать время конца и признак отмены. После этого
    // elapsed в снимках перестаёт расти.
    void markFinished(bool cancelled) noexcept;

    [[nodiscard]] ProgressSnapshot snapshot() const noexcept;
    [[nodiscard]] std::string toText() const;

private:
    // Наносекунды steady_clock: атомик, чтобы снимок можно было читать из UI
    // одновременно с записью из фонового потока.
    static std::int64_t ticks() noexcept;

    std::atomic<std::uint64_t> items_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> itemsTotal_{0};
    std::atomic<std::size_t> tasksDone_{0};
    std::atomic<std::size_t> tasksTotal_{0};
    std::atomic<std::size_t> workersActive_{0};
    std::atomic<std::size_t> workersTotal_{0};
    std::atomic<std::int64_t> startedTicks_{0};
    std::atomic<std::int64_t> endedTicks_{0};
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancelled_{false};
};

// ---------------------------------------------------------------------------
// Задача
// ---------------------------------------------------------------------------

// Всё, что задаче нужно от координатора: отмену, счётчики прогресса и её имя.
// Копируется один раз на задачу (вне горячего цикла), поэтому внутри цикла
// обращения к нему — только атомарные сложения.
class ScanTaskContext {
public:
    ScanTaskContext(ScanCancellationToken token, ProgressCounter* counter, std::string name,
                    std::string category) noexcept;

    // Точная проверка отмены — для границ крупных этапов задачи.
    [[nodiscard]] bool stopRequested() const noexcept;
    // Кооперативная проверка горячего цикла: раз в kCancelCheckItems элементов.
    bool shouldStop() noexcept;
    [[nodiscard]] std::stop_token stopToken() const noexcept { return token_.native(); }

    void addItems(std::uint64_t count) noexcept;
    void addBytes(std::uint64_t count) noexcept;
    // Сколько уже насчитал сама задача (в отчёт попадает по задаче, а не по
    // прогону: иначе в параллельном прогоне счётчики разных задач смешаются).
    [[nodiscard]] std::uint64_t items() const noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& category() const noexcept { return category_; }

private:
    ScanCancellationToken token_;
    ProgressCounter* counter_{nullptr};  // nullptr — прогресс отключён, счётчики ведутся только свои
    std::string name_;
    std::string category_;
    std::atomic<std::uint64_t> items_{0};
    std::atomic<std::uint64_t> bytes_{0};
};

// Единица работы координатора.
//
// Контракт тела run:
//   * возвращается самостоятельно, проверяя context.shouldStop() в цикле;
//   * не бросает исключений — бросок поймается и станет TaskStatus::Failed,
//     но «упавшая» задача не должна ронять прогон целиком;
//   * не блокирует UI и не ждёт другие задачи: пул не терпит взаимоблокировок.
struct ScanTask {
    std::string name;                  // ключ провайдера: «temp.user», «browser.cache»
    std::string category;              // категория правил (§4 FR-3)
    std::uint64_t expectedItems{};     // 0 — заранее неизвестно; иначе даёт знаменатель прогресса
    std::function<void(ScanTaskContext&)> run;
};

// ---------------------------------------------------------------------------
// Отчёт о прогоне
// ---------------------------------------------------------------------------

// Pending держит в отчёте задачу, которую пул не успел взять: после отмены их
// остаётся много, и «молча пропала задача» в журнале выглядит как потеря работы.
enum class TaskStatus { Pending, Completed, Cancelled, Failed, Skipped };
const char* toString(TaskStatus status) noexcept;

struct TaskReport {
    std::string name;
    std::string category;
    TaskStatus status{TaskStatus::Pending};
    std::uint64_t items{};    // обработано элементов
    std::uint64_t bytes{};    // найдено байт
    std::chrono::milliseconds duration{};
    std::string error;        // текст исключения, если status == Failed
};

struct ScanRunReport {
    std::uint64_t generation{};  // номер прогона, растёт монотонно
    std::vector<TaskReport> tasks;  // в порядке передачи в координатор
    std::size_t workers{};         // сколько потоков реально работало
    std::uint64_t items{};
    std::uint64_t bytes{};
    std::size_t completed{};
    std::size_t cancelledTasks{};
    std::size_t failed{};
    std::size_t skipped{};
    std::chrono::milliseconds duration{};
    bool cancelled{};

    [[nodiscard]] std::size_t count(TaskStatus status) const noexcept;
    [[nodiscard]] std::string toText() const;
};

// Результат неизменяем после публикации: shared_ptr<const …> (SPEC §6.4).
using ScanRunReportPtr = std::shared_ptr<const ScanRunReport>;

// ---------------------------------------------------------------------------
// Пул рабочих потоков
// ---------------------------------------------------------------------------

// Очередь работ на N потоках. Пул живёт внутри одного прогона ScanCoordinator,
// но класс самодостаточен: CleanupExecutor (SPEC §8 Этап 3) использует его так
// же — параллельное удаление со своей отменой.
//
// Особенности, ради которых он написан, а не взят готовым:
//   * stop_token задаётся снаружи и общий с прогоном: одна кнопка «Отмена»
//     останавливает и обход, и удаление;
//   * submit() не ждёт и не блокируется — вложенная постановка работы не может
//     исчерпать пул и вызвать взаимоблокировку;
//   * работа, начатая до отмены, доходит до конца: прервать синхронный вызов
//     Win32 нельзя (то же решение, что у platform::InventoryCache).
class WorkerPool {
public:
    // workers — число потоков (0 → один). stop — токен отмены прогона.
    // progress — необязательный счётчик: пул сообщает ему, сколько потоков
    // занято сейчас, чтобы полоса прогресса не показывала «0 из 8» работающих.
    WorkerPool(std::size_t workers, std::stop_token stop, ProgressCounter* progress = nullptr);
    // Дренирует очередь и присоединяется к потокам. Если в момент уничтожения
    // работа ещё идёт, деструктор её дождётся: бросать поток работать с
    // разрушенным пулом — хуже, чем подождать.
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    // Поставить работу. false — пул закрыт или остановлен: работа не будет
    // выполнена (координатор в этом случае помечает задачу как Skipped).
    // Не noexcept: постановка в очередь выделяет память, и обрывать прогон
    // terminate из-за нехватки памяти хуже, чем пробросить std::bad_alloc.
    bool submit(std::function<void()> job);

    // Дождаться, пока очередь опустеет и ни одна работа не выполняется.
    void drain() noexcept;
    // drain() + остановка и присоединение потоков. Повторный вызов безопасен.
    void close() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;      // потоков в пуле
    [[nodiscard]] std::size_t pending() const noexcept;   // работ в очереди
    [[nodiscard]] std::size_t active() const noexcept;    // выполняется сейчас
    [[nodiscard]] std::size_t completed() const noexcept; // выполнено всего
    [[nodiscard]] std::string toText() const;

private:
    void workerMain(std::stop_token token);
    void runJob(std::function<void()> job) noexcept;
    void dropQueueLocked() noexcept;  // только под mutex_

    std::stop_token stop_;
    ProgressCounter* progress_{nullptr};
    std::size_t size_{1};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> queue_;
    std::size_t active_{0};
    std::size_t completed_{0};
    bool closing_{false};
    bool joined_{false};
    std::vector<std::jthread> workers_;
};

// ---------------------------------------------------------------------------
// Координатор
// ---------------------------------------------------------------------------

struct ScanCoordinatorOptions {
    // Число рабочих потоков. 0 — выбрать автоматически: по числу задач и
    // hardware_concurrency, с ограничением maxWorkers.
    std::size_t workers{};
    std::size_t maxWorkers{kDefaultMaxWorkers};
    // Как часто UI читает прогресс (§6.4 — раз в 100 мс).
    std::chrono::milliseconds progressInterval{kProgressInterval};

    // Колбэк прогресса. Вызывается из фонового потока координатора, поэтому
    // UI обязан маршалить его в свой поток сам (PostMessage) — либо передать
    // dispatcher ниже и не думать об этом. Исключение из колбэка ловится и
    // логируется: интерфейс не имеет права ронять прогон сканирования.
    std::function<void(const ProgressSnapshot&)> onProgress{};
    // Маршалинг колбэка: вызывающий передаёт свою функцию «выполнить в UI-потоке»
    // (в приложении — обёртку над PostMessage). Не задан — колбэк зовётся
    // напрямую из потока координатора (так удобно CLI и тестам).
    std::function<void(std::function<void()>&&)> dispatcher{};

    // Писать прогресс и финал прогона в журнал. В UI избыточно (там своя
    // полоса), в CLI и при разборе «почему сканирование шло долго» — нужно.
    bool logProgress{true};
};

// Единственный владелец пула, прогресса и отмены прогона.
//
// Не копируется и не перемещается: внутри живёт std::jthread и stop_source.
// Экземпляр держит тот, кто переживает окно (app shell) или процесс (CLI).
//
// Потокобезопасность. start(), run(), requestStop(), progress(), result(),
// running() и token() можно звать из UI- и рабочих потоков одновременно.
// Сам прогон всегда идёт в фоне: UI-поток не блокируется (SPEC §6.4).
class ScanCoordinator {
public:
    explicit ScanCoordinator(ScanCoordinatorOptions options = {});
    // Запрашивает остановку и присоединяется к фоновому потоку прогона, если он
    // ещё идёт.
    ~ScanCoordinator();

    ScanCoordinator(const ScanCoordinator&) = delete;
    ScanCoordinator& operator=(const ScanCoordinator&) = delete;
    ScanCoordinator(ScanCoordinator&&) = delete;
    ScanCoordinator& operator=(ScanCoordinator&&) = delete;

    // Запустить прогон в фоне и вернуться сразу. false — уже идёт прогон
    // (повторный клик «Сканировать» игнорируется, а не затирает текущий).
    bool start(std::vector<ScanTask> tasks);
    // Блокирующий прогон: start() + ожидание + отчёт. Для CLI и тестов; из
    // UI-потока не вызывается. Если прогон уже идёт, повторный вызов его не
    // прерывает, а возвращает отчёт текущего (ещё не завершённого) прогона.
    [[nodiscard]] ScanRunReportPtr run(std::vector<ScanTask> tasks);

    // Дождаться конца текущего прогона (waitFor возвращает false по таймауту).
    bool wait();
    bool waitFor(std::chrono::milliseconds timeout);

    // Запросить остановку: из UI-потока по кнопке «Отмена». Отмена
    // кооперативная — уже начатые задачи доходят до конца, не начатые
    // помечаются Skipped. Идемпотентно, безопасно вне прогона.
    void requestStop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;  // номер последнего запуска
    [[nodiscard]] ProgressSnapshot progress() const noexcept;  // снимок для UI
    [[nodiscard]] ScanRunReportPtr result() const noexcept;    // nullptr, пока не опубликован
    // Токен текущего прогона — для кода вне координатора (например, чтобы
    // прервать долгий обход, уже начатый провайдером).
    [[nodiscard]] ScanCancellationToken token() const noexcept;
    [[nodiscard]] std::string toText() const;  // состояние для журнала и баг-репорта

private:
    void runLoop();
    // Пул + постановка задач. Отдельный метод, чтобы runLoop() мог обернуть его
    // в try/catch: из потока прогона наружу exception() означает std::terminate.
    void runTasks(std::stop_token token, std::vector<TaskReport>& reports);
    // Досчёт незакрытых задач, сборка ScanRunReport и его публикация. Счётчики
    // прогресса закрываются здесь же: последний снимок должен уйти до того, как
    // UI увидит running() == false.
    void publishReport(std::stop_token token, std::vector<TaskReport>& reports, bool aborted,
                       const std::string& failure);
    void runTask(std::stop_token token, std::size_t index, std::vector<TaskReport>& reports);
    void progressLoop(std::stop_token token, const std::atomic<bool>& active);
    void emitProgress(const ProgressSnapshot& snapshot);
    void logRunStart(std::size_t tasks) const;
    void logRunFinish(const ScanRunReport& report) const;
    [[nodiscard]] std::size_t resolveWorkers(std::size_t tasks) const noexcept;

    ScanCoordinatorOptions options_;

    mutable std::mutex stateMutex_;
    std::condition_variable stateCv_;
    std::stop_source stop_;        // источник отмены текущего прогона
    std::stop_token runToken_;     // его копия: раздаётся рабочим потокам
    std::vector<ScanTask> tasks_;  // хранилище задач на время прогона
    std::jthread runner_;
    ProgressCounter progress_;
    ScanRunReportPtr result_;
    std::uint64_t generation_{0};
    bool running_{false};
};

}  // namespace mrproper::engine
