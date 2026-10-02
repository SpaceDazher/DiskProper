// MrProper — реализация темы: реестр, палитра, шрифты, DPI.
// Спека: §5 (строки «Тема» и «DPI»), §7 + ADR-003 (тема общая для Direct2D и
// нативных контролов), §5 «Доступность» (контраст и масштаб текста), §6.4
// (UI-поток без лишней работы), §10 (утилита не должна быть слепой к настройкам
// доступности). Контракт — в theme.hpp; здесь только реализация и разбор
// технических решений, которые видно не из заголовка.
//
// Решения, которые не видны из заголовка.
//
// 1. Реестр читается RegGetValueW, а не «открыть ключ и прочитать значение».
//    Причина в HRESULT-коде отказа: RegGetValueW возвращает LSTATUS и различает
//    «значения нет» (ERROR_FILE_NOT_FOUND) и «доступа нет» (ERROR_ACCESS_DENIED),
//    а RegOpenKeyExW + RegQueryValueExW эту разницу теряет в GetLastError,
//    который к тому же уже затёрт. В лог попадает только второе: отсутствие
//    AppsUseLightTheme на новой установке — норма, а не проблема.
//
// 2. DWM вызывается динамически, без #pragma comment(lib, "dwmapi.lib").
//    Причина практическая: dwmapi.dll — это оболочка рабочего стола, и её
//    импорт в статическую библиотеку переносит требование «этот DLL есть» на
//    любой конечный бинарник, даже headless-CLI, которому тема не нужна вовсе.
//    Тот же приём применён к uxtheme: обе DLL лежат в system32 и грузятся
//    процессами оболочки, но не мы. Указатели кэшируются в статических
//    переменных с инициализацией при первом обращении (гарантия потокобезопасности
//    из C++11); модули намеренно не выгружаются — дочерний указатель пережил бы
//    FreeLibrary, а «утечка» в виде одного handle на весь процесс тут безопаснее
//    гонки.
//
// 3. Контраст считается по формуле WCAG, а палитра — по таблице, из которой
//    значения ДОБИРАЮТСЯ до нужного контраста. Порядок именно такой: сначала
//    «хочу такой акцент», потом «добить до читаемости». Обратный порядок даёт
//    палитру, одинаковую на всех машинах, и намертво зашитый синий — то есть
//    игнорирует настройку пользователя, ради которой модуль написан.
//
// 4. Кегли задаются в DIP, а масштабирование — в одном месте (Metrics::dip,
//    FontDesc::pixelHeight). Никаких «× dpi / 96» в коде отрисовки: там
//    умножение забывают чаще всего, и ошибка проявляется только на втором
//    мониторе с другим масштабом.
//
// 5. Логирование — только на отказах, и только когда отказ информативен.
//    Отсутствие ключа реестра (норма на новой установке) в лог не пишется:
//    иначе каждый запуск на Windows 11 24H2 добавлял бы запись о «проблеме»,
//    которой нет, и утопал бы полезные сообщения.
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>

// commctrl.h — из-за NMCUSTOMDRAW в контракте (шапка столбцов, D-75). Модуль
// не создаёт контролов, но рисует их шапку по сообщению от SysHeader32.
#include <commctrl.h>

#include "core/log.hpp"
#include "platform/win_handle.hpp"

// Windows включается заголовком theme.hpp: HWND и LOGFONTW входят в контракт
// модуля, и прятать их за void* ради «чистоты» заголовка значило бы усложнить
// вызывающий код без единой выгоды (слой ui и так Win32, ADR-004 касается core).
#include <dwmapi.h>

namespace mrproper::ui::theme {
namespace {

// ---------------------------------------------------------------------------
// Ключи и значения реестра
// ---------------------------------------------------------------------------

// Системные ключи темы. Именно эти два пути называет SPEC §5: решение о теме
// приложений лежит в Personalize\AppsUseLightTheme.
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kDwmKey[] = L"Software\\Microsoft\\Windows\\DWM";
constexpr wchar_t kAppsUseLightTheme[] = L"AppsUseLightTheme";
constexpr wchar_t kSystemUsesLightTheme[] = L"SystemUsesLightTheme";  // Windows 11 24H2+
constexpr wchar_t kColorizationColor[] = L"ColorizationColor";

// Настройки приложения. Пишем только сюда: ключ создаётся при первом сохранении,
// читается при каждом старте, отсутствие означает «настроек не было».
constexpr wchar_t kUiSettingsKey[] = L"Software\\MrProper\\UI";
constexpr wchar_t kThemeModeValue[] = L"ThemeMode";
constexpr wchar_t kFontScaleValue[] = L"FontScalePercent";

// ---------------------------------------------------------------------------
// Границы
// ---------------------------------------------------------------------------

constexpr unsigned kReferenceDpi = 96;             // 1 DIP в логических пикселях
constexpr unsigned kMinDpi = 72;                  // ниже Windows не масштабирует
constexpr unsigned kMaxDpi = 480;                 // 500 % и выше — не машина
constexpr double kMinFontScale = 0.75;            // как в настройках Windows
constexpr double kMaxFontScale = 2.0;
constexpr int kContrastSteps = 20;                // шагов подбора контраста
constexpr double kReferenceSystemFontDip = 12.0;  // системный UI-шрифт 9 pt при 96 DPI

// Контрастные требования. 7.0 — AAA на основной текст (SPEC §5, доступность:
// цифры и названия разделов должны читаться без напряжения), 4.5 — AA на
// пояснения, 3.0 — AA для нетекстовых элементов (WCAG 1.4.11): рамки выделения
// и акцент считаются не «текстом», но именно по ним видно, что элемент активен.
constexpr double kContrastBodyText = 7.0;
constexpr double kContrastSecondaryText = 4.5;
constexpr double kContrastNonText = 3.0;
// Неактивный элемент по WCAG от контраста освобождён, но «незаметно серый» —
// это плохо в тьме, поэтому нижняя граница всё же ставится: пользователь должен
// видеть, что элемент есть.
constexpr double kContrastDisabledText = 2.0;

// ---------------------------------------------------------------------------
// Реестр
// ---------------------------------------------------------------------------

enum class RegistryRead { Ok, Missing, Failed };

RegistryRead readDword(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD& out) noexcept {
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LSTATUS status = ::RegGetValueW(root, subkey, value, RRF_RT_REG_DWORD, nullptr, &data, &size);
    if (status == ERROR_SUCCESS && size == sizeof(data)) {
        out = data;
        return RegistryRead::Ok;
    }
    // ERROR_FILE_NOT_FOUND/ERROR_PATH_NOT_FOUND — значения просто нет: на чистой
    // установке Windows 11 AppsUseLightTheme может отсутствовать, и это норма.
    // Всё остальное (ACCESS_DENIED, неверная среда, сетевой профиль) — уже
    // интересно, и об этом пишем в лог: тема молча откатилась на значения по
    // умолчанию, и пользователь будет искать причину в другом месте.
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return RegistryRead::Missing;
    core::logWarn("ui.theme.registry",
                  "чтение значения реестра не удалось, берётся значение по умолчанию",
                  core::LogFields{core::logField("where", "RegGetValueW"),
                                  core::logField("subkey", std::wstring(subkey)),
                                  core::logField("value", std::wstring(value)),
                                  core::logField("status", static_cast<long long>(status))});
    return RegistryRead::Failed;
}

RegistryRead readString(HKEY root, const wchar_t* subkey, const wchar_t* value, std::wstring& out) noexcept {
    wchar_t buffer[32] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    const LSTATUS status =
        ::RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS) {
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) {
            core::logWarn("ui.theme.registry",
                          "чтение строкового значения реестра не удалось, берётся значение по умолчанию",
                          core::LogFields{core::logField("where", "RegGetValueW"),
                                          core::logField("subkey", std::wstring(subkey)),
                                          core::logField("value", std::wstring(value)),
                                          core::logField("status", static_cast<long long>(status))});
            return RegistryRead::Failed;
        }
        return RegistryRead::Missing;
    }
    out = buffer;
    return RegistryRead::Ok;
}

