// Свойства физического диска: модель, серийник, прошивка, признак «съёмный».
// Спека: §4 FR-1 п.2 — «Модель, серийник, прошивка, тип шины:
// IOCTL_STORAGE_QUERY_PROPERTY со свойствами StorageDeviceProperty,
// StorageDeviceIdProperty, StorageAdapterProperty (STORAGE_ADAPTER_DESCRIPTOR →
// BusType: NVMe=0x11, SATA/ATA, SCSI, USB)».
//
// Модуль закрывает первые два свойства из этого перечня:
//   * StorageDeviceProperty → STORAGE_DEVICE_DESCRIPTOR: VendorId, ProductId,
//     SerialNumber, ProductRevision, RemovableMedia, CommandQueueing и BusType
//     «как есть»;
//   * StorageDeviceIdProperty → STORAGE_DEVICE_ID_DESCRIPTOR: идентификаторы
//     устройства, в том числе EUI-64. Это запасной источник идентификатора:
//     у части USB/ SCSI-контроллеров SerialNumber пуст, а EUI-64 приходит
//     именно отсюда, и серийник в карточке диска (FR-2) тогда не пустой.
//
// Чего модуль сознательно НЕ делает (соседние задачи, тот же FR-1 п.2):
//   * BusType не превращается в core::model::BusType: правило «NVMe=0x11,
//     SATA/ATA, SCSI, USB» живёт в platform::bus_type (STORAGE_ADAPTER_DESCRIPTOR),
//     а этот модуль отдаёт число как есть;
//   * TRIM и кэш записи не читаются: это platform::trim_cache
//     (DEVICE_TRIM_DESCRIPTOR, DEVICE_WRITE_CACHE_DESCRIPTOR);
//   * диски не перечисляются — это platform::devices (FR-1 п.1);
//   * инвентарь не собирается и core::model::PhysicalDisk не заполняется: модель
//     складывает DiskInventory, этот модуль даёт ему сырые строки и флаги;
//   * «здоровье» носителя не строится: SMART/NVMe в MVP не входит (§13.1, v1.1),
//     и модуль не строит под неё задел — придумывать интерфейс, который никто
//     не вызывает, значит зафиксировать его в заголовке навсегда.
//
// Файл не включает windows.h — намеренно, как devices.hpp: объявления
// используют только переносимые типы, поэтому включить этот заголовок может и
// слой без Win32 (тесты, отчёт, CLI), а единица трансляции, которой windows.h
// нужен по существу, добавляет его сама. Так граница слоёв не расплывается по
// одному заголовку (ADR-004).
//
// Свой QueryStatus вместо ProbeStatus из size_probe — тоже осознанно: соседние
// модули платформы пишутся параллельно, и size_probe сам запрещает зависеть от
// чужих заголовков. Дублируется не механизм (RAII дескрипторов и текст ошибок
// берутся из win_handle.hpp и win_error.hpp), а только перечень состояний
// ответа: он у каждого IOCTL свой — здесь ещё есть «ответ получен, но разбирать
// его нельзя» (Malformed), чего у запроса размера не бывает.
//
// Честность строк важнее их полноты. Ответ драйвера — это блоб с целочисленными
// смещениями, и доверять им нельзя: смещение вне ответа — чтение за границей
// буфера, а строка без завершающего NUL — обрезанная мусором. Поэтому каждая
// строка читается в границах фактически полученного ответа, а пустая строка в
// результате означает одно и то же: «драйвер не сообщил значение», а не «значение
// пустое». Отчёт и UI (FR-2, FR-8) различают эти случаи по пустой строке и не
// придумывают за отсутствующим серийником заглушку вида «Unknown disk».
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mrproper::platform::storage_query {

// Таймаут на одно устройство (SPEC §4 FR-1: «устойчивость к "отказавшим"
// дискам (таймаут 2 с на устройство, устройство помечается недоступным,
// приложение не падает)»). Таймаут на ожидание ответа, а не на CreateFileW:
// открытие синхронно и изнутри модуля не ограничивается (см. комментарий у
// openDevice в storage_query.cpp).
inline constexpr std::chrono::milliseconds kDefaultDeviceTimeout{2000};

// Итог одной попытки. Ok — единственное состояние, при котором данные можно
// класть в модель; всё остальное по FR-1 означает «устройство помечено
// недоступным» и должно быть видно в логе с кодом Win32.
enum class QueryStatus : std::uint8_t {
    Ok,              // свойства получены
    TimedOut,        // устройство не ответило за таймаут
    AccessDenied,    // нет прав на дескриптор (обычно — процесс не повышен)
    NotFound,        // устройства нет: диск отключён или уехал
    Unsupported,     // драйвер не отдаёт это свойство
    Malformed,       // ответ получен, но разобрать его нельзя (обрезан, смещение вне ответа)
    InvalidArgument, // пустой путь или отрицательный номер диска
    Unavailable,     // прочие ошибки Win32
};

// Стабильное нелокализуемое имя состояния: им заполняются JSON-дамп дисков
// (FR-8) и лог. Текст системного сообщения по коду — общий для слоя
// platform::win32ErrorText(win32Error), поэтому второй функции «описать ошибку»
// здесь нет: собирать её заново значило бы завести вторую копию разбора
// кодов ошибок рядом с win_error.hpp.
const wchar_t* toString(QueryStatus status) noexcept;

// Разобранный STORAGE_DEVICE_DESCRIPTOR — ровно то, что прислал драйвер, плюс
// готовые строки, которые нужны карточке диска.
//
// Смещения (VendorIdOffset и соседние) наружу не отдаются: это деталь разбора
// ответа, а не свойство диска. В лог они уходят как есть при разборе.
struct DeviceDescriptor {
    std::wstring vendor;     // VendorId: «Samsung», «ATA»
    std::wstring product;    // ProductId: «SSD 990 PRO»
    std::wstring model;      // vendor + product с одним пробелом — то, что видно в FR-2
    std::wstring serial;     // SerialNumber
    std::wstring firmware;   // ProductRevision

    std::uint32_t deviceType{};           // STORAGE_DEVICE_TYPE «как есть» (SCSI-2)
    std::uint32_t deviceTypeModifier{};   // модификатор SCSI-2, обычно 0
    std::uint32_t busType{};              // STORAGE_BUS_TYPE «как есть»: BusTypeNVMe = 0x11 и т. д.
    bool removableMedia{};                // RemovableMedia: признак «съёмный» от драйвера
    bool commandQueueing{};               // CommandQueueing: не путать с «очередь команд включена»
    std::uint32_t version{};              // Version из заголовка
    std::uint32_t descriptorSize{};       // Size из заголовка: сколько байт в ответе по мнению драйвера
    std::uint32_t rawPropertiesLength{};  // RawPropertiesLength: длина блока строк

    // BusType == BusTypeUnknown (0x00) — драйвер не определил шину. Это не то же
    // самое, что «шина известна и равна Unknown», поэтому проверка отдельная:
    // карточке диска (FR-2) полезно сказать «интерфейс не определён», а не
    // молча показать пустое поле.
    [[nodiscard]] bool busTypeKnown() const noexcept { return busType != 0; }

    // UTF-8-вид тех же строк. Модель, отчёт и лог работают в UTF-8
    // (core::model, core::log), Win32 — в UTF-16 (SPEC §6.3), и слой держит оба
    // вида для каждой строки: так сделано и для путей (physicalDrivePath и
    // physicalDrivePathUtf8 в devices.hpp). Оба вида берутся из одного разбора,
    // а перевод делает platform::toUtf8 внутри модуля — заголовок остаётся без
    // windows.h (ADR-004), поэтому объявления, а не определения.
    //
    // Некорректная UTF-16-последовательность даёт пустую строку: в ответе
    // драйвера такого текста не бывает, а для вызывающего пустая строка означает
    // то же, что и раньше, — «значение не получено». Проверять tryToUtf8 в
    // вызывающем не нужно.
    [[nodiscard]] std::string modelUtf8() const;
    [[nodiscard]] std::string serialUtf8() const;
    [[nodiscard]] std::string firmwareUtf8() const;
};

// Разобранный STORAGE_DEVICE_ID_DESCRIPTOR.
//
// Ответ — это список идентификаторов, и в списке встречаются и двоичные
// (EUI-64), и текстовые (ASCII/UTF-8) записи. Наружу отдаются по одному
// представителю каждого вида: карточке диска нужен серийник, а не полный
// список. Сколько всего идентификаторов было в ответе и сколько удалось
// разобрать — тоже: по разнице видно, что обход упёрся в границу буфера, и это
// лучше, чем молчаливый неполный список.
struct DeviceId {
    std::wstring text;                   // первый текстовый (Ascii/Utf8) идентификатор
    std::array<std::uint8_t, 8> eui64{}; // байты первого EUI-64, в порядке ответа
    std::uint32_t textType{};            // STORAGE_IDENTIFIER_TYPE текстового, «как есть»
    std::uint32_t textCodeSet{};         // STORAGE_IDENTIFIER_CODE_SET текстового, «как есть»
    std::uint32_t version{};             // Version из заголовка
    std::uint32_t descriptorSize{};      // Size из заголовка
    std::uint32_t reportedCount{};       // NumberOfIdentifiers: сколько заявил драйвер
    std::uint32_t parsedCount{};         // сколько удалось разобрать без нарушения границ
    bool hasText{};
    bool hasEui64{};
    // Обход дошёл до конца списка. false означает, что заявленный драйвером
    // NumberOfIdentifiers не совпал с тем, что поместилось в ответ: либо
    // драйвер соврал, либо ответ обрезан. Разбираем сколько есть, но результат
    // помечаем — иначе «полный» список окажется половиной молча.
    bool complete{true};

    // UTF-8-вид текстового идентификатора — по той же причине, что и у
    // DeviceDescriptor: модель и отчёт живут в UTF-8, перевод внутри модуля.
    [[nodiscard]] std::string textUtf8() const;
};

struct StoragePropertiesResult {
    DeviceDescriptor device{};
    DeviceId id{};
    QueryStatus status{QueryStatus::Unavailable};
    std::uint32_t win32Error{};             // код Win32; при TimedOut — ERROR_TIMEOUT
    std::chrono::milliseconds elapsed{};    // сколько ждали результат

    [[nodiscard]] bool ok() const noexcept { return status == QueryStatus::Ok; }
};

// Счётчики таймаутов — только диагностика (SPEC §12: в отчёте видно, сколько
// устройств не ответило). На поведение запросов не влияет.
struct TimeoutStats {
    // Вызовы, не уложившиеся в таймаут. Часть из них оставила буфер ответа
    // у драйвера: освобождать память, в которую тот может писать после
    // отмены, нельзя, и такая утечка — плата за «приложение не падает»
    // (см. комментарий у ResponseBuffer в storage_query.cpp).
    std::uint32_t abandonedCalls{};
};

[[nodiscard]] TimeoutStats storageQueryTimeoutStats() noexcept;

// Опрашивающие функции noexcept: нехватка памяти и любая ошибка Win32
// возвращаются статусом, а не исключением (SPEC §4 FR-1: «приложение не
// падает», §5: «Ни один отказ IOCTL/устройства/файла не роняет процесс»).
//
// timeout <= 0 означает явный отказ от ожидания: вызов выполняется в потоке
// вызывающего (синхронно, без FILE_FLAG_OVERLAPPED) и зависание устройства ничем
// не ограничено. Это тот же контракт, что у size_probe: вызывающий сам решает,
// в каком потоке живёт риск зависания (SPEC §6.4 — рабочий поток пула).

// Свойства по номеру диска: сам собирает «\\.\PhysicalDriveN».
[[nodiscard]] StoragePropertiesResult queryStorageProperties(int diskNumber,
                                                             std::chrono::milliseconds timeout =
                                                                 kDefaultDeviceTimeout) noexcept;

// Свойства по готовому device path: «\\.\PhysicalDriveN», либо путь вида
// «\\?\X#&…\», который отдаёт platform::devices (FR-1 п.1).
[[nodiscard]] StoragePropertiesResult queryStorageProperties(std::wstring_view devicePath,
                                                             std::chrono::milliseconds timeout =
                                                                 kDefaultDeviceTimeout) noexcept;

}  // namespace mrproper::platform::storage_query
