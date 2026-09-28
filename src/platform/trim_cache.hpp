// Свойства хранилища для карточки диска: TRIM, кэш записи и предел числа
// одновременных IO-операций. Это три значения из одной строки §4 FR-1 п.2 —
// `StorageDeviceTrimProperty`, `StorageDeviceWriteCacheProperty`,
// `StorageDeviceIoCapabilityProperty` — плюс `StorageDeviceLBProvisioningProperty`,
// который в наборе тоже есть, но в этот модуль не входит (см. границы ниже).
//
// Почему именно эти три. SPEC §13.1 оставляет в MVP «только TRIM/кэш записи в
// карточке диска», а §2 в не-цели выносит всё остальное здоровья носителя:
// SMART, анализ поверхности, дефрагментацию. §8 Этап 6 (v1.1) добавляет к этим
// свойствам SMART/NVMe-здоровье и температуру — то есть модуль рассчитан на
// то, что его читают и дальше, а не выбрасывают через версию.
//
// Что делает модуль. Один открытый дескриптор устройства, три запроса
// IOCTL_STORAGE_QUERY_PROPERTY, на выходе — по одному `PropertyResult` на
// свойство плюс сводный `StorageFlagsResult`. Каждый запрос ограничен таймаутом
// (FR-1: «таймаут 2 с на устройство, устройство помечается недоступным,
// приложение не падает»): дескриптор открыт с FILE_FLAG_OVERLAPPED, запрос ждёт
// своё событие и по истечении срока отменяется через CancelIoEx.
//
// Чего модуль НЕ делает и кто делает это вместо него.
//
//   * остальные свойства той же строки FR-1 п.2 — `StorageDeviceProperty`
//     (модель, серийник, прошивка, MediaRemovable), `StorageDeviceIdProperty`,
//     `StorageAdapterProperty` (тип шины), тонкое выделение
//     (`StorageDeviceLBProvisioningProperty`): это соседние модули того же
//     обхода. Здесь только те три свойства, что перечислены выше, и смешивать
//     их в один дескриптор значило бы получить два разных ответа на один вопрос
//     при правке любого из них;
//   * модель `core::PhysicalDisk` (§6.3, поле `trimSupported`): её собирает
//     DiskInventory. Модуль ничего не знает про модель и не пишет в неё —
//     `TrimInfo::trimEnabled` переводится в поле вызывающим;
//   * третье поле ответа IOCTL_STORAGE_GET_DEVICE_NUMBER (bits
//     RemovableMedia/ReadOnlyMedia/WriteCacheEnabled, которые приносит
//     `platform::devices::RawDeviceNumber`) — это про «съёмный/только чтение»,
//     а не про TRIM, и разбирает его слой, который строит PhysicalDisk;
//   * перечисление дисков (FR-1 п.1), размер (FR-1 п.3), разметка (FR-1 п.4),
//     тома (FR-1 п.5-6) — `platform::devices` / `layout` / `size_probe` /
//     `volumes`;
//   * кэш картины и реакция на WM_DEVICECHANGE (требование к результату FR-1) —
//     DiskInventory;
//   * SMART, NVMe log page 0x02, температура — v1.1 (SPEC §8 Этап 6).
//
// Модуль самодостаточен: он не включает заголовки соседних модулей src/platform
// (все они пишутся параллельно) и опирается только на общие для слоя
// win_handle.hpp (ADR-001, RAII) и win_error.hpp. Поэтому `physicalDrivePath`
// здесь свой, в собственном пространстве имён: тот же путь собирает и
// platform::size_probe, но общий помощник появится только когда сойдутся волны,
// а ждать этого ради одного вызываемого нельзя.
//
// Заголовок намеренно не включает windows.h: все типы переносимые, поэтому его
// можно включить оттуда, где Win32 нет (тесты, дамп дисков, CLI), а слой Win32
// добавляет <windows.h> и <winioctl.h> сам — так граница слоёв не расплывается
// по одному заголовку (то же решение, что в devices.hpp).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mrproper::platform::trim_cache {

// Таймаут на одно свойство. Ровно тот, что требует FR-1 («2 с на устройство»)
// и который применяет platform::devices: свойства читаются в том же проходе
// инвентаризации, и другое число здесь означало бы, что карточка диска «не
// ответила» там, где номер диска уже получен.
inline constexpr std::chrono::milliseconds kDefaultPropertyTimeout{2000};

