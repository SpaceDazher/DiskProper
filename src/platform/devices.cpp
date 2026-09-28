// Перечисление физических дисков через SetupAPI (SPEC §4 FR-1 п.1):
// SetupDiGetClassDevsW(GUID_DEVINTERFACE_DISK) → SetupDiEnumDeviceInterfaces →
// SetupDiGetDeviceInterfaceDetailW → CreateFileW → IOCTL_STORAGE_GET_DEVICE_NUMBER.
//
// Честность результата важнее полноты: диск, который не открылся или не ответил,
// попадает в список с номером -1 и записью в EnumerateResult::issues, а не
// исчезает. Молчаливый список «как получилось» в приложении, которое дальше
// показывает пользователю «диск недоступен», выглядел бы как «диска нет».
//
// Чего здесь нет намеренно: повышения прав (номер диска — запрос без прав
// доступа), собственных реализаций RAII и кодов ошибок и любых Win32-типов в
// заголовке. Последнее позволяет включить devices.hpp оттуда, где windows.h
// нет, — и не даёт слою расплыться по одному заголовку.
#include "devices.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// INITGUID до любого заголовка с DEFINE_GUID: без него winioctl.h объявляет
// GUID_DEVINTERFACE_DISK как extern "C" const без определения, и символ
// пришлось бы брать из uuid.lib. Порядок включений здесь однозначен — ни
// windows.h, ни setupapi.h winioctl.h не включают (проверено по заголовкам
// установленного SDK 10.0.19041).
#define INITGUID
#include <windows.h>

#include <setupapi.h>
#include <winioctl.h>

// Свои RAII-обёртки и свой разбор кодов ошибок в слое не заводятся: для этого
// есть win_handle.hpp (ADR-001) и win_error.hpp, и вторая реализация того же
// разъедуется с первой за месяц (та же мысль, что в core::disk_model про
// насыщающую арифметику).
#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::devices {
namespace {

// HANDLE: событие отменяемого ожидания и дескриптор устройства. Обход полон
// ранних выходов (путь не получен, диск не открылся, номер не пришёл), и без
// владения дескриптором каждый такой выход оставлял бы его открытым.
using ScopedHandle = platform::unique_handle<platform::KernelHandlePolicy>;

// Политика закрытия набора устройств SetupAPI.
//
// Сам дескриптор — не HANDLE и не HKEY: закрывает его SetupDiDestroyDeviceInfoList.
// Политика заведена здесь, а не дописана в win_handle.hpp, потому что этот файл
// не владеет чужим заголовком, а набор устройств нужен ровно этому обходу.
struct DeviceInfoSetPolicy {
    using HandleType = HDEVINFO;

    // INVALID_HANDLE_VALUE — ошибка SetupDiGetClassDevsW. В отличие от HANDLE,
    // nullptr ошибкой здесь не считается: пустой набор — законное «устройств
    // нет», его надо обойти, просто он ничего не перечислит.
    [[nodiscard]] static bool isValid(HandleType handle) noexcept { return handle != INVALID_HANDLE_VALUE; }

    // Проверка внутри close, а не в вызывающем: деструктор обязан закрывать
    // безусловно, иначе пустой набор станет источником
    // SetupDiDestroyDeviceInfoList(невалидный дескриптор).
    static void close(HandleType handle) noexcept {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            ::SetupDiDestroyDeviceInfoList(handle);
        }
    }
};

using ScopedDeviceInfoSet = platform::unique_handle<DeviceInfoSetPolicy>;

// ---------------------------------------------------------------------------
// Пределы обхода
// ---------------------------------------------------------------------------

// Нижняя граница ответа SetupDiGetDeviceInterfaceDetailW: сама структура плюс
// хотя бы один символ пути. Меньше — значит драйвер или SetupAPI вернули мусор.
constexpr std::uint32_t kMinDetailBytes = static_cast<std::uint32_t>(sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W));

// Верхняя граница: путь устройства — это «\\?\» плюс несколько идентификаторов
// PnP, реально он занимает сотни байт. Килобайт с запасом — предохранитель от
// драйвера, который сообщил ошибочный размер (FR-1: обход не должен ни
// упасть, ни уйти в аллокацию на мусор).
constexpr std::uint32_t kMaxDetailBytes = 64u * 1024u;

