// Разметка диска и разделы: IOCTL_DISK_GET_DRIVE_LAYOUT_EX (SPEC §4 FR-1 п.4).
// Разбор и преобразования описаны в layout.hpp; здесь только Win32-часть.
#include "layout.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "devices.hpp"

namespace mrproper::platform {
namespace {

// --- Константы разбора -------------------------------------------------------

// Значения PARTITION_STYLE (winioctl.h). Тот же набор приходит в
// PARTITION_INFORMATION_EX::PartitionStyle — по нему выбирается ветка union.
constexpr std::uint32_t kPartStyleMbr = 0;
constexpr std::uint32_t kPartStyleGpt = 1;
constexpr std::uint32_t kPartStyleRaw = 2;

// Ответ IOCTL — буфер переменного размера: 128 разделов по ~144 байта занимают
// меньше 20 КиБ. Потолок 1 МиБ — страховка от вендорного драйвера, который отдаёт
// мусор: бесконечный рост буфера на «сломанном» устройстве ровно так и роняет
// процесс, а §5 это запрещает.
constexpr std::size_t kInitialLayoutBytes = 4096;
constexpr std::size_t kMaxLayoutBytes = 1u << 20;
constexpr unsigned kMaxGrowSteps = 6;

// GPT-атрибуты (winioctl.h, PARTITION_INFORMATION_GPT::Attributes).
constexpr std::uint64_t kGptPlatformRequired = 0x0000000000000001ull;
constexpr std::uint64_t kGptNoBlockIoProtocol = 0x0000000000000002ull;
constexpr std::uint64_t kGptLegacyBiosBootable = 0x0000000000000004ull;
constexpr std::uint64_t kGptNoDriveLetter = 0x8000000000000000ull;
constexpr std::uint64_t kGptHidden = 0x4000000000000000ull;
constexpr std::uint64_t kGptShadowCopy = 0x2000000000000000ull;
constexpr std::uint64_t kGptReadOnly = 0x1000000000000000ull;
constexpr std::uint64_t kGptOffline = 0x0800000000000000ull;
constexpr std::uint64_t kGptDax = 0x0400000000000000ull;
constexpr std::uint64_t kGptService = 0x0200000000000000ull;

// Диапазон скрытых типов MBR (варианты 0x02..0x0E со старшим битом). Сам бит
// «скрыт» в таблице MBR есть, но IOCTL_DISK_GET_DRIVE_LAYOUT_EX его не отдаёт,
// поэтому скрытность для MBR выводится только отсюда. WinRE (0x27) в диапазон не
// входит: он помечается как Recovery, а скрытность видна по отсутствию буквы на
// шаге привязки томов (FR-1 п.5).
constexpr std::uint8_t kMbrHiddenFirst = 0x12;
constexpr std::uint8_t kMbrHiddenLast = 0x1E;

// core::Guid из компонентов DEFINE_GUID. Порядок байт — текстовый, как в строке
// GUID, поэтому хвост копируется напрямую.
constexpr core::Guid makeGuid(std::uint32_t data1, std::uint16_t data2, std::uint16_t data3,
                              const std::array<std::uint8_t, 8>& tail) noexcept {
    return core::Guid{
        static_cast<std::uint8_t>((data1 >> 24) & 0xFFu),
        static_cast<std::uint8_t>((data1 >> 16) & 0xFFu),
        static_cast<std::uint8_t>((data1 >> 8) & 0xFFu),
        static_cast<std::uint8_t>(data1 & 0xFFu),
        static_cast<std::uint8_t>((data2 >> 8) & 0xFFu),
        static_cast<std::uint8_t>(data2 & 0xFFu),
        static_cast<std::uint8_t>((data3 >> 8) & 0xFFu),
        static_cast<std::uint8_t>(data3 & 0xFFu),
        tail[0], tail[1], tail[2], tail[3], tail[4], tail[5], tail[6], tail[7],
    };
}

struct KnownGptType {
    core::Guid type;
    const char* name;
    core::PartitionKind kind;
};

// Типы разделов, известные драйверам дисков и диспетчерам томов. Значения — из
// Windows Kits shared/diskguid.h (PARTITION_*_GUID). Объявлять их через
// initguid.h здесь нельзя: символ пришлось бы дублировать с другим .cpp слоя
// platform (два определения одной константы — LNK2005), либо тянуть extern-объявление
// и надеяться на uuid.lib. Константы локальные, а источник истины один — сам SDK.
constexpr KnownGptType kKnownGptTypes[] = {
    // PARTITION_BASIC_DATA_GUID
    {makeGuid(0xEBD0A0A2u, 0xB9E5u, 0x4433u, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}),
     "Basic data partition", core::PartitionKind::BasicData},
    // PARTITION_SYSTEM_GUID
    {makeGuid(0xC12A7328u, 0xF81Fu, 0x11D2u, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}),
     "EFI system partition", core::PartitionKind::Efi},
    // PARTITION_MSFT_RESERVED_GUID
    {makeGuid(0xE3C9E316u, 0x0B5Cu, 0x4DB8u, {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}),
     "Microsoft reserved space (MSR)", core::PartitionKind::Msr},
    // PARTITION_MSFT_RECOVERY_GUID
    {makeGuid(0xDE94BBA4u, 0x06D1u, 0x4D40u, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}),
     "Microsoft recovery partition", core::PartitionKind::Recovery},
    // PARTITION_WINDOWS_SYSTEM_GUID — служебная системная область Windows.
    {makeGuid(0x57434F53u, 0xE3E3u, 0x4631u, {0xA5, 0xC5, 0x26, 0xD2, 0x24, 0x38, 0x73, 0xAA}),
     "Windows system partition", core::PartitionKind::System},
    // PARTITION_MAIN_OS_GUID
    {makeGuid(0x57434F53u, 0x8F45u, 0x405Eu, {0x8A, 0x23, 0x18, 0x6D, 0x8A, 0x43, 0x30, 0xD3}),
     "Main OS partition", core::PartitionKind::System},
    // PARTITION_OS_DATA_GUID — данные ОС, служебной не является.
    {makeGuid(0x57434F53u, 0x23F2u, 0x44D5u, {0xA8, 0x30, 0x67, 0xBB, 0xDA, 0xA6, 0x09, 0xF9}),
     "OS data partition", core::PartitionKind::BasicData},
    // PARTITION_PRE_INSTALLED_GUID
    {makeGuid(0x57434F53u, 0x7FE0u, 0x4196u, {0x9B, 0x42, 0x42, 0x7B, 0x51, 0x64, 0x34, 0x84}),
     "Pre-installed applications partition", core::PartitionKind::Oem},
    // PARTITION_BSP_GUID
    {makeGuid(0x57434F53u, 0x4DF9u, 0x45B9u, {0x8E, 0x9E, 0x23, 0x70, 0xF0, 0x06, 0x45, 0x7C}),
     "BSP partition", core::PartitionKind::Oem},
    // PARTITION_DPP_GUID
    {makeGuid(0x57434F53u, 0x94CBu, 0x43F0u, {0xA5, 0x33, 0xD7, 0x3C, 0x10, 0xCF, 0xA5, 0x7D}),
     "DPP partition", core::PartitionKind::Oem},
    // PARTITION_PATCH_GUID
    {makeGuid(0x8967A686u, 0x96AAu, 0x6AA8u, {0x95, 0x89, 0xA8, 0x42, 0x56, 0x54, 0x10, 0x90}),
     "Patch partition", core::PartitionKind::Oem},
    // PARTITION_LEGACY_BL_GUID — загрузчик вне GPT (BIOS).
    {makeGuid(0x424CA0E2u, 0x7CB2u, 0x4FB9u, {0x81, 0x43, 0xC5, 0x2A, 0x99, 0x39, 0x8B, 0xC6}),
     "Legacy boot loader partition", core::PartitionKind::Oem},
    // PARTITION_LEGACY_BL_GUID_BACKUP
    {makeGuid(0x424C3E6Cu, 0xD79Fu, 0x49CBu, {0x93, 0x5D, 0x36, 0xD7, 0x14, 0x67, 0xA2, 0x88}),
     "Legacy boot loader backup partition", core::PartitionKind::Oem},
    // PARTITION_LDM_METADATA_GUID — динамический диск.
    {makeGuid(0x5808C8AAu, 0x7E8Fu, 0x42E0u, {0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3}),
     "LDM metadata partition", core::PartitionKind::Reserved},
    // PARTITION_LDM_DATA_GUID
    {makeGuid(0xAF9B60A0u, 0x1431u, 0x4F62u, {0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD}),
     "LDM data partition", core::PartitionKind::Reserved},
    // PARTITION_SPACES_GUID
    {makeGuid(0xE75CAF8Fu, 0xF680u, 0x4CEEu, {0xAF, 0xA3, 0xB0, 0x01, 0xE5, 0x6E, 0xFC, 0x2D}),
     "Storage Spaces protective partition", core::PartitionKind::Reserved},
    // PARTITION_SPACES_DATA_GUID
    {makeGuid(0xE7ADDCB4u, 0xDC34u, 0x4539u, {0x9A, 0x76, 0xEB, 0xBD, 0x07, 0xBE, 0x6F, 0x7E}),
     "Storage Spaces data partition", core::PartitionKind::Reserved},
    // PARTITION_MSFT_SNAPSHOT_GUID — хранилище VSS, пользовательских данных нет.
    {makeGuid(0xCADDEBF1u, 0x4400u, 0x4DE8u, {0xB1, 0x03, 0x12, 0x11, 0x7D, 0xCF, 0x3C, 0xCF}),
     "Microsoft shadow copy partition", core::PartitionKind::Reserved},
    // PARTITION_ENTRY_UNUSED_GUID — пустая запись таблицы.
    {makeGuid(0x00000000u, 0x0000u, 0x0000u, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
     "Unused entry", core::PartitionKind::Unknown},
};

