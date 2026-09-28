// Реализация storage_query: единственный файл модуля, где встречается windows.h.
// Так же, как devices.cpp и size_probe.cpp: Win32-специфика живёт в платформе
// (SPEC §6.1, ADR-004), и граница проходит по границе каталогов.
//
// Слои одной попытки (SPEC §4 FR-1 п.2):
//   1) открыть устройство, перебирая права доступа от минимальных к требовательным;
//   2) PropertyExistsQuery — отличить «свойства нет» от «запрос упал»;
//   3) два PropertyStandardQuery: первый с пустым буфером узнаёт размер ответа,
//      второй читает дескриптор;
//   4) разбор смещений строго в границах фактически принятого ответа;
//   5) отказ, таймаут или нечитаемый ответ — статус с кодом Win32 и записью в
//      лог, а не исключение (SPEC §4 FR-1: «приложение не падает»).
//
// Таймаут на ответ, а не на CreateFileW: открытие синхронно, и ограничить его
// изнутри модуля нельзя — это делает вызывающий, отправляя запрос в рабочий поток
// пула (SPEC §6.4). Здесь ловится зависание на IOCTL: дескриптор открыт с
// FILE_FLAG_OVERLAPPED, запрос уходит асинхронно и ждёт своего события.
//
// Про утечку буфера на таймауте. После CancelIoEx драйвер теоретически может
// ещё писать в буфер ответа, и освобождать такую память нельзя. Поэтому при
// неудачной отмене буфер не освобождается, а счётчик abandonedCalls растёт: это
// утечка величиной в один дескриптор свойств на одно зависшее устройство за
// жизнь процесса, и молчаливый use-after-free после этого дороже на порядок.
// Тот же приём и с той же оговоркой — в devices.cpp.
#include "storage_query.hpp"

// WIN32_LEAN_AND_MEAN определяется профилем сборки слоя (src/platform/CMakeLists.txt),
// поэтому здесь он не объявляется: повторное определение того же макроса — это
// предупреждение C4005, а с /WX оно становится ошибкой.
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Свои RAII-обёртки и свой разбор кодов ошибок в слое не заводятся: для этого
// есть win_handle.hpp (ADR-001) и win_error.hpp, и вторая реализация того же
// разъедутся с первой за месяц (та же мысль, что в devices.cpp).
#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::storage_query {
namespace {

using ScopedHandle = platform::unique_handle<platform::KernelHandlePolicy>;

// Вызовы, которым пришлось бросить буфер ответа: за ними остались операции в
// драйвере. Счётчик — только для отчёта и отладки (SPEC §12), на поведение
// запросов не влияет.
std::atomic<std::uint32_t> gAbandonedCalls{0};

// Сколько ждать после CancelIoEx, прежде чем признать отмену неудачной. Не
// ноль: без этого ожидания буфер освободился бы под драйвером.
constexpr std::uint32_t kCancelGraceMs = 1000;

// Верхняя граница ответа. Дескриптор — это заголовок плюс несколько строк,
// настоящий ответ занимает сотни байт. Килобайт с запасом: предохранитель от
// драйвера, который сообщил ошибочный размер (FR-1: обход не должен ни упасть,
// ни уйти в аллокацию на мусор).
constexpr std::size_t kMaxPropertyBytes = 64u * 1024u;

// Сколько идентификаторов перебираем в STORAGE_DEVICE_ID_DESCRIPTOR. Настоящий
// диск отдаёт один-два; тридцать два — предохранитель от драйвера, который
// заявил миллион.
constexpr std::uint32_t kMaxIdentifiers = 32u;

// Заголовок одного идентификатора — всё, что до Identifier[1]. От этой границы
// считаются длины при обходе списка.
constexpr std::size_t kIdentifierHeaderBytes = offsetof(STORAGE_IDENTIFIER, Identifier);
static_assert(kIdentifierHeaderBytes == 16, "STORAGE_IDENTIFIER: заголовок 4+4+2+2+4 байт");

// Дескрипторы читаются из буфера ответа как есть, поэтому выравнивание буфера
// должно покрывать выравнивание структуры. Память под ответ берётся оператором
// new, то есть выровнена по __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 на x64), а
// обе структуры хранят DWORD и enum — им хватает и четырёх байт.
static_assert(alignof(STORAGE_DEVICE_DESCRIPTOR) <= alignof(std::max_align_t),
              "буфер ответа выровнен operator new под любой фундаментальный тип");
static_assert(alignof(STORAGE_DEVICE_ID_DESCRIPTOR) <= alignof(std::max_align_t),
              "буфер ответа выровнен operator new под любой фундаментальный тип");

// Права доступа, которые пробуем по очереди, от минимальных к требовательным.
// Порядок не переставлен для красоты: SPEC §5 требует, чтобы приложение работало
// без повышения прав, а IOCTL_STORAGE_QUERY_PROPERTY объявлен с FILE_ANY_ACCESS,
// то есть модель и серийник диска отдаются и при нулевом наборе прав (так и
// открывают \\.\PhysicalDriveN инструменты инвентаризации). GENERIC_READ на
// физическом диске без повышения обычно отказывает, поэтому он идёт следом:
// сначала то, что работает у обычного пользователя, и только потом то, что
// нужно отдельным драйверам.
constexpr DWORD kAccessChain[] = {
    0,
    FILE_READ_ATTRIBUTES,
    GENERIC_READ,
    GENERIC_READ | GENERIC_WRITE,
};

// ---------------------------------------------------------------------------
// Буфер ответа
// ---------------------------------------------------------------------------

// Владение памятью, в которую пишет драйвер. Пока он наш — освобождается сам;
// если отмена запроса не сработала, буфер уходит драйверу вместе с утечкой.
class ResponseBuffer {
public:
    ResponseBuffer() noexcept = default;

