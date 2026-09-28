// Корзина приложения — файловая сторона: перенос байтов, манифест на диске,
// восстановление и вытеснение (SPEC §4 FR-7, §9.1 ADR-005, §5 «Пути»,
// §6.4 «Потоки и отмена»).
//
// Разделение с core::trash. Ядро (ADR-004, переносимое, без Windows) решает,
// что класть в корзину, кого вытеснять при нехватке места, как разбирать
// манифест и что восстанавливать при конфликте; оно ничего не знает о том, как
// байты оказались в корзине. Этот модуль — ровно эта часть: создать каталог
// транзакции, перенести (или скопировать) объект, записать манифест, вернуть
// содержимое на место, физически удалить вытесненное. Решения не принимает
// ничего из перечисленного: TrashMoveResult — это отчёт о том, что случилось,
// а TrashStatus — не «можно/нельзя», а «получилось/не получилось».
//
// Три вещи, ради которых модуль и написан.
//
// 1. Своя корзина, а не системная (ADR-005). Системная не масштабируется на
//    десятки ГБ кэшей и не даёт транзакции: у неё нет ни идентификатора
//    операции, ни манифеста, ни «отменить всё, что было в этом запуске».
//    Своя корзина — это %ProgramData%\MrProper\Trash\<txId>\ плюс manifest.json,
//    и плата за это — место на диске, пока корзину не очистили: поэтому лимиты
//    (≤ 2 ГБ и ≤ 7 дней) и физическое вытеснение тоже часть этого модуля.
//
// 2. Кросс-томовое перемещение = копирование (FR-7). RenameFile не переносит
//    объект между томами, и молчаливый «перенос» через case-подмену буквы диска
//    стоил бы пользователю данных. Здесь это честно: сначала копия целиком,
//    и только потом удаление источника; отмена на любом шаге оставляет источник
//    на месте, а копия считается тем, что реально перенесено. Движок может
//    показать «это займёт время», а крупные транзакции — вообще не начинать
//    (core::planTrashPlacement).
//
// 3. Восстановление возвращает то, что было (FR-7). Манифест хранит исходный
//    путь, размер, mtime и ACL, поэтому восстановление — это не «скопировать
//    обратно», а «вернуть на место с теми же правами и временем». Конфликт
//    (на месте уже что-то есть) — не ошибка платформы, а решение пользователя:
//    core::planRestore переводит его в Overwrite или Conflict, а этот модуль
//    при Conflict ничего не трогает.
//
// Чего модуль не делает намеренно.
//
//  * Не повышает права (§5: повышение — один раз при старте). Отказ доступа —
//    это TrashStatus::AccessDenied с кодом Win32, а не попытка что-то обойти.
//  * Не копирует потоки данных (ADS). §5 требует определить поведение явно:
//    здесь оно «не трогаем» — читается только основной поток, и это же
//    означает, что скрытые потоки мусора не поедут в корзину.
//  * Не обходит reparse points (FR-6). Симлинк и junction не копируются и не
//    разворачиваются: обход защищает от петель и выхода за пределы пути, а
//    junction внутри каталога-кандидата — это почти всегда не тот объект,
//    который пользователь собирался удалить. Сколько таких точек встретилось —
//    в TrashMoveResult::skippedReparsePoints, то есть видно и в логе, и в отчёте.
//  * Не трогает hard links: копирование разрывает связь (в корзине будет
//    отдельная копия каждого имени). Это цена кросс-томового переноса, и она
//    названа здесь, а не обнаружится у пользователя на месте.
//  * Не ходит по симлинкам и не создаёт их: SecurityDescriptor, ADS и reparse
//    points восстанавливаются не «как было», а «безопасно».
//
// Ошибки — значениями, а не исключениями. Обход каталогов и копирование —
// горячий цикл (SPEC §5, §6.4), где отказ это строка отчёта, а не раскрутка
// стека; единственное исключение, которого можно ждать, — std::bad_alloc.
// Каждый отказ несёт TrashStatus, код Win32 и путь, на котором он случился:
// по §12 этого достаточно, чтобы понять отказ без повторного запуска.
//
// Слой Windows. Заголовок намеренно свободен от <windows.h> (как остальные
// заголовки platform/): наружу уходят переносимые типы, коды Win32 — числами
// std::uint32_t. Тексты ошибок собирает formatStatus(), который читает
// FormatMessageW, поэтому соответствуют языку установки (§5).
//
// Порядок работы типичной транзакции:
//
//   core::TrashLedger ledger;                       // что и куда класть
//   const std::string txId = ledger.begin(now);     // Open
//   const platform::TrashOptions opts{.stop = token};
//   for (const auto& candidate : items) {
//       const auto staged = platform::stageTrashItem(..., opts);   // замерил и перенёс
//       if (!staged.ok()) { report(staged.transfer); continue; }
//       ledger.addItem(txId, staged.item);
//   }
//   platform::writeManifest(dir, tx, opts);          // атомарно
//   ledger.commit(txId);                             // Committed — можно отменять
//
// Отмена того же сценария — restoreTrashItems() по core::RestorePlan, после
// чего каталог транзакции убирается через purgeTransactionDir().
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <stop_token>

