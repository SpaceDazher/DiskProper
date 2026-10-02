// Реализация модуля томов (SPEC §4 FR-1 п.5, п.6). Контракт, границы и
// список «чего модуль не делает» — в volumes.hpp; здесь только код.
//
// Порядок чтения: контракт модуля → findExtents/readExtents (единственное
// место с таймаутом) → сборка Probe в queryWide → перечисление в enumerate().
//
// Слой Win32 (SPEC §6.1, ADR-004): единственное место проекта, где допустим
// windows.h. Никаких WinAPI из этого файла в ядро не утекает — наружу выходят
// только core::Volume и целые числа, поэтому юнит-тесты карты разделов
// собираются на любом хосте.
#include "volumes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "log.hpp"

namespace mrproper::platform::volumes {
namespace {

// ---------------------------------------------------------------------------
// Пределы
//
// Буферы Win32 в этой части API растут по требованию: длинное имя тома или
// длинный путь точки монтирования возвращают ERROR_MORE_DATA вместе с числом,
// которое нужно. Потолок нужен, чтобы «растущий» буфер не превратился в
// бесконечное выделение на неисправном драйвере.
// ---------------------------------------------------------------------------

constexpr std::size_t kVolumeNameChars = 512;   // минимум, который требует FindFirstVolumeW
constexpr std::size_t kMountPointChars = 1024;  // типовой путь точки монтирования
constexpr std::size_t kMaxNameChars = 32768;    // потолок роста буфера имён
constexpr std::size_t kLabelChars = 261;        // MAX_PATH: максимальная метка тома
constexpr std::size_t kFileSystemChars = 64;    // «NTFS», «exFAT», «ReFS» — с запасом

// Сколько extent'ов ожидается у обычного тома (диск + смещение) и сколько
// пробуем при ERROR_MORE_DATA (спан- и динамические тома). 128 — с большим
// запасом: больше том на одной машине не собирает, а нехватка бафера честно
// попадает в журнал, а не выглядит как «у тома один диск».
constexpr DWORD kFirstExtentCount = 8;
constexpr DWORD kMaxExtentCount = 128;

// Права, с которыми открывают том. Разделение не формальное: с нулём прав
// хендл годится для запроса extent'ов, а GetVolumeInformationByHandleW
// требует именно GENERIC_READ, и открыть том на чтение может не хватить прав
// (том read-only, том без буквы под BitLocker).
constexpr DWORD kReadAccess = GENERIC_READ;
constexpr DWORD kQueryAccess = 0;

// ---------------------------------------------------------------------------
// RAII
// ---------------------------------------------------------------------------

// Минимальный RAII для HANDLE. Общий win_handle.hpp (задача 31) этому модулю
// не выдавался, поэтому обёртка локальная и держит ровно то, что нужно здесь:
// одно значение, одно закрытие, адрес под ручку поиска. Дублирование
// намеренное и недолгое — при появлении общей обёртки заменяется ею.
class Handle {
public:
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset(other.value_);
            other.value_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~Handle() { reset(); }

    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    bool reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) ::CloseHandle(value_);
        value_ = value;
        return valid();
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

// Буфер под ответ IOCTL. Выравнивание здесь не педантизм: в ответе есть
// LARGE_INTEGER, а vector<unsigned char> выровнен по одному байту, и приведение
// такого адреса к структуре — undefined behavior.
class AlignedBytes {
public:
    explicit AlignedBytes(std::size_t bytes)
        : words_((bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0) {}
    void* data() noexcept { return words_.data(); }
    std::size_t size() const noexcept { return words_.size() * sizeof(std::uint64_t); }

private:
    std::vector<std::uint64_t> words_;
};

// ---------------------------------------------------------------------------
// Преобразования и обработка строк
// ---------------------------------------------------------------------------

constexpr bool isDriveLetter(wchar_t letter) noexcept {
    return (letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z');
}

// UTF-8 → UTF-16. MB_ERR_INVALID_CHARS вместо «заменить на »: молча
// испорченный путь тома хуже явного отказа, потому что он выглядит как
// несуществующий том.
std::wstring toWide(std::string_view utf8) noexcept {
    if (utf8.empty()) return {};
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                              static_cast<int>(utf8.size()), wide.data(), needed);
    if (written <= 0) return {};
    wide.resize(static_cast<std::size_t>(written));
    return wide;
}

// Хвостовые разделители — вон. «\\?\Volume{GUID}\» и «\\?\Volume{GUID}» —
// один и тот же том, а модель хранит второй вид (core/model.hpp).
std::wstring_view trimTrailingSeparators(std::wstring_view text) noexcept {
    while (text.size() > 1 && (text.back() == L'\\' || text.back() == L'/')) {
        text.remove_suffix(1);
    }
    return text;
}

// Путь тома в том виде, в каком его ждут GetVolumePathNamesForVolumeNameW и
// GetVolumeInformationW: с завершающим разделителем.
std::wstring withTrailingSeparator(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.back() == L'\\' || text.back() == L'/') return std::wstring(text);
    std::wstring result(text);
    result.push_back(L'\\');
    return result;
}

// Точка монтирования в виде модели: «C:\Mount\» → «C:\Mount».
std::wstring normalizeMountPoint(std::wstring_view text) {
    if (text.empty()) return {};
    // Корень «X:\» — единственное место, где хвостовой разделитель обязателен:
    // «C:» — это «текущий каталог диска C», а не корень тома. Схлопывать его
    // нельзя (core::disk_model::isVolumeRootPath).
    if (text.size() == 3 && isDriveLetter(text[0]) && text[1] == L':' && text[2] == L'\\') {
        return std::wstring(text);
    }
    return std::wstring(trimTrailingSeparators(text));
}

// Буква только для точки монтирования вида «C:\». У тома, смонтированного в
// папку («D:\Data»), буквы диска нет, а «\\.\D:» — это уже другой том: его
// extent'ы в ответ на запрос о нашем попали бы в карту разделов молча и неверно.
wchar_t driveRootLetter(std::wstring_view mountPoint) noexcept {
    if (mountPoint.size() != 3 || !isDriveLetter(mountPoint[0]) || mountPoint[1] != L':') return 0;
    if (mountPoint[2] != L'\\' && mountPoint[2] != L'/') return 0;
    return mountPoint[0];
}

// «C:\» → «C:» как путь устройства тома: этот путь в Win32 не принимает
// завершающего разделителя.
std::wstring driveDevicePath(wchar_t letter) {
    std::wstring path = L"\\\\.\\";
    wchar_t upper = letter;
    if (upper >= L'a' && upper <= L'z') upper = static_cast<wchar_t>(upper - L'a' + L'A');
    path.push_back(upper);
    path.push_back(L':');
    return path;
}

// ---------------------------------------------------------------------------
// Логирование
//
// Один сценарий на весь модуль: отказ всегда виден в Probe/Enumeration и всегда
// попадает в журнал с путём и кодом Windows (SPEC §5, §12). Хелперы коды
// возвращают, а печатает их один владелец контекста — иначе в журнале были бы
// строки без пути.
// ---------------------------------------------------------------------------

core::LogFields probeFields(std::string_view path, std::uint32_t win32) {
    core::LogFields fields;
    fields.push_back(core::logField("path", std::string(path)));
    fields.push_back(core::logField("win32", win32));
    return fields;
}

// Имя одного отказа: «extents», «timeout»… Единственная таблица имён на
// модуль, её читают и errorName (публичный), и errorList (журнал).
struct NamedError {
    Error flag;
    const char* name;
};

constexpr NamedError kErrorNames[] = {
    {Error::Enumerate, "enumerate"},
    {Error::MountPoints, "mountPoints"},
    {Error::Information, "information"},
    {Error::Open, "open"},
    {Error::Extents, "extents"},
    {Error::Timeout, "timeout"},
    {Error::Path, "path"},
    {Error::Internal, "internal"},
};

// Отказы одной строкой: «mountPoints|extents». Маска годится для фильтра по
// логу, а человек читает текст.
std::string errorList(Error errors) {
    std::string text;
    for (const NamedError& named : kErrorNames) {
        if (!has(errors, named.flag)) continue;
        if (!text.empty()) text.push_back('|');
        text += named.name;
    }
    return text.empty() ? std::string("none") : text;
}

// ---------------------------------------------------------------------------
// IOCTL с предельным ожиданием
// ---------------------------------------------------------------------------

// Один запрос к устройству, ограниченный kDeviceTimeoutMs (FR-1: «таймаут 2 с
// на устройство»).
//
// Хендл обязан быть открыт с FILE_FLAG_OVERLAPPED — иначе DeviceIoControl с
// OVERLAPPED отвергнут с ERROR_INVALID_PARAMETER. Прервать операцию можно
// только CancelIoEx, и после отмены OVERLAPPED нельзя освобождать, пока ядро
// не закончило, поэтому отменённая операция дожидается через
// GetOverlappedResult(..., TRUE). Иначе «таймаут» превратился бы в
// use-after-free в чистом виде.
bool deviceIoControl(HANDLE device, DWORD controlCode, void* out, DWORD outBytes, DWORD& transferred,
                     DWORD& error) noexcept {
    transferred = 0;
    error = ERROR_SUCCESS;

    const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        error = ::GetLastError();
        return false;
    }
    Handle eventHandle;
    eventHandle.reset(event);

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;

