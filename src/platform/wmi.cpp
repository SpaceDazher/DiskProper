// Реализация модуля WMI (SPEC §4 FR-1 п.7 и п.8). Контракт, границы и
// список «чего модуль не делает» — в wmi.hpp; здесь только код.
//
// Порядок чтения: RAII (ComPtr/Bstr/Variant/EnumeratorGuard) → чтение свойств
// объекта → обход одного запроса (forEachRow) → разбор классов → сессия
// (Session::Impl) → manage-bde.
//
// Слой Win32 (SPEC §6.1, ADR-004): единственное место проекта, где допустимы
// windows.h и COM. Наружу выходят перечислители модуля и целые числа, поэтому
// ни ядро, ни UI не видят IWbemServices.
#include "wmi.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
// WMI: IWbemLocator, IWbemServices, IWbemClassObject, CLSID_WbemLocator и
// IID_IWbemLocator (все они объявлены в WbemCli.h, который подключает
// WbemIdl.h). Именно WbemIdl.h, а не wbemuuid.h: в Windows 10 SDK (10.0.19041.0)
// файла wbemuuid.h нет вообще, а CLSID_WbemLocator лежит в WbemCli.h.
//
// GUID'ы CLSID_WbemLocator/IID_IWbemLocator поставляются библиотекой
// Wbemuuid.lib. CMakeLists.txt слоя общий для всех модулей platform, и правит
// его владелец инфраструктуры, поэтому библиотека подключается отсюда — так же,
// как её подключают примеры MSDN. ole32 и oleaut32 перечислены в
// target_link_libraries слоя.
#pragma comment(lib, "wbemuuid.lib")
#include <WbemIdl.h>
// При WIN32_LEAN_AND_MEAN windows.h не приносит OLE: объявления CoInitializeEx,
// IUnknown, VARIANT и SafeArray живут здесь. Подключаются явно и безопасно
// (заголовки идемпотентны).
#include <ole2.h>
#include <oleauto.h>
// RPC_C_AUTHN_* — константы RPC, объявленные в rpc.h. С ole2.h они не
// приходят, а нужны для CoSetProxyBlanket.
#include <rpc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::wmi {
namespace {

// ---------------------------------------------------------------------------
// Поля записи в журнал
// ---------------------------------------------------------------------------
//
// Собираются явно, а не макросом MRP_LOG_*: макрос разворачивает пакет в
// logFieldList, и подстановка в logField спотыкается на парах, где значение —
// lvalue или указатель (core/log.hpp). Явный список даёт ровно ту же запись,
// и такая форма заголовком журнала прямо предусмотрена.
core::LogFields& addField(core::LogFields& fields, const char* key, const wchar_t* value) noexcept {
    fields.push_back(core::logField(key, value));
    return fields;
}

core::LogFields& addField(core::LogFields& fields, const char* key, const std::string& value) noexcept {
    fields.push_back(core::logField(key, value));
    return fields;
}

// Числа приводятся к int64_t: в журнале HRESULT и счётчики должны читаться
// одинаково, а разрядность size_t на x64 в JSON смотрелась бы неоднородно.
template <typename T>
    requires std::is_arithmetic_v<T>
core::LogFields& addField(core::LogFields& fields, const char* key, T value) noexcept {
    fields.push_back(core::logField(key, static_cast<std::int64_t>(value)));
    return fields;
}

// ---------------------------------------------------------------------------
// Пространства имён и запросы
// ---------------------------------------------------------------------------

// Локальные пространства имён. Удалённый WMI не используется: см. границы в
// wmi.hpp.
constexpr wchar_t kCimV2Namespace[] = L"ROOT\\CIMV2";
constexpr wchar_t kStorageNamespace[] = L"ROOT\\Microsoft\\Windows\\Storage";

// Язык запросов. Все запросы — WQL.
constexpr wchar_t kWql[] = L"WQL";

// Win32_* живут в core WMI с незапамятных времён, поэтому свойства перечислены
// явно: так запрос читается как утверждение о том, что именно нужно.
//
// MSFT_* — наоборот. Их схема меняется от сборки к сборке (Storage Management
// появлялся постепенно), а запрос со свойством, которого в системе нет, падает
// целиком. Поэтому там SELECT *, а свойства читаются по одному: отсутствующее
// даёт пустое поле, а не отказ всего запроса.
constexpr wchar_t kQueryEncryptVolume[] =
    L"SELECT DriveLetter, EncryptState, ConversionStatus, ProtectionStatus, LockStatus, EncryptionMethod "
    L"FROM Win32_EncryptVolume";
constexpr wchar_t kQueryDiskDrives[] =
    L"SELECT Index, DeviceID, PNPDeviceID, Model, SerialNumber, FirmwareRevision, InterfaceType, MediaType, Status, "
    L"Size, Partitions FROM Win32_DiskDrive";
constexpr wchar_t kQueryPhysicalDisks[] = L"SELECT * FROM MSFT_PhysicalDisk";
constexpr wchar_t kQueryVolumes[] = L"SELECT * FROM MSFT_Volume";
constexpr wchar_t kQueryStorageFaults[] = L"SELECT * FROM MSFT_StorageFault";

// ---------------------------------------------------------------------------
// Коды значений классов
//
// Числа взяты из схемы (ValueMap в msft_*.mof Windows SDK и документация
// Win32_EncryptVolume) и служат для ОДНОЙ вещи: не превратить незнакомое
// значение в «зашифровано» или «не зашифровано». Всё нераспознанное становится
// Unknown, а сырое число остаётся в структуре результата.
// ---------------------------------------------------------------------------

// Win32_EncryptVolume.EncryptState.
constexpr std::uint32_t kEncryptStateEncrypted = 1;
constexpr std::uint32_t kEncryptStateNotEncrypted = 2;
constexpr std::uint32_t kEncryptStateEncrypting = 3;
constexpr std::uint32_t kEncryptStateDecrypting = 4;

// Win32_EncryptVolume.ProtectionStatus.
constexpr std::uint32_t kProtectionStatusOn = 1;
constexpr std::uint32_t kProtectionStatusOff = 2;

// Win32_EncryptVolume.LockStatus.
constexpr std::uint32_t kLockStatusUnlocked = 0;
constexpr std::uint32_t kLockStatusLocked = 1;

// Win32_EncryptVolume.ConversionStatus.
constexpr std::uint32_t kConversionStatusInProgress = 1;
constexpr std::uint32_t kConversionStatusFullyEncrypted = 2;
constexpr std::uint32_t kConversionStatusFullyDecrypted = 3;

// MSFT_PhysicalDisk.MediaType.
constexpr std::uint32_t kMediaTypeUnspecified = 0;
constexpr std::uint32_t kMediaTypeHdd = 3;
constexpr std::uint32_t kMediaTypeSsd = 4;

// MSFT_* .OperationalStatus — значения, на которые стоит смотреть в отчёте.
constexpr std::uint32_t kOperationalStatusOk = 2;
constexpr std::uint32_t kOperationalStatusDegraded = 3;
constexpr std::uint32_t kOperationalStatusPredictiveFailure = 5;
constexpr std::uint32_t kOperationalStatusError = 6;
constexpr std::uint32_t kOperationalStatusNonRecoverableError = 7;
constexpr std::uint32_t kOperationalStatusScanNeeded = 0xD00D;
constexpr std::uint32_t kOperationalStatusSpotFixNeeded = 0xD00E;
constexpr std::uint32_t kOperationalStatusFullRepairNeeded = 0xD00F;

// MSFT_Volume.DriveType.
constexpr std::uint32_t kDriveTypeUnknown = 0;
constexpr std::uint32_t kDriveTypeInvalidRoot = 1;
constexpr std::uint32_t kDriveTypeRemovable = 2;
constexpr std::uint32_t kDriveTypeFixed = 3;
constexpr std::uint32_t kDriveTypeRemote = 4;
constexpr std::uint32_t kDriveTypeCdRom = 5;
constexpr std::uint32_t kDriveTypeRamDisk = 6;

// MSFT_Volume.FileSystemType.
constexpr std::uint32_t kFileSystemTypeUnknown = 0;
constexpr std::uint32_t kFileSystemTypeFat = 4;
constexpr std::uint32_t kFileSystemTypeFat16 = 5;
constexpr std::uint32_t kFileSystemTypeFat32 = 6;
constexpr std::uint32_t kFileSystemTypeNtfs = 14;
constexpr std::uint32_t kFileSystemTypeRefs = 15;
constexpr std::uint32_t kFileSystemTypeCsvfsNtfs = 0x8000;
constexpr std::uint32_t kFileSystemTypeCsvfsRefs = 0x8001;

// MSFT_Disk/MSFT_Volume.HealthStatus.
constexpr std::uint16_t kHealthStatusHealthy = 0;
constexpr std::uint16_t kHealthStatusWarning = 1;
constexpr std::uint16_t kHealthStatusUnhealthy = 2;

// RPC_E_INVALID_AUTHN (0x8001010A) — код, на котором останавливается
// ConnectServer к локальному пространству, если CoInitializeSecurity не
// вызвана. Имени в winerror.h установленного SDK (10.0.19041.0) нет, поэтому
// значение записано явно — ради читаемого лога, а не ради сравнения.
constexpr HRESULT kRpcInvalidAuthn = static_cast<HRESULT>(0x8001010AL);

// ---------------------------------------------------------------------------
// Отрицательный кэш классов
// ---------------------------------------------------------------------------
//
// MSFT_* могут отсутствовать: на Windows 10 без Storage Management, на сборках
// без поддержки пулов. Повторная попытка стоит секунды на каждом обходе
// инвентаризации (FR-1 перечитывает картину по WM_DEVICECHANGE), а за время
// работы приложения класс не появляется. Поэтому «здесь такого класса нет»
// запоминается один раз на процесс — как факт окружения, а не как отказ.
std::atomic<bool> g_faultClassUnavailable{false};
std::atomic<bool> g_physicalDiskClassUnavailable{false};
std::atomic<bool> g_volumeClassUnavailable{false};

// ---------------------------------------------------------------------------
// RAII
// ---------------------------------------------------------------------------

// Владение COM-интерфейсом. Ручной Release() здесь означал бы утечку на любом
// раннем возврате, а WMI-обход возвращается часто: каждый Next даёт пакет
// объектов, которые нужно освободить даже при отказе разбора. То же требование,
// что в win_handle.hpp для HANDLE (ADR-001), только для COM.
template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;

