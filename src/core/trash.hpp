// Корзина приложения: манифест транзакции, лимиты, решения о размещении и
// восстановление (SPEC §4 FR-7, §9.1 ADR-005).
//
// Почему своя корзина (ADR-005): системная не масштабируется на десятки ГБ
// кэшей и не даёт транзакции. Своя — отсюда и обязательства: манифест
// (исходный путь, размер, mtime, ACL), лимит 2 ГБ / 7 дней, кэш крупнее
// 100 МБ в корзину не кладём, отменяемо, пока транзакция не «схлопнулась»
// (§7.2).
//
// Модуль переносимый (ADR-004): ни Windows API, ни файлового ввода-вывода.
// Байты переносит platform::vfs, ядро решает, что и куда класть, разбирает
// манифест и считает, кого вытеснить при нехватке места. Поэтому всё
// проверяемое здесь — чистые функции и данные, а наличие файлов на диске
// приходит через пробу, которую реализует платформа.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace mrproper::core {

class TrashError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Лимиты и константы формата (FR-7)
// ---------------------------------------------------------------------------

// Схема манифеста. Повышается только при несовместимой смене формата; на
// меньшей схеме манифест не читаем вовсе, а не читаем «по-старому».
inline constexpr int kTrashManifestSchema = 1;
inline constexpr const char* kTrashManifestFileName = "manifest.json";
inline constexpr const char* kTrashAppDirName = "MrProper";
inline constexpr const char* kTrashDirName = "Trash";

// Лимиты по умолчанию: ≤ 2 ГБ и ≤ 7 дней (FR-7).
// Единицы двоичные (2 * 1024^3, 7 * 24 * 3600) — как их показывает
// formatBytes(bytes, 1, true); круглое «2 ГБ» из спеки не должно означать
// 2 000 000 000 байт на диске, который считает 2 ГиБ.
inline constexpr std::uint64_t kDefaultTrashMaxBytes = 2147483648ull;
inline constexpr std::int64_t kDefaultTrashMaxAgeSeconds = 604800;

// Порог «крупного кэша»: всё, что больше 100 МБ, удаляется напрямую с записью
// в журнал (FR-7: «иначе корзина раздувается»).
inline constexpr std::uint64_t kTrashLargeItemBytes = 104857600ull;

struct TrashLimits {
    std::uint64_t maxBytes{kDefaultTrashMaxBytes};
    std::int64_t maxAgeSeconds{kDefaultTrashMaxAgeSeconds};
    std::uint64_t largeItemBytes{kTrashLargeItemBytes};
};

// ---------------------------------------------------------------------------
// Транзакция и её манифест
// ---------------------------------------------------------------------------

enum class TrashItemKind { File, Directory };

// Состояние транзакции. «Схлопнулась» (Collapsed) означает, что часть
// содержимого вытеснена из корзины навсегда: отменять такую транзакцию уже
// нельзя (§7.2), и UndoService обязан это уважать.
enum class TrashTxState { Open, Committed, Undone, Collapsed };

const char* toString(TrashItemKind kind);
const char* toString(TrashTxState state);

// Один перемещённый объект. Данные, нужные для восстановления, и только они:
// ядро ничего не знает о том, как байты оказались в корзине.
struct TrashItem {
    TrashItemKind kind{TrashItemKind::File};
    std::string originalPath;   // исходный путь, utf-8
    std::string payload;        // имя объекта внутри каталога транзакции, без разделителей
    std::uint64_t bytes{};
    std::uint32_t fileCount{1};
    std::int64_t mtime{};       // unix-секунды, время изменения исходного объекта
    bool readOnly{};
    // SDDL исходного объекта: пустая строка — ACL не менялись или недоступны.
    // Заполняет и применяет платформа, ядло строки не интерпретирует.
    std::string aclSddl;
    // Перенос между томами — это копирование (FR-7): честно показываем, что
    // это долго, и по возможности предлагаем прямой делет.
    bool crossVolume{};
    std::string sourceVolume;   // "\\?\Volume{...}\" — проверяем доступность при восстановлении
};

