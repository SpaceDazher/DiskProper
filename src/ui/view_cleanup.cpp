// MrProper — экран «Очистка»: дерево категорий, чекбоксы, агрегаты, прогресс, отмена.
//
// Разбор решений — в view_cleanup.hpp. Здесь только код, в порядке заголовка:
// каталог категорий → состояния → агрегаты → раскладка → модель → окно.

#include "view_cleanup.hpp"

#include <commctrl.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM: позиция мыши в WM_LBUTTONUP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "theme.hpp"

namespace mrproper::ui::cleanup {
namespace {

using core::CleanupCandidate;
using core::PlanAction;
using core::SafetyLevel;
using core::SkipReason;

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------
//
// Макросы MRP_LOG_* из core/log.hpp непригодны: logFieldList раскрывает пакет в
// вызов logField по одному аргументу, поэтому любое поле даёт C2661, и макрос
// компилируется только вовсе без полей. Собираем поля явно — тем же способом,
// что и app_shell.cpp (дефект в чужом файле из слоя ui править нельзя).
void logEvent(core::LogLevel level, std::string_view event, std::string_view message) noexcept {
    core::Logger::instance().write(level, event, message, core::LogFields{});
}

void logWin32(std::string_view event, std::string_view where, unsigned long code) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField("where", where));
    fields.push_back(core::logField("code", code));
    core::Logger::instance().write(core::LogLevel::Warn, event, "Win32 call failed", std::move(fields));
}

// ---------------------------------------------------------------------------
// Строки и ключи
// ---------------------------------------------------------------------------

// Склейка списка: имена процессов, удерживающих файлы (FR-4 LockedBy), причины
// («почему это мусор», FR-4). Разделитель у обоих языков один: список имён
// процессов не переводится.
std::string joinWith(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.append(separator);
        out.append(parts[i]);
    }
    return out;
}

std::string sizeText(std::uint64_t bytes) { return formatBytes(bytes); }
std::string countText(std::size_t value) { return formatCount(static_cast<std::uint64_t>(value)); }

// Смещение «сейчас» для возраста файла. Модель хранит время как unix-секунды
// (§6.3), и «сколько лет кандидату» считается от текущего момента, а не от
// времени последней записи в журнал.
std::int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Запись выбора — всегда 0 или 1, и приведение написано явно: narrowing-конверсия
// в uint8_t ловится MSVC как C4244, а /WX превращает её в ошибку сборки.
constexpr std::uint8_t kUnchecked = 0;
constexpr std::uint8_t kChecked = 1;

// ASCII-приведение к нижнему регистру для строки фильтра. Русские буквы не
// трогаются: без таблицы регистров их не свернуть, и ввод «КЭШ» в верхнем
// регистре не найдёт «Кэш». Это осознанное ограничение фильтра, а не поломка
// поиска: дерево показывает все категории, пока строка пуста.
std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

bool matchesFilter(std::string_view filter, const std::vector<std::string>& haystacks) {
    const std::string needle = asciiLower(filter);
    if (needle.empty()) return true;
    for (const std::string& text : haystacks) {
        if (asciiLower(text).find(needle) != std::string::npos) return true;
    }
    return false;
}

// Ключи узлов. Ключ, а не индекс: индекс меняется при новом скане, а состояние
// страницы (PageState) обязано пережить и скан, и перезапуск.
std::string itemKeyOf(std::size_t candidateIndex) { return "item:" + std::to_string(candidateIndex); }

std::string categoryKeyOf(std::string_view category) { return "cat:" + std::string(category); }

constexpr std::string_view kItemKeyPrefix = "item:";
constexpr std::string_view kCategoryKeyPrefix = "cat:";

