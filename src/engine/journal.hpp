// Журнал транзакций очистки и снимок плана перед удалением (SPEC §4 FR-5,
// FR-6, FR-8, §7.2, §6.1, §6.2, §12, §10).
//
// ---------------------------------------------------------------------------
// Что модуль делает и почему отдельно от журнала `core::log`
// ---------------------------------------------------------------------------
//
// Спека требует двух разных вещей, и их часто путают:
//
//   * «Перед исполнением — снимок состояния: список операций, PID, версия,
//     размер; запись в журнал» (FR-5). Это ДОКУМЕНТ: что именно приложение
//     обещало удалить, с каким PID и версией, и по какому плану. Он должен
//     пережить падение процесса, потому что по нему отвечают перед
//     пользователем и по нему же судят, что вообще было обещано;
//   * «список выполненных операций, ошибки, время» в отчёте (FR-8) и
//     «Отмена (Ctrl+Z) доступна, пока транзакция не схлопнулась» (§7.2).
//     Это ИСХОД: что реально произошло с каждым элементом, какой HRESULT и
//     куда делись байты.
//
// `core::log` — третья вещь: диагностическая лента (кольцо + файл, уровни,
// троттлинг). Её можно молча обрезать по уровню, она не обязана быть
// полной, и по ней нельзя доказать, что удаляли. Если писать «что удалили»
// в `core::log`, то (1) любая ошибка записи лога сделает запись о деле не
// доказуемой, а (2) отчёт и журнал разойдутся — они читают разные источники.
// Поэтому здесь свой формат и своя дисциплина: одна строка на событие, одна
// запись на операцию, ничего не выбрасывается молча.
//
// ---------------------------------------------------------------------------
// Формат
// ---------------------------------------------------------------------------
//
// Журнал — JSON Lines (одна запись на строку, ровно как у `core::log`, но с
// доменными полями операции). Причина та же: файл дописывается целиком,
// строка не «разъезжается» при падении, а разбор не требует библиотеки.
// Снимок плана — отдельный файл `plan-<txId>.json`, потому что список
// операций это сотни–тысячи строк, и держать его в общей ленте нельзя: одна
// транзакция раздула бы файл, а экран «Отчёт» (§7.1) всё равно читает план
// отдельно от журнала.
//
// Снимок пишется АТОМАРНО (во временный файл + rename) и по умолчанию
// ПЕРЕЧИТЫВАЕТСЯ обратно перед тем, как движку разрешают удалять. Это и есть
// смысл FR-5: «запись в журнал» — не «вызвали write», а «файл есть и верен».
// Если снимок не записался или не прошёл проверку, `beginTransaction`
// возвращает false, и удаление обязано быть запрещено: согласие пользователя
// было дано на конкретный список операций, и без записи этого списка
// согласие ничем не подтверждено.
//
// ---------------------------------------------------------------------------
// Порядок событий транзакции
// ---------------------------------------------------------------------------
//
//   tx.begin        — снимок уже записан и проверен
//   op.result       — по одной записи на операцию (FR-6: «ошибки не
//                     фатальны: собираются в отчёт, остальные операции
//                     продолжаются»)
//   tx.end          — committed / cancelled / failed
//
// Открытая транзакция в файле — это не «идёт сейчас», а признак обрыва:
// либо процесс упал, либо запись `tx.end` не дошла. Такие транзакции
// `pendingTransactions` отдаёт отдельным списком, и по ним UI может предложить
// восстановление из корзины (§7.2), а отчёт — показать неполную операцию
// (SPEC §12: «отмена во время операции оставляет систему в согласованном
// состоянии»).
//
// ---------------------------------------------------------------------------
// Границы модуля
// ---------------------------------------------------------------------------
//
//   * план, снимок и dry-run — `core::plan` (FR-5). Здесь их не переизобретают:
//     журнал получает готовый `core::PlanSnapshot` и печатает его тем же
//     `core::snapshotToJson`, каким его печатает CLI;
//   * что удалять, в каком порядке, с retry и Restart Manager —
//     `engine::executor`. Журнал не решает ничего из этого и не может
//     запретить операцию сам: он либо дал снимок, либо нет;
//   * корзина, манифест, восстановление — `core::trash`/`core::undo` и
//     `engine::undo`. Журнал хранит ссылку `undoRef` (id транзакции корзины)
//     и больше ничего о корзине не знает;
//   * права, длинные пути, `\\?\` — `platform::vfs`. Здесь файловый ввод-вывод
//     сделан на `std::filesystem` (переносимо, ADR-004): пути журнала короткие,
//     в узкой кодировке Windows и в каталоге с не-ASCII именем работать не
//     пришлось бы — см. `nativePath` в .cpp.
//
// Модуль не включает windows.h и не знает про WinAPI: HRESULT приходит
// числом от исполнителя (§12 — «все ошибки в логе с путём и HRESULT»).
//
// ---------------------------------------------------------------------------
// Правила, которые модуль соблюдает по построению
// ---------------------------------------------------------------------------
//
// 1. Ничего не бросает наружу, кроме std::bad_alloc. Любой отказ — это
//    `false`/`ok == false` плюс `lastError()`/`problem`, потому что отказ
//    журнала не должен ронять удаление: операции уже идут, а потерянная
//    запись журнала — это потеря доказательства, а не данных пользователя.
//    Исключение из правила одно — `beginTransaction`: там снимок обязателен,
//    и «не смог записать» обязано быть отказом, а не предупреждением.
//
// 2. Порядок внутри транзакции не переставляется: снимок → tx.begin →
//    op.result* → tx.end. Нарушение этого порядка делает журнал нечитаемым
//    ровно в тот момент, когда он нужен, — после падения.
//
// 3. Запись обрезанным хвостом не считается записью: `scanFile` отличает
//    «последняя строка оборвана» (`tornTail`) от «битая строка в середине»
//    (`damagedLines`) и не выдаёт первую за повторную попытку. Молчаливый
//    разбор обрезанного хвоста выглядел бы как полный журнал.
//
// 4. Снимки и архивы журнала ограничены по времени и по числу файлов
//    (здесь — 7 дней, как корзина в FR-7; отчёты в FR-8 держат 20 файлов):
//    «журнал» без границ — это вторая корзина, которую пользователь не
//    чистит. Файлы с незнакомым именем не удаляются никогда.
//
// 5. Модуль не решает, «хороша» ли транзакция, и не отказывает в записи из-за
//    того, что операция удалила системный каталог: политику безопасности
//    держит движок, а журнал хранит факты.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/plan.hpp"

