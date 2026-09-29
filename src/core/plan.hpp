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
#include <memory>
#include <string>
#include <string_view>
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

// Сколько путей операция показывает в dry-run/JSON. Полный список лежит в
// манифесте (CandidateManifest::allowed) и нужен исп��лнителю; в операцию
// попадает только начало — иначе отчёт на 200 тысяч строк нечитаем, а человек
// перед удалением всё равно не пересчитывает список (SPEC §12).
inline constexpr std::size_t kMaxOperationPaths = 32;

// ---------------------------------------------------------------------------
// Манифест удаления: что кандидату РАЗРЕШЕНО удалять (docs/review-02.md F-01..F-04)
// ---------------------------------------------------------------------------
//
// Отбор кандидата и операция удаления — разные вещи, и раньше они были разными
// только на словах. Правило отбирало файлы (minAgeDays, locatorExcludes), счёт
// в отчёте писал «по возрасту отсечено файлов: 412», а исполнитель получал
// путь КОРНЯ и сносил каталог целиком: вместе со «свежими» файлами и с явно
// исключёнными правилом unins*.exe. Отчёт описывал арифметику объёма, а не
// операцию — это подтверждённый прогоном обход F-01.
//
// Манифест — вторая половина отбора: список путей, которые правило оставило
// кандидату. Удалять можно только по нему. Удаление КОРНЯ допустимо лишь там,
// где правило доказало, что отбирать нечего (rootDeleteAllowed): min-age,
// исключений, порога размера и фильтра листьев нет, и обход ничего не
// пропустил. Иначе операция удаляет по списку, а неполный список означает,
// что элемент не выбирается вовсе: «список неполон» и «удалить всё» несовместимы.
//
// Манифест — продолжение кандидата, а не отдельный документ: candidateIndex
// связывает его с CleanupCandidate, правило известно из неё же (ruleId).
// Отдельным полем в core::CleanupCandidate он станет вместе с model.hpp.

// Один файл, разрешённый к удалению.
struct AllowedEntry {
    std::string path;               // путь в форме модели (§6.3): обратный слэш, буква диска
    std::uint64_t allocatedBytes{}; // аллоцированный размер: столько освободится
};

// Список разрешённого к удалению. Владеет им сборщик; план держит общий
// указатель, потому что список бывает длинным (десятки тысяч путей) и
// пересборка плана при каждом щелчке чекбокса не должна копировать его.
struct AllowedSet {
    std::vector<AllowedEntry> entries;
    std::uint64_t bytes{};   // сумма allocatedBytes — она же allocatedBytes кандидата
    std::size_t omitted{};   // сколько путей не поместилось (предел списка)
    bool complete{true};      // обход дошёл до конца и список не обрезан
};

struct CandidateManifest {
    std::size_t candidateIndex{};
    std::string ruleId;
    std::string rootPath;

    // nullptr — список не собирался: правило ничего не может отсечь, и тогда
    // право удалить корень целиком выражено флагом rootDeleteAllowed.
    std::shared_ptr<const AllowedSet> allowed;

    // Право удалить корень целиком. Ставит сборщик и только когда множество,
    // которое вернул обход, совпадает со всем содержимым корня.
    bool rootDeleteAllowed{false};

    // Правило объявлено «только оценка» (F-04): элемент не удаляется ни при
    // каком профиле, подтверждении и уровне.
    bool estimateOnly{false};

    // Порог размера файла из правила (F-02). 0 — не объявлен.
    std::uint64_t minFileBytes{};

    // Категория пользовательских данных. Без порога размера такие элементы не
    // выбираются вовсе, а с порогом — только через корзину приложения.
    bool userData{false};

    // Список в памяти ядра: тесты и слои, у которых нет обхода ФС. complete
    // и omitted — для неполного списка (обход прерван или предел путей).
    static std::shared_ptr<const AllowedSet> makeAllowedSet(std::vector<AllowedEntry> entries,
                                                            bool complete = true, std::size_t omitted = 0);

    // Есть ли что удалять по этому манифесту.
    [[nodiscard]] bool deletable() const;
    // Почему удалять нельзя (человекочитаемо: dry-run, журнал, отчёт).
    [[nodiscard]] std::string blockReason() const;
};

