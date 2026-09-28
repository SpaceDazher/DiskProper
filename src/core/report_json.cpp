// Детерминированный JSON-отчёт: карта разделов, кандидаты, операции, ошибки
// (SPEC §4 FR-8). Порядок ключей зафиксирован в заголовке — он и есть контракт
// golden-теста: перестановка ключей ломает эталон так же, как изменение значения.
#include "report_json.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"
#include "model.hpp"

namespace mrproper::core {
namespace {

using mrproper::json::Value;

// Числа в mrproper::json::Value — double, поэтому целое выше 2^53 выводится с
// потерей точности. 2^53 байт — 9 петабайт; такого диска не существует, но
// «не существует» — не проверка: validateReport отказывается принимать такой
// отчёт, иначе тихо испорченный размер ушёл бы в баг-репорт как точный.
constexpr double kMaxExactInteger = 9007199254740992.0;  // 2^53

// Единственная точка, где целое становится числом JSON: приведение делается
// явно один раз, чтобы опечатка в static_cast не проскочила мимо глаз.
template <typename T>
Value num(T value) {
    return Value(static_cast<double>(static_cast<std::int64_t>(value)));
}

bool exactInJson(std::uint64_t value) {
    return static_cast<double>(value) < kMaxExactInteger;
}

// Токены перечислений. model.hpp объявляет toString() для них, но определений в
// ядре пока нет (файла model.cpp в задачах не значится), а модуль обязан
// линковаться сам по себе: поэтому имена свои, статические. Общих символов с
// будущим model.cpp они не создают. Слова и регистр те же, что у соседей
// (core::plan), иначе один и тот же уровень риска в отчёте назывался бы по-разному.
const char* safetyToken(SafetyLevel safety) {
    switch (safety) {
        case SafetyLevel::Safe: return "safe";
        case SafetyLevel::Review: return "review";
        case SafetyLevel::Risky: return "risky";
    }
    return "review";
}

const char* actionToken(PlanAction action) {
    switch (action) {
        case PlanAction::Delete: return "delete";
        case PlanAction::Trash: return "trash";
        case PlanAction::Keep: return "keep";
        case PlanAction::SkipLocked: return "skip-locked";
    }
    return "keep";
}

const char* partitionKindToken(PartitionKind kind) {
    switch (kind) {
        case PartitionKind::BasicData: return "basic-data";
        case PartitionKind::System: return "system";
        case PartitionKind::Msr: return "msr";
        case PartitionKind::Recovery: return "recovery";
        case PartitionKind::Oem: return "oem";
        case PartitionKind::Efi: return "efi";
        case PartitionKind::Reserved: return "reserved";
        case PartitionKind::Unallocated: return "unallocated";
        case PartitionKind::Unknown: return "unknown";
    }
    return "unknown";
}

const char* busToken(BusType bus) {
    switch (bus) {
        case BusType::Sata: return "sata";
        case BusType::Nvme: return "nvme";
        case BusType::Usb: return "usb";
        case BusType::Scsi: return "scsi";
        case BusType::Sd: return "sd";
        case BusType::Sas: return "sas";
        case BusType::Virtual: return "virtual";
        case BusType::Raid: return "raid";
        case BusType::Unknown: return "unknown";
    }
    return "unknown";
}

const char* statusToken(ReportOperationStatus status) {
    switch (status) {
        case ReportOperationStatus::Success: return "success";
        case ReportOperationStatus::Failed: return "failed";
        case ReportOperationStatus::Skipped: return "skipped";
        case ReportOperationStatus::Partial: return "partial";
    }
    return "success";
}

const char* reportKindToken(ReportKind kind) {
    switch (kind) {
        case ReportKind::Scan: return "scan";
        case ReportKind::Cleanup: return "cleanup";
        case ReportKind::DryRun: return "dry-run";
    }
    return "cleanup";
}

// Освобождённые байты: операция засчитана, если действие что-то удаляет, а статус
// не «пропущено». «Частично» освободившееся тоже идёт в итог — до скольки дошло,
// до того и освободилось (фактические байты платформа и кладёт в ReportOperation).
bool countsAsFreed(const ReportOperation& op) {
    if (op.status == ReportOperationStatus::Skipped) return false;
    return op.action == PlanAction::Delete || op.action == PlanAction::Trash;
}

Value volumeValue(const Volume& volume, const ReportOptions& options) {
    std::vector<Value> mountPoints;
    mountPoints.reserve(volume.mountPoints.size());
    for (const std::string& mountPoint : volume.mountPoints) mountPoints.push_back(Value(mountPoint));

    // Экстенты парой «диск + смещение»: массив массивов, а не объект — ключ был бы
    // номером диска, и порядок ключей зависел бы от нумерации дисков.
    std::vector<Value> diskExtents;
    diskExtents.reserve(volume.diskExtents.size());
    for (const auto& extent : volume.diskExtents) {
        diskExtents.push_back(Value::array({num(extent.first), num(extent.second)}));
    }

    // totalBytes меньше freeBytes бывает у повреждённого тома; «usedBytes» в этом
    // случае считаем нулём, а не вычитанием в обратную сторону.
    const std::uint64_t usedBytes = volume.totalBytes >= volume.freeBytes ? volume.totalBytes - volume.freeBytes : 0;

    return Value::object({
        {"guidPath", Value(options.maskVolumeGuids ? maskVolumeGuid(volume.volumeGuidPath) : volume.volumeGuidPath)},
        {"label", Value(volume.label)},
        {"fileSystem", Value(volume.fileSystem)},
        {"totalBytes", num(volume.totalBytes)},
        {"freeBytes", num(volume.freeBytes)},
        {"usedBytes", num(usedBytes)},
        {"encrypted", Value(volume.encrypted)},
        {"dirty", Value(volume.dirty)},
        {"readOnly", Value(volume.readOnly)},
        {"mountPoints", Value::array(std::move(mountPoints))},
        {"diskExtents", Value::array(std::move(diskExtents))},
    });
}

Value partitionValue(const Partition& partition, const ReportOptions& options) {
    return Value::object({
        {"index", num(partition.index)},
        {"offsetBytes", num(partition.offsetBytes)},
        {"lengthBytes", num(partition.lengthBytes)},
        {"kind", Value(partitionKindToken(partition.kind))},
        {"mbrType", num(partition.mbrType)},
        // gptType бывает null (MBR-диск): «типа нет» и «тип нулевой GUID» — разные
        // вещи, и пустой строкой они не сливаются.
        {"gptType", partition.hasGptType ? Value(guidToString(partition.gptType)) : Value()},
        {"gptName", Value(partition.gptName)},
        {"system", Value(partition.system)},
        {"boot", Value(partition.boot)},
        {"hidden", Value(partition.hidden)},
        {"hasVolume", Value(partition.hasVolume)},
        {"volume", partition.hasVolume ? volumeValue(partition.volume, options) : Value()},
    });
}

Value diskValue(const PhysicalDisk& disk, const ReportOptions& options) {
    std::vector<Value> partitions;
    partitions.reserve(disk.partitions.size());
    for (const Partition& partition : disk.partitions) partitions.push_back(partitionValue(partition, options));

    return Value::object({
        {"number", num(disk.number)},
        {"model", Value(disk.model)},
        {"serial", Value(options.maskSerials ? maskSerial(disk.serial) : disk.serial)},
        {"firmware", Value(disk.firmware)},
        {"bus", Value(busToken(disk.bus))},
        {"devicePath", Value(disk.devicePath)},
        {"sizeBytes", num(disk.sizeBytes)},
        {"removable", Value(disk.removable)},
        {"readOnly", Value(disk.readOnly)},
        {"trimSupported", Value(disk.trimSupported)},
        {"smartAvailable", Value(disk.smartAvailable)},
        {"partitionCount", num(disk.partitions.size())},
        {"partitions", Value::array(std::move(partitions))},
    });
}

Value candidateValue(const CleanupCandidate& candidate) {
    std::vector<Value> reasons;
    reasons.reserve(candidate.reasons.size());
    for (const std::string& reason : candidate.reasons) reasons.push_back(Value(reason));

    std::vector<Value> lockedBy;
    lockedBy.reserve(candidate.lockedBy.size());
    for (const ProcessRef& process : candidate.lockedBy) {
        lockedBy.push_back(Value::object({{"pid", num(process.pid)}, {"name", Value(process.name)}}));
    }

    return Value::object({
        {"ruleId", Value(candidate.ruleId)},
        {"category", Value(candidate.category)},
        {"path", Value(candidate.path)},
        {"displayName", Value(candidate.displayName)},
        {"logicalBytes", num(candidate.logicalBytes)},
        {"allocatedBytes", num(candidate.allocatedBytes)},
        {"fileCount", num(candidate.fileCount)},
        {"oldestWrite", num(candidate.oldestWrite)},
        {"newestWrite", num(candidate.newestWrite)},
        {"lastAccess", num(candidate.lastAccess)},
        {"safety", Value(safetyToken(candidate.safety))},
        {"confidence", num(candidate.confidence)},
        // locked — короткий ответ на вопрос пользователя «почему это не удалили?»;
        // lockedBy — подробности для отчёта о пропущенных путях (§9.4).
        {"locked", Value(!candidate.lockedBy.empty())},
        {"reasons", Value::array(std::move(reasons))},
        {"lockedBy", Value::array(std::move(lockedBy))},
    });
}

Value operationValue(const ReportOperation& op) {
    return Value::object({
        {"candidateIndex", num(op.candidateIndex)},
        {"action", Value(actionToken(op.action))},
        {"status", Value(statusToken(op.status))},
        {"category", Value(op.category)},
        {"displayName", Value(op.displayName)},
        {"path", Value(op.path)},
        {"bytes", num(op.bytes)},
        {"safety", Value(safetyToken(op.safety))},
        {"confidence", num(op.confidence)},
        {"attempts", num(op.attempts)},
        {"transactionId", Value(op.transactionId)},
        {"startedAtUnix", num(op.startedAtUnix)},
        {"finishedAtUnix", num(op.finishedAtUnix)},
        {"detail", Value(op.detail)},
    });
}

Value errorValue(const ReportError& error) {
    return Value::object({
        {"scope", Value(error.scope)},
        {"code", Value(error.code)},
        {"message", Value(error.message)},
        {"path", Value(error.path)},
        {"operation", Value(error.operation)},
        {"atUnix", num(error.atUnix)},
        {"count", num(error.count)},
    });
}

// Итоги по дискам — общая часть и summarizeReport, и partitionMapToJson, чтобы
// карта в отчёте и карта отдельным файлом не разошлись числами.
struct MapTotals {
    std::size_t diskCount{};
    std::size_t partitionCount{};
    std::size_t volumeCount{};
    std::uint64_t diskBytes{};
    std::uint64_t volumeBytes{};
};

MapTotals mapTotals(const std::vector<PhysicalDisk>& disks) {
    MapTotals totals;
    totals.diskCount = disks.size();
    for (const PhysicalDisk& disk : disks) {
        totals.diskBytes += disk.sizeBytes;
        totals.partitionCount += disk.partitions.size();
        for (const Partition& partition : disk.partitions) {
            if (!partition.hasVolume) continue;
            ++totals.volumeCount;
            totals.volumeBytes += partition.volume.totalBytes;
        }
    }
    return totals;
}

Value totalsValue(const ReportTotals& totals) {
    return Value::object({
        {"diskCount", num(totals.diskCount)},
        {"partitionCount", num(totals.partitionCount)},
        {"volumeCount", num(totals.volumeCount)},
        {"diskBytes", num(totals.diskBytes)},
        {"volumeBytes", num(totals.volumeBytes)},
        {"candidateCount", num(totals.candidateCount)},
        {"candidateBytes", num(totals.candidateBytes)},
        {"lockedCandidateCount", num(totals.lockedCandidateCount)},
        {"operationCount", num(totals.operationCount)},
        {"untouchedCount", num(totals.untouchedCount)},
        {"succeededCount", num(totals.succeededCount)},
        {"partialCount", num(totals.partialCount)},
        {"failedCount", num(totals.failedCount)},
        {"skippedCount", num(totals.skippedCount)},
        {"freedBytes", num(totals.freedBytes)},
        {"failedBytes", num(totals.failedBytes)},
        {"errorCount", num(totals.errorCount)},
        {"errorOccurrences", num(totals.errorOccurrences)},
    });
}

// Запись в отчёт: пустые секции тоже печатаются ([] вместо отсутствующего ключа).
// Читатель (человек или скрипт) не должен гадать, «есть ли такой раздел».
void collectDisks(const std::vector<PhysicalDisk>& disks, const ReportOptions& options, std::vector<Value>& out) {
    if (!options.includeDisks) return;
    out.reserve(disks.size());
    for (const PhysicalDisk& disk : disks) out.push_back(diskValue(disk, options));
}

void collectCandidates(const std::vector<CleanupCandidate>& candidates, const ReportOptions& options,
                       std::vector<Value>& out) {
    if (!options.includeCandidates) return;
    out.reserve(candidates.size());
    for (const CleanupCandidate& candidate : candidates) out.push_back(candidateValue(candidate));
}

void collectOperations(const std::vector<ReportOperation>& operations, std::vector<Value>& out) {
    out.reserve(operations.size());
    for (const ReportOperation& op : operations) out.push_back(operationValue(op));
}

void collectErrors(const std::vector<ReportError>& errors, const ReportOptions& options, std::vector<Value>& out) {
    if (!options.includeErrors) return;
    out.reserve(errors.size());
    for (const ReportError& error : errors) out.push_back(errorValue(error));
}

void checkConfidence(int confidence, const std::string& where, std::vector<std::string>& problems) {
    if (confidence < 0 || confidence > 100) {
        problems.push_back(where + ": уверенность " + std::to_string(confidence) + " вне [0, 100]");
    }
}

void checkBytes(std::uint64_t bytes, const std::string& where, std::vector<std::string>& problems) {
    if (!exactInJson(bytes)) {
        problems.push_back(where + ": значение " + std::to_string(bytes) + " байт не представимо точно в JSON");
    }
}

// Проверка операций — общая для выполненных и оставленных на месте: обе
// секции описывают одну и ту же работу, различается только исход.
void checkOperations(const std::vector<ReportOperation>& operations, std::size_t candidateCount, bool executed,
                     std::vector<std::string>& problems) {
    std::set<std::size_t> seen;  // поиск дубликатов, а не сортировка вывода
    for (std::size_t i = 0; i < operations.size(); ++i) {
        const ReportOperation& op = operations[i];
        const std::string where = (executed ? "операция " : "оставленный элемент ") + std::to_string(i);
        if (candidateCount != 0 && op.candidateIndex >= candidateCount) {
            problems.push_back(where + ": candidateIndex " + std::to_string(op.candidateIndex) +
                               " вне списка кандидатов (" + std::to_string(candidateCount) + ")");
        }
        if (candidateCount != 0 && !seen.insert(op.candidateIndex).second) {
            problems.push_back(where + ": две записи на кандидата " + std::to_string(op.candidateIndex));
        }
        checkConfidence(op.confidence, where, problems);
        checkBytes(op.bytes, where, problems);
        if (op.attempts == 0) problems.push_back(where + ": попыток не меньше одной, а стоит 0");
        if (op.startedAtUnix != 0 && op.finishedAtUnix != 0 && op.finishedAtUnix < op.startedAtUnix) {
            problems.push_back(where + ": окончание раньше начала");
        }
        // Инвариант §6.3: Keep и SkipLocked ничего не освобождают. Значит и в
        // отчёте у них ноль байт — иначе «освобождено» посчитается дважды.
        if (op.action == PlanAction::Keep || op.action == PlanAction::SkipLocked) {
            if (op.bytes != 0) {
                problems.push_back(where + ": у действия " + std::string(actionToken(op.action)) +
                                   " освобождается 0 байт, а указано " + std::to_string(op.bytes));
            }
            if (executed && op.status != ReportOperationStatus::Skipped) {
                problems.push_back(where + ": действие " + std::string(actionToken(op.action)) +
                                   " не выполняется, а статус «" + std::string(statusToken(op.status)) + "»");
            }
        }
    }
}

}  // namespace

const char* toString(ReportKind kind) {
    return reportKindToken(kind);
}

const char* toString(ReportOperationStatus status) {
    return statusToken(status);
}

std::string maskSerial(std::string_view serial) {
    if (serial.size() <= kSerialTailKept) return std::string(serial.size(), '*');
    return std::string(serial.size() - kSerialTailKept, '*') +
           std::string(serial.substr(serial.size() - kSerialTailKept));
}

std::string maskVolumeGuid(std::string_view volumeGuidPath) {
    static constexpr std::string_view kVolumePrefix = "\\\\?\\Volume{";
    if (volumeGuidPath.size() <= kVolumePrefix.size() + 1) return std::string(volumeGuidPath);
    if (volumeGuidPath.compare(0, kVolumePrefix.size(), kVolumePrefix) != 0) return std::string(volumeGuidPath);

    // Хвостовой разделитель («\\?\Volume{…}\») — не часть идентификатора, но
    // терять его нельзя: путь в отчёте должен остаться копируемым. Отрезаем,
    // маскируем и приклеиваем обратно.
    const bool trailingSeparator = volumeGuidPath.back() == '\\' || volumeGuidPath.back() == '/';
    const std::string_view body =
        trailingSeparator ? volumeGuidPath.substr(0, volumeGuidPath.size() - 1) : volumeGuidPath;
    if (body.back() != '}') return std::string(volumeGuidPath);

    std::string masked = std::string(kVolumePrefix) + "****}";
    if (trailingSeparator) masked.push_back(volumeGuidPath.back());
    return masked;
}

std::string guidToString(const Guid& guid) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < guid.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        const unsigned byte = guid[i];
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0Fu]);
    }
    return out;
}

