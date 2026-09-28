// Реализация оркестрации сканирования (SPEC §6.4 «Потоки и отмена»).
// Контракт, границы и список «чего модуль не делает» — в scan_coordinator.hpp;
// здесь только код.
//
// Порядок чтения: отмена (ScanCancellationToken / ScanCancellationSource) →
// прогресс (ProgressCounter) → пул (WorkerPool) → координатор (ScanCoordinator).
//
// Слой: файл переносимый, без windows.h — оркестрация ничего не знает про
// Win32, поэтому её покрывают обычные юнит-тесты на любом хосте (SPEC §6.1,
// ADR-004). Зависимость ровно одна, core::log и core::units.
#include "scan_coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/units.hpp"

namespace mrproper::engine {
namespace {

// Идентификаторы событий журнала. Стабильные строки, а не тексты сообщений:
// по ним ищут в log-файле, а тексты переводятся (§6.2).
constexpr std::string_view kEventRunStart = "scan.coordinator.start";
constexpr std::string_view kEventRunFinish = "scan.coordinator.finish";
constexpr std::string_view kEventRunAborted = "scan.coordinator.aborted";
constexpr std::string_view kEventProgress = "scan.coordinator.progress";
constexpr std::string_view kEventTaskFailed = "scan.coordinator.task.failed";
constexpr std::string_view kEventPoolJobFailed = "scan.coordinator.job.failed";
constexpr std::string_view kEventCallbackFailed = "scan.coordinator.callback.failed";

// Миллисекунды → «850 мс» / «12,4 с» / «3 мин 5 с». Своя функция вместо
// core::formatAge: тому на вход идут секунды от «сейчас», а здесь длительность
// конкретного прогона, у которой нет опорной точки.
std::string durationText(std::chrono::milliseconds value) {
    const std::int64_t ms = value.count();
    if (ms < 0) return "0 мс";
    if (ms < 1000) return std::to_string(ms) + " мс";
    char buffer[48];
    if (ms < 60'000) {
        std::snprintf(buffer, sizeof buffer, "%.1f с", static_cast<double>(ms) / 1000.0);
        return buffer;
    }
    const std::int64_t minutes = ms / 60'000;
    const std::int64_t rest = (ms % 60'000) / 1000;
    std::snprintf(buffer, sizeof buffer, "%lld мин %lld с", static_cast<long long>(minutes),
                  static_cast<long long>(rest));
    return buffer;
}

// Событие по упавшей задаче: без него в журнале нет ни имени провайдера, ни
// причины (SPEC §12 — все ошибки в логе).
void logTaskFailed(const ScanTask& task, const std::string& what) {
    core::LogFields fields;
    fields.push_back(core::logField("task", task.name));
    fields.push_back(core::logField("category", task.category));
    fields.push_back(core::logField("what", what));
    core::logError(kEventTaskFailed, "задача сканирования завершилась исключением", fields);
}

// Заготовка отчёта по задаче: всё, кроме исхода, известно на момент постановки.
TaskReport makePendingReport(const ScanTask& task) {
    TaskReport report;
    report.name = task.name;
    report.category = task.category;
    report.status = TaskStatus::Pending;
    return report;
}

}  // namespace

// ---------------------------------------------------------------------------
// Отмена
// ---------------------------------------------------------------------------

ScanCancellationToken::ScanCancellationToken(std::stop_token token) noexcept : token_(std::move(token)) {}

bool ScanCancellationToken::stopRequested() const noexcept { return token_.stop_requested(); }

bool ScanCancellationToken::stoppable() const noexcept { return token_.stop_possible(); }

bool ScanCancellationToken::shouldStop() noexcept {
    // Проверка состояния остановки — раз в kCancelCheckItems вызовов (§6.4).
    // Счётчик уменьшается на каждом элементе обхода, а stop_requested() дёргается
    // только на 256-м: в горячем цикле это заметно дешевле проверки на каждом
    // элементе, а задержка отмены в 256 итераций на файловом вводе-выводе
    // измеряется единицами миллисекунд.
    --countdown_;
    if (countdown_ != 0) return false;
    countdown_ = kCancelCheckItems;
    return token_.stop_requested();
}

bool ScanCancellationSource::requestStop() noexcept { return source_.request_stop(); }

void ScanCancellationSource::reset() noexcept {
    // Новый stop_state: прежние токены у уже идущих потоков остаются
    // действительными (у них свой stop_state), а этот источник снова можно
    // останавливать — ровно то, что нужно для следующего прогона.
    source_ = std::stop_source{};
}

// ---------------------------------------------------------------------------
// Прогресс
// ---------------------------------------------------------------------------

double ProgressSnapshot::fraction() const noexcept {
    if (itemsTotal == 0) return 0.0;  // размер неизвестен заранее — см. комментарий в hpp
    const double done = static_cast<double>(itemsDone) / static_cast<double>(itemsTotal);
    return done < 1.0 ? done : 1.0;
}

std::string ProgressSnapshot::toText() const {
    std::string out = core::formatCount(itemsDone);
    if (itemsTotal != 0) {
        out += " из ";
        out += core::formatCount(itemsTotal);
    }
    out += " элементов, ";
    out += core::formatBytes(bytesFound);
    out += ", задач ";
    out += std::to_string(tasksDone);
    out += "/";
    out += std::to_string(tasksTotal);
    out += ", потоков ";
    out += std::to_string(workersActive);
    out += "/";
    out += std::to_string(workersTotal);
    out += ", ";
    out += durationText(elapsed);
    if (cancelled) {
        out += ", отменено";
    } else if (finished) {
        out += ", завершено";
    } else {
        out += ", идёт";
    }
    return out;
}

std::int64_t ProgressCounter::ticks() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void ProgressCounter::begin() noexcept {
    items_.store(0, std::memory_order_relaxed);
    bytes_.store(0, std::memory_order_relaxed);
    itemsTotal_.store(0, std::memory_order_relaxed);
    tasksDone_.store(0, std::memory_order_relaxed);
    tasksTotal_.store(0, std::memory_order_relaxed);
    workersActive_.store(0, std::memory_order_relaxed);
    workersTotal_.store(0, std::memory_order_relaxed);
    endedTicks_.store(0, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    cancelled_.store(false, std::memory_order_relaxed);
    startedTicks_.store(ticks(), std::memory_order_relaxed);
}

void ProgressCounter::addItems(std::uint64_t count) noexcept {
    items_.fetch_add(count, std::memory_order_relaxed);
}

void ProgressCounter::addBytes(std::uint64_t count) noexcept {
    bytes_.fetch_add(count, std::memory_order_relaxed);
}

void ProgressCounter::setItemsTotal(std::uint64_t total) noexcept {
    itemsTotal_.store(total, std::memory_order_relaxed);
}

void ProgressCounter::setTasksTotal(std::size_t total) noexcept {
    tasksTotal_.store(total, std::memory_order_relaxed);
}

void ProgressCounter::setWorkersTotal(std::size_t total) noexcept {
    workersTotal_.store(total, std::memory_order_relaxed);
}

void ProgressCounter::taskFinished() noexcept { tasksDone_.fetch_add(1, std::memory_order_relaxed); }

void ProgressCounter::workerStarted() noexcept { workersActive_.fetch_add(1, std::memory_order_relaxed); }

void ProgressCounter::workerFinished() noexcept { workersActive_.fetch_sub(1, std::memory_order_relaxed); }

void ProgressCounter::markFinished(bool cancelled) noexcept {
    // Порядок важен для читателя снимка: сначала время конца, потом признак
    // finished с release — увидев finished == true, читатель уже прочитает
    // корректный endedTicks_ (acquire в snapshot()).
    endedTicks_.store(ticks(), std::memory_order_relaxed);
    cancelled_.store(cancelled, std::memory_order_relaxed);
    finished_.store(true, std::memory_order_release);
}

ProgressSnapshot ProgressCounter::snapshot() const noexcept {
    ProgressSnapshot snap;
    snap.itemsDone = items_.load(std::memory_order_relaxed);
    snap.bytesFound = bytes_.load(std::memory_order_relaxed);
    snap.itemsTotal = itemsTotal_.load(std::memory_order_relaxed);
    snap.tasksDone = tasksDone_.load(std::memory_order_relaxed);
    snap.tasksTotal = tasksTotal_.load(std::memory_order_relaxed);
    snap.workersActive = workersActive_.load(std::memory_order_relaxed);
    snap.workersTotal = workersTotal_.load(std::memory_order_relaxed);
    snap.finished = finished_.load(std::memory_order_acquire);
    snap.cancelled = cancelled_.load(std::memory_order_relaxed);

    const std::int64_t started = startedTicks_.load(std::memory_order_relaxed);
    if (started != 0) {
        // Уже завершённый прогон не должен «стареть»: время конца зафиксировано
        // один раз, а не пересчитывается на каждом чтении.
        const std::int64_t until = snap.finished ? endedTicks_.load(std::memory_order_relaxed) : ticks();
        snap.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(until - started));
    }
    return snap;
}

std::string ProgressCounter::toText() const { return snapshot().toText(); }

// ---------------------------------------------------------------------------
// Отчёт о прогоне
// ---------------------------------------------------------------------------

const char* toString(TaskStatus status) noexcept {
    switch (status) {
        case TaskStatus::Pending:
            return "pending";
        case TaskStatus::Completed:
            return "completed";
        case TaskStatus::Cancelled:
            return "cancelled";
        case TaskStatus::Failed:
            return "failed";
        case TaskStatus::Skipped:
            return "skipped";
    }
    return "unknown";
}

std::size_t ScanRunReport::count(TaskStatus status) const noexcept {
    std::size_t total = 0;
    for (const TaskReport& task : tasks) {
        if (task.status == status) ++total;
    }
    return total;
}

std::string ScanRunReport::toText() const {
    std::string out = "прогон #";
    out += std::to_string(generation);
    out += ": задач ";
    out += std::to_string(tasks.size());
    out += " (выполнено ";
    out += std::to_string(completed);
    out += ", отменено ";
    out += std::to_string(cancelledTasks);
    out += ", пропущено ";
    out += std::to_string(skipped);
    out += ", с ошибкой ";
    out += std::to_string(failed);
    out += "), элементов ";
    out += core::formatCount(items);
    out += ", ";
    out += core::formatBytes(bytes);
    out += ", потоков ";
    out += std::to_string(workers);
    out += ", ";
    out += durationText(duration);
    if (cancelled) out += ", отменён";
    return out;
}

// ---------------------------------------------------------------------------
// Контекст задачи
// ---------------------------------------------------------------------------

ScanTaskContext::ScanTaskContext(ScanCancellationToken token, ProgressCounter* counter, std::string name,
                                 std::string category) noexcept
    : token_(std::move(token)), counter_(counter), name_(std::move(name)), category_(std::move(category)) {}

bool ScanTaskContext::stopRequested() const noexcept { return token_.stopRequested(); }

bool ScanTaskContext::shouldStop() noexcept { return token_.shouldStop(); }

void ScanTaskContext::addItems(std::uint64_t count) noexcept {
    items_.fetch_add(count, std::memory_order_relaxed);
    if (counter_ != nullptr) counter_->addItems(count);
}

void ScanTaskContext::addBytes(std::uint64_t count) noexcept {
    bytes_.fetch_add(count, std::memory_order_relaxed);
    if (counter_ != nullptr) counter_->addBytes(count);
}

std::uint64_t ScanTaskContext::items() const noexcept { return items_.load(std::memory_order_relaxed); }

std::uint64_t ScanTaskContext::bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Пул рабочих потоков
// ---------------------------------------------------------------------------

WorkerPool::WorkerPool(std::size_t workers, std::stop_token stop, ProgressCounter* progress)
    : stop_(stop), progress_(progress), size_(workers != 0 ? workers : 1) {
    if (progress_ != nullptr) progress_->setWorkersTotal(size_);
    try {
        workers_.reserve(size_);
        for (std::size_t i = 0; i < size_; ++i) {
            // Лямбда копирует токен прогона: рабочий смотрит на отмену прогона,
            // а не на свой собственный stop_token. request_stop() из деструктора
            // jthread будит поток только через condition_variable_any, а здесь
            // очередь просыпается и по submit(), и по close() — этого достаточно.
            workers_.emplace_back([this, run = stop_](std::stop_token) { workerMain(run); });
        }
    } catch (...) {
        // Создание потока упало (нет ресурсов). Уже созданные потоки пришлось бы
        // ждать в деструкторе вектора — а они спят на condition_variable до
        // close(), которого не будет: будим и закрываем сами, потом Rethrow.
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        wake_.notify_all();
        workers_.clear();
        throw;
    }
}

WorkerPool::~WorkerPool() {
    // Разрушение без close() обязано быть безопасным: поток, работающий с
    // разрушенным пулом, — хуже, чем лишнее ожидание.
    close();
}

bool WorkerPool::submit(std::function<void()> job) {
    std::lock_guard lock(mutex_);
    if (closing_ || stop_.stop_requested()) return false;
    queue_.push_back(std::move(job));
    wake_.notify_one();
    return true;
}

void WorkerPool::drain() noexcept {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}

void WorkerPool::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (joined_) return;  // повторный вызов безопасен
    }
    // Сначала дорабатываем очередь, и только потом закрываем: контракт close() —
    // это drain() + остановка, а не «выбросить не начатое».
    drain();
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    // Будим ДО join: спящий поток иначе не увидит closing_.
    wake_.notify_all();
    {
        std::lock_guard lock(mutex_);
        joined_ = true;
    }
    // Разрушение jthread: request_stop() своему токену плюс join. Работа уже
    // дренирована, поэтому потоки уже вышли и присоединение мгновенно.
    workers_.clear();
}