    explicit ComPtr(T* value) noexcept
        : value_(value) {
    }

    ~ComPtr() {
        reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : value_(other.value_) {
        other.value_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) reset(other.value_);
        return *this;
    }

    [[nodiscard]] T* get() const noexcept {
        return value_;
    }

    explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

    T* operator->() const noexcept {
        return value_;
    }

    // Взять владение указателем, полученным снаружи.
    void reset(T* value = nullptr) noexcept {
        if (value_ != nullptr && value_ != value) value_->Release();
        value_ = value;
    }

private:
    T* value_{nullptr};
};

// BSTR. Каждый вызов WMI с BSTR требует SysAllocString/SysFreeString, а
// SysFreeString(nullptr) допустим — значит обёртка без проверок безопасна.
class Bstr {
public:
    Bstr() noexcept = default;

    explicit Bstr(const wchar_t* text) noexcept
        : value_(::SysAllocString(text)) {
    }

    ~Bstr() {
        ::SysFreeString(value_);
    }

    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;

    Bstr(Bstr&& other) noexcept
        : value_(other.value_) {
        other.value_ = nullptr;
    }

    [[nodiscard]] BSTR get() const noexcept {
        return value_;
    }

    explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    BSTR value_{nullptr};
};

// VARIANT. IWbemClassObject::Get требует инициализированного VARIANT на входе и
// освобождения на выходе; без VariantClear строка из WMI утекает на каждом
// прочитанном свойстве, а у нас их десятки на строку.
class Variant {
public:
    Variant() noexcept {
        ::VariantInit(&value_);
    }

    ~Variant() {
        ::VariantClear(&value_);
    }

    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;

    [[nodiscard]] VARIANT* put() noexcept {
        return &value_;
    }

    [[nodiscard]] const VARIANT& get() const noexcept {
        return value_;
    }

private:
    VARIANT value_{};
};

// IWbemEnumerator плюс пакет объектов: в освобождении нужны и перечислитель, и
// все объекты, которые остались непрочитанными. Ручной Release() здесь означал
// бы утечку пакета на каждом отказе разбора.
//
// Перечислитель — IEnumWbemClassObject, а не IWbemEnumerator: последнего в
// Windows 10 SDK (10.0.19041.0, WbemCli.h) просто нет, а ExecQuery отдаёт
// именно IEnumWbemClassObject** — ровно то, что нужно для прямого обхода.
class EnumeratorGuard {
public:
    explicit EnumeratorGuard(IEnumWbemClassObject* value) noexcept
        : value_(value) {
    }

    ~EnumeratorGuard() {
        reset();
    }

    EnumeratorGuard(const EnumeratorGuard&) = delete;
    EnumeratorGuard& operator=(const EnumeratorGuard&) = delete;

    void reset(IEnumWbemClassObject* value = nullptr) noexcept {
        if (value_ != nullptr) {
            for (IWbemClassObject* object : objects_) {
                if (object != nullptr) object->Release();
            }
            value_->Release();
        }
        objects_.clear();
        value_ = value;
    }

    // Забрать владение пакетом: объекты освободит guard, вызывающий читает из
    // них свойства и не освобождает ничего сам.
    void adopt(std::vector<IWbemClassObject*>& batch) noexcept {
        objects_ = std::move(batch);
    }

    // Принятое владение для чтения. После adopt() исходный batch пуст (вектор
    // перемещён), поэтому читать из него нельзя: код, который это делал, брал
    // элемент за границей пустого вектора, а в Debug с включённым
    // _ITERATOR_DEBUG_LEVEL это assert. Нашли clang-tidy и разбор волны P1.
    [[nodiscard]] const std::vector<IWbemClassObject*>& objects() const noexcept {
        return objects_;
    }

private:
    IEnumWbemClassObject* value_{nullptr};
    std::vector<IWbemClassObject*> objects_;
};

// ---------------------------------------------------------------------------
// Имя HRESULT
// ---------------------------------------------------------------------------

// Только те коды, ради которых модуль различает отказы. Остальное печатается
// шестнадцатеричным: придумывать имя для незнакомого кода нельзя, потому что
// читатель пойдёт искать его в документации и не найдёт. Коды WMI —
// перечислители tag_WBEMSTATUS из WbemCli.h, а не макросы winerror.h.
std::string hresultNameImpl(HRESULT hr) {
    struct Named {
        HRESULT value;
        const char* name;
    };
    constexpr Named kNamed[] = {
        {S_OK, "S_OK"},
        {S_FALSE, "S_FALSE"},
        {E_FAIL, "E_FAIL"},
        {E_UNEXPECTED, "E_UNEXPECTED"},
        {E_INVALIDARG, "E_INVALIDARG"},
        {E_POINTER, "E_POINTER"},
        {E_OUTOFMEMORY, "E_OUTOFMEMORY"},
        {E_NOINTERFACE, "E_NOINTERFACE"},
        {RPC_E_CHANGED_MODE, "RPC_E_CHANGED_MODE"},
        {RPC_E_TOO_LATE, "RPC_E_TOO_LATE"},
        {kRpcInvalidAuthn, "RPC_E_INVALID_AUTHN"},
        {REGDB_E_CLASSNOTREG, "REGDB_E_CLASSNOTREG"},
        {WBEM_S_TIMEDOUT, "WBEM_S_TIMEDOUT"},
        // WBEM_S_FALSE в таблицу не внесён: он равен 0x00000001, то есть
        // совпадает с S_FALSE, и вторая запись того же значения только
        // вводила бы в заблуждение при чтении кода.
        {WBEM_S_PARTIAL_RESULTS, "WBEM_S_PARTIAL_RESULTS"},
        {WBEM_E_FAILED, "WBEM_E_FAILED"},
        {WBEM_E_NOT_FOUND, "WBEM_E_NOT_FOUND"},
        {WBEM_E_ACCESS_DENIED, "WBEM_E_ACCESS_DENIED"},
        {WBEM_E_OUT_OF_MEMORY, "WBEM_E_OUT_OF_MEMORY"},
        {WBEM_E_NOT_SUPPORTED, "WBEM_E_NOT_SUPPORTED"},
        {WBEM_E_NOT_AVAILABLE, "WBEM_E_NOT_AVAILABLE"},
        {WBEM_E_INVALID_CLASS, "WBEM_E_INVALID_CLASS"},
        {WBEM_E_INVALID_NAMESPACE, "WBEM_E_INVALID_NAMESPACE"},
        {WBEM_E_INVALID_QUERY, "WBEM_E_INVALID_QUERY"},
        {WBEM_E_INVALID_PARAMETER, "WBEM_E_INVALID_PARAMETER"},
        {WBEM_E_PROVIDER_NOT_FOUND, "WBEM_E_PROVIDER_NOT_FOUND"},
        {WBEM_E_PROVIDER_LOAD_FAILURE, "WBEM_E_PROVIDER_LOAD_FAILURE"},
        {WBEM_E_INITIALIZATION_FAILURE, "WBEM_E_INITIALIZATION_FAILURE"},
        {WBEM_E_TRANSPORT_FAILURE, "WBEM_E_TRANSPORT_FAILURE"},
        {WBEM_E_CONNECTION_FAILED, "WBEM_E_CONNECTION_FAILED"},
        {WBEM_E_CALL_CANCELLED, "WBEM_E_CALL_CANCELLED"},
        {WBEM_E_TIMED_OUT, "WBEM_E_TIMED_OUT"},
        {WBEM_E_UNSUPPORTED_LOCALE, "WBEM_E_UNSUPPORTED_LOCALE"},
        {WBEM_E_PRIVILEGE_NOT_HELD, "WBEM_E_PRIVILEGE_NOT_HELD"},
    };
    for (const Named& entry : kNamed) {
        if (entry.value == hr) return entry.name;
    }
    DWORD win32Code = ERROR_SUCCESS;
    if (tryWin32Code(hr, win32Code)) return "HRESULT_FROM_WIN32";
    return "HRESULT";
}

// Перечисление кончилось штатно, а не отказом: у Next это WBEM_S_FALSE.
bool isEndOfEnumeration(HRESULT hr) noexcept {
    return hr == WBEM_S_FALSE || hr == S_FALSE;
}

// ---------------------------------------------------------------------------
// Чтение свойств объекта
// ---------------------------------------------------------------------------
//
// WMI отдаёт одно и то же свойство разными типами в зависимости от класса и
// сборки: строка как VT_BSTR, 64-битное число как VT_BSTR с цифрами (так
// отдаёт uint64 в CIM) и то же число как VT_I8, а флаг — как VT_BOOL.
// Поэтому читатели терпимы к типу, а нераспознанное даёт «свойства нет».
// Молча превращать нераспознанное в ноль нельзя: вызывающий должен видеть
// разницу между «ноль» и «не ответили».

// Прочитать свойство в VARIANT. Отсутствующее свойство — не ошибка запроса:
// Get вернёт WBEM_E_NOT_FOUND, и читатель решит, что делать с пустым полем.
bool getProperty(IWbemClassObject* object, const wchar_t* name, Variant& out) noexcept {
    if (object == nullptr) return false;
    Bstr propertyName(name);
    if (!propertyName) return false;
    return isSuccess(object->Get(propertyName.get(), 0, out.put(), nullptr, nullptr));
}