struct KnownMbrType {
    std::uint8_t type;
    const char* name;
    core::PartitionKind kind;
};

// Байты типов MBR. Тип, которого нет в списке, читается как Unknown, но в отчёте
// всё равно печатается как 0xNN — «не знаю» и «нечего не знать» это разные вещи.
constexpr KnownMbrType kKnownMbrTypes[] = {
    {0x00, "Empty entry", core::PartitionKind::Reserved},
    {0x01, "FAT12 (less than 32 MB)", core::PartitionKind::BasicData},
    {0x02, "XENIX root (hidden)", core::PartitionKind::Reserved},
    {0x04, "FAT16 (less than 32 MB)", core::PartitionKind::BasicData},
    {0x05, "Extended partition (CHS)", core::PartitionKind::Reserved},
    {0x06, "FAT16 (32 MB - 2 GB)", core::PartitionKind::BasicData},
    {0x07, "NTFS / exFAT / high FAT32", core::PartitionKind::BasicData},
    {0x0B, "FAT32 (CHS)", core::PartitionKind::BasicData},
    {0x0C, "FAT32 (LBA)", core::PartitionKind::BasicData},
    {0x0E, "FAT16 (LBA)", core::PartitionKind::BasicData},
    {0x0F, "Extended partition (LBA)", core::PartitionKind::Reserved},
    {0x11, "FAT12 (hidden)", core::PartitionKind::BasicData},
    {0x12, "Compaq diagnostics (hidden)", core::PartitionKind::Reserved},
    {0x14, "FAT16 less than 32 MB (hidden)", core::PartitionKind::BasicData},
    {0x16, "FAT16 (hidden)", core::PartitionKind::BasicData},
    {0x17, "NTFS / exFAT (hidden)", core::PartitionKind::BasicData},
    {0x1B, "FAT32 (hidden, CHS)", core::PartitionKind::BasicData},
    {0x1C, "FAT32 (hidden, LBA)", core::PartitionKind::BasicData},
    {0x1E, "FAT16 (hidden, LBA)", core::PartitionKind::BasicData},
    {0x27, "Windows Recovery Environment", core::PartitionKind::Recovery},
    {0x42, "Windows Dynamic Disk (LDM)", core::PartitionKind::Reserved},
    {0x82, "Linux swap", core::PartitionKind::Reserved},
    {0x83, "Linux (ext2/3/4, XFS, ...)", core::PartitionKind::BasicData},
    {0x8E, "Linux LVM", core::PartitionKind::BasicData},
    {0x8F, "Linux LVM (stripe set)", core::PartitionKind::BasicData},
    {0xA5, "FreeBSD / Unix (FFS)", core::PartitionKind::BasicData},
    {0xA6, "OpenBSD", core::PartitionKind::BasicData},
    {0xA9, "NetBSD / BSDI FFS", core::PartitionKind::BasicData},
    {0xAB, "Darwin (HFS+)", core::PartitionKind::BasicData},
    {0xAF, "Darwin (HFS+)", core::PartitionKind::BasicData},
    {0xB7, "BSDI FFS", core::PartitionKind::BasicData},
    {0xC5, "DragonFly BSD", core::PartitionKind::BasicData},
    {0xEE, "GPT protective MBR", core::PartitionKind::Reserved},
    {0xEF, "EFI system partition", core::PartitionKind::Efi},
    {0xFD, "Linux RAID", core::PartitionKind::Reserved},
};

