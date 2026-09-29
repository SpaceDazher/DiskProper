// mrproper-cli: команды `plan` и `apply` — реализация (SPEC §4 FR-5).
//
// Контракт и разбор решений описаны в cmd_apply.hpp; здесь — код. Порядок
// функций повторяет порядок жизни команды:
//
//   разбор аргументов → источник кандидатов (живой скан или файл) → фильтр по
//   категориям → core::buildPlan → core::validatePlan → показ (dry-run) →
//   [подтверждение] → core::DryRunGate → выполнение → итог.
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
//
// Подключение движка (makeFileEnvironment в конце файла) — единственное
// место, где слой cli берёт engine::ScanCoordinator и engine::CleanupExecutor:
// разбор аргументов, построение плана и показ остаются переносимыми и
// проверяются без диска (§11.1).
#include "cmd_apply.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "args.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/rules.hpp"
#include "core/rulesync.hpp"
#include "core/units.hpp"
#include "engine/candidate_collector.hpp"
#include "engine/executor.hpp"
#include "engine/file_system_probe.hpp"
#include "engine/scan_coordinator.hpp"
#include "engine/scoring_bridge.hpp"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

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

// ---------------------------------------------------------------------------
// Манифест разрешённого к удалению (docs/review-02.md F-01)
// ---------------------------------------------------------------------------
//
// Список того, что правило оставило кандидату, — половина отбора: без него
// buildPlan справедливо не выбирает ничего, и утилита не чистит вовсе (план с
// нулем операций и skipReason=needs-enumeration у каждого). Отсюда два пути:
//
//   1. Манифест пришёл в файле (его пишет core::report_json в секцию кандидата
//      того же скана). Он и есть список: перечисление не нужно, ничего не
//      измеряется, ничего не додумывается.
//   2. Манифеста в файле нет (рукописный дамп, старый отчёт, чужой генератор).
//      Тогда список восстанавливает сам CLI: он перечисляет корень кандидата и
//      берёт этот перечень ТОЛЬКО если он совпадает с тем, что кандидат
//      объявляет (число файлов). Совпадение означает, что правило ничего не
//      оставило внутри, и весь перечень — разрешённое. Расхождение означает
//      либо устаревший файл, либо то, что правило что-то отсекло, — и тогда
//      список неизвестен: элемент остаётся в плане как «не трогаем» с причиной
//      (needs-enumeration), а не с выдуманным перечнем.
//
// Оговорка о честности: перечисление измеряет логические размеры, а модель
// ждёт аллоцированные (с округлением на кластер). Поэтому при восстановлении
// манифеста кандидату проставляются измеренные числа, а в его reasons добавляется
// строка о том, что объём пересчитан CLI. Обещание от этого получается меньше
// фактического освобождения, а не больше: недобор безопаснее, чем перебор.

constexpr std::size_t kMaxEnumeratedPaths = 200000;  // столько же, сколько держит сборщик
constexpr std::size_t kMaxEnumerationDepth = 64;     // junction-петля (FR-6) не должна увести обход

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

struct RootEnumeration {
    std::vector<core::AllowedEntry> entries;
    bool complete{true};
    std::string note;  // причина, если перечисление не вышло
};

// Перечисление корня кандидата: все обычные файлы, ссылки не раскрываются
// (FR-6), глубина и число элементов ограничены.
RootEnumeration enumerateRoot(std::string_view rootUtf8) {
    RootEnumeration result;
    const std::filesystem::path root = pathFromUtf8(rootUtf8);
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(root, ec);
    if (ec || !std::filesystem::exists(status)) {
        result.complete = false;
        result.note = "корень кандидата не открывается: " + std::string(rootUtf8);
        return result;
    }
    if (!std::filesystem::is_directory(status)) {
        // Кандидат-файл: правило «MEMORY.DMP» и подобные. Список из одного пути.
        std::error_code sizeEc;
        const std::uintmax_t size = std::filesystem::file_size(root, sizeEc);
        result.entries.push_back(core::AllowedEntry{pathToUtf8(root), static_cast<std::uint64_t>(size)});
        return result;
    }

    std::vector<std::pair<std::filesystem::path, std::size_t>> stack;
    stack.emplace_back(root, 0);
    while (!stack.empty()) {
        const auto [directory, depth] = stack.back();
        stack.pop_back();
        std::error_code itEc;
        std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::none, itEc);
        if (itEc) {
            result.complete = false;
            result.note = "каталог не перечислен: " + pathToUtf8(directory);
            return result;
        }
        const std::filesystem::directory_iterator end;
        while (it != end) {
            // Всё читается до increment: после шага итератор указывает уже на
            // следующий элемент, и обращение к нему читало бы чужой путь.
            const std::filesystem::directory_entry& entry = *it;
            const std::filesystem::path entryPath = entry.path();
            std::error_code kindEc;
            const bool isLink = entry.is_symlink(kindEc);
            const bool isDir = !isLink && entry.is_directory(kindEc);
            const bool isFile = !isLink && entry.is_regular_file(kindEc);
            std::uint64_t size = 0;
            if (isFile) {
                std::error_code sizeEc;
                size = static_cast<std::uint64_t>(entry.file_size(sizeEc));
            }
            std::error_code stepEc;
            it.increment(stepEc);
            if (stepEc) {
                result.complete = false;
                result.note = "перечисление прервано: " + pathToUtf8(directory);
                return result;
            }
            if (isLink) continue;  // FR-6: ссылку не раскрываем
            if (isFile) {
                if (result.entries.size() >= kMaxEnumeratedPaths) {
                    result.complete = false;
                    result.note = "в корне больше " + std::to_string(kMaxEnumeratedPaths) + " файлов: перечень неполон";
                    return result;
                }
                // Размер, который прочитать нельзя (нет прав на атрибуты), — это
                // ноль, а не выдуманное число: кандидату тогда обещается меньше,
                // и в reasons попадает строка о неполном объёме.
                result.entries.push_back(core::AllowedEntry{pathToUtf8(entryPath), size});
            } else if (isDir) {
                if (depth + 1 >= kMaxEnumerationDepth) {
                    result.complete = false;
                    result.note = "глубина вложенности больше " + std::to_string(kMaxEnumerationDepth) +
                                  ": перечень неполон (junction-петля?)";
                    return result;
                }
                stack.emplace_back(entryPath, depth + 1);
            }
        }
    }
    return result;
}

