#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <utility>

namespace mrproper::core {
namespace {

// Пределы одной записи. Без них сообщение с путём на 300 КБ или дампом
// содержимого файла разом выест лимит ротации и утащит за собой историю.
constexpr std::size_t EVENT_LIMIT = 128;
constexpr std::size_t MESSAGE_LIMIT = 4096;
constexpr std::size_t FIELD_KEY_LIMIT = 64;
constexpr std::size_t FIELD_VALUE_LIMIT = 1024;
constexpr std::size_t FIELD_COUNT_LIMIT = 64;

// Короткий номер потока вместо хеша std::thread::id: хеш потока помечен
// устаревшим в C++20, а UI и разбор лога всё равно нуждаются в «потоке 3»,
// а не в 64-битном числе.
std::uint64_t threadOrdinal() noexcept {
    static std::atomic<std::uint64_t> next{1};
    static thread_local const std::uint64_t ordinal = next.fetch_add(1, std::memory_order_relaxed);
    return ordinal;
}

std::int64_t nowEpochMillis() noexcept {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count();
}

// Обрезка с явной пометкой: молча пропавший хвост пути в логе хуже обрезанного.
std::string clip(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return std::string(text);
    char tail[48];
    std::snprintf(tail, sizeof(tail), "...[+%llu bytes]", static_cast<unsigned long long>(text.size() - limit));
    std::string out(text.substr(0, limit));
    out += tail;
    return out;
}

// ISO 8601 в UTC: «2026-02-18T10:11:12.345Z». Своя запись вместо
// std::put_time — тот тащит locale на каждый вызов и не умеет миллисекунды.
std::string formatTimestamp(std::int64_t epochMillis) {
    std::time_t seconds = static_cast<std::time_t>(epochMillis / 1000);
    auto millis = static_cast<int>(epochMillis % 1000);
    if (millis < 0) {  // для времени до эпохи округление уходит в минус
        millis += 1000;
        --seconds;
    }
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &seconds) != 0) return "1970-01-01T00:00:00.000Z";
#else
    if (gmtime_r(&seconds, &utc) == nullptr) return "1970-01-01T00:00:00.000Z";
#endif
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return buffer;
}

// Открытие файла на дозапись. Вариант fopen помечен в MSVC как небезопасный
// (C4996 при /W4 /WX), а fopen_s не существует вне CRT от Microsoft.
// Возвращает 0 при успехе, иначе код ошибки — он же уходит в fileError().
int openAppend(const char* path, std::FILE*& out) {
#if defined(_MSC_VER)
    out = nullptr;
    return static_cast<int>(fopen_s(&out, path, "ab"));
#else
    out = std::fopen(path, "ab");
    return out == nullptr ? errno : 0;
#endif
}

// Экранирование по правилам JSON. Байты >= 0x80 проходят как есть: файл
// объявлен UTF-8, а невалидную последовательность «съест» только просмотрщик.
void appendJsonString(std::string& out, std::string_view value) {
    static const char* kHexDigits = "0123456789ABCDEF";
    out += '"';
    for (const char symbol : value) {
        const auto code = static_cast<unsigned char>(symbol);
        switch (code) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (code < 0x20) {
                    out += "\\u00";
                    out += kHexDigits[code >> 4];
                    out += kHexDigits[code & 0x0F];
                } else {
                    out += symbol;
                }
                break;
        }
    }
    out += '"';
}

// Значение без кавычек, но только если это действительно JSON-скаляр.
// Страховка для asText == false: битое значение даст строку, а не испорченный файл.
void appendJsonScalar(std::string& out, const LogField& field) {
    const char first = field.value.empty() ? '\0' : field.value.front();
    const bool scalar = first == '-' || first == '+' || (first >= '0' && first <= '9') || first == 't' ||
                        first == 'f' || first == 'n';
    if (scalar) {
        if (field.value.empty()) {
            out += "null";
        } else {
            out += field.value;
        }
    } else {
        appendJsonString(out, field.value);
    }
}

