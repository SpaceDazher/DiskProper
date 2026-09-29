// mrproper-cli: команды `plan` и `apply` (SPEC §4 FR-5, §6.2 «cli», §7.2).
//
// Что именно здесь и почему. Спека требует от CLI двух вещей (SPEC §6.2, строка
// `cli`): «mrproper-cli scan --json, --plan, --apply — headless-режим для CI и
// e2e». Настоящий файл закрывает вторую половину этой строки:
//
//   * `plan` — показать план очистки и ничего не удалять; `--json` печатает ровно
//     тот документ, который строит core::plan::planToJson, чтобы план из CLI и
//     план на экране «Очистка» не расходились ни в одном поле;
//   * `apply` — очистка с обязательным подтверждением. Dry-run тут не «режим
//     отладки», а поведение по умолчанию (FR-5: «Dry-run обязателен и запускается
//     по умолчанию перед первым удалением в сессии»): без `--execute` команда
//     показывает точный список операций и выходит с кодом Ok, удалив ноль байт.
//
// Три решения, которые модуль принимает на себя, а не отдаёт вызывающему:
//
//  1. Подтверждение нельзя пропустить «по недосмотру». Спросить должны,
//     перечислив, что именно будет удалено, и ждать ответа человека: слово
//     DELETE (для плана с Risky — DELETE-RISKY, второе подтверждение по §12).
//     Ключ --yes отменяет вопрос, но не отменяет печать списка операций и
//     снимка состояния (FR-5: «перед исполнением — снимок состояния: список
//     операций, PID, версия, размер; запись в журнал»). Тот же список
//     подтверждает core::DryRunGate: исполнить можно только план, который в
//     этой же сессии показан; сменились выборы — подтверждение сгорело.
//
//  2. Удалять по файлу нельзя. Ключ --candidates читает готовый JSON (дамп скана
//     или отчёт), и это удобно для golden-тестов и e2e, но для удаления список
//     мог устареть: между сканом и очисткой файл мог исчезнуть, появиться или
//     сменить владельца. Поэтому --candidates + --execute отклоняется, а
//     удаление работает только по кандидатам живого скана. Плановые проверки
//     движка (путь внутри корня правила, белый список защищённых каталогов,
//     пропуск reparse) — это FR-6 и зона engine::CleanupExecutor; здесь они не
//     дублируются, иначе две реализации разойдутся.
//
//  3. Скан и исполнитель операций не выдумываются, а внедряются. Правило
//     команды — не придумывать чужой API: движок уже собран
//     (engine::ScanCoordinator и engine::CleanupExecutor, волна G1), и его
//     подключение живёт в makeFileEnvironment(), а не в разборе аргументов.
//     Через ApplyEnvironment проходят три точки: candidates (где взять
//     кандидатов), executeOperation (выполнить одну операцию — осталось для
//     вызывающих, у которых есть свой исполнитель), liveScan и executePlan
//     (настоящий путь CLI: живой скан и настоящий исполнитель целиком).
//     Пока исполнитель не подключён, --execute честно отказывает с кодом
//     NoExecutor, а не изображает очистку.
//
//  4. Удаление идёт по кандидатам ЖИВОГО скана, а не по файлу. Список из
//     --candidates для удаления запрещён (FR-5: план — снимок, а не разрешение
//     навсегда), поэтому `apply --execute` без --candidates сам разворачивает
//     правила из --rules, обходит ФС и строит план по только что увиденному.
//     Набор правил нужен исполнителю и отдельно: корень правила — это граница
//     проверки «путь внутри корня» (FR-6), а без него любая операция была бы
//     SkippedOutsideRoot.
//
// Формат машинного вывода (--json), как его читает CI:
//
//   * `plan --json`   — документ core::plan::planToJson (схема 1);
//   * `apply --json`  — документ этой команды: { schema, kind:"apply", dryRun,
//     plan:<документ planToJson>, snapshot:<снимок состояния>, execution:{…} }
//     либо execution:null, когда операция была отказной (--execute не задан).
//   Человеческий текст и вопрос о подтверждении всегда идут в err: stdout в
//   режиме --json остаётся машинным (то же требование, что у `scan --json`).
//
// Локализация: сообщения команд — по-русски, как текст core::plan и остальной
// вывод CLI. SPEC §1.1 прямо называет CLI инструментом разработчика, CI и e2e,
// отдельная локализация CLI в MVP не проектировалась.
#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "core/plan.hpp"
#include "core/rules.hpp"