struct TrashTransaction {
    std::string txId;
    std::int64_t createdAt{};   // unix-секунды
    std::string appVersion;
    std::vector<TrashItem> items;
    TrashTxState state{TrashTxState::Open};
    std::uint64_t purgedBytes{};  // сколько схлопнуто безвозвратно

    std::uint64_t totalBytes() const;
    std::uint32_t itemCount() const;

    // Отменять можно, пока транзакция не схлопнулась и не восстановлена.
    bool undoable() const;
};

// Снимок транзакции для расчёта лимитов: ядру достаточно итогов, а не тела
// манифеста, поэтому лимиты считаются по дешёвому вектору.
struct TrashTransactionInfo {
    std::string txId;
    std::int64_t createdAt{};
    std::uint64_t bytes{};
    std::uint32_t itemCount{};
};

TrashTransactionInfo infoOf(const TrashTransaction& tx);

struct TrashUsage {
    std::uint64_t totalBytes{};
    std::uint32_t transactionCount{};
    std::uint64_t itemCount{};
    std::int64_t oldestAt{};  // 0 — корзина пуста
    std::int64_t newestAt{};
};

TrashUsage summarize(const std::vector<TrashTransactionInfo>& transactions);

// ---------------------------------------------------------------------------
// Манифест: JSON
// ---------------------------------------------------------------------------

json::Value toJson(const TrashItem& item);
json::Value toJson(const TrashTransaction& tx);
TrashItem itemFromJson(const json::Value& value, const std::string& origin);
TrashTransaction transactionFromJson(const json::Value& value, const std::string& origin);

std::string serializeManifest(const TrashTransaction& tx, int indent = 2);
TrashTransaction parseTrashManifest(std::string_view text, const std::string& origin = "trash-manifest.json");

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------

// Корневой каталог корзины: <ProgramData>\MrProper\Trash (FR-7). ProgramData,
// а не каталог пользователя: корзина общая для всех запусков, включая запуск
// с повышенными правами, иначе файл, удалённый администратором, нельзя было бы
// восстановить обычным запуском.
std::string trashRootPath(std::string_view programDataDir);

// Каталог транзакции и путь манифеста в нём. Недопустимый txId — исключение:
// идентификатор приезжает из манифеста, то есть с диска, и не должен уметь
// выйти за пределы корня корзины.
std::string transactionDir(std::string_view trashRoot, std::string_view txId);
std::string manifestPath(std::string_view transactionDirPath);

// Склейка путей: разделитель выбирается по содержимому (обратный слэш или
// двоеточие — значит Windows-путь), лишние разделители не дублируются.
std::string joinPath(std::string_view base, std::string_view leaf);

// ---------------------------------------------------------------------------
// Идентификатор транзакции
// ---------------------------------------------------------------------------

// Метка времени UTC для показа и отчётов: "20260927T225900Z".
std::string formatUtcStamp(std::int64_t nowUnixSeconds);

// "<метка>-<счётчик>-<nonce>", например "20260927T225900Z-000007-1a2b3c4d".
// Считается из переданных значений, а не из системных часов, — иначе тест не
// был бы повторяемым. Счётчик даёт уникальность в пределах секунды, nonce —
// различает два процесса, стартовавших одновременно. Метка внутри txId без
// двоеточий (двоеточие в Windows — это поток данных, такое имя каталога
// нельзя ни создать, ни прочитать), поэтому результат проходит isValidTxId.
std::string makeTxId(std::int64_t nowUnixSeconds, std::uint32_t counter, std::uint32_t nonce);

// Проверка идентификатора: 1..64 символа [0-9A-Za-z-], без ведущего дефиса.
// Всё, что не проходит, в имя каталога не попадает.
bool isValidTxId(std::string_view txId);

