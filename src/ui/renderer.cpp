// MrProper — Direct2D-рендерер с откатом на GDI. Спека и обоснование границ
// модуля — в renderer.hpp; здесь реализация.
//
// Файл разложен на три слоя, у каждого своё правило:
//
//   1) анонимный namespace: ComPtr, переводы типов, создание устройств
//      (D3D11 → D2D → DirectWrite), probe и разбиение текста на строки для
//      GDI. Всё, что не зависит от окна, живёт здесь;
//   2) Renderer::Impl: состояние (устройства, поверхности, кэш кистей и
//      шрифтов, DPI, последняя ошибка) и операции кадра;
//   3) публичные методы Renderer: проверка аргументов и переадресация в Impl.
//
// Чего в файле нет намеренно: обработки оконных сообщений (app_shell,
// задача 66), палитры (ui::Theme, 68), строк локализации (ui::Locale, 69),
// содержимого экранов (71-74). Рендерер не знает, что именно рисует, —
// поэтому его можно проверить на пустом окне, а экран можно написать, не
// думая о том, Direct2D это или GDI.
#include "renderer.hpp"

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "platform/win_error.hpp"

namespace mrproper::ui::render {
namespace {

// ---------------------------------------------------------------------------
// ComPtr
// ---------------------------------------------------------------------------
//
// Минимальный владелец COM-интерфейсов. WRL из Windows SDK подошёл бы, но
// лишняя зависимость от _COM_Outptr_ аннотаций и SAL того же SDK здесь
// ничего не даёт: нужен ровно Release в деструкторе и QueryInterface для
// приведения к производному интерфейсу. Смысл тот же, что у
// platform::unique_handle (SPEC §9.1 ADR-001, «RAII-обёртки»): забытый
// Release на каждом кадре — утечка, которая в приложении с анимацией
// прогресса накапливается за минуты работы.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;

    explicit ComPtr(T* pointer) noexcept
        : pointer_(pointer) {
    }

    ~ComPtr() {
        reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset(other.pointer_);
            other.pointer_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] T* get() const noexcept {
        return pointer_;
    }

    [[nodiscard]] T* operator->() const noexcept {
        return pointer_;
    }

    explicit operator bool() const noexcept {
        return pointer_ != nullptr;
    }

    // Адрес для CreateXxx(&out): прежнее значение освобождается, чтобы COM
    // не получил неинициализированный адрес.
    [[nodiscard]] T** put() noexcept {
        reset();
        return &pointer_;
    }

    T* release() noexcept {
        T* const pointer = pointer_;
        pointer_ = nullptr;
        return pointer;
    }

    void reset(T* pointer = nullptr) noexcept {
        if (pointer_ == pointer) return;
        if (pointer_ != nullptr) pointer_->Release();
        pointer_ = pointer;
    }

    // Приведение к производному интерфейсу: ID3D11Device → IDXGIDevice.
    // IID_PPV_ARGS сам вызывает AddRef, поэтому out обязан быть пустым.
    template <typename U>
    [[nodiscard]] HRESULT query(U** out) const noexcept {
        if (pointer_ == nullptr) return E_POINTER;
        return pointer_->QueryInterface(IID_PPV_ARGS(out));
    }

private:
    T* pointer_{};
};

// ---------------------------------------------------------------------------
// Переводы типов
// ---------------------------------------------------------------------------

template <typename T, std::size_t N>
[[nodiscard]] constexpr UINT countOf(const T (&)[N]) noexcept {
    return static_cast<UINT>(N);
}

// Канал цвета 0..1 → байт. Значение вне диапазона обрезается, а не
// оборачивается: ошибка в теме (альфа -0.2) при обёртывании дала бы 255 —
// белый, и «полупрозрачная» подложка стала бы заливкой во весь экран.
[[nodiscard]] BYTE toChannel(float value) noexcept {
    const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
    return static_cast<BYTE>(static_cast<int>(clamped * 255.0F + 0.5F));
}

[[nodiscard]] D2D1_COLOR_F toD2dColor(const Color& color) noexcept {
    return D2D1_COLOR_F{color.red, color.green, color.blue, color.alpha};
}

[[nodiscard]] COLORREF toColorRef(const Color& color) noexcept {
    return RGB(toChannel(color.red), toChannel(color.green), toChannel(color.blue));
}

[[nodiscard]] D2D1_RECT_F toD2dRect(const Rect& rect) noexcept {
    return D2D1_RECT_F{rect.left, rect.top, rect.right, rect.bottom};
}

// DIP → пиксели с округлением: GDI не умеет субпиксельную заливку, а
// «полпикселя» в прямоугольнике означают рамку разной ширины слева и
// справа — это видно на карте разделов (FR-2) в первую очередь.
[[nodiscard]] int toPixels(float dip, float dpi) noexcept {
    return static_cast<int>(std::lround(dip * dpi / kDipsPerInch));
}

[[nodiscard]] RECT toPixelsRect(const Rect& rect, float dpiX, float dpiY) noexcept {
    RECT out{};
    out.left = toPixels(rect.left, dpiX);
    out.top = toPixels(rect.top, dpiY);
    out.right = toPixels(rect.right, dpiX);
    out.bottom = toPixels(rect.bottom, dpiY);
    return out;
}

[[nodiscard]] float sanitizeFontSize(float sizeDip) noexcept {
    // Меньше 1 DIP не различимо: это ошибка в теме, а не замысел, и такой
    // текст всё равно нечитаем. Нижняя граница защищает и от деления на ноль
    // при расчёте lfHeight.
    return sizeDip < 1.0F ? 1.0F : sizeDip;
}

[[nodiscard]] DWRITE_FONT_WEIGHT toDwriteWeight(int weight) noexcept {
    // Диапазон DWRITE_FONT_WEIGHT — 1..950, вне его CreateTextFormat вернёт
    // E_INVALIDARG. Приводим к границам, а не отдаём мусор.
    if (weight < 1) return DWRITE_FONT_WEIGHT_NORMAL;
    if (weight > 950) return DWRITE_FONT_WEIGHT_EXTRA_BLACK;
    return static_cast<DWRITE_FONT_WEIGHT>(weight);
}

[[nodiscard]] LONG toGdiWeight(int weight) noexcept {
    if (weight < 1) return FW_NORMAL;
    if (weight > 1000) return FW_BLACK;
    return static_cast<LONG>(weight);
}

// Локаль пользователя для DirectWrite. Нужна, чтобы правила подстановки и
// ширина цифр считались по языку интерфейса, а не по языку системы (§5:
// «даты/числа через GetLocaleInfoEx»).
[[nodiscard]] std::wstring userLocaleName() {
    const int needed = ::GetUserDefaultLocaleName(nullptr, 0);
    if (needed <= 0) return L"en-us";
    std::wstring locale(static_cast<std::size_t>(needed), L'\0');
    const int written = ::GetUserDefaultLocaleName(locale.data(), needed);
    if (written <= 0) return L"en-us";
    locale.resize(static_cast<std::size_t>(written));
    return locale;
}

// Шрифт по умолчанию — явным именем, а не пустой строкой: пустая строка у
// GDI означает «шрифт текущего DC», то есть нечто, зависящее от того, какой
// DC первым попался. Segoe UI присутствует на всех Windows 10/11 (§5), а на
// локалях без него CreateFontIndirect подставит ближайший гарнитурный
// аналог — это лучше, чем пустое имя.
[[nodiscard]] const wchar_t* defaultFamilyName() noexcept {
    return L"Segoe UI";
}

[[nodiscard]] std::wstring familyOf(const FontSpec& spec) {
    if (spec.family.empty()) return defaultFamilyName();
    return platform::toUtf16(spec.family);
}

[[nodiscard]] std::wstring localeOf(const FontSpec& spec) {
    if (spec.localeName.empty()) return userLocaleName();
    return platform::toUtf16(spec.localeName);
}

[[nodiscard]] float verticalFactor(TextVerticalAlign align) noexcept {
    switch (align) {
        case TextVerticalAlign::Top:
            return 0.0F;
        case TextVerticalAlign::Center:
            return 0.5F;
        case TextVerticalAlign::Bottom:
            return 1.0F;
    }
    return 0.0F;
}

// Выравнивание абзаца в DirectWrite. Для RTL «логическое» выравнивание
// переворачивается: иначе Center остался бы по центру, а Leading уехал бы
// вправо, и текст в арабском читался бы с другого края (§5, RTL-ready).
[[nodiscard]] DWRITE_TEXT_ALIGNMENT dwriteAlignment(const TextOptions& options) noexcept {
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (options.horizontal) {
        case TextAlign::Leading:
            alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
            break;
        case TextAlign::Center:
            alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
            break;
        case TextAlign::Trailing:
            alignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
            break;
    }
    if (options.rightToLeft) {
        if (alignment == DWRITE_TEXT_ALIGNMENT_LEADING) alignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
        else if (alignment == DWRITE_TEXT_ALIGNMENT_TRAILING) alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    }
    return alignment;
}

// ---------------------------------------------------------------------------
// Feature-level и создание устройств
// ---------------------------------------------------------------------------

// Снизу вверх. 11_1 запрошен первым: приложению нужен только базовый D2D 1.1,
// но на 11_1 flip-модельный путь работает без оговорок. Уровень, который
// вернул D3D11CreateDevice, попадает в FeatureProbe и в лог: «Direct2D есть,
// но на 10_0» — полезно при разборе артефактов на старых видеокартах.
constexpr D3D_FEATURE_LEVEL kRequestedLevels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
};

// Ширина «без ограничения» для раскладки DirectWrite. Не 0: у раскладки 0
// означает «переносить по любой ширине», а нам нужен именно однострочный
// замер без переноса.
constexpr float kUnlimitedWidth = 1.0e6F;

struct GraphicsCreation {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIDevice> dxgiDevice;
    FeatureProbe probe;
};