// Манифест из секции кандидата (core::report_json пишет ключ «manifest»).
// Отсутствие ключа — не ошибка: файл мог быть собран без него. Возвращает
// false и пустой out, если манифеста в элементе нет.
bool parseManifest(const mrproper::json::Value& item, std::size_t candidateIndex, core::CandidateManifest& out,
                   std::string& error) {
    const mrproper::json::Value* node = item.find("manifest");
    if (node == nullptr) return false;
    if (!node->isObject()) {
        error = "ключ \"manifest\" не объект";
        return false;
    }
    core::CandidateManifest manifest;
    manifest.candidateIndex = candidateIndex;
    manifest.ruleId = readString(*node, "ruleId");
    manifest.rootPath = readString(*node, "rootPath");
    manifest.rootDeleteAllowed = readBool(*node, "rootDelete");
    manifest.estimateOnly = readBool(*node, "estimateOnly");
    manifest.userData = readBool(*node, "userData");
    if (const std::optional<std::uint64_t> value = readUint(*node, "minFileBytes")) {
        manifest.minFileBytes = *value;
    }
    if (manifest.rootPath.empty()) manifest.rootPath = readString(item, "path");

    if (!manifest.rootDeleteAllowed) {
        std::vector<core::AllowedEntry> entries;
        const mrproper::json::Value* paths = node->find("allowedPaths");
        if (paths != nullptr && paths->isArray()) {
            entries.reserve(paths->items().size());
            for (const mrproper::json::Value& entry : paths->items()) {
                std::string path;
                std::uint64_t bytes = 0;
                if (entry.isString()) {
                    path = entry.asString();  // список без размеров: сумма не сойдётся с кандидатом
                } else if (entry.isObject()) {
                    path = readString(entry, "path");
                    if (const std::optional<std::uint64_t> value = readUint(entry, "bytes")) bytes = *value;
                }
                if (trim(path).empty()) {
                    error = "в списке разрешённого путь пуст";
                    return false;
                }
                entries.push_back(core::AllowedEntry{path, bytes});
            }
        }
        // complete=false означает «список неполон», а CandidateManifest::deletable
        // на таком списке возвращает false: элемент останется неудаляемым, и
        // это правильный исход для обрезанного отчёта.
        const bool complete = readBool(*node, "allowedComplete");
        const std::size_t omitted = readUint(*node, "allowedOmitted").value_or(0);
        manifest.allowed = core::CandidateManifest::makeAllowedSet(std::move(entries), complete, omitted);
    }
    out = std::move(manifest);
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
                                               {"status", Value(result.status)},
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
            {"transactionId", Value(summary.transactionId)},
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

// Восстановление манифеста перечислением корня (docs/review-02.md F-01).
//
// Возвращает true, если манифест построен и кандидату можно доверять измеренные
// числа; false — если перечень неизвестен, и тогда элемент остаётся в плане без
// операции с причиной needs-enumeration. Причина пишется в note: по ней видно,
// чего именно не хватило — «файл устарел» и «правило оставило часть файлов»
// требуют от человека разных действий.
bool synthesizeManifest(core::CleanupCandidate& candidate, core::CandidateManifest& out, std::string& note) {
    if (candidate.path.empty()) return false;
    const RootEnumeration enumeration = enumerateRoot(candidate.path);
    if (!enumeration.complete) {
        note = candidate.displayName + ": " + enumeration.note;
        return false;
    }
    if (enumeration.entries.empty()) {
        note = candidate.displayName + ": в корне нет файлов — удалять нечего";
        return false;
    }
    // Сколько файлов кандидат объявляет. Ноль означает «счётчик не заполнен», и
    // тогда сверять не с чем: доверяем только тому, что перечисление закончилось.
    if (candidate.fileCount != 0 &&
        static_cast<std::uint64_t>(candidate.fileCount) != enumeration.entries.size()) {
        note = candidate.displayName + ": в файле нет манифеста, а в корне " +
               std::to_string(enumeration.entries.size()) + " файлов при " + std::to_string(candidate.fileCount) +
               " в кандидате — правило что-то оставило, какой перечень удалять, из файла не известно; "
               "нужен отчёт скана вместе с манифестом или повторный скан";
        return false;
    }

    std::uint64_t bytes = 0;
    for (const core::AllowedEntry& entry : enumeration.entries) bytes += entry.allocatedBytes;

    core::CandidateManifest manifest;
    manifest.candidateIndex = 0;  // индекс проставляет вызывающий
    manifest.ruleId = candidate.ruleId;
    manifest.rootPath = candidate.path;
    manifest.userData = core::isUserDataCategory(candidate.category);
    // Оценка «только оценка» из файла не восстанавливается: такого поля в дампе
    // нет. Правило, объявленное оценкой, таким способом к удалению не придёт
    // (нужен настоящий скан), а не наоборот.
    manifest.allowed = core::CandidateManifest::makeAllowedSet(enumeration.entries);

    // Объём кандидата приводится к измеренному: обещание «освободится N» должно
    // опираться на то, что лежит на диске сейчас, а логический размер и
    // аллоцированный (с округлением на кластер) — не одно и то же число.
    std::string noteText = "объём пересчитан CLI по файлам на диске";
    if (candidate.allocatedBytes != bytes) {
        noteText += " (в файле было " + std::to_string(candidate.allocatedBytes) + " байт, измерено " +
                    std::to_string(bytes) + ")";
    }
    candidate.logicalBytes = bytes;
    candidate.allocatedBytes = bytes;
    candidate.fileCount = static_cast<std::uint32_t>(enumeration.entries.size());
    candidate.reasons.push_back(noteText + ": план обещает меньше, чем освободится по-настоящему");

    out = std::move(manifest);
    return true;
}

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
    std::vector<core::CandidateManifest> manifests;
    std::shared_ptr<const core::RuleSet> rules;
    if (options.candidatesPath.has_value()) {
        std::string error;
        if (!loadCandidatesFile(*options.candidatesPath, candidates, manifests, error)) {
            io.err << "MrProper: " << error << "\n";
            core::logError("plan.candidates", "не прочитан список кандидатов",
                           core::LogFields{core::logField("file", *options.candidatesPath),
                                           core::logField("error", error)});
            return PlanExit::NoCandidates;
        }
    } else if (!options.rulesPath.empty() && env.liveScan) {
        // Живой скан — единственный источник, из которого можно удалять (FR-5:
        // план это снимок состояния, а не разрешение навсегда). Правила нужны и
        // исполнителю: из локатора он выводит корень, а корень — это граница
        // проверки перед удалением.
        LiveScan scan;
        std::string error;
        if (!env.liveScan(options.rulesPath, options.categories, io.err, scan, error)) {
            io.err << "MrProper: скан не дал кандидатов" << (error.empty() ? "" : ": " + error) << "\n";
            core::logError("plan.scan", "живой скан не дал кандидатов",
                           core::LogFields{core::logField("rules", options.rulesPath),
                                           core::logField("error", error)});
            return PlanExit::NoCandidates;
        }
        candidates = std::move(scan.candidates);
        manifests = std::move(scan.manifests);
        rules = std::move(scan.rules);
        io.err << "MrProper: живой скан: кандидатов " << core::formatCount(candidates.size()) << ", манифестов "
               << core::formatCount(manifests.size()) << "\n";
    } else if (env.candidates) {
        std::string error;
        if (!env.candidates(candidates, error)) {
            io.err << "MrProper: скан не дал кандидатов" << (error.empty() ? "" : ": " + error) << "\n";
            core::logError("plan.scan", "скан не дал кандидатов", core::LogFields{core::logField("error", error)});
            return PlanExit::NoCandidates;
        }
    } else {
        io.err << "MrProper: не задано, откуда взять кандидатов: укажите --rules <каталог> (живой скан), "
                  "--candidates <файл> (план без удаления) или подключите скан "
                  "(ApplyEnvironment::candidates)\n";
        return PlanExit::NoCandidates;
    }

    if (!options.categories.empty()) {
        const std::size_t before = candidates.size();
        // Фильтр меняет индексы, а манифест привязан к индексу кандидата:
        // без переноса список разрешённого достался бы не тому элементу, и
        // plan --category удалял бы не то (F-01).
        const std::vector<core::CleanupCandidate> all = candidates;
        const std::vector<core::CandidateManifest> allManifests = manifests;
        candidates = filterByCategory(candidates, options.categories);
        // Соответствие «было → стало» строим по правилу (путь кандидата), а не
        // по номеру: после фильтра индексы сдвинуты, а путь остаётся тем же.
        std::vector<core::CandidateManifest> filtered;
        for (std::size_t newIndex = 0; newIndex < candidates.size(); ++newIndex) {
            for (const core::CandidateManifest& manifest : allManifests) {
                if (manifest.candidateIndex >= all.size()) continue;
                if (all[manifest.candidateIndex].path != candidates[newIndex].path) continue;
                core::CandidateManifest moved = manifest;
                moved.candidateIndex = newIndex;
                filtered.push_back(std::move(moved));
                break;
            }
        }
        manifests = std::move(filtered);
        if (candidates.size() != before) {
            io.err << "MrProper: фильтр по категориям оставил " << core::formatCount(candidates.size()) << " из "
                   << core::formatCount(before) << "\n";
        }
    }

    core::PlanOptions planOptions = options.plan;
    // Review по умолчанию не берётся (SPEC §4 FR-3, FR-4; docs/review-02.md
    // F-03): пароли браузера, prefetch и доставка не должны попадать в план
    // сами. Включается либо профилем «выбрать всё», либо явным ключом — в CLI
    // это то же самое, что галочка на экране «Очистка».
    if (options.includeReview && planOptions.maxDefaultSafety < core::SafetyLevel::Review) {
        planOptions.maxDefaultSafety = core::SafetyLevel::Review;
    }
    // FR-5: dry-run обязателен и включается по умолчанию. Флаг --execute — это
    // единственный способ его выключить, и он ничего не добавляет к «сначала
    // показать»: список операций печатается в обоих случаях.
    planOptions.dryRun = !options.execute;
    out.plan = core::buildPlan(candidates, planOptions, manifests.empty() ? nullptr : &manifests);

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
    out.manifests = std::move(manifests);
    out.rules = std::move(rules);
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

// Основной путь CLI: целиком через исполнитель движка. Объявлено здесь, потому
// что executePlan вызывает его раньше, чем оно определено ниже.
PlanExit executeWithEngine(const ApplyOptions& options, const ApplyIo& io, const ApplyEnvironment& env,
                           ApplyReport& out);

void printOperationResult(std::ostream& out, std::size_t number, const core::PlanOperation& op,
                          const OperationResult& result) {
    // Статус операции, а не «ок/ошибка»: пропуск по политике — это не ошибка,
    // но для человека это и не «ок», и называть его «ошибкой» значило бы
    // соврать о том, что стало с файлом (§12 — правда в отчёте).
    const std::string verdict = !result.status.empty() ? result.status : (result.ok ? "ок" : "ошибка");
    out << "  [" << number << "] " << verdict << " · " << actionName(op.action) << " · "
        << core::formatBytes(op.bytes) << " · " << op.path << "\n";
    if (!result.detail.empty()) {
        if (!result.ok || result.status == "AlreadyGone") out << "        причина: " << result.detail << "\n";
    } else if (!result.ok) {
        out << "        причина: исполнитель не вернул причину\n";
    }
}

PlanExit executePlan(const ApplyOptions& options, const ApplyIo& io, const ApplyEnvironment& env, ApplyReport& out) {
    // 1. Список операций и снимок состояния показываются ДО вопроса и ДО
    //    удаления: FR-5 требует, чтобы человек видел, что именно исчезнет.
    printPlanText(options, io, out, /*beforeExecute=*/true);
    reportSnapshot(out.snapshot, out.dryRun, io.err);

    // 2. Список из файла удалять нельзя: между сканом и очисткой он мог
    //    устареть (FR-5 — план это снимок, а не разрешение навсегда).
    //    Проверка стоит ПЕРЕД ранним возвратом по пустому плану: иначе
    //    `apply --candidates f.json --execute` на плане, из которого ничего не
    //    выбирается, отвечал бы «0, удалять нечего» — то есть молча соглашался
    //    удалять по файлу, который запрещён (D-49). Запрет не зависит от того,
    //    нашлись операции или нет.
    if (options.candidatesPath.has_value()) {
        out.refusal = "--candidates читает готовый список: удалять по нему нельзя, план мог устареть";
        io.err << "MrProper: " << out.refusal
               << "\n  Удаление доступно только по кандидатам живого скана.\n";
        core::logWarn("apply.refused", "--candidates вместе с --execute запрещено",
                      core::LogFields{core::logField("file", *options.candidatesPath),
                                      core::logField("signature", out.snapshot.planSignature)});
        return PlanExit::Refused;
    }

    // 3. Пустой план — не отказ, а «удалять нечего»: команды прошли, элементов
    //    под профиль не нашлось. Ранний возврат стоит после запрета выше, иначе
    //    пустой список из файла проходил бы как успешное выполнение.
    if (out.dryRun.operations.empty()) {
        out.refusal = "нечего выполнять: в плане 0 операций";
        core::logInfo("apply.empty", "в плане нет операций — выполнение не потребовалось",
                      core::LogFields{core::logField("signature", out.snapshot.planSignature)});
        return PlanExit::Ok;
    }

    // 4. Без исполнителя операций --execute не выполняется. Молча вывести
    //    «удалено 0 байт» здесь означало бы соврать о результате. Исполнителей
    //    два вида: целиком (executePlan, engine::CleanupExecutor) и
    //    пооперационный (executeOperation, для вызывающих со своим движком).
    if (!env.executePlan && !env.executeOperation) {
        out.refusal = "исполнитель операций не подключён (engine::CleanupExecutor ещё не собран)";
        io.err << "MrProper: " << out.refusal << "\n";
        core::logError("apply.noexecutor", "запрошено выполнение без исполнителя операций",
                       core::LogFields{core::logField("signature", out.snapshot.planSignature)});
        return PlanExit::NoExecutor;
    }

    // 5. Подтверждение. Сессия CLI — один запуск процесса, поэтому подтверждение
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

    // 6. Ядро проверяет, что показан и подтверждён именно этот план: сменились
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

    // 7. Выполнение. Основной путь CLI — целиком через исполнитель движка:
    //    фазы проверок (корень правила, белый список защищённых каталогов,
    //    пропуск reparse, снимок состояния, Restart Manager), работа с
    //    манифестом разрешённого и перенос в корзину принадлежат
    //    engine::CleanupExecutor (FR-6, docs/review-02.md F-01), и второй их
    //    вариант в CLI рано или поздно разошёлся бы с ним.
    if (env.executePlan) return executeWithEngine(options, io, env, out);

    // 8. Пооперационный исполнитель: ошибка одной операции не отменяет
    //    остальные (FR-6).
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

// ---------------------------------------------------------------------------
// Отмена по Ctrl+C
// ---------------------------------------------------------------------------
//
// Удаление кооперативно отменяемо (FR-6), но кооперативность должна быть с
// кем-то: без обработчика консоли Ctrl+C убивает процесс посреди переноса в
// корзину, и манифест транзакции не пишется — то есть отменять будет нечего.
// Обработчик просит остановиться у того, кто сейчас работает, и всегда
// возвращает TRUE: «приложение остановит само себя» — правда, а «процесс
// исчезнет, ничего не записав» — нет. Указатели, а не ссылки: в обработчике
// консоли ничего кроме атомарной загрузки и noexcept-вызова делать нельзя.

std::atomic<engine::ScanCoordinator*> g_runningScan{nullptr};
std::atomic<engine::CleanupExecutor*> g_runningExecutor{nullptr};

void requestRunningWorkToStop() noexcept {
    if (engine::ScanCoordinator* scan = g_runningScan.load(std::memory_order_acquire)) scan->requestStop();
    if (engine::CleanupExecutor* executor = g_runningExecutor.load(std::memory_order_acquire)) executor->requestStop();
}

#if defined(_WIN32)
BOOL WINAPI consoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            requestRunningWorkToStop();
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

// Один раз на процесс: повторная регистрация того же обработчика ничего не
// меняет, но и не нужна.
void installConsoleStopHandler() {
    static const bool installed = [] {
        ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
        return true;
    }();
    (void)installed;
}
#else
void installConsoleStopHandler() {}
#endif

// ---------------------------------------------------------------------------
// Исполнение через engine::CleanupExecutor
// ---------------------------------------------------------------------------

// Статус строки для человека и для JSON. Язык движка (done, skipped-busy…)
// годится в журнал, но в отчёте apply он обязан читаться как действие:
// «Deleted» или «AlreadyGone» отвечают на вопрос «что стало с этим путём»
// прямо, а «done» — только на вопрос «была ли ошибка».
std::string statusOfItem(engine::ItemOutcome outcome, core::PlanAction action) {
    switch (outcome) {
        case engine::ItemOutcome::Done:
            return action == core::PlanAction::Trash ? "Trashed" : "Deleted";
        case engine::ItemOutcome::AlreadyGone:
            return "AlreadyGone";
        case engine::ItemOutcome::SkippedBusy:
            return "Locked";
        case engine::ItemOutcome::SkippedLockUnknown:
            return "LockUnknown";
        case engine::ItemOutcome::SkippedProtected:
            return "Protected";
        case engine::ItemOutcome::SkippedOutsideRoot:
            return "OutsideRoot";
        case engine::ItemOutcome::SkippedReparse:
            return "Reparse";
        case engine::ItemOutcome::SkippedChanged:
            return "Changed";
        case engine::ItemOutcome::SkippedInvalid:
            return "InvalidPath";
        case engine::ItemOutcome::SkippedNoManifest:
            return "NoManifest";
        case engine::ItemOutcome::NotStarted:
            return "NotStarted";
        case engine::ItemOutcome::NotSelected:
            return "NotSelected";
        case engine::ItemOutcome::Cancelled:
            return "Cancelled";
        case engine::ItemOutcome::Failed:
            break;
    }
    return "Failed";
}

// Адаптер исполнителя: engine::CleanupExecutor поверх контракта PlanExecutor.
//
// Набор правил обязателен не «для полноты»: buildChecklist выводит из него
// корень правила, а фаза проверки требует границу (FR-6) — без корня каждая
// операция честно закончилась бы SkippedOutsideRoot, то есть удаление было бы
// невозможно. Пустой набор — это не тихий обход проверки, а отказ с кодом 5.
bool executePlanWithEngine(const PlanExecutionInput& input, PlanExecution& out) {
    if (input.candidates == nullptr || input.plan == nullptr) {
        out.abortReason = "исполнителю не переданы кандидаты или план";
        return false;
    }
    installConsoleStopHandler();
    const std::vector<core::CleanupCandidate>& candidates = *input.candidates;

    engine::CleanupExecutorOptions options;
    options.trashRoot = input.trashRoot;
    options.appVersion = input.appVersion;
    // Все проверки FR-6 включены: без них удаление идёт вслепую. checkStamps
    // ловит «файл изменился после сканирования» (§10), checkLocks — занятый
    // путь через Restart Manager, корень правила, белый список защищённых
    // каталогов и признак reparse проверяются фазой A всегда.
    options.checkStamps = true;
    options.checkLocks = true;
    // Закрывать чужие приложения из CLI нечем: спрашивать некого, а FR-6
    // запрещает закрывать без подтверждения. Занятый путь становится
    // Skip (locked) и попадает в отчёт с именем держателя — ровно то действие,
    // которое описано в плане (PlanAction::SkipLocked).
    options.allowCloseProcesses = false;

    if (input.progress != nullptr) {
        options.progressInterval = std::chrono::milliseconds(500);
        options.onProgress = [stream = input.progress](const engine::ProgressSnapshot& snapshot) {
            *stream << "MrProper: очистка: операций " << snapshot.itemsDone << ", освобождено "
                    << core::formatBytes(snapshot.bytesFound) << ", потоков " << snapshot.workersActive << '/'
                    << snapshot.workersTotal << "\n";
        };
    }

    engine::ChecklistOptions checklistOptions;
    checklistOptions.rules = input.rules;
    const engine::Checklist checklist = engine::buildChecklist(candidates, *input.plan, checklistOptions);
    if (checklist.executable() == 0) {
        out.abortReason = "чек-лист пуст: план не выбрал ни одной операции Delete или Trash";
        return false;
    }

    engine::CleanupExecutor executor(options);
    g_runningExecutor.store(&executor, std::memory_order_release);
    const engine::ExecutionRefusal refusal = executor.run(candidates, *input.plan, checklist);
    g_runningExecutor.store(nullptr, std::memory_order_release);
    out.summary = executor.toText();
    if (refusal != engine::ExecutionRefusal::Completed) {
        out.abortReason = std::string("исполнитель отказал: ") + engine::toString(refusal);
        return false;
    }
    const engine::ExecutionReportPtr report = executor.result();
    if (report == nullptr) {
        out.abortReason = "исполнитель отработал, но отчёта не опубликовал";
        return false;
    }

    out.items.assign(candidates.size(), PlanExecutionItem{});
    for (const engine::ItemReport& item : report->items) {
        if (item.candidateIndex >= out.items.size()) continue;
        PlanExecutionItem& target = out.items[item.candidateIndex];
        target.ok = item.ok();
        target.status = statusOfItem(item.outcome, item.action);
        target.freedBytes = item.reclaimedBytes;
        target.transactionId = item.txId;
        target.detail = item.detail;
    }
    out.freedBytes = report->reclaimedBytes;
    out.transactionId = report->trashTxId;
    out.aborted = report->cancelled;
    if (out.aborted) out.abortReason = "прогон прерван отменой (Ctrl+C)";
    return true;
}

// Прогон целиком и разбор его отчёта в сводку команды. Коды SPEC §7.2:
// успех 0, отказ 3, отказ исполнителя 5, частичный результат — сводка в отчёте
// и код 6, а не тишина (FR-6).
PlanExit executeWithEngine(const ApplyOptions& options, const ApplyIo& io, const ApplyEnvironment& env,
                           ApplyReport& out) {
    PlanExecutionInput input;
    input.candidates = &out.candidates;
    input.manifests = &out.manifests;
    input.plan = &out.plan;
    input.rules = out.rules.get();
    input.trashRoot = options.trashRoot;
    input.appVersion = env.appVersion;
    input.progress = &io.err;

    PlanExecution execution;
    out.executed = true;
    const bool ran = env.executePlan(input, execution);

    const std::vector<core::PlanOperation>& operations = out.dryRun.operations;
    ExecutionSummary& summary = out.execution;
    summary.operations.reserve(operations.size());
    summary.results.reserve(operations.size());
    summary.transactionId = execution.transactionId;

    if (!ran) {
        out.refusal = execution.abortReason.empty() ? "исполнитель отказался выполнять план" : execution.abortReason;
        io.err << "MrProper: " << out.refusal << "\n";
        io.err << "MrProper: удалено 0 байт\n";
        core::logError("apply.executor.refused", "исполнитель отказался выполнять план",
                       core::LogFields{core::logField("refusal", out.refusal),
                                       core::logField("signature", out.snapshot.planSignature)});
        return PlanExit::NoExecutor;
    }

    const bool perOperationLines = !options.json;  // в --json итог едет в stdout
    for (const core::PlanOperation& op : operations) {
        OperationResult result;
        if (op.candidateIndex < execution.items.size()) {
            const PlanExecutionItem& item = execution.items[op.candidateIndex];
            result.ok = item.ok;
            result.status = item.status;
            result.freedBytes = item.freedBytes;
            result.transactionId = item.transactionId;
            result.detail = item.detail;
        } else {
            result.ok = false;
            result.status = "NoResult";
            result.detail = "исполнитель не вернул результат по кандидату " + std::to_string(op.candidateIndex);
        }
        if (result.status.empty()) result.status = result.ok ? "Done" : "Failed";

        summary.operations.push_back(op);
        summary.results.push_back(result);
        ++summary.attempted;
        if (result.ok) {
            ++summary.succeeded;
            summary.freedBytes += result.freedBytes;
            if (perOperationLines) printOperationResult(io.err, summary.attempted, op, result);
            continue;
        }
        // Строку, которую отмена застала в очереди, нельзя называть ошибкой
        // исполнения: объект цел, и «не успели» — другое утверждение, чем
        // «не смогли» (FR-6). Она уходит в notRun и в счётчик попадает иначе.
        if (result.status == "NotStarted" || result.status == "Cancelled") {
            summary.notRun.push_back(op);
            printOperationResult(io.err, summary.attempted, op, result);
            core::logWarn("apply.notrun", "операция не начата: прогон прерван отменой",
                          core::LogFields{core::logField("path", op.path),
                                          core::logField("status", result.status),
                                          core::logField("detail", result.detail)});
            continue;
        }
        ++summary.failed;
        printOperationResult(io.err, summary.attempted, op, result);
        core::logError("apply.operation", "операция не выполнена",
                       core::LogFields{core::logField("path", op.path),
                                       core::logField("action", std::string(actionName(op.action))),
                                       core::logField("bytes", op.bytes),
                                       core::logField("status", result.status),
                                       core::logField("detail", result.detail),
                                       core::logField("transactionId", result.transactionId)});
    }
    summary.aborted = execution.aborted;
    summary.abortReason = execution.abortReason;

    if (!execution.summary.empty()) io.err << "MrProper: " << execution.summary << "\n";
    io.err << "MrProper: выполнено " << summary.succeeded << " из " << summary.attempted << ", освобождено "
           << core::formatBytes(summary.freedBytes) << " (по аллоцированному размеру), ошибок: " << summary.failed;
    if (!summary.notRun.empty()) io.err << ", не начато: " << summary.notRun.size();
    if (!summary.transactionId.empty()) {
        io.err << ", транзакция корзины " << summary.transactionId << " (FR-7, отменяемо)";
    }
    io.err << "\n";
    core::logInfo("apply.done", "план выполнен движком",
                  core::LogFields{core::logField("attempted", summary.attempted),
                                  core::logField("succeeded", summary.succeeded),
                                  core::logField("failed", summary.failed),
                                  core::logField("freedBytes", summary.freedBytes),
                                  core::logField("transactionId", summary.transactionId)});
    return summary.failed == 0 ? PlanExit::Ok : PlanExit::Failed;
}

// ---------------------------------------------------------------------------
// Живой скан по --rules: правила из каталога и обход ФС
// ---------------------------------------------------------------------------

// Переменные окружения, которые подставляются в локаторы правил: тот же
// закрытый список, что у команды scan (cmd_scan.cpp, kRuleEnvironmentNames), и по
// той же причине — в локатор попадает только то, что правилам действительно
// нужно, а весь дамп окружения не должен печататься ни в отчёт, ни в журнал
// (SPEC §5 «Локализация», docs/rules-authoring.md §4). Список продублирован
// намеренно: этот мост не должен зависеть от внутренних деталей команды scan;
// когда у scan появится публичный доступ к дампу окружения, функция удаляется.
constexpr std::string_view kRuleEnvironmentNames[] = {
    "ALLUSERSPROFILE", "APPDATA",       "LOCALAPPDATA", "ProgramData",  "ProgramFiles",
    "ProgramFiles(x86)", "ProgramW6432", "PUBLIC",       "SystemDrive",  "SystemRoot",
    "TEMP",           "TMP",            "USERPROFILE",  "windir",
};

// Чтение переменной окружения в UTF-8. На Windows — GetEnvironmentVariableW, а
// не std::getenv: узкие функции отдают путь в кодовой странице консоли, и
// «%TEMP%» с кириллицей приехал бы в локатор правила мусором (§5 «Пути»).
std::optional<std::string> readEnvironmentValue(std::string_view name) {
#if defined(_WIN32)
    const std::wstring wideName(name.begin(), name.end());
    std::wstring buffer(1024, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD written = ::GetEnvironmentVariableW(wideName.c_str(), buffer.data(),
                                                        static_cast<DWORD>(buffer.size()));
        if (written == 0) return std::nullopt;
        if (written < buffer.size()) {
            buffer.resize(written);
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), written, nullptr, 0, nullptr, nullptr);
            if (size <= 0) return std::nullopt;
            std::string value(static_cast<std::size_t>(size), '\0');
            if (::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), written, value.data(), size, nullptr, nullptr) <= 0) {
                return std::nullopt;
            }
            return value;
        }
        buffer.resize(written);
    }
    return std::nullopt;
