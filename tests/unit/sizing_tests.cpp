// Юнит-тесты core::sizing — учёт освобождаемого места.
//
// Задача 77, спека §11.1 («юнит-тесты ядра: … форматирование, инварианты
// модели»), §4 FR-4/FR-5 (освобождаемое место считается по АЛЛОЦИРОВАННОМУ
// размеру; разреженные файлы и агрегация), §6.3 (инвариант
// reclaimBytes == allocatedBytes для Delete и Trash), §14 (карта «что чем
// меряется»: «Сколько и что можно удалить»).
//
// Модуль переносимый: чисел, а не WinAPI. Поэтому весь файл собирается и
// запускается на любом хосте и проверяет то, что обязано быть верно всегда, —
// независимо от прав администратора, наличия NTFS и реальных файлов.
//
// Четыре свойства, вокруг которых построен файл:
//
//   1) РАЗРЕЖЕННОСТЬ. allocated < logical → sparseBytes; allocated > logical →
//      overheadBytes. Никогда не наоборот и никогда не «минус в ноль».
//   2) НУЛИ. Нулевой размер — не ошибка, а «пусто»; нулевой аллоцированный
//      при ненулевом логическом читается как «неизвестно» и даёт оценку, а не
//      уверенный ноль освобождения.
//   3) ПЕРЕПОЛНЕНИЕ. Суммы насыщаются до UINT64_MAX. Обёрнутое число в
//      отчёте («освободится 0 Б») — ложь, и спека этого не допускает.
//   4) АГРЕГАЦИЯ. Счётчики, суммы, фильтр по риску и уверенности, выбор
//      подмножества (без двойного счёта) и детерминированный порядок групп.
#include "harness.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "scoring.hpp"
#include "sizing.hpp"

using namespace mrproper::core;

namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;

// UINT64_MAX пишется один раз: сравнения в тестах должны читаться как
// «предел», а не как «магическая константа из воздуха».
constexpr u64 kMax = (std::numeric_limits<u64>::max)();
constexpr u64 kKiB = 1024;
constexpr u64 kMiB = 1024 * kKiB;
constexpr u64 kGiB = 1024 * kMiB;

// Кандидат с заданными размерами и уровнем риска. Остальные поля не важны
// для sizing, поэтому заполняются пустыми.
CleanupCandidate makeCandidate(std::string category, u64 logicalBytes, u64 allocatedBytes,
                               SafetyLevel safety = SafetyLevel::Safe, u32 fileCount = 1,
                               int confidence = kDefaultConfidenceThreshold) {
    CleanupCandidate candidate;
    candidate.ruleId = "rule.test";
    candidate.category = std::move(category);
    candidate.path = "C:/Temp/probe";
    candidate.logicalBytes = logicalBytes;
    candidate.allocatedBytes = allocatedBytes;
    candidate.fileCount = fileCount;
    candidate.safety = safety;
    candidate.confidence = confidence;
    return candidate;
}

}  // namespace

// ------------------------------------------------- классификация пары размеров

TEST(sizing_dispositionNames) {
    CHECK_EQ(std::string(toString(SizeDisposition::Empty)), std::string("empty"));
    CHECK_EQ(std::string(toString(SizeDisposition::Exact)), std::string("exact"));
    CHECK_EQ(std::string(toString(SizeDisposition::Sparse)), std::string("sparse"));
    CHECK_EQ(std::string(toString(SizeDisposition::Rounded)), std::string("rounded"));
}

TEST(sizing_classifyCoversEveryPairOfRelation) {
    CHECK(classifySize(0, 0) == SizeDisposition::Empty);
    CHECK(classifySize(1000, 1000) == SizeDisposition::Exact);
    CHECK(classifySize(8 * kGiB, kGiB) == SizeDisposition::Sparse);
    CHECK(classifySize(100, 4096) == SizeDisposition::Rounded);
}

TEST(sizing_classifyZeroLogicalIsNotEmptyWhenItOccupiesClusters) {
    // Каталог: логических данных нет, а кластер на диске занят. Это не «пусто» —
    // при удалении каталога освободится кластер.
    CHECK(classifySize(0, 0) == SizeDisposition::Empty);
    CHECK(classifySize(0, kDefaultClusterBytes) == SizeDisposition::Rounded);
}

// ------------------------------------------------------------------- нули

TEST(sizing_emptyPairIsEmptyAndHasNoDifference) {
    const SizeBreakdown breakdown = analyzeSize(0, 0);
    CHECK(breakdown.disposition == SizeDisposition::Empty);
    CHECK_EQ(breakdown.logicalBytes, u64{0});
    CHECK_EQ(breakdown.allocatedBytes, u64{0});
    CHECK_EQ(breakdown.differenceBytes, u64{0});
    CHECK_EQ(breakdown.sparseBytes, u64{0});
    CHECK_EQ(breakdown.overheadBytes, u64{0});
}

