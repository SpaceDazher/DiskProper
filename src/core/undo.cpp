// Отмена очистки: корзина приложения, лимиты, конфликты и восстановление.
// Реализация части SPEC §4 FR-7. Файловой системы здесь нет: платформа (engine +
// platform::vfs) приносит снимок состояния и выполняет план, ядро считает решения.
#include "undo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "json.hpp"

namespace mrproper::core {
namespace {

// Точности double в JSON хватает для байтов и unix-секунд до 2^53 (9 ПБ и
// 285 миллионов лет) — но диапазон проверять всё равно обязательно, иначе битый
// манифест превратится в мусорные числа.
constexpr double kMaxExactInteger = 9007199254740992.0;  // 2^53

[[noreturn]] void fail(const std::string& origin, const std::string& message) {
    throw UndoError(origin + ": " + message);
}

std::string missingField(const char* key) { return "нет обязательного поля \"" + std::string(key) + "\""; }

const json::Value* member(const json::Value& node, const char* key) { return node.find(key); }

std::string stringField(const json::Value& node, const char* key, const std::string& origin, bool required,
                        const std::string& fallback = {}) {
    const json::Value* value = member(node, key);
    if (value == nullptr || value->isNull()) {
        if (required) fail(origin, missingField(key));
        return fallback;
    }
    if (!value->isString()) {
        fail(origin, "поле \"" + std::string(key) + "\" должно быть строкой");
    }
    return value->asString();
}

std::int64_t intField(const json::Value& node, const char* key, const std::string& origin, bool required,
                      std::int64_t fallback = 0) {
    const json::Value* value = member(node, key);
    if (value == nullptr || value->isNull()) {
        if (required) fail(origin, missingField(key));
        return fallback;
    }
    if (!value->isNumber()) {
        fail(origin, "поле \"" + std::string(key) + "\" должно быть числом");
    }
    const double raw = value->asNumber();
    if (!std::isfinite(raw) || raw != std::floor(raw)) {
        fail(origin, "поле \"" + std::string(key) + "\" должно быть целым числом");
    }
    if (raw < -kMaxExactInteger || raw > kMaxExactInteger) {
        fail(origin, "поле \"" + std::string(key) + "\" вне диапазона int64");
    }
    return static_cast<std::int64_t>(raw);
}

std::uint64_t uintField(const json::Value& node, const char* key, const std::string& origin, bool required,
                        std::uint64_t fallback = 0) {
    const json::Value* value = member(node, key);
    if (value == nullptr || value->isNull()) {
        if (required) fail(origin, missingField(key));
        return fallback;
    }
    if (!value->isNumber()) {
        fail(origin, "поле \"" + std::string(key) + "\" должно быть числом");
    }
    const double raw = value->asNumber();
    if (!std::isfinite(raw) || raw != std::floor(raw) || raw < 0.0) {
        fail(origin, "поле \"" + std::string(key) + "\" должно быть неотрицательным целым числом");
    }
    if (raw > kMaxExactInteger) {
        fail(origin, "поле \"" + std::string(key) + "\" вне диапазона uint64");
    }
    return static_cast<std::uint64_t>(raw);
}

std::uint32_t uint32Field(const json::Value& node, const char* key, const std::string& origin,
                          std::uint32_t fallback = 0) {
    const std::uint64_t raw = uintField(node, key, origin, false, fallback);
    if (raw > 0xFFFFFFFFull) {
        fail(origin, "поле \"" + std::string(key) + "\" вне диапазона uint32");
    }
    return static_cast<std::uint32_t>(raw);
}

bool boolField(const json::Value& node, const char* key, const std::string& origin, bool fallback = false) {
    const json::Value* value = member(node, key);
    if (value == nullptr || value->isNull()) return fallback;
    if (!value->isBool()) {
        fail(origin, "поле \"" + std::string(key) + "\" должно быть true или false");
    }
    return value->asBool();
}

bool hasControlChars(std::string_view text) {
    for (const char c : text) {
        if (c == '\0' || static_cast<unsigned char>(c) < 0x20) return true;
    }
    return false;
}

// Символы идентификатора транзакции: он же имя каталога, поэтому никаких
// разделителей, точек-родителей и пробелов.
bool isValidTxId(std::string_view txId) {
    if (txId.empty() || txId.size() > 64) return false;
    for (const char c : txId) {
        const bool okChar = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
        if (!okChar) return false;
    }
    return txId != "." && txId != "..";
}

bool isSeparator(char c) { return c == '\\' || c == '/'; }

std::string_view fileNameOf(std::string_view path) {
    for (std::size_t i = path.size(); i > 0; --i) {
        if (isSeparator(path[i - 1])) return path.substr(i);
    }
    return path;
}

// Суффикс вставляется перед расширением: file.txt -> file.mrproper-restore.txt,
// чтобы расширение осталось узнаваемым для приложений.
std::string applySuffix(std::string_view path, std::string_view suffix) {
    const std::string_view name = fileNameOf(path);
    const std::size_t dot = name.rfind('.');
    // Точка в начале имени — это скрытый файл (".config"), а не расширение.
    if (dot == std::string_view::npos || dot == 0) return std::string(path) + std::string(suffix);

    const std::size_t nameOffset = path.size() - name.size();
    std::string out(path.substr(0, nameOffset));
    out.append(name.substr(0, dot));
    out.append(suffix);
    out.append(name.substr(dot));
    return out;
}

std::string questionText(const std::string& path, RestoreConflict conflict) {
    switch (conflict) {
        case RestoreConflict::TargetExists:
            return "На месте уже есть файл: " + path + ". Перезаписать, восстановить рядом или пропустить?";
        case RestoreConflict::TargetTypeMismatch:
            return "На месте объект другого типа: " + path + ". Перезаписать или пропустить?";
        case RestoreConflict::TargetUnknown:
            return "Не удалось проверить цель: " + path + ". Восстановить не перезаписывая или пропустить?";
        case RestoreConflict::TargetLocked:
            return "Файл занят другим приложением: " + path + ". Закройте приложение или пропустите.";
        case RestoreConflict::ParentMissing:
            return "Родительского каталога нет: " + path + ". Каталог будет создан.";
        case RestoreConflict::NotEnoughSpace:
            return "На целевом томе не хватает места для " + path + ". Пропустить элемент.";
        case RestoreConflict::AlreadyRestored:
            return "Элемент уже восстановлен: " + path + ".";
        case RestoreConflict::MissingInTrash:
            return "Содержимое утрачено, в корзине осталась только запись: " + path + ".";
        case RestoreConflict::None:
            break;
    }
    return std::string();
}

// Конфликт, который восстановление не разрешает само. ParentMissing —
// предупреждение: каталог платформа создаст. Остальное требует решения.
bool isBlocking(RestoreConflict conflict) {
    switch (conflict) {
        case RestoreConflict::None:
        case RestoreConflict::ParentMissing:
            return false;
        case RestoreConflict::TargetExists:
        case RestoreConflict::TargetTypeMismatch:
        case RestoreConflict::TargetUnknown:
        case RestoreConflict::TargetLocked:
        case RestoreConflict::NotEnoughSpace:
        case RestoreConflict::AlreadyRestored:
        case RestoreConflict::MissingInTrash:
            return true;
    }
    return true;
}

// Конфликт, который лечится перезаписью. Занятый файл, нехватка места,
// неизвестная цель, «уже восстановлено» и «утрачено» перезаписью не лечатся.
bool isOverwritable(RestoreConflict conflict) {
    return conflict == RestoreConflict::TargetExists || conflict == RestoreConflict::TargetTypeMismatch;
}

// Приоритет, когда мешает сразу несколько причин: сначала те, что делают
// бессмысленным любой выбор (утрачено, уже возвращено), затем неустранимые
// физически (занято, нет места), затем те, где есть выбор (существует файл).
RestoreConflict worstConflict(RestoreConflict candidate, RestoreConflict current) {
    const auto rank = [](RestoreConflict conflict) {
        switch (conflict) {
            case RestoreConflict::None: return 0;
            case RestoreConflict::ParentMissing: return 1;
            case RestoreConflict::TargetExists: return 2;
            case RestoreConflict::TargetTypeMismatch: return 3;
            case RestoreConflict::TargetUnknown: return 4;
            case RestoreConflict::NotEnoughSpace: return 5;
            case RestoreConflict::TargetLocked: return 6;
            case RestoreConflict::AlreadyRestored: return 7;
            case RestoreConflict::MissingInTrash: return 8;
        }
        return 0;
    };
    return rank(candidate) > rank(current) ? candidate : current;
}

bool isExpired(const UndoLimits& limits, const UndoTransaction& tx, std::int64_t nowUnix) {
    if (limits.maxAgeSeconds <= 0) return false;  // возрастной лимит выключен
    if (tx.createdUnix <= 0) return false;         // время неизвестно: чужое не выкидываем
    return nowUnix - tx.createdUnix > limits.maxAgeSeconds;
}

std::uint64_t additionOverflow(std::uint64_t a, std::uint64_t b) {
    if (a > 0xFFFFFFFFFFFFFFFFull - b) return 0xFFFFFFFFFFFFFFFFull;
    return a + b;
}

std::string formatSeconds(std::uint64_t seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu с", static_cast<unsigned long long>(seconds));
    return std::string(buf);
}

// Порядок вытеснения: сперва просроченное по возрасту (его всё равно чистить,
// отмена не страдает), внутри групп — от самых старых к новым, при равенстве по
// времени — по txId, чтобы план был воспроизводим.
std::vector<const UndoTransaction*> sortedForEviction(const UndoLimits& limits, const UndoTrashState& usage,
                                                        std::int64_t nowUnix) {
    std::vector<const UndoTransaction*> ordered;
    ordered.reserve(usage.transactions.size());
    for (const auto& tx : usage.transactions) ordered.push_back(&tx);

    std::sort(ordered.begin(), ordered.end(), [&limits, nowUnix](const UndoTransaction* a, const UndoTransaction* b) {
        const bool expiredA = isExpired(limits, *a, nowUnix);
        const bool expiredB = isExpired(limits, *b, nowUnix);
        if (expiredA != expiredB) return expiredA;
        if (a->createdUnix != b->createdUnix) return a->createdUnix < b->createdUnix;
        return a->txId < b->txId;
    });
    return ordered;
}

// Пересчёт агрегатов плана после любого изменения записей: агрегаты в плане
// должны быть производными, иначе «пропущено» и «восстановлено» разъезжаются.
void finalizePlan(UndoPlan& plan) {
    plan.bytesPlanned = 0;
    plan.bytesSkipped = 0;
    plan.conflictCount = 0;
    plan.skippedCount = 0;
    plan.requiresUserDecision = false;
    plan.questions.clear();

    for (const auto& entry : plan.entries) {
        if (entry.action == RestoreDecision::Skip) {
            plan.bytesSkipped = additionOverflow(plan.bytesSkipped, entry.sizeBytes);
            ++plan.skippedCount;
        } else {
            plan.bytesPlanned = additionOverflow(plan.bytesPlanned, entry.sizeBytes);
        }
        if (entry.conflict != RestoreConflict::None) ++plan.conflictCount;
        if (entry.needsUserDecision) {
            plan.requiresUserDecision = true;
            const std::string path = entry.targetPath.empty() ? entry.originalPath : entry.targetPath;
            plan.questions.push_back(questionText(path, entry.conflict));
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Транзакция
// ---------------------------------------------------------------------------

std::uint64_t UndoTransaction::totalBytes() const {
    std::uint64_t sum = 0;
    for (const auto& entry : entries) sum = additionOverflow(sum, entry.sizeBytes);
    return sum;
}

std::uint64_t UndoTransaction::restorableBytes() const {
    std::uint64_t sum = 0;
    for (const auto& entry : entries) {
        if (!entry.restored) sum = additionOverflow(sum, entry.sizeBytes);
    }
    return sum;
}

std::size_t UndoTransaction::restorableCount() const {
    std::size_t count = 0;
    for (const auto& entry : entries) {
        if (!entry.restored) ++count;
    }
    return count;
}

TrashEntry* UndoTransaction::find(std::string_view originalPath) {
    for (auto& entry : entries) {
        if (entry.originalPath == originalPath) return &entry;
    }
    return nullptr;
}

const TrashEntry* UndoTransaction::find(std::string_view originalPath) const {
    for (const auto& entry : entries) {
        if (entry.originalPath == originalPath) return &entry;
    }
    return nullptr;
}

bool undoAvailable(const UndoTransaction& tx) { return !tx.collapsed && tx.restorableCount() > 0; }

UndoTransaction makeTrashTransaction(std::string txId, std::string appVersion, std::int64_t nowUnix) {
    UndoTransaction tx;
    tx.schemaVersion = kUndoManifestSchema;
    tx.txId = std::move(txId);
    tx.appVersion = std::move(appVersion);
    tx.createdUnix = nowUnix;
    tx.collapsed = false;
    return tx;
}

// ---------------------------------------------------------------------------
// Пути и манифест
// ---------------------------------------------------------------------------

std::string concatPath(std::string_view base, std::string_view leaf) {
    if (base.empty()) return std::string(leaf);
    if (leaf.empty()) return std::string(base);
    std::string out(base);
    if (!isSeparator(out.back()) && !isSeparator(leaf.front())) out.push_back('\\');
    out.append(leaf);
    return out;
}

std::string trashTransactionDir(std::string_view trashRoot, std::string_view txId) {
    return concatPath(trashRoot, txId);
}

std::string trashManifestPath(std::string_view trashRoot, std::string_view txId) {
    return concatPath(trashTransactionDir(trashRoot, txId), kUndoManifestFileName);
}

bool isSafeStoredPath(std::string_view storedPath) {
    if (storedPath.empty()) return false;
    if (hasControlChars(storedPath)) return false;
    if (isSeparator(storedPath.front())) return false;               // абсолютный путь
    if (storedPath.find(':') != std::string_view::npos) return false; // «C:…», потоки NTFS, ADS

    std::size_t start = 0;
    while (start <= storedPath.size()) {
        const std::size_t slash = storedPath.find_first_of("/\\", start);
        const std::string_view part =
            storedPath.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part == "..") return false;  // выход за пределы каталога транзакции
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string serializeTrashManifest(const UndoTransaction& tx, int indent) {
    std::vector<json::Value> entries;
    entries.reserve(tx.entries.size());
    for (const auto& entry : tx.entries) {
        entries.push_back(json::Value::object({
            {"originalPath", json::Value(entry.originalPath)},
            {"storedPath", json::Value(entry.storedPath)},
            {"isDirectory", json::Value(entry.isDirectory)},
            {"sizeBytes", json::Value(static_cast<double>(entry.sizeBytes))},
            {"fileCount", json::Value(static_cast<double>(entry.fileCount))},
            {"modifiedUnix", json::Value(static_cast<double>(entry.modifiedUnix))},
            {"createdUnix", json::Value(static_cast<double>(entry.createdUnix))},
            {"aclSddl", json::Value(entry.aclSddl)},
            {"volumeGuidPath", json::Value(entry.volumeGuidPath)},
            {"restored", json::Value(entry.restored)},
            {"restoredUnix", json::Value(static_cast<double>(entry.restoredUnix))},
        }));
    }

    const json::Value root = json::Value::object({
        {"schema", json::Value(static_cast<double>(tx.schemaVersion))},
        {"txId", json::Value(tx.txId)},
        {"appVersion", json::Value(tx.appVersion)},
        {"createdUnix", json::Value(static_cast<double>(tx.createdUnix))},
        {"collapsed", json::Value(tx.collapsed)},
        {"entries", json::Value::array(std::move(entries))},
    });
    return root.dump(indent);
}

UndoTransaction parseUndoManifest(std::string_view text, const std::string& origin) {
    json::Value root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        throw UndoError(origin + ": манифест не разобран: " + e.what());
    }
    if (!root.isObject()) fail(origin, "манифест должен быть объектом JSON");

    UndoTransaction tx;
    const std::int64_t schema = intField(root, "schema", origin, true, kUndoManifestSchema);
    if (schema != kUndoManifestSchema) {
        fail(origin, "неизвестная версия схемы манифеста " + std::to_string(schema) + " (ожидается " +
                         std::to_string(kUndoManifestSchema) + ")");
    }
    tx.schemaVersion = static_cast<int>(schema);

    tx.txId = stringField(root, "txId", origin, true);
    if (!isValidTxId(tx.txId)) {
        fail(origin, "поле \"txId\" должно быть непустым идентификатором из букв, цифр, '.', '_' и '-'");
    }
    tx.appVersion = stringField(root, "appVersion", origin, false);
    tx.createdUnix = intField(root, "createdUnix", origin, false, 0);
    tx.collapsed = boolField(root, "collapsed", origin, false);

    const json::Value* entriesNode = member(root, "entries");
    if (entriesNode == nullptr || entriesNode->isNull()) return tx;
    if (!entriesNode->isArray()) fail(origin, "поле \"entries\" должно быть массивом");

    tx.entries.reserve(entriesNode->items().size());
    for (const auto& item : entriesNode->items()) {
        if (!item.isObject()) fail(origin, "каждый элемент \"entries\" должен быть объектом");

        TrashEntry entry;
        entry.originalPath = stringField(item, "originalPath", origin, true);
        if (hasControlChars(entry.originalPath)) {
            fail(origin, "поле \"originalPath\" содержит управляющие символы");
        }
        entry.storedPath = stringField(item, "storedPath", origin, true);
        if (!isSafeStoredPath(entry.storedPath)) {
            fail(origin,
                 "поле \"storedPath\" должно быть относительным путём внутри каталога транзакции: \"" +
                     entry.storedPath + "\"");
        }
        entry.isDirectory = boolField(item, "isDirectory", origin, false);
        entry.sizeBytes = uintField(item, "sizeBytes", origin, false, 0);
        entry.fileCount = uint32Field(item, "fileCount", origin, 0);
        entry.modifiedUnix = intField(item, "modifiedUnix", origin, false, 0);
        entry.createdUnix = intField(item, "createdUnix", origin, false, 0);
        entry.aclSddl = stringField(item, "aclSddl", origin, false);
        entry.volumeGuidPath = stringField(item, "volumeGuidPath", origin, false);
        entry.restored = boolField(item, "restored", origin, false);
        entry.restoredUnix = intField(item, "restoredUnix", origin, false, 0);
        if (entry.createdUnix == 0) entry.createdUnix = tx.createdUnix;
        tx.entries.push_back(std::move(entry));
    }
    return tx;
}

// ---------------------------------------------------------------------------
// Лимиты и вытеснение
// ---------------------------------------------------------------------------

const char* toString(TrashDisposition disposition) {
    switch (disposition) {
        case TrashDisposition::ToTrash: return "to-trash";
        case TrashDisposition::DirectDelete: return "direct-delete";
        case TrashDisposition::NoSpace: return "no-space";
    }
    return "no-space";
}

std::vector<std::string> orderForEviction(const UndoLimits& limits, const UndoTrashState& usage, std::int64_t nowUnix) {
    const std::vector<const UndoTransaction*> ordered = sortedForEviction(limits, usage, nowUnix);
    std::vector<std::string> order;
    order.reserve(ordered.size());
    for (const UndoTransaction* tx : ordered) order.push_back(tx->txId);
    return order;
}

TrashPurgePlan planTrashPurge(const UndoLimits& limits, const UndoTrashState& usage, std::uint64_t neededBytes,
                              std::int64_t nowUnix) {
    TrashPurgePlan plan;
    const std::vector<const UndoTransaction*> ordered = sortedForEviction(limits, usage, nowUnix);

    std::uint64_t expiredBytes = 0;
    for (const UndoTransaction* tx : ordered) {
        if (!isExpired(limits, *tx, nowUnix)) continue;
        plan.expiredTxIds.push_back(tx->txId);
        expiredBytes = additionOverflow(expiredBytes, tx->totalBytes());
    }

    // Бюджет влезания — место после чистки просроченного. usage.totalBytes может не
    // совпадать с суммой транзакций (рядом лежат чужие файлы), поэтому считаем от
    // измеренного платформой значения.
    const std::uint64_t freeAfterExpired = usage.totalBytes > expiredBytes ? usage.totalBytes - expiredBytes : 0;
    const std::uint64_t deficit = neededBytes > freeAfterExpired ? neededBytes - freeAfterExpired : 0;
    plan.bytesNeeded = deficit;
    plan.bytesReclaimed = expiredBytes;

    if (deficit == 0) {
        plan.canAdmit = true;
        return plan;
    }

    std::uint64_t evictable = 0;
    for (const UndoTransaction* tx : ordered) {
        if (evictable >= deficit) break;
        if (isExpired(limits, *tx, nowUnix)) continue;  // уже учтено как просроченное
        plan.overflowTxIds.push_back(tx->txId);
        evictable = additionOverflow(evictable, tx->totalBytes());
    }

    plan.canAdmit = evictable >= deficit;
    plan.bytesReclaimed = additionOverflow(expiredBytes, evictable);
    return plan;
}

TrashAdmission admitToTrash(const UndoLimits& limits, const UndoTrashState& usage, const TrashRequest& request,
                            std::int64_t nowUnix) {
    TrashAdmission admission;
    admission.bytesAccepted = request.sizeBytes;

    const std::string name = request.displayName.empty() ? std::string("элемент") : request.displayName;

    // Крупный кэш в корзину не кладём: она растёт, а толку от возможности отменить
    // удаление сотни мегабайт кэша нет (SPEC §4 FR-7).
    if (request.sizeBytes > limits.largeItemThresholdBytes) {
        admission.disposition = TrashDisposition::DirectDelete;
        admission.bytesAccepted = 0;
        admission.note = "Размер " + name + " превышает порог корзины — прямое удаление с записью в журнал";
        return admission;
    }

    const TrashPurgePlan purge = planTrashPurge(limits, usage, request.sizeBytes, nowUnix);
    const std::uint64_t projected = additionOverflow(usage.totalBytes, request.sizeBytes);
    if (projected <= limits.maxTotalBytes) {
        admission.disposition = TrashDisposition::ToTrash;
        admission.bytesAccepted = request.sizeBytes;
        admission.bytesToEvict = 0;
        admission.note = "Помещается в корзину приложения, отмена доступна";
        return admission;
    }

    admission.bytesOverLimit = projected - limits.maxTotalBytes;
    admission.evictTxIds = purge.overflowTxIds;
    admission.bytesToEvict = purge.bytesNeeded;
    admission.evictExpiredOnly = purge.overflowTxIds.empty() && !purge.expiredTxIds.empty();

    if (!purge.canAdmit) {
        admission.disposition = TrashDisposition::NoSpace;
        admission.bytesAccepted = 0;
        admission.note = "В корзине не хватает места даже после вытеснения старых транзакций (" + name +
                          ") — прямое удаление с записью в журнал";
        return admission;
    }

    admission.disposition = TrashDisposition::ToTrash;
    admission.bytesAccepted = request.sizeBytes;
    if (purge.overflowTxIds.empty()) {
        admission.note = "Помещается в корзину после чистки просроченного, отмена доступна";
        return admission;
    }
    admission.note = "Помещается в корзину после вытеснения " + std::to_string(purge.overflowTxIds.size()) +
                     " старых транзакций";
    return admission;
}

// ---------------------------------------------------------------------------
// Кросс-томовое перемещение = копирование
// ---------------------------------------------------------------------------

const char* toString(RestoreConflict conflict) {
    switch (conflict) {
        case RestoreConflict::None: return "none";
        case RestoreConflict::TargetExists: return "target-exists";
        case RestoreConflict::TargetTypeMismatch: return "target-type-mismatch";
        case RestoreConflict::TargetUnknown: return "target-unknown";
        case RestoreConflict::TargetLocked: return "target-locked";
        case RestoreConflict::ParentMissing: return "parent-missing";
        case RestoreConflict::NotEnoughSpace: return "not-enough-space";
        case RestoreConflict::AlreadyRestored: return "already-restored";
        case RestoreConflict::MissingInTrash: return "missing-in-trash";
    }
    return "none";
}

const char* toString(ConflictPolicy policy) {
    switch (policy) {
        case ConflictPolicy::Ask: return "ask";
        case ConflictPolicy::Skip: return "skip";
        case ConflictPolicy::Overwrite: return "overwrite";
        case ConflictPolicy::Rename: return "rename";
    }
    return "ask";
}

const char* toString(RestoreDecision action) {
    switch (action) {
        case RestoreDecision::Restore: return "restore";
        case RestoreDecision::Skip: return "skip";
        case RestoreDecision::Rename: return "rename";
        case RestoreDecision::Overwrite: return "overwrite";
    }
    return "skip";
}

CopyEstimate estimateTransfer(std::uint64_t bytes, std::string_view fromVolume, std::string_view toVolume,
                              std::uint64_t throughputBytesPerSec) {
    CopyEstimate estimate;
    estimate.bytes = bytes;
    estimate.volumeKnown = !fromVolume.empty() && !toVolume.empty();

    if (bytes == 0) return estimate;
    if (!estimate.volumeKnown) {
        // Том неизвестен: цена неопределима. Но перемещение между томами всегда
        // означает копирование, поэтому предупреждение остаётся.
        estimate.crossVolume = true;
        estimate.slowEnoughToWarn = true;
        return estimate;
    }

    estimate.crossVolume = fromVolume != toVolume;
    if (!estimate.crossVolume) return estimate;  // один том: перемещение, а не копирование

    estimate.throughputBytesPerSec =
        throughputBytesPerSec != 0 ? throughputBytesPerSec : kDefaultCopyThroughputBytesPerSec;
    estimate.seconds = (bytes + estimate.throughputBytesPerSec - 1) / estimate.throughputBytesPerSec;
    estimate.slowEnoughToWarn = estimate.seconds >= kSlowCopySeconds;
    estimate.recommendsDirectDelete = bytes >= kLargeTransactionBytes;
    return estimate;
}

CopyEstimate estimateRestoreCost(const UndoTransaction& tx, std::string_view trashVolume,
                                  std::uint64_t throughputBytesPerSec) {
    // Считаем по возвращаемым байтам. Если хотя бы один элемент лежит на другом
    // томе, копирование будет для всей операции — а не только для части.
    std::uint64_t bytes = 0;
    bool crossVolume = false;
    bool volumeKnown = !trashVolume.empty();
    for (const auto& entry : tx.entries) {
        if (entry.restored) continue;
        bytes = additionOverflow(bytes, entry.sizeBytes);
        if (entry.volumeGuidPath.empty()) {
            volumeKnown = false;
        } else if (!trashVolume.empty() && entry.volumeGuidPath != trashVolume) {
            crossVolume = true;
        }
    }

    CopyEstimate estimate;
    estimate.bytes = bytes;
    estimate.volumeKnown = volumeKnown;
    estimate.crossVolume = crossVolume || !volumeKnown;
    if (bytes == 0 || !estimate.crossVolume) return estimate;  // тот же том: это перемещение

    if (!volumeKnown) {
        // Том неизвестен: цена неопределима, но молчать нельзя — кросс-томовое
        // перемещение всегда означает копирование.
        estimate.slowEnoughToWarn = true;
        return estimate;
    }

    estimate.throughputBytesPerSec =
        throughputBytesPerSec != 0 ? throughputBytesPerSec : kDefaultCopyThroughputBytesPerSec;
    estimate.seconds = (bytes + estimate.throughputBytesPerSec - 1) / estimate.throughputBytesPerSec;
    estimate.slowEnoughToWarn = estimate.seconds >= kSlowCopySeconds;
    estimate.recommendsDirectDelete = bytes >= kLargeTransactionBytes;
    return estimate;
}

std::string restoreRenameTarget(const std::string& path, int attempt, std::string_view suffix) {
    if (attempt < 1) attempt = 1;
    const std::string base = applySuffix(path, suffix);
    if (attempt == 1) return base;
    // Номер вставляем перед суффиксом, чтобы «файл (2)» не выглядело
    // продолжением расширения.
    const std::size_t at = base.size() - suffix.size();
    return base.substr(0, at) + " (" + std::to_string(attempt) + ")" + std::string(suffix) +
           base.substr(at + suffix.size());
}

// ---------------------------------------------------------------------------
// Восстановление: конфликты, частичное восстановление, итог
// ---------------------------------------------------------------------------

UndoPlan buildRestorePlan(const UndoTransaction& tx, const RestoreRequest& request,
                             const std::vector<RestoreTargetInfo>& targets) {
    if (request.txId != tx.txId) {
        throw UndoError("buildRestorePlan: запрошена транзакция \"" + request.txId + "\", а дана \"" + tx.txId + "\"");
    }
    if (targets.size() < tx.entries.size()) {
        // Короткий снимок состояния — это гонка или ошибка платформы. Молча
        // считать цель «незанятой» здесь нельзя: восстановление пошло бы поверх
        // чужого файла, а FR-7 запрещает перезаписывать без вопроса.
        throw UndoError("buildRestorePlan: снимок состояния короче транзакции " + tx.txId + " (" +
                         std::to_string(targets.size()) + " целей на " + std::to_string(tx.entries.size()) +
                         " записей)");
    }

    UndoPlan plan;
    plan.txId = tx.txId;
    plan.fullRestore = request.entryIndexes.empty();

    // Выборка приводится к отсортированному вектору: проверка вхождения — в
    // логарифм, а не линейный поиск на каждой записи.
    std::vector<std::size_t> selected = request.entryIndexes;
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    const bool hasSelection = !selected.empty();
    const auto isSelected = [&selected, hasSelection](std::size_t index) {
        return !hasSelection || std::binary_search(selected.begin(), selected.end(), index);
    };

    // Остаток свободного места по томам считается накопительно: пять файлов по
    // 300 МБ не должны «поместиться» в 500 МБ по одному.
    std::map<std::string, std::uint64_t> freeByVolume;
    for (const auto& target : targets) {
        if (target.path.empty() || target.volumeGuidPath.empty()) continue;
        const auto [it, inserted] = freeByVolume.emplace(target.volumeGuidPath, target.freeBytes);
        if (!inserted && target.freeBytes > it->second) it->second = target.freeBytes;
    }

    plan.entries.reserve(tx.entries.size());
    for (std::size_t index = 0; index < tx.entries.size(); ++index) {
        const TrashEntry& entry = tx.entries[index];
        if (!isSelected(index)) continue;
        const RestoreTargetInfo& target = targets[index];

        UndoPlanEntry item;
        item.entryIndex = index;
        item.originalPath = entry.originalPath;
        item.sizeBytes = entry.sizeBytes;
        item.targetPath = target.path.empty() ? entry.originalPath : target.path;
        item.action = RestoreDecision::Restore;
        item.conflict = RestoreConflict::None;

        // 1. Уже возвращён: повторно не трогаем (частичное восстановление не
        //    превращается в повторную запись поверх живого файла).
        if (entry.restored) {
            item.action = RestoreDecision::Skip;
            item.conflict = RestoreConflict::AlreadyRestored;
            item.note = "Элемент уже восстановлен ранее";
        } else if (!target.contentExists) {
            // 2. Содержимое утрачено: запись манифеста осталась, файла нет.
            item.action = RestoreDecision::Skip;
            item.conflict = RestoreConflict::MissingInTrash;
            item.note = "Содержимое в корзине утрачено, восстановление невозможно";
        }

        if (item.conflict == RestoreConflict::None) {
            // 3. Конфликты целевого места. Цель, о которой платформа ничего не
            //    знает, — не «свободна», а «неизвестна»: идти туда вслепую нельзя.
            if (target.path.empty()) {
                item.conflict = RestoreConflict::TargetUnknown;
            }
            if (target.exists && target.isDirectory != entry.isDirectory) {
                item.conflict = worstConflict(item.conflict, RestoreConflict::TargetTypeMismatch);
            } else if (target.exists) {
                item.conflict = worstConflict(item.conflict, RestoreConflict::TargetExists);
            }
            if (target.locked) item.conflict = worstConflict(item.conflict, RestoreConflict::TargetLocked);
            if (!target.parentExists) item.conflict = worstConflict(item.conflict, RestoreConflict::ParentMissing);

            if (!target.volumeGuidPath.empty()) {
                std::uint64_t& free = freeByVolume[target.volumeGuidPath];
                if (entry.sizeBytes > free) {
                    item.conflict = worstConflict(item.conflict, RestoreConflict::NotEnoughSpace);
                } else {
                    free -= entry.sizeBytes;
                }
            }

            if (isBlocking(item.conflict)) {
                switch (request.conflictPolicy) {
                    case ConflictPolicy::Skip:
                        item.action = RestoreDecision::Skip;
                        item.note = "Пропущено по политике конфликта: " + std::string(toString(item.conflict));
                        break;
                    case ConflictPolicy::Overwrite:
                        if (isOverwritable(item.conflict)) {
                            item.action = RestoreDecision::Overwrite;
                            item.note = "Перезапись существующего по явному решению пользователя";
                        } else {
                            item.action = RestoreDecision::Skip;
                            item.note = "Перезапись не помогает при конфликте " + std::string(toString(item.conflict));
                        }
                        break;
                    case ConflictPolicy::Rename:
                        if (isOverwritable(item.conflict)) {
                            item.action = RestoreDecision::Rename;
                            item.targetPath = restoreRenameTarget(item.targetPath);
                            item.note = "Восстановление рядом, исходный объект не тронут";
                        } else {
                            item.action = RestoreDecision::Skip;
                            item.note = "Переименование не помогает при конфликте " +
                                        std::string(toString(item.conflict));
                        }
                        break;
                    case ConflictPolicy::Ask:
                        // FR-7: «существующий файл — не перезаписывать, спросить».
                        // Пропуск здесь временный: applyRestoreAnswers вернёт запись в работу.
                        item.action = RestoreDecision::Skip;
                        item.needsUserDecision = true;
                        item.note = "Нужно решение пользователя: " + std::string(toString(item.conflict));
                        break;
                }
            } else if (item.conflict == RestoreConflict::ParentMissing) {
                item.note = "Родительский каталог будет создан";
            }
        }

        plan.entries.push_back(std::move(item));
    }

    plan.cost = estimateRestoreCost(tx, request.trashVolume);
    finalizePlan(plan);
    return plan;
}

UndoPlan applyRestoreAnswers(UndoPlan plan, const RestoreAnswers& answers) {
    for (auto& item : plan.entries) {
        if (item.action != RestoreDecision::Skip) continue;

        ConflictPolicy policy = ConflictPolicy::Ask;
        bool answered = false;
        for (const auto& [index, chosen] : answers.byEntry) {
            if (index == item.entryIndex) {
                policy = chosen;
                answered = true;
                break;
            }
        }
        if (!answered && !item.needsUserDecision) continue;  // не конфликт: ответ не нужен

        switch (policy) {
            case ConflictPolicy::Skip:
                item.action = RestoreDecision::Skip;
                item.needsUserDecision = false;
                item.note = "Пропущено по решению пользователя";
                break;
            case ConflictPolicy::Overwrite:
                if (isOverwritable(item.conflict)) {
                    item.action = RestoreDecision::Overwrite;
                    item.needsUserDecision = false;
                    item.note = "Перезапись существующего по решению пользователя";
                } else {
                    item.needsUserDecision = false;
                    item.note = "Перезапись невозможна: " + std::string(toString(item.conflict));
                }
                break;
            case ConflictPolicy::Rename: {
                if (!isOverwritable(item.conflict)) {
                    item.needsUserDecision = false;
                    item.note = "Восстановление рядом невозможно: " + std::string(toString(item.conflict));
                    break;
                }
                item.action = RestoreDecision::Rename;
                item.needsUserDecision = false;
                item.note = "Восстановление рядом по решению пользователя";
                for (const auto& [index, path] : answers.renameTo) {
                    if (index == item.entryIndex && !path.empty()) item.targetPath = path;
                }
                if (item.targetPath == item.originalPath) item.targetPath = restoreRenameTarget(item.targetPath);
                break;
            }
            case ConflictPolicy::Ask:
                // Без ответа элемент остаётся пропущенным: «не ответил» не должно
                // превращаться в перезапись (FR-7).
                item.note = "Ожидает решения пользователя: " + std::string(toString(item.conflict));
                break;
        }
    }
    finalizePlan(plan);
    return plan;
}

RestoreOutcome summarizeRestore(const UndoPlan& plan, const std::vector<RestoreEntryResult>& results) {
    RestoreOutcome outcome;
    outcome.skippedCount = plan.skippedCount;

    std::vector<bool> accounted(plan.entries.size(), false);
    for (const auto& result : results) {
        if (result.entryIndex >= plan.entries.size()) continue;
        if (accounted[result.entryIndex]) continue;  // повторный отчёт по записи игнорируем
        accounted[result.entryIndex] = true;
        if (result.ok) {
            ++outcome.restoredCount;
            outcome.bytesRestored = additionOverflow(outcome.bytesRestored, result.bytesRestored);
        } else {
            ++outcome.failedCount;
            outcome.failures.push_back(result);
        }
    }
    for (std::size_t i = 0; i < plan.entries.size(); ++i) {
        const bool planned = plan.entries[i].action != RestoreDecision::Skip;
        if (planned && !accounted[i]) ++outcome.notAttemptedCount;
    }

    outcome.summary = "Восстановлено " + std::to_string(outcome.restoredCount) + " из " +
                      std::to_string(plan.entries.size()) + ", пропущено " + std::to_string(outcome.skippedCount);
    if (outcome.failedCount > 0) {
        outcome.summary += ", ошибок " + std::to_string(outcome.failedCount);
    }
    if (outcome.notAttemptedCount > 0) {
        outcome.summary += ", не выполнено " + std::to_string(outcome.notAttemptedCount);
    }
    if (plan.cost.crossVolume && plan.cost.slowEnoughToWarn) {
        outcome.summary += "; копирование между томами заняло около " + formatSeconds(plan.cost.seconds);
    }
    return outcome;
}

UndoTransaction applyRestored(const UndoTransaction& tx, const UndoPlan& plan,
                               const std::vector<RestoreEntryResult>& results, std::int64_t nowUnix) {
    UndoTransaction updated = tx;
    for (const auto& result : results) {
        if (!result.ok || result.entryIndex >= updated.entries.size()) continue;
        TrashEntry& entry = updated.entries[result.entryIndex];
        if (entry.restored) continue;
        entry.restored = true;
        entry.restoredUnix = nowUnix;
    }
    // Схлопнулась — значит, возвращать больше нечего: Ctrl+Z должен погаснуть
    // (SPEC §7.2). Неудачные записи остаются: их можно повторить.
    if (updated.restorableCount() == 0) updated.collapsed = true;
    (void)plan;  // план влияет только через результаты; параметр оставлен для симметрии API
    return updated;
}

}  // namespace mrproper::core
