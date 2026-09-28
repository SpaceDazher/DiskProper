// Исполнение плана очистки: пул рабочих потоков, чек-лист операций, Skip (locked)
// для занятых путей и сбор ошибок (SPEC §4 FR-6, §8 Этап 3).
//
// Место в архитектуре (SPEC §6.1): слой Engine, рядом с ScanCoordinator. План
// (core::plan) отвечает на вопрос «что и как делать», исполнитель — на вопрос
// «сделать ли это вообще, в каком порядке потоками и что из этого получилось».
// Содержимое правил, обход ФС, скоринг и модули платформы — чужие: здесь только
// оркестрация, поэтому файл переносимый (без windows.h) и читается без
// знания Win32, как scan_coordinator и locks (SPEC §6.1, ADR-004).
//
// Что именно требует FR-6 и где это в файле:
//
//   * «Фоновые потоки (пул), отмена через CancellationToken, прогресс — через
//     колбэк в UI-поток» — CleanupExecutor владеет std::stop_source прогона,
//     работами в engine::WorkerPool (тот же пул, что у сканера) и счётчиком
//     engine::ProgressCounter. Колбэк зовётся из фонового потока, поэтому
//     маршалинг в UI-поток (PostMessage) — забота вызывающего: для этого в
//     опциях есть dispatcher.
//   * «Защита от съедания чужих данных» — четыре проверки выполняются ДО любой
//     операции, в порядке из FR-6: пропуск reparse points, нормализация пути и
//     принадлежность корню правила (platform::vfs_paths::checkRuleRoot — снимает
//     8.3 и «проходит» symlink), белый список защищённых каталогов, проверка
//     «файл не изменился после сканирования» (§10). Любой отказ здесь даёт
//     пропуск, а не попытку удалить.
//   * «Блокировки: определение через Restart Manager, опция “закрыть эти
//     приложения” (RmShutdown) с подтверждением; повтор с backoff 3×» — фаза A
//     опрашивает engine::locks, фаза B (в потоке, породившем прогон, то есть
//     там, где можно спросить человека) закрывает подтверждённые процессы и
//     переспрашивает блокировки, фаза C исполняет. Занятый путь, который так и
//     остался занятым, становится Skip (locked) — ровно то действие, которое
//     описано в FR-5 и в плане как PlanAction::SkipLocked.
//   * «Ошибки не фатальны: собираются в отчёт, остальные операции
//     продолжаются» — у каждого пункта чек-листа свой исход (ItemReport), отказ
//     пишется в ExecutionReport::errors и в журнал с путём и кодом (§12), а
//     прогон идёт дальше. Исключение внутри одной операции тоже не фатально:
//     оно превращается в ItemOutcome::Failed с текстом.
//
// Чего модуль НЕ делает намеренно:
//
//   1. Не строит план и не показывает dry-run: это core::plan (FR-5) и
//      core::DryRunGate. Исполнитель получает готовый core::CleanupPlan и
//      чек-лист, а при options.dryRun честно отказывается (ExecutionRefusal:
//      DryRun) — удалять без показанного плана запрещено. Чек-лист при этом не
//      самостоятельное решение: он сверяется с планом (индекс кандидата и
//      действие), и расхождение — ExecutionRefusal::BadChecklist, а не выбор
//      «правильного» из двух списков.
//   2. Не решает, что класть в корзину, а что удалять напрямую: это
//      core::planTrashPlacement (FR-7), и действие приходит из плана. Если
//      корзина недоступна, Trash-операция завершается отказом с внятной
//      причиной, а не тихим превращением в Delete.
//   3. Не ведёт журнал транзакций и не пишет манифест сам по себе: манифест
//      нужен, чтобы UndoService мог восстановить, поэтому writeManifest
//      вызывается в конце прогона, а решение «можно ли отменять» принимает
//      core::TrashLedger — модуль лишь отдаёт собранную транзакцию в отчёте.
//   4. Не трогает UI и не хранит состояние между прогонами: у каждого прогона
//      своя транзакция корзины и своя копия отчёта, опубликованная как
//      shared_ptr<const> (SPEC §6.4 — «результаты не мутируются после
//      публикации»).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/model.hpp"
#include "core/plan.hpp"
#include "core/rules.hpp"
#include "locks.hpp"
#include "scan_coordinator.hpp"

