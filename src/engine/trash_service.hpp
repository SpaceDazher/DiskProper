// Корзина приложения на диске: оркестрация над core::trash (решения) и
// platform::vfs_trash (байты) — лимиты, перенос, вытеснение, очистка (SPEC §4
// FR-7, §9.1 ADR-005).
//
// Спека: §4 FR-7 («своя корзина %ProgramData%\MrProper\Trash\<txId>\ +
// manifest.json; ≤ 2 ГБ и ≤ 7 дней; кэши > 100 МБ — прямое удаление с записью
// в журнал; кросс-томовое перемещение = копирование; восстановление полное или
// частичное, с проверкой конфликтов»), §6.2 (`engine::*` — оркестрация,
// транзакции, undo), §6.4 (отмена `std::stop_token`, проверка каждые 256
// элементов), §7.2 («отмена доступна, пока транзакция не схлопнулась»),
// §5 (устойчивость: ни один отказ не роняет процесс), §12 (все ошибки в логе с
// путём и HRESULT), §9.1 ADR-004/ADR-005.
//
// ---------------------------------------------------------------------------
// Зачем модуль, если решения и байты уже есть
// ---------------------------------------------------------------------------
//
// Между «core::planTrashPlacement сказал: в корзину» и «на диске лежит
// каталог с манифестом» лежит последовательность, которую не должен брать на
// себя ни один из двух соседей, потому что оба о ней не знают:
//
//   core::trash — переносимое ядро (ADR-004): решает, что класть в корзину, кого
//   вытеснять при нехватке места, как выглядит манифест и план восстановления.
//   Ни файлового ввода-вывода, ни Windows, ни часов.
//
//   platform::vfs_trash — Win32: создаёт каталог, зерит (или копирует при
//   кросс-томовом переносе), пишет манифест атомарно, возвращает содержимое на
//   место, физически сносит вытесненное. Решений не принимает: TrashMoveResult —
//   отчёт о том, что случилось, а не «можно/нельзя».
//
// Между ними — то, ради чего и существует этот модуль:
//
//   1. Порядок. Вытеснить ДО переноса, а не после: перенос увеличивает корзину,
//      и вытеснение «по факту» означало бы лишний копи-паста-цикл на самом
//      дорогом шаге. То есть план вытеснения (core) исполняется до того, как
//      начнётся байтовый обмен (platform), и обе фазы обязаны быть в одной
//      транзакции с честным откатом.
//   2. Единый журнал. `core::TrashLedger` живёт в памяти процесса, а каталоги —
//      на диске, и после перезапуска приложения они не сойдутся: манифест мог
//      не записаться, каталог мог остаться от прерванного копирования. Отсюда
//      `open()` — подъём состояния с диска в журнал, и он же отвечает за то,
//      что лимит «2 ГБ» считается по тому, что реально лежит, а не по тому, что
//      помнит процесс.
//   3. Лимиты как действие, а не как число. «≤ 2 ГБ и ≤ 7 дней» — это обещание
//      пользователю, и кто-то должен выполнить его: посчитать возраст, выбрать
//      жертв, снести их каталоги, обновить журнал и — главное — честно сказать,
//      что транзакция стала неотменяемой (§7.2). Ядро жертв выбирает, платформа
//      сносит, а «схлопнулась» отмечает тот, кто знает и про учёт, и про то, что
//      больше нечего отменять, — то есть сервис.
//   4. Целостность манифеста. Транзакция считается отменяемой только после
//      успешной записи manifest.json, а частично восстановленная транзакция
//      обязана перестать обещать уже возвращённые элементы, иначе следующий
//      запуск предложит восстановить файл, которого больше нет.
//   5. Границы при отказе. Ни один отказ не должен оставить на диске
//      наполовину перенесённый мусор: каталог транзакции создаётся целиком, при
//      сорвавшемся копировании платформа убирает копию сама, а `abandon()`
//      сносит недоделанную транзакцию, потому что незаписанный манифест — это
//      мусор, который нельзя ни восстановить, ни посчитать.
//
// ---------------------------------------------------------------------------
// Правила, которые модуль соблюдает по построению
// ---------------------------------------------------------------------------
//
// 1. Ничего не бросает наружу, кроме std::bad_alloc. Отказ — это `TrashStatus`
//    плюс код Win32 и путь в структуре результата: FR-6 требует, чтобы отказ по
//    одному файлу не останавливал прогон, а §5 — чтобы ни один отказ не ронял
//    процесс. Исключение из «положить файл в корзину» погасило бы всю очистку
//    из-за одного занятого файла. (core::TrashError ловится в open(): он приходит
//    из идентификатора, прочитанного с диска.)
//
// 2. Решение принимает ядро, а не сервис. Все проверки лимитов, крупного кэша и
//    кросс-томового переноса — core::planTrashPlacement/planEviction; сервис их
//    только исполняет и возвращает план вызывающему. Иначе «2 ГБ» и «100 МБ»
//    оказались бы в двух местах и разошлись бы при первой же правке.
//
// 3. Часы приходят параметром (`now`), а не читаются в модуле: иначе лимиты
//    нельзя проверить тестом с фиксированным «сейчас», а возрастной лимит — это
//    ровно вычисление от «сейчас».
//
// 4. Отмена проверяется раз в `cancelCheckEvery` элементов (SPEC §6.4). Проверка
//    на каждом элементе стоила бы лишнего атомарного чтения на каждом файле
//    миллионного каталога; на каждом 256-м — секунды неотзывчивой кнопки
//    «Отмена» на больших переносах.
//
// 5. Отмена не превращается в безвозвратное удаление. Сорвавшееся копирование
//    платформа откатывает сама (исходник на месте, копии в корзине нет), а всё
//    успешно перенесённое до отмены закрывается манифестом — иначе «отмена» на
//    середине прогона снесла бы файлы, которые пользователь уже не способен
//    вернуть. Каталог снимается (`abandon`) только когда в транзакции не лежит
//    ни байта.
//
// 6. Повышения прав нет (§5: повышение — один раз при старте). Отказ доступа —
//    это TrashStatus::AccessDenied с кодом Win32 в отчёте, а не попытка обойти.
//
// 7. Прямое удаление крупного кэша сервис НЕ делает. FR-7 требует «прямое
//    удаление с записью в журнал», но удаляет его исполнитель плана
//    (engine::executor, vfs::deleteEntry), а записывает в журнал —
//    engine::journal. Здесь это решение доходит только как
//    `Placement::plan.placement == DirectDelete`: сервис говорит «не в корзину»,
//    а байты убирает тот, кому они принадлежат по плану.
//
// 8. Конфликты при восстановлении — не решение этого модуля. Сервис исполняет
//    готовый `core::RestorePlan` (и Conflict при этом ничего не трогает), а
//    «перезаписывать или спросить» решает пользователь через
//    engine::undo_service (FR-7).
//
// 9. Всё, что сервис удаляет сам, он же и забывает в журнале: забытая
//    транзакция с удалённым каталогом — это в отчёте «место занято», которого
//    нет. Забывание происходит только после успешного сноса каталога, иначе
//    содержимое, которое ещё можно вернуть, перестаёт быть видимым.
//
// 10. Никаких фоновых потоков и тихих мест. Сервис не создаёт потоков, не
//     планирует очистку по таймеру и не трогает корзину с испорченным
//     манифестом без явного решения вызывающего: битые каталоги считаются и
//     показываются, удаляются только через purgeBroken() или empty().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/trash.hpp"
#include "platform/vfs_trash.hpp"

