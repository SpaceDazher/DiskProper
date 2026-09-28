#include "trash.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "units.hpp"

namespace mrproper::core {
namespace {

constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kSecondsPerHour = 3600;
constexpr std::int64_t kSecondsPerDay = 86400;

// 2^53 — предел, до которого double хранит целые без потери точности. Манифест
// читается через json::Value, где число это double, поэтому числа крупнее
// принимаются не молча: лучше отказ, чем испорченный размер, из-за которого
// восстановление легло бы не туда.
constexpr double kMaxExactInteger = 9007199254740992.0;

// Порог, после которого кросс-томовой перенос (это копирование, FR-7)
// предлагается отменить в пользу прямого удаления. Спека говорит «большие
// транзакции — сразу прямой делет» без числа, поэтому порог назван и вынесен
// сюда: это политика интерфейса, а не измерение.
constexpr std::uint64_t kCrossVolumeComfortBytes = 16777216ull;  // 16 МБ

// --- Время: только арифметика, без часов системы и без локали ---------------

// Год/месяц/день из числа дней с 1970-01-01 (алгоритм civil_from_days
// Howard Hinnant). Нужен, чтобы метка транзакции совпадала на любом хосте и в
// тестах: gmtime/localtime завязаны на локаль и на часовой пояс.
std::int64_t civilFromDays(std::int64_t daysSinceEpoch) {    const std::int64_t z = daysSinceEpoch + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;                                      // [0, 146096]
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
    const std::int64_t mp = (5 * doy + 2) / 153;                                     // [0, 11]
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;                            // [1, 12]
    return (y + (month <= 2 ? 1 : 0)) * 10000 + month * 100 + day;  // упаковано как yyyymmdd
}

// Деление с округлением вниз и неотрицательным остатком: метка времени должна
// быть верной и для отметок до 1970 года (тесты с отрицательным now).
void floorDivMod(std::int64_t value, std::int64_t divisor, std::int64_t& quotient, std::int64_t& rest) {
    quotient = value / divisor;
    rest = value % divisor;
    if (rest < 0) {
        rest += divisor;
        --quotient;
    }
}

// Разложение отметки времени на календарные части и секунды суток — общая часть
// для двух форм метки: с двоеточиями (для показа) и без них (для имени каталога).
void utcParts(std::int64_t nowUnixSeconds, std::int64_t& year, std::int64_t& month, std::int64_t& day,
              std::int64_t& restOfDay) {
    std::int64_t days = 0;
    std::int64_t rest = 0;
    floorDivMod(nowUnixSeconds, kSecondsPerDay, days, rest);
    const std::int64_t packed = civilFromDays(days);
    year = packed / 10000;
    month = (packed / 100) % 100;
    day = packed % 100;
    restOfDay = rest;
}

// Метка без двоеточий: только она годится для имени каталога, потому что
// двоеточие в Windows — это поток данных (поток «Z»), и путь с ним нельзя
// ни создать, ни прочитать обычным способом.
std::string compactStamp(std::int64_t nowUnixSeconds) {
    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;
    std::int64_t rest = 0;
    utcParts(nowUnixSeconds, year, month, day, rest);

    char buf[64];  // запас на год длиннее четырёх цифр
    std::snprintf(buf, sizeof(buf), "%04lld%02lld%02lldT%02lld%02lld%02lldZ", static_cast<long long>(year),
                  static_cast<long long>(month), static_cast<long long>(day), static_cast<long long>(rest / kSecondsPerHour),
                  static_cast<long long>((rest % kSecondsPerHour) / kSecondsPerMinute),
                  static_cast<long long>(rest % kSecondsPerMinute));
    return std::string(buf);
}

// --- Проверки манифеста -----------------------------------------------------

[[noreturn]] void fail(const std::string& origin, const std::string& what) { throw TrashError(origin + ": " + what); }

// Обязательное поле: отсутствие — TrashError, а не runtime_error из json.
// Иначе вызывающий, который ловит только ошибки корзины, пропустил бы битый
// манифест дальше по коду.
const json::Value& requireNode(const json::Value& parent, const char* field, const std::string& origin) {
    const json::Value* node = parent.find(field);
    if (node == nullptr) fail(origin, std::string("нет обязательного поля \"") + field + "\"");
    return *node;
}

std::int64_t expectInt(const json::Value& value, const char* field, const std::string& origin) {
    const std::string named = std::string("поле \"") + field + "\"";
    if (!value.isNumber()) fail(origin, named + " должно быть числом");
    const double raw = value.asNumber();
    if (!std::isfinite(raw) || raw != std::floor(raw)) fail(origin, named + " должно быть целым числом");
    if (raw < -kMaxExactInteger || raw > kMaxExactInteger) fail(origin, named + " вне диапазона точного целого");
    return static_cast<std::int64_t>(raw);
}

std::int64_t requireInt(const json::Value& parent, const char* field, const std::string& origin) {
    return expectInt(requireNode(parent, field, origin), field, origin);
}

std::int64_t optionalInt(const json::Value& parent, const char* field, const std::string& origin,
                         std::int64_t fallback) {
    const json::Value* node = parent.find(field);
    if (node == nullptr) return fallback;
    return expectInt(*node, field, origin);
}

std::uint64_t optionalNonNegative(const json::Value& parent, const char* field, const std::string& origin,
                                  std::uint64_t fallback) {
    const json::Value* node = parent.find(field);
    if (node == nullptr) return fallback;
    const std::int64_t raw = expectInt(*node, field, origin);
    if (raw < 0) fail(origin, std::string("поле \"") + field + "\" не может быть отрицательным");
    return static_cast<std::uint64_t>(raw);
}

// Счётчик файлов каталога живёт в 32-битном поле: значение крупнее упирается в
// потолок, а не переворачивается в мусор при сужении.
std::uint32_t clampToU32(std::uint64_t value) {
    const std::uint64_t top = std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(value > top ? top : value);
}

bool optionalBool(const json::Value& parent, const char* field, const std::string& origin, bool fallback) {
    const json::Value* node = parent.find(field);
    if (node == nullptr) return fallback;
    if (!node->isBool()) fail(origin, std::string("поле \"") + field + "\" должно быть true или false");
    return node->asBool();
}

std::string optionalString(const json::Value& parent, const char* field, const std::string& origin,
                           const std::string& fallback) {
    const json::Value* node = parent.find(field);
    if (node == nullptr) return fallback;
    if (!node->isString()) fail(origin, std::string("поле \"") + field + "\" должно быть строкой");
    return node->asString();
}

std::string requireString(const json::Value& parent, const char* field, const std::string& origin) {
    const json::Value& node = requireNode(parent, field, origin);
    if (!node.isString()) fail(origin, std::string("поле \"") + field + "\" должно быть строкой");
    return node.asString();
}

// Неизвестное поле — ошибка, а не предупреждение: манифест лежит на диске, его
// мог дописать посторонний или он остался от другой версии. Молчаливое «прочитал
// как понял» здесь означало бы восстановление не туда — тот же довод, что и для
// набора правил (ADR-008).
void rejectUnknownFields(const json::Value& node, const std::vector<std::string>& allowed, const std::string& origin) {
    for (const auto& member : node.members()) {
        if (std::find(allowed.begin(), allowed.end(), member.first) == allowed.end()) {
            fail(origin, "неизвестное поле \"" + member.first + "\"");
        }
    }
}

const std::vector<std::string>& itemFields() {
    static const std::vector<std::string> kFields = {"kind",    "originalPath", "payload",     "bytes",
                                                      "mtime",   "fileCount",    "readOnly",    "aclSddl",
                                                      "crossVolume", "sourceVolume"};
    return kFields;
}

const std::vector<std::string>& transactionFields() {
    static const std::vector<std::string> kFields = {"schema", "txId",     "createdAt", "appVersion",
                                                      "state",  "purgedBytes", "items"};
    return kFields;
}

TrashItemKind itemKindFromString(const std::string& text, const std::string& origin) {
    if (text == "file") return TrashItemKind::File;
    if (text == "directory") return TrashItemKind::Directory;
    fail(origin, "kind: допустимы file|directory, получено \"" + text + "\"");
}

TrashTxState stateFromString(const std::string& text, const std::string& origin) {
    if (text == "open") return TrashTxState::Open;
    if (text == "committed") return TrashTxState::Committed;
    if (text == "undone") return TrashTxState::Undone;
    if (text == "collapsed") return TrashTxState::Collapsed;
    fail(origin, "state: допустимы open|committed|undone|collapsed, получено \"" + text + "\"");
}

// Сложение с насыщением: сумма размеров транзакций не должна переворачиваться
// в ноль переполнением — иначе лимит 2 ГБ «вмещает» что угодно.
std::uint64_t addSaturating(std::uint64_t a, std::uint64_t b) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return std::numeric_limits<std::uint64_t>::max();
    return a + b;
}

bool isExpired(const TrashLimits& limits, const TrashTransactionInfo& tx, std::int64_t now) {
    // maxAgeSeconds <= 0 — возрастной лимит выключен (значимые настройки).
    if (limits.maxAgeSeconds <= 0) return false;
    return now - tx.createdAt > limits.maxAgeSeconds;
}

// Порядок вытеснения: от самой старой транзакции к самой новой; при равном
// времени — по идентификатору, чтобы план был воспроизводим.
bool olderFirst(const TrashTransactionInfo* a, const TrashTransactionInfo* b) {
    if (a->createdAt != b->createdAt) return a->createdAt < b->createdAt;
    return a->txId < b->txId;
}

}  // namespace

