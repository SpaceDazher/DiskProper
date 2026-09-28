// Реализация журнала транзакций и снимка плана. Обоснование границ, формата и
// правил — в journal.hpp; здесь код и замечания, которые видны с этого уровня.
//
// Порядок работы модуля в одном запуске очистки:
//
//   1. `writeSnapshot` — снимок состояния (FR-5): список операций, PID, версия,
//      размер. Пишется атомарно (временный файл + rename) и перечитывается
//      обратно, чтобы «запись в журнал» означала «файл есть и верен»;
//   2. `beginTransaction` — снимок ПЕРВЫМ, потом запись `tx.begin`. Отказ здесь
//      означает «не начинать»: согласие пользователя было дано на конкретный
//      список операций, и без этого списка подтверждать нечего;
//   3. `recordOperation` — по одной записи на операцию из пула исполнителя
//      (§6.4). Ошибки не фатальны (FR-6): операция падает, журнал — нет;
//   4. `commit` / `cancel` / `fail` — запись `tx.end`. Если её не будет,
//      транзакция останется открытой, и это будет видно при следующем запуске.
//
// Про побочные эффекты. Модуль пишет на диск в двух местах: снимок и сам
// журнал, и оба раза через временный файл с последующим rename. Причина не в
// «красоте атомарности», а в требовании §12: «отмена во время операции
// оставляет систему в согласованном состоянии», а согласованное состояние
// невозможно, если на диске осталась половина JSON-документа.
//
// Про HRESULT. Win32-код приходит числом от исполнителя: журнал не знает,
// откуда он (platform::vfs, Restart Manager, корзина) и не должен знать —
// §12 требует, чтобы код был в записи, а не чтобы модуль его добывал. Ноль
// означает «код неприменим» и остаётся нулём, а не пропадает: отсутствующее
// поле в записи читалось бы как «запись оборвалась».
//
// Про кодировки путей. Модель и журнал — UTF-8 (§6.3), а файловая система
// Windows ждёт UTF-16, иначе кириллица в имени пользователя превращает путь в
// несуществующий (SPEC §5: «не-ASCII и кириллица»). Узкий `std::fopen` с UTF-8
// на этом месте сломался бы, поэтому `nativePath` и `nativeToUtf8` переводят
// строку сами: на Windows это арифметика по суррогатам без windows.h, на
// других хостах путь и так байтовый.
#include "journal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>
#include <utility>

#include "core/log.hpp"
#include "core/units.hpp"

