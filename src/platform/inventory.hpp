// Кэш картины дисков: сбор за один проход, иммутабельный снимок, реакция на
// WM_DEVICECHANGE и деградированный режим при отказе устройства.
//
// Спека: §4 FR-1 (требования к результату: «обход всех дисков за один проход,
// устойчивость к "отказавшим" дискам (таймаут 2 с на устройство, устройство
// помечается недоступным, приложение не падает), результат кэшируется и
// перечитывается по событию WM_DEVICECHANGE»), §10 (риск «Диск/раздел отваливается
// в IOCTL» → «таймауты, изоляция на устройство, degraded-режим, понятное
// сообщение»), §8 Этап 1 («DiskInventory + кэш + реакция на WM_DEVICECHANGE»),
// §6.4 («результаты не мутируются после публикации: shared_ptr<const ScanResult>»).
//
// ---------------------------------------------------------------------------
// Что модуль делает
// ---------------------------------------------------------------------------
//   1) Сбор: обходит все физические диски за один проход и склеивает
//      core::PhysicalDisk из соседних модулей платформы —
//        devices    (FR-1 п.1: SetupAPI + IOCTL_STORAGE_GET_DEVICE_NUMBER),
//        size_probe (FR-1 п.3: IOCTL_DISK_GET_LENGTH_INFO),
//        layout     (FR-1 п.4: IOCTL_DISK_GET_DRIVE_LAYOUT_EX),
//        volumes    (FR-1 п.5: Find*Volume + VOLUME_GET_VOLUME_DISK_EXTENTS),
//        size_probe (FR-1 п.6: GetDiskFreeSpaceExW);
//   2) Публикация: core::DiskInventory внутри снимка, который UI читает без
//      блокировок (SPEC §6.4) — снимок иммутабелен, наружу отдаётся только
//      shared_ptr<const Snapshot>;
//   3) Кэш: перечитывание не по таймеру, а по событию WM_DEVICECHANGE, с
//      защитой от шторма событий (minRefreshInterval) и слиянием параллельных
//      запросов в один обход;
//   4) Деградированный режим: таймаут на устройство (2 с по FR-1), изоляция
//      отказа на одном диске, понятное сообщение вместо падения — и главное:
//      непустой прошлый снимок не выбрасывается, если новый обход не дал ни
//      одного диска с данными.
//
// ---------------------------------------------------------------------------
// Чего модуль сознательно НЕ делает (границы, чтобы работа не расползлась)
// ---------------------------------------------------------------------------
//   * не читает свойства хранилища (FR-1 п.2): модель, серийник, прошивка,
//     шина, TRIM и кэш записи добывают platform::storage_query (StorageDevice
//     Property / StorageDeviceIdProperty), platform::bus_type
//     (STORAGE_ADAPTER_DESCRIPTOR) и platform::trim_cache (TRIM / write cache).
//     Модуль их не вызывает: это соседние задачи, и их API меняется вместе с
//     ними. Для свойств здесь есть структура DiskProperties и единственная точка
//     записи в модель — applyProperties. Как только появится источник, обход
//     дополняется тремя вызовами рядом с этим местом, а кэш уже умеет переносить
//     значения из прошлого снимка (Options::reuseLastKnownProperties) — чтобы
//     при отказе устройства карточка диска не теряла модель и серийник;
//   * не спрашивает WMI (FR-1 п.7, BitLocker) — platform::wmi;
//   * не создаёт окон и не живёт в UI: WM_DEVICECHANGE приходит в UI-поток, сюда
//     приходит только пара (wParam, lParam) — см. classifyDeviceChange и
//     InventoryCache::onDeviceChange;
//   * не печатает JSON (FR-8) и не рисует карту разделов (FR-2): за JSON отвечает
//     core::report_json, текст для баг-репортов даёт core::DiskInventory::toText,
//     отрисовка — UI поверх снимка.
//
// ---------------------------------------------------------------------------
// Границы таймаутов — кто за что отвечает
// ---------------------------------------------------------------------------
// Таймаут на устройство (FR-1) держит не весь слой, а конкретный модуль на
// конкретном вызове, иначе «изоляция на устройство» превратилась бы в зависание
// всего обхода:
//   * size_probe сам ограничивает ожидание своего IOCTL потоком и
//     CancelIoEx — повторно ограничивать его вызов не нужно;
//   * layout::readDriveLayout работает синхронно, без OVERLAPPED, поэтому его
//     ограничивает вызывающий: здесь он уходит в отдельный поток, и если за
//     таймаут не вернулся — раздел помечается недоступным (в потоке остаётся
//     вызов внутри драйвера; он убирает состояние сам, когда вернётся);
//   * volumes::enumerate() — перечисление томов целиком: собственный таймаут у
//     него есть на IOCTL, но не на GetVolumeInformationW/CreateFileW, и
//     volumes.hpp прямо передаёт этот таймаут вызывающему, то есть сюда.
//
// Счётчик брошенных вызовов — abandonedCalls(): после таймаута поток остаётся в
// драйвере, и это плата за «приложение не падает». Её видно, а не прячется.
//
// ---------------------------------------------------------------------------
// Слой
// ---------------------------------------------------------------------------
// Файл не включает windows.h намеренно: объявления используют только переносимые
// типы, поэтому его могут включить engine, ui, cli и тесты без Win32. Единственная
// единица трансляции с WinAPI — inventory.cpp; она физически не может попасть в
// mrproper_core, потому что граница проходит по границе каталогов (SPEC §6.1,
// ADR-004). Единственная зависимость — вниз, на mrproper_core и соседние
// модули src/platform.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/disk_model.hpp"
#include "core/model.hpp"

