// MrProper — рендерер графики собственной природы: Direct2D + DirectWrite поверх
// DXGI swap chain, с откатом на GDI. Спека: §7 (ADR-003, гибрид D2D и
// нативных контролов), §5 (совместимость: «fallback для старых драйверов
// (D2D → GDI)»), §10 (риск «старые GPU/драйверы ломают Direct2D», митигация
// «fallback на GDI для схем и текста, feature-probe, тест на VM без
// аппаратного ускорения»), §6.1 (UI — единственный поток с сообщениями, рендер
// и ввод; без блокировок и I/O), §6.2 (`ui::*` — «окна, вкладки, рендер, темы,
// локализация, навигация»), §9 (src/ui: «app-shell, views, d2d renderer, theme,
// i18n, a11y»), §9.1 ADR-003.
//
// --- Зачем модуль такой границы ----------------------------------------------
//
// ADR-003 сознательно оставляет D2D только для графики, которой нет в
// нативных контролах: карта разделов (вложенные полосы), кольцевые и линейные
// диаграммы, анимация прогресса (FR-2, §7). Списки остаются на
// `SysListView32` + `NM_CUSTOMDRAW`, и их отрисовку этот модуль не
// обслуживает. Поэтому здесь нет ни скролла, ни выделения, ни клавиатурной
// навигации: это ровно тот объём, который ADR-003 вынес из Direct2D в
// нативные контролы, чтобы не платить за него втрое (SPEC §7).
//
// Модуль отвечает на четыре вопроса и больше ни за что:
//
//   1) «можно ли на этой машине рисовать через D2D?» — feature-probe до
//      создания окна: D3D11 с BGRA-поддержкой (без неё D2D-поверх D3D11
//      невозможен в принципе), DirectWrite, затем WARP, затем reference;
//   2) «на чём именно рисуем?» — Direct2D (аппаратный путь, включая WARP —
//      это Direct2D программно, без потери API) либо GDI;
//   3) «как восстановиться?» — D2DERR_RECREATE_TARGET и DXGI_ERROR_DEVICE_REMOVED
//      обрабатываются пересозданием ресурсов, а не падением процесса (SPEC §5:
//      «ни один отказ не роняет процесс»);
//   4) «как в него рисовать?» — примитивы в DIP (1/96 дюйма) с пересчётом в
//      пиксели по DPI, кисти и шрифты как маленький кэш, чтобы не создавать
//      COM-объекты в кадре.
//
// Чего модуль НЕ делает и кто делает это вместо него:
//
//   * палитру, шрифты по системной теме и DPI-темы — соседний `ui::Theme`
//     (задача 68). Здесь только `FontSpec` и `Color` как значения: рендерер
//     не знает, откуда взялся цвет;
//   * локализацию строк — соседний `ui::Locale` (задача 69). Текст приходит
//     готовым, в UTF-8 или UTF-16;
//   * содержимое экранов (карта дисков, дерево очистки, отчёт) — задачи
//     71-74. Здесь нет ни одного знания о дисках, категориях и правилах;
//   * окно, класс окна, WM_DPICHANGED, обработку сообщений — соседний
//     `ui::AppShell` (задача 66). Рендерер владеет HWND только как цель
//     для swap chain и не трогает его стили, кроме одного флага, который
//     описан в RendererOptions::setTopmostStyle.
//
// --- Границы заголовка -------------------------------------------------------
//
// Заголовочный файл включает <windows.h> (в нём живёт HWND в контракте
// `create`), но НЕ включает d2d1/dwrite/dxgi: COM-интерфейсы наружу не
// выпускаются, всё D2D-закрытое живёт в pimpl. Причина практическая —
// соседние файлы слоя (app_shell, view_disks, theme) подключают этот
// заголовок вместе со своими Win32-сообщениями, и разница между
// «нужен HWND» и «нужен весь d2d1_1.h» тут означала бы либо утечку D2D
// в объявления всех экранов, либо несовместимость с их собственными
// заголовками.
//
// Единицы длины — DIP во всём публичном API. Причина: §7 и §12 требуют
// одинаковой вёрстки при 100 %, 125 %, 150 % и 200 % DPI, а вёстка в
// пикселях означала бы, что каждый экран сам пересчитывает шрифты и отступы.
// Пиксели появляются ровно в двух местах: `resize` (размер буфера swap chain
// задаётся в пикселях) и `clientPixels` (для отчёта о размере окна).
#pragma once

