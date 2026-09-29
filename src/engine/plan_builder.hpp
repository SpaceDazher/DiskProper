// Экран «Очистка»: выборы пользователя, агрегаты, обязательный dry-run и снимок
// плана (SPEC §4 FR-5 «План очистки», §4 FR-4 «Risky скрыт по умолчанию»,
// §6.2 engine::*, §6.3 модель, §12 «ни один элемент не удаляется без объяснения»).
//
// ---------------------------------------------------------------------------
// Граница слоёв
// ---------------------------------------------------------------------------
//
// Что уже есть ниже. core::plan (задача 55) — чистая функция:
// (кандидаты, core::PlanOptions) -> core::CleanupPlan, плюс makeDryRunReport,
// makeSnapshot, planToJson, validatePlan и DryRunGate. Решение «что и почему
// удаляем» принимает ровно одна функция — core::decideCandidate, и это
// единственное место, где живут правила отбора (FR-4, FR-5).
//
// Чего не хватает для экрана. У экрана «Очистка» есть СОСТОЯНИЕ, которое живёт
// между вызовами: что человек уже отщёлкнул, какой профиль выбран, показан ли
// Risky, подтверждён ли dry-run именно для этого плана. План — чистая функция,
// а взаимодействие с человеком — нет. Если каждое из трёх мест (UI, CLI,
// исполнитель) соберёт такое состояние само, ответы на один и тот же вопрос
// («сколько освободится, если выбрать всё», «можно ли начинать удаление», «что
// попадёт в снимок») разойдутся — а FR-5 требует, чтобы список операций,
// показанный человеку, и список выполненных операций совпадали.
//
// Модуль PlanBuilder — это и есть состояние экрана поверх чистого плана:
//   * хранит кандидатов (собственная копия), опции отбора и выборы пользователя;
//   * пересобирает план при каждом изменении и публикует его как
//     shared_ptr<const core::CleanupPlan> — тот же приём иммутабельной
//     публикации, что у engine::ScanResult (§6.4: «результаты не мутируются
//     после публикации»): исполнитель держит снимок плана, пока UI продолжает
//     щёлкать чекбоксы;
//   * считает агрегаты для дерева «категория → элемент»;
//   * держит гейт dry-run (FR-5: «dry-run обязателен и запускается по умолчанию
//     перед первым удалением в сессии») и делает снимок состояния перед
//     исполнением с записью в журнал.
//
// ---------------------------------------------------------------------------
// Почему план собирается здесь, а не вызывается core::buildPlan
// ---------------------------------------------------------------------------
//
// core::PlanOptions описывает отбор ПРАВИЛАМИ (профиль, порог, корзина, Risky),
// но не выбор конкретного человека («этот элемент я снял», «этот — добавил»).
// Как только в плане появляется ручной выбор, три функции ядра на нём
// перестают быть точными:
//
//   * core::makeDryRunReport и core::planToJson пересчитывают решение через
//     core::decideCandidate и пропускают элемент, у которого action в плане не
//     совпал с решением по профилю. Для ручного выбора это не «защита от
//     чужого плана», а молчаливое выбрасывание операции, которую человек
//     только что включил — прямое нарушение FR-5 («показывает точный список
//     операций»);
//   * core::makeSnapshot считает состав операций тем же способом.
//
// Поэтому решения остаются в ядре (core::decideCandidate вызывается на каждый
// кандидат), а СБОРКА результата — здесь: один проход строит и core::CleanupPlan,
// и представление для UI из одних и тех же решений, поэтому разойтись они не
// могут. Списком операций, dry-run, снимком и JSON плана с ручными выборами
// тоже занимается этот модуль; core::makeDryRunReport / makeSnapshot /
// planToJson остаются верными для плана без ручных выборов (CLI --plan,
// юнит-тесты W16) и не меняются.
//
// Инвариант из core::plan проверяется здесь явно: результат прогоняется через
// core::validatePlan, и prepareForExecution отказывается стартовать удаление,
// если тот нашёл хоть одно нарушение.
//
// ---------------------------------------------------------------------------
// Инварианты, которые держит rebuild (SPEC §4 FR-5, §6.3)
// ---------------------------------------------------------------------------
//
//   1. Ровно один элемент плана на кандидата, item.candidateIndex == позиция.
//      Индексы кандидатов — единственные ссылки между сканом, планом и
//      отчётом, поэтому порядок кандидатов не переставляется никогда.
//   2. reclaimBytes == allocatedBytes для Delete/Trash и 0 для Keep/SkipLocked
//      (проверяет core::validatePlan).
//   3. Элемент с непустым lockedBy не удаляется НИКОГДА: выбор пользователя его
//      не отменяет, иначе FR-5 «Skip (locked)» превратился бы в «удалить
//      занятое» — с отказом в середине операции и записью в журнал.
//   4. Risky не выбирается, пока пользователь не подтвердил «показать всё»
//      (FR-4, §12). Подтверждение двухшаговое: askRevealRisky() — запрос
//      показать, confirmRiskyReveal(true) — согласие; отказ возвращает false и
//      ничего не меняет.
//   5. Любое изменение выборов, профиля, порога или корзины меняет состав
//      операций, а значит и core::CleanupPlan::planSignature(), и прежнее
//      подтверждение dry-run перестаёт действовать само (core::DryRunGate).
//      Специально «продлевать» подтверждение нельзя: человек подтверждал
//      конкретный список. Граница отпечатка: в него входят действие и
//      reclaimBytes каждого кандидата, поэтому любое изменение состава
//      операций его меняет, а вот перестановка имён в части «не трогаем»
//      (например, кто именно держит уже занятые файлы) — нет: исполняемый
//      список при этом прежний, и подтверждать то же самое ещё раз незачем.
//   6. Ни одна функция не бросает исключений намеренно (SPEC §5 «устойчивость»
//      распространяется и на движок). Единственный источник исключения —
//      std::bad_alloc при выделении строк и векторов.
//
// ---------------------------------------------------------------------------
// Слой
// ---------------------------------------------------------------------------
//
// Файл не включает windows.h: плана достаточно, чтобы собрать его на любом
// хосте (ADR-004, SPEC §11 п.1). Единственный побочный эффект — запись в
// структурный журнал (core::log). Значения, которых движок знать не может
// (PID, версия приложения), приходят в core::PlanSnapshotContext от вызывающего.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/plan.hpp"

