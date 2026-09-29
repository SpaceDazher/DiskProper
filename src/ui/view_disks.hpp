// MrProper — экран «Диски»: карта разделов на Direct2D и дерево
// «физический диск → разделы → тома» на SysListView32 с NM_CUSTOMDRAW.
//
// Спека: §4 FR-2 (дерево диск → раздел → том; набор полей на узел; визуализация
// горизонтальными полосами пропорционально размеру с видимой вложенностью
// «диск → разделы»; клик по разделу открывает карточку с деталями; экспорт карты
// в JSON и в текст; сортировка и фильтры «только с буквами / только системные /
// removable»), §7 и §7.1 п.2 (ADR-003: Direct2D — только для графики собственной
// природы, таблицы — нативные контролы с кастомной отрисовкой), §5
// (доступность, клавиатурная навигация, DPI per-monitor v2, локализация ru + en,
// тема светлая/тёмная), §6.1 и §6.4 (слой UI — единственный поток с сообщениями,
// рендер и ввод; снимок инвентаризации иммутабелен и приезжает готовым),
// §10 (диск может не ответить — экран показывает это, а не падает), §12
// (карта разделов совпадает с эталоном на MBR/GPT и мультидиск-VM).
//
// ---------------------------------------------------------------------------
// Почему файл разделён на два слоя, а не «просто экран»
// ---------------------------------------------------------------------------
//
// Слой экрана состоит из двух частей с очень разной ценой ошибки:
//
//   1. Модель (DisksViewModel, MapDiskModel, раскладка карты и раскладка окна) —
//      чистый C++ без единого Win32-типа. Это «что показываем»: дерево, поля
//      узла, фильтры, сортировка, геометрия полос карты и попадания мышью.
//      Здесь принимаются все решения, которые обязаны быть одинаковыми при
//      любом DPI, языке и теме, поэтому их можно проверить без окна (тот же
//      приём, что в nav.* — RailLayout и hitTest, и в cleanup.* —
//      CleanupLayout и CleanupViewModel). Числа занятости и свободного места
//      считает не этот слой, а core::disk_model (FR-1 п.6): экран и CLI должны
//      показывать одно и то же число для одного и того же тома (SPEC §11.4), а
//      две реализации одного правила разойдутся.
//
//   2. Окно (DisksScreen) — SysListView32 с NM_CUSTOMDRAW, окно карты
//      разделов с Direct2D-рендерером, карточка деталей, кнопки фильтров и
//      экспорта. По ADR-003 таблица рисуется нативным контролом, а полосы
//      карты — Direct2D: вручную нарисованный весь UI стоил бы втрое дороже
//      (SPEC §7), а нативный контрол без NM_CUSTOMDRAW не дал бы темной темы,
//      своих цветов занятости и значков шифрования.
//
// Почему дерево на ListView, а не на SysTreeView32: узел дерева FR-2 — это не
// одна подпись, а набор чисел (размер, занято/свободно, ФС, разряд, тип,
// шифрование, TRIM). SysTreeView32 даёт одну строку на узел и ноль колонок, то
// есть сортировать и сравнивать там нечем, а §7 ADR-003 прямо говорит, что
// таблицы — это SysListView32 + NM_CUSTOMDRAW. Вложенность в списке делается
// штатным средством: изображение-отступ переменной ширины у первого столбца
// (LVSIL_SMALL), поэтому отступ настоящий, а не «пробелы в тексте».
//
// ---------------------------------------------------------------------------
// Границы слоёв
// ---------------------------------------------------------------------------
//
// Зависимости ровно одна и только вниз: core::disk_model (модель дисков и
// арифметика занятости), core::report_json (экспорт карты, FR-2) и
// ui::nav (PageState страницы). Ни engine, ни platform, ни scanner в этом файле
// быть не должно: инвентаризация приходит мостом (mv_bridge, задача 75) как
// готовый неизменяемый снимок — §6.4 прямо запрещает мутировать результаты
// после публикации, а обход дисков с таймаутами на устройство (FR-1) в UI не
// живёт никогда. Исключение (ShellExecute) в экран не проникает: «Открыть в
// проводнике» — это обратный обработчик, экран только собирает путь.
//
// Что экран сознательно НЕ делает:
//   * не опрашивает устройства и не реагирует на WM_DEVICECHANGE — это кэш
//     platform::inventory и мост, который зовёт publishInventory();
//   * не пишет файлы при экспорте — FR-2 требует «экспорт карты», но место
//     экспорта выбирает вызывающий (§6.1: UI-поток не занимается I/O);
//   * не реализует «Проверить» (FR-2 помечает кнопку как v1.1) — кнопка есть и
//     выключена, с подписью; включение без проверки диска было бы обещанием;
//   * не рисует карту на GDI, если Direct2D поднялся, и не выключает Direct2D
//     на живом D2D-адаптере: откат на GDI — решение рендерера (SPEC §5), и
//     последний рубеж (полностью свой GDI-рисунок) включается только когда
//     рендерер не смог создаться вовсе.
//
// ---------------------------------------------------------------------------
// Правила этого файла
// ---------------------------------------------------------------------------
//
//   * Поток — только UI (§6.1). Никаких блокировок и никакого I/O: publish*
//     зовутся из UI-потока после PostMessage от фонового потока.
//   * Исключение не пересекает границу Win32: статические процедуры окон ловят
//     всё сами (§5 «устойчивость»), поэтому хуки модели объявлены без noexcept,
//     а чистые функции (computeMap, DisksLayout) помечены noexcept.
//   * Каждый неуспех WinAPI и каждый отказ Direct2D пишется в журнал с кодом
//     ошибки (§5, §12): «карта не нарисовалась» без причины не чинится.
//   * Тексты — только через ui::locale (StringId, а не литерал). Единицы объёма
//     и числа — core::units; названия шины и типа раздела — core::disk_model
//     (там же, где они живут для CLI и отчёта, чтобы экран и отчёт говорили
//     одно и то же).
#pragma once

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой ui, по SPEC §7 это законное место

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/disk_model.hpp"
#include "nav.hpp"