// Перебор драйверов D3D11. Порядок — не «аппаратный путь любой ценой», а §10:
// старый или сломанный драйвер не должен превращать приложение в чёрное
// окно, поэтому за аппаратным идёт программный, и только потом GDI.
bool createD3D11Device(const RendererOptions& options, GraphicsCreation& out) {
    struct Attempt {
        D3D_DRIVER_TYPE type;
        DriverKind kind;
    };

    Attempt attempts[3];
    std::size_t attemptCount = 0;
    if (options.preferHardware) {
        attempts[attemptCount] = Attempt{D3D_DRIVER_TYPE_HARDWARE, DriverKind::Hardware};
        ++attemptCount;
    }
    if (options.allowWarpFallback) {
        attempts[attemptCount] = Attempt{D3D_DRIVER_TYPE_WARP, DriverKind::Warp};
        ++attemptCount;
    }
    // Программный растеризатор D3D11 — последняя попытка до GDI. Поддержан не
    // на всех сборках (слои D3D11 SDK могут отсутствовать), и тогда попытка
    // вернёт ошибку, которая попадёт в probe.note.
    attempts[attemptCount] = Attempt{D3D_DRIVER_TYPE_REFERENCE, DriverKind::Reference};
    ++attemptCount;

    std::string reasons;
    for (std::size_t index = 0; index < attemptCount; ++index) {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_9_1;

        // D3D11_CREATE_DEVICE_BGRA_SUPPORT — не формальность: без этого флага
        // D2D поверх D3D11 не работает вообще, и D3D11CreateDevice возвращает
        // E_INVALIDARG. Флаг одновременно служит проверкой «умеет ли этот
        // драйвер Direct2D».
        const HRESULT hr = ::D3D11CreateDevice(nullptr, attempts[index].type, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, kRequestedLevels,
                                               countOf(kRequestedLevels), D3D11_SDK_VERSION, device.put(), &achieved,
                                               context.put());
        if (FAILED(hr)) {
            if (!reasons.empty()) reasons += "; ";
            reasons += driverKindName(attempts[index].kind);
            reasons += ": ";
            reasons += platform::hresultErrorText(hr);
            continue;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device.query(dxgiDevice.put())) || !dxgiDevice) {
            if (!reasons.empty()) reasons += "; ";
            reasons += driverKindName(attempts[index].kind);
            reasons += ": IDXGIDevice недоступен";
            continue;
        }

        out.probe.d3d11Available = true;
        out.probe.bgraSupport = true;
        out.probe.driver = attempts[index].kind;
        out.probe.hardwareAcceleration = attempts[index].kind == DriverKind::Hardware;
        out.probe.featureLevelMajor = static_cast<int>(achieved >> 12);
        out.probe.featureLevelMinor = static_cast<int>((achieved >> 8) & 0x0FU);
        out.probe.note = "D3D11: ";
        out.probe.note += driverKindName(attempts[index].kind);
        out.device = std::move(device);
        out.context = std::move(context);
        out.dxgiDevice = std::move(dxgiDevice);

        // Описание адаптера. Пустое на программных драйверах — норма, ошибкой
        // не считаем. Версии строкового драйвера здесь нет: в установленном
        // SDK описание с DriverVersion появляется только в DXGI_ADAPTER_DESC2,
        // которого нет в заголовках, — поэтому в лог идут имя адаптера и
        // идентификаторы, а версию драйвера при необходимости смотрят в
        // dxdiag.
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(out.dxgiDevice->GetAdapter(adapter.put())) && adapter) {
            DXGI_ADAPTER_DESC description{};
            if (SUCCEEDED(adapter->GetDesc(&description))) {
                out.probe.adapterName = platform::toUtf8(description.Description);
                out.probe.vendorId = description.VendorId;
                out.probe.deviceId = description.DeviceId;
            }
        }
        return true;
    }

    out.probe.note = "D3D11 недоступен";
    if (!reasons.empty()) {
        out.probe.note += ": ";
        out.probe.note += reasons;
    }
    return false;
}

// D2D-фабрика однопоточная: рендер живёт в UI-потоке (§6.4), а
// D2D1_FACTORY_TYPE_MULTI_THREADED дороже без всякой пользы.
bool createD2DFactory(ComPtr<ID2D1Factory1>& factory) {
    const HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                           reinterpret_cast<void**>(factory.put()));
    return SUCCEEDED(hr) && static_cast<bool>(factory);
}

// D2D-устройство поверх DXGI-устройства. Это и есть настоящая проверка
// «умеет ли машина Direct2D»: фабрика создаётся где угодно, а CreateDevice
// поверх конкретного адаптера может отказаться.
bool createD2DDevice(const ComPtr<IDXGIDevice>& dxgiDevice, ComPtr<ID2D1Device>& device,
                     ComPtr<ID2D1DeviceContext>& context) {
    if (!dxgiDevice) return false;

    ComPtr<ID2D1Factory1> factory;
    if (!createD2DFactory(factory)) return false;

    ComPtr<ID2D1Device> newDevice;
    if (FAILED(factory->CreateDevice(dxgiDevice.get(), newDevice.put())) || !newDevice) return false;

    ComPtr<ID2D1DeviceContext> newContext;
    if (FAILED(newDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, newContext.put())) || !newContext) {
        return false;
    }

    device = std::move(newDevice);
    context = std::move(newContext);
    return true;
}

bool createDirectWrite(ComPtr<IDWriteFactory>& factory) {
    // SHARED, а не SINGLE_THREADED: фабрика DWrite кэширует наборы шрифтов, и
    // разделяемый вариант заметно быстрее при создании десятков шрифтов темы
    // (§5: тема переключается без перезапуска).
    // DWriteCreateFactory принимает именно IUnknown** (в отличие от
    // D2D1CreateFactory, где void**), поэтому приведение здесь явное.
    const HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(factory.put()));
    return SUCCEEDED(hr) && static_cast<bool>(factory);
}

// Полный probe: без окна, без swap chain, всё освобождается на выходе. Одна
// реализация на «до окна» и «после окна» — иначе эти два момента разойдутся
// и приложение откроет окно, на котором рисовать нечем.
FeatureProbe runProbe(const RendererOptions& options) {
    GraphicsCreation creation;
    const bool d3dOk = createD3D11Device(options, creation);
    FeatureProbe probe = creation.probe;

    ComPtr<ID2D1Factory1> factory;
    probe.d2d1FactoryAvailable = createD2DFactory(factory);

    ComPtr<IDWriteFactory> dwrite;
    probe.directWriteAvailable = createDirectWrite(dwrite);

    if (d3dOk) {
        ComPtr<ID2D1Device> d2dDevice;
        ComPtr<ID2D1DeviceContext> d2dContext;
        probe.d2dDeviceAvailable = createD2DDevice(creation.dxgiDevice, d2dDevice, d2dContext);
        if (!probe.d2dDeviceAvailable) {
            probe.note += "; D2D-устройство поверх DXGI не создалось";
        }
        if (probe.driver != DriverKind::Hardware) {
            probe.note += "; аппаратного ускорения нет";
        }
    }

    if (!probe.d3d11Available && probe.note.empty()) {
        probe.note = "D3D11 недоступен";
    }
    return probe;
}

// ---------------------------------------------------------------------------
// GDI: разбиение текста на строки
// ---------------------------------------------------------------------------
//
// У GDI нет ни раскладки DirectWrite, ни обрезки с многоточием, поэтому строки
// считает сам модуль. Логика общая с measureText: то, что нарисовано, и то,
// что измерено, считается одним кодом — иначе «не влезает» проверялось бы
// одним способом, а обрезалось другим, и это ровно тот класс ошибок, который
// §12 запрещает («нет обрезанных строк при 150 % DPI»).
struct GdiLine {
    std::size_t begin{};
    std::size_t length{};
};

struct GdiTextLayout {
    std::vector<GdiLine> lines;
    bool overflow{false};  // текст не поместился в maxLines или в ширину
};

[[nodiscard]] int measureLinePx(HDC dc, std::wstring_view line) {
    if (line.empty()) return 0;
    SIZE size{};
    if (::GetTextExtentPoint32W(dc, line.data(), static_cast<int>(line.size()), &size) == FALSE) return 0;
    return size.cx;
}

// Жадный перенос по словам. Слово длиннее строки режется по символам: пути
// в MrProper длинные (§5), и иначе подпись с путём не рисовалась бы вовсе.
[[nodiscard]] GdiTextLayout gdiBreakLines(HDC dc, std::wstring_view text, int maxWidthPx, int maxLines) {
    GdiTextLayout layout;
    if (text.empty()) {
        layout.lines.push_back(GdiLine{0, 0});
        return layout;
    }

    std::size_t paragraphStart = 0;
    for (;;) {
        const std::size_t paragraphEnd = text.find(L'\n', paragraphStart);
        const std::size_t end = paragraphEnd == std::wstring_view::npos ? text.size() : paragraphEnd;
        const std::wstring_view paragraph = text.substr(paragraphStart, end - paragraphStart);

        std::size_t lineStart = paragraphStart;
        for (;;) {
            const std::size_t restLength = end - lineStart;
            const std::wstring_view rest = text.substr(lineStart, restLength);
            if (rest.empty()) {
                layout.lines.push_back(GdiLine{lineStart, 0});
                break;
            }
            if (maxWidthPx <= 0 || measureLinePx(dc, rest) <= maxWidthPx) {
                layout.lines.push_back(GdiLine{lineStart, restLength});
                break;
            }

            // Последний пробел, после которого строка ещё помещается.
            std::size_t candidate = 0;
            bool found = false;
            std::size_t space = rest.rfind(L' ');
            while (space != std::wstring_view::npos) {
                if (measureLinePx(dc, rest.substr(0, space)) <= maxWidthPx) {
                    candidate = space;
                    found = true;
                    break;
                }
                if (space == 0) break;
                space = rest.rfind(L' ', space - 1);
            }
            if (found) {
                layout.lines.push_back(GdiLine{lineStart, candidate});
                layout.overflow = true;
                lineStart += candidate + 1;  // пробел не рисуем
                continue;
            }

            // Единственное слово не помещается: режем по символам. Ноль
            // символов здесь означал бы бесконечный цикл, поэтому минимум — 1.
            std::size_t fit = 0;
            while (fit < rest.size() && measureLinePx(dc, rest.substr(0, fit + 1)) <= maxWidthPx) {
                ++fit;
            }
            if (fit == 0) fit = 1;
            layout.lines.push_back(GdiLine{lineStart, fit});
            layout.overflow = true;
            lineStart += fit;
        }

        if (paragraphEnd == std::wstring_view::npos) break;
        paragraphStart = paragraphEnd + 1;
    }

    if (maxLines > 0 && layout.lines.size() > static_cast<std::size_t>(maxLines)) {
        layout.lines.resize(static_cast<std::size_t>(maxLines));
        layout.overflow = true;
    }
    return layout;
}

