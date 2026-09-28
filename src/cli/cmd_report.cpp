// mrproper-cli: команда «report» — превращение отчёта в машинный вид, в JSON и
// в самодостаточный HTML (SPEC §4 FR-8 «Отчёт», §8 Этап 4, §11.4 «golden-тесты
// данных», §12 «отчёт (HTML/JSON) содержит карту, кандидатов, операции и
// ошибки; открывается без приложения»).
//
// ---------------------------------------------------------------------------
// Что делает команда
// ---------------------------------------------------------------------------
// Вход — JSON-отчёт, который напечатали «scan --json», «plan --json» или
// «apply --json» (SPEC §6.2: CLI отдаёт машину stdout, и отчёт — главный
// артефакт для баг-репорта, ADR-007). Выход — тот же отчёт в нормализованном
// JSON или самодостаточный HTML-документ:
//
//     mrproper-cli scan --json > scan.json
//     mrproper-cli report --json  --in scan.json --out scan.norm.json
//     mrproper-cli report --html  --in scan.json --out scan.html
//     mrproper-cli scan --json | mrproper-cli report --html -o scan.html
//
// Разбор отчёта сделан здесь, а не в core, сознательно: core::report_json
// только пишет (SPEC §6.1 — «модуль ничего не пишет на диск и ничего не
// печатает»), а читать отчёт обратно в модель нужно только CLI и тестам. Ядро
// при этом остаётся единственным, кто знает формат: и запись (reportToJson), и
// чтение (здесь) опираются на core::kReportJsonSchema и набор токенов, и
// расхождение между ними ломает сборку тестами, а не тихо портит отчёт.
//
// Терпимость к входу здесь одна и осознанная: неизвестные поля игнорируются
// (отчёт может прийти из более новой версии приложения), а неизвестный ТОКЕН в
// известном поле — ошибка, потому что это уже не «другая версия», а подмена
// смысла: safety="safee" не должен молча превращаться в Review.
//
// ---------------------------------------------------------------------------
// Коды возврата (общая таблица CLI; те же значения в cmd_rules.cpp)
// ---------------------------------------------------------------------------
//     0   успех;
//    64   неверные аргументы (EX_USAGE);
//    65   данные не годятся (EX_DATAERR) — отчёт повреждён, схема неизвестна,
//         неизвестный токен;
//    66   нет входа (EX_NOINPUT) — файл не найден или не читается;
//    69   проверка невозможна (EX_UNAVAILABLE) — например, HTML получился с
//         внешними ссылками и проверка самодостаточности не пройдена;
//    70   внутренняя ошибка (EX_SOFTWARE) — непойманное исключение.
//
// ---------------------------------------------------------------------------
// Контракт с остальным CLI (файл main.cpp, задача 61)
// ---------------------------------------------------------------------------
//     int cmdReport(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
//     int cmdReport(int argc, char** argv, std::ostream& out, std::ostream& err);
//
// Как и в cmd_rules.cpp: зависимостей от других файлов src/cli нет, точки входа
// живут в namespace mrproper::cli, args — то, что стоит после «report».
// Отсутствие обеих форм вывода не считается ошибкой: по умолчанию печатается
// нормализованный JSON в stdout.
//
// ---------------------------------------------------------------------------
// Ограничения, о которых честно сказано в справке команды
// ---------------------------------------------------------------------------
//   * «--show-serials» не может раскрыть уже замаскированные серийники: в
//     отчёте их может не быть в исходном виде (SPEC §5), и команда не
//     притворяется обратным;
//   * «--generated-at» нужен для детерминированного HTML в golden-тесте: без
//     него берётся время окончания операции из самого отчёта, а если его там
//     нет — 0 и подпись «неизвестно». Часов машины команда не читает намеренно:
//     одинаковый вход обязан давать одинаковый файл (SPEC §11.4).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/report_html.hpp"
#include "core/report_json.hpp"

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

// ---------------------------------------------------------------------------
// Коды возврата и константы
// ---------------------------------------------------------------------------

constexpr int kExitOk = 0;
constexpr int kExitUsage = 64;
constexpr int kExitDataError = 65;
constexpr int kExitNoInput = 66;
constexpr int kExitUnavailable = 69;
constexpr int kExitSoftware = 70;

// Потолок на входной отчёт. Отчёт с 10 МБ — это не отчёт, а чужой файл,
// подставленный вместо него; читать его целиком незачем.
constexpr std::uintmax_t kMaxReportBytes = 64u * 1024u * 1024u;

