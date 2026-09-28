// Построение плана очистки: отбор кандидатов, агрегаты, dry-run, снимок (SPEC §4 FR-5).
//
// Переносимый модуль: без Windows API и без ввода-вывода — только решения. Движок
// (engine::PlanBuilder) превращает план в операции, UI показывает агрегаты и
// обязательный dry-run, CLI печатает тот же список в JSON.
//
// Разделение ответственности:
//   * core::scoring — «насколько это безопасно» (SafetyLevel, ConfidenceScore);
//   * core::plan     — «что именно удаляем по умолчанию и сколько это освободит».
//
// Инварианты (SPEC §6.3), которые держит buildPlan и проверяет validatePlan:
//   * ровно один CleanupPlanItem на кандидата, candidateIndex совпадает с позицией;
//   * reclaimBytes == allocatedBytes для Delete/Trash и 0 для Keep/SkipLocked;
//   * элемент с blocked-файлами (lockedBy) никогда не удаляется, а помечается SkipLocked;
//   * Risky не выбирается, пока пользователь явно не включил «показать все» (SPEC §12).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "model.hpp"
#include "scoring.hpp"

namespace mrproper::core {

// Порог отбора по умолчанию — тот же, что у скоринга: элемент с низкой
// уверенностью («создан сегодня», «шаблон широкий») сам не попадает в план.
inline constexpr int kPlanDefaultConfidenceThreshold = kDefaultConfidenceThreshold;

// Выше этого объёма кандидат удаляется напрямую, а не через корзину приложения:
// корзина ограничена (SPEC §4 FR-7) и не должна раздуваться большими кэшами.
inline constexpr std::uint64_t kTrashDirectDeleteBytes = 100ull * 1024ull * 1024ull;

// Профиль отбора: что считать выбранным по умолчанию.
enum class SelectionProfile {
    SafeOnly,    // только Safe — профиль «осторожный» из онбординга
    Recommended, // Safe всегда, Review при confidence >= порога — профиль по умолчанию
    Everything,  // всё, что не заблокировано: «выбрать всё»
};

const char* toString(SelectionProfile profile);
// Разбор имени профиля из настроек/CLI. Неизвестное имя — Recommended.
SelectionProfile selectionProfileFromString(const char* text);

// Почему кандидат не попал в план. Нужен не только для текста, но и для агрегатов
// («сколько скрыто Risky», «сколько ниже порога»).
enum class SkipReason {
    None,           // выбран
    Locked,         // файлы держат приложения (FR-5 Skip (locked), FR-6)
    RiskyHidden,    // Risky без явного подтверждения (FR-4, §12)
    BelowConfidence,// уверенность ниже порога отбора
    ProfileFiltered,// не проходит уровень безопасности профиля
    TooSmall,       // объём ниже минимального порога
};

const char* toString(SkipReason reason);

struct PlanOptions {
    SelectionProfile profile{SelectionProfile::Recommended};
    int confidenceThreshold{kPlanDefaultConfidenceThreshold};
    // Risky показывается и выбирается только через «показать все» с подтверждением
    // (SPEC §4 FR-4, §12). Отдельно от профиля: профиль отвечает за «что брать по
    // умолчанию», этот флаг — за дополнительное подтверждение опасного уровня.
    bool allowRisky{false};
    bool useTrash{true};   // false — всё прямым удалением (ADR-006 для больших объёмов)
    std::uint64_t trashDirectDeleteAboveBytes{kTrashDirectDeleteBytes};
    std::uint64_t minReclaimBytes{};  // не брать кандидатов меньше порога
    bool dryRun{true};                // FR-5: dry-run обязателен и включается по умолчанию
};

// Решение по одному кандидату: действие, признак «выбран» и человекочитаемая причина.
struct SelectionDecision {
    PlanAction action{PlanAction::Keep};
    bool selected{false};
    SkipReason skip{SkipReason::None};
    std::string reason;
};

// Чистое решение по одному кандидату — тот же код, что вызывает buildPlan.
SelectionDecision decideCandidate(const CleanupCandidate& candidate, const PlanOptions& options);

// true для Delete/Trash: действие, которое реально освобождает место.
bool isRemovableAction(PlanAction action);

struct ActionTotals {
    std::size_t count{};
    std::uint64_t bytes{};
};

// Агрегаты по действиям. У Keep и SkipLocked reclaimBytes всегда 0.
struct PlanActionTotals {
    ActionTotals deleteOps;
    ActionTotals trashOps;
    ActionTotals keepOps;
    ActionTotals skipLockedOps;