namespace mrproper::ui::disks {

// ---------------------------------------------------------------------------
// Фильтры и сортировка (SPEC §4 FR-2: «Сортировка, фильтр „только с буквами /
// только системные / removable“»)
// ---------------------------------------------------------------------------
//
// Три независимых флага, а не один переключатель «режим»: FR-2 перечисляет
// именно три фильтра, и человек хочет «только системные И съёмные» чаще, чем
// «системные ИЛИ съёмные». Взаимоисключающий переключатель означал бы, что
// половина комбинаций недостижима.
//
// Смысл флагов на диске и на томе различается, и это указано в подписи поля:
// «только с буквами» оставляет диск, если хоть один его том смонтирован с
// буквы, «только системные» — если на диске есть системный том или системный
// раздел, «съёмные» — если сам диск помечен removable. Считает это модель по
// данным core::hasDriveLetter / core::isSystemPartition.
struct DiskFilters {
    bool letteredOnly{};   // «только с буквами» — есть том с буквой диска
    bool systemOnly{};     // «только системные» — есть системный раздел или том
    bool removableOnly{};  // removable — свойство диска (FR-1 п.2)

    [[nodiscard]] bool any() const noexcept {
        return letteredOnly || systemOnly || removableOnly;
    }

    friend bool operator==(const DiskFilters&, const DiskFilters&) = default;
};

// Ключ сортировки. Четыре, потому что четыре вопроса человек задаёт о списке
// дисков: «где я стою» (номер), «сколько там места» (размер), «где тесно»
// (свободно), «что это за железо» (модель).
enum class SortKey : std::uint8_t { Number, Size, FreeSpace, Model };
enum class SortDirection : std::uint8_t { Ascending, Descending };

struct SortOrder {
    SortKey key{SortKey::Number};
    SortDirection direction{SortDirection::Ascending};