void WorkerPool::workerMain(std::stop_token token) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            // Предусловие: work не выполняется после остановки прогона. Синхронный
            // вызов Win32 прервать нельзя, поэтому начатое доходит до конца, а не
            // начатое отбрасывается — под отброшенную работу у вызывающего есть
            // слот в отчёте (Skipped/Cancelled).
            wake_.wait(lock, [this, &token] { return closing_ || token.stop_requested() || !queue_.empty(); });
            if (closing_ || token.stop_requested()) {
                dropQueueLocked();
                lock.unlock();
                idle_.notify_all();
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        if (progress_ != nullptr) progress_->workerStarted();

        runJob(std::move(job));

        {
            std::lock_guard lock(mutex_);
            --active_;
            ++completed_;
            if (progress_ != nullptr) progress_->workerFinished();
            if (queue_.empty() && active_ == 0) idle_.notify_all();
        }
    }
}

void WorkerPool::runJob(std::function<void()> job) noexcept {
    try {
        job();
    } catch (const std::exception& e) {
        // Работа пула не имеет права ронять прогон: исключение из задачи — это
        // её исход (Failed), а не конец процесса. Задача обычно ловит своё
        // сама (runTask), здесь ловится то, что осталось.
        core::LogFields fields;
        fields.push_back(core::logField("what", e.what()));
        core::logError(kEventPoolJobFailed, "работа пула завершилась исключением", fields);
    } catch (...) {
        core::logError(kEventPoolJobFailed, "работа пула завершилась неизвестным исключением");
    }
}