LogFields clipFields(LogFields fields) {
    if (fields.size() > FIELD_COUNT_LIMIT) fields.resize(FIELD_COUNT_LIMIT);
    for (LogField& one : fields) {
        one.key = clip(one.key, FIELD_KEY_LIMIT);
        one.value = clip(one.value, FIELD_VALUE_LIMIT);
    }
    return fields;
}

void appendFields(std::string& out, const LogFields& fields) {
    if (fields.empty()) return;
    out += ",\"fields\":{";
    bool first = true;
    for (const LogField& one : fields) {
        if (!first) out += ',';
        first = false;
        appendJsonString(out, one.key);
        out += ':';
        if (one.asText) {
            appendJsonString(out, one.value);
        } else {
            appendJsonScalar(out, one);
        }
    }
    out += '}';
}

}  // namespace

const char* logLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Off:
            return "off";
    }
    return "info";
}

bool logLevelFromString(std::string_view text, LogLevel& out) noexcept {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char symbol) {
        return static_cast<char>(symbol >= 'A' && symbol <= 'Z' ? symbol - 'A' + 'a' : symbol);
    });
    if (lower == "trace") out = LogLevel::Trace;
    else if (lower == "debug") out = LogLevel::Debug;
    else if (lower == "info") out = LogLevel::Info;
    else if (lower == "warn" || lower == "warning") out = LogLevel::Warn;
    else if (lower == "error" || lower == "err") out = LogLevel::Error;
    else if (lower == "off" || lower == "none") out = LogLevel::Off;
    else return false;
    return true;
}

