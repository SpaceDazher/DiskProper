// Юнит-тесты core::plan: агрегаты плана, порог отбора и поведение на пустых
// кандидатах. Задача 76.
//
// Спека: §9.1 «Юнит-тесты (Core, без Windows) — правила, glob, min-age, скоринг,
// JSON, форматирование, инварианты модели», §11.1 (пункт 1 стратегии:
// переносимое ядро проверяется на каждом PR за секунды), §12 — «Ни один элемент
// не удаляется без видимого объяснения и уровня риска», §4 FR-4/FR-5/FR-7.
//
// Три свойства, вокруг которых построен весь файл, — все три бьют по пользователю,
// а не по тесту:
//
//   1. АГРЕГАТЫ — это три цифры, которые человек видит на экране «Очистка»
//      («сколько освободим, если выбрать всё / только Safe / только выбранное»)
//      плюс счётчики скрытого. Ошибка в агрегате — это обещание пользователю
//      «освободится 12 ГБ», которого не случится, поэтому суммы проверяются
//      вручную по каждому кандидату, а не «скопировано из реализации».
//      Отдельно проверяется, что заблокированные файлы не попадают ни в одну
//      цифру обещаний: обещать место, которое удалить нельзя, хуже, чем не
//      обещать ничего.
//   2. ПОРОГ ОТБОРА — граница, где «Review с низкой уверенностью» перестаёт
//      быть мусором по умолчанию. Проверяются сама граница (равенство порогу
//      выбирает), приведение порога к шкале 0..100 (иначе «порог 500» молча
//      отбрал бы всё, а «порог -1» — ничего), профили отбора и «показать всё»
//      для Risky, а также то, что заблокированный кандидат не проходит ни через
//      какой профиль.
//   3. ПУСТЫЕ КАНДИДАТЫ — не редкий крайний случай, а обычное состояние экрана
//      сразу после запуска и на диске без мусора. Ни один агрегат, dry-run,
//      снимок и JSON в этом состоянии не имеет права падать или показывать
//      «0 элементов, которые мы удалим» с ненулевыми байтами.
//
// Модуль переносимый (SPEC §6.1): здесь нет ни Windows API, ни ввода-вывода, ни
// файловой системы — весь вход в памяти, часов системы нет, время приходит полем.
// main() живёт в core_tests.cpp (tests/unit/CMakeLists.txt).
#include "harness.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"
#include "model.hpp"
#include "plan.hpp"

using mrproper::json::Value;
using namespace mrproper::core;

namespace {

// Объёмы, на которых считаются агрегаты: мелочь (1..9 КБ) и одно крупное
// значение 200 МБ, разведённое далеко за порогом корзины (100 МиБ). Тест не
// зависит от круглых констант: важно лишь «меньше / равно / больше».
constexpr std::uint64_t kSmall = 1000;
constexpr std::uint64_t kMedium = 5000;
constexpr std::uint64_t kLarge = 7000;
constexpr std::uint64_t kRiskyVolume = 9000;
constexpr std::uint64_t kHuge = 200ull * 1000ull * 1000ull;  // 200 МБ > 100 МиБ

// Кандидат с заполненными обязательными полями. Значения по умолчанию — «обычный
// Review, уверенность 80», то есть проходит порог по умолчанию: тест, который
// хочет непроходной кандидат, меняет safety или confidence явно и читаемо.
CleanupCandidate candidate(const std::string& category, SafetyLevel safety, int confidence,
                           std::uint64_t bytes) {
    CleanupCandidate c;
    c.ruleId = "rule." + category;
    c.category = category;
    c.path = "C:/Test/" + category + "/" + std::to_string(bytes);
    c.displayName = category + " " + std::to_string(bytes);
    c.logicalBytes = bytes;
    c.allocatedBytes = bytes;
    c.fileCount = 1;
    c.oldestWrite = 1000000;
    c.newestWrite = 1000000;
    c.lastAccess = 1000000;
    c.safety = safety;
    c.confidence = confidence;
    c.reasons.push_back("тестовый кандидат");
    return c;
}

// Кандидат с приложениями, которые держат файлы (Restart Manager, FR-6).
CleanupCandidate lockedCandidate(const std::string& category, SafetyLevel safety, int confidence,
                                 std::uint64_t bytes, std::uint32_t lockers) {
    CleanupCandidate c = candidate(category, safety, confidence, bytes);
    for (std::uint32_t i = 0; i < lockers; ++i) {
        c.lockedBy.push_back(ProcessRef{1000 + i, "app" + std::to_string(i)});
    }
    return c;
}

// Смешанный набор, на котором проверяются агрегаты: три выбранных кандидата
// (в том числе один больше порога корзины), один ниже порога уверенности, один
// скрытый Risky и один заблокированный. Состав подобран так, чтобы каждая
// цифра агрегатов была различима: суммы «выбрано», «всё» и «только Safe»
// отличаются друг от друга.
std::vector<CleanupCandidate> mixedCandidates() {
    std::vector<CleanupCandidate> list;
    list.push_back(candidate("temp", SafetyLevel::Safe, 90, kSmall));    // 0: корзина
    list.push_back(candidate("logs", SafetyLevel::Review, 80, kMedium));  // 1: корзина
    list.push_back(candidate("logs", SafetyLevel::Review, 20, kLarge));   // 2: ниже порога
    list.push_back(candidate("other", SafetyLevel::Risky, 95, kRiskyVolume));  // 3: скрыт Risky
    list.push_back(candidate("cache", SafetyLevel::Safe, 60, kHuge));     // 4: прямое удаление
    list.push_back(lockedCandidate("cache", SafetyLevel::Safe, 70, kSmall, 2));  // 5: занято
    return list;
}

// Смешанный набор без заблокированного: удобно там, где блокировка мешает
// считать «все байты» иначе, чем обычно.
std::vector<CleanupCandidate> unlockedCandidates() {
    std::vector<CleanupCandidate> list;
    list.push_back(candidate("temp", SafetyLevel::Safe, 90, kSmall));
    list.push_back(candidate("logs", SafetyLevel::Review, 80, kMedium));
    list.push_back(candidate("other", SafetyLevel::Risky, 95, kRiskyVolume));
    list.push_back(candidate("cache", SafetyLevel::Safe, 60, kHuge));
    return list;
}

// Опции «показать все»: Risky разрешён — единственный способ увидеть опасный
// уровень (SPEC §4 FR-4, §12).
PlanOptions riskyAllowed() {
    PlanOptions options;
    options.allowRisky = true;
    return options;
}

const CategoryAggregate* categoryOf(const CleanupPlan& plan, const std::string& name) {
    for (const CategoryAggregate& entry : plan.categories) {
        if (entry.category == name) return &entry;
    }
    return nullptr;
}

}  // namespace