namespace mrproper::engine::trash_service {

// ---------------------------------------------------------------------------
// Конфигурация
// ---------------------------------------------------------------------------

// Настройки сервиса. Всё, что положено выносить в настройки приложения (FR-9),
// вынесено; значения по умолчанию — ровно те, что требует FR-7 (2 ГБ, 7 дней,
// крупный кэш 100 МБ) и живут в core::kDefaultTrashMaxBytes и соседях.
struct Options {
    // Корень корзины. Пусто — platform::defaultTrashRoot(), то есть
    // %ProgramData%\MrProper\Trash (FR-7). Корзина общая для всех запусков,
    // включая запуск с повышенными правами: иначе файл, удалённый
    // администратором, нельзя было бы восстановить обычным запуском.
    std::string trashRoot;
    core::TrashLimits limits{};
    // Версия приложения в манифесте: по ней видно, чем была собрана транзакция.
    std::string appVersion;
    // Частота проверки отмены (SPEC §6.4: «каждые 256 элементов»). 0 — взять
    // платформенный kTrashCancelCheckEvery.
    std::size_t cancelCheckEvery{};
    // Писать ли собственные строки журнала. Платформенные дубли выключаются
    // всегда: движок пишет одну строку на объект (путь, статус, код Win32), а
    // две строки на один отказ читаются как два разных отказа.
    bool logFailures{true};
};

// ---------------------------------------------------------------------------
// Открытие и подъём состояния
// ---------------------------------------------------------------------------

// Что получилось при открытии корзины. Отказ здесь — не «сервис мёртв»:
// корзина нужна для отмены, а не для удаления, поэтому вызывающий продолжает
// работу с прямым удалением и показывает в отчёте, что отмены не будет.
struct OpenReport {
    platform::TrashStatus status{platform::TrashStatus::Ok};
    std::uint32_t win32Error{};
    std::string path;      // на каком пути отказало (корень или каталог)
    std::string root;      // корень корзины, который в итоге используется
    std::uint32_t scannedDirs{};
    std::uint32_t adopted{};    // транзакций прочитано и поднято в журнал
    std::uint32_t staleDirs{};  // каталоги с манифестом, но не в состоянии
                                // Committed: недописанные (сбой при фиксации)
    std::uint32_t brokenDirs{};  // каталоги без читаемого манифеста

