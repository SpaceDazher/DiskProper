// MrProper — структурный лог в файл и кольцевой буфер для UI (SPEC §6.2).
// Переносимый модуль: без Windows API и без зависимостей — ядро собирается и
// тестируется на любом хосте (SPEC §6.1, ADR-004). Поэтому и формат записи
// пишется здесь же, а не через core::json: в таблице модулей §6.2 у core::log
// в графе «Зависит от» стоит прочерк.
//
// Формат файла — JSON Lines: одна запись на строку, ключи стабильные
// ("ts", "level", "tid", "seq", "event", "msg", "fields"). Читается и глазами,
// и jq, и переживает падение процесса: строка дописывается целиком.
//
// Отказоустойчивость — главное свойство модуля: логирование не имеет права
// ронять процесс (SPEC §5, §12: «все ошибки в логе с путём и HRESULT»).
// Ни одна функция записи не бросает исключений, обрыв файла не теряет запись
// молча — она остаётся в кольце, а число отказов отдаёт Logger::fileFailures().
// Путь файла задаёт вызывающий (платформенный слой знает про %LOCALAPPDATA%).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mrproper::core {

// Уровни записи. Порог задаётся LogOptions::level; записи ниже порога
// отбрасываются целиком (в том числе в кольцо) и считаются в suppressed().
// По умолчанию Info: Trace/Debug в приложении с полумиллионом файлов не
// включаются, иначе лог станет дороже самой работы.
enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

// «trace»/«debug»/«info»/«warn»/«warning»/«error»/«off», регистр не важен.
const char* logLevelName(LogLevel level) noexcept;
bool logLevelFromString(std::string_view text, LogLevel& out) noexcept;

// Именованное поле записи. asText == false означает «значение уже приведено к
// JSON-скаляру»: числа и true/false пишутся без кавычек, остальное — строкой.
struct LogField {
    std::string key;
    std::string value;
    bool asText{true};
};

using LogFields = std::vector<LogField>;

LogField logField(std::string key, std::string value);
LogField logField(std::string key, std::string_view value);
LogField logField(std::string key, const char* value);

// Пути в модели — std::wstring (§6.3), лог же UTF-8: приводим здесь, чтобы
// вызывающий не вспоминал про кодировки в каждом месте с HRESULT.
std::string toUtf8(std::wstring_view text);
LogField logField(std::string key, const std::wstring& value);
LogField logField(std::string key, const wchar_t* value);

// Арифметические типы пишутся в JSON без кавычек: logField("hr", hresult).
// Числа с плавающей точкой печатаются с 10 значащими цифр — для лога достаточно.
template <typename T>
    requires std::is_arithmetic_v<T>
LogField logField(std::string key, T value) {
    LogField out;
    out.key = std::move(key);
    out.asText = false;
    char buffer[32];
    if constexpr (std::is_same_v<T, bool>) {
        out.value = value ? "true" : "false";
    } else if constexpr (std::is_floating_point_v<T>) {
        std::snprintf(buffer, sizeof(buffer), "%.10g", static_cast<double>(value));
        out.value = buffer;
    } else if constexpr (std::is_signed_v<T>) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        out.value = buffer;
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
        out.value = buffer;
    }
    return out;
}

// Одна запись лога. Неизменяемым её делает Logger: наружу отдаются копии
// (snapshot), а кольцо внутри перезаписывает записи по мере вытеснения.
struct LogRecord {
    LogLevel level{LogLevel::Info};
    std::int64_t epochMillis{};  // UTC: разбор лога не должен зависеть от TZ машины
    std::uint64_t thread{};      // короткий номер потока (1, 2, …)
    std::uint64_t sequence{};    // сквозной номер записи, начиная с 1
    std::string event;           // стабильный идентификатор: «scan.completed»
    std::string message;         // человекочитаемый текст
    LogFields fields;            // пары ключ/значение
};

struct LogOptions {
    LogLevel level{LogLevel::Info};
    std::size_t ringCapacity{512};                     // записей в кольце для UI
    std::size_t fileMaxBytes{4u * 1024u * 1024u};      // порог ротации, 0 — без неё
    int keepFiles{3};                                  // сколько прошлых файлов хранить
    bool createDirectory{true};                        // создать каталог файла, если нет
    bool echoToStderr{false};                          // дублировать в stderr (CLI, отладка)
};

// Г singleton приложения: файл + кольцо для UI. Потокобезопасен — писать могут
// рабочие потоки движка, читать кольцо — поток UI (SPEC §6.4).
class Logger {
public:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Процесс-одиночка. Экземпляр создаётся динамически и намеренно живёт до
    // конца программы: порядок разрушения статических объектов не определён,
    // а писать в лог будут и деструкторы других модулей.
    static Logger& instance() noexcept;

    // Открыть файл лога на дозапись (ротация выполняется по fileMaxBytes).
    // false — файл не открыт, причина в fileError(); кольцо при этом работает,
    // поэтому лог до инициализации файла не теряется.
    bool open(std::string path, const LogOptions& options = {});
    void close() noexcept;
    bool isOpen() const noexcept;
    std::string path() const;

    void setLevel(LogLevel level) noexcept;
    LogLevel level() const noexcept;

