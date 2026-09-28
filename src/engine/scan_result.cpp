// Снимок результата сканирования и статистика прогона: реализация
// (SPEC §6.4, §6.3). Слой engine, переносимый: ни windows.h, ни COM, ни
// ввода-вывода (SPEC §6.1, ADR-004).
//
// Разделение работы на «считается один раз» и «читается много раз»:
//   * один раз при публикации — агрегаты по категориям и уровням безопасности,
//     счётчики прогона, проверка согласованности;
//   * читается часто — готовые ScanTotals и ScanStats. UI перерисовывает дерево
//     часто, и пересчёт по сотням кандидатов на каждом кадре — лишняя работа
//     (SPEC §5: «Производительность», «Память ≤ 150 МБ»).
#include "scan_result.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "core/log.hpp"
#include "core/sizing.hpp"
#include "core/units.hpp"

namespace mrproper::engine {
namespace {

// FNV-1a: отпечаток снимка для журнала и для дешёвой проверки «другой ли
// результат». Не криптографическая сумма и защитой не является: задача —
// связать два сообщения журнала об одном прогоне.
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mixValue(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFull;
        hash *= kFnvPrime;
    }
}

void mixText(std::uint64_t& hash, std::string_view text) noexcept {
    for (const char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= kFnvPrime;
    }
}

// Русские окончания. core::units::formatCount жёстко зашит на «файл», а здесь
// считаются кандидаты («элемент») и категории, поэтому форма своя.
// Временная мера: когда core::units получит plural с параметром (pluralRu там
// приватная), эти три строки удаляются, а вызовы переходят на core::units.
const char* pluralEnding(std::uint64_t count, const char* one, const char* few, const char* many) {
    const std::uint64_t mod100 = count % 100;
    if (mod100 >= 11 && mod100 <= 14) return many;
    switch (count % 10) {
        case 1:
            return one;
        case 2:
        case 3:
        case 4:
            return few;
        default:
            return many;
    }
}

std::string countWith(std::uint64_t count, const char* one, const char* few, const char* many) {
    return std::to_string(count) + " " + pluralEnding(count, one, few, many);
}

std::string itemsText(std::uint64_t count) { return countWith(count, "элемент", "элемента", "элементов"); }
std::string categoriesText(std::uint64_t count) { return countWith(count, "категория", "категории", "категорий"); }

// Длительность прогона. Для значений меньше секунды core::units::formatAge
// («0 с») обманывает: скан, который занял 40 мс, выглядит как мгновенный.
std::string durationText(std::chrono::milliseconds value) {
    if (value.count() < 0) return "—";
    if (value < std::chrono::seconds(1)) return std::to_string(value.count()) + " мс";
    return core::formatAge(static_cast<std::int64_t>(value.count() / 1000));
}

std::int64_t unixNowSeconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Ключ категории в агрегатах. Пустая категория у кандидата — дефект разметки,
// но показывать её как пустую строку хуже: причина пропала бы совсем. Тот же
// приём, что в core::sizing (kUnknownGroupKey).
const std::string& categoryKey(const core::CleanupCandidate& candidate) {
    static const std::string unknown{core::kUnknownGroupKey};
    return candidate.category.empty() ? unknown : candidate.category;
}

SafetyTotals* safetyBucket(CategoryTotals& totals, core::SafetyLevel safety) {
    switch (safety) {
        case core::SafetyLevel::Safe:
            return &totals.safe;
        case core::SafetyLevel::Review:
            return &totals.review;
        case core::SafetyLevel::Risky:
            return &totals.risky;
    }
    // Неизвестное значение перечисления (испорченный байт в структуре) считаем
    // Review: это самый осторожный уровень, который всё ещё показывается.
    return &totals.review;
}

// Перегрузка для агрегатов по всему снимку: те же три корзины, что и у строки
// дерева. Отдельная функция, а не переиспользование CategoryTotals: у ScanTotals
// свои счётчики, и «сложить в totals.categories» здесь нельзя — снимок уже
// разобран по строкам дерева.
SafetyTotals* safetyBucket(ScanTotals& totals, core::SafetyLevel safety) {
    switch (safety) {
        case core::SafetyLevel::Safe:
            return &totals.safe;
        case core::SafetyLevel::Review:
            return &totals.review;
        case core::SafetyLevel::Risky:
            return &totals.risky;
    }
    return &totals.review;
}

void accumulate(SafetyTotals& bucket, const core::CleanupCandidate& candidate) {
    ++bucket.candidateCount;
    bucket.allocatedBytes = core::saturatingAdd(bucket.allocatedBytes, candidate.allocatedBytes);
    bucket.logicalBytes = core::saturatingAdd(bucket.logicalBytes, candidate.logicalBytes);
    if (!candidate.lockedBy.empty()) {
        ++bucket.lockedCount;
    }
}

// Агрегаты снимка. Считается один раз при публикации (см. шапку файла).
ScanTotals computeTotals(const std::vector<core::PhysicalDisk>& disks,
                         const std::vector<core::CleanupCandidate>& candidates) {
    ScanTotals totals;
    totals.candidateCount = candidates.size();
    totals.diskCount = disks.size();

    for (const auto& disk : disks) {
        totals.partitionCount += disk.partitions.size();
        totals.diskBytes = core::saturatingAdd(totals.diskBytes, disk.sizeBytes);
        for (const auto& partition : disk.partitions) {
            // Том на разделе учитывается один раз: FR-1 п.5 допускает, что том
            // виден через несколько разделов, и повторный учёт завысил бы
            // «свободно станет».
            if (!partition.hasVolume) continue;
            ++totals.volumeCount;
            totals.volumeTotalBytes = core::saturatingAdd(totals.volumeTotalBytes, partition.volume.totalBytes);
            totals.freeBytes = core::saturatingAdd(totals.freeBytes, partition.volume.freeBytes);
        }
    }

    // map, а не unordered_map: порядок категорий не должен зависеть от того, в
    // каком потоке нашёлся кандидат. Дальше список сортируется по убыванию
    // освобождаемого места — так требуют FR-5 и golden-тесты (§11.4).
    std::map<std::string, CategoryTotals> byCategory;
    for (const auto& candidate : candidates) {
        CategoryTotals& category = byCategory[categoryKey(candidate)];
        ++category.candidateCount;
        category.allocatedBytes = core::saturatingAdd(category.allocatedBytes, candidate.allocatedBytes);
        category.logicalBytes = core::saturatingAdd(category.logicalBytes, candidate.logicalBytes);
        category.fileCount += candidate.fileCount;
        if (!candidate.lockedBy.empty()) {
            ++category.lockedCount;
        }
        accumulate(*safetyBucket(category, candidate.safety), candidate);

        totals.allocatedBytes = core::saturatingAdd(totals.allocatedBytes, candidate.allocatedBytes);
        totals.logicalBytes = core::saturatingAdd(totals.logicalBytes, candidate.logicalBytes);
        totals.fileCount += candidate.fileCount;
        accumulate(*safetyBucket(totals, candidate.safety), candidate);
    }

    totals.categories.reserve(byCategory.size());
    for (auto& entry : byCategory) {
        totals.categories.push_back(std::move(entry.second));
    }
    std::sort(totals.categories.begin(), totals.categories.end(), [](const CategoryTotals& left, const CategoryTotals& right) {
        if (left.allocatedBytes != right.allocatedBytes) return left.allocatedBytes > right.allocatedBytes;
        return left.category < right.category;
    });
    return totals;
}

// Сколько замечаний уровня Error. addError() считает отказы БЕЗ записи,
// addIssue() — с записью, поэтому полный счётчик ошибок складывается из двух
// источников, а не берётся из одного.
std::size_t errorIssueCount(const std::vector<ScanIssue>& issues) {
    std::size_t count = 0;
    for (const auto& issue : issues) {
        if (issue.level == ScanIssueLevel::Error) {
            ++count;
        }
    }
    return count;
}

void appendReason(std::string& target, std::string reason) {
    if (reason.empty()) return;
    if (!target.empty()) {
        target += "; ";
    }
    target += std::move(reason);
}

}  // namespace

