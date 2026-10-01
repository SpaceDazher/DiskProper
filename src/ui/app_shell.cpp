// Реализация каркаса приложения: окно, классы окон, DPI, закрытие.
// Спека и разбор решений — в app_shell.hpp; здесь только код.

#include "app_shell.hpp"

#include <commctrl.h>
#include <objbase.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM: позиция мыши в WM_COMMAND

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "locale.hpp" // каталог строк: подписи рельса лежат прямо в mrproper::ui

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

// --- Объекты GDI на время одной отрисовки ------------------------------------
//
// Ручной DeleteObject в двух выходах из функции отрисовки — это ровно тот код,
// где про вторую ветку забывают, а утечка GDI-объектов кончается «рисунок
// перестал обновляться» через несколько тысяч перерисовок. Владелец один
// класс на оба типа, потому что отличается только вызов DeleteObject.
class ScopedGdi {
public:
    explicit ScopedGdi(HGDIOBJ handle) noexcept : handle_(handle) {}
    ~ScopedGdi() {
        if (handle_ != nullptr) (void)::DeleteObject(handle_);
    }

    ScopedGdi(const ScopedGdi&) = delete;
    ScopedGdi& operator=(const ScopedGdi&) = delete;
    ScopedGdi(ScopedGdi&&) = delete;
    ScopedGdi& operator=(ScopedGdi&&) = delete;

    [[nodiscard]] HGDIOBJ get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HGDIOBJ handle_{nullptr};
};

// RailRect — это x/y/width/height, а Win32 ждёт right/bottom. Одно
// преобразование здесь, иначе в отрисовке появятся четыре места, где
// «ширина плюс x» могли бы забыть.
[[nodiscard]] RECT asRect(const RailRect& rect) noexcept {
    return RECT{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
}

// --- Хранилище настроек интерфейса (SPEC §4 FR-9) ---------------------------
//
// Плоское «ключ → значение», каким пользуются Navigator::exportState и
// SettingsViewModel::exportSettings (NavStateStore), лежит в ветке
// HKCU\Software\MrProper\UI рядом с ThemeMode и FontScalePercent (theme.cpp):
// это узел того же пользователя, а второй файл настроек у одного человека был
// бы двумя правдами об одном и том же.
//
// Реестр, а не файл: настройки маленькие, писать их должен UI-поток (SPEC §6.1
// запрещает I/O в нём), а реестр — это память ядра, а не диск. Тот же выбор
// уже сделан theme.cpp ради темы.
//
// Ключи при записи НЕ удаляются. В этой же ветке лежит ThemeMode, и выборочное
// удаление «своих» ключей рано или поздно съело бы чужую настройку; вместо
// этого каждое значение перезаписывается целиком, а формат защищён номером
// версии в самом хранилище (nav.version, settings.version).
constexpr wchar_t kUiSettingsSubkey[] = L"Software\\MrProper\\UI";

// Имя значения реестра длиннее быть не может (winreg.h не задаёт, но 255
// символов — предел, который не переживает ни один читатель).
constexpr DWORD kMaxValueName = 256;

// Ключ положения окна. Имена настроек пишутся точками (nav.current,
// settings.language) — ограничение реестра на имя значения это допускает, и
// читать их удобнее, чем подчёркивания.
constexpr std::string_view kPlacementKey = "window.placement";

// Прочитать все строковые значения ветки. Отсутствие ветки — не ошибка, а
// первый запуск: возвращается false, store остаётся пустым, и приложение
// стартует с настроек по умолчанию.
bool readUiSettings(NavStateStore& store) noexcept {
    try {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kUiSettingsSubkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return false;
        }
        bool clean = true;
        for (DWORD index = 0;; ++index) {
            wchar_t name[kMaxValueName]{};
            DWORD nameChars = static_cast<DWORD>(std::size(name));
            DWORD type = 0;
            DWORD dataBytes = 0;
            // Два вызова на значение: первый спрашивает размер, второй читает.
            // Второй начинается с ПОЛНОГО размера буфера имени: lpcchValueName —
            // параметр в обе стороны, и после первого вызова в нём лежит длина
            // найденного имени. Если отдать её как «размер буфера», RegEnumValue
            // потребует места под имя плюс завершающий нуль и вернёт
            // ERROR_MORE_DATA (234) на значении, которое читается без единой ошибки.
            const LSTATUS listed = ::RegEnumValueW(key, index, name, &nameChars, nullptr, &type, nullptr, &dataBytes);
            if (listed == ERROR_NO_MORE_ITEMS) break;
            if (listed != ERROR_SUCCESS) {
                clean = false;
                break;
            }
            // Чужие значения (DWORD, двоичные данные) — не настройки интерфейса:
            // их не читаем и не переписываем.
            if (type != REG_SZ || dataBytes == 0 || dataBytes > (1U << 20)) continue;
            const std::wstring wideName(name, nameChars);
            std::wstring value(dataBytes / sizeof(wchar_t) + 1U, L'\0');
            DWORD actual = static_cast<DWORD>(value.size() * sizeof(wchar_t));
            nameChars = static_cast<DWORD>(std::size(name));
            const LSTATUS read = ::RegEnumValueW(key, index, name, &nameChars, nullptr, &type,
                                                 reinterpret_cast<LPBYTE>(value.data()), &actual);
            if (read != ERROR_SUCCESS) {
                clean = false;
                break;
            }
            // Хвостовой нуль (или нули, если буфер был с запасом) снимаем, а
            // строку обрезаем по фактически прочитанному объёму: иначе в
            // настройку уехал бы мусор из непрочитанного буфера.
            value.resize(actual / sizeof(wchar_t));
            while (!value.empty() && value.back() == L'\0') value.pop_back();
            const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), -1, nullptr, 0, nullptr, nullptr);
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (needed <= 1 || size <= 1) continue;
            std::string keyText(static_cast<std::size_t>(needed), '\0');
            std::string valueText(static_cast<std::size_t>(size), '\0');
            (void)::WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), -1, keyText.data(), needed, nullptr, nullptr);
            (void)::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, valueText.data(), size, nullptr, nullptr);
            keyText.resize(static_cast<std::size_t>(needed - 1));
            valueText.resize(static_cast<std::size_t>(size - 1));
            store[keyText] = std::move(valueText);
        }
        ::RegCloseKey(key);
        return clean;
    } catch (...) {
        return false;
    }
}