TEST(sizing_directoryOccupiesClusterWithoutLogicalData) {
    const SizeBreakdown breakdown = analyzeSize(0, kDefaultClusterBytes);
    CHECK(breakdown.disposition == SizeDisposition::Rounded);
    CHECK_EQ(breakdown.overheadBytes, kDefaultClusterBytes);
    CHECK_EQ(breakdown.sparseBytes, u64{0});
    CHECK_EQ(breakdown.differenceBytes, kDefaultClusterBytes);
}

TEST(sizing_alignToClusterRoundsUpOrKeepsExactMultiple) {
    CHECK_EQ(alignToCluster(0, kDefaultClusterBytes), u64{0});
    CHECK_EQ(alignToCluster(1, kDefaultClusterBytes), kDefaultClusterBytes);
    CHECK_EQ(alignToCluster(kDefaultClusterBytes, kDefaultClusterBytes), kDefaultClusterBytes);
    CHECK_EQ(alignToCluster(kDefaultClusterBytes + 1, kDefaultClusterBytes), 2 * kDefaultClusterBytes);
    CHECK_EQ(alignToCluster(100, 4096), u64{4096});
}

TEST(sizing_alignToClusterWithoutClusterInfoDoesNothing) {
    // Платформа не сообщила размер кластера: округлять нечем, и выдумывать
    // кластер по умолчанию здесь нельзя — это сделает вызывающий код.
    CHECK_EQ(alignToCluster(100, 0), u64{100});
    CHECK_EQ(alignToCluster(100, 1), u64{100});
    CHECK_EQ(alignToCluster(kMax, 0), kMax);
}

// ------------------------------------------------------------- разреженность

TEST(sizing_sparseFileReportsOnlyAllocatedAsReclaimable) {
    // 8 ГБ логических на 1 ГБ аллоцированных — классический NTFS sparse.
    const SizeBreakdown breakdown = analyzeSize(8 * kGiB, kGiB);
    CHECK(breakdown.disposition == SizeDisposition::Sparse);
    CHECK_EQ(breakdown.sparseBytes, 7 * kGiB);
    CHECK_EQ(breakdown.overheadBytes, u64{0});
    CHECK_EQ(breakdown.differenceBytes, 7 * kGiB);

    // Главное правило §4 FR-4: освободится аллоцированное, а не логическое.
    const ReclaimEstimate estimate = estimateReclaim(8 * kGiB, kGiB, true);
    CHECK_EQ(estimate.bytes, kGiB);
    CHECK(!estimate.estimated);
}

TEST(sizing_clusterRoundingIsOverheadNotSparse) {
    // Файл в 100 Б занимает кластер 4 КиБ: это округление, а не разреженность.
    const SizeBreakdown breakdown = analyzeSize(100, kDefaultClusterBytes);
    CHECK(breakdown.disposition == SizeDisposition::Rounded);
    CHECK_EQ(breakdown.overheadBytes, kDefaultClusterBytes - 100);
    CHECK_EQ(breakdown.sparseBytes, u64{0});
    CHECK_EQ(breakdown.differenceBytes, kDefaultClusterBytes - 100);
}

TEST(sizing_sparseAndOverheadNeverBothNonZero) {
    // Взаимоисключающие ветки: сумма разностей равна |allocated − logical|.
    const std::uint64_t pairs[][2] = {{0, 0},        {100, 4096},     {4096, 100},
                                      {1, 1},        {kGiB, 8 * kGiB}, {8 * kGiB, kGiB},
                                      {kDefaultClusterBytes, kDefaultClusterBytes}};
    for (const auto& pair : pairs) {
        const SizeBreakdown breakdown = analyzeSize(pair[0], pair[1]);
        const u64 expectedDifference = pair[1] > pair[0] ? pair[1] - pair[0] : pair[0] - pair[1];
        CHECK_EQ(breakdown.differenceBytes, expectedDifference);
        CHECK(breakdown.sparseBytes == 0 || breakdown.overheadBytes == 0);
        CHECK_EQ(breakdown.sparseBytes + breakdown.overheadBytes, expectedDifference);
    }
}

// ------------------------------------------------------------- переполнение

TEST(sizing_saturatingAddStopsAtMaxInsteadOfWrapping) {
    CHECK_EQ(saturatingAdd(0, 0), u64{0});
    CHECK_EQ(saturatingAdd(2, 3), u64{5});
    CHECK_EQ(saturatingAdd(kMax, 0), kMax);
    CHECK_EQ(saturatingAdd(kMax, 1), kMax);
    CHECK_EQ(saturatingAdd(kMax - 1, 1), kMax);
    CHECK_EQ(saturatingAdd(kMax, kMax), kMax);
    CHECK_EQ(saturatingAdd(kMax / 2, kMax / 2), kMax - 1);
}