const KnownGptType* findKnownGptType(const core::Guid& type) noexcept {
    for (const KnownGptType& known : kKnownGptTypes) {
        if (known.type == type) return &known;
    }
    return nullptr;
}

const KnownMbrType* findKnownMbrType(std::uint8_t type) noexcept {
    for (const KnownMbrType& known : kKnownMbrTypes) {
        if (known.type == type) return &known;
    }
    return nullptr;
}

// --- Преобразования ----------------------------------------------------------

// Win32 GUID (little-endian Data1..Data3) → core::Guid в текстовом порядке.
core::Guid guidFromNative(const GUID& native) noexcept {
    return makeGuid(native.Data1, native.Data2, native.Data3,
                    {native.Data4[0], native.Data4[1], native.Data4[2], native.Data4[3],
                     native.Data4[4], native.Data4[5], native.Data4[6], native.Data4[7]});
}

// Нулевой GUID в нативной раскладке. Нулевой PartitionType означает «тип не
// задан» (в том числе пустую запись таблицы GPT), нулевой PartitionId — «нет
// идентификатора»: в обоих случаях поле оставляем незаполненным, чтобы в отчёте
// не светились нули как настоящие значения.
bool isZeroNativeGuid(const GUID& native) noexcept {
    if (native.Data1 != 0 || native.Data2 != 0 || native.Data3 != 0) return false;
    for (const BYTE byte : native.Data4) {
        if (byte != 0) return false;
    }
    return true;
}