ReportTotals summarizeReport(const Report& report) {
    ReportTotals totals;
    if (report.options.includeDisks) {
        const MapTotals map = mapTotals(report.disks);
        totals.diskCount = map.diskCount;
        totals.partitionCount = map.partitionCount;
        totals.volumeCount = map.volumeCount;
        totals.diskBytes = map.diskBytes;
        totals.volumeBytes = map.volumeBytes;
    }
    if (report.options.includeCandidates) {
        totals.candidateCount = report.candidates.size();
        for (const CleanupCandidate& candidate : report.candidates) {
            totals.candidateBytes += candidate.allocatedBytes;
            if (!candidate.lockedBy.empty()) ++totals.lockedCandidateCount;
        }
    }
    if (report.options.includeOperations) {
        totals.operationCount = report.operations.size();
        for (const ReportOperation& op : report.operations) {
            switch (op.status) {
                case ReportOperationStatus::Success: ++totals.succeededCount; break;
                case ReportOperationStatus::Partial: ++totals.partialCount; break;
                case ReportOperationStatus::Failed: ++totals.failedCount; break;
                case ReportOperationStatus::Skipped: ++totals.skippedCount; break;
            }
            if (countsAsFreed(op)) {
                totals.freedBytes += op.bytes;
            } else if (op.status == ReportOperationStatus::Failed) {
                totals.failedBytes += op.bytes;
            }
        }
    }
    if (report.options.includeUntouched) totals.untouchedCount = report.untouched.size();
    if (report.options.includeErrors) {
        totals.errorCount = report.errors.size();
        for (const ReportError& error : report.errors) totals.errorOccurrences += error.count;
    }
    return totals;
}