bool writeSetting(HKEY root, const wchar_t* subkey, const wchar_t* value, const wchar_t* text) noexcept {
    HKEY rawKey = nullptr;
    const LSTATUS opened = ::RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &rawKey, nullptr);
    // Ключ берётся в владение сразу после открытия, до любой проверки: если ключ
    // всё-таки создан, закрыть его обязан деструктор, а не ветка отказа.
    const auto key = platform::adopt<platform::RegistryHandlePolicy>(rawKey);
    if (opened != ERROR_SUCCESS || !key) {
        core::logWarn("ui.theme.registry", "не удалось открыть ключ настроек темы",
                      core::LogFields{core::logField("where", "RegCreateKeyExW"),
                                      core::logField("subkey", std::wstring(subkey)),
                                      core::logField("status", static_cast<long long>(opened))});
        return false;
    }
    const LSTATUS status = ::RegSetValueExW(key.get(), value, 0, REG_SZ,
                                            reinterpret_cast<const BYTE*>(text),
                                            static_cast<DWORD>((std::wcslen(text) + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        core::logWarn("ui.theme.registry", "не удалось сохранить настройку темы",
                      core::LogFields{core::logField("where", "RegSetValueExW"),
                                      core::logField("value", std::wstring(value)),
                                      core::logField("status", static_cast<long long>(status))});
        return false;
    }
    return true;
}

bool writeSetting(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD number) noexcept {
    HKEY rawKey = nullptr;
    const LSTATUS opened = ::RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &rawKey, nullptr);
    const auto key = platform::adopt<platform::RegistryHandlePolicy>(rawKey);
    if (opened != ERROR_SUCCESS || !key) {
        core::logWarn("ui.theme.registry", "не удалось открыть ключ настроек темы",
                      core::LogFields{core::logField("where", "RegCreateKeyExW"),
                                      core::logField("subkey", std::wstring(subkey)),
                                      core::logField("status", static_cast<long long>(opened))});
        return false;
    }
    const LSTATUS status =
        ::RegSetValueExW(key.get(), value, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&number), sizeof(number));
    if (status != ERROR_SUCCESS) {
        core::logWarn("ui.theme.registry", "не удалось сохранить настройку темы",
                      core::LogFields{core::logField("where", "RegSetValueExW"),
                                      core::logField("value", std::wstring(value)),
                                      core::logField("status", static_cast<long long>(status))});
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Динамические API оболочки
// ---------------------------------------------------------------------------

using DwmGetColorizationColorFn = HRESULT(WINAPI*)(DWORD* color, BOOL* isActive);

// Возвращает указатель на DwmGetColorizationColor или nullptr. Кэш на процесс:
// вызов идёт при смене темы (десятки раз за сеанс), а LoadLibrary на каждый
// чих не нужен.
DwmGetColorizationColorFn dwmColorizationColor() noexcept {
    static const auto resolved = []() -> DwmGetColorizationColorFn {
        // Сначала пробуем уже загруженную оболочкой DLL (в процессе с темами она
        // почти всегда в памяти), затем LoadLibrary для осторожности.
        HMODULE module = ::GetModuleHandleW(L"dwmapi.dll");
        if (module == nullptr) module = ::LoadLibraryW(L"dwmapi.dll");
        if (module == nullptr) return static_cast<DwmGetColorizationColorFn>(nullptr);
        const auto address = ::GetProcAddress(module, "DwmGetColorizationColor");
        return reinterpret_cast<DwmGetColorizationColorFn>(address);
    }();
    return resolved;
}

// uxtheme: тёмный режим нативных контролов официально не документирован, но
// без него SysListView32 и SysTreeViewSys остаются светлыми на тёмной теме —
// то есть ровно тот случай, когда «аккуратный» интерфейс выглядит сломанным
// (SPEC §7). Берём и по имени (на свежих сборках uxtheme экспортирует эти
// функции под именами), и по ординалу (на старых есть только он), и каждый раз
// проверяем на nullptr.
//
// Что означает результат SetWindowTheme: функция возвращает не дескриптор темы,
// а признак успеха (ненулевое значение), поэтому проверять надо на nullptr, а не
// разбирать HRESULT.
using SetWindowThemeFn = void*(WINAPI*)(HWND, const wchar_t*, const wchar_t*);
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using SetPreferredAppModeFn = int(WINAPI*)(int);
using RefreshImmersiveColorPolicyFn = void(WINAPI*)();

struct UxthemeApi {
    SetWindowThemeFn setWindowTheme{nullptr};
    AllowDarkModeForWindowFn allowDarkModeForWindow{nullptr};
    SetPreferredAppModeFn setPreferredAppMode{nullptr};
    RefreshImmersiveColorPolicyFn refreshImmersiveColorPolicy{nullptr};
    bool darkModeAvailable{false};
};

UxthemeApi uxthemeApi() noexcept {
    static const auto resolved = []() -> UxthemeApi {
        UxthemeApi api;
        // LoadLibrary намеренно не вызываем: если uxtheme не загружена, тёмный
        // режим нативных контролов в этом процессе всё равно недоступен, а
        // загрузка DLL в обход списка зависимостей — лишнее требование к
        // конечному бинарнику (то же рассуждение, что и для dwmapi).
        const HMODULE module = ::GetModuleHandleW(L"uxtheme.dll");
        if (module == nullptr) return api;

        const auto resolve = [module](const char* name, WORD ordinal) -> FARPROC {
            if (const FARPROC byName = ::GetProcAddress(module, name); byName != nullptr) return byName;
            return ::GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
        };

        api.setWindowTheme = reinterpret_cast<SetWindowThemeFn>(resolve("SetWindowTheme", 0));
        api.refreshImmersiveColorPolicy =
            reinterpret_cast<RefreshImmersiveColorPolicyFn>(resolve("RefreshImmersiveColorPolicyState", 104));
        api.setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(resolve("SetPreferredAppMode", 135));
        api.allowDarkModeForWindow =
            reinterpret_cast<AllowDarkModeForWindowFn>(resolve("AllowDarkModeForWindow", 136));
        api.darkModeAvailable = api.allowDarkModeForWindow != nullptr || api.setWindowTheme != nullptr;
        return api;
    }();
    return resolved;
}

// ---------------------------------------------------------------------------
// Арифметика цвета
// ---------------------------------------------------------------------------

constexpr Color kWhite{0xFF, 0xFF, 0xFF, 0xFF};
constexpr Color kBlack{0x00, 0x00, 0x00, 0xFF};

// Акцент Windows 10/11 по умолчанию. Используется, только когда не ответил ни
// DWM, ни реестр, — то есть в самом худшем случае.
constexpr Color kDefaultAccent{0x00, 0x78, 0xD4, 0xFF};

// Минимум контраста подписи нативного контрола. WCAG AA для обычного текста,
// то же число, что у ensureContrast по умолчанию, — названо явно, потому что
// здесь контраст проверяется не «на глаз», а функцией.
constexpr double kTextContrast = 4.5;

// Смешивание: t = 0 отдаёт первый цвет, t = 1 — второй. Округление — до
// ближайшего целого, иначе восемь смешиваний дают заметную потерю точности в
// младших битах цвета.
Color mix(const Color& from, const Color& to, double t) noexcept {
    const auto channel = [t](int source, int target) -> std::uint8_t {
        const double value =
            static_cast<double>(source) + (static_cast<double>(target) - static_cast<double>(source)) * t;
        return static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0) + 0.5);
    };
    const auto alpha = [t](int source, int target) -> std::uint8_t {
        const double value =
            static_cast<double>(source) + (static_cast<double>(target) - static_cast<double>(source)) * t;
        return static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0) + 0.5);
    };
    return Color{channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b),
                 alpha(from.a, to.a)};
}

