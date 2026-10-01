// MrProper — экран «Отчёт»: результат последней операции, журнал операций и
// ошибок, экспорт в HTML / JSON / текстовый дамп (SPEC §4 FR-8, §7.1 п.4
// «Отчёт — результат последней операции + журнал + экспорт», §5 «Приватность:
// отчёт содержит серийники — пользователь может исключить их перед отправкой»,
// §12 «Отчёт (HTML/JSON) содержит карту, кандидатов, операции и ошибки;
// открывается без приложения», §6.1 слой UI, §6.4 снимки, §7 ADR-003, §9.1
// предупреждения как ошибки, §5 доступность / DPI / тема / локализация).
//
// ---------------------------------------------------------------------------
// Почему экран состоит из журнала, а не из «списка отчётов»
// ---------------------------------------------------------------------------
//
// FR-8 перечисляет содержимое отчёта (карта разделов, кандидаты, операции,
// ошибки, время, версии), но §7.1 п.4 описывает экран словами «результат
// последней операции + журнал + экспорт». Оба требования закрываются одним
// списком из трёх источников, а не двумя вкладками:
//
//   * операции отчёта (core::Report::operations и ::untouched) — что сделали;
//   * ошибки отчёта (core::Report::errors) — почему освободилось меньше, чем
//     показал план (FR-6: «ошибки не фатальны… собираются в отчёт»);
//   * события журнала приложения (кольцо core::Logger, §6.2) — что происходило
//     вокруг операции: старт, отмена, отказ устройства, ротация набора правил.
//
// Одна лента нужна человеку, который разбирает «почему освободилось не то»:
// ошибка без операции, к которой она относится, и операция без ошибки, которая
// её вызвала, — это два разных вопроса, а в общей ленте они стоят рядом и
// сортируются по времени. Раздельные списки заставили бы человека переключать
// фильтр на каждом шаге.
//
// ---------------------------------------------------------------------------
// Почему файл разделён на три слоя, а не «просто окно»
// ---------------------------------------------------------------------------
//
//   1. Модель (ReportViewModel) — чистый C++ без единого Win32-типа. Это «что
//      показываем»: строки журнала, фильтры, сортировка, выделение, строки
//      карточки и сводки, а также три представления отчёта на экспорт. Здесь
//      принимаются решения, обязательные при любом DPI, языке и теме, поэтому их
//      можно проверить без окна — тот же приём, что в nav.*, cleanup.*,
//      disks.*.
//   2. Хранилище (saveReport, listReportsUtf8, pruneReports) — запись файлов в
//      %LOCALAPPDATA%\MrProper\reports\ с ротацией «последние 20» (FR-8).
//      Функции платформенные по природе, но слой platform своего модуля
//      отчётов не имеет, а требование FR-8 «отчёт пишется в … с ограничением
//      количества файлов» закрывать всё равно надо. Поэтому это свободные
//      функции с чистой сигнатурой, а не методы окна: мост экранов (задача 75) может
//      вызвать их из рабочего потока, и тогда запись файла вообще не попадёт в
//      UI-поток (§6.1). Вызывающий, у которого есть свой обработчик экспорта,
//      вообще их не зовёт — экран отдаёт готовый текст и не настаивает.
//   3. Окно (ReportScreen) — SysListView32 с NM_CUSTOMDRAW на журнал, карточка
//      выделенной строки, строка состояния, кнопки экспорта и две галочки
//      (приватность и «только проблемы»). По ADR-003 таблица рисуется
//      нативным контролом, поэтому список здесь — SysListView32, а не
//      Direct2D-поверхность.
//
// ---------------------------------------------------------------------------
// Границы слоёв
// ---------------------------------------------------------------------------
//
// Зависимости ровно одна и только вниз: core::report_json, core::report_html,
// core::log (кольцо для UI), core::units, core::disk_model, ui::locale,
// ui::theme, ui::nav. Ни engine, ни scanner в этом файле быть не должно: отчёт
// приходит мостом (задача 75) готовым неизменяемым снимком — §6.4 прямо запрещает
// мутировать результаты после публикации, а исполнение операций в UI не живёт
// никогда. Шелл-команды («cmd /c», ShellExecute) не применяются: §5 запрещает
// их для привилегированных операций, а открыть каталог отчётов должен
// вызывающий через свой обработчик (как «Открыть в проводнике» на экране
// «Диски»).
//
// Что экран сознательно НЕ делает:
//   * не запускает сканирование и очистку и не знает про них — у него только
//     результат;
//   * не придумывает время: метки берутся из самого отчёта (детерминизм,
//     §11.4), а в имени файла — локальное время машины в момент сохранения;
//   * не маскирует серийники «по умолчанию тайно»: галочка «Маскировать
//     серийники» включена (FR-8, §5 «Приватность»), и снятие её — осознанное
//     действие человека, а не следствие сбоя;
//   * не удаляет чужие файлы из каталога отчётов: ротация трогает только
//     имена нашего формата (префикс kReportFilePrefix).
//
// ---------------------------------------------------------------------------
// Правила этого файла
// ---------------------------------------------------------------------------
//
//   * Поток — только UI (§6.1). Никаких блокировок в модели; гонка с фоновым
//     потоком закрыта на мосту и на кольце лога (Logger::snapshot отдаёт
//     копию под своим mutex).
//   * Исключение не пересекает границу Win32: статические процедуры окон ловят
//     всё сами (§5 «Устойчивость»), поэтому хуки модели объявлены без noexcept,
//     а чистые функции помечены noexcept.
//   * Каждый неуспех WinAPI пишется в журнал с кодом ошибки (§5, §12):
//     «отчёт не сохранился» без причины не чинится.
//   * Тексты — только через ui::locale (StringId) либо через локальный словарь
//     экрана (см. Word в .cpp: в каталоге нет слов журнала и колонок, список
//     ключей — владельцу локализации).
//   * Единицы объёма и числа — core::units/locale, названия дисков и разделов —
//     core::disk_model: экран, CLI и отчёт обязаны говорить одно и то же
//     (SPEC §11.4).
#pragma once

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой ui, по SPEC §7 это законное место

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.hpp"
#include "core/report_html.hpp"
#include "core/report_json.hpp"
#include "nav.hpp"

