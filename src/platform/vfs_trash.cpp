// Реализация vfs_trash: единственный файл модуля, где встречается windows.h.
// Наружу (vfs_trash.hpp) уходят переносимые типы, чтобы engine, ui и cli не
// тащили windows.h в свои заголовки (SPEC §6.1, §6.2, ADR-004).
//
// Спека: §4 FR-7 (своя корзина, манифест, кросс-томовое копирование,
// восстановление с проверкой конфликтов), §9.1 ADR-005 (своя корзина вместо
// системной), §5 (длинные пути, пробелы, не-ASCII, hard links, ADS — поведение
// определено явно), §6.4 (отмена через std::stop_token, проверка каждые 256
// элементов), §12 (отказ виден в логе с путём и кодом Win32).
//
// Своих обёрток Win32 модуль не заводит: дескрипторы берёт win_handle.hpp
// (RAII, ADR-001), тексты ошибок и переводы UTF-8 ↔ UTF-16 — win_error.hpp.
// Вторая реализация того же разъелась бы с первой (та же мысль, что в
// devices.cpp и trim_cache.cpp).
//
// Три технических решения, которые стоит знать, прежде чем править файл.
//
// 1. Все пути нормализуются один раз, на входе (longPath), и дальше ходят в
//    Win32 в форме \\?\…: префикс снимает MAX_PATH и разбор точек, поэтому
//    кириллица, пробелы и путь длиннее 260 символов перестают быть отдельным
//    случаем. Обратно в отчёт и в манифест путь уходит без префикса.
//
// 2. Кросс-томовый перенос устроен как «сначала копия, потом удаление», а не
//    наоборот и не «переименовать, надеясь». Копия сорвалась — недокопированное
//    снимается, источник остаётся на месте, статус Cancelled или IoError, и
//    sourceIntact() у результата истинно. Исключение — момент после успешной
//    копии, когда источник начали сносить и упали посередине: тогда обе копии
//    стоят в мире, и результат говорит об этом полями status и sourceRemoved,
//    потому что откатом «украсть» у пользователя вторую копию хуже.
//
// 3. Reparse points не разворачиваются (FR-6). junction внутри каталога-кандидата
//    чаще всего уводит за пределы того, что пользователь собирался удалить, а
//    обход каталога по такой ссылке — это петля и выход за пределы пути. При
//    копировании такие точки пропускаются и считаются в skippedReparsePoints,
//    при удалении — снимаются как ссылки, без обхода.
#include "vfs_trash.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// windows.h идёт первым: из него берутся LARGE_INTEGER, WIN32_FILE_ATTRIBUTE_DATA,
// MOVEFILE_REPLACE_EXISTING и всё остальное, о чём объявления ниже.
// WIN32_LEAN_AND_MEAN и NOMINMAX приходят из профиля WIN32 (cmake/warnings.cmake).
#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место
#include <aclapi.h>   // GetNamedSecurityInfoW, SetNamedSecurityInfoW, GetDacl
#include <sddl.h>     // Convert*SecurityDescriptor*SecurityDescriptorW, SDDL_REVISION_1

