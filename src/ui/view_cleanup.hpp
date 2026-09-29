// MrProper — экран «Очистка»: дерево категорий, чекбоксы, агрегаты, прогресс и отмена.
//
// Спека: §4 FR-3 (каталог категорий мусора), FR-4 (оценка кандидата: SafetyLevel,
// ConfidenceScore, Reasons, LockedBy), FR-5 (план очистки: дерево категорий →
// элементы, чекбоксы и агрегаты «всё / только Safe / только выбранное», dry-run
// обязателен и по умолчанию, снимок состояния перед исполнением), FR-6 (отмена
// через CancellationToken, прогресс колбэком в UI-поток), §7.1 п.3 («Очистка»),
// §7.2 (UX-гайдлайн: ничего не удаляется без явного действия и списка того, что
// будет удалено; для каждого элемента объём, возраст, объяснение, иконка риска;
// большая цифра сверху с указанием «по аллоцированному размеру»; Ctrl+Z, пока
// транзакция не схлопнулась), §5 (доступность: клавиатурная навигация, фокус,
// локализация ru + en, DPI per-monitor v2), §6.1/§6.4 (слой UI — единственный
// поток с сообщениями; фоновые результаты приходят готовым неизменяемым
// снимком), §7 (ADR-3: дерево — нативный SysTreeView32 с NM_CUSTOMDRAW).
//
// ---------------------------------------------------------------------------
// Почему файл разделён на два слоя, а не «просто экран»
// ---------------------------------------------------------------------------
//
// Слой экрана состоит из двух частей с очень разной ценой ошибки:
//
//   1. Модель (CleanupViewModel и всё, что выше CleanupScreen в этом файле) —
//      чистый C++ без единого Win32-типа. Это «что показываем»: дерево, чекбоксы,
//      агрегаты, прогресс, отмена, обязательный dry-run, раскладка в DIP и
//      попадания мышью. Здесь принимаются все решения, которые обязаны быть
//      одинаковыми при любом DPI, языке и теме, — поэтому их можно проверить
//      без окна (тот же приём, что в nav.*: RailLayout и hitTest там тоже чистая
//      арифметика). Решения по деньгам («сколько освободим») считает не этот
//      слой, а core::plan: модель лишь собирает выбор пользователя в набор
//      кандидатов и спрашивает у ядра решение по каждому — иначе экран и CLI
//      показали бы разные цифры для одного и того же скана (SPEC §11.4).
//
//   2. Окно (CleanupScreen) — SysTreeView32 с чекбоксами, панель агрегатов,
//      полоса прогресса, кнопки и таблица точного списка операций для dry-run.
//      По ADR-3 дерево и таблицы рисуются нативными контролами с кастомной
//      отрисовкой, а не «всё на Direct2D»: иначе вручную пишутся скролл,
//      выделение, клавиатурная навигация и чтение с экрана (стоимость ×3–5 к
//      бюджету MVP).
//
// Почему окно живёт в этом же файле, а не в отдельном view_cleanup_window.*:
// владение файлами у волн узкое, и экран без модели или модель без окна —
// половина задачи. Разделение выражано типами (модель не знает про HWND), а не
// количеством файлов.
//
// ---------------------------------------------------------------------------
// Границы слоёв
// ---------------------------------------------------------------------------
//
// Зависимость ровно одна и только вниз: core (модель кандидата, план, скоринг),
// ui::locale (строки и числа в языке интерфейса) и ui::theme (палитра и цвета
// риска). Ни engine, ни scanner в этом файле быть не должно: слой ui собирается
// в mrproper_ui, которая линкуется с core и platform, а модель скана приходит
// мостом (mv_bridge) как готовый снимок — §6.4 прямо запрещает мутировать
// результаты после публикации. Исключение (CancellationToken, RemoveDirectory,
// Restart Manager) в экран не проникает: отмена — это вызов обратного обработчика
// onCancel, а решение «остановились» приходит обратно вызовом publishFinished.
//
// ---------------------------------------------------------------------------
// Правила этого файла
// ---------------------------------------------------------------------------
//
//   * Поток — только UI (§6.1). Никаких блокировок и никакого I/O: publish*
//     зовутся из UI-потока после PostMessage от фонового потока, а внутри
//     publish* — только счётчики и перерисовка.
//   * Исключение не пересекает границу Win32: статическая процедура окна ловит
//     всё сама (§5 «устойчивость»), поэтому хуки модели объявлены без noexcept,
//     а чистые функции вроде buildLayout помечены noexcept.
//   * Каждый неуспех WinAPI пишется в журнал с кодом ошибки (§5, §12).
//   * Тексты — только через ui::locale (StringId, а не литерал): опечатка в
//     литерале даёт пустую строку в интерфейсе, опечатка в StringId — ошибку
//     компиляции.
#pragma once

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой ui, по SPEC §7 это законное место

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "core/plan.hpp"
#include "locale.hpp"
#include "nav.hpp"