namespace mrproper {

// Определение только ради ссылки в приватной части CleanupExecutor: журнал
// транзакций корзины (core::TrashLedger) — переносимое ядро, его достаточно
// знать по имени, а включать core/trash.hpp в публичный заголовок исполнителя
// незачем: вызывающему журнал не нужен, он получает собранную транзакцию в
// отчёте (txId и отчёт по элементам).
namespace core {
class TrashLedger;
}

}  // namespace mrproper

namespace mrproper::engine {

// ---------------------------------------------------------------------------
// Значения по умолчанию (FR-6, §5 «Производительность»)
// ---------------------------------------------------------------------------

// Потолок пула. Очистка — это файловый ввод-вывод (переименование, unlink,
// копирование при кросс-томовом переносе), а не счёт, поэтому 8 потоков —
// потолок из спеки; ниже берётся hardware_concurrency, но не больше потолка.
inline constexpr std::size_t kDefaultMaxExecutorWorkers = 8;

// Сколько ошибок чек-листа держим в отчёте целиком. Дерево в 500 тысяч файлов
// может дать отказ на каждом файле, а §5 требует удерживать рабочую память в
// пределах 150 МБ: подробности храним до предела, дальше — только счётчик
// errorsDropped, а сводка в UI говорит «и ещё N».
inline constexpr std::size_t kMaxReportedExecutorErrors = 256;

// ---------------------------------------------------------------------------
// Чек-лист
// ---------------------------------------------------------------------------

// Что исполнять. Собирается из плана (core::CleanupPlan) и кандидатов
// (core::CleanupCandidate) заранее, до запуска пула, и с этого момента не
// меняется: рабочие потоки только читают.
//
// ruleRoot — корень правила в UTF-8. Это не украшение: platform::vfs_delete
// требует границу (пустой корень означает SkippedOutsideRoot для всех путей),
// а FR-6 требует «проверку, что путь внутри ожидаемого корня правила». Корень
// выводится из locator правила функцией ruleRootOf или задаётся вызывающим
// (см. ChecklistOptions::ruleRootOverride).
struct ChecklistEntry {
    std::size_t candidateIndex{};                 // индекс в кандидатах и в плане
    core::PlanAction action{core::PlanAction::Keep};
    std::uint64_t plannedBytes{};                 // сколько обещает план (аллоцированный размер)
    std::string ruleRoot;                         // UTF-8; пусто — удалять нельзя
    std::string trashPayload;                     // имя элемента внутри транзакции корзины
};

// Действие, под которое чек-лист исполняется. Отдельная функция, а не сравнение
// с двумя значениями в трёх местах: исполняемые действия — это ровно Delete и
// Trash, а Keep и SkipLocked в план не входят.
[[nodiscard]] bool executesAction(core::PlanAction action) noexcept;

struct ChecklistOptions {
    // Набор правил, из которого берётся корень: ruleRoot = ruleRootOf(rule по
    // ruleId кандидата). nullptr — корни задаёт вызывающий.
    const core::RuleSet* rules{nullptr};

    // Корень для всех элементов сразу. Задаёт тот, кто точно знает корень
    // (например, корень локатора, из которого вырос кандидат). Непустое
    // значение перекрывает rules.
    std::string ruleRootOverride;

    // Включать в чек-лист то, что план не собирается трогать (Keep и
    // SkipLocked из плана). По умолчанию включено: экран «Очистка» и отчёт
    // показывают, что осталось на месте и почему (FR-5), и строка, которой
    // нет в чек-листе, в отчёте просто исчезает.
    bool includeUntouched{true};
};

struct Checklist {
    std::vector<ChecklistEntry> entries;

    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }

    // Сколько операций собираются выполнить (Delete или Trash). Ноль —
    // исполнять нечего, и прогон честно возвращает ExecutionRefusal::NothingToDo.
    [[nodiscard]] std::size_t executable() const noexcept;
    // Сколько байт по плану собираются освободить (аллоцированный размер,
    // §4 FR-4) — это знаменатель прогресса и ожидаемое число в отчёте.
    [[nodiscard]] std::uint64_t bytes() const noexcept;