namespace mrproper::ui::report {

// ---------------------------------------------------------------------------
// Экспорт (FR-8: «Экспорт: HTML (человекочитаемый), JSON (машиночитаемый),
// текстовый дамп (для баг-репортов)»)
// ---------------------------------------------------------------------------

enum class ExportFormat : std::uint8_t { Html, Json, Text };

const char* toString(ExportFormat format) noexcept;

// Расширение файла с точкой: ".html", ".json", ".txt". Именно с точкой, потому
// что вызывающий склеивает имя файла сам (см. ReportViewModel::suggestedName).
std::string_view extensionFor(ExportFormat format) noexcept;

// Сколько отчётов каталог хранит (FR-8: «с ограничением количества файлов
// (последние 20)»). Не «20 файлов вообще», а 20 последних по времени имени:
// имя файла начинается с метки времени, поэтому порядок имени совпадает с
// порядком создания, и ротация не зависит от того, откуда взят mtime.
inline constexpr std::size_t kMaxKeptReports = 20;

// Префикс имени файла отчёта. Ротация трогает только наши имена: каталог
// %LOCALAPPDATA%\MrProper\reports\ принадлежит приложению, но человек мог
// положить туда свой файл, и удалять чужое молча нельзя.
inline constexpr wchar_t kReportFilePrefix[] = L"MrProper-";

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------

// Что за строка журнала. Три значения, а не флаг: у строки один смысл, и
// «и операция, и ошибка» в одной строке означало бы, что фильтр по виду строки
// нельзя объяснить пользователю.
enum class RowKind : std::uint8_t { Operation, Error, Event };

const char* toString(RowKind kind) noexcept;

// Строка журнала — плоская структура без варианта. Три источника (операция,
// ошибка, событие журнала) делят большую часть полей, а вариант здесь дал бы
// читателю «какой набор полей заполнен» вместо одного взгляда на структуру.
// Незаполненные поля пусты: карточка деталей печатает только то, что есть
// (пустая строка «категория:» в отчёте — это шум).
struct JournalRow {
    RowKind kind{RowKind::Event};
    core::LogLevel level{core::LogLevel::Info};
    std::int64_t atUnix{};  // 0 — время не передали, печатаем прочерк

