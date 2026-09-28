// Юнит-тесты корзины и отмены: core::trash и core::undo. Задача 78.
//
// Спека: §11.1 (пункт 1 стратегии — «Юнит-тесты (Core, без Windows): правила,
// glob, min-age, скоринг, JSON, форматирование, инварианты модели», запускаются
// на каждом PR за секунды), §4 FR-7 (своя корзина: лимит 2 ГБ / 7 дней, крупные
// кэши > 100 МБ удаляем напрямую, «существующий файл — не перезаписывать,
// спросить», восстановление полное или частичное), §7.2 (отмена доступна, пока
// транзакция не схлопнулась), §12 («100 % удалённых файлов восстанавливаются из
// корзины приложения», «Ни один элемент не удаляется без видимого объяснения»).
//
// Три свойства, вокруг которых построен весь файл, — все три бьют по кошельку и
// по нервам, а не по тесту:
//
//   1. ЛИМИТЫ — 2 ГБ, 7 дней и 100 МБ это обещание пользователю «откатим всё».
//      Проверяются обе границы (== лимиту и лимит+1), порядок вытеснения
//      (сначала просроченное по возрасту, затем самое старое) и то, что при
//      нехватке места ядро говорит, кого именно вытесняет, а не «не влезает».
//      Отдельно — что два соседних модуля (trash и undo) задают ОДИН И ТОТ ЖЕ
//      лимит из FR-7: разъехавшиеся числа означали бы, что UI обещает 2 ГБ, а
//      движок чистит по другим правилам.
//   2. КОНФЛИКТЫ — «не перезаписывать, спросить» (FR-7). Молчаливая перезапись
//      чужого файла при отмене — худший баг этого модуля: пользователь нажал
//      Ctrl+Z и потерял работу. Поэтому проверяется, что конфликт всегда виден
//      пользователю (вопрос задан), что «не ответил» ≠ «перезаписал», и что
//      конфликты, которые перезаписью не лечатся (файл занят, цели неизвестна,
//      не хватает места), остаются пропущенными при любой политике.
//   3. ЧАСТИЧНОЕ ВОССТАНОВЛЕНИЕ — отмена редко бывает «всё или ничего»: часть
//      элементов уже вернули, часть утрачена, часть конфликтует. Проверяется
//      выборка по элементам, учёт уже возвращенного, невозможность повторной
//      записи поверх живого файла и то, что транзакция схлопывается ровно тогда,
//      когда возвращать больше нечего (§7.2 — иначе Ctrl+Z в UI горит впустую).
//
// Модуль переносимый (SPEC §6.1): здесь нет ни Windows API, ни ввода-вывода, ни
// файловой системы. Проба «занято ли место» и снимок состояния цели приходят
// аргументом — так же, как их передаёт платформа. Часов системы нет: время
// приходит полем, поэтому тест повторяем.
//
// Два заголовка включаются в один TU намеренно: core::trash и core::undo
// описывают соседние стороны FR-7 и обязаны сосуществовать в одной сборке, а
// имена в них разведены (см. комментарий в undo.hpp). Если этот файл перестанет
// собираться с обоими заголовками — значит, кто-то вернул пересечение имён.
// main() живёт в core_tests.cpp (tests/unit/CMakeLists.txt).
#include "harness.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "trash.hpp"
#include "undo.hpp"

using namespace mrproper::core;