// ------------------------------------------------- пустые кандидаты

TEST(plan_emptyCandidates_givesEmptyPlan) {
    const CleanupPlan plan = buildPlan({});
    CHECK(plan.empty());
    CHECK(plan.items.empty());
    CHECK(plan.categories.empty());
    CHECK_EQ(plan.totals.candidateCount, std::size_t{0});
    CHECK_EQ(plan.totals.selectedCount, std::size_t{0});
    CHECK_EQ(plan.totals.selectedBytes, std::uint64_t{0});
    CHECK_EQ(plan.totals.allCount, std::size_t{0});
    CHECK_EQ(plan.totals.allBytes, std::uint64_t{0});
    CHECK_EQ(plan.totals.safeOnlyCount, std::size_t{0});
    CHECK_EQ(plan.totals.safeOnlyBytes, std::uint64_t{0});
    CHECK(validatePlan({}, plan).empty());
}

TEST(plan_emptyCandidates_skipCountersAreZero) {
    const CleanupPlan plan = buildPlan({});
    CHECK_EQ(plan.totals.hiddenRiskyCount, std::size_t{0});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{0});
    CHECK_EQ(plan.totals.tooSmallCount, std::size_t{0});
    CHECK_EQ(plan.totals.profileFilteredCount, std::size_t{0});
    CHECK_EQ(plan.totals.byAction.deleteOps.count, std::size_t{0});
    CHECK_EQ(plan.totals.byAction.trashOps.count, std::size_t{0});
    CHECK_EQ(plan.totals.byAction.keepOps.count, std::size_t{0});
    CHECK_EQ(plan.totals.byAction.skipLockedOps.count, std::size_t{0});
    CHECK_EQ(plan.totals.byAction.deleteOps.bytes, std::uint64_t{0});
    CHECK_EQ(plan.totals.byAction.keepOps.bytes, std::uint64_t{0});
}

TEST(plan_emptyCandidates_hasNoOperations) {
    const CleanupPlan plan = buildPlan({});
    CHECK_EQ(plan.operationCount(), std::size_t{0});
    CHECK(plan.operationIndexes().empty());
    CHECK(plan.item(0) == nullptr);
}

TEST(plan_emptyCandidates_dryRunShowsNothingToDo) {
    const std::vector<CleanupCandidate> list;
    const CleanupPlan plan = buildPlan(list);
    const DryRunReport report = makeDryRunReport(list, plan);
    CHECK(report.dryRun);
    CHECK_EQ(report.operationCount, std::size_t{0});
    CHECK_EQ(report.totalBytes, std::uint64_t{0});
    CHECK(report.operations.empty());
    CHECK(report.untouched.empty());
    CHECK_EQ(report.headline, std::string("Освободится 0 Б — 0 элементов из 0 элементов"));
    CHECK(report.text.find("ни один элемент") != std::string::npos);
}

TEST(plan_emptyCandidates_snapshotIsEmpty) {
    const std::vector<CleanupCandidate> list;
    const CleanupPlan plan = buildPlan(list);
    PlanSnapshotContext context;
    context.appVersion = "1.0.0-test";
    context.pid = 4242;
    context.createdAtUnix = 1700000000;
    const PlanSnapshot snapshot = makeSnapshot(list, plan, context);
    CHECK_EQ(snapshot.operationCount, std::size_t{0});
    CHECK_EQ(snapshot.totalBytes, std::uint64_t{0});
    CHECK(snapshot.operations.empty());
    CHECK_EQ(snapshot.appVersion, context.appVersion);
    CHECK_EQ(snapshot.pid, static_cast<std::uint32_t>(4242));
    CHECK_EQ(snapshot.planSignature, plan.planSignature());
}

TEST(plan_emptyCandidates_jsonHasZeroTotals) {
    const std::vector<CleanupCandidate> list;
    const CleanupPlan plan = buildPlan(list);
    const Value doc = mrproper::json::parse(planToJson(list, plan));
    CHECK(doc.isObject());
    const Value* totals = doc.find("totals");
    CHECK(totals != nullptr);
    if (totals != nullptr) {
        CHECK_EQ(totals->find("candidates")->asNumber(), 0.0);
        CHECK_EQ(totals->find("selected")->asNumber(), 0.0);
        CHECK_EQ(totals->find("selectedBytes")->asNumber(), 0.0);
        CHECK_EQ(totals->find("allBytes")->asNumber(), 0.0);
        CHECK_EQ(totals->find("safeOnlyBytes")->asNumber(), 0.0);
    }
    CHECK_EQ(doc.find("operations")->items().size(), std::size_t{0});
    CHECK_EQ(doc.find("untouched")->items().size(), std::size_t{0});
    CHECK_EQ(doc.find("categories")->items().size(), std::size_t{0});
    CHECK_EQ(doc.find("dryRun")->asBool(), true);
}