// Осветление/затемнение на долю: «hover» и «pressed» получаются одним приёмом,
// а не двумя наборами цветов на схему.
Color shade(const Color& color, double amount, bool lighter) noexcept {
    return mix(color, lighter ? kWhite : kBlack, amount);
}

// COLORREF из реестра/DWM: 0x00BBGGRR, красный в младшем байте, альфа 0.
Color fromColorRef(DWORD value) noexcept {
    return Color{static_cast<std::uint8_t>(value & 0xFFU), static_cast<std::uint8_t>((value >> 8) & 0xFFU),
                 static_cast<std::uint8_t>((value >> 16) & 0xFFU), 0xFF};
}

std::uint32_t clampDpi(unsigned dpi) noexcept {
    if (dpi < kMinDpi) return kMinDpi;
    if (dpi > kMaxDpi) return kMaxDpi;
    return dpi;
}

double clampFontScale(double scale) noexcept {
    if (scale < kMinFontScale) return kMinFontScale;
    if (scale > kMaxFontScale) return kMaxFontScale;
    return scale;
}

bool matchesSettingName(LPARAM value, const wchar_t* expected) noexcept {
    if (value == 0) return false;
    const auto text = reinterpret_cast<const wchar_t*>(value);
    // Сравнение без учёта регистра и без затрагивания локали: «AppsUseLightTheme»
    // и «appsuselighttheme» — одно и то же имя, а CompareStringOrdinal не
    // трогает состояние процесса, в отличие от CompareString/LCMapString.
    return ::CompareStringOrdinal(text, -1, expected, -1, TRUE) == CSTR_EQUAL;
}

} // namespace

// ---------------------------------------------------------------------------
// Имена
// ---------------------------------------------------------------------------

const char* toString(Mode mode) noexcept {
    switch (mode) {
    case Mode::Auto:
        return "auto";
    case Mode::Light:
        return "light";
    case Mode::Dark:
        return "dark";
    }
    return "auto";
}

const char* toString(Scheme scheme) noexcept {
    switch (scheme) {
    case Scheme::Light:
        return "light";
    case Scheme::Dark:
        return "dark";
    case Scheme::HighContrast:
        return "high-contrast";
    }
    return "light";
}

// ---------------------------------------------------------------------------
// Цвет
// ---------------------------------------------------------------------------

std::wstring toD2DColor(const Color& color) {
    return toD2DColor(color, false);
}

