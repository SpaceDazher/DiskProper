// MrProper — экран «Настройки»: правила, уровни риска, версия набора,
// проверка обновлений, сброс к встроенному набору.
//
// Разбор решений — в view_settings.hpp. Здесь код в порядке заголовка:
// вспомогательное → раскладка → модель → окно.

#include "view_settings.hpp"

#include <commctrl.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM: позиция мыши в WM_LBUTTONUP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/log.hpp"
#include "locale.hpp"
#include "mv_bridge.hpp"
#include "theme.hpp"

namespace mrproper::ui::settings {

using core::Language;
using core::SafetyLevel;

namespace {

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------
//
// Макросы MRP_LOG_* из core/log.hpp непригодны: logFieldList раскрывает пакет в
// вызов logField по одному аргументу, поэтому любое поле даёт C2661. Собираем
// поля явно — тем же способом, что и остальные экраны слоя.
void logEvent(core::LogLevel level, std::string_view event, std::string_view message) noexcept {
    core::Logger::instance().write(level, event, message, core::LogFields{});
}

void logWin32(std::string_view event, std::string_view where, unsigned long code) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField("where", where));
    fields.push_back(core::logField("code", code));
    core::Logger::instance().write(core::LogLevel::Warn, event, "Win32 call failed", std::move(fields));
}

// Каталог строк должен быть поднят до первого контрола: без initialize()
// (вызова нет ни в одном файле проекта — проверено поиском по src/) подписи
// колонок и строки состояния остаются ключами. Идемпотентно, поэтому проверка
// флага вместо повторной загрузки.
void ensureStrings() noexcept {
    if (!mrproper::ui::isInitialized()) (void)mrproper::ui::initialize();
}

// ---------------------------------------------------------------------------
// Строки
// ---------------------------------------------------------------------------
//
// Все 47 строк этого экрана лежат в каталоге (src/ui/locale.hpp, ключи
// settings.*), а не в таблице экрана. Таблица kLocalTexts была здесь потому, что
// ui/locale.hpp на момент её написания был чужим файлом, и строки с
// ключами settings.* пришлось положить рядом с экраном. Побочный эффект был
// измеримый: ключи были, переводы были, а selfCheck() их не видел — «каталог
// полон» оставалось зелёным при 47 строках, которые переводчик править не мог.
//
// Поэтому ниже только чтение: ключ приходит из кода, перевод — из каталога.
// Ключ, которого в каталоге нет, tr() отдаёт как есть: это видно на экране и
// находится поиском по исходникам, что лучше молчаливой пустой строки.
std::string text(std::string_view key) { return tr(key); }

// Перевод с подстановкой. Шаблон берётся тем же text(), а формат — общий
// core::formatTemplate, поэтому «{0}» одинаково работает и в строках каталога, и
// в строках, собранных экраном.
std::string text(std::string_view key, const core::StringArgs& args) {
    return core::formatTemplate(text(key), args);
}

std::string joinWith(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.append(separator);
        out.append(parts[i]);
    }
    return out;
}

std::string asciiLower(std::string_view value) {
    std::string out(value);
    for (char& character : out) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return out;
}

bool containsFold(const std::string& haystackLower, const std::string& needleLower) {
    return needleLower.empty() || haystackLower.find(needleLower) != std::string::npos;
}

std::int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string countText(std::size_t value) { return formatCount(static_cast<std::uint64_t>(value)); }

// Дата в формате локали языка интерфейса (§5 «даты/числа через
// GetLocaleInfoEx»). В UI-потоке, без системных вызовов в горячем цикле.
std::string dateText(std::int64_t unixSecondsUtc) {
    if (unixSecondsUtc <= 0) return text("settings.neverChecked");
    return toUtf8(formatDate(unixSecondsUtc, DateStyle::DateTime));
}

// Правило без снимка набора: FR-9 разрешает работать на встроенном наборе, но
// пустой список правил на экране настроек — это «приложение сломано», поэтому об
// отсутствии снимка говорим явно и в строке состояния, и в списке.
bool isSafetyLevel(core::SafetyLevel level) noexcept {
    return level == SafetyLevel::Safe || level == SafetyLevel::Review || level == SafetyLevel::Risky;
}

std::size_t safetyIndex(SafetyLevel level) noexcept {
    switch (level) {
    case SafetyLevel::Safe: return 0;
    case SafetyLevel::Review: return 1;
    case SafetyLevel::Risky: return 2;
    }
    return 1;
}

SafetyLevel safetyFromIndex(std::size_t index) noexcept {
    switch (index) {
    case 0: return SafetyLevel::Safe;
    case 2: return SafetyLevel::Risky;
    default: break;
    }
    return SafetyLevel::Review;
}

std::string safetyToken(SafetyLevel level) { return std::to_string(safetyIndex(level)); }

}  // namespace

// ---------------------------------------------------------------------------
// Идентификаторы команд
// ---------------------------------------------------------------------------

bool isSettingsControl(WORD controlId) noexcept {
    return controlId >= static_cast<WORD>(ControlId::First) && controlId <= static_cast<WORD>(ControlId::Last);
}

// ---------------------------------------------------------------------------
// Раскладка
// ---------------------------------------------------------------------------

bool SettingsRect::empty() const noexcept { return width <= 0 || height <= 0; }

bool SettingsRect::contains(int px, int py) const noexcept {
    return px >= x && py >= y && px < x + width && py < y + height;
}

std::vector<int> SettingsLayout::fitWidths(int availablePx, const std::vector<int>& desiredPx, int gapPx,
                                           int minWidthPx) {
    std::vector<int> widths;
    if (desiredPx.empty()) return widths;
    const std::size_t count = desiredPx.size();
    const long long gaps = static_cast<long long>(std::max(0, gapPx)) * static_cast<long long>(count - 1U);
    const long long usable = static_cast<long long>(availablePx) - gaps;
    if (usable <= 0) {
        // Места нет совсем: каждой кнопке — минимум, чтобы её было видно, а
        // лишние кнопки вызывающий прячет по buttonCount().
        widths.assign(count, std::max(1, minWidthPx));
        return widths;
    }

    long long sum = 0;
    for (const int value : desiredPx) sum += std::max(0, value);
    widths.reserve(count);
    if (sum <= usable) {
        for (const int value : desiredPx) widths.push_back(std::max(1, value));
        return widths;
    }

    // Не помещаются — жмём пропорционально, но не ниже минимума: кнопка без
    // подписи хуже кнопки с многоточием, но её всё ещё видно и можно нажать.
    const long long floor = std::min<long long>(std::max(1, minWidthPx), usable);
    long long used = 0;
    for (const int value : desiredPx) {
        long long scaled = static_cast<long long>(std::max(0, value)) * usable / std::max<long long>(1, sum);
        scaled = std::max(floor, scaled);
        used += scaled;
        widths.push_back(static_cast<int>(std::min(scaled, usable)));
    }
    // Разницу отдаём первой кнопке: иначе сумма меньше доступной на кнопки- gap,
    // и строка панели визуально короче остальных.
    if (!widths.empty() && used < usable) {
        widths.front() += static_cast<int>(usable - used);
    }
    return widths;
}

SettingsLayout SettingsLayout::compute(const SettingsMetrics& metrics, int dpi, int clientWidthPx,
                                       int clientHeightPx, const std::vector<int>& buttonWidthsPx) {
    SettingsLayout out;
    out.width_ = std::max(0, clientWidthPx);
    out.height_ = std::max(0, clientHeightPx);

    const int effectiveDpi = dpi > 0 ? dpi : kDefaultDpi;
    const auto px = [&effectiveDpi](double dip) { return dipToPx(dip, effectiveDpi); };

    out.padding_ = std::max(0, px(metrics.paddingDip));
    out.gap_ = std::max(0, px(metrics.gapDip));
    out.buttonGap_ = std::max(0, px(metrics.buttonGapDip));
    out.rowHeight_ = std::max(1, px(metrics.toolbarRowHeightDip));
    out.cramped_ = out.width_ < px(metrics.minWidthDip) || out.height_ < px(metrics.minHeightDip);

    const int contentLeft = out.padding_;
    const int contentWidth = std::max(0, out.width_ - 2 * out.padding_);

    // Ширины кнопок: измеренные подписи, а при их отсутствии (первый расчёт до
    // создания контролов) — равные доли. Ровно kToolbarButtonCount: раскладка
    // обязана знать число мест заранее, иначе кнопки наезжают друг на друга.
    std::vector<int> wanted = buttonWidthsPx;
    if (wanted.size() != kToolbarButtonCount) {
        const int share =
            std::max(1, (contentWidth - out.buttonGap_ * static_cast<int>(kToolbarRow1Count - 1U)) /
                            static_cast<int>(kToolbarRow1Count));
        wanted.assign(kToolbarButtonCount, share);
    }
    const int minButton = std::max(1, px(metrics.buttonMinWidthDip));
    const std::vector<int> fitted = fitWidths(contentWidth, wanted, out.buttonGap_, minButton);
    out.row1Widths_.assign(fitted.begin(),
                           fitted.begin() + static_cast<std::ptrdiff_t>(kToolbarRow1Count));
    out.row2Widths_.assign(fitted.begin() + static_cast<std::ptrdiff_t>(kToolbarRow1Count), fitted.end());
    out.row1Count_ = out.row1Widths_.size();
    out.row2Count_ = out.row2Widths_.size();

    // Сверху вниз: три строки панели, строка состояния, фильтр, список (единственная
    // гибкая часть), пояснение и «о программе». Снизу ничего не прижато к краю:
    // настройки читают сверху вниз, и прижатая снизу кнопка уводила бы последнюю
    // строку под список.
    const int toolbar = out.rowHeight_ * static_cast<int>(kToolbarRowCount) + out.gap_ * 2;
    int top = out.padding_;
    out.row1Top_ = top;
    out.row2Top_ = top + out.rowHeight_ + out.gap_ / 2;
    out.row3Top_ = top + 2 * (out.rowHeight_ + out.gap_ / 2);
    top += toolbar;

    out.statusHeight_ = std::max(0, px(metrics.statusHeightDip));
    out.statusTop_ = top;
    out.filterHeight_ = std::max(0, px(metrics.filterHeightDip));
    out.filterTop_ = out.statusTop_ + out.statusHeight_ + out.gap_;
    out.detailsHeight_ = std::max(0, px(metrics.detailsHeightDip));
    out.aboutHeight_ = std::max(0, px(metrics.aboutHeightDip));

    const int listTop = out.filterTop_ + out.filterHeight_ + out.gap_;
    int bottom = out.height_ - out.padding_ - out.aboutHeight_ - out.gap_ - out.detailsHeight_ - out.gap_;
    if (bottom < listTop) bottom = listTop;
    out.listTop_ = listTop;
    out.listHeight_ = std::max(0, bottom - listTop);
    out.detailsTop_ = listTop + out.listHeight_ + out.gap_;
    out.aboutTop_ = out.detailsTop_ + out.detailsHeight_ + out.gap_;

    // Что уступает место при маленьком окне, решается по важности: без списка
    // правил экран не делает ничего, а пояснение и «о программе» справочные.
    const int listMinimum = std::max(0, px(48.0));
    if (out.listHeight_ < listMinimum) {
        const int missing = listMinimum - out.listHeight_;
        out.listHeight_ = listMinimum;
        out.detailsTop_ += missing;
        out.aboutTop_ += missing;
    }
    out.filterVisible_ = out.filterTop_ + out.filterHeight_ <= out.height_;
    out.detailsVisible_ = out.detailsTop_ + out.detailsHeight_ <= out.height_ - out.padding_;
    out.aboutVisible_ = out.aboutTop_ + out.aboutHeight_ <= out.height_ - out.padding_;

    // Третья строка панели: подпись и комбинатор языка, подпись и комбинатор
    // уровня риска. Считаются справа налево от края контента, чтобы длинный
    // русский перевод не наезжал на левый край при растяжении окна.
    const int label = std::max(0, px(metrics.labelWidthDip));
    const int languageCombo = std::max(1, px(metrics.languageComboDip));
    const int safetyCombo = std::max(1, px(metrics.safetyComboDip));
    int right = contentLeft + contentWidth;
    out.safetyComboLeft_ = std::max(contentLeft, right - safetyCombo);
    out.safetyLabelLeft_ = std::max(contentLeft, out.safetyComboLeft_ - out.gap_ - label);
    right = out.safetyLabelLeft_ - out.gap_ * 2;
    out.languageComboLeft_ = std::max(contentLeft, right - languageCombo);
    out.languageLabelLeft_ = std::max(contentLeft, out.languageComboLeft_ - out.gap_ - label);
    out.autoUpdateWidth_ = std::max(1, std::min(260, contentWidth / 2));
    out.autoUpdateLeft_ = contentLeft;
    return out;
}

