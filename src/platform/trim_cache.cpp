// Свойства хранилища для карточки диска: TRIM, кэш записи, предел очереди IO.
// Спека: §4 FR-1 п.2 (IOCTL_STORAGE_QUERY_PROPERTY со свойствами
// StorageDeviceTrimProperty, StorageDeviceWriteCacheProperty,
// StorageDeviceIoCapabilityProperty), §13.1 («в MVP только TRIM/кэш записи в
// карточке диска»), требование к результату FR-1 («устойчивость к "отказавшим"
// дискам: таймаут 2 с на устройство, устройство помечается недоступным,
// приложение не падает»), §5 (без рантайм-повышения), §12 (отказ виден в логе
// с путём и кодом).
//
// Форма запроса — из комментария самого SDK над STORAGE_DESCRIPTOR_HEADER:
// «all property descriptors can be cast into a STORAGE_DESCRIPTOR_HEADER, the
// IOCTL can be called once with a small buffer then again using a buffer as
// large as the header reports is necessary». Отсюда два прохода на свойство:
// маленький буфер спрашивает размер, большой приносит данные. Один проход «с
// запасом» на 4 КБ работал бы, но платил бы лишней записью в буфер на каждый
// диск при каждом обходе.
//
// Устойчивость к «отказавшим» дискам. Дескриптор открыт с FILE_FLAG_OVERLAPPED,
// поэтому запрос можно ждать по событию и отменять: по истечении таймаута
// CancelIoEx, затем короткое ожидание; если отмена не завершилась, буфер и
// OVERLAPPED намеренно не уничтожаются (драйвер может писать в них прямо
// сейчас), а следующие свойства на этом устройстве не запрашиваются. Утечка —
// несколько килобайт на зависшее устройство за жизнь процесса; молчаливый
// use-after-free дороже и заметно чаще, потому что «зависший» диск здесь
// регулярная ситуация, а не исключительная.
//
// Чего здесь нет намеренно: повышения прав (все три свойства запрашиваются
// IOCTL с FILE_ANY_ACCESS, поэтому GENERIC_READ достаточно, а SPEC §5 требует
// не повышать права ради показа данных), SMART и NVMe-здоровья (v1.1, §8
// Этап 6), тонкого выделения (StorageDeviceLBProvisioningProperty — чужое
// свойство и чужой модуль) и разбора третьего поля ответа
// IOCTL_STORAGE_GET_DEVICE_NUMBER (это про «съёмный/только чтение», см. шапку
// trim_cache.hpp).
//
// Своих RAII-обёрток и разборов кодов ошибок модуль не заводит: для этого есть
// win_handle.hpp (ADR-001) и win_error.hpp, и вторая реализация того же
// разъедутся с первой (та же мысль, что в devices.cpp).
#include "trim_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// windows.h включается вторым: он нужен для OVERLAPPED, CreateFileW и
// DeviceIoControl, а объявления свойств хранилища лежат в winioctl.h.
#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место
#include <winioctl.h>

