// Тип шины диска: перевод BusType в core::BusType и свойства из него.
// Спека: §4 FR-1 п.2, §6.2, §6.3. Контракт — в bus_type.hpp.
//
// Единственное место, где появляется <winioctl.h>, — static_assert в начале
// файла. Таблица kBus* объявлена в заголовке числами, и это единственный способ
// убедиться, что она совпадает с установленным Windows SDK: если Microsoft
// сдвинет значение, падать должно на сверке, а не на карточке диска у
// пользователя. Второй копии того же перечисления в <ntddstor.h> касаться
// нельзя: имена перечислителей совпадают, и единица трансляции с обоими
// заголовками не собирается.
#include "bus_type.hpp"

#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace mrproper::platform::bus_type {
namespace {

// ---------------------------------------------------------------------------
// Сверка с установленным SDK
//
// Перечисление STORAGE_BUS_TYPE из <winioctl.h>. Список полный, включая
// значения, которые core::BusType не различает (1394, SSA, Fibre, iSCSI, SCM,
// UFS, Spaces): сверяется таблица, а не перевод.
// ---------------------------------------------------------------------------

static_assert(static_cast<std::uint32_t>(BusTypeUnknown) == kBusUnknown, "MrProper: таблица шин разошлась с SDK (Unknown)");
static_assert(static_cast<std::uint32_t>(BusTypeScsi) == kBusScsi, "MrProper: таблица шин разошлась с SDK (Scsi)");
static_assert(static_cast<std::uint32_t>(BusTypeAtapi) == kBusAtapi, "MrProper: таблица шин разошлась с SDK (Atapi)");
static_assert(static_cast<std::uint32_t>(BusTypeAta) == kBusAta, "MrProper: таблица шин разошлась с SDK (Ata)");
static_assert(static_cast<std::uint32_t>(BusType1394) == kBus1394, "MrProper: таблица шин разошлась с SDK (1394)");
static_assert(static_cast<std::uint32_t>(BusTypeSsa) == kBusSsa, "MrProper: таблица шин разошлась с SDK (Ssa)");
static_assert(static_cast<std::uint32_t>(BusTypeFibre) == kBusFibre, "MrProper: таблица шин разошлась с SDK (Fibre)");
static_assert(static_cast<std::uint32_t>(BusTypeUsb) == kBusUsb, "MrProper: таблица шин разошлась с SDK (Usb)");
static_assert(static_cast<std::uint32_t>(BusTypeRAID) == kBusRaid, "MrProper: таблица шин разошлась с SDK (RAID)");
static_assert(static_cast<std::uint32_t>(BusTypeiScsi) == kBusIScsi, "MrProper: таблица шин разошлась с SDK (iScsi)");
static_assert(static_cast<std::uint32_t>(BusTypeSas) == kBusSas, "MrProper: таблица шин разошлась с SDK (Sas)");
static_assert(static_cast<std::uint32_t>(BusTypeSata) == kBusSata, "MrProper: таблица шин разошлась с SDK (Sata)");
static_assert(static_cast<std::uint32_t>(BusTypeSd) == kBusSd, "MrProper: таблица шин разошлась с SDK (Sd)");
static_assert(static_cast<std::uint32_t>(BusTypeMmc) == kBusMmc, "MrProper: таблица шин разошлась с SDK (Mmc)");
static_assert(static_cast<std::uint32_t>(BusTypeVirtual) == kBusVirtual, "MrProper: таблица шин разошлась с SDK (Virtual)");
static_assert(static_cast<std::uint32_t>(BusTypeFileBackedVirtual) == kBusFileBackedVirtual,
              "MrProper: таблица шин разошлась с SDK (FileBackedVirtual)");
static_assert(static_cast<std::uint32_t>(BusTypeSpaces) == kBusSpaces, "MrProper: таблица шин разошлась с SDK (Spaces)");
static_assert(static_cast<std::uint32_t>(BusTypeNvme) == kBusNvme, "MrProper: таблица шин разошлась с SDK (Nvme)");
static_assert(static_cast<std::uint32_t>(BusTypeSCM) == kBusScm, "MrProper: таблица шин разошлась с SDK (SCM)");
static_assert(static_cast<std::uint32_t>(BusTypeUfs) == kBusUfs, "MrProper: таблица шин разошлась с SDK (Ufs)");
static_assert(static_cast<std::uint32_t>(BusTypeMax) == kBusMax, "MrProper: таблица шин разошлась с SDK (Max)");
static_assert(static_cast<std::uint32_t>(BusTypeMaxReserved) == kBusMaxReserved,
              "MrProper: таблица шин разошлась с SDK (MaxReserved)");

// Таблица имён и подписей. Порядок — по значению, поэтому поиск линейный по
// двадцати записям не выходит за рамки одной строки кеша; сортировка ради
// бинарного поиска дала бы двадцать строк данных там, где хватает switch.
struct RawBusEntry {
    std::uint32_t raw;
    const char* name;   // имя перечислителя Windows, как в winioctl.h
    const char* label;  // короткий технический токен для отчёта и лога
};

constexpr RawBusEntry kRawBusTable[] = {
    { kBusUnknown, "BusTypeUnknown", "Unknown" },
    { kBusScsi, "BusTypeScsi", "SCSI" },
    { kBusAtapi, "BusTypeAtapi", "ATA (ATAPI)" },
    { kBusAta, "BusTypeAta", "ATA" },
    { kBus1394, "BusType1394", "IEEE 1394" },
    { kBusSsa, "BusTypeSsa", "SSA" },
    { kBusFibre, "BusTypeFibre", "Fibre Channel" },
    { kBusUsb, "BusTypeUsb", "USB" },
    { kBusRaid, "BusTypeRAID", "RAID" },
    { kBusIScsi, "BusTypeiScsi", "iSCSI" },
    { kBusSas, "BusTypeSas", "SAS" },
    { kBusSata, "BusTypeSata", "SATA" },
    { kBusSd, "BusTypeSd", "SD" },
    { kBusMmc, "BusTypeMmc", "MMC" },
    { kBusVirtual, "BusTypeVirtual", "Virtual" },
    { kBusFileBackedVirtual, "BusTypeFileBackedVirtual", "File-backed virtual" },
    { kBusSpaces, "BusTypeSpaces", "Storage Spaces" },
    { kBusNvme, "BusTypeNvme", "NVMe" },
    { kBusScm, "BusTypeSCM", "SCM" },
    { kBusUfs, "BusTypeUfs", "UFS" },
};

// Запись по сырому значению либо nullptr. Значения 0x14 и 0x7F в таблицу не
// входят намеренно: это концы диапазона, а не шины.
[[nodiscard]] constexpr const RawBusEntry* findRawBus(std::uint32_t raw) noexcept {
    for (const RawBusEntry& entry : kRawBusTable) {
        if (entry.raw == raw) return &entry;
    }
    return nullptr;
}

// Сборка BusProperties. Один набор правил на оба входа: перевод из raw и
// разбор уже готового core::BusType отличаются только тем, откуда взялись
// bus, raw, known и vendor.
[[nodiscard]] BusProperties makeProperties(core::BusType bus, std::uint32_t raw, bool known, bool vendor) noexcept {
    BusProperties properties;
    properties.bus = bus;
    properties.raw = raw;
    properties.known = known;
    properties.vendor = vendor;
    properties.external = isExternalBus(bus);
    properties.virtualDisk = isVirtualBus(bus);
    properties.raid = isRaidBus(bus);
    properties.media = mediaKindFor(bus);
    properties.smart = smartTransportFor(bus);
    // У виртуального диска ответ про TRIM бессмысленен по существу, а не
    // «обычно бесполезен», поэтому там спрашивать не стоит. В остальных
    // случаях спрашивать надо: решение принимает свойство устройства
    // (StorageDeviceTrimProperty, FR-1 п.2), а не шина.
    properties.trimWorthAsking = !properties.virtualDisk;
    return properties;
}

}  // namespace