    // Основная запись. Никогда не бросает.
    void write(LogLevel level, std::string_view event, std::string_view message, LogFields fields = {}) noexcept;

    // Кольцо для UI: копия от самой старой записи к самой новой.
    std::vector<LogRecord> snapshot() const;
    std::size_t ringSize() const noexcept;
    std::size_t ringCapacity() const noexcept;
    // Сквозной номер последней записи: UI читает его, чтобы не перерисовывать
    // список, пока новых записей нет.
    std::uint64_t lastSequence() const noexcept;

    void flush() noexcept;
    // Принудительная ротация: например, перед экспортом отчёта или лога.
    bool rotate() noexcept;

    std::uint64_t fileRecords() const noexcept;   // успешно записано в файл
    std::uint64_t fileFailures() const noexcept;  // ошибки открытия/записи/ротации
    std::uint64_t suppressed() const noexcept;    // отброшено порогом уровня
    std::uint64_t lost() const noexcept;           // потеряно из-за сбоя памяти
    std::string fileError() const;                // текст последней ошибки, "" — ошибок нет

private:
    void resetRingLocked();
    void pushRingLocked(const LogRecord& record);
    void appendLineLocked(const std::string& line);
    void noteFileErrorLocked(int code);
    bool rotateLocked();
    std::size_t currentSizeLocked();
    std::string archivedPathLocked(int index) const;

    mutable std::mutex mutex_;
    std::FILE* file_{nullptr};
    std::string path_;
    std::string fileError_;
    std::size_t fileBytes_{0};
    LogOptions options_{};
    std::vector<LogRecord> ring_;  // фиксированный размер, кольцо на голову и хвост
    std::size_t ringHead_{0};
    std::size_t ringSize_{0};
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> fileRecords_{0};
    std::atomic<std::uint64_t> fileFailures_{0};
    std::atomic<std::uint64_t> suppressed_{0};
    std::atomic<std::uint64_t> lost_{0};
};

// Запись в виде строки JSON Lines — ровно то, что уходит в файл. Тот же
// формат использует UI при показе журнала, поэтому дублировать правила
// экранирования в интерфейсе не придётся.
std::string formatLogRecord(const LogRecord& record);

void logTrace(std::string_view event, std::string_view message, LogFields fields = {}) noexcept;
void logDebug(std::string_view event, std::string_view message, LogFields fields = {}) noexcept;
void logInfo(std::string_view event, std::string_view message, LogFields fields = {}) noexcept;
void logWarn(std::string_view event, std::string_view message, LogFields fields = {}) noexcept;
void logError(std::string_view event, std::string_view message, LogFields fields = {}) noexcept;

// Ошибка в принятой форме: путь и HRESULT добавляются полями сами (SPEC §12:
// «все ошибки в логе с путём и HRESULT»), чтобы вызывающий их не забывал.
void logFailure(std::string_view event, std::string_view message, std::string_view path, std::int64_t hresult,
                LogFields fields = {}) noexcept;

namespace detail {
// Пары «ключ, значение» в одну коллекцию полей. Нечётное число аргументов —
// ошибка компиляции, а не молча потерянное значение.
template <typename... Args>
LogFields logFieldList(Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0, "MRP_LOG_*: после сообщения идут пары «ключ, значение»");
    LogFields fields;
    fields.reserve(sizeof...(Args) / 2);
    (void)std::initializer_list<int>{(fields.push_back(logField(std::forward<Args>(args))), 0)...};
    return fields;
}
}  // namespace detail
}  // namespace mrproper::core

// Короткая форма для мест с парой-тройкой полей:
//     MRP_LOG_INFO("scan.done", "скан завершён", "candidates", n, "bytes", bytes);
//     MRP_LOG_ERROR("vfs.stat", "не удалось прочитать атрибуты", "path", pathUtf8, "hr", hr);
// Без макроса остаётся вызов с явно собранным списком полей — обе формы дают
// одну и ту же запись, макрос лишь убирает повторяющийся список.
#define MRP_LOG_TRACE(event, message, ...) \
    ::mrproper::core::logTrace((event), (message), ::mrproper::core::detail::logFieldList(__VA_ARGS__))
#define MRP_LOG_DEBUG(event, message, ...) \
    ::mrproper::core::logDebug((event), (message), ::mrproper::core::detail::logFieldList(__VA_ARGS__))
#define MRP_LOG_INFO(event, message, ...) \
    ::mrproper::core::logInfo((event), (message), ::mrproper::core::detail::logFieldList(__VA_ARGS__))
#define MRP_LOG_WARN(event, message, ...) \
    ::mrproper::core::logWarn((event), (message), ::mrproper::core::detail::logFieldList(__VA_ARGS__))
#define MRP_LOG_ERROR(event, message, ...) \
    ::mrproper::core::logError((event), (message), ::mrproper::core::detail::logFieldList(__VA_ARGS__))
// Ошибка с обязательными путём и HRESULT.
#define MRP_LOG_FAILURE(event, message, path, hresult, ...)                                                \
    ::mrproper::core::logFailure((event), (message), (path), (hresult),                                  \
                                 ::mrproper::core::detail::logFieldList(__VA_ARGS__))