#include "core/log.hpp"
#include "devices.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::trim_cache {
namespace {

using ScopedHandle = platform::unique_handle<platform::KernelHandlePolicy>;

// Раскладка ответа фиксируется сборкой, а не догадкой: если в следующем SDK
// структура изменится, модуль обязан остановиться на этапе сборки, а не читать
// поля по старым смещениям на живой системе.
static_assert(sizeof(STORAGE_DESCRIPTOR_HEADER) == 8, "заголовок дескриптора — два DWORD");
static_assert(sizeof(DEVICE_TRIM_DESCRIPTOR) == 12, "TRIM: Version, Size, TrimEnabled + выравнивание");
static_assert(sizeof(STORAGE_WRITE_CACHE_PROPERTY) == 28,
              "кэш записи: Version, Size, 4 перечисления, 3 BOOLEAN + выравнивание");
static_assert(sizeof(STORAGE_DEVICE_IO_CAPABILITY_DESCRIPTOR) == 16, "предел IO: Version, Size, два предела");

// Идентификаторы свойств — константы, чтобы в коде и в логе стояло то же
// написание, что в SPEC §4 FR-1 п.2, а не «магическое» число перечисления.
constexpr STORAGE_PROPERTY_ID kTrimProperty = StorageDeviceTrimProperty;
constexpr STORAGE_PROPERTY_ID kWriteCacheProperty = StorageDeviceWriteCacheProperty;
constexpr STORAGE_PROPERTY_ID kIoCapabilityProperty = StorageDeviceIoCapabilityProperty;

// Права доступа для попыток открыть устройство.
constexpr DWORD kAccessNone = 0;
constexpr DWORD kAccessAttributes = FILE_READ_ATTRIBUTES;
constexpr DWORD kAccessRead = GENERIC_READ;
constexpr DWORD kAccessReadWrite = GENERIC_READ | GENERIC_WRITE; // FR-1 п.3

// Цепочка прав, от требовательных к минимальным: каждая следующая строго мягче.
// Последний вариант — «без прав вовсе»: запросы с FILE_ANY_ACCESS он пропускает,
// поэтому диск, который нельзя открыть на чтение (занят другим процессом, том
// в BitLocker без ключа), не выпадает из инвентаря из-за одного свойства.
std::vector<DWORD> accessChain(Access access) {
    switch (access) {
        case Access::Read:
            return {kAccessRead, kAccessAttributes, kAccessNone};
        case Access::ReadWrite:
            return {kAccessReadWrite, kAccessRead, kAccessAttributes, kAccessNone};
        case Access::Minimal:
            return {kAccessAttributes, kAccessNone};
    }
    // Недостижимо при enum class, но компилятор требует возврата на всех путях.
    return {kAccessRead, kAccessAttributes, kAccessNone};
}

// Имя события модуля в логе — одной строкой, чтобы по grep находились все записи.
constexpr std::string_view kLogEvent = "platform.trim_cache.query";

// Открытый дескриптор устройства плюс счётчик «зависших» запросов. Счётчик
// уходит в результат: устройств, на которых пришлось бросить запрос, в
// инвентаре не видно, и «0 непрочитанных свойств» из-за таймаута не должен
// читаться как «устройство честно сообщило, что ничего не знает».
struct DeviceSession {
    ScopedHandle device;
    std::uint32_t abandonedCalls{};
};

// ---------------------------------------------------------------------------
// Буфер ответа и отправка запроса
// ---------------------------------------------------------------------------

// Буфер ответа держим в std::uint64_t, а не в std::byte, по одной причине:
// выравнивание. vector<std::byte> просит у аллокатора выравнивание
// sizeof(std::byte) == 1, а ответ читается как структура с выравниванием 4-8
// байт; с uint64_t аллокатор обязан вернуть память, выровненную под uint64_t, и
// приведение к указателю на структуру законно с точки зрения выравнивания.
// Сами байты ответа к типу uint64_t отношения не имеют — это буфер, а не число.
using RawWords = std::vector<std::uint64_t>;

std::size_t wordsFor(std::size_t bytes) noexcept {
    return (bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
}

// Всё, к чему драйвер может обратиться после таймаута, живёт в одном объекте на
// куче: отмена, которая не завершилась, означает, что объектом больше нельзя
// владеть (см. комментарий к файлу). Событие — тоже внутри: если утекает буфер,
// нельзя закрывать и событие, на которое драйвер поставил ожидание.
struct PendingRequest {
    RawWords words;
    OVERLAPPED overlapped{};
    ScopedHandle event;
};

struct PendingSlot {
    std::unique_ptr<PendingRequest> request;
    std::uint32_t error{ERROR_SUCCESS};
};

PendingSlot makePending(std::size_t bytes) {
    PendingSlot slot;
    slot.request = std::make_unique<PendingRequest>();
    slot.request->words.assign(wordsFor(bytes), 0);
    slot.request->event = platform::adopt(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!slot.request->event.valid()) {
        slot.error = static_cast<std::uint32_t>(::GetLastError());
        slot.request.reset();
        return slot;
    }
    slot.request->overlapped.hEvent = slot.request->event.get();
    return slot;
}

enum class SendStatus : std::uint8_t {
    Completed, // ответ получен
    Failed,    // драйвер отказал, код в win32Error
    TimedOut,  // ответ не пришёл за отведённое время, отмена удалась
    Abandoned, // отмена не завершилась: буфер утекает, устройство больше не трогать
};

struct SendOutcome {
    SendStatus status{SendStatus::Failed};
    std::uint32_t win32Error{ERROR_SUCCESS};
    std::uint32_t returned{}; // сколько байт драйвер записал
};

DWORD waitMilliseconds(std::chrono::milliseconds timeout) noexcept {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return 0;
    }
    constexpr auto limit = static_cast<std::chrono::milliseconds::rep>(MAXDWORD);
    const auto count = timeout.count();
    return count > limit ? MAXDWORD : static_cast<DWORD>(count);
}

// Запрос пишет в буфер вызывающего: PendingRequest владеет буфером, OVERLAPPED и
// событием вместе, и владелец обязан быть один — при незавершённой отмене тройку
// нельзя уничтожать, и «кто от неё избавится» не должно зависеть от того, кто
// первым догадается. Решение утекать принимает вызывающий: он владеет указателем
// и отпускает его через release() после Abandoned (см. readProperty).
//
// input — void*, а не const void*: сигнатура DeviceIoControl в SDK объявляет
// входной буфер как LPVOID и по контракту его не пишет. Наша
// STORAGE_PROPERTY_QUERY — неконстантная локальная переменная, поэтому
// приведение не требуется.
SendOutcome sendIoctl(HANDLE device, DWORD ioctlCode, void* input, DWORD inputBytes, PendingRequest& request,
                      DWORD outputBytes, DWORD timeoutMs) noexcept {
    SendOutcome outcome;

    DWORD returned = 0;
    const BOOL started = ::DeviceIoControl(device, ioctlCode, input, inputBytes, request.words.data(), outputBytes,
                                          &returned, &request.overlapped);
    if (started == FALSE) {
        const std::uint32_t startedError = static_cast<std::uint32_t>(::GetLastError());
        if (startedError != ERROR_IO_PENDING) {
            outcome.status = SendStatus::Failed;
            outcome.win32Error = startedError;
            return outcome;
        }
        const DWORD wait = ::WaitForSingleObject(request.event.get(), timeoutMs);
        if (wait == WAIT_TIMEOUT) {
            // FR-1: устройство не ответило за 2 с — помечаем недоступным и идём
            // дальше, а не ждём его вечно.
            ::CancelIoEx(device, &request.overlapped);
            if (::WaitForSingleObject(request.event.get(), static_cast<DWORD>(kCancelGrace.count())) !=
                WAIT_OBJECT_0) {
                // Драйвер ещё жив: писать в буфер он может прямо сейчас, а
                // уничтожать его нельзя. Возвращаем особое состояние, по
                // которому вызывающий утекает буфер и прекращает опрос
                // устройства: следующий запрос сюда был бы гонкой с драйвером.
                outcome.status = SendStatus::Abandoned;
                outcome.win32Error = static_cast<std::uint32_t>(ERROR_TIMEOUT);
                return outcome;
            }
            outcome.status = SendStatus::TimedOut;
            outcome.win32Error = static_cast<std::uint32_t>(ERROR_TIMEOUT);
            return outcome;
        }
        if (wait != WAIT_OBJECT_0) {
            outcome.status = SendStatus::Failed;
            outcome.win32Error = static_cast<std::uint32_t>(::GetLastError());
            return outcome;
        }
    }

    // GetOverlappedResult нужен в обоих случаях: при синхронном завершении
    // событие не сигнализируется, а число принятых байт есть только здесь.
    if (::GetOverlappedResult(device, &request.overlapped, &returned, FALSE) == FALSE) {
        outcome.status = SendStatus::Failed;
        outcome.win32Error = static_cast<std::uint32_t>(::GetLastError());
        return outcome;
    }
    outcome.status = SendStatus::Completed;
    outcome.returned = returned;
    return outcome;
}

// «Дай ещё места»: драйвер отказал не по существу, а из-за размера буфера.
bool isShortBuffer(DWORD code) noexcept {
    return code == ERROR_INSUFFICIENT_BUFFER || code == ERROR_MORE_DATA || code == ERROR_BUFFER_OVERFLOW;
}

// Код Win32 → состояние опроса. Разбор именно такой, потому что вызывающий
// обязан отличать «устройства нет» от «нет прав» и от «свойства нет»: это
// разные строки в отчёте и разные действия (FR-1: недоступное устройство
// помечается, а не теряется).
QueryStatus classifyFailure(DWORD code) noexcept {
    switch (code) {
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_BAD_COMMAND:
            return QueryStatus::Unsupported;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return QueryStatus::AccessDenied;
        case ERROR_TIMEOUT:
        case ERROR_IO_INCOMPLETE:
            return QueryStatus::TimedOut;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_DEVICE_NOT_CONNECTED:
            return QueryStatus::NotFound;
        case ERROR_DEVICE_REMOVED:
            return QueryStatus::DeviceRemoved;
        default:
            return QueryStatus::Unavailable;
    }
}

// Что драйвер написал в заголовке ответа. 0 — «не написал»: так отвечает
// драйвер, проигнорировавший двухпроходную схему, и это не повод принимать
// неразобранные байты за данные.
std::uint32_t headerSize(const std::uint64_t* words, std::size_t wordCount) noexcept {
    if (words == nullptr || wordCount < wordsFor(sizeof(STORAGE_DESCRIPTOR_HEADER))) {
        return 0;
    }
    const auto* header = reinterpret_cast<const STORAGE_DESCRIPTOR_HEADER*>(words);
    return header->Size;
}

std::chrono::milliseconds elapsedSince(std::chrono::steady_clock::time_point started) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
}

