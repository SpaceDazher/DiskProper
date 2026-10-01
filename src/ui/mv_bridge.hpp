// MrProper — мост модель ↔ представление (SPEC §6.4 «Потоки и отмена»).
//
// ---------------------------------------------------------------------------
// Зачем файл
// ---------------------------------------------------------------------------
//
// §6.4 описывает три требования, которые не выполняются «просто структурой»:
//
//   1) «Один UI-поток (владеет HWND и D2D-ресурсами)» и «UI — без блокировок и
//      I/O» (§6.1). Значит, фон не может трогать ни модель экрана, ни контролы:
//      обход ФС на 60 с не имеет права превращаться в белый список Windows.
//   2) «Счётчик файлов/байт публикуется атомарно, UI читает раз в 100 мс».
//      Кадров прогресса за минуту скана будет десятки тысяч, а показать можно
//      последний: доставлять их все — значит забить очередь сообщений.
//   3) «Результаты не мутируются после публикации: shared_ptr<const ScanResult>».
//
// Мост — единственное место, где эти три требования встречаются: он переносит
// кадр из фонового потока в UI-поток, отдаёт представлению только константные
// снимки и гарантирует, что в пути отрисовки нет ни блокировок, ни I/O.
//
// ---------------------------------------------------------------------------
// Ключевое решение: очередь — это очередь сообщений ОС
// ---------------------------------------------------------------------------
//
// Кадр не кладётся в std::deque под мьютексом. Он аллоцируется и уходит в
// PostMessageW как LPARAM; окно-приёмник (скрытое, невидимое) выполняет его в
// UI-потоке и удаляет. Свойств ровно два, и оба нужны:
//
//   * порядок: сообщения одного потока в очередь одного окна идут в порядке
//     отправки, поэтому «сначала кандидаты, потом результат операции» — правда,
//     а не обещание;
//   * отсутствие блокировок: в мосте вообще нет contended-состояния. Два потока
//     не ждут друг друга нигде: фоновый только аллоцирует и вызывает PostMessage,
//     UI-поток — достаёт и исполняет.
//
// Второе свойство не бесплатно: аллокация на каждый кадр. Поэтому прогресс идёт
// отдельным путём и не аллоцируется впустую (см. «Слияние прогресса»).
//
// ---------------------------------------------------------------------------
// Слияние прогресса: «последний выигрывает»
// ---------------------------------------------------------------------------
//
// Кадры прогресса монотонны (счётчики только растут), поэтому промежуточные
// кадры не несут информации: показывать «прочитано 40 000 файлов» после
// «прочитано 41 000» поздно. Мост держит ОДИН слот последнего кадра плюс флаг
// «просыпаться»: фоновый поток кладёт кадр в слот и просит один сигнал, если
// сигнала ещё не было, UI-поток на сигнал забирает последний кадр и рисует.
//
// Единственный мьютекс в файле — этот слот, и берётся он фоновым потоком не
// чаще раза в options.progressInterval (по умолчанию 100 мс — период из §6.4),
// а UI-потоком — один раз за тик. В пути отрисовки и в доставке наблюдателям
// блокировок нет вообще. Так же сделан и счётчик currentPath в
// engine::ScanProgress; заменить слот на атомарный указатель нельзя — в
// тулчейне проекта (MSVC v142) нет std::atomic<std::shared_ptr>.
//
// ---------------------------------------------------------------------------
// Границы слоёв
// ---------------------------------------------------------------------------
//
// Зависимость ровно одна и только вниз: core. Ни engine, ни scanner, ни
// platform в заголовке нет, и это не перестраховка:
//
//   * src/ui линкуется только с core и platform (см. src/ui/CMakeLists.txt),
//     поэтому мост, зовущий engine::ScanCoordinator, собрался бы, но не
//     слинковался бы в mrproper.exe;
//   * экраны (view_disks, view_cleanup) по условию задач не знают про engine —
//     они получают модель отсюда, готовым снимком, и не должны узнавать, какой
//     модуль её собрал;
//   * поэтому приведение типов движка в кадры моста делает вызывающий (тот,
//     кто и так линкует engine) обычным postSnapshot(...) — см. таблицу ниже.
//
// Исключение из «только core::log» — StartupFeed в конце файла: его payload'ы
// по определению core::DiskInventory и core::RuleSet, и подменить их собственными
// типами моста нельзя (их читают экраны и отчёт). platform и engine там по-прежнему
// не нужны: обход дисков и чтение набора правил живут в .cpp.
//
// Таблица «кто что читает» (для вызывающего, который публикует):
//
//   EventKind::Inventory        → DisksScreen::publishInventory
//   EventKind::Disks            → DisksScreen::publishDisks
//   EventKind::ScanProgress     → CleanupScreen::publishScanProgress
//   EventKind::Candidates       → CleanupScreen::publishCandidates
//   EventKind::OperationResult  → CleanupScreen::publishOperationResult
//   EventKind::Phase            → CleanupScreen::beginScan/endScan/fail
//   EventKind::UndoAvailable    → CleanupScreen::setUndoAvailable
//   EventKind::RuleSet          → SettingsScreen::publishRuleSet
//   EventKind::Notice, Error    → строка состояния любого экрана
//
// Наблюдатель достаёт payload через Event::as<T>(): тип проверяется, чужой тип
// даёт nullptr, а не reinterpret_cast с непредсказуемой памятью.
//
// ---------------------------------------------------------------------------
// Правила этого файла
// ---------------------------------------------------------------------------
//
//   * Поток: publish/post* зовутся из ЛЮБОГО потока; subscribe, pump и всё
//     остальное — только из UI-потока. Мост это проверяет и считает нарушения,
//     а не полагается на «договорённость».
//   * Исключение не пересекает границу Win32: обработчик скрытого окна и
//     вызов наблюдателя обёрнуты в try/catch (§5 «устойчивость»).
//   * Никаких блокировок и никакого I/O в пути доставки: доставка — это вызов
//     наблюдателя, который пересчитывает счётчики и просит перерисовку.
//   * Каждый отказ Win32 (не создалось окно, не ушла posted-команда) пишется в
//     журнал с кодом ошибки (§5, §12).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include "core/disk_model.hpp"
#include "core/rules.hpp"