    [[nodiscard]] bool executes(const ChecklistEntry& entry) const noexcept;
    // Найти строку по индексу кандидата. nullptr — кандидата нет в чек-листе.
    [[nodiscard]] const ChecklistEntry* find(std::size_t candidateIndex) const noexcept;
};

// Корень правила: часть locator без glob-символов (*, ?, [), обрезанная по
// последнему разделителю. Для «%LOCALAPPDATA%\Microsoft\Edge\User Data\*\Cache\**»
// это «%LOCALAPPDATA%\Microsoft\Edge\User Data» (в уже раскрытом locator —
// «C:\Users\Ev\AppData\Local\Microsoft\Edge\User Data»).
//
// Почему именно префикс, а не «корень, найденный обходом»: корень из
// locator — это надмножество любого каталога, который правило когда-либо
// развернёт, поэтому проверка «путь внутри корня» на нём не может ложно
// запретить, зато ложно разрешить не может. Обратное (взять корень из кандидата)
// было бы проверкой «путь внутри самого себя».
//
// Пустая строка — корень вывести нельзя (локатор начинается с подстановки или
// glob стоит в первом сегменте). Это не «корень = весь диск»: пустой корень
// означает отказ удалять (SkippedOutsideRoot), то есть безопасный исход.
[[nodiscard]] std::string ruleRootOf(std::string_view resolvedLocator);
[[nodiscard]] std::string ruleRootOf(const core::Rule& rule);

// Имя элемента внутри транзакции корзины по порядковому номеру операции:
// «item-000001». Проходит core::isValidPayloadName, поэтому подходит и для
// каталога транзакции на диске. Отдельная функция, чтобы журнал, отчёт и
// манифест называли один и тот же объект одинаково.
[[nodiscard]] std::string trashPayloadName(std::size_t index);

// Собрать чек-лист по кандидатам и плану. Ничего не удаляет, ничего не
// спрашивает: чистая функция, безопасная звать из UI-потока перед показом
// dry-run.
[[nodiscard]] Checklist buildChecklist(const std::vector<core::CleanupCandidate>& candidates,
                                       const core::CleanupPlan& plan, const ChecklistOptions& options = {});

// ---------------------------------------------------------------------------
// Исход одной операции
// ---------------------------------------------------------------------------

// Итог по строке чек-листа. Значения повторяют статусы platform::vfs_delete,
// потому что исполнитель не изобретает свой язык: «удалено», «уже исчез»,
// «занято», «защищено», «вне корня», «точка монтирования», «изменился»,
// «неверный путь», «отмена», «ошибка». Два добавленных состояния — про то, что
// не дошло до платформы вовсе: строка не начата (отмена в очереди) и строка, на
// которую план не собирался идти (Keep / SkipLocked из плана).
enum class ItemOutcome : std::uint8_t {
    NotStarted = 0,        // операция не выполнена: отмена пришла, пока строка ждала в очереди
    NotSelected,           // план не выбрал этот кандидат (Keep / SkipLocked из FR-5)
    Done,                  // объект удалён или перенесён в корзину
    AlreadyGone,           // объекта не было: цель достигнута, это не ошибка
    SkippedBusy,           // держит процесс: Skip (locked) из FR-5/FR-6
    SkippedLockUnknown,    // Restart Manager не ответил: удалять нельзя, но это и не «занято»
    SkippedProtected,      // защищённый каталог: белый список FR-6
    SkippedOutsideRoot,    // путь вне корня правила либо корень не задан
    SkippedReparse,        // symlink/junction: FR-6 — не раскрываем
    SkippedChanged,        // объект изменился после сканирования (§10)
    SkippedInvalid,        // путь пуст, относительный или в пространстве имён устройств
    Cancelled,             // отмена пришла во время операции
    Failed,                // отказ Win32 или исключение внутри операции
};

[[nodiscard]] const char* toString(ItemOutcome outcome) noexcept;