// Сколько ждать после CancelIoEx, прежде чем признать отмену неудачной. Не ноль:
// пока драйвер не освободил буфер ответа, уничтожать его нельзя.
inline constexpr std::chrono::milliseconds kCancelGrace{1000};

// Верхняя граница дескриптора, в которую верим. Три наших структуры — 12, 28 и
// 16 байт; kilobyт с запасом — предохранитель от драйвера, который в заголовке
// ответа сообщил неправдоподобный Size (FR-1: обход не должен уйти в аллокацию
// на мусор).
inline constexpr std::uint32_t kMaxDescriptorBytes = 4096;

// ---------------------------------------------------------------------------
// Состояние опроса
// ---------------------------------------------------------------------------

// Итог запроса одного свойства. Ok — единственное состояние, при котором
// разобранные значения можно класть в модель; всё остальное означает «устройство
// или драйвер не дали данных» и обязано быть видно вызывающему, а не молчать
// (FR-1: «устройство помечается недоступным, приложение не падает»).
enum class QueryStatus : std::uint8_t {
    Ok,            // свойство прочитано и разобрано
    Unsupported,   // драйвер свойства не знает: ERROR_NOT_SUPPORTED и подобные
    NotFound,      // устройства нет: уехало, отключено, неверный номер
    AccessDenied,  // не хватило прав на дескриптор — помогает только повышение
    TimedOut,      // устройство не ответило за таймаут
    DeviceRemoved, // устройство исчезло прямо во время запроса
    InvalidAnswer, // ответ есть, но разбирать его нельзя: обрыв или чужой формат
    // Пустой путь или отрицательный номер диска. Отдельное состояние, а не
    // NotFound: вызов перепутал номер диска, и это ошибка вызывающего, а не
    // «диска нет».
    InvalidArgument,
    Unavailable,   // прочие отказы Win32 и нехватка памяти
};

// Стабильное имя состояния: не локализуется, идёт в лог и в JSON-дамп дисков
// (SPEC §8 Этап 4 — golden-тесты сравнивают дампы, поэтому перевод здесь был бы
// источником расхождений).
const char* toString(QueryStatus status) noexcept;

// То же имя в UTF-16 — для UI и текстового дампа, где строки широкие (§6.3).
const wchar_t* toWideString(QueryStatus status) noexcept;

// Причина одним предложением: имя состояния плюс текст системы по коду Win32.
// Пустая строка только при Ok. Широкая форма — для UI и отчёта, узкая (UTF-8) —
// для лога; текст берётся у самой системы, поэтому соответствует языку
// установки, а не захардкоженному словарю (SPEC §5, §12).
[[nodiscard]] std::wstring formatStatusWide(QueryStatus status, std::uint32_t win32Error);
[[nodiscard]] std::string formatStatus(QueryStatus status, std::uint32_t win32Error);

// ---------------------------------------------------------------------------
// Три состояния и тип кэша
// ---------------------------------------------------------------------------

// Три состояния вместо bool — потому что в дескрипторах хранилища третьим
// значением BOOLEAN-поля почти всегда 0xFF («драйвер не знает»), и молча
// превращать его в false значило бы показывать пользователю «TRIM выключен» там,
// где на самом деле «драйвер не сказал». core::model для bool-полей оставляет
// только два состояния, поэтому решение «показывать или не показывать» принимает
// вызывающий через toBool().
enum class Tristate : std::uint8_t {
    Unknown, // драйвер не сообщил (0xFF) либо свойство не прочитано
    No,
    Yes,
};

const char* toString(Tristate value) noexcept;

// 0 → No, 1 → Yes, всё остальное (0xFF и любой мусор) → Unknown. Правило одно на
// все BOOLEAN-поля этих дескрипторов.
[[nodiscard]] Tristate toTristate(std::uint8_t raw) noexcept;

// Значение для bool-поля модели: Unknown превращается в заданный запасной вариант
// (обычно false — «не показываем поле», а не «показываем ложь»).
[[nodiscard]] bool toBool(Tristate value, bool unknownValue = false) noexcept;

// Режим кэша записи по STORAGE_WRITE_CACHE_PROPERTY::WriteCacheType.
enum class WriteCacheType : std::uint8_t {
    Unknown,     // WriteCacheTypeUnknown или непрочитанное значение
    None,        // кэша на устройстве нет
    WriteBack,   // кэш с обратной записью (WriteCacheTypeWriteBack)
    WriteThrough,// только сквозная запись (WriteCacheTypeWriteThrough)
};