// Границы для идентификатора экземпляра (тот же смысл, что у kMaxDetailBytes).
constexpr std::uint32_t kMaxInstanceIdChars = 4096u;

// Права доступа, которые пробуем по очереди, от требовательных к минимальным.
//
// IOCTL_STORAGE_GET_DEVICE_NUMBER имеет FILE_ANY_ACCESS, поэтому последний
// вариант (0) — не формальность: без него диск, который нельзя открыть на
// чтение и запись (том в BitLocker, ключа от которого ещё нет; диск, занятый
// другим процессом), выпал бы из инвентаря целиком, хотя его номер и путь
// узнать можно. GENERIC_READ|GENERIC_WRITE идёт первым не потому, что он
// нужен здесь, а ради единообразия с FR-1 п.3: следующий модуль открывает
// \\.\PhysicalDriveN именно с этими правами, и лишний прогон без надобности
// не нужен.
constexpr DWORD kAccessChain[] = {
    GENERIC_READ | GENERIC_WRITE,
    FILE_READ_ATTRIBUTES,
    0,
};

// ---------------------------------------------------------------------------
// Мелкие помощники
// ---------------------------------------------------------------------------

// Буфер под широкие строки SetupAPI. API считает длину в символах, а читать
// std::wstring_view поверх сырого буфера нельзя: за пределами строки там мусор
// от драйвера. Наружу отдаём только то, что до первого NUL.
class WideBuffer {
public:
    void reset(std::size_t chars) {
        chars_ = chars;
        data_.assign(chars, L'\0');
    }

    [[nodiscard]] DWORD chars() const noexcept { return static_cast<DWORD>(chars_); }
    [[nodiscard]] wchar_t* data() noexcept { return data_.data(); }

    [[nodiscard]] std::wstring_view text() const noexcept {
        if (data_.empty()) {
            return std::wstring_view();
        }
        std::size_t length = 0;
        while (length < data_.size() && data_[length] != L'\0') {
            ++length;
        }
        return std::wstring_view(data_.data(), length);
    }

private:
    std::vector<wchar_t> data_;
    std::size_t chars_{};
};

// Блок ответа SetupDiGetDeviceInterfaceDetailW: структура и следующий за ней
// путь лежат в одном куске памяти. Берём массив структур, а не vector<std::byte>
// — выравнивание тогда гарантирует сама структура.
using DetailBlock = SP_DEVICE_INTERFACE_DETAIL_DATA_W;

// Сколько элементов массива нужно, чтобы покрыть detailSize байт.
constexpr std::size_t detailCapacity(std::size_t detailBytes) noexcept {
    return (detailBytes + sizeof(DetailBlock) - 1) / sizeof(DetailBlock);
}

// Путь устройства из блока ответа — в пределах того размера, который вернул
// SetupAPI. Доверять «строка обязательно чем-то кончится NUL» нельзя: при
// обрезанном ответе это чтение за пределами буфера.
std::wstring_view detailPath(const DetailBlock* detail, std::size_t detailBytes) noexcept {
    constexpr std::size_t pathOffset = offsetof(DetailBlock, DevicePath);
    if (detail == nullptr || detailBytes <= pathOffset) {
        return std::wstring_view();
    }
    const std::size_t maxChars = (detailBytes - pathOffset) / sizeof(wchar_t);
    const wchar_t* const first = detail->DevicePath;
    std::size_t length = 0;
    while (length < maxChars && first[length] != L'\0') {
        ++length;
    }
    return std::wstring_view(first, length);
}

// Завершающий разделитель у пути устройства — ровно один.
//
// SetupDiGetDeviceInterfaceDetailW уже возвращает путь с завершающим '\'
// (того же требует FR-1 п.1), но полагаться на это молча нельзя: второй
// разделитель даёт «\\?\X#&…\\», который не открывается и ломает
// SetupDiOpenDevRegKey и запросы WMI. Поэтому проверяем, а не добавляем.
std::wstring withTrailingSeparator(std::wstring path) {
    if (path.empty() || path.back() == L'\\') {
        return path;
    }
    path.push_back(L'\\');
    return path;
}