// ---------------------------------------------------------------------------
// Имена
// ---------------------------------------------------------------------------

const char* scanPhaseName(ScanPhase phase) noexcept {
    switch (phase) {
        case ScanPhase::Idle:
            return "idle";
        case ScanPhase::Inventory:
            return "inventory";
        case ScanPhase::Candidates:
            return "candidates";
        case ScanPhase::Scoring:
            return "scoring";
        case ScanPhase::Finalizing:
            return "finalizing";
        case ScanPhase::Done:
            return "done";
        case ScanPhase::Cancelled:
            return "cancelled";
        case ScanPhase::Failed:
            return "failed";
    }
    return "unknown";
}

const char* scanIssueLevelName(ScanIssueLevel level) noexcept {
    switch (level) {
        case ScanIssueLevel::Info:
            return "info";
        case ScanIssueLevel::Warning:
            return "warning";
        case ScanIssueLevel::Error:
            return "error";
    }
    return "unknown";
}

const char* scanStageName(ScanStage stage) noexcept {
    switch (stage) {
        case ScanStage::Inventory:
            return "inventory";
        case ScanStage::RuleSet:
            return "ruleset";
        case ScanStage::Candidates:
            return "candidates";
        case ScanStage::Walking:
            return "walking";
        case ScanStage::Scoring:
            return "scoring";
        case ScanStage::Publishing:
            return "publishing";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Прогресс
// ---------------------------------------------------------------------------

double ScanProgressSnapshot::fraction() const noexcept {
    // totalBytes == 0 — «объём неизвестен», а не «сделано ноль». Показывать по
    // нему долю нельзя: индикатор прыгал бы с 0 % на 100 % и обратно.
    if (totalBytes == 0 || bytesScanned == 0) return 0.0;
    const double done = static_cast<double>(bytesScanned);
    const double total = static_cast<double>(totalBytes);
    if (done >= total) return 1.0;
    return done / total;
}

void ScanProgress::begin(std::uint64_t generation, std::int64_t startedAtUnix) noexcept {
    files_.store(0, std::memory_order_relaxed);
    directories_.store(0, std::memory_order_relaxed);
    bytes_.store(0, std::memory_order_relaxed);
    logicalBytes_.store(0, std::memory_order_relaxed);
    candidates_.store(0, std::memory_order_relaxed);
    errors_.store(0, std::memory_order_relaxed);
    skipped_.store(0, std::memory_order_relaxed);
    cancelRequests_.store(0, std::memory_order_relaxed);
    totalBytes_.store(0, std::memory_order_relaxed);
    phase_.store(static_cast<std::uint8_t>(ScanPhase::Inventory), std::memory_order_relaxed);
    startedAtUnix_.store(startedAtUnix, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(pathMutex_);
        path_.clear();
    }
    start_ = std::chrono::steady_clock::now();
    // Флаг поднимается последним: читатель, увидевший begun_ == true, уже имеет
    // право смотреть на start_ (acquire/release).
    begun_.store(true, std::memory_order_release);
    generation_.store(generation, std::memory_order_relaxed);
}

void ScanProgress::setPhase(ScanPhase phase) noexcept {
    phase_.store(static_cast<std::uint8_t>(phase), std::memory_order_relaxed);
}

void ScanProgress::addFile(std::uint64_t logicalBytes, std::uint64_t allocatedBytes) noexcept {
    // relaxed, а не seq_cst: счётчики публикуют числа, а не данные, и каждый
    // файл обхода — это горячий цикл (SPEC §5: не более 8 потоков, потоковая
    // обработка). Потери обновлений не бывает: fetch_add атомарен.
    files_.fetch_add(1, std::memory_order_relaxed);
    logicalBytes_.fetch_add(logicalBytes, std::memory_order_relaxed);
    bytes_.fetch_add(allocatedBytes, std::memory_order_relaxed);
}

void ScanProgress::addDirectory() noexcept { directories_.fetch_add(1, std::memory_order_relaxed); }

void ScanProgress::addCandidate() noexcept { candidates_.fetch_add(1, std::memory_order_relaxed); }

void ScanProgress::addError() noexcept { errors_.fetch_add(1, std::memory_order_relaxed); }

void ScanProgress::addSkipped() noexcept { skipped_.fetch_add(1, std::memory_order_relaxed); }

void ScanProgress::setTotalBytes(std::uint64_t totalBytes) noexcept {
    totalBytes_.store(totalBytes, std::memory_order_relaxed);
}

void ScanProgress::addTotalBytes(std::uint64_t totalBytes) noexcept {
    totalBytes_.fetch_add(totalBytes, std::memory_order_relaxed);
}

void ScanProgress::setCurrentPath(std::string path) {
    // Мьютекс здесь — единственное место блокировки в прогрессе, и вызывающий
    // обязан звать метод на смену каталога, а не на каждый файл.
    std::lock_guard<std::mutex> lock(pathMutex_);
    if (path == path_) return;  // не дёргать мьютекс и не менять строку впустую
    path_ = std::move(path);
}

void ScanProgress::noteCancelRequest() noexcept { cancelRequests_.fetch_add(1, std::memory_order_relaxed); }

void ScanProgress::noteCancelRequests(std::uint64_t count) noexcept {
    if (count == 0) return;
    cancelRequests_.fetch_add(count, std::memory_order_relaxed);
}

ScanPhase ScanProgress::phase() const noexcept { return static_cast<ScanPhase>(phase_.load(std::memory_order_relaxed)); }

bool ScanProgress::cancelRequested() const noexcept { return cancelRequests_.load(std::memory_order_relaxed) != 0; }

ScanProgressSnapshot ScanProgress::snapshot() const {
    ScanProgressSnapshot out;
    out.phase = static_cast<ScanPhase>(phase_.load(std::memory_order_relaxed));
    out.generation = generation_.load(std::memory_order_relaxed);
    out.filesScanned = files_.load(std::memory_order_relaxed);
    out.directoriesScanned = directories_.load(std::memory_order_relaxed);
    out.bytesScanned = bytes_.load(std::memory_order_relaxed);
    out.logicalBytesScanned = logicalBytes_.load(std::memory_order_relaxed);
    out.candidatesFound = candidates_.load(std::memory_order_relaxed);
    out.errors = errors_.load(std::memory_order_relaxed);
    out.skipped = skipped_.load(std::memory_order_relaxed);
    out.cancelRequests = cancelRequests_.load(std::memory_order_relaxed);
    out.cancelRequested = out.cancelRequests != 0;
    out.totalBytes = totalBytes_.load(std::memory_order_relaxed);
    out.startedAtUnix = startedAtUnix_.load(std::memory_order_relaxed);
    out.nowUnix = unixNowSeconds();
    if (begun_.load(std::memory_order_acquire)) {
        // Длительность по steady_clock: системные часы умеют прыгать назад при
        // переводе времени, и «скан занял -3 с» в отчёте — не объяснение.
        // Явный duration_cast обязателен: steady_clock::duration — наносекунды,
        // а поле elapsed — миллисекунды, и неявного преобразования между ними нет.
        out.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
    }
    {
        std::lock_guard<std::mutex> lock(pathMutex_);
        out.currentPath = path_;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Агрегаты
// ---------------------------------------------------------------------------

double ScanTotals::reclaimableFraction() const noexcept {
    if (volumeTotalBytes == 0) return 0.0;
    const double total = static_cast<double>(volumeTotalBytes);
    const double free = static_cast<double>(freeBytes);
    const double reclaim = static_cast<double>(allocatedBytes);
    const double after = free + reclaim;
    if (after >= total) return 1.0;
    return after / total;
}

const CategoryTotals* ScanTotals::category(std::string_view name) const noexcept {
    for (const auto& totals : categories) {
        if (totals.category == name) return &totals;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Снимок
// ---------------------------------------------------------------------------

bool ScanResult::empty() const noexcept { return candidates.empty() && disks.empty(); }

const core::PhysicalDisk* ScanResult::diskByNumber(int number) const noexcept {
    for (const auto& disk : disks) {
        if (disk.number == number) return &disk;
    }
    return nullptr;
}

const CategoryTotals* ScanResult::category(std::string_view name) const noexcept {
    return totals.category(name);
}

std::vector<std::size_t> ScanResult::largestCandidateIndexes(std::size_t limit) const {
    std::vector<std::size_t> indexes;
    indexes.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        indexes.push_back(index);
    }
    std::sort(indexes.begin(), indexes.end(), [this](std::size_t left, std::size_t right) {
        if (candidates[left].allocatedBytes != candidates[right].allocatedBytes) {
            return candidates[left].allocatedBytes > candidates[right].allocatedBytes;
        }
        return left < right;  // при равных байтах — исходный порядок: детерминированно
    });
    if (indexes.size() > limit) {
        indexes.resize(limit);
    }
    return indexes;
}

std::string ScanResult::headline() const {
    if (totals.candidateCount == 0) {
        return stats.cancelled ? "Скан прерван: ничего не найдено" : "Ничего не найдено";
    }
    // «по аллоцированному размеру» — FR-4 и §7.2: большая цифра всегда с этим
    // уточнением, иначе она читается как «столько данных лежит на диске».
    std::string text = "Освободится " + core::formatBytes(totals.allocatedBytes) + " — " + itemsText(totals.candidateCount) +
                       " в " + categoriesText(totals.categories.size()) + " (по аллоцированному размеру)";
    if (stats.cancelled) {
        text += "; скан прерван";
    } else if (stats.degraded) {
        text += "; есть замечания";
    }
    return text;
}

std::string ScanResult::toText() const {
    std::string out;
    out += "Скан #" + std::to_string(generation);
    out += ": ";
    out += stats.cancelled ? "прерван" : (stats.degraded ? "завершён с замечаниями" : "завершён");
    out += " за " + durationText(stats.duration);
    out += "\nДиски: " + std::to_string(totals.diskCount) + ", разделов: " + std::to_string(totals.partitionCount) +
           ", томов: " + std::to_string(totals.volumeCount) + "; объём дисков " + core::formatBytes(totals.diskBytes) +
           ", свободно на томах " + core::formatBytes(totals.freeBytes);
    if (totals.volumeTotalBytes > 0) {
        out += " (свободно станет " + core::formatPercent(totals.reclaimableFraction()) + ")";
    }
    out += "\n" + headline();
    out += "\nПо уровням: Safe " + std::to_string(totals.safe.candidateCount) + " (" + core::formatBytes(totals.safe.allocatedBytes) +
           "), Review " + std::to_string(totals.review.candidateCount) + " (" + core::formatBytes(totals.review.allocatedBytes) +
           "), Risky " + std::to_string(totals.risky.candidateCount) + " (" + core::formatBytes(totals.risky.allocatedBytes) + ")";
    for (const auto& category : totals.categories) {
        out += "\n  " + category.category + ": " + core::formatBytes(category.allocatedBytes) + " в " +
               std::to_string(category.candidateCount) + " шт.";
    }
    out += "\nПравила: " + (rulesVersion.empty() ? std::string("(не указана версия)") : rulesVersion) +
           "; потоков в пуле: " + std::to_string(stats.workerThreads) + "; правил: " + std::to_string(stats.ruleCount);
    out += "\nОбход: файлов " + core::formatCount(stats.filesScanned) + ", каталогов " +
           std::to_string(stats.directoriesScanned) + ", байт (аллоцированных) " + core::formatBytes(stats.bytesScanned) +
           ", байт (логических) " + core::formatBytes(stats.logicalBytesScanned);
    out += "\nОтказы: ошибок " + std::to_string(stats.errorCount) + ", пропущено " + std::to_string(stats.skippedCount) +
           ", заблокировано кандидатов " + std::to_string(stats.lockedCandidateCount);
    if (stats.issuesDropped > 0) {
        out += ", замечаний не поместилось " + std::to_string(stats.issuesDropped);
    }
    if (stats.cancelled) {
        out += "\nОтмена: " + (stats.cancelReason.empty() ? std::string("(причина не указана)") : stats.cancelReason);
    }
    if (stats.degraded) {
        out += "\nПотери: " + (stats.degradedReason.empty() ? std::string("(причина не указана)") : stats.degradedReason);
    }
    out += "\nЗамечаний: " + std::to_string(issues.size());
    if (!issues.empty()) {
        // В баг-репорте список из 512 строк бесполезен: первые 20 отделяют
        // «массовую потерю» от единичного сбоя, а счётчик выше говорит, что
        // замечаний было больше.
        const std::size_t shown = std::min<std::size_t>(issues.size(), 20);
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& issue = issues[index];
            out += "\n  [";
            out += scanIssueLevelName(issue.level);
            out += "/";
            out += scanStageName(issue.stage);
            out += "] ";
            out += issue.subject.empty() ? issue.message : issue.subject + ": " + issue.message;
            if (issue.code != 0) {
                out += " (код " + std::to_string(issue.code) + ")";
            }
        }
        if (issues.size() > shown) {
            out += "\n  … ещё " + std::to_string(issues.size() - shown);
        }
    }
    out += "\nОтпечаток: " + std::to_string(signature());
    return out;
}

std::uint64_t ScanResult::signature() const noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    mixValue(hash, generation);
    mixValue(hash, static_cast<std::uint64_t>(disks.size()));
    mixValue(hash, static_cast<std::uint64_t>(candidates.size()));
    mixValue(hash, static_cast<std::uint64_t>(totals.diskCount));
    mixValue(hash, static_cast<std::uint64_t>(totals.candidateCount));
    mixValue(hash, totals.allocatedBytes);
    mixValue(hash, totals.logicalBytes);
    mixValue(hash, static_cast<std::uint64_t>(totals.categories.size()));
    mixValue(hash, static_cast<std::uint64_t>(stats.duration.count()));
    mixValue(hash, complete ? 1u : 0u);
    mixText(hash, rulesVersion);
    return hash;
}

// ---------------------------------------------------------------------------
// Сборщик
// ---------------------------------------------------------------------------

ScanResultBuilder::ScanResultBuilder(ScanBuildOptions options) : options_(std::move(options)) {}

bool ScanResultBuilder::addDisk(core::PhysicalDisk disk) {
    if (sealed_) {
        ++rejected_;
        return false;
    }
    disks_.push_back(std::move(disk));
    return true;
}

bool ScanResultBuilder::addCandidate(core::CleanupCandidate candidate) {
    if (sealed_) {
        ++rejected_;
        return false;
    }
    if (candidate.path.empty()) {
        // Кандидат без пути удалить нельзя и объяснить нечем (§12: ни один
        // элемент не удаляется без объяснения). Молча пропустить его значило бы
        // получить расхождение «цифры в UI vs операции в отчёте».
        ++rejected_;
        return false;
    }
    candidates_.push_back(std::move(candidate));
    return true;
}

bool ScanResultBuilder::addIssue(ScanIssue issue) {
    if (sealed_) {
        ++rejected_;
        return false;
    }
    if (issues_.size() >= kMaxScanIssues) {
        ++issuesDropped_;
        return false;
    }
    issues_.push_back(std::move(issue));
    return true;
}

void ScanResultBuilder::addError() {
    if (sealed_) {
        ++rejected_;
        return;
    }
    ++errors_;
}

void ScanResultBuilder::noteCancelled(std::string reason) {
    if (sealed_) {
        ++rejected_;
        return;
    }
    cancelled_ = true;
    appendReason(cancelReason_, std::move(reason));
}

void ScanResultBuilder::noteDegraded(std::string reason) {
    if (sealed_) {
        ++rejected_;
        return;
    }
    degraded_ = true;
    appendReason(degradedReason_, std::move(reason));
}

std::vector<std::string> ScanResultBuilder::validate() const {
    // Собираем пробный снимок и прогоняем через общий валидатор: две проверки в
    // одном месте, иначе «до публикации» и «после» проверяли бы разное.
    // Копия векторов — цена одного validate(), который вызывают перед
    // публикацией, а не на каждый кадр UI.
    ScanResult probe;
    probe.disks = disks_;
    probe.candidates = candidates_;
    probe.issues = issues_;
    probe.rulesVersion = options_.rulesVersion;
    probe.generation = options_.generation;
    probe.totals = computeTotals(probe.disks, probe.candidates);
    probe.stats.candidateCount = probe.candidates.size();
    probe.stats.ruleCount = options_.ruleCount;
    probe.stats.workerThreads = options_.workerThreads;
    probe.stats.errorCount = errors_ + errorIssueCount(probe.issues);
    probe.stats.issuesDropped = issuesDropped_;
    probe.stats.cancelled = cancelled_;
    probe.stats.degraded = degraded_;
    // Причины переносятся в пробу: validateScanResult требует их непустыми при
    // cancelled/degraded, иначе проверка ругалась бы на нормальный отменённый
    // прогон, у которого причина есть, но в пробу не попала.
    probe.stats.cancelReason = cancelReason_;
    probe.stats.degradedReason = degradedReason_;
    probe.complete = !cancelled_;
    return validateScanResult(probe);
}

ScanResultPtr ScanResultBuilder::publish(const ScanProgressSnapshot& progress) {
    if (sealed_) {
        return published_;
    }
    // Запечатываем ДО сборки: если что-то пойдёт не так, повторный publish()
    // обязан вернуть тот же снимок, а не собрать второй из опустошённого
    // сборщика.
    sealed_ = true;

    ScanResult result;
    result.disks = std::move(disks_);
    result.candidates = std::move(candidates_);
    result.issues = std::move(issues_);
    result.rulesVersion = options_.rulesVersion;
    result.generation = options_.generation != 0 ? options_.generation : progress.generation;
    if (progress.nowUnix != 0) {
        result.finishedAt = std::chrono::system_clock::time_point{std::chrono::seconds{progress.nowUnix}};
    }
    result.complete = !cancelled_;

    result.stats.startedAtUnix = progress.startedAtUnix;
    result.stats.finishedAtUnix = progress.nowUnix;
    result.stats.duration = progress.elapsed;
    result.stats.filesScanned = progress.filesScanned;
    result.stats.directoriesScanned = progress.directoriesScanned;
    result.stats.bytesScanned = progress.bytesScanned;
    result.stats.logicalBytesScanned = progress.logicalBytesScanned;
    result.stats.skippedCount = static_cast<std::size_t>(progress.skipped);
    result.stats.candidateCount = result.candidates.size();
    result.stats.ruleCount = options_.ruleCount;
    result.stats.workerThreads = options_.workerThreads;
    result.stats.errorCount = errors_ + errorIssueCount(result.issues);
    result.stats.issuesDropped = issuesDropped_;
    // Счётчик заблокированных берётся из самих кандидатов (FR-4 LockedBy), а не
    // из прогресса: список кандидатов — единственный источник истины, иначе
    // цифра в UI разошлась бы с содержимым снимка.
    for (const auto& candidate : result.candidates) {
        if (!candidate.lockedBy.empty()) {
            ++result.stats.lockedCandidateCount;
        }
    }
    result.stats.cancelled = cancelled_;
    result.stats.degraded = degraded_;
    result.stats.cancelReason = cancelReason_;
    result.stats.degradedReason = degradedReason_;

    result.totals = computeTotals(result.disks, result.candidates);

    published_ = std::make_shared<const ScanResult>(std::move(result));
    const ScanResult& snapshot = *published_;
    core::logInfo("engine.scan.publish", "снимок сканирования опубликован",
                  core::LogFields{core::logField("generation", snapshot.generation),
                                  core::logField("candidates", static_cast<std::int64_t>(snapshot.candidates.size())),
                                  core::logField("allocatedBytes", snapshot.totals.allocatedBytes),
                                  core::logField("disks", static_cast<std::int64_t>(snapshot.disks.size())),
                                  core::logField("durationMs", static_cast<std::int64_t>(snapshot.stats.duration.count())),
                                  core::logField("errors", static_cast<std::int64_t>(snapshot.stats.errorCount)),
                                  core::logField("cancelled", snapshot.stats.cancelled),
                                  core::logField("degraded", snapshot.stats.degraded),
                                  core::logField("threads", static_cast<std::int64_t>(snapshot.stats.workerThreads)),
                                  core::logField("rules", snapshot.rulesVersion),
                                  core::logField("signature", snapshot.signature())});
    return published_;
}

ScanResultPtr ScanResultBuilder::publish(const ScanProgress& progress) { return publish(progress.snapshot()); }

// ---------------------------------------------------------------------------
// Публикация
// ---------------------------------------------------------------------------

ScanResultPtr ScanResultSlot::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void ScanResultSlot::publish(ScanResultPtr result) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(result);
    if (current_ != nullptr) {
        ++publishCount_;
    }
}

void ScanResultSlot::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.reset();
}

bool ScanResultSlot::hasValue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ != nullptr;
}

