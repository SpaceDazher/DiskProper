// Навигация и страницы — реализация. Спека и границы слоёв описаны в nav.hpp:
// здесь только арифметика рельса, переходы и текстовое сохранение состояния.
//
// Файл не включает windows.h намеренно: рельс не должен зависеть от окна, иначе
// его геометрию и сохранение состояния нельзя проверить без запуска UI, а
// «пять страниц» — без живого окна (SPEC §5: устойчивость, ни один отказ не
// роняет процесс).
#include "ui/nav.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>
#include <utility>

namespace mrproper::ui {
namespace {

// Глубина истории «назад». Ограничение осознанное: сессия очистки может
// длиться часами, а неограниченный вектор страниц — это память, которой
// никто не пользуется (пятидесяти шагов назад достаточно всем).
constexpr std::size_t kHistoryLimit = 64;

// Предохранители разбора. Настройки правит человек, а значения приходят из
// файла: «scroll=-4000000» не должен ни уронить экран, ни съесть память.
constexpr int kMaxPersistedDip = 1000000;
constexpr int kMaxPersistedPx = 10000000;

int clampInt(int value, int low, int high) noexcept {
    if (value < low) {
        return low;
    }
    return (value > high) ? high : value;
}

// Сравнение ASCII без учёта регистра: ключи в настройках пишут руками, и
// «Overview» вместо «overview» — это не повод терять сохранённое состояние.
bool iequalsAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// Число из строки без исключений и без локали: std::from_chars не бросает и не
// зависит от C-локали (запятая как разделитель не сдвинет разбор).
bool parseInt(std::string_view text, int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    out = value;
    return true;
}

// --- Кодирование значений -----------------------------------------------------
//
// Значение может содержать что угодно: пользовательский фильтр — это свободный
// текст. Поэтому разделители формата (';', ',', '=', '%') и управляющие
// символы экранируются, а «безопасный» набор (буквы, цифры, '-', '_', '.', '~')
// остаётся читаемым в файле настроек.

bool isUnreserved(char c) noexcept {
    const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    const bool digit = (c >= '0' && c <= '9');
    return alpha || digit || c == '-' || c == '_' || c == '.' || c == '~';
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

std::string encodeComponent(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (isUnreserved(c)) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[(uc >> 4) & 0x0F]);
            out.push_back(kHex[uc & 0x0F]);
        }
    }
    return out;
}

