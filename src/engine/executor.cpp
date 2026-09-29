// Исполнение плана очистки: пул, чек-лист, Skip (locked), сбор ошибок.
// Контракт модуля и разбор требований FR-6 — в шапке executor.hpp; здесь код.
//
// Порядок чтения файла: утилиты и словари (почему именно эти строки) → словари
// статусов, чек-лист и отчёт → состояние прогона → фаза A (проверки) → фаза B
// (закрытие приложений) → фаза C (исполнение) → фаза D (финализация) → класс
// CleanupExecutor (пуск, отмена, прогресс, публикация отчёта).
#include "executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/glob.hpp"
#include "core/log.hpp"
#include "core/trash.hpp"
#include "core/units.hpp"
#include "platform/process_control.hpp"
#include "platform/vfs_delete.hpp"
#include "platform/vfs_paths.hpp"
#include "platform/vfs_trash.hpp"
#include "platform/win_error.hpp"

namespace mrproper::engine {
namespace {

namespace vfs = platform::vfs;
namespace vfs_paths = platform::vfs_paths;
namespace shutdown = platform::shutdown;

// ---------------------------------------------------------------------------
// Имена событий журнала
// ---------------------------------------------------------------------------
// Отдельные константы, а не склейка «префикс + суффикс» по месту: grep по
// журналу должен находить все записи модуля одним словом, а склейка строк на
// каждом вызове стоила бы аллокации в горячем пути.
constexpr std::string_view kEventRefused{"engine.executor.refused"};
constexpr std::string_view kEventStart{"engine.executor.start"};
constexpr std::string_view kEventSkip{"engine.executor.skip"};
constexpr std::string_view kEventCloseAsk{"engine.executor.close"};
constexpr std::string_view kEventTrash{"engine.executor.trash"};
constexpr std::string_view kEventManifest{"engine.executor.trash.manifest"};
constexpr std::string_view kEventItemFailed{"engine.executor.item.failed"};
constexpr std::string_view kEventJobFailed{"engine.executor.job.failed"};
constexpr std::string_view kEventFinish{"engine.executor.finish"};
constexpr std::string_view kEventAborted{"engine.executor.aborted"};

// ---------------------------------------------------------------------------
// Время
// ---------------------------------------------------------------------------
//
// Наносекунды steady_clock — для длительностей (часы нельзя двигать назад, а
// длительность в отчёте должна быть неотрицательной); unix-секунды
// system_clock — для метки транзакции (core::makeTxId ждёт именно unix).
std::int64_t steadyTicks() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t unixNow() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::chrono::milliseconds sinceTick(std::int64_t tick) noexcept {
    const std::int64_t delta = steadyTicks() - tick;
    if (delta <= 0) return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds{delta});
}

// ---------------------------------------------------------------------------
// Переводы путей
// ---------------------------------------------------------------------------
//
// Модель, журнал и отчёт — UTF-8 (§6.3), файловые вызовы — UTF-16. Перевод в
// одном месте, чтобы «переведу и забуду проверить» не случилось ни разу.
std::wstring wide(std::string_view utf8) {
    return platform::toUtf16(utf8);
}

// ---------------------------------------------------------------------------
// Словари исходов
// ---------------------------------------------------------------------------
//
// Итог операции платформы в итог строки чек-листа. Отдельные функции, а не
// сравнение перечислений: платформа вправе добавить статус, и тогда «новое
// значение молча стало успехом» окажется в отчёте как выполненная операция.
// Неизвестное значение трактуется как отказ, а не как успех.

// Итог удаления (platform::vfs) в строку чек-листа.
ItemOutcome outcomeOf(vfs::DeleteStatus status) noexcept {
    switch (status) {
        case vfs::DeleteStatus::Deleted:
            return ItemOutcome::Done;
        case vfs::DeleteStatus::AlreadyGone:
            return ItemOutcome::AlreadyGone;
        case vfs::DeleteStatus::SkippedBusy:
            return ItemOutcome::SkippedBusy;
        case vfs::DeleteStatus::SkippedProtected:
            return ItemOutcome::SkippedProtected;
        case vfs::DeleteStatus::SkippedOutsideRoot:
            return ItemOutcome::SkippedOutsideRoot;
        case vfs::DeleteStatus::SkippedReparse:
            return ItemOutcome::SkippedReparse;
        case vfs::DeleteStatus::SkippedChanged:
            return ItemOutcome::SkippedChanged;
        case vfs::DeleteStatus::SkippedCancelled:
            return ItemOutcome::Cancelled;
        case vfs::DeleteStatus::SkippedInvalid:
            return ItemOutcome::SkippedInvalid;
        case vfs::DeleteStatus::Failed:
            break;
    }
    return ItemOutcome::Failed;
}

// Вердикт проверки пути движка (platform::vfs_paths) в строку чек-листа. Inside
// сюда не попадает: вызывающий проверяет вердикт отдельно.
ItemOutcome outcomeOf(vfs_paths::Verdict verdict) noexcept {
    switch (verdict) {
        case vfs_paths::Verdict::Inside:
            return ItemOutcome::Done;
        case vfs_paths::Verdict::OutsideRoot:
            return ItemOutcome::SkippedOutsideRoot;
        case vfs_paths::Verdict::ReparsePoint:
            return ItemOutcome::SkippedReparse;
        case vfs_paths::Verdict::ProtectedPath:
            return ItemOutcome::SkippedProtected;
        case vfs_paths::Verdict::Unresolvable:
            break;
    }
    return ItemOutcome::SkippedInvalid;
}

// Итог переноса в корзину (platform) в строку чек-листа.
ItemOutcome outcomeOf(platform::TrashStatus status) noexcept {
    switch (status) {
        case platform::TrashStatus::Ok:
            return ItemOutcome::Done;
        case platform::TrashStatus::NotFound:
            return ItemOutcome::AlreadyGone;
        case platform::TrashStatus::Cancelled:
            return ItemOutcome::Cancelled;
        case platform::TrashStatus::AccessDenied:
        case platform::TrashStatus::InvalidArgument:
        case platform::TrashStatus::Unsupported:
        case platform::TrashStatus::Corrupt:
        case platform::TrashStatus::IoError:
        case platform::TrashStatus::OutOfSpace:
        case platform::TrashStatus::OutOfMemory:
        case platform::TrashStatus::AlreadyExists:
            break;
    }
    return ItemOutcome::Failed;
}

// Причина пропуска одной строкой: у каждого статуса своя формулировка, потому
// что «молчаливый» пропуск в отчёте читался бы как «мы ничего не проверяли».
const char* skipDetail(ItemOutcome outcome) noexcept {
    switch (outcome) {
        case ItemOutcome::NotStarted:
            return "операция не начата: прогона коснулась отмена";
        case ItemOutcome::NotSelected:
            return "план не выбрал этот кандидат";
        case ItemOutcome::SkippedBusy:
            return "путь держит процесс (Skip (locked))";
        case ItemOutcome::SkippedLockUnknown:
            return "Restart Manager не ответил: удалять нельзя, пока неизвестно, держит ли путь процесс";
        case ItemOutcome::SkippedProtected:
            return "путь в защищённом каталоге (белый список FR-6)";
        case ItemOutcome::SkippedOutsideRoot:
            return "путь вне корня правила";
        case ItemOutcome::SkippedReparse:
            return "symlink/junction: FR-6 не раскрываем";
        case ItemOutcome::SkippedChanged:
            return "объект изменился после сканирования";
        case ItemOutcome::SkippedInvalid:
            return "путь непригоден как цель операции";
        case ItemOutcome::Cancelled:
            return "операция прервана отменой";
        default:
            break;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Поля журнала
// ---------------------------------------------------------------------------
//
// Списки полей собираются явно, а не макросом MRP_LOG_*: макрос разворачивает
// пакет в один вызов logField, и на двух-трёх полях это лишняя хрупкость ради
// экономии двух строк.

core::LogFields pathField(std::string_view path) {
    core::LogFields fields;
    fields.push_back(core::logField("path", std::string{path}));
    return fields;
}

core::LogFields addField(core::LogFields fields, std::string key, std::uint64_t value) {
    fields.push_back(core::logField(std::move(key), value));
    return fields;
}

std::string exceptionText(const std::exception& error) {
    return error.what() != nullptr ? std::string{error.what()} : std::string{"исключение без описания"};
}

// Снимок состояния из строки отчёта обратно в тип платформы. Отдельная функция
// по той же причине, что и словари: в заголовке движка платформенных типов
// нет, а удалению нужен именно FileStamp.
std::optional<vfs::FileStamp> stampOf(const ItemReport& item) {
    if (!item.stampKnown) return std::nullopt;
    vfs::FileStamp stamp;
    stamp.sizeBytes = item.stampSize;
    stamp.lastWriteTicks = item.stampWriteTicks;
    stamp.changeTicks = item.stampChangeTicks;
    return stamp;
}

// Код Win32 в поле hr строки отчёта. ItemReport::hr — это HRESULT (§12: «все
// ошибки в логе с путём и HRESULT»), а часть платформенных модулей отдаёт отказ
// сырым кодом Win32. Смешивать два соглашения в одном поле нельзя: разбирающий
// отчёт по HRESULT не отличил бы «5» (access denied) от 0x80070005, а журнал
// отдал бы это в системный текст по разным правилам.
std::int32_t hresultOrZero(std::uint32_t win32Code) noexcept {
    return static_cast<std::int32_t>(platform::hresultFromWin32(win32Code));
}

}  // namespace

// ---------------------------------------------------------------------------
// Словари статусов
// ---------------------------------------------------------------------------

const char* toString(ItemOutcome outcome) noexcept {
    switch (outcome) {
        case ItemOutcome::NotStarted:
            return "not-started";
        case ItemOutcome::NotSelected:
            return "not-selected";
        case ItemOutcome::Done:
            return "done";
        case ItemOutcome::AlreadyGone:
            return "already-gone";
        case ItemOutcome::SkippedBusy:
            return "skipped-busy";
        case ItemOutcome::SkippedLockUnknown:
            return "skipped-lock-unknown";
        case ItemOutcome::SkippedProtected:
            return "skipped-protected";
        case ItemOutcome::SkippedOutsideRoot:
            return "skipped-outside-root";
        case ItemOutcome::SkippedReparse:
            return "skipped-reparse";
        case ItemOutcome::SkippedChanged:
            return "skipped-changed";
        case ItemOutcome::SkippedInvalid:
            return "skipped-invalid";
        case ItemOutcome::Cancelled:
            return "cancelled";
        case ItemOutcome::Failed:
            break;
    }
    return "failed";
}

bool isSuccess(ItemOutcome outcome) noexcept {
    return outcome == ItemOutcome::Done || outcome == ItemOutcome::AlreadyGone;
}

bool isSkipped(ItemOutcome outcome) noexcept {
    switch (outcome) {
        case ItemOutcome::NotStarted:
        case ItemOutcome::NotSelected:
        case ItemOutcome::SkippedBusy:
        case ItemOutcome::SkippedLockUnknown:
        case ItemOutcome::SkippedProtected:
        case ItemOutcome::SkippedOutsideRoot:
        case ItemOutcome::SkippedReparse:
        case ItemOutcome::SkippedChanged:
        case ItemOutcome::SkippedInvalid:
            return true;
        default:
            break;
    }
    return false;
}

bool needsReboot(ItemOutcome outcome) noexcept {
    return outcome == ItemOutcome::SkippedBusy;
}

const char* toString(ExecutionRefusal refusal) noexcept {
    switch (refusal) {
        case ExecutionRefusal::Completed:
            return "completed";
        case ExecutionRefusal::NothingToDo:
            return "nothing-to-do";
        case ExecutionRefusal::DryRun:
            return "dry-run";
        case ExecutionRefusal::AlreadyRunning:
            return "already-running";
        case ExecutionRefusal::BadChecklist:
            return "bad-checklist";
        case ExecutionRefusal::Cancelled:
            break;
    }
    return "cancelled";
}

// ---------------------------------------------------------------------------
// Чек-лист
// ---------------------------------------------------------------------------

bool executesAction(core::PlanAction action) noexcept {
    return action == core::PlanAction::Delete || action == core::PlanAction::Trash;
}

bool Checklist::executes(const ChecklistEntry& entry) const noexcept {
    return executesAction(entry.action);
}

std::size_t Checklist::executable() const noexcept {
    std::size_t count = 0;
    for (const ChecklistEntry& entry : entries) {
        if (executesAction(entry.action)) ++count;
    }
    return count;
}

std::uint64_t Checklist::bytes() const noexcept {
    // Байты берутся из плана, а не из кандидатов: план — это то, что
    // подтвердил пользователь, и именно эти цифры показывает экран «Очистка».
    std::uint64_t total = 0;
    for (const ChecklistEntry& entry : entries) {
        if (executesAction(entry.action)) total += entry.plannedBytes;
    }
    return total;
}

const ChecklistEntry* Checklist::find(std::size_t candidateIndex) const noexcept {
    for (const ChecklistEntry& entry : entries) {
        if (entry.candidateIndex == candidateIndex) return &entry;
    }
    return nullptr;
}

// Позиция первого метасимвола шаблона core::glob. Класс символов «[…]» не
// метасимвол: его содержимое — литералы. Незакрытая «[» — тоже литерал («в имени
// каталога», «C:\a\b[1]\*.tmp»), иначе корень правила срезался бы на два уровня
// выше своего каталога.
std::size_t firstMetachar(std::string_view pattern) noexcept {
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '*' || ch == '?') return i;
        if (ch == '[') {
            const std::size_t close = pattern.find(']', i + 1);
            if (close == std::string_view::npos) return std::string_view::npos;
            i = close;
        }
    }
    return std::string_view::npos;
}