namespace mrproper::ui::mv {

// ---------------------------------------------------------------------------
// Константы §6.4
// ---------------------------------------------------------------------------

// «UI читает раз в 100 мс» — период из спецификации. За ним стоит не оптимизация,
// а человек: цифры на экране не меняются быстрее, чем их успевают увидеть, а
// читать чаще — значит чаще будить UI-поток.
inline constexpr std::chrono::milliseconds kProgressInterval{100};

// Потолок очереди кадров. Снимок (кандидаты, инвентаризация) — это результат
// работы, а не поток событий: очередь физически не должна расти от них, но если
// публикующий код всё же публикует пачкой, мост обязан перестать принимать
// нетерминальные кадры и сказать об этом, а не расти вместе с ними.
inline constexpr std::size_t kMaxPendingEvents = 256;

// ---------------------------------------------------------------------------
// Виды кадров
// ---------------------------------------------------------------------------

// Что пришло из модели. Список закрыт: добавлять вид — значит дописывать
// обработку во всех наблюдателях, а молчаливый «неизвестный кадр» хуже явного
// отказа. Unknown не существует намеренно: неизвестный номер приходит из
// испорченной памяти, и разбираться с таким должен журнал, а не UI.
enum class EventKind : std::uint8_t {
    Inventory,        ///< снимок инвентаризации (карта дисков, FR-1)
    Disks,            ///< список дисков без обёртки инвентаризации
    ScanProgress,     ///< прогресс скана (сливается, доставляется отдельно)
    Candidates,       ///< снимок кандидатов очистки (FR-3, FR-5)
    Phase,            ///< смена фазы работы экрана
    OperationResult,  ///< результат одной операции удаления (FR-6)
    UndoAvailable,    ///< появилась или исчезла возможность отмены (FR-7)
    Notice,           ///< строка в строку состояния
    Error,            ///< отказ, который обязан быть виден (§5, §12)
    RuleSet,          ///< набор правил очистки (FR-4, ADR-008)
};

// Имя вида для журнала: читается глазами в логе и не переводится.
[[nodiscard]] const char* eventKindName(EventKind kind) noexcept;

// Кадр, доставленный наблюдателю.
//
// Кадр — значение: он не ссылается на модель экрана и не даёт ей ничего
// мутировать. Данные лежат внутри как shared_ptr<const T>, и это тот самый тип
// из §6.4: «результаты не мутируются после публикации». Само значение
// копируется целиком (несколько счётчиков и два указателя), поэтому размер
// кадра известен на этапе компиляции.
class Event {
public:
    Event() = default;