namespace mrproper::ui::cleanup {

// ---------------------------------------------------------------------------
// Каталог категорий (SPEC §4 FR-3)
// ---------------------------------------------------------------------------
//
// Порядок таблицы задан спецификацией: «каталог категорий» — это список, а не
// набор, и дерево обязано показывать его в том порядке, в котором человек
// читал FR-3. Порядок по убыванию объёма (как делает core::CategoryAggregate)
// удобнее для быстрого выбора, но «Большие файлы пользователя» оказывались бы
// первыми и сдвигали бы «Корзину» вниз — список переставал быть списком.
// Сортировка по объёму остаётся доступной: filter + узлы дерева сортируются
// по selectedBytes, а таблица операций dry-run уже отсортирована планом.
inline constexpr std::array<std::string_view, 22> kCategoryOrder{{
    "temp.user",         "temp.system",     "recycle.bin",   "browser.cache",
    "browser.history",   "firefox.cache",   "ms.update",     "delivery.opt",
    "winsxs.report",     "prefetch",        "crash.dumps",   "wer",
    "logs",              "icon.font.cache", "shadercache",   "installer.cache",
    "memory.dumps",      "thumbnails",      "npm.pip.cache", "user.bigfiles",
    "dev.caches",        "wsl.vhdx.report",
}};

// Место категории в FR-3. Неизвестный идентификатор (правило из нового набора
// правил, §9.2) уходит в конец списка, но не теряется: дерево показывает сам
// идентификатор, а не пустую строку.
[[nodiscard]] std::size_t categoryOrder(std::string_view categoryId) noexcept;

// Название категории для узла дерева. Ключ каталога строк («cleanup.category.*»)
// первее идентификатора, идентификатор — последний запасной вариант.
[[nodiscard]] std::string categoryTitle(std::string_view categoryId);

// ---------------------------------------------------------------------------
// Состояния узла и экрана
// ---------------------------------------------------------------------------

// Три состояния чекбокса (SPEC §5 доступность: состояние показывается знаком,
// а не только цветом; здесь их три, и «частично» — не украшение, а третье
// состояние, которое обязано существовать у родителя с выбранными детьми).
enum class CheckState : std::uint8_t { Unchecked, Partial, Checked };

// Состояние экрана. Порядок значим: он же порядок «действий» в switch.
enum class ScreenState : std::uint8_t {
    Idle,       // скана не было: дерево пустое, кнопка очистки выключена
    Scanning,   // идёт скан; прогресс неопределённый (marquee)
    Planning,   // снимок получен, считается план
    DryRun,     // FR-5: показан точный список операций, ждём подтверждения
    Ready,      // план готов, очистка разрешена (dry-run для этого плана подтверждён)
    Running,    // идёт удаление
    Cancelling, // отмена запрошена, ждём остановки фонового пула
    Cancelled,  // остановились по запросу пользователя
    Done,       // завершилось (часть операций могла не удалиться — см. failed)
    Failed,     // отказ скана или исполнения
};

const char* toString(CheckState state) noexcept;
const char* toString(ScreenState state) noexcept;

// ---------------------------------------------------------------------------
// Узлы дерева
// ---------------------------------------------------------------------------

// Элемент дерева — один кандидат из FR-4 плюс решение по нему из плана.
struct ItemNode {
    std::size_t candidateIndex{};  // индекс в снимке кандидатов: по нему plan
    std::string key;               // "item:<индекс>" — ключ состояния страницы
    std::string label;             // displayName кандидата (FR-4)
    std::string path;              // путь: объяснение «почему это мусор» начинается с него
    std::string category;          // идентификатор категории FR-3
    std::string reason;            // Reasons, склеенные в строку (FR-4)
    std::string lockedBy;          // имена процессов, удерживающих файлы (FR-4)
    core::SafetyLevel safety{core::SafetyLevel::Review};
    core::PlanAction action{core::PlanAction::Keep};
    core::SkipReason skip{core::SkipReason::None};
    std::uint64_t bytes{};  // reclaimBytes: 0 у Keep и SkipLocked (§6.3)
    std::uint64_t logicalBytes{};
    std::uint64_t allocatedBytes{};
    std::uint32_t fileCount{};
    int confidence{};
    std::int64_t oldestWrite{};  // unix-секунды
    bool checked{};
    bool selectable{};  // занятые процессом (SkipLocked) выбрать нельзя
    bool riskyHidden{}; // Risky без «показать все» (FR-4, §12)
};

// Категория дерева. Агрегаты пересчитываются при каждом изменении выбора:
// «сколько освободим, если выбрать всё / только Safe / только выбранное» (FR-5)
// — это три цифры, которые человек видит до того, как нажмёт кнопку.
struct CategoryNode {
    std::string category;
    std::string key;  // "cat:<категория>"
    std::string title;
    core::SafetyLevel safety{core::SafetyLevel::Review};  // наивысший среди видимых детей
    CheckState check{CheckState::Unchecked};
    std::size_t itemCount{};       // видимых элементов
    std::size_t selectedCount{};
    std::size_t selectableCount{};  // из них выбираемых (без SkipLocked)
    std::uint64_t allBytes{};       // «если выбрать всё» в этой категории
    std::uint64_t selectedBytes{};  // «только выбранное»
    std::uint64_t safeOnlyBytes{};  // «только Safe»
    std::vector<std::size_t> items;  // индексы в CleanupViewModel::items()
};

// ---------------------------------------------------------------------------
// Агрегаты и прогресс
// ---------------------------------------------------------------------------

// Три цифры FR-5 плюс то, что нужно подсказке под ними.
struct CleanupAggregates {
    std::uint64_t allBytes{};       // «если выбрать всё» (кроме заблокированных)
    std::uint64_t safeOnlyBytes{};  // «если выбрать только Safe»
    std::uint64_t selectedBytes{};  // «только выбранное» — то, что реально удалится
    std::size_t allCount{};
    std::size_t safeOnlyCount{};
    std::size_t selectedCount{};
    std::size_t candidateCount{};   // всего кандидатов в снимке
    std::size_t lockedCount{};      // SkipLocked: заняты приложениями
    std::size_t hiddenRiskyCount{}; // скрыто Risky (FR-4)
    std::size_t belowThresholdCount{};
    std::size_t tooSmallCount{};
    std::size_t profileFilteredCount{};