std::string ruleRootOf(std::string_view resolvedLocator) {
    if (resolvedLocator.empty()) return {};
    // Глоб-символы core::glob: *, ?, [. Первый из них обрывает корень: всё
    // после него — уже содержимое правила, а не его каталог.
    const std::size_t cut = firstMetachar(resolvedLocator);
    std::string root;
    if (cut == std::string_view::npos) {
        // Локатор без метасимволов называет ровно один объект, и это не «каталог
        // плюс содержимое», а сам каталог либо сам файл. Раньше последний
        // компонент срезался всегда, и восемь правил с точным локатором получали
        // корень на уровень выше своего каталога: «%LOCALAPPDATA%\npm-cache»
        // давал «%LOCALAPPDATA%», «C:\Windows\WinSxS» — «C:\Windows». Теперь
        // каталог остаётся своим корнем, а файл даёт корень своего родителя.
        // Вопрос «каталог это или файл» решает ФС: один вызов на правило, кэш
        // в buildChecklist. Несуществующий путь считаем файлом — это совпадает
        // с прежним поведением, то есть ничего не ухудшается.
        const std::string exact(resolvedLocator);
        std::string trimmed = exact;
        while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\')) trimmed.pop_back();
        if (trimmed.find('%') != std::string::npos) return {};
        if (platform::isDirectory(trimmed)) {
            root = trimmed;
        } else {
            const std::size_t slash = trimmed.find_last_of("/\\");
            if (slash == std::string_view::npos) return {};
            root = trimmed.substr(0, slash);
        }
    } else {
        const std::string_view head = resolvedLocator.substr(0, cut);
        const std::size_t slash = head.find_last_of("/\\");
        if (slash == std::string_view::npos) return {};
        root = std::string(head.substr(0, slash));
    }
    // Хвостовой разделитель не нужен: isInsideRoot срезает завершающие
    // разделители с обеих сторон, а лишний символ в логе сбивает с толку.
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    // Неразрешённая подстановка («%ПЕРЕМЕННАЯ%\…») или не-дисковый путь —
    // корень вывести нельзя, а пустой корень означает отказ удалять.
    if (root.find('%') != std::string::npos) return {};
    if (root.size() < 3 || root[1] != ':') return {};
    return root;
}

std::string ruleRootOf(const core::Rule& rule) {
    const std::string_view locator =
        rule.resolvedLocator.empty() ? std::string_view{rule.locator} : std::string_view{rule.resolvedLocator};
    return ruleRootOf(locator);
}

std::string trashPayloadName(std::size_t index) {
    std::string payload = "item-";
    const std::string digits = std::to_string(index);
    if (digits.size() < 6) payload.append(6 - digits.size(), '0');
    payload += digits;
    return payload;
}

Checklist buildChecklist(const std::vector<core::CleanupCandidate>& candidates, const core::CleanupPlan& plan,
                         const ChecklistOptions& options) {
    Checklist checklist;
    checklist.entries.reserve(plan.items.size());
    std::size_t sequence = 0;
    // Корень и локатор — свойства правила, а не строки, поэтому считаются один
    // раз на ruleId. Раньше ruleRootOf звался на каждую строку, а проверка
    // «каталог это или файл» добавила бы обращение к ФС на каждую строку плана.
    std::unordered_map<std::string, std::pair<std::string, std::string>> ruleFacts;
    for (const core::CleanupPlanItem& item : plan.items) {
        if (item.candidateIndex >= candidates.size()) continue;  // индекс не согласован — решает вызывающий
        const core::CleanupCandidate& candidate = candidates[item.candidateIndex];
        const bool wanted = executesAction(item.action);
        if (!wanted && !options.includeUntouched) continue;

        ChecklistEntry entry;
        entry.candidateIndex = item.candidateIndex;
        entry.action = item.action;
        entry.plannedBytes = item.reclaimBytes != 0 ? item.reclaimBytes : candidate.allocatedBytes;
        if (wanted) {
            if (!options.ruleRootOverride.empty()) {
                entry.ruleRoot = options.ruleRootOverride;
            } else if (options.rules != nullptr) {
                if (const core::Rule* rule = options.rules->byId(candidate.ruleId)) {
                    const auto [it, inserted] = ruleFacts.try_emplace(candidate.ruleId);
                    if (inserted) {
                        it->second.first = ruleRootOf(*rule);
                        it->second.second =
                            rule->resolvedLocator.empty() ? rule->locator : rule->resolvedLocator;
                    }
                    entry.ruleRoot = it->second.first;
                    entry.ruleLocator = it->second.second;
                }
            }
            // Пустой корень остаётся пустым намеренно: он означает «удалять
            // нельзя» (SkippedOutsideRoot), и это безопаснее, чем подставить
            // корень тома или корень кэша.
            entry.trashPayload = trashPayloadName(sequence++);
        }
        checklist.entries.push_back(std::move(entry));
    }
    return checklist;
}

// ---------------------------------------------------------------------------
// Отчёт
// ---------------------------------------------------------------------------

std::size_t ExecutionReport::count(ItemOutcome outcome) const noexcept {
    std::size_t found = 0;
    for (const ItemReport& item : items) {
        if (item.outcome == outcome) ++found;
    }
    return found;
}

