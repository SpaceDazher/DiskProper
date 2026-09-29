// Реализация size_probe: единственный файл модуля, где встречается windows.h.
// Всё Win32-специфичное — здесь; наружу (size_probe.hpp) уходят только
// переносимые типы, чтобы потребители не тащили windows.h в свои заголовки.
//
// Слои работы (SPEC §4 FR-1 п.3 и п.6):
//   1) открыть дескриптор устройства (RAII, права по спецификации);
//   2) выполнить Win32-вызов в отдельном потоке;
//   3) подождать результат не дольше таймаута устройства;
//   4) на отказ или таймаут вернуть «недоступно» с кодом Win32 и записью в лог.
//
// Спека §4 FR-1: «устойчивость к "отказавшим" дискам (таймаут 2 с на
// устройство, устройство помечается недоступным, приложение не падает)».

#include "size_probe.hpp"

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "core/log.hpp"
#include "devices.hpp"

namespace mrproper::platform {
namespace {

// Вызовы, которые не уложились в таймаут: за ними остались потоки внутри
// драйвера. Счётчик — только для отчёта и отладки, на поведение не влияет.
std::atomic<std::uint32_t> gAbandonedCalls{0};

// INVALID_HANDLE_VALUE — это ((HANDLE)(LONG_PTR)-1), то есть приведение
// литерала, а не constant expression. Поэтому объявляем константой: с
// constexpr компилятор отказывает, а обычный const вполне годится.
const HANDLE kNoHandle = INVALID_HANDLE_VALUE;

// --- RAII для HANDLE (SPEC §9.1 ADR-001: RAII для HANDLE) ----------------

// Единственный владелец дескриптора в модуле: закрывает в деструкторе, поэтому
// путь «открыли — запросили — вышли» не оставляет дескриптор висеть ни при
// ошибке, ни при исключении.
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != kNoHandle && handle_ != nullptr; }

    HANDLE release() noexcept {
        const HANDLE taken = handle_;
        handle_ = kNoHandle;
        return taken;
    }

    void reset(HANDLE handle = kNoHandle) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{kNoHandle};
};

// --- Код ошибки Win32 → состояние опроса -----------------------------------

ProbeStatus classify(DWORD error) noexcept {
    switch (error) {
        case ERROR_SUCCESS:
            return ProbeStatus::Ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NO_MORE_FILES:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_DEV_NOT_EXIST:
            return ProbeStatus::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_SHARING_VIOLATION:
            return ProbeStatus::AccessDenied;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return ProbeStatus::Unsupported;
        default:
            return ProbeStatus::Unavailable;
    }
}

// --- Внутренний результат вызова ------------------------------------------

// Значение + состояние + код Win32. Отдельный тип от публичных результатов:
// те включают ещё и замер времени, который считает уже внешний слой.
template <typename T>
struct CallResult {
    T value{};
    ProbeStatus status{ProbeStatus::Unavailable};
    std::uint32_t win32Error{};
};

// --- Вызов под границей времени (таймаут на устройство) -------------------

template <typename T>
struct CallSlot {
    std::mutex mutex;
    std::condition_variable ready;
    bool done{false};
    CallResult<T> value{};
};