// Многоточие GDI-пути: последняя оставленная строка укорачивается до «…».
[[nodiscard]] std::wstring gdiLastLineWithEllipsis(HDC dc, std::wstring_view text, GdiLine line, int maxWidthPx) {
    constexpr wchar_t ellipsis = L'…';  // …
    std::wstring out;
    if (maxWidthPx <= 0) return out;

    std::wstring tail;
    std::size_t keep = line.length;
    while (keep > 0) {
        tail.assign(text.substr(line.begin, keep));
        tail += ellipsis;
        if (measureLinePx(dc, tail) <= maxWidthPx) {
            out = tail;
            return out;
        }
        --keep;
    }
    // Даже один символ с многоточием не помещается: рисуем одно многоточие.
    out.assign(1, ellipsis);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Имена для лога и UI
// ---------------------------------------------------------------------------

const char* backendName(Backend backend) noexcept {
    switch (backend) {
        case Backend::None:
            return "нет";
        case Backend::Direct2D:
            return "Direct2D";
        case Backend::GdiDxgi:
            return "GDI поверх DXGI";
        case Backend::GdiWindow:
            return "GDI на окне";
    }
    return "неизвестно";
}

const char* driverKindName(DriverKind driver) noexcept {
    switch (driver) {
        case DriverKind::None:
            return "нет";
        case DriverKind::Hardware:
            return "Hardware";
        case DriverKind::Warp:
            return "WARP";
        case DriverKind::Reference:
            return "Reference";
    }
    return "неизвестно";
}

FeatureProbe probeFeatures() noexcept {
    return runProbe(RendererOptions{});
}

// ---------------------------------------------------------------------------
// Состояние рендерера
// ---------------------------------------------------------------------------

struct Renderer::Impl {
    struct Brush {
        Color color;
        ComPtr<ID2D1SolidColorBrush> d2d;
        HBRUSH gdi{};
    };

    struct Font {
        FontSpec spec;
        ComPtr<IDWriteTextFormat> textFormat;
        HFONT gdiFont{};
        // Метрики в DIP, посчитанные один раз при создании шрифта. В D2D они
        // берутся у начертания (IDWriteFont::GetMetrics) и у пробной раскладки,
        // потому что SetMaxHeight обязан считаться ровно той же высотой строки,
        // которой DirectWrite рисует, — иначе maxLines отрезает строку не там.
        float ascentDip{};
        float descentDip{};
        float lineHeightDip{};
        // DPI, на который создан gdiFont: lfHeight зависит от DPI, поэтому
        // смена DPI обязана привести к пересозданию шрифта, а не к молчаливой
        // смене кегля на экране (WM_DPICHANGED, §5).
        float gdiFontDpi{0.0F};
    };

    ~Impl() {
        releaseAll();
    }

    // --- Состояние -----------------------------------------------------------

    HWND window{};
    RendererOptions options;
    Backend backend{Backend::None};
    FeatureProbe probe;
    bool frameActive{false};
    std::uint32_t recreateAttempts{0};
    std::string lastErrorText;
    std::int64_t lastErrorHr{0};

    // D3D11 и DXGI.
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<IDXGISurface1> backBuffer;
    HDC backBufferDc{};

    // D2D и DirectWrite.
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<IDWriteFactory> dwriteFactory;

    // GDI-поверхность: битовая карта в памяти, из которой кадр либо
    // показывается на окне, либо копируется в буфер swap chain.
    HDC memoryDc{};
    HBITMAP dibBitmap{};
    HGDIOBJ previousBitmap{};

    float dpiX{kDipsPerInch};
    float dpiY{kDipsPerInch};
    std::uint32_t widthPx{};
    std::uint32_t heightPx{};

    std::vector<Brush> brushes;
    std::vector<Font> fonts;

    // --- Освобождение --------------------------------------------------------

    // DC буфера swap chain нельзя держать между кадрами: DXGI считает буфер
    // занятым, и следующий Present возвращает DXGI_ERROR_INVALID_CALL.
    void releaseBackBufferDc() noexcept {
        if (backBufferDc != nullptr && backBuffer) {
            backBuffer->ReleaseDC(nullptr);
        }
        backBufferDc = nullptr;
        backBuffer.reset();
    }

    void releaseGdiSurface() noexcept {
        if (memoryDc != nullptr) {
            if (previousBitmap != nullptr) ::SelectObject(memoryDc, previousBitmap);
            ::DeleteDC(memoryDc);
        }
        if (dibBitmap != nullptr) ::DeleteObject(dibBitmap);
        memoryDc = nullptr;
        dibBitmap = nullptr;
        previousBitmap = nullptr;
    }

    // Поверхности, зависящие от размера окна.
    void releaseSurface() noexcept {
        releaseBackBufferDc();
        d2dTarget.reset();
        if (d2dContext) d2dContext->SetTarget(nullptr);
        releaseGdiSurface();
    }

    // Всё, кроме окна, опций, счётчика пересозданий и последней ошибки:
    // их читают после пересоздания (лог и «О программе»).
    void releaseAll() noexcept {
        releaseSurface();
        swapChain.reset();
        dwriteFactory.reset();
        d2dTarget.reset();
        d2dContext.reset();
        d2dDevice.reset();
        d2dFactory.reset();
        dxgiDevice.reset();
        d3dContext.reset();
        d3dDevice.reset();
        clearCache();
        frameActive = false;
    }

    void clearCache() noexcept {
        for (Brush& brush : brushes) {
            if (brush.gdi != nullptr) ::DeleteObject(brush.gdi);
        }
        brushes.clear();
        for (Font& font : fonts) {
            if (font.gdiFont != nullptr) ::DeleteObject(font.gdiFont);
        }
        fonts.clear();
    }

    // --- Ошибки и лог --------------------------------------------------------

    // Ни одна функция записи отказа не имеет права бросить: SPEC §5 требует,
    // чтобы ни один отказ не ронял процесс. Поэтому try/catch вокруг выделения
    // памяти, а код HRESULT пишется до него — он не выделяет.
    bool fail(const char* api, HRESULT hr) noexcept {
        lastErrorHr = static_cast<std::int64_t>(hr);
        try {
            lastErrorText = api;
            lastErrorText += ": ";
            lastErrorText += platform::hresultErrorText(hr);
            core::LogFields fields;
            fields.push_back(core::logField("api", api));
            fields.push_back(core::logField("backend", backendName(backend)));
            fields.push_back(core::logField("backendInt", static_cast<int>(backend)));
            core::logFailure("ui.renderer.failed", lastErrorText, std::string_view{}, lastErrorHr, fields);
        } catch (const std::exception&) {
            // Логирование не имеет права ронять процесс (core::log сам это
            // гарантирует, но и здесь строка собирается вручную).
        }
        return false;
    }

    bool failWin32(const char* api) noexcept {
        return fail(api, platform::lastErrorHresult());
    }

    void logCreated() const noexcept {
        try {
            core::LogFields fields;
            fields.push_back(core::logField("backend", backendName(backend)));
            fields.push_back(core::logField("driver", driverKindName(probe.driver)));
            fields.push_back(core::logField("hwAccel", probe.hardwareAcceleration));
            fields.push_back(core::logField("bgra", probe.bgraSupport));
            fields.push_back(core::logField("d2d", probe.d2dDeviceAvailable));
            fields.push_back(core::logField("dwrite", probe.directWriteAvailable));
            fields.push_back(core::logField("featureLevel", probe.featureLevelMajor * 10 + probe.featureLevelMinor));
            fields.push_back(core::logField("width", widthPx));
            fields.push_back(core::logField("height", heightPx));
            fields.push_back(core::logField("dpiX", static_cast<double>(dpiX)));
            fields.push_back(core::logField("dpiY", static_cast<double>(dpiY)));
            fields.push_back(core::logField("adapter", probe.adapterName));
            fields.push_back(core::logField("note", probe.note));
            core::logInfo("ui.renderer.created", "графика создана", std::move(fields));
        } catch (const std::exception&) {
            // см. fail()
        }
    }

    // --- Создание ------------------------------------------------------------

    void readWindowDpi() {
        const UINT dpi = ::GetDpiForWindow(window);
        if (dpi == 0) {
            // Окно ещё не создано или GetDpiForWindow недоступен: берём DPI
            // монитора, а если и он нулевой — базовые 96.
            const UINT system = ::GetDpiForSystem();
            dpiX = system == 0 ? kDipsPerInch : static_cast<float>(system);
            dpiY = dpiX;
            return;
        }
        dpiX = static_cast<float>(dpi);
        dpiY = static_cast<float>(dpi);
    }

    bool init(HWND target, const RendererOptions& opts) {
        window = target;
        options = opts;
        if (window == nullptr) return fail("Renderer::create", E_INVALIDARG);

        RECT client{};
        if (::GetClientRect(window, &client) == FALSE) return failWin32("GetClientRect");
        const LONG width = client.right - client.left;
        const LONG height = client.bottom - client.top;
        if (width <= 0 || height <= 0) {
            // Окно ещё не показано: рисовать нечего, и это не поломка. Вызов
            // create повторяют из WM_SIZE, когда размер стал ненулевым.
            return fail("Renderer::create", E_INVALIDARG);
        }
        widthPx = static_cast<std::uint32_t>(width);
        heightPx = static_cast<std::uint32_t>(height);
        readWindowDpi();

        GraphicsCreation creation;
        const bool d3dOk = createD3D11Device(options, creation);
        if (d3dOk) {
            d3dDevice = std::move(creation.device);
            d3dContext = std::move(creation.context);
            dxgiDevice = std::move(creation.dxgiDevice);
        }
        probe = creation.probe;

        ComPtr<ID2D1Factory1> factory;
        probe.d2d1FactoryAvailable = createD2DFactory(factory);
        ComPtr<IDWriteFactory> dwrite;
        probe.directWriteAvailable = createDirectWrite(dwrite);

        if (d3dOk && probe.d2d1FactoryAvailable) {
            // Имена с префиксом new, а не d2dDevice/d2dContext: одноимённая
            // локальная переменная прятала бы член класса, а /W4 такой
            // перекрывающий член объявляет предупреждением C4458, которое в
            // проекте вместе с /WX становится ошибкой сборки (ADR-001).
            ComPtr<ID2D1Device> newD2dDevice;
            ComPtr<ID2D1DeviceContext> newD2dContext;
            probe.d2dDeviceAvailable = createD2DDevice(dxgiDevice, newD2dDevice, newD2dContext);
            if (probe.d2dDeviceAvailable) {
                d2dFactory = std::move(factory);
                d2dDevice = std::move(newD2dDevice);
                d2dContext = std::move(newD2dContext);
                dwriteFactory = std::move(dwrite);
            } else {
                probe.note += "; D2D-устройство поверх DXGI не создалось";
            }
        }

        // WS_EX_TOPMOST ставится ДО создания цепочки: flip-модельная цепочка на
        // дочернем окне без него в части сборок Windows отдаёт пустой кадр
        // (поведение, воспроизведённое в примерах Microsoft). Стиль меняется у
        // окна, которым владеет app_shell, поэтому флаг вынесен в опции.
        if (d2dContext && options.setTopmostStyle) {
            const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
            if ((style & WS_EX_TOPMOST) == 0) {
                ::SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_TOPMOST);
            }
        }

        if (dxgiDevice && buildSwapChain()) {
            if (d2dContext && dwriteFactory && buildD2DTarget()) {
                backend = Backend::Direct2D;
            } else {
                probe.note += "; D2D-таргет не создан, рисуем через GDI поверх DXGI";
                backend = Backend::GdiDxgi;
            }
        } else if (!options.allowGdiFallback) {
            return fail("Renderer::create", DXGI_ERROR_UNSUPPORTED);
        } else {
            probe.note += "; swap chain недоступен, рисуем GDI прямо на окне";
            backend = Backend::GdiWindow;
        }

        if (backend != Backend::Direct2D) {
            // D2D-объекты на этом пути больше не нужны и только занимают память.
            d2dTarget.reset();
            if (d2dContext) d2dContext->SetTarget(nullptr);
            d2dContext.reset();
            d2dDevice.reset();
            d2dFactory.reset();
            dwriteFactory.reset();
            clearCache();
            if (backend == Backend::GdiDxgi && !ensureBackBufferDc()) {
                backend = Backend::GdiWindow;
            }
            if (!buildGdiSurface()) return false;
        }

        logCreated();
        return true;
    }

    // Swap chain перебирается по нескольким сочетаниям формата и метода
    // present. Flip-модельные варианты идут первыми (дешевле и без мерцания),
    // DISCARD — последним: он работает везде, но медленнее. R8G8B8A8_UNORM
    // оставлен на случай драйверов, не умеющих B8G8R8A8.
    //
    // Фабрика создаётся отдельным вызовом CreateDXGIFactory1 и приводится к
    // IDXGIFactory2: в установленном SDK (10.0.19041) нет ни CreateDXGIFactory2,
    // ни метода CreateSwapChainForHwnd у устройства — цепочка для окна есть
    // только у фабрики второй версии.
    bool buildSwapChain() {
        if (window == nullptr || !d3dDevice) return false;

        ComPtr<IDXGIFactory2> factory;
        ComPtr<IDXGIFactory1> factory1;
        HRESULT factoryHr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory1.put()));
        if (SUCCEEDED(factoryHr) && factory1) factoryHr = factory1.query(factory.put());
        if (FAILED(factoryHr) || !factory) {
            return fail("CreateDXGIFactory1", FAILED(factoryHr) ? factoryHr : E_FAIL);
        }

        struct Combo {
            DXGI_FORMAT format;
            DXGI_SWAP_EFFECT effect;
            UINT bufferCount;
        };
        std::vector<Combo> combos;
        if (options.allowFlipModel) {
            combos.push_back(Combo{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2});
            combos.push_back(Combo{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_EFFECT_FLIP_DISCARD, 2});
        }
        combos.push_back(Combo{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_EFFECT_DISCARD, 1});
        combos.push_back(Combo{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_EFFECT_DISCARD, 1});

        std::string reasons;
        for (const Combo& combo : combos) {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = widthPx;
            description.Height = heightPx;
            description.Format = combo.format;
            description.SampleDesc.Count = 1;  // без MSAA: целью является буфер
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = combo.bufferCount;
            description.SwapEffect = combo.effect;
            // Окно непрозрачное: DWM игнорирует альфу, и её «непрозрачность»
            // в буфере не должна влиять на композицию.
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            description.Scaling = DXGI_SCALING_NONE;  // размер буфера == размер клиентской области
            description.Flags = 0;

            ComPtr<IDXGISwapChain1> chain;
            const HRESULT hr =
                factory->CreateSwapChainForHwnd(d3dDevice.get(), window, &description, nullptr, nullptr, chain.put());
            if (SUCCEEDED(hr) && chain) {
                swapChain = std::move(chain);
                return true;
            }
            if (!reasons.empty()) reasons += "; ";
            reasons += platform::hresultErrorText(hr);
        }
        return fail("IDXGIFactory2::CreateSwapChainForHwnd", DXGI_ERROR_UNSUPPORTED);
    }

    // Цель рисования — битовая карта поверх буфера swap chain. Явная цель (а не
    // ID2D1BitmapRenderTarget) выбрана потому, что в установленном SDK
    // (10.0.19041) у ID2D1DeviceContext нет CreateBitmapRenderTarget, а
    // CreateBitmapFromDxgiSurface есть и ведёт себя именно так, как нужно:
    // D2D рисует прямо в буфер, Present делаем мы.
    bool buildD2DTarget() {
        if (!d2dContext || !swapChain) return false;

        ComPtr<IDXGISurface> surface;
        HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));
        if (FAILED(hr) || !surface) return fail("IDXGISwapChain1::GetBuffer", FAILED(hr) ? hr : E_FAIL);

        D2D1_BITMAP_PROPERTIES1 properties{};
        properties.dpiX = dpiX;
        properties.dpiY = dpiY;
        // DXGI_FORMAT_UNKNOWN — «формат как у поверхности», а не «свой».
        properties.pixelFormat.format = DXGI_FORMAT_UNKNOWN;
        properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        ComPtr<ID2D1Bitmap1> target;
        hr = d2dContext->CreateBitmapFromDxgiSurface(surface.get(), &properties, target.put());
        if (FAILED(hr) || !target) return fail("ID2D1DeviceContext::CreateBitmapFromDxgiSurface", FAILED(hr) ? hr : E_FAIL);

        d2dContext->SetTarget(target.get());
        d2dContext->SetUnitMode(D2D1_UNIT_MODE_DIPS);  // все координаты — в DIP
        d2dContext->SetDpi(dpiX, dpiY);
        d2dContext->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        d2dTarget = std::move(target);
        return true;
    }

    // Поверхность в памяти: top-down 32-битная DIB. top-down выбран сознательно
    // — строки копируются на экран сверху вниз без разворота, и ClearType-текст
    // не требует переворота альфа-канала.
    bool buildGdiSurface() {
        releaseGdiSurface();
        if (widthPx == 0 || heightPx == 0) return fail("Renderer::resize", E_INVALIDARG);
        if (window == nullptr) return fail("Renderer::resize", E_INVALIDARG);

        HDC reference = ::GetDC(window);
        if (reference == nullptr) return failWin32("GetDC");

        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(widthPx);
        information.bmiHeader.biHeight = -static_cast<LONG>(heightPx);  // отрицательная высота = top-down
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(reference, &information, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr) {
            const HRESULT hr = platform::lastErrorHresult();
            ::ReleaseDC(window, reference);
            return fail("CreateDIBSection", hr);
        }
        HDC memory = ::CreateCompatibleDC(reference);
        ::ReleaseDC(window, reference);
        if (memory == nullptr) {
            ::DeleteObject(bitmap);
            return failWin32("CreateCompatibleDC");
        }

        dibBitmap = bitmap;
        memoryDc = memory;
        previousBitmap = ::SelectObject(memoryDc, dibBitmap);
        // Прозрачный фон обязателен: иначе GDI затирает подложку, нарисованную
        // прямоугольником, при каждом выводе текста.
        ::SetBkMode(memoryDc, TRANSPARENT);
        return true;
    }

    // DC буфера берётся у IDXGISurface1: у IDXGISurface метода GetDC нет, он
    // появился только в Surface1 (Windows 8). DC держится ровно между
    // BitBlt и ReleaseDC — пока он взят, буфер считается занятым, и Present
    // вернёт DXGI_ERROR_INVALID_CALL.
    bool ensureBackBufferDc() {
        if (backBufferDc != nullptr) return true;
        if (!swapChain) return false;
        ComPtr<IDXGISurface> surface;
        const HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));
        if (FAILED(hr) || !surface) return fail("IDXGISwapChain::GetBuffer", FAILED(hr) ? hr : E_FAIL);
        ComPtr<IDXGISurface1> surface1;
        if (FAILED(surface.query(surface1.put())) || !surface1) {
            return fail("IDXGISurface -> IDXGISurface1", DXGI_ERROR_UNSUPPORTED);
        }
        HDC dc = nullptr;
        const HRESULT dcHr = surface1->GetDC(FALSE, &dc);
        if (FAILED(dcHr) || dc == nullptr) return fail("IDXGISurface1::GetDC", FAILED(dcHr) ? dcHr : E_FAIL);
        backBuffer = std::move(surface1);
        backBufferDc = dc;
        return true;
    }

    // --- Пересоздание --------------------------------------------------------

    // Полное пересоздание: потеря устройства разрушает и DXGI, и D2D, и
    // DirectWrite-ресурсы, поэтому собираем всё заново на том же окне.
    // Попытки ограничены kMaxRecreateAttempts; после исчерпания — GDI, потому
    // что бесконечный цикл пересоздания на сломанном драйвере хуже, чем
    // рисование через GDI.
    bool rebuild() {
        if (recreateAttempts >= kMaxRecreateAttempts) {
            if (backend == Backend::GdiWindow) return false;
            fail("Renderer::recreate", DXGI_ERROR_DEVICE_REMOVED);
            swapChain.reset();
            releaseBackBufferDc();
            d2dContext.reset();
            d2dDevice.reset();
            d2dFactory.reset();
            dwriteFactory.reset();
            d2dTarget.reset();
            clearCache();
            backend = Backend::GdiWindow;
            probe.note += "; переход на GDI: попыток пересоздания исчерпано";
            probe.d2dDeviceAvailable = false;
            if (!buildGdiSurface()) return false;
            logCreated();
            return true;
        }

        ++recreateAttempts;
        const HWND target = window;
        const RendererOptions opts = options;
        releaseAll();
        if (!init(target, opts)) return false;
        lastErrorText.clear();
        lastErrorHr = 0;
        return backend == Backend::Direct2D;
    }

    // --- Кэш ресурсов --------------------------------------------------------

    [[nodiscard]] Brush* brushById(BrushId id) {
        if (id == kInvalidBrush || id > brushes.size()) return nullptr;
        return &brushes[id - 1];
    }

    [[nodiscard]] Font* fontById(FontId id) {
        if (id == kInvalidFont || id > fonts.size()) return nullptr;
        return &fonts[id - 1];
    }

    BrushId addBrush(const Color& color) {
        if (backend == Backend::None) return kInvalidBrush;
        Brush brush;
        brush.color = color;
        if (backend == Backend::Direct2D) {
            if (!d2dContext) return kInvalidBrush;
            ComPtr<ID2D1SolidColorBrush> created;
            const HRESULT hr = d2dContext->CreateSolidColorBrush(toD2dColor(color), created.put());
            if (FAILED(hr) || !created) {
                fail("ID2D1DeviceContext::CreateSolidColorBrush", FAILED(hr) ? hr : E_FAIL);
                return kInvalidBrush;
            }
            brush.d2d = std::move(created);
        }
        brushes.push_back(std::move(brush));
        return static_cast<BrushId>(brushes.size());
    }

    FontId addFont(const FontSpec& spec) {
        if (backend == Backend::None) return kInvalidFont;
        Font font;
        font.spec = spec;
        font.spec.sizeDip = sanitizeFontSize(spec.sizeDip);
        if (backend == Backend::Direct2D) {
            if (!dwriteFactory) return kInvalidFont;
            const std::wstring family = familyOf(font.spec);
            const std::wstring locale = localeOf(font.spec);
            ComPtr<IDWriteTextFormat> format;
            const HRESULT hr = dwriteFactory->CreateTextFormat(
                family.c_str(), nullptr, toDwriteWeight(font.spec.weight),
                font.spec.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                font.spec.sizeDip, locale.c_str(), format.put());
            if (FAILED(hr) || !format) {
                fail("IDWriteFactory::CreateTextFormat", FAILED(hr) ? hr : E_FAIL);
                return kInvalidFont;
            }
            font.textFormat = std::move(format);
            measureFont(font);
        }
        fonts.push_back(std::move(font));
        return static_cast<FontId>(fonts.size());
    }

    // Метрики шрифта для D2D-пути, один раз при создании.
    //
    // Откуда две разные величины. Ascent и descent есть у начертания
    // (IDWriteFont::GetMetrics) — это базовая линия, нужная вёрстке. А высота
    // строки, которой реально рисует DirectWrite, берётся из замера пробной
    // раскладки: она включает интерлиньяж и кегль, посчитанный самим
    // DirectWrite, и именно с ней согласуется SetMaxHeight. Обе величины
    // нужны: первая — для базовой линии и подписи, вторая — для отсечения по
    // числу строк.
    void measureFont(Font& font) {
        if (!dwriteFactory || !font.textFormat) return;

        const std::wstring family = familyOf(font.spec);
        ComPtr<IDWriteFontCollection> collection;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(dwriteFactory->GetSystemFontCollection(collection.put(), FALSE)) && collection &&
            SUCCEEDED(collection->FindFamilyName(family.c_str(), &index, &exists)) && exists) {
            ComPtr<IDWriteFontFamily> fontFamily;
            ComPtr<IDWriteFont> face;
            if (SUCCEEDED(collection->GetFontFamily(index, fontFamily.put())) && fontFamily &&
                SUCCEEDED(fontFamily->GetFirstMatchingFont(toDwriteWeight(font.spec.weight),
                                                            DWRITE_FONT_STRETCH_NORMAL,
                                                            font.spec.italic ? DWRITE_FONT_STYLE_ITALIC
                                                                             : DWRITE_FONT_STYLE_NORMAL,
                                                            face.put())) &&
                face) {
                // IDWriteFont::GetMetrics возвращает void (в отличие от
                // IDWriteFontFace::GetMetrics, возвращающего HRESULT), поэтому
                // проверять нечего: признак неудачи здесь — designUnitsPerEm == 0.
                DWRITE_FONT_METRICS faceMetrics{};
                face->GetMetrics(&faceMetrics);
                if (faceMetrics.designUnitsPerEm > 0) {
                    const float scale = font.spec.sizeDip / static_cast<float>(faceMetrics.designUnitsPerEm);
                    font.ascentDip = static_cast<float>(faceMetrics.ascent) * scale;
                    font.descentDip = static_cast<float>(faceMetrics.descent) * scale;
                    font.lineHeightDip =
                        static_cast<float>(faceMetrics.ascent + faceMetrics.descent + faceMetrics.lineGap) * scale;
                }
            }
        }

        // Пробная раскладка: одна строка «ХMg» (буквы с выносными элементами),
        // ширина без ограничения. Её высота — то, чем DirectWrite измеряет
        // строку, и только она годится для SetMaxHeight.
        ComPtr<IDWriteTextLayout> probeLayout;
        constexpr wchar_t sample[] = L"ХMg";
        constexpr UINT32 sampleLength = static_cast<UINT32>(sizeof(sample) / sizeof(sample[0]) - 1);
        if (SUCCEEDED(dwriteFactory->CreateTextLayout(sample, sampleLength, font.textFormat.get(), kUnlimitedWidth,
                                                      0.0F, probeLayout.put())) &&
            probeLayout) {
            DWRITE_TEXT_METRICS probeMetrics{};
            if (SUCCEEDED(probeLayout->GetMetrics(&probeMetrics)) && probeMetrics.height > 0.0F) {
                font.lineHeightDip = probeMetrics.height;
            }
        }
    }

    [[nodiscard]] HBRUSH gdiBrushFor(Brush& brush) {
        if (brush.gdi == nullptr) {
            const HBRUSH created = ::CreateSolidBrush(toColorRef(brush.color));
            if (created == nullptr) {
                failWin32("CreateSolidBrush");
                return nullptr;
            }
            brush.gdi = created;
        }
        return brush.gdi;
    }

    // Шрифт GDI создаётся лениво и пересоздаётся при смене DPI: lfHeight
    // зависит от DPI, и кэш, переживший WM_DPICHANGED, рисовал бы текст
    // прежнего размера.
    [[nodiscard]] HFONT gdiFontFor(Font& font) {
        if (font.gdiFont != nullptr && font.gdiFontDpi == dpiY) return font.gdiFont;
        if (font.gdiFont != nullptr) ::DeleteObject(font.gdiFont);

        // Высота em в пикселях: 1 DIP = dpi/96 пикселя, а lfHeight задаётся в
        // пунктах, то есть в 1/72 дюйма. Отсюда пересчёт через MulDiv, а не
        // умножение на 0.75 «на глаз».
        const LONG dpiLong = static_cast<LONG>(std::lround(dpiY));
        const int emPixels = static_cast<int>(std::lround(sanitizeFontSize(font.spec.sizeDip) * dpiY / kDipsPerInch));
        const LONG height = dpiLong > 0 ? -::MulDiv(static_cast<LONG>(emPixels), 72, dpiLong) : -12;

        LOGFONTW description{};
        description.lfHeight = height;
        description.lfWeight = toGdiWeight(font.spec.weight);
        description.lfItalic = font.spec.italic ? TRUE : FALSE;
        description.lfUnderline = FALSE;
        description.lfStrikeOut = FALSE;
        description.lfCharSet = DEFAULT_CHARSET;
        description.lfOutPrecision = OUT_TT_PRECIS;
        description.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        // ClearType доступен на 32-битной DIB и заметно разборчив при 150 % DPI
        // (§12). Альфа-канал после ClearType остаётся нулевым, но окно
        // непрозрачное, и альфа не участвует в композиции.
        description.lfQuality = CLEARTYPE_QUALITY;
        description.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
        const std::wstring family = familyOf(font.spec);
        if (!family.empty()) {
            ::wcsncpy_s(description.lfFaceName, LF_FACESIZE, family.c_str(), _TRUNCATE);
        }

        font.gdiFont = ::CreateFontIndirectW(&description);
        if (font.gdiFont == nullptr) {
            failWin32("CreateFontIndirectW");
            return nullptr;
        }
        font.gdiFontDpi = dpiY;
        return font.gdiFont;
    }

    // --- Кадр ----------------------------------------------------------------

    bool beginFrame(const Color* clearColor) {
        if (backend == Backend::None || widthPx == 0 || heightPx == 0) return false;
        if (frameActive) return false;
        if (backend == Backend::Direct2D && (!d2dTarget || !d2dContext)) return false;
        if (backend != Backend::Direct2D && memoryDc == nullptr) return false;

        if (backend == Backend::Direct2D) {
            d2dContext->BeginDraw();
            if (clearColor != nullptr) {
                ComPtr<ID2D1SolidColorBrush> brush;
                if (SUCCEEDED(d2dContext->CreateSolidColorBrush(toD2dColor(*clearColor), brush.put())) && brush) {
                    d2dContext->FillRectangle(fullRect(), brush.get());
                }
            } else {
                d2dContext->Clear(D2D1_COLOR_F{0.0F, 0.0F, 0.0F, 0.0F});
            }
        } else {
            gdiClear(clearColor != nullptr ? *clearColor : Color::rgba(0.0F, 0.0F, 0.0F, 0.0F));
        }
        frameActive = true;
        return true;
    }

    void gdiClear(const Color& color) {
        if (memoryDc == nullptr) return;
        RECT area{};
        area.right = static_cast<LONG>(widthPx);
        area.bottom = static_cast<LONG>(heightPx);
        const HBRUSH brush = ::CreateSolidBrush(toColorRef(color));
        if (brush == nullptr) {
            failWin32("CreateSolidBrush");
            return;
        }
        ::FillRect(memoryDc, &area, brush);
        ::DeleteObject(brush);
    }

    [[nodiscard]] D2D1_RECT_F fullRect() const {
        return D2D1_RECT_F{0.0F, 0.0F, static_cast<FLOAT>(widthPx) * kDipsPerInch / dpiX,
                           static_cast<FLOAT>(heightPx) * kDipsPerInch / dpiY};
    }

    bool endFrame() {
        if (!frameActive) return false;
        frameActive = false;
        if (backend == Backend::Direct2D && !d2dContext) return false;

        if (backend == Backend::Direct2D) {
            const HRESULT hr = d2dContext->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) {
                fail("ID2D1DeviceContext::EndDraw", hr);
                rebuild();
                return false;
            }
            if (FAILED(hr)) {
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                    fail("ID2D1DeviceContext::EndDraw", hr);
                    rebuild();
                    return false;
                }
                return fail("ID2D1DeviceContext::EndDraw", hr);
            }
            const UINT flags = options.waitForVerticalBlank ? 0 : DXGI_PRESENT_DO_NOT_WAIT;
            const HRESULT presentHr = swapChain ? swapChain->Present(flags, 0) : DXGI_ERROR_INVALID_CALL;
            if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
                fail("IDXGISwapChain1::Present", presentHr);
                rebuild();
                return false;
            }
            if (FAILED(presentHr)) return fail("IDXGISwapChain1::Present", presentHr);
            recreateAttempts = 0;  // кадр показан — счётчик попыток можно сбросить
            return true;
        }

        if (backend == Backend::GdiDxgi) {
            if (!ensureBackBufferDc()) return false;
            blitTo(backBufferDc);
            releaseBackBufferDc();
            const HRESULT presentHr = swapChain ? swapChain->Present(0, 0) : DXGI_ERROR_INVALID_CALL;
            if (FAILED(presentHr)) return fail("IDXGISwapChain1::Present", presentHr);
            return true;
        }

        // GdiWindow: вывод прямо на окно. Для приложения, которое рисует само
        // из WM_PAINT, есть blitToHdc — тогда endFrame вызывать не нужно.
        HDC windowDc = ::GetDC(window);
        if (windowDc == nullptr) return failWin32("GetDC");
        blitTo(windowDc);
        ::ReleaseDC(window, windowDc);
        return true;
    }

    // Копирование накопленного кадра в произвольный DC.
    void blitTo(HDC target) {
        if (memoryDc == nullptr || target == nullptr || widthPx == 0 || heightPx == 0) return;
        if (::BitBlt(target, 0, 0, static_cast<int>(widthPx), static_cast<int>(heightPx), memoryDc, 0, 0, SRCCOPY) ==
            FALSE) {
            failWin32("BitBlt");
        }
    }

    // --- Примитивы -----------------------------------------------------------

    void fillRect(const Rect& rect, BrushId id) {
        Brush* brush = brushById(id);
        if (brush == nullptr || rect.isEmpty()) return;
        if (backend == Backend::Direct2D) {
            if (d2dContext && brush->d2d) d2dContext->FillRectangle(toD2dRect(rect), brush->d2d.get());
            return;
        }
        if (memoryDc == nullptr) return;
        RECT pixels = toPixelsRect(rect, dpiX, dpiY);
        const HBRUSH gdi = gdiBrushFor(*brush);
        if (gdi != nullptr) ::FillRect(memoryDc, &pixels, gdi);
    }

    void fillRoundedRect(const Rect& rect, float radiusDip, BrushId id) {
        if (radiusDip <= 0.0F) {
            fillRect(rect, id);
            return;
        }
        Brush* brush = brushById(id);
        if (brush == nullptr || rect.isEmpty()) return;
        if (backend == Backend::Direct2D) {
            if (d2dContext && brush->d2d) {
                // Структура собирается вручную, а не через D2D1::RoundedRect:
                // помощники живут в d2d1helper.h, который в установленном SDK
                // тянет за собой свою версию макросов, а модулю достаточно
                // одной структуры из d2d1.h.
                D2D1_ROUNDED_RECT rounded{};
                rounded.rect = toD2dRect(rect);
                rounded.radiusX = radiusDip;
                rounded.radiusY = radiusDip;
                d2dContext->FillRoundedRectangle(rounded, brush->d2d.get());
            }
            return;
        }
        if (memoryDc == nullptr) return;
        RECT pixels = toPixelsRect(rect, dpiX, dpiY);
        const HBRUSH gdi = gdiBrushFor(*brush);
        if (gdi == nullptr) return;
        const int diameterX = toPixels(radiusDip * 2.0F, dpiX);
        const int diameterY = toPixels(radiusDip * 2.0F, dpiY);
        ::RoundRect(memoryDc, pixels.left, pixels.top, pixels.right, pixels.bottom, diameterX, diameterY);
    }

    void fillEllipse(const Rect& rect, BrushId id) {
        Brush* brush = brushById(id);
        if (brush == nullptr || rect.isEmpty()) return;
        if (backend == Backend::Direct2D) {
            if (d2dContext && brush->d2d) {
                const D2D1_RECT_F area = toD2dRect(rect);
                const D2D1_POINT_2F center{area.left + area.right, area.top + area.bottom};
                D2D1_ELLIPSE ellipse{};
                ellipse.point = D2D1_POINT_2F{center.x / 2.0F, center.y / 2.0F};
                ellipse.radiusX = (area.right - area.left) / 2.0F;
                ellipse.radiusY = (area.bottom - area.top) / 2.0F;
                d2dContext->FillEllipse(&ellipse, brush->d2d.get());
            }
            return;
        }
        if (memoryDc == nullptr) return;
        RECT pixels = toPixelsRect(rect, dpiX, dpiY);
        const HBRUSH gdi = gdiBrushFor(*brush);
        if (gdi == nullptr) return;
        HGDIOBJ previous = ::SelectObject(memoryDc, gdi);
        ::Ellipse(memoryDc, pixels.left, pixels.top, pixels.right, pixels.bottom);
        ::SelectObject(memoryDc, previous);
    }

    // Рамка собирается четырьмя заливками, одинаково для обоих путей. Так
    // дешевле и, главное, одинаково: у Direct2D в установленном SDK нет
    // StrokeRectangle, а карандаш GDI рисует по ЦЕНТРУ границы, из-за чего
    // рамки на D2D и GDI разошлись бы на половину толщины — на тонких
    // разделителях карты разделов (FR-2) это видно. Здесь прямоугольник —
    // внешняя граница рамки, а сама рамка лежит внутри него.
    void strokeRect(const Rect& rect, BrushId id, float strokeWidthDip) {
        if (rect.isEmpty() || strokeWidthDip <= 0.0F) return;
        const float width = strokeWidthDip;
        fillRect(Rect{rect.left, rect.top, rect.right, rect.top + width}, id);
        fillRect(Rect{rect.left, rect.bottom - width, rect.right, rect.bottom}, id);
        fillRect(Rect{rect.left, rect.top + width, rect.left + width, rect.bottom - width}, id);
        fillRect(Rect{rect.right - width, rect.top + width, rect.right, rect.bottom - width}, id);
    }

    void strokeLine(Point from, Point to, BrushId id, float strokeWidthDip) {
        Brush* brush = brushById(id);
        if (brush == nullptr || strokeWidthDip <= 0.0F) return;
        if (backend == Backend::Direct2D) {
            if (d2dContext && brush->d2d) {
                d2dContext->DrawLine(D2D1_POINT_2F{from.x, from.y}, D2D1_POINT_2F{to.x, to.y}, brush->d2d.get(),
                                     strokeWidthDip);
            }
            return;
        }
        if (memoryDc == nullptr) return;

        const int width = std::max(1, toPixels(strokeWidthDip, dpiY));
        const HPEN pen = ::CreatePen(PS_SOLID, width, toColorRef(brush->color));
        if (pen == nullptr) {
            failWin32("CreatePen");
            return;
        }
        HGDIOBJ previousPen = ::SelectObject(memoryDc, pen);
        // Прозрачный фон режима отменяет пунктир на концах линии; он уже
        // установлен при создании поверхности, но линия может рисоваться и в
        // чужом DC через blitToHdc — там режим тоже нужен.
        const int previousBkMode = ::SetBkMode(memoryDc, TRANSPARENT);
        ::MoveToEx(memoryDc, toPixels(from.x, dpiX), toPixels(from.y, dpiY), nullptr);
        ::LineTo(memoryDc, toPixels(to.x, dpiX), toPixels(to.y, dpiY));
        ::SetBkMode(memoryDc, previousBkMode);
        ::SelectObject(memoryDc, previousPen);
        ::DeleteObject(pen);
    }

    void clear(const Color& color) {
        if (backend == Backend::Direct2D) {
            if (!d2dTarget) return;
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(d2dContext->CreateSolidColorBrush(toD2dColor(color), brush.put())) && brush) {
                d2dContext->FillRectangle(fullRect(), brush.get());
            }
            return;
        }
        gdiClear(color);
    }

    // --- Текст ---------------------------------------------------------------

    // Метрики шрифта в DIP. Не константная: в GDI-режиме шрифт должен быть
    // создан, иначе GetTextMetricsW вернёт метрики шрифта по умолчанию — то
    // есть вёрстку другого кегля.
    [[nodiscard]] FontMetrics metricsOf(Font& font) {
        FontMetrics metrics;
        if (backend == Backend::Direct2D) {
            // Метрики посчитаны при создании шрифта: IDWriteTextFormat метрик не
            // отдаёт (в этом SDK у него нет GetMetrics), а начертание и пробная
            // раскладка дают ровно те числа, которыми DirectWrite рисует.
            metrics.ascentDip = font.ascentDip;
            metrics.descentDip = font.descentDip;
            metrics.lineHeightDip = font.lineHeightDip;
            return metrics;
        }
        if (memoryDc == nullptr) return metrics;
        if (gdiFontFor(font) == nullptr) return metrics;
        HGDIOBJ previous = ::SelectObject(memoryDc, font.gdiFont);
        TEXTMETRICW textMetrics{};
        const BOOL ok = ::GetTextMetricsW(memoryDc, &textMetrics);
        ::SelectObject(memoryDc, previous);
        if (ok == FALSE) return metrics;
        const float scale = kDipsPerInch / dpiY;
        metrics.ascentDip = static_cast<float>(textMetrics.tmAscent) * scale;
        metrics.descentDip = static_cast<float>(textMetrics.tmDescent) * scale;
        metrics.lineHeightDip =
            static_cast<float>(textMetrics.tmHeight + textMetrics.tmExternalLeading) * scale;
        return metrics;
    }

    [[nodiscard]] Size measureTextUtf16(Font& font, std::wstring_view text, float maxWidthDip) {
        if (backend == Backend::Direct2D) {
            ComPtr<IDWriteTextLayout> layout;
            const float width = maxWidthDip > 0.0F ? maxWidthDip : kUnlimitedWidth;
            const HRESULT hr = dwriteFactory ? dwriteFactory->CreateTextLayout(
                                                   text.data(), static_cast<UINT32>(text.size()),
                                                   font.textFormat.get(), width, 0.0F, layout.put())
                                             : E_FAIL;
            if (FAILED(hr) || !layout) {
                fail("IDWriteFactory::CreateTextLayout", FAILED(hr) ? hr : E_FAIL);
                return Size{};
            }
            DWRITE_TEXT_METRICS textMetrics{};
            if (FAILED(layout->GetMetrics(&textMetrics))) return Size{};
            return Size{textMetrics.width, textMetrics.height};
        }

        if (memoryDc == nullptr) return Size{};
        const HFONT gdiFont = gdiFontFor(font);
        if (gdiFont == nullptr) return Size{};
        HGDIOBJ previous = ::SelectObject(memoryDc, gdiFont);
        const int maxWidthPx = maxWidthDip > 0.0F ? toPixels(maxWidthDip, dpiX) : 0;
        const GdiTextLayout layout = gdiBreakLines(memoryDc, text, maxWidthPx, 0);
        const FontMetrics metrics = metricsOf(font);
        int widest = 0;
        for (const GdiLine& line : layout.lines) {
            const std::wstring_view piece = text.substr(line.begin, line.length);
            widest = std::max(widest, measureLinePx(memoryDc, piece));
        }
        const int lineHeightPx = toPixels(metrics.lineHeightDip, dpiY);
        ::SelectObject(memoryDc, previous);
        return Size{static_cast<float>(widest) * kDipsPerInch / dpiX,
                    static_cast<float>(lineHeightPx * static_cast<int>(layout.lines.size())) * kDipsPerInch / dpiY};
    }

    void drawTextUtf16(std::wstring_view text, const Rect& area, Font& font, Brush& brush,
                       const TextOptions& textOptions) {
        if (area.isEmpty()) return;
        if (backend == Backend::Direct2D) {
            drawTextD2D(text, area, font, brush.d2d.get(), textOptions);
            return;
        }
        drawTextGdi(text, area, font, brush, textOptions);
    }

    // Обрезка текста под maxLines и многоточие. Установленный здесь SDK
    // (10.0.19041) не имеет поля trailingEllipsis в DWRITE_TRIMMING, поэтому
    // штатное «многоточие» DirectWrite недоступно, и обрезка считается здесь:
    // сначала проверяется, помещается ли текст целиком (обычный случай, одна
    // раскладка), и только при переполнении выполняется двоичный поиск по
    // длине префикса. Стоимость — O(log n) раскладок и только на тексте,
    // который реально не влез.
    [[nodiscard]] std::wstring fittedText(std::wstring_view text, Font& font, const TextOptions& textOptions,
                                          float maxWidth, float maxHeight) {
        if (text.empty() || textOptions.maxLines <= 0 || maxHeight <= 0.0F) return std::wstring(text);
        if (!dwriteFactory || !font.textFormat) return std::wstring(text);

        // Раскладка-зонд: если текст помещается целиком, ничего не трогаем.
        if (layoutFits(text, font, maxWidth, maxHeight)) return std::wstring(text);
        // Многоточие выключено — отдаём текст как есть: вызывающий сам решил,
        // что лучше обрезка по границе, чем исчезновение строки целиком.
        if (!textOptions.ellipsis) return std::wstring(text);

        constexpr wchar_t ellipsis = L'…';  // …
        std::size_t low = 0;
        std::size_t high = text.size();
        std::size_t best = 0;
        while (low <= high) {
            const std::size_t middle = low + (high - low) / 2;
            std::wstring candidate(text.substr(0, middle));
            candidate += ellipsis;
            if (layoutFits(candidate, font, maxWidth, maxHeight)) {
                best = middle;
                low = middle + 1;
            } else {
                if (middle == 0) break;
                high = middle - 1;
            }
        }
        std::wstring out(text.substr(0, best));
        out += ellipsis;
        return out;
    }

    [[nodiscard]] bool layoutFits(std::wstring_view text, Font& font, float maxWidth, float maxHeight) const {
        if (!dwriteFactory || !font.textFormat) return true;
        ComPtr<IDWriteTextLayout> layout;
        const float width = maxWidth > 0.0F ? maxWidth : kUnlimitedWidth;
        if (FAILED(dwriteFactory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), font.textFormat.get(),
                                                  width, 0.0F, layout.put())) ||
            !layout) {
            return true;  // не смогли измерить — не режем текст
        }
        if (maxWidth > 0.0F) {
            // Раскладка по ширине всегда «помещается» по высоте одной строки,
            // но текст может быть шире: DWrite обрезает его по словам молча,
            // и тогда последнее слово исчезает. Отлавливаем это по ширине.
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(layout->GetMetrics(&metrics))) return true;
            if (metrics.width > maxWidth + 0.5F) return false;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return true;
        return metrics.height <= maxHeight + 0.5F;
    }

    void drawTextD2D(std::wstring_view text, const Rect& area, Font& font, ID2D1Brush* brush,
                     const TextOptions& textOptions) {
        if (!d2dContext || !font.textFormat || !dwriteFactory) return;

        font.textFormat->SetTextAlignment(dwriteAlignment(textOptions));
        font.textFormat->SetWordWrapping(textOptions.wordWrap ? DWRITE_WORD_WRAPPING_WRAP
                                                                : DWRITE_WORD_WRAPPING_NO_WRAP);
        font.textFormat->SetReadingDirection(textOptions.rightToLeft ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                                                      : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);

        const FontMetrics metrics = metricsOf(font);
        const float maxLinesHeight =
            textOptions.maxLines > 0 ? static_cast<float>(textOptions.maxLines) * metrics.lineHeightDip : 0.0F;
        // Подгонка под maxLines делается до раскладки: иначе лишние строки
        // остались бы видны, а SetMaxHeight просто срезал бы последнюю строку
        // по глифам, без многоточия.
        const std::wstring fitted = fittedText(text, font, textOptions, area.width(), maxLinesHeight);
        if (fitted.empty()) return;

        ComPtr<IDWriteTextLayout> layout;
        const HRESULT hr = dwriteFactory->CreateTextLayout(fitted.data(), static_cast<UINT32>(fitted.size()),
                                                           font.textFormat.get(), area.width(), 0.0F, layout.put());
        if (FAILED(hr) || !layout) {
            fail("IDWriteFactory::CreateTextLayout", FAILED(hr) ? hr : E_FAIL);
            return;
        }
        if (maxLinesHeight > 0.0F) layout->SetMaxHeight(maxLinesHeight);

        DWRITE_TEXT_METRICS textMetrics{};
        if (FAILED(layout->GetMetrics(&textMetrics))) return;

        // ID2D1RenderTarget::DrawTextLayout принимает начало раскладки, а не
        // прямоугольник: вертикальное выравнивание считается здесь, по
        // фактической высоте текста, а горизонтальное — форматом внутри
        // раскладки шириной в area.
        D2D1_POINT_2F origin{area.left, area.top};
        const float shift = (area.height() - textMetrics.height) * verticalFactor(textOptions.vertical);
        if (shift > 0.0F) origin.y += shift;
        d2dContext->DrawTextLayout(origin, layout.get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void drawTextGdi(std::wstring_view text, const Rect& area, Font& font, Brush& brush, const TextOptions& textOptions) {
        if (memoryDc == nullptr) return;
        const HFONT gdiFont = gdiFontFor(font);
        if (gdiFont == nullptr) return;

        HGDIOBJ previousFont = ::SelectObject(memoryDc, gdiFont);
        const FontMetrics metrics = metricsOf(font);
        int lineHeightPx = toPixels(metrics.lineHeightDip, dpiY);
        if (lineHeightPx <= 0) {
            TEXTMETRICW raw{};
            if (::GetTextMetricsW(memoryDc, &raw) != FALSE) lineHeightPx = raw.tmHeight + raw.tmExternalLeading;
        }
        if (lineHeightPx <= 0) lineHeightPx = toPixels(kDipsPerInch, dpiY);

        int boxWidthPx = area.width() > 0.0F ? toPixels(area.width(), dpiX) : 0;
        GdiTextLayout layout = gdiBreakLines(memoryDc, text, boxWidthPx, textOptions.maxLines);
        if (boxWidthPx <= 0) {
            // Ширины не задано — измеряем сами, иначе выравнивание по центру и
            // по правому краю считать не от чего.
            for (const GdiLine& line : layout.lines) {
                boxWidthPx = std::max(boxWidthPx, measureLinePx(memoryDc, text.substr(line.begin, line.length)));
            }
        }

        std::wstring lastLine;
        if (layout.overflow && textOptions.ellipsis && !layout.lines.empty()) {
            lastLine = gdiLastLineWithEllipsis(memoryDc, text, layout.lines.back(), boxWidthPx);
        }

        const int totalHeight = lineHeightPx * static_cast<int>(layout.lines.size());
        const float areaTop = area.top;
        const float areaBottom = area.top + area.height();
        int y = 0;
        switch (textOptions.vertical) {
            case TextVerticalAlign::Top:
                y = toPixels(areaTop, dpiY);
                break;
            case TextVerticalAlign::Center:
                y = toPixels(areaTop, dpiY) + (toPixels(areaBottom, dpiY) - toPixels(areaTop, dpiY) - totalHeight) / 2;
                break;
            case TextVerticalAlign::Bottom:
                y = toPixels(areaBottom, dpiY) - totalHeight;
                break;
        }

        const COLORREF previousColor = ::SetTextColor(memoryDc, toColorRef(brush.color));
        for (std::size_t index = 0; index < layout.lines.size(); ++index) {
            RECT line{};
            line.left = toPixels(area.left, dpiX);
            line.right = line.left + boxWidthPx;
            line.top = y;
            line.bottom = y + lineHeightPx;
            std::wstring_view piece = text.substr(layout.lines[index].begin, layout.lines[index].length);
            if (index + 1 == layout.lines.size() && !lastLine.empty()) piece = std::wstring_view(lastLine);
            if (piece.empty()) {
                y += lineHeightPx;
                continue;
            }

            // Многоточие уже вставлено вручную (gdiLastLineWithEllipsis), и
            // DT_END_ELLIPSIC-обрезка GDI поверх дала бы второе многоточие.
            UINT flags = DT_SINGLELINE | DT_NOPREFIX;
            if (textOptions.rightToLeft) flags |= DT_RTLREADING;
            switch (textOptions.horizontal) {
                case TextAlign::Leading:
                    flags |= textOptions.rightToLeft ? DT_RIGHT : DT_LEFT;
                    break;
                case TextAlign::Center:
                    flags |= DT_CENTER;
                    break;
                case TextAlign::Trailing:
                    flags |= textOptions.rightToLeft ? DT_LEFT : DT_RIGHT;
                    break;
            }
            ::DrawTextW(memoryDc, piece.data(), static_cast<int>(piece.size()), &line, flags);
            y += lineHeightPx;
        }
        ::SetTextColor(memoryDc, previousColor);
        ::SelectObject(memoryDc, previousFont);
    }
};

// ---------------------------------------------------------------------------
// Renderer: тонкий слой над Impl
// ---------------------------------------------------------------------------

Renderer::Renderer()
    : impl_(std::make_unique<Impl>()) {
}

Renderer::~Renderer() = default;

Renderer::Renderer(Renderer&& other) noexcept = default;

Renderer& Renderer::operator=(Renderer&& other) noexcept = default;

bool Renderer::create(HWND window, const RendererOptions& options) {
    destroy();
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->init(window, options);
}

void Renderer::destroy() noexcept {
    if (impl_) impl_->releaseAll();
}

bool Renderer::ready() const noexcept {
    return impl_ && impl_->backend != Backend::None;
}

Backend Renderer::backend() const noexcept {
    return impl_ ? impl_->backend : Backend::None;
}

const FeatureProbe& Renderer::features() const noexcept {
    static const FeatureProbe empty{};
    return impl_ ? impl_->probe : empty;
}

bool Renderer::hardwareAccelerated() const noexcept {
    return impl_ && impl_->probe.hardwareAcceleration;
}

std::string Renderer::lastError() const {
    return impl_ ? impl_->lastErrorText : std::string{};
}

std::int64_t Renderer::lastHresult() const noexcept {
    return impl_ ? impl_->lastErrorHr : 0;
}

std::string Renderer::describe() const {
    if (!impl_) return "renderer: не создан";
    char buffer[512] = {};
    const FeatureProbe& probe = impl_->probe;
    const int written = std::snprintf(buffer, sizeof(buffer),
                                      "backend=%s driver=%s featureLevel=%d_%d hwAccel=%d bgra=%d d2d=%d dwrite=%d "
                                      "size=%ux%u dpi=%.0f/%.0f recreates=%u",
                                      backendName(impl_->backend), driverKindName(probe.driver), probe.featureLevelMajor,
                                      probe.featureLevelMinor, probe.hardwareAcceleration ? 1 : 0,
                                      probe.bgraSupport ? 1 : 0, probe.d2dDeviceAvailable ? 1 : 0,
                                      probe.directWriteAvailable ? 1 : 0, impl_->widthPx, impl_->heightPx, impl_->dpiX,
                                      impl_->dpiY, impl_->recreateAttempts);
    if (written <= 0) return "renderer: описание не сформировано";
    std::string text(buffer, static_cast<std::size_t>(written));
    if (!probe.adapterName.empty()) {
        text += " adapter=";
        text += probe.adapterName;
    }
    if (!probe.note.empty()) {
        text += " note=";
        text += probe.note;
    }
    return text;
}

bool Renderer::beginFrame() {
    return impl_ && impl_->beginFrame(nullptr);
}

bool Renderer::beginFrame(const Color& clearColor) {
    return impl_ && impl_->beginFrame(&clearColor);
}

bool Renderer::endFrame() {
    return impl_ && impl_->endFrame();
}

bool Renderer::frameActive() const noexcept {
    return impl_ && impl_->frameActive;
}

bool Renderer::resize(std::uint32_t widthPx, std::uint32_t heightPx) {
    if (!impl_) return false;
    Impl& impl = *impl_;
    if (impl.backend == Backend::None) return false;
    if (widthPx == 0 || heightPx == 0) {
        // Свёрнутое окно: DXGI запрещает ResizeBuffers с нулевым размером, а
        // Present всё равно не будет вызван (нет WM_PAINT). Отмечаем нулевой
        // размер, чтобы кадр стал пустой операцией.
        impl.widthPx = 0;
        impl.heightPx = 0;
        return false;
    }
    if (widthPx == impl.widthPx && heightPx == impl.heightPx) return true;

    // Ссылки на буфер до ResizeBuffers обязаны быть отпущены: DXGI вернёт
    // E_INVALIDARG, если буфер ещё занят.
    ComPtr<IDXGISurface> surface;
    if (impl.swapChain) {
        (void)impl.swapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));
    }
    impl.releaseSurface();
    surface.reset();

    impl.widthPx = widthPx;
    impl.heightPx = heightPx;

    if (impl.swapChain) {
        const HRESULT hr = impl.swapChain->ResizeBuffers(0, widthPx, heightPx, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return impl.fail("IDXGISwapChain1::ResizeBuffers", hr);
    }

    // Пока путь Direct2D, пересоздаём только цель рисования. Если таргет не
    // поднялся, окно не теряется: уходим на GDI поверх того же буфера, и уже
    // для него нужна поверхность в памяти — поэтому обе ветки сходятся на
    // buildGdiSurface() ниже, а не возвращаются по отдельности.
    if (impl.backend == Backend::Direct2D && impl.buildD2DTarget()) return true;
    if (impl.backend == Backend::Direct2D) {
        impl.backend = Backend::GdiDxgi;
        impl.d2dContext.reset();
        impl.d2dDevice.reset();
        impl.d2dFactory.reset();
        impl.dwriteFactory.reset();
        impl.clearCache();
    }
    if (impl.backend == Backend::GdiDxgi && !impl.ensureBackBufferDc()) {
        impl.backend = Backend::GdiWindow;
    }
    if (!impl.buildGdiSurface()) return false;
    return true;
}

