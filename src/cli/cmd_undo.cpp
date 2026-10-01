// mrproper-cli: команда `undo` — реализация (SPEC §4 FR-7, §4 FR-9, §7.2).
//
// Контракт и коды описаны в cmd_undo.hpp; здесь — код. Порядок функций
// повторяет порядок жизни команды:
//
//   разбор аргументов → снимок корзины → выбор транзакции → план от движка →
//   показ снимка ДО вопроса → [подтверждение] → [решения по конфликтам] →
//   восстановление → итог и код возврата.
//
// Проверки, которые нельзя пропустить (каждая с комментарием «почему»):
//   * снимок печатается ДО вопроса (FR-5), иначе человек подтверждает то, чего
//     не видел;
//   * слово подтверждения сверяется дословно, как у apply: ответ «почти такой же»
//     не подтверждение;
//   * молчание не перезаписывает (FR-7): `--yes` при политике Ask превращает
//     неотвеченный конфликт в пропуск, а не в перезапись;
//   * план без единого возвращаемого элемента — это «нечего восстанавливать» (4),
//     а не успех с нулём: повторная отмена обязана быть тихой и честной;
//   * ключ -Destructive у отмены не принимается: отмена ничего не разрушает
//     сверх возврата файлов, а неизвестный ключ обязан быть ошибкой разбора.
//
// Подключение движка (makeFileUndoEnvironment в конце файла) — единственное
// место, где слой cli берёт engine::undo_service: разбор аргументов, показ и
// коды возврата остаются переносимыми и проверяются без диска (§11.1).
#include "cmd_undo.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "args.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/trash.hpp"
#include "core/units.hpp"
#include "engine/undo_service.hpp"