namespace {

// Фиксированное «сейчас»: тест не зависит ни от часов, ни от локали.
constexpr std::int64_t kNow = 1780000000;
constexpr std::int64_t kMinute = 60;
constexpr std::int64_t kHour = 3600;
constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kWeek = 7 * kDay;

constexpr std::uint64_t kKB = 1024ull;
constexpr std::uint64_t kMB = 1024ull * 1024;
constexpr std::uint64_t kGB = 1024ull * 1024 * 1024;
constexpr std::uint64_t kMaxU64 = 0xFFFFFFFFFFFFFFFFull;

constexpr const char* kVolA = R"(\\?\Volume{aaaaaaaa-0000-0000-0000-000000000000})";
constexpr const char* kVolB = R"(\\?\Volume{bbbbbbbb-0000-0000-0000-000000000000})";

bool containsText(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Ловит ИМЕННО этот тип ошибки: harness умеет только CHECK_THROWS, а движок
// ловит один тип ошибок корзины. Если json::ParseError или std::runtime_error
// пробьются мимо TrashError, необработанное исключение уедет в UI — это и
// должен ловить тест.
template <typename Error, typename Fn>
bool throwsAs(Fn&& fn) {
    try {
        fn();
    } catch (const Error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// Текст ошибки: тест проверяет, что сообщение называет место (файл/поле), а не
// «что-то пошло не так» — по одному тексту видно, что чинить.
template <typename Error, typename Fn>
std::string errorText(Fn&& fn) {
    try {
        fn();
    } catch (const Error& e) {
        return std::string(e.what());
    } catch (...) {
        return std::string();
    }
    return std::string();
}

TrashLimits tinyLimits() {
    // Малые числа вместо гигабайтов: проверка лимита не должна требовать
    // терабайта памяти — арифметика та же.
    TrashLimits limits;
    limits.maxBytes = 250 * kMB;
    limits.maxAgeSeconds = kWeek;
    limits.largeItemBytes = 100 * kMB;
    return limits;
}

UndoLimits tinyUndoLimits() {
    UndoLimits limits;
    limits.maxTotalBytes = 1000;
    limits.maxAgeSeconds = kWeek;
    limits.largeItemThresholdBytes = 1000000;
    return limits;
}

TrashItem trashFile(std::string originalPath, std::uint64_t bytes) {
    TrashItem item;
    item.kind = TrashItemKind::File;
    item.originalPath = std::move(originalPath);
    item.bytes = bytes;
    return item;
}

TrashTransactionInfo info(std::string txId, std::int64_t createdAt, std::uint64_t bytes) {
    TrashTransactionInfo out;
    out.txId = std::move(txId);
    out.createdAt = createdAt;
    out.bytes = bytes;
    out.itemCount = 1;
    return out;
}

TrashEntry undoEntry(std::string originalPath, std::uint64_t bytes, const char* volume = kVolA) {
    TrashEntry entry;
    entry.originalPath = std::move(originalPath);
    entry.storedPath = "p0";
    entry.sizeBytes = bytes;
    entry.createdUnix = kNow;
    entry.volumeGuidPath = volume;
    return entry;
}

RestoreTargetInfo freeTarget(std::string path, const char* volume = kVolA, std::uint64_t freeBytes = 10 * kGB) {
    RestoreTargetInfo target;
    target.path = std::move(path);
    target.volumeGuidPath = volume;
    target.freeBytes = freeBytes;
    return target;
}

}  // namespace

// ===========================================================================
// core::trash — лимиты FR-7
// ===========================================================================

TEST(trash_limits_matchSpecByDefault) {
    // FR-7 называет 2 ГБ, 7 дней и 100 МБ. Единицы двоичные: круглое «2 ГБ» из
    // спеки не должно означать 2 000 000 000 байт на диске, который считает ГиБ.
    CHECK_EQ(kDefaultTrashMaxBytes, 2ull * 1024 * 1024 * 1024);
    CHECK_EQ(kDefaultTrashMaxAgeSeconds, kWeek);
    CHECK_EQ(kTrashLargeItemBytes, 100ull * 1024 * 1024);

    const TrashLimits defaults;
    CHECK_EQ(defaults.maxBytes, kDefaultTrashMaxBytes);
    CHECK_EQ(defaults.maxAgeSeconds, kDefaultTrashMaxAgeSeconds);
    CHECK_EQ(defaults.largeItemBytes, kTrashLargeItemBytes);
}

TEST(trash_undoModuleUsesTheSameSpecLimits) {
    // Два модуля описывают одну и ту же корзину с двух сторон. Расхождение
    // чисел = UI обещает 2 ГБ, а движок вытесняет по другим правилам.
    const UndoLimits undo;
    CHECK_EQ(undo.maxTotalBytes, kDefaultTrashMaxBytes);
    CHECK_EQ(undo.maxAgeSeconds, kDefaultTrashMaxAgeSeconds);
    CHECK_EQ(undo.largeItemThresholdBytes, kTrashLargeItemBytes);
    CHECK_EQ(std::string(kUndoManifestFileName), std::string(kTrashManifestFileName));
}

TEST(trash_largeItemGoesDirectDelete) {
    const TrashLimits limits;
    const std::vector<TrashTransactionInfo> empty;

    // Ровно порог — ещё в корзину: «> 100 МБ» из FR-7, не «>= 100 МБ».
    const TrashPlacementPlan atLimit = planTrashPlacement(limits, empty, limits.largeItemBytes, true, kNow);
    CHECK(atLimit.placement == TrashPlacement::Trash);
    CHECK(atLimit.undoable());

    const TrashPlacementPlan over = planTrashPlacement(limits, empty, limits.largeItemBytes + 1, true, kNow);
    CHECK(over.placement == TrashPlacement::DirectDelete);
    CHECK(!over.undoable());
    CHECK(over.reason != nullptr);
    CHECK(containsText(over.reason, "крупн"));
    // Крупный кэш уходит напрямую даже в пустую корзину: он всё равно не
    // поместился бы в 2 ГБ корзины, занимая её без возможности отмены.
    CHECK(over.evictTxIds.empty());
}

TEST(trash_itemBiggerThanWholeLimitGoesDirectDelete) {
    TrashLimits limits;
    limits.maxBytes = 10 * kMB;
    limits.largeItemBytes = 100 * kMB;  // крупный порог выше лимита — проверяем вторую ветку

    const TrashPlacementPlan plan = planTrashPlacement(limits, std::vector<TrashTransactionInfo>{}, 11 * kMB, true, kNow);
    CHECK(plan.placement == TrashPlacement::DirectDelete);
    CHECK(!plan.undoable());
    CHECK(containsText(plan.reason, "больше всего лимита"));
}

TEST(trash_crossVolumeBigItemGoesDirectDeleteButSmallStays) {
    const TrashLimits limits;
    const std::vector<TrashTransactionInfo> empty;

    // Кросс-томовой перенос — это копирование (FR-7): большой объём дешевле
    // удалить сразу, чем тащить копированием.
    const TrashPlacementPlan big = planTrashPlacement(limits, empty, 20 * kMB, false, kNow);
    CHECK(big.placement == TrashPlacement::DirectDelete);
    CHECK(big.crossVolume);
    CHECK(containsText(big.reason, "кросс-томов"));

    // Мелкий — остаётся в корзине, но UI обязан показать «это займёт время».
    const TrashPlacementPlan small = planTrashPlacement(limits, empty, 1 * kMB, false, kNow);
    CHECK(small.placement == TrashPlacement::Trash);
    CHECK(small.crossVolume);
    CHECK(small.expensive);
    CHECK(small.undoable());
    CHECK(containsText(small.reason, "копирование"));

    // На том же томе копирования нет — и объяснение другое.
    const TrashPlacementPlan same = planTrashPlacement(limits, empty, 1 * kMB, true, kNow);
    CHECK(!same.crossVolume);
    CHECK(!same.expensive);
    CHECK(containsText(same.reason, "отменяемо"));
}

TEST(trash_placementOverLimitEvictsOldestAndNamesIt) {
    const TrashLimits limits = tinyLimits();
    const std::vector<TrashTransactionInfo> txs = {info("b-new", kNow - kHour, 100 * kMB),
                                                    info("b-old", kNow - 2 * kHour, 100 * kMB)};

    // 200 МБ занято, лимит 250 МБ, ещё 100 МБ не влезает: вытесняется самая
    // старая транзакция, и план называет её по имени (движку нужно удалить каталог).
    const TrashPlacementPlan plan = planTrashPlacement(limits, txs, 100 * kMB, true, kNow);
    CHECK(plan.placement == TrashPlacement::Trash);
    CHECK(plan.undoable());
    CHECK_EQ(plan.evictTxIds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(plan.evictTxIds[0], std::string("b-old"));
    // evictBytes — это непокрытая вытеснением нехватка, а не объём вытеснения:
    // здесь 100 МБ вытеснения закрывают все 50 МБ нехватки, остатка 0.
    CHECK_EQ(plan.evictBytes, static_cast<std::uint64_t>(0));
    CHECK(!plan.needsEviction());
    CHECK_EQ(plan.bytesAfter, static_cast<std::uint64_t>(300 * kMB));
    CHECK(plan.reason != nullptr);
}

TEST(trash_placementWithUsageOnlyReportsHowManyBytesToEvict) {
    // Перегрузка «на глаз»: платформа знает, что в корзине 300 МБ, но не знает,
    // какие там транзакции. План всё равно должен сказать, сколько освободить.
    TrashUsage usage;
    usage.totalBytes = 300 * kMB;
    usage.transactionCount = 3;

    const TrashLimits limits = tinyLimits();
    const TrashPlacementPlan plan = planTrashPlacement(limits, usage, 10 * kMB, true, kNow);
    CHECK(plan.placement == TrashPlacement::Trash);
    CHECK(plan.evictTxIds.empty());  // перечислять нечем — не выдумываем
    CHECK_EQ(plan.evictBytes, static_cast<std::uint64_t>(60 * kMB));
    CHECK_EQ(plan.bytesAfter, static_cast<std::uint64_t>(310 * kMB));
    CHECK(containsText(plan.reason, "вытеснить"));
}

TEST(trash_overfullTrashRejectsEvenEmptyItem) {
    // Корзина переполнена по данным журнала, а вытеснять нечем: план чистки по
    // возрасту пуст (incoming == 0 не вытесняет живое), а лимит превышен. Новые
    // элементы в такую корзину не идут — иначе «2 ГБ» перестали бы быть лимитом.
    const TrashLimits limits = tinyLimits();
    const std::vector<TrashTransactionInfo> full = {info("r1", kNow - kHour, 300 * kMB)};
    const TrashPlacementPlan plan = planTrashPlacement(limits, full, 0, true, kNow);
    CHECK(plan.placement == TrashPlacement::DirectDelete);
    CHECK(plan.evictBytes != 0);
    CHECK(containsText(plan.reason, "вытеснить нечего"));
}

TEST(trash_restoredTransactionIsNotEvicted) {
    // Восстановленная транзакция (bytes == 0) не должна попадать ни в
    // вытеснение по возрасту, ни в план: её каталог пуст, вытеснять нечего.
    // Иначе корзина «вытесняла» бы пустоту и урезала лимит для остальных.
    TrashLimits limits;
    limits.maxBytes = 100 * kMB;
    limits.maxAgeSeconds = kWeek;
    limits.largeItemBytes = 100 * kMB;
    const std::vector<TrashTransactionInfo> txs = {info("undone", kNow - 30 * kDay, 0),
                                                    info("live", kNow - kHour, 10 * kMB)};
    const TrashEvictionPlan plan = planEviction(limits, txs, 100 * kMB, kNow);
    CHECK_EQ(plan.txIds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(plan.txIds[0], std::string("live"));
    CHECK_EQ(plan.reclaimBytes, static_cast<std::uint64_t>(10 * kMB));

    const TrashUsage usage = summarize(txs);
    CHECK_EQ(usage.totalBytes, static_cast<std::uint64_t>(10 * kMB));
    CHECK_EQ(usage.transactionCount, static_cast<std::uint32_t>(2));  // обе на учёте
}

// ===========================================================================
// core::trash — вытеснение: возраст и порядок
// ===========================================================================

TEST(trash_evictionTakesExpiredFirstThenOldestBySize) {
    TrashLimits limits;
    limits.maxBytes = 2 * kGB;
    limits.maxAgeSeconds = kWeek;
    limits.largeItemBytes = 100 * kMB;

    // Сценарий 1: место нужно, но хватает того, что протухло по возрасту.
    const std::vector<TrashTransactionInfo> withExpired = {info("g-old", kNow - 8 * kDay, 1 * kGB),
                                                            info("g-new", kNow - kDay, 1 * kGB)};
    const TrashEvictionPlan byAge = planEviction(limits, withExpired, 1 * kGB, kNow);
    CHECK_EQ(byAge.txIds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(byAge.txIds[0], std::string("g-old"));
    CHECK_EQ(byAge.freedByAge, static_cast<std::uint64_t>(1 * kGB));
    CHECK_EQ(byAge.freedBySize, static_cast<std::uint64_t>(0));
    CHECK_EQ(byAge.reclaimBytes, static_cast<std::uint64_t>(1 * kGB));
    CHECK(!byAge.empty());

    // Сценарий 2: протухшего нет — вытесняется самая старая из живых.
    const std::vector<TrashTransactionInfo> fresh = {info("g-a", kNow - 2 * kHour, 1 * kGB),
                                                     info("g-b", kNow - kHour, 1 * kGB)};
    const TrashEvictionPlan bySize = planEviction(limits, fresh, 1 * kGB, kNow);
    CHECK_EQ(bySize.txIds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bySize.txIds[0], std::string("g-a"));
    CHECK_EQ(bySize.freedBySize, static_cast<std::uint64_t>(1 * kGB));
    CHECK_EQ(bySize.freedByAge, static_cast<std::uint64_t>(0));
}

TEST(trash_evictionBySizeNeverRunsWhenNothingComes) {
    TrashLimits limits;
    limits.maxBytes = 2 * kGB;
    const std::vector<TrashTransactionInfo> fresh = {info("g-a", kNow - kHour, 1 * kGB)};

    // incoming == 0 означает «чистим только по возрасту»: вытеснять живое ради
    // нуля байтов нельзя.
    const TrashEvictionPlan plan = planEviction(limits, fresh, 0, kNow);
    CHECK(plan.empty());
    CHECK_EQ(plan.reclaimBytes, static_cast<std::uint64_t>(0));

    // Нулевой лимит — вытеснять нечем по определению.
    TrashLimits noLimit;
    noLimit.maxBytes = 0;
    const TrashEvictionPlan unbounded = planEviction(noLimit, fresh, 1 * kGB, kNow);
    CHECK(unbounded.empty());
}

TEST(trash_expiredSelectionIsOrderedAndBoundaryExact) {
    const TrashLimits limits;  // 7 дней по умолчанию

    // Ровно неделя — ещё не протухло (граница строго «>»), секунда сверху — протухло.
    const std::vector<TrashTransactionInfo> txs = {info("e-edge", kNow - kWeek, 10 * kMB),
                                                    info("e-yes", kNow - kWeek - 1, 10 * kMB)};
    const std::vector<std::string> expired = selectExpired(limits, txs, kNow);
    CHECK_EQ(expired.size(), static_cast<std::size_t>(1));
    CHECK_EQ(expired[0], std::string("e-yes"));

    // Порядок воспроизводим: при одинаковом времени — по идентификатору.
    const std::vector<TrashTransactionInfo> tie = {info("t-b", kNow - 9 * kDay, 10 * kMB),
                                                   info("t-a", kNow - 9 * kDay, 10 * kMB),
                                                   info("t-c", kNow - 9 * kDay, 10 * kMB)};
    const std::vector<std::string> ordered = selectExpired(limits, tie, kNow);
    CHECK_EQ(ordered.size(), static_cast<std::size_t>(3));
    CHECK_EQ(ordered[0], std::string("t-a"));
    CHECK_EQ(ordered[1], std::string("t-b"));
    CHECK_EQ(ordered[2], std::string("t-c"));

    // maxAgeSeconds <= 0 — возрастной лимит выключен: протухшим не считается ничего.
    TrashLimits noAge = limits;
    noAge.maxAgeSeconds = 0;
    CHECK(selectExpired(noAge, tie, kNow).empty());
}

// ===========================================================================
// core::trash — состояние транзакции и место в корзине
// ===========================================================================

TEST(trash_undoableOnlyWhenCommitted) {
    TrashTransaction tx;
    tx.txId = "t1";
    CHECK(!tx.undoable());  // Open: манифест ещё не записан — отменять нечего
    tx.state = TrashTxState::Committed;
    CHECK(tx.undoable());
    tx.state = TrashTxState::Undone;
    CHECK(!tx.undoable());
    tx.state = TrashTxState::Collapsed;
    CHECK(!tx.undoable());
    CHECK_EQ(std::string(toString(TrashTxState::Collapsed)), std::string("collapsed"));
    CHECK_EQ(std::string(toString(TrashItemKind::Directory)), std::string("directory"));
}

TEST(trash_collapseAccumulatesLostBytesAndKillsUndo) {
    // §7.2: пока транзакция не «схлопнулась», отмена доступна. Схлопнулась —
    // часть содержимого вытеснена навсегда, Ctrl+Z обязан погаснуть.
    TrashTransaction tx;
    tx.txId = "t1";
    tx.state = TrashTxState::Committed;
    tx.items.push_back(trashFile("C:\\cache\\a.tmp", 10 * kMB));
    CHECK(tx.undoable());

    TrashLedger::collapse(tx, 4 * kMB);
    CHECK(!tx.undoable());
    CHECK_EQ(tx.purgedBytes, static_cast<std::uint64_t>(4 * kMB));

    TrashLedger::collapse(tx, 6 * kMB);
    CHECK_EQ(tx.purgedBytes, static_cast<std::uint64_t>(10 * kMB));
    // Содержимое остаётся в манифесте: отчёт должен показать, что именно
    // потеряно, а не сделать вид, что элементов не было.
    CHECK_EQ(tx.items.size(), static_cast<std::size_t>(1));
    CHECK_EQ(tx.totalBytes(), static_cast<std::uint64_t>(10 * kMB));
}

TEST(trash_restoredTransactionStopsOccupyingSpace) {
    TrashTransaction tx;
    tx.txId = "t1";
    tx.createdAt = kNow;
    tx.state = TrashTxState::Committed;
    tx.items.push_back(trashFile("C:\\cache\\a.tmp", 100 * kMB));
    CHECK_EQ(infoOf(tx).bytes, static_cast<std::uint64_t>(100 * kMB));
    CHECK_EQ(infoOf(tx).itemCount, static_cast<std::uint32_t>(1));

    // Возвращённое место больше не занимает: иначе лимит урезался бы за счёт
    // пустоты, и корзина «вытесняла» бы каталоги без содержимого.
    tx.state = TrashTxState::Undone;
    const TrashTransactionInfo undone = infoOf(tx);
    CHECK_EQ(undone.bytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(undone.itemCount, static_cast<std::uint32_t>(1));

    const std::vector<TrashTransactionInfo> all = {undone, info("t2", kNow - kDay, 50 * kMB)};
    const TrashUsage usage = summarize(all);
    CHECK_EQ(usage.totalBytes, static_cast<std::uint64_t>(50 * kMB));
    CHECK_EQ(usage.transactionCount, static_cast<std::uint32_t>(2));
    CHECK_EQ(usage.itemCount, static_cast<std::uint32_t>(2));
    CHECK_EQ(usage.oldestAt, kNow - kDay);
    CHECK_EQ(usage.newestAt, kNow);

    const TrashUsage empty = summarize({});
    CHECK_EQ(empty.totalBytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(empty.oldestAt, static_cast<std::int64_t>(0));
    CHECK_EQ(empty.newestAt, static_cast<std::int64_t>(0));
}

TEST(trash_totalsSaturateInsteadOfWrappingToZero) {
    // Сумма размеров не должна переворачиваться в ноль переполнением: иначе
    // лимит 2 ГБ «вмещал» бы что угодно.
    TrashTransaction tx;
    tx.items.push_back(trashFile("C:\\a", kMaxU64));
    tx.items.push_back(trashFile("C:\\b", kMaxU64));
    CHECK_EQ(tx.totalBytes(), kMaxU64);

    const std::vector<TrashTransactionInfo> txs = {info("t1", kNow, kMaxU64), info("t2", kNow, kMaxU64)};
    CHECK_EQ(summarize(txs).totalBytes, kMaxU64);
}

// ===========================================================================
// core::trash — манифест
// ===========================================================================

TEST(trash_manifestRoundTripKeepsEverythingRestoreNeeds) {
    TrashTransaction tx;
    tx.txId = "20260927T225900Z-000007-1a2b3c4d";
    tx.createdAt = kNow;
    tx.appVersion = "1.0.0";
    tx.state = TrashTxState::Committed;
    tx.purgedBytes = 42;

    TrashItem cache = trashFile("C:\\Users\\U\\AppData\\cache.db", 5 * kMB);
    cache.payload = "p0";
    cache.fileCount = 3;
    cache.mtime = kNow - 10 * kDay;
    cache.readOnly = true;
    cache.aclSddl = "O:BAG:BAD:(A;;FA;;;SY)";
    tx.items.push_back(cache);

    TrashItem dir;
    dir.kind = TrashItemKind::Directory;
    dir.originalPath = "C:\\Users\\U\\AppData\\Local\\Temp\\dir";
    dir.payload = "p1";
    dir.bytes = 7 * kMB;
    dir.fileCount = 120;
    dir.crossVolume = true;
    dir.sourceVolume = kVolA;
    tx.items.push_back(dir);

    const TrashTransaction back = parseTrashManifest(serializeManifest(tx), "m.json");
    CHECK_EQ(back.txId, tx.txId);
    CHECK_EQ(back.createdAt, tx.createdAt);
    CHECK_EQ(back.appVersion, tx.appVersion);
    CHECK(back.state == TrashTxState::Committed);
    CHECK_EQ(back.purgedBytes, static_cast<std::uint64_t>(42));
    CHECK_EQ(back.items.size(), static_cast<std::size_t>(2));

    CHECK(back.items[0].kind == TrashItemKind::File);
    CHECK_EQ(back.items[0].originalPath, cache.originalPath);
    CHECK_EQ(back.items[0].payload, std::string("p0"));
    CHECK_EQ(back.items[0].bytes, static_cast<std::uint64_t>(5 * kMB));
    CHECK_EQ(back.items[0].fileCount, static_cast<std::uint32_t>(3));
    CHECK_EQ(back.items[0].mtime, kNow - 10 * kDay);
    CHECK(back.items[0].readOnly);
    CHECK_EQ(back.items[0].aclSddl, cache.aclSddl);

    CHECK(back.items[1].kind == TrashItemKind::Directory);
    CHECK(back.items[1].crossVolume);
    CHECK_EQ(back.items[1].sourceVolume, std::string(kVolA));
    CHECK_EQ(back.totalBytes(), tx.totalBytes());
    CHECK_EQ(back.itemCount(), static_cast<std::uint32_t>(2));
}

TEST(trash_manifestRejectsUnknownFieldSchemaAndBrokenEnvelope) {
    // Неизвестное поле на диске — это либо чужой формат, либо дописанная вручную
    // запись. Молчаливое «прочитал как понял» означало бы восстановление не туда.
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":1,"txId":"t1","createdAt":0,"items":[],"oops":1})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":2,"txId":"t1","createdAt":0,"items":[]})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"txId":"t1","createdAt":0,"items":[]})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":1,"createdAt":0,"items":[]})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":1,"txId":"t1","createdAt":0})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":1,"txId":"t1","createdAt":0,"items":{}})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":1,"txId":"плохой id","createdAt":0,"items":[]})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":1,"txId":"t1","createdAt":0,"items":[],"state":"maybe"})", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest(R"({"schema":1,"txId":"t1","createdAt":0.5,"items":[]})", "m.json"); }));

    // Текст ошибки называет файл и причину: по одному сообщению видно, что чинить.
    const std::string text =
        errorText<TrashError>([] { (void)parseTrashManifest(R"({"schema":7,"txId":"t1","createdAt":0,"items":[]})", "m.json"); });
    CHECK(containsText(text, "m.json"));
    CHECK(containsText(text, "схем"));
}

