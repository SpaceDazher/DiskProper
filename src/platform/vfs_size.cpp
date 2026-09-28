// Реализация vfs_size: единственный файл модуля, где встречается windows.h.
// Всё Win32-специфичное здесь; наружу (vfs_size.hpp) уходят только типы и
// результаты, чтобы обход каталогов не тащил windows.h в свои заголовки.
//
// Слои работы одного элемента (SPEC §4 FR-4, §6.3):
//   1) узнать признаки (FILE_ATTRIBUTE_*) — отсюда понятно, считать ли вообще;
//   2) взять пару «логический / аллоцированный» одним согласованным чтением;
//   3) отдать результат с признаком allocatedKnown и кодом Win32.
//
// Главное правило, ради которого модуль существует: освобождаемое место — это
// аллоцированный размер (SPEC §4 FR-4). Логическая длина файла в оценку не
// входит: у разреженного файла «8 ГБ» может лежать 1 ГБ, и обе цифры верны,
// но свободное место тома меняет вторая.

#include "vfs_size.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.hpp"

namespace mrproper::platform::vfs {
namespace {

// --- Счётчики и их разгрузка (SPEC §5, §12) -------------------------------

std::atomic<std::uint64_t> gMeasured{0};
std::atomic<std::uint64_t> gFailed{0};
std::atomic<std::uint64_t> gAllocatedUnknown{0};
std::atomic<std::uint64_t> gLogThrottled{0};

// Обход считает сотни тысяч элементов, и каждый отказ писать в лог — это
// полмиллиона записей, которые съедают и место, и время. Поэтому первые отказы
// пишутся подробно, дальше — каждая тысячная, а число подавленных отказов
// лежит в sizeProbeStats(): по журналу видно и причину, и масштаб.
constexpr std::uint64_t kLoggedFirst = 8;
constexpr std::uint64_t kLoggedEvery = 1000;

// Инкремент счётчика вместе с решением «писать ли в журнал».
bool takeFailureSlot(std::atomic<std::uint64_t>& counter) noexcept {
    const std::uint64_t ordinal = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= kLoggedFirst || ordinal % kLoggedEvery == 0) {
        return true;
    }
    gLogThrottled.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// --- Код ошибки Win32 → состояние -----------------------------------------

SizeStatus classify(DWORD error) noexcept {
    switch (error) {
        case ERROR_SUCCESS:
            return SizeStatus::Ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NO_MORE_FILES:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_UNEXP_NET_ERR:
            return SizeStatus::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE:
            return SizeStatus::AccessDenied;
        case ERROR_INVALID_PARAMETER:
        case ERROR_BAD_PATHNAME:
            return SizeStatus::InvalidArgument;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return SizeStatus::Unsupported;
        default:
            return SizeStatus::Unavailable;
    }
}

// --- Путь в длинной форме --------------------------------------------------

// \\?\C:\… и \\?\UNC\server\share\… — единственная форма, в которой файловые
// вызовы Win32 работают с путями длиннее MAX_PATH (SPEC §4 FR-6: «поддержка
// \\?\ для путей > MAX_PATH»). Правила ровно такие:
//
//   * уже длинный путь и путь к устройству (\\.\…) не трогаем;
//   * прямой слэш внутри пути означает, что вызыватель пользуется обычной
//     формой: под \\?\ прямой слэш недопустим, и такой путь пришлось бы
//     переписывать посимвольно — проще оставить как есть;
//   * относительный путь не разрешаем: текущий каталог модуль не меняет, а
//     значит, относительный путь остаётся верным до конца вызова.
std::wstring toExtendedPath(std::wstring_view path) {
    const bool prefixed = path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
                          (path[2] == L'?' || path[2] == L'.') && path[3] == L'\\';
    if (prefixed) {
        return std::wstring(path);
    }
    for (const wchar_t symbol : path) {
        if (symbol == L'/') {
            return std::wstring(path);
        }
    }
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        // UNC: \\server\share\dir -> \\?\UNC\server\share\dir
        std::wstring result = L"\\\\?\\UNC\\";
        result.append(path.substr(2));
        return result;
    }
    if (path.size() < 2 || path[1] != L':') {
        return std::wstring(path);
    }
    std::wstring result = L"\\\\?\\";
    result.append(path);
    return result;
}

// --- Признаки элемента -----------------------------------------------------

FileFlags flagsFromAttributes(DWORD attributes) noexcept {
    FileFlags flags = FileFlags::None;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        flags = flags | FileFlags::Directory;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        flags = flags | FileFlags::ReparsePoint;
    }
    if ((attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0) {
        flags = flags | FileFlags::Sparse;
    }
    if ((attributes & FILE_ATTRIBUTE_COMPRESSED) != 0) {
        flags = flags | FileFlags::Compressed;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        flags = flags | FileFlags::ReadOnly;
    }
    if ((attributes & FILE_ATTRIBUTE_OFFLINE) != 0) {
        flags = flags | FileFlags::Offline;
    }
    if ((attributes & FILE_ATTRIBUTE_RECALL_ON_OPEN) != 0) {
        flags = flags | FileFlags::RecallOnOpen;
    }
    if ((attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0) {
        flags = flags | FileFlags::RecallOnDataAccess;
    }
    return flags;
}

const wchar_t* flagName(FileFlags flag) noexcept {
    switch (flag) {
        case FileFlags::Directory:
            return L"directory";
        case FileFlags::ReparsePoint:
            return L"reparse";
        case FileFlags::Sparse:
            return L"sparse";
        case FileFlags::Compressed:
            return L"compressed";
        case FileFlags::ReadOnly:
            return L"readonly";
        case FileFlags::Offline:
            return L"offline";
        case FileFlags::RecallOnOpen:
            return L"recall-on-open";
        case FileFlags::RecallOnDataAccess:
            return L"recall-on-data-access";
        case FileFlags::None:
            break;
    }
    return L"none";
}

// --- Запись отказов в журнал (SPEC §12: путь и HRESULT обязательны) ----------

// Отказ «не измерилось ничего»: элемент выпал из оценки целиком.
void logMeasureFailure(std::string_view event, std::wstring_view path, SizeStatus status,
                       std::uint32_t win32Error) noexcept {
    if (!takeFailureSlot(gFailed)) {
        return;
    }
    try {
        // Поля собираются явно, а не макросом MRP_LOG_FAILURE: набор здесь
        // разнородный (строки и числа), а разворачивание пакета в
        // core::detail::logFieldList на MSVC v142 спотыкается на перегрузках
        // logField. Формат записи тот же — путь и код Win32 logFailure ставит
        // первыми сам.
        core::LogFields fields;
        fields.push_back(core::logField("errorDomain", "win32"));
        fields.push_back(core::logField("status", toString(status)));
        core::logFailure(event, "размер элемента не измерен: неизвестны и логический, и аллоцированный размер",
                         core::toUtf8(path), static_cast<std::int64_t>(win32Error), std::move(fields));
    } catch (...) {
        // Сбой записи в журнал не имеет права превращать отказ Win32 в падение
        // обхода (SPEC §5).
    }
}

// Аллоцированный размер не получен, логический известен: оценка освобождения
// теперь по логическому (core::estimateReclaim), и это надо показать в журнале.
void logAllocatedUnknown(std::wstring_view path, const FileSize& size) noexcept {
    // «Неприменимо» — не отказ: каталог и reparse point аллоцированного размера
    // не имеют by design (см. asEntry). Логировать их на каждом элементе обхода
    // значило бы залить журнал событиями, которых там быть не должно.
    if (size.allocatedStatus == SizeStatus::NotApplicable) {
        return;
    }
    if (!takeFailureSlot(gAllocatedUnknown)) {
        return;
    }
    try {
        core::LogFields fields;
        fields.push_back(core::logField("errorDomain", "win32"));
        fields.push_back(core::logField("status", toString(size.allocatedStatus)));
        fields.push_back(core::logField("win32Error", static_cast<long long>(size.allocatedWin32Error)));
        fields.push_back(core::logField("logicalBytes", size.logicalBytes));
        fields.push_back(core::logField("flags", describeFlags(size.flags)));
        fields.push_back(core::logField("path", core::toUtf8(path)));
        core::logWarn("vfs.size.allocated_unknown",
                      "аллоцированный размер недоступен: освобождение оценивается по логическому размеру",
                      std::move(fields));
    } catch (...) {
        // Как и выше: оценка «по логическому» лучше, чем падение обхода.
    }
}

// --- Аллоцированный размер по пути (SPEC §4 FR-4) -------------------------

AllocatedSizeResult readAllocatedSize(const std::wstring& extendedPath) noexcept {
    AllocatedSizeResult result{};

    DWORD high = 0;
    const DWORD low = ::GetCompressedFileSizeW(extendedPath.c_str(), &high);
    if (low == INVALID_FILE_SIZE) {
        // INVALID_FILE_SIZE — это 0xFFFFFFFF, и он же законный размер файла
        // ровно в 4 ГиБ − 1 Б. Отличает отказ от успеха только GetLastError,
        // поэтому он и читается сразу же — документированная проверка именно
        // такой парой вызовов.
        const DWORD error = ::GetLastError();
        if (error != ERROR_SUCCESS) {
            result.status = classify(error);
            result.win32Error = error;
            return result;
        }
    }
    result.bytes = (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
    result.status = SizeStatus::Ok;
    return result;
}

AllocatedSizeResult noMemory() noexcept {
    return AllocatedSizeResult{0, SizeStatus::Unavailable, static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY)};
}

std::chrono::milliseconds elapsedSince(std::chrono::steady_clock::time_point started) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
}

// Заготовка результата по признакам и логическому размеру. Каталог и reparse
// point получают allocatedStatus == NotApplicable: аллоцированного размера у них
// нет by design, а не «не удалось прочитать»:
//
//   * каталог занимает кластер, а не имеет длину; его стоимость вызывающий
//     добавляет через core::SizeAccumulator::addDirectory с размером кластера из
//     queryClusterSize (логика агрегации — в переносимом core, здесь Win32);
//   * reparse point обход не спускает (SPEC §4 FR-6), а GetCompressedFileSizeW
//     ссылки не умеет — он всегда идёт по цели, то есть посчитал бы чужое дерево.
FileSize asEntry(std::uint64_t logicalBytes, FileFlags flags) noexcept {
    FileSize size{};
    size.status = SizeStatus::Ok;
    size.flags = flags;
    size.logicalBytes = logicalBytes;
    size.allocatedStatus = SizeStatus::NotApplicable;
    if (hasFlag(flags, FileFlags::Directory)) {
        size.logicalBytes = 0;
    }
    return size;
}

// Свести результат запроса аллоцированного размера с общим результатом и
// отметить элемент в счётчиках.
void finishEntry(FileSize& size, std::wstring_view path, const AllocatedSizeResult& allocated) noexcept {
    if (allocated.ok()) {
        size.allocatedBytes = allocated.bytes;
        size.allocatedKnown = true;
        size.allocatedStatus = SizeStatus::Ok;
        gMeasured.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    size.allocatedBytes = 0;
    size.allocatedKnown = false;
    size.allocatedStatus = allocated.status;
    size.allocatedWin32Error = allocated.win32Error;
    logAllocatedUnknown(path, size);
}

// Отказ всего измерения: элемент выпадает из оценки (нет прав, элемента нет,
// том не отвечает). Логируется сразу и с троттлингом.
FileSize failure(std::wstring_view path, SizeStatus status, std::uint32_t win32Error) noexcept {
    FileSize size{};
    size.status = status;
    size.win32Error = win32Error;
    size.allocatedStatus = status;
    size.allocatedWin32Error = win32Error;
    logMeasureFailure("vfs.size.failed", path, status, win32Error);
    return size;
}

}  // namespace

const wchar_t* toString(SizeStatus status) noexcept {
    switch (status) {
        case SizeStatus::Ok:
            return L"ok";
        case SizeStatus::NotApplicable:
            return L"not-applicable";
        case SizeStatus::InvalidArgument:
            return L"invalid-argument";
        case SizeStatus::NotFound:
            return L"not-found";
        case SizeStatus::AccessDenied:
            return L"access-denied";
        case SizeStatus::Unsupported:
            return L"unsupported";
        case SizeStatus::Unavailable:
            break;
    }
    return L"unavailable";
}

std::wstring formatSizeError(SizeStatus status, std::uint32_t win32Error) {
    if (status == SizeStatus::Ok) {
        return {};
    }
    std::wstring text = toString(status);
    if (win32Error == 0) {
        return text;
    }

    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        win32Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length != 0 && buffer != nullptr) {
        std::wstring systemText(buffer, length);
        // FormatMessageW завершает текст переводом строки — в одну строку он
        // не нужен, а в лог и JSON попадать не должен.
        while (!systemText.empty() && (systemText.back() == L'\r' || systemText.back() == L'\n')) {
            systemText.pop_back();
        }
        if (!systemText.empty()) {
            text += L": ";
            text += systemText;
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return text;
}

std::wstring describeFlags(FileFlags flags) {
    static constexpr FileFlags kAll[] = {FileFlags::Directory,    FileFlags::ReparsePoint,
                                         FileFlags::Sparse,       FileFlags::Compressed,
                                         FileFlags::ReadOnly,     FileFlags::Offline,
                                         FileFlags::RecallOnOpen, FileFlags::RecallOnDataAccess};
    std::wstring text;
    for (const FileFlags flag : kAll) {
        if (!hasFlag(flags, flag)) {
            continue;
        }
        if (!text.empty()) {
            text += L" | ";
        }
        text += flagName(flag);
    }
    return text.empty() ? std::wstring(L"none") : text;
}

SizeProbeStats sizeProbeStats() noexcept {
    SizeProbeStats stats{};
    stats.measured = gMeasured.load(std::memory_order_relaxed);
    stats.failed = gFailed.load(std::memory_order_relaxed);
    stats.allocatedUnknown = gAllocatedUnknown.load(std::memory_order_relaxed);
    stats.logThrottled = gLogThrottled.load(std::memory_order_relaxed);
    return stats;
}

void resetSizeProbeStats() noexcept {
    gMeasured.store(0, std::memory_order_relaxed);
    gFailed.store(0, std::memory_order_relaxed);
    gAllocatedUnknown.store(0, std::memory_order_relaxed);
    gLogThrottled.store(0, std::memory_order_relaxed);
}

AllocatedSizeResult queryAllocatedSize(std::wstring_view path) noexcept {
    if (path.empty()) {
        return AllocatedSizeResult{0, SizeStatus::InvalidArgument, static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER)};
    }
    try {
        return readAllocatedSize(toExtendedPath(path));
    } catch (const std::bad_alloc&) {
        // Единственное, что может вылететь, — нехватка памяти на копии пути.
        // Наружу не пускаем: обход не имеет права падать из-за одного элемента.
        return noMemory();
    }
}

FileSize measurePath(std::wstring_view path) noexcept {
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    if (path.empty()) {
        FileSize size = failure(path, SizeStatus::InvalidArgument, static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER));
        size.elapsed = elapsedSince(started);
        return size;
    }