// Имя объекта внутри транзакции: 1..64 символа [0-9A-Za-z._-], не "." и не
// "..", без разделителей и двоеточия (никаких потоков данных, никаких хвостов).
bool isValidPayloadName(std::string_view name);

// ---------------------------------------------------------------------------
// Лимиты: что класть в корзину, а что удалять напрямую
// ---------------------------------------------------------------------------

enum class TrashPlacement { Trash, DirectDelete };

struct TrashPlacementPlan {
    TrashPlacement placement{TrashPlacement::Trash};
    bool crossVolume{};             // перенос между томами = копирование
    bool expensive{};               // показываем «это займёт время»
    std::uint64_t bytesAfter{};     // сколько останется в корзине после размещения
    std::uint64_t evictBytes{};     // сколько нужно освободить до лимита
    std::vector<std::string> evictTxIds;  // кого придётся вытеснить (в порядке удаления)
    const char* reason{};           // краткое объяснение (RU) для журнала и UI

    bool undoable() const { return placement == TrashPlacement::Trash; }
    bool needsEviction() const { return evictBytes != 0; }
};

// Решение по одному объекту: в корзину или напрямую, с учётом крупного кэша
// (> 100 МБ), кросс-томового переноса, текущей заполненности и протухших
// транзакций. Ничего не удаляет: возвращает план, решение принимает движок.
// Список транзакций нужен, чтобы назвать, кого вытеснять; из одних итогов
// (перегрузка ниже) вытеснение не перечисляется, но evictBytes считается.
TrashPlacementPlan planTrashPlacement(const TrashLimits& limits,
                                      const std::vector<TrashTransactionInfo>& transactions, std::uint64_t itemBytes,
                                      bool sameVolume, std::int64_t now);

TrashPlacementPlan planTrashPlacement(const TrashLimits& limits, const TrashUsage& usage, std::uint64_t itemBytes,
                                      bool sameVolume, std::int64_t now);

// Что вытеснить, чтобы в корзине освободилось место. Сначала уходят всё, что
// старше maxAgeSeconds, затем — от самой старой транзакции к самой новой.
// Физическое удаление делает платформа, ядро только планирует.
struct TrashEvictionPlan {
    std::vector<std::string> txIds;
    std::uint64_t reclaimBytes{};
    std::uint64_t freedByAge{};
    std::uint64_t freedBySize{};

    bool empty() const { return txIds.empty(); }
};

TrashEvictionPlan planEviction(const TrashLimits& limits, const std::vector<TrashTransactionInfo>& transactions,
                               std::uint64_t incomingBytes, std::int64_t now);

// Транзакции старше возрастного лимита — их чистят независимо от свободного
// места (в том числе при нулевом incomingBytes).
std::vector<std::string> selectExpired(const TrashLimits& limits,
                                       const std::vector<TrashTransactionInfo>& transactions, std::int64_t now);

// ---------------------------------------------------------------------------
// Журнал корзины
// ---------------------------------------------------------------------------

// Транзакции в памяти плюс проверка лимитов. Диском (создание каталога,
// перенос байтов, запись манифеста) занимается платформа; журнал решает, что
// вытеснить, и хранит состояние, из которого движок понимает, можно ли отменять.
class TrashLedger {
public:
    TrashLedger() = default;
    explicit TrashLedger(TrashLimits limits, std::string trashRoot = {});

    const TrashLimits& limits() const { return limits_; }
    void setLimits(const TrashLimits& limits) { limits_ = limits; }

    const std::string& root() const { return root_; }
    void setRoot(std::string trashRoot) { root_ = std::move(trashRoot); }

    // Создаёт транзакцию и возвращает её идентификатор. Идентификаторы не
    // повторяются внутри журнала: счётчик и nonce растут от каждого begin.
    std::string begin(std::int64_t now, const std::string& appVersion = {});

    // Добавляет элемент; незаданный payload получает имя по номеру элемента.
    // false — транзакции нет (забыта или схлопнулась).
    bool addItem(std::string_view txId, TrashItem item);

