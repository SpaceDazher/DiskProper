#include "disk_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Единственное определение насыщающей арифметики размеров в ядре —
// core::sizing. Своих saturatingAdd/saturatingSub здесь не заводим: две
// реализации «сложения, которое не переполняется» разъедутся за месяц.
#include "sizing.hpp"
// Формат чисел для текста: те же «1,2 ГБ», что на экране «Диски».
#include "units.hpp"

namespace mrproper::core {
namespace {

constexpr char kSeparator = '\\';

// «Неизвестно» в тексте. Отдельное слово, а не «0 ГБ»: ноль байт свободного
// места и отсутствие данных выглядят в отчёте одинаково, а означают разное.
constexpr const char* kUnknownText = "н/д";

// ASCII-приведение регистра. Локали не касаемся намеренно: пути Windows
// сравниваются регистронезависимо, а приведение через toupper зависит от
// кодовой страницы процесса и меняет результат вместе с ней.
char foldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Посимвольный обход пути в нормализованном виде: «/» и «\» — один
// разделитель, повторы разделителей внутри пути пропускаются, регистр
// ASCII-букв приводится к нижнему (по флагу). Выделений памяти нет —
// сравнение путей зовётся из noexcept-функций поиска тома.
//
// Отдельно сохранена пара разделителей в начале пути: это UNC-адрес
// «\\server\share», и схлопывание превратило бы его в путь на локальном диске.
class PathChars {
public:
    explicit PathChars(std::string_view path, bool foldCase = true) noexcept
        : path_(path), foldCase_(foldCase) {}