const char* toString(WriteCacheType value) noexcept;

// Драйвер может прислать значение вне перечисления — это Unknown, а не «как
// получилось»: в карточке диска такой тип читается как «режим неизвестен».
[[nodiscard]] WriteCacheType toWriteCacheType(std::uint32_t raw) noexcept;

// ---------------------------------------------------------------------------
// Разобранные свойства
// ---------------------------------------------------------------------------

// TRIM: знает ли устройство о свободных блоках, которые можно не читать.
// Источник — StorageDeviceTrimProperty, ответ DEVICE_TRIM_DESCRIPTOR.
//
// Про историю с двумя раскладками этого свойства — чтобы её не «починили»
// позже. В WDK 8 ответом на StorageDeviceTrimProperty была структура
// STORAGE_DEVICE_TRIM_PROPERTY (TrimEnabled, RetrimNeeded, TrimGranularity, без
// заголовка), в современном Windows — DEVICE_TRIM_DESCRIPTOR с
// STORAGE_DESCRIPTOR_HEADER и одним полем TrimEnabled. Установленный здесь SDK
// (10.0.19041.0) не содержит STORAGE_DEVICE_TRIM_PROPERTY вообще, и документирует
// ровно одну раскладку. Угадывать вторую по догадке нельзя: 8 байт ответа
// выглядели бы правдоподобно, а RetrimNeeded и Granularity оказались бы мусором
// из булевых полей. Поэтому читаем только документированный дескриптор, а
// несовпадение объявляем InvalidAnswer.
struct TrimInfo {
    Tristate trimEnabled{Tristate::Unknown}; // TrimEnabled: включён ли TRIM
    std::uint8_t rawTrimEnabled{0xFF};      // байт как пришёл (0xFF = «не знаю»)
    std::uint32_t version{};                // STORAGE_DESCRIPTOR_HEADER::Version
    std::uint32_t descriptorSize{};         // STORAGE_DESCRIPTOR_HEADER::Size
    std::uint32_t returnedBytes{};          // сколько байт драйвер записал
};

// Кэш записи: как устройство пишет, есть ли энергонезависимый кэш и можно ли его
// переключать. Источник — StorageDeviceWriteCacheProperty, ответ
// STORAGE_WRITE_CACHE_PROPERTY.
//
// Зачем столько полей одному флажку «кэш записи». В карточке диска (§7.1)
// пользователю важно не «есть кэш», а риск потери данных при питании: включённый
// WriteBack без батареи — это «данные в кэше потеряются при отключении питания»,
// и это же объяснение должно попасть в отчёт. Поэтому разбираются все поля, а
// не первое подходящее.
struct WriteCacheInfo {
    WriteCacheType type{WriteCacheType::Unknown}; // WriteCacheType
    std::uint32_t rawType{};                     // значение перечисления как есть
    Tristate enabled{Tristate::Unknown};         // WriteCacheEnabled
    Tristate changeable{Tristate::Unknown};      // WriteCacheChangeable
    Tristate writeThroughSupported{Tristate::Unknown}; // WriteThroughSupported
    Tristate flushCacheSupported{Tristate::Unknown};    // FlushCacheSupported
    Tristate powerProtection{Tristate::Unknown};        // UserDefinedPowerProtection
    Tristate batteryBacked{Tristate::Unknown};          // NVCacheEnabled
    std::uint32_t version{};
    std::uint32_t descriptorSize{};
    std::uint32_t returnedBytes{};

    // Энергонезависимый кэш с обратной записью включён: единственное состояние,
    // при котором пропажа питания теряет уже записанные данные. Именно его
    // показывает предупреждение в карточке диска.
    [[nodiscard]] bool volatileWriteCache() const noexcept {
        return type == WriteCacheType::WriteBack && enabled == Tristate::Yes;
    }
};