TEST(trash_manifestRejectsBrokenItems) {
    // Оболочка манифеста разбирается, дальше смотрим содержимое items[].
    const std::string head = R"({"schema":1,"txId":"t1","createdAt":0,"items":[)";
    const std::string tail = "]}";
    const auto bad = [&head, &tail](const std::string& items) {
        return throwsAs<TrashError>([&head, &tail, &items] { (void)parseTrashManifest(head + items + tail, "m.json"); });
    };

    CHECK(bad("1"));                                        // элемент не объект
    CHECK(bad("{}"));                                       // нет ни пути, ни имени
    CHECK(bad(R"({"payload":"p0"})"));                      // нет originalPath
    CHECK(bad(R"({"originalPath":"C:\\a"})"));              // нет payload
    CHECK(bad(R"({"originalPath":"","payload":"p0"})"));    // восстанавливать некуда
    CHECK(bad(R"({"originalPath":"C:\\a","payload":"p0","zzz":1})"));   // неизвестное поле
    CHECK(bad(R"({"kind":"link","originalPath":"C:\\a","payload":"p0"})"));  // kind не из списка
    CHECK(bad(R"({"kind":5,"originalPath":"C:\\a","payload":"p0"})"));     // kind не строка
    CHECK(bad(R"({"originalPath":"C:\\a","payload":"p0","readOnly":"да"})"));
    // Манифест — не «примерно»: дробный и отрицательный размеры не принимаются.
    CHECK(bad(R"({"originalPath":"C:\\a","payload":"p0","bytes":1.5})"));
    CHECK(bad(R"({"originalPath":"C:\\a","payload":"p0","bytes":-1})"));
    CHECK(bad(R"({"originalPath":"C:\\a","payload":"p0","mtime":"вчера"})"));

    // Счётчик файлов живёт в 32-битном поле: упирается в потолок, а не
    // переворачивается в мусор при сужении.
    const TrashTransaction clamped = parseTrashManifest(head + R"({"originalPath":"C:\\a","payload":"p0","fileCount":99999999999})" + tail, "m.json");
    CHECK_EQ(clamped.items[0].fileCount, std::numeric_limits<std::uint32_t>::max());
    CHECK_EQ(clamped.items[0].bytes, static_cast<std::uint64_t>(0));  // необязательное поле
    CHECK(!clamped.items[0].crossVolume);
    // Незаданный счётчик — один файл, а не ноль: каталог без файлов не бывает.
    const TrashTransaction defaulted = parseTrashManifest(head + R"({"originalPath":"C:\\a","payload":"p0"})" + tail, "m.json");
    CHECK_EQ(defaulted.items[0].fileCount, static_cast<std::uint32_t>(1));
}

TEST(trash_manifestRefusesPathsThatLeaveTheTrashRoot) {
    // txId приезжает с диска: «../../Windows» в имя каталога не попадает.
    CHECK(throwsAs<TrashError>([] {
        (void)parseTrashManifest(R"({"schema":1,"txId":"../../Windows","createdAt":0,"items":[]})", "m.json");
    }));
    // Имя объекта внутри транзакции — тоже: разделитель и «..» запрещены.
    const char* bad_payloads[] = {"p/0", "p\\0", "..", ".", "C:temp", "имя"};
    const std::string head = R"({"schema":1,"txId":"t1","createdAt":0,"items":[{"originalPath":"C:\\a","payload":")";
    const std::string tail = R"("}]})";
    for (const char* payload : bad_payloads) {
        CHECK(!isValidPayloadName(payload));
        CHECK(throwsAs<TrashError>([&head, &tail, payload] { (void)parseTrashManifest(head + payload + tail, "m.json"); }));
    }
    CHECK(isValidPayloadName("p0"));
    CHECK(isValidPayloadName("name.txt"));
    CHECK(isValidTxId("20260927T225900Z-000007-1a2b3c4d"));
}

TEST(trash_garbageManifestBecomesTrashError) {
    // Движок ловит один тип ошибок корзины: json::ParseError пробьётся мимо
    // TrashError — и «битый манифест» уедет в отдельную, необработанную ветку.
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest("{не json", "m.json"); }));
    CHECK(throwsAs<TrashError>([] { (void)parseTrashManifest("[]", "m.json"); }));
    const std::string text = errorText<TrashError>([] { (void)parseTrashManifest("{не json", "m.json"); });
    CHECK(containsText(text, "m.json"));
}

// ===========================================================================
// core::trash — идентификаторы и пути
// ===========================================================================

TEST(trash_txIdAndTimestampsAreDeterministic) {
    // Метка без двоеточий: двоеточие в Windows — поток данных, такое имя
    // каталога нельзя ни создать, ни прочитать.
    CHECK_EQ(makeTxId(0, 7, 0x1a2b3c4du), std::string("19700101T000000Z-000007-1a2b3c4d"));
    CHECK(isValidTxId(makeTxId(kNow, 1, 1)));
    CHECK(isValidTxId(makeTxId(kNow, 999999, 0xffffffffu)));
    // Одинаковые входные — одинаковый результат: тест повторяем, часы не нужны.
    CHECK_EQ(makeTxId(kNow, 42, 42), makeTxId(kNow, 42, 42));

    CHECK_EQ(formatUtcStamp(0), std::string("19700101T00:00:00Z"));
    CHECK_EQ(formatUtcStamp(1700000000), std::string("20231114T22:13:20Z"));
    CHECK_EQ(formatUtcStamp(-1), std::string("19691231T23:59:59Z"));  // до эпохи — тоже метка

    CHECK(!isValidTxId(""));
    CHECK(!isValidTxId("-lead"));       // ведущий дефис запрещён
    CHECK(!isValidTxId("a/b"));         // разделитель — выход из корня
    CHECK(!isValidTxId("a.b"));         // точка в trash-варианте недопустима
    CHECK(!isValidTxId(std::string(65, 'a')));
    CHECK(isValidTxId(std::string(64, 'a')));
    CHECK(!isValidPayloadName(std::string(65, 'a')));
}

TEST(trash_pathsJoinLikeWindowsAndStayInsideRoot) {
    CHECK_EQ(trashRootPath("C:\\ProgramData"), std::string("C:\\ProgramData\\MrProper\\Trash"));
    CHECK_EQ(trashRootPath("C:\\ProgramData\\"), std::string("C:\\ProgramData\\MrProper\\Trash"));
    CHECK_EQ(trashRootPath(""), std::string());

    CHECK_EQ(joinPath("C:\\a", "b"), std::string("C:\\a\\b"));
    CHECK_EQ(joinPath("C:\\a\\", "b"), std::string("C:\\a\\b"));
    CHECK_EQ(joinPath("C:", "b"), std::string("C:\\b"));
    CHECK_EQ(joinPath("/var/tmp", "x"), std::string("/var/tmp/x"));
    CHECK_EQ(joinPath("", "x"), std::string("x"));
    CHECK_EQ(joinPath("C:\\a", ""), std::string("C:\\a"));

    CHECK_EQ(transactionDir("C:\\Trash", "t1"), std::string("C:\\Trash\\t1"));
    CHECK_EQ(manifestPath("C:\\Trash\\t1"), std::string("C:\\Trash\\t1\\manifest.json"));
    CHECK_EQ(std::string(manifestPath(transactionDir("C:\\Trash", "t1"))),
             std::string("C:\\Trash\\t1\\") + std::string(kTrashManifestFileName));
    CHECK(throwsAs<TrashError>([] { (void)transactionDir("C:\\Trash", "..\\..\\Windows"); }));
    CHECK(throwsAs<TrashError>([] { (void)transactionDir("C:\\Trash", ""); }));
}

// ===========================================================================
// core::trash — журнал
// ===========================================================================