// Текст свойства в UTF-8. Числа приводятся к тексту, VT_BOOL — к «true»/«false»:
// строковые свойства классов хранилища (FirmwareVersion, SerialNumber) на
// разных сборках приходят то как BSTR, то как число.
//
// Случай VT_LPWSTR здесь не обрабатывается намеренно. В этом SDK указатели
// лежат во вложенном объединении tagVARIANT (n2), а IWbemClassObject::Get
// строки отдаёт как VT_BSTR — обращаться к pwszVal ради несуществующего случая
// значит привязаться к внутреннему устройству SDK.
bool getString(IWbemClassObject* object, const wchar_t* name, std::string& utf8) noexcept {
    Variant value;
    if (!getProperty(object, name, value)) return false;
    const VARIANT& variant = value.get();
    switch (variant.vt) {
        case VT_BSTR:
            if (variant.bstrVal == nullptr) return false;
            utf8 = toUtf8(std::wstring_view(variant.bstrVal, ::SysStringLen(variant.bstrVal)));
            return true;
        case VT_I4:
            utf8 = std::to_string(variant.lVal);
            return true;
        case VT_UI4:
            utf8 = std::to_string(static_cast<unsigned long>(variant.ulVal));
            return true;
        case VT_I8:
            utf8 = std::to_string(static_cast<long long>(variant.llVal));
            return true;
        case VT_UI8:
            utf8 = std::to_string(static_cast<unsigned long long>(variant.ullVal));
            return true;
        case VT_BOOL:
            utf8 = variant.boolVal == VARIANT_FALSE ? "false" : "true";
            return true;
        case VT_NULL:
        case VT_EMPTY:
            // VT_NULL, VT_EMPTY и любой нераспознанный тип дают по контракту
            // читателя один и тот же отказ — «свойства нет» (см. контракт
            // функции выше). Раньше это были три копии `return false;` подряд,
            // и проверка branch-clone указывала справедливо: правка одного
            // случая разошлась бы с остальными, а результат зависел бы от
            // того, какой VT именно отдал WMI в этот раз. Ветви объединены.
        default:
            return false;
    }
}

// Первое непустое из нескольких имён одного свойства. Нужно схемам, которые
// отличаются между сборками: неизвестное имя даёт пустое поле, а не отказ.
bool getFirstString(IWbemClassObject* object, std::initializer_list<const wchar_t*> names, std::string& utf8) noexcept {
    for (const wchar_t* name : names) {
        if (getString(object, name, utf8) && !utf8.empty()) return true;
    }
    return false;
}

// Беззнаковое целое свойство. Цифровая строка разбирается вручную, а не через
// wcstoull: у uint64 из WMI знака не бывает, а основание всегда десятичное.
bool getUint64Value(const VARIANT& variant, std::uint64_t& out) noexcept {
    switch (variant.vt) {
        case VT_I4: {
            if (variant.lVal < 0) return false;
            out = static_cast<std::uint64_t>(variant.lVal);
            return true;
        }
        case VT_UI4:
            out = static_cast<std::uint64_t>(variant.ulVal);
            return true;
        case VT_I8: {
            if (variant.llVal < 0) return false;
            out = static_cast<std::uint64_t>(variant.llVal);
            return true;
        }
        case VT_UI8:
            out = variant.ullVal;
            return true;
        case VT_INT: {
            if (variant.intVal < 0) return false;
            out = static_cast<std::uint64_t>(variant.intVal);
            return true;
        }
        case VT_UINT:
            out = static_cast<std::uint64_t>(variant.uintVal);
            return true;
        case VT_I2: {
            if (variant.iVal < 0) return false;
            out = static_cast<std::uint64_t>(variant.iVal);
            return true;
        }
        case VT_UI2:
            out = static_cast<std::uint64_t>(variant.uiVal);
            return true;
        case VT_BSTR: {
            if (variant.bstrVal == nullptr) return false;
            const std::wstring_view text(variant.bstrVal, ::SysStringLen(variant.bstrVal));
            if (text.empty()) return false;
            std::uint64_t value = 0;
            for (const wchar_t symbol : text) {
                if (symbol < L'0' || symbol > L'9') return false;
                value = value * 10u + static_cast<std::uint64_t>(symbol - L'0');
            }
            out = value;
            return true;
        }
        default:
            return false;
    }
}

bool getUint64(IWbemClassObject* object, const wchar_t* name, std::uint64_t& out) noexcept {
    Variant value;
    if (!getProperty(object, name, value)) return false;
    return getUint64Value(value.get(), out);
}

