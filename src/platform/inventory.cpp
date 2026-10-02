// Реализация кэша инвентаризации: единственный файл модуля, где встречается
// windows.h. Наружу (inventory.hpp) уходят только переносимые типы, чтобы
// engine, ui, cli и тесты не тащили Win32 в свои заголовки (SPEC §6.1, ADR-004).
//
// Порядок работы по SPEC §4 FR-1 и §10:
//
//   1) devices::enumerateDiskInterfaces  — какие диски есть (FR-1 п.1);
//   2) platform::queryDiskSize         — размер диска (FR-1 п.3), таймаут внутри;
//   3) platform::readDriveLayout        — разметка (FR-1 п.4), таймаут держим мы;
//   4) volumes::enumerate + привязка extent'ов к разделам (FR-1 п.5);
//   5) platform::queryVolumeSpace      — свободное место (FR-1 п.6);
//   6) core::DiskInventory::fromDisks    — детерминированный порядок, агрегаты и
//                                           проверка согласованности (SPEC §6.3).
//
// Ни один отказ на этих шагах не выходит наружу исключением: всё превращается в
// Issue, DeviceState и строку в degradedReasons, потому что FR-1 требует, чтобы
// «отказавший» диск не ронял приложение, а §10 — чтобы это выглядело как
// degraded-режим с понятным сообщением, а не как пустой экран.

#include "inventory.hpp"

#include <windows.h>

#include <cfgmgr32.h>  // DEV_BROADCAST_DEVICEINTERFACE_W
#include <dbt.h>       // DBT_* — события и типы устройств WM_DEVICECHANGE
#include <winioctl.h>     // GUID_DEVINTERFACE_DISK, GUID_DEVINTERFACE_VOLUME

#include <algorithm>
#include <atomic>
#include <exception>
#include <future>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/units.hpp"
#include "devices.hpp"
#include "layout.hpp"
#include "size_probe.hpp"
#include "volumes.hpp"
#include "win_error.hpp"