bool ExecutionReport::complete() const noexcept {
    return !cancelled && failed == 0 && skipped == 0 && errors.empty();
}

bool ExecutionReport::hasProblems() const noexcept {
    return failed != 0 || cancelled != 0 || skipped != 0;
}

std::string ExecutionReport::toText() const {
    std::string out = "очистка #";
    out += std::to_string(generation);
    out += ": операций ";
    out += std::to_string(total);
    out += " (выполнено ";
    out += std::to_string(done);
    out += ", уже отсутствовало ";
    out += std::to_string(alreadyGone);
    out += ", пропущено ";
    out += std::to_string(skipped);
    out += ", из них занято ";
    out += std::to_string(skippedBusy);
    out += ", не выбрано ";
    out += std::to_string(notSelected);
    out += ", с ошибкой ";
    out += std::to_string(failed);
    out += ", отменено ";
    out += std::to_string(cancelledItems);
    out += "), освобождено ";
    out += core::formatBytes(reclaimedBytes);
    out += " из ";
    out += core::formatBytes(plannedBytes);
    out += ", ошибок ";
    out += std::to_string(errors.size());
    if (errorsDropped != 0) {
        out += " (ещё ";
        out += std::to_string(errorsDropped);
        out += " не показано)";
    }
    if (!trashTxId.empty()) {
        out += ", корзина ";
        out += trashTxId;
    }
    if (requiresReboot != 0) {
        out += ", нужна перезагрузка для ";
        out += std::to_string(requiresReboot);
        out += " путей";
    }
    out += ", потоков ";
    out += std::to_string(workers);
    if (cancelled) out += ", ПРЕРВАНО";
    return out;
}

// ---------------------------------------------------------------------------
// Состояние прогона
// ---------------------------------------------------------------------------

// Определено здесь, а не в заголовке: журнал корзины и параметры файловых
// операций — чужие слою типы, и тащить их в публичный заголовок движка нельзя
// (SPEC §6.1: engine стоит над platform). Живёт ровно один прогон и только в его
// потоке; отчёт при этом пишется из рабочих потоков, поэтому под мьютексом.
struct CleanupExecutor::RunContext {
    std::mutex errorsMutex;
    std::mutex ledgerMutex;

    core::TrashLedger ledger;
    platform::TrashOptions trashOptions;

    std::string trashRoot;
    std::string trashDir;
    std::string txId;
    // Транзакция заведена и готова принимать элементы. false — либо Trash-строк
    // нет, либо каталог не создался: тогда строки станут отказами с внятной
    // причиной, а не «перенесём и потеряем содержимое без манифеста».
    bool trashReady{};
};

// Закрыть строку чек-листа решением «пропустить» с внятной причиной.
void skipItem(ItemReport& item, ItemOutcome outcome) {
    item.outcome = outcome;
    const char* reason = skipDetail(outcome);
    if (reason != nullptr && *reason != '\0') item.detail = reason;
    item.code = toString(outcome);
}

// ---------------------------------------------------------------------------
// Фаза A: проверки (параллельно)
// ---------------------------------------------------------------------------
//
// Один поток на одну строку чек-листа. Проверки идут строго в порядке FR-6 и
// §10, и каждая следующая дешевле той, что может запретить операцию:
//
//   1. корень правила задан? Нет — SkippedOutsideRoot, даже без обращения к ФС;
//   2. нормализация пути (GetFinalPathNameByHandleW), принадлежность корню
//      правила, белый список защищённых каталогов, признак reparse — всё это
//      одна проверка platform::vfs_paths::checkRuleRoot: нормализация снимает
//      8.3, «проходит» symlink и доказывает, что путь действительно свой;
//   3. тип объекта и снимок состояния (§10: файл не изменился после скана);
//   4. Restart Manager (engine::locks) с повтором с backoff 3× (FR-6).
//
// Пропуск reparse points (FR-6) проверяется дважды: вердиктом vfs_paths на
// кандидате и повторно — платформенным модулем при самом удалении.

void CleanupExecutor::checkItem(const core::CleanupCandidate& candidate, ItemReport& item) {
    const std::stop_token token = runToken_;
    if (item.path.empty()) {
        item.path = candidate.path;
    }
    if (!executesAction(item.action)) {
        skipItem(item, ItemOutcome::NotSelected);
        return;
    }
    if (token.stop_requested()) {
        skipItem(item, ItemOutcome::NotStarted);
        return;
    }
    if (item.ruleRoot.empty()) {
        // FR-6 требует «проверку, что путь внутри ожидаемого корня правила», а
        // корень неизвестен — значит проверить нечего. Подставлять корень тома
        // здесь нельзя: это ровно тот случай, который спека запрещает.
        skipItem(item, ItemOutcome::SkippedOutsideRoot);
        item.detail = "корень правила не задан: FR-6 требует границу, а пустой корень означает отказ";
        return;
    }

    // 2. Нормализация, корень правила, белый список, reparse.
    const vfs_paths::RootGuard root = vfs_paths::prepareRoot(item.ruleRoot);
    const vfs_paths::Guard guard = vfs_paths::checkRuleRoot(item.path, root);
    if (!guard.allowed()) {
        if (!guard.normalized.empty()) item.path = guard.normalized;
        item.hr = hresultOrZero(guard.lastError);
        skipItem(item, outcomeOf(guard.verdict));
        return;
    }
    if (!guard.normalized.empty()) item.path = guard.normalized;
    if (token.stop_requested()) {
        skipItem(item, ItemOutcome::NotStarted);
        return;
    }

    // 3. Тип объекта и снимок состояния. Снимок в отчёт кладём всегда — журнал и
    // разбор «почему пропустили» его хотят, — но в удаление он подставляется
    // только для файлов: снимок каталога означал бы «каталог не менялся»,
    // хотя менялись файлы внутри (vfs_delete обходит дерево без снимка).
    item.isDirectory = platform::isDirectory(item.path);
    if (options_.checkStamps) {
        const std::optional<vfs::FileStamp> stamp = vfs::readFileStamp(wide(item.path));
        if (stamp.has_value()) {
            item.stampKnown = true;
            item.stampSize = stamp->sizeBytes;
            item.stampWriteTicks = stamp->lastWriteTicks;
            item.stampChangeTicks = stamp->changeTicks;
        }
    }

    // 4. Restart Manager. Путь, который RM не смог опросить, удалять нельзя
    // (locks::LockInfo::unknown): «не знаю» и «занято» — разные утверждения, и
    // сводить их к «занято» нельзя, потому что потом это нельзя объяснить.
    if (!options_.checkLocks) return;

    locks::Options query = options_.lockOptions;
    // Журнал на каждый кандидат заполнил бы файл на длинном плане; сводку пишет
    // финализация один раз за прогон.
    query.logFailures = false;
    const locks::LockInfo info = locks::queryPath(item.path, query, token);
    item.lockAttempts = info.attempts > 0 ? info.attempts : 0;
    item.lockHolders = info.holders.size();
    item.lockStatus = locks::toString(info.state);
    item.requiresReboot = info.requiresReboot();

    if (info.cancelled) {
        skipItem(item, ItemOutcome::NotStarted);
        item.detail = "проверка блокировок прервана отменой";
        return;
    }
    if (info.free()) return;

    if (info.unknown()) {
        item.hr = hresultOrZero(info.win32Error);
        item.lockDetail = locks::describe(info);
        skipItem(item, ItemOutcome::SkippedLockUnknown);
        if (!info.statusText.empty()) item.detail = info.statusText;
        return;
    }

    // Занят. Кто именно держит — в отчёт (FR-4), решение — в фазу B.
    for (const locks::Holder& holder : info.holders) {
        core::ProcessRef ref;
        ref.pid = holder.pid;
        ref.name = holder.name;
        item.lockedBy.push_back(std::move(ref));
    }
    item.lockDetail = locks::describe(info);
    item.outcome = ItemOutcome::SkippedBusy;
    item.code = toString(item.outcome);
    item.detail = item.requiresReboot ? "путь держит процесс, и освободить его можно только перезагрузкой"
                                      : "путь держит процесс (Skip (locked))";
}

