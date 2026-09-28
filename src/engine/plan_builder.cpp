// Реализация PlanBuilder — состояние экрана «Очистка» поверх чистого
// core::plan (SPEC §4 FR-5, §4 FR-4, §6.2, §6.3, §12). Границы модуля и
// инварианты описаны в plan_builder.hpp — здесь только реализация.
//
// Ключевое отличие от core::plan, ради которого модуль существует: решение по
// кандидату остаётся в ядре (core::decideCandidate), а вот СОСТОЯНИЕ (ручные
// выборы, профиль, подтверждение dry-run) и СБОРКА результата из решений —
// здесь. Один проход rebuild() строит core::CleanupPlan и PlanView из одних и
// тех же решений, поэтому цифры агрегатов и состав операций разойтись не могут.

#include "plan_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/log.hpp"
#include "core/units.hpp"

namespace mrproper::engine {
namespace {

// Идентификаторы событий журнала. Стабильные строки, а не тексты сообщений:
// по ним ищут в log-файле (как в scan_coordinator.cpp).
constexpr std::string_view kEventRebuild = "plan.builder.rebuild";
constexpr std::string_view kEventSelection = "plan.builder.selection";
constexpr std::string_view kEventRisky = "plan.builder.risky";
constexpr std::string_view kEventDryRun = "plan.builder.dryrun";
constexpr std::string_view kEventSnapshot = "plan.builder.snapshot";
constexpr std::string_view kEventBlocked = "plan.builder.execute.blocked";

// core/model.hpp объявляет toString(PlanAction) без определения (см. комментарий
// в core/disk_model.hpp: модуля model.cpp в проекте нет) — ссылаться на него
// нельзя, линкер ответит LNK2019. Поэтому имена действий и уровней свои, как в
// core/plan.cpp.
const char* actionToken(core::PlanAction action) {
    switch (action) {
        case core::PlanAction::Delete: return "delete";
        case core::PlanAction::Trash: return "trash";
        case core::PlanAction::Keep: return "keep";
        case core::PlanAction::SkipLocked: return "skip-locked";
    }
    return "keep";
}

const char* actionLabel(core::PlanAction action) {
    switch (action) {
        case core::PlanAction::Delete: return "удалить";
        case core::PlanAction::Trash: return "в корзину";
        case core::PlanAction::Keep: return "оставить";
        case core::PlanAction::SkipLocked: return "пропуск (занято)";
    }
    return "оставить";
}

const char* safetyToken(core::SafetyLevel safety) {
    switch (safety) {
        case core::SafetyLevel::Safe: return "safe";
        case core::SafetyLevel::Review: return "review";
        case core::SafetyLevel::Risky: return "risky";
    }
    return "review";
}

const char* selectionToken(SelectionOverride selection) {
    switch (selection) {
        case SelectionOverride::Auto: return "auto";
        case SelectionOverride::KeptByUser: return "user-keep";
        case SelectionOverride::SelectedByUser: return "user-select";
    }
    return "auto";
}

// Русские склонения для заголовков и строк отчёта — те же правила, что в
// core/plan.cpp: их приходится дублировать, потому что там они внутренние.
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

std::string itemCount(std::size_t count) { return std::to_string(count) + " " + itemWord(count); }

// Короткая причина, почему профиль не взял элемент. Для строк представления
// нужна именно короткая форма: полная причина кандидата уже есть в
// core::decideCandidate и попадает в reason.
std::string skipPhrase(core::SkipReason skip) {
    switch (skip) {
        case core::SkipReason::None: return {};
        case core::SkipReason::Locked: return "файлы держит приложение";
        case core::SkipReason::RiskyHidden: return "уровень Risky скрыт по умолчанию";
        case core::SkipReason::BelowConfidence: return "уверенность ниже порога отбора";
        case core::SkipReason::ProfileFiltered: return "не проходит уровень безопасности профиля";
        case core::SkipReason::TooSmall: return "объём ниже минимального порога";
    }
    return {};
}

core::PlanOperation makeOperation(const PlanItemView& row) {
    core::PlanOperation op;
    op.candidateIndex = row.candidateIndex;
    op.action = row.action;
    op.skip = row.skip;
    op.category = row.category;
    op.displayName = row.displayName;
    op.path = row.path;
    op.bytes = row.selected ? row.bytes : 0;
    op.safety = row.safety;
    op.confidence = row.confidence;
    op.reason = row.reason;
    return op;
}

std::string operationLine(std::size_t number, const core::PlanOperation& op) {
    std::string line = "  " + std::to_string(number) + ". [" + actionLabel(op.action) + "] ";
    line += op.displayName.empty() ? op.path : op.displayName;
    line += " — " + core::formatBytes(op.bytes);
    line += " · " + std::string(safetyToken(op.safety));
    line += " · уверенность " + std::to_string(op.confidence);
    if (!op.path.empty() && op.path != op.displayName) line += " · " + op.path;
    return line;
}

core::ActionTotals& slotFor(core::PlanActionTotals& totals, core::PlanAction action) {
    switch (action) {
        case core::PlanAction::Delete: return totals.deleteOps;
        case core::PlanAction::Trash: return totals.trashOps;
        case core::PlanAction::Keep: return totals.keepOps;
        case core::PlanAction::SkipLocked: return totals.skipLockedOps;
    }
    return totals.keepOps;
}

std::int64_t nowUnixSeconds() {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count();
}

std::string joinProblems(const std::vector<std::string>& problems) {
    std::string out;
    for (std::size_t i = 0; i < problems.size(); ++i) {
        if (i != 0) out += "; ";
        out += problems[i];
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Выбор пользователя
// ---------------------------------------------------------------------------

const char* toString(SelectionOverride value) { return selectionToken(value); }

// ---------------------------------------------------------------------------
// Конструкторы и состав кандидатов
// ---------------------------------------------------------------------------

PlanBuilder::PlanBuilder(std::vector<core::CleanupCandidate> candidates, core::PlanOptions options)
    : options_(options) {
    setCandidates(std::move(candidates));
}

void PlanBuilder::setCandidates(std::vector<core::CleanupCandidate> candidates) {
    candidates_ = std::move(candidates);
    // Ручные выборы относятся к конкретным элементам прошлого скана: индексы
    // после нового скана указывают на другие пути, поэтому переносить их было бы
    // молчаливой подменой решения. Сбрасываем вместе с подтверждением dry-run.
    overrides_.assign(candidates_.size(), SelectionOverride::Auto);
    riskyRevealPending_ = false;
    gate_.beginSession();
    rebuild();
}

// ---------------------------------------------------------------------------
// Параметры отбора
// ---------------------------------------------------------------------------

void PlanBuilder::setOptions(core::PlanOptions options) {
    riskyRevealPending_ = false;
    options_ = options;
    rebuild();
}

void PlanBuilder::setProfile(core::SelectionProfile profile) {
    if (options_.profile == profile) return;
    options_.profile = profile;
    rebuild();
}

void PlanBuilder::setConfidenceThreshold(int threshold) {
    const int clamped = std::clamp(threshold, 0, 100);
    if (options_.confidenceThreshold == clamped) return;
    options_.confidenceThreshold = clamped;
    rebuild();
}

void PlanBuilder::setUseTrash(bool useTrash) {
    if (options_.useTrash == useTrash) return;
    options_.useTrash = useTrash;
    rebuild();
}

void PlanBuilder::setMinReclaimBytes(std::uint64_t bytes) {
    if (options_.minReclaimBytes == bytes) return;
    options_.minReclaimBytes = bytes;
    rebuild();
}

// ---------------------------------------------------------------------------
// Risky: показать всё с подтверждением (FR-4, §12)
// ---------------------------------------------------------------------------

void PlanBuilder::askRevealRisky() {
    if (riskyRevealPending_ || options_.allowRisky) return;
    riskyRevealPending_ = true;
    core::logInfo(kEventRisky, "запрошен показ элементов уровня Risky — нужно подтверждение пользователя");
}

bool PlanBuilder::confirmRiskyReveal(bool accepted) {
    riskyRevealPending_ = false;
    if (!accepted) {
        core::logInfo(kEventRisky, "показ Risky отклонён: элементы уровня Risky остаются скрытыми");
        return false;
    }
    if (options_.allowRisky) return true;  // уже показаны: повторное согласие ничего не меняет
    options_.allowRisky = true;
    rebuild();
    return true;
}

void PlanBuilder::hideRisky() {
    riskyRevealPending_ = false;
    if (!options_.allowRisky) return;
    options_.allowRisky = false;
    // Ручные выборы Risky-элементов НЕ сбрасываются: показ скрыт, решение о
    // них снова спросится у пользователя, а не у профиля. Если Risky покажут
    // ещё раз, его выбор вернётся вместе с ними.
    rebuild();
}

// ---------------------------------------------------------------------------
// Выборы пользователя
// ---------------------------------------------------------------------------

SelectionOverride PlanBuilder::overrideFor(std::size_t candidateIndex) const noexcept {
    if (candidateIndex >= overrides_.size()) return SelectionOverride::Auto;
    return overrides_[candidateIndex];
}

bool PlanBuilder::setSelected(std::size_t candidateIndex, bool selected) {
    if (candidateIndex >= candidates_.size()) return false;
    const core::CleanupCandidate& candidate = candidates_[candidateIndex];

    if (!candidate.lockedBy.empty()) return false;  // FR-5: Skip (locked) выбором не отменяется
    if (selected && candidate.safety == core::SafetyLevel::Risky && !options_.allowRisky) return false;

    const SelectionOverride wanted = selected ? SelectionOverride::SelectedByUser : SelectionOverride::KeptByUser;
    if (overrides_[candidateIndex] == wanted) return true;
    overrides_[candidateIndex] = wanted;
    rebuild();

    core::LogFields fields;
    fields.push_back(core::logField("index", static_cast<std::uint64_t>(candidateIndex)));
    fields.push_back(core::logField("path", candidate.path));
    fields.push_back(core::logField("selection", selectionToken(wanted)));
    fields.push_back(core::logField("bytes", candidate.allocatedBytes));
    fields.push_back(core::logField("selectedBytes", view_.totals.selectedBytes));
    core::logInfo(kEventSelection, "выбор элемента изменён", fields);
    return true;
}

bool PlanBuilder::clearOverride(std::size_t candidateIndex) {
    if (candidateIndex >= overrides_.size()) return false;
    if (overrides_[candidateIndex] == SelectionOverride::Auto) return true;
    overrides_[candidateIndex] = SelectionOverride::Auto;
    rebuild();
    return true;
}

void PlanBuilder::selectAllVisible() {
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const core::CleanupCandidate& candidate = candidates_[index];
        if (!candidate.lockedBy.empty()) continue;
        if (candidate.safety == core::SafetyLevel::Risky && !options_.allowRisky) continue;
        overrides_[index] = SelectionOverride::SelectedByUser;
    }
    rebuild();
}

void PlanBuilder::selectSafeOnly() {
    // Третье состояние кнопки экрана рядом с «выбрать всё» и «снять всё»:
    // остаётся ровно то, что безопасно. Скрытый Risky и занятые элементы не
    // трогаются — их нельзя ни взять, ни снять (FR-4, FR-5).
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const core::CleanupCandidate& candidate = candidates_[index];
        if (!candidate.lockedBy.empty()) continue;
        if (candidate.safety == core::SafetyLevel::Safe) {
            overrides_[index] = SelectionOverride::SelectedByUser;
        } else if (candidate.safety == core::SafetyLevel::Risky && !options_.allowRisky) {
            continue;  // скрыт: снимать нечего, человек его не видел
        } else {
            overrides_[index] = SelectionOverride::KeptByUser;
        }
    }
    rebuild();
}

void PlanBuilder::selectNone() {
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const core::CleanupCandidate& candidate = candidates_[index];
        if (!candidate.lockedBy.empty()) continue;
        if (candidate.safety == core::SafetyLevel::Risky && !options_.allowRisky) continue;
        overrides_[index] = SelectionOverride::KeptByUser;
    }
    rebuild();
}

bool PlanBuilder::setCategorySelected(std::string_view category, bool selected) {
    bool found = false;
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const core::CleanupCandidate& candidate = candidates_[index];
        if (candidate.category != category) continue;
        found = true;
        if (!candidate.lockedBy.empty()) continue;
        if (candidate.safety == core::SafetyLevel::Risky && !options_.allowRisky) continue;
        overrides_[index] = selected ? SelectionOverride::SelectedByUser : SelectionOverride::KeptByUser;
    }
    if (found) rebuild();
    return found;
}