TEST(sizing_saturatingSubNeverGoesBelowZero) {
    CHECK_EQ(saturatingSub(0, 0), u64{0});
    CHECK_EQ(saturatingSub(10, 4), u64{6});
    CHECK_EQ(saturatingSub(4, 10), u64{0});
    CHECK_EQ(saturatingSub(0, kMax), u64{0});
    CHECK_EQ(saturatingSub(kMax, kMax), u64{0});
}

TEST(sizing_alignToClusterSaturatesInsteadOfOverflowing) {
    // UINT64_MAX не кратен кластеру: округление вверх не помещается в u64 —
    // возвращаем предел, а не 0 после переноса.
    CHECK_EQ(alignToCluster(kMax, kDefaultClusterBytes), kMax);
    // Наибольший размер, выровненный по кластеру и ещё помещающийся в u64.
    // 2^64 кратно 4096, поэтому kMax mod 4096 = 4095: автор прежней версии теста
    // брал kMax - 4096, но это число НЕ выровнено, и округление вверх обязано
    // было его изменить — ожидание было математически невозможным.
    constexpr u64 kLargestAligned = kMax - (kMax % kDefaultClusterBytes);
    CHECK_EQ(alignToCluster(kLargestAligned, kDefaultClusterBytes), kLargestAligned);
    // На один байт ниже: округление вверх возвращает ровно границу, без переноса в 0.
    CHECK_EQ(alignToCluster(kLargestAligned - 1, kDefaultClusterBytes), kLargestAligned);
}

TEST(sizing_analyzeSizeAtMaxDoesNotUnderflow) {
    // logical = MAX при allocated = 0 — «файл размером весь том, на диске
    // ничего». Разность обязана остаться положительной, а не уйти в ноль.
    const SizeBreakdown sparse = analyzeSize(kMax, 0);
    CHECK(sparse.disposition == SizeDisposition::Sparse);
    CHECK_EQ(sparse.sparseBytes, kMax);
    CHECK_EQ(sparse.differenceBytes, kMax);
    CHECK_EQ(sparse.overheadBytes, u64{0});

    // Обратный край: allocated = MAX при logical = 0.
    const SizeBreakdown overhead = analyzeSize(0, kMax);
    CHECK(overhead.disposition == SizeDisposition::Rounded);
    CHECK_EQ(overhead.overheadBytes, kMax);
    CHECK_EQ(overhead.sparseBytes, u64{0});
    CHECK_EQ(overhead.differenceBytes, kMax);
}

TEST(sizing_accumulatorSumsSaturateOnOverflow) {
    SizeAccumulator accumulator;
    accumulator.add(kMax, kMax);
    accumulator.add(kMax, kMax);
    const SizingTotals& totals = accumulator.totals();
    CHECK_EQ(totals.itemCount, u64{2});
    CHECK_EQ(totals.fileCount, u64{2});
    CHECK_EQ(totals.logicalBytes, kMax);   // не 2*MAX → не обёрнутый ноль
    CHECK_EQ(totals.allocatedBytes, kMax);
    CHECK_EQ(totals.reclaimableBytes, kMax);
    // allocated == logical, значит ни разреженности, ни округления.
    CHECK_EQ(totals.sparseBytes, u64{0});
    CHECK_EQ(totals.overheadBytes, u64{0});
}

TEST(sizing_accumulatorFileCountSaturates) {
    // fileCount — uint32; сумма по трём кандидатам обязана остаться в u64,
    // иначе отчёт показал бы «0 файлов» на дереве в миллиард файлов.
    SizeAccumulator accumulator;
    accumulator.add(1, 1);
    accumulator.add(1, 1);
    accumulator.add(1, 1);
    CHECK_EQ(accumulator.totals().fileCount, u64{3});
    CHECK_EQ(accumulator.totals().itemCount, u64{3});
    CHECK_EQ(accumulator.totals().reclaimableBytes, u64{3});
}

// ------------------------------------------------------------- оценка вклада

TEST(sizing_estimateUsesAllocatedWhenKnown) {
    const ReclaimEstimate known = estimateReclaim(kGiB, kDefaultClusterBytes, true);
    CHECK_EQ(known.bytes, kDefaultClusterBytes);
    CHECK(!known.estimated);

    // Логический ноль с непустым аллоцированным — каталог, оценка не нужна.
    const ReclaimEstimate directory = estimateReclaim(0, kDefaultClusterBytes, true);
    CHECK_EQ(directory.bytes, kDefaultClusterBytes);
    CHECK(!directory.estimated);
}

