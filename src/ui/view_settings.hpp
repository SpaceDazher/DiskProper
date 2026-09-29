// MrProper — экран «Настройки»: правила, уровни риска, версия набора,
// проверка обновлений, сброс к встроенному набору.
//
// Спека: §4 FR-9 (правила вкл/выкл, смена safety-уровня с двойным
// подтверждением для Risky; никакой телеметрии, статистика — только по
// явной кнопке; локальные файлы правил + ручной импорт), §9.2 п.5 и
// «Офлайн-режим и сброс» (пользователь видит версию набора, дату, результат
// последней проверки, кнопки «проверить сейчас» и «вернуть встроенный набор»;
// сброс отключает автообновление до следующего явного включения), §7.1 п.5
// (пятый экран: правила, безопасность, автозапуск, о программе), §8 Этап 4
// («экран настроек, импорт/экспорт набора правил»), §5 (доступность,
// клавиатурная навигация, DPI per-monitor v2, локализация ru + en, тема),
// §6.1 (слой UI — единственный поток с сообщениями, рендер и ввод; UI-поток
// не занимается I/O), §6.4 (результаты не мутируются после публикации), §12
// (Risky по умолчанию скрыты — настройки не могут это отменить молча).
//
// ---------------------------------------------------------------------------
// Что делает этот экран и что он сознательно НЕ делает
// ---------------------------------------------------------------------------
//
// Экран — единственное место, где человек решает, что приложению вообще
// можно удалять. Отсюда три следствия, которые и определяют его устройство:
//
//   1. Решения пользователя (правило включено/выключено, какой у него уровень
//      риска) — это данные, а не состояние окна. Они лежат в модели, имеют
//      ключ по идентификатору правила (не по индексу в списке: индекс меняется
//      при обновлении набора, идентификатор — нет) и переживают и смену
//      набора правил, и перезапуск приложения. Движок берёт их отсюда же через
//      exportSettings() — иначе «я выключил это правило, а оно всё равно
//      чистится» станет возможным.
//
//   2. Проверка обновлений — враждебный вход по построению (§9.2), поэтому
//      экран показывает ровно то, что человек должен знать: какая версия
//      набора активна, когда она проверена и чем закончилась последняя
//      проверка. Политику (схема → версия приложения → подпись → хеши →
//      разбор) считает core::rulesync, а модель только применяет готовое
//      решение ядра: переиначивать её здесь означало бы две реализации одного
//      правила удаления файлов.
//
//   3. Никакой телеметрии (FR-9, ADR-007). «Экспортировать статистику» —
//      единственный способ что-либо отдать наружу, и отдаёт он локальные
//      сведения по явному нажатию, а не по таймеру.
//
// Чего на экране нет и почему:
//
//   * Автозапуска. §7.1 упоминает его в списке блоков экрана, но FR-9 и §8
//     Этап 7 относят его к v1.2 (служба и планировщик). Работающего переключателя
//     здесь нет намеренно: кнопка, которая ничего не делает, вредит больше,
//     чем её отсутствие. В блоке «о программе» написано прямо, что автозапуск
//     и проверка по расписанию придут в v1.2.
//   * Прямого удаления файлов, скачивания набора и записи state.json: это
//     работа моста и platform (§6.1 — UI-поток не занимается I/O). Экран
//     только собирает намерение и отдаёт его вызывающему через обратный
//     обработчик, а результат принимает готовым снимком.
//   * Сети. Проверка обновлений ходит в сеть из фонового потока; экран только
//     показывает «идёт проверка» и принимает результат.
//
// ---------------------------------------------------------------------------
// Почему файл разделён на модель, раскладку и окно (тот же приём, что в
// view_cleanup и view_disks, и по той же причине)
// ---------------------------------------------------------------------------
//
// Модель (SettingsViewModel) — чистый C++ без единого Win32-типа: правила,
// решения пользователя, состояние набора, тексты. Раскладка
// (SettingsLayout) — чистая арифметика в DIP: где лежит список, где кнопки,
// что под указателем. Окно (SettingsScreen) — SysListView32 с чекбоксами и
// NM_CUSTOMDRAW, комбинаторы, флажок автообновления, поле фильтра и кнопки.
//
// Разрез даёт то, ради чего он и сделан: решения «что показывать» и «что
// произойдёт по клику» проверяются без окна, DPI и Direct2D, а в Win32
// остаётся ровно то, чему чистый C++ научить нельзя. Обратных зависимостей у
// слоя нет: этот файл зависит только от core (rules, rulesync, model) и от
// соседей по ui (locale, nav, theme) — ни engine, ни platform, ни scanner.
//
// ---------------------------------------------------------------------------
// Правила этого файла
// ---------------------------------------------------------------------------
//
//   * Поток — только UI (§6.1). Никаких блокировок и никакого I/O: publish*
//     зовутся в UI-потоке (кадр из фона приходит через PostMessage моста,
//     §6.4), и внутри publish* только пересборка строк и перерисовка.
//   * Исключение не пересекает границу Win32: статические процедуры окон ловят
//     всё сами (§5 «устойчивость»), поэтому хуки модели объявлены без
//     noexcept, а чистые функции (раскладка, попадания) помечены noexcept.
//   * Каждый неуспех WinAPI пишется в журнал с кодом ошибки (§5, §12).
//   * Тексты — через ui::locale. Ключей экрана в каталоге строк пока девять
//     (settings.* из задачи 69), а экрану нужно больше; недостающие строки
//     лежат в локальной таблице trFallback в .cpp с явной пометкой, что они
//     временные и переедут в каталог строк, когда им достанется файл.
//   * Уровни риска берутся из core::SafetyLevel, а не из собственного перечисления:
//     уровень риска решает, что будет удалено (FR-4), и две копии этого типа
//     разошлись бы при первом же новом правиле.
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

