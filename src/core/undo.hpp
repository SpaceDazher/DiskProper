// Отмена очистки: корзина приложения, лимиты, конфликты и восстановление
// (SPEC §4 FR-7, §7.2 «Отмена (Ctrl+Z) доступна, пока транзакция не схлопнулась»).
//
// Переносимый модуль: без Windows API (SPEC §6.1 — ядро собирается на любом
// хосте). Модуль не ходит по диску и не двигает файлы: работа с файловой системой
// и учёт занятого места делает platform::vfs, оркестровку — engine::UndoService.
// Здесь только то, что обязано быть детерминированным и тестируемым без диска:
//   * модель транзакции и манифест manifest.json (разбор/сериализация);
//   * решения по лимитам корзины (2 ГБ / 7 дней / крупные элементы → Delete);
//   * оценка «это займёт время» для кросс-томового перемещения;
//   * план восстановления: частичное, по элементу, с разбором конфликтов.
//
// Имена модуля намеренно не пересекаются с соседним core::trash (манифест,
// лимиты, вытеснение): оба описывают соседние стороны FR-7, живут в одном
// пространстве имён mrproper::core и обязаны сосуществовать в одной сборке —
// одинаковое имя в двух заголовках даёт redefinition, а одинаковая свободная
// функция — LNK2005 при линковке обоих объектов. Поэтому здесь UndoError,
// UndoTransaction, UndoLimits, UndoPlan, RestoreDecision, concatPath.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mrproper::core {

// Ошибка манифеста или некорректные входные данные. Как RuleError в правилах:
// с именем файла и полем — по одному тексту видно, что чинить.
class UndoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Текущая версия манифеста. Неизвестная (более новая) версия — отказ, а не
// догадка: прочитать чужой формат и записать поверх — способ потерять корзину.
inline constexpr int kUndoManifestSchema = 1;

// Имя файла манифеста внутри каталога транзакции (SPEC §4 FR-7).
inline constexpr const char* kUndoManifestFileName = "manifest.json";

// Порог «это займёт время» для кросс-томового копирования: ниже — копируем
// молча, выше — UI обязан предупредить пользователя (SPEC §4 FR-7).
inline constexpr std::uint64_t kSlowCopySeconds = 5;

// Скорость копирования, если платформа не передала замер: 80 МиБ/с — консервативная
// оценка (медленный HDD, а не NVMe), чтобы «это займёт время» не пропало из UI.
inline constexpr std::uint64_t kDefaultCopyThroughputBytesPerSec = 80ull * 1024 * 1024;

// Начиная с этого объёма крупная транзакция выгоднее удалить напрямую,
// чем тащить через корзину (SPEC §4 FR-7: «большие транзакции предлагаем сразу
// прямой делет»).
inline constexpr std::uint64_t kLargeTransactionBytes = 512ull * 1024 * 1024;

// Суффикс, который добавляется к имени при восстановлении «рядом» (Rename).
inline constexpr const char* kRestoreSuffix = ".mrproper-restore";

// ---------------------------------------------------------------------------
// Ограничения корзины
// ---------------------------------------------------------------------------
// Значения по умолчанию — ровно те, что требует SPEC §4 FR-7: ≤ 2 ГБ и ≤ 7 дней,
// элементы крупнее 100 МБ удаляются напрямую.
struct UndoLimits {
    std::uint64_t maxTotalBytes{2ull * 1024 * 1024 * 1024};
    std::int64_t maxAgeSeconds{7 * 24 * 60 * 60};
    std::uint64_t largeItemThresholdBytes{100ull * 1024 * 1024};
};

// ---------------------------------------------------------------------------
// Модель транзакции
// ---------------------------------------------------------------------------

// Одна запись манифеста: что унесли с исходного места (SPEC §4 FR-7 — исходный
// путь, размер, mtime, ACL).
struct TrashEntry {
    std::string originalPath;     // UTF-8 путь до очистки
    std::string storedPath;       // путь внутри каталога транзакции, всегда относительный
    std::uint64_t sizeBytes{};    // аллоцированный размер (то, что вернёт freeBytes)
    std::int64_t modifiedUnix{};  // mtime исходного элемента, unix-секунды; 0 — неизвестно
    std::int64_t createdUnix{};   // когда элемент положили в корзину
    std::uint32_t fileCount{};
    std::string aclSddl;          // ACL в SDDL; пусто — ACL не переносили
    std::string volumeGuidPath;   // том-источник: нужен, чтобы отличить кросс-томовое перемещение
    bool isDirectory{};           // каталог восстанавливается целиком, тип важен для конфликтов
    bool restored{};              // уже возвращён: повторно не восстанавливаем
    std::int64_t restoredUnix{};
};