namespace mrproper::platform::inventory {

// ---------------------------------------------------------------------------
// Политика сбора
// ---------------------------------------------------------------------------

struct Options {
    // Таймаут на одно устройство (FR-1: «таймаут 2 с на устройство»). Применяется
    // к size_probe, к разметке (собственный поток) и к перечислению томов.
    std::chrono::milliseconds deviceTimeout{2000};

    // Таймаут на перечисление всех томов системы отдельно от таймаута на диск:
    // один вызов, поэтому и бюджет другой. Превышение не роняет сбор — карта
    // дисков останется, томов в ней не будет, снимок будет помечен деградированным.
    std::chrono::milliseconds volumeEnumerationTimeout{5000};

    // DIGCF_PRESENT: только присутствующие устройства. Исчезнувший диск не должен
    // оставаться в карте разделов.
    bool presentOnly{true};

    // Вставлять неразмеченные промежутки как core::PartitionKind::Unallocated.
    // false (по умолчанию) — только настоящие разделы, иначе их число разойдётся
    // с Get-Disk и diskpart (§8 Этап 1, §12). true — карта FR-2 показывает всю
    // поверхность диска, включая незанятые области.
    bool includeUnallocated{false};

    // Спрашивать свободное место на томах (FR-1 п.6, GetDiskFreeSpaceExW). false
    // оставляет core::Volume::totalBytes и freeBytes нулевыми: карта разделов
    // остаётся полной, а суммы занятости — нет.
    bool queryVolumeSpace{true};

    // Переносить свойства хранилища (FR-1 п.2) из прошлого снимка. Считывает их
    // соседний модуль (см. границы в шапке), а кэш помнит их между обходами:
    // потерять модель и серийник из-за одного зависшего диска неправильно, и у
    // диска, который сегодня не ответил, модель не менялась. Пока источника нет,
    // значения пусты и перенос ничего не меняет — включать опцию имеет смысл
    // вместе с ним.
    bool reuseLastKnownProperties{true};