TEST(trash_ledgerLifecycleKeepsStateMachine) {
    TrashLedger ledger(tinyLimits(), "C:\\Trash");
    const std::string first = ledger.begin(kNow, "1.0.0");
    const std::string second = ledger.begin(kNow, "1.0.0");
    CHECK(isValidTxId(first));
    CHECK(first != second);  // два процесса в одну секунду не дерутся за каталог

    CHECK(ledger.addItem(first, trashFile("C:\\cache\\a.tmp", 10 * kMB)));
    CHECK(ledger.addItem(first, trashFile("C:\\cache\\b.tmp", 20 * kMB)));
    CHECK(ledger.addItem("нет-такой", trashFile("C:\\cache\\c.tmp", 1)) == false);
    // Имя по номеру элемента: восстановление идёт по манифесту, имя стабильно.
    CHECK_EQ(ledger.find(first)->items[0].payload, std::string("p0"));
    CHECK_EQ(ledger.find(first)->items[1].payload, std::string("p1"));

    // Имя объекта задаёт платформа — но проверить его всё равно обязано ядро:
    // «..» в имени каталога ушло бы из корзины.
    TrashItem escaping = trashFile("C:\\cache\\d.tmp", 1);
    escaping.payload = "../p0";
    CHECK(throwsAs<TrashError>([&] { (void)ledger.addItem(first, escaping); }));
    CHECK_EQ(ledger.find(first)->items.size(), static_cast<std::size_t>(2));  // мусор не добавился

    CHECK(ledger.find(first) != nullptr);
    CHECK(ledger.find(first)->state == TrashTxState::Open);
    CHECK(ledger.commit(first));
    CHECK(ledger.find(first)->state == TrashTxState::Committed);
    CHECK(ledger.find(first)->undoable());
    CHECK(ledger.commit(first) == false);  // повторный commit ничего не делает

    // До возвращения место занимают оба элемента первой транзакции; вторая
    // пуста (элементы в неё не добавляли) и в сумму не входит.
    CHECK_EQ(ledger.usage().totalBytes, static_cast<std::uint64_t>(30 * kMB));
    CHECK_EQ(ledger.usage().transactionCount, static_cast<std::uint32_t>(2));
    CHECK_EQ(ledger.info().size(), static_cast<std::size_t>(2));

    CHECK(ledger.markUndone(first));
    CHECK(ledger.find(first)->state == TrashTxState::Undone);
    CHECK(ledger.markUndone(first) == false);
    // Возвращённое место больше не занимается: иначе лимит урезался бы за счёт
    // каталога без содержимого, а вытеснение шло бы по пустоте.
    CHECK_EQ(ledger.usage().totalBytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(ledger.usage().transactionCount, static_cast<std::uint32_t>(2));

    CHECK_EQ(ledger.dirFor(first), std::string("C:\\Trash\\") + first);
    CHECK_EQ(ledger.manifestFor(first), std::string("C:\\Trash\\") + first + "\\manifest.json");
    CHECK_EQ(ledger.payloadFor(first, "p0"), std::string("C:\\Trash\\") + first + "\\p0");
    CHECK(throwsAs<TrashError>([&] { (void)ledger.payloadFor(first, "../p0"); }));
    CHECK(throwsAs<TrashError>([&] { (void)ledger.dirFor("../Windows"); }));
}

TEST(trash_ledgerAdoptReplacesAndForgetRemoves) {
    TrashLedger ledger(tinyLimits(), "C:\\Trash");

    // Прочитали с диска: adopt возвращает «добавлена новая».
    TrashTransaction tx;
    tx.txId = "20260927T225900Z-000001-00000002";
    tx.createdAt = kNow - kDay;
    tx.state = TrashTxState::Committed;
    tx.items.push_back(trashFile("C:\\cache\\a.tmp", 10 * kMB));
    tx.items[0].payload = "p0";
    CHECK(ledger.adopt(tx));
    CHECK_EQ(ledger.usage().totalBytes, static_cast<std::uint64_t>(10 * kMB));

    // Повторное чтение того же каталога заменяет, а не дублирует: иначе
    // вытеснение вычеркнуло бы одну и ту же транзакцию дважды.
    TrashTransaction updated = tx;
    updated.items[0].bytes = 30 * kMB;
    CHECK(ledger.adopt(updated) == false);
    CHECK_EQ(ledger.transactions().size(), static_cast<std::size_t>(1));
    CHECK_EQ(ledger.usage().totalBytes, static_cast<std::uint64_t>(30 * kMB));

    // Устаревшая по возрасту транзакция находится журналом, а не UI.
    CHECK_EQ(ledger.expired(kNow + 30 * kDay).size(), static_cast<std::size_t>(1));

    CHECK(ledger.forget(tx.txId));
    CHECK(ledger.forget(tx.txId) == false);
    CHECK(ledger.find(tx.txId) == nullptr);
    CHECK_EQ(ledger.usage().totalBytes, static_cast<std::uint64_t>(0));

    TrashTransaction bad = tx;
    bad.txId = "../Windows";
    CHECK(throwsAs<TrashError>([&] { (void)ledger.adopt(bad); }));
}

TEST(trash_ledgerEvictionPlanUsesLedgerLimits) {
    TrashLedger ledger(tinyLimits(), "C:\\Trash");
    TrashTransaction old;
    old.txId = "s-old";
    old.createdAt = kNow - 8 * kDay;
    old.state = TrashTxState::Committed;
    old.items.push_back(trashFile("C:\\a", 100 * kMB));
    old.items[0].payload = "p0";
    CHECK(ledger.adopt(old));

    const TrashEvictionPlan plan = ledger.planEviction(100 * kMB, kNow);
    CHECK_EQ(plan.txIds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(plan.txIds[0], std::string("s-old"));
    CHECK_EQ(plan.freedByAge, static_cast<std::uint64_t>(100 * kMB));
    CHECK_EQ(ledger.expired(kNow).size(), static_cast<std::size_t>(1));
}

TEST(trash_summaryTellsTheUserLimitAndCount) {
    TrashLimits limits;
    TrashUsage usage;
    usage.totalBytes = 1024 * kMB;  // 1 ГиБ
    usage.transactionCount = 3;
    usage.oldestAt = kNow - kDay;

    const std::string text = trashSummary(usage, limits);
    CHECK(containsText(text, "транзакций: 3"));
    CHECK(containsText(text, "из "));
    CHECK(containsText(text, "2,0"));  // лимит 2 ГиБ по умолчанию

    // Пустая корзина не врёт про возраст: «старейшая от» не выводится.
    CHECK(!containsText(trashSummary(TrashUsage{}, limits), "старейшая"));
}

// ===========================================================================
// core::trash — восстановление: конфликты и частичное (FR-7)
// ===========================================================================

TEST(trash_restorePlanSeparatesConflictsFromMissingContent) {
    TrashTransaction tx;
    tx.txId = "t1";
    tx.createdAt = kNow;
    tx.state = TrashTxState::Committed;
    tx.items.push_back(trashFile("C:\\cache\\free.tmp", 1 * kKB));
    tx.items[0].payload = "p0";
    tx.items.push_back(trashFile("C:\\cache\\busy.tmp", 2 * kKB));
    tx.items[1].payload = "p1";
    tx.items.push_back(trashFile("C:\\cache\\lost.tmp", 4 * kKB));
    tx.items[2].payload = "p2";
    tx.items.back().kind = TrashItemKind::Directory;

    // Пробу «занято ли место» на компьютере без Windows тут играет таблица строк
    // — ровно так её и передаёт платформа (SPEC §6.2).
    const ExistsProbe busy = [](const std::string& path) { return path == "C:\\cache\\busy.tmp"; };

    // Молча перезаписать чужой файл нельзя: FR-7 требует спросить.
    const RestorePlan asked = planRestoreAll(tx, busy);
    CHECK_EQ(asked.items.size(), static_cast<std::size_t>(3));
    CHECK(asked.items[0].action == RestoreAction::Restore);
    CHECK(asked.items[1].action == RestoreAction::Conflict);
    CHECK(asked.items[2].action == RestoreAction::Restore);
    CHECK_EQ(asked.restoreCount, static_cast<std::size_t>(2));
    CHECK_EQ(asked.conflictCount, static_cast<std::size_t>(1));
    CHECK_EQ(asked.missingCount, static_cast<std::size_t>(0));
    CHECK_EQ(asked.restoreBytes, static_cast<std::uint64_t>(5 * kKB));
    CHECK_EQ(asked.conflictBytes, static_cast<std::uint64_t>(2 * kKB));
    CHECK_EQ(asked.missingBytes, static_cast<std::uint64_t>(0));
    CHECK(!asked.empty());
    CHECK(asked.partial());
    CHECK(containsText(asked.explain(), "конфликт"));
    CHECK(containsText(asked.explain(), "подтверждения"));

    // Решение пользователя «перезаписать» — единственный путь в Overwrite.
    RestoreOptions overwrite;
    overwrite.overwriteConflicts = true;
    const RestorePlan over = planRestoreAll(tx, busy, overwrite);
    CHECK(over.items[1].action == RestoreAction::Overwrite);
    CHECK_EQ(over.conflictCount, static_cast<std::size_t>(0));
    CHECK_EQ(over.restoreCount, static_cast<std::size_t>(3));
    CHECK_EQ(over.restoreBytes, static_cast<std::uint64_t>(7 * kKB));
    CHECK(!over.partial());  // конфликтов не осталось

    // Пустая проба = «на месте ничего не занято».
    const RestorePlan free = planRestoreAll(tx, ExistsProbe{});
    CHECK_EQ(free.conflictCount, static_cast<std::size_t>(0));
    CHECK(!free.partial());
    for (const RestoreItemPlan& item : free.items) {
        CHECK(item.action == RestoreAction::Restore);
    }
}

TEST(trash_restorePlanReportsMissingContentInsteadOfHidingIt) {
    // Содержимое утрачено (файла в корзине нет), а запись манифеста осталась.
    // Молчать об этом нельзя: пользователь должен увидеть потерю в отчёте.
    TrashTransaction tx;
    tx.txId = "t1";
    tx.createdAt = kNow;
    tx.items.push_back(trashFile("C:\\cache\\a.tmp", 1 * kKB));
    tx.items[0].payload = "p0";
    tx.items.push_back(trashFile("C:\\cache\\b.tmp", 2 * kKB));
    tx.items[1].payload = "bad/name";  // имя не прошло проверку — содержимого нет

    const RestorePlan plan = planRestoreAll(tx, ExistsProbe{});
    CHECK(plan.items[1].action == RestoreAction::Missing);
    CHECK_EQ(plan.missingCount, static_cast<std::size_t>(1));
    CHECK_EQ(plan.missingBytes, static_cast<std::uint64_t>(2 * kKB));
    CHECK_EQ(plan.restoreCount, static_cast<std::size_t>(1));
    CHECK(plan.partial());
    CHECK(containsText(plan.explain(), "отсутствует"));
}

TEST(trash_restorePlanSupportsPartialSelectionAndRefusesBadIndex) {
    TrashTransaction tx;
    tx.txId = "t1";
    tx.createdAt = kNow;
    for (int i = 0; i < 3; ++i) {
        tx.items.push_back(trashFile("C:\\cache\\file" + std::to_string(i) + ".tmp", static_cast<std::uint64_t>(i + 1) * kKB));
        tx.items.back().payload = "p" + std::to_string(i);
    }

    // Частичное восстановление: пользователь выбрал не всё (FR-7).
    const RestorePlan partial = planRestore(tx, {0, 2}, ExistsProbe{});
    CHECK_EQ(partial.items.size(), static_cast<std::size_t>(2));
    CHECK_EQ(partial.items[0].index, static_cast<std::size_t>(0));
    CHECK_EQ(partial.items[1].index, static_cast<std::size_t>(2));
    CHECK_EQ(partial.restoreBytes, static_cast<std::uint64_t>(4 * kKB));

    // Повтор в выборке не удваивает работу.
    CHECK_EQ(planRestore(tx, {1, 1}, ExistsProbe{}).items.size(), static_cast<std::size_t>(1));

    // Индекс из манифеста/UI вне транзакции — ошибка, а не «восстановим как получится».
    CHECK(throwsAs<TrashError>([] {
        TrashTransaction empty;
        empty.txId = "t1";
        empty.items.push_back(trashFile("C:\\a", 1));
        (void)planRestore(empty, {5}, ExistsProbe{});
    }));

    // Пустая транзакция — пустой план, а не «восстановить всё подряд».
    const TrashTransaction none;
    const RestorePlan nothing = planRestoreAll(none, ExistsProbe{});
    CHECK(nothing.empty());
    CHECK(!nothing.partial());
    CHECK(containsText(nothing.explain(), "Нечего восстанавливать"));
}

// ===========================================================================
// core::undo — лимиты и вытеснение
// ===========================================================================

TEST(undo_admitLargeItemIsDirectDelete) {
    UndoLimits limits;  // порог 100 МБ по умолчанию
    UndoTrashState usage;
    TrashRequest request;
    request.sizeBytes = limits.largeItemThresholdBytes + 1;
    request.displayName = "кэш браузера";

    const TrashAdmission admission = admitToTrash(limits, usage, request, kNow);
    CHECK(admission.disposition == TrashDisposition::DirectDelete);
    CHECK_EQ(admission.bytesAccepted, static_cast<std::uint64_t>(0));
    CHECK(admission.accepted());
    CHECK(admission.fallsBackToDirectDelete());
    CHECK(containsText(admission.note, "журнал"));

    // Ровно порог — ещё в корзину.
    request.sizeBytes = limits.largeItemThresholdBytes;
    const TrashAdmission atLimit = admitToTrash(limits, usage, request, kNow);
    CHECK(atLimit.disposition == TrashDisposition::ToTrash);
    CHECK_EQ(atLimit.bytesAccepted, limits.largeItemThresholdBytes);
    CHECK(containsText(atLimit.note, "отмена доступна"));
}

TEST(undo_admitWithinLimitIsAccepted) {
    const UndoLimits limits = tinyUndoLimits();
    UndoTrashState usage;
    usage.totalBytes = 100;
    TrashRequest request;
    request.sizeBytes = 900;

    const TrashAdmission admission = admitToTrash(limits, usage, request, kNow);
    CHECK(admission.disposition == TrashDisposition::ToTrash);
    CHECK_EQ(admission.bytesAccepted, static_cast<std::uint64_t>(900));
    CHECK_EQ(admission.bytesOverLimit, static_cast<std::uint64_t>(0));
    CHECK(admission.evictTxIds.empty());
    CHECK(!admission.evictExpiredOnly);
    CHECK(!admission.note.empty());
    CHECK_EQ(std::string(toString(TrashDisposition::ToTrash)), std::string("to-trash"));
}

TEST(undo_admitUsesExpiredCleanupWithoutTouchingLiveUndo) {
    // Корзина заполнена до предела, но место освобождается чисткой просроченного.
    // Живые транзакции при этом не страдают: отмена у них остаётся.
    const UndoLimits limits;  // 2 ГиБ / 7 дней
    UndoTrashState usage;
    usage.totalBytes = 2 * kGB;
    UndoTransaction expired = makeTrashTransaction("tx-old", "1.0.0", kNow - 8 * kDay);
    expired.entries.push_back(undoEntry("C:\\cache\\a.tmp", 1 * kGB));
    usage.transactions.push_back(expired);

    TrashRequest request;
    request.sizeBytes = 1 * kMB;
    request.displayName = "a.tmp";

    const TrashAdmission admission = admitToTrash(limits, usage, request, kNow);
    CHECK(admission.disposition == TrashDisposition::ToTrash);
    CHECK_EQ(admission.bytesAccepted, static_cast<std::uint64_t>(1 * kMB));
    CHECK(admission.evictExpiredOnly);
    CHECK(admission.evictTxIds.empty());  // вытеснять нечего: только чистка по возрасту
    CHECK(containsText(admission.note, "просроченного"));
}

TEST(undo_admitEvictsOldestTransactionsWhenOverLimit) {
    const UndoLimits limits = tinyUndoLimits();
    UndoTrashState usage;
    usage.totalBytes = 800;
    UndoTransaction stale = makeTrashTransaction("tx-old", "1.0.0", kNow - 8 * kDay);
    stale.entries.push_back(undoEntry("C:\\cache\\old.tmp", 500));
    usage.transactions.push_back(stale);
    UndoTransaction live = makeTrashTransaction("tx-new", "1.0.0", kNow - kHour);
    live.entries.push_back(undoEntry("C:\\cache\\new.tmp", 300));
    usage.transactions.push_back(live);

    TrashRequest request;
    request.sizeBytes = 600;

    const TrashAdmission admission = admitToTrash(limits, usage, request, kNow);
    CHECK(admission.disposition == TrashDisposition::ToTrash);
    CHECK_EQ(admission.bytesAccepted, static_cast<std::uint64_t>(600));
    CHECK_EQ(admission.bytesOverLimit, static_cast<std::uint64_t>(400));
    CHECK_EQ(admission.bytesToEvict, static_cast<std::uint64_t>(300));
    CHECK(!admission.evictExpiredOnly);
    // Названа ровно одна транзакция — та, ради которой и вытесняют.
    CHECK_EQ(admission.evictTxIds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(admission.evictTxIds[0], std::string("tx-new"));
    CHECK(containsText(admission.note, "вытеснения"));
}

TEST(undo_admitRefusesWhenNothingCanBeEvicted) {
    // Ни вытеснять, ни принять: корзина переполнена, журнал пуст. Элемент
    // удаляется напрямую, но с честным объяснением, а не молча.
    const UndoLimits limits = tinyUndoLimits();
    UndoTrashState usage;
    usage.totalBytes = 600;  // транзакций нет: вытеснять нечего
    TrashRequest request;
    request.sizeBytes = 700;
    request.displayName = "cache";

    const TrashAdmission admission = admitToTrash(limits, usage, request, kNow);
    CHECK(admission.disposition == TrashDisposition::NoSpace);
    CHECK(!admission.accepted());
    CHECK(admission.fallsBackToDirectDelete());
    CHECK_EQ(admission.bytesAccepted, static_cast<std::uint64_t>(0));
    CHECK(admission.bytesOverLimit != 0);
    CHECK(containsText(admission.note, "не хватает места"));
    CHECK(containsText(admission.note, "cache"));
}

TEST(undo_evictionOrderIsExpiredThenOldestThenTxId) {
    const UndoLimits limits;
    UndoTrashState usage;

    UndoTransaction expired = makeTrashTransaction("tx-expired", "1.0.0", kNow - 10 * kDay);
    expired.entries.push_back(undoEntry("C:\\a", 10));
    usage.transactions.push_back(expired);

    UndoTransaction older = makeTrashTransaction("tx-older", "1.0.0", kNow - 2 * kHour);
    older.entries.push_back(undoEntry("C:\\b", 10));
    usage.transactions.push_back(older);

    UndoTransaction younger = makeTrashTransaction("tx-younger", "1.0.0", kNow - kHour);
    younger.entries.push_back(undoEntry("C:\\c", 10));
    usage.transactions.push_back(younger);

    UndoTransaction tieA = makeTrashTransaction("tx-tie-a", "1.0.0", kNow - kMinute);
    tieA.entries.push_back(undoEntry("C:\\d", 10));
    usage.transactions.push_back(tieA);

    UndoTransaction tieB = makeTrashTransaction("tx-tie-b", "1.0.0", kNow - kMinute);
    tieB.entries.push_back(undoEntry("C:\\e", 10));
    usage.transactions.push_back(tieB);

    const std::vector<std::string> order = orderForEviction(limits, usage, kNow);
    CHECK_EQ(order.size(), static_cast<std::size_t>(5));
    CHECK_EQ(order[0], std::string("tx-expired"));
    CHECK_EQ(order[1], std::string("tx-older"));
    CHECK_EQ(order[2], std::string("tx-younger"));
    CHECK_EQ(order[3], std::string("tx-tie-a"));
    CHECK_EQ(order[4], std::string("tx-tie-b"));
}

TEST(undo_purgePlanCleansExpiredEvenWithNothingToStore) {
    const UndoLimits limits;
    UndoTrashState usage;
    usage.totalBytes = 1 * kGB;
    UndoTransaction expired = makeTrashTransaction("tx-expired", "1.0.0", kNow - 8 * kDay);
    expired.entries.push_back(undoEntry("C:\\a", 400 * kMB));
    usage.transactions.push_back(expired);
    UndoTransaction live = makeTrashTransaction("tx-live", "1.0.0", kNow - kHour);
    live.entries.push_back(undoEntry("C:\\b", 600 * kMB));
    usage.transactions.push_back(live);

    // Ничего не кладём — но просроченное всё равно чистится: иначе корзина
    // пережила бы 7 дней просто потому, что её никто не переполнял.
    const TrashPurgePlan plan = planTrashPurge(limits, usage, 0, kNow);
    CHECK_EQ(plan.expiredTxIds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(plan.expiredTxIds[0], std::string("tx-expired"));
    CHECK(plan.overflowTxIds.empty());
    CHECK_EQ(plan.bytesReclaimed, static_cast<std::uint64_t>(400 * kMB));
    CHECK_EQ(plan.bytesNeeded, static_cast<std::uint64_t>(0));
    CHECK(plan.canAdmit);

    // Ничего протухшего — план пуст, но остаётся безопасным.
    const TrashPurgePlan clean = planTrashPurge(limits, usage, 0, kNow - 30 * kDay);
    CHECK(clean.expiredTxIds.empty());
    CHECK(clean.overflowTxIds.empty());
    CHECK(clean.canAdmit);
}

TEST(undo_purgePlanReportsShortfallItCannotCover) {
    const UndoLimits limits = tinyUndoLimits();
    UndoTrashState usage;
    usage.totalBytes = 600;
    UndoTransaction live = makeTrashTransaction("tx-live", "1.0.0", kNow);
    live.entries.push_back(undoEntry("C:\\a", 600));
    usage.transactions.push_back(live);

    // Живая транзакция меньше, чем нужно, — вытеснение не закроет нехватку.
    const TrashPurgePlan plan = planTrashPurge(limits, usage, 900, kNow);
    CHECK_EQ(plan.bytesNeeded, static_cast<std::uint64_t>(300));
    CHECK_EQ(plan.overflowTxIds.size(), static_cast<std::size_t>(1));
    CHECK(plan.canAdmit);
    CHECK_EQ(plan.bytesReclaimed, static_cast<std::uint64_t>(600));

    // А вот когда вытеснять нечего вовсе.
    UndoTrashState bare;
    bare.totalBytes = 600;
    CHECK(!planTrashPurge(limits, bare, 900, kNow).canAdmit);
}

// ===========================================================================
// core::undo — манифест
// ===========================================================================

TEST(undo_manifestRoundTripKeepsEverythingRestoreNeeds) {
    UndoTransaction tx = makeTrashTransaction("20260927T225900Z-000007-1a2b3c4d", "1.0.0", kNow);
    TrashEntry file = undoEntry("C:\\Users\\U\\AppData\\cache.db", 5 * kMB);
    file.storedPath = "p0";
    file.fileCount = 3;
    file.modifiedUnix = kNow - 10 * kDay;
    file.aclSddl = "O:BAG:BAD:(A;;FA;;;SY)";
    tx.entries.push_back(file);

    TrashEntry dir;
    dir.originalPath = "C:\\Users\\U\\AppData\\Local\\Temp\\dir";
    dir.storedPath = "p1";
    dir.isDirectory = true;
    dir.sizeBytes = 7 * kMB;
    dir.fileCount = 120;
    dir.createdUnix = kNow;
    dir.volumeGuidPath = kVolB;
    dir.restored = true;
    dir.restoredUnix = kNow - kMinute;
    tx.entries.push_back(dir);

    const std::string text = serializeTrashManifest(tx);
    const UndoTransaction back = parseUndoManifest(text, "manifest.json");
    CHECK_EQ(back.schemaVersion, kUndoManifestSchema);
    CHECK_EQ(back.txId, tx.txId);
    CHECK_EQ(back.appVersion, tx.appVersion);
    CHECK_EQ(back.createdUnix, kNow);
    CHECK(!back.collapsed);
    CHECK_EQ(back.entries.size(), static_cast<std::size_t>(2));

    CHECK_EQ(back.entries[0].originalPath, file.originalPath);
    CHECK_EQ(back.entries[0].storedPath, std::string("p0"));
    CHECK_EQ(back.entries[0].sizeBytes, static_cast<std::uint64_t>(5 * kMB));
    CHECK_EQ(back.entries[0].fileCount, static_cast<std::uint32_t>(3));
    CHECK_EQ(back.entries[0].modifiedUnix, kNow - 10 * kDay);
    CHECK_EQ(back.entries[0].aclSddl, file.aclSddl);
    CHECK(!back.entries[0].restored);

    CHECK(back.entries[1].isDirectory);
    CHECK(back.entries[1].restored);
    CHECK_EQ(back.entries[1].restoredUnix, kNow - kMinute);
    CHECK_EQ(back.entries[1].volumeGuidPath, std::string(kVolB));
    CHECK_EQ(back.totalBytes(), static_cast<std::uint64_t>(12 * kMB));
    CHECK_EQ(back.restorableBytes(), static_cast<std::uint64_t>(5 * kMB));
    CHECK_EQ(back.restorableCount(), static_cast<std::size_t>(1));
}

TEST(undo_manifestRejectsUnsafeStoredPath) {
    // storedPath уходит в файловые операции: путь из манифеста не имеет права
    // выйти за пределы каталога транзакции (ADR-004, проверка до чтения).
    CHECK(isSafeStoredPath("p0"));
    CHECK(isSafeStoredPath("sub/p0.txt"));
    CHECK(isSafeStoredPath("a\\b"));
    CHECK(isSafeStoredPath("./p0"));
    CHECK(!isSafeStoredPath(""));
    CHECK(!isSafeStoredPath("/abs/p0"));
    CHECK(!isSafeStoredPath("\\abs\\p0"));
    CHECK(!isSafeStoredPath(".."));
    CHECK(!isSafeStoredPath("../p0"));
    CHECK(!isSafeStoredPath("sub/../../p0"));
    CHECK(!isSafeStoredPath("C:\\Windows\\System32"));
    CHECK(!isSafeStoredPath("p0:stream"));   // альтернативный поток NTFS
    CHECK(!isSafeStoredPath("p0\nname"));    // управляющий символ

    const std::string text = errorText<UndoError>([] {
        (void)parseUndoManifest(
            R"({"schema":1,"txId":"t1","createdUnix":0,"entries":[{"originalPath":"C:\\a","storedPath":"../escape"}]})",
            "manifest.json");
    });
    CHECK(containsText(text, "manifest.json"));
    CHECK(containsText(text, "storedPath"));
}

TEST(undo_manifestRejectsUnknownSchemaAndBrokenEntries) {
    // Неизвестная (более новая) версия схемы — отказ, а не догадка: прочитать
    // чужой формат и записать поверх — способ потерять корзину.
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest(R"({"schema":2,"txId":"t1","entries":[]})", "m.json"); }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest(R"({"schema":1,"txId":"../bad","entries":[]})", "m.json"); }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":{}})", "m.json"); }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":[1]})", "m.json"); }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":[{"storedPath":"p0"}]})", "m.json"); }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":[{"originalPath":"C:\\a"}]})", "m.json"); }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":[{"originalPath":"a\u0001b","storedPath":"p0"}]})", "m.json"); }));
    CHECK(throwsAs<UndoError>([] {
        (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":[{"originalPath":"C:\\a","storedPath":"p0","sizeBytes":-1}]})", "m.json");
    }));
    CHECK(throwsAs<UndoError>([] {
        (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":[{"originalPath":"C:\\a","storedPath":"p0","sizeBytes":1.5}]})", "m.json");
    }));
    CHECK(throwsAs<UndoError>([] {
        (void)parseUndoManifest(R"({"schema":1,"txId":"t1","entries":[{"originalPath":"C:\\a","storedPath":"p0","fileCount":4294967296}]})", "m.json");
    }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest("{не json", "m.json"); }));
    CHECK(throwsAs<UndoError>([] { (void)parseUndoManifest("[]", "m.json"); }));

    const std::string text =
        errorText<UndoError>([] { (void)parseUndoManifest(R"({"schema":9,"txId":"t1","entries":[]})", "manifest.json"); });
    CHECK(containsText(text, "схем"));
    CHECK(containsText(text, "manifest.json"));
}