    // Доля выбранного от «всего» — заполнение панели плана. Ноль, когда выбрать
    // нечего: полоса при нуле должна быть пустой, а не полной.
    [[nodiscard]] double selectedFraction() const noexcept;
};

// Прогресс скана: скан идёт в фоне, дерево ещё пустое (SPEC §6.4 — UI читает
// счётчики раз в 100 мс, но и без таймера публикация кадра из фона честнее).
// Счётчики обязаны доехать до экрана: скан на C: SSD идёт до минуты (§5
// «Производительность»), и полоса-marquee без чисел читается как зависшая.
struct ScanProgress {
    std::uint64_t filesSeen{};
    std::uint64_t bytesSeen{};
    std::string currentLabel;  // путь или имя правила — «чем занят скан»
    bool indeterminate{true};  // скан не знает заранее, сколько найдёт
};

// Прогресс очистки: счётчики исполнения и состояние отмены.
struct CleanupProgress {
    ScreenState state{ScreenState::Idle};
    std::size_t totalOperations{};
    std::size_t completedOperations{};
    std::size_t failedOperations{};
    std::uint64_t totalBytes{};
    std::uint64_t freedBytes{};
    std::string currentLabel;

    // 0…1 для полосы прогресса. indeterminate (marquee) даёт 0: полоса без
    // движения читается как «зависло», поэтому для скана она marquee.
    [[nodiscard]] double fraction() const noexcept;
    [[nodiscard]] bool indeterminate() const noexcept;
    [[nodiscard]] bool busy() const noexcept;      // идёт скан или очистка
    [[nodiscard]] bool cancelAvailable() const noexcept;
    [[nodiscard]] bool canStart() const noexcept;   // есть что удалять и не идёт работа
};

// ---------------------------------------------------------------------------
// Раскладка и попадания (чистая арифметика, DIP)
// ---------------------------------------------------------------------------

// Метрики экрана в DIP (1/96 дюйма). Per-monitor v2 (§5) меняет pixelsPerDip,
// но не размеры элементов, поэтому вёрстка не знает про DPI.
struct CleanupMetrics {
    double summaryHeightDip{84.0};    // три цифры + подпись «по аллоцированному размеру»
    double progressHeightDip{20.0};   // полоса прогресса и её подпись
    double detailsHeightDip{48.0};    // объяснение и «занято процессами» (§7.2)
    double toolbarHeightDip{48.0};    // две строки кнопок по 24 DIP
    double dryRunHeightDip{220.0};    // таблица точного списка операций
    double paddingDip{8.0};
    double gapDip{6.0};
    double minTreeHeightDip{120.0};
    double minWidthDip{520.0};
    double minHeightDip{300.0};
};

// Прямоугольник в пикселях клиентской области. Правая и нижняя границы не
// включаются: соседние прямоугольники делят область без зазора и без двойного
// попадания (то же соглашение, что в RailRect).
struct CleanupRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(int px, int py) const noexcept;
};