    // Размер и свободное место по умолчанию читаются «побольше сверху», номер
    // и модель — «по возрастанию»: сортировка по номеру должна давать 0, 1, 2,
    // иначе дерево прыгает при каждой перечитке инвентаризации.
    [[nodiscard]] static SortOrder defaultFor(SortKey key) noexcept {
        SortOrder order;
        order.key = key;
        order.direction =
            (key == SortKey::Size || key == SortKey::FreeSpace) ? SortDirection::Descending
                                                               : SortDirection::Ascending;
        return order;
    }

    friend bool operator==(const SortOrder&, const SortOrder&) = default;
};

const char* toString(SortKey key) noexcept;

// ---------------------------------------------------------------------------
// Строка дерева «диск → раздел → том»
// ---------------------------------------------------------------------------

enum class NodeKind : std::uint8_t { Disk, Partition, Volume };

// Что рисует одна строка списка. Готовые строки, а не сырые числа: подпись
// зависит от языка интерфейса, а список перерисовывается целиком при смене
// языка (locale::revision), поэтому хранить перевод в строке, которая живёт
// дольше одного кадра, означало бы хранить устаревшую копию перевода.
struct TreeRow {
    NodeKind kind{NodeKind::Disk};

    // Стабильный ключ данных, а не индекс: индекс меняется при новой
    // инвентаризации, а выделение и раскрытие обязаны пережить и WM_DEVICECHANGE,
    // и перезапуск приложения (PageState, §5, §7.2).
    std::string key;  // "d:0", "d:0p:3", "d:0p:3v:0"

    std::string title;     // «Диск 0», «Раздел 3», «C: (Windows)»
    std::string freeText;  // столбец «Свободно»
    std::string usedText;  // столбец «Занято»

    std::uint64_t sizeBytes{};
    std::uint64_t freeBytes{};
    std::uint64_t usedBytes{};

    int diskNumber{-1};
    std::uint32_t partitionIndex{};
    int level{};  // 0, 1, 2 — уровень вложенности (глубина отступа)

    bool hasChildren{};
    bool expanded{true};
    bool system{};        // системный раздел или том (FR-2 «системный»)
    bool encrypted{};     // FR-1 п.7, FR-2 «шифрование»
    bool removable{};     // removable-диск
    bool lowSpace{};      // мало свободного места (core::needsFreeSpaceWarning)
    bool unavailable{};   // диск не ответил (FR-1: «устройство помечается
                          // недоступным, приложение не падает»)
    bool sizeKnown{true}; // размер не пришёл: показываем «н/д», а не ноль