namespace mrproper::engine::journal {

// ---------------------------------------------------------------------------
// Константы формата и пределы
// ---------------------------------------------------------------------------

// Схема журнала. Повышается только при несовместимой смене полей; на меньшей
// схеме запись не читается вовсе, а не читается «по-старому» (то же правило,
// что у манифеста корзины, §4 FR-7).
inline constexpr int kJournalSchema = 1;

// Имена файлов в каталоге журнала.
inline constexpr const char* kJournalFileName = "journal.jsonl";
inline constexpr const char* kSnapshotDirName = "snapshots";
inline constexpr std::string_view kSnapshotFilePrefix = "plan-";
inline constexpr std::string_view kSnapshotFileSuffix = ".json";
inline constexpr std::string_view kArchivedJournalPrefix = "journal-";
inline constexpr std::string_view kArchivedJournalSuffix = ".jsonl";

// Пределы по умолчанию. Журнал не должен расти без границы: 4 МБ на
// ротацию, 5 архивов (≈ 20 МБ верхней границы на все транзакции) и 7 дней
// на снимки — столько же, сколько живут транзакции в корзине (FR-7).
inline constexpr std::uint64_t kJournalDefaultMaxBytes = 4ull * 1024ull * 1024ull;
inline constexpr int kJournalDefaultKeepFiles = 5;
inline constexpr std::int64_t kSnapshotDefaultRetentionSeconds = 7 * 24 * 60 * 60;

// Снимок больше этого размера — признак того, что в план попал не кандидат, а
// каталог целиком. Такой снимок не пишется: он не поместился бы в разумный
// отчёт, и его появление означает ошибку плана, а не объём.
inline constexpr std::uint64_t kSnapshotMaxBytes = 2ull * 1024ull * 1024ull;

// Сколько байт журнала читает scanFile. Файл больше предела — это уже не
// журнал, а мусор в каталоге; читаем начало, остальное честно не трогаем.
inline constexpr std::uint64_t kJournalScanMaxBytes = 8ull * 1024ull * 1024ull;

// ---------------------------------------------------------------------------
// Время и идентификатор транзакции
// ---------------------------------------------------------------------------

// Часы модуля. Подменяются в JournalOptions::nowMillis: детерминированное
// время — условие golden-тестов (SPEC §11.4), а «время из системных часов»
// внутри логики записи недопустимо.
std::int64_t nowUnixMillis() noexcept;
std::int64_t nowUnixSeconds() noexcept;

// «2026-09-28T05:34:12.345Z» — человек читает журнал глазами, когда разбирает
// инцидент (SPEC §7.1, экран «Отчёт»).
std::string formatTimestamp(std::int64_t unixMillis);

// «20260928T053412» — компактная метка для имён файлов. Сравнение таких меток
// лексикографически равно сравнению по времени, поэтому архивы и снимки
// сортируются именем, а не датой модификации (которая ещё и зависит от копирования).
std::string formatFileStamp(std::int64_t unixMillis);

// Идентификатор транзакции: «20260928T053412-1234-000007» (время, PID, счётчик
// модуля). Читаемый в отчёте, уникальный в пределах процесса, безопасный для
// имени файла: только цифры и дефисы, никаких разделителей пути.
std::string makeTxId(std::int64_t unixMillis, std::uint32_t pid, std::uint64_t counter);

// Грамматика txId проверяется перед тем, как из него строится имя файла: путь
// из журнала приходит с диска, и «plan-<строка>.json» не должен ни выйти за
// пределы каталога, ни стереть чужой файл.
bool isValidTxId(std::string_view txId) noexcept;

// Метка времени из txId (первые 15 символов) для отсечения старых снимков.
bool txIdStamp(std::string_view txId, std::string& stamp) noexcept;

// ---------------------------------------------------------------------------
// Состояния и статусы — строки стабильны: по ним пишутся отчёт и поиск по журналу
// ---------------------------------------------------------------------------

enum class TxState : std::uint8_t {
    Open,       // начата, tx.end ещё не записан (в том числе — обрыв процесса)
    Committed,  // все операции получили результат, транзакция закрыта
    Cancelled,  // пользователь отменил (FR-6, отмена кооперативная)
    Failed,     // транзакция закрыта с ошибкой, часть операций могла не выполниться
};

enum class OpStatus : std::uint8_t {
    Deleted,   // удалено напрямую (ADR-006: большие кэши мимо корзины)
    Trashed,   // перенесено в корзину приложения (FR-7)
    Skipped,   // действие Keep: путь оставлен по плану
    Blocked,   // Skip (locked): файл держат приложения (FR-5, FR-6)
    Cancelled, // отмена пришла до/во время операции
    Failed,    // HRESULT в записи, остальные операции продолжаются (FR-6)
};

enum class JournalEvent : std::uint8_t {
    TxBegin,    // «tx.begin»    — снимок записан, транзакция открыта
    Operation,  // «op.result»   — результат одной операции
    TxEnd,      // «tx.end»      — транзакция закрыта
    Rotate,     // «rotate»      — журнал ушёл в архив
    Note,       // «note»        — свободная запись модуля
};

const char* toString(TxState state) noexcept;
const char* toString(OpStatus status) noexcept;
const char* toString(JournalEvent event) noexcept;
bool txStateFromString(std::string_view text, TxState& out) noexcept;
bool opStatusFromString(std::string_view text, OpStatus& out) noexcept;
bool journalEventFromString(std::string_view text, JournalEvent& out) noexcept;

// Связь с планом: операция удаляет (и освобождает место), или нет. Из этого
// выводится «сходится ли транзакция» без обращения к плану.
bool isRemovableStatus(OpStatus status) noexcept;

// Статус операции по действию плана и факту выполнения.
OpStatus statusForAction(core::PlanAction action, bool succeeded) noexcept;

// ---------------------------------------------------------------------------
// Запись журнала (одна строка файла)
// ---------------------------------------------------------------------------

// Поля повторяют JSON один в один: структура нужна вызывающему, чтобы собрать
// отчёт, не разбирая JSON. Значения, которые могут отсутствовать, помечены
// «нулевым» состоянием, а не optional: лишний optional в горячем пути записи
// (одна запись на операцию, их бывают тысячи) стоит дороже, чем проверка.
struct JournalRecord {
    // Сквозной номер записи в файле, начиная с 1. Продолжается после
    // перезапуска процесса: записи одного файла нумеруются подряд, а не по
    // сеансам, иначе «запись N» в отчёте указывала бы на две разные строки.
    std::uint64_t sequence{};
    std::int64_t atUnixMillis{};
    JournalEvent event{JournalEvent::Note};
    std::string txId;
    TxState state{TxState::Open};
    std::size_t operationIndex{};  // позиция операции в снимке
    bool hasOperation{false};      // false для событий без операции
    // Операции не было в снимке плана. По этому флагу `summarize` восстанавливает
    // `TxTotals::unplanned` при чтении журнала обратно. По одному индексу он не
    // восстанавливается: для такой операции пишется candidateIndex, а он может
    // случайно попасть в диапазон плана, поэтому флаг кладётся в запись явно.
    bool unplanned{false};
    OpStatus status{OpStatus::Skipped};
    core::PlanAction action{core::PlanAction::Keep};
    std::uint64_t bytes{};         // фактически освобождено за операцию, 0 если не удаляли
    std::int64_t durationMillis{}; // время операции
    std::int64_t hresult{};        // 0 — код Win32 не применим (§12: HRESULT в логе)
    std::string path;              // utf-8 путь операции
    std::string detail;            // текст ошибки / пояснение
    std::string undoRef;           // id транзакции корзины, "" — прямой делет
    // Версия, PID и подтверждение dry-run — только у tx.begin/tx.end: повторять
    // их в каждой из тысяч записей операций незачем, а для сводки они и нужны
    // один раз. Там же — размеры плана, иначе сводка транзакции не знала бы,
    // сколько операций обещано и сколько байт под ними.
    std::string appVersion;
    std::uint32_t pid{};
    bool dryRunAcknowledged{false};
    std::uint64_t plannedOperations{};
    std::uint64_t plannedBytes{};
    std::uint64_t planSignature{};
};

json::Value toJson(const JournalRecord& record);

// Одна строка журнала с завершающим '\n' — ровно то, что дописывается в файл.
std::string formatRecord(const JournalRecord& record);

// Разбор строки. Никогда не бросает: `problem` получает причину, а false —
// ответ «это не наша запись».
bool recordFromJson(const json::Value& value, JournalRecord& out, std::string& problem);

// ---------------------------------------------------------------------------
// Снимок плана (FR-5)
// ---------------------------------------------------------------------------

// Снимок: ровно то, что собрал `core::makeSnapshot`, плюс кто и когда его снял.
struct SnapshotRequest {
    std::string txId;
    core::PlanSnapshot snapshot;
    bool dryRunAcknowledged{false};  // FR-5: dry-run показан и подтверждён
    std::uint32_t pid{};              // для снимка, снятого до поднятия привилегий
};

// Что получилось на диске после записи снимка.
struct SnapshotInfo {
    std::string txId;
    std::string fileName;
    std::string path;                 // полный путь, utf-8
    std::uint64_t fileBytes{};
    std::uint32_t operationCount{};
    std::uint64_t totalBytes{};
    std::uint64_t planSignature{};
    bool verified{false};             // файл перечитан и сошёлся по счётчикам
};

// Шапка снимка без тела операций: экран «Отчёт» и восстановление после падения
// спрашивают «что обещали», а не перечитывают тысячу строк.
struct SnapshotHeader {
    int schema{};
    std::string txId;
    std::string appVersion;
    std::uint32_t pid{};
    std::int64_t createdAtUnix{};
    std::int64_t capturedAtUnixMillis{};
    std::uint64_t planSignature{};
    std::size_t operationCount{};
    std::uint64_t totalBytes{};
    bool dryRunAcknowledged{false};
};

std::string snapshotFileName(std::string_view txId);

// Собрать JSON снимка. Отдельная функция, а не деталь внутри транзакции:
// содержимое снимка нужно и для теста, и для отчёта без запуска транзакции.
// capturedAtUnixMillis = 0 — взять системные часы; тест с подменёнными часами
// передаёт своё значение, иначе детерминированный JSON не получится.
std::string serializeSnapshot(const SnapshotRequest& request, std::int64_t capturedAtUnixMillis = 0);

// Шапка из готового снимка без чтения файла.
bool snapshotHeaderFromJson(const json::Value& value, SnapshotHeader& out, std::string& problem);

// ---------------------------------------------------------------------------
// Итоги транзакции
// ---------------------------------------------------------------------------

struct TxTotals {
    std::size_t planned{};
    std::size_t deleted{};
    std::size_t trashed{};
    std::size_t skipped{};
    std::size_t blocked{};
    std::size_t cancelled{};
    std::size_t failed{};
    // Результаты по операциям, которых не было в снимке. Ненулевое значение —
    // не «мелочь», а признак расхождения плана и исполнения: именно его ищут
    // при разборе инцидента.
    std::size_t unplanned{};
    std::uint64_t bytesFreed{};   // освобождено прямой делет
    std::uint64_t bytesInTrash{};  // ушло в корзину: место освободится при её очистке
    std::int64_t durationMillis{};