    // Корень доступен — можно работать. Битые каталоги этому не мешают: они
    // учтены отдельно и ждут явного решения.
    [[nodiscard]] bool ok() const noexcept { return status == platform::TrashStatus::Ok; }
};

// ---------------------------------------------------------------------------
// Перенос элементов и закрытие транзакции
// ---------------------------------------------------------------------------

// Что получилось с одним объектом. Отказ несёт путь и код Win32 (SPEC §12):
// по ним отказ читается без повторного запуска, а исполнитель продолжает
// прогон (FR-6).
struct StageOutcome {
    platform::TrashStatus status{platform::TrashStatus::Ok};
    std::uint32_t win32Error{};
    std::string path;         // исходный путь
    std::string failedPath;   // на каком пути остановились (при копировании)
    std::string payload;      // имя объекта внутри транзакции
    std::string destination;  // куда фактически легло
    std::uint64_t bytes{};    // фактически перенесено, а не исходный размер
    std::uint32_t files{};
    bool crossVolume{};       // перенос оказался копированием (FR-7)
    bool inLedger{};          // элемент учтён в транзакции и уйдёт в манифест
    // Решение FR-7: элемент в корзину не кладём, вызывающий удаляет его сам.
    // Отказа при этом не было — поэтому status остаётся Ok, а inLedger пуст.
    bool directDelete{};

    [[nodiscard]] bool ok() const noexcept { return status == platform::TrashStatus::Ok; }
    [[nodiscard]] bool placed() const noexcept { return inLedger; }
    [[nodiscard]] bool cancelled() const noexcept { return status == platform::TrashStatus::Cancelled; }
    // Исходник цел — значит, откат отработал и пользователь ничего не потерял.
    [[nodiscard]] bool sourceIntact() const noexcept { return !placed() && !crossVolume; }
};

// Прогон по списку: цикл с проверкой отмены раз в cancelCheckEvery элементов.
struct StageSummary {
    std::string txId;
    std::vector<StageOutcome> items;  // по одному на исходный путь, в том же порядке
    std::uint64_t bytes{};
    std::uint32_t files{};
    std::uint32_t staged{};       // ушло в корзину
    std::uint32_t directDelete{}; // решено удалить напрямую (FR-7)
    std::uint32_t failed{};       // отказ переноса
    std::uint32_t skipped{};      // осталось необработанным из-за отмены
    bool cancelled{};