// Предел числа незавершённых IO-операций. Источник —
// StorageDeviceIoCapabilityProperty, ответ STORAGE_DEVICE_IO_CAPABILITY_DESCRIPTOR
// (LunMaxIoCount / AdapterMaxIoCount).
//
// Замечание об имени. Свойство называется IoCapability, а дескриптор в SDK
// описывает именно предел числа одновременных операций; никаких «возможностей
// LBA с метаданными» в этом ответе нет (такие структуры относятся к другим,
// WDK-специфичным запросам). Поэтому модуль называет поля так, как они названы в
// ответе, и не обещает ничего сверх этого.
struct IoCapabilityInfo {
    std::uint32_t lunMaxIoCount{};     // предел для LUN
    std::uint32_t adapterMaxIoCount{}; // предел для адаптера
    std::uint32_t version{};
    std::uint32_t descriptorSize{};
    std::uint32_t returnedBytes{};

    // Практический предел устройства: из двух чисел большее, а при обоих нулях
    // догадки не делаем — вызывающий трактует нули как «драйвер не сообщил».
    [[nodiscard]] std::uint32_t maxOutstandingIo() const noexcept {
        return lunMaxIoCount > adapterMaxIoCount ? lunMaxIoCount : adapterMaxIoCount;
    }
};

// ---------------------------------------------------------------------------
// Результат одного свойства
// ---------------------------------------------------------------------------

// Шаблон, а не три одинаковых структуры: форма результата у всех трёх свойств
// одна и та же, а различается только содержимое info.
template <class Info>
struct PropertyResult {
    Info info{};
    QueryStatus status{QueryStatus::Unavailable};
    std::uint32_t win32Error{};                       // код Win32; 0 — не Win32
    std::chrono::milliseconds elapsed{};              // сколько занял запрос

    // Данные получены и разобраны: только это состояние позволяет класть info
    // в модель. В остальных info остаётся заполненным Unknown, и это честнее
    // подставных значений.
    [[nodiscard]] bool ok() const noexcept { return status == QueryStatus::Ok; }
};

// Права доступа, под которыми открывается устройство. Читаются они как цепочка
// попыток от требовательных к минимальным (сборка цепочки — в .cpp): каждая
// следующая строго мягче предыдущей, последняя — «без прав вовсе», что
// допускает запросы FILE_ANY_ACCESS, но не запись.
enum class Access : std::uint8_t {
    Read,       // GENERIC_READ, затем FILE_READ_ATTRIBUTES, затем 0 — по умолчанию
    ReadWrite,  // GENERIC_READ|GENERIC_WRITE — форма FR-1 п.3 для \\.\PhysicalDriveN
    Minimal,    // FILE_READ_ATTRIBUTES, затем 0
};

// Параметры запроса.
struct QueryOptions {
    // timeout <= 0 означает «не ждать вовсе»: запрос всё равно уходит в
    // overlapped-режиме, но с нулевым ожиданием, и неуспевший помечается
    // TimedOut. Это способ опросить максимум дисков за секунду, не зависнув на
    // первом «мёртвом».
    std::chrono::milliseconds timeout{kDefaultPropertyTimeout};

    // Предохранитель на размер дескриптора; см. kMaxDescriptorBytes.
    std::uint32_t maxDescriptorBytes{kMaxDescriptorBytes};

    // Права доступа к дескриптору устройства. По умолчанию Read: все три
    // свойства запрашиваются IOCTL с FILE_ANY_ACCESS, поэтому повышение прав
    // (§5: «рантайм-повышения нет… повышение — один раз при старте») не нужно
    // и не должно быть условием того, что карточка диска вообще заполнится.
    Access access{Access::Read};

    // Писать ли неудачи в лог. Да по умолчанию (SPEC §5, §12: отказ виден в
    // логе с путём и кодом), но дампы и CI, где «свойство не поддерживается» —
    // норма, а не сбой, выключают это явно.
    bool logFailures{true};
};

// ---------------------------------------------------------------------------
// Сводный результат по диску
// ---------------------------------------------------------------------------

// Три свойства вместе. Статус у каждого свойства свой: диск вполне может знать
// про TRIM и не знать про предел очереди IO, и «всё или ничего» тут сделало бы
// карточку пустой.
struct StorageFlags {
    PropertyResult<TrimInfo> trim;
    PropertyResult<WriteCacheInfo> writeCache;
    PropertyResult<IoCapabilityInfo> ioCapability;

    [[nodiscard]] std::uint32_t answered() const noexcept {
        return (trim.ok() ? 1u : 0u) + (writeCache.ok() ? 1u : 0u) + (ioCapability.ok() ? 1u : 0u);
    }