// Итог одного запроса номера диска: получилось или нет, на каком этапе и с
// каким кодом. Отдельная структура нужна вызывающему, чтобы в замечание
// попал точный этап (открытие не сработало или не ответил IOCTL) — по одному
// коду это не различить.
struct QueryOutcome {
    bool ok{false};
    Stage stage{Stage::OpenDevice};
    std::uint32_t win32Error{};
    RawDeviceNumber value{};
};

// Открывает устройство, перебирая права доступа, и ждёт открытия не дольше
// kDeviceTimeoutMs.
//
// Про границу этого таймаута, чтобы её не переоценили: CreateFileW синхронен,
// и если драйвер диска не вернёт управление из открытия, ждать тут нечего —
// единственная граница это такой вызов в рабочем потоке пула (SPEC §6.4), и
// это делает вызывающий. Здесь ловится зависание на IOCTL: дескриптор открыт с
// FILE_FLAG_OVERLAPPED, поэтому запрос уходит асинхронно и ждёт своего события.
bool openForQuery(const std::wstring& devicePath, ScopedHandle& out, std::uint32_t& win32Error) {
    for (const DWORD access : kAccessChain) {
        ScopedHandle device = platform::adopt(::CreateFileW(devicePath.c_str(), access,
                                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                                            FILE_FLAG_OVERLAPPED, nullptr));
        if (device.valid()) {
            // std::move обязателен: device — именованный lvalue, а перенос
            // владения живёт в операторе присваивания от rvalue.
            out = std::move(device);
            return true;
        }
        win32Error = ::GetLastError();
    }
    return false;
}