    // Сколько операций получили результат. Сходится ли транзакция — это
    // `accounted() == planned && unplanned == 0`, а не «ошибок нет»: молча
    // пропущенная операция тоже делает транзакцию несогласованной.
    std::size_t accounted() const noexcept;
    bool balanced() const noexcept;
};

// Сводка одной транзакции — то, что показывает экран «Отчёт» (§7.1) и то, на
// чём строится предложение восстановления после обрыва.
struct TransactionSummary {
    std::string txId;
    TxState state{TxState::Open};
    bool dryRunAcknowledged{false};
    std::string appVersion;
    std::uint32_t pid{};
    std::int64_t startedAtUnixMillis{};
    std::int64_t endedAtUnixMillis{};  // 0 — не закрыта
    std::uint64_t planSignature{};
    std::string snapshotFile;
    std::string detail;
    TxTotals totals;

    bool open() const noexcept { return state == TxState::Open; }
    std::int64_t elapsedMillis() const noexcept;
};

std::string toString(const TransactionSummary& summary);
std::string toText(const std::vector<TransactionSummary>& summaries);

// ---------------------------------------------------------------------------
// Чтение журнала
// ---------------------------------------------------------------------------

// Результат разбора файла. `tornTail` и `damagedLines` — разные вещи: первый
// это нормальная последняя строка, оборванная падением процесса (её теряем и
// говорим об этом), второй — битая запись в середине (файл повреждён).
struct JournalScan {
    std::string file;  // основной файл: текущий журнал, а при чтении архива — он сам
    std::vector<std::string> files;  // все прочитанные файлы, от свежих к старым
    std::uint64_t fileBytes{};
    std::vector<JournalRecord> records;
    std::size_t tornTail{};
    std::size_t damagedLines{};
    std::vector<std::string> problems;  // пусто — файл цел
    bool truncated{};                   // файл больше kJournalScanMaxBytes
};

std::vector<TransactionSummary> summarize(const JournalScan& scan);

// Транзакции без `tx.end`. Их наличие означает обрыв (процесс упал или запись
// не дошла), а не «сейчас идёт»: вызывающий сверяет `pid` и время. Это и есть
// вход восстановления после падения (§7.2, §12).
std::vector<TransactionSummary> pendingTransactions(const JournalScan& scan);

// ---------------------------------------------------------------------------
// Настройки и сам журнал
// ---------------------------------------------------------------------------

struct JournalOptions {
    // Каталог журнала, utf-8. Пустой — журнал выключен (тесты, CLI без
    // профиля, отключённая диагностика): модуль остаётся рабочим, все вызовы
    // возвращают «ничего не записано», но не падают.
    std::string rootDirectory;
    std::string appVersion;
    std::uint32_t pid{};
    bool enabled{true};
    bool createDirectories{true};
    // Перечитать снимок после записи и сверить счётчики. Один лишний проход по
    // файлу на транзакцию — цена доказательства, что согласие пользователя
    // зафиксировано (FR-5).
    bool verifySnapshot{true};
    // Сбрасывать буфер на каждой записи. Иначе журнал теряет последние записи
    // при падении процесса — а именно они и объясняют, почему он упал.
    bool flushEveryRecord{true};
    std::uint64_t maxFileBytes{kJournalDefaultMaxBytes};
    int keepFiles{kJournalDefaultKeepFiles};
    std::int64_t snapshotRetentionSeconds{kSnapshotDefaultRetentionSeconds};
    // Подменяемые часы: см. nowUnixMillis.
    std::function<std::int64_t()> nowMillis;
};

struct BeginResult {
    bool ok{false};
    std::string txId;
    SnapshotInfo snapshot;
    std::string problem;  // "" при ok
};

// Журнал транзакций.
//
// Владеет одним открытым `std::ofstream` и одним открытым txId: параллельных
// очисток в приложении не бывает, а две открытые транзакции означали бы две
// нераздельные истории в одном файле. Потокобезопасен — `recordOperation` зовёт
// пул исполнителя (SPEC §6.4), а чтение файла идёт из UI-потока.
class TransactionJournal {
public:
    TransactionJournal();
    explicit TransactionJournal(JournalOptions options);
    ~TransactionJournal();
    TransactionJournal(const TransactionJournal&) = delete;
    TransactionJournal& operator=(const TransactionJournal&) = delete;
    TransactionJournal(TransactionJournal&&) = delete;
    TransactionJournal& operator=(TransactionJournal&&) = delete;