// Вызывает fn в отдельном потоке и ждёт результат не дольше timeout.
// std::nullopt означает ровно одно — «вызов не уложился в срок», то есть
// устройство не отвечает и вызывающий должен пометить его недоступным
// (SPEC §4 FR-1, §10). Поток при этом не прерывается: он убирает своё
// состояние сам, когда вызов вернётся, поэтому у вызывающего не остаётся ни
// висящих дескрипторов, ни обращения к освобождённой памяти.
template <typename T, typename Fn>
std::optional<CallResult<T>> runWithDeadline(std::chrono::milliseconds timeout, Fn&& fn) noexcept {
    if (timeout <= std::chrono::milliseconds::zero()) {
        // timeout <= 0: вызывающий явно отказался от ожидания, вызов идёт в его
        // потоке, и зависание ничем не ограничено.
        try {
            return fn();
        } catch (...) {
            return CallResult<T>{};  // наружу исключения не выпускаем (FR-1)
        }
    }

    auto slot = std::make_shared<CallSlot<T>>();
    try {
        // fn копируется в поток, а вместе с ним и его данные: у вызывающего
        // может быть timeout <= 0 или мёртвое устройство, и после возврата из
        // функции ссылка на string_view вызывающего уже недействительна.
        std::thread worker([slot, fn = std::forward<Fn>(fn)]() {
            CallResult<T> result{};
            try {
                result = fn();
            } catch (...) {
                // Единственный выход наружу — «недоступно» с нулевым кодом:
                // причина не в Win32, а в нехватке памяти внутри вызова.
            }
            {
                // Присваивание ниже не бросает: CallResult — POD, копирование
                // не выделяет памяти.
                const std::lock_guard<std::mutex> lock(slot->mutex);
                slot->value = result;
                slot->done = true;
            }
            slot->ready.notify_one();
        });
        // Отсоединён намеренно: если вызов не уложился в срок, ждать поток
        // нельзя — иначе «мёртвый» диск снова валит приложение.
        worker.detach();
    } catch (const std::system_error&) {
        // Поток не создался (исчерпан лимит): это не таймаут, а нехватка
        // ресурса, и код ошибки должен говорить именно об этом.
        CallResult<T> failure{};
        failure.win32Error = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
        return failure;
    }

    std::unique_lock<std::mutex> lock(slot->mutex);
    if (!slot->ready.wait_for(lock, timeout, [&slot] { return slot->done; })) {
        gAbandonedCalls.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    return slot->value;
}

// --- Собственно запросы (SPEC §4 FR-1 п.3 и п.6) ---------------------------

UniqueHandle openDisk(std::wstring_view path, DWORD access) {
    const std::wstring target(path);
    // FILE_SHARE_READ|FILE_SHARE_WRITE обязателен: без них драйвер откажет,
    // пока диском параллельно пользуется система, а это обычное дело.
    return UniqueHandle(::CreateFileW(target.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, 0, nullptr));
}

// Размер физического диска: IOCTL_DISK_GET_LENGTH_INFO на «\\.\PhysicalDriveN».
CallResult<std::uint64_t> readDiskLength(const std::wstring& path) {
    CallResult<std::uint64_t> result{};

    // Права по SPEC §4 FR-1 п.3: GENERIC_READ|GENERIC_WRITE.
    UniqueHandle handle = openDisk(path, GENERIC_READ | GENERIC_WRITE);
    DWORD error = handle.valid() ? ERROR_SUCCESS : ::GetLastError();

    if (!handle.valid() && error == ERROR_ACCESS_DENIED) {
        // Повышенных прав у процесса может не быть (SPEC §5: повышение — один
        // раз на старте), а для IOCTL длины хватает чтения. Отказ по правам не
        // равен «диск недоступен», поэтому это вторая, best-effort попытка.
        handle = openDisk(path, GENERIC_READ);
        error = handle.valid() ? ERROR_SUCCESS : ::GetLastError();
    }

    if (!handle.valid()) {
        result.status = classify(error);
        result.win32Error = error;
        return result;
    }

    GET_LENGTH_INFORMATION info{};
    DWORD returned = 0;
    if (!::DeviceIoControl(handle.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &info, sizeof info, &returned,
                           nullptr) ||
        returned < static_cast<DWORD>(sizeof info)) {
        error = ::GetLastError();
        if (error == ERROR_SUCCESS) {
            // Вызов «успешен», а вывод короче структуры — так ведёт себя битый
            // драйвер; настоящего кода ошибки нет, поэтому подставляем свой.
            error = ERROR_INVALID_DATA;
        }
        result.status = classify(error);
        result.win32Error = error;
        return result;
    }

    if (info.Length.QuadPart < 0) {
        // QuadPart знаковый (LONGLONG), отрицательной длины диска не бывает.
        result.status = ProbeStatus::InvalidArgument;
        result.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_DATA);
        return result;
    }

    result.value = static_cast<std::uint64_t>(info.Length.QuadPart);
    result.status = ProbeStatus::Ok;
    return result;
}