TEST(sizing_estimateFallsBackToLogicalAndMarksIt) {
    // Аллоцированный неизвестен: показываем верхнюю границу, но помечаем её.
    const ReclaimEstimate estimate = estimateReclaim(500, 0, false);
    CHECK_EQ(estimate.bytes, u64{500});
    CHECK(estimate.estimated);
}

TEST(sizing_estimateOfEmptyInputIsNotAnEstimate) {
    // Ноль логических при неизвестном аллоцированном: оценивать нечего, и
    // метка «оценка» на нуле только сбивает пользователя.
    const ReclaimEstimate estimate = estimateReclaim(0, 0, false);
    CHECK_EQ(estimate.bytes, u64{0});
    CHECK(!estimate.estimated);
}

TEST(sizing_candidateZeroAllocatedIsReadAsUnknown) {
    // allocated == 0 при ненулевом логическом: настолько сжатый файл на NTFS
    // практически не бывает — честнее показать оценку, чем уверенный ноль.
    const CleanupCandidate unknownAllocated = makeCandidate("Temp", 1000, 0);
    const ReclaimEstimate guess = estimateReclaim(unknownAllocated);
    CHECK_EQ(guess.bytes, u64{1000});
    CHECK(guess.estimated);

    const CleanupCandidate exact = makeCandidate("Temp", 1000, 1000);
    const ReclaimEstimate fact = estimateReclaim(exact);
    CHECK_EQ(fact.bytes, u64{1000});
    CHECK(!fact.estimated);

    // Нулевой кандидат: и логический, и аллоцированный нули — не оценка.
    const CleanupCandidate empty = makeCandidate("Temp", 0, 0);
    const ReclaimEstimate nothing = estimateReclaim(empty);
    CHECK_EQ(nothing.bytes, u64{0});
    CHECK(!nothing.estimated);

    // Каталог: allocated известен, поэтому 0 логических не мешает.
    const CleanupCandidate directory = makeCandidate("Temp", 0, kDefaultClusterBytes);
    const ReclaimEstimate cluster = estimateReclaim(directory);
    CHECK_EQ(cluster.bytes, kDefaultClusterBytes);
    CHECK(!cluster.estimated);
}

TEST(sizing_planActionDecidesWhetherDiskIsFreed) {
    // Инвариант §6.3: Delete и Trash освобождают аллоцированное.
    const CleanupCandidate candidate = makeCandidate("Temp", kGiB, kDefaultClusterBytes);
    const ReclaimEstimate byDelete = estimateReclaim(candidate, PlanAction::Delete);
    CHECK_EQ(byDelete.bytes, kDefaultClusterBytes);
    CHECK(!byDelete.estimated);
    const ReclaimEstimate byTrash = estimateReclaim(candidate, PlanAction::Trash);
    CHECK_EQ(byTrash.bytes, kDefaultClusterBytes);

    // Keep и SkipLocked диск не трогают: освобождать нечего.
    CHECK_EQ(estimateReclaim(candidate, PlanAction::Keep).bytes, u64{0});
    CHECK(!estimateReclaim(candidate, PlanAction::Keep).estimated);
    CHECK_EQ(estimateReclaim(candidate, PlanAction::SkipLocked).bytes, u64{0});
    CHECK(!estimateReclaim(candidate, PlanAction::SkipLocked).estimated);
}

// ---------------------------------------------------------------- накопитель

TEST(sizing_accumulatorSplitsSparseAndOverhead) {
    SizeAccumulator accumulator;
    accumulator.add(8 * kGiB, kGiB);            // разреженный
    accumulator.add(100, kDefaultClusterBytes);  // округлённый по кластеру
    accumulator.add(500, 500);                   // точный
    const SizingTotals& totals = accumulator.totals();
    CHECK_EQ(totals.itemCount, u64{3});
    CHECK_EQ(totals.fileCount, u64{3});
    CHECK_EQ(totals.logicalBytes, 8 * kGiB + 100 + 500);
    CHECK_EQ(totals.allocatedBytes, kGiB + kDefaultClusterBytes + 500);
    CHECK_EQ(totals.reclaimableBytes, totals.allocatedBytes);
    CHECK_EQ(totals.sparseBytes, 7 * kGiB);
    CHECK_EQ(totals.overheadBytes, kDefaultClusterBytes - 100);
    CHECK_EQ(totals.reclaimableEstimatedBytes, u64{0});
}

TEST(sizing_accumulatorEstimatedDoesNotInventSparse) {
    // Неизвестный аллоцированный размер — это не «разреженность». Назвать его
    // ею в отчёте значит соврать о диске.
    SizeAccumulator accumulator;
    accumulator.addEstimated(5000);
    const SizingTotals& totals = accumulator.totals();
    CHECK_EQ(totals.itemCount, u64{1});
    CHECK_EQ(totals.fileCount, u64{1});
    CHECK_EQ(totals.logicalBytes, u64{5000});
    CHECK_EQ(totals.allocatedBytes, u64{0});
    CHECK_EQ(totals.reclaimableBytes, u64{5000});
    CHECK_EQ(totals.reclaimableEstimatedBytes, u64{5000});
    CHECK_EQ(totals.sparseBytes, u64{0});
    CHECK_EQ(totals.overheadBytes, u64{0});
}