// ---------------------------------------------------------------------------
// Типы и их текстовое имя
// ---------------------------------------------------------------------------

const char* toString(TrashItemKind kind) { return kind == TrashItemKind::Directory ? "directory" : "file"; }

const char* toString(TrashTxState state) {
    switch (state) {
        case TrashTxState::Open: return "open";
        case TrashTxState::Committed: return "committed";
        case TrashTxState::Undone: return "undone";
        case TrashTxState::Collapsed: return "collapsed";
    }
    return "open";
}

std::uint64_t TrashTransaction::totalBytes() const {
    std::uint64_t total = 0;
    for (const auto& item : items) total = addSaturating(total, item.bytes);
    return total;
}

std::uint32_t TrashTransaction::itemCount() const {
    const std::size_t count = items.size();
    if (count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(count);
}

bool TrashTransaction::undoable() const { return state == TrashTxState::Committed; }

TrashTransactionInfo infoOf(const TrashTransaction& tx) {
    TrashTransactionInfo info;
    info.txId = tx.txId;
    info.createdAt = tx.createdAt;
    // Восстановленная транзакция места не занимает: содержимое вернулось по
    // местам, а её каталог платформа удалит по forget(). Считать прежние байты
    // значило бы вытеснять пустоту и урезать лимит для остальных.
    info.bytes = tx.state == TrashTxState::Undone ? 0 : tx.totalBytes();
    info.itemCount = tx.itemCount();
    return info;
}

TrashUsage summarize(const std::vector<TrashTransactionInfo>& transactions) {
    TrashUsage usage;
    usage.transactionCount = static_cast<std::uint32_t>(transactions.size());
    for (const auto& tx : transactions) {
        usage.totalBytes = addSaturating(usage.totalBytes, tx.bytes);
        usage.itemCount = addSaturating(usage.itemCount, tx.itemCount);
        if (usage.oldestAt == 0 || (tx.createdAt != 0 && tx.createdAt < usage.oldestAt)) usage.oldestAt = tx.createdAt;
        if (tx.createdAt > usage.newestAt) usage.newestAt = tx.createdAt;
    }
    if (transactions.empty()) {
        usage.oldestAt = 0;
        usage.newestAt = 0;
    }
    return usage;
}

// ---------------------------------------------------------------------------
// Манифест: JSON
// ---------------------------------------------------------------------------

json::Value toJson(const TrashItem& item) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("kind", json::Value(toString(item.kind)));
    members.emplace_back("originalPath", json::Value(item.originalPath));
    members.emplace_back("payload", json::Value(item.payload));
    members.emplace_back("bytes", json::Value(static_cast<double>(item.bytes)));
    members.emplace_back("fileCount", json::Value(static_cast<double>(item.fileCount)));
    members.emplace_back("mtime", json::Value(static_cast<double>(item.mtime)));
    members.emplace_back("readOnly", json::Value(item.readOnly));
    members.emplace_back("aclSddl", json::Value(item.aclSddl));
    members.emplace_back("crossVolume", json::Value(item.crossVolume));
    members.emplace_back("sourceVolume", json::Value(item.sourceVolume));
    return json::Value::object(std::move(members));
}

