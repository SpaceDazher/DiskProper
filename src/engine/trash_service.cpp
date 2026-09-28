// Реализация корзины приложения: оркестрация core::trash (решения) и
// platform::vfs_trash (байты). Обоснование границ и инвариантов — в
// trash_service.hpp; здесь код и замечания, которые видны с этого уровня.
//
// Модуль состоит из пяти шагов, и каждый отвечает на один вопрос:
//
//   1. `open`/`refresh` — «что реально лежит в корзине»: манифесты читаются с
//      диска и поднимаются в core::TrashLedger. После этого лимиты считаются по
//      тому, что есть, а не по тому, что помнит процесс (FR-7: ≤ 2 ГБ и ≤ 7
//      дней — обещание пользователю, а не намерение).
//   2. `planPlacement`/`admit` — «куда это деть»: решение считает ядро, сервис
//      исполняет — сперва вытеснение, потом перенос байтов. Обратный порядок
//      стоил бы лишнего копирования на самом дорогом шаге.
//   3. `stage`/`stageAll`/`commit` — «перенести и закрыть»: манифест пишется
//      атомарно платформой, и только после этого транзакция становится
//      отменяемой (§7.2).
//   4. `enforceLimits`/`purgeExpired`/`purgeTransaction`/`empty` — «выполнить
//      обещание лимитов»: жертв выбирает ядро, сносит платформа, а сервис
//      переводит журнал в «схлопнулась», чтобы отмена исчезла из интерфейса
//      вместе с возможностью.
//   5. `planRestore`/`restore` — «вернуть на место»: план строит ядро, исполняет
//      платформа, а сервис следит, чтобы манифест не продолжал обещать уже
//      возвращённые файлы.
//
// Чего модуль не делает и почему это важно. Ни одно решение FR-7 не принимается
// здесь повторно: крупный кэш, лимит, кросс-томовый перенос считает ядро, и
// сервис, который решил бы это ещё раз, был бы вторым местом с числами «2 ГБ» и
// «100 МБ». Прямое удаление сервис не делает (исполнитель плана), политику
// конфликтов при восстановлении — тоже (undo_service), повышения прав не
// просит (§5).
//
// Про порядок в `commit()`. Манифест на диске и состояние в памяти должны
// совпадать, иначе следующий запуск прочитает состояние, которого не было:
// сначала в копии транзакции выставляется Committed, потом пишется файл, и лишь
// после успеха записи меняется журнал. Обратный порядок оставил бы на диске
// манифест со state=open, который «не отменяемая транзакция с содержимым».
//
// Про отмену. Копирование платформа откатывает сама, поэтому отменённый перенос
// одного объекта не оставляет на диске ничего. Но если до отмены уже что-то
// легло, сносить каталог нельзя: это было бы удаление без возможности отмены.
// Поэтому в `admit()` и `admitAll()` отмена останавливает перенос и закрывает
// транзакцию манифестом, а `abandon()` сносит каталог только когда в
// транзакции не лежит ни байта.
#include "trash_service.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/units.hpp"

