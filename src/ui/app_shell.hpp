// MrProper — каркас приложения: окно, регистрация классов, DPI, закрытие.
//
// Спека: §7 (ADR-3 — UI это Win32 с нативными контролами и Direct2D для
// графики собственной природы), §5 (DPI per-monitor v2, устойчивость «ни один
// отказ IOCTL/устройства/файла не роняет процесс», доступность, «всегда
// UAC-манифест»), §6.1 (слой ui — единственный поток с сообщениями, рендером и
// вводом), §6.4 (отмена кооперативная), §9.1 ADR-001 (предупреждения как
// ошибки, изоляция WinAPI).
//
// ---------------------------------------------------------------------------
// Что этот файл делает и почему он первый в слое ui
// ---------------------------------------------------------------------------
//
// Задача 66 — скелет, на который волны W14-W15 навешивают экраны. Поэтому здесь
// нет ни Direct2D (задача 67), ни темы (68), ни локализации (69), ни навигации
// (70): всё это приходит под уже работающее окно, уже с готовым DPI и уже с
// корректным закрытием. Обратный порядок не работает — окно, созданное без
// учёта DPI, потом приходится переделывать вместе со всеми контролами.
//
// Четыре шага жизненного цикла, и порядок между ними обязателен:
//
//   1) DPI-awareness  — ДО первого окна. Осознанность процесса выбирается один
//      раз и навсегда; сменить её после создания окна нельзя. Манифест
//      (packaging/app.manifest) уже объявляет PerMonitorV2, поэтому вызов API
//      здесь — вторая линия, а не единственная (см. enablePerMonitorV2Dpi).
//   2) Регистрация классов окон — до CreateWindowExW, иначе HWND не создастся.
//   3) Создание окна: главное + дочерний хост содержимого.
//   4) Цикл сообщений до WM_QUIT.
//
// Два окна, а не одно, потому что §7 требует смешанной вёрстки: собственная
// графика (карта разделов, диаграммы) рисуется на D2D, а таблицы — это
// SysListView32/SysTreeView32. Окно-контейнер даёт этим двум мирам общий
// клиентский прямоугольник и общий DPI, не давая им знать друг о друге.
//
// ---------------------------------------------------------------------------
// Чего здесь нет и кто это должен сделать
// ---------------------------------------------------------------------------
//
//   * Шрифты, цвета, переключение темы — задача 68 (theme.*): сейчас фон берётся
//     из GetSysColorBrush(COLOR_WINDOW), то есть следует системной теме, но не
//     перерисовывается по WM_SETTINGCHANGE.
//   * Тексты интерфейса из ресурсов — задача 69 (locale.*): строки здесь
//     литералы, и это осознанная заглушка.
//   * Меню, рельс и страницы — задача 70 (nav.*).
//   * Сохранение геометрии окна между запусками (реестр, FR-9) — задача 74:
//     каркас снимает WINDOWPLACEMENT в lastPlacement() и отдаёт его наверх.
//
// ---------------------------------------------------------------------------
// Правила этого файла
// ---------------------------------------------------------------------------
//
//   * Окно принадлежит UI-потоку. Никаких блокировок и никакого I/O в обработ-
//     чиках: фоновая работа приходит готовыми кадрами (SPEC §6.4).
//   * Исключение не пересекает границу Win32: статическая процедура окна
//     ловит всё и превращает в запись в журнал, иначе std::terminate внутри
//     пользовательского режима Windows не даст узнать, что случилось.
//   * Каждый неуспех WinAPI пишется в журнал с кодом ошибки (§5, §12).
#pragma once

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой UI, по SPEC §7 это законное место

#include <cstdint>
#include <string>

// NMHDR живёт в commctrl.h, а подключать его ради одного указателя в хуке
// событий незачем: здесь достаточно знать, что указатель передаётся на неизменность.
// Именно tagNMHDR, а не NMHDR: в commctrl.h NMHDR — это typedef, и объявление
// struct NMHDR завело бы второй, несовместимый тип с тем же именем (C2371).
struct tagNMHDR;