json::Value toJson(const TrashTransaction& tx) {
    std::vector<json::Value> items;
    items.reserve(tx.items.size());
    for (const auto& item : tx.items) items.push_back(toJson(item));

    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("schema", json::Value(kTrashManifestSchema));
    members.emplace_back("txId", json::Value(tx.txId));
    members.emplace_back("createdAt", json::Value(static_cast<double>(tx.createdAt)));
    members.emplace_back("appVersion", json::Value(tx.appVersion));
    members.emplace_back("state", json::Value(toString(tx.state)));
    members.emplace_back("purgedBytes", json::Value(static_cast<double>(tx.purgedBytes)));
    members.emplace_back("items", json::Value::array(std::move(items)));
    return json::Value::object(std::move(members));
}

TrashItem itemFromJson(const json::Value& value, const std::string& origin) {
    if (!value.isObject()) fail(origin, "элемент корзины должен быть объектом");
    rejectUnknownFields(value, itemFields(), origin);

    TrashItem item;
    item.kind = itemKindFromString(optionalString(value, "kind", origin, "file"), origin);
    item.originalPath = requireString(value, "originalPath", origin);
    if (item.originalPath.empty()) fail(origin, "originalPath пуст — нечего восстанавливать");

    item.payload = requireString(value, "payload", origin);
    if (!isValidPayloadName(item.payload)) {
        fail(origin, "недопустимое имя объекта в корзине \"" + item.payload + "\"");
    }

    item.bytes = optionalNonNegative(value, "bytes", origin, 0);
    item.fileCount = clampToU32(optionalNonNegative(value, "fileCount", origin, 1));
    item.mtime = optionalInt(value, "mtime", origin, 0);
    item.readOnly = optionalBool(value, "readOnly", origin, false);
    item.aclSddl = optionalString(value, "aclSddl", origin, std::string());
    item.crossVolume = optionalBool(value, "crossVolume", origin, false);
    item.sourceVolume = optionalString(value, "sourceVolume", origin, std::string());
    return item;
}