std::optional<std::size_t> candidateIndexFromKey(std::string_view key) {
    if (key.size() <= kItemKeyPrefix.size()) return std::nullopt;
    if (key.substr(0, kItemKeyPrefix.size()) != kItemKeyPrefix) return std::nullopt;
    std::size_t value = 0;
    for (std::size_t i = kItemKeyPrefix.size(); i < key.size(); ++i) {
        const char ch = key[i];
        if (ch < '0' || ch > '9') return std::nullopt;
        // Ключ приходит из строки состояния, то есть из файла настроек, который
        // человек мог править руками (FR-9): длина строки ограничена, чтобы
        // разбор не ушёл в переполнение размера.
        if (value > (static_cast<std::size_t>(1) << 40U) / 10U) return std::nullopt;
        value = value * 10U + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

// Человекочитаемое имя кандидата: displayName из правила (FR-4), иначе хвост
// пути, иначе сам путь. Подписи без узла быть не может — строка без имени
// неотличима от мусора в интерфейсе.
std::string candidateLabel(const CleanupCandidate& candidate) {
    if (!candidate.displayName.empty()) return candidate.displayName;
    const std::size_t slash = candidate.path.find_last_of("/\\");
    if (slash != std::string::npos && slash + 1 < candidate.path.size()) {
        return candidate.path.substr(slash + 1);
    }
    return candidate.path;
}

CheckState checkStateOf(std::size_t selectable, std::size_t selected) noexcept {
    if (selected == 0) return CheckState::Unchecked;
    if (selectable != 0 && selected >= selectable) return CheckState::Checked;
    return CheckState::Partial;
}

std::string safetyText(SafetyLevel level) {
    switch (level) {
    case SafetyLevel::Safe: return tr(StringId::kSafetySafe);
    case SafetyLevel::Review: return tr(StringId::kSafetyReview);
    case SafetyLevel::Risky: return tr(StringId::kSafetyRisky);
    }
    return tr(StringId::kCommonUnknown);
}

// Уровень риска целиком: слово уровня плюс подсказка для Risky. Одного слова
// «Рискованно» мало — человек должен понимать, что будет с элементом по
// умолчанию (FR-4, §12). Общая функция для подписи узла и для строки dry-run:
// уровень риска в двух местах обязан звучать одинаково.
std::string riskText(SafetyLevel level) {
    std::string text = safetyText(level);
    if (level == SafetyLevel::Risky) {
        text += " — ";
        text += tr(StringId::kSafetyRiskyHint);
    }
    return text;
}

// «Почему это мусор» одной строкой: правило может дать несколько причин, а в
// строке dry-run они читаются как один список, а не как три отдельных куска.
std::string joinReasons(const std::vector<std::string>& reasons) {
    std::string text;
    for (const std::string& reason : reasons) {
        if (reason.empty()) continue;
        if (!text.empty()) text += "; ";
        text += reason;
    }
    return text;
}

// Текст действия для таблицы dry-run. Отдельных строк «удалить»/«в корзину» в
// каталоге нет, но ключи с точным смыслом есть, а FR-5 различает Delete и
// Trash: человек должен видеть, куда денется файл.
std::string actionText(PlanAction action) {
    switch (action) {
    case PlanAction::Delete: return tr(StringId::kCleanupDeleteLabel);
    case PlanAction::Trash: return tr(StringId::kCleanupTrashLabel);
    case PlanAction::Keep: return tr(StringId::kCommonNone);
    case PlanAction::SkipLocked: return tr(StringId::kCommonLocked);
    }
    return tr(StringId::kCommonNone);
}

// Высота строки дерева — оценка, а не измерение: SysTreeView32 умеет измерить
// сам, но раскладка решает по этому числу, сколько места оставить под дерево.
// Оценка нужна только затем, чтобы дерево не занимало пол-окна при трёх
// кандидатах.
constexpr double kTreeRowHeightDip = 22.0;

}  // namespace

// ---------------------------------------------------------------------------
// Каталог категорий (SPEC §4 FR-3)
// ---------------------------------------------------------------------------

std::size_t categoryOrder(std::string_view categoryId) noexcept {
    for (std::size_t i = 0; i < kCategoryOrder.size(); ++i) {
        if (kCategoryOrder[i] == categoryId) return i;
    }
    return kCategoryOrder.size();
}

std::string categoryTitle(std::string_view categoryId) {
    const std::string key = categoryKey(categoryId);
    if (!key.empty()) return tr(key);
    return std::string(categoryId);
}

const char* toString(CheckState state) noexcept {
    switch (state) {
    case CheckState::Unchecked: return "unchecked";
    case CheckState::Partial: return "partial";
    case CheckState::Checked: return "checked";
    }
    return "unchecked";
}

const char* toString(ScreenState state) noexcept {
    switch (state) {
    case ScreenState::Idle: return "idle";
    case ScreenState::Scanning: return "scanning";
    case ScreenState::Planning: return "planning";
    case ScreenState::DryRun: return "dryRun";
    case ScreenState::Ready: return "ready";
    case ScreenState::Running: return "running";
    case ScreenState::Cancelling: return "cancelling";
    case ScreenState::Cancelled: return "cancelled";
    case ScreenState::Done: return "done";
    case ScreenState::Failed: return "failed";
    }
    return "idle";
}

bool isCleanupControl(WORD controlId) noexcept {
    return controlId >= static_cast<WORD>(ControlId::First) &&
           controlId <= static_cast<WORD>(ControlId::Last);
}

// ---------------------------------------------------------------------------
// Агрегаты и прогресс
// ---------------------------------------------------------------------------

double CleanupAggregates::selectedFraction() const noexcept {
    if (allBytes == 0) return 0.0;
    const double fraction = static_cast<double>(selectedBytes) / static_cast<double>(allBytes);
    if (fraction <= 0.0) return 0.0;
    if (fraction >= 1.0) return 1.0;
    return fraction;
}

double CleanupProgress::fraction() const noexcept {
    if (totalOperations == 0) return 0.0;
    const double done = static_cast<double>(completedOperations + failedOperations);
    const double all = static_cast<double>(totalOperations);
    if (done <= 0.0) return 0.0;
    if (done >= all) return 1.0;
    return done / all;
}

bool CleanupProgress::indeterminate() const noexcept {
    if (state == ScreenState::Scanning || state == ScreenState::Planning) return true;
    // Отменённый скан: операций исполнения ещё ноль, и отсчитывать по ним
    // нечего. Диапазон 0…0 на полосе рисуется как заполненная или как пустая
    // в зависимости от версии comctl32, то есть сообщает о прогрессе, которого
    // нет. До полной остановки полоса остаётся бегунком.
    return state == ScreenState::Cancelling && totalOperations == 0;
}

bool CleanupProgress::busy() const noexcept {
    return state == ScreenState::Scanning || state == ScreenState::Planning ||
           state == ScreenState::Running || state == ScreenState::Cancelling;
}

bool CleanupProgress::cancelAvailable() const noexcept {
    return state == ScreenState::Scanning || state == ScreenState::Running;
}

bool CleanupProgress::canStart() const noexcept {
    return state == ScreenState::Idle || state == ScreenState::Ready || state == ScreenState::Done ||
           state == ScreenState::Cancelled || state == ScreenState::Failed;
}

// ---------------------------------------------------------------------------
// Раскладка
// ---------------------------------------------------------------------------

bool CleanupRect::empty() const noexcept { return width <= 0 || height <= 0; }

bool CleanupRect::contains(int px, int py) const noexcept {
    if (empty()) return false;
    return px >= x && px < x + width && py >= y && py < y + height;
}

int CleanupLayout::gapPx(int rowHeight) noexcept { return std::max(2, rowHeight / 6); }

CleanupRect CleanupLayout::buttonInRow(const CleanupRect& row, int index, int count, int gap,
                                      int rowTop) noexcept {
    if (count <= 0 || row.width <= 0) return CleanupRect{};
    const int cell = row.width / count;
    const int extra = row.width - cell * count;
    const int clamped = std::clamp(index, 0, count - 1);
    const int x = row.x + clamped * cell + std::min(clamped, extra);
    const int width = cell + (clamped < extra ? 1 : 0);
    return CleanupRect{x, rowTop, std::max(0, width - gap), std::max(0, row.height)};
}

CleanupLayout CleanupLayout::compute(const CleanupMetrics& metrics, int dpi, int clientWidthPx,
                                     int clientHeightPx, bool dryRunVisible, int catalogHeightPx) {
    CleanupLayout out;
    out.width_ = std::max(0, clientWidthPx);
    out.height_ = std::max(0, clientHeightPx);
    out.dryRunVisible_ = dryRunVisible;

    const theme::Metrics scale = theme::metricsForDpi(dpi > 0 ? static_cast<unsigned>(dpi) : kDefaultDpi);
    const auto px = [&scale](double dip) { return dip <= 0.0 ? 0 : scale.dip(dip); };

    out.cramped_ = out.width_ < px(metrics.minWidthDip) || out.height_ < px(metrics.minHeightDip);

    // Сверху вниз: агрегаты, полоса прогресса, дерево (единственная гибкая
    // часть). Кнопки прижаты к низу, панель dry-run и пояснение — над ними.
    // Кнопка «Очистить сейчас» не должна прыгать при каждой смене числа строк,
    // поэтому снизу всё зафиксировано, а двигается только дерево.
    out.summary_ = std::min(out.height_, px(metrics.summaryHeightDip));
    out.progress_ = std::min(out.height_ - out.summary_, px(metrics.progressHeightDip));
    out.toolbar_ = std::min(out.height_, px(metrics.toolbarHeightDip));
    out.dryRun_ = dryRunVisible ? std::min(out.height_, px(metrics.dryRunHeightDip)) : 0;

    const int top = out.summary_ + out.progress_;
    int bottom = out.height_ - out.toolbar_ - out.dryRun_;
    if (bottom < top) bottom = top;
    out.detailsTop_ = bottom;

    const int minTree = px(metrics.minTreeHeightDip);
    int tree = std::max(0, out.detailsTop_ - top);
    if (catalogHeightPx > 0 && tree > catalogHeightPx) tree = std::min(tree, std::max(catalogHeightPx, minTree));
    if (tree < minTree) tree = std::min(minTree, std::max(0, out.detailsTop_ - top));
    out.treeBottom_ = top + tree;

    const int detailsWanted = px(metrics.detailsHeightDip);
    out.details_ = std::clamp(detailsWanted, 0, std::max(0, out.detailsTop_ - out.treeBottom_));
    out.dryRunTop_ = out.treeBottom_ + out.details_;
    return out;
}

CleanupRect CleanupLayout::summaryRect() const noexcept { return CleanupRect{0, 0, width_, summary_}; }

CleanupRect CleanupLayout::progressRect() const noexcept {
    return CleanupRect{0, summary_, width_, progress_};
}

CleanupRect CleanupLayout::treeRect() const noexcept {
    return CleanupRect{0, summary_ + progress_, width_, treeBottom_ - summary_ - progress_};
}

CleanupRect CleanupLayout::detailsRect() const noexcept { return CleanupRect{0, treeBottom_, width_, details_}; }

CleanupRect CleanupLayout::dryRunRect() const noexcept { return CleanupRect{0, dryRunTop_, width_, dryRun_}; }

CleanupRect CleanupLayout::toolbarRect() const noexcept {
    return CleanupRect{0, std::max(0, height_ - toolbar_), width_, toolbar_};
}

CleanupRect CleanupLayout::cleanButtonRect() const noexcept {
    const CleanupRect row = toolbarRect();
    return buttonInRow(row, 0, 4, gapPx(row.height), row.y);
}

CleanupRect CleanupLayout::cancelButtonRect() const noexcept {
    const CleanupRect row = toolbarRect();
    return buttonInRow(row, 1, 4, gapPx(row.height), row.y);
}

CleanupRect CleanupLayout::rescanButtonRect() const noexcept {
    const CleanupRect row = toolbarRect();
    return buttonInRow(row, 2, 4, gapPx(row.height), row.y);
}

CleanupRect CleanupLayout::undoButtonRect() const noexcept {
    const CleanupRect row = toolbarRect();
    return buttonInRow(row, 3, 4, gapPx(row.height), row.y);
}

CleanupRect CleanupLayout::selectAllButtonRect() const noexcept {
    const CleanupRect row = toolbarRect();
    return buttonInRow(row, 0, 3, gapPx(row.height), row.y + row.height / 2);
}

CleanupRect CleanupLayout::clearSelectionButtonRect() const noexcept {
    const CleanupRect row = toolbarRect();
    return buttonInRow(row, 1, 3, gapPx(row.height), row.y + row.height / 2);
}

CleanupRect CleanupLayout::showAllButtonRect() const noexcept {
    const CleanupRect row = toolbarRect();
    return buttonInRow(row, 2, 3, gapPx(row.height), row.y + row.height / 2);
}

CleanupRect CleanupLayout::dryRunCloseRect() const noexcept {
    const CleanupRect panel = dryRunRect();
    const int side = panel.height / 3;
    return CleanupRect{std::max(panel.x, panel.x + panel.width - side - 4), panel.y + 2, side, side};
}

bool CleanupLayout::cramped() const noexcept { return cramped_; }
int CleanupLayout::clientWidthPx() const noexcept { return width_; }
int CleanupLayout::clientHeightPx() const noexcept { return height_; }
bool CleanupLayout::dryRunVisible() const noexcept { return dryRunVisible_ && dryRun_ > 0; }

HitTarget CleanupLayout::hitTest(int px, int py) const noexcept {
    if (cramped_) return HitTarget::None;
    // Порядок проверки — от частного к общему: кнопки лежат внутри строки
    // инструментов, панель dry-run — между деревом и строкой, и без порядка
    // первое же совпадение отдало бы щелчок не туда.
    if (cleanButtonRect().contains(px, py)) return HitTarget::Clean;
    if (cancelButtonRect().contains(px, py)) return HitTarget::Cancel;
    if (rescanButtonRect().contains(px, py)) return HitTarget::Rescan;
    if (showAllButtonRect().contains(px, py)) return HitTarget::ShowAll;
    if (selectAllButtonRect().contains(px, py)) return HitTarget::SelectAll;
    if (clearSelectionButtonRect().contains(px, py)) return HitTarget::ClearSelection;
    if (undoButtonRect().contains(px, py)) return HitTarget::Undo;
    if (dryRunVisible() && dryRunCloseRect().contains(px, py)) return HitTarget::DryRunClose;
    if (dryRunVisible() && dryRunRect().contains(px, py)) return HitTarget::DryRun;
    if (treeRect().contains(px, py)) return HitTarget::Tree;
    if (detailsRect().contains(px, py)) return HitTarget::Details;
    if (progressRect().contains(px, py)) return HitTarget::Progress;
    if (summaryRect().contains(px, py)) return HitTarget::Summary;
    return HitTarget::None;
}

// ---------------------------------------------------------------------------
// Модель
// ---------------------------------------------------------------------------

struct CleanupViewModel::Impl {
    // Снимок скана (§6.4: результаты не мутируются после публикации).
    std::shared_ptr<const std::vector<CleanupCandidate>> candidates;
    // Что каждому кандидату разрешено удалять (docs/review-02.md F-01).
    // nullptr — сборщик манифестов не подключён, и план тогда ничего не
    // выбирает: обещать удаление корня, не сказав что внутри, нельзя.
    std::shared_ptr<const std::vector<core::CandidateManifest>> manifests;
    core::PlanOptions options;
    bool showAllRisky{false};
    std::string filter;
    std::vector<CategoryNode> categories;
    std::vector<ItemNode> items;
    std::vector<std::uint8_t> selected;  // по индексам кандидатов
    std::vector<std::string> expanded;
    std::string focusKey;
    CleanupAggregates aggregates;
    CleanupProgress progress;
    ScanProgress scan;  // счётчики прохода; см. CleanupViewModel::scanProgress()
    core::CleanupPlan profilePlan;
    core::DryRunGate gate;
    std::string error;
    bool undoAvailable{false};
};

CleanupViewModel::CleanupViewModel() : impl_(std::make_unique<Impl>()) {
    // FR-5: dry-run обязателен и включается по умолчанию.
    impl_->options.dryRun = true;
}

core::PlanOptions CleanupViewModel::effectiveOptions() const {
    const auto& s = *impl_;
    core::PlanOptions options;
    // Решение по кандидату, который человек выбрал сам, не должно снова
    // спрашивать профиль и порог уверенности: явный выбор — это и есть
    // подтверждение. Порог и профиль остаются в агрегатах, но не в том, что
    // реально удалится.
    options.profile = core::SelectionProfile::Everything;
    options.confidenceThreshold = 0;
    options.allowRisky = s.options.allowRisky;
    options.useTrash = s.options.useTrash;
    options.trashDirectDeleteAboveBytes = s.options.trashDirectDeleteAboveBytes;
    options.minReclaimBytes = 0;
    options.dryRun = true;
    return options;
}

void CleanupViewModel::rebuild(bool keepSelection) {
    auto& s = *impl_;
    s.categories.clear();
    s.items.clear();
    s.aggregates = CleanupAggregates{};
    if (!s.candidates) {
        s.profilePlan = core::CleanupPlan{};
        return;
    }

    const std::vector<CleanupCandidate>& all = *s.candidates;
    s.options.dryRun = true;
    // Манифесты идут в план вместе с кандидатами: без них buildPlan не выберет
    // ничего (docs/review-02.md F-01), и экран показал бы «нечего удалять» при
    // полном дереве.
    const std::vector<core::CandidateManifest>* manifests = s.manifests != nullptr ? s.manifests.get() : nullptr;
    s.profilePlan = core::buildPlan(all, s.options, manifests);
    if (s.selected.size() != all.size()) s.selected.assign(all.size(), kUnchecked);

    const core::PlanOptions effective = effectiveOptions();
    std::size_t selectedCount = 0;
    std::uint64_t selectedBytes = 0;
    std::size_t lockedCount = 0;
    std::size_t riskyLocked = 0;

    for (std::size_t i = 0; i < all.size(); ++i) {
        const CleanupCandidate& candidate = all[i];
        // Решение по профилю — из ядра (core::plan владеет отбором, §6.2).
        const core::CandidateManifest* manifest = manifestOf(i);
        const core::SelectionDecision decision = core::decideCandidate(candidate, s.options, manifest);
        const bool locked = decision.action == PlanAction::SkipLocked || !candidate.lockedBy.empty();
        const bool risky = candidate.safety == SafetyLevel::Risky;
        const bool riskyHidden = risky && !s.showAllRisky;
        const std::string label = candidateLabel(candidate);
        const bool visible =
            !riskyHidden && matchesFilter(s.filter, {label, candidate.path, candidate.category});
        // Risky без второго подтверждения остаётся видимым, но невыбираемым
        // (FR-4 «скрыт по умолчанию, показать можно только через „показать все“,
        // с подтверждением»; §9 — двойное подтверждение для Risky).
        const bool selectable = visible && !locked && (!risky || s.options.allowRisky);

        if (!keepSelection) s.selected[i] = (decision.selected && selectable) ? kChecked : kUnchecked;
        // Выбор, сделанный для прежнего состава кандидатов, не должен утащить
        // за собой занятый файл: выбор снимается, а узел остаётся видимым.
        if (s.selected[i] == kChecked && !selectable) s.selected[i] = kUnchecked;
        const bool checked = s.selected[i] == kChecked;

        if (locked) ++lockedCount;
        if (risky && !selectable) ++riskyLocked;

        if (checked) {
            ++selectedCount;
            const core::SelectionDecision chosen = core::decideCandidate(candidate, effective, manifest);
            if (chosen.selected) selectedBytes += candidate.allocatedBytes;
        }
        if (!visible) continue;

        ItemNode node;
        node.candidateIndex = i;
        node.key = itemKeyOf(i);
        node.label = label;
        node.path = candidate.path;
        node.category = candidate.category;
        node.reason = joinWith(candidate.reasons, "; ");
        std::vector<std::string> processes;
        processes.reserve(candidate.lockedBy.size());
        for (const core::ProcessRef& process : candidate.lockedBy) processes.push_back(process.name);
        node.lockedBy = joinWith(processes, ", ");
        node.safety = candidate.safety;
        node.confidence = candidate.confidence;
        node.logicalBytes = candidate.logicalBytes;
        node.allocatedBytes = candidate.allocatedBytes;
        node.fileCount = candidate.fileCount;
        node.oldestWrite = candidate.oldestWrite;
        node.checked = checked;
        node.selectable = selectable;
        node.riskyHidden = riskyHidden;
        if (checked) {
            const core::SelectionDecision chosen = core::decideCandidate(candidate, effective, manifest);
            node.action = chosen.action;
            node.skip = chosen.skip;
            node.bytes = chosen.selected ? candidate.allocatedBytes : 0U;
        } else if (locked) {
            node.action = PlanAction::SkipLocked;
            node.skip = SkipReason::Locked;
            node.bytes = 0U;
        } else {
            // Снятый пользователем выбор — это не «пропуск по причине», а
            // Keep: причина была бы враньём (SkipReason описывает отбор, §6.3).
            node.action = PlanAction::Keep;
            node.skip = SkipReason::None;
            node.bytes = 0U;
        }
        s.items.push_back(std::move(node));
    }

    // Агрегаты по категориям считает ядро; берём оттуда «если выбрать всё» и
    // «только Safe» — своими числами экран разошёлся бы с CLI (§11.4).
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> fromPlan;
    for (const core::CategoryAggregate& aggregate : s.profilePlan.categories) {
        fromPlan[aggregate.category] = {aggregate.allBytes, aggregate.safeOnlyBytes};
    }

    std::map<std::string, std::vector<std::size_t>> grouped;
    for (std::size_t index = 0; index < s.items.size(); ++index) {
        grouped[s.items[index].category].push_back(index);
    }
    std::vector<std::string> order;
    order.reserve(grouped.size());
    for (const auto& entry : grouped) order.push_back(entry.first);
    // Порядок FR-3, а не по убыванию объёма: «каталог категорий» — это список,
    // и человек читал его именно в этом порядке (см. комментарий к
    // kCategoryOrder). Категории вне таблицы идут в конец по имени.
    std::sort(order.begin(), order.end(), [](const std::string& left, const std::string& right) {
        const std::size_t leftOrder = categoryOrder(left);
        const std::size_t rightOrder = categoryOrder(right);
        if (leftOrder != rightOrder) return leftOrder < rightOrder;
        return left < right;
    });

    s.categories.reserve(order.size());
    for (const std::string& category : order) {
        CategoryNode node;
        node.category = category;
        node.key = categoryKeyOf(category);
        node.title = categoryTitle(category);
        node.items = grouped[category];
        node.itemCount = node.items.size();
        const auto known = fromPlan.find(category);
        if (known != fromPlan.end()) {
            node.allBytes = known->second.first;
            node.safeOnlyBytes = known->second.second;
        }
        // Внутри категории — по убыванию объёма: это тот случай, где размер
        // действительно помогает выбрать («что именно здесь большое»).
        std::sort(node.items.begin(), node.items.end(), [&s](std::size_t left, std::size_t right) {
            const std::uint64_t leftBytes = s.items[left].allocatedBytes;
            const std::uint64_t rightBytes = s.items[right].allocatedBytes;
            if (leftBytes != rightBytes) return leftBytes > rightBytes;
            return s.items[left].label < s.items[right].label;
        });
        for (const std::size_t index : node.items) {
            const ItemNode& item = s.items[index];
            node.safety = std::max(node.safety, item.safety);
            if (item.checked) {
                ++node.selectedCount;
                node.selectedBytes += item.bytes;
            }
            if (item.selectable) ++node.selectableCount;
        }
        node.check = checkStateOf(node.selectableCount, node.selectedCount);
        s.categories.push_back(std::move(node));
    }

    s.aggregates.candidateCount = all.size();
    s.aggregates.allCount = s.profilePlan.totals.allCount;
    s.aggregates.allBytes = s.profilePlan.totals.allBytes;
    s.aggregates.safeOnlyCount = s.profilePlan.totals.safeOnlyCount;
    s.aggregates.safeOnlyBytes = s.profilePlan.totals.safeOnlyBytes;
    s.aggregates.belowThresholdCount = s.profilePlan.totals.belowThresholdCount;
    s.aggregates.tooSmallCount = s.profilePlan.totals.tooSmallCount;
    s.aggregates.profileFilteredCount = s.profilePlan.totals.profileFilteredCount;
    s.aggregates.lockedCount = lockedCount;
    // «Скрытых или невыбираемых Risky» — одно число на оба случая: не показан
    // из-за «показать все» и показан, но требующий второго подтверждения. Иначе
    // подпись на кнопке «Показать все» меняла бы смысл, оставаясь той же.
    s.aggregates.hiddenRiskyCount = riskyLocked;
    s.aggregates.selectedCount = selectedCount;
    s.aggregates.selectedBytes = selectedBytes;

    if (!s.focusKey.empty() && findItem(s.focusKey) == nullptr && findCategory(s.focusKey) == nullptr) {
        s.focusKey.clear();
    }
}

const ItemNode* CleanupViewModel::findItem(std::string_view key) const {
    const auto& s = *impl_;
    if (key.substr(0, kCategoryKeyPrefix.size()) == kCategoryKeyPrefix) return nullptr;
    const std::optional<std::size_t> index = candidateIndexFromKey(key);
    if (!index.has_value()) return nullptr;
    for (const ItemNode& node : s.items) {
        if (node.candidateIndex == *index) return &node;
    }
    return nullptr;
}

const CategoryNode* CleanupViewModel::findCategory(std::string_view key) const {
    const auto& s = *impl_;
    if (key.substr(0, kCategoryKeyPrefix.size()) != kCategoryKeyPrefix) return nullptr;
    const std::string category = std::string(key.substr(kCategoryKeyPrefix.size()));
    for (const CategoryNode& node : s.categories) {
        if (node.category == category) return &node;
    }
    return nullptr;
}

const std::vector<CategoryNode>& CleanupViewModel::categories() const noexcept { return impl_->categories; }
const std::vector<ItemNode>& CleanupViewModel::items() const noexcept { return impl_->items; }
std::size_t CleanupViewModel::visibleCategoryCount() const noexcept { return impl_->categories.size(); }
const CleanupAggregates& CleanupViewModel::aggregates() const noexcept { return impl_->aggregates; }
const CleanupProgress& CleanupViewModel::progress() const noexcept { return impl_->progress; }
const ScanProgress& CleanupViewModel::scanProgress() const noexcept { return impl_->scan; }
ScreenState CleanupViewModel::state() const noexcept { return impl_->progress.state; }
std::string CleanupViewModel::errorText() const { return impl_->error; }
std::string CleanupViewModel::focusKey() const { return impl_->focusKey; }
std::string CleanupViewModel::filter() const { return impl_->filter; }
core::SelectionProfile CleanupViewModel::profile() const noexcept { return impl_->options.profile; }
bool CleanupViewModel::showAllRisky() const noexcept { return impl_->showAllRisky; }
bool CleanupViewModel::allowRisky() const noexcept { return impl_->options.allowRisky; }
bool CleanupViewModel::useTrash() const noexcept { return impl_->options.useTrash; }
int CleanupViewModel::confidenceThreshold() const noexcept { return impl_->options.confidenceThreshold; }
std::uint64_t CleanupViewModel::minReclaimBytes() const noexcept { return impl_->options.minReclaimBytes; }
std::size_t CleanupViewModel::selectedCount() const noexcept { return impl_->aggregates.selectedCount; }
bool CleanupViewModel::undoAvailable() const noexcept { return impl_->undoAvailable; }
const core::CleanupPlan& CleanupViewModel::profilePlan() const noexcept { return impl_->profilePlan; }

bool CleanupViewModel::hasCandidates() const noexcept {
    return impl_->candidates != nullptr && !impl_->candidates->empty();
}

void CleanupViewModel::publishCandidates(
    std::shared_ptr<const std::vector<CleanupCandidate>> candidates) {
    publishCandidates(std::move(candidates), nullptr);
}

void CleanupViewModel::publishCandidates(
    std::shared_ptr<const std::vector<CleanupCandidate>> candidates,
    std::shared_ptr<const std::vector<core::CandidateManifest>> manifests) {
    auto& s = *impl_;
    s.candidates = std::move(candidates);
    s.manifests = std::move(manifests);
    s.error.clear();
    s.progress = CleanupProgress{};
    s.progress.state = s.candidates != nullptr ? ScreenState::Ready : ScreenState::Idle;
    // Новый скан — новый план: прежнее подтверждение dry-run относилось к
    // прежнему списку операций и не может его разрешать (FR-5).
    s.gate = core::DryRunGate{};
    s.selected.clear();
    s.expanded.clear();
    s.focusKey.clear();
    rebuild(false);
    expandAll();
}

void CleanupViewModel::clear() {
    auto& s = *impl_;
    s.candidates.reset();
    s.manifests.reset();
    s.selected.clear();
    s.expanded.clear();
    s.focusKey.clear();
    s.error.clear();
    s.gate = core::DryRunGate{};
    s.progress = CleanupProgress{};
    s.progress.state = ScreenState::Idle;
    s.scan = ScanProgress{};  // счётчики относятся к этому проходу, а не к следующему
    s.undoAvailable = false;
    rebuild(false);
}

void CleanupViewModel::setProfile(core::SelectionProfile profile) {
    auto& s = *impl_;
    if (s.options.profile == profile) return;
    s.options.profile = profile;
    // Профиль задаёт «по умолчанию» и агрегат «если выбрать всё», но не
    // текущий выбор: человек, снявший галочки, не ждёт, что они вернутся от
    // смены профиля в настройках. Поэтому rebuild идёт с keepSelection.
    rebuild(true);
}

void CleanupViewModel::setShowAllRisky(bool show) {
    auto& s = *impl_;
    if (s.showAllRisky == show) return;
    s.showAllRisky = show;
    rebuild(true);
}

void CleanupViewModel::setAllowRisky(bool allow) {
    auto& s = *impl_;
    if (s.options.allowRisky == allow) return;
    s.options.allowRisky = allow;
    rebuild(true);
}

void CleanupViewModel::setUseTrash(bool useTrash) {
    auto& s = *impl_;
    if (s.options.useTrash == useTrash) return;
    s.options.useTrash = useTrash;
    rebuild(true);
}

void CleanupViewModel::setIncludeReview(bool include) {
    auto& s = *impl_;
    const core::SafetyLevel level = include ? core::SafetyLevel::Review : core::SafetyLevel::Safe;
    if (s.options.maxDefaultSafety == level) return;
    s.options.maxDefaultSafety = level;
    rebuild(true);
}

bool CleanupViewModel::includeReview() const noexcept {
    return impl_->options.maxDefaultSafety >= core::SafetyLevel::Review;
}

void CleanupViewModel::setConfidenceThreshold(int threshold) {
    auto& s = *impl_;
    const int clamped = std::clamp(threshold, 0, 100);
    if (s.options.confidenceThreshold == clamped) return;
    s.options.confidenceThreshold = clamped;
    rebuild(true);
}

void CleanupViewModel::setMinReclaimBytes(std::uint64_t bytes) {
    auto& s = *impl_;
    if (s.options.minReclaimBytes == bytes) return;
    s.options.minReclaimBytes = bytes;
    rebuild(true);
}

void CleanupViewModel::setFilter(std::string_view filter) {
    auto& s = *impl_;
    if (s.filter == filter) return;
    s.filter.assign(filter);
    // Фильтр — вид, а не решение: выбор после него остаётся прежним, иначе
    // набор букв в строке поиска молча менял бы, что будет удалено.
    rebuild(true);
}

void CleanupViewModel::setExpanded(std::string_view key, bool expanded) {
    auto& s = *impl_;
    const auto position = std::find(s.expanded.begin(), s.expanded.end(), key);
    const bool current = position != s.expanded.end();
    if (current == expanded) return;
    if (expanded) {
        s.expanded.emplace_back(key);
    } else {
        s.expanded.erase(position);
    }
}

bool CleanupViewModel::isExpanded(std::string_view key) const noexcept {
    const auto& s = *impl_;
    return std::find(s.expanded.begin(), s.expanded.end(), key) != s.expanded.end();
}

void CleanupViewModel::expandAll() {
    auto& s = *impl_;
    s.expanded.clear();
    s.expanded.reserve(s.categories.size());
    for (const CategoryNode& node : s.categories) {
        if (node.itemCount > 0) s.expanded.push_back(node.key);
    }
}

void CleanupViewModel::collapseAll() {
    impl_->expanded.clear();
}

std::optional<CheckState> CleanupViewModel::nodeCheckState(std::string_view key) const {
    if (const CategoryNode* node = findCategory(key)) return node->check;
    if (const ItemNode* node = findItem(key)) {
        return node->checked ? CheckState::Checked : CheckState::Unchecked;
    }
    return std::nullopt;
}

bool CleanupViewModel::isNodeChecked(std::string_view key) const {
    const std::optional<CheckState> state = nodeCheckState(key);
    return state.has_value() && *state == CheckState::Checked;
}

bool CleanupViewModel::setNodeChecked(std::string_view key, bool checked) {
    auto& s = *impl_;
    if (const CategoryNode* category = findCategory(key)) {
        if (checked && category->selectableCount == 0) return false;
        for (const std::size_t index : category->items) {
            const ItemNode& item = s.items[index];
            if (!item.selectable) continue;
            s.selected[item.candidateIndex] = checked ? kChecked : kUnchecked;
        }
    } else if (const ItemNode* item = findItem(key)) {
        if (checked && !item->selectable) return false;
        s.selected[item->candidateIndex] = checked ? kChecked : kUnchecked;
    } else {
        return false;
    }
    rebuild(true);
    return true;
}

bool CleanupViewModel::toggleNode(std::string_view key) { return setNodeChecked(key, !isNodeChecked(key)); }

bool CleanupViewModel::toggleFocused() {
    const std::string key = impl_->focusKey;
    if (key.empty()) return false;
    return toggleNode(key);
}

void CleanupViewModel::selectAll() {
    auto& s = *impl_;
    for (const ItemNode& item : s.items) {
        s.selected[item.candidateIndex] = item.selectable ? kChecked : kUnchecked;
    }
    rebuild(true);
}

void CleanupViewModel::clearSelection() {
    auto& s = *impl_;
    for (const ItemNode& item : s.items) s.selected[item.candidateIndex] = kUnchecked;
    rebuild(true);
}

void CleanupViewModel::selectSafeOnly() {
    auto& s = *impl_;
    for (const ItemNode& item : s.items) {
        const bool take = item.selectable && item.safety == SafetyLevel::Safe;
        s.selected[item.candidateIndex] = take ? kChecked : kUnchecked;
    }
    rebuild(true);
}

void CleanupViewModel::setFocusKey(std::string_view key) {
    auto& s = *impl_;
    s.focusKey.assign(key);
}

std::vector<std::string> CleanupViewModel::visibleOrder() const {
    const auto& s = *impl_;
    std::vector<std::string> order;
    order.reserve(s.categories.size() + 8);
    for (const CategoryNode& category : s.categories) {
        order.push_back(category.key);
        if (!isExpanded(category.key)) continue;
        for (const std::size_t index : category.items) order.push_back(s.items[index].key);
    }
    return order;
}

bool CleanupViewModel::moveFocus(int delta) {
    auto& s = *impl_;
    const std::vector<std::string> order = visibleOrder();
    if (order.empty() || delta == 0) return false;
    const auto position = std::find(order.begin(), order.end(), s.focusKey);
    std::size_t index = 0;
    if (position == order.end()) {
        // Фокуса нет: вниз — с начала, вверх — с конца, как в списке.
        index = delta > 0 ? 0 : order.size() - 1;
    } else {
        const long current = static_cast<long>(std::distance(order.begin(), position));
        const long next = current + delta;
        if (next < 0 || next >= static_cast<long>(order.size())) return false;
        index = static_cast<std::size_t>(next);
    }
    s.focusKey = order[index];
    return true;
}

bool CleanupViewModel::focusNextVisible() { return moveFocus(1); }
bool CleanupViewModel::focusPreviousVisible() { return moveFocus(-1); }

bool CleanupViewModel::handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown) {
    (void)shiftDown;  // сдвиг в дереве не означает выделение диапазона: его нет
    if (controlDown) {
        // §5 «Клавиатурная навигация, фокус», §7.2 Ctrl+Z.
        if (virtualKey == 'A') {
            selectAll();
            return true;
        }
        if (virtualKey == 'Z') return undo();
    }
    switch (virtualKey) {
    case VK_SPACE:
    case VK_RETURN: return toggleFocused();
    case VK_UP: return moveFocus(-1);
    case VK_DOWN: return moveFocus(1);
    case VK_LEFT: {
        if (const CategoryNode* category = findCategory(impl_->focusKey)) {
            if (isExpanded(category->key)) {
                setExpanded(category->key, false);
                return true;
            }
            return false;
        }
        if (const ItemNode* item = findItem(impl_->focusKey)) {
            impl_->focusKey = categoryKeyOf(item->category);
            return true;
        }
        return false;
    }
    case VK_RIGHT: {
        if (const CategoryNode* category = findCategory(impl_->focusKey)) {
            if (category->itemCount > 0 && !isExpanded(category->key)) {
                setExpanded(category->key, true);
                return true;
            }
            return moveFocus(1);
        }
        return false;
    }
    case VK_ESCAPE: {
        // Escape на панели dry-run закрывает её (список остаётся, он построен
        // заново при следующем запуске), а в дереве снимает выбор.
        if (impl_->progress.state == ScreenState::DryRun) {
            closeDryRun();
            return true;
        }
        clearSelection();
        return true;
    }
    default: break;
    }
    return false;
}