    // Следующий значащий символ; false — путь закончился.
    bool next(char& out) noexcept {
        while (pos_ < path_.size()) {
            const char raw = path_[pos_++];
            const bool separator = (raw == '/' || raw == kSeparator);
            if (!separator) {
                // Любая буква или цифра прерывает серию разделителей: иначе
                // «C:\Temp» схлопнулся бы в «C:\Temp» без разделителя, то есть
                // в имя файла, которого на диске нет.
                separatorRun_ = 0;
                out = foldCase_ ? foldAscii(raw) : raw;
                return true;
            }
            if (separatorRun_ == 0 || (pos_ == 2 && separatorRun_ == 1)) {
                out = kSeparator;
                ++separatorRun_;
                return true;
            }
            ++separatorRun_;  // лишний разделитель внутри пути
        }
        return false;
    }

private:
    std::string_view path_;
    std::size_t pos_ = 0;
    int separatorRun_ = 0;
    bool foldCase_ = true;
};

// Хвостовые разделители незначимы: «C:\Mount» и «C:\Mount\» — одна папка.
// Но у корня тома и у UNC-шары разделитель убрать нельзя: «C:\» без слэша —
// это уже относительный путь «текущий каталог диска C», а «\\server\share» —
// относительный путь внутри шары. Оба значения живут в одном пространстве
// имён с абсолютными путями, поэтому различие обязано сохраниться.
std::string stripTrailingSeparators(std::string normalized) {
    std::size_t end = normalized.size();
    while (end > 0 && normalized[end - 1] == kSeparator) --end;
    if (end == 0) return normalized;  // путь из одних разделителей

    // «C:» -> «C:\»: после буквы с двоеточием корень восстанавливается.
    if (normalized[end - 1] == ':') {
        normalized.resize(end);
        normalized.push_back(kSeparator);
        return normalized;
    }

    // UNC-корень «\\server\share» содержит ровно три разделителя: два ведущих
    // и один между именем и шарой. Больше — значит путь ушёл глубже, и хвост
    // убирается честно. Считаем по уже сокращённой части: иначе хвостовой
    // разделитель, который мы только что сняли, попал бы в счёт и перепутал
    // корень с вложенной папкой.
    if (end >= 3 && normalized[0] == kSeparator && normalized[1] == kSeparator) {
        std::size_t separators = 0;
        for (std::size_t i = 0; i < end; ++i) {
            if (normalized[i] == kSeparator) ++separators;
        }
        if (separators == 3) {
            normalized.resize(end);
            normalized.push_back(kSeparator);
            return normalized;
        }
    }

    normalized.resize(end);
    return normalized;
}

std::string normalizeImpl(std::string_view path, bool foldCase) {
    std::string out;
    out.reserve(path.size());
    PathChars chars(path, foldCase);
    char c = 0;
    while (chars.next(c)) out.push_back(c);
    return stripTrailingSeparators(std::move(out));
}

// --- extent'ы тома ----------------------------------------------------------
//
// Том может лежать на нескольких дисках (динамические и спан-тома, RAID —
// FR-1 п.5). Кто его показывает, тот и отвечает за его свободное место: том
// учитывается на диске, в разметке которого он перечислен, и ни на каком
// больше. Иначе сумма по дискам считала бы одно место дважды, а это ровно та
// ошибка, которую нельзя допустить в числе «свободно». Тома, идущие по
// нескольким дискам, помечаются отдельно, чтобы интерфейс мог показать
// «динамический том» (FR-1 п.5: спаны и RAID).

// Более одного разного номера диска в extent'ах.
bool spansMultipleDisks(const Volume& volume) noexcept {
    int first = -1;
    for (const auto& extent : volume.diskExtents) {
        if (first < 0) {
            first = extent.first;
            continue;
        }
        if (extent.first != first) return true;
    }
    return false;
}

// --- текст ------------------------------------------------------------------

std::string orUnknown(const std::string& value) {
    return value.empty() ? std::string(kUnknownText) : value;
}

std::string sizeText(std::uint64_t bytes, bool known) {
    return known ? formatBytes(bytes) : std::string(kUnknownText);
}

void appendFlag(std::string& flags, bool enabled, const char* text) {
    if (!enabled) return;
    if (!flags.empty()) flags += ", ";
    flags += text;
}

// Строка раздела для карты: вид разметки, том, метка, флаги.
std::string describePartition(const PhysicalDisk& disk, const Partition& partition) {
    // Имя GPT бывает пустым у системных разделов — это норма, а не сбой, и
    // отдельной пометки не требует: в скобках остаётся вид раздела.
    std::string out =
        "  Раздел " + std::to_string(partition.index) + ": " + partitionKindName(partition.kind);
    if (!partition.gptName.empty()) out += " «" + partition.gptName + "»";
    if (partition.hasVolume) {
        const Volume& volume = partition.volume;
        const VolumeUsage usage = volumeUsage(volume);
        out += ", " + orUnknown(volume.fileSystem);
        if (!volume.label.empty()) out += " «" + volume.label + "»";
        out += ", " + sizeText(usage.totalBytes, usage.sizesKnown);
        out += ", свободно " + sizeText(usage.freeBytes, usage.sizesKnown);
        if (usage.sizesKnown) out += " (" + formatPercent(usage.freeFraction) + ")";
        if (!volume.mountPoints.empty()) {
            out += ", точки монтирования: ";
            for (std::size_t i = 0; i < volume.mountPoints.size(); ++i) {
                if (i > 0) out += ", ";
                out += volume.mountPoints[i];
            }
        }
    } else {
        out += ", тома нет";
    }
    std::string flags;
    appendFlag(flags, partition.system, "системный");
    appendFlag(flags, partition.boot, "загрузочный");
    appendFlag(flags, partition.hidden, "скрытый");
    if (partition.hasVolume) {
        appendFlag(flags, partition.volume.encrypted, "шифрованный");
        appendFlag(flags, partition.volume.readOnly || disk.readOnly, "только чтение");
        appendFlag(flags, partition.volume.dirty, "не выгружен");
    }
    if (!flags.empty()) out += " [" + flags + "]";
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------

std::string normalizePath(std::string_view path) { return normalizeImpl(path, false); }

std::string pathKey(std::string_view path) { return normalizeImpl(path, true); }

bool samePath(std::string_view a, std::string_view b) noexcept {
    PathChars charsA(a);
    PathChars charsB(b);
    for (;;) {
        char ca = 0;
        char cb = 0;
        const bool okA = charsA.next(ca);
        const bool okB = charsB.next(cb);
        if (okA != okB) {
            // Разница ровно в хвостовом разделителе: «C:\Mount» и «C:\Mount\» —
            // одна и та же папка, а точка монтирования у платформы может
            // прийти в любой из двух видов.
            if (okA) return ca == kSeparator && !charsA.next(ca);
            if (okB) return cb == kSeparator && !charsB.next(cb);
            return false;
        }
        if (!okA) return true;
        if (ca != cb) return false;
    }
}

bool pathIsWithin(std::string_view path, std::string_view root) noexcept {
    PathChars charsPath(path);
    PathChars charsRoot(root);
    char lastRoot = '\0';
    for (;;) {
        char cp = 0;
        char cr = 0;
        const bool okPath = charsPath.next(cp);
        const bool okRoot = charsRoot.next(cr);
        if (!okRoot) {
            // Корень кончился. Совпадение целиком — это «путь == корень».
            // Если путь ещё не кончился, продолжить можно только разделителем:
            // так «C:\Windows» вмещает «C:\Windows\Temp», а «C:\Program» не
            // вмещает «C:\Program Files\Temp» (это разные папки).
            if (!okPath) return true;
            // Корень вида «C:» — не каталог, а текущий каталог диска: внутри
            // него не лежит ничего, даже «C:\Windows».
            if (lastRoot == ':') return false;
            return lastRoot == kSeparator || cp == kSeparator;
        }
        if (!okPath) return false;
        if (cp != cr) return false;
        lastRoot = cr;
    }
}

std::optional<char> driveLetterOf(std::string_view mountPoint) noexcept {
    if (mountPoint.size() < 2 || mountPoint[1] != ':') return std::nullopt;
    const char letter = mountPoint[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))) return std::nullopt;
    return static_cast<char>((letter >= 'a' && letter <= 'z') ? letter - 'a' + 'A' : letter);
}

bool isVolumeRootPath(std::string_view path) noexcept {
    const std::optional<char> letter = driveLetterOf(path);
    if (!letter) return false;
    PathChars chars(path);
    char c = 0;
    if (!chars.next(c) || c != foldAscii(*letter)) return false;
    if (!chars.next(c) || c != ':') return false;
    if (!chars.next(c) || c != kSeparator) return false;
    return !chars.next(c);  // после разделителя ничего: ровно корень тома
}

bool hasDriveLetter(const Volume& volume) noexcept {
    for (const std::string& mountPoint : volume.mountPoints) {
        if (driveLetterOf(mountPoint)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Занятость и свободное место
// ---------------------------------------------------------------------------

double freeRatio(std::uint64_t totalBytes, std::uint64_t freeBytes) noexcept {
    if (totalBytes == 0) return 0.0;
    if (freeBytes >= totalBytes) return 1.0;
    return static_cast<double>(freeBytes) / static_cast<double>(totalBytes);
}

double usedRatio(std::uint64_t totalBytes, std::uint64_t freeBytes) noexcept {
    if (totalBytes == 0) return 0.0;
    const std::uint64_t used = saturatingSub(totalBytes, freeBytes);
    if (used >= totalBytes) return 1.0;
    return static_cast<double>(used) / static_cast<double>(totalBytes);
}

VolumeUsage volumeUsage(const Volume& volume) noexcept {
    VolumeUsage usage;
    usage.totalBytes = volume.totalBytes;
    usage.freeBytes = volume.freeBytes;
    usage.usedBytes = saturatingSub(volume.totalBytes, volume.freeBytes);
    usage.sizesKnown = volume.totalBytes > 0;
    usage.consistent = volume.freeBytes <= volume.totalBytes;
    usage.freeFraction = freeRatio(volume.totalBytes, volume.freeBytes);
    usage.usedFraction = usedRatio(volume.totalBytes, volume.freeBytes);
    return usage;
}

DiskUsage diskUsage(const PhysicalDisk& disk) noexcept {
    DiskUsage usage;
    usage.sizeBytes = disk.sizeBytes;
    usage.sizeKnown = disk.sizeBytes > 0;

    // Свободное место известно, если ответил хотя бы один том и ни один не
    // ответил противоречиво. Ноль томов — тоже «неизвестно»: диск без
    // перечисленных томов нельзя считать полным на 100 %.
    bool anyVolume = false;
    bool allSizesKnown = true;
    bool allConsistent = true;

    for (const Partition& partition : disk.partitions) {
        usage.partitionCount += 1;
        usage.partitionedBytes = saturatingAdd(usage.partitionedBytes, partition.lengthBytes);
        if (!partition.hasVolume) continue;

        const Volume& volume = partition.volume;
        usage.volumeCount += 1;
        anyVolume = true;
        const VolumeUsage volumeNumbers = volumeUsage(volume);
        usage.freeBytes = saturatingAdd(usage.freeBytes, volumeNumbers.freeBytes);
        usage.usedBytes = saturatingAdd(usage.usedBytes, volumeNumbers.usedBytes);
        if (volumeNumbers.sizesKnown) {
            usage.volumeBytes = saturatingAdd(usage.volumeBytes, volumeNumbers.totalBytes);
        } else {
            allSizesKnown = false;
        }
        if (!volumeNumbers.consistent) allConsistent = false;
        if (spansMultipleDisks(volume)) {
            usage.spanningVolumeCount += 1;
            usage.spanningFreeBytes = saturatingAdd(usage.spanningFreeBytes, volumeNumbers.freeBytes);
        }
        if (volume.readOnly || disk.readOnly) {
            usage.readOnlyBytes = saturatingAdd(usage.readOnlyBytes, volumeNumbers.totalBytes);
        }
    }

    usage.freeKnown = anyVolume && allSizesKnown && allConsistent;
    usage.unallocatedBytes = saturatingSub(disk.sizeBytes, usage.partitionedBytes);
    return usage;
}

InventoryUsage inventoryUsage(const std::vector<PhysicalDisk>& disks) {
    InventoryUsage usage;
    for (const PhysicalDisk& disk : disks) {
        usage.diskCount += 1;
        usage.totalBytes = saturatingAdd(usage.totalBytes, disk.sizeBytes);
        usage.partitionCount += static_cast<std::uint64_t>(disk.partitions.size());
    }
    for (const VolumeEntry& entry : collectVolumes(disks)) {
        const Volume& volume = *entry.volume;
        const VolumeUsage numbers = volumeUsage(volume);
        usage.volumeCount += 1;
        if (entry.spansMultipleDisks) usage.spanningVolumeCount += 1;
        if (numbers.sizesKnown) {
            usage.knownVolumeCount += 1;
            usage.volumeBytes = saturatingAdd(usage.volumeBytes, numbers.totalBytes);
            usage.freeBytes = saturatingAdd(usage.freeBytes, numbers.freeBytes);
            usage.usedBytes = saturatingAdd(usage.usedBytes, numbers.usedBytes);
        } else {
            usage.unknownVolumeCount += 1;
        }
        if (volume.encrypted) usage.encryptedBytes = saturatingAdd(usage.encryptedBytes, numbers.totalBytes);
        if (volume.readOnly || (entry.disk != nullptr && entry.disk->readOnly)) {
            usage.readOnlyBytes = saturatingAdd(usage.readOnlyBytes, numbers.totalBytes);
        }
        if (entry.disk != nullptr && entry.disk->removable) {
            usage.removableBytes = saturatingAdd(usage.removableBytes, numbers.totalBytes);
        }
    }
    usage.unallocatedBytes = saturatingSub(usage.totalBytes, usage.volumeBytes);
    usage.freeFraction = freeRatio(usage.volumeBytes, usage.freeBytes);
    usage.usedFraction = usedRatio(usage.volumeBytes, usage.usedBytes);
    usage.freeKnown = usage.volumeCount > 0 && usage.unknownVolumeCount == 0;
    return usage;
}

std::uint64_t writableFreeBytes(const std::vector<PhysicalDisk>& disks) {
    std::uint64_t freeBytes = 0;
    for (const VolumeEntry& entry : collectVolumes(disks)) {
        const Volume& volume = *entry.volume;
        if (volume.readOnly) continue;
        if (entry.disk != nullptr && entry.disk->readOnly) continue;
        if (!volumeUsage(volume).sizesKnown) continue;
        freeBytes = saturatingAdd(freeBytes, volume.freeBytes);
    }
    return freeBytes;
}

bool hasRoomFor(std::uint64_t freeBytes, std::uint64_t requiredBytes) noexcept {
    return freeBytes >= requiredBytes;
}

bool needsFreeSpaceWarning(const InventoryUsage& usage) noexcept {
    if (!usage.freeKnown) return false;  // нет данных — это не «мало места»
    if (usage.volumeBytes == 0) return false;
    return usage.freeFraction < kLowFreeFraction || usage.freeBytes < kLowFreeBytes;
}

// ---------------------------------------------------------------------------
// Перечисление и поиск
// ---------------------------------------------------------------------------

const char* toString(PartitionScheme scheme) noexcept {
    switch (scheme) {
        case PartitionScheme::Mbr: return "MBR";
        case PartitionScheme::Gpt: return "GPT";
        case PartitionScheme::Unknown: break;
    }
    return "неизвестно";
}

PartitionScheme schemeOf(const PhysicalDisk& disk) noexcept {
    bool anyGptType = false;
    bool anyMbrType = false;
    for (const Partition& partition : disk.partitions) {
        if (partition.hasGptType) anyGptType = true;
        if (partition.mbrType != 0) anyMbrType = true;
    }
    if (anyGptType) return PartitionScheme::Gpt;
    if (anyMbrType) return PartitionScheme::Mbr;
    return PartitionScheme::Unknown;
}

std::vector<VolumeEntry> collectVolumes(const std::vector<PhysicalDisk>& disks) {
    std::vector<VolumeEntry> entries;
    std::vector<std::string> seenGuids;
    for (const PhysicalDisk& disk : disks) {
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            const Volume& volume = partition.volume;
            if (!volume.volumeGuidPath.empty()) {
                const std::string key = pathKey(volume.volumeGuidPath);
                if (std::find(seenGuids.begin(), seenGuids.end(), key) != seenGuids.end()) continue;
                seenGuids.push_back(key);
            }
            VolumeEntry entry;
            entry.volume = &volume;
            entry.partition = &partition;
            entry.disk = &disk;
            entry.diskNumber = disk.number;
            entry.partitionIndex = partition.index;
            entry.extentCount = volume.diskExtents.size();
            entry.spansMultipleDisks = spansMultipleDisks(volume);
            entries.push_back(entry);
        }
    }
    return entries;
}

void sortInventory(std::vector<PhysicalDisk>& disks) {
    std::sort(disks.begin(), disks.end(), [](const PhysicalDisk& a, const PhysicalDisk& b) {
        if (a.number != b.number) return a.number < b.number;
        return a.devicePath < b.devicePath;
    });
    for (PhysicalDisk& disk : disks) {
        std::sort(disk.partitions.begin(), disk.partitions.end(),
                  [](const Partition& a, const Partition& b) {
                      if (a.offsetBytes != b.offsetBytes) return a.offsetBytes < b.offsetBytes;
                      return a.index < b.index;
                  });
        for (Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            std::sort(partition.volume.mountPoints.begin(), partition.volume.mountPoints.end(),
                      [](const std::string& a, const std::string& b) { return pathKey(a) < pathKey(b); });
            std::sort(partition.volume.diskExtents.begin(), partition.volume.diskExtents.end());
        }
    }
}

const PhysicalDisk* findDiskByNumber(const std::vector<PhysicalDisk>& disks, int number) noexcept {
    for (const PhysicalDisk& disk : disks) {
        if (disk.number == number) return &disk;
    }
    return nullptr;
}

const Partition* findPartition(const PhysicalDisk& disk, std::uint32_t index) noexcept {
    for (const Partition& partition : disk.partitions) {
        if (partition.index == index) return &partition;
    }
    return nullptr;
}

const Volume* findVolumeByGuidPath(const std::vector<PhysicalDisk>& disks,
                                   std::string_view volumeGuidPath) noexcept {
    if (volumeGuidPath.empty()) return nullptr;
    const std::string wanted = pathKey(volumeGuidPath);
    for (const PhysicalDisk& disk : disks) {
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            if (pathKey(partition.volume.volumeGuidPath) == wanted) return &partition.volume;
        }
    }
    return nullptr;
}

const Volume* findVolumeByMountPoint(const std::vector<PhysicalDisk>& disks,
                                     std::string_view mountPoint) noexcept {
    if (mountPoint.empty()) return nullptr;
    const std::string wanted = pathKey(mountPoint);
    for (const PhysicalDisk& disk : disks) {
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            for (const std::string& candidate : partition.volume.mountPoints) {
                if (pathKey(candidate) == wanted) return &partition.volume;
            }
        }
    }
    return nullptr;
}

const Volume* findVolumeForPath(const std::vector<PhysicalDisk>& disks, std::string_view path) noexcept {
    const Volume* best = nullptr;
    std::size_t bestLength = 0;
    for (const PhysicalDisk& disk : disks) {
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            for (const std::string& mountPoint : partition.volume.mountPoints) {
                if (!pathIsWithin(path, mountPoint)) continue;
                if (best != nullptr && mountPoint.size() <= bestLength) continue;
                best = &partition.volume;
                bestLength = mountPoint.size();
            }
        }
    }
    return best;
}