std::wstring toD2DColor(const Color& color, bool withAlpha) {
    static constexpr wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(withAlpha ? 9U : 7U);
    out.push_back(L'#');
    const int channels = withAlpha ? 4 : 3;
    const std::uint8_t values[4] = {withAlpha ? color.a : static_cast<std::uint8_t>(0xFF), color.r, color.g, color.b};
    for (int index = 0; index < channels; ++index) {
        const std::uint8_t value = values[index];
        out.push_back(kDigits[static_cast<std::size_t>(value >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(value & 0x0FU)]);
    }
    return out;
}

std::string toHexUtf8(const Color& color) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(7U);
    out.push_back('#');
    const std::uint8_t values[3] = {color.r, color.g, color.b};
    for (const std::uint8_t value : values) {
        out.push_back(kDigits[static_cast<std::size_t>(value >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(value & 0x0FU)]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Контраст
// ---------------------------------------------------------------------------

double relativeLuminance(const Color& color) noexcept {
    // Альфа игнорируется сознательно: все цвета палитры непрозрачные, а
    // полупрозрачный цвет считать нечем — результат зависит от того, что под ним.
    const auto linear = [](std::uint8_t raw) -> double {
        const double channel = static_cast<double>(raw) / 255.0;
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
}

double contrastRatio(const Color& first, const Color& second) noexcept {
    const double firstLuminance = relativeLuminance(first);
    const double secondLuminance = relativeLuminance(second);
    const double lighter = firstLuminance > secondLuminance ? firstLuminance : secondLuminance;
    const double darker = firstLuminance > secondLuminance ? secondLuminance : firstLuminance;
    return (lighter + 0.05) / (darker + 0.05);
}

Color readableOn(const Color& background, double minRatio) noexcept {
    const double onWhite = contrastRatio(kWhite, background);
    const double onBlack = contrastRatio(kBlack, background);
    if (onWhite >= minRatio && onWhite >= onBlack) return kWhite;
    if (onBlack >= minRatio) return kBlack;
    return onWhite >= onBlack ? kWhite : kBlack;
}

Color ensureContrast(const Color& foreground, const Color& background, double minRatio) noexcept {
    if (contrastRatio(foreground, background) >= minRatio) return foreground;
    // Двигаемся к тому краю спектра, который дальше от фона: на светлом фоне
    // текст темнеет, на тёмном — светлеет. Пытаться в другую сторону бессмысленно:
    // она только приближает текст к фону.
    const bool towardLight = relativeLuminance(background) < 0.5;
    const double step = 1.0 / static_cast<double>(kContrastSteps);
    Color candidate = foreground;
    for (int stepIndex = 1; stepIndex <= kContrastSteps; ++stepIndex) {
        candidate = mix(foreground, towardLight ? kWhite : kBlack, step * static_cast<double>(stepIndex));
        if (contrastRatio(candidate, background) >= minRatio) return candidate;
    }
    // Ни одна сторона не дотянула (фон того же тона, что и текст) — отдаём
    // крайний цвет: хоть что-то должно быть видно.
    return towardLight ? kWhite : kBlack;
}

// ---------------------------------------------------------------------------
// Палитра
// ---------------------------------------------------------------------------

Palette makePalette(Scheme scheme, const Color& accent) noexcept {
    Palette palette{};

    if (scheme == Scheme::HighContrast) {
        // Режим высокой контрастности: палитра — это ровно то, что выбрал
        // пользователь в настройках Windows. Ни одна «красивая» правка здесь
        // недопустима, поэтому модуль берёт цвета системы и не «улучшает» их
        // (иначе мы бы тихо вернули человеку то, что он отключил).
        palette.windowBackground = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_WINDOW)), 0, 0, 0xFF};
        palette.surface = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_BTNFACE)), 0, 0, 0xFF};
        palette.surfaceAlt = palette.surface;
        palette.surfaceHover = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_BTNHIGHLIGHT)), 0, 0, 0xFF};
        palette.surfaceSelected = palette.windowBackground;
        palette.border = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_3DDKSHADOW)), 0, 0, 0xFF};
        palette.divider = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_3DSHADOW)), 0, 0, 0xFF};
        palette.textPrimary = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_WINDOWTEXT)), 0, 0, 0xFF};
        palette.textSecondary = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_BTNTEXT)), 0, 0, 0xFF};
        palette.textDisabled = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_GRAYTEXT)), 0, 0, 0xFF};
        palette.accent = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_HIGHLIGHT)), 0, 0, 0xFF};
        palette.accentHover = palette.accent;
        palette.accentPressed = palette.accent;
        palette.textOnAccent = Color{static_cast<std::uint8_t>(::GetSysColor(COLOR_HIGHLIGHTTEXT)), 0, 0, 0xFF};
        palette.focusRing = palette.accent;
        // Семантические цвета в этом режиме вырождаются в цвет текста: иначе
        // «безопасный» пункт оказался бы единственным различимым, а это ровно
        // то, что режим высокой контрастности и отменяет.
        palette.success = palette.textPrimary;
        palette.warning = palette.textPrimary;
        palette.danger = palette.textPrimary;
        palette.riskSafe = palette.textPrimary;
        palette.riskReview = palette.textPrimary;
        palette.riskRisky = palette.textPrimary;
        return palette;
    }

    if (scheme == Scheme::Dark) {
        // Тёмная база. Значения — не инверсия светлых, а подбор: инвертированный
        // оранжевый акцент на тёмном фоне превращается в грязно-коричневый.
        palette.windowBackground = Color{0x20, 0x20, 0x20, 0xFF};
        palette.surface = Color{0x2B, 0x2B, 0x2B, 0xFF};
        palette.surfaceAlt = Color{0x26, 0x26, 0x26, 0xFF};
        palette.surfaceHover = Color{0x33, 0x33, 0x33, 0xFF};
        palette.border = Color{0x3D, 0x3D, 0x3D, 0xFF};
        palette.divider = Color{0x35, 0x35, 0x35, 0xFF};
        palette.textPrimary = Color{0xFF, 0xFF, 0xFF, 0xFF};
        palette.textSecondary = Color{0xC5, 0xC5, 0xC5, 0xFF};
        palette.textDisabled = Color{0x7A, 0x7A, 0x7A, 0xFF};
        palette.success = Color{0x6C, 0xCB, 0x5F, 0xFF};
        palette.warning = Color{0xFC, 0xE1, 0x00, 0xFF};
        palette.danger = Color{0xFF, 0x99, 0xA4, 0xFF};
    } else {
        // Светлая база.
        palette.windowBackground = Color{0xF3, 0xF3, 0xF3, 0xFF};
        palette.surface = Color{0xFF, 0xFF, 0xFF, 0xFF};
        palette.surfaceAlt = Color{0xFA, 0xFA, 0xFA, 0xFF};
        palette.surfaceHover = Color{0xEF, 0xEF, 0xEF, 0xFF};
        palette.border = Color{0xE5, 0xE5, 0xE5, 0xFF};
        palette.divider = Color{0xED, 0xED, 0xED, 0xFF};
        palette.textPrimary = Color{0x1A, 0x1A, 0x1A, 0xFF};
        palette.textSecondary = Color{0x5D, 0x5D, 0x5D, 0xFF};
        palette.textDisabled = Color{0x9E, 0x9E, 0x9E, 0xFF};
        palette.success = Color{0x0F, 0x7B, 0x0F, 0xFF};
        palette.warning = Color{0x9D, 0x5D, 0x00, 0xFF};
        palette.danger = Color{0xC4, 0x2B, 0x1C, 0xFF};
    }

    const bool dark = scheme == Scheme::Dark;

    // Акцент пользователя сначала, требования — потом. Акцент, прочитанный на
    // светлой теме, на тёмной может оказаться невидимым (классический случай:
    // тёмно-синий Windows 10 в тьме Windows 11), поэтому контраст акцента к фону
    // доводится до нетекстового минимума.
    palette.accent = ensureContrast(accent, palette.windowBackground, kContrastNonText);
    palette.accent = ensureContrast(palette.accent, palette.surface, kContrastNonText);
    // Наведение и нажатие — Toward читаемую сторону: на светлом это затемнение,
    // на тёмном осветление. Ошибка в этом месте (осветление на светлом) даёт
    // «кнопку, которая на вид исчезает под курсором».
    palette.accentHover = ensureContrast(shade(palette.accent, 0.12, dark), palette.surface, kContrastNonText);
    palette.accentPressed = ensureContrast(shade(palette.accent, 0.24, dark), palette.surface, kContrastNonText);
    palette.surfaceSelected = mix(palette.accent, palette.surface, dark ? 0.78 : 0.85);
    palette.textOnAccent = readableOn(palette.accent, kContrastSecondaryText);
    palette.focusRing = ensureContrast(palette.accent, palette.surface, kContrastNonText);

    // Текст проверяется против ВСЕХ подложек, на которых он реально лежит:
    // окно, карточка, «зебра» списка и выделенная строка. Проверка только
    // против фона окна — классическая ошибка, из-за которой текст в выделенной
    // строке пропадает.
    palette.textPrimary = ensureContrast(palette.textPrimary, palette.windowBackground, kContrastBodyText);
    palette.textPrimary = ensureContrast(palette.textPrimary, palette.surface, kContrastSecondaryText);
    palette.textPrimary = ensureContrast(palette.textPrimary, palette.surfaceAlt, kContrastSecondaryText);
    palette.textPrimary = ensureContrast(palette.textPrimary, palette.surfaceSelected, kContrastSecondaryText);

    palette.textSecondary = ensureContrast(palette.textSecondary, palette.windowBackground, kContrastSecondaryText);
    palette.textSecondary = ensureContrast(palette.textSecondary, palette.surface, kContrastSecondaryText);
    palette.textSecondary =
        ensureContrast(palette.textSecondary, palette.surfaceSelected, kContrastSecondaryText);
    palette.textDisabled = ensureContrast(palette.textDisabled, palette.surface, kContrastDisabledText);
    palette.textDisabled = ensureContrast(palette.textDisabled, palette.windowBackground, kContrastDisabledText);

    // Семантические цвета показываются и как текст («+3,2 ГБ освободится»), и как
    // заливка плашки, поэтому проверяем их как текст на карточке.
    palette.success = ensureContrast(palette.success, palette.surface, kContrastSecondaryText);
    palette.warning = ensureContrast(palette.warning, palette.surface, kContrastSecondaryText);
    palette.danger = ensureContrast(palette.danger, palette.surface, kContrastSecondaryText);

    // Иконка риска (§7.2) повторяет семантические цвета: отдельный оттенок для
    // «опасного» пункта означал бы вторую правду о том, что безопасно.
    palette.riskSafe = palette.success;
    palette.riskReview = palette.warning;
    palette.riskRisky = palette.danger;
    return palette;
}

