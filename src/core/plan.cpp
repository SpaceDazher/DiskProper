// Построение плана очистки: отбор, агрегаты, dry-run, снимок (SPEC §4 FR-5).
#include "plan.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "model.hpp"
#include "units.hpp"

namespace mrproper::core {
namespace {

// Хеш содержимого плана (FNV-1a, 64 бита). Нужен не для безопасности, а чтобы
// «показывали этот план или нет» проверялось точно, а не по указателю.
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mixValue(std::uint64_t& hash, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFull;
        hash *= kFnvPrime;
    }
}

// Токены перечислений для JSON и текста. model.hpp объявляет toString() для них,
// но определения в ядре пока нет (файла model.cpp в задачах не значится), а модуль
// обязан линковаться сам по себе: поэтому имена свои, статические. Свои они и в
// model.cpp, когда он появится, — общих символов не создаём.
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

// «1 элемент / 2 элемента / 5 элементов» — в units.cpp своя склонка для файлов,
// а здесь речь об элементах плана, поэтому слова свои.
const char* itemWord(std::size_t count) {
    const std::size_t mod100 = count % 100;
    if (mod100 >= 11 && mod100 <= 14) return "элементов";
    switch (count % 10) {
        case 1: return "элемент";
        case 2:
        case 3:
        case 4: return "элемента";
        default: return "элементов";
    }
}

std::string itemCount(std::size_t count) {
    return std::to_string(count) + " " + itemWord(count);
}

std::string actionLabel(PlanAction action) {
    switch (action) {
        case PlanAction::Delete: return "удалить";
        case PlanAction::Trash: return "в корзину";
        case PlanAction::Keep: return "оставить";
        case PlanAction::SkipLocked: return "пропуск (занято)";
    }
    return "оставить";
}

const char* profileLabel(SelectionProfile profile) {
    switch (profile) {
        case SelectionProfile::SafeOnly: return "только безопасное";
        case SelectionProfile::Recommended: return "рекомендуемый";
        case SelectionProfile::Everything: return "выбрать всё";
    }
    return "рекомендуемый";
}

// Имена приложений, удерживающих файлы: «msedge, Photos».
std::string describeLockers(const CleanupCandidate& candidate) {
    std::string names;
    for (std::size_t i = 0; i < candidate.lockedBy.size() && i < 3; ++i) {
        if (!names.empty()) names += ", ";
        const std::string& name = candidate.lockedBy[i].name;
        names += name.empty() ? ("pid " + std::to_string(candidate.lockedBy[i].pid)) : name;
    }
    if (candidate.lockedBy.size() > 3) names += ", …";
    return names;
}

ActionTotals& slotFor(PlanActionTotals& totals, PlanAction action) {
    switch (action) {
        case PlanAction::Delete: return totals.deleteOps;
        case PlanAction::Trash: return totals.trashOps;
        case PlanAction::Keep: return totals.keepOps;
        case PlanAction::SkipLocked: return totals.skipLockedOps;
    }
    return totals.keepOps;
}

CategoryAggregate& categorySlot(std::vector<CategoryAggregate>& categories, const std::string& category) {
    for (CategoryAggregate& entry : categories) {
        if (entry.category == category) return entry;
    }
    categories.push_back(CategoryAggregate{category});
    return categories.back();
}

// Порог из опций, приведённый к диапазону шкалы 0..100: иначе «порог 500» молча
// отобрал бы всё, а «порог -1» — ничего.
int effectiveThreshold(const PlanOptions& options) {
    return std::clamp(options.confidenceThreshold, 0, 100);
}

std::string thresholdReason(int confidence, int threshold) {
    return "уверенность " + std::to_string(confidence) + " ниже порога отбора " + std::to_string(threshold) +
           " — элемент не выбран по умолчанию";
}

PlanOperation makeOperation(std::size_t index, const CleanupCandidate& candidate, PlanAction action, SkipReason skip,
                            std::uint64_t bytes, const std::string& reason) {
    PlanOperation op;
    op.candidateIndex = index;
    op.action = action;
    op.skip = skip;
    op.category = candidate.category;
    op.displayName = candidate.displayName;
    op.path = candidate.path;
    op.bytes = bytes;
    op.safety = candidate.safety;
    op.confidence = candidate.confidence;
    op.reason = reason;
    return op;
}

std::string operationLine(std::size_t number, const PlanOperation& op) {
    std::string line = "  " + std::to_string(number) + ". [" + actionLabel(op.action) + "] ";
    line += op.displayName.empty() ? op.path : op.displayName;
    line += " — " + formatBytes(op.bytes);
    line += " · " + std::string(safetyToken(op.safety));
    line += " · уверенность " + std::to_string(op.confidence);
    if (!op.path.empty() && op.path != op.displayName) line += " · " + op.path;
    return line;
}

}  // namespace