#include <windows.h>  // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mrproper::ui::render {

// ---------------------------------------------------------------------------
// Метрики: DIP и пиксели
// ---------------------------------------------------------------------------

// DIP, заданный спецификацией: 96 DIP на дюйм при 100 % масштабе. Константа
// нужна обеим подсистемам — и D2D (D2D1_MATRIX_3X2_F / dpi), и GDI (пересчёт
// lfHeight), — иначе они считают разные точки.
inline constexpr float kDipsPerInch = 96.0F;

struct Point {
    float x{};
    float y{};
};

struct Size {
    float width{};
    float height{};

    [[nodiscard]] bool isEmpty() const noexcept {
        return width <= 0.0F || height <= 0.0F;
    }
};

struct Rect {
    float left{};
    float top{};
    float right{};
    float bottom{};

    [[nodiscard]] float width() const noexcept {
        return right - left;
    }
    [[nodiscard]] float height() const noexcept {
        return bottom - top;
    }
    [[nodiscard]] bool isEmpty() const noexcept {
        return right <= left || bottom <= top;
    }
    [[nodiscard]] Point center() const noexcept {
        return Point{left + width() / 2.0F, top + height() / 2.0F};
    }
    [[nodiscard]] bool contains(Point point) const noexcept {
        return point.x >= left && point.x < right && point.y >= top && point.y < bottom;
    }
};

// Цвет — всегда в sRGB с альфой 0..1. Альфа нужна не «на будущее»: сцена
// §7 рисует полупрозрачные подложки (полоса прогресса поверх карты, затемнение
// неактивной ветки), а D2D1_ALPHA_MODE_PREMULTIPLIED требует premultiplied
// значения — то есть цвет должен приходить уже в том виде, в каком его
// наложит блендер, иначе края будут темнее или светлее фона.
struct Color {
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};

    [[nodiscard]] static Color rgb(float r, float g, float b) noexcept {
        return Color{r, g, b, 1.0F};
    }

    [[nodiscard]] static Color rgba(float r, float g, float b, float a) noexcept {
        return Color{r, g, b, a};
    }

    // 0xRRGGBB (как Win32-константы в теме) → Color с полной альфой.
    [[nodiscard]] static Color fromRgbHex(std::uint32_t hex) noexcept {
        return Color{static_cast<float>((hex >> 16) & 0xFFU) / 255.0F,
                     static_cast<float>((hex >> 8) & 0xFFU) / 255.0F, static_cast<float>(hex & 0xFFU) / 255.0F, 1.0F};
    }
};

// ---------------------------------------------------------------------------
// Путь рисования и feature-probe (§5, §10)
// ---------------------------------------------------------------------------

// Что реально рисует рендерер. Direct2D — целевой путь; GdiDxgi — компромисс
// для машин, где D3D11 и DXGI есть, а D2D поверх D3D11 не поднялся (старый
// драйвер, удалённый рабочий стол, WARP без BGRA); GdiWindow — последний
// рубеж, когда не работает даже D3D11: та же отрисовка GDI, но прямо на
// клиентскую область окна, без swap chain.
enum class Backend : std::uint32_t {
    None = 0,
    Direct2D = 1,
    GdiDxgi = 2,
    GdiWindow = 3,
};

const char* backendName(Backend backend) noexcept;

// Драйвер D3D11, на котором поднялся рендерер. Различение важно для §10:
// WARP — это не «ускорение», но это Direct2D, и на VM без GPU (§11, e2e на
// эталонных образах) именно WARP позволяет прогнать тот же код, что и на
// обычной машине.
enum class DriverKind : std::uint32_t {
    None = 0,
    Hardware = 1,  // D3D_DRIVER_TYPE_HARDWARE
    Warp = 2,      // D3D_DRIVER_TYPE_WARP
    Reference = 3,  // D3D_DRIVER_TYPE_REFERENCE (программный растеризатор D3D11)
};

const char* driverKindName(DriverKind driver) noexcept;