std::string CleanupViewModel::nodeText(std::string_view key) const {
    if (const CategoryNode* category = findCategory(key)) {
        std::string text = category->title;
        text += " — ";
        text += sizeText(category->allBytes);
        text += " (";
        text += trPlural(StringId::kCleanupCandidates, category->itemCount);
        text += ")";
        return text;
    }
    if (const ItemNode* item = findItem(key)) {
        // §7.2: для каждого элемента — объём, возраст и признак занятости.
        std::string text = item->label;
        text += " — ";
        text += sizeText(item->allocatedBytes);
        if (item->fileCount > 0) {
            text += ", ";
            text += trPlural(StringId::kCleanupFiles, item->fileCount);
        }
        if (!item->lockedBy.empty()) {
            text += " — ";
            text += tr(StringId::kCommonLocked);
        } else if (item->safety == SafetyLevel::Risky) {
            text += " — ";
            text += safetyText(SafetyLevel::Risky);
        }
        return text;
    }
    return std::string(key);
}

std::string CleanupViewModel::nodeDetails(std::string_view key) const {
    // Уровень риска идёт первым: он отвечает на вопрос «можно ли это вообще
    // удалять», а объяснение — на вопрос «почему». Подпись уровня считалась и
    // раньше, но на экран не попадала ни разу (review-05 F-04).
    const std::string risk = nodeRiskLabel(key);
    if (const ItemNode* item = findItem(key)) {
        // FR-4: почему это мусор. Пустые причины — дефект правила, и молча
        // показать пустоту хуже, чем сказать «нет данных»: G4 обещает ноль
        // кандидатов «без объяснения».
        std::string text = risk;
        if (!text.empty()) text += "\r\n";
        text += tr(StringId::kCleanupWhyJunk);
        text += ": ";
        text += item->reason.empty() ? tr(StringId::kCommonNone) : item->reason;
        if (!item->path.empty()) {
            text += "\r\n";
            text += item->path;
        }
        if (!item->lockedBy.empty()) {
            text += "\r\n";
            text += tr(StringId::kCleanupLockedBy, item->lockedBy);
        }
        if (item->oldestWrite > 0) {
            const std::int64_t age = std::max<std::int64_t>(0, nowUnixSeconds() - item->oldestWrite);
            text += "\r\n";
            text += tr(StringId::kCleanupOlder, formatAge(age));
        }
        return text;
    }
    if (const CategoryNode* category = findCategory(key)) {
        std::string text = risk;
        if (!text.empty()) text += "\r\n";
        text += selectedText();
        text += " · ";
        text += safeOnlyText();
        if (category->selectableCount == 0) {
            text += " · ";
            text += lockedText();
        }
        return text;
    }
    return reclaimHintText();
}