TrashTransaction transactionFromJson(const json::Value& value, const std::string& origin) {
    if (!value.isObject()) fail(origin, "манифест должен быть объектом");
    rejectUnknownFields(value, transactionFields(), origin);

    const std::int64_t schema = requireInt(value, "schema", origin);
    if (schema != kTrashManifestSchema) {
        fail(origin, "схема манифеста " + std::to_string(schema) + " не поддерживается (ожидается " +
                         std::to_string(kTrashManifestSchema) + ")");
    }

    TrashTransaction tx;
    tx.txId = requireString(value, "txId", origin);
    if (!isValidTxId(tx.txId)) fail(origin, "недопустимый txId \"" + tx.txId + "\"");
    tx.createdAt = requireInt(value, "createdAt", origin);
    tx.appVersion = optionalString(value, "appVersion", origin, std::string());
    tx.state = stateFromString(optionalString(value, "state", origin, toString(TrashTxState::Open)), origin);
    tx.purgedBytes = optionalNonNegative(value, "purgedBytes", origin, 0);

    const json::Value& items = requireNode(value, "items", origin);
    if (!items.isArray()) fail(origin, "поле \"items\" должно быть массивом");
    tx.items.reserve(items.items().size());
    for (std::size_t i = 0; i < items.items().size(); ++i) {
        tx.items.push_back(itemFromJson(items.items()[i], origin + ": items[" + std::to_string(i) + "]"));
    }
    return tx;
}

std::string serializeManifest(const TrashTransaction& tx, int indent) { return toJson(tx).dump(indent); }

TrashTransaction parseManifest(std::string_view text, const std::string& origin) {
    // json::ParseError ловим и переименовываем: движок ловит один тип ошибок
    // корзины, иначе «битый манифест» ушёл бы в отдельную ветку обработки.
    try {
        return transactionFromJson(json::parse(text), origin);
    } catch (const json::ParseError& e) {
        throw TrashError(origin + ": манифест не разобран (" + e.what() + ")");
    }
}

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------

std::string joinPath(std::string_view base, std::string_view leaf) {
    if (base.empty()) return std::string(leaf);
    if (leaf.empty()) return std::string(base);

    std::string head(base);
    char last = head.back();
    if (last == '/' || last == '\\') return head + std::string(leaf);

    // Windows-путь узнаём по обратному слэшу или по диску («C:»): разделитель
    // в нём — обратный слэш, иначе пришлось бы угадывать по умолчанию.
    const bool windowsStyle = head.find('\\') != std::string::npos || head.find(':') != std::string::npos;
    if (last == ':') return head + "\\" + std::string(leaf);
    return head + (windowsStyle ? "\\" : "/") + std::string(leaf);
}

std::string trashRootPath(std::string_view programDataDir) {
    if (programDataDir.empty()) return std::string();
    return joinPath(joinPath(programDataDir, kTrashAppDirName), kTrashDirName);
}