    [[nodiscard]] EventKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    // Момент публикации, unix-время. 0 — платформа время не передала; в журнале
    // это честнее, чем выдуманное «сейчас».
    [[nodiscard]] std::int64_t atUnix() const noexcept { return atUnix_; }
    [[nodiscard]] bool empty() const noexcept { return payload_ == nullptr; }

    // Снимок известного типа. nullptr, если тип не тот: подменить payload
    // другого типа нельзя, а вот ЗАБЫТЬ, что именно пришло, — можно, и это
    // должно быть видно проверкой, а не повреждением памяти.
    template <class T>
    [[nodiscard]] std::shared_ptr<const T> as() const noexcept {
        if (type_ != std::type_index(typeid(T))) return nullptr;
        return std::static_pointer_cast<const T>(payload_);
    }

    // Имя типа payload для журнала, когда as<T>() вернул nullptr.
    [[nodiscard]] const char* payloadTypeName() const noexcept;

    // Создать кадр из готового снимка. Тип T выводится из payload.
    template <class T>
    [[nodiscard]] static Event snapshot(EventKind kind, std::shared_ptr<const T> payload,
                                        std::uint64_t sequence, std::int64_t atUnix = 0) {
        Event event;
        event.kind_ = kind;
        event.sequence_ = sequence;
        event.atUnix_ = atUnix;
        event.type_ = std::type_index(typeid(T));
        event.payload_ = std::move(payload);
        return event;
    }

    // Создать кадр из значения: мост сам замораживает его в shared_ptr<const T>.
    // Это и есть «публикация» из §6.4 — после freeze() объект нельзя изменить,
    // и представление получает ровно тот снимок, который видел сборщик.
    template <class T>
    [[nodiscard]] static Event value(EventKind kind, T&& object, std::uint64_t sequence,
                                     std::int64_t atUnix = 0) {
        using Held = std::remove_cv_t<std::remove_reference_t<T>>;
        return snapshot(kind, std::make_shared<const Held>(std::forward<T>(object)), sequence, atUnix);
    }

private:
    EventKind kind_{};
    std::uint64_t sequence_{};
    std::int64_t atUnix_{};
    std::type_index type_{typeid(void)};
    std::shared_ptr<const void> payload_;
};

// ---------------------------------------------------------------------------
// Кадры
// ---------------------------------------------------------------------------

// Прогресс: ровно те числа, которые рисует полоса и строка состояния (§6.4).
//
// Это НЕ снимок прогресса движка: мост не знает про engine и не должен про него
// знать. Приводящий переносит счётчики сюда один раз, UI читает готовые
// значения и не ходит в счётчики — поэтому «цифры на экране» и «цифры в отчёте»
// расходятся только если расходятся в приводящем коде, и это видно сразу.
struct ProgressFrame {
    std::uint64_t generation{};  // номер прогона: «другой скан» без сравнения содержимого
    std::uint64_t sequence{};    // номер кадра в мосте: «кадр новее» без сравнения счётчиков
    std::uint64_t itemsDone{};
    std::uint64_t itemsTotal{};  // 0 — объём неизвестен заранее (обход ФС не знает)
    std::uint64_t bytesDone{};
    std::uint64_t bytesTotal{};  // 0 — то же
    std::size_t tasksDone{};
    std::size_t tasksTotal{};
    std::size_t workersActive{};
    std::size_t workersTotal{};
    std::string phase;         ///< уже готовая строка фазы (ui::locale, §5 — не литерал)
    std::string currentPath;   ///< где идёт обход; меняет координатор, не обход
    std::chrono::milliseconds elapsed{};