void Renderer::setDpi(float dpiX, float dpiY) {
    if (!impl_) return;
    Impl& impl = *impl_;
    const float x = dpiX > 0.0F ? dpiX : kDipsPerInch;
    const float y = dpiY > 0.0F ? dpiY : kDipsPerInch;
    if (x == impl.dpiX && y == impl.dpiY) return;
    impl.dpiX = x;
    impl.dpiY = y;
    if (impl.backend == Backend::Direct2D && impl.d2dContext) {
        // Цель и её единицы задаются в пикселях и DIP, поэтому после смены DPI
        // цель пересоздаётся, а не только перенастраивается.
        if (!impl.buildD2DTarget()) {
            impl.fail("ID2D1DeviceContext::CreateBitmapFromDxgiSurface", E_FAIL);
        }
    }
}

float Renderer::dpiX() const noexcept {
    return impl_ ? impl_->dpiX : kDipsPerInch;
}

float Renderer::dpiY() const noexcept {
    return impl_ ? impl_->dpiY : kDipsPerInch;
}

float Renderer::dipToPixelX(float dip) const noexcept {
    return dip * dpiX() / kDipsPerInch;
}

float Renderer::dipToPixelY(float dip) const noexcept {
    return dip * dpiY() / kDipsPerInch;
}