    std::string key;  // ключ данных, а не индекс: выделение переживает новый отчёт

    // Готовые подписи колонок. Считаются при сборке строк, а не в окне: список
    // перерисовывается целиком, и форматирование времени в обработчике
    // отрисовки означало бы локализацию на каждый кадр.
    std::string timeText;
    std::string kindText;
    std::string sourceText;   // категория операции / scope ошибки / событие журнала
    std::string subjectText;  // имя элемента, путь или сообщение
    std::string statusText;   // «Удалено», «Пропущено», «Ошибка», уровень журнала
    std::string bytesText;    // объём: аллоцированный (FR-4) либо прочерк

    // Данные для карточки деталей. Копии, а не указатели в отчёт: журнал
    // событий приезжает позже отчёта (ошибка может прийти после публикации
    // снимка), и висячий указатель в UI — ровно тот дефект, который §5 запрещает.
    std::string category;
    std::string displayName;
    std::string path;
    std::string detail;
    std::string code;
    std::string message;
    std::string scope;
    std::string event;
    std::string transactionId;
    std::string safetyText;
    std::string actionText;
    std::uint64_t bytes{};
    std::uint32_t attempts{};
    std::uint32_t occurrences{1};
    int confidence{};
    std::int64_t startedAtUnix{};
    std::int64_t finishedAtUnix{};
    core::PlanAction action{core::PlanAction::Keep};
    core::SafetyLevel safety{core::SafetyLevel::Review};
    std::vector<core::LogField> fields;  // именованные поля события журнала
};

// Фильтры журнала. Не один переключатель «режима»: FR-6 требует, чтобы
// нефатальные ошибки не мешали основной работе, поэтому «показывать операции»
// и «показывать ошибки» — независимые флаги, а «только проблемы» — отдельный
// быстрый фильтр (строки уровня Warn/Error плюс провалившиеся операции).
struct ReportFilters {
    bool operations{true};
    bool errors{true};
    bool events{true};
    bool problemsOnly{false};
    core::LogLevel minLevel{core::LogLevel::Info};
    std::string search;  // подстрока по видимым полям, регистр не важен; пусто — все

    [[nodiscard]] bool any() const noexcept {
        return !operations || !errors || !events || problemsOnly || minLevel != core::LogLevel::Info ||
               !search.empty();
    }

    friend bool operator==(const ReportFilters&, const ReportFilters&) = default;
};

// Сортировка журнала. Три ключа — три вопроса человека: «что было последним»
// (время), «где сбой» (вид строки) и «что съело место» (объём).
enum class SortKey : std::uint8_t { Time, Kind, Size };
enum class SortDirection : std::uint8_t { Ascending, Descending };

struct SortOrder {
    SortKey key{SortKey::Time};
    SortDirection direction{SortDirection::Descending};

    // Журнал читают с конца (что произошло последним), список ошибок — с
    // начала. Сортировка по времени поэтому по умолчанию убывающая: иначе
    // список прыгал бы наверх при каждой новой строке.
    [[nodiscard]] static SortOrder defaultFor(SortKey key) noexcept {
        SortOrder order;
        order.key = key;
        order.direction = (key == SortKey::Kind) ? SortDirection::Ascending : SortDirection::Descending;
        return order;
    }

    friend bool operator==(const SortOrder&, const SortOrder&) = default;
};

// Значение «выделения нет» и «строки нет». Не npos в std::string::npos:
// пользователь выбирал это число сотни раз, и оно должно называть отсутствие,
// а не «не найдено в строке».
inline constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

// ---------------------------------------------------------------------------
// Модель экрана
// ---------------------------------------------------------------------------

// Всё состояние экрана «Отчёт» без единого Win32-типа. Не потокобезопасна и не
// имеет блокировок намеренно: объект принадлежит UI-потоку (§6.1), а гонка с
// фоновым потоком закрыта на мосте — фон публикует неизменяемый снимок, в
// UI-поток он попадает через PostMessage (§6.4).
class ReportViewModel {
public:
    ReportViewModel();