#include "core/trash.hpp"

namespace mrproper::platform {

// ---------------------------------------------------------------------------
// Состояние операции
// ---------------------------------------------------------------------------

// Итог одной файловой операции. Ok — единственное состояние, в котором
// результат можно класть в отчёт об успехе; остальные несут причину.
enum class TrashStatus : std::uint8_t {
    Ok,              // операция выполнена
    Cancelled,       // отмена по std::stop_token; состояние на диске не тронуто
    NotFound,        // источника или содержимого корзины нет
    AlreadyExists,   // на месте уже есть: восстановление без права перезаписи
    AccessDenied,    // не хватило прав — помогает только повышение (§5)
    InvalidArgument, // путь/идентификатор не прошли проверку (ошибка вызывающего)
    Corrupt,         // манифест не читается: битый JSON или чужая схема
    OutOfSpace,      // на томе назначения не хватило места под копию
    Unsupported,     // операция недоступна для этого объекта (нет ACL и т.п.)
    IoError,         // прочие отказы Win32
    OutOfMemory,     // не хватило памяти на разбор каталога
};

// Стабильное имя состояния: не локализуется, идёт в лог и в JSON-отчёт.
[[nodiscard]] const char* toString(TrashStatus status) noexcept;

// Причина одним предложением: имя состояния плюс текст системы по коду Win32.
// Текст берётся у самой системы (FormatMessageW), поэтому соответствует языку
// установки, а не захардкоженному словарю (SPEC §5, §12).
[[nodiscard]] std::string formatStatus(TrashStatus status, std::uint32_t win32Error);
[[nodiscard]] std::wstring formatStatusWide(TrashStatus status, std::uint32_t win32Error);

// ---------------------------------------------------------------------------
// Общие параметры операций
// ---------------------------------------------------------------------------

// Прогресс для UI: эквивалент того, что показывает счётчик байтов на экране
// очистки (§7.2). bytesTotal/filesTotal равны нулю, пока неизвестно (каталог
// ещё не обойдён или перенос идёт одним rename — считать нечего).
struct TrashProgress {
    std::uint64_t bytesDone{};
    std::uint64_t bytesTotal{};
    std::uint32_t filesDone{};
    std::uint32_t filesTotal{};