// ---------------------------------------------------------------------------
// Запрос одного свойства
// ---------------------------------------------------------------------------

// Итог разбора одного свойства — до превращения в тип модуля.
struct PropertyAnswer {
    QueryStatus status{QueryStatus::Unavailable};
    std::uint32_t win32Error{ERROR_SUCCESS};
    std::uint32_t version{};
    std::uint32_t descriptorSize{};
    std::uint32_t returnedBytes{};
};

// Два прохода IOCTL_STORAGE_QUERY_PROPERTY. Разобранная структура попадает в raw
// только при Ok: частично заполненный дескриптор в Info проникать не должен.
template <class Descriptor>
PropertyAnswer readProperty(HANDLE device, STORAGE_PROPERTY_ID propertyId, const QueryOptions& options,
                            std::uint32_t& abandoned, Descriptor& raw) {
    PropertyAnswer answer;
    const DWORD wait = waitMilliseconds(options.timeout);

    STORAGE_PROPERTY_QUERY request{};
    request.PropertyId = propertyId;
    request.QueryType = PropertyStandardQuery;
    // AdditionalParameters не заполняется: у наших трёх свойств дополнительных
    // параметров нет, а поле объявлено как BYTE[1] — заполнять нечего.

    // Проход 1: буфер ровно под заголовок дескриптора. Нормальный ответ — отказ
    // ERROR_INSUFFICIENT_BUFFER с заполненным Size в заголовке; если драйвер
    // уложился в 8 байт, для наших дескрипторов (12/28/16) это усечение, и
    // тогда просто берём размер структуры.
    std::uint32_t needed = 0;
    {
        const DWORD headerBytes = static_cast<DWORD>(sizeof(STORAGE_DESCRIPTOR_HEADER));
        PendingSlot first = makePending(headerBytes);
        if (!first.request) {
            answer.status = QueryStatus::Unavailable;
            answer.win32Error = first.error;
            return answer;
        }
        const SendOutcome probe = sendIoctl(device, IOCTL_STORAGE_QUERY_PROPERTY, &request,
                                            static_cast<DWORD>(sizeof(request)), *first.request, headerBytes, wait);
        if (probe.status == SendStatus::Abandoned) {
            // Драйвер ещё может писать в буфер и в OVERLAPPED: тройка утекает
            // целиком, дальше это устройство не опрашивается.
            (void)first.request.release();
            abandoned = 1;
            answer.status = QueryStatus::TimedOut;
            answer.win32Error = probe.win32Error;
            return answer;
        }
        if (probe.status == SendStatus::TimedOut) {
            answer.status = QueryStatus::TimedOut;
            answer.win32Error = probe.win32Error;
            return answer;
        }
        if (probe.status == SendStatus::Failed && !isShortBuffer(probe.win32Error)) {
            answer.status = classifyFailure(probe.win32Error);
            answer.win32Error = probe.win32Error;
            return answer;
        }
        needed = headerSize(first.request->words.data(), first.request->words.size());
    }

    // Проход 2: буфер по размеру из заголовка. Если драйвер вопреки своему же
    // Size снова просит «ещё», делается ровно одна повторная попытка с
    // предельным размером: дальше это уже перебор бюджета на каждый диск
    // системы. Обход без повтора потерял бы дескриптор на драйвере, который врёт
    // про Size, а «просто не показали TRIM» хуже одного лишнего IOCTL.
    std::uint32_t size = std::max(needed, static_cast<std::uint32_t>(sizeof(Descriptor)));
    PendingSlot slot;
    SendOutcome outcome{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (size > options.maxDescriptorBytes) {
            // Драйвер запросил больше, чем мы готовы выделить: для дескриптора в
            // 12-28 байт это подозрительно, и принимать такое нельзя.
            answer.status = QueryStatus::InvalidAnswer;
            answer.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_DATA);
            return answer;
        }
        slot = makePending(size);
        if (!slot.request) {
            answer.status = QueryStatus::Unavailable;
            answer.win32Error = slot.error;
            return answer;
        }
        outcome = sendIoctl(device, IOCTL_STORAGE_QUERY_PROPERTY, &request, static_cast<DWORD>(sizeof(request)),
                            *slot.request, size, wait);
        const bool wantRetry = outcome.status == SendStatus::Failed && isShortBuffer(outcome.win32Error) &&
                               size < options.maxDescriptorBytes;
        if (!wantRetry) {
            break;
        }
        size = options.maxDescriptorBytes;
    }

    if (outcome.status == SendStatus::Abandoned) {
        // Буфер ответа утекает вместе с OVERLAPPED и событием (см. PendingRequest),
        // а оставшиеся свойства устройства вызывающий не запрашивает по
        // возвращённому abandoned.
        (void)slot.request.release();
        abandoned = 1;
        answer.status = QueryStatus::TimedOut;
        answer.win32Error = outcome.win32Error;
        return answer;
    }
    if (outcome.status == SendStatus::TimedOut) {
        answer.status = QueryStatus::TimedOut;
        answer.win32Error = outcome.win32Error;
        return answer;
    }
    if (outcome.status == SendStatus::Failed) {
        // Отказ «мал буфер» после предельной попытки — это не отказ устройства,
        // а нехватка наших предохранителей; остальное разбирается по коду.
        answer.status =
            isShortBuffer(outcome.win32Error) ? QueryStatus::InvalidAnswer : classifyFailure(outcome.win32Error);
        answer.win32Error = outcome.win32Error;
        return answer;
    }

    // Ответ получен — проверяем, что его можно разбирать. Короче структуры —
    // читать нечего; длиннее буфера — драйвер или GetOverlappedResult врёт, и
    // доверять такому ответу нельзя.
    if (outcome.returned < sizeof(Descriptor) || outcome.returned > size) {
        answer.status = QueryStatus::InvalidAnswer;
        answer.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_DATA);
        return answer;
    }

    const std::uint64_t* const words = slot.request->words.data();
    const auto* header = reinterpret_cast<const STORAGE_DESCRIPTOR_HEADER*>(words);
    answer.version = header->Version;
    answer.descriptorSize = header->Size;
    if (answer.descriptorSize != 0 && answer.descriptorSize < sizeof(Descriptor)) {
        // Драйвер сам признался, что отдал меньше, чем нужно: разбирать нечего,
        // и молча читать поля за пределами ответа — это ровно тот выход за
        // границу буфера, ради которого проверка и написана.
        answer.status = QueryStatus::InvalidAnswer;
        answer.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_DATA);
        return answer;
    }

    answer.returnedBytes = outcome.returned;
    raw = *reinterpret_cast<const Descriptor*>(words);
    answer.status = QueryStatus::Ok;
    return answer;
}