    [[nodiscard]] bool ok() const noexcept { return failed == 0 && !cancelled; }
    [[nodiscard]] bool undoable() const noexcept { return staged != 0; }
    [[nodiscard]] std::uint32_t processed() const noexcept { return staged + directDelete + failed; }
};

// Итог закрытия транзакции. Отменяемой транзация становится только здесь: до
// записи манифеста на диске отменять нечего (core::TrashTxState).
struct CommitResult {
    platform::TrashStatus status{platform::TrashStatus::Ok};
    std::uint32_t win32Error{};
    std::string txId;
    std::string manifestPath;     // куда писался манифест
    std::uint64_t bytes{};
    std::uint32_t items{};
    std::uint32_t failedItems{};  // не перенеслось на этапе переноса
    bool manifestWritten{};
    bool committed{};  // состояние Committed — отменять можно
    bool cancelled{};

    [[nodiscard]] bool ok() const noexcept { return manifestWritten && committed; }
    // Транзакция без элементов закрывается в Committed честно (пустая), но
    // отменять в ней нечего, и отчёт должен это сказать.
    [[nodiscard]] bool undoable() const noexcept { return committed && items != 0; }
};

// ---------------------------------------------------------------------------
// Очистка: вытеснение по лимитам, протухшее, битое, «очистить корзину»
// ---------------------------------------------------------------------------

// Что физически удалили. removedBytes — фактически снятое, а не оценка по
// манифесту: кросс-томовое копирование могло оборваться, и «уменьшение
// записанного» ничего не значит.
struct PurgeReport {
    std::vector<std::string> purged;          // каталоги сняты
    std::vector<std::string> failed;          // где не вышло
    std::vector<std::string> collapsedTxIds;  // отменять их больше нельзя (§7.2)
    std::uint64_t removedBytes{};
    std::uint32_t removedDirs{};
    std::uint32_t failedCount{};
    bool cancelled{};  // вытеснение остановлено пользователем; остальное — нет

    [[nodiscard]] bool empty() const noexcept { return purged.empty() && failed.empty(); }
    [[nodiscard]] bool ok() const noexcept { return failed.empty(); }
};

// ---------------------------------------------------------------------------
// Решение о размещении
// ---------------------------------------------------------------------------

// Ответ на «куда это деть» с фактами, на которых ответ основан. Ядро решает
// (core::planTrashPlacement), сервис добавляет то, что ядру не известно: удалось
// ли вообще определить том источника.
struct Placement {
    core::TrashPlacementPlan plan{};
    // Сравнение томов удалось. known=false — источник или корзина на сетевом
    // пути или точке монтирования: тогда решение о кросс-томовом переносе
    // принимает сам MoveFileEx (FR-7), и plan.crossVolume не является
    // утверждением о факте.
    bool volumeKnown{true};
    std::string sourceVolume;  // "\\?\Volume{…}\" — как уходит в манифест
    std::string trashVolume;
    bool sameVolume{true};
    // Размер на момент решения был известен: из плана очистки либо из замера
    // платформой. false — решение принималось при itemBytes == 0, то есть размер
    // был неизвестен и лимит считался по нижней оценке.
    bool measured{};