    // Доля готового в 0.0…1.0. Без знаменателя возвращает 0.0, а не деление на
    // ноль и не 1.0: индикатор без знаменателя обязан показывать неопределённое
    // состояние (indeterminate), иначе «сканирование ничего не нашло» и «ещё не
    // начато» станут неразличимы.
    [[nodiscard]] double fraction() const noexcept;
    [[nodiscard]] bool indeterminate() const noexcept;
    // То же, что fraction(), но с зажимом в [0,1]: полоса прогресса не должна
    // выехать за края из-за того, что счётчик разошёлся с оценкой.
    [[nodiscard]] double clampedFraction() const noexcept;
    [[nodiscard]] std::string toText() const;
};

// Короткий кадр для всего, что не прогресс и не снимок: строка состояния,
// результат операции, ошибка, признак доступности отмены.
struct StatusFrame {
    std::string text;           ///< готовая строка интерфейса (ui::locale)
    std::uint64_t freedBytes{}; ///< освобождено операцией (FR-6)
    std::uint64_t generation{};
    bool ok{true};              ///< false — это отказ, а не успех (EventKind::Error)
    std::int64_t atUnix{};
};

// ---------------------------------------------------------------------------
// Наблюдатели
// ---------------------------------------------------------------------------

// Счётчики моста. Читаются из любого потока (атомики) — их берёт баг-репорт и
// журнал, а не отрисовка.
struct BridgeStats {
    std::uint64_t posted{};         ///< принято к маршалингу
    std::uint64_t delivered{};      ///< доставлено наблюдателям в UI-потоке
    std::uint64_t dropped{};        ///< не поставлено: очередь полна или PostMessage не удался
    std::uint64_t throttled{};      ///< прогресс, снятый троттлингом (чаще 100 мс)
    std::uint64_t coalesced{};      ///< прогресс, слитый с предыдущим в один кадр
    std::uint64_t progressDelivered{};  ///< кадров прогресса отдано наблюдателям
    std::uint64_t misthreaded{};    ///< попытка доставить или подписаться не из UI-потока
    std::uint64_t observerErrors{}; ///< исключения из наблюдателей (поглочены, с записью в журнал)
    std::uint64_t sinkCreated{};    ///< 1 — окно-приёмник создано, 0 — доставка внешняя
    [[nodiscard]] std::string toText() const;
};

// Наблюдатель моста. Реализуется экраном или моделью экрана.
//
// Методы вызываются ТОЛЬКО в UI-потоке, внутри доставки, и обязаны быть
// короткими: там можно менять счётчики модели и просить перерисовку, но нельзя
// ждать, читать файлы и звать обратно в модель-движок (§6.1).
class Observer {
public:
    virtual ~Observer();

    // Кадр с payload. Обычные снимки, результаты и строки состояния.
    virtual void onFrame(const Event& event);
    // Прогресс. Приходит отдельно от onFrame и уже слитый: за один тик — не
    // более одного кадра, сколько бы их ни накопилось в фоне.
    virtual void onProgress(const ProgressFrame& frame);
    // Конец тика: после того как доставлено всё накопленное. Нужен для счётчиков
    // «кадров в секунду» и для отладки задержки; писать в UI здесь нельзя.
    virtual void onTick(const BridgeStats& stats);
};

// ---------------------------------------------------------------------------
// Диспетчер: перенос работы в UI-поток
// ---------------------------------------------------------------------------

// Абстракция «выполнить в UI-потоке». Существует, чтобы мост проверялся без
// окна (тесты, headless-CLI) и чтобы смена способа переноса не трогала ни
// публикацию, ни наблюдателей.
class Dispatcher {
public:
    virtual ~Dispatcher();

    // Находимся ли мы в UI-потоке. Мост запоминает поток создания и сверяется
    // с ним: «UI-поток» — это не «какой угодно», а тот, в котором жив HWND.
    [[nodiscard]] virtual bool onUiThread() const noexcept = 0;