    bool enabled() const noexcept;
    const JournalOptions& options() const noexcept;
    std::string rootDirectory() const;
    std::string currentFile() const;  // пусто, если журнал выключен
    std::string snapshotDirectory() const;

    // Счётчики для отчёта о самом журнале: сколько записей дошло до файла и
    // сколько отказов записи было (SPEC §12 — отказ журнала виден, а не тих).
    std::uint64_t recordsWritten() const;
    std::uint64_t failures() const;
    std::string lastError() const;

    // ---- запись ---------------------------------------------------------

    // Снимок состояния (FR-5). Атомарная запись + при `verifySnapshot`
    // обратное чтение. false — снимка нет и удаление запрещено; причина в
    // `lastError()`, потому что «тихо не записать» здесь нельзя.
    bool writeSnapshot(const SnapshotRequest& request, SnapshotInfo& out);

    // Снимок ПЕРВЫМ, потом `tx.begin`. false означает «не начинать»: снимок не
    // записан либо не сошёлся. Это единственный отказ, который обязан быть
    // отказом, а не предупреждением.
    BeginResult beginTransaction(const core::PlanSnapshot& snapshot, bool dryRunAcknowledged);

    // Результат одной операции (FR-6: ошибки не фатальны). Пишет строку и
    // обновляет итоги. Планируемый размер берётся из снимка, в запись попадает
    // фактически освобождённый.
    bool recordOperation(const core::PlanOperation& operation, OpStatus status, std::uint64_t bytesFreed,
                         std::int64_t durationMillis, std::int64_t hresult, std::string detail = {},
                         std::string undoRef = {});

