// Закрытие процессов по запросу пользователя — реализация. Спека и границы
// модуля описаны в process_control.hpp; здесь комментарии к решениям, которые
// из сигнатуры не видны.
//
// Слой Win32 (SPEC §6.1, ADR-004): единственное место проекта, где допустим
// windows.h. Заголовочный файл SDK называется RestartManager.h (не rstrtmgr.h,
// как указано в некоторых источниках), и подключать его нужно именно так.
//
// Три факта о Restart Manager, на которых держится модуль и которые отличаются от
// того, что написано в MSDN:
//
// 1. RmShutdown в SDK принимает ТРИ аргумента:
//        DWORD RmShutdown(DWORD hSessionHandle, ULONG lActionFlags,
//                         RM_WRITE_STATUS_CALLBACK fnStatus);
//    Двухаргументная форма (dwSessionHandle, dwForceShutdown) из MSDN в заго-
//    ловках отсутствует, и код, написанный по MSDN, не компилируется. Второй
//    аргумент — не BOOL, а флаги RM_SHUTDOWN_TYPE: RmForceShutdown (0x1) и
//    RmShutdownOnlyRegistered (0x10). Второй флаг здесь неприменим: он просит
//    закрывать только те приложения, которые зарегистрированы для перезапуска,
//    а мы перезапуск не регистрируем, поэтому при нуле «мягко» приложение
//    получает WM_CLOSE и может спросить пользователя о несохранённом.
//
// 2. Процесс для RM — это пара (PID, время создания), а не PID. Без реального
//    ProcessStartTime переиспользованный PID указывает на чужой процесс, и
//    закрывается не то приложение. Поэтому время запуска читается всегда —
//    раньше, чем процесс попадёт в предпросмотр.
//
// 3. RmShutdown не гарантирует, что процесс завершился: это команда, а не
//    факт. Приложение могло зависнуть на своём обработчике сохранения. Файл
//    после RmShutdown может быть всё ещё занят, поэтому модуль ждёт сигнала от
//    дескриптора процесса и только после этого пишет Closed.
//
// Почему подтверждение встроено в контракт, а не передано на волю вызывающего.
// Очистка запускается одним нажатием, и «закрыть эти приложения» — единственная
// операция модуля, которая задевает то, что пользователь запускал сам и в чём у
// него могут быть несохранённые данные. Если бы подтверждение было необязательным
// параметром, любой забывший call-site тихо закрыл бы браузер пользователя. Здесь
// вызова без confirm не существует: результат NotConfirmed, журнал с записью
// «подтверждение не получено».
//
// Почему системные процессы защищены именем, а не флагом. Список (см. kProtected)
// отвечает на вопрос «что нельзя закрывать никогда», и настройка такого списка —
// приглашение однажды её снять. Снятие невозможно, потому что флага нет; второй
// заслон — сам Restart Manager: он возвращает RmCritical для процессов, закрывать
// которые нельзя, и такие записи выбрасываются из пачки до RmShutdown.
//
// Почему ошибка ACCESS_DENIED не повторяется. Она означает «у нас нет прав на
// этот процесс» (другой пользователь, другая целостность). Повтор через полсекунды
// даст тот же ответ и только растянет операцию; вместо этого возвращается Failed с
// настоящим HRESULT, а движок поступает по FR-6: «ошибки не фатальны — собираются
// в отчёт, остальные операции продолжаются».
//
// Почему waitForExit сделан срезами, а не одним WaitForSingleObject. Вызов с
// большим таймаутом не отменяется, а пользователь после нажатия «Отмена» ждёт
// мгновенно (§6.4). Пауза перед повтором — тоже.
#include "process_control.hpp"

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место
#include <RestartManager.h> // RmStartSession/RmRegisterResources/RmShutdown/RmGetList (FR-6)
#include <tlhelp32.h>       // CreateToolhelp32Snapshot — поиск процессов по имени (FR-3/FR-4)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::shutdown {
namespace {

using ScopedHandle = platform::unique_handle<platform::KernelHandlePolicy>;

// ---------------------------------------------------------------------------
// Имена
// ---------------------------------------------------------------------------

// Процессы, которые нельзя закрывать никогда. Критерий — не «системный», а
// «без него система не грузится или теряет пользовательскую сессию»: закрытие
// lsass даёт мгновенный сброс сеанса, закрытие svchost уносит половину служб
// вместе с обновлениями и сетью, csrss/wininit/winlogon/lsm — это сама сессия.
// Explorer сюда не входит: его можно закрыть осознанно (пользователь подтвердил),
// Windows его поднимет заново, а нажатие правой кнопкой на иконке программы уже
// умеет просить закрыть всё лишнее. Имена без суффикса — сравнение его снимает
// само (см. isProtectedProcess).
constexpr std::wstring_view kProtected[] = {
    L"system",         // ядро и реестр (PID 4)
    L"system idle process", L"registry", L"secure system", L"memory compression", L"smss",  L"csrss",
    L"wininit",        // первый пользовательский процесс
    L"winlogon",       // вход в систему
    L"services",       // SCM: без него службы не запускаются
    L"svchost",        // хост всех служб
    L"lsass",          // аутентификация
    L"lsm",            // менеджер сессий
    L"dwm",            // композитор рабочего стола
    L"sihost",         // оболочка входа
    L"logon ui",       // окно входа и блокировки
    L"wmiprvse",       // хост WMI: через него считаются диски (FR-1) и BitLocker
};

// CompareStringOrdinal, а не towlower/_wcsicmp: регистр имён задаёт сама система,
// а функция с «языковым» сравнением в turkic-локалях ведёт себя неожиданно
// (об этом же §10 в vfs_delete). Длина передаётся в int, поэтому длинное имя
// сравниваем как «не равно», а не как приведённое.
[[nodiscard]] bool equalNoCase(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    if (left.empty()) return true;
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                 static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

constexpr std::wstring_view kExeSuffix = L".exe";

// Имя без суффикса: и «lsass», и «lsass.exe» — один и тот же процесс. Сравнение
// регистронезависимое, поэтому суффикс убираем в любом написании.
[[nodiscard]] std::wstring_view stripExeSuffix(std::wstring_view name) noexcept {
    if (name.size() <= kExeSuffix.size()) return name;
    const std::wstring_view tail = name.substr(name.size() - kExeSuffix.size());
    if (!equalNoCase(tail, kExeSuffix)) return name;
    return name.substr(0, name.size() - kExeSuffix.size());
}

// Имя файла из полного пути: вызывающий может прислать и «chrome», и
// «C:\Program Files\Google\Chrome\Application\chrome.exe», а решение о защите
// обязано быть одинаковым в обоих случаях.
[[nodiscard]] std::wstring_view fileNameOf(std::wstring_view path) noexcept {
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return path;
    return path.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// Время создания процесса
// ---------------------------------------------------------------------------

// FILETIME ↔ uint64: пара «младшие/старшие DWORD» без memcpy, чтобы не тянуть в
// модуль <cstring> ради четырёх байт.
[[nodiscard]] std::uint64_t toTicks(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | static_cast<std::uint64_t>(time.dwLowDateTime);
}

[[nodiscard]] FILETIME fromTicks(std::uint64_t ticks) noexcept {
    FILETIME time{};
    time.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
    time.dwHighDateTime = static_cast<DWORD>((ticks >> 32) & 0xFFFFFFFFull);
    return time;
}

// ---------------------------------------------------------------------------
// Сессия Restart Manager
// ---------------------------------------------------------------------------

// RAII-обёртка сессии RM (ADR-001: дескрипторы не теряются). Сессия не дескриптор
// в смысле HANDLE, но владение ею ровно такое же: забытый RmEndSession оставляет
// запись в RM до перезагрузки, а второй RmStartSession без RmEndSession в том же
// процессе может упереться в лимит сессий.
class RmSession {
public:
    RmSession() noexcept {
        // Ключ сессии RM возвращает наружу и используется для RmJoinSession;
        // закрывать его нечем, поэтому буфер локальный.
        std::array<wchar_t, CCH_RM_SESSION_KEY + 1> key{};
        DWORD handle = 0;
        error_ = ::RmStartSession(&handle, 0, key.data());
        if (error_ != ERROR_SUCCESS) return;
        handle_ = handle;
        valid_ = true;
    }

    RmSession(const RmSession&) = delete;
    RmSession& operator=(const RmSession&) = delete;

    RmSession(RmSession&& other) noexcept
        : handle_(other.handle_)
        , error_(other.error_)
        , valid_(other.valid_) {
        other.handle_ = 0;
        other.valid_ = false;
    }

    RmSession& operator=(RmSession&& other) noexcept {
        if (this != &other) {
            end();
            handle_ = other.handle_;
            error_ = other.error_;
            valid_ = other.valid_;
            other.handle_ = 0;
            other.valid_ = false;
        }
        return *this;
    }

    ~RmSession() noexcept {
        end();
    }

    void end() noexcept {
        if (valid_) ::RmEndSession(handle_);
        handle_ = 0;
        valid_ = false;
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

    [[nodiscard]] DWORD handle() const noexcept {
        return handle_;
    }

    [[nodiscard]] DWORD error() const noexcept {
        return error_;
    }

private:
    DWORD handle_{0};
    DWORD error_{ERROR_SUCCESS};
    bool valid_{false};
};

// Состояние процессов в сессии по данным RM. Требуется не для «кто держит файл»
// (это соседний модуль), а для двух вещей: узнать, какие из зарегистрированных
// процессов критические (RmCritical), и получить честный статус приложения после
// RmShutdown, когда общий код отказа не говорит, кого RM всё-таки закрыл.
struct ProcessState {
    DWORD pid{};
    ULONG status{};    // RM_APP_STATUS
    RM_APP_TYPE type{}; // RmCritical — закрывать нельзя
};

struct SessionState {
    DWORD code{ERROR_SUCCESS};
    DWORD rebootReasons{};
    std::vector<ProcessState> processes;

    [[nodiscard]] const ProcessState* find(DWORD pid) const noexcept {
        for (const ProcessState& item : processes) {
            if (item.pid == pid) return &item;
        }
        return nullptr;
    }
};

// RmGetList в SDK — две фазы: спросить, сколько записей нужно, затем забрать.
// Список успевает измениться между вызовами, и RM отвечает ERROR_MORE_DATA ещё
// раз, поэтому обе фазы повторяются ограниченное число раз.
[[nodiscard]] SessionState readSessionState(DWORD session) {
    SessionState state;
    for (std::uint32_t attempt = 0; attempt < kSessionListAttempts; ++attempt) {
        UINT needed = 0;
        UINT count = 0;
        DWORD reasons = 0;

        const DWORD code = ::RmGetList(session, &needed, &count, nullptr, &reasons);
        state.code = code;
        state.rebootReasons = reasons;
        if (code != ERROR_MORE_DATA) return state;
        if (needed == 0) return state;

        std::vector<RM_PROCESS_INFO> buffer(needed);
        count = needed;
        reasons = 0;
        const DWORD filled = ::RmGetList(session, &needed, &count, buffer.data(), &reasons);
        state.code = filled;
        state.rebootReasons = reasons;
        // Список успел измениться между фазами: это ровно тот случай, ради
        // которого цикл и повторяется, поэтому идём на следующую попытку.
        if (filled != ERROR_SUCCESS) continue;

        state.processes.clear();
        state.processes.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            const RM_PROCESS_INFO& info = buffer[index];
            ProcessState item;
            item.pid = info.Process.dwProcessId;
            item.status = info.AppStatus;
            item.type = info.ApplicationType;
            state.processes.push_back(item);
        }
        return state;
    }

    // Список уехал три раза подряд: для модуля это «состояние неизвестно», а не
    // «процессов нет» — поэтому код отказа сохраняется, а список пуст.
    state.code = ERROR_MORE_DATA;
    state.processes.clear();
    return state;
}

[[nodiscard]] bool statusClosed(ULONG status) noexcept {
    const ULONG closed = static_cast<ULONG>(RmStatusStopped) | static_cast<ULONG>(RmStatusStoppedOther) |
                         static_cast<ULONG>(RmStatusRestarted);
    return (status & closed) != 0;
}

[[nodiscard]] bool statusMasked(ULONG status) noexcept {
    return (status & static_cast<ULONG>(RmStatusShutdownMasked)) != 0;
}

[[nodiscard]] bool statusErrored(ULONG status) noexcept {
    return (status & static_cast<ULONG>(RmStatusErrorOnStop)) != 0;
}

[[nodiscard]] bool isCritical(const SessionState& state, DWORD pid) noexcept {
    const ProcessState* item = state.find(pid);
    return item != nullptr && item->type == RmCritical;
}

// ---------------------------------------------------------------------------
// Регистрация и завершение
// ---------------------------------------------------------------------------

[[nodiscard]] RM_UNIQUE_PROCESS toUniqueProcess(const ProcessRef& process) noexcept {
    RM_UNIQUE_PROCESS unique{};
    unique.dwProcessId = process.pid;
    unique.ProcessStartTime = fromTicks(process.startTimeTicks);
    return unique;
}

// Зарегистрировать приложения в сессии. Файл, ради которого всё затевалось,
// регистрируется первым, но его отсутствие не должно мешать: закрыть браузер
// можно и потеряв файл (он всё равно не будет удалён, если процесс держит его
// дальше), поэтому при отказе с файлом регистрация повторяется без него.
[[nodiscard]] DWORD registerBatch(const RmSession& session, std::vector<RM_UNIQUE_PROCESS>& apps,
                                   const std::wstring& file) noexcept {
    if (apps.empty()) return ERROR_SUCCESS;
    if (!file.empty()) {
        const wchar_t* files[1] = {file.c_str()};
        const DWORD code =
            ::RmRegisterResources(session.handle(), 1, files, static_cast<UINT>(apps.size()), apps.data(), 0, nullptr);
        if (code == ERROR_SUCCESS) return code;
    }
    return ::RmRegisterResources(session.handle(), 0, nullptr, static_cast<UINT>(apps.size()), apps.data(), 0, nullptr);
}

[[nodiscard]] DWORD shutdownSession(const RmSession& session, bool force) noexcept {
    const ULONG flags = force ? static_cast<ULONG>(RmForceShutdown) : 0UL;
    return ::RmShutdown(session.handle(), flags, nullptr);
}

// ---------------------------------------------------------------------------
// Ожидание
// ---------------------------------------------------------------------------

// Пауза перед повтором: base·2^n с потолком. Вызывающий передаёт номер попытки,
// на которой попытка закончилась (1 — первая).
[[nodiscard]] std::chrono::milliseconds backoffFor(std::uint32_t attempt, const ShutdownOptions& options) noexcept {
    std::chrono::milliseconds delay = options.backoff;
    if (delay.count() < 0) delay = std::chrono::milliseconds{0};
    const std::chrono::milliseconds ceiling = options.maxBackoff.count() > 0 ? options.maxBackoff : options.backoff;
    for (std::uint32_t index = 1; index < attempt; ++index) {
        if (delay >= ceiling) return ceiling;
        delay *= 2;
    }
    if (delay > ceiling) delay = ceiling;
    return delay;
}

[[nodiscard]] bool waitBackoff(std::chrono::milliseconds delay, const std::stop_token& stop) {
    auto left = delay;
    while (left.count() > 0) {
        if (stop.stop_requested()) return false;
        const std::chrono::milliseconds slice = (left < kWaitSlice) ? left : kWaitSlice;
        std::this_thread::sleep_for(slice);
        left -= slice;
    }
    return !stop.stop_requested();
}

// Ожидание сигнала «процесс завершился». Срезами по kWaitSlice, а не одним
// WaitForSingleObject на всё время: отмена должна срабатывать мгновенно (§6.4).
[[nodiscard]] bool waitForExit(const ScopedHandle& process, std::chrono::milliseconds timeout,
                               const std::stop_token& stop) noexcept {
    if (!process) return true;  // дескриптора нет — открыть процесс не удалось: считаем, что его уже нет
    std::chrono::milliseconds left = timeout.count() > 0 ? timeout : std::chrono::milliseconds{0};
    while (true) {
        if (stop.stop_requested()) return false;
        if (left.count() <= 0) return false;  // время вышло
        const std::chrono::milliseconds slice = (left < kWaitSlice) ? left : kWaitSlice;
        const DWORD waited = ::WaitForSingleObject(process.get(), static_cast<DWORD>(slice.count()));
        if (waited == WAIT_OBJECT_0) return true;
        if (waited != WAIT_TIMEOUT) return false;  // WAIT_FAILED: дескриптор не тот
        left -= slice;
    }
}

// ---------------------------------------------------------------------------
// Отчётные поля
// ---------------------------------------------------------------------------

// Поля собираются явно, а не макросом MRP_LOG_*: core::detail::logFieldList
// разворачивает пакет в один вызов logField со всеми аргументами, и на MSVC
// такой вызов не разрешается (C2661) — то же, что зафиксировано в vfs_delete.cpp
// и storage_query.cpp. Чужий заголовок не правим.
[[nodiscard]] core::LogFields entryFields(const ShutdownEntry& entry) {
    core::LogFields fields;
    fields.push_back(core::logField("pid", static_cast<long long>(entry.process.pid)));
    fields.push_back(core::logField("name", core::toUtf8(entry.process.name)));
    fields.push_back(core::logField("decision", toString(entry.decision)));
    fields.push_back(core::logField("status", toString(entry.status)));
    if (entry.attempts != 0) fields.push_back(core::logField("attempts", static_cast<long long>(entry.attempts)));
    if (entry.hr != 0) fields.push_back(core::logField("hr", static_cast<long long>(entry.hr)));
    if (!entry.detail.empty()) fields.push_back(core::logField("detail", core::toUtf8(entry.detail)));
    return fields;
}

[[nodiscard]] core::LogFields summaryFields(const ShutdownSummary& summary) {
    core::LogFields fields;
    fields.push_back(core::logField("processes", static_cast<long long>(summary.entries.size())));
    fields.push_back(core::logField("closed", static_cast<long long>(summary.closed)));
    fields.push_back(core::logField("alreadyExited", static_cast<long long>(summary.alreadyExited)));
    if (summary.protectedSkipped != 0) {
        fields.push_back(core::logField("protected", static_cast<long long>(summary.protectedSkipped)));
    }
    if (summary.selfSkipped != 0) fields.push_back(core::logField("self", static_cast<long long>(summary.selfSkipped)));
    if (summary.unknownSkipped != 0) {
        fields.push_back(core::logField("unknown", static_cast<long long>(summary.unknownSkipped)));
    }
    if (summary.notConfirmed != 0) {
        fields.push_back(core::logField("notConfirmed", static_cast<long long>(summary.notConfirmed)));
    }
    if (summary.refused != 0) fields.push_back(core::logField("refused", static_cast<long long>(summary.refused)));
    if (summary.timedOut != 0) fields.push_back(core::logField("timedOut", static_cast<long long>(summary.timedOut)));
    if (summary.cancelled != 0) {
        fields.push_back(core::logField("cancelled", static_cast<long long>(summary.cancelled)));
    }
    if (summary.failed != 0) fields.push_back(core::logField("failed", static_cast<long long>(summary.failed)));
    fields.push_back(core::logField("attempts", static_cast<long long>(summary.attempts)));
    fields.push_back(core::logField("waitedMs", static_cast<long long>(summary.waited.count())));
    return fields;
}

void logSummary(const ShutdownSummary& summary) {
    // Ничего не просили закрывать — это не событие журнала: на каждом
    // «Skip (locked)» в отчёте такая строка превратилась бы в шум.
    if (summary.entries.empty()) return;

    if (summary.hr != 0) {
        core::logFailure("platform.procs.close", "не все процессы закрыты", core::toUtf8(summary.resourcePath),
                         static_cast<std::int64_t>(summary.hr), summaryFields(summary));
        return;
    }
    if (summary.allClosed()) {
        core::logInfo("platform.procs.close", "процессы закрыты", summaryFields(summary));
        return;
    }
    core::logWarn("platform.procs.close", "закрытие процессов завершено частично", summaryFields(summary));
}

void logLargePreview(const ShutdownPreview& preview) {
    if (preview.entries.size() <= kLargePreviewWarning) return;
    core::LogFields fields;
    fields.push_back(core::logField("processes", static_cast<long long>(preview.entries.size())));
    core::logWarn("platform.procs.preview", "список процессов велик: диалог подтверждения обязан быть прокручиваемым",
                  std::move(fields));
}

// ---------------------------------------------------------------------------
// Внутренние операции над записью отчёта
// ---------------------------------------------------------------------------

// Помечающие помощники возвращают void намеренно: они вызываются как
// «установить статус и уйти», а возврат ссылки на запись только провоцирует
// [[nodiscard]]-предупреждения в местах, где результат не нужен.
void markDecided(ShutdownEntry& entry, ShutdownDecision decision, std::wstring_view detail) {
    entry.decision = decision;
    entry.status = ShutdownStatus::NotAttempted;
    entry.detail.assign(detail.data(), detail.size());
}

void markClosed(ShutdownEntry& entry) {
    entry.status = ShutdownStatus::Closed;
    if (entry.detail.empty()) entry.detail = L"закрыт по ответу Restart Manager";
}

void markAlreadyExited(ShutdownEntry& entry) {
    entry.status = ShutdownStatus::AlreadyExited;
    if (entry.detail.empty()) entry.detail = L"процесса уже нет";
}

void markRefused(ShutdownEntry& entry, std::int32_t code, std::wstring_view detail) {
    entry.status = ShutdownStatus::Refused;
    entry.hr = code;
    entry.detail.assign(detail.data(), detail.size());
}

void markFailed(ShutdownEntry& entry, std::int32_t code, std::wstring_view detail) {
    entry.status = ShutdownStatus::Failed;
    entry.hr = code;
    entry.detail.assign(detail.data(), detail.size());
}

void markCancelled(ShutdownEntry& entry) {
    entry.status = ShutdownStatus::Cancelled;
    if (entry.detail.empty()) entry.detail = L"операция отменена";
}

[[nodiscard]] std::int32_t hresult(DWORD win32Code) noexcept {
    return static_cast<std::int32_t>(platform::hresultFromWin32(win32Code));
}

// Человекочитаемая причина отказа RM. Текст системы из win_error.hpp в отчёт не
// идёт: «Отказано в доступе» без пути и без PID бесполезно, а «нет прав на
// завершение процесса» — полезно.
//
// Отдельного кода «приложение не даёт закрыться» у RmShutdown нет: в
// RestartManager.h перечислены ERROR_FAIL_SHUTDOWN, ERROR_FAIL_NOACTION_REBOOT,
// ERROR_CANCELLED, ERROR_SEM_TIMEOUT, ERROR_BAD_ARGUMENTS, ERROR_WRITE_FAULT,
// ERROR_OUTOFMEMORY и ERROR_INVALID_HANDLE (ERROR_NOT_HUNG из MSDN в SDK и в
// списке заголовка отсутствует, и ссылаться на него нельзя). Признак отказа
// приложения ловится по флагам: RmStatusShutdownMasked в ответе RmGetList и
// RmRebootReason* в rebootReasons.
[[nodiscard]] std::wstring_view shutdownReason(DWORD code, DWORD rebootReasons) noexcept {
    if ((rebootReasons & static_cast<DWORD>(RmRebootReasonPermissionDenied)) != 0) {
        return L"нет прав на завершение процесса (Restart Manager сообщил об отказе в доступе)";
    }
    if ((rebootReasons & static_cast<DWORD>(RmRebootReasonCriticalProcess)) != 0 ||
        (rebootReasons & static_cast<DWORD>(RmRebootReasonCriticalService)) != 0) {
        return L"процесс критический для системы: закрыть его нельзя";
    }
    if ((rebootReasons & static_cast<DWORD>(RmRebootReasonSessionMismatch)) != 0) {
        return L"процесс запущен в другой сессии терминального сервера";
    }
    if ((rebootReasons & static_cast<DWORD>(RmRebootReasonDetectedSelf)) != 0) {
        return L"Restart Manager обнаружил текущий процесс";
    }
    switch (code) {
        case ERROR_ACCESS_DENIED:
            return L"нет прав на завершение процесса";
        case ERROR_FAIL_NOACTION_REBOOT:
            return L"закрытие процесса возможно только после перезагрузки";
        case ERROR_FAIL_SHUTDOWN:
            return L"Restart Manager не смог закрыть приложение";
        case ERROR_CANCELLED:
            return L"закрытие отменено системой";
        case ERROR_SEM_TIMEOUT:
            return L"Restart Manager не ответил вовремя";
        case ERROR_INVALID_HANDLE:
            return L"сессия Restart Manager закрыта";
        default:
            return L"Restart Manager не смог закрыть процесс";
    }
}

// Счётчик отказа пачки для отчёта: первый HRESULT, который не пустой, иначе
// ничего не записываем.
void rememberFailure(ShutdownSummary& summary, std::int32_t value) noexcept {
    if (value != 0 && summary.hr == 0) summary.hr = value;
}

void recount(ShutdownSummary& summary) noexcept {
    summary.closed = 0;
    summary.alreadyExited = 0;
    summary.protectedSkipped = 0;
    summary.selfSkipped = 0;
    summary.unknownSkipped = 0;
    summary.notConfirmed = 0;
    summary.refused = 0;
    summary.timedOut = 0;
    summary.cancelled = 0;
    summary.failed = 0;

    for (const ShutdownEntry& entry : summary.entries) {
        switch (entry.status) {
            case ShutdownStatus::Closed:
                ++summary.closed;
                break;
            case ShutdownStatus::AlreadyExited:
                ++summary.alreadyExited;
                break;
            case ShutdownStatus::NotConfirmed:
                ++summary.notConfirmed;
                break;
            case ShutdownStatus::Refused:
                ++summary.refused;
                break;
            case ShutdownStatus::Timeout:
                ++summary.timedOut;
                break;
            case ShutdownStatus::Cancelled:
                ++summary.cancelled;
                break;
            case ShutdownStatus::Failed:
                ++summary.failed;
                break;
            case ShutdownStatus::NotAttempted:
                break;
        }

        switch (entry.decision) {
            case ShutdownDecision::Protected:
                ++summary.protectedSkipped;
                break;
            case ShutdownDecision::Self:
                ++summary.selfSkipped;
                break;
            case ShutdownDecision::Unknown:
                ++summary.unknownSkipped;
                break;
            case ShutdownDecision::WouldClose:
            case ShutdownDecision::AlreadyExited:
                break;
        }
    }
}

// Подтверждён ли процесс: в approved есть тот же PID, а время запута��, если оно
// известно у обоих, совпадает. Процесс, которого не было в показанном списке,
// подтверждённым не считается: диалог не мог его показать, а значит и не мог
// спросить про него пользователя.
[[nodiscard]] bool isApproved(const std::vector<ProcessRef>& approved, const ProcessRef& candidate) noexcept {
    for (const ProcessRef& item : approved) {
        if (item.pid != candidate.pid) continue;
        if (candidate.startTimeTicks != 0 && item.startTimeTicks != 0 &&
            item.startTimeTicks != candidate.startTimeTicks) {
            continue;
        }
        return true;
    }
    return false;
}

// Рабочая запись на время операции: элемент отчёта плюс дескриптор процесса для
// ожидания выхода. Дескриптор открывается ДО RmShutdown — после команды процесс
// может завершиться, и OpenProcess на уже мёртвом процессе ничего полезного не
// скажет. Имя не Pending: так называется статус ожидания в winnt.h, и пересечение
// имён с заголовками Windows в модуле, который их включает, — источник правок,
// которые никто не ищет.
struct PendingProcess {
    ShutdownEntry* entry{nullptr};
    ScopedHandle handle{};
};

}  // namespace

// ---------------------------------------------------------------------------
// Решения и статусы
// ---------------------------------------------------------------------------

const char* toString(ShutdownDecision decision) noexcept {
    switch (decision) {
        case ShutdownDecision::WouldClose:
            return "wouldClose";
        case ShutdownDecision::AlreadyExited:
            return "alreadyExited";
        case ShutdownDecision::Protected:
            return "protected";
        case ShutdownDecision::Self:
            return "self";
        case ShutdownDecision::Unknown:
            return "unknown";
    }
    return "unknown";
}

const char* toString(ShutdownStatus status) noexcept {
    switch (status) {
        case ShutdownStatus::NotAttempted:
            return "notAttempted";
        case ShutdownStatus::Closed:
            return "closed";
        case ShutdownStatus::NotConfirmed:
            return "notConfirmed";
        case ShutdownStatus::Refused:
            return "refused";
        case ShutdownStatus::Timeout:
            return "timeout";
        case ShutdownStatus::Cancelled:
            return "cancelled";
        case ShutdownStatus::Failed:
            return "failed";
    }
    return "failed";
}

bool isClosed(ShutdownStatus status) noexcept {
    return status == ShutdownStatus::Closed || status == ShutdownStatus::AlreadyExited;
}

bool isSkipped(ShutdownStatus status) noexcept {
    return status == ShutdownStatus::NotAttempted || status == ShutdownStatus::NotConfirmed ||
           status == ShutdownStatus::Refused || status == ShutdownStatus::Cancelled;
}

std::size_t ShutdownPreview::approvableCount() const noexcept {
    std::size_t count = 0;
    for (const ShutdownEntry& entry : entries) {
        if (entry.approvable()) ++count;
    }
    return count;
}

std::size_t ShutdownPreview::protectedCount() const noexcept {
    std::size_t count = 0;
    for (const ShutdownEntry& entry : entries) {
        if (entry.decision == ShutdownDecision::Protected) ++count;
    }
    return count;
}

std::size_t ShutdownPreview::alreadyExitedCount() const noexcept {
    std::size_t count = 0;
    for (const ShutdownEntry& entry : entries) {
        if (entry.decision == ShutdownDecision::AlreadyExited) ++count;
    }
    return count;
}

std::wstring ShutdownPreview::describe() const {
    std::wstring text;
    for (const ShutdownEntry& entry : entries) {
        if (!entry.approvable()) continue;
        if (!text.empty()) text += L", ";
        if (!entry.process.name.empty()) {
            text += entry.process.name;
        } else {
            text += L"PID ";
        }
        text += L" (";
        text += std::to_wstring(entry.process.pid);
        text += L")";
    }
    return text;
}

// ---------------------------------------------------------------------------
// Процессы
// ---------------------------------------------------------------------------

bool isProtectedProcess(std::wstring_view name) noexcept {
    const std::wstring_view bare = stripExeSuffix(fileNameOf(name));
    if (bare.empty()) return false;
    for (const std::wstring_view protectedName : kProtected) {
        if (equalNoCase(bare, protectedName)) return true;
    }
    return false;
}

std::vector<std::wstring> protectedProcessNames() {
    std::vector<std::wstring> names;
    names.reserve(sizeof(kProtected) / sizeof(kProtected[0]));
    for (const std::wstring_view name : kProtected) names.emplace_back(name);
    return names;
}

std::optional<std::uint64_t> readProcessStartTime(std::uint32_t pid) noexcept {
    if (pid == 0) return std::nullopt;
    const ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
    if (!process) return std::nullopt;
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!::GetProcessTimes(process.get(), &creation, &exit, &kernel, &user)) return std::nullopt;
    return toTicks(creation);
}

bool isProcessRunning(std::uint32_t pid) noexcept {
    if (pid == 0) return false;
    const ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
    if (!process) {
        // ERROR_ACCESS_DENIED — процесс существует, но прав на чтение нет.
        // Считать его отсутствующим нельзя: иначе RM-сессия решит, что файл
        // свободен, и удалит его из-под работающей программы (§10).
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(process.get(), &code)) return true;  // не прочитали — считаем живым
    return code == STILL_ACTIVE;
}

bool isProcessRunning(const ProcessRef& process) noexcept {
    if (!isProcessRunning(process.pid)) return false;
    if (process.startTimeTicks == 0) return true;
    const std::optional<std::uint64_t> actual = readProcessStartTime(process.pid);
    if (!actual) return true;  // прочитать не удалось — не считаем отсутствующим
    return *actual == process.startTimeTicks;
}

std::wstring queryProcessName(std::uint32_t pid) {
    if (pid == 0) return {};
    const ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
    if (!process) return {};

    // Предел расширенного пути — 32767 символов; расти дольше незачем.
    constexpr std::size_t kFirstGuess = 260;
    constexpr std::size_t kMaxPathChars = 32768;
    std::wstring buffer(kFirstGuess, L'\0');
    while (true) {
        DWORD size = static_cast<DWORD>(buffer.size());
        if (::QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size) != 0) {
            buffer.resize(size);
            return fileNameOf(buffer).empty() ? std::wstring{} : std::wstring(fileNameOf(buffer));
        }
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
        if (buffer.size() >= kMaxPathChars) return {};
        buffer.resize((buffer.size() * 2 < kMaxPathChars) ? buffer.size() * 2 : kMaxPathChars);
    }
}