// Что под указателем мыши. Отдельное перечисление, а не сравнение прямоугольников
// в WndProc: так путь «пиксели → действие» проверяется без окна.
enum class HitTarget : std::uint8_t {
    None,
    Summary,
    Tree,
    Details,
    DryRun,
    Progress,
    Clean,        // «Очистить сейчас» / в DryRun — «Подтверждаю»
    Cancel,
    Rescan,
    ShowAll,      // «Показать все» (Risky, FR-4)
    SelectAll,    // «Выбрать всё»
    ClearSelection,
    Undo,         // Ctrl+Z, §7.2
    DryRunClose,
};

// Раскладка экрана. Чистая функция от метрик, DPI и размеров окна: значение
// ничего не владеет и копируется по значению, поэтому его можно хранить в
// обработчике WM_PAINT и сравнивать в тесте.
class CleanupLayout {
public:
    CleanupLayout() = default;

    // catalogHeightPx — сколько пикселей дерево реально просит (высота дерева по
    // числу строк). Если окно ниже, дерево получает минимум, а не ноль: пустое
    // дерево при открытом окне читается как поломка.
    static CleanupLayout compute(const CleanupMetrics& metrics, int dpi, int clientWidthPx,
                                 int clientHeightPx, bool dryRunVisible, int catalogHeightPx = 0);

    [[nodiscard]] CleanupRect summaryRect() const noexcept;
    [[nodiscard]] CleanupRect toolbarRect() const noexcept;
    [[nodiscard]] CleanupRect treeRect() const noexcept;
    [[nodiscard]] CleanupRect detailsRect() const noexcept;
    [[nodiscard]] CleanupRect dryRunRect() const noexcept;
    [[nodiscard]] CleanupRect progressRect() const noexcept;
    [[nodiscard]] CleanupRect cleanButtonRect() const noexcept;
    [[nodiscard]] CleanupRect cancelButtonRect() const noexcept;
    [[nodiscard]] CleanupRect rescanButtonRect() const noexcept;
    [[nodiscard]] CleanupRect showAllButtonRect() const noexcept;
    [[nodiscard]] CleanupRect selectAllButtonRect() const noexcept;
    [[nodiscard]] CleanupRect clearSelectionButtonRect() const noexcept;
    [[nodiscard]] CleanupRect undoButtonRect() const noexcept;
    [[nodiscard]] CleanupRect dryRunCloseRect() const noexcept;