    // Закрытие транзакции. Повторный вызов или вызов без открытой транзакции —
    // false (состояние не выдумывается: закрыта транзакция закрыта).
    bool commit(std::string detail = {});
    bool cancel(std::string detail = {});
    bool fail(std::string detail = {});

    // Свободная запись (например, «план изменён, снимок перезаписан»).
    void writeNote(std::string event, std::string detail);

    // Удалить снимки старше `snapshotRetentionSeconds`. Файлы с незнакомым
    // именем не трогаются: чужой файл в каталоге журнала удалять нельзя.
    // Возвращает число удалённых.
    std::size_t pruneSnapshots();

    // Состояние открытой транзакции.
    std::string currentTxId() const;
    bool transactionOpen() const noexcept;
    TxTotals totals() const;

    // ---- чтение ---------------------------------------------------------

    // Разбор одного файла. Никогда не бросает: отсутствующий файл — пустой
    // результат с записью в `problems`.
    static JournalScan scanFile(const std::string& path);

    // Текущий файл плюс архивы. `files` перечислены от свежих к старым (так их
    // удобнее показывать в отчёте), а `records` идут строго в хронологическом
    // порядке: архивы от старых к новым, поверх них текущий файл. Порядок
    // записей значим — по нему восстанавливается состояние транзакции, начатой
    // до ротации. `scanCurrent` — то, что вызывает экран «Отчёт» (§7.1) и CLI.
    static JournalScan scanCurrent(const JournalOptions& options);

    // Архивы, свежие первыми. Имена сортируются меткой времени в имени, а не
    // датой модификации: копирование журнала не должно менять его порядок.
    static std::vector<std::string> listArchivedFiles(const JournalOptions& options);

    // Сводки по всем транзакциям из текущего и архивных файлов.
    static std::vector<TransactionSummary> readTransactions(const JournalOptions& options);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mrproper::engine::journal