namespace mrproper::engine::journal {
namespace {

namespace fs = std::filesystem;
using json::Value;

constexpr std::int64_t kMillisPerSecond = 1000;

// Имена событий журнала. Константы, а не склейка строк по месту: grep по
// журналу должен находить все записи модуля одним словом.
constexpr std::string_view kEventTxBegin{"engine.journal.tx.begin"};
constexpr std::string_view kEventTxEnd{"engine.journal.tx.end"};
constexpr std::string_view kEventOperation{"engine.journal.op"};
constexpr std::string_view kEventSnapshot{"engine.journal.snapshot"};
constexpr std::string_view kEventPrune{"engine.journal.prune"};
constexpr std::string_view kEventRotate{"engine.journal.rotate"};

// Метка времени в имени архива: 16 знаков, чтобы лексикографический порядок
// совпадал с хронологическим при любой длине числа.
constexpr int kArchiveStampWidth = 16;

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

std::string padNumber(std::uint64_t value, int width) {
    std::string digits = std::to_string(value);
    const std::size_t wanted = static_cast<std::size_t>(width);
    if (digits.size() >= wanted) return digits;
    return std::string(wanted - digits.size(), '0') + digits;
}

std::int64_t floorDiv(std::int64_t value, std::int64_t divisor) noexcept {
    std::int64_t quotient = value / divisor;
    if (value % divisor != 0 && ((value < 0) != (divisor < 0))) --quotient;
    return quotient;
}

std::int64_t floorMod(std::int64_t value, std::int64_t divisor) noexcept {
    return value - floorDiv(value, divisor) * divisor;
}

bool utcFields(std::int64_t unixSeconds, std::tm& out) noexcept {
    const std::time_t raw = static_cast<std::time_t>(unixSeconds);
#if defined(_WIN32)
    return ::gmtime_s(&out, &raw) == 0;
#else
    return ::gmtime_r(&raw, &out) != nullptr;
#endif
}

// Метка «неразбираемого» времени: формат остаётся тем же, имя снимка не теряет
// грамматику, а сортировка относит такой файл к самым старым.
constexpr const char* kUnknownStamp = "00000000T000000";

// UTF-8 -> нативный путь. Windows: разбор UTF-8 в UTF-16 своими руками, потому
// что windows.h в слое движка нет (см. шапку .cpp), а WinAPI для этого и не
// нужен. Остальные хосты: путь и так байтовый.
std::filesystem::path nativePath(std::string_view utf8) {
#if defined(_WIN32)
    std::wstring wide;
    wide.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::uint32_t code = 0;
        std::size_t extra = 0;
        if (lead < 0x80u) {
            code = lead;
        } else if ((lead & 0xE0u) == 0xC0u) {
            code = lead & 0x1Fu;
            extra = 1;
        } else if ((lead & 0xF0u) == 0xE0u) {
            code = lead & 0x0Fu;
            extra = 2;
        } else if ((lead & 0xF8u) == 0xF0u) {
            code = lead & 0x07u;
            extra = 3;
        } else {
            code = 0xFFFD;  // мусорный байт — замена; путь всё равно не найдётся
        }
        if (extra != 0 && i + extra >= utf8.size()) {
            code = 0xFFFD;
            extra = 0;
        } else {
            for (std::size_t k = 1; k <= extra; ++k) {
                const auto next = static_cast<unsigned char>(utf8[i + k]);
                if ((next & 0xC0u) != 0x80u) {
                    code = 0xFFFD;
                    extra = k - 1;
                    break;
                }
                code = (code << 6) | (next & 0x3Fu);
            }
        }
        i += extra + 1;
        if (code >= 0x10000u) {
            const std::uint32_t shifted = code - 0x10000u;
            wide.push_back(static_cast<wchar_t>(0xD800u + (shifted >> 10)));
            wide.push_back(static_cast<wchar_t>(0xDC00u + (shifted & 0x3FFu)));
        } else {
            wide.push_back(static_cast<wchar_t>(code));
        }
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

// Нативный путь -> UTF-8. Нужен для имён файлов, пришедших с диска: в них
// может быть кириллица, и `path::string()` на Windows вернул бы ANSI-форму,
// по которой файл потом не найдётся.
std::string nativeToUtf8(const fs::path& path) {
#if defined(_WIN32)
    const std::wstring& wide = path.native();
    std::string out;
    out.reserve(wide.size());
    for (std::size_t i = 0; i < wide.size(); ++i) {
        std::uint32_t code = static_cast<std::uint32_t>(wide[i]);
        if (code >= 0xD800u && code <= 0xDBFFu && i + 1 < wide.size()) {
            const std::uint32_t low = static_cast<std::uint32_t>(wide[i + 1]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                ++i;
            }
        }
        if (code >= 0xD800u && code <= 0xDFFFu) code = 0xFFFD;
        if (code < 0x80u) {
            out += static_cast<char>(code);
        } else if (code < 0x800u) {
            out += static_cast<char>(0xC0u | (code >> 6));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        } else if (code < 0x10000u) {
            out += static_cast<char>(0xE0u | (code >> 12));
            out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (code >> 18));
            out += static_cast<char>(0x80u | ((code >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        }
    }
    return out;
#else
    return path.native();
#endif
}

// Префикс/суффикс по нативному пути: ASCII-сравнение работает и для wchar_t,
// и для char, поэтому проверка имён файлов не зависит от платформы.
bool nativeHasPrefix(const fs::path& path, std::string_view ascii) noexcept {
    const auto& native = path.native();
    if (native.size() < ascii.size()) return false;
    for (std::size_t i = 0; i < ascii.size(); ++i) {
        if (static_cast<char>(native[i]) != ascii[i]) return false;
    }
    return true;
}

bool nativeHasSuffix(const fs::path& path, std::string_view ascii) noexcept {
    const auto& native = path.native();
    if (native.size() < ascii.size()) return false;
    for (std::size_t i = 0; i < ascii.size(); ++i) {
        if (static_cast<char>(native[native.size() - ascii.size() + i]) != ascii[i]) return false;
    }
    return true;
}

std::string joinPath(std::string_view directory, std::string_view name) {
    if (directory.empty()) return std::string(name);
    if (directory.back() == '/' || directory.back() == '\\') return std::string(directory) + std::string(name);
    return std::string(directory) + "/" + std::string(name);
}

std::string fileNameOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string(path) : std::string(path.substr(slash + 1));
}

bool hasPrefix(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool hasSuffix(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---------------------------------------------------------------------------
// Безопасное чтение JSON: разбор журнала не должен ронять разбор журнала
// ---------------------------------------------------------------------------

std::string readString(const Value& object, std::string_view key) {
    const Value* found = object.find(key);
    return (found != nullptr && found->isString()) ? found->asString() : std::string{};
}

std::int64_t readNumber(const Value& object, std::string_view key, std::int64_t fallback = 0) {
    const Value* found = object.find(key);
    return (found != nullptr && found->isNumber()) ? static_cast<std::int64_t>(found->asNumber()) : fallback;
}

bool readFlag(const Value& object, std::string_view key) {
    const Value* found = object.find(key);
    return found != nullptr && found->isBool() && found->asBool();
}

bool hasKey(const Value& object, std::string_view key) {
    return object.find(key) != nullptr;
}

// Поиск значения по паре «имя — код». Строковые имена стабильны (см. шапку
// перечислений), поэтому таблица объявлена один раз, а не собирается на вызов.
struct NameEntry {
    const char* name;
    std::uint8_t code;
};

template <typename Enum, std::size_t N>
bool codeFromName(std::string_view text, const NameEntry (&table)[N], Enum& out) noexcept {
    for (const NameEntry& entry : table) {
        if (text == entry.name) {
            out = static_cast<Enum>(entry.code);
            return true;
        }
    }
    return false;
}

template <typename Enum, std::size_t N>
const char* nameFromCode(Enum value, const NameEntry (&table)[N]) noexcept {
    const auto code = static_cast<std::uint8_t>(value);
    for (const NameEntry& entry : table) {
        if (entry.code == code) return entry.name;
    }
    return "unknown";
}

constexpr NameEntry kTxStateNames[] = {
    {"open", static_cast<std::uint8_t>(TxState::Open)},
    {"committed", static_cast<std::uint8_t>(TxState::Committed)},
    {"cancelled", static_cast<std::uint8_t>(TxState::Cancelled)},
    {"failed", static_cast<std::uint8_t>(TxState::Failed)},
};

constexpr NameEntry kOpStatusNames[] = {
    {"deleted", static_cast<std::uint8_t>(OpStatus::Deleted)},
    {"trashed", static_cast<std::uint8_t>(OpStatus::Trashed)},
    {"skipped", static_cast<std::uint8_t>(OpStatus::Skipped)},
    {"blocked", static_cast<std::uint8_t>(OpStatus::Blocked)},
    {"cancelled", static_cast<std::uint8_t>(OpStatus::Cancelled)},
    {"failed", static_cast<std::uint8_t>(OpStatus::Failed)},
};

constexpr NameEntry kJournalEventNames[] = {
    {"tx.begin", static_cast<std::uint8_t>(JournalEvent::TxBegin)},
    {"op.result", static_cast<std::uint8_t>(JournalEvent::Operation)},
    {"tx.end", static_cast<std::uint8_t>(JournalEvent::TxEnd)},
    {"rotate", static_cast<std::uint8_t>(JournalEvent::Rotate)},
    {"note", static_cast<std::uint8_t>(JournalEvent::Note)},
};

const char* actionToken(core::PlanAction action) {
    switch (action) {
        case core::PlanAction::Delete:
            return "delete";
        case core::PlanAction::Trash:
            return "trash";
        case core::PlanAction::Keep:
            return "keep";
        case core::PlanAction::SkipLocked:
            return "skip_locked";
    }
    return "unknown";
}

bool actionFromToken(std::string_view text, core::PlanAction& out) noexcept {
    if (text == "delete") {
        out = core::PlanAction::Delete;
    } else if (text == "trash") {
        out = core::PlanAction::Trash;
    } else if (text == "keep") {
        out = core::PlanAction::Keep;
    } else if (text == "skip_locked") {
        out = core::PlanAction::SkipLocked;
    } else {
        return false;
    }
    return true;
}

// Чтение файла с пределом. Больше предела журнал не бывает: это либо мусор в
// каталоге, либо файл, который кто-то подсунул вместо журнала.
bool readFileLimited(const std::string& path, std::uint64_t limit, std::string& out, bool& truncated) {
    std::ifstream stream(nativePath(path), std::ios::binary);
    if (!stream) return false;

    std::error_code ec;
    const std::uintmax_t size = fs::file_size(nativePath(path), ec);
    if (ec) return false;

    std::uint64_t want = static_cast<std::uint64_t>(size);
    if (want > limit) {
        want = limit;
        truncated = true;
    }
    out.resize(static_cast<std::size_t>(want));
    if (want == 0) return true;

    stream.read(out.data(), static_cast<std::streamsize>(want));
    out.resize(static_cast<std::size_t>(stream.gcount()));
    return true;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t stop = text.find('\n', start);
        if (stop == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, stop - start));
        start = stop + 1;
    }
    return lines;
}

// Разбор строки журнала в запись. Никогда не бросает: незнакомая или битая
// строка — это `false` и причина, а не исключение из экрана «Отчёт».
bool parseRecordLine(std::string_view line, JournalRecord& out, std::string& problem) {
    if (line.empty()) return false;
    Value value;
    try {
        value = json::parse(line);
    } catch (const json::ParseError& error) {
        problem = std::string("строка не разобрана: ") + error.what();
        return false;
    }
    return recordFromJson(value, out, problem);
}

}  // namespace

// ---------------------------------------------------------------------------
// Время и идентификатор транзакции
// ---------------------------------------------------------------------------

std::int64_t nowUnixMillis() noexcept {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
}

std::int64_t nowUnixSeconds() noexcept {
    return floorDiv(nowUnixMillis(), kMillisPerSecond);
}

std::string formatTimestamp(std::int64_t unixMillis) {
    const std::int64_t seconds = floorDiv(unixMillis, kMillisPerSecond);
    std::tm parts{};
    if (!utcFields(seconds, parts)) return "0000-00-00T00:00:00.000Z";
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec,
                  static_cast<int>(floorMod(unixMillis, kMillisPerSecond)));
    return buffer;
}

std::string formatFileStamp(std::int64_t unixMillis) {
    const std::int64_t seconds = floorDiv(unixMillis, kMillisPerSecond);
    std::tm parts{};
    if (!utcFields(seconds, parts)) return kUnknownStamp;
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1,
                  parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return buffer;
}

std::string makeTxId(std::int64_t unixMillis, std::uint32_t pid, std::uint64_t counter) {
    return formatFileStamp(unixMillis) + "-" + std::to_string(pid) + "-" + padNumber(counter, 6);
}

bool isValidTxId(std::string_view txId) noexcept {
    // Грамматика makeTxId: 8 цифр, 'T', 6 цифр, '-', 1..10 цифр, '-', 1..10 цифр.
    if (txId.size() < 19 || txId.size() > 37) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        if (!isDigit(txId[i])) return false;
    }
    if (txId[8] != 'T') return false;
    for (std::size_t i = 9; i < 15; ++i) {
        if (!isDigit(txId[i])) return false;
    }
    if (txId[15] != '-') return false;