constexpr const char* kUsage =
    "mrproper-cli report — отчёт очистки в JSON или в самодостаточный HTML\n"
    "(SPEC §4 FR-8, §11.4)\n"
    "\n"
    "  report [--json|--html] [опции]  читает JSON-отчёт (--in, по умолчанию stdin)\n"
    "                                 и печатает нормализованный JSON или HTML\n"
    "\n"
    "Опции:\n"
    "  --in <файл|->        исходный JSON-отчёт (по умолчанию stdin)\n"
    "  --out <файл|->       куда писать результат (по умолчанию stdout)\n"
    "  --json               нормализованный JSON-отчёт (по умолчанию)\n"
    "  --html               самодостаточный HTML: CSS внутри, ни одной внешней ссылки\n"
    "  --title <текст>      заголовок отчёта\n"
    "  --lang <ru|en>       язык подписей HTML (по умолчанию ru)\n"
    "  --generated-at <t>   метка времени отчёта, unix-секунды (для golden-тестов)\n"
    "  --show-serials       не маскировать серийники при выводе HTML (в исходном\n"
    "                       отчёте они могут быть уже замаскированы)\n"
    "  --max-candidates <n> показать только первые n кандидатов в HTML (0 — все)\n"
    "  --no-candidates      не печатать кандидатов\n"
    "  --no-disks           не печатать карту разделов\n"
    "  --no-partition-map   не печатать цветную полосу разделов\n"
    "  --no-reasons         не печатать колонку «почему это мусор» (FR-4)\n"
    "  --binary-units       размеры в 1024 (по умолчанию 1000, как в core::units)\n"
    "  --compact            JSON без отступов\n"
    "  --allow-external     не считать внешние ссылки в HTML ошибкой\n"
    "  -h, --help           эта справка\n"
    "\n"
    "Коды возврата:\n"
    "  0 успех   64 неверные аргументы   65 данные не годятся\n"
    "  66 нет входа   69 проверка невозможна   70 внутренняя ошибка\n";

enum class Output { Json, Html };

struct Options {
    Output output{Output::Json};
    std::optional<std::filesystem::path> input;
    std::optional<std::filesystem::path> output_;
    std::string title;
    core::HtmlReportLanguage language{core::HtmlReportLanguage::Russian};
    std::int64_t generatedAt{};
    bool generatedAtSet{};
    bool showSerials{};
    std::size_t maxCandidates{};
    bool showCandidates{true};
    bool showDisks{true};
    bool showPartitionMap{true};
    bool showReasons{true};
    bool binaryUnits{};
    bool compact{};
    bool allowExternal{};
    std::string problem;
};

// ---------------------------------------------------------------------------
// Пути и ввод-вывод
// ---------------------------------------------------------------------------

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string quoted(const std::filesystem::path& path) { return "\"" + toUtf8(path) + "\""; }

std::string readAllStdin() {
    std::string text;
    char buffer[64 * 1024];
    while (std::cin.read(buffer, static_cast<std::streamsize>(sizeof(buffer))) || std::cin.gcount() > 0) {
        text.append(buffer, static_cast<std::size_t>(std::cin.gcount()));
        if (text.size() > kMaxReportBytes) break;
    }
    return text;
}

bool readInput(const Options& options, std::string& text, std::string& problem) {
    if (!options.input.has_value()) {
        text = readAllStdin();
        if (text.empty()) {
            problem = "пустой ввод: укажите --in <файл> или подайте отчёт на stdin";
            return false;
        }
        return true;
    }
    const std::filesystem::path& path = *options.input;
    if (path == std::filesystem::path(L"-")) {
        text = readAllStdin();
        if (text.empty()) {
            problem = "пустой ввод: stdin не содержит отчёта";
            return false;
        }
        return true;
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        problem = "файл не найден или недоступен: " + quoted(path);
        return false;
    }
    if (size > kMaxReportBytes) {
        problem = "файл " + quoted(path) + " больше " + std::to_string(kMaxReportBytes) + " байт";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        problem = "файл " + quoted(path) + " не открывается на чтение";
        return false;
    }
    std::string bytes;
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        stream.read(bytes.data(), static_cast<std::streamsize>(size));
        if (stream.gcount() != static_cast<std::streamsize>(size)) {
            problem = "файл " + quoted(path) + " прочитан не полностью";
            return false;
        }
    }
    text = std::move(bytes);
    return true;
}