constexpr char kHexDigits[] = "0123456789ABCDEF";

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// UTF-16 → UTF-8. name — фиксированный массив WCHAR[36], а не C-строка, поэтому
// длина ищется по первому нулю; дальше работает переносимый core::toUtf8.
std::string narrowGptName(const wchar_t* name, std::size_t capacity) {
    std::size_t length = 0;
    while (length < capacity && name[length] != L'\0') ++length;
    return core::toUtf8(std::wstring_view(name, length));
}

// UTF-8 → UTF-16 для CreateFileW. Путь приходит из модуля устройств, который
// хранит его в UTF-8 (§6.3), но на всякий случай есть запасной вариант через
// системную кодовую страницу: лучше открыть путь в ACP, чем не открыть вовсе.
std::wstring widenUtf8(std::string_view text) {
    if (text.empty() || text.size() > 0x7FFFFFFFu) return {};
    const int size = static_cast<int>(text.size());
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int needed = MultiByteToWideChar(CP_UTF8, flags, text.data(), size, nullptr, 0);
    if (needed <= 0) {
        codePage = CP_ACP;
        flags = 0;
        needed = MultiByteToWideChar(CP_ACP, flags, text.data(), size, nullptr, 0);
    }
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(codePage, flags, text.data(), size, wide.data(), needed) <= 0) return {};
    return wide;
}

// Для "\\.\PhysicalDriveN" путь готов как есть. Для device path из
// SetupDiGetDeviceInterfaceDetailW ("\\?\X#&…") завершающий '\' обязателен, иначе
// драйвер отвечает ERROR_INVALID_FUNCTION; путь тома ("\\?\Volume{…}\") уже
// оканчивается на '\'. Буквы дисков ("\\.\C:") не трогаем: слэш там лишний.
std::wstring devicePathToWide(std::string_view path) {
    std::wstring wide = widenUtf8(path);
    constexpr std::string_view kNtPrefix = "\\\\?\\";
    const bool isNtPath = path.size() > kNtPrefix.size() && path.substr(0, kNtPrefix.size()) == kNtPrefix;
    if (isNtPath && !wide.empty() && wide.back() != L'\\') wide.push_back(L'\\');
    return wide;
}

std::uint64_t toBytes(const LARGE_INTEGER& value) noexcept {
    // Отрицательное значение — мусор от драйвера: такие разделы отсекает
    // finalizeLayout (consistent == false), а не сдвиг на 2^64.
    return value.QuadPart < 0 ? 0 : static_cast<std::uint64_t>(value.QuadPart);
}

// --- RAII-обёртки (ADR-001) --------------------------------------------------