bool getUint32(IWbemClassObject* object, const wchar_t* name, std::uint32_t& out) noexcept {
    std::uint64_t value = 0;
    if (!getUint64(object, name, value)) return false;
    if (value > 0xFFFFFFFFull) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool getUint16(IWbemClassObject* object, const wchar_t* name, std::uint16_t& out) noexcept {
    std::uint32_t value = 0;
    if (!getUint32(object, name, value)) return false;
    if (value > 0xFFFFu) return false;
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool getBool(IWbemClassObject* object, const wchar_t* name, bool& out) noexcept {
    Variant value;
    if (!getProperty(object, name, value)) return false;
    const VARIANT& variant = value.get();
    // Сравнение с VARIANT_TRUE, а не приведение к bool: «любое ненулевое — истина»
    // здесь означало бы, что VT_I4 со значением 2 читается как истина, а у полей
    // вроде Severity значение 2 — это вполне обычное «2».
    if (variant.vt == VT_BOOL) {
        out = variant.boolVal != VARIANT_FALSE;
        return true;
    }
    if (variant.vt == VT_I4) {
        out = variant.lVal != 0;
        return true;
    }
    return false;
}

// Побитовое ИЛИ всех значений массива (OperationalStatus). Класс отдаёт массив
// статусов, и «Lost Communication» вместе с «OK» должны уместиться в одно
// поле, поэтому набор хранится маской, а проверяется hasOperationalStatus /
// operationalStatusDescription.
std::uint32_t getStatusMask(IWbemClassObject* object, const wchar_t* name) noexcept {
    Variant value;
    if (!getProperty(object, name, value)) return 0;
    const VARIANT& variant = value.get();
    std::uint32_t mask = 0;

    // Некоторые сборки отдают единственное значение вместо массива — тогда
    // маска собирается из одного числа.
    std::uint64_t single = 0;
    if (variant.vt != VT_ARRAY && getUint64Value(variant, single) && single <= 0xFFFFFFFFull) {
        return static_cast<std::uint32_t>(single);
    }
    if (variant.vt != VT_ARRAY || variant.parray == nullptr) return 0;

    // Тип элементов массива узнаётся вызовом SafeArrayGetVartype: в этом SDK
    // у tagSAFEARRAY нет поля с типом элемента, в отличие от Automation.
    VARTYPE elementType = VT_EMPTY;
    if (isFailure(::SafeArrayGetVartype(variant.parray, &elementType))) return 0;
    switch (elementType) {
        case VT_I2:
        case VT_UI2:
        case VT_I4:
        case VT_UI4:
        case VT_BSTR:
            break;
        default:
            return 0;
    }

    LONG lower = 0;
    LONG upper = -1;
    if (isFailure(::SafeArrayGetLBound(variant.parray, 1, &lower))) return 0;
    if (isFailure(::SafeArrayGetUBound(variant.parray, 1, &upper))) return 0;
    if (upper < lower) return 0;

    // Набор статусов длиннее 64 в схеме не встречается; потолок защищает от
    // провайдера, отдавшего пустой массив с завышенной верхней границей.
    constexpr LONG kMaxStatusItems = 64;
    const LONG available = upper - lower + 1;
    const LONG count = available < kMaxStatusItems ? available : kMaxStatusItems;
    for (LONG index = 0; index < count; ++index) {
        // SafeArrayGetElement требует, чтобы vt элемента был выставлен по типу
        // массива: иначе он не знает, что именно разворачивать. Индекс — тоже
        // обязательно неконстантный LONG*: константный указатель функция не
        // принимает.
        VARIANT element;
        ::VariantInit(&element);
        element.vt = elementType;
        element.parray = variant.parray;
        LONG position = lower + index;
        if (isSuccess(::SafeArrayGetElement(variant.parray, &position, &element))) {
            std::uint64_t number = 0;
            if (getUint64Value(element, number) && number <= 0xFFFFFFFFull) {
                mask |= static_cast<std::uint32_t>(number);
            }
        }
        // Для VT_BSTR SafeArrayGetElement выделяет строку — её снимает
        // VariantClear, иначе на каждом элементе утекает блок памяти.
        ::VariantClear(&element);
    }
    return mask;
}

// ---------------------------------------------------------------------------
// Обход одного запроса
// ---------------------------------------------------------------------------

// Итог обхода: сколько строк прочитано и чем обход закончился. rows != 0 при
// ошибке — значит, часть данных всё же пришла (WMI отдаёт пакет и сообщает об
// отказе одновременно), и решает вызывающий.
struct QueryOutcome {
    std::size_t rows{0};
    HRESULT hr{S_OK};
};

// Единый путь любого запроса: ExecQuery, пакетное Next с предельным сроком,
// reader на каждый объект, обязательный Release каждого объекта.
//
// С WBEM_FLAG_RETURN_IMMEDIATELY ExecQuery отдаёт перечислитель сразу, а данные
// приходят пакетами в Next, у которого предельный срок есть (lTimeout). Так
// инвентаризация не зависает на провайдере, который отвечает медленно
// (FR-1, §10).
template <typename Reader>
QueryOutcome forEachRow(IWbemServices* services, const wchar_t* wql, Reader&& reader) noexcept {
    QueryOutcome outcome;
    if (services == nullptr || wql == nullptr) {
        outcome.hr = E_POINTER;
        return outcome;
    }

    Bstr language(kWql);
    Bstr query(wql);
    if (!language || !query) {
        outcome.hr = E_OUTOFMEMORY;
        return outcome;
    }

    const LONG flags = WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY;
    IEnumWbemClassObject* raw = nullptr;
    // ExecQuery в этом SDK (WbemCli.h) параметра таймаута не имеет вообще:
    // возвращает он перечислитель, а ожидание данных происходит уже в Next,
    // где предельный срок задаётся (kQueryTimeoutMs). С WBEM_FLAG_RETURN_
    // IMMEDIATELY сам ExecQuery возвращает перечислитель сразу и большую
    // часть работы делает провайдер в фоне — то есть длинный запрос всё равно
    // не держит вызывающий поток. Случай, когда провайдер не отдаёт перечислитель
    // вовсе, ограничен таймаутом рабочего потока инвентаризации.
    const HRESULT hr = services->ExecQuery(language.get(), query.get(), flags, nullptr, &raw);
    if (isFailure(hr) || raw == nullptr) {
        outcome.hr = hr;
        return outcome;
    }
    EnumeratorGuard enumerator(raw);

    std::vector<IWbemClassObject*> batch(kRowBatch);
    for (;;) {
        ULONG returned = 0;
        const HRESULT nextHr =
            raw->Next(static_cast<LONG>(kQueryTimeoutMs), static_cast<ULONG>(batch.size()), batch.data(), &returned);
        if (returned == 0) {
            // WBEM_S_FALSE — конец перечисления; отказ приходит сюда же, но с
            // кодом, и код важнее молчаливого выхода.
            outcome.hr = nextHr;
            break;
        }
        enumerator.adopt(batch);
        // Читаем ПРИНЯТОЕ владение, а не перемещённый batch: после adopt() он пуст.
        const std::vector<IWbemClassObject*>& adopted = enumerator.objects();
        for (ULONG index = 0; index < returned && index < adopted.size(); ++index) {
            IWbemClassObject* const object = adopted[index];
            if (object == nullptr) continue;
            reader(object);
            ++outcome.rows;
        }
        batch.assign(kRowBatch, nullptr);
        if (outcome.rows >= kMaxRows) {
            core::LogFields fields;
            addField(fields, "wql", wql);
            addField(fields, "rows", static_cast<std::int64_t>(outcome.rows));
            core::logWarn("wmi.rowLimit", "запрос WMI обрезан по потолку строк", fields);
            outcome.hr = WBEM_S_FALSE;
            break;
        }
        if (isFailure(nextHr)) {
            outcome.hr = nextHr;
            break;
        }
    }
    return outcome;
}

// ---------------------------------------------------------------------------
// Разбор объектов в структуры результата
// ---------------------------------------------------------------------------

EncryptionState encryptionStateFromCode(std::uint32_t code) noexcept {
    switch (code) {
        case kEncryptStateEncrypted:
            return EncryptionState::Encrypted;
        case kEncryptStateNotEncrypted:
            return EncryptionState::NotEncrypted;
        case kEncryptStateEncrypting:
            return EncryptionState::Encrypting;
        case kEncryptStateDecrypting:
            return EncryptionState::Decrypting;
        default:
            // 5, 6 и любые будущие значения: состояние неизвестно, сырое число
            // остаётся в encryptStateCode. Превращать их в NotEncrypted было бы
            // прямой опасностью для пользователя, который смотрит на карточку.
            return EncryptionState::Unknown;
    }
}

ProtectionState protectionStateFromCode(std::uint32_t code) noexcept {
    switch (code) {
        case kProtectionStatusOn:
            return ProtectionState::Enabled;
        case kProtectionStatusOff:
            return ProtectionState::Disabled;
        default:
            return ProtectionState::Unknown;
    }
}

VolumeLockState lockStateFromCode(std::uint32_t code) noexcept {
    switch (code) {
        case kLockStatusUnlocked:
            return VolumeLockState::Unlocked;
        case kLockStatusLocked:
            return VolumeLockState::Locked;
        default:
            return VolumeLockState::Unknown;
    }
}

ConversionState conversionStateFromCode(std::uint32_t code) noexcept {
    switch (code) {
        case kConversionStatusInProgress:
            return ConversionState::InProgress;
        case kConversionStatusFullyEncrypted:
        case kConversionStatusFullyDecrypted:
            return ConversionState::Complete;
        default:
            return ConversionState::Unknown;
    }
}

VolumeEncryption readEncryptVolume(IWbemClassObject* object) noexcept {
    VolumeEncryption state;
    std::string driveLetter;
    if (getString(object, L"DriveLetter", driveLetter)) state.driveLetter = normalizeDriveLetter(driveLetter);
    (void)getUint32(object, L"EncryptState", state.encryptStateCode);
    (void)getUint32(object, L"ConversionStatus", state.conversionStatusCode);
    (void)getUint32(object, L"ProtectionStatus", state.protectionStatusCode);
    (void)getUint32(object, L"LockStatus", state.lockStatusCode);
    state.state = encryptionStateFromCode(state.encryptStateCode);
    state.protection = protectionStateFromCode(state.protectionStatusCode);
    state.lockState = lockStateFromCode(state.lockStatusCode);
    state.conversion = conversionStateFromCode(state.conversionStatusCode);
    (void)getString(object, L"EncryptionMethod", state.encryptionMethod);
    state.source = EncryptionSource::Wmi;
    state.hr = S_OK;
    return state;
}

PhysicalDiskInfo readPhysicalDisk(IWbemClassObject* object) noexcept {
    PhysicalDiskInfo disk;
    (void)getString(object, L"DeviceId", disk.deviceId);
    disk.number = physicalDriveNumber(disk.deviceId);
    (void)getFirstString(object, {L"FriendlyName", L"Model"}, disk.friendlyName);
    (void)getString(object, L"Model", disk.model);
    (void)getString(object, L"Manufacturer", disk.manufacturer);
    (void)getString(object, L"SerialNumber", disk.serialNumber);
    (void)getString(object, L"FirmwareVersion", disk.firmwareVersion);
    (void)getString(object, L"PartNumber", disk.partNumber);
    (void)getUint64(object, L"Size", disk.sizeBytes);
    (void)getUint64(object, L"AllocatedSize", disk.allocatedBytes);
    // BusType в MSFT_PhysicalDisk пронумерован так же, как STORAGE_BUS_TYPE из
    // FR-1 п.2 (сверка с msft_physicaldisk.mof: 7 = USB, 11 = SATA,
    // 17 = NVMe). Модуль отдаёт сырое число, а перевод в core::BusType и имя
    // для отчёта берёт вызывающий через platform::bus_type (busTypeFromRaw,
    // rawBusName) — вторая таблица того же смысла в этом файле была бы
    // дублированием, которое со временем разошлось бы с таблицей соседа.
    (void)getUint32(object, L"BusType", disk.busType);
    (void)getUint32(object, L"MediaType", disk.mediaType);
    (void)getUint16(object, L"HealthStatus", disk.healthStatus);
    disk.operationalStatus = getStatusMask(object, L"OperationalStatus");
    (void)getBool(object, L"IsOffline", disk.offline);
    (void)getBool(object, L"IsReadOnly", disk.readOnly);
    (void)getBool(object, L"IsBoot", disk.boot);
    (void)getBool(object, L"IsSystem", disk.system);
    return disk;
}

VolumeInfo readVolume(IWbemClassObject* object) noexcept {
    VolumeInfo volume;
    // MSFT_Volume.DriveLetter — это Char16, то есть один символ без
    // двоеточия. Приводим к общему виду «C:», в котором живут и
    // Win32_EncryptVolume, и core::Volume.
    Variant driveLetter;
    if (getProperty(object, L"DriveLetter", driveLetter) && driveLetter.get().vt == VT_BSTR &&
        driveLetter.get().bstrVal != nullptr && ::SysStringLen(driveLetter.get().bstrVal) > 0) {
        // Символ буквы — всегда ASCII, поэтому нормализатору отдаётся он же,
        // без перекодировки.
        const char letter = static_cast<char>(driveLetter.get().bstrVal[0]);
        volume.driveLetter = normalizeDriveLetter(std::string_view(&letter, 1));
    }
    (void)getString(object, L"Path", volume.path);
    // ObjectId и UniqueId — два поля с одним смыслом (идентификатор тома).
    // Берём первое, которое действительно похоже на GUID-путь, чтобы не
    // угадывать, какое из них заполнено в этой сборке.
    std::string identity;
    (void)getFirstString(object, {L"ObjectId", L"UniqueId"}, identity);
    if (identity.rfind("\\\\?\\Volume{", 0) == 0) {
        volume.volumeGuidPath = std::move(identity);
    }
    (void)getString(object, L"FileSystem", volume.fileSystem);
    (void)getString(object, L"FileSystemLabel", volume.fileSystemLabel);
    (void)getUint64(object, L"Size", volume.sizeBytes);
    (void)getUint64(object, L"SizeRemaining", volume.sizeRemainingBytes);
    (void)getUint32(object, L"DriveType", volume.driveType);
    (void)getUint32(object, L"FileSystemType", volume.fileSystemType);
    (void)getUint16(object, L"HealthStatus", volume.healthStatus);
    volume.operationalStatus = getStatusMask(object, L"OperationalStatus");
    return volume;
}

DiskDriveInfo readDiskDrive(IWbemClassObject* object) noexcept {
    DiskDriveInfo drive;
    (void)getString(object, L"DeviceID", drive.deviceId);
    drive.index = physicalDriveNumber(drive.deviceId);
    if (drive.index < 0) {
        std::uint32_t index = 0;
        if (getUint32(object, L"Index", index) && index <= 0x7FFFFFFFu) {
            drive.index = static_cast<int>(index);
        }
    }
    (void)getString(object, L"PNPDeviceID", drive.pnpDeviceId);
    (void)getString(object, L"Model", drive.model);
    (void)getString(object, L"SerialNumber", drive.serialNumber);
    (void)getFirstString(object, {L"FirmwareRevision", L"FirmwareVersion"}, drive.firmwareRevision);
    (void)getString(object, L"InterfaceType", drive.interfaceType);
    (void)getString(object, L"MediaType", drive.mediaType);
    (void)getString(object, L"Status", drive.status);
    (void)getUint64(object, L"Size", drive.sizeBytes);
    (void)getUint32(object, L"Partitions", drive.partitions);
    return drive;
}

StorageFaultInfo readStorageFault(IWbemClassObject* object) noexcept {
    StorageFaultInfo fault;
    // Схема MSFT_StorageFault отличается между сборками Windows, поэтому у
    // каждого поля короткий список известных имён, а пустое поле означает
    // «такого свойства здесь нет», а не «события нет».
    (void)getFirstString(object, {L"InstanceID", L"FaultId", L"ObjectId"}, fault.instanceId);
    (void)getFirstString(object, {L"DeviceId", L"DeviceID"}, fault.deviceId);
    (void)getFirstString(object, {L"FaultType", L"StorageFaultType"}, fault.faultType);
    (void)getFirstString(object, {L"Description", L"FaultDescription", L"Reason"}, fault.description);
    (void)getUint32(object, L"Severity", fault.severity);
    (void)getFirstString(object, {L"OccurrenceDate", L"CreationDate"}, fault.occurrenceDate);
    return fault;
}

// ---------------------------------------------------------------------------
// manage-bde -status (fallback FR-1 п.7)
// ---------------------------------------------------------------------------

// Сколько байт вывода manage-bde мы готовы принять. Реальный вывод на десяток
// томов — единицы килобайт; мегабайт с запасом хватает на любую разумную
// конфигурацию, а ограничение защищает от бесконечного чтения.
constexpr std::size_t kMaxManageBdeOutputBytes = 1024 * 1024;

// Снимок вывода для журнала: только печатный ASCII, всё прочее — «?».
// При перенаправлении дочерний процесс пишет в однобайтовую кодировку консоли,
// а журнал пишется в UTF-8 (SPEC §6.3), и без подмены туда попали бы
// невалидные байты.
std::string asciiForLog(std::string_view text) noexcept {
    std::string out;
    out.reserve(text.size() < 512 ? text.size() : std::size_t{512});
    for (const char symbol : text) {
        const unsigned char value = static_cast<unsigned char>(symbol);
        const bool printable = value >= 0x20u && value < 0x7Fu;
        out.push_back(printable ? symbol : '?');
        if (out.size() >= 512u) break;
    }
    return out;
}

bool isAsciiSpace(char symbol) noexcept {
    return symbol == ' ' || symbol == '\t' || symbol == '\r' || symbol == '\v' || symbol == '\f';
}

std::string_view trimAscii(std::string_view line) noexcept {
    std::size_t begin = 0;
    std::size_t end = line.size();
    while (begin < end && isAsciiSpace(line[begin])) ++begin;
    while (end > begin && isAsciiSpace(line[end - 1])) --end;
    return line.substr(begin, end - begin);
}

// Заголовок секции тома в выводе manage-bde — путь: «C:\», иногда с
// завершающим пробелом. Он не локализуется (это путь), в отличие от названий
// полей, поэтому по нему том и ищется.
bool isVolumeHeader(std::string_view line, std::string& driveLetterUtf8) noexcept {
    if (line.size() < 3 || line.size() > 4) return false;
    if (line[1] != ':') return false;
    const char letter = line[0];
    const bool latin = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
    if (!latin) return false;
    for (std::size_t index = 2; index < line.size(); ++index) {
        if (line[index] != '\\') return false;
    }
    driveLetterUtf8 = normalizeDriveLetter(std::string_view(&letter, 1));
    return !driveLetterUtf8.empty();
}

// Процент «100 %» в строке. Число с символом процента не локализуется, поэтому
// разбор переживает любой язык интерфейса manage-bde. В выводе
// `manage-bde -status` единственное число с процентом — доля шифрования тома,
// поэтому берётся первое найденное. false — процента в строке нет.
bool parsePercentage(std::string_view line, std::uint32_t& percent) noexcept {
    std::size_t index = 0;
    while (index < line.size()) {
        if (line[index] < '0' || line[index] > '9') {
            ++index;
            continue;
        }
        std::uint64_t value = 0;
        const std::size_t digitsStart = index;
        while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
            if (value <= 1000u) value = value * 10u + static_cast<std::uint64_t>(line[index] - '0');
            ++index;
        }
        const bool hasDigits = index > digitsStart;
        std::size_t probe = index;
        while (probe < line.size() && isAsciiSpace(line[probe])) ++probe;
        if (!hasDigits || probe >= line.size()) continue;
        // '%' — обычный знак процента; 0xA0 — узкая неразрывная пробел, которой
        // в некоторых локалях разделяют число и знак.
        const char tail = line[probe];
        if (tail == '%' || static_cast<unsigned char>(tail) == 0xA0u) {
            percent = static_cast<std::uint32_t>(value > 100u ? 100u : value);
            return true;
        }
    }
    return false;
}

// Запустить `manage-bde -status <буква>` и прочитать его вывод.
//
// Три решения, каждое из которых важно для утилиты, которая показывает
// пользователю «том зашифрован»:
//
//  1) полный путь к исполняемому файлу берётся из GetSystemDirectoryW: поиск по
//     текущему каталогу означал бы, что manage-bde.exe рядом с программой
//     подменяет системный (SPEC §5: утилита очистки не должна запускать то, что
//     ей подложили);
//  2) окно не создаётся (CREATE_NO_WINDOW), иначе при каждом обходе мигает
//     консоль;
//  3) чтение идёт перекрывающимся ReadFile с предельным ожиданием, а по его
//     истечении процесс снимается. Иначе «зависший» manage-bde держит
//     инвентаризацию, а она идёт в потоке UI-цикла.
struct ManageBdeRun {
    bool started{false};
    bool timedOut{false};
    HRESULT hr{E_FAIL};
    DWORD exitCode{0};
    std::string output;  // как есть, в кодировке консоли дочернего процесса
};

ManageBdeRun runManageBde(std::wstring_view driveLetter) noexcept {
    ManageBdeRun run;

    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT systemLength = ::GetSystemDirectoryW(systemDirectory, static_cast<UINT>(MAX_PATH));
    if (systemLength == 0 || systemLength >= static_cast<UINT>(MAX_PATH)) {
        run.hr = lastErrorHresult();
        return run;
    }
    // Аргумент — буква с двоеточием («C:»). Завершающий разделитель не нужен и
    // опасен: он стоял бы прямо перед закрывающей кавычкой строки команды.
    const std::wstring commandLine =
        std::wstring(systemDirectory) + L"\\manage-bde.exe -status " + std::wstring(driveLetter);

    SECURITY_ATTRIBUTES inheritable = {};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    // Не перекрывающийся дескрипт NUL: у перенаправленного вывода все три
    // стандартных потока должны быть валидны и наследуемы, иначе
    // CreateProcessW с STARTF_USESTDHANDLES откажется.
    const unique_handle<KernelHandlePolicy> nul(::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                                              FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                                              OPEN_EXISTING, 0, nullptr));
    if (!nul) {
        run.hr = lastErrorHresult();
        return run;
    }

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (::CreatePipe(&readEnd, &writeEnd, &inheritable, 0) == FALSE) {
        run.hr = lastErrorHresult();
        return run;
    }
    // Родителю конец записи не нужен, а ребёнку конец чтения передавать нельзя:
    // иначе EOF в трубе не наступит и чтение будет ждать вечно.
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    const unique_handle<KernelHandlePolicy> parentRead(readEnd);
    unique_handle<KernelHandlePolicy> childWrite(writeEnd);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nul.get();
    startup.hStdOutput = childWrite.get();
    startup.hStdError = childWrite.get();

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION process = {};
    if (::CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                         &startup, &process) == FALSE) {
        run.hr = lastErrorHresult();
        return run;
    }
    const unique_handle<KernelHandlePolicy> processHandle(process.hProcess);
    const unique_handle<KernelHandlePolicy> threadHandle(process.hThread);

    // Родитель закрывает свой конец записи, иначе чтение не увидит EOF.
    childWrite.reset();

    OVERLAPPED overlapped = {};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const unique_handle<KernelHandlePolicy> readEvent(overlapped.hEvent);
    if (!readEvent) {
        run.hr = lastErrorHresult();
        ::TerminateProcess(processHandle.get(), 1);
        return run;
    }

    const ULONGLONG deadline = ::GetTickCount64() + kManageBdeTimeoutMs;
    char buffer[4096];
    run.started = true;
    for (;;) {
        // Событие переиспользуется вместе с OVERLAPPED, а система оставляет его
        // установленным после завершения операции. Без сброса WaitForSingleObject
        // на втором чтении вернулся бы мгновенно, и GetOverlappedResult сообщил
        // бы ERROR_IO_INCOMPLETE — то есть обрыв читался бы как отказ.
        ::ResetEvent(overlapped.hEvent);
        DWORD transferred = 0;
        const BOOL ok =
            ::ReadFile(parentRead.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &transferred, &overlapped);
        if (ok == FALSE && ::GetLastError() != ERROR_IO_PENDING) {
            run.hr = lastErrorHresult();
            break;
        }
        if (ok == FALSE) {
            const ULONGLONG now = ::GetTickCount64();
            const DWORD left = now >= deadline ? 0u : static_cast<DWORD>(deadline - now);
            if (left == 0u) {
                run.timedOut = true;
                run.hr = hresultFromWin32(ERROR_TIMEOUT);
                break;
            }
            if (::WaitForSingleObject(overlapped.hEvent, left) == WAIT_TIMEOUT) {
                run.timedOut = true;
                run.hr = hresultFromWin32(ERROR_TIMEOUT);
                break;
            }
        }
        if (::GetOverlappedResult(parentRead.get(), &overlapped, &transferred, FALSE) == FALSE) {
            run.hr = lastErrorHresult();
            break;
        }
        if (transferred == 0) break;  // EOF
        if (run.output.size() < kMaxManageBdeOutputBytes) {
            run.output.append(buffer, static_cast<std::size_t>(transferred));
        }
        if (::GetTickCount64() >= deadline) {
            run.timedOut = true;
            run.hr = hresultFromWin32(ERROR_TIMEOUT);
            break;
        }
    }

    if (run.timedOut) {
        ::TerminateProcess(processHandle.get(), 1);
        ::WaitForSingleObject(processHandle.get(), 2000);
        return run;
    }
    if (::WaitForSingleObject(processHandle.get(), static_cast<DWORD>(kManageBdeTimeoutMs)) != WAIT_OBJECT_0) {
        // Процесс не завершился, хотя труба уже закрыта: снимаем его, иначе он
        // останется висеть после возврата.
        ::TerminateProcess(processHandle.get(), 1);
        ::WaitForSingleObject(processHandle.get(), 2000);
        run.timedOut = true;
        run.hr = hresultFromWin32(ERROR_TIMEOUT);
        return run;
    }
    DWORD exitCode = 0;
    if (::GetExitCodeProcess(processHandle.get(), &exitCode) != FALSE) run.exitCode = exitCode;
    run.hr = S_OK;
    return run;
}