    // Поставить работу в UI-поток. false — доставить не удалось (окно
    // уничтожено, не хватило памяти): вызывающий обязан это заметить, потерянный
    // молча кадр выглядит как «интерфейс завис».
    virtual bool post(std::function<void()>&& job) noexcept = 0;
};

// Перенос через PostMessageW в окно-приёмник. Рабочая схема приложения.
class PostMessageDispatcher final : public Dispatcher {
public:
    // window — HWND окна-приёмника (void*, чтобы заголовок оставался без Win32).
    // Ни nullptr, ни уничтоженное окно молча не теряют кадры: post() вернёт
    // false, мост посчитает это в dropped и напишет в журнал один раз.
    explicit PostMessageDispatcher(void* window, std::uint32_t uiThreadId) noexcept;
    ~PostMessageDispatcher() override;

    PostMessageDispatcher(const PostMessageDispatcher&) = delete;
    PostMessageDispatcher& operator=(const PostMessageDispatcher&) = delete;
    PostMessageDispatcher(PostMessageDispatcher&&) = delete;
    PostMessageDispatcher& operator=(PostMessageDispatcher&&) = delete;

    [[nodiscard]] bool onUiThread() const noexcept override;
    bool post(std::function<void()>&& job) noexcept override;

    // Почему последняя отправка не удалась (0 — отправок не было). В журнал
    // идёт вместе с кодом: «кадр потерян» без причины не чинится.
    [[nodiscard]] std::uint32_t lastError() const noexcept;
    [[nodiscard]] std::uint64_t failed() const noexcept;
    [[nodiscard]] void* window() const noexcept { return window_; }

private:
    void* window_{nullptr};
    std::uint32_t uiThreadId_{0};
    std::uint32_t lastError_{0};
    std::atomic<std::uint64_t> failed_{0};
};

// Перенос «здесь и сейчас». Для тестов и headless-режима: в UI нет окон, и
// доставка обязана быть синхронной, иначе тест проверяет не мост, а таймер.
// В приложении не использовать: единственный UI-поток — это тот, где жив HWND.
class InlineDispatcher final : public Dispatcher {
public:
    explicit InlineDispatcher(std::uint32_t uiThreadId) noexcept;
    ~InlineDispatcher() override;

    [[nodiscard]] bool onUiThread() const noexcept override;
    bool post(std::function<void()>&& job) noexcept override;

    [[nodiscard]] std::uint64_t posted() const noexcept;

private:
    std::uint32_t uiThreadId_{0};
    std::atomic<std::uint64_t> posted_{0};
};

// ---------------------------------------------------------------------------
// Мост
// ---------------------------------------------------------------------------

// Связывает фоновые публикации с UI-потоком.
//
// Жизненный цикл (в порядке, который менять нельзя):
//   1) мост создаётся в UI-потоке — он запоминает его идентификатор и, если не
//      передан чужой диспетчер, создаёт скрытое окно-приёмник;
//   2) экраны подписываются (subscribe) — тоже в UI-потоке;
//   3) фоновые потоки публикуют (post*) — из любого потока и в любой момент;
//   4) мост уничтожается в UI-потоке: окно-приёмник снимается, таймер
//      выключается, наблюдатели отпускаются.
//
// Пункт 4 требует, чтобы перед разрушением моста фоновые потоки были
// остановлены и присоединены (это делает владелец прогона: у движка — деструктор
// координатора). Публиковать в разрушенный или разрушаемый мост нельзя, и мост
// не берёт на себя проверку этого: гонка с разрушением — ошибка владения, а не
// повод вставать блокировкой на пути доставки.
//
// Ни один шаг не бросает исключений наружу, и ни один не ждёт фоновые потоки:
// пока идёт скан, закрытие окна не виснет.
class ModelViewBridge {
public:
    struct Options {
        // Окно-родитель для приёмника (HWND). nullptr — окно верхнего уровня.
        // Нужен, чтобы окно-приёмник не мигало в списке Alt+Tab.
        void* ownerWindow{nullptr};
        // Создавать ли своё окно-приёмник. false — диспетчер передаётся
        // снаружи (тесты, headless). В приложении — true.
        bool createSink{true};
        // Троттлинг прогресса (§6.4 — 100 мс).
        std::chrono::milliseconds progressInterval{kProgressInterval};
        // Потолок очереди нетерминальных кадров (kMaxPendingEvents).
        std::size_t maxPendingEvents{kMaxPendingEvents};
        // Периодический тик по таймеру окна. Нужен как страховка: если сигнал о
        // прогрессе потерялся (PostMessage не удался), прогресс всё равно
        // доедет следующим тиком. 0 — только по сигналам.
        std::chrono::milliseconds tickInterval{kProgressInterval};
        // Писать ли маршалинг в core::log. В приложении полезно (по «скан
        // длился вечно» видно, доехал ли кадр), в тестах — шум.
        bool logTraffic{false};
    };