// HANDLE с закрытием в деструкторе. Копирования нет: дескриптор у процесса один.
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

    HANDLE release() noexcept {
        HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

// Буфер под ответ IOCTL. Элемент uint64_t — чтобы адрес был выровнен так же, как
// ответ с LARGE_INTEGER, и ответ можно было читать прямо из буфера, без memcpy.
class LayoutBuffer {
public:
    explicit LayoutBuffer(std::size_t bytes) noexcept
        : words_((bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t)) {}

    void* data() noexcept { return words_.data(); }
    const void* data() const noexcept { return words_.data(); }
    std::size_t size() const noexcept { return words_.size() * sizeof(std::uint64_t); }

private:
    std::vector<std::uint64_t> words_;
};

// --- Разбор ответа -----------------------------------------------------------

LayoutPartition makePartition(const PARTITION_INFORMATION_EX& entry) {
    LayoutPartition part;
    part.number = entry.PartitionNumber;
    part.style = layoutStyleFromRaw(static_cast<std::uint32_t>(entry.PartitionStyle));
    part.offsetBytes = toBytes(entry.StartingOffset);
    part.lengthBytes = toBytes(entry.PartitionLength);
    part.rewritePartition = entry.RewritePartition != FALSE;

    switch (part.style) {
        case LayoutStyle::Gpt:
            part.hasGptType = !isZeroNativeGuid(entry.Gpt.PartitionType);
            part.gptType = guidFromNative(entry.Gpt.PartitionType);
            part.hasPartitionId = !isZeroNativeGuid(entry.Gpt.PartitionId);
            part.partitionId = guidFromNative(entry.Gpt.PartitionId);
            part.gptName = narrowGptName(entry.Gpt.Name, ARRAYSIZE(entry.Gpt.Name));
            part.attributes = gptAttributesFromRaw(entry.Gpt.Attributes, entry.IsServicePartition != FALSE);
            part.kind = kindFromGptType(part.gptType);
            break;
        case LayoutStyle::Mbr:
            part.mbrType = entry.Mbr.PartitionType;
            // У MBR это сигнатура раздела, а не GUID типа: смысл другой, поэтому
            // в gptType она не попадает.
            part.hasPartitionId = !isZeroNativeGuid(entry.Mbr.PartitionId);
            part.partitionId = guidFromNative(entry.Mbr.PartitionId);
            part.attributes =
                mbrAttributesFromRaw(part.mbrType, entry.Mbr.BootIndicator != FALSE,
                                     entry.Mbr.RecognizedPartition != FALSE, entry.Mbr.HiddenSectors);
            part.kind = kindFromMbrType(part.mbrType);
            break;
        default:
            // PARTITION_STYLE_RAW и неведомое значение ветки union: смещение и
            // длина остаются, тип остаётся Unknown — выдумывать нечего.
            part.kind = core::PartitionKind::Unknown;
            break;
    }
    return part;
}

bool parseLayout(const LayoutBuffer& buffer, std::size_t returned, LayoutReadResult& result) {
    const std::size_t headerBytes = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
    const std::size_t entryBytes = sizeof(PARTITION_INFORMATION_EX);
    if (returned < headerBytes) {
        result.winError = ERROR_INVALID_DATA;
        return false;
    }

    const auto* layout = static_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
    const auto* base = static_cast<const std::uint8_t*>(buffer.data());
    result.style = layoutStyleFromRaw(layout->PartitionStyle);

    if (layout->PartitionStyle == kPartStyleGpt) {
        result.hasGptDiskId = !isZeroNativeGuid(layout->Gpt.DiskId);
        result.gptDiskId = guidFromNative(layout->Gpt.DiskId);
        result.usableStartOffset = toBytes(layout->Gpt.StartingUsableOffset);
        result.usableLength = toBytes(layout->Gpt.UsableLength);
        result.maxPartitionCount = layout->Gpt.MaxPartitionCount;
    } else if (layout->PartitionStyle == kPartStyleMbr) {
        result.mbrSignature = layout->Mbr.Signature;
        result.mbrChecksum = layout->Mbr.CheckSum;
    }

    // Счётчик разделов сверяем с тем, что реально пришло: вендорный драйвер может
    // приписать больше, и верить ему нельзя, но и падать из-за этого нельзя (§5) —
    // читаем столько записей, сколько поместилось.
    const std::size_t capacityEntries = (returned - headerBytes) / entryBytes;
    std::size_t count = layout->PartitionCount;
    if (count > capacityEntries) count = capacityEntries;

    result.partitions.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto* entry = reinterpret_cast<const PARTITION_INFORMATION_EX*>(base + headerBytes +
                                                                                 entryBytes * index);
        result.partitions.push_back(makePartition(*entry));
    }
    return true;
}

// --- Win32 -------------------------------------------------------------------