// Снимок списка процессов. Toolhelp32 под нагрузкой может вернуть
// ERROR_BAD_LENGTH — это единственный отказ, который лечится повтором.
[[nodiscard]] ScopedHandle snapshotProcesses() noexcept {
    for (std::uint32_t attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
        const HANDLE raw = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (raw != INVALID_HANDLE_VALUE) return ScopedHandle(raw);
        if (::GetLastError() != ERROR_BAD_LENGTH) return ScopedHandle();
    }
    return ScopedHandle();
}

// Совпадает ли имя процесса с одним из запрошенных. Сравнение без учёта регистра
// и без учёта суффикса: правило пишет «msedge», а Toolhelp отдаёт «msedge.exe».
[[nodiscard]] bool matchesAnyName(std::wstring_view candidate, const std::vector<std::wstring>& names) noexcept {
    const std::wstring_view bare = stripExeSuffix(candidate);
    for (const std::wstring& wanted : names) {
        if (wanted.empty()) continue;
        if (equalNoCase(bare, stripExeSuffix(wanted))) return true;
    }
    return false;
}

std::vector<ProcessRef> findProcessesByName(const std::vector<std::wstring>& names) {
    std::vector<ProcessRef> found;
    if (names.empty()) return found;

    const ScopedHandle snapshot = snapshotProcesses();
    if (!snapshot) return found;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry)) return found;

    const DWORD self = ::GetCurrentProcessId();
    do {
        if (entry.th32ProcessID != 0 && entry.th32ProcessID != self &&
            matchesAnyName(entry.szExeFile, names)) {
            ProcessRef process;
            process.pid = static_cast<std::uint32_t>(entry.th32ProcessID);
            process.name = entry.szExeFile;
            process.startTimeTicks = readProcessStartTime(process.pid).value_or(0);
            found.push_back(std::move(process));
        }
        entry.dwSize = sizeof(entry);  // Process32Next требует размер на каждом шаге
    } while (::Process32NextW(snapshot.get(), &entry));

    return found;
}