namespace mrproper::cli {
namespace {

// ---------------------------------------------------------------------------
// Мелкие помощники: строки и числа
// ---------------------------------------------------------------------------

std::string toLowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return text.substr(begin, end - begin);
}

std::int64_t nowUnixSeconds() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Дата в местном времени для человека. Список транзакций читают глазами («когда
// это было»), а машина получает unix-секунды в документе --json: текст, зависящий
// от локали и часового пояса, в машинном выводе был бы дефектом (§11.4).
std::string formatLocalTime(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return "не передано";
    const std::time_t seconds = static_cast<std::time_t>(unixSeconds);
    std::tm parts{};
#if defined(_WIN32)
    if (localtime_s(&parts, &seconds) != 0) return std::to_string(unixSeconds) + " (unix)";
#else
    if (localtime_r(&seconds, &parts) == nullptr) return std::to_string(unixSeconds) + " (unix)";
#endif
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0) {
        return std::to_string(unixSeconds) + " (unix)";
    }
    return std::string{buffer};
}

std::string formatBytes(std::uint64_t bytes) { return core::formatBytes(bytes, 1, true); }

// Русские окончания считаются, а не выбираются на глаз: «1 элемент» и «2
// элемента» в сводке читаются как отчёт, а «1 элементов» — как ошибка вывода.
std::string pluralItems(std::size_t count) {
    if (count == 0) return "0 элементов";
    const std::size_t hundreds = count % 100;
    const std::size_t tens = count % 10;
    if (hundreds >= 11 && hundreds <= 14) return std::to_string(count) + " элементов";
    if (tens == 1) return std::to_string(count) + " элемент";
    if (tens >= 2 && tens <= 4) return std::to_string(count) + " элемента";
    return std::to_string(count) + " элементов";
}

std::string pluralTransactions(std::size_t count) {
    if (count == 0) return "0 транзакций";
    const std::size_t hundreds = count % 100;
    const std::size_t tens = count % 10;
    if (hundreds >= 11 && hundreds <= 14) return std::to_string(count) + " транзакций";
    if (tens == 1) return std::to_string(count) + " транзакция";
    if (tens >= 2 && tens <= 4) return std::to_string(count) + " транзакции";
    return std::to_string(count) + " транзакций";
}

// Сколько элементов плана команда собирается вернуть. Пропущенные записи в это
// число не входят: «вернуть 2 из 5» — честная формулировка, а «вернуть 5 из 5» при
// двух пропущенных конфликтах — враньё, из которого код возврата потом растёт
// неправильно.
std::size_t restorableCount(const UndoPlanView& plan) {
    std::size_t count = 0;
    for (const UndoPlanEntryView& entry : plan.entries) {
        if (entry.action != "skip") ++count;
    }
    return count;
}

core::ConflictPolicy parseConflictPolicy(const std::string& text, bool& ok) {
    const std::string token = toLowerAscii(trim(text));
    ok = true;
    if (token == "ask") return core::ConflictPolicy::Ask;
    if (token == "skip") return core::ConflictPolicy::Skip;
    if (token == "overwrite") return core::ConflictPolicy::Overwrite;
    if (token == "rename") return core::ConflictPolicy::Rename;
    ok = false;
    return core::ConflictPolicy::Ask;
}

const char* policyWord(core::ConflictPolicy policy) {
    switch (policy) {
        case core::ConflictPolicy::Ask: return "пропустить";
        case core::ConflictPolicy::Skip: return "пропустить";
        case core::ConflictPolicy::Overwrite: return "перезаписать";
        case core::ConflictPolicy::Rename: return "рядом";
    }
    return "пропустить";
}

// Ответ на вопрос о конфликте. Слова, а не номера: «р», «с» и «о» читаются как
// «ряд», «skip» и «overwrite» только на английском, а человек у нас русский.
// Любой непонятный ответ — это Skip, потому что «молчание не перезаписывает»
// (FR-7): элемент останется в корзине, и отмена его не потеряет.
core::ConflictPolicy parseConflictAnswer(const std::string& answer, bool& understood) {
    const std::string token = toLowerAscii(trim(answer));
    understood = true;
    if (token == "перезаписать" || token == "заменить" || token == "о" || token == "o" || token == "overwrite") {
        return core::ConflictPolicy::Overwrite;
    }
    if (token == "рядом" || token == "р" || token == "r" || token == "rename") {
        return core::ConflictPolicy::Rename;
    }
    if (token == "пропустить" || token == "п" || token == "s" || token == "skip") return core::ConflictPolicy::Skip;
    understood = false;
    return core::ConflictPolicy::Skip;
}

// ---------------------------------------------------------------------------
// Показ
// ---------------------------------------------------------------------------

// Снимок перед восстановлением (FR-5). Всегда в stderr — и в текстовом режиме,
// и в --json: stdout в машинном режиме остаётся машинным (§6.2).
void printSnapshot(const UndoOptions& /*options*/, const UndoIo& io, const UndoSnapshot& snapshot,
                   const UndoPlanView& plan) {
    io.err << "MrProper: снимок отмены перед восстановлением (FR-5)\n";
    io.err << "  корзина: " << (snapshot.trashRoot.empty() ? "неизвестна" : snapshot.trashRoot) << "\n";
    if (plan.txId.empty()) {
        io.err << "  транзакций в корзине: " + std::to_string(snapshot.transactions.size()) + " · отменяемых: " +
                std::to_string(snapshot.availableTransactions) << "\n";
    } else {
        const UndoTransactionView* tx = nullptr;
        for (const UndoTransactionView& view : snapshot.transactions) {
            if (view.txId == plan.txId) {
                tx = &view;
                break;
            }
        }
        io.err << "  транзакция: " << plan.txId;
        if (tx != nullptr) io.err << " · состояние: " << tx->state << " · создана: " << formatLocalTime(tx->createdUnix);
        io.err << "\n";
        io.err << "  элементов: " + std::to_string(plan.entries.size()) + " · вернуть: " +
                std::to_string(restorableCount(plan)) + " из " + std::to_string(plan.entries.size()) +
                " · объём: " + formatBytes(plan.bytesPlanned) + " · конфликтов: " +
                std::to_string(plan.conflictCount) << "\n";
    }
    if (plan.crossVolume || plan.slowEnoughToWarn) {
        io.err << "  ";
        if (plan.crossVolume) {
            // Том может быть и неизвестен: ядро считает худший случай (копирование),
            // и молчать об этом нельзя — «переместить» окажется «скопировать».
            io.err << "перенос между томами или том неизвестен: возврат будет копированием";
        }
        if (plan.slowEnoughToWarn && plan.copySeconds > 0) {
            io.err << " · это займёт время (около " << plan.copySeconds << " с)";
        }
        io.err << "\n";
    }
    if (!snapshot.brokenDirs.empty()) {
        io.err << "  каталогов без читаемого манифеста: " + std::to_string(snapshot.brokenDirs.size()) << "\n";
    }
    for (const std::string& warning : plan.warnings) io.err << "  ! " << warning << "\n";
    // Вопросы плана печатаются целиком и ДО подтверждения (FR-7: «спросить», а не
    // решить за человека). Дальше каждый конфликт спрашивается отдельно.
    if (plan.requiresUserDecision && !plan.questions.empty()) {
        io.err << "  вопросов по конфликтам: " << std::to_string(plan.questions.size())
                << " — по каждому будет задан вопрос\n";
        for (const std::string& question : plan.questions) io.err << "    · " << question << "\n";
    }
}

void printPlanEntries(const UndoOptions& options, const UndoIo& io, const UndoPlanView& plan) {
    std::ostream& out = options.json ? io.err : io.out;
    out << "MrProper: план восстановления " << plan.txId << " — вернуть " << restorableCount(plan) << " из "
        << plan.entries.size() << ", " << formatBytes(plan.bytesPlanned) << "\n";
    std::size_t number = 0;
    for (const UndoPlanEntryView& entry : plan.entries) {
        ++number;
        out << "  [" << number << "] " << entry.action << " · " << formatBytes(entry.bytes) << " · "
            << entry.targetPath << "\n";
        if (entry.action == "skip") {
            const std::string reason = entry.note.empty()
                                           ? (entry.conflict.empty() ? std::string("пропущено") : entry.conflict)
                                           : entry.note;
            out << "        причина: " << reason << "\n";
        } else if (!entry.conflict.empty() && entry.action != "restore") {
            out << "        конфликт: " << entry.conflict << " → " << entry.action << "\n";
        }
    }
}

void printRestoreResult(const UndoOptions& options, const UndoIo& io, const UndoRestoreView& restore) {
    std::ostream& out = options.json ? io.err : io.out;
    out << "MrProper: " << (restore.summary.empty() ? ("восстановление " + restore.txId) : restore.summary) << "\n";
    std::size_t number = 0;
    for (const UndoItemView& item : restore.items) {
        ++number;
        out << "  [" << number << "] " << (item.ok ? "восстановлено" : "не восстановлено") << " · "
            << formatBytes(item.bytes) << " · " << item.targetPath << "\n";
        if (!item.ok && !item.detail.empty()) out << "        причина: " << item.detail << "\n";
    }
    out << "  вернулось: " << restore.restored << " · пропущено: " << restore.skipped << " · ошибок: "
        << restore.failed << " · не выполнено: " << restore.notAttempted << " · байт: " << restore.bytesRestored
        << "\n";
    if (!restore.manifestWritten) {
        out << "  ВНИМАНИЕ: манифест не приведён в соответствие с фактом"
            << (restore.manifestProblem.empty() ? "" : (": " + restore.manifestProblem))
            << " — корзину надо перечитать\n";
    }
    if (restore.purged) out << "  каталог транзакции удалён: возвращено всё\n";
    if (restore.cancelled) out << "  восстановление прервано: невыполненное осталось в корзине\n";
}

// ---------------------------------------------------------------------------
// Коды возврата
// ---------------------------------------------------------------------------

// Ничего не вернулось и нечего возвращать — тихий честный отказ (4), а не ошибка.
UndoExit nothingToUndo(const UndoIo& io, UndoReport& report, const std::string& why) {
    report.refusal = why;
    io.err << kProgramName << " undo: отменять нечего: " << why << "\n";
    io.err << kProgramName << ": посмотреть, что есть в корзине: " << kProgramName << " undo --list\n";
    return UndoExit::NothingToUndo;
}

UndoExit classifyProblem(UndoProblem problem) {
    switch (problem) {
        case UndoProblem::NoRoot: return UndoExit::NoService;
        case UndoProblem::NotFound:
        case UndoProblem::NotUndoable: return UndoExit::NothingToUndo;
        case UndoProblem::None:
        case UndoProblem::Corrupt: break;
    }
    return UndoExit::Failed;
}

// Код по итогу восстановления: полностью — 0, часть — 6, ничего — 7. Пропуски
// по решению пользователя тоже делают результат неполным: человек ответил
// «пропустить» и должен увидеть это в коде, а не в тишине stdout.
UndoExit classifyRestore(const UndoRestoreView& restore, std::size_t planned) {
    if (restore.restored >= planned && restore.failed == 0 && restore.skipped == 0 && restore.notAttempted == 0) {
        return UndoExit::Ok;
    }
    if (restore.restored > 0) return UndoExit::Partial;
    return UndoExit::Failed;
}

// ---------------------------------------------------------------------------
// Машинный вывод
// ---------------------------------------------------------------------------

mrproper::json::Value numberValue(std::uint64_t value) {
    return mrproper::json::Value(static_cast<double>(value));
}

mrproper::json::Value transactionJson(const UndoTransactionView& tx) {
    using mrproper::json::Value;
    return Value::object({
        {"txId", Value(tx.txId)},
        {"appVersion", Value(tx.appVersion)},
        {"state", Value(tx.state)},
        {"createdAt", numberValue(static_cast<std::uint64_t>(tx.createdUnix < 0 ? 0 : tx.createdUnix))},
        {"bytes", numberValue(tx.bytes)},
        {"items", numberValue(tx.items)},
        {"restorable", numberValue(tx.restorable)},
        {"available", Value(tx.available)},
        {"expired", Value(tx.expired)},
        {"note", Value(tx.note)},
    });
}

mrproper::json::Value stringArray(const std::vector<std::string>& values) {
    using mrproper::json::Value;
    std::vector<Value> items;
    items.reserve(values.size());
    for (const std::string& value : values) items.push_back(Value(value));
    return Value::array(std::move(items));
}

mrproper::json::Value planJson(const UndoPlanView& plan) {
    using mrproper::json::Value;
    std::vector<Value> entries;
    std::vector<Value> questions;
    entries.reserve(plan.entries.size());
    for (const UndoPlanEntryView& entry : plan.entries) {
        entries.push_back(Value::object({
            {"index", numberValue(entry.entryIndex)},
            {"originalPath", Value(entry.originalPath)},
            {"targetPath", Value(entry.targetPath)},
            {"bytes", numberValue(entry.bytes)},
            {"action", Value(entry.action)},
            {"conflict", Value(entry.conflict)},
            {"needsUserDecision", Value(entry.needsUserDecision)},
            {"note", Value(entry.note)},
        }));
    }
    for (const std::string& question : plan.questions) questions.push_back(Value(question));

    return Value::object({
        {"txId", Value(plan.txId)},
        {"policy", Value(std::string(core::toString(plan.policy)))},
        {"entries", Value::array(std::move(entries))},
        {"questions", Value::array(std::move(questions))},
        {"warnings", stringArray(plan.warnings)},
        {"bytesPlanned", numberValue(plan.bytesPlanned)},
        {"restorable", numberValue(restorableCount(plan))},
        {"conflicts", numberValue(plan.conflictCount)},
        {"skipped", numberValue(plan.skippedCount)},
        {"requiresUserDecision", Value(plan.requiresUserDecision)},
        {"crossVolume", Value(plan.crossVolume)},
        {"slowEnoughToWarn", Value(plan.slowEnoughToWarn)},
        {"copySeconds", numberValue(plan.copySeconds)},
        {"ok", Value(plan.ok)},
        {"problem", Value(toString(plan.problem))},
        {"detail", Value(plan.detail)},
    });
}

mrproper::json::Value restoreJson(const UndoRestoreView& restore) {
    using mrproper::json::Value;
    std::vector<Value> items;
    items.reserve(restore.items.size());
    for (const UndoItemView& item : restore.items) {
        items.push_back(Value::object({{"targetPath", Value(item.targetPath)},
                                       {"action", Value(item.action)},
                                       {"ok", Value(item.ok)},
                                       {"bytes", numberValue(item.bytes)},
                                       {"status", Value(item.status)},
                                       {"win32Error", numberValue(item.win32Error)},
                                       {"detail", Value(item.detail)}}));
    }
    return Value::object({
        {"txId", Value(restore.txId)},
        {"items", Value::array(std::move(items))},
        {"restored", numberValue(restore.restored)},
        {"skipped", numberValue(restore.skipped)},
        {"failed", numberValue(restore.failed)},
        {"notAttempted", numberValue(restore.notAttempted)},
        {"bytesRestored", numberValue(restore.bytesRestored)},
        {"cancelled", Value(restore.cancelled)},
        {"manifestWritten", Value(restore.manifestWritten)},
        {"manifestProblem", Value(restore.manifestProblem)},
        {"purged", Value(restore.purged)},
        {"summary", Value(restore.summary)},
        {"ok", Value(restore.ok)},
        {"problem", Value(toString(restore.problem))},
        {"detail", Value(restore.detail)},
    });
}

const char* modeName(UndoMode mode) {
    switch (mode) {
        case UndoMode::List: return "list";
        case UndoMode::Plan: return "plan";
        case UndoMode::Restore: return "restore";
    }
    return "list";
}

std::string undoJson(const UndoOptions& options, const UndoReport& report) {
    using mrproper::json::Value;
    std::vector<Value> transactions;
    transactions.reserve(report.snapshot.transactions.size());
    for (const UndoTransactionView& tx : report.snapshot.transactions) transactions.push_back(transactionJson(tx));
    std::vector<Value> broken;
    for (const std::string& dir : report.snapshot.brokenDirs) broken.push_back(Value(dir));

    Value plan = Value();
    if (!report.plan.txId.empty() || report.plan.problem != UndoProblem::None) plan = planJson(report.plan);
    Value restore = Value();
    if (!report.restore.txId.empty() || report.restore.problem != UndoProblem::None) {
        restore = restoreJson(report.restore);
    }

    const std::string document =
        Value::object({
            {"schema", numberValue(1)},
            {"kind", Value(std::string("undo"))},
            {"mode", Value(std::string(modeName(options.mode)))},
            {"trashRoot", Value(report.snapshot.trashRoot)},
            {"totalBytes", numberValue(report.snapshot.totalBytes)},
            {"availableTransactions", numberValue(report.snapshot.availableTransactions)},
            {"restorableItems", numberValue(report.snapshot.restorableItems)},
            {"expiredTransactions", numberValue(report.snapshot.expiredTransactions)},
            {"brokenDirs", Value::array(std::move(broken))},
            {"transactions", Value::array(std::move(transactions))},
            {"plan", plan},
            {"restore", restore},
            {"executed", Value(report.executed)},
            {"confirmed", Value(report.confirmed)},
            {"refusal", Value(report.refusal)},
        })
            .dump(2);
    return document + "\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Тексты
// ---------------------------------------------------------------------------

const char* toString(UndoProblem problem) {
    switch (problem) {
        case UndoProblem::None: return "none";
        case UndoProblem::NoRoot: return "no-root";
        case UndoProblem::NotFound: return "not-found";
        case UndoProblem::NotUndoable: return "not-undoable";
        case UndoProblem::Corrupt: return "corrupt";
    }
    return "none";
}

std::string formatUndoListText(const UndoOptions& /*options*/, const UndoSnapshot& snapshot) {
    std::ostringstream text;
    text << "MrProper: корзина приложения: " << (snapshot.trashRoot.empty() ? "неизвестна" : snapshot.trashRoot)
         << "\n";
    if (snapshot.transactions.empty()) text << "  транзакций нет: отменять нечего\n";
    for (const UndoTransactionView& tx : snapshot.transactions) {
        text << "  " << tx.txId << " · " << formatLocalTime(tx.createdUnix) << " · " << tx.items << " элем. · "
             << formatBytes(tx.bytes) << " · " << tx.state;
        if (tx.available) {
            text << " · отменяема";
        } else if (!tx.note.empty()) {
            text << " · " << tx.note;
        }
        if (tx.expired) text << " · просрочена";
        text << "\n";
    }
    text << "  всего: " << pluralTransactions(snapshot.transactions.size()) << " · отменяемых: "
         << snapshot.availableTransactions << " · элементов: " << pluralItems(snapshot.restorableItems)
         << " · объём: " << formatBytes(snapshot.totalBytes);
    if (snapshot.expiredTransactions > 0) text << " · просрочено: " << snapshot.expiredTransactions;
    if (!snapshot.brokenDirs.empty()) text << " · битых каталогов: " << std::to_string(snapshot.brokenDirs.size());
    text << "\n";
    return text.str();
}

std::string lastUndoableTransaction(const UndoSnapshot& snapshot) {
    for (const UndoTransactionView& tx : snapshot.transactions) {
        if (tx.available && !tx.txId.empty()) return tx.txId;
    }
    return std::string{};
}

const char* undoUsage() {
    static const std::string text = [] {
        std::string usage = "Использование: mrproper-cli undo [ключи]\n"
                            "\n"
                            "Отмена очистки: список транзакций корзины и восстановление файлов\n"
                            "(SPEC §4 FR-7, §7.2 «отмена доступна, пока транзакция не схлопнулась»).\n"
                            "По умолчанию — сухой прогон: команда показывает, что вернётся, и ничего\n"
                            "не переносит. Возврат включается ключом --execute и только после\n"
                            "подтверждения.\n"
                            "\n"
                            "Ключи:\n"
                            "  --list                        транзакции корзины: дата, элементы, объём (FR-9)\n"
                            "  --last                        отменить последнюю отменяемую транзакцию\n"
                            "  --tx <id>                     отменить конкретную транзакцию\n"
                            "  --execute                     вернуть файлы (без него — сухой прогон)\n"
                            "  --yes, -y                     не спрашивать подтверждение (неинтерактивный режим)\n"
                            "  --conflict skip|overwrite|rename\n"
                            "                                ответ сразу на все конфликты; без ключа — спросить\n"
                            "  --keep-transaction               не сносить каталог транзакции после полного\n"
                            "                                  возврата: манифест останется со state=undone\n"
                            "  --json                        машинный вывод в stdout; текст и вопрос — в stderr\n"
                            "  --trash-root <каталог>        корзина приложения (FR-7);\n"
                            "                                  пусто — %ProgramData%\\MrProper\\Trash\n"
                            "  -h, --help                    этот текст\n"
                            "\n"
                            "Коды возврата:\n"
                            "  0  список показан или отмена выполнена полностью\n"
                            "  2  не разобраны аргументы\n"
                            "  3  подтверждение не получено — не восстановлено ни байта\n"
                            "  4  отменять нечего: корзина пуста, транзакция не найдена, уже\n"
                            "     восстановлена или схлопнулась\n"
                            "  5  отмена недоступна: корень корзины неизвестен\n"
                            "  6  восстановлено частично: часть элементов пропущена или с ошибкой\n"
                            "  7  не вернулось ни одного элемента\n"
                            "\n"
                            "Правила безопасности:\n"
                            "  * снимок (транзакция, элементы, объём, конфликты) печатается до вопроса;\n"
                            "  * слово подтверждения — ";
        usage += kUndoConfirmationToken;
        usage +=
            "; ключ --yes его отменяет, а ключа -Destructive у отмены нет:\n"
            "    отмена ничего не разрушает сверх возврата файлов;\n"
            "  * занятое место не перезаписывается молча (FR-7): без --conflict команда\n"
            "    спрашивает «перезаписать / рядом / пропустить», а с --yes неотвеченный\n"
            "    конфликт становится пропуском, а не перезаписью;\n"
            "  * ключ -Destructive не принимается: отмена не разрушает данные.\n";
        return usage;
    }();
    return text.c_str();
}

// ---------------------------------------------------------------------------
// Разбор аргументов
// ---------------------------------------------------------------------------

std::optional<UndoOptions> parseUndoOptions(const std::vector<std::string>& args, std::string& error) {
    UndoOptions options;
    bool sawTarget = false;  // задан --tx или --last
    bool sawList = false;

    const auto valueOf = [&args, &error](std::size_t& index, const std::string& key, std::string& out) {
        if (index + 1 >= args.size()) {
            error = key + ": не хватает значения";
            return false;
        }
        ++index;
        out = args[index];
        if (trim(out).empty()) {
            error = key + ": пустое значение";
            return false;
        }
        return true;
    };

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
            continue;
        }
        if (arg == "--json") {
            options.json = true;
            continue;
        }
        if (arg == "--yes" || arg == "-y") {
            options.yes = true;
            continue;
        }
        if (arg == "--execute") {
            options.execute = true;
            continue;
        }
        if (arg == "--list") {
            options.list = true;
            sawList = true;
            continue;
        }
        if (arg == "--keep-transaction") {
            options.keepTransaction = true;
            continue;
        }
        if (arg == "--last") {
            options.last = true;
            sawTarget = true;
            continue;
        }
        if (arg == "--tx") {
            std::string value;
            if (!valueOf(index, arg, value)) return std::nullopt;
            if (options.txId.has_value()) {
                error = "--tx задан дважды";
                return std::nullopt;
            }
            options.txId = value;
            sawTarget = true;
            continue;
        }
        if (arg == "--trash-root") {
            std::string value;
            if (!valueOf(index, arg, value)) return std::nullopt;
            options.trashRoot = value;
            continue;
        }
        if (arg == "--conflict") {
            std::string value;
            if (!valueOf(index, arg, value)) return std::nullopt;
            bool ok = false;
            const core::ConflictPolicy policy = parseConflictPolicy(value, ok);
            if (!ok) {
                error = "--conflict \"" + value + "\": ожидается ask, skip, overwrite или rename";
                return std::nullopt;
            }
            options.conflict = policy;
            options.conflictGiven = true;
            continue;
        }
        // Неизвестный ключ — ошибка, а не предупреждение: молча проигнорированный
        // --exectute превратил бы сухой прогон в настоящую отмену.
        error = "неизвестный ключ: " + arg;
        return std::nullopt;
    }