namespace mrproper::engine::trash_service {
namespace {

namespace pl = platform;

// Имена событий журнала. Отдельные константы, а не склейка «префикс + суффикс»
// по месту: grep по журналу должен находить все записи модуля одним словом.
constexpr std::string_view kEventOpen{"engine.trash.open"};
constexpr std::string_view kEventAdmit{"engine.trash.admit"};
constexpr std::string_view kEventStage{"engine.trash.stage"};
constexpr std::string_view kEventCommit{"engine.trash.commit"};
constexpr std::string_view kEventAbandon{"engine.trash.abandon"};
constexpr std::string_view kEventEvict{"engine.trash.evict"};
constexpr std::string_view kEventPurge{"engine.trash.purge"};
constexpr std::string_view kEventRestore{"engine.trash.restore"};
constexpr std::string_view kEventState{"engine.trash.state"};

// Сообщения. Один набор строк на весь модуль: формулировка отказа попадает и в
// журнал, и в отчёт, и разойтись они не могут.
constexpr std::string_view kMsgAdmitDirect{"элемент решено удалить напрямую, не в корзину"};
constexpr std::string_view kMsgAdmitPlaced{"элемент перенесён в корзину"};
constexpr std::string_view kMsgMeasure{"объект не удалось измерить"};
constexpr std::string_view kMsgNoTxDir{"каталог транзакции не создан, в корзину положить нечем"};
constexpr std::string_view kMsgStageFail{"объект не удалось перенести в корзину"};
constexpr std::string_view kMsgStageUnknown{"транзакция не открыта, перенос невозможен"};
constexpr std::string_view kMsgNotOpen{"транзакция не в состоянии Open"};
constexpr std::string_view kMsgManifest{"манифест транзакции не записан"};
constexpr std::string_view kMsgPurgeFail{"каталог транзакции не снят"};
constexpr std::string_view kMsgPurgeBroken{"каталог без манифеста не снят"};
constexpr std::string_view kMsgRewrite{"манифест не перезаписан"};
constexpr std::string_view kMsgNotUndoable{"транзакция не отменяема, восстановление не выполнялось"};
constexpr std::string_view kMsgCancelled{"работа остановлена по отмене пользователя"};

// Признаки состояния транзакции читаются из манифеста на диске, и «не то
// состояние» — это не то же самое, что «нет транзакции»: первое требует
// внимания (часть содержимого может быть уже недоступна), второе — просто
// «нечего восстанавливать».
constexpr std::string_view kMsgStaleAdopted{"транзакция на диске осталась незакрытой: сбой при фиксации"};
constexpr std::string_view kMsgUndoneSkipped{"транзакция помечена Undone, но каталог на месте: сбой при снятии"};
constexpr std::string_view kMsgBadTxId{"идентификатор транзакции с диска не прошёл проверку"};

// Сложение без переполнения: суммы байтов в отчёте не должны переворачиваться в
// ноль на 32-битном переполнении (тип uint64, но лимит может прийти извне).
std::uint64_t addSaturating(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b ? std::numeric_limits<std::uint64_t>::max() : a + b;
}

// Узкое место преобразования size_t → uint32_t. Счётчики отчёта 32-битные по
// соглашению с моделью (core::TrashUsage), а циклы оперируют size_t.
std::uint32_t narrow(std::size_t value) noexcept {
    return value > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(value);
}

// Слить два отчёта об очистке: вызывающий (admit/empty) складывает результаты
// своих шагов в один снимок, иначе «что вытеснили за прогон» пришлось бы собирать
// вручную в трёх местах.
void merge(PurgeReport& into, PurgeReport&& from) {
    into.purged.insert(into.purged.end(), std::make_move_iterator(from.purged.begin()),
                       std::make_move_iterator(from.purged.end()));
    into.failed.insert(into.failed.end(), std::make_move_iterator(from.failed.begin()),
                       std::make_move_iterator(from.failed.end()));
    into.collapsedTxIds.insert(into.collapsedTxIds.end(), std::make_move_iterator(from.collapsedTxIds.begin()),
                               std::make_move_iterator(from.collapsedTxIds.end()));
    into.removedBytes = addSaturating(into.removedBytes, from.removedBytes);
    into.removedDirs += from.removedDirs;
    into.failedCount += from.failedCount;
    into.cancelled = into.cancelled || from.cancelled;
}

// Отказ одной строкой. Своими словами сервис пишет только то, чего платформа не
// написала: путь, состояние и код Win32 (SPEC §12). Сама платформа молчит —
// иначе один отказ давал бы две строки журнала.
void logFailure(bool enabled, std::string_view event, std::string_view message, const std::string& path,
                std::uint32_t win32Error, const char* status, core::LogFields fields = {}) {
    if (!enabled) return;
    fields.push_back(core::logField("status", status));
    core::logFailure(event, message, path, static_cast<std::int64_t>(win32Error), std::move(fields));
}

core::LogFields bytesField(std::uint64_t bytes) {
    core::LogFields fields;
    fields.push_back(core::logField("bytes", bytes));
    return fields;
}

}  // namespace

// ---------------------------------------------------------------------------
// Конфигурация
// ---------------------------------------------------------------------------

void TrashService::setLimits(core::TrashLimits limits) {
    options_.limits = limits;
    // Журнал и сервис обязаны считать по одному лимиту: иначе решение о
    // размещении принято по одним числам, а вытеснение выполнено по другим.
    ledger_.setLimits(limits);
}

void TrashService::setRoot(std::string trashRoot) {
    options_.trashRoot = std::move(trashRoot);
    ledger_.setRoot(options_.trashRoot);
}

std::string TrashService::resolvedRoot() const {
    return options_.trashRoot.empty() ? pl::defaultTrashRoot() : options_.trashRoot;
}

std::size_t TrashService::cancelStride() const noexcept {
    return options_.cancelCheckEvery != 0 ? options_.cancelCheckEvery : pl::kTrashCancelCheckEvery;
}

platform::TrashOptions TrashService::effective(const platform::TrashOptions& trashOptions) const {
    platform::TrashOptions opts = trashOptions;
    // Журналирование отказов ведёт сервис (одна строка на объект). Отмена,
    // прогресс и «вернуть ли время/ACL при восстановлении» остаются за
    // вызывающим: это его решения, а не сервиса.
    opts.logFailures = false;
    if (opts.cancelCheckEvery == 0) opts.cancelCheckEvery = cancelStride();
    return opts;
}

std::string TrashService::payloadName(std::size_t index) { return "p" + std::to_string(index); }

std::string TrashService::dirFor(std::string_view txId) const {
    if (!core::isValidTxId(txId)) return {};
    const std::string root = ledger_.root().empty() ? resolvedRoot() : ledger_.root();
    if (root.empty()) return {};
    return core::joinPath(root, txId);
}

std::string TrashService::manifestFor(std::string_view txId) const {
    const std::string dir = dirFor(txId);
    return dir.empty() ? std::string{} : core::manifestPath(dir);
}

std::uint32_t TrashService::failuresOf(std::string_view txId) const {
    const auto it = failures_.find(std::string(txId));
    return it == failures_.end() ? 0u : it->second;
}

void TrashService::noteFailure(std::string_view txId) {
    if (txId.empty()) return;
    std::uint32_t& count = failures_[std::string(txId)];
    if (count != std::numeric_limits<std::uint32_t>::max()) ++count;
}

// ---------------------------------------------------------------------------
// Открытие: подъём состояния с диска
// ---------------------------------------------------------------------------

OpenReport TrashService::open(const platform::TrashOptions& trashOptions) {
    OpenReport report;
    const std::string root = resolvedRoot();
    setRoot(root);
    if (root.empty()) {
        // ProgramData недоступен (сервисная сессия). Выдумывать корень хуже, чем
        // сказать «отменять нечем»: удаление при этом продолжится напрямую.
        report.status = pl::TrashStatus::InvalidArgument;
        core::LogFields fields;
        fields.push_back(core::logField("programData", pl::programDataDir()));
        logFailure(options_.logFailures, kEventOpen, "корень корзины неизвестен: нет ProgramData", "<root>",
                   0, pl::toString(report.status), std::move(fields));
        return report;
    }
    report.root = root;

    // Корень создаётся сразу: иначе первый запуск сообщал бы «нечем отменять»
    // в момент, когда очистка уже идёт, а пустой каталог всё равно нужен.
    platform::TrashOptions verbose = effective(trashOptions);
    // Своего кода Win32 у ensureDirectoryTree нет, а молчать об отказе нельзя:
    // единственный источник текста с кодом ошибки — сама платформа.
    verbose.logFailures = true;
    const pl::TrashStatus created = pl::ensureDirectoryTree(root, verbose);
    if (created != pl::TrashStatus::Ok) {
        report.status = created;
        report.path = root;
        logFailure(options_.logFailures, kEventOpen, "корень корзины не создан", root, 0, pl::toString(created));
        return report;
    }
    return refresh(trashOptions);
}

OpenReport TrashService::refresh(const platform::TrashOptions& trashOptions) {
    OpenReport report;
    const platform::TrashOptions opts = effective(trashOptions);
    const std::string root = resolvedRoot();
    report.root = root;
    if (root.empty()) {
        report.status = pl::TrashStatus::InvalidArgument;
        return report;
    }

    pl::TrashInventory inventory = pl::loadInventory(root, opts);
    report.scannedDirs = inventory.scannedDirs;

    std::vector<std::string> broken = std::move(inventory.brokenDirs);
    for (auto& tx : inventory.transactions) {
        // Идентификатор нужен до adopt: внутри он перемещается, а при отказе
        // (core::TrashError на недопустимом имени) объект уже пуст.
        const std::string txId = tx.txId;
        if (tx.state == core::TrashTxState::Undone) {
            // Содержимое пользователю уже вернули, но каталог остался: сбой между
            // пометкой и снятием. В учёт такая транзакция не берётся (иначе лимит
            // считал бы несуществующие байты), а каталог убирается только
            // явным решением — purgeBroken() или empty().
            logFailure(options_.logFailures, kEventOpen, kMsgUndoneSkipped, core::joinPath(root, txId), 0, "undone",
                       bytesField(tx.totalBytes()));
            broken.push_back(core::joinPath(root, txId));
            continue;
        }
        if (tx.state == core::TrashTxState::Open) {
            // Сбой между записью манифеста и пометкой Committed: содержимое на
            // месте и восстановимо фактически, но состояние говорит «не
            // отменяемо». Молчать об этом нельзя — отчёт обязан показать.
            ++report.staleDirs;
            logFailure(options_.logFailures, kEventOpen, kMsgStaleAdopted, core::joinPath(root, txId), 0, "open",
                       bytesField(tx.totalBytes()));
        }
        try {
            if (ledger_.adopt(std::move(tx))) ++report.adopted;
        } catch (const core::TrashError& error) {
            // Идентификатор с диска не прошёл core::isValidTxId: каталог не
            // наш транзакционный, принять его в учёт нельзя, а сносить
            // содержимое неизвестного каталога без решения тоже.
            logFailure(options_.logFailures, kEventOpen, kMsgBadTxId, core::joinPath(root, txId), 0, "invalid-txid");
            core::LogFields fields;
            fields.push_back(core::logField("error", error.what()));
            core::logWarn(kEventOpen, "транзакция не принята в журнал", std::move(fields));
            broken.push_back(core::joinPath(root, txId));
        }
    }

    brokenDirs_ = std::move(broken);
    report.brokenDirs = narrow(brokenDirs_.size());
    opened_ = true;

    core::LogFields fields;
    fields.push_back(core::logField("root", root));
    fields.push_back(core::logField("adopted", report.adopted));
    fields.push_back(core::logField("stale", report.staleDirs));
    fields.push_back(core::logField("broken", report.brokenDirs));
    core::logInfo(kEventOpen, "корзина открыта", std::move(fields));
    return report;
}

PurgeReport TrashService::purgeBroken(const platform::TrashOptions& trashOptions) {
    PurgeReport report;
    const platform::TrashOptions opts = effective(trashOptions);
    std::vector<std::string> remaining;
    remaining.reserve(brokenDirs_.size());

    for (std::size_t i = 0; i < brokenDirs_.size(); ++i) {
        const std::string& dir = brokenDirs_[i];
        if (opts.stop.stop_requested()) {
            report.cancelled = true;
            remaining.insert(remaining.end(), brokenDirs_.begin() + static_cast<std::ptrdiff_t>(i),
                             brokenDirs_.end());
            break;
        }
        const pl::TrashPurgeResult result = pl::purgePath(dir, opts);
        report.removedBytes = addSaturating(report.removedBytes, result.removedBytes);
        report.removedDirs += result.removedDirs;
        if (result.ok()) {
            report.purged.push_back(dir);
            continue;
        }
        // Не снятый каталог остаётся в списке: следующий open() увидит его
        // снова, и «очистить корзину» не будет оставлять мусор навсегда.
        remaining.push_back(dir);
        ++report.failedCount;
        report.failed.push_back(dir);
        logFailure(options_.logFailures, kEventPurge, kMsgPurgeBroken, dir, result.win32Error,
                   pl::toString(result.status));
    }

    brokenDirs_ = std::move(remaining);
    if (!report.empty() || report.cancelled) {
        core::LogFields fields;
        fields.push_back(core::logField("purged", narrow(report.purged.size())));
        fields.push_back(core::logField("failed", report.failedCount));
        fields.push_back(core::logField("removedBytes", report.removedBytes));
        fields.push_back(core::logField("cancelled", report.cancelled));
        core::logInfo(kEventPurge, "каталоги без манифеста обработаны", std::move(fields));
    }
    return report;
}

// ---------------------------------------------------------------------------
// Размещение
// ---------------------------------------------------------------------------

Placement TrashService::planPlacement(std::string_view sourcePath, std::uint64_t itemBytes, std::int64_t now) const {
    Placement placement;
    placement.measured = itemBytes != 0;
    if (sourcePath.empty()) {
        placement.plan.placement = core::TrashPlacement::DirectDelete;
        placement.plan.reason = "пустой путь: удалять нечего";
        return placement;
    }

    const std::string source(sourcePath);
    const std::string root = resolvedRoot();

    bool known = false;
    const bool same = pl::sameVolume(source, root, known);
    placement.volumeKnown = known;
    // Том неизвестен (сетевой путь, точка монтирования) — решение о кросс-томовом
    // переносе принимает MoveFileEx, а не сравнение строк (шапка
    // platform::sameVolume). Поэтому наружу уходит «не дорого» с пометкой
    // volumeKnown=false, а не выдуманное «тот же том».
    placement.sameVolume = known ? same : true;
    placement.sourceVolume = pl::volumeOfPath(source).volumeGuid;
    placement.trashVolume = pl::volumeOfPath(root).volumeGuid;
    placement.plan = core::planTrashPlacement(limits(), ledger_.info(), itemBytes, known ? same : true, now);
    return placement;
}

AdmitResult TrashService::admit(const AdmitRequest& request, const platform::TrashOptions& trashOptions) {
    AdmitResult result;
    const platform::TrashOptions opts = effective(trashOptions);

    if (request.sourcePath.empty()) {
        result.placement.plan.placement = core::TrashPlacement::DirectDelete;
        result.placement.plan.reason = "пустой путь: удалять нечего";
        result.stage.status = pl::TrashStatus::InvalidArgument;
        result.stage.path = request.sourcePath;
        logFailure(options_.logFailures, kEventAdmit, "пустой путь в запросе размещения", "<empty>", 0,
                   pl::toString(result.stage.status));
        return result;
    }

    // Размер обычно известен из скана. Когда неизвестен, объект измеряется один
    // раз здесь, и решение пересчитывается на фактическом размере: «крупный
    // кэш» иначе определялось бы по нулю и крупный кэш попал бы в корзину.
    std::uint64_t bytes = request.itemBytes;
    if (bytes == 0) {
        pl::TrashFacts facts;
        const pl::TrashStatus measured = pl::readPathFacts(request.sourcePath, facts, opts);
        if (measured == pl::TrashStatus::Cancelled) {
            result.stage.status = measured;
            result.stage.path = request.sourcePath;
            result.stage.failedPath = request.sourcePath;
            return result;
        }
        if (measured != pl::TrashStatus::Ok) {
            result.stage.status = measured;
            result.stage.path = request.sourcePath;
            result.stage.failedPath = request.sourcePath;
            result.placement.plan.placement = core::TrashPlacement::DirectDelete;
            result.placement.plan.reason = "объект не измерен: решение по лимитам не принимается";
            logFailure(options_.logFailures, kEventAdmit, kMsgMeasure, request.sourcePath, 0,
                       pl::toString(measured));
            return result;
        }
        bytes = facts.bytes;
    }

    result.placement = planPlacement(request.sourcePath, bytes, request.now);

    if (!result.placement.intoTrash()) {
        // Решение FR-7 (крупный кэш, не влезает, дорогое кросс-томовое
        // копирование): байты убирает исполнитель плана и пишет об этом в
        // журнал, сервис только говорит «не в корзину».
        result.stage.directDelete = true;
        result.stage.path = request.sourcePath;
        result.stage.destination = request.sourcePath;
        core::LogFields fields;
        fields.push_back(core::logField("path", request.sourcePath));
        fields.push_back(core::logField("bytes", bytes));
        fields.push_back(core::logField("reason", reasonText(result.placement)));
        core::logInfo(kEventAdmit, kMsgAdmitDirect, std::move(fields));
        return result;
    }

    if (result.placement.plan.needsEviction()) {
        // Вытеснение ДО переноса: перенос увеличивает корзину, и вытеснение «по
        // факту» означало бы лишний копи-паста-цикл на самом дорогом шаге.
        result.eviction = enforceLimits(bytes, request.now, opts);
        if (result.eviction.cancelled) {
            result.stage.status = pl::TrashStatus::Cancelled;
            result.stage.path = request.sourcePath;
            return result;
        }
        // Решение после освобождения места может измениться в обе стороны: после
        // вытеснения крупный элемент способен стать обычным, а переполненная
        // корзина — влезающей.
        result.placement = planPlacement(request.sourcePath, bytes, request.now);
        if (!result.placement.intoTrash()) {
            result.stage.directDelete = true;
            result.stage.path = request.sourcePath;
            result.stage.destination = request.sourcePath;
            core::LogFields fields;
            fields.push_back(core::logField("path", request.sourcePath));
            fields.push_back(core::logField("bytes", bytes));
            fields.push_back(core::logField("reason", reasonText(result.placement)));
            core::logInfo(kEventAdmit, kMsgAdmitDirect, std::move(fields));
            return result;
        }
    }

    const bool ownsTx = request.openTxId.empty();
    std::string txId = request.openTxId;
    if (ownsTx) {
        txId = beginTransaction(request.now, opts);
        if (txId.empty()) {
            // Каталог не создался (нет прав, нет места): в корзину положить
            // нечем, и решение «не в корзину» — единственное честное. Причина
            // уже записана в журог внутри beginTransaction с путём и кодом.
            result.stage.status = pl::TrashStatus::IoError;
            result.stage.path = request.sourcePath;
            result.placement.plan.placement = core::TrashPlacement::DirectDelete;
            result.placement.plan.reason = "каталог транзакции не создан, в корзину положить нечем";
            return result;
        }
    }

    result.stage = stagePrepared(txId, request.sourcePath, opts);
    result.txId = txId;
    result.placed = result.stage.placed();

    if (ownsTx) {
        const core::TrashTransaction* open = ledger_.find(txId);
        if (result.stage.cancelled()) {
            // Отмена при уже перенесённых байтах: закрываем транзакцию манифестом,
            // а не сносим каталог — снос удалил бы файлы без возможности отмены
            // (правило 5). Пустую транзакцию сносим: каталог без манифеста — мусор.
            if (open != nullptr && open->items.empty()) {
                (void)dropEmptyOwned(txId, opts);
                result.txId.clear();
            } else if (open != nullptr) {
                result.commit = commit(txId, opts);
            }
        } else if (open != nullptr && open->items.empty()) {
            // Ни байта не легло — и при отказе переноса каталог транзакции пуст,
            // а пустой каталог без манифеста нельзя ни восстановить, ни посчитать
            // (правило 10). Оставить его значило бы копить по каталогу на каждый
            // прогон, где первый элемент не лёг: в следующем open() они читались
            // бы как битые и попадали в отчёт как чужой мусор. Причина отказа уже
            // записана внутри beginTransaction/stagePrepared.
            (void)dropEmptyOwned(txId, opts);
            result.txId.clear();
        }
    }

    if (result.placed) {
        core::LogFields fields;
        fields.push_back(core::logField("tx", txId));
        fields.push_back(core::logField("path", request.sourcePath));
        fields.push_back(core::logField("bytes", result.stage.bytes));
        fields.push_back(core::logField("crossVolume", result.stage.crossVolume));
        core::logInfo(kEventAdmit, kMsgAdmitPlaced, std::move(fields));
    }
    return result;
}

TrashService::AdmitAllResult TrashService::admitAll(const std::vector<AdmitRequest>& requests,
                                                    const platform::TrashOptions& trashOptions) {
    AdmitAllResult out;
    const platform::TrashOptions opts = effective(trashOptions);
    if (requests.empty()) return out;

    const std::size_t stride = cancelStride();
    out.staged.items.reserve(requests.size());

    for (std::size_t i = 0; i < requests.size(); ++i) {
        // Отмена проверяется раз в stride элементов (SPEC §6.4): чаще — лишнее
        // атомарное чтение на каждом файле, реже — секунды неотзывчивой кнопки.
        if ((i % stride) == 0 && opts.stop.stop_requested()) {
            out.staged.cancelled = true;
            out.staged.skipped = narrow(requests.size() - i);
            break;
        }

        AdmitRequest one = requests[i];
        one.openTxId = out.txId;
        AdmitResult admitted = admit(one, opts);

        if (!admitted.eviction.empty()) merge(out.eviction, std::move(admitted.eviction));
        if (admitted.placed && out.txId.empty()) out.txId = admitted.txId;
        if (admitted.stage.path.empty()) admitted.stage.path = one.sourcePath;
        out.staged.items.push_back(admitted.stage);

        if (admitted.placed) {
            ++out.staged.staged;
            out.staged.bytes = addSaturating(out.staged.bytes, admitted.stage.bytes);
            out.staged.files += admitted.stage.files;
            out.intoTrash = true;
            continue;
        }
        if (admitted.stage.directDelete) {
            ++out.staged.directDelete;
            continue;
        }
        ++out.staged.failed;
        if (admitted.stage.cancelled()) {
            out.staged.cancelled = true;
            out.staged.skipped = narrow(requests.size() - i - 1);
            break;
        }
    }

    out.staged.txId = out.txId;
    if (!out.txId.empty()) {
        // Закрытие даже при отмене: перенесённое должно остаться отменяемым,
        // иначе «отмена» означала бы безвозвратное удаление (правило 5).
        out.commit = commit(out.txId, opts);
    }

    core::LogFields fields;
    fields.push_back(core::logField("tx", out.txId));
    fields.push_back(core::logField("staged", out.staged.staged));
    fields.push_back(core::logField("directDelete", out.staged.directDelete));
    fields.push_back(core::logField("failed", out.staged.failed));
    fields.push_back(core::logField("skipped", out.staged.skipped));
    fields.push_back(core::logField("cancelled", out.staged.cancelled));
    fields.push_back(core::logField("bytes", out.staged.bytes));
    core::logInfo(kEventStage, out.staged.cancelled ? kMsgCancelled : "прогон очистки: перенос завершён",
                  std::move(fields));
    return out;
}

// ---------------------------------------------------------------------------
// Транзакция
// ---------------------------------------------------------------------------

std::string TrashService::beginTransaction(std::int64_t now, const platform::TrashOptions& trashOptions) {
    const platform::TrashOptions opts = effective(trashOptions);
    const std::string txId = ledger_.begin(now, options_.appVersion);

    std::string dir;
    const pl::TrashStatus status = pl::createTransactionDirectory(resolvedRoot(), txId, dir, opts);
    if (status != pl::TrashStatus::Ok) {
        // Запись в журнале откатывается: транзакция без каталога — это мусор в
        // учёте, который сам себя не покажет (восстанавливать нечего, лимит по
        // ней считать нечего).
        (void)ledger_.forget(txId);
        logFailure(options_.logFailures, kEventCommit, kMsgNoTxDir, dir.empty() ? resolvedRoot() : dir, 0,
                   pl::toString(status));
        return {};
    }
    return txId;
}

StageOutcome TrashService::stage(std::string_view txId, std::string_view sourcePath,
                                  const platform::TrashOptions& trashOptions) {
    return stagePrepared(txId, sourcePath, effective(trashOptions));
}

StageOutcome TrashService::stagePrepared(std::string_view txId, std::string_view sourcePath,
                                         const platform::TrashOptions& opts) {
    StageOutcome outcome;
    outcome.path = std::string(sourcePath);

    if (sourcePath.empty() || txId.empty() || !core::isValidTxId(txId)) {
        outcome.status = pl::TrashStatus::InvalidArgument;
        return outcome;
    }
    const core::TrashTransaction* tx = ledger_.find(txId);
    if (tx == nullptr) {
        outcome.status = pl::TrashStatus::NotFound;
        logFailure(options_.logFailures, kEventStage, kMsgStageUnknown, outcome.path, 0, pl::toString(outcome.status));
        return outcome;
    }
    if (tx->state != core::TrashTxState::Open) {
        // Дописывать закрытую транзакцию нельзя: манифест на диске уже описывает
        // другое содержимое, и элемент остался бы в корзине без записи обратной
        // ссылки — то есть стал бы мусором, который нельзя ни найти, ни вернуть.
        outcome.status = pl::TrashStatus::InvalidArgument;
        logFailure(options_.logFailures, kEventStage, kMsgNotOpen, outcome.path, 0, pl::toString(outcome.status));
        return outcome;
    }

    const std::string dir = dirFor(txId);
    if (dir.empty()) {
        outcome.status = pl::TrashStatus::InvalidArgument;
        return outcome;
    }

    pl::TrashStageRequest request;
    request.transactionDir = dir;
    request.originalPath = outcome.path;
    // Имя по номеру элемента, а не по исходному имени файла: в каталоге
    // транзакции не может быть разделителя, двоеточия (поток данных) или «..»,
    // а исходное имя приезжает с диска и правилам core не подчинено.
    request.payload = payloadName(tx->items.size());
    outcome.payload = request.payload;

    const pl::TrashStageResult staged = pl::stageTrashItem(request, opts);
    outcome.status = staged.transfer.status;
    outcome.win32Error = staged.transfer.win32Error;
    outcome.failedPath = staged.transfer.failedPath;
    outcome.destination = staged.transfer.destinationPath;
    outcome.bytes = staged.transfer.bytesMoved;
    outcome.files = staged.transfer.filesMoved;
    outcome.crossVolume = staged.transfer.crossVolume;

    if (staged.ok()) {
        // addItem бросает только на недопустимом payload, а он сгенерирован
        // здесь и проверен форматом: единственное исключение на весь модуль —
        // std::bad_alloc, и оно в обоих слоях одинаково.
        outcome.inLedger = ledger_.addItem(txId, staged.item);
    } else {
        noteFailure(txId);
        logFailure(options_.logFailures, kEventStage, kMsgStageFail,
                   outcome.failedPath.empty() ? outcome.path : outcome.failedPath, outcome.win32Error,
                   pl::toString(outcome.status));
    }
    return outcome;
}

StageSummary TrashService::stageAll(std::string_view txId, const std::vector<std::string>& sourcePaths,
                                    const platform::TrashOptions& trashOptions) {
    const platform::TrashOptions opts = effective(trashOptions);
    StageSummary summary;
    summary.txId = std::string(txId);
    summary.items.reserve(sourcePaths.size());
    const std::size_t stride = cancelStride();

    for (std::size_t i = 0; i < sourcePaths.size(); ++i) {
        if ((i % stride) == 0 && opts.stop.stop_requested()) {
            summary.cancelled = true;
            summary.skipped = narrow(sourcePaths.size() - i);
            break;
        }
        StageOutcome one = stagePrepared(txId, sourcePaths[i], opts);
        if (one.ok() && one.placed()) {
            ++summary.staged;
            summary.bytes = addSaturating(summary.bytes, one.bytes);
            summary.files += one.files;
        } else if (one.cancelled()) {
            // Отмена — не отказ: элемент не сносили и не переносили, исходник на
            // месте. Поэтому в failed он не попадает, а прогон просто останавливается.
            summary.cancelled = true;
            summary.skipped = narrow(sourcePaths.size() - i);
            summary.items.push_back(std::move(one));
            break;
        } else {
            ++summary.failed;
        }
        summary.items.push_back(std::move(one));
    }

    core::LogFields fields;
    fields.push_back(core::logField("tx", summary.txId));
    fields.push_back(core::logField("staged", summary.staged));
    fields.push_back(core::logField("failed", summary.failed));
    fields.push_back(core::logField("skipped", summary.skipped));
    fields.push_back(core::logField("cancelled", summary.cancelled));
    fields.push_back(core::logField("bytes", summary.bytes));
    core::logInfo(kEventStage, summary.cancelled ? kMsgCancelled : "перенос завершён", std::move(fields));
    return summary;
}

CommitResult TrashService::commit(std::string_view txId, const platform::TrashOptions& trashOptions) {
    CommitResult result;
    result.txId = std::string(txId);
    const platform::TrashOptions opts = effective(trashOptions);

    if (txId.empty() || !core::isValidTxId(txId)) {
        result.status = pl::TrashStatus::InvalidArgument;
        return result;
    }
    const core::TrashTransaction* view = ledger_.find(txId);
    if (view == nullptr) {
        result.status = pl::TrashStatus::NotFound;
        logFailure(options_.logFailures, kEventCommit, kMsgStageUnknown, std::string(txId), 0,
                   pl::toString(result.status));
        return result;
    }

    result.bytes = view->totalBytes();
    result.items = view->itemCount();
    result.manifestPath = manifestFor(txId);

    if (view->state == core::TrashTxState::Committed || view->state == core::TrashTxState::Undone) {
        // Повторный вызов: манифест уже записан, состояние не трогаем. Иначе
        // повторный commit после частичного восстановления затёр бы манифест
        // элементами, которых в корзине уже нет.
        result.manifestWritten = true;
        result.committed = view->state == core::TrashTxState::Committed;
        return result;
    }
    if (view->state != core::TrashTxState::Open) {
        // Collapsed: содержимое вытеснено, отменять нечего, а манифест с
        // элементами обещал бы обратное.
        result.status = pl::TrashStatus::InvalidArgument;
        logFailure(options_.logFailures, kEventCommit, "транзакция схлопнулась, закрывать нечего", std::string(txId), 0,
                   pl::toString(result.status));
        return result;
    }

    result.failedItems = failuresOf(txId);
    if (opts.stop.stop_requested()) {
        result.cancelled = true;
        result.status = pl::TrashStatus::Cancelled;
        return result;
    }

    // Копия, а не ссылка: состояние в файле и в памяти обязаны совпасть. Сначала
    // в копии выставляется Committed, потом пишется файл, и только после успеха
    // записи меняется журнал — иначе на диске остался бы манифест со
    // state=open, то есть «неотменяемая транзакция с содержимым».
    core::TrashTransaction snapshot = *view;
    snapshot.state = core::TrashTxState::Committed;
    const pl::TrashStatus status = pl::writeManifest(dirFor(txId), snapshot, opts);
    result.status = status;
    if (status != pl::TrashStatus::Ok) {
        logFailure(options_.logFailures, kEventCommit, kMsgManifest, result.manifestPath, 0, pl::toString(status));
        return result;
    }
    result.manifestWritten = true;
    result.committed = ledger_.commit(txId);
    failures_.erase(std::string(txId));

    core::LogFields fields;
    fields.push_back(core::logField("tx", result.txId));
    fields.push_back(core::logField("items", result.items));
    fields.push_back(core::logField("failed", result.failedItems));
    fields.push_back(core::logField("bytes", result.bytes));
    fields.push_back(core::logField("undoable", result.undoable()));
    core::logInfo(kEventCommit, "транзакция закрыта, манифест записан", std::move(fields));
    return result;
}

// Тихий снос транзакции, которой сервис владел и в которой не лежит ни байта.
// Отдельный путь вместо публичного abandon() намеренно: там снос — происшествие
// («недоделанная транзакция снята по вызову»), а здесь обычный исход «корзина
// недоступна» или «первый элемент не лёг», и warn на каждый такой прогон только
// шумит в журнале. Причина уже записана в beginTransaction/stagePrepared.
bool TrashService::dropEmptyOwned(std::string_view txId, const platform::TrashOptions& opts) {
    PurgeReport report;
    return purgeDirectory(txId, report, opts, /*keepCollapsed=*/false);
}

bool TrashService::abandon(std::string_view txId, const platform::TrashOptions& trashOptions) {
    const platform::TrashOptions opts = effective(trashOptions);
    if (txId.empty() || !core::isValidTxId(txId)) return false;
    const core::TrashTransaction* view = ledger_.find(txId);
    if (view == nullptr) return false;
    if (view->state != core::TrashTxState::Open) {
        // Закрытую транзакцию отменяют через restore: там файлы возвращаются по
        // местам, а здесь они были бы просто снесены.
        logFailure(options_.logFailures, kEventAbandon, kMsgNotOpen, std::string(txId), 0, "not-open");
        return false;
    }

    PurgeReport report;
    const bool purged = purgeDirectory(txId, report, opts, /*keepCollapsed=*/false);
    core::LogFields fields;
    fields.push_back(core::logField("tx", std::string(txId)));
    fields.push_back(core::logField("purged", purged));
    fields.push_back(core::logField("failed", report.failedCount));
    core::logWarn(kEventAbandon, "недоделанная транзакция снесена", std::move(fields));
    return purged;
}

// ---------------------------------------------------------------------------
// Очистка и лимиты
// ---------------------------------------------------------------------------

bool TrashService::purgeDirectory(std::string_view txId, PurgeReport& report, const platform::TrashOptions& opts,
                                 bool keepCollapsed) {
    if (!core::isValidTxId(txId)) return false;

    core::TrashTransaction* tx = ledger_.find(txId);
    const bool known = tx != nullptr;
    const std::uint64_t bytes = known ? tx->totalBytes() : 0u;
    const bool openTx = known && tx->state == core::TrashTxState::Open;

    const std::string dir = dirFor(txId);
    if (dir.empty()) {
        ++report.failedCount;
        report.failed.push_back(std::string(txId));
        return false;
    }

    const pl::TrashPurgeResult result = pl::purgeTransactionDirectory(dir, opts);
    report.removedBytes = addSaturating(report.removedBytes, result.removedBytes);
    report.removedDirs += result.removedDirs;
    if (!result.ok()) {
        // Снять не смогли — транзакция остаётся в журнале целой: забывать её
        // после неудачного сноса значило бы вычесть из лимита то, что всё ещё
        // лежит на диске.
        ++report.failedCount;
        report.failed.push_back(dir);
        logFailure(options_.logFailures, kEventPurge, kMsgPurgeFail, dir, result.win32Error, pl::toString(result.status));
        return false;
    }
    report.purged.push_back(dir);

    if (known) {
        if (openTx && keepCollapsed) {
            // Содержимое открытой транзакции снято (вытеснили по лимиту или
            // удалили по просьбе): элементы уходят из транзакции, состояние —
            // Collapsed. Сама запись остаётся, чтобы прогон увидел «отменять уже
            // нечего» (§7.2), а не исчезновение транзакции.
            tx->items.clear();
            core::TrashLedger::collapse(*tx, bytes);
            report.collapsedTxIds.push_back(std::string(txId));
        } else {
            ledger_.forget(txId);
        }
        failures_.erase(std::string(txId));
    }
    return true;
}

PurgeReport TrashService::executeEviction(const core::TrashEvictionPlan& plan, const platform::TrashOptions& opts) {
    PurgeReport report;
    if (plan.txIds.empty()) return report;

    // Жертв выбирает ядро (core::planEviction), здесь они снимаются по одной: это
    // длинная операция (2 ГБ), и её тоже должно можно отменить.
    for (const std::string& txId : plan.txIds) {
        if (opts.stop.stop_requested()) {
            report.cancelled = true;
            break;
        }
        (void)purgeDirectory(txId, report, opts, /*keepCollapsed=*/true);
    }

    core::LogFields fields;
    fields.push_back(core::logField("planned", narrow(plan.txIds.size())));
    fields.push_back(core::logField("purged", narrow(report.purged.size())));
    fields.push_back(core::logField("collapsed", narrow(report.collapsedTxIds.size())));
    fields.push_back(core::logField("failed", report.failedCount));
    fields.push_back(core::logField("removedBytes", report.removedBytes));
    fields.push_back(core::logField("cancelled", report.cancelled));
    core::logInfo(kEventEvict, report.cancelled ? kMsgCancelled : "вытеснение по лимитам выполнено", std::move(fields));
    return report;
}

PurgeReport TrashService::enforceLimits(std::uint64_t incomingBytes, std::int64_t now,
                                        const platform::TrashOptions& trashOptions) {
    const platform::TrashOptions opts = effective(trashOptions);
    // План целиком считает ядро: возрастной лимит уходит всегда, дальше — от
    // самой старой транзакции, пока хватает места. Сервис ничего не решает.
    const core::TrashEvictionPlan plan = core::planEviction(limits(), ledger_.info(), incomingBytes, now);
    return executeEviction(plan, opts);
}

PurgeReport TrashService::purgeExpired(std::int64_t now, const platform::TrashOptions& trashOptions) {
    const platform::TrashOptions opts = effective(trashOptions);
    // incomingBytes = 0 — «чистим только по возрасту»: вытеснять ради нуля нельзя,
    // иначе корзина опустела бы от одной проверки лимита.
    const core::TrashEvictionPlan plan = core::planEviction(limits(), ledger_.info(), 0, now);
    return executeEviction(plan, opts);
}

PurgeReport TrashService::purgeTransaction(std::string_view txId, const platform::TrashOptions& trashOptions) {
    PurgeReport report;
    const platform::TrashOptions opts = effective(trashOptions);
    if (txId.empty() || !core::isValidTxId(txId)) {
        logFailure(options_.logFailures, kEventPurge, "недопустимый идентификатор транзакции", std::string(txId), 0,
                   pl::toString(pl::TrashStatus::InvalidArgument));
        return report;
    }
    (void)purgeDirectory(txId, report, opts, /*keepCollapsed=*/true);
    return report;
}

PurgeReport TrashService::empty(const platform::TrashOptions& trashOptions) {
    const platform::TrashOptions opts = effective(trashOptions);
    PurgeReport report;

    // 1. Всё, что в журнале. Тем же путём, что и вытеснение по лимитам: план
    //    строится на «убрать всё», а снимает его платформа.
    core::TrashEvictionPlan plan;
    plan.txIds.reserve(ledger_.transactions().size());
    for (const core::TrashTransaction& tx : ledger_.transactions()) plan.txIds.push_back(tx.txId);
    if (!plan.txIds.empty()) merge(report, executeEviction(plan, opts));

    // 2. Каталоги без манифеста: в журнале их нет, но «очистить корзину» обязано
    //    убрать и их — иначе на диске остаётся ровно то, что нельзя ни показать,
    //    ни восстановить.
    if (!opts.stop.stop_requested()) merge(report, purgeBroken(opts));

    core::LogFields fields;
    fields.push_back(core::logField("purged", narrow(report.purged.size())));
    fields.push_back(core::logField("failed", report.failedCount));
    fields.push_back(core::logField("removedBytes", report.removedBytes));
    fields.push_back(core::logField("cancelled", report.cancelled));
    core::logInfo(kEventPurge, "корзина очищена", std::move(fields));
    return report;
}

// ---------------------------------------------------------------------------
// Восстановление
// ---------------------------------------------------------------------------

core::RestorePlan TrashService::planRestore(std::string_view txId, const std::vector<std::size_t>& selection,
                                            bool overwriteConflicts) const {
    const core::TrashTransaction* tx = ledger_.find(txId);
    if (tx == nullptr || !tx->undoable()) return {};

    core::RestoreOptions options;
    // Пользователь один раз решил судьбу занятых мест для всей операции; решение
    // по каждому элементу принимает core::planRestore (FR-7).
    options.overwriteConflicts = overwriteConflicts;
    // Проба занятости — настоящая, файловая: список элементов ничего не знает
    // о том, что появилось на диске после очистки.
    return core::planRestore(*tx, selection, pl::existsProbe(), options);
}

core::RestorePlan TrashService::planRestoreAll(std::string_view txId, bool overwriteConflicts) const {
    return planRestore(txId, {}, overwriteConflicts);
}

RestoreOutcome TrashService::restore(std::string_view txId, const core::RestorePlan& plan,
                                     const platform::TrashOptions& trashOptions) {
    RestoreOutcome outcome;
    outcome.txId = std::string(txId);
    const platform::TrashOptions opts = effective(trashOptions);

    if (txId.empty() || !core::isValidTxId(txId)) return outcome;
    const core::TrashTransaction* view = ledger_.find(txId);
    if (view == nullptr) {
        logFailure(options_.logFailures, kEventRestore, "транзакция не найдена в журнале", std::string(txId), 0, "not-found");
        return outcome;
    }
    if (plan.items.empty()) {
        // Возвращать нечего: всё в плане — Conflict (FR-7: не перезаписывать,
        // спросить) либо потерянное содержимое. Это не отказ платформы, но
        // элементы в транзакции остаются, и отчёт обязан назвать их число —
        // иначе интерфейс покажет «осталось 0» при транзакции на 1200 файлов.
        outcome.remainingItems = view->itemCount();
        return outcome;
    }
    if (!view->undoable()) {
        // Схлопнутая транзакция — отменять нечего, и это не отказ платформы:
        // интерфейс должен сказать «содержимое уже удалено», а не «ошибка».
        logFailure(options_.logFailures, kEventRestore, kMsgNotUndoable, std::string(txId), 0, "not-undoable");
        return outcome;
    }

    outcome.summary = pl::restoreTrashItems(dirFor(txId), plan, opts);

    // Возвращённые элементы убираются из транзакции: манифест не должен
    // продолжать обещать файлы, которых на диске уже нет, иначе следующий
    // запуск предложил бы восстановить несуществующее. Индексы результатов
    // платформы совпадают с порядком plan.items (restoreTrashItems идёт по
    // плану), а plan.items[index].index — это индекс в самой транзакции.
    std::vector<std::size_t> returned;
    const std::size_t common = std::min(outcome.summary.items.size(), plan.items.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (outcome.summary.items[i].ok()) returned.push_back(plan.items[i].index);
    }
    outcome.restoredItems = narrow(returned.size());

    if (returned.empty()) {
        outcome.remainingItems = view->itemCount();
        core::LogFields fields;
        fields.push_back(core::logField("tx", outcome.txId));
        fields.push_back(core::logField("conflicts", outcome.summary.conflictCount));
        fields.push_back(core::logField("missing", outcome.summary.missingCount));
        fields.push_back(core::logField("failed", outcome.summary.failedCount));
        core::logWarn(kEventRestore, "ни один элемент не возвращён: конфликты или потерянное содержимое", std::move(fields));
        return outcome;
    }

    core::TrashTransaction* tx = ledger_.find(txId);
    if (tx == nullptr) return outcome;  // не случиться: единственный писатель журнала — сервис
    // С конца по возрастанию: удаление элемента сдвигает индексы, и обратный
    // проход делает их недействительными ровно для тех, что уже убраны.
    std::sort(returned.begin(), returned.end(), std::greater<std::size_t>());
    for (const std::size_t index : returned) {
        if (index < tx->items.size()) tx->items.erase(tx->items.begin() + static_cast<std::ptrdiff_t>(index));
    }
    outcome.remainingItems = tx->itemCount();

    if (tx->items.empty()) {
        // Всё вернулось: транзакция помечается Undone, каталог снимается, запись
        // забывается (правило 9). Если снос не выйдет, запись останется с нулём
        // элементов, а её каталог при следующем open() попадёт в «осиротевшие» и
        // будет убран — то есть сбой не оставляет мусор навсегда.
        (void)ledger_.markUndone(txId);
        PurgeReport dropped;
        outcome.closed = purgeDirectory(txId, dropped, opts, /*keepCollapsed=*/false);
    } else {
        // Частичное восстановление: манифест перезаписывается на оставшееся.
        outcome.manifestRewritten = rewriteManifest(*tx, opts);
    }

    core::LogFields fields;
    fields.push_back(core::logField("tx", outcome.txId));
    fields.push_back(core::logField("restored", outcome.restoredItems));
    fields.push_back(core::logField("remaining", outcome.remainingItems));
    fields.push_back(core::logField("conflicts", outcome.summary.conflictCount));
    fields.push_back(core::logField("missing", outcome.summary.missingCount));
    fields.push_back(core::logField("failed", outcome.summary.failedCount));
    fields.push_back(core::logField("closed", outcome.closed));
    fields.push_back(core::logField("restoredBytes", outcome.summary.restoredBytes));
    core::logInfo(kEventRestore, "восстановление выполнено", std::move(fields));
    return outcome;
}

RestoreOutcome TrashService::restoreAll(std::string_view txId, bool overwriteConflicts,
                                        const platform::TrashOptions& trashOptions) {
    return restore(txId, planRestoreAll(txId, overwriteConflicts), trashOptions);
}

bool TrashService::rewriteManifest(const core::TrashTransaction& tx, const platform::TrashOptions& opts) {
    const std::string dir = dirFor(tx.txId);
    if (dir.empty()) return false;
    const pl::TrashStatus status = pl::writeManifest(dir, tx, opts);
    if (status != pl::TrashStatus::Ok) {
        logFailure(options_.logFailures, kEventRestore, kMsgRewrite, core::manifestPath(dir), 0, pl::toString(status));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Отчёт
// ---------------------------------------------------------------------------

Snapshot TrashService::snapshot(const platform::TrashOptions& trashOptions) const {
    const platform::TrashOptions opts = effective(trashOptions);
    Snapshot snap;

    snap.usage = ledger_.usage();
    snap.transactionCount = narrow(ledger_.transactions().size());
    snap.itemCount = narrow(snap.usage.itemCount);
    snap.brokenDirs = narrow(brokenDirs_.size());
    for (const core::TrashTransaction& tx : ledger_.transactions()) {
        snap.purgedBytes = addSaturating(snap.purgedBytes, tx.purgedBytes);
    }

    const std::string root = resolvedRoot();
    // Учёт идёт по манифестам, а этот обход — по диску. Расхождение означает
    // либо сорванное копирование, либо чужой каталог в корне, и оно обязано быть
    // видно в отчёте, а не прятаться за «посчитанным по манифестам».
    const pl::TrashPurgeResult measured = pl::measureTrashRoot(root, opts);
    snap.onDiskBytes = measured.removedBytes;
    snap.freeBytes = pl::freeBytesOfPath(root);
    snap.summary = core::trashSummary(snap.usage, limits());
    if (snap.driftBytes() != 0) {
        snap.summary += "; на диске сверх учёта: " + core::formatBytes(snap.driftBytes(), 1, true);
    }
    if (snap.brokenDirs != 0) {
        snap.summary += "; каталогов без манифеста: " + core::formatCount(snap.brokenDirs);
    }
    snap.describe = describe(snap);
    return snap;
}

std::string TrashService::summary() const { return core::trashSummary(ledger_.usage(), limits()); }

// ---------------------------------------------------------------------------
// Тексты
// ---------------------------------------------------------------------------

std::string describe(const PurgeReport& report) {
    std::string out = "purged=";
    out += std::to_string(report.purged.size());
    out += " collapsed=";
    out += std::to_string(report.collapsedTxIds.size());
    out += " failed=";
    out += std::to_string(report.failedCount);
    out += " removedBytes=";
    out += core::formatBytes(report.removedBytes, 1, true);
    out += " cancelled=";
    out += report.cancelled ? "true" : "false";
    return out;
}

std::string describe(const OpenReport& report) {
    std::string out = "root=";
    out += report.root.empty() ? "<unknown>" : report.root;
    out += " adopted=";
    out += std::to_string(report.adopted);
    out += " stale=";
    out += std::to_string(report.staleDirs);
    out += " broken=";
    out += std::to_string(report.brokenDirs);
    out += " status=";
    out += pl::toString(report.status);
    return out;
}

std::string describe(const StageSummary& summary) {
    std::string out = "tx=";
    out += summary.txId.empty() ? "-" : summary.txId;
    out += " staged=";
    out += std::to_string(summary.staged);
    out += " directDelete=";
    out += std::to_string(summary.directDelete);
    out += " failed=";
    out += std::to_string(summary.failed);
    out += " skipped=";
    out += std::to_string(summary.skipped);
    out += " files=";
    out += std::to_string(summary.files);
    out += " bytes=";
    out += core::formatBytes(summary.bytes, 1, true);
    out += " cancelled=";
    out += summary.cancelled ? "true" : "false";
    return out;
}

std::string describe(const CommitResult& result) {
    std::string out = "tx=";
    out += result.txId.empty() ? "-" : result.txId;
    out += " items=";
    out += std::to_string(result.items);
    out += " failed=";
    out += std::to_string(result.failedItems);
    out += " bytes=";
    out += core::formatBytes(result.bytes, 1, true);
    out += " undoable=";
    out += result.undoable() ? "true" : "false";
    out += " status=";
    out += pl::toString(result.status);
    return out;
}

std::string describe(const Snapshot& snapshot) {
    std::string out = "transactions=";
    out += std::to_string(snapshot.transactionCount);
    out += " items=";
    out += std::to_string(snapshot.itemCount);
    out += " accounted=";
    out += core::formatBytes(snapshot.usage.totalBytes, 1, true);
    out += " onDisk=";
    out += core::formatBytes(snapshot.onDiskBytes, 1, true);
    out += " drift=";
    out += core::formatBytes(snapshot.driftBytes(), 1, true);
    out += " broken=";
    out += std::to_string(snapshot.brokenDirs);
    out += " purged=";
    out += core::formatBytes(snapshot.purgedBytes, 1, true);
    out += " free=";
    out += core::formatBytes(snapshot.freeBytes, 1, true);
    return out;
}

std::string reasonText(const Placement& placement) {
    if (placement.plan.reason == nullptr) return "решение не принято";
    return placement.plan.reason;
}

}  // namespace mrproper::engine::trash_service