std::uint64_t ScanResultSlot::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ != nullptr ? current_->generation : std::uint64_t{0};
}

std::size_t ScanResultSlot::publishCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishCount_;
}

// ---------------------------------------------------------------------------
// Проверка согласованности
// ---------------------------------------------------------------------------

std::vector<std::string> validateScanResult(const ScanResult& result) {
    std::vector<std::string> problems;

    std::uint64_t allocatedSum = 0;
    std::uint64_t logicalSum = 0;
    std::uint64_t fileSum = 0;
    std::size_t lockedCount = 0;
    std::map<std::string, std::size_t> perCategory;
    std::set<std::string> seen;

    for (std::size_t index = 0; index < result.candidates.size(); ++index) {
        const auto& candidate = result.candidates[index];
        const std::string where = "кандидат #" + std::to_string(index) + " (" + candidate.path + ")";

        if (candidate.path.empty()) {
            problems.push_back(where + ": пустой путь — элемент нельзя ни показать, ни удалить");
        }
        if (candidate.ruleId.empty()) {
            // Без идентификатора правила не выполняется инвариант §6.3 «путь
            // кандидата находится внутри корня правила» и нечем объяснить
            // пользователю, почему этот элемент найден (FR-4, §12).
            problems.push_back(where + ": пустой ruleId — кандидат не привязан к правилу");
        }
        if (candidate.confidence < 0 || candidate.confidence > 100) {
            problems.push_back(where + ": confidence " + std::to_string(candidate.confidence) + " вне [0;100] (§6.3)");
        }
        switch (candidate.safety) {
            case core::SafetyLevel::Safe:
            case core::SafetyLevel::Review:
            case core::SafetyLevel::Risky:
                break;
            default:
                problems.push_back(where + ": неизвестный уровень безопасности");
                break;
        }
        // Один и тот же путь в одной категории дважды означает двойной счёт и в
        // totals, и в плане: освобождение будет завышено. Дедупликация — забота
        // координатора (два правила могут матчить один каталог), а валидатор
        // обязан это показать, а не промолчать.
        const std::string key = categoryKey(candidate) + '\x1f' + candidate.path;
        if (!seen.insert(key).second) {
            problems.push_back(where + ": путь встречается в категории повторно — объём посчитан дважды");
        }

        allocatedSum = core::saturatingAdd(allocatedSum, candidate.allocatedBytes);
        logicalSum = core::saturatingAdd(logicalSum, candidate.logicalBytes);
        fileSum += candidate.fileCount;
        if (!candidate.lockedBy.empty()) {
            ++lockedCount;
        }
        ++perCategory[categoryKey(candidate)];
    }

    if (result.totals.candidateCount != result.candidates.size()) {
        problems.push_back("totals.candidateCount (" + std::to_string(result.totals.candidateCount) +
                           ") != candidates.size() (" + std::to_string(result.candidates.size()) + ")");
    }
    if (result.totals.allocatedBytes != allocatedSum) {
        problems.push_back("totals.allocatedBytes (" + std::to_string(result.totals.allocatedBytes) +
                           ") != сумма кандидатов (" + std::to_string(allocatedSum) + ")");
    }
    if (result.totals.logicalBytes != logicalSum) {
        problems.push_back("totals.logicalBytes (" + std::to_string(result.totals.logicalBytes) +
                           ") != сумма кандидатов (" + std::to_string(logicalSum) + ")");
    }
    if (result.totals.fileCount != fileSum) {
        problems.push_back("totals.fileCount (" + std::to_string(result.totals.fileCount) +
                           ") != сумма кандидатов (" + std::to_string(fileSum) + ")");
    }
    if (result.totals.safe.candidateCount + result.totals.review.candidateCount + result.totals.risky.candidateCount !=
        result.candidates.size()) {
        problems.push_back("сумма кандидатов по уровням безопасности не равна общему числу кандидатов");
    }

    // Агрегаты по категориям: тот же набор, тот же порядок, те же числа.
    if (result.totals.categories.size() != perCategory.size()) {
        problems.push_back("число категорий в totals (" + std::to_string(result.totals.categories.size()) +
                           ") != числу категорий среди кандидатов (" + std::to_string(perCategory.size()) + ")");
    }
    for (const auto& category : result.totals.categories) {
        const auto found = perCategory.find(category.category);
        if (found == perCategory.end()) {
            problems.push_back("категория в totals без кандидатов: " + category.category);
            continue;
        }
        if (found->second != category.candidateCount) {
            problems.push_back("категория " + category.category + ": в totals " +
                               std::to_string(category.candidateCount) + " кандидатов, среди кандидатов " +
                               std::to_string(found->second));
        }
    }
    for (std::size_t index = 1; index < result.totals.categories.size(); ++index) {
        const CategoryTotals& previous = result.totals.categories[index - 1];
        const CategoryTotals& current = result.totals.categories[index];
        const bool ordered = previous.allocatedBytes > current.allocatedBytes ||
                             (previous.allocatedBytes == current.allocatedBytes && previous.category < current.category);
        if (!ordered) {
            problems.push_back("категории не отсортированы по убыванию объёма: " + previous.category + " перед " +
                               current.category);
        }
    }

    if (result.totals.diskCount != result.disks.size()) {
        problems.push_back("totals.diskCount (" + std::to_string(result.totals.diskCount) +
                           ") != disks.size() (" + std::to_string(result.disks.size()) + ")");
    }
    std::set<int> diskNumbers;
    for (const auto& disk : result.disks) {
        if (disk.number < 0) {
            problems.push_back("диск с отрицательным номером " + std::to_string(disk.number) +
                               " — номер не получен (FR-1: устройство помечается недоступным)");
        }
        if (!diskNumbers.insert(disk.number).second) {
            problems.push_back("диск №" + std::to_string(disk.number) + " учтён дважды — обход дисков должен быть одноразовым (FR-1)");
        }
    }

    if (result.stats.candidateCount != result.candidates.size()) {
        problems.push_back("stats.candidateCount (" + std::to_string(result.stats.candidateCount) +
                           ") != candidates.size() (" + std::to_string(result.candidates.size()) + ")");
    }
    if (result.stats.lockedCandidateCount != lockedCount) {
        problems.push_back("stats.lockedCandidateCount (" + std::to_string(result.stats.lockedCandidateCount) +
                           ") != кандидатов с lockedBy (" + std::to_string(lockedCount) + ")");
    }
    if (result.stats.cancelled && result.complete) {
        problems.push_back("stats.cancelled == true, но complete == true: прерванный прогон не может быть полным");
    }
    if (result.stats.cancelled && result.stats.cancelReason.empty()) {
        problems.push_back("прогон отменён без причины — в отчёте и в UI нечего показать");
    }
    if (result.stats.degraded && result.stats.degradedReason.empty()) {
        problems.push_back("прогон помечен деградированным без причины");
    }
    if (result.stats.duration.count() < 0) {
        problems.push_back("отрицательная длительность прогона: " + std::to_string(result.stats.duration.count()) + " мс");
    }
    if (result.stats.startedAtUnix != 0 && result.stats.finishedAtUnix != 0 &&
        result.stats.finishedAtUnix < result.stats.startedAtUnix) {
        problems.push_back("время окончания раньше времени начала: " + std::to_string(result.stats.startedAtUnix) + " → " +
                           std::to_string(result.stats.finishedAtUnix));
    }
    if (result.issues.size() > kMaxScanIssues) {
        problems.push_back("замечаний в снимке больше kMaxScanIssues: " + std::to_string(result.issues.size()));
    }
    if (result.rulesVersion.empty()) {
        // FR-8 требует версию набора правил в отчёте: без неё нельзя понять,
        // по каким правилам сработал скан.
        problems.push_back("не указана версия набора правил (FR-8: отчёт обязан её содержать)");
    }
    return problems;
}

}  // namespace mrproper::engine