    // --- Снимок отчёта (SPEC §6.4) -----------------------------------------
    //
    // Приём снимка пересобирает журнал, но НЕ сбрасывает выделение и фильтры:
    // выделение задано ключом строки, а ключи переживают новый отчёт. Иначе
    // каждый экспорт возвращал бы человека в начало списка.
    void publishReport(core::Report report);
    void publishReport(std::shared_ptr<const core::Report> report);
    [[nodiscard]] bool hasReport() const noexcept;
    // nullptr — отчёта ещё не было. Указатель действителен до следующего
    // publishReport или clearReport: снимок иммутабелен (§6.4).
    [[nodiscard]] const core::Report* report() const noexcept;
    void clearReport();

    // --- Журнал приложения (кольцо core::Logger) ---------------------------
    //
    // refreshJournal читает кольцо лога: это единственный источник, который
    // знает про события вокруг операции, и он потокобезопасен (Logger::snapshot
    // отдаёт копию под своим mutex). Проверка lastSequence() перед snapshot()
    // не нужна — snapshot и lastSequence берут один и тот же mutex, а лишняя
    // пара вызовов на каждый кадр журнала дороже выигрыша.
    void refreshJournal();
    // Готовая лента от моста (тесты, второй источник). Сортируется и фильтруется
    // так же, как кольцо лога.
    void publishJournal(std::vector<core::LogRecord> records);
    [[nodiscard]] std::size_t journalCount() const noexcept;
    // Сквозной номер последней прочитанной записи лога: экран по нему решает,
    // есть ли смысл перечитывать кольцо (лишний refresh на пустом кольце
    // перерисовывает список впустую).
    [[nodiscard]] std::uint64_t journalSequence() const noexcept;

    // --- Фильтры и сортировка ------------------------------------------------
    void setFilters(ReportFilters filters);
    void setFilter(std::string_view which, bool enabled);
    void toggleProblemsOnly();
    void setSearch(std::string_view text);
    void clearFilters();
    [[nodiscard]] ReportFilters filters() const noexcept;
    void setSort(SortKey key, SortDirection direction);
    void toggleSort(SortKey key);
    [[nodiscard]] SortOrder sort() const noexcept;

    // --- Строки журнала ------------------------------------------------------
    [[nodiscard]] const std::vector<JournalRow>& rows() const noexcept;
    [[nodiscard]] std::size_t rowCount() const noexcept;
    [[nodiscard]] const JournalRow* rowAt(std::size_t index) const;
    [[nodiscard]] const JournalRow* selectedRow() const;
    [[nodiscard]] std::size_t indexForKey(std::string_view key) const;
    [[nodiscard]] std::string keyAt(std::size_t index) const;
    // Сколько строк каждого вида прошло через фильтры — для строки состояния.
    [[nodiscard]] std::size_t visibleCount(RowKind kind) const noexcept;
    // Строк скрыто фильтрами (0 — фильтры ничего не скрыли).
    [[nodiscard]] std::size_t hiddenCount() const noexcept;

    // --- Выделение и клавиатура (§5 «Клавиатурная навигация, фокус») ---------
    //
    // Стрелки вверх-вниз обрабатывает сам SysListView32 — модель их не трогает,
    // иначе одно нажатие двигало бы выделение дважды.
    void setSelectedIndex(std::size_t index);
    void setSelectedKey(std::string_view key);
    [[nodiscard]] std::size_t selectedIndex() const noexcept;
    void selectFirst();
    void selectLast();
    void clearSelection();
    // Сдвиг выделения на delta строк; true — состояние изменилось.
    bool moveSelection(int delta);
    // Страница вверх-вниз: страница — это столько строк, сколько влезло в
    // список. Размер страницы окно сообщает модели через setViewLines, иначе
    // PgUp/PgDn прыгали бы на фиксированные 19 строк независимо от окна.
    bool moveSelectionByPage(int direction, int viewLines);
    void setViewLines(int lines);
    // Возвращает true, когда ключ обработан (вызывающему не нужно звать
    // DefWindowProc). Клавиши списка (стрелки, Home/End, PgUp/PgDn) сквозные.
    bool handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown);

