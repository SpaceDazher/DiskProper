// mrproper-cli: команда `undo` — отмена последней очистки и восстановление
// по идентификатору транзакции (SPEC §4 FR-7, §4 FR-9, §7.2, §6.2 «cli»).
//
// ---------------------------------------------------------------------------
// Зачем команда отдельная от apply
// ---------------------------------------------------------------------------
//
// apply умеет класть в корзину, но отменить очистку из самой утилиты было
// нечем: в реестре команд были scan, plan, apply, report, rules и disks.
// Собственное восстановление при этом существовало и работало —
// core::undo (переносимое решение, ADR-004), engine::undo_service
// (оркестрация: снимок цели, план, вопросы, перенос) и
// platform::vfs_trash (байты, ACL, mtime). Не хватало только тонкой обвязки:
// показать человеку, что можно отменить, спросить подтверждение и позвать
// сервис. Пользователь после очистки не мог вернуть файлы из программы, а
// e2e-сценарий Test-UndoRestore.ps1 проверял слой core контрактом манифеста,
// обходя отсутствие команды. Этот файл — ровно эта обвязка, и ничего сверх.
//
// ---------------------------------------------------------------------------
// Почему контракт повторяет apply, а не придумывает свой
// ---------------------------------------------------------------------------
//
// Тот же словарь, та же последовательность «снимок ДО вопроса», тот же смысл
// кодов возврата. Человек, отменивший очистку по инструкции для apply, не
// должен угадывать заново:
//
//   * dry-run по умолчанию (`undo --last` показывает снимок и план, не
//     возвращая ни байта; включается `--execute` — тем же словом, что у apply);
//   * подтверждение словом из подсказки, иначе `--yes`;
//   * ключ -Destructive у отмены НЕ НУЖЕН: отмена не разрушает ничего сверх
//     возврата файлов на прежние места, а решить её может и должен человек без
//     особого ключа (в отличие от apply, где ключ отличает «посмотреть» от
//     «удалить»).
//
// ---------------------------------------------------------------------------
// Коды возврата — в духе apply (src/cli/cmd_apply.hpp), свои имена
// ---------------------------------------------------------------------------
//
// Общие коды каркаса объявляет args.hpp; переиспользовать PlanExit нельзя —
// объявление одного и того же имени в одном namespace не собирается. Здесь:
//
//   0 Ok            — список показан или отмена выполнена полностью;
//   2 Usage         — не разобраны аргументы;
//   3 Refused       — подтверждение не получено: не восстановлено ни байта;
//   4 NothingToUndo — отменять нечего: корзина пуста, транзакция не найдена,
//                     уже восстановлена или схлопнулась (SPEC §7.2);
//   5 NoService     — отмена недоступна: корень корзины неизвестен;
//   6 Partial       — вернулось меньше, чем планировалось: часть элементов
//                     пропущена решением пользователя или с ошибкой (FR-6 —
//                     отказ по элементу не фатален);
//   7 Failed        — не вернулось ни одного элемента.
//
// 4 — это НЕ ошибка: повторная отмена уже отменённой транзакции обязана быть
// тихим «нечего восстанавливать» с кодом 0 у --list и с кодом 4 у попытки
// восстановления, потому что CI обязан отличать «отменять нечего» от
// «сломалась отмена».
//
// ---------------------------------------------------------------------------
// Конфликты (FR-7: «существующий файл — не перезаписывать, спросить»)
// ---------------------------------------------------------------------------
//
// Политика одна — core::ConflictPolicy, объявленное ядром: Ask, Skip,
// Overwrite, Rename. Ask — значение по умолчанию и единственное, при котором
// вопрос задаётся человеку. Три способа ответить на конфликты:
//
//   1) интерактивно: команда печатает вопросы плана (core::UndoPlan::questions)
//      и ждёт слова «перезаписать» / «рядом» / «пропустить»;
//   2) ключом `--conflict skip|overwrite|rename` — ответ сразу на все;
//   3) ключом `--yes` вместе с политикой по умолчанию: ответ «не отвечать»
//      превращается в «пропустить», а НЕ в «перезаписать» (правило 3 шапки
//      engine/undo_service.hpp). Молчание не перезаписывает — это требование
//      FR-7, а не перестраховка.
//
// Восстановленный по Rename элемент не трогает исходное место: чужой файл,
// который там появился, остаётся нетронутым, а возвращённый получает суффикс
// .mrproper-restore (core::undo::restoreRenameTarget).
//
// ---------------------------------------------------------------------------
// Формат машинного вывода (--json)
// ---------------------------------------------------------------------------
//
//   { schema:1, kind:"undo", mode:"list"|"plan"|"restore", trashRoot,
//     transactions:[{txId,createdAt,appVersion,state,bytes,items,restorable,
//                   available,expired,note}],
//     plan:{ txId, entries:[{originalPath,targetPath,bytes,action,conflict,
//                            needsUserDecision,note}], questions:[…],
//            bytesPlanned, conflicts, requiresUserDecision, crossVolume,
//            copySeconds, slowEnoughToWarn } | null,
//     restore:{ txId, items:[{targetPath,action,ok,bytes,status,detail}],
//               restored, skipped, failed, notAttempted, bytesRestored,
//               cancelled, manifestWritten, purged, summary } | null,
//     confirmed, refusal }
//
// Человеческий текст и вопросы всегда идут в stderr: stdout в режиме --json
// остаётся машинным (то же требование, что у scan/plan/apply, §6.2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/undo.hpp"