// Разбор вывода manage-bde для одного тома: секция начинается со строки с
// путём («C:\»), состояние определяется по проценту шифрования.
VolumeEncryption parseManageBde(std::string_view output, const std::string& wantedDriveLetter) noexcept {
    VolumeEncryption state;
    state.driveLetter = wantedDriveLetter;
    state.source = EncryptionSource::ManageBde;

    bool inWantedSection = false;
    bool seenHeader = false;
    bool seenPercentage = false;
    std::uint32_t percent = 0;

    std::size_t position = 0;
    for (;;) {
        const std::size_t lineEnd = output.find('\n', position);
        const std::size_t end = lineEnd == std::string_view::npos ? output.size() : lineEnd;
        const std::string_view line = trimAscii(output.substr(position, end - position));

        std::string header;
        if (isVolumeHeader(line, header)) {
            inWantedSection = header == wantedDriveLetter;
            if (inWantedSection) seenHeader = true;
            // Новая секция начинается с чистого состояния: процент из чужого
            // тома не должен пережить переход.
            seenPercentage = false;
            percent = 0;
        } else if (inWantedSection && !seenPercentage) {
            std::uint32_t value = 0;
            if (parsePercentage(line, value)) {
                percent = value;
                seenPercentage = true;
            }
        }

        if (lineEnd == std::string_view::npos) break;
        position = lineEnd + 1;
    }

    if (!seenHeader) {
        state.hr = S_OK;
        state.detail = "manage-bde: тома " + wantedDriveLetter + " нет в выводе";
        return state;
    }
    if (!seenPercentage) {
        state.hr = S_OK;
        state.detail = "manage-bde: в выводе тома " + wantedDriveLetter + " нет процента шифрования";
        return state;
    }
    if (percent >= 100u) {
        state.state = EncryptionState::Encrypted;
    } else if (percent > 0u) {
        state.state = EncryptionState::Encrypting;
    } else {
        state.state = EncryptionState::NotEncrypted;
    }
    state.hr = S_OK;
    return state;
}

}  // namespace

