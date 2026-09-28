#include "sizing.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "units.hpp"

namespace mrproper::core {
namespace {

// Скобки вокруг max() — страховка от макроса windows.h в чужом включении.
constexpr std::uint64_t kMaxBytes = (std::numeric_limits<std::uint64_t>::max)();

bool inScope(SafetyLevel safety, SizingScope scope) noexcept {
    switch (scope) {
        case SizingScope::All: return true;
        case SizingScope::NotRisky: return safety != SafetyLevel::Risky;
        case SizingScope::SafeOnly: return safety == SafetyLevel::Safe;
    }
    return true;  // все значения перечисления обработаны
}

// Один кандидат в агрегат. Общий шаг для aggregateSizes и groupSizes, чтобы
// агрегат по дереву и агрегат по группе считались одинаково.
void accumulateCandidate(SizingTotals& totals, const CleanupCandidate& candidate) noexcept {
    totals.itemCount = saturatingAdd(totals.itemCount, 1);
    totals.fileCount = saturatingAdd(totals.fileCount, candidate.fileCount);
    totals.logicalBytes = saturatingAdd(totals.logicalBytes, candidate.logicalBytes);
    totals.allocatedBytes = saturatingAdd(totals.allocatedBytes, candidate.allocatedBytes);

    const ReclaimEstimate estimate = estimateReclaim(candidate);
    totals.reclaimableBytes = saturatingAdd(totals.reclaimableBytes, estimate.bytes);
    if (estimate.estimated) {
        totals.reclaimableEstimatedBytes = saturatingAdd(totals.reclaimableEstimatedBytes, estimate.bytes);
    }

    const SizeBreakdown breakdown = analyzeSize(candidate.logicalBytes, candidate.allocatedBytes);
    totals.sparseBytes = saturatingAdd(totals.sparseBytes, breakdown.sparseBytes);
    totals.overheadBytes = saturatingAdd(totals.overheadBytes, breakdown.overheadBytes);
}

std::string orUnknown(const std::string& value) {
    return value.empty() ? std::string(kUnknownGroupKey) : value;
}

std::string groupKey(const CleanupCandidate& candidate, GroupBy groupBy) {
    switch (groupBy) {
        case GroupBy::Category: return orUnknown(candidate.category);
        case GroupBy::RuleId: return orUnknown(candidate.ruleId);
        case GroupBy::Safety: return toString(candidate.safety);
        case GroupBy::None: break;
    }
    return std::string();
}

}  // namespace

const char* toString(SizeDisposition disposition) noexcept {
    switch (disposition) {
        case SizeDisposition::Empty: return "empty";
        case SizeDisposition::Exact: return "exact";
        case SizeDisposition::Sparse: return "sparse";
        case SizeDisposition::Rounded: return "rounded";
    }
    return "exact";
}

SizeDisposition classifySize(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept {
    if (logicalBytes == 0 && allocatedBytes == 0) return SizeDisposition::Empty;
    if (allocatedBytes < logicalBytes) return SizeDisposition::Sparse;
    if (allocatedBytes > logicalBytes) return SizeDisposition::Rounded;
    return SizeDisposition::Exact;
}

SizeBreakdown analyzeSize(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept {
    SizeBreakdown breakdown;
    breakdown.logicalBytes = logicalBytes;
    breakdown.allocatedBytes = allocatedBytes;
    breakdown.disposition = classifySize(logicalBytes, allocatedBytes);
    if (allocatedBytes > logicalBytes) {
        breakdown.overheadBytes = allocatedBytes - logicalBytes;
    } else {
        breakdown.sparseBytes = logicalBytes - allocatedBytes;
    }
    breakdown.differenceBytes = breakdown.sparseBytes + breakdown.overheadBytes;
    return breakdown;
}

std::uint64_t alignToCluster(std::uint64_t bytes, std::uint64_t clusterBytes) noexcept {
    if (clusterBytes <= 1 || bytes == 0) return bytes;
    // Деление и остаток безопасны при bytes == UINT64_MAX: кластеров не больше
    // UINT64_MAX / 2, единица к нему не переполняет счётчик.
    std::uint64_t clusters = bytes / clusterBytes;
    if (bytes % clusterBytes != 0) ++clusters;
    if (clusters > kMaxBytes / clusterBytes) return kMaxBytes;
    return clusters * clusterBytes;
}

std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
    if (kMaxBytes - a < b) return kMaxBytes;
    return a + b;
}

std::uint64_t saturatingSub(std::uint64_t a, std::uint64_t b) noexcept {
    return a < b ? 0 : a - b;
}

ReclaimEstimate estimateReclaim(std::uint64_t logicalBytes, std::uint64_t allocatedBytes,
                                bool allocatedKnown) noexcept {
    ReclaimEstimate estimate;
    if (allocatedKnown) {
        estimate.bytes = allocatedBytes;
        return estimate;
    }
    // Аллоцированный размер неизвестен: показываем верхнюю границу, но помечаем
    // её как оценку, чтобы интерфейс не выдавал её за факт (§4 FR-4).
    estimate.bytes = logicalBytes;
    estimate.estimated = logicalBytes != allocatedBytes;
    return estimate;
}

ReclaimEstimate estimateReclaim(const CleanupCandidate& candidate) noexcept {
    const bool allocatedKnown = candidate.allocatedBytes != 0 || candidate.logicalBytes == 0;
    return estimateReclaim(candidate.logicalBytes, candidate.allocatedBytes, allocatedKnown);
}

ReclaimEstimate estimateReclaim(const CleanupCandidate& candidate, PlanAction action) noexcept {
    // Keep и SkipLocked не меняют диск: освобождать нечего (инвариант §6.3).
    if (action == PlanAction::Keep || action == PlanAction::SkipLocked) return ReclaimEstimate{};
    return estimateReclaim(candidate);
}

void SizeAccumulator::add(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept {
    totals_.itemCount = saturatingAdd(totals_.itemCount, 1);
    totals_.fileCount = saturatingAdd(totals_.fileCount, 1);
    totals_.logicalBytes = saturatingAdd(totals_.logicalBytes, logicalBytes);
    totals_.allocatedBytes = saturatingAdd(totals_.allocatedBytes, allocatedBytes);
    totals_.reclaimableBytes = saturatingAdd(totals_.reclaimableBytes, allocatedBytes);
    // sparseBytes/overheadBytes не трогаем в addEstimated: неизвестный аллоцированный
    // размер — это не «разреженность», и называть его так было бы враньём в отчёте.
    if (allocatedBytes < logicalBytes) {
        totals_.sparseBytes = saturatingAdd(totals_.sparseBytes, logicalBytes - allocatedBytes);
    } else {
        totals_.overheadBytes = saturatingAdd(totals_.overheadBytes, allocatedBytes - logicalBytes);
    }
}

void SizeAccumulator::addEstimated(std::uint64_t logicalBytes) noexcept {
    totals_.itemCount = saturatingAdd(totals_.itemCount, 1);
    totals_.fileCount = saturatingAdd(totals_.fileCount, 1);
    totals_.logicalBytes = saturatingAdd(totals_.logicalBytes, logicalBytes);
    const ReclaimEstimate estimate = estimateReclaim(logicalBytes, 0, false);
    totals_.reclaimableBytes = saturatingAdd(totals_.reclaimableBytes, estimate.bytes);
    totals_.reclaimableEstimatedBytes = saturatingAdd(totals_.reclaimableEstimatedBytes, estimate.bytes);
}

void SizeAccumulator::addDirectory(std::uint64_t clusterBytes) noexcept {
    // Каталог сам занимает кластер, и при удалении освобождает его.
    totals_.itemCount = saturatingAdd(totals_.itemCount, 1);
    totals_.allocatedBytes = saturatingAdd(totals_.allocatedBytes, clusterBytes);
    totals_.reclaimableBytes = saturatingAdd(totals_.reclaimableBytes, clusterBytes);
    totals_.overheadBytes = saturatingAdd(totals_.overheadBytes, clusterBytes);
}

void SizeAccumulator::reset() noexcept { totals_ = SizingTotals{}; }

SizingTotals aggregateSizes(const std::vector<CleanupCandidate>& candidates, SizingScope scope,
                            int minConfidence) noexcept {
    SizingTotals totals;
    for (const CleanupCandidate& candidate : candidates) {
        if (!inScope(candidate.safety, scope)) continue;
        if (candidate.confidence < minConfidence) continue;
        accumulateCandidate(totals, candidate);
    }
    return totals;
}

SizingTotals aggregateSizes(const std::vector<CleanupCandidate>& candidates,
                            const std::vector<std::size_t>& selected, int minConfidence) noexcept {
    SizingTotals totals;
    // Маска вместо проверки «есть ли индекс в selected»: повторный выбор одного
    // элемента не должен удваивать освобождаемое.
    std::vector<char> counted(candidates.size(), 0);
    for (const std::size_t index : selected) {
        if (index >= counted.size() || counted[index] != 0) continue;
        counted[index] = 1;
        const CleanupCandidate& candidate = candidates[index];
        if (candidate.confidence < minConfidence) continue;
        accumulateCandidate(totals, candidate);
    }
    return totals;
}

std::vector<SizeGroup> groupSizes(const std::vector<CleanupCandidate>& candidates, GroupBy groupBy,
                                  SizingScope scope, int minConfidence) {
    std::vector<SizeGroup> groups;
    if (groupBy == GroupBy::None) {
        groups.push_back(SizeGroup{std::string(), aggregateSizes(candidates, scope, minConfidence)});
        return groups;
    }

    std::map<std::string, std::size_t> slotOf;
    for (const CleanupCandidate& candidate : candidates) {
        if (!inScope(candidate.safety, scope)) continue;
        if (candidate.confidence < minConfidence) continue;
        const std::string key = groupKey(candidate, groupBy);
        const auto found = slotOf.find(key);
        std::size_t slot = 0;
        if (found == slotOf.end()) {
            slot = groups.size();
            slotOf.emplace(key, slot);
            groups.push_back(SizeGroup{key, SizingTotals{}});
        } else {
            slot = found->second;
        }
        accumulateCandidate(groups[slot].totals, candidate);
    }

    std::sort(groups.begin(), groups.end(), [](const SizeGroup& a, const SizeGroup& b) {
        if (a.totals.reclaimableBytes != b.totals.reclaimableBytes) {
            return a.totals.reclaimableBytes > b.totals.reclaimableBytes;
        }
        return a.key < b.key;
    });
    return groups;
}

std::string describeSizeUsage(const CleanupCandidate& candidate, int decimals) {
    const SizeBreakdown breakdown = analyzeSize(candidate.logicalBytes, candidate.allocatedBytes);
    if (breakdown.disposition == SizeDisposition::Rounded && candidate.logicalBytes == 0) {
        // Каталог: логических данных нет, а место кластер всё равно занимает.
        return "каталог: " + formatBytes(breakdown.allocatedBytes, decimals) + " на диске, логических данных нет";
    }
    switch (breakdown.disposition) {
        case SizeDisposition::Empty:
            return "пусто: ни логического, ни аллоцированного размера";
        case SizeDisposition::Exact:
            return formatBytes(breakdown.allocatedBytes, decimals) + " на диске, столько же логических";
        case SizeDisposition::Sparse:
            return formatBytes(breakdown.allocatedBytes, decimals) + " на диске из " +
                   formatBytes(breakdown.logicalBytes, decimals) +
                   " логических — разреженный или сжатый файл";
        case SizeDisposition::Rounded:
            return formatBytes(breakdown.allocatedBytes, decimals) + " на диске при " +
                   formatBytes(breakdown.logicalBytes, decimals) +
                   " логических — выравнивание по кластеру или сжатие";
    }
    return std::string();
}

std::string describeTotals(const SizingTotals& totals, int decimals) {
    std::string text = "освободится " + formatBytes(totals.reclaimableBytes, decimals) +
                       " по аллоцированному размеру";
    if (totals.reclaimableEstimatedBytes != 0) {
        text += ", из них " + formatBytes(totals.reclaimableEstimatedBytes, decimals) +
                " — оценка по логическому размеру";
    }
    if (totals.sparseBytes != 0) {
        text += "; разрежено " + formatBytes(totals.sparseBytes, decimals);
    }
    if (totals.overheadBytes != 0) {
        text += "; выравнивание по кластерам " + formatBytes(totals.overheadBytes, decimals);
    }
    text += "; " + formatCount(totals.fileCount);
    return text;
}

}  // namespace mrproper::core