namespace mrproper::ui {

// 96 DPI — база масштаба. Windows считает 100 % ровно 96 DPI, и любое деление на
// это число в интерфейсе должно идти через это имя, а не через магическое
// число в трёх местах кода.
inline constexpr UINT kBaseDpi = 96;

// Имена классов окон. Класс главного окна и класс хоста содержимого разные:
// у них разные стили перерисовки, и общий класс означал бы, что дочернее окно
// наследует CS_DBLCLKS главного там, где это не нужно.
inline constexpr wchar_t kMainWindowClass[] = L"MrProper.MainWindow";
inline constexpr wchar_t kContentHostClass[] = L"MrProper.ContentHost";

// Заголовки по умолчанию. Локализация строк — задача 69; до неё это литералы.
inline constexpr wchar_t kDefaultWindowTitle[] = L"MrProper";
inline constexpr wchar_t kDefaultContentHostTitle[] = L"Область содержимого";

// Коды возврата runApp(). Различимы и в wWinMain, и в журнале: «окно не
// создалось» и «цикл сообщений отвалился» — разные диагностические ситуации.
namespace exitcode {
inline constexpr int ok = 0;                       ///< Окно закрыто штатно.
inline constexpr int noInstance = 2;               ///< wWinMain без HINSTANCE — запускать нечего.
inline constexpr int classRegistrationFailed = 3;  ///< Классы окон не зарегистрированы.
inline constexpr int windowCreationFailed = 4;     ///< HWND не создан.
inline constexpr int messageLoopFailed = 5;        ///< GetMessage вернул -1.
inline constexpr int unhandledException = 6;       ///< Исключение дошло до wWinMain.
}  // namespace exitcode

// Фактическая DPI-осознанность процесса. Отличать «настроили вызовом API» и
// «настроил манифест» нужно только для журнала, но различать per-monitor v1 и
// v2 — нужно для требования §5: v2 пересчитывает не только окно, но и всё
// содержимое при переносе на другой монитор, v1 этого не делает.
enum class DpiAwarenessMode : std::uint8_t {
    unknown,      ///< Определить не удалось (API недоступна) — считаем худшим случаем.
    unaware,      ///< Система масштабирует окно сама, содержимое будет мыльным.
    systemAware,  ///< Один масштаб на весь сеанс: на втором мониторе окно неверное.
    perMonitor,   ///< v1: окно переезжает между мониторами, содержимое — нет.
    perMonitorV2, ///< То, что требует SPEC §5.
};

// Стабильное имя режима для журнала: читается глазами в логе, а не переводится.
[[nodiscard]] const char* dpiAwarenessModeName(DpiAwarenessMode mode) noexcept;

// Текущий масштаб интерфейса. Windows не поддерживает анизотропный DPI, поэтому
// по горизонтали и вертикали значение одно; ось Y оставлена отдельным полем,
// чтобы вызывающий не сравнивал с «DPI окна» и не путал его с системным.
struct DpiScale {
    UINT x{kBaseDpi};
    UINT y{kBaseDpi};

    // Процент масштаба: 96 → 100, 144 → 150, 192 → 200. Целочисленный, потому
    // что раскладка и шрифты всё равно приводятся к целым пикселям.
    [[nodiscard]] unsigned percent() const noexcept {
        return static_cast<unsigned>((static_cast<unsigned>(x) * 100u) / kBaseDpi);
    }

    [[nodiscard]] bool operator==(const DpiScale&) const noexcept = default;
};

// Почему закрывают окно. Обработчик onCloseRequested обязан различать «пользователь
// нажал крестик» и «система гасит сеанс»: во втором случае отмена запроса
// возвращает FALSE в WM_QUERYENDSESSION, и это единственный способ не дать
// Windows завершить работу.
enum class CloseReason : std::uint8_t {
    user,          ///< WM_CLOSE: крестик, Alt+F4, пункт меню.
    system,        ///< WM_QUERYENDSESSION: выключение или перезагрузка.
    programmatic,  ///< Запрос из кода (requestClose).
};

// ---------------------------------------------------------------------------
// AppShell — окно приложения
// ---------------------------------------------------------------------------
//
// Экземпляр живёт на стеке вызывающего (обычно wWinMain) и переживает цикл
// сообщений. На кучу он не выносится сознательно: объект окна — это в первую
// очередь состояние между вызовами, а не разделяемый ресурс, и lifetime,
// совпадающий с lifetime цикла сообщений, снимает целый класс ошибок. Связь
// «окно → объект» идёт через GWLP_USERDATA, который заполняется в WM_NCCREATE и
// чистится в WM_NCDESTROY; delete this не вызывается нигде.
class AppShell {
public:
    struct Options {
        std::wstring mainClassName{kMainWindowClass};
        std::wstring contentClassName{kContentHostClass};
        std::wstring windowTitle{kDefaultWindowTitle};
        std::wstring contentHostTitle{kDefaultContentHostTitle};

        // 1120×760 — рабочая область ноутбука 1366×768 после панели задач и
        // заголовка; окно по умолчанию должно помещаться без maximize.
        int width{1120};
        int height{760};

        // Минимальный размер: ниже этого экраны §7.1 (карта дисков плюс дерево
        // плюс карточка деталей) перестают помещаться и начинают ломать вёрстку.
        int minWidth{900};
        int minHeight{600};