    if (!::DeviceIoControl(device, controlCode, nullptr, 0, out, outBytes, nullptr, &overlapped)) {
        const DWORD last = ::GetLastError();
        if (last != ERROR_IO_PENDING) {
            error = last;
            return false;
        }
        const DWORD wait = ::WaitForSingleObject(event, kDeviceTimeoutMs);
        if (wait != WAIT_OBJECT_0) {
            ::CancelIoEx(device, &overlapped);
            DWORD discarded = 0;
            ::GetOverlappedResult(device, &overlapped, &discarded, TRUE);
            error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : ::GetLastError();
            return false;
        }
    }

    if (!::GetOverlappedResult(device, &overlapped, &transferred, FALSE)) {
        error = ::GetLastError();
        return false;
    }
    return true;
}

Handle openVolume(const std::wstring& path, DWORD access) noexcept {
    Handle handle;
    handle.reset(::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    return handle;
}

// ---------------------------------------------------------------------------
// Extent'ы: привязка тома к дискам (FR-1 п.5)
// ---------------------------------------------------------------------------

struct Extents {
    std::vector<std::pair<int, std::uint64_t>> items;
    bool ok{false};
    std::uint32_t error{0};
};

// Ответ — внешние данные, поэтому число extent'ов сверяется с тем, что реально
// пришло в буфер: иначе драйвер с неверным счётчиком увел бы нас за границу
// выделения, а «одна лишняя запись» в карте разделов — это уже чужой том.
void readExtentsFrom(const void* raw, DWORD transferred, Extents& result) noexcept {
    const auto* head = static_cast<const VOLUME_DISK_EXTENTS*>(raw);
    const std::size_t header = offsetof(VOLUME_DISK_EXTENTS, Extents);
    std::size_t capacity = 0;
    if (static_cast<std::size_t>(transferred) > header) {
        capacity = (static_cast<std::size_t>(transferred) - header) / sizeof(DISK_EXTENT);
    }
    const std::size_t reported = static_cast<std::size_t>(head->NumberOfDiskExtents);
    const std::size_t count = (reported <= capacity) ? reported : capacity;
    result.items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const DISK_EXTENT& extent = head->Extents[index];
        result.items.emplace_back(static_cast<int>(extent.DiskNumber),
                                  static_cast<std::uint64_t>(extent.StartingOffset.QuadPart));
    }
    result.ok = true;
}

// Том не сообщает число extent'ов заранее, поэтому сначала спрашиваем типовой
// том (диск + смещение), а на ERROR_MORE_DATA повторяем с запасом.
Extents readExtents(HANDLE device) noexcept {
    Extents result;
    constexpr DWORD kAttempts[2] = {kFirstExtentCount, kMaxExtentCount};
    for (const DWORD count : kAttempts) {
        AlignedBytes buffer(sizeof(VOLUME_DISK_EXTENTS) + (static_cast<std::size_t>(count) - 1) * sizeof(DISK_EXTENT));
        DWORD transferred = 0;
        DWORD error = ERROR_SUCCESS;
        const bool ok =
            deviceIoControl(device, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, buffer.data(),
                            static_cast<DWORD>(buffer.size()), transferred, error);
        if (ok) {
            readExtentsFrom(buffer.data(), transferred, result);
            return result;
        }
        result.error = error;
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) return result;
    }
    return result;
}