    // Предохранитель на число устройств: нормально обход заканчивается на
    // ERROR_NO_MORE_ITEMS, но сломанный драйвер может отдавать индексы вечно.
    std::uint32_t maxDevices{64};
};

// Почему этот обход понадобился. Значения — константы публичного WinAPI
// (winuser.h/dbt.h), а не изобретение модуля: заголовок обязан оставаться
// переносимым, поэтому числа записаны здесь, а в inventory.cpp они
// сверяются static_assert с настоящими макросами DBT_*. Расхождение с SDK
// будет ошибкой компиляции, а не тихой ошибкой разбора события.
inline constexpr std::uint32_t kDeviceNodesChanged = 0x0007;  // DBT_DEVNODES_CHANGED
inline constexpr std::uint32_t kDeviceArrival = 0x8000;       // DBT_DEVICEARRIVAL
inline constexpr std::uint32_t kDeviceQueryRemove = 0x8001;   // DBT_DEVICEQUERYREMOVE
inline constexpr std::uint32_t kDeviceQueryRemoveFailed = 0x8002;  // DBT_DEVICEQUERYREMOVEFAILED
inline constexpr std::uint32_t kDeviceRemovePending = 0x8003;      // DBT_DEVICEREMOVEPENDING
inline constexpr std::uint32_t kDeviceRemoveComplete = 0x8004;     // DBT_DEVICEREMOVECOMPLETE
inline constexpr std::uint32_t kDeviceTypeSpecific = 0x8005;       // DBT_DEVICETYPESPECIFIC

// Тип устройства из заголовка широковещания (dbch_devicetype / dbcd_devicetype).
inline constexpr std::uint32_t kDeviceTypeVolume = 0x00000002;           // DBT_DEVTYPE_VOLUME
inline constexpr std::uint32_t kDeviceTypeDeviceInterface = 0x00000005; // DBT_DEVTYPE_DEVICEINTERFACE

// ---------------------------------------------------------------------------
// Состояние устройства после опроса
// ---------------------------------------------------------------------------

// Итог по одному физическому диску. FR-1 требует «устройство помечается
// недоступным» — это и есть этот перечень: значение не Ok означает, что часть
// карты по диску отсутствует, и интерфейс должен показать диск серым, а не
// выбросить его молча.
enum class DeviceState : std::uint8_t {
    Unknown = 0,  // состояние не определено: диска нет в перечислении
    Ok,           // размер, разметка и тома получены
    TimedOut,     // не ответил за таймаут (FR-1: «отказавший» диск)
    AccessDenied, // нет прав на \\.\PhysicalDriveN: процесс не повышен (SPEC §5, §12)
    NotFound,     // устройство исчезло между перечислением и опросом
    Unsupported,  // драйвер не отдаёт запрошенное (старый контроллер, виртуальный диск)
    LayoutFailed, // размер получен, разметка — нет
    NoNumber,     // SetupAPI не дал номер: нечем открыть \\.\PhysicalDriveN
};

// Стабильное имя состояния для лога, JSON-дампа дисков и UI: не локализуется.
const char* deviceStateName(DeviceState state) noexcept;

// Состояние по коду Win32 — для вызовов, которые отдают только код (разметка,
// CreateFileW). Чистая функция, проверяется без Windows.
DeviceState deviceStateFromWin32(std::uint32_t win32Error) noexcept;

// Состояние по ответу size_probe. Объявлено здесь, а не в .cpp, чтобы
// InventoryCache и отчёт могли разбирать ProbeStatus одинаково.
struct ProbeOutcome {
    DeviceState state{DeviceState::Unknown};
    bool sizeKnown{false};
    bool layoutKnown{false};
};

// Что известно о диске после обхода. Читается UI и попадает в отчёт (FR-8),
// поэтому здесь ровно то, что нужно показать: состояние, ошибка, время.
struct DiskProbe {
    int diskNumber{-1};          // -1 — номер не получен (см. DeviceState::NoNumber)
    std::string devicePath;      // "\\?\X#&…\" — как отдал SetupAPI, с завершающим разделителем
    std::string instanceId;      // SetupDiGetDeviceInstanceIdW, UTF-8; пусто, если не получили
    DeviceState state{DeviceState::Unknown};
    std::uint32_t sizeError{};   // код Win32 из FR-1 п.3; 0, если размер получен
    std::uint32_t layoutError{}; // код Win32 из FR-1 п.4; 0, если разметка прочитана
    std::uint64_t sizeBytes{};   // 0, если размер не получен — и это «нет данных», а не «пусто»
    std::size_t partitionCount{};
    std::size_t volumeCount{};   // томов, привязанных к диску (том через несколько дисков учтён один раз)
    std::chrono::milliseconds elapsed{};  // сколько занял весь опрос диска
    bool numberKnown{false};
    bool sizeKnown{false};
    bool layoutKnown{false};
    std::string message;  // по-русски, одно предложение; пусто при state == Ok
};

// ---------------------------------------------------------------------------
// Замечания обхода
// ---------------------------------------------------------------------------

// Этап, на котором что-то не сработало. Нужен не для красоты: этап в логе
// отличает «диск не ответил» (FR-1: устройство помечается недоступным) от
// «SetupAPI не отдал путь устройства» (конфигурация системы, а не диск).
enum class Stage {
    Enumerate,     // devices: перечисление интерфейсов дисков (FR-1 п.1)
    DiskSize,      // size_probe: IOCTL_DISK_GET_LENGTH_INFO (FR-1 п.3)
    DiskLayout,    // layout: IOCTL_DISK_GET_DRIVE_LAYOUT_EX (FR-1 п.4)
    VolumeList,    // volumes: перечисление томов (FR-1 п.5)
    VolumeSpace,   // size_probe: GetDiskFreeSpaceExW (FR-1 п.6)
    VolumeBinding, // привязка тома к разделу по extent'ам (FR-1 п.5)
    Cache,         // политика кэша: отклонённый снимок, шторм событий
};

// Короткое имя этапа для лога и отчёта: не локализуется.
const char* stageName(Stage stage) noexcept;

enum class IssueLevel : std::uint8_t { Info, Warning, Error };
const char* issueLevelName(IssueLevel level) noexcept;

// Замечание обхода. Не фатально: FR-1 требует, чтобы ни один отказ не ронял
// приложение, поэтому неудача — это запись в списке, а не исключение.
struct Issue {
    Stage stage{Stage::Enumerate};
    IssueLevel level{IssueLevel::Info};
    int diskNumber{-1};       // -1 — замечание уровня инвентаризации
    std::string subject;      // путь устройства или volumeGuidPath, к которому относится замечание
    std::uint32_t win32Error{};  // 0 — «замечание без системного кода»
    std::string message;      // по-русски, одно предложение
};

// ---------------------------------------------------------------------------
// Свойства хранилища (FR-1 п.2) — точка приклейки для соседнего модуля
// ---------------------------------------------------------------------------

// Модель, серийник, прошивка, шина, TRIM и кэш записи читает
// platform::storage_query, которого в слое на момент написания этого модуля
// ещё нет. Структура и applyProperties() существуют уже сейчас, чтобы обход не
// пришлось переписывать, когда появится соседний модуль, и чтобы модель/серийник
// не терялись при отказе устройства (Options::reuseLastKnownProperties).
struct DiskProperties {
    int diskNumber{-1};
    std::string model;
    std::string serial;
    std::string firmware;
    core::BusType bus{core::BusType::Unknown};
    bool removable{};
    bool readOnly{};
    bool trimSupported{};
    bool smartAvailable{};
    // Свойства пришли от FR-1 п.2 (хотя бы частично). false — значит «не
    // опрошено», и UI не должен показывать «нет TRIM», показывая «н/д».
    bool known{false};
};

// Перенести свойства в модель диска. Молча игнорирует свойства чужого диска
// (diskNumber не совпадает) и не затирает модель пустой строкой при known == false.
void applyProperties(core::PhysicalDisk& disk, const DiskProperties& properties) noexcept;

// ---------------------------------------------------------------------------
// Причина обхода
// ---------------------------------------------------------------------------

// Почему обход выполняется. В снимке и в логе: разница между «пользователь
// нажал кнопку» и «пришло событие WM_DEVICECHANGE» важна при разборе, почему
// карта перерисовалась сама.
enum class RefreshReason : std::uint8_t {
    Manual,        // явный запрос вызывающего (кнопка «Обновить»)
    Startup,       // первый обход при старте приложения
    DeviceChange,  // реакция на WM_DEVICECHANGE
    Periodic,      // плановая перепроверка по таймеру
    AfterCleanup,  // после очистки: изменились занятость и точки монтирования
};

const char* refreshReasonName(RefreshReason reason) noexcept;

// ---------------------------------------------------------------------------
// Снимок
// ---------------------------------------------------------------------------

// Почему снимок: SPEC §6.4 требует, чтобы результат не мутировался после
// публикации. Сбор идёт в фоновом потоке, UI читает снимок параллельно, поэтому
// наружу отдаётся только shared_ptr<const Snapshot>, а сам снимок меняется
// целиком (новый поколение), а не по кускам.
struct Snapshot {
    // Карта дисков в детерминированном порядке с пересчитанными агрегатами и
    // замечаниями согласованности (core::validateInventory).
    core::DiskInventory inventory;

