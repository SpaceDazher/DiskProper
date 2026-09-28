// Разметка диска и разделы: IOCTL_DISK_GET_DRIVE_LAYOUT_EX (SPEC §4 FR-1 п.4).
//
// Что отдаёт модуль:
//   1) чтение разметки целиком — по номеру диска или по device path;
//   2) чистый разбор: GPT-тип → core::PartitionKind, MBR-тип → core::PartitionKind,
//      GPT/MBR-атрибуты → флаги, имена типов для UI и отчёта (FR-2, FR-8);
//   3) мост в core::Partition (§6.3). Тома в этот шаг не входят: их привязку к
//      разделам делает соседний модуль инвентаризации (FR-1 п.5), поэтому здесь
//      у core::Partition остаётся hasVolume == false.
//
// Слой Win32 (§6.1, §6.2, ADR-004): windows.h живёт только в .cpp. Разборы типов
// и атрибутов — обычные функции без WinAPI, их удобно вызывать и проверять на
// любом хосте, а engine/cli/ui получают отсюда переносимый заголовок. Единственная
// зависимость — вниз, на mrproper_core.
//
// Про порядок байт в GUID. core::Guid — это 16 байт в порядке записи GUID из
// строки («xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx», RFC 4122), а НЕ little-endian
// раскладка Win32-структуры. Приведение Win32 GUID → core::Guid спрятано в .cpp,
// и наружу эта разница не выходит: canonical-строка читается и пишется одинаково.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"

namespace mrproper::platform {

// Стиль разметки. Тип переносимый: тот же разбор нужен отчёту и тестам, где
// Windows нет. Значения намеренно НЕ равны PARTITION_STYLE (winioctl.h) —
// отличать «0 = MBR» от «0 = Unknown» при разборе сырого DWORD опасно, поэтому
// перевод живёт в layoutStyleFromRaw, а Unknown стоит первым.
enum class LayoutStyle : std::uint32_t {
    Unknown = 0,
    Mbr = 1,  // PARTITION_STYLE_MBR = 0
    Gpt = 2,  // PARTITION_STYLE_GPT = 1
    Raw = 3,  // PARTITION_STYLE_RAW = 2
};

// Атрибуты раздела, приведённые к флагам. GPT-атрибуты приходят 64-битной маской
// (PARTITION_INFORMATION_GPT::Attributes), MBR отдельной маски не имеет — там
// ориентируемся на BootIndicator, RecognizedPartition и диапазон скрытых типов.
struct LayoutAttributes {
    bool bootIndicator{};       // MBR BootIndicator; GPT — GPT_ATTRIBUTE_LEGACY_BIOS_BOOTABLE
    bool hidden{};              // MBR — диапазон скрытых типов 0x12..0x1E; GPT — HIDDEN или PLATFORM_REQUIRED
    bool noDriveLetter{};       // GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER
    bool readOnly{};            // GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY; у MBR этим IOCTL не сообщается
    bool shadowCopy{};          // GPT_BASIC_DATA_ATTRIBUTE_SHADOW_COPY
    bool offline{};             // GPT_BASIC_DATA_ATTRIBUTE_OFFLINE
    bool dax{};                 // GPT_BASIC_DATA_ATTRIBUTE_DAX
    bool service{};             // GPT_BASIC_DATA_ATTRIBUTE_SERVICE либо IsServicePartition
    bool platformRequired{};    // GPT_ATTRIBUTE_PLATFORM_REQUIRED
    bool noBlockIoProtocol{};   // GPT_ATTRIBUTE_NO_BLOCK_IO_PROTOCOL
    bool recognized{};          // MBR RecognizedPartition; GPT — тип есть в списке известных
    std::uint64_t gptRaw{};     // маска Attributes как её вернул драйвер (0 у MBR)
    std::uint32_t mbrHiddenSectors{};  // MBR HiddenSectors
};

// Один раздел в том виде, в каком его отдал драйвер. core::Partition для этого
// слишком тесен: там нет GPT-атрибутов, флагов и номера раздела драйвера.
struct LayoutPartition {
    std::uint32_t number{};       // PARTITION_INFORMATION_EX::PartitionNumber
    std::uint64_t offsetBytes{};  // StartingOffset
    std::uint64_t lengthBytes{};  // PartitionLength
    LayoutStyle style{LayoutStyle::Unknown};  // стиль именно этого раздела, а не диска
    core::PartitionKind kind{core::PartitionKind::Unknown};

    // MBR: байт типа из PARTITION_INFORMATION_MBR. У GPT остаётся 0.
    std::uint8_t mbrType{};

    // GPT: тип раздела (PartitionType) — по нему определяется kind.
    core::Guid gptType{};
    bool hasGptType{};

    // Уникальный идентификатор раздела: у GPT — PartitionId, у MBR — сигнатура
    // раздела из MBR. Смысл разный, поэтому поле одно и с флагом присутствия.
    core::Guid partitionId{};
    bool hasPartitionId{};

    // GPT: имя раздела (Name[36]) в UTF-8 без нулевого хвоста. У MBR пусто.
    std::string gptName;

    LayoutAttributes attributes{};