// Операция достигнута своей цели: объект удалён или перенесён в корзину.
[[nodiscard]] bool isSuccess(ItemOutcome outcome) noexcept;
// Операция пропущена: объект цел, виден в отчёте как пропущенный.
[[nodiscard]] bool isSkipped(ItemOutcome outcome) noexcept;
// Требуется перезагрузка, чтобы освободить путь (критичный процесс, чужая
// сессия). Для очистки это «пропустить и сказать почему» (FR-8).
[[nodiscard]] bool needsReboot(ItemOutcome outcome) noexcept;

// Строка отчёта — ровно то, что показывает UI, что уходит в журнал и что
// разбирает человек, когда «очистка освободила меньше, чем обещала».
struct ItemReport {
    std::size_t candidateIndex{};
    core::PlanAction action{core::PlanAction::Keep};
    ItemOutcome outcome{ItemOutcome::NotStarted};

    std::string path;         // UTF-8, как в модели (§6.3)
    std::string displayName;
    std::string category;
    std::string ruleRoot;     // корень, по которому проверяли принадлежность

    std::uint64_t plannedBytes{};    // сколько обещал план (аллоцированный размер)
    std::uint64_t reclaimedBytes{};   // сколько фактически освободили/перенесли

    std::uint32_t attempts{};         // попытки удаления с повтором (FR-6: 3×)
    std::uint32_t filesDone{};        // для дерева: файлов удалено
    std::uint32_t dirsDone{};         // для дерева: каталогов удалено
    std::uint32_t problems{};         // отказов внутри дерева, даже если корень удалён
    std::uint32_t problemsDropped{};  // отказов не показано (обрезано по пределу)
    std::chrono::milliseconds duration{};

    std::int32_t hr{};      // всегда HRESULT (§12 «все ошибки в логе с путём и
                            // HRESULT»): настоящий HRESULT приходит от платформы
                            // как есть, код Win32 оборачивается
                            // HRESULT_FROM_WIN32; 0 — отказа не было
    std::string detail;     // причина по-человечески; у пропуска — почему пропустили
    std::string code;       // стабильный короткий код статуса для JSON-отчёта

    std::string txId;       // транзакция корзины, если элемент перенесён (FR-7)
    std::string payload;    // имя элемента внутри транзакции

    std::vector<core::ProcessRef> lockedBy;  // кто держал путь (FR-4)
    bool requiresReboot{};

    // Признаки, снятые фазой проверки до операции. Это не содержимое объекта,
    // а то, чем журнал и разбор «почему кандидат уцелел» объясняют решение.
    bool isDirectory{};                    // объект — каталог (значит, дерево, а не файл)
    bool stampKnown{};                     // состояние объекта прочитано
    std::uint64_t stampSize{};             // размер на момент проверки
    std::uint64_t stampWriteTicks{};       // время изменения (100-нс интервалы FILETIME)
    std::uint64_t stampChangeTicks{};      // время изменения метаданных
    int lockAttempts{};                    // попыток опроса Restart Manager (FR-6: 3×)
    std::size_t lockHolders{};             // сколько держателей вернул RM
    std::string lockStatus;                // состояние блокировки (engine::locks)
    std::string lockDetail;                // сводка locks::describe — для журнала и UI

    [[nodiscard]] bool ok() const noexcept { return isSuccess(outcome); }
    [[nodiscard]] bool skipped() const noexcept { return isSkipped(outcome); }
    [[nodiscard]] bool failed() const noexcept { return outcome == ItemOutcome::Failed; }
    // Что-то делали с объектом: без этого «очистка ничего не тронула» нельзя
    // отличить от «очистка не дошла до этой строки».
    [[nodiscard]] bool touched() const noexcept { return outcome != ItemOutcome::NotSelected; }
};

// Отказ для отчёта и журнала. Ошибки не фатальны (FR-6), поэтому отказ — это
// запись, а не исключение прогона.
struct ExecutionError {
    std::size_t candidateIndex{};
    std::string path;
    std::int32_t hr{};
    std::string code;     // стабильный код для отчёта
    std::string message;  // текст для UI и журнала
};

// Итог прогона. Публикуется один раз и после этого не мутируется (§6.4).
struct ExecutionReport {
    std::uint64_t generation{};  // номер прогона, растёт монотонно