    // --- Тексты --------------------------------------------------------------
    [[nodiscard]] std::string titleText() const;
    // Строка состояния: итоги последней операции + путь, куда пишутся отчёты.
    [[nodiscard]] std::string statusText() const;
    // Сводка отчёта по FR-8 (вид, время, версии, счётчики) — одна строка на
    // пункт. Пусто, пока отчёта нет.
    [[nodiscard]] std::vector<std::string> summaryLines() const;
    // Карточка выделенной строки: «подпись: значение», по одной на строку.
    [[nodiscard]] std::vector<std::string> detailLines() const;
    // Что было записано последним («Отчёт сохранён: …», строка report.saved).
    [[nodiscard]] std::string savedText() const;
    // Сколько отчётов осталось в каталоге (FR-8 «последние 20»).
    [[nodiscard]] std::size_t keptCount() const noexcept;
    void noteSave(const std::string& pathUtf8, std::size_t kept, std::size_t pruned);
    // Нарушения инвариантов отчёта (core::validateReport). Нефатальны: экран
    // показывает их, а не отказывается показывать отчёт (SPEC §10).
    [[nodiscard]] std::vector<std::string> problems() const;
    [[nodiscard]] bool hasProblems() const noexcept;

    // --- Экспорт (FR-8) ------------------------------------------------------
    //
    // Опции отчёта — те же, что пишет core::report_json, и модель отдаёт их
    // вызывающему: «маскировать серийники» (§5 «Приватность») и состав
    // разделов. Галочка экрана меняет maskSerials и сразу пересобирает тексты.
    [[nodiscard]] core::ReportOptions reportOptions() const;
    void setReportOptions(const core::ReportOptions& options);
    void setMaskSerials(bool mask);
    [[nodiscard]] bool maskSerials() const noexcept;
    // Настройки HTML-рендера, производные от опций отчёта и языка интерфейса.
    [[nodiscard]] core::HtmlReportOptions htmlOptions() const;
    // Текст отчёта в выбранном формате. Пустая строка — экспортировать нечего
    // (отчёта нет): вызывающий обязан это проверить, иначе он создаст файл на
    // двести байт с заголовком «пусто».
    [[nodiscard]] std::string exportText(ExportFormat format) const;
    [[nodiscard]] bool canExport() const noexcept;
    // Имя файла по умолчанию для окна сохранения: метка локального времени плюс
    // вид операции. Вызывающий подставляет свой каталог.
    [[nodiscard]] std::string suggestedName(ExportFormat format, std::int64_t atUnix) const;

    // --- Раскладка -----------------------------------------------------------
    //
    // Высота карточки деталей в DIP — единственное перетаскиваемое место экрана,
    // и она обязана переживать уход на другую страницу (PageState, §5, §7.2).
    [[nodiscard]] int cardHeightDip() const noexcept;
    void setCardHeightDip(int heightDip);

