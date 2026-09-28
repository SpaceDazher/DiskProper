// Реализация gpt_kind: таблица типов раздела GPT и разбор по ней. Здесь же
// единственное место модуля, где таблица лежит целиком, — снаружи наружу уходит
// только перечисление (gpt_kind.hpp).
//
// Про источники значений. Группы помечены в комментариях:
//   * UEFI/PI — спецификация UEFI, раздел про GPT-типы (ESP, MBR system,
//     зарезервированные записи);
//   * Microsoft — Windows Kits, shared/diskguid.h, константы PARTITION_*_GUID;
//   * Linux — список типов util-linux (gdisk/blkid);
//   * Apple — типы разделов macOS/iOS;
//   * VMware — VMFS.
// Объявлять эти GUID через initguid.h здесь нельзя: символ пришлось бы
// продублировать с другим .cpp слоя platform (два определения одной константы —
// LNK2005) либо тянуть extern-объявление и надеяться на uuid.lib. Константы
// локальные, а источник истины один — SDK и спецификация, названные выше.
//
// Про порядок байт: core::Guid записан в порядке строки GUID (RFC 4122),
// поэтому makeGuid раскладывает компоненты от старшего байта к младшему, а
// хвост копируется напрямую. Проверка на дубль в конце файла ловит опечатку в
// значении, которую иначе не заметил бы ни компилятор, ни тест: поиск идёт
// сверху вниз, и вторая копия GUID просто никогда не читалась бы.
//
// Про неполноту таблицы. Это осознанный компромисс: в мире десятки разных
// GPT-типов (CloudH, Ceph, DST, NVM Express...), и длинать список значило бы
// тащить в проект чужие соглашения без проверяемого источника. Неизвестный
// GUID честно читается как Unknown и печатается канонической строкой — карта
// разделов от этого не врёт (SPEC §4 FR-2 требует показать 100 % разделов).