bool isAnyProcessRunning(const std::vector<std::wstring>& names) noexcept {
    if (names.empty()) return false;
    const ScopedHandle snapshot = snapshotProcesses();
    if (!snapshot) return false;  // не смогли узнать — не врём: «не запущено» здесь означало бы «можно чистить»

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry)) return false;

    do {
        if (entry.th32ProcessID != 0 && matchesAnyName(entry.szExeFile, names)) return true;
        entry.dwSize = sizeof(entry);
    } while (::Process32NextW(snapshot.get(), &entry));

    return false;
}

// ---------------------------------------------------------------------------
// Мост к модели (§6.3)
// ---------------------------------------------------------------------------

std::vector<ProcessRef> fromModelProcessRefs(const std::vector<core::ProcessRef>& refs) {
    std::vector<ProcessRef> processes;
    if (refs.empty()) return processes;

    processes.reserve(refs.size());
    for (const core::ProcessRef& ref : refs) {
        if (ref.pid == 0) continue;  // PID 0 не бывает приложением
        ProcessRef process;
        process.pid = ref.pid;
        // Имя из модели в UTF-8: в платформе имена живут в UTF-16 (§6.3).
        process.name = platform::toUtf16(ref.name);
        // startTimeTicks намеренно остаётся нулевым: модель его не хранит, а
        // выдуманное время хуже отсутствующего — planShutdown прочитает
        // настоящее.
        processes.push_back(std::move(process));
    }
    return processes;
}