#include "core/i18n.hpp"
#include "core/model.hpp"
#include "core/rules.hpp"
#include "core/rulesync.hpp"
#include "locale.hpp"
#include "nav.hpp"

namespace mrproper::ui::settings {

// ---------------------------------------------------------------------------
// Строка правила
// ---------------------------------------------------------------------------
//
// Правило в наборе (core::Rule) и решение пользователя (вкл/выкл, уровень
// риска) — разные вещи, и в таблице должны быть видны обе: иначе человек видит
// «Risky» и не понимает, что это уровень из набора, а не то, что он выбрал.
// Поэтому у строки есть и safety (уровень набора), и safety (эффективный,
// с учётом решения), и overridden — было ли решение вручную.

struct RuleRow {
    std::string key;      // идентификатор правила ("temp.user") — ключ состояния
    std::string title;    // заголовок из набора правил, по текущему языку
    std::string category; // идентификатор категории FR-3
    std::string pattern;  // раскрытый шаблон пути, который задаёт корень удаления
    std::string note;     // примечание автора правила

    core::SafetyLevel setSafety{core::SafetyLevel::Review};   // уровень в наборе
    core::SafetyLevel safety{core::SafetyLevel::Review};      // эффективный уровень
    std::int64_t minAgeDays{};                               // минимальный возраст файла