void WorkerPool::dropQueueLocked() noexcept {
    // Слот std::function уничтожается здесь же: незакрытая работа не должна
    // висеть в памяти до конца приложения.
    queue_.clear();
}

std::size_t WorkerPool::size() const noexcept {
    std::lock_guard lock(mutex_);
    return size_;
}

std::size_t WorkerPool::pending() const noexcept {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::size_t WorkerPool::active() const noexcept {
    std::lock_guard lock(mutex_);
    return active_;
}

std::size_t WorkerPool::completed() const noexcept {
    std::lock_guard lock(mutex_);
    return completed_;
}

std::string WorkerPool::toText() const {
    std::lock_guard lock(mutex_);
    std::string out = "пул: потоков ";
    out += std::to_string(size_);
    out += ", в очереди ";
    out += std::to_string(queue_.size());
    out += ", активно ";
    out += std::to_string(active_);
    out += ", выполнено ";
    out += std::to_string(completed_);
    if (closing_) out += ", закрывается";
    if (stop_.stop_requested()) out += ", отмена";
    return out;
}

// ---------------------------------------------------------------------------
// Координатор
// ---------------------------------------------------------------------------

ScanCoordinator::ScanCoordinator(ScanCoordinatorOptions options) : options_(std::move(options)) {
    if (options_.maxWorkers == 0) options_.maxWorkers = kDefaultMaxWorkers;
    if (options_.progressInterval <= std::chrono::milliseconds::zero()) options_.progressInterval = kProgressInterval;
}

ScanCoordinator::~ScanCoordinator() {
    requestStop();
    if (runner_.joinable()) runner_.join();
}

std::size_t ScanCoordinator::resolveWorkers(std::size_t tasks) const noexcept {
    const std::size_t maxWorkers = options_.maxWorkers;
    if (options_.workers != 0) return std::min(options_.workers, maxWorkers);
    if (tasks == 0) return 1;
    std::size_t hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;  // неизвестно — берём консервативное число
    // Больше потоков, чем задач, смысла не имеет: лишние потоки только спят.
    return std::clamp(std::min(tasks, hardware), std::size_t{1}, maxWorkers);
}

bool ScanCoordinator::start(std::vector<ScanTask> tasks) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (running_) return false;  // повторный клик «Сканировать» игнорируется

    // Предыдущий поток, если его не дождались, присоединяем ДО любых правок:
    // присваивание в joinable-поток вызывает std::terminate, а разблокировка
    // нужна, чтобы ждать вне мьютекса состояния.
    if (runner_.joinable()) {
        std::jthread previous = std::move(runner_);
        lock.unlock();
        previous.join();
        lock.lock();
        // Пока мьютекс был отпущен, прогон мог начать кто-то ещё.
        if (running_) return false;
    }

    // Новый прогон — новый stop_state: сбросить источник нельзя, а «погоняем
    // отменённый прогон» — не то поведение, которое нужно приложению.
    stop_ = std::stop_source{};
    runToken_ = stop_.get_token();
    tasks_ = std::move(tasks);

    progress_.begin();
    progress_.setTasksTotal(tasks_.size());
    // Знаменатель прогресса известен, только если ВСЕ задачи его назвали:
    // смешивать «частично известный» счётчик с «полностью неизвестным» нельзя,
    // иначе полоса прогресса покажет недобор как 99 %.
    std::uint64_t expected = 0;
    bool known = !tasks_.empty();
    for (const ScanTask& task : tasks_) {
        if (task.expectedItems == 0) {
            known = false;
            break;
        }
        expected += task.expectedItems;
    }
    progress_.setItemsTotal(known ? expected : std::uint64_t{0});

    ++generation_;
    running_ = true;
    runner_ = std::jthread([this] { runLoop(); });
    return true;
}

