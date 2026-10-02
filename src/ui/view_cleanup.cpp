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
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "locale.hpp"
#include "mv_bridge.hpp"
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

// Каталог строк грузит оболочка при старте (locale.hpp:456: «до создания
// окон»), но вызова initialize() в проекте нет вообще — проверено поиском по
// src/. Без него подписи экранов остаются ключами («cleanup.tree»,
// «units.byte»), поэтому экран поднимает каталог сам. Идемпотентно и дёшево:
// повторный вызов не нужен, проверяется флаг.
void ensureStrings() noexcept {
    if (!mrproper::ui::isInitialized()) (void)mrproper::ui::initialize();
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

// Слов «в коде» на этом экране больше нет. Раньше здесь стоял двуязычный выбор
// pick(ru, en): четыре пояснения пустого дерева категорий жили в исходнике, а
// строка про набор правил не имела английского варианта вообще — на английском
// экране она оставалась русской. Теперь все они в каталоге строк
// (cleanup.hint.* в src/ui/locale.hpp), вместе с формами множественного числа
// для двух счётчиков: «1 правило, 5 категорий» и «5 правил, 1 категория» —
// разные предложения, и одна форма на оба числа дала бы «1 правило, 1 категорий».

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

double CleanupMetrics::summaryHeightDip() const noexcept {
    // Панель агрегатов = отступ сверху + крупная цифра + три пояснения +
    // отступ снизу. Раньше высота была константой 84 DIP, а четыре строки
    // делили её поровну: на строку крупной цифры (кегль 32 DIP, высота ячейки
    // около 38 px) оставалось 18 px, и заголовок рисовался срезанным сверху.
    return 2.0 * summaryPaddingDip + metricLineHeightDip + 3.0 * bodyLineHeightDip;
}

double CleanupMetrics::toolbarHeightDip() const noexcept {
    // Две строки кнопок плюс зазор между ними и отступы по краям строки.
    return 2.0 * buttonHeightDip + buttonGapDip + 2.0 * toolbarPaddingDip;
}

int CleanupLayout::gapPx(int rowHeight) noexcept { return std::max(2, rowHeight / 6); }

CleanupRect CleanupLayout::buttonInRow(const CleanupRect& row, int index, int count, int gap, int rowTop,
                                      int rowHeight) noexcept {
    if (count <= 0 || row.width <= 0 || rowHeight <= 0) return CleanupRect{};
    const int cell = row.width / count;
    const int extra = row.width - cell * count;
    const int clamped = std::clamp(index, 0, count - 1);
    const int x = row.x + clamped * cell + std::min(clamped, extra);
    const int width = cell + (clamped < extra ? 1 : 0);
    return CleanupRect{x, rowTop, std::max(0, width - gap), rowHeight};
}

CleanupRect CleanupLayout::buttonRow(int row) const noexcept {
    if (buttonHeight_ <= 0 || toolbar_ <= 0 || width_ <= 0) return CleanupRect{};
    // Нижняя строка прижата к низу клиента через отступ: её нижняя граница
    // равна height_ - toolbarPad_ и НИКОГДА не выходит за height_. Верхняя
    // строка стоит на buttonGap_ выше нижней. Обе высоты одинаковы и не
    // превышают половины доступного места (это гарантировано в compute()), так
    // что строки не накладываются даже в окне ниже минимального.
    const int inset = std::min(toolbarPad_, std::max(0, width_ / 8));
    const int bottomRow = height_ - toolbarPad_ - buttonHeight_;
    const int top = (row <= 0) ? bottomRow - buttonGap_ - buttonHeight_ : bottomRow;
    return CleanupRect{inset, top, std::max(0, width_ - 2 * inset), buttonHeight_};
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
    out.summary_ = std::min(out.height_, px(metrics.summaryHeightDip()));
    out.progress_ = std::min(out.height_ - out.summary_, px(metrics.progressHeightDip));
    out.toolbar_ = std::min(out.height_, px(metrics.toolbarHeightDip()));
    out.dryRun_ = dryRunVisible ? std::min(out.height_, px(metrics.dryRunHeightDip)) : 0;

    // Геометрия кнопок — из той же полосы toolbar_. Отступы и зазор сжимаются
    // первыми, высота кнопки — последней и никогда не падает ниже минимума,
    // пока в полосе есть для неё место.
    out.toolbarPad_ = std::clamp(px(metrics.toolbarPaddingDip), 0, out.toolbar_ / 2);
    const int gapWanted = px(metrics.buttonGapDip);
    const int free = std::max(0, out.toolbar_ - 2 * out.toolbarPad_);
    out.buttonGap_ = std::min(gapWanted, std::max(0, free - 2));
    out.buttonHeight_ = std::min(px(metrics.buttonHeightDip), std::max(0, free - out.buttonGap_) / 2);

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
    const CleanupRect row = buttonRow(0);
    return buttonInRow(row, 0, 4, gapPx(row.height), row.y, row.height);
}

CleanupRect CleanupLayout::cancelButtonRect() const noexcept {
    const CleanupRect row = buttonRow(0);
    return buttonInRow(row, 1, 4, gapPx(row.height), row.y, row.height);
}

CleanupRect CleanupLayout::rescanButtonRect() const noexcept {
    const CleanupRect row = buttonRow(0);
    return buttonInRow(row, 2, 4, gapPx(row.height), row.y, row.height);
}

CleanupRect CleanupLayout::undoButtonRect() const noexcept {
    const CleanupRect row = buttonRow(0);
    return buttonInRow(row, 3, 4, gapPx(row.height), row.y, row.height);
}

CleanupRect CleanupLayout::selectAllButtonRect() const noexcept {
    const CleanupRect row = buttonRow(1);
    return buttonInRow(row, 0, 3, gapPx(row.height), row.y, row.height);
}

CleanupRect CleanupLayout::clearSelectionButtonRect() const noexcept {
    const CleanupRect row = buttonRow(1);
    return buttonInRow(row, 1, 3, gapPx(row.height), row.y, row.height);
}

CleanupRect CleanupLayout::showAllButtonRect() const noexcept {
    const CleanupRect row = buttonRow(1);
    return buttonInRow(row, 2, 3, gapPx(row.height), row.y, row.height);
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
    kChildHint,
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

// Высота строки текста по настоящему шрифту, а не по размеру элемента. GDI рисует
// текст в прямоугольнике DT_SINGLELINE и обрезает его по высоте ячейки
// (tmHeight + tmExternalLeading), поэтому именно эта величина решает, влезет ли
// строка. Здесь она измеряется один раз и поступает и в раскладку
// (ViewState::syncMetrics), и в отрисовку: две стороны не должны считать высоту
// строки по-разному, иначе панель агрегатов снова вырастет не по шрифту.
int cleanupFontLineHeightPx(HDC dc, HFONT font) noexcept {
    if (font == nullptr) return 0;
    const HGDIOBJ previous = ::SelectObject(dc, font);
    TEXTMETRICW metrics{};
    const int height = ::GetTextMetricsW(dc, &metrics) != FALSE
                           ? metrics.tmHeight + metrics.tmExternalLeading
                           : 0;
    ::SelectObject(dc, previous);
    return std::max(1, height);
}

// Высота строки шрифта без DC окна: во временном контексте. Нужна раскладке,
// которая считается до отрисовки.
int cleanupFontLineHeight(HFONT font) noexcept {
    HDC dc = ::CreateCompatibleDC(nullptr);
    if (dc == nullptr) return 0;
    const int height = cleanupFontLineHeightPx(dc, font);
    ::DeleteDC(dc);
    return height;
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
    HWND hint{nullptr};
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
    std::shared_ptr<mv::ScreenEndpoint> feed;
    std::shared_ptr<const core::RuleSet> ruleSet;
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

    // Пояснение поверх дерева — ровно тогда, когда дерево пусто. Показывать его
    // всегда нельзя: закрытый текстом скан это хуже, чем дерево.
    void placeHint(const CleanupLayout& layout) {
        if (hint == nullptr) return;
        const bool empty = model.categories().empty();
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const int pad = std::max(2, scale.dip(8.0));
        const CleanupRect treeRect = layout.treeRect();
        int height = treeRect.height;
        if (empty) {
            // Пояснению нужна высота. Дерево без скана занимает полосу в 120 px,
            // а текста в нём четыре строки: рамка оставалась нарисованной, но
            // пустой (проверено снимком окна). Нижняя граница — клиентская
            // область, иначе рамка уезжает под нижние кнопки.
            RECT client{};
            if (::GetClientRect(window, &client) == FALSE) return;
            height = std::max(height, static_cast<int>(scale.dip(150.0)));
            height = std::min(height, std::max(0, static_cast<int>(client.bottom) - treeRect.y - pad));
        }
        const CleanupRect box{treeRect.x + pad, treeRect.y + pad,
                              std::max(treeRect.x + pad, treeRect.x + treeRect.width - pad),
                              treeRect.y + pad + std::max(0, height - 2 * pad)};
        if (empty && !box.empty()) {
            ::SetWindowPos(hint, HWND_TOP, box.x, box.y, box.width, box.height, SWP_NOACTIVATE);
            ::ShowWindow(hint, SW_SHOW);
        } else {
            ::ShowWindow(hint, SW_HIDE);
        }
    }

    // ------------------------------------------------------------------------
    // Набор правил и статусы из фонового потока
    // ------------------------------------------------------------------------
    //
    // Экран «Очистки» получает кандидатов только после скана, а скан запускает
    // человек. Пока скана не было, дерево пусто — и оно обязано объяснять это и
    // называть, что будет искать набор правил (FR-3/FR-4), иначе экран выглядит
    // как «ничего не нашлось».
    void attachFeed() {
        if (feed != nullptr || window == nullptr) return;
        feed = mv::StartupFeed::instance().subscribe(window);
        if (const std::shared_ptr<const core::RuleSet> ready = mv::StartupFeed::instance().ruleSet()) {
            ruleSet = ready;
        }
        mv::StartupFeed::instance().start();
    }

    void detachFeed() noexcept {
        if (feed == nullptr) return;
        mv::StartupFeed::instance().unsubscribe(feed);
        feed.reset();
    }

    void applyFeedFrames() {
        if (feed == nullptr) return;
        bool changed = false;
        mv::Event event;
        while (feed->take(event)) {
            switch (event.kind()) {
            case mv::EventKind::RuleSet: {
                if (const auto rules = event.as<core::RuleSet>()) {
                    ruleSet = rules;
                    changed = true;
                }
                break;
            }
            case mv::EventKind::Notice:
            case mv::EventKind::Error: {
                if (const std::shared_ptr<const std::string> text = event.as<std::string>()) {
                    setChildText(status, *text);
                    changed = true;
                }
                break;
            }
            default:
                break;
            }
        }
        if (changed) refreshAll();
    }

    // Сколько правил и сколько категорий в наборе — для текста пояснения. Ноль
    // означает «набор ещё читается», и тогда текст об этом и говорит.
    [[nodiscard]] std::size_t ruleCount() const noexcept {
        return ruleSet ? ruleSet->rules.size() : 0U;
    }

    [[nodiscard]] std::size_t ruleCategoryCount() const noexcept {
        if (!ruleSet) return 0U;
        std::set<std::string> categories;
        for (const core::Rule& rule : ruleSet->rules) categories.insert(rule.category);
        return categories.size();
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
        placeHint(layout);

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
        syncMetrics();
    }

    // Высоты строк панели агрегатов и высота кнопки — из настоящих шрифтов,
    // а не из констант в метриках. Иначе при масштабе текста Windows 125-200 %
    // крупная цифра переставала помещаться в свою строку (обрезалась сверху), а
    // кнопка оставалась ниже высоты своей подписи. Минимум кнопки при этом не
    // меняется: он задан отдельно (§7.2, доступность), и шрифт может его
    // только увеличить.
    void syncMetrics() {
        const theme::Metrics themeScale = theme::metricsForDpi(static_cast<unsigned>(dpi > 0 ? dpi : 96));
        const int metricLine = cleanupFontLineHeight(fonts[0]);
        const int bodyLine = cleanupFontLineHeight(fonts[2]);
        if (metricLine > 0) metrics.metricLineHeightDip = std::max(1.0, themeScale.undo(metricLine));
        if (bodyLine > 0) metrics.bodyLineHeightDip = std::max(1.0, themeScale.undo(bodyLine));
        const int wanted = bodyLine > 0 ? bodyLine + 2 * std::max(1, themeScale.dip(4.0)) : 0;
        metrics.buttonHeightDip =
            std::max(kMinButtonHeightDip, wanted > 0 ? themeScale.undo(wanted) : metrics.buttonHeightDip);
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
        // Пояснение вместо пустого дерева категорий. Создаётся всегда, показывается
        // только когда дерево пусто: «скана не было» и «дерево сломалось» — это
        // разные ситуации, и без слов они выглядят одинаково.
        hint = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_OWNERDRAW, 0, 0, 0, 0, window,
                                 reinterpret_cast<HMENU>(kChildHint), instance, nullptr);
        const std::array<ControlId, 7> ids{ControlId::Clean,        ControlId::Cancel,      ControlId::Rescan,
                                           ControlId::ShowAll,      ControlId::SelectAll,   ControlId::ClearSelection,
                                           ControlId::Undo};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            buttons[i] = ::CreateWindowExW(0, L"BUTTON", nullptr, childVisible | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                           0, 0, window,
                                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ids[i])), instance,
                                           nullptr);
        }
        const std::array<HWND, 13> created{summary, progress, status, details, tree,  dryRunTitle, dryRunList,
                                           dryRunClose, hint, buttons[0], buttons[1], buttons[2], buttons[3]};
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
            if (child == hint) continue;  // подпись без клавиш: подкласс ей не нужен
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
    // Отступ сверху равен отступу снизу: первая строка — крупная цифра, и раньше
    // она начиналась с половины отступа и уходила под верхний край панели, то
    // есть заголовок рисовался срезанным (дефект M1).
    const int padding = std::max(1, scale.dip(state.metrics.summaryPaddingDip));
    const int top = box.top + padding;
    const int limit = box.bottom - padding;

    // Строка получает столько, сколько просит её шрифт. Крупная цифра — первой
    // и без уступок: если места не хватает, не хватает его пояснениям, а не
    // заголовку. Остаток делят три пояснения пропорционально своим высотам, и
    // последняя строка забирает остаток от целочисленного деления.
    std::array<int, 4> wanted{cleanupFontLineHeightPx(dc, used[0]), cleanupFontLineHeightPx(dc, used[1]),
                              cleanupFontLineHeightPx(dc, used[2]), cleanupFontLineHeightPx(dc, used[3])};
    std::array<int, 4> heights{};
    heights[0] = std::clamp(wanted[0], 0, std::max(0, limit - top));
    const int rest = std::max(0, limit - top - heights[0]);
    const int smallSum = wanted[1] + wanted[2] + wanted[3];
    int taken = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        heights[i] = (i + 1 == lines.size())
                         ? std::max(0, rest - taken)
                         : (smallSum > 0 ? static_cast<int>(static_cast<long long>(wanted[i]) * rest / smallSum) : 0);
        taken += heights[i];
    }

    int y = top;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        RECT line{box.left + padding, y, box.right - padding, y + heights[i]};
        y += heights[i];
        if (line.bottom <= line.top) continue;
        if (used[i] != nullptr) ::SelectObject(dc, used[i]);
        ::SetTextColor(dc, theme::colorRef(colors[i]));
        ::DrawTextW(dc, lines[i].c_str(), -1, &line,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    return TRUE;
}