std::vector<core::ProcessRef> toModelProcessRefs(const std::vector<ProcessRef>& processes) {
    std::vector<core::ProcessRef> refs;
    if (processes.empty()) return refs;

    refs.reserve(processes.size());
    for (const ProcessRef& process : processes) {
        if (process.pid == 0) continue;
        core::ProcessRef ref;
        ref.pid = process.pid;
        ref.name = platform::toUtf8(process.name);
        refs.push_back(std::move(ref));
    }
    return refs;
}

std::vector<core::ProcessRef> toModelProcessRefs(const std::vector<ShutdownEntry>& entries) {
    std::vector<core::ProcessRef> refs;
    if (entries.empty()) return refs;

    refs.reserve(entries.size());
    for (const ShutdownEntry& entry : entries) {
        if (entry.process.pid == 0) continue;
        core::ProcessRef ref;
        ref.pid = entry.process.pid;
        ref.name = platform::toUtf8(entry.process.name);
        refs.push_back(std::move(ref));
    }
    return refs;
}

// ---------------------------------------------------------------------------
// Предпросмотр
// ---------------------------------------------------------------------------

ShutdownPreview planShutdown(const std::vector<ProcessRef>& processes, const ShutdownOptions& options) {
    ShutdownPreview preview;
    if (!options.resourcePath.empty()) preview.resourcePath.assign(options.resourcePath);
    if (processes.empty()) return preview;

    preview.entries.reserve(processes.size());
    const DWORD self = ::GetCurrentProcessId();

    for (const ProcessRef& requested : processes) {
        if (requested.pid == 0) continue;  // PID 0 не бывает приложением

        // Дубликаты: один и тот же PID, пришедший из нескольких кандидатов,
        // показывается пользователю один раз — иначе в диалоге он увидит одно и
        // то же приложение столько раз, сколько файлов он держит.
        const bool duplicate =
            std::any_of(preview.entries.begin(), preview.entries.end(),
                        [&requested](const ShutdownEntry& seen) { return seen.process.pid == requested.pid; });
        if (duplicate) continue;

        ShutdownEntry entry;
        entry.process = requested;
        if (entry.process.name.empty()) entry.process.name = queryProcessName(entry.process.pid);

        if (entry.process.pid == self) {
            markDecided(entry, ShutdownDecision::Self, L"это текущий процесс MrProper");
        } else if (isProtectedProcess(entry.process.name)) {
            markDecided(entry, ShutdownDecision::Protected, L"системный процесс: закрывать нельзя");
        } else if (!isProcessRunning(entry.process)) {
            markDecided(entry, ShutdownDecision::AlreadyExited, L"процесс уже завершён");
        } else {
            // Время запуска нужно самому RM, иначе переиспользованный PID
            // укажет на чужой процесс. Прочитать не удалось (нет прав, процесс
            // умер между проверками) — закрывать нельзя: неизвестно, тот ли это
            // процесс.
            if (entry.process.startTimeTicks == 0) {
                const std::optional<std::uint64_t> started = readProcessStartTime(entry.process.pid);
                if (started) {
                    entry.process.startTimeTicks = *started;
                } else {
                    markDecided(entry, ShutdownDecision::Unknown, L"не удалось прочитать время запуска процесса");
                }
            }
        }

        preview.entries.push_back(std::move(entry));
    }

    logLargePreview(preview);
    return preview;
}