bool writeOutput(const Options& options, const std::string& text, std::string& problem) {
    if (!options.output_.has_value() || *options.output_ == std::filesystem::path(L"-")) {
        std::cout << text;
        std::cout.flush();
        if (!std::cout) {
            problem = "не удалось записать в stdout";
            return false;
        }
        return true;
    }
    const std::filesystem::path& path = *options.output_;
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            problem = "не удалось создать каталог для " + quoted(path) + ": " + ec.message();
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        problem = "файл " + quoted(path) + " не открывается на запись";
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) {
        problem = "файл " + quoted(path) + " записан не полностью";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Чтение отчёта обратно в модель
// ---------------------------------------------------------------------------

// Токены значений в отчёте — ровно те, что пишет core::report_json. Своих
// таблиц «на всякий случай» здесь нет: неизвестный токен обязан быть ошибкой,
// иначе «safety:» с опечаткой тихо превратится в безопасный Review.
core::SafetyLevel safetyFromToken(std::string_view token, std::string& problem) {
    if (token == "safe") return core::SafetyLevel::Safe;
    if (token == "review") return core::SafetyLevel::Review;
    if (token == "risky") return core::SafetyLevel::Risky;
    problem = "неизвестный уровень риска \"" + std::string(token) + "\" (ожидаются safe, review, risky)";
    return core::SafetyLevel::Review;
}

core::PlanAction actionFromToken(std::string_view token, std::string& problem) {
    if (token == "delete") return core::PlanAction::Delete;
    if (token == "trash") return core::PlanAction::Trash;
    if (token == "keep") return core::PlanAction::Keep;
    if (token == "skip-locked") return core::PlanAction::SkipLocked;
    problem = "неизвестное действие \"" + std::string(token) + "\" (ожидаются delete, trash, keep, skip-locked)";
    return core::PlanAction::Keep;
}

core::ReportOperationStatus statusFromToken(std::string_view token, std::string& problem) {
    if (token == "success") return core::ReportOperationStatus::Success;
    if (token == "failed") return core::ReportOperationStatus::Failed;
    if (token == "skipped") return core::ReportOperationStatus::Skipped;
    if (token == "partial") return core::ReportOperationStatus::Partial;
    problem = "неизвестный статус операции \"" + std::string(token) +
              "\" (ожидаются success, failed, skipped, partial)";
    return core::ReportOperationStatus::Success;
}

core::BusType busFromToken(std::string_view token, std::string& problem) {
    if (token == "sata") return core::BusType::Sata;
    if (token == "nvme") return core::BusType::Nvme;
    if (token == "usb") return core::BusType::Usb;
    if (token == "scsi") return core::BusType::Scsi;
    if (token == "sd") return core::BusType::Sd;
    if (token == "sas") return core::BusType::Sas;
    if (token == "virtual") return core::BusType::Virtual;
    if (token == "raid") return core::BusType::Raid;
    if (token == "unknown") return core::BusType::Unknown;
    problem = "неизвестный тип шины \"" + std::string(token) + "\"";
    return core::BusType::Unknown;
}

core::PartitionKind partitionKindFromToken(std::string_view token, std::string& problem) {
    if (token == "basic-data") return core::PartitionKind::BasicData;
    if (token == "system") return core::PartitionKind::System;
    if (token == "msr") return core::PartitionKind::Msr;
    if (token == "recovery") return core::PartitionKind::Recovery;
    if (token == "oem") return core::PartitionKind::Oem;
    if (token == "efi") return core::PartitionKind::Efi;
    if (token == "reserved") return core::PartitionKind::Reserved;
    if (token == "unallocated") return core::PartitionKind::Unallocated;
    if (token == "unknown") return core::PartitionKind::Unknown;
    problem = "неизвестный тип раздела \"" + std::string(token) + "\"";
    return core::PartitionKind::Unknown;
}

core::ReportKind reportKindFromToken(std::string_view token, std::string& problem) {
    if (token == "scan") return core::ReportKind::Scan;
    if (token == "cleanup") return core::ReportKind::Cleanup;
    if (token == "dry-run") return core::ReportKind::DryRun;
    problem = "неизвестный вид отчёта \"" + std::string(token) + "\" (ожидаются scan, cleanup, dry-run)";
    return core::ReportKind::Cleanup;
}

// Чтение объекта: необязательное поле с типом по умолчанию. Отсутствующее поле и
// поле неверного типа — не одно и то же: первое нормально (отчёт мог быть
// записан без раздела), второе означает повреждение или подмену.
const json::Value& childOrNull(const json::Value& node, std::string_view key) {
    static const json::Value kNull;
    const json::Value* found = node.find(key);
    return found != nullptr ? *found : kNull;
}

std::string readString(const json::Value& node, std::string_view key, std::string_view fallback = {}) {
    const json::Value& value = childOrNull(node, key);
    if (value.isNull()) return std::string(fallback);
    if (!value.isString()) return std::string(fallback);
    return value.asString();
}

std::uint64_t readCount(const json::Value& node, std::string_view key, std::uint64_t fallback = 0) {
    const json::Value& value = childOrNull(node, key);
    if (!value.isNumber()) return fallback;
    const double raw = value.asNumber();
    if (!(raw >= 0.0)) return fallback;
    return static_cast<std::uint64_t>(raw);
}

std::int64_t readSigned(const json::Value& node, std::string_view key, std::int64_t fallback = 0) {
    const json::Value& value = childOrNull(node, key);
    if (!value.isNumber()) return fallback;
    const double raw = value.asNumber();
    if (!(raw >= -9.0e15 && raw <= 9.0e15)) return fallback;
    return static_cast<std::int64_t>(raw);
}

bool readFlag(const json::Value& node, std::string_view key, bool fallback = false) {
    const json::Value& value = childOrNull(node, key);
    if (!value.isBool()) return fallback;
    return value.asBool();
}

std::vector<std::string> readStringArray(const json::Value& node, std::string_view key) {
    std::vector<std::string> out;
    const json::Value& value = childOrNull(node, key);
    if (!value.isArray()) return out;
    out.reserve(value.items().size());
    for (const json::Value& item : value.items()) {
        if (item.isString()) out.push_back(item.asString());
    }
    return out;
}

// «0x80070005» → 0x80070005. Код ошибки приходит строкой (SPEC §4 FR-6: в
// отчёте видно, чем именно упало), а модель хранит число.
std::uint32_t readErrorCode(const json::Value& node) {
    const std::string text = readString(node, "code");
    if (text.empty()) return 0;
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 0);
        if (consumed != text.size()) return 0;
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        return 0;
    }
}