        // Центрировать на рабочей области основного монитора. По умолчанию да:
        // окно, появившееся в углу, читается как «приложение ещё не открыто».
        bool centerOnPrimary{true};

        // Показывать ли окно сразу после создания (false — создать скрытым,
        // показать позже из кода).
        bool showImmediately{true};

        // nCmdShow из wWinMain: -1 (SW_SHOWDEFAULT) означает «параметр запуска не
        // задал своё», и решение принимает WinMain по умолчанию.
        int showCommand{SW_SHOWDEFAULT};
    };

    explicit AppShell(Options options = {});
    ~AppShell();

    AppShell(const AppShell&) = delete;
    AppShell& operator=(const AppShell&) = delete;
    AppShell(AppShell&&) = delete;
    AppShell& operator=(AppShell&&) = delete;

    // --- Шаг 1: DPI-awareness ------------------------------------------------
    //
    // Статические: состояние процесса, а не объекта. Возвращаемый режим —
    // фактический, а не «получилось ли вызвать»: манифест мог настроить
    // осознанность раньше нас, и тогда ошибка вызова — это успех.
    [[nodiscard]] static DpiAwarenessMode enablePerMonitorV2DpiAwareness() noexcept;
    [[nodiscard]] static DpiAwarenessMode currentDpiAwareness() noexcept;

    // DPI окна (GetDpiForWindow) с откатом на системную, если окна ещё нет или
    // API вернула 0. Второй аргумент — DPI того монитора, где окно находится;
    // он и есть источник значения при Per-Monitor v2.
    [[nodiscard]] static DpiScale dpiFor(HWND window) noexcept;
    [[nodiscard]] static UINT dpiForSystem() noexcept;

    // --- Шаг 2 и 3: классы и окно -------------------------------------------
    [[nodiscard]] bool registerClasses() noexcept;
    [[nodiscard]] HWND createMainWindow() noexcept;

    // --- Шаг 4: цикл сообщений ----------------------------------------------
    [[nodiscard]] int pumpMessages() noexcept;

    // Все четыре шага по порядку. Код возврата — из namespace exitcode.
    [[nodiscard]] int run(HINSTANCE instance) noexcept;

    // --- Состояние ----------------------------------------------------------
    [[nodiscard]] HWND mainWindow() const noexcept { return mainWindow_; }
    [[nodiscard]] HWND contentHost() const noexcept { return contentHost_; }
    [[nodiscard]] HINSTANCE instance() const noexcept { return instance_; }
    [[nodiscard]] DpiScale dpi() const noexcept { return dpi_; }
    [[nodiscard]] DpiAwarenessMode awareness() const noexcept { return awareness_; }
    [[nodiscard]] bool closeRequested() const noexcept { return closeRequested_; }

    // Геометрия и состояние окна, снятые в момент закрытия. Их читает тот, кто
    // умеет сохранять настройки (FR-9, задача 74): каркас их только запоминает.
    [[nodiscard]] const WINDOWPLACEMENT& lastPlacement() const noexcept { return lastPlacement_; }

    // Закрытие из кода (пункт меню, сигнал от движка). Идёт тем же путём, что и
    // WM_CLOSE: обработчик onCloseRequested решает, можно ли закрываться.
    void requestClose() noexcept;

    // Положить окно в заданное состояние (развёрнуто/свёрнуто/восстановлено).
    // Отдельный метод, потому что WM_SIZE приходит и при сворачивании, и при
    // смене монитора, и в обоих случаях раскладку трогать нельзя.
    void restoreFrom(const WINDOWPLACEMENT& placement) noexcept;

protected:
    // Хуки для волн W14-W15. Сейчас пустые: их переопределит слой экранов.
    // Все вызываются в UI-потоке и внутри обработчика окна, поэтому здесь
    // запрещено блокировать. Бросать можно: исключение ловит статическая
    // процедура окна (windowProc) и записывает его в журнал. Работает это
    // потому, что путь «хук → обработчик → windowProc» намеренно не помечен
    // noexcept: помеченный noexcept обработчик превратил бы исключение из
    // экрана в std::terminate, и §5 («ни один отказ не роняет процесс») перестал
    // бы выполняться. Граница непроницаемости — публичный API каркаса.
    virtual void onCreate(HWND /*window*/) {}

    // Размер клиентской области в пикселях. При SIZE_MINIMIZED не приходит.
    virtual void onSize(SIZE /*size*/) {}

    // Смена DPI: suggested — прямоугольник, который система предлагает занять.
    // Он уже применён окном к этому моменту, переносить его повторно не нужно.
    virtual void onDpiChanged(const DpiScale& /*dpi*/, const RECT& /*suggested*/) {}