std::string reportToJson(const Report& report, int indent) {
    std::vector<Value> disks;
    std::vector<Value> candidates;
    std::vector<Value> operations;
    std::vector<Value> untouched;
    std::vector<Value> errors;

    collectDisks(report.disks, report.options, disks);
    collectCandidates(report.candidates, report.options, candidates);
    if (report.options.includeOperations) collectOperations(report.operations, operations);
    if (report.options.includeUntouched) collectOperations(report.untouched, untouched);
    collectErrors(report.errors, report.options, errors);

    const Value root = Value::object({
        {"schema", Value(report.schemaVersion)},
        {"kind", Value(reportKindToken(report.kind))},
        {"app",
         Value::object({
             {"version", Value(report.environment.appVersion)},
             {"pid", num(report.environment.pid)},
             {"rulesVersion", Value(report.environment.rulesVersion)},
         })},
        {"os",
         Value::object({
             {"caption", Value(report.environment.osCaption)},
             {"version", Value(report.environment.osVersion)},
             {"build", num(report.environment.osBuild)},
             {"architecture", Value(report.environment.architecture)},
         })},
        {"timing",
         Value::object({
             {"startedAtUnix", num(report.timing.startedAtUnix)},
             {"finishedAtUnix", num(report.timing.finishedAtUnix)},
             {"durationMs", num(report.timing.durationMs)},
         })},
        // Приватность в отчёте видна явно (SPEC §5): по privacy видно, что
        // серийники замаскированы, а не «потерялись».
        {"privacy",
         Value::object({
             {"serialsMasked", Value(report.options.maskSerials)},
             {"volumeGuidsMasked", Value(report.options.maskVolumeGuids)},
         })},
        {"totals", totalsValue(summarizeReport(report))},
        {"disks", Value::array(std::move(disks))},
        {"candidates", Value::array(std::move(candidates))},
        {"operations", Value::array(std::move(operations))},
        {"untouched", Value::array(std::move(untouched))},
        {"errors", Value::array(std::move(errors))},
        {"notes", Value(report.notes)},
    });

    std::string text = root.dump(indent);
    text.push_back('\n');
    return text;
}