// GUID в отчёте — строка «xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx» (core
// guidToString). Нераспознанная строка — «типа нет», а не нулевой GUID: у MBR
// диска gptType бывает null, и подменять его «нулями» значит выдумать данные.
bool parseGuidString(std::string_view text, core::Guid& out) {
    static constexpr std::size_t kDigits = 32;
    std::string digits;
    digits.reserve(kDigits);
    for (const char c : text) {
        if (c == '-') continue;
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) return false;
        digits.push_back(c);
    }
    if (digits.size() != kDigits) return false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto nibble = [&digits](std::size_t index) {
            const char c = digits[index];
            const int value = std::isdigit(static_cast<unsigned char>(c)) != 0
                                  ? c - '0'
                                  : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
            return static_cast<std::uint8_t>(value);
        };
        out[i] = static_cast<std::uint8_t>((nibble(i * 2u) << 4) | nibble(i * 2u + 1u));
    }
    return true;
}

// Парсер отчёта. Каждый отказ — std::string с точным местом: «disks[1]
// partitions[0]: неизвестный тип раздела "…"».
class ReportParser {
public:
    explicit ReportParser(core::Report& report) : report_(report) {}

    bool parse(const json::Value& root) {
        if (!root.isObject()) {
            return fail("отчёт должен быть объектом JSON");
        }
        const json::Value& schema = childOrNull(root, "schema");
        if (schema.isNumber()) {
            const int version = static_cast<int>(schema.asNumber());
            report_.schemaVersion = version;
            if (version != core::kReportJsonSchema) {
                return fail("неизвестная версия схемы отчёта: " + std::to_string(version) + ", ждём " +
                            std::to_string(core::kReportJsonSchema));
            }
        }
        if (const json::Value* kind = root.find("kind"); kind != nullptr && kind->isString()) {
            report_.kind = reportKindFromToken(kind->asString(), problem_);
        }
        if (const json::Value* app = root.find("app"); app != nullptr && app->isObject()) {
            report_.environment.appVersion = readString(*app, "version");
            report_.environment.pid = static_cast<std::uint32_t>(readCount(*app, "pid"));
            report_.environment.rulesVersion = readString(*app, "rulesVersion");
        }
        if (const json::Value* os = root.find("os"); os != nullptr && os->isObject()) {
            report_.environment.osCaption = readString(*os, "caption");
            report_.environment.osVersion = readString(*os, "version");
            report_.environment.osBuild = static_cast<std::uint32_t>(readCount(*os, "build"));
            report_.environment.architecture = readString(*os, "architecture");
        }
        if (const json::Value* timing = root.find("timing"); timing != nullptr && timing->isObject()) {
            report_.timing.startedAtUnix = readSigned(*timing, "startedAtUnix");
            report_.timing.finishedAtUnix = readSigned(*timing, "finishedAtUnix");
            report_.timing.durationMs = readSigned(*timing, "durationMs");
        }
        // Приватность: флаг говорит, маскированы ли серийники в самом отчёте.
        // Команда не может их раскрыть и не делает вид, что может (SPEC §5).
        if (const json::Value* privacy = root.find("privacy"); privacy != nullptr && privacy->isObject()) {
            report_.options.maskSerials = readFlag(*privacy, "serialsMasked", true);
            report_.options.maskVolumeGuids = readFlag(*privacy, "volumeGuidsMasked", true);
        }
        if (const json::Value* notes = root.find("notes"); notes != nullptr && notes->isString()) {
            report_.notes = notes->asString();
        }
        if (!parseArrayOf(root, "disks", "disks", [this](const json::Value& node) { parseDisk(node); })) return false;
        if (!parseArrayOf(root, "candidates", "candidates", [this](const json::Value& node) { parseCandidate(node); })) {
            return false;
        }
        if (!parseArrayOf(root, "operations", "operations", [this](const json::Value& node) { parseOperation(node); })) {
            return false;
        }
        if (!parseArrayOf(root, "untouched", "untouched", [this](const json::Value& node) { parseOperation(node); })) {
            return false;
        }
        if (!parseArrayOf(root, "errors", "errors", [this](const json::Value& node) { parseError(node); })) return false;
        return problem_.empty();
    }