float Renderer::pixelToDipX(float pixel) const noexcept {
    return pixel * kDipsPerInch / dpiX();
}

float Renderer::pixelToDipY(float pixel) const noexcept {
    return pixel * kDipsPerInch / dpiY();
}

Size Renderer::clientPixels() const {
    if (!impl_) return Size{};
    return Size{static_cast<float>(impl_->widthPx), static_cast<float>(impl_->heightPx)};
}

Size Renderer::clientDip() const {
    if (!impl_) return Size{};
    return Size{static_cast<float>(impl_->widthPx) * kDipsPerInch / impl_->dpiX,
                static_cast<float>(impl_->heightPx) * kDipsPerInch / impl_->dpiY};
}

BrushId Renderer::createSolidBrush(const Color& color) {
    return impl_ ? impl_->addBrush(color) : kInvalidBrush;
}

void Renderer::releaseBrush(BrushId brush) {
    if (!impl_) return;
    if (brush == kInvalidBrush || brush > impl_->brushes.size()) return;
    Impl::Brush& entry = impl_->brushes[brush - 1];
    if (entry.gdi != nullptr) ::DeleteObject(entry.gdi);
    impl_->brushes[brush - 1] = Impl::Brush{};
}

FontId Renderer::createFont(const FontSpec& spec) {
    return impl_ ? impl_->addFont(spec) : kInvalidFont;
}