Color riskColor(const Palette& palette, core::SafetyLevel level) noexcept {
    switch (level) {
    case core::SafetyLevel::Safe:
        return palette.riskSafe;
    case core::SafetyLevel::Review:
        return palette.riskReview;
    case core::SafetyLevel::Risky:
        return palette.riskRisky;
    }
    return palette.riskReview;  // неизвестный уровень риска показываем как «проверь»
}

// ---------------------------------------------------------------------------
// Нативные списки и деревья
// ---------------------------------------------------------------------------

Color nativeTextOn(const Color& foreground, const Color& background) noexcept {
    return ensureContrast(foreground, background, kTextContrast);
}

NativeRow nativeRow(const Palette& palette, bool selected, bool distinct, bool muted) noexcept {
    NativeRow row{};
    if (selected) {
        row.background = palette.surfaceSelected;
        // Подпись выделенной строки — тот же контраст, только против ЕЁ фона.
        // «Выделение» в тёмной схеме светлое, а в светлой тёмное, и подпись,
        // взятая вслепую, оказывается белым по белому ровно на той строке,
        // которую человек выбрал.
        row.text = nativeTextOn(palette.textOnAccent, row.background);
        return row;
    }
    row.background = distinct ? palette.surfaceAlt : palette.surface;
    // muted — это textSecondary, а НЕ textDisabled: у контраста WCAG для
    // недоступного элемента исключение («элемент недоступен по определению»),
    // а здесь недоступна строка, а не список, и смысл строки прочесть надо —
    // иначе в тёмной схеме она исчезает из ворот «текст в области строк»
    // ровно тем же способом, каким исчезала до этого (D-75).
    const Color wanted = muted ? palette.textSecondary : palette.textPrimary;
    row.text = nativeTextOn(wanted, row.background);
    return row;
}

NativeRow nativeHeader(const Palette& palette, bool pressed) noexcept {
    NativeRow header{};
    header.background = pressed ? palette.surfaceHover : palette.surfaceAlt;
    header.text = nativeTextOn(palette.textPrimary, header.background);
    return header;
}

void applyNativeColors(HWND control, NativeControl kind, const Palette& palette) noexcept {
    if (control == nullptr) return;
    // Тёмный режим нативному списку и дереву здесь НЕ включается — см. договор
    // в theme.hpp: подтема DarkMode_Explorer перекрывает фон, заданный
    // сообщением, и возвращает контролу системный цвет. Цвет фона строки при
    // этом всё равно задаётся один раз и для всех экранов — здесь, а не в
    // каждом view_*.cpp.
    const NativeRow base = nativeRow(palette, false, false, false);
    // Цвет идёт в lParam, а не в wParam: так его передают макросы
    // ListView_SetBkColor/SetTextColor/SetTextBkColor и TreeView_Set*Color в
    // commctrl.h. Отправка цвета в wParam — не «не сработало», а худший
    // вариант: lParam=0 это CLR_BLACK, и контрол честно заливал строки чёрным
    // (проверено: в тёмной и в светлой схеме область строк становилась
    // 0,0,0, а подпись — невидимой).
    if (kind == NativeControl::List) {
        ::SendMessageW(control, LVM_SETBKCOLOR, 0, static_cast<LPARAM>(colorRef(base.background)));
        // Фон ПОД ТЕКСТОМ — отдельное сообщение, и без него comctl32 рисует
        // подложку подписи системным цветом окна: в тёмной схеме это белая
        // полоса под каждой подписью поверх тёмных строк.
        ::SendMessageW(control, LVM_SETTEXTBKCOLOR, 0, static_cast<LPARAM>(colorRef(base.background)));
        ::SendMessageW(control, LVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(colorRef(base.text)));
    } else {
        ::SendMessageW(control, TVM_SETBKCOLOR, 0, static_cast<LPARAM>(colorRef(base.background)));
        ::SendMessageW(control, TVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(colorRef(base.text)));
        // Линии дерева — тоже из палитры: иначе в тёмной схеме они остаются
        // серыми системными и выглядят как чужая сетка поверх тёмной панели.
        ::SendMessageW(control, TVM_SETLINECOLOR, 0, static_cast<LPARAM>(colorRef(palette.divider)));
    }
    ::InvalidateRect(control, nullptr, FALSE);
}

void fillNativeRow(HDC dc, const RECT& box, const Color& background) noexcept {
    if (dc == nullptr) return;
    if (box.right <= box.left || box.bottom <= box.top) return;
    // Цвет фона строки на DC и цвет фона, заданный контролу сообщением, —
    // одно и то же число (nativeRow), поэтому заливка не может разойтись с
    // тем, что потом нарисует сам comctl32.
    HBRUSH brush = ::CreateSolidBrush(static_cast<COLORREF>(colorRef(background)));
    if (brush == nullptr) return;
    ::FillRect(dc, &box, brush);
    ::DeleteObject(brush);
}

void applyNativeHeaderTheme(HWND list, Scheme scheme) noexcept {
    if (list == nullptr) return;
    // В светлой схеме шапка системная и правильная: DarkMode_Explorer здесь
    // только сломал бы её, поэтому вызова нет.
    if (scheme != Scheme::Dark) return;
    HWND header =
        reinterpret_cast<HWND>(::SendMessageW(list, LVM_GETHEADER, static_cast<WPARAM>(0), static_cast<LPARAM>(0)));
    if (header == nullptr) return;
    // Шапка под темой и под подтемой «тёмного» рисуется через DrawThemeBackground
    // и не смотрит ни на WM_CTLCOLORHDR, ни на цвета DC — замерено: полоса
    // шапки оставалась 240,240,240 и при подтеме, и при ответе на
    // WM_CTLCOLORHDR. Единственный документированный рычаг — снять тему с самой
    // шапки (SetWindowTheme с пустой подтемой), после чего она рисуется
    // классически и берёт фон из ответа на WM_CTLCOLORHDR (см. childProc
    // экранов). В СВЕТЛОЙ схеме этого не делаем: там шапка системная, читаемая
    // и выглядит как у остальных окон Windows.
    const UxthemeApi api = uxthemeApi();
    if (api.setWindowTheme == nullptr) return;
    // Снятие темы — через тот же динамический uxtheme, что и остальные вызовы
    // модуля: статический импорт uxtheme.dll означал бы требование «эта DLL
    // есть» в каждом бинарнике (см. раздел 2 в шапке файла).
    (void)api.setWindowTheme(header, L"", L"");
}