bool decodeComponent(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= text.size()) {
            return false;
        }
        const int hi = hexValue(text[i + 1]);
        const int lo = hexValue(text[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

}  // namespace

// --- Страницы ----------------------------------------------------------------

const PageDescriptor& pageDescriptor(PageId page) noexcept {
    const std::size_t index = pageIndex(page);
    return kPages[index];
}

bool isValidPage(PageId page) noexcept {
    return static_cast<unsigned>(page) < static_cast<unsigned>(PageId::Count);
}

std::size_t pageIndex(PageId page) noexcept {
    const unsigned raw = static_cast<unsigned>(page);
    return (raw < kPageCount) ? static_cast<std::size_t>(raw) : 0;
}

std::optional<PageId> pageByIndex(std::size_t index) noexcept {
    if (index >= kPageCount) {
        return std::nullopt;
    }
    return kPages[index].id;
}

std::optional<PageId> pageByKey(std::string_view key) noexcept {
    for (const PageDescriptor& descriptor : kPages) {
        if (iequalsAscii(descriptor.key, key)) {
            return descriptor.id;
        }
    }
    return std::nullopt;
}

std::string_view pageKey(PageId page) noexcept {
    return pageDescriptor(page).key;
}

std::string_view pageTitleKey(PageId page) noexcept {
    return pageDescriptor(page).titleKey;
}

std::string_view pageTitleFallback(PageId page, core::Language lang) noexcept {
    const PageDescriptor& descriptor = pageDescriptor(page);
    return (lang == core::Language::English) ? descriptor.titleEn : descriptor.titleRu;
}

int pageOrdinal(PageId page) noexcept {
    return static_cast<int>(pageIndex(page)) + 1;
}

std::optional<PageId> pageByOrdinal(int ordinal) noexcept {
    if (ordinal < 1 || ordinal > static_cast<int>(kPageCount)) {
        return std::nullopt;
    }
    return kPages[static_cast<std::size_t>(ordinal - 1)].id;
}

char pageAccessKey(PageId page) noexcept {
    // '1'..'5' — и символ, и код виртуальной клавиши Win32 (0x31..0x35).
    return static_cast<char>('1' + pageOrdinal(page) - 1);
}

bool pageMatchesAccessKey(PageId page, std::uint32_t virtualKey) noexcept {
    if (!isValidPage(page)) {
        return false;
    }
    return virtualKey == static_cast<std::uint32_t>(static_cast<unsigned char>(pageAccessKey(page)));
}

// --- Перевод DIP в пиксели ----------------------------------------------------

RailSide defaultRailSide(core::Language lang) noexcept {
    return core::isRtl(lang) ? RailSide::Right : RailSide::Left;
}

int dipToPx(double dip, int dpi) noexcept {
    // !(dip > 0.0) вместо dip <= 0.0: NaN тоже должен давать ноль, иначе
    // испорченные метрики превращаются в отрицательный прямоугольник.
    if (!(dip > 0.0)) {
        return 0;
    }
    const double effectiveDpi = (dpi > 0) ? static_cast<double>(dpi) : static_cast<double>(kDefaultDpi);
    const double px = dip * effectiveDpi / static_cast<double>(kDefaultDpi);
    constexpr double kIntMax = 2147483000.0;
    if (px >= kIntMax) {
        return 2147483000;
    }
    return static_cast<int>(px + 0.5);
}

int dpiScalePercent(int dpi) noexcept {
    const int effectiveDpi = (dpi > 0) ? dpi : kDefaultDpi;
    return (effectiveDpi * 100) / kDefaultDpi;
}

bool RailRect::empty() const noexcept {
    return width <= 0 || height <= 0;
}

bool RailRect::contains(int px, int py) const noexcept {
    if (empty()) {
        return false;
    }
    return px >= x && px < x + width && py >= y && py < y + height;
}

// --- Макет рельса -------------------------------------------------------------

RailLayout RailLayout::compute(const RailMetrics& metrics, int dpi, int windowHeightPx, RailSide side,
                               int scrollOffsetPx) {
    RailLayout layout;
    layout.side_ = side;
    layout.widthPx_ = dipToPx(metrics.widthDip, dpi);
    layout.itemHeightPx_ = std::max(1, dipToPx(metrics.itemHeightDip, dpi));
    layout.gapPx_ = std::max(0, dipToPx(metrics.itemGapDip, dpi));
    layout.paddingTopPx_ = std::max(0, dipToPx(metrics.paddingTopDip, dpi));
    layout.paddingBottomPx_ = std::max(0, dipToPx(metrics.paddingBottomDip, dpi));
    layout.minimumHeightPx_ = std::max(1, dipToPx(metrics.minimumHeightDip, dpi));
    layout.viewportHeightPx_ = (windowHeightPx > 0) ? windowHeightPx : 0;
    layout.stridePx_ = layout.itemHeightPx_ + layout.gapPx_;
    const int items = static_cast<int>(kPageCount);
    layout.contentHeightPx_ = layout.paddingTopPx_ + items * layout.itemHeightPx_ +
                              (items - 1) * layout.gapPx_ + layout.paddingBottomPx_;
    layout.scrollOffsetPx_ = clampInt(scrollOffsetPx, 0, layout.maxScrollOffsetPx());
    return layout;
}

RailSide RailLayout::side() const noexcept {
    return side_;
}

int RailLayout::railWidthPx() const noexcept {
    return widthPx_;
}

int RailLayout::itemHeightPx() const noexcept {
    return itemHeightPx_;
}

int RailLayout::itemStridePx() const noexcept {
    return stridePx_;
}

int RailLayout::contentHeightPx() const noexcept {
    return contentHeightPx_;
}

int RailLayout::viewportHeightPx() const noexcept {
    return viewportHeightPx_;
}

int RailLayout::minimumHeightPx() const noexcept {
    return minimumHeightPx_;
}

int RailLayout::scrollOffsetPx() const noexcept {
    return scrollOffsetPx_;
}

int RailLayout::maxScrollOffsetPx() const noexcept {
    return std::max(0, contentHeightPx_ - viewportHeightPx_);
}

bool RailLayout::scrollable() const noexcept {
    return maxScrollOffsetPx() > 0;
}

RailRect RailLayout::itemRectWithoutScroll(PageId page) const noexcept {
    RailRect rect;
    if (!isValidPage(page)) {
        return rect;
    }
    rect.x = 0;
    rect.y = paddingTopPx_ + static_cast<int>(pageIndex(page)) * stridePx_;
    rect.width = widthPx_;
    rect.height = itemHeightPx_;
    return rect;
}

RailRect RailLayout::itemRect(PageId page) const noexcept {
    RailRect rect = itemRectWithoutScroll(page);
    rect.y -= scrollOffsetPx_;
    return rect;
}

RailRect RailLayout::toClient(const RailRect& logical, int windowWidthPx) const noexcept {
    if (side_ == RailSide::Left || windowWidthPx <= 0) {
        return logical;
    }
    RailRect mirrored = logical;
    mirrored.x = windowWidthPx - widthPx_ - logical.x;
    return mirrored;
}

std::optional<PageId> RailLayout::hitTest(int x, int y) const noexcept {
    if (x < 0 || x >= widthPx_) {
        return std::nullopt;
    }
    if (y < 0 || y >= viewportHeightPx_) {
        return std::nullopt;
    }
    // Прокрутка складывается с координатой окна, поэтому «первый видимый
    // пункт» может быть не нулевым индексом.
    const int content = y + scrollOffsetPx_ - paddingTopPx_;
    if (content < 0) {
        return std::nullopt;  // верхний отступ рельса
    }
    const int index = content / stridePx_;
    if (index < 0 || index >= static_cast<int>(kPageCount)) {
        return std::nullopt;  // нижний отступ или за содержимым
    }
    if (content - index * stridePx_ >= itemHeightPx_) {
        return std::nullopt;  // зазор между пунктами никому не принадлежит
    }
    return kPages[static_cast<std::size_t>(index)].id;
}

int RailLayout::scrollOffsetToShow(PageId page, int marginPx) const noexcept {
    if (!isValidPage(page)) {
        return scrollOffsetPx_;
    }
    const int margin = std::max(0, marginPx);
    const int top = paddingTopPx_ + static_cast<int>(pageIndex(page)) * stridePx_;
    const int bottom = top + itemHeightPx_;
    int wanted = scrollOffsetPx_;
    if (top - wanted < margin) {
        wanted = top - margin;
    } else if (bottom - wanted > viewportHeightPx_ - margin) {
        wanted = bottom - viewportHeightPx_ + margin;
    }
    return clampInt(wanted, 0, maxScrollOffsetPx());
}

// --- Состояние страницы ------------------------------------------------------

bool PageState::operator==(const PageState& other) const {
    return scrollOffsetDip == other.scrollOffsetDip && splitterDip == other.splitterDip &&
           selectedKey == other.selectedKey && expandedKeys == other.expandedKeys && filter == other.filter;
}

bool PageState::operator!=(const PageState& other) const {
    return !(*this == other);
}

std::string encodePageState(const PageState& state) {
    std::string out = "v=1";
    out += ";scroll=";
    out += std::to_string(state.scrollOffsetDip);
    out += ";splitter=";
    out += std::to_string(state.splitterDip);
    out += ";selected=";
    out += encodeComponent(state.selectedKey);
    out += ";expanded=";
    for (std::size_t i = 0; i < state.expandedKeys.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += encodeComponent(state.expandedKeys[i]);
    }
    out += ";filter=";
    out += encodeComponent(state.filter);
    return out;
}

std::optional<PageState> decodePageState(std::string_view text) {
    PageState state;
    bool versionSeen = false;

    // Поля независимы: сначала собираем сырые строки, потом разбираем, чтобы
    // порядок в файле не влиял на результат.
    std::string selected;
    std::vector<std::string> expanded;
    std::string filter;
    int scroll = 0;
    int splitter = 0;
    bool hasScroll = false;
    bool hasSplitter = false;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t sep = text.find(';', pos);
        const std::string_view token =
            (sep == std::string_view::npos) ? text.substr(pos) : text.substr(pos, sep - pos);
        pos = (sep == std::string_view::npos) ? text.size() + 1 : sep + 1;

        if (!token.empty()) {
            const std::size_t eq = token.find('=');
            const std::string_view name = (eq == std::string_view::npos) ? token : token.substr(0, eq);
            const std::string_view value = (eq == std::string_view::npos) ? std::string_view() : token.substr(eq + 1);

            if (name == "v") {
                int version = 0;
                if (!parseInt(value, version) || version != kNavStateVersion) {
                    return std::nullopt;  // файл писала другая (более новая) версия
                }
                versionSeen = true;
            } else if (name == "scroll") {
                hasScroll = parseInt(value, scroll);
            } else if (name == "splitter") {
                hasSplitter = parseInt(value, splitter);
            } else if (name == "selected") {
                if (!decodeComponent(value, selected)) {
                    return std::nullopt;
                }
            } else if (name == "filter") {
                if (!decodeComponent(value, filter)) {
                    return std::nullopt;
                }
            } else if (name == "expanded") {
                expanded.clear();
                // Пустое значение — это «ничего не раскрыто», а не один
                // пустой узел: без этой проверки пустой список превращался бы в
                // вектор из одного пустого ключа, и дерево раскрывало бы
                // несуществующий узел.
                if (!value.empty()) {
                    std::size_t itemPos = 0;
                    while (itemPos <= value.size()) {
                        const std::size_t itemSep = value.find(',', itemPos);
                        const std::string_view item = (itemSep == std::string_view::npos)
                                                         ? value.substr(itemPos)
                                                         : value.substr(itemPos, itemSep - itemPos);
                        itemPos = (itemSep == std::string_view::npos) ? value.size() + 1 : itemSep + 1;
                        std::string decoded;
                        if (!decodeComponent(item, decoded)) {
                            return std::nullopt;
                        }
                        expanded.push_back(std::move(decoded));
                    }
                }
            }
            // Неизвестное поле игнорируем: формат версионирован, новая версия
            // приложения вправе дописать своё, старая не должна этим мешать.
        }

        if (sep == std::string_view::npos) {
            break;
        }
    }

    if (!versionSeen) {
        return std::nullopt;
    }
    state.scrollOffsetDip = hasScroll ? clampInt(scroll, 0, kMaxPersistedDip) : 0;
    state.splitterDip = hasSplitter ? clampInt(splitter, 0, kMaxPersistedDip) : 0;
    state.selectedKey = std::move(selected);
    state.expandedKeys = std::move(expanded);
    state.filter = std::move(filter);
    return state;
}