#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform {
namespace {

// ---------------------------------------------------------------------------
// Константы формата и файлового обмена
// ---------------------------------------------------------------------------

// Порция чтения/записи при копировании. 1 МиБ — компромисс: крупнее не
// выигрывает заметно (диск всё равно отдаёт данные блоками), а мельче пришлось
// бы делать больше системных вызовов на каждый файл.
constexpr std::size_t kCopyChunkBytes = 1u << 20;

// Потолок манифеста при чтении. Манифест — это список элементов транзакции на
// десятки килобайт; 32 МиБ означает «файл не наш», и читать его дальше незачем
// (это всё равно не даст валидный core::TrashTransaction).
constexpr std::uintmax_t kMaxManifestBytes = 32u * 1024u * 1024u;

// Путь из переменной окружения короче 4 КиБ не бывает; 4096 — с запасом на
// будущий вызов с путём глубже, при этом стек не занимает килобайтами.
constexpr std::size_t kEnvironmentPathChars = 4096;

// Тик в 100 нс между 1601-01-01 и 1970-01-01. Разница между FILETIME и
// unix-секундами — это и есть константа, а не «сейчас минус 1970»: считать её
// из системных часов нельзя, иначе тест зависел бы от момента запуска.
constexpr std::int64_t kFileTimeUnixEpochDelta = 11644473600;

// Суффикс временного файла манифеста: рядом с настоящим, на том же томе, чтобы
// подстановка была атомарной.
constexpr const wchar_t* kManifestTempSuffix = L".tmp";

constexpr const wchar_t* kLongPrefix = L"\\\\?\\";
constexpr const wchar_t* kDevicePrefix = L"\\\\.\\";
constexpr const wchar_t* kUncPrefix = L"\\\\?\\UNC\\";

// ---------------------------------------------------------------------------
// Пути: нормализация и разбор
// ---------------------------------------------------------------------------

std::wstring toWide(std::string_view text) { return toUtf16(text); }
std::string toNarrow(std::wstring_view text) { return toUtf8(text); }

// Windows-пути пишутся в лог и в манифест без префикса \\?\: он является
// деталью вызова WinAPI, а не частью пути, который видит пользователь.
std::string plainOf(std::wstring_view wide) {
    std::wstring text(wide);
    if (text.rfind(kUncPrefix, 0) == 0) {
        text = L"\\\\" + text.substr(std::wstring_view(kUncPrefix).size());
    } else if (text.rfind(kLongPrefix, 0) == 0) {
        text = text.substr(std::wstring_view(kLongPrefix).size());
    }
    return toNarrow(text);
}

// Склеивание «каталог + имя» одним обратным слэшем. Имя элемента корзины уже
// проверено core::isValidPayloadName, а каталог приходит из проверенного txId,
// поэтому разделитель-в-имени сюда попасть не может.
std::wstring joinWide(const std::wstring& dir, std::wstring_view leaf) {
    std::wstring result(dir);
    if (!result.empty() && result.back() != L'\\' && result.back() != L':') {
        result.push_back(L'\\');
    }
    result.append(leaf);
    return result;
}

bool isSeparator(wchar_t symbol) { return symbol == L'\\' || symbol == L'/'; }

// Сколько символов в начале пути занимает корень (том): столько префиксов не
// создаётся. Для \\?\C:\ProgramData это 6 («\\?\C:» уже есть), для
// \\?\UNC\server\share\x — конец имени шары, для \\?\Volume{…}\x — конец метки
// тома. Относительный путь даёт 0: такой разбирается от первого символа.
std::size_t rootLength(const std::wstring& path) {
    if (path.rfind(kUncPrefix, 0) == 0) {
        const std::size_t serverEnd = path.find(L'\\', std::wstring_view(kUncPrefix).size());
        if (serverEnd == std::wstring::npos) return path.size();
        const std::size_t shareEnd = path.find(L'\\', serverEnd + 1);
        if (shareEnd == std::wstring::npos) return path.size();
        return shareEnd + 1;
    }
    if (path.rfind(kLongPrefix, 0) == 0 || path.rfind(kDevicePrefix, 0) == 0) {
        const std::size_t volumeEnd = path.find(L'\\', std::wstring_view(kLongPrefix).size());
        if (volumeEnd == std::wstring::npos) return path.size();
        return volumeEnd + 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Время
// ---------------------------------------------------------------------------

// Секунды и 100-нс тики делятся раздельно: если делить разность одним
// выражением, даты до 1970 года дают отрицательный остаток и ошибку на целую
// секунду (именно на них и спотыкаются юнит-тесты с фикстурами).
std::int64_t fileTimeToUnix(FILETIME time) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    return static_cast<std::int64_t>(ticks.QuadPart / 10000000ull) - kFileTimeUnixEpochDelta;
}

FILETIME unixToFileTime(std::int64_t unixSeconds) {
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<unsigned long long>((unixSeconds + kFileTimeUnixEpochDelta) * 10000000);
    FILETIME time{};
    time.dwLowDateTime = ticks.LowPart;
    time.dwHighDateTime = ticks.HighPart;
    return time;
}

// ---------------------------------------------------------------------------
// Коды Win32 → TrashStatus
// ---------------------------------------------------------------------------

TrashStatus classify(DWORD error) {
    switch (error) {
        case ERROR_SUCCESS:
            return TrashStatus::Ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NOT_FOUND:
        case ERROR_NO_MORE_FILES:
            return TrashStatus::NotFound;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
        case ERROR_DIR_NOT_EMPTY:  // место занято содержимым: решение за вызывающим
            return TrashStatus::AlreadyExists;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE:
            return TrashStatus::AccessDenied;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return TrashStatus::OutOfSpace;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_CALL_NOT_IMPLEMENTED:
        case ERROR_INVALID_PARAMETER:  // неверный путь или флаг — ошибка вызывающего
            return TrashStatus::Unsupported;
        default:
            return TrashStatus::IoError;
    }
}

TrashStatus classifyLastError() { return classify(::GetLastError()); }

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------

constexpr const char* kLogMove = "trash.move";
constexpr const char* kLogCopy = "trash.move.copy";
constexpr const char* kLogRestore = "trash.restore";
constexpr const char* kLogPurge = "trash.purge";
constexpr const char* kLogManifest = "trash.manifest";
constexpr const char* kLogScan = "trash.scan";

// Поля собираются списком явно, а не макросом MRP_LOG_*: макрос разворачивает
// пакет «ключ, значение» в один вызов logField, и на двух парах полей это
// перестаёт компилироваться (C2661). Здесь пар больше двух всегда.
void logTrashFailure(const char* event, std::string_view message, std::string_view pathUtf8, DWORD error,
                     const TrashOptions& options) {
    if (!options.logFailures) return;

    core::LogFields fields;
    fields.push_back(core::logField("path", std::string(pathUtf8)));
    fields.push_back(core::logField("win32", static_cast<std::uint64_t>(error)));
    fields.push_back(core::logField("errorText", win32ErrorText(error)));
    core::logError(event, message, std::move(fields));
}

// Отмена пишется отдельно от отказа: в отчёте об очистке «пользователь нажал
// Отмена» и «нет доступа» — разные события, и смешивать их нельзя.
void logTrashCancelled(const char* event, std::string_view pathUtf8, const TrashOptions& options) {
    if (!options.logFailures) return;

    core::LogFields fields;
    fields.push_back(core::logField("path", std::string(pathUtf8)));
    core::logWarn(event, "операция прервана по требованию пользователя", std::move(fields));
}

// ---------------------------------------------------------------------------
// Атрибуты и каталоги
// ---------------------------------------------------------------------------

bool attributesOf(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data) {
    return ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != FALSE;
}

bool directoryExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool exists(const std::wstring& path) { return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool isReparse(DWORD attributes) { return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
bool isReadOnly(DWORD attributes) { return (attributes & FILE_ATTRIBUTE_READONLY) != 0; }

// Оба структурных типа атрибутов (WIN32_FILE_ATTRIBUTE_DATA и WIN32_FIND_DATAW)
// несут размер одинаково, поэтому помощник один на оба: обходы пользуются вторым
// (FindNextFileW требует именно его), а одиночный замер — первым.
template <class FileData>
std::uint64_t sizeOf(const FileData& data) {
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | static_cast<std::uint64_t>(data.nFileSizeLow);
}

// Снятие признака «только чтение». Нужно перед удалением: DeleteFileW на
// read-only файле возвращает ACCESS_DENIED, и мусор, который нельзя удалить без
// повышения, — это ровно тот случай, ради которого в утилите есть повышение при
// старте (§5).
bool clearReadOnly(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !isReadOnly(attributes)) return true;
    return ::SetFileAttributesW(path.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) != FALSE;
}

// Создать каталог вместе со всеми недостающими родителями. Существующий каталог —
// успех: повторный запуск не должен считаться ошибкой. Объявлена здесь, а не
// рядом с разбором пути, потому что опирается на directoryExists и classify.
TrashStatus ensureDirectoryTreeW(const std::wstring& path) {
    if (path.empty()) return TrashStatus::InvalidArgument;

    const std::size_t start = rootLength(path);
    for (std::size_t index = start; index <= path.size(); ++index) {
        const bool atEnd = index == path.size();
        if (!atEnd && !isSeparator(path[index])) continue;

        std::wstring prefix = path.substr(0, index);
        // Лишний слэш на конце префикса не создаёт каталога: "\\?\C:\ProgramData\"
        // и "\\?\C:\ProgramData" — одно и то же, но первое на границе цикла
        // выглядит как имя с хвостовым разделителем.
        while (prefix.size() > 4 && prefix.back() == L'\\') {
            prefix.pop_back();
        }
        if (prefix.size() <= 4) continue;  // «\\?\C:» и короче — это корень

        if (::CreateDirectoryW(prefix.c_str(), nullptr) != FALSE) continue;
        const DWORD error = ::GetLastError();
        if (error == ERROR_ALREADY_EXISTS && directoryExists(prefix)) continue;
        return classify(error);
    }
    return directoryExists(path) ? TrashStatus::Ok : TrashStatus::NotFound;
}

// Обход каталога. Единственный владелец дескриптора поиска — unique_handle из
// win_handle.hpp, поэтому «забыл FindClose» здесь невозможен, а ранний выход по
// отмене или отказу всё равно закрывает поиск.
class DirScan {
public:
    explicit DirScan(const std::wstring& directory) {
        const std::wstring pattern = joinWide(directory, L"*");
        handle_ = adopt<FindHandlePolicy>(::FindFirstFileW(pattern.c_str(), &data_));
    }

    DirScan(const DirScan&) = delete;
    DirScan& operator=(const DirScan&) = delete;
    DirScan(DirScan&&) = delete;
    DirScan& operator=(DirScan&&) = delete;

    [[nodiscard]] bool valid() const { return handle_.valid(); }

    // Следующая запись, кроме «.» и «..». Имя без пути: склейку делает вызывающий,
    // потому что у него есть и родительский каталог.
    [[nodiscard]] bool next(std::wstring& name) {
        while (handle_.valid()) {
            if (::FindNextFileW(handle_.get(), &data_) == FALSE) return false;
            if (isSkippedEntry(data_.cFileName)) continue;
            name.assign(data_.cFileName);
            return true;
        }
        return false;
    }

    // Действителен до следующего next(): FindNextFileW перезаписывает запись.
    [[nodiscard]] const WIN32_FIND_DATAW& current() const noexcept { return data_; }

private:
    // Точки и две точки FindFirstFile возвращает всегда: пропуск их здесь
    // избавляет от проверки «не выйти из каталога» в каждом обходе отдельно.
    static bool isSkippedEntry(const wchar_t* name) {
        if (name == nullptr) return true;
        if (name[0] != L'.') return false;
        return name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0');
    }

    unique_handle<FindHandlePolicy> handle_{};
    WIN32_FIND_DATAW data_{};
};

// ---------------------------------------------------------------------------
// Тома
// ---------------------------------------------------------------------------

// GetVolumePathNameW и GetVolumeNameForVolumeMountPointW — функции Win32: они
// понимают MAX_PATH, но не префикс \\?\: для них путь сначала раздевается.
std::wstring withoutLongPrefix(const std::wstring& path) {
    if (path.rfind(kUncPrefix, 0) == 0) return L"\\\\" + path.substr(std::wstring_view(kUncPrefix).size());
    if (path.rfind(kLongPrefix, 0) == 0) return path.substr(std::wstring_view(kLongPrefix).size());
    return path;
}

// «\\?\Volume{…}\» для тома, где лежит путь. Пусто, если том определить не
// удалось: у сетевого пути без смонтированного тома и у пути на устройство это
// ERROR_PATH_NOT_FOUND, и это нормальный ответ, а не повод падать.
std::wstring volumeGuidOf(const std::wstring& path) {
    const std::wstring plain = withoutLongPrefix(path);

    std::wstring mount(MAX_PATH, L'\0');
    const DWORD needed = ::GetVolumePathNameW(plain.c_str(), &mount.front(), static_cast<DWORD>(mount.size()));
    if (needed == 0) return {};
    mount.resize(needed < mount.size() ? needed : mount.size() - 1);

    std::wstring guid(64, L'\0');
    const DWORD guidLength =
        ::GetVolumeNameForVolumeMountPointW(mount.c_str(), &guid.front(), static_cast<DWORD>(guid.size()));
    if (guidLength == 0 || guidLength >= guid.size()) return {};
    guid.resize(guidLength);
    return guid;
}

// ---------------------------------------------------------------------------
// Отмена и прогресс
// ---------------------------------------------------------------------------

// Счётчик переноса: прогресс для UI, учёт байтов и файлов, проверка отмены.
// Живёт один на операцию, поэтому «проверять отмену каждые 256 элементов» (§6.4)
// считается здесь, а не дублируется в каждой функции обхода.
struct Transfer {
    const TrashOptions* options{};
    TrashProgress progress{};
    std::size_t ticksLeft{1};
    std::uint32_t reparseSkipped{0};

    // true — пора прекращать работу. Проверка идёт по счётчику, а не на каждом
    // элементе: на каталоге в миллион файлов проверка stop_requested() в каждом
    // элементе сама по себе съедала бы заметную долю времени обхода.
    [[nodiscard]] bool cancelRequested() {
        if (options == nullptr) return false;
        if (ticksLeft == 0) {
            const std::size_t every = options->cancelCheckEvery != 0 ? options->cancelCheckEvery : kTrashCancelCheckEvery;
            ticksLeft = every;
            if (options->stop.stop_requested()) return true;
        }
        --ticksLeft;
        return false;
    }

    void addBytes(std::uint64_t bytes) {
        progress.bytesDone += bytes;
        publish();
    }

    void countFile() {
        ++progress.filesDone;
        publish();
    }

    // Размер известен заранее (его дал readPathFacts), а число файлов растёт по
    // ходу: без итогового размера доля завершения прыгала бы с нуля на каждом
    // файле и показывала «0 %» всё время копирования.
    void setTotals(std::uint64_t bytesTotal, std::uint32_t filesTotal) {
        progress.bytesTotal = bytesTotal;
        progress.filesTotal = filesTotal;
        publish();
    }

    void publish() {
        if (options != nullptr && options->progress) {
            options->progress(progress);
        }
    }
};

// ---------------------------------------------------------------------------
// Копирование
// ---------------------------------------------------------------------------

// Один файл целиком — своими руками, а не CopyFileExW, потому что нужны три
// вещи, которых у готовой функции нет: побайтовый прогресс для UI, проверка
// отмены внутри многосотбайтового файла и отчёт о том, сколько действительно
// скопировано (отчёт о свободном месте не должен врать).
TrashStatus copyFileBytes(const std::wstring& source, const std::wstring& destination, Transfer& transfer,
                          std::uint64_t& copiedBytes) {
    const auto input = adopt<KernelHandlePolicy>(
        ::CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input.valid()) return classifyLastError();

    // Заметка про перезапись: решение принимает вызывающий, и до сюда оно уже
    // применено (прежнее содержимое снято), поэтому CREATE_ALWAYS честен: файл
    // назначения на этом шаге либо не существует, либо только что удалён.
    const auto output = adopt<KernelHandlePolicy>(::CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                                                 nullptr));
    if (!output.valid()) return classifyLastError();

    // Время исходника берём с его же дескриптора: открывать файл второй раз
    // ради двух атрибутов незачем.
    FILETIME access{};
    FILETIME modified{};
    FILETIME created{};
    const bool haveTimes = ::GetFileTime(input.get(), &access, &modified, &created) != FALSE;

    std::vector<char> buffer;
    try {
        buffer.resize(kCopyChunkBytes);
    } catch (const std::bad_alloc&) {
        return TrashStatus::OutOfMemory;
    }

    for (;;) {
        if (transfer.cancelRequested()) return TrashStatus::Cancelled;

        DWORD read = 0;
        if (::ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE) {
            const DWORD error = ::GetLastError();
            return error == ERROR_HANDLE_EOF ? TrashStatus::Ok : classify(error);
        }
        if (read == 0) break;

        DWORD written = 0;
        if (::WriteFile(output.get(), buffer.data(), read, &written, nullptr) == FALSE) return classifyLastError();
        if (written != read) return classifyLastError();  // короткая запись: диск полон или сбой

        copiedBytes += written;
        transfer.addBytes(written);
    }

    if (haveTimes && (transfer.options == nullptr || transfer.options->applyTimestamps)) {
        (void)::SetFileTime(output.get(), &access, &modified, &created);
    }
    return TrashStatus::Ok;
}

// Кадр обхода: относительный путь внутри дерева, полный путь, время изменения
// каталога и признак «дети уже разложены». Стек явный, а не рекурсия: дерево
// глубиной в тысячи уровней при рекурсивном обходе положило бы стек процесса,
// а SPEC §5 требует, чтобы ни один отказ не ронял приложение.
struct DirFrame {
    std::wstring rel;   // путь внутри дерева относительно корня обхода; "" — корень
    std::wstring path;  // полный путь
    FILETIME modified{};
    bool haveModified{false};
    bool expanded{false};

    [[nodiscard]] bool isRoot() const { return rel.empty(); }
};

TrashStatus applyTimestamp(const std::wstring& path, FILETIME modified) {
    // FILE_FLAG_BACKUP_SEMANTICS обязателен: без него SetFileTime не открывает
    // каталог даже с FILE_WRITE_ATTRIBUTES.
    const auto handle = adopt<KernelHandlePolicy>(
        ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.valid()) return classifyLastError();
    return ::SetFileTime(handle.get(), nullptr, &modified, nullptr) != FALSE ? TrashStatus::Ok : classifyLastError();
}

// Копирование дерева каталога. Порядок принципиален: сначала содержимое, и только
// потом (по завершении кадра) время каталога — иначе выставленное заранее время
// затиралось бы созданием содержимого.
TrashStatus copyDirectoryTree(const std::wstring& source, const std::wstring& destination, Transfer& transfer) {
    std::vector<DirFrame> stack;
    stack.push_back(DirFrame{std::wstring(), source, {}, false, false});

    while (!stack.empty()) {
        if (transfer.cancelRequested()) return TrashStatus::Cancelled;

        DirFrame& frame = stack.back();
        if (!frame.expanded) {
            frame.expanded = true;
            // Копии полей, а не ссылка на кадр: push_back ниже перевыделит память,
            // и ссылка stack.back() после этого указывала бы в освобождённое.
            const std::wstring current = frame.path;
            const std::wstring relBase = frame.rel;
            const std::wstring target = frame.isRoot() ? destination : joinWide(destination, relBase);

            if (!directoryExists(target)) {
                if (::CreateDirectoryW(target.c_str(), nullptr) == FALSE) {
                    const DWORD error = ::GetLastError();
                    if (error != ERROR_ALREADY_EXISTS) return classify(error);
                }
            }

            if (transfer.options == nullptr || transfer.options->applyTimestamps) {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (attributesOf(current, data)) {
                    stack.back().modified = data.ftLastWriteTime;
                    stack.back().haveModified = true;
                }
            }

            DirScan scan(current);
            if (!scan.valid()) {
                const DWORD error = ::GetLastError();
                // Нечитаемый каталог — не повод бросать копирование всего дерева:
                // источник ещё не тронут (его сносят только после успешной копии),
                // а вызывающий получит отказ с путём.
                if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) return classify(error);
            } else {
                std::wstring name;
                while (scan.next(name)) {
                    const WIN32_FIND_DATAW& data = scan.current();
                    const std::wstring child = joinWide(current, name);
                    const std::wstring childRel = joinWide(relBase, name);
                    const std::wstring childTarget = joinWide(target, name);

                    if (isReparse(data.dwFileAttributes)) {
                        ++transfer.reparseSkipped;  // FR-6: не разворачиваем
                        continue;
                    }
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        stack.push_back(DirFrame{childRel, child, {}, false, false});
                        continue;
                    }
                    std::uint64_t copied = 0;
                    const TrashStatus status = copyFileBytes(child, childTarget, transfer, copied);
                    if (status != TrashStatus::Ok) return status;
                    transfer.countFile();
                }
            }
            continue;
        }

        // Кадр разложен целиком: восстанавливаем время каталога и снимаем кадр.
        // Порядок обратный (сначала дети, потом родитель) — так mtime каталога не
        // затирается последующей записью в него.
        if (frame.haveModified) {
            const std::wstring target = frame.isRoot() ? destination : joinWide(destination, frame.rel);
            (void)applyTimestamp(target, frame.modified);  // отказ здесь не фатален: данные уже скопированы
        }
        stack.pop_back();
    }
    return TrashStatus::Ok;
}