ScanRunReportPtr ScanCoordinator::run(std::vector<ScanTask> tasks) {
    start(std::move(tasks));
    wait();
    return result();
}

bool ScanCoordinator::wait() {
    std::unique_lock<std::mutex> lock(stateMutex_);
    stateCv_.wait(lock, [this] { return !running_; });
    if (runner_.joinable()) {
        std::jthread local = std::move(runner_);
        lock.unlock();
        local.join();
    }
    return true;
}

bool ScanCoordinator::waitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (!stateCv_.wait_for(lock, timeout, [this] { return !running_; })) return false;
    if (runner_.joinable()) {
        std::jthread local = std::move(runner_);
        lock.unlock();
        local.join();
    }
    return true;
}

void ScanCoordinator::requestStop() noexcept {
    // Мьютекс обязателен: start() присваивает новый stop_source под ним же, и
    // без мьютекса это была бы гонка «отменяем уже чужой прогон».
    std::lock_guard lock(stateMutex_);
    stop_.request_stop();
}

bool ScanCoordinator::running() const noexcept {
    std::lock_guard lock(stateMutex_);
    return running_;
}

std::uint64_t ScanCoordinator::generation() const noexcept {
    std::lock_guard lock(stateMutex_);
    return generation_;
}

ProgressSnapshot ScanCoordinator::progress() const noexcept { return progress_.snapshot(); }