    bool enabled{true};
    bool overridden{false};  // решение пользователя отличается от набора
};

// ---------------------------------------------------------------------------
// Раскладка и попадания (чистая арифметика, DIP)
// ---------------------------------------------------------------------------
//
// Метрики в DIP (1/96 дюйма), а не в пикселях: при переносе окна на другой
// монитор pixelsPerDip меняется, а размеры элементов — нет (§5, DPI per-monitor
// v2). Вёрстка идёт сверху вниз: три строки панели (действия с набором,
// экспорт и автообновление, язык и уровень риска), строка состояния набора,
// фильтр, список правил (единственная гибкая часть), пояснение к выбранному
// правилу и строка «о программе».
struct SettingsMetrics {
    double toolbarRowHeightDip{28.0};
    double statusHeightDip{44.0};     // версия набора + результат последней проверки
    double filterHeightDip{24.0};
    double detailsHeightDip{88.0};    // правило: идентификатор, категория, уровень, путь
    double aboutHeightDip{40.0};      // имя + «никакой телеметрии» + автозапуск v1.2
    double paddingDip{8.0};
    double gapDip{6.0};
    double buttonGapDip{6.0};
    double buttonMinWidthDip{96.0};
    double labelWidthDip{112.0};      // «Язык интерфейса», «Уровень риска» перед комбинаторами
    double languageComboDip{140.0};
    double safetyComboDip{150.0};
    double minWidthDip{560.0};
    double minHeightDip{380.0};
};

// Прямоугольник в пикселях клиентской области. Правая и нижняя границы не
// включаются: соседние прямоугольники делят область без зазора и без двойного
// попадания (то же соглашение, что в RailRect, CleanupRect и DisksRect).
struct SettingsRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(int px, int py) const noexcept;
};

// Что под указателем. Отдельное перечисление, а не сравнение прямоугольников
// в WndProc: так путь «пиксели → действие» проверяется без окна.
enum class HitTarget : std::uint8_t {
    None,
    Toolbar,        // строка кнопок целиком (кнопки разбираются своими прямоугольниками)
    Status,         // строка состояния набора правил
    Filter,         // поле фильтра
    List,           // список правил
    Details,        // пояснение к выбранному правилу
    About,          // блок «о программе»
    CheckNow,       // «Проверить сейчас» (§9.2 п.5)
    ImportRules,    // ручной импорт набора правил (FR-9, Этап 4)
    RestoreEmbedded,  // «Вернуть встроенный набор» (§9.2, обязательная кнопка)
    ExportRuleSet,    // экспорт активного набора правил (Этап 4)
    ExportStatistics,  // «экспортировать статистику» (FR-9, ADR-007)
    AutoUpdate,       // «Обновлять правила автоматически» (§9.2)
    LanguageCombo,    // язык интерфейса (§5 «локализация ru + en»)
    SafetyCombo,      // уровень риска выбранного правила (FR-9)
};

// Количество кнопок в каждой строке панели. Числа, а не «сколько получилось»:
// раскладка обязана знать, сколько мест зарезервировать под подписи до того,
// как контролы измерили свою ширину. Порядок индексов — порядок ControlId.
inline constexpr std::size_t kToolbarRowCount = 3;
inline constexpr std::size_t kToolbarRow1Count = 3;  // проверить, импорт, вернуть встроенный
inline constexpr std::size_t kToolbarRow2Count = 2;  // экспорт набора, экспорт статистики
inline constexpr std::size_t kToolbarButtonCount = kToolbarRow1Count + kToolbarRow2Count;

// Раскладка экрана: чистая функция от метрик, DPI, размеров окна и размеров
// подписей кнопок. Значение ничего не владеет и копируется по знажению,
// поэтому его можно хранить в обработчике WM_PAINT и сравнивать в тесте.
class SettingsLayout {
public:
    SettingsLayout() = default;

    // Ширины кнопок приходят снаружи (после измерения текста контролом): подписи
    // живут в каталоге строк, по-русски и по-английски они разной длины, а
    // фиксированная ширина означала бы либо обрезанную подпись (§12), либо
    // пустое место. Здесь они только распределяются: если места не хватает, все
    // кнопки сжимаются пропорционально, но не ниже buttonMinWidthDip.
    //
    // Номера кнопок идут по порядку ControlId: первые три — первая строка,
    // следующие две — вторая. Третья строка кнопок не имеет: там комбинаторы
    // языка и уровня риска и флажок автообновления, ширина которых задана
    // метриками.
    static SettingsLayout compute(const SettingsMetrics& metrics, int dpi, int clientWidthPx,
                                  int clientHeightPx, const std::vector<int>& buttonWidthsPx = {});