// Открытие устройства на чтение. IOCTL_DISK_GET_DRIVE_LAYOUT_EX объявлен как
// FILE_ANY_ACCESS, поэтому GENERIC_READ — с запасом, а FILE_READ_ATTRIBUTES —
// запасной вариант для машин, где открытие \\.\PhysicalDriveN на чтение требует
// прав (выключенный фильтр диска). Ошибка возвращается в error.
HANDLE openDiskForRead(const std::wstring& path, std::uint32_t& error) noexcept {
    constexpr DWORD kAccessModes[] = {GENERIC_READ, FILE_READ_ATTRIBUTES};
    error = ERROR_FILE_NOT_FOUND;
    for (const DWORD access : kAccessModes) {
        const HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            error = ERROR_SUCCESS;
            return handle;
        }
        error = static_cast<std::uint32_t>(GetLastError());
        if (error != ERROR_ACCESS_DENIED) break;  // более слабый режим тут не поможет
    }
    return INVALID_HANDLE_VALUE;
}

// IOCTL с растущим буфером. Драйвер сообщает нужный размер через ERROR_MORE_DATA
// (старые драйверы) либо ERROR_INSUFFICIENT_BUFFER, иногда неполным, поэтому берём
// максимум из «вдвое больше» и «сколько сказал драйвер».
bool readLayoutIoctl(HANDLE handle, LayoutReadResult& result) {
    const std::size_t headerBytes = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
    const std::size_t entryBytes = sizeof(PARTITION_INFORMATION_EX);
    std::size_t capacity = std::max(kInitialLayoutBytes, headerBytes + entryBytes * 4);

    for (unsigned step = 0; step <= kMaxGrowSteps; ++step) {
        if (capacity > kMaxLayoutBytes) {
            result.winError = ERROR_MORE_DATA;
            return false;
        }
        LayoutBuffer buffer(capacity);
        DWORD returned = 0;
        if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
            const auto error = static_cast<std::uint32_t>(GetLastError());
            if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) {
                result.winError = error;
                return false;
            }
            capacity = std::max(capacity * 2, static_cast<std::size_t>(returned));
            continue;
        }
        if (returned == 0) {
            result.winError = ERROR_INVALID_DATA;  // драйвер ответил, но данных не отдал
            return false;
        }
        // Драйвер не имеет права вернуть больше, чем размер буфера, но проверка
        // дешевле, чем чтение памяти по чужому указателю.
        return parseLayout(buffer, std::min<std::size_t>(returned, buffer.size()), result);
    }
    result.winError = ERROR_MORE_DATA;
    return false;
}

core::Partition makeCoreUnallocated(const LayoutGap& gap, std::uint32_t index) {
    core::Partition part;
    part.index = index;
    part.offsetBytes = gap.offsetBytes;
    part.lengthBytes = gap.lengthBytes;
    part.kind = core::PartitionKind::Unallocated;
    return part;
}

}  // namespace

// --- Чтение разметки ---------------------------------------------------------
// Путь диска строит devices (единственный владелец этого преобразования).

LayoutReadResult readDriveLayout(int diskNumber, std::uint64_t diskSizeBytes) {
    if (diskNumber < 0) {
        LayoutReadResult result;
        result.diskNumber = diskNumber;
        result.devicePath = devices::physicalDrivePathUtf8(diskNumber);
        result.winError = ERROR_INVALID_PARAMETER;
        return result;
    }
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskNumber);

    LayoutReadResult result;
    result.diskNumber = diskNumber;
    result.devicePath = devices::physicalDrivePathUtf8(diskNumber);
    result.diskSizeBytes = diskSizeBytes;

    ScopedHandle handle(openDiskForRead(path, result.winError));
    if (!handle.valid()) return result;
    if (!readLayoutIoctl(handle.get(), result)) return result;

    finalizeLayout(result);
    result.ok = true;
    return result;
}

LayoutReadResult readDriveLayoutForDevice(std::string_view devicePath, std::uint64_t diskSizeBytes) {
    LayoutReadResult result;
    result.devicePath = std::string(devicePath);
    result.diskSizeBytes = diskSizeBytes;

    if (devicePath.empty()) {
        result.winError = ERROR_INVALID_PARAMETER;
        return result;
    }
    const std::wstring path = devicePathToWide(devicePath);
    if (path.empty()) {
        result.winError = ERROR_INVALID_NAME;
        return result;
    }

    ScopedHandle handle(openDiskForRead(path, result.winError));
    if (!handle.valid()) return result;
    if (!readLayoutIoctl(handle.get(), result)) return result;

    finalizeLayout(result);
    result.ok = true;
    return result;
}