    std::vector<ItemReport> items;     // по чек-листу, в том же порядке
    std::vector<ExecutionError> errors;  // обрезан по kMaxReportedExecutorErrors

    std::size_t errorsDropped{};  // отказов не показано подробно

    std::size_t total{};          // строк в чек-листе
    std::size_t done{};           // удалено или перенесено
    std::size_t alreadyGone{};    // объекта уже не было
    std::size_t skipped{};        // пропущено политикой или блокировкой
    std::size_t skippedBusy{};    // из пропущенных: держит процесс (Skip (locked))
    std::size_t notSelected{};    // план не выбрал
    std::size_t failed{};
    std::size_t cancelledItems{}; // строк, сорванных отменой
    std::size_t requiresReboot{}; // строк, освободить которые можно только перезагрузкой

    std::uint64_t plannedBytes{};
    std::uint64_t reclaimedBytes{};

    std::size_t workers{};            // потоков в пуле
    std::string trashTxId;            // транзакция корзины прогона (FR-7), пусто — не было
    std::chrono::milliseconds duration{};
    // Прогон прерван отменой. Отличается от cancelledItems: отмена может прийти
    // после того, как все строки уже отработали, и тогда «прервано» означает
    // «пользователь нажал Отмена слишком поздно», а не «часть работы потеряна».
    bool cancelled{};

    [[nodiscard]] std::size_t count(ItemOutcome outcome) const noexcept;
    // Прогон исполнен целиком: каждая строка либо достигла цели, либо сознательно
    // не трогалась по плану. Единый флаг для e2e-критерия «места освободились,
    // системные каталоги не тронуты» (§8 Этап 3).
    [[nodiscard]] bool complete() const noexcept;
    // Есть ли о чём разбираться: отказ, отмена или занятый путь.
    [[nodiscard]] bool hasProblems() const noexcept;
    [[nodiscard]] std::string toText() const;
};

using ExecutionReportPtr = std::shared_ptr<const ExecutionReport>;

// Почему прогон не начался. Отказ — это не ошибка модуля, а решение о том, что
// исполнять нельзя или нечего; тексты объясняют, что именно не так.
enum class ExecutionRefusal : std::uint8_t {
    Completed = 0,    // прогон состоялся (даже если часть строк пропущена)
    NothingToDo,      // в чек-листе нет ни Delete, ни Trash
    DryRun,           // options.dryRun: FR-5 — сначала показать план
    AlreadyRunning,   // прогон уже идёт; повторный клик игнорируется
    BadChecklist,     // чек-лист не согласован с кандидатами или планом
    Cancelled,        // отмена пришла до начала работы
};

[[nodiscard]] const char* toString(ExecutionRefusal refusal) noexcept;

// ---------------------------------------------------------------------------
// Параметры прогона
// ---------------------------------------------------------------------------

// «Закрыть эти приложения» (FR-6, RmShutdown с подтверждением). Вызывается из
// потока, породившего прогон, ровно один раз на каждый занятый путь, у которого
// есть кого закрыть; `pids` — процессы-держатели, которые движок готов закрыть.
//
// Возврат false — отказ: ничего не закрывается, кандидат становится
// Skip (locked). Исключение из колбэка считается отказом: упавший диалог —
// не согласие. В UI реализуется окном со списком процессов; в CLI и тестах —
// функцией, которая всегда возвращает заранее заданный ответ.
using ProcessCloseConfirm =
    std::function<bool(std::string_view pathUtf8, const std::vector<std::uint32_t>& pids)>;

struct CleanupExecutorOptions {
    // Потоки пула. 0 — выбрать автоматически: по числу операций и
    // hardware_concurrency, с ограничением maxWorkers.
    std::size_t workers{};
    std::size_t maxWorkers{kDefaultMaxExecutorWorkers};

    // Как часто публиковать прогресс (§6.4 — UI читает счётчики раз в 100 мс).
    std::chrono::milliseconds progressInterval{kProgressInterval};

    // Как строить чек-лист, если вызывающий передал run(candidates, plan) без
    // явного чек-листа.
    ChecklistOptions checklist{};