    std::size_t index = 16;
    std::size_t digits = 0;
    while (index < txId.size() && isDigit(txId[index])) {
        ++index;
        ++digits;
    }
    if (digits == 0 || digits > 10) return false;
    if (index >= txId.size() || txId[index] != '-') return false;
    ++index;

    digits = 0;
    while (index < txId.size() && isDigit(txId[index])) {
        ++index;
        ++digits;
    }
    return digits >= 1 && digits <= 10 && index == txId.size();
}

bool txIdStamp(std::string_view txId, std::string& stamp) noexcept {
    if (!isValidTxId(txId)) return false;
    stamp.assign(txId.substr(0, 15));
    return true;
}

// ---------------------------------------------------------------------------
// Состояния и статусы
// ---------------------------------------------------------------------------

const char* toString(TxState state) noexcept {
    return nameFromCode(state, kTxStateNames);
}

const char* toString(OpStatus status) noexcept {
    return nameFromCode(status, kOpStatusNames);
}

const char* toString(JournalEvent event) noexcept {
    return nameFromCode(event, kJournalEventNames);
}

bool txStateFromString(std::string_view text, TxState& out) noexcept {
    return codeFromName(text, kTxStateNames, out);
}

bool opStatusFromString(std::string_view text, OpStatus& out) noexcept {
    return codeFromName(text, kOpStatusNames, out);
}

bool journalEventFromString(std::string_view text, JournalEvent& out) noexcept {
    return codeFromName(text, kJournalEventNames, out);
}

bool isRemovableStatus(OpStatus status) noexcept {
    return status == OpStatus::Deleted || status == OpStatus::Trashed;
}

OpStatus statusForAction(core::PlanAction action, bool succeeded) noexcept {
    if (!succeeded) return OpStatus::Failed;
    switch (action) {
        case core::PlanAction::Delete:
            return OpStatus::Deleted;
        case core::PlanAction::Trash:
            return OpStatus::Trashed;
        case core::PlanAction::Keep:
            return OpStatus::Skipped;
        case core::PlanAction::SkipLocked:
            return OpStatus::Blocked;
    }
    return OpStatus::Failed;
}

// ---------------------------------------------------------------------------
// Запись журнала
// ---------------------------------------------------------------------------

Value toJson(const JournalRecord& record) {
    std::vector<std::pair<std::string, Value>> members;
    members.reserve(record.hasOperation ? 20 : 14);
    members.emplace_back("schema", Value(kJournalSchema));
    members.emplace_back("seq", Value(static_cast<double>(record.sequence)));
    members.emplace_back("ts", Value(static_cast<double>(record.atUnixMillis)));
    // Читаемое время рядом с числом: журнал открывают и глазами, когда разбирают
    // инцидент, и jq, когда строят отчёт (SPEC §8, FR-8).
    members.emplace_back("tsText", Value(formatTimestamp(record.atUnixMillis)));
    members.emplace_back("event", Value(std::string(toString(record.event))));
    members.emplace_back("txId", Value(record.txId));
    members.emplace_back("state", Value(std::string(toString(record.state))));

    if (record.hasOperation) {
        members.emplace_back("index", Value(static_cast<double>(record.operationIndex)));
        members.emplace_back("status", Value(std::string(toString(record.status))));
        members.emplace_back("action", Value(std::string(actionToken(record.action))));
        members.emplace_back("bytesFreed", Value(static_cast<double>(record.bytes)));
        members.emplace_back("durationMs", Value(static_cast<double>(record.durationMillis)));
        members.emplace_back("hresult", Value(static_cast<double>(record.hresult)));
        members.emplace_back("path", Value(record.path));
        members.emplace_back("undoRef", Value(record.undoRef));
    }
    members.emplace_back("detail", Value(record.detail));

    // Версия, PID, подтверждение dry-run и размеры плана — только у событий
    // транзакции: в записях операций они повторялись бы тысячи раз, не добавляя
    // ничего, кроме места в файле.
    if (record.event == JournalEvent::TxBegin || record.event == JournalEvent::TxEnd) {
        members.emplace_back("appVersion", Value(record.appVersion));
        members.emplace_back("pid", Value(static_cast<double>(record.pid)));
        members.emplace_back("dryRunAcknowledged", Value(record.dryRunAcknowledged));
        members.emplace_back("plannedOperations", Value(static_cast<double>(record.plannedOperations)));
        members.emplace_back("plannedBytes", Value(static_cast<double>(record.plannedBytes)));
        members.emplace_back("planSignature", Value(static_cast<double>(record.planSignature)));
    }
    return Value::object(std::move(members));
}

std::string formatRecord(const JournalRecord& record) {
    std::string line = toJson(record).dump();
    line += '\n';
    return line;
}

bool recordFromJson(const Value& value, JournalRecord& out, std::string& problem) {
    if (!value.isObject()) {
        problem = "запись журнала не объект";
        return false;
    }
    const std::int64_t schema = readNumber(value, "schema", 0);
    if (schema != kJournalSchema) {
        problem = "неизвестная схема журнала: " + std::to_string(schema);
        return false;
    }

    JournalRecord record;
    record.sequence = static_cast<std::uint64_t>(readNumber(value, "seq", 0));
    record.atUnixMillis = readNumber(value, "ts", 0);
    const std::string event = readString(value, "event");
    if (!journalEventFromString(event, record.event)) {
        problem = "неизвестное событие журнала: " + event;
        return false;
    }
    record.txId = readString(value, "txId");
    if (!record.txId.empty() && !isValidTxId(record.txId)) {
        problem = "повреждённый идентификатор транзакции: " + record.txId;
        return false;
    }
    if (!txStateFromString(readString(value, "state"), record.state)) {
        problem = "неизвестное состояние транзакции: " + readString(value, "state");
        return false;
    }

    if (hasKey(value, "index")) {
        record.hasOperation = true;
        record.operationIndex = static_cast<std::size_t>(readNumber(value, "index", 0));
        if (!opStatusFromString(readString(value, "status"), record.status)) {
            problem = "неизвестный статус операции: " + readString(value, "status");
            return false;
        }
        if (!actionFromToken(readString(value, "action"), record.action)) {
            problem = "неизвестное действие операции: " + readString(value, "action");
            return false;
        }
        record.bytes = static_cast<std::uint64_t>(readNumber(value, "bytesFreed", 0));
        record.durationMillis = readNumber(value, "durationMs", 0);
        record.hresult = readNumber(value, "hresult", 0);
        record.path = readString(value, "path");
        record.undoRef = readString(value, "undoRef");
    }
    record.detail = readString(value, "detail");
    record.appVersion = readString(value, "appVersion");
    record.pid = static_cast<std::uint32_t>(readNumber(value, "pid", 0));
    record.dryRunAcknowledged = readFlag(value, "dryRunAcknowledged");
    record.plannedOperations = static_cast<std::uint64_t>(readNumber(value, "plannedOperations", 0));
    record.plannedBytes = static_cast<std::uint64_t>(readNumber(value, "plannedBytes", 0));
    record.planSignature = static_cast<std::uint64_t>(readNumber(value, "planSignature", 0));

    out = std::move(record);
    return true;
}

// ---------------------------------------------------------------------------
// Снимок плана
// ---------------------------------------------------------------------------

std::string snapshotFileName(std::string_view txId) {
    return std::string(kSnapshotFilePrefix) + std::string(txId) + std::string(kSnapshotFileSuffix);
}

std::string serializeSnapshot(const SnapshotRequest& request, std::int64_t capturedAtUnixMillis) {
    // Тело снимка печатает core::plan — тот же код, что печатает план в CLI и
    // в отчёте. Своя копия полей означала бы, что журнал и отчёт когда-нибудь
    // разойдутся, а снимок нужен именно для сверки с отчётом.
    Value body;
    try {
        body = json::parse(core::snapshotToJson(request.snapshot));
    } catch (const json::ParseError&) {
        return {};
    }
    const std::int64_t captured = capturedAtUnixMillis != 0 ? capturedAtUnixMillis : nowUnixMillis();

    return Value::object({
                          {"journalSchema", Value(kJournalSchema)},
                          {"kind", Value("plan-snapshot")},
                          {"txId", Value(request.txId)},
                          {"pid", Value(static_cast<double>(request.pid))},
                          {"dryRunAcknowledged", Value(request.dryRunAcknowledged)},
                          {"capturedAtUnixMillis", Value(static_cast<double>(captured))},
                          {"snapshot", std::move(body)},
                      })
        .dump(2);
}

bool snapshotHeaderFromJson(const Value& value, SnapshotHeader& out, std::string& problem) {
    if (!value.isObject()) {
        problem = "снимок не объект";
        return false;
    }
    const std::int64_t schema = readNumber(value, "journalSchema", -1);
    if (schema != kJournalSchema) {
        problem = "снимок другой схемы журнала: " + std::to_string(schema);
        return false;
    }
    const Value* body = value.find("snapshot");
    if (body == nullptr || !body->isObject()) {
        problem = "в снимке нет тела плана";
        return false;
    }

    SnapshotHeader header;
    header.schema = kJournalSchema;
    header.txId = readString(value, "txId");
    header.appVersion = readString(*body, "appVersion");
    header.pid = static_cast<std::uint32_t>(readNumber(value, "pid", 0));
    header.createdAtUnix = readNumber(*body, "createdAtUnix", 0);
    header.capturedAtUnixMillis = readNumber(value, "capturedAtUnixMillis", 0);
    header.planSignature = static_cast<std::uint64_t>(readNumber(*body, "planSignature", 0));
    header.operationCount = static_cast<std::size_t>(readNumber(*body, "operationCount", 0));
    header.totalBytes = static_cast<std::uint64_t>(readNumber(*body, "totalBytes", 0));
    header.dryRunAcknowledged = readFlag(value, "dryRunAcknowledged");
    if (!isValidTxId(header.txId)) {
        problem = "в снимке некорректный идентификатор транзакции";
        return false;
    }
    out = std::move(header);
    return true;
}

// ---------------------------------------------------------------------------
// Итоги и сводки
// ---------------------------------------------------------------------------

std::size_t TxTotals::accounted() const noexcept {
    return deleted + trashed + skipped + blocked + cancelled + failed;
}

bool TxTotals::balanced() const noexcept {
    return accounted() == planned && unplanned == 0;
}

std::int64_t TransactionSummary::elapsedMillis() const noexcept {
    if (endedAtUnixMillis == 0 || startedAtUnixMillis == 0) return 0;
    const std::int64_t delta = endedAtUnixMillis - startedAtUnixMillis;
    return delta > 0 ? delta : 0;
}

std::string toString(const TransactionSummary& summary) {
    std::string out;
    out += "транзакция ";
    out += summary.txId.empty() ? "(без идентификатора)" : summary.txId;
    out += "  [";
    out += toString(summary.state);
    out += "]";
    out += summary.dryRunAcknowledged ? "  план подтверждён\n" : "  план НЕ подтверждён\n";
    out += "  версия " + (summary.appVersion.empty() ? std::string("неизвестна") : summary.appVersion);
    out += ", pid " + std::to_string(summary.pid);
    out += ", начало " + formatTimestamp(summary.startedAtUnixMillis);
    if (summary.endedAtUnixMillis != 0) out += ", конец " + formatTimestamp(summary.endedAtUnixMillis);
    out += ", длительность " + std::to_string(summary.elapsedMillis()) + " мс\n";
    if (!summary.snapshotFile.empty()) out += "  снимок: " + summary.snapshotFile + "\n";
    out += "  операций по плану " + std::to_string(summary.totals.planned) + ", результатов " +
           std::to_string(summary.totals.accounted());
    if (summary.totals.unplanned != 0) out += ", вне плана " + std::to_string(summary.totals.unplanned);
    out += "\n";
    out += "  удалено " + std::to_string(summary.totals.deleted) + " (" +
           core::formatBytes(summary.totals.bytesFreed) + "), в корзину " + std::to_string(summary.totals.trashed) +
           " (" + core::formatBytes(summary.totals.bytesInTrash) + "), пропущено " +
           std::to_string(summary.totals.skipped) + ", заблокировано " + std::to_string(summary.totals.blocked) +
           ", отменено " + std::to_string(summary.totals.cancelled) + ", ошибок " +
           std::to_string(summary.totals.failed) + "\n";
    if (!summary.detail.empty()) out += "  " + summary.detail + "\n";
    return out;
}

std::string toText(const std::vector<TransactionSummary>& summaries) {
    std::string out;
    for (const TransactionSummary& summary : summaries) out += toString(summary);
    if (out.empty()) out = "журнал пуст: очистка ещё не выполнялась\n";
    return out;
}

std::vector<TransactionSummary> summarize(const JournalScan& scan) {
    std::vector<TransactionSummary> summaries;
    for (const JournalRecord& record : scan.records) {
        if (record.txId.empty()) continue;  // служебная запись без транзакции

        TransactionSummary* target = nullptr;
        for (TransactionSummary& candidate : summaries) {
            if (candidate.txId == record.txId) {
                target = &candidate;
                break;
            }
        }
        // Запись без tx.begin (оборванный файл, чужой writer) — сводку всё равно
        // показываем: отсутствующая транзакция в отчёте хуже неполной сводки.
        if (target == nullptr) {
            summaries.push_back(TransactionSummary{});
            target = &summaries.back();
            target->txId = record.txId;
            target->state = TxState::Open;
            target->startedAtUnixMillis = record.atUnixMillis;
        }

        switch (record.event) {
            case JournalEvent::TxBegin:
                target->appVersion = record.appVersion;
                target->pid = record.pid;
                target->dryRunAcknowledged = record.dryRunAcknowledged;
                target->startedAtUnixMillis = record.atUnixMillis;
                target->state = TxState::Open;
                target->planSignature = record.planSignature;
                target->totals.planned = static_cast<std::size_t>(record.plannedOperations);
                target->snapshotFile = snapshotFileName(record.txId);
                break;
            case JournalEvent::Operation:
                switch (record.status) {
                    case OpStatus::Deleted:
                        ++target->totals.deleted;
                        target->totals.bytesFreed += record.bytes;
                        break;
                    case OpStatus::Trashed:
                        ++target->totals.trashed;
                        target->totals.bytesInTrash += record.bytes;
                        break;
                    case OpStatus::Skipped:
                        ++target->totals.skipped;
                        break;
                    case OpStatus::Blocked:
                        ++target->totals.blocked;
                        break;
                    case OpStatus::Cancelled:
                        ++target->totals.cancelled;
                        break;
                    case OpStatus::Failed:
                        ++target->totals.failed;
                        break;
                }
                break;
            case JournalEvent::TxEnd:
                target->state = record.state;
                target->endedAtUnixMillis = record.atUnixMillis;
                if (!record.detail.empty()) target->detail = record.detail;
                target->totals.durationMillis = target->elapsedMillis();
                break;
            case JournalEvent::Rotate:
            case JournalEvent::Note:
                break;
        }
    }
    return summaries;
}

std::vector<TransactionSummary> pendingTransactions(const JournalScan& scan) {
    std::vector<TransactionSummary> pending;
    for (TransactionSummary& summary : summarize(scan)) {
        if (summary.open()) pending.push_back(std::move(summary));
    }
    return pending;
}

// ---------------------------------------------------------------------------
// Чтение журнала
// ---------------------------------------------------------------------------

JournalScan TransactionJournal::scanFile(const std::string& path) {
    JournalScan scan;
    scan.file = path;
    scan.files.push_back(path);

    std::string text;
    bool truncated = false;
    if (!readFileLimited(path, kJournalScanMaxBytes, text, truncated)) {
        scan.problems.push_back("файл журнала не прочитан: " + path);
        return scan;
    }
    scan.fileBytes = static_cast<std::uint64_t>(text.size());
    scan.truncated = truncated;

    std::vector<std::string> lines = splitLines(text);
    if (truncated && !lines.empty()) {
        // Первая строка при обрезке по лимиту заведомо неполная: её не считаем
        // ни записью, ни «оборванным хвостом» — это следствие чтения, а не падения.
        lines.erase(lines.begin());
        scan.problems.push_back("файл журнала прочитан не полностью (предел " +
                                std::to_string(static_cast<unsigned long long>(kJournalScanMaxBytes)) +
                                " байт): " + path);
    }

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string& line = lines[index];
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;

        JournalRecord record;
        std::string problem;
        if (parseRecordLine(line, record, problem)) {
            scan.records.push_back(std::move(record));
            continue;
        }
        if (index + 1 == lines.size()) {
            // Последняя строка: обрыв при падении процесса — нормальная
            // ситуация, её теряем молча, но считаем.
            ++scan.tornTail;
            scan.problems.push_back("последняя строка журнала оборвана (" + problem + ")");
        } else {
            ++scan.damagedLines;
            scan.problems.push_back("строка " + std::to_string(index + 1) + " повреждена: " + problem);
        }
    }
    return scan;
}