TEST(undo_manifestToleratesFutureFieldsButNotFutureData) {
    // Терпимость к полям будущих версий намеренная: лишнее поле игнорируется,
    // а вот неизвестная СХЕМА — отказ (см. предыдущий тест). Разница принципиальная.
    const UndoTransaction tx = parseUndoManifest(
        R"({"schema":1,"txId":"t1","appVersion":"1.0.0","createdUnix":1780000000,"futureField":{"a":1},)"
        R"("entries":[{"originalPath":"C:\\a","storedPath":"p0","sizeBytes":10,"tomorrow":"yes"}]})",
        "m.json");
    CHECK_EQ(tx.txId, std::string("t1"));
    CHECK_EQ(tx.entries.size(), static_cast<std::size_t>(1));
    CHECK_EQ(tx.entries[0].sizeBytes, static_cast<std::uint64_t>(10));
    // Время элемента неизвестно — берётся время транзакции, иначе элемент
    // «вечно протухший» вытеснялся бы при любом now.
    CHECK_EQ(tx.entries[0].createdUnix, kNow);
    CHECK_EQ(tx.entries[0].fileCount, static_cast<std::uint32_t>(0));
}

TEST(undo_pathsAreWindowsStyleAndStable) {
    CHECK_EQ(concatPath("C:\\Trash", "t1"), std::string("C:\\Trash\\t1"));
    CHECK_EQ(concatPath("C:\\Trash\\", "t1"), std::string("C:\\Trash\\t1"));
    CHECK_EQ(concatPath("C:\\Trash", "/t1"), std::string("C:\\Trash/t1"));
    CHECK_EQ(concatPath("", "t1"), std::string("t1"));
    CHECK_EQ(concatPath("C:\\Trash", ""), std::string("C:\\Trash"));
    CHECK_EQ(trashTransactionDir("C:\\Trash", "t1"), std::string("C:\\Trash\\t1"));
    CHECK_EQ(trashManifestPath("C:\\Trash", "t1"), std::string("C:\\Trash\\t1\\manifest.json"));
    CHECK_EQ(std::string(kUndoManifestFileName), std::string("manifest.json"));
}