    // Что известно о каждом устройстве: состояние, ошибка, время. В core::PhysicalDisk
    // места для этого нет, а UI и отчёт должны различать «нет данных» и «пусто».
    std::vector<DiskProbe> devices;

    // Замечания обхода: отказы IOCTL/устройств плюс расхождения привязки томов.
    std::vector<Issue> issues;

    // Тома, которые не удалось привязать ни к одному разделу: динамический диск,
    // RAID с собственными томами или отказ IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS.
    // Они не потеряны — они в этом списке (FR-1 п.5).
    std::vector<core::Volume> unboundVolumes;

    // Свойства хранилижа, известные на момент обхода (см. DiskProperties).
    std::vector<DiskProperties> properties;

    std::uint64_t generation{};  // монотонный номер снимка: в логе и отчёте видно, что снимок новый
    RefreshReason reason{RefreshReason::Manual};
    std::chrono::system_clock::time_point collectedAt{};
    std::chrono::milliseconds duration{};

    // Перечисление дисков дошло до конца (devices::EnumerateResult::ok).
    bool disksComplete{false};
    // Перечисление томов дошло до конца (volumes::Enumeration::completed).
    bool volumesComplete{false};

    // Деградированный режим (§10): карта собрана, но неполна — есть не менее
    // одной причины в degradedReasons. Приложение при этом работает: данные
    // показываются с пометкой, а не отбрасываются.
    bool degraded{false};
    std::vector<std::string> degradedReasons;