SettingsRect SettingsLayout::toolbarRow1Rect() const noexcept {
    return SettingsRect{padding_, row1Top_, std::max(0, width_ - 2 * padding_), rowHeight_};
}

SettingsRect SettingsLayout::toolbarRow2Rect() const noexcept {
    return SettingsRect{padding_, row2Top_, std::max(0, width_ - 2 * padding_), rowHeight_};
}

SettingsRect SettingsLayout::toolbarRow3Rect() const noexcept {
    return SettingsRect{padding_, row3Top_, std::max(0, width_ - 2 * padding_), rowHeight_};
}

SettingsRect SettingsLayout::statusRect() const noexcept {
    return SettingsRect{padding_, statusTop_, std::max(0, width_ - 2 * padding_), std::max(0, statusHeight_)};
}

SettingsRect SettingsLayout::filterRect() const noexcept {
    return SettingsRect{padding_, filterTop_, std::max(0, width_ - 2 * padding_), std::max(0, filterHeight_)};
}

SettingsRect SettingsLayout::listRect() const noexcept {
    return SettingsRect{padding_, listTop_, std::max(0, width_ - 2 * padding_), std::max(0, listHeight_)};
}

SettingsRect SettingsLayout::detailsRect() const noexcept {
    if (!detailsVisible_) return SettingsRect{};
    return SettingsRect{padding_, detailsTop_, std::max(0, width_ - 2 * padding_), std::max(0, detailsHeight_)};
}

SettingsRect SettingsLayout::aboutRect() const noexcept {
    if (!aboutVisible_) return SettingsRect{};
    return SettingsRect{padding_, aboutTop_, std::max(0, width_ - 2 * padding_), std::max(0, aboutHeight_)};
}

SettingsRect SettingsLayout::buttonRect(std::size_t index) const noexcept {
    if (index < row1Count_) {
        int x = padding_;
        for (std::size_t i = 0; i < index && i < row1Widths_.size(); ++i) x += row1Widths_[i] + buttonGap_;
        if (index >= row1Widths_.size()) return SettingsRect{};
        return SettingsRect{x, row1Top_, row1Widths_[index], rowHeight_};
    }
    const std::size_t second = index - row1Count_;
    if (second >= row2Count_ || second >= row2Widths_.size()) return SettingsRect{};
    int x = padding_;
    for (std::size_t i = 0; i < second && i < row2Widths_.size(); ++i) x += row2Widths_[i] + buttonGap_;
    return SettingsRect{x, row2Top_, row2Widths_[second], rowHeight_};
}

std::size_t SettingsLayout::buttonCount() const noexcept { return row1Count_ + row2Count_; }

SettingsRect SettingsLayout::autoUpdateRect() const noexcept {
    return SettingsRect{autoUpdateLeft_, row3Top_, std::max(0, autoUpdateWidth_), rowHeight_};
}

SettingsRect SettingsLayout::languageLabelRect() const noexcept {
    return SettingsRect{languageLabelLeft_, row3Top_, std::max(0, safetyLabelLeft_ - languageLabelLeft_ - gap_),
                        rowHeight_};
}

SettingsRect SettingsLayout::languageComboRect() const noexcept {
    const int width = std::max(0, safetyLabelLeft_ - gap_ - languageComboLeft_);
    return SettingsRect{languageComboLeft_, row3Top_, width, rowHeight_};
}

SettingsRect SettingsLayout::safetyLabelRect() const noexcept {
    const int width = std::max(0, safetyComboLeft_ - gap_ - safetyLabelLeft_);
    return SettingsRect{safetyLabelLeft_, row3Top_, width, rowHeight_};
}

SettingsRect SettingsLayout::safetyComboRect() const noexcept {
    return SettingsRect{safetyComboLeft_, row3Top_, std::max(0, width_ - padding_ - safetyComboLeft_), rowHeight_};
}

bool SettingsLayout::filterVisible() const noexcept { return filterVisible_ && !filterRect().empty(); }
bool SettingsLayout::detailsVisible() const noexcept { return detailsVisible_ && !detailsRect().empty(); }
bool SettingsLayout::aboutVisible() const noexcept { return aboutVisible_ && !aboutRect().empty(); }
bool SettingsLayout::cramped() const noexcept { return cramped_; }
int SettingsLayout::clientWidthPx() const noexcept { return width_; }
int SettingsLayout::clientHeightPx() const noexcept { return height_; }
int SettingsLayout::listHeightPx() const noexcept { return std::max(0, listHeight_); }

HitTarget SettingsLayout::hitTest(int px, int py) const noexcept {
    if (cramped_) return HitTarget::None;
    // Порядок — от частного к общему: кнопки лежат внутри строк панели, а строки
    // внутри окна, и без порядка первое же совпадение отдало бы щелчок не туда.
    const std::array<HitTarget, kToolbarButtonCount> buttonTargets{
        HitTarget::CheckNow, HitTarget::ImportRules, HitTarget::RestoreEmbedded, HitTarget::ExportRuleSet,
        HitTarget::ExportStatistics};
    for (std::size_t i = 0; i < buttonTargets.size(); ++i) {
        if (buttonRect(i).contains(px, py)) return buttonTargets[i];
    }
    if (autoUpdateRect().contains(px, py)) return HitTarget::AutoUpdate;
    if (languageComboRect().contains(px, py)) return HitTarget::LanguageCombo;
    if (safetyComboRect().contains(px, py)) return HitTarget::SafetyCombo;
    if (filterRect().contains(px, py)) return HitTarget::Filter;
    if (listRect().contains(px, py)) return HitTarget::List;
    if (detailsRect().contains(px, py)) return HitTarget::Details;
    if (aboutRect().contains(px, py)) return HitTarget::About;
    if (statusRect().contains(px, py)) return HitTarget::Status;
    if (toolbarRow1Rect().contains(px, py) || toolbarRow2Rect().contains(px, py) ||
        toolbarRow3Rect().contains(px, py)) {
        return HitTarget::Toolbar;
    }
    return HitTarget::None;
}

// ---------------------------------------------------------------------------
// Модель
// ---------------------------------------------------------------------------

namespace {

// Решение пользователя по одному правилу. Хранится отдельно от набора правил:
// правила обновляются (ADR-002), а решение человека — нет, и сброс набора не
// должен молча стирать то, что человек настраивал.
struct RuleOverride {
    bool enabled{true};
    SafetyLevel safety{SafetyLevel::Review};
};

}  // namespace

struct SettingsViewModel::Impl {
    std::shared_ptr<const core::RuleSet> ruleSet;
    std::vector<RuleRow> rows;  // видимые строки после фильтра
    std::map<std::string, RuleOverride, std::less<>> overrides;
    std::string filter;
    std::string selected;
    std::string armedRisky;
    std::string lastAction;
    core::RuleSetStatus status;
    Language language{Language::Russian};
    int scrollDip{};
    bool checking{false};
    std::int64_t checkStartedAt{};

    // Пересобрать строки из снимка набора, применив решения пользователя.
    // Выделение задано идентификатором: идентификатор переживает обновление
    // набора, а индекс строки — нет.
    void rebuild() {
        rows.clear();
        if (!ruleSet) return;
        const std::string needle = asciiLower(filter);
        for (const core::Rule& rule : ruleSet->rules) {
            if (rule.id.empty()) continue;
            RuleRow row;
            row.key = rule.id;
            row.title = rule.title();
            row.category = categoryName(rule.id, rule.category, std::string());
            row.pattern = rule.resolvedLocator.empty() ? rule.locator : rule.resolvedLocator;
            row.note = rule.note;
            row.setSafety = rule.safety;
            row.safety = rule.safety;
            row.enabled = true;
            row.minAgeDays = rule.minAgeDays;

            if (const auto found = overrides.find(rule.id); found != overrides.end()) {
                row.enabled = found->second.enabled;
                row.safety = found->second.safety;
            }
            row.overridden = !row.enabled || row.safety != rule.safety;

            if (!needle.empty()) {
                const bool hit = containsFold(asciiLower(row.key), needle) ||
                                 containsFold(asciiLower(row.title), needle) ||
                                 containsFold(asciiLower(row.category), needle) ||
                                 containsFold(asciiLower(row.pattern), needle);
                if (!hit) continue;
            }
            rows.push_back(std::move(row));
        }
        if (selected.empty() || std::none_of(rows.begin(), rows.end(), [this](const RuleRow& row) {
                return row.key == selected;
            })) {
            selected = rows.empty() ? std::string() : rows.front().key;
        }
    }

    // Заголовки правил двуязычные и лежат в самом наборе, поэтому после смены
    // языка их надо перечитать, а не ждать следующей публикации набора.
    void refreshTitles() {
        if (!ruleSet) return;
        for (RuleRow& row : rows) {
            if (const core::Rule* rule = ruleSet->byId(row.key)) {
                row.title = rule->title();
                row.category = categoryName(rule->id, rule->category, std::string());
            }
        }
    }

    [[nodiscard]] const core::Rule* rule(std::string_view id) const {
        return ruleSet ? ruleSet->byId(std::string(id)) : nullptr;
    }

    // Решение пользователя для правила (с запасным вариантом «как в наборе»).
    [[nodiscard]] RuleOverride overrideFor(std::string_view id) const {
        if (const auto found = overrides.find(id); found != overrides.end()) return found->second;
        RuleOverride value;
        if (const core::Rule* found = rule(id)) value.safety = found->safety;
        return value;
    }
};

SettingsViewModel::SettingsViewModel() : impl_(std::make_unique<Impl>()) {}

void SettingsViewModel::publishRuleSet(std::shared_ptr<const core::RuleSet> rules) {
    impl_->ruleSet = std::move(rules);
    impl_->rebuild();
}

void SettingsViewModel::publishRuleSet(core::RuleSet rules) {
    impl_->ruleSet = std::make_shared<const core::RuleSet>(std::move(rules));
    impl_->rebuild();
}

void SettingsViewModel::clearRuleSet() {
    impl_->ruleSet.reset();
    impl_->rows.clear();
    impl_->selected.clear();
    impl_->armedRisky.clear();
}

bool SettingsViewModel::hasRuleSet() const noexcept { return static_cast<bool>(impl_->ruleSet); }
const core::RuleSet* SettingsViewModel::ruleSet() const noexcept { return impl_->ruleSet.get(); }

std::size_t SettingsViewModel::ruleCount() const noexcept {
    return impl_->ruleSet ? impl_->ruleSet->size() : 0U;
}

std::size_t SettingsViewModel::visibleRuleCount() const noexcept { return impl_->rows.size(); }

void SettingsViewModel::refreshRuleTexts() { impl_->refreshTitles(); }

