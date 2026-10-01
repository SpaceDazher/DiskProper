// MrProper — мост модель ↔ представление: реализация (SPEC §6.4).
//
// Проектные решения и их обоснование — в заголовке. Здесь только Win32-часть
// (PostMessage, скрытое окно-приёмник, таймер) и сам маршалинг. Единственное
// место в файле, где живёт windows.h, — по правилу слоя ui: «ни один файл этого
// каталога не должен включать <d2d1.h> вне .cpp и не должен обращаться к WinAPI
// без обёртки из platform» (src/ui/CMakeLists.txt). Заголовок моста остаётся без
// Win32, поэтому его можно включить в код, собираемый без окон.
#include "mv_bridge.hpp"

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cwchar>

#include "core/log.hpp"
#include "core/rulesync.hpp"
#include "platform/inventory.hpp"

namespace mrproper::ui::mv {
namespace {

// ---------------------------------------------------------------------------
// Константы Win32 и окна
// ---------------------------------------------------------------------------

// Собственные сообщения моста. Диапазон WM_APP… — единственный, который
// гарантированно не занят Win32 и приложением; два значения, потому что работа
// и тик — разные вещи: работа исполняется сразу, тик только просит дойти до
// слота прогресса.
inline constexpr UINT kBridgeJob = WM_APP + 0x41;   // выполнить работу в UI-потоке
inline constexpr UINT kBridgeTick = WM_APP + 0x42;  // отдать накопленный прогресс
inline constexpr UINT_PTR kBridgeTimerId = 0x42;    // идентификатор WM_TIMER моста

// Имя класса окна-приёмника. Одно на процесс: два моста (главный и, например,
// диалог dry-run) переиспользуют один класс, а регистрация второго возвращает
// ERROR_CLASS_ALREADY_EXISTS — это штатный исход, а не отказ.
inline constexpr wchar_t kBridgeClassName[] = L"MrProper.ModelViewBridge";

// Стили окна-приёмника: невидимое, без заголовка, без активации.
// WS_EX_TOOLWINDOW убирает его из Alt+Tab, WS_EX_NOACTIVATE — не даёт
// перехватить фокус, что важно: окно-приёмник не должно влиять на ввод
// пользователя (§5 «Клавиатурная навигация, фокус»).
inline constexpr DWORD kSinkStyle = WS_POPUP;
inline constexpr DWORD kSinkExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

// Сколько значащих цифр печатать в журнале. 21 знак хватает на uint64 (максимум
// 18446744073709551615 — это 20 цифр), а snprintf с буфером в 32 байта ничего
// не обрезает. Возвращать длину, а не строку, приходится из-за обработки ошибки
// snprintf: молчаливый «пустой хвост» в журнале читается как «счётчик ноль».
int formatNumber(char (&buffer)[32], std::uint64_t value) noexcept {
    const int written =
        std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return written > 0 && written < static_cast<int>(sizeof(buffer)) ? written : 0;
}

void appendNumber(std::string& out, std::uint64_t value) noexcept {
    char buffer[32] = {};
    if (const int written = formatNumber(buffer, value); written > 0) {
        out.append(buffer, static_cast<std::size_t>(written));
    }
}

// Процент одной строкой: «37.0 %». В журнале округление своё и намеренно
// грубее, чем в отрисовке (там числа берёт core::units): журнал не должен
// выглядеть точнее, чем измерение.
void appendPercent(std::string& out, double fraction) noexcept {
    char buffer[32] = {};
    const double clamped = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    const int written =
        std::snprintf(buffer, sizeof(buffer), "%.1f%%", static_cast<double>(clamped) * 100.0);
    if (written > 0 && written < static_cast<int>(sizeof(buffer))) {
        out.append(buffer, static_cast<std::size_t>(written));
    }
}

// Наносекунды steady_clock в целом числе. Через int64, а не через
// std::atomic<time_point>: атомик требует тривиальной копируемости, и полагаться
// на деталь реализации часов в MSVC v142 не стоит.
std::int64_t nowNanos() noexcept {
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since).count();
}

// Забрать и удалить работы, оставшиеся в очереди сообщений окна. Нужна перед
// уничтожением приёмника: система молча выбрасывает сообщения уничтоженного
// окна, а вместе с ними — указатели на поставленные работы, и закрытие окна во
// время скана оставило бы утечку на каждый кадр.
void discardPendingJobs(HWND sink) noexcept {
    if (sink == nullptr) return;
    MSG pending{};
    while (::PeekMessageW(&pending, sink, kBridgeJob, kBridgeJob, PM_REMOVE) != FALSE) {
        delete reinterpret_cast<std::function<void()>*>(pending.lParam);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Имена и вычисления кадров
// ---------------------------------------------------------------------------

const char* eventKindName(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::Inventory: return "Inventory";
    case EventKind::Disks: return "Disks";
    case EventKind::ScanProgress: return "ScanProgress";
    case EventKind::Candidates: return "Candidates";
    case EventKind::Phase: return "Phase";
    case EventKind::OperationResult: return "OperationResult";
    case EventKind::UndoAvailable: return "UndoAvailable";
    case EventKind::Notice: return "Notice";
    case EventKind::Error: return "Error";
    case EventKind::RuleSet: return "RuleSet";
    default: break;
    }
    // Неизвестный номер приходит из испорченной памяти или из более новой сборки
    // модели. Строка читаемая и в обоих случаях.
    return "Unknown";
}

const char* Event::payloadTypeName() const noexcept { return type_.name(); }

double ProgressFrame::fraction() const noexcept {
    // Знаменатель выбирается по тому, что известно. Байты предпочтительнее
    // элементов: обход ФС считает элементы неточно (каталог — один элемент, а
    // может содержать миллион файлов), а байты сопоставимы между категориями.
    if (bytesTotal > 0) {
        if (bytesDone >= bytesTotal) return 1.0;
        return static_cast<double>(bytesDone) / static_cast<double>(bytesTotal);
    }
    if (itemsTotal > 0) {
        if (itemsDone >= itemsTotal) return 1.0;
        return static_cast<double>(itemsDone) / static_cast<double>(itemsTotal);
    }
    if (tasksTotal > 0 && tasksDone > 0) {
        return static_cast<double>(tasksDone) / static_cast<double>(tasksTotal);
    }
    return 0.0;  // объём неизвестен заранее — индикатор обязан быть неопределённым
}

bool ProgressFrame::indeterminate() const noexcept { return bytesTotal == 0 && itemsTotal == 0; }

double ProgressFrame::clampedFraction() const noexcept {
    const double value = fraction();
    if (!(value > 0.0)) return 0.0;  // NaN тоже сюда: «неизвестно» лучше мусора на полосе
    return value > 1.0 ? 1.0 : value;
}

std::string ProgressFrame::toText() const {
    std::string out;
    out.reserve(192);
    if (!phase.empty()) out.append(phase).append(": ");
    out.append("элементов ");
    appendNumber(out, itemsDone);
    if (itemsTotal > 0) {
        out.append(" из ");
        appendNumber(out, itemsTotal);
    }
    out.append(", байт ");
    appendNumber(out, bytesDone);
    if (bytesTotal > 0) {
        out.append(" из ");
        appendNumber(out, bytesTotal);
    }
    out.append(", задач ");
    appendNumber(out, static_cast<std::uint64_t>(tasksDone));
    if (tasksTotal > 0) {
        out.append(" из ");
        appendNumber(out, static_cast<std::uint64_t>(tasksTotal));
    }
    if (!indeterminate()) {
        out.append(", ");
        appendPercent(out, fraction());
    }
    if (!currentPath.empty()) {
        out.append(" — ");
        // Длинный путь обрезается с начала: конец пути (имя каталога) полезнее
        // начала («C:\Users\...» у всех начинается одинаково).
        if (currentPath.size() > 160) {
            out.append("…").append(currentPath.substr(currentPath.size() - 160));
        } else {
            out.append(currentPath);
        }
    }
    return out;
}

std::string BridgeStats::toText() const {
    std::string out;
    out.reserve(208);
    out.append("маршалинг: ");
    const auto append = [&out](std::string_view name, std::uint64_t value) {
        out.append(name).append("=");
        appendNumber(out, value);
        out.append(" ");
    };
    append("отдано", delivered);
    append("поставлено", posted);
    append("потеряно", dropped);
    append("слито", coalesced);
    append("троттлинг", throttled);
    append("кадров_прогресса", progressDelivered);
    append("не_из_UI", misthreaded);
    append("ошибок_наблюдателей", observerErrors);
    append("окно_приёмник", sinkCreated);
    return out;
}

// ---------------------------------------------------------------------------
// Наблюдатель
// ---------------------------------------------------------------------------

Observer::~Observer() = default;

// Пустые тела без имён параметров: именованный неиспользуемый параметр даёт
// C4100 на /W4, а объявить виртуальный метод с телом-заглушкой иначе нельзя —
// он обязан существовать, чтобы экран переопределял только нужное.
void Observer::onFrame(const Event&) {}
void Observer::onProgress(const ProgressFrame&) {}
void Observer::onTick(const BridgeStats&) {}

// ---------------------------------------------------------------------------
// Диспетчеры
// ---------------------------------------------------------------------------

Dispatcher::~Dispatcher() = default;

PostMessageDispatcher::PostMessageDispatcher(void* window, std::uint32_t uiThreadId) noexcept
    : window_(window), uiThreadId_(uiThreadId) {}

PostMessageDispatcher::~PostMessageDispatcher() = default;

bool PostMessageDispatcher::onUiThread() const noexcept {
    return uiThreadId_ != 0 && ::GetCurrentThreadId() == uiThreadId_;
}

bool PostMessageDispatcher::post(std::function<void()>&& job) noexcept {
    if (window_ == nullptr || !job) return false;

    // Владение указателем переходит окну: то выполнит и удалит. Размещение
    // nothrow, потому что post() — noexcept, и terminate из-за нехватки памяти
    // хуже, чем честный отказ с кодом в журнале.
    std::function<void()>* payload = nullptr;
    try {
        // Размещение nothrow спасает только от нехватки памяти на самом блоке:
        // конструктор std::function тоже умеет бросить, а post() — noexcept.
        payload = new (std::nothrow) std::function<void()>(std::move(job));
    } catch (...) {
        payload = nullptr;
    }
    if (payload == nullptr) {
        lastError_ = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
        failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (::PostMessageW(static_cast<HWND>(window_), kBridgeJob, 0, reinterpret_cast<LPARAM>(payload)) ==
        FALSE) {
        const auto code = static_cast<std::uint32_t>(::GetLastError());
        lastError_ = code;
        failed_.fetch_add(1, std::memory_order_relaxed);
        delete payload;  // окно не получило — владение остаётся за нами
        return false;
    }
    lastError_ = 0;
    return true;
}

std::uint32_t PostMessageDispatcher::lastError() const noexcept { return lastError_; }

std::uint64_t PostMessageDispatcher::failed() const noexcept {
    return failed_.load(std::memory_order_relaxed);
}

InlineDispatcher::InlineDispatcher(std::uint32_t uiThreadId) noexcept : uiThreadId_(uiThreadId) {}
InlineDispatcher::~InlineDispatcher() = default;

bool InlineDispatcher::onUiThread() const noexcept {
    return uiThreadId_ != 0 && ::GetCurrentThreadId() == uiThreadId_;
}

bool InlineDispatcher::post(std::function<void()>&& job) noexcept {
    if (!job) return false;
    posted_.fetch_add(1, std::memory_order_relaxed);
    try {
        job();
    } catch (...) {
        // Исключение из доставки не имеет права пересечь границу вызывающего:
        // post() — noexcept, и std::terminate здесь означал бы, что ошибка в
        // наблюдателе убила приложение (§5 «устойчивость»).
        return false;
    }
    return true;
}

std::uint64_t InlineDispatcher::posted() const noexcept { return posted_.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Мост: внутреннее состояние
// ---------------------------------------------------------------------------

// Счётчики моста живут отдельно от самого моста и держатся общим указателем.
// Причина одна, но она принципиальная: работа, поставленная в очередь ОС,
// приезжает позже и в худшем случае — уже после того, как мост разрушен
// (пользователь закрыл окно, пока шёл скан). Если бы счётчик оставался полем
// моста, «снять кадр из очереди» после разрушения было бы обращением к
// освобождённой памяти. Объект счётчиков переживает очередь и умирает вместе
// с последней работой.
struct BridgeCounters {
    // Мост ещё жив. Ставится false в его деструкторе ДО всего остального, и
    // проверяется работой из очереди перед разыменованием владельца: так кадр,
    // пришедший после закрытия окна, не лезет в освобождённую память.
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> inFlight{0};
    std::atomic<std::uint64_t> posted{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> throttled{0};
    std::atomic<std::uint64_t> coalesced{0};
    std::atomic<std::uint64_t> progressDelivered{0};
    std::atomic<std::uint64_t> misthreaded{0};
    std::atomic<std::uint64_t> observerErrors{0};
    std::atomic<std::int64_t> lastProgressPostNs{0};
    std::atomic<bool> progressWake{false};
};

// Слот прогресса: единственное место с блокировкой в мосте (см. заголовок).
// Пишет фоновый поток не чаще раза в progressInterval, читает UI-поток один раз
// за тик. Значение — разделяемое: читатель держит shared_ptr, поэтому слот
// освобождается не в момент чтения, а когда читатель договорил с кадром.
struct ProgressSlot {
    std::mutex mutex;
    std::shared_ptr<const ProgressFrame> frame;
};

struct ModelViewBridge::Impl {
    // --- Параметры (не меняются после создания) ---
    ModelViewBridge::Options options;
    std::uint32_t uiThreadId{0};

    // --- Приёмник ---
    HWND sink{nullptr};
    bool classRegistered{false};

    // --- Диспетчер ---
    std::unique_ptr<Dispatcher> dispatcher;
    ModelViewBridge* owner{nullptr};  // nullptr после destroy(): окно больше не зовёт мост

    // --- Счётчики и прогресс ---
    std::shared_ptr<BridgeCounters> counters;
    std::unique_ptr<ProgressSlot> slot;

    // --- Наблюдатели (только UI-поток) ---
    std::vector<std::shared_ptr<Observer>> observers;
    std::vector<std::shared_ptr<Observer>> deferredAdd;
    std::vector<std::shared_ptr<Observer>> deferredRemove;
    std::uint64_t tickBaseline{0};  // счётчик доставок на момент прошлого onTick
    bool dispatching{false};

    // Журнал отказов: «поток» и «очередь» пишутся по одному разу, иначе поток,
    // публикующий по миллиону кадров в секунду, засоряет лог быстрее, чем его
    // читает человек.
    std::atomic<int> threadWarned{0};
    std::atomic<int> queueWarned{0};

    ~Impl() {
        // Порядок важен: сначала окно перестаёт звать мост, потом умирает
        // приёмник. Иначе сообщение, пришедшее в DestroyWindow, выполнилось бы
        // на освобождаемом this.
        if (sink != nullptr) {
            if (classRegistered) ::KillTimer(sink, kBridgeTimerId);
            discardPendingJobs(sink);
            ::DestroyWindow(sink);
            sink = nullptr;
        }
        observers.clear();
        deferredAdd.clear();
        deferredRemove.clear();
    }
};

// ---------------------------------------------------------------------------
// Мост: журнал
// ---------------------------------------------------------------------------

void ModelViewBridge::logBridge(std::string_view event, std::string_view message) noexcept {
    core::logInfo(event, message);
}

void ModelViewBridge::logWin32Failure(std::string_view event, std::string_view where,
                                      unsigned long code) noexcept {
    try {
        core::LogFields fields;
        fields.push_back(core::logField("where", where));
        fields.push_back(core::logField("code", static_cast<std::int64_t>(code)));
        core::logError(event, "не удалось вызвать WinAPI — кадр не доставлен", std::move(fields));
    } catch (...) {
        // Журнал не имеет права ронять интерфейс (§5, §12): если и он отказал,
        // мост просто молчит об этом отказе.
    }
}

void ModelViewBridge::logOnce(std::string_view event, std::string_view message,
                              std::atomic<int>& flag) noexcept {
    if (flag.exchange(1, std::memory_order_relaxed) != 0) return;
    logBridge(event, message);
}

std::int64_t ModelViewBridge::unixNow() noexcept {
    // Через std::chrono, а не через GetSystemTimeAsFileTime: мосту не нужна
    // Win32-функция ради одного поля, а system_clock на Windows даёт те же
    // 100 нс, что и FILETIME, и переживает запуск до инициализации COM.
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since).count();
    return static_cast<std::int64_t>(seconds);
}

// ---------------------------------------------------------------------------
// Мост: окно-приёмник
// ---------------------------------------------------------------------------

namespace {

// Обработчик окна-приёмника. Статическая функция: мост лежит в GWLP_USERDATA,
// и иначе статической памяти на каждый экран не нашлось бы.
//
// Исключение не пересекает границу Win32 (§5): работа обёрнута в try/catch.
LRESULT CALLBACK sinkWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    auto* bridge = reinterpret_cast<ModelViewBridge*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == kBridgeJob) {
        auto* job = reinterpret_cast<std::function<void()>*>(lParam);
        // Владение указателем перешло к нам вместе с сообщением.
        if (job != nullptr) {
            std::function<void()> work;
            try {
                work = std::move(*job);
            } catch (...) {
            }
            delete job;
            if (work) {
                try {
                    work();
                } catch (...) {
                    core::logError("ui.mv.job", "работа маршалинга бросила исключение");
                }
            }
        }
        return 0;
    }
    if (message == kBridgeTick || (message == WM_TIMER && wParam == kBridgeTimerId)) {
        if (bridge != nullptr) bridge->pump();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

void ModelViewBridge::createSink() noexcept {
    if (impl_->sink != nullptr) return;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW description{};
    description.cbSize = static_cast<UINT>(sizeof(description));
    description.lpfnWndProc = &sinkWindowProc;
    description.hInstance = instance;
    description.lpszClassName = kBridgeClassName;

    if (::RegisterClassExW(&description) != 0) {
        impl_->classRegistered = true;
    } else if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logWin32Failure("ui.mv.sink", "RegisterClassExW", ::GetLastError());
        return;  // без класса окно не создастся — мост останется без приёмника
    }

    // Родитель — окно приложения: приёмник не должен быть отдельным «приложением»
    // в Alt+Tab, а с nullptr он мигает за панелью задач при создании.
    impl_->sink = ::CreateWindowExW(kSinkExStyle, kBridgeClassName, nullptr, kSinkStyle, 0, 0, 0, 0,
                                     static_cast<HWND>(impl_->options.ownerWindow), nullptr, instance,
                                     nullptr);
    if (impl_->sink == nullptr) {
        logWin32Failure("ui.mv.sink", "CreateWindowExW(sink)", ::GetLastError());
        return;
    }

    ::SetWindowLongPtrW(impl_->sink, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_->owner));

    if (impl_->options.tickInterval.count() > 0) {
        // Таймер окна — страховка для слитого прогресса: если сигнал о прогрессе
        // потерялся (PostMessage не удался), следующий тик всё равно его отдаст.
        if (::SetTimer(impl_->sink, kBridgeTimerId,
                       static_cast<UINT>(impl_->options.tickInterval.count()),
                       nullptr) == 0) {
            logWin32Failure("ui.mv.sink", "SetTimer(tick)", ::GetLastError());
        }
    }
    logBridge("ui.mv.sink", "мост модель-представление: окно-приёмник создано");
}

void ModelViewBridge::destroySink() noexcept {
    if (impl_->sink == nullptr) return;

    // Сначала снимаем обратный адрес: между KillTimer/DestroyWindow и концом
    // функции мост уже не должен принимать сообщений.
    ::SetWindowLongPtrW(impl_->sink, GWLP_USERDATA, 0);
    impl_->owner = nullptr;
    ::KillTimer(impl_->sink, kBridgeTimerId);

    // Оставшиеся в очереди работы удаляются здесь: см. discardPendingJobs.
    discardPendingJobs(impl_->sink);

    if (::DestroyWindow(impl_->sink) == FALSE) {
        logWin32Failure("ui.mv.sink", "DestroyWindow(sink)", ::GetLastError());
    }
    impl_->sink = nullptr;
}

// ---------------------------------------------------------------------------
// Мост: создание и разрушение
// ---------------------------------------------------------------------------

ModelViewBridge::ModelViewBridge(Options options)
    : ModelViewBridge(nullptr, std::move(options)) {}

ModelViewBridge::ModelViewBridge(std::unique_ptr<Dispatcher> dispatcher, Options options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->counters = std::make_shared<BridgeCounters>();
    impl_->slot = std::make_unique<ProgressSlot>();
    impl_->owner = this;
    // Мост создаётся в UI-потоке по контракту; его идентификатор и есть
    // определение «UI-потока» для всех проверок ниже.
    impl_->uiThreadId = ::GetCurrentThreadId();

    if (dispatcher) {
        impl_->options.createSink = false;
        impl_->dispatcher = std::move(dispatcher);
        logBridge("ui.mv.bridge", "мост с внешним диспетчером (окно-приёмник не создаётся)");
    } else if (impl_->options.createSink) {
        createSink();
        if (impl_->sink != nullptr) {
            impl_->dispatcher =
                std::make_unique<PostMessageDispatcher>(impl_->sink, impl_->uiThreadId);
        }
    }

    if (impl_->dispatcher == nullptr) {
        // Мост остаётся живым объектом, но «мёртвым» для публикации: отказ
        // должен быть явным, чтобы не выглядеть как «интерфейс завис».
        logBridge("ui.mv.bridge", "мост без приёмника — публикация будет отклонена");
    }
}

ModelViewBridge::~ModelViewBridge() {
    if (impl_ == nullptr) return;
    // Мост мёртв для очереди: ни одна оставшаяся работа не дойдёт до
    // наблюдателей. Флаг ставится первым, до снятия окна и освобождения
    // состояния, — порядок здесь и есть защита от гонки с разрушением.
    impl_->counters->alive.store(false, std::memory_order_release);
    if (!onUiThread()) {
        // Разрушение не из UI-потока — ошибка владения, но ронять из-за неё
        // процесс нельзя: снимаем приёмник, а факт пишем в журнал.
        impl_->counters->misthreaded.fetch_add(1, std::memory_order_relaxed);
        core::logError("ui.mv.bridge",
                       "мост уничтожен не из UI-потока — состояние экранов может быть неполным");
    }
    destroySink();
    impl_->dispatcher.reset();
    impl_->observers.clear();
}

bool ModelViewBridge::ready() const noexcept { return impl_ != nullptr && impl_->dispatcher != nullptr; }

void* ModelViewBridge::sinkWindow() const noexcept { return static_cast<void*>(impl_->sink); }

bool ModelViewBridge::onUiThread() const noexcept {
    return impl_ != nullptr && impl_->uiThreadId != 0 && ::GetCurrentThreadId() == impl_->uiThreadId;
}

std::uint32_t ModelViewBridge::uiThreadId() const noexcept {
    return impl_ != nullptr ? impl_->uiThreadId : 0;
}

// ---------------------------------------------------------------------------
// Мост: поток и наблюдатели
// ---------------------------------------------------------------------------

// Проверка «мы в UI-потоке» с журналом. Мост не падает на нарушении: интерфейс
// переживает ошибки вызывающего (§5), а нарушение считается и пишется — молча
// игнорировать его нельзя, это ровно та гонка, ради которой мост существует.
bool ModelViewBridge::claimUiThread(const char* what) const noexcept {
    if (onUiThread()) return true;
    impl_->counters->misthreaded.fetch_add(1, std::memory_order_relaxed);
    core::LogFields fields;
    fields.push_back(core::logField("what", what != nullptr ? what : "unknown"));
    fields.push_back(core::logField("uiThreadId", static_cast<std::int64_t>(impl_->uiThreadId)));
    fields.push_back(core::logField("threadId", static_cast<std::int64_t>(::GetCurrentThreadId())));
    core::logError("ui.mv.thread", "вызов моста сделан не из UI-потока — действие отклонено",
                   std::move(fields));
    return false;
}

bool ModelViewBridge::subscribe(std::shared_ptr<Observer> observer) {
    if (!observer || !claimUiThread("subscribe")) return false;
    if (std::find(impl_->observers.begin(), impl_->observers.end(), observer) != impl_->observers.end()) {
        return true;  // повторная подписка безвредна: два одинаковых вызова дали бы двойную доставку
    }
    if (impl_->dispatching) {
        // Доставка идёт прямо сейчас: список не трогаем, изменение вступит в
        // силу после неё, иначе итерация по вектору развалилась бы.
        impl_->deferredAdd.push_back(std::move(observer));
        return true;
    }
    impl_->observers.push_back(std::move(observer));
    return true;
}

bool ModelViewBridge::unsubscribe(const std::shared_ptr<Observer>& observer) {
    if (!observer || !claimUiThread("unsubscribe")) return false;
    if (impl_->dispatching) {
        impl_->deferredRemove.push_back(observer);
        return true;
    }
    const auto found = std::find(impl_->observers.begin(), impl_->observers.end(), observer);
    if (found == impl_->observers.end()) return false;
    impl_->observers.erase(found);
    return true;
}

void ModelViewBridge::clearObservers() {
    if (!claimUiThread("clearObservers")) return;
    if (impl_->dispatching) {
        impl_->deferredRemove = impl_->observers;
        impl_->deferredRemove.insert(impl_->deferredRemove.end(), impl_->deferredAdd.begin(),
                                     impl_->deferredAdd.end());
        impl_->deferredAdd.clear();
        return;
    }
    impl_->observers.clear();
}

std::size_t ModelViewBridge::observerCount() const noexcept { return impl_->observers.size(); }

void ModelViewBridge::applyObserverChanges() noexcept {
    for (const auto& observer : impl_->deferredRemove) {
        const auto found = std::find(impl_->observers.begin(), impl_->observers.end(), observer);
        if (found != impl_->observers.end()) impl_->observers.erase(found);
    }
    impl_->deferredRemove.clear();
    for (const auto& observer : impl_->deferredAdd) {
        if (std::find(impl_->observers.begin(), impl_->observers.end(), observer) == impl_->observers.end()) {
            impl_->observers.push_back(observer);
        }
    }
    impl_->deferredAdd.clear();
}

// ---------------------------------------------------------------------------
// Мост: публикация
// ---------------------------------------------------------------------------

std::uint64_t ModelViewBridge::nextSequence() noexcept {
    // Монотонный номер кадра. Он нужен представлению, чтобы отличить «тот же
    // прогон, следующий кадр» от «новый прогон», не сравнивая содержимое
    // снимков. fetch_add, а не «прочитал и записал»: два фоновых потока
    // публикуют одновременно.
    return impl_->counters->sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool ModelViewBridge::postEvent(Event event, bool critical) {
    if (impl_ == nullptr) return false;
    if (!ready()) {
        impl_->counters->dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Потолок очереди. Он нужен не «на всякий случай», а по конкретной причине:
    // представление может публиковать кадры чаще, чем UI их успевает разобрать
    // (дёргали «Обновить» много раз подряд). Терминальные кадры потолок обходят:
    // «работа закончилась» потерять нельзя, а снимок при этом всё равно один —
    // следующий прогон принесёт новый.
    const std::uint64_t inFlight = impl_->counters->inFlight.load(std::memory_order_relaxed);
    if (!critical && impl_->options.maxPendingEvents > 0 && inFlight >= impl_->options.maxPendingEvents) {
        impl_->counters->dropped.fetch_add(1, std::memory_order_relaxed);
        logOnce("ui.mv.queue", "очередь кадров переполнена — нетерминальный кадр отброшен",
                impl_->queueWarned);
        return false;
    }

    // Счётчики живут отдельно от моста (см. BridgeCounters): работа приезжает
    // позже и в худшем случае — уже после разрушения моста, и тогда счётчик всё
    // равно должен быть жив, чтобы кадр снялся с учёта.
    auto counters = impl_->counters;
    ModelViewBridge* owner = impl_->owner;
    const std::uint64_t sequence = event.sequence();
    const EventKind kind = event.kind();

    // Работа собирается ДО учёта счётчиков: std::function выделяет память, и
    // обрыв по bad_alloc не должен оставить «кадр в полёте» навсегда.
    std::function<void()> job = [counters, owner, event = std::move(event)]() mutable {
        // Работа приезжает в UI-поток. Флаг alive проверяется ПЕРЕД разыменованием
        // owner: если мост уже разрушен, кадр не доставляем (наблюдателей нет), а
        // обращаться к освобождённому this — прямой путь к упавшему приложению.
        // Счётчик снимается в любом случае.
        if (counters->alive.load(std::memory_order_acquire) && owner != nullptr && owner->ready()) {
            owner->notify(event);
        }
        counters->inFlight.fetch_sub(1, std::memory_order_relaxed);
    };

    counters->inFlight.fetch_add(1, std::memory_order_relaxed);
    counters->posted.fetch_add(1, std::memory_order_relaxed);

    const bool queued = impl_->dispatcher->post(std::move(job));

    if (!queued) {
        impl_->counters->inFlight.fetch_sub(1, std::memory_order_relaxed);
        impl_->counters->dropped.fetch_add(1, std::memory_order_relaxed);
        const auto* posted = dynamic_cast<const PostMessageDispatcher*>(impl_->dispatcher.get());
        if (posted != nullptr && posted->failed() > 0) {
            logWin32Failure("ui.mv.post", "PostMessageW(job)", posted->lastError());
        }
        return false;
    }

    if (impl_->options.logTraffic) {
        core::LogFields fields;
        fields.push_back(core::logField("kind", eventKindName(kind)));
        fields.push_back(core::logField("seq", static_cast<std::int64_t>(sequence)));
        core::logDebug("ui.mv.post", "кадр поставлен в очередь UI-потока", std::move(fields));
    }
    return true;
}

bool ModelViewBridge::postStatus(EventKind kind, StatusFrame frame, bool critical) {
    if (impl_ == nullptr) return false;
    if (frame.atUnix == 0) frame.atUnix = unixNow();
    const std::uint64_t sequence = nextSequence();
    return postEvent(
        Event::snapshot(kind, std::make_shared<const StatusFrame>(std::move(frame)), sequence), critical);
}

bool ModelViewBridge::postStatus(EventKind kind, std::string text, bool critical) {
    StatusFrame frame;
    frame.text = std::move(text);
    frame.ok = kind != EventKind::Error;
    return postStatus(kind, std::move(frame), critical);
}

bool ModelViewBridge::postProgress(ProgressFrame frame) {
    if (impl_ == nullptr) return false;
    if (!ready()) {
        impl_->counters->dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Троттлинг. Обход ФС меняет счётчик на каждом файле, а §6.4 обещает UI чтение
    // раз в 100 мс: публиковать чаще — значит напрасно будить UI-поток и
    // аллоцировать кадр, который никто не увидит. Кадры с известным знаменателем
    // и полной долей троттлингу не подлежат: «работа закончилась» показывать
    // надо сразу.
    const std::int64_t now = nowNanos();
    const std::int64_t previous = impl_->counters->lastProgressPostNs.load(std::memory_order_relaxed);
    const auto interval =
        std::chrono::duration_cast<std::chrono::nanoseconds>(impl_->options.progressInterval);
    const bool finalFrame = !frame.indeterminate() && frame.fraction() >= 1.0;
    if (!finalFrame && previous != 0 && now - previous < interval.count()) {
        impl_->counters->throttled.fetch_add(1, std::memory_order_relaxed);
        return true;  // кадр учтён, показан будет более поздний
    }
    impl_->counters->lastProgressPostNs.store(now, std::memory_order_relaxed);

    frame.sequence = nextSequence();
    auto published = std::make_shared<const ProgressFrame>(std::move(frame));

    // Слияние «последний выигрывает»: промежуточный кадр не несёт информации
    // (счётчики монотонны), а очередь сообщений не должна расти от прогресса.
    bool replaced = false;
    {
        std::lock_guard<std::mutex> guard(impl_->slot->mutex);
        replaced = impl_->slot->frame != nullptr;
        impl_->slot->frame = std::move(published);
    }
    if (replaced) impl_->counters->coalesced.fetch_add(1, std::memory_order_relaxed);
    impl_->counters->posted.fetch_add(1, std::memory_order_relaxed);

    // Работа тика готовится до переключения флага: если std::function не собрался
    // (bad_alloc), кадр останется в слоте, и его заберёт тик по таймеру — лучше
    // опоздание на 100 мс, чем «прогресс пропал». Флаг alive — та же защита от
    // разыменования this в работе, оставшейся в очереди после закрытия окна.
    std::function<void()> tick = [this, counters = impl_->counters]() {
        if (counters->alive.load(std::memory_order_acquire)) pump();
    };

    // Один сигнал на все накопленные кадры: если UI ещё не забрал прогресс,
    // второй сигнал ничего не изменит, а в очередь сообщений ляжет лишний.
    if (impl_->counters->progressWake.exchange(true, std::memory_order_acq_rel)) return true;
    const bool queued = impl_->dispatcher->post(std::move(tick));
    if (!queued) {
        impl_->counters->progressWake.store(false, std::memory_order_release);
        impl_->counters->dropped.fetch_add(1, std::memory_order_relaxed);
        const auto* posted = dynamic_cast<const PostMessageDispatcher*>(impl_->dispatcher.get());
        if (posted != nullptr && posted->failed() > 0) {
            logWin32Failure("ui.mv.progress", "PostMessageW(tick)", posted->lastError());
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Мост: доставка
// ---------------------------------------------------------------------------

void ModelViewBridge::notify(const Event& event) noexcept {
    if (impl_ == nullptr) return;
    if (!onUiThread()) {
        // Сюда попадать нельзя: значит диспетчер выполнил работу не там, где
        // живут контролы. Наблюдателей не трогаем — они не потокобезопасны
        // (§6.1), нарушение считаем и пишем в журнал один раз.
        impl_->counters->misthreaded.fetch_add(1, std::memory_order_relaxed);
        logOnce("ui.mv.thread", "кадр доставлен не из UI-потока — наблюдатели не вызваны",
                impl_->threadWarned);
        return;
    }

    impl_->dispatching = true;
    for (const auto& observer : impl_->observers) {
        if (!observer) continue;
        try {
            observer->onFrame(event);
        } catch (...) {
            // Наблюдатель — код представления, и он не имеет права ронять скан
            // (§5 «устойчивость»). Считаем отказ и идём к следующему.
            impl_->counters->observerErrors.fetch_add(1, std::memory_order_relaxed);
            core::logError("ui.mv.observer", "наблюдатель бросил исключение в onFrame");
        }
    }
    impl_->dispatching = false;
    applyObserverChanges();
    impl_->counters->delivered.fetch_add(1, std::memory_order_relaxed);
}

void ModelViewBridge::deliverProgress() noexcept {
    // Забор слота: ровно один раз за тик и вне какой-либо блокировки у
    // наблюдателей. Кадр уходит им как shared_ptr<const ProgressFrame> — тот же
    // иммутабельный контракт, что и у снимков.
    std::shared_ptr<const ProgressFrame> frame;
    {
        std::lock_guard<std::mutex> guard(impl_->slot->mutex);
        frame = std::move(impl_->slot->frame);
        impl_->slot->frame.reset();
    }
    if (!frame) return;

    if (!onUiThread()) {
        impl_->counters->misthreaded.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    impl_->counters->progressWake.store(false, std::memory_order_release);
    impl_->counters->progressDelivered.fetch_add(1, std::memory_order_relaxed);

    impl_->dispatching = true;
    for (const auto& observer : impl_->observers) {
        if (!observer) continue;
        try {
            observer->onProgress(*frame);
        } catch (...) {
            impl_->counters->observerErrors.fetch_add(1, std::memory_order_relaxed);
            core::logError("ui.mv.observer", "наблюдатель бросил исключение в onProgress");
        }
    }
    impl_->dispatching = false;
    applyObserverChanges();
}

void ModelViewBridge::pump() {
    if (impl_ == nullptr) return;
    if (!onUiThread()) {
        impl_->counters->misthreaded.fetch_add(1, std::memory_order_relaxed);
        logOnce("ui.mv.thread", "pump() вызван не из UI-потока", impl_->threadWarned);
        return;
    }
    pumpInternal();
}

void ModelViewBridge::pumpInternal() noexcept {
    // Слот прогресса забирается независимо от того, остались ли кадры в очереди
    // сообщений: тик по таймеру существует именно ради этого случая.
    deliverProgress();
    if (impl_->observers.empty()) return;

    // onTick зовётся только когда что-то доехало. Тик раз в 100 мс сам по себе не
    // повод будить представление: пустой тик не должен ни перерисовывать окно,
    // ни пересчитывать агрегаты.
    const std::uint64_t total = impl_->counters->delivered.load(std::memory_order_relaxed) +
                                impl_->counters->progressDelivered.load(std::memory_order_relaxed);
    if (total == impl_->tickBaseline) return;
    impl_->tickBaseline = total;

    const BridgeStats current = stats();
    for (const auto& observer : impl_->observers) {
        if (!observer) continue;
        try {
            observer->onTick(current);
        } catch (...) {
            impl_->counters->observerErrors.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// Мост: счётчики
// ---------------------------------------------------------------------------

std::uint64_t ModelViewBridge::pending() const noexcept {
    return impl_->counters->inFlight.load(std::memory_order_relaxed);
}

void ModelViewBridge::resetStats() noexcept {
    auto& counters = *impl_->counters;
    counters.posted.store(0, std::memory_order_relaxed);
    counters.delivered.store(0, std::memory_order_relaxed);
    counters.dropped.store(0, std::memory_order_relaxed);
    counters.throttled.store(0, std::memory_order_relaxed);
    counters.coalesced.store(0, std::memory_order_relaxed);
    counters.progressDelivered.store(0, std::memory_order_relaxed);
    counters.misthreaded.store(0, std::memory_order_relaxed);
    counters.observerErrors.store(0, std::memory_order_relaxed);
}

BridgeStats ModelViewBridge::stats() const noexcept {
    const auto& counters = *impl_->counters;
    BridgeStats out;
    out.posted = counters.posted.load(std::memory_order_relaxed);
    out.delivered = counters.delivered.load(std::memory_order_relaxed);
    out.dropped = counters.dropped.load(std::memory_order_relaxed);
    out.throttled = counters.throttled.load(std::memory_order_relaxed);
    out.coalesced = counters.coalesced.load(std::memory_order_relaxed);
    out.progressDelivered = counters.progressDelivered.load(std::memory_order_relaxed);
    out.misthreaded = counters.misthreaded.load(std::memory_order_relaxed);
    out.observerErrors = counters.observerErrors.load(std::memory_order_relaxed);
    out.sinkCreated = impl_->sink != nullptr ? 1u : 0u;
    return out;
}

std::string ModelViewBridge::toText() const {
    std::string out = stats().toText();
    out.append("uiThreadId=");
    appendNumber(out, static_cast<std::uint64_t>(impl_->uiThreadId));
    out.append(", наблюдателей ");
    appendNumber(out, static_cast<std::uint64_t>(impl_->observers.size()));
    return out;
}

// ---------------------------------------------------------------------------
// Стартовая раздача: ScreenEndpoint и StartupFeed
// ---------------------------------------------------------------------------
//
// Проектные решения — в шапке (mv_bridge.hpp). Здесь три вещи, которых нет в
// мосте самом: очередь кадров у экрана, фоновый поток и чтение набора правил с
// диска. Мост они не меняют: кадр по-прежнему уходит PostMessage'ом в окно
// UI-потока, и по-прежнему его исполняет UI-поток.
namespace {

void feedLog(core::LogLevel level, std::string_view event, std::string_view message) noexcept {
    core::Logger::instance().write(level, event, message, core::LogFields{});
}

void feedLogField(core::LogLevel level, std::string_view event, std::string_view message,
                  std::string_view name, std::string_view value) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField(std::string(name), std::string(value)));
    core::Logger::instance().write(level, event, message, std::move(fields));
}

// UTF-16 → UTF-8 без суррогатных сюрпризов. Своя обёртка нужна потому, что в
// файле нет ui::locale (мост не знает про локализацию), а std::filesystem на
// Windows отдаёт пути в UTF-16.
std::string toUtf8Path(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr,
                          nullptr);
    return utf8;
}

// «NAME=value\n» для подстановки переменных в правила (%LOCALAPPDATA% и подобное
// внутри locator'ов). Тот же дамп, что у CLI и у rulesync-клиента: свой здесь
// означал бы, что правило разрешается на экране иначе, чем в отчёте.
std::string environmentDumpUtf8() {
    LPWCH block = ::GetEnvironmentStringsW();
    if (block == nullptr) return {};
    std::string out;
    for (const wchar_t* cursor = block; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1u) {
        const std::wstring_view entry(cursor);
        // Скрытые переменные вида «=C:=C:\» начинаются с «=» и для правил
        // бесполезны; пропускаем их, как это делает platform::rulesync_client.
        const std::size_t equals = entry.find(L'=');
        if (equals == std::wstring_view::npos || equals == 0) continue;
        out += toUtf8Path(std::wstring(entry.substr(0, equals)));
        out.push_back('=');
        out += toUtf8Path(std::wstring(entry.substr(equals + 1u)));
        out.push_back('\n');
    }
    ::FreeEnvironmentStringsW(block);
    return out;
}

// Сколько уровней вверх от каталога экземпляра искать набор правил. Пять с
// запасом: build\a2\src\ui\Debug (отладочная раскладка) — уже четыре.
inline constexpr int kRuleSearchLevels = 6;

std::wstring moduleDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) return {};
    buffer.resize(written);
    const std::size_t slash = buffer.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return buffer.substr(0, slash);
}

std::wstring environmentDirectory() {
    const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (written == 0) return {};
    std::wstring buffer(written, L'\0');
    const DWORD filled = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), written);
    if (filled == 0 || filled >= buffer.size()) return {};
    buffer.resize(filled);
    return buffer;
}

struct RuleLoad {
    core::RuleSet set;
    std::string origin;   // каталог, из которого взят набор
    std::string version;  // версия набора (из манифеста или из правил)
    std::string problem;  // непусто, если набор не прочитан
};

// Каталоги, где набор правил может лежать, в порядке убывания правды.
//
//   1) %LOCALAPPDATA%\MrProper\rules — сюда rulesync-клиент кладёт обновлённый
//      набор (ADR-008); если человек обновлял правила, показывать надо их, а не
//      те, что лежат рядом с программой;
//   2) <каталог экземпляра>\rules — отладочная и портативная раскладка;
//   3) вверх по дереву: share\MrProper\rules (установленная раскладка, §8) и
//      снова rules (репозиторий: build\a2\src\ui\Debug → корень).
std::vector<std::filesystem::path> ruleSearchPaths() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    const std::wstring local = environmentDirectory();
    if (!local.empty()) candidates.push_back(fs::path(local) / L"MrProper" / L"rules");

    std::wstring dir = moduleDirectory();
    for (int level = 0; level <= kRuleSearchLevels && !dir.empty(); ++level) {
        candidates.push_back(fs::path(dir) / L"rules");
        candidates.push_back(fs::path(dir) / L"share" / L"MrProper" / L"rules");
        const std::size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        dir = dir.substr(0, slash);
    }
    return candidates;
}

// Сколько *.json в каталоге. Ноль — каталога нет или набора в нём нет.
std::size_t countRuleFiles(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    std::error_code code;
    if (!fs::is_directory(directory, code)) return 0;
    std::size_t count = 0;
    for (fs::directory_iterator it(directory, code), end; !code && it != end; it.increment(code)) {
        if (!it->is_regular_file(code)) continue;
        if (it->path().extension() == L".json") ++count;
    }
    return count;
}

// Чтение набора тем же способом, что и CLI (cmd_scan.cpp loadRuleSetFromDirectory):
// порядок файлов фиксирован сортировкой, манифест опознаётся разбором, а не
// именем. Свой разбор здесь означал бы, что экран «Настройки» и отчёт считают
// разные правила (§11.4 — цифры и правила на всех поверхностях одни).
RuleLoad loadRuleSetFromDisk() {
    namespace fs = std::filesystem;
    RuleLoad out;
    std::vector<fs::path> files;
    for (const fs::path& candidate : ruleSearchPaths()) {
        if (countRuleFiles(candidate) == 0) continue;
        std::error_code code;
        for (fs::directory_iterator it(candidate, code), end; !code && it != end; it.increment(code)) {
            if (it->is_regular_file(code) && it->path().extension() == L".json") files.push_back(it->path());
        }
        out.origin = toUtf8Path(candidate.wstring());
        break;
    }
    if (out.origin.empty()) {
        out.problem = "каталог набора правил не найден (искали рядом с программой и в %LOCALAPPDATA%)";
        return out;
    }

    std::sort(files.begin(), files.end());
    std::vector<std::pair<std::string, std::string>> texts;
    texts.reserve(files.size());
    for (const fs::path& path : files) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            out.problem = "файл правил не читается: " + toUtf8Path(path.wstring());
            return out;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string text = buffer.str();
        const std::string name = toUtf8Path(path.filename().wstring());
        try {
            out.version = core::parseRuleSetManifest(text, name).version;
            continue;  // манифест, а не правило
        } catch (const core::RuleSyncError&) {
            // Не манифест: пойдёт в набор, разберётся загрузчик.
        }
        texts.emplace_back(name, text);
    }
    if (texts.empty()) {
        out.problem = "в каталоге набора нет ни одного файла правил";
        return out;
    }

    try {
        out.set = core::loadRuleFiles(texts, environmentDumpUtf8(), nullptr);
        core::validateRuleSet(out.set);
    } catch (const std::exception& failure) {
        out.problem = failure.what();
        return out;
    }
    if (out.version.empty()) out.version = out.set.version;
    return out;
}

}  // namespace

// --- ScreenEndpoint ---------------------------------------------------------

ScreenEndpoint::ScreenEndpoint(void* window, std::uint32_t message) noexcept
    : window_(window), message_(message) {}

ScreenEndpoint::~ScreenEndpoint() = default;

bool ScreenEndpoint::take(Event& out) {
    if (window_ == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.erase(queue_.begin());
    return true;
}

std::uint64_t ScreenEndpoint::received() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
}

std::uint64_t ScreenEndpoint::lost() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return lost_;
}

void ScreenEndpoint::onFrame(const Event& event) {
    // Кадр кладём в очередь ДО PostMessage: иначе сообщение может обработаться
    // раньше, чем кадр окажется в очереди, и экран показал бы старое состояние.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++received_;
        // Больше двух кадров ждать бессмысленно: показывается последний, а
        // переполнение очереди — это уже признак того, что экран не читает
        // кадры вообще (лог об этом скажет).
        if (queue_.size() >= 8) {
            queue_.erase(queue_.begin());
            ++lost_;
        }
        queue_.push_back(event);
    }
    if (window_ == nullptr) return;
    if (::PostMessageW(static_cast<HWND>(window_), message_, 0, 0) == FALSE) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++lost_;
        const unsigned long code = ::GetLastError();
        feedLogField(core::LogLevel::Warn, "ui.feed.post", "кадр не доставлен окну экрана", "code",
                     std::to_string(code));
    }
}

void ScreenEndpoint::onProgress(const ProgressFrame& /*frame*/) {
    // Прогресса раздача не публикует: на старте идёт обход дисков и чтение
    // правил, а не скан. Когда скан появится, у него будет свой владелец.
}

void ScreenEndpoint::onTick(const BridgeStats& /*stats*/) {}

// --- StartupFeed ------------------------------------------------------------

struct StartupFeed::Impl {
    std::unique_ptr<ModelViewBridge> bridge;
    std::vector<std::shared_ptr<ScreenEndpoint>> endpoints;
    std::mutex mutex;  // endpoints и снимки