    explicit ResponseBuffer(std::size_t bytes) {
        if (bytes != 0) {
            data_ = std::make_unique<std::byte[]>(bytes);
            size_ = bytes;
        }
    }

    ResponseBuffer(const ResponseBuffer&) = delete;
    ResponseBuffer& operator=(const ResponseBuffer&) = delete;
    ResponseBuffer(ResponseBuffer&&) = default;
    ResponseBuffer& operator=(ResponseBuffer&&) = default;
    ~ResponseBuffer() = default;

    [[nodiscard]] std::byte* data() noexcept { return data_.get(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Память остаётся за драйвером: драйвер может записать в неё после
    // таймаута, поэтому освободить её можно только вместе с утечкой.
    void giveUpToDriver() noexcept {
        size_ = 0;
        (void)data_.release();
    }

private:
    std::unique_ptr<std::byte[]> data_;
    std::size_t size_{};
};

// ---------------------------------------------------------------------------
// Код ошибки Win32 → состояние запроса
// ---------------------------------------------------------------------------

QueryStatus classify(std::uint32_t error) noexcept {
    switch (error) {
        case ERROR_SUCCESS:
            return QueryStatus::Ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NO_MORE_FILES:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_DEVICE_NOT_CONNECTED:
            return QueryStatus::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_SHARING_VIOLATION:
            return QueryStatus::AccessDenied;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return QueryStatus::Unsupported;
        // Ответ есть, но разобрать его нельзя: обрезан, или драйвер прислал
        // смещение, которого в ответе нет. Это не «устройство недоступно» и не
        // «свойства нет», поэтому у него своё состояние.
        case ERROR_INVALID_DATA:
        case ERROR_BAD_LENGTH:
            return QueryStatus::Malformed;
        default:
            break;
    }
    return QueryStatus::Unavailable;
}

// ---------------------------------------------------------------------------
// Один IOCTL_STORAGE_QUERY_PROPERTY
// ---------------------------------------------------------------------------

// Итог вызова: получилось ли, сколько байт драйвер принял (успешном или нет —
// на запрос размера ответ приходит именно с ошибкой), какой код и не остался ли
// буфер за драйвером.
struct IoctlOutcome {
    bool ok{false};
    std::size_t returned{};
    std::uint32_t win32Error{};
    bool bufferLeftToDriver{false};
};

// Выполнить запрос свойства под границей времени.
//
// overlapped == false — синхронный вызов на дескрипторе, открытом без
// FILE_FLAG_OVERLAPPED: таймаут в этом случае не действует, и зависание
// ограничивает только вызывающий (timeout <= 0 в публичной функции).
// noexcept: внутри нет ни одной операции, способной бросить, — все ошибки
// возвращаются статусом, а буфер живёт в ResponseBuffer.
IoctlOutcome sendPropertyQuery(HANDLE device, const STORAGE_PROPERTY_QUERY& query, ResponseBuffer& buffer,
                               std::uint32_t timeoutMs, bool overlapped) noexcept {
    IoctlOutcome outcome{};

    if (!overlapped) {
        // На дескрипторе без FILE_FLAG_OVERLAPPED структура OVERLAPPED не
        // передаётся вовсе: драйвер отвечает на месте, и метод ожидания — это
        // WaitForSingleObject на конкретном вызове, а не на конкретном буфере.
        DWORD rawReturned = 0;
        // const_cast: сигнатура DeviceIoControl объявляет входной буфер как
        // LPVOID, хотя запрос только читается. const_cast здесь — единственный
        // способ передать структуру, которая по смыслу неизменяема, и ничего
        // тут не пишется: буфер входа копируется ядром.
        if (::DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY,
                              const_cast<STORAGE_PROPERTY_QUERY*>(&query), sizeof query, buffer.data(),
                              static_cast<DWORD>(buffer.size()), &rawReturned, nullptr) == FALSE) {
            outcome.win32Error = ::GetLastError();
            // lpBytesReturned при BUFFER_TOO_SMALL содержит требуемый размер:
            // на этом держится весь запрос размера ниже.
            outcome.returned = rawReturned;
            return outcome;
        }
        outcome.ok = true;
        outcome.returned = rawReturned;
        return outcome;
    }

    ScopedHandle event = platform::adopt(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        outcome.win32Error = ::GetLastError();
        return outcome;
    }

    OVERLAPPED overlappedStruct{};
    overlappedStruct.hEvent = event.get();

    DWORD returned = 0;
    const BOOL started =
        ::DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, const_cast<STORAGE_PROPERTY_QUERY*>(&query),
                          sizeof query, buffer.data(), static_cast<DWORD>(buffer.size()), &returned,
                          &overlappedStruct);
    if (started == FALSE) {
        const std::uint32_t startedError = ::GetLastError();
        if (startedError != ERROR_IO_PENDING) {
            // Вызов завершился сразу, в том числе с ERROR_INSUFFICIENT_BUFFER на
            // запросе размера: возвращённое число байт в этом случае значимо.
            outcome.win32Error = startedError;
            outcome.returned = returned;
            return outcome;
        }
        const DWORD wait = ::WaitForSingleObject(event.get(), timeoutMs);
        if (wait == WAIT_TIMEOUT) {
            // Устройство не ответило за таймаут — FR-1: помечаем недоступным и
            // идём дальше, а не ждём вечно.
            ::CancelIoEx(device, &overlappedStruct);
            if (::WaitForSingleObject(event.get(), kCancelGraceMs) != WAIT_OBJECT_0) {
                outcome.bufferLeftToDriver = true;
                buffer.giveUpToDriver();
            }
            gAbandonedCalls.fetch_add(1, std::memory_order_relaxed);
            outcome.win32Error = static_cast<std::uint32_t>(WAIT_TIMEOUT);
            return outcome;
        }
        if (wait != WAIT_OBJECT_0) {
            outcome.win32Error = ::GetLastError();
            return outcome;
        }
    }