    // Создать с собственным окном-приёмником (приложение).
    explicit ModelViewBridge(Options options = {});
    // Создать с чужим диспетчером (тесты, headless). Окно не создаётся, тик
    // вызывающий pumps() сам.
    explicit ModelViewBridge(std::unique_ptr<Dispatcher> dispatcher, Options options = {});
    ~ModelViewBridge();

    ModelViewBridge(const ModelViewBridge&) = delete;
    ModelViewBridge& operator=(const ModelViewBridge&) = delete;
    ModelViewBridge(ModelViewBridge&&) = delete;
    ModelViewBridge& operator=(ModelViewBridge&&) = delete;

    // Живой ли мост: окно создано и диспетчер на месте. После destroy() мост
    // можно пересоздать — публиковать в него больше нельзя.
    [[nodiscard]] bool ready() const noexcept;
    // HWND окна-приёмника (nullptr, если окно не создавалось). Нужен каркасу,
    // если он захочет вызвать pump() из своего таймера.
    [[nodiscard]] void* sinkWindow() const noexcept;

    // --- Поток --------------------------------------------------------------
    // Находимся ли в UI-потоке. Всё, кроме post*, обязано быть здесь.
    [[nodiscard]] bool onUiThread() const noexcept;
    [[nodiscard]] std::uint32_t uiThreadId() const noexcept;

    // --- Наблюдатели (только UI-поток) --------------------------------------
    //
    // Подписка во время доставки не применяется сразу: изменения копятся и
    // вступают в силу после того, как доставка закончится. Иначе наблюдатель,
    // отписавший себя из своего же onFrame(), инвалидировал бы итерацию.
    bool subscribe(std::shared_ptr<Observer> observer);
    bool unsubscribe(const std::shared_ptr<Observer>& observer);
    void clearObservers();
    [[nodiscard]] std::size_t observerCount() const noexcept;

    // --- Публикация (любой поток) -------------------------------------------
    //
    // critical=true — кадр, который нельзя терять: результат операции, конец
    // работы, ошибка. Он проходит мимо потолка очереди (очередь сообщений ОС
    // не переполняется, а терять конец работы нельзя), и это единственное
    // место, где потолок обходится.
    template <class T>
    bool postSnapshot(EventKind kind, std::shared_ptr<const T> payload, bool critical = false) {
        return postEvent(Event::snapshot(kind, std::move(payload), nextSequence()), critical);
    }

    // Опубликовать значение: мост замораживает его в shared_ptr<const T>.
    template <class T>
    bool postValue(EventKind kind, T&& value, bool critical = false) {
        using Held = std::remove_cv_t<std::remove_reference_t<T>>;
        return postSnapshot(kind, std::make_shared<const Held>(std::forward<T>(value)), critical);
    }

    // Короткий кадр: строка состояния, результат операции, ошибка.
    bool postStatus(EventKind kind, StatusFrame frame, bool critical = false);
    bool postStatus(EventKind kind, std::string text, bool critical = false);

    // Прогресс. Возвращает false только если кадр не поставлен по существу
    // (мост мёртв или PostMessage не удался); «снято троттлингом» — это true:
    // кадр учтён, просто предыдущий ещё не показан.
    bool postProgress(ProgressFrame frame);