    // Окно меньше минимума: показываем то, что помещается, и не рисуем остальное.
    // Возвращать «ничего не видно» нельзя — тогда экран молча пустеет при
    // перетаскивании окна мышью.
    [[nodiscard]] bool cramped() const noexcept;
    [[nodiscard]] int clientWidthPx() const noexcept;
    [[nodiscard]] int clientHeightPx() const noexcept;
    [[nodiscard]] bool dryRunVisible() const noexcept;

    [[nodiscard]] HitTarget hitTest(int px, int py) const noexcept;

private:
    // Разобранная по полосам высота: полосы идут сверху вниз в порядке
    // summaryRect → progressRect → treeRect → detailsRect → dryRunRect →
    // toolbarRect, а снизу прижаты кнопки, поэтому дерево — единственная
    // гибкая часть и единственная, кто двигает границы остальных.
    int width_{0};
    int height_{0};
    int summary_{0};
    int progress_{0};
    int toolbar_{0};
    int details_{0};
    int dryRun_{0};
    int treeBottom_{0};
    int detailsTop_{0};
    int dryRunTop_{0};
    bool dryRunVisible_{false};
    bool cramped_{false};

    // Зазор между кнопками в строке и кнопка по индексу. Обе функции чистые и
    // принимают прямоугольник строки, а не окно: раскладка обязана считаться без
    // HWND (как RailLayout в nav).
    [[nodiscard]] static int gapPx(int rowHeight) noexcept;
    [[nodiscard]] static CleanupRect buttonInRow(const CleanupRect& row, int index, int count, int gap,
                                                 int rowTop) noexcept;
};

// ---------------------------------------------------------------------------
// Модель экрана
// ---------------------------------------------------------------------------

// Всё состояние экрана «Очистка» без единого Win32-типа. Не потокобезопасна и
// не имеет блокировок намеренно: объект принадлежит UI-потоку (§6.1), а гонка с
// фоновым потоком закрыта на мосту — фон публикует неизменяемый снимок, а в
// UI-поток он попадает через PostMessage (§6.4).
class CleanupViewModel {
public:
    CleanupViewModel();

    // --- Снимок скана (SPEC §6.4: shared_ptr<const ScanResult>) --------------
    //
    // Приём нового снимка сбрасывает выбор в рекомендованный профиль: старый
    // выбор относился к старому составу кандидатов, и переносить его молча
    // значило бы удалять то, о чём человек не informed. Профиль, «показать
    // все» и строка фильтра — настройки пользователя, они переживают скан.
    void publishCandidates(std::shared_ptr<const std::vector<core::CleanupCandidate>> candidates);
    void clear();
    [[nodiscard]] bool hasCandidates() const noexcept;

    // --- Профиль и рискованные (FR-4, §9) -----------------------------------
    void setProfile(core::SelectionProfile profile);
    [[nodiscard]] core::SelectionProfile profile() const noexcept;

    // «Показать все» — первое подтверждение для Risky. Второе — allowRisky:
    // без него Risky остаётся видимым, но невыбираемым (§9 «двойное
    // подтверждение для Risky», §12).
    void setShowAllRisky(bool show);
    [[nodiscard]] bool showAllRisky() const noexcept;
    void setAllowRisky(bool allow);
    [[nodiscard]] bool allowRisky() const noexcept;

    // Прямое удаление вместо корзины приложения (ADR-006, FR-7): включается
    // кнопкой настройки, потому что решение «куда деть 4 ГБ кэша» принимает
    // человек, а не алгоритм.
    void setUseTrash(bool useTrash);
    [[nodiscard]] bool useTrash() const noexcept;

    // Порог уверенности (FR-4 ConfidenceScore, core::kPlanDefaultConfidenceThreshold).
    void setConfidenceThreshold(int threshold);
    [[nodiscard]] int confidenceThreshold() const noexcept;