#include "gpt_kind.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace mrproper::platform {
namespace {

// core::Guid из компонентов DEFINE_GUID. Порядок байт — текстовый, как в строке
// GUID, поэтому Data1/Data2/Data3 раскладываются от старшего байта, а хвост
// копируется как есть.
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

struct GptTypeEntry {
    core::Guid type;
    const char* name;
    core::PartitionKind kind;
    GptTypeFamily family;
};

// Таблица типов. Порядок групп — UEFI, Microsoft, Linux, Apple, VMware; внутри
// группы записи идут так, как их удобнее читать: сначала те, что встречаются на
// любой машине, потом хост-специфичные.
constexpr GptTypeEntry kGptTypes[] = {
    // --- Спецификация UEFI/PI ------------------------------------------------
    // C12A7328-F81F-11D2-BA4B-00A0C93EC93B: EFI system partition. Загрузчик
    // UEFI; на Windows 10/11 это скрытая системная область без буквы.
    {makeGuid(0xC12A7328u, 0xF81Fu, 0x11D2u, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}),
     "EFI system partition", core::PartitionKind::Efi, GptTypeFamily::Uefi},
    // 024DEE41-33E7-11D3-9D69-0008C781F39F: MBR system partition — служебная
    // область для legacy-загрузки, зарезервирована спецификацией.
    {makeGuid(0x024DEE41u, 0x33E7u, 0x11D3u, {0x9D, 0x69, 0x00, 0x08, 0xC7, 0x81, 0xF3, 0x9F}),
     "MBR system partition", core::PartitionKind::Reserved, GptTypeFamily::Uefi},
    // 21686148-6449-6E6F-744E-656564454649: Reserved — тип, который спецификация
    // отдаёт под служебные нужды платформы (на Windows — Data Recovery).
    {makeGuid(0x21686148u, 0x6449u, 0x6E6Fu, {0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}),
     "Reserved (platform)", core::PartitionKind::Reserved, GptTypeFamily::Uefi},
    // 00000000-0000-0000-0000-000000000000: неиспользованная запись таблицы
    // разделов. Раздела за ней нет, поэтому вид — Unknown, а не Reserved:
    // «нет раздела» и «раздел есть, но служебный» в карте различаются.
    {makeGuid(0x00000000u, 0x0000u, 0x0000u, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
     "Unused entry", core::PartitionKind::Unknown, GptTypeFamily::Uefi},
    // 6A85CF4D-1DD2-11B1-99A6-080020736631: маркер «свободно» из util-linux
    // (gdisk). Единственный известный тип, который означает именно
    // неразмеченную область, поэтому он и единственный, кто даёт Unallocated:
    // промежутки между разделами в остальных случаях считает layout.
    {makeGuid(0x6A85CF4Du, 0x1DD2u, 0x11B1u, {0x99, 0xA6, 0x08, 0x00, 0x20, 0x73, 0x66, 0x31}),
     "Free space", core::PartitionKind::Unallocated, GptTypeFamily::Other},

    // --- Windows Kits, shared/diskguid.h ------------------------------------
    // E3C9E316-0B5C-4DB8-817D-F92DF00215AE: PARTITION_MSFT_RESERVED_GUID (MSR).
    // Метаданные GPT для будущих разделов; данных пользователя не несёт.
    {makeGuid(0xE3C9E316u, 0x0B5Cu, 0x4DB8u, {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}),
     "Microsoft reserved space (MSR)", core::PartitionKind::Msr, GptTypeFamily::Microsoft},
    // EBD0A0A2-B9E5-4433-87C0-68B6B72699C7: PARTITION_BASIC_DATA_GUID. Это
    // обычный пользовательский том Windows — включая диск C:.
    {makeGuid(0xEBD0A0A2u, 0xB9E5u, 0x4433u, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}),
     "Basic data partition", core::PartitionKind::BasicData, GptTypeFamily::Microsoft},
    // DE94BBA4-06D1-4D40-A16A-BFD50179D6AC: PARTITION_MSFT_RECOVERY_GUID,
    // среда восстановления Windows (WinRE). Пользовательских файлов там нет.
    {makeGuid(0xDE94BBA4u, 0x06D1u, 0x4D40u, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}),
     "Microsoft recovery partition", core::PartitionKind::Recovery, GptTypeFamily::Microsoft},
    // 57434F53-E3E3-4631-A5C5-26D2243873AA: PARTITION_WINDOWS_SYSTEM_GUID,
    // служебная системная область Windows.
    {makeGuid(0x57434F53u, 0xE3E3u, 0x4631u, {0xA5, 0xC5, 0x26, 0xD2, 0x24, 0x38, 0x73, 0xAA}),
     "Windows system partition", core::PartitionKind::System, GptTypeFamily::Microsoft},
    // 57434F53-8F45-405E-8A23-186D8A4330D3: PARTITION_MAIN_OS_GUID — раздел с
    // установленной ОС (встречается на OEM-образах, не на обычных Win10/11).
    {makeGuid(0x57434F53u, 0x8F45u, 0x405Eu, {0x8A, 0x23, 0x18, 0x6D, 0x8A, 0x43, 0x30, 0xD3}),
     "Main OS partition", core::PartitionKind::System, GptTypeFamily::Microsoft},
    // 57434F53-23F2-44D5-A830-67BBDAA609F9: PARTITION_OS_DATA_GUID — данные ОС.
    // Служебной областью не является: пользовательские файлы лежат на нём же.
    {makeGuid(0x57434F53u, 0x23F2u, 0x44D5u, {0xA8, 0x30, 0x67, 0xBB, 0xDA, 0xA6, 0x09, 0xF9}),
     "OS data partition", core::PartitionKind::BasicData, GptTypeFamily::Microsoft},
    // 57434F53-7FE0-4196-9B42-427B51643484: PARTITION_PRE_INSTALLED_GUID —
    // предустановленные приложения и данные с завода.
    {makeGuid(0x57434F53u, 0x7FE0u, 0x4196u, {0x9B, 0x42, 0x42, 0x7B, 0x51, 0x64, 0x34, 0x84}),
     "Pre-installed applications partition", core::PartitionKind::Oem, GptTypeFamily::Microsoft},
    // 57434F53-4DF9-45B9-8E9E-2370F006457C: PARTITION_BSP_GUID — базовая
    // система вендора.
    {makeGuid(0x57434F53u, 0x4DF9u, 0x45B9u, {0x8E, 0x9E, 0x23, 0x70, 0xF0, 0x06, 0x45, 0x7C}),
     "BSP partition", core::PartitionKind::Oem, GptTypeFamily::Microsoft},
    // 57434F53-94CB-43F0-A533-D73C10CFA57D: PARTITION_DPP_GUID — драйверы и
    // пакеты вендора.
    {makeGuid(0x57434F53u, 0x94CBu, 0x43F0u, {0xA5, 0x33, 0xD7, 0x3C, 0x10, 0xCF, 0xA5, 0x7D}),
     "DPP partition", core::PartitionKind::Oem, GptTypeFamily::Microsoft},
    // 8967A686-96AA-6AA8-9589-A84256541090: PARTITION_PATCH_GUID — раздел с
    // обновлениями Windows.
    {makeGuid(0x8967A686u, 0x96AAu, 0x6AA8u, {0x95, 0x89, 0xA8, 0x42, 0x56, 0x54, 0x10, 0x90}),
     "Patch partition", core::PartitionKind::Oem, GptTypeFamily::Microsoft},
    // 424CA0E2-7CB2-4FB9-8143-C52A99398BC6: PARTITION_LEGACY_BL_GUID —
    // загрузчик вне GPT (BIOS).
    {makeGuid(0x424CA0E2u, 0x7CB2u, 0x4FB9u, {0x81, 0x43, 0xC5, 0x2A, 0x99, 0x39, 0x8B, 0xC6}),
     "Legacy boot loader partition", core::PartitionKind::Oem, GptTypeFamily::Microsoft},
    // 424C3E6C-D79F-49CB-935D-36D71467A288: PARTITION_LEGACY_BL_GUID_BACKUP.
    {makeGuid(0x424C3E6Cu, 0xD79Fu, 0x49CBu, {0x93, 0x5D, 0x36, 0xD7, 0x14, 0x67, 0xA2, 0x88}),
     "Legacy boot loader backup partition", core::PartitionKind::Oem, GptTypeFamily::Microsoft},
    // 5808C8AA-7E8F-42E0-85D2-E1E90434CFB3: PARTITION_LDM_METADATA_GUID —
    // метаданные динамических дисков Windows.
    {makeGuid(0x5808C8AAu, 0x7E8Fu, 0x42E0u, {0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3}),
     "LDM metadata partition", core::PartitionKind::Reserved, GptTypeFamily::Microsoft},
    // AF9B60A0-1431-4F62-BC68-3311714A69AD: PARTITION_LDM_DATA_GUID — область
    // данных динамических дисков. В карте она служебная: файлы живут в томах
    // поверх динамического диска, а не в этом разделе.
    {makeGuid(0xAF9B60A0u, 0x1431u, 0x4F62u, {0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD}),
     "LDM data partition", core::PartitionKind::Reserved, GptTypeFamily::Microsoft},
    // E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D: PARTITION_SPACES_GUID — защитная
    // область Storage Spaces.
    {makeGuid(0xE75CAF8Fu, 0xF680u, 0x4CEEu, {0xAF, 0xA3, 0xB0, 0x01, 0xE5, 0x6E, 0xFC, 0x2D}),
     "Storage Spaces protective partition", core::PartitionKind::Reserved, GptTypeFamily::Microsoft},
    // E7ADDCB4-DC34-4539-9A76-EBBD07BE6F7E: PARTITION_SPACES_DATA_GUID.
    {makeGuid(0xE7ADDCB4u, 0xDC34u, 0x4539u, {0x9A, 0x76, 0xEB, 0xBD, 0x07, 0xBE, 0x6F, 0x7E}),
     "Storage Spaces data partition", core::PartitionKind::Reserved, GptTypeFamily::Microsoft},
    // 45B0969E-9B03-4F30-B4C6-B4B80CEFF106: устаревший тип Storage Spaces,
    // остаётся на дисках, размеченных старыми версиями Windows.
    {makeGuid(0x45B0969Eu, 0x9B03u, 0x4F30u, {0xB4, 0xC6, 0xB4, 0xB8, 0x0C, 0xEF, 0xF1, 0x06}),
     "Storage Spaces partition", core::PartitionKind::Reserved, GptTypeFamily::Microsoft},

    // --- Linux (util-linux: gdisk/blkid) ------------------------------------
    // 0FC63DAF-8483-4772-8E79-3D69D8477DE4: Linux filesystem data — ext2/3/4,
    // XFS, Btrfs. Пользовательские данные.
    {makeGuid(0x0FC63DAFu, 0x8483u, 0x4772u, {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}),
     "Linux filesystem data", core::PartitionKind::BasicData, GptTypeFamily::Linux},
    // 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709: Linux root (x86-64).
    {makeGuid(0x4F68BCE3u, 0xE8CDu, 0x4DB1u, {0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09}),
     "Linux root (x86-64)", core::PartitionKind::BasicData, GptTypeFamily::Linux},
    // B921B045-1DF0-41C3-AF44-4C6F280D3FAE: Linux root (ARM-64).
    {makeGuid(0xB921B045u, 0x1DF0u, 0x41C3u, {0xAF, 0x44, 0x4C, 0x6F, 0x28, 0x0D, 0x3F, 0xAE}),
     "Linux root (ARM-64)", core::PartitionKind::BasicData, GptTypeFamily::Linux},
    // BC13C2FF-59E6-4262-A352-B275FD6F7172: Linux root (i386).
    {makeGuid(0xBC13C2FFu, 0x59E6u, 0x4262u, {0xA3, 0x52, 0xB2, 0x75, 0xFD, 0x6F, 0x71, 0x72}),
     "Linux root (i386)", core::PartitionKind::BasicData, GptTypeFamily::Linux},
    // 933AC7E1-2EB4-4F13-B844-0E14E2AEF915: Linux /home.
    {makeGuid(0x933AC7E1u, 0x2EB4u, 0x4F13u, {0xB8, 0x44, 0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15}),
     "Linux /home", core::PartitionKind::BasicData, GptTypeFamily::Linux},
    // E6D6D379-F507-44C2-A23C-238F2A3DF928: Linux LVM (физический том).
    {makeGuid(0xE6D6D379u, 0xF507u, 0x44C2u, {0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28}),
     "Linux LVM physical volume", core::PartitionKind::BasicData, GptTypeFamily::Linux},
    // A19D880F-05FC-4D3B-A006-743F0F84911E: Linux RAID. Данные пользователя
    // внутри массива есть, поэтому это Basic Data, а не служебный раздел.
    {makeGuid(0xA19D880Fu, 0x05FCu, 0x4D3Bu, {0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E}),
     "Linux RAID", core::PartitionKind::BasicData, GptTypeFamily::Linux},
    // 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F: Linux swap. Файлов нет вообще, но
    // это не «свободное место» — память подкачки может быть занята, поэтому
    // вид Reserved, а не Unallocated.
    {makeGuid(0x0657FD6Du, 0xA4ABu, 0x43C4u, {0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F}),
     "Linux swap", core::PartitionKind::Reserved, GptTypeFamily::Linux},

    // --- Apple --------------------------------------------------------------
    // 48465300-0000-11AA-AA11-00306543ECAC: Apple HFS+.
    {makeGuid(0x48465300u, 0x0000u, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple HFS+", core::PartitionKind::BasicData, GptTypeFamily::Apple},
    // 5265636F-7900-11AA-AA11-00306543ECAC: Apple Core Storage, контейнер APFS.
    {makeGuid(0x5265636Fu, 0x7900u, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple Core Storage (APFS)", core::PartitionKind::BasicData, GptTypeFamily::Apple},
    // 7C3457EF-0000-11AA-AA11-00306543ECAC: загрузочный раздел Apple.
    {makeGuid(0x7C3457EFu, 0x0000u, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple boot partition", core::PartitionKind::Efi, GptTypeFamily::Apple},
    // 53746F72-6167-11AA-AA11-00306543ECAC: Apple APFS preboot — ресурсы
    // загрузки, по роли то же, что ESP.
    {makeGuid(0x53746F72u, 0x6167u, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple APFS preboot", core::PartitionKind::Efi, GptTypeFamily::Apple},
    // 426F6F74-0000-11AA-AA11-00306543ECAC: Apple TV recovery.
    {makeGuid(0x426F6F74u, 0x0000u, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple TV recovery", core::PartitionKind::Recovery, GptTypeFamily::Apple},
    // 52414944-0000-11AA-AA11-00306543ECAC: Apple RAID.
    {makeGuid(0x52414944u, 0x0000u, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple RAID", core::PartitionKind::Reserved, GptTypeFamily::Apple},
    // 52414944-5F4F-11AA-AA11-00306543ECAC: тот же RAID, отсоединённый.
    {makeGuid(0x52414944u, 0x5F4Fu, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple RAID (offline)", core::PartitionKind::Reserved, GptTypeFamily::Apple},
    // 69646961-0000-11AA-AA11-00306543ECAC: Apple RAID (другая разметка).
    {makeGuid(0x69646961u, 0x0000u, 0x11AAu, {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}),
     "Apple RAID (iAdi)", core::PartitionKind::Reserved, GptTypeFamily::Apple},

    // --- Прочее оборудование ------------------------------------------------
    // AA31E02A-400F-11DB-9590-000C2911D1B8: VMware VMFS, хранилище ESXi.
    {makeGuid(0xAA31E02Au, 0x400Fu, 0x11DBu, {0x95, 0x90, 0x00, 0x0C, 0x29, 0x11, 0xD1, 0xB8}),
     "VMware VMFS datastore", core::PartitionKind::BasicData, GptTypeFamily::Vmware},
};

constexpr std::size_t kGptTypeCount = sizeof(kGptTypes) / sizeof(kGptTypes[0]);

// Поиск линейный: записей меньше сорока, спрашивают по одному разу на раздел
// диска при инвентаризации (SPEC §4 FR-1), поэтому хеш-таблица тут была бы
// дороже самой таблицы.
const GptTypeEntry* findEntry(const core::Guid& type) noexcept {
    for (const GptTypeEntry& entry : kGptTypes) {
        if (entry.type == type) return &entry;
    }
    return nullptr;
}

// Проверка таблицы на этапе компиляции: одинаковый GUID дважды — это опечатка,
// которую иначе не увидит ни компилятор, ни тест, потому что поиск читает
// первую запись и молча игнорирует вторую.
constexpr bool hasDuplicateTypes() noexcept {
    for (std::size_t i = 0; i < kGptTypeCount; ++i) {
        for (std::size_t j = i + 1; j < kGptTypeCount; ++j) {
            if (kGptTypes[i].type == kGptTypes[j].type) return true;
        }
    }
    return false;
}
static_assert(!hasDuplicateTypes(), "gpt_kind: в таблице типов есть повторяющийся GUID");

// Каноническая запись GUID: «xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx», заглавные
// шестнадцатеричные. Своя реализация вместо вызова platform::layout намеренно:
// соседние модули платформы пишутся параллельно, и опираться на их заголовки
// здесь нельзя (см. шапку gpt_kind.hpp).
std::string canonicalGuidString(const core::Guid& guid) {
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < guid.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
        const std::uint8_t byte = guid[index];
        text.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
        text.push_back(kHexDigits[byte & 0x0Fu]);
    }
    return text;
}

}  // namespace

core::PartitionKind gptKindFromType(const core::Guid& type) noexcept {
    const GptTypeEntry* entry = findEntry(type);
    return entry != nullptr ? entry->kind : core::PartitionKind::Unknown;
}

std::optional<GptTypeInfo> gptTypeInfo(const core::Guid& type) noexcept {
    const GptTypeEntry* entry = findEntry(type);
    if (entry == nullptr) return std::nullopt;
    return GptTypeInfo{entry->kind, entry->family, entry->name};
}

bool isKnownGptType(const core::Guid& type) noexcept {
    return findEntry(type) != nullptr;
}

const char* gptKindName(const core::Guid& type) noexcept {
    const GptTypeEntry* entry = findEntry(type);
    return entry != nullptr ? entry->name : nullptr;
}

std::string gptKindLabel(const core::Guid& type) {
    if (const char* name = gptKindName(type); name != nullptr) return name;
    return canonicalGuidString(type);  // незнакомый тип показываем как есть
}

const char* gptFamilyName(GptTypeFamily family) noexcept {
    switch (family) {
        case GptTypeFamily::Uefi:
            return "UEFI";
        case GptTypeFamily::Microsoft:
            return "Microsoft";
        case GptTypeFamily::Linux:
            return "Linux";
        case GptTypeFamily::Apple:
            return "Apple";
        case GptTypeFamily::Vmware:
            return "VMware";
        case GptTypeFamily::Other:
            return "Other";
        case GptTypeFamily::Unknown:
            break;
    }
    return "Unknown";
}

bool gptKindIsService(core::PartitionKind kind) noexcept {
    switch (kind) {
        case core::PartitionKind::Msr:
        case core::PartitionKind::Reserved:
        case core::PartitionKind::Unallocated:
        // «Не классифицировали» — тоже повод не предлагать раздел: правило
        // очистки не должно опираться на догадку о типе.
        case core::PartitionKind::Unknown:
            return true;
        case core::PartitionKind::BasicData:
        case core::PartitionKind::System:
        case core::PartitionKind::Recovery:
        case core::PartitionKind::Oem:
            return false;
    }
    return true;
}

bool gptKindMayHoldUserData(core::PartitionKind kind) noexcept {
    switch (kind) {
        // Recovery — это WinRE.wim, пользовательских файлов там нет, поэтому
        // в список True он не попадает.
        case core::PartitionKind::BasicData:
        case core::PartitionKind::System:
        case core::PartitionKind::Oem:
            return true;
        case core::PartitionKind::Msr:
        case core::PartitionKind::Recovery:
        case core::PartitionKind::Reserved:
        case core::PartitionKind::Unallocated:
        case core::PartitionKind::Unknown:
            return false;
    }
    return false;
}

}  // namespace mrproper::platform