// Категории пользовательских данных: элемент без порога размера в них не
// выбирается никогда (SPEC §4 FR-3 «Файлы > 1 ГБ», docs/review-02.md F-02).
bool isUserDataCategory(std::string_view category);

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
    ReviewOffByDefault,  // Review не выбран по умолчанию: уровень включается явно (FR-3, FR-4)
    EstimateOnly,        // правило объявлено «только оценка» (F-04)
    NoSizeThreshold,     // пользовательские данные без порога размера файла (F-02)
    NeedsEnumeration,    // нет списка разрешённых файлов: корень удалять нельзя (F-01)
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

    // Уровень, который отбирается ПО УМОЛЧАНИЮ (SPEC §4 FR-3 «выкл. по
    // умолчанию, с явным подтверждением» для browser.history, prefetch,
    // delivery.opt; FR-4 «Review/Risky по умолчанию выключены»). По умолчанию
    // Safe: Review берётся только явным действием — профилем «выбрать всё» или
    // ручной галочкой, — иначе план сразу после скана обещал удалить пароли
    // браузера и рабочий стол (docs/review-02.md F-02, F-03).
    SafetyLevel maxDefaultSafety{SafetyLevel::Safe};
};

// Решение по одному кандидату: действие, признак «выбран» и человекочитаемая причина.
struct SelectionDecision {
    PlanAction action{PlanAction::Keep};
    bool selected{false};
    SkipReason skip{SkipReason::None};
    std::string reason;
};

// Причины, которые не снимаются ни профилем, ни подтверждением, ни сменой
// уровня: правило объявило «только оценка», у пользовательских данных нет
// порога размера, либо список разрешённых файлов не получен. Ручной выбор их
// не преодолевает (docs/review-02.md F-01..F-04).
bool isRuleBlock(SkipReason reason);

// Чистое решение по одному кандидату — тот же код, что вызывает buildPlan.
// manifest — вторая половина отбора (что правило разрешило удалить); без него
// кандидат, из которого нельзя доказать «удалять всё», не выбирается вовсе.
SelectionDecision decideCandidate(const CleanupCandidate& candidate, const PlanOptions& options,
                                  const CandidateManifest* manifest = nullptr);

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
    // Манифесты кандидатов, по которым принимались решения. Копия списков
    // разрешённого (сами списки разделяются указателем), поэтому план
    // самодостаточен: исполнитель получает из него и решение, и то, что
    // удалять. Отсортированы по candidateIndex.
    std::vector<CandidateManifest> manifests;
    PlanOptions options;
    bool dryRun{true};

    bool empty() const;
    std::size_t operationCount() const;
    const CleanupPlanItem* item(std::size_t candidateIndex) const;
    std::vector<std::size_t> operationIndexes() const;  // индексы кандидатов к удалению
    // Манифест кандидата или nullptr, если сборка его не дала.
    const CandidateManifest* manifestFor(std::size_t candidateIndex) const;

    // Отпечаток содержимого плана: сменились выборы, профиль, порог или состав
    // операций — отпечаток другой, значит прежнего подтверждения dry-run больше нет.
    std::uint64_t planSignature() const;
};

CleanupPlan buildPlan(const std::vector<CleanupCandidate>& candidates, const PlanOptions& options = PlanOptions{},
                      const std::vector<CandidateManifest>* manifests = nullptr);

// Одна строка плана: и в списке операций, и в снимке, и в отчёте.
//
// Два объяснения, а не одно. `evidence` — «почему это мусор» (FR-4: Reasons
// кандидата, список строк от правила); `reason` — «почему такое действие»
// (выбрано по профилю, занято файлами, ниже порога). Показывать только второе
// нельзя: человек перед удалением должен знать и уровень риска (`safety`),
// и то, что именно он сейчас сносит (review-05 F-01).
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
    std::string reason;                // почему такое действие
    std::vector<std::string> evidence;  // почему это мусор (FR-4)

    // Что именно удалит эта операция (docs/review-02.md F-01). При удалении по
    // корню rootDeleteOnly == true и allowedPaths пуст: корень и есть весь
    // список. Иначе здесь начало списка (не длиннее kMaxOperationPaths), а
    // allowedCount — полное число файлов; полный список — в манифесте.
    std::vector<std::string> allowedPaths;
    std::size_t allowedCount{};
    bool rootDeleteOnly{true};
};

// Человекочитаемое объяснение строки операции одним куском: сначала «почему это
// мусор», затем «почему такое действие». Одинаково для UI, CLI и журнала —
// расходиться тут нечем, а пустая строка означала бы элемент без объяснения
// (SPEC §12).
std::string operationReason(const PlanOperation& op);

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