// IOCTL_STORAGE_GET_DEVICE_NUMBER на уже открытом устройстве.
bool sendDeviceNumber(HANDLE device, RawDeviceNumber& out, std::uint32_t& win32Error) {
    // Буфер ответа живёт на куче намеренно. После неудачной отмены драйвер
    // теоретически ещё может в него писать, и освобождать такую память нельзя
    // (см. ветку таймаута). Утечка — 12 байт на одно зависшее устройство за
    // жизнь процесса; молчаливый use-after-free дороже.
    auto buffer = std::make_unique<RawDeviceNumber>();

    ScopedHandle event = platform::adopt(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        win32Error = ::GetLastError();
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();

    DWORD returned = 0;
    const BOOL started =
        ::DeviceIoControl(device, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, buffer.get(),
                          static_cast<DWORD>(sizeof(RawDeviceNumber)), &returned, &overlapped);
    if (started == FALSE) {
        const std::uint32_t startedError = ::GetLastError();
        if (startedError != ERROR_IO_PENDING) {
            win32Error = startedError;
            return false;
        }
        const DWORD wait = ::WaitForSingleObject(event.get(), kDeviceTimeoutMs);
        if (wait == WAIT_TIMEOUT) {
            // Диск не ответил за 2 с — FR-1: помечаем недоступным и идём дальше.
            ::CancelIoEx(device, &overlapped);
            if (::WaitForSingleObject(event.get(), kCancelGraceMs) != WAIT_OBJECT_0) {
                // Отмена не сработала: буфер утекает намеренно, иначе драйвер
                // запишет в уже освобождённую память.
                (void)buffer.release();
            }
            win32Error = ERROR_TIMEOUT;
            return false;
        }
        if (wait != WAIT_OBJECT_0) {
            win32Error = ::GetLastError();
            return false;
        }
    }

    // GetOverlappedResult нужен в обоих случаях: при синхронном завершении
    // событие не сигнализируется, а число принятых байт есть только здесь.
    if (::GetOverlappedResult(device, &overlapped, &returned, FALSE) == FALSE) {
        win32Error = ::GetLastError();
        return false;
    }
    if (static_cast<std::size_t>(returned) < sizeof(RawDeviceNumber)) {
        // Ответ короче структуры: доверять таким байтам нельзя, номер из них
        // был бы мусором, который выглядел бы как «диск 41237».
        win32Error = ERROR_INVALID_DATA;
        return false;
    }
    out = *buffer;
    return true;
}

// Открыть, спросить номер, закрыть — весь цикл FR-1 п.1 на одном устройстве.
QueryOutcome queryDeviceNumberLocked(const std::wstring& devicePath) {
    QueryOutcome outcome;

    ScopedHandle device;
    std::uint32_t openError = ERROR_SUCCESS;
    if (!openForQuery(devicePath, device, openError)) {
        outcome.stage = Stage::OpenDevice;
        outcome.win32Error = openError;
        return outcome;
    }

    RawDeviceNumber number{};
    std::uint32_t queryError = ERROR_SUCCESS;
    if (!sendDeviceNumber(device.get(), number, queryError)) {
        outcome.stage = Stage::DeviceNumber;
        outcome.win32Error = queryError;
        return outcome;
    }

    outcome.ok = true;
    outcome.value = number;
    return outcome;
}

// Идентификатор экземпляра («USBSTOR\DISK&VEN_…») — для отчёта и разбора
// «что это за диск» (FR-8). Для инвентаря он необязателен, поэтому неудача —
// пустая строка плюс замечание уровня InstanceId, а не повод выкинуть диск.
std::wstring queryInstanceId(HDEVINFO infoSet, SP_DEVINFO_DATA& deviceInfo, std::uint32_t& win32Error) {
    DWORD chars = 0;
    if (::SetupDiGetDeviceInstanceIdW(infoSet, &deviceInfo, nullptr, 0, &chars) == FALSE) {
        win32Error = ::GetLastError();
        return {};
    }
    if (chars == 0 || chars > kMaxInstanceIdChars) {
        win32Error = ERROR_INVALID_DATA;
        return {};
    }

    WideBuffer buffer;
    buffer.reset(chars);
    if (::SetupDiGetDeviceInstanceIdW(infoSet, &deviceInfo, buffer.data(), buffer.chars(), nullptr) == FALSE) {
        win32Error = ::GetLastError();
        return {};
    }
    win32Error = ERROR_SUCCESS;
    return std::wstring(buffer.text());
}

// ---------------------------------------------------------------------------
// Обход
// ---------------------------------------------------------------------------

// Поля записи. Макросы MRP_LOG_* в core/log.hpp для списка из двух и более
// пар непригодны: detail::logFieldList разворачивает пакет целиком в один
// вызов logField со всеми аргументами разом, и такой вызов не разрешается
// (C2661: нет перегрузки, принимающей столько аргументов). Проверено сборкой
// на этом файле. Чужий заголовок не правим (владелец — автор core::log),
// поэтому поля собираются здесь явно, тем же набором logField.
mrproper::core::LogFields issueFields(const Issue& issue) {
    mrproper::core::LogFields fields{
        mrproper::core::logField("stage", stageName(issue.stage)),
        mrproper::core::logField("devicePath", issue.devicePath),
    };
    if (issue.error != 0) {
        // Текст системы рядом с кодом: «ERROR_ACCESS_DENIED = 5» в логе читается
        // хуже, чем «Отказано в доступе» (SPEC §5, §12).
        fields.push_back(mrproper::core::logField("error", issue.error));
        fields.push_back(mrproper::core::logField("errorText", platform::win32ErrorText(issue.error)));
    }
    return fields;
}

// Замечание попадает и в результат, и в лог: FR-1 и SPEC §12 требуют, чтобы
// отказ был виден с HRESULT и путём, а не растворился в пустом списке.
void addIssue(EnumerateResult& result, Stage stage, std::string devicePath, std::uint32_t error, std::string message) {
    result.issues.push_back(Issue{stage, std::move(devicePath), error, std::move(message)});
    const Issue& issue = result.issues.back();
    mrproper::core::logWarn("platform.devices.enumerate", issue.message, issueFields(issue));
}

// Один интерфейс: путь устройства, идентификатор экземпляра, номер диска.
bool collectInterface(HDEVINFO infoSet, SP_DEVICE_INTERFACE_DATA& interfaceData, EnumerateResult& result) {
    // SP_DEVINFO_DATA запрашивается в том же (первом) вызове, что и размер:
    // отдельный запрос ради идентификатора экземпляра не нужен.
    SP_DEVINFO_DATA deviceInfo{};
    deviceInfo.cbSize = sizeof(deviceInfo);

    DWORD detailSize = 0;
    if (::SetupDiGetDeviceInterfaceDetailW(infoSet, &interfaceData, nullptr, 0, &detailSize, &deviceInfo) == FALSE) {
        const std::uint32_t error = ::GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            addIssue(result, Stage::InterfaceDetail, {}, error,
                     "SetupDiGetDeviceInterfaceDetailW не вернул размер пути устройства");
            return false;
        }
    }
    if (detailSize < kMinDetailBytes || detailSize > kMaxDetailBytes) {
        addIssue(result, Stage::InterfaceDetail, {}, 0,
                 "SetupAPI вернул неправдоподобный размер пути устройства: " + std::to_string(detailSize));
        return false;
    }

    std::vector<DetailBlock> detail(detailCapacity(detailSize));
    detail.front().cbSize = sizeof(DetailBlock);
    if (::SetupDiGetDeviceInterfaceDetailW(infoSet, &interfaceData, detail.data(), detailSize, nullptr, nullptr) ==
        FALSE) {
        addIssue(result, Stage::InterfaceDetail, {}, ::GetLastError(),
                 "SetupDiGetDeviceInterfaceDetailW не вернул путь устройства");
        return false;
    }

    const std::wstring devicePath = withTrailingSeparator(std::wstring(detailPath(detail.data(), detailSize)));
    if (devicePath.empty()) {
        addIssue(result, Stage::InterfaceDetail, {}, 0, "путь устройства пуст");
        return false;
    }

    DiskInterface disk;
    disk.devicePath = platform::toUtf8(devicePath);

    std::uint32_t instanceError = ERROR_SUCCESS;
    const std::wstring instanceId = queryInstanceId(infoSet, deviceInfo, instanceError);
    if (instanceId.empty()) {
        addIssue(result, Stage::InstanceId, disk.devicePath, instanceError,
                 "SetupDiGetDeviceInstanceIdW не вернул идентификатор экземпляра");
    } else {
        disk.instanceId = platform::toUtf8(instanceId);
    }

    const QueryOutcome outcome = queryDeviceNumberLocked(devicePath);
    if (outcome.ok) {
        disk.deviceType = outcome.value.deviceType;
        disk.flags = outcome.value.flags;
        if (outcome.value.deviceNumber > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            // Номер диска — DWORD, а модель держит int (SPEC §6.3). Значение
            // вроде 0xFFFFFFFF у настоящего диска не бывает, но молча
            // обрезать его до -1 значило бы выдать «номер неизвестен» там, где
            // номер есть, просто он неправдоподобен.
            addIssue(result, Stage::DeviceNumber, disk.devicePath, 0,
                     "номер диска не помещается в int: " + std::to_string(outcome.value.deviceNumber));
        } else {
            disk.number = static_cast<int>(outcome.value.deviceNumber);
            disk.numberKnown = true;
            disk.accessible = true;
        }
    } else {
        addIssue(result, outcome.stage, disk.devicePath, outcome.win32Error,
                 outcome.stage == Stage::OpenDevice ? "устройство не открылось"
                                                    : "устройство не ответило на запрос номера");
    }

    // Повтор одного и того же пути (переустановка драйвера, зеркало) в
    // инвентаре выглядел бы как два диска. Молча выбрасывать нельзя, но и
    // замечать каждое повторение незачем — это норма SetupAPI, а не сбой.
    for (const DiskInterface& existing : result.disks) {
        if (existing.devicePath == disk.devicePath) {
            return true;
        }
    }

    // Разные интерфейсы с одним номером: номер выдаёт драйвер, и он обязан
    // быть уникален, поэтому первый остаётся, а расхождение уходит в
    // замечания — выбросить устройство на молчаливом основании нельзя.
    if (disk.numberKnown) {
        for (const DiskInterface& existing : result.disks) {
            if (existing.numberKnown && existing.number == disk.number) {
                addIssue(result, Stage::EnumInterfaces, disk.devicePath, 0,
                         "два интерфейса сообщили один номер диска " + std::to_string(disk.number));
                return true;
            }
        }
    }

    result.disks.push_back(std::move(disk));
    return true;
}

