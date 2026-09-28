// mrproper-cli: команды `plan` и `apply` — реализация (SPEC §4 FR-5).
//
// Контракт и разбор решений описаны в cmd_apply.hpp; здесь — код. Порядок
// функций повторяет порядок жизни команды:
//
//   разбор аргументов → источник кандидатов → фильтр по категориям →
//   core::buildPlan → core::validatePlan → показ (dry-run) → [подтверждение] →
//   core::DryRunGate → выполнение → итог.
//
// Проверки, которые нельзя пропустить (каждая с комментарием «почему»):
//   * validatePlan до показа и до исполнения — инварианты §6.3, а нарушенный
//     план удаляет не то;
//   * --candidates вместе с --execute запрещено — список из файла мог устареть;
//   * снимок состояния печатается до вопроса, а не после (FR-5);
//   * подтверждение спрашивается всегда, кроме явного --yes, и ответ обязан
//     совпасть с токеном дословно (регистр и пробелы нормализуются);
//   * ошибка одной операции не отменяет остальные (FR-6), но попадает в лог и
//     в exit-код.
#include "cmd_apply.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/log.hpp"
#include "core/units.hpp"

namespace mrproper::cli {
namespace {

// Верхняя граница числа, которое ещё можно прочитать из JSON без потери
// точности: json::Value хранит числа как double, а байты до 2^64 в double не
// помещаются. 2^64 — первое значение, которое уже не представимо.
constexpr double kTwoTo64 = 18446744073709551616.0;

// ---------------------------------------------------------------------------
// Мелкие помощники: строки и числа
// ---------------------------------------------------------------------------

std::string toLowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string toUpperAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return text.substr(begin, end - begin);
}

std::vector<std::string> splitCommas(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ',') {
            parts.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    parts.push_back(trim(current));
    return parts;
}

// Разбор целого без std::stoll: тот бросает исключения, а CLI-разбор должен
// возвращать ошибку словом, а не падать стеком.
bool parseIntRange(const std::string& text, long long minValue, long long maxValue, long long& out) {
    if (text.empty()) return false;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }
    if (index >= text.size()) return false;
    long long value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return false;
        const long long digit = static_cast<long long>(c - '0');
        if (value > (9223372036854775807LL - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (negative) value = -value;
    if (value < minValue || value > maxValue) return false;
    out = value;
    return true;
}

// Размер с необязательным суффиксом: 512, 512k, 40m, 2g. Множитель двоичный
// (1k = 1024 байт) и записывается в текст --help, чтобы «что я написал» и «что
// поняла программа» совпадали.
bool parseSize(const std::string& text, std::uint64_t& out) {
    std::string digits = trim(text);
    std::uint64_t multiplier = 1;
    if (!digits.empty()) {
        const char last = static_cast<char>(std::tolower(static_cast<unsigned char>(digits.back())));
        if (last == 'k' || last == 'm' || last == 'g') {
            multiplier = (last == 'k') ? 1024ull : (last == 'm' ? 1024ull * 1024ull : 1024ull * 1024ull * 1024ull);
            digits.pop_back();
        }
    }
    long long value = 0;
    if (!parseIntRange(digits, 0, 9223372036854775807LL, value)) return false;
    if (multiplier != 1 && static_cast<std::uint64_t>(value) > 0xFFFFFFFFFFFFFFFFull / multiplier) return false;
    out = static_cast<std::uint64_t>(value) * multiplier;
    return true;
}

// Свои имена действий: core::toString(PlanAction) объявлен в model.hpp, но
// определения в ядре пока нет, а тянуть ради подписи зависимость от того, что
// её завтра напишут, не нужно. Тексты — по-русски, как остальной вывод CLI.
const char* actionName(core::PlanAction action) {
    switch (action) {
        case core::PlanAction::Delete: return "удалить";
        case core::PlanAction::Trash: return "в корзину";
        case core::PlanAction::Keep: return "оставить";
        case core::PlanAction::SkipLocked: return "пропустить (занято)";
    }
    return "оставить";
}

std::string formatTimestamp(std::int64_t unixSeconds) {
    // Время снимка (FR-5) печатаем unix-секундами: локаль и часовой пояс в
    // вывод CLI означали бы, что один и тот же прогон на двух машинах даёт
    // разный текст, а отчёт ради этого и делается детерминированным (§11.4).
    if (unixSeconds <= 0) return "не передано";
    return std::to_string(unixSeconds) + " (unix)";
}

std::string pluralItems(std::size_t count) {
    if (count == 0) return "0 элементов";
    const std::size_t hundreds = count % 100;
    const std::size_t tens = count % 10;
    const bool last = hundreds >= 11 && hundreds <= 14;
    if (last) return std::to_string(count) + " элементов";
    if (tens == 1) return std::to_string(count) + " элемент";
    if (tens >= 2 && tens <= 4) return std::to_string(count) + " элемента";
    return std::to_string(count) + " элементов";
}

std::string pluralOperations(std::size_t count) {
    if (count == 0) return "0 операций";
    const std::size_t hundreds = count % 100;
    const std::size_t tens = count % 10;
    const bool last = hundreds >= 11 && hundreds <= 14;
    if (last) return std::to_string(count) + " операций";
    if (tens == 1) return std::to_string(count) + " операция";
    if (tens >= 2 && tens <= 4) return std::to_string(count) + " операции";
    return std::to_string(count) + " операций";
}

// ---------------------------------------------------------------------------
// Чтение кандидатов из JSON
// ---------------------------------------------------------------------------

std::optional<std::uint64_t> readUint(const mrproper::json::Value& object, const char* key) {
    const mrproper::json::Value* field = object.find(key);
    if (field == nullptr || !field->isNumber()) return std::nullopt;
    const double number = field->asNumber();
    if (number < 0.0 || number >= kTwoTo64) return std::nullopt;
    if (number != std::floor(number)) return std::nullopt;
    return static_cast<std::uint64_t>(number);
}

std::optional<std::int64_t> readInt64(const mrproper::json::Value& object, const char* key) {
    const mrproper::json::Value* field = object.find(key);
    if (field == nullptr || !field->isNumber()) return std::nullopt;
    const double number = field->asNumber();
    if (number != std::floor(number)) return std::nullopt;
    if (number < -kTwoTo64 || number >= kTwoTo64) return std::nullopt;
    return static_cast<std::int64_t>(number);
}

std::string readString(const mrproper::json::Value& object, const char* key) {
    const mrproper::json::Value* field = object.find(key);
    if (field == nullptr || !field->isString()) return std::string();
    return field->asString();
}

bool readBool(const mrproper::json::Value& object, const char* key) {
    const mrproper::json::Value* field = object.find(key);
    return field != nullptr && field->isBool() && field->asBool();
}

core::SafetyLevel readSafety(const std::string& text, bool& ok) {
    const std::string token = toLowerAscii(trim(text));
    ok = true;
    if (token == "safe") return core::SafetyLevel::Safe;
    if (token == "review") return core::SafetyLevel::Review;
    if (token == "risky") return core::SafetyLevel::Risky;
    ok = false;
    return core::SafetyLevel::Review;
}

bool parseCandidate(const mrproper::json::Value& item, core::CleanupCandidate& out, std::string& error) {
    if (!item.isObject()) {
        error = "элемент списка не объект";
        return false;
    }
    core::CleanupCandidate candidate;
    candidate.path = readString(item, "path");
    if (trim(candidate.path).empty()) {
        // Единственное обязательное поле. Всё остальное имеет осмысленное
        // умолчание, а кандидат без пути нельзя ни показать, ни удалить.
        error = "нет обязательного поля \"path\"";
        return false;
    }
    candidate.ruleId = readString(item, "ruleId");
    candidate.category = readString(item, "category");
    candidate.displayName = readString(item, "displayName");
    if (candidate.displayName.empty()) candidate.displayName = candidate.path;

    if (const std::optional<std::uint64_t> value = readUint(item, "logicalBytes")) {
        candidate.logicalBytes = *value;
    }
    if (const std::optional<std::uint64_t> value = readUint(item, "allocatedBytes")) {
        candidate.allocatedBytes = *value;
    }
    if (const std::optional<std::uint64_t> value = readUint(item, "fileCount")) {
        const std::uint64_t count = *value;
        if (count > 0xFFFFFFFFull) {
            error = "fileCount больше, чем помещается в uint32";
            return false;
        }
        candidate.fileCount = static_cast<std::uint32_t>(count);
    }
    if (const std::optional<std::int64_t> value = readInt64(item, "oldestWrite")) candidate.oldestWrite = *value;
    if (const std::optional<std::int64_t> value = readInt64(item, "newestWrite")) candidate.newestWrite = *value;
    if (const std::optional<std::int64_t> value = readInt64(item, "lastAccess")) candidate.lastAccess = *value;

    const std::optional<std::uint64_t> confidence = readUint(item, "confidence");
    if (confidence.has_value()) {
        if (*confidence > 100) {
            error = "confidence " + std::to_string(*confidence) + " вне 0..100";
            return false;
        }
        candidate.confidence = static_cast<int>(*confidence);
    }

    const std::string safetyToken = readString(item, "safety");
    if (!safetyToken.empty()) {
        bool ok = false;
        candidate.safety = readSafety(safetyToken, ok);
        if (!ok) {
            error = "safety \"" + safetyToken + "\": ожидается safe, review или risky";
            return false;
        }
    }

    if (const mrproper::json::Value* reasons = item.find("reasons"); reasons != nullptr && reasons->isArray()) {
        for (const mrproper::json::Value& reason : reasons->items()) {
            if (!reason.isString()) {
                error = "reasons содержит не строку";
                return false;
            }
            candidate.reasons.push_back(reason.asString());
        }
    }

    if (const mrproper::json::Value* locked = item.find("lockedBy");
        locked != nullptr && locked->isArray()) {
        for (const mrproper::json::Value& process : locked->items()) {
            if (!process.isObject()) {
                error = "lockedBy содержит не объект";
                return false;
            }
            core::ProcessRef ref;
            const std::optional<std::uint64_t> pid = readUint(process, "pid");
            if (pid.has_value() && *pid <= 0xFFFFFFFFull) ref.pid = static_cast<std::uint32_t>(*pid);
            ref.name = readString(process, "name");
            candidate.lockedBy.push_back(std::move(ref));
        }
    }

    if (readBool(item, "locked") && candidate.lockedBy.empty()) {
        // «locked: true» без подробностей — это не кандидат, а непроверяемое
        // утверждение. Молча принять его значило бы получить план, в котором
        // файл, занятый приложением, выбран к удалению. Отказываем элемент.
        error = "locked=true, но lockedBy пуст: неизвестно, кем удерживается файл";
        return false;
    }

    out = std::move(candidate);
    return true;
}

mrproper::json::Value embedJson(const std::string& text, const char* what) {
    // Встраиваем документ, который построили сами (core::planToJson и
    // core::snapshotToJson), а не собираем ключи заново: так схема
    // вывода `plan --json` и `apply --json` не может разойтись с ядром.
    try {
        return mrproper::json::parse(text);
    } catch (const mrproper::json::ParseError&) {
        return mrproper::json::Value::object({{"embeddedError", mrproper::json::Value(what)}});
    }
}

// Числа в JSON — целые (SPEC §11.4), а Value хранит их как double, поэтому
// приведение делается в одном месте и явно.
template <typename T>
mrproper::json::Value numberValue(T value) {
    return mrproper::json::Value(static_cast<double>(static_cast<std::uint64_t>(value)));
}

std::string applyJson(bool executed, bool confirmed, const std::string& refusal, const ApplyReport& report) {
    using mrproper::json::Value;

    const ExecutionSummary& summary = report.execution;

    std::vector<Value> operationItems;
    std::vector<Value> errorItems;
    std::vector<Value> notRunItems;
    operationItems.reserve(summary.operations.size());
    errorItems.reserve(summary.failed);
    notRunItems.reserve(summary.notRun.size());

    const auto operationValue = [](const core::PlanOperation& op) {
        return Value::object({{"index", numberValue(op.candidateIndex)},
                              {"action", Value(std::string(actionName(op.action)))},
                              {"path", Value(op.path)},
                              {"bytes", numberValue(op.bytes)}});
    };

    for (std::size_t i = 0; i < summary.operations.size(); ++i) {
        const core::PlanOperation& op = summary.operations[i];
        const OperationResult& result = i < summary.results.size() ? summary.results[i] : OperationResult{};
        operationItems.push_back(Value::object({{"index", numberValue(op.candidateIndex)},
                                               {"action", Value(std::string(actionName(op.action)))},
                                               {"path", Value(op.path)},
                                               {"bytes", numberValue(op.bytes)},
                                               {"safety", Value(core::toString(op.safety))},
                                               {"ok", Value(result.ok)},
                                               {"freedBytes", numberValue(result.freedBytes)},
                                               {"transactionId", Value(result.transactionId)},
                                               {"detail", Value(result.detail)}}));
        if (!result.ok) {
            errorItems.push_back(Value::object({{"index", numberValue(op.candidateIndex)},
                                                {"path", Value(op.path)},
                                                {"detail", Value(result.detail)}}));
        }
    }
    for (const core::PlanOperation& op : summary.notRun) notRunItems.push_back(operationValue(op));

    Value execution = Value();
    if (executed) {
        execution = Value::object({
            {"attempted", numberValue(summary.attempted)},
            {"succeeded", numberValue(summary.succeeded)},
            {"failed", numberValue(summary.failed)},
            {"freedBytes", numberValue(summary.freedBytes)},
            {"aborted", Value(summary.aborted)},
            {"abortReason", Value(summary.abortReason)},
            {"operations", Value::array(std::move(operationItems))},
            {"errors", Value::array(std::move(errorItems))},
            {"notRun", Value::array(std::move(notRunItems))},
        });
    }

    const std::string document =
        Value::object({
            {"schema", Value(1)},
            {"kind", Value(std::string("apply"))},
            {"dryRun", Value(!executed)},
            {"executed", Value(executed)},
            {"confirmed", Value(confirmed)},
            {"refusal", Value(refusal)},
            {"plan", embedJson(core::planToJson(report.candidates, report.plan), "planToJson")},
            {"snapshot", embedJson(core::snapshotToJson(report.snapshot), "snapshotToJson")},
            {"execution", execution},
        })
            .dump(2);
    return document + "\n";
}

// ---------------------------------------------------------------------------
// Сборка плана
// ---------------------------------------------------------------------------

std::vector<core::CleanupCandidate> filterByCategory(const std::vector<core::CleanupCandidate>& candidates,
                                                      const std::vector<std::string>& categories) {
    if (categories.empty()) return candidates;
    std::vector<std::string> wanted;
    wanted.reserve(categories.size());
    for (const std::string& category : categories) wanted.push_back(toLowerAscii(category));

    std::vector<core::CleanupCandidate> filtered;
    filtered.reserve(candidates.size());
    for (const core::CleanupCandidate& candidate : candidates) {
        const std::string category = toLowerAscii(candidate.category);
        if (std::find(wanted.begin(), wanted.end(), category) != wanted.end()) filtered.push_back(candidate);
    }
    return filtered;
}

// Приводит набор операций к виду, пригодному для показа и для журнала.
void reportSnapshot(const core::PlanSnapshot& snapshot, const core::DryRunReport& dryRun, std::ostream& err) {
    err << "MrProper: снимок состояния перед выполнением (FR-5)\n";
    err << "  версия: " << (snapshot.appVersion.empty() ? "не передана" : snapshot.appVersion);
    err << " · pid: " << snapshot.pid << " · время: " << formatTimestamp(snapshot.createdAtUnix) << "\n";
    err << "  операций: " + std::to_string(snapshot.operationCount) + " · объём: " +
         core::formatBytes(snapshot.totalBytes) + " (аллоцированный размер)\n";
    err << "  отпечаток плана: " << std::to_string(snapshot.planSignature) << "\n";
    err << "  кандидатов всего: " << core::formatCount(dryRun.operations.size() + dryRun.untouched.size()) << "\n";

    // Журнал: FR-5 требует записи снимка, а §12 — пути в каждой ошибке.
    core::logInfo("plan.snapshot", "снимок состояния перед выполнением",
                  core::LogFields{core::logField("operations", snapshot.operationCount),
                                  core::logField("bytes", snapshot.totalBytes),
                                  core::logField("signature", snapshot.planSignature),
                                  core::logField("pid", static_cast<long long>(snapshot.pid)),
                                  core::logField("version", snapshot.appVersion)});
}

// Достаёт кандидатов и строит план. Ничего не печатает: печать — у вызывающих,
// чтобы --json и текстовый режим решали это в одном месте.
PlanExit prepareRun(const ApplyOptions& options, const ApplyIo& io, const ApplyEnvironment& env, ApplyReport& out) {
    out = ApplyReport{};

    std::vector<core::CleanupCandidate> candidates;
    if (options.candidatesPath.has_value()) {
        std::string error;
        if (!loadCandidatesFile(*options.candidatesPath, candidates, error)) {
            io.err << "MrProper: " << error << "\n";
            core::logError("plan.candidates", "не прочитан список кандидатов",
                           core::LogFields{core::logField("file", *options.candidatesPath),
                                           core::logField("error", error)});
            return PlanExit::NoCandidates;
        }
    } else if (env.candidates) {
        std::string error;
        if (!env.candidates(candidates, error)) {
            io.err << "MrProper: скан не дал кандидатов" << (error.empty() ? "" : ": " + error) << "\n";
            core::logError("plan.scan", "скан не дал кандидатов", core::LogFields{core::logField("error", error)});
            return PlanExit::NoCandidates;
        }
    } else {
        io.err << "MrProper: не задано, откуда взять кандидатов: подключите скан "
                  "(ApplyEnvironment::candidates) или укажите --candidates <файл>\n";
        return PlanExit::NoCandidates;
    }

    if (!options.categories.empty()) {
        const std::size_t before = candidates.size();
        candidates = filterByCategory(candidates, options.categories);
        if (candidates.size() != before) {
            io.err << "MrProper: фильтр по категориям оставил " << core::formatCount(candidates.size()) << " из "
                   << core::formatCount(before) << "\n";
        }
    }

    core::PlanOptions planOptions = options.plan;
    // FR-5: dry-run обязателен и включается по умолчанию. Флаг --execute — это
    // единственный способ его выключить, и он ничего не добавляет к «сначала
    // показать»: список операций печатается в обоих случаях.
    planOptions.dryRun = !options.execute;
    out.plan = core::buildPlan(candidates, planOptions);

    // Инварианты §6.3 проверяются до показа и тем более до удаления: план, в
    // котором reclaimBytes не сходится с allocatedBytes, удаляет не то.
    const std::vector<std::string> problems = core::validatePlan(candidates, out.plan);
    if (!problems.empty()) {
        io.err << "MrProper: план не согласован, выполнение запрещено (" << problems.size() << " нарушений):\n";
        for (const std::string& problem : problems) io.err << "  · " << problem << "\n";
        core::logError("plan.invalid", "план нарушает инварианты §6.3",
                       core::LogFields{core::logField("problems", problems.size()),
                                       core::logField("first", problems.front())});
        return PlanExit::Failed;
    }

    out.candidates = std::move(candidates);
    out.dryRun = core::makeDryRunReport(out.candidates, out.plan);

    core::PlanSnapshotContext context;
    if (env.snapshotContext) context = env.snapshotContext();
    out.snapshot = core::makeSnapshot(out.candidates, out.plan, context);
    return PlanExit::Ok;
}

// Показ плана. В режиме --json текст идёт в err: stdout остаётся машинным.
void printPlanText(const ApplyOptions& options, const ApplyIo& io, const ApplyReport& report, bool beforeExecute) {
    std::ostream& target = options.json ? io.err : io.out;
    if (beforeExecute) target << "MrProper: сухой прогон перед выполнением — ниже точный список операций\n";
    target << report.dryRun.text;
    if (report.dryRun.operations.empty()) {
        target << "Под выбранный профиль не подошёл ни один элемент: удалять нечего.\n";
    }
    if (!options.execute) {
        target << "Ничего не удалено: это был сухой прогон (FR-5). Выполнить: mrproper-cli apply --execute";
        if (!options.yes) target << " — команда спросит подтверждение";
        target << "\n";
    }
}

// ---------------------------------------------------------------------------
// Подтверждение
// ---------------------------------------------------------------------------

bool hasRiskyOperation(const core::DryRunReport& dryRun) {
    for (const core::PlanOperation& op : dryRun.operations) {
        if (op.safety == core::SafetyLevel::Risky) return true;
    }
    return false;
}

PlanExit askConfirmation(const ApplyIo& io, const ApplyEnvironment& env, const ApplyReport& report) {
    const std::string token = requiredConfirmationToken(report.dryRun);
    std::string question = "MrProper: будет выполнено " + pluralOperations(report.dryRun.operations.size()) +
                           ", освободится " + core::formatBytes(report.dryRun.totalBytes) +
                           " (аллоцированный размер).";
    if (hasRiskyOperation(report.dryRun)) {
        question += " В списке есть Risky-операции — это двойное подтверждение (SPEC §12).";
    }
    question += "\nВведите " + token + " для подтверждения (Ctrl+C — отмена): ";

    std::string answer;
    const bool asked = env.ask ? env.ask(question, answer) : askOnStream(question, io, answer);
    if (!asked) {
        io.err << "MrProper: подтверждение не получено (нет ввода) — ничего не удалено\n";
        core::logWarn("apply.refused", "подтверждение не получено: нет ввода",
                      core::LogFields{core::logField("signature", report.snapshot.planSignature)});
        return PlanExit::Refused;
    }
    if (toUpperAscii(trim(answer)) != token) {
        io.err << "MrProper: подтверждение не получено (ожидалось " << token << ") — ничего не удалено\n";
        core::logWarn("apply.refused", "подтверждение не получено: ответ не совпал",
                      core::LogFields{core::logField("signature", report.snapshot.planSignature),
                                      core::logField("expected", token)});
        return PlanExit::Refused;
    }
    return PlanExit::Ok;
}

// ---------------------------------------------------------------------------
// Выполнение
// ---------------------------------------------------------------------------

void printOperationResult(std::ostream& out, std::size_t number, const core::PlanOperation& op,
                          const OperationResult& result) {
    out << "  [" << number << "] " << (result.ok ? "ок" : "ошибка") << " · " << actionName(op.action) << " · "
        << core::formatBytes(op.bytes) << " · " << op.path << "\n";
    if (!result.ok) out << "        причина: " << (result.detail.empty() ? "исполнитель не вернул причину" : result.detail)
                        << "\n";
}

PlanExit executePlan(const ApplyOptions& options, const ApplyIo& io, const ApplyEnvironment& env, ApplyReport& out) {
    // 1. Список операций и снимок состояния показываются ДО вопроса и ДО
    //    удаления: FR-5 требует, чтобы человек видел, что именно исчезнет.
    printPlanText(options, io, out, /*beforeExecute=*/true);
    reportSnapshot(out.snapshot, out.dryRun, io.err);

    if (out.dryRun.operations.empty()) {
        out.refusal = "нечего выполнять: в плане 0 операций";
        core::logInfo("apply.empty", "в плане нет операций — выполнение не потребовалось",
                      core::LogFields{core::logField("signature", out.snapshot.planSignature)});
        return PlanExit::Ok;
    }

    // 2. Список из файла удалять нельзя: между сканом и очисткой он мог
    //    устареть (FR-5 — план это снимок, а не разрешение навсегда).
    if (options.candidatesPath.has_value()) {
        out.refusal = "--candidates читает готовый список: удалять по нему нельзя, план мог устареть";
        io.err << "MrProper: " << out.refusal
               << "\n  Удаление доступно только по кандидатам живого скана.\n";
        core::logWarn("apply.refused", "--candidates вместе с --execute запрещено",
                      core::LogFields{core::logField("file", *options.candidatesPath),
                                      core::logField("signature", out.snapshot.planSignature)});
        return PlanExit::Refused;
    }

    // 3. Без исполнителя операций --execute не выполняется. Молча вывести
    //    «удалено 0 байт» здесь означало бы соврать о результате.
    if (!env.executeOperation) {
        out.refusal = "исполнитель операций не подключён (engine::CleanupExecutor ещё не собран)";
        io.err << "MrProper: " << out.refusal << "\n";
        core::logError("apply.noexecutor", "запрошено выполнение без исполнителя операций",
                       core::LogFields{core::logField("signature", out.snapshot.planSignature)});
        return PlanExit::NoExecutor;
    }

    // 4. Подтверждение. Сессия CLI — один запуск процесса, поэтому подтверждение
    //    всегда нужно заново, а --yes — единственный способ его не спрашивать.
    if (options.yes) {
        io.err << "MrProper: подтверждение получено ключом --yes (неинтерактивный режим)\n";
        core::logWarn("apply.confirmed", "подтверждение ключом --yes",
                      core::LogFields{core::logField("signature", out.snapshot.planSignature)});
    } else {
        const PlanExit confirmation = askConfirmation(io, env, out);
        if (confirmation != PlanExit::Ok) {
            out.refusal = "подтверждение не получено";
            return confirmation;
        }
    }
    out.confirmed = true;

    // 5. Ядро проверяет, что показан и подтверждён именно этот план: сменились
    //    выборы, профиль или порог — отпечаток другой, и подтверждение сгорело.
    core::DryRunGate gate;
    gate.beginSession();
    if (gate.mustShowBeforeExecute(out.plan)) {
        gate.acknowledge(out.plan);
    }
    if (!gate.alreadyShown(out.plan)) {
        out.refusal = "план не показан в этой сессии — DryRunGate не пропускает";
        io.err << "MrProper: " << out.refusal << "\n";
        return PlanExit::Refused;
    }

    // 6. Выполнение. Ошибка одной операции не отменяет остальные (FR-6).
    const std::vector<core::PlanOperation>& operations = out.dryRun.operations;
    ExecutionSummary& summary = out.execution;
    summary.operations.reserve(operations.size());
    summary.results.reserve(operations.size());
    out.executed = true;

    const bool perOperationLines = !options.json;  // в --json итог едет в stdout
    for (std::size_t i = 0; i < operations.size(); ++i) {
        const core::PlanOperation& op = operations[i];
        if (op.candidateIndex >= out.candidates.size()) {
            // Такого быть не может после validatePlan, но исполнять операцию с
            // неизвестным кандидатом нельзя: путь в плане тогда ничем не проверен.
            OperationResult result;
            result.ok = false;
            result.detail = "операция ссылается на кандидата " + std::to_string(op.candidateIndex) +
                            ", которого нет в списке";
            summary.operations.push_back(op);
            summary.results.push_back(result);
            ++summary.attempted;
            ++summary.failed;
            printOperationResult(io.err, summary.attempted, op, result);
            continue;
        }

        OperationResult result;
        const bool keepGoing = env.executeOperation(op, out.candidates[op.candidateIndex], result);
        if (result.ok && result.detail.empty()) result.detail = "исполнитель не вернул деталей";
        if (!result.ok && result.detail.empty()) result.detail = "исполнитель не вернул причину";

        summary.operations.push_back(op);
        summary.results.push_back(result);
        ++summary.attempted;
        if (result.ok) {
            ++summary.succeeded;
            summary.freedBytes += result.freedBytes;
            if (perOperationLines) printOperationResult(io.err, summary.attempted, op, result);
        } else {
            ++summary.failed;
            printOperationResult(io.err, summary.attempted, op, result);
            core::logError("apply.operation", "операция не выполнена",
                           core::LogFields{core::logField("path", op.path),
                                           core::logField("action", std::string(actionName(op.action))),
                                           core::logField("bytes", op.bytes),
                                           core::logField("detail", result.detail),
                                           core::logField("transactionId", result.transactionId)});
        }

        if (!keepGoing) {
            summary.aborted = true;
            summary.abortReason = "исполнитель остановил выполнение";
            const auto first = operations.begin() + static_cast<std::ptrdiff_t>(i) + 1;
            summary.notRun.assign(first, operations.end());
            core::logWarn("apply.aborted", "исполнитель остановил выполнение",
                          core::LogFields{core::logField("after", summary.attempted),
                                          core::logField("notRun", summary.notRun.size())});
            break;
        }
    }

    io.err << "MrProper: выполнено " << summary.succeeded << " из " << summary.attempted << ", освобождено "
           << core::formatBytes(summary.freedBytes) << " (по аллоцированному размеру), ошибок: " << summary.failed;
    if (!summary.notRun.empty()) io.err << ", не начато: " << summary.notRun.size();
    io.err << "\n";
    core::logInfo("apply.done", "план выполнен",
                  core::LogFields{core::logField("attempted", summary.attempted),
                                  core::logField("succeeded", summary.succeeded),
                                  core::logField("failed", summary.failed),
                                  core::logField("freedBytes", summary.freedBytes),
                                  core::logField("notRun", summary.notRun.size())});
    return summary.failed == 0 ? PlanExit::Ok : PlanExit::Failed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Разбор аргументов
// ---------------------------------------------------------------------------

std::optional<ApplyOptions> parseApplyOptions(const std::vector<std::string>& args, std::string& error) {
    error.clear();
    ApplyOptions options;
    bool dryRunFlag = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        const auto takeValue = [&](std::string& value) {
            if (index + 1 >= args.size()) {
                error = "ключу " + arg + " нужно значение";
                return false;
            }
            value = args[++index];
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            options.help = true;
            continue;
        }
        if (arg == "--json") {
            options.json = true;
            continue;
        }
        if (arg == "--execute") {
            options.execute = true;
            continue;
        }
        if (arg == "--dry-run") {
            dryRunFlag = true;
            continue;
        }
        if (arg == "--yes" || arg == "-y") {
            options.yes = true;
            continue;
        }
        if (arg == "--no-trash") {
            options.plan.useTrash = false;
            continue;
        }
        if (arg == "--allow-risky") {
            options.plan.allowRisky = true;
            continue;
        }
        if (arg == "--profile") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            // Свой разбор вместо core::selectionProfileFromString: тот для
            // неизвестного имени молча возвращает Recommended, а опечатка в
            // профиле не должна молча менять то, что собираются удалить.
            const std::string name = toLowerAscii(trim(value));
            if (name == "safe-only" || name == "safeonly" || name == "safe") {
                options.plan.profile = core::SelectionProfile::SafeOnly;
            } else if (name == "recommended" || name == "default") {
                options.plan.profile = core::SelectionProfile::Recommended;
            } else if (name == "everything" || name == "all") {
                options.plan.profile = core::SelectionProfile::Everything;
            } else {
                error = "неизвестный профиль «" + value + "»: бывают safe-only, recommended, everything";
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--confidence-threshold") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            long long parsed = 0;
            if (!parseIntRange(value, 0, 100, parsed)) {
                error = "--confidence-threshold: ожидается число 0..100, получено «" + value + "»";
                return std::nullopt;
            }
            options.plan.confidenceThreshold = static_cast<int>(parsed);
            continue;
        }
        if (arg == "--min-reclaim") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            if (!parseSize(value, options.plan.minReclaimBytes)) {
                error = "--min-reclaim: ожидается размер в байтах (можно 512k, 40m, 2g), получено «" + value + "»";
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--trash-direct-delete-above") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            if (!parseSize(value, options.plan.trashDirectDeleteAboveBytes)) {
                error = "--trash-direct-delete-above: ожидается размер, получено «" + value + "»";
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--category") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            for (const std::string& part : splitCommas(value)) {
                if (!part.empty()) options.categories.push_back(part);
            }
            if (options.categories.empty()) {
                error = "--category: пустое имя категории";
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--candidates") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            if (trim(value).empty()) {
                error = "--candidates: пустой путь";
                return std::nullopt;
            }
            options.candidatesPath = value;
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            error = "неизвестный ключ: " + arg;
            return std::nullopt;
        }
        error = "команда не принимает позиционных аргументов: " + arg;
        return std::nullopt;
    }

    if (dryRunFlag && options.execute) {
        error = "--dry-run и --execute взаимоисключающи: выберите одно";
        return std::nullopt;
    }
    return options;
}

const char* planUsage() {
    return "Использование: mrproper-cli plan [ключи]\n"
           "\n"
           "Показывает план очистки и ничего не удаляет. Расчёт тот же, что на экране\n"
           "\"Очистка\": профиль, порог уверенности, корзина приложения (SPEC §4 FR-5).\n"
           "Скомандовать удаление этой командой нельзя — для этого `apply`.\n"
           "\n"
           "Ключи:\n"
           "  --json                          машинный вывод: документ плана в stdout\n"
           "  --profile <имя>                 safe-only | recommended (по умолчанию) | everything\n"
           "  --confidence-threshold <0..100> порог уверенности кандидата (по умолчанию 50)\n"
           "  --allow-risky                   показать и выбрать Risky (по умолчанию скрыты, §12)\n"
           "  --no-trash                      всё прямым удалением, минуя корзину приложения\n"
           "  --min-reclaim <размер>          не брать кандидатов меньше размера (1024 в k/m/g)\n"
           "  --trash-direct-delete-above <размер>\n"
           "                                  выше этого объёма удаляем сразу, а не в корзину\n"
           "  --category <id>[,<id>…]        только эти категории; ключ можно повторять\n"
           "  --candidates <файл>             кандидаты из JSON: дамп скана или отчёт core::report_json\n"
           "  -h, --help                      этот текст\n";
}

const char* applyUsage() {
    return "Использование: mrproper-cli apply [ключи]\n"
           "\n"
           "Очистка с обязательным подтверждением (SPEC §4 FR-5). По умолчанию — сухой\n"
           "прогон: команда показывает точный список операций и ничего не удаляет.\n"
           "Удаление включается ключом --execute и только после подтверждения.\n"
           "\n"
           "Ключи:\n"
           "  --execute                       выполнить план (без него — сухой прогон)\n"
           "  --yes, -y                       не спрашивать подтверждение (неинтерактивный режим CI)\n"
           "  --json                          машинный вывод в stdout; текст и вопрос — в stderr\n"
           "  --profile <имя>                 safe-only | recommended (по умолчанию) | everything\n"
           "  --confidence-threshold <0..100> порог уверенности кандидата (по умолчанию 50)\n"
           "  --allow-risky                   разрешить Risky; подтверждение станет DELETE-RISKY\n"
           "  --no-trash                      всё прямым удалением, минуя корзину приложения\n"
           "  --min-reclaim <размер>          не брать кандидатов меньше размера (1024 в k/m/g)\n"
           "  --trash-direct-delete-above <размер>\n"
           "                                  выше этого объёма удаляем сразу, а не в корзину\n"
           "  --category <id>[,<id>…]        только эти категории; ключ можно повторять\n"
           "  --candidates <файл>             кандидаты из JSON; вместе с --execute запрещено:\n"
           "                                  список из файла мог устареть, удалять по нему нельзя\n"
           "  -h, --help                      этот текст\n"
           "\n"
           "Правила безопасности:\n"
           "  * без --execute не удаляется ни одного байта (FR-5, dry-run по умолчанию);\n"
           "  * снимок состояния (операции, PID, версия, объём) печатается до вопроса;\n"
           "  * слово подтверждения — DELETE, а если в плане есть Risky — DELETE-RISKY;\n"
           "  * Risky по умолчанию скрыты, --allow-risky — второе подтверждение (§12).\n";
}

// ---------------------------------------------------------------------------
// Кандидаты из JSON
// ---------------------------------------------------------------------------

std::vector<core::CleanupCandidate> parseCandidatesJson(std::string_view text, std::string& error) {
    error.clear();
    std::vector<core::CleanupCandidate> candidates;

    std::string_view body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEFu && static_cast<unsigned char>(body[1]) == 0xBBu &&
        static_cast<unsigned char>(body[2]) == 0xBFu) {
        body.remove_prefix(3);  // BOM от Notepad и PowerShell
    }
    if (trim(std::string(body)).empty()) {
        error = "пустой документ";
        return candidates;
    }