TEST(plan_emptyCandidates_signatureDependsOnOptionsOnly) {
    const CleanupPlan a = buildPlan({});
    const CleanupPlan b = buildPlan({});
    CHECK_EQ(a.planSignature(), b.planSignature());

    // Подпись — это «показывали ли именно этот план»: пустой план с другим
    // порогом или профилем это уже другой план, даже если удалять нечего.
    PlanOptions options;
    options.confidenceThreshold = 90;
    const CleanupPlan c = buildPlan({}, options);
    CHECK(a.planSignature() != c.planSignature());

    PlanOptions risky;
    risky.allowRisky = true;
    CHECK(a.planSignature() != buildPlan({}, risky).planSignature());
}

TEST(plan_emptyCandidates_dryRunGateDoesNotBlock) {
    const CleanupPlan plan = buildPlan({});
    DryRunGate gate;
    CHECK(!gate.mustShowBeforeExecute(plan));
    gate.acknowledge(plan);
    CHECK(gate.alreadyShown(plan));
    CHECK(!gate.mustShowBeforeExecute(plan));
}

// ------------------------------------------------- агрегаты

TEST(plan_aggregates_selectedCountAndBytes) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    CHECK_EQ(plan.totals.candidateCount, std::size_t{6});
    // Выбраны 0, 1 и 4: 1000 + 5000 + 200000000.
    CHECK_EQ(plan.totals.selectedCount, std::size_t{3});
    CHECK_EQ(plan.totals.selectedBytes, kSmall + kMedium + kHuge);
    // Поштучная проверка: сумма по элементам плана обязана совпасть с totals.
    std::uint64_t byItems = 0;
    for (const CleanupPlanItem& item : plan.items) byItems += item.reclaimBytes;
    CHECK_EQ(byItems, plan.totals.selectedBytes);
    CHECK(validatePlan(list, plan).empty());
}

TEST(plan_aggregates_allExcludesLocked) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    // Кандидат 5 заблокирован: его 1000 байт не обещаются ни в одной цифре.
    CHECK_EQ(plan.totals.allCount, std::size_t{5});
    CHECK_EQ(plan.totals.allBytes, kSmall + kMedium + kLarge + kRiskyVolume + kHuge);
}

TEST(plan_aggregates_safeOnlyCountsOnlySafeLevel) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    // Safe без блокировки — кандидаты 0 и 4.
    CHECK_EQ(plan.totals.safeOnlyCount, std::size_t{2});
    CHECK_EQ(plan.totals.safeOnlyBytes, kSmall + kHuge);
}

TEST(plan_aggregates_safeOnlyExcludesLockedSafe) {
    // Кандидат Safe, но занятый: в «только Safe» он не входит — иначе экран
    // обещал бы место, которое удалить нельзя.
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    CHECK(list[5].safety == SafetyLevel::Safe);
    CHECK(!list[5].lockedBy.empty());
    CHECK(plan.item(5) != nullptr);
    if (plan.item(5) != nullptr) {
        CHECK(plan.item(5)->action == PlanAction::SkipLocked);
        CHECK_EQ(plan.item(5)->reclaimBytes, std::uint64_t{0});
    }
    CHECK_EQ(plan.totals.safeOnlyBytes, kSmall + kHuge);
}

TEST(plan_aggregates_byActionCountsAndBytes) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    const PlanActionTotals& byAction = plan.totals.byAction;
    // Delete: только кандидат 4 (200 МБ — больше порога корзины).
    CHECK_EQ(byAction.deleteOps.count, std::size_t{1});
    CHECK_EQ(byAction.deleteOps.bytes, kHuge);
    // Trash: кандидаты 0 и 1.
    CHECK_EQ(byAction.trashOps.count, std::size_t{2});
    CHECK_EQ(byAction.trashOps.bytes, kSmall + kMedium);
    // Keep: кандидаты 2 и 3 — освобождаемого места им не обещаем.
    CHECK_EQ(byAction.keepOps.count, std::size_t{2});
    CHECK_EQ(byAction.keepOps.bytes, std::uint64_t{0});
    // SkipLocked: кандидат 5.
    CHECK_EQ(byAction.skipLockedOps.count, std::size_t{1});
    CHECK_EQ(byAction.skipLockedOps.bytes, std::uint64_t{0});
    // forAction отдаёт тот же слот, что и поле структуры.
    CHECK_EQ(byAction.forAction(PlanAction::Delete).count, byAction.deleteOps.count);
    CHECK_EQ(byAction.forAction(PlanAction::Trash).count, byAction.trashOps.count);
    CHECK_EQ(byAction.forAction(PlanAction::Keep).count, byAction.keepOps.count);
    CHECK_EQ(byAction.forAction(PlanAction::SkipLocked).count, byAction.skipLockedOps.count);
    CHECK_EQ(byAction.deleteOps.count + byAction.trashOps.count + byAction.keepOps.count +
                 byAction.skipLockedOps.count,
             plan.totals.candidateCount);
}

TEST(plan_aggregates_skipReasonCounters) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    CHECK_EQ(plan.totals.hiddenRiskyCount, std::size_t{1});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{1});
    CHECK_EQ(plan.totals.tooSmallCount, std::size_t{0});
    CHECK_EQ(plan.totals.profileFilteredCount, std::size_t{0});
}