bool CleanupExecutor::checkPhase(const std::vector<core::CleanupCandidate>& candidates,
                                 std::vector<ItemReport>& items) {
    // Строки, которые план не собирается трогать, получают исход сразу: работать
    // для них нечего, а в пул их пускать незачем.
    std::vector<std::size_t> targets;
    targets.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!executesAction(items[i].action)) {
            skipItem(items[i], ItemOutcome::NotSelected);
            continue;
        }
        targets.push_back(i);
    }
    if (targets.empty()) return true;

    const std::size_t workers = resolveWorkers(targets.size());
    WorkerPool pool(workers, runToken_, &progress_);
    std::size_t submitted = 0;
    bool submitFailed = false;
    for (const std::size_t index : targets) {
        if (runToken_.stop_requested()) break;
        ItemReport& item = items[index];
        const core::CleanupCandidate& candidate = candidates[item.candidateIndex];
        bool queued = false;
        try {
            queued = pool.submit([this, &candidate, &item] {
                // Исключение внутри проверки не фатально (FR-6), но такую строку
                // нельзя оставлять NotStarted: в фазе C этот исход означает
                // «проверена и разрешена», а проверена она не была — путь ушёл бы
                // на удаление без проверки корня, белого списка и Restart Manager.
                // Исход ставится раньше текста: если сборка строки тоже бросит,
                // Failed в отчёте уже есть.
                try {
                    checkItem(candidate, item);
                } catch (const std::exception& error) {
                    item.outcome = ItemOutcome::Failed;
                    item.code = toString(item.outcome);
                    item.detail = std::string{"исключение при проверке: "} + exceptionText(error);
                } catch (...) {
                    item.outcome = ItemOutcome::Failed;
                    item.code = toString(item.outcome);
                    item.detail = "исключение при проверке (неизвестное)";
                }
                progress_.taskFinished();
                emitProgress(false);
            });
        } catch (const std::exception& error) {
            submitFailed = true;
            core::LogFields fields = pathField(item.path);
            fields.push_back(core::logField("what", exceptionText(error)));
            core::logError(kEventJobFailed, "работа фазы проверок не поставлена в пул", std::move(fields));
            break;
        }
        if (!queued) break;
        ++submitted;
    }
    pool.close();

    // Строки, до которых пул не дошёл (отмена или сбой постановки), остаются
    // нетронутыми: NotStarted — это «мы не начали», а не «операция не удалась».
    // Причину пишем явно: в отчёте «мы не успели» и «пользователь нажал Отмена» —
    // разные утверждения, а молчаливое NotStarted читалось бы как «всё прошло».
    for (std::size_t i = submitted; i < targets.size(); ++i) {
        ItemReport& item = items[targets[i]];
        skipItem(item, ItemOutcome::NotStarted);
        if (submitFailed) item.detail = "работа не поставлена в пул: прогон прерван";
    }
    emitProgress(false);
    return !runToken_.stop_requested();
}

// ---------------------------------------------------------------------------
// Фаза B: закрытие приложений (последовательно, в потоке прогона)
// ---------------------------------------------------------------------------
//
// Спрашивать пользователя можно только там, где нет пула: фаза A уже собрала
// все ответы Restart Manager, поэтому здесь мы точно знаем, кого имеет смысл
// закрывать, и не породим восемь диалогов сразу (FR-6: «опция “закрыть эти
// приложения” (RmShutdown) с подтверждением»).

// Одобрить ровно те PID, на которые пользователь ответил «закрыть». Модуль
// platform::shutdown повторно спрашивает подтверждение, и подмена колбэка на
// «одобрить список из ответа пользователя» — это ровно один вопрос на прогон,
// а не вопрос на каждый вызов Win32.
shutdown::ShutdownConfirmation approving(const std::vector<std::uint32_t>& pids) {
    return [pids](const shutdown::ShutdownPreview& preview, std::vector<shutdown::ProcessRef>& approved) {
        approved.clear();
        for (const shutdown::ShutdownEntry& entry : preview.entries) {
            if (!entry.approvable()) continue;
            if (std::find(pids.begin(), pids.end(), entry.process.pid) == pids.end()) continue;
            approved.push_back(entry.process);
        }
        return true;
    };
}

// Итог закрытия одной строкой. Своего describe() у ShutdownSummary нет, а в
// журнал и в reason к Skip (locked) должно попасть всё, что произошло: молча
// записанное «закрыть не удалось» невозможно отличить от «пользователь отказал».
std::string summaryText(const shutdown::ShutdownSummary& summary) {
    std::string out = "закрыто ";
    out += std::to_string(summary.closed);
    out += ", уже завершено ";
    out += std::to_string(summary.alreadyExited);
    out += ", отказ ";
    out += std::to_string(summary.refused);
    out += ", таймаут ";
    out += std::to_string(summary.timedOut);
    out += ", защищено ";
    out += std::to_string(summary.protectedSkipped);
    out += ", не подтверждено ";
    out += std::to_string(summary.notConfirmed);
    out += ", отказов ";
    out += std::to_string(summary.failed);
    if (summary.hr != 0) {
        out += ", HRESULT ";
        out += std::to_string(summary.hr);
    }
    return out;
}

bool CleanupExecutor::closeHolders(ItemReport& item, std::string& note) {
    if (!options_.allowCloseProcesses || !options_.confirmClose) {
        note = "закрытие приложений не запрошено: путь остаётся Skip (locked)";
        return false;
    }
    std::vector<std::uint32_t> pids;
    for (const core::ProcessRef& process : item.lockedBy) {
        if (process.pid != 0) pids.push_back(process.pid);
    }
    if (pids.empty()) {
        note = "закрывать нечего: держатель помечен как критичный, замаскированный или это сам MrProper";
        return false;
    }

    bool approved = false;
    try {
        approved = options_.confirmClose(item.path, pids);
    } catch (const std::exception& error) {
        note = std::string{"диалог закрытия приложений бросил исключение: "} + exceptionText(error);
        return false;
    } catch (...) {
        note = "диалог закрытия приложений бросил неизвестное исключение";
        return false;
    }
    if (!approved) {
        note = "пользователь не согласился закрывать приложения";
        return false;
    }

    std::vector<shutdown::ProcessRef> refs;
    refs.reserve(item.lockedBy.size());
    for (const core::ProcessRef& process : item.lockedBy) {
        shutdown::ProcessRef ref;
        ref.pid = process.pid;
        ref.name = wide(process.name);
        refs.push_back(std::move(ref));
    }

    shutdown::ShutdownOptions shutdownOptions;
    shutdownOptions.confirm = approving(pids);
    shutdownOptions.force = options_.forceClose;
    shutdownOptions.exitWait = options_.exitWait;
    shutdownOptions.resourcePath = wide(item.path);
    // Журналирование ведёт исполнитель: одна строка на кандидата вместо
    // отдельной записи на каждый RmShutdown. Сам модуль platform отказа не
    // скрывает — его итог целиком попадает в note и в отчёт.

    const shutdown::ShutdownPreview preview = shutdown::planShutdown(refs, shutdownOptions);
    if (!preview.needsConfirmation()) {
        note = "предпросмотр не нашёл ни одного процесса, который можно закрыть: " + core::toUtf8(preview.describe());
        return false;
    }
    const shutdown::ShutdownSummary summary = shutdown::closeProcesses(preview, shutdownOptions, runToken_);
    if (summary.closed == 0) {
        note = "ни один процесс не закрылся: " + summaryText(summary);
        return false;
    }
    note = "закрыто процессов: " + std::to_string(summary.closed) + " (" + summaryText(summary) + ")";
    return true;
}

bool CleanupExecutor::recheckLock(std::string_view path, ItemReport& item) {
    locks::Options query = options_.lockOptions;
    query.logFailures = false;
    const locks::LockInfo info = locks::queryPath(path, query, runToken_);
    item.lockAttempts += info.attempts > 0 ? info.attempts : 0;
    item.lockHolders = info.holders.size();
    item.lockStatus = locks::toString(info.state);
    item.lockDetail = locks::describe(info);
    if (info.free()) return true;
    item.hr = hresultOrZero(info.win32Error);
    return false;
}

bool CleanupExecutor::closePhase(std::vector<ItemReport>& items) {
    if (items.empty()) return true;
    bool asked = false;
    for (ItemReport& item : items) {
        if (runToken_.stop_requested()) return false;
        if (item.outcome != ItemOutcome::SkippedBusy) continue;
        // Освободить путь можно только перезагрузкой — закрывать процессы
        // бессмысленно, а вопрос пользователю про процессы, которых нельзя
        // закрыть, вводит в заблуждение.
        if (item.requiresReboot) continue;

        std::string note;
        const bool closed = closeHolders(item, note);
        if (options_.logFailures || closed) {
            core::LogFields fields = addField(pathField(item.path), "closed", closed ? std::uint64_t{1} : std::uint64_t{0});
            if (closed) core::logInfo(kEventCloseAsk, note, std::move(fields));
        }
        if (!closed) {
            item.detail = note;
            continue;
        }
        if (recheckLock(item.path, item)) {
            // Приложения ушли, путь свободен: строка возвращается в работу.
            item.outcome = ItemOutcome::NotStarted;
            item.code = toString(item.outcome);
            item.detail = note;
            item.requiresReboot = false;
            asked = true;
            continue;
        }
        item.outcome = ItemOutcome::SkippedBusy;
        item.code = toString(item.outcome);
        item.detail = note + "; путь всё ещё занят";
    }
    if (asked) emitProgress(true);
    return !runToken_.stop_requested();
}

// ---------------------------------------------------------------------------
// Фаза C: исполнение (параллельно)
// ---------------------------------------------------------------------------

// Соответствует ли путь строки локатору правила, из которого кандидат вырос.
// Пустой локатор — сверять нечем: корень тогда задал вызывающий, и границу
// полностью держит checkRuleRoot. Сравнение регистронезависимое и по разделителям
// — тем же matchPath, что и на скане (src/core/rules.cpp, Rule::matches).
bool locatorStillMatches(const ItemReport& item) {
    if (item.ruleLocator.empty()) return true;
    if (item.path.empty()) return false;
    return core::matchPath(item.ruleLocator, core::normalizeSeparators(item.path));
}