// Запасной путь запроса: по букве диска. Известное различие поведения — на
// части систем запрос по «\\.\C:» отвечает там, где запрос по GUID-пути молчит.
// Ровно один повтор и только для корня «X:\»: путь-папка дала бы extent'ы
// чужого тома.
Extents readExtentsForDriveLetter(std::wstring_view mountPoint) noexcept {
    const wchar_t letter = driveRootLetter(mountPoint);
    if (letter == 0) return {};
    Handle device = openVolume(driveDevicePath(letter), kQueryAccess);
    if (!device.valid()) {
        Extents attempt;
        attempt.error = ::GetLastError();
        return attempt;
    }
    return readExtents(device.get());
}

// ---------------------------------------------------------------------------
// Метка, файловая система, флаги (FR-1 п.6)
// ---------------------------------------------------------------------------

struct VolumeInfo {
    std::wstring label;
    std::wstring fileSystem;
    bool readOnly{false};
};

VolumeInfo infoFromBuffers(const wchar_t* label, const wchar_t* fileSystem, DWORD flags) noexcept {
    VolumeInfo info;
    info.label = label;
    info.fileSystem = fileSystem;
    // FILE_READ_ONLY_VOLUME — единственный флаг GetVolumeInformation, который
    // меняет поведение приложения: в такой том FR-7 не обещает свободное место,
    // а FR-6 не пишет в него файлы.
    info.readOnly = (flags & FILE_READ_ONLY_VOLUME) != 0u;
    return info;
}