    [[nodiscard]] SettingsRect toolbarRow1Rect() const noexcept;
    [[nodiscard]] SettingsRect toolbarRow2Rect() const noexcept;
    [[nodiscard]] SettingsRect toolbarRow3Rect() const noexcept;
    [[nodiscard]] SettingsRect statusRect() const noexcept;
    [[nodiscard]] SettingsRect filterRect() const noexcept;
    [[nodiscard]] SettingsRect listRect() const noexcept;
    [[nodiscard]] SettingsRect detailsRect() const noexcept;
    [[nodiscard]] SettingsRect aboutRect() const noexcept;

    // Кнопки: индексы 0…2 — первая строка (проверить, импорт, вернуть
    // встроенный), индексы 3…4 — вторая (экспорт набора, экспорт статистики).
    // При тесном окне хвост отбрасывается, и экран обязан знать, какие кнопки
    // вообще создавать.
    [[nodiscard]] SettingsRect buttonRect(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t buttonCount() const noexcept;

    [[nodiscard]] SettingsRect autoUpdateRect() const noexcept;
    [[nodiscard]] SettingsRect languageLabelRect() const noexcept;
    [[nodiscard]] SettingsRect languageComboRect() const noexcept;
    [[nodiscard]] SettingsRect safetyLabelRect() const noexcept;
    [[nodiscard]] SettingsRect safetyComboRect() const noexcept;

    [[nodiscard]] bool filterVisible() const noexcept;
    [[nodiscard]] bool detailsVisible() const noexcept;
    [[nodiscard]] bool aboutVisible() const noexcept;
    // Окно меньше минимума: показываем то, что помещается, и не рисуем остальное.
    [[nodiscard]] bool cramped() const noexcept;
    [[nodiscard]] int clientWidthPx() const noexcept;
    [[nodiscard]] int clientHeightPx() const noexcept;
    // Высота списка, которую он просит под своё содержимое: раскладка отдаёт
    // списку остаток окна, а список — ровно столько, сколько нужно.
    [[nodiscard]] int listHeightPx() const noexcept;

    [[nodiscard]] HitTarget hitTest(int px, int py) const noexcept;

    // Пропорциональное ужимание набора ширин под доступную полосу. Чистая
    // функция: ею пользуются и раскладка, и тест.
    [[nodiscard]] static std::vector<int> fitWidths(int availablePx, const std::vector<int>& desiredPx, int gapPx,
                                                    int minWidthPx);

private:
    int width_{0};
    int height_{0};
    int padding_{0};
    int gap_{0};
    int buttonGap_{0};
    int row1Top_{0};
    int row2Top_{0};
    int row3Top_{0};
    int statusTop_{0};
    int statusHeight_{0};
    int filterTop_{0};
    int filterHeight_{0};
    int listTop_{0};
    int listHeight_{0};
    int detailsTop_{0};
    int detailsHeight_{0};
    int aboutTop_{0};
    int aboutHeight_{0};
    int rowHeight_{0};
    std::size_t row1Count_{kToolbarRow1Count};
    std::size_t row2Count_{kToolbarRow2Count};
    std::vector<int> row1Widths_;
    std::vector<int> row2Widths_;
    int autoUpdateLeft_{0};
    int autoUpdateWidth_{0};
    int languageLabelLeft_{0};
    int languageComboLeft_{0};
    int safetyLabelLeft_{0};
    int safetyComboLeft_{0};
    bool filterVisible_{true};
    bool detailsVisible_{true};
    bool aboutVisible_{true};
    bool cramped_{false};
};

// ---------------------------------------------------------------------------
// Модель экрана
// ---------------------------------------------------------------------------

// Всё состояние экрана «Настройки» без единого Win32-типа. Не потокобезопасна и
// не имеет блокировок намеренно: объект принадлежит UI-потоку (§6.1), а гонка с
// фоновым потоком закрыта на мосте — фон публикует неизменяемый снимок набора
// правил, а в UI-поток он попадает через PostMessage (§6.4).
class SettingsViewModel {
public:
    SettingsViewModel();