std::string toUtf8(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto unit = static_cast<std::uint32_t>(text[i]);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size()) {  // ведущий сурогат
            const auto low = static_cast<std::uint32_t>(text[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (unit >= 0xD800 && unit <= 0xDFFF) unit = 0xFFFD;  // непарный сурогат
        if (unit < 0x80) {
            out += static_cast<char>(unit);
        } else if (unit < 0x800) {
            out += static_cast<char>(0xC0 | (unit >> 6));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        } else if (unit < 0x10000) {
            out += static_cast<char>(0xE0 | (unit >> 12));
            out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (unit >> 18));
            out += static_cast<char>(0x80 | ((unit >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
    }
    return out;
}

LogField logField(std::string key, std::string value) {
    return LogField{std::move(key), std::move(value), true};
}

LogField logField(std::string key, std::string_view value) {
    return LogField{std::move(key), std::string(value), true};
}

LogField logField(std::string key, const char* value) {
    return logField(std::move(key), value == nullptr ? std::string_view{} : std::string_view(value));
}

LogField logField(std::string key, const std::wstring& value) {
    return LogField{std::move(key), toUtf8(value), true};
}

LogField logField(std::string key, const wchar_t* value) {
    const std::wstring_view view = value == nullptr ? std::wstring_view{} : std::wstring_view(value);
    return LogField{std::move(key), toUtf8(view), true};
}

std::string formatLogRecord(const LogRecord& record) {
    std::string line;
    line.reserve(record.event.size() + record.message.size() + 128);
    line += "{\"ts\":\"";
    line += formatTimestamp(record.epochMillis);
    line += "\",\"level\":\"";
    line += logLevelName(record.level);
    line += "\",\"tid\":";
    line += std::to_string(record.thread);
    line += ",\"seq\":";
    line += std::to_string(record.sequence);
    line += ",\"event\":";
    appendJsonString(line, record.event);
    line += ",\"msg\":";
    appendJsonString(line, record.message);
    appendFields(line, record.fields);
    line += '}';
    return line;
}

Logger& Logger::instance() noexcept {
    // «Утечка» намеренная: см. комментарий в log.hpp. Порядок разрушения
    // статических объектов не определён, а лог нужен и их деструкторам.
    static Logger* singleton = new Logger();
    return *singleton;
}

Logger::~Logger() {
    close();
}

void Logger::resetRingLocked() {
    const std::size_t capacity = options_.ringCapacity == 0 ? 1 : options_.ringCapacity;
    ring_.assign(capacity, LogRecord{});
    ringHead_ = 0;
    ringSize_ = 0;
}

void Logger::pushRingLocked(const LogRecord& record) {
    const std::size_t wanted = options_.ringCapacity == 0 ? 1 : options_.ringCapacity;
    if (ring_.size() != wanted) resetRingLocked();  // кольцо живо и до open()
    const std::size_t capacity = ring_.size();
    if (capacity == 0) return;
    if (ringSize_ == capacity) {  // вытесняем самую старую запись
        ring_[ringHead_] = record;
        ringHead_ = (ringHead_ + 1) % capacity;
        return;
    }
    ring_[(ringHead_ + ringSize_) % capacity] = record;
    ++ringSize_;
}

void Logger::noteFileErrorLocked(int code) {
    fileFailures_.fetch_add(1, std::memory_order_relaxed);
    fileError_ = code == 0 ? "unknown file error" : std::system_category().message(code);
}

std::string Logger::archivedPathLocked(int index) const {
    return path_ + "." + std::to_string(index);
}

std::size_t Logger::currentSizeLocked() {
    if (file_ == nullptr) return 0;
    if (std::fflush(file_) != 0) return fileBytes_;
    if (std::fseek(file_, 0, SEEK_END) != 0) return fileBytes_;
    const long position = std::ftell(file_);
    return position < 0 ? fileBytes_ : static_cast<std::size_t>(position);
}

bool Logger::rotateLocked() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (path_.empty()) return false;
    const int keep = options_.keepFiles;
    if (keep <= 0) {
        std::remove(path_.c_str());
    } else {
        std::error_code ignored;
        std::filesystem::remove(archivedPathLocked(keep), ignored);  // самый старый — в корзину
        for (int index = keep - 1; index >= 1; --index) {
            const std::string from = archivedPathLocked(index);
            if (std::filesystem::exists(from)) std::rename(from.c_str(), archivedPathLocked(index + 1).c_str());
        }
        std::rename(path_.c_str(), archivedPathLocked(1).c_str());
    }
    const int code = openAppend(path_.c_str(), file_);
    if (file_ == nullptr) {
        noteFileErrorLocked(code);
        return false;
    }
    fileBytes_ = currentSizeLocked();
    fileError_.clear();
    return true;
}

bool Logger::open(std::string path, const LogOptions& options) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
        fileError_.clear();
        options_ = options;
        resetRingLocked();
        if (path.empty()) {
            fileError_ = "empty log path";
            fileFailures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (options_.createDirectory) {
            const std::filesystem::path parent = std::filesystem::path(path).parent_path();
            if (!parent.empty()) {
                std::error_code ignored;  // точную причину сообщит fopen ниже
                std::filesystem::create_directories(parent, ignored);
            }
        }
        const int code = openAppend(path.c_str(), file_);
        if (file_ == nullptr) {
            noteFileErrorLocked(code);
            path_ = path;  // путь сохраняем: UI покажет, куда лог пытался писать
            return false;
        }
        path_ = std::move(path);
        fileBytes_ = currentSizeLocked();
        return true;
    } catch (...) {
        lost_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void Logger::close() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
        fileBytes_ = 0;
    } catch (...) {
        lost_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Logger::isOpen() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_ != nullptr;
}

std::string Logger::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

void Logger::setLevel(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    options_.level = level;
}

LogLevel Logger::level() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_.level;
}

void Logger::write(LogLevel level, std::string_view event, std::string_view message, LogFields fields) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level == LogLevel::Off || level > options_.level) {
            suppressed_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        LogRecord record;
        record.level = level;
        record.epochMillis = nowEpochMillis();
        record.thread = threadOrdinal();
        record.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        record.event = clip(event, EVENT_LIMIT);
        record.message = clip(message, MESSAGE_LIMIT);
        record.fields = clipFields(std::move(fields));
        pushRingLocked(record);  // кольцо получает запись всегда, даже без файла
        const std::string line = formatLogRecord(record);
        if (options_.echoToStderr) {
            std::fputs(line.c_str(), stderr);
            std::fputc('\n', stderr);
        }
        appendLineLocked(line);
    } catch (...) {
        // Память кончилась или мьютекс не сработал: лог не имеет права ронять
        // процесс, поэтому запись просто теряется и попадает в счётчик.
        lost_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::appendLineLocked(const std::string& line) {
    if (file_ == nullptr) return;
    if (options_.fileMaxBytes != 0 && fileBytes_ + line.size() > options_.fileMaxBytes) {
        if (!rotateLocked()) return;  // кольцо запись уже получило, файл — нет
    }
    if (std::fwrite(line.data(), 1, line.size(), file_) != line.size()) {
        noteFileErrorLocked(errno);
        return;
    }
    fileBytes_ += line.size();
    fileRecords_.fetch_add(1, std::memory_order_relaxed);
    // Сбрасываем на диск после каждой записи: лог должен пережить падение процесса,
    // иначе ошибки последних секунд потеряются вместе с буфером CRT.
    if (std::fflush(file_) != 0) noteFileErrorLocked(errno);
}

std::vector<LogRecord> Logger::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogRecord> out;
    if (ring_.empty()) return out;
    out.reserve(ringSize_);
    for (std::size_t i = 0; i < ringSize_; ++i) out.push_back(ring_[(ringHead_ + i) % ring_.size()]);
    return out;
}

std::size_t Logger::ringSize() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ringSize_;
}