std::vector<std::string> TransactionJournal::listArchivedFiles(const JournalOptions& options) {
    std::vector<std::string> files;
    if (options.rootDirectory.empty()) return files;

    std::error_code ec;
    fs::directory_iterator it(nativePath(options.rootDirectory), ec);
    if (ec) return files;

    const std::string current = kJournalFileName;
    for (const fs::directory_entry& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) continue;
        const fs::path name = entry.path().filename();
        if (nativeToUtf8(name) == current) continue;
        if (!nativeHasPrefix(name, kArchivedJournalPrefix) || !nativeHasSuffix(name, kArchivedJournalSuffix)) continue;
        files.push_back(joinPath(options.rootDirectory, nativeToUtf8(name)));
    }
    // Свежие первыми: имя архива начинается с метки времени фиксированной
    // длины, поэтому порядок имени и есть хронологический порядок.
    std::sort(files.begin(), files.end(), [](const std::string& left, const std::string& right) {
        return fileNameOf(left) > fileNameOf(right);
    });
    return files;
}

JournalScan TransactionJournal::scanCurrent(const JournalOptions& options) {
    JournalScan scan;
    if (options.rootDirectory.empty()) {
        // Без каталога читать нечего, а «journal.jsonl» относительно текущего
        // каталога процесса — это чужой файл, и трогать его нельзя.
        scan.problems.push_back("журнал выключен: каталог не задан");
        return scan;
    }

    std::vector<std::string> ordered;
    ordered.push_back(joinPath(options.rootDirectory, kJournalFileName));
    for (const std::string& file : listArchivedFiles(options)) ordered.push_back(file);

    bool first = true;
    for (const std::string& file : ordered) {
        JournalScan part = scanFile(file);
        if (first) {
            scan = std::move(part);
            first = false;
            continue;
        }
        scan.files.push_back(file);
        scan.records.insert(scan.records.end(), part.records.begin(), part.records.end());
        scan.tornTail += part.tornTail;
        scan.damagedLines += part.damagedLines;
        scan.problems.insert(scan.problems.end(), part.problems.begin(), part.problems.end());
        scan.truncated = scan.truncated || part.truncated;
    }
    return scan;
}