void finalizeLayout(LayoutReadResult& result) {
    // Порядок разделов по смещению — иначе карта диска (FR-2) и golden-дампы
    // отчёта зависят от того, в каком порядке драйвер перечислил записи.
    std::stable_sort(result.partitions.begin(), result.partitions.end(),
                     [](const LayoutPartition& left, const LayoutPartition& right) {
                         if (left.offsetBytes != right.offsetBytes) return left.offsetBytes < right.offsetBytes;
                         return left.lengthBytes < right.lengthBytes;
                     });

    result.gaps.clear();
    result.consistent = true;
    std::uint64_t cursor = 0;
    for (const LayoutPartition& part : result.partitions) {
        if (part.offsetBytes < cursor) {
            // Пересечение: дальше считать промежутки бессмысленно, цифры разделов
            // тоже складывать нельзя. Показываем как есть, но помечаем.
            result.consistent = false;
            result.gaps.clear();
            return;
        }
        if (part.offsetBytes > cursor) {
            result.gaps.push_back(LayoutGap{cursor, part.offsetBytes - cursor});
        }
        const bool overflow = part.lengthBytes > UINT64_MAX - part.offsetBytes;
        cursor = overflow ? UINT64_MAX : part.offsetBytes + part.lengthBytes;
        if (overflow) {
            result.consistent = false;
            result.gaps.clear();
            return;
        }
    }

    if (result.diskSizeBytes == 0) return;  // без размера диска промежутки не считаем
    if (cursor > result.diskSizeBytes) {
        // Раздел вылезает за пределы диска — частая картина на динамических
        // томах и RAID: помечаем и не дорисовываем хвост.
        result.consistent = false;
        return;
    }
    if (cursor < result.diskSizeBytes) {
        result.gaps.push_back(LayoutGap{cursor, result.diskSizeBytes - cursor});
    }
}

// --- Чистый разбор -----------------------------------------------------------

LayoutStyle layoutStyleFromRaw(std::uint32_t raw) noexcept {
    switch (raw) {
        case kPartStyleMbr:
            return LayoutStyle::Mbr;
        case kPartStyleGpt:
            return LayoutStyle::Gpt;
        case kPartStyleRaw:
            return LayoutStyle::Raw;
        default:
            return LayoutStyle::Unknown;
    }
}

const char* layoutStyleName(LayoutStyle style) noexcept {
    switch (style) {
        case LayoutStyle::Mbr:
            return "MBR";
        case LayoutStyle::Gpt:
            return "GPT";
        case LayoutStyle::Raw:
            return "RAW";
        case LayoutStyle::Unknown:
            break;
    }
    return "Unknown";
}

core::PartitionKind kindFromGptType(const core::Guid& type) noexcept {
    const KnownGptType* known = findKnownGptType(type);
    return known != nullptr ? known->kind : core::PartitionKind::Unknown;
}

core::PartitionKind kindFromMbrType(std::uint8_t mbrType) noexcept {
    const KnownMbrType* known = findKnownMbrType(mbrType);
    return known != nullptr ? known->kind : core::PartitionKind::Unknown;
}

const char* gptTypeName(const core::Guid& type) noexcept {
    const KnownGptType* known = findKnownGptType(type);
    return known != nullptr ? known->name : nullptr;
}

std::string gptTypeLabel(const core::Guid& type) {
    if (const char* name = gptTypeName(type); name != nullptr) return name;
    return guidToCanonicalString(type);  // незнакомый тип показываем как есть
}

const char* mbrTypeName(std::uint8_t mbrType) noexcept {
    const KnownMbrType* known = findKnownMbrType(mbrType);
    return known != nullptr ? known->name : nullptr;
}

std::string mbrTypeLabel(std::uint8_t mbrType) {
    if (const char* name = mbrTypeName(mbrType); name != nullptr) return name;
    // Неизвестный тип печатаем байтом: «не знаю» и «нечего не знать» в отчёте —
    // разные вещи, и по 0xNN видно, чего именно модуль не знает.
    std::string text = "0x";
    text.push_back(kHexDigits[(mbrType >> 4) & 0x0F]);
    text.push_back(kHexDigits[mbrType & 0x0F]);
    return text;
}