namespace mrproper::engine {

// ---------------------------------------------------------------------------
// Выбор пользователя относительно решения по профилю
// ---------------------------------------------------------------------------

enum class SelectionOverride : std::uint8_t {
    Auto,           // решает профиль: core::decideCandidate
    KeptByUser,     // чекбокс снят: элемент остаётся на месте
    SelectedByUser  // чекбокс включён вручную, даже если профиль его не взял
};

const char* toString(SelectionOverride value);

// ---------------------------------------------------------------------------
// Представление для экрана «Очистка» (FR-5)
// ---------------------------------------------------------------------------

// Строка дерева «категория → элемент» вместе с причиной, по которой строка
// выглядит именно так. SPEC §12: элемент без объяснения и уровня риска
// показывать нельзя, поэтому reason заполняется всегда.
struct PlanItemView {
    std::size_t candidateIndex{};
    std::string category;
    std::string displayName;
    std::string path;
    std::uint64_t bytes{};  // аллоцированный объём кандидата: столько освободится, если строка выбрана
    core::SafetyLevel safety{core::SafetyLevel::Review};
    int confidence{};
    core::PlanAction action{core::PlanAction::Keep};
    core::SkipReason skip{core::SkipReason::None};
    SelectionOverride selection{SelectionOverride::Auto};
    bool selected{};  // чекбокс включён — элемент попадёт в операции
    bool visible{};   // строка показывается (Risky скрыт, пока не подтверждён показ)
    bool locked{};    // файлы держит приложение — чекбокс недоступен
    // Что удалится на самом деле (docs/review-02.md F-01): rootDeleteOnly —
    // удаляется каталог целиком (правило ничего не отсекло), иначе элемент
    // удаляется по списку из allowedCount файлов. Чекбокс у такого элемента
    // недоступен, пока список не получен (SkipReason::NeedsEnumeration).
    std::size_t allowedCount{};
    bool rootDeleteOnly{true};
    std::string reason;
};

// Раздел дерева. Частичное выделение — третье состояние чекбокса категории:
// allSelected при selectedCount == selectableCount, partiallySelected между 0 и
// этим числом.
struct PlanCategoryView {
    std::string category;
    std::size_t itemCount{};
    std::size_t selectedCount{};
    std::uint64_t selectedBytes{};
    std::uint64_t selectableBytes{};  // «выбрать всё» в этой категории
    std::uint64_t safeOnlyBytes{};    // «если выбрать только Safe»
    std::size_t selectableCount{};
    std::size_t hiddenRiskyCount{};
    std::size_t lockedCount{};
    bool allSelected{};
    bool partiallySelected{};
};

// Агрегаты плана. Числа «выбрано» и «осталось на месте» считаются по тем же
// решениям, что и элементы плана, поэтому цифры на экране и состав операций
// расходятся быть не могут.
struct PlanTotals {
    std::size_t candidateCount{};
    std::size_t selectedCount{};
    std::uint64_t selectedBytes{};