    // GetOverlappedResult нужен в обоих случаях: при синхронном завершении
    // событие не сигнализируется, а число принятых байт есть только здесь.
    if (::GetOverlappedResult(device, &overlappedStruct, &returned, FALSE) == FALSE) {
        outcome.win32Error = ::GetLastError();
        return outcome;
    }
    outcome.ok = true;
    outcome.returned = returned;
    return outcome;
}

STORAGE_PROPERTY_QUERY makeQuery(STORAGE_PROPERTY_ID propertyId) noexcept {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = propertyId;
    query.QueryType = PropertyStandardQuery;
    query.AdditionalParameters[0] = 0;
    return query;
}

// Есть ли у устройства это свойство.
//
// Ответ на exists-запрос — BOOL. Нужен, чтобы отличить «у диска нет такого
// свойства» (для UI — «драйвер не сообщает серийник») от «запрос упал»
// («диск недоступен»): это разные строки карточки и разные записи в логе.
// std::nullopt означает, что драйвер на такой запрос не ответил вовсе, и тогда
// решение принимает обычный запрос — так поступают драйверы без поддержки
// exists-запроса.
std::optional<bool> propertyExists(HANDLE device, STORAGE_PROPERTY_ID propertyId, std::uint32_t timeoutMs,
                                   bool overlapped) {
    STORAGE_PROPERTY_QUERY query = makeQuery(propertyId);
    query.QueryType = PropertyExistsQuery;

    ResponseBuffer buffer(sizeof(BOOL));
    const IoctlOutcome outcome = sendPropertyQuery(device, query, buffer, timeoutMs, overlapped);
    if (!outcome.ok || outcome.returned < sizeof(BOOL) || buffer.data() == nullptr) {
        return std::nullopt;
    }
    BOOL exists = FALSE;
    std::memcpy(&exists, buffer.data(), sizeof(BOOL));
    return exists != FALSE;
}

// ---------------------------------------------------------------------------
// Разбор ответа
// ---------------------------------------------------------------------------

// Строка известной длины по смещению. Границу задаёт вызывающий, а не поиск
// NUL: у идентификатора длина равна IdentifierSize, и без этого ограничения
// строка без завершающего NUL съела бы байты следующего идентификатора.
std::wstring stringOfLength(const std::byte* base, std::size_t offset, std::size_t chars) {
    std::wstring text;
    text.resize(chars);
    if (chars != 0) {
        std::memcpy(text.data(), base + offset, chars * sizeof(wchar_t));
    }
    return text;
}

// Строка по смещению от начала ответа.
//
// Смещение приходит от драйвера, поэтому границей служит только фактически
// принятое число байт: смещение за этой границей даёт пустую строку («драйвер
// не сообщил»), а не чтение за пределы буфера. Что строка кончается NUL —
// предположение, а не факт, поэтому поиск конца ограничен той же границей.
//
// Байты сравниваются по одному, а не через wchar_t: смещения ведут в блок
// RawDeviceProperties, то есть выровненный доступ к wchar_t там не гарантирован,
// а приведение указателя на невыровненный адрес — неопределённое поведение.
std::wstring stringAtOffset(const std::byte* base, std::size_t limit, std::uint32_t offset) {
    if (base == nullptr || offset == 0 || offset >= limit) {
        return {};
    }
    const std::size_t maxChars = (limit - offset) / sizeof(wchar_t);
    const std::byte* first = base + offset;
    std::size_t length = 0;
    while (length < maxChars) {
        const std::byte* cursor = first + length * sizeof(wchar_t);
        if (cursor[0] == std::byte{0} && cursor[1] == std::byte{0}) {
            break;
        }
        ++length;
    }
    return stringOfLength(base, offset, length);
}

// Драйвер дополняет строки пробелами до границы RawPropertiesLength, поэтому
// «SAMSUNG SSD 990 PRO   » — это та же модель с хвостом заполнения, а не другая
// модель. Без обрезки карточка диска (FR-2) показывала бы мусор, а отчёт
// (FR-8) — тем более.
std::wstring trimPadding(std::wstring text) {
    const auto isFill = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\0'; };
    std::size_t begin = 0;
    while (begin < text.size() && isFill(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && isFill(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// Модель для карточки диска: «Samsung SSD 990 PRO». Склейка не оставляет двойных
// пробелов, когда драйвер сообщил только одно из двух полей: пустое место — это
// «сообщено не всё», а не «модель называется Samsung».
std::wstring joinModel(const std::wstring& vendor, const std::wstring& product) {
    if (vendor.empty()) {
        return product;
    }
    if (product.empty()) {
        return vendor;
    }
    std::wstring model(vendor);
    model.push_back(L' ');
    model.append(product);
    return model;
}

DeviceDescriptor parseDescriptor(const STORAGE_DEVICE_DESCRIPTOR* descriptor, std::size_t returnedBytes) {
    DeviceDescriptor out;
    out.version = descriptor->Version;
    out.descriptorSize = descriptor->Size;
    out.rawPropertiesLength = descriptor->RawPropertiesLength;
    out.deviceType = descriptor->DeviceType;
    out.deviceTypeModifier = descriptor->DeviceTypeModifier;
    out.busType = static_cast<std::uint32_t>(descriptor->BusType);
    out.removableMedia = descriptor->RemovableMedia != 0;
    out.commandQueueing = descriptor->CommandQueueing != 0;

    // Граница — принятое число байт, а не Size из заголовка: у части драйверов
    // Size описывает только заголовок дескриптора, и строки, лежащие дальше,
    // молча пропали бы — модель и серийник стали бы пустыми у диска, который их
    // на самом деле сообщил. Само Size в ответе не проверяется: чтение всё
    // равно ограничено returned, то есть в безопасности; расхождение попадает в
    // результат и в лог, а не в данные.
    const std::byte* base = reinterpret_cast<const std::byte*>(descriptor);
    out.vendor = trimPadding(stringAtOffset(base, returnedBytes, descriptor->VendorIdOffset));
    out.product = trimPadding(stringAtOffset(base, returnedBytes, descriptor->ProductIdOffset));
    out.firmware = trimPadding(stringAtOffset(base, returnedBytes, descriptor->ProductRevisionOffset));
    out.serial = trimPadding(stringAtOffset(base, returnedBytes, descriptor->SerialNumberOffset));
    out.model = joinModel(out.vendor, out.product);
    return out;
}

DeviceId parseDescriptor(const STORAGE_DEVICE_ID_DESCRIPTOR* descriptor, std::size_t returnedBytes) {
    DeviceId out;
    out.version = descriptor->Version;
    out.descriptorSize = descriptor->Size;
    out.complete = false;

    const std::byte* base = reinterpret_cast<const std::byte*>(descriptor);
    // Курсор двигается по списку идентификаторов, поэтому объявлен без const:
    // на каждом шаге он сдвигается на длину текущего идентификатора.
    std::size_t cursor = offsetof(STORAGE_DEVICE_ID_DESCRIPTOR, Identifiers);
    // NumberOfIdentifiers — это DWORD, то есть unsigned long, и на MSVC это НЕ
    // тот же тип, что std::uint32_t: шаблон std::min не выводит общий тип и
    // отказывается. Приведение явное.
    const std::uint32_t reported = static_cast<std::uint32_t>(descriptor->NumberOfIdentifiers);
    out.reportedCount = reported;
    const std::uint32_t planned = std::min<std::uint32_t>(reported, kMaxIdentifiers);

    for (std::uint32_t index = 0; index < planned; ++index) {
        // Заголовок идентификатора должен целиком помещаться в ответ, иначе
        // читать нечего: усечённый ответ — это Malformed у вызывающего, а не
        // молчаливо неполный список.
        if (cursor > returnedBytes || returnedBytes - cursor < kIdentifierHeaderBytes) {
            break;
        }

        STORAGE_IDENTIFIER header{};
        std::memcpy(&header, base + cursor, kIdentifierHeaderBytes);

        const std::size_t payloadOffset = cursor + kIdentifierHeaderBytes;
        if (header.IdentifierSize == 0 || payloadOffset > returnedBytes ||
            static_cast<std::size_t>(header.IdentifierSize) > returnedBytes - payloadOffset) {
            break;
        }
        const std::byte* payload = base + payloadOffset;

        if (!out.hasEui64 && header.Type == StorageIdTypeEUI64 && header.CodeSet == StorageIdCodeSetBinary &&
            header.IdentifierSize == out.eui64.size()) {
            std::memcpy(out.eui64.data(), payload, out.eui64.size());
            out.hasEui64 = true;
        } else if (!out.hasText &&
                   (header.CodeSet == StorageIdCodeSetAscii || header.CodeSet == StorageIdCodeSetUtf8)) {
            // Идентификатор имеет собственную границу — IdentifierSize, а не
            // конец ответа: строка без NUL иначе забрала бы байты следующего
            // идентификатора. Нечётная длина отбрасывается делением: половины
            // wchar_t в ответе не бывает.
            out.text = trimPadding(stringOfLength(base, payloadOffset,
                                                 static_cast<std::size_t>(header.IdentifierSize) / sizeof(wchar_t)));
            out.textType = static_cast<std::uint32_t>(header.Type);
            out.textCodeSet = static_cast<std::uint32_t>(header.CodeSet);
            out.hasText = true;
        }
        ++out.parsedCount;

        // Следующий идентификатор: либо NextOffset из заголовка, либо шаг, который
        // вычисляется сам, когда NextOffset не задан. Нулевой шаг — конец обхода.
        // Шаг, уводящий курсор назад, отдельно не ловится: обход и так ограничен
        // planned, то есть не более kMaxIdentifiers итераций, и повторный разбор
        // того же идентификатора ничего не меняет (первый EUI-64 и первый текстовый
        // уже записаны).
        const std::size_t step = header.NextOffset != 0
                                     ? static_cast<std::size_t>(header.NextOffset)
                                     : kIdentifierHeaderBytes + static_cast<std::size_t>(header.IdentifierSize);
        if (step == 0) {
            break;
        }
        cursor += step;
    }

    // complete — «список разобран целиком». Расхождение с заявленным
    // NumberOfIdentifiers означает либо ложь драйвера, либо обрезанный ответ;
    // в обоих случаях результат помечается, чтобы неполный список не выглядел
    // полным.
    out.complete = out.parsedCount >= reported;
    return out;
}

// ---------------------------------------------------------------------------
// Чтение дескриптора
// ---------------------------------------------------------------------------

// Значение + состояние + код Win32. Значение — уже разобранное, то есть
// переносимый тип модуля: сырой дескриптор драйвера наружу не отдаётся, иначе
// STORAGE_PROPERTY_* утекли бы в заголовки слоя без windows.h.
template <typename T>
struct QuerySlot {
    T value{};
    QueryStatus status{QueryStatus::Unavailable};
    std::uint32_t win32Error{};
};

// Два вызова IOCTL: сначала размер, потом сам дескриптор. Так требует MSDN
// («вызовите с буфером нулевого размера, чтобы получить нужный размер»), и так
// же поступают все инструменты инвентаризации.
//
// Отдельный случай: драйвер, который на запрос размера отвечает ошибкой вместо
// числа (бывает и «успех» с нулём). Тогда идём с предельным буфером, а
// фактическую длину берём из числа принятых байт следующего вызова. Обе ветки
// честны — разбирается ровно столько, сколько драйвер вернул.
// Два параметра шаблона, а не один: Raw — тип ответа драйвера (для проверки
// минимальной длины и для разбора), Parsed — то, что уходит наружу. Одним
// параметром обойтись нельзя: разбор делают перегруженные parseDescriptor, а в
// теле шаблона вызов с зависимым аргументом иначе не разрешается.
template <typename Raw, typename Parsed>
QuerySlot<Parsed> readDescriptor(HANDLE device, STORAGE_PROPERTY_ID propertyId, std::uint32_t timeoutMs,
                                 bool overlapped) {
    static_assert(std::is_same_v<Parsed, DeviceDescriptor> || std::is_same_v<Parsed, DeviceId>,
                  "наружу отдаются только разобранные DeviceDescriptor и DeviceId");
    QuerySlot<Parsed> slot{};

    if (const std::optional<bool> exists = propertyExists(device, propertyId, timeoutMs, overlapped)) {
        if (!*exists) {
            slot.status = QueryStatus::Unsupported;
            slot.win32Error = static_cast<std::uint32_t>(ERROR_NOT_SUPPORTED);
            return slot;
        }
    }

    const STORAGE_PROPERTY_QUERY query = makeQuery(propertyId);

    ResponseBuffer sizeProbe;
    const IoctlOutcome probe = sendPropertyQuery(device, query, sizeProbe, timeoutMs, overlapped);

    std::size_t bytes = 0;
    if (probe.returned > 0 && probe.returned <= kMaxPropertyBytes) {
        bytes = probe.returned;
    } else if (probe.win32Error == ERROR_SUCCESS || probe.win32Error == ERROR_INSUFFICIENT_BUFFER) {
        bytes = kMaxPropertyBytes;
    } else {
        slot.status = probe.win32Error == static_cast<std::uint32_t>(WAIT_TIMEOUT) ? QueryStatus::TimedOut
                                                                                    : classify(probe.win32Error);
        slot.win32Error = probe.win32Error;
        return slot;
    }

    if (bytes < sizeof(Raw)) {
        // Ответ короче заголовка: разбирать нечего, а доверять таким байтам
        // нельзя — «модель» из них была бы мусором, который выглядел бы как
        // настоящее значение.
        slot.status = QueryStatus::Malformed;
        slot.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_DATA);
        return slot;
    }

    ResponseBuffer buffer(bytes);
    const IoctlOutcome read = sendPropertyQuery(device, query, buffer, timeoutMs, overlapped);
    if (!read.ok) {
        slot.status = read.win32Error == static_cast<std::uint32_t>(WAIT_TIMEOUT) ? QueryStatus::TimedOut
                                                                                  : classify(read.win32Error);
        slot.win32Error = read.win32Error;
        return slot;
    }
    if (read.returned < sizeof(Raw)) {
        slot.status = QueryStatus::Malformed;
        slot.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_DATA);
        return slot;
    }

    slot.value = parseDescriptor(reinterpret_cast<const Raw*>(buffer.data()), read.returned);
    slot.status = QueryStatus::Ok;
    return slot;
}

// ---------------------------------------------------------------------------
// Открытие устройства
// ---------------------------------------------------------------------------

struct OpenOutcome {
    ScopedHandle handle;
    QueryStatus status{QueryStatus::Unavailable};
    std::uint32_t win32Error{};
};

OpenOutcome openDevice(const std::wstring& path, bool overlapped) {
    OpenOutcome outcome;
    const DWORD flags = overlapped ? FILE_FLAG_OVERLAPPED : DWORD{0};
    for (const DWORD access : kAccessChain) {
        ScopedHandle device = platform::adopt(::CreateFileW(path.c_str(), access,
                                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                            OPEN_EXISTING, flags, nullptr));
        if (device.valid()) {
            outcome.handle = std::move(device);
            outcome.status = QueryStatus::Ok;
            return outcome;
        }
        // Последний код и есть тот, что показываем: цепочка прав — это
        // перебор вариантов, а не накопление ошибок.
        outcome.win32Error = ::GetLastError();
    }
    outcome.status = classify(outcome.win32Error);
    return outcome;
}

// ---------------------------------------------------------------------------
// Лог
// ---------------------------------------------------------------------------

std::chrono::milliseconds elapsedSince(std::chrono::steady_clock::time_point started) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
}

// Поля собираются явно, а не макросом MRP_LOG_*: набор разнородный (строки и
// числа), а разворачивание пакета в core::detail::logFieldList на MSVC спотыкается
// на перегрузках logField (C2661) — проверено сборкой на devices.cpp.
core::LogFields queryFields(const StoragePropertiesResult& result, std::chrono::milliseconds timeout) {
    core::LogFields fields;
    fields.push_back(core::logField("status", toString(result.status)));
    fields.push_back(core::logField("elapsedMs", static_cast<long long>(result.elapsed.count())));
    fields.push_back(core::logField("timeoutMs", static_cast<long long>(timeout.count())));
    return fields;
}

// Отказ — в лог с путём и кодом Win32 (SPEC §12: «все ошибки в логе с путём и
// HRESULT»). Не noexcept по одной причине: сборка полей выделяет память, и
// вызывающий ловит это отдельно — логирование не имеет права ронять опрос.
void logUnavailable(const StoragePropertiesResult& result, const std::wstring& path,
                    std::chrono::milliseconds timeout) {
    core::LogFields fields = queryFields(result, timeout);
    fields.push_back(core::logField("propertyId", "StorageDeviceProperty"));
    core::logFailure("platform.storage_query.properties",
                     "свойства устройства не получены: диск помечен недоступным", core::toUtf8(path),
                     static_cast<std::int64_t>(result.win32Error), std::move(fields));
}

// Успех — на уровне Debug, чтобы обычный прогон не писал в лог по записи на
// каждый диск. Серийник в лог не попадает: лог — это ровно тот файл, который
// потом прикладывают к баг-репорту, а SPEC §5 и ADR-007 требуют, чтобы серийники
// можно было исключить перед отправкой. Признак «сообщён» — можно.
void logCollected(const StoragePropertiesResult& result, const std::wstring& path, std::chrono::milliseconds timeout) {
    core::LogFields fields = queryFields(result, timeout);
    fields.push_back(core::logField("model", result.device.model));
    fields.push_back(core::logField("busType", static_cast<long long>(result.device.busType)));
    fields.push_back(core::logField("busTypeKnown", result.device.busTypeKnown()));
    fields.push_back(core::logField("removableMedia", result.device.removableMedia));
    fields.push_back(core::logField("serialKnown", !result.device.serial.empty()));
    fields.push_back(core::logField("firmwareKnown", !result.device.firmware.empty()));
    // Size из заголовка и фактически принятое число байт кладутся рядом: у части
    // драйверов они расходятся, и по логу это видно сразу, а по карточке — нет.
    fields.push_back(core::logField("descriptorSize", static_cast<long long>(result.device.descriptorSize)));
    fields.push_back(core::logField("idPresent", result.id.hasEui64 || result.id.hasText));
    fields.push_back(core::logField("devicePath", core::toUtf8(path)));
    core::logDebug("platform.storage_query.properties", "свойства устройства получены", std::move(fields));
}

}  // namespace

const wchar_t* toString(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::Ok:
            return L"ok";
        case QueryStatus::TimedOut:
            return L"timeout";
        case QueryStatus::AccessDenied:
            return L"access-denied";
        case QueryStatus::NotFound:
            return L"not-found";
        case QueryStatus::Unsupported:
            return L"unsupported";
        case QueryStatus::Malformed:
            return L"malformed";
        case QueryStatus::InvalidArgument:
            return L"invalid-argument";
        case QueryStatus::Unavailable:
            break;
    }
    return L"unavailable";
}