    // --- Состояние страницы (PageState) --------------------------------------
    void applyPageState(const PageState& state);
    [[nodiscard]] PageState pageState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Раскладка (чистая арифметика, DIP)
// ---------------------------------------------------------------------------
//
// Тот же приём, что на экране «Очистка» (view_cleanup.hpp, CleanupLayout):
// раскладка — чистая функция от метрик, DPI, размеров клиента и ШИРИН ПОДПИСЕЙ,
// без единого HWND. Поэтому её можно посчитать и проверить без окна, а WM_SIZE
// зовёт только её и раздаёт готовые прямоугольники контролам.
//
// Почему ширины подписей приходят аргументом, а не считаются внутри: подпись
// измеряется шрифтом нативного контрола (GetTextExtent), и это единственное
// место, где нужен HWND. Всё остальное — арифметика, которая обязана
// переживать и DPI, и язык, и смену темы без окна.

// Сколько контролов стоит в нижнем ряду: пять кнопок экспорта и две галочки.
// Одна строка не помещается в узкое окно, поэтому ряд переносится (см.
// ReportLayout::compute), и число нужно и в раскладке, и в вызывающем коде.
inline constexpr std::size_t kReportBottomControls = 7;

// Метрики экрана в DIP (1/96 дюйма). Per-monitor v2 (§5) меняет pixelsPerDip,
// но не размеры элементов, поэтому вёрстка не знает про DPI.
struct ReportMetrics {
    double paddingDip{8.0};         // отступ содержимого от края клиента
    double gapDip{6.0};             // зазор между списком, карточкой, строкой и рядом кнопок
    double statusHeightDip{20.0};   // строка состояния под списком
    double buttonHeightDip{28.0};   // высота кнопок и галочек нижнего ряда
    double buttonTextPadDip{20.0};  // запас вокруг подписи кнопки (рамка + воздух)
    // Нижняя граница ширины кнопки: сжатие подписи не имеет права сделать
    // цель нажатия меньше, чем её можно попасть мышью. 56 DIP — ширина
    // «Обновить» вместе с рамкой и полями; ниже Windows кнопку уже не
    // нарисовать читаемой.
    double minButtonWidthDip{56.0};
    double minCardHeightDip{72.0};  // карточка деталей не схлопывается в строку
    double minListHeightDip{48.0};  // и журнал остаётся виден, даже если места мало
    double minWidthDip{520.0};      // ниже этого окно считается тесным (§7.2)
    double minHeightDip{320.0};
};

// Высота кнопки, ниже которой нажимать нельзя. Системная кнопка в Windows имеет
// высоту 23 px при 96 DPI; 24 DIP — тот же пол с запасом на рамку. Пока в клиенте
// есть место, раскладка держит кнопку не ниже этой отметки и жертвует высотой
// списка, а не целью нажатия.
inline constexpr double kMinButtonHeightDip = 24.0;

// Прямоугольник в пикселях клиентской области. Правая и нижняя границы не
// включаются (то же соглашение, что в CleanupRect).
struct ReportRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] int right() const noexcept { return x + width; }
    [[nodiscard]] int bottom() const noexcept { return y + height; }
};

// Раскладка экрана «Отчёт». Ничего не владеет и копируется по значению.
class ReportLayout {
public:
    ReportLayout() = default;

    // natural — ширина каждого из семи нижних контролов по его подписи, в
    // пикселях (её снимает окно через GetTextExtent). Ноль или меньше нуля
    // означает «подпись ещё не измерена» и берётся как минимум.
    static ReportLayout compute(const ReportMetrics& metrics, int dpi, int clientWidthPx, int clientHeightPx,
                                int cardHeightDip, const std::array<int, kReportBottomControls>& natural);

    [[nodiscard]] ReportRect listRect() const noexcept;    // SysListView32 с журналом
    [[nodiscard]] ReportRect statusRect() const noexcept;  // строка состояния
    [[nodiscard]] ReportRect cardRect() const noexcept;    // карточка выделенной строки
    // Прямоугольник нижнего контрола: 0…4 — кнопки экспорта, 5…6 — галочки.
    [[nodiscard]] ReportRect childRect(int index) const noexcept;
    // Сколько строк занял нижний ряд: 1 — поместился, 2 — перенёсся.
    [[nodiscard]] int rows() const noexcept;
    // Подписи не поместились даже в две строки: ширины ужаты до минимума, часть
    // текста обрезана. Окно всё равно остаётся в своих границах.
    [[nodiscard]] bool squeezed() const noexcept;
    // Окно меньше минимума: рисуем то, что помещается, остальное скрыто.
    [[nodiscard]] bool cramped() const noexcept;
    [[nodiscard]] int paddingPx() const noexcept;
    [[nodiscard]] int clientWidthPx() const noexcept;
    [[nodiscard]] int clientHeightPx() const noexcept;

private:
    int width_{0};
    int height_{0};
    int padding_{0};
    int gap_{0};
    int buttonHeight_{0};
    int rows_{0};
    int split_{0};  // индекс первого контрола второй строки
    bool squeezed_{false};
    bool cramped_{false};
    ReportRect list_{};
    ReportRect status_{};
    ReportRect card_{};
    std::array<ReportRect, kReportBottomControls> children_{};