#else
    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
#endif
}

// Дамп «NAME=value» по строке на переменную — тот формат, который ждёт
// core::loadRuleFiles. Переменных, которых нет, в дампе нет вовсе:
// core::expandEnvironment помечает %НЕИЗВЕСТНО% как неразрешённое, и такой
// локатор отбрасывается целиком, а это честнее, чем подставить пустой путь.
std::string processEnvironmentDump() {
    std::string dump;
    for (const std::string_view name : kRuleEnvironmentNames) {
        const std::optional<std::string> value = readEnvironmentValue(name);
        if (!value.has_value()) continue;
        dump.append(name);
        dump.push_back('=');
        dump.append(*value);
        dump.push_back('\n');
    }
    return dump;
}

// Набор правил из каталога. Формат каталога — тот же, что у команды scan:
// *.json, плюс manifest.json рядом (он опознаётся попыткой разбора, а не
// именем файла, потому что форма у него другая). Порядок чтения фиксирован
// сортировкой: иначе первая ошибка валидации называла бы другой файл в
// зависимости от порядка каталога на диске.
bool loadRuleSetFromDirectory(const std::string& directory, const std::string& environment, core::RuleSet& out,
                              std::string& rulesVersion, std::string& error) {
    namespace fs = std::filesystem;

    std::error_code code;
    const fs::path root(directory);
    if (!fs::is_directory(root, code)) {
        error = "каталог набора правил не найден: " + directory;
        return false;
    }

    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, code)) {
        if (entry.is_regular_file(code) && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        error = "в каталоге " + directory + " нет ни одного файла *.json";
        return false;
    }

    std::vector<std::pair<std::string, std::string>> texts;
    texts.reserve(files.size());
    for (const fs::path& path : files) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "файл правил не читается: " + path.string();
            return false;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string text = buffer.str();
        const std::string name = path.filename().string();
        try {
            rulesVersion = core::parseRuleSetManifest(text, name).version;
            continue;  // манифест набора, а не правило
        } catch (const core::RuleSyncError&) {
            // Не манифест — идёт в набор правил, разберётся загрузчик.
        }
        texts.emplace_back(name, text);
    }
    if (texts.empty()) {
        error = "в каталоге " + directory + " нет ни одного файла правил (только manifest.json)";
        return false;
    }

    try {
        out = core::loadRuleFiles(texts, environment);
        core::validateRuleSet(out);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
    if (rulesVersion.empty()) rulesVersion = out.version;
    return true;
}