    [[nodiscard]] const std::string& problem() const { return problem_; }

private:
    bool fail(std::string text) {
        problem_ = std::move(text);
        return false;
    }

    // Массив объектов: каждый элемент разбирает переданный обработчик, а его
    // отказ поднимается наверх с индексом элемента.
    template <typename Handler>
    bool parseArrayOf(const json::Value& root, std::string_view key, std::string_view name, Handler handler) {
        const json::Value& array = childOrNull(root, key);
        if (array.isNull()) return true;
        if (!array.isArray()) return fail(std::string(name) + " должен быть массивом");
        for (std::size_t i = 0; i < array.items().size(); ++i) {
            const std::string prefix = std::string(name) + "[" + std::to_string(i) + "]: ";
            problem_.clear();
            const std::size_t before = report_.errors.size();
            const std::size_t disksBefore = report_.disks.size();
            const std::size_t candidatesBefore = report_.candidates.size();
            const std::size_t operationsBefore = report_.operations.size();
            const std::size_t untouchedBefore = report_.untouched.size();
            (void)before;
            handler(array.items()[i]);
            if (!problem_.empty()) return fail(prefix + problem_);
            if (key == "disks" && report_.disks.size() == disksBefore) {
                return fail(prefix + "элемент не является объектом");
            }
            if (key == "candidates" && report_.candidates.size() == candidatesBefore) {
                return fail(prefix + "элемент не является объектом");
            }
            if (key == "operations" && report_.operations.size() == operationsBefore) {
                return fail(prefix + "элемент не является объектом");
            }
            if (key == "untouched" && report_.untouched.size() == untouchedBefore) {
                return fail(prefix + "элемент не является объектом");
            }
            if (key == "errors" && report_.errors.size() == before) return fail(prefix + "элемент не является объектом");
        }
        return true;
    }

    void parseDisk(const json::Value& node) {
        if (!node.isObject()) {
            problem_ = "элемент не является объектом";
            return;
        }
        core::PhysicalDisk disk;
        disk.number = static_cast<int>(readCount(node, "number"));
        disk.model = readString(node, "model");
        disk.serial = readString(node, "serial");
        disk.firmware = readString(node, "firmware");
        disk.devicePath = readString(node, "devicePath");
        disk.sizeBytes = readCount(node, "sizeBytes");
        disk.removable = readFlag(node, "removable");
        disk.readOnly = readFlag(node, "readOnly");
        disk.trimSupported = readFlag(node, "trimSupported");
        disk.smartAvailable = readFlag(node, "smartAvailable");
        if (const json::Value* bus = node.find("bus"); bus != nullptr && bus->isString()) {
            disk.bus = busFromToken(bus->asString(), problem_);
            if (!problem_.empty()) return;
        }
        if (const json::Value* partitions = node.find("partitions"); partitions != nullptr) {
            if (!partitions->isArray()) {
                problem_ = "partitions должен быть массивом";
                return;
            }
            for (std::size_t i = 0; i < partitions->items().size(); ++i) {
                problem_.clear();
                disk.partitions.push_back(parsePartition(partitions->items()[i], i));
                if (!problem_.empty()) {
                    problem_ = "partitions[" + std::to_string(i) + "]: " + problem_;
                    return;
                }
            }
        }
        report_.disks.push_back(std::move(disk));
    }

    core::Partition parsePartition(const json::Value& node, std::size_t index) {
        core::Partition partition;
        (void)index;
        if (!node.isObject()) {
            problem_ = "элемент не является объектом";
            return partition;
        }
        partition.index = static_cast<std::uint32_t>(readCount(node, "index"));
        partition.offsetBytes = readCount(node, "offsetBytes");
        partition.lengthBytes = readCount(node, "lengthBytes");
        partition.mbrType = static_cast<std::uint8_t>(readCount(node, "mbrType"));
        partition.gptName = readString(node, "gptName");
        partition.system = readFlag(node, "system");
        partition.boot = readFlag(node, "boot");
        partition.hidden = readFlag(node, "hidden");
        if (const json::Value* kind = node.find("kind"); kind != nullptr && kind->isString()) {
            partition.kind = partitionKindFromToken(kind->asString(), problem_);
            if (!problem_.empty()) return partition;
        }
        if (const json::Value* gpt = node.find("gptType"); gpt != nullptr && gpt->isString()) {
            partition.hasGptType = parseGuidString(gpt->asString(), partition.gptType);
            if (!partition.hasGptType) {
                problem_ = "gptType \"" + gpt->asString() + "\" не GUID";
                return partition;
            }
        }
        partition.hasVolume = readFlag(node, "hasVolume");
        if (const json::Value* volume = node.find("volume"); volume != nullptr && volume->isObject()) {
            partition.volume.volumeGuidPath = readString(*volume, "guidPath");
            partition.volume.label = readString(*volume, "label");
            partition.volume.fileSystem = readString(*volume, "fileSystem");
            partition.volume.totalBytes = readCount(*volume, "totalBytes");
            partition.volume.freeBytes = readCount(*volume, "freeBytes");
            partition.volume.encrypted = readFlag(*volume, "encrypted");
            partition.volume.dirty = readFlag(*volume, "dirty");
            partition.volume.readOnly = readFlag(*volume, "readOnly");
            partition.volume.mountPoints = readStringArray(*volume, "mountPoints");
        }
        return partition;
    }