    const ActionTotals& forAction(PlanAction action) const;
};

// Три цифры, которые показывает экран «Очистка» (FR-5): сколько освободим, если
// выбрать всё / только Safe / только выбранное.
struct PlanTotals {
    std::size_t candidateCount{};
    std::size_t selectedCount{};
    std::uint64_t selectedBytes{};
    std::size_t allCount{};
    std::uint64_t allBytes{};       // «если выбрать всё» (кроме заблокированных)
    std::size_t safeOnlyCount{};
    std::uint64_t safeOnlyBytes{};  // «если выбрать только Safe»
    std::size_t hiddenRiskyCount{};
    std::size_t belowThresholdCount{};
    std::size_t tooSmallCount{};
    std::size_t profileFilteredCount{};
    PlanActionTotals byAction;
};

struct CategoryAggregate {
    std::string category;
    std::size_t candidateCount{};
    std::size_t selectedCount{};
    std::uint64_t selectedBytes{};
    std::uint64_t allBytes{};
    std::uint64_t safeOnlyBytes{};
};

struct CleanupPlan {
    std::vector<CleanupPlanItem> items;  // по одному на кандидата, в порядке кандидатов
    PlanTotals totals;
    std::vector<CategoryAggregate> categories;  // по убыванию выбранных байт, затем по имени
    PlanOptions options;
    bool dryRun{true};

    bool empty() const;
    std::size_t operationCount() const;
    const CleanupPlanItem* item(std::size_t candidateIndex) const;
    std::vector<std::size_t> operationIndexes() const;  // индексы кандидатов к удалению

    // Отпечаток содержимого плана: сменились выборы, профиль, порог или состав
    // операций — отпечаток другой, значит прежнего подтверждения dry-run больше нет.
    std::uint64_t planSignature() const;
};

CleanupPlan buildPlan(const std::vector<CleanupCandidate>& candidates, const PlanOptions& options = PlanOptions{});

// Одна строка плана: и в списке операций, и в снимке, и в отчёте.
struct PlanOperation {
    std::size_t candidateIndex{};
    PlanAction action{PlanAction::Keep};
    SkipReason skip{SkipReason::None};  // для выбранных всегда None
    std::string category;
    std::string displayName;
    std::string path;
    std::uint64_t bytes{};
    SafetyLevel safety{SafetyLevel::Review};
    int confidence{};
    std::string reason;
};

// Точный список операций перед первым удалением в сессии (FR-5).
struct DryRunReport {
    bool dryRun{true};
    std::uint64_t totalBytes{};
    std::size_t operationCount{};
    std::vector<PlanOperation> operations;  // ровно то, что будет выполнено
    std::vector<PlanOperation> untouched;   // что останется на месте и почему
    std::string headline;                   // «Освободится 1,2 ГБ — 214 элементов из 340»
    std::string text;                       // тот же список текстом: UI, CLI, журнал
};

DryRunReport makeDryRunReport(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan);

// Детерминированный JSON плана (SPEC §11.4 — golden-тесты): ключи в фиксированном порядке.
std::string planToJson(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan);

// Снимок состояния перед исполнением: список операций, PID, версия, размер (FR-5).
// Заполняет журнал; значения PID и версии приходят от платформы — ядро их не знает.
struct PlanSnapshotContext {
    std::string appVersion;
    std::uint32_t pid{};
    std::int64_t createdAtUnix{};
};

struct PlanSnapshot {
    std::string appVersion;
    std::uint32_t pid{};
    std::int64_t createdAtUnix{};
    std::size_t operationCount{};
    std::uint64_t totalBytes{};
    std::uint64_t planSignature{};
    std::vector<PlanOperation> operations;
};

PlanSnapshot makeSnapshot(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan,
                          const PlanSnapshotContext& context);

std::string snapshotToJson(const PlanSnapshot& snapshot);

// «Dry-run обязателен и запускается по умолчанию перед первым удалением в сессии».
// Сессия — один запуск приложения: смена выбора или профиля требует нового показа.
class DryRunGate {
public:
    void beginSession();                              // новая сессия: показываем заново
    void acknowledge(const CleanupPlan& plan);         // пользователь увидел и подтвердил план
    bool alreadyShown(const CleanupPlan& plan) const;  // показывали именно этот план
    bool mustShowBeforeExecute(const CleanupPlan& plan) const;

private:
    std::uint64_t signature_{};
    bool acknowledged_{false};
};

// Нарушения инвариантов §6.3. Пустой список — план согласован.
std::vector<std::string> validatePlan(const std::vector<CleanupCandidate>& candidates, const CleanupPlan& plan);

}  // namespace mrproper::core