    // Куда открывать в проводнике из этой строки: точка монтирования тома.
    // У раздела и диска пусто — там открывать нечего, и кнопка выключается.
    std::string explorerPath;
};

// ---------------------------------------------------------------------------
// Карта разделов: данные (модель) и геометрия (чистая функция)
// ---------------------------------------------------------------------------
//
// Модель отдаёт данные (смещение и длина каждого раздела, занятость тома), а
// геометрию считает computeMap — чистая функция без окна, без DPI и без
// Direct2D. Так карта проверяется обычным юнит-тестом, а Direct2D занимается
// только раскладкой примитивов по уже посчитанным прямоугольникам.

struct MapMetrics {
    double labelHeightDip{14.0};   // строка «Диск 0 · модель»
    double barHeightDip{16.0};     // полоса диска с сегментами разделов
    double fillHeightDip{6.0};     // полоса занятости тома внутри сегмента
    double captionHeightDip{12.0}; // подпись «Свободно … из …»
    double diskGapDip{10.0};       // зазор между дисками
    double paddingDip{6.0};        // поля карты слева и справа
    double segmentGapDip{1.0};     // зазор между сегментами (иначе их не различить)
    double minWidthDip{160.0};     // уже — подписи не помещаются, карта «тесная»
    double minLabelSegmentDip{48.0};  // ниже этой ширины подпись сегмента не рисуется
};

// Модель одной полосы: том внутри раздела внутри диска (FR-2 «вложенность
// „диск → разделы“ видна визуально»).
struct MapVolumeModel {
    std::string key;
    std::string label;
    std::uint64_t totalBytes{};
    std::uint64_t freeBytes{};
    bool known{};       // размер тома известен
    bool selected{};
    bool lowSpace{};
    bool encrypted{};
};

struct MapSegmentModel {
    std::string key;
    std::string label;
    std::uint64_t offsetBytes{};
    std::uint64_t lengthBytes{};
    bool unallocated{};  // неразмеченное место: это не раздел и не том
    bool system{};
    bool encrypted{};
    bool selected{};
    bool unavailable{};
    bool hasVolume{false};
    MapVolumeModel volume;
};

struct MapDiskModel {
    int diskNumber{-1};
    std::string label;     // «Диск 0»
    std::string subtitle;  // модель · шина
    std::string caption;   // «Свободно 123,4 ГБ из 512,0 ГБ»
    std::uint64_t sizeBytes{};
    bool unavailable{};
    bool selected{};
    std::vector<MapSegmentModel> segments;
};

// --- Геометрия ---

struct MapVolumeBar {
    std::string key;
    // Ключ сегмента-родителя. Хранится рядом с ключом тома, а не выводится из
    // него сравнением префиксов: «d:0p:1» является префиксом «d:0p:10v:0», и
    // полоса тома десятого раздела нарисовалась бы ещё и в первом.
    std::string parent;
    float left{};
    float top{};
    float width{};
    float height{};
    double usedFraction{};  // 0..1, доля занятого от размера тома
    bool known{};
    bool lowSpace{};
    bool selected{};
};

struct MapSegment {
    std::string key;
    std::string label;
    float left{};
    float top{};
    float width{};
    float height{};
    double fraction{};  // доля полосы диска, 0..1
    bool unallocated{};
    bool system{};
    bool encrypted{};
    bool selected{};
    bool unavailable{};
};

struct MapBar {
    int diskNumber{-1};
    std::string label;
    std::string subtitle;
    std::string caption;
    // Три полосы блока диска: подпись сверху, сама полоса с сегментами и подпись
    // «свободно … из …» под ней. Хранятся вместе с геометрией, а не
    // вычисляются художником: у художника их всё равно нет, а значит он
    // угадывал бы высоту строки заново на каждой карте.
    float labelTop{};
    float labelHeight{};
    float top{};
    float height{};
    float captionTop{};
    float captionHeight{};
    float left{};
    float width{};
    std::uint64_t sizeBytes{};
    bool unavailable{};
    bool selected{};
    std::vector<MapSegment> segments;
    std::vector<MapVolumeBar> volumes;
};

// Готовая раскладка карты в DIP. Значение ничего не владеет и копируется по
// значению, поэтому её можно хранить в обработчике WM_PAINT и сравнивать в
// тесте.
struct MapLayout {
    std::vector<MapBar> bars;
    float width{};
    float height{};
    // true — содержимое не поместилось в отведённую высоту и блоки дисков
    // сжаты. Показывать это нужно честно: иначе часть дисков молча исчезает с
    // карты, а дерево про них ещё напоминает.
    bool cramped{};