std::vector<TransactionSummary> TransactionJournal::readTransactions(const JournalOptions& options) {
    return summarize(scanCurrent(options));
}

// ---------------------------------------------------------------------------
// TransactionJournal: хранение файла, запись, снимок
// ---------------------------------------------------------------------------

struct TransactionJournal::Impl {
    JournalOptions options;
    mutable std::mutex mutex;

    std::ofstream stream;
    std::string currentFile;
    std::string snapshotDir;
    std::string lastError;
    std::uint64_t sequence{};
    std::uint64_t recordsWritten{};
    std::uint64_t failures{};
    std::uint64_t currentBytes{};

    std::string txId;
    bool txOpen{false};
    bool dryRunAcknowledged{false};
    std::uint64_t plannedBytes{0};  // размер плана из снимка: в tx.end остаётся планом
    std::int64_t txStartedAt{};
    std::uint64_t txCounter{};
    TxTotals totals;
    // candidateIndex каждой запланированной операции в порядке снимка: по нему
    // результат сопоставляется с планом, и «лишний» результат виден сразу.
    std::vector<std::size_t> planned;

    std::int64_t now() const {
        return options.nowMillis ? options.nowMillis() : nowUnixMillis();
    }

    void noteError(std::string message) {
        lastError = std::move(message);
        ++failures;
    }

    bool ensureDirectories() {
        if (options.rootDirectory.empty()) {
            noteError("каталог журнала не задан");
            return false;
        }
        snapshotDir = joinPath(options.rootDirectory, kSnapshotDirName);
        if (!options.createDirectories) return true;

        std::error_code ec;
        fs::create_directories(nativePath(snapshotDir), ec);
        if (ec) {
            noteError("каталог журнала не создан: " + snapshotDir + " (" + ec.message() + ")");
            return false;
        }
        return true;
    }