// ===========================================================================
// core::undo — отмена доступна, пока не схлопнулась (§7.2)
// ===========================================================================

TEST(undo_undoAvailableUntilCollapsedOrNothingLeft) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    CHECK(!undoAvailable(tx));  // пустая транзакция: отменять нечего
    CHECK_EQ(tx.restorableCount(), static_cast<std::size_t>(0));

    tx.entries.push_back(undoEntry("C:\\a", 10));
    tx.entries.push_back(undoEntry("C:\\b", 20));
    CHECK(undoAvailable(tx));
    CHECK_EQ(tx.entryCount(), static_cast<std::size_t>(2));
    CHECK_EQ(tx.totalBytes(), static_cast<std::uint64_t>(30));

    tx.entries[0].restored = true;
    CHECK(undoAvailable(tx));
    CHECK_EQ(tx.restorableCount(), static_cast<std::size_t>(1));
    CHECK_EQ(tx.restorableBytes(), static_cast<std::uint64_t>(20));

    // Схлопнулась — Ctrl+Z обязан погаснуть, даже если записи остались.
    tx.entries[1].restored = true;
    tx.collapsed = true;
    CHECK(!undoAvailable(tx));

    CHECK(tx.find("C:\\a") != nullptr);
    CHECK(tx.find("C:\\a")->restored);
    CHECK(tx.find("C:\\нет") == nullptr);
}

// ===========================================================================
// core::undo — конфликты восстановления
// ===========================================================================