bool isSystemPartition(const Partition& partition) noexcept {
    return partition.kind == PartitionKind::System || partition.system;
}

bool isSystemVolume(const std::vector<PhysicalDisk>& disks, std::string_view volumeGuidPath) noexcept {
    const std::string wanted = pathKey(volumeGuidPath);
    for (const PhysicalDisk& disk : disks) {
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            if (pathKey(partition.volume.volumeGuidPath) != wanted) continue;
            return isSystemPartition(partition);
        }
    }
    return false;
}

bool hasDiskData(const PhysicalDisk& disk) noexcept {
    return disk.sizeBytes > 0 || !disk.partitions.empty() || !disk.devicePath.empty();
}

bool isDiskUnavailable(const PhysicalDisk& disk) noexcept { return !hasDiskData(disk); }

// ---------------------------------------------------------------------------
// Проверка согласованности
// ---------------------------------------------------------------------------

const char* toString(IssueSeverity severity) noexcept {
    switch (severity) {
        case IssueSeverity::Info: return "info";
        case IssueSeverity::Warning: return "warning";
        case IssueSeverity::Error: return "error";
    }
    return "info";
}

std::vector<DiskIssue> validateDisk(const PhysicalDisk& disk) {
    std::vector<DiskIssue> issues;
    const bool sizeKnown = disk.sizeBytes > 0;

    auto add = [&issues, &disk](IssueSeverity severity, bool hasPartition, std::uint32_t partitionIndex,
                                const char* code, std::string message) {
        DiskIssue issue;
        issue.severity = severity;
        issue.diskNumber = disk.number;
        issue.hasPartition = hasPartition;
        issue.partitionIndex = partitionIndex;
        issue.code = code;
        issue.message = std::move(message);
        issues.push_back(std::move(issue));
    };

    std::uint64_t partitionedBytes = 0;
    std::uint64_t volumeBytes = 0;
    for (const Partition& partition : disk.partitions) {
        partitionedBytes = saturatingAdd(partitionedBytes, partition.lengthBytes);
        if (partition.hasVolume) {
            volumeBytes = saturatingAdd(volumeBytes, partition.volume.totalBytes);
        }
    }

    if (!hasDiskData(disk)) {
        add(IssueSeverity::Warning, false, 0, "disk.no_data",
            "Устройство не ответило: ни размера, ни разделов, ни пути (FR-1: таймаут 2 с на устройство)");
    } else if (!sizeKnown) {
        add(IssueSeverity::Warning, false, 0, "disk.size_unknown",
            "Размер диска неизвестен, а разделы перечислены: проверить их границы нечем");
    }
    if (sizeKnown && partitionedBytes > disk.sizeBytes) {
        add(IssueSeverity::Error, false, 0, "disk.partitioned_exceeds_size",
            "Сумма длин разделов " + formatBytes(partitionedBytes) + " больше размера диска " +
                formatBytes(disk.sizeBytes));
    }
    if (sizeKnown && disk.sizeBytes > partitionedBytes) {
        add(IssueSeverity::Info, false, 0, "disk.unallocated",
            "Не размечено " + formatBytes(disk.sizeBytes - partitionedBytes) +
                ": это не занятое и не свободное место, в оценку оно не идёт");
    }
    if (sizeKnown && volumeBytes > partitionedBytes) {
        add(IssueSeverity::Warning, false, 0, "disk.volume_totals_exceed_partitioned",
            "Сумма объёмов томов " + formatBytes(volumeBytes) + " больше размера разметки " +
                formatBytes(partitionedBytes));
    }

    // Повторы индексов: платформа обязана нумеровать разделы по одному разу,
    // иначе «раздел 2» в интерфейсе означает две разные вещи.
    for (std::size_t i = 0; i < disk.partitions.size(); ++i) {
        for (std::size_t j = i + 1; j < disk.partitions.size(); ++j) {
            if (disk.partitions[i].index != disk.partitions[j].index) continue;
            add(IssueSeverity::Error, true, disk.partitions[j].index, "partition.duplicate_index",
                "Раздел с индексом " + std::to_string(disk.partitions[j].index) + " перечислен дважды");
        }
    }

    // Перекрытия: соседи в порядке смещения. Разность считаем вычитанием, а не
    // сложением: мусорные данные платформы не должны переполнить сумму и
    // скрыть перекрытие.
    std::vector<std::size_t> order;
    order.reserve(disk.partitions.size());
    for (std::size_t i = 0; i < disk.partitions.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&disk](std::size_t a, std::size_t b) {
        if (disk.partitions[a].offsetBytes != disk.partitions[b].offsetBytes) {
            return disk.partitions[a].offsetBytes < disk.partitions[b].offsetBytes;
        }
        return disk.partitions[a].index < disk.partitions[b].index;
    });
    for (std::size_t i = 1; i < order.size(); ++i) {
        const Partition& previous = disk.partitions[order[i - 1]];
        const Partition& current = disk.partitions[order[i]];
        if (previous.lengthBytes == 0) continue;
        if (current.offsetBytes - previous.offsetBytes < previous.lengthBytes) {
            add(IssueSeverity::Error, true, current.index, "partition.overlap",
                "Раздел " + std::to_string(current.index) + " (смещение " + std::to_string(current.offsetBytes) +
                    ") перекрывает раздел " + std::to_string(previous.index));
        }
    }

    for (const Partition& partition : disk.partitions) {
        const std::string indexText = std::to_string(partition.index);
        if (partition.lengthBytes == 0) {
            add(IssueSeverity::Warning, true, partition.index, "partition.zero_length",
                "Раздел " + indexText + " нулевой длины");
        }
        if (sizeKnown && (partition.offsetBytes > disk.sizeBytes ||
                          partition.lengthBytes > disk.sizeBytes - partition.offsetBytes)) {
            add(IssueSeverity::Error, true, partition.index, "partition.beyond_disk",
                "Раздел " + indexText + " выходит за конец диска: смещение " +
                    std::to_string(partition.offsetBytes) + ", длина " + std::to_string(partition.lengthBytes) +
                    ", размер диска " + std::to_string(disk.sizeBytes));
        }
        if (partition.kind == PartitionKind::Unallocated && partition.hasVolume) {
            add(IssueSeverity::Error, true, partition.index, "partition.unallocated_with_volume",
                "Раздел " + indexText + " помечен как неразмеченный, но на нём найден том");
        }
        if (!partition.hasVolume) continue;

        const Volume& volume = partition.volume;
        if (volume.freeBytes > volume.totalBytes) {
            add(IssueSeverity::Error, true, partition.index, "volume.free_exceeds_total",
                "Том на разделе " + indexText + " сообщил свободного больше, чем объём: " +
                    formatBytes(volume.freeBytes) + " при объёме " + formatBytes(volume.totalBytes));
        }
        if (!volume.mountPoints.empty() && volume.totalBytes == 0) {
            add(IssueSeverity::Warning, true, partition.index, "volume.no_size",
                "Том на разделе " + indexText + " смонтирован, но размер не пришёл: свободное место неизвестно");
        }
        if (volume.mountPoints.empty()) {
            add(IssueSeverity::Info, true, partition.index, "volume.no_mount_point",
                "Том на разделе " + indexText +
                    " без точки монтирования: в дереве дисков не виден и не попадает в фильтр «только с буквами»");
        }
        if (volume.fileSystem.empty()) {
            add(IssueSeverity::Info, true, partition.index, "volume.no_file_system",
                "Файловая система тома на разделе " + indexText +
                    " не определена (метка раздела, RAW или отказ чтения)");
        }
        if (volume.encrypted) {
            add(IssueSeverity::Info, true, partition.index, "volume.encrypted",
                "Том на разделе " + indexText + " зашифрован: содержимое для подсчёта недоступно");
        }
        if (volume.readOnly || disk.readOnly) {
            add(IssueSeverity::Info, true, partition.index, "volume.read_only",
                "Том на разделе " + indexText + " только для чтения: удалять на нём нельзя");
        }
        if (volume.dirty) {
            add(IssueSeverity::Warning, true, partition.index, "volume.dirty",
                "Файловая система тома на разделе " + indexText +
                    " не выгружена корректно: удалять на нём нельзя");
        }
        if (volume.diskExtents.empty()) {
            add(IssueSeverity::Info, true, partition.index, "volume.no_extents",
                "Привязка тома на разделе " + indexText + " к диску неизвестна (нет disk extent'ов)");
        } else {
            for (const auto& extent : volume.diskExtents) {
                if (extent.first != disk.number || !sizeKnown) continue;
                if (extent.second > disk.sizeBytes) {
                    add(IssueSeverity::Error, true, partition.index, "volume.extent_beyond_disk",
                        "Том на разделе " + indexText + " начинается за концом диска: смещение " +
                            std::to_string(extent.second));
                }
            }
            if (spansMultipleDisks(volume)) {
                add(IssueSeverity::Info, true, partition.index, "volume.spans_disks",
                    "Том на разделе " + indexText +
                        " занимает место на нескольких дисках: учтён на первом, на остальных не суммируется");
            }
        }
        if (partition.lengthBytes > 0 && volume.totalBytes > partition.lengthBytes) {
            add(IssueSeverity::Warning, true, partition.index, "volume.total_exceeds_partition",
                "Объём тома на разделе " + indexText + " больше размера самого раздела");
        }
    }

    return issues;
}