    // Доля завершения 0..1; 0, пока размер неизвестно: «не знаю» честнее, чем
    // бегущие 99 % у операции, которая заняла полсекунды.
    [[nodiscard]] double fraction() const noexcept;
};

using TrashProgressFn = std::function<void(const TrashProgress&)>;

// Как часто проверять отмену. §6.4 требует проверять каждые 256 элементов:
// чаще — лишние атомарные чтения на каждом файле миллионного каталога, реже —
// ощутимая задержка отклика кнопки «Отмена».
inline constexpr std::size_t kTrashCancelCheckEvery = 256;

struct TrashOptions {
    std::stop_token stop{};                 // отмена (SPEC §6.4: stop_source/stop_token)
    std::size_t cancelCheckEvery{kTrashCancelCheckEvery};
    bool logFailures{true};                 // §12: отказ виден в логе с путём и кодом
    bool applyTimestamps{true};             // вернуть mtime при восстановлении
    bool applyAcl{true};                    // вернуть SDDL при восстановлении
    bool removeSourceAfterCopy{true};       // кросс-томовый перенос: снести источник
    TrashProgressFn progress{};             // вызывается из рабочего потока
};

// ---------------------------------------------------------------------------
// Пути и сведения об объекте
// ---------------------------------------------------------------------------

// Нормализованный длинный путь в UTF-8: «C:\Dir\File» -> «\\?\C:\Dir\File»,
// «\\server\share\x» -> «\\?\UNC\server\share\x». Префикс \\?\ отключает
// разбор точек, чередование слэшей и MAX_PATH, поэтому приводить путь надо
// один раз, на входе, а не в каждом вызове WinAPI.
[[nodiscard]] std::string longPath(std::string_view pathUtf8);

// Уже нормализованный путь (\\?\… или \\.\…): такой приводить повторно не надо.
[[nodiscard]] bool isLongPath(std::string_view pathUtf8);

// Родительский каталог и имя объекта. Для корня тома возвращают пустой результат
// и сам путь соответственно: подниматься выше тома нельзя, а имя тома — это его
// метка тома, а не имя файла.
[[nodiscard]] std::string parentPath(std::string_view pathUtf8);
[[nodiscard]] std::string fileName(std::string_view pathUtf8);

// Есть ли объект по пути. Проба атрибутов, а не открытие: файла может быть
// много, а открывать каждый только ради «существует ли» дороже в сотни раз.
[[nodiscard]] bool pathExists(std::string_view pathUtf8);
[[nodiscard]] bool isDirectory(std::string_view pathUtf8);

// Проба для core::planRestore: «занято ли место назначения» (FR-7).
[[nodiscard]] core::ExistsProbe existsProbe();

// Том, на котором лежит объект. volumeGuid — то, что возвращает
// GetVolumeNameForVolumeMountPointW («\\?\Volume{…}\»): именно эту строку
// core::TrashItem::sourceVolume кладёт в манифест, и по ней при восстановлении
// проверяется, что том на месте (съёмный диск могли вынуть).
struct TrashVolumeInfo {
    std::string volumeGuid;    // «\\?\Volume{…}\» либо «C:\» для буквы диска
    std::string mountPoint;    // первая точка монтирования: «C:\»
    std::uint64_t freeBytes{};
    std::uint64_t totalBytes{};
    bool valid{};