bool SettingsViewModel::setRuleEnabled(std::string_view ruleId, bool enabled) {
    if (ruleId.empty() || impl_->rule(ruleId) == nullptr) return false;
    const RuleOverride current = impl_->overrideFor(ruleId);
    if (current.enabled == enabled) return false;
    impl_->overrides[std::string(ruleId)] = RuleOverride{enabled, current.safety};
    impl_->rebuild();
    return true;
}

bool SettingsViewModel::toggleRule(std::string_view ruleId) {
    if (ruleId.empty() || impl_->rule(ruleId) == nullptr) return false;
    const RuleOverride current = impl_->overrideFor(ruleId);
    return setRuleEnabled(ruleId, !current.enabled);
}

bool SettingsViewModel::ruleEnabled(std::string_view ruleId) const noexcept {
    return impl_->overrideFor(ruleId).enabled;
}

bool SettingsViewModel::requestSafety(std::string_view ruleId, SafetyLevel level) {
    if (ruleId.empty() || !isSafetyLevel(level) || impl_->rule(ruleId) == nullptr) return false;
    const RuleOverride current = impl_->overrideFor(ruleId);
    if (current.safety == level) return false;

    // Risky — единственный уровень, удаляющий данные вне корня правила без
    // дополнительных гарантий, поэтому FR-9 и §9 требуют для него второго
    // подтверждения. Первое нажатие вооружает, второе (для того же правила)
    // применяет. Понижение риска подтверждения не требует.
    if (level == SafetyLevel::Risky) {
        if (impl_->armedRisky != ruleId) {
            impl_->armedRisky = std::string(ruleId);
            return false;
        }
        impl_->armedRisky.clear();
    } else {
        impl_->armedRisky.clear();
    }
    impl_->overrides[std::string(ruleId)] = RuleOverride{current.enabled, level};
    impl_->rebuild();
    return true;
}

std::string SettingsViewModel::riskyArmedRuleId() const { return impl_->armedRisky; }

bool SettingsViewModel::riskyArmed(std::string_view ruleId) const noexcept {
    return !ruleId.empty() && impl_->armedRisky == ruleId;
}

void SettingsViewModel::disarmRisky() { impl_->armedRisky.clear(); }

std::size_t SettingsViewModel::overrideCount() const noexcept {
    // Считаем по видимым правилам: «изменено 3» из 200 строк отфильтрованного
    // списка должно означать «изменено 3 из показанных», иначе цифра пугает
    // правилом, которого человек сейчас не видит.
    std::size_t count = 0;
    for (const RuleRow& row : impl_->rows) {
        if (row.overridden) ++count;
    }
    return count;
}

std::size_t SettingsViewModel::enabledCount() const noexcept {
    std::size_t count = 0;
    for (const RuleRow& row : impl_->rows) {
        if (row.enabled) ++count;
    }
    return count;
}

void SettingsViewModel::forgetOverrides() {
    if (impl_->overrides.empty()) return;
    const std::size_t dropped = impl_->overrides.size();
    impl_->overrides.clear();
    impl_->armedRisky.clear();
    impl_->rebuild();
    impl_->lastAction = text("settings.overridesForgotten", core::StringArgs{countText(dropped)});
}

void SettingsViewModel::setFilter(std::string_view filter) {
    if (impl_->filter == filter) return;
    impl_->filter = std::string(filter);
    impl_->rebuild();
}

std::string SettingsViewModel::filter() const { return impl_->filter; }

bool SettingsViewModel::filtered() const noexcept { return !impl_->filter.empty(); }

void SettingsViewModel::setSelectedRule(std::string_view ruleId) {
    if (ruleId.empty()) {
        impl_->selected.clear();
        return;
    }
    const auto found = std::find_if(impl_->rows.begin(), impl_->rows.end(), [ruleId](const RuleRow& row) {
        return row.key == ruleId;
    });
    if (found == impl_->rows.end()) return;
    impl_->selected = found->key;
}

std::string SettingsViewModel::selectedRuleId() const { return impl_->selected; }

const RuleRow* SettingsViewModel::selectedRule() const {
    for (const RuleRow& row : impl_->rows) {
        if (row.key == impl_->selected) return &row;
    }
    return nullptr;
}

const std::vector<RuleRow>& SettingsViewModel::rules() const noexcept { return impl_->rows; }

const RuleRow* SettingsViewModel::ruleById(std::string_view ruleId) const {
    for (const RuleRow& row : impl_->rows) {
        if (row.key == ruleId) return &row;
    }
    return nullptr;
}

std::optional<std::size_t> SettingsViewModel::indexOfRule(std::string_view ruleId) const {
    for (std::size_t i = 0; i < impl_->rows.size(); ++i) {
        if (impl_->rows[i].key == ruleId) return i;
    }
    return std::nullopt;
}

std::string SettingsViewModel::ruleIdAt(std::size_t index) const {
    if (index >= impl_->rows.size()) return std::string();
    return impl_->rows[index].key;
}

bool SettingsViewModel::moveSelection(int delta) {
    if (impl_->rows.empty() || delta == 0) return false;
    std::size_t index = 0;
    const auto current = indexOfRule(impl_->selected);
    if (current) {
        const long long moved = static_cast<long long>(*current) + delta;
        if (moved < 0) {
            index = 0;
        } else if (static_cast<std::size_t>(moved) >= impl_->rows.size()) {
            index = impl_->rows.size() - 1;
        } else {
            index = static_cast<std::size_t>(moved);
        }
    }
    impl_->selected = impl_->rows[index].key;
    // Выделение другое — ожидание подтверждения Risky сбрасывается: человек
    // передумал, о каком именно правиле идёт речь.
    impl_->armedRisky.clear();
    return true;
}

void SettingsViewModel::selectFirst() {
    if (impl_->rows.empty()) {
        impl_->selected.clear();
        return;
    }
    impl_->selected = impl_->rows.front().key;
}

void SettingsViewModel::clearSelection() { impl_->selected.clear(); }

bool SettingsViewModel::handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown) {
    (void)controlDown;
    (void)shiftDown;
    if (impl_->selected.empty()) return false;
    switch (virtualKey) {
    case VK_SPACE:  // включить/выключить правило
        return toggleRule(impl_->selected);
    case VK_LEFT: {  // уровень ниже
        const RuleRow* row = selectedRule();
        if (row == nullptr) return false;
        const std::size_t index = safetyIndex(row->safety);
        if (index == 0) return false;
        return requestSafety(row->key, safetyFromIndex(index - 1));
    }
    case VK_RIGHT: {  // уровень выше
        const RuleRow* row = selectedRule();
        if (row == nullptr) return false;
        const std::size_t index = safetyIndex(row->safety);
        if (index + 1 >= safetyLevels().size()) return false;
        return requestSafety(row->key, safetyFromIndex(index + 1));
    }
    case VK_ESCAPE:  // снять ожидание подтверждения
        if (impl_->armedRisky.empty()) return false;
        disarmRisky();
        return true;
    default: break;
    }
    return false;
}

void SettingsViewModel::setRuleSetStatus(core::RuleSetStatus status) {
    impl_->status = std::move(status);
    // Смена набора вниз (откат, сброс) означает другое содержимое правил, и
    // выделение по идентификатору может указывать в пустоту — rebuild() это
    // учтёт, пересобрав строки из нового снимка, если он уже пришёл.
    if (impl_->selected.empty() && !impl_->rows.empty()) impl_->selected = impl_->rows.front().key;
}

const core::RuleSetStatus& SettingsViewModel::ruleSetStatus() const noexcept { return impl_->status; }

bool SettingsViewModel::shouldCheckNow(std::int64_t nowUnixSeconds) const noexcept {
    return impl_->status.shouldCheck(nowUnixSeconds);
}

bool SettingsViewModel::beginCheck() {
    if (impl_->checking) return false;
    impl_->checking = true;
    impl_->checkStartedAt = nowUnixSeconds();
    return true;
}

bool SettingsViewModel::checking() const noexcept { return impl_->checking; }

void SettingsViewModel::publishCheckResult(const core::RuleSetVerification& verification,
                                           std::int64_t nowUnixSeconds) {
    // Политика §9.2 целиком в core::rulesync: отчёт уже содержит и вердикт, и
    // список причин. Здесь только применяем решение ядра — вторая реализация
    // «применить или откатить» разошлась бы с движком.
    impl_->status = verification.ok() ? core::withAppliedCandidate(impl_->status, verification, nowUnixSeconds)
                                       : core::withFailedCheck(impl_->status, verification, nowUnixSeconds);
    impl_->checking = false;
    impl_->checkStartedAt = 0;
    impl_->lastAction = verification.summary();
}

void SettingsViewModel::setCheckedStatus(core::RuleSetStatus status) {
    impl_->status = std::move(status);
    impl_->checking = false;
    impl_->checkStartedAt = 0;
}

bool SettingsViewModel::requestRestoreEmbedded(std::int64_t nowUnixSeconds) {
    // Уже встроенный набор и без решений пользователя — менять нечего, и кнопка
    // в этом состоянии выключена (§9.2: сброс обязан быть возможен, но повторный
    // сброс ничего не делает и не должен выглядеть как «сработало»).
    if (impl_->status.embedded && impl_->overrides.empty()) return false;
    const std::size_t dropped = impl_->overrides.size();
    impl_->overrides.clear();
    impl_->armedRisky.clear();
    impl_->status = core::resetToEmbeddedSet(impl_->status, nowUnixSeconds);
    impl_->rebuild();
    impl_->lastAction = text("settings.restoreDone", core::StringArgs{countText(dropped)});
    return true;
}

void SettingsViewModel::setAutoUpdate(bool enabled) { impl_->status.autoUpdateEnabled = enabled; }
bool SettingsViewModel::autoUpdate() const noexcept { return impl_->status.autoUpdateEnabled; }

void SettingsViewModel::setLanguage(Language lang) { impl_->language = lang; }
Language SettingsViewModel::language() const noexcept { return impl_->language; }

// --- Тексты ------------------------------------------------------------------

std::string SettingsViewModel::rulesetVersionText() const {
    std::string version = impl_->status.version.empty() ? text("settings.builtinSet") : impl_->status.version;
    return text("settings.rulesetVersion") + ": " + version;
}

std::string SettingsViewModel::lastCheckedText() const {
    const std::string stamp = dateText(impl_->status.lastCheckEpochSeconds);
    if (impl_->checking) return stamp + " · " + text("settings.checking");
    if (impl_->status.lastResult.empty()) return text("settings.rulesetChecked", core::StringArgs{stamp});
    return text("settings.rulesetChecked", core::StringArgs{stamp + " — " + impl_->status.lastResult});
}

std::string SettingsViewModel::summaryText() const {
    return text("settings.rulesSummary",
                core::StringArgs{countText(ruleCount()), countText(enabledCount()), countText(overrideCount())});
}

std::string SettingsViewModel::statusText() const {
    std::vector<std::string> parts;
    parts.push_back(rulesetVersionText());
    parts.push_back(lastCheckedText());
    if (hasRuleSet()) {
        parts.push_back(summaryText());
    } else {
        parts.push_back(text("settings.noRuleSet"));
    }
    if (!impl_->lastAction.empty()) parts.push_back(impl_->lastAction);
    return joinWith(parts, " · ");
}