ScanRunReportPtr ScanCoordinator::result() const noexcept {
    std::lock_guard lock(stateMutex_);
    return result_;
}

ScanCancellationToken ScanCoordinator::token() const noexcept {
    std::lock_guard lock(stateMutex_);
    return ScanCancellationToken(runToken_);
}

std::string ScanCoordinator::toText() const {
    std::lock_guard lock(stateMutex_);
    std::string out = "координатор: прогон #";
    out += std::to_string(generation_);
    out += running_ ? " идёт" : " не идёт";
    out += ", задач в очереди ";
    out += std::to_string(tasks_.size());
    out += ", ";
    out += progress_.toText();  // снимок берётся на атомиках, мьютекс не нужен
    if (result_ != nullptr) {
        out += "; ";
        out += result_->toText();
    }
    return out;
}

void ScanCoordinator::runLoop() {
    const std::size_t total = tasks_.size();
    const std::stop_token token = runToken_;

    // Поток публикации прогресса живёт ровно прогон: он будит UI раз в
    // options_.progressInterval (§6.4 — 100 мс) и умирает вместе с прогоном.
    std::atomic<bool> active{true};
    std::jthread progressThread([this, &active, token](std::stop_token) { progressLoop(token, active); });

    std::vector<TaskReport> reports;
    bool aborted = false;
    std::string failure;
    try {
        reports.reserve(total);
        for (const ScanTask& task : tasks_) reports.push_back(makePendingReport(task));
        runTasks(token, reports);
    } catch (const std::exception& e) {
        // Из потока прогона наружу std::exception — это std::terminate. Отчёт
        // всё равно публикуется: UI должен увидеть неполный результат с
        // причиной, а не «зависшую» полосу прогресса.
        aborted = true;
        failure = e.what();
        core::LogFields fields;
        fields.push_back(core::logField("tasks", total));
        fields.push_back(core::logField("what", failure));
        core::logError(kEventRunAborted, "прогон прерван исключением, отчёт публикуется неполным", fields);
    } catch (...) {
        aborted = true;
        failure = "неизвестное исключение";
        core::logError(kEventRunAborted, "прогон прерван неизвестным исключением, отчёт публикуется неполным");
    }

    // Поток прогресса останавливаем ДО публикации: иначе после running_ == false
    // он успеет прислать ещё один снимок «идёт».
    active.store(false, std::memory_order_release);
    stateCv_.notify_all();  // будим поток прогресса, чтобы он не ждал остаток интервала
    progressThread.request_stop();
    progressThread.join();

    publishReport(token, reports, aborted, failure);
}