    [[nodiscard]] bool intoTrash() const noexcept { return plan.placement == core::TrashPlacement::Trash; }
    [[nodiscard]] bool undoable() const noexcept { return plan.undoable(); }
    [[nodiscard]] const char* reason() const noexcept { return plan.reason; }
    // FR-7: «честно показываем, что это займёт время».
    [[nodiscard]] bool slow() const noexcept { return plan.expensive; }
};

// Что просят положить в корзину.
struct AdmitRequest {
    std::string sourcePath;  // utf-8, откуда забираем
    // Размер в байтах. 0 — «неизвестно»: сервис измерит объект сам
    // (platform::readPathFacts) и пересчитает решение. Обычно размер известен из
    // скана, и тогда обход не повторяется.
    std::uint64_t itemBytes{};
    std::int64_t now{};  // unix-секунды для возрастного лимита
    // Идентификатор уже открытой транзакции. Пусто — сервис откроет новую
    // (один запуск очистки = одна транзакция, отменяемая целиком).
    std::string openTxId;
};

// Итог одного размещения.
struct AdmitResult {
    Placement placement{};
    std::string txId;        // пусто, если разместить не удалось
    StageOutcome stage{};    // заполнено всегда: и успех, и решение, и отказ
    PurgeReport eviction{};  // что вытеснили ради места (пусто, если не нужно)
    // Заполнено, когда сервис был обязан закрыть транзакцию сам, а не мог
    // оставить её открытой: отмена при уже перенесённых байтах (см. правило 5).
    CommitResult commit{};
    bool placed{};  // элемент лежит в корзине и учтён в манифесте

    // Прямое удаление — не отказ: это решение FR-7 для крупного кэша. Вызывающий
    // удаляет объект сам (engine::executor) и пишет запись в журнал.
    [[nodiscard]] bool needsDirectDelete() const noexcept { return !placed && stage.directDelete; }
    // Отказ самого сервиса: объект не измерился, каталог транзакции не создался
    // или перенос сорвался. Отличать от решения нужно потому, что «прямое
    // удаление» повторит исполнитель, а отказ он только запишет в отчёт (FR-6).
    [[nodiscard]] bool failed() const noexcept { return !placed && !stage.ok(); }
    [[nodiscard]] bool ok() const noexcept { return placed; }
};

// ---------------------------------------------------------------------------
// Восстановление: исполнение готового плана
// ---------------------------------------------------------------------------

// Итог восстановления по плану core::RestorePlan. Политика (что выбрано, чем
// кончилось «занято на месте», спрашивать ли пользователя) — задача
// undo_service; сервис исполняет и поддерживает целостность манифеста.
struct RestoreOutcome {
    platform::TrashRestoreSummary summary{};
    std::string txId;
    std::uint32_t restoredItems{};
    std::uint32_t remainingItems{};  // сколько элементов осталось в транзакции
    // Транзакция возвращена целиком: помечена Undone, каталог снят, из журнала
    // забыта. Частичное восстановление оставляет транзакцию в корзине, но
    // манифест перезаписывается на оставшиеся элементы.
    bool closed{};
    bool manifestRewritten{};  // манифест приведён в соответствие с фактом

    [[nodiscard]] bool allRestored() const noexcept { return closed; }
    [[nodiscard]] bool ok() const noexcept { return summary.allRestored(); }
};

// ---------------------------------------------------------------------------
// Снимок состояния для интерфейса и отчёта (FR-8)
// ---------------------------------------------------------------------------

struct Snapshot {
    core::TrashUsage usage{};  // по манифестам: что сервис считает своим
    std::uint64_t onDiskBytes{};  // обходом каталогов: что лежит на самом деле
    std::uint32_t transactionCount{};
    std::uint32_t itemCount{};
    std::uint32_t brokenDirs{};  // каталоги без читаемого манифеста
    std::uint64_t purgedBytes{};  // схлопнуто безвозвратно по журналу
    std::uint64_t freeBytes{};    // свободно на томе корзины
    std::string summary;          // ru, для UI: «1,2 ГиБ из 2 ГиБ, транзакций: 3»
    std::string describe;         // машинная строка для журнала и отчёта