    // «Выбрать всё» глазами пользователя: всё ВИДИМОЕ и незаблокированное.
    // Отличается от allBytes: скрытые Risky в эту сумму не входят, пока
    // человек их не показал (FR-4, §12).
    std::size_t visibleSelectableCount{};
    std::uint64_t visibleSelectableBytes{};

    // Потолок: всё, что вообще можно удалить, включая скрытый Risky.
    std::size_t allCount{};
    std::uint64_t allBytes{};
    std::size_t safeOnlyCount{};
    std::uint64_t safeOnlyBytes{};

    // Почему элементы остались на месте — те же счётчики, что у core::plan,
    // плюс отдельно оставленное самим пользователем.
    std::size_t lockedCount{};
    std::uint64_t lockedBytes{};
    std::size_t hiddenRiskyCount{};
    std::uint64_t hiddenRiskyBytes{};
    std::size_t belowThresholdCount{};
    std::size_t tooSmallCount{};
    std::size_t profileFilteredCount{};
    std::size_t keptByUserCount{};
    std::uint64_t keptByUserBytes{};

    core::PlanActionTotals byAction;
};

// Три цифры, которые показывает экран (FR-5): выбранное / «выбрать всё» /
// «только Safe», готовой строкой для подписи и для журнала.
struct CleanupSummary {
    std::size_t selectedCount{};
    std::uint64_t selectedBytes{};
    std::size_t selectAllCount{};
    std::uint64_t selectAllBytes{};
    std::size_t safeOnlyCount{};
    std::uint64_t safeOnlyBytes{};
    std::string text;
};

// Снимок состояния экрана: иммутабельный, как и план. UI рисует его, а
// исполнитель берёт из него операции.
struct PlanView {
    std::size_t candidateCount{};
    std::size_t revision{};  // монотонный номер пересборки: «план изменился» без сравнения
    bool dryRun{true};       // FR-5: dry-run включён по умолчанию
    PlanTotals totals;
    std::vector<PlanCategoryView> categories;  // крупные сверху, затем по имени
    std::vector<PlanItemView> items;           // в порядке кандидатов
    std::string headline;                      // «Освободится 1,2 ГБ — 214 элементов из 340»
};

// ---------------------------------------------------------------------------
// Гейт dry-run и снимок перед исполнением (FR-5)
// ---------------------------------------------------------------------------

// Результат prepareForExecution: можно ли начинать удаление и что уходит в
// снимок. Проблемы заполняются всегда (их видно в журнале и в отчёте), снимок —
// только когда ready.
struct PlanPreparation {
    bool ready{};
    std::vector<std::string> problems;
    core::PlanSnapshot snapshot;
    std::string dryRunText;
    std::size_t operationCount{};
    std::uint64_t totalBytes{};
    std::uint64_t planSignature{};
};

// ---------------------------------------------------------------------------
// PlanBuilder
// ---------------------------------------------------------------------------

// Не потокобезопасен и не должен быть: его ведёт один поток (UI-поток, §6.4).
// Потокобезопасная часть — публикация: publishedPlan() отдаёт константную
// ссылку, которую можно держать сколько угодно.
class PlanBuilder {
public:
    PlanBuilder() { rebuild(); }
    PlanBuilder(std::vector<core::CleanupCandidate> candidates, core::PlanOptions options = core::PlanOptions{});