TEST(plan_aggregates_tooSmallAndProfileFilteredCounters) {
    PlanOptions options;
    options.profile = SelectionProfile::SafeOnly;
    options.minReclaimBytes = kMedium;
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list, options);
    // Профиль SafeOnly отсекает Review (1, 2) и Risky (3) — 3 кандидата;
    // TooSmall дополнительно убирает кандидата 0 (1000 < 5000).
    CHECK_EQ(plan.totals.profileFilteredCount, std::size_t{2});
    CHECK_EQ(plan.totals.tooSmallCount, std::size_t{1});
    // Risky отсекается раньше профиля: счётчик скрытого Risky, а не профиля.
    CHECK_EQ(plan.totals.hiddenRiskyCount, std::size_t{1});
    CHECK_EQ(plan.totals.selectedCount, std::size_t{1});
    CHECK_EQ(plan.totals.selectedBytes, kHuge);
}

TEST(plan_categories_sortedBySelectedBytes) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    CHECK_EQ(plan.categories.size(), std::size_t{4});
    // cache (200 МБ — крупнейший выбранный) → logs (5000) → temp (1000) → other (0).
    CHECK_EQ(plan.categories[0].category, std::string("cache"));
    CHECK_EQ(plan.categories[1].category, std::string("logs"));
    CHECK_EQ(plan.categories[2].category, std::string("temp"));
    CHECK_EQ(plan.categories[3].category, std::string("other"));
    for (std::size_t i = 1; i < plan.categories.size(); ++i) {
        CHECK(plan.categories[i - 1].selectedBytes >= plan.categories[i].selectedBytes);
    }
}

TEST(plan_categories_tieBrokenByNameAscending) {
    std::vector<CleanupCandidate> list;
    list.push_back(candidate("zulu", SafetyLevel::Review, 80, kSmall));
    list.push_back(candidate("alpha", SafetyLevel::Review, 80, kSmall));
    list.push_back(candidate("mike", SafetyLevel::Review, 80, kSmall));
    const CleanupPlan plan = buildPlan(list);
    CHECK_EQ(plan.categories.size(), std::size_t{3});
    // Одинаковые выбранные байты — порядок по имени, иначе список «дрожит»
    // между прогонами и скачет перед глазами.
    CHECK_EQ(plan.categories[0].category, std::string("alpha"));
    CHECK_EQ(plan.categories[1].category, std::string("mike"));
    CHECK_EQ(plan.categories[2].category, std::string("zulu"));
    CHECK_EQ(plan.categories[0].selectedBytes, plan.categories[1].selectedBytes);
}

TEST(plan_categories_splitBytesAndCounts) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);

    const CategoryAggregate* logs = categoryOf(plan, "logs");
    CHECK(logs != nullptr);
    if (logs != nullptr) {
        CHECK_EQ(logs->candidateCount, std::size_t{2});
        CHECK_EQ(logs->selectedCount, std::size_t{1});
        CHECK_EQ(logs->selectedBytes, kMedium);
        // Кандидат 2 не выбран, но он не заблокирован — в «всё» входит.
        CHECK_EQ(logs->allBytes, kMedium + kLarge);
        // Review в «только Safe» не входит.
        CHECK_EQ(logs->safeOnlyBytes, std::uint64_t{0});
    }

    const CategoryAggregate* temp = categoryOf(plan, "temp");
    CHECK(temp != nullptr);
    if (temp != nullptr) {
        CHECK_EQ(temp->selectedCount, std::size_t{1});
        CHECK_EQ(temp->selectedBytes, kSmall);
        CHECK_EQ(temp->safeOnlyBytes, kSmall);
    }
}

TEST(plan_categories_lockedBytesExcludedButCounted) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    const CategoryAggregate* cache = categoryOf(plan, "cache");
    CHECK(cache != nullptr);
    if (cache != nullptr) {
        // Два кандидата: выбранный 200 МБ и заблокированный 1000 Б.
        CHECK_EQ(cache->candidateCount, std::size_t{2});
        CHECK_EQ(cache->selectedCount, std::size_t{1});
        CHECK_EQ(cache->selectedBytes, kHuge);
        CHECK_EQ(cache->allBytes, kHuge);
        // Заблокированный Safe не попадает и в «только Safe».
        CHECK_EQ(cache->safeOnlyBytes, kHuge);
    }
}

TEST(plan_itemLookupIsByCandidateIndex) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    CHECK_EQ(plan.items.size(), list.size());
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        const CleanupPlanItem* item = plan.item(i);
        CHECK(item != nullptr);
        if (item != nullptr) {
            CHECK_EQ(item->candidateIndex, i);
            CHECK_EQ(item->action, plan.items[i].action);
            CHECK_EQ(item->reclaimBytes, plan.items[i].reclaimBytes);
        }
    }
    CHECK(plan.item(plan.items.size()) == nullptr);
}

TEST(plan_operationIndexesListRemovableInOrder) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    const std::vector<std::size_t> indexes = plan.operationIndexes();
    const std::vector<std::size_t> expected = {0, 1, 4};
    CHECK_EQ(indexes.size(), expected.size());
    CHECK_EQ(plan.operationCount(), expected.size());
    for (std::size_t i = 0; i < indexes.size() && i < expected.size(); ++i) {
        CHECK_EQ(indexes[i], expected[i]);
    }
    std::uint64_t sum = 0;
    for (const std::size_t index : indexes) sum += list[index].allocatedBytes;
    CHECK_EQ(sum, plan.totals.selectedBytes);
}

TEST(plan_validateAcceptsBuiltPlanForManyProfiles) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const SelectionProfile profiles[] = {SelectionProfile::SafeOnly, SelectionProfile::Recommended,
                                         SelectionProfile::Everything};
    for (const SelectionProfile profile : profiles) {
        PlanOptions options;
        options.profile = profile;
        options.allowRisky = true;
        CHECK(validatePlan(list, buildPlan(list, options)).empty());
    }
}