void ScanCoordinator::runTasks(std::stop_token token, std::vector<TaskReport>& reports) {
    const std::size_t workers = resolveWorkers(reports.size());
    logRunStart(reports.size());
    {
        WorkerPool pool(workers, token, &progress_);
        for (std::size_t i = 0; i < reports.size(); ++i) {
            // У каждой задачи свой слот отчёта: записи из разных потоков не
            // пересекаются, поэтому мьютекс на счётчиках задач не нужен, а
            // отчёт читается только после close() (join даёт happens-before).
            const std::size_t index = i;
            if (!pool.submit([this, token, index, &reports] { runTask(token, index, reports); })) {
                // Пул отказался принять работу (уже запрошена остановка или он
                // закрыт). Не начатые задачи досчитаются в publishReport.
                break;
            }
        }
        pool.close();  // дренирование очереди + присоединение потоков
    }
    // workersTotal не обнуляем: полосе прогресса полезно знать, сколько потоков
    // было у прогона, а не «0» после его конца.
}

void ScanCoordinator::runTask(std::stop_token token, std::size_t index, std::vector<TaskReport>& reports) {
    if (index >= tasks_.size() || index >= reports.size()) return;
    const ScanTask& task = tasks_[index];
    TaskReport& report = reports[index];
    if (report.status != TaskStatus::Pending) return;  // задачу уже закрыли

    const auto started = std::chrono::steady_clock::now();
    if (task.run == nullptr) {
        report.status = TaskStatus::Skipped;
        report.error = "у задачи нет тела run";
    } else if (token.stop_requested()) {
        report.status = TaskStatus::Cancelled;
    } else {
        ScanTaskContext context(ScanCancellationToken(token), &progress_, task.name, task.category);
        try {
            task.run(context);
            // Задача, попросившая отмену, честно помечается отменённой: её
            // результат неполон, и выдавать его за Completed нельзя.
            report.status = (token.stop_requested() || context.stopRequested()) ? TaskStatus::Cancelled
                                                                              : TaskStatus::Completed;
        } catch (const std::exception& e) {
            report.status = TaskStatus::Failed;
            report.error = e.what();
            logTaskFailed(task, report.error);
        } catch (...) {
            report.status = TaskStatus::Failed;
            report.error = "неизвестное исключение";
            logTaskFailed(task, report.error);
        }
        report.items = context.items();
        report.bytes = context.bytes();
    }
    report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    progress_.taskFinished();
}