// --- Навигатор ----------------------------------------------------------------

Navigator::Navigator() = default;

PageId Navigator::current() const noexcept {
    return current_;
}

PageId Navigator::selection() const noexcept {
    return selection_;
}

std::uint64_t Navigator::revision() const noexcept {
    return revision_;
}

core::Language Navigator::language() const noexcept {
    return language_;
}

void Navigator::setLanguage(core::Language lang) {
    if (lang == language_) {
        return;
    }
    language_ = lang;
    // Подписи рельса берутся из запасного набора, пока не загружен каталог
    // строк, поэтому смена языка — это изменение содержимого, а не только
    // содержимого настроек: без нового revision рельс останется на старом.
    bumpRevision();
}

void Navigator::setTitleResolver(TitleResolver resolver) {
    titleResolver_ = std::move(resolver);
    bumpRevision();
}

void Navigator::bumpRevision() noexcept {
    ++revision_;
}

void Navigator::markDirty(PageId page) noexcept {
    dirty_[indexFor(page)] = true;
}

std::size_t Navigator::indexFor(PageId page) const noexcept {
    return pageIndex(page);
}

void Navigator::notifyPageChanged(PageId from, PageId to) {
    if (!pageChanged_) {
        return;
    }
    // Копия обработчика: он может переподписаться прямо во время вызова, и
    // смена подписки не должна отменять уже начатое уведомление.
    const PageChangedHandler handler = pageChanged_;
    handler(from, to);
}