std::string CleanupViewModel::nodeRiskLabel(std::string_view key) const {
    SafetyLevel level = SafetyLevel::Safe;
    if (const ItemNode* item = findItem(key)) {
        level = item->safety;
    } else if (const CategoryNode* category = findCategory(key)) {
        level = category->safety;
    } else {
        return std::string();
    }
    return riskText(level);
}

std::string CleanupViewModel::summaryText() const {
    return tr(StringId::kCleanupSummary, sizeText(impl_->aggregates.allBytes));
}

std::string CleanupViewModel::selectedText() const {
    return tr(StringId::kCleanupSelected, sizeText(impl_->aggregates.selectedBytes));
}

std::string CleanupViewModel::safeOnlyText() const {
    return tr(StringId::kCleanupSafeOnly, sizeText(impl_->aggregates.safeOnlyBytes));
}

std::string CleanupViewModel::reclaimHintText() const {
    return tr(StringId::kCommonReclaimHint);
}

std::string CleanupViewModel::lockedText() const {
    return tr(StringId::kCleanupLockedCount, countText(impl_->aggregates.lockedCount));
}

std::string CleanupViewModel::statusText() const {
    const auto& s = *impl_;
    switch (s.progress.state) {
    case ScreenState::Idle:
        return s.candidates ? tr(StringId::kCleanupNoCandidates)
                            : tr(StringId::kStatusIdle);
    case ScreenState::Scanning:
    case ScreenState::Planning: {
        std::string text = tr(StringId::kStatusScanning);
        // Счётчики прохода показываем только когда фон их уже прислал: ноль
        // файлов на первом кадре — это «ещё ничего не посмотрено», а не
        // «посмотрено ноль», и рисовать его как результат значило бы врать.
        if (s.scan.filesSeen > 0) {
            text += " · ";
            text += countText(s.scan.filesSeen);
        }
        if (s.scan.bytesSeen > 0) {
            text += " · ";
            text += sizeText(s.scan.bytesSeen);
        }
        return text;
    }
    case ScreenState::DryRun:
        return tr(StringId::kCleanupDryRunNotice) + " · " +
               trPlural(StringId::kCleanupCandidates, s.progress.totalOperations);
    case ScreenState::Ready: return summaryText();
    case ScreenState::Running: {
        std::string text = tr(StringId::kStatusCleaning);
        text += " ";
        text += tr(StringId::kStatusProgress,
                           core::StringArgs{countText(s.progress.completedOperations),
                                            countText(s.progress.totalOperations)});
        if (!s.progress.currentLabel.empty()) {
            text += " · ";
            text += s.progress.currentLabel;
        }
        return text;
    }
    case ScreenState::Cancelling: return tr(StringId::kStatusCancelRequested);
    case ScreenState::Cancelled:
        return tr(StringId::kActionCancel) + ": " +
               trPlural(StringId::kCleanupCandidates, s.progress.completedOperations);
    case ScreenState::Done: {
        std::string text = tr(StringId::kStatusIdle);
        if (s.undoAvailable) {
            text += " · ";
            text += tr(StringId::kCleanupUndoAvailable);
        }
        return text;
    }
    case ScreenState::Failed:
        return tr(StringId::kCommonError) + ": " +
               (s.error.empty() ? tr(StringId::kCommonUnknown) : s.error);
    }
    return std::string();
}

const core::CandidateManifest* CleanupViewModel::manifestOf(std::size_t candidateIndex) const noexcept {
    const auto& s = *impl_;
    if (!s.manifests) return nullptr;
    const std::vector<core::CandidateManifest>& manifests = *s.manifests;
    const auto found = std::lower_bound(manifests.begin(), manifests.end(), candidateIndex,
                                        [](const core::CandidateManifest& manifest, std::size_t index) {
                                            return manifest.candidateIndex < index;
                                        });
    if (found == manifests.end() || found->candidateIndex != candidateIndex) return nullptr;
    return &*found;
}

std::vector<CleanupCandidate> CleanupViewModel::selectedCandidates() const {
    const auto& s = *impl_;
    std::vector<CleanupCandidate> out;
    if (!s.candidates) return out;
    for (const ItemNode& item : s.items) {
        if (!item.checked) continue;
        out.push_back((*s.candidates)[item.candidateIndex]);
    }
    return out;
}

core::CleanupPlan CleanupViewModel::planFor(const std::vector<core::CleanupCandidate>& selected) const {
    const std::vector<core::CandidateManifest> manifests = selectedManifests();
    return core::buildPlan(selected, effectiveOptions(), manifests.empty() ? nullptr : &manifests);
}

std::vector<core::CandidateManifest> CleanupViewModel::selectedManifests() const {
    const auto& s = *impl_;
    std::vector<core::CandidateManifest> out;
    if (!s.candidates) return out;
    // Список разрешённого переносится на позиции сжатого списка выбранного:
    // индекс в effectivePlan — это позиция там, а не во всём списке скана, и
    // без переноса план удалял бы по списку одного элемента для другого (F-01).
    for (const ItemNode& item : s.items) {
        if (!item.checked) continue;
        const core::CandidateManifest* manifest = manifestOf(item.candidateIndex);
        if (manifest == nullptr) continue;
        core::CandidateManifest moved = *manifest;
        moved.candidateIndex = out.size();
        out.push_back(std::move(moved));
    }
    return out;
}

core::CleanupPlan CleanupViewModel::effectivePlan() const {
    return planFor(selectedCandidates());
}

std::vector<std::string> CleanupViewModel::problems() const {
    const std::vector<CleanupCandidate> selected = selectedCandidates();
    return core::validatePlan(selected, planFor(selected));
}

core::DryRunReport CleanupViewModel::dryRunReport() const {
    const std::vector<CleanupCandidate> selected = selectedCandidates();
    return core::makeDryRunReport(selected, planFor(selected));
}

bool CleanupViewModel::dryRunAcknowledged() const { return impl_->gate.alreadyShown(effectivePlan()); }

void CleanupViewModel::acknowledgeDryRun() { impl_->gate.acknowledge(effectivePlan()); }

void CleanupViewModel::closeDryRun() {
    auto& s = *impl_;
    if (s.progress.state != ScreenState::DryRun) return;
    // Закрытие панели не подтверждает план: FR-5 требует именно подтверждения,
    // поэтому следующая попытка снова покажет список операций.
    s.progress.state = s.candidates != nullptr ? ScreenState::Ready : ScreenState::Idle;
    s.progress.currentLabel.clear();
}

void CleanupViewModel::forgetDryRunAcknowledgement() { impl_->gate = core::DryRunGate{}; }

std::vector<CleanupViewModel::DryRunRow> CleanupViewModel::dryRunRows() const {
    std::vector<DryRunRow> rows;
    // FR-5: показываем ровно то, что будет выполнено. «Что останется на месте»
    // (DryRunReport::untouched) в таблицу не идёт: там сотни строк, которые
    // человек удалять не собирался, и они бы скрыли собой то, что он собирался.
    const core::DryRunReport report = dryRunReport();
    rows.reserve(report.operations.size());
    for (const core::PlanOperation& operation : report.operations) {
        DryRunRow row;
        row.category = categoryTitle(operation.category);
        row.item = operation.displayName;
        row.action = actionText(operation.action);
        row.bytes = sizeText(operation.bytes);
        // Уровень риска и «почему это мусор» едут из плана: решение о действии
        // принимает core::plan, и экран не имеет права придумывать второе (FR-4).
        row.safety = riskText(operation.safety);
        row.why = joinReasons(operation.evidence);
        row.reason = operation.reason;
        rows.push_back(std::move(row));
    }
    return rows;
}

void CleanupViewModel::beginScan() {
    auto& s = *impl_;
    s.error.clear();
    s.progress = CleanupProgress{};
    s.progress.state = ScreenState::Scanning;
    s.scan = ScanProgress{};
}

void CleanupViewModel::publishScanProgress(ScanProgress progress) {
    auto& s = *impl_;
    // Кадр, уже поставленный в очередь до отмены, не должен воскресить
    // Scanning: иначе endScan() увидит не Cancelling и объявит отменённый
    // скан успешным (та же осторожность, что в publishOperationResult).
    if (s.progress.state != ScreenState::Cancelling) s.progress.state = ScreenState::Scanning;
    s.scan = std::move(progress);
}