    // FR-5: «Dry-run обязателен и запускается по умолчанию перед первым
    // удалением в сессии». При true исполнитель не делает ничего и возвращает
    // ExecutionRefusal::DryRun — обойти показ плана через него нельзя.
    bool dryRun{false};

    // Снимать ли состояние объекта до операции (§10: «проверка Version/LastWrite
    // перед удалением»). Файл с изменившимся состоянием получает
    // SkippedChanged; содержимое каталога проверяется по факту обхода внутри
    // platform::vfs. Выключать только ради скорости в тестах.
    bool checkStamps{true};

    // Проверять блокировки через Restart Manager перед каждой операцией
    // (FR-6). Выключать только в тестах и в прогонах, где блокировки уже
    // проверены: без проверки путь удаляется вслепую.
    bool checkLocks{true};
    locks::Options lockOptions{};  // внутри — повтор с backoff 3× (FR-6)

    // Опция «закрыть эти приложения». Без confirmClose включённый флаг
    // бесполезен: закрывать без подтверждения FR-6 запрещает.
    bool allowCloseProcesses{false};
    ProcessCloseConfirm confirmClose{};
    // RmForceShutdown. По умолчанию false: приложение получает WM_CLOSE и
    // может спросить пользователя о несохранённом (§1.1 — данные важнее удобства).
    bool forceClose{false};
    std::chrono::milliseconds exitWait{1500};

    // Удаление: «повтор с backoff 3×» (FR-6) задаётся платформенному модулю.
    std::uint32_t deleteAttempts{3};
    std::chrono::milliseconds deleteBackoff{50};
    std::chrono::milliseconds maxDeleteBackoff{2000};

    // Корзина (FR-7). Пустой корень → platform::defaultTrashRoot(); если и его
    // нет, Trash-операция завершается отказом с внятной причиной, а не
    // тихим прямым удалением.
    std::string trashRoot{};
    std::string appVersion{};  // пишется в манифест транзакции

    // Подробности отказов: до этого предела ошибки пишутся в отчёт целиком,
    // дальше растёт только errorsDropped (§5 — память в пределах 150 МБ).
    std::size_t maxReportedErrors{kMaxReportedExecutorErrors};

    // Колбэк прогресса. Зовётся из фонового потока исполнителя, поэтому UI
    // обязан маршалить его в свой поток сам (PostMessage) — либо передать
    // dispatcher ниже. Исключение из колбэка ловится и пишется в журнал:
    // интерфейс не имеет права ронять очистку.
    std::function<void(const ProgressSnapshot&)> onProgress{};
    // Маршалинг колбэка: «выполнить в UI-потоке». Не задан — колбэк зовётся
    // напрямую (удобно CLI и тестам).
    std::function<void(std::function<void()>&&)> dispatcher{};

    // Чек-лист по мере выполнения: для живого списка операций в UI. Зовётся из
    // рабочего потока, поэтому маршалинг — забота вызывающего (то же, что у
    // onProgress). Выключен по умолчанию: в UI полосы прогресса хватает, а
    // лишние аллокации на каждую строку плана не нужны.
    std::function<void(const ItemReport&)> onItem{};
    bool emitItemCallback{false};

    bool logProgress{true};
    bool logFailures{true};
};

// ---------------------------------------------------------------------------
// Исполнитель
// ---------------------------------------------------------------------------