// Свободное место: GetDiskFreeSpaceExW по пути тома, буквы или каталога.
CallResult<VolumeSpace> readVolumeSpace(const std::wstring& rootPath) {
    CallResult<VolumeSpace> result{};

    ULARGE_INTEGER freeToCaller{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER freeTotal{};
    if (!::GetDiskFreeSpaceExW(rootPath.c_str(), &freeToCaller, &total, &freeTotal)) {
        const DWORD error = ::GetLastError();
        result.status = classify(error);
        result.win32Error = error;
        return result;
    }

    result.value.totalBytes = total.QuadPart;
    result.value.freeBytesTotal = freeTotal.QuadPart;
    result.value.freeBytesToCaller = freeToCaller.QuadPart;
    result.status = ProbeStatus::Ok;
    return result;
}

std::chrono::milliseconds elapsedSince(std::chrono::steady_clock::time_point started) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
}

// Отказ устройства — в лог с путём и кодом Win32 (SPEC §12: «все ошибки в логе
// с путём и HRESULT»). Само сообщение и состояние собирает formatProbeError,
// здесь важны структурированные поля. Функция не noexcept: сборка полей
// выделяет память, и вызывающий ловит это отдельно — логирование не имеет
// права ронять опрос (SPEC §5).
void logUnavailable(std::string_view event, std::wstring_view path, ProbeStatus status, std::uint32_t win32Error,
                    std::chrono::milliseconds elapsed, std::chrono::milliseconds timeout) {
    // Поля собираются явно, а не макросом MRP_LOG_FAILURE: набор разнородный
    // (строки и числа), и разворачивание пакета в core::detail::logFieldList на
    // MSVC v142 спотыкается на перегрузках logField. Формат записи тот же —
    // путь и код Win32 logFailure ставит первыми сам.
    core::LogFields fields;
    fields.push_back(core::logField("errorDomain", "win32"));
    fields.push_back(core::logField("status", toString(status)));
    fields.push_back(core::logField("elapsedMs", static_cast<long long>(elapsed.count())));
    fields.push_back(core::logField("timeoutMs", static_cast<long long>(timeout.count())));
    core::logFailure(event, "устройство помечено недоступным: получить размер и/или свободное место не удалось",
                     core::toUtf8(path), static_cast<std::int64_t>(win32Error), std::move(fields));
}

}  // namespace

const wchar_t* toString(ProbeStatus status) noexcept {
    switch (status) {
        case ProbeStatus::Ok:
            return L"ok";
        case ProbeStatus::TimedOut:
            return L"timeout";
        case ProbeStatus::AccessDenied:
            return L"access-denied";
        case ProbeStatus::NotFound:
            return L"not-found";
        case ProbeStatus::Unsupported:
            return L"unsupported";
        case ProbeStatus::InvalidArgument:
            return L"invalid-argument";
        case ProbeStatus::Unavailable:
            break;
    }
    return L"unavailable";
}

std::wstring formatProbeError(ProbeStatus status, std::uint32_t win32Error) {
    if (status == ProbeStatus::Ok) {
        return {};
    }
    std::wstring text = toString(status);
    if (win32Error == 0) {
        return text;
    }

    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        win32Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length != 0 && buffer != nullptr) {
        std::wstring systemText(buffer, length);
        // FormatMessageW завершает текст переводом строки — в одну строку он
        // не нужен, а в лог и JSON попадать не должен.
        while (!systemText.empty() && (systemText.back() == L'\r' || systemText.back() == L'\n')) {
            systemText.pop_back();
        }
        if (!systemText.empty()) {
            text += L": ";
            text += systemText;
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return text;
}

ProbeTimeoutStats probeTimeoutStats() noexcept {
    ProbeTimeoutStats stats{};
    stats.abandonedCalls = gAbandonedCalls.load(std::memory_order_relaxed);
    return stats;
}

DiskSizeResult queryDiskSize(int diskNumber, std::chrono::milliseconds timeout) noexcept {
    DiskSizeResult result{};
    if (diskNumber < 0) {
        result.status = ProbeStatus::InvalidArgument;
        result.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER);
        return result;
    }
    try {
        return queryDiskSize(physicalDrivePath(diskNumber), timeout);
    } catch (const std::bad_alloc&) {
        result.status = ProbeStatus::Unavailable;
        result.win32Error = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
        return result;
    }
}