    // Что под указателем. Порядок проверки — от глубокого к широкому: полоса
    // тома лежит внутри сегмента раздела, сегмент — внутри полосы диска, и
    // клик по узкой полосе должен попасть в узел, а не в его родителя.
    [[nodiscard]] std::optional<std::string> hitTest(float x, float y) const;
};

// Высота карты под указанное число дисков. Раскладка окна спрашивает это, чтобы
// отдать карте ровно столько места, сколько ей нужно, а не «половину окна».
[[nodiscard]] float mapHeightDipFor(const MapMetrics& metrics, std::size_t diskCount) noexcept;

[[nodiscard]] MapLayout computeMap(const MapMetrics& metrics, const std::vector<MapDiskModel>& disks,
                                   float widthDip, float heightDip) noexcept;

// ---------------------------------------------------------------------------
// Раскладка окна
// ---------------------------------------------------------------------------

// Сколько кнопок в каждой группе. Числа, а не «сколько получилось»:
// раскладка обязана знать, сколько мест зарезервировать под фильтры и действия
// ещё до того, как контролы измерили свои подписи.
inline constexpr int kDisksFilterCount = 3;
inline constexpr int kDisksActionCount = 5;

struct DisksMetrics {
    double paddingDip{6.0};
    double buttonHeightDip{26.0};
    double buttonGapDip{6.0};
    double buttonPaddingDip{10.0};  // запас под подпись внутри кнопки
    double buttonMinWidthDip{72.0};
    double statusHeightDip{20.0};
    double cardTitleHeightDip{22.0};
    double cardLineHeightDip{17.0};
    double cardPaddingDip{8.0};
    double cardMinHeightDip{56.0};
    double mapMinHeightDip{80.0};
    double mapMaxHeightDip{360.0};
    double listMinHeightDip{100.0};
    double gapDip{6.0};
    double minWidthDip{560.0};
    double minHeightDip{320.0};
    double numericColumnDip{96.0};  // «Свободно» и «Занято» — числа, ширина фиксирована
};

// Прямоугольник в пикселях клиентской области. Правая и нижняя границы не
// включаются: соседние прямоугольники делят область без зазора и без двойного
// попадания (то же соглашение, что в RailRect и CleanupRect).
struct DisksRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(int px, int py) const noexcept;
};

// Что под указателем мыши. Отдельное перечисление, а не сравнение
// прямоугольников в WndProc: так путь «пиксели → действие» проверяется без
// окна.
enum class HitTarget : std::uint8_t {
    None,
    Filters,
    Actions,
    Status,
    Map,
    List,
    Card,
    FilterLetters,
    FilterSystem,
    FilterRemovable,
    ExportMap,       // JSON (FR-2: экспорт карты в JSON)
    ExportText,      // текст, для баг-репортов (FR-2)
    OpenInExplorer,
    Refresh,
    Check,           // FR-2 помечает «Проверить» как v1.1: кнопка есть и выключена
};

// Раскладка экрана. Чистая функция от метрик, DPI, размеров окна и размеров
// подписей кнопок: значение ничего не владеет и копируется по значению.
//
// Ширины кнопок приходят снаружи (после измерения текста контролом): надписи
// живут в каталоге строк, они по-русски и по-английски разной длины, а
// фиксированная ширина кнопки означала бы либо обрезанную подпись (§12), либо
// пустое место. Здесь они только распределяются: если места не хватает, все
// кнопки сжимаются пропорционально, но не ниже buttonMinWidthDip.
class DisksLayout {
public:
    DisksLayout() = default;

    static DisksLayout compute(const DisksMetrics& metrics, int dpi, int clientWidthPx, int clientHeightPx,
                               int mapHeightPx, int cardLineCount,
                               const std::vector<int>& filterWidthsPx = {},
                               const std::vector<int>& actionWidthsPx = {});

    [[nodiscard]] DisksRect filtersRect() const noexcept;
    [[nodiscard]] DisksRect actionsRect() const noexcept;
    [[nodiscard]] DisksRect statusRect() const noexcept;
    [[nodiscard]] DisksRect mapRect() const noexcept;
    [[nodiscard]] DisksRect listRect() const noexcept;
    [[nodiscard]] DisksRect cardRect() const noexcept;

    // Кнопки по порядку: фильтры — lettered, system, removable; действия —
    // export map, export text, open, refresh, check.
    [[nodiscard]] DisksRect filterButtonRect(int index) const noexcept;
    [[nodiscard]] DisksRect actionButtonRect(int index) const noexcept;

    // Число видимых кнопок в каждой группе: при тесном окне хвост списка
    // отбрасывается, и экран обязан знать, какие кнопки вообще создавать.
    [[nodiscard]] int filterButtonCount() const noexcept;
    [[nodiscard]] int actionButtonCount() const noexcept;

    // Лента прокрутки дерева: список в раскладке отделён от карточки, а при
    // тесном окне карточка уезжает первой.
    [[nodiscard]] bool cardVisible() const noexcept;
    [[nodiscard]] bool statusVisible() const noexcept;