// ---------------------------------------------------------------------------
// Превращение ответа драйвера в типы модуля
// ---------------------------------------------------------------------------

TrimInfo toInfo(const DEVICE_TRIM_DESCRIPTOR& raw, const PropertyAnswer& answer) noexcept {
    TrimInfo info;
    info.rawTrimEnabled = raw.TrimEnabled; // BOOLEAN — unsigned char
    info.trimEnabled = toTristate(raw.TrimEnabled);
    info.version = answer.version;
    info.descriptorSize = answer.descriptorSize;
    info.returnedBytes = answer.returnedBytes;
    return info;
}

WriteCacheInfo toInfo(const STORAGE_WRITE_CACHE_PROPERTY& raw, const PropertyAnswer& answer) noexcept {
    WriteCacheInfo info;
    info.rawType = static_cast<std::uint32_t>(raw.WriteCacheType);
    info.type = toWriteCacheType(info.rawType);
    info.enabled = toTristate(static_cast<std::uint8_t>(raw.WriteCacheEnabled));
    info.changeable = toTristate(static_cast<std::uint8_t>(raw.WriteCacheChangeable));
    info.writeThroughSupported = toTristate(static_cast<std::uint8_t>(raw.WriteThroughSupported));
    info.flushCacheSupported = toTristate(static_cast<std::uint8_t>(raw.FlushCacheSupported));
    info.powerProtection = toTristate(static_cast<std::uint8_t>(raw.UserDefinedPowerProtection));
    info.batteryBacked = toTristate(static_cast<std::uint8_t>(raw.NVCacheEnabled));
    info.version = answer.version;
    info.descriptorSize = answer.descriptorSize;
    info.returnedBytes = answer.returnedBytes;
    return info;
}