void CleanupViewModel::endScan() {
    auto& s = *impl_;
    s.progress.state = s.progress.state == ScreenState::Cancelling ? ScreenState::Cancelled
                                                                  : ScreenState::Ready;
    s.progress.currentLabel.clear();
}

void CleanupViewModel::fail(std::string reason) {
    auto& s = *impl_;
    s.error = std::move(reason);
    s.progress.state = ScreenState::Failed;
    s.progress.currentLabel.clear();
}

bool CleanupViewModel::beginCleanup() {
    auto& s = *impl_;
    if (s.progress.busy()) return false;
    if (!hasCandidates()) {
        s.error = tr(StringId::kCleanupNoCandidates);
        return false;
    }
    const core::CleanupPlan plan = effectivePlan();
    if (plan.operationCount() == 0) {
        s.error = tr(StringId::kCleanupNoCandidates);
        s.progress.state = ScreenState::Ready;
        return false;
    }
    if (!s.gate.alreadyShown(plan)) {
        // FR-5: dry-run обязателен и запускается по умолчанию перед первым
        // удалением в сессии. Спрашивать «вы уверены?» без списка нельзя —
        // §7.2 требует списка того, что будет удалено.
        s.progress.state = ScreenState::DryRun;
        s.progress.totalOperations = plan.operationCount();
        s.progress.totalBytes = plan.totals.selectedBytes;
        return false;
    }
    s.progress = CleanupProgress{};
    s.progress.state = ScreenState::Running;
    s.progress.totalOperations = plan.operationCount();
    s.progress.totalBytes = plan.totals.selectedBytes;
    s.error.clear();
    return true;
}

void CleanupViewModel::publishOperationResult(bool ok, std::uint64_t freedBytes, std::string_view label) {
    auto& s = *impl_;
    // После отмены пул успевает досчитать операцию в полёте, поэтому результат
    // принимается и в Cancelling — иначе счётчик разошёлся бы с журналом.
    if (s.progress.state != ScreenState::Running && s.progress.state != ScreenState::Cancelling) return;
    s.progress.completedOperations += 1;
    if (ok) {
        s.progress.freedBytes += freedBytes;
    } else {
        s.progress.failedOperations += 1;
    }
    s.progress.currentLabel.assign(label);
}

bool CleanupViewModel::requestCancel() {
    auto& s = *impl_;
    if (!s.progress.cancelAvailable()) return false;
    s.progress.state = ScreenState::Cancelling;
    return true;
}

void CleanupViewModel::publishFinished() {
    auto& s = *impl_;
    if (s.progress.state == ScreenState::Cancelling) {
        s.progress.state = ScreenState::Cancelled;
    } else if (s.progress.state == ScreenState::Running) {
        s.progress.state = ScreenState::Done;
    }
    s.progress.currentLabel.clear();
}

void CleanupViewModel::setUndoAvailable(bool available) { impl_->undoAvailable = available; }

bool CleanupViewModel::undo() {
    auto& s = *impl_;
    // §7.2: отмена доступна, пока транзакция не «схлопнулась», то есть пока
    // корзина приложения не переполнилась. Флаг ставит движок; экран только
    // спрашивает.
    if (!s.undoAvailable) return false;
    s.undoAvailable = false;
    return true;
}

int CleanupViewModel::catalogHeightPx(const CleanupMetrics& metrics, int dpi) const noexcept {
    (void)metrics;
    const theme::Metrics scale = theme::metricsForDpi(dpi > 0 ? static_cast<unsigned>(dpi) : kDefaultDpi);
    std::size_t rows = 0;
    for (const CategoryNode& category : impl_->categories) {
        ++rows;
        if (isExpanded(category.key)) rows += category.itemCount;
    }
    return scale.dip(kTreeRowHeightDip * static_cast<double>(rows == 0 ? 1 : rows));
}

void CleanupViewModel::applyPageState(const PageState& state) {
    auto& s = *impl_;
    if (s.filter != state.filter) {
        s.filter = state.filter;
        rebuild(true);
    }
    s.expanded = state.expandedKeys;
    s.focusKey = state.selectedKey;
}

PageState CleanupViewModel::pageState() const {
    const auto& s = *impl_;
    PageState state;
    // Прокрутку держит SysTreeView32, а состояние страницы в DIP живёт у
    // навигатора: два хранилища прокрутки рассинхронизировались бы при
    // перерисовке дерева.
    state.scrollOffsetDip = 0;
    state.splitterDip = 0;
    state.selectedKey = s.focusKey;
    state.expandedKeys = s.expanded;
    state.filter = s.filter;
    return state;
}

// ---------------------------------------------------------------------------
// Окно экрана
// ---------------------------------------------------------------------------
//
// Ниже только Win32 (SPEC §7, ADR-3). Всё, что можно было решить без окна,
// вынесено в модель и раскладку выше; здесь осталось ровно три вещи, которые
// Win32 умеет сам и чему нельзя научить чистый C++: дерево с чекбоксами и
// клавиатурным фокусом, полоса прогресса и нажатия на кнопки.


namespace detail {

constexpr wchar_t kCleanupViewClass[] = L"MrProper.CleanupView";

// Отложенная перерисовка после действия внутри обработчика уведомления. Вставка
// и удаление узлов прямо в TVN_ITEMCHANGING — это перерисовка дерева из его же
// обработчика, на которую comctl32 не рассчитан: узлы на момент обработки ещё
// не созданы.
constexpr UINT kMsgSyncModel = WM_APP + 1;

// Идентификаторы дочерних окон. Отдельное пространство от ControlId (команды
// WM_COMMAND): иначе WM_COMMAND кнопки «Очистить сейчас» пришёл бы от списка
// строк dry-run, и разбирать пришлось бы по источнику.
enum : UINT_PTR {
    kChildSummary = 1,
    kChildProgress,
    kChildStatus,
    kChildDetails,
    kChildTree,
    kChildDryRunTitle,
    kChildDryRunList,
    kChildDryRunClose,
    kChildFirstButton,
};

// Три состояния чекбокса на «можно» и на «нельзя». Выключенные нужны узлам,
// которые выбрать нельзя (занятый файл, Risky без подтверждения): серый квадрат
// читается как «нельзя» без подсказки, и об этом не надо догадываться.
constexpr int kStateImageEnabledCount = 3;

int stateImageIndex(CheckState state, bool enabled) noexcept {
    int base = 0;
    switch (state) {
    case CheckState::Unchecked: base = 0; break;
    case CheckState::Checked: base = 1; break;
    case CheckState::Partial: base = 2; break;
    }
    return enabled ? base : base + kStateImageEnabledCount;
}

struct NodeRef {
    bool category;
    std::size_t index;
};

// Подкласс контрола: исходная процедура и сам контрол. Владеет ими экран.
struct ChildProc {
    HWND hwnd{nullptr};
    WNDPROC prev{nullptr};
};

// Процедуры окон объявлены здесь: ViewState обращается к childProc (подкласс),
// а CleanupScreen::create — к viewProc, и обе определены ниже.
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// Состояние окна. Отдельная именованная структура, а не содержимое Impl, нужна
// затем, чтобы вспомогательные функции файла принимали ViewState&:
// вложенный приватный Impl из заголовка они назвать не могут.
struct ViewState {
    CleanupScreen::Callbacks callbacks;
    CleanupViewModel model;
    theme::Theme theme;
    CleanupMetrics metrics{};
    int dpi{kDefaultDpi};

    HWND window{nullptr};
    HWND summary{nullptr};
    HWND progress{nullptr};
    HWND status{nullptr};
    HWND details{nullptr};
    HWND tree{nullptr};
    HWND dryRunTitle{nullptr};
    HWND dryRunList{nullptr};
    HWND dryRunClose{nullptr};
    std::array<HWND, 7> buttons{};
    std::array<HFONT, 4> fonts{};
    HBRUSH surfaceBrush{nullptr};
    HIMAGELIST stateImages{nullptr};
    std::vector<NodeRef> nodeRefs;
    std::vector<HTREEITEM> nodeHandles;
    std::vector<ChildProc> children;
    bool syncing{false};
    bool controlsReady{false};
    bool marquee{false};
    bool dryRunColumnReady{false};  // столбец списка dry-run создан один раз

    ~ViewState() {
        for (HFONT& font : fonts) {
            if (font != nullptr) ::DeleteObject(font);
        }
        if (stateImages != nullptr) ::ImageList_Destroy(stateImages);
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
    }

    void setChildText(HWND child, std::string_view text) {
        if (child == nullptr) return;
        const std::wstring wide = toWide(text);
        ::SetWindowTextW(child, wide.c_str());
    }

    // Ключ узла по HTREEITEM. lParam — индекс в nodeRefs, а не указатель на
    // модель: модель перестраивает векторы при каждом щелчке, и висячие
    // указатели в дереве означали бы выход за пределы памяти.
    std::string keyFor(HTREEITEM handle) const {
        if (handle == nullptr || tree == nullptr) return std::string();
        TVITEMW item{};
        item.mask = TVIF_PARAM;
        item.hItem = handle;
        if (::SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) == FALSE) {
            return std::string();
        }
        const LPARAM param = item.lParam;
        if (param < 0) return std::string();
        const std::size_t index = static_cast<std::size_t>(param);
        if (index >= nodeRefs.size()) return std::string();
        const NodeRef& reference = nodeRefs[index];
        if (reference.category) {
            if (reference.index >= model.categories().size()) return std::string();
            return model.categories()[reference.index].key;
        }
        if (reference.index >= model.items().size()) return std::string();
        return model.items()[reference.index].key;
    }

    HTREEITEM handleFor(const std::string& key) const {
        for (std::size_t i = 0; i < nodeHandles.size(); ++i) {
            if (keyFor(nodeHandles[i]) == key) return nodeHandles[i];
        }
        return nullptr;
    }