    // Расхождение учёта и диска. Ненулевое — признак того, что на диске есть
    // содержимое без манифеста (сорванное копирование) либо вытекшая
    // транзакция. Это видно в отчёте, а не прячется.
    [[nodiscard]] std::uint64_t driftBytes() const noexcept {
        return onDiskBytes > usage.totalBytes ? onDiskBytes - usage.totalBytes : 0;
    }
};

// ---------------------------------------------------------------------------
// Сервис
// ---------------------------------------------------------------------------

// Владеет одним журналом core::TrashLedger и одним корнем корзины на диске.
// Потокобезопасности здесь нет и не требуется: операции идут последовательно из
// движка очистки (пул исполнителя — отдельный слой), а внутри циклов отмена
// проверяется без блокировок, потому что токен — единственное разделяемое
// состояние.
class TrashService {
public:
    TrashService() = default;
    explicit TrashService(Options options) : options_(std::move(options)) {}

    // --- Конфигурация ---
    const Options& options() const noexcept { return options_; }
    // Смена лимитов на ходу (FR-9, настройки): действует со следующего решения о
    // размещении, уже вытесненное не возвращается.
    void setLimits(core::TrashLimits limits);
    const core::TrashLimits& limits() const noexcept { return options_.limits; }
    const std::string& root() const noexcept { return options_.trashRoot; }
    void setRoot(std::string trashRoot);
    const std::string& appVersion() const noexcept { return options_.appVersion; }
    void setAppVersion(std::string version) { options_.appVersion = std::move(version); }

    // --- Журнал ---
    core::TrashLedger& ledger() noexcept { return ledger_; }
    const core::TrashLedger& ledger() const noexcept { return ledger_; }
    core::TrashUsage usage() const { return ledger_.usage(); }
    const core::TrashTransaction* find(std::string_view txId) const { return ledger_.find(txId); }
    // Пути внутри корня. Идентификатор проверяется, а exception не бросается: он
    // может прийти из манифеста на диске, то есть извне (правило 1).
    [[nodiscard]] std::string dirFor(std::string_view txId) const;
    [[nodiscard]] std::string manifestFor(std::string_view txId) const;
    // Имя элемента внутри транзакции: «p0», «p1», … По номеру, а не по исходному
    // имени файла: в каталоге транзакции не может быть ни разделителя, ни
    // двоеточия (потоки данных), ни «..».
    [[nodiscard]] static std::string payloadName(std::size_t index);

    // --- Открытие ---
    // Создать корень при необходимости, прочитать манифесты, поднять транзакции
    // в журнал. Битый каталог считается, а не удаляется (правило 10); транзакция
    // в состоянии Undone с остатками каталога — тоже считается, но в учёт не
    // берётся: её содержимое пользователю уже вернули. Повторный вызов безопасен.
    OpenReport open(const platform::TrashOptions& trashOptions = {});
    // Перечитать корень. Нужен после чужого прогона (вторая копия приложения) и
    // перед отчётом. Открытая в этом процессе транзакция не страдает: adopt
    // заменяет записи по идентификатору, а незаписанной транзакции на диске нет.
    OpenReport refresh(const platform::TrashOptions& trashOptions = {});
    // Снести каталоги без читаемого манифеста. Отдельным вызовом, а не внутри
    // open(): содержимое каталога, чей смысл неизвестен, нельзя удалить молча
    // (FR-6).
    PurgeReport purgeBroken(const platform::TrashOptions& trashOptions = {});

    // --- Размещение ---
    // Решение без побочных эффектов: что лежит, чем кончится, кого придётся
    // вытеснить. Нужен интерфейсу до старта очистки («это займёт время», «в
    // корзину не влезет»).
    Placement planPlacement(std::string_view sourcePath, std::uint64_t itemBytes, std::int64_t now) const;
    // Разместить: план → вытеснение по лимитам → перенос в уже открытую или новую
    // транзакцию. Возвращает и решение (для журнала и отчёта), и факт (что
    // легло).
    AdmitResult admit(const AdmitRequest& request, const platform::TrashOptions& trashOptions = {});
    // Прогон целиком: один запуск очистки = одна транзакция, один цикл с
    // проверкой отмены, одно закрытие. Решение принимается по каждому элементу
    // (у крупного кэша оно своё), а вытеснение случается там, где оно нужно, —
    // иначе корзина переполняется в середине прогона.
    struct AdmitAllResult {
        std::string txId;
        StageSummary staged{};
        CommitResult commit{};
        PurgeReport eviction{};  // всё, что вытеснили за прогон
        bool intoTrash{};         // что-то лежит в корзине и отменяемо