void PlanBuilder::clearOverrides() {
    if (overrides_.empty()) return;
    bool hadOverrides = false;
    for (const SelectionOverride value : overrides_) {
        if (value != SelectionOverride::Auto) {
            hadOverrides = true;
            break;
        }
    }
    if (!hadOverrides) return;
    overrides_.assign(overrides_.size(), SelectionOverride::Auto);
    rebuild();
}

// ---------------------------------------------------------------------------
// Решение по кандидату с учётом выбора пользователя
// ---------------------------------------------------------------------------

core::SelectionDecision PlanBuilder::decideWithOverride(const core::CleanupCandidate& candidate,
                                                       SelectionOverride selection) const {
    const core::SelectionDecision profileDecision = core::decideCandidate(candidate, options_);

    if (selection == SelectionOverride::Auto) return profileDecision;

    if (selection == SelectionOverride::KeptByUser) {
        // Занятые файлы остаются SkipLocked: человек может захотеть оставить
        // элемент, который и так не удаляется, но обещать это нельзя — снятая
        // галочка не отменяет блокировку (FR-5, FR-6).
        if (profileDecision.action == core::PlanAction::SkipLocked) return profileDecision;
        core::SelectionDecision decision = profileDecision;
        decision.action = core::PlanAction::Keep;
        decision.selected = false;
        decision.skip = core::SkipReason::None;
        decision.reason = "снято пользователем — элемент остаётся на месте, действие не выполняется";
        return decision;
    }

    // SelectedByUser: профиль элемент не взял, человек взял.
    if (profileDecision.selected) return profileDecision;  // уже выбран — менять нечего
    if (profileDecision.action == core::PlanAction::SkipLocked) {
        core::SelectionDecision decision = profileDecision;
        decision.reason += "; ручной выбор проигнорирован: файлы держит приложение";
        return decision;
    }
    if (profileDecision.skip == core::SkipReason::RiskyHidden) {
        core::SelectionDecision decision = profileDecision;
        decision.reason += "; сначала подтвердите «показать всё» — Risky выбирается только с ним (SPEC §12)";
        return decision;
    }

    // Действие (корзина или прямое удаление) по-прежнему решает ядро: снимаем
    // только те ограничения, из-за которых элемент не попал в профиль
    // (профиль, минимальный объём), и заново спрашиваем core::decideCandidate.
    core::PlanOptions permissive = options_;
    permissive.profile = core::SelectionProfile::Everything;
    permissive.minReclaimBytes = 0;
    const core::SelectionDecision forced = core::decideCandidate(candidate, permissive);
    if (forced.action == core::PlanAction::SkipLocked) return profileDecision;  // страховка

    core::SelectionDecision decision = forced;
    decision.reason = "выбрано пользователем вручную: " + std::string(actionLabel(decision.action));
    if (!options_.useTrash) {
        decision.reason += "; корзина приложения выключена — прямое удаление с записью в журнал";
    } else if (decision.action == core::PlanAction::Delete) {
        decision.reason += "; объём больше " + core::formatBytes(options_.trashDirectDeleteAboveBytes) +
                           " — прямое удаление с записью в журнал (SPEC §4 FR-7)";
    } else {
        decision.reason += "; операцию можно отменить через корзину приложения";
    }
    const std::string profileReason = skipPhrase(profileDecision.skip);
    if (!profileReason.empty()) decision.reason += "; профиль не взял: " + profileReason;
    return decision;
}