bool Navigator::goTo(PageId page) {
    if (!isValidPage(page) || page == current_) {
        return false;
    }
    const PageId from = current_;
    current_ = page;
    selection_ = page;
    if (backHistory_.size() >= kHistoryLimit) {
        backHistory_.erase(backHistory_.begin());
    }
    backHistory_.push_back(from);
    forwardHistory_.clear();
    markDirty(page);  // «какая страница открыта» — тоже сохраняемое состояние
    bumpRevision();
    notifyPageChanged(from, page);
    return true;
}

bool Navigator::goToIndex(std::size_t index) {
    const std::optional<PageId> page = pageByIndex(index);
    return page ? goTo(*page) : false;
}

bool Navigator::goByOrdinal(int ordinal) {
    const std::optional<PageId> page = pageByOrdinal(ordinal);
    return page ? goTo(*page) : false;
}

bool Navigator::goToNext() {
    const std::optional<PageId> next = pageByIndex(pageIndex(current_) + 1);
    return next ? goTo(*next) : false;
}

bool Navigator::goToPrevious() {
    const std::size_t index = pageIndex(current_);
    if (index == 0) {
        return false;
    }
    return goTo(kPages[index - 1].id);
}

bool Navigator::goToFirst() {
    return goTo(kPages.front().id);
}