// Прямое удаление: дерево для каталога, один объект для файла. «Повтор с
// backoff 3×» (FR-6) задаётся платформенному модулю, который сам решает, какие
// коды отказа повторяемы (SHARING/LOCK_VIOLATION), а какие нет (ACCESS_DENIED
// на каталоге — это права, а не блокировка).
void CleanupExecutor::deleteItem(ItemReport& item) {
    const std::wstring pathWide = wide(item.path);
    const std::wstring rootWide = wide(item.ruleRoot);

    vfs::DeleteOptions deleteOptions;
    // View живёт до конца вызова: rootWide объявлен выше и не перемещается.
    deleteOptions.allowedRoot = rootWide;
    deleteOptions.maxAttempts = options_.deleteAttempts;
    deleteOptions.backoff = options_.deleteBackoff;
    deleteOptions.maxBackoff = options_.maxDeleteBackoff;

    if (!item.isDirectory) {
        vfs::DeleteRequest request;
        request.path = pathWide;
        request.kind = vfs::DeleteKind::File;
        if (const std::optional<vfs::FileStamp> stamp = stampOf(item)) request.expectedStamp = stamp;
        const vfs::DeleteResult result = vfs::deleteEntry(request, deleteOptions, runToken_);
        item.outcome = outcomeOf(result.status);
        item.hr = result.hr;
        item.attempts = result.attempts;
        item.duration = result.waited;
        item.filesDone = result.status == vfs::DeleteStatus::Deleted ? 1u : 0u;
        item.detail = core::toUtf8(result.detail);
        item.code = toString(item.outcome);
        if (item.detail.empty()) {
            const char* reason = skipDetail(item.outcome);
            if (reason != nullptr) item.detail = reason;
        }
        if (isSuccess(item.outcome)) item.reclaimedBytes = item.plannedBytes;
        return;
    }

    // Каталог: обход снизу вверх делает платформенный модуль, он же не
    // раскрывает reparse points (FR-6) и не роняет прогон на отказе внутри
    // дерева. onEntry нужен ради счётчиков: одна строка чек-листа может
    // означать сто тысяч файлов, и «удалено файлов» в отчёте полезнее, чем
    // ничего.
    vfs::DeleteEntryCallback onEntry = [&item](const vfs::DeleteResult& entry) {
        if (entry.status == vfs::DeleteStatus::Deleted) {
            ++item.filesDone;
            return;
        }
        if (entry.status == vfs::DeleteStatus::Failed) {
            ++item.problems;
            if (item.hr == 0) item.hr = entry.hr;
            if (item.detail.empty()) item.detail = core::toUtf8(entry.detail);
        }
    };

    const std::int64_t startTick = steadyTicks();
    const vfs::TreeDeleteSummary summary = vfs::deleteTree(pathWide, deleteOptions, runToken_, onEntry);
    item.duration = sinceTick(startTick);
    item.filesDone = static_cast<std::uint32_t>(summary.filesDeleted);
    item.dirsDone = static_cast<std::uint32_t>(summary.dirsDeleted);
    item.problems = static_cast<std::uint32_t>(summary.failed);
    item.problemsDropped = static_cast<std::uint32_t>(summary.problemsDropped);

    if (!summary.problems.empty()) {
        const vfs::DeleteResult& first = summary.problems.front();
        if (item.hr == 0) item.hr = first.hr;
        if (item.detail.empty()) item.detail = core::toUtf8(first.detail);
    }

    if (summary.cancelled) {
        item.outcome = ItemOutcome::Cancelled;
    } else if (summary.failed != 0) {
        item.outcome = ItemOutcome::Failed;
    } else if (summary.skipped() != 0) {
        item.outcome = summary.problems.empty() ? ItemOutcome::SkippedInvalid
                                                : outcomeOf(summary.problems.front().status);
    } else if (summary.dirsDeleted != 0 || summary.filesDeleted != 0) {
        item.outcome = ItemOutcome::Done;
    } else {
        item.outcome = ItemOutcome::AlreadyGone;
    }
    item.code = toString(item.outcome);
    if (item.detail.empty()) {
        const char* reason = skipDetail(item.outcome);
        if (reason != nullptr && *reason != '\0') item.detail = reason;
    }
    if (isSuccess(item.outcome)) {
        // Фактически освобождено посчитать нельзя, не открыв каждый файл перед
        // удалением (это удвоило бы ввод-вывод на миллионе файлов), поэтому в
        // отчёт идёт аллоцированный размер кандидата (§4 FR-4) — та же цифра,
        // которую обещал план. Проверка «место освободилось» — по отчёту о
        // свободном месте, а не по этому счётчику.
        item.reclaimedBytes = item.plannedBytes;
    }
}

// Перенос в корзину приложения (FR-7).
void CleanupExecutor::trashItem(ItemReport& item, RunContext& run) {
    if (!run.trashReady || run.txId.empty()) {
        item.outcome = ItemOutcome::Failed;
        item.code = toString(item.outcome);
        item.detail = "корзина недоступна: перенос не выполнен, а прямое удаление не заменяет его (решение о размещении принимает план, FR-7)";
        return;
    }

    // Повторная проверка границы непосредственно перед переносом: в отличие от
    // удаления, у переноса в корзину нет собственной защиты, а между фазой A и
    // фазой C прошло время, за которое путь мог смениться (FR-6, §10).
    const vfs_paths::RootGuard root = vfs_paths::prepareRoot(item.ruleRoot);
    const vfs_paths::Guard guard = vfs_paths::checkRuleRoot(item.path, root);
    if (!guard.allowed()) {
        item.hr = hresultOrZero(guard.lastError);
        skipItem(item, outcomeOf(guard.verdict));
        return;
    }

    platform::TrashStageRequest request;
    request.transactionDir = run.trashDir;
    request.originalPath = item.path;
    request.payload = item.payload;
    // Граница правила уходит в сам слой корзины: обход дерева работает с
    // лексическими путями, и проверка только верхнего элемента оставляла окно,
    // в котором каталог, подменённый на junction, уводил снос за пределы корня
    // (FR-6, §12 «отсутствие изменений вне корней правил»).
    request.allowedRoot = item.ruleRoot;
    const platform::TrashStageResult staged = platform::stageTrashItem(request, run.trashOptions);

    item.txId = run.txId;
    item.hr = hresultOrZero(staged.transfer.win32Error);
    item.attempts = 1;
    item.filesDone = staged.transfer.filesMoved;
    item.outcome = outcomeOf(staged.transfer.status);
    item.code = toString(item.outcome);
    if (staged.transfer.ok()) {
        item.detail = staged.transfer.crossVolume ? "перенесено в корзину приложения (кросс-томовым копированием)"
                                                  : "перенесено в корзину приложения";
        item.reclaimedBytes = staged.transfer.bytesMoved != 0 ? staged.transfer.bytesMoved : item.plannedBytes;
        // Элемент — в журнал транзакции: манифест в финализации увидит всё, что
        // перенесено, и только после этого транзация станет отменяемой (FR-7).
        std::lock_guard lock(run.ledgerMutex);
        run.ledger.addItem(run.txId, staged.item);
    } else {
        item.detail = platform::formatStatus(staged.transfer.status, staged.transfer.win32Error);
        if (!staged.transfer.failedPath.empty()) {
            item.detail += ": ";
            item.detail += staged.transfer.failedPath;
        }
    }
}

bool CleanupExecutor::executePhase(std::vector<ItemReport>& items, RunContext& run) {
    if (items.empty()) return true;
    std::vector<std::size_t> targets;
    targets.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        // NotStarted — единственный исход, означающий «проверен, разрешён, ждёт
        // операции». Остальные строки уже имеют окончательный исход и просто
        // учитываются в прогрессе: иначе они навсегда остались бы «в работе».
        if (items[i].outcome == ItemOutcome::NotStarted && executesAction(items[i].action)) {
            targets.push_back(i);
            continue;
        }
        noteItem(items[i]);
    }
    if (targets.empty()) return !runToken_.stop_requested();

    const std::size_t workers = resolveWorkers(targets.size());
    WorkerPool pool(workers, runToken_, &progress_);
    std::size_t submitted = 0;
    bool submitFailed = false;
    for (const std::size_t index : targets) {
        if (runToken_.stop_requested()) break;
        ItemReport& item = items[index];
        const bool trash = item.action == core::PlanAction::Trash;
        bool queued = false;
        try {
            queued = pool.submit([this, &item, &run, trash] {
                const std::int64_t startTick = steadyTicks();
                try {
                    // Повторная сверка с локатором правила непосредственно перед
                    // операцией (§10: «путь, не совпавший ни с одним известным
                    // правилом, никогда не удаляется»). На скане совпадение уже
                    // было, но между фазой A и фазой C путь мог уйти из-под
                    // правила, а корень — каталог пошире того, что правило
                    // собиралось трогать. Одна проверка шаблона на элемент
                    // дешевле, чем операция, которую придётся откатывать.
                    if (!locatorStillMatches(item)) {
                        skipItem(item, ItemOutcome::SkippedOutsideRoot);
                        item.detail =
                            "путь не соответствует локатору правила: элемент вне того, что правило отбирало";
                    } else if (trash) {
                        trashItem(item, run);
                    } else {
                        deleteItem(item);
                    }
                } catch (const std::exception& error) {
                    // Исключение одной операции не фатально (FR-6): оно стало
                    // отказом этой строки, а не концом прогона.
                    item.outcome = ItemOutcome::Failed;
                    item.code = toString(item.outcome);
                    item.detail = std::string{"исключение при операции: "} + exceptionText(error);
                } catch (...) {
                    item.outcome = ItemOutcome::Failed;
                    item.code = toString(item.outcome);
                    item.detail = "исключение при операции (неизвестное)";
                }
                if (item.duration.count() == 0) item.duration = sinceTick(startTick);
                noteItem(item);
            });
        } catch (const std::exception& error) {
            submitFailed = true;
            core::LogFields fields = pathField(item.path);
            fields.push_back(core::logField("what", exceptionText(error)));
            core::logError(kEventJobFailed, "работа фазы исполнения не поставлена в пул", std::move(fields));
            break;
        }
        if (!queued) break;
        ++submitted;
    }
    pool.close();

    // Не начатые строки: объект цел, это «мы не успели», а не «не смогли».
    // Причина (отмена или сбой постановки) пишется в строку, потому что по
    // одному NotStarted их не различить.
    for (std::size_t i = submitted; i < targets.size(); ++i) {
        ItemReport& item = items[targets[i]];
        skipItem(item, ItemOutcome::NotStarted);
        if (submitFailed) item.detail = "работа не поставлена в пул: прогон прерван";
        noteItem(item);
    }
    emitProgress(false);
    return !runToken_.stop_requested();
}