std::string SettingsViewModel::detailsText() const {
    const RuleRow* row = selectedRule();
    if (row == nullptr) {
        if (filtered() && !impl_->rows.empty()) return text("settings.noRulesFound");
        return text("settings.ruleNotSelected");
    }
    core::StringArgs args;
    args.push_back(row->key);
    args.push_back(row->category.empty() ? std::string(tr(StringId::kCommonUnknown)) : row->category);
    args.push_back(row->pattern.empty() ? std::string(tr(StringId::kCommonNone)) : row->pattern);
    args.push_back(std::to_string(row->minAgeDays));
    args.push_back(safetyText(row->safety));
    args.push_back(safetyText(row->setSafety));
    args.push_back(safetyHint(row->safety));
    const std::string base = text("settings.detailsTemplate", args);
    std::string out = base;
    if (!row->note.empty()) out.append("\r\n").append(row->note);
    const std::string hint = riskyHintText();
    if (!hint.empty()) out.append("\r\n").append(hint);
    return out;
}

std::string SettingsViewModel::riskyHintText() const {
    if (impl_->armedRisky.empty()) return std::string();
    const RuleRow* row = ruleById(impl_->armedRisky);
    if (row == nullptr) return std::string();
    return row->title + " — " + text("settings.riskyConfirm");
}

std::string SettingsViewModel::aboutText() const {
    core::StringArgs args;
    args.push_back(tr(StringId::kAppName));
    args.push_back(tr(StringId::kAppTagline));
    std::string out = text("settings.aboutApp", args);
    out.append("\r\n").append(text("settings.aboutPrivacy"));
    out.append(" · ").append(text("settings.aboutAutostart"));
    return out;
}

std::string SettingsViewModel::filterLabelText() const { return text("settings.filterLabel") + ":"; }
std::string SettingsViewModel::autoUpdateText() const { return tr(StringId::kSettingsAutoUpdate); }
std::string SettingsViewModel::languageLabelText() const { return tr(StringId::kSettingsLanguage) + ":"; }
std::string SettingsViewModel::safetyLabelText() const { return text("settings.safetyLevelLabel") + ":"; }

std::string SettingsViewModel::safetyText(SafetyLevel level) const {
    switch (level) {
    case SafetyLevel::Safe: return tr(StringId::kSafetySafe);
    case SafetyLevel::Review: return tr(StringId::kSafetyReview);
    case SafetyLevel::Risky: return tr(StringId::kSafetyRisky);
    }
    return tr(StringId::kCommonUnknown);
}

std::string SettingsViewModel::safetyHint(SafetyLevel level) const {
    switch (level) {
    case SafetyLevel::Safe: return tr(StringId::kSafetySafeHint);
    case SafetyLevel::Review: return tr(StringId::kSafetyReviewHint);
    case SafetyLevel::Risky: return tr(StringId::kSafetyRiskyHint);
    }
    return std::string();
}

std::string SettingsViewModel::enabledText(bool enabled) const {
    return enabled ? text("settings.ruleEnabled") : text("settings.ruleDisabled");
}

std::string SettingsViewModel::checkNowText() const {
    return checking() ? text("settings.checking") : tr(StringId::kSettingsCheckNow);
}

std::string SettingsViewModel::importRulesText() const { return text("settings.importRules"); }
std::string SettingsViewModel::restoreRulesText() const { return tr(StringId::kSettingsRestoreRules); }
std::string SettingsViewModel::exportRuleSetText() const { return text("settings.exportRules"); }
std::string SettingsViewModel::exportStatisticsText() const { return text("settings.exportStatistics"); }

std::vector<std::string> SettingsViewModel::languageNames() {
    return {core::languageName(Language::Russian), core::languageName(Language::English)};
}

std::vector<SafetyLevel> SettingsViewModel::safetyLevels() {
    return {SafetyLevel::Safe, SafetyLevel::Review, SafetyLevel::Risky};
}

// --- Экспорт -----------------------------------------------------------------

std::string SettingsViewModel::statisticsText() const {
    // Только то, что человек и так видит на экране, и ничего, что приложение
    // собирает само по себе (FR-9, ADR-007: никакой телеметрии).
    std::vector<std::string> lines;
    lines.push_back(text("settings.statisticsHeader"));
    lines.push_back(text("settings.statSource") + ": " +
                    (impl_->status.embedded ? text("settings.builtinSet") : std::string(tr(StringId::kCommonNone))));
    lines.push_back(text("settings.statVersion") + ": " +
                    (impl_->status.version.empty() ? text("settings.builtinSet") : impl_->status.version));
    lines.push_back(text("settings.statVerified") + ": " +
                    (impl_->status.verifiedVersion.empty() ? std::string(tr(StringId::kCommonNone))
                                                           : impl_->status.verifiedVersion));
    lines.push_back(text("settings.statInstalled") + ": " + dateText(impl_->status.installedEpochSeconds));
    lines.push_back(text("settings.statChecked") + ": " + dateText(impl_->status.lastCheckEpochSeconds));
    lines.push_back(text("settings.statResult") + ": " +
                    (impl_->status.lastResult.empty() ? std::string(tr(StringId::kCommonNone))
                                                       : impl_->status.lastResult));
    lines.push_back(text("settings.statAutoUpdate") + ": " + (impl_->status.autoUpdateEnabled ? "1" : "0"));
    lines.push_back(text("settings.statLanguage") + ": " + core::languageTag(impl_->language));
    lines.push_back(text("settings.statRules") + ": " + countText(ruleCount()));
    lines.push_back(text("settings.statEnabled") + ": " + countText(enabledCount()));
    // Именно все решения пользователя, а не число по видимым строкам: статистика
    // описывает установку целиком, а фильтр ниже выгружается отдельной строкой,
    // и читатель видит, по чему считали остальные цифры.
    lines.push_back(text("settings.statChanged") + ": " + countText(impl_->overrides.size()));
    lines.push_back(text("settings.statFilter") + ": " +
                    (impl_->filter.empty() ? std::string(tr(StringId::kCommonNone)) : impl_->filter));
    lines.push_back(text("settings.statSelected") + ": " +
                    (impl_->selected.empty() ? std::string(tr(StringId::kCommonNone)) : impl_->selected));
    return joinWith(lines, "\r\n") + "\r\n";
}

std::string SettingsViewModel::ruleSetExportJson() const {
    // Формат совпадает со схемой набора правил (§9.2, ADR-002): экспортированный
    // файл можно положить в rules/ и подписать tools\sign-rules.ps1, поэтому
    // писать «свой» формат ради удобства экрана нельзя.
    if (!impl_->ruleSet) return std::string();
    std::vector<json::Value> rules;
    rules.reserve(impl_->rows.size());
    for (const RuleRow& row : impl_->rows) {
        const core::Rule* rule = impl_->rule(row.key);
        if (rule == nullptr) continue;
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("id", json::Value(rule->id));
        members.emplace_back("category", json::Value(rule->category));
        members.emplace_back("safety", json::Value(safetyToken(row.safety)));
        members.emplace_back("locator", json::Value(rule->locator));
        std::vector<json::Value> excludes;
        excludes.reserve(rule->locatorExcludes.size());
        for (const std::string& exclude : rule->locatorExcludes) {
            excludes.push_back(json::Value(exclude));
        }
        members.emplace_back("locatorExcludes", json::Value::array(std::move(excludes)));
        members.emplace_back("minAgeDays", json::Value(static_cast<double>(rule->minAgeDays)));
        if (!rule->titleRu.empty()) members.emplace_back("titleRu", json::Value(rule->titleRu));
        if (!rule->titleEn.empty()) members.emplace_back("titleEn", json::Value(rule->titleEn));
        if (!rule->note.empty()) members.emplace_back("note", json::Value(rule->note));
        members.emplace_back("enabled", json::Value(row.enabled));
        rules.push_back(json::Value::object(std::move(members)));
    }
    std::vector<std::pair<std::string, json::Value>> root;
    root.emplace_back("schemaVersion", json::Value(impl_->ruleSet->schemaVersion));
    root.emplace_back("version", json::Value(impl_->ruleSet->version));
    root.emplace_back("minAppVersion", json::Value(impl_->ruleSet->minAppVersion));
    root.emplace_back("rules", json::Value::array(std::move(rules)));
    return json::Value::object(std::move(root)).dump(2);
}

// --- Сохранение настроек -----------------------------------------------------

namespace {

// Ключи плоского хранилища настроек (NavStateStore, nav). Префикс settings.
// отделяет их от ключей навигации ("nav.*") в одном и том же файле.
constexpr std::string_view kKeyVersion = "settings.version";
constexpr std::string_view kKeyLanguage = "settings.language";
constexpr std::string_view kKeyAutoUpdate = "settings.autoUpdate";
constexpr std::string_view kKeyRuleSetState = "settings.ruleSetState";
constexpr std::string_view kKeyRules = "settings.rules";
constexpr std::string_view kKeyFilter = "settings.filter";
constexpr std::string_view kKeySelected = "settings.selected";
constexpr int kSettingsFormatVersion = 1;

bool hasSeparator(std::string_view value) {
    return value.find(';') != std::string_view::npos || value.find('=') != std::string_view::npos;
}

std::vector<std::string> splitWith(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : value) {
        if (character == separator) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    parts.push_back(current);
    return parts;
}

}  // namespace

void SettingsViewModel::exportSettings(NavStateStore& store) const {
    store[std::string(kKeyVersion)] = std::to_string(kSettingsFormatVersion);
    store[std::string(kKeyLanguage)] = core::languageTag(impl_->language);
    store[std::string(kKeyAutoUpdate)] = impl_->status.autoUpdateEnabled ? "1" : "0";
    store[std::string(kKeyRuleSetState)] = core::serializeRuleSetStatus(impl_->status);
    store[std::string(kKeyFilter)] = impl_->filter;
    store[std::string(kKeySelected)] = impl_->selected;

    // Решения по правилам: записи «id=вкл:уровень», записи через ';'. Идентификатор
    // с разделителем в экспорт не попадает (в журнал уходит предупреждение):
    // записать его так, чтобы файл перестал разбираться, хуже, чем потерять
    // одну настройку, о которой скажет сам экран («изменено N»).
    std::string encoded;
    for (const auto& [id, value] : impl_->overrides) {
        if (hasSeparator(id) || id.empty() || id.find(':') != std::string::npos) {
            logEvent(core::LogLevel::Warn, "ui.settings.export",
                     "rule id with separator skipped: " + id);
            continue;
        }
        if (!encoded.empty()) encoded.push_back(';');
        encoded.append(id);
        encoded.push_back('=');
        encoded.push_back(value.enabled ? '1' : '0');
        encoded.push_back(':');
        encoded.append(safetyToken(value.safety));
    }
    store[std::string(kKeyRules)] = encoded;

    store[std::string(kNavPageStatePrefix) + std::string(pageKey(PageId::Settings))] = encodePageState(pageState());
}