TEST(plan_validateCatchesTamperedReclaimBytes) {
    const std::vector<CleanupCandidate> list = unlockedCandidates();
    CleanupPlan plan = buildPlan(list);
    plan.items[0].reclaimBytes += 1;  // план обещает больше, чем отдаст движок
    const std::vector<std::string> problems = validatePlan(list, plan);
    CHECK(!problems.empty());
}

TEST(plan_validateCatchesForeignSkipLocked) {
    const std::vector<CleanupCandidate> list = unlockedCandidates();
    CleanupPlan plan = buildPlan(list);
    plan.items[0].action = PlanAction::SkipLocked;  // lockedBy пуст — нарушение инварианта §6.3
    const std::vector<std::string> problems = validatePlan(list, plan);
    CHECK(!problems.empty());
}

TEST(plan_validateCatchesWrongItemCount) {
    const std::vector<CleanupCandidate> list = unlockedCandidates();
    CleanupPlan plan = buildPlan(list);
    plan.items.pop_back();
    const std::vector<std::string> problems = validatePlan(list, plan);
    CHECK_EQ(problems.size(), std::size_t{1});
}

// ------------------------------------------------- порог отбора

TEST(plan_threshold_selectsAtTheBoundary) {
    PlanOptions options;
    options.confidenceThreshold = 80;
    const CleanupCandidate onLine = candidate("temp", SafetyLevel::Review, 80, kSmall);
    const CleanupCandidate below = candidate("temp", SafetyLevel::Review, 79, kSmall);
    const CleanupPlan plan = buildPlan({onLine, below}, options);
    CHECK_EQ(plan.totals.selectedCount, std::size_t{1});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{1});
    CHECK(plan.items[0].action != plan.items[1].action);
    CHECK(isRemovableAction(plan.items[0].action));
    CHECK(!isRemovableAction(plan.items[1].action));
    CHECK_EQ(plan.items[1].reclaimBytes, std::uint64_t{0});
}

TEST(plan_threshold_defaultMatchesScoringThreshold) {
    CHECK_EQ(kPlanDefaultConfidenceThreshold, kDefaultConfidenceThreshold);
    PlanOptions options;
    CHECK_EQ(options.confidenceThreshold, kPlanDefaultConfidenceThreshold);
    const CleanupCandidate weak = candidate("temp", SafetyLevel::Review, kPlanDefaultConfidenceThreshold - 1, kSmall);
    const CleanupPlan plan = buildPlan({weak});
    CHECK_EQ(plan.totals.selectedCount, std::size_t{0});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{1});
}

TEST(plan_threshold_aboveScaleIsClampedToHundred) {
    PlanOptions options;
    options.confidenceThreshold = 500;  // шкала уверенности 0..100
    const std::vector<CleanupCandidate> list = {candidate("temp", SafetyLevel::Review, 100, kSmall),
                                                 candidate("logs", SafetyLevel::Review, 99, kMedium)};
    const CleanupPlan plan = buildPlan(list, options);
    CHECK_EQ(plan.totals.selectedCount, std::size_t{1});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{1});
    CHECK_EQ(plan.items[0].reclaimBytes, kSmall);
}

TEST(plan_threshold_belowScaleIsClampedToZero) {
    PlanOptions options;
    options.confidenceThreshold = -10;  // иначе «порог -1» отбрал бы вообще всё
    const std::vector<CleanupCandidate> list = {candidate("temp", SafetyLevel::Review, 0, kSmall),
                                                 candidate("logs", SafetyLevel::Review, 0, kMedium)};
    const CleanupPlan plan = buildPlan(list, options);
    CHECK_EQ(plan.totals.selectedCount, std::size_t{2});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{0});
    CHECK_EQ(plan.totals.selectedBytes, kSmall + kMedium);
}

TEST(plan_threshold_skippedForEverythingProfile) {
    PlanOptions options;
    options.profile = SelectionProfile::Everything;
    const CleanupCandidate hopeless = candidate("temp", SafetyLevel::Review, 0, kSmall);
    const CleanupPlan plan = buildPlan({hopeless}, options);
    CHECK_EQ(plan.totals.selectedCount, std::size_t{1});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{0});
    CHECK_EQ(plan.totals.selectedBytes, kSmall);
}

TEST(plan_threshold_appliesInSafeOnlyProfile) {
    PlanOptions options;
    options.profile = SelectionProfile::SafeOnly;
    const CleanupCandidate safeButUnsure = candidate("temp", SafetyLevel::Safe, 20, kSmall);
    const CleanupPlan plan = buildPlan({safeButUnsure}, options);
    CHECK_EQ(plan.totals.selectedCount, std::size_t{0});
    CHECK_EQ(plan.totals.belowThresholdCount, std::size_t{1});
}

TEST(plan_threshold_changeIsVisibleInSignature) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    PlanOptions low;
    PlanOptions high;
    high.confidenceThreshold = 95;
    const CleanupPlan a = buildPlan(list, low);
    const CleanupPlan b = buildPlan(list, high);
    CHECK(a.planSignature() != b.planSignature());
    // Порог 95 выше уверенности всех шести кандидатов: удалять нечего.
    CHECK_EQ(b.totals.selectedCount, std::size_t{0});
    CHECK_EQ(b.totals.belowThresholdCount, std::size_t{4});
    CHECK_EQ(b.operationCount(), std::size_t{0});
    // Порог 50 — картина обычная: 0, 1 и 4.
    CHECK_EQ(a.totals.selectedCount, std::size_t{3});
}