// ---------------------------------------------------------------------------
// Обход с удалением и обход с измерением
// ---------------------------------------------------------------------------

// Один обход, два режима. Отдельные функции читались бы понятнее, но обход —
// это почти весь код, и различие между «считать» и «снимать» не требует
// своей копии: измерение просто ничего не удаляет.
TrashStatus walkTree(const std::wstring& root, const TrashOptions& options, bool remove, TrashPurgeResult& result) {
    // Корень обхода — не обязательно каталог: элемент транзакции может быть
    // файлом, а очистка корзины обязана уметь снять и его.
    WIN32_FILE_ATTRIBUTE_DATA rootData{};
    if (!attributesOf(root, rootData)) {
        result.status = classifyLastError();
        if (result.status == TrashStatus::NotFound) result.notFound = true;
        return result.status;
    }
    if ((rootData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result.removedBytes = sizeOf(rootData);
        ++result.removedFiles;
        if (!remove) return TrashStatus::Ok;

        const std::wstring rootPath = root;
        if (isReadOnly(rootData.dwFileAttributes)) (void)clearReadOnly(rootPath);
        if (::DeleteFileW(rootPath.c_str()) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                result.status = TrashStatus::NotFound;
                result.notFound = true;
                return result.status;
            }
            result.status = classify(error);
            result.win32Error = error;
            result.failedPath = plainOf(rootPath);
            return result.status;
        }
        return TrashStatus::Ok;
    }

    std::vector<DirFrame> stack;
    stack.push_back(DirFrame{std::wstring(), root, {}, false, false});
    std::size_t ticksLeft = 1;

    const auto cancelRequested = [&options, &ticksLeft]() {
        if (ticksLeft == 0) {
            ticksLeft = options.cancelCheckEvery != 0 ? options.cancelCheckEvery : kTrashCancelCheckEvery;
            if (options.stop.stop_requested()) return true;
        }
        --ticksLeft;
        return false;
    };

    while (!stack.empty()) {
        if (cancelRequested()) {
            result.status = TrashStatus::Cancelled;
            result.cancelled = true;
            return result.status;
        }

        DirFrame& frame = stack.back();
        if (!frame.expanded) {
            frame.expanded = true;
            // Копии полей, а не ссылка на кадр: push_back ниже перевыделит память.
            const std::wstring current = frame.path;
            const std::wstring relBase = frame.rel;

            if (!directoryExists(current)) {
                // Каталог уже исчез (его снял другой процесс, или он был томом
                // без прав): это «удалять было нечего», а не отказ.
                if (frame.isRoot()) {
                    result.status = TrashStatus::NotFound;
                    result.notFound = true;
                    return result.status;
                }
                stack.pop_back();
                continue;
            }

            DirScan scan(current);
            if (!scan.valid()) {
                const DWORD error = ::GetLastError();
                if (frame.isRoot()) {
                    if (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND) {
                        result.status = TrashStatus::NotFound;
                        result.notFound = true;
                        return result.status;
                    }
                    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) {
                        result.status = classify(error);
                        result.win32Error = error;
                        result.failedPath = plainOf(current);
                        return result.status;
                    }
                } else {
                    ++result.skippedReparsePoints;  // не досчитались: содержимое неизвестно
                    stack.pop_back();
                    continue;
                }
            } else {
                std::wstring name;
                while (scan.next(name)) {
                    const WIN32_FIND_DATAW& data = scan.current();
                    const std::wstring child = joinWide(current, name);
                    const bool isLink = isReparse(data.dwFileAttributes);
                    const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                    if (isLink) {
                        // Ссылка снимается как ссылка: обходить её нельзя (FR-6),
                        // а измерять её содержимое — тем более.
                        ++result.skippedReparsePoints;
                        if (remove) {
                            if (isReadOnly(data.dwFileAttributes)) (void)clearReadOnly(child);
                            const BOOL removed =
                                isDir ? ::RemoveDirectoryW(child.c_str()) : ::DeleteFileW(child.c_str());
                            if (removed == FALSE) {
                                const DWORD error = ::GetLastError();
                                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                                    result.status = classify(error);
                                    result.win32Error = error;
                                    result.failedPath = plainOf(child);
                                    return result.status;
                                }
                            } else {
                                ++result.removedFiles;
                            }
                        }
                        continue;
                    }
                    if (isDir) {
                        stack.push_back(DirFrame{joinWide(relBase, name), child, {}, false, false});
                        continue;
                    }
                    if (remove) {
                        if (isReadOnly(data.dwFileAttributes)) (void)clearReadOnly(child);
                        if (::DeleteFileW(child.c_str()) == FALSE) {
                            const DWORD error = ::GetLastError();
                            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                                result.status = classify(error);
                                result.win32Error = error;
                                result.failedPath = plainOf(child);
                                return result.status;
                            }
                        } else {
                            ++result.removedFiles;
                        }
                    } else {
                        ++result.removedFiles;
                    }
                    result.removedBytes += sizeOf(data);
                }
            }
            continue;
        }

        // Корень обхода снимается тем же RemoveDirectoryW, что и любой
        // подкаталог: операция называется «очистить корзину», и оставить после
        // неё пустой каталог транзакции — значит через неделю получить корзину,
        // полную пустых каталогов, у которых даже манифеста нет.
        if (remove) {
            if (::RemoveDirectoryW(frame.path.c_str()) == FALSE) {
                const DWORD error = ::GetLastError();
                if (error != ERROR_DIR_NOT_EMPTY && error != ERROR_FILE_NOT_FOUND &&
                    error != ERROR_PATH_NOT_FOUND) {
                    result.status = classify(error);
                    result.win32Error = error;
                    result.failedPath = plainOf(frame.path);
                    return result.status;
                }
            } else {
                ++result.removedDirs;
            }
        } else {
            ++result.removedDirs;
        }
        stack.pop_back();
    }
    return TrashStatus::Ok;
}

