// Реализация каркаса приложения: окно, классы окон, DPI, закрытие.
// Спека и разбор решений — в app_shell.hpp; здесь только код.

#include "app_shell.hpp"

#include <commctrl.h>
#include <objbase.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM: позиция мыши в WM_COMMAND

#include <cstddef>
#include <exception>
#include <string_view>
#include <utility>

#include "core/log.hpp"

namespace mrproper::ui {
namespace {

// Стиль главного окна. WS_CLIPCHILDREN обязателен: окно непрозрачно, и без этой
// строки система шлёт WM_PAINT в главное окно при каждой перерисовке
// содержимого — то есть на каждое движение мыши внутри списка.
constexpr DWORD kMainWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

// Хост содержимого: ребёнок главного окна во весь клиентский прямоугольник.
constexpr DWORD kContentHostStyle = WS_CHILD | WS_VISIBLE;

// Перестановка окон и активация при перестановке содержимого запрещены: раскладку
// хоста и окна, получившего новый DPI, никто не должен перехватывать, а фокус
// ввода обязан остаться там, где его оставил пользователь.
constexpr UINT kFlagsNoZOrderNoActivate = SWP_NOZORDER | SWP_NOACTIVATE;

// Фон из системной палитры: он уже следует системной теме (§5 «Тема»), и до
// появления theme.* (§7, задача 68) это единственное, что нужно окну, чтобы не
// мигать белым при перерисовке.
[[nodiscard]] HBRUSH systemWindowBrush() noexcept {
    HBRUSH brush = ::GetSysColorBrush(COLOR_WINDOW);
    if (brush == nullptr) {
        // Редкий, но реальный случай: неполная системная схема не дала кисть.
        // COLOR_BTNFACE тоже системный и тоже переживает переключение
        // светлая/тёмная, поэтому чуть другой оттенок лучше, чем hbrBackground =
        // nullptr: без него окно не стирает фон и мерцает при каждой перерисовке.
        brush = ::GetSysColorBrush(COLOR_BTNFACE);
    }
    return brush;
}

// --- Запись в журнал --------------------------------------------------------
//
// Макросы MRP_LOG_* из core/log.hpp здесь непригодны: detail::logFieldList
// (core/log.hpp, строка с initializer_list) раскрывает пакет в вызов logField по
// одному аргументу, поэтому любое поле даёт C2661 «нет перегрузки logField,
// принимающей 1 аргумент»: макрос компилируется только совсем без полей.
// Дефект в чужом файле (владелец core/log.hpp — волна W04-W06), править его из
// слоя ui нельзя, поэтому поля собираются здесь явно — тем же способом, который
// предлагает сам комментарий над макросами в core/log.hpp («без макроса остаётся
// вызов с явно собранным списком полей»). Нечётное число аргументов даёт ошибку
// компиляции, а не молча потерянное значение — так же, как static_assert в
// logFieldList.
void collectFields(core::LogFields&) noexcept {
    // Пустой хвост: полей нет. Отдельная перегрузка, а не default-аргумент,
    // чтобы рекурсия останавливалась без std::initializer_list. Параметр без
    // имени: /W4 ругается C4100 на неиспользуемый формальный параметр, а /WX
    // превращает это в ошибку сборки.
}

template <typename Key, typename Value, typename... Rest>
void collectFields(core::LogFields& out, Key&& key, Value&& value, Rest&&... rest) {
    out.push_back(core::logField(std::forward<Key>(key), std::forward<Value>(value)));
    collectFields(out, std::forward<Rest>(rest)...);
}

// Сообщение — string_view, а не const char*: половина вызовов в приложении
// собирает текст через core::units и передаёт std::string, и const char* здесь
// дал бы C2440 ровно в тех местах, где исключение уже поймано и текст готов.
template <typename... Args>
void logEvent(core::LogLevel level, const char* event, std::string_view message, Args&&... pairs) {
    core::LogFields fields;
    collectFields(fields, std::forward<Args>(pairs)...);
    core::Logger::instance().write(level, event, message, std::move(fields));
}

}  // namespace

// ---------------------------------------------------------------------------
// Режим DPI-осознанности
// ---------------------------------------------------------------------------

const char* dpiAwarenessModeName(DpiAwarenessMode mode) noexcept {
    switch (mode) {
    case DpiAwarenessMode::unknown:      return "unknown";
    case DpiAwarenessMode::unaware:      return "unaware";
    case DpiAwarenessMode::systemAware:  return "system-aware";
    case DpiAwarenessMode::perMonitor:   return "per-monitor-v1";
    case DpiAwarenessMode::perMonitorV2: return "per-monitor-v2";
    }
    return "unknown";
}

DpiAwarenessMode AppShell::fromAwareness(DPI_AWARENESS awareness) noexcept {
    switch (awareness) {
    case DPI_AWARENESS_UNAWARE:           return DpiAwarenessMode::unaware;
    case DPI_AWARENESS_SYSTEM_AWARE:      return DpiAwarenessMode::systemAware;
    case DPI_AWARENESS_PER_MONITOR_AWARE: return DpiAwarenessMode::perMonitor;
    case DPI_AWARENESS_INVALID:           break;
    }
    return DpiAwarenessMode::unknown;
}

DpiAwarenessMode AppShell::currentDpiAwareness() noexcept {
    // Контекст осознанности берётся у потока: он всегда есть, даже если
    // процесс не настраивал ничего, — в этом случае GetThreadDpiAwarenessContext
    // вернёт DPI_AWARENESS_CONTEXT_UNAWARE, а не ошибку. nullptr означает, что
    // API недоступна (сборка против старого SDK в CI), и тогда результат
    // неизвестен, что честнее, чем выдуманное «v2».
    const DPI_AWARENESS_CONTEXT context = ::GetThreadDpiAwarenessContext();
    if (context == nullptr) return DpiAwarenessMode::unknown;
    return fromAwareness(::GetAwarenessFromDpiAwarenessContext(context));
}

DpiAwarenessMode AppShell::enablePerMonitorV2DpiAwareness() noexcept {
    // Главное здесь — не путать отказ с ошибкой. Осознанность процесса
    // выбирается один раз, и манифест (packaging/app.manifest, §5 «DPI») делает
    // это до первой строки кода: тогда SetProcessDpiAwarenessContext вернёт
    // ERROR_ACCESS_DENIED, и это означает «уже настроено, не трогай», а не
    // «не получилось». Решение принимается по фактическому awareness, который
    // читается отдельным вызовом.
    const BOOL applied = ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD error = applied != FALSE ? ERROR_SUCCESS : ::GetLastError();

    DpiAwarenessMode mode = currentDpiAwareness();
    if (mode != DpiAwarenessMode::perMonitorV2) {
        // Лестница отката. v1 лучше, чем system-aware: окно хотя бы переезжает
        // между мониторами целиком, пусть и без пересчёта содержимого.
        if (mode == DpiAwarenessMode::unknown || mode == DpiAwarenessMode::unaware ||
            mode == DpiAwarenessMode::systemAware) {
            (void)::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
            mode = currentDpiAwareness();
        }
        if (mode != DpiAwarenessMode::perMonitor && mode != DpiAwarenessMode::perMonitorV2) {
            // Последний шаг — хотя бы не мыль: системная осознанность с
            // масштабированием средствами Windows лучше, чем полное отсутствие
            // осознанности на ноутбуке с 150 %.
            (void)::SetProcessDPIAware();
            mode = currentDpiAwareness();
        }
    }

    switch (mode) {
    case DpiAwarenessMode::perMonitorV2:
        logEvent(core::LogLevel::Info, "ui.dpi.awareness", "процесс работает в режиме per-monitor v2", "source",
            applied != FALSE ? "api" : "manifest");
        break;
    case DpiAwarenessMode::perMonitor:
        logEvent(core::LogLevel::Warn, "ui.dpi.awareness",
            "включился только per-monitor v1: содержимое не пересчитается при переносе окна", "hr", error);
        break;
    case DpiAwarenessMode::systemAware:
    case DpiAwarenessMode::unaware:
    case DpiAwarenessMode::unknown:
        // Не фатально: приложение запустится, но на мультимониторной сборке
        // окно будет выглядеть неверно. Ронять из-за этого процесс нельзя —
        // §5 требует, чтобы ни один отказ среды не мешал работе утилиты.
        logEvent(core::LogLevel::Warn, "ui.dpi.awareness", "DPI-осознанность ниже требуемой (SPEC §5)", "mode",
            dpiAwarenessModeName(mode));
        break;
    }
    return mode;
}

UINT AppShell::dpiForSystem() noexcept {
    const UINT dpi = ::GetDpiForSystem();
    return dpi == 0 ? kBaseDpi : dpi;
}

DpiScale AppShell::dpiFor(HWND window) noexcept {
    if (window != nullptr) {
        // GetDpiForWindow отдаёт один коэффициент на обе оси: анизотропного DPI
        // в Windows не существует, и «96 по X и 144 по Y» — невозможное окно.
        const UINT dpi = ::GetDpiForWindow(window);
        if (dpi != 0) return DpiScale{dpi, dpi};
    }
    const UINT system = dpiForSystem();
    return DpiScale{system, system};
}

// ---------------------------------------------------------------------------
// AppShell
// ---------------------------------------------------------------------------

AppShell::AppShell(Options options) : options_(std::move(options)) {
    dpi_ = DpiScale{dpiForSystem(), dpiForSystem()};
}

AppShell::~AppShell() {
    // Классы окон намеренно не выгружаются UnregisterClass: после выхода из
    // цикла сообщений процесс заканчивается, а выгрузка класса заняла бы
    // единственное место, где можно ошибиться, — после WM_DESTROY, когда
    // часть состояния уже недоступна. Пока цикл работает, вызывающий
    // использует requestClose(), и никаких «висящих» окон не остаётся.
    if (mainWindow_ != nullptr) {
        ::DestroyWindow(mainWindow_);
    }
}

bool AppShell::registerClass(const WNDCLASSEXW* windowClass) noexcept {
    if (::RegisterClassExW(windowClass) != 0) return true;
    const DWORD error = ::GetLastError();
    if (error == ERROR_CLASS_ALREADY_EXISTS) {
        // Так бывает при повторной регистрации в одном процессе (например, если
        // окно пересоздают после смены темы). Класс уже описан, использовать его
        // безопасно — и это не ошибка.
        logEvent(core::LogLevel::Info,
            "ui.window.class_exists",
            "класс окна уже зарегистрирован, используем существующий",
            "class",
            windowClass->lpszClassName);
        return true;
    }
    logEvent(core::LogLevel::Error,
        "ui.window.class_registration_failed",
        "не удалось зарегистрировать класс окна",
        "hr",
        error);
    return false;
}

bool AppShell::registerClasses() noexcept {
    if (classesRegistered_) return true;
    if (instance_ == nullptr) {
        logEvent(core::LogLevel::Error,
            "ui.window.classes.no_instance",
            "регистрация классов без HINSTANCE невозможна");
        return false;
    }

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    // CS_DBLCLKS у главного окна: двойной клик по заголовку разворачивает окно,
    // и это поведение ожидают по умолчанию. CS_HREDRAW|CS_VREDRAW заставляют
    // перерисовывать всё при изменении размеров — дешевле, чем разбираться с
    // остатками содержимого при первой же перетаскиваемой рамке.
    mainClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    mainClass.lpfnWndProc = &AppShell::windowProc;
    mainClass.hInstance = instance_;
    mainClass.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hIconSm = mainClass.hIcon;
    mainClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = systemWindowBrush();
    mainClass.lpszClassName = options_.mainClassName.c_str();
    if (!registerClass(&mainClass)) return false;

    WNDCLASSEXW contentClass{};
    contentClass.cbSize = sizeof(contentClass);
    contentClass.style = CS_HREDRAW | CS_VREDRAW;
    contentClass.lpfnWndProc = &AppShell::windowProc;
    contentClass.hInstance = instance_;
    // Иконки у дочернего окна нет: она не появляется в панели задач и в
    // переключателе Alt+Tab, а пустое поле означало бы лишний запрос к USER32.
    contentClass.hIcon = nullptr;
    contentClass.hIconSm = nullptr;
    contentClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    contentClass.hbrBackground = systemWindowBrush();
    contentClass.lpszClassName = options_.contentClassName.c_str();
    if (!registerClass(&contentClass)) return false;

    classesRegistered_ = true;
    logEvent(core::LogLevel::Info, "ui.window.classes_registered", "классы окон зарегистрированы", "count", 2);
    return true;
}

void AppShell::centerOnPrimary(int width, int height, int& x, int& y) const noexcept {
    // rcWork, а не rcMonitor: монитор может быть частично за пределами экрана,
    // и центрирование по нему увело бы окно за край. Плюс на панели задач
    // центрировать нельзя — заголовок уезжает под неё.
    const HMONITOR monitor = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == 0) return;  // остаётся CW_USEDEFAULT
    const RECT& work = info.rcWork;
    x = work.left + ((work.right - work.left) - width) / 2;
    y = work.top + ((work.bottom - work.top) - height) / 2;
}