core::BusType busTypeFromRaw(std::uint32_t raw) noexcept {
    switch (raw) {
        // Совпадения поимённые: SCSI, USB, SAS, SATA, SD, RAID, Virtual.
        case kBusScsi: return core::BusType::Scsi;
        // Без этого случая NVMe молча уезжал в Unknown: значение есть в
        // таблице, а case не был написан. Компилятор такое не ловит — ловит
        // только сверка на живом значении BusType, пришедшем от драйвера.
        case kBusNvme: return core::BusType::Nvme;
        case kBusUsb: return core::BusType::Usb;
        case kBusSas: return core::BusType::Sas;
        case kBusSata: return core::BusType::Sata;
        case kBusSd: return core::BusType::Sd;
        case kBusRaid: return core::BusType::Raid;
        case kBusVirtual: return core::BusType::Virtual;
        // Одно ATA-семейство. Отдельного значения для ATAPI в core::BusType
        // нет, а разводить «оптику» и «жёсткий диск» в модели §6.3 не
        // просили. Исходное значение остаётся видно в describeRawBus.
        case kBusAta:
        case kBusAtapi: return core::BusType::Sata;
        // Виртуальные диски: файл-подложка и пул Storage Spaces для
        // приложения то же самое, что и Virtual. Физические носители под пулом
        // перечисляются как отдельные диски — FR-1 обходит их сам.
        case kBusFileBackedVirtual:
        case kBusSpaces: return core::BusType::Virtual;
        // Картридер отдаёт BusTypeMmc, а «SD» в модели — то же устройство.
        case kBusMmc: return core::BusType::Sd;
        // 1394, SSA, Fibre, iSCSI, SCM, UFS, BusTypeUnknown и всё, чего нет в
        // таблице: в core::BusType (§6.3) для них значений нет, и выдумывать
        // свои нельзя. Точное имя при этом остаётся доступно через
        // rawBusName/describeRawBus.
        default: return core::BusType::Unknown;
    }
}

bool isKnownRawBus(std::uint32_t raw) noexcept {
    // 0x00..0x13 — все значения таблицы, включая сам BusTypeUnknown: «драйвер
    // сказал, что не знает» и «драйвер сказал ерунду» — разные случаи, и оба
    // известны. kBusMax (0x14) и kBusMaxReserved (0x7F) — концы диапазона.
    return raw <= kBusUfs;
}