IoCapabilityInfo toInfo(const STORAGE_DEVICE_IO_CAPABILITY_DESCRIPTOR& raw, const PropertyAnswer& answer) noexcept {
    IoCapabilityInfo info;
    info.lunMaxIoCount = raw.LunMaxIoCount;
    info.adapterMaxIoCount = raw.AdapterMaxIoCount;
    info.version = answer.version;
    info.descriptorSize = answer.descriptorSize;
    info.returnedBytes = answer.returnedBytes;
    return info;
}

// ---------------------------------------------------------------------------
// Открытие устройства
// ---------------------------------------------------------------------------

// Права доступа по цепочке, от требовательных к минимальным. Последний вариант —
// «без прав вовсе»: IOCTL с FILE_ANY_ACCESS он пропускает, поэтому диск,
// который нельзя открыть на чтение (занят другим процессом, том в BitLocker
// без ключа), не выпадает из инвентаря из-за одного свойства.
QueryStatus openDevice(DeviceSession& session, std::wstring_view devicePath, Access access, std::uint32_t& error) {
    error = ERROR_SUCCESS;
    if (devicePath.empty()) {
        error = static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER);
        return QueryStatus::InvalidArgument;
    }

    // CreateFileW требует завершающий NUL, а путь из SetupAPI может быть
    // подстрокой чужой строки: копия здесь, а не «поверьте, что вызвали с
    // wstring».
    const std::wstring path(devicePath);
    for (const DWORD rights : accessChain(access)) {
        // FILE_SHARE_READ|WRITE обязателен: пока диском пользуется система (а
        // это обычное дело), без них драйвер откажет.
        ScopedHandle device = platform::adopt(::CreateFileW(path.c_str(), rights, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (device.valid()) {
            session.device = std::move(device);
            return QueryStatus::Ok;
        }
        error = static_cast<std::uint32_t>(::GetLastError());
        if (error == ERROR_INVALID_PARAMETER) {
            // Путь драйвер не понимает: ослабление прав тут не поможет, цепочка
            // дальше бессмысленна.
            break;
        }
    }
    return classifyFailure(error);
}

// ---------------------------------------------------------------------------
// Логирование и заполнение результата
// ---------------------------------------------------------------------------

// Отказ по свойству — в лог с путём и кодом Windows (SPEC §5, §12).
//
// Unsupported — не сбой, а норма для части носителей (адаптеры отдают не все
// свойства), поэтому он уходит на debug: иначе каждая инвентаризация
// засорялась бы строками про ожидаемое. Уровень вообще выключается вызывающим
// (QueryOptions::logFailures) там, где неудачи ожидаемы по условию.
void logPropertyFailure(const std::wstring& devicePath, std::string_view property, QueryStatus status,
                        std::uint32_t win32Error, std::chrono::milliseconds elapsed, bool enabled) {
    if (!enabled || status == QueryStatus::Ok) {
        return;
    }

    // Поля собираются вручную: макросы MRP_LOG_* для списка из двух и более
    // пар непригодны (detail::logFieldList разворачивает пакет в один вызов
    // logField, и такой вызов не разрешается). Чужой заголовок не правим.
    mrproper::core::LogFields fields{
        mrproper::core::logField("property", property),
        mrproper::core::logField("status", toString(status)),
        mrproper::core::logField("devicePath", devicePath),
    };
    if (win32Error != 0) {
        // Текст системы рядом с кодом: «ERROR_ACCESS_DENIED = 5» в логе читается
        // хуже, чем «Отказано в доступе» (SPEC §5, §12).
        fields.push_back(mrproper::core::logField("win32", win32Error));
        fields.push_back(mrproper::core::logField("errorText", platform::win32ErrorText(win32Error)));
    }
    if (elapsed.count() > 0) {
        fields.push_back(mrproper::core::logField("elapsedMs", elapsed.count()));
    }

    if (status == QueryStatus::Unsupported) {
        mrproper::core::logDebug(kLogEvent, "свойство не поддерживается устройством", fields);
        return;
    }
    mrproper::core::logWarn(kLogEvent, "свойство хранилища не прочитано", fields);
}

// Свойство не опрашивали вовсе — обычно потому, что предыдущий запрос на этом
// устройстве пришлось бросить. Молчать об этом нельзя: иначе в отчёте «TRIM
// выключен» и «TRIM не читали» выглядят одинаково.
template <class Info>
void markNotRead(PropertyResult<Info>& result, QueryStatus status, std::uint32_t win32Error) noexcept {
    result.status = status;
    result.win32Error = win32Error;
    result.info = Info{};
}

// Нехватка памяти посреди обхода: помечаем непрочитанным только то, что ещё не
// прочитано. Верхний статус открытия устройства не трогаем — открытие от
// выделения памяти не зависит, и сбрасывать его было бы ложью.
//
// Функция ничего не выделяет и ничего не логирует: вызывается прямо из
// обработчика std::bad_alloc, где и выделение, и бросок исключения опасны.
// Запись о нехватке памяти делает вызывающий код — одним вызовом без полей.
template <class Info>
void markOutOfMemory(PropertyResult<Info>& result) noexcept {
    if (!result.ok()) {
        result.status = QueryStatus::Unavailable;
        result.win32Error = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
    }
}

// ---------------------------------------------------------------------------
// Тела запросов
// ---------------------------------------------------------------------------

// Один запрос одного свойства. Шаблон на пару типов, а не три копии: Info и
// Descriptor обязаны соответствовать друг другу (toInfo перегружен по типу
// ответа), и компилятор проверяет это на месте вызова.
template <class Info, class Descriptor>
void readSingleProperty(std::wstring_view devicePath, const QueryOptions& options, STORAGE_PROPERTY_ID propertyId,
                        std::string_view propertyName, PropertyResult<Info>& out) {
    const auto started = std::chrono::steady_clock::now();
    const std::wstring path(devicePath);

    DeviceSession session;
    std::uint32_t openError = ERROR_SUCCESS;
    out.status = openDevice(session, path, options.access, openError);
    out.win32Error = openError;
    if (out.status != QueryStatus::Ok) {
        out.elapsed = elapsedSince(started);
        logPropertyFailure(path, propertyName, out.status, out.win32Error, out.elapsed, options.logFailures);
        return;
    }

    Descriptor raw{};
    const PropertyAnswer answer =
        readProperty<Descriptor>(session.device.get(), propertyId, options, session.abandonedCalls, raw);
    out.status = answer.status;
    out.win32Error = answer.win32Error;
    out.elapsed = elapsedSince(started);
    if (answer.status == QueryStatus::Ok) {
        out.info = toInfo(raw, answer);
    }
    logPropertyFailure(path, propertyName, out.status, out.win32Error, out.elapsed, options.logFailures);
}

// Три свойства за одно открытие устройства. Тело может бросить std::bad_alloc —
// единственное исключение, которого стоит ждать от кода с выделением памяти;
// noexcept-обёртка ловит его и превращает в статус.
void readStorageFlags(std::wstring_view devicePath, int diskNumber, const QueryOptions& options,
                      StorageFlagsResult& result) {
    const auto started = std::chrono::steady_clock::now();
    result.diskNumber = diskNumber;
    result.devicePath.assign(devicePath);

    DeviceSession session;
    std::uint32_t openError = ERROR_SUCCESS;
    result.status = openDevice(session, result.devicePath, options.access, openError);
    result.win32Error = openError;
    if (result.status != QueryStatus::Ok) {
        result.elapsed = elapsedSince(started);
        if (options.logFailures) {
            mrproper::core::LogFields fields{
                mrproper::core::logField("disk", result.diskNumber),
                mrproper::core::logField("status", toString(result.status)),
                mrproper::core::logField("devicePath", result.devicePath),
            };
            if (result.win32Error != 0) {
                fields.push_back(mrproper::core::logField("win32", result.win32Error));
                fields.push_back(mrproper::core::logField("errorText", platform::win32ErrorText(result.win32Error)));
            }
            mrproper::core::logWarn(kLogEvent, "устройство не открыто для чтения свойств", fields);
        }
        return;
    }

    // Одна форма на три свойства. Порядок фиксирован и одинаков для всех
    // дисков — он же порядок полей в отчёте. После Abandoned опрос прекращается:
    // устройство ещё держит предыдущий буфер.
    DEVICE_TRIM_DESCRIPTOR trimRaw{};
    STORAGE_WRITE_CACHE_PROPERTY writeCacheRaw{};
    STORAGE_DEVICE_IO_CAPABILITY_DESCRIPTOR ioRaw{};

    const auto readOne = [&session, &options, &result](auto& property, STORAGE_PROPERTY_ID id, const char* name,
                                                      auto& raw) {
        using Descriptor = std::remove_cvref_t<decltype(raw)>;
        const auto propertyStarted = std::chrono::steady_clock::now();
        const PropertyAnswer answer =
            readProperty<Descriptor>(session.device.get(), id, options, session.abandonedCalls, raw);
        property.status = answer.status;
        property.win32Error = answer.win32Error;
        if (answer.status == QueryStatus::Ok) {
            property.info = toInfo(raw, answer);
        }
        logPropertyFailure(result.devicePath, name, property.status, property.win32Error, elapsedSince(propertyStarted),
                           options.logFailures);
        return session.abandonedCalls == 0;
    };

    constexpr auto timeoutCode = static_cast<std::uint32_t>(ERROR_TIMEOUT);
    if (!readOne(result.flags.trim, kTrimProperty, "trim", trimRaw)) {
        markNotRead(result.flags.writeCache, QueryStatus::TimedOut, timeoutCode);
        markNotRead(result.flags.ioCapability, QueryStatus::TimedOut, timeoutCode);
    } else if (!readOne(result.flags.writeCache, kWriteCacheProperty, "writeCache", writeCacheRaw)) {
        markNotRead(result.flags.ioCapability, QueryStatus::TimedOut, timeoutCode);
    } else {
        (void)readOne(result.flags.ioCapability, kIoCapabilityProperty, "ioCapability", ioRaw);
    }

    result.abandonedCalls = session.abandonedCalls;
    result.elapsed = elapsedSince(started);

    if (options.logFailures) {
        mrproper::core::LogFields fields{
            mrproper::core::logField("disk", result.diskNumber),
            mrproper::core::logField("trim", toString(result.flags.trim.status)),
            mrproper::core::logField("writeCache", toString(result.flags.writeCache.status)),
            mrproper::core::logField("ioCapability", toString(result.flags.ioCapability.status)),
            mrproper::core::logField("abandoned", result.abandonedCalls),
            mrproper::core::logField("elapsedMs", result.elapsed.count()),
        };
        mrproper::core::logInfo(kLogEvent, "свойства хранилища прочитаны", fields);
    }
}

// Да/нет для дампа.
std::string yesNo(Tristate value) noexcept {
    switch (value) {
        case Tristate::Yes:
            return "да";
        case Tristate::No:
            return "нет";
        case Tristate::Unknown:
            break;
    }
    return "неизвестно";
}

}  // namespace