const char* toString(SelectionProfile profile) {
    switch (profile) {
        case SelectionProfile::SafeOnly: return "safe-only";
        case SelectionProfile::Recommended: return "recommended";
        case SelectionProfile::Everything: return "everything";
    }
    return "recommended";
}

SelectionProfile selectionProfileFromString(const char* text) {
    if (text == nullptr) return SelectionProfile::Recommended;
    const std::string value(text);
    if (value == "safe-only" || value == "SafeOnly" || value == "safe") return SelectionProfile::SafeOnly;
    if (value == "everything" || value == "Everything" || value == "all") return SelectionProfile::Everything;
    return SelectionProfile::Recommended;
}

const char* toString(SkipReason reason) {
    switch (reason) {
        case SkipReason::None: return "none";
        case SkipReason::Locked: return "locked";
        case SkipReason::RiskyHidden: return "risky-hidden";
        case SkipReason::BelowConfidence: return "below-confidence";
        case SkipReason::ProfileFiltered: return "profile-filtered";
        case SkipReason::TooSmall: return "too-small";
    }
    return "none";
}

bool isRemovableAction(PlanAction action) {
    return action == PlanAction::Delete || action == PlanAction::Trash;
}

const ActionTotals& PlanActionTotals::forAction(PlanAction action) const {
    switch (action) {
        case PlanAction::Delete: return deleteOps;
        case PlanAction::Trash: return trashOps;
        case PlanAction::Keep: return keepOps;
        case PlanAction::SkipLocked: return skipLockedOps;
    }
    return keepOps;
}

SelectionDecision decideCandidate(const CleanupCandidate& candidate, const PlanOptions& options) {
    SelectionDecision decision;
    const int threshold = effectiveThreshold(options);

    // 1. Файлы держит работающее приложение: удалить их нельзя, обещать место
    //    тоже нельзя (FR-5 Skip (locked), FR-6 блокировки -> Skip).
    if (!candidate.lockedBy.empty()) {
        decision.action = PlanAction::SkipLocked;
        decision.skip = SkipReason::Locked;
        decision.reason = "файлы держат " + itemCount(candidate.lockedBy.size()) + ": " + describeLockers(candidate) +
                          " — закрыть приложение и повторить";
        return decision;
    }

    // 2. Risky по умолчанию скрыт: только «показать все» с подтверждением (FR-4, §12).
    if (candidate.safety == SafetyLevel::Risky && !options.allowRisky) {
        decision.skip = SkipReason::RiskyHidden;
        decision.reason = "уровень Risky скрыт по умолчанию — включить может только пользователь";
        return decision;
    }

    // 3. Слишком мелкий вклад в освобождение места.
    if (options.minReclaimBytes != 0 && candidate.allocatedBytes < options.minReclaimBytes) {
        decision.skip = SkipReason::TooSmall;
        decision.reason = "объём " + formatBytes(candidate.allocatedBytes) + " ниже минимального порога " +
                          formatBytes(options.minReclaimBytes);
        return decision;
    }

    // 4. Профиль отбора и порог уверенности.
    switch (options.profile) {
        case SelectionProfile::SafeOnly:
            if (candidate.safety != SafetyLevel::Safe) {
                decision.skip = SkipReason::ProfileFiltered;
                decision.reason = "профиль «только безопасное»: элемент помечен " +
                                  std::string(safetyToken(candidate.safety));
                return decision;
            }
            break;
        case SelectionProfile::Recommended:
        case SelectionProfile::Everything:
            break;
    }
    if (options.profile != SelectionProfile::Everything && candidate.confidence < threshold) {
        decision.skip = SkipReason::BelowConfidence;
        decision.reason = thresholdReason(candidate.confidence, threshold);
        return decision;
    }

    // 5. Выбран. Куда именно: в корзину приложения (её можно восстановить) или
    //    напрямую — большие кэши в корзине раздувают её (FR-7, ADR-005/006).
    decision.selected = true;
    if (options.useTrash && candidate.allocatedBytes <= options.trashDirectDeleteAboveBytes) {
        decision.action = PlanAction::Trash;
        decision.reason = "выбран по умолчанию; удаление через корзину приложения — операцию можно отменить";
    } else if (options.useTrash) {
        decision.action = PlanAction::Delete;
        decision.reason = "выбран по умолчанию; объём больше " + formatBytes(options.trashDirectDeleteAboveBytes) +
                          " — прямое удаление с записью в журнал";
    } else {
        decision.action = PlanAction::Delete;
        decision.reason = "выбран по умолчанию; прямое удаление с записью в журнал";
    }
    return decision;
}