    // Open -> Committed: манифест записан, содержимое можно восстанавливать.
    bool commit(std::string_view txId);
    // Committed -> Undone: элементы возвращены по местам.
    bool markUndone(std::string_view txId);

    // Схлопывание: содержимое вытеснено, отменять нечего (§7.2). purgedBytes —
    // сколько потеряно безвозвратно (для отчёта).
    static void collapse(TrashTransaction& tx, std::uint64_t purgedBytes);

    // Ставит транзакцию в журнал, как будто её прочитали с диска. Транзакция
    // с уже известным идентификатором заменяется; с некорректным — TrashError.
    bool adopt(TrashTransaction tx);

    // Забывает транзакцию — после того как платформа физически удалила каталог.
    bool forget(std::string_view txId);

    TrashTransaction* find(std::string_view txId);
    const TrashTransaction* find(std::string_view txId) const;
    const std::vector<TrashTransaction>& transactions() const { return transactions_; }

    std::vector<TrashTransactionInfo> info() const;
    TrashUsage usage() const;
    TrashEvictionPlan planEviction(std::uint64_t incomingBytes, std::int64_t now) const;
    std::vector<std::string> expired(std::int64_t now) const;

    // Пути внутри корня журнала — чтобы платформа не склеивала их сама.
    std::string dirFor(std::string_view txId) const;
    std::string manifestFor(std::string_view txId) const;
    std::string payloadFor(std::string_view txId, std::string_view payload) const;

private:
    TrashLimits limits_{};
    std::string root_;
    std::vector<TrashTransaction> transactions_;
    std::uint32_t counter_{};
    std::uint32_t nonce_{};
};

// ---------------------------------------------------------------------------
// Восстановление (FR-7: полное или частичное, по элементу)
// ---------------------------------------------------------------------------

enum class RestoreAction {
    Restore,    // на месте ничего нет — восстанавливаем
    Overwrite,  // место занято, но пользователь разрешил перезапись
    Conflict,   // место занято: не перезаписывать, спросить (FR-7)
    Missing,    // содержимого нет в корзине — восстанавливать нечего
};

struct RestoreItemPlan {
    std::size_t index{};  // индекс элемента в транзакции
    TrashItem item;
    RestoreAction action{RestoreAction::Restore};
};

struct RestoreOptions {
    // Пользователь согласился перезаписать занятые места: конфликты переходят
    // в Overwrite вместо Conflict.
    bool overwriteConflicts{};
};

struct RestorePlan {
    std::vector<RestoreItemPlan> items;
    std::uint64_t restoreBytes{};
    std::uint64_t conflictBytes{};
    std::uint64_t missingBytes{};
    std::size_t restoreCount{};
    std::size_t conflictCount{};
    std::size_t missingCount{};

    bool empty() const { return items.empty(); }
    // Частичное восстановление: что-то восстановится, что-то нет.
    bool partial() const;
    // Короткая сводка (RU) для UI и отчёта.
    std::string explain() const;
};

// Проба места назначения: true, если путь уже существует. Платформа
// реализует её через GetFileAttributesW, юнит-тесты — таблицей строк.
using ExistsProbe = std::function<bool(const std::string& path)>;

// План восстановления. selection — индексы элементов транзакции; пустой
// список означает «всё». Пустая проба означает «на месте ничего не занято».
// Ничего не переносит: план показывает, куда что класть и где спросить.
RestorePlan planRestore(const TrashTransaction& tx, const std::vector<std::size_t>& selection,
                        const ExistsProbe& exists, const RestoreOptions& options = {});

// То же для всех элементов транзакции.
RestorePlan planRestoreAll(const TrashTransaction& tx, const ExistsProbe& exists,
                           const RestoreOptions& options = {});

// Сводка корзины для UI и отчёта (FR-8), ru: «1,2 ГБ из 2 ГБ, транзакций: 3».
std::string trashSummary(const TrashUsage& usage, const TrashLimits& limits);

}  // namespace mrproper::core