bool nativeListItemRect(HWND list, int index, RECT& box) noexcept {
    if (list == nullptr || index < 0) return false;
    // LVM_GETITEMRECT: код «что вернуть» (LVIR_BOUNDS) макрос кладёт в
    // RECT.left, поэтому у ListView_GetItemRect четыре аргумента. Список
    // спрашиваем именно об этом прямоугольнике: у CDDS_ITEMPREPAINT он и есть
    // границы строки на экране.
    RECT request{};
    request.left = LVIR_BOUNDS;
    if (::SendMessageW(list, LVM_GETITEMRECT, static_cast<WPARAM>(index),
                       reinterpret_cast<LPARAM>(&request)) == FALSE) {
        return false;
    }
    box = request;
    return box.right > box.left && box.bottom > box.top;
}

// ---------------------------------------------------------------------------
// Шрифты и метрики
// ---------------------------------------------------------------------------

int FontDesc::pixelHeight(unsigned dpi) const noexcept {
    const double pixels = sizeDip * static_cast<double>(clampDpi(dpi)) / static_cast<double>(kReferenceDpi);
    return static_cast<int>(std::clamp(pixels + 0.5, 1.0, 4096.0));
}

LOGFONTW FontDesc::toLogFont(unsigned dpi) const noexcept {
    LOGFONTW font{};
    // Отрицательный lfHeight — высота знака в пикселях (положительный задал бы
    // высоту ячейки, и текст «прыгал» бы на полстроки от шрифта к шрифту).
    font.lfHeight = -pixelHeight(dpi);
    font.lfWeight = weight;
    font.lfItalic = italic ? TRUE : FALSE;
    // DEFAULT_CHARSET, а не RUSSIAN_CHARSET: набор символов выбирает локаль
    // процесса, и хардкод RUSSIAN_CHARSET ломал бы английский интерфейс и
    // иероглифы при смене языка (SPEC §5, локализация ru + en).
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfOutPrecision = OUT_TT_PRECIS;
    font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    font.lfQuality = CLEARTYPE_QUALITY;
    font.lfPitchAndFamily = fixedPitch ? static_cast<BYTE>(FIXED_PITCH | FF_MODERN)
                                       : static_cast<BYTE>(DEFAULT_PITCH | FF_SWISS);
    // LF_FACESIZE — константа 32, длиннее гарнитуры не бывает; _TRUNCATE вместо
    // ошибки, потому что это чистое отображение, а не проверка ввода.
    (void)::wcsncpy_s(font.lfFaceName, family.c_str(), _TRUNCATE);
    return font;
}

const FontDesc& Typography::forRole(FontRole role) const noexcept {
    switch (role) {
    case FontRole::Caption:
        return caption;
    case FontRole::Body:
        return body;
    case FontRole::BodyStrong:
        return bodyStrong;
    case FontRole::Subtitle:
        return subtitle;
    case FontRole::Title:
        return title;
    case FontRole::Metric:
        return metric;
    case FontRole::Mono:
        return mono;
    case FontRole::MonoStrong:
        return monoStrong;
    }
    return body;
}

int Metrics::dip(double value) const noexcept {
    return static_cast<int>(std::clamp(value * scale + 0.5, -32768.0, 32767.0));
}

double Metrics::undo(int pixels) const noexcept {
    return static_cast<double>(pixels) / scale;
}

Metrics metricsForDpi(unsigned dpi, double fontScale) noexcept {
    Metrics metrics;
    metrics.dpi = clampDpi(dpi == 0 ? dpiForSystem() : dpi);
    metrics.scale = static_cast<double>(metrics.dpi) / static_cast<double>(kReferenceDpi);
    metrics.fontScale = clampFontScale(fontScale);
    return metrics;
}

namespace {

// Системный UI-шрифт под конкретный DPI. Гарнитура и начертание — из
// SPI_GETNONCLIENTMETRICS, то есть ровно те, что выбрал пользователь в настройках
// Windows; кегль возвращается в DIP, чтобы сравнить его с нашим масштабом.
struct SystemFont {
    std::wstring family{L"Segoe UI"};
    double sizeDip{kReferenceSystemFontDip};
    int weight{400};
    bool fromSystem{false};
};

SystemFont readSystemFont(unsigned dpi) noexcept {
    SystemFont font;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    // Второй параметр SystemParametersInfoForDpi для SPI_GETNONCLIENTMETRICS — размер
    // системного шрифта в пикселях для запрошенного DPI; девять пунктов при 96 DPI
    // дают ровно двенадцать пикселей, то есть тот самый системный Segoe UI.
    const UINT uiFontSize = static_cast<UINT>(
        ::MulDiv(9, static_cast<int>(clampDpi(dpi)), 72));
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, uiFontSize, &metrics, 0, clampDpi(dpi)) != FALSE &&
        metrics.lfMessageFont.lfFaceName[0] != L'\0') {
        font.family = metrics.lfMessageFont.lfFaceName;
        if (metrics.lfMessageFont.lfWeight > 0) font.weight = metrics.lfMessageFont.lfWeight;
        font.fromSystem = true;
        // lfHeight отрицателен и равен высоте знака в пикселях уже под нужным DPI.
        const int pixels = metrics.lfMessageFont.lfHeight < 0 ? -metrics.lfMessageFont.lfHeight
                                                              : metrics.lfMessageFont.lfHeight;
        if (pixels > 0) {
            const double dip = static_cast<double>(pixels) * static_cast<double>(kReferenceDpi) /
                               static_cast<double>(clampDpi(dpi));
            if (dip > 0.0) font.sizeDip = dip;
        }
    }
    return font;
}

} // namespace

Typography makeTypography(unsigned dpi, double fontScale) noexcept {
    const unsigned effectiveDpi = clampDpi(dpi == 0 ? dpiForSystem() : dpi);
    const SystemFont system = readSystemFont(effectiveDpi);

    // Системный кегль — не наш размер текста, а кегль заголовка окна (9 pt).
    // Но если пользователь увеличил масштаб текста в настройках Windows, его
    // выбор — это тоже запрос доступности, и он должен дойти до интерфейса:
    // поэтому системный кегль входит множителем, а не выбрасывается.
    double systemRatio = 1.0;
    if (system.fromSystem && system.sizeDip > 0.0) {
        systemRatio = system.sizeDip / kReferenceSystemFontDip;
    }
    const double scale = std::clamp(systemRatio, 0.8, 2.0) * clampFontScale(fontScale);

    const auto sized = [&system, scale](const wchar_t* family, double sizeDip, int weight, bool italic,
                                        bool fixedPitch) {
        FontDesc desc;
        desc.family = family;
        desc.sizeDip = sizeDip * scale;
        desc.weight = weight;
        desc.italic = italic;
        desc.fixedPitch = fixedPitch;
        return desc;
    };

    // Моноширинная гарнитура для чисел. Системного моноширинного шрифта у
    // Windows нет, а столбики цифр в карте разделов и в таблице размеров обязаны
    // стоять (SPEC §7: карта разделов, «крупная цифра сверху»). Consolas есть в
    // Windows 10 22H2 — это наш пол (§5), а если гарнитуры не окажется,
    // DirectWrite подставит свою замену, и это штатный механизм, а не ошибка.
    static constexpr wchar_t kMonoFamily[] = L"Consolas";

    Typography type;
    type.caption = sized(system.family.c_str(), 12.0, system.weight, false, false);
    type.body = sized(system.family.c_str(), 14.0, system.weight, false, false);
    type.bodyStrong = sized(system.family.c_str(), 14.0, 600, false, false);
    type.subtitle = sized(system.family.c_str(), 18.0, system.weight, false, false);
    type.title = sized(system.family.c_str(), 24.0, 600, false, false);
    type.metric = sized(system.family.c_str(), 32.0, 600, false, false);
    type.mono = sized(kMonoFamily, 13.0, 400, false, true);
    type.monoStrong = sized(kMonoFamily, 13.0, 600, false, true);
    return type;
}