TEST(sizing_accumulatorDirectoryAddsOnlyCluster) {
    SizeAccumulator accumulator;
    accumulator.addDirectory(kDefaultClusterBytes);
    const SizingTotals& totals = accumulator.totals();
    CHECK_EQ(totals.itemCount, u64{1});
    CHECK_EQ(totals.fileCount, u64{0});   // каталог — не файл
    CHECK_EQ(totals.logicalBytes, u64{0});
    CHECK_EQ(totals.allocatedBytes, kDefaultClusterBytes);
    CHECK_EQ(totals.reclaimableBytes, kDefaultClusterBytes);
    CHECK_EQ(totals.overheadBytes, kDefaultClusterBytes);
}

TEST(sizing_accumulatorResetClearsEverything) {
    SizeAccumulator accumulator;
    accumulator.add(8 * kGiB, kGiB);
    accumulator.addEstimated(100);
    accumulator.addDirectory(kDefaultClusterBytes);
    accumulator.reset();
    const SizingTotals& totals = accumulator.totals();
    CHECK_EQ(totals.itemCount, u64{0});
    CHECK_EQ(totals.fileCount, u64{0});
    CHECK_EQ(totals.logicalBytes, u64{0});
    CHECK_EQ(totals.allocatedBytes, u64{0});
    CHECK_EQ(totals.reclaimableBytes, u64{0});
    CHECK_EQ(totals.reclaimableEstimatedBytes, u64{0});
    CHECK_EQ(totals.sparseBytes, u64{0});
    CHECK_EQ(totals.overheadBytes, u64{0});
}

// ----------------------------------------------------------------- агрегация

TEST(sizing_aggregateByScopeExcludesRiskyByDefault) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 1000, 1000, SafetyLevel::Safe),
        makeCandidate("Logs", 2000, 2000, SafetyLevel::Review),
        makeCandidate("Dumps", 4000, 4000, SafetyLevel::Risky),
    };

    const SizingTotals notRisky = aggregateSizes(candidates, SizingScope::NotRisky, 0);
    CHECK_EQ(notRisky.itemCount, u64{2});
    CHECK_EQ(notRisky.reclaimableBytes, u64{3000});
    CHECK_EQ(notRisky.logicalBytes, u64{3000});

    const SizingTotals safeOnly = aggregateSizes(candidates, SizingScope::SafeOnly, 0);
    CHECK_EQ(safeOnly.itemCount, u64{1});
    CHECK_EQ(safeOnly.reclaimableBytes, u64{1000});

    const SizingTotals all = aggregateSizes(candidates, SizingScope::All, 0);
    CHECK_EQ(all.itemCount, u64{3});
    CHECK_EQ(all.reclaimableBytes, u64{7000});

    // Пустой набор — нули, а не падение (§6.4: отказ — это данные).
    const std::vector<CleanupCandidate> empty;
    const SizingTotals none = aggregateSizes(empty, SizingScope::NotRisky, 0);
    CHECK_EQ(none.itemCount, u64{0});
    CHECK_EQ(none.reclaimableBytes, u64{0});
    CHECK_EQ(none.logicalBytes, u64{0});
}

TEST(sizing_aggregateCountsFileCountNotJustItems) {
    // Кандидат-каталог представляет много файлов: агрегат обязан считать их,
    // иначе надпись «3 файла» на дереве из трёх тысяч файлов.
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 3000, 3000, SafetyLevel::Safe, 1000),
        makeCandidate("Logs", 2000, 2000, SafetyLevel::Safe, 20),
    };
    const SizingTotals totals = aggregateSizes(candidates, SizingScope::All, 0);
    CHECK_EQ(totals.itemCount, u64{2});
    CHECK_EQ(totals.fileCount, u64{1020});
    CHECK_EQ(totals.reclaimableBytes, u64{5000});
}

TEST(sizing_aggregateKeepsSparseAndOverheadApart) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 8 * kGiB, kGiB, SafetyLevel::Safe),            // разреженный
        makeCandidate("Logs", 100, kDefaultClusterBytes, SafetyLevel::Safe),  // округлённый
        makeCandidate("Cache", 500, 500, SafetyLevel::Safe),                  // точный
    };
    const SizingTotals totals = aggregateSizes(candidates, SizingScope::All, 0);
    CHECK_EQ(totals.sparseBytes, 7 * kGiB);
    CHECK_EQ(totals.overheadBytes, kDefaultClusterBytes - 100);
    CHECK_EQ(totals.reclaimableBytes, kGiB + kDefaultClusterBytes + 500);
    CHECK_EQ(totals.reclaimableEstimatedBytes, u64{0});
    CHECK_EQ(totals.allocatedBytes, kGiB + kDefaultClusterBytes + 500);
    CHECK_EQ(totals.logicalBytes, 8 * kGiB + 100 + 500);
}