// Куда складывают задачи пула. Кандидаты и манифесты пишут несколько потоков, а
// читает один главный поток после конца прогона, поэтому под замком.
class LiveScanSink {
public:
    void add(std::vector<core::CleanupCandidate> candidates, std::vector<core::CandidateManifest> manifests) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t count = candidates.size();
        const std::size_t base = candidates_.size();
        for (core::CleanupCandidate& candidate : candidates) candidates_.push_back(std::move(candidate));
        for (core::CandidateManifest& manifest : manifests) {
            // Манифесты приходят с индексами своей задачи (сборщик нумерует
            // кандидатов внутри результата одной категории), а в плане индекс —
            // это позиция в общем списке. Сдвиг на базу обязателен: без него
            // манифест достался бы чужому кандидату, и план удалил бы не то
            // (docs/review-02.md F-01).
            if (manifest.candidateIndex >= count) continue;
            manifest.candidateIndex += base;
            manifests_.push_back(std::move(manifest));
        }
        ++completed_;
    }

    [[nodiscard]] std::size_t completed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_;
    }

    [[nodiscard]] std::vector<core::CleanupCandidate> candidates() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return candidates_;
    }

    [[nodiscard]] std::vector<core::CandidateManifest> manifests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return manifests_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<core::CleanupCandidate> candidates_;
    std::vector<core::CandidateManifest> manifests_;
    std::size_t completed_{};
};