// ---------------------------------------------------------------------------
// Фаза D: финализация
// ---------------------------------------------------------------------------

void CleanupExecutor::prepareTrash(RunContext& run, const std::vector<ItemReport>& items) {
    const bool needTrash = std::any_of(items.begin(), items.end(), [](const ItemReport& item) {
        return item.action == core::PlanAction::Trash;
    });
    if (!needTrash) return;

    run.trashRoot = options_.trashRoot.empty() ? platform::defaultTrashRoot() : options_.trashRoot;
    if (run.trashRoot.empty()) {
        // ProgramData неизвестен (сервисная сессия). Молча удалять вместо
        // переноса нельзя: FR-7 различает «в корзину» и «навсегда», и решение
        // об этом принимал план, а не исполнитель.
        if (options_.logFailures) {
            core::logError(kEventTrash, "корень корзины неизвестен: операции Trash невозможны");
        }
        return;
    }
    run.ledger.setRoot(run.trashRoot);
    run.txId = run.ledger.begin(unixNow(), options_.appVersion);
    if (!core::isValidTxId(run.txId)) {
        if (options_.logFailures) {
            core::LogFields fields = pathField(run.trashRoot);
            fields.push_back(core::logField("txid", run.txId));
            core::logError(kEventTrash, "идентификатор транзакции непригоден", std::move(fields));
        }
        run.txId.clear();
        return;
    }

    run.trashOptions.stop = runToken_;
    // Журналирование ведёт исполнитель: одна строка на кандидат вместо записи
    // на каждый перемещённый файл внутри транзакции.
    run.trashOptions.logFailures = false;
    run.trashOptions.progress = [this](const platform::TrashProgress& progress) {
        // Кросс-томовый перенос идёт долго (FR-7: «это займёт время»), а строки
        // чек-листа в этот момент не меняются: без этого снимка полоса прогресса
        // стояла бы на месте всё время копирования гигабайтов.
        (void)progress;
        emitProgress(false);
    };

    std::string dir;
    const platform::TrashStatus status = platform::createTransactionDirectory(run.trashRoot, run.txId, dir, run.trashOptions);
    if (status != platform::TrashStatus::Ok) {
        if (options_.logFailures) {
            core::LogFields fields = pathField(run.trashRoot);
            fields.push_back(core::logField("txid", run.txId));
            fields.push_back(core::logField("status", platform::toString(status)));
            core::logError(kEventTrash, "каталог транзакции корзины не создан", std::move(fields));
        }
        run.txId.clear();
        return;
    }
    run.trashDir = dir;
    run.trashReady = true;
    {
        // Идентификатор транзакции читает toText() из журнала, а журнал пишется
        // и из рабочего потока, поэтому запись — под тем же замком, что и
        // публикация отчёта в finalize().
        std::lock_guard lock(mutex_);
        txId_ = run.txId;
    }

    if (options_.logFailures) {
        core::LogFields fields = pathField(dir);
        fields.push_back(core::logField("txid", run.txId));
        core::logInfo(kEventTrash, "транзакция корзины заведена", std::move(fields));
    }
}

void CleanupExecutor::finishTrash(RunContext& run, ExecutionReport& report) {
    if (run.txId.empty()) return;
    const core::TrashTransaction* tx = run.ledger.find(run.txId);
    if (tx == nullptr) return;
    if (tx->itemCount() == 0) {
        // Ничего не перенесли: каталог транзакции без манифеста — мусор в
        // ProgramData, который при следующем запуске читался бы как «битая
        // транзакция».
        const platform::TrashPurgeResult purged = platform::purgeTransactionDirectory(run.trashDir, run.trashOptions);
        if (options_.logFailures && !purged.ok()) {
            core::LogFields fields = pathField(run.trashDir);
            fields.push_back(core::logField("status", platform::toString(purged.status)));
            core::logWarn(kEventManifest, "пустой каталог транзакции не убран", std::move(fields));
        }
        return;
    }

    // Манифест на диске — транзакцию можно отменять (§7.2, FR-7). Отказ здесь
    // означает «элементы перенесены, но отменить их нечем», поэтому это ошибка
    // уровня прогона, а не отдельной строки.
    const platform::TrashStatus status = platform::writeManifest(run.trashDir, *tx, run.trashOptions);
    if (status != platform::TrashStatus::Ok) {
        {
            std::lock_guard lock(run.errorsMutex);
            ExecutionError error;
            error.path = run.trashDir;
            error.code = "trash-manifest";
            error.message = "манифест транзакции не записан: перенесённые элементы нельзя отменить (" +
                            platform::formatStatus(status, 0) + ")";
            if (report.errors.size() < options_.maxReportedErrors) {
                report.errors.push_back(std::move(error));
            } else {
                ++report.errorsDropped;
            }
        }
        if (options_.logFailures) {
            core::LogFields fields = addField(pathField(run.trashDir), "items", tx->itemCount());
            fields.push_back(core::logField("status", platform::toString(status)));
            core::logError(kEventManifest, "манифест транзакции корзины не записан", std::move(fields));
        }
        return;
    }
    run.ledger.commit(run.txId);
    if (options_.logFailures) {
        core::LogFields fields = addField(pathField(run.trashDir), "items", tx->itemCount());
        fields = addField(std::move(fields), "bytes", tx->totalBytes());
        core::logInfo(kEventManifest, "манифест транзакции корзины записан", std::move(fields));
    }
}

void CleanupExecutor::countTotals(const std::vector<ItemReport>& items, ExecutionReport& report) const {
    report.total = items.size();
    for (const ItemReport& item : items) {
        switch (item.outcome) {
            case ItemOutcome::Done:
                ++report.done;
                break;
            case ItemOutcome::AlreadyGone:
                ++report.alreadyGone;
                break;
            case ItemOutcome::NotSelected:
                ++report.notSelected;
                break;
            case ItemOutcome::Failed:
                ++report.failed;
                break;
            case ItemOutcome::Cancelled:
            case ItemOutcome::NotStarted:
                ++report.cancelledItems;
                break;
            default:
                ++report.skipped;
                break;
        }
        if (item.outcome == ItemOutcome::SkippedBusy) ++report.skippedBusy;
        if (item.requiresReboot) ++report.requiresReboot;
        if (executesAction(item.action)) report.plannedBytes += item.plannedBytes;
        report.reclaimedBytes += item.reclaimedBytes;
    }
}

void CleanupExecutor::finalize(std::vector<ItemReport>& items, ExecutionReport& report, RunContext& run) {
    // Окончательные исходы всем строкам уже проставлены до вызова (execute,
    // шаг со NotStarted до сбора отказов), поэтому здесь только корзина,
    // счётчики и публикация.
    finishTrash(run, report);
    countTotals(items, report);
    report.items = std::move(items);
    report.trashTxId = run.txId;

    progress_.markFinished(report.cancelled);
    emitProgress(true);

    {
        std::lock_guard lock(mutex_);
        result_ = std::make_shared<const ExecutionReport>(report);
        running_ = false;
    }
    doneCv_.notify_all();

    if (options_.logProgress) {
        core::LogFields fields;
        fields = addField(std::move(fields), "generation", report.generation);
        fields = addField(std::move(fields), "total", report.total);
        fields = addField(std::move(fields), "done", report.done);
        fields = addField(std::move(fields), "skipped", report.skipped);
        fields = addField(std::move(fields), "failed", report.failed);
        fields = addField(std::move(fields), "cancelled", report.cancelledItems);
        fields = addField(std::move(fields), "runCancelled", report.cancelled);
        fields = addField(std::move(fields), "reclaimed", report.reclaimedBytes);
        if (report.cancelled || report.failed != 0) {
            core::logWarn(kEventFinish, report.toText(), std::move(fields));
        } else {
            core::logInfo(kEventFinish, report.toText(), std::move(fields));
        }
    }
}

// ---------------------------------------------------------------------------
// Класс: прогресс, журнал, вспомогательное
// ---------------------------------------------------------------------------

std::size_t CleanupExecutor::resolveWorkers(std::size_t operations) const noexcept {
    const std::size_t cap = options_.maxWorkers != 0 ? options_.maxWorkers : kDefaultMaxExecutorWorkers;
    if (options_.workers != 0) return std::min(options_.workers, std::max<std::size_t>(cap, 1));
    if (operations == 0) return 1;
    // Одна операция — один поток: двумя потоками дерево на 500 тысяч файлов не
    // станет быстрее, а память вырастет вдвое.
    const std::size_t cap1 = std::max<std::size_t>(cap, 1);
    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t cores = hardware != 0 ? static_cast<std::size_t>(hardware) : 2u;
    return std::max<std::size_t>(1, std::min({operations, cores, cap1}));
}