    // --- Доставка (только UI-поток) -----------------------------------------
    //
    // Отдать накопленный прогресс наблюдателям. Кадры из очереди сообщений ОС
    // исполняются обработчиком окна сами; здесь — только то, что слито в слот
    // прогресса, и хвост доставки. Вызывать из своего таймера не обязательно:
    // окно-приёмник тикает само (options.tickInterval), а пустой pump() без
    // накопленного прогресса ничего не делает.
    void pump();

    // Сколько кадров сейчас ждут в очереди сообщений ОС (оценка: счётчик
    // поставленных минус доставленных). Для журнала и телеметрии.
    [[nodiscard]] std::uint64_t pending() const noexcept;

    // Обнулить счётчики. Только UI-поток; накопленное не трогает.
    void resetStats() noexcept;

    [[nodiscard]] BridgeStats stats() const noexcept;
    // Одна строка для журнала и баг-репорта (FR-8).
    [[nodiscard]] std::string toText() const;

private:
    struct Impl;

    bool postEvent(Event event, bool critical);
    void deliverProgress() noexcept;
    void notify(const Event& event) noexcept;
    void pumpInternal() noexcept;
    void applyObserverChanges() noexcept;
    [[nodiscard]] bool claimUiThread(const char* what) const noexcept;
    void createSink() noexcept;
    void destroySink() noexcept;
    // Монотонный номер кадра (атомарный: публикуют несколько фоновых потоков).
    [[nodiscard]] std::uint64_t nextSequence() noexcept;
    // Unix-время в секундах; 0 — система не дала (журнал честнее выдумки).
    [[nodiscard]] static std::int64_t unixNow() noexcept;
    static void logBridge(std::string_view event, std::string_view message) noexcept;
    static void logWin32Failure(std::string_view event, std::string_view where, unsigned long code) noexcept;
    // Записать в журнал один раз: поток, публикующий по миллиону кадров в
    // секунду, иначе засоряет лог быстрее, чем его читает человек.
    static void logOnce(std::string_view event, std::string_view message, std::atomic<int>& flag) noexcept;

    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Стартовая раздача: кому вообще есть что показать
// ---------------------------------------------------------------------------
//
// Проблема, которую эта часть решает, не в мосте, а в окне: пять экранов
// написаны, но данные на них приходят из фонового потока, а этот поток
// никто не поднимал. Модель молча остаётся пустой, и «экран умеет рисовать
// состояние» превращается в «экран умеет рисовать пустоту» — ровно тот отказ,
// который в этой волне и чинился.
//
// Поэтому раздача данных на старте живёт здесь, а не в оболочке:
//
//   1) StartupFeed поднимает ОДИН фоновый поток на процесс: он читает набор
//      правил с диска и обходит устройства (оба шага — I/O, а §6.1 запрещает
//      I/O в UI-потоке);
//   2) результат уходит в тот же ModelViewBridge одним снимком на вид данных —
//      то есть тем же путём «фон → UI», который уже описан в шапке файла;
//   3) экран, подписанный через ScreenEndpoint, получает сообщение kFeedMessage
//      в своё окно и разбирает кадр уже в UI-потоке.
//
// Экран подписывается сам в своём create() и не зависит ни от оболочки, ни от
// того, вызвали ли её: пять экранов, созданных в любом порядке, получают данные
// одинаково. Оболочке (владелец app_shell.*) остаётся только создать экраны.
//
// Чего раздача не делает намеренно: она НЕ запускает скан очистки и НЕ удаляет
// ничего. Скан — это минута работы по всему диску, и запускать его без
// человека нельзя; «Очистка» поэтому показывает состояние «скан ещё не
// выполнялся» и кнопку, а не выдуманные кандидаты.

// Сообщение, которым экран забирает кадр из очереди раздачи. WM_APP == 0x8000
// (тот же диапазон, что у kMsgSyncModel в экранах), номер не совпадает ни с
// одним из них: сообщения адресны окну, но одинаковый номер у пяти экранов
// означал бы, что «перерисовать из модели» и «пришёл кадр из фона» —
// одно и то же событие, а это разные вещи с разными последствиями.
inline constexpr std::uint32_t kFeedMessage = 0x8000u + 0x65u;  // WM_APP + 101

// Приёмник кадров на стороне экрана.
//
// Наблюдатель не трогает экран напрямую: он кладёт кадр в свою очередь и
// просит окно сообщением kFeedMessage. Причина — время жизни, а не
// осторожность: мост держит наблюдателя shared_ptr и может доставлять кадр в
// любой момент, а экран в этот момент уже уничтожен (пользователь закрыл
// вкладку между PostMessage и обработкой). Сообщение окну, которое исчезло,
// просто не придёт — и это дешевле, чем разыменование висящего указателя.
class ScreenEndpoint final : public Observer {
public:
    explicit ScreenEndpoint(void* window, std::uint32_t message = kFeedMessage) noexcept;
    ~ScreenEndpoint() override;