    [[nodiscard]] bool complete() const noexcept { return answered() == 3u; }
};

struct StorageFlagsResult {
    int diskNumber{-1};              // номер диска, если вызывающий его знал, иначе -1
    std::wstring devicePath;         // путь, который на самом деле открыли (UTF-16)
    StorageFlags flags;
    // Статус открытия устройства. ok() — устройство ответило хотя бы частично:
    // свойства внутри могут быть Unsupported, и это нормально. Не ok() — открыть
    // не удалось вовсе, и тогда все три свойства непрочитаны.
    QueryStatus status{QueryStatus::Unavailable};
    std::uint32_t win32Error{};
    std::chrono::milliseconds elapsed{};
    // Сколько запросов пришлось бросить: отмена не завершилась, буфер утекает и
    // остальные свойства не опрашивались. Ноль в этом поле означает «устройство
    // ответило, молчания драйвера не было»; единица — «устройство зависло», и в
    // отчёте это должно быть видно, а не выглядеть как «свойств нет».
    std::uint32_t abandonedCalls{};

    [[nodiscard]] bool ok() const noexcept { return status == QueryStatus::Ok; }
};

// ---------------------------------------------------------------------------
// Запросы
// ---------------------------------------------------------------------------

// Три отдельных запроса по готовому device path: «\\?\X#&…\» (его отдаёт
// platform::devices) либо «\\.\PhysicalDriveN». Ничего не бросают и не требуют
// повышения прав: любой отказ — это статус с кодом, а не исключение (FR-1:
// «приложение не падает»).
//
// devicePath принимается как string_view и копируется перед CreateFileW: путь из
// SetupAPI может быть подстрокой строки, а CreateFileW требует завершающий NUL.
[[nodiscard]] PropertyResult<TrimInfo> queryTrim(std::wstring_view devicePath,
                                                 const QueryOptions& options = {}) noexcept;

[[nodiscard]] PropertyResult<WriteCacheInfo> queryWriteCache(std::wstring_view devicePath,
                                                             const QueryOptions& options = {}) noexcept;

[[nodiscard]] PropertyResult<IoCapabilityInfo> queryIoCapability(std::wstring_view devicePath,
                                                                 const QueryOptions& options = {}) noexcept;

// Три свойства за одно открытие устройства. Именно такая форма нужна FR-1:
// «обход всех дисков за один проход», и открывать \\.\PhysicalDriveN трижды под
//ряд — это три лишних CreateFileW на каждый диск системы.
//
// Возвращает частичный результат: если устройство ответило на TRIM и не ответило
// на остальное, это будет видно по статусам свойств, а не по пустому отказу.
[[nodiscard]] StorageFlagsResult queryStorageFlags(std::wstring_view devicePath,
                                                  const QueryOptions& options = {}) noexcept;

// То же по номеру диска (0, 1, 2… — как их отдаёт platform::devices).
[[nodiscard]] StorageFlagsResult queryStorageFlagsOnDisk(int diskNumber,
                                                         const QueryOptions& options = {}) noexcept;

// «\\.\PhysicalDriveN» — форма, которую требует FR-1 п.3. Отрицательный номер
// даёт пустую строку: такого диска нет, и открывать нечего. Проверка «диск есть и
// отвечает» — это открытие устройства, то есть queryStorageFlagsOnDisk.
[[nodiscard]] std::wstring physicalDrivePath(int diskNumber);

// ---------------------------------------------------------------------------
// Тексты для лога и дампа
// ---------------------------------------------------------------------------

// Однострочные описания разобранных свойств: лог, текстовый дамп (FR-8) и
// объяснение в карточке диска. Не локализация: строки русские, как и остальные
// сообщения слоя (см. devices::Issue::message), а UI переводит свои подписи
// отдельно через core::i18n.
[[nodiscard]] std::string describe(const TrimInfo& info);
[[nodiscard]] std::string describe(const WriteCacheInfo& info);
[[nodiscard]] std::string describe(const IoCapabilityInfo& info);

// Сводная строка по диску со статусами — то, что попадает в лог инвентаризации и
// в текстовый дамп карты разделов. При непрочитанных свойствах печатает их
// имена, а не молчит: «TRIM неизвестен» и отсутствие строки должны читаться
// по-разному.
[[nodiscard]] std::string describe(const StorageFlagsResult& result);

}  // namespace mrproper::platform::trim_cache
