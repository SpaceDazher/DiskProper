// Отмена очистки: решение (что спросить), перепроверка цели перед переносом и
// приведение манифеста в соответствие с фактом. Обоснование, словарь и правила
// модуля — в undo_service.hpp; здесь только исполнение.
//
// Спека: §4 FR-7, §4 FR-6 (блокировки, «ошибки не фатальны»), §7.2 (Ctrl+Z пока
// транзакция не схлопнулась), §6.4 (отмена `std::stop_token`, проверка раз в 256
// элементов), §5 (устойчивость, никаких прав на старте), §12 (ошибки в логе с
// путём и HRESULT).

#include "undo_service.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "core/log.hpp"
#include "core/units.hpp"

namespace mrproper::engine::undo_service {

namespace {

// Имена событий журнала — стабильные, не локализуются (SPEC §8, §12).
constexpr const char* kLogSnapshot = "undo.snapshot";
constexpr const char* kLogPlan = "undo.plan";
constexpr const char* kLogRestore = "undo.restore";
constexpr const char* kLogItem = "undo.item";
constexpr const char* kLogDenied = "undo.restore.denied";
constexpr const char* kLogManifest = "undo.manifest";
constexpr const char* kLogPurge = "undo.purge";

// Список полей журнала собирается явно, а не макросом MRP_LOG_*: макрос
// разворачивает пакет в один вызов logField, и на двух-трёх полях такой вызов —
// лишняя хрупкость ради экономии двух строк (так же сделано в executor).
core::LogFields field(std::string key, std::string value) {
    core::LogFields fields;
    fields.push_back(core::logField(std::move(key), std::move(value)));
    return fields;
}

core::LogFields add(core::LogFields fields, std::string key, std::string value) {
    fields.push_back(core::logField(std::move(key), std::move(value)));
    return fields;
}

core::LogFields add(core::LogFields fields, std::string key, std::uint64_t value) {
    fields.push_back(core::logField(std::move(key), value));
    return fields;
}

void logPath(const char* event, core::LogLevel level, std::string_view message, std::string_view path,
             std::uint32_t win32Error, bool logFailures, std::string_view txId = {}) {
    if (!logFailures) return;
    core::LogFields fields = field("path", std::string{path});
    fields = add(std::move(fields), "hr", static_cast<std::uint64_t>(win32Error));
    if (!txId.empty()) fields = add(std::move(fields), "tx", std::string{txId});
    switch (level) {
        case core::LogLevel::Error:
            core::logError(event, message, std::move(fields));
            break;
        case core::LogLevel::Warn:
            core::logWarn(event, message, std::move(fields));
            break;
        default:
            core::logInfo(event, message, std::move(fields));
            break;
    }
}

std::string exceptionText(const std::exception& error) {
    return error.what() != nullptr ? std::string{error.what()} : std::string{"исключение без описания"};
}

// Транзакция корзины → модель решения core::undo.
core::UndoTransaction toUndoView(const core::TrashTransaction& tx) {
    core::UndoTransaction view;
    view.txId = tx.txId;
    view.appVersion = tx.appVersion;
    view.createdUnix = tx.createdAt;
    // Отменяема только Committed (§7.2): Open — манифест ещё не записан,
    // Undone — уже вернули, Collapsed — содержимое вытеснено навсегда.
    view.collapsed = tx.state != core::TrashTxState::Committed;
    view.entries.reserve(tx.items.size());
    for (const core::TrashItem& item : tx.items) {
        core::TrashEntry entry;
        entry.originalPath = item.originalPath;
        // payload — уже относительное имя без разделителей и двоеточий
        // (core::isValidPayloadName), то есть ровно то, что ждёт storedPath.
        entry.storedPath = item.payload;
        entry.sizeBytes = item.bytes;
        entry.fileCount = item.fileCount;
        entry.modifiedUnix = item.mtime;
        entry.createdUnix = tx.createdAt;
        entry.aclSddl = item.aclSddl;
        entry.volumeGuidPath = item.sourceVolume;
        entry.isDirectory = item.kind == core::TrashItemKind::Directory;
        view.entries.push_back(std::move(entry));
    }
    return view;
}

const core::TrashEntry* entryOf(const core::UndoTransaction& view, std::size_t index) {
    return index < view.entries.size() ? &view.entries[index] : nullptr;
}

// Запрос платформы на возврат одного элемента. Права перезаписи здесь единственное
// место, где решение пользователя превращается в перезапись (FR-7), и всё
// остальное — про пути, время и ACL, которые берутся из манифеста.
platform::TrashRestoreRequest restoreRequestOf(const std::string& txDir, const core::TrashEntry& entry,
                                               std::string_view targetPath, bool overwrite) {
    platform::TrashRestoreRequest request;
    request.transactionDir = txDir;
    request.item.kind = entry.isDirectory ? core::TrashItemKind::Directory : core::TrashItemKind::File;
    request.item.originalPath = entry.originalPath;
    request.item.payload = entry.storedPath;
    request.item.bytes = entry.sizeBytes;
    request.item.fileCount = entry.fileCount;
    request.item.mtime = entry.modifiedUnix;
    request.item.aclSddl = entry.aclSddl;
    request.item.sourceVolume = entry.volumeGuidPath;
    request.action = overwrite ? core::RestoreAction::Overwrite : core::RestoreAction::Restore;
    request.destinationPath = std::string{targetPath};
    return request;
}

// Почему элемент не вернулся — одним предложением для журнала и отчёта. Различие
// между «занято», «утрачено» и «нет прав» здесь обязательно: это три разных
// решения пользователя, и свалить их в «ошибка» значит потерять объяснение.
std::string platformDetail(const platform::TrashRestoreResult& restored) {
    if (restored.ok()) return {};
    switch (restored.status) {
        case platform::TrashStatus::Ok:
            break;
        case platform::TrashStatus::Cancelled:
            return "операция отменена";
        case platform::TrashStatus::NotFound:
            return "содержимое в корзине утрачено, восстановление невозможно";
        case platform::TrashStatus::AlreadyExists:
            return "на месте уже есть объект, перезапись не разрешена";
        case platform::TrashStatus::AccessDenied:
            return "нет прав на исходное место";
        case platform::TrashStatus::InvalidArgument:
            return "путь или имя объекта в корзине недопустимы";
        case platform::TrashStatus::Corrupt:
            return "манифест транзакции не читается";
        case platform::TrashStatus::OutOfSpace:
            return "на целевом томе не хватает места";
        case platform::TrashStatus::Unsupported:
            return "операция недоступна для этого объекта";
        case platform::TrashStatus::IoError:
        case platform::TrashStatus::OutOfMemory:
            break;
    }
    return "перенос не выполнен: " + platform::formatStatus(restored.status, restored.win32Error);
}

// Конфликт ядра → статус платформы для отчёта: у отказа должен быть тот же
// словарь, что у остальных операций (SPEC §8), иначе «занято» и «нет прав»
// выглядели бы одинаково.
platform::TrashStatus deniedStatus(core::RestoreConflict conflict) {
    switch (conflict) {
        case core::RestoreConflict::MissingInTrash:
            return platform::TrashStatus::NotFound;
        case core::RestoreConflict::TargetExists:
        case core::RestoreConflict::TargetTypeMismatch:
            return platform::TrashStatus::AlreadyExists;
        case core::RestoreConflict::TargetLocked:
        case core::RestoreConflict::ParentMissing:
            return platform::TrashStatus::AccessDenied;
        case core::RestoreConflict::None:
        case core::RestoreConflict::TargetUnknown:
        case core::RestoreConflict::NotEnoughSpace:
        case core::RestoreConflict::AlreadyRestored:
            break;
    }
    return platform::TrashStatus::InvalidArgument;
}

std::string deniedReason(core::RestoreConflict conflict) {
    switch (conflict) {
        case core::RestoreConflict::MissingInTrash:
            return "содержимое в корзине утрачено, восстановление невозможно";
        case core::RestoreConflict::TargetExists:
            return "на месте уже есть объект, перезапись не разрешена";
        case core::RestoreConflict::TargetTypeMismatch:
            return "на месте объект другого типа, перезапись не разрешена";
        case core::RestoreConflict::TargetLocked:
            return "объект на месте занят другим приложением";
        case core::RestoreConflict::ParentMissing:
            return "не удалось создать родительский каталог";
        case core::RestoreConflict::TargetUnknown:
            return "цель не описана, восстанавливать вслепую нельзя";
        case core::RestoreConflict::NotEnoughSpace:
            return "на целевом томе не хватает места";
        case core::RestoreConflict::AlreadyRestored:
            return "элемент уже восстановлен ранее";
        case core::RestoreConflict::None:
            break;
    }
    return "восстановление невозможно";
}

// Сравнение томов без платформенного вызова: строки формата «\\?\Volume{…}\»
// приходят и из манифеста, и из volumeOfPath, а сравнивать их нужно так же, как
// это делает файловая система, — регистронезависимо и целиком.
bool sameVolumeGuid(std::string_view left, std::string_view right) {
    if (left.size() != right.size() || left.empty()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = static_cast<unsigned char>(left[i]);
        const auto b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

// Признак «пора спросить отмену» (§6.4: каждые 256 элементов). Первая проверка
// тоже через счётчик (0 % 256 == 0), поэтому отмена доходит до хранения в том же
// цикле, а не на следующей записи.
struct CancelWatch {
    std::size_t every{platform::kTrashCancelCheckEvery};
    std::size_t counter{};

    [[nodiscard]] bool due(std::stop_token stop) const noexcept {
        return stop.stop_requested() || (counter % every) == 0;
    }
    void bump() noexcept { ++counter; }
};

// Манифест после восстановления. Ключ возвращённого элемента — пара
// (payload, originalPath), а не индекс: транзакция перечитывается с диска перед
// записью (её могли тронуть другой процесс), и сопоставление по индексу тогда
// сняло бы чужие элементы. Возвращённые элементы уходят из транзакции — их
// содержимого в корзине больше нет, — а транзакция без остатка становится Undone,
// то есть Ctrl+Z гаснет (§7.2).
core::TrashTransaction rebuildAfterRestore(const core::TrashTransaction& tx, const core::UndoTransaction& updatedView,
                                            const std::vector<std::pair<std::string, std::string>>& restoredKeys) {
    const auto isRestored = [&restoredKeys](const core::TrashItem& item) {
        return std::find(restoredKeys.begin(), restoredKeys.end(),
                         std::make_pair(item.payload, item.originalPath)) != restoredKeys.end();
    };

    core::TrashTransaction kept;
    kept.txId = updatedView.txId;
    kept.createdAt = updatedView.createdUnix;
    kept.appVersion = updatedView.appVersion;
    kept.purgedBytes = tx.purgedBytes;
    kept.items.reserve(tx.items.size());
    for (const core::TrashItem& item : tx.items) {
        if (isRestored(item)) continue;
        kept.items.push_back(item);
    }
    // Undone — «содержимое вернулось по местам»; Collapsed остаётся для
    // вытесненного (его ставит trash_service), потому что потерянные байты здесь не
    // наш счёт. Состояние «возвращать больше нечего» считает ядро.
    kept.state = updatedView.restorableCount() == 0 && kept.items.empty() ? core::TrashTxState::Undone
                                                                         : core::TrashTxState::Committed;
    return kept;
}

}  // namespace

// ---------------------------------------------------------------------------
// Тексты
// ---------------------------------------------------------------------------

std::string UndoService::detailText(const ItemResult& result) {
    if (result.ok) return "восстановлено";
    if (!result.detail.empty()) return result.detail;
    return platform::formatStatus(result.status, result.win32Error);
}

std::string UndoService::describe(const Snapshot& snapshot) {
    std::string text = "корзина: транзакций " + core::formatCount(snapshot.transactions.size()) +
                       ", отменяемых " + core::formatCount(snapshot.availableTransactions) + ", элементов " +
                       core::formatCount(snapshot.restorableItems) + ", " +
                       core::formatBytes(snapshot.totalBytes, 1, true);
    if (snapshot.expiredTransactions > 0) {
        text += ", просрочено " + core::formatCount(snapshot.expiredTransactions);
    }
    if (!snapshot.brokenDirs.empty()) {
        text += ", битых каталогов " + core::formatCount(snapshot.brokenDirs.size());
    }
    return text;
}

std::string UndoService::describe(const RestorePlanResult& result) {
    if (!result.ok) return "восстановление невозможно: " + result.problem;
    const std::size_t total = result.plan.entries.size();
    const std::size_t skipped = std::min(static_cast<std::size_t>(result.plan.skippedCount), total);
    std::string text = "план восстановления " + result.txId + ": вернуть " + core::formatCount(total - skipped) +
                       " из " + core::formatCount(total) + ", " + core::formatBytes(result.plan.bytesPlanned, 1, true);
    if (result.plan.conflictCount > 0) {
        text += ", конфликтов " + core::formatCount(result.plan.conflictCount);
    }
    if (result.plan.requiresUserDecision) {
        text += ", вопросов " + core::formatCount(result.plan.questions.size());
    }
    if (result.plan.cost.crossVolume) text += ", копирование между томами";
    if (result.plan.cost.slowEnoughToWarn) {
        text += ", это займёт время (около " + core::formatCount(result.plan.cost.seconds) + " с)";
    }
    return text;
}

std::string UndoService::describe(const RestoreResult& result) {
    std::string text = "восстановление " + result.txId;
    if (!result.outcome.summary.empty()) {
        text += ": " + result.outcome.summary;
    }
    if (!result.manifestWritten && !result.manifestProblem.empty()) {
        text += "; манифест не обновлён: " + result.manifestProblem;
    }
    if (result.purged) text += "; каталог транзакции удалён";
    return text;
}

// ---------------------------------------------------------------------------
// Конфигурация
// ---------------------------------------------------------------------------

std::string UndoService::root() const {
    if (!options_.trashRoot.empty()) return options_.trashRoot;
    return platform::defaultTrashRoot();
}

void UndoService::setRoot(std::string trashRoot) { options_.trashRoot = std::move(trashRoot); }

platform::TrashOptions UndoService::effective(const platform::TrashOptions& trashOptions) const {
    platform::TrashOptions out = trashOptions;
    out.applyTimestamps = options_.restoreTimestamps;
    out.applyAcl = options_.restoreAcl;
    // Собственные строки журнала пишет движок (путь, статус, код Win32), а
    // платформенные дубли выключаются: две строки на один отказ читаются как два
    // разных отказа.
    out.logFailures = false;
    if (out.cancelCheckEvery == 0) {
        out.cancelCheckEvery =
            options_.cancelCheckEvery != 0 ? options_.cancelCheckEvery : platform::kTrashCancelCheckEvery;
    }
    return out;
}

bool UndoService::dirOf(const std::string& root, std::string_view txId, std::string& dirOut, std::string& problem) {
    try {
        dirOut = core::transactionDir(root, txId);
    } catch (const std::exception& error) {
        // core::transactionDir бросает на недопустимом идентификаторе, а он
        // приезжает с диска (правило 1 шапки файла).
        problem = exceptionText(error);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Снимок корзины
// ---------------------------------------------------------------------------

Snapshot UndoService::snapshot(std::int64_t now, const platform::TrashOptions& trashOptions) const {
    Snapshot out;
    // Имя не root(): локальная переменная скрыла бы метод с таким же именем, и
    // строка стала бы «вызов std::string» вместо чтения корня.
    const std::string trashRootPath = root();
    if (root.empty()) {
        // Корень неизвестен (нет %ProgramData%) — это отказ, а не «корзина пуста»:
        // вызывающий обязан показать его иначе, чем пустой список.
        logPath(kLogSnapshot, core::LogLevel::Warn, "корень корзины неизвестен, отмена недоступна", root, 0,
                options_.logFailures);
        return out;
    }

    const platform::TrashOptions options = effective(trashOptions);
    const platform::TrashInventory inventory = platform::loadInventory(root, options);
    out.brokenDirs = inventory.brokenDirs;
    out.scannedDirs = inventory.scannedDirs;

    std::vector<std::string> expiredIds;
    try {
        expiredIds = core::selectExpired(options_.limits, inventory.info(), now);
    } catch (const std::exception&) {
        // Возраст — вспомогательная отметка: её отказ не должен отменять снимок.
        expiredIds.clear();
    }
    const auto isExpired = [&expiredIds](const std::string_view txId) {
        return std::find(expiredIds.begin(), expiredIds.end(), txId) != expiredIds.end();
    };

    out.transactions.reserve(inventory.transactions.size());
    for (const core::TrashTransaction& tx : inventory.transactions) {
        TransactionView view;
        view.txId = tx.txId;
        view.appVersion = tx.appVersion;
        view.createdUnix = tx.createdAt;
        view.bytes = tx.totalBytes();
        view.itemCount = tx.itemCount();
        view.state = tx.state;
        view.available = tx.undoable() && !tx.items.empty();
        view.restorableCount = view.available ? view.itemCount : 0u;
        view.expired = isExpired(tx.txId);
        std::string dir;
        std::string problem;
        if (dirOf(root, tx.txId, dir, problem)) view.directory = dir;
        switch (tx.state) {
            case core::TrashTxState::Open:
                view.note = "манифест ещё не записан, отмена недоступна";
                break;
            case core::TrashTxState::Committed:
                view.note =
                    view.expired ? "просрочена: отмена ещё доступна, но содержимое скоро вытеснится" : std::string{};
                break;
            case core::TrashTxState::Undone:
                view.note = "уже восстановлена";
                break;
            case core::TrashTxState::Collapsed:
                view.note = "схлопнулась: содержимое вытеснено, отменять нечего";
                break;
        }
        if (view.available) {
            ++out.availableTransactions;
            out.restorableItems += view.restorableCount;
        }
        if (view.expired) ++out.expiredTransactions;
        out.totalBytes += view.bytes;
        out.transactions.push_back(std::move(view));
    }

    // От новых к старым: список отмены читают сверху вниз, и свежая транзакция — та,
    // которую отменяют чаще всего. При равном времени порядок по txId, чтобы снимок
    // был воспроизводим.
    std::sort(out.transactions.begin(), out.transactions.end(),
              [](const TransactionView& a, const TransactionView& b) {
                  if (a.createdUnix != b.createdUnix) return a.createdUnix > b.createdUnix;
                  return a.txId < b.txId;
              });

    if (options_.logFailures) {
        core::LogFields fields = add(field("root", root), "transactions", out.transactions.size());
        fields = add(std::move(fields), "available", out.availableTransactions);
        fields = add(std::move(fields), "broken", out.brokenDirs.size());
        fields = add(std::move(fields), "bytes", out.totalBytes);
        core::logInfo(kLogSnapshot, describe(out), std::move(fields));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Чтение транзакции
// ---------------------------------------------------------------------------

bool UndoService::readTransaction(std::string_view txId, const platform::TrashOptions& trashOptions,
                                  core::TrashTransaction& txOut, std::string& dirOut,
                                  RestorePlanResult& problem) const {
    // Имя не root(): локальная переменная скрыла бы метод с таким же именем, и
    // строка стала бы «вызов std::string» вместо чтения корня.
    const std::string trashRootPath = root();
    problem.status = platform::TrashStatus::Ok;
    problem.win32Error = 0;
    problem.problem.clear();
    if (root.empty()) {
        problem.status = platform::TrashStatus::InvalidArgument;
        problem.problem = "корень корзины неизвестен: восстановление недоступно";
        return false;
    }
    problem.txId = std::string{txId};
    if (!dirOf(root, txId, dirOut, problem.problem)) {
        problem.status = platform::TrashStatus::InvalidArgument;
        return false;
    }
    problem.directory = dirOut;

    const platform::TrashStatus read = platform::readManifest(dirOut, txOut);
    if (read != platform::TrashStatus::Ok) {
        // Код Win32 платформа в статусе не отдаёт, поэтому в лог идёт имя статуса,
        // а не выдуманное число (§12: «все ошибки в логе с путём и HRESULT»).
        problem.status = read;
        problem.problem = "манифест транзакции не прочитан: " + platform::formatStatus(read, 0);
        logPath(kLogPlan, core::LogLevel::Warn, "манифест транзакции не прочитан", dirOut, 0, options_.logFailures, txId);
        return false;
    }
    if (txOut.txId != txId) {
        // Каталог и манифест разошлись: возврат из такого каталога положил бы данные
        // не туда. Отказ, а не догадка.
        problem.status = platform::TrashStatus::Corrupt;
        problem.problem = "в каталоге " + dirOut + " лежит манифест транзакции " + txOut.txId;
        logPath(kLogPlan, core::LogLevel::Warn, problem.problem, dirOut, 0, options_.logFailures, txId);
        return false;
    }
    if (!txOut.undoable()) {
        problem.status = platform::TrashStatus::InvalidArgument;
        switch (txOut.state) {
            case core::TrashTxState::Open:
                problem.problem = "транзакция не закрыта (манифест не подтверждён): отмена недоступна";
                break;
            case core::TrashTxState::Undone:
                problem.problem = "транзакция уже восстановлена";
                break;
            case core::TrashTxState::Collapsed:
                problem.problem = "транзакция схлопнулась: содержимое вытеснено, отменять нечего";
                break;
            case core::TrashTxState::Committed:
                problem.problem = "транзакция пуста, отменять нечего";
                break;
        }
        return false;
    }
    (void)trashOptions;  // чтение манифеста параметров не принимат (core::trash не бросает)
    return true;
}

// ---------------------------------------------------------------------------
// Снимок состояния цели
// ---------------------------------------------------------------------------

core::RestoreTargetInfo UndoService::probeTarget(std::string_view txDir, const core::TrashEntry& entry,
                                                  std::stop_token stop, std::string& warning) const {
    core::RestoreTargetInfo info;
    info.path = entry.originalPath;
    if (info.path.empty()) {
        // Цель неизвестна — ядро даст TargetUnknown, то есть «спросить», а не
        // «считать свободной» (FR-7: не перезаписывать без вопроса).
        warning = "у элемента нет исходного пути, цель восстановления неизвестна";
        return info;
    }

    const bool exists = platform::pathExists(info.path);
    info.exists = exists;
    info.isDirectory = exists ? platform::isDirectory(info.path) : entry.isDirectory;

    if (exists && options_.checkLocks) {
        // Отказ Restart Manager не равен «файл свободен»: неизвестный ответ RM —
        // предупреждение, а не разрешение на перезапись (FR-6, engine::locks).
        const locks::LockInfo lock = locks::queryPath(info.path, options_.lockOptions, stop);
        if (lock.locked()) {
            info.locked = true;
        } else if (lock.unknown()) {
            warning = "занятость " + info.path + " не определена: " + lock.statusText;
        }
    }

    const std::string parent = platform::parentPath(info.path);
    info.parentExists = parent.empty() || platform::pathExists(parent);

    // Том и свободное место спрашиваются там, где ответ достоверен: несуществующий
    // путь не описывает том надёжнее своего родительского каталога.
    const std::string volumeProbe = exists || parent.empty() ? info.path : parent;
    const platform::TrashVolumeInfo volume = platform::volumeOfPath(volumeProbe);
    if (volume.volumeGuid.empty()) {
        warning = "том для " + info.path + " определить не удалось: проверка свободного места пропущена";
    } else if (volume.freeBytes == 0) {
        // Ноль свободных байт на живом томе не бывает — значит, место не спросили.
        // Оставить это как «0» нельзя: ядро превратит нули в NotEnoughSpace и
        // заблокирует восстановление из-за отказа опроса.
        warning = "свободное место на томе " + info.path + " узнать не удалось: проверка вместимости пропущена";
    } else {
        info.volumeGuidPath = volume.volumeGuid;
        info.freeBytes = volume.freeBytes;
        if (!entry.volumeGuidPath.empty() && !sameVolumeGuid(entry.volumeGuidPath, volume.volumeGuid)) {
            // Исходный том записан при очистке; если он не тот, диск вынули или
            // заменили. Для съёмного носителя с прежней буквой это ровно тот
            // случай, где «вернуть на место» опасно, поэтому — предупреждение в
            // журнал, отчёт и UI.
            warning = "том исходного элемента не совпадает с текущим: " + entry.volumeGuidPath + " → " +
                      volume.volumeGuid;
        }
    }

    const std::string stored = core::joinPath(txDir, entry.storedPath);
    info.contentExists = platform::pathExists(stored);
    if (!info.contentExists) {
        warning = "содержимое " + stored + " в корзине отсутствует: восстановить нечего";
    }
    return info;
}

// ---------------------------------------------------------------------------
// План восстановления
// ---------------------------------------------------------------------------

RestorePlanResult UndoService::planRestore(std::string_view txId, const core::RestoreRequest& request,
                                            std::int64_t now, std::stop_token stop) const {
    (void)now;  // возраст транзакции в плане не участвует: он нужен лимитам вытеснения
    RestorePlanResult result;
    result.txId = std::string{txId};

    const platform::TrashOptions options = effective({});
    core::TrashTransaction tx;
    if (!readTransaction(txId, options, tx, result.directory, result)) return result;

    // Имя не root(): локальная переменная скрыла бы метод с таким же именем, и
    // строка стала бы «вызов std::string» вместо чтения корня.
    const std::string trashRootPath = root();
    result.view = toUndoView(tx);

    // Том корзины нужен ядру для оценки копирования (FR-7: «честно показываем,
    // что это долго»). Не задан — спрашиваем один раз на план, а не по элементу.
    std::string trashVolume = request.trashVolume;
    if (trashVolume.empty()) trashVolume = platform::volumeOfPath(root).volumeGuid;
    result.trashVolume = trashVolume;

    core::RestoreRequest wanted = request;
    wanted.txId = tx.txId;
    wanted.trashVolume = trashVolume;

    CancelWatch watch;
    watch.every = options.cancelCheckEvery != 0 ? options.cancelCheckEvery : platform::kTrashCancelCheckEvery;

    std::vector<core::RestoreTargetInfo> targets;
    targets.reserve(result.view.entries.size());
    for (const core::TrashEntry& entry : result.view.entries) {
        watch.bump();
        if (watch.due(stop)) {
            result.status = platform::TrashStatus::Cancelled;
            result.problem = "отменено до построения плана";
            return result;
        }
        std::string warning;
        targets.push_back(probeTarget(result.directory, entry, stop, warning));
        if (!warning.empty()) result.warnings.push_back(std::move(warning));
    }

    try {
        result.plan = core::buildRestorePlan(result.view, wanted, targets);
    } catch (const std::exception& error) {
        // Расхождение плана и снимка — ошибка слоёв, а не «восстановить нельзя»:
        // молчаливый отказ здесь означал бы, что отмена не работает, и никто бы
        // об этом не узнал.
        result.status = platform::TrashStatus::Corrupt;
        result.problem = "план восстановления не построен: " + exceptionText(error);
        logPath(kLogPlan, core::LogLevel::Error, result.problem, result.directory, 0, options_.logFailures, txId);
        return result;
    }

    // Цена копирования — с той скоростью, которую задал вызывающий, а не с
    // умолчанием ядра: предупреждение «это займёт время» обязано считаться по той
    // же модели, по которой считает остальной проект.
    result.plan.cost = core::estimateRestoreCost(
        result.view, trashVolume,
        options_.throughputBytesPerSec != 0 ? options_.throughputBytesPerSec : core::kDefaultCopyThroughputBytesPerSec);

    result.ok = true;
    if (options_.logFailures) {
        core::LogFields fields = add(field("tx", result.txId), "items", result.plan.entries.size());
        fields = add(std::move(fields), "conflicts", result.plan.conflictCount);
        fields = add(std::move(fields), "questions", result.plan.questions.size());
        fields = add(std::move(fields), "bytes", result.plan.bytesPlanned);
        core::logInfo(kLogPlan, describe(result), std::move(fields));
        for (const std::string& warning : result.warnings) {
            core::logWarn(kLogPlan, warning, field("tx", result.txId));
        }
    }
    return result;
}

core::UndoPlan UndoService::applyAnswers(core::UndoPlan plan, const core::RestoreAnswers& answers) {
    return core::applyRestoreAnswers(std::move(plan), answers);
}

// ---------------------------------------------------------------------------
// Исполнение
// ---------------------------------------------------------------------------

RestoreResult UndoService::restore(const RestorePlanResult& prepared, std::int64_t now, std::stop_token stop,
                                    const ProgressFn& progress) {
    RestoreResult result;
    result.txId = prepared.txId;
    if (!prepared.ok) {
        result.summary = "восстановление невозможно: " + prepared.problem;
        logPath(kLogRestore, core::LogLevel::Warn, "восстановление невозможно", prepared.directory, 0,
                options_.logFailures, prepared.txId);
        return result;
    }

    const platform::TrashOptions options = effective({});
    const core::UndoPlan& plan = prepared.plan;
    result.items.reserve(plan.entries.size());

    CancelWatch watch;
    watch.every = options.cancelCheckEvery != 0 ? options.cancelCheckEvery : platform::kTrashCancelCheckEvery;

    // Результаты ведутся в двух пространствах индексов, и это не опечатка:
    // core::summarizeRestore читает entryIndex как позицию в плане, а
    // core::applyRestored — как индекс элемента транзакции. При полном
    // восстановлении они совпадают, при частичном — нет, поэтому перевод делается
    // здесь явно, а не «повезло».
    std::vector<core::RestoreEntryResult> planResults;
    std::vector<core::RestoreEntryResult> txResults;
    std::vector<std::pair<std::string, std::string>> restoredKeys;
    planResults.reserve(plan.entries.size());
    txResults.reserve(plan.entries.size());

    std::uint64_t bytesRestored = 0;
    std::size_t done = 0;
    for (std::size_t position = 0; position < plan.entries.size(); ++position) {
        const core::UndoPlanEntry& planned = plan.entries[position];
        watch.bump();
        if (watch.due(stop)) {
            // Остальные записи остаются без результата — summarizeRestore посчитает
            // их как «не выполнено», и это честнее, чем «восстановлено всё».
            result.cancelled = true;
            break;
        }

        ItemResult item;
        item.planIndex = position;
        item.itemIndex = planned.entryIndex;
        item.decision = planned.action;
        item.targetPath = planned.targetPath;

        const bool wantsOverwrite = planned.action == core::RestoreDecision::Overwrite;
        const bool wantsMove = planned.action != core::RestoreDecision::Skip;
        bool wasCancelled = false;

        if (!wantsMove) {
            // Пропуск — это решение (политика, ответ пользователя, «уже
            // восстановлено», «утрачено»), а не сбой: в результатах такая запись не
            // попадает, и summarizeRestore считает её пропущенной.
            item.ok = false;
            item.status = deniedStatus(planned.conflict);
            item.detail = planned.note.empty() ? deniedReason(planned.conflict) : planned.note;
        } else {
            const core::TrashEntry* entry = entryOf(prepared.view, planned.entryIndex);
            // FR-7, правило 5 шапки файла: перед самым переносом цель
            // перепроверяется. План мог быть построен до того, как файл появился,
            // и «не перезаписывать» должно быть свойством исполнения, а не плана.
            const bool existsNow = platform::pathExists(item.targetPath);
            if (entry == nullptr) {
                item.status = platform::TrashStatus::InvalidArgument;
                item.detail = "запись плана не соответствует транзакции: индекс " +
                              std::to_string(planned.entryIndex);
            } else if (entry->restored) {
                item.status = platform::TrashStatus::AlreadyExists;
                item.detail = deniedReason(core::RestoreConflict::AlreadyRestored);
            } else if (!core::isValidPayloadName(entry->storedPath)) {
                item.status = platform::TrashStatus::InvalidArgument;
                item.detail = "недопустимое имя объекта в корзине: " + entry->storedPath;
            } else if (existsNow && !wantsOverwrite) {
                item.status = platform::TrashStatus::AlreadyExists;
                item.detail = "цель снова занята: " + item.targetPath + " (план строился раньше)";
            } else {
                const platform::TrashRestoreRequest request =
                    restoreRequestOf(prepared.directory, *entry, item.targetPath, wantsOverwrite);
                const platform::TrashRestoreResult restored = platform::restoreTrashItem(request, options);
                item.status = restored.status;
                item.win32Error = restored.win32Error;
                item.ok = restored.ok();
                item.bytesRestored = restored.bytesRestored;
                item.detail = platformDetail(restored);
                if (item.ok) {
                    bytesRestored += restored.bytesRestored;
                } else if (restored.status == platform::TrashStatus::Cancelled) {
                    result.cancelled = true;
                    wasCancelled = true;
                }
            }
            if (!item.ok && options_.logFailures) {
                core::LogFields fields = add(field("path", item.targetPath), "tx", result.txId);
                fields = add(std::move(fields), "status", static_cast<std::uint64_t>(item.status));
                fields = add(std::move(fields), "hr", static_cast<std::uint64_t>(item.win32Error));
                core::logWarn(wasCancelled ? kLogItem : kLogDenied,
                              item.detail.empty() ? "элемент не восстановлен" : item.detail, std::move(fields));
            }
        }

        result.items.push_back(item);

        core::RestoreEntryResult planResult;
        planResult.entryIndex = position;  // пространство плана — для summarizeRestore
        planResult.ok = item.ok;
        planResult.bytesRestored = item.bytesRestored;
        planResult.error = item.detail;
        planResults.push_back(planResult);

        core::RestoreEntryResult txResult = planResult;
        txResult.entryIndex = planned.entryIndex;  // пространство транзакции — для applyRestored
        txResults.push_back(txResult);
        if (item.ok) {
            const core::TrashEntry* entry = entryOf(prepared.view, planned.entryIndex);
            if (entry != nullptr) restoredKeys.emplace_back(entry->storedPath, entry->originalPath);
        }

        ++done;
        if (progress) {
            Progress step;
            step.done = done;
            step.total = plan.entries.size();
            step.entry = &planned;
            step.result = item;
            step.bytesRestored = bytesRestored;
            progress(step);
        }
        if (result.cancelled) break;
    }

    result.outcome = core::summarizeRestore(plan, planResults);

    // Манифест — только после факта (правило 7 шапки файла): сначала перенос, потом
    // отметка. Иначе отмена или сбой оставили бы транзакцию, которая обещает вернуть
    // то, чего в корзине уже нет.
    if (txResults.empty()) {
        result.manifestWritten = true;  // переносить было нечего: манифест в силе
    } else {
        core::UndoTransaction updatedView;
        try {
            updatedView = core::applyRestored(prepared.view, plan, txResults, now);
        } catch (const std::exception& error) {
            updatedView = prepared.view;
            result.manifestProblem = "состояние транзакции не пересчитано: " + exceptionText(error);
        }

        // Перечитываем манифест перед записью: пока шёл возврат, транзакцию могли
        // тронуть другой процесс (другая копия приложения), и запись по старому
        // снимку вернула бы на диск уже снятые элементы.
        core::TrashTransaction current;
        std::string dirOut;
        RestorePlanResult reread;
        if (!readTransaction(result.txId, options, current, dirOut, reread)) {
            result.manifestProblem = reread.problem;
        } else {
            const core::TrashTransaction updated = rebuildAfterRestore(current, updatedView, restoredKeys);
            const platform::TrashStatus written = platform::writeManifest(dirOut, updated, options);
            result.manifestWritten = written == platform::TrashStatus::Ok;
            if (!result.manifestWritten) {
                result.manifestProblem = "манифест не записан: " + platform::formatStatus(written, 0);
                logPath(kLogManifest, core::LogLevel::Error, result.manifestProblem, dirOut, 0, options_.logFailures,
                        result.txId);
            } else {
                if (options_.logFailures) {
                    core::LogFields fields = add(field("tx", result.txId), "items", updated.items.size());
                    fields = add(std::move(fields), "state", static_cast<std::uint64_t>(updated.state));
                    core::logInfo(kLogManifest, "манифест приведён в соответствие с фактом", std::move(fields));
                }
                // Каталог сносится только когда вернулось всё и манифест записан:
                // тогда в нём не остаётся ничего, что можно было бы потерять.
                if (options_.purgeAfterFullRestore && updated.items.empty() && result.outcome.failedCount == 0 &&
                    !result.cancelled) {
                    const platform::TrashPurgeResult purged = platform::purgeTransactionDirectory(dirOut, options);
                    result.purged = purged.ok();
                    if (!result.purged && options_.logFailures) {
                        logPath(kLogPurge, core::LogLevel::Warn, "каталог восстановленной транзакции не удалён", dirOut,
                                0, true, result.txId);
                    }
                }
            }
        }
    }

    result.summary = describe(result);
    if (options_.logFailures) {
        core::LogFields fields = add(field("tx", result.txId), "restored", result.outcome.restoredCount);
        fields = add(std::move(fields), "skipped", result.outcome.skippedCount);
        fields = add(std::move(fields), "failed", result.outcome.failedCount);
        fields = add(std::move(fields), "notAttempted", result.outcome.notAttemptedCount);
        fields = add(std::move(fields), "bytes", result.outcome.bytesRestored);
        fields = add(std::move(fields), "cancelled", result.cancelled);
        core::logInfo(kLogRestore, result.summary, std::move(fields));
    }
    return result;
}

RestoreResult UndoService::restoreNow(std::string_view txId, const core::RestoreRequest& request, std::int64_t now,
                                      std::stop_token stop, const ProgressFn& progress) {
    const RestorePlanResult prepared = planRestore(txId, request, now, stop);
    return restore(prepared, now, stop, progress);
}

bool UndoService::purge(std::string_view txId, std::string& detail, const platform::TrashOptions& trashOptions) {
    detail.clear();
    // Имя не root(): локальная переменная скрыла бы метод с таким же именем, и
    // строка стала бы «вызов std::string» вместо чтения корня.
    const std::string trashRootPath = root();
    if (root.empty()) {
        detail = "корень корзины неизвестен";
        return false;
    }
    std::string dir;
    std::string problem;
    if (!dirOf(root, txId, dir, problem)) {
        detail = problem;
        logPath(kLogPurge, core::LogLevel::Warn, "недопустимый идентификатор транзакции", root, 0, options_.logFailures,
                txId);
        return false;
    }
    const platform::TrashOptions options = effective(trashOptions);
    const platform::TrashPurgeResult purged = platform::purgeTransactionDirectory(dir, options);
    if (!purged.ok()) {
        detail = purged.status == platform::TrashStatus::NotFound
                     ? "каталога транзакции нет: " + dir
                     : platform::formatStatus(purged.status, purged.win32Error);
        logPath(kLogPurge, core::LogLevel::Warn, "каталог транзакции не удалён", dir, 0, options_.logFailures, txId);
        return false;
    }
    if (options_.logFailures) {
        core::LogFields fields = add(field("path", dir), "tx", std::string{txId});
        fields = add(std::move(fields), "removedBytes", purged.removedBytes);
        core::logInfo(kLogPurge, "каталог транзакции удалён", std::move(fields));
    }
    return true;
}

}  // namespace mrproper::engine::undo_service