    bool ensureOpen() {
        if (stream.is_open()) return true;
        if (!options.enabled) {
            noteError("журнал выключен");
            return false;
        }
        if (!ensureDirectories()) return false;

        currentFile = joinPath(options.rootDirectory, kJournalFileName);
        stream.open(nativePath(currentFile), std::ios::binary | std::ios::app);
        if (!stream.is_open()) {
            noteError("файл журнала не открыт: " + currentFile);
            return false;
        }
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(nativePath(currentFile), ec);
        currentBytes = ec ? 0 : static_cast<std::uint64_t>(size);
        return true;
    }

    void closeStream() {
        if (stream.is_open()) {
            stream.flush();
            stream.close();
        }
    }

    std::string uniqueArchivePath() {
        const std::string stamp = padNumber(static_cast<std::uint64_t>(now()), kArchiveStampWidth);
        const std::string base =
            std::string(kArchivedJournalPrefix) + stamp + std::string(kArchivedJournalSuffix);
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const std::string name =
                attempt == 0 ? base : std::string(kArchivedJournalPrefix) + stamp + "-" +
                                        padNumber(static_cast<std::uint64_t>(attempt), 3) +
                                        std::string(kArchivedJournalSuffix);
            const std::string path = joinPath(options.rootDirectory, name);
            std::error_code ec;
            if (!fs::exists(nativePath(path), ec)) return path;
            // Две ротации в одну миллисекунду: имя обязано остаться уникальным,
            // иначе rename перезапишет чужой архив.
        }
        return {};
    }

    void pruneArchives() {
        if (options.keepFiles < 0) return;
        const std::vector<std::string> files = TransactionJournal::listArchivedFiles(options);
        const std::size_t keep = static_cast<std::size_t>(options.keepFiles);
        if (files.size() <= keep) return;
        for (std::size_t index = keep; index < files.size(); ++index) {
            std::error_code ec;
            fs::remove(nativePath(files[index]), ec);
        }
    }

    // Единственное место, где строка попадает в файл: счётчики и признак отказа
    // считаются здесь, а не в каждом вызывающем.
    bool writeLine(const std::string& line) {
        if (!ensureOpen()) return false;
        stream << line;
        if (!stream) {
            noteError("запись журнала не записана: " + currentFile);
            return false;
        }
        currentBytes += line.size();
        ++recordsWritten;
        if (options.flushEveryRecord) {
            // Сброс на каждой записи: недописанный буфер — это ровно те строки,
            // которые объясняли бы падение, а не «терять их» экономит ноль.
            stream.flush();
            if (!stream) noteError("сброс журнала не удался: " + currentFile);
        }
        return true;
    }

    bool rotate() {
        closeStream();
        if (currentBytes == 0) return ensureOpen();  // пустой журнал ротацию не заслужил

        const std::string archive = uniqueArchivePath();
        if (archive.empty()) {
            noteError("не удалось подобрать имя архива журнала");
            return false;
        }
        std::error_code ec;
        fs::rename(nativePath(currentFile), nativePath(archive), ec);
        if (ec) {
            noteError("журнал не переименован в архив: " + ec.message());
            return false;
        }
        currentBytes = 0;
        pruneArchives();
        if (!ensureOpen()) return false;

        JournalRecord note;
        note.event = JournalEvent::Rotate;
        note.sequence = ++sequence;
        note.atUnixMillis = now();
        note.detail = "файл ушёл в архив " + fileNameOf(archive);
        const bool written = writeLine(formatRecord(note));

        core::LogFields fields;
        fields.push_back(core::logField("archive", fileNameOf(archive)));
        fields.push_back(core::logField("records", recordsWritten));
        core::logInfo(kEventRotate, "журнал поратирован", fields);
        return written;
    }

    bool append(JournalRecord record) {
        record.sequence = ++sequence;
        if (record.atUnixMillis == 0) record.atUnixMillis = now();
        // Запись внутри открытой транзакции принадлежит ей: без txId сводка
        // развалилась бы на безымянные строки.
        if (record.txId.empty()) record.txId = txId;
        if (!ensureOpen()) return false;

        const std::string line = formatRecord(record);
        if (options.maxFileBytes != 0 && currentBytes != 0 && currentBytes + line.size() > options.maxFileBytes) {
            if (!rotate()) return false;
        }
        return writeLine(line);
    }

    void countStatus(OpStatus status, std::uint64_t bytes) {
        switch (status) {
            case OpStatus::Deleted:
                ++totals.deleted;
                totals.bytesFreed += bytes;
                break;
            case OpStatus::Trashed:
                ++totals.trashed;
                totals.bytesInTrash += bytes;
                break;
            case OpStatus::Skipped:
                ++totals.skipped;
                break;
            case OpStatus::Blocked:
                ++totals.blocked;
                break;
            case OpStatus::Cancelled:
                ++totals.cancelled;
                break;
            case OpStatus::Failed:
                ++totals.failed;
                break;
        }
    }

    // Позиция операции в снимке. Ищем по candidateIndex, а не по позиции: снимок
    // хранит операции в порядке кандидатов, и сопоставлять надо с кандидатом.
    std::size_t positionOf(std::size_t candidateIndex) const {
        for (std::size_t index = 0; index < planned.size(); ++index) {
            if (planned[index] == candidateIndex) return index;
        }
        return planned.size();
    }

    bool writeSnapshotLocked(const SnapshotRequest& request, SnapshotInfo& out);
    bool endTransaction(TxState state, std::string detail);
};