// Надёжный вариант из спеки: по хендлу, без повторного разрешения пути.
bool queryInformationByHandle(HANDLE device, VolumeInfo& info) noexcept {
    std::array<wchar_t, kLabelChars> label{};
    std::array<wchar_t, kFileSystemChars> fileSystem{};
    DWORD serialNumber = 0;
    DWORD maxComponentLength = 0;
    DWORD flags = 0;
    const bool ok = ::GetVolumeInformationByHandleW(device, label.data(), static_cast<DWORD>(label.size()),
                                                    &serialNumber, &maxComponentLength, &flags, fileSystem.data(),
                                                    static_cast<DWORD>(fileSystem.size()));
    if (!ok) return false;
    info = infoFromBuffers(label.data(), fileSystem.data(), flags);
    return true;
}

// Откат: вариант по пути открывает том сам и прав на том не требует.
bool queryInformationByPath(const std::wstring& path, VolumeInfo& info) noexcept {
    std::array<wchar_t, kLabelChars> label{};
    std::array<wchar_t, kFileSystemChars> fileSystem{};
    DWORD serialNumber = 0;
    DWORD maxComponentLength = 0;
    DWORD flags = 0;
    const bool ok = ::GetVolumeInformationW(path.c_str(), label.data(), static_cast<DWORD>(label.size()),
                                             &serialNumber, &maxComponentLength, &flags, fileSystem.data(),
                                             static_cast<DWORD>(fileSystem.size()));
    if (!ok) return false;
    info = infoFromBuffers(label.data(), fileSystem.data(), flags);
    return true;
}

// Оба варианта в одном месте, чтобы вызывающий не повторял логику выбора. Код
// отказа читается вызывающим сразу после возврата: между этими вызовами не
// должно оказаться ни одного обращения к Win32, иначе GetLastError отвечает не
// на тот вопрос.
bool queryInformation(const std::wstring& path, HANDLE reader, VolumeInfo& info) noexcept {
    if (reader != nullptr && reader != INVALID_HANDLE_VALUE && queryInformationByHandle(reader, info)) {
        return true;
    }
    return queryInformationByPath(path, info);
}

// ---------------------------------------------------------------------------
// Точки монтирования (FR-1 п.5)
// ---------------------------------------------------------------------------

struct MountPoints {
    std::vector<std::string> items;
    // true — перечисление дошло до конца. У тома без точек монтирования это
    // тоже true: «точек нет» и «перечисление оборвалось» — разные вещи, и
    // второе должно быть видно в карте разделов.
    bool completed{false};
    std::uint32_t error{0};
};