// Каталог %ProgramData%\MrProper\Trash\<txId> вместе с manifest.json.
struct UndoTransaction {
    int schemaVersion{kUndoManifestSchema};
    std::string txId;
    std::string appVersion;
    std::int64_t createdUnix{};
    bool collapsed{};  // «схлопнулась»: транзакция вытеснена или восстановлена целиком
    std::vector<TrashEntry> entries;

    std::uint64_t totalBytes() const;
    std::uint64_t restorableBytes() const;
    std::size_t entryCount() const { return entries.size(); }
    std::size_t restorableCount() const;

    TrashEntry* find(std::string_view originalPath);
    const TrashEntry* find(std::string_view originalPath) const;
};

// Отмена доступна, пока транзакция не схлопнулась и в ней есть что возвращать
// (SPEC §7.2 — иначе Ctrl+Z в UI должен быть серым).
bool undoAvailable(const UndoTransaction& tx);

// Пустая транзакция с заданными реквизитами: платформа создаёт каталог, ядро —
// запись манифеста.
UndoTransaction makeTrashTransaction(std::string txId, std::string appVersion, std::int64_t nowUnix);

// ---------------------------------------------------------------------------
// Манифест: <trashRoot>/<txId>/manifest.json
// ---------------------------------------------------------------------------
// Склейка пути с учётом завершающего разделителя (оба вида разделителя).
std::string concatPath(std::string_view base, std::string_view leaf);
std::string trashTransactionDir(std::string_view trashRoot, std::string_view txId);
std::string trashManifestPath(std::string_view trashRoot, std::string_view txId);

// indent = 2 — «пишется во временный файл и переименовывается» человек читает
// diff глазами; -1 — компактно.
std::string serializeTrashManifest(const UndoTransaction& tx, int indent = 2);

// Разбор манифеста. Бросает UndoError с указанием файла и поля.
// Терпимость к полям будущих версий намеренная (игнорируются), но версия схемы
// проверяется строго: неизвестная версия — отказ.
UndoTransaction parseUndoManifest(std::string_view text, const std::string& origin);

// storedPath обязан быть относительным и не содержать «..»/корня: манифест может
// оказаться подделанным или приехать из повреждённого состояния, а путь из него
// уходит в файловые операции. Проверка дешёвая и обязана быть до любого чтения.
bool isSafeStoredPath(std::string_view storedPath);

// ---------------------------------------------------------------------------
// Лимиты: что можно положить, что вытеснить
// ---------------------------------------------------------------------------

// Что делать с кандидатом при очистке.
enum class TrashDisposition {
    ToTrash,       // переносим в корзину приложения
    DirectDelete,  // крупный элемент: удаляем напрямую, пишем в журнал
    NoSpace,       // в корзину не влезает даже после вытеснения старых транзакций
};

const char* toString(TrashDisposition disposition);

// Состояние корзины на диске: что занято и какие транзакции ещё лежат.
// totalBytes задаёт платформа (свободное место «на глаз» ненадёжно), транзакции
// нужны для вытеснения и для проверки «не схлопнулась».
struct UndoTrashState {
    std::uint64_t totalBytes{};
    std::vector<UndoTransaction> transactions;
};

// Один элемент на очистку.
struct TrashRequest {
    std::uint64_t sizeBytes{};
    bool isDirectory{};
    std::string volumeGuidPath;
    std::string displayName;  // для текста в журнале
};

struct TrashAdmission {
    TrashDisposition disposition{TrashDisposition::ToTrash};
    std::uint64_t bytesAccepted{};        // сколько реально занимает элемент в корзине
    std::uint64_t bytesOverLimit{};       // насколько не влезает в лимит
    std::uint64_t bytesToEvict{};         // сколько нужно вытеснить, чтобы влезло
    std::vector<std::string> evictTxIds;  // кандидаты на вытеснение, самые старые первыми
    bool evictExpiredOnly{};              // вытесняется только просроченное — отмена не страдает
    std::string note;                     // человекочитаемое объяснение (ru) для журнала и UI

    bool accepted() const { return disposition != TrashDisposition::NoSpace; }
    // Корзина недоступна — элемент удаляется напрямую (журнал это уже пишет).
    bool fallsBackToDirectDelete() const { return disposition != TrashDisposition::ToTrash; }
};