    // Окно меньше минимума: показываем то, что помещается.
    [[nodiscard]] bool cramped() const noexcept;
    [[nodiscard]] int clientWidthPx() const noexcept;
    [[nodiscard]] int clientHeightPx() const noexcept;

    [[nodiscard]] HitTarget hitTest(int px, int py) const noexcept;

    // Пропорциональное ужимание набора ширин под доступную полосу. Чистая
    // функция: ею пользуется и раскладка, и тест.
    [[nodiscard]] static std::vector<int> fitWidths(int availablePx, const std::vector<int>& desiredPx, int gapPx,
                                                    int minWidthPx);

private:
    int width_{0};
    int height_{0};
    int contentLeft_{0};
    int contentWidth_{0};
    int gap_{0};
    int buttonGap_{0};
    int filtersTop_{0};
    int actionsTop_{0};
    int statusTop_{0};
    int mapTop_{0};
    int listTop_{0};
    int cardTop_{0};
    int listHeight_{0};
    int buttonHeight_{0};
    int statusHeight_{0};
    int mapHeight_{0};
    int cardHeight_{0};
    int filterCount_{0};
    int actionCount_{0};
    std::vector<int> filterWidths_;
    std::vector<int> actionWidths_;
    bool cardVisible_{true};
    bool statusVisible_{true};
    bool cramped_{false};
};

// ---------------------------------------------------------------------------
// Модель экрана
// ---------------------------------------------------------------------------

// Формат экспорта карты (FR-2: «Экспорт карты в JSON и в текст»). JSON — для
// отчёта и разбора программно, текст — для баг-репорта, который читают глазами
// без приложения.
enum class ExportFormat : std::uint8_t { Json, Text };

const char* toString(ExportFormat format) noexcept;

// Оба представления карты, посчитанные один раз. Формируется по полному
// инвентарю, а не по отфильтрованному: баг-репорт должен содержать всю
// картину, иначе «а у меня диск D не показывался» останется без ответа.
struct MapExport {
    std::string json;
    std::string text;

    [[nodiscard]] bool empty() const noexcept {
        return json.empty() && text.empty();
    }
};

// Всё состояние экрана «Диски» без единого Win32-типа. Не потокобезопасна и не
// имеет блокировок намеренно: объект принадлежит UI-потоку (§6.1), а гонка с
// фоновым потоком закрыта на мосту — фон публикует неизменяемый снимок, а в
// UI-поток он попадает через PostMessage (§6.4).
class DisksViewModel {
public:
    DisksViewModel();

    // --- Снимок инвентаризации (SPEC §6.4) ----------------------------------
    //
    // Приём снимка пересобирает дерево и карту, но НЕ сбрасывает выделение и
    // раскрытие: они заданы ключами, а ключи переживают перечитку. Сбрасывать
    // их на каждом WM_DEVICECHANGE — значит терять место человека в дереве
    // каждый раз, когда воткнули флешку.
    void publishInventory(std::shared_ptr<const core::DiskInventory> inventory);
    // Список дисков напрямую: мост может отдать и снимок, и «сырой» список, а
    // приведение к детерминированному порядку (core::sortInventory) всё равно
    // обязано произойти — иначе карта и дерево прыгали бы между прогонами.
    void publishDisks(std::vector<core::PhysicalDisk> disks);
    void clear();

    [[nodiscard]] bool hasInventory() const noexcept;
    [[nodiscard]] const core::DiskInventory* inventory() const noexcept;
    [[nodiscard]] std::size_t diskCount() const noexcept;
    [[nodiscard]] std::size_t visibleDiskCount() const noexcept;
    [[nodiscard]] std::size_t unavailableDiskCount() const noexcept;

    // --- Фильтры и сортировка (FR-2) ----------------------------------------
    void setFilter(std::string_view which, bool enabled);
    void setFilters(DiskFilters filters);
    [[nodiscard]] DiskFilters filters() const noexcept;
    void setSort(SortKey key, SortDirection direction);
    void toggleSort(SortKey key);
    [[nodiscard]] SortOrder sort() const noexcept;
    // Активен ли фильтр (для галочки на кнопке). Ключ — те же три строки, что
    // и в setFilter: «lettered», «system», «removable».
    [[nodiscard]] bool filterEnabled(std::string_view which) const noexcept;