// Точки монтирования тома: GetVolumePathNamesForVolumeNameW.
//
// Именно этот вызов, а не FindFirstVolumeMountPointW. Измерено на этой машине
// (Windows 11 22631, с повышенными правами) на томе
// "\\?\Volume{a07a2c80-863a-45e8-83e8-fb56de919bd1}\", смонтированном на C:::
//
//   FindFirstVolumeMountPointW("\\?\Volume{GUID}\")       -> INVALID_HANDLE_VALUE, 18 (ERROR_NO_MORE_FILES)
//   FindFirstVolumeMountPointW("C:\")                        -> INVALID_HANDLE_VALUE, 18
//   FindFirstVolumeMountPointW("\\?\Volume{GUID}")            -> INVALID_HANDLE_VALUE, 123
//   GetVolumePathNamesForVolumeNameW("\\?\Volume{GUID}\")     -> TRUE, "C:\"
//   GetVolumePathNamesForVolumeNameW("\\?\Volume{GUID}")      -> FALSE, 123
//
// Расхождение в семантике, а не в правах: FindFirstVolumeMountPointW
// перечисляет точки монтирования ВНУТРИ корня, а не «на что смонтирован сам
// том». У тома с единственной буквой внутри себя ничего нет, поэтому честный
// ответ — «дальше нет», и старая ветка кода тихо отдавала пустой список.
// Из-за этого у каждого тома было 0 точек монтирования, в карте висели четыре
// «volume.no_mount_point», а `disks --strict` завершался кодом 4.
//
// Формат ответа — пути, разделённые NUL, с завершающим пустым элементом
// ("C:\\0D:\\0\\0"). Завершающий разделитель в ИМЕНИ тома обязателен (без
// него 123), поэтому путь приводится к виду с одним разделителем перед вызовом.
MountPoints readMountPoints(const std::wstring& volumePath) noexcept {
    MountPoints result;
    if (volumePath.empty()) {
        result.error = ERROR_INVALID_NAME;
        return result;
    }

    // Ровно один завершающий разделитель: без него имя тома не принимается
    // (измерено: 123), а с двумя открывается уже не том.
    std::wstring volumeName = volumePath;
    while (volumeName.size() > 1 && volumeName.back() == L'\\') {
        volumeName.pop_back();
    }
    if (volumeName.size() != 2 || volumeName[1] != L':') {
        // «C:» без разделителя — это «каталог диска C», а не корень тома.
        volumeName.push_back(L'\\');
    }

    std::wstring buffer(kMountPointChars, L'\0');
    for (;;) {
        DWORD needed = 0;
        const BOOL ok = ::GetVolumePathNamesForVolumeNameW(volumeName.c_str(), buffer.data(),
                                                           static_cast<DWORD>(buffer.size()), &needed);
        if (ok) {
            const wchar_t* const first = buffer.data();
            const wchar_t* cursor = first;
            const wchar_t* const limit = first + buffer.size();
            while (cursor < limit && *cursor != L'\0') {
                std::wstring entry(cursor);
                const std::wstring point = normalizeMountPoint(entry);
                if (!point.empty()) result.items.push_back(core::toUtf8(point));
                cursor += entry.size() + 1;
            }
            result.completed = true;
            return result;
        }

        const DWORD code = ::GetLastError();
        if (code == ERROR_MORE_DATA && buffer.size() < kMaxNameChars) {
            const std::size_t wanted = (needed > buffer.size()) ? needed : buffer.size() * 2;
            buffer.assign(std::min(wanted, kMaxNameChars), L'\0');
            continue;
        }
        // Имя тома в неприемлемом виде (например, не "\\?\Volume{...}\"): это
        // не «точек нет», а отказ — различать обязательно (FR-1, §5).
        result.error = code;
        return result;
    }
}

// ---------------------------------------------------------------------------
// Сборка тома
// ---------------------------------------------------------------------------