TEST(plan_threshold_selectionChangeIsVisibleInSignature) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan base = buildPlan(list);
    const CleanupPlan risky = buildPlan(list, riskyAllowed());
    CHECK(base.planSignature() != risky.planSignature());
    CHECK_EQ(risky.totals.hiddenRiskyCount, std::size_t{0});
    CHECK_EQ(risky.totals.selectedCount, std::size_t{4});
}

TEST(plan_minReclaimBytes_boundaryAndBelow) {
    PlanOptions options;
    options.minReclaimBytes = kMedium;
    const std::vector<CleanupCandidate> list = {candidate("temp", SafetyLevel::Review, 80, kMedium),  // ровно порог
                                                 candidate("logs", SafetyLevel::Review, 80, kLarge),    // выше порога
                                                 candidate("other", SafetyLevel::Review, 80, kSmall)};   // ниже порога
    const CleanupPlan plan = buildPlan(list, options);
    CHECK_EQ(plan.totals.selectedCount, std::size_t{2});
    CHECK_EQ(plan.totals.tooSmallCount, std::size_t{1});
    CHECK_EQ(plan.totals.selectedBytes, kMedium + kLarge);
    CHECK_EQ(plan.items[2].reclaimBytes, std::uint64_t{0});
}

TEST(plan_profile_safeOnlyKeepsOnlySafe) {
    PlanOptions options;
    options.profile = SelectionProfile::SafeOnly;
    const std::vector<CleanupCandidate> list = unlockedCandidates();
    const CleanupPlan plan = buildPlan(list, options);
    // Safe выбраны (0 и 3), Review отфильтрован профилем, Risky скрыт отдельно.
    CHECK_EQ(plan.totals.selectedCount, std::size_t{2});
    CHECK_EQ(plan.totals.profileFilteredCount, std::size_t{1});
    CHECK_EQ(plan.totals.hiddenRiskyCount, std::size_t{1});
    CHECK_EQ(plan.totals.selectedBytes, kSmall + kHuge);
}

TEST(plan_profile_recommendedKeepsSafeAndReview) {
    PlanOptions options;
    options.profile = SelectionProfile::Recommended;
    const std::vector<CleanupCandidate> list = unlockedCandidates();
    const CleanupPlan plan = buildPlan(list, options);
    // Safe 90 и Safe 60 плюс Review 80 выбраны; Risky скрыт отдельно от профиля.
    CHECK_EQ(plan.totals.selectedCount, std::size_t{3});
    CHECK_EQ(plan.totals.profileFilteredCount, std::size_t{0});
    CHECK_EQ(plan.totals.hiddenRiskyCount, std::size_t{1});
    CHECK_EQ(plan.totals.selectedBytes, kSmall + kMedium + kHuge);
}

TEST(plan_riskyHiddenUntilUserAllowsIt) {
    const CleanupCandidate risky = candidate("other", SafetyLevel::Risky, 95, kRiskyVolume);
    const CleanupPlan hidden = buildPlan({risky});
    CHECK_EQ(hidden.totals.hiddenRiskyCount, std::size_t{1});
    CHECK_EQ(hidden.totals.selectedCount, std::size_t{0});
    // Скрытый Risky не выбран, но он и не заблокирован — в «всё» входит,
    // чтобы цифра «если выбрать всё» не врёт.
    CHECK_EQ(hidden.totals.allBytes, kRiskyVolume);

    PlanOptions options;
    options.allowRisky = true;
    const CleanupPlan shown = buildPlan({risky}, options);
    CHECK_EQ(shown.totals.hiddenRiskyCount, std::size_t{0});
    CHECK_EQ(shown.totals.selectedCount, std::size_t{1});
    CHECK_EQ(shown.totals.selectedBytes, kRiskyVolume);
}

TEST(plan_lockedBeatsEveryProfileAndOption) {
    PlanOptions options;
    options.profile = SelectionProfile::Everything;
    options.allowRisky = true;
    options.minReclaimBytes = 1;
    const CleanupCandidate locked = lockedCandidate("other", SafetyLevel::Risky, 100, kRiskyVolume, 1);
    const CleanupPlan plan = buildPlan({locked}, options);
    CHECK(plan.items[0].action == PlanAction::SkipLocked);
    CHECK_EQ(plan.items[0].reclaimBytes, std::uint64_t{0});
    CHECK_EQ(plan.totals.selectedCount, std::size_t{0});
    CHECK_EQ(plan.totals.allCount, std::size_t{0});
    CHECK_EQ(plan.totals.allBytes, std::uint64_t{0});
    CHECK_EQ(plan.totals.byAction.skipLockedOps.count, std::size_t{1});
    CHECK_EQ(plan.totals.hiddenRiskyCount, std::size_t{0});
    CHECK(validatePlan({locked}, plan).empty());
}

TEST(plan_trashOrDirectDeleteByVolume) {
    const std::vector<CleanupCandidate> list = {candidate("temp", SafetyLevel::Review, 90, kSmall),
                                                 candidate("logs", SafetyLevel::Review, 90, kTrashDirectDeleteBytes),
                                                 candidate("other", SafetyLevel::Review, 90, kTrashDirectDeleteBytes + 1)};
    const CleanupPlan plan = buildPlan(list);
    // Ровно порог — ещё корзина, на байт больше — прямое удаление (FR-7, ADR-006).
    CHECK(plan.items[0].action == PlanAction::Trash);
    CHECK(plan.items[1].action == PlanAction::Trash);
    CHECK(plan.items[2].action == PlanAction::Delete);
    CHECK_EQ(plan.totals.byAction.trashOps.count, std::size_t{2});
    CHECK_EQ(plan.totals.byAction.deleteOps.count, std::size_t{1});
    CHECK_EQ(plan.totals.byAction.deleteOps.bytes, kTrashDirectDeleteBytes + 1);

    PlanOptions direct;
    direct.useTrash = false;
    const CleanupPlan noTrash = buildPlan(list, direct);
    CHECK_EQ(noTrash.totals.byAction.trashOps.count, std::size_t{0});
    CHECK_EQ(noTrash.totals.byAction.deleteOps.count, std::size_t{3});
    CHECK_EQ(noTrash.totals.selectedBytes, plan.totals.selectedBytes);
}