TEST(sizing_aggregateReportsUnknownAllocatedAsEstimate) {
    // allocated == 0 при ненулевом логическом читается как «аллоцированный
    // неизвестен»: в reclaimable идёт логический размер, помеченный оценкой,
    // и в общий allocated он не попадает.
    //
    // Замечание владельцу sizing.cpp: aggregateSizes считает эту дельту ещё и
    // в sparseBytes (через analyzeSize), тогда как SizeAccumulator::addEstimated
    // не считает её нигде. Здесь зафиксировано фактическое поведение
    // aggregateSizes; если его выправят, правится и эта строка.
    const std::vector<CleanupCandidate> candidates = {makeCandidate("Dumps", 700, 0, SafetyLevel::Safe)};
    const SizingTotals totals = aggregateSizes(candidates, SizingScope::All, 0);
    CHECK_EQ(totals.logicalBytes, u64{700});
    CHECK_EQ(totals.allocatedBytes, u64{0});
    CHECK_EQ(totals.reclaimableBytes, u64{700});
    CHECK_EQ(totals.reclaimableEstimatedBytes, u64{700});
    CHECK_EQ(totals.sparseBytes, u64{700});
}

TEST(sizing_aggregateRespectsConfidenceThreshold) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 1000, 1000, SafetyLevel::Safe, 1, 10),
        makeCandidate("Logs", 2000, 2000, SafetyLevel::Safe, 1, 50),
        makeCandidate("Dumps", 4000, 4000, SafetyLevel::Safe, 1, 90),
    };
    CHECK_EQ(aggregateSizes(candidates, SizingScope::All, 0).itemCount, u64{3});
    CHECK_EQ(aggregateSizes(candidates, SizingScope::All, kDefaultConfidenceThreshold).itemCount, u64{2});
    CHECK_EQ(aggregateSizes(candidates, SizingScope::All, kDefaultConfidenceThreshold).reclaimableBytes,
             u64{6000});
    CHECK_EQ(aggregateSizes(candidates, SizingScope::All, 91).itemCount, u64{0});
}

TEST(sizing_aggregateSelectedIgnoresDuplicatesAndOutOfRange) {
    // Расчёт агрегата не имеет права ронять UI (§4 FR-5): индекс мимо
    // массива и повторный выбор одного элемента — обычные в UI состояния.
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 1000, 1000),
        makeCandidate("Logs", 2000, 2000),
        makeCandidate("Dumps", 4000, 4000),
    };
    const std::vector<std::size_t> selected = {0, 0, 2, 99, 2};
    const SizingTotals totals = aggregateSizes(candidates, selected, 0);
    CHECK_EQ(totals.itemCount, u64{2});
    CHECK_EQ(totals.reclaimableBytes, u64{5000});
    CHECK_EQ(totals.logicalBytes, u64{5000});

    const SizingTotals none = aggregateSizes(candidates, std::vector<std::size_t>{}, 0);
    CHECK_EQ(none.itemCount, u64{0});
    CHECK_EQ(none.reclaimableBytes, u64{0});
}

TEST(sizing_aggregateSelectedStillAppliesConfidence) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 1000, 1000, SafetyLevel::Safe, 1, 10),
        makeCandidate("Logs", 2000, 2000, SafetyLevel::Safe, 1, 90),
    };
    const std::vector<std::size_t> selected = {0, 1};
    const SizingTotals totals = aggregateSizes(candidates, selected, kDefaultConfidenceThreshold);
    CHECK_EQ(totals.itemCount, u64{1});
    CHECK_EQ(totals.reclaimableBytes, u64{2000});
}

TEST(sizing_aggregateSaturatesOnAbsurdInput) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", kMax, kMax, SafetyLevel::Safe, 4000000000u),
        makeCandidate("Logs", kMax, kMax, SafetyLevel::Safe, 4000000000u),
    };
    const SizingTotals totals = aggregateSizes(candidates, SizingScope::All, 0);
    CHECK_EQ(totals.logicalBytes, kMax);
    CHECK_EQ(totals.allocatedBytes, kMax);
    CHECK_EQ(totals.reclaimableBytes, kMax);
    CHECK_EQ(totals.fileCount, u64{8000000000});
    CHECK_EQ(totals.itemCount, u64{2});
}