// Записать все значения. Ничего не удаляет — см. комментарий выше про ThemeMode.
bool writeUiSettings(const NavStateStore& store) noexcept {
    try {
        HKEY key = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, kUiSettingsSubkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                              nullptr) != ERROR_SUCCESS) {
            return false;
        }
        bool clean = true;
        for (const auto& [name, value] : store) {
            const std::wstring wideName = toWide(name);
            const std::wstring wideValue = toWide(value);
            const DWORD bytes = static_cast<DWORD>((wideValue.size() + 1U) * sizeof(wchar_t));
            if (::RegSetValueExW(key, wideName.c_str(), 0, REG_SZ,
                                 reinterpret_cast<const BYTE*>(wideValue.c_str()), bytes) != ERROR_SUCCESS) {
                clean = false;
            }
        }
        ::RegCloseKey(key);
        return clean;
    } catch (...) {
        return false;
    }
}

// Положение окна — пять чисел через «|», без заголовков: формат читается
// глазами в отладчике реестра, а разбирать его должна только эта пара функций.
std::string encodePlacement(const WINDOWPLACEMENT& placement) {
    const RECT& rect = placement.rcNormalPosition;
    return std::to_string(placement.showCmd) + "|" + std::to_string(rect.left) + "|" +
           std::to_string(rect.top) + "|" + std::to_string(rect.right) + "|" + std::to_string(rect.bottom);
}