namespace mrproper::cli {

// ---------------------------------------------------------------------------
// Коды возврата
// ---------------------------------------------------------------------------
// Свои значения, а не переиспользование общих кодов каркаса CLI: общие коды
// объявляет main.cpp (задача «каркас»), и объявление одного и того же имени в
// одном namespace — это переопределение, из-за которого перестанет собираться
// всё, что включает оба заголовка. Вызывающий отображает эти значения в свои
// как угодно; здесь они названы по смыслу, чтобы по логу и по коду было видно,
// что именно произошло.
enum class PlanExit : int {
    Ok = 0,           // план показан; в apply без --execute — удалено 0 байт
    Usage = 2,        // не разобрали аргументы: неизвестный ключ, нет значения, два взаимоисключающих ключа
    Refused = 3,      // подтверждение не получено или выполнение запрещено — ничего не удалено
    NoCandidates = 4, // нечем планировать: источник кандидатов не задан или не прочитан
    NoExecutor = 5,   // запрошено выполнение, а исполнитель операций не подключён
    Failed = 6,       // часть операций не удалась; ошибки не фатальны (FR-6), план исполнен частично
};

// ---------------------------------------------------------------------------
// Разобранные аргументы
// ---------------------------------------------------------------------------
// Всё, что влияет на решения, лежит в двух структурах: ApplyOptions::plan —
// это core::PlanOptions без переименований (профиль, порог уверенности, корзина,
// Risky), остальное — про сам запуск команды.
struct ApplyOptions {
    core::PlanOptions plan{};

    bool json{false};         // --json: в stdout только машинный вывод
    bool execute{false};      // --execute: реально удалять (без него — dry-run)
    bool yes{false};          // --yes: не задавать вопроса (неинтерактивный режим CI)
    bool help{false};         // --help / -h

    // --category <id> — оставить только эти категории (можно повторять и перечислять
    // через запятую). Пусто — все категории. Фильтр применяется к кандидатам до
    // buildPlan, поэтому индексы в плане совпадают с позициями в отфильтрованном
    // списке (инвариант §6.3 держится сам по себе).
    std::vector<std::string> categories;

    // --candidates <файл> — взять кандидатов из JSON: отчёт core::report_json
    // (раздел "candidates") либо голый массив кандидатов. Только для показа
    // плана: вместе с --execute такая комбинация отклоняется (см. выше).
    std::optional<std::string> candidatesPath;

    // --include-review — брать кандидатов уровня Review (SPEC §4 FR-3, FR-4).
    // По умолчанию берётся только Safe: пароли браузера, prefetch и доставка
    // не должны попадать в план сами (docs/review-02.md F-03). Включение —
    // явное действие человека, как галочка на экране «Очистка»; профиль
    // everything берёт Review и без этого ключа.
    bool includeReview{false};

    // --rules <каталог> — набор правил для ЖИВОГО скана. Только этот путь
    // пригоден для удаления: кандидаты берутся из только что увиденной файловой
    // системы, а не из файла (FR-5). Пусто — живого скана нет, и тогда
    // кандидаты можно взять только из --candidates (то есть план без удаления).
    std::string rulesPath;