std::size_t Logger::ringCapacity() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_.size();
}

std::uint64_t Logger::lastSequence() const noexcept {
    return sequence_.load(std::memory_order_relaxed);
}

void Logger::flush() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_ != nullptr) std::fflush(file_);
    } catch (...) {
        lost_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Logger::rotate() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return rotateLocked();
    } catch (...) {
        lost_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

std::uint64_t Logger::fileRecords() const noexcept {
    return fileRecords_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::fileFailures() const noexcept {
    return fileFailures_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::suppressed() const noexcept {
    return suppressed_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::lost() const noexcept {
    return lost_.load(std::memory_order_relaxed);
}

std::string Logger::fileError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fileError_;
}

void logTrace(std::string_view event, std::string_view message, LogFields fields) noexcept {
    Logger::instance().write(LogLevel::Trace, event, message, std::move(fields));
}

void logDebug(std::string_view event, std::string_view message, LogFields fields) noexcept {
    Logger::instance().write(LogLevel::Debug, event, message, std::move(fields));
}

void logInfo(std::string_view event, std::string_view message, LogFields fields) noexcept {
    Logger::instance().write(LogLevel::Info, event, message, std::move(fields));
}

void logWarn(std::string_view event, std::string_view message, LogFields fields) noexcept {
    Logger::instance().write(LogLevel::Warn, event, message, std::move(fields));
}

void logError(std::string_view event, std::string_view message, LogFields fields) noexcept {
    Logger::instance().write(LogLevel::Error, event, message, std::move(fields));
}

void logFailure(std::string_view event, std::string_view message, std::string_view path, std::int64_t hresult,
                LogFields fields) noexcept {
    // Путь и HRESULT — первые два поля: так их видно в любом просмотрщике
    // и в первой же строке выгрузки отчёта (SPEC §12).
    fields.insert(fields.begin(), {logField("path", path), logField("hr", hresult)});
    Logger::instance().write(LogLevel::Error, event, message, std::move(fields));
}

}  // namespace mrproper::core
