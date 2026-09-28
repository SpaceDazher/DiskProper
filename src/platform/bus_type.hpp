// Тип шины диска: разбор значения BusType в core::BusType и свойства, которые
// из него следуют. Спека: §4 FR-1 п.2 («модель, серийник, прошивка, тип шины»,
// `STORAGE_ADAPTER_DESCRIPTOR` → `BusType`: NVMe=0x11, SATA/ATA, SCSI, USB),
// §6.2 (модули слоя platform, провайдеры SMART), §6.3 (`BusType` в модели), §9.1
// ADR-004 (обратной зависимости у Win32 нет).
//
// Модуль отвечает на один вопрос: «что означает число, которое пришло в поле
// BusType». Больше он не делает ничего, и это осознанный выбор:
//
//   * не отправляет IOCTL_STORAGE_QUERY_PROPERTY. Запрос свойств хранилища
//     (StorageDeviceProperty, StorageAdapterProperty, StorageDeviceTrimProperty
//     и остальные из FR-1 п.2) — соседний модуль platform::storage_query, он же
//     владеет таймаутом на устройство и правами доступа. Здесь нужен только
//     перевод уже прочитанного значения;
//   * не собирает core::PhysicalDisk — это шаг инвентаризации (FR-1 целиком).
//     Мост «прочитали BusType → записали в модель» делает вызывающий;
//   * не даёт собственного имени для core::BusType. Короткое имя перечисления
//     модели уже есть в core: core::busTypeName (disk_model.hpp). Второе
//     такое же имя отличалось бы от первого только регистром букв, а это
//     худший вид дублирования: опечатку в строке заметят не сразу.
//
// Отсюда главное свойство модуля: заголовочный файл не включает ни
// <windows.h>, ни <winioctl.h> и не знает ни одного типа WinAPI. Объявления
// работают с переносимыми типами, поэтому этот заголовок может включить и слой
// без Win32 (отчёт, CLI, юнит-тесты на любом хосте), а <winioctl.h> добавляет
// только .cpp — и только ради static_assert, сверяющего таблицу с установленным
// Windows SDK (см. константы kBus*).
//
// --- Источники значения и почему таблица одна ------------------------------
//
// FR-1 п.2 называет BusType у STORAGE_ADAPTER_DESCRIPTOR, и в установленном
// здесь SDK (10.0.19041.0) это так и есть: у адаптера поле BusType — это BYTE,
// рядом стоят BusMajorVersion/BusMinorVersion (WORD). Но то же поле есть и у
// STORAGE_DEVICE_DESCRIPTOR (StorageDeviceProperty), где оно объявлено как
// STORAGE_BUS_TYPE — то есть DWORD.
//
// Значения у обоих полей общие: комментарий в winioctl.h прямо говорит, что
// BusType дескриптора устройства содержит «bus type (as defined above)» — ту же
// таблицу, что и у адаптера. Поэтому функция перевода одна, а различаются
// только ширина поля и то, кто его читает. Два примечания, из-за которых это
// стоит проговорить:
//
//   * у адаптера поле однобайтовое, поэтому коды вендоров (>= 0x80, см. ниже)
//     в него физически не помещаются, а у дескриптора устройства — помещаются;
//   * для NTDDI_VERSION < NTDDI_WINXP в SDK есть отдельная, до-Вистова ветка
//     раскладки адаптера, где BusType объявлен как BOOLEAN и SrbType с
//     AddressType нет. На Windows 10/11 (единственная цель проекта) эта ветка
//     не используется, но если значение пришло из ответа старого драйвера,
//     доверять старшей половине DWORD нельзя — отсюда проверки диапазона
//     в busTypeFromRaw.
#pragma once

#include <cstdint>
#include <string>

#include "core/model.hpp"