CleanupPlan buildPlan(const std::vector<CleanupCandidate>& candidates, const PlanOptions& options) {
    CleanupPlan plan;
    plan.options = options;
    plan.dryRun = options.dryRun;
    plan.items.reserve(candidates.size());
    plan.totals.candidateCount = candidates.size();

    std::vector<CategoryAggregate> categories;

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const CleanupCandidate& candidate = candidates[index];
        const SelectionDecision decision = decideCandidate(candidate, options);

        CleanupPlanItem item;
        item.candidateIndex = index;
        item.action = decision.action;
        item.reclaimBytes = decision.selected ? candidate.allocatedBytes : 0;
        plan.items.push_back(item);

        ActionTotals& action = slotFor(plan.totals.byAction, decision.action);
        ++action.count;
        action.bytes += item.reclaimBytes;

        if (decision.selected) {
            ++plan.totals.selectedCount;
            plan.totals.selectedBytes += candidate.allocatedBytes;
        } else {
            switch (decision.skip) {
                case SkipReason::RiskyHidden: ++plan.totals.hiddenRiskyCount; break;
                case SkipReason::BelowConfidence: ++plan.totals.belowThresholdCount; break;
                case SkipReason::TooSmall: ++plan.totals.tooSmallCount; break;
                case SkipReason::ProfileFiltered: ++plan.totals.profileFilteredCount; break;
                case SkipReason::Locked:
                case SkipReason::None: break;
            }
        }

        // «Выбрать всё» и «только Safe» считаем по тому, что удалить вообще можно:
        // заблокированные файлы не входят ни в одну из цифр.
        if (decision.action != PlanAction::SkipLocked) {
            ++plan.totals.allCount;
            plan.totals.allBytes += candidate.allocatedBytes;
        }
        if (candidate.safety == SafetyLevel::Safe && decision.action != PlanAction::SkipLocked) {
            ++plan.totals.safeOnlyCount;
            plan.totals.safeOnlyBytes += candidate.allocatedBytes;
        }

        CategoryAggregate& category = categorySlot(categories, candidate.category);
        ++category.candidateCount;
        if (decision.action != PlanAction::SkipLocked) category.allBytes += candidate.allocatedBytes;
        if (candidate.safety == SafetyLevel::Safe && decision.action != PlanAction::SkipLocked) {
            category.safeOnlyBytes += candidate.allocatedBytes;
        }
        if (decision.selected) {
            ++category.selectedCount;
            category.selectedBytes += candidate.allocatedBytes;
        }
    }

    // Крупные категории первыми — так список читается как «с чего начать».
    std::stable_sort(categories.begin(), categories.end(), [](const CategoryAggregate& a, const CategoryAggregate& b) {
        if (a.selectedBytes != b.selectedBytes) return a.selectedBytes > b.selectedBytes;
        return a.category < b.category;
    });
    plan.categories = std::move(categories);
    return plan;
}

bool CleanupPlan::empty() const {
    return items.empty();
}

std::size_t CleanupPlan::operationCount() const {
    return totals.byAction.deleteOps.count + totals.byAction.trashOps.count;
}

const CleanupPlanItem* CleanupPlan::item(std::size_t candidateIndex) const {
    for (const CleanupPlanItem& item : items) {
        if (item.candidateIndex == candidateIndex) return &item;
    }
    return nullptr;
}

std::vector<std::size_t> CleanupPlan::operationIndexes() const {
    std::vector<std::size_t> indexes;
    indexes.reserve(operationCount());
    for (const CleanupPlanItem& item : items) {
        if (isRemovableAction(item.action)) indexes.push_back(item.candidateIndex);
    }
    return indexes;
}