// ---------------------------------------------------------------------------
// Закрытие
// ---------------------------------------------------------------------------

ShutdownSummary closeProcesses(const ShutdownPreview& preview, const ShutdownOptions& options, std::stop_token stop) {
    ShutdownSummary summary;
    summary.entries = preview.entries;
    summary.resourcePath = preview.resourcePath;
    if (summary.resourcePath.empty() && !options.resourcePath.empty()) {
        summary.resourcePath.assign(options.resourcePath);
    }

    if (summary.entries.empty()) return summary;

    // 1. Отмена до всего (§6.4). Подтверждение при отмене не показывается.
    if (stop.stop_requested()) {
        for (ShutdownEntry& entry : summary.entries) {
            if (entry.approvable()) markCancelled(entry);
        }
        recount(summary);
        logSummary(summary);
        return summary;
    }

    // 2. Кандидаты: только те, кого модуль готов закрыть.
    std::vector<ShutdownEntry*> candidates;
    candidates.reserve(summary.entries.size());
    for (ShutdownEntry& entry : summary.entries) {
        if (entry.approvable()) candidates.push_back(&entry);
    }
    if (candidates.empty()) {
        recount(summary);
        logSummary(summary);
        return summary;
    }

    // 3. Подтверждение — ровно один раз, до первого RmShutdown. Без колбэка
    //    закрытия нет: это часть контракта, а не забывчивость вызывающего.
    if (!options.confirm) {
        for (ShutdownEntry* entry : candidates) {
            entry->status = ShutdownStatus::NotConfirmed;
            entry->detail = L"подтверждение не запрошено: закрытие не выполняется";
        }
        recount(summary);
        core::LogFields fields = summaryFields(summary);
        fields.push_back(core::logField("reason", std::string("нет обработчика подтверждения")));
        core::logWarn("platform.procs.close", "закрытие процессов не подтверждено", std::move(fields));
        logSummary(summary);
        return summary;
    }

    ShutdownPreview dialog;
    dialog.resourcePath = summary.resourcePath;
    dialog.entries.reserve(candidates.size());
    for (const ShutdownEntry* entry : candidates) dialog.entries.push_back(*entry);

    std::vector<ProcessRef> approved;
    bool granted = false;
    try {
        granted = options.confirm(dialog, approved);
    } catch (const std::exception& error) {
        core::LogFields fields;
        fields.push_back(core::logField("reason", std::string("обработчик подтверждения бросил: ") + error.what()));
        core::logError("platform.procs.confirm", "подтверждение не получено", std::move(fields));
        granted = false;
    } catch (...) {
        core::LogFields fields;
        fields.push_back(
            core::logField("reason", std::string("обработчик подтверждения бросил неизвестное исключение")));
        core::logError("platform.procs.confirm", "подтверждение не получено", std::move(fields));
        granted = false;
    }

    if (!granted) {
        for (ShutdownEntry* entry : candidates) {
            entry->status = ShutdownStatus::NotConfirmed;
            entry->detail = L"пользователь не подтвердил закрытие";
        }
        recount(summary);
        logSummary(summary);
        return summary;
    }
    summary.confirmed = true;

    // 4. Осталось то, что пользователь подтвердил И что было показано. Всё
    //    остальное — NotConfirmed: пользователь о нём не нажал «закрыть».
    std::vector<PendingProcess> pending;
    pending.reserve(candidates.size());
    for (ShutdownEntry* entry : candidates) {
        if (!isApproved(approved, entry->process)) {
            entry->status = ShutdownStatus::NotConfirmed;
            entry->detail = L"пользователь не подтвердил закрытие этого приложения";
            continue;
        }
        PendingProcess item;
        item.entry = entry;
        item.handle.reset(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                        static_cast<DWORD>(entry->process.pid)));
        pending.push_back(std::move(item));
    }
    if (pending.empty()) {
        recount(summary);
        logSummary(summary);
        return summary;
    }

    // Подтверждение получено — это событие журнала: по нему видно, что именно
    // приложение решило закрыть, даже если само закрытие потом не удалось.
    {
        core::LogFields fields;
        fields.push_back(core::logField("approved", static_cast<long long>(pending.size())));
        fields.push_back(core::logField("declined", static_cast<long long>(candidates.size() - pending.size())));
        fields.push_back(core::logField("processes", core::toUtf8(dialog.describe())));
        core::logInfo("platform.procs.confirm", "закрытие процессов подтверждено пользователем", std::move(fields));
    }

    // 5. Сессия RM. Её отказ — общий для всех кандидатов и повтором не лечится.
    RmSession session;
    if (!session.valid()) {
        const std::int32_t failure = hresult(session.error());
        for (PendingProcess& item : pending) {
            markFailed(*item.entry, failure, L"Restart Manager не смог открыть сессию");
        }
        rememberFailure(summary, failure);
        recount(summary);
        logSummary(summary);
        return summary;
    }

    // 6. Попытки с backoff (FR-6: 3×). Повторяется только то, что повторяемо:
    //    ACCESS_DENIED, «закрытие отменено», «нужна перезагрузка» и отказ
    //    самого приложения повтором не проходят, повтор лишь растянет операцию.
    const std::uint32_t attempts = options.maxAttempts == 0 ? 1 : options.maxAttempts;
    DWORD lastCode = ERROR_SUCCESS;
    const auto startedAt = std::chrono::steady_clock::now();

    for (std::uint32_t attempt = 1; attempt <= attempts; ++attempt) {
        if (stop.stop_requested()) {
            for (PendingProcess& item : pending) markCancelled(*item.entry);
            pending.clear();
            break;
        }
        summary.attempts = attempt;

        std::vector<RM_UNIQUE_PROCESS> unique;
        unique.reserve(pending.size());
        for (const PendingProcess& item : pending) unique.push_back(toUniqueProcess(item.entry->process));

        const DWORD registration = registerBatch(session, unique, summary.resourcePath);
        if (registration != ERROR_SUCCESS) {
            const std::int32_t failure = hresult(registration);
            for (PendingProcess& item : pending) {
                item.entry->attempts = attempt;
                markFailed(*item.entry, failure, shutdownReason(registration, 0));
            }
            rememberFailure(summary, failure);
            pending.clear();
            break;
        }

        // Второй заслон на тот же риск, что и список имён: сам RM сообщает,
        // какие из зарегистрированных процессов критические. Такие выбрасываются
        // из пачки, а сессия перерегистрируется без них — иначе RmShutdown
        // закроет и их.
        const SessionState before = readSessionState(session.handle());
        std::vector<PendingProcess> closable;
        closable.reserve(pending.size());
        for (PendingProcess& item : pending) {
            if (isCritical(before, static_cast<DWORD>(item.entry->process.pid))) {
                markDecided(*item.entry, ShutdownDecision::Protected,
                            L"Restart Manager пометил процесс как критический");
                continue;
            }
            closable.push_back(std::move(item));
        }
        pending.swap(closable);
        if (pending.empty()) break;

        unique.clear();
        for (const PendingProcess& item : pending) unique.push_back(toUniqueProcess(item.entry->process));
        if (registerBatch(session, unique, summary.resourcePath) != ERROR_SUCCESS) {
            const std::int32_t failure = hresult(ERROR_INVALID_HANDLE);
            for (PendingProcess& item : pending) {
                item.entry->attempts = attempt;
                markFailed(*item.entry, failure, L"сессия Restart Manager сброшена после фильтрации процессов");
            }
            rememberFailure(summary, failure);
            pending.clear();
            break;
        }

        const DWORD code = shutdownSession(session, options.force);
        lastCode = code;

        if (code == ERROR_SUCCESS) {
            for (PendingProcess& item : pending) {
                item.entry->attempts = attempt;
                // RM подтвердил закрытие, но это команда, а не факт: ждём
                // сигнала от дескриптора — файл может быть всё ещё занят.
                bool exited = false;
                if (options.exitWait.count() <= 0) {
                    exited = true;  // ждать не просили: доверяем ответу RM
                } else if (item.handle) {
                    exited = waitForExit(item.handle, options.exitWait, stop);
                } else {
                    // Дескриптор не открылся: либо процесс уже умер, либо не
                    // хватило прав. Различает это честная проверка, а не
                    // оптимизм — иначе «не смогли открыть» читалось бы как
                    // «процесса нет».
                    exited = !isProcessRunning(item.entry->process);
                }
                if (exited) {
                    markClosed(*item.entry);
                } else if (stop.stop_requested()) {
                    markCancelled(*item.entry);
                } else {
                    item.entry->status = ShutdownStatus::Timeout;
                    item.entry->hr = hresult(ERROR_TIMEOUT);
                    item.entry->detail = L"Restart Manager закрыл процесс, но он не завершился за отведённое время";
                    rememberFailure(summary, item.entry->hr);
                }
            }
            pending.clear();
            break;
        }

        const SessionState after = readSessionState(session.handle());
        const std::wstring_view reason = shutdownReason(code, after.rebootReasons);
        std::vector<PendingProcess> retry;
        retry.reserve(pending.size());

        for (PendingProcess& item : pending) {
            item.entry->attempts = attempt;
            const ProcessState* state = after.find(static_cast<DWORD>(item.entry->process.pid));
            const ULONG appStatus = state != nullptr ? state->status : 0;

            // Часть приложений RM мог закрыть даже при общем отказе: их честно
            // считаем закрытыми, а не «ошибкой».
            if (statusClosed(appStatus)) {
                markClosed(*item.entry);
                continue;
            }
            // Приложение само запретило закрытие (фильтр RmNoShutdown) — повтор
            // не поможет, а пользователю это надо показать как Refused, а не
            // как «ошибка очистки».
            if (statusMasked(appStatus)) {
                markRefused(*item.entry, hresult(code), L"приложение запретило своё закрытие");
                continue;
            }
            // RM сообщил, что не смог остановить именно это приложение.
            if (statusErrored(appStatus)) {
                markFailed(*item.entry, hresult(code), L"Restart Manager не смог остановить приложение");
                rememberFailure(summary, item.entry->hr);
                continue;
            }
            if (code == ERROR_ACCESS_DENIED) {
                markFailed(*item.entry, hresult(code), reason);
                rememberFailure(summary, item.entry->hr);
                continue;
            }
            // ERROR_CANCELLED и ERROR_FAIL_NOACTION_REBOOT из списка
            // RestartManager.h: закрытие отменено системой или возможно только
            // после перезагрузки. Повтор здесь бессилен.
            if (code == ERROR_CANCELLED || code == ERROR_FAIL_NOACTION_REBOOT) {
                markRefused(*item.entry, hresult(code), reason);
                continue;
            }
            // Критический процесс или чужая сессия терминального сервера по
            // rebootReasons: тоже не «попробуем ещё раз».
            if ((after.rebootReasons & (static_cast<DWORD>(RmRebootReasonCriticalProcess) |
                                        static_cast<DWORD>(RmRebootReasonCriticalService) |
                                        static_cast<DWORD>(RmRebootReasonSessionMismatch) |
                                        static_cast<DWORD>(RmRebootReasonDetectedSelf) |
                                        static_cast<DWORD>(RmRebootReasonPermissionDenied))) != 0) {
                markRefused(*item.entry, hresult(code), reason);
                continue;
            }
            // ERROR_FAIL_SHUTDOWN, ERROR_SEM_TIMEOUT и всё прочее повторяемо:
            // приложение могло освобождать файл в момент команды.
            retry.push_back(std::move(item));
        }
        pending.swap(retry);
        if (pending.empty()) break;

        if (attempt >= attempts) break;
        if (!waitBackoff(backoffFor(attempt, options), stop)) {
            for (PendingProcess& item : pending) markCancelled(*item.entry);
            pending.clear();
            break;
        }
    }

    // 7. Попытки исчерпаны: осталось то, что RM не смог закрыть.
    if (!pending.empty()) {
        const std::int32_t failure = hresult(lastCode == ERROR_SUCCESS ? ERROR_TIMEOUT : lastCode);
        for (PendingProcess& item : pending) {
            markFailed(*item.entry, failure, shutdownReason(lastCode, 0));
        }
        rememberFailure(summary, failure);
    }

    summary.waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
    recount(summary);
    logSummary(summary);
    return summary;
}

ShutdownSummary closeProcessesByName(const std::vector<std::wstring>& names, const ShutdownOptions& options,
                                     std::stop_token stop) {
    const std::vector<ProcessRef> found = findProcessesByName(names);
    const ShutdownPreview preview = planShutdown(found, options);
    return closeProcesses(preview, options, stop);
}

}  // namespace mrproper::platform::shutdown