// Решение по одному элементу: крупный (> 100 МБ) — сразу Delete, иначе влезает
// ли в корзину с учётом возраста (7 дней) и вытеснения (2 ГБ).
TrashAdmission admitToTrash(const UndoLimits& limits, const UndoTrashState& usage, const TrashRequest& request,
                            std::int64_t nowUnix);

// Порядок вытеснения: сперва просроченное по возрасту (его всё равно чистить),
// затем самое старое из живых. Внутри — по времени создания, при равенстве по
// txId, чтобы план был воспроизводим.
std::vector<std::string> orderForEviction(const UndoLimits& limits, const UndoTrashState& usage, std::int64_t nowUnix);

struct TrashPurgePlan {
    std::vector<std::string> expiredTxIds;  // старше maxAgeSeconds — чистятся всегда
    std::vector<std::string> overflowTxIds; // вытесняются ради места, самые старые первыми
    std::uint64_t bytesReclaimed{};
    std::uint64_t bytesNeeded{};
    bool canAdmit{true};  // хватит ли места после вытеснения
};

TrashPurgePlan planTrashPurge(const UndoLimits& limits, const UndoTrashState& usage, std::uint64_t neededBytes,
                              std::int64_t nowUnix);

// ---------------------------------------------------------------------------
// Кросс-томовое перемещение = копирование
// ---------------------------------------------------------------------------
// «Переместить» между томами физически невозможно: либо копируем, либо сначала
// удаляем. Пользователь должен видеть цену до нажатия, а не после.
struct CopyEstimate {
    bool crossVolume{};        // перемещение между томами (или том неизвестен — цена как у копирования)
    bool volumeKnown{true};    // оба тома известны: оценка точная, а не «на глаз»
    std::uint64_t bytes{};
    std::uint64_t throughputBytesPerSec{};
    std::uint64_t seconds{};
    bool slowEnoughToWarn{false};         // «это займёт время»
    bool recommendsDirectDelete{false};   // крупная транзакция: предложить Delete
};

CopyEstimate estimateTransfer(std::uint64_t bytes, std::string_view fromVolume, std::string_view toVolume,
                              std::uint64_t throughputBytesPerSec = kDefaultCopyThroughputBytesPerSec);

// Цена восстановления транзакции: сумма возвращаемых байт и копирование, если
// исходный том отличается от тома корзины.
CopyEstimate estimateRestoreCost(const UndoTransaction& tx, std::string_view trashVolume,
                                  std::uint64_t throughputBytesPerSec = kDefaultCopyThroughputBytesPerSec);

// ---------------------------------------------------------------------------
// Восстановление: конфликты, частичное восстановление, итог
// ---------------------------------------------------------------------------

// Что мешает вернуть элемент на место.
enum class RestoreConflict {
    None,               // конфликта нет
    TargetExists,       // на месте уже есть файл или каталог
    TargetTypeMismatch, // тип не совпадает: каталог на месте, а вернуть нужно файл
    TargetUnknown,      // платформа не смогла описать цель — восстанавливать вслепую нельзя
    TargetLocked,       // файл держит процесс (Restart Manager)
    ParentMissing,      // родительского каталога нет
    NotEnoughSpace,     // на целевом томе не хватает места
    AlreadyRestored,    // элемент уже возвращён: повторно не трогаем
    MissingInTrash,     // содержимое потеряно — запись манифеста есть, файла нет
};

const char* toString(RestoreConflict conflict);

// Ответ пользователя на конфликт. Ask — значение по умолчанию: FR-7 требует
// «не перезаписывать, спросить», поэтому молчаливые Skip/Overwrite выбирает
// только UI после явного действия человека.
enum class ConflictPolicy {
    Ask,
    Skip,
    Overwrite,
    Rename,
};

const char* toString(ConflictPolicy policy);

// Что план восстановления предлагает сделать с записью.
enum class RestoreDecision {
    Restore,   // вернуть на исходное место
    Skip,      // не возвращать (пропущено, отказ пользователя, уже возвращено)
    Rename,    // вернуть рядом под другим именем
    Overwrite, // заменить существующее
};

const char* toString(RestoreDecision action);

// Сведения о целевом месте, которые знает платформа (SPEC §6.2 platform::vfs).
// Ядро их не добывает и не проверяет: план строится из снимка состояния.
struct RestoreTargetInfo {
    std::string path;
    bool exists{};
    bool isDirectory{};
    bool locked{};
    bool parentExists{true};
    std::uint64_t freeBytes{};
    std::string volumeGuidPath;
    bool contentExists{true};  // ложь, если в корзине нет самого элемента
};