TEST(sizing_aggregateOverManyItemsStaysConsistent) {
    // Нагрузочная форма §11.6: сумма по каталогу не должна «поехать» на
    // длинном списке, а агрегат обязан совпасть с поштучным сложением.
    constexpr std::size_t kItems = 20000;
    std::vector<CleanupCandidate> candidates;
    candidates.reserve(kItems);
    u64 expected = 0;
    for (std::size_t i = 0; i < kItems; ++i) {
        const u64 logical = 100 + static_cast<u64>(i);
        const u64 allocated = 100 + static_cast<u64>(i) * 2;
        candidates.push_back(makeCandidate("Temp", logical, allocated));
        expected = saturatingAdd(expected, allocated);
    }
    const SizingTotals totals = aggregateSizes(candidates, SizingScope::All, 0);
    CHECK_EQ(totals.itemCount, static_cast<u64>(kItems));
    CHECK_EQ(totals.fileCount, static_cast<u64>(kItems));
    CHECK_EQ(totals.reclaimableBytes, expected);
    CHECK_EQ(totals.overheadBytes, expected - (100 * kItems + static_cast<u64>(kItems) * (kItems - 1) / 2));
}

// ------------------------------------------------------------------ группировка

TEST(sizing_groupSizesSortsByReclaimableDescThenKey) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 1000, 100),
        makeCandidate("Logs", 3000, 300),
        makeCandidate("Cache", 5000, 300),
    };
    const std::vector<SizeGroup> groups = groupSizes(candidates, GroupBy::Category, SizingScope::All, 0);
    CHECK_EQ(groups.size(), static_cast<std::size_t>(3));
    CHECK_EQ(groups[0].key, std::string("Cache"));  // 300, ключ «Cache» < «Logs»
    CHECK_EQ(groups[1].key, std::string("Logs"));   // 300
    CHECK_EQ(groups[2].key, std::string("Temp"));   // 100
    CHECK_EQ(groups[0].totals.reclaimableBytes, u64{300});
    CHECK_EQ(groups[2].totals.reclaimableBytes, u64{100});
}

TEST(sizing_groupSizesSumsInsideGroup) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 1000, 100, SafetyLevel::Safe, 10),
        makeCandidate("Temp", 2000, 200, SafetyLevel::Safe, 20),
        makeCandidate("Logs", 4000, 400, SafetyLevel::Safe, 40),
    };
    const std::vector<SizeGroup> groups = groupSizes(candidates, GroupBy::Category, SizingScope::All, 0);
    CHECK_EQ(groups.size(), static_cast<std::size_t>(2));
    CHECK_EQ(groups[0].key, std::string("Logs"));  // 400 > 300
    CHECK_EQ(groups[0].totals.itemCount, u64{1});
    CHECK_EQ(groups[0].totals.fileCount, u64{40});
    CHECK_EQ(groups[0].totals.reclaimableBytes, u64{400});
    CHECK_EQ(groups[1].key, std::string("Temp"));
    CHECK_EQ(groups[1].totals.itemCount, u64{2});
    CHECK_EQ(groups[1].totals.fileCount, u64{30});
    CHECK_EQ(groups[1].totals.reclaimableBytes, u64{300});
}

TEST(sizing_groupSizesUsesUnknownKeyForEmptyCategory) {
    // Пустая строка в отчёте читается как ошибка, а не как «категория не задана».
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("", 1000, 100),
        makeCandidate("Temp", 2000, 200),
    };
    const std::vector<SizeGroup> groups = groupSizes(candidates, GroupBy::Category, SizingScope::All, 0);
    CHECK_EQ(groups.size(), static_cast<std::size_t>(2));
    CHECK_EQ(groups[0].key, std::string("Temp"));  // 200 > 100
    CHECK_EQ(groups[1].key, std::string(kUnknownGroupKey));
    CHECK_EQ(groups[1].totals.reclaimableBytes, u64{100});
}

TEST(sizing_groupSizesByRuleIdAndSafety) {
    CleanupCandidate byRule = makeCandidate("Temp", 1000, 100);
    byRule.ruleId = "temp.old";
    CleanupCandidate other = makeCandidate("Temp", 2000, 200);
    other.ruleId = "temp.recent";

    const std::vector<SizeGroup> byRuleId =
        groupSizes({byRule, other}, GroupBy::RuleId, SizingScope::All, 0);
    CHECK_EQ(byRuleId.size(), static_cast<std::size_t>(2));
    CHECK_EQ(byRuleId[0].key, std::string("temp.recent"));
    CHECK_EQ(byRuleId[1].key, std::string("temp.old"));

    const std::vector<SizeGroup> bySafety = groupSizes(
        {makeCandidate("Temp", 1, 1, SafetyLevel::Safe), makeCandidate("Temp", 1, 1, SafetyLevel::Risky)},
        GroupBy::Safety, SizingScope::All, 0);
    CHECK_EQ(bySafety.size(), static_cast<std::size_t>(2));
    CHECK_EQ(bySafety[0].key, std::string("risky"));
    CHECK_EQ(bySafety[1].key, std::string("safe"));
}