// ---------------------------------------------------------------------------
// Файлы манифеста
// ---------------------------------------------------------------------------

TrashStatus readWholeFile(const std::wstring& path, std::string& out) {
    const auto handle = adopt<KernelHandlePolicy>(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) return classifyLastError();

    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle.get(), &size) == FALSE) return classifyLastError();
    if (size.QuadPart < 0 || static_cast<std::uintmax_t>(size.QuadPart) > kMaxManifestBytes) {
        return TrashStatus::Corrupt;
    }

    try {
        out.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (const std::bad_alloc&) {
        return TrashStatus::OutOfMemory;
    }
    if (out.empty()) return TrashStatus::Ok;

    DWORD readTotal = 0;
    if (::ReadFile(handle.get(), out.data(), static_cast<DWORD>(out.size()), &readTotal, nullptr) == FALSE) {
        return classifyLastError();
    }
    out.resize(readTotal);
    return TrashStatus::Ok;
}

// Атомарная запись: временный файл рядом, затем переименование. Прямая запись на
// месте оставила бы после сбоя питания обрезанный JSON, который при следующем
// запуске выглядел бы как «транзакция есть, а восстанавливать нечего» — то
// есть как потерянные данные.
TrashStatus writeFileAtomic(const std::wstring& path, std::string_view data) {
    const std::wstring temp = path + kManifestTempSuffix;
    (void)clearReadOnly(temp);

    // Без const: дескриптор освобождается через reset() на всех путях выхода,
    // а unique_handle намеренно не даёт звать reset() у константного объекта.
    auto handle = adopt<KernelHandlePolicy>(::CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) return classifyLastError();

    std::size_t writtenTotal = 0;
    for (;;) {
        DWORD written = 0;
        const std::size_t chunk = std::min<std::size_t>(data.size() - writtenTotal, kCopyChunkBytes);
        if (::WriteFile(handle.get(), data.data() + writtenTotal, static_cast<DWORD>(chunk), &written, nullptr) ==
            FALSE) {
            const TrashStatus status = classifyLastError();
            handle.reset();
            (void)::DeleteFileW(temp.c_str());
            return status;
        }
        if (written == 0) break;
        writtenTotal += written;
    }

    // Данные должны оказаться на диске до переименования: иначе «успешно
    // записанный» манифест может исчезнуть целиком.
    const BOOL flushed = ::FlushFileBuffers(handle.get());
    const DWORD flushError = ::GetLastError();
    handle.reset();
    if (flushed == FALSE) {
        (void)::DeleteFileW(temp.c_str());
        return classify(flushError);
    }
    if (writtenTotal != data.size()) {
        (void)::DeleteFileW(temp.c_str());
        return TrashStatus::IoError;
    }

    (void)clearReadOnly(path);
    if (::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const TrashStatus status = classifyLastError();
        (void)::DeleteFileW(temp.c_str());
        return status;
    }
    return TrashStatus::Ok;
}

// ---------------------------------------------------------------------------
// Перенос: общие шаги
// ---------------------------------------------------------------------------

// Сборка назначения и проверка входных данных. payload проверяется всегда, даже
// когда destinationPath задан: это последний рубеж перед тем, как имя элемента
// станет частью пути на диске.
TrashStatus resolveDestination(const TrashMoveRequest& request, const TrashOptions& options, std::string& destOut) {
    if (request.sourcePath.empty()) return TrashStatus::InvalidArgument;

    if (!request.destinationPath.empty()) {
        destOut = request.destinationPath;
        return TrashStatus::Ok;
    }
    if (request.transactionDir.empty() || !core::isValidPayloadName(request.payload)) {
        logTrashFailure(kLogMove, "недопустимое имя элемента корзины", request.payload, ERROR_INVALID_NAME, options);
        return TrashStatus::InvalidArgument;
    }
    destOut = core::joinPath(request.transactionDir, request.payload);
    return TrashStatus::Ok;
}

// Проверка «влезет ли копия». Для разреженных файлов (VHDX, теневые копии)
// логический размер завышает потребность в разы, поэтому предварительный отказ
// по ним был бы ложным: их пропускаем и полагаемся на реальную ошибку записи.
TrashStatus checkFreeSpace(const std::string& destination, const TrashFacts& facts, const TrashOptions& options) {
    if (facts.sparse || facts.bytes == 0) return TrashStatus::Ok;
    const std::string parent = parentPath(destination);
    const std::uint64_t freeBytes = freeBytesOfPath(parent.empty() ? destination : parent);
    if (freeBytes == 0) return TrashStatus::Ok;  // том не спросить — не повод отказывать
    if (freeBytes >= facts.bytes) return TrashStatus::Ok;
    logTrashFailure(kLogCopy, "на томе назначения не хватило места под копию", destination, ERROR_DISK_FULL,
                    options);
    return TrashStatus::OutOfSpace;
}

// Снять прежнее содержимое назначения перед перезаписью. Отдельная функция,
// потому что «перезапись» в этом модуле означает «прежнее содержимое снято», а не
// «слить два каталога» (FR-7: не перезаписывать без разрешения пользователя).
TrashStatus clearDestination(const std::string& destination, const TrashOptions& options) {
    const std::wstring path = toWide(longPath(destination));
    if (!exists(path)) return TrashStatus::Ok;
    const auto purged = purgePath(destination, options);
    if (!purged.ok()) return purged.status;
    return TrashStatus::Ok;
}