std::uint64_t CleanupPlan::planSignature() const {
    std::uint64_t hash = kFnvOffsetBasis;
    mixValue(hash, static_cast<std::uint64_t>(options.profile));
    mixValue(hash, static_cast<std::uint64_t>(effectiveThreshold(options)));
    mixValue(hash, options.allowRisky ? 1ull : 0ull);
    mixValue(hash, options.useTrash ? 1ull : 0ull);
    mixValue(hash, options.minReclaimBytes);
    mixValue(hash, dryRun ? 1ull : 0ull);
    for (const CleanupPlanItem& item : items) {
        mixValue(hash, static_cast<std::uint64_t>(item.candidateIndex));
        mixValue(hash, static_cast<std::uint64_t>(item.action));
        mixValue(hash, item.reclaimBytes);
    }
    return hash;
}

DryRunReport makeDryRunReport(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan) {
    DryRunReport report;
    report.dryRun = plan.dryRun;
    report.totalBytes = plan.totals.selectedBytes;
    report.operations.reserve(plan.operationCount());
    report.untouched.reserve(plan.items.size());

    for (const CleanupPlanItem& item : plan.items) {
        if (item.candidateIndex >= candidates.size()) continue;  // защита от чужого/битого плана
        const CleanupCandidate& candidate = candidates[item.candidateIndex];
        const SelectionDecision decision = decideCandidate(candidate, plan.options);
        if (item.action != decision.action) {
            // План и кандидат разошлись — доверяем решению по кандидату и не показываем
            // пользователю операцию, которую движок выполнит иначе.
            continue;
        }
        PlanOperation op = makeOperation(item.candidateIndex, candidate, item.action, decision.skip, item.reclaimBytes,
                                        decision.reason);
        if (isRemovableAction(item.action)) {
            report.operations.push_back(std::move(op));
        } else {
            report.untouched.push_back(std::move(op));
        }
    }
    report.operationCount = report.operations.size();

    report.headline = "Освободится " + formatBytes(report.totalBytes) + " — " + itemCount(report.operationCount) +
                      " из " + itemCount(plan.totals.candidateCount);

    std::string text;
    text += "MrProper: план очистки (dry-run — ничего не удалено)\n";
    text += "Профиль: ";
    text += profileLabel(plan.options.profile);
    text += " · порог уверенности: " + std::to_string(plan.options.confidenceThreshold);
    text += " · Risky: ";
    text += plan.options.allowRisky ? "разрешён" : "скрыт";
    text += " · корзина: ";
    text += plan.options.useTrash ? "включена" : "выключена";
    text += "\n";
    text += report.headline;
    text += "\n";
    text += "Агрегаты: всё " + formatBytes(plan.totals.allBytes) + " · только Safe " +
            formatBytes(plan.totals.safeOnlyBytes) + " · выбрано " + formatBytes(plan.totals.selectedBytes) + "\n";
    if (!plan.categories.empty()) {
        text += "По категориям:\n";
        for (const CategoryAggregate& category : plan.categories) {
            text += "  " + category.category + ": выбрано " + formatBytes(category.selectedBytes) + " из " +
                    formatBytes(category.allBytes) + " (" + itemCount(category.candidateCount) + ")\n";
        }
    }
    text += "Операции (" + std::to_string(report.operations.size()) + "):\n";
    if (report.operations.empty()) {
        text += "  — нет: под выбранный профиль не подошёл ни один элемент\n";
    }
    for (std::size_t i = 0; i < report.operations.size(); ++i) {
        text += operationLine(i + 1, report.operations[i]);
        text += "\n    причина: " + report.operations[i].reason + "\n";
    }
    text += "Не трогаем (" + std::to_string(report.untouched.size()) + "):\n";
    if (report.untouched.empty()) {
        text += "  — нет\n";
    }
    for (std::size_t i = 0; i < report.untouched.size(); ++i) {
        text += operationLine(i + 1, report.untouched[i]);
        text += "\n    причина: " + report.untouched[i].reason + "\n";
    }
    report.text = std::move(text);
    return report;
}