namespace mrproper::cli {

// ---------------------------------------------------------------------------
// Коды возврата
// ---------------------------------------------------------------------------
enum class UndoExit : int {
    Ok = 0,            // список показан или отмена выполнена полностью
    Usage = 2,         // не разобраны аргументы: неизвестный ключ, нет значения
    Refused = 3,       // подтверждение не получено — не восстановлено ни байта
    NothingToUndo = 4, // отменять нечего: пусто, не найдено, уже восстановлено
    NoService = 5,     // корень корзины неизвестен — отмена недоступна
    Partial = 6,       // вернулось меньше, чем планировалось (FR-6: не фатально)
    Failed = 7,        // не вернулось ни одного элемента
};

// Слово, которое надо ввести для подтверждения отмены. Тот же приём, что у
// apply (DELETE / DELETE-RISKY): ответ обязан совпасть дословно, иначе отказ.
inline constexpr const char* kUndoConfirmationToken = "DRY-RESTORE";

// ---------------------------------------------------------------------------
// Разобранные аргументы
// ---------------------------------------------------------------------------

// Что именно просит человек. Три режима, и они не смешиваются: список —
// вопрос «что можно отменить», план — «что вернётся», восстановление —
// «вернуть это».
enum class UndoMode : int {
    List,     // --list: транзакции корзины (дата, элементы, объём) — FR-9
    Plan,     // --tx/--last без --execute: снимок и план, ничего не возвращено
    Restore,  // --tx/--last с --execute: восстановление с подтверждением
};

struct UndoOptions {
    UndoMode mode{UndoMode::List};
    bool list{false};      // --list
    bool last{false};      // --last
    bool execute{false};   // --execute: возвращать, а не показывать
    bool yes{false};       // --yes: не задавать вопросов (неинтерактивный режим)
    bool json{false};      // --json: в stdout только машинный вывод
    bool help{false};      // --help / -h

    // --tx <id>: отменить конкретную транзакцию.
    std::optional<std::string> txId;

    // --trash-root <каталог>: корзина приложения (FR-7). Пусто —
    // %ProgramData%\MrProper\Trash. Ключ обязателен не для красоты: песочница
    // проверки и CI обязаны держать транзакции в своём каталоге, а не в чужом
    // ProgramData (тот же контракт, что у apply).
    std::string trashRoot;

    // --conflict skip|overwrite|rename: ответ сразу на все конфликты.
    // core::ConflictPolicy::Ask (по умолчанию) — команда спрашивает человека.
    core::ConflictPolicy conflict{core::ConflictPolicy::Ask};
    bool conflictGiven{false};