    // Сколько строк нужно семи контролам при данных ширинах (перебор разреза).
    // 0 — не помещается ни одна: ширина контрола больше всей строки.
    [[nodiscard]] static int packRows(const std::array<int, kReportBottomControls>& widths, int gap, int available,
                                      int& split) noexcept;
    // Раскладка одной строки: сумма ширин и зазоров не превышает available.
    [[nodiscard]] static bool rowFits(const std::array<int, kReportBottomControls>& widths, int gap, int available,
                                      int from, int to) noexcept;
    // Ширина строки после раскладки: сумма ширин плюс зазоры между ними.
    [[nodiscard]] static int rowWidth(const std::array<int, kReportBottomControls>& widths, int gap, int from,
                                      int to) noexcept;
    // Последний проход: равномерно ужать набор до суммы, которая помещается в
    // строку. Деление по остатку, а не int(w*k/total): сумма сходится точно, и
    // последняя кнопка не вылезает за край на единицу.
    static void clampRow(std::array<int, kReportBottomControls>& widths, int count, int from, int gap,
                         int available);
};

// ---------------------------------------------------------------------------
// Хранилище отчётов (FR-8: «Отчёт пишется в %LOCALAPPDATA%\MrProper\reports\, с
// ограничением количества файлов (последние 20)»)
// ---------------------------------------------------------------------------
//
// Свободные функции, а не методы окна: запись файла — платформенная работа, и
// мост (задача 75) вправе звать её из рабочего потока, тогда как окно зовёт её
// только когда обработчик экспорта не задан (см. ReportScreen::Callbacks).
// Каждая функция возвращает результат и никогда не бросает: отчёт — не причина
// уронить приложение (SPEC §5, §12).
//
// Путь каталога берётся из %LOCALAPPDATA%, а не из SHGetKnownFolderPath:
// последняя живёт в shell32, а подключать новую системную библиотеку из слоя
// интерфейса нельзя без правки CMakeLists.txt, а слой ui имеет ровно тот
// перечень, который нужен окнам. Переменная окружения — то же самое значение,
// которым пользуется сама Windows для профиля пользователя, а если её нет
// (сломанный профиль), берётся %USERPROFILE%\AppData\Local.

struct SaveOutcome {
    bool ok{};
    std::string pathUtf8;   // полный путь записанного файла в UTF-8
    std::string errorText;  // пусто при успехе; при отказе — с кодом Win32
    std::size_t kept{};     // сколько отчётов осталось в каталоге
    std::size_t pruned{};   // сколько удалено ротацией
};

// Каталог отчётов в UTF-8. Пустая строка — путь не определился (тогда запись
// невозможна, и saveReport вернёт ok == false с причиной).
[[nodiscard]] std::string reportsDirectoryUtf8();

// Отчёты каталога, новые первыми (по имени, а значит по времени). Сортировка
// убывающая: список для человека, который ищет «последний отчёт».
[[nodiscard]] std::vector<std::string> listReportsUtf8();

// Записать текст отчёта: файл с меткой времени и видом операции в указанном
// формате, затем ротация «последние 20». Пустой текст не пишется вовсе — отчёт
// на нулевой байт бесполезен и только занимает место в каталоге (зато «мы писали
// отчёт» в журнале потом врало бы). Имя файла собирается той же функцией, что и
// ReportViewModel::suggestedName, поэтому подсказка и реальный файл не
// расходятся.
SaveOutcome saveReport(std::string_view text, ExportFormat format, core::ReportKind kind, std::int64_t atUnix);

// Удалить всё старше keep файлов нашего формата. Возвращает число удалённых.
// keep == 0 — удалить все наши отчёты (в каталоге ничего чужого не трогается).
std::size_t pruneReports(std::size_t keep);

// ---------------------------------------------------------------------------
// Окно экрана
// ---------------------------------------------------------------------------