// Результат feature-probe. Заполняется один раз при создании рендерера и
// целиком уходит в лог вместе с текстом отказа: по §12 отказ обязан быть
// виден в логе с кодом, а «Direct2D не поднялся» без причины — это ровно
// тот случай, который потом не воспроизвести.
struct FeatureProbe {
    bool d3d11Available{};  // удалось создать хоть какое-то устройство D3D11
    bool bgraSupport{};     // D3D11_CREATE_DEVICE_BGRA_SUPPORT: без него D2D поверх D3D11 невозможен
    bool d2d1FactoryAvailable{};   // D2D1CreateFactory
    bool directWriteAvailable{};   // DWriteCreateFactory
    bool d2dDeviceAvailable{};     // ID2D1Factory1::CreateDevice поверх DXGI-устройства
    DriverKind driver{DriverKind::None};
    int featureLevelMajor{};  // 11 для 11_0; 0 — неизвестно
    int featureLevelMinor{};
    bool hardwareAcceleration{};  // D3D_DRIVER_TYPE_HARDWARE (не WARP, не reference)

    // Описание адаптера в UTF-8. Пустое на программных драйверах — это
    // нормально, а не ошибка. Строковой версии драйвера здесь нет намеренно:
    // в установленном SDK она появляется только в DXGI_ADAPTER_DESC2, которого
    // нет в заголовках, и выдумывать пустое поле ради симметрии с отчётом
    // dxdiag незачем.
    std::string adapterName;
    std::uint32_t vendorId{};
    std::uint32_t deviceId{};

    // Человекочитаемая причина текущего выбора: «D2D: WARP, аппаратного
    // ускорения нет» или «D2D недоступен (D3D11_CREATE_DEVICE_BGRA_SUPPORT
    // не поддержан) → GDI». Пишется в лог при создании рендерера.
    std::string note;

    // Можно ли рисовать через D2D: есть устройство D3D11 с BGRA и D2D-устройство
    // поверх него. Проверяется отдельно от d3d11Available, потому что GDI-путь
    // не требует D3D11 вовсе.
    [[nodiscard]] bool direct2dUsable() const noexcept {
        return d3d11Available && bgraSupport && d2d1FactoryAvailable && d2dDeviceAvailable;
    }
};

// Probe без окна и без swap chain: создаёт временное устройство D3D11, проверяет
// D2D-устройство поверх него и отпускает всё. Нужен вызывающему (app_shell,
// экран «О программе», диагностика) ДО создания окна, чтобы не открывать
// окно, на котором рисовать нечем, и чтобы решение «Direct2D или GDI» было
// видно в логе раньше первого кадра. Ничего не бросает: неудача — это
// заполненная структура с d3d11Available == false.
FeatureProbe probeFeatures() noexcept;

// ---------------------------------------------------------------------------
// Опции создания
// ---------------------------------------------------------------------------

struct RendererOptions {
    // Пробовать аппаратный путь первым. false имеет смысл только для отладки:
    // на машине с испорченным драйвером WARP-путь даёт тот же результат
    // предсказуемо.
    bool preferHardware{true};

    // WARP как запасной вариант. Без него машина без GPU уходит на GDI, и на
    // эталонной VM (§11) весь код D2D остаётся непроверенным.
    bool allowWarpFallback{true};

    // Финальный откат на GDI (§5, §10). Выключать только при отладке самого
    // отката: без него старая видеокарта превращает приложение в чёрное окно.
    bool allowGdiFallback{true};

    // DXGI_PRESENT: ждать вертикальной развёртки (1) или показывать кадр сразу
    // (0). По умолчанию — ожидание: у приложения с анимацией прогресса (FR-6)
    // без него кадры идут быстрее монитора, а монитор показывает последний.
    // На GDI-пути флаг не действует — там вертикальной развёртки нет.
    bool waitForVerticalBlank{true};

    // Поставить WS_EX_TOPMOST окну перед созданием swap chain: без него
    // flip-модельная цепочка на дочернем окне в некоторых сборках Windows
    // отдаёт пустой кадр. Стиль меняется у окна, которым владеет app_shell,
    // поэтому флаг вынесен наружу: у кого-то своё окно без верхнего края —
    // тот выключает, и рендерер ничего не трогает.
    bool setTopmostStyle{true};

    // Метод present для swap chain. По умолчанию перебираются flip-модельные
    // варианты, а DISCARD — последним, потому что он работает везде и стоит
    // дороже. Явное значение нужно для проверки «а работает ли без flip».
    bool allowFlipModel{true};
};