    // Состояние одного устройства; nullptr, если диска нет в снимке.
    const DiskProbe* deviceOf(int diskNumber) const noexcept;

    // Есть ли в снимке хоть один диск, о котором известно ЧТО-ТО ПОЛЕЗНОЕ:
    // размер или хотя бы один раздел. Именно этим ответом решается, публиковать
    // ли снимок (InventoryCache::publishable) — непустой путь устройства карту
    // разделов не рисует, и публиковать такой снимок вместо прошлого значило бы
    // отдать пользователю пустую карту.
    bool hasDiskData() const noexcept;

    // Сколько устройств не ответило за таймаут (для лога и отчёта, §12).
    std::uint32_t timedOutCount() const noexcept;
    // Сколько устройств в снимке без данных (нет размера и разделов) — их UI
    // показывает серым, как и требует FR-1.
    std::uint32_t unavailableCount() const noexcept;

    // Причина деградации одной строкой — для баннера в UI и для поля в отчёте.
    std::string degradedSummary() const;

    // Снимок целиком текстом: карта разделов (core), состояние устройств,
    // замечания и непривязанные тома. Для баг-репортов (FR-2, FR-8).
    std::string toText() const;
};

// ---------------------------------------------------------------------------
// Сбор
// ---------------------------------------------------------------------------

// Собрать карту один раз, без кэша и без потоков. Нужен вызывающему, который сам
// держит поток (CLI, тест, движок) и не хочет платить за фонового работника
// кэша. Функция ничего не бросает наружу: нехватка памяти и отказ Win32
// превращаются в деградированный снимок с записью в лог (SPEC §5, §12).
//
// previous — необязательный прошлый снимок: из него берутся свойства хранилища
// (Options::reuseLastKnownProperties) и, если включено carryOver, данные
// недоступного устройства. Владеть им не нужно, он должен пережить вызов.
[[nodiscard]] std::shared_ptr<const Snapshot> collect(const Options& options, RefreshReason reason,
                                                      const Snapshot* previous = nullptr) noexcept;

// ---------------------------------------------------------------------------
// WM_DEVICECHANGE
// ---------------------------------------------------------------------------

// Событие WM_DEVICECHANGE в терминах модуля. Значения совпадают с wParam из
// winuser.h (сверяется static_assert в .cpp).
enum class DeviceEvent : std::uint32_t {
    Unknown = 0,
    DeviceNodesChanged = kDeviceNodesChanged,   // devnode появился или исчез: карта дисков устарела
    DeviceArrival = kDeviceArrival,             // система увидела новое устройство
    DeviceQueryRemove = kDeviceQueryRemove,     // система спрашивает, можно ли отключать
    DeviceQueryRemoveFailed = kDeviceQueryRemoveFailed,  // отключение сорвалось
    DeviceRemovePending = kDeviceRemovePending, // вот-вот отключат, устройство ещё доступно
    DeviceRemoveComplete = kDeviceRemoveComplete,        // устройства больше нет
    DeviceTypeSpecific = kDeviceTypeSpecific,   // событие конкретного типа устройства
};

const char* deviceEventName(DeviceEvent event) noexcept;

// Что делать с событием.
enum class DeviceChangeAction : std::uint8_t {
    Ignore,     // на карту дисков не влияет: перечитывать незачем
    MarkStale,  // карта помечается устаревшей, обход запускается при следующем удобном случае
    Refresh,    // карту надо перечитать
};

const char* deviceChangeActionName(DeviceChangeAction action) noexcept;

// Заголовок широковещания из lParam. Читается осторожно: указатель приходит из
// системы, поэтому чтение защищено SEH, а неизвестная или битая структура даёт
// «не разобрано», а не падение.
struct BroadcastInfo {
    bool parsed{false};        // заголовок прочитан и выглядит правдоподобно
    std::uint32_t sizeBytes{}; // dbch_size / dbcd_size
    std::uint32_t deviceType{};// dbch_devicetype / dbcd_devicetype
    bool knownDeviceType{false};
    bool storageClass{false};   // класс GUID_DEVINTERFACE_DISK или GUID_DEVINTERFACE_VOLUME
};

[[nodiscard]] BroadcastInfo readBroadcastInfo(std::uintptr_t lParam) noexcept;

// Решение по событию: что означает wParam для карты дисков. Вынесено из кэша
// отдельной чистой функцией, чтобы политику можно было проверить и без потоков.
//
// Правило простое и осознанное: DBT_DEVNODES_CHANGED и появление/исчезновение
// устройства меняют либо состав дисков, либо их разметку, поэтому карта
// перечитывается целиком. Точечного обновления одного диска здесь нет намеренно:
// частичное обновление оставило бы в снимке смесь свежих и устаревших чисел без
// признака, где кончается одно и начинается другое, а FR-1 требует, чтобы
// результат был честным. События о запросах отключения (DBT_DEVICEQUERYREMOVE и
// DBT_DEVICEQUERYREMOVEFAILED) и о не-хранилище (сетевой адаптер, порт) только
// помечают кэш устаревшим.
[[nodiscard]] DeviceChangeAction deviceChangeActionFor(DeviceEvent event, const BroadcastInfo& info) noexcept;

struct DeviceChange {
    DeviceEvent event{DeviceEvent::Unknown};
    DeviceChangeAction action{DeviceChangeAction::Ignore};
    bool needsRefresh{false};
    std::string reason;  // по-русски: что произошло и что сделано (лог, UI)
};

// Разобрать WM_DEVICECHANGE без побочных эффектов.
[[nodiscard]] DeviceChange classifyDeviceChange(std::uint32_t wParam, std::uintptr_t lParam) noexcept;

// ---------------------------------------------------------------------------
// Кэш
// ---------------------------------------------------------------------------

// Счётчики кэша: диагностика для журнала и отчёта (§12 — «видно, сколько
// устройств не ответило»). На поведение не влияют.
struct CacheStats {
    std::uint64_t collections{};              // выполнено обходов
    std::uint64_t published{};                // снимков опубликовано
    std::uint64_t rejected{};                 // обходов отброшено: пустая карта при живом предыдущем снимке
    std::uint64_t coalesced{};                // запросов, слитых с уже идущим обходом
    std::uint64_t debounced{};                // запросов, отложенных до minRefreshInterval
    std::uint64_t deviceChanges{};            // событий WM_DEVICECHANGE
    std::uint64_t refreshesFromDeviceChange{};// обходов, начатых по событию
    std::uint64_t timedOutDevices{};          // устройств, не ответивших за таймаут (по всем обходам)
    std::uint64_t abandonedCalls{};           // вызовов, оставшихся в драйвере после таймаута
    std::chrono::steady_clock::time_point lastRefresh{};
    std::chrono::milliseconds lastDuration{};
};

struct CacheOptions {
    Options collect;  // как собирать (см. Options)
    // Анти-шторм. WM_DEVICECHANGE приходит пачками: монтирование тома даёт
    // несколько событий подряд, и на каждый полный обход заново открывать все
    // диски — бессмысленная нагрузка. События в этом окне помечают кэш
    // устаревшим, а обход выполняется один — когда окно истекло.
    std::chrono::milliseconds minRefreshInterval{750};
    // Реагировать ли на WM_DEVICECHANGE автоматически. false — события только
    // помечают кэш устаревшим, перечитывание делает вызывающий (полезно тесту и
    // CLI, где нет окна).
    bool autoRefreshOnDeviceChange{true};
};

// Кэш картины дисков.
//
// Владение и потоки. Класс не копируется и не перемещается: внутри живёт
// std::jthread, и копия кэша означала бы два потока на одну карту. Владение
// экземпляром — у того, кто переживает окно (обычно app shell).
//
// Потокобезопасность. current(), stats(), busy(), stale() можно звать из UI-потока
// в любой момент: они берут мьютекс и отдают копию указателя на иммутабельный
// снимок. Сбор идёт в фоновом потоке, поэтому UI-поток на WM_DEVICECHANGE
// возвращается сразу и не ждёт по 2 с на каждый диск (SPEC §6.4: единственный
// UI-поток не блокируется на I/O).
//
// Деградированный режим. Если обход не дал ни одного диска с данными, а
// предыдущий снимок был непустым, публикация не происходит: current() продолжает
// отдавать прошлый снимок, а кэш помечается устаревшим с причиной. Пустая карта
// на месте рабочей — это потеря данных у пользователя (FR-2, §12: «0 крашей» и
// «понятное сообщение» важнее, чем честная пустота).
class InventoryCache {
public:
    explicit InventoryCache(CacheOptions options = {});
    // Останавливает фоновый поток и присоединяется к нему. Если в момент
    // уничтожения идёт обход, деструктор дождётся его: прервать синхронный
    // вызов Win32 нельзя, а бросать поток работать с разрушенным кэшем — можно.
    // Верхняя граница ожидания — сумма таймаутов обхода (FR-1: 2 с на диск плюс
    // перечисление томов).
    ~InventoryCache();