// ---------------------------------------------------------------------------
// COM
// ---------------------------------------------------------------------------

const char* comInitResultName(ComInitResult result) noexcept {
    switch (result) {
        case ComInitResult::Ready:
            return "ready";
        case ComInitResult::AlreadyReady:
            return "already-ready";
        case ComInitResult::AlreadyThread:
            return "already-threaded";
        case ComInitResult::Failed:
            return "failed";
    }
    return "unknown";
}

ComApartment::ComApartment() noexcept {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    hr_ = hr;
    switch (hr) {
        case S_OK:
            // Квартира наша: именно мы увеличили счётчик, и именно мы обязаны
            // вызвать CoUninitialize.
            result_ = ComInitResult::Ready;
            ownsApartment_ = true;
            break;
        case S_FALSE:
            // Поток уже в MTA. Счётчик при S_FALSE не растёт, и CoUninitialize
            // здесь был бы чужим: снял бы инициализацию, которую поднял кто-то
            // ещё.
            result_ = ComInitResult::AlreadyReady;
            ownsApartment_ = false;
            break;
        case RPC_E_CHANGED_MODE:
            // Поток уже в STA — так бывает в UI-потоке, где COM могли поднять
            // Direct2D, Shell или Restart Manager. WMI от STA работает, поэтому
            // это не отказ, но и не наша квартира.
            result_ = ComInitResult::AlreadyThread;
            ownsApartment_ = false;
            break;
        default:
            result_ = ComInitResult::Failed;
            ownsApartment_ = false;
            break;
    }
}

ComApartment::~ComApartment() noexcept {
    if (ownsApartment_) ::CoUninitialize();
}

bool ComApartment::ready() const noexcept {
    return result_ != ComInitResult::Failed;
}

ComInitResult ComApartment::result() const noexcept {
    return result_;
}

HRESULT ComApartment::hr() const noexcept {
    return hr_;
}

HRESULT ensureComSecurity() noexcept {
    // CoInitializeSecurity общепроцессный и должен вызываться до любых других
    // COM-вызовов: без него ConnectServer к локальному пространству отказывает
    // RPC_E_INVALID_AUTHN. Отсюда и once_flag.
    static std::once_flag once;
    static HRESULT result = E_FAIL;
    std::call_once(once, []() {
        const HRESULT hr = ::CoInitializeSecurity(
            nullptr,  // аутентификация: по умолчанию процесса
            -1,       // число служб авторизации берём у COM
            nullptr,  // список авторизации
            nullptr,  // имя сервера DCOM
            RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE,
            nullptr);  // домен и имя пользователя не задаём
        // RPC_E_TOO_LATE означает «безопасность уже настроена»: с точки зрения
        // нашей задачи это успех, а не отказ.
        result = (isFailure(hr) && hr != RPC_E_TOO_LATE) ? hr : S_OK;
    });
    return result;
}

// ---------------------------------------------------------------------------
// Диагностика
// ---------------------------------------------------------------------------

std::string hresultName(HRESULT hr) {
    return hresultNameImpl(hr);
}

bool isClassMissing(HRESULT hr) noexcept {
    return hr == WBEM_E_INVALID_CLASS || hr == WBEM_E_INVALID_NAMESPACE || hr == WBEM_E_NOT_FOUND;
}