// Общий ход кросс-томового копирования: скопировать, примерить на копию время и
// права, затем снести источник. Отказ после начала сноса источника откатом не
// исправляется: пользователю лучше две копии, чем ни одной.
TrashStatus copyAndRemove(const std::string& source, const std::string& destination, const TrashFacts& facts,
                          bool overwrite, const TrashOptions& options, Transfer& transfer, TrashMoveResult& result) {
    const std::wstring sourcePath = toWide(longPath(source));
    const std::wstring destinationPath = toWide(longPath(destination));

    const std::string parent = parentPath(destination);
    if (!parent.empty()) {
        const TrashStatus prepared = ensureDirectoryTreeW(toWide(longPath(parent)));
        if (prepared != TrashStatus::Ok) {
            result.status = prepared;
            result.failedPath = parent;
            return prepared;
        }
    }
    if (overwrite) {
        const TrashStatus cleared = clearDestination(destination, options);
        if (cleared != TrashStatus::Ok) {
            result.status = cleared;
            result.failedPath = destination;
            return cleared;
        }
    }

    std::uint64_t copied = 0;
    const TrashStatus status = facts.directory ? copyDirectoryTree(sourcePath, destinationPath, transfer)
                                               : copyFileBytes(sourcePath, destinationPath, transfer, copied);
    if (status != TrashStatus::Ok) {
        result.status = status;
        result.failedPath = plainOf(destinationPath);
        if (status != TrashStatus::Cancelled) {
            // Недокопированное содержимое снимаем: в корзине не должно лежать
            // то, чего нет в манифесте, иначе «восстановить» будет нечего.
            const auto rollback = purgePath(destination, options);
            result.destinationRemoved = rollback.ok();
        }
        return status;
    }

    // Байты в отчёте — по замеру, а не по счётчику копирования: в манифест и в
    // отчёт об освобождении места должно уйти одно и то же число.
    result.bytesMoved = facts.bytes;
    result.filesMoved = facts.fileCount;

    if (options.applyTimestamps && facts.mtime != 0) {
        (void)applyTimestamp(destinationPath, unixToFileTime(facts.mtime));
    }
    if (options.applyAcl && !facts.aclSddl.empty()) {
        // Отказ здесь не фатален: данные уже в корзине, права — нет, и это видно
        // в журнале, а не теряется молча.
        (void)applySecuritySddl(destination, facts.aclSddl);
    }

    if (!options.removeSourceAfterCopy) return TrashStatus::Ok;

    const auto removed = purgePath(source, options);
    if (removed.ok()) {
        result.sourceRemoved = true;
        return TrashStatus::Ok;
    }
    // Копия в корзине есть, источник снести не удалось: обе копии стоят места,
    // и это видно по status/sourceRemoved, а не прячется.
    result.status = removed.status;
    result.win32Error = removed.win32Error;
    result.failedPath = removed.failedPath;
    result.sourceRemoved = false;
    return removed.status;
}

}  // namespace

// ---------------------------------------------------------------------------
// Статусы
// ---------------------------------------------------------------------------

const char* toString(TrashStatus status) noexcept {
    switch (status) {
        case TrashStatus::Ok:
            return "ok";
        case TrashStatus::Cancelled:
            return "cancelled";
        case TrashStatus::NotFound:
            return "notFound";
        case TrashStatus::AlreadyExists:
            return "alreadyExists";
        case TrashStatus::AccessDenied:
            return "accessDenied";
        case TrashStatus::InvalidArgument:
            return "invalidArgument";
        case TrashStatus::Corrupt:
            return "corrupt";
        case TrashStatus::OutOfSpace:
            return "outOfSpace";
        case TrashStatus::Unsupported:
            return "unsupported";
        case TrashStatus::IoError:
            return "ioError";
        case TrashStatus::OutOfMemory:
            return "outOfMemory";
    }
    return "unknown";  // значение извне перечисления: не «неизвестно молча», а явный текст
}

std::string formatStatus(TrashStatus status, std::uint32_t win32Error) {
    if (status == TrashStatus::Ok || win32Error == 0) {
        return std::string(toString(status));
    }
    return std::string(toString(status)) + ": " + win32ErrorText(win32Error);
}

std::wstring formatStatusWide(TrashStatus status, std::uint32_t win32Error) {
    return toUtf16(formatStatus(status, win32Error));
}

double TrashProgress::fraction() const noexcept {
    if (bytesTotal == 0) return 0.0;
    const long double done = static_cast<long double>(bytesDone);
    const long double total = static_cast<long double>(bytesTotal);
    if (done >= total) return 1.0;
    return static_cast<double>(done / total);
}

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------

std::string longPath(std::string_view pathUtf8) {
    std::wstring wide = toWide(pathUtf8);
    for (wchar_t& symbol : wide) {
        if (symbol == L'/') symbol = L'\\';
    }
    if (wide.empty() || wide.rfind(kLongPrefix, 0) == 0 || wide.rfind(kDevicePrefix, 0) == 0) {
        return toNarrow(wide);
    }
    // UNC без префикса: \\server\share -> \\?\UNC\server\share.
    if (wide.rfind(L"\\\\", 0) == 0) {
        return toNarrow(std::wstring(kUncPrefix) + wide.substr(2));
    }
    // Относительный путь и «C:имя» (относительный от диска) приводить нельзя:
    // \\?\ запрещает относительные пути в принципе, и такой путь после
    // приведения просто перестал бы существовать.
    if (wide.size() < 2 || wide[1] != L':' || (wide.size() > 2 && wide[2] != L'\\')) {
        return toNarrow(wide);
    }
    if (wide.size() == 2) {
        return toNarrow(wide + L"\\");
    }
    return toNarrow(std::wstring(kLongPrefix) + wide);
}

bool isLongPath(std::string_view pathUtf8) {
    const std::wstring wide = toWide(pathUtf8);
    return wide.rfind(kLongPrefix, 0) == 0 || wide.rfind(kDevicePrefix, 0) == 0 || wide.rfind(kUncPrefix, 0) == 0;
}

std::string parentPath(std::string_view pathUtf8) {
    const std::wstring wide = toWide(pathUtf8);
    const std::size_t separator = wide.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator == 0) return {};
    const std::wstring parent = wide.substr(0, separator);
    // «\\?\C:» и «C:» — это корень тома, а не каталог: выше него не подняться.
    if (parent.size() == 2 && parent[1] == L':') return {};
    if (parent.size() == 6 && parent.rfind(kLongPrefix, 0) == 0) return {};
    return toNarrow(parent);
}

std::string fileName(std::string_view pathUtf8) {
    const std::wstring wide = toWide(pathUtf8);
    const std::size_t separator = wide.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator + 1 >= wide.size()) return toNarrow(wide);
    return toNarrow(wide.substr(separator + 1));
}

bool pathExists(std::string_view pathUtf8) { return exists(toWide(longPath(pathUtf8))); }

bool isDirectory(std::string_view pathUtf8) { return directoryExists(toWide(longPath(pathUtf8))); }

core::ExistsProbe existsProbe() { return [](const std::string& path) { return pathExists(path); }; }

// ---------------------------------------------------------------------------
// Тома
// ---------------------------------------------------------------------------