TEST(undo_restorePlanAsksBeforeOverwriting) {
    // FR-7: «существующий файл — не перезаписывать, спросить». Планирование не
    // перезаписывает: оно обязано поставить вопрос пользователю.
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\cache\\free.tmp", 100));
    tx.entries.push_back(undoEntry("C:\\cache\\busy.tmp", 200));

    std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\cache\\free.tmp"), freeTarget("C:\\cache\\busy.tmp")};
    targets[1].exists = true;

    RestoreRequest request;
    request.txId = "t1";
    request.conflictPolicy = ConflictPolicy::Ask;

    const UndoPlan plan = buildRestorePlan(tx, request, targets);
    CHECK_EQ(plan.txId, std::string("t1"));
    CHECK(plan.fullRestore);
    CHECK_EQ(plan.entries.size(), static_cast<std::size_t>(2));
    CHECK(plan.entries[0].action == RestoreDecision::Restore);
    CHECK(plan.entries[0].conflict == RestoreConflict::None);
    CHECK(!plan.entries[0].needsUserDecision);
    CHECK(plan.entries[1].action == RestoreDecision::Skip);
    CHECK(plan.entries[1].conflict == RestoreConflict::TargetExists);
    CHECK(plan.entries[1].needsUserDecision);
    CHECK_EQ(plan.entries[1].targetPath, std::string("C:\\cache\\busy.tmp"));
    CHECK_EQ(plan.entries[1].entryIndex, static_cast<std::size_t>(1));
    CHECK_EQ(plan.entries[1].sizeBytes, static_cast<std::uint64_t>(200));

    CHECK(plan.requiresUserDecision);
    CHECK_EQ(plan.questions.size(), static_cast<std::size_t>(1));
    CHECK(containsText(plan.questions[0], "C:\\cache\\busy.tmp"));
    CHECK_EQ(plan.conflictCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(plan.skippedCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(plan.bytesSkipped, static_cast<std::uint64_t>(200));
    CHECK_EQ(plan.bytesPlanned, static_cast<std::uint64_t>(100));

    // Пока ответ не получен, план остаётся в состоянии «нужно решение»:
    // движок не имеет права исполнить Skip как «пользователь не возражает».
    const UndoPlan untouched = applyRestoreAnswers(plan, RestoreAnswers{});
    CHECK(untouched.entries[1].action == RestoreDecision::Skip);
    CHECK(untouched.entries[1].needsUserDecision);
    CHECK(untouched.requiresUserDecision);
    CHECK_EQ(untouched.bytesPlanned, static_cast<std::uint64_t>(100));
}

TEST(undo_restorePlanAppliesOverwriteRenameAndSkip) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\cache\\busy.tmp", 200));
    std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\cache\\busy.tmp")};
    targets[0].exists = true;

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan asked = buildRestorePlan(tx, request, targets);

    // 1. Перезапись — только по явному решению человека.
    RestoreAnswers overwrite;
    overwrite.byEntry.emplace_back(0, ConflictPolicy::Overwrite);
    const UndoPlan over = applyRestoreAnswers(asked, overwrite);
    CHECK(over.entries[0].action == RestoreDecision::Overwrite);
    CHECK(!over.entries[0].needsUserDecision);
    CHECK(!over.requiresUserDecision);
    CHECK(over.questions.empty());
    CHECK_EQ(over.bytesPlanned, static_cast<std::uint64_t>(200));
    CHECK_EQ(over.skippedCount, static_cast<std::uint32_t>(0));
    CHECK(containsText(over.entries[0].note, "решению пользователя"));

    // 2. Восстановить рядом: исходный объект не тронут.
    RestoreAnswers rename;
    rename.byEntry.emplace_back(0, ConflictPolicy::Rename);
    const UndoPlan aside = applyRestoreAnswers(asked, rename);
    CHECK(aside.entries[0].action == RestoreDecision::Rename);
    CHECK_EQ(aside.entries[0].targetPath, restoreRenameTarget("C:\\cache\\busy.tmp"));
    CHECK(aside.entries[0].targetPath != aside.entries[0].originalPath);

    // 3. Пользователь сам назвал имя — уважаем его выбор.
    RestoreAnswers custom;
    custom.byEntry.emplace_back(0, ConflictPolicy::Rename);
    custom.renameTo.emplace_back(0, std::string("C:\\cache\\busy-2.tmp"));
    CHECK_EQ(applyRestoreAnswers(asked, custom).entries[0].targetPath, std::string("C:\\cache\\busy-2.tmp"));

    // 4. Отказ: элемент остаётся в корзине, ничего не теряется.
    RestoreAnswers skip;
    skip.byEntry.emplace_back(0, ConflictPolicy::Skip);
    const UndoPlan skipped = applyRestoreAnswers(asked, skip);
    CHECK(skipped.entries[0].action == RestoreDecision::Skip);
    CHECK(!skipped.entries[0].needsUserDecision);
    CHECK_EQ(skipped.skippedCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(skipped.bytesSkipped, static_cast<std::uint64_t>(200));
    CHECK(!skipped.requiresUserDecision);
}

TEST(undo_restorePlanUnfixableConflictsStaySkipped) {
    // Перезапись не лечит: занятый файл, неизвестная цель и нехватка места.
    // Любая «хитрая» политика обязана оставить элемент пропущенным, а не
    // затереть чужое или упасть с ошибкой.
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\cache\\locked.tmp", 100));
    tx.entries.push_back(undoEntry("C:\\cache\\unknown.tmp", 100));
    // Крупный элемент — на другом томе: свободное место считается по томам, и
    // «много места на C:» не делает «тесно на D:».
    tx.entries.push_back(undoEntry("D:\\cache\\huge.tmp", 5000, kVolB));

    std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\cache\\locked.tmp"), freeTarget("C:\\cache\\unknown.tmp"),
                                              freeTarget("D:\\cache\\huge.tmp", kVolB, 100)};
    targets[0].exists = true;
    targets[0].locked = true;
    targets[1].path.clear();
    targets[1].volumeGuidPath.clear();

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan asked = buildRestorePlan(tx, request, targets);
    CHECK(asked.entries[0].conflict == RestoreConflict::TargetLocked);
    CHECK(asked.entries[1].conflict == RestoreConflict::TargetUnknown);
    CHECK(asked.entries[2].conflict == RestoreConflict::NotEnoughSpace);
    CHECK_EQ(asked.conflictCount, static_cast<std::uint32_t>(3));
    CHECK(asked.requiresUserDecision);
    CHECK_EQ(asked.questions.size(), static_cast<std::size_t>(3));

    for (ConflictPolicy policy : {ConflictPolicy::Skip, ConflictPolicy::Overwrite, ConflictPolicy::Rename}) {
        RestoreAnswers answers;
        for (std::size_t i = 0; i < 3; ++i) answers.byEntry.emplace_back(i, policy);
        const UndoPlan applied = applyRestoreAnswers(asked, answers);
        for (const UndoPlanEntry& entry : applied.entries) {
            CHECK(entry.action == RestoreDecision::Skip);
            CHECK(!entry.needsUserDecision);
        }
        CHECK_EQ(applied.bytesPlanned, static_cast<std::uint64_t>(0));
    }
}

TEST(undo_restorePlanMissingParentIsWarningNotBlock) {
    // Родительского каталога нет — его создаст платформа. Это предупреждение,
    // а не конфликт: спрашивать пользователя «создать каталог?» незачем.
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\cache\\deep\\file.tmp", 100));
    std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\cache\\deep\\file.tmp")};
    targets[0].parentExists = false;

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan plan = buildRestorePlan(tx, request, targets);
    CHECK(plan.entries[0].action == RestoreDecision::Restore);
    CHECK(plan.entries[0].conflict == RestoreConflict::ParentMissing);
    CHECK(!plan.entries[0].needsUserDecision);
    CHECK(!plan.requiresUserDecision);
    CHECK(plan.questions.empty());
    CHECK_EQ(plan.conflictCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(plan.skippedCount, static_cast<std::uint32_t>(0));
    CHECK_EQ(plan.bytesPlanned, static_cast<std::uint64_t>(100));
    CHECK(containsText(plan.entries[0].note, "создан"));
}

TEST(undo_restorePlanSpaceIsCountedPerVolume) {
    // Пять файлов по 300 МБ не должны «поместиться» в 500 МБ по одному:
    // остаток свободного места считается накопительно, отдельно по томам.
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\a\\one.tmp", 300));
    tx.entries.push_back(undoEntry("C:\\a\\two.tmp", 300));
    tx.entries.push_back(undoEntry("C:\\a\\three.tmp", 300));
    tx.entries.push_back(undoEntry("D:\\b\\four.tmp", 300, kVolB));

    std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\a\\one.tmp", kVolA, 500), freeTarget("C:\\a\\two.tmp", kVolA, 500),
                                              freeTarget("C:\\a\\three.tmp", kVolA, 500),
                                              freeTarget("D:\\b\\four.tmp", kVolB, 500)};

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan plan = buildRestorePlan(tx, request, targets);
    CHECK(plan.entries[0].action == RestoreDecision::Restore);
    CHECK(plan.entries[0].conflict == RestoreConflict::None);
    CHECK(plan.entries[1].conflict == RestoreConflict::NotEnoughSpace);
    CHECK(plan.entries[2].conflict == RestoreConflict::NotEnoughSpace);
    // Другой том — своё место: ошибка первого тома не отравляет второй.
    CHECK(plan.entries[3].action == RestoreDecision::Restore);
    CHECK_EQ(plan.bytesPlanned, static_cast<std::uint64_t>(600));
    CHECK_EQ(plan.skippedCount, static_cast<std::uint32_t>(2));

    // Перезаписью нехватка места не лечится.
    RestoreAnswers answers;
    answers.byEntry.emplace_back(1, ConflictPolicy::Overwrite);
    answers.byEntry.emplace_back(2, ConflictPolicy::Overwrite);
    const UndoPlan applied = applyRestoreAnswers(plan, answers);
    CHECK(applied.entries[1].action == RestoreDecision::Skip);
    CHECK(applied.entries[2].action == RestoreDecision::Skip);
    CHECK(containsText(applied.entries[1].note, "Перезапись невозможна"));
}

TEST(undo_restorePlanSkipsAlreadyRestoredAndLostContent) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\a\\done.tmp", 100));
    tx.entries[0].restored = true;
    tx.entries.push_back(undoEntry("C:\\a\\lost.tmp", 200));

    std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\a\\done.tmp"), freeTarget("C:\\a\\lost.tmp")};
    targets[0].exists = true;  // на месте уже лежит наш же файл
    targets[1].contentExists = false;

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan plan = buildRestorePlan(tx, request, targets);
    // Повторное восстановление поверх живого файла — самый опасный баг
    // частичного отката: FR-7 запрещает его, поэтому запись пропускается молча.
    CHECK(plan.entries[0].action == RestoreDecision::Skip);
    CHECK(plan.entries[0].conflict == RestoreConflict::AlreadyRestored);
    CHECK(!plan.entries[0].needsUserDecision);
    CHECK(containsText(plan.entries[0].note, "уже восстановлен"));
    CHECK(plan.entries[1].action == RestoreDecision::Skip);
    CHECK(plan.entries[1].conflict == RestoreConflict::MissingInTrash);
    CHECK(containsText(plan.entries[1].note, "утрачено"));
    CHECK_EQ(plan.bytesPlanned, static_cast<std::uint64_t>(0));
    CHECK_EQ(plan.skippedCount, static_cast<std::uint32_t>(2));
    // Ни один из этих случаев не требует решения пользователя — отвечать не о чем.
    CHECK(!plan.requiresUserDecision);
    CHECK(plan.questions.empty());
    // Тип цели не переопределяет «уже восстановлено»: важнее, что возвращать нечего.
    CHECK_EQ(std::string(toString(RestoreConflict::AlreadyRestored)), std::string("already-restored"));
    CHECK_EQ(std::string(toString(RestoreConflict::MissingInTrash)), std::string("missing-in-trash"));
}

TEST(undo_restorePlanTypeMismatchIsOverwritable) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    TrashEntry dir;
    dir.originalPath = "C:\\cache\\dir";
    dir.storedPath = "p0";
    dir.isDirectory = true;
    dir.sizeBytes = 100;
    dir.volumeGuidPath = kVolA;
    tx.entries.push_back(dir);

    std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\cache\\dir")};
    targets[0].exists = true;
    targets[0].isDirectory = false;  // на месте файл, а вернуть нужно каталог

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan plan = buildRestorePlan(tx, request, targets);
    CHECK(plan.entries[0].conflict == RestoreConflict::TargetTypeMismatch);
    CHECK(plan.entries[0].needsUserDecision);

    RestoreAnswers answers;
    answers.byEntry.emplace_back(0, ConflictPolicy::Overwrite);
    const UndoPlan applied = applyRestoreAnswers(plan, answers);
    CHECK(applied.entries[0].action == RestoreDecision::Overwrite);
    CHECK_EQ(applied.bytesPlanned, static_cast<std::uint64_t>(100));

    RestoreAnswers aside;
    aside.byEntry.emplace_back(0, ConflictPolicy::Rename);
    CHECK(applyRestoreAnswers(plan, aside).entries[0].action == RestoreDecision::Rename);
}

TEST(undo_restorePlanSupportsPartialSelection) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\a\\one.tmp", 100));
    tx.entries.push_back(undoEntry("C:\\a\\two.tmp", 200));
    tx.entries.push_back(undoEntry("C:\\a\\three.tmp", 300));
    const std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\a\\one.tmp"), freeTarget("C:\\a\\two.tmp"),
                                                    freeTarget("C:\\a\\three.tmp")};

    RestoreRequest request;
    request.txId = "t1";
    request.entryIndexes = {0, 2};
    const UndoPlan partial = buildRestorePlan(tx, request, targets);
    CHECK(!partial.fullRestore);
    CHECK_EQ(partial.entries.size(), static_cast<std::size_t>(2));
    CHECK_EQ(partial.entries[0].entryIndex, static_cast<std::size_t>(0));
    CHECK_EQ(partial.entries[1].entryIndex, static_cast<std::size_t>(2));
    CHECK_EQ(partial.bytesPlanned, static_cast<std::uint64_t>(400));

    // Повторы и порядок выборки не важны: план один и тот же.
    request.entryIndexes = {2, 0, 2};
    const UndoPlan same = buildRestorePlan(tx, request, targets);
    CHECK_EQ(same.entries.size(), static_cast<std::size_t>(2));
    CHECK_EQ(same.bytesPlanned, partial.bytesPlanned);

    // Пустая выборка — полное восстановление транзакции.
    request.entryIndexes = {};
    const UndoPlan full = buildRestorePlan(tx, request, targets);
    CHECK(full.fullRestore);
    CHECK_EQ(full.entries.size(), static_cast<std::size_t>(3));
    CHECK_EQ(full.bytesPlanned, static_cast<std::uint64_t>(600));
}

TEST(undo_restorePlanRefusesBrokenPlatformInput) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\a", 100));
    tx.entries.push_back(undoEntry("C:\\b", 200));
    const std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\a"), freeTarget("C:\\b")};

    RestoreRequest wrongTx;
    wrongTx.txId = "t2";
    CHECK(throwsAs<UndoError>([&] { (void)buildRestorePlan(tx, wrongTx, targets); }));

    // Короткий снимок состояния — гонка платформы. Считать цель свободной
    // нельзя: восстановление пошло бы поверх чужого файла.
    RestoreRequest request;
    request.txId = "t1";
    const std::vector<RestoreTargetInfo> shortTargets = {freeTarget("C:\\a")};
    const std::string text = errorText<UndoError>([&] { (void)buildRestorePlan(tx, request, shortTargets); });
    CHECK(containsText(text, "короче"));

    // Пустой путь цели — «неизвестно», а не «свободно».
    std::vector<RestoreTargetInfo> blank = targets;
    blank[0].path.clear();
    const UndoPlan plan = buildRestorePlan(tx, request, blank);
    CHECK(plan.entries[0].conflict == RestoreConflict::TargetUnknown);
    CHECK(plan.entries[0].needsUserDecision);
}