    SafetyLevel safetyFor(HTREEITEM handle) const {
        if (tree == nullptr) return SafetyLevel::Safe;
        TVITEMW item{};
        item.mask = TVIF_PARAM;
        item.hItem = handle;
        if (::SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) == FALSE) {
            return SafetyLevel::Safe;
        }
        const LPARAM param = item.lParam;
        if (param < 0) return SafetyLevel::Safe;
        const std::size_t index = static_cast<std::size_t>(param);
        if (index >= nodeRefs.size()) return SafetyLevel::Safe;
        const NodeRef& reference = nodeRefs[index];
        if (reference.category) {
            if (reference.index >= model.categories().size()) return SafetyLevel::Safe;
            return model.categories()[reference.index].safety;
        }
        if (reference.index >= model.items().size()) return SafetyLevel::Safe;
        return model.items()[reference.index].safety;
    }

    void applyStateImage(HTREEITEM handle, CheckState state, bool enabled) {
        if (tree == nullptr || handle == nullptr) return;
        TVITEMW item{};
        item.mask = TVIF_STATE;
        item.stateMask = TVIS_STATEIMAGEMASK;
        item.state = stateImageIndex(state, enabled);
        item.hItem = handle;
        ::SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }

    void syncTree() {
        if (tree == nullptr) return;

        // Положение прокрутки и фокус переживают перестроение: без этого щелчок
        // по чекбоксу внизу списка прыгал бы дерево наверх.
        std::string firstVisible;
        if (const HTREEITEM visible = TreeView_GetFirstVisible(tree)) firstVisible = keyFor(visible);
        const std::string focused = model.focusKey();

        syncing = true;
        TreeView_DeleteAllItems(tree);
        nodeRefs.clear();
        nodeHandles.clear();

        const std::vector<CategoryNode>& categories = model.categories();
        const std::vector<ItemNode>& items = model.items();
        for (std::size_t index = 0; index < categories.size(); ++index) {
            const CategoryNode& category = categories[index];
            std::wstring text = toWide(model.nodeText(category.key));
            TVINSERTSTRUCTW parent{};
            parent.hParent = TVI_ROOT;
            parent.hInsertAfter = TVI_LAST;  // порядок задаёт модель (каталог FR-3)
            parent.itemex.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
            parent.itemex.pszText = const_cast<wchar_t*>(text.c_str());
            parent.itemex.cChildren = 1;
            parent.itemex.lParam = static_cast<LPARAM>(nodeRefs.size());
            nodeRefs.push_back(NodeRef{true, index});
            const HTREEITEM parentItem = TreeView_InsertItem(tree, &parent);
            if (parentItem == nullptr) {
                syncing = false;
                logWin32("ui.cleanup.tree", "TreeView_InsertItem(category)", ::GetLastError());
                return;
            }
            nodeHandles.push_back(parentItem);
            if (model.isExpanded(category.key)) {
                ::SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(parentItem));
            }
            for (const std::size_t itemIndex : category.items) {
                if (itemIndex >= items.size()) continue;
                std::wstring itemText = toWide(model.nodeText(items[itemIndex].key));
                TVINSERTSTRUCTW child{};
                child.hParent = parentItem;
                child.hInsertAfter = TVI_LAST;
                child.itemex.mask = TVIF_TEXT | TVIF_PARAM;
                child.itemex.pszText = const_cast<wchar_t*>(itemText.c_str());
                child.itemex.lParam = static_cast<LPARAM>(nodeRefs.size());
                nodeRefs.push_back(NodeRef{false, itemIndex});
                const HTREEITEM childItem = TreeView_InsertItem(tree, &child);
                if (childItem == nullptr) {
                    syncing = false;
                    logWin32("ui.cleanup.tree", "TreeView_InsertItem(item)", ::GetLastError());
                    return;
                }
                nodeHandles.push_back(childItem);
            }
        }
        syncing = false;

        for (std::size_t i = 0; i < nodeHandles.size(); ++i) {
            const NodeRef& reference = nodeRefs[i];
            if (reference.category) {
                const CategoryNode& category = categories[reference.index];
                applyStateImage(nodeHandles[i], category.check, category.selectableCount > 0);
            } else {
                const ItemNode& item = items[reference.index];
                applyStateImage(nodeHandles[i], item.checked ? CheckState::Checked : CheckState::Unchecked,
                                item.selectable);
            }
        }

        if (!firstVisible.empty()) {
            if (const HTREEITEM handle = handleFor(firstVisible)) {
                ::SendMessageW(tree, TVM_ENSUREVISIBLE, 0, reinterpret_cast<LPARAM>(handle));
            }
        }
        if (!focused.empty()) {
            if (const HTREEITEM handle = handleFor(focused)) {
                ::SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(handle));
            }
        }
    }

    void syncDryRun() {
        const bool visible = model.state() == ScreenState::DryRun;
        const int show = visible ? SW_SHOW : SW_HIDE;
        if (dryRunTitle != nullptr) ::ShowWindow(dryRunTitle, show);
        if (dryRunList != nullptr) ::ShowWindow(dryRunList, show);
        if (dryRunClose != nullptr) ::ShowWindow(dryRunClose, show);
        if (!visible || dryRunList == nullptr) return;

        setChildText(dryRunTitle, tr(StringId::kCleanupDryRunNotice));
        if (!dryRunColumnReady) {
            // В отчётном списке столбец должен существовать, даже когда
            // заголовок скрыт (LVS_NOCOLUMNHEADER): без него LVM_INSERTITEM
            // не вставит строку. Ширина с запасом — строки длинные, а
            // заголовка, который мог бы объяснить обрезку, нет.
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.cx = 2400;
            column.pszText = const_cast<wchar_t*>(L"");
            dryRunColumnReady = ListView_InsertColumn(dryRunList, 0, &column) != 0;
        }
        ::SendMessageW(dryRunList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                       static_cast<LPARAM>(LVS_EX_DOUBLEBUFFER));
        ListView_DeleteAllItems(dryRunList);
        const std::vector<CleanupViewModel::DryRunRow> rows = model.dryRunRows();
        ListView_SetItemCount(dryRunList, static_cast<int>(rows.size()));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            // Строка подтверждения: категория, элемент, объём, действие, УРОВЕНЬ
            // РИСКА и причина (FR-4/FR-5/§12). Уровень и объяснение не
            // выбрасываются, даже если строка не влезет по ширине: это
            // последнее, что человек видит перед удалением.
            std::string line = rows[i].category + " · " + rows[i].item + " — " + rows[i].bytes + " — " + rows[i].action;
            line += " · ";
            line += rows[i].safety;
            if (!rows[i].why.empty()) {
                line += " · ";
                line += tr(StringId::kCleanupWhyJunk);
                line += ": ";
                line += rows[i].why;
            }
            if (!rows[i].reason.empty()) {
                line += " · ";
                line += rows[i].reason;
            }
            const std::wstring wide = toWide(line);
            LVITEMW entry{};
            entry.mask = LVIF_TEXT;
            entry.iItem = static_cast<int>(i);
            entry.pszText = const_cast<wchar_t*>(wide.c_str());
            ListView_InsertItem(dryRunList, &entry);
        }
        setChildText(dryRunClose, tr(StringId::kActionClose));
    }
    void syncTexts() {
        const std::string focused = model.focusKey();
        if (!focused.empty() && !model.progress().busy()) {
            setChildText(details, model.nodeDetails(focused));
        } else if (model.progress().busy() && !model.scanProgress().currentLabel.empty()) {
            // Чем занят скан (путь или имя правила). Полоса-бегунок не говорит,
            // куда смотреть, а проход по диску идёт до минуты (§5
            // «Производительность»), и молчаливая минута — это минута, в
            // которую человек решает, что программа зависла.
            setChildText(details, model.scanProgress().currentLabel);
        } else {
            setChildText(details, model.reclaimHintText());
        }
        setChildText(status, model.statusText());
    }

    void syncProgress() {
        if (progress == nullptr) return;
        const CleanupProgress& state = model.progress();
        // Пока идёт скан, точное число кандидатов неизвестно: полоса marquee.
        // Полоса, стоящая на нуле, читается как «зависло» (§6.4 — UI читает
        // счётчики, а не выдумывает итог).
        if (state.indeterminate()) {
            if (!marquee) {
                ::SendMessageW(progress, PBM_SETMARQUEE, TRUE, 30);
                marquee = true;
            }
            return;
        }
        if (marquee) {
            ::SendMessageW(progress, PBM_SETMARQUEE, FALSE, 0);
            marquee = false;
        }
        const std::size_t limit = static_cast<std::size_t>(0x7FFFFFFFU);
        if (state.state == ScreenState::Ready) {
            // В покое полоса показывает долю выбранного от «если выбрать всё» —
            // тот же агрегат FR-5, а не остаток работы: работать ещё не начали,
            // и «осталось 100 %» было бы враньём.
            const int percent = static_cast<int>(model.aggregates().selectedFraction() * 100.0);
            ::SendMessageW(progress, PBM_SETRANGE32, 0, 100);
            ::SendMessageW(progress, PBM_SETPOS, static_cast<WPARAM>(std::clamp(percent, 0, 100)), 0);
            return;
        }
        const std::size_t total = std::min(state.totalOperations, limit);
        ::SendMessageW(progress, PBM_SETRANGE32, 0, static_cast<LPARAM>(total));
        std::size_t done = 0;
        if (state.state == ScreenState::Running || state.state == ScreenState::Cancelling) {
            done = std::min(state.completedOperations + state.failedOperations, total);
        } else if (state.state == ScreenState::Done) {
            done = total;
        }
        ::SendMessageW(progress, PBM_SETPOS, static_cast<WPARAM>(done), 0);
    }

    void setButton(std::size_t index, std::string_view text, bool enabled) {
        if (index >= buttons.size()) return;
        const HWND button = buttons[index];
        if (button == nullptr) return;
        setChildText(button, text);
        if ((::IsWindowEnabled(button) != 0) == enabled) return;
        ::EnableWindow(button, enabled ? TRUE : FALSE);
    }

    void syncButtons() {
        const CleanupProgress& state = model.progress();
        const CleanupAggregates& aggregates = model.aggregates();
        const bool busy = state.busy();
        const bool dryRun = model.state() == ScreenState::DryRun;
        std::size_t selectable = 0;
        for (const CategoryNode& category : model.categories()) selectable += category.selectableCount;

        setButton(0, dryRun ? tr(StringId::kActionOk)
                            : tr(StringId::kActionClean),
                  !busy && (dryRun ? state.totalOperations > 0 : aggregates.selectedCount > 0));
        setButton(1, tr(StringId::kActionCancel), state.cancelAvailable());
        setButton(2, tr(StringId::kActionRescan), !busy);

        // «Показать все» — два подтверждения в одной кнопке (§9: Risky с двойным
        // подтверждением). Первое нажатие показывает Risky, второе разрешает
        // их выбирать; подпись меняется, чтобы шаг был виден.
        const std::string riskyCount =
            " (" + trPlural(StringId::kCleanupCandidates, aggregates.hiddenRiskyCount) + ")";
        const bool secondStep = model.showAllRisky() && !model.allowRisky();
        setButton(3,
                  (secondStep ? tr(StringId::kSafetyRisky)
                              : tr(StringId::kActionShowAll)) +
                      riskyCount,
                  !busy && aggregates.hiddenRiskyCount > 0 && !model.allowRisky());

        setButton(4, tr(StringId::kActionSelectAll),
                  !busy && selectable > 0 && aggregates.selectedCount < selectable);
        setButton(5, tr(StringId::kActionClearSelection),
                  !busy && aggregates.selectedCount > 0);
        setButton(6, tr(StringId::kActionUndo), model.undoAvailable());
    }

    CleanupLayout currentLayout() const {
        RECT client{};
        if (window == nullptr || ::GetClientRect(window, &client) == FALSE) return CleanupLayout{};
        const bool dryRunVisible = model.state() == ScreenState::DryRun;
        return CleanupLayout::compute(metrics, dpi, static_cast<int>(client.right), static_cast<int>(client.bottom),
                                      dryRunVisible, model.catalogHeightPx(metrics, dpi));
    }

    void place(HWND child, const CleanupRect& rect, bool visible) {
        if (child == nullptr) return;
        const int show = visible ? SW_SHOW : SW_HIDE;
        if (rect.empty() || !visible) {
            ::ShowWindow(child, show);
            return;
        }
        ::SetWindowPos(child, nullptr, rect.x, rect.y, rect.width, rect.height,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(child, show);
    }

    void layout() {
        const CleanupLayout layout = currentLayout();
        const int gap = theme::metricsForDpi(static_cast<unsigned>(dpi)).dip(6.0);
        place(summary, layout.summaryRect(), !layout.cramped());
        place(tree, layout.treeRect(), !layout.cramped());
        place(details, layout.detailsRect(), !layout.cramped());
        place(dryRunList, layout.dryRunRect(), layout.dryRunVisible());
        place(dryRunTitle, CleanupRect{layout.dryRunRect().x + 6, layout.dryRunRect().y + 2,
                                       std::max(0, layout.dryRunRect().width / 3), 20},
              layout.dryRunVisible());
        place(dryRunClose, layout.dryRunCloseRect(), layout.dryRunVisible());

        // Полоса прогресса и подпись состояния делят одну строку: подпись важнее
        // точной ширины полосы, а «Отмена запрошена…» человек должен прочитать,
        // не разворачивая окно.
        const CleanupRect progressRow = layout.progressRect();
        const int barWidth = progressRow.empty() ? 0 : progressRow.width / 3;
        place(progress, CleanupRect{progressRow.x, progressRow.y, barWidth - gap, progressRow.height},
              progressRow.height > 0);
        place(status, CleanupRect{progressRow.x + barWidth, progressRow.y,
                                  std::max(0, progressRow.width - barWidth), progressRow.height},
              progressRow.height > 0);

        const std::array<CleanupRect, 7> rects{layout.cleanButtonRect(), layout.cancelButtonRect(),
                                               layout.rescanButtonRect(), layout.showAllButtonRect(),
                                               layout.selectAllButtonRect(),
                                               layout.clearSelectionButtonRect(),
                                               layout.undoButtonRect()};
        for (std::size_t i = 0; i < buttons.size(); ++i) place(buttons[i], rects[i], true);
    }

    void applyFonts() {
        // Сначала создаём новые шрифты, потом перевешиваем их на контролы и
        // только потом удаляем старые: удалить HFONT, висящий на контроле, —
        // значит оставить контрол со шрифтом в никуда.
        const std::array<theme::FontRole, 4> roles{theme::FontRole::Metric, theme::FontRole::BodyStrong,
                                                  theme::FontRole::Body, theme::FontRole::Mono};
        std::array<HFONT, 4> next{};
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const LOGFONTW description = theme.font(roles[i]).toLogFont(static_cast<unsigned>(dpi));
            next[i] = ::CreateFontIndirectW(&description);
        }
        // Крупные числа в агрегатах, полужирный для дерева, обычный для
        // пояснений, моноширинный для списка операций (колонки должны читаться).
        if (summary != nullptr) {
            ::SendMessageW(summary, WM_SETFONT, reinterpret_cast<WPARAM>(next[0]), TRUE);
        }
        if (tree != nullptr) {
            ::SendMessageW(tree, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        }
        for (const HWND child : {status, details, dryRunTitle, dryRunClose}) {
            if (child != nullptr) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next[2]), TRUE);
        }
        for (const HWND button : buttons) {
            if (button != nullptr) {
                ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(next[2]), TRUE);
            }
        }
        if (dryRunList != nullptr) {
            ::SendMessageW(dryRunList, WM_SETFONT, reinterpret_cast<WPARAM>(next[3]), TRUE);
        }
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (fonts[i] != nullptr) ::DeleteObject(fonts[i]);
            fonts[i] = next[i];
        }
    }

    // Чекбоксы рисуются сами: картинки состояния — это те же три знака, что
    // рисует NM_CUSTOMDRAW, только контрол сам резервирует под них место и сам
    // обрабатывает щелчок. GDI, а не D2D: контрол рисует их сам, и подсовывать
    // ему D2D-поверхность нельзя.
    HIMAGELIST buildStateImages() {
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const int side = std::max(12, scale.dip(16.0));
        const int count = kStateImageEnabledCount * 2;
        HIMAGELIST list = ::ImageList_Create(side, side, ILC_COLOR | ILC_MASK, count, count);
        if (list == nullptr) {
            logWin32("ui.cleanup.stateImages", "ImageList_Create", ::GetLastError());
            return nullptr;
        }
        // Фон картинки заливается цветом поверхности и этим же цветом помечается
        // как прозрачный (ImageList_AddMasked принимает цвет-ключ, а не битмап
        // маски). Раньше фон оставался белым, и в тёмной теме чекбоксы были бы
        // белыми квадратами.
        const COLORREF surfaceKey = theme::colorRef(theme.palette().surface);
        HBRUSH surface = ::CreateSolidBrush(surfaceKey);
        const HDC screen = ::GetDC(nullptr);
        if (screen == nullptr || surface == nullptr) {
            if (surface != nullptr) ::DeleteObject(surface);
            if (screen != nullptr) ::ReleaseDC(nullptr, screen);
            ::ImageList_Destroy(list);
            return nullptr;
        }
        for (int index = 0; index < count; ++index) {
            HDC dc = ::CreateCompatibleDC(screen);
            HBITMAP bitmap = ::CreateBitmap(side, side, 1, 32, nullptr);
            if (dc == nullptr || bitmap == nullptr) {
                if (dc != nullptr) ::DeleteDC(dc);
                if (bitmap != nullptr) ::DeleteObject(bitmap);
                break;
            }
            const HGDIOBJ old = ::SelectObject(dc, bitmap);
            RECT full{0, 0, side, side};
            ::FillRect(dc, &full, surface);
            paintStateImage(dc, side, index);
            ::SelectObject(dc, old);
            ::ImageList_AddMasked(list, bitmap, surfaceKey);
            ::DeleteObject(bitmap);
            ::DeleteDC(dc);
        }
        ::DeleteObject(surface);
        ::ReleaseDC(nullptr, screen);
        return list;
    }

    void paintStateImage(HDC dc, int side, int index) const {
        const theme::Palette& palette = theme.palette();
        const bool enabled = index < kStateImageEnabledCount;
        const int state = index % kStateImageEnabledCount;
        const theme::Color border = enabled ? palette.textSecondary : palette.textDisabled;
        const theme::Color fill = enabled ? palette.surface : palette.surfaceAlt;
        const theme::Color mark = enabled ? palette.accent : palette.textDisabled;

        HBRUSH brush = ::CreateSolidBrush(theme::colorRef(fill));
        HPEN pen = ::CreatePen(PS_SOLID, 1, theme::colorRef(border));
        const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
        const HGDIOBJ oldPen = ::SelectObject(dc, pen);
        ::Rectangle(dc, 1, 1, side - 1, side - 1);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(brush);
        ::DeleteObject(pen);

        if (state == 0) return;  // пустой квадрат
        const int left = side / 4;
        const int right = side - left;
        const int middle = side / 2;
        HPEN stroke = ::CreatePen(PS_SOLID, std::max(1, side / 8), theme::colorRef(mark));
        const HGDIOBJ previous = ::SelectObject(dc, stroke);
        if (state == 1) {  // галочка
            ::MoveToEx(dc, left, middle, nullptr);
            ::LineTo(dc, middle, side - left);
            ::LineTo(dc, right, left);
        } else {  // частично выбранный узел — короткая черта
            ::MoveToEx(dc, left, middle, nullptr);
            ::LineTo(dc, right, middle);
        }
        ::SelectObject(dc, previous);
        ::DeleteObject(stroke);
    }

    void applyPalette() {
        const theme::Palette& palette = theme.palette();
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
        surfaceBrush = ::CreateSolidBrush(theme::colorRef(palette.surface));
        if (tree != nullptr) {
            TreeView_SetBkColor(tree, theme::colorRef(palette.surface));
            TreeView_SetTextColor(tree, theme::colorRef(palette.textPrimary));
            // Подтема «тёмного» у нативных контролов нет в документированном
            // API (ADR-003), и модуль темы делает это через безопасные вызовы
            // uxtheme; отказ — не повод оставлять контрол белым.
            (void)theme::enableDarkModeForWindow(tree, theme.scheme());
        }
        if (stateImages != nullptr) ::ImageList_Destroy(stateImages);
        stateImages = buildStateImages();
        if (stateImages != nullptr && tree != nullptr) {
            TreeView_SetImageList(tree, stateImages, TVSIL_STATE);
        }
    }

    void reloadTheme() {
        theme.reload();
        applyPalette();
        if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
    }

    void createChild(HWND child) {
        if (child == nullptr || window == nullptr) return;
        const WNDPROC prev =
            reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&childProc)));
        if (prev == nullptr) {
            logWin32("ui.cleanup.subclass", "SetWindowLongPtrW(GWLP_WNDPROC)", ::GetLastError());
            return;
        }
        ::SetWindowLongPtrW(child, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        children.push_back(ChildProc{child, prev});
    }

    void removeChild(HWND child) {
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (children[i].hwnd != child) continue;
            const WNDPROC prev = children[i].prev;
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
            ::SetWindowLongPtrW(child, GWLP_USERDATA, 0);
            if (prev != nullptr) {
                ::SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev));
            }
            return;
        }
    }

    void command(ControlId id) {
        switch (id) {
        case ControlId::Clean: {
            if (model.state() == ScreenState::DryRun) {
                // Подтверждение панели dry-run: сначала разрешаем именно этот
                // план, потом запускаем. Порядок обязателен — иначе план
                // изменится между подтверждением и запуском.
                model.acknowledgeDryRun();
            }
            if (model.beginCleanup() && callbacks.onStartCleanup) callbacks.onStartCleanup();
            break;
        }
        case ControlId::Cancel:
            if (model.requestCancel() && callbacks.onCancel) callbacks.onCancel();
            break;
        case ControlId::Rescan:
            model.beginScan();
            if (callbacks.onRescan) callbacks.onRescan();
            break;
        case ControlId::ShowAll:
            // Первое нажатие — «показать», второе — «разрешить». Одно нажатие
            // не может быть и тем, и другим: подтверждений должно быть два.
            if (!model.showAllRisky()) {
                model.setShowAllRisky(true);
            } else {
                model.setAllowRisky(true);
            }
            break;
        case ControlId::SelectAll: model.selectAll(); break;
        case ControlId::ClearSelection: model.clearSelection(); break;
        case ControlId::Undo:
            if (model.undo() && callbacks.onUndo) callbacks.onUndo();
            break;
        case ControlId::DryRunClose: model.closeDryRun(); break;
        }
    }

    bool createControls(HINSTANCE instance) {
        const DWORD childVisible = WS_CHILD | WS_VISIBLE;
        summary = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_OWNERDRAW, 0, 0, 0, 0, window,
                                    reinterpret_cast<HMENU>(kChildSummary), instance, nullptr);
        progress = ::CreateWindowExW(0, PROGRESS_CLASSW, nullptr, childVisible | PBS_SMOOTH, 0, 0, 0, 0,
                                     window, reinterpret_cast<HMENU>(kChildProgress), instance, nullptr);
        status = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0,
                                   window, reinterpret_cast<HMENU>(kChildStatus), instance, nullptr);
        details = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0,
                                    window, reinterpret_cast<HMENU>(kChildDetails), instance, nullptr);
        // TVS_DISABLEDRAGDROP — обязателен: перетаскивание строк переставило бы
        // категории и сломало бы каталог FR-3. TVS_NOTOOLTIPS — потому что
        // объяснение показывает поле под деревом (одним текстом, доступным
        // с клавиатуры, tooltip был бы недоступен экранному диктору).
        tree = ::CreateWindowExW(0, WC_TREEVIEWW, nullptr,
                                 childVisible | WS_TABSTOP | WS_HSCROLL | TVS_HASBUTTONS | TVS_HASLINES |
                                     TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_NOTOOLTIPS |
                                     TVS_DISABLEDRAGDROP,
                                 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kChildTree), instance, nullptr);
        dryRunTitle = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_LEFT, 0, 0, 0, 0, window,
                                        reinterpret_cast<HMENU>(kChildDryRunTitle), instance, nullptr);
        dryRunList = ::CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                                       childVisible | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                           LVS_SHOWSELALWAYS,
                                       0, 0, 0, 0, window, reinterpret_cast<HMENU>(kChildDryRunList), instance,
                                       nullptr);
        dryRunClose = ::CreateWindowExW(0, L"BUTTON", nullptr, childVisible | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                                        window, reinterpret_cast<HMENU>(kChildDryRunClose), instance, nullptr);
        const std::array<ControlId, 7> ids{ControlId::Clean,        ControlId::Cancel,      ControlId::Rescan,
                                           ControlId::ShowAll,      ControlId::SelectAll,   ControlId::ClearSelection,
                                           ControlId::Undo};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            buttons[i] = ::CreateWindowExW(0, L"BUTTON", nullptr, childVisible | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                           0, 0, window,
                                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ids[i])), instance,
                                           nullptr);
        }
        const std::array<HWND, 12> created{summary, progress, status, details, tree,  dryRunTitle, dryRunList,
                                           dryRunClose, buttons[0], buttons[1], buttons[2], buttons[3]};
        for (const HWND child : created) {
            if (child == nullptr) {
                logWin32("ui.cleanup.create", "CreateWindowExW(child)", ::GetLastError());
                return false;
            }
        }
        for (const HWND child : created) {
            // Подкласс нужен дереву, списку и кнопкам: они едят клавиши, и без
            // подкласса Ctrl+Z и Escape до модели не дошли бы (§5 «Клавиатурная
            // навигация, фокус», §7.2). Панель агрегатов и полоса прогресса
            // фокуса не получают (у них нет WS_TABSTOP), им подкласс не нужен.
            if (child == summary || child == progress) continue;
            createChild(child);
        }
        for (const HWND button : buttons) {
            if (button != nullptr) createChild(button);
        }
        controlsReady = true;
        return true;
    }

    // Отложенная перерисовка модели. Клавиши приходят в родителя из
    // подкласса контрола, то есть дерево перестраивается, не выйдя из
    // собственного обработчика сообщения; пересборка узлов (TreeView_
    // DeleteAllItems) внутри TVN/WM_KEYDOWN — это изменение дерева из его же
    // обработчика, на которое comctl32 не рассчитан. Поэтому визуальное
    // обновление всегда на следующем витке очереди сообщений, а модель
    // меняется сразу: она и есть источник истины.
    void requestSync() {
        if (window != nullptr) ::PostMessageW(window, kMsgSyncModel, 0, 0);
    }

    void refreshAll() {
        if (!controlsReady) return;
        syncTree();
        syncDryRun();
        syncTexts();
        syncProgress();
        syncButtons();
        layout();
        if (summary != nullptr) ::InvalidateRect(summary, nullptr, TRUE);
        if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
    }
};