// ---------------------------------------------------------------------------
// Пересборка плана и представления
// ---------------------------------------------------------------------------

void PlanBuilder::rebuild() {
    ++revision_;

    plan_ = core::CleanupPlan{};
    plan_.options = options_;
    plan_.dryRun = options_.dryRun;  // FR-5: dry-run обязателен и включён по умолчанию
    plan_.items.reserve(candidates_.size());
    plan_.totals.candidateCount = candidates_.size();

    view_ = PlanView{};
    view_.candidateCount = candidates_.size();
    view_.revision = revision_;
    view_.dryRun = plan_.dryRun;
    view_.items.reserve(candidates_.size());

    // Категории набираются в порядке первого появления, сортируются один раз в
    // конце — так порядок не зависит от того, в каком порядке считали агрегаты.
    std::vector<core::CategoryAggregate> coreCategories;
    std::vector<PlanCategoryView> viewCategories;
    std::unordered_map<std::string, std::size_t> categorySlotOf;
    coreCategories.reserve(32);
    viewCategories.reserve(32);
    categorySlotOf.reserve(32);

    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const core::CleanupCandidate& candidate = candidates_[index];
        const SelectionOverride selection = overrideFor(index);
        const core::SelectionDecision decision = decideWithOverride(candidate, selection);
        const bool removable = core::isRemovableAction(decision.action);
        const bool locked = !candidate.lockedBy.empty();
        const bool visible = candidate.safety != core::SafetyLevel::Risky || options_.allowRisky;

        core::CleanupPlanItem item;
        item.candidateIndex = index;
        item.action = decision.action;
        item.reclaimBytes = removable ? candidate.allocatedBytes : 0;
        plan_.items.push_back(item);

        core::ActionTotals& actionTotals = slotFor(plan_.totals.byAction, decision.action);
        ++actionTotals.count;
        actionTotals.bytes += item.reclaimBytes;
        // Те же счётчики в представлении: JSON плана отдаёт view_.totals.byAction,
        // а исполнитель сверяется с plan_.totals.byAction. Молчащие нули в одном
        // из них — это расхождение «цифры на экране» и «состава операций».
        core::ActionTotals& viewActionTotals = slotFor(view_.totals.byAction, decision.action);
        ++viewActionTotals.count;
        viewActionTotals.bytes += item.reclaimBytes;

        if (removable) {
            ++plan_.totals.selectedCount;
            plan_.totals.selectedBytes += item.reclaimBytes;
            ++view_.totals.selectedCount;
            view_.totals.selectedBytes += item.reclaimBytes;
        } else {
            switch (decision.skip) {
                case core::SkipReason::RiskyHidden:
                    ++plan_.totals.hiddenRiskyCount;
                    ++view_.totals.hiddenRiskyCount;
                    view_.totals.hiddenRiskyBytes += candidate.allocatedBytes;
                    break;
                case core::SkipReason::BelowConfidence:
                    ++plan_.totals.belowThresholdCount;
                    ++view_.totals.belowThresholdCount;
                    break;
                case core::SkipReason::TooSmall:
                    ++plan_.totals.tooSmallCount;
                    ++view_.totals.tooSmallCount;
                    break;
                case core::SkipReason::ProfileFiltered:
                    ++plan_.totals.profileFilteredCount;
                    ++view_.totals.profileFilteredCount;
                    break;
                case core::SkipReason::Locked:
                    ++view_.totals.lockedCount;
                    view_.totals.lockedBytes += candidate.allocatedBytes;
                    break;
                case core::SkipReason::None:
                    if (selection == SelectionOverride::KeptByUser) {
                        ++view_.totals.keptByUserCount;
                        view_.totals.keptByUserBytes += candidate.allocatedBytes;
                    }
                    break;
            }
        }

        // «Выбрать всё» глазами пользователя и потолок «можно удалить в принципе»
        // (FR-5). Скрытый Risky в потолок входит, в «выбрать всё» — нет.
        if (!locked) {
            ++plan_.totals.allCount;
            plan_.totals.allBytes += candidate.allocatedBytes;
            ++view_.totals.allCount;
            view_.totals.allBytes += candidate.allocatedBytes;
        }
        if (candidate.safety == core::SafetyLevel::Safe && !locked) {
            ++plan_.totals.safeOnlyCount;
            plan_.totals.safeOnlyBytes += candidate.allocatedBytes;
            ++view_.totals.safeOnlyCount;
            view_.totals.safeOnlyBytes += candidate.allocatedBytes;
        }
        if (visible && !locked) {
            ++view_.totals.visibleSelectableCount;
            view_.totals.visibleSelectableBytes += candidate.allocatedBytes;
        }

        PlanItemView row;
        row.candidateIndex = index;
        row.category = candidate.category;
        row.displayName = candidate.displayName;
        row.path = candidate.path;
        row.bytes = candidate.allocatedBytes;
        row.safety = candidate.safety;
        row.confidence = candidate.confidence;
        row.action = decision.action;
        row.skip = decision.skip;
        row.selection = selection;
        row.selected = removable;
        row.visible = visible;
        row.locked = locked;
        row.reason = decision.reason;
        view_.items.push_back(std::move(row));

        // Слот категории: одна и та же строка дерева для core::CategoryAggregate
        // (его читают core-функции) и PlanCategoryView (его рисует UI).
        std::size_t slot = 0;
        const auto found = categorySlotOf.find(candidate.category);
        if (found == categorySlotOf.end()) {
            slot = coreCategories.size();
            categorySlotOf.emplace(candidate.category, slot);
            coreCategories.push_back(core::CategoryAggregate{candidate.category});
            viewCategories.push_back(PlanCategoryView{candidate.category});
        } else {
            slot = found->second;
        }
        core::CategoryAggregate& coreCategory = coreCategories[slot];
        PlanCategoryView& viewCategory = viewCategories[slot];
        ++coreCategory.candidateCount;
        ++viewCategory.itemCount;
        if (!locked) coreCategory.allBytes += candidate.allocatedBytes;
        if (candidate.safety == core::SafetyLevel::Safe && !locked) {
            coreCategory.safeOnlyBytes += candidate.allocatedBytes;
            viewCategory.safeOnlyBytes += candidate.allocatedBytes;
        }
        if (removable) {
            ++coreCategory.selectedCount;
            coreCategory.selectedBytes += candidate.allocatedBytes;
            ++viewCategory.selectedCount;
            viewCategory.selectedBytes += candidate.allocatedBytes;
        }
        if (visible && !locked) {
            ++viewCategory.selectableCount;
            viewCategory.selectableBytes += candidate.allocatedBytes;
        }
        if (locked) ++viewCategory.lockedCount;
        if (decision.skip == core::SkipReason::RiskyHidden) ++viewCategory.hiddenRiskyCount;
    }

    // Крупные категории первыми — так список читается как «с чего начать».
    std::vector<std::size_t> order(coreCategories.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&coreCategories](std::size_t a, std::size_t b) {
        if (coreCategories[a].selectedBytes != coreCategories[b].selectedBytes) {
            return coreCategories[a].selectedBytes > coreCategories[b].selectedBytes;
        }
        return coreCategories[a].category < coreCategories[b].category;
    });
    plan_.categories.reserve(order.size());
    view_.categories.reserve(order.size());
    for (const std::size_t slot : order) {
        plan_.categories.push_back(coreCategories[slot]);
        PlanCategoryView category = viewCategories[slot];
        // Третье состояние чекбокса категории: выбрано всё / часть / ничего.
        category.allSelected =
            category.selectableCount != 0 && category.selectedCount == category.selectableCount;
        category.partiallySelected = category.selectedCount != 0 && !category.allSelected;
        view_.categories.push_back(std::move(category));
    }

    if (view_.totals.selectedCount == 0) {
        view_.headline = "Ничего не выбрано — " + itemCount(view_.candidateCount) + " без действия";
    } else {
        view_.headline = "Освободится " + core::formatBytes(view_.totals.selectedBytes) + " — " +
                         itemCount(view_.totals.selectedCount) + " из " + itemCount(view_.candidateCount);
    }

    // Публикация плана: исполнитель держит константную ссылку, пока UI продолжает
    // щёлкать чекбоксы (§6.4 — результаты не мутируются после публикации).
    published_ = std::make_shared<const core::CleanupPlan>(plan_);

    core::LogFields fields;
    fields.push_back(core::logField("revision", static_cast<std::uint64_t>(revision_)));
    fields.push_back(core::logField("candidates", static_cast<std::uint64_t>(candidates_.size())));
    fields.push_back(core::logField("selected", static_cast<std::uint64_t>(view_.totals.selectedCount)));
    fields.push_back(core::logField("bytes", view_.totals.selectedBytes));
    fields.push_back(core::logField("operations", static_cast<std::uint64_t>(plan_.operationCount())));
    fields.push_back(core::logField("categories", static_cast<std::uint64_t>(view_.categories.size())));
    fields.push_back(core::logField("profile", core::toString(options_.profile)));
    core::logDebug(kEventRebuild, view_.headline, fields);
}