LayoutAttributes gptAttributesFromRaw(std::uint64_t attributes, bool isServicePartition) noexcept {
    LayoutAttributes parsed;
    parsed.gptRaw = attributes;
    parsed.platformRequired = (attributes & kGptPlatformRequired) != 0;
    parsed.noBlockIoProtocol = (attributes & kGptNoBlockIoProtocol) != 0;
    parsed.bootIndicator = (attributes & kGptLegacyBiosBootable) != 0;
    parsed.noDriveLetter = (attributes & kGptNoDriveLetter) != 0;
    parsed.shadowCopy = (attributes & kGptShadowCopy) != 0;
    parsed.readOnly = (attributes & kGptReadOnly) != 0;
    parsed.offline = (attributes & kGptOffline) != 0;
    parsed.dax = (attributes & kGptDax) != 0;
    parsed.service = isServicePartition || (attributes & kGptService) != 0;
    // PLATFORM_REQUIRED Windows ставит тем разделам, которые прячет по умолчанию
    // (ESP, MSR, системные), поэтому в «скрыт» он входит наравне с HIDDEN.
    parsed.hidden = (attributes & (kGptHidden | kGptPlatformRequired)) != 0;
    return parsed;
}

LayoutAttributes mbrAttributesFromRaw(std::uint8_t mbrType, bool bootIndicator, bool recognized,
                                      std::uint32_t hiddenSectors) noexcept {
    LayoutAttributes parsed;
    parsed.bootIndicator = bootIndicator;
    parsed.recognized = recognized;
    parsed.hidden = mbrType >= kMbrHiddenFirst && mbrType <= kMbrHiddenLast;
    parsed.mbrHiddenSectors = hiddenSectors;
    // readOnly у MBR этим IOCTL не отдаётся: остаётся false, и это честнее, чем
    // «заодно и read-only выставим».
    return parsed;
}

std::string guidToCanonicalString(const core::Guid& guid) {
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < guid.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
        text.push_back(kHexDigits[(guid[index] >> 4) & 0x0F]);
        text.push_back(kHexDigits[guid[index] & 0x0F]);
    }
    return text;
}

std::optional<core::Guid> guidFromCanonicalString(std::string_view text) {
    // Приём строки не должен иметь границ: неизвестный тип раздела приходит из
    // отчёта и из пользовательского ввода, а писать за пределы массива из-за
    // «лишней» пары hex-символов нельзя. Проверка — до записи, не после.
    core::Guid guid{};
    std::size_t written = 0;
    for (const char c : text) {
        if (c == '-' || c == '{' || c == '}' || c == ' ') continue;
        const int digit = hexDigit(c);
        if (digit < 0) return std::nullopt;
        if (written >= guid.size() * 2) return std::nullopt;
        if (written == 0) {
            guid[0] = static_cast<std::uint8_t>(digit);  // старший разряд пары
        } else {
            guid[written - 1] = static_cast<std::uint8_t>((guid[written - 1] << 4) | digit);
        }
        ++written;
    }
    if (written != guid.size() * 2) return std::nullopt;
    return guid;
}

// --- Мост в модель -----------------------------------------------------------

std::vector<core::Partition> toCorePartitions(const LayoutReadResult& result, bool includeUnallocated) {
    std::vector<core::Partition> out;
    out.reserve(result.partitions.size() + (includeUnallocated ? result.gaps.size() : 0));

    // Разделы и промежутки идут по возрастанию смещения, поэтому достаточно двух
    // указателей: карта диска (FR-2) получается в одном проходе.
    std::size_t gapIndex = 0;
    const auto pushGap = [&](const LayoutGap& gap) {
        if (includeUnallocated) out.push_back(makeCoreUnallocated(gap, static_cast<std::uint32_t>(out.size())));
    };

    for (const LayoutPartition& part : result.partitions) {
        while (gapIndex < result.gaps.size() && result.gaps[gapIndex].offsetBytes <= part.offsetBytes) {
            pushGap(result.gaps[gapIndex]);
            ++gapIndex;
        }

        core::Partition mapped;
        mapped.index = static_cast<std::uint32_t>(out.size());
        mapped.offsetBytes = part.offsetBytes;
        mapped.lengthBytes = part.lengthBytes;
        mapped.mbrType = part.mbrType;
        mapped.gptType = part.gptType;
        mapped.hasGptType = part.hasGptType;
        mapped.gptName = part.gptName;
        mapped.kind = part.kind;
        mapped.system = part.kind == core::PartitionKind::System;
        mapped.boot = part.attributes.bootIndicator || part.kind == core::PartitionKind::Efi ||
                      part.kind == core::PartitionKind::System;
        mapped.hidden = part.attributes.hidden;
        // hasVolume остаётся false: том приклеивает шаг привязки томов (FR-1 п.5).
        out.push_back(std::move(mapped));
    }

    for (; gapIndex < result.gaps.size(); ++gapIndex) pushGap(result.gaps[gapIndex]);
    return out;
}

}  // namespace mrproper::platform