    // --keep-transaction: оставить каталог транзакции после полного возврата.
    // По умолчанию каталог сносится (FR-7 «≤ 2 ГБ и ≤ 7 дней»: возвращённое
    // содержимое не должно занимать место), и манифест уходит вместе с ним.
    // Ключ нужен там, где состояние транзакции после отмены обязано быть видно
    // на диске (проверка FR-7 и §7.2 в e2e: state=undone в manifest.json).
    bool keepTransaction{false};
};

// Разбор аргументов команды (без имени команды: вызывающий отдаёт только хвост).
// Пустой optional с заполненной error — ошибка разбора; пустой optional с
// пустой error означает «напечатать --help».
std::optional<UndoOptions> parseUndoOptions(const std::vector<std::string>& args, std::string& error);

// Текст --help. Отдельная функция, а не константа в заголовке: общий список
// команд печатает каркас, и его текст живёт здесь, рядом с разбором ключей.
const char* undoUsage();

// ---------------------------------------------------------------------------
// Проекции движка для вывода
// ---------------------------------------------------------------------------

// Почему план построить не вышло. Отдельное поле, а не разбор текста: по
// строке «транзакция не найдена» нельзя надёжно отличить «отменять нечего» (4)
// от «манифест бит» (7), а CI обязан их различать.
enum class UndoProblem : int {
    None,
    NoRoot,        // корень корзины неизвестен
    NotFound,      // транзакции нет в корзине
    NotUndoable,   // состояние undone/collapsed: отмены больше нет (§7.2)
    Corrupt,       // манифест не прочитан или каталог с ним разошёлся
};

const char* toString(UndoProblem problem);

struct UndoTransactionView {
    std::string txId;
    std::string appVersion;
    std::string state;  ///< committed | open | undone | collapsed
    std::int64_t createdUnix{};
    std::uint64_t bytes{};
    std::uint32_t items{};
    std::uint32_t restorable{};
    bool available{};
    bool expired{};
    std::string note;  ///< RU: почему недоступно
};

struct UndoSnapshot {
    std::string trashRoot;
    std::vector<UndoTransactionView> transactions;  ///< от новых к старым
    std::vector<std::string> brokenDirs;            ///< каталоги без манифеста
    std::uint64_t totalBytes{};
    std::uint32_t availableTransactions{};
    std::uint32_t restorableItems{};
    std::uint32_t expiredTransactions{};
    bool rootKnown{true};
    UndoProblem problem{UndoProblem::None};
    std::string detail;
};

struct UndoPlanEntryView {
    // Индекс записи в транзакции: по нему ответы пользователя адресуются ядру
    // (core::RestoreAnswers::byEntry), а по позиции в плане — нет: при частичном
    // восстановлении это разные пространства индексов (см. undo_service.cpp).
    std::size_t entryIndex{};
    std::string originalPath;
    std::string targetPath;  ///< при Rename — новое имя
    std::uint64_t bytes{};
    std::string action;    ///< restore | skip | rename | overwrite
    std::string conflict;  ///< пусто, если конфликта нет
    bool needsUserDecision{};
    std::string note;
};

struct UndoPlanView {
    std::string txId;
    std::vector<UndoPlanEntryView> entries;
    std::vector<std::string> questions;
    // Предупреждения движка о том, что он не смог проверить: том неизвестен,
    // занятость не определена, содержимого нет. Это НЕ вопросы пользователю и
    // не конфликты — смешивать их нельзя, иначе «конфликтов: 1» появилось бы
    // там, где ни одного конфликта нет.
    std::vector<std::string> warnings;
    std::uint64_t bytesPlanned{};
    std::uint32_t conflictCount{};
    std::uint32_t skippedCount{};
    bool requiresUserDecision{};
    bool crossVolume{};
    bool slowEnoughToWarn{};
    std::uint64_t copySeconds{};
    bool ok{};
    UndoProblem problem{UndoProblem::None};
    std::string detail;
    // Политика, по которой план построен. Исполнение перестраивает план заново
    // (цель перепроверяется перед переносом — правило 5 шапки undo_service.hpp),
    // и без этого поля перестроенный план вернулся бы к Ask, потеряв явный
    // --conflict overwrite/rename.
    core::ConflictPolicy policy{core::ConflictPolicy::Ask};
};

struct UndoItemView {
    std::string targetPath;
    std::string action;
    bool ok{};
    std::uint64_t bytes{};
    std::string status;
    std::uint32_t win32Error{};
    std::string detail;
};

struct UndoRestoreView {
    std::string txId;
    std::vector<UndoItemView> items;
    std::uint32_t restored{};
    std::uint32_t skipped{};
    std::uint32_t failed{};
    std::uint32_t notAttempted{};
    std::uint64_t bytesRestored{};
    bool cancelled{};
    bool manifestWritten{};
    std::string manifestProblem;
    bool purged{};
    std::string summary;
    bool ok{};  ///< план построен и исполнение состоялось
    UndoProblem problem{UndoProblem::None};
    std::string detail;
};

struct UndoReport {
    UndoSnapshot snapshot;
    UndoPlanView plan;
    UndoRestoreView restore;
    bool executed{false};   ///< true только если что-то реально вернули
    bool confirmed{false};  ///< подтверждение получено (в том числе по --yes)
    std::string refusal;    ///< почему ничего не восстановлено; пусто, если вопросов не было
};

// ---------------------------------------------------------------------------
// Потоки и внедряемые зависимости
// ---------------------------------------------------------------------------
// Потоки передаются явно по трём причинам, что и у apply: команда проверяется
// тестом без перенаправления stdout, машинный вывод отделён от человеческого,
// а ответы на вопросы не берутся из глобального std::cin.
struct UndoIo {
    std::ostream& out;  ///< машина: JSON или текст списка
    std::ostream& err;  ///< человек: снимок, вопросы, ошибки
    std::istream& in;   ///< ответы: подтверждение и решения по конфликтам
};

// Вопрос человеку и его ответ. false — спросить не удалось (конец ввода), а
// это отказ, а не согласие.
using UndoPrompt = std::function<bool(const std::string& question, std::string& answer)>;

// Снимок корзины: что вообще можно отменить. false + заполненный snapshot с
// problem — корень неизвестен или корзина не читается.
using UndoSnapshotFn = std::function<bool(std::int64_t nowUnix, UndoSnapshot& out)>;

// План восстановления транзакции при заданной политике конфликтов. false —
// план построить не вышло; причина в out.problem.
using UndoPlanFn = std::function<bool(const std::string& txId, core::ConflictPolicy policy, std::int64_t nowUnix,
                                      UndoPlanView& out)>;

// Ответы пользователя по конфликтам: индекс записи в транзакции и решение.
// Пустой вектор — ответов не было, и тогда неотвеченный конфликт остаётся
// пропущенным, а не перезаписанным (FR-7).
using UndoAnswers = std::vector<std::pair<std::size_t, core::ConflictPolicy>>;

// Исполнение плана с ответами. Пересчёт по ответам делает исполнитель (ядро:
// «нет ответа — пропуск»), и out.items отражает ПРИНЯТЫЕ решения, а не план до
// вопросов. false — восстановление не состоялось; причина в out.problem.
using UndoRestoreFn =
    std::function<bool(const UndoPlanView&, const UndoAnswers&, std::int64_t nowUnix, UndoRestoreView& out)>;

// Применить ключи команды к окружению. Единственный канал, которым разобранные
// --trash-root и --keep-transaction доходят до движка: окружение создаёт каркас
// ДО разбора аргументов (args.cpp), поэтому передать их иначе нечем.
using UndoConfigureFn = std::function<bool(const std::string& trashRoot, bool purgeAfterFullRestore)>;

// Корень корзины, который движок использует на самом деле (заданный --trash-root
// или платформенный по умолчанию). Нужен для снимка при `undo --tx`: без него
// строка «корзина:» в снимке была бы пустой, хотя корзина известна.
using UndoRootFn = std::function<std::string()>;

struct UndoEnvironment {
    UndoSnapshotFn snapshot;   ///< пусто → отмена недоступна (NoService)
    UndoPlanFn plan;           ///< пусто → NoService
    UndoRestoreFn restore;     ///< пусто → NoService
    UndoConfigureFn configure; ///< пусто → корень корзины остаётся платформенным
    UndoRootFn trashRoot;      ///< пусто → корень в снимке неизвестен
    UndoPrompt ask;            ///< пусто → вопрос в err, ответ из io.in
    std::string appVersion;    ///< для журнала и манифеста
};

// Готовое окружение настоящей программы: движок engine::undo_service поверх
// платформы. Это ЕДИНСТВЕННОЕ место слоя cli, где берётся undo_service: разбор
// аргументов, показ и коды остаются переносимыми и проверяются без диска.
UndoEnvironment makeFileUndoEnvironment();

// Вопрос в err, ответ — одна строка из in. false, если строку прочитать не
// удалось; answer в этом случае пуст.
bool askUndoOnStream(const std::string& question, const UndoIo& io, std::string& answer);

// ---------------------------------------------------------------------------
// Точки входа
// ---------------------------------------------------------------------------

// Основная функция: разбор, показ, подтверждение, восстановление и код
// возврата. report заполняется всегда (кроме ошибки разбора аргументов).
UndoExit runUndo(const std::vector<std::string>& args, const UndoIo& io, const UndoEnvironment& env, UndoReport& report);

// Оборачиватель для каркаса: тот же выход, но без отчёта.
int runUndoCommand(const std::vector<std::string>& args, const UndoIo& io, const UndoEnvironment& env);

// Слово отмены по транзакции: последняя отменяемая в снимке или пустая строка,
// если отменять нечего (SPEC §7.2 — «Ctrl+Z серый»).
std::string lastUndoableTransaction(const UndoSnapshot& snapshot);

// Список транзакций текстом (--list). Отдельная функция, чтобы её мог
// прочитать и e2e, и help.
std::string formatUndoListText(const UndoOptions& options, const UndoSnapshot& snapshot);

}  // namespace mrproper::cli
