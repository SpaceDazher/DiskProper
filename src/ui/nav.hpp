// Навигация и страницы: рельс, пять экранов, переключение, сохранение состояния.
//
// Спека: §7.1 (пять экранов: Обзор, Диски, Очистка, Отчёт, Настройки),
// §7.2 (гайдлайн по UX), §5 (доступность: клавиатурная навигация и фокус;
// DPI per-monitor v2; локализация ru + en, RTL-ready; тема светлая/тёмная),
// §6.1 (слой UI — единственный поток с сообщениями, рендер и ввод), §6.4
// (результаты не мутируются после публикации), §4 FR-9 (настройки хранятся
// локально), §8 Этап 0 (каркас страниц).
//
// ---------------------------------------------------------------------------
// Что этот модуль решает и почему без него нельзя
// ---------------------------------------------------------------------------
//
// Экраны приложения — это пять страниц одного окна, и между ними есть три
// вещи, которые нельзя размазывать по пяти view-файлам:
//
//   1. Кто сейчас открыт. Экраны (задачи W15/71-74) не должны знать друг о
//      друге: «Диски» не переключает «Очистку», его об этом не касается.
//      Переключение живёт здесь, а экран подписан на один обратный вызов.
//   2. Состояние каждой страницы. Возврат на «Очистку» после «Дисков» обязан
//      вернуть прокрутку, выделение, раскрытые ветки дерева и строку фильтра
//      (SPEC §5 доступность, §7.2 UX). Экраны эти поля не изобретают: у них
//      есть готовая PageState, которую они читают и пишут.
//   3. Сохранение между запусками. Настройки и отчёт живут на диске, и «какая
//      страница была открыта» — такое же постоянное свойство, как положение
//      разделителя. Формат задаётся здесь как обычный текст, а куда его
//      положить (реестр, JSON, INI) решает слой настроек (W15/74).
//
// Модуль намеренно не знает Win32: ни HWND, ни GDI, ни Direct2D в нём нет.
// Рельс — это пять прямоугольников и подписей, то есть чистая арифметика и
// текст. Окно и отрисовку делает app_shell (W14/66) и renderer (W14/67), а
// этот файл даёт им геометрию, попадания мышью, клавиатурные переходы и
// содержимое рельса. Такой разрез даёт две вещи, которых не даёт код внутри
// view: (а) кнопку «Открыть в проводнике» и DPI-арифметику можно проверить
// без окна; (б) ни один экран не может случайно решить, что «Настройки»
// открываются по Ctrl+, — это решение одно на всех.
//
// Подписи. Тексты живут в каталоге строк (ui/locale, задача W14/69), поэтому
// модуль отдаёт ключ (`nav.page.cleanup`) и умеет подставить подпись через
// переданный резолвер. Встроенный запасной вариант (ru/en) нужен потому, что
// каталог загружается позже каркаса: рельс должен быть осмысленным уже в
// первый кадр и не показывать ключ вместо слова «Очистка». Ключи и подписи
// в одном описании страницы — иначе «пять страниц» разъезжается между
// таблицей, перечислением и разметкой.
//
// Границы слоёв: зависимость ровно одна и только вниз — на переносимое ядро
// (`core::i18n` — язык, направление письма, RTL). Никаких обратных ссылок:
// engine/ui-views/locale подключают этот файл, он не знает их протоколов.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/i18n.hpp"