    try {
        const std::wstring extended = toExtendedPath(path);
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!::GetFileAttributesExW(extended.c_str(), GetFileExInfoStandard, &attributes)) {
            const DWORD error = ::GetLastError();
            FileSize size = failure(path, classify(error), error);
            size.elapsed = elapsedSince(started);
            return size;
        }

        FileSize size = asEntry((static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
                                    static_cast<std::uint64_t>(attributes.nFileSizeLow),
                                flagsFromAttributes(attributes.dwFileAttributes));
        const bool allocatedApplies = !hasFlag(size.flags, FileFlags::Directory) &&
                                      !hasFlag(size.flags, FileFlags::ReparsePoint);
        if (allocatedApplies) {
            finishEntry(size, path, readAllocatedSize(extended));
        }
        size.elapsed = elapsedSince(started);
        return size;
    } catch (const std::bad_alloc&) {
        FileSize size = failure(path, SizeStatus::Unavailable, static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY));
        size.elapsed = elapsedSince(started);
        return size;
    }
}

FileSize measureHandle(HANDLE file, std::wstring_view pathForLog) noexcept {
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        FileSize size =
            failure(pathForLog, SizeStatus::InvalidArgument, static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER));
        size.elapsed = elapsedSince(started);
        return size;
    }

    // FileStandardInfo: EndOfFile и AllocationSize из одного чтения, поэтому
    // пара согласована — файл не успевает измениться между ними. Это и есть
    // отличие от measurePath, где два чтения принадлежат разным моментам.
    FILE_STANDARD_INFO standard{};
    if (!::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, static_cast<DWORD>(sizeof standard))) {
        const DWORD error = ::GetLastError();
        FileSize size = failure(pathForLog, classify(error), error);
        size.elapsed = elapsedSince(started);
        return size;
    }

    // Признаки — вторым вызовом, тоже по дескриптору: путь заново не
    // разрешается, а значит, указать на другой файл уже не может.
    FILE_ATTRIBUTE_TAG_INFO tags{};
    FileFlags flags = FileFlags::None;
    if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tags, static_cast<DWORD>(sizeof tags))) {
        flags = flagsFromAttributes(tags.FileAttributes);
    }

    if (standard.EndOfFile.QuadPart < 0) {
        // Длина файла отрицательной быть не может: либо драйвер, либо неверная
        // пара «дескриптор, элемент».
        FileSize size = failure(pathForLog, SizeStatus::InvalidArgument,
                                static_cast<std::uint32_t>(ERROR_INVALID_DATA));
        size.elapsed = elapsedSince(started);
        return size;
    }

    FileSize size = asEntry(static_cast<std::uint64_t>(standard.EndOfFile.QuadPart), flags);
    if (hasFlag(flags, FileFlags::Directory)) {
        // Каталог: остаётся NotApplicable, как и в measurePath/measureFindData.
        // Речь о стоимости самого блока каталога, а она у всех трёх входов
        // считается одинаково — через addDirectory в вызывающем.
    } else if (standard.AllocationSize.QuadPart < 0) {
        // То же для аллоцированного: честная «оценка по логическому» лучше
        // отрицательного числа, которое потом посчитают как освобождение.
        size.allocatedStatus = SizeStatus::InvalidArgument;
        size.allocatedWin32Error = static_cast<std::uint32_t>(ERROR_INVALID_DATA);
        logAllocatedUnknown(pathForLog, size);
    } else {
        size.allocatedBytes = static_cast<std::uint64_t>(standard.AllocationSize.QuadPart);
        size.allocatedKnown = true;
        size.allocatedStatus = SizeStatus::Ok;
        gMeasured.fetch_add(1, std::memory_order_relaxed);
    }
    size.elapsed = elapsedSince(started);
    return size;
}