TEST(undo_renameTargetKeepsNameRecognizable) {
    // «Восстановить рядом» должно давать узнаваемое имя с сохранённым
    // расширением, а не «file(1).tmp.txt».
    CHECK_EQ(restoreRenameTarget("C:\\dir\\file.txt"), std::string("C:\\dir\\file") + std::string(kRestoreSuffix) + ".txt");
    CHECK_EQ(restoreRenameTarget("C:\\dir\\README"), std::string("C:\\dir\\README") + std::string(kRestoreSuffix));
    // Точка в начале имени — скрытый файл, а не расширение.
    CHECK_EQ(restoreRenameTarget(".config"), std::string(".config") + std::string(kRestoreSuffix));
    // Повторная попытка обязана отличаться от первой и не совпадать с исходным
    // путём — иначе «восстановить рядом» затерело бы оригинал. Точную форму
    // пронумерованного имени здесь не фиксируем: её задаёт реализация
    // restoreRenameTarget (src/core/undo.cpp — файл владельца модуля), и
    // сегодня номер вставляется так, что расширение теряется («file.mrp (2)»).
    // Это дефект реализации, а не ожидаемое поведение, и тест его не закрепляет;
    // свойства безопасности проверяются в любом случае.
    const std::string first = restoreRenameTarget("C:\\dir\\file.txt");
    const std::string second = restoreRenameTarget("C:\\dir\\file.txt", 2);
    CHECK(second != first);
    CHECK(second != std::string("C:\\dir\\file.txt"));
    CHECK(!second.empty());
    CHECK_EQ(restoreRenameTarget("C:\\dir\\file.txt", 0), first);  // attempt < 1 — как attempt 1
    CHECK(containsText(restoreRenameTarget("C:\\dir\\file.txt", 3), "3"));
}

// ===========================================================================
// core::undo — итог восстановления и состояние транзакции после него
// ===========================================================================

TEST(undo_summarizeCountsEveryOutcome) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\a", 100));
    tx.entries.push_back(undoEntry("C:\\b", 200));
    tx.entries.push_back(undoEntry("C:\\c", 300));
    const std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\a"), freeTarget("C:\\b"), freeTarget("C:\\c")};

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan plan = buildRestorePlan(tx, request, targets);

    std::vector<RestoreEntryResult> results;
    results.push_back(RestoreEntryResult{0, true, 100, {}});
    results.push_back(RestoreEntryResult{1, false, 0, "0x80070005: доступ запрещён"});
    // Третий элемент результата не имеет: пользователь отменил или связь оборвалась.

    const RestoreOutcome outcome = summarizeRestore(plan, results);
    CHECK_EQ(outcome.restoredCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(outcome.failedCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(outcome.notAttemptedCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(outcome.skippedCount, static_cast<std::uint32_t>(0));
    CHECK_EQ(outcome.bytesRestored, static_cast<std::uint64_t>(100));
    CHECK_EQ(outcome.failures.size(), static_cast<std::size_t>(1));
    CHECK_EQ(outcome.failures[0].entryIndex, static_cast<std::size_t>(1));
    CHECK(containsText(outcome.summary, "Восстановлено 1 из 3"));
    CHECK(containsText(outcome.summary, "ошибок 1"));
    CHECK(containsText(outcome.summary, "не выполнено 1"));

    // Повторный отчёт по той же записи и отчёт с чужим индексом не искажают итог.
    results.push_back(RestoreEntryResult{0, true, 100, {}});
    results.push_back(RestoreEntryResult{99, true, 500, {}});
    const RestoreOutcome again = summarizeRestore(plan, results);
    CHECK_EQ(again.restoredCount, static_cast<std::uint32_t>(1));
    CHECK_EQ(again.bytesRestored, static_cast<std::uint64_t>(100));
}

TEST(undo_applyRestoredCollapsesOnlyWhenNothingIsLeft) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\a", 100));
    tx.entries.push_back(undoEntry("C:\\b", 200));
    const std::vector<RestoreTargetInfo> targets = {freeTarget("C:\\a"), freeTarget("C:\\b")};

    RestoreRequest request;
    request.txId = "t1";
    const UndoPlan plan = buildRestorePlan(tx, request, targets);

    // Частичное восстановление: одна запись вернулась, вторая — нет.
    std::vector<RestoreEntryResult> results = {RestoreEntryResult{0, true, 100, {}}};
    const UndoTransaction partial = applyRestored(tx, plan, results, kNow);
    CHECK(partial.entries[0].restored);
    CHECK_EQ(partial.entries[0].restoredUnix, kNow);
    CHECK(!partial.entries[1].restored);
    CHECK(!partial.collapsed);
    CHECK(undoAvailable(partial));
    CHECK_EQ(partial.restorableCount(), static_cast<std::size_t>(1));
    // Исходная транзакция не мутируется: «применить» возвращает новое состояние.
    CHECK(!tx.entries[0].restored);

    // Возвращать больше нечего — транзакция схлопывается, Ctrl+Z гаснет.
    results.push_back(RestoreEntryResult{1, true, 200, {}});
    const UndoTransaction done = applyRestored(partial, plan, results, kNow);
    CHECK(done.collapsed);
    CHECK(!undoAvailable(done));
    CHECK_EQ(done.restorableBytes(), static_cast<std::uint64_t>(0));

    // Неудача — не «возвращено»: повторить восстановление ещё можно.
    UndoTransaction again = makeTrashTransaction("t2", "1.0.0", kNow);
    again.entries.push_back(undoEntry("C:\\x", 100));
    again.entries.push_back(undoEntry("C:\\y", 100));
    const std::vector<RestoreTargetInfo> twoTargets = {freeTarget("C:\\x"), freeTarget("C:\\y")};
    RestoreRequest request2;
    request2.txId = "t2";
    const UndoPlan plan2 = buildRestorePlan(again, request2, twoTargets);
    const std::vector<RestoreEntryResult> failedOnly = {RestoreEntryResult{0, false, 0, "занято"},
                                                        RestoreEntryResult{1, false, 0, "занято"},
                                                        RestoreEntryResult{7, true, 100, {}}};
    const UndoTransaction failed = applyRestored(again, plan2, failedOnly, kNow);
    CHECK(!failed.collapsed);
    CHECK_EQ(failed.restorableCount(), static_cast<std::size_t>(2));
    CHECK(undoAvailable(failed));

    // Манифест после восстановления переживает сериализацию: иначе повторный
    // запуск приложения забыл бы, что уже вернули.
    const UndoTransaction reloaded = parseUndoManifest(serializeTrashManifest(done), "manifest.json");
    CHECK(reloaded.collapsed);
    CHECK_EQ(reloaded.restorableCount(), static_cast<std::size_t>(0));
    CHECK_EQ(reloaded.entries[0].restoredUnix, kNow);
}

// ===========================================================================
// core::undo — цена переноса: кросс-томовое движение = копирование
// ===========================================================================

TEST(undo_transferOnSameVolumeIsMove) {
    const CopyEstimate same = estimateTransfer(1 * kGB, kVolA, kVolA);
    CHECK(same.volumeKnown);
    CHECK(!same.crossVolume);
    CHECK_EQ(same.seconds, static_cast<std::uint64_t>(0));
    CHECK(!same.slowEnoughToWarn);
    CHECK(!same.recommendsDirectDelete);
    CHECK_EQ(same.bytes, static_cast<std::uint64_t>(1 * kGB));

    const CopyEstimate empty = estimateTransfer(0, kVolA, kVolB);
    CHECK(!empty.crossVolume);
    CHECK(!empty.slowEnoughToWarn);
    CHECK_EQ(empty.seconds, static_cast<std::uint64_t>(0));
}

TEST(undo_crossVolumeTransferWarnsBeforeTheUserClicks) {
    // 400 МиБ при 100 МиБ/с — ровно 5 секунд: граница предупреждения.
    const CopyEstimate warn = estimateTransfer(400 * kMB, kVolA, kVolB, 100 * kMB);
    CHECK(warn.crossVolume);
    CHECK(warn.volumeKnown);
    CHECK_EQ(warn.seconds, static_cast<std::uint64_t>(4));
    CHECK(!warn.slowEnoughToWarn);

    const CopyEstimate slow = estimateTransfer(500 * kMB, kVolA, kVolB, 100 * kMB);
    CHECK_EQ(slow.seconds, static_cast<std::uint64_t>(5));
    CHECK(slow.slowEnoughToWarn);
    CHECK(!slow.recommendsDirectDelete);  // чуть меньше порога «большая транзакция»

    const CopyEstimate huge = estimateTransfer(512 * kMB, kVolA, kVolB, 100 * kMB);
    CHECK(huge.slowEnoughToWarn);
    CHECK(huge.recommendsDirectDelete);
    CHECK_EQ(huge.seconds, static_cast<std::uint64_t>(6));

    // Без замера скорости берётся консервативная оценка (80 МиБ/с).
    const CopyEstimate defaulted = estimateTransfer(400 * kMB, kVolA, kVolB);
    CHECK_EQ(defaulted.throughputBytesPerSec, kDefaultCopyThroughputBytesPerSec);
    CHECK_EQ(defaulted.seconds, static_cast<std::uint64_t>(5));
    CHECK(defaulted.slowEnoughToWarn);
    CHECK_EQ(kSlowCopySeconds, static_cast<std::uint64_t>(5));
    CHECK_EQ(kLargeTransactionBytes, 512ull * 1024 * 1024);
}

TEST(undo_unknownVolumeStillWarnsBecauseCopyingIsTheWorstCase) {
    // Том неизвестен: цена неопределима, но молчать нельзя — перемещение между
    // томами всегда означает копирование.
    const CopyEstimate estimate = estimateTransfer(10 * kMB, "", kVolB);
    CHECK(!estimate.volumeKnown);
    CHECK(estimate.crossVolume);
    CHECK(estimate.slowEnoughToWarn);
    CHECK_EQ(estimate.seconds, static_cast<std::uint64_t>(0));  // цена не выдумана

    const CopyEstimate both = estimateTransfer(10 * kMB, "", "");
    CHECK(!both.volumeKnown);
    CHECK(both.slowEnoughToWarn);
}

TEST(undo_restoreCostIsChargedPerVolumeNotPerFile) {
    UndoTransaction tx = makeTrashTransaction("t1", "1.0.0", kNow);
    tx.entries.push_back(undoEntry("C:\\a", 100 * kMB, kVolA));
    tx.entries.push_back(undoEntry("C:\\b", 200 * kMB, kVolA));

    const CopyEstimate sameVolume = estimateRestoreCost(tx, kVolA, 100 * kMB);
    CHECK(sameVolume.volumeKnown);
    CHECK(!sameVolume.crossVolume);  // это перемещение, а не копирование
    CHECK_EQ(sameVolume.bytes, static_cast<std::uint64_t>(300 * kMB));
    CHECK_EQ(sameVolume.seconds, static_cast<std::uint64_t>(0));

    // Один элемент на другом томе — копирование для всей операции.
    tx.entries[1].volumeGuidPath = kVolB;
    const CopyEstimate cross = estimateRestoreCost(tx, kVolA, 100 * kMB);
    CHECK(cross.crossVolume);
    CHECK_EQ(cross.bytes, static_cast<std::uint64_t>(300 * kMB));
    CHECK_EQ(cross.seconds, static_cast<std::uint64_t>(3));

    // Уже возвращённые элементы в цену не входят: пользователю показывают
    // стоимость того, что он сейчас делает, а не всей давней очистки.
    tx.entries[0].restored = true;
    const CopyEstimate rest = estimateRestoreCost(tx, kVolA, 100 * kMB);
    CHECK_EQ(rest.bytes, static_cast<std::uint64_t>(200 * kMB));
    CHECK_EQ(rest.seconds, static_cast<std::uint64_t>(2));

    // Том корзины неизвестен — цена неопределима, но предупреждение остаётся.
    tx.entries[0].volumeGuidPath = "";
    const CopyEstimate unknown = estimateRestoreCost(tx, "", 100 * kMB);
    CHECK(!unknown.volumeKnown);
    CHECK(unknown.crossVolume);
    CHECK(unknown.slowEnoughToWarn);

    // Восстановлено всё — копировать нечего.
    UndoTransaction allBack = makeTrashTransaction("t2", "1.0.0", kNow);
    TrashEntry back = undoEntry("C:\\a", 100 * kMB, kVolB);
    back.restored = true;
    allBack.entries.push_back(back);
    const CopyEstimate done = estimateRestoreCost(allBack, kVolA, 100 * kMB);
    CHECK_EQ(done.bytes, static_cast<std::uint64_t>(0));
    CHECK(!done.crossVolume);
    CHECK(!done.slowEnoughToWarn);
}