    [[nodiscard]] bool ok() const noexcept { return valid; }
};

[[nodiscard]] TrashVolumeInfo volumeOfPath(std::string_view pathUtf8);

// Один том или разные. known=false — сравнивать нечем (том не определился,
// путь сетевой или точки монтирования нет), и решение о кросс-томовом переносе
// в этом случае принимает не сравнение, а сам MoveFileEx.
[[nodiscard]] bool sameVolume(std::string_view firstUtf8, std::string_view secondUtf8, bool& known);

// Сколько свободно на томе, где лежит путь (проверка «влезет ли копия»).
[[nodiscard]] std::uint64_t freeBytesOfPath(std::string_view pathUtf8);

// Что платформа знает об объекте «до» переноса. Заполняется один раз, и именно
// эти поля core::TrashItem уезжают в манифест: без них восстановление не сможет
// вернуть ни права, ни время изменения.
struct TrashFacts {
    std::uint64_t bytes{};        // логический размер (файл или сумма по каталогу)
    std::uint32_t fileCount{};    // 1 для файла; число файлов в дереве для каталога
    std::int64_t mtime{};         // unix-секунды, время изменения объекта
    bool readOnly{};              // FILE_ATTRIBUTE_READONLY на объекте
    bool directory{};
    bool sparse{};                // FILE_ATTRIBUTE_SPARSE_FILE — полезно отчёту
    std::string aclSddl;          // SDDL; пусто — ACL не менялись или недоступны
    std::string volumeGuid;       // «\\?\Volume{…}\» исходного тома
    std::uint32_t skippedReparsePoints{};  // junction/symlink, встреченные в дереве
};

// Замер объекта: байты, число файлов, mtime, признаки и ACL. Рекурсивный
// обход с пропуском reparse points (FR-6), поэтому каталог в миллион файлов
// читается потоково и держит в памяти один уровень, а не все пути (§5).
[[nodiscard]] TrashStatus readPathFacts(std::string_view pathUtf8, TrashFacts& facts, const TrashOptions& options);

// ---------------------------------------------------------------------------
// Корень корзины и каталоги транзакций
// ---------------------------------------------------------------------------

// %ProgramData% из переменной окружения. Пусто, если переменной нет (сервисная
// сессия): подставлять выдуманный путь хуже, чем сказать вызывающему, что
// корневой каталог неизвестен. SHGetFolderPath сознательно не используется —
// он живёт в shell32, которого у слоя platform нет в списке библиотек.
[[nodiscard]] std::string programDataDir();

// <ProgramData>\MrProper\Trash (FR-7). Пусто, если неизвестен ProgramData.
[[nodiscard]] std::string defaultTrashRoot();

// Создать каталог вместе со всеми недостающими родителями (ProgramData\MrProper\Trash).
// Существующий каталог — успех: повторный запуск не должен считаться ошибкой.
[[nodiscard]] TrashStatus ensureDirectoryTree(std::string_view pathUtf8, const TrashOptions& options);

// Каталог транзакции: <trashRoot>\<txId>. Идентификатор проверяется
// core::isValidTxId, поэтому «../../Windows» из манифеста на диске каталогом
// стать не может — вместо исключения возвращается InvalidArgument.
[[nodiscard]] TrashStatus transactionDirectoryFor(std::string_view trashRoot, std::string_view txId,
                                                 std::string& dirOut, const TrashOptions& options);

// Создать каталог транзакции, куда класть элементы. Создаётся целиком, а не
// «лениво»: транзакция, чей каталог не создался, не должна оставить на диске
// наполовину перенесённое содержимое.
[[nodiscard]] TrashStatus createTransactionDirectory(std::string_view trashRoot, std::string_view txId,
                                                     std::string& dirOut, const TrashOptions& options);

// ---------------------------------------------------------------------------
// Перенос в корзину
// ---------------------------------------------------------------------------

// Что именно переносим. Назначение по умолчанию — <transactionDir>\<payload>,
// поэтому идентификатор элемента (payload) задаёт ядро, а платформа не может
// придумать имя сама: имя элемента восстановления читается из манифеста.
struct TrashMoveRequest {
    std::string sourcePath;     // utf-8, откуда забираем
    std::string transactionDir; // utf-8, каталог транзакции
    std::string payload;        // имя объекта внутри транзакции (core::isValidPayloadName)
    // Переопределение назначения. Непусто — используется как есть (восстановление
    // по конкретному пути); пусто — transactionDir\payload.
    std::string destinationPath;
    // Заменить существующий файл назначения. Только для восстановления с
    // core::RestoreAction::Overwrite: молча перезаписывать файл, который может
    // быть важнее удалённого, FR-7 запрещает.
    bool overwriteDestination{};
};

// Итог переноса. Байты и файлы — фактически перенесённые, а не исходные:
// кросс-томовый перенос, прерванный на середине, обязан показать, сколько
// действительно ушло в корзину, иначе отчёт соврёт о свободном месте.
struct TrashMoveResult {
    TrashStatus status{TrashStatus::IoError};
    std::uint32_t win32Error{};   // код Win32; 0 — отказ не из Win32
    std::string failedPath;       // utf-8, на каком пути остановились
    TrashFacts facts{};           // что измерили до переноса
    std::uint64_t bytesMoved{};
    std::uint32_t filesMoved{};
    std::uint32_t skippedReparsePoints{};
    bool crossVolume{};           // перенос оказался копированием
    bool sourceRemoved{};         // источник удалён (кросс-томовый случай)
    bool destinationRemoved{};    // копия снята откатом после сорвавшегося копирования
    std::string destinationPath;  // utf-8, куда фактически легло