FileSize measureFindData(const WIN32_FIND_DATAW& findData, std::wstring_view path) noexcept {
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    FileSize size = asEntry((static_cast<std::uint64_t>(findData.nFileSizeHigh) << 32) |
                                static_cast<std::uint64_t>(findData.nFileSizeLow),
                            flagsFromAttributes(findData.dwFileAttributes));
    const bool allocatedApplies = !hasFlag(size.flags, FileFlags::Directory) &&
                                  !hasFlag(size.flags, FileFlags::ReparsePoint);
    if (allocatedApplies) {
        finishEntry(size, path, queryAllocatedSize(path));
    }
    size.elapsed = elapsedSince(started);
    return size;
}

ClusterSize queryClusterSize(std::wstring_view pathOnVolume) noexcept {
    if (pathOnVolume.empty()) {
        return ClusterSize{0, 0, 0, SizeStatus::InvalidArgument, static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER)};
    }
    try {
        // Путь передаётся как есть, без \\?\: определить том и по короткому
        // пути можно, а лишнее преобразование здесь только добавило бы повод
        // ошибиться (GetDiskFreeSpaceW работает с корнем диска или буквой).
        const std::wstring path(pathOnVolume);
        DWORD sectorsPerCluster = 0;
        DWORD bytesPerSector = 0;
        DWORD freeClusters = 0;
        DWORD totalClusters = 0;
        if (!::GetDiskFreeSpaceW(path.c_str(), &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)) {
            const DWORD error = ::GetLastError();
            return ClusterSize{0, 0, 0, classify(error), error};
        }
        if (sectorsPerCluster == 0 || bytesPerSector == 0) {
            // Нулевой кластер у тома не бывает: нуль приходит от драйвера,
            // который ответил, но ничего не сказал.
            return ClusterSize{0, sectorsPerCluster, bytesPerSector, SizeStatus::Unsupported,
                               static_cast<std::uint32_t>(ERROR_INVALID_DATA)};
        }
        return ClusterSize{static_cast<std::uint64_t>(sectorsPerCluster) *
                               static_cast<std::uint64_t>(bytesPerSector),
                           bytesPerSector, sectorsPerCluster, SizeStatus::Ok, ERROR_SUCCESS};
    } catch (const std::bad_alloc&) {
        return ClusterSize{0, 0, 0, SizeStatus::Unavailable, static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY)};
    }
}

}  // namespace mrproper::platform::vfs