bool TransactionJournal::Impl::writeSnapshotLocked(const SnapshotRequest& request, SnapshotInfo& out) {
    if (!isValidTxId(request.txId)) {
        noteError("некорректный идентификатор транзакции для снимка: " + request.txId);
        return false;
    }
    if (!options.enabled) {
        noteError("журнал выключен: снимок плана не записан");
        return false;
    }
    if (!ensureDirectories()) return false;

    const std::string text = serializeSnapshot(request, now());
    if (text.empty()) {
        noteError("снимок плана не сериализуется");
        return false;
    }
    if (static_cast<std::uint64_t>(text.size()) > kSnapshotMaxBytes) {
        noteError("снимок плана больше предела " +
                  std::to_string(static_cast<unsigned long long>(kSnapshotMaxBytes)) + " байт");
        return false;
    }

    const std::string name = snapshotFileName(request.txId);
    const std::string path = joinPath(snapshotDir, name);
    const std::string temp = path + ".tmp";

    {
        // Временный файл + rename: снимок либо существует целиком, либо его нет.
        // Оборванный на середине снимок после падения означал бы, что согласие
        // пользователя подтверждено документом, который нельзя прочесть.
        std::ofstream file(nativePath(temp), std::ios::binary | std::ios::trunc);
        if (!file) {
            noteError("временный файл снимка не создан: " + temp);
            return false;
        }
        file << text;
        file.flush();
        const bool written = file.good();
        file.close();
        if (!written) {
            noteError("снимок плана не записан: " + temp);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(nativePath(temp), nativePath(path), ec);
    if (ec) {
        // rename поверх существующего файла на Windows отказывает, а повторный
        // снимок с тем же txId (перезапуск операции) — обычное дело.
        std::error_code removeEc;
        fs::remove(nativePath(path), removeEc);
        fs::rename(nativePath(temp), nativePath(path), ec);
    }
    if (ec) {
        noteError("снимок плана не сохранён: " + path + " (" + ec.message() + ")");
        return false;
    }

    SnapshotInfo info;
    info.txId = request.txId;
    info.fileName = name;
    info.path = path;
    info.operationCount = static_cast<std::uint32_t>(request.snapshot.operationCount);
    info.totalBytes = request.snapshot.totalBytes;
    info.planSignature = request.snapshot.planSignature;
    info.fileBytes = static_cast<std::uint64_t>(text.size());

    if (options.verifySnapshot) {
        // Обратное чтение: «запись в журнал» должна означать «файл есть и верен»,
        // иначе подтверждённый список операций ничем не подтверждён.
        std::string readBack;
        bool truncated = false;
        SnapshotHeader header;
        std::string problem;
        Value parsed;
        const bool read = readFileLimited(path, kSnapshotMaxBytes, readBack, truncated);
        if (!read) {
            problem = "снимок не перечитан";
        } else if (truncated) {
            problem = "снимок не помещается в предел чтения";
        } else {
            try {
                parsed = json::parse(readBack);
            } catch (const json::ParseError& error) {
                problem = error.what();
            }
        }
        if (problem.empty() && !snapshotHeaderFromJson(parsed, header, problem)) {
            // problem уже заполнен разбором шапки
        }
        if (!problem.empty() || header.txId != request.txId || header.operationCount != info.operationCount ||
            header.totalBytes != info.totalBytes || header.planSignature != info.planSignature) {
            noteError("снимок плана не сошёлся при проверке: " + path +
                      (problem.empty() ? std::string(" (счётчики не совпали)") : " (" + problem + ")"));
            return false;
        }
        info.verified = true;
    }

    out = std::move(info);

    core::LogFields fields;
    fields.push_back(core::logField("txId", request.txId));
    fields.push_back(core::logField("file", path));
    fields.push_back(core::logField("operations", static_cast<std::uint64_t>(request.snapshot.operationCount)));
    fields.push_back(core::logField("bytes", request.snapshot.totalBytes));
    fields.push_back(core::logField("dryRun", request.dryRunAcknowledged));
    fields.push_back(core::logField("verified", info.verified));
    core::logInfo(kEventSnapshot, "снимок плана записан", fields);
    return true;
}

bool TransactionJournal::Impl::endTransaction(TxState state, std::string detail) {
    if (!txOpen) {
        noteError("транзакция не открыта: закрытие не записано");
        return false;
    }

    const std::int64_t finished = now();
    totals.durationMillis = finished - txStartedAt;
    if (totals.durationMillis < 0) totals.durationMillis = 0;

    JournalRecord record;
    record.event = JournalEvent::TxEnd;
    record.txId = txId;
    record.state = state;
    record.atUnixMillis = finished;
    record.appVersion = options.appVersion;
    record.pid = options.pid;
    record.dryRunAcknowledged = dryRunAcknowledged;
    record.plannedOperations = static_cast<std::uint64_t>(totals.planned);
    // plannedBytes здесь — это план, а не результат: в tx.end он обязан остаться
    // тем же, что и в tx.begin, иначе «что обещали» и «что вышло» смешаются.
    record.plannedBytes = plannedBytes;
    record.detail = detail;
    if (!record.detail.empty()) {
        record.detail += "; учтено " + std::to_string(totals.accounted()) + "/" +
                         std::to_string(totals.planned) + " операций, освобождено " +
                         core::formatBytes(totals.bytesFreed);
    }
    const bool written = append(record);

    core::LogFields fields;
    fields.push_back(core::logField("txId", txId));
    fields.push_back(core::logField("state", std::string(toString(state))));
    fields.push_back(core::logField("planned", static_cast<std::uint64_t>(totals.planned)));
    fields.push_back(core::logField("accounted", static_cast<std::uint64_t>(totals.accounted())));
    fields.push_back(core::logField("deleted", static_cast<std::uint64_t>(totals.deleted)));
    fields.push_back(core::logField("trashed", static_cast<std::uint64_t>(totals.trashed)));
    fields.push_back(core::logField("failed", static_cast<std::uint64_t>(totals.failed)));
    fields.push_back(core::logField("cancelled", static_cast<std::uint64_t>(totals.cancelled)));
    fields.push_back(core::logField("unplanned", static_cast<std::uint64_t>(totals.unplanned)));
    fields.push_back(core::logField("bytesFreed", totals.bytesFreed));
    fields.push_back(core::logField("durationMs", totals.durationMillis));
    core::logInfo(kEventTxEnd, "транзакция закрыта", fields);

    txOpen = false;
    txId.clear();
    planned.clear();
    return written;
}

// ---- публичная часть класса ----

TransactionJournal::TransactionJournal() : impl_(std::make_unique<Impl>()) {}

TransactionJournal::TransactionJournal(JournalOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    if (impl_->options.rootDirectory.empty()) impl_->options.enabled = false;
}

TransactionJournal::~TransactionJournal() {
    if (!impl_) return;
    // Незакрытая транзакция — это обрыв: пишем честный tx.end, чтобы в отчёте
    // она не выглядела как «идёт прямо сейчас».
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->txOpen) impl_->endTransaction(TxState::Cancelled, "журнал закрыт без завершения транзакции");
    impl_->closeStream();
}

bool TransactionJournal::enabled() const noexcept {
    return impl_ && impl_->options.enabled && !impl_->options.rootDirectory.empty();
}

const JournalOptions& TransactionJournal::options() const noexcept {
    return impl_->options;
}

std::string TransactionJournal::rootDirectory() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->options.rootDirectory;
}

std::string TransactionJournal::currentFile() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->currentFile;
}

std::string TransactionJournal::snapshotDirectory() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshotDir;
}

std::uint64_t TransactionJournal::recordsWritten() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->recordsWritten;
}

std::uint64_t TransactionJournal::failures() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->failures;
}

std::string TransactionJournal::lastError() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lastError;
}

bool TransactionJournal::writeSnapshot(const SnapshotRequest& request, SnapshotInfo& out) {
    out = SnapshotInfo{};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) {
        impl_->noteError("журнал выключен: снимок плана не записан");
        return false;
    }
    return impl_->writeSnapshotLocked(request, out);
}