std::string partitionMapToJson(const std::vector<PhysicalDisk>& disks, const ReportOptions& options, int indent) {
    std::vector<Value> diskValues;
    collectDisks(disks, options, diskValues);
    const MapTotals totals = mapTotals(disks);

    const Value root = Value::object({
        {"schema", Value(kReportJsonSchema)},
        {"kind", Value("disk-map")},
        {"privacy",
         Value::object({
             {"serialsMasked", Value(options.maskSerials)},
             {"volumeGuidsMasked", Value(options.maskVolumeGuids)},
         })},
        {"totals",
         Value::object({
             {"diskCount", num(totals.diskCount)},
             {"partitionCount", num(totals.partitionCount)},
             {"volumeCount", num(totals.volumeCount)},
             {"diskBytes", num(totals.diskBytes)},
             {"volumeBytes", num(totals.volumeBytes)},
         })},
        {"disks", Value::array(std::move(diskValues))},
    });

    std::string text = root.dump(indent);
    text.push_back('\n');
    return text;
}

std::vector<std::string> validateReport(const Report& report) {
    std::vector<std::string> problems;
    if (report.schemaVersion != kReportJsonSchema) {
        problems.push_back("неизвестная версия схемы отчёта: " + std::to_string(report.schemaVersion) + ", ждём " +
                           std::to_string(kReportJsonSchema));
    }
    if (report.timing.finishedAtUnix != 0 && report.timing.startedAtUnix != 0 &&
        report.timing.finishedAtUnix < report.timing.startedAtUnix) {
        problems.push_back("время окончания раньше времени начала");
    }
    if (report.timing.durationMs < 0) problems.push_back("длительность операции отрицательна");

    for (std::size_t i = 0; i < report.disks.size(); ++i) {
        const PhysicalDisk& disk = report.disks[i];
        const std::string where = "диск " + std::to_string(disk.number);
        checkBytes(disk.sizeBytes, where, problems);
        for (const Partition& partition : disk.partitions) {
            checkBytes(partition.lengthBytes, where + ", раздел " + std::to_string(partition.index), problems);
            if (!partition.hasVolume) continue;
            const Volume& volume = partition.volume;
            const std::string volumeWhere = where + ", том " + std::to_string(partition.index);
            checkBytes(volume.totalBytes, volumeWhere, problems);
            checkBytes(volume.freeBytes, volumeWhere, problems);
            // Том больше раздела не бывает; «dirty»-том с мусором в таблице — да.
            if (partition.lengthBytes != 0 && volume.totalBytes > partition.lengthBytes) {
                problems.push_back(volumeWhere + ": том больше раздела");
            }
        }
    }

    for (std::size_t i = 0; i < report.candidates.size(); ++i) {
        const CleanupCandidate& candidate = report.candidates[i];
        const std::string where = "кандидат " + std::to_string(i);
        checkConfidence(candidate.confidence, where, problems);
        checkBytes(candidate.logicalBytes, where, problems);
        checkBytes(candidate.allocatedBytes, where, problems);
        // Инвариант §6.3: allocatedBytes ≤ logicalBytes.
        if (candidate.allocatedBytes > candidate.logicalBytes) {
            problems.push_back(where + ": allocatedBytes " + std::to_string(candidate.allocatedBytes) +
                               " больше logicalBytes " + std::to_string(candidate.logicalBytes));
        }
        if (candidate.oldestWrite != 0 && candidate.newestWrite != 0 && candidate.oldestWrite > candidate.newestWrite) {
            problems.push_back(where + ": самый старый mtime позже самого нового");
        }
    }

    checkOperations(report.operations, report.candidates.size(), true, problems);
    checkOperations(report.untouched, report.candidates.size(), false, problems);

    for (std::size_t i = 0; i < report.errors.size(); ++i) {
        const ReportError& error = report.errors[i];
        const std::string where = "ошибка " + std::to_string(i);
        if (error.scope.empty()) problems.push_back(where + ": пустой раздел (scope)");
        if (error.message.empty()) problems.push_back(where + ": пустой текст ошибки");
        if (error.count == 0) problems.push_back(where + ": счётчик повторов 0");
    }

    return problems;
}

}  // namespace mrproper::core