// ---------------------------------------------------------------------------
// Системные источники
// ---------------------------------------------------------------------------

Mode storedMode() noexcept {
    std::wstring text;
    if (readString(HKEY_CURRENT_USER, kUiSettingsKey, kThemeModeValue, text) != RegistryRead::Ok) {
        return Mode::Auto;
    }
    if (text == L"light") return Mode::Light;
    if (text == L"dark") return Mode::Dark;
    return Mode::Auto;  // неизвестное значение — то же, что отсутствие
}

bool storeMode(Mode mode) noexcept {
    const wchar_t* name = L"auto";
    switch (mode) {
    case Mode::Auto:
        name = L"auto";
        break;
    case Mode::Light:
        name = L"light";
        break;
    case Mode::Dark:
        name = L"dark";
        break;
    }
    return writeSetting(HKEY_CURRENT_USER, kUiSettingsKey, kThemeModeValue, name);
}

unsigned storedFontScalePercent() noexcept {
    DWORD value = 100;
    if (readDword(HKEY_CURRENT_USER, kUiSettingsKey, kFontScaleValue, value) != RegistryRead::Ok) return 100;
    if (value < static_cast<DWORD>(kMinFontScale * 100.0)) return static_cast<unsigned>(kMinFontScale * 100.0);
    if (value > static_cast<DWORD>(kMaxFontScale * 100.0)) return static_cast<unsigned>(kMaxFontScale * 100.0);
    return static_cast<unsigned>(value);
}

bool storeFontScalePercent(unsigned percent) noexcept {
    const double clamped = std::clamp(static_cast<double>(percent) / 100.0, kMinFontScale, kMaxFontScale);
    return writeSetting(HKEY_CURRENT_USER, kUiSettingsKey, kFontScaleValue,
                        static_cast<DWORD>(clamped * 100.0 + 0.5));
}

Scheme systemScheme() noexcept {
    // Порядок — по важности для пользователя, а не по порядку вызовов.
    // Высокая контрастность — режим доступности, включённый сознательно, и он
    // перебивает всё остальное; AppUseLightTheme в этот момент может вообще
    // говорить о другом (оболочка в режиме контраста выставляет свою схему).
    HIGHCONTRASTW highContrast{};
    highContrast.cbSize = sizeof(highContrast);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) != FALSE &&
        (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0U) {
        return Scheme::HighContrast;
    }

    DWORD lightTheme = 1;
    if (readDword(HKEY_CURRENT_USER, kPersonalizeKey, kAppsUseLightTheme, lightTheme) == RegistryRead::Ok) {
        return lightTheme != 0 ? Scheme::Light : Scheme::Dark;
    }
    // Windows 11 24H2 и новее: часть новых установок не пишет AppsUseLightTheme,
    // но пишет SystemUsesLightTheme. Отсутствие обоих ключей — не ошибка, а
    // «тема не задана»: берём светлую, как это делает сама Windows.
    DWORD shellLightTheme = 1;
    if (readDword(HKEY_CURRENT_USER, kPersonalizeKey, kSystemUsesLightTheme, shellLightTheme) ==
        RegistryRead::Ok) {
        return shellLightTheme != 0 ? Scheme::Light : Scheme::Dark;
    }
    return Scheme::Light;
}

Color systemAccent() noexcept {
    if (const auto resolve = dwmColorizationColor(); resolve != nullptr) {
        DWORD colorization = 0;
        BOOL active = FALSE;
        // SUCCEEDED — макрос, а не функция, поэтому его нельзя вызвать с
        // разделителем областей: «::SUCCEEDED(...)» не компилируется. От HRESULT
        // DwmGetColorizationColor отказа не бывает (функция возвращает S_OK), но
        // проверка лишней не будет: она снимает вопрос «а если DWM занят».
        if (SUCCEEDED(resolve(&colorization, &active)) && active != FALSE) {
            return fromColorRef(colorization);
        }
    }
    // Реестровый запасной путь: он же основной, если dwmapi недоступен (сборка
    // без оболочки, старый сервер, диагностика).
    DWORD colorization = 0;
    if (readDword(HKEY_CURRENT_USER, kPersonalizeKey, kColorizationColor, colorization) == RegistryRead::Ok) {
        return fromColorRef(colorization);
    }
    if (readDword(HKEY_CURRENT_USER, kDwmKey, kColorizationColor, colorization) == RegistryRead::Ok) {
        return fromColorRef(colorization);
    }
    return kDefaultAccent;
}

unsigned dpiForSystem() noexcept {
    if (const UINT dpi = ::GetDpiForSystem(); dpi != 0) return clampDpi(dpi);
    // GetDpiForSystem есть начиная с 1607 и в нашей целевой ОС есть всегда, но
    // ветка нужна для тестовых сборок и для не-DPI-aware окружения: десктопный
    // DC даёт тот же ответ и не требует контекста осведомлённости.
    if (const HDC desktop = ::GetDC(nullptr); desktop != nullptr) {
        const int dpi = ::GetDeviceCaps(desktop, LOGPIXELSX);
        ::ReleaseDC(nullptr, desktop);
        if (dpi > 0) return clampDpi(static_cast<unsigned>(dpi));
    }
    return kReferenceDpi;
}

unsigned dpiForWindow(HWND window) noexcept {
    if (window == nullptr) return dpiForSystem();
    if (const UINT dpi = ::GetDpiForWindow(window); dpi != 0) return clampDpi(dpi);
    return dpiForSystem();
}

bool enableDarkModeForWindow(HWND window, Scheme scheme) noexcept {
    if (window == nullptr) return false;
    const UxthemeApi api = uxthemeApi();
    if (!api.darkModeAvailable) return false;

    // Режим приложения, а не системы: пользователь в настройках MrProper мог
    // выбрать тёмную тему на светлой системе, и нативные контролы обязаны
    // соответствовать палитре, иначе окно располовинится.
    const bool wantDark = scheme == Scheme::Dark;
    if (api.refreshImmersiveColorPolicy != nullptr) api.refreshImmersiveColorPolicy();
    if (api.setPreferredAppMode != nullptr) {
        // 0 — Default, 1 — AllowDark, 2 — ForceDark. Ставим AllowDark, а не
        // ForceDark: принудительная тьма перекрашивает и то, что тема уже
        // покрасила сама, и ломает собственные цвета нашей палитры.
        api.setPreferredAppMode(wantDark ? 1 : 0);
    }
    if (api.allowDarkModeForWindow != nullptr) api.allowDarkModeForWindow(window, wantDark);
    if (api.setWindowTheme != nullptr) {
        // Документированная часть: подтема DarkMode_Explorer переключает цвета
        // нативного контрола. nullptr означает «подтема не применилась» — это
        // отказ, а не повод ронять отрисовку, поэтому он возвращается наружу, и
        // вызывающий просто рисует контрол как умеет.
        const void* applied = api.setWindowTheme(window, wantDark ? L"DarkMode_Explorer" : nullptr, nullptr);
        if (applied == nullptr) return false;
    }
    return true;
}