bool Navigator::goToLast() {
    return goTo(kPages.back().id);
}

void Navigator::moveSelection(int delta) {
    if (delta == 0) {
        return;
    }
    const auto count = static_cast<long long>(kPageCount);
    // long long: delta может прийти из разбора WM_KEYDOWN и быть большим по
    // модулю, а отрицательный остаток от деления на положительное — это ловушка,
    // из-за которой стрелка вверх из первого пункта уехала бы в середину.
    long long next = (static_cast<long long>(pageIndex(selection_)) + static_cast<long long>(delta)) % count;
    if (next < 0) {
        next += count;
    }
    setSelection(kPages[static_cast<std::size_t>(next)].id);
}

void Navigator::setSelection(PageId page) {
    if (!isValidPage(page) || page == selection_) {
        return;
    }
    selection_ = page;
    // Фокус рельса — часть того, что нарисовано, поэтому revision растёт.
    bumpRevision();
}

bool Navigator::activateSelection() {
    return goTo(selection_);
}

bool Navigator::selectByAccessKey(std::uint32_t virtualKey) {
    for (const PageDescriptor& descriptor : kPages) {
        if (pageMatchesAccessKey(descriptor.id, virtualKey)) {
            const PageId page = descriptor.id;
            setSelection(page);
            return goTo(page);
        }
    }
    return false;
}

bool Navigator::canGoBack() const noexcept {
    return !backHistory_.empty();
}