BeginResult TransactionJournal::beginTransaction(const core::PlanSnapshot& snapshot, bool dryRunAcknowledged) {
    BeginResult result;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) {
        impl_->noteError("журнал выключен: транзакция не начата");
        result.problem = "журнал выключен: транзакция не начата";
        return result;
    }
    if (impl_->txOpen) {
        result.problem = "транзакция уже открыта: " + impl_->txId;
        impl_->noteError(result.problem);
        return result;
    }

    const std::int64_t started = impl_->now();
    const std::string txId = makeTxId(started, impl_->options.pid, ++impl_->txCounter);

    SnapshotRequest request;
    request.txId = txId;
    request.snapshot = snapshot;
    request.dryRunAcknowledged = dryRunAcknowledged;
    request.pid = impl_->options.pid;

    SnapshotInfo info;
    // Снимок ПЕРВЫМ. Пока tx.begin не записан, удалять нечего начинать: список
    // операций, на который пользователь дал согласие, обязан существовать.
    if (!impl_->writeSnapshotLocked(request, info)) {
        result.problem = impl_->lastError.empty() ? std::string("снимок плана не записан") : impl_->lastError;
        return result;
    }

    impl_->planned.clear();
    impl_->planned.reserve(snapshot.operations.size());
    for (const core::PlanOperation& operation : snapshot.operations) {
        impl_->planned.push_back(operation.candidateIndex);
    }

    impl_->totals = TxTotals{};
    impl_->totals.planned = snapshot.operations.size();
    impl_->plannedBytes = snapshot.totalBytes;
    impl_->txId = txId;
    impl_->txStartedAt = started;
    impl_->dryRunAcknowledged = dryRunAcknowledged;
    impl_->txOpen = true;

    JournalRecord record;
    record.event = JournalEvent::TxBegin;
    record.txId = txId;
    record.state = TxState::Open;
    record.atUnixMillis = started;
    record.appVersion = impl_->options.appVersion;
    record.pid = impl_->options.pid;
    record.dryRunAcknowledged = dryRunAcknowledged;
    record.plannedOperations = static_cast<std::uint64_t>(snapshot.operations.size());
    record.plannedBytes = snapshot.totalBytes;
    record.planSignature = snapshot.planSignature;
    record.detail = "снимок " + info.fileName + ", подтверждение dry-run: " +
                    std::string(dryRunAcknowledged ? "да" : "нет");
    const bool written = impl_->append(record);

    core::LogFields fields;
    fields.push_back(core::logField("txId", txId));
    fields.push_back(core::logField("snapshot", info.fileName));
    fields.push_back(core::logField("operations", static_cast<std::uint64_t>(snapshot.operations.size())));
    fields.push_back(core::logField("bytes", snapshot.totalBytes));
    fields.push_back(core::logField("dryRun", dryRunAcknowledged));
    core::logInfo(kEventTxBegin, "транзакция начата", fields);

    result.txId = txId;
    result.snapshot = info;
    if (!written) {
        // Транзакцию, о начале которой не записано, продолжать незачем: журнал
        // разойдётся с тем, что лежит на диске, и отчёт станет ложью.
        result.problem = impl_->lastError;
        impl_->endTransaction(TxState::Failed, "запись о начале транзакции не удалась");
        return result;
    }
    result.ok = true;
    return result;
}

bool TransactionJournal::recordOperation(const core::PlanOperation& operation, OpStatus status, std::uint64_t bytesFreed,
                                         std::int64_t durationMillis, std::int64_t hresult, std::string detail,
                                         std::string undoRef) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) return false;
    if (!impl_->txOpen) {
        impl_->noteError("результат операции без открытой транзакции: " + operation.path);
        return false;
    }

    const std::size_t position = impl_->positionOf(operation.candidateIndex);
    const bool planned = position < impl_->planned.size();

    JournalRecord record;
    record.event = JournalEvent::Operation;
    record.txId = impl_->txId;
    record.state = TxState::Open;
    record.operationIndex = planned ? position : operation.candidateIndex;
    record.hasOperation = true;
    record.status = status;
    record.action = operation.action;
    record.bytes = isRemovableStatus(status) ? bytesFreed : 0;
    record.durationMillis = durationMillis;
    record.hresult = hresult;
    record.path = operation.path;
    record.undoRef = std::move(undoRef);
    record.detail = std::move(detail);
    if (!planned) {
        // Результат по операции, которой не было в снимке: это расхождение плана
        // и исполнения, и журнал обязан его назвать, а не молча учесть.
        ++impl_->totals.unplanned;
        record.detail += record.detail.empty() ? "операции нет в снимке плана" : " (операции нет в снимке плана)";
    }

    const bool written = impl_->append(record);
    impl_->countStatus(status, record.bytes);

    // В кольцевую ленту попадают только сбои и отмены: успешные операции
    // описывает журнал транзакции, а дублировать тысячу строк в ленту — значит
    // платить за это и в горячем пути, и глазами при разборе.
    if (status == OpStatus::Failed || status == OpStatus::Cancelled) {
        core::LogFields fields;
        fields.push_back(core::logField("txId", impl_->txId));
        fields.push_back(core::logField("index", static_cast<std::uint64_t>(record.operationIndex)));
        fields.push_back(core::logField("status", std::string(toString(status))));
        fields.push_back(core::logField("durationMs", durationMillis));
        core::logFailure(kEventOperation, "операция не удалась", operation.path, hresult, fields);
    }
    return written;
}

bool TransactionJournal::commit(std::string detail) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) return false;
    return impl_->endTransaction(TxState::Committed, std::move(detail));
}

bool TransactionJournal::cancel(std::string detail) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) return false;
    return impl_->endTransaction(TxState::Cancelled,
                                  detail.empty() ? "отменено пользователем" : std::move(detail));
}

bool TransactionJournal::fail(std::string detail) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) return false;
    return impl_->endTransaction(TxState::Failed, std::move(detail));
}

void TransactionJournal::writeNote(std::string event, std::string detail) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) return;
    JournalRecord record;
    record.event = JournalEvent::Note;
    record.atUnixMillis = impl_->now();
    record.txId = impl_->txOpen ? impl_->txId : std::string{};
    record.detail = detail.empty() ? std::move(event) : (event + ": " + std::move(detail));
    (void)impl_->append(record);
}

std::size_t TransactionJournal::pruneSnapshots() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!enabled()) return 0;
    if (!impl_->ensureDirectories()) return 0;

    const std::int64_t retention = impl_->options.snapshotRetentionSeconds;
    const std::string cutoffStamp =
        retention > 0 ? formatFileStamp(impl_->now() - retention * kMillisPerSecond) : std::string{};

    std::vector<fs::path> doomed;
    std::error_code ec;
    fs::directory_iterator it(nativePath(impl_->snapshotDir), ec);
    if (ec) return 0;

    for (const fs::directory_entry& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) continue;
        const fs::path native = entry.path().filename();
        const std::string name = nativeToUtf8(native);

        // Незаконченный временный файл — мусор после обрыва: снимком он не
        // является и стать им уже не может (имя с .tmp читатель не откроет).
        if (nativeHasPrefix(native, kSnapshotFilePrefix) && nativeHasSuffix(native, ".json.tmp")) {
            doomed.push_back(entry.path());
            continue;
        }
        if (!hasPrefix(name, kSnapshotFilePrefix) || !hasSuffix(name, kSnapshotFileSuffix)) continue;

        const std::string txId = name.substr(
            kSnapshotFilePrefix.size(), name.size() - kSnapshotFilePrefix.size() - kSnapshotFileSuffix.size());
        std::string stamp;
        if (!txIdStamp(txId, stamp)) continue;  // файл не наш — не трогаем
        if (!cutoffStamp.empty() && stamp >= cutoffStamp) continue;
        doomed.push_back(entry.path());
    }

    std::size_t removed = 0;
    for (const fs::path& path : doomed) {
        std::error_code removeEc;
        if (fs::remove(path, removeEc) && !removeEc) ++removed;
    }
    if (removed != 0) {
        core::LogFields fields;
        fields.push_back(core::logField("removed", static_cast<std::uint64_t>(removed)));
        fields.push_back(core::logField("cutoff", cutoffStamp.empty() ? std::string("нет") : cutoffStamp));
        core::logInfo(kEventPrune, "старые снимки удалены", fields);
    }
    return removed;
}

std::string TransactionJournal::currentTxId() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->txOpen ? impl_->txId : std::string{};
}

bool TransactionJournal::transactionOpen() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->txOpen;
}

TxTotals TransactionJournal::totals() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->totals;
}

}  // namespace mrproper::engine::journal