    InventoryCache(const InventoryCache&) = delete;
    InventoryCache& operator=(const InventoryCache&) = delete;
    InventoryCache(InventoryCache&&) = delete;
    InventoryCache& operator=(InventoryCache&&) = delete;

    // Текущий снимок; nullptr, пока не было ни одного успешного обхода.
    [[nodiscard]] std::shared_ptr<const Snapshot> current() const noexcept;

    // Номер поколения текущего снимка (0 — снимка ещё нет). UI использует его
    // как признак «карта обновилась», не сравнивая содержимое.
    [[nodiscard]] std::uint64_t generation() const noexcept;

    // Идёт ли обход прямо сейчас.
    [[nodiscard]] bool busy() const noexcept;

    // Помечен ли кэш устаревшим: событие пришло или обход не удался, а нового
    // снимка ещё нет.
    [[nodiscard]] bool stale() const noexcept;
    [[nodiscard]] std::string staleReason() const;

    // Перечитать карту в потоке вызывающего. Возвращает опубликованный снимок:
    // либо новый, либо (в деградированном режиме) предыдущий. Блокирует, поэтому
    // из UI-потока не вызывается.
    [[nodiscard]] std::shared_ptr<const Snapshot> refreshNow(RefreshReason reason = RefreshReason::Manual);