std::vector<DiskIssue> validateInventory(const std::vector<PhysicalDisk>& disks) {
    std::vector<DiskIssue> issues;

    for (std::size_t i = 0; i < disks.size(); ++i) {
        for (std::size_t j = i + 1; j < disks.size(); ++j) {
            if (disks[i].number != disks[j].number) continue;
            DiskIssue issue;
            issue.severity = IssueSeverity::Error;
            issue.diskNumber = disks[j].number;
            issue.code = "disk.duplicate_number";
            issue.message = "Номер диска " + std::to_string(disks[j].number) + " встречается дважды";
            issues.push_back(std::move(issue));
        }
    }

    for (const PhysicalDisk& disk : disks) {
        for (DiskIssue& issue : validateDisk(disk)) issues.push_back(std::move(issue));
    }

    // Точка монтирования указывает ровно на один том. Две — это либо ошибка
    // платформы, либо подобный том, смонтированный дважды; очистка по такому
    // пути была бы неоднозначной, поэтому считаем ошибкой.
    struct MountRef {
        std::string key;
        int diskNumber{-1};
        std::uint32_t partitionIndex{};
    };
    std::vector<MountRef> mounts;
    for (const PhysicalDisk& disk : disks) {
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            for (const std::string& mountPoint : partition.volume.mountPoints) {
                MountRef ref;
                ref.key = pathKey(mountPoint);
                if (ref.key.empty()) continue;
                ref.diskNumber = disk.number;
                ref.partitionIndex = partition.index;
                mounts.push_back(std::move(ref));
            }
        }
    }
    std::sort(mounts.begin(), mounts.end(), [](const MountRef& a, const MountRef& b) { return a.key < b.key; });
    for (std::size_t i = 1; i < mounts.size(); ++i) {
        if (mounts[i].key != mounts[i - 1].key) continue;
        DiskIssue issue;
        issue.severity = IssueSeverity::Error;
        issue.diskNumber = mounts[i].diskNumber;
        issue.hasPartition = true;
        issue.partitionIndex = mounts[i].partitionIndex;
        issue.code = "mount_point.conflict";
        issue.message = "Точка монтирования " + mounts[i].key +
                        " принадлежит двум томам (диск " + std::to_string(mounts[i - 1].diskNumber) +
                        " и диск " + std::to_string(mounts[i].diskNumber) + ")";
        issues.push_back(std::move(issue));
    }

    // Повтор volumeGuidPath на разных разделах: один и тот же том в списке
    // дважды. В агрегатах он считается один раз, но данные платформы стоит
    // показать как требующие внимания.
    struct GuidRef {
        std::string key;
        int diskNumber{-1};
        std::uint32_t partitionIndex{};
    };
    std::vector<GuidRef> guids;
    for (const PhysicalDisk& disk : disks) {
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume || partition.volume.volumeGuidPath.empty()) continue;
            GuidRef ref;
            ref.key = pathKey(partition.volume.volumeGuidPath);
            ref.diskNumber = disk.number;
            ref.partitionIndex = partition.index;
            guids.push_back(std::move(ref));
        }
    }
    std::sort(guids.begin(), guids.end(), [](const GuidRef& a, const GuidRef& b) { return a.key < b.key; });
    for (std::size_t i = 1; i < guids.size(); ++i) {
        if (guids[i].key != guids[i - 1].key) continue;
        DiskIssue issue;
        issue.severity = IssueSeverity::Warning;
        issue.diskNumber = guids[i].diskNumber;
        issue.hasPartition = true;
        issue.partitionIndex = guids[i].partitionIndex;
        issue.code = "volume.duplicate_guid";
        issue.message = "Том " + guids[i].key + " встречается в списке дважды (диск " +
                        std::to_string(guids[i - 1].diskNumber) + " и диск " + std::to_string(guids[i].diskNumber) +
                        "): в агрегатах учтён один раз";
        issues.push_back(std::move(issue));
    }

    return issues;
}