Change classifyMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    switch (message) {
    case WM_SETTINGCHANGE:
        // wParam при смене режима высокой контрастности равен коду действия SPI.
        if (wParam == SPI_SETHIGHCONTRAST) return Change::Scheme;
        if (matchesSettingName(lParam, kAppsUseLightTheme) || matchesSettingName(lParam, kSystemUsesLightTheme) ||
            matchesSettingName(lParam, L"ImmersiveColorSet") ||
            matchesSettingName(lParam, L"ColorizationColor") || matchesSettingName(lParam, L"TrayApps")) {
            return Change::Scheme;
        }
        // Смена шрифта или метрик системы меняет нашу типографику, но не палитру.
        if (matchesSettingName(lParam, L"NonClientMetrics") || matchesSettingName(lParam, L"WindowsThemeElement")) {
            return Change::Typography;
        }
        return Change::None;
    case WM_THEMECHANGED:  // тема оформления пересобрана
    case WM_SYSCOLORCHANGE: // системные цвета изменились (в т. ч. высокая контрастность)
        return Change::Scheme;
    case WM_DPICHANGED:
        return Change::Dpi;
    default:
        return Change::None;
    }
}

// ---------------------------------------------------------------------------
// Контекст темы
// ---------------------------------------------------------------------------

Theme::Theme() {
    mode_ = storedMode();
    fontScalePercent_ = storedFontScalePercent();
    applyScheme();
    applyTypography(dpiForSystem());
}

Theme::Theme(HWND window) : Theme() {
    setDpi(dpiForWindow(window));
}

// Применяет выбор пользователя к системе. Высокая контрастность перебивает и
// Light, и Dark: этот режим включают не для красоты, а потому что иначе текст
// не читается, и «тёмная» в настройках при включённой контрастности означает
// «сделай как в системе». Пользовательский выбор, наоборот, перебивает
// системную схему: иначе переключатель темы в настройках ничего не делал бы.
void Theme::applyScheme() noexcept {
    scheme_ = systemScheme();
    if (scheme_ != Scheme::HighContrast) {
        if (mode_ == Mode::Light) {
            scheme_ = Scheme::Light;
        } else if (mode_ == Mode::Dark) {
            scheme_ = Scheme::Dark;
        }
    }
    palette_ = makePalette(scheme_, systemAccent());
}

void Theme::applyTypography(unsigned dpi) noexcept {
    metrics_ = metricsForDpi(dpi, static_cast<double>(fontScalePercent_) / 100.0);
    typography_ = makeTypography(metrics_.dpi, metrics_.fontScale);
}

void Theme::reload() noexcept {
    mode_ = storedMode();
    fontScalePercent_ = storedFontScalePercent();
    applyScheme();
    applyTypography(metrics_.dpi);
}

bool Theme::applyMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    switch (classifyMessage(message, wParam, lParam)) {
    case Change::None:
        return false;
    case Change::Scheme:
        reload();
        return true;
    case Change::Typography:
        applyTypography(metrics_.dpi);
        return true;
    case Change::Dpi:
        // Новый DPI приходит в младшем слове wParam. Окно при этом должен
        // переехать и изменить размер по прямоугольнику из lParam — это работа
        // app_shell (SPEC §5, §6.4): тема сообщает числа, окно двигает себя.
        setDpi(static_cast<unsigned>(LOWORD(wParam)));
        return true;
    }
    return false;
}

bool Theme::setMode(Mode mode) noexcept {
    if (mode_ == mode) return false;
    // Сохраняем ДО применения и не откатываем при отказе: если реестр недоступен
    // (ключ запрещён политикой, профиль без записи), тема всё равно должна
    // переключиться в текущем сеансе, иначе кнопка «тёмная» выглядит сломанной.
    // Отказ записан в лог внутри writeSetting, и при следующем запуске режим
    // вернётся к системному — это ожидаемо и заметно пользователю.
    if (!storeMode(mode)) {
        // Отказ описан в комментарии выше и уже записан в лог внутри storeMode;
        // здесь только фиксируем, что выбор действует в этом сеансе.
        core::logDebug("ui.theme.mode", "выбор темы применён в сеансе, но не сохранён",
                       core::LogFields{core::logField("mode", std::string(toString(mode)))});
    }
    mode_ = mode;
    applyScheme();
    return true;
}

bool Theme::setFontScalePercent(unsigned percent) noexcept {
    const double clamped = std::clamp(static_cast<double>(percent) / 100.0, kMinFontScale, kMaxFontScale);
    const unsigned rounded = static_cast<unsigned>(clamped * 100.0 + 0.5);
    if (rounded == fontScalePercent_) return false;
    fontScalePercent_ = rounded;
    if (!storeFontScalePercent(rounded)) {
        core::logDebug("ui.theme.fontscale", "масштаб текста применён в сеансе, но не сохранён",
                       core::LogFields{core::logField("percent", static_cast<long long>(rounded))});
    }
    applyTypography(metrics_.dpi);
    return true;
}

void Theme::setDpi(unsigned dpi) noexcept {
    const unsigned effective = clampDpi(dpi == 0 ? dpiForSystem() : dpi);
    if (effective == metrics_.dpi) return;
    applyTypography(effective);
}

Mode Theme::mode() const noexcept {
    return mode_;
}

Scheme Theme::scheme() const noexcept {
    return scheme_;
}

const Palette& Theme::palette() const noexcept {
    return palette_;
}

const Typography& Theme::typography() const noexcept {
    return typography_;
}

const Metrics& Theme::metrics() const noexcept {
    return metrics_;
}

unsigned Theme::fontScalePercent() const noexcept {
    return fontScalePercent_;
}

const FontDesc& Theme::font(FontRole role) const noexcept {
    return typography_.forRole(role);
}

Metrics Theme::metricsFor(HWND window) const noexcept {
    return metricsForDpi(dpiForWindow(window), metrics_.fontScale);
}

Typography Theme::typographyFor(HWND window) const {
    const Metrics target = metricsFor(window);
    return makeTypography(target.dpi, target.fontScale);
}

FontDesc Theme::fontFor(FontRole role, HWND window) const {
    // Окно обычно одно, и его DPI совпадает с metrics_, поэтому быстрый путь не
    // трогает реестр и SystemParametersInfoForDpi: пересчёт кеглей нужен только
    // в момент перетаскивания между мониторами.
    if (dpiForWindow(window) == metrics_.dpi) return typography_.forRole(role);
    return typographyFor(window).forRole(role);
}

} // namespace mrproper::ui::theme