    // Драйвер просит перезаписать раздел при записи разметки. Для чтения
    // не используется, но в отчёте полезно: означает «раздел помечен, а не создан».
    bool rewritePartition{};
};

// Участок диска, не занятый ни одним разделом. Промежутки между разделами
// считаются всегда (их границы известны из самой разметки), а хвост от последнего
// раздела до конца диска — только если вызывающий передал размер диска (FR-1 п.3
// отдаёт его через IOCTL_DISK_GET_LENGTH_INFO): без размера «хвост» был бы
// выдумкой.
struct LayoutGap {
    std::uint64_t offsetBytes{};
    std::uint64_t lengthBytes{};
};

// Ответ чтения разметки. При ok == false заполнены winError и devicePath —
// этого хватает вызывающему, чтобы записать в лог путь и HRESULT (§5, §12).
struct LayoutReadResult {
    bool ok{};
    std::uint32_t winError{};  // GetLastError(), если !ok
    int diskNumber{-1};         // -1, если читали по device path
    std::string devicePath;     // путь, по которому читалась разметка — для лога

    LayoutStyle style{LayoutStyle::Unknown};
    std::uint64_t diskSizeBytes{};  // если вызывающий передал размер диска
    // false — разделы пересекаются или вылезают за размер диска. Такой диск
    // показываем, но помечаем: цифры раздела тогда нельзя складывать (§5).
    bool consistent{true};

    // GPT-шапка из DRIVE_LAYOUT_INFORMATION_EX::Gpt.
    bool hasGptDiskId{};
    core::Guid gptDiskId{};
    std::uint64_t usableStartOffset{};  // служебные области GPT в gaps не входят
    std::uint64_t usableLength{};
    std::uint32_t maxPartitionCount{};

    // MBR-шапка из DRIVE_LAYOUT_INFORMATION_EX::Mbr.
    std::uint32_t mbrSignature{};
    std::uint32_t mbrChecksum{};

    std::vector<LayoutPartition> partitions;  // по возрастанию смещения (см. finalizeLayout)
    std::vector<LayoutGap> gaps;
};

// --- Чтение разметки (FR-1 п.4) ----------------------------------------------

// Основной вход инвентаризации: разметка физического диска по его номеру.
// diskSizeBytes необязателен; с ним дополнительно считаются gaps.
LayoutReadResult readDriveLayout(int diskNumber, std::uint64_t diskSizeBytes = 0);

// То же по пути устройства: "\\.\PhysicalDriveN", device path из
// SetupDiGetDeviceInterfaceDetailW ("\\?\X#&…", завершающий '\' добавляется сам)
// или путь тома. ВНИМАНИЕ: для тома смещения отсчитываются от начала тома, а не
// диска — это особенность самого IOCTL, а не этого модуля.
LayoutReadResult readDriveLayoutForDevice(std::string_view devicePath, std::uint64_t diskSizeBytes = 0);

// "\\\\.\\PhysicalDriveN" в UTF-8 берётся из devices::physicalDrivePathUtf8:
// единственное место, где номер диска превращается в путь (ADR-004 про слои).
// Раньше здесь была своя копия, и она конфликтовала с size_probe (C2371).

// Порядок частей результата: сортировка разделов по смещению, подсчёт gaps и
// проверка согласованности. Вынесено отдельно, потому что это чистая операция
// над уже собранным результатом (фикстуры и тесты строят его руками).
void finalizeLayout(LayoutReadResult& result);

// --- Чистый разбор ----------------------------------------------------------

LayoutStyle layoutStyleFromRaw(std::uint32_t raw) noexcept;
const char* layoutStyleName(LayoutStyle style) noexcept;

// GPT-тип (PartitionType) → вид раздела для UI (§4 FR-2) и правил.
core::PartitionKind kindFromGptType(const core::Guid& type) noexcept;

// MBR-байт типа → вид раздела. 0xEE (защитная MBR на GPT-диске) помечается
// Reserved: раздел служебный, данные на нём не лежат.
core::PartitionKind kindFromMbrType(std::uint8_t mbrType) noexcept;

// Имена типов для UI и отчёта. gptTypeName возвращает nullptr для неизвестного
// GUID, gptTypeLabel — каноническую строку GUID, чтобы отчёт оставался полным.
const char* gptTypeName(const core::Guid& type) noexcept;
std::string gptTypeLabel(const core::Guid& type);
const char* mbrTypeName(std::uint8_t mbrType) noexcept;
std::string mbrTypeLabel(std::uint8_t mbrType);

// Атрибуты. Для GPT маску разбираем по битам GPT_ATTRIBUTE_* и
// GPT_BASIC_DATA_ATTRIBUTE_* (winioctl.h), isServicePartition — флаг
// PARTITION_INFORMATION_EX::IsServicePartition.
LayoutAttributes gptAttributesFromRaw(std::uint64_t attributes, bool isServicePartition) noexcept;
LayoutAttributes mbrAttributesFromRaw(std::uint8_t mbrType, bool bootIndicator, bool recognized,
                                      std::uint32_t hiddenSectors) noexcept;

// GUID в текстовом виде и обратно. Игнорирует регистр и дефисы; на мусорной
// строке возвращает пустую строку / std::nullopt (разбор не имеет права бросать).
std::string guidToCanonicalString(const core::Guid& guid);
std::optional<core::Guid> guidFromCanonicalString(std::string_view text);

// --- Мост в модель (§6.3) ----------------------------------------------------

// core::Partition для каждого раздела. hasVolume остаётся false: том приклеивает
// соседний шаг инвентаризации (FR-1 п.5).
//
// includeUnallocated == false (по умолчанию) — только настоящие разделы: их
// число должно совпадать с Get-Disk / diskpart (§8 Этап 1, §12). true —
// дополнительно вставлять промежутки как core::PartitionKind::Unallocated, чтобы
// карта диска показывала всю поверхность, включая незанятые области.
std::vector<core::Partition> toCorePartitions(const LayoutReadResult& result, bool includeUnallocated = false);

}  // namespace mrproper::platform