void Renderer::releaseFont(FontId font) {
    if (!impl_) return;
    if (font == kInvalidFont || font > impl_->fonts.size()) return;
    Impl::Font& entry = impl_->fonts[font - 1];
    if (entry.gdiFont != nullptr) ::DeleteObject(entry.gdiFont);
    impl_->fonts[font - 1] = Impl::Font{};
}

FontMetrics Renderer::fontMetrics(FontId font) const {
    static const FontMetrics empty{};
    if (!impl_ || impl_->backend == Backend::None) return empty;
    Impl::Font* entry = impl_->fontById(font);
    if (entry == nullptr) return empty;
    return impl_->metricsOf(*entry);
}

Size Renderer::measureText(FontId font, std::string_view textUtf8, float maxWidthDip) const {
    return measureTextUtf16(font, platform::toUtf16(textUtf8), maxWidthDip);
}

Size Renderer::measureTextUtf16(FontId font, std::wstring_view textUtf16, float maxWidthDip) const {
    static const Size empty{};
    if (!impl_ || impl_->backend == Backend::None) return empty;
    Impl::Font* entry = impl_->fontById(font);
    if (entry == nullptr) return empty;
    return impl_->measureTextUtf16(*entry, textUtf16, maxWidthDip);
}

void Renderer::clear(const Color& color) {
    if (impl_) impl_->clear(color);
}