// Выборка: пустой entryIndexes — полное восстановление транзакции, иначе
// частичное по перечисленным индексам.
struct RestoreRequest {
    std::string txId;
    std::vector<std::size_t> entryIndexes;
    ConflictPolicy conflictPolicy{ConflictPolicy::Ask};
    bool restoreTimestamps{true};
    bool restoreAcl{true};
    std::string trashVolume;  // том корзины: нужен для оценки кросс-томового копирования
};

struct UndoPlanEntry {
    std::size_t entryIndex{};
    std::string originalPath;
    std::uint64_t sizeBytes{};
    RestoreDecision action{RestoreDecision::Restore};
    std::string targetPath;  // при Rename — новое имя
    RestoreConflict conflict{RestoreConflict::None};
    bool needsUserDecision{false};
    std::string note;  // человекочитаемое объяснение (ru)
};

struct UndoPlan {
    std::string txId;
    bool fullRestore{};  // восстанавливаем транзакцию целиком
    std::vector<UndoPlanEntry> entries;
    std::uint64_t bytesPlanned{};
    std::uint64_t bytesSkipped{};
    std::uint32_t conflictCount{};
    std::uint32_t skippedCount{};
    bool requiresUserDecision{false};
    CopyEstimate cost;                    // «это займёт время»
    std::vector<std::string> questions;   // тексты вопросов для UI, по одному на конфликт
};

// План восстановления по снимку состояния целей. Ничего не пишет на диск:
// платформа выполняет план и возвращает результат, ядро считает итог.
// targets[i] описывает цель для tx.entries[i]; вектор короче транзакции — ошибка
// платформы (UndoError), а не «цель свободна»: восстановление вслепую перезаписало
// бы чужой файл, а FR-7 запрещает перезаписывать без вопроса. Пустой targets[i].path
// означает «цель неизвестна» и даёт RestoreConflict::TargetUnknown.
UndoPlan buildRestorePlan(const UndoTransaction& tx, const RestoreRequest& request,
                          const std::vector<RestoreTargetInfo>& targets);

// Ответы на вопросы плана. Индексы — номера записей в tx.entries.
struct RestoreAnswers {
    std::vector<std::pair<std::size_t, ConflictPolicy>> byEntry;
    std::vector<std::pair<std::size_t, std::string>> renameTo;  // новый путь для Rename
};

// Пересчитывает план по ответам пользователя. Записи без ответа остаются
// пропущенными: «не ответил» не должно превращаться в перезапись.
UndoPlan applyRestoreAnswers(UndoPlan plan, const RestoreAnswers& answers);

// Имя для «восстановить рядом»: путь с суффиксом перед расширением
// (C:\a\file.txt -> C:\a\file.mrproper-restore.txt). attempt > 1 добавляет
// номер — так имя остаётся узнаваемым и не конфликтует само с собой.
std::string restoreRenameTarget(const std::string& path, int attempt = 1,
                                std::string_view suffix = kRestoreSuffix);

// Результат фактического восстановления по одной записи (ошибка не фатальна —
// SPEC §4 FR-6: остальные операции продолжаются).
struct RestoreEntryResult {
    std::size_t entryIndex{};
    bool ok{};
    std::uint64_t bytesRestored{};
    std::string error;  // HRESULT/текст от платформы
};

struct RestoreOutcome {
    std::uint32_t restoredCount{};
    std::uint32_t skippedCount{};
    std::uint32_t failedCount{};
    std::uint32_t notAttemptedCount{};  // план есть, а результата нет: отмена или обрыв
    std::uint64_t bytesRestored{};
    std::vector<RestoreEntryResult> failures;
    std::string summary;  // «4 из 6 восстановлено, 1 ошибка» — для журнала и отчёта
};

RestoreOutcome summarizeRestore(const UndoPlan& plan, const std::vector<RestoreEntryResult>& results);

// Манифест после восстановления: успешно возвращённые записи помечаются
// (повторно не восстанавливаются), а транзакция без остатка схлопывается —
// отмены больше нет (SPEC §7.2).
UndoTransaction applyRestored(const UndoTransaction& tx, const UndoPlan& plan,
                              const std::vector<RestoreEntryResult>& results, std::int64_t nowUnix);

}  // namespace mrproper::core