    // --- Набор правил (FR-4) -------------------------------------------------
    //
    // Снимок неизменяемый и общий: правила решают, что удаляется, и держать
    // их дважды — значит показать одно и удалить по другому. Решения
    // пользователя (см. ниже) при этом НЕ сбрасываются: они привязаны к
    // идентификаторам правил, а идентификаторы переживают обновление набора.
    void publishRuleSet(std::shared_ptr<const core::RuleSet> rules);
    // Явный набор без снимка: мост может уже разобрать правила и отдать их
    // значением. Копия всё равно делается — модель не зависит от того, кто
    // ещё держит этот набор.
    void publishRuleSet(core::RuleSet rules);
    void clearRuleSet();
    [[nodiscard]] bool hasRuleSet() const noexcept;
    [[nodiscard]] const core::RuleSet* ruleSet() const noexcept;
    [[nodiscard]] std::size_t ruleCount() const noexcept;
    [[nodiscard]] std::size_t visibleRuleCount() const noexcept;

    // Пересобрать подписи строк после смены языка интерфейса: заголовок правила
    // хранится в самом наборе на двух языках, и в строках лежит копия.
    void refreshRuleTexts();

    // --- Решения пользователя по правилам (FR-9) ----------------------------
    //
    // Смена уровня риска идёт через requestSafety(), а не через setSafety() по
    // одной причине: Risky требует двойного подтверждения (§4 FR-9, §9).
    // Первая попытка вооружает правило и возвращает false, вторая (для того же
    // правила) применяет уровень. Понижение риска (Risky → Review → Safe)
    // подтверждения не требует: оно ничего не удаляет, а только сужает выбор.
    bool setRuleEnabled(std::string_view ruleId, bool enabled);
    bool toggleRule(std::string_view ruleId);
    [[nodiscard]] bool ruleEnabled(std::string_view ruleId) const noexcept;

    bool requestSafety(std::string_view ruleId, core::SafetyLevel level);
    // Правило, ждущее второго подтверждения ("" — не вооружено ни одно).
    [[nodiscard]] std::string riskyArmedRuleId() const;
    [[nodiscard]] bool riskyArmed(std::string_view ruleId) const noexcept;
    // Снять ожидание подтверждения: выбор другого правила или нажатие Escape.
    void disarmRisky();

    // Сколько правил изменено человеком, сколько включено. Первое число —
    // главное на экране: «правил N, из них изменено M» отвечает на вопрос
    // «а что именно я наменил» без чтения файла настроек.
    [[nodiscard]] std::size_t overrideCount() const noexcept;
    [[nodiscard]] std::size_t enabledCount() const noexcept;
    // Вернуть все решения к состоянию набора правил. Отдельная от сброса
    // набора кнопка нужна потому, что это разные вещи: набор меняет версия
    // правил, а здесь человек возвращает то, что менял сам.
    void forgetOverrides();

    // --- Фильтр и выделение --------------------------------------------------
    void setFilter(std::string_view filter);
    [[nodiscard]] std::string filter() const;
    // true — строка фильтра не пустая и список отфильтрован.
    [[nodiscard]] bool filtered() const noexcept;

    void setSelectedRule(std::string_view ruleId);
    [[nodiscard]] std::string selectedRuleId() const;
    [[nodiscard]] const RuleRow* selectedRule() const;
    [[nodiscard]] const std::vector<RuleRow>& rules() const noexcept;  // видимые строки
    [[nodiscard]] const RuleRow* ruleById(std::string_view ruleId) const;
    [[nodiscard]] std::optional<std::size_t> indexOfRule(std::string_view ruleId) const;
    [[nodiscard]] std::string ruleIdAt(std::size_t index) const;
    bool moveSelection(int delta);
    void selectFirst();
    void clearSelection();