HWND AppShell::createMainWindow() noexcept {
    if (mainWindow_ != nullptr) return mainWindow_;
    if (instance_ == nullptr) {
        logEvent(core::LogLevel::Error, "ui.window.create.no_instance", "создание окна без HINSTANCE невозможно");
        return nullptr;
    }
    if (!classesRegistered_ && !registerClasses()) return nullptr;

    const UINT dpi = dpiForSystem();
    RECT frame{0, 0, options_.width, options_.height};
    // AdjustWindowRectExForDpi учитывает метрики кнопок и рамки именно этого
    // DPI. Без него окно «1120×760» на мониторе 150 % получает рамку от
    // 96 DPI, и клиентская область меньше задуманной примерно на треть.
    if (::AdjustWindowRectExForDpi(&frame, kMainWindowStyle, FALSE, 0, dpi) == 0) {
        // Старый путь на случай, если API недоступна: рамка будет посчитана для
        // системного DPI, но окно всё равно появится.
        (void)::AdjustWindowRectEx(&frame, kMainWindowStyle, FALSE, 0);
        logEvent(core::LogLevel::Warn,
            "ui.window.adjust_rect_fallback",
            "AdjustWindowRectExForDpi недоступен, рамка посчитана вслепую");
    }
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    int x = static_cast<int>(CW_USEDEFAULT);
    int y = static_cast<int>(CW_USEDEFAULT);
    if (options_.centerOnPrimary) centerOnPrimary(width, height, x, y);

    // this уходит в CREATESTRUCT::lpCreateParams и оттуда в GWLP_USERDATA — так
    // объект переживает любые сообщения, включая те, что приходят до WM_CREATE.
    const HWND window = ::CreateWindowExW(0, options_.mainClassName.c_str(), options_.windowTitle.c_str(),
                                          kMainWindowStyle, x, y, width, height, nullptr, nullptr, instance_, this);
    if (window == nullptr) {
        logEvent(core::LogLevel::Error,
            "ui.window.creation_failed",
            "не удалось создать главное окно",
            "hr",
            ::GetLastError());
        return nullptr;
    }

    if (options_.showImmediately) {
        // Возвращаемое значение ShowWindow — предыдущее состояние видимости, а не
        // «успех»: FALSE на первом показе это норма, поэтому проверять нечего.
        ::ShowWindow(window, options_.showCommand);
    }
    ::UpdateWindow(window);
    logEvent(core::LogLevel::Info, "ui.window.created", "главное окно создано", "dpi", dpi);
    return window;
}