    // --- Дерево --------------------------------------------------------------
    // Строки в порядке отрисовки: фильтрованные, отсортированные и с учётом
    // раскрытия. Именно их получает SysListView32.
    [[nodiscard]] const std::vector<TreeRow>& rows() const noexcept;
    [[nodiscard]] std::size_t rowCount() const noexcept;
    [[nodiscard]] const TreeRow* rowFor(std::string_view key) const;
    [[nodiscard]] std::optional<std::size_t> indexFor(std::string_view key) const;
    [[nodiscard]] std::string keyAt(std::size_t index) const;

    void setSelectedKey(std::string_view key);
    [[nodiscard]] std::string selectedKey() const;
    void selectFirst();
    void clearSelection();
    // Родитель узла («d:0p:3v:0» → «d:0p:3»). Нужен клавиатуре: без него
    // выйти из вложенного узла нечем.
    [[nodiscard]] std::string parentKey(std::string_view key) const;
    [[nodiscard]] bool selectParent(std::string_view key);
    // true — состояние изменилось, экран обязан перерисоваться.
    bool moveSelection(int delta);
    bool toggleExpanded(std::string_view key);
    void expandAll();
    void collapseAll();

    // Клавиши экрана (§5 «Клавиатурная навигация, фокус»). Стрелки вверх-вниз
    // обрабатывает сам SysListView32 — модель их не трогает, иначе один
    // нажатие двигало бы выделение дважды. Возвращает true, когда ключ
    // обработан (вызывающему не нужно звать DefWindowProc).
    bool handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown);

    // --- Тексты экрана -------------------------------------------------------
    // Всё, что показывается текстом, живёт здесь, а не в окне: строки зависят от
    // языка интерфейса (locale::revision) и от счётчиков, которые меняются при
    // каждом щелчке.
    [[nodiscard]] std::string statusText() const;
    [[nodiscard]] std::string cardTitle() const;
    // Строки карточки деталей: «Метка: значение», по одной на строку. Порядок —
    // как в FR-2: сначала главное (что это), потом цифры, потом свойства
    // устройства.
    [[nodiscard]] std::vector<std::string> cardLines() const;
    // Куда открывать в проводнике для текущего выделения. Пусто — открывать
    // нечего, и экран выключает кнопку, а не жмёт молча.
    [[nodiscard]] std::string explorerPath() const;
    [[nodiscard]] bool explorerAvailable() const noexcept;

    // Экспорт карты (FR-2). Считается по полному инвентарю.
    [[nodiscard]] MapExport exportBundle() const;
    // Файл для экспорта не выбирается и не пишется в модели: это дело вызывающего
    // (§6.1 — UI-поток не занимается I/O).

    // Замечания инвентаризации (core::validateInventory) и «диск не ответил»
    // (FR-1). Показываются в строке состояния и в карточке: молча рисовать
    // противоречивые цифры нельзя, а ронять приложение из-за них — тоже.
    [[nodiscard]] std::vector<std::string> problems() const;
    [[nodiscard]] bool hasProblems() const noexcept;

    // --- Карта (FR-2) -------------------------------------------------------
    [[nodiscard]] const std::vector<MapDiskModel>& mapDisks() const noexcept;
    // Высота карты под текущий набор дисков: раскладка окна отдаёт карте
    // ровно столько места, сколько ей нужно, а дереву — остаток.
    [[nodiscard]] int mapHeightDip(const MapMetrics& mapMetrics) const noexcept;

    // --- Состояние страницы (PageState) --------------------------------------
    // splitterDip здесь — высота карты в DIP. Это единственное «перетаскиваемое»
    // место экрана, и оно обязано переживать уход на другую страницу: человек
    // поднял карту, чтобы разглядеть разметку, и вернулся — карта должна быть
    // такой же (SPEC §5 доступность, §7.2).
    void applyPageState(const PageState& state);
    [[nodiscard]] PageState pageState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Окно экрана