    [[nodiscard]] bool ok() const noexcept { return status == TrashStatus::Ok; }
    // Исходник цел — значит, откат отработал и пользователь ничего не потерял.
    [[nodiscard]] bool sourceIntact() const noexcept {
        return status != TrashStatus::Ok && !sourceRemoved;
    }
};

// Перенос объекта в каталог транзакции. Один и тот же вызов обслуживает и
// обычный случай (тот же том — атомарное переименование), и кросс-томовый
// (FR-7: копирование целиком, и только потом удаление источника).
//
// Что происходит при отмене: rename ничего не откатывает (он атомарен), а
// копирование снимает недокопированное содержимое и возвращает Cancelled с
// нетронутым источником. То есть отмена не оставляет ни «полуфайла» в
// корзине, ни «наполовину удалённого» исходника.
[[nodiscard]] TrashMoveResult moveToTrash(const TrashMoveRequest& request, const TrashOptions& options);

// Замерить и перенести элемент транзакции в один вызов: на выходе core::TrashItem
// с заполненными байтами, mtime, ACL и признаком кросс-томового переноса —
// то есть ровно то, что обязано попасть в манифест для восстановления.
struct TrashStageRequest {
    std::string transactionDir;
    std::string originalPath;  // utf-8, откуда забираем; это же вернётся в item
    std::string payload;       // имя элемента внутри транзакции
};

struct TrashStageResult {
    TrashMoveResult transfer{};
    core::TrashItem item{};  // заполнен, только если transfer.ok()

    [[nodiscard]] bool ok() const noexcept { return transfer.ok(); }
};

[[nodiscard]] TrashStageResult stageTrashItem(const TrashStageRequest& request, const TrashOptions& options);

// ---------------------------------------------------------------------------
// Манифест на диске
// ---------------------------------------------------------------------------

// Записать manifest.json атомарно: сначала <manifest>.tmp в том же каталоге,
// затем MoveFileExW с REPLACE_EXISTING|WRITE_THROUGH. Прямая запись на месте
// оставила бы после сбоя питания обрезанный JSON, который при следующем запуске
// выглядел бы как «транзакция есть, а восстанавливать нечего».
[[nodiscard]] TrashStatus writeManifest(std::string_view transactionDir, const core::TrashTransaction& tx,
                                         const TrashOptions& options);

// Прочитать манифест каталога. Битый или чужой манифест — это TrashStatus::Corrupt,
// а не исключение: один испорченный файл не должен ронять разбор всей корзины.
[[nodiscard]] TrashStatus readManifest(std::string_view transactionDir, core::TrashTransaction& txOut);

// Каталоги транзакций вместе с прочитанными манифестами. Каталог без читаемого
// манифеста попадает в broken: его нельзя ни восстановить, ни учётно посчитать,
// и молча выбросить его содержимое — тоже нельзя, поэтому решение остаётся за
// вызывающим (обычно это purgeTransactionDir()).
struct TrashInventory {
    std::vector<core::TrashTransaction> transactions;
    std::vector<std::string> brokenDirs;  // каталоги без читаемого манифеста
    std::uint32_t scannedDirs{};

    [[nodiscard]] core::TrashUsage usage() const;
    [[nodiscard]] std::vector<core::TrashTransactionInfo> info() const;
    [[nodiscard]] bool empty() const noexcept { return transactions.empty(); }
};

[[nodiscard]] TrashInventory loadInventory(std::string_view trashRoot, const TrashOptions& options);

// ---------------------------------------------------------------------------
// Восстановление (FR-7: полное или частичное, по элементу)
// ---------------------------------------------------------------------------

struct TrashRestoreRequest {
    std::string transactionDir;    // откуда забираем содержимое
    core::TrashItem item{};        // originalPath, payload, aclSddl, mtime
    core::RestoreAction action{core::RestoreAction::Restore};
    std::string destinationPath;    // по умолчанию item.originalPath
};

struct TrashRestoreResult {
    TrashStatus status{TrashStatus::IoError};
    std::uint32_t win32Error{};
    std::string failedPath;
    bool restored{};        // содержимое вернулось на место
    bool conflictSkipped{}; // действие было Conflict: на месте занято, не трогали
    bool crossVolume{};     // возврат оказался копированием (корзина на другом томе)
    std::uint64_t bytesRestored{};
    std::uint32_t filesRestored{};
    std::uint32_t skippedReparsePoints{};