void CleanupExecutor::logStart(const Checklist& checklist, std::size_t workers) const {
    if (!options_.logProgress) return;
    core::LogFields fields;
    fields = addField(std::move(fields), "operations", checklist.executable());
    fields = addField(std::move(fields), "rows", checklist.size());
    fields = addField(std::move(fields), "workers", workers);
    fields = addField(std::move(fields), "bytes", checklist.bytes());
    fields.push_back(core::logField("locks", options_.checkLocks));
    fields.push_back(core::logField("closeApps", options_.allowCloseProcesses));
    core::logInfo(kEventStart, "очистка запущена", std::move(fields));
}

void CleanupExecutor::logRefusal(ExecutionRefusal refusal, const Checklist& checklist) const {
    if (!options_.logProgress) return;
    core::LogFields fields;
    fields = addField(std::move(fields), "rows", checklist.size());
    fields = addField(std::move(fields), "operations", checklist.executable());
    fields.push_back(core::logField("refusal", std::string{toString(refusal)}));
    core::logWarn(kEventRefused, "очистка не начата", std::move(fields));
}

void CleanupExecutor::noteItem(const ItemReport& item) {
    progress_.addItems(1);
    if (item.reclaimedBytes != 0) progress_.addBytes(item.reclaimedBytes);
    emitProgress(false);

    if (options_.emitItemCallback && options_.onItem) {
        const auto callback = options_.onItem;
        try {
            callback(item);
        } catch (const std::exception& error) {
            core::LogFields fields = pathField(item.path);
            fields.push_back(core::logField("what", exceptionText(error)));
            core::logError(kEventJobFailed, "колбэк чек-листа бросил исключение", std::move(fields));
        } catch (...) {
            core::logError(kEventJobFailed, "колбэк чек-листа бросил неизвестное исключение");
        }
    }

    if (!options_.logFailures) return;
    if (item.outcome == ItemOutcome::Failed || item.outcome == ItemOutcome::Cancelled) {
        core::LogFields fields = pathField(item.path);
        fields = addField(std::move(fields), "action", static_cast<std::uint64_t>(item.action));
        fields = addField(std::move(fields), "attempts", item.attempts);
        fields = addField(std::move(fields), "problems", item.problems);
        fields.push_back(core::logField("detail", item.detail));
        core::logFailure(kEventItemFailed, "операция очистки не выполнена", item.path,
                         static_cast<std::int64_t>(item.hr), std::move(fields));
        return;
    }
    // Skip (locked) — не ошибка, но и не «сделано»: без записи в журнал
    // «почему файл уцелел» пришлось бы искать по экрану.
    if (item.outcome == ItemOutcome::SkippedBusy || item.outcome == ItemOutcome::SkippedLockUnknown) {
        core::LogFields fields = addField(pathField(item.path), "holders", item.lockHolders);
        fields = addField(std::move(fields), "attempts", static_cast<std::uint64_t>(item.lockAttempts));
        fields.push_back(core::logField("detail", item.detail));
        if (!item.lockDetail.empty()) fields.push_back(core::logField("locks", item.lockDetail));
        core::logWarn(kEventSkip, item.detail, std::move(fields));
    }
}

void CleanupExecutor::emitProgress(bool force) {
    const auto callback = options_.onProgress;
    if (!callback) return;
    const std::int64_t now = steadyTicks();
    if (!force) {
        const std::int64_t interval =
            std::chrono::duration_cast<std::chrono::nanoseconds>(options_.progressInterval).count();
        const std::int64_t last = lastProgressTicks_.load(std::memory_order_relaxed);
        if (last != 0 && now - last < interval) return;
    }
    lastProgressTicks_.store(now, std::memory_order_relaxed);
    const ProgressSnapshot snapshot = progress_.snapshot();

    const auto dispatcher = options_.dispatcher;
    if (!dispatcher) {
        try {
            callback(snapshot);
        } catch (const std::exception& error) {
            core::LogFields fields;
            fields.push_back(core::logField("what", exceptionText(error)));
            core::logError(kEventJobFailed, "колбэк прогресса бросил исключение", std::move(fields));
        } catch (...) {
            core::logError(kEventJobFailed, "колбэк прогресса бросил неизвестное исключение");
        }
        return;
    }
    // Снимок и колбэк копируются по значению: отложенный вызов не должен
    // держать указатель на исполнитель, который к тому времени может умереть.
    try {
        dispatcher([callback, snapshot] {
            try {
                callback(snapshot);
            } catch (...) {  // NOLINT(bugprone-empty-catch) — UI не имеет права ронять очистку
            }
        });
    } catch (const std::exception& error) {
        core::LogFields fields;
        fields.push_back(core::logField("what", exceptionText(error)));
        core::logError(kEventJobFailed, "диспетчер колбэка прогресса бросил исключение", std::move(fields));
    } catch (...) {
        core::logError(kEventJobFailed, "диспетчер колбэка прогресса бросил неизвестное исключение");
    }
}

void CleanupExecutor::addError(ExecutionReport& report, const ItemReport& item, RunContext& run) {
    if (item.outcome != ItemOutcome::Failed && item.outcome != ItemOutcome::Cancelled) return;
    std::lock_guard lock(run.errorsMutex);
    if (report.errors.size() >= options_.maxReportedErrors) {
        ++report.errorsDropped;
        return;
    }
    ExecutionError error;
    error.candidateIndex = item.candidateIndex;
    error.path = item.path;
    error.hr = item.hr;
    error.code = item.code;
    error.message = item.detail;
    report.errors.push_back(std::move(error));
}

// ---------------------------------------------------------------------------
// Класс: прогон
// ---------------------------------------------------------------------------

ExecutionRefusal CleanupExecutor::beginRun(const Checklist& checklist) {
    if (options_.dryRun) {
        // FR-5: «Dry-run обязателен и запускается по умолчанию перед первым
        // удалением в сессии». Обойти показ плана через исполнитель нельзя,
        // поэтому это отказ, а не «выполнить, но ничего не удалять».
        logRefusal(ExecutionRefusal::DryRun, checklist);
        return ExecutionRefusal::DryRun;
    }
    if (checklist.empty() || checklist.executable() == 0) {
        logRefusal(ExecutionRefusal::NothingToDo, checklist);
        return ExecutionRefusal::NothingToDo;
    }
    std::lock_guard lock(mutex_);
    if (running_) {
        logRefusal(ExecutionRefusal::AlreadyRunning, checklist);
        return ExecutionRefusal::AlreadyRunning;
    }
    // Новый прогон — новое состояние отмены: иначе прогон, запущенный после
    // отменённого, начнёт с уже запрошенной остановки.
    stop_ = std::stop_source{};
    runToken_ = stop_.get_token();
    result_.reset();
    ++generation_;
    running_ = true;
    return ExecutionRefusal::Completed;
}