TEST(plan_customTrashThresholdIsHonoured) {
    PlanOptions options;
    options.trashDirectDeleteAboveBytes = kMedium;
    const std::vector<CleanupCandidate> list = {candidate("temp", SafetyLevel::Review, 90, kSmall),
                                                 candidate("logs", SafetyLevel::Review, 90, kLarge)};
    const CleanupPlan plan = buildPlan(list, options);
    CHECK(plan.items[0].action == PlanAction::Trash);
    CHECK(plan.items[1].action == PlanAction::Delete);
}

TEST(plan_decideCandidateIsPureAndConsistentWithPlan) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const PlanOptions options;
    const CleanupPlan plan = buildPlan(list, options);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const SelectionDecision decision = decideCandidate(list[i], options);
        // Повторный вызов на том же кандидате обязан дать тот же ответ:
        // решение — чистая функция, UI пересчитывает её при каждом касании.
        const SelectionDecision again = decideCandidate(list[i], options);
        CHECK_EQ(decision.action, again.action);
        CHECK_EQ(decision.selected, again.selected);
        CHECK_EQ(decision.skip, again.skip);
        // И ровно это решение лежит в собранном плане.
        CHECK(plan.items[i].action == decision.action);
        CHECK_EQ(plan.items[i].reclaimBytes, decision.selected ? list[i].allocatedBytes : std::uint64_t{0});
        // SkipReason None ровно тогда, когда кандидат выбран.
        CHECK_EQ(decision.skip == SkipReason::None, decision.selected);
        CHECK(decision.selected == isRemovableAction(decision.action));
    }
}

TEST(plan_everyDecisionHasHumanReason) {
    // SPEC §12: «ни один элемент не удаляется без видимого объяснения».
    // Пустая строка причины — это элемент, о котором пользователь не знает.
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const PlanOptions risky = riskyAllowed();
    for (const CleanupCandidate& c : list) {
        CHECK(!decideCandidate(c, PlanOptions{}).reason.empty());
        CHECK(!decideCandidate(c, risky).reason.empty());
    }
    const CleanupPlan plan = buildPlan(list);
    const DryRunReport report = makeDryRunReport(list, plan);
    for (const PlanOperation& op : report.operations) {
        CHECK(!op.reason.empty());
        CHECK(op.skip == SkipReason::None);
    }
    for (const PlanOperation& op : report.untouched) {
        CHECK(!op.reason.empty());
        CHECK(op.skip != SkipReason::None);
    }
}

TEST(plan_profileNameParsing) {
    CHECK(selectionProfileFromString("safe-only") == SelectionProfile::SafeOnly);
    CHECK(selectionProfileFromString("SafeOnly") == SelectionProfile::SafeOnly);
    CHECK(selectionProfileFromString("safe") == SelectionProfile::SafeOnly);
    CHECK(selectionProfileFromString("everything") == SelectionProfile::Everything);
    CHECK(selectionProfileFromString("all") == SelectionProfile::Everything);
    // Неизвестное имя из настроек не должно молча включать «выбрать всё».
    CHECK(selectionProfileFromString("что-то ещё") == SelectionProfile::Recommended);
    CHECK(selectionProfileFromString("") == SelectionProfile::Recommended);
    CHECK(selectionProfileFromString(nullptr) == SelectionProfile::Recommended);
}

TEST(plan_profileAndSkipReasonTokens) {
    CHECK_EQ(std::string(toString(SelectionProfile::SafeOnly)), std::string("safe-only"));
    CHECK_EQ(std::string(toString(SelectionProfile::Recommended)), std::string("recommended"));
    CHECK_EQ(std::string(toString(SelectionProfile::Everything)), std::string("everything"));
    CHECK_EQ(std::string(toString(SkipReason::None)), std::string("none"));
    CHECK_EQ(std::string(toString(SkipReason::Locked)), std::string("locked"));
    CHECK_EQ(std::string(toString(SkipReason::RiskyHidden)), std::string("risky-hidden"));
    CHECK_EQ(std::string(toString(SkipReason::BelowConfidence)), std::string("below-confidence"));
    CHECK_EQ(std::string(toString(SkipReason::ProfileFiltered)), std::string("profile-filtered"));
    CHECK_EQ(std::string(toString(SkipReason::TooSmall)), std::string("too-small"));
}

TEST(plan_isRemovableActionOnlyForDeleteAndTrash) {
    CHECK(isRemovableAction(PlanAction::Delete));
    CHECK(isRemovableAction(PlanAction::Trash));
    CHECK(!isRemovableAction(PlanAction::Keep));
    CHECK(!isRemovableAction(PlanAction::SkipLocked));
}

// ------------------------------------------------- dry-run и снимок