// ---------------------------------------------------------------------------
// Представление
// ---------------------------------------------------------------------------

const PlanItemView* PlanBuilder::item(std::size_t candidateIndex) const noexcept {
    if (candidateIndex >= view_.items.size()) return nullptr;
    return &view_.items[candidateIndex];
}

const PlanCategoryView* PlanBuilder::category(std::string_view name) const noexcept {
    for (const PlanCategoryView& entry : view_.categories) {
        if (entry.category == name) return &entry;
    }
    return nullptr;
}

CleanupSummary PlanBuilder::summary() const {
    CleanupSummary summary;
    summary.selectedCount = view_.totals.selectedCount;
    summary.selectedBytes = view_.totals.selectedBytes;
    summary.selectAllCount = view_.totals.visibleSelectableCount;
    summary.selectAllBytes = view_.totals.visibleSelectableBytes;
    summary.safeOnlyCount = view_.totals.safeOnlyCount;
    summary.safeOnlyBytes = view_.totals.safeOnlyBytes;
    summary.text = "Выбрано " + core::formatBytes(summary.selectedBytes) + " (" + itemCount(summary.selectedCount) +
                   ") · выбрать всё " + core::formatBytes(summary.selectAllBytes) + " (" +
                   itemCount(summary.selectAllCount) + ") · только Safe " + core::formatBytes(summary.safeOnlyBytes) +
                   " (" + itemCount(summary.safeOnlyCount) + ")";
    return summary;
}