std::string transactionDir(std::string_view trashRoot, std::string_view txId) {
    if (!isValidTxId(txId)) {
        // txId приезжает из манифеста, то есть с диска: имя каталога собирается
        // только из проверенных символов, иначе «../../Windows» вышел бы из
        // корня корзины.
        throw TrashError("недопустимый идентификатор транзакции \"" + std::string(txId) + "\"");
    }
    return joinPath(trashRoot, txId);
}

std::string manifestPath(std::string_view transactionDirPath) { return joinPath(transactionDirPath, kTrashManifestFileName); }

// ---------------------------------------------------------------------------
// Идентификатор транзакции
// ---------------------------------------------------------------------------

std::string formatUtcStamp(std::int64_t nowUnixSeconds) {
    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;
    std::int64_t rest = 0;
    utcParts(nowUnixSeconds, year, month, day, rest);

    char buf[64];  // запас на год длиннее четырёх цифр
    std::snprintf(buf, sizeof(buf), "%04lld%02lld%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(year),
                  static_cast<long long>(month), static_cast<long long>(day), static_cast<long long>(rest / kSecondsPerHour),
                  static_cast<long long>((rest % kSecondsPerHour) / kSecondsPerMinute),
                  static_cast<long long>(rest % kSecondsPerMinute));
    return std::string(buf);
}

std::string makeTxId(std::int64_t nowUnixSeconds, std::uint32_t counter, std::uint32_t nonce) {
    char counterText[16];
    std::snprintf(counterText, sizeof(counterText), "%06lx", static_cast<unsigned long>(counter & 0xFFFFFFu));
    char nonceText[16];
    std::snprintf(nonceText, sizeof(nonceText), "%08lx", static_cast<unsigned long>(nonce));
    // Метка компактная, без двоеточий: результат должен проходить isValidTxId,
    // иначе получившееся имя каталога нельзя было бы даже собрать.
    return compactStamp(nowUnixSeconds) + "-" + std::string(counterText) + "-" + std::string(nonceText);
}

bool isValidTxId(std::string_view txId) {
    if (txId.empty() || txId.size() > 64) return false;
    if (txId.front() == '-') return false;
    for (const char c : txId) {
        const bool okChar = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
        if (!okChar) return false;
    }
    return true;
}