TimeoutStats storageQueryTimeoutStats() noexcept {
    TimeoutStats stats{};
    stats.abandonedCalls = gAbandonedCalls.load(std::memory_order_relaxed);
    return stats;
}

// UTF-8-вид строк дескриптора. Определения здесь, а не в заголовке, по одной
// причине: перевод живёт в win_error.hpp, а тот по закону ADR-004 включает
// windows.h — и заголовок storage_query.hpp обязан оставаться включаемым туда,
// где windows.h нет (ADR-004, devices.hpp).
std::string DeviceDescriptor::modelUtf8() const { return platform::toUtf8(model); }
std::string DeviceDescriptor::serialUtf8() const { return platform::toUtf8(serial); }
std::string DeviceDescriptor::firmwareUtf8() const { return platform::toUtf8(firmware); }
std::string DeviceId::textUtf8() const { return platform::toUtf8(text); }

StoragePropertiesResult queryStorageProperties(std::wstring_view devicePath,
                                               std::chrono::milliseconds timeout) noexcept {
    StoragePropertiesResult result{};
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::wstring path;

    try {
        // Копия пути нужна и потоку запроса, и логу: string_view вызывающего к
        // моменту логирования уже недействителен.
        path.assign(devicePath);

        if (path.empty()) {
            result.status = QueryStatus::InvalidArgument;
            result.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER);
        } else {
            // timeout <= 0 — явный отказ от ожидания: запрос идёт в потоке
            // вызывающего, синхронно и без FILE_FLAG_OVERLAPPED. Ограничивать
            // зависание такого вызова обязан вызывающий (рабочий поток пула).
            const bool overlapped = timeout > std::chrono::milliseconds::zero();
            const auto timeoutMs = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(timeout.count(), 0, static_cast<std::int64_t>(MAXDWORD)));

            const OpenOutcome opened = openDevice(path, overlapped);
            result.status = opened.status;
            result.win32Error = opened.win32Error;

            if (opened.status == QueryStatus::Ok) {
                const QuerySlot<DeviceDescriptor> device =
                    readDescriptor<STORAGE_DEVICE_DESCRIPTOR, DeviceDescriptor>(opened.handle.get(),
                                                                                StorageDeviceProperty, timeoutMs,
                                                                                overlapped);
                result.device = device.value;
                result.status = device.status;
                result.win32Error = device.win32Error;

                // Идентификатор — запасной источник серийника, поэтому он и
                // читается, когда основной дескриптор не пришёл: у части
                // контроллеров STORAGE_DEVICE_DESCRIPTOR без серийника, а
                // EUI-64 приходит именно отсюда. Но статус результата
                // остаётся по основному дескриптору — идентификатор не
                // заменяет модель, прошивку и признак «съёмный», и объявлять
                // успех по нему было бы приукрашиванием.
                //
                // На зависшем устройстве (TimedOut) второй запрос не делается:
                // он стоил бы ещё одного таймаута на диск, который и так уже
                // помечен недоступным.
                if (device.status == QueryStatus::Ok || device.status == QueryStatus::Unsupported ||
                    device.status == QueryStatus::Malformed) {
                    const QuerySlot<DeviceId> id =
                        readDescriptor<STORAGE_DEVICE_ID_DESCRIPTOR, DeviceId>(opened.handle.get(),
                                                                               StorageDeviceIdProperty, timeoutMs,
                                                                               overlapped);
                    result.id = id.value;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        // Единственное, что может вылететь, — нехватка памяти на копии пути и
        // на строках ответа. Наружу не пускаем: FR-1 требует, чтобы приложение
        // не падало.
        result = StoragePropertiesResult{};
        result.status = QueryStatus::Unavailable;
        result.win32Error = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
    }

    result.elapsed = elapsedSince(started);

    try {
        if (result.ok()) {
            logCollected(result, path, timeout);
        } else {
            logUnavailable(result, path, timeout);
        }
    } catch (...) {
        // Сбой записи в лог (нехватка памяти) — не повод потерять результат
        // опроса и не повод уронить процесс.
    }
    return result;
}

StoragePropertiesResult queryStorageProperties(int diskNumber, std::chrono::milliseconds timeout) noexcept {
    if (diskNumber < 0) {
        StoragePropertiesResult result{};
        result.status = QueryStatus::InvalidArgument;
        result.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER);
        result.elapsed = std::chrono::milliseconds::zero();
        try {
            core::logFailure("platform.storage_query.properties",
                             "свойства устройства не запрошены: отрицательный номер диска", "physicalDrive",
                             static_cast<std::int64_t>(ERROR_INVALID_PARAMETER), queryFields(result, timeout));
        } catch (...) {
            // Логирование не имеет права ронять вызывающего: нехватка памяти при
            // сборке полей — единственное, что тут может вылететь.
        }
        return result;
    }
    // Путь собирается здесь, в одном месте: и открывается он здесь же, и в лог
    // попадает отсюда, поэтому номер диска не может превратиться в «\\.\PhysicalDrive-1».
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskNumber);
    return queryStorageProperties(std::wstring_view(path), timeout);
}

}  // namespace mrproper::platform::storage_query