// Живой скан: правила из --rules, обход ФС и оценка кандидатов — тем же кодом
// движка, что и команда scan. Кандидаты здесь НЕ сортируются: у манифеста
// индекс кандидата, и перестановка списка без переноса манифестов отдала бы
// план не тем путям (docs/review-02.md F-01). Порядок и так детерминирован:
// одна задача на категорию, а внутри категории кандидаты идут в порядке
// правил.
bool liveScanWithEngine(const std::string& rulesPath, const std::vector<std::string>& categories, std::ostream& human,
                        LiveScan& out, std::string& error) {
    installConsoleStopHandler();

    core::RuleSet rules;
    std::string rulesVersion;
    if (!loadRuleSetFromDirectory(rulesPath, processEnvironmentDump(), rules, rulesVersion, error)) return false;

    if (!categories.empty()) {
        std::vector<core::Rule> kept;
        kept.reserve(rules.rules.size());
        for (const core::Rule& rule : rules.rules) {
            const std::string wanted = toLowerAscii(trim(rule.category));
            if (std::find(categories.begin(), categories.end(), wanted) != categories.end()) {
                kept.push_back(rule);
            }
        }
        if (kept.empty()) {
            error = "ни одно правило не относится ни к одной из указанных категорий";
            return false;
        }
        rules.rules = std::move(kept);
    }

    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<std::string> categoryOrder;
    for (const core::Rule& rule : rules.rules) {
        if (std::find(categoryOrder.begin(), categoryOrder.end(), rule.category) == categoryOrder.end()) {
            categoryOrder.push_back(rule.category);
        }
    }
    human << "MrProper: скан: набор правил " << (rulesVersion.empty() ? std::string("без версии") : rulesVersion)
          << ", правил " << rules.rules.size() << ", задач " << categoryOrder.size() << "\n";

    engine::scoring_bridge::ScoringContext scoringContext;
    scoringContext.now = now;
    // Состояние процессов не проверялось: определение блокировок в скане не
    // участвует, и мост скажет об этом строкой в причинах, а не промолчит
    // (FR-4 LockedBy остаётся пустым, и это видно).
    scoringContext.processClosedStateKnown = false;

    LiveScanSink sink;
    std::vector<engine::ScanTask> tasks;
    tasks.reserve(categoryOrder.size());
    for (const std::string& category : categoryOrder) {
        core::RuleSet subset;
        subset.schemaVersion = rules.schemaVersion;
        subset.version = rules.version;
        subset.minAppVersion = rules.minAppVersion;
        for (const core::Rule& rule : rules.rules) {
            if (rule.category == category) subset.rules.push_back(rule);
        }
        engine::CollectOptions collectOptions;
        collectOptions.now = now;
        const engine::scoring_bridge::RuleLookup lookup = engine::scoring_bridge::makeRuleLookup(subset);

        tasks.push_back(engine::ScanTask{
            category,
            category,
            0,  // число элементов заранее неизвестно: обход ФС не знает счётчика
            [subset = std::move(subset), collectOptions, scoringContext, lookup,
             &sink](engine::ScanTaskContext& context) {
                // Свой адаптер обхода на задачу: обход идёт в пуле, и требовать
                // потокобезопасности реализации нельзя.
                std::unique_ptr<engine::FileSystemProbe> probe = engine::makeVfsFileSystemProbe();
                if (!probe) return;  // адаптер не выдался: задача пуста, а не падение прогона
                engine::CollectResult collected =
                    engine::collectCandidates(subset, *probe, collectOptions, context.stopToken());
                engine::scoring_bridge::scoreCandidates(collected.candidates, lookup, scoringContext);
                std::uint64_t found = 0;
                for (const core::CleanupCandidate& candidate : collected.candidates) {
                    found += candidate.allocatedBytes;
                }
                context.addItems(collected.stats.filesSeen);
                context.addBytes(found);
                sink.add(std::move(collected.candidates), std::move(collected.manifests));
            },
        });
    }

    std::mutex progressMutex;
    std::chrono::steady_clock::time_point nextPrint{};
    engine::ScanCoordinatorOptions coordinatorOptions;
    coordinatorOptions.logProgress = false;  // прогресс идёт в stderr, а не в журнал
    coordinatorOptions.onProgress = [&human, &progressMutex, &nextPrint](const engine::ProgressSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(progressMutex);
        const auto now = std::chrono::steady_clock::now();
        if (!snapshot.finished && now < nextPrint) return;
        nextPrint = now + std::chrono::milliseconds(1000);
        human << "MrProper: скан: элементов " << snapshot.itemsDone << ", найдено "
              << core::formatBytes(snapshot.bytesFound) << ", задач " << snapshot.tasksDone << '/' << snapshot.tasksTotal
              << "\n";
    };

    engine::ScanCoordinator coordinator(coordinatorOptions);
    g_runningScan.store(&coordinator, std::memory_order_release);
    const engine::ScanRunReportPtr runReport = coordinator.run(std::move(tasks));
    g_runningScan.store(nullptr, std::memory_order_release);

    if (runReport == nullptr) {
        error = "прогон скана не дал отчёта";
        return false;
    }
    if (!runReport->tasks.empty() && runReport->completed == 0) {
        error = "ни одна задача скана не выполнена";
        return false;
    }
    if (runReport->cancelled) {
        // Отменённый скан — не повод удалять по неполному списку: план из
        // половины обхода хуже, чем никакого (FR-5).
        error = "скан прерван, список кандидатов неполон — удаление по нему запрещено";
        return false;
    }

    out.candidates = sink.candidates();
    out.manifests = sink.manifests();
    // Набор правил едет вместе с кандидатами: из него исполнитель выводит
    // корень правила, а без корня удаление невозможно (FR-6).
    out.rules = std::make_shared<const core::RuleSet>(std::move(rules));

    std::uint64_t reclaimable = 0;
    for (const core::CleanupCandidate& candidate : out.candidates) reclaimable += candidate.allocatedBytes;
    human << "MrProper: скан: кандидатов " << core::formatCount(out.candidates.size()) << ", манифестов "
          << core::formatCount(out.manifests.size()) << ", освободится " << core::formatBytes(reclaimable)
          << ", задач выполнено " << runReport->completed << " из " << runReport->tasks.size() << "\n";

    core::logInfo("apply.scan", "живой скан завершён",
                  core::LogFields{core::logField("rules", rulesPath),
                                  core::logField("rulesVersion", rulesVersion),
                                  core::logField("candidates", out.candidates.size()),
                                  core::logField("manifests", out.manifests.size()),
                                  core::logField("reclaimable", reclaimable)});
    return true;
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
        if (arg == "--include-review") {
            // Явное «да» на уровень Review: в CLI это то же, что галочка в
            // настройках экрана «Очистка» (docs/review-02.md F-03).
            options.includeReview = true;
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
        if (arg == "--rules") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            if (trim(value).empty()) {
                error = "--rules: пустой путь к каталогу правил";
                return std::nullopt;
            }
            options.rulesPath = value;
            continue;
        }
        if (arg == "--trash-root") {
            std::string value;
            if (!takeValue(value)) return std::nullopt;
            if (trim(value).empty()) {
                error = "--trash-root: пустой путь к корзине приложения";
                return std::nullopt;
            }
            options.trashRoot = value;
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
           "  --include-review                брать и уровень Review (по умолчанию только Safe, FR-3)\n"
           "  --no-trash                      всё прямым удалением, минуя корзину приложения\n"
           "  --min-reclaim <размер>          не брать кандидатов меньше размера (1024 в k/m/g)\n"
           "  --trash-direct-delete-above <размер>\n"
           "                                  выше этого объёма удаляем сразу, а не в корзину\n"
           "  --category <id>[,<id>…]        только эти категории; ключ можно повторять\n"
           "  --rules <каталог>               правила для живого скана; единственный путь к удалению\n"
           "  --trash-root <каталог>          корзина приложения (FR-7); пусто — %ProgramData%\\MrProper\\Trash\n"
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
           "  --include-review                брать и уровень Review (по умолчанию только Safe, FR-3)\n"
           "  --no-trash                      всё прямым удалением, минуя корзину приложения\n"
           "  --min-reclaim <размер>          не брать кандидатов меньше размера (1024 в k/m/g)\n"
           "  --trash-direct-delete-above <размер>\n"
           "                                  выше этого объёма удаляем сразу, а не в корзину\n"
           "  --category <id>[,<id>…]        только эти категории; ключ можно повторять\n"
           "  --rules <каталог>               правила для живого скана: без них удаление невозможно\n"
           "  --trash-root <каталог>          корзина приложения (FR-7); пусто — %ProgramData%\\MrProper\\Trash\n"
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

// Внутренний разбор с причинами восстановления манифеста: наружу они идут
// через reasons кандидата, а наружу (в stderr) — вызывающая команда.
bool parseCandidatesJsonImpl(std::string_view text, std::vector<core::CleanupCandidate>& candidates,
                             std::vector<core::CandidateManifest>& manifests, std::vector<std::string>& notes,
                             std::string& error);

std::vector<core::CleanupCandidate> parseCandidatesJson(std::string_view text, std::string& error) {
    std::vector<core::CleanupCandidate> candidates;
    std::vector<core::CandidateManifest> manifests;
    std::string parseError;
    (void)parseCandidatesJson(text, candidates, manifests, parseError);
    if (!parseError.empty()) {
        error = parseError;
        candidates.clear();
    }
    return candidates;
}

bool parseCandidatesJson(std::string_view text, std::vector<core::CleanupCandidate>& candidates,
                         std::vector<core::CandidateManifest>& manifests, std::string& error) {
    std::vector<std::string> notes;
    const bool ok = parseCandidatesJsonImpl(text, candidates, manifests, notes, error);
    if (!ok) {
        candidates.clear();
        manifests.clear();
    }
    return ok;
}

// notes — человекочитаемые причины, по которым у элементов не оказалось
// манифеста. Они же дописываются в reasons кандидата: причина обязана быть
// видна в dry-run и в JSON плана, а не только в stderr.
bool parseCandidatesJsonImpl(std::string_view text, std::vector<core::CleanupCandidate>& candidates,
                             std::vector<core::CandidateManifest>& manifests, std::vector<std::string>& notes,
                             std::string& error) {
    error.clear();
    candidates.clear();
    manifests.clear();
    notes.clear();

    std::string_view body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEFu && static_cast<unsigned char>(body[1]) == 0xBBu &&
        static_cast<unsigned char>(body[2]) == 0xBFu) {
        body.remove_prefix(3);  // BOM от Notepad и PowerShell
    }
    if (trim(std::string(body)).empty()) {
        error = "пустой документ";
        return false;
    }

    mrproper::json::Value root;
    try {
        root = mrproper::json::parse(body);
    } catch (const mrproper::json::ParseError& exception) {
        error = std::string("не разобрался JSON: ") + exception.what();
        return false;
    }

    const mrproper::json::Value* array = nullptr;
    if (root.isArray()) {
        array = &root;
    } else if (root.isObject()) {
        array = root.find("candidates");
        if (array == nullptr) {
            error = "в объекте нет раздела \"candidates\" (ожидался отчёт core::report_json или массив кандидатов)";
            return false;
        }
    } else {
        error = "ожидался объект с разделом \"candidates\" или массив кандидатов";
        return false;
    }
    if (!array->isArray()) {
        error = "раздел \"candidates\" не массив";
        return false;
    }

    candidates.reserve(array->items().size());
    for (std::size_t i = 0; i < array->items().size(); ++i) {
        core::CleanupCandidate candidate;
        std::string itemError;
        if (!parseCandidate(array->items()[i], candidate, itemError)) {
            error = "кандидат " + std::to_string(i) + ": " + itemError;
            candidates.clear();
            return false;
        }
        // Манифест — вторая половина отбора (F-01). Он либо пришёл в файле
        // (тогда он и есть список), либо его нет и список восстанавливается
        // перечислением корня; и в том и в другом случае элемент без списка
        // остаётся в плане с причиной needs-enumeration, а не выкидывается.
        core::CandidateManifest manifest;
        if (!parseManifest(array->items()[i], i, manifest, itemError)) {
            if (!itemError.empty()) {
                error = "кандидат " + std::to_string(i) + ": манифест: " + itemError;
                candidates.clear();
                return false;
            }
            std::string note;
            if (synthesizeManifest(candidate, manifest, note)) {
                manifest.candidateIndex = i;
                manifests.push_back(std::move(manifest));
            } else if (!note.empty()) {
                // Причина уходит и в stderr, и в reasons кандидата: элемент
                // останется в плане как «не трогаем», и объяснение обязано быть
                // там, где человек его читает.
                notes.push_back(note);
                candidate.reasons.push_back(note);
            }
        } else {
            manifests.push_back(std::move(manifest));
        }
        candidates.push_back(std::move(candidate));
    }
    return true;
}

bool loadCandidatesFile(const std::string& path, std::vector<core::CleanupCandidate>& out, std::string& error) {
    std::vector<core::CandidateManifest> manifests;
    return loadCandidatesFile(path, out, manifests, error);
}

bool loadCandidatesFile(const std::string& path, std::vector<core::CleanupCandidate>& out,
                        std::vector<core::CandidateManifest>& manifests, std::string& error) {
    out.clear();
    manifests.clear();
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
    std::vector<core::CleanupCandidate> candidates;
    if (!parseCandidatesJson(buffer.str(), candidates, manifests, parseError)) {
        error = path + ": " + parseError;
        candidates.clear();
        manifests.clear();
        return false;
    }
    out = std::move(candidates);
    return true;
}

// ---------------------------------------------------------------------------
// Окружение и подтверждение
// ---------------------------------------------------------------------------

ApplyEnvironment makeFileEnvironment() {
    ApplyEnvironment environment;
    // Живой скан по --rules и настоящий исполнитель engine::CleanupExecutor.
    // Подключение живёт здесь, а не в разборе аргументов и не в построении
    // плана: без него команда остаётся переносимой и проверяемой без диска
    // (§11.1), а с настоящей программой удаление работает.
    environment.liveScan = [](const std::string& rulesPath, const std::vector<std::string>& categories,
                              std::ostream& human, LiveScan& out, std::string& error) {
        return liveScanWithEngine(rulesPath, categories, human, out, error);
    };
    environment.executePlan = [](const PlanExecutionInput& input, PlanExecution& out) {
        return executePlanWithEngine(input, out);
    };
    const char* version = appVersion();
    environment.appVersion = version == nullptr ? std::string{} : std::string{version};
    // Снимок состояния (FR-5) требует версию и PID процесса: без них человек
    // перед удалением не знает, из какой сборки и из какого процесса оно
    // запущено. Время core::makeSnapshot проставит сам.
    environment.snapshotContext = []() {
        core::PlanSnapshotContext context;
        context.appVersion = appVersion() == nullptr ? std::string{} : std::string{appVersion()};
        context.createdAtUnix = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
#if defined(_WIN32)
        context.pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
#endif
        return context;
    };
    return environment;
}

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