    mrproper::json::Value root;
    try {
        root = mrproper::json::parse(body);
    } catch (const mrproper::json::ParseError& exception) {
        error = std::string("не разобрался JSON: ") + exception.what();
        return candidates;
    }

    const mrproper::json::Value* array = nullptr;
    if (root.isArray()) {
        array = &root;
    } else if (root.isObject()) {
        array = root.find("candidates");
        if (array == nullptr) {
            error = "в объекте нет раздела \"candidates\" (ожидался отчёт core::report_json или массив кандидатов)";
            return candidates;
        }
    } else {
        error = "ожидался объект с разделом \"candidates\" или массив кандидатов";
        return candidates;
    }
    if (!array->isArray()) {
        error = "раздел \"candidates\" не массив";
        return candidates;
    }

    candidates.reserve(array->items().size());
    for (std::size_t i = 0; i < array->items().size(); ++i) {
        core::CleanupCandidate candidate;
        std::string itemError;
        if (!parseCandidate(array->items()[i], candidate, itemError)) {
            error = "кандидат " + std::to_string(i) + ": " + itemError;
            candidates.clear();
            return candidates;
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

bool loadCandidatesFile(const std::string& path, std::vector<core::CleanupCandidate>& out, std::string& error) {
    out.clear();
    error.clear();

    // Путь в модели — UTF-8 (§6.3), а std::filesystem::path на Windows ждёт
    // UTF-16, поэтому собираем u8string явно: так путь с кириллицей открывается
    // одинаково в любой кодовой странице.
    const std::filesystem::path filePath{std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())};
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream) {
        error = "не открылся файл со списком кандидатов: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) {
        error = "не прочитан файл со списком кандидатов: " + path;
        return false;
    }
    if (buffer.str().size() > kMaxCandidatesFileBytes) {
        error = "файл больше " + core::formatBytes(kMaxCandidatesFileBytes) + ": это не дамп скана: " + path;
        return false;
    }

    std::string parseError;
    std::vector<core::CleanupCandidate> candidates = parseCandidatesJson(buffer.str(), parseError);
    if (!parseError.empty()) {
        error = path + ": " + parseError;
        return false;
    }
    out = std::move(candidates);
    return true;
}

// ---------------------------------------------------------------------------
// Окружение и подтверждение
// ---------------------------------------------------------------------------

ApplyEnvironment makeFileEnvironment() { return ApplyEnvironment{}; }

bool askOnStream(const std::string& question, const ApplyIo& io, std::string& answer) {
    answer.clear();
    io.err << question;
    io.err.flush();
    std::string line;
    if (!std::getline(io.in, line)) {
        // Конец ввода или ошибка потока — это отказ, а не согласие: в CI поток
        // может закрыться, и «пустая строка» не должна читаться как «да».
        io.err << "\n";
        return false;
    }
    answer = trim(line);
    return true;
}

std::string requiredConfirmationToken(const core::DryRunReport& dryRun) {
    return hasRiskyOperation(dryRun) ? "DELETE-RISKY" : "DELETE";
}

// ---------------------------------------------------------------------------
// Команды
// ---------------------------------------------------------------------------

PlanExit runPlan(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env,
                 ApplyReport& out) {
    std::string error;
    const std::optional<ApplyOptions> parsed = parseApplyOptions(args, error);
    if (!parsed.has_value()) {
        io.err << "MrProper plan: " << error << "\n";
        return PlanExit::Usage;
    }
    ApplyOptions options = *parsed;
    if (options.help) {
        io.out << planUsage();
        return PlanExit::Ok;
    }
    if (options.execute) {
        // Не ошибка: пользователь явно спросил, и ответ должен быть честным —
        // команда plan не удаляет ничего (SPEC §7.2).
        options.execute = false;
        io.err << "MrProper: plan --execute не удаляет ничего; для очистки используйте apply --execute\n";
    }

    const PlanExit prepared = prepareRun(options, io, env, out);
    if (prepared != PlanExit::Ok) return prepared;

    if (options.json) {
        io.out << core::planToJson(out.candidates, out.plan) << "\n";
        return PlanExit::Ok;
    }
    printPlanText(options, io, out, /*beforeExecute=*/false);
    return PlanExit::Ok;
}

PlanExit runApply(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env,
                  ApplyReport& out) {
    std::string error;
    const std::optional<ApplyOptions> parsed = parseApplyOptions(args, error);
    if (!parsed.has_value()) {
        io.err << "MrProper apply: " << error << "\n";
        return PlanExit::Usage;
    }
    const ApplyOptions options = *parsed;
    if (options.help) {
        io.out << applyUsage();
        return PlanExit::Ok;
    }

    PlanExit code = prepareRun(options, io, env, out);
    if (code == PlanExit::Ok) {
        // Список операций печатается ровно один раз на запуск: без --execute
        // это он и есть весь вывод команды, а с --execute его печатает
        // executePlan — вместе со снимком состояния и до вопроса (FR-5).
        // Ветка --json звала printPlanText ещё и при --execute, и список
        // уходил в stderr дважды подряд.
        if (!options.execute) {
            // В машинном режиме текст плана уходит в err, stdout занят JSON
            // (то же требование, что у `scan --json`).
            printPlanText(options, io, out, /*beforeExecute=*/false);
        }
        if (options.execute) {
            code = executePlan(options, io, env, out);
        } else {
            out.refusal = "сухой прогон: --execute не задан, удалено 0 байт";
            core::logInfo("apply.dryrun", "сухой прогон без выполнения",
                          core::LogFields{core::logField("candidates", out.plan.totals.candidateCount),
                                          core::logField("selected", out.plan.totals.selectedCount),
                                          core::logField("bytes", out.plan.totals.selectedBytes)});
        }
    }

    if (options.json) io.out << applyJson(out.executed, out.confirmed, out.refusal, out);
    return code;
}

int runPlanCommand(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env) {
    ApplyReport report;
    return static_cast<int>(runPlan(args, io, env, report));
}

int runApplyCommand(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env) {
    ApplyReport report;
    return static_cast<int>(runApply(args, io, env, report));
}

}  // namespace mrproper::cli