// Идентификаторы команд WM_COMMAND. Диапазон 2201… выделен под экран «Отчёт»
// и не пересекается с диапазонами «Очистка» (2001…) и «Диски» (2101…): чужой
// обработчик отличает команду своего экрана по одному сравнению.
enum class ControlId : WORD {
    ExportHtml = 2201,
    ExportJson,
    ExportText,
    RefreshJournal,
    OpenFolder,
    MaskSerials,
    ProblemsOnly,
    First = ExportHtml,
    Last = ProblemsOnly,
};

[[nodiscard]] bool isReportControl(WORD controlId) noexcept;

// Экран «Отчёт» как окно: журнал операций и ошибок на SysListView32 с
// NM_CUSTOMDRAW, карточка выделенной строки, строка состояния, экспорт
// HTML/JSON/текста и две галочки.
//
// Экземпляр живёт на строке вызывающего (обычно мост экранов, задача 75) и
// переживает окно: create/destroy вызываются явно, деструктор закрывает окно,
// если забыли. Обратные обработчики зовутся в UI-потоке, внутри обработчика
// окна, поэтому блокировать в них нельзя.
class ReportScreen {
public:
    // Экспорт: вызывающий решает, куда писать. Экран отдаёт готовый текст и
    // имя файла по умолчанию (§6.1 — запись файла не в UI-потоке). Если
    // обработчик не задан, экран пишет сам через saveReport() и обновляет
    // строку состояния: отчёт без обработчика всё равно нужен человеку
    // (FR-8), а «ничего не делать» — худший из вариантов.
    using ExportHandler = std::function<void(ExportFormat format, const std::string& suggestedName,
                                             const std::string& text)>;
    // Открыть каталог отчётов: вызывающий знает, чем открыть (ShellExecute от
    // него же), экран отдаёт путь и ничего не запускает сам (§5: никаких
    // shell-команд).
    using OpenFolderHandler = std::function<void(const std::string& directory)>;
    // Перечитать отчёт у моста (например, после очистки). Без обработчика
    // кнопка выключена: жать её молча — хуже, чем серой кнопки.
    using RefreshHandler = std::function<void()>;

    struct Callbacks {
        ExportHandler onExport;
        OpenFolderHandler onOpenFolder;
        RefreshHandler onRefresh;
    };

    explicit ReportScreen(Callbacks callbacks = {});
    ~ReportScreen();

    ReportScreen(const ReportScreen&) = delete;
    ReportScreen& operator=(const ReportScreen&) = delete;
    ReportScreen(ReportScreen&&) = delete;
    ReportScreen& operator=(ReportScreen&&) = delete;

    // Создать окно-экран в parent (обычно хост содержимого app_shell). nullptr —
    // не зарегистрировался класс окна или не хватило ресурсов; причина в журнале.
    [[nodiscard]] HWND create(HWND parent, int dpi);
    [[nodiscard]] HWND window() const noexcept;
    void destroy() noexcept;

    // Смена DPI (WM_DPICHANGED): пересчитать раскладку и пересоздать шрифты.
    void setDpi(int dpi);

    [[nodiscard]] ReportViewModel& model() noexcept;
    [[nodiscard]] const ReportViewModel& model() const noexcept;

    // Перенести модель в контролы: после любого изменения модели и после
    // публикации кадра из фона. Список сам не знает, что модель изменилась, —
    // иначе модель знала бы про SysListView32.
    void refresh();

    // Обновление темы: перечитать палитру, пересоздать шрифты и применить к
    // контролам (§5 «Тема»).
    void reloadTheme();

    // --- Приём снимка из фонового потока (звонятся в UI-потоке) --------------
    void publishReport(core::Report report);
    void publishReport(std::shared_ptr<const core::Report> report);
    // Готовая лента журнала от моста; без неё экран читает кольцо core::Logger.
    void publishJournal(std::vector<core::LogRecord> records);
    // Перечитать кольцо лога: экран зовёт это по своему таймеру, чтобы журнал
    // дополнялся сам, пока открыт (FR-6: ошибки приезжают и после операции).
    void refreshJournal();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mrproper::ui::report