void Renderer::fillRect(const Rect& rect, BrushId brush) {
    if (impl_) impl_->fillRect(rect, brush);
}

void Renderer::fillRoundedRect(const Rect& rect, float radiusDip, BrushId brush) {
    if (impl_) impl_->fillRoundedRect(rect, radiusDip, brush);
}

void Renderer::fillEllipse(const Rect& rect, BrushId brush) {
    if (impl_) impl_->fillEllipse(rect, brush);
}

void Renderer::strokeRect(const Rect& rect, BrushId brush, float strokeWidthDip) {
    if (impl_) impl_->strokeRect(rect, brush, strokeWidthDip);
}

void Renderer::strokeLine(Point from, Point to, BrushId brush, float strokeWidthDip) {
    if (impl_) impl_->strokeLine(from, to, brush, strokeWidthDip);
}

void Renderer::drawText(std::string_view textUtf8, const Rect& area, FontId font, BrushId brush,
                        const TextOptions& options) {
    if (!impl_) return;
    drawTextUtf16(platform::toUtf16(textUtf8), area, font, brush, options);
}

void Renderer::drawTextUtf16(std::wstring_view textUtf16, const Rect& area, FontId font, BrushId brush,
                             const TextOptions& options) {
    if (!impl_) return;
    Impl::Font* entry = impl_->fontById(font);
    if (entry == nullptr) return;
    Impl::Brush* paint = impl_->brushById(brush);
    if (paint == nullptr) return;
    impl_->drawTextUtf16(textUtf16, area, *entry, *paint, options);
}

bool Renderer::recreate() {
    return impl_ ? impl_->rebuild() : false;
}

std::uint32_t Renderer::recreateAttempts() const noexcept {
    return impl_ ? impl_->recreateAttempts : 0;
}

bool Renderer::blitToHdc(HDC dc) {
    if (!impl_ || impl_->backend == Backend::Direct2D) return false;
    impl_->blitTo(dc);
    return true;
}

HWND Renderer::window() const noexcept {
    return impl_ ? impl_->window : nullptr;
}

}  // namespace mrproper::ui::render