bool isValidPayloadName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == "..") return false;
    for (const char c : name) {
        const bool okChar = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                            c == '.' || c == '-';
        if (!okChar) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Лимиты
// ---------------------------------------------------------------------------

std::vector<std::string> selectExpired(const TrashLimits& limits,
                                       const std::vector<TrashTransactionInfo>& transactions, std::int64_t now) {
    std::vector<const TrashTransactionInfo*> sorted;
    sorted.reserve(transactions.size());
    for (const auto& tx : transactions) {
        if (tx.bytes == 0) continue;  // восстановленное место не занимает
        sorted.push_back(&tx);
    }
    std::sort(sorted.begin(), sorted.end(), olderFirst);

    std::vector<std::string> out;
    for (const auto* tx : sorted) {
        if (!isExpired(limits, *tx, now)) break;  // дальше только более свежие
        out.push_back(tx->txId);
    }
    return out;
}

TrashEvictionPlan planEviction(const TrashLimits& limits, const std::vector<TrashTransactionInfo>& transactions,
                              std::uint64_t incomingBytes, std::int64_t now) {
    TrashEvictionPlan plan;

    std::vector<const TrashTransactionInfo*> sorted;
    sorted.reserve(transactions.size());
    std::uint64_t total = 0;
    for (const auto& tx : transactions) {
        if (tx.bytes == 0) continue;  // восстановленное место не занимает
        sorted.push_back(&tx);
        total = addSaturating(total, tx.bytes);
    }
    std::sort(sorted.begin(), sorted.end(), olderFirst);

    // Шаг 1 — возрастной лимит: протухшее уходит всегда, независимо от того,
    // есть ли свободное место. Иначе корзина в 2 ГБ могла бы пережить 7 дней
    // просто потому, что её никто не переполнял.
    for (const auto* tx : sorted) {
        if (!isExpired(limits, *tx, now)) break;
        plan.txIds.push_back(tx->txId);
        plan.reclaimBytes = addSaturating(plan.reclaimBytes, tx->bytes);
        plan.freedByAge = addSaturating(plan.freedByAge, tx->bytes);
        total = total > tx->bytes ? total - tx->bytes : 0;
    }

    // Шаг 2 — место: от самой старой оставшейся транзакции. incomingBytes == 0
    // означает «чистим только по возрасту»: вытеснять ради нуля нельзя.
    for (const auto* tx : sorted) {
        if (incomingBytes == 0) break;    // чистим только по возрасту
        if (limits.maxBytes == 0) break;  // лимит выключен — вытеснять нечем
        if (addSaturating(total, incomingBytes) <= limits.maxBytes) break;
        if (std::find(plan.txIds.begin(), plan.txIds.end(), tx->txId) != plan.txIds.end()) continue;
        plan.txIds.push_back(tx->txId);
        plan.reclaimBytes = addSaturating(plan.reclaimBytes, tx->bytes);
        plan.freedBySize = addSaturating(plan.freedBySize, tx->bytes);
        total = total > tx->bytes ? total - tx->bytes : 0;
    }
    return plan;
}

TrashPlacementPlan planTrashPlacement(const TrashLimits& limits, const std::vector<TrashTransactionInfo>& transactions,
                                      std::uint64_t itemBytes, bool sameVolume, std::int64_t now) {
    TrashPlacementPlan plan;
    plan.crossVolume = !sameVolume;
    // Кросс-томовой перенос — это копирование (FR-7): показываем честно, что
    // это долго, даже когда решение остаётся «в корзину».
    plan.expensive = !sameVolume;

    if (itemBytes > limits.largeItemBytes) {
        // FR-7: «для больших кэшей (> 100 МБ) — прямое удаление с записью в
        // журнал (иначе корзина раздувается)».
        plan.placement = TrashPlacement::DirectDelete;
        plan.reason = "крупный кэш: в корзину не кладём, удаляем напрямую (FR-7)";
        return plan;
    }
    if (limits.maxBytes != 0 && itemBytes > limits.maxBytes) {
        // Не поместится даже в пустую корзину: держать это в корзине нельзя,
        // иначе лимит перестанет быть лимитом.
        plan.placement = TrashPlacement::DirectDelete;
        plan.reason = "объект больше всего лимита корзины — в неё он не поместится";
        return plan;
    }
    if (!sameVolume && itemBytes >= kCrossVolumeComfortBytes) {
        // «Большие транзакции предлагаем сразу прямой делет» (FR-7): копировать
        // сотни мегабайт медленнее, чем удалить.
        plan.placement = TrashPlacement::DirectDelete;
        plan.reason = "кросс-томовой перенос копирует данные и долгий — быстрее удалить сразу";
        return plan;
    }

    const TrashUsage usage = summarize(transactions);
    const TrashEvictionPlan eviction = planEviction(limits, transactions, itemBytes, now);
    plan.evictTxIds = eviction.txIds;
    plan.bytesAfter = addSaturating(usage.totalBytes, itemBytes);

    if (limits.maxBytes != 0 && plan.bytesAfter > limits.maxBytes) {
        const std::uint64_t over = plan.bytesAfter - limits.maxBytes;
        // Вытеснение закрывает нехватку целиком; остаток означает, что
        // вытеснять нечего (несовместимые данные лимита и журнала) — тогда
        // «лимит 2 ГБ» перестал бы быть лимитом.
        plan.evictBytes = over > eviction.reclaimBytes ? over - eviction.reclaimBytes : 0;
        if (plan.evictBytes != 0) {
            plan.placement = TrashPlacement::DirectDelete;
            plan.reason = "корзина переполнена и вытеснить нечего — удаляем напрямую";
            return plan;
        }
    }

    if (plan.evictTxIds.empty()) {
        plan.reason = plan.crossVolume ? "кросс-томовой перенос — это копирование, займёт время"
                                       : "помещается в корзину, отменяемо";
    } else {
        plan.reason = "в корзине освободим место за счёт протухших и старых транзакций";
    }
    return plan;
}

TrashPlacementPlan planTrashPlacement(const TrashLimits& limits, const TrashUsage& usage, std::uint64_t itemBytes,
                                      bool sameVolume, std::int64_t now) {
    // Вариант для вызывающего, который знает только итоги: правила те же, но
    // перечислить вытесняемые транзакции нечем, поэтому evictTxIds пуст, а
    // evictBytes говорит, сколько освободить. План вытеснения целиком строит
    // вызывающий через planEviction(limits, ledger.info(), …).
    TrashPlacementPlan plan = planTrashPlacement(limits, std::vector<TrashTransactionInfo>{}, itemBytes, sameVolume, now);
    plan.evictTxIds.clear();
    plan.evictBytes = 0;
    plan.bytesAfter = addSaturating(usage.totalBytes, itemBytes);
    if (plan.placement == TrashPlacement::Trash && limits.maxBytes != 0 && plan.bytesAfter > limits.maxBytes) {
        plan.evictBytes = plan.bytesAfter - limits.maxBytes;
        plan.reason = "нужно вытеснить протухшие или старые транзакции, чтобы уложиться в лимит";
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Журнал корзины
// ---------------------------------------------------------------------------

TrashLedger::TrashLedger(TrashLimits limits, std::string trashRoot)
    : limits_(limits), root_(std::move(trashRoot)) {}

std::string TrashLedger::begin(std::int64_t now, const std::string& appVersion) {
    ++counter_;
    // nonce растёт нечётным шагом: два процесса, стартовавшие в одну секунду,
    // разойдутся идентификаторами и не подерутся за один каталог.
    nonce_ += 0x9E3779B9u;
    const std::string txId = makeTxId(now, counter_, nonce_);

    TrashTransaction tx;
    tx.txId = txId;
    tx.createdAt = now;
    tx.appVersion = appVersion;
    tx.state = TrashTxState::Open;
    transactions_.push_back(std::move(tx));
    return txId;
}

bool TrashLedger::addItem(std::string_view txId, TrashItem item) {
    TrashTransaction* tx = find(txId);
    if (tx == nullptr) return false;
    if (item.payload.empty()) {
        // Имя по номеру элемента: восстановление идёт по манифесту, поэтому
        // оно должно быть стабильным и не содержать разделителей.
        item.payload = "p" + std::to_string(tx->items.size());
    }
    if (!isValidPayloadName(item.payload)) {
        throw TrashError("недопустимое имя объекта в корзине \"" + item.payload + "\"");
    }
    tx->items.push_back(std::move(item));
    return true;
}

bool TrashLedger::commit(std::string_view txId) {
    TrashTransaction* tx = find(txId);
    if (tx == nullptr || tx->state != TrashTxState::Open) return false;
    tx->state = TrashTxState::Committed;
    return true;
}

bool TrashLedger::markUndone(std::string_view txId) {
    TrashTransaction* tx = find(txId);
    if (tx == nullptr || tx->state != TrashTxState::Committed) return false;
    tx->state = TrashTxState::Undone;
    return true;
}

void TrashLedger::collapse(TrashTransaction& tx, std::uint64_t purgedBytes) {
    tx.purgedBytes = addSaturating(tx.purgedBytes, purgedBytes);
    // Схлопнулась — значит содержимое вытеснено, а вместе с ним исчезла и
    // возможность отмены (§7.2). Часть элементов, потерянных безвозвратно,
    // движок удаляет из tx.items сам: ядро не знает, что именно платформа
    // успела снести.
    tx.state = TrashTxState::Collapsed;
}

bool TrashLedger::adopt(TrashTransaction tx) {
    if (!isValidTxId(tx.txId)) throw TrashError("недопустимый идентификатор транзакции \"" + tx.txId + "\"");
    for (auto& existing : transactions_) {
        if (existing.txId == tx.txId) {
            existing = std::move(tx);
            return false;
        }
    }
    transactions_.push_back(std::move(tx));
    return true;
}

bool TrashLedger::forget(std::string_view txId) {
    for (auto it = transactions_.begin(); it != transactions_.end(); ++it) {
        if (std::string_view(it->txId) == txId) {
            transactions_.erase(it);
            return true;
        }
    }
    return false;
}

TrashTransaction* TrashLedger::find(std::string_view txId) {
    for (auto& tx : transactions_) {
        if (std::string_view(tx.txId) == txId) return &tx;
    }
    return nullptr;
}

const TrashTransaction* TrashLedger::find(std::string_view txId) const {
    for (const auto& tx : transactions_) {
        if (std::string_view(tx.txId) == txId) return &tx;
    }
    return nullptr;
}

std::vector<TrashTransactionInfo> TrashLedger::info() const {
    std::vector<TrashTransactionInfo> out;
    out.reserve(transactions_.size());
    for (const auto& tx : transactions_) out.push_back(infoOf(tx));
    return out;
}

TrashUsage TrashLedger::usage() const { return summarize(info()); }

TrashEvictionPlan TrashLedger::planEviction(std::uint64_t incomingBytes, std::int64_t now) const {
    return mrproper::core::planEviction(limits_, info(), incomingBytes, now);
}

std::vector<std::string> TrashLedger::expired(std::int64_t now) const { return selectExpired(limits_, info(), now); }

std::string TrashLedger::dirFor(std::string_view txId) const { return transactionDir(root_, txId); }

std::string TrashLedger::manifestFor(std::string_view txId) const { return manifestPath(transactionDir(root_, txId)); }

std::string TrashLedger::payloadFor(std::string_view txId, std::string_view payload) const {
    if (!isValidPayloadName(payload)) throw TrashError("недопустимое имя объекта в корзине \"" + std::string(payload) + "\"");
    return joinPath(transactionDir(root_, txId), payload);
}

// ---------------------------------------------------------------------------
// Восстановление
// ---------------------------------------------------------------------------

bool RestorePlan::partial() const { return !items.empty() && (conflictCount != 0 || missingCount != 0); }

std::string RestorePlan::explain() const {
    if (items.empty()) return "Нечего восстанавливать: в транзакции нет выбранных элементов";
    std::string text = "К восстановлению: " + formatCount(static_cast<std::uint64_t>(restoreCount)) + ", " +
                       formatBytes(restoreBytes, 1, true);
    if (conflictCount != 0) {
        text += "; конфликтов (на месте есть файл): " + formatCount(static_cast<std::uint64_t>(conflictCount));
    }
    if (missingCount != 0) {
        text += "; в корзине отсутствует: " + formatCount(static_cast<std::uint64_t>(missingCount));
    }
    if (conflictCount != 0) text += " — перезапись только после подтверждения";
    return text;
}

RestorePlan planRestore(const TrashTransaction& tx, const std::vector<std::size_t>& selection, const ExistsProbe& exists,
                        const RestoreOptions& options) {
    RestorePlan plan;
    if (tx.items.empty()) return plan;

    // Пустой selection означает «всё»: полное восстановление — обычный случай,
    // частичное (FR-7) задаётся списком индексов.
    std::vector<bool> chosen(tx.items.size(), selection.empty());
    for (const std::size_t index : selection) {
        if (index >= tx.items.size()) {
            throw TrashError("индекс элемента " + std::to_string(index) + " вне транзакции " + tx.txId);
        }
        chosen[index] = true;
    }

    for (std::size_t i = 0; i < tx.items.size(); ++i) {
        if (!chosen[i]) continue;
        const TrashItem& item = tx.items[i];

        RestoreItemPlan entry;
        entry.index = i;
        entry.item = item;

        if (item.payload.empty() || !isValidPayloadName(item.payload)) {
            // Содержимого нет (или имя объекта подставлено из чужого манифеста) —
            // восстанавливать нечего, но элемент видно в плане, чтобы отчёт
            // показал потерю, а не молчал.
            entry.action = RestoreAction::Missing;
        } else if (exists && exists(item.originalPath)) {
            // FR-7: «существующий файл — не перезаписывать, спросить».
            entry.action = options.overwriteConflicts ? RestoreAction::Overwrite : RestoreAction::Conflict;
        } else {
            entry.action = RestoreAction::Restore;
        }

        switch (entry.action) {
            case RestoreAction::Restore:
                plan.restoreCount += 1;
                plan.restoreBytes = addSaturating(plan.restoreBytes, item.bytes);
                break;
            case RestoreAction::Overwrite:
                plan.restoreCount += 1;
                plan.restoreBytes = addSaturating(plan.restoreBytes, item.bytes);
                break;
            case RestoreAction::Conflict:
                plan.conflictCount += 1;
                plan.conflictBytes = addSaturating(plan.conflictBytes, item.bytes);
                break;
            case RestoreAction::Missing:
                plan.missingCount += 1;
                plan.missingBytes = addSaturating(plan.missingBytes, item.bytes);
                break;
        }
        plan.items.push_back(std::move(entry));
    }
    return plan;
}

RestorePlan planRestoreAll(const TrashTransaction& tx, const ExistsProbe& exists, const RestoreOptions& options) {
    return planRestore(tx, std::vector<std::size_t>{}, exists, options);
}

std::string trashSummary(const TrashUsage& usage, const TrashLimits& limits) {
    std::string text = formatBytes(usage.totalBytes, 1, true) + " из " + formatBytes(limits.maxBytes, 1, true);
    text += ", транзакций: " + std::to_string(usage.transactionCount);
    if (usage.oldestAt != 0) {
        // Точный возраст считает вызывающий (ему видны текущие часы), здесь
        // только дата создания: сводка не должна врать «только что».
        text += ", старейшая от " + formatUtcStamp(usage.oldestAt);
    }
    return text;
}

}  // namespace mrproper::core