    std::thread worker;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

    std::shared_ptr<const core::DiskInventory> inventory;
    std::shared_ptr<const core::RuleSet> rules;
    std::string rulesOrigin;
    std::string rulesProblem;
    std::string inventoryProblem;
    std::size_t diskCount{0};
    std::uint64_t unavailable{0};

    // Фоновый поток. Останавливается только в деструкторе раздачи: обход
    // дисков нельзя прерывать на полпути, оставляя экран с половиной карты.
    void run() noexcept;
    void publishInventory();
    void publishRules(const RuleLoad& load);
};

void StartupFeed::Impl::run() noexcept {
    // Порядок осознанный. Правила читаются быстро (десятки мелких файлов) и нужны
    // двум экранам сразу; обход дисков — это IOCTL с таймаутом 2 с на
    // устройство, то есть секунды. Начать с правил — значит, что «Настройки» и
    // «Очистка» наполняются, пока «Диски» ещё ждут устройства.
    const RuleLoad load = loadRuleSetFromDisk();
    publishRules(load);
    publishInventory();
    finished.store(true, std::memory_order_release);
}

void StartupFeed::Impl::publishRules(const RuleLoad& load) {
    if (load.problem.empty()) {
        auto set = std::make_shared<const core::RuleSet>(load.set);
        {
            std::lock_guard<std::mutex> lock(mutex);
            rules = set;
            rulesOrigin = load.origin;
            rulesProblem.clear();
        }
        if (bridge) {
            (void)bridge->postSnapshot(EventKind::RuleSet, set, true);
        }
        feedLogField(core::LogLevel::Info, "ui.feed.rules", "набор правил прочитан", "origin", load.origin);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        rulesProblem = load.problem;
    }
    feedLogField(core::LogLevel::Warn, "ui.feed.rules", "набор правил не прочитан", "problem", load.problem);
    // Отказ виден на экране «Настройки» строкой состояния, а не пустым списком:
    // §5 — «каждый отказ виден и записан».
    if (bridge) {
        (void)bridge->postStatus(EventKind::Error, std::string("Набор правил не прочитан: ") + load.problem, true);
    }
}

void StartupFeed::Impl::publishInventory() {
    const platform::inventory::Options options;
    const std::shared_ptr<const platform::inventory::Snapshot> snapshot =
        platform::inventory::collect(options, platform::inventory::RefreshReason::Startup, nullptr);
    if (snapshot == nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        inventoryProblem = "обход дисков не вернул снимок";
        return;
    }

    const core::DiskInventory& map = snapshot->inventory;
    auto published = std::make_shared<const core::DiskInventory>(map);
    std::string problem;
    if (!snapshot->hasDiskData()) {
        // Пустая карта и «карту не удалось прочитать» — разные ситуации, и
        // подпись на экране обязана их различать: первое — «устройств нет»,
        // второе — «нужны права администратора» (§5, §10).
        problem = "устройства не прочитаны: \\.\\PhysicalDriveN открывается только с повышенными правами — "
                  "запустите MrProper от имени администратора";
    } else if (snapshot->unavailableCount() > 0) {
        problem = "часть устройств не ответила за таймаут: показаны только те, что ответили";
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        inventory = published;
        inventoryProblem = problem;
        diskCount = map.disks().size();
        unavailable = snapshot->unavailableCount();
    }
    if (bridge) {
        (void)bridge->postSnapshot(EventKind::Inventory, published, true);
        if (!problem.empty()) (void)bridge->postStatus(EventKind::Notice, problem, false);
    }
    feedLogField(core::LogLevel::Info, "ui.feed.inventory", "инвентаризация опубликована", "disks",
                 std::to_string(map.disks().size()));
}

StartupFeed::StartupFeed() : impl_(std::make_unique<Impl>()) {}

// Разрушение раздачи — на выходе из процесса, в UI-потоке. Фоновый поток здесь
// обязателен к присоединению: ~thread на присоединяемом потоке вызывает
// std::terminate (а не «тихо отваливается»), то есть закрытие окна кончалось бы
// аварийным кодом — ровно тот отказ, который ловит ui-smoke.ps1 как «процесс
// упал». Обход дисков не прерывается на полпути: оснастка присоединяется целиком,
// а опубликованные снимки к этому моменту уже никому не нужны.
StartupFeed::~StartupFeed() {
    if (impl_ && impl_->worker.joinable()) impl_->worker.join();
}

StartupFeed& StartupFeed::instance() noexcept {
    // Функциональный статик: инициализация потокобезопасна начиная с C++11, а
    // экземпляр обязан быть один на процесс (см. шапку).
    static StartupFeed feed;
    return feed;
}

std::shared_ptr<ScreenEndpoint> StartupFeed::subscribe(void* window, std::uint32_t message) {
    if (window == nullptr) return nullptr;
    auto endpoint = std::make_shared<ScreenEndpoint>(window, message);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->bridge == nullptr) {
        ModelViewBridge::Options options;
        options.ownerWindow = window;
        options.createSink = true;
        options.logTraffic = false;
        impl_->bridge = std::make_unique<ModelViewBridge>(std::move(options));
    }
    if (impl_->bridge) {
        // Наблюдатель остаётся жить, пока экран его не отпишет: мост держит
        // только shared_ptr, и молчаливый отзыв потерял бы кадры на ровном
        // месте — при пересоздании экрана.
        (void)impl_->bridge->subscribe(endpoint);
    }
    impl_->endpoints.push_back(endpoint);
    return endpoint;
}