bool isVendorRawBus(std::uint32_t raw) noexcept {
    return raw >= kBusVendorFirst;
}

const char* rawBusName(std::uint32_t raw) noexcept {
    const RawBusEntry* entry = findRawBus(raw);
    return entry != nullptr ? entry->name : nullptr;
}

std::string describeRawBus(std::uint32_t raw) {
    char code[16] = {};
    // «0x%02X» печатает и 0x11, и 0x1F85 — ширину в два знака задаёт минимум,
    // а не предел, поэтому DWORD из дескриптора устройства не обрезается.
    std::snprintf(code, sizeof(code), "0x%02X", raw);

    std::string out = "BusType=";
    out += code;

    if (const RawBusEntry* entry = findRawBus(raw); entry != nullptr) {
        out += " (";
        out += entry->name;
        out += ", ";
        out += entry->label;
        out += ")";
        return out;
    }

    if (raw == kBusMax) {
        out += " (BusTypeMax: значений шины дальше нет)";
    } else if (raw == kBusMaxReserved) {
        out += " (BusTypeMaxReserved: конец диапазона Microsoft)";
    } else if (isVendorRawBus(raw)) {
        out += " (код вендора, значение >= 0x80)";
    } else {
        out += " (нет такого значения в STORAGE_BUS_TYPE)";
    }
    return out;
}

const char* mediaKindName(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::NonRotational: return "non-rotational";
        case MediaKind::Unknown: break;
    }
    return "unknown";
}

const char* smartTransportName(SmartTransport transport) noexcept {
    switch (transport) {
        case SmartTransport::Ata: return "ata";
        case SmartTransport::ScsiPassThrough: return "scsi-pass-through";
        case SmartTransport::None: break;
    }
    return "none";
}

bool isExternalBus(core::BusType bus) noexcept {
    switch (bus) {
        case core::BusType::Usb:
        case core::BusType::Sd: return true;
        case core::BusType::Unknown:
        case core::BusType::Sata:
        case core::BusType::Nvme:
        case core::BusType::Scsi:
        case core::BusType::Sas:
        case core::BusType::Virtual:
        case core::BusType::Raid: break;
    }
    return false;
}

bool isVirtualBus(core::BusType bus) noexcept {
    return bus == core::BusType::Virtual;
}

bool isRaidBus(core::BusType bus) noexcept {
    return bus == core::BusType::Raid;
}

MediaKind mediaKindFor(core::BusType bus) noexcept {
    switch (bus) {
        // Вращения нет по определению этих шин.
        case core::BusType::Nvme:
        case core::BusType::Sd: return MediaKind::NonRotational;
        // Про остальные шины (включая USB и виртуальные) тип носителя по шине
        // не определяется: за мостом USB стоит и флешка, и внешний HDD.
        // «Вращается» не возвращается никогда намеренно: утверждать это по
        // шине нельзя, а выдумывать значение, которое никто не производит, —
        // тоже.
        case core::BusType::Unknown:
        case core::BusType::Sata:
        case core::BusType::Usb:
        case core::BusType::Scsi:
        case core::BusType::Sas:
        case core::BusType::Virtual:
        case core::BusType::Raid: break;
    }
    return MediaKind::Unknown;
}

SmartTransport smartTransportFor(core::BusType bus) noexcept {
    switch (bus) {
        // IOCTL_SMART_RCV_DRIVE_DATA: команда ATA отдаёт адаптер SATA/ATA.
        case core::BusType::Sata: return SmartTransport::Ata;
        // IOCTL_SCSI_MINIPORT + IOCTL_SCSI_PASS_THROUGH. NVMe — по CDB 0x06 и
        // log page 0x02 (§6.2), SAS — потому что ATA-команда доходит только
        // через passthrough, SCSI и RAID — по той же причине, USB — потому
        // что иначе не спросить ничего (отказ у мостов — норма).
        case core::BusType::Nvme:
        case core::BusType::Scsi:
        case core::BusType::Sas:
        case core::BusType::Raid:
        case core::BusType::Usb: return SmartTransport::ScsiPassThrough;
        // Флеш-носителям SMART не полагается, за виртуальным диском его нет,
        // а неизвестная шина — это «не слать SCSI-пакеты во всё подряд».
        case core::BusType::Unknown:
        case core::BusType::Sd:
        case core::BusType::Virtual: break;
    }
    return SmartTransport::None;
}

BusProperties busProperties(core::BusType bus) noexcept {
    // Исходного значения здесь нет и быть не может: у core::BusType его не
    // хранит. known означает «шина названа», vendor — false, потому что код
    // вендора до перевода в модель не доходит.
    return makeProperties(bus, kBusUnknown, bus != core::BusType::Unknown, false);
}

BusProperties busPropertiesFromRaw(std::uint32_t raw) noexcept {
    return makeProperties(busTypeFromRaw(raw), raw, isKnownRawBus(raw), isVendorRawBus(raw));
}

}  // namespace mrproper::platform::bus_type