    // --trash-root <каталог> — куда складывать корзину приложения (FR-7).
    // Пусто — %ProgramData%\MrProper\Trash (platform::defaultTrashRoot).
    // Ключ существует не для красоты: песочница проверки и CI обязаны держать
    // транзакции в своём каталоге, а не в чужом ProgramData.
    std::string trashRoot;
};

// Разбор аргументов команды (без имени команды: caller отдаёт только хвост).
// Пустой optional с заполненной error — ошибка разбора; пустой optional с
// пустой error означает «спросить --help».
std::optional<ApplyOptions> parseApplyOptions(const std::vector<std::string>& args, std::string& error);

// Тексты --help. Отдельная функция, а не константа в заголовке: caller (main.cpp)
// печатает их в общий список команд.
const char* planUsage();
const char* applyUsage();

// ---------------------------------------------------------------------------
// Потоки и внедряемые зависимости
// ---------------------------------------------------------------------------
// Потоки передаются явно по трём причинам: команда проверяется в тесте без
// перенаправления stdout, прогресс идёт в отдельный поток от машинного вывода,
// а подтверждение читается не из глобального std::cin.
struct ApplyIo {
    std::ostream& out;  // машинный вывод: JSON или текст плана
    std::ostream& err;  // человек: подтверждение, ход операций, ошибки
    std::istream& in;   // ответ на вопрос о подтверждении
};

// Результат одной операции — то, что о ней знает исполнитель (engine).
// Строки не фатальны: ошибка одной операции не отменяет остальные (FR-6).
struct OperationResult {
    bool ok{};                       // операция выполнена
    std::string status;              // что именно сделано: Deleted, Trashed, AlreadyGone, Locked, …
    std::uint64_t freedBytes{};      // фактически освобождено (аллоцированный размер, FR-4)
    std::string transactionId;       // txId корзины приложения для Trash (FR-7), пусто для Delete
    std::string detail;              // HRESULT/текст платформы; при ошибке — обязателен
};

// Исполнитель одной операции плана. Возвращает false, когда работать дальше
// нельзя (нет прав, отмена, диск отвалился): цикл прерывается, а уже
// сделанное остаётся сделанным и попадает в отчёт. Само действие пишет
// результат в out. Смысл разделения: «операция не удалась» — это результат,
// «исполнять нельзя» — это отказ всего запуска, и смешивать их нельзя.
using OperationExecutor =
    std::function<bool(const core::PlanOperation&, const core::CleanupCandidate&, OperationResult&)>;

// Источник кандидатов: живой скан (engine::ScanCoordinator) либо, если он не
// подключён, --candidates <файл>. false + error означает «источника нет».
using CandidateSource = std::function<bool(std::vector<core::CleanupCandidate>&, std::string& error)>;

// Кандидаты вместе с манифестами и набором правил, из которого их собрали.
//
// Набор правил едет вместе с кандидатами не для красоты: исполнитель выводит
// из него корень правила, а корень — это граница проверки «путь внутри корня
// правила» (FR-6). Без него любая операция закончилась бы SkippedOutsideRoot,
// то есть удаление было бы невозможно в принципе.
struct LiveScan {
    std::vector<core::CleanupCandidate> candidates;
    std::vector<core::CandidateManifest> manifests;
    std::shared_ptr<const core::RuleSet> rules;
};

// Живой скан по набору правил из каталога (--rules). Второй параметр —
// человеческий канал: прогресс обхода идёт в stderr, потому что stdout в
// режиме --json остаётся машинным (§6.2).
using LiveScanSource = std::function<bool(const std::string& rulesPath, const std::vector<std::string>& categories,
                                          std::ostream& humanChannel, LiveScan& out, std::string& error)>;

// Что исполнителю нужно на фазе проверки и при переносе в корзину. Указатели,
// а не значения: те же объекты живут в отчёте команды, а копия плана на
// тысячи строк здесь была бы второй правдой о том, что собираются удалить.
struct PlanExecutionInput {
    const std::vector<core::CleanupCandidate>* candidates{};
    const std::vector<core::CandidateManifest>* manifests{};
    const core::CleanupPlan* plan{};
    // Набор правил, по которому шёл скан. nullptr — корни правил неизвестны, и
    // тогда исполнитель не сможет проверить принадлежность корню (FR-6).
    const core::RuleSet* rules{};
    std::string trashRoot;    // пусто — корзина приложения по умолчанию (FR-7)
    std::string appVersion;   // пишется в манифест транзакции
    // Куда писать ход операций. nullptr — молча: команда печатает итог сама.
    std::ostream* progress{nullptr};
};

// Что исполнитель сделал с одним кандидатом. Индекс — позиция кандидата в том
// списке, который ушёл исполнителю (инвариант §6.3).
struct PlanExecutionItem {
    bool ok{};                   // цель достигнута: удалено, перенесено в корзину или уже отсутствовало
    std::string status;          // Deleted | Trashed | AlreadyGone | Locked | Protected | …
    std::uint64_t freedBytes{};  // фактически освобождено
    std::string transactionId;   // транзакция корзины, если был перенос (FR-7)
    std::string detail;          // причина: HRESULT платформы или «почему пропустили»
};

// Итог прогона целиком.
struct PlanExecution {
    std::vector<PlanExecutionItem> items;  // по одному на кандидата, в том же порядке
    std::uint64_t freedBytes{};
    std::string transactionId;  // транзакция корзины прогона, если была (FR-7)
    bool aborted{false};         // прогон прерван отменой
    std::string abortReason;
    std::string summary;  // итоговая строка движка для stderr
};

// Исполнитель всего плана целиком (engine::CleanupExecutor). false — прогон не
// состоялся (dry-run, нечего делать, чек-лист разошёлся с планом, отмена до
// старта), и тогда out.abortReason объясняет почему: молча вывести «удалено 0
// байт» после такого отказа было бы враньём о результате.
using PlanExecutor = std::function<bool(const PlanExecutionInput&, PlanExecution&)>;

// Снимок состояния (FR-5): версия, PID, время — их знает платформа, ядро
// оставляет поля пустыми и ждёт их от сюда.
using SnapshotContextProvider = std::function<core::PlanSnapshotContext()>;

// Вопрос человеку и его ответ. false означает «спросить не удалось» (конец
// ввода, поток закрыт) — это отказ, а не согласие.
using ConfirmationPrompt = std::function<bool(const std::string& question, std::string& answer)>;

struct ApplyEnvironment {
    CandidateSource candidates;                // пусто → только --candidates
    OperationExecutor executeOperation;        // пусто → --execute откажется (NoExecutor)
    LiveScanSource liveScan;                   // пусто → --rules не работает
    PlanExecutor executePlan;                  // пусто → --execute откажется (NoExecutor)
    SnapshotContextProvider snapshotContext;   // пусто → снимок без версии/PID/времени
    ConfirmationPrompt ask;                    // пусто → вопрос задаётся в err, ответ читается из io.in
    // Версия приложения для снимка состояния и манифеста транзакции корзины
    // (FR-7). Пусто — версия не выдумывается.
    std::string appVersion;
};

// Готовое окружение настоящей программы: кандидаты — живой скан по --rules,
// операции выполняет engine::CleanupExecutor вместе со своими проверками FR-6,
// снимок состояния знает версию и PID процесса. Команда, у которой своё
// окружение (тесты, библиотека), остаётся переносимой: движок подключается
// только здесь, а не в разборе аргументов и не в построении плана.
ApplyEnvironment makeFileEnvironment();

// Вопрос в err, ответ — одна строка из in. Возвращает false, если строку
// прочитать не удалось; answer в этом случае пуст.
bool askOnStream(const std::string& question, const ApplyIo& io, std::string& answer);

// ---------------------------------------------------------------------------
// Результат команды
// ---------------------------------------------------------------------------
// Ход исполнения. Отдаётся наружу целиком: вызывающий (main.cpp) может
// показать его в отчёте, не разбирая вывод консоли.
struct ExecutionSummary {
    std::size_t attempted{};      // сколько операций ушло исполнителю
    std::size_t succeeded{};      // сколько завершилось успехом
    std::size_t failed{};         // сколько с ошибкой (FR-6: не фатально)
    std::uint64_t freedBytes{};   // фактически освобождено
    bool aborted{};               // исполнитель попросил остановиться
    std::string abortReason;      // чем остановился (пусто при aborted == false)
    std::string transactionId;    // транзакция корзины прогона (FR-7), пусто — переноса не было
    std::vector<core::PlanOperation> operations;  // ровно те, что ушли исполнителю
    std::vector<OperationResult> results;         // параллельный вектор: results[i] к operations[i]
    std::vector<core::PlanOperation> notRun;      // начать не успели: план показан, но не выполнен
};

struct ApplyReport {
    std::vector<core::CleanupCandidate> candidates;  // по ним построен план; нужны исполнителю и отчёту
    std::vector<core::CandidateManifest> manifests;  // что каждому кандидату разрешено удалять (F-01)
    // Набор правил, по которому шёл живой скан: нужен исполнителю для корня
    // правила (FR-6). Пусто — кандидаты пришли из файла, а удалять по файлу
    // запрещено, поэтому корень и не понадобится.
    std::shared_ptr<const core::RuleSet> rules;
    core::CleanupPlan plan;        // что решил core::plan
    core::DryRunReport dryRun;     // тот же список операций текстом (FR-5)
    core::PlanSnapshot snapshot;   // снимок состояния перед исполнением (FR-5)
    ExecutionSummary execution;    // пусто, если --execute не задан
    bool executed{false};          // true только если исполнитель реально отработал
    bool confirmed{false};         // подтверждение получено (в том числе по --yes)
    std::string refusal;           // почему ничего не выполнено; пусто, если вопросов не было
};

// ---------------------------------------------------------------------------
// Точки входа
// ---------------------------------------------------------------------------
// Основные функции: разбор аргументов, работа, вывод и код возврата. out
// заполняется всегда (кроме ошибки разбора аргументов) — вызывающему отчёт
// нужен и когда команда отказала.
PlanExit runPlan(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env,
                 ApplyReport& out);
PlanExit runApply(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env,
                  ApplyReport& out);

// Оборачиватели для main.cpp: тот же выход, но без отчёта.
int runPlanCommand(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env);
int runApplyCommand(const std::vector<std::string>& args, const ApplyIo& io, const ApplyEnvironment& env);

// ---------------------------------------------------------------------------
// Кандидаты из JSON
// ---------------------------------------------------------------------------
// Принимает и отчёт core::report_json (раздел "candidates"), и голый массив
// кандидатов в том же формате. Обязателен только path: остальные поля имеют
// осмысленные умолчания, а отсутствующий файл читается пустым кандидатом и
// выглядит как «нашлось 0 байтов» — это худший вид молчания для команды, из
// которой потом удаляют. Ошибка всегда называет элемент, который не разобрался.
std::vector<core::CleanupCandidate> parseCandidatesJson(std::string_view text, std::string& error);

// То же плюс манифесты разрешённого к удалению (docs/review-02.md F-01): файл
// скана несёт ключ "manifest" в каждом кандидате, и без него план не знает, что
// именно удалять. Кандидату без манифеста CLI восстанавливает перечень сам —
// перечислением корня, и только если перечень совпадает с объявленным числом
// файлов; иначе манифеста не будет, и элемент останется в плане с причиной.
bool parseCandidatesJson(std::string_view text, std::vector<core::CleanupCandidate>& candidates,
                         std::vector<core::CandidateManifest>& manifests, std::string& error);

// Тот же разбор из файла. Путь в UTF-8 (как везде в модели, §6.3), BOM и
// переводы строк переживаются, файл больше kMaxCandidatesFileBytes не читается.
bool loadCandidatesFile(const std::string& path, std::vector<core::CleanupCandidate>& out, std::string& error);
bool loadCandidatesFile(const std::string& path, std::vector<core::CleanupCandidate>& out,
                        std::vector<core::CandidateManifest>& manifests, std::string& error);

// Потолок файла кандидатов: список на 64 МБ — это уже не «дамп скана», а
// подозрительный вход, и читать его молча нельзя.
inline constexpr std::uint64_t kMaxCandidatesFileBytes = 64ull * 1024ull * 1024ull;

// Слово, которое надо ввести для подтверждения. Для плана, где есть хоть одна
// операция Risky, — отдельное слово: второе подтверждение опасного уровня
// (SPEC §12, FR-9). Берём DryRunReport, а не CleanupPlan: уровень Risky есть
// только у операции, у плана хранится действие и байты.
std::string requiredConfirmationToken(const core::DryRunReport& dryRun);

}  // namespace mrproper::cli