std::string planToJson(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan) {
    using mrproper::json::Value;

    const PlanTotals& totals = plan.totals;
    const auto actionJson = [](const ActionTotals& totals) {
        return Value::object({{"count", Value(static_cast<double>(totals.count))},
                              {"bytes", Value(static_cast<double>(totals.bytes))}});
    };

    std::vector<Value> categoryItems;
    categoryItems.reserve(plan.categories.size());
    for (const CategoryAggregate& category : plan.categories) {
        categoryItems.push_back(Value::object({
            {"category", Value(category.category)},
            {"candidates", Value(static_cast<double>(category.candidateCount))},
            {"selected", Value(static_cast<double>(category.selectedCount))},
            {"selectedBytes", Value(static_cast<double>(category.selectedBytes))},
            {"allBytes", Value(static_cast<double>(category.allBytes))},
            {"safeOnlyBytes", Value(static_cast<double>(category.safeOnlyBytes))},
        }));
    }

    const DryRunReport report = makeDryRunReport(candidates, plan);
    const auto operationJson = [](const PlanOperation& op) {
        return Value::object({
            {"index", Value(static_cast<double>(op.candidateIndex))},
            {"action", Value(actionToken(op.action))},
            {"skipReason", Value(toString(op.skip))},
            {"category", Value(op.category)},
            {"name", Value(op.displayName)},
            {"path", Value(op.path)},
            {"bytes", Value(static_cast<double>(op.bytes))},
            {"safety", Value(safetyToken(op.safety))},
            {"confidence", Value(op.confidence)},
            {"reason", Value(op.reason)},
        });
    };

    std::vector<Value> operationItems;
    operationItems.reserve(report.operations.size());
    for (const PlanOperation& op : report.operations) operationItems.push_back(operationJson(op));

    std::vector<Value> untouchedItems;
    untouchedItems.reserve(report.untouched.size());
    for (const PlanOperation& op : report.untouched) untouchedItems.push_back(operationJson(op));

    return Value::object({
        {"schema", Value(1)},
        {"dryRun", Value(plan.dryRun)},
        {"profile", Value(toString(plan.options.profile))},
        {"options",
         Value::object({
             {"confidenceThreshold", Value(plan.options.confidenceThreshold)},
             {"allowRisky", Value(plan.options.allowRisky)},
             {"useTrash", Value(plan.options.useTrash)},
             {"trashDirectDeleteAboveBytes", Value(static_cast<double>(plan.options.trashDirectDeleteAboveBytes))},
             {"minReclaimBytes", Value(static_cast<double>(plan.options.minReclaimBytes))},
         })},
        {"totals",
         Value::object({
             {"candidates", Value(static_cast<double>(totals.candidateCount))},
             {"selected", Value(static_cast<double>(totals.selectedCount))},
             {"selectedBytes", Value(static_cast<double>(totals.selectedBytes))},
             {"allCount", Value(static_cast<double>(totals.allCount))},
             {"allBytes", Value(static_cast<double>(totals.allBytes))},
             {"safeOnlyCount", Value(static_cast<double>(totals.safeOnlyCount))},
             {"safeOnlyBytes", Value(static_cast<double>(totals.safeOnlyBytes))},
             {"hiddenRisky", Value(static_cast<double>(totals.hiddenRiskyCount))},
             {"belowThreshold", Value(static_cast<double>(totals.belowThresholdCount))},
             {"tooSmall", Value(static_cast<double>(totals.tooSmallCount))},
             {"profileFiltered", Value(static_cast<double>(totals.profileFilteredCount))},
             {"byAction",
              Value::object({
                  {"delete", actionJson(totals.byAction.deleteOps)},
                  {"trash", actionJson(totals.byAction.trashOps)},
                  {"keep", actionJson(totals.byAction.keepOps)},
                  {"skipLocked", actionJson(totals.byAction.skipLockedOps)},
              })},
         })},
        {"categories", Value::array(std::move(categoryItems))},
        {"operations", Value::array(std::move(operationItems))},
        {"untouched", Value::array(std::move(untouchedItems))},
    })
        .dump(2);
}

PlanSnapshot makeSnapshot(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan,
                          const PlanSnapshotContext& context) {
    const DryRunReport report = makeDryRunReport(candidates, plan);
    PlanSnapshot snapshot;
    snapshot.appVersion = context.appVersion;
    snapshot.pid = context.pid;
    snapshot.createdAtUnix = context.createdAtUnix;
    snapshot.operationCount = report.operations.size();
    snapshot.totalBytes = report.totalBytes;
    snapshot.planSignature = plan.planSignature();
    snapshot.operations = report.operations;
    return snapshot;
}