ExecutionRefusal CleanupExecutor::execute(const std::vector<core::CleanupCandidate>& candidates,
                                          const core::CleanupPlan& plan, const Checklist& checklist) {
    // Согласованность чек-листа с кандидатами: строка без кандидата означала бы
    // чтение за границей вектора, а «молча пропустить» — потерю операции без
    // следа в отчёте.
    for (const ChecklistEntry& entry : checklist.entries) {
        if (entry.candidateIndex >= candidates.size()) {
            logRefusal(ExecutionRefusal::BadChecklist, checklist);
            return ExecutionRefusal::BadChecklist;
        }
    }
    // Чек-лист — производная от плана, а не самостоятельное решение. Разошлись
    // (другая сборка, устаревшая копия, ручная правка) — то подтверждённый
    // dry-run (FR-5) и то, что сейчас пойдёт в работу, это разные списки, а
    // выбрать «правильный» исполнитель не в его силах. Сверка двумя
    // указателями за O(n + m): оба списка идут в порядке кандидатов (контракт
    // core::CleanupPlan, по которому их строит buildChecklist), а квадрат на
    // плане в 500 тысяч строк — это минуты ожидания перед первым удалением.
    std::size_t planAt = 0;
    for (const ChecklistEntry& entry : checklist.entries) {
        while (planAt < plan.items.size() && plan.items[planAt].candidateIndex < entry.candidateIndex) ++planAt;
        if (planAt >= plan.items.size() || plan.items[planAt].candidateIndex != entry.candidateIndex ||
            plan.items[planAt].action != entry.action) {
            logRefusal(ExecutionRefusal::BadChecklist, checklist);
            return ExecutionRefusal::BadChecklist;
        }
    }
    if (runToken_.stop_requested()) {
        logRefusal(ExecutionRefusal::Cancelled, checklist);
        return ExecutionRefusal::Cancelled;
    }

    RunContext run;
    const std::size_t workers = resolveWorkers(checklist.executable());
    lastProgressTicks_.store(0, std::memory_order_relaxed);
    progress_.begin();
    progress_.setItemsTotal(checklist.size());
    progress_.setTasksTotal(checklist.size());
    progress_.setWorkersTotal(workers);
    logStart(checklist, workers);

    std::vector<ItemReport> items;
    items.reserve(checklist.entries.size());
    for (const ChecklistEntry& entry : checklist.entries) {
        items.push_back(makeItem(candidates[entry.candidateIndex], entry));
    }

    ExecutionReport report;
    report.generation = generation_;
    report.workers = workers;

    prepareTrash(run, items);

    const std::int64_t startTick = steadyTicks();
    bool aborted = false;
    // executePhase отмечает каждую строку ровно один раз (noteItem) — по мере
    // того, как строки получают окончательный исход: полоса прогресса идёт
    // вместе с удалением, а не прыгает в 100 % в конце. Если до неё не дошли
    // (отмена, сбой постановки работы, исключение), строки не отмечены вовсе:
    // счётчик прогресса застыл бы на нуле, а журнал не получил бы ни одной
    // записи о прерванном прогоне. Отметить их обязан execute — у него есть все
    // строки и их окончательные исходы.
    bool itemsNoted = false;
    try {
        if (!checkPhase(candidates, items)) aborted = true;
        if (!aborted && !closePhase(items)) aborted = true;
        if (!aborted) {
            executePhase(items, run);
            itemsNoted = true;
        }
    } catch (const std::exception& error) {
        // Прогон целиком не состоялся (например, память кончилась в середине
        // постановки). Отчёт всё равно публикуется: частично выполненная
        // очистка обязана быть видна, иначе пользователь не поймёт, что
        // произошло с его диском.
        core::LogFields fields;
        fields.push_back(core::logField("what", exceptionText(error)));
        fields = addField(std::move(fields), "done", report.done);
        core::logError(kEventAborted, "прогон очистки прерван исключением, отчёт публикуется неполным",
                       std::move(fields));
        aborted = true;
    } catch (...) {
        core::logError(kEventAborted, "прогон очистки прерван неизвестным исключением, отчёт публикуется неполным");
        aborted = true;
    }
    report.cancelled = aborted || runToken_.stop_requested();
    report.duration = sinceTick(startTick);

    // Строки, оставшиеся не начатыми, получают окончательный исход ДО сбора
    // отказов: «пользователь нажал Отмена» и «пул не принял работу» — разные
    // утверждения, и молчаливое NotStarted в отчёте выглядело бы как «всё
    // прошло». Причину, набранную в фазе, сохраняем: она точнее общего текста.
    for (ItemReport& item : items) {
        if (item.outcome != ItemOutcome::NotStarted) continue;
        const std::string reason = item.detail;
        if (report.cancelled) {
            item.outcome = ItemOutcome::Cancelled;
            item.detail = reason.empty() ? std::string{"отмена до начала операции"} : reason;
        } else {
            // Отмены не было, а строка всё равно не начата: прогон прерван
            // сбоем (например, память кончилась на постановке работы). Это
            // отказ прогона, а не «пользователь отменил».
            item.outcome = ItemOutcome::Failed;
            item.detail =
                reason.empty() ? std::string{"операция не начата: прогон прерван до постановки работы"} : reason;
        }
        item.code = toString(item.outcome);
    }

    // Прерванный прогон: строки всё равно уходят в журнал и в счётчики, иначе
    // «очистку отменили» и «очистка ничего не тронула» выглядели бы одинаково.
    // Повторно отмеченные строки (исключение ровно посередине executePhase)
    // дадут лишь перебор itemsDone, который fraction() всё равно срезает в 1.0.
    if (!itemsNoted) {
        for (const ItemReport& item : items) noteItem(item);
    }

    // Отказы в отчёт: часть строк может остаться не начатой, и их «мы не
    // успели» тоже должно быть видно — это делает шаг выше.
    for (const ItemReport& item : items) {
        addError(report, item, run);
    }
    finalize(items, report, run);
    return ExecutionRefusal::Completed;
}

ItemReport CleanupExecutor::makeItem(const core::CleanupCandidate& candidate, const ChecklistEntry& entry) const {
    ItemReport item;
    item.candidateIndex = entry.candidateIndex;
    item.action = entry.action;
    item.path = candidate.path;
    item.displayName = candidate.displayName;
    item.category = candidate.category;
    item.plannedBytes = entry.plannedBytes;
    item.ruleRoot = entry.ruleRoot;
    item.ruleLocator = entry.ruleLocator;
    item.payload = entry.trashPayload;
    item.outcome = ItemOutcome::NotStarted;
    item.code = toString(item.outcome);
    return item;
}

ExecutionRefusal CleanupExecutor::start(const std::vector<core::CleanupCandidate>& candidates,
                                        const core::CleanupPlan& plan, const Checklist& checklist) {
    const ExecutionRefusal refusal = beginRun(checklist);
    if (refusal != ExecutionRefusal::Completed) return refusal;
    // Снимок входа: execute() работает в отдельном потоке, а вызывающий после
    // start() вправе освободить свои векторы.
    runCandidates_ = candidates;
    runPlan_ = plan;
    runChecklist_ = checklist;
    {
        // Прежний поток, если его не дождались, присоединяем ДО присваивания
        // нового: присваивание в joinable-поток вызывает std::terminate(), то
        // есть второй клик «Очистить» после завершившегося фонового прогона
        // убивал процесс (контракт класса на это прямо разрешает: generation_
        // растёт, повторный запуск отработанного прогона — не «уже идёт»).
        // Мьютекс на время ожидания отпускаем: прежний поток в finalize() берёт
        // его сам, и join() под мьютексом — это взаимоблокировка.
        std::unique_lock lock(mutex_);
        if (runner_.joinable()) {
            std::jthread previous = std::move(runner_);
            lock.unlock();
            previous.join();
            lock.lock();
            // Пока мьютекс был отпущен, прежний поток мог дописать отчёт и
            // снять running_. Свой прогон он завершить не мог: finalize()
            // отработал до beginRun (иначе running_ было бы true и start()
            // получил бы AlreadyRunning), а чужой прогон тоже не начался бы.
            // Значение возвращаем своё — по контракту это делает beginRun.
            running_ = true;
        }
    }
    try {
        runner_ = std::jthread([this] { runBackground(); });
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        doneCv_.notify_all();
        core::LogFields fields;
        fields.push_back(core::logField("what", exceptionText(error)));
        core::logError(kEventAborted, "фоновый поток очистки не создан", std::move(fields));
        return ExecutionRefusal::AlreadyRunning;
    }
    return ExecutionRefusal::Completed;
}

ExecutionRefusal CleanupExecutor::start(const std::vector<core::CleanupCandidate>& candidates,
                                        const core::CleanupPlan& plan) {
    return start(candidates, plan, buildChecklist(candidates, plan, options_.checklist));
}

void CleanupExecutor::runBackground() {
    try {
        execute(runCandidates_, runPlan_, runChecklist_);
    } catch (...) {  // execute() не бросает; страховка нужна, чтобы из потока
        // прогона наружу не ушло исключение (это std::terminate) и чтобы
        // running() не застрял в true навсегда.
        core::logError(kEventAborted, "фоновый прогон очистки завершился исключением вне execute()");
        std::lock_guard lock(mutex_);
        running_ = false;
        doneCv_.notify_all();
    }
}

ExecutionRefusal CleanupExecutor::run(const std::vector<core::CleanupCandidate>& candidates,
                                      const core::CleanupPlan& plan, const Checklist& checklist) {
    const ExecutionRefusal refusal = beginRun(checklist);
    if (refusal != ExecutionRefusal::Completed) return refusal;
    try {
        return execute(candidates, plan, checklist);
    } catch (...) {  // execute() не бросает, но прогон не должен уйти из-под
        // вызывающего с непойманным исключением.
        core::logError(kEventAborted, "синхронный прогон очистки завершился исключением вне execute()");
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        doneCv_.notify_all();
        return ExecutionRefusal::AlreadyRunning;
    }
}

ExecutionRefusal CleanupExecutor::run(const std::vector<core::CleanupCandidate>& candidates,
                                      const core::CleanupPlan& plan) {
    return run(candidates, plan, buildChecklist(candidates, plan, options_.checklist));
}

bool CleanupExecutor::wait() {
    std::unique_lock lock(mutex_);
    doneCv_.wait(lock, [this] { return !running_; });
    return true;
}

bool CleanupExecutor::waitFor(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return doneCv_.wait_for(lock, timeout, [this] { return !running_; });
}

void CleanupExecutor::requestStop() noexcept {
    stop_.request_stop();
    // Разбудить ожидание: кто-то может ждать конца прогона и без кнопки
    // «Отмена» (например, разбирает отчёт по таймауту).
    doneCv_.notify_all();
}

bool CleanupExecutor::running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_;
}

ProgressSnapshot CleanupExecutor::progress() const noexcept {
    return progress_.snapshot();
}

ExecutionReportPtr CleanupExecutor::result() const noexcept {
    std::lock_guard lock(mutex_);
    return result_;
}

std::uint64_t CleanupExecutor::generation() const noexcept {
    std::lock_guard lock(mutex_);
    return generation_;
}

std::stop_token CleanupExecutor::token() const noexcept {
    std::lock_guard lock(mutex_);
    return runToken_;
}

std::string CleanupExecutor::toText() const {
    std::string out = "исполнитель очистки: ";
    out += running() ? "прогон идёт" : "простой";
    out += ", прогонов ";
    out += std::to_string(generation());
    // Отчёт и идентификатор транзакции читаются под тем же замком, под каким
    // публикуются: toText() зовут и из рабочего потока (запись в журнал), где
    // гонка с finalize() — это чтение shared_ptr на полпути пересчёта счётчиков.
    std::string txId;
    ExecutionReportPtr report;
    {
        std::lock_guard lock(mutex_);
        txId = txId_;
        report = result_;
    }
    if (!txId.empty()) {
        out += ", последняя транзакция корзины ";
        out += txId;
    }
    if (report) {
        out += "; ";
        out += report->toText();
    }
    return out;
}

CleanupExecutor::CleanupExecutor(CleanupExecutorOptions options) : options_(std::move(options)) {}

CleanupExecutor::~CleanupExecutor() {
    stop_.request_stop();
    doneCv_.notify_all();
    if (runner_.joinable()) runner_.join();
}

}  // namespace mrproper::engine
