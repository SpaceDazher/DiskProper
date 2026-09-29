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

// Уровень риска по-русски. safetyToken() даёт машинное слово для JSON, а
// подтверждение перед удалением (FR-5) читает человек: «safe» там не ответ на
// вопрос «это безопасно удалить?».
const char* riskLabel(SafetyLevel safety) {
    switch (safety) {
        case SafetyLevel::Safe: return "безопасно";
        case SafetyLevel::Review: return "проверить";
        case SafetyLevel::Risky: return "риск";
    }
    return "проверить";
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

// Один отказ «уровень элемента не входит в то, что берётся по умолчанию»
// в двух формулировках: уровень не входит и кто это задал — профиль или потолок
// уровня. Формулировка одна, потому что решение одно: safe-only и recommended с
// потолком safe отсекают Review одинаково, и текст причины не имеет права
// выдавать это за два разных решения (SPEC §12 — план объясняет свой выбор).
std::string safetyCeilingReason(SafetyLevel safety, std::string_view ceiling) {
    return "уровень " + std::string(safetyToken(safety)) + " не входит в потолок отбора по умолчанию (" +
           std::string(ceiling) + "); взять его может только человек — галочкой или профилем «выбрать всё» "
           "(SPEC §4 FR-3, FR-4)";
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

// Ранг уровня риска: чем больше, тем опаснее. Порядок задан моделью
// (core/model.hpp: Safe < Review < Risky) и нужен, чтобы «Review не выше того
// уровня, что разрешён по умолчанию» не переписывалось вручную в трёх местах.
int safetyRank(SafetyLevel safety) {
    switch (safety) {
        case SafetyLevel::Safe: return 0;
        case SafetyLevel::Review: return 1;
        case SafetyLevel::Risky: return 2;
    }
    return 1;
}

// ASCII-в нижний регистр: сравнение путей на Windows нерегистрозависимо, а
// модель хранит UTF-8 (ADR-004), поэтому приводим только ASCII-буквы.
std::string lowerAscii(const std::string& text) {
    std::string out = text;
    for (char& symbol : out) {
        if (symbol >= 'A' && symbol <= 'Z') symbol = static_cast<char>(symbol - 'A' + 'a');
    }
    return out;
}

// «path лежит внутри root». Ровно та же граница, на которой держится запрет
// удаления чужих данных (FR-6): корень совпадает или путь продолжается
// разделителем. Без неё список разрешённого — это просто список путей.
bool insideRoot(const std::string& root, const std::string& path) {
    if (root.empty() || path.size() < root.size()) return false;
    if (lowerAscii(path.substr(0, root.size())) != lowerAscii(root)) return false;
    if (path.size() == root.size()) return true;
    const char next = path[root.size()];
    return next == '\\' || next == '/';
}

std::string thresholdReason(int confidence, int threshold) {
    return "уверенность " + std::to_string(confidence) + " ниже порога отбора " + std::to_string(threshold) +
           " — элемент не выбран по умолчанию";
}

PlanOperation makeOperation(std::size_t index, const CleanupCandidate& candidate, PlanAction action, SkipReason skip,
                            std::uint64_t bytes, const std::string& reason, const CandidateManifest* manifest) {
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
    op.evidence = candidate.reasons;  // FR-4: «почему это мусор» едет вместе со строкой

    // Что удалится на самом деле (F-01): список из манифеста, а не путь
    // корня. Полный список — в манифесте, здесь только начало: человек
    // подтверждает состав, а не перечитывает двести тысяч строк.
    if (manifest != nullptr && isRemovableAction(action)) {
        if (manifest->rootDeleteAllowed || manifest->allowed == nullptr) {
            op.rootDeleteOnly = true;
            op.allowedCount = 1;
        } else {
            op.rootDeleteOnly = false;
            const AllowedSet& allowed = *manifest->allowed;
            op.allowedCount = allowed.entries.size();
            for (const AllowedEntry& entry : allowed.entries) {
                if (op.allowedPaths.size() >= kMaxOperationPaths) break;
                op.allowedPaths.push_back(entry.path);
            }
        }
    }
    return op;
}

// Строка dry-run: категория, элемент, объём, действие, УРОВЕНЬ РИСКА и причина
// (FR-5 + §12). Уровень риска и объяснение — обязательные части строки, а не
// украшение: это последнее подтверждение перед удалением.
std::string operationLine(std::size_t number, const PlanOperation& op) {
    std::string line = "  " + std::to_string(number) + ". [" + actionLabel(op.action) + "] ";
    line += op.displayName.empty() ? op.path : op.displayName;
    line += " — " + formatBytes(op.bytes);
    line += " · уровень риска: " + std::string(riskLabel(op.safety));
    line += " · уверенность " + std::to_string(op.confidence);
    if (!op.path.empty() && op.path != op.displayName) line += " · " + op.path;
    if (!op.rootDeleteOnly && op.allowedCount > 0) {
        // «Удаляется не каталог, а вот эти файлы» — сама строка, без неё
        // человек подтверждает то, чего не видел (docs/review-02.md F-01).
        line += " · удаляется по списку: " + std::to_string(op.allowedCount) + " " + itemWord(op.allowedCount);
        if (op.allowedPaths.size() < op.allowedCount) line += " (показаны первые " + std::to_string(op.allowedPaths.size()) + ")";
    }
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
        case SkipReason::ReviewOffByDefault: return "review-off-by-default";
        case SkipReason::EstimateOnly: return "estimate-only";
        case SkipReason::NoSizeThreshold: return "no-size-threshold";
        case SkipReason::NeedsEnumeration: return "needs-enumeration";
    }
    return "none";
}

bool isUserDataCategory(std::string_view category) {
    // Категория из SPEC §4 FR-3, элемент которой — данные пользователя, а не
    // мусор. Список короткий и явный: угадывать по названию правила нельзя,
    // потому что цена ошибки — удалённый рабочий стол (F-02).
    return category == "user.bigfiles";
}

std::shared_ptr<const AllowedSet> CandidateManifest::makeAllowedSet(std::vector<AllowedEntry> entries,
                                                                   bool complete, std::size_t omitted) {
    auto set = std::make_shared<AllowedSet>();
    for (const AllowedEntry& entry : entries) {
        set->bytes += entry.allocatedBytes;
    }
    set->entries = std::move(entries);
    set->complete = complete;
    set->omitted = omitted;
    return set;
}

bool CandidateManifest::deletable() const {
    if (estimateOnly) return false;
    if (rootDeleteAllowed) return true;
    if (allowed == nullptr) return false;  // список не получен: корень удалять нельзя
    if (!allowed->complete) return false;   // список неполон: неполнота и «удалить всё» несовместимы
    if (allowed->entries.empty()) return false;  // правило не оставило ни одного файла
    return true;
}

std::string CandidateManifest::blockReason() const {
    if (estimateOnly) {
        return "правило «" + (ruleId.empty() ? std::string("без имени") : ruleId) +
               "» объявлено «только оценка»: элемент не удаляется ни при каком протверждении (SPEC §4 FR-3)";
    }
    if (rootDeleteAllowed) return {};
    if (allowed == nullptr) {
        return "правило «" + (ruleId.empty() ? std::string("без имени") : ruleId) +
               "» может что-то отсечь внутри корня, а список разрешённых файлов не получен: "
               "удалять каталог целиком нельзя (SPEC §4 FR-7)";
    }
    if (!allowed->complete) {
        return "список разрешённых файлов неполон: обход прерван или список обрезан (осталось неучтённых путей: " +
               std::to_string(allowed->omitted) + ") — удалять нельзя, иначе пропадёт и неучтённое";
    }
    return "правило не оставило ни одного файла для удаления";
}

bool isRuleBlock(SkipReason reason) {
    // Причины, которые нельзя снять ни профилем, ни подтверждением, ни сменой
    // уровня: запрет задано самим правилом (docs/review-02.md F-01..F-04).
    return reason == SkipReason::EstimateOnly || reason == SkipReason::NoSizeThreshold ||
           reason == SkipReason::NeedsEnumeration;
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

SelectionDecision decideCandidate(const CleanupCandidate& candidate, const PlanOptions& options,
                                  const CandidateManifest* manifest) {
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

    // 2. «Только оценка»: запрет задаёт правило, и снимать его нечем — ни
    //    --allow-risky, ни профиль «выбрать всё», ни смена уровня элемента
    //    (docs/review-02.md F-04: образ WSL и WinSxS удалялись целиком).
    if (manifest != nullptr && manifest->estimateOnly) {
        decision.skip = SkipReason::EstimateOnly;
        decision.reason = manifest->blockReason();
        return decision;
    }

    // 3. Risky по умолчанию скрыт: только «показать все» с подтверждением (FR-4, §12).
    if (candidate.safety == SafetyLevel::Risky && !options.allowRisky) {
        decision.skip = SkipReason::RiskyHidden;
        decision.reason = "уровень Risky скрыт по умолчанию — включить может только пользователь";
        return decision;
    }

    // 4. Пользовательские данные: FR-3 «файлы > 1 ГБ». Пока порога размера в
    //    схеме правил не было, категория брала весь каталог документов, и
    //    уверенность 100 при пороге 50 отбирала его по умолчанию (F-02).
    const bool userData = manifest != nullptr ? manifest->userData : isUserDataCategory(candidate.category);
    const std::uint64_t minFileBytes = manifest != nullptr ? manifest->minFileBytes : 0;
    if (userData && minFileBytes == 0) {
        decision.skip = SkipReason::NoSizeThreshold;
        decision.reason = "категория «" + candidate.category +
                          "» не объявляет порог размера файла: без него элемент не выбирается вовсе "
                          "(SPEC §4 FR-3 «файлы > 1 ГБ»)";
        return decision;
    }

    // 5. Что именно будет удалено. Корнем каталога нельзя: если правило хоть
    //    что-то отсекает (min-age, исключения, порог размера, фильтр листьев),
    //    удалять можно только перечисленные файлы, а их список обязателен
    //    (docs/review-02.md F-01: «по возрасту отсечено файлов: 412» описывало
    //    счёт, а удалялся весь %TEMP% вместе со свежими файлами).
    if (manifest == nullptr || !manifest->deletable()) {
        decision.skip = SkipReason::NeedsEnumeration;
        decision.reason = manifest != nullptr
                              ? manifest->blockReason()
                              : "нет списка разрешённых файлов: удалять каталог целиком нельзя (SPEC §4 FR-7)";
        return decision;
    }

    // 6. Слишком мелкий вклад в освобождение места.
    if (options.minReclaimBytes != 0 && candidate.allocatedBytes < options.minReclaimBytes) {
        decision.skip = SkipReason::TooSmall;
        decision.reason = "объём " + formatBytes(candidate.allocatedBytes) + " ниже минимального порога " +
                          formatBytes(options.minReclaimBytes);
        return decision;
    }

    // 7. Профиль отбора и порог уверенности.
    switch (options.profile) {
        case SelectionProfile::SafeOnly:
            // Отказ тот же, что у recommended с потолком safe, — отличается
            // только токен (profile-filtered) и то, кто задал потолок: профиль.
            if (candidate.safety != SafetyLevel::Safe) {
                decision.skip = SkipReason::ProfileFiltered;
                decision.reason = safetyCeilingReason(candidate.safety, "профиль «только безопасное»");
                return decision;
            }
            break;
        case SelectionProfile::Recommended: {
            // Review по умолчанию выключен: FR-3 («выкл. по умолчанию, с явным
            // подтверждением») и FR-4. Раньше единственным «не брать по
            // умолчанию» был Risky, и пароли браузера попадали в план сразу
            // после скана (docs/review-02.md F-03).
            //
            // Подтверждённый Risky (allowRisky — это и есть «показать всё» с
            // согласием) потолком не повторяется: иначе явное действие
            // оказывалось бы слабее умолчания.
            if (candidate.safety == SafetyLevel::Risky && options.allowRisky) break;
            if (safetyRank(candidate.safety) > safetyRank(options.maxDefaultSafety)) {
                // Review и уровни выше него — один и тот же отказ от потолка
                // по умолчанию; токены разные, потому что по ним видно, что
                // человек собирался включить (см. SkipReason в plan.hpp).
                const bool review = candidate.safety == SafetyLevel::Review;
                decision.skip = review ? SkipReason::ReviewOffByDefault : SkipReason::ProfileFiltered;
                decision.reason = safetyCeilingReason(candidate.safety, "потолок " +
                                                                  std::string(safetyToken(options.maxDefaultSafety)));
                return decision;
            }
            break;
        }
        case SelectionProfile::Everything:
            // «Выбрать всё» — явное действие человека, и оно берёт Review.
            // Risky при этом по-прежнему требует подтверждения «показать всё».
            break;
    }
    if (options.profile != SelectionProfile::Everything && candidate.confidence < threshold) {
        decision.skip = SkipReason::BelowConfidence;
        decision.reason = thresholdReason(candidate.confidence, threshold);
        return decision;
    }

    // 8. Выбран. Куда именно: в корзину приложения (её можно восстановить) или
    //    напрямую — большие кэши в корзине раздувают её (FR-7, ADR-005/006).
    //    Пользовательские данные — всегда через корзину: категория состоит из
    //    крупных файлов, то есть почти всегда «больше порога», и прямое
    //    удаление документов необратимо (docs/review-02.md F-02).
    decision.selected = true;
    if (userData) {
        decision.action = PlanAction::Trash;
        decision.reason = "выбран по умолчанию; удаление через корзину приложения — операцию можно отменить; "
                          "для пользовательских данных прямое удаление запрещено (SPEC §4 FR-7)";
    } else if (options.useTrash && candidate.allocatedBytes <= options.trashDirectDeleteAboveBytes) {
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

CleanupPlan buildPlan(const std::vector<CleanupCandidate>& candidates, const PlanOptions& options,
                      const std::vector<CandidateManifest>* manifests) {
    CleanupPlan plan;
    plan.options = options;
    plan.dryRun = options.dryRun;
    plan.items.reserve(candidates.size());
    plan.totals.candidateCount = candidates.size();

    // Манифесты копируются в план (списки файлов разделяются указателем), и
    // план остаётся самодостаточным: исполнитель получает из него и решение,
    // и перечень того, что удалять. Порядок — по индексу кандидата, чтобы
    // поиск был двоичным, а не линейным на каждом элементе.
    if (manifests != nullptr) plan.manifests = *manifests;
    std::stable_sort(plan.manifests.begin(), plan.manifests.end(),
                     [](const CandidateManifest& a, const CandidateManifest& b) {
                         return a.candidateIndex < b.candidateIndex;
                     });

    std::vector<CategoryAggregate> categories;

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const CleanupCandidate& candidate = candidates[index];
        const CandidateManifest* manifest = plan.manifestFor(index);
        const SelectionDecision decision = decideCandidate(candidate, options, manifest);

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
                case SkipReason::ProfileFiltered:
                case SkipReason::ReviewOffByDefault: ++plan.totals.profileFilteredCount; break;
                // Запреты, заданные правилом, и «нет списка разрешённого» — не
                // «плохо набралось», а «нельзя»: в цифрах профиля они не живут,
                // но видны в dry-run и в reasons элемента.
                case SkipReason::EstimateOnly:
                case SkipReason::NoSizeThreshold:
                case SkipReason::NeedsEnumeration:
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

const CandidateManifest* CleanupPlan::manifestFor(std::size_t candidateIndex) const {
    // manifests отсортированы по индексу (buildPlan), поэтому достаточно
    // двоичного поиска; nullptr — сборка не дала манифеста, и тогда элемент
    // по правилам decideCandidate удалять нельзя.
    const auto found = std::lower_bound(manifests.begin(), manifests.end(), candidateIndex,
                                        [](const CandidateManifest& manifest, std::size_t index) {
                                            return manifest.candidateIndex < index;
                                        });
    if (found == manifests.end() || found->candidateIndex != candidateIndex) return nullptr;
    return &*found;
}

std::uint64_t CleanupPlan::planSignature() const {
    std::uint64_t hash = kFnvOffsetBasis;
    mixValue(hash, static_cast<std::uint64_t>(options.profile));
    mixValue(hash, static_cast<std::uint64_t>(effectiveThreshold(options)));
    mixValue(hash, static_cast<std::uint64_t>(safetyRank(options.maxDefaultSafety)));
    mixValue(hash, options.allowRisky ? 1ull : 0ull);
    mixValue(hash, options.useTrash ? 1ull : 0ull);
    mixValue(hash, options.minReclaimBytes);
    mixValue(hash, dryRun ? 1ull : 0ull);
    for (const CleanupPlanItem& item : items) {
        mixValue(hash, static_cast<std::uint64_t>(item.candidateIndex));
        mixValue(hash, static_cast<std::uint64_t>(item.action));
        mixValue(hash, item.reclaimBytes);
        // Со СПИСКОМ удаляемого в отпечатке: сменился набор файлов под
        // кандидатом — прежнее подтверждение dry-run относилось к другому
        // списку, даже если сумма байт совпала (FR-5).
        const CandidateManifest* manifest = manifestFor(item.candidateIndex);
        mixValue(hash, manifest == nullptr ? 0ull : 1ull);
        if (manifest != nullptr) {
            mixValue(hash, manifest->rootDeleteAllowed ? 1ull : 0ull);
            mixValue(hash, manifest->estimateOnly ? 1ull : 0ull);
            mixValue(hash, manifest->minFileBytes);
            mixValue(hash, manifest->allowed == nullptr
                                ? 0ull
                                : static_cast<std::uint64_t>(manifest->allowed->entries.size()));
            mixValue(hash, manifest->allowed == nullptr ? 0ull : manifest->allowed->bytes);
        }
    }
    return hash;
}

std::string operationReason(const PlanOperation& op) {
    std::string text;
    const auto append = [&text](const std::string& part) {
        if (part.empty()) return;
        if (!text.empty()) text += "; ";
        text += part;
    };
    // Сначала «почему это мусор» (FR-4: объяснение кандидата), затем «почему
    // такое действие» (FR-5: решение плана). Порядок не декоративный: человек
    // сначала решает, мусор ли это, и только потом — куда оно денется.
    for (const std::string& line : op.evidence) append(line);
    append(op.reason);
    if (text.empty()) text = "причина не указана";  // не молчим: пустота — тоже ответ
    return text;
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
        const CandidateManifest* manifest = plan.manifestFor(item.candidateIndex);
        const SelectionDecision decision = decideCandidate(candidate, plan.options, manifest);
        if (item.action != decision.action) {
            // План и кандидат разошлись — доверяем решению по кандидату и не показываем
            // пользователю операцию, которую движок выполнит иначе.
            continue;
        }
        PlanOperation op = makeOperation(item.candidateIndex, candidate, item.action, decision.skip, item.reclaimBytes,
                                        decision.reason, manifest);
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
        text += "\n    причина: " + operationReason(report.operations[i]) + "\n";
    }
    text += "Не трогаем (" + std::to_string(report.untouched.size()) + "):\n";
    if (report.untouched.empty()) {
        text += "  — нет\n";
    }
    for (std::size_t i = 0; i < report.untouched.size(); ++i) {
        text += operationLine(i + 1, report.untouched[i]);
        text += "\n    причина: " + operationReason(report.untouched[i]) + "\n";
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
        std::vector<Value> evidence;
        evidence.reserve(op.evidence.size());
        for (const std::string& line : op.evidence) evidence.push_back(Value(line));
        std::vector<Value> allowed;
        allowed.reserve(op.allowedPaths.size());
        for (const std::string& path : op.allowedPaths) allowed.push_back(Value(path));
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
            {"rootDeleteOnly", Value(op.rootDeleteOnly)},
            {"allowedCount", Value(static_cast<double>(op.allowedCount))},
            {"allowedPaths", Value::array(std::move(allowed))},
            {"reason", Value(operationReason(op))},
            {"evidence", Value::array(std::move(evidence))},
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
             {"maxDefaultSafety", Value(safetyToken(plan.options.maxDefaultSafety))},
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
            {"reason", Value(operationReason(op))},
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
            // ГЛАВНЫЙ инвариант F-01: операция удаления обязана нести список
            // того, что правило разрешило, либо право удалить корень целиком.
            // Оба «не доказаны» — это ровно тот случай, которым в review-02
            // сносили %TEMP% вместе со свежими и исключёнными файлами.
            const CandidateManifest* manifest = plan.manifestFor(i);
            if (manifest == nullptr) {
                problems.push_back("кандидат " + std::to_string(i) +
                                   " выбран к удалению, но манифест (список разрешённого) не получен");
            } else if (!manifest->deletable()) {
                problems.push_back("кандидат " + std::to_string(i) + " выбран к удалению, хотя по правилу нельзя: " +
                                   manifest->blockReason());
            } else if (!manifest->rootDeleteAllowed && manifest->allowed != nullptr) {
                const AllowedSet& allowed = *manifest->allowed;
                std::uint64_t sum = 0;
                for (const AllowedEntry& entry : allowed.entries) {
                    sum += entry.allocatedBytes;
                    if (!insideRoot(candidate.path, entry.path)) {
                        problems.push_back("кандидат " + std::to_string(i) + ": в списке удаляемого путь вне корня " +
                                           entry.path);
                    }
                    if (entry.allocatedBytes < manifest->minFileBytes) {
                        problems.push_back("кандидат " + std::to_string(i) + ": в списке удаляемого файл " +
                                           entry.path + " меньше порога правила " +
                                           std::to_string(manifest->minFileBytes) + " байт");
                    }
                }
                if (sum != candidate.allocatedBytes) {
                    // Обещание «освободится N» и список того, что удаляем, обязаны
                    // сходиться: иначе отчёт показывает одну цифру, а удаляется
                    // другое множество файлов.
                    problems.push_back("кандидат " + std::to_string(i) + ": сумма списка удаляемого " +
                                       std::to_string(sum) + " != allocatedBytes " +
                                       std::to_string(candidate.allocatedBytes));
                }
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