bool hasErrors(const std::vector<DiskIssue>& issues) noexcept {
    for (const DiskIssue& issue : issues) {
        if (issue.severity == IssueSeverity::Error) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Текст
// ---------------------------------------------------------------------------

std::string describeVolume(const Volume& volume) {
    const VolumeUsage usage = volumeUsage(volume);
    std::string out;
    if (volume.mountPoints.empty()) {
        out = volume.volumeGuidPath.empty() ? std::string("(без тома и монтирования)") : volume.volumeGuidPath;
    } else {
        for (std::size_t i = 0; i < volume.mountPoints.size(); ++i) {
            if (i > 0) out += ", ";
            out += volume.mountPoints[i];
        }
    }
    out += " — " + orUnknown(volume.fileSystem);
    if (!volume.label.empty()) out += " «" + volume.label + "»";
    out += ", объём " + sizeText(usage.totalBytes, usage.sizesKnown);
    out += ", свободно " + sizeText(usage.freeBytes, usage.sizesKnown);
    if (usage.sizesKnown) out += " (" + formatPercent(usage.freeFraction) + ")";
    std::string flags;
    appendFlag(flags, volume.encrypted, "шифрованный");
    appendFlag(flags, volume.readOnly, "только чтение");
    appendFlag(flags, volume.dirty, "не выгружен");
    if (volume.mountPoints.empty()) appendFlag(flags, true, "без точки монтирования");
    if (!flags.empty()) out += " [" + flags + "]";
    return out;
}

std::string describeDisk(const PhysicalDisk& disk) {
    const DiskUsage usage = diskUsage(disk);
    std::string out = "Диск " + std::to_string(disk.number) + ": " + orUnknown(disk.model);
    if (!disk.devicePath.empty()) out += " (" + disk.devicePath + ")";
    out += " [" + std::string(busTypeName(disk.bus)) + "]";
    out += ", размер " + sizeText(usage.sizeBytes, usage.sizeKnown);
    out += ", разметка " + std::string(toString(schemeOf(disk)));
    out += ", свободно " + sizeText(usage.freeBytes, usage.freeKnown);
    if (usage.freeKnown) out += " (" + formatPercent(freeRatio(usage.volumeBytes, usage.freeBytes)) + " от томов)";
    std::string flags;
    appendFlag(flags, disk.removable, "съёмный");
    appendFlag(flags, disk.readOnly, "только чтение");
    appendFlag(flags, disk.trimSupported, "TRIM");
    appendFlag(flags, disk.smartAvailable, "SMART");
    if (!flags.empty()) out += " [" + flags + "]";
    if (usage.spanningVolumeCount > 0) {
        out += ", томов через несколько дисков: " + std::to_string(usage.spanningVolumeCount);
    }
    if (disk.partitions.empty()) {
        out += "\n  разделы не перечислены";
    }
    for (const Partition& partition : disk.partitions) {
        out += "\n" + describePartition(disk, partition);
    }
    if (usage.unallocatedBytes > 0) {
        out += "\n  не размечено: " + formatBytes(usage.unallocatedBytes);
    }
    return out;
}

std::string describeIssue(const DiskIssue& issue) {
    std::string out = "[" + std::string(toString(issue.severity)) + "] " + issue.code;
    if (issue.diskNumber >= 0) out += ", диск " + std::to_string(issue.diskNumber);
    if (issue.hasPartition) out += ", раздел " + std::to_string(issue.partitionIndex);
    out += ": " + issue.message;
    return out;
}

// Счётчики карты разделов идут подписями («Дисков:», «разделов:», «томов:»),
// поэтому числа голые: подпись уже называет предмет, а существительное после
// числа было бы вторым словом о том же самом («Дисков: 2 файлов», D-70).
std::string toText(const std::vector<PhysicalDisk>& disks) {
    const InventoryUsage usage = inventoryUsage(disks);
    std::string out = "MrProper: карта разделов\n";
    out += "Дисков: " + std::to_string(usage.diskCount) + ", разделов: " + std::to_string(usage.partitionCount) +
           ", томов: " + std::to_string(usage.volumeCount);
    if (usage.spanningVolumeCount > 0) {
        out += " (через несколько дисков: " + std::to_string(usage.spanningVolumeCount) + ")";
    }
    if (usage.unknownVolumeCount > 0) {
        out += ", размер не получен у: " + std::to_string(usage.unknownVolumeCount);
    }
    out += "\n";
    out += "Диски: " + formatBytes(usage.totalBytes) + ", из них в файловых системах: " +
           formatBytes(usage.volumeBytes) + ", не размечено: " + formatBytes(usage.unallocatedBytes) + "\n";
    out += "Занято: " + formatBytes(usage.usedBytes) + ", свободно: " + formatBytes(usage.freeBytes);
    if (usage.freeKnown) {
        out += " (" + formatPercent(usage.freeFraction) + ")";
        if (needsFreeSpaceWarning(usage)) out += " — мало места";
    } else {
        out += " (нет данных)";
    }
    out += "\n";
    if (usage.encryptedBytes > 0) out += "Зашифровано: " + formatBytes(usage.encryptedBytes) + "\n";
    if (usage.readOnlyBytes > 0) out += "Только чтение: " + formatBytes(usage.readOnlyBytes) + "\n";
    out += "\n";
    for (const PhysicalDisk& disk : disks) {
        out += describeDisk(disk) + "\n\n";
    }
    const std::vector<DiskIssue> issues = validateInventory(disks);
    if (!issues.empty()) {
        out += "Замечания к инвентаризации: " + std::to_string(issues.size()) + "\n";
        for (const DiskIssue& issue : issues) {
            out += "  " + describeIssue(issue) + "\n";
        }
    }
    return out;
}

const char* busTypeName(BusType bus) noexcept {
    switch (bus) {
        case BusType::Sata: return "Sata";
        case BusType::Nvme: return "Nvme";
        case BusType::Usb: return "Usb";
        case BusType::Scsi: return "Scsi";
        case BusType::Sd: return "Sd";
        case BusType::Sas: return "Sas";
        case BusType::Virtual: return "Virtual";
        case BusType::Raid: return "Raid";
        case BusType::Unknown: break;
    }
    return "Unknown";
}

const char* partitionKindName(PartitionKind kind) noexcept {
    switch (kind) {
        case PartitionKind::BasicData: return "BasicData";
        case PartitionKind::System: return "System";
        case PartitionKind::Msr: return "Msr";
        case PartitionKind::Recovery: return "Recovery";
        case PartitionKind::Oem: return "Oem";
        case PartitionKind::Efi: return "Efi";
        case PartitionKind::Reserved: return "Reserved";
        case PartitionKind::Unallocated: return "Unallocated";
        case PartitionKind::Unknown: break;
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Снимок инвентаризации
// ---------------------------------------------------------------------------

DiskInventory DiskInventory::fromDisks(std::vector<PhysicalDisk> disks) {
    DiskInventory inventory;
    inventory.disks_ = std::move(disks);
    sortInventory(inventory.disks_);
    inventory.usage_ = inventoryUsage(inventory.disks_);
    inventory.issues_ = validateInventory(inventory.disks_);
    return inventory;
}

std::vector<VolumeEntry> DiskInventory::volumes() const { return collectVolumes(disks_); }

const PhysicalDisk* DiskInventory::diskByNumber(int number) const noexcept {
    return findDiskByNumber(disks_, number);
}

DiskUsage DiskInventory::usageOf(int number) const noexcept {
    const PhysicalDisk* disk = findDiskByNumber(disks_, number);
    if (disk == nullptr) return DiskUsage{};
    return diskUsage(*disk);
}

const Volume* DiskInventory::findVolumeByGuidPath(std::string_view volumeGuidPath) const noexcept {
    return mrproper::core::findVolumeByGuidPath(disks_, volumeGuidPath);
}

const Volume* DiskInventory::findVolumeForPath(std::string_view path) const noexcept {
    return mrproper::core::findVolumeForPath(disks_, path);
}

std::uint64_t DiskInventory::writableFree() const { return writableFreeBytes(disks_); }

std::string DiskInventory::toText() const { return mrproper::core::toText(disks_); }

}  // namespace mrproper::core