// ---------------------------------------------------------------------------
// Имена состояний и их тексты
// ---------------------------------------------------------------------------

const char* toString(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::Ok:
            return "ok";
        case QueryStatus::Unsupported:
            return "unsupported";
        case QueryStatus::NotFound:
            return "not_found";
        case QueryStatus::AccessDenied:
            return "access_denied";
        case QueryStatus::TimedOut:
            return "timed_out";
        case QueryStatus::DeviceRemoved:
            return "device_removed";
        case QueryStatus::InvalidAnswer:
            return "invalid_answer";
        case QueryStatus::InvalidArgument:
            return "invalid_argument";
        case QueryStatus::Unavailable:
            break;
    }
    return "unavailable";
}

const wchar_t* toWideString(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::Ok:
            return L"ok";
        case QueryStatus::Unsupported:
            return L"unsupported";
        case QueryStatus::NotFound:
            return L"not_found";
        case QueryStatus::AccessDenied:
            return L"access_denied";
        case QueryStatus::TimedOut:
            return L"timed_out";
        case QueryStatus::DeviceRemoved:
            return L"device_removed";
        case QueryStatus::InvalidAnswer:
            return L"invalid_answer";
        case QueryStatus::InvalidArgument:
            return L"invalid_argument";
        case QueryStatus::Unavailable:
            break;
    }
    return L"unavailable";
}