        [[nodiscard]] bool ok() const noexcept { return commit.ok(); }
    };
    AdmitAllResult admitAll(const std::vector<AdmitRequest>& requests, const platform::TrashOptions& trashOptions = {});

    // --- Транзакция ---
    // Открыть транзакцию: запись в журнале плюс каталог на диске. Пустой
    // результат — каталог не создался (нет прав, нет места), и запись в журнале
    // откатывается: транзакция без каталога — мусор в учёте.
    std::string beginTransaction(std::int64_t now, const platform::TrashOptions& trashOptions = {});
    // Положить один объект в открытую транзакцию.
    StageOutcome stage(std::string_view txId, std::string_view sourcePath,
                       const platform::TrashOptions& trashOptions = {});
    // Цикл по объектам в уже открытой транзакции (отмена раз в N элементов).
    StageSummary stageAll(std::string_view txId, const std::vector<std::string>& sourcePaths,
                          const platform::TrashOptions& trashOptions = {});
    // Записать манифест и перевести транзакцию в Committed. Только после этого
    // она отменяема (§7.2). Повторный вызов для уже закрытой транзакции
    // безопасен и ничего не перезаписывает.
    CommitResult commit(std::string_view txId, const platform::TrashOptions& trashOptions = {});
    // Снести недоделанную транзакцию: манифеста нет → отменять нечего → каталог
    // снимается, запись забывается. Безопасная альтернатива commit() при отказе
    // или отмене на середине.
    bool abandon(std::string_view txId, const platform::TrashOptions& trashOptions = {});

    // --- Очистка и лимиты (FR-7) ---
    // Привести корзину к лимитам: протухшее уходит всегда, дальше — от самой
    // старой транзакции, пока хватает места под incomingBytes. Ничего не
    // размещает и никуда не переносит.
    PurgeReport enforceLimits(std::uint64_t incomingBytes, std::int64_t now,
                              const platform::TrashOptions& trashOptions = {});
    // Только возрастной лимит (7 дней), независимо от свободного места.
    PurgeReport purgeExpired(std::int64_t now, const platform::TrashOptions& trashOptions = {});
    // Удалить одну транзакцию целиком («удалить из корзины»).
    PurgeReport purgeTransaction(std::string_view txId, const platform::TrashOptions& trashOptions = {});
    // Очистить корзину целиком («Очистить корзину»): сначала то, что в журнале,
    // затем каталоги без манифеста — иначе «очистить» оставило бы на диске
    // именно то, что нельзя показать пользователю.
    PurgeReport empty(const platform::TrashOptions& trashOptions = {});

    // --- Восстановление (FR-7) ---
    // План восстановления по журналу сервиса. selection — индексы элементов,
    // пустой список означает «всё». Проба занятости — platform::existsProbe(),
    // поэтому «на месте уже есть» проверяется по-настоящему, а не по списку.
    core::RestorePlan planRestore(std::string_view txId, const std::vector<std::size_t>& selection,
                                  bool overwriteConflicts) const;
    core::RestorePlan planRestoreAll(std::string_view txId, bool overwriteConflicts) const;
    // Исполнить план и привести манифест в соответствие с фактом: полное
    // восстановление закрывает транзакцию, частичное — перезаписывает манифест на
    // оставшиеся элементы. План с Conflict ничего не трогает (FR-7: не
    // перезаписывать, спросить).
    RestoreOutcome restore(std::string_view txId, const core::RestorePlan& plan,
                           const platform::TrashOptions& trashOptions = {});
    // То же для всех элементов транзакции.
    RestoreOutcome restoreAll(std::string_view txId, bool overwriteConflicts,
                              const platform::TrashOptions& trashOptions = {});