    if (options.help) {
        options.mode = UndoMode::List;
        return options;
    }

    // Что именно просит человек: три взаимоисключающих режима, и смешивать их
    // нельзя — «показать список и сразу отменить последнее» не имеет смысла.
    if (sawList && sawTarget) {
        error = "--list и --last/--tx вместе не имеет смысла: выберите одно";
        return std::nullopt;
    }
    if (!sawList && !sawTarget) {
        error = "не выбрано действие: --list, --last или --tx <id>";
        return std::nullopt;
    }
    if (sawList && options.execute) {
        error = "--list с --execute вместе не имеет смысла: список ничего не возвращает";
        return std::nullopt;
    }

    options.mode = sawList ? UndoMode::List : (options.execute ? UndoMode::Restore : UndoMode::Plan);

    // Идентификатор транзакции проверяется тем же правилом, что и на диске
    // (core::isValidTxId): иначе путь склеился бы с чужим каталогом, и отказ
    // пришёл бы от файловой системы вместо внятного сообщения об аргументе.
    if (options.txId.has_value() && !core::isValidTxId(*options.txId)) {
        error = "--tx \"" + *options.txId +
                "\": недопустимый идентификатор транзакции (ожидаются только буквы, цифры, точка и дефис)";
        return std::nullopt;
    }
    return options;
}