    // Клавиши экрана (§5 «Клавиатурная навигация, фокус»): пробел вкл/выкл,
    // стрелки влево-вправо меняют уровень риска выбранного правила. Возвращает
    // true, когда ключ обработан (вызывающему не нужно звать DefWindowProc).
    bool handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown);

    // --- Набор правил: версия, проверка, сброс (§9.2) -----------------------
    //
    // Состояние набора — core::RuleSetStatus, то есть ровно то, что пишется в
    // %LOCALAPPDATA%\MrProper\rules\state.json и что переживает перезапуск.
    // Свою копию этого типа модель не заводит: версию, дату и результат
    // проверки читает и сохраняет код, который и применяет набор (ADR-008).
    void setRuleSetStatus(core::RuleSetStatus status);
    [[nodiscard]] const core::RuleSetStatus& ruleSetStatus() const noexcept;
    // Пора ли проверять при старте (§9.2 п.1 — не чаще раза в 24 ч).
    [[nodiscard]] bool shouldCheckNow(std::int64_t nowUnixSeconds) const noexcept;

    // Ручная проверка по кнопке «Проверить сейчас». Интервал в 24 часа её не
    // ограничивает: человек нажал кнопку, значит ждать суток он не собирался.
    // false — проверка уже идёт (вторая кнопка не должна запускать вторую).
    bool beginCheck();
    [[nodiscard]] bool checking() const noexcept;

    // Результат проверки приходит из фонового потока готовым отчётом ядра.
    // Политика §9.2 (применить отвергнутый кандидат / оставить текущий /
    // откатиться) решается здесь функциями core::rulesync, а не вручную.
    void publishCheckResult(const core::RuleSetVerification& verification, std::int64_t nowUnixSeconds);
    // Тот же результат, когда решение уже принято вызывающим (например, он
    // откатил набор к последнему рабочему и знает об этом больше модели).
    void setCheckedStatus(core::RuleSetStatus status);

    // «Вернуть встроенный набор» (§9.2): скачанные правила больше не
    // используются, автообновление выключается до следующего явного включения.
    // Возвращает false, если встроенный набор уже активен и менять нечего.
    bool requestRestoreEmbedded(std::int64_t nowUnixSeconds);

    // Автообновление. По умолчанию включено (core::RuleSetStatus), но после
    // сброса к встроенному — выключено, и включить его должен человек явно.
    void setAutoUpdate(bool enabled);
    [[nodiscard]] bool autoUpdate() const noexcept;

    // Язык интерфейса. Модель только запоминает намерение и сообщает о нём
    // вызывающему: переключение языка трогает глобальное состояние
    // (SetThreadUILanguage, каталог строк) и обязано быть сохранено в
    // настройках, а это уже не работа экрана.
    void setLanguage(core::Language lang);
    [[nodiscard]] core::Language language() const noexcept;

    // --- Тексты (SPEC §5 «локализация», §7.2) -------------------------------
    //
    // Подписи не хранятся в строках правил: они зависят от языка интерфейса и
    // от счётчиков, которые меняются при каждом щелчке, а хранить значило бы
    // хранить копию строки, которая устареет первой.
    [[nodiscard]] std::string rulesetVersionText() const;   // «Версия набора правил: 2026.02.1»
    [[nodiscard]] std::string lastCheckedText() const;      // «Последняя проверка: … — …»
    [[nodiscard]] std::string statusText() const;           // одна строка в строку состояния
    [[nodiscard]] std::string summaryText() const;          // «Правил: 42 · включено: 30 · изменено: 3»
    [[nodiscard]] std::string detailsText() const;          // пояснение к выбранному правилу
    [[nodiscard]] std::string riskyHintText() const;        // просьба подтвердить Risky
    [[nodiscard]] std::string aboutText() const;            // блок «о программе»
    [[nodiscard]] std::string filterLabelText() const;
    [[nodiscard]] std::string autoUpdateText() const;
    [[nodiscard]] std::string languageLabelText() const;
    [[nodiscard]] std::string safetyLabelText() const;
    [[nodiscard]] std::string safetyText(core::SafetyLevel level) const;
    [[nodiscard]] std::string safetyHint(core::SafetyLevel level) const;
    [[nodiscard]] std::string enabledText(bool enabled) const;
    // Подписи кнопок. checkNowText() во время проверки объясняет, что
    // происходит, и кнопка при этом выключена.
    [[nodiscard]] std::string checkNowText() const;
    [[nodiscard]] std::string importRulesText() const;
    [[nodiscard]] std::string restoreRulesText() const;
    [[nodiscard]] std::string exportRuleSetText() const;
    [[nodiscard]] std::string exportStatisticsText() const;
    // Список языков для комбинатора: название языка на его же языке, иначе
    // «English» в русском интерфейсе читается как ошибка перевода.
    [[nodiscard]] static std::vector<std::string> languageNames();
    // Уровни риска для комбинатора в порядке от безопасного к рискованному.
    [[nodiscard]] static std::vector<core::SafetyLevel> safetyLevels();