    // Нижний порог объёма: «не брать кандидатов меньше N байт».
    void setMinReclaimBytes(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t minReclaimBytes() const noexcept;

    // --- Фильтр и раскрытие --------------------------------------------------
    void setFilter(std::string_view filter);
    [[nodiscard]] std::string filter() const;

    void setExpanded(std::string_view key, bool expanded);
    [[nodiscard]] bool isExpanded(std::string_view key) const noexcept;
    void expandAll();
    void collapseAll();

    // --- Выбор (чекбоксы) ----------------------------------------------------
    //
    // Ключ — «cat:<категория>» или «item:<индекс>», а не индекс массива: индекс
    // меняется при новом скане, а состояние страницы (PageState) обязано
    // пережить и скан, и перезапуск приложения.
    [[nodiscard]] std::optional<CheckState> nodeCheckState(std::string_view key) const;
    [[nodiscard]] bool isNodeChecked(std::string_view key) const;

    // false — узел не выбран и состояние не изменилось. Причины отказа:
    // неизвестный ключ, узел не выбираем (занят приложением), либо Risky без
    // второго подтверждения (allowRisky). Молчаливый отказ хуже явного: человек
    // ткнул в чекбокс и должен понять почему.
    bool setNodeChecked(std::string_view key, bool checked);
    bool toggleNode(std::string_view key);
    bool toggleFocused();

    void selectAll();         // «Выбрать всё»
    void clearSelection();    // «Снять выбор»
    void selectSafeOnly();    // агрегат «только Safe» становится выбором
    [[nodiscard]] std::size_t selectedCount() const noexcept;

    // --- Фокус и клавиатура (SPEC §5 «Клавиатурная навигация, фокус») ---------
    //
    // Фокус идёт только по видимым узлам в порядке дерева: категория, её
    // раскрытые элементы, следующая категория. Курсор в дереве живёт
    // параллельно (выделение), и это разные вещи — на них расходятся фокус и
    // содержимое, экран обязан знать, что потерял фокус.
    void setFocusKey(std::string_view key);
    [[nodiscard]] std::string focusKey() const;
    bool moveFocus(int delta);
    bool focusNextVisible();
    bool focusPreviousVisible();

    // true — ключ обработан моделью (вызывающему не нужно звать DefWindowProc).
    bool handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown);

    // --- Дерево (данные для SysTreeView32 и для теста) ----------------------
    [[nodiscard]] const std::vector<CategoryNode>& categories() const noexcept;
    [[nodiscard]] const std::vector<ItemNode>& items() const noexcept;
    [[nodiscard]] const ItemNode* findItem(std::string_view key) const;
    [[nodiscard]] const CategoryNode* findCategory(std::string_view key) const;
    [[nodiscard]] std::size_t visibleCategoryCount() const noexcept;

    // Тексты узлов. Отдельные функции, а не поля в структурах: подпись зависит
    // от языка интерфейса и от счётчиков, которые меняются при каждом щелчке, а
    // хранить её в узле значит хранить копию строки, которая устареет первой.
    [[nodiscard]] std::string nodeText(std::string_view key) const;
    [[nodiscard]] std::string nodeDetails(std::string_view key) const;
    [[nodiscard]] std::string nodeRiskLabel(std::string_view key) const;

    // --- Агрегаты (FR-5) -----------------------------------------------------
    [[nodiscard]] const CleanupAggregates& aggregates() const noexcept;
    [[nodiscard]] std::string summaryText() const;      // «Будет освобождено: …»
    [[nodiscard]] std::string selectedText() const;     // «Выбрано: …»
    [[nodiscard]] std::string safeOnlyText() const;     // «Только безопасное (…)»
    [[nodiscard]] std::string reclaimHintText() const;  // «По аллоцированному размеру»
    [[nodiscard]] std::string lockedText() const;       // «Пропущено занятых: N»
    [[nodiscard]] std::string statusText() const;       // строка состояния под прогрессом

    // --- План (FR-5) ---------------------------------------------------------
    //
    // План целиком (профиль «как будто выбрано всё») и план выбранного — две
    // разные вещи, и разные цифры для них — это не расхождение, а требование:
    // агрегат «если выбрать всё» обязан оставаться прежним, когда человек снял
    // пять галочек. Решения о действиях в обоих случаях принимает core::plan.
    [[nodiscard]] const core::CleanupPlan& profilePlan() const noexcept;
    [[nodiscard]] core::CleanupPlan effectivePlan() const;
    [[nodiscard]] std::vector<core::CleanupCandidate> selectedCandidates() const;

    // Проверка инвариантов §6.3 на выбранном: reclaimBytes == allocatedBytes для
    // Delete/Trash и 0 для Keep/SkipLocked, ровно один элемент на кандидата.
    // Пустой список — план согласован; непустой экран показывает, что не так.
    [[nodiscard]] std::vector<std::string> problems() const;