namespace mrproper::ui {

// --- Страницы (SPEC §7.1) ----------------------------------------------------

// Порядок enum = порядок пунктов в рельсе сверху вниз и порядок Alt+1..Alt+5.
enum class PageId : std::uint8_t {
    Overview = 0,  // Обзор: дашборд, карта дисков, быстрые действия
    Disks,         // Диски: дерево дисков/разделов/томов, карточка, экспорт карты
    Cleanup,       // Очистка: дерево категорий, чекбоксы, агрегаты, dry-run
    Report,        // Отчёт: результат операции, журнал, экспорт
    Settings,      // Настройки: правила, безопасность, автозапуск, о программе
    Count,         // Не страница: конец перечисления
};

// Описание страницы — единственный источник правды о рельсе: идентификатор,
// ключ в настройках и отчёте, ключ каталога строк, запасные подписи ru/en и
// горячая клавиша. Таблица объявлена ниже, kPageCount берётся из неё, поэтому
// «пять страниц» не может разъехаться с кодом, который по ней ходит.
struct PageDescriptor {
    PageId id;
    std::string_view key;       // "overview" — ключ в StateStore и в отчёте
    std::string_view titleKey;  // "nav.page.overview" — ключ каталога строк
    std::string_view titleRu;   // запасная подпись, если каталог не загружен
    std::string_view titleEn;
    std::string_view shortcut;  // "Alt+1" — подсказка и AcceleratorKey для UIA
};

inline constexpr std::array<PageDescriptor, 5> kPages{{
    {PageId::Overview, "overview", "nav.page.overview", "Обзор", "Overview", "Alt+1"},
    {PageId::Disks, "disks", "nav.page.disks", "Диски", "Disks", "Alt+2"},
    {PageId::Cleanup, "cleanup", "nav.page.cleanup", "Очистка", "Cleanup", "Alt+3"},
    {PageId::Report, "report", "nav.page.report", "Отчёт", "Report", "Alt+4"},
    {PageId::Settings, "settings", "nav.page.settings", "Настройки", "Settings", "Alt+5"},
}};

inline constexpr std::size_t kPageCount = kPages.size();

// Страница, открытая при старте. Онбординг (SPEC §4 FR-9, §8 Этап 5) начинает
// с обзора: он объясняет, что будет удалено, и предлагает безопасный профиль.
inline constexpr PageId kStartupPage = PageId::Overview;

// Базовый DPI для пересчёта DIP. Per-monitor v2 (§5) означает, что в реальном
// окне DPI всегда приходит от WM_DPICHANGED, а 96 — только точка отсчёта.
inline constexpr int kDefaultDpi = 96;

namespace detail {

// Страховка от тихой рассинхронизации: если в таблицу добавят пункт не в том
// порядке, компиляция падает здесь, а не через месяц в виде «рельс показывает
// не ту страницу».
constexpr bool pageTableMatchesEnum() {
    for (std::size_t i = 0; i < kPages.size(); ++i) {
        if (static_cast<std::size_t>(kPages[i].id) != i) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

static_assert(detail::pageTableMatchesEnum(), "kPages должен идти в порядке значений PageId");

// Описание страницы. Значение вне перечисления (битая настройка, неудачный
// static_cast) даёт первый пункт: вызывающий код обязан проверить isValidPage,
// но падать на этом модуле нечему.
const PageDescriptor& pageDescriptor(PageId page) noexcept;

bool isValidPage(PageId page) noexcept;                   // id < PageId::Count
std::size_t pageIndex(PageId page) noexcept;              // 0..kPageCount-1
std::optional<PageId> pageByIndex(std::size_t index) noexcept;
std::optional<PageId> pageByKey(std::string_view key) noexcept;  // регистр не важен

std::string_view pageKey(PageId page) noexcept;           // "cleanup"
std::string_view pageTitleKey(PageId page) noexcept;      // "nav.page.cleanup"
std::string_view pageTitleFallback(PageId page, core::Language lang) noexcept;

int pageOrdinal(PageId page) noexcept;                    // 1..5 — «Alt+N»
std::optional<PageId> pageByOrdinal(int ordinal) noexcept;
char pageAccessKey(PageId page) noexcept;                 // '1'..'5'
// Свёрка с кодом клавиши Win32: у цифр код совпадает с символом, поэтому
// windows.h в заголовке не нужен, а AppShell может звать прямо из WM_COMMAND.
bool pageMatchesAccessKey(PageId page, std::uint32_t virtualKey) noexcept;

// --- Рельс: метрики, геометрия, попадания ------------------------------------
//
// Сторона рельса — для RTL (§5 «RTL-ready»): ru и en остаются LTR, но ветка
// зеркалирования появляется здесь, а не в раскладке окна, иначе добавление
// арабского языка переписывало бы всё рисование.
enum class RailSide : std::uint8_t { Left, Right };

// RTL-язык — рельс справа. ru/en → Left.
RailSide defaultRailSide(core::Language lang) noexcept;

// Метрики в DIP (1/96 дюйма), а не в пикселях: мониторы с другим DPI и
// настройка масштаба меняют pixelsPerDip, но не размеры элементов.
struct RailMetrics {
    double itemHeightDip{48.0};      // высота пункта — цель нажатия не меньше
    double itemGapDip{4.0};          // зазор между пунктами
    double widthDip{224.0};          // ширина: подпись «Настройки» + отступы
    double paddingTopDip{12.0};      // отступ от верхней кромки окна
    double paddingBottomDip{12.0};
    double minimumHeightDip{160.0};  // ниже этого окно сжимать нельзя
};

struct RailRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    bool empty() const noexcept;
    // Правая и нижняя границы не включаются: соседние прямоугольники должны
    // делить окно без зазора и без двойного попадания.
    bool contains(int px, int py) const noexcept;
};

int dipToPx(double dip, int dpi) noexcept;   // округление к целому, dip <= 0 → 0
int dpiScalePercent(int dpi) noexcept;      // 96 → 100, 144 → 150

// Макет рельса: чистая арифметика без окна. Значение не владеет ресурсами и
// копируется по значению, поэтому его можно хранить в обработчике WM_PAINT и
// сравнивать в тестах.
class RailLayout {
public:
    RailLayout() = default;

    // scrollOffsetPx — положение прокрутки, пришедшее от владельца (Navigator);
    // макет только обрезает его в допустимые границы. Прокрутка хранится у
    // владельца намеренно: макет — производная величина, состояние — нет.
    static RailLayout compute(const RailMetrics& metrics, int dpi, int windowHeightPx,
                              RailSide side = RailSide::Left, int scrollOffsetPx = 0);

    RailSide side() const noexcept;
    int railWidthPx() const noexcept;
    int itemHeightPx() const noexcept;
    int itemStridePx() const noexcept;   // высота пункта + зазор
    int contentHeightPx() const noexcept;   // весь рельс целиком, без прокрутки
    int viewportHeightPx() const noexcept;  // высота окна (рельс во всю высоту)
    int minimumHeightPx() const noexcept;

    int scrollOffsetPx() const noexcept;    // применённый, уже обрезанный
    int maxScrollOffsetPx() const noexcept;
    bool scrollable() const noexcept;

    // Прямоугольник пункта с учётом прокрутки, в логических координатах рельса
    // (x отсчитывается от края рельса, а не окна).
    RailRect itemRect(PageId page) const noexcept;
    RailRect itemRectWithoutScroll(PageId page) const noexcept;
    // Перевод логических координат в клиентские окна: для RailSide::Right
    // рельс зеркалится относительно ширины окна.
    RailRect toClient(const RailRect& logical, int windowWidthPx) const noexcept;

    // Попадание мыши по логическим координатам рельса. Зазоры между пунктами и
    // отступы не принадлежат ни одному пункту — они дают nullopt, а не
    // «ближайший», иначе промах по краю незаметно переключает страницу.
    std::optional<PageId> hitTest(int x, int y) const noexcept;

    // Минимальная прокрутка, при которой пункт виден целиком (с отступом
    // margin). Текущая — если он уже на экране.
    int scrollOffsetToShow(PageId page, int marginPx = 0) const noexcept;

private:
    RailSide side_{RailSide::Left};
    int widthPx_{0};
    int itemHeightPx_{0};
    int gapPx_{0};
    int paddingTopPx_{0};
    int paddingBottomPx_{0};
    int minimumHeightPx_{0};
    int stridePx_{0};
    int contentHeightPx_{0};
    int viewportHeightPx_{0};
    int scrollOffsetPx_{0};
};

// --- Состояние страницы и его сохранение -------------------------------------

// Ключи в StateStore. Префиксы разведены намеренно: подпись страницы
// ("nav.page.overview") и её состояние ("nav.state.overview") живут в одном
// хранилище настроек, и склейка ключей означала бы, что перевод страницы
// перезаписывает прокрутку.
inline constexpr std::string_view kNavStateVersionKey = "nav.version";
inline constexpr std::string_view kNavCurrentPageKey = "nav.current";
inline constexpr std::string_view kNavRailScrollKey = "nav.rail.scroll";
inline constexpr std::string_view kNavPageStatePrefix = "nav.state.";

// Версия формата. Пока она 1, поля можно добавлять без миграции: читающий код
// обязан игнорировать незнакомые поля. Настоящая смена формата обязана
// поднять номер — иначе старая версия приложения примет новый файл за свой и
// потеряет состояние молча.
inline constexpr int kNavStateVersion = 1;

// Состояние одной страницы: то, что обязано пережить уход на другой экран и
// перезапуск. Экраны читают и пишут эти поля, ничего не изобретая.
struct PageState {
    int scrollOffsetDip{0};  // прокрутка содержимого страницы
    int splitterDip{0};      // положение разделителя; 0 — «по умолчанию»
    std::string selectedKey;  // выделенная строка/узел (ключ данных, не индекс)
    std::vector<std::string> expandedKeys;  // раскрытые ветки дерева
    std::string filter;  // строка фильтра, ввод пользователя

    bool operator==(const PageState& other) const;
    bool operator!=(const PageState& other) const;
};

// Текстовое представление состояния. Обратимое и устойчивое к мусору: `%`-
// экранирование разделителей (';', ',', '=', '%') внутри значений, версия в
// первом поле, неизвестные поля игнорируются, числа зажаты в разумные границы.
// Такой формат переживает правку файла настроек руками — а настройки в этом
// приложении читают и пишут люди.
std::string encodePageState(const PageState& state);
// nullopt — версия записи не наша (файл писала более новая сборка): состояние
// не трогаем, иначе пользователь потерял бы его при откате версии.
std::optional<PageState> decodePageState(std::string_view text);

using NavStateStore = std::map<std::string, std::string>;

// Данные одного пункта рельса для отрисовки и для UIA (SPEC §5 доступность).
// Строки — UTF-8, как принято в модели (core/model.hpp); в UTF-16 их
// переводит ui (DirectWrite, WM_SETTEXT).
struct RailItemView {
    PageId page;
    std::size_t index;
    std::string label;      // подпись из каталога или запасная
    std::string accessKey;  // "1".."5" — AcceleratorKey
    std::string shortcut;   // "Alt+1" — подсказка
    bool active;            // страница открыта
    bool selected;          // фокус рельса
};

// --- Навигатор ---------------------------------------------------------------

// Модель рельса и переходов. Поток — только UI (SPEC §6.1: единственный поток
// с сообщениями), поэтому класс намеренно не потокобезопасен и не имеет
// блокировок: mutex в пути отрисовки дороже самой отрисовки. Гонка с фоновым
// потоком закрыта на мосту (mv_bridge, задача W15/75): фон публикует
// неизменяемый снимок, а в UI-поток попадает через PostMessage.
class Navigator {
public:
    // Смена страницы: (откуда, куда). Старый == новому не бывает — вызывающий
    // узнаёт о смене ровно тогда, когда она произошла. Обработчик не должен
    // сам переключать страницу (он вызывается уже из перехода).
    using PageChangedHandler = std::function<void(PageId from, PageId to)>;
    // Подпись по ключу каталога строк (ui/locale, задача W14/69). Пустой ответ
    // означает «перевода нет» — берётся встроенная подпись.
    using TitleResolver = std::function<std::string(std::string_view key)>;
    // Правка состояния страницы на месте: dirty-флаг и revision обновляются
    // сами, в отличие от возвращаемой ссылки.
    using StateChange = std::function<void(PageState&)>;

    Navigator();

    PageId current() const noexcept;
    // Выделение в рельсе. Обычно совпадает с current(): страница открыта и
    // подсвечена. Расходится, когда фокус уехал на рельс при уже открытой
    // странице (клавиатура: стрелка вниз, затем Tab) — тогда фокус и содержимое
    // разные вещи, и экран должен знать, что он потерял фокус.
    PageId selection() const noexcept;

    // Меняется при каждом изменении, важном для отрисовки (страница, фокус,
    // язык). Оболочка запоминает значение и перерисовывает рельс, когда оно
    // разошлось, — это дешевле, чем раздавать события каждому экрану.
    std::uint64_t revision() const noexcept;

    core::Language language() const noexcept;
    // Смена языка без перезапуска (SPEC §5): подписи рельса берутся из
    // запасного варианта, поэтому нужен перерисовывающий revision.
    void setLanguage(core::Language lang);
    void setTitleResolver(TitleResolver resolver);

    // --- Переключение страниц ------------------------------------------------
    // false — перехода не было: страница уже открыта или значение неверное.
    bool goTo(PageId page);
    bool goToIndex(std::size_t index);
    bool goByOrdinal(int ordinal);  // Alt+1..Alt+5
    bool goToNext();               // без переноса: конец списка
    bool goToPrevious();           // без переноса: начало списка
    bool goToFirst();
    bool goToLast();

    // --- Клавиатура рельса (SPEC §5 «Клавиатурная навигация, фокус») ---------
    // Перенос по кругу, как в списке: с пятого пункта вверх — первый.
    void moveSelection(int delta);
    void setSelection(PageId page);
    // Enter/Space: открыть выделенное. false — выделенное уже открыто; экран
    // в этом случае решает сам, что делать (обычно просто вернуть фокус).
    bool activateSelection();
    // Alt+цифра: и выбор, и переход одним действием.
    bool selectByAccessKey(std::uint32_t virtualKey);

    // --- История «назад/вперёд» ---------------------------------------------
    // Нужна не «красивости», а отказу от действия: вернувшись на «Очистку»,
    // пользователь видит тот же план, который отменил. Глубина ограничена
    // kHistoryLimit, поэтому долгая сессия не растёт без границ.
    bool canGoBack() const noexcept;
    bool canGoForward() const noexcept;
    bool goBack();
    bool goForward();
    void clearHistory();

    // --- Состояние страницы --------------------------------------------------
    const PageState& state(PageId page) const noexcept;
    void setState(PageId page, PageState value);
    void patchState(PageId page, const StateChange& change);
    void resetState(PageId page);
    void resetAllStates();
    // Помечено ли состояние изменённым и не сохранённым: оболочка пишет
    // настройки при уходе со страницы и на выходе, а не на каждое движение
    // мыши по дереву.
    bool isStateDirty(PageId page) const noexcept;
    bool hasDirtyState() const noexcept;
    void markSaved();

    // --- Прокрутка рельса ----------------------------------------------------
    int railScrollPx() const noexcept;
    void setRailScrollPx(int px);
    int maxRailScrollPx(const RailMetrics& metrics, int dpi, int windowHeightPx) const;

    // --- Содержимое рельса ---------------------------------------------------
    std::string label(PageId page) const;
    RailItemView item(PageId page) const;
    std::vector<RailItemView> items() const;

    // --- Сохранение ----------------------------------------------------------
    // Экспорт дописывает свои ключи в переданное хранилище и ничего не стирает:
    // там же лежат настройки правил, и сброс настроек — это отдельное решение
    // (FR-9). Ключи страниц пишутся всегда, иначе «сброс к значениям по
    // умолчанию» тихо оставил бы старые фильтры.
    void exportState(NavStateStore& store) const;
    // Возвращает число применённых записей. problems (необязателен) получает
    // человекочитаемые причины, по которему запись не применена: их показывает
    // журнал, потому что «настройки не применились» без причины не чинится.
    std::size_t importState(const NavStateStore& store, std::vector<std::string>* problems = nullptr);

    // Полный сброс: текущая страница, состояния, история, прокрутка. Используется
    // настройками (FR-9 «сброс») и не трогает другие разделы настроек.
    void resetAll();

private:
    void markDirty(PageId page) noexcept;
    void bumpRevision() noexcept;
    void notifyPageChanged(PageId from, PageId to);
    PageState& stateFor(PageId page) noexcept;
    std::size_t indexFor(PageId page) const noexcept;

    PageId current_{kStartupPage};
    PageId selection_{kStartupPage};
    core::Language language_{core::kDefaultLanguage};
    std::uint64_t revision_{1};
    int railScrollPx_{0};
    std::array<PageState, kPageCount> states_{};
    std::array<bool, kPageCount> dirty_{};
    std::vector<PageId> backHistory_;
    std::vector<PageId> forwardHistory_;
    PageChangedHandler pageChanged_;
    TitleResolver titleResolver_;
};

}  // namespace mrproper::ui