// Единственный владелец пула, отмены и отчёта.
//
// Не копируется и не перемещается: внутри живут std::jthread, std::stop_source
// и указатель на прогон. Экземпляр держит тот, кто переживает окно (app
// shell) или процесс (CLI).
//
// Потокобезопасность. start(), run(), requestStop(), progress(), result(),
// running() можно звать из UI- и рабочих потоков одновременно. Сам прогон всегда
// идёт в фоне: UI-поток не блокируется (SPEC §6.4).
//
// Порядок фаз прогона (каждая фаза завершается до следующей):
//
//   A. Проверка — параллельно, по одному потоку на строку: нормализация пути и
//      принадлежность корню правила, белый список защищённых каталогов, признак
//      reparse, снимок состояния (§10) и, если включено, Restart Manager.
//   B. Закрытие приложений — в потоке прогона, последовательно: спросить
//      пользователя и закрыть подтверждённые процессы, затем переспросить
//      блокировки у тех строк, чьи держатели должны были уйти. Без этого шага
//      фаза A породила бы диалоги из восьми рабочих потоков сразу.
//   C. Исполнение — параллельно: Delete (vfs::deleteTree / deleteEntry) и
//      Trash (перенос в каталог транзакции) по одной операции на поток.
//   D. Финализация — в потоке прогона: манифест транзакции корзины, подсчёт
//      итогов, публикация отчёта, запись в журнал.
class CleanupExecutor {
public:
    explicit CleanupExecutor(CleanupExecutorOptions options = {});
    // Запрашивает остановку и присоединяется к фоновому потоку прогона, если он
    // ещё идёт.
    ~CleanupExecutor();

    CleanupExecutor(const CleanupExecutor&) = delete;
    CleanupExecutor& operator=(const CleanupExecutor&) = delete;
    CleanupExecutor(CleanupExecutor&&) = delete;
    CleanupExecutor& operator=(CleanupExecutor&&) = delete;

    // Запустить прогон в фоне и вернуться сразу. false — AlreadyRunning
    // (повторный клик «Очистить» игнорируется, а не затирает текущий прогон).
    ExecutionRefusal start(const std::vector<core::CleanupCandidate>& candidates, const core::CleanupPlan& plan,
                            const Checklist& checklist);

    // То же с чек-листом, собранным внутри по options.checklist.
    ExecutionRefusal start(const std::vector<core::CleanupCandidate>& candidates, const core::CleanupPlan& plan);

    // Блокирующий прогон: start() + ожидание + отчёт. Для CLI и тестов; из
    // UI-потока не вызывается. Отчёт доступен и через result().
    ExecutionRefusal run(const std::vector<core::CleanupCandidate>& candidates, const core::CleanupPlan& plan,
                         const Checklist& checklist);
    ExecutionRefusal run(const std::vector<core::CleanupCandidate>& candidates, const core::CleanupPlan& plan);

    // Дождаться конца текущего прогона (waitFor возвращает false по таймауту).
    bool wait();
    bool waitFor(std::chrono::milliseconds timeout);

    // Отмена из UI-потока по кнопке «Отмена». Кооперативная: уже начатые
    // операции доходят до конца (синхронный вызов Win32 не прервать), не
    // начатые помечаются NotStarted/Cancelled и остаются нетронутыми.
    // Идемпотентно, безопасно вне прогона.
    void requestStop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    // Снимок прогресса для UI: обработано операций, освобождено байт, потоки.
    [[nodiscard]] ProgressSnapshot progress() const noexcept;
    [[nodiscard]] ExecutionReportPtr result() const noexcept;  // nullptr, пока не опубликован
    [[nodiscard]] std::uint64_t generation() const noexcept;    // номер последнего прогона
    // Текен отмены текущего прогона — для кода, который сам ходит в файловую
    // систему мимо исполнителя (тот же приём, что у ScanCoordinator).
    [[nodiscard]] std::stop_token token() const noexcept;
    [[nodiscard]] std::string toText() const;  // состояние для журнала и баг-репорта

private:
    // Состояние одного прогона, которым владеет поток прогона: журнал корзины,
    // каталог транзакции, параметры файловых операций и мьютексы отчёта и
    // журнала. Объявлен здесь только именем, определён в .cpp — так
    // платформенные типы (TrashOptions, FileStamp) не протаскиваются в
    // публичный заголовок движка, который обязан оставаться переносимым
    // (SPEC §6.1, ADR-004).
    struct RunContext;