    // --- Экспорт (FR-9, §8 Этап 4) -----------------------------------------
    //
    // Никакой телеметрии: наружу отдаётся только то, что человек сам нажал
    // кнопкой отдать. Файлы не пишутся — это дело вызывающего (§6.1).
    std::string statisticsText() const;  // «экспортировать статистику»
    std::string ruleSetExportJson() const;  // «экспортировать набор правил»

    // --- Сохранение между запусками (FR-9) ----------------------------------
    //
    // Формат — тот же плоский «ключ → значение», что у NavStateStore и
    // PageState (nav, задача 70): такие файлы правят руками, поэтому они
    // должны оставаться читаемыми. Куда их положить (реестр или JSON) решает
    // мост, а не экран.
    void exportSettings(NavStateStore& store) const;
    // Сколько ключей принято. problems (необязателен) получает то, что
    // прочитать не удалось: молча выброшенная настройка читается как «сбросил
    // программу».
    std::size_t importSettings(const NavStateStore& store, std::vector<std::string>* problems = nullptr);

    // --- Состояние страницы (PageState, задача 70) ---------------------------
    // Возврат на «Настройки» после другого экрана обязан вернуть выбранное
    // правило, прокрутку списка и строку фильтра (§5, §7.2). Прокрутка приходит
    // из окна (ListView_GetTopIndex) в DIP и хранится здесь же, чтобы
    // pageState() был полным.
    void setScrollOffsetDip(int dip);
    [[nodiscard]] int scrollOffsetDip() const noexcept;
    void applyPageState(const PageState& state);
    [[nodiscard]] PageState pageState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Окно экрана
// ---------------------------------------------------------------------------

// Идентификаторы команд WM_COMMAND. Диапазон 2201… выделен под экран
// «Настройки» и не пересекается с диапазонами «Очистки» (2001…) и «Дисков»
// (2101…): чужой обработчик отличает команду экрана от своей по одному
// сравнению, а не по списку.
enum class ControlId : WORD {
    CheckNow = 2201,
    ImportRules,
    RestoreEmbedded,
    ExportRuleSet,
    ExportStatistics,
    First = CheckNow,
    Last = ExportStatistics,
};

[[nodiscard]] bool isSettingsControl(WORD controlId) noexcept;

// Экран «Настройки» как окно: список правил на SysListView32 с чекбоксами и
// NM_CUSTOMDRAW, строка состояния набора, фильтр, комбинаторы языка и уровня
// риска, флажок автообновления и пять кнопок.
//
// Экземпляр живёт на стеке вызывающего (обычно мост экранов, задача 75) и
// переживает окно: create/destroy вызываются явно, деструктор закрывает окно,
// если забыли. Обратные обработчики зовутся в UI-потоке, внутри обработчика
// окна, поэтому блокировать в них нельзя.
class SettingsScreen {
public:
    // Проверить обновления правил сейчас (§9.2 п.5). Сеть и файлы — не здесь:
    // вызывающий запускает фоновую проверку и возвращает результат через
    // SettingsScreen::publishCheckResult.
    using CheckNowHandler = std::function<void()>;
    // Ручной импорт набора правил (FR-9 «импорт вручную», §8 Этап 4): файл
    // выбирает и проверяет вызывающий (core::rulesync + platform), экран
    // только инициирует выбор.
    using ImportRulesHandler = std::function<void()>;
    // «Вернуть встроенный набор»: вызывающий удаляет скачанные правила с диска
    // (§9.2), модель уже перевела состояние на встроенный набор.
    using RestoreEmbeddedHandler = std::function<void()>;
    // Экспорт: экран отдаёт готовый текст, файл выбирает и пишет вызывающий
    // (§6.1 — запись файла не в UI-потоке).
    using ExportHandler = std::function<void(const std::string& text)>;
    // Смена языка интерфейса: модель запомнила намерение, а применить язык и
    // сохранить его — дело вызывающего (язык трогает глобальное состояние).
    using LanguageHandler = std::function<void(core::Language lang)>;