namespace mrproper::platform::inventory {
namespace {

// Записи журнала собираются полями вручную (core::logField), а не макросом
// MRP_LOG_*: core::detail::logFieldList разворачивает пакет целиком в один вызов
// logField со всеми аргументами разом, и список из двух и более пар на MSVC не
// разрешается (C2661) — ровно то, из-за чего platform::devices пишет так же.
// Макрос остаётся только там, где полей нет вовсе.

// Значения событий и типов устройств в заголовке — переносимая копия DBT_* из
// winuser.h. Сверка с SDK на этапе компиляции: если Microsoft поменяет число,
// модуль падает на сборке, а не молча перестаёт узнавать WM_DEVICECHANGE.
static_assert(kDeviceNodesChanged == DBT_DEVNODES_CHANGED, "DBT_DEVNODES_CHANGED разошёлся с SDK");
static_assert(kDeviceArrival == DBT_DEVICEARRIVAL, "DBT_DEVICEARRIVAL разошёлся с SDK");
static_assert(kDeviceQueryRemove == DBT_DEVICEQUERYREMOVE, "DBT_DEVICEQUERYREMOVE разошёлся с SDK");
static_assert(kDeviceQueryRemoveFailed == DBT_DEVICEQUERYREMOVEFAILED, "DBT_DEVICEQUERYREMOVEFAILED разошёлся с SDK");
static_assert(kDeviceRemovePending == DBT_DEVICEREMOVEPENDING, "DBT_DEVICEREMOVEPENDING разошёлся с SDK");
static_assert(kDeviceRemoveComplete == DBT_DEVICEREMOVECOMPLETE, "DBT_DEVICEREMOVECOMPLETE разошёлся с SDK");
static_assert(kDeviceTypeSpecific == DBT_DEVICETYPESPECIFIC, "DBT_DEVICETYPESPECIFIC разошёлся с SDK");
static_assert(kDeviceTypeVolume == DBT_DEVTYP_VOLUME, "DBT_DEVTYP_VOLUME разошёлся с SDK");
static_assert(kDeviceTypeDeviceInterface == DBT_DEVTYP_DEVICEINTERFACE, "DBT_DEVTYP_DEVICEINTERFACE разошёлся с SDK");

// Не больше стольких причин деградации в снимке. Причины важны для диагностики,
// но на машине с двадцатью отказавшими дисками двадцать строк в баннере — это уже
// не сообщение, а шум; счётчик отказов остаётся в devices и в логе.
constexpr std::size_t kMaxDegradedReasons = 8;

// Потоки, оставшиеся внутри драйвера после таймаута (см. callBounded). Только
// диагностика: на таймауте вызов не отменяется, поток убирает состояние сам.
std::atomic<std::uint64_t> gAbandonedCalls{0};

// Поколения снимков сквозные для процесса: номер в логе должен показывать
// порядок снимков, даже если кэш был пересоздан.
std::atomic<std::uint64_t> gGeneration{0};

std::uint64_t nextGeneration() noexcept { return gGeneration.fetch_add(1, std::memory_order_relaxed) + 1; }

// --- Хелперы текста --------------------------------------------------------

std::string winErrorText(std::uint32_t code) {
    if (code == 0) return "без кода";
    return platform::win32ErrorText(code) + " (" + std::to_string(code) + ")";
}

std::string elapsedText(std::chrono::milliseconds elapsed) {
    return std::to_string(elapsed.count()) + " мс";
}

// Путь устройства в модели: без завершающего разделителя. devices отдаёт путь с
// разделителем (его требуют IOCTL), а core::PhysicalDisk::devicePath — это
// идентификатор устройства, а не путь для вызова.
std::string stripTrailingSeparators(std::string_view path) {
    std::size_t end = path.size();
    while (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/')) --end;
    return std::string(path.substr(0, end));
}

std::string diskLabel(int diskNumber) {
    return diskNumber >= 0 ? ("диск " + std::to_string(diskNumber)) : std::string("диск без номера");
}

std::string volumeLabel(const core::Volume& volume) {
    return volume.volumeGuidPath.empty() ? std::string("том без пути") : volume.volumeGuidPath;
}

// Маска отказов volumes::Error → строка для лога: перечисление томам не
// вернуло часть данных, и это надо назвать, а не записать «ошибка».
std::string volumeErrorList(volumes::Error errors) {
    static constexpr volumes::Error kFlags[] = {
        volumes::Error::Enumerate, volumes::Error::MountPoints, volumes::Error::Information,
        volumes::Error::Open,      volumes::Error::Extents,     volumes::Error::Timeout,
        volumes::Error::Path,      volumes::Error::Internal,
    };
    std::string out;
    for (const volumes::Error flag : kFlags) {
        if (!volumes::has(errors, flag)) continue;
        if (!out.empty()) out += "|";
        out += volumes::errorName(flag);
    }
    return out.empty() ? std::string("none") : out;
}

// --- Классификация отказов --------------------------------------------------

// Код Win32 → состояние устройства. Нужна для вызовов, которые отдают только код:
// разметка (FR-1 п.4) и CreateFileW внутри соседних модулей. size_probe отдаёт
// собственный ProbeStatus, он переводится рядом.
DeviceState deviceStateFromWin32(std::uint32_t win32Error) noexcept {
    switch (win32Error) {
        case ERROR_SUCCESS:
            return DeviceState::Ok;
        case ERROR_TIMEOUT:
        // WAIT_TIMEOUT (258) больше не приходит: size_probe и storage_query
        // кладут в win32Error настоящий код ERROR_TIMEOUT. Ветка оставлена как
        // страховка от старых снимков и чужих модулей — код 258 в каталоге
        // ERROR_ не значит таймаут и текста системы не имеет.
        case WAIT_TIMEOUT:
            return DeviceState::TimedOut;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_SHARING_VIOLATION:
            return DeviceState::AccessDenied;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NO_MORE_FILES:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_INVALID_HANDLE:
            return DeviceState::NotFound;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return DeviceState::Unsupported;
        case ERROR_INVALID_PARAMETER:
            return DeviceState::NoNumber;
        default:
            return DeviceState::LayoutFailed;
    }
}

DeviceState deviceStateFromProbeStatus(platform::ProbeStatus status) noexcept {
    switch (status) {
        case platform::ProbeStatus::Ok:
            return DeviceState::Ok;
        case platform::ProbeStatus::TimedOut:
            return DeviceState::TimedOut;
        case platform::ProbeStatus::AccessDenied:
            return DeviceState::AccessDenied;
        case platform::ProbeStatus::NotFound:
            return DeviceState::NotFound;
        case platform::ProbeStatus::Unsupported:
            return DeviceState::Unsupported;
        case platform::ProbeStatus::InvalidArgument:
            return DeviceState::NoNumber;
        case platform::ProbeStatus::Unavailable:
            return DeviceState::LayoutFailed;
    }
    return DeviceState::Unknown;
}

// Худшее из состояний устройства: отказ на любом этапе делает диск недоступным в
// целом, но диагностика должна называть самую тяжёлую причину — таймаут важнее
// «драйвер не поддерживает», потому что таймаут означает «диск может быть ещё и
// сломан».
constexpr int statePriority(DeviceState state) noexcept {
    switch (state) {
        case DeviceState::Ok:
            return 0;
        case DeviceState::LayoutFailed:
            return 1;
        case DeviceState::Unsupported:
            return 2;
        case DeviceState::NotFound:
            return 3;
        case DeviceState::AccessDenied:
            return 4;
        case DeviceState::NoNumber:
            return 5;
        case DeviceState::TimedOut:
            return 6;
        case DeviceState::Unknown:
            return 7;
    }
    return 7;
}

DeviceState worse(DeviceState left, DeviceState right) noexcept {
    return statePriority(left) >= statePriority(right) ? left : right;
}

ProbeOutcome combine(platform::ProbeStatus sizeStatus, bool layoutKnown, std::uint32_t layoutError) noexcept {
    ProbeOutcome outcome;
    outcome.sizeKnown = sizeStatus == platform::ProbeStatus::Ok;
    outcome.layoutKnown = layoutKnown;
    if (layoutKnown) {
        outcome.state = worse(deviceStateFromProbeStatus(sizeStatus), DeviceState::Ok);
    } else {
        // Нулевой код при непрочитанной разметке — брак драйвера, а не «всё
        // хорошо»: молча Ok здесь означал бы диск без разделов и без причины.
        const DeviceState layoutState = layoutError == 0 ? DeviceState::LayoutFailed
                                                          : deviceStateFromWin32(layoutError);
        outcome.state = worse(deviceStateFromProbeStatus(sizeStatus), layoutState);
    }
    return outcome;
}

// --- Вызов под таймаутом ----------------------------------------------------

// Таймаут держит вызывающий, а не отмена вызова Win32: вызов уходит в отдельный
// поток, вызывающий ждёт не дольше предела и по его истечении помечает устройство
// недоступным. Это ровно приём size_probe; здесь он нужен там, где OVERLAPPED нет
// вовсе (platform::readDriveLayout, volumes::enumerate).
//
// Плата за приём известна и учитывается: после таймаута поток остаётся внутри
// драйвера до возврата вызова. Состояние он убирает сам, поэтому на следующий
// обход это не влияет — но счётчик gAbandonedCalls растёт, и это видно.
template <typename T, typename Fn>
[[nodiscard]] bool callBounded(std::chrono::milliseconds timeout, T& out, Fn&& call) {
    try {
        if (timeout.count() <= 0) {
            // Явный отказ от ожидания: вызов выполняется в потоке вызывающего.
            out = call();
            return true;
        }
        std::future<T> future = std::async(std::launch::async, std::forward<Fn>(call));
        if (future.wait_for(timeout) != std::future_status::ready) {
            gAbandonedCalls.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        out = future.get();
        return true;
    } catch (const std::bad_alloc&) {
        MRP_LOG_ERROR("inventory.bounded", "не хватило памяти для результата вызова под таймаутом");
    } catch (const std::exception& error) {
        MRP_LOG_ERROR("inventory.bounded", std::string("вызов под таймаутом бросил исключение: ") + error.what());
    } catch (...) {
        MRP_LOG_ERROR("inventory.bounded", "вызов под таймаутом бросил неизвестное исключение");
    }
    return false;
}

// --- Заголовок широковещания WM_DEVICECHANGE --------------------------------

// Чтение lParam. Указатель приходит из системы, поэтому единственная честная
// проверка — SEH: битая или чужая структура обязана дать «не разобрано», а не
// падение процесса (SPEC §5: ни один отказ не роняет приложение).
//
// В функции нет объектов с деструкторами: MSVC не разрешает __try в функции,
// требующей раскрутки стека (C2712), поэтому результат копируется в POD.
bool readBroadcastHeader(std::uintptr_t lParam, std::uint32_t& size, std::uint32_t& deviceType) noexcept {
    size = 0;
    deviceType = 0;
    if (lParam == 0) return false;
    __try {
        const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
        size = header->dbch_size;
        deviceType = header->dbch_devicetype;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        size = 0;
        deviceType = 0;
        return false;
    }
    return size >= sizeof(DEV_BROADCAST_HDR);
}

// Класс устройства из DEV_BROADCAST_DEVICEINTERFACE_W: по нему решается, влияет
// ли событие на карту дисков (GUID_DEVINTERFACE_DISK / GUID_DEVINTERFACE_VOLUME)
// или это сетевой адаптер, принтер и прочее.
bool readDeviceInterfaceClass(std::uintptr_t lParam, std::uint32_t size, GUID& classGuid) noexcept {
    if (lParam == 0 || size < sizeof(DEV_BROADCAST_HDR) + sizeof(GUID)) return false;
    __try {
        const auto* info = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lParam);
        classGuid = info->dbcc_classguid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool isStorageClass(const GUID& classGuid) noexcept {
    return ::IsEqualGUID(classGuid, GUID_DEVINTERFACE_DISK) != 0 ||
           ::IsEqualGUID(classGuid, GUID_DEVINTERFACE_VOLUME) != 0;
}

// --- Контекст сбора --------------------------------------------------------

// Всё, что нужно собирающим функциям помимо их аргументов. Отдельная структура
// вместо длинного списка параметров — и потому, что все получатели состояния
// (issues, degradedReasons) должны быть одни и те же на всём обходе.
struct CollectContext {
    const Options* options{nullptr};
    const Snapshot* previous{nullptr};
    std::vector<Issue>* issues{nullptr};
    std::vector<std::string>* degraded{nullptr};
};

void addIssue(CollectContext& ctx, Stage stage, IssueLevel level, int diskNumber, std::string subject,
              std::uint32_t win32Error, std::string message) {
    if (ctx.issues == nullptr) return;
    Issue issue;
    issue.stage = stage;
    issue.level = level;
    issue.diskNumber = diskNumber;
    issue.subject = std::move(subject);
    issue.win32Error = win32Error;
    issue.message = std::move(message);
    ctx.issues->push_back(std::move(issue));
}

void addDegraded(CollectContext& ctx, std::string reason) {
    if (ctx.degraded == nullptr || ctx.degraded->size() >= kMaxDegradedReasons) return;
    ctx.degraded->push_back(std::move(reason));
}

const DiskProperties* previousProperties(const Snapshot* previous, int diskNumber) noexcept {
    if (previous == nullptr) return nullptr;
    for (const DiskProperties& properties : previous->properties) {
        if (properties.diskNumber == diskNumber) return &properties;
    }
    return nullptr;
}

// --- Сбор одного устройства (FR-1 п.1, п.3, п.4) ---------------------------

// Один диск целиком: размер, разметка, свойства. Отказ на любом шаге оставляет
// диск в списке с номером и путём устройства, но без данных, и заполняет
// DiskProbe — FR-1: «устройство помечается недоступным, приложение не падает».
void collectOneDisk(const devices::DiskInterface& iface, CollectContext& ctx, std::vector<core::PhysicalDisk>& disks,
                    std::vector<DiskProbe>& probes) {
    const Options& options = *ctx.options;
    const auto started = std::chrono::steady_clock::now();

    core::PhysicalDisk disk;
    disk.number = iface.number;
    disk.devicePath = stripTrailingSeparators(iface.devicePath);

    DiskProbe probe;
    probe.diskNumber = iface.number;
    probe.devicePath = iface.devicePath;  // с завершающим разделителем: так возвращает SetupAPI
    probe.instanceId = iface.instanceId;
    probe.numberKnown = iface.numberKnown && iface.number >= 0;

    if (!probe.numberKnown) {
        // Без номера нельзя открыть \\.\PhysicalDriveN, а значит нельзя задать ни
        // один следующий IOCTL. Диск остаётся в карте видимым, но пустым.
        probe.state = DeviceState::NoNumber;
        probe.message = "SetupAPI не дал номер устройства: размер и разметку спросить нечем";
        addIssue(ctx, Stage::Enumerate, IssueLevel::Warning, disk.number, probe.devicePath, 0, probe.message);
        addDegraded(ctx, diskLabel(disk.number) + ": " + probe.message);
        // Путь из модели убирается по той же причине, что и для недоступного
        // диска с номером: core::isDiskUnavailable() считает диск доступным уже
        // по непустому пути устройства, а доступных данных здесь нет.
        disk.devicePath.clear();
        probes.push_back(std::move(probe));
        disks.push_back(std::move(disk));
        return;
    }

    // --- FR-1 п.3: размер диска. Таймаут держит сам size_probe.
    const platform::DiskSizeResult size = platform::queryDiskSize(iface.number, options.deviceTimeout);
    disk.sizeBytes = size.ok() ? size.lengthBytes : std::uint64_t{0};

    // --- FR-1 п.4: разметка. Вызов синхронный, без OVERLAPPED, поэтому таймаут
    // держим мы. Размер диска передаём сразу: с ним layout считает промежутки и
    // проверяет согласованность (FR-2 «вся поверхность»), а второй запрос к
    // устройству — это ещё один шанс упереться в зависший диск.
    platform::LayoutReadResult driveLayout;
    // Захват по значению — не стилистика, а требование таймаута. На таймауте поток
    // остаётся внутри драйвера и работает после возврата отсюда, поэтому он не
    // должен видеть ни диск, ни опции вызывающего: иначе «брошенный» вызов
    // продолжил бы читать уже разрушенный кадр стека. Аргументы readDriveLayout —
    // числа, лямбде достаточно их копий.
    const int diskNumber = disk.number;
    const std::uint64_t diskSizeBytes = disk.sizeBytes;
    const bool layoutAnswered =
        callBounded(options.deviceTimeout, driveLayout,
                    [diskNumber, diskSizeBytes] { return platform::readDriveLayout(diskNumber, diskSizeBytes); });
    const bool layoutKnown = layoutAnswered && driveLayout.ok;
    if (layoutKnown) {
        disk.partitions = platform::toCorePartitions(driveLayout, options.includeUnallocated);
    }

    const ProbeOutcome outcome = combine(size.status, layoutKnown,
                                         layoutAnswered ? driveLayout.winError : static_cast<std::uint32_t>(ERROR_TIMEOUT));
    probe.state = outcome.state;
    probe.sizeKnown = size.ok();
    probe.sizeError = size.win32Error;
    probe.layoutKnown = layoutKnown;
    probe.layoutError = layoutKnown ? std::uint32_t{0}
                                    : (layoutAnswered ? driveLayout.winError : static_cast<std::uint32_t>(ERROR_TIMEOUT));
    probe.partitionCount = disk.partitions.size();

    // Замечания и причины деградации — по каждому отказу отдельно, чтобы журнал
    // отвечал на вопрос «что именно сломалось», а не только «диск недоступен».
    if (!size.ok()) {
        const DeviceState state = deviceStateFromProbeStatus(size.status);
        const std::string subject = devices::physicalDrivePathUtf8(iface.number);
        addIssue(ctx, Stage::DiskSize, state == DeviceState::AccessDenied ? IssueLevel::Warning : IssueLevel::Error,
                 disk.number, subject, size.win32Error,
                 std::string("не удалось получить размер ") + diskLabel(disk.number) + ": " + deviceStateName(state) +
                     ", " + winErrorText(size.win32Error));
        if (state == DeviceState::AccessDenied) {
            // Что именно не прочитано, должно быть написано здесь: разметка
            // (IOCTL_DISK_GET_DRIVE_LAYOUT_EX — FILE_ANY_ACCESS) на этой машине
            // читается без повышения прав, а размера (IOCTL_DISK_GET_LENGTH_INFO
            // — FILE_READ_ACCESS, то есть GENERIC_READ) без них не достаётся.
            // Измерено на Windows 11 22631, medium integrity:
            //   \\.\PhysicalDrive0 + FILE_READ_ATTRIBUTES -> LAYOUT_EX OK PartitionCount=5
            //   \\.\PhysicalDrive0 + FILE_READ_ATTRIBUTES -> LENGTH_INFO FALSE, ERROR_ACCESS_DENIED (5)
            // Раньше текст обещал отказ обоих шагов, и карта в JSON показывала
            // разделы — то есть врёт ровно в том случае, когда данные есть.
            const bool layoutMissing = !layoutKnown;
            addDegraded(ctx, "нет прав на " + subject + " (" + winErrorText(size.win32Error) + ") — " +
                                 (layoutMissing ? "размер и разметка не прочитаны"
                                                : "размер диска не прочитан, разметка прочитана") +
                                 ", карта неполна (нужно повышение прав)");
        } else {
            addDegraded(ctx, diskLabel(disk.number) + " не ответил на запрос размера: " + deviceStateName(state) + ", " +
                                 winErrorText(size.win32Error));
        }
    }
    if (!layoutKnown) {
        const DeviceState state = deviceStateFromWin32(probe.layoutError);
        addIssue(ctx, Stage::DiskLayout, state == DeviceState::AccessDenied ? IssueLevel::Warning : IssueLevel::Error,
                 disk.number, disk.devicePath, probe.layoutError,
                 std::string("разметка ") + diskLabel(disk.number) + " не прочитана: " + deviceStateName(state) + ", " +
                     winErrorText(probe.layoutError));
        addDegraded(ctx, diskLabel(disk.number) + ": разметка не прочитана (" + deviceStateName(state) + ", " +
                             winErrorText(probe.layoutError) + ")");
    } else if (!driveLayout.consistent) {
        addIssue(ctx, Stage::DiskLayout, IssueLevel::Error, disk.number, disk.devicePath, 0,
                 std::string("разметка ") + diskLabel(disk.number) +
                     " противоречива: разделы пересекаются или выходят за размер диска — цифры разделов складывать нельзя");
        addDegraded(ctx, diskLabel(disk.number) + ": разметка противоречива, цифры разделов нельзя складывать");
    }

    // Свойства хранилища (FR-1 п.2) добывают соседние модули
    // (storage_query → модель/серийник/прошивка, bus_type → шина, trim_cache →
    // TRIM и кэш записи); этот модуль их не зовёт, граница описана в inventory.hpp.
    // Переносим прошлые значения, чтобы при отказе устройства не исчезли модель и
    // серийник из карточки диска (FR-2): у диска, который сегодня не ответил,
    // модель не менялась.
    if (options.reuseLastKnownProperties) {
        if (const DiskProperties* known = previousProperties(ctx.previous, disk.number);
            known != nullptr && known->known) {
            applyProperties(disk, *known);
        }
    }

    // core::PhysicalDisk::devicePath в модели — это не просто идентификатор:
    // core::isDiskUnavailable() считает диск доступным уже по непустому пути
    // устройства. Диск, у которого не удалось получить ни размера, ни разметки,
    // обязан выглядеть недоступным (FR-1: «устройство помечается недоступным»),
    // поэтому путь убирается из модели, а сырое значение остаётся в DiskProbe —
    // для журнала, отчёта и следующего обхода.
    if (!probe.sizeKnown && disk.partitions.empty()) {
        disk.devicePath.clear();
    }

    probe.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (probe.state != DeviceState::Ok) {
        if (probe.message.empty()) {
            probe.message = std::string(diskLabel(disk.number)) + ": " + deviceStateName(probe.state);
            if (probe.sizeError != 0) {
                probe.message += ", размер — " + winErrorText(probe.sizeError);
            }
            if (probe.layoutError != 0) {
                probe.message += ", разметка — " + winErrorText(probe.layoutError);
            }
        }
        core::logWarn("inventory.device", "устройство недоступно для полной карты",
                      core::LogFields{core::logField("disk", disk.number),
                                      core::logField("state", deviceStateName(probe.state)),
                                      core::logField("sizeError", probe.sizeError),
                                      core::logField("layoutError", probe.layoutError),
                                      core::logField("elapsedMs", static_cast<std::int64_t>(probe.elapsed.count())),
                                      core::logField("devicePath", probe.devicePath)});
    } else {
        core::logDebug("inventory.device", "диск опрошен",
                       core::LogFields{core::logField("disk", disk.number),
                                       core::logField("sizeBytes", disk.sizeBytes),
                                       core::logField("partitions", static_cast<std::int64_t>(disk.partitions.size())),
                                       core::logField("elapsedMs", static_cast<std::int64_t>(probe.elapsed.count()))});
    }

    probes.push_back(std::move(probe));
    disks.push_back(std::move(disk));
}

// --- Привязка томов к разделам (FR-1 п.5, п.6) -----------------------------

// Том принадлежит разделу, если хоть бы один его extent (номер диска + смещение)
// попадает внутрь раздела. Динамические тома, RAID-тома с собственными томами и
// спаны находятся именно так.
bool extentInside(const core::Volume& volume, int diskNumber, std::uint64_t offset, std::uint64_t length) {
    if (length == 0) return false;
    for (const auto& extent : volume.diskExtents) {
        if (extent.first != diskNumber) continue;
        if (extent.second >= offset && extent.second - offset < length) return true;
    }
    return false;
}

// Корень для GetDiskFreeSpaceExW: точка монтирования предпочтительнее GUID-пути
// (её отдаёт и пользователю, и квоты), но при её отсутствии подходит и сам том.
std::string volumeRoot(const core::Volume& volume) {
    if (!volume.mountPoints.empty()) return volume.mountPoints.front();
    return volume.volumeGuidPath;
}

// volumes — рабочая копия, и это не оптимизация, а требование типов: размер и
// свободное место измеряются один раз на том и записываются в том, который потом
// разойдётся по разделам, поэтому источник должен быть изменяемым. Список томов
// на диске десятки, копия ничего не стоит, а намерение видно из сигнатуры.
void bindVolumes(std::vector<core::PhysicalDisk>& disks, volumes::Enumeration volumes, CollectContext& ctx,
                 std::vector<core::Volume>& unbound, std::vector<DiskProbe>& probes) {
    const Options& options = *ctx.options;
    std::vector<bool> attached(volumes.volumes.size(), false);
    std::vector<std::string> extraAttachments(volumes.volumes.size());

    for (core::PhysicalDisk& disk : disks) {
        for (core::Partition& partition : disk.partitions) {
            for (std::size_t index = 0; index < volumes.volumes.size(); ++index) {
                const core::Volume& volume = volumes.volumes[index];
                if (!extentInside(volume, disk.number, partition.offsetBytes, partition.lengthBytes)) continue;
                if (attached[index]) {
                    // Том виден на нескольких разделах (спан, динамика, повторный
                    // опрос). Учтён он будет один раз — на первом разделе, — иначе
                    // одно место посчиталось бы в сумме по дискам дважды. Факт
                    // множественной привязки попадает в замечания, чтобы карта не
                    // выглядела «потерявшей» том.
                    const std::string where = diskLabel(disk.number) + " раздел " + std::to_string(partition.index);
                    extraAttachments[index] =
                        extraAttachments[index].empty() ? where : extraAttachments[index] + ", " + where;
                    continue;
                }
                partition.hasVolume = true;
                partition.volume = volume;
                attached[index] = true;
                for (DiskProbe& probe : probes) {
                    if (probe.diskNumber == disk.number) {
                        ++probe.volumeCount;
                        break;
                    }
                }
            }
        }
    }

    for (std::size_t index = 0; index < volumes.volumes.size(); ++index) {
        core::Volume& source = volumes.volumes[index];
        if (!attached[index]) {
            // Том не потерян: он в unboundVolumes и в замечаниях. Так ведут себя
            // динамические тома, RAID с собственными томами и отказ
            // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS (FR-1 п.5).
            unbound.push_back(source);
            addIssue(ctx, Stage::VolumeBinding, IssueLevel::Info, -1, source.volumeGuidPath, 0,
                     source.diskExtents.empty()
                         ? volumeLabel(source) +
                               " не привязан к разделу: IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS не ответил "
                               "(динамический диск, RAID или отказ драйвера)"
                         : volumeLabel(source) + " не совпал ни с одним разделом: extent'ы вне перечисленных разделов");
            continue;
        }
        if (!extraAttachments[index].empty()) {
            addIssue(ctx, Stage::VolumeBinding, IssueLevel::Info, -1, source.volumeGuidPath, 0,
                     volumeLabel(source) + " виден на нескольких разделах (" + extraAttachments[index] +
                         "): в суммах учтён один раз");
        }
        if (!options.queryVolumeSpace) continue;
        const std::string root = volumeRoot(source);
        if (root.empty()) continue;
        const std::wstring wideRoot = platform::toUtf16(root);
        if (wideRoot.empty()) {
            addIssue(ctx, Stage::VolumeSpace, IssueLevel::Warning, -1, source.volumeGuidPath, 0,
                     "путь тома не удалось привести в UTF-16: " + root + " — свободное место неизвестно");
            continue;
        }
        const platform::VolumeSpaceResult space = platform::queryVolumeSpace(wideRoot, options.deviceTimeout);
        if (!space.ok()) {
            // Том смонтирован, а размер не пришёл: это пробел в цифрах, которые
            // показывает интерфейс (FR-1 п.6). Раздел без ФС (метка раздела,
            // RAW) таким отказом не считается — там отсутствие размера законно.
            if (!source.mountPoints.empty()) {
                addIssue(ctx, Stage::VolumeSpace, IssueLevel::Warning, -1, root, space.win32Error,
                         "свободное место на томе " + root + " неизвестно: " + winErrorText(space.win32Error));
                addDegraded(ctx, "свободное место на томе " + root + " неизвестно (" + winErrorText(space.win32Error) +
                                     ")");
            }
            continue;
        }
        source.totalBytes = space.space.totalBytes;
        source.freeBytes = space.space.freeBytesTotal;
        // Значения записываются в копию тома внутри раздела: модель иммутабельна,
        // а обход идёт до сборки снимка.
        const std::string key = core::pathKey(source.volumeGuidPath);
        for (core::PhysicalDisk& disk : disks) {
            bool updated = false;
            for (core::Partition& partition : disk.partitions) {
                if (!partition.hasVolume) continue;
                if (core::pathKey(partition.volume.volumeGuidPath) != key) continue;
                partition.volume.totalBytes = source.totalBytes;
                partition.volume.freeBytes = source.freeBytes;
                updated = true;
                break;
            }
            if (updated) break;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Имена перечислений
// ---------------------------------------------------------------------------

const char* deviceStateName(DeviceState state) noexcept {
    switch (state) {
        case DeviceState::Ok:
            return "ok";
        case DeviceState::TimedOut:
            return "timeout";
        case DeviceState::AccessDenied:
            return "access_denied";
        case DeviceState::NotFound:
            return "not_found";
        case DeviceState::Unsupported:
            return "unsupported";
        case DeviceState::LayoutFailed:
            return "layout_failed";
        case DeviceState::NoNumber:
            return "no_number";
        case DeviceState::Unknown:
            break;
    }
    return "unknown";
}

const char* stageName(Stage stage) noexcept {
    switch (stage) {
        case Stage::Enumerate:
            return "enumerate";
        case Stage::DiskSize:
            return "disk_size";
        case Stage::DiskLayout:
            return "disk_layout";
        case Stage::VolumeList:
            return "volume_list";
        case Stage::VolumeSpace:
            return "volume_space";
        case Stage::VolumeBinding:
            return "volume_binding";
        case Stage::Cache:
            return "cache";
    }
    return "unknown";
}

const char* issueLevelName(IssueLevel level) noexcept {
    switch (level) {
        case IssueLevel::Info:
            return "info";
        case IssueLevel::Warning:
            return "warning";
        case IssueLevel::Error:
            return "error";
    }
    return "unknown";
}

const char* refreshReasonName(RefreshReason reason) noexcept {
    switch (reason) {
        case RefreshReason::Manual:
            return "manual";
        case RefreshReason::Startup:
            return "startup";
        case RefreshReason::DeviceChange:
            return "device_change";
        case RefreshReason::Periodic:
            return "periodic";
        case RefreshReason::AfterCleanup:
            return "after_cleanup";
    }
    return "unknown";
}

const char* deviceEventName(DeviceEvent event) noexcept {
    switch (event) {
        case DeviceEvent::DeviceNodesChanged:
            return "devnodes_changed";
        case DeviceEvent::DeviceArrival:
            return "device_arrival";
        case DeviceEvent::DeviceQueryRemove:
            return "device_query_remove";
        case DeviceEvent::DeviceQueryRemoveFailed:
            return "device_query_remove_failed";
        case DeviceEvent::DeviceRemovePending:
            return "device_remove_pending";
        case DeviceEvent::DeviceRemoveComplete:
            return "device_remove_complete";
        case DeviceEvent::DeviceTypeSpecific:
            return "device_type_specific";
        case DeviceEvent::Unknown:
            break;
    }
    return "unknown";
}

const char* deviceChangeActionName(DeviceChangeAction action) noexcept {
    switch (action) {
        case DeviceChangeAction::Ignore:
            return "ignore";
        case DeviceChangeAction::MarkStale:
            return "mark_stale";
        case DeviceChangeAction::Refresh:
            return "refresh";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Свойства хранилища (FR-1 п.2)
// ---------------------------------------------------------------------------

void applyProperties(core::PhysicalDisk& disk, const DiskProperties& properties) noexcept {
    if (!properties.known) return;  // «не опрошено» — не значит «нет»
    if (properties.diskNumber != disk.number) return;
    disk.model = properties.model;
    disk.serial = properties.serial;
    disk.firmware = properties.firmware;
    disk.bus = properties.bus;
    disk.removable = properties.removable;
    disk.readOnly = properties.readOnly;
    disk.trimSupported = properties.trimSupported;
    disk.smartAvailable = properties.smartAvailable;
}

// ---------------------------------------------------------------------------
// Снимок
// ---------------------------------------------------------------------------

const DiskProbe* Snapshot::deviceOf(int diskNumber) const noexcept {
    for (const DiskProbe& probe : devices) {
        if (probe.diskNumber == diskNumber) return &probe;
    }
    return nullptr;
}

bool Snapshot::hasDiskData() const noexcept {
    // Именно «данные», а не «упомянут диск»: core::hasDiskData() считает
    // достаточным непустой путь устройства, а для решения кэша такой снимок
    // бесполезен — карту разделов он не рисует. Смотрим на то, что видно
    // пользователю: размер диска или хотя бы один раздел.
    for (const core::PhysicalDisk& disk : inventory.disks()) {
        if (disk.sizeBytes > 0 || !disk.partitions.empty()) return true;
    }
    return false;
}

std::uint32_t Snapshot::timedOutCount() const noexcept {
    std::uint32_t count = 0;
    for (const DiskProbe& probe : devices) {
        if (probe.state == DeviceState::TimedOut) ++count;
    }
    return count;
}

std::uint32_t Snapshot::unavailableCount() const noexcept {
    std::uint32_t count = 0;
    for (const core::PhysicalDisk& disk : inventory.disks()) {
        if (core::isDiskUnavailable(disk)) ++count;
    }
    return count;
}

std::string Snapshot::degradedSummary() const {
    if (!degraded) return {};
    std::string out;
    for (const std::string& cause : degradedReasons) {
        if (!out.empty()) out += "; ";
        out += cause;
    }
    return out;
}

std::string Snapshot::toText() const {
    std::string out = inventory.toText();
    out += "\n\nСнимок #" + std::to_string(generation) + " (" + refreshReasonName(reason) + "), обход " +
           elapsedText(duration) + ", перечисление дисков: " + (disksComplete ? "завершено" : "прервано") +
           ", перечисление томов: " + (volumesComplete ? "завершено" : "прервано") + "\n";
    out += "Устройства:\n";
    if (devices.empty()) {
        out += "  (нет)\n";
    }
    for (const DiskProbe& probe : devices) {
        out += "  " + diskLabel(probe.diskNumber) + " — " + deviceStateName(probe.state);
        if (probe.sizeKnown) {
            out += ", размер " + core::formatBytes(probe.sizeBytes, 0, true);
        } else if (probe.sizeError != 0) {
            out += ", размер неизвестен (" + winErrorText(probe.sizeError) + ")";
        }
        if (probe.state != DeviceState::Ok) {
            out += ", " + elapsedText(probe.elapsed);
        }
        out += "\n";
    }
    if (degraded) {
        out += "Деградированный режим: " + degradedSummary() + "\n";
    }
    if (!unboundVolumes.empty()) {
        out += "Тома без привязки к разделу: " + std::to_string(unboundVolumes.size()) + "\n";
        for (const core::Volume& volume : unboundVolumes) {
            out += "  " + volumeLabel(volume) + " — " + core::describeVolume(volume) + "\n";
        }
    }
    if (!issues.empty()) {
        out += "Замечания обхода: " + std::to_string(issues.size()) + "\n";
        for (const Issue& issue : issues) {
            out += "  [" + std::string(issueLevelName(issue.level)) + "] " + std::string(stageName(issue.stage)) +
                   ": " + issue.message + "\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Сбор
// ---------------------------------------------------------------------------

std::shared_ptr<const Snapshot> collect(const Options& options, RefreshReason reason, const Snapshot* previous) noexcept {
    const auto started = std::chrono::steady_clock::now();

    auto snapshot = std::make_shared<Snapshot>();
    snapshot->generation = nextGeneration();
    snapshot->reason = reason;
    snapshot->collectedAt = std::chrono::system_clock::now();

    CollectContext ctx;
    ctx.options = &options;
    ctx.previous = previous;
    ctx.issues = &snapshot->issues;
    ctx.degraded = &snapshot->degradedReasons;

    std::vector<core::PhysicalDisk> disks;
    std::vector<DiskProbe> probes;
    volumes::Enumeration volumeList;

    try {
        // --- FR-1 п.1: какие диски есть в системе ---------------------------
        devices::EnumerateOptions enumerateOptions;
        enumerateOptions.presentOnly = options.presentOnly;
        enumerateOptions.maxDevices = options.maxDevices;
        const devices::EnumerateResult enumerated = devices::enumerateDiskInterfaces(enumerateOptions);
        snapshot->disksComplete = enumerated.ok;
        for (const devices::Issue& raw : enumerated.issues) {
            const bool setupFailure =
                raw.stage == devices::Stage::ClassDevs || raw.stage == devices::Stage::EnumInterfaces;
            addIssue(ctx, Stage::Enumerate, setupFailure ? IssueLevel::Error : IssueLevel::Warning, -1, raw.devicePath,
                     raw.error, raw.message);
            if (setupFailure) {
                addDegraded(ctx, "перечисление дисков прервано на этапе " + std::string(devices::stageName(raw.stage)) +
                                     ": " + winErrorText(raw.error) + " — список дисков может быть неполным");
            }
        }

        // --- FR-1 п.3, п.4: диски по одному, с таймаутом на каждый ------------
        for (const devices::DiskInterface& iface : enumerated.disks) {
            collectOneDisk(iface, ctx, disks, probes);
        }

        // --- FR-1 п.5: тома -------------------------------------------------
        const bool volumesAnswered =
            callBounded(options.volumeEnumerationTimeout, volumeList, [] { return volumes::enumerate(); });
        snapshot->volumesComplete = volumesAnswered && volumeList.completed;
        if (!volumesAnswered) {
            addIssue(ctx, Stage::VolumeList, IssueLevel::Error, -1, {}, static_cast<std::uint32_t>(ERROR_TIMEOUT),
                     "перечисление томов не уложилось в " + elapsedText(options.volumeEnumerationTimeout) +
                         " — тома в карту не попали");
            addDegraded(ctx, "перечисление томов не уложилось в " + elapsedText(options.volumeEnumerationTimeout) +
                                 ": тома и точки монтирования в карте не показаны");
            core::logWarn("inventory.volumes", "перечисление томов не уложилось в таймаут",
                          core::LogFields{core::logField("timeoutMs",
                                                          static_cast<std::int64_t>(
                                                              options.volumeEnumerationTimeout.count()))});
        } else {
            if (volumes::failed(volumeList.errors)) {
                addIssue(ctx, Stage::VolumeList, IssueLevel::Warning, -1, {}, volumeList.lastError,
                         "перечисление томов прошло с отказами (" + volumeErrorList(volumeList.errors) +
                             "): данные части томов неполны");
            }
            if (!volumeList.completed) {
                addDegraded(ctx, "перечисление томов не дошло до конца: список томов неполон");
            }
        }
    } catch (const std::bad_alloc&) {
        MRP_LOG_ERROR("inventory.collect", "не хватило памяти на сборе карты дисков");
        addDegraded(ctx, "сбор прерван: не хватило памяти — карта неполная");
    } catch (const std::exception& error) {
        MRP_LOG_ERROR("inventory.collect", std::string("сбор карты дисков бросил исключение: ") + error.what());
        addDegraded(ctx, std::string("сбор прерван: ") + error.what());
    } catch (...) {
        MRP_LOG_ERROR("inventory.collect", "сбор карты дисков бросил неизвестное исключение");
        addDegraded(ctx, "сбор прерван неизвестной ошибкой — карта неполная");
    }

    try {
        bindVolumes(disks, volumeList, ctx, snapshot->unboundVolumes, probes);  // volumeList копируется
    } catch (const std::exception& error) {
        // Привязка томов не обязана ронять обход: карта дисков и разделов уже
        // собрана, терять её из-за тома нельзя.
        MRP_LOG_ERROR("inventory.bind", std::string("привязка томов к разделам бросила исключение: ") + error.what());
        addDegraded(ctx, "тома не привязаны к разделам: сбой на этапе привязки");
    } catch (...) {
        MRP_LOG_ERROR("inventory.bind", "привязка томов к разделам бросила неизвестное исключение");
        addDegraded(ctx, "тома не привязаны к разделам: сбой на этапе привязки");
    }

    // Свойства хранилища: известные переносятся из прошлого снимка (см.
    // collectOneDisk), незапрошенные — пустые записи, чтобы структура была полной
    // и без FR-1 п.2. Порядок по номеру диска: перечисление SetupAPI не
    // гарантирует порядок, а снимок обязан быть детерминированным (SPEC §6.4).
    for (const core::PhysicalDisk& disk : disks) {
        if (const DiskProperties* known = previousProperties(previous, disk.number);
            known != nullptr && known->known) {
            snapshot->properties.push_back(*known);
        } else {
            DiskProperties properties;
            properties.diskNumber = disk.number;
            snapshot->properties.push_back(std::move(properties));
        }
    }
    std::sort(snapshot->properties.begin(), snapshot->properties.end(),
              [](const DiskProperties& left, const DiskProperties& right) { return left.diskNumber < right.diskNumber; });

    // Состояния устройств — в снимок: без них нечего показать серым в интерфейсе
    // и нечего положить в отчёт (FR-2, FR-8).
    snapshot->devices = std::move(probes);

    snapshot->inventory = core::DiskInventory::fromDisks(std::move(disks));
    snapshot->degraded = !snapshot->degradedReasons.empty();
    snapshot->duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    core::logInfo("inventory.collect", "карта дисков собрана",
                  core::LogFields{core::logField("generation", snapshot->generation),
                                  core::logField("reason", refreshReasonName(reason)),
                                  core::logField("disks", static_cast<std::int64_t>(snapshot->inventory.disks().size())),
                                  core::logField("volumes",
                                                 static_cast<std::int64_t>(snapshot->inventory.volumes().size())),
                                  core::logField("timedOut", snapshot->timedOutCount()),
                                  core::logField("unavailable", snapshot->unavailableCount()),
                                  core::logField("degraded", snapshot->degraded),
                                  core::logField("durationMs", static_cast<std::int64_t>(snapshot->duration.count())),
                                  core::logField("disksComplete", snapshot->disksComplete),
                                  core::logField("volumesComplete", snapshot->volumesComplete)});
    if (snapshot->degraded) {
        core::logWarn("inventory.degraded", snapshot->degradedSummary(),
                      core::LogFields{core::logField("generation", snapshot->generation),
                                      core::logField("reasons", static_cast<std::int64_t>(snapshot->degradedReasons.size()))});
    }

    return snapshot;
}

// ---------------------------------------------------------------------------
// WM_DEVICECHANGE
// ---------------------------------------------------------------------------

BroadcastInfo readBroadcastInfo(std::uintptr_t lParam) noexcept {
    BroadcastInfo info;
    std::uint32_t size = 0;
    std::uint32_t deviceType = 0;
    if (!readBroadcastHeader(lParam, size, deviceType)) {
        // lParam == 0 — норма для DBT_DEVNODES_CHANGED: там указателя нет вовсе.
        return info;
    }
    info.parsed = true;
    info.sizeBytes = size;
    info.deviceType = deviceType;
    info.knownDeviceType = deviceType == kDeviceTypeVolume || deviceType == kDeviceTypeDeviceInterface;
    if (deviceType == kDeviceTypeVolume) {
        // Событие по логическому тому: карта дисков меняется всегда.
        info.storageClass = true;
    } else if (deviceType == kDeviceTypeDeviceInterface) {
        GUID classGuid{};
        if (readDeviceInterfaceClass(lParam, size, classGuid)) {
            info.storageClass = isStorageClass(classGuid);
        }
    }
    return info;
}

DeviceChangeAction deviceChangeActionFor(DeviceEvent event, const BroadcastInfo& info) noexcept {
    switch (event) {
        case DeviceEvent::DeviceNodesChanged:
            // Диск появился или исчез как devnode: меняется состав дисков и, у
            // смонтированного тома, точки монтирования. Карта перечитывается
            // целиком.
            return DeviceChangeAction::Refresh;
        case DeviceEvent::DeviceArrival:
        case DeviceEvent::DeviceRemovePending:
        case DeviceEvent::DeviceRemoveComplete:
            // Событие приходит с указателем на интерфейс устройства. Если класс
            // известен и это не хранилище (сетевой адаптер, принтер, HID) — на
            // карту дисков оно не влияет. Класс неизвестен (старая система, OEM
            // событие) — перечитываем: лучше лишний обход, чем карта, которая
            // врёт.
            if (info.knownDeviceType && !info.storageClass) return DeviceChangeAction::MarkStale;
            return DeviceChangeAction::Refresh;
        case DeviceEvent::DeviceQueryRemove:
        case DeviceEvent::DeviceQueryRemoveFailed:
            // Система только спрашивает разрешение на отключение. Ответ придёт
            // отдельным событием, карту трогать рано.
            return DeviceChangeAction::MarkStale;
        case DeviceEvent::DeviceTypeSpecific:
        case DeviceEvent::Unknown:
            break;
    }
    return DeviceChangeAction::MarkStale;
}

DeviceChange classifyDeviceChange(std::uint32_t wParam, std::uintptr_t lParam) noexcept {
    DeviceChange change;
    switch (wParam) {
        case kDeviceNodesChanged:
            change.event = DeviceEvent::DeviceNodesChanged;
            break;
        case kDeviceArrival:
            change.event = DeviceEvent::DeviceArrival;
            break;
        case kDeviceQueryRemove:
            change.event = DeviceEvent::DeviceQueryRemove;
            break;
        case kDeviceQueryRemoveFailed:
            change.event = DeviceEvent::DeviceQueryRemoveFailed;
            break;
        case kDeviceRemovePending:
            change.event = DeviceEvent::DeviceRemovePending;
            break;
        case kDeviceRemoveComplete:
            change.event = DeviceEvent::DeviceRemoveComplete;
            break;
        case kDeviceTypeSpecific:
            change.event = DeviceEvent::DeviceTypeSpecific;
            break;
        default:
            change.event = DeviceEvent::Unknown;
            break;
    }
    const BroadcastInfo info = readBroadcastInfo(lParam);
    change.action = deviceChangeActionFor(change.event, info);
    change.needsRefresh = change.action == DeviceChangeAction::Refresh;
    change.reason = std::string(deviceEventName(change.event)) + " → " + deviceChangeActionName(change.action);
    if (info.parsed) {
        change.reason += info.storageClass ? ", устройство: хранилище"
                                           : (info.knownDeviceType ? ", устройство: не хранилище"
                                                                    : ", класс устройства неизвестен");
    }
    return change;
}

// ---------------------------------------------------------------------------
// Кэш
// ---------------------------------------------------------------------------

InventoryCache::InventoryCache(CacheOptions options) : options_(std::move(options)) {
    if (options_.minRefreshInterval.count() < 0) {
        options_.minRefreshInterval = std::chrono::milliseconds{0};
    }
    // Свежий кэш без снимка — это тоже «нет актуальных данных»: иначе вызывающий,
    // который опрашивает только stale(), решил бы, что показывать нечего и ждать
    // нечего, и карта дисков не была бы запрошена вовсе.
    stale_ = true;
    staleReason_ = "снимка ещё нет";
}

InventoryCache::~InventoryCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    // std::jthread в деструкторе сам запрашивает остановку и присоединяется.
}

std::shared_ptr<const Snapshot> InventoryCache::current() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

std::uint64_t InventoryCache::generation() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ != nullptr ? current_->generation : std::uint64_t{0};
}

bool InventoryCache::busy() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_ || pending_;
}

bool InventoryCache::stale() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stale_;
}

std::string InventoryCache::staleReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return staleReason_;
}

void InventoryCache::ensureWorkerLocked() {
    if (stopping_ || worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token token) { workerLoop(token); });
}

std::shared_ptr<const Snapshot> InventoryCache::refreshNow(RefreshReason reason) {
    return runCollection(reason, true);
}

bool InventoryCache::requestRefresh(RefreshReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    if (reason == RefreshReason::DeviceChange) {
        ++stats_.refreshesFromDeviceChange;
    }
    if (busy_) {
        // Идущий обход начался до события и мог не увидеть его: следующий проход
        // обязателен и паузу не ждёт.
        ++stats_.coalesced;
        pendingForced_ = true;
    } else if (current_ == nullptr) {
        // Первый снимок паузой не ограничивается: ждать нечего, а UI без карты
        // дисков не запускается.
        pendingForced_ = true;
    } else if (options_.minRefreshInterval.count() > 0 && lastRefresh_.time_since_epoch().count() != 0 &&
               std::chrono::steady_clock::now() < lastRefresh_ + options_.minRefreshInterval) {
        // Запрос попал в паузу после предыдущего обхода: счётчик показывает,
        // сколько событий схлопнулось в один перечитываемый проход.
        ++stats_.debounced;
    }
    pendingReason_ = reason;
    pending_ = true;
    ensureWorkerLocked();
    wake_.notify_one();
    return true;
}

DeviceChange InventoryCache::onDeviceChange(std::uint32_t wParam, std::uintptr_t lParam) {
    const DeviceChange change = classifyDeviceChange(wParam, lParam);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.deviceChanges;
    }
    if (change.action == DeviceChangeAction::Ignore) {
        core::logDebug("inventory.devicechange", "событие устройства не влияет на карту дисков",
                       core::LogFields{core::logField("wParam", wParam),
                                       core::logField("event", deviceEventName(change.event)),
                                       core::logField("action", deviceChangeActionName(change.action))});
        return change;
    }
    invalidate(change.reason);
    if (change.needsRefresh && options_.autoRefreshOnDeviceChange) {
        requestRefresh(RefreshReason::DeviceChange);
    }
    core::logInfo("inventory.devicechange", "реакция на WM_DEVICECHANGE",
                  core::LogFields{core::logField("wParam", wParam),
                                  core::logField("event", deviceEventName(change.event)),
                                  core::logField("action", deviceChangeActionName(change.action)),
                                  core::logField("lParam", lParam),
                                  core::logField("reason", change.reason)});
    return change;
}

void InventoryCache::invalidate(std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    stale_ = true;
    if (staleReason_.empty()) {
        staleReason_ = std::move(reason);
    }
}

bool InventoryCache::waitForIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return wake_.wait_for(lock, timeout, [this] { return !pending_ && !busy_; });
}

CacheStats InventoryCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats copy = stats_;
    copy.abandonedCalls = gAbandonedCalls.load(std::memory_order_relaxed);
    return copy;
}

std::string InventoryCache::toText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out = "Кэш карты дисков: ";
    if (current_ == nullptr) {
        out += "снимка ещё нет";
    } else {
        out += "снимок #" + std::to_string(current_->generation) + " (" + refreshReasonName(current_->reason) + ", " +
               elapsedText(current_->duration) + ")";
    }
    out += ", обходов: " + std::to_string(stats_.collections);
    out += ", опубликовано: " + std::to_string(stats_.published);
    out += ", отброшено: " + std::to_string(stats_.rejected);
    out += ", слито запросов: " + std::to_string(stats_.coalesced);
    out += ", отложено по паузе: " + std::to_string(stats_.debounced);
    out += ", событий устройства: " + std::to_string(stats_.deviceChanges);
    out += ", обходов по событию: " + std::to_string(stats_.refreshesFromDeviceChange);
    out += ", таймаутов устройств: " + std::to_string(stats_.timedOutDevices);
    out += ", брошенных вызовов: " + std::to_string(gAbandonedCalls.load(std::memory_order_relaxed));
    out += std::string(", устарел: ") + (stale_ ? "да" : "нет");
    if (stale_ && !staleReason_.empty()) {
        out += " (" + staleReason_ + ")";
    }
    if (current_ != nullptr && current_->degraded) {
        out += "\nДеградированный режим: " + current_->degradedSummary();
    }
    return out;
}

bool InventoryCache::publishable(const Snapshot& next, const Snapshot* previous) noexcept {
    if (next.hasDiskData()) return true;
    if (previous == nullptr || !previous->hasDiskData()) return true;  // нечего терять
    return false;
}

std::shared_ptr<const Snapshot> InventoryCache::runCollection(RefreshReason reason, bool allowPublish) {
    // Сбор последователен: два обхода одновременно открывают одни и те же
    // устройства и только мешают друг другу. Мьютекс не тот, что у состояния:
    // первый можно держать минуты, второе читается постоянно.
    std::lock_guard<std::mutex> collection(collectionMutex_);

    std::shared_ptr<const Snapshot> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = current_;
        busy_ = true;
        ++stats_.collections;
    }
    auto next = collect(options_.collect, reason, previous.get());

    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    stats_.timedOutDevices += next->timedOutCount();
    if (!allowPublish || stopping_) {
        // Разбираемый кэш или остановка приложения: счётчики честные, публикация
        // не нужна — результат всё равно никто не прочитает.
        return current_;
    }
    if (publishable(*next, previous.get())) {
        current_ = std::move(next);
        stale_ = false;
        staleReason_.clear();
        lastRefresh_ = std::chrono::steady_clock::now();
        ++stats_.published;
        return current_;
    }
    // Деградированный режим на уровне кэша: карта без единого диска с данными
    // хуже, чем прошлый снимок, поэтому он остаётся на месте, а кэш честно
    // помечается устаревшим с причиной (FR-1, §10).
    ++stats_.rejected;
    stale_ = true;
    staleReason_ = "обход не дал ни одного диска с данными — показан предыдущий снимок";
    core::logError("inventory.cache",
                   "обход #" + std::to_string(next->generation) +
                       " не дал ни одного диска с данными: снимок не опубликован, показан предыдущий",
                   core::LogFields{core::logField("generation", next->generation),
                                   core::logField("previous", previous != nullptr ? previous->generation
                                                                                   : std::uint64_t{0}),
                                   core::logField("issues", static_cast<std::int64_t>(next->issues.size()))});
    return current_;
}

void InventoryCache::workerLoop(std::stop_token token) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (!pending_) {
            wake_.wait(lock, token, [this] { return stopping_ || pending_; });
            if (stopping_) return;
        }
        // Пауза между обходами: WM_DEVICECHANGE приходит пачками (монтирование
        // тома — несколько событий подряд), и каждый полный обход заново открывает
        // все диски. Запрос поверх идущего обхода и первый снимок паузу обходят.
        if (!pendingForced_ && options_.minRefreshInterval.count() > 0 &&
            lastRefresh_.time_since_epoch().count() != 0) {
            const auto earliest = lastRefresh_ + options_.minRefreshInterval;
            if (std::chrono::steady_clock::now() < earliest) {
                wake_.wait_until(lock, token, earliest,
                                 [this, earliest] { return stopping_ || std::chrono::steady_clock::now() >= earliest; });
                if (stopping_) return;
            }
        }
        const RefreshReason reason = pendingReason_;
        pendingReason_ = RefreshReason::Manual;
        pending_ = false;
        pendingForced_ = false;
        lock.unlock();
        runCollection(reason, true);
        lock.lock();
    }
}

}  // namespace mrproper::platform::inventory