namespace mrproper::platform::bus_type {

// ---------------------------------------------------------------------------
// Сырые значения STORAGE_BUS_TYPE
//
// Перечисление продублировано числами, а не ссылками на имена из SDK. Причина
// конкретная: в Windows SDK таблица объявлена дважды — в <winioctl.h> и в
// <ntddstor.h> — с одинаковыми именами перечислителей, и единица трансляции,
// включившая оба заголовка, не собирается. Своя таблица от этого свободна, а
// соответствие установленному SDK проверяется static_assert в .cpp: если
// Microsoft добавит или сдвинет значение, сборка упадёт на сверке, а не
// тихо поменяет смысл у миллиона уже собранных дисков.
//
// Значения < 0x80 зарезервированы за Microsoft (комментарий в winioctl.h),
// поэтому 0x80 и выше — это коды вендоров: угадать их нельзя, и модуль их
// честно помечает как нераспознанные, а не подставляет BusType::Unknown
// молча.
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kBusUnknown = 0x00;
inline constexpr std::uint32_t kBusScsi = 0x01;
inline constexpr std::uint32_t kBusAtapi = 0x02;
inline constexpr std::uint32_t kBusAta = 0x03;
inline constexpr std::uint32_t kBus1394 = 0x04;
inline constexpr std::uint32_t kBusSsa = 0x05;
inline constexpr std::uint32_t kBusFibre = 0x06;
inline constexpr std::uint32_t kBusUsb = 0x07;
inline constexpr std::uint32_t kBusRaid = 0x08;
inline constexpr std::uint32_t kBusIScsi = 0x09;
inline constexpr std::uint32_t kBusSas = 0x0A;
inline constexpr std::uint32_t kBusSata = 0x0B;
inline constexpr std::uint32_t kBusSd = 0x0C;
inline constexpr std::uint32_t kBusMmc = 0x0D;
inline constexpr std::uint32_t kBusVirtual = 0x0E;
inline constexpr std::uint32_t kBusFileBackedVirtual = 0x0F;
inline constexpr std::uint32_t kBusSpaces = 0x10;
inline constexpr std::uint32_t kBusNvme = 0x11;
inline constexpr std::uint32_t kBusScm = 0x12;
inline constexpr std::uint32_t kBusUfs = 0x13;

// BusTypeMax: первое значение, которого нет в таблице. Не шина.
inline constexpr std::uint32_t kBusMax = 0x14;
// BusTypeMaxReserved: верхняя граница диапазона Microsoft.
inline constexpr std::uint32_t kBusMaxReserved = 0x7F;
// Первое значение, принадлежащее вендору. Всё, что не меньше, — не наша таблица.
inline constexpr std::uint32_t kBusVendorFirst = 0x80;

// ---------------------------------------------------------------------------
// Перевод в модель
// ---------------------------------------------------------------------------

// core::BusType по сырому значению BusType. Таблица решений (каждое решение
// объяснено, потому что «почему 1394 стал Unknown» — вопрос, который зададут
// при первом же баге в тексте отчёта):
//
//   0x01 SCSI                              → Scsi     (ровно то же имя)
//   0x11 NVMe                              → Nvme     (ровно то же имя; 0x11 —
//                                                        значение, которое FR-1
//                                                        п.2 называет явно)
//   0x07 USB                               → Usb      (ровно то же имя)
//   0x0A SAS                               → Sas      (ровно то же имя)
//   0x0B SATA                             → Sata     (ровно то же имя)
//   0x0C SD                                → Sd       (ровно то же имя)
//   0x08 RAID                              → Raid     (ровно то же имя)
//   0x0E Virtual                           → Virtual  (ровно то же имя)
//   0x0F FileBackedVirtual                 → Virtual  (то же самое по смыслу:
//                                                        виртуальный диск поверх файлов)
//   0x10 Spaces                            → Virtual  (пул Storage Spaces для
//                                                        приложения выглядит как
//                                                        виртуальный диск;
//                                                        физические носители под ним
//                                                        — отдельные диски, FR-1
//                                                        обходит их сам)
//   0x03 ATA, 0x02 ATAPI                   → Sata     (одно ATA-семейство, а в
//                                                        core::BusType отдельного
//                                                        значения для ATAPI нет;
//                                                        точная подпись остаётся
//                                                        в describeRawBus, где
//                                                        видно исходное 0x02)
//   0x0D MMC                               → Sd       (картридер SD отдаёт
//                                                        BusTypeMmc, а «SD» в
//                                                        модели — то же самое
//                                                        устройство с точки зрения
//                                                        пользователя)
//   0x04 1394, 0x05 SSA, 0x06 Fibre,
//   0x09 iSCSI, 0x12 SCM, 0x13 UFS,
//   0x00 Unknown, всё, что не в таблице   → Unknown  (в core::BusType для них
//                                                        нет значений, §6.3;
//                                                        выдумывать свои нельзя,
//                                                        потому что модель
//                                                        перечисляется в спеке,
//                                                        а имя «Unknown» в
//                                                        интерфейсе означает
//                                                        ровно одно: «тип шины
//                                                        определить не удалось»)
//
// Последнее значение Unknown — осознанный отказ от угадывания. Диск с шиной
// Fibre Channel или FireWire отображается как «неизвестно», но точная подпись
// («BusType=0x06 (BusTypeFibre)») остаётся доступна вызывающему через
// describeRawBus, поэтому диагностика не теряется.
[[nodiscard]] core::BusType busTypeFromRaw(std::uint32_t raw) noexcept;

// Есть ли значение в таблице STORAGE_BUS_TYPE. false у 0x14 (BusTypeMax) и
// 0x7F (BusTypeMaxReserved) тоже: это концы диапазона, а не шины.
[[nodiscard]] bool isKnownRawBus(std::uint32_t raw) noexcept;

// Код вендора (>= 0x80). Отдельная проверка нужна для внятной диагностики:
// «тип шины неизвестен» и «драйвер вернул свой код» — разные новости, и второе
// указывает, где искать (на модель устройства в диспетчере устройств).
[[nodiscard]] bool isVendorRawBus(std::uint32_t raw) noexcept;

// Имя перечислителя Windows как есть: "BusTypeNvme", "BusTypeUsb"… Для отчёта
// (FR-8) и лога. nullptr у значения, которого нет в таблице, — вызывающий сам
// решает, чем заменить. Имена латинские и не локализуются: это идентификаторы
// из SDK, по ним ищут в баг-репортах.
[[nodiscard]] const char* rawBusName(std::uint32_t raw) noexcept;

// Подпись значения для отчёта, лога и карточки диска (FR-2 «модель/интерфейс»,
// FR-8 — экспорт карты):
//   известное   → "BusType=0x11 (BusTypeNvme, NVMe)"
//   0x14        → "BusType=0x14 (BusTypeMax: значений шины дальше нет)"
//   0x7F        → "BusType=0x7F (BusTypeMaxReserved: конец диапазона Microsoft)"
//   код вендора → "BusType=0x85 (код вендора, значение >= 0x80)"
//   прочее      → "BusType=0x1F (нет такого значения в STORAGE_BUS_TYPE)"
// Число в скобках всегда есть, поэтому по подписи видно, что прислал драйвер,
// даже когда расшифровать значение не удалось.
[[nodiscard]] std::string describeRawBus(std::uint32_t raw);

// ---------------------------------------------------------------------------
// Свойства, которые следуют из типа шины
//
// Здесь важна одна мысль: шина не отвечает на вопрос «вращается ли диск» и
// «есть ли на нём TRIM». Отвечают свойства устройства из FR-1 п.2
// (StorageDeviceTrimProperty, MediaRemovable) и модель. Шина отвечает на
// вопросы, ответы на которые из свойств не следуют: как обращаться к устройству
// (какой IOCTL вообще имеет смысл), и как устройство выглядит снаружи
// (переносное, виртуальное, RAID-контроллер).
// ---------------------------------------------------------------------------

// Что можно утверждать о носителе по шине. Перечисление, а не «bool», потому
// что false у bool читался бы как «жёсткий диск», а по SATA/SAS/SCSI/USB это
// неизвестно: там стоит и диск, и плата. Значения Rotational здесь нет —
// произвести его нечем: ни одна шина не говорит «да, крутится», поэтому и
// обещать его модуль не вправе.
enum class MediaKind : std::uint8_t {
    Unknown,       // шина не отвечает: SATA, SAS, SCSI, RAID, USB, Virtual, Unknown
    NonRotational, // вращения нет по определению шины: NVMe, SD (включая MMC)
};

// Стабильное имя для лога и JSON (не локализуется).
[[nodiscard]] const char* mediaKindName(MediaKind kind) noexcept;

// Тип носителя по шине. MediaKind::Unknown — не «жёсткий диск», а «шина не
// отвечает»: именно этот отказ отвечает и SATA, и SAS, и SCSI, и RAID, и USB,
// и виртуальные диски.
[[nodiscard]] MediaKind mediaKindFor(core::BusType bus) noexcept;

// Каким IOCTL вообще имеет смысл спрашивать SMART (SPEC §6.2: «ATA
// IOCTL_SMART_RCV_DRIVE_DATA (feature 0xD0, 30 атрибутов + SMART_GET_VERSION),
// NVMe через IOCTL_SCSI_MINIPORT + IOCTL_SCSI_PASS_THROUGH (CDB 0x06 / NVMe log
// page 0x02)»).
enum class SmartTransport : std::uint8_t {
    None,               // SMART здесь ждать нечего: флеш, виртуальный диск, шина неизвестна
    Ata,                // IOCTL_SMART_RCV_DRIVE_DATA: SATA/ATA/ATAPI
    ScsiPassThrough,    // IOCTL_SCSI_MINIPORT + IOCTL_SCSI_PASS_THROUGH
};

// Стабильное имя для лога и JSON (не локализуется).
[[nodiscard]] const char* smartTransportName(SmartTransport transport) noexcept;

// Транспорт по core::BusType. Особые случаи, о которых стоит знать заранее:
//   * SAS → ScsiPassThrough, а не Ata: у SRB-дисков ATA-команда доходит только
//     через SCSI PASS THROUGH с ATA passthrough;
//   * USB → ScsiPassThrough по той же причине, но отказ здесь — норма, а не
//     неисправность: большинство мостов SMART не пересылают. Отказ значит
//     «данных нет» (§6.4), а не «диск сломан»;
//   * RAID → ScsiPassThrough: контроллер может пропустить команду, а может и
//     нет. Тот же договор: не ответил — SMART недоступен;
//   * Unknown → None. Неизвестная шина не должна превращаться в попытку слать
//     SCSI-пакеты во всё подряд: дескриптор не тот.
[[nodiscard]] SmartTransport smartTransportFor(core::BusType bus) noexcept;

// Переносное устройство (USB-мост, картридер). Такое устройство пользователь
// вправе выдернуть, поэтому молча удалять с него файлы нельзя — этот признак
// инвентаризация использует отдельно от поля removable из FR-1 п.2.
[[nodiscard]] bool isExternalBus(core::BusType bus) noexcept;

// Виртуальный диск (Virtual, FileBackedVirtual, Spaces). Настоящих носителей
// за ним нет: SMART и TRIM здесь не имеют смысла, а имя диска в отчёте должно
// это показывать.
[[nodiscard]] bool isVirtualBus(core::BusType bus) noexcept;

// RAID-контроллер. Диск с таким типом шины — верхушка массива, а не носитель:
// ёмкости физических носителей считаются на других дисках, поэтому в отчёте
// это помечается отдельно.
[[nodiscard]] bool isRaidBus(core::BusType bus) noexcept;

// Сводка свойств: всё, что модуль умеет сказать о шине, в одной структуре —
// так карточка диска (FR-2) не собирает её из шести вызовов и не может забыть
// половину полей.
struct BusProperties {
    core::BusType bus{core::BusType::Unknown};
    std::uint32_t raw{kBusUnknown};  // значение BusType, как его отдал драйвер
    bool known{};                    // raw есть в таблице STORAGE_BUS_TYPE
    bool vendor{};                   // raw — код вендора (>= 0x80)
    bool external{};                 // USB, SD, MMC
    bool virtualDisk{};              // Virtual, FileBackedVirtual, Spaces
    bool raid{};                     // RAID
    MediaKind media{ MediaKind::Unknown };
    SmartTransport smart{ SmartTransport::None };
    // Стоит ли запрашивать StorageDeviceTrimProperty (FR-1 п.2). «Стоит» — не
    // значит «поддерживается»: ответ приходит из свойства устройства, этот флаг
    // лишь говорит, что у виртуального диска ответ заведомо бессмыслен, а на
    // NVMe и SATA — вполне осмыслен. Мосты USB ответ не пересылают чаще, чем
    // пересылают, поэтому флаг у внешних устройств тоже остаётся включённым:
    // ложное «да» от пересылки лучше, чем ложное «нет» от недоверия.
    bool trimWorthAsking{true};
};

[[nodiscard]] BusProperties busProperties(core::BusType bus) noexcept;

// То же по сырому значению: bus заполняется переводом busTypeFromRaw, поэтому
// известные и вендорские значения не теряются (у них bus == Unknown, но raw и
// vendor — на месте).
[[nodiscard]] BusProperties busPropertiesFromRaw(std::uint32_t raw) noexcept;

}  // namespace mrproper::platform::bus_type
