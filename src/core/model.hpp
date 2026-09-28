// Переносимая часть модели данных (SPEC §6.3). Здесь нет Windows-типов:
// GUID хранится как массив байт, файловые пути — как UTF-8.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mrproper::core {

using Guid = std::array<std::uint8_t, 16>;

enum class BusType { Unknown, Sata, Nvme, Usb, Scsi, Sd, Sas, Virtual, Raid };
enum class SafetyLevel { Safe, Review, Risky };
enum class PartitionKind { BasicData, System, Msr, Recovery, Oem, Efi, Reserved, Unallocated, Unknown };
enum class PlanAction { Delete, Trash, Keep, SkipLocked };
enum class Category { Temp, Browser, Logs, Dumps, Cache, Update, Prefetch, Installer, UserBigFiles, Other };

const char* toString(SafetyLevel level);
const char* toString(BusType bus);
const char* toString(PartitionKind kind);
const char* toString(PlanAction action);
const char* toString(Category category);

struct Volume {
    std::string volumeGuidPath;   // путь вида \\?\Volume{...} (без завершающего слэша)
    std::string label;
    std::string fileSystem;       // NTFS, FAT32, ReFS
    std::uint64_t totalBytes{};
    std::uint64_t freeBytes{};
    bool encrypted{};
    bool dirty{};
    bool readOnly{};
    std::vector<std::string> mountPoints;                  // "C:\\", "D:\\Data"
    std::vector<std::pair<int, std::uint64_t>> diskExtents; // номер диска + смещение
};

struct Partition {
    std::uint32_t index{};
    std::uint64_t offsetBytes{};
    std::uint64_t lengthBytes{};
    std::uint8_t mbrType{};
    Guid gptType{};
    bool hasGptType{};
    std::string gptName;
    bool system{};
    bool boot{};
    bool hidden{};
    PartitionKind kind{PartitionKind::Unknown};
    bool hasVolume{};
    Volume volume;
};

struct PhysicalDisk {
    int number{};
    std::string devicePath;   // \\?\X#&…
    std::string model;
    std::string serial;
    std::string firmware;
    BusType bus{BusType::Unknown};
    std::uint64_t sizeBytes{};
    bool removable{};
    bool readOnly{};
    bool trimSupported{};
    bool smartAvailable{};
    std::vector<Partition> partitions;
};

// Ссылка на процесс, удерживающий файл (Restart Manager).
struct ProcessRef {
    std::uint32_t pid{};
    std::string name;
};

struct CleanupCandidate {
    std::string ruleId;
    std::string category;
    std::string path;
    std::string displayName;
    std::uint64_t logicalBytes{};
    std::uint64_t allocatedBytes{};
    std::uint32_t fileCount{};
    std::int64_t oldestWrite{};   // unix-секунды
    std::int64_t newestWrite{};
    std::int64_t lastAccess{};
    SafetyLevel safety{SafetyLevel::Review};
    int confidence{};
    std::vector<std::string> reasons;
    std::vector<ProcessRef> lockedBy;
};

struct CleanupPlanItem {
    std::size_t candidateIndex{};
    PlanAction action{PlanAction::Keep};
    std::uint64_t reclaimBytes{};
};

}  // namespace mrproper::core