std::string QueryError::toString() const {
    std::string out = hresultNameImpl(hr);
    DWORD win32Code = ERROR_SUCCESS;
    if (tryWin32Code(hr, win32Code)) {
        out += " (Win32=";
        out += std::to_string(static_cast<unsigned long>(win32Code));
        out += ")";
    }
    out += ": ";
    out += hresultErrorText(hr);
    if (!operation.empty()) {
        out += " [";
        out += operation;
        out += "]";
    }
    if (!wql.empty()) {
        out += " wql=";
        out += wql;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Имена значений
// ---------------------------------------------------------------------------

const char* encryptionStateName(EncryptionState state) noexcept {
    switch (state) {
        case EncryptionState::Unknown:
            return "unknown";
        case EncryptionState::NotEncrypted:
            return "not-encrypted";
        case EncryptionState::Encrypted:
            return "encrypted";
        case EncryptionState::Encrypting:
            return "encrypting";
        case EncryptionState::Decrypting:
            return "decrypting";
    }
    return "unknown";
}

bool isEncrypted(EncryptionState state) noexcept {
    return state == EncryptionState::Encrypted;
}

const char* encryptionSourceName(EncryptionSource source) noexcept {
    switch (source) {
        case EncryptionSource::Unknown:
            return "unknown";
        case EncryptionSource::Wmi:
            return "wmi";
        case EncryptionSource::ManageBde:
            return "manage-bde";
    }
    return "unknown";
}

const char* mediaTypeName(std::uint32_t mediaType) noexcept {
    switch (mediaType) {
        case kMediaTypeUnspecified:
            return "Unspecified";
        case kMediaTypeHdd:
            return "HDD";
        case kMediaTypeSsd:
            return "SSD";
        default:
            return nullptr;
    }
}

const char* healthStatusName(std::uint16_t healthStatus) noexcept {
    switch (healthStatus) {
        case kHealthStatusHealthy:
            return "Healthy";
        case kHealthStatusWarning:
            return "Warning";
        case kHealthStatusUnhealthy:
            return "Unhealthy";
        default:
            return nullptr;
    }
}

bool hasOperationalStatus(std::uint32_t mask, std::uint32_t value) noexcept {
    return (mask & value) != 0;
}

const char* operationalStatusDescription(std::uint32_t mask) noexcept {
    // Порядок — по тяжести: показывать надо самое важное из набора, а не то,
    // что попалось первым. Статусы, которых в маске нет, пропускаются, поэтому
    // «OK» в конце означает «иных известных статусов нет».
    if (hasOperationalStatus(mask, kOperationalStatusNonRecoverableError)) return "Non-Recoverable Error";
    if (hasOperationalStatus(mask, kOperationalStatusError)) return "Error";
    if (hasOperationalStatus(mask, kOperationalStatusPredictiveFailure)) return "Predictive Failure";
    if (hasOperationalStatus(mask, kOperationalStatusFullRepairNeeded)) return "Full Repair Needed";
    if (hasOperationalStatus(mask, kOperationalStatusSpotFixNeeded)) return "Spot Fix Needed";
    if (hasOperationalStatus(mask, kOperationalStatusDegraded)) return "Degraded";
    if (hasOperationalStatus(mask, kOperationalStatusScanNeeded)) return "Scan Needed";
    if (hasOperationalStatus(mask, kOperationalStatusOk)) return "OK";
    return nullptr;
}

const char* driveTypeName(std::uint32_t driveType) noexcept {
    switch (driveType) {
        case kDriveTypeUnknown:
            return "Unknown";
        case kDriveTypeInvalidRoot:
            return "InvalidRootPath";
        case kDriveTypeRemovable:
            return "Removable";
        case kDriveTypeFixed:
            return "Fixed";
        case kDriveTypeRemote:
            return "Remote";
        case kDriveTypeCdRom:
            return "CD-ROM";
        case kDriveTypeRamDisk:
            return "RamDisk";
        default:
            return nullptr;
    }
}

const char* fileSystemTypeName(std::uint32_t fileSystemType) noexcept {
    switch (fileSystemType) {
        case kFileSystemTypeUnknown:
            return "Unknown";
        case kFileSystemTypeFat:
            return "FAT";
        case kFileSystemTypeFat16:
            return "FAT16";
        case kFileSystemTypeFat32:
            return "FAT32";
        case kFileSystemTypeNtfs:
            return "NTFS";
        case kFileSystemTypeRefs:
            return "ReFS";
        case kFileSystemTypeCsvfsNtfs:
            return "CSVFS_NTFS";
        case kFileSystemTypeCsvfsRefs:
            return "CSVFS_ReFS";
        default:
            return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Помощники
// ---------------------------------------------------------------------------

std::string normalizeDriveLetter(std::string_view utf8) noexcept {
    // Модуль работает с UTF-8 (SPEC §6.3), а Win32_EncryptVolume отдаёт букву в
    // UTF-16. Диапазон букв один, поэтому сверяемся с латиницей прямо в UTF-8 и
    // не перекодируем впустую.
    std::size_t begin = 0;
    std::size_t end = utf8.size();
    while (begin < end && isAsciiSpace(utf8[begin])) ++begin;
    while (end > begin && (isAsciiSpace(utf8[end - 1]) || utf8[end - 1] == '/')) --end;
    if (begin >= end) return {};

    char letter = utf8[begin];
    const bool latin = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
    if (!latin) return {};
    // «C», «C:», «C:\», «C:\Folder» — всё это буква C. А вот «Software» или
    // «\\server\share» — нет: без двоеточия на втором месте это не путь тома.
    const bool hasColon = end - begin >= 2 && utf8[begin + 1] == ':';
    if (end - begin >= 2 && !hasColon) return {};
    if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');

    std::string out;
    out.push_back(letter);
    out.push_back(':');
    return out;
}

namespace {

// Сравнение без учёта регистра по ASCII: путь устройства приходит от WMI как
// «\\.\PHYSICALDRIVE0», но в других источниках встречается и другой регистр.
bool asciiEqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

int physicalDriveNumber(std::string_view deviceIdUtf8) noexcept {
    constexpr std::string_view kPrefix = "\\\\.\\PHYSICALDRIVE";
    if (deviceIdUtf8.size() <= kPrefix.size()) return -1;
    if (!asciiEqualsIgnoreCase(deviceIdUtf8.substr(0, kPrefix.size()), kPrefix)) return -1;
    std::size_t index = kPrefix.size();
    long long number = 0;
    for (; index < deviceIdUtf8.size(); ++index) {
        const char symbol = deviceIdUtf8[index];
        if (symbol == '\\') break;  // «\\.\PHYSICALDRIVE0\» — с завершающим разделителем
        if (symbol < '0' || symbol > '9') return -1;
        number = number * 10 + static_cast<long long>(symbol - '0');
        if (number > 100000) return -1;
    }
    if (index == kPrefix.size()) return -1;
    if (index != deviceIdUtf8.size() && deviceIdUtf8[index] != '\\') return -1;
    return static_cast<int>(number);
}

const VolumeEncryption* findEncryption(const std::vector<VolumeEncryption>& states,
                                       std::string_view driveLetterUtf8) noexcept {
    const std::string wanted = normalizeDriveLetter(driveLetterUtf8);
    if (wanted.empty()) return nullptr;
    for (const VolumeEncryption& state : states) {
        if (state.driveLetter == wanted) return &state;
    }
    return nullptr;
}

std::string comparePhysicalDiskSize(const PhysicalDiskInfo& wmi, std::uint64_t ioctlSizeBytes) noexcept {
    if (wmi.sizeBytes == 0) return {};
    if (ioctlSizeBytes == 0) return "IOCTL не вернул размер диска, WMI: " + std::to_string(wmi.sizeBytes);
    const std::uint64_t larger = wmi.sizeBytes > ioctlSizeBytes ? wmi.sizeBytes : ioctlSizeBytes;
    const std::uint64_t smaller = wmi.sizeBytes > ioctlSizeBytes ? ioctlSizeBytes : wmi.sizeBytes;
    const std::uint64_t difference = larger - smaller;
    // Порог: 1 МиБ или 0,5 % от большего значения. Меньшие расхождения — это
    // округление и разный момент выборки (WMI и IOCTL читают диск в разное
    // время). Большие — повод для баг-репорта: на карточке диска пользователь
    // увидит одну цифру, а отчёт должен уметь объяснить расхождение.
    constexpr std::uint64_t kAbsoluteTolerance = 1024ull * 1024ull;
    if (difference <= kAbsoluteTolerance) return {};
    if (difference * 200ull <= larger) return {};  // 0,5 % от большего
    return "размер диска по WMI " + std::to_string(wmi.sizeBytes) + " байт против " + std::to_string(ioctlSizeBytes) +
           " байт по IOCTL";
}

// ---------------------------------------------------------------------------
// Сессия
// ---------------------------------------------------------------------------

struct Session::Impl {
    ComApartment com;
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> cimv2;
    ComPtr<IWbemServices> storage;
    QueryError error;
    // Класса в этом пространстве имён нет. Отдельный флаг, а не разбор error:
    // «класса нет» — это факт окружения, из которого вытекает отрицательный
    // кэш, а не отказ, который надо показывать в интерфейсе.
    bool classMissing{false};

    // Подключиться к пространству имён. Отдельный шаг, потому что подключение
    // к хранилищу нужно только MSFT-запросам, и его провал не должен ломать
    // запрос BitLocker.
    bool connectNamespace(ComPtr<IWbemServices>& services, const wchar_t* nameSpace) noexcept {
        if (services) return true;
        if (!com.ready()) {
            fail(com.hr(), "CoInitializeEx");
            return false;
        }
        const HRESULT securityHr = ensureComSecurity();
        if (isFailure(securityHr)) {
            fail(securityHr, "CoInitializeSecurity");
            return false;
        }
        if (!locator) {
            IWbemLocator* raw = nullptr;
            const HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                                  reinterpret_cast<void**>(&raw));
            if (isFailure(hr) || raw == nullptr) {
                fail(hr, "CoCreateInstance");
                return false;
            }
            locator.reset(raw);
        }
        Bstr nameSpaceBstr(nameSpace);
        if (!nameSpaceBstr) {
            fail(E_OUTOFMEMORY, "SysAllocString");
            return false;
        }
        // Параметры ConnectServer: локальное пространство, текущий пользователь,
        // без пароля, локаль по умолчанию, флаги безопасности нулевые — удалённый
        // хост не используется (границы в wmi.hpp).
        IWbemServices* raw = nullptr;
        const HRESULT hr =
            locator->ConnectServer(nameSpaceBstr.get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &raw);
        if (isFailure(hr) || raw == nullptr) {
            fail(hr, "ConnectServer");
            return false;
        }
        services.reset(raw);
        // CoSetProxyBlanket на локальном пространстве не обязателен, но полезен:
        // он выравнивает уровень аутентификации и имперсонации с тем, что
        // ожидает провайдер хранилища. Отказ здесь не фатален — доступ к
        // локальному пространству уже есть, — поэтому он попадает только в лог.
        // Уровень — RPC_C_AUTHN_LEVEL_CALL, как в эталонных примерах WMI:
        // RPC_C_AUTHN_LEVEL_CALL_CREDENTIALS в Windows 10 SDK не объявлен, а
        // RPC_C_AUTHN_LEVEL_PKT_PRIVACY для локального провайдера избыточен.
        IUnknown* const proxy = static_cast<IUnknown*>(raw);
        const HRESULT blanketHr = ::CoSetProxyBlanket(proxy, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                                                      EOAC_NONE);
        if (isFailure(blanketHr)) {
            core::LogFields fields;
            addField(fields, "ns", nameSpace);
            addField(fields, "hr", static_cast<std::int64_t>(blanketHr));
            core::logWarn("wmi.proxyBlanket", "CoSetProxyBlanket не удался, продолжаем без него", fields);
        }
        return true;
    }

    void fail(HRESULT hr, const char* operation, const wchar_t* wql = nullptr) noexcept {
        error.hr = hr;
        error.operation = operation == nullptr ? "" : operation;
        error.wql = wql == nullptr ? std::string() : toUtf8(std::wstring_view(wql));
        const std::string line = error.toString();
        core::LogFields fields;
        addField(fields, "hr", static_cast<std::int64_t>(hr));
        core::logWarn("wmi.failed", line.c_str(), fields);
    }

    // Общий обход: подключение, ExecQuery, пакетное Next, reader на каждый объект.
    // Пустой вектор у вызывающего означает «данных нет», а не «устройств нет».
    template <typename Reader>
    void query(ComPtr<IWbemServices>& services, const wchar_t* nameSpace, const wchar_t* wql,
               Reader&& reader) noexcept {
        classMissing = false;
        error = QueryError{};
        if (!connectNamespace(services, nameSpace)) {
            classMissing = isClassMissing(error.hr);
            return;
        }
        const QueryOutcome outcome = forEachRow(services.get(), wql, std::forward<Reader>(reader));
        if (isEndOfEnumeration(outcome.hr) || isSuccess(outcome.hr)) return;
        if (outcome.hr == WBEM_S_TIMEDOUT) {
            // Успешный HRESULT, но по смыслу — отказ: ждали дольше
            // kQueryTimeoutMs. Молча проглотить его нельзя, иначе в отчёте
            // будет «нет данных» без причины.
            error.hr = outcome.hr;
            error.operation = "Next";
            error.wql = toUtf8(std::wstring_view(wql));
            const std::string line = error.toString();
            core::LogFields fields;
            addField(fields, "timeoutMs", static_cast<std::int64_t>(kQueryTimeoutMs));
            core::logWarn("wmi.timeout", line.c_str(), fields);
            return;
        }
        if (isClassMissing(outcome.hr)) {
            // Отдельная запись без «ошибки»: класс отсутствует — это свойство
            // системы, а не сбой запроса.
            classMissing = true;
            error.hr = outcome.hr;
            error.operation = "ExecQuery";
            error.wql = toUtf8(std::wstring_view(wql));
            return;
        }
        fail(outcome.hr, "ExecQuery", wql);
    }
};

Session::Session()
    : impl_(std::make_unique<Impl>()) {
}

Session::~Session() = default;

Session::Session(Session&& other) noexcept = default;

Session& Session::operator=(Session&& other) noexcept = default;

bool Session::connect() noexcept {
    if (impl_ == nullptr) return false;
    return impl_->connectNamespace(impl_->cimv2, kCimV2Namespace);
}

bool Session::connected() const noexcept {
    return impl_ != nullptr && static_cast<bool>(impl_->cimv2);
}

const QueryError& Session::lastError() const noexcept {
    static const QueryError kEmpty{};
    return impl_ == nullptr ? kEmpty : impl_->error;
}

std::vector<VolumeEncryption> Session::volumeEncryption() noexcept {
    std::vector<VolumeEncryption> states;
    if (impl_ == nullptr) return states;
    impl_->query(impl_->cimv2, kCimV2Namespace, kQueryEncryptVolume, [&states](IWbemClassObject* object) {
        states.push_back(readEncryptVolume(object));
    });
    return states;
}

std::vector<DiskDriveInfo> Session::diskDrives() noexcept {
    std::vector<DiskDriveInfo> drives;
    if (impl_ == nullptr) return drives;
    impl_->query(impl_->cimv2, kCimV2Namespace, kQueryDiskDrives, [&drives](IWbemClassObject* object) {
        drives.push_back(readDiskDrive(object));
    });
    return drives;
}

std::vector<PhysicalDiskInfo> Session::physicalDisks() noexcept {
    std::vector<PhysicalDiskInfo> disks;
    if (impl_ == nullptr) return disks;
    if (g_physicalDiskClassUnavailable.load(std::memory_order_relaxed)) return disks;
    impl_->query(impl_->storage, kStorageNamespace, kQueryPhysicalDisks, [&disks](IWbemClassObject* object) {
        disks.push_back(readPhysicalDisk(object));
    });
    if (impl_->classMissing) g_physicalDiskClassUnavailable.store(true, std::memory_order_relaxed);
    return disks;
}

std::vector<VolumeInfo> Session::volumes() noexcept {
    std::vector<VolumeInfo> volumes;
    if (impl_ == nullptr) return volumes;
    if (g_volumeClassUnavailable.load(std::memory_order_relaxed)) return volumes;
    impl_->query(impl_->storage, kStorageNamespace, kQueryVolumes, [&volumes](IWbemClassObject* object) {
        volumes.push_back(readVolume(object));
    });
    if (impl_->classMissing) g_volumeClassUnavailable.store(true, std::memory_order_relaxed);
    return volumes;
}

std::vector<StorageFaultInfo> Session::storageFaults() noexcept {
    std::vector<StorageFaultInfo> faults;
    if (impl_ == nullptr) return faults;
    if (g_faultClassUnavailable.load(std::memory_order_relaxed)) return faults;
    impl_->query(impl_->storage, kStorageNamespace, kQueryStorageFaults, [&faults](IWbemClassObject* object) {
        faults.push_back(readStorageFault(object));
    });
    if (impl_->classMissing) g_faultClassUnavailable.store(true, std::memory_order_relaxed);
    return faults;
}

// ---------------------------------------------------------------------------
// Разовые запросы
// ---------------------------------------------------------------------------

std::vector<VolumeEncryption> queryVolumeEncryption() noexcept {
    Session session;
    return session.volumeEncryption();
}

std::vector<PhysicalDiskInfo> queryPhysicalDisks() noexcept {
    Session session;
    return session.physicalDisks();
}

std::vector<VolumeInfo> queryVolumes() noexcept {
    Session session;
    return session.volumes();
}

std::vector<StorageFaultInfo> queryStorageFaults() noexcept {
    Session session;
    return session.storageFaults();
}

std::vector<DiskDriveInfo> queryDiskDrives() noexcept {
    Session session;
    return session.diskDrives();
}

VolumeEncryption queryVolumeEncryptionFromWmi(std::string_view driveLetterUtf8) noexcept {
    VolumeEncryption result;
    const std::string wanted = normalizeDriveLetter(driveLetterUtf8);
    if (wanted.empty()) {
        result.hr = E_INVALIDARG;
        result.detail = "пустая буква диска";
        return result;
    }
    Session session;
    if (!session.connect()) {
        result.hr = session.lastError().hr;
        result.detail = "WMI: " + session.lastError().toString();
        return result;
    }
    const std::vector<VolumeEncryption> states = session.volumeEncryption();
    const VolumeEncryption* found = findEncryption(states, wanted);
    if (found != nullptr) return *found;
    // Тома в выборке нет — это не «не зашифрован», а «не ответили».
    result.driveLetter = wanted;
    result.hr = session.lastError().hr;
    result.detail = states.empty() ? "WMI: Win32_EncryptVolume вернул пустую выборку"
                                   : "WMI: тома " + wanted + " нет в Win32_EncryptVolume";
    return result;
}

VolumeEncryption queryVolumeEncryptionFromManageBde(std::string_view driveLetterUtf8) noexcept {
    VolumeEncryption result;
    const std::string wanted = normalizeDriveLetter(driveLetterUtf8);
    if (wanted.empty()) {
        result.hr = E_INVALIDARG;
        result.detail = "пустая буква диска";
        return result;
    }
    const std::wstring argument = toUtf16(wanted);
    const ManageBdeRun run = runManageBde(std::wstring_view(argument));
    result.source = EncryptionSource::ManageBde;
    result.driveLetter = wanted;
    result.hr = run.hr;
    result.exitCode = run.exitCode;
    if (!run.started) {
        result.detail = "manage-bde: не запустился — " + hresultErrorText(run.hr);
        return result;
    }
    if (run.timedOut) {
        result.detail =
            "manage-bde: не ответил за " + std::to_string(static_cast<unsigned long>(kManageBdeTimeoutMs)) + " мс";
        return result;
    }
    if (run.exitCode != 0) {
        // Ненулевой код у manage-bde -status означает «том не найден» или «нет
        // прав», а не «том не зашифрован».
        result.detail = "manage-bde: код возврата " + std::to_string(static_cast<unsigned long>(run.exitCode));
        return result;
    }
    VolumeEncryption parsed = parseManageBde(std::string_view(run.output), wanted);
    parsed.exitCode = run.exitCode;
    // Снимок вывода в журнал обязателен именно при неудачном разборе: «процент не
    // найден» — самая частая причина Unknown, и по выводу видно, что именно
    // прислала система.
    if (!parsed.known()) {
        core::LogFields fields;
        addField(fields, "drive", wanted);
        addField(fields, "output", asciiForLog(run.output));
        core::logWarn("wmi.manageBde.parse", parsed.detail.c_str(), fields);
    }
    return parsed;
}

VolumeEncryption queryVolumeEncryption(std::string_view driveLetterUtf8) noexcept {
    // Порядок источников — из FR-1 п.7: сначала WMI, manage-bde только если WMI
    // по этому тому ничего не сказал.
    const VolumeEncryption fromWmi = queryVolumeEncryptionFromWmi(driveLetterUtf8);
    if (fromWmi.known()) return fromWmi;
    const VolumeEncryption fallback = queryVolumeEncryptionFromManageBde(driveLetterUtf8);
    if (fallback.known()) return fallback;
    // Оба источника молчат: результат остаётся честно неизвестным, но с обеими
    // причинами, иначе потеряется половина диагностики.
    VolumeEncryption result = fallback;
    result.driveLetter = fromWmi.driveLetter;
    result.hr = fromWmi.hr;
    result.detail = "WMI: " + fromWmi.detail + "; " + fallback.detail;
    core::LogFields fields;
    addField(fields, "drive", fromWmi.driveLetter);
    core::logWarn("wmi.encryption.unknown", result.detail.c_str(), fields);
    return result;
}

}  // namespace mrproper::platform::wmi