void queryWide(std::string_view pathUtf8, const std::wstring& widePath, Probe& probe) noexcept {
    // Два пути из одного источника, и разница между ними не косметическая.
    // Измерено на этой машине (NVMe, 4 тома, Windows 11 22631):
    //
    //   CreateFileW("\\?\Volume{GUID}")       -> OK;  EXTENTS -> NumberOfDiskExtents=1 DiskNumber=0
    //   CreateFileW("\\?\Volume{GUID}\")      -> OK;  EXTENTS -> ERROR_INVALID_PARAMETER (87)
    //   CreateFileW("\\.\C:")                -> OK;  EXTENTS -> NumberOfDiskExtents=1 DiskNumber=0
    //   CreateFileW("\\.\C:\")               -> OK;  EXTENTS -> ERROR_INVALID_PARAMETER (87)
    //
    // Разделитель под «\\?\» и после буквы не нормализуется: хендл открывается
    // не на том, а «внутрь» него, и запрос уровня тома на таком хендле не
    // отвечает. Именно отсюда был 0x00000057 (ERROR_INVALID_PARAMETER) в
    // «volume_list: … (mountPoints|open|extents)» и нулевой extentCount.
    //
    // С завершающим разделителем остаётся ровно то, где он обязателен по
    // документации: FindFirstVolumeMountPointW требует корень с «\», и
    // GetVolumeInformationW тоже. Проверено: GetDiskFreeSpaceExW на обоих
    // вариантах отвечает одинаково (на «\\?\Volume{GUID}\» без прав — total=126833651712).
    const std::wstring rootPath = withTrailingSeparator(widePath);   // для точек монтирования и ФС
    const std::wstring devicePath = std::wstring(trimTrailingSeparators(widePath));  // для CreateFileW

    // --- Точки монтирования ------------------------------------------------
    const MountPoints mounts = readMountPoints(rootPath);
    if (mounts.completed) {
        probe.volume.mountPoints = mounts.items;
        probe.mountPointsKnown = true;
    } else {
        probe.errors |= Error::MountPoints;
        probe.lastError = mounts.error;
        core::logWarn("volumes.mountPoints", "не удалось перечислить точки монтирования тома",
                      probeFields(pathUtf8, mounts.error));
    }

    // --- Метка, ФС, флаги --------------------------------------------------
    // Читающий хендл — ускорение, а не условие: queryInformation умеет и по
    // пути (GetVolumeInformationW), и на этой машине без повышения прав
    // GENERIC_READ на том не даётся (ERROR_ACCESS_DENIED, 5), хотя метка и ФС
    // читаются. Поэтому отказ здесь — не пропуск данных, а неудачный путь
    // к тем же данным: отмечать Error::Open можно только когда не открылся
    // НИ ОДИН хендл, то есть когда действительно нечем спросить extent'ы.
    Handle reader = openVolume(devicePath, kReadAccess);
    const DWORD readerError = reader.valid() ? ERROR_SUCCESS : ::GetLastError();

    VolumeInfo info;
    // Не const: ветка «файловой системы нет» превращает отказ в успех, и
    // записывать это нужно здесь же, а не в прокси между вызовами.
    bool informationKnown = queryInformation(rootPath, reader.get(), info);
    if (!informationKnown) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_UNRECOGNIZED_VOLUME) {
            // Тома нет файловой системы: метка раздела, RAW, только что
            // отформатированный том («The volume does not contain a recognized
            // file system», 1005). Для карты разделов это норма, а не отказ:
            // раздел в списке должен остаться, пусть и без ФС.
            probe.fileSystemAbsent = true;
            informationKnown = true;
        } else {
            probe.errors |= Error::Information;
            probe.lastError = code;
            core::logWarn("volumes.information", "не удалось прочитать сведения о томе", probeFields(pathUtf8, code));
        }
    }
    if (informationKnown) {
        probe.informationKnown = true;
        probe.volume.label = core::toUtf8(info.label);
        probe.volume.fileSystem = core::toUtf8(info.fileSystem);
        probe.volume.readOnly = info.readOnly;
    }

    // --- Extent'ы -----------------------------------------------------------
    // Запрос идёт на хендле с нулевым доступом: для IOCTL тома прав на том не
    // нужно, а открыть его на чтение может быть нельзя. Есть только такой
    // хендл — берём читающий.
    Handle opener = reader.valid() ? Handle{} : openVolume(devicePath, kQueryAccess);
    HANDLE device = INVALID_HANDLE_VALUE;
    if (opener.valid()) {
        device = opener.get();
    } else if (reader.valid()) {
        device = reader.get();
    } else {
        // Ни читающего хендла, ни хендла с нулевым доступом: extent'ы спросить
        // нечем, и это уже настоящий пробел в данных (в отличие от отказа
        // открыться на чтение выше). Код от дескриптора важнее кода от
        // читающего хендла: он отвечает на вопрос «почему нечем спросить».
        const DWORD code = reader.valid() ? ERROR_SUCCESS : readerError;
        probe.errors |= Error::Open;
        probe.lastError = code != ERROR_SUCCESS ? code : ::GetLastError();
        core::logWarn("volumes.open", "не удалось открыть том для запроса extent'ов", probeFields(pathUtf8, probe.lastError));
    }

    if (device != INVALID_HANDLE_VALUE) {
        Extents extents = readExtents(device);
        if (!extents.ok) {
            for (const std::string& point : probe.volume.mountPoints) {
                const Extents retry = readExtentsForDriveLetter(toWide(point));
                if (!retry.ok) continue;
                extents = retry;
                break;
            }
        }
        if (extents.ok) {
            probe.extentsKnown = true;
            probe.volume.diskExtents = std::move(extents.items);
            probe.spansMultipleDisks = probe.volume.diskExtents.size() > 1;
        } else {
            probe.errors |= Error::Extents;
            if (extents.error == ERROR_TIMEOUT) probe.errors |= Error::Timeout;
            probe.lastError = extents.error;
            core::logWarn("volumes.extents", "не удалось получить привязку тома к дискам",
                          probeFields(pathUtf8, extents.error));
        }
    }

    // Сведение в одну запись журнала: путь, отказы по именам и что всё-таки
    // узнали. Одна строка на том читается в отчёте, пять — только в отладчике.
    if (failed(probe.errors)) {
        core::LogFields fields = probeFields(pathUtf8, probe.lastError);
        fields.push_back(core::logField("errors", errorList(probe.errors)));
        fields.push_back(core::logField("mountPoints", probe.mountPointsKnown));
        fields.push_back(core::logField("information", probe.informationKnown));
        fields.push_back(core::logField("extents", probe.extentsKnown));
        core::logWarn("volumes.query", "том опрошен частично", std::move(fields));
    }

    probe.found = probe.mountPointsKnown || probe.informationKnown || probe.extentsKnown;
}

}  // namespace