    // --- Dry-run (FR-5 «обязателен и по умолчанию перед первым удалением») ---
    [[nodiscard]] core::DryRunReport dryRunReport() const;
    [[nodiscard]] bool dryRunAcknowledged() const;
    void acknowledgeDryRun();
    void forgetDryRunAcknowledgement();
    // Закрыть панель dry-run без подтверждения (крестик, Escape): план снова
    // будет показан при следующей попытке очистки.
    void closeDryRun();

    // Список операций для таблицы dry-run: категория, элемент, действие, объём,
    // УРОВЕНЬ РИСКА и причина (FR-4 + FR-5 + §12). Таблица показывает ровно то,
    // что будет выполнено (FR-5), и ничего кроме. Уровень риска и объяснение —
    // обязательные поля строки: подтверждение перед удалением обязано называть
    // и уровень, и то, что именно собираются снести (review-05 F-01).
    struct DryRunRow {
        std::string category;
        std::string item;
        std::string action;
        std::string bytes;
        std::string safety;  // «Безопасно» / «Требует подтверждения» / «Рискованно»
        std::string why;     // «почему это мусор» (Reasons кандидата, FR-4)
        std::string reason;  // «почему такое действие» (решение плана, FR-5)
    };
    [[nodiscard]] std::vector<DryRunRow> dryRunRows() const;

    // --- Ход работы: скан, запуск, прогресс, отмена (FR-6) ------------------
    void beginScan();
    void publishScanProgress(ScanProgress progress);
    void endScan();
    void fail(std::string reason);

    // Запуск очистки. false — запускать нельзя, и state() скажет почему:
    // нечего удалять, идёт скан, либо dry-run для этого плана ещё не
    // подтверждён (FR-5). В последнем случае состояние становится DryRun:
    // показать список операций дешевле и безопаснее, чем спросить «вы уверены?»
    // без списка.
    bool beginCleanup();
    void publishOperationResult(bool ok, std::uint64_t freedBytes, std::string_view label = {});
    // Отмена кооперативная (§6.4): запрос переводит экран в Cancelling, а
    // останавливать пул обязан вызывающий (он и знает про CancellationToken).
    // false — отменять нечего (работа не идёт или уже остановлена).
    bool requestCancel();
    void publishFinished();
    void setUndoAvailable(bool available);
    [[nodiscard]] bool undoAvailable() const noexcept;
    bool undo();  // Ctrl+Z, §7.2: работает, пока транзакция не схлопнулась

    [[nodiscard]] const CleanupProgress& progress() const noexcept;
    // Счётчики прохода скана. Отдельны от CleanupProgress не по прихоти, а
    // потому что это две разные работы: CleanupProgress считает операции
    // исполнения, ScanProgress — просмотренные файлы и байты, и в любой момент
    // у одного из этих двух режимов половина полей была бы нулевой.
    [[nodiscard]] const ScanProgress& scanProgress() const noexcept;
    [[nodiscard]] ScreenState state() const noexcept;
    [[nodiscard]] std::string errorText() const;

    // Хватает ли места под дерево: высота, которую просит дерево из
    // visibleCategoryCount + раскрытых элементов. Раскладка получает это число,
    // чтобы отдать дереву остаток окна, а не половину и не ноль.
    [[nodiscard]] int catalogHeightPx(const CleanupMetrics& metrics, int dpi) const noexcept;

    // --- Состояние страницы (PageState, задача 70) ----------------------
    //
    // Возврат на «Очистку» после «Дисков» обязан вернуть раскрытые ветки,
    // выделенный узел и прокрутку (§5, §7.2). Ключи узлов стабильны между
    // сканами, поэтому состояние переживает и перезапуск.
    void applyPageState(const PageState& state);
    [[nodiscard]] PageState pageState() const;

private:
    // Пересобрать дерево и агрегаты из снимка кандидатов и настроек.
    // keepSelection=false — новый скан: выбор сбрасывается в рекомендованный
    // профиль. true — изменились только настройки или пользовательский выбор.
    void rebuild(bool keepSelection);