// ---------------------------------------------------------------------------
// Вопросы
// ---------------------------------------------------------------------------

bool askUndoOnStream(const std::string& question, const UndoIo& io, std::string& answer) {
    answer.clear();
    io.err << question;
    io.err.flush();
    if (!std::getline(io.in, answer)) return false;
    // \r от CRLF в консоли cmd.exe обязан уйти, иначе слово подтверждения
    // никогда не совпадёт и отмена будет недоступна в живом терминале.
    while (!answer.empty() && (answer.back() == '\r' || answer.back() == '\n')) answer.pop_back();
    return true;
}

// ---------------------------------------------------------------------------
// Основной ход
// ---------------------------------------------------------------------------

UndoExit runUndo(const std::vector<std::string>& args, const UndoIo& io, const UndoEnvironment& env,
                 UndoReport& report) {
    std::string error;
    const std::optional<UndoOptions> parsed = parseUndoOptions(args, error);
    if (!parsed.has_value()) {
        if (error.empty()) {
            io.err << undoUsage();
        } else {
            io.err << kProgramName << " undo: " << error << "\n";
            io.err << kProgramName << ": подсказка: " << kProgramName << " undo --help\n";
        }
        return UndoExit::Usage;
    }
    const UndoOptions options = *parsed;
    if (options.help) {
        io.out << undoUsage();
        return UndoExit::Ok;
    }

    // Сервис не подключён — честный отказ (5), а не «нечего восстанавливать»:
    // разница видна по коду, и CI не примет тишину за пустую корзину.
    if (!env.snapshot || !env.plan || !env.restore) {
        report.refusal = "движок восстановления не подключён";
        io.err << kProgramName << " undo: ОШИБКА: отмена недоступна: " << report.refusal << "\n";
        return UndoExit::NoService;
    }

    // Единственный канал, которым разобранные --trash-root и --keep-transaction
    // доходят до движка: окружение создаёт каркас ДО разбора аргументов.
    if (env.configure && !env.configure(options.trashRoot, !options.keepTransaction)) {
        report.refusal =
            options.trashRoot.empty() ? "корень корзины недоступен" : ("корзина недоступна: " + options.trashRoot);
        io.err << kProgramName << " undo: ОШИБКА: " << report.refusal << "\n";
        return UndoExit::NoService;
    }
    // Корень, который движок использует на самом деле: без него строка «корзина:»
    // в снимке перед `undo --tx` была бы «неизвестна», хотя корзина известна.
    if (env.trashRoot) {
        report.snapshot.trashRoot = env.trashRoot();
    } else if (!options.trashRoot.empty()) {
        report.snapshot.trashRoot = options.trashRoot;
    }

    const std::int64_t now = nowUnixSeconds();

    // --- Список -------------------------------------------------------------
    if (options.mode == UndoMode::List) {
        if (!env.snapshot(now, report.snapshot)) {
            report.refusal = report.snapshot.detail.empty() ? "корзина не прочитана" : report.snapshot.detail;
            io.err << kProgramName << " undo: ОШИБКА: " << report.refusal << "\n";
            return classifyProblem(report.snapshot.problem);
        }
        if (options.json) {
            io.out << undoJson(options, report);
        } else {
            io.out << formatUndoListText(options, report.snapshot);
        }
        core::logInfo("undo.list", "снимок корзины показан",
                      core::LogFields{core::logField("root", report.snapshot.trashRoot),
                                      core::logField("transactions", report.snapshot.transactions.size()),
                                      core::logField("available", report.snapshot.availableTransactions)});
        return UndoExit::Ok;
    }

    // --- Выбор транзакции ---------------------------------------------------
    std::string txId;
    if (options.txId.has_value()) {
        txId = *options.txId;
    } else {
        // Снимок нужен и здесь: без него нечем ни выбрать последнюю транзакцию,
        // ни показать человеку, что именно он сейчас отменяет (FR-5).
        if (!env.snapshot(now, report.snapshot)) {
            report.refusal = report.snapshot.detail.empty() ? "корзина не прочитана" : report.snapshot.detail;
            io.err << kProgramName << " undo: ОШИБКА: " << report.refusal << "\n";
            return classifyProblem(report.snapshot.problem);
        }
        txId = lastUndoableTransaction(report.snapshot);
        if (txId.empty()) {
            const std::string why =
                report.snapshot.transactions.empty()
                    ? "корзина пуста"
                    : ("отменяемых транзакций нет среди " + std::to_string(report.snapshot.transactions.size()));
            return nothingToUndo(io, report, why);
        }
    }

    // --- План ----------------------------------------------------------------
    if (!env.plan(txId, options.conflict, now, report.plan)) {
        report.plan.txId = txId;
        report.plan.detail = report.plan.detail.empty() ? "план восстановления не построен" : report.plan.detail;
        report.refusal = report.plan.detail;
        const UndoExit problem = classifyProblem(report.plan.problem);
        if (problem == UndoExit::NothingToUndo) {
            // Тот же тихий честный отказ, что и пустая корзина: «нечего», а потом
            // причина движка. Без рамки сообщение движка («манифест не прочитан:
            // notFound») выглядит как поломка, а отменять тут нечего — и человек
            // решил бы, что утилита сломалась (SPEC §5: ни один отказ не роняет
            // процесс и обязан быть объяснён).
            io.err << kProgramName << " undo: отменять нечего: ";
            if (report.plan.problem == UndoProblem::NotUndoable) {
                io.err << "транзакция " << txId << " уже восстановлена или схлопнулась (§7.2)\n";
            } else {
                io.err << "транзакции " << txId << " нет в корзине "
                       << (report.snapshot.trashRoot.empty() ? std::string("(корень неизвестен)")
                                                            : report.snapshot.trashRoot)
                       << "\n";
            }
        }
        io.err << kProgramName << " undo: причина: " << report.plan.detail << "\n";
        core::logError("undo.plan", "план восстановления не построен",
                       core::LogFields{core::logField("tx", txId),
                                       core::logField("problem", toString(report.plan.problem)),
                                       core::logField("detail", report.plan.detail)});
        return problem;
    }
    report.plan.txId = report.plan.txId.empty() ? txId : report.plan.txId;
    report.plan.policy = options.conflict;

    const std::size_t planned = restorableCount(report.plan);
    if (planned == 0 && !report.plan.requiresUserDecision) {
        // Транзакция прочитана, но возвращать нечего: всё помечено пропущенным
        // (уже возвращено, содержимое утрачено, родителя нет). Это тот же
        // «нечего восстанавливать», что и пустая корзина (§7.2).
        //
        // requiresUserDecision здесь — исключение, а не мелочь: конфликт делает
        // элемент пропущенным ДО решения человека, и «нечего восстанавливать»
        // было бы враньём (FR-7 требует спросить, а не отказать). Такой план
        // идёт дальше: показ, вопросы, восстановление по ответу.
        std::string why = "в транзакции " + txId + " нет элементов, которые можно вернуть";
        for (const UndoPlanEntryView& entry : report.plan.entries) {
            if (!entry.note.empty()) {
                why += ": " + entry.note;
                break;
            }
        }
        printSnapshot(options, io, report.snapshot, report.plan);
        printPlanEntries(options, io, report.plan);
        return nothingToUndo(io, report, why);
    }

    // --- Снимок ДО вопроса (FR-5) -------------------------------------------
    printSnapshot(options, io, report.snapshot, report.plan);

    // --- Сухой прогон --------------------------------------------------------
    if (options.mode == UndoMode::Plan) {
        printPlanEntries(options, io, report.plan);
        report.refusal = "сухой прогон: --execute не задан, возвращено 0 байт";
        if (!options.json) {
            io.out << "Ничего не восстановлено: это был сухой прогон (FR-5). Вернуть: " << kProgramName
                   << " undo --tx " << txId << " --execute — команда спросит подтверждение\n";
        }
        if (options.json) io.out << undoJson(options, report);
        core::logInfo("undo.dryrun", "сухой прогон отмены",
                      core::LogFields{core::logField("tx", txId),
                                      core::logField("entries", report.plan.entries.size()),
                                      core::logField("restorable", planned),
                                      core::logField("bytes", report.plan.bytesPlanned)});
        return UndoExit::Ok;
    }

    // --- Подтверждение -------------------------------------------------------
    if (!options.yes) {
        std::string question = "MrProper: будет возвращено " + pluralItems(planned) + ", " +
                               formatBytes(report.plan.bytesPlanned);
        if (report.plan.requiresUserDecision) {
            question += "; конфликтов: " + std::to_string(report.plan.conflictCount);
        }
        question += ". Введите " + std::string(kUndoConfirmationToken) + " для подтверждения (Ctrl+C — отмена): ";

        std::string answer;
        const bool asked = env.ask ? env.ask(question, answer) : askUndoOnStream(question, io, answer);
        if (!asked) {
            report.refusal = "подтверждение не получено (нет ввода)";
            io.err << kProgramName << " undo: подтверждение не получено (нет ввода) — ничего не восстановлено\n";
            core::logWarn("undo.refused", "подтверждение не получено: нет ввода", core::LogFields{});
            if (options.json) io.out << undoJson(options, report);
            return UndoExit::Refused;
        }
        if (trim(answer) != kUndoConfirmationToken) {
            report.refusal = "подтверждение не получено (ожидалось " + std::string(kUndoConfirmationToken) + ")";
            io.err << kProgramName << " undo: подтверждение не получено (ожидалось " << kUndoConfirmationToken
                   << ") — ничего не восстановлено\n";
            core::logWarn("undo.refused", "подтверждение не получено: ответ не совпал",
                          core::LogFields{core::logField("tx", txId)});
            if (options.json) io.out << undoJson(options, report);
            return UndoExit::Refused;
        }
    }
    report.confirmed = true;

    // --- Решения по конфликтам ----------------------------------------------
    //
    // Порядок именно такой: сначала подтверждение всего возврата, потом ответы
    // по конфликтам. Иначе человек подтвердил бы «3 файла», а потом узнал, что
    // один из них перезапишет чужой, — а вопрос он уже не задавал.
    UndoAnswers answers;
    if (report.plan.requiresUserDecision && options.conflict == core::ConflictPolicy::Ask) {
        if (options.yes) {
            // Молчание не перезаписывает (FR-7): без ответа конфликт пропускается,
            // элемент остаётся в корзине, и отмена его не потеряна.
            io.err << kProgramName
                   << " undo: --yes без --conflict: конфликты пропущены, ничего не перезаписано (FR-7)\n";
        } else {
            std::size_t number = 0;
            for (const UndoPlanEntryView& entry : report.plan.entries) {
                ++number;
                if (!entry.needsUserDecision) continue;
                const std::string question =
                    kProgramName + std::string(": конфликт ") + std::to_string(number) + " из " +
                    std::to_string(report.plan.entries.size()) + " (" +
                    (entry.conflict.empty() ? std::string("занято") : entry.conflict) + "): " + entry.targetPath +
                    " — введите «перезаписать», «рядом» или «пропустить» (Enter — пропустить): ";
                std::string answer;
                const bool asked = env.ask ? env.ask(question, answer) : askUndoOnStream(question, io, answer);
                if (!asked) {
                    // Конец ввода — это «не ответил», а не «согласился»: элемент
                    // остаётся в корзине, и отмена его не потеряна (FR-7).
                    io.err << "  ввод закончился — элемент пропущен\n";
                    answers.emplace_back(entry.entryIndex, core::ConflictPolicy::Skip);
                    continue;
                }
                bool understood = false;
                const core::ConflictPolicy chosen = parseConflictAnswer(answer, understood);
                if (!understood && !trim(answer).empty()) {
                    io.err << "  непонятный ответ \"" << trim(answer) << "\" — " << policyWord(chosen)
                           << " (FR-7: молчание не перезаписывает)\n";
                }
                io.err << "  решение: " << policyWord(chosen) << "\n";
                answers.emplace_back(entry.entryIndex, chosen);
            }
        }
    }

    // --- Восстановление ------------------------------------------------------
    if (!env.restore(report.plan, answers, now, report.restore)) {
        report.restore.txId = report.restore.txId.empty() ? txId : report.restore.txId;
        report.refusal = report.restore.detail.empty() ? "восстановление не состоялось" : report.restore.detail;
        io.err << kProgramName << " undo: ОШИБКА: " << report.refusal << "\n";
        core::logError("undo.restore", "восстановление не состоялось",
                       core::LogFields{core::logField("tx", txId), core::logField("detail", report.refusal)});
        if (options.json) io.out << undoJson(options, report);
        return classifyProblem(report.restore.problem);
    }
    report.restore.txId = report.restore.txId.empty() ? txId : report.restore.txId;
    report.executed = report.restore.restored > 0;
    printRestoreResult(options, io, report.restore);

    const UndoExit code = classifyRestore(report.restore, restorableCount(report.plan));
    if (code == UndoExit::Failed) {
        report.refusal = "не вернулось ни одного элемента: " + report.restore.summary;
    }
    if (options.json) io.out << undoJson(options, report);
    return code;
}