int AppShell::pumpMessages() noexcept {
    MSG message{};
    for (;;) {
        const BOOL received = ::GetMessageW(&message, nullptr, 0, 0);
        if (received == 0) {
            // WM_QUIT: wParam — код возврата, который выложил PostQuitMessage.
            return static_cast<int>(message.wParam);
        }
        if (received == -1) {
            // -1 означает ошибку (нет сообщения). GetLastError даст причину,
            // но цикл всё равно надо закрыть: иначе окно останется на экране без
            // цикла, то есть приложение перестанет отвечать.
            logEvent(core::LogLevel::Error,
                "ui.message_loop.failed",
                "GetMessage вернул ошибку",
                "hr",
                ::GetLastError());
            return exitcode::messageLoopFailed;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

int AppShell::run(HINSTANCE instance) noexcept {
    if (instance == nullptr) {
        logEvent(core::LogLevel::Error, "ui.shell.no_instance", "wWinMain вызван без HINSTANCE");
        return exitcode::noInstance;
    }
    instance_ = instance;

    // Шаг 1 до шага 2: осознанность выбирается до первого окна, иначе система
    // успеет применить системное масштабирование и пересчёт будет неверным.
    awareness_ = enablePerMonitorV2DpiAwareness();

    if (!registerClasses()) return exitcode::classRegistrationFailed;
    if (createMainWindow() == nullptr) return exitcode::windowCreationFailed;
    return pumpMessages();
}

void AppShell::requestClose() noexcept {
    if (mainWindow_ == nullptr) return;
    // PostMessage, а не SendMessage: закрытие может запросить фоновый поток
    // движка, а SendMessage из него блокировался бы до обработки в UI-потоке —
    // и если UI-поток ждёт этот поток, получится взаимоблокировка. Пользовательский
    // путь (WM_CLOSE) при этом не меняется: он и так приходит из UI-потока, а
    // обработчик onCloseRequested решает в обоих случаях одно и то же.
    (void)::PostMessageW(mainWindow_, WM_CLOSE, 0, 0);
}

void AppShell::restoreFrom(const WINDOWPLACEMENT& placement) noexcept {
    if (mainWindow_ == nullptr) return;
    if (placement.length != sizeof(WINDOWPLACEMENT)) {
        logEvent(core::LogLevel::Warn,
            "ui.window.placement_invalid",
            "размер WINDOWPLACEMENT не совпал, положение окна не применено");
        return;
    }
    // Положение восстанавливается до показа окна, поэтому showWindowCmd из
    // структуры уважается, а не переопределяется нашим ShowWindow.
    if (::SetWindowPlacement(mainWindow_, &placement) == FALSE) {
        logEvent(core::LogLevel::Warn,
            "ui.window.placement_failed",
            "SetWindowPlacement не принял положение окна",
            "hr",
            ::GetLastError());
    }
}

void AppShell::rememberPlacement(HWND window) noexcept {
    lastPlacement_ = WINDOWPLACEMENT{};
    lastPlacement_.length = sizeof(WINDOWPLACEMENT);
    if (::GetWindowPlacement(window, &lastPlacement_) == FALSE) {
        // Не сбрасываем length: структура остаётся в согласованном виде, просто
        // без данных, и читающий её код увидит нули, а не мусор из стека.
        lastPlacement_ = WINDOWPLACEMENT{};
        logEvent(core::LogLevel::Warn,
            "ui.window.placement_unavailable",
            "не удалось снять положение окна",
            "hr",
            ::GetLastError());
    }
}

// ---------------------------------------------------------------------------
// Обработка сообщений
// ---------------------------------------------------------------------------

AppShell* AppShell::fromWindow(HWND window) noexcept {
    return reinterpret_cast<AppShell*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

bool AppShell::isMainWindow(HWND window) const noexcept {
    return window == mainWindow_;
}

LRESULT CALLBACK AppShell::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Граница между кодом C++ и кодом Windows. Исключение, дошедшее сюда,
    // std::terminate в неизвестной точке; перехват даёт запись в журнал вместо
    // молчаливого падения (§5, §12).
    try {
        if (message == WM_NCCREATE) {
            // Единственное окно, где this ещё доступен: он приходит в
            // CREATESTRUCT::lpCreateParams, потому что окно создано с this
            // в lpParam. Всё остальное читает GWLP_USERDATA.
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            if (create != nullptr) {
                ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            }
        }
        AppShell* shell = fromWindow(window);
        if (shell != nullptr) return shell->handleMessage(window, message, wParam, lParam);
        return ::DefWindowProcW(window, message, wParam, lParam);
    } catch (const std::exception& error) {
        logEvent(core::LogLevel::Error,
            "ui.window.handler_threw",
            std::string("обработчик сообщения бросил исключение: ") + error.what());
        return 0;
    } catch (...) {
        logEvent(core::LogLevel::Error,
            "ui.window.handler_threw",
            "обработчик сообщения бросил неизвестное исключение");
        return 0;
    }
}

LRESULT AppShell::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE:
        return ::DefWindowProcW(window, message, wParam, lParam);

    case WM_CREATE:
        return handleCreate(window, reinterpret_cast<const CREATESTRUCTW*>(lParam));

    case WM_SIZE:
        handleSize(window, wParam);
        return 0;

    case WM_GETMINMAXINFO:
        handleGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(lParam));
        return 0;

    case WM_DPICHANGED:
        // wParam несёт новый DPI: LOWORD по X, HIWORD по Y. lParam — RECT,
        // предложенный системой под новый масштаб; применяем ровно его, потому
        // что «посчитать окно заново» здесь означает перечеркнуть решение
        // системы о положении относительно других окон.
        handleDpiChanged(window, static_cast<UINT>(LOWORD(wParam)), static_cast<UINT>(HIWORD(wParam)),
                         reinterpret_cast<const RECT*>(lParam));
        return 0;

    case WM_DPICHANGED_AFTERPARENT:
        // v2 шлёт дочерним окнам BEFOREPARENT/AFTERPARENT вместо WM_DPICHANGED.
        // Блок AFTERPARENT — единственное, что нужно хосту содержимого: система
        // к этому моменту уже пересчитала его размеры, осталось обновить кэш и
        // перерисовать.
        handleDpiChanged(window, static_cast<UINT>(LOWORD(wParam)), static_cast<UINT>(HIWORD(wParam)), nullptr);
        return 0;

    case WM_DPICHANGED_BEFOREPARENT:
        // Ничего делать нельзя: размеры окна сейчас меняет система.
        return 0;

    case WM_DISPLAYCHANGE:
        // Смена топологии мониторов: у наведённого окна мог смениться DPI.
        // Сообщение получают и дочерние окна, а читать DPI и писать в журнал
        // нужно один раз — по главному окну.
        if (isMainWindow(window)) {
            dpi_ = dpiFor(window);
            ::InvalidateRect(window, nullptr, FALSE);
            logEvent(core::LogLevel::Info, "ui.dpi.display_changed", "сменилась топология мониторов", "dpi", dpi_.x);
        }
        return 0;

    case WM_COMMAND:
        // Пункты меню, кнопки и рельс приходят сюда: маршрутизация в хук, чтобы
        // экран не писал вторую таблицу сообщений. Главное окно — единственное,
        // которое интересуется командами; хосту содержимого они не нужны.
        if (!isMainWindow(window)) break;
        return handleCommand(window, wParam, lParam);

    case WM_NOTIFY:
        // NM_CUSTOMDRAW таблиц §7 приходит сюда, а не в хук самого контрола.
        if (!isMainWindow(window)) break;
        return handleNotify(window, lParam);

    case WM_KEYDOWN:
        // Ctrl+Z (§7.2) и цифровые переходы между страницами.
        if (!isMainWindow(window)) break;
        return handleKeyDown(window, wParam, lParam);

    case WM_CLOSE:
        handleClose(window);
        return 0;

    case WM_QUERYENDSESSION:
        if (!isMainWindow(window)) return ::DefWindowProcW(window, message, wParam, lParam);
        return handleQueryEndSession(window);

    case WM_ENDSESSION:
        if (!isMainWindow(window)) return ::DefWindowProcW(window, message, wParam, lParam);
        handleEndSession(window, wParam != FALSE);
        return 0;

    case WM_DESTROY:
        handleDestroy(window);
        return 0;

    case WM_NCDESTROY:
        handleNcDestroy(window);
        return ::DefWindowProcW(window, message, wParam, lParam);

    case WM_ERASEBKGND:
        // Хост содержимого закрашивается сам в WM_PAINT: когда в нём появится
        // D2D-поверхность (задача 67), двойная закраска исчезла бы сама.
        if (!isMainWindow(window)) return 1;
        break;

    case WM_PAINT:
        if (!isMainWindow(window)) {
            paintContentHost(window);
            return 0;
        }
        break;

    default:
        break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT AppShell::handleCreate(HWND window, const CREATESTRUCTW* create) {
    if (create == nullptr || create->lpCreateParams == nullptr) {
        // Окно создано кем-то другим и без нас — продолжать незачем.
        logEvent(core::LogLevel::Error,
            "ui.window.create_params_missing",
            "у окна нет lpCreateParams: это не каркас приложения");
        return -1;  // отказ создать окно
    }

    mainWindow_ = window;
    // Именно GetDpiForWindow, а не dpiForSystem(): окно может появиться на
    // втором мониторе (например, при восстановлении положения), и системный DPI
    // тогда не имеет отношения к его содержимому.
    dpi_ = dpiFor(window);

    contentHost_ = ::CreateWindowExW(0, options_.contentClassName.c_str(), options_.contentHostTitle.c_str(),
                                     kContentHostStyle, 0, 0, 0, 0, window, nullptr, instance_, this);
    if (contentHost_ == nullptr) {
        // Не фатально: окно без содержимого всё равно работает (заголовок,
        // рамка, закрытие), и падать из-за пустого хостa при запуске — ровно тот
        // отказ среды, который §5 запрещает превращать в краш.
        logEvent(core::LogLevel::Error, "ui.content_host.creation_failed", "не удалось создать хост содержимого", "hr",
            ::GetLastError());
    } else {
        layoutContentHost();
    }

    // Текст окна (заголовок и имя хоста содержимого) доступен экранному диктору
    // и UI Automation (§5 «Доступность»): без него окно озвучивается как «пустое».
    logEvent(core::LogLevel::Info,
        "ui.window.create_completed",
        "клиентская область и хост содержимого готовы",
        "dpi",
        dpi_.x,
        "contentHost",
        contentHost_ != nullptr);
    onCreate(window);
    return 0;
}

void AppShell::handleSize(HWND window, WPARAM sizeType) {
    if (sizeType == SIZE_MINIMIZED) {
        // В свёрнутом виде клиентский прямоугольник бессмысленен, а SetWindowPos
        // в ноль размеров ещё и разбудит перерисовку при каждом сворачивании.
        return;
    }
    // Раскладку хоста двигает только главное окно. WM_SIZE приходит и хосту
    // содержимого, и его ответный SetWindowPos был бы вызовом вхолостую.
    if (isMainWindow(window)) {
        layoutContentHost();
    }

    RECT client{};
    if (::GetClientRect(window, &client) != 0) {
        const SIZE size{client.right - client.left, client.bottom - client.top};
        onSize(size);
    }
}

void AppShell::handleGetMinMaxInfo(MINMAXINFO* info) noexcept {
    if (info == nullptr) return;
    // ptMinTrackSize задаётся в ФИЗИЧЕСКИХ пикселях монитора, и при
    // Per-Monitor V2 система его не пересчитывает: заявленные 900×600 на
    // мониторе 150 % означали бы 600×400 логических пикселей — ровно тот размер,
    // при котором вёрстка §7.1 (карта разделов плюс дерево плюс карточка
    // деталей) ломается. Поэтому минимум переводится в физические пиксели
    // текущего масштаба, а dpi_ к этому моменту уже новый: WM_DPICHANGED
    // обновляет его до SetWindowPos, а тот уже вызывает это сообщение.
    const auto scale = [](int logical, UINT dpi) -> LONG {
        const long long scaled =
            (static_cast<long long>(logical) * static_cast<long long>(dpi) + (kBaseDpi / 2)) / kBaseDpi;
        // Окно не может быть меньше рамки: отрицательный или нулевой размер
        // минимальной области система трактует как «не сжимать», а минус —
        // как некорректное значение.
        return scaled > 0 ? static_cast<LONG>(scaled) : 1L;
    };
    info->ptMinTrackSize.x = scale(options_.minWidth, dpi_.x);
    info->ptMinTrackSize.y = scale(options_.minHeight, dpi_.y);
}

void AppShell::handleDpiChanged(HWND window, UINT dpiX, UINT dpiY, const RECT* suggested) {
    if (dpiX == 0 || dpiY == 0) {
        // Пустой wParam приходит от вложенных окон в некоторых сценариях
        // (родитель не сообщил DPI). Молчаливый 96 здесь означил бы скачок
        // содержимого, поэтому берём DPI, который система всё же посчитала.
        const DpiScale actual = dpiFor(window);
        dpiX = actual.x;
        dpiY = actual.y;
    }
    const DpiScale next{dpiX, dpiY};
    const bool changed = !(next == dpi_);
    dpi_ = next;

    if (isMainWindow(window) && suggested != nullptr && suggested->right > suggested->left &&
        suggested->bottom > suggested->top) {
        // Ровно предложенный прямоугольник и ровно эти флаги: без SWP_NOZORDER
        // окно подскочило бы поверх других, а с SWP_FRAMECHANGED рамка
        // перерисовалась бы до того, как система её обновит.
        (void)::SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, kFlagsNoZOrderNoActivate);
    }

    if (contentHost_ != nullptr) {
        ::InvalidateRect(contentHost_, nullptr, FALSE);
    }
    ::InvalidateRect(window, nullptr, FALSE);

    logEvent(core::LogLevel::Info, "ui.dpi.changed", "масштаб интерфейса изменился", "dpi", dpiX);
    if (changed) {
        // suggested для дочернего окна нет — подставляем текущий прямоугольник,
        // чтобы обработчик всегда получал одинаковый вид аргумента.
        RECT fallback{};
        if (suggested == nullptr) (void)::GetWindowRect(window, &fallback);
        const RECT& effective = suggested != nullptr ? *suggested : fallback;
        onDpiChanged(dpi_, effective);
    }
}

void AppShell::handleClose(HWND window) {
    if (!onCloseRequested(CloseReason::user)) {
        // Закрытие отменено: окно обязано остаться на экране, поэтому
        // DestroyWindow здесь не вызывается.
        logEvent(core::LogLevel::Info, "ui.window.close_cancelled", "закрытие отменено обработчиком");
        return;
    }
    closeRequested_ = true;
    rememberPlacement(window);
    logEvent(core::LogLevel::Info, "ui.window.closing", "окно закрывается по команде пользователя", "dpi", dpi_.x);
    (void)::DestroyWindow(window);
}

LRESULT AppShell::handleQueryEndSession(HWND window) {
    // Система спрашивает разрешение и ждёт ответа. Положение окна снимаем здесь,
    // а не в WM_CLOSE: при завершении сеанса WM_CLOSE не придёт вовсе.
    rememberPlacement(window);
    const bool allowed = onCloseRequested(CloseReason::system);
    logEvent(core::LogLevel::Info, allowed ? "ui.session.end_allowed" : "ui.session.end_denied",
            allowed ? "системе разрешено завершить сеанс" : "завершение сеанса отменено приложением");
    return allowed ? TRUE : FALSE;
}

void AppShell::handleEndSession(HWND window, bool sessionEnding) noexcept {
    if (window == nullptr) return;
    if (!sessionEnding) {
        // wParam == FALSE: сеанс отменён (пользователь передумал выключать).
        // Данные уже сняты, перезаписывать их заново незачем.
        logEvent(core::LogLevel::Info, "ui.session.end_cancelled", "завершение сеанса отменено системой");
        return;
    }
    rememberPlacement(window);
    // Последнее сообщение перед тем, как система нас убьёт. Цикл сообщений
    // закрываем сами: иначе WM_QUIT не будет обработан, а onShutdown не
    // вызовется — а он освобождает то, что живёт дольше окна.
    logEvent(core::LogLevel::Info, "ui.session.ending", "система завершает сеанс, окно закрывается штатно");
    ::PostQuitMessage(exitcode::ok);
}

void AppShell::handleDestroy(HWND window) {
    // WM_DESTROY приходит и хосту содержимого. PostQuitMessage оттуда убил бы
    // цикл сообщений живого окна, поэтому условие обязательное.
    if (!isMainWindow(window)) return;
    onShutdown();
    ::PostQuitMessage(exitcode::ok);
}

void AppShell::handleNcDestroy(HWND window) noexcept {
    if (isMainWindow(window)) {
        // Порядок важен: сначала снимаем ссылки на уже несуществующие окна, потом
        // чистим userdata. Иначе обработчик, пришедший между этими двумя
        // строками, полезет в мёртвое HWND.
        mainWindow_ = nullptr;
        contentHost_ = nullptr;
        logEvent(core::LogLevel::Info, "ui.window.destroyed", "главное окно уничтожено");
    }
    ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
}

// --- Ввод: команды, уведомления, клавиатура ---------------------------------
//
// Три маршрутизатора одного назначения: донести сообщение до хука экрана и, если
// тот не справился, вернуть сообщение системе без изменения. Возвращать TRUE из
// WM_NOTIFY нельзя — для уведомлений это «0», а не «позволяю DefWindowProc».
// WM_NOTIFY, который не обработан, обязан уйти в DefWindowProc: там его
// разбирают NM_CLICK и прочие стандартные реакции, иначе теряется весь смысл
// вызова сообщения вовсе.

LRESULT AppShell::handleCommand(HWND window, WPARAM wParam, LPARAM lParam) {
    const auto controlId = static_cast<WORD>(LOWORD(wParam));
    const auto notificationCode = static_cast<WORD>(HIWORD(wParam));
    // Для меню lParam равен нулю, и координаты мыши не имеют смысла: хук обязан
    // смотреть на controlId, а не на caret.
    const POINT caret{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (onCommand(window, controlId, notificationCode, caret)) return 0;
    return ::DefWindowProcW(window, WM_COMMAND, wParam, lParam);
}

LRESULT AppShell::handleNotify(HWND window, LPARAM lParam) {
    // lParam — адрес NMHDR внутри памяти отправителя. Он валиден только на время
    // вызова, и разбирать конкретную структуру (NMCUSTOMDRAW и далее) обязан
    // вызывающий: у него есть тип уведомления в hdr->code.
    const auto* header = reinterpret_cast<const tagNMHDR*>(lParam);
    if (header != nullptr && onNotify(header->hwndFrom, header)) return 0;
    return ::DefWindowProcW(window, WM_NOTIFY, 0, lParam);
}

LRESULT AppShell::handleKeyDown(HWND window, WPARAM wParam, LPARAM lParam) {
    if (onKeyDown(window, static_cast<UINT>(wParam), lParam)) return 0;
    // Необработанный ключ уходит системе: обработка ускорителей меню (Alt+F) и
    // повтор при удержании живёт именно там.
    return ::DefWindowProcW(window, WM_KEYDOWN, wParam, lParam);
}

void AppShell::layoutContentHost() noexcept {
    if (mainWindow_ == nullptr || contentHost_ == nullptr) return;
    RECT client{};
    if (::GetClientRect(mainWindow_, &client) == 0) return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    // SetWindowPos с теми же размерами и положением — вызов вхолостую: он всё
    // равно проходит через USER32 и будит перерисовку хоста. Хост и так
    // растянут на клиентскую область, поэтому чаще всего именно этот случай.
    RECT current{};
    if (::GetWindowRect(contentHost_, &current) != 0) {
        POINT origin{client.left, client.top};
        if (::ClientToScreen(mainWindow_, &origin) != 0 && current.left == origin.x && current.top == origin.y &&
            current.right - current.left == width && current.bottom - current.top == height) {
            return;
        }
    }
    (void)::SetWindowPos(contentHost_, nullptr, 0, 0, width, height, kFlagsNoZOrderNoActivate);
}

void AppShell::paintContentHost(HWND window) noexcept {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(window, &paint);
    if (dc != nullptr) {
        RECT client{};
        (void)::GetClientRect(window, &client);
        auto brush = reinterpret_cast<HBRUSH>(::GetClassLongPtrW(window, GCLP_HBRBACKGROUND));
        if (brush == nullptr) brush = systemWindowBrush();
        (void)::FillRect(dc, &client, brush);
    }
    // EndPaint обязателен в любом случае: невызванный он оставляет невалидированную
    // область, и система будет слать WM_PAINT снова и снова.
    (void)::EndPaint(window, &paint);
}

// ---------------------------------------------------------------------------
// Точка входа
// ---------------------------------------------------------------------------

int runApp(HINSTANCE instance, int showCommand) noexcept {
    if (instance == nullptr) {
        logEvent(core::LogLevel::Error, "ui.shell.no_instance", "приложение запущено без HINSTANCE");
        return exitcode::noInstance;
    }

    // COM нужен UI-потоку (OLE, перетаскивание, общие контролы v6), а не
    // интерфейсу, поэтому инициализация здесь. RPC_E_CHANGED_MODE означает, что
    // кто-то уже инициализировал поток в другом режиме: это не наша ошибка и
    // CoUninitialize вызывать нельзя — иначе мы разбалансируем чужой счётчик.
    bool comInitialized = false;
    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(comResult)) {
        comInitialized = true;
    } else if (comResult != RPC_E_CHANGED_MODE) {
        logEvent(core::LogLevel::Error,
            "ui.shell.com_init_failed",
            "CoInitializeEx не сработала",
            "hr",
            static_cast<std::int64_t>(comResult));
    }

    // Общие контролы v6 — SysListView32 и SysTreeView32 из §7. Без этого
    // приложение отрисует старые контролы без тем и без CustomDraw.
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    if (::InitCommonControlsEx(&controls) == FALSE) {
        logEvent(core::LogLevel::Warn,
            "ui.shell.common_controls_failed",
            "общие контролы v6 не инициализированы",
            "hr",
            ::GetLastError());
    }

    int result = exitcode::ok;
    try {
        // nCmdShow из wWinMain уважается: «показать свёрнутым» (запуск из
        // уведомления) нельзя потерять, подставив собственное значение.
        AppShell::Options options;
        options.showCommand = showCommand;
        AppShell shell{std::move(options)};
        result = shell.run(instance);
    } catch (const std::exception& error) {
        // SPEC §5: ни один отказ не роняет процесс. Сюда попадает только то,
        // что вылетело за пределы обработчиков окна (в windowProc исключения уже
        // ловятся), то есть ошибки выделения памяти в самом каркасе.
        logEvent(core::LogLevel::Error,
            "ui.shell.unhandled_exception",
            std::string("исключение в каркасе приложения: ") + error.what());
        result = exitcode::unhandledException;
    } catch (...) {
        logEvent(core::LogLevel::Error,
            "ui.shell.unhandled_exception",
            "каркас приложения бросил неизвестное исключение");
        result = exitcode::unhandledException;
    }

    if (comInitialized) {
        (void)::CoUninitialize();
    }
    logEvent(core::LogLevel::Info, "ui.shell.exit", "приложение завершено", "code", result);
    return result;
}

}  // namespace mrproper::ui

// wWinMain — единственная точка входа приложения. UNICODE определён на уровне
// проекта, поэтому линкер сам выбирает wWinMainCRTStartup; явно задавать точку
// входа не нужно и нельзя (иначе придётся дублировать её в CMake).
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previousInstance*/, PWSTR /*commandLine*/, int showCommand) {
    return mrproper::ui::runApp(instance, showCommand);
}