// ---------------------------------------------------------------------------
// Публичный контракт
// ---------------------------------------------------------------------------

Probe query(std::string_view volumeGuidPathUtf8) noexcept {
    Probe probe;
    if (volumeGuidPathUtf8.empty()) {
        probe.errors = Error::Path;
        probe.lastError = ERROR_INVALID_NAME;
        core::logWarn("volumes.query", "запрошен том с пустым путём", probeFields(std::string_view{}, ERROR_INVALID_NAME));
        return probe;
    }
    probe.volume.volumeGuidPath = normalizeGuidPath(volumeGuidPathUtf8);
    try {
        const std::wstring wide = toWide(probe.volume.volumeGuidPath);
        if (wide.empty()) {
            probe.errors = Error::Path;
            probe.lastError = ERROR_INVALID_NAME;
            core::logWarn("volumes.query", "путь тома не удалось привести в UTF-16",
                          probeFields(probe.volume.volumeGuidPath, ERROR_INVALID_NAME));
            return probe;
        }
        queryWide(probe.volume.volumeGuidPath, wide, probe);
    } catch (const std::exception& error) {
        // Спека FR-1: ни один отказ не роняет процесс. Единственное, что здесь
        // может вылететь, — bad_alloc, но и он обязан остаться в журнале.
        probe.errors |= Error::Internal;
        probe.lastError = ERROR_NOT_ENOUGH_MEMORY;
        core::logError("volumes.query", std::string("опрос тома прерван: ") + error.what(),
                       probeFields(probe.volume.volumeGuidPath, ERROR_NOT_ENOUGH_MEMORY));
    }
    return probe;
}

std::vector<std::string> mountPoints(std::string_view volumeGuidPathUtf8) noexcept {
    std::vector<std::string> points;
    try {
        const std::wstring wide = toWide(normalizeGuidPath(volumeGuidPathUtf8));
        if (wide.empty()) {
            if (!volumeGuidPathUtf8.empty()) {
                core::logWarn("volumes.mountPoints", "путь тома не удалось привести в UTF-16",
                              probeFields(normalizeGuidPath(volumeGuidPathUtf8), ERROR_INVALID_NAME));
            }
            return points;
        }
        const MountPoints mounts = readMountPoints(withTrailingSeparator(wide));
        points = mounts.items;
        if (!mounts.completed) {
            core::logWarn("volumes.mountPoints", "не удалось перечислить точки монтирования тома",
                          probeFields(volumeGuidPathUtf8, mounts.error));
        }
    } catch (const std::exception& error) {
        core::logError("volumes.mountPoints", std::string("перечисление точек монтирования прервано: ") + error.what(),
                       probeFields(volumeGuidPathUtf8, ERROR_NOT_ENOUGH_MEMORY));
    }    return points;
}