    ScreenEndpoint(const ScreenEndpoint&) = delete;
    ScreenEndpoint& operator=(const ScreenEndpoint&) = delete;
    ScreenEndpoint(ScreenEndpoint&&) = delete;
    ScreenEndpoint& operator=(ScreenEndpoint&&) = delete;

    [[nodiscard]] void* window() const noexcept { return window_; }

    // Забрать накопленные кадры. Только UI-поток, из обработчика сообщения.
    // Возвращает false, когда кадров нет или окно уже не наше: экран в этом
    // случае просто ничего не перерисовывает.
    bool take(Event& out);

    // Сколько кадров пришло, а сколько забрать не удалось (окно исчезло).
    [[nodiscard]] std::uint64_t received() const noexcept;
    [[nodiscard]] std::uint64_t lost() const noexcept;

private:
    void onFrame(const Event& event) override;
    void onProgress(const ProgressFrame& frame) override;
    void onTick(const BridgeStats& stats) override;

    void* window_{nullptr};
    std::uint32_t message_{kFeedMessage};
    // Счётчики читаются константными методами, а правится очередь — из
    // наблюдателя; блокировка поэтому mutable (иначе const-метод не смог бы её
    // взять, и счётчики пришлось бы отдавать копией).
    mutable std::mutex mutex_;
    std::vector<Event> queue_;
    std::uint64_t received_{0};
    std::uint64_t lost_{0};
};

// Одна раздача на процесс. Экземпляр один: два фоновых обхода дисков были бы
// двумя минутами работы и двумя наборами чисел на экране, а §11.4 требует
// одного.
class StartupFeed {
public:
    static StartupFeed& instance() noexcept;

    StartupFeed(const StartupFeed&) = delete;
    StartupFeed& operator=(const StartupFeed&) = delete;
    StartupFeed(StartupFeed&&) = delete;
    StartupFeed& operator=(StartupFeed&&) = delete;
    ~StartupFeed();

    // Подписать окно экрана. Только UI-поток. Мост и окно-приёмник создаются
    // здесь же, в первом вызове: мост обязан быть создан в UI-потоке, иначе он
    // не знает, куда доставлять (см. «Жизненный цикл моста»).
    [[nodiscard]] std::shared_ptr<ScreenEndpoint> subscribe(void* window, std::uint32_t message = kFeedMessage);

    // Отписать экран. Только UI-поток.
    void unsubscribe(const std::shared_ptr<ScreenEndpoint>& endpoint) noexcept;

    // Поднять фоновую работу. Идемпотентно и безопасно из любого потока:
    // повторный вызов не заводит второго потока.
    void start() noexcept;

    // Что уже приехало, — чтобы экран нарисовал данные сразу, не дожидаясь
    // сообщения. Снимок неизменяемый; nullptr означает «ещё не приехало».
    [[nodiscard]] std::shared_ptr<const core::DiskInventory> inventory() const noexcept;
    [[nodiscard]] std::shared_ptr<const core::RuleSet> ruleSet() const noexcept;

    // Диагностика для журнала и «О программе»: откуда взят набор правил, что с
    // обходом дисков, сколько кадров доставлено. Одна строка, читаемая глазами.
    [[nodiscard]] std::string toText() const;

private:
    StartupFeed();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mrproper::ui::mv