    // WM_COMMAND от меню, рельса и кнопок. controlId — идентификатор команды или
    // контрола, notificationCode — код уведомления (BN_CLICKED, EN_CHANGE, …),
    // caret — позиция мыши в экранных координатах (для команд мыши); у обычных
    // пунктов меню она нулевая. true — команда обработана, DefWindowProc звать
    // не нужно. Именно сюда экраны §7.1 вешают «Очистить сейчас», экспорт отчёта
    // и переключение страниц.
    virtual bool onCommand(HWND /*source*/, WORD /*controlId*/, WORD /*notificationCode*/, POINT /*caret*/) {
        return false;
    }

    // WM_NOTIFY от дочерних контролов. header указывает на NMHDR, за которым
    // следует конкретная структура уведомления (NMCUSTOMDRAW, NM_CLICK, …) —
    // разбирать её обязан вызывающий, каркас даёт только точный адрес. true —
    // обработано, код возврата 0. Таблицы §7 рисуются через NM_CUSTOMDRAW, и
    // без этого хука экран не сможет взять оформление у списка.
    virtual bool onNotify(HWND /*source*/, const tagNMHDR* /*header*/) { return false; }

    // WM_KEYDOWN в главном окне: Ctrl+Z для отмены (§7.2) и цифровые «горячие»
    // клавиши страниц. true — обработано. Необработанный ключ уходит дальше, в
    // DefWindowProc, поэтому ускорители меню и фокус в контролах не ломаются.
    virtual bool onKeyDown(HWND /*window*/, UINT /*virtualKey*/, LPARAM /*keyData*/) { return false; }

    // Разрешение закрытия. false отменяет закрытие пользователя (окно остаётся)
    // и возвращает FALSE в WM_QUERYENDSESSION (система не гасит сеанс).
    virtual bool onCloseRequested(CloseReason /*reason*/) { return true; }

    // Последний вызов перед PostQuitMessage: сброс ресурсов, которые живут дольше
    // окна. Исключения здесь ловятся и не мешают выходу из цикла.
    virtual void onShutdown() {}

private:
    // Обработчики, разложенные по сообщениям. Каждый — с одной темой, чтобы
    // switch оставался таблицей «сообщение → что делаем».
    //
    // Функции, на пути которых встречается виртуальный хук, помечены как
    // возвращающие значение, но НЕ noexcept: исключение из переопределения
    // должно дойти до try/catch в windowProc. Остальные — noexcept, они
    // вызывают хуков и бросить не могут.
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] LRESULT handleCreate(HWND window, const CREATESTRUCTW* create);
    void handleSize(HWND window, WPARAM sizeType);
    void handleGetMinMaxInfo(MINMAXINFO* info) noexcept;
    void handleDpiChanged(HWND window, UINT dpiX, UINT dpiY, const RECT* suggested);
    void handleClose(HWND window);
    [[nodiscard]] LRESULT handleQueryEndSession(HWND window);
    void handleEndSession(HWND window, bool sessionEnding) noexcept;
    void handleDestroy(HWND window);
    [[nodiscard]] LRESULT handleCommand(HWND window, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] LRESULT handleNotify(HWND window, LPARAM lParam);
    [[nodiscard]] LRESULT handleKeyDown(HWND window, WPARAM wParam, LPARAM lParam);
    void handleNcDestroy(HWND window) noexcept;
    void paintContentHost(HWND window) noexcept;

    // Служебное.
    void layoutContentHost() noexcept;
    void rememberPlacement(HWND window) noexcept;
    void centerOnPrimary(int width, int height, int& x, int& y) const noexcept;
    [[nodiscard]] bool isMainWindow(HWND window) const noexcept;
    [[nodiscard]] bool registerClass(const WNDCLASSEXW* windowClass) noexcept;
    [[nodiscard]] static AppShell* fromWindow(HWND window) noexcept;
    [[nodiscard]] static DpiAwarenessMode fromAwareness(DPI_AWARENESS awareness) noexcept;

    Options options_;
    HINSTANCE instance_{nullptr};
    HWND mainWindow_{nullptr};
    HWND contentHost_{nullptr};
    DpiScale dpi_{};
    DpiAwarenessMode awareness_{DpiAwarenessMode::unknown};
    WINDOWPLACEMENT lastPlacement_{};
    bool closeRequested_{false};
    bool classesRegistered_{false};
};

// Тело wWinMain: DPI → COM → общие контролы → окно → цикл → выход.
// Отдельная свободная функция, потому что точка входа одна на приложение, а
// класс занимается окном, а не инициализацией процесса. Исключения наружу не
// выходят (§5, §12): std::terminate из wWinMain неотличим от краша.
[[nodiscard]] int runApp(HINSTANCE instance, int showCommand) noexcept;

}  // namespace mrproper::ui