DiskSizeResult queryDiskSize(std::wstring_view devicePath, std::chrono::milliseconds timeout) noexcept {
    DiskSizeResult result{};
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    try {
        // Копия пути нужна и потоку, и логу: она переживает и таймаут, и
        // возврат из функции (string_view вызывающего к тому моменту мёртв).
        const std::wstring path(devicePath);
        if (path.empty()) {
            result.status = ProbeStatus::InvalidArgument;
            result.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER);
        } else {
            const std::optional<CallResult<std::uint64_t>> call =
                runWithDeadline<std::uint64_t>(timeout, [path] { return readDiskLength(path); });
            if (call) {
                result.lengthBytes = call->value;
                result.status = call->status;
                result.win32Error = call->win32Error;
            } else {
                // Вызов не уложился: устройство не отвечает, отдаём «недоступно»
                // с кодом ожидания, чтобы по логу было видно, откуда причина.
                // Именно ERROR_TIMEOUT (1460), а не WAIT_TIMEOUT (258): второе —
                // WAIT-результат WaitForSingleObject, в каталоге ERROR_ его нет,
                // и FormatMessageW для 258 не находит текста. Так же поступают
                // devices, volumes, trim_cache и storage_query.
                result.status = ProbeStatus::TimedOut;
                result.win32Error = static_cast<std::uint32_t>(ERROR_TIMEOUT);
            }
        }
    } catch (const std::bad_alloc&) {
        // Единственное, что здесь может вылететь, — нехватка памяти на копии
        // пути. Наружу не пускаем: FR-1 требует, чтобы приложение не падало.
        result = DiskSizeResult{};
        result.status = ProbeStatus::Unavailable;
        result.win32Error = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
    }

    result.elapsed = elapsedSince(started);
    if (!result.ok()) {
        try {
            logUnavailable("size_probe.disk_length", devicePath, result.status, result.win32Error, result.elapsed,
                           timeout);
        } catch (...) {
            // Нехватка памяти при записи в лог — не повод потерять результат
            // опроса и не повод уронить процесс.
        }
    }
    return result;
}

VolumeSpaceResult queryVolumeSpace(std::wstring_view rootPath, std::chrono::milliseconds timeout) noexcept {
    VolumeSpaceResult result{};
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    try {
        const std::wstring path(rootPath);
        if (path.empty()) {
            result.status = ProbeStatus::InvalidArgument;
            result.win32Error = static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER);
        } else {
            const std::optional<CallResult<VolumeSpace>> call =
                runWithDeadline<VolumeSpace>(timeout, [path] { return readVolumeSpace(path); });
            if (call) {
                result.space = call->value;
                result.status = call->status;
                result.win32Error = call->win32Error;
            } else {
                result.status = ProbeStatus::TimedOut;
                result.win32Error = static_cast<std::uint32_t>(ERROR_TIMEOUT);
            }
        }
    } catch (const std::bad_alloc&) {
        result = VolumeSpaceResult{};
        result.status = ProbeStatus::Unavailable;
        result.win32Error = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
    }

    result.elapsed = elapsedSince(started);
    if (!result.ok()) {
        try {
            logUnavailable("size_probe.volume_space", rootPath, result.status, result.win32Error, result.elapsed,
                           timeout);
        } catch (...) {
            // Как и выше: сбой записи в лог не влияет на результат опроса.
        }
    }
    return result;
}

}  // namespace mrproper::platform