int runUndoCommand(const std::vector<std::string>& args, const UndoIo& io, const UndoEnvironment& env) {
    UndoReport report;
    return static_cast<int>(runUndo(args, io, env, report));
}

// ---------------------------------------------------------------------------
// Мост к движку: единственное место слоя cli, где берётся engine::undo_service
// ---------------------------------------------------------------------------

namespace {

using engine::undo_service::ItemResult;
using engine::undo_service::RestorePlanResult;
using engine::undo_service::RestoreResult;
using engine::undo_service::Snapshot;
using engine::undo_service::TransactionView;
using engine::undo_service::UndoService;

// Причина отказа плана в код возврата. Разбор текста ошибки здесь был бы
// хрупким: по строке «транзакция не найдена» нельзя надёжно отличить «отменять
// нечего» (4) от «манифест бит» (7), а CI обязан их различать. Причина
// приходит из платформенного кода статуса, и он устроен так, чтобы различие
// было однозначным.
UndoProblem classify(const RestorePlanResult& result) {
    if (result.ok) return UndoProblem::None;
    if (result.status == platform::TrashStatus::NotFound) return UndoProblem::NotFound;
    if (result.status == platform::TrashStatus::Corrupt) return UndoProblem::Corrupt;
    if (result.status == platform::TrashStatus::Cancelled) return UndoProblem::Corrupt;
    if (result.status == platform::TrashStatus::InvalidArgument) {
        // Транзакция прочитана, но отменяема не (core::TrashTransaction::undoable):
        // open / undone / collapsed. Отличать их от «идентификатор не тот» при
        // одном и том же коде можно только по тому, нашёлся ли каталог.
        const std::string& problem = result.problem;
        if (problem.find("уже восстановлена") != std::string::npos ||
            problem.find("схлопнулась") != std::string::npos || problem.find("не закрыта") != std::string::npos ||
            problem.find("пуста") != std::string::npos) {
            return UndoProblem::NotUndoable;
        }
        return UndoProblem::NotFound;
    }
    return UndoProblem::Corrupt;
}

UndoTransactionView project(const TransactionView& view) {
    UndoTransactionView out;
    out.txId = view.txId;
    out.appVersion = view.appVersion;
    out.createdUnix = view.createdUnix;
    out.bytes = view.bytes;
    out.items = view.itemCount;
    out.restorable = view.restorableCount;
    out.state = core::toString(view.state);
    out.available = view.available;
    out.expired = view.expired;
    out.note = view.note;
    return out;
}

void projectPlan(const RestorePlanResult& result, UndoPlanView& out) {
    out.txId = result.txId;
    out.detail = result.problem;
    out.ok = result.ok;
    out.problem = classify(result);
    out.entries.clear();
    out.questions.clear();
    if (!result.ok) return;
    const core::UndoPlan& plan = result.plan;
    out.entries.reserve(plan.entries.size());
    for (const core::UndoPlanEntry& entry : plan.entries) {
        UndoPlanEntryView item;
        item.entryIndex = entry.entryIndex;
        item.originalPath = entry.originalPath;
        item.targetPath = entry.targetPath;
        item.bytes = entry.sizeBytes;
        item.action = core::toString(entry.action);
        item.conflict = core::toString(entry.conflict);
        item.needsUserDecision = entry.needsUserDecision;
        item.note = entry.note;
        out.entries.push_back(std::move(item));
    }
    out.questions = plan.questions;
    out.warnings = result.warnings;
    out.bytesPlanned = plan.bytesPlanned;
    out.conflictCount = plan.conflictCount;
    out.skippedCount = plan.skippedCount;
    out.requiresUserDecision = plan.requiresUserDecision;
    out.crossVolume = plan.cost.crossVolume;
    out.slowEnoughToWarn = plan.cost.slowEnoughToWarn;
    out.copySeconds = plan.cost.seconds;
}

void projectRestore(const RestoreResult& result, UndoRestoreView& out) {
    out.txId = result.txId;
    out.restored = result.outcome.restoredCount;
    out.skipped = result.outcome.skippedCount;
    out.failed = result.outcome.failedCount;
    out.notAttempted = result.outcome.notAttemptedCount;
    out.bytesRestored = result.outcome.bytesRestored;
    out.cancelled = result.cancelled;
    out.manifestWritten = result.manifestWritten;
    out.manifestProblem = result.manifestProblem;
    out.purged = result.purged;
    out.summary = result.summary;
    out.ok = true;
    out.items.clear();
    out.items.reserve(result.items.size());
    for (const ItemResult& item : result.items) {
        UndoItemView view;
        view.targetPath = item.targetPath;
        view.action = core::toString(item.decision);
        view.ok = item.ok;
        view.bytes = item.bytesRestored;
        view.status = platform::toString(item.status);
        view.win32Error = item.win32Error;
        view.detail = UndoService::detailText(item);
        out.items.push_back(std::move(view));
    }
}

}  // namespace