    // Новый скан: состав кандидатов сменился, ручные выборы прошлого экрана к
    // новым элементам не относятся и сбрасываются. Подтверждение dry-run тоже:
    // список операций другой.
    void setCandidates(std::vector<core::CleanupCandidate> candidates);

    // Манифесты скана: что каждому кандидату разрешено удалять
    // (docs/review-02.md F-01..F-04). Без них план справедливо не выбирает
    // ничего: корень удалять нельзя, пока не доказано, что отбирать нечего.
    // Списки файлов разделяются указателем — копии не делается.
    void setManifests(std::vector<core::CandidateManifest> manifests);
    void setCandidates(std::vector<core::CleanupCandidate> candidates,
                       std::vector<core::CandidateManifest> manifests);
    [[nodiscard]] const core::CandidateManifest* manifestFor(std::size_t candidateIndex) const noexcept;

    [[nodiscard]] const std::vector<core::CleanupCandidate>& candidates() const noexcept { return candidates_; }
    [[nodiscard]] bool empty() const noexcept { return candidates_.empty(); }

    // ---- параметры отбора (профиль, порог, корзина, минимальный объём) ----
    void setOptions(core::PlanOptions options);
    [[nodiscard]] const core::PlanOptions& options() const noexcept { return options_; }
    void setProfile(core::SelectionProfile profile);
    void setConfidenceThreshold(int threshold);
    void setUseTrash(bool useTrash);
    void setMinReclaimBytes(std::uint64_t bytes);

    // Потолок уровня по умолчанию (SPEC §4 FR-3, FR-4; docs/review-02.md F-03):
    // Safe — по умолчанию, Review — только явным действием человека (эта
    // функция, то есть галочка в настройках) или профилем «выбрать всё».
    // Выше Review не поднимается: Risky включается отдельно, двойным
    // подтверждением (askRevealRisky + confirmRiskyReveal, §9).
    void setMaxDefaultSafety(core::SafetyLevel level);
    [[nodiscard]] core::SafetyLevel maxDefaultSafety() const noexcept { return options_.maxDefaultSafety; }

    // ---- Risky: показать можно только через «показать всё» с подтверждением
    //      (FR-4, §12) ----
    [[nodiscard]] bool riskyRevealPending() const noexcept { return riskyRevealPending_; }
    [[nodiscard]] bool riskyRevealed() const noexcept { return options_.allowRisky; }
    void askRevealRisky();
    // true — человек согласился и Risky показан; false — отказ, состояние
    // прежнее. Повторное подтверждение при уже показанном Risky тоже честно
    // возвращает true: показать уже показано.
    [[nodiscard]] bool confirmRiskyReveal(bool accepted);
    void hideRisky();