    [[nodiscard]] bool ok() const noexcept { return status == TrashStatus::Ok; }
};

// Вернуть один элемент на место. Missing — в корзине нет содержимого (жёсткая
// ссылка была удалена вытеснением), Conflict — на месте занято и перезапись не
// разрешена: в обоих случаях ничего не трогаем, а вызывающий решает, что
// показывать пользователю.
[[nodiscard]] TrashRestoreResult restoreTrashItem(const TrashRestoreRequest& request, const TrashOptions& options);

// Прогнать план core::planRestore: план уже решил, что восстанавливать, что
// спросить и чего в корзине уже нет, платформа только исполняет.
struct TrashRestoreSummary {
    std::vector<TrashRestoreResult> items;
    std::uint32_t restoredCount{};
    std::uint32_t conflictCount{};
    std::uint32_t missingCount{};
    std::uint32_t failedCount{};
    std::uint64_t restoredBytes{};
    bool cancelled{};

    [[nodiscard]] bool allRestored() const noexcept { return failedCount == 0 && conflictCount == 0 && !cancelled; }
};

[[nodiscard]] TrashRestoreSummary restoreTrashItems(std::string_view transactionDir, const core::RestorePlan& plan,
                                                    const TrashOptions& options);

// ---------------------------------------------------------------------------
// Вытеснение и очистка (FR-7: лимиты 2 ГБ / 7 дней)
// ---------------------------------------------------------------------------

struct TrashPurgeResult {
    TrashStatus status{TrashStatus::Ok};
    std::uint32_t win32Error{};
    std::string failedPath;
    std::uint64_t removedBytes{};
    std::uint32_t removedFiles{};
    std::uint32_t removedDirs{};
    std::uint32_t skippedReparsePoints{};
    bool cancelled{};
    bool notFound{};  // удалять было нечего — это успех, а не отказ

    [[nodiscard]] bool ok() const noexcept {
        return status == TrashStatus::Ok || status == TrashStatus::NotFound;
    }
};

// Рекурсивное удаление пути. Reparse points удаляются как ссылки, без обхода:
// иначе junction уводил бы удаление за пределы мусора (FR-6).
[[nodiscard]] TrashPurgeResult purgePath(std::string_view pathUtf8, const TrashOptions& options);

// Убрать каталог транзакции целиком. Вызывается после того, как ядро забыло
// транзакцию (core::TrashLedger::forget) или решило, что она больше не нужна.
[[nodiscard]] TrashPurgeResult purgeTransactionDirectory(std::string_view transactionDir, const TrashOptions& options);

// Физически удалить вытесненное. План (кого и в каком порядке) считает ядро —
// core::planEviction, здесь только исполнение и отчёт о том, что удалилось.
struct TrashPurgeSummary {
    std::vector<TrashPurgeResult> results;  // по каталогу из плана
    std::uint64_t removedBytes{};
    std::uint32_t removedDirs{};
    std::uint32_t failedCount{};
    std::vector<std::string> failedDirs;
    bool cancelled{};

    [[nodiscard]] bool empty() const noexcept { return results.empty(); }
};

[[nodiscard]] TrashPurgeSummary purgeEviction(std::string_view trashRoot, const core::TrashEvictionPlan& plan,
                                              const TrashOptions& options);

// Сколько байт занимает корзина на диске фактически (обход файлов), в отличие от
// usage(), который считает по манифестам. Расхождение бывает всегда: манифест
// мог не записаться, а чужой каталог мог остаться. Для отчёта честнее второе.
[[nodiscard]] TrashPurgeResult measureTrashRoot(std::string_view trashRoot, const TrashOptions& options);

// ---------------------------------------------------------------------------
// ACL
// ---------------------------------------------------------------------------

// SDDL объекта для манифеста (FR-7: исходный путь, размер, mtime, ACL).
// Пустая строка — ACL не менялись или недоступны текущему пользователю: это
// нормально, и различие хранится в манифесте как пустая строка.
[[nodiscard]] std::string readSecuritySddl(std::string_view pathUtf8);

// Вернуть ACL объекту при восстановлении. Отказ доступа не фатален: файл
// возвращён, права — нет, и это видно по TrashStatus::AccessDenied.
[[nodiscard]] TrashStatus applySecuritySddl(std::string_view pathUtf8, std::string_view sddl);

}  // namespace mrproper::platform