    // Всё исполнение. Вызывается из фонового потока (start) или из вызывающего
    // (run). Ничего не бросает наружу: исключение превращается в отчёт с
    // пометкой об аварии, потому что из потока прогона наружу exception() — это
    // std::terminate.
    ExecutionRefusal execute(const std::vector<core::CleanupCandidate>& candidates, const core::CleanupPlan& plan,
                              const Checklist& checklist);
    // Фазы A-D. Каждая возвращает false, когда прогон прерван. Фазы A и B не
    // принимают RunContext: проверки и решение «закрыть ли приложения» не имеют
    // дела с корзиной, и лишний параметр означал бы, что они её трогают.
    bool checkPhase(const std::vector<core::CleanupCandidate>& candidates, std::vector<ItemReport>& items);
    bool closePhase(std::vector<ItemReport>& items);
    bool executePhase(std::vector<ItemReport>& items, RunContext& run);
    void finalize(std::vector<ItemReport>& items, ExecutionReport& report, RunContext& run);

    // Шаг фазы A по одному элементу: нормализация, корень, белый список,
    // reparse, снимок состояния, Restart Manager. Заполняет строку отчёта.
    // Данные строки чек-листа (корень, payload, ожидаемые байты) уже скопированы
    // в отчёт makeItem(), поэтому кандидат нужен только ради его полей.
    void checkItem(const core::CleanupCandidate& candidate, ItemReport& item);
    // Переспрос блокировки после закрытия приложений. true — путь освободился,
    // операцию можно выполнять.
    bool recheckLock(std::string_view path, ItemReport& item);
    // Закрыть держателей кандидата по подтверждению пользователя.
    bool closeHolders(ItemReport& item, std::string& note);
    // Прямое удаление: дерево для каталога, один объект для файла.
    void deleteItem(ItemReport& item);
    // Перенос в корзину приложения (FR-7). Элемент транзакции кладётся в
    // журнал прогона — так манифест в финализации увидит всё, что перенесено.
    void trashItem(ItemReport& item, RunContext& run);
    // Завести транзакцию корзины, если в чек-листе есть Trash-строки.
    void prepareTrash(RunContext& run, const std::vector<ItemReport>& items);
    // Манифест транзакции (FR-7) либо уборка пустого каталога.
    void finishTrash(RunContext& run, ExecutionReport& report);
    // Подсчёт итогов отчёта по строкам чек-листа.
    void countTotals(const std::vector<ItemReport>& items, ExecutionReport& report) const;

    // Строка чек-листа получила окончательный исход: двигаем прогресс, зовём
    // колбэк UI и пишем в журнал. Ровно один раз на строку.
    void noteItem(const ItemReport& item);
    void emitProgress(bool force);
    void addError(ExecutionReport& report, const ItemReport& item, RunContext& run);
    void logRefusal(ExecutionRefusal refusal, const Checklist& checklist) const;
    void logStart(const Checklist& checklist, std::size_t workers) const;
    [[nodiscard]] std::size_t resolveWorkers(std::size_t operations) const noexcept;
    [[nodiscard]] ItemReport makeItem(const core::CleanupCandidate& candidate, const ChecklistEntry& entry) const;
    // Общий вход для run() и start(): отказ до прогона и захват состояния.
    [[nodiscard]] ExecutionRefusal beginRun(const Checklist& checklist);
    // Тело фонового прогона (start). Ничего не бросает.
    void runBackground();

    CleanupExecutorOptions options_;

    // Снимок входа фонового прогона: start() копирует их сюда, потому что
    // execute() работает в отдельном потоке, а вызывающий после start() вправе
    // освободить свои векторы. Копия осознанная — на плане в тысячи строк это
    // десятки килобайт, а взамен невозможно удалить кандидата из-под прогона.
    std::vector<core::CleanupCandidate> runCandidates_;
    core::CleanupPlan runPlan_;
    Checklist runChecklist_;

    mutable std::mutex mutex_;
    std::condition_variable doneCv_;
    std::stop_source stop_;      // источник отмены текущего прогона
    std::stop_token runToken_;   // его копия: раздаётся пулу и платформенным вызовам
    std::jthread runner_;
    ProgressCounter progress_;
    ExecutionReportPtr result_;
    std::uint64_t generation_{0};
    bool running_{false};
    // Транзакция прогона, к которой сводятся все Trash-операции (FR-7).
    std::string txId_;
    // Отметка последней публикации прогресса: throttle на progressInterval.
    // Атомик, потому что отметку обновляют рабочие потоки фазы C.
    std::atomic<std::int64_t> lastProgressTicks_{0};
};

}  // namespace mrproper::engine