bool Navigator::canGoForward() const noexcept {
    return !forwardHistory_.empty();
}

bool Navigator::goBack() {
    if (backHistory_.empty()) {
        return false;
    }
    const PageId from = current_;
    const PageId to = backHistory_.back();
    backHistory_.pop_back();
    forwardHistory_.push_back(from);
    current_ = to;
    selection_ = to;
    markDirty(to);
    bumpRevision();
    notifyPageChanged(from, to);
    return true;
}

bool Navigator::goForward() {
    if (forwardHistory_.empty()) {
        return false;
    }
    const PageId from = current_;
    const PageId to = forwardHistory_.back();
    forwardHistory_.pop_back();
    backHistory_.push_back(from);
    current_ = to;
    selection_ = to;
    markDirty(to);
    bumpRevision();
    notifyPageChanged(from, to);
    return true;
}

void Navigator::clearHistory() {
    backHistory_.clear();
    forwardHistory_.clear();
}

PageState& Navigator::stateFor(PageId page) noexcept {
    return states_[indexFor(page)];
}

const PageState& Navigator::state(PageId page) const noexcept {
    return states_[indexFor(page)];
}

void Navigator::setState(PageId page, PageState value) {
    if (!isValidPage(page)) {
        return;
    }
    states_[indexFor(page)] = std::move(value);
    markDirty(page);
    bumpRevision();
}

void Navigator::patchState(PageId page, const StateChange& change) {
    if (!isValidPage(page) || !change) {
        return;
    }
    change(states_[indexFor(page)]);
    markDirty(page);
    bumpRevision();
}

void Navigator::resetState(PageId page) {
    setState(page, PageState{});
}

void Navigator::resetAllStates() {
    states_ = std::array<PageState, kPageCount>{};
    for (std::size_t i = 0; i < kPageCount; ++i) {
        dirty_[i] = true;
    }
    bumpRevision();
}

bool Navigator::isStateDirty(PageId page) const noexcept {
    return dirty_[indexFor(page)];
}

bool Navigator::hasDirtyState() const noexcept {
    return std::any_of(dirty_.begin(), dirty_.end(), [](bool flag) { return flag; });
}

void Navigator::markSaved() {
    for (std::size_t i = 0; i < kPageCount; ++i) {
        dirty_[i] = false;
    }
}

int Navigator::railScrollPx() const noexcept {
    return railScrollPx_;
}

void Navigator::setRailScrollPx(int px) {
    const int value = (px > 0) ? px : 0;
    if (value == railScrollPx_) {
        return;
    }
    railScrollPx_ = value;
    markDirty(current_);
    bumpRevision();
}

int Navigator::maxRailScrollPx(const RailMetrics& metrics, int dpi, int windowHeightPx) const {
    return RailLayout::compute(metrics, dpi, windowHeightPx, defaultRailSide(language_)).maxScrollOffsetPx();
}

std::string Navigator::label(PageId page) const {
    const PageDescriptor& descriptor = pageDescriptor(page);
    if (titleResolver_) {
        try {
            const std::string resolved = titleResolver_(descriptor.titleKey);
            if (!resolved.empty()) {
                return resolved;
            }
        } catch (...) {
            // Каталог строк — зона вызывающего. Сбой в нём не имеет права ронять
            // отрисовку рельса: ниже берётся встроенная подпись, и экран
            // остаётся рабочим. Практика сброса резолвера — на стороне locale.
        }
    }
    return std::string(pageTitleFallback(page, language_));
}

RailItemView Navigator::item(PageId page) const {
    RailItemView view;
    view.page = page;
    view.index = pageIndex(page);
    view.label = label(page);
    const char key = pageAccessKey(page);
    view.accessKey.assign(1, key);
    view.shortcut = std::string(pageDescriptor(page).shortcut);
    view.active = (page == current_);
    view.selected = (page == selection_);
    return view;
}