// ---------------------------------------------------------------------------
// Кисти, шрифты, текст
// ---------------------------------------------------------------------------

// Кисти и шрифты — дескрипторы в маленьком кэше рендерера, а не COM-объекты в
// руках вызывающего. Причина практическая: кадр рисуется на UI-потоке и не
// имеет права падать из-за того, что кто-то забыл Release(); кэш же живёт
// вместе с рендерером и освобождается в его деструкторе. Ноль — «не создан».
using BrushId = std::uint32_t;
using FontId = std::uint32_t;
inline constexpr BrushId kInvalidBrush = 0;
inline constexpr FontId kInvalidFont = 0;

struct FontSpec {
    // UTF-8, как у всех строк проекта (§5: локализация ru + en). Пусто — шрифт
    // по умолчанию системы: для DirectWrite это «Segoe UI», для GDI — тот же
    // список, что и у SystemParametersInfo(SPI_GETNONCLIENTMETRICS). Явное
    // имя важно: у GDI и DWrite список доступных шрифтов различается, и без
    // него один и тот же экран на двух путях выглядел бы по-разному.
    std::string family;

    // Имя локали для DirectWrite (UTF-8, например «ru-RU»). Пусто — локаль
    // пользователя по умолчанию (GetUserDefaultLocaleName). Это не украшение:
    // от локали зависят правила подстановки и ширина цифр, то есть ровно то,
    // из-за чего «нет обрезанных строк при 150 % DPI» (§12) ломается в первую
    // очередь.
    std::string localeName;

    float sizeDip{14.0F};
    int weight{400};  // DWRITE_FONT_WEIGHT_*: 400 normal, 600 semibold, 700 bold
    bool italic{false};
};

struct FontMetrics {
    float ascentDip{};
    float descentDip{};
    float lineHeightDip{};  // строка целиком, включая интерлиньяж
};

enum class TextAlign : std::uint32_t {
    Leading = 0,  // по левому краю; в RTL-ready вёрстке (SPEC §5) это «по логическому началу»
    Center = 1,
    Trailing = 2,
};

enum class TextVerticalAlign : std::uint32_t {
    Top = 0,
    Center = 1,
    Bottom = 2,
};

struct TextOptions {
    TextAlign horizontal{TextAlign::Leading};
    TextVerticalAlign vertical{TextVerticalAlign::Top};

    // Строк не больше указанного. 0 — без ограничения: нужно экрану отчёта,
    // где ячейка переносится по мере роста окна.
    int maxLines{1};

    // Перенос по словам. По умолчанию выключен: подпись на полосе карты
    // дисков переноситься не должна, а обрезаться с многоточием (§12 — «нет
    // обрезанных строк при 150 % DPI», но это про поля, а не про подпись).
    bool wordWrap{false};

    // Многоточие, когда текст не влезает в maxLines. По умолчанию включено:
    // подпись, которая молча обрезается на середине слова, читается как
    // ошибка вёрстки.
    bool ellipsis{true};

    // Направление абзаца справа налево (§5: «ru + en, RTL-ready»). Решения о
    // языке принимает соседний ui::Locale (задача 69); рендерер только
    // применяет: у DirectWrite меняет направление чтения и переворачивает
    // выравнивание (Leading ↔ Trailing), у GDI добавляет DT_RTLREADING.
    // Менять локаль вручную не нужно — она приходит в FontSpec.
    bool rightToLeft{false};
};

// ---------------------------------------------------------------------------
// Рендерер
// ---------------------------------------------------------------------------

// Сколько раз подряд разрешено пересоздавать ресурсы после потери устройства,
// прежде чем рендерер уйдёт в GDI. Три попытки — с запасом на обычное
// «драйвер перевзошёл горячую переподключаемую видеокарту» (ноутбук в дока),
// но без бесконечного цикла: на действительно сломанном драйвере
// пересоздание не помогает никогда, и честная отрисовка через GDI лучше
// процесса, который каждые 16 мс пересоздаёт устройство.
inline constexpr std::uint32_t kMaxRecreateAttempts = 3;