    void parseCandidate(const json::Value& node) {
        if (!node.isObject()) {
            problem_ = "элемент не является объектом";
            return;
        }
        core::CleanupCandidate candidate;
        candidate.ruleId = readString(node, "ruleId");
        candidate.category = readString(node, "category");
        candidate.path = readString(node, "path");
        candidate.displayName = readString(node, "displayName");
        candidate.logicalBytes = readCount(node, "logicalBytes");
        candidate.allocatedBytes = readCount(node, "allocatedBytes");
        candidate.fileCount = static_cast<std::uint32_t>(readCount(node, "fileCount"));
        candidate.oldestWrite = readSigned(node, "oldestWrite");
        candidate.newestWrite = readSigned(node, "newestWrite");
        candidate.lastAccess = readSigned(node, "lastAccess");
        if (const json::Value* safety = node.find("safety"); safety != nullptr && safety->isString()) {
            candidate.safety = safetyFromToken(safety->asString(), problem_);
            if (!problem_.empty()) return;
        }
        candidate.confidence = static_cast<int>(readSigned(node, "confidence"));
        candidate.reasons = readStringArray(node, "reasons");
        if (const json::Value* lockedBy = node.find("lockedBy"); lockedBy != nullptr && lockedBy->isArray()) {
            for (const json::Value& item : lockedBy->items()) {
                if (!item.isObject()) continue;
                core::ProcessRef process;
                process.pid = static_cast<std::uint32_t>(readCount(item, "pid"));
                process.name = readString(item, "name");
                candidate.lockedBy.push_back(std::move(process));
            }
        }
        report_.candidates.push_back(std::move(candidate));
    }

    void parseOperation(const json::Value& node) {
        if (!node.isObject()) {
            problem_ = "элемент не является объектом";
            return;
        }
        core::ReportOperation operation;
        operation.candidateIndex = static_cast<std::size_t>(readCount(node, "candidateIndex"));
        operation.category = readString(node, "category");
        operation.displayName = readString(node, "displayName");
        operation.path = readString(node, "path");
        operation.bytes = readCount(node, "bytes");
        operation.confidence = static_cast<int>(readSigned(node, "confidence"));
        operation.attempts = static_cast<std::uint32_t>(readCount(node, "attempts", 1));
        operation.transactionId = readString(node, "transactionId");
        operation.startedAtUnix = readSigned(node, "startedAtUnix");
        operation.finishedAtUnix = readSigned(node, "finishedAtUnix");
        operation.detail = readString(node, "detail");
        if (const json::Value* action = node.find("action"); action != nullptr && action->isString()) {
            operation.action = actionFromToken(action->asString(), problem_);
            if (!problem_.empty()) return;
        }
        if (const json::Value* safety = node.find("safety"); safety != nullptr && safety->isString()) {
            operation.safety = safetyFromToken(safety->asString(), problem_);
            if (!problem_.empty()) return;
        }
        if (const json::Value* status = node.find("status"); status != nullptr && status->isString()) {
            operation.status = statusFromToken(status->asString(), problem_);
            if (!problem_.empty()) return;
        }
        report_.operations.push_back(std::move(operation));
    }

    void parseError(const json::Value& node) {
        if (!node.isObject()) {
            problem_ = "элемент не является объектом";
            return;
        }
        core::ReportError error;
        error.scope = readString(node, "scope");
        error.code = readString(node, "code");
        error.message = readString(node, "message");
        error.path = readString(node, "path");
        error.operation = readString(node, "operation");
        error.atUnix = readSigned(node, "atUnix");
        error.count = static_cast<std::uint32_t>(readCount(node, "count", 1));
        report_.errors.push_back(std::move(error));
    }

    core::Report& report_;
    std::string problem_;
};

// ---------------------------------------------------------------------------
// Отчёт → HTML
// ---------------------------------------------------------------------------