std::string snapshotToJson(const PlanSnapshot& snapshot) {
    using mrproper::json::Value;

    std::vector<Value> operationItems;
    operationItems.reserve(snapshot.operations.size());
    for (const PlanOperation& op : snapshot.operations) {
        operationItems.push_back(Value::object({
            {"index", Value(static_cast<double>(op.candidateIndex))},
            {"action", Value(actionToken(op.action))},
            {"category", Value(op.category)},
            {"name", Value(op.displayName)},
            {"path", Value(op.path)},
            {"bytes", Value(static_cast<double>(op.bytes))},
            {"safety", Value(safetyToken(op.safety))},
            {"confidence", Value(op.confidence)},
        }));
    }

    return Value::object({
        {"schema", Value(1)},
        {"appVersion", Value(snapshot.appVersion)},
        {"pid", Value(static_cast<double>(snapshot.pid))},
        {"createdAtUnix", Value(static_cast<double>(snapshot.createdAtUnix))},
        {"planSignature", Value(static_cast<double>(snapshot.planSignature))},
        {"operationCount", Value(static_cast<double>(snapshot.operationCount))},
        {"totalBytes", Value(static_cast<double>(snapshot.totalBytes))},
        {"operations", Value::array(std::move(operationItems))},
    })
        .dump(2);
}

void DryRunGate::beginSession() {
    signature_ = 0;
    acknowledged_ = false;
}

void DryRunGate::acknowledge(const CleanupPlan& plan) {
    signature_ = plan.planSignature();
    acknowledged_ = true;
}

bool DryRunGate::alreadyShown(const CleanupPlan& plan) const {
    return acknowledged_ && signature_ == plan.planSignature();
}

bool DryRunGate::mustShowBeforeExecute(const CleanupPlan& plan) const {
    if (plan.operationCount() == 0) return false;  // удалять нечего — показывать нечего
    return !alreadyShown(plan);
}

std::vector<std::string> validatePlan(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan) {
    std::vector<std::string> problems;
    if (plan.items.size() != candidates.size()) {
        problems.push_back("в плане " + std::to_string(plan.items.size()) + " элементов, а кандидатов " +
                           std::to_string(candidates.size()));
        return problems;
    }

    std::uint64_t selectedSum = 0;
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        const CleanupPlanItem& item = plan.items[i];
        const CleanupCandidate& candidate = candidates[i];
        if (item.candidateIndex != i) {
            problems.push_back("элемент " + std::to_string(i) + " ссылается на кандидата " +
                               std::to_string(item.candidateIndex));
        }
        if (item.action == PlanAction::SkipLocked && candidate.lockedBy.empty()) {
            problems.push_back("кандидат " + std::to_string(i) + " помечен SkipLocked, но lockedBy пуст");
        }
        if (isRemovableAction(item.action)) {
            if (item.reclaimBytes != candidate.allocatedBytes) {
                problems.push_back("кандидат " + std::to_string(i) + ": reclaimBytes " +
                                   std::to_string(item.reclaimBytes) + " != allocatedBytes " +
                                   std::to_string(candidate.allocatedBytes));
            }
            if (!candidate.lockedBy.empty()) {
                problems.push_back("кандидат " + std::to_string(i) + " выбран к удалению, но файлы заблокированы");
            }
            selectedSum += item.reclaimBytes;
        } else if (item.reclaimBytes != 0) {
            problems.push_back("кандидат " + std::to_string(i) + ": действие " + std::string(actionToken(item.action)) +
                               " не удаляет, но reclaimBytes " + std::to_string(item.reclaimBytes));
        }
        if (candidate.confidence < 0 || candidate.confidence > 100) {
            problems.push_back("кандидат " + std::to_string(i) + ": confidence " +
                               std::to_string(candidate.confidence) + " вне 0..100");
        }
    }

    if (selectedSum != plan.totals.selectedBytes) {
        problems.push_back("сумма reclaimBytes " + std::to_string(selectedSum) + " != totals.selectedBytes " +
                           std::to_string(plan.totals.selectedBytes));
    }
    std::size_t selectedCount = 0;
    for (const CleanupPlanItem& item : plan.items) {
        if (isRemovableAction(item.action)) ++selectedCount;
    }
    if (selectedCount != plan.totals.selectedCount) {
        problems.push_back("число выбранных " + std::to_string(selectedCount) + " != totals.selectedCount " +
                           std::to_string(plan.totals.selectedCount));
    }
    return problems;
}

}  // namespace mrproper::core