ViewState* stateOf(HWND window) {
    return reinterpret_cast<ViewState*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// Иконка риска (§7.2: «иконка риска» у каждого элемента). Форма различает уровень
// без цвета: при высокой контрастности три цвета риска вырождаются в цвет
// текста (theme.cpp), и «безопасно» с «рискованно» обязаны отличаться чем-то
// ещё. Рисуем в CDDS_ITEMPOSTPAINT — после того, как контрол нарисовал строку:
// в PREPAINT фон строки затирает всё, что нарисовано рядом с подписью.
void paintRiskIcon(HDC dc, const RECT& item, SafetyLevel safety, const theme::Palette& palette, int side) {
    const theme::Color color = theme::riskColor(palette, safety);
    const theme::Color edge = theme::ensureContrast(palette.windowBackground, color, 3.0);
    const int left = item.right + 4;
    const int top = item.top + ((item.bottom - item.top) - side) / 2;
    if (left + side > item.right + 4096) return;  // защита от абсурдной геометрии
    RECT box{left, top, left + side, top + side};

    HBRUSH brush = ::CreateSolidBrush(theme::colorRef(color));
    HPEN pen = ::CreatePen(PS_SOLID, 1, theme::colorRef(edge));
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    switch (safety) {
    case SafetyLevel::Safe: ::Ellipse(dc, box.left, box.top, box.right, box.bottom); break;
    case SafetyLevel::Review: {
        POINT triangle[3]{{box.left + side / 2, box.top},
                          {box.right, box.bottom},
                          {box.left, box.bottom}};
        ::Polygon(dc, triangle, 3);
        break;
    }
    case SafetyLevel::Risky: {
        POINT diamond[4]{{box.left + side / 2, box.top},
                         {box.right, box.top + side / 2},
                         {box.left + side / 2, box.bottom},
                         {box.left, box.top + side / 2}};
        ::Polygon(dc, diamond, 4);
        break;
    }
    }
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(brush);
    ::DeleteObject(pen);
}

// Панель агрегатов: три цифры FR-5 плюс подпись «по аллоцированному размеру»
// (§7.2: большая цифра сверху, всегда с указанием units). Рисуется GDI, потому
// что это текст в рамке, а не карта разделов: Direct2D нужен там, где графика
// собственной природы (ADR-003).
LRESULT drawSummary(ViewState& state, const DRAWITEMSTRUCT& draw) {
    const theme::Palette& palette = state.theme.palette();
    const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(state.dpi));
    HDC dc = draw.hDC;
    RECT box = draw.rcItem;

    HBRUSH surface = ::CreateSolidBrush(theme::colorRef(palette.surface));
    ::FillRect(dc, &box, surface);
    ::DeleteObject(surface);
    HBRUSH border = ::CreateSolidBrush(theme::colorRef(palette.border));
    ::FrameRect(dc, &box, border);
    ::DeleteObject(border);

    const CleanupAggregates& aggregates = state.model.aggregates();
    std::array<std::wstring, 4> lines;
    lines[0] = toWide(state.model.summaryText());
    lines[1] = toWide(state.model.selectedText() + " · " + state.model.safeOnlyText());
    std::string third = state.model.lockedText();
    if (aggregates.belowThresholdCount > 0) {
        third += " · ";
        third += trPlural(StringId::kCleanupCandidates, aggregates.belowThresholdCount);
    }
    lines[2] = toWide(third);
    lines[3] = toWide(state.model.reclaimHintText());

    const std::array<HFONT, 4> used{state.fonts[0], state.fonts[2], state.fonts[2], state.fonts[2]};
    const std::array<theme::Color, 4> colors{palette.textPrimary, palette.textPrimary, palette.textSecondary,
                                            palette.textSecondary};
    const int padding = scale.dip(8.0);
    RECT row{box.left + padding, box.top + padding / 2, box.right - padding, box.bottom - padding};
    if (row.bottom <= row.top) return TRUE;
    const int lineHeight = (row.bottom - row.top) / static_cast<int>(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        RECT line{row.left, row.top + static_cast<int>(i) * lineHeight, row.right,
                  row.top + static_cast<int>(i + 1) * lineHeight};
        if (used[i] != nullptr) ::SelectObject(dc, used[i]);
        ::SetTextColor(dc, theme::colorRef(colors[i]));
        ::DrawTextW(dc, lines[i].c_str(), -1, &line,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    return TRUE;
}



// Обработчик окна экрана. Исключение не пересекает границу Win32 (§5): ловим
// здесь и пишем в журнал, иначе std::terminate внутри пользовательского режима
// не дал бы узнать, что произошло.
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ViewState* state = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        // lpCreateParams — const void*: разыменования тут нет, указатель живёт
        // дольше окна, поэтому снимаем const один раз здесь.
        state = const_cast<ViewState*>(static_cast<const ViewState*>(create->lpCreateParams));
        // Свой HWND известен уже здесь, а WM_CREATE (следующим сообщением) создаёт
        // детей именно от него: без этой строки CreateWindowExW получил бы
        // пустого родителя и дочерние окна не создались бы.
        if (state != nullptr) state->window = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    } else {
        state = stateOf(window);
    }
    if (state == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);

    try {
        switch (message) {
        case WM_CREATE: {
            if (!state->createControls(::GetModuleHandleW(nullptr))) return -1;
            return 0;
        }
        case WM_SIZE:
            state->layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (info != nullptr) {
                const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(state->dpi));
                info->ptMinTrackSize.x = scale.dip(state->metrics.minWidthDip);
                info->ptMinTrackSize.y = scale.dip(state->metrics.minHeightDip);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            const int newDpi = HIWORD(wParam);
            if (newDpi > 0) state->dpi = newDpi;
            state->theme.setDpi(static_cast<unsigned>(state->dpi));
            if (suggested != nullptr) {
                ::SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                               suggested->bottom - suggested->top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            state->applyPalette();
            state->applyFonts();
            state->refreshAll();
            return 0;
        }
        case WM_SETFOCUS:
            // §5 «Клавиатурная навигация, фокус»: фокус должен быть виден.
            if (state->tree != nullptr) ::SetFocus(state->tree);
            return 0;
        case WM_KEYDOWN: {
            if (state->model.handleKeyDown(static_cast<std::uint32_t>(wParam),
                                           (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                           (::GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
                state->requestSync();
                return 1;
            }
            break;
        }
        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) break;
            const WORD id = LOWORD(wParam);
            if (!isCleanupControl(id)) break;
            state->command(static_cast<ControlId>(id));
            state->refreshAll();
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header == nullptr) break;
            if (header->hwndFrom == state->tree) {
                switch (header->code) {
                case TVN_ITEMCHANGINGW: {
                    if (state->syncing) break;
                    const auto* change = reinterpret_cast<const NMTREEVIEW*>(lParam);
                    if (change == nullptr) break;
                    if ((change->itemNew.stateMask & TVIS_STATEIMAGEMASK) == 0) break;
                    // Модель — единственный источник истины о выборе, поэтому
                    // собственное циклирование картинки отменяем (TRUE), а своё
                    // состояние переносим отложенной перерисовкой: вставлять и
                    // удалять узлы внутри этого уведомления нельзя.
                    const std::string key = state->keyFor(change->itemNew.hItem);
                    if (!key.empty()) {
                        (void)state->model.toggleNode(key);
                        state->requestSync();
                    }
                    return TRUE;
                }
                case TVN_SELCHANGEDW: {
                    if (state->syncing) break;
                    const auto* change = reinterpret_cast<const NMTREEVIEW*>(lParam);
                    if (change == nullptr) break;
                    const std::string key = state->keyFor(change->itemNew.hItem);
                    if (!key.empty() && key != state->model.focusKey()) {
                        state->model.setFocusKey(key);
                        state->syncTexts();
                    }
                    return 0;
                }
                case NM_DBLCLK: {
                    const std::string key = state->keyFor(TreeView_GetSelection(state->tree));
                    if (key.empty() || state->callbacks.onOpenInExplorer == nullptr) break;
                    if (const ItemNode* item = state->model.findItem(key)) {
                        if (item->path.empty()) break;
                        state->callbacks.onOpenInExplorer(item->path);
                    }
                    return 0;
                }
                case NM_CUSTOMDRAW: {
                    auto* draw = reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);
                    if (draw == nullptr) break;
                    const theme::Palette& palette = state->theme.palette();
                    const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(state->dpi));
                    switch (draw->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT: {
                        // Цвет подписи — из темы, а не системный: на тёмной
                        // палитре системный чёрный текст нечитаем.
                        ::SetTextColor(draw->nmcd.hdc, theme::colorRef(palette.textPrimary));
                        ::SetBkMode(draw->nmcd.hdc, TRANSPARENT);
                        return CDRF_NEWFONT;
                    }
                    case CDDS_ITEMPOSTPAINT: {
                        const auto handle = reinterpret_cast<HTREEITEM>(draw->nmcd.dwItemSpec);
                        paintRiskIcon(draw->nmcd.hdc, draw->nmcd.rc, state->safetyFor(handle), palette,
                                      std::max(6, scale.dip(8.0)));
                        return CDRF_DODEFAULT;
                    }
                    default: break;
                    }
                    return CDRF_DODEFAULT;
                }
                default: break;
                }
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw == nullptr) break;
            if (draw->CtlType == ODT_STATIC && draw->CtlID == kChildSummary &&
                draw->hwndItem == state->summary) {
                return drawSummary(*state, *draw);
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const theme::Palette& palette = state->theme.palette();
            ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
            ::SetBkColor(dc, theme::colorRef(palette.surface));
            if (state->surfaceBrush == nullptr) {
                state->surfaceBrush = ::CreateSolidBrush(theme::colorRef(palette.surface));
            }
            return reinterpret_cast<LRESULT>(state->surfaceBrush);
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE: {
            if (theme::classifyMessage(message, wParam, lParam) == theme::Change::None) break;
            state->reloadTheme();
            state->refreshAll();
            return 0;
        }
        case kMsgSyncModel:
            state->refreshAll();
            return 0;
        case WM_ERASEBKGND: {
            // Дети перекрывают окно целиком; стирать собственную поверхность
            // незачем, а лишнее стирание мигает при перерисовке дерева.
            return 1;
        }
        case WM_DESTROY:
            state->controlsReady = false;
            return 0;
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default: break;
        }
    } catch (const std::exception& error) {
        logEvent(core::LogLevel::Error, "ui.cleanup.exception", error.what());
        return message == WM_CREATE ? -1 : 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

// Подкласс контрола. Нужен для одного: клавиши уходят родителю, где их
// разбирает модель (§5 «Клавиатурная навигация», §7.2 Ctrl+Z). SysTreeView32
// свои клавиши ест, а BUTTON — тоже, и Ctrl+Z на кнопке молча пропадал бы.
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = stateOf(window);
    if (state == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        state->removeChild(window);
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    if (message == WM_KEYDOWN && state->window != nullptr) {
        const LRESULT handled = ::SendMessageW(state->window, WM_KEYDOWN, wParam, lParam);
        if (handled != 0) return handled;
    }
    for (const ChildProc& entry : state->children) {
        if (entry.hwnd == window && entry.prev != nullptr) {
            return ::CallWindowProcW(entry.prev, window, message, wParam, lParam);
        }
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}


}  // namespace detail


// ---------------------------------------------------------------------------
// CleanupScreen
// ---------------------------------------------------------------------------

struct CleanupScreen::Impl : detail::ViewState {
    Impl() { dpi = static_cast<int>(theme.metrics().dpi); }
};

CleanupScreen::CleanupScreen(Callbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->callbacks = std::move(callbacks);
}

CleanupScreen::~CleanupScreen() { destroy(); }

HWND CleanupScreen::create(HWND parent, int dpi) {
    auto& state = *impl_;
    if (parent == nullptr) return nullptr;
    if (state.window != nullptr) return state.window;
    if (dpi > 0) state.dpi = dpi;

    // SysTreeView32 и SysListView32 без ICC_* не создаются вовсе, а §7 требует
    // именно их. Повторный вызов безвреден.
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    if (::InitCommonControlsEx(&controls) == FALSE) {
        logWin32("ui.cleanup.create", "InitCommonControlsEx", ::GetLastError());
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = 0;
    windowClass.lpfnWndProc = &detail::viewProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = detail::kCleanupViewClass;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logWin32("ui.cleanup.create", "RegisterClassExW", ::GetLastError());
        return nullptr;
    }
    state.window = ::CreateWindowExW(0, detail::kCleanupViewClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                     parent, nullptr, windowClass.hInstance, &state);
    if (state.window == nullptr) {
        logWin32("ui.cleanup.create", "CreateWindowExW(view)", ::GetLastError());
        return nullptr;
    }
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
    return state.window;
}

HWND CleanupScreen::window() const noexcept { return impl_->window; }

void CleanupScreen::destroy() noexcept {
    auto& state = *impl_;
    if (state.window == nullptr) return;
    HWND window = state.window;
    state.window = nullptr;
    state.controlsReady = false;
    // Детей снимает DestroyWindow: каждый пришлёт WM_NCDESTROY и вернёт свою
    // исходную процедуру. Список children здесь не чистим — иначе подкласс
    // остался бы на уничтоженном контроле.
    ::DestroyWindow(window);
    state.nodeRefs.clear();
    state.nodeHandles.clear();
    state.children.clear();
    state.summary = nullptr;
    state.progress = nullptr;
    state.status = nullptr;
    state.details = nullptr;
    state.tree = nullptr;
    state.dryRunTitle = nullptr;
    state.dryRunList = nullptr;
    state.dryRunClose = nullptr;
    state.buttons.fill(nullptr);
}

void CleanupScreen::setDpi(int dpi) {
    auto& state = *impl_;
    if (dpi > 0) state.dpi = dpi;
    state.theme.setDpi(static_cast<unsigned>(state.dpi));
    if (state.window == nullptr) return;
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
}

CleanupViewModel& CleanupScreen::model() noexcept { return impl_->model; }
const CleanupViewModel& CleanupScreen::model() const noexcept { return impl_->model; }

void CleanupScreen::refresh() { impl_->refreshAll(); }

void CleanupScreen::reloadTheme() {
    auto& state = *impl_;
    state.reloadTheme();
    state.refreshAll();
}

void CleanupScreen::publishScanProgress(ScanProgress progress) {
    auto& state = *impl_;
    state.model.publishScanProgress(std::move(progress));
    state.refreshAll();
}

void CleanupScreen::publishCandidates(
    std::shared_ptr<const std::vector<core::CleanupCandidate>> candidates) {
    publishCandidates(std::move(candidates), nullptr);
}

void CleanupScreen::publishCandidates(
    std::shared_ptr<const std::vector<core::CleanupCandidate>> candidates,
    std::shared_ptr<const std::vector<core::CandidateManifest>> manifests) {
    auto& state = *impl_;
    state.model.publishCandidates(std::move(candidates), std::move(manifests));
    state.refreshAll();
}

void CleanupScreen::publishOperationResult(bool ok, std::uint64_t freedBytes) {
    auto& state = *impl_;
    state.model.publishOperationResult(ok, freedBytes);
    state.refreshAll();
}

void CleanupScreen::publishFinished() {
    auto& state = *impl_;
    state.model.publishFinished();
    state.refreshAll();
}

void CleanupScreen::setUndoAvailable(bool available) {
    auto& state = *impl_;
    state.model.setUndoAvailable(available);
    state.refreshAll();
}


}  // namespace mrproper::ui::cleanup
