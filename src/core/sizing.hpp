// Учёт освобождаемого места: аллоцированный против логического размера,
// разреженные (sparse) файлы и агрегация reclaimable (SPEC §4 FR-4, §4 FR-5, §6.3).
//
// Разделение слоёв: числа для одного файла (логический и аллоцированный размер)
// добывает platform::vfs на Windows, а считать по ним — переносимая логика без
// windows.h, которую тестируют на любом хосте (SPEC §6.1, ADR-004).
//
// Главное правило спеки (§4 FR-4): освобождаемое место считается по АЛЛОЦИРОВАННОМУ
// размеру — только он меняет свободное место тома. Логический размер нужен рядом,
// для объяснения, но в оценку освобождения не входит: у разреженного файла
// «8 ГБ» на диске может лежать 1 ГБ, и обе цифры при этом верны.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "model.hpp"

namespace mrproper::core {

// Размер кластера по умолчанию (NTFS — 4 КиБ). Используется только там, где
// платформа не сообщила размер кластера тома: без него нельзя объяснить, почему
// файл в 100 Б занимает 4 КиБ.
inline constexpr std::uint64_t kDefaultClusterBytes = 4096;

// Ключ группы, когда поле у кандидата не заполнено: пустая строка в отчёте
// выглядит как ошибка, а не как «категория не задана».
inline constexpr const char* kUnknownGroupKey = "(unknown)";

// Как соотносятся логический и аллоцированный размер.
enum class SizeDisposition {
    Empty,   // ничего не занимает: пустой файл или каталог без файлов
    Exact,   // аллоцированный равен логическому
    Sparse,  // аллоцированный меньше логического: разреженность или сжатие
    Rounded, // аллоцированный больше логического: выравнивание по кластеру или сжатие
};

const char* toString(SizeDisposition disposition) noexcept;

// Разбор пары размеров с готовыми числами для интерфейса и отчёта.
struct SizeBreakdown {
    SizeDisposition disposition{SizeDisposition::Exact};
    std::uint64_t logicalBytes{};
    std::uint64_t allocatedBytes{};
    // |allocated − logical|: насколько оценка расходится с тем, что на диске.
    std::uint64_t differenceBytes{};
    // Логический размер, не выделенный на диске (0 при Exact и Rounded).
    std::uint64_t sparseBytes{};
    // Занято сверх логического размера — округление по кластеру (0 при Exact и Sparse).
    std::uint64_t overheadBytes{};
};

SizeDisposition classifySize(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept;
SizeBreakdown analyzeSize(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept;

// Округление вверх до целого кластера. clusterBytes <= 1 — округления нет.
// Результат насыщается: ошибки обхода не превращаются в 0 после переполнения.
std::uint64_t alignToCluster(std::uint64_t bytes, std::uint64_t clusterBytes) noexcept;

// Насыщающая арифметика размеров. Сумма по дереву каталогов переполнить uint64
// не должна: «освободится 18,4 эксабайта» — ложь в отчёте, поэтому предел
// UINT64_MAX, а не обёрнутое число.
std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) noexcept;
std::uint64_t saturatingSub(std::uint64_t a, std::uint64_t b) noexcept;

// Сколько освободит один элемент.
struct ReclaimEstimate {
    std::uint64_t bytes{};
    // true — аллоцированный размер неизвестен, оценка взята по логическому.
    // Такой вклад показываем отдельно, чтобы не выдавать верхнюю границу за факт.
    bool estimated{false};
};

// allocatedKnown=false означает, что платформа не отдала аллоцированный размер
// (обход не смог прочитать атрибут): тогда считаем по логическому и помечаем оценку.
ReclaimEstimate estimateReclaim(std::uint64_t logicalBytes, std::uint64_t allocatedBytes,
                                bool allocatedKnown = true) noexcept;

// По кандидату. allocatedBytes == 0 при ненулевом логическом читается как
// «аллоцированный размер неизвестен»: полностью сжатый файл на NTFS настолько
// редок, что честнее показать оценку, чем уверенно показать ноль.
ReclaimEstimate estimateReclaim(const CleanupCandidate& candidate) noexcept;

// С учётом действия плана: Keep и SkipLocked не освобождают ничего
// (инвариант §6.3: reclaimBytes == allocatedBytes для Delete и Trash).
ReclaimEstimate estimateReclaim(const CleanupCandidate& candidate, PlanAction action) noexcept;

// Агрегаты по набору кандидатов. Суммы насыщающие, счётчики — тоже: дерево из
// 500 тыс. файлов не должно переполнить счётчик и показать «0 файлов».
struct SizingTotals {
    std::uint64_t itemCount{};                 // сколько элементов учтено
    std::uint64_t fileCount{};                 // сколько файлов учтено
    std::uint64_t logicalBytes{};              // сумма логических размеров
    std::uint64_t allocatedBytes{};            // сумма аллоцированных
    std::uint64_t reclaimableBytes{};          // сколько освободим (по аллоцированному)
    std::uint64_t reclaimableEstimatedBytes{}; // часть reclaimable, посчитанная по логическому
    std::uint64_t sparseBytes{};               // логический размер, не выделенный на диске
    std::uint64_t overheadBytes{};             // аллоцированный сверх логического
};

// Накопитель по каталогу: без исключений и без выделений, потому что вызывается
// из горячего цикла обхода (SPEC §5 «потоковая обработка»).
class SizeAccumulator {
public:
    // Файл с известной парой размеров: логический — сколько данных, аллоцированный —
    // сколько кластеров на диске.
    void add(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept;

    // Файл, у которого аллоцированный размер неизвестен: в reclaimable идёт
    // логический размер, но весь вклад помечается оценкой.
    void addEstimated(std::uint64_t logicalBytes) noexcept;

    // Сам каталог занимает кластер, и при удалении он тоже освобождается.
    void addDirectory(std::uint64_t clusterBytes) noexcept;

    void reset() noexcept;

    const SizingTotals& totals() const noexcept { return totals_; }

private:
    SizingTotals totals_{};
};

// Что попадает в агрегат. Risky по умолчанию вне: он скрыт и показывается только
// через «показать все» с подтверждением (SPEC §4 FR-4).
enum class SizingScope { All, NotRisky, SafeOnly };

// Агрегат по отобранным кандидатам. minConfidence — порог из scoring
// (kDefaultConfidenceThreshold); 0 означает «без порога».
SizingTotals aggregateSizes(const std::vector<CleanupCandidate>& candidates,
                            SizingScope scope = SizingScope::NotRisky, int minConfidence = 0) noexcept;

// Агрегат по явно выбранным элементам: индексы в candidates. Дубликаты и индексы
// за границами игнорируются — расчёт агрегата не имеет права ронять UI.
SizingTotals aggregateSizes(const std::vector<CleanupCandidate>& candidates,
                            const std::vector<std::size_t>& selected, int minConfidence = 0) noexcept;

// Группировка агрегатов для дерева на экране «Очистка» (SPEC §4 FR-5).
enum class GroupBy { None, Category, RuleId, Safety };

struct SizeGroup {
    std::string key;
    SizingTotals totals;
};

// Порядок результата детерминированный: по убыванию освобождаемого, при равенстве
// по ключу, — иначе golden-тесты отчётов и скриншоты UI дрожат.
std::vector<SizeGroup> groupSizes(const std::vector<CleanupCandidate>& candidates, GroupBy groupBy,
                                  SizingScope scope = SizingScope::NotRisky, int minConfidence = 0);

// Пояснение для Reasons: «сколько на диске и чем отличается от логического».
// Спека (§4 FR-4, §7.2) требует, чтобы большая цифра всегда сопровождалась словами
// «по аллоцированному размеру».
std::string describeSizeUsage(const CleanupCandidate& candidate, int decimals = 1);
std::string describeTotals(const SizingTotals& totals, int decimals = 1);

}  // namespace mrproper::core