bool decodePlacement(std::string_view text, WINDOWPLACEMENT& out) noexcept {
    long long numbers[5] = {0, 0, 0, 0, 0};
    std::size_t field = 0;
    std::size_t index = 0;
    while (index <= text.size() && field < std::size(numbers)) {
        const std::size_t separator = text.find('|', index);
        const std::size_t end = (separator == std::string_view::npos) ? text.size() : separator;
        if (end == index) return false;  // пустое поле — запись битая, не «ноль»
        // strtoll требует char*, а разбирать постоянную строку из реестра
        // дешевле, чем городить ручный перевод цифр.
        std::string fieldText(text.substr(index, end - index));
        char* tail = nullptr;
        const long long value = ::_strtoi64(fieldText.c_str(), &tail, 10);
        if (tail == nullptr || *tail != '\0') return false;
        numbers[field++] = value;
        if (separator == std::string_view::npos) break;
        index = separator + 1;
    }
    if (field != std::size(numbers)) return false;
    if (numbers[0] < 0 || numbers[0] > SW_SHOWMAXIMIZED) return false;
    // Прямоугольник нужен непустым: нулевой размер означал бы окно без окна.
    if (numbers[3] - numbers[1] < 100 || numbers[4] - numbers[2] < 100) return false;
    out = WINDOWPLACEMENT{};
    out.length = sizeof(WINDOWPLACEMENT);
    out.showCmd = static_cast<UINT>(numbers[0]);
    out.rcNormalPosition = RECT{static_cast<LONG>(numbers[1]), static_cast<LONG>(numbers[2]),
                                static_cast<LONG>(numbers[3]), static_cast<LONG>(numbers[4])};
    return true;
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
// ---------------------------------------------------------------------------
// Экраны: создание, показ, освобождение
// ---------------------------------------------------------------------------

HWND AppShell::screenWindow(PageId page) const noexcept {
    switch (page) {
        case PageId::Overview: return overviewScreen_ ? overviewScreen_->window() : nullptr;
        case PageId::Disks: return disksScreen_ ? disksScreen_->window() : nullptr;
        case PageId::Cleanup: return cleanupScreen_ ? cleanupScreen_->window() : nullptr;
        case PageId::Report: return reportScreen_ ? reportScreen_->window() : nullptr;
        case PageId::Settings: return settingsScreen_ ? settingsScreen_->window() : nullptr;
    }
    return nullptr;
}

void AppShell::mountScreens() {
    if (screensReady_ || contentHost_ == nullptr) return;
    const int dpi = static_cast<int>(dpi_.x);
    // До создания экранов: их create() сам зовёт locale::initialize(), а тот
    // применяет язык локали ОС и затёр бы сохранённый выбор пользователя.
    restoreUiState();
    // Порядок важен только для читаемости журнала: все пять создаются сразу, чтобы
    // переключение страницы не ждало создания окна (переключение должно быть мгновенным).
    disksScreen_ = std::make_unique<disks::DisksScreen>();
    cleanupScreen_ = std::make_unique<cleanup::CleanupScreen>();
    overviewScreen_ = std::make_unique<overview::OverviewScreen>();
    reportScreen_ = std::make_unique<report::ReportScreen>();

    // Язык — единственный хук экрана настроек, который до сих пор был мёртвым:
    // комбобокс шлёт CBN_SELCHANGE, модель запоминает намерение, а применить его
    // мог только вызывающий. Модель настроек и каталог строк живут в разных
    // слоях, поэтому подписка вешается здесь, а не внутри экрана.
    settings::SettingsScreen::Callbacks settingsCallbacks;
    settingsCallbacks.onLanguageChanged = [this](core::Language language) {
        applyInterfaceLanguage(language);
        // Сразу, а не «при следующем выходе»: язык, выбранный и потерянный при
        // аварийном закрытии, читался бы как «настройка не работает».
        saveUiState();
    };
    settingsScreen_ = std::make_unique<settings::SettingsScreen>(std::move(settingsCallbacks));

    struct Slot {
        PageId page;
        HWND window;
    };
    const Slot slots[] = {
        {PageId::Disks, disksScreen_->create(contentHost_, dpi)},
        {PageId::Cleanup, cleanupScreen_->create(contentHost_, dpi)},
        {PageId::Overview, overviewScreen_->create(contentHost_, dpi)},
        {PageId::Report, reportScreen_->create(contentHost_, dpi)},
        {PageId::Settings, settingsScreen_->create(contentHost_, dpi)},
    };
    for (const Slot& slot : slots) {
        if (slot.window == nullptr) {
            logEvent(core::LogLevel::Error, "ui.screen.create_failed",
                     "экран не создался в хосте содержимого", "page", pageKey(slot.page));
        }
    }
    // Правила, фильтр и выбранное правило — после create(): модель наполняется
    // при создании экрана, и импорт поверх неё затирал бы то, что уже загрузил
    // сам экран (пустой список правил, например).
    if (settingsScreen_ != nullptr) {
        std::vector<std::string> problems;
        const std::size_t applied = settingsScreen_->model().importSettings(savedState_, &problems);
        for (const std::string& problem : problems) {
            logEvent(core::LogLevel::Warn, "ui.settings.restore_problem", problem);
        }
        if (applied > 0) {
            logEvent(core::LogLevel::Info, "ui.settings.restored",
                     "настройки правил восстановлены", "keys", static_cast<long long>(applied));
        }
        settingsScreen_->refresh();
    }
    screensReady_ = true;
    showActiveScreen(navigator_.current());
    logEvent(core::LogLevel::Info, "ui.screens.mounted",
             "экраны созданы в хосте содержимого", "count", static_cast<long long>(std::size(slots)));
}

void AppShell::showActiveScreen(PageId page) {
    if (!screensReady_) return;
    for (PageId candidate : {PageId::Overview, PageId::Disks, PageId::Cleanup, PageId::Report,
                             PageId::Settings}) {
        const HWND screen = screenWindow(candidate);
        if (screen == nullptr) continue;
        // SW_SHOW для активной страницы, SW_HIDE для остальных: экран, оставленный
        // видимым, перекрывает активный и рисуется поверх него.
        ::ShowWindow(screen, candidate == page ? SW_SHOW : SW_HIDE);
    }
    refreshActiveScreen();
    layoutContentHost();
    invalidateChrome();
}

void AppShell::refreshActiveScreen() {
    if (!screensReady_) return;
    switch (navigator_.current()) {
        case PageId::Overview:
            if (overviewScreen_) overviewScreen_->refresh();
            break;
        case PageId::Disks:
            if (disksScreen_) disksScreen_->refresh();
            break;
        case PageId::Cleanup:
            if (cleanupScreen_) cleanupScreen_->refresh();
            break;
        case PageId::Report:
            if (reportScreen_) reportScreen_->refresh();
            break;
        case PageId::Settings:
            if (settingsScreen_) settingsScreen_->refresh();
            break;
    }
}

// ---------------------------------------------------------------------------
// Настройки между запусками (SPEC §4 FR-9)
// ---------------------------------------------------------------------------

void AppShell::restoreUiState() noexcept {
    try {
        // Каталог строк и язык локали ОС — ДО сохранённого языка: initialize()
        // применяет язык локали, и вызов после него затёр бы выбор пользователя
        // ровно тем, ради чего он восстанавливается.
        if (!isInitialized()) (void)initialize();

        std::vector<std::string> problems;
        const std::size_t applied = navigator_.importState(savedState_, &problems);
        for (const std::string& problem : problems) {
            logEvent(core::LogLevel::Warn, "ui.nav.restore_problem", problem);
        }

        // Язык. Ключ settings.language пишет модель настроек (exportSettings),
        // и читать его надо здесь же: иначе выбор языка жил бы в реестре, но
        // никто бы его не применял — ровно тот отказ, что и был.
        const auto language = savedState_.find(std::string("settings.language"));
        if (language != savedState_.end()) {
            if (const std::optional<core::Language> parsed = core::parseLanguage(language->second)) {
                applyInterfaceLanguage(*parsed);
            } else {
                logEvent(core::LogLevel::Warn, "ui.settings.language_unparsed",
                         "сохранённый язык не разобран, взят язык локали",
                         "value", language->second);
            }
        }

        // Положение окна тоже часть хранилища; модель настроек его держит для
        // моста (SettingsScreen::setWindowPlacement), и без этого поля
        // сохранённый прямоугольник просто некуда положить.
        const auto placement = savedState_.find(std::string(kPlacementKey));
        if (placement != savedState_.end()) {
            WINDOWPLACEMENT restored{};
            if (decodePlacement(placement->second, restored)) {
                settingsPlacement_ = restored;
            } else {
                logEvent(core::LogLevel::Warn, "ui.window.placement_unparsed",
                         "сохранённое положение окна не разобрано");
            }
        }

        layoutRail();
        logEvent(core::LogLevel::Info, "ui.state.restored", "состояние интерфейса восстановлено", "navKeys",
                 static_cast<long long>(applied), "language", std::string(core::languageTag(currentLanguage())));
    } catch (...) {
        MRP_LOG_ERROR("ui.state.restore_failed", "состояние интерфейса не восстановлено");
    }
}

void AppShell::applyInterfaceLanguage(core::Language language) noexcept {
    try {
        if (!isInitialized()) (void)initialize();
        // Три вещи, и по отдельности они расходятся: каталог строк (tr), язык
        // Win32-контролов (SetThreadUILanguage) и запасные подписи рельса,
        // которые берутся из встроенного набора по navigator_.language().
        const Language applied = mrproper::ui::setLanguage(language);
        navigator_.setLanguage(applied);
        if (settingsScreen_ != nullptr) settingsScreen_->model().setLanguage(applied);
        layoutRail();
        layoutContentHost();
        // Только активный экран: скрытые перерисуются при показе, а обновлять
        // все пять означало бы работу впустую на каждом переключении языка.
        refreshActiveScreen();
        invalidateChrome();
        logEvent(core::LogLevel::Info, "ui.language.applied", "язык интерфейса применён", "language",
                 std::string(core::languageTag(applied)), "revision", static_cast<long long>(revision()));
    } catch (...) {
        MRP_LOG_ERROR("ui.language.apply_failed", "язык интерфейса не применён");
    }
}

void AppShell::saveUiState() noexcept {
    try {
        // Положение снимается здесь, а не берётся из lastPlacement_: закрытие
        // могло прийти не через WM_CLOSE (завершение сеанса), и тогда память о
        // нём ещё пустая.
        if (lastPlacement_.length != sizeof(WINDOWPLACEMENT) && mainWindow_ != nullptr) {
            rememberPlacement(mainWindow_);
        }
        NavStateStore store;
        navigator_.exportState(store);
        if (settingsScreen_ != nullptr) settingsScreen_->model().exportSettings(store);
        if (lastPlacement_.length == sizeof(WINDOWPLACEMENT)) {
            store[std::string(kPlacementKey)] = encodePlacement(lastPlacement_);
        }
        const bool clean = writeUiSettings(store);
        logEvent(clean ? core::LogLevel::Info : core::LogLevel::Warn, "ui.state.saved",
                 clean ? "состояние интерфейса сохранено" : "состояние интерфейса не сохранено", "keys",
                 static_cast<long long>(store.size()));
    } catch (...) {
        MRP_LOG_ERROR("ui.state.save_failed", "состояние интерфейса не сохранено");
    }
}

void AppShell::restoreSavedPlacement(HWND window) noexcept {
    if (window == nullptr) return;
    if (settingsPlacement_.length != sizeof(WINDOWPLACEMENT)) return;
    WINDOWPLACEMENT placement = settingsPlacement_;
    if (options_.showCommand == SW_SHOWMINIMIZED) {
        // Запуск из ярлыка «Свёрнутым» не должен разворачивать окно.
        placement.showCmd = SW_SHOWMINIMIZED;
    }
    // Прямоугольник из прошлого запуска мог остаться на мониторе, которого
    // больше нет: такое окно не показать, а не показать за краем экрана.
    const HMONITOR monitor = ::MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(monitor, &info) != 0) {
        const RECT& work = info.rcWork;
        RECT& rect = placement.rcNormalPosition;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width > work.right - work.left || height > work.bottom - work.top ||
            rect.right <= work.left || rect.left >= work.right || rect.bottom <= work.top || rect.top >= work.bottom) {
            logEvent(core::LogLevel::Warn, "ui.window.placement_offscreen",
                     "сохранённое положение не помещается на экране, окно открыто по умолчанию");
            return;
        }
        rect.left = std::clamp(rect.left, work.left, std::max(work.left, work.right - width));
        rect.top = std::clamp(rect.top, work.top, std::max(work.top, work.bottom - height));
        rect.right = rect.left + width;
        rect.bottom = rect.top + height;
    }
    restoreFrom(placement);
}