// Владение ресурсами и контекст рисования. Не копируется и не переносится
// между потоками (§6.4: единственный поток с сообщениями, рендер и ввод).
// Порядок вызовов в кадре: beginFrame → примитивы → endFrame. Вне кадра
// примитивы не рисуют и возвращают false: забытый endFrame после WM_SIZE или
// после пересоздания устройства — обычная ошибка, и молчаливый вызов в пустоту
// её только закрепляет.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&& other) noexcept;

    // Создать графику для окна. Второй вызов без destroy() игнорируется и
    // возвращает false: окно у рендерера одно на всё время жизни.
    //
    // Что происходит внутри: попытка D3D11 (hardware → WARP → reference),
    // проверка BGRA, D2D-устройство, DirectWrite, swap chain, D2D-таргет. При
    // любой неудаче цепочка идёт вниз до GDI (§5). В GDI-режиме на окне рисуем
    // только после успешного beginFrame, а endFrame сам выводит кадр в
    // клиентскую область — если вызывающий рисует сам из WM_PAINT, он
    // вызывает blitToHdc() и не вызывает endFrame() дважды.
    //
    // false означает, что рисовать нечем: в lastError() — причина, в лог она
    // уже записана. Причины: нулевой HWND, нулевой размер клиентской области
    // (окно ещё не показано — вызовите позже), allowGdiFallback == false вместе
    // с неудачным D2D.
    bool create(HWND window, const RendererOptions& options = RendererOptions{});

    // Отпустить всю графику. Безопасно вызывать всегда, в том числе после
    // неудачного create() и дважды. После destroy() объект снова пригоден к
    // create() — это нужно при пересоздании окна.
    void destroy() noexcept;

    // Готов ли рендерер к рисованию.
    [[nodiscard]] bool ready() const noexcept;

    [[nodiscard]] Backend backend() const noexcept;
    [[nodiscard]] const FeatureProbe& features() const noexcept;

    // Аппаратное ли ускорение на самом деле. Различение «D2D» и «D2D на
    // WARP» нужно отчёту об ошибках и e2e-прогону на VM без GPU (§11).
    [[nodiscard]] bool hardwareAccelerated() const noexcept;

    // Текст последнего отказа в UTF-8 и его HRESULT. Пустая строка и 0 — отказа
    // не было. HRESULT хранится как знаковый int64, потому что так его пишет
    // лог (core::logFailure) и так его читают без приведения типов.
    [[nodiscard]] std::string lastError() const;
    [[nodiscard]] std::int64_t lastHresult() const noexcept;

    // Одна строка для лога и для экрана «О программе»: путь, драйвер, версия
    // feature level, размер клиентской области, DPI.
    [[nodiscard]] std::string describe() const;

    // --- Кадр -----------------------------------------------------------------

    // Начать кадр: очистка surface последним заданным цветом (или прозрачным
    // для D2D) и открытие D2D-транзакции. false — рендерер не готов или уже
    // внутри кадра.
    bool beginFrame();

    // То же с явной очисткой: вызывающий задаёт цвет подложки сам (экран
    // рисует свою подложку целиком, а не полагается на дефолт).
    bool beginFrame(const Color& clearColor);

    // Закрыть кадр: EndDraw + Present. false означает «кадр не показан» —
    // resize, потеря устройства или окно свёрнуто. Это не ошибка: вызывающий
    // просто не планирует анимацию до следующего успешного кадра. Код отказа
    // при этом записан в lastError(), чтобы «кадры не идут» в логе не были
    // безмолвными.
    bool endFrame();

    // Открыт ли кадр прямо сейчас.
    [[nodiscard]] bool frameActive() const noexcept;

    // --- Геометрия окна и DPI (§5, per-monitor v2) ---------------------------

    // Новый размер клиентской области в пикселях, из WM_SIZE. Пересоздаёт буфер
    // swap chain и D2D-таргет. Вызывать при каждом изменении размера, включая
    // свёртывание (0x0): без этого Present на следующем кадре вернёт
    // DXGI_ERROR_INVALID_CALL. true — размер применён.
    bool resize(std::uint32_t widthPx, std::uint32_t heightPx);

    // DPI окна из WM_DPICHANGED. Меняет единицы D2D-таргета и пересоздаёт его,
    // потому что dpi — свойство таргета, а не трансформации.
    void setDpi(float dpiX, float dpiY);

    [[nodiscard]] float dpiX() const noexcept;
    [[nodiscard]] float dpiY() const noexcept;

    // DIP → пиксели и обратно. Нужны экранам, которые строят сетку в DIP, но
    // рисуют линии попиксельно (карта разделов, FR-2).
    [[nodiscard]] float dipToPixelX(float dip) const noexcept;
    [[nodiscard]] float dipToPixelY(float dip) const noexcept;
    [[nodiscard]] float pixelToDipX(float pixel) const noexcept;
    [[nodiscard]] float pixelToDipY(float pixel) const noexcept;

    [[nodiscard]] Size clientPixels() const;
    [[nodiscard]] Size clientDip() const;

    // --- Ресурсы --------------------------------------------------------------

    // Создать однотонную кисть. Дешёвая операция, но и в кадре создавать
    // ничего не надо: цветов на экране десятки, а кисть на каждый кадр — нет.
    // Возвращает kInvalidBrush, если рендерер не готов или памяти не хватило.
    [[nodiscard]] BrushId createSolidBrush(const Color& color);
    void releaseBrush(BrushId brush);

    // Создать шрифт. Возвращает kInvalidFont при отказе; экран обязан проверить,
    // иначе он рисует текст невидимым кэшем и «пустую» ячейку списка.
    [[nodiscard]] FontId createFont(const FontSpec& spec);
    void releaseFont(FontId font);

    // Метрики шрифта: нужны для вёрстки строк (высота строки, базовая линия),
    // иначе при 150 % DPI подписи наезжают друг на друга (§12).
    [[nodiscard]] FontMetrics fontMetrics(FontId font) const;

    // Размер текста в DIP. maxWidthDip > 0 — с переносом по ширине. Точность
    // двух путей различается на доли пикселя (DWrite — хинтинг по шрифту, GDI —
    // по устройству), поэтому результат используется для вёрстки, а не для
    // сравнения с эталоном.
    [[nodiscard]] Size measureText(FontId font, std::string_view textUtf8, float maxWidthDip = 0.0F) const;
    [[nodiscard]] Size measureTextUtf16(FontId font, std::wstring_view textUtf16, float maxWidthDip = 0.0F) const;

    // --- Примитивы (все координаты и толщины в DIP) --------------------------

    void clear(const Color& color);
    void fillRect(const Rect& rect, BrushId brush);
    void fillRoundedRect(const Rect& rect, float radiusDip, BrushId brush);
    void fillEllipse(const Rect& rect, BrushId brush);
    void strokeRect(const Rect& rect, BrushId brush, float strokeWidthDip = 1.0F);
    void strokeLine(Point from, Point to, BrushId brush, float strokeWidthDip = 1.0F);
    void drawText(std::string_view textUtf8, const Rect& area, FontId font, BrushId brush,
                  const TextOptions& options = TextOptions{});
    void drawTextUtf16(std::wstring_view textUtf16, const Rect& area, FontId font, BrushId brush,
                       const TextOptions& options = TextOptions{});

    // --- Восстановление после потери устройства ------------------------------

    // Пересоздать D2D-ресурсы после D2DERR_RECREATE_TARGET или
    // DXGI_ERROR_DEVICE_REMOVED. Вызывающий делает это сам, в своём потоке и в
    // своём темпе: модуль не ставит таймеры и не трогает оконные сообщения.
    // Восстановление конечно — после N неудач подряд (kMaxRecreateAttempts)
    // рендерер честно уходит в GDI, потому что бесконечный цикл пересоздания
    // на сломанном драйвере хуже, чем рисование через GDI.
    bool recreate();

    // Сколько раз пересоздавались ресурсы. Ненулевое значение в логе — признак
    // проблемы с драйвером, и полезно в отчёте об ошибках.
    [[nodiscard]] std::uint32_t recreateAttempts() const noexcept;

    // --- GDI-путь: ручной вывод кадра ----------------------------------------

    // Перенести накопленный кадр в переданный DC. Нужен вызывающему, который
    // рисует из WM_PAINT (EndPaint даёт DC, освобождать который вызвавший
    // обязан сам). При GdiDxgi и Direct2D — ничего не делает, возвращает
    // false: там кадр выводит endFrame().
    bool blitToHdc(HDC dc);

    // Скрытые от глаз детали, которые e2e и «О программе» имеют право спросить.
    [[nodiscard]] HWND window() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mrproper::ui::render