// Пояснение вместо пустого дерева категорий: заголовок, причина, что будет
// искать набор правил и куда нажать.
//
// Слова берутся из двух источников и это важно: «скан ещё не выполнялся» — из
// состояния модели, «правил N по M категориям» — из набора, который пришёл из
// фонового потока. Человек видит, что приложение знает, что искать, и жмёт
// одну кнопку, вместо того чтобы гадать, не сломалось ли что-то.
LRESULT drawHint(ViewState& state, const DRAWITEMSTRUCT& draw) {
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

    const int pad = std::max(2, scale.dip(12.0));
    const int titleHeight = std::max(10, scale.dip(22.0));
    RECT inner{box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
    if (inner.bottom <= inner.top) return TRUE;

    const bool scanning = state.model.progress().busy();
    const std::size_t rules = state.ruleCount();
    const std::size_t categories = state.ruleCategoryCount();

    std::vector<std::wstring> lines;
    lines.push_back(toWide(tr(StringId::kCleanupHintNoScan)));
    if (scanning) {
        lines.push_back(toWide(tr(StringId::kCleanupHintScanning)));
    } else {
        lines.push_back(toWide(tr(StringId::kCleanupHintIdle)));
    }
    if (rules > 0) {
        // Два счётчика — две строки с формой множественного числа. Запятая между
        // ними одинакова в обоих языках и в каталоге не лежит: каталог хранит
        // слова, а не типографику.
        const std::string ruleSet =
            tr(StringId::kCleanupHintRuleSetPrefix) +
            trPlural(StringId::kCleanupHintRuleCount, static_cast<std::uint64_t>(rules)) + ", " +
            trPlural(StringId::kCleanupHintCategoryCount, static_cast<std::uint64_t>(categories)) +
            tr(StringId::kCleanupHintRuleSetTail);
        lines.push_back(toWide(ruleSet));
    } else {
        lines.push_back(toWide(tr(StringId::kOverviewNoteRulesLoading)));
    }
    lines.push_back(toWide(tr(StringId::kCleanupHintPressRescan, tr(StringId::kActionRescan))));

    ::SetBkMode(dc, TRANSPARENT);
    if (state.fonts[0] != nullptr) ::SelectObject(dc, state.fonts[0]);
    ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
    RECT title{inner.left, inner.top, inner.right, std::min(inner.bottom, inner.top + titleHeight)};
    ::DrawTextW(dc, lines.front().c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    if (state.fonts[2] != nullptr) ::SelectObject(dc, state.fonts[2]);
    ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
    constexpr DWORD wrap = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;
    int y = title.bottom + scale.dip(4.0);
    for (std::size_t i = 1; i < lines.size(); ++i) {
        RECT line{inner.left, y, inner.right, inner.bottom};
        (void)::DrawTextW(dc, lines[i].c_str(), -1, &line, wrap | DT_CALCRECT);
        if (line.bottom <= y) break;
        (void)::DrawTextW(dc, lines[i].c_str(), -1, &line, wrap);
        y = line.bottom + scale.dip(4.0);
        if (y >= inner.bottom) break;
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
        case WM_DPICHANGED:
        case WM_DPICHANGED_BEFOREPARENT:
        case WM_DPICHANGED_AFTERPARENT: {
            // Per-monitor v2 (§5) сообщает смену масштаба дочерним окнам
            // парами BEFORE/AFTERPARENT, и в этой паре lParameter НЕ является
            // прямоугольником: размер окна пересчитывает система сама, от нас
            // нужны только DPI, шрифты и раскладка. Без этих двух веток
            // экран остаётся в метриках прежнего масштаба — это измерено на
            // эмуляции 150 % (WM_DPICHANGED_AFTERPARENT, тот же приём, что в
            // tools\ui-smoke.ps1): клиент 998x1065, а панель агрегатов 101 px и
            // кнопки 32 px, то есть всё посчитано в пикселях 96 DPI.
            const bool hasSuggested = (message == WM_DPICHANGED);
            const auto* suggested = hasSuggested ? reinterpret_cast<const RECT*>(lParam) : nullptr;
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
            if (draw->CtlType == ODT_STATIC && draw->CtlID == kChildHint &&
                draw->hwndItem == state->hint) {
                return drawHint(*state, *draw);
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
        case mv::kFeedMessage:
            // Набор правил или причина отказа. Модель экрана не трогаем: до скана
            // ей нечего знать, текст пояснения строится из набора (§6.1).
            state->applyFeedFrames();
            return 0;
        case WM_ERASEBKGND: {
            // Дети перекрывают окно целиком; стирать собственную поверхность
            // незачем, а лишнее стирание мигает при перерисовке дерева.
            return 1;
        }
        case WM_PAINT: {
            // Фон окна — из темы. Раньше WM_PAINT у экрана «Очистка» не
            // обрабатывался вовсе, а WM_ERASEBKGND отвечал «стёрто», так что
            // область между контролами оставалась тем цветом, что закрасила
            // системная кисть класса окна: в тёмной теме это белый прямоугольник
            // во всю страницу (проверено снимком окна).
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(window, &paint);
            if (dc != nullptr) {
                RECT client{};
                ::GetClientRect(window, &client);
                if (state->surfaceBrush != nullptr) {
                    ::FillRect(dc, &client, state->surfaceBrush);
                }
            }
            (void)::EndPaint(window, &paint);
            return 0;
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
    ensureStrings();
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
    state.attachFeed();
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
    state.detachFeed();
    state.nodeRefs.clear();
    state.nodeHandles.clear();
    state.children.clear();
    state.summary = nullptr;
    state.progress = nullptr;
    state.status = nullptr;
    state.details = nullptr;
    state.tree = nullptr;
    state.hint = nullptr;
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

// ---------------------------------------------------------------------------
// Экран «Обзор»
// ---------------------------------------------------------------------------
//
// Почему он здесь, объяснено в заголовке (view_cleanup.hpp). Здесь только
// рисование: четыре плитки с числами, версия набора правил и — когда скана не
// было — прямое указание, что нажать.
//
// Рисование целиком на GDI в WM_PAINT по одной причине: этот экран не должен
// зависеть от Direct2D. В отличие от карты разделов у него нет графики собственной
// природы, а §7 ADR-003 прямо запрещает рисовать интерфейс целиком вручную там,
// где хватает контролов. Здесь контролов нет по существу (плитки и текст), но
// Direct2D-рендерер поднимается один на процесс и принадлежит экрану «Диски» —
// завязывать на него сводку значило бы получить пустую страницу там, где
// рендерер не поднялся (нет D3D11), а §5 требует обратного.
namespace mrproper::ui::overview {
namespace {

constexpr wchar_t kOverviewViewClass[] = L"MrProper.OverviewView";

// Идентификаторы кнопок. Диапазон свой (§7.1), чтобы WM_COMMAND экрана «Диски»
// или «Очистки» не пришёл сюда под тем же номером.
enum : int { kOverviewScanButton = 1, kOverviewDisksButton = 2 };

// «Нет данных» в плитке. Не перевод и не пустая строка: ноль в плитке
// свободного места читается как «диск пуст», а это ложь, когда размер просто не
// пришёл (FR-1: устройство могло не ответить). Ровно тот же приём и то же
// обоснование, что в src/ui/view_disks.cpp (kNoData).
constexpr std::string_view kNoData = "—";

std::string sizeOrDash(std::uint64_t bytes, bool known) {
    return known ? formatBytes(bytes) : std::string(kNoData);
}

struct ViewState {
    OverviewScreen::Callbacks callbacks;
    OverviewSnapshot snapshot;
    theme::Theme theme;
    int dpi{96};

    HWND window{nullptr};
    HWND scanButton{nullptr};
    HWND disksButton{nullptr};
    std::array<HFONT, 5> fonts{};
    HBRUSH surfaceBrush{nullptr};
    std::shared_ptr<mv::ScreenEndpoint> feed;
    bool controlsReady{false};
    bool syncing{false};

    ~ViewState() {
        for (HFONT& font : fonts) {
            if (font != nullptr) ::DeleteObject(font);
        }
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
    }

    [[nodiscard]] theme::Metrics scale() const {
        return theme::metricsForDpi(static_cast<unsigned>(dpi > 0 ? dpi : 96));
    }

    void applyFonts() {
        const theme::Typography typography = theme::makeTypography(static_cast<unsigned>(dpi));
        const std::array<theme::FontRole, 5> roles{theme::FontRole::Title, theme::FontRole::Metric,
                                                   theme::FontRole::BodyStrong, theme::FontRole::Body,
                                                   theme::FontRole::Caption};
        for (std::size_t i = 0; i < fonts.size(); ++i) {
            if (fonts[i] != nullptr) ::DeleteObject(fonts[i]);
            const LOGFONTW logFont = typography.forRole(roles[i]).toLogFont(static_cast<unsigned>(dpi));
            fonts[i] = ::CreateFontIndirectW(&logFont);
        }
    }

    void applyPalette() {
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
        surfaceBrush = ::CreateSolidBrush(theme::colorRef(theme.palette().surface));
    }

    // --- Раскладка ----------------------------------------------------------
    //
    // Сетка из двух рядов по две плитки, потом пояснение. Числа не «плавают»:
    // ширина плитки фиксирована, а текст внутри выровнен по левому краю, чтобы
    // десятки гигабайт и единицы не прыгали при обновлении (SPEC §12).
    struct Tile {
        RECT rect;
        std::wstring name;    // что это за число: «Диски», «Свободно», ...
        std::wstring value;
        std::wstring caption;
    };

    [[nodiscard]] std::vector<Tile> tiles() const {
        const theme::Metrics metrics = scale();
        RECT client{};
        if (window == nullptr || ::GetClientRect(window, &client) == FALSE) return {};
        const int pad = std::max(4, metrics.dip(12.0));
        const int gap = std::max(2, metrics.dip(8.0));
        const int tileHeight = std::max(24, metrics.dip(64.0));
        const int columnWidth = (client.right - pad * 2 - gap) / 2;

        // Четыре плитки в фиксированном порядке: диски, свободно, кандидаты,
        // освободится. «Нет данных» и «нет скана» — это разные надписи, а не
        // ноль: ноль после скана и ноль без скана значат разное (§4 FR-3).
        // Размер печатается только когда он известен, иначе прочерк: плитка
        // «Диски» на хосте без прав администратора получала «0 Б» вместо «—»
        // (то же, что делает и не делает экран «Диски»: sizeOrDash).
        const std::string disks =
            snapshot.inventoryKnown ? std::to_string(snapshot.diskCount) : tr(StringId::kOverviewValueReading);
        const std::string freeText = sizeOrDash(snapshot.freeBytes, snapshot.freeKnown);
        const std::string candidates =
            snapshot.scanned ? std::to_string(snapshot.candidates) : tr(StringId::kOverviewValueNoScan);
        const std::string reclaim = sizeOrDash(snapshot.reclaimableBytes, snapshot.scanned);

        std::vector<Tile> out;
        const int top = pad + std::max(10, metrics.dip(24.0)) + std::max(2, metrics.dip(8.0));
        out.push_back(Tile{{pad, top, pad + columnWidth, top + tileHeight},
                           toWide(tr(StringId::kOverviewTileDisks)), toWide(disks),
                           toWide(snapshot.inventoryKnown
                                      ? std::to_string(snapshot.diskCount) + " · " +
                                            sizeOrDash(snapshot.totalBytes, snapshot.sizeKnown)
                                      : tr(StringId::kOverviewCaptionDisksReading))});
        out.push_back(Tile{{pad + columnWidth + gap, top, client.right - pad, top + tileHeight},
                           toWide(tr(StringId::kOverviewTileFree)), toWide(freeText),
                           toWide(tr(StringId::kOverviewCaptionFree))});
        out.push_back(Tile{{pad, top + tileHeight + gap, pad + columnWidth, top + tileHeight * 2 + gap},
                           toWide(tr(StringId::kOverviewTileCandidates)), toWide(candidates),
                           toWide(tr(StringId::kOverviewCaptionCandidates))});
        out.push_back(Tile{{pad + columnWidth + gap, top + tileHeight + gap, client.right - pad,
                            top + tileHeight * 2 + gap},
                           toWide(tr(StringId::kOverviewTileReclaimable)), toWide(reclaim),
                           toWide(tr(StringId::kOverviewCaptionReclaimable))});
        return out;
    }

    [[nodiscard]] std::vector<std::wstring> notes() const {
        std::vector<std::wstring> out;
        out.push_back(toWide(snapshot.scanned ? tr(StringId::kOverviewNoteScanDone)
                                             : tr(StringId::kOverviewNoteNeedScan)));
        if (snapshot.ruleCount > 0) {
            out.push_back(toWide(tr(StringId::kOverviewNoteRules,
                                    core::StringArgs{std::to_string(snapshot.ruleCount), snapshot.ruleVersion})));
        } else {
            out.push_back(toWide(tr(StringId::kOverviewNoteRulesLoading)));
        }
        return out;
    }

    // --- Данные -------------------------------------------------------------

    void attachFeed() {
        if (feed != nullptr || window == nullptr) return;
        feed = mv::StartupFeed::instance().subscribe(window);
        if (const std::shared_ptr<const core::DiskInventory> inventory = mv::StartupFeed::instance().inventory()) {
            applyInventory(*inventory);
        }
        if (const std::shared_ptr<const core::RuleSet> rules = mv::StartupFeed::instance().ruleSet()) {
            applyRules(*rules);
        }
        mv::StartupFeed::instance().start();
    }

    void detachFeed() noexcept {
        if (feed == nullptr) return;
        mv::StartupFeed::instance().unsubscribe(feed);
        feed.reset();
    }

    void applyInventory(const core::DiskInventory& inventory) {
        // Агрегаты берёт core::disk_model, а не экран: те же числа показывает
        // страница «Диски» и пишет отчёт (§11.4 — одно число на все поверхности).
        const core::InventoryUsage& usage = inventory.usage();
        snapshot.diskCount = inventory.disks().size();
        snapshot.totalBytes = usage.totalBytes;
        snapshot.freeBytes = usage.freeBytes;
        snapshot.inventoryKnown = true;
        // «Прочитано» не значит «известно». Хост без прав администратора
        // возвращает пустую карту: freeKnown у неё false (томов нет), и печать
        // 0 байт значила бы «свободно 0», то есть «диск полон».
        snapshot.freeKnown = usage.freeKnown;
        snapshot.sizeKnown = usage.totalBytes > 0;
    }

    void applyRules(const core::RuleSet& rules) {
        snapshot.ruleCount = rules.rules.size();
        snapshot.ruleVersion = rules.version;
    }

    void applyFeedFrames() {
        if (feed == nullptr) return;
        bool changed = false;
        mv::Event event;
        while (feed->take(event)) {
            switch (event.kind()) {
            case mv::EventKind::Inventory: {
                if (const auto inventory = event.as<core::DiskInventory>()) {
                    applyInventory(*inventory);
                    changed = true;
                }
                break;
            }
            case mv::EventKind::RuleSet: {
                if (const auto rules = event.as<core::RuleSet>()) {
                    applyRules(*rules);
                    changed = true;
                }
                break;
            }
            default:
                break;
            }
        }
        if (changed) refresh();
    }

    // --- Рисование ----------------------------------------------------------

    void draw(HDC dc) {
        const theme::Palette& palette = theme.palette();
        const theme::Metrics metrics = scale();
        RECT client{};
        (void)::GetClientRect(window, &client);
        HBRUSH surface = ::CreateSolidBrush(theme::colorRef(palette.surface));
        (void)::FillRect(dc, &client, surface);
        ::DeleteObject(surface);

        ::SetBkMode(dc, TRANSPARENT);
        constexpr DWORD single = DT_LEFT | DT_SINGLELINE | DT_NOPREFIX;
        constexpr DWORD wrap = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;

        int y = std::max(2, metrics.dip(10.0));
        const int pad = std::max(2, metrics.dip(12.0));
        if (fonts[0] != nullptr) ::SelectObject(dc, fonts[0]);
        ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
        const std::wstring title = toWide(tr(StringId::kOverviewTitle));
        RECT titleRect{pad, y, client.right - pad, y + std::max(10, metrics.dip(24.0))};
        (void)::DrawTextW(dc, title.c_str(), -1, &titleRect, single | DT_VCENTER);
        y = titleRect.bottom + metrics.dip(6.0);

        for (const Tile& tile : tiles()) {
            HBRUSH fill = ::CreateSolidBrush(theme::colorRef(palette.surfaceAlt));
            (void)::FillRect(dc, &tile.rect, fill);
            ::DeleteObject(fill);
            HBRUSH border = ::CreateSolidBrush(theme::colorRef(palette.border));
            (void)::FrameRect(dc, &tile.rect, border);
            ::DeleteObject(border);

            const int inset = std::max(2, pad / 2);
            // Название плитки — той же вторичной краской, что и подпись: плитка
            // читается как «подпись · число · детали», и без названия «1» и «0 Б»
            // не о чем.
            if (fonts[3] != nullptr) ::SelectObject(dc, fonts[3]);
            ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
            RECT name{tile.rect.left + inset, tile.rect.top + metrics.dip(4.0), tile.rect.right - inset,
                      tile.rect.top + metrics.dip(18.0)};
            (void)::DrawTextW(dc, tile.name.c_str(), -1, &name, single | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Число — основным цветом, а не акцентом: на тёмной теме акцент
            // тёмный, и «0 Б» читалось как серое пятно (проверено снимком).
            if (fonts[1] != nullptr) ::SelectObject(dc, fonts[1]);
            ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
            RECT value{tile.rect.left + inset, name.bottom, tile.rect.right - inset,
                       tile.rect.top + metrics.dip(44.0)};
            (void)::DrawTextW(dc, tile.value.c_str(), -1, &value, single | DT_VCENTER | DT_END_ELLIPSIS);

            if (fonts[4] != nullptr) ::SelectObject(dc, fonts[4]);
            ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
            RECT caption{tile.rect.left + inset, value.bottom, tile.rect.right - inset, tile.rect.bottom};
            (void)::DrawTextW(dc, tile.caption.c_str(), -1, &caption, single | DT_END_ELLIPSIS);
        }
        y += std::max(24, metrics.dip(64.0)) * 3 + std::max(2, metrics.dip(8.0)) * 2;

        if (fonts[3] != nullptr) ::SelectObject(dc, fonts[3]);
        ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
        for (const std::wstring& note : notes()) {
            RECT line{pad, y, client.right - pad, client.bottom};
            (void)::DrawTextW(dc, note.c_str(), -1, &line, wrap | DT_CALCRECT);
            if (line.bottom <= y) break;
            (void)::DrawTextW(dc, note.c_str(), -1, &line, wrap);
            y = line.bottom + metrics.dip(4.0);
            if (y >= client.bottom) break;
        }
    }

    // --- Раскладка контролов -------------------------------------------------

    void layout() {
        if (window == nullptr) return;
        RECT client{};
        (void)::GetClientRect(window, &client);
        const theme::Metrics metrics = scale();
        const int pad = std::max(2, metrics.dip(12.0));
        const int gap = std::max(2, metrics.dip(8.0));
        const int height = std::max(16, metrics.dip(30.0));
        const int width = std::max(60, metrics.dip(150.0));
        const int y = client.bottom - pad - height;
        if (scanButton != nullptr) {
            ::SetWindowPos(scanButton, nullptr, pad, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            ::ShowWindow(scanButton, SW_SHOW);
        }
        if (disksButton != nullptr) {
            ::SetWindowPos(disksButton, nullptr, pad + width + gap, y, width, height,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            ::ShowWindow(disksButton, SW_SHOW);
        }
    }

    void refresh() {
        if (!controlsReady) return;
        layout();
        (void)::InvalidateRect(window, nullptr, FALSE);
    }
};

void logOverview(core::LogLevel level, std::string_view event, std::string_view message) noexcept {
    core::Logger::instance().write(level, event, message, core::LogFields{});
}

void logOverviewWin32(std::string_view event, std::string_view where, unsigned long code) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField("where", where));
    fields.push_back(core::logField("code", code));
    core::Logger::instance().write(core::LogLevel::Warn, event, "Win32 call failed", std::move(fields));
}

// Тот же предостерегающий вызов каталога, что и на экране «Очистка»: он живёт в
// том же файле, и подписи обзора обязаны быть подписями, а не ключами.
void ensureOverviewStrings() noexcept {
    if (!mrproper::ui::isInitialized()) (void)mrproper::ui::initialize();
}

ViewState* stateOf(HWND window) {
    return reinterpret_cast<ViewState*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ViewState* state = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = create != nullptr ? static_cast<ViewState*>(create->lpCreateParams) : nullptr;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (state != nullptr) state->window = window;
    } else {
        state = stateOf(window);
    }

    try {
        switch (message) {
        case WM_CREATE:
            return 0;
        case WM_SIZE:
            if (state != nullptr) state->refresh();
            return 0;
        case WM_ERASEBKGND:
            // Свою поверхность закрашиваем в WM_PAINT целиком.
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(window, &paint);
            if (dc != nullptr && state != nullptr) state->draw(dc);
            (void)::EndPaint(window, &paint);
            return 0;
        }
        case WM_COMMAND: {
            if (state == nullptr) break;
            const WORD id = LOWORD(wParam);
            const WORD notification = HIWORD(wParam);
            if (notification != BN_CLICKED) break;
            if (id == kOverviewScanButton && state->callbacks.onScan) state->callbacks.onScan();
            if (id == kOverviewDisksButton && state->callbacks.onShowDisks) state->callbacks.onShowDisks();
            return 0;
        }
        case mv::kFeedMessage:
            if (state != nullptr) state->applyFeedFrames();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE: {
            if (state == nullptr) break;
            if (theme::classifyMessage(message, wParam, lParam) == theme::Change::None) break;
            state->theme.reload();
            state->applyPalette();
            state->applyFonts();
            state->refresh();
            return 0;
        }
        case WM_DESTROY:
            if (state != nullptr) state->controlsReady = false;
            return 0;
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
    } catch (const std::exception& error) {
        logOverview(core::LogLevel::Error, "ui.overview.exception", error.what());
        return message == WM_CREATE ? -1 : 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

struct OverviewScreen::Impl : ViewState {
    Impl() { dpi = static_cast<int>(theme.metrics().dpi); }
};

OverviewScreen::OverviewScreen(Callbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->callbacks = std::move(callbacks);
}

OverviewScreen::~OverviewScreen() { destroy(); }

HWND OverviewScreen::create(HWND parent, int dpi) {
    auto& state = *impl_;
    if (parent == nullptr) return nullptr;
    ensureOverviewStrings();
    if (state.window != nullptr) return state.window;
    if (dpi > 0) state.dpi = dpi;

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    if (::InitCommonControlsEx(&controls) == FALSE) {
        logOverviewWin32("ui.overview.create", "InitCommonControlsEx", ::GetLastError());
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = 0;
    windowClass.lpfnWndProc = &viewProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = kOverviewViewClass;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logOverviewWin32("ui.overview.create", "RegisterClassExW", ::GetLastError());
        return nullptr;
    }
    state.window = ::CreateWindowExW(0, kOverviewViewClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                                     nullptr, windowClass.hInstance, &state);
    if (state.window == nullptr) {
        logOverviewWin32("ui.overview.create", "CreateWindowExW(view)", ::GetLastError());
        return nullptr;
    }

    state.scanButton = ::CreateWindowExW(0, L"BUTTON", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                         0, 0, 0, state.window,
                                         reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kOverviewScanButton)),
                                         windowClass.hInstance, nullptr);
    state.disksButton = ::CreateWindowExW(0, L"BUTTON", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                          0, 0, 0, state.window,
                                          reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kOverviewDisksButton)),
                                          windowClass.hInstance, nullptr);
    if (state.scanButton == nullptr || state.disksButton == nullptr) {
        logOverviewWin32("ui.overview.create", "CreateWindowExW(button)", ::GetLastError());
        return nullptr;
    }
    ::SetWindowTextW(state.scanButton, toWide(tr(StringId::kActionScan)).c_str());
    ::SetWindowTextW(state.disksButton, toWide(tr(StringId::kOverviewActionOpenDisks)).c_str());
    state.applyPalette();
    state.applyFonts();
    state.controlsReady = true;
    state.attachFeed();
    state.refresh();
    return state.window;
}

HWND OverviewScreen::window() const noexcept { return impl_->window; }

void OverviewScreen::destroy() noexcept {
    auto& state = *impl_;
    if (state.window == nullptr) return;
    HWND window = state.window;
    state.window = nullptr;
    state.controlsReady = false;
    ::DestroyWindow(window);
    state.detachFeed();
    state.scanButton = nullptr;
    state.disksButton = nullptr;
}

void OverviewScreen::setDpi(int dpi) {
    auto& state = *impl_;
    if (dpi > 0) state.dpi = dpi;
    state.theme.setDpi(static_cast<unsigned>(state.dpi));
    if (state.window == nullptr) return;
    state.applyPalette();
    state.applyFonts();
    state.refresh();
}

void OverviewScreen::reloadTheme() {
    auto& state = *impl_;
    state.theme.reload();
    state.applyPalette();
    state.applyFonts();
    state.refresh();
}

void OverviewScreen::refresh() { impl_->refresh(); }

void OverviewScreen::publishScanSummary(std::size_t candidates, std::uint64_t reclaimableBytes, bool scanned) {
    auto& state = *impl_;
    state.snapshot.candidates = candidates;
    state.snapshot.reclaimableBytes = reclaimableBytes;
    state.snapshot.scanned = scanned;
    state.refresh();
}

const OverviewSnapshot& OverviewScreen::snapshot() const noexcept { return impl_->snapshot; }

}  // namespace mrproper::ui::overview