std::wstring formatStatusWide(QueryStatus status, std::uint32_t win32Error) {
    if (status == QueryStatus::Ok) {
        return L"свойство прочитано";
    }

    // Имя состояния плюс текст самой системы: «ERROR_ACCESS_DENIED = 5» в логе
    // читается хуже, чем «Отказано в доступе» (SPEC §5, §12).
    std::wstring text = toWideString(status);
    const std::wstring detail = platform::formatSystemMessage(static_cast<DWORD>(win32Error));
    if (!detail.empty()) {
        text += L": ";
        text += detail;
    }
    return text;
}

std::string formatStatus(QueryStatus status, std::uint32_t win32Error) {
    return platform::toUtf8(formatStatusWide(status, win32Error));
}

const char* toString(Tristate value) noexcept {
    switch (value) {
        case Tristate::Yes:
            return "yes";
        case Tristate::No:
            return "no";
        case Tristate::Unknown:
            break;
    }
    return "unknown";
}

const char* toString(WriteCacheType value) noexcept {
    switch (value) {
        case WriteCacheType::None:
            return "none";
        case WriteCacheType::WriteBack:
            return "write-back";
        case WriteCacheType::WriteThrough:
            return "write-through";
        case WriteCacheType::Unknown:
            break;
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Приведение значений драйвера
// ---------------------------------------------------------------------------

Tristate toTristate(std::uint8_t raw) noexcept {
    // 0xFF в этих дескрипторах — «драйвер не знает», а не «нет». Всё, кроме
    // строгих 0 и 1, трактуется как неизвестность: показать пользователю
    // «выключено» там, где драйвер промолчал, — значит соврать (SPEC §2: у
    // каждого показанного поля должно быть объяснение).
    if (raw == 1) {
        return Tristate::Yes;
    }
    if (raw == 0) {
        return Tristate::No;
    }
    return Tristate::Unknown;
}

bool toBool(Tristate value, bool unknownValue) noexcept {
    switch (value) {
        case Tristate::Yes:
            return true;
        case Tristate::No:
            return false;
        case Tristate::Unknown:
            break;
    }
    return unknownValue;
}

WriteCacheType toWriteCacheType(std::uint32_t raw) noexcept {
    switch (raw) {
        case 0:
            return WriteCacheType::Unknown; // WriteCacheTypeUnknown
        case 1:
            return WriteCacheType::None; // WriteCacheTypeNone
        case 2:
            return WriteCacheType::WriteBack; // WriteCacheTypeWriteBack
        case 3:
            return WriteCacheType::WriteThrough; // WriteCacheTypeWriteThrough
        default:
            break;
    }
    return WriteCacheType::Unknown;
}

// ---------------------------------------------------------------------------
// Запросы
// ---------------------------------------------------------------------------

PropertyResult<TrimInfo> queryTrim(std::wstring_view devicePath, const QueryOptions& options) noexcept {
    PropertyResult<TrimInfo> result;
    try {
        readSingleProperty<TrimInfo, DEVICE_TRIM_DESCRIPTOR>(devicePath, options, kTrimProperty, "trim", result);
    } catch (const std::bad_alloc&) {
        markOutOfMemory(result);
        // Без полей: вызываем из обработчика нехватки памяти, а сборка полей
        // лога её только усилила бы.
        mrproper::core::logWarn(kLogEvent, "не хватило памяти на чтение свойства хранилища");
    }
    return result;
}

PropertyResult<WriteCacheInfo> queryWriteCache(std::wstring_view devicePath, const QueryOptions& options) noexcept {
    PropertyResult<WriteCacheInfo> result;
    try {
        readSingleProperty<WriteCacheInfo, STORAGE_WRITE_CACHE_PROPERTY>(devicePath, options, kWriteCacheProperty,
                                                                        "writeCache", result);
    } catch (const std::bad_alloc&) {
        markOutOfMemory(result);
        mrproper::core::logWarn(kLogEvent, "не хватило памяти на чтение свойства хранилища");
    }
    return result;
}

PropertyResult<IoCapabilityInfo> queryIoCapability(std::wstring_view devicePath, const QueryOptions& options) noexcept {
    PropertyResult<IoCapabilityInfo> result;
    try {
        readSingleProperty<IoCapabilityInfo, STORAGE_DEVICE_IO_CAPABILITY_DESCRIPTOR>(
            devicePath, options, kIoCapabilityProperty, "ioCapability", result);
    } catch (const std::bad_alloc&) {
        markOutOfMemory(result);
        mrproper::core::logWarn(kLogEvent, "не хватило памяти на чтение свойства хранилища");
    }
    return result;
}

StorageFlagsResult queryStorageFlags(std::wstring_view devicePath, const QueryOptions& options) noexcept {
    StorageFlagsResult result;
    try {
        readStorageFlags(devicePath, -1, options, result);
    } catch (const std::bad_alloc&) {
        // Частичный ответ не выбрасываем: то, что устройство уже сообщило, — это
        // верные данные, и потерять их хуже, чем потерять одно оставшееся.
        markOutOfMemory(result.flags.trim);
        markOutOfMemory(result.flags.writeCache);
        markOutOfMemory(result.flags.ioCapability);
        mrproper::core::logWarn(kLogEvent, "не хватило памяти на чтение свойств: ответ частичный");
    }
    return result;
}

StorageFlagsResult queryStorageFlagsOnDisk(int diskNumber, const QueryOptions& options) noexcept {
    if (diskNumber < 0) {
        StorageFlagsResult result;
        result.diskNumber = diskNumber;
        result.status = QueryStatus::InvalidArgument;
        result.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER);
        return result;
    }
    return queryStorageFlags(physicalDrivePath(diskNumber), options);
}

std::wstring physicalDrivePath(int diskNumber) {
    // Своя функция в namespace trim_cache остаётся для локальных вызовов,
    // но путь строит единственный владелец — devices.
    if (diskNumber < 0) {
        return {};
    }
    return devices::physicalDrivePath(diskNumber);
}

// ---------------------------------------------------------------------------
// Тексты для лога и дампа
// ---------------------------------------------------------------------------

namespace {

template <class Info>
void appendProperty(std::string& out, std::string_view label, const PropertyResult<Info>& result,
                    std::string_view okText) {
    if (!out.empty()) {
        out += "; ";
    }
    out += label;
    out += ": ";
    out += result.ok() ? std::string(okText) : formatStatus(result.status, result.win32Error);
}

}  // namespace

std::string describe(const TrimInfo& info) {
    switch (info.trimEnabled) {
        case Tristate::Yes:
            return "TRIM включён";
        case Tristate::No:
            return "TRIM выключен";
        case Tristate::Unknown:
            break;
    }
    return "TRIM неизвестен";
}

std::string describe(const WriteCacheInfo& info) {
    std::string out = "кэш записи: тип=";
    out += toString(info.type);
    out += ", включён=";
    out += yesNo(info.enabled);
    out += ", батарея=";
    out += yesNo(info.batteryBacked);
    out += ", переключаемый=";
    out += yesNo(info.changeable);
    out += ", сквозная запись=";
    out += yesNo(info.writeThroughSupported);
    out += ", сброс кэша=";
    out += yesNo(info.flushCacheSupported);
    return out;
}

std::string describe(const IoCapabilityInfo& info) {
    if (info.lunMaxIoCount == 0 && info.adapterMaxIoCount == 0) {
        // Драйвер не сообщил ни одного предела: это «неизвестно», а не «очереди
        // нет» — иначе пользователю покажут неправду о пределе.
        return "предел числа IO неизвестен";
    }
    return "предел числа IO: LUN=" + std::to_string(info.lunMaxIoCount) +
           ", адаптер=" + std::to_string(info.adapterMaxIoCount);
}

std::string describe(const StorageFlagsResult& result) {
    if (!result.ok()) {
        return "устройство не прочитано: " + formatStatus(result.status, result.win32Error);
    }

    std::string out;
    appendProperty(out, "TRIM", result.flags.trim, describe(result.flags.trim.info));
    appendProperty(out, "кэш записи", result.flags.writeCache, describe(result.flags.writeCache.info));
    appendProperty(out, "предел числа IO", result.flags.ioCapability, describe(result.flags.ioCapability.info));

    if (result.flags.writeCache.ok() && result.flags.writeCache.info.volatileWriteCache()) {
        // Причина, по которой вообще стоило разбирать все поля кэша: режим
        // write-back без батареи — единственное состояние, при котором
        // отключение питания теряет уже записанные данные.
        out += "; кэш записи энергонезависимый (write-back без батареи)";
    }
    if (result.abandonedCalls != 0) {
        out += "; часть запросов отменена по таймауту";
    }
    return out;
}

}  // namespace mrproper::platform::trim_cache