    // Запросить перечитывание в фоновом потоке. false — обход уже идёт, запрос
    // слит с ним (cacheStats().coalesced) и будет выполнен следом. Не блокирует.
    bool requestRefresh(RefreshReason reason = RefreshReason::Manual);

    // Реакция на WM_DEVICECHANGE. Вызывается из UI-потока обработчика окна.
    // Разбирает событие, при необходимости помечает кэш устаревшим и запускает
    // перечитывание (если CacheOptions::autoRefreshOnDeviceChange).
    DeviceChange onDeviceChange(std::uint32_t wParam, std::uintptr_t lParam);

    // Пометить кэш устаревшим и перечитать при ближайшей возможности, минуя
    // minRefreshInterval (например, после операции очистки, изменившей карту).
    void invalidate(std::string reason);

    // Дождаться конца текущего обхода. Для тестов и CLI; UI не пользуется.
    bool waitForIdle(std::chrono::milliseconds timeout);

    [[nodiscard]] CacheStats stats() const;
    [[nodiscard]] std::string toText() const;  // состояние кэша для журнала и баг-репорта

private:
    void workerLoop(std::stop_token token);
    std::shared_ptr<const Snapshot> runCollection(RefreshReason reason, bool allowPublish);
    void ensureWorkerLocked();
    // Нельзя ли публиковать снимок: пустая карта при непустом предыдущем.
    static bool publishable(const Snapshot& next, const Snapshot* previous) noexcept;

    CacheOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;

    std::shared_ptr<const Snapshot> current_;
    std::string staleReason_;
    bool pending_{false};        // есть запрос на обход
    bool pendingForced_{false};  // этот запрос обходит minRefreshInterval
    bool busy_{false};           // обход выполняется прямо сейчас
    // Помечен ли кэш устаревшим: событие пришло или обход не удался, а нового
    // снимка так и нет. Причину держит staleReason_.
    bool stale_{false};
    bool stopping_{false};
    RefreshReason pendingReason_{RefreshReason::Manual};
    std::chrono::steady_clock::time_point lastRefresh_{};

    CacheStats stats_{};

    // Сбор последователен: два обхода одновременно открывают одни и те же
    // устройства и только мешают друг другу. Мьютекс не тот, что у состояния:
    // его можно держать минуты, а состояние читается постоянно.
    std::mutex collectionMutex_;
};

}  // namespace mrproper::platform::inventory