    // --- Отчёт ---
    // Снимок целиком. ВНИМАНИЕ: считает корзину обходом каталогов, поэтому
    // вызывается перед отчётом или по кнопке пользователя, а не в цикле.
    Snapshot snapshot(const platform::TrashOptions& trashOptions = {}) const;
    // Кратко для журнала и интерфейса (ru), без обхода диска.
    std::string summary() const;

private:
    // Опции вызывающего плюс настройки сервиса: журналирование ведёт сервис
    // (одна строка на отказ), а отмена и прогресс остаются за вызывающим.
    [[nodiscard]] platform::TrashOptions effective(const platform::TrashOptions& trashOptions) const;
    // Шаг проверки отмены: из настроек сервиса, иначе платформенный.
    [[nodiscard]] std::size_t cancelStride() const noexcept;
    // Корень: заданный или платформенный по умолчанию.
    [[nodiscard]] std::string resolvedRoot() const;
    // Перенос одного объекта с уже готовыми опциями — чтобы в цикле не
    // пересобирать TrashOptions на каждом файле.
    StageOutcome stagePrepared(std::string_view txId, std::string_view sourcePath, const platform::TrashOptions& opts);
    // Тихий снос транзакции, которой сервис владел (admit с пустым openTxId) и в
    // которой не лежит ни байта: каталог и запись в журнале, без warn в лог.
    // Отличается от публичного abandon() только журналированием: там снос —
    // происшествие, здесь обычный исход «первый элемент не лёг».
    [[nodiscard]] bool dropEmptyOwned(std::string_view txId, const platform::TrashOptions& opts);
    // Снять каталог транзакции и привести журнал в соответствие с фактом:
    // содержимого больше нет. keepCollapsed=true — для вытеснения по лимиту и
    // для явного удаления пользователем: открытая транзакция схлопывается и
    // остаётся в журнале, потому что текущий прогон должен увидеть «отменять
    // нечего» (§7.2), а не исчезновение записи. false — только abandon():
    // недоделанная транзакция забывается целиком.
    bool purgeDirectory(std::string_view txId, PurgeReport& report, const platform::TrashOptions& opts,
                        bool keepCollapsed);
    // Исполнить план вытеснения (core) на диске и в журнале.
    PurgeReport executeEviction(const core::TrashEvictionPlan& plan, const platform::TrashOptions& opts);
    // Перезаписать манифест транзакции текущим содержимым журнала.
    [[nodiscard]] bool rewriteManifest(const core::TrashTransaction& tx, const platform::TrashOptions& opts);
    // Счётчик отказов по транзакции: попадает в CommitResult, чтобы отчёт
    // говорил «1200 из 1201», а не «1200».
    [[nodiscard]] std::uint32_t failuresOf(std::string_view txId) const;
    void noteFailure(std::string_view txId);

    Options options_{};
    core::TrashLedger ledger_{};
    std::vector<std::string> brokenDirs_;
    // txId → сколько объектов этой транзакции не удалось перенести.
    std::unordered_map<std::string, std::uint32_t> failures_;
    bool opened_{};
};

// ---------------------------------------------------------------------------
// Тексты для журнала и отчёта
// ---------------------------------------------------------------------------

// «purged=3 collapsed=1 removedBytes=412 МБ failed=0» — одна строка на прогон.
[[nodiscard]] std::string describe(const PurgeReport& report);
// «root=… adopted=3 stale=0 broken=1 ok=true».
[[nodiscard]] std::string describe(const OpenReport& report);
// «tx=… staged=1200 direct=3 failed=1 bytes=412 МБ».
[[nodiscard]] std::string describe(const StageSummary& summary);
// «tx=… items=1200 failed=1 bytes=412 МБ undoable=true».
[[nodiscard]] std::string describe(const CommitResult& result);
// «transactions=3 items=1200 onDisk=1,2 ГиБ drift=0 Б» — строка отчёта.
[[nodiscard]] std::string describe(const Snapshot& snapshot);
// Причина размещения по-русски, готовая строкой в журнал.
[[nodiscard]] std::string reasonText(const Placement& placement);

}  // namespace mrproper::engine::trash_service