void StartupFeed::unsubscribe(const std::shared_ptr<ScreenEndpoint>& endpoint) noexcept {
    if (!endpoint) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->endpoints.begin(); it != impl_->endpoints.end(); ++it) {
        if (*it == endpoint) {
            impl_->endpoints.erase(it);
            break;
        }
    }
    if (impl_->bridge) (void)impl_->bridge->unsubscribe(endpoint);
}

void StartupFeed::start() noexcept {
    if (impl_->started.exchange(true, std::memory_order_acq_rel)) return;
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    } catch (const std::exception& failure) {
        // Фоновый поток не поднялся — приложение обязано работать и без него:
        // экраны покажут состояние «данных ещё нет» с пояснением (§5).
        feedLogField(core::LogLevel::Error, "ui.feed.start", "фоновый поток не создан", "problem", failure.what());
    }
}

std::shared_ptr<const core::DiskInventory> StartupFeed::inventory() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->inventory;
}

std::shared_ptr<const core::RuleSet> StartupFeed::ruleSet() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->rules;
}

std::string StartupFeed::toText() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string out = "feed: дисков=";
    out += std::to_string(impl_->diskCount);
    out += ", не ответило=";
    out += std::to_string(impl_->unavailable);
    out += ", правила=";
    if (impl_->rules) {
        out += std::to_string(impl_->rules->rules.size());
        out += " верс.";
        out += impl_->rules->version;
        if (!impl_->rulesOrigin.empty()) {
            out += " из ";
            out += impl_->rulesOrigin;
        }
    } else if (!impl_->rulesProblem.empty()) {
        out += "не прочитаны (";
        out += impl_->rulesProblem;
        out += ')';
    } else {
        out += "ещё читаются";
    }
    out += ", экранов=";
    out += std::to_string(impl_->endpoints.size());
    out += impl_->finished.load(std::memory_order_acquire) ? ", фон завершён" : ", фон идёт";
    return out;
}

}  // namespace mrproper::ui::mv