// ---------------------------------------------------------------------------

// Идентификаторы команд WM_COMMAND. Диапазон 2101… выделен под экран «Диски»
// и не пересекается с диапазоном экрана «Очистка» (2001…): чужой обработчик
// отличает команду экрана от своей по одному сравнению, а не по списку.
enum class ControlId : WORD {
    FilterLetters = 2101,
    FilterSystem,
    FilterRemovable,
    ExportMap,
    ExportText,
    OpenInExplorer,
    Refresh,
    Check,
    First = FilterLetters,
    Last = Check,
};

[[nodiscard]] bool isDisksControl(WORD controlId) noexcept;

// Экран «Диски» как окно: карта разделов (Direct2D, §7 ADR-003) + дерево на
// SysListView32 с NM_CUSTOMDRAW + карточка деталей + фильтры + экспорт карты.
//
// Экземпляр живёт на стеке вызывающего (обычно мост экранов, задача 75) и
// переживает окно: create/destroy вызываются явно, деструктор закрывает окно,
// если забыли. Обратные обработчики зовутся в UI-потоке, внутри обработчика
// окна, поэтому блокировать в них нельзя.
class DisksScreen {
public:
    // Открыть путь в проводнике. Экран отдаёт путь (точка монтирования тома) и
    // ничего не знает о том, чем вызывающий его откроет.
    using OpenInExplorerHandler = std::function<void(const std::string& path)>;
    // Экспорт карты: вызывающий решает, куда писать. Экран отдаёт готовый
    // текст в нужном формате (§6.1 — запись файла не в UI-потоке).
    using ExportHandler = std::function<void(ExportFormat format, const std::string& text)>;
    // Перечитать инвентаризацию: кэш platform::inventory и мост решают, как.
    using RefreshHandler = std::function<void()>;
    // FR-2 помечает «Проверить» как v1.1. Обработчик объявлен уже сейчас: когда
    // проверка появится, кнопка включится одним флагом, а API экрана не
    // сломается у моста, который уже написан.
    using CheckHandler = std::function<void()>;

    struct Callbacks {
        OpenInExplorerHandler onOpenInExplorer;
        ExportHandler onExport;
        RefreshHandler onRefresh;
        CheckHandler onCheck;
    };

    explicit DisksScreen(Callbacks callbacks = {});
    ~DisksScreen();

    DisksScreen(const DisksScreen&) = delete;
    DisksScreen& operator=(const DisksScreen&) = delete;
    DisksScreen(DisksScreen&&) = delete;
    DisksScreen& operator=(DisksScreen&&) = delete;

    // Создать окно-экран в parent (обычно хост содержимого app_shell). nullptr —
    // не зарегистрировался класс окна или не хватило ресурсов; причина в журнале.
    [[nodiscard]] HWND create(HWND parent, int dpi);
    [[nodiscard]] HWND window() const noexcept;
    void destroy() noexcept;

    // Смена DPI (WM_DPICHANGED): пересчитать раскладку, пересоздать отступы
    // списка и пересоздать D2D-ресурсы — они привязаны к DPI окна.
    void setDpi(int dpi);

    [[nodiscard]] DisksViewModel& model() noexcept;
    [[nodiscard]] const DisksViewModel& model() const noexcept;

    // Перенести модель в контролы: после любого изменения модели и после
    // публикации кадра из фона. Дерево само не знает, что модель изменилась, —
    // иначе модель знала бы про SysListView32.
    void refresh();

    // Обновление темы: перечитать палитру, пересоздать кисти Direct2D и
    // применить к контролам (§5 «Тема»).
    void reloadTheme();

    // --- Приём снимка из фонового потока (звонятся в UI-потоке) -------------
    void publishInventory(std::shared_ptr<const core::DiskInventory> inventory);
    void publishDisks(std::vector<core::PhysicalDisk> disks);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mrproper::ui::disks