// Заметки отчёта — свободный текст; в HTML они идут списком, поэтому текст
// делится по строкам, а не показывается одним абзацем.
std::vector<std::string> splitNotes(std::string_view notes) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin <= notes.size()) {
        const std::size_t end = notes.find('\n', begin);
        if (end == std::string_view::npos) {
            const std::string line(notes.substr(begin));
            if (!line.empty()) out.push_back(line);
            break;
        }
        std::string line(notes.substr(begin, end - begin));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.push_back(line);
        begin = end + 1u;
    }
    return out;
}

core::HtmlReportInput toHtmlInput(const core::Report& report, const Options& options) {
    core::HtmlReportInput input;
    input.meta.appVersion = report.environment.appVersion;
    input.meta.osName = report.environment.osCaption;
    input.meta.osVersion = report.environment.osVersion;
    input.title = options.title;
    // Время отчёта — из самого отчёта, а не из часов машины: одинаковый вход
    // обязан давать одинаковый файл (SPEC §11.4). Явно заданное
    // --generated-at приоритетнее.
    input.generatedAtUnix = options.generatedAtSet ? options.generatedAt : report.timing.finishedAtUnix;
    input.startedAtUnix = report.timing.startedAtUnix;
    input.finishedAtUnix = report.timing.finishedAtUnix;
    input.dryRun = report.kind != core::ReportKind::Cleanup;

    const core::ReportTotals totals = core::summarizeReport(report);
    input.plannedBytes = totals.candidateBytes;
    input.freedBytes = totals.freedBytes;
    for (const core::ReportOperation& operation : report.operations) {
        if (!operation.transactionId.empty()) {
            input.transactionId = operation.transactionId;
            break;
        }
    }

    input.disks = report.disks;
    input.candidates = report.candidates;

    // Операции и «не тронуто» попадают в один раздел, но помечены по-разному:
    // пропущенная операция — это тоже результат (FR-6: остальное продолжается).
    const auto append = [&input](const core::ReportOperation& operation, bool skippedByDefault) {
        core::HtmlReportOperation html;
        html.action = operation.action;
        html.name = operation.displayName.empty() ? operation.category : operation.displayName;
        html.path = operation.path;
        html.bytes = operation.bytes;
        html.safety = operation.safety;
        html.confidence = operation.confidence;
        html.success = operation.status != core::ReportOperationStatus::Failed;
        html.skipped = skippedByDefault || operation.status == core::ReportOperationStatus::Skipped;
        html.error = operation.detail;
        html.durationMs = operation.finishedAtUnix > 0 && operation.startedAtUnix > 0
                              ? (operation.finishedAtUnix - operation.startedAtUnix) * 1000
                              : 0;
        input.operations.push_back(std::move(html));
    };
    for (const core::ReportOperation& operation : report.operations) append(operation, false);
    for (const core::ReportOperation& operation : report.untouched) append(operation, true);

    for (const core::ReportError& error : report.errors) {
        core::HtmlReportIssue issue;
        issue.stage = error.scope;
        issue.subject = error.path.empty() ? error.operation : error.path;
        issue.message = error.message;
        issue.code = readErrorCode(error);
        input.issues.push_back(std::move(issue));
    }

    input.notes = splitNotes(report.notes);
    return input;
}

core::HtmlReportOptions toHtmlOptions(const Options& options) {
    core::HtmlReportOptions html;
    html.language = options.language;
    html.maskSerials = !options.showSerials;
    html.showDisks = options.showDisks;
    html.showPartitionMap = options.showPartitionMap;
    html.showCandidates = options.showCandidates;
    html.showReasons = options.showReasons;
    html.maxCandidates = options.maxCandidates;
    html.binaryUnits = options.binaryUnits;
    return html;
}

// ---------------------------------------------------------------------------
// Разбор аргументов
// ---------------------------------------------------------------------------

struct ArgsCursor {
    const std::vector<std::string>& items;
    std::size_t position{0};

    [[nodiscard]] bool done() const { return position >= items.size(); }
};

bool takeValue(ArgsCursor& cursor, const std::string& flag, std::string& value, std::string& problem) {
    if (cursor.position + 1u >= cursor.items.size()) {
        problem = "опция " + flag + " требует значения";
        return false;
    }
    ++cursor.position;
    value = cursor.items[cursor.position];
    return true;
}