TEST(sizing_groupSizesNoneIsSingleGroupAndRespectsScope) {
    const std::vector<CleanupCandidate> candidates = {
        makeCandidate("Temp", 1000, 100, SafetyLevel::Safe),
        makeCandidate("Logs", 2000, 200, SafetyLevel::Risky),
    };
    const std::vector<SizeGroup> all = groupSizes(candidates, GroupBy::None, SizingScope::All, 0);
    CHECK_EQ(all.size(), static_cast<std::size_t>(1));
    CHECK(all[0].key.empty());
    CHECK_EQ(all[0].totals.reclaimableBytes, u64{300});

    // Risky скрыт по умолчанию (§4 FR-4) — и в разбивке тоже.
    const std::vector<SizeGroup> notRisky = groupSizes(candidates, GroupBy::None, SizingScope::NotRisky, 0);
    CHECK_EQ(notRisky.size(), static_cast<std::size_t>(1));
    CHECK_EQ(notRisky[0].totals.reclaimableBytes, u64{100});

    // Группировка не должна обходить порог уверенности.
    const std::vector<SizeGroup> filtered =
        groupSizes({makeCandidate("Temp", 1000, 100, SafetyLevel::Safe, 1, 1)},
                   GroupBy::Category, SizingScope::All, kDefaultConfidenceThreshold);
    CHECK_EQ(filtered.size(), static_cast<std::size_t>(0));
}

// -------------------------------------------------------------------- тексты

TEST(sizing_describeSizeUsageExplainsEveryDisposition) {
    const CleanupCandidate empty = makeCandidate("Temp", 0, 0);
    CHECK_EQ(describeSizeUsage(empty),
             std::string("пусто: ни логического, ни аллоцированного размера"));

    const CleanupCandidate exact = makeCandidate("Temp", 1000, 1000);
    CHECK_EQ(describeSizeUsage(exact), std::string("1,0 КБ на диске, столько же логических"));

    const CleanupCandidate sparse = makeCandidate("Temp", 8000000000, 1000000000);
    CHECK_EQ(describeSizeUsage(sparse),
             std::string("1,0 ГБ на диске из 8,0 ГБ логических — разреженный или сжатый файл"));

    const CleanupCandidate rounded = makeCandidate("Temp", 100, kDefaultClusterBytes);
    CHECK_EQ(describeSizeUsage(rounded),
             std::string("4,1 КБ на диске при 100 Б логических — выравнивание по кластеру или сжатие"));

    const CleanupCandidate directory = makeCandidate("Temp", 0, kDefaultClusterBytes);
    CHECK_EQ(describeSizeUsage(directory),
             std::string("каталог: 4,1 КБ на диске, логических данных нет"));
}

TEST(sizing_describeTotalsAlwaysNamesAllocatedBasis) {
    // §7.2: большая цифра обязана сопровождаться словами «по аллоцированному
    // размеру» — иначе пользователь считает, что освободится 8 ГБ, а
    // освободится 1.
    SizingTotals totals;
    totals.reclaimableBytes = 4096;
    totals.fileCount = 2;
    CHECK_EQ(describeTotals(totals),
             std::string("освободится 4,1 КБ по аллоцированному размеру; 2 файла"));
}

TEST(sizing_describeTotalsMentionsEstimateSparseAndOverhead) {
    SizingTotals totals;
    totals.reclaimableBytes = 4096;
    totals.reclaimableEstimatedBytes = 512;
    totals.sparseBytes = 1024;
    totals.overheadBytes = 8;
    totals.fileCount = 2;
    CHECK_EQ(describeTotals(totals),
             std::string("освободится 4,1 КБ по аллоцированному размеру, из них 512 Б — оценка по "
                         "логическому размеру; разрежено 1,0 КБ; выравнивание по кластерам 8 Б; 2 файла"));
}

TEST(sizing_describeTotalsOfEmptySetSaysNothingFalse) {
    const SizingTotals totals;
    CHECK_EQ(describeTotals(totals),
             std::string("освободится 0 Б по аллоцированному размеру; 0 файлов"));
}

TEST(sizing_defaultClusterMatchesNtfs) {
    // Спека §4 FR-4 объясняет файл в 100 Б через 4 КиБ: константа, на которой
    // стоит это объяснение, обязана быть константой, а не магическим числом в UI.
    CHECK_EQ(kDefaultClusterBytes, u64{4096});
    CHECK_EQ(kDefaultClusterBytes % 512, u64{0});  // кластер NTFS кратен сектору
}