std::size_t SettingsViewModel::importSettings(const NavStateStore& store, std::vector<std::string>* problems) {
    std::size_t applied = 0;
    const auto note = [problems](std::string message) {
        if (problems != nullptr) problems->push_back(std::move(message));
    };

    if (const auto version = store.find(std::string(kKeyVersion)); version != store.end()) {
        if (version->second != std::to_string(kSettingsFormatVersion)) {
            note(std::string(kKeyVersion) + ": неизвестная версия " + version->second);
        } else {
            ++applied;
        }
    }
    if (const auto value = store.find(std::string(kKeyLanguage)); value != store.end()) {
        if (const auto language = core::parseLanguage(value->second)) {
            impl_->language = *language;
            ++applied;
        } else {
            note(std::string(kKeyLanguage) + ": не язык: " + value->second);
        }
    }
    if (const auto value = store.find(std::string(kKeyAutoUpdate)); value != store.end()) {
        if (value->second == "0" || value->second == "1") {
            impl_->status.autoUpdateEnabled = value->second == "1";
            ++applied;
        } else {
            note(std::string(kKeyAutoUpdate) + ": не 0/1: " + value->second);
        }
    }
    if (const auto value = store.find(std::string(kKeyRuleSetState)); value != store.end()) {
        impl_->status = core::parseRuleSetStatus(value->second);
        ++applied;
    }
    if (const auto value = store.find(std::string(kKeyRules)); value != store.end()) {
        impl_->overrides.clear();
        for (const std::string& record : splitWith(value->second, ';')) {
            if (record.empty()) continue;  // хвостовой разделитель
            const std::vector<std::string> fields = splitWith(record, '=');
            if (fields.size() != 2) {
                note(std::string(kKeyRules) + ": не разобрано: " + record);
                continue;
            }
            const std::vector<std::string> value_parts = splitWith(fields[1], ':');
            if (fields[0].empty() || value_parts.size() != 2) {
                note(std::string(kKeyRules) + ": не разобрано: " + record);
                continue;
            }
            if (value_parts[0] != "0" && value_parts[0] != "1") {
                note(std::string(kKeyRules) + ": не 0/1: " + record);
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(std::strtoul(value_parts[1].c_str(), nullptr, 10));
            if (index > 2) {
                note(std::string(kKeyRules) + ": неизвестный уровень: " + record);
                continue;
            }
            impl_->overrides[fields[0]] = RuleOverride{value_parts[0] == "1", safetyFromIndex(index)};
            ++applied;
        }
    }
    if (const auto value = store.find(std::string(kKeyFilter)); value != store.end()) {
        impl_->filter = value->second;
        ++applied;
    }
    if (const auto value = store.find(std::string(kKeySelected)); value != store.end()) {
        impl_->selected = value->second;
        ++applied;
    }
    if (const auto value = store.find(std::string(kNavPageStatePrefix) + std::string(pageKey(PageId::Settings)));
        value != store.end()) {
        if (const auto state = decodePageState(value->second)) {
            applyPageState(*state);
            ++applied;
        } else {
            note(std::string(kNavPageStatePrefix) + "settings: не разобрано");
        }
    }
    impl_->rebuild();
    return applied;
}

void SettingsViewModel::setScrollOffsetDip(int dip) { impl_->scrollDip = std::max(0, dip); }
int SettingsViewModel::scrollOffsetDip() const noexcept { return impl_->scrollDip; }

void SettingsViewModel::applyPageState(const PageState& state) {
    impl_->scrollDip = std::max(0, state.scrollOffsetDip);
    impl_->filter = state.filter;
    impl_->selected = state.selectedKey;
    impl_->rebuild();
}

PageState SettingsViewModel::pageState() const {
    PageState state;
    state.scrollOffsetDip = impl_->scrollDip;
    state.selectedKey = impl_->selected;
    state.filter = impl_->filter;
    return state;
}

// ---------------------------------------------------------------------------
// Окно экрана
// ---------------------------------------------------------------------------
//
// Ниже только Win32 (SPEC §7, ADR-3). Всё, что можно было решить без окна,
// вынесено в модель и раскладку выше; здесь осталось ровно четыре вещи, которые
// Win32 умеет сам: список с системными флажками и клавиатурным фокусом,
// комбинаторы, поле ввода и нажатия на кнопки.

namespace detail {

constexpr wchar_t kSettingsViewClass[] = L"MrProper.SettingsView";

// Отложенная перерисовка после действия внутри обработчика уведомления: вставка
// и удаление строк прямо в LVN_ITEMCHANGED — это перерисовка списка из его же
// обработчика, на которую comctl32 не рассчитан.
constexpr UINT kMsgSyncModel = WM_APP + 1;

// Идентификаторы дочерних окон. Отдельное пространство от ControlId (команды
// WM_COMMAND): иначе WM_COMMAND кнопки «Проверить сейчас» пришёл бы от списка
// правил, и разбирать пришлось бы по источнику.
enum : UINT_PTR {
    kChildList = 1,
    kChildStatus,
    kChildDetails,
    kChildAbout,
    kChildFilterLabel,
    kChildFilterEdit,
    kChildAutoUpdate,
    kChildLanguageLabel,
    kChildLanguageCombo,
    kChildSafetyLabel,
    kChildSafetyCombo,
    kChildHint,
};

// Порядок кнопок панели = порядок индексов раскладки и порядок ControlId.
constexpr std::array<ControlId, kToolbarButtonCount> kButtonIds{
    ControlId::CheckNow, ControlId::ImportRules, ControlId::RestoreEmbedded, ControlId::ExportRuleSet,
    ControlId::ExportStatistics};

// Столбцы списка правил.
enum : int { kColumnRule = 0, kColumnCategory, kColumnSafety, kColumnCount };

// Подкласс контрола: исходная процедура и сам контрол. Владеет ими экран.
struct ChildProc {
    HWND hwnd{nullptr};
    WNDPROC prev{nullptr};
};

// Процедуры окон объявлены здесь: ViewState обращается к childProc (подкласс),
// а SettingsScreen::create — к viewProc, и обе определены ниже.
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// Состояние окна. Отдельная именованная структура, а не содержимое Impl,
// нужна затем, чтобы вспомогательные функции файла принимали ViewState&:
// вложенный приватный Impl из заголовка они назвать не могут.
struct ViewState {
    SettingsScreen::Callbacks callbacks;
    SettingsViewModel model;
    theme::Theme theme;
    SettingsMetrics metrics{};
    SettingsLayout current;
    int dpi{kDefaultDpi};

    HWND window{nullptr};
    HWND list{nullptr};
    HWND status{nullptr};
    HWND details{nullptr};
    HWND about{nullptr};
    HWND hint{nullptr};
    HWND filterLabel{nullptr};
    HWND filterEdit{nullptr};
    HWND autoUpdate{nullptr};
    HWND languageLabel{nullptr};
    HWND languageCombo{nullptr};
    HWND safetyLabel{nullptr};
    HWND safetyCombo{nullptr};
    std::array<HWND, kToolbarButtonCount> buttons{};
    std::array<HFONT, 4> fonts{};  // подзаголовок, полужирный (список), обычный, поле фильтра
    HBRUSH surfaceBrush{nullptr};
    std::vector<ChildProc> children;
    std::vector<std::string> rowKeys;
    std::shared_ptr<mv::ScreenEndpoint> feed;
    WINDOWPLACEMENT placement{};
    bool syncing{false};
    bool controlsReady{false};
    bool columnsReady{false};
    std::uint64_t columnsLanguageRevision{0};  // ревизия языка, на которой созданы столбцы
    std::uint64_t combosLanguageRevision{0};   // и на которой заполнены комбинаторы
    bool languageReady{false};
    bool safetyReady{false};
    bool filterSyncing{false};
    bool hasPlacement{false};

    ~ViewState() {
        for (HFONT& font : fonts) {
            if (font != nullptr) ::DeleteObject(font);
        }
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
    }

    void setChildText(HWND child, std::string_view value) {
        if (child == nullptr) return;
        const std::wstring wide = toWide(value);
        ::SetWindowTextW(child, wide.c_str());
    }

    std::string childText(HWND child) const {
        if (child == nullptr) return std::string();
        const int length = ::GetWindowTextLengthW(child);
        if (length <= 0) return std::string();
        std::wstring buffer(static_cast<std::size_t>(length) + 1U, L'\0');
        const int copied = ::GetWindowTextW(child, buffer.data(), length + 1);
        if (copied <= 0) return std::string();
        buffer.resize(static_cast<std::size_t>(copied));
        return toUtf8(buffer);
    }

    void setEnabled(HWND child, bool enabled) {
        if (child == nullptr) return;
        if ((::IsWindowEnabled(child) != 0) == enabled) return;
        ::EnableWindow(child, enabled ? TRUE : FALSE);
    }

    void setCheck(HWND child, bool checked) {
        if (child == nullptr) return;
        if ((::SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED) == checked) return;
        ::SendMessageW(child, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    void setComboSelection(HWND combo, int index) {
        if (combo == nullptr) return;
        if (::SendMessageW(combo, CB_GETCURSEL, 0, 0) == static_cast<LRESULT>(index)) return;
        // CB_SETCURSEL не шлёт CBN_SELCHANGE, поэтому обработчику комбинатора
        // возвращаться не придётся и цикла не возникнет.
        ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }

    int comboSelection(HWND combo) const {
        if (combo == nullptr) return -1;
        return static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
    }

    // --- Подкласс ------------------------------------------------------------

    void createChild(HWND child) {
        if (child == nullptr || window == nullptr) return;
        const WNDPROC prev = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&childProc)));
        if (prev == nullptr) {
            logWin32("ui.settings.subclass", "SetWindowLongPtrW(GWLP_WNDPROC)", ::GetLastError());
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

    // --- Тема ----------------------------------------------------------------

    void applyPalette() {
        const theme::Palette& palette = theme.palette();
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
        surfaceBrush = ::CreateSolidBrush(theme::colorRef(palette.surface));
        if (list != nullptr) {
            ListView_SetBkColor(list, theme::colorRef(palette.surface));
            ListView_SetTextColor(list, theme::colorRef(palette.textPrimary));
            // Подтемы «тёмного» у нативных контролов нет в документированном
            // API (ADR-003), и модуль темы делает это через безопасные вызовы
            // uxtheme; отказ — не повод оставлять контрол белым.
            (void)theme::enableDarkModeForWindow(list, theme.scheme());
        }
    }

    void reloadTheme() {
        theme.reload();
        applyPalette();
        applyFonts();
        if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
    }

    void applyFonts() {
        // Сначала создаём новые шрифты, потом перевешиваем их на контролы и
        // только потом удаляем старые: удалить HFONT, висящий на контроле, —
        // значит оставить контрол со шрифтом в никуда.
        const std::array<theme::FontRole, 4> roles{theme::FontRole::Subtitle, theme::FontRole::BodyStrong,
                                                  theme::FontRole::Body, theme::FontRole::Body};
        std::array<HFONT, 4> next{};
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const LOGFONTW description = theme.font(roles[i]).toLogFont(static_cast<unsigned>(dpi));
            next[i] = ::CreateFontIndirectW(&description);
        }
        if (status != nullptr) ::SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(next[0]), TRUE);
        if (list != nullptr) ::SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        for (const HWND child : {details, about, filterLabel, autoUpdate, languageLabel, languageCombo,
                                  safetyLabel, safetyCombo}) {
            if (child != nullptr) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next[2]), TRUE);
        }
        for (const HWND button : buttons) {
            if (button != nullptr) ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(next[2]), TRUE);
        }
        if (filterEdit != nullptr) {
            ::SendMessageW(filterEdit, WM_SETFONT, reinterpret_cast<WPARAM>(next[3]), TRUE);
        }
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (fonts[i] != nullptr) ::DeleteObject(fonts[i]);
            fonts[i] = next[i];
        }
    }

    // Поле зеркалится по локали ОС, а не по языку приложения (§5 «RTL-ready»):
    // русский и английский оба LTR, но интерфейс на арабской Windows обязан быть
    // зеркальным. Ветка проверяется принудительным направлением
    // (locale::forceTextDirection), а не будущим языком: код, который нельзя
    // выполнить, через год оказывается нерабочим.
    void applyReadingOrder() {
        for (const HWND child : {list, languageCombo, safetyCombo, filterEdit}) {
            if (child == nullptr) continue;
            const LONG_PTR ex = ::GetWindowLongPtrW(child, GWL_EXSTYLE);
            const auto styles = readingOrderStyles(static_cast<std::uint32_t>(ex));
            ::SetWindowLongPtrW(child, GWL_EXSTYLE, static_cast<LONG_PTR>(styles));
        }
    }

    // --- Создание контролов --------------------------------------------------

    bool createControls(HINSTANCE instance) {
        const DWORD childVisible = WS_CHILD | WS_VISIBLE;
        list = ::CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                                 childVisible | WS_TABSTOP | WS_HSCROLL | LVS_REPORT | LVS_SHOWSELALWAYS |
                                     LVS_NOSORTHEADER,
                                 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kChildList), instance, nullptr);
        status = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_LEFT, 0, 0, 0, 0, window,
                                   reinterpret_cast<HMENU>(kChildStatus), instance, nullptr);
        details = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0,
                                    window, reinterpret_cast<HMENU>(kChildDetails), instance, nullptr);
        about = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window,
                                  reinterpret_cast<HMENU>(kChildAbout), instance, nullptr);
        filterLabel = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_RIGHT, 0, 0, 0, 0, window,
                                        reinterpret_cast<HMENU>(kChildFilterLabel), instance, nullptr);
        // ES_AUTOHSCROLL: список правил длинный, и горизонтальная прокрутка поля
        // при длинном идентификаторе лучше, чем обрезанный текст фильтра.
        filterEdit = ::CreateWindowExW(0, L"EDIT", nullptr, childVisible | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                       window, reinterpret_cast<HMENU>(kChildFilterEdit), instance, nullptr);
        autoUpdate = ::CreateWindowExW(0, L"BUTTON", nullptr, childVisible | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0,
                                       0, 0, window, reinterpret_cast<HMENU>(kChildAutoUpdate), instance, nullptr);
        languageLabel = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_RIGHT, 0, 0, 0, 0, window,
                                          reinterpret_cast<HMENU>(kChildLanguageLabel), instance, nullptr);
        languageCombo = ::CreateWindowExW(0, L"COMBOBOX", nullptr, childVisible | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                                          0, 0, 0, window, reinterpret_cast<HMENU>(kChildLanguageCombo), instance,
                                          nullptr);
        safetyLabel = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_RIGHT, 0, 0, 0, 0, window,
                                        reinterpret_cast<HMENU>(kChildSafetyLabel), instance, nullptr);
        safetyCombo = ::CreateWindowExW(0, L"COMBOBOX", nullptr, childVisible | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0,
                                        0, 0, window, reinterpret_cast<HMENU>(kChildSafetyCombo), instance, nullptr);
        // Пояснение вместо пустого списка правил: список без набора — это не
        // «настраивать нечего», а «набор не прочитан», и разница видна только по
        // словам (§9.2, §5 «каждый отказ виден»).
        hint = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_OWNERDRAW, 0, 0, 0, 0, window,
                                 reinterpret_cast<HMENU>(kChildHint), instance, nullptr);
        for (std::size_t i = 0; i < kButtonIds.size(); ++i) {
            buttons[i] = ::CreateWindowExW(0, L"BUTTON", nullptr, childVisible | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0,
                                           0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kButtonIds[i])),
                                           instance, nullptr);
        }

        const std::array<HWND, 12> created{list, status, details, about, filterLabel, filterEdit, autoUpdate,
                                           languageLabel, languageCombo, safetyLabel, safetyCombo, hint};
        for (const HWND child : created) {
            if (child == nullptr) {
                logWin32("ui.settings.create", "CreateWindowExW(child)", ::GetLastError());
                return false;
            }
        }
        for (const HWND button : buttons) {
            if (button == nullptr) {
                logWin32("ui.settings.create", "CreateWindowExW(button)", ::GetLastError());
                return false;
            }
        }
        for (const HWND child : created) {
            // Подкласс нужен списку, комбинаторам, полю фильтра и кнопкам: они
            // едят клавиши, и без подкласса пробел (вкл/выкл правило) и стрелки
            // (уровень риска) до модели не дошли бы (§5 «Клавиатурная навигация,
            // фокус»). Статическим подписям подкласс не нужен.
            if (child != hint) createChild(child);
        }
        for (const HWND button : buttons) createChild(button);

        // Флажки правил рисует сам comctl32: свои картинки означали бы вторую
        // реализацию «вкл/выкл» и рассинхрон с клавиатурой (LVS_EX_CHECKBOXES
        // обрабатывает и Space, и мышь). Состояние приходит в LVN_ITEMCHANGED,
        // и модель — единственный источник истины: своё циклирование мы
        // отменяем и перерисовываем отложенно.
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                                      LVS_EX_DOUBLEBUFFER);
        applyReadingOrder();
        controlsReady = true;
        return true;
    }

    // --- Подписи и содержимое -------------------------------------------------

    void syncTexts() {
        setChildText(status, model.statusText());
        setChildText(details, model.detailsText());
        setChildText(about, model.aboutText());
        setChildText(filterLabel, model.filterLabelText());
        setChildText(autoUpdate, model.autoUpdateText());
        setChildText(languageLabel, model.languageLabelText());
        setChildText(safetyLabel, model.safetyLabelText());

        if (filterEdit != nullptr) {
            // Пока пользователь печатает, текст в поле и в модели совпадают, и
            // поле не трогается: SetWindowText посылает EN_CHANGE, который
            // здесь же обрабатывается иначе (см. флаг filterSyncing).
            const std::string typed = childText(filterEdit);
            if (typed != model.filter()) {
                filterSyncing = true;
                setChildText(filterEdit, model.filter());
                filterSyncing = false;
            }
        }
    }

    // ------------------------------------------------------------------------
    // Набор правил из фонового потока
    // ------------------------------------------------------------------------
    //
    // Экран «Настроек» без набора правил показывал пустой список и не говорил
    // почему: набор приходит с диска (ADR-008), читать его — I/O, а §6.1 запрещает
    // его в UI-потоке. Поэтому чтение делает раздача (mv::StartupFeed), а экран
    // только принимает готовый неизменяемый снимок.
    void attachFeed() {
        if (feed != nullptr || window == nullptr) return;
        feed = mv::StartupFeed::instance().subscribe(window);
        if (const std::shared_ptr<const core::RuleSet> ready = mv::StartupFeed::instance().ruleSet()) {
            model.publishRuleSet(ready);
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
                    model.publishRuleSet(rules);
                    changed = true;
                }
                break;
            }
            case mv::EventKind::Notice:
            case mv::EventKind::Error: {
                if (const std::shared_ptr<const std::string> text = event.as<std::string>()) {
                    // Причина отказа — в строке состояния, а не в заголовке окна:
                    // её видно, не переключая страницу, и она остаётся в журнале.
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

    void syncButtons() {
        // Кнопка без обработчика выключена: экран не знает, кто выполнит
        // действие, и молча гасить нажатие хуже, чем показать, что оно некуда.
        const bool haveCheck = static_cast<bool>(callbacks.onCheckNow);
        const bool haveImport = static_cast<bool>(callbacks.onImportRules);
        const bool haveRestore = static_cast<bool>(callbacks.onRestoreEmbedded);
        const bool haveExportRules = static_cast<bool>(callbacks.onExportRuleSet);
        const bool haveExportStats = static_cast<bool>(callbacks.onExportStatistics);
        const bool alreadyEmbedded = model.ruleSetStatus().embedded && model.overrideCount() == 0;

        setChildText(buttons[0], model.checkNowText());
        setChildText(buttons[1], model.importRulesText());
        setChildText(buttons[2], model.restoreRulesText());
        setChildText(buttons[3], model.exportRuleSetText());
        setChildText(buttons[4], model.exportStatisticsText());
        setEnabled(buttons[0], haveCheck && !model.checking());
        setEnabled(buttons[1], haveImport);
        setEnabled(buttons[2], haveRestore && !alreadyEmbedded);
        setEnabled(buttons[3], haveExportRules && model.hasRuleSet());
        setEnabled(buttons[4], haveExportStats);
    }

    void syncCombos() {
        // Пункты обоих комбинаторов заполняются один раз — и остаются на
        // прежнем языке: безопасность в комбинаторе переводится, а список
        // уровней риска нет. При смене языка списки пересоздаются, иначе
        // английский экран сохранял бы русское «Безопасно» (тот же отказ, что и
        // с заголовками столбцов, только в другом контроле).
        const std::uint64_t languageRevision = revision();
        if (languageReady && languageRevision != combosLanguageRevision) {
            if (languageCombo != nullptr) {
                (void)::SendMessageW(languageCombo, CB_RESETCONTENT, 0, 0);
            }
            if (safetyCombo != nullptr) {
                (void)::SendMessageW(safetyCombo, CB_RESETCONTENT, 0, 0);
            }
            languageReady = false;
            safetyReady = false;
        }
        if (!languageReady && languageCombo != nullptr) {
            for (const std::string& name : SettingsViewModel::languageNames()) {
                const std::wstring wide = toWide(name);
                ::SendMessageW(languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
            }
            languageReady = true;
        }
        if (!safetyReady && safetyCombo != nullptr) {
            for (const SafetyLevel level : SettingsViewModel::safetyLevels()) {
                const std::wstring wide = toWide(model.safetyText(level));
                ::SendMessageW(safetyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
            }
            safetyReady = true;
        }
        if (languageReady && safetyReady) combosLanguageRevision = languageRevision;
        setComboSelection(languageCombo, model.language() == Language::English ? 1 : 0);
        setCheck(autoUpdate, model.autoUpdate());

        const RuleRow* row = model.selectedRule();
        setEnabled(safetyCombo, row != nullptr);
        setEnabled(safetyLabel, row != nullptr);
        if (row != nullptr) {
            setComboSelection(safetyCombo, static_cast<int>(safetyIndex(row->safety)));
        } else {
            setComboSelection(safetyCombo, -1);
        }
    }

    void syncColumns() {
        if (list == nullptr) return;
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        // Заголовки столбцов — из каталога строк, иначе при смене языка
        // таблица осталась бы с русскими заголовками.
        //
        // LVCOLUMN не умеет менять текст УЖЕ созданного столбца, поэтому при
        // смене языка столбцы пересоздаются. Без этого английская таблица
        // осталась бы с русскими заголовками — тот самый «переключилось не всё»,
        // который ловит сравнение снимков.
        const std::uint64_t languageRevision = revision();
        if (columnsReady && languageRevision != columnsLanguageRevision) {
            // LVM_DELETECOLUMN по одному, а не ListView_DeleteAllColumns: макрос
            // есть не во всех сборках commctrl.h, а сообщение есть всегда.
            for (int column = kColumnCount - 1; column >= 0; --column) {
                (void)::SendMessageW(list, LVM_DELETECOLUMN, static_cast<WPARAM>(column), 0);
            }
            columnsReady = false;
        }
        if (!columnsReady) {
            const std::array<std::string_view, kColumnCount> titles{"settings.ruleColumn",
                                                                   "settings.categoryColumn",
                                                                   "settings.safetyColumn"};
            for (int i = 0; i < kColumnCount; ++i) {
                const std::wstring header = toWide(text(titles[static_cast<std::size_t>(i)]));
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_SUBITEM;
                column.pszText = const_cast<wchar_t*>(header.c_str());
                column.iSubItem = i;
                if (ListView_InsertColumn(list, i, &column) == -1) {
                    logWin32("ui.settings.list", "ListView_InsertColumn", ::GetLastError());
                    return;
                }
            }
            columnsLanguageRevision = languageRevision;
            columnsReady = true;
        }
        RECT client{};
        if (::GetClientRect(list, &client) == FALSE) return;
        const int total = client.right - client.left;
        // Места под значок риска ( рисуется NM_CUSTOMDRAW у правого края строки)
        // резервируем явно: иначе иконка ложилась бы на подпись уровня.
        const int iconSpace = scale.dip(20.0);
        const int category = std::max(60, scale.dip(120.0));
        const int safety = std::max(70, scale.dip(110.0));
        ListView_SetColumnWidth(list, kColumnRule, std::max(60, total - category - safety - iconSpace));
        ListView_SetColumnWidth(list, kColumnCategory, category);
        ListView_SetColumnWidth(list, kColumnSafety, safety);
    }

    void syncList() {
        if (list == nullptr) return;
        // Положение прокрутки переживает перестроение: без этого щелчок по
        // правилу внизу списка прыгал бы список наверх.
        std::string firstVisible;
        if (const int top = ListView_GetTopIndex(list); top >= 0) {
            if (static_cast<std::size_t>(top) < rowKeys.size()) firstVisible = rowKeys[static_cast<std::size_t>(top)];
        }

        syncing = true;
        ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        rowKeys.clear();

        const std::vector<RuleRow>& rows = model.rules();
        rowKeys.reserve(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const RuleRow& row = rows[i];
            const std::wstring title = toWide(row.title.empty() ? row.key : row.title);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
            item.iItem = static_cast<int>(i);
            item.lParam = static_cast<LPARAM>(i);
            item.stateMask = LVIS_STATEIMAGEMASK;
            // Картинки состояния приходят из системного списка флажков comctl32:
            // 1 — пусто, 2 — отмечено (LVS_EX_CHECKBOXES).
            item.state = row.enabled ? 2 : 1;
            item.pszText = const_cast<wchar_t*>(title.c_str());
            if (ListView_InsertItem(list, &item) == -1) {
                logWin32("ui.settings.list", "ListView_InsertItem", ::GetLastError());
                break;
            }
            const std::wstring category = toWide(row.category);
            const std::wstring safety = toWide(model.safetyText(row.safety));
            ListView_SetItemText(list, static_cast<int>(i), kColumnCategory, const_cast<wchar_t*>(category.c_str()));
            ListView_SetItemText(list, static_cast<int>(i), kColumnSafety, const_cast<wchar_t*>(safety.c_str()));
            rowKeys.push_back(row.key);
        }
        // Выделение восстанавливаем после вставки: иначе строка вставляется
        // выделенной, а модель о ней ещё не знает.
        const std::string selected = model.selectedRuleId();
        for (std::size_t i = 0; i < rowKeys.size(); ++i) {
            if (rowKeys[i] != selected) continue;
            ListView_SetItemState(list, static_cast<WPARAM>(i), LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            if (firstVisible.empty()) {
                ListView_EnsureVisible(list, static_cast<int>(i), FALSE);
            }
            break;
        }
        if (!firstVisible.empty()) {
            for (std::size_t i = 0; i < rowKeys.size(); ++i) {
                if (rowKeys[i] != firstVisible) continue;
                ListView_EnsureVisible(list, static_cast<int>(i), FALSE);
                break;
            }
        }
        if (rows.empty()) {
            setChildText(details, model.detailsText());
        }
        ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        syncing = false;
    }

    // Ширина кнопки по её подписи: раскладка не знает шрифтов, а фиксированная
    // ширина означала бы либо обрезанную подпись (§12), либо пустое место.
    [[nodiscard]] std::vector<int> measureButtons() const {
        // Квалифицированное имя через пространство тем: внутри класса есть член
        // с именем theme, и только квалификатор отличает его от namespace.
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const int padding = std::max(8, scale.dip(16.0));
        const std::array<std::string, kToolbarButtonCount> labels{
            model.checkNowText(), model.importRulesText(), model.restoreRulesText(), model.exportRuleSetText(),
            model.exportStatisticsText()};
        std::vector<int> widths;
        widths.reserve(labels.size());
        HDC dc = window != nullptr ? ::GetDC(window) : nullptr;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            int width = 48;
            const std::wstring wide = toWide(labels[i]);
            if (dc != nullptr && i < buttons.size() && buttons[i] != nullptr) {
                if (fonts[2] != nullptr) ::SelectObject(dc, fonts[2]);
                SIZE measured{};
                ::GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &measured);
                width = measured.cx + padding;
            }
            widths.push_back(std::max(1, width));
        }
        if (dc != nullptr) ::ReleaseDC(window, dc);
        return widths;
    }

    // --- Раскладка ------------------------------------------------------------

    void place(HWND child, const SettingsRect& rect, bool visible) {
        if (child == nullptr) return;
        if (!visible || rect.empty()) {
            ::ShowWindow(child, SW_HIDE);
            return;
        }
        ::SetWindowPos(child, nullptr, rect.x, rect.y, rect.width, rect.height, SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(child, SW_SHOW);
    }

    // Пояснение поверх списка правил — только когда правил нет. Фильтр при этом
    // может отсечь всё в пустом наборе, и тогда причина другая, поэтому emptiness
    // считается по набору, а не по видимым строкам.
    void placeHint() {
        if (hint == nullptr) return;
        const SettingsRect listRect = current.listRect();
        const bool empty = model.ruleCount() == 0;
        const int pad = std::max(2, dipToPx(8.0, dpi));
        const SettingsRect box{listRect.x + pad, listRect.y + pad,
                               std::max(listRect.x + pad, listRect.x + listRect.width - pad),
                               std::max(listRect.y + pad, listRect.y + listRect.height - pad)};
        place(hint, empty ? box : SettingsRect{}, empty);
        if (empty) ::SetWindowPos(hint, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void layout() {
        int clientWidth = 0;
        int clientHeight = 0;
        RECT client{};
        if (window != nullptr && ::GetClientRect(window, &client) != FALSE) {
            clientWidth = client.right - client.left;
            clientHeight = client.bottom - client.top;
        }
        current = SettingsLayout::compute(metrics, dpi, clientWidth, clientHeight, measureButtons());
        place(list, current.listRect(), true);
        place(status, current.statusRect(), true);
        place(details, current.detailsRect(), current.detailsVisible());
        place(about, current.aboutRect(), current.aboutVisible());
        placeHint();

        // Подпись фильтра слева от поля, само поле — на всю оставшуюся строку.
        const SettingsRect filter = current.filterRect();
        const int labelWidth = std::min(std::max(0, filter.width / 4), dipToPx(96.0, dpi));
        place(filterLabel, SettingsRect{filter.x, filter.y, std::max(0, labelWidth - dipToPx(4.0, dpi)),
                                        filter.height},
              current.filterVisible());
        place(filterEdit,
              SettingsRect{filter.x + labelWidth, filter.y, std::max(0, filter.width - labelWidth), filter.height},
              current.filterVisible());

        for (std::size_t i = 0; i < buttons.size(); ++i) {
            place(buttons[i], current.buttonRect(i), i < current.buttonCount());
        }
        // Раскрытый список комбинатора выше самой строки: у CBS_DROPDOWNLIST
        // высота окна определяется родителем, и по высоте строки раскрывающийся
        // список не помещался бы в окно.
        const int dropHeight = dipToPx(120.0, dpi);
        const SettingsRect autoUpdateRect = current.autoUpdateRect();
        place(autoUpdate, autoUpdateRect, true);
        const SettingsRect languageLabelRect = current.languageLabelRect();
        place(languageLabel, languageLabelRect, !languageLabelRect.empty());
        const SettingsRect languageComboRect = current.languageComboRect();
        place(languageCombo,
              SettingsRect{languageComboRect.x, languageComboRect.y, languageComboRect.width, dropHeight},
              !languageComboRect.empty());
        const SettingsRect safetyLabelRect = current.safetyLabelRect();
        place(safetyLabel, safetyLabelRect, !safetyLabelRect.empty());
        const SettingsRect safetyComboRect = current.safetyComboRect();
        place(safetyCombo,
              SettingsRect{safetyComboRect.x, safetyComboRect.y, safetyComboRect.width, dropHeight},
              !safetyComboRect.empty());
        syncColumns();
    }

    // --- Действия -------------------------------------------------------------

    void command(ControlId id) {
        switch (id) {
        case ControlId::CheckNow: {
            // Ручная проверка не ограничена интервалом в 24 часа (§9.2 п.1) —
            // ограничение касается автоматической проверки при запуске.
            if (!model.beginCheck()) break;
            if (callbacks.onCheckNow) callbacks.onCheckNow();
            break;
        }
        case ControlId::ImportRules:
            if (callbacks.onImportRules) callbacks.onImportRules();
            break;
        case ControlId::RestoreEmbedded:
            if (model.requestRestoreEmbedded(nowUnixSeconds()) && callbacks.onRestoreEmbedded) {
                callbacks.onRestoreEmbedded();
            }
            break;
        case ControlId::ExportRuleSet:
            if (callbacks.onExportRuleSet) callbacks.onExportRuleSet(model.ruleSetExportJson());
            break;
        case ControlId::ExportStatistics:
            if (callbacks.onExportStatistics) callbacks.onExportStatistics(model.statisticsText());
            break;
        }
    }

    // Отложенная перерисовка модели. Клавиши приходят в родителя из подкласса
    // контрола, а строки добавляются и удаляются в syncList(): и то и другое —
    // изменение списка из его же обработчика, на которое comctl32 не рассчитан.
    // Поэтому визуальное обновление всегда на следующем витке очереди
    // сообщений, а модель меняется сразу: она и есть источник истины.
    void requestSync() {
        if (window != nullptr) ::PostMessageW(window, kMsgSyncModel, 0, 0);
    }

    void rememberScroll() {
        if (list == nullptr) return;
        const int top = ListView_GetTopIndex(list);
        if (top < 0) return;
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        model.setScrollOffsetDip(static_cast<int>(scale.undo(top)));
    }

    void refreshAll() {
        if (!controlsReady) return;
        rememberScroll();
        syncList();
        syncTexts();
        syncCombos();
        syncButtons();
        layout();
        if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
    }
};

ViewState* stateOf(HWND window) {
    return reinterpret_cast<ViewState*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

// Значок риска (§7.2: «иконка риска» у каждого элемента). Форма различает уровень
// без цвета: при высокой контрастности три цвета риска вырождаются в цвет текста
// (theme.cpp), и «безопасно» с «рискованно» обязаны отличаться чем-то ещё.
void paintRiskIcon(HDC dc, const RECT& item, SafetyLevel safety, const theme::Palette& palette, int side) {
    if (side <= 0) return;
    const theme::Color color = theme::riskColor(palette, safety);
    const theme::Color edge = theme::ensureContrast(palette.windowBackground, color, 3.0);
    const int left = item.right - side - 4;
    const int top = item.top + ((item.bottom - item.top) - side) / 2;
    if (left < item.left || top < item.top) return;  // строка уже кончилась
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
                         {box.left + side / 2, box.top + side / 2}};
        ::Polygon(dc, diamond, 4);
        break;
    }
    }
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(brush);
    ::DeleteObject(pen);
}

// Пояснение вместо пустого списка правил: рамка, заголовок, причина и действие.
// Рисуется GDI по WM_DRAWITEM — тот же путь, что у соседних экранов, и он не
// зависит от Direct2D: если рендерер не поднялся, список правил всё равно должен
// объяснять, почему он пуст (§5).
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

    ::SetBkMode(dc, TRANSPARENT);
    if (state.fonts[1] != nullptr) ::SelectObject(dc, state.fonts[1]);
    ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
    RECT title{inner.left, inner.top, inner.right, std::min(inner.bottom, inner.top + titleHeight)};
    const std::wstring headline = toWide(text("settings.empty.title"));
    ::DrawTextW(dc, headline.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    if (state.fonts[2] != nullptr) ::SelectObject(dc, state.fonts[2]);
    ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
    constexpr DWORD wrap = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;
    int y = title.bottom + scale.dip(4.0);
    for (const char* key : {"settings.empty.why", "settings.empty.what"}) {
        const std::wstring body = toWide(text(key));
        RECT line{inner.left, y, inner.right, inner.bottom};
        (void)::DrawTextW(dc, body.c_str(), -1, &line, wrap | DT_CALCRECT);
        if (line.bottom <= y) break;
        (void)::DrawTextW(dc, body.c_str(), -1, &line, wrap);
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
        // Свой HWND известен уже здесь, а WM_CREATE (следующим сообщением)
        // создаёт детей именно от него: без этой строки CreateWindowExW получил
        // бы пустого родителя и дочерние окна не создались бы.
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
        case WM_SIZE: state->layout(); return 0;
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
                               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            state->applyPalette();
            state->applyFonts();
            state->refreshAll();
            return 0;
        }
        case WM_SETFOCUS:
            // §5 «Клавиатурная навигация, фокус»: фокус должен быть виден и стоять
            // на содержимом, а не на подписи.
            if (state->list != nullptr) ::SetFocus(state->list);
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
            const WORD id = LOWORD(wParam);
            const WORD notification = HIWORD(wParam);
            if (state->autoUpdate != nullptr &&
                id == static_cast<WORD>(detail::kChildAutoUpdate) && notification == BN_CLICKED) {
                state->model.setAutoUpdate(
                    ::SendMessageW(state->autoUpdate, BM_GETCHECK, 0, 0) == BST_CHECKED);
                state->refreshAll();
                return 0;
            }
            if (state->languageCombo != nullptr && id == static_cast<WORD>(detail::kChildLanguageCombo) &&
                notification == CBN_SELCHANGE) {
                const int index = state->comboSelection(state->languageCombo);
                const Language language = index == 1 ? Language::English : Language::Russian;
                state->model.setLanguage(language);
                if (state->callbacks.onLanguageChanged) state->callbacks.onLanguageChanged(language);
                state->requestSync();
                return 0;
            }
            if (state->safetyCombo != nullptr && id == static_cast<WORD>(detail::kChildSafetyCombo) &&
                notification == CBN_SELCHANGE) {
                const int index = state->comboSelection(state->safetyCombo);
                const std::string ruleId = state->model.selectedRuleId();
                if (index < 0 || ruleId.empty()) break;
                // Risky требует второго подтверждения (§4 FR-9, §9): первое
                // нажатие вооружает правило и возвращает false, поэтому комбинатор
                // возвращается к прежнему уровню, а пояснение объясняет, что
                // нужно нажать ещё раз.
                (void)state->model.requestSafety(ruleId, safetyFromIndex(static_cast<std::size_t>(index)));
                state->requestSync();
                return 0;
            }
            if (state->filterEdit != nullptr && id == static_cast<WORD>(detail::kChildFilterEdit) &&
                notification == EN_CHANGE) {
                if (state->filterSyncing) break;
                state->model.setFilter(state->childText(state->filterEdit));
                state->requestSync();
                return 0;
            }
            if (notification != BN_CLICKED) break;
            if (!isSettingsControl(id)) break;
            state->command(static_cast<ControlId>(id));
            state->refreshAll();
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header == nullptr) break;
            if (header->hwndFrom != state->list) break;
            switch (header->code) {
            case LVN_ITEMCHANGED: {
                const auto* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if (change == nullptr) break;
                const int index = change->iItem;
                if (index < 0 || static_cast<std::size_t>(index) >= state->rowKeys.size()) break;
                const std::string ruleId = state->rowKeys[static_cast<std::size_t>(index)];
                const bool wasSelected = (change->uOldState & LVIS_SELECTED) != 0;
                const bool isSelected = (change->uNewState & LVIS_SELECTED) != 0;
                if (!state->syncing && wasSelected != isSelected) {
                    state->model.setSelectedRule(ruleId);
                    state->syncCombos();
                    state->syncTexts();
                    return 0;
                }
                if (state->syncing) break;
                if ((change->uChanged & LVIF_STATE) == 0) break;
                const UINT stateMask = LVIS_STATEIMAGEMASK;
                const UINT flipped = static_cast<UINT>(change->uNewState ^ change->uOldState) & stateMask;
                if (flipped == 0) break;
                const bool checked = (change->uNewState & stateMask) != 0;
                if (checked == state->model.ruleEnabled(ruleId)) break;
                // Модель — единственный источник истины о включении правила,
                // поэтому собственное циклирование флажка отменяем, а своё
                // состояние переносим отложенной перерисовкой: вставлять и
                // удалять строки внутри этого уведомления нельзя.
                ListView_SetItemState(state->list, static_cast<WPARAM>(index),
                                      checked ? 0u : stateMask, stateMask);
                (void)state->model.toggleRule(ruleId);
                state->requestSync();
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
                    const std::size_t index = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
                    const std::vector<RuleRow>& rows = state->model.rules();
                    const bool enabled = index < rows.size() && rows[index].enabled;
                    // Цвет подписи — из темы, а не системный: на тёмной палитре
                    // системный чёрный текст нечитаем. Выключенное правило
                    // показываем приглушённым, иначе «выключено» читается только
                    // по отсутствию флажка.
                    ::SetTextColor(draw->nmcd.hdc,
                                   theme::colorRef(enabled ? palette.textPrimary : palette.textSecondary));
                    ::SetBkMode(draw->nmcd.hdc, TRANSPARENT);
                    return CDRF_NEWFONT;
                }
                case CDDS_ITEMPOSTPAINT: {
                    const std::size_t index = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
                    const std::vector<RuleRow>& rows = state->model.rules();
                    if (index >= rows.size()) return CDRF_DODEFAULT;
                    paintRiskIcon(draw->nmcd.hdc, draw->nmcd.rc, rows[index].safety, palette,
                                  std::max(6, scale.dip(10.0)));
                    return CDRF_DODEFAULT;
                }
                default: break;
                }
                return CDRF_DODEFAULT;
            }
            default: break;
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw == nullptr) break;
            if (draw->CtlType == ODT_STATIC && draw->CtlID == static_cast<UINT>(detail::kChildHint) &&
                draw->hwndItem == state->hint) {
                return detail::drawHint(*state, *draw);
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const theme::Palette& palette = state->theme.palette();
            ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
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
        case kMsgSyncModel: state->refreshAll(); return 0;
        case mv::kFeedMessage:
            // Набор правил или причина, по которой он не прочитан. Модель
            // меняется только здесь, в UI-потоке (§6.1).
            state->applyFeedFrames();
            return 0;
        case WM_ERASEBKGND:
            // Дети перекрывают окно целиком; стирать собственную поверхность
            // незачем, а лишнее стирание мигает при перерисовке списка.
            return 1;
        case WM_DESTROY: state->controlsReady = false; return 0;
        case WM_NCDESTROY: ::SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        default: break;
        }
    } catch (const std::exception& error) {
        logEvent(core::LogLevel::Error, "ui.settings.exception", error.what());
        return message == WM_CREATE ? -1 : 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

// Подкласс контрола. Нужен для двух вещей: чтобы вернуть исходную процедуру при
// уничтожении (WM_NCDESTROY) и чтобы клавиши доходили до модели (§5
// «Клавиатурная навигация, фокус»).
//
// Клавиши пересылаются только из списка и только те, которые он сам не
// использует. Пробел отправляется нативному флажку LVS_EX_CHECKBOXES: иначе
// пробел в поле фильтра вставлял бы пробел в модель, а пробел в списке не
// переключил бы правило.
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = stateOf(window);
    if (state == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        state->removeChild(window);
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    if (message == WM_KEYDOWN && state->window != nullptr && window == state->list &&
        wParam != static_cast<WPARAM>(VK_SPACE)) {
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
// SettingsScreen
// ---------------------------------------------------------------------------

struct SettingsScreen::Impl : detail::ViewState {
    Impl() { dpi = static_cast<int>(theme.metrics().dpi); }
};

SettingsScreen::SettingsScreen(Callbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->callbacks = std::move(callbacks);
}

SettingsScreen::~SettingsScreen() { destroy(); }

HWND SettingsScreen::create(HWND parent, int dpi) {
    auto& state = *impl_;
    if (parent == nullptr) return nullptr;
    ensureStrings();
    if (state.window != nullptr) return state.window;
    if (dpi > 0) state.dpi = dpi;

    // SysListView32 без ICC_* не создаётся вовсе, а §7 требует именно его.
    // Повторный вызов безвреден.
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    if (::InitCommonControlsEx(&controls) == FALSE) {
        logWin32("ui.settings.create", "InitCommonControlsEx", ::GetLastError());
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = 0;
    windowClass.lpfnWndProc = &detail::viewProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = detail::kSettingsViewClass;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logWin32("ui.settings.create", "RegisterClassExW", ::GetLastError());
        return nullptr;
    }
    state.window = ::CreateWindowExW(0, detail::kSettingsViewClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                     parent, nullptr, windowClass.hInstance, &state);
    if (state.window == nullptr) {
        logWin32("ui.settings.create", "CreateWindowExW(view)", ::GetLastError());
        return nullptr;
    }
    state.applyPalette();
    state.applyFonts();
    state.attachFeed();
    state.refreshAll();
    return state.window;
}

HWND SettingsScreen::window() const noexcept { return impl_->window; }

void SettingsScreen::destroy() noexcept {
    auto& state = *impl_;
    if (state.window == nullptr) return;
    HWND window = state.window;
    state.window = nullptr;
    state.controlsReady = false;
    // Геометрия снимается до разрушения окна: после DestroyWindow снимать
    // нечего, а мост (задача 75) спросит положение окна при закрытии.
    state.rememberScroll();
    ::DestroyWindow(window);
    state.detachFeed();
    state.children.clear();
    state.rowKeys.clear();
    state.list = nullptr;
    state.status = nullptr;
    state.details = nullptr;
    state.about = nullptr;
    state.hint = nullptr;
    state.filterLabel = nullptr;
    state.filterEdit = nullptr;
    state.autoUpdate = nullptr;
    state.languageLabel = nullptr;
    state.languageCombo = nullptr;
    state.safetyLabel = nullptr;
    state.safetyCombo = nullptr;
    state.buttons.fill(nullptr);
}

void SettingsScreen::setDpi(int dpi) {
    auto& state = *impl_;
    if (dpi > 0) state.dpi = dpi;
    state.theme.setDpi(static_cast<unsigned>(state.dpi));
    if (state.window == nullptr) return;
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
}

SettingsViewModel& SettingsScreen::model() noexcept { return impl_->model; }
const SettingsViewModel& SettingsScreen::model() const noexcept { return impl_->model; }

void SettingsScreen::refresh() { impl_->refreshAll(); }

void SettingsScreen::reloadTheme() {
    auto& state = *impl_;
    state.reloadTheme();
    state.refreshAll();
}

void SettingsScreen::publishRuleSet(std::shared_ptr<const core::RuleSet> rules) {
    auto& state = *impl_;
    state.model.publishRuleSet(std::move(rules));
    state.refreshAll();
}

void SettingsScreen::publishRuleSet(core::RuleSet rules) {
    auto& state = *impl_;
    state.model.publishRuleSet(std::move(rules));
    state.refreshAll();
}

void SettingsScreen::publishCheckResult(const core::RuleSetVerification& verification,
                                       std::int64_t nowUnixSeconds) {
    auto& state = *impl_;
    state.model.publishCheckResult(verification, nowUnixSeconds);
    state.refreshAll();
}

void SettingsScreen::setCheckedStatus(core::RuleSetStatus status) {
    auto& state = *impl_;
    state.model.setCheckedStatus(std::move(status));
    state.refreshAll();
}

void SettingsScreen::setWindowPlacement(const WINDOWPLACEMENT& placement) noexcept {
    impl_->placement = placement;
    impl_->hasPlacement = true;
}

WINDOWPLACEMENT SettingsScreen::windowPlacement() const noexcept { return impl_->placement; }

bool SettingsScreen::hasWindowPlacement() const noexcept { return impl_->hasPlacement; }

}  // namespace mrproper::ui::settings