// Детерминированный порядок: карта дисков (FR-2) и отчёт (FR-8) не должны
// дрожать между прогонами (SPEC §6.4). Диски с известным номером идут по
// номеру, диски без номера — после них, по пути: иначе «диск -1» оказался бы
// между настоящими дисками по случаю порядка выдачи SetupAPI.
void sortDisks(std::vector<DiskInterface>& disks) {
    std::stable_sort(disks.begin(), disks.end(), [](const DiskInterface& left, const DiskInterface& right) {
        if (left.numberKnown != right.numberKnown) {
            return left.numberKnown;
        }
        if (left.number != right.number) {
            return left.number < right.number;
        }
        return left.devicePath < right.devicePath;
    });
}

}  // namespace

const char* stageName(Stage stage) noexcept {
    switch (stage) {
        case Stage::ClassDevs:
            return "class_devs";
        case Stage::EnumInterfaces:
            return "enum_interfaces";
        case Stage::InterfaceDetail:
            return "interface_detail";
        case Stage::OpenDevice:
            return "open_device";
        case Stage::DeviceNumber:
            return "device_number";
        case Stage::InstanceId:
            return "instance_id";
    }
    return "unknown";
}

EnumerateResult enumerateDiskInterfaces(const EnumerateOptions& options) {
    EnumerateResult result;

    DWORD classFlags = DIGCF_DEVICEINTERFACE;
    if (options.presentOnly) {
        classFlags |= DIGCF_PRESENT;
    }

    // INVALID_HANDLE_VALUE — ошибка; пустой (nullptr) набор — законное «дисков
    // нет».
    ScopedDeviceInfoSet infoSet = platform::adopt<DeviceInfoSetPolicy>(
        ::SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, classFlags));
    if (!infoSet.valid()) {
        addIssue(result, Stage::ClassDevs, {}, ::GetLastError(), "SetupDiGetClassDevsW не вернул набор устройств");
        return result;
    }
    if (infoSet.get() == nullptr) {
        // Набор пуст: обходить нечего. Звать SetupDiEnumDeviceInterfaces с
        // nullptr бессмысленно — пришло бы ERROR_INVALID_HANDLE вместо
        // ERROR_NO_MORE_ITEMS, и неполный обход выглядел бы как сбой.
        result.ok = true;
        mrproper::core::logInfo("platform.devices.enumerate", "дисковых интерфейсов нет",
                                mrproper::core::LogFields{mrproper::core::logField("disks", 0)});
        return result;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);

    bool exhausted = false;
    bool enumFailed = false;
    for (DWORD index = 0; index < options.maxDevices; ++index) {
        if (::SetupDiEnumDeviceInterfaces(infoSet.get(), nullptr, &GUID_DEVINTERFACE_DISK, index, &interfaceData) ==
            FALSE) {
            const std::uint32_t error = ::GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) {
                // Нормальное завершение обхода.
                exhausted = true;
            } else {
                enumFailed = true;
                addIssue(result, Stage::EnumInterfaces, {}, error, "обход интерфейсов дисков прерван");
            }
            break;
        }
        // Неудача по одному интерфейсу уже записана в issues — следующий.
        (void)collectInterface(infoSet.get(), interfaceData, result);
    }

    // Цикл дошёл до предела, не спросив у SetupAPI «ещё есть?» — обход неполный,
    // и это надо сказать явно: иначе вызывающий решит, что дисков ровно столько.
    if (!exhausted && !enumFailed) {
        addIssue(result, Stage::EnumInterfaces, {}, 0,
                 "обход остановлен на пределе maxDevices=" + std::to_string(options.maxDevices));
    }

    result.ok = exhausted;
    sortDisks(result.disks);

    mrproper::core::logInfo(
        "platform.devices.enumerate", "физические диски перечислены",
        mrproper::core::LogFields{
            mrproper::core::logField("disks", result.disks.size()),
            mrproper::core::logField("issues", result.issues.size()),
            mrproper::core::logField("complete", result.ok),
        });
    return result;
}

bool queryDeviceNumber(const std::wstring& devicePath, RawDeviceNumber& out, std::uint32_t& win32Error) {
    const QueryOutcome outcome = queryDeviceNumberLocked(devicePath);
    if (!outcome.ok) {
        win32Error = outcome.win32Error;
        return false;
    }
    out = outcome.value;
    win32Error = ERROR_SUCCESS;
    return true;
}

std::wstring physicalDrivePath(int number) {
    // Отрицательный номер — не ошибка вызова, а «такого диска нет»: путь
    // всё равно годится для отчёта, поэтому собираем как есть. Проверка того,
    // что диск есть и отвечает, — это открытие устройства.
    return std::wstring(L"\\\\.\\PhysicalDrive") + std::to_wstring(number);
}

std::string physicalDrivePathUtf8(int number) {
    return platform::toUtf8(physicalDrivePath(number));
}

}  // namespace mrproper::platform::devices