UndoEnvironment makeFileUndoEnvironment() {
    UndoEnvironment env;
    env.appVersion = appVersion();

    // Сервис живёт в разделяемом владельце: окружение копируется по значнию, а
    // корень корзины задаётся позже, из --trash-root.
    auto service = std::make_shared<UndoService>();

    env.configure = [service](const std::string& trashRoot, bool purgeAfterFullRestore) {
        if (!trashRoot.empty()) service->setRoot(trashRoot);
        service->setPurgeAfterFullRestore(purgeAfterFullRestore);
        return true;
    };

    env.trashRoot = [service]() { return service->root(); };

    env.snapshot = [service](std::int64_t nowUnix, UndoSnapshot& out) {
        out.trashRoot = service->root();
        if (out.trashRoot.empty()) {
            // Корень неизвестен — это отказ (5), а не «корзина пуста»: иначе
            // отсутствие %ProgramData% читалось бы как «отменять нечего».
            out.rootKnown = false;
            out.problem = UndoProblem::NoRoot;
            out.detail = "корень корзины неизвестен (%ProgramData% недоступен для этой сессии)";
            return false;
        }
        const Snapshot snapshot = service->snapshot(nowUnix);
        out.brokenDirs = snapshot.brokenDirs;
        out.totalBytes = snapshot.totalBytes;
        out.availableTransactions = snapshot.availableTransactions;
        out.restorableItems = snapshot.restorableItems;
        out.expiredTransactions = snapshot.expiredTransactions;
        out.transactions.clear();
        out.transactions.reserve(snapshot.transactions.size());
        for (const TransactionView& view : snapshot.transactions) out.transactions.push_back(project(view));
        return true;
    };

    env.plan = [service](const std::string& txId, core::ConflictPolicy policy, std::int64_t nowUnix,
                         UndoPlanView& out) {
        core::RestoreRequest request;
        request.txId = txId;
        request.conflictPolicy = policy;
        const RestorePlanResult prepared = service->planRestore(txId, request, nowUnix);
        out.policy = policy;
        projectPlan(prepared, out);
        return prepared.ok;
    };

    env.restore = [service](const UndoPlanView& view, const UndoAnswers& answers, std::int64_t nowUnix,
                            UndoRestoreView& out) {
        // План строится заново, с той же политикой и с ответами: так цель
        // перепроверяется непосредственно перед переносом (правило 5 шапки
        // undo_service.hpp) — между показом снимка и возвратом мог появиться файл.
        // Восстановленный план нужен здесь только для ответа «почему не вышло»,
        // поэтому в отчёт он не попадает: отчёт заполняется из restore().
        UndoPlanView rebuilt;
        core::RestoreRequest request;
        request.txId = view.txId;
        request.conflictPolicy = view.policy;
        RestorePlanResult prepared = service->planRestore(view.txId, request, nowUnix);
        projectPlan(prepared, rebuilt);
        if (!prepared.ok) {
            out.problem = rebuilt.problem;
            out.detail = rebuilt.detail.empty() ? "план восстановления не построен" : rebuilt.detail;
            return false;
        }

        core::RestoreAnswers given;
        given.byEntry = answers;
        if (!given.byEntry.empty()) {
            // «Нет ответа — пропуск» — правило ядра, и оно одно на весь проект
            // (core::applyRestoreAnswers). Своим перебором записей мы бы нарушили
            // его вторым местом.
            prepared.plan = core::applyRestoreAnswers(std::move(prepared.plan), std::move(given));
            projectPlan(prepared, rebuilt);
        }

        const RestoreResult result = service->restore(prepared, nowUnix);
        projectRestore(result, out);
        return true;
    };

    return env;
}

}  // namespace mrproper::cli