    struct Callbacks {
        CheckNowHandler onCheckNow;
        ImportRulesHandler onImportRules;
        RestoreEmbeddedHandler onRestoreEmbedded;
        ExportHandler onExportRuleSet;
        ExportHandler onExportStatistics;
        LanguageHandler onLanguageChanged;
    };

    explicit SettingsScreen(Callbacks callbacks = {});
    ~SettingsScreen();

    SettingsScreen(const SettingsScreen&) = delete;
    SettingsScreen& operator=(const SettingsScreen&) = delete;
    SettingsScreen(SettingsScreen&&) = delete;
    SettingsScreen& operator=(SettingsScreen&&) = delete;

    // Создать окно-экран в parent (обычно хост содержимого app_shell). nullptr —
    // не зарегистрировался класс окна или не хватило ресурсов; причина в журнале.
    [[nodiscard]] HWND create(HWND parent, int dpi);
    [[nodiscard]] HWND window() const noexcept;
    void destroy() noexcept;

    // Смена DPI (WM_DPICHANGED): пересчитать раскладку, пересоздать шрифты и
    // D2D-ресурсы, применить новые размеры колонок списка.
    void setDpi(int dpi);

    [[nodiscard]] SettingsViewModel& model() noexcept;
    [[nodiscard]] const SettingsViewModel& model() const noexcept;

    // Перенести модель в контролы: после любого изменения модели и после
    // публикации кадра из фона. Список сам не знает, что модель изменилась, —
    // иначе модель знала бы про SysListView32.
    void refresh();

    // Обновление темы: перечитать палитру, шрифты и применить к контролам
    // (§5 «Тема»).
    void reloadTheme();

    // --- Приём кадров из фонового потока (звонятся в UI-потоке) -------------
    void publishRuleSet(std::shared_ptr<const core::RuleSet> rules);
    void publishRuleSet(core::RuleSet rules);
    void publishCheckResult(const core::RuleSetVerification& verification, std::int64_t nowUnixSeconds);
    void setCheckedStatus(core::RuleSetStatus status);
    // Геометрия окна — по договорённости с app_shell (задача 66 оставила
    // сохранение положения окна задаче 74). Каркас снимает WINDOWPLACEMENT при
    // закрытии, экран держит его и отдаёт мосту, а тот пишет его в настройки
    // через SettingsViewModel::exportSettings. Тип WINDOWPLACENT — Win32,
    // поэтому эти три метода живут на экране, а не в модели: модель без
    // единого Win32-типа проверяется отдельно от окна.
    void setWindowPlacement(const WINDOWPLACEMENT& placement) noexcept;
    [[nodiscard]] WINDOWPLACEMENT windowPlacement() const noexcept;
    [[nodiscard]] bool hasWindowPlacement() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mrproper::ui::settings