TEST(plan_dryRunSplitsOperationsAndUntouched) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    const DryRunReport report = makeDryRunReport(list, plan);
    CHECK(report.dryRun);
    CHECK_EQ(report.operationCount, plan.operationCount());
    CHECK_EQ(report.operations.size(), std::size_t{3});
    CHECK_EQ(report.untouched.size(), std::size_t{3});
    CHECK_EQ(report.totalBytes, plan.totals.selectedBytes);
    std::uint64_t sum = 0;
    for (const PlanOperation& op : report.operations) sum += op.bytes;
    CHECK_EQ(sum, plan.totals.selectedBytes);
    for (const PlanOperation& op : report.untouched) {
        CHECK_EQ(op.bytes, std::uint64_t{0});
        CHECK(!isRemovableAction(op.action));
    }
    CHECK(report.headline.find("из 6") != std::string::npos);
}

TEST(plan_dryRunDropsItemThatContradictsCandidate) {
    // План и кандидат разошлись (движок уже пересчитал): показывать операцию,
    // которую движок выполнит иначе, нельзя — она просто исчезает из отчёта.
    const std::vector<CleanupCandidate> list = mixedCandidates();
    CleanupPlan plan = buildPlan(list);
    plan.items[1].action = PlanAction::SkipLocked;
    const DryRunReport report = makeDryRunReport(list, plan);
    CHECK_EQ(report.operations.size(), std::size_t{2});
    for (const PlanOperation& op : report.operations) CHECK(op.candidateIndex != 1);
}

TEST(plan_dryRunIgnoresOutOfRangeItemIndex) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    CleanupPlan plan = buildPlan(list);
    plan.items[0].candidateIndex = list.size();  // битый или чужой план
    const DryRunReport report = makeDryRunReport(list, plan);
    CHECK_EQ(report.operations.size(), std::size_t{2});
}

TEST(plan_dryRunIsOffWhenOptionSaysSo) {
    PlanOptions options;
    options.dryRun = false;
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list, options);
    CHECK(!plan.dryRun);
    CHECK(!makeDryRunReport(list, plan).dryRun);
}

TEST(plan_jsonIsDeterministicAndAgreesWithTotals) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    const std::string first = planToJson(list, plan);
    const std::string second = planToJson(list, buildPlan(list));
    CHECK_EQ(first, second);

    const Value doc = mrproper::json::parse(first);
    CHECK_EQ(doc.find("profile")->asString(), std::string("recommended"));
    CHECK_EQ(doc.find("options")->find("confidenceThreshold")->asNumber(),
             static_cast<double>(plan.options.confidenceThreshold));
    const Value* totals = doc.find("totals");
    CHECK(totals != nullptr);
    if (totals != nullptr) {
        CHECK_EQ(totals->find("candidates")->asNumber(), static_cast<double>(list.size()));
        CHECK_EQ(totals->find("selected")->asNumber(), static_cast<double>(plan.totals.selectedCount));
        CHECK_EQ(totals->find("selectedBytes")->asNumber(), static_cast<double>(plan.totals.selectedBytes));
        CHECK_EQ(totals->find("allBytes")->asNumber(), static_cast<double>(plan.totals.allBytes));
        CHECK_EQ(totals->find("safeOnlyBytes")->asNumber(), static_cast<double>(plan.totals.safeOnlyBytes));
        CHECK_EQ(totals->find("hiddenRisky")->asNumber(), 1.0);
        CHECK_EQ(totals->find("belowThreshold")->asNumber(), 1.0);
        CHECK_EQ(totals->find("byAction")->find("delete")->find("count")->asNumber(), 1.0);
    }
    CHECK_EQ(doc.find("operations")->items().size(), std::size_t{3});
    CHECK_EQ(doc.find("untouched")->items().size(), std::size_t{3});
    CHECK_EQ(doc.find("categories")->items().size(), std::size_t{4});
}

TEST(plan_snapshotCarriesOperationsAndSignature) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    const CleanupPlan plan = buildPlan(list);
    PlanSnapshotContext context;
    context.appVersion = "1.0.0";
    context.pid = 7;
    context.createdAtUnix = 1700000000;
    const PlanSnapshot snapshot = makeSnapshot(list, plan, context);
    CHECK_EQ(snapshot.operationCount, std::size_t{3});
    CHECK_EQ(snapshot.totalBytes, plan.totals.selectedBytes);
    CHECK_EQ(snapshot.planSignature, plan.planSignature());
    CHECK_EQ(snapshot.operations.size(), std::size_t{3});

    const std::string json = snapshotToJson(snapshot);
    CHECK_EQ(json, snapshotToJson(snapshot));
    const Value doc = mrproper::json::parse(json);
    CHECK_EQ(doc.find("appVersion")->asString(), std::string("1.0.0"));
    CHECK_EQ(doc.find("operationCount")->asNumber(), 3.0);
    CHECK_EQ(doc.find("operations")->items().size(), std::size_t{3});
    CHECK_EQ(doc.find("planSignature")->asNumber(), static_cast<double>(snapshot.planSignature));
}

TEST(plan_dryRunGateAsksAgainAfterPlanChanged) {
    const std::vector<CleanupCandidate> list = mixedCandidates();
    DryRunGate gate;
    const CleanupPlan plan = buildPlan(list);
    CHECK(gate.mustShowBeforeExecute(plan));
    gate.acknowledge(plan);
    CHECK(gate.alreadyShown(plan));
    CHECK(!gate.mustShowBeforeExecute(plan));

    // Сменился выбор — прежнего подтверждения недостаточно (FR-5).
    PlanOptions options;
    options.allowRisky = true;
    const CleanupPlan changed = buildPlan(list, options);
    CHECK(!gate.alreadyShown(changed));
    CHECK(gate.mustShowBeforeExecute(changed));

    // Новая сессия — показываем заново даже для того же плана.
    gate.beginSession();
    CHECK(!gate.alreadyShown(plan));
    CHECK(gate.mustShowBeforeExecute(plan));
}