std::vector<RailItemView> Navigator::items() const {
    std::vector<RailItemView> out;
    out.reserve(kPageCount);
    for (const PageDescriptor& descriptor : kPages) {
        out.push_back(item(descriptor.id));
    }
    return out;
}

void Navigator::exportState(NavStateStore& store) const {
    store[std::string(kNavStateVersionKey)] = std::to_string(kNavStateVersion);
    store[std::string(kNavCurrentPageKey)] = std::string(pageKey(current_));
    store[std::string(kNavRailScrollKey)] = std::to_string(railScrollPx_);
    for (const PageDescriptor& descriptor : kPages) {
        const std::string key = std::string(kNavPageStatePrefix) + std::string(descriptor.key);
        store[key] = encodePageState(states_[pageIndex(descriptor.id)]);
    }
}

std::size_t Navigator::importState(const NavStateStore& store, std::vector<std::string>* problems) {
    const auto report = [problems](std::string message) {
        if (problems != nullptr) {
            problems->push_back(std::move(message));
        }
    };

    // Версия формата проверяется до всего остального: запись новой версии
    // приложения нельзя частично принять — половина полей прочиталась бы как
    // свои, и состояние оказалось бы смесью двух форматов.
    const auto versionEntry = store.find(std::string(kNavStateVersionKey));
    if (versionEntry != store.end()) {
        int version = 0;
        if (!parseInt(versionEntry->second, version) || version != kNavStateVersion) {
            report("nav.version=" + versionEntry->second + ": состояние навигации не применено, формат чужой");
            return 0;
        }
    }

    std::size_t applied = 0;

    const auto currentEntry = store.find(std::string(kNavCurrentPageKey));
    if (currentEntry != store.end()) {
        const std::optional<PageId> page = pageByKey(currentEntry->second);
        if (page) {
            current_ = *page;
            selection_ = *page;
            ++applied;
        } else {
            report("nav.current=" + currentEntry->second + ": неизвестная страница, оставлена текущая");
        }
    }

    const auto scrollEntry = store.find(std::string(kNavRailScrollKey));
    if (scrollEntry != store.end()) {
        int scroll = 0;
        if (parseInt(scrollEntry->second, scroll)) {
            // Верхняя граница от макета неизвестна в этот момент (окна ещё
            // может не быть) — макет обрежет прокрутку при первом расчёте.
            railScrollPx_ = clampInt(scroll, 0, kMaxPersistedPx);
            ++applied;
        } else {
            report("nav.rail.scroll=" + scrollEntry->second + ": не число, прокрутка рельса сброшена");
        }
    }

    for (const PageDescriptor& descriptor : kPages) {
        const std::string key = std::string(kNavPageStatePrefix) + std::string(descriptor.key);
        const auto entry = store.find(key);
        if (entry == store.end()) {
            continue;
        }
        const std::optional<PageState> state = decodePageState(entry->second);
        if (state) {
            states_[pageIndex(descriptor.id)] = *state;
            dirty_[pageIndex(descriptor.id)] = false;
            ++applied;
        } else {
            report(key + "=" + entry->second + ": запись не наша, состояние страницы оставлено прежним");
        }
    }

    if (applied != 0) {
        // Импорт — это восстановление из сохранения, а не новое изменение:
        // писать обратно то, что только что прочитали, незачем.
        clearHistory();
        markSaved();
        bumpRevision();
    }
    return applied;
}

void Navigator::resetAll() {
    const PageId from = current_;
    current_ = kStartupPage;
    selection_ = kStartupPage;
    railScrollPx_ = 0;
    states_ = std::array<PageState, kPageCount>{};
    for (std::size_t i = 0; i < kPageCount; ++i) {
        dirty_[i] = true;
    }
    clearHistory();
    bumpRevision();
    if (from != current_) {
        notifyPageChanged(from, current_);
    }
}

}  // namespace mrproper::ui