Enumeration enumerate() noexcept {
    Enumeration result;
    try {
        Handle find;
        std::wstring buffer(kVolumeNameChars, L'\0');
        bool opened = false;
        for (;;) {
            // ВНИМАНИЕ, форма в SDK: FindFirstVolumeW возвращает HANDLE
            // (INVALID_HANDLE_VALUE при отказе) и принимает ДВА аргумента —
            // буфер и его размер. MSDN до сих пор описывает другую сигнатуру
            // (BOOL + HANDLE* наружу, три аргумента); она в заголовочном файле
            // SDK отсутствует, и настоящая реализация читает только RCX и RDX
            // (проверено дизассемблированием KernelBase на этой машине).
            // Код, написанный по MSDN, здесь просто не компилируется.
            if (find.reset(::FindFirstVolumeW(buffer.data(), static_cast<DWORD>(buffer.size())))) {
                opened = true;
                break;
            }
            const DWORD code = ::GetLastError();
            if (code != ERROR_MORE_DATA || buffer.size() >= kMaxNameChars) {
                result.errors |= Error::Enumerate;
                result.lastError = code;
                core::logError("volumes.enumerate", "не удалось начать перечисление томов",
                               probeFields(std::string_view{}, code));
                break;
            }
            buffer.resize(std::min(buffer.size() * 2, kMaxNameChars));
        }
        if (!opened) return result;

        for (;;) {
            const std::wstring guidPath(trimTrailingSeparators(buffer.c_str()));
            if (!guidPath.empty()) {
                const Probe probe = query(core::toUtf8(guidPath));
                result.volumes.push_back(probe.volume);
                result.errors |= probe.errors;
                if (probe.lastError != 0) result.lastError = probe.lastError;
                ++result.probed;
            }
            if (::FindNextVolumeW(find.get(), buffer.data(), static_cast<DWORD>(buffer.size()))) continue;
            const DWORD code = ::GetLastError();
            if (code == ERROR_MORE_DATA && buffer.size() < kMaxNameChars) {
                buffer.resize(std::min(buffer.size() * 2, kMaxNameChars));
                continue;
            }
            if (code == ERROR_NO_MORE_FILES) {
                result.completed = true;
                break;
            }
            result.errors |= Error::Enumerate;
            result.lastError = code;
            core::logError("volumes.enumerate", "перечисление томов оборвано", probeFields(std::string_view{}, code));
            break;
        }
    } catch (const std::exception& error) {
        result.errors |= Error::Internal;
        result.lastError = ERROR_NOT_ENOUGH_MEMORY;
        core::logError("volumes.enumerate", std::string("перечисление томов прервано: ") + error.what(),
                       probeFields(std::string_view{}, ERROR_NOT_ENOUGH_MEMORY));
    }

    core::LogFields fields;
    fields.push_back(core::logField("probed", result.probed));
    fields.push_back(core::logField("completed", result.completed));
    if (failed(result.errors)) {
        fields.push_back(core::logField("errors", errorList(result.errors)));
        fields.push_back(core::logField("win32", result.lastError));
        core::logWarn("volumes.enumerate", "перечисление томов завершено с отказами", std::move(fields));
    } else {
        core::logDebug("volumes.enumerate", "перечисление томов завершено", std::move(fields));
    }
    return result;
}

std::string normalizeGuidPath(std::string_view devicePathUtf8) noexcept {
    std::size_t end = devicePathUtf8.size();
    while (end > 1 && (devicePathUtf8[end - 1] == '\\' || devicePathUtf8[end - 1] == '/')) {
        --end;
    }
    return std::string(devicePathUtf8.substr(0, end));
}

const char* errorName(Error error) noexcept {
    for (const NamedError& named : kErrorNames) {
        if (error == named.flag) return named.name;
    }
    return "none";
}

char volumeDriveLetter(std::string_view mountPointUtf8) noexcept {
    if (mountPointUtf8.size() < 2 || mountPointUtf8[1] != ':') return 0;
    const char letter = mountPointUtf8[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))) return 0;
    return (letter >= 'a') ? static_cast<char>(letter - 'a' + 'A') : letter;
}

}  // namespace mrproper::platform::volumes