bool parseArgs(const std::vector<std::string>& argv, Options& options) {
    ArgsCursor cursor{argv, 0};
    bool positionalSeen = false;
    while (!cursor.done()) {
        const std::string item = cursor.items[cursor.position++];
        if (item == "-h" || item == "--help") {
            options.problem.clear();
            options.input.reset();
            options.output = Output::Html;
            options.title = "__help__";
            return true;
        }
        if (item == "--json") {
            options.output = Output::Json;
            continue;
        }
        if (item == "--html") {
            options.output = Output::Html;
            continue;
        }
        if (item == "--in" || item == "--input") {
            std::string value;
            if (!takeValue(cursor, item, value, options.problem)) return false;
            options.input = pathFromUtf8(value);
            continue;
        }
        if (item == "--out" || item == "--output") {
            std::string value;
            if (!takeValue(cursor, item, value, options.problem)) return false;
            options.output_ = pathFromUtf8(value);
            continue;
        }
        if (item == "--title") {
            if (!takeValue(cursor, item, options.title, options.problem)) return false;
            continue;
        }
        if (item == "--lang") {
            std::string value;
            if (!takeValue(cursor, item, value, options.problem)) return false;
            options.language = core::htmlLanguageFromString(value);
            continue;
        }
        if (item == "--generated-at") {
            std::string value;
            if (!takeValue(cursor, item, value, options.problem)) return false;
            try {
                options.generatedAt = static_cast<std::int64_t>(std::stoll(value));
                options.generatedAtSet = true;
            } catch (const std::exception&) {
                options.problem = "--generated-at ждёт unix-секунды, получено \"" + value + "\"";
                return false;
            }
            continue;
        }
        if (item == "--max-candidates") {
            std::string value;
            if (!takeValue(cursor, item, value, options.problem)) return false;
            try {
                options.maxCandidates = static_cast<std::size_t>(std::stoull(value));
            } catch (const std::exception&) {
                options.problem = "--max-candidates ждёт число, получено \"" + value + "\"";
                return false;
            }
            continue;
        }
        if (item == "--show-serials") {
            options.showSerials = true;
            continue;
        }
        if (item == "--no-candidates") {
            options.showCandidates = false;
            continue;
        }
        if (item == "--no-disks") {
            options.showDisks = false;
            continue;
        }
        if (item == "--no-partition-map") {
            options.showPartitionMap = false;
            continue;
        }
        if (item == "--no-reasons") {
            options.showReasons = false;
            continue;
        }
        if (item == "--binary-units") {
            options.binaryUnits = true;
            continue;
        }
        if (item == "--compact") {
            options.compact = true;
            continue;
        }
        if (item == "--allow-external") {
            options.allowExternal = true;
            continue;
        }
        if (!item.empty() && item.front() == '-') {
            options.problem = "неизвестная опция «" + item + "»";
            return false;
        }
        if (positionalSeen) {
            options.problem = "лишний аргумент «" + item + "», ожидался один входной отчёт";
            return false;
        }
        options.input = pathFromUtf8(item);
        positionalSeen = true;
    }
    return true;
}

int runReport(const Options& options, std::ostream& out, std::ostream& err) {
    if (options.title == "__help__") {
        out << kUsage;
        return kExitOk;
    }

    std::string text;
    std::string problem;
    if (!readInput(options, text, problem)) {
        err << "ОШИБКА: " << problem << '\n';
        return kExitNoInput;
    }

    // Разбор JSON и проверка схемы. Здесь отчёт может быть любым: привести его
    // к каноническому виду и отрисовать — работа команды, а не движка.
    json::Value root;
    try {
        root = json::parse(text);
    } catch (const json::ParseError& error) {
        err << "ОШИБКА: JSON не разбирается: " << error.what() << '\n';
        return kExitDataError;
    }

    core::Report report;
    ReportParser parser(report);
    if (!parser.parse(root)) {
        err << "ОШИБКА: отчёт не читается: " << parser.problem() << '\n';
        return kExitDataError;
    }

    std::string result;
    if (options.output == Output::Json) {
        result = core::reportToJson(report, options.compact ? -1 : 2);
    } else {
        result = core::renderHtmlReport(toHtmlInput(report, options), toHtmlOptions(options));
        // Самодостаточность — требование FR-8 и §12 («открывается без
        // приложения»), и регрессия «случайно подключили CDN» должна ломать
        // CI, а не тихо попадать в баг-репорт.
        if (!core::isStandaloneHtml(result) && !options.allowExternal) {
            err << "ОШИБКА: HTML получился не самодостаточным, внешние ссылки: "
                << core::findExternalReferences(result) << '\n';
            return kExitUnavailable;
        }
    }

    if (!writeOutput(options, result, problem)) {
        err << "ОШИБКА: " << problem << '\n';
        return kExitNoInput;
    }
    return kExitOk;
}

}  // namespace

// ---------------------------------------------------------------------------
// Точки входа
// ---------------------------------------------------------------------------

int cmdReport(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    Options options;
    if (!parseArgs(args, options)) {
        err << "ОШИБКА: " << options.problem << "\n\n" << kUsage;
        return kExitUsage;
    }
    try {
        return runReport(options, out, err);
    } catch (const std::bad_alloc&) {
        err << "ОШИБКА: не хватило памяти\n";
        return kExitSoftware;
    } catch (const std::exception& error) {
        err << "ОШИБКА: " << error.what() << '\n';
        return kExitSoftware;
    }
}

int cmdReport(int argc, char** argv, std::ostream& out, std::ostream& err) {
    std::vector<std::string> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    return cmdReport(args, out, err);
}

}  // namespace mrproper::cli