    // ---- выборы пользователя ----
    // false означает «элемент нельзя поменять», а не «сбой»: индекс вне
    // диапазона, файлы заняты, скрытый Risky. Причина видна в reason строки.
    [[nodiscard]] bool setSelected(std::size_t candidateIndex, bool selected);
    [[nodiscard]] bool clearOverride(std::size_t candidateIndex);
    // «Выбрать всё» и «снять всё» в дереве: галочки на видимых незаблокированных
    // строках, скрытые Risky и занятые элементы не трогаются.
    void selectAllVisible();
    // «Только Safe» (третья цифра агрегатов, FR-5): Safe — выбраны, видимые
    // остальные — сняты, скрытый Risky и занятые не трогаются.
    void selectSafeOnly();
    void selectNone();
    [[nodiscard]] bool setCategorySelected(std::string_view category, bool selected);
    // Вернуть элемент к решению профиля.
    void clearOverrides();
    [[nodiscard]] SelectionOverride overrideFor(std::size_t candidateIndex) const noexcept;

    // ---- результат ----
    [[nodiscard]] const core::CleanupPlan& plan() const noexcept { return plan_; }
    [[nodiscard]] const PlanView& view() const noexcept { return view_; }
    // План для исполнителя: константная ссылка, переживает дальнейшие пересборки.
    [[nodiscard]] std::shared_ptr<const core::CleanupPlan> publishedPlan() const noexcept { return published_; }
    // Указатели на элементы представления действительны до следующей пересборки
    // (любой смены выборов или опций): тогда нужен index(), а не сохранённый
    // указатель. Долгоживущая ссылка для исполнителя — publishedPlan().
    [[nodiscard]] const PlanItemView* item(std::size_t candidateIndex) const noexcept;
    [[nodiscard]] const PlanCategoryView* category(std::string_view name) const noexcept;
    [[nodiscard]] CleanupSummary summary() const;
    // Ровно то, что будет выполнено, в порядке кандидатов.
    [[nodiscard]] std::vector<core::PlanOperation> operations() const;
    // FR-5: точный список операций перед первым удалением в сессии.
    [[nodiscard]] core::DryRunReport dryRun() const;
    // Тот же список текстом: UI, CLI, журнал.
    [[nodiscard]] std::string toText() const;
    // Детерминированный JSON (SPEC §11.4, golden-тесты): ключи в фиксированном
    // порядке, состав — с ручными выборами.
    [[nodiscard]] std::string toJson() const;

    // ---- гейт dry-run и снимок (FR-5) ----
    // Новая сессия приложения: показываем dry-run заново, даже если план не
    // менялся с прошлого запуска.
    void beginSession();
    [[nodiscard]] bool dryRunConfirmed() const noexcept;
    [[nodiscard]] bool mustConfirmDryRun() const;
    void confirmDryRun();
    // Снимок состояния перед исполнением: список операций, PID, версия, размер.
    // Проверяет гейт dry-run и core::validatePlan; при проблемах снимок НЕ
    // делается, а причины возвращаются вызывающему и пишутся в журнал.
    [[nodiscard]] PlanPreparation prepareForExecution(const core::PlanSnapshotContext& context);

    // Нарушения инвариантов: core::validatePlan плюс собственные проверки
    // (выборы вне диапазона, счётчики представления).
    [[nodiscard]] std::vector<std::string> validate() const;

private:
    void rebuild();
    [[nodiscard]] core::SelectionDecision decideWithOverride(std::size_t candidateIndex,
                                                             SelectionOverride selection) const;
    // Запрещено ли правилом брать элемент: «только оценка», отсутствие порога
    // размера у пользовательских данных, неполный список разрешённого
    // (docs/review-02.md F-01..F-04). Ни «выбрать всё», ни галочка категории
    // такой запрет не снимают.
    [[nodiscard]] bool blockedByRule(std::size_t candidateIndex) const;

    std::vector<core::CleanupCandidate> candidates_;
    std::vector<core::CandidateManifest> manifests_;  // параллелен candidates_ по candidateIndex
    std::vector<SelectionOverride> overrides_;  // параллелен candidates_; пусто = Auto
    core::PlanOptions options_;
    core::CleanupPlan plan_;
    PlanView view_;
    std::shared_ptr<const core::CleanupPlan> published_;
    core::DryRunGate gate_;
    bool riskyRevealPending_{false};
    std::size_t revision_{0};
};

}  // namespace mrproper::engine