TrashVolumeInfo volumeOfPath(std::string_view pathUtf8) {
    TrashVolumeInfo info;
    const std::wstring path = toWide(longPath(pathUtf8));
    const std::wstring plain = withoutLongPrefix(path);

    std::wstring mount(MAX_PATH, L'\0');
    const DWORD needed = ::GetVolumePathNameW(plain.c_str(), &mount.front(), static_cast<DWORD>(mount.size()));
    if (needed == 0) return info;
    mount.resize(needed < mount.size() ? needed : mount.size() - 1);

    info.mountPoint = toNarrow(mount);
    info.volumeGuid = toNarrow(volumeGuidOf(path));

    ULARGE_INTEGER available{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER freeBytes{};
    if (::GetDiskFreeSpaceExW(plain.c_str(), &available, &total, &freeBytes) != FALSE) {
        info.freeBytes = available.QuadPart;
        info.totalBytes = total.QuadPart;
    }
    // Том определён, даже если метку получить не удалось (сетевой путь): без
    // volumeGuid сравнение томов невозможно, но сам факт тома полезен отчёту.
    info.valid = !info.mountPoint.empty();
    return info;
}

bool sameVolume(std::string_view firstUtf8, std::string_view secondUtf8, bool& known) {
    const std::wstring first = volumeGuidOf(toWide(longPath(firstUtf8)));
    const std::wstring second = volumeGuidOf(toWide(longPath(secondUtf8)));
    if (first.empty() || second.empty()) {
        // Том не определён — сравнивать нечем, и врать «разные тома» или «один
        // том» здесь нельзя: решение принимает попытка переименования.
        known = false;
        return false;
    }
    known = true;
    return ::CompareStringOrdinal(first.c_str(), static_cast<int>(first.size()), second.c_str(),
                                  static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

std::uint64_t freeBytesOfPath(std::string_view pathUtf8) {
    const std::wstring plain = withoutLongPrefix(toWide(longPath(pathUtf8)));
    ULARGE_INTEGER available{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER freeBytes{};
    if (::GetDiskFreeSpaceExW(plain.c_str(), &available, &total, &freeBytes) == FALSE) return 0;
    return available.QuadPart;
}

// ---------------------------------------------------------------------------
// Сведения об объекте
// ---------------------------------------------------------------------------

TrashStatus readPathFacts(std::string_view pathUtf8, TrashFacts& facts, const TrashOptions& options) {
    facts = TrashFacts{};

    const std::wstring path = toWide(longPath(pathUtf8));
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!attributesOf(path, data)) return classifyLastError();

    facts.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    facts.readOnly = isReadOnly(data.dwFileAttributes);
    facts.sparse = (data.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
    facts.mtime = fileTimeToUnix(data.ftLastWriteTime);
    facts.volumeGuid = toNarrow(volumeGuidOf(path));
    facts.aclSddl = readSecuritySddl(pathUtf8);
    facts.fileCount = facts.directory ? 0u : 1u;  // каталог сам по себе файлом не считается
    facts.bytes = facts.directory ? 0u : sizeOf(data);
    if (!facts.directory) return TrashStatus::Ok;

    // Рекурсивный счёт. Стек явный (см. DirFrame), отмена проверяется каждые 256
    // элементов (SPEC §6.4). В стеке лежит один путь на уровень, а не все пути
    // дерева: каталог в миллион файлов не должен съедать память (§5).
    std::vector<std::wstring> stack;
    stack.push_back(path);
    std::size_t ticksLeft = options.cancelCheckEvery != 0 ? options.cancelCheckEvery : kTrashCancelCheckEvery;
    std::uint32_t skipped = 0;

    while (!stack.empty()) {
        if (ticksLeft == 0) {
            ticksLeft = options.cancelCheckEvery != 0 ? options.cancelCheckEvery : kTrashCancelCheckEvery;
            if (options.stop.stop_requested()) return TrashStatus::Cancelled;
        }
        --ticksLeft;

        const std::wstring current = stack.back();
        DirScan scan(current);
        if (!scan.valid()) {
            // Нечитаемый подкаталог считаем пропущенным, а не «пустым»: иначе
            // размер корзины в отчёте был бы занижен и «удалилось 4 ГБ»
            // означало бы «удалилось меньше 4 ГБ».
            ++skipped;
            stack.pop_back();
            continue;
        }

        std::wstring name;
        while (scan.next(name)) {
            const WIN32_FIND_DATAW& entry = scan.current();
            const std::wstring child = joinWide(current, name);
            if (isReparse(entry.dwFileAttributes)) {
                ++skipped;  // FR-6
                continue;
            }
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                stack.push_back(child);
                continue;
            }
            facts.bytes += sizeOf(entry);
            ++facts.fileCount;
        }
        stack.pop_back();
    }

    facts.skippedReparsePoints = skipped;
    return TrashStatus::Ok;
}

// ---------------------------------------------------------------------------
// Корень корзины и каталоги транзакций
// ---------------------------------------------------------------------------

std::string programDataDir() {
    // GetEnvironmentVariableW, а не SHGetKnownFolderPath: последний живёт в
    // shell32, которой в списке библиотек слоя нет, а тянуть новую библиотеку
    // ради одного вызова — значит править CMakeLists чужого слоя.
    wchar_t buffer[kEnvironmentPathChars] = {};
    const DWORD length =
        ::GetEnvironmentVariableW(L"ProgramData", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= kEnvironmentPathChars) return {};
    return toNarrow(std::wstring_view(buffer, length));
}

std::string defaultTrashRoot() {
    const std::string programData = programDataDir();
    if (programData.empty()) return {};
    return core::trashRootPath(programData);
}

TrashStatus ensureDirectoryTree(std::string_view pathUtf8, const TrashOptions& options) {
    if (pathUtf8.empty()) return TrashStatus::InvalidArgument;
    const TrashStatus status = ensureDirectoryTreeW(toWide(longPath(pathUtf8)));
    if (status != TrashStatus::Ok) {
        logTrashFailure(kLogMove, "каталог корзины не создан", pathUtf8, ::GetLastError(), options);
    }
    return status;
}

TrashStatus transactionDirectoryFor(std::string_view trashRoot, std::string_view txId, std::string& dirOut,
                                    const TrashOptions& options) {
    if (trashRoot.empty() || txId.empty()) return TrashStatus::InvalidArgument;
    // Идентификатор приезжает из манифеста, то есть с диска: имя каталога
    // собирается только из проверенных символов, иначе «../../Windows» вышел бы
    // из корня корзины (то же рассуждение, что в core::transactionDir, но здесь
    // без исключения — вызывающий получает статус).
    if (!core::isValidTxId(txId)) {
        logTrashFailure(kLogMove, "недопустимый идентификатор транзакции", std::string(txId), ERROR_INVALID_NAME,
                        options);
        return TrashStatus::InvalidArgument;
    }
    dirOut = core::joinPath(std::string(trashRoot), std::string(txId));
    return TrashStatus::Ok;
}

TrashStatus createTransactionDirectory(std::string_view trashRoot, std::string_view txId, std::string& dirOut,
                                      const TrashOptions& options) {
    TrashStatus status = transactionDirectoryFor(trashRoot, txId, dirOut, options);
    if (status != TrashStatus::Ok) return status;

    status = ensureDirectoryTreeW(toWide(longPath(std::string(trashRoot))));
    if (status != TrashStatus::Ok) {
        logTrashFailure(kLogMove, "корень корзины не создан", std::string(trashRoot), ::GetLastError(), options);
        return status;
    }

    const std::wstring dir = toWide(longPath(dirOut));
    if (directoryExists(dir)) return TrashStatus::Ok;
    if (::CreateDirectoryW(dir.c_str(), nullptr) != FALSE) return TrashStatus::Ok;

    const DWORD error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS && directoryExists(dir)) return TrashStatus::Ok;
    logTrashFailure(kLogMove, "каталог транзакции не создан", dirOut, error, options);
    return classify(error);
}

// ---------------------------------------------------------------------------
// Перенос в корзину
// ---------------------------------------------------------------------------

TrashMoveResult moveToTrash(const TrashMoveRequest& request, const TrashOptions& options) {
    TrashMoveResult result;

    std::string destination;
    const TrashStatus resolved = resolveDestination(request, options, destination);
    if (resolved != TrashStatus::Ok) {
        result.status = resolved;
        result.win32Error = ERROR_INVALID_NAME;
        return result;
    }
    result.destinationPath = destination;

    // 1. Замер. Нужен и для переименования: в манифест обязаны уйти реальные
    //    байты, mtime и ACL, а без них восстановление вернёт «что-то».
    const TrashStatus measured = readPathFacts(request.sourcePath, result.facts, options);
    if (measured != TrashStatus::Ok) {
        result.status = measured;
        result.win32Error = ::GetLastError();
        result.failedPath = request.sourcePath;
        if (measured == TrashStatus::Cancelled) {
            logTrashCancelled(kLogMove, request.sourcePath, options);
        } else {
            logTrashFailure(kLogMove, "объект не удалось обойти", request.sourcePath, result.win32Error, options);
        }
        return result;
    }

    Transfer transfer;
    transfer.options = &options;
    transfer.setTotals(result.facts.bytes, result.facts.fileCount);

    // 2. Тот же том — атомарное переименование. Это и быстрый путь, и
    //    единственный, который сохраняет права, времена и hard links без
    //    копирования, поэтому пробуется первым. Отказ ERROR_NOT_SAME_DEVICE —
    //    не ошибка, а ответ «тома разные», и тогда включается копирование.
    const std::wstring sourcePath = toWide(longPath(request.sourcePath));
    const std::wstring destinationPath = toWide(longPath(destination));
    const DWORD moveFlags = request.overwriteDestination ? MOVEFILE_REPLACE_EXISTING : 0;
    if (::MoveFileExW(sourcePath.c_str(), destinationPath.c_str(), moveFlags) != FALSE) {
        result.status = TrashStatus::Ok;
        result.bytesMoved = result.facts.bytes;
        result.filesMoved = result.facts.fileCount;
        result.skippedReparsePoints = result.facts.skippedReparsePoints;
        transfer.progress.bytesDone = transfer.progress.bytesTotal;
        transfer.progress.filesDone = transfer.progress.filesTotal;
        transfer.publish();
        return result;
    }

    const DWORD moveError = ::GetLastError();
    if (moveError != ERROR_NOT_SAME_DEVICE) {
        result.status = classify(moveError);
        result.win32Error = moveError;
        result.failedPath = plainOf(destinationPath);
        if (result.status == TrashStatus::Cancelled) {
            logTrashCancelled(kLogMove, request.sourcePath, options);
        } else {
            logTrashFailure(kLogMove, "перенос не выполнен", destination, moveError, options);
        }
        return result;
    }

    // 3. Разные тома (FR-7): это копирование, и UI показывает его как долгое.
    result.crossVolume = true;
    const TrashStatus space = checkFreeSpace(destination, result.facts, options);
    if (space != TrashStatus::Ok) {
        result.status = space;
        result.win32Error = ERROR_DISK_FULL;
        result.failedPath = destination;
        return result;
    }

    const TrashStatus status =
        copyAndRemove(request.sourcePath, destination, result.facts, request.overwriteDestination, options, transfer,
                      result);
    result.status = status;
    result.skippedReparsePoints = transfer.reparseSkipped;
    if (status == TrashStatus::Cancelled) {
        logTrashCancelled(kLogCopy, request.sourcePath, options);
    } else if (status != TrashStatus::Ok) {
        logTrashFailure(kLogCopy, "кросс-томовый перенос не завершён", result.failedPath, result.win32Error, options);
    }
    return result;
}

TrashStageResult stageTrashItem(const TrashStageRequest& request, const TrashOptions& options) {
    TrashStageResult staged;

    TrashMoveRequest move;
    move.sourcePath = request.originalPath;
    move.transactionDir = request.transactionDir;
    move.payload = request.payload;
    staged.transfer = moveToTrash(move, options);
    if (!staged.transfer.ok()) return staged;

    staged.item.kind = staged.transfer.facts.directory ? core::TrashItemKind::Directory : core::TrashItemKind::File;
    staged.item.originalPath = request.originalPath;
    staged.item.payload = request.payload;
    staged.item.bytes = staged.transfer.bytesMoved;
    staged.item.fileCount = staged.transfer.filesMoved;
    staged.item.mtime = staged.transfer.facts.mtime;
    staged.item.readOnly = staged.transfer.facts.readOnly;
    staged.item.aclSddl = staged.transfer.facts.aclSddl;
    staged.item.crossVolume = staged.transfer.crossVolume;
    staged.item.sourceVolume = staged.transfer.facts.volumeGuid;
    return staged;
}

// ---------------------------------------------------------------------------
// Манифест
// ---------------------------------------------------------------------------

TrashStatus writeManifest(std::string_view transactionDir, const core::TrashTransaction& tx,
                         const TrashOptions& options) {
    if (transactionDir.empty()) return TrashStatus::InvalidArgument;

    const std::string dir = std::string(transactionDir);
    const TrashStatus prepared = ensureDirectoryTreeW(toWide(longPath(dir)));
    if (prepared != TrashStatus::Ok) {
        logTrashFailure(kLogManifest, "каталог транзакции не найден", dir, ::GetLastError(), options);
        return prepared;
    }

    std::string text;
    try {
        text = core::serializeManifest(tx, 2);
    } catch (const core::TrashError&) {
        return TrashStatus::InvalidArgument;
    } catch (const std::bad_alloc&) {
        return TrashStatus::OutOfMemory;
    }

    const std::string path = core::manifestPath(dir);
    const TrashStatus written = writeFileAtomic(toWide(longPath(path)), text);
    if (written != TrashStatus::Ok) {
        logTrashFailure(kLogManifest, "манифест не записан", path, ::GetLastError(), options);
    }
    return written;
}

TrashStatus readManifest(std::string_view transactionDir, core::TrashTransaction& txOut) {
    if (transactionDir.empty()) return TrashStatus::InvalidArgument;

    const std::string path = core::manifestPath(std::string(transactionDir));
    std::string text;
    const TrashStatus read = readWholeFile(toWide(longPath(path)), text);
    if (read != TrashStatus::Ok) return read;

    try {
        txOut = core::parseManifest(text, path);
    } catch (const core::TrashError&) {
        return TrashStatus::Corrupt;  // битый JSON или чужая схема — это не транзакция
    } catch (const std::bad_alloc&) {
        return TrashStatus::OutOfMemory;
    }
    return TrashStatus::Ok;
}

std::vector<core::TrashTransactionInfo> TrashInventory::info() const {
    std::vector<core::TrashTransactionInfo> infos;
    infos.reserve(transactions.size());
    for (const auto& tx : transactions) {
        infos.push_back(core::infoOf(tx));
    }
    return infos;
}

core::TrashUsage TrashInventory::usage() const { return core::summarize(info()); }

TrashInventory loadInventory(std::string_view trashRoot, const TrashOptions& options) {
    TrashInventory inventory;
    if (trashRoot.empty()) return inventory;

    const std::wstring root = toWide(longPath(std::string(trashRoot)));
    if (!directoryExists(root)) return inventory;

    DirScan scan(root);
    if (!scan.valid()) return inventory;

    std::wstring name;
    while (scan.next(name)) {
        const WIN32_FIND_DATAW& data = scan.current();
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        const std::string dir = toNarrow(joinWide(root, name));
        ++inventory.scannedDirs;

        if (isReparse(data.dwFileAttributes)) {
            // Каталог-ссылка в корне корзины — не наша транзакция. Обходить её
            // нельзя (FR-6), а удалять без явного решения вызывающего нельзя
            // тем более.
            inventory.brokenDirs.push_back(dir);
            continue;
        }
        if (!core::isValidTxId(toUtf8(name))) {
            inventory.brokenDirs.push_back(dir);  // неизвестное содержимое, удалять нельзя
            continue;
        }

        core::TrashTransaction tx;
        const TrashStatus status = readManifest(dir, tx);
        if (status == TrashStatus::Ok) {
            inventory.transactions.push_back(std::move(tx));
            continue;
        }
        // Каталог без читаемого манифеста нельзя ни восстановить, ни посчитать
        // по учёту, поэтому решение остаётся за вызывающим.
        logTrashFailure(kLogScan, "манифест транзакции не прочитан", dir,
                        status == TrashStatus::Corrupt ? ERROR_INVALID_DATA : ::GetLastError(), options);
        inventory.brokenDirs.push_back(dir);
    }
    return inventory;
}

// ---------------------------------------------------------------------------
// Восстановление
// ---------------------------------------------------------------------------

TrashRestoreResult restoreTrashItem(const TrashRestoreRequest& request, const TrashOptions& options) {
    TrashRestoreResult result;

    // Missing и Conflict — это решения, а не отказы платформы: в обоих случаях
    // на диске ничего не меняется, и вызывающий показывает пользователю то, что
    // он и должен увидеть (FR-7: «существующий файл — не перезаписывать, спросить»).
    if (request.action == core::RestoreAction::Missing) {
        result.status = TrashStatus::NotFound;
        return result;
    }
    if (request.action == core::RestoreAction::Conflict) {
        result.status = TrashStatus::AlreadyExists;
        result.conflictSkipped = true;
        return result;
    }
    if (!core::isValidPayloadName(request.item.payload)) {
        result.status = TrashStatus::InvalidArgument;
        return result;
    }

    const std::string destination =
        request.destinationPath.empty() ? std::string(request.item.originalPath) : request.destinationPath;
    if (destination.empty()) {
        result.status = TrashStatus::InvalidArgument;
        return result;
    }

    const std::string source = core::joinPath(request.transactionDir, request.item.payload);
    const std::wstring sourcePath = toWide(longPath(source));
    const std::wstring destinationPath = toWide(longPath(destination));

    if (!exists(sourcePath)) {
        result.status = TrashStatus::NotFound;
        result.failedPath = plainOf(sourcePath);
        return result;
    }

    TrashFacts facts;
    const TrashStatus measured = readPathFacts(source, facts, options);
    if (measured != TrashStatus::Ok) {
        result.status = measured;
        result.failedPath = source;
        if (measured == TrashStatus::Cancelled) {
            logTrashCancelled(kLogRestore, destination, options);
        } else {
            logTrashFailure(kLogRestore, "содержимое корзины не прочитано", source, ::GetLastError(), options);
        }
        return result;
    }
    result.bytesRestored = facts.bytes;
    result.filesRestored = facts.fileCount;
    result.skippedReparsePoints = facts.skippedReparsePoints;

    const bool overwrite = request.action == core::RestoreAction::Overwrite;
    if (exists(destinationPath) && !overwrite) {
        result.status = TrashStatus::AlreadyExists;
        result.conflictSkipped = true;
        result.failedPath = destination;
        return result;
    }

    // Тот же том — переименованием: содержимое возвращается на место без
    // копирования и без риска, что на диске останется две копии. Права и времена
    // при этом сохраняются сами (объект переехал в корзину тем же rename).
    const DWORD moveFlags = overwrite ? MOVEFILE_REPLACE_EXISTING : 0;
    if (::MoveFileExW(sourcePath.c_str(), destinationPath.c_str(), moveFlags) != FALSE) {
        result.status = TrashStatus::Ok;
        return result;
    }

    const DWORD moveError = ::GetLastError();
    if (moveError == ERROR_ALREADY_EXISTS) {
        result.status = TrashStatus::AlreadyExists;
        result.conflictSkipped = true;
        result.failedPath = destination;
        return result;
    }
    if (moveError != ERROR_NOT_SAME_DEVICE) {
        result.status = classify(moveError);
        result.win32Error = moveError;
        result.failedPath = plainOf(sourcePath);
        logTrashFailure(kLogRestore, "восстановление не выполнено", destination, moveError, options);
        return result;
    }

    // Корзина и исходное место на разных томах: возвращаем копированием.
    result.crossVolume = true;

    Transfer transfer;
    transfer.options = &options;
    transfer.setTotals(facts.bytes, facts.fileCount);

    const std::string parent = parentPath(destination);
    if (!parent.empty()) {
        // Недостающие родители назначения создаются: пользователь мог удалить
        // исходный каталог целиком, и «восстановить» без этого некуда.
        const TrashStatus prepared = ensureDirectoryTreeW(toWide(longPath(parent)));
        if (prepared != TrashStatus::Ok) {
            result.status = prepared;
            result.failedPath = parent;
            return result;
        }
    }
    if (overwrite) {
        const TrashStatus cleared = clearDestination(destination, options);
        if (cleared != TrashStatus::Ok) {
            result.status = cleared;
            result.failedPath = destination;
            return result;
        }
    }

    std::uint64_t copied = 0;
    const TrashStatus copiedStatus =
        facts.directory ? copyDirectoryTree(sourcePath, destinationPath, transfer)
                        : copyFileBytes(sourcePath, destinationPath, transfer, copied);
    if (copiedStatus != TrashStatus::Ok) {
        result.status = copiedStatus;
        result.failedPath = plainOf(destinationPath);
        if (copiedStatus == TrashStatus::Cancelled) {
            logTrashCancelled(kLogRestore, destination, options);
        } else {
            logTrashFailure(kLogRestore, "копирование при восстановлении не завершено", destination,
                            ::GetLastError(), options);
        }
        return result;
    }

    if (options.applyTimestamps && request.item.mtime != 0) {
        (void)applyTimestamp(destinationPath, unixToFileTime(request.item.mtime));
    }
    if (options.applyAcl && !request.item.aclSddl.empty()) {
        (void)applySecuritySddl(destination, request.item.aclSddl);
    }

    // Копия на месте — содержимое в корзине можно убрать, иначе транзакция
    // навсегда осталась бы занимать место.
    (void)purgePath(source, options);

    result.status = TrashStatus::Ok;
    return result;
}

TrashRestoreSummary restoreTrashItems(std::string_view transactionDir, const core::RestorePlan& plan,
                                      const TrashOptions& options) {
    TrashRestoreSummary summary;
    summary.items.reserve(plan.items.size());

    for (const auto& entry : plan.items) {
        if (options.stop.stop_requested()) {
            summary.cancelled = true;
            break;
        }

        TrashRestoreRequest request;
        request.transactionDir = std::string(transactionDir);
        request.item = entry.item;
        request.action = entry.action;

        TrashRestoreResult result = restoreTrashItem(request, options);
        if (result.ok()) {
            ++summary.restoredCount;
            summary.restoredBytes += result.bytesRestored;
        } else if (result.conflictSkipped) {
            ++summary.conflictCount;
        } else if (result.status == TrashStatus::NotFound) {
            ++summary.missingCount;
        } else {
            ++summary.failedCount;
        }
        summary.items.push_back(std::move(result));
    }
    return summary;
}

// ---------------------------------------------------------------------------
// Вытеснение и очистка
// ---------------------------------------------------------------------------

TrashPurgeResult purgePath(std::string_view pathUtf8, const TrashOptions& options) {
    TrashPurgeResult result;
    if (pathUtf8.empty()) {
        result.status = TrashStatus::InvalidArgument;
        return result;
    }

    const std::wstring path = toWide(longPath(pathUtf8));
    if (!exists(path)) {
        result.status = TrashStatus::NotFound;
        result.notFound = true;
        return result;
    }
    // Каталог может оказаться read-only: без снятия признака RemoveDirectoryW
    // вернёт ACCESS_DENIED, и содержимое корзины останется на месте.
    (void)clearReadOnly(path);

    result.status = walkTree(path, options, /*remove=*/true, result);
    if (result.status == TrashStatus::Cancelled) {
        logTrashCancelled(kLogPurge, pathUtf8, options);
    } else if (result.status != TrashStatus::Ok) {
        logTrashFailure(kLogPurge, "путь не удалён", result.failedPath.empty() ? plainOf(path) : result.failedPath,
                        result.win32Error, options);
    }
    return result;
}

TrashPurgeResult purgeTransactionDirectory(std::string_view transactionDir, const TrashOptions& options) {
    TrashPurgeResult result;
    if (transactionDir.empty()) {
        result.status = TrashStatus::InvalidArgument;
        return result;
    }

    const std::wstring path = toWide(longPath(transactionDir));
    if (!exists(path)) {
        result.status = TrashStatus::NotFound;
        result.notFound = true;
        return result;
    }
    // Не-каталог на месте транзакции — это либо чужая подкладка, либо след
    // сбоя. Обращаться с ним как с обычным путём безопасно, а вот рекурсивно
    // обходить неизвестный путь нельзя.
    if (!directoryExists(path)) return purgePath(transactionDir, options);

    result.status = walkTree(path, options, /*remove=*/true, result);
    if (result.status == TrashStatus::Cancelled) {
        logTrashCancelled(kLogPurge, transactionDir, options);
    } else if (result.status != TrashStatus::Ok) {
        logTrashFailure(kLogPurge, "каталог транзакции не удалён", plainOf(path), result.win32Error, options);
    }
    return result;
}

TrashPurgeSummary purgeEviction(std::string_view trashRoot, const core::TrashEvictionPlan& plan,
                                const TrashOptions& options) {
    TrashPurgeSummary summary;
    summary.results.reserve(plan.txIds.size());

    for (const auto& txId : plan.txIds) {
        if (options.stop.stop_requested()) {
            summary.cancelled = true;
            break;
        }
        if (!core::isValidTxId(txId)) {
            ++summary.failedCount;
            summary.failedDirs.push_back(std::string(txId));
            continue;
        }

        const std::string dir = core::joinPath(std::string(trashRoot), txId);
        TrashPurgeResult result = purgeTransactionDirectory(dir, options);
        if (result.cancelled) {
            summary.cancelled = true;
        } else if (!result.ok()) {
            ++summary.failedCount;
            summary.failedDirs.push_back(dir);
        }
        summary.removedBytes += result.removedBytes;
        summary.removedDirs += result.removedDirs;
        summary.results.push_back(std::move(result));
    }
    return summary;
}

TrashPurgeResult measureTrashRoot(std::string_view trashRoot, const TrashOptions& options) {
    TrashPurgeResult result;
    if (trashRoot.empty()) {
        result.status = TrashStatus::InvalidArgument;
        return result;
    }

    const std::wstring path = toWide(longPath(trashRoot));
    if (!directoryExists(path)) {
        result.status = TrashStatus::NotFound;
        result.notFound = true;
        return result;
    }
    // Тот же обход без удаления. В removedBytes/removedFiles здесь лежит
    // измеренное, а не снятое: имена полей общие у обоих режимов намеренно —
    // различать «сколько весит» и «сколько сняли» двумя структурами ради одного
    // обхода дороже, чем одна эта строка комментария.
    result.status = walkTree(path, options, /*remove=*/false, result);
    return result;
}

// ---------------------------------------------------------------------------
// ACL
// ---------------------------------------------------------------------------

std::string readSecuritySddl(std::string_view pathUtf8) {
    if (pathUtf8.empty()) return {};

    const std::wstring path = toWide(longPath(pathUtf8));
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD error = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                 nullptr, &dacl, nullptr, &descriptor);
    if (error != ERROR_SUCCESS || descriptor == nullptr) {
        return {};  // нет прав читать дескриптор — это «ACL не менялись»
    }

    LPWSTR text = nullptr;
    // Пять аргументов, а не четыре: у W-варианта между ревизией SDDL и выходным
    // указателем стоит ещё и SECURITY_INFORMATION — в манифест уходит только DACL
    // (владелец и SACL §4 FR-7 не требует, а SACL чаще всего нечитаем без прав).
    const DWORD converted = ::ConvertSecurityDescriptorToStringSecurityDescriptorW(
        descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr);
    ::LocalFree(descriptor);
    if (converted != ERROR_SUCCESS || text == nullptr) return {};

    std::string sddl = toNarrow(text);
    ::LocalFree(text);
    return sddl;
}

TrashStatus applySecuritySddl(std::string_view pathUtf8, std::string_view sddl) {
    if (pathUtf8.empty() || sddl.empty()) return TrashStatus::InvalidArgument;

    const std::wstring sddlWide = toWide(sddl);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddlWide.c_str(), SDDL_REVISION_1, &descriptor,
                                                              nullptr) == FALSE) {
        return classifyLastError();
    }

    BOOL daclPresent = FALSE;
    PACL dacl = nullptr;
    BOOL daclValid = FALSE;
    // GetSecurityDescriptorDacl, а не GetDacl: в установленном Windows SDK
    // (10.0.19041.0) функции GetDacl нет, и модуль с ней не собрался бы.
    (void)::GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclValid);
    if (!daclPresent || dacl == nullptr) {
        ::LocalFree(descriptor);
        return TrashStatus::InvalidArgument;
    }

    const std::wstring path = toWide(longPath(pathUtf8));
    const DWORD error = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                                 DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
    ::LocalFree(descriptor);
    return error == ERROR_SUCCESS ? TrashStatus::Ok : classify(error);
}

}  // namespace mrproper::platform