    // Опции, по которым принимается решение о действии для ВЫБРАННОГО
    // кандидата: профиль и порог уверенности там не участвуют, потому что
    // выбор сделал человек.
    [[nodiscard]] core::PlanOptions effectiveOptions() const;

    // Ключи видимых узлов в порядке дерева — по ним ходит фокус.
    [[nodiscard]] std::vector<std::string> visibleOrder() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Окно экрана
// ---------------------------------------------------------------------------

// Идентификаторы команд WM_COMMAND. Диапазон выделен под экран «Очистка», чтобы
// чужой обработчик (меню, рельс) отличил команду экрана от своей по одному
// сравнению, а не по списку.
enum class ControlId : WORD {
    Clean = 2001,
    Cancel,
    Rescan,
    ShowAll,
    SelectAll,
    ClearSelection,
    Undo,
    DryRunClose,
    First = Clean,
    Last = DryRunClose,
};

[[nodiscard]] bool isCleanupControl(WORD controlId) noexcept;

// Экран «Очистка» как окно: SysTreeView32 с чекбоксами (SPEC §7, ADR-3), панель
// агрегатов, полоса прогресса, кнопки и таблица точного списка операций dry-run.
//
// Экземпляр живёт на стеке вызывающего (обычно мост экранов, задача 75) и
// переживает окно: create/destroy вызываются явно, деструктор закрывает окно,
// если забыли. Обратные обработчики зовутся в UI-потоке, внутри обработчика
// окна, поэтому блокировать в них нельзя.
class CleanupScreen {
public:
    // Очистка запущена: движок получил план и начал пул. Экран отсюда больше
    // ничего не ждёт — прогресс приходит publishOperationResult.
    using StartCleanupHandler = std::function<void()>;
    // Запрошена отмена: вызывающий обязан остановить CancellationToken (§6.4).
    // Экран не решает, чем остановится фоновая работа, — это не его дело.
    using CancelHandler = std::function<void()>;
    using RescanHandler = std::function<void()>;
    // Ctrl+Z: undo доступно, пока транзакция не схлопнулась (§7.2).
    using UndoHandler = std::function<void()>;
    using OpenInExplorerHandler = std::function<void(const std::string& path)>;

    struct Callbacks {
        StartCleanupHandler onStartCleanup;
        CancelHandler onCancel;
        RescanHandler onRescan;
        UndoHandler onUndo;
        OpenInExplorerHandler onOpenInExplorer;
    };

    explicit CleanupScreen(Callbacks callbacks = {});
    ~CleanupScreen();

    CleanupScreen(const CleanupScreen&) = delete;
    CleanupScreen& operator=(const CleanupScreen&) = delete;
    CleanupScreen(CleanupScreen&&) = delete;
    CleanupScreen& operator=(CleanupScreen&&) = delete;

    // Создать окно-экран в parent (обычно хост содержимого app_shell). nullptr —
    // не зарегистрировался класс окна или не хватило ресурсов; причина в журнале.
    [[nodiscard]] HWND create(HWND parent, int dpi);
    [[nodiscard]] HWND window() const noexcept;
    void destroy() noexcept;

    // Смена DPI (WM_DPICHANGED): пересчитать раскладку и пересоздать картинки
    // чекбоксов — они рисуются в пикселях, и 16×16 на 200 % мылятся.
    void setDpi(int dpi);

    [[nodiscard]] CleanupViewModel& model() noexcept;
    [[nodiscard]] const CleanupViewModel& model() const noexcept;

    // Перенести модель в контролы: после любого изменения модели и после
    // публикации кадра из фона. Само по себе дерево не знает, что модель
    // изменилась, — иначе модель знала бы про SysTreeView32.
    void refresh();

    // Обновление темы: перечитать палитру и применить к контролам (§5 «Тема»).
    void reloadTheme();

    // --- Приём кадров из фонового потока (звонятся в UI-потоке) -------------
    void publishScanProgress(ScanProgress progress);
    void publishCandidates(std::shared_ptr<const std::vector<core::CleanupCandidate>> candidates);
    void publishOperationResult(bool ok, std::uint64_t freedBytes);
    void publishFinished();
    void setUndoAvailable(bool available);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mrproper::ui::cleanup