void AppShell::onShutdown() noexcept {
    // SPEC §4 FR-9: настройки переживают перезапуск. Сохраняем здесь, а не в
    // деструкторе: к этому моменту экраны ещё живы, и их модели ещё можно
    // спросить (деструктор AppShell разрушает окна, и модель уже пуста).
    saveUiState();
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
    // Настройки предыдущего запуска читаются здесь, до первого окна: язык нужен
    // до первого нарисованного контрола, а положение — до первого ShowWindow.
    // Читается один раз и живёт в savedState_ до конца сеанса.
    (void)readUiSettings(savedState_);
    // Тема читается один раз здесь, до первого окна: первый же WM_PAINT уже
    // должен знать цвета иначе кадр мигнёт системным белым.
    theme_.setDpi(dpi_.x);

    // Подписи рельса. Резолвер отдаёт ПУСТУЮ строку, когда перевода нет:
    // core::StringCatalog::resolve на неизвестном ключе возвращает сам ключ
    // (core/i18n.cpp), а рисовать в рельсе «nav.page.overview» вместо слова
    // «Обзор» нельзя. Пустой ответ Navigator::label превращает во встроенную
    // подпись ru/en из kPages — рельс осмыслен с первого кадра и без каталога.
    navigator_.setTitleResolver([](std::string_view key) -> std::string {
        const std::string translated = tr(key);
        if (translated.empty() || translated == key) return std::string();
        return translated;
    });
    // Подписка на смену страницы: перерисовать рельс и хост и позвать экран.
    navigator_.setPageChangedHandler([this](PageId from, PageId to) { handlePageChanged(from, to); });
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
        // Положение из прошлого запуска — до ShowWindow: показанное окно уже не
        // двигает SetWindowPlacement так же незаметно, как скрытое.
        restoreSavedPlacement(window);
        // Возвращаемое значение ShowWindow — предыдущее состояние видимости, а не
        // «успех»: FALSE на первом показе это норма, поэтому проверять нечего.
        ::ShowWindow(window, options_.showCommand);
    }
    ::UpdateWindow(window);
    // Фокус ввода — на рельсе, а не «где оказалось»: без этого первое же
    // нажатие Ctrl+1..5 ушло бы мимо нашего WM_KEYDOWN, и горячие клавиши
    // страниц выглядели бы сломанными при живом приложении. SetFocus работает
    // только внутри потока, чужие окна он не забирает.
    ::SetFocus(window);
    logEvent(core::LogLevel::Info, "ui.window.created", "главное окно создано", "dpi", dpi,
        "railWidthPx", railWidthPx());
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
            theme_.setDpi(dpi_.x);
            layoutRail();
            layoutContentHost();
            invalidateChrome();
            logEvent(core::LogLevel::Info, "ui.dpi.display_changed", "сменилась топология мониторов", "dpi", dpi_.x);
        }
        return 0;

    // Смена темы и системных цветов. applyMessage внутри решает, что именно
    // изменилось (схема, шрифты, DPI), и возвращает false, если ничего: молча
    // согласиться на WM_SETTINGCHANGE, не перечитав цвета, значило бы оставить
    // окно в прошлой теме до следующего запуска (§5 «Тема»).
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        if (theme_.applyMessage(message, wParam, lParam)) {
            applyWindowDarkMode(window);
            invalidateChrome();
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
        // Ctrl+Z (§7.2) и цифровые переходы между страницами. Ключи приходят
        // и в главное окно (фокус на рельсе), и в хост содержимого (фокус на
        // активном экране) — оба окна живут в одном цикле сообщений, поэтому
        // «Ctrl+3» работает откуда угодно (§5 доступность: клавиатура не
        // должна «застревать» там, где стоит фокус).
        if (!isMainWindow(window) && window != contentHost_) break;
        return handleKeyDown(window, wParam, lParam);

    // --- Рельс: мышь и фокус -------------------------------------------------
    // Только главное окно: хост содержимого занимает всю остальную площадь,
    // и мышь над пунктом рельса приходит в WM_LBUTTONDOWN главного окна, а не
    // вложенного. Вне рельса сообщение уходит в DefWindowProc, чтобы поведение
    // главного окна (будущее меню, заголовок) не менялось.

    case WM_MOUSEMOVE:
        if (isMainWindow(window) && handleRailMouseMove(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
            return 0;
        }
        break;

    case WM_MOUSELEAVE:
        if (!isMainWindow(window)) break;
        // Подписка одноразовая: без сброса флага следующий WM_MOUSEMOVE не
        // переподписался бы и наведение «залипло» бы после ухода с рельса.
        railMouseTracked_ = false;
        if (railHover_.has_value() || railPressed_.has_value()) {
            railHover_.reset();
            railPressed_.reset();
            invalidateChrome();
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (isMainWindow(window) && handleRailClick(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (!isMainWindow(window)) break;
        handleRailRelease(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        // Прерванное нажатие (Alt+Tab, другое окно перехватило мышь) не должно
        // оставлять пункт «залипшим» нажатым.
        if (!isMainWindow(window)) break;
        if (railPressed_.has_value()) {
            railPressed_.reset();
            invalidateChrome();
        }
        return 0;

    case WM_SETFOCUS:
        if (!isMainWindow(window)) break;
        railFocused_ = true;
        invalidateChrome();
        return 0;

    case WM_KILLFOCUS:
        if (!isMainWindow(window)) break;
        railFocused_ = false;
        invalidateChrome();
        return 0;

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
        // Фон целиком закрашивается в WM_PAINT, поэтому закраска здесь только
        // мелькала бы. 1 означает «считать стёртым»: невызванная закраска
        // оставила бы мусор от прошлого кадра в области, которую WM_PAINT
        // перерисует позже в этом же кадре.
        return 1;

    case WM_PAINT:
        if (isMainWindow(window)) {
            paintMainWindow(window);
            return 0;
        }
        paintContentHost(window);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

// Хост содержимого создан как дочернее окно ТОГО ЖЕ класса и того же
// windowProc, поэтому его WM_CREATE приходит в handleMessage и попадает сюда
// же. Собирать на нём главное окно нельзя: ниже стоят mainWindow_ = window и
// CreateWindowExW хоста содержимого, а значит хост содержимого завёл бы
// собственного хоста содержимого, тот — ещё одного, и так до исчерпания
// стека. Наружу это выходит как 0xC000041D (STATUS_FATAL_USER_CALLBACK_
// EXCEPTION), то есть ui-smoke.ps1 рапортовал «процесс упал», а не «окно
// пустое». Признак «своё это окно или чужое» берём из WS_CHILD: он задан
// стилем окна, а не порядком присваиваний, и потому не зависит от того,
// успел ли mainWindow_ уже получить значение.
void AppShell::applyWindowDarkMode(HWND window) noexcept {
    // Реентерабельность, а не «осторожность». theme::enableDarkModeForWindow
    // внутри зовёт RefreshImmersiveColorPolicy(), а та рассылает WM_SETTINGCHANGE
    // со сменой ImmersiveColorSet — в том числе самому этому окну. Обработчик
    // WM_SETTINGCHANGE вызывает enableDarkModeForWindow снова (цвет мог
    // поменяться), тот снова рассылает сообщение, и рекурсия съедает стек:
    // приложение падало с 0xC00000FD (STATUS_STACK_OVERFLOW) прямо в WM_CREATE,
    // то есть до первого кадра — ровно тот отказ, который ui-smoke.ps1 рапортует
    // как «процесс упал» (код 6). Повторный вход молча игнорируется: вложенная
    // рассылка всё равно дойдёт до конца, а тему мы применим на следующем
    // WM_SETTINGCHANGE или при следующей перерисовке.
    if (darkModeApplying_) return;
    darkModeApplying_ = true;
    (void)theme::enableDarkModeForWindow(window, theme_.scheme());
    darkModeApplying_ = false;
}

LRESULT AppShell::handleContentHostCreate(HWND window) {
    contentHost_ = window;
    // Ни DPI, ни тему здесь не перечитываем: хост создан в ту же секунду, что и
    // главное окно, на том же мониторе, а его собственный WM_DPICHANGED_AFTERPARENT
    // разбудит handleDpiChanged и обновит кэш при первой же смене масштаба.
    // Раскладку хоста тоже не считаем: в момент его собственного WM_CREATE
    // клиентская область ещё нулевая, и единственный источник правильных
    // размеров — handleSize главного окна, вызванный сразу после этого.
    logEvent(core::LogLevel::Info, "ui.content_host.created", "хост содержимого создан");
    onCreate(window);
    return 0;
}

LRESULT AppShell::handleCreate(HWND window, const CREATESTRUCTW* create) {
    if (create == nullptr || create->lpCreateParams == nullptr) {
        // Окно создано кем-то другим и без нас — продолжать незачем.
        logEvent(core::LogLevel::Error,
            "ui.window.create_params_missing",
            "у окна нет lpCreateParams: это не каркас приложения");
        return -1;  // отказ создать окно
    }

    if ((::GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0) return handleContentHostCreate(window);

    mainWindow_ = window;
    dpi_ = dpiFor(window);
    theme_.setDpi(dpi_.x);
    applyWindowDarkMode(window);
    // Именно GetDpiForWindow, а не dpiForSystem(): окно может появиться на
    // втором мониторе (например, при восстановлении положения), и системный DPI
    // тогда не имеет отношения к его содержимому.
    dpi_ = dpiFor(window);
    theme_.setDpi(dpi_.x);
    // Нативные контролы внутри хоста содержимого обязаны стать тёмными вместе
    // с окном: иначе при тёмной теме белый SysListView32 выглядит как дыра.
    applyWindowDarkMode(window);

    // Рельс — до хоста содержимого: ширина рельса входит в раскладку хоста,
    // и хост, созданный раньше, получил бы в WM_SIZE ещё одну пустую раскладку.
    layoutRail();

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
        // Экраны живут в хосте содержимого: без них окно показывает только рельс.
        mountScreens();
    }

    // Текст окна (заголовок и имя хоста содержимого) доступен экранному диктору
    // и UI Automation (§5 «Доступность»): без него окно озвучивается как «пустое».
    logEvent(core::LogLevel::Info,
        "ui.window.create_completed",
        "клиентская область, рельс и хост содержимого готовы",
        "dpi",
        dpi_.x,
        "contentHost",
        contentHost_ != nullptr,
        "railWidthPx",
        railWidthPx(),
        "page",
        std::string(pageKey(navigator_.current())));
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
        layoutRail();
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
    // Ширина рельса задана в DIP (§7.1, ui::nav RailMetrics), поэтому при
    // 150 % она 224 → 336 px. Кегли и метрики темы обязаны уехать вместе с ней,
    // иначе подписи останутся 96-DPI на полосе в 336 px.
    theme_.setDpi(dpi_.x);

    if (isMainWindow(window) && suggested != nullptr && suggested->right > suggested->left &&
        suggested->bottom > suggested->top) {
        // Ровно предложенный прямоугольник и ровно эти флаги: без SWP_NOZORDER
        // окно подскочило бы поверх других, а с SWP_FRAMECHANGED рамка
        // перерисовалась бы до того, как система её обновит.
        (void)::SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, kFlagsNoZOrderNoActivate);
        // WM_SIZE после SetWindowPos пересчитает раскладку сам, но к этому
        // моменту dpi_ уже новый — а если система не прислала suggested
        // (перенос без изменения размера), раскладку надо посчитать здесь.
        layoutRail();
        layoutContentHost();
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
    // вызовется — а он сохраняет состояние интерфейса (§4 FR-9) и освобождает
    // то, что живёт дольше окна.
    saveUiState();
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
        railHover_.reset();
        railPressed_.reset();
        railFocused_ = false;
        railMouseTracked_ = false;
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
    // Сначала рельс: он владеет глобальными переходами между страницами, и их
    // должен видеть и экран. Перехватывание без разбора ключа здесь отдало бы
    // Ctrl+Z экрану только потому, что он объявил хук раньше рельса.
    if (handleNavKey(window, static_cast<UINT>(wParam))) return 0;
    if (onKeyDown(window, static_cast<UINT>(wParam), lParam)) return 0;
    // Необработанный ключ уходит системе: обработка ускорителей меню (Alt+F) и
    // повтор при удержании живёт именно там.
    return ::DefWindowProcW(window, WM_KEYDOWN, wParam, lParam);
}

void AppShell::layoutRail() noexcept {
    int clientWidth = 0;
    int clientHeight = 0;
    if (mainWindow_ != nullptr) {
        RECT client{};
        if (::GetClientRect(mainWindow_, &client) != 0) {
            clientWidth = client.right - client.left;
            clientHeight = client.bottom - client.top;
        }
    }
    railLayout_ = RailLayout::compute(railMetrics_, static_cast<int>(dpi_.x), clientHeight,
                                      defaultRailSide(navigator_.language()), navigator_.railScrollPx());
    // Прокрутка хранится у Navigator, а применяется макетом. Расхождение после
    // смены DPI или языка (рельс стал шире/выше и прокрутка больше не нужна)
    // заносим обратно, иначе следующий WM_PAINT посчитал бы макет заново — и
    // получилось бы два разных ответа на один и тот же вопрос.
    if (navigator_.railScrollPx() != railLayout_.scrollOffsetPx()) {
        navigator_.setRailScrollPx(railLayout_.scrollOffsetPx());
    }
}

std::optional<PageId> AppShell::railHitTest(POINT clientPoint) const noexcept {
    if (mainWindow_ == nullptr) return std::nullopt;
    int x = clientPoint.x;
    if (railLayout_.side() == RailSide::Right) {
        // Зеркалирование появляется здесь, а не в каждом обработчике мыши:
        // RailLayout::hitTest ждёт координаты от ведущего края рельса, и при
        // RailSide::Right это правый край окна (§5 «RTL-ready»).
        RECT client{};
        if (::GetClientRect(mainWindow_, &client) == 0) return std::nullopt;
        x = (client.right - client.left) - x;
    }
    return railLayout_.hitTest(x, clientPoint.y);
}

void AppShell::invalidateChrome() noexcept {
    if (mainWindow_ == nullptr) return;
    // Рельс и хост — два разных окна, у каждого своя невалидированная область.
    // Инвалидировать только главное нельзя: WS_CLIPCHILDREN вырезает из его
    // области прямоугольник хоста, и содержимое осталось бы от прежней
    // страницы — то самое «окно живо, а картинка старая».
    ::InvalidateRect(mainWindow_, nullptr, FALSE);
    if (contentHost_ != nullptr) ::InvalidateRect(contentHost_, nullptr, FALSE);
}

bool AppShell::showPage(PageId page) {
    if (!isValidPage(page)) {
        logEvent(core::LogLevel::Warn,
            "ui.nav.invalid_page",
            "запрошена страница вне перечисления",
            "value",
            static_cast<int>(page));
        return false;
    }
    // Фокус рельса следует за целью даже при повторе: иначе второй щелчок по
    // активному пункту оставил бы фокус на прошлой странице и стрелка вниз
    // уехала бы не с того места, куда кликнул человек.
    navigator_.setSelection(page);
    const bool changed = navigator_.goTo(page);
    if (!changed) invalidateChrome();
    return changed;
}

void AppShell::handlePageChanged(PageId from, PageId to) {
    logEvent(core::LogLevel::Info,
        "ui.nav.page_changed",
        "открыта страница интерфейса",
        "from",
        pageKey(from),
        "to",
        pageKey(to),
        "ordinal",
        pageOrdinal(to));
    // Экран готовит содержимое хоста ДО перерисовки: иначе между сменой
    // current_ и первым WM_PAINT хоста пользователь увидел бы пустой белый
    // прямоугольник — ровно тот отказ, который ловит ui-smoke.ps1.
    onPageChanged(from, to);
    showActiveScreen(to);
    invalidateChrome();
}

bool AppShell::handleRailMouseMove(HWND window, int x, int y) noexcept {
    const std::optional<PageId> hit = railHitTest(POINT{x, y});
    trackRailMouseLeave(window);
    if (hit == railHover_) return false;
    railHover_ = hit;
    invalidateChrome();
    return true;
}

bool AppShell::handleRailClick(HWND window, int x, int y) {
    const std::optional<PageId> hit = railHitTest(POINT{x, y});
    if (!hit.has_value()) {
        // Щелчок мимо пунктов (зазор, отступ, область содержимого) не должен
        // ничего открывать: «промахнулся — ничего не произошло» надёжнее, чем
        // переключение на ближайший пункт.
        if (railPressed_.has_value()) {
            railPressed_.reset();
            invalidateChrome();
        }
        return false;
    }
    if (railPressed_.has_value() && *railPressed_ == *hit) return true;
    railPressed_ = hit;
    trackRailMouseLeave(window);
    // Фокус — на рельс: иначе после первого щелчка клавиатура продолжила бы
    // слать стрелки в хост содержимого, а не в то, что человек только что
    // открыл (§5 «Клавиатурная навигация, фокус»).
    ::SetFocus(window);
    // Захват мыши: отпустить кнопку над другим пунктом должно означать
    // «отменить», а не «переключить на тот, под которым отпустили».
    ::SetCapture(window);
    invalidateChrome();
    return true;
}

void AppShell::handleRailRelease(HWND window, int x, int y) {
    if (!railPressed_.has_value()) return;
    const std::optional<PageId> hit = railHitTest(POINT{x, y});
    const bool activate = hit.has_value() && *hit == *railPressed_;
    railPressed_.reset();
    if (::GetCapture() == window) (void)::ReleaseCapture();
    if (activate) (void)showPage(*hit);
    invalidateChrome();
}

void AppShell::trackRailMouseLeave(HWND window) noexcept {
    if (railMouseTracked_) return;
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = window;
    // dwHoverTime = 0 с TME_LEAVE означает «подписаться один раз»: без HOVEREVENTS
    // это единственный код в структуре, который важен здесь, а значение по
    // умолчанию (300 мс) заставило бы ждать отсчёта перед уходом курсора.
    track.dwHoverTime = 0;
    // Отказ означает «подписка уже есть» и ничего больше: попробуем снова при
    // следующем движении, флаг снимется в WM_MOUSELEAVE.
    railMouseTracked_ = (::TrackMouseEvent(&track) != FALSE);
}

bool AppShell::handleNavKey(HWND /*window*/, UINT virtualKey) {
    const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;

    // Ctrl+1..Ctrl+5 — глобальный переход на страницу: работает независимо от
    // того, где стоит фокус. Alt+1..Alt+5 принимается вторым вариантом
    // (см. nav.hpp: он уйдёт меню вместе с Alt+F, поэтому в подписи рельса
    // показывается только Ctrl). Ctrl+Alt — не наш случай: это системные
    // сочетания цифровой клавиатуры.
    if (ctrl != alt) {
        for (std::size_t i = 0; i < kPageCount; ++i) {
            const auto digit = static_cast<UINT>(static_cast<unsigned char>('1' + static_cast<int>(i)));
            if (virtualKey != digit) continue;
            if (ctrl) {
                if (kCtrlDigitSwitchesPage) (void)showPage(kPages[i].id);
            } else if (kAltDigitSwitchesPage) {
                // selectByAccessKey делает и выбор, и переход одним действием.
                (void)navigator_.selectByAccessKey(virtualKey);
            }
            // Клавиша считается обработанной и когда нужная страница уже
            // открыта: иначе Ctrl+1 ушёл бы дальше и сработал как ускоритель
            // меню, то есть повторное нажатие делало бы что-то другое.
            return true;
        }
    }

    // Стрелки, Home/End, PageUp/PageDown, Enter и пробел принадлежат рельсу
    // только когда фокус стоит на нём. Иначе они ушли бы в дерево очистки и
    // список дисков мимо себя, и клавиатура перестала бы работать там, где
    // работала (SPEC §5, §7.1).
    if (!railFocused_) return false;
    switch (virtualKey) {
    case VK_UP:
    case VK_DOWN:
        navigator_.moveSelection(virtualKey == VK_DOWN ? 1 : -1);
        // moveSelection двигает выделение, но не страницу; если страница уже
        // та, notifyPageChanged не будет и перерисовки тоже — а выделение
        // (и рамка фокуса) обязаны уехать.
        if (!navigator_.activateSelection()) invalidateChrome();
        return true;
    case VK_HOME:
        if (!navigator_.goToFirst()) invalidateChrome();
        return true;
    case VK_END:
        if (!navigator_.goToLast()) invalidateChrome();
        return true;
    case VK_PRIOR:
        if (!navigator_.goToPrevious()) invalidateChrome();
        return true;
    case VK_NEXT:
        if (!navigator_.goToNext()) invalidateChrome();
        return true;
    case VK_RETURN:
    case VK_SPACE:
        if (!navigator_.activateSelection()) invalidateChrome();
        return true;
    default:
        break;
    }
    return false;
}

std::wstring AppShell::railLabel(PageId page) const {
    const std::string_view fallback = pageTitleFallback(page, navigator_.language());
    // Navigator::label уже вернул подпись из каталога, если перевод есть, и
    // встроенную ru/en — если нет. Слой locale отдаёт пустую строку на
    // неизвестном ключе, а core::StringCatalog::resolve вернул бы сам ключ
    // («nav.page.overview» в рельсе вместо «Обзора»), поэтому проверка на
    // пустоту здесь не формальность, а единственная защита от такого вывода.
    const std::wstring resolved = toWide(navigator_.label(page));
    if (!resolved.empty()) return resolved;
    return toWide(fallback);
}

std::wstring AppShell::railShortcutHint(PageId page) const {
    const std::string_view shortcut = pageDescriptor(page).shortcut;
    return toWide(shortcut);
}

void AppShell::fillWithColor(HDC dc, const RECT& rect, const theme::Color& color) noexcept {
    if (dc == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) return;
    const ScopedGdi brush(::CreateSolidBrush(theme::colorRef(color)));
    if (!brush) return;  // CreateSolidBrush может вернуть nullptr при нехватке GDI
    (void)::FillRect(dc, &rect, reinterpret_cast<HBRUSH>(brush.get()));
}

void AppShell::paintMainWindow(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(window, &paint);
    // EndPaint зовётся только после успешного BeginPaint: при nullptr окну
    // нечего валидировать, и вызов был бы вызовом по пустому дескриптору.
    if (dc == nullptr) return;

    RECT client{};
    (void)::GetClientRect(window, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    paintRail(dc, client, clientWidth, clientHeight);
    (void)::EndPaint(window, &paint);
}

void AppShell::paintRail(HDC dc, const RECT& client, int clientWidth, int clientHeight) {
    if (dc == nullptr || clientWidth <= 0 || clientHeight <= 0) return;
    const theme::Palette& palette = theme_.palette();
    const theme::Metrics& metrics = theme_.metrics();
    const unsigned dpi = dpi_.x != 0 ? dpi_.x : kBaseDpi;

    // 1) Фон клиентской области. Тот же цвет, что у хоста содержимого: рельс и
    //    содержимое — одно поле, и разный фон читался бы как шов.
    fillWithColor(dc, client, palette.windowBackground);

    const int railWidth = railLayout_.railWidthPx();
    if (railWidth <= 0) return;
    const bool rtl = (railLayout_.side() == RailSide::Right);
    const RECT rail = asRect(railLayout_.toClient(RailRect{0, 0, railWidth, clientHeight}, clientWidth));

    // 2) Полотно рельса — surface, а не windowBackground: иначе у рельса не
    //    было бы собственного цвета и он был бы неотличим от содержимого.
    fillWithColor(dc, rail, palette.surface);

    // 3) Разделитель толщиной в один DIP. Линия, а не тень: тень стоила бы
    //    второго прохода с полупрозрачной кистью, а линия в один пиксель видна
    //    и в светлой, и в тёмной схеме, и при высокой контрастности.
    const int dividerThickness = std::max(1, metrics.dip(1.0));
    const int dividerLeft = rtl ? rail.left : rail.right - dividerThickness;
    fillWithColor(dc, RECT{dividerLeft, rail.top, dividerLeft + dividerThickness, rail.bottom}, palette.border);

    // 4) Шрифты: обычный для неактивных, полужирный для активного. Создаются
    //    на один WM_PAINT и удаляются ScopedGdi — GDI-объекты, переживающие
    //    перерисовку, копятся до «рисунок перестал обновляться».
    LOGFONTW bodyLog = theme_.typography().body.toLogFont(dpi);
    LOGFONTW strongLog = theme_.typography().bodyStrong.toLogFont(dpi);
    const ScopedGdi bodyFont(::CreateFontIndirectW(&bodyLog));
    const ScopedGdi strongFont(::CreateFontIndirectW(&strongLog));
    const HGDIOBJ oldFont = bodyFont ? ::SelectObject(dc, bodyFont.get()) : nullptr;
    const int oldBkMode = ::SetBkMode(dc, TRANSPARENT);

    const int inset = metrics.dip(12.0);
    const int hintWidth = metrics.dip(56.0);

    for (const PageDescriptor& descriptor : kPages) {
        const PageId page = descriptor.id;
        const RECT item = asRect(railLayout_.toClient(railLayout_.itemRect(page), clientWidth));
        if (item.bottom <= rail.top || item.top >= rail.bottom) continue;  // прокрученный пункт

        const bool active = (navigator_.current() == page);
        const bool selected = (navigator_.selection() == page);
        const bool hovered = railHover_.has_value() && railHover_ == page;
        const bool pressed = railPressed_.has_value() && railPressed_ == page;

        if (active) {
            fillWithColor(dc, item, palette.accent);
        } else if (pressed) {
            fillWithColor(dc, item, palette.accentPressed);
        } else if (selected) {
            fillWithColor(dc, item, palette.surfaceSelected);
        } else if (hovered) {
            fillWithColor(dc, item, palette.surfaceHover);
        }

        // Рамка фокуса — только когда фокус действительно на рельсе. Без
        // клавиатуры выделение и так читается по фону, а с клавиатуры рамка
        // обязательна: иначе человек не видит, куда уедут стрелки (§5).
        //
        // NULL_BRUSH здесь обязателен, а не украшение: Rectangle заливает контур
        // ТЕКУЩЕЙ кистью DC, а текущей является системная белая — ни одна
        // наша кисть в DC не выбрана (fillWithColor передаёт её прямо в FillRect).
        // Белая заливка стирала акцент активного пункта, а подпись на нём рисуется
        // цветом textOnAccent, который в светлой схеме тоже белый: полоса из двух
        // цветов и ноль пикселей текста (D-61). С NULL_BRUSH рисуется только
        // контур, заливка остаётся акцентной, и контраст подписи решает палитра.
        if (railFocused_ && selected) {
            const ScopedGdi pen(::CreatePen(PS_SOLID, std::max(1, metrics.dip(1.0)), theme::colorRef(palette.focusRing)));
            if (pen) {
                const HGDIOBJ oldPen = ::SelectObject(dc, pen.get());
                const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
                (void)::Rectangle(dc, item.left + 1, item.top + 1, item.right - 1, item.bottom - 1);
                ::SelectObject(dc, oldBrush);
                ::SelectObject(dc, oldPen);
            }
        }

        const theme::Color textColor = active ? palette.textOnAccent : palette.textPrimary;
        const theme::Color hintColor = active ? palette.textOnAccent : palette.textSecondary;
        const HGDIOBJ itemFont = (active && strongFont) ? strongFont.get() : bodyFont.get();
        if (itemFont != nullptr) (void)::SelectObject(dc, itemFont);
        (void)::SetTextColor(dc, theme::colorRef(textColor));

        // Правая колонка — подсказка горячей клавиши, левая — подпись. При RTL
        // колонки меняются местами: текст идёт справа налево, и подпись должна
        // прилегать к ведущему краю, а подсказка — к противоположному.
        RECT labelRect{item.left + inset, item.top, item.right - inset, item.bottom};
        RECT hintRect{item.right - inset - hintWidth, item.top, item.right - inset, item.bottom};
        if (rtl) std::swap(labelRect, hintRect);

        const std::wstring hint = railShortcutHint(page);
        if (!hint.empty()) {
            (void)::SetTextColor(dc, theme::colorRef(hintColor));
            (void)::DrawTextW(dc, hint.c_str(), -1, &hintRect,
                              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        const std::wstring label = railLabel(page);
        if (!label.empty()) {
            (void)::SetTextColor(dc, theme::colorRef(textColor));
            // DT_NOPREFIX обязателен: без него «&» в подписи съедал бы букву и
            // рисовал вместо неё амперсанд.
            (void)::DrawTextW(dc, label.c_str(), -1, &labelRect,
                              DT_SINGLELINE | DT_VCENTER | (rtl ? DT_RIGHT : DT_LEFT) | DT_END_ELLIPSIS |
                                  DT_NOPREFIX);
        }
    }

    if (oldFont != nullptr) (void)::SelectObject(dc, oldFont);
    ::SetBkMode(dc, oldBkMode);
}

void AppShell::layoutContentHost() noexcept {
    if (mainWindow_ == nullptr || contentHost_ == nullptr) return;
    RECT client{};
    if (::GetClientRect(mainWindow_, &client) == 0) return;
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0) return;

    // Хост занимает клиентскую область МИНУС рельс (§7.1). Раньше он был на всю
    // область, и рельс, нарисованный поверх, закрывал бы левый край первого
    // экрана; теперь ширина рельса вычитается явно, а при RailSide::Right хост
    // прижат к левому краю.
    const int railWidth = railLayout_.railWidthPx();
    const int hostWidth = clientWidth - railWidth;
    if (hostWidth <= 0) return;  // окно уже рельса: хосту негде жить, но и не падаём
    const int hostLeft = (railLayout_.side() == RailSide::Right) ? 0 : railWidth;

    // Окна экранов — дети хоста, и create() задаёт им только класс и родителя, но не
    // размер. Без этого они остаются нулевыми: хост рисует фон, внутри него пусто,
    // и ворота «окно не пустое» видят ноль чернил при живых экранах в памяти.
    // Раскладка экрана совпадает с клиентской областью хоста.
    for (PageId page : {PageId::Overview, PageId::Disks, PageId::Cleanup, PageId::Report, PageId::Settings}) {
        const HWND screen = screenWindow(page);
        if (screen == nullptr) continue;
        (void)::SetWindowPos(screen, nullptr, 0, 0, hostWidth, clientHeight, kFlagsNoZOrderNoActivate);
    }

    // SetWindowPos с теми же размерами и положением — вызов вхолостую: он всё
    // равно проходит через USER32 и будит перерисовку хоста. Чаще всего именно
    // этот случай (окно изменило размер, но не ширину рельса).
    RECT current{};
    if (::GetWindowRect(contentHost_, &current) != 0) {
        POINT origin{hostLeft, 0};
        if (::ClientToScreen(mainWindow_, &origin) != 0 && current.left == origin.x && current.top == origin.y &&
            current.right - current.left == hostWidth && current.bottom - current.top == clientHeight) {
            return;
        }
    }
    if (::SetWindowPos(contentHost_, nullptr, hostLeft, 0, hostWidth, clientHeight, kFlagsNoZOrderNoActivate) == 0) {
        logEvent(core::LogLevel::Warn,
            "ui.layout.content_host_failed",
            "не удалось переместить хост содержимого",
            "hr",
            ::GetLastError());
    }

}

void AppShell::paintContentHost(HWND window) noexcept {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(window, &paint);
    if (dc != nullptr) {
        RECT client{};
        (void)::GetClientRect(window, &client);
        // Фон хоста — из темы, а не GetSysColorBrush: окно и содержимое должны
        // совпадать по цвету, иначе шов между рельсом и содержимым виден даже
        // на первом кадре.
        fillWithColor(dc, client, theme_.palette().windowBackground);
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