void ScanCoordinator::progressLoop(std::stop_token token, const std::atomic<bool>& active) {
    const std::chrono::milliseconds interval = options_.progressInterval;
    // Первый снимок сразу: UI не должен ждать 100 мс, чтобы нарисовать «пуск».
    emitProgress(progress_.snapshot());
    while (active.load(std::memory_order_acquire) && !token.stop_requested()) {
        {
            // Ждём либо интервал, либо конец прогона (runLoop будит stateCv_ перед
            // остановкой этого потока) — так публикация отчёта не задерживается
            // на последний интервал.
            std::unique_lock<std::mutex> lock(stateMutex_);
            stateCv_.wait_for(lock, interval, [this, &active] {
                return !active.load(std::memory_order_acquire) || !running_;
            });
        }
        if (!active.load(std::memory_order_acquire)) break;
        emitProgress(progress_.snapshot());
    }
}

void ScanCoordinator::emitProgress(const ProgressSnapshot& snapshot) {
    if (options_.logProgress) {
        core::LogFields fields;
        fields.push_back(core::logField("items", snapshot.itemsDone));
        fields.push_back(core::logField("bytes", snapshot.bytesFound));
        fields.push_back(core::logField("tasksDone", static_cast<std::uint64_t>(snapshot.tasksDone)));
        fields.push_back(core::logField("tasksTotal", static_cast<std::uint64_t>(snapshot.tasksTotal)));
        fields.push_back(core::logField("workers", static_cast<std::uint64_t>(snapshot.workersActive)));
        fields.push_back(core::logField("cancelled", snapshot.cancelled));
        core::logDebug(kEventProgress, snapshot.toText(), fields);
    }

    const std::function<void(const ProgressSnapshot&)>& callback = options_.onProgress;
    if (!callback) return;
    // Исключение из колбэка не имеет права уронить прогон сканирования
    // (SPEC §5): интерфейс — чужой код, и его ошибка не отменяет обход диска.
    // Обратное тоже верно: колбэк не должен блокировать прогон. Вызов wait() из
    // колбэка — взаимоблокировка (прогон ждёт завершения колбэка, а колбэк ждёт
    // конца прогона), поэтому блокирующее ожидание в UI не используется.
    if (options_.dispatcher) {
        // Колбэк копируется внутрь лямбды: маршалинг (PostMessage) выполняет её
        // позже и в общем случае уже после разрушения координатора, поэтому
        // лямбда не имеет права держать указатель на this.
        try {
            options_.dispatcher([callback, snapshot] {
                try {
                    callback(snapshot);
                } catch (...) {
                    core::logError(kEventCallbackFailed, "колбэк прогресса бросил исключение");
                }
            });
        } catch (const std::exception& e) {
            core::LogFields fields;
            fields.push_back(core::logField("what", e.what()));
            core::logError(kEventCallbackFailed, "диспетчер колбэка прогресса бросил исключение", fields);
        } catch (...) {
            core::logError(kEventCallbackFailed, "диспетчер колбэка прогресса бросил неизвестное исключение");
        }
        return;
    }
    try {
        callback(snapshot);
    } catch (...) {
        core::logError(kEventCallbackFailed, "колбэк прогресса бросил исключение");
    }
}