std::vector<core::PlanOperation> PlanBuilder::operations() const {
    std::vector<core::PlanOperation> result;
    result.reserve(plan_.operationCount());
    for (const PlanItemView& row : view_.items) {
        if (!row.selected) continue;
        result.push_back(makeOperation(row));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Dry-run, текст, JSON (FR-5)
// ---------------------------------------------------------------------------

core::DryRunReport PlanBuilder::dryRun() const {
    core::DryRunReport report;
    report.dryRun = plan_.dryRun;
    report.totalBytes = view_.totals.selectedBytes;
    report.operations.reserve(plan_.operationCount());
    report.untouched.reserve(view_.items.size());
    for (const PlanItemView& row : view_.items) {
        core::PlanOperation op = makeOperation(row);
        if (row.selected) {
            report.operations.push_back(std::move(op));
        } else {
            report.untouched.push_back(std::move(op));
        }
    }
    report.operationCount = report.operations.size();
    report.headline = view_.headline;
    report.text = toText();
    return report;
}

std::string PlanBuilder::toText() const {
    const CleanupSummary numbers = summary();
    std::string text;
    text += "MrProper: план очистки (dry-run — список операций, ничего не удалено)\n";
    text += "Профиль: ";
    text += core::toString(options_.profile);
    text += " · порог уверенности: " + std::to_string(options_.confidenceThreshold);
    text += " · Risky: ";
    text += options_.allowRisky ? "показан (подтверждён)" : "скрыт";
    text += " · корзина: ";
    text += options_.useTrash ? "включена" : "выключена";
    text += "\n";
    text += view_.headline;
    text += "\n";
    text += numbers.text;
    text += "\n";
    text += "Осталось на месте: занято " + itemCount(view_.totals.lockedCount) + " · скрыто Risky " +
            itemCount(view_.totals.hiddenRiskyCount) + " · снято пользователем " +
            itemCount(view_.totals.keptByUserCount) + " · ниже порога " +
            itemCount(view_.totals.belowThresholdCount) + " · профиль " + itemCount(view_.totals.profileFilteredCount) +
            " · мелочь " + itemCount(view_.totals.tooSmallCount) + "\n";
    if (!view_.categories.empty()) {
        text += "По категориям:\n";
        for (const PlanCategoryView& entry : view_.categories) {
            text += "  " + entry.category + ": выбрано " + core::formatBytes(entry.selectedBytes) + " из " +
                    core::formatBytes(entry.selectableBytes) + " · только Safe " +
                    core::formatBytes(entry.safeOnlyBytes) + " (" + itemCount(entry.itemCount) + ")\n";
        }
    }

    const std::vector<core::PlanOperation> operationsList = operations();
    text += "Операции (" + std::to_string(operationsList.size()) + "):\n";
    if (operationsList.empty()) {
        text += "  — нет: под выбранный профиль не подошёл ни один элемент\n";
    }
    for (std::size_t i = 0; i < operationsList.size(); ++i) {
        text += operationLine(i + 1, operationsList[i]);
        text += "\n    причина: " + operationsList[i].reason + "\n";
    }

    text += "Не трогаем (" + std::to_string(view_.items.size() - operationsList.size()) + "):\n";
    std::size_t line = 0;
    for (const PlanItemView& row : view_.items) {
        if (row.selected) continue;
        ++line;
        const core::PlanOperation op = makeOperation(row);
        text += operationLine(line, op);
        text += "\n    причина: " + op.reason + "\n";
    }
    if (line == 0) text += "  — нет\n";
    return text;
}

std::string PlanBuilder::toJson() const {
    using mrproper::json::Value;

    const auto actionJson = [](const core::ActionTotals& totals) {
        return Value::object({{"count", Value(static_cast<double>(totals.count))},
                              {"bytes", Value(static_cast<double>(totals.bytes))}});
    };
    const auto number = [](std::uint64_t value) { return Value(static_cast<double>(value)); };

    std::vector<Value> categoryItems;
    categoryItems.reserve(view_.categories.size());
    for (const PlanCategoryView& entry : view_.categories) {
        categoryItems.push_back(Value::object({
            {"category", Value(entry.category)},
            {"candidates", Value(static_cast<double>(entry.itemCount))},
            {"selected", Value(static_cast<double>(entry.selectedCount))},
            {"selectedBytes", number(entry.selectedBytes)},
            {"selectableBytes", number(entry.selectableBytes)},
            {"safeOnlyBytes", number(entry.safeOnlyBytes)},
            {"hiddenRisky", Value(static_cast<double>(entry.hiddenRiskyCount))},
            {"locked", Value(static_cast<double>(entry.lockedCount))},
        }));
    }

    std::vector<Value> itemItems;
    itemItems.reserve(view_.items.size());
    for (const PlanItemView& row : view_.items) {
        itemItems.push_back(Value::object({
            {"index", Value(static_cast<double>(row.candidateIndex))},
            {"category", Value(row.category)},
            {"name", Value(row.displayName)},
            {"path", Value(row.path)},
            {"bytes", number(row.bytes)},
            {"safety", Value(safetyToken(row.safety))},
            {"confidence", Value(row.confidence)},
            {"action", Value(actionToken(row.action))},
            {"skipReason", Value(core::toString(row.skip))},
            {"selection", Value(selectionToken(row.selection))},
            {"selected", Value(row.selected)},
            {"visible", Value(row.visible)},
            {"locked", Value(row.locked)},
            {"reason", Value(row.reason)},
        }));
    }

    std::vector<Value> operationItems;
    const std::vector<core::PlanOperation> operationsList = operations();
    operationItems.reserve(operationsList.size());
    for (const core::PlanOperation& op : operationsList) {
        operationItems.push_back(Value::object({
            {"index", Value(static_cast<double>(op.candidateIndex))},
            {"action", Value(actionToken(op.action))},
            {"category", Value(op.category)},
            {"name", Value(op.displayName)},
            {"path", Value(op.path)},
            {"bytes", number(op.bytes)},
            {"safety", Value(safetyToken(op.safety))},
            {"confidence", Value(op.confidence)},
            {"reason", Value(op.reason)},
        }));
    }

    const core::PlanActionTotals& byAction = view_.totals.byAction;
    return Value::object({
        {"schema", Value(1)},
        {"dryRun", Value(plan_.dryRun)},
        {"revision", Value(static_cast<double>(view_.revision))},
        {"planSignature", number(plan_.planSignature())},
        {"profile", Value(core::toString(options_.profile))},
        {"options",
         Value::object({
             {"confidenceThreshold", Value(options_.confidenceThreshold)},
             {"allowRisky", Value(options_.allowRisky)},
             {"useTrash", Value(options_.useTrash)},
             {"trashDirectDeleteAboveBytes", number(options_.trashDirectDeleteAboveBytes)},
             {"minReclaimBytes", number(options_.minReclaimBytes)},
         })},
        {"headline", Value(view_.headline)},
        {"totals",
         Value::object({
             {"candidates", Value(static_cast<double>(view_.totals.candidateCount))},
             {"selected", Value(static_cast<double>(view_.totals.selectedCount))},
             {"selectedBytes", number(view_.totals.selectedBytes)},
             {"visibleSelectable", Value(static_cast<double>(view_.totals.visibleSelectableCount))},
             {"visibleSelectableBytes", number(view_.totals.visibleSelectableBytes)},
             {"allCount", Value(static_cast<double>(view_.totals.allCount))},
             {"allBytes", number(view_.totals.allBytes)},
             {"safeOnlyCount", Value(static_cast<double>(view_.totals.safeOnlyCount))},
             {"safeOnlyBytes", number(view_.totals.safeOnlyBytes)},
             {"locked", Value(static_cast<double>(view_.totals.lockedCount))},
             {"hiddenRisky", Value(static_cast<double>(view_.totals.hiddenRiskyCount))},
             {"belowThreshold", Value(static_cast<double>(view_.totals.belowThresholdCount))},
             {"tooSmall", Value(static_cast<double>(view_.totals.tooSmallCount))},
             {"profileFiltered", Value(static_cast<double>(view_.totals.profileFilteredCount))},
             {"keptByUser", Value(static_cast<double>(view_.totals.keptByUserCount))},
             {"byAction",
              Value::object({
                  {"delete", actionJson(byAction.deleteOps)},
                  {"trash", actionJson(byAction.trashOps)},
                  {"keep", actionJson(byAction.keepOps)},
                  {"skipLocked", actionJson(byAction.skipLockedOps)},
              })},
         })},
        {"categories", Value::array(std::move(categoryItems))},
        {"items", Value::array(std::move(itemItems))},
        {"operations", Value::array(std::move(operationItems))},
    })
        .dump(2);
}

// ---------------------------------------------------------------------------
// Гейт dry-run и снимок перед исполнением (FR-5)
// ---------------------------------------------------------------------------

void PlanBuilder::beginSession() {
    gate_.beginSession();
    core::logInfo(kEventDryRun, "начата сессия плана: dry-run показывается заново (SPEC §4 FR-5)");
}

bool PlanBuilder::dryRunConfirmed() const noexcept { return gate_.alreadyShown(plan_); }

bool PlanBuilder::mustConfirmDryRun() const { return gate_.mustShowBeforeExecute(plan_); }

void PlanBuilder::confirmDryRun() {
    if (plan_.operationCount() == 0) {
        core::logWarn(kEventDryRun, "подтверждать нечего: в плане нет операций");
        return;
    }
    const std::vector<std::string> problems = validate();
    if (!problems.empty()) {
        core::LogFields fields;
        fields.push_back(core::logField("problems", joinProblems(problems)));
        core::logWarn(kEventDryRun, "план не проходит проверку — dry-run не подтверждён", fields);
        return;
    }
    gate_.acknowledge(plan_);
    core::LogFields fields;
    fields.push_back(core::logField("operations", static_cast<std::uint64_t>(plan_.operationCount())));
    fields.push_back(core::logField("bytes", view_.totals.selectedBytes));
    fields.push_back(core::logField("signature", plan_.planSignature()));
    core::logInfo(kEventDryRun, "план очистки подтверждён пользователем", fields);
}

PlanPreparation PlanBuilder::prepareForExecution(const core::PlanSnapshotContext& context) {
    PlanPreparation preparation;
    preparation.operationCount = plan_.operationCount();
    preparation.totalBytes = view_.totals.selectedBytes;
    preparation.planSignature = plan_.planSignature();
    preparation.problems = validate();

    if (preparation.operationCount == 0) preparation.problems.push_back("в плане нет операций — удалять нечего");
    if (gate_.mustShowBeforeExecute(plan_)) {
        preparation.problems.push_back("dry-run этого плана не показан и не подтверждён (SPEC §4 FR-5)");
    }

    if (!preparation.problems.empty()) {
        core::LogFields fields;
        fields.push_back(core::logField("operations", static_cast<std::uint64_t>(preparation.operationCount)));
        fields.push_back(core::logField("bytes", preparation.totalBytes));
        fields.push_back(core::logField("signature", preparation.planSignature));
        fields.push_back(core::logField("problems", joinProblems(preparation.problems)));
        core::logWarn(kEventBlocked, "исполнение не начато: план не готов", fields);
        return preparation;
    }

    // Снимок состояния (FR-5): список операций, PID, версия, размер. Время
    // приходит от платформы; нулевое означает «не передано» — тогда ставим часы
    // модуля, чтобы снимок не оказался с epoch-датой.
    core::PlanSnapshotContext effective = context;
    if (effective.createdAtUnix == 0) effective.createdAtUnix = nowUnixSeconds();

    preparation.snapshot.appVersion = effective.appVersion;
    preparation.snapshot.pid = effective.pid;
    preparation.snapshot.createdAtUnix = effective.createdAtUnix;
    preparation.snapshot.operationCount = preparation.operationCount;
    preparation.snapshot.totalBytes = preparation.totalBytes;
    preparation.snapshot.planSignature = preparation.planSignature;
    preparation.snapshot.operations = operations();
    preparation.dryRunText = toText();
    preparation.ready = true;

    core::LogFields fields;
    fields.push_back(core::logField("pid", static_cast<std::uint64_t>(effective.pid)));
    fields.push_back(core::logField("appVersion", effective.appVersion));
    fields.push_back(core::logField("operations", static_cast<std::uint64_t>(preparation.snapshot.operationCount)));
    fields.push_back(core::logField("bytes", preparation.snapshot.totalBytes));
    fields.push_back(core::logField("signature", preparation.snapshot.planSignature));
    fields.push_back(core::logField("createdAtUnix", static_cast<std::int64_t>(preparation.snapshot.createdAtUnix)));
    core::logInfo(kEventSnapshot, preparation.dryRunText, fields);
    return preparation;
}

// ---------------------------------------------------------------------------
// Проверка инвариантов
// ---------------------------------------------------------------------------

std::vector<std::string> PlanBuilder::validate() const {
    std::vector<std::string> problems = core::validatePlan(candidates_, plan_);

    if (overrides_.size() != candidates_.size()) {
        problems.push_back("выборов " + std::to_string(overrides_.size()) + " при кандидатах " +
                           std::to_string(candidates_.size()));
        return problems;
    }

    std::uint64_t selectedSum = 0;
    std::size_t selectedCount = 0;
    for (const PlanItemView& row : view_.items) {
        if (row.selected) {
            ++selectedCount;
            selectedSum += row.bytes;
            if (row.locked) {
                problems.push_back("элемент " + std::to_string(row.candidateIndex) +
                                   " выбран к удалению, но файлы заблокированы");
            }
        } else if (row.selection == SelectionOverride::SelectedByUser && !row.locked &&
                   row.skip != core::SkipReason::RiskyHidden) {
            // Единственный допустимый повод не выполнить ручной выбор — занятые
            // файлы и скрытый Risky; они проверяются выше. Всё остальное —
            // расхождение представления с планом.
            problems.push_back("элемент " + std::to_string(row.candidateIndex) +
                               " выбран пользователем, но план его не взял");
        }
    }

    if (view_.items.size() != plan_.items.size()) {
        problems.push_back("в представлении " + std::to_string(view_.items.size()) + " строк, в плане " +
                           std::to_string(plan_.items.size()));
    }
    if (selectedCount != view_.totals.selectedCount || selectedSum != view_.totals.selectedBytes) {
        problems.push_back("агрегаты представления не сходятся со строками: выбрано " +
                           std::to_string(selectedCount) + " / " + core::formatBytes(selectedSum) +
                           ", заявлено " + std::to_string(view_.totals.selectedCount) + " / " +
                           core::formatBytes(view_.totals.selectedBytes));
    }

    // Агрегаты по действиям: план отдаёт исполнитель, представление — UI и JSON.
    const core::PlanAction allActions[] = {core::PlanAction::Delete, core::PlanAction::Trash,
                                            core::PlanAction::Keep, core::PlanAction::SkipLocked};
    for (const core::PlanAction action : allActions) {
        const core::ActionTotals& fromPlan = plan_.totals.byAction.forAction(action);
        const core::ActionTotals& fromView = view_.totals.byAction.forAction(action);
        if (fromPlan.count != fromView.count || fromPlan.bytes != fromView.bytes) {
            problems.push_back("агрегаты действия " + std::string(actionToken(action)) + " в представлении (" +
                               std::to_string(fromView.count) + " / " + std::to_string(fromView.bytes) +
                               " байт) не сходятся с планом (" + std::to_string(fromPlan.count) + " / " +
                               std::to_string(fromPlan.bytes) + " байт)");
        }
    }
    return problems;
}

}  // namespace mrproper::engine