void ScanCoordinator::logRunStart(std::size_t tasks) const {
    core::LogFields fields;
    fields.push_back(core::logField("generation", generation_));
    fields.push_back(core::logField("tasks", static_cast<std::uint64_t>(tasks)));
    fields.push_back(core::logField("workers", static_cast<std::uint64_t>(resolveWorkers(tasks))));
    core::logInfo(kEventRunStart, "скан запущен", fields);
}

void ScanCoordinator::logRunFinish(const ScanRunReport& report) const {
    core::LogFields fields;
    fields.push_back(core::logField("generation", report.generation));
    fields.push_back(core::logField("tasks", static_cast<std::uint64_t>(report.tasks.size())));
    fields.push_back(core::logField("completed", static_cast<std::uint64_t>(report.completed)));
    fields.push_back(core::logField("cancelledTasks", static_cast<std::uint64_t>(report.cancelledTasks)));
    fields.push_back(core::logField("failed", static_cast<std::uint64_t>(report.failed)));
    fields.push_back(core::logField("skipped", static_cast<std::uint64_t>(report.skipped)));
    fields.push_back(core::logField("items", report.items));
    fields.push_back(core::logField("bytes", report.bytes));
    fields.push_back(core::logField("workers", static_cast<std::uint64_t>(report.workers)));
    fields.push_back(core::logField("durationMs", static_cast<std::int64_t>(report.duration.count())));
    fields.push_back(core::logField("cancelled", report.cancelled));
    if (report.cancelled) {
        core::logWarn(kEventRunFinish, report.toText(), fields);
    } else {
        core::logInfo(kEventRunFinish, report.toText(), fields);
    }
}

void ScanCoordinator::publishReport(std::stop_token token, std::vector<TaskReport>& reports, bool aborted,
                                    const std::string& failure) {
    const bool cancelled = token.stop_requested() || aborted;

    ScanRunReport report;
    report.generation = generation_;
    report.workers = resolveWorkers(reports.size());
    report.cancelled = cancelled;
    if (aborted && !failure.empty()) {
        core::LogFields fields;
        fields.push_back(core::logField("generation", report.generation));
        fields.push_back(core::logField("what", failure));
        fields.push_back(core::logField("tasks", static_cast<std::uint64_t>(reports.size())));
        core::logWarn(kEventRunAborted, "прогон закрыт досрочно", fields);
    }
    for (TaskReport& task : reports) {
        if (task.status == TaskStatus::Pending) {
            // Задачу не забрал ни один поток: после отмены это «отменена», без
            // отмены — пул не взял работу. Молча пропадать из отчёта она не
            // должна: по отчёту видно, что именно не выполнено.
            task.status = cancelled ? TaskStatus::Cancelled : TaskStatus::Skipped;
        }
        report.items += task.items;
        report.bytes += task.bytes;
        switch (task.status) {
            case TaskStatus::Completed:
                ++report.completed;
                break;
            case TaskStatus::Cancelled:
                ++report.cancelledTasks;
                break;
            case TaskStatus::Failed:
                ++report.failed;
                break;
            case TaskStatus::Skipped:
                ++report.skipped;
                break;
            case TaskStatus::Pending:
                break;  // сюда дойти не может: выше он заменён
        }
    }
    // Экспорт: elapsed снимается ПОСЛЕ markFinished, иначе отчёт содержал бы
    // длительность, которую успела «дорасти» служебная строка.
    progress_.markFinished(report.cancelled);
    report.tasks = std::move(reports);
    report.duration = progress_.snapshot().elapsed;
    emitProgress(progress_.snapshot());

    const ScanRunReportPtr published = std::make_shared<const ScanRunReport>(std::move(report));
    {
        std::lock_guard lock(stateMutex_);
        result_ = published;
        tasks_.clear();  // освобождаем тела задач сразу: память прогона не нужна после отчёта
        running_ = false;
    }
    stateCv_.notify_all();
    logRunFinish(*published);
}

}  // namespace mrproper::engine
