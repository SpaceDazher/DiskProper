// Реализация модуля Restart Manager. Спека и обоснование — в
// restart_manager.hpp; здесь только код и замечания, которые видны с этого
// уровня.
//
// Три вызова Win32, из которых модуль состоит, имеют разную природу отказа,
// и это определяет весь код:
//
//   * RmStartSession может не открыться вовсе (RM недоступен, память, семафор),
//     и тогда проверять нечего: сессии нет, а не «файлы свободны»;
//   * RmRegisterResources отказывает по существу (нет файла, нет прав) и
//     возвращает код Win32, который обязан попасть в результат, иначе
//     ResourceNotFound и AccessDenied неразличимы в отчёте;
//   * RmGetList отвечает двумя-тремя проходами и может ответить ERROR_MORE_DATA
//     сколько угодно раз, если процессы уходят и приходят прямо во время
//     проверки. Отсюда ограниченный цикл (kMaxListPasses) вместо «пока не
//     перестанет».
//
// Чего модуль сознательно не делает. Ни одна функция не бросает наружу
// исключений, кроме std::bad_alloc из std::vector/std::wstring: проверка «кто
// держит файл» стоит в горячем пути сканирования (SPEC §6.4), где бросок
// погасил бы оценку половины кандидатов, а пользователь увидел бы «оценка
// неизвестна» вместо «занято приложением X». Отказ — это status и код в
// результате.
//
// Про `\\?\` и длинные пути. Ни один вызов этого модуля не нормализует пути
// через `\\?\`: нормализация и проверка «путь внутри ожидаемого корня правила» —
// работа vfs-памяти (SPEC §4 FR-6, vfs_paths), и второй нормализатор в слое
// дал бы два разных ответа на один вопрос. Единственное, что делается с путём
// здесь, — завершающий обратный слэш для каталога (resourcePath): без него RM
// ищет файл с таким именем и честно отвечает «держателей нет», хотя каталог
// открыт в приложении. Если путь длиннее MAX_PATH, GetFileAttributesW не
// ответит, resourcePath вернёт путь как есть, и ответ будет честным: RM сам
// скажет, что не нашёл.
//
// Про образ процесса. RM имени образа не отдаёт, а пользователю нужно «какое
// именно приложение» и правилу `requiresProcessesClosed` нужно имя exe
// (FR-3/FR-4). Поэтому по каждому найденному PID делается один
// OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) + QueryFullProcessImageNameW.
// Прав elevated не требуется (SPEC §5: «рантайм-повышения нет»), а у процесса
// чужого пользователя путь останется пустым — и это отдельное состояние
// (imagePathKnown == false), а не «приложение неизвестно».
#include "restart_manager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// windows.h идёт первым: RestartManager.h объявляет WCHAR, DWORD, FILETIME,
// BOOL и макрос WINAPI_FAMILY_PARTITION, которых без него просто нет.
#include <windows.h>         // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место
#include <RestartManager.h>  // RmStartSession, RmRegisterResources, RmGetList (FR-6)

#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::restart_manager {
namespace {

using ScopedHandle = platform::unique_handle<platform::KernelHandlePolicy>;

// Имя события модуля в логе — одной строкой, чтобы по grep находились все
// записи.
constexpr std::string_view kLogEvent = "platform.restart_manager";

// Коды RM, которых нет в winerror.h. В RestartManager.h они тоже не
// объявлены — приходят как «прочие отказы» — но по смыслу это не «прочие»:
// первый означает, что сессия не может работать в принципе, второй — что её
// закрыли (вызвать проверку заново имеет смысл, повторять внутри сессии — нет).
constexpr std::uint32_t kRmSessionCritical = 494;
constexpr std::uint32_t kRmSessionEnded = 495;

// Раскладка ответов RM фиксируется сборкой, а не догадкой: если в следующем
// SDK структуры изменятся, модуль обязан остановиться на этапе сборки, а не
// читать поля по старым смещениям на живой системе.
static_assert(sizeof(RM_UNIQUE_PROCESS) == 12, "RM_UNIQUE_PROCESS: DWORD + FILETIME + выравнивание");
static_assert(offsetof(RM_PROCESS_INFO, Process) == 0,
              "идентификатор обязан лежать в начале RM_PROCESS_INFO: на этом держится второй проход RmGetList");
static_assert(sizeof(RM_PROCESS_INFO) >= sizeof(RM_UNIQUE_PROCESS) + sizeof(wchar_t) * (CCH_RM_MAX_APP_NAME + 1),
              "в RM_PROCESS_INFO нет поля strAppName ожидаемого размера");

// FILETIME как одно беззнаковое число. Имя не toUInt64: в соседних модулях
// платформы есть свои fileTimeValue с другой парой перегрузок, и одноимённая
// функция с третьей сигнатурой в том же слое — это ровно тот конфликт имён,
// из-за которого платформа однажды уже перестала собираться.
[[nodiscard]] std::uint64_t fileTimeValue(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | static_cast<std::uint64_t>(time.dwLowDateTime);
}

// Длина широкой строки в пределах поля фиксированного размера. wcsnlen из CRT
// здесь не используется намеренно: у массива фиксированного размера CCH_*
// нет завершающего нуля в гарантии, и читать его можно ровно настолько, на
// сколько поле даёт.
[[nodiscard]] std::size_t boundedLength(const wchar_t* text, std::size_t limit) noexcept {
    if (text == nullptr) return 0;
    std::size_t length = 0;
    while (length < limit && text[length] != L'\0') {
        ++length;
    }
    return length;
}

[[nodiscard]] std::wstring fixedField(const wchar_t* text, std::size_t limit) {
    return std::wstring(text == nullptr ? L"" : text, boundedLength(text, limit));
}

// Тип приложения RM → своё перечисление. Unknown означает и «не
// классифицировано», и «значение из будущего SDK»: показывать пользователю
// «приложение не распознано» и не выдавать видовых утверждений в обоих
// случаях — одно и то же правильно.
[[nodiscard]] AppType toAppType(RM_APP_TYPE raw) noexcept {
    switch (raw) {
        case RmUnknownApp:
            return AppType::Unknown;
        case RmMainWindow:
            return AppType::MainWindow;
        case RmOtherWindow:
            return AppType::OtherWindow;
        case RmService:
            return AppType::Service;
        case RmExplorer:
            return AppType::Explorer;
        case RmConsole:
            return AppType::Console;
        case RmCritical:
            return AppType::Critical;
    }
    return AppType::Unknown;
}

// Ответ по образу процесса: путь и время создания, если их удалось прочитать.
// Разделено полями, а не одним «получилось/не получилось», потому что путь
// читается одним вызовом, а время старта — другим, и они могут разойтись на
// процессе, который умер между ними.
struct ImageProbe {
    std::wstring path;
    bool pathKnown{};
    std::uint64_t startTime{};
    bool startTimeKnown{};
};

// Один дескриптор на процесс, два вопроса к нему, никаких прав сверх
// минимальных: PROCESS_QUERY_LIMITED_INFORMATION достаточно и для пути к
// образу, и для GetProcessTimes, поэтому повышение прав не требуется ни при
// одном варианте (SPEC §5). Отказ — пустой результат: имя процесса из RM
// остаётся, теряется только путь.
[[nodiscard]] ImageProbe probeImage(DWORD pid) noexcept {
    ImageProbe probe;
    if (pid == 0) return probe;

    const ScopedHandle process = platform::adopt(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid()) return probe;

    wchar_t buffer[MAX_PATH] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    if (::QueryFullProcessImageNameW(process.get(), 0, buffer, &size) != FALSE) {
        // size на выходе — длина без завершающего нуля.
        probe.path.assign(buffer, static_cast<std::size_t>(size));
        probe.pathKnown = true;
    }

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(process.get(), &creation, &exitTime, &kernel, &user) != FALSE) {
        probe.startTime = fileTimeValue(creation);
        probe.startTimeKnown = true;
    }
    return probe;
}

// ---------------------------------------------------------------------------
// Коды Win32 → статусы модуля
// ---------------------------------------------------------------------------

[[nodiscard]] RegisterStatus classifyRegister(std::uint32_t code) noexcept {
    switch (code) {
        case ERROR_SUCCESS:
            return RegisterStatus::Ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            // «Пути уже нет» — это не блокировка. Показывать пользователю «файл
            // держит процесс» для кандидата, который исчез сам, значит врать.
            return RegisterStatus::ResourceNotFound;
        case ERROR_ACCESS_DENIED:
            return RegisterStatus::AccessDenied;
        case ERROR_INVALID_HANDLE:
            // Сессию закрыли извне (или её нет вовсе) — её надо открыть заново.
            return RegisterStatus::NoSession;
        case ERROR_INVALID_PARAMETER:
            return RegisterStatus::InvalidArgument;
        case kRmSessionCritical:
            return RegisterStatus::SessionCritical;
        case kRmSessionEnded:
            return RegisterStatus::SessionEnded;
        default:
            return RegisterStatus::Failed;
    }
}

[[nodiscard]] QueryStatus classifyQuery(std::uint32_t code) noexcept {
    switch (code) {
        case ERROR_SUCCESS:
            return QueryStatus::Ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return QueryStatus::ResourceNotFound;
        case ERROR_ACCESS_DENIED:
            return QueryStatus::AccessDenied;
        case ERROR_INVALID_HANDLE:
            return QueryStatus::NoSession;
        case ERROR_INVALID_PARAMETER:
            return QueryStatus::InvalidArgument;
        case kRmSessionEnded:
            return QueryStatus::SessionEnded;
        case ERROR_OUTOFMEMORY:
            return QueryStatus::OutOfMemory;
        case ERROR_MORE_DATA:
            // Ответ успел измениться столько раз, сколько раз позволил
            // kMaxListPasses. Повторять проверку заново имеет смысл, верить
            // этому ответу — нет. Код тот же, что у любого неопознанного, и
            // ветвь объединена с default: держать две копии одного отказа
            // значило бы рисковать забыть поправить одну из них — и тогда
            // «список изменился» молча превратился бы в «список неверен».
        default:
            return QueryStatus::Failed;
    }
}

[[nodiscard]] QueryStatus classifyStart(std::uint32_t code) noexcept {
    if (code == ERROR_OUTOFMEMORY) return QueryStatus::OutOfMemory;
    return QueryStatus::Failed;
}

// ---------------------------------------------------------------------------
// Двухпроходный (на деле трёхпроходный) RmGetList
// ---------------------------------------------------------------------------

// Что удалось вытащить из RM.
struct ListFetch {
    bool ok{false};
    bool tooLarge{false}; // RM ответил числом процессов выше предохранителя
    std::uint32_t win32Error{ERROR_SUCCESS};
    std::uint32_t rebootReasons{};
    std::vector<RM_PROCESS_INFO> info;
};

// Три вызова на один ответ, и порядок обязателен.
//
//   1. rgAffectedApps = nullptr, pnProcInfo = 0. RM отвечает ERROR_MORE_DATA и
//      сообщает, сколько процессов затронуто: нужен массив на это число.
//   2. rgAffectedApps = массив ключей. RM сообщает, сколько записей
//      RM_PROCESS_INFO понадобится для описаний.
//   2. rgAffectedApps = массив, в который RM записывает идентификаторы
//      (RM_UNIQUE_PROCESS, лежат в начале RM_PROCESS_INFO) и сообщает, сколько
//      записей RM_PROCESS_INFO понадобится для описаний;
//   3. rgAffectedApps = тот же массив, выросший до этого числа. Идентификаторы
//      в нём уже есть — RM использует пару (PID, время старта) как входной
//      признак записи, поэтому расти массив можно не теряя их.
//
// Замечание о типе четвёртого аргумента. В заголовке SDK это
// `RM_PROCESS_INFO rgAffectedApps[]`, а не массив идентификаторов, поэтому на
// втором проходе передаётся массив RM_PROCESS_INFO, а не RM_UNIQUE_PROCESS:
// неявного приведения между этими типами указателей в C++ нет, и пример из
// MSDN написан в расчёте на то, что объявленный тип там другой. Раскладку
// фиксируют static_assert выше: идентификатор обязан лежать в начале
// описания, иначе второй проход записал бы «в никуда».
//
// Между проходами состав держателей может измениться, поэтому ERROR_MORE_DATA
// на третьем вызове означает «ещё раз», а не «сдался». Цикл ограничен
// kMaxListPasses: ответ, который не успокаивается, — это гонка с живой
// системой, и честнее вернуть отказ, чем крутить цикл до бесконечности.
[[nodiscard]] ListFetch fetchProcessInfo(DWORD sessionKey, std::size_t maxProcesses) noexcept {
    ListFetch fetch;
    std::vector<RM_PROCESS_INFO> info;

    for (int pass = 0; pass < kMaxListPasses; ++pass) {
        DWORD rebootReasons = 0;

        UINT needed = 0;
        UINT provided = 0;
        const DWORD first = ::RmGetList(sessionKey, &needed, &provided, nullptr, &rebootReasons);
        if (first != ERROR_SUCCESS && first != ERROR_MORE_DATA) {
            fetch.win32Error = static_cast<std::uint32_t>(first);
            return fetch;
        }
        if (needed == 0) {
            // Никто не держит — это полноценный ответ, а не «не удалось
            // спросить». Именно он разрешает удалять без вопросов (FR-5).
            fetch.ok = true;
            fetch.rebootReasons = static_cast<std::uint32_t>(rebootReasons);
            return fetch;
        }
        if (static_cast<std::size_t>(needed) > maxProcesses) {
            fetch.tooLarge = true;
            fetch.win32Error = ERROR_INVALID_DATA;
            return fetch;
        }

        // Второй вызов заполняет в массиве только пары (PID, время старта) —
        // это и есть идентификаторы держателей, которые RM знает. Дальше тот
        // же массив используется под описания, поэтому лишних структур и
        // копирования идентификаторов не понадобится.
        info.assign(needed, RM_PROCESS_INFO{});
        UINT identityCapacity = needed;
        UINT infoNeeded = 0;
        const DWORD second = ::RmGetList(sessionKey, &infoNeeded, &identityCapacity, info.data(), &rebootReasons);
        if (second != ERROR_SUCCESS && second != ERROR_MORE_DATA) {
            fetch.win32Error = static_cast<std::uint32_t>(second);
            return fetch;
        }
        if (infoNeeded == 0) {
            fetch.ok = true;
            fetch.rebootReasons = static_cast<std::uint32_t>(rebootReasons);
            return fetch;
        }
        if (static_cast<std::size_t>(infoNeeded) > maxProcesses) {
            fetch.tooLarge = true;
            fetch.win32Error = ERROR_INVALID_DATA;
            return fetch;
        }

        // Расширение массива сохраняет уже заполненные Process и обнуляет
        // хвост: идентификаторы остаются входными данными третьего вызова.
        info.resize(infoNeeded);
        UINT described = infoNeeded;
        UINT capacity = infoNeeded;
        const DWORD third = ::RmGetList(sessionKey, &described, &capacity, info.data(), &rebootReasons);
        if (third == ERROR_MORE_DATA) {
            // Держатели добавились между проходами — начинаем сначала.
            continue;
        }
        if (third != ERROR_SUCCESS) {
            fetch.win32Error = static_cast<std::uint32_t>(third);
            return fetch;
        }

        if (static_cast<std::size_t>(described) < info.size()) {
            info.resize(static_cast<std::size_t>(described));
        }
        fetch.info = std::move(info);
        fetch.rebootReasons = static_cast<std::uint32_t>(rebootReasons);
        fetch.ok = true;
        return fetch;
    }

    fetch.win32Error = static_cast<std::uint32_t>(ERROR_MORE_DATA);
    return fetch;
}

// Один ответ RM → ProcessInfo с добиранием пути к образу и проверкой, что PID
// всё ещё тот же процесс.
[[nodiscard]] ProcessInfo toProcessInfo(const RM_PROCESS_INFO& raw, const QueryOptions& options) noexcept {
    ProcessInfo info;
    info.pid = static_cast<std::uint32_t>(raw.Process.dwProcessId);
    info.startTime = fileTimeValue(raw.Process.ProcessStartTime);
    info.appName = fixedField(raw.strAppName, static_cast<std::size_t>(CCH_RM_MAX_APP_NAME) + 1u);
    info.serviceName = fixedField(raw.strServiceShortName, static_cast<std::size_t>(CCH_RM_MAX_SVC_NAME) + 1u);
    info.type = toAppType(raw.ApplicationType);
    info.appStatus = static_cast<std::uint32_t>(raw.AppStatus);
    info.restartable = raw.bRestartable != FALSE;

    // TSSessionId == 0xFFFFFFFF — это «сессия неприменима», а не сессия
    // 4294967295. В модели хранится -1, иначе это число уедет в UI и в отчёт
    // (SPEC §8) как настоящее.
    const std::uint32_t session = static_cast<std::uint32_t>(raw.TSSessionId);
    const bool noSession = session == static_cast<std::uint32_t>(RM_INVALID_TS_SESSION);
    info.tsSessionId = noSession ? -1 : static_cast<std::int32_t>(session);

    info.self = info.pid == ::GetCurrentProcessId();

    if (!options.resolveImagePaths) return info;

    const ImageProbe probe = probeImage(raw.Process.dwProcessId);
    if (probe.pathKnown) {
        info.imagePath = probe.path;
        info.imagePathKnown = true;
    }
    // Время старта есть и не совпало — PID уже за другим процессом. Имя и PID
    // из ответа RM к этому моменту относятся к мёртвому процессу, и предлагать
    // пользователю «закрыть PID N» было бы предложением закрыть не то.
    if (probe.startTimeKnown && info.startTime != 0 && probe.startTime != info.startTime) {
        info.pidReused = true;
    }
    return info;
}

// Порядок и уникальность ответа. RM возвращает по записи на каждый затронутый
// ресурс, поэтому один и тот же процесс встречается столько раз, сколько его
// файлов стоит в наборе. Повторы в UI выглядели бы как «четыре приложения», а
// в отчёте (FR-8) завышали бы число держателей. Порядок по PID — чтобы дамп
// был сравним между прогонами: золотые тесты и diff отчётов не терпят
// перестановку.
void sortAndDeduplicate(std::vector<ProcessInfo>& processes) noexcept {
    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& left, const ProcessInfo& right) {
        if (left.pid != right.pid) return left.pid < right.pid;
        return left.startTime < right.startTime;
    });

    const auto sameProcess = [](const ProcessInfo& left, const ProcessInfo& right) {
        return left.pid == right.pid && left.startTime == right.startTime;
    };
    processes.erase(std::unique(processes.begin(), processes.end(), sameProcess), processes.end());
}

// Заголовок для записи в лог. Поля собираются вручную: макросы MRP_LOG_* для
// списка из двух и более пар непригодны (detail::logFieldList разворачивает
// пакет в один вызов logField, и такой вызов не разрешается). Чужой заголовок
// не правим.
[[nodiscard]] mrproper::core::LogFields failureFields(const LockResult& result) {
    mrproper::core::LogFields fields;
    fields.reserve(6);
    fields.push_back(mrproper::core::logField("status", toString(result.status)));
    fields.push_back(mrproper::core::logField("resources", result.registeredResources));
    fields.push_back(mrproper::core::logField("processes", static_cast<std::uint64_t>(result.processes.size())));
    if (result.win32Error != 0u) {
        const DWORD code = static_cast<DWORD>(result.win32Error);
        fields.push_back(mrproper::core::logField("win32", static_cast<std::uint64_t>(result.win32Error)));
        fields.push_back(mrproper::core::logField("errorText", platform::win32ErrorText(code)));
    }
    if (result.rebootReasons != 0u) {
        fields.push_back(mrproper::core::logField("rebootReasons", static_cast<std::uint64_t>(result.rebootReasons)));
    }
    return fields;
}

void logFailure(const LockResult& result, std::string_view message, const QueryOptions& options) noexcept {
    if (!options.logFailures) return;
    mrproper::core::logWarn(kLogEvent, message, failureFields(result));
}

// Часть описания, которая добавляется только когда что-то включено: иначе
// «перезапуск: нет, остановлено: нет, ошибка остановки: нет» в каждой строке
// лога превращается в шум, в котором не видно настоящей новости.
void appendFlag(std::string& out, const char* name, bool value) {
    if (!value) return;
    out += " ";
    out += name;
    out += "=да";
}

[[nodiscard]] const wchar_t* fileNameOf(const std::wstring& path) noexcept {
    if (path.empty()) return nullptr;
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return path.c_str();
    if (separator + 1 >= path.size()) return nullptr; // путь кончается разделителем
    return path.c_str() + separator + 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Имена состояний и их тексты
// ---------------------------------------------------------------------------

const char* toString(AppType type) noexcept {
    switch (type) {
        case AppType::MainWindow:
            return "main_window";
        case AppType::OtherWindow:
            return "other_window";
        case AppType::Service:
            return "service";
        case AppType::Explorer:
            return "explorer";
        case AppType::Console:
            return "console";
        case AppType::Critical:
            return "critical";
        case AppType::Unknown:
            break;
    }
    return "unknown";
}

const wchar_t* toWideString(AppType type) noexcept {
    switch (type) {
        case AppType::MainWindow:
            return L"main_window";
        case AppType::OtherWindow:
            return L"other_window";
        case AppType::Service:
            return L"service";
        case AppType::Explorer:
            return L"explorer";
        case AppType::Console:
            return L"console";
        case AppType::Critical:
            return L"critical";
        case AppType::Unknown:
            break;
    }
    return L"unknown";
}

const char* toString(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::Ok:
            return "ok";
        case QueryStatus::NoSession:
            return "no_session";
        case QueryStatus::InvalidArgument:
            return "invalid_argument";
        case QueryStatus::ResourceNotFound:
            return "resource_not_found";
        case QueryStatus::AccessDenied:
            return "access_denied";
        case QueryStatus::SessionEnded:
            return "session_ended";
        case QueryStatus::AnswerTooLarge:
            return "answer_too_large";
        case QueryStatus::Failed:
            return "failed";
        case QueryStatus::OutOfMemory:
            break;
    }
    return "out_of_memory";
}

const char* toString(RegisterStatus status) noexcept {
    switch (status) {
        case RegisterStatus::Ok:
            return "ok";
        case RegisterStatus::NoSession:
            return "no_session";
        case RegisterStatus::InvalidArgument:
            return "invalid_argument";
        case RegisterStatus::ResourceNotFound:
            return "resource_not_found";
        case RegisterStatus::AccessDenied:
            return "access_denied";
        case RegisterStatus::SessionCritical:
            return "session_critical";
        case RegisterStatus::SessionEnded:
            return "session_ended";
        case RegisterStatus::Failed:
            break;
    }
    return "failed";
}

const wchar_t* toWideString(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::Ok:
            return L"ok";
        case QueryStatus::NoSession:
            return L"no_session";
        case QueryStatus::InvalidArgument:
            return L"invalid_argument";
        case QueryStatus::ResourceNotFound:
            return L"resource_not_found";
        case QueryStatus::AccessDenied:
            return L"access_denied";
        case QueryStatus::SessionEnded:
            return L"session_ended";
        case QueryStatus::AnswerTooLarge:
            return L"answer_too_large";
        case QueryStatus::Failed:
            return L"failed";
        case QueryStatus::OutOfMemory:
            break;
    }
    return L"out_of_memory";
}

std::wstring formatStatusWide(QueryStatus status, std::uint32_t win32Error) {
    if (status == QueryStatus::Ok) {
        return L"проверка выполнена";
    }

    // Имя состояния плюс текст самой системы: «ERROR_ACCESS_DENIED = 5» в логе
    // читается хуже, чем «Отказано в доступе» (SPEC §5, §12).
    std::wstring text = toWideString(status);
    const std::wstring detail = platform::formatSystemMessage(static_cast<DWORD>(win32Error));
    if (!detail.empty()) {
        text += L": ";
        text += detail;
    }
    return text;
}

std::string formatStatus(QueryStatus status, std::uint32_t win32Error) {
    return platform::toUtf8(formatStatusWide(status, win32Error));
}

QueryStatus toQueryStatus(RegisterStatus status) noexcept {
    switch (status) {
        case RegisterStatus::Ok:
            return QueryStatus::Ok;
        case RegisterStatus::NoSession:
            return QueryStatus::NoSession;
        case RegisterStatus::InvalidArgument:
            return QueryStatus::InvalidArgument;
        case RegisterStatus::ResourceNotFound:
            return QueryStatus::ResourceNotFound;
        case RegisterStatus::AccessDenied:
            return QueryStatus::AccessDenied;
        case RegisterStatus::SessionEnded:
            return QueryStatus::SessionEnded;
        case RegisterStatus::SessionCritical:
        case RegisterStatus::Failed:
            // Отдельного состояния для SessionCritical в ответе нет: это
            // «проверка не удалась», а отличать её от других отказов должен код
            // Win32 в отчёте, а не тип в UI.
            break;
    }
    return QueryStatus::Failed;
}

bool isRetryable(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::SessionEnded: // сессию открыли заново — имеет смысл
        case QueryStatus::Failed:       // отказ RM может быть временным
        case QueryStatus::OutOfMemory:  // память появится
            return true;
        case QueryStatus::Ok:
        case QueryStatus::NoSession:
        case QueryStatus::InvalidArgument:
        case QueryStatus::ResourceNotFound:
        case QueryStatus::AccessDenied:
        case QueryStatus::AnswerTooLarge:
            break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ProcessInfo
// ---------------------------------------------------------------------------

std::wstring ProcessInfo::displayName() const {
    if (!appName.empty()) return appName;
    if (!serviceName.empty()) return serviceName;
    if (const wchar_t* file = fileNameOf(imagePath); file != nullptr && *file != L'\0') {
        return std::wstring(file);
    }
    // Номер процесса пользователю ничего не говорит, но это последний
    // вариант: «что-то держит файл» лучше, чем пустое имя.
    return L"PID " + std::to_wstring(pid);
}

const ProcessInfo* LockResult::findPid(std::uint32_t pid) const noexcept {
    const auto found = std::find_if(processes.begin(), processes.end(),
                                    [pid](const ProcessInfo& process) { return process.pid == pid; });
    return found == processes.end() ? nullptr : &*found;
}

// ---------------------------------------------------------------------------
// Сессия
// ---------------------------------------------------------------------------

Session::Session() noexcept {
    wchar_t sessionKeyText[CCH_RM_SESSION_KEY + 1] = {};
    DWORD sessionKey = 0;
    // dwSessionFlags == 0: параметр зарезервирован самой Restart Manager и
    // обязан быть нулём, поэтому наружу он не выведен — вызывающему нечего
    // передавать, и передавать было бы ошибкой.
    const DWORD result = ::RmStartSession(&sessionKey, 0, sessionKeyText);
    if (result != ERROR_SUCCESS) {
        // Сессии нет — но startError() обязан её описать, иначе отказ
        // «Restart Manager недоступен» и отказ «нет памяти» в отчёте
        // неразличимы.
        startError_ = static_cast<std::uint32_t>(result);
        return;
    }
    sessionKey_ = sessionKey;
}

Session::~Session() noexcept {
    end();
}

Session::Session(Session&& other) noexcept
    : sessionKey_(other.sessionKey_),
      startError_(other.startError_),
      registerError_(other.registerError_),
      registeredResources_(other.registeredResources_) {
    other.sessionKey_ = 0;
    other.startError_ = 0;
    other.registerError_ = 0;
    other.registeredResources_ = 0;
}

Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        // Сначала закрыть своё, потом забрать чужое: если забыть, останется
        // сессия, которую уже никто не закроет, — ровно та утечка, ради которой
        // класс и написан.
        end();
        sessionKey_ = other.sessionKey_;
        startError_ = other.startError_;
        registerError_ = other.registerError_;
        registeredResources_ = other.registeredResources_;
        other.sessionKey_ = 0;
        other.startError_ = 0;
        other.registerError_ = 0;
        other.registeredResources_ = 0;
    }
    return *this;
}

RegisterStatus Session::registerResources(const std::vector<std::wstring>& files,
                                          const std::vector<std::wstring>& services) noexcept {
    registerError_ = 0;
    registeredResources_ = 0;
    if (!started()) return RegisterStatus::NoSession;
    if (files.empty() && services.empty()) return RegisterStatus::InvalidArgument;

    // RmRegisterResources принимает LPCWSTR*, а не массив строк: собираем
    // указатели один раз. Пустой путь в наборе — ошибка вызывающего, а не
    // повод молча выкинуть элемент: регистрация идёт целиком, и результат
    // «Ok» при выброшенном пути означал бы, что проверен не тот набор.
    std::vector<LPCWSTR> fileNames;
    std::vector<LPCWSTR> serviceNames;
    fileNames.reserve(files.size());
    serviceNames.reserve(services.size());
    for (const std::wstring& file : files) {
        if (file.empty()) return RegisterStatus::InvalidArgument;
        fileNames.push_back(file.c_str());
    }
    for (const std::wstring& service : services) {
        if (service.empty()) return RegisterStatus::InvalidArgument;
        serviceNames.push_back(service.c_str());
    }
    if (fileNames.empty() && serviceNames.empty()) return RegisterStatus::InvalidArgument;

    const DWORD result = ::RmRegisterResources(sessionKey_, static_cast<UINT>(fileNames.size()), fileNames.data(), 0,
                                               nullptr, static_cast<UINT>(serviceNames.size()), serviceNames.data());
    if (result != ERROR_SUCCESS) {
        registerError_ = static_cast<std::uint32_t>(result);
        return classifyRegister(registerError_);
    }
    registeredResources_ = fileNames.size() + serviceNames.size();
    return RegisterStatus::Ok;
}

RegisterStatus Session::registerFiles(const std::vector<std::wstring>& files) noexcept {
    return registerResources(files, {});
}

QueryStatus Session::list(LockResult& out, const QueryOptions& options) noexcept {
    // Очистка до любой проверки: иначе при отказе в результате остался бы
    // список от прошлого успешного вызова, который выглядел бы как ответ на
    // текущий вопрос.
    out.status = QueryStatus::NoSession;
    out.win32Error = 0;
    out.processes.clear();
    out.rebootReasons = 0;
    out.registeredResources = registeredResources_;

    if (!started()) {
        out.win32Error = startError_;
        return out.status;
    }

    const std::size_t maxProcesses = options.maxProcesses == 0 ? kMaxProcessesFromRm : options.maxProcesses;
    const ListFetch fetch = fetchProcessInfo(sessionKey_, maxProcesses);
    if (!fetch.ok) {
        out.win32Error = fetch.win32Error;
        out.status = fetch.tooLarge ? QueryStatus::AnswerTooLarge : classifyQuery(fetch.win32Error);
        return out.status;
    }

    out.rebootReasons = fetch.rebootReasons;
    out.processes.reserve(fetch.info.size());
    for (const RM_PROCESS_INFO& raw : fetch.info) {
        out.processes.push_back(toProcessInfo(raw, options));
    }
    sortAndDeduplicate(out.processes);
    out.status = QueryStatus::Ok;
    return out.status;
}

LockResult Session::list(const QueryOptions& options) {
    LockResult out;
    (void)list(out, options);
    return out;
}

void Session::end() noexcept {
    if (sessionKey_ == 0) return;
    const DWORD sessionKey = sessionKey_;
    // Обнуление до вызова: RmEndSession может вызвать статическую
    // деструктуризацию где-то в RM, и повторный end() из деструктора после
    // исключения обязан быть no-op, а не вторым RmEndSession на тот же ключ.
    sessionKey_ = 0;
    registeredResources_ = 0;

    const DWORD result = ::RmEndSession(sessionKey);
    if (result != ERROR_SUCCESS) {
        // Отказ здесь не восстановим: сессия уже не наша. Пишем в лог, потому
        // что незакрытая сессия — это утечка во внешней службе, и заметить её
        // больше негде.
        mrproper::core::LogFields fields;
        fields.push_back(mrproper::core::logField("win32", static_cast<std::uint64_t>(result)));
        fields.push_back(mrproper::core::logField("errorText", platform::win32ErrorText(static_cast<DWORD>(result))));
        mrproper::core::logDebug(kLogEvent, "сессия Restart Manager закрылась с ошибкой", fields);
    }
}

// ---------------------------------------------------------------------------
// Проверка одним вызовом
// ---------------------------------------------------------------------------

std::wstring resourcePath(std::wstring_view path) {
    if (path.empty()) return {};
    // Уже готовая форма каталога: проверять нечего, а лишний GetFileAttributesW
    // на каждый путь каталога в наборе — это лишняя работа.
    if (path.back() == L'\\' || path.back() == L'/') return std::wstring(path);

    const std::wstring text(path);
    const DWORD attributes = ::GetFileAttributesW(text.c_str());
    // Несуществующий путь возвращаем как есть: RmRegisterResources ответит
    // ERROR_FILE_NOT_FOUND, и это правильный ответ, а не догадка модуля.
    if (attributes == INVALID_FILE_ATTRIBUTES) return text;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return text;

    std::wstring directory = text;
    directory.push_back(L'\\');
    return directory;
}

LockResult whoLocksFiles(const std::vector<std::wstring>& files, const QueryOptions& options) {
    LockResult result;
    if (files.empty()) {
        // «Проверять нечего» и «проверили, никто не держит» — разные
        // утверждения, и только второе разрешает удалять (FR-5).
        result.status = QueryStatus::InvalidArgument;
        result.win32Error = 0;
        return result;
    }

    std::vector<std::wstring> paths;
    paths.reserve(files.size());
    for (const std::wstring& file : files) {
        paths.push_back(options.markDirectories ? resourcePath(file) : std::wstring(file));
    }

    Session session;
    if (!session.started()) {
        result.status = classifyStart(session.startError());
        result.win32Error = session.startError();
        logFailure(result, "сессия Restart Manager не открылась", options);
        return result;
    }

    const RegisterStatus registered = session.registerFiles(paths);
    if (registered != RegisterStatus::Ok) {
        result.status = toQueryStatus(registered);
        result.win32Error = session.registerError();
        result.registeredResources = 0;
        logFailure(result, "ресурсы не зарегистрированы в Restart Manager", options);
        return result;
    }

    result.registeredResources = paths.size();
    const QueryStatus status = session.list(result, options);
    if (status != QueryStatus::Ok) {
        logFailure(result, "Restart Manager не ответил, кто держит файл", options);
    }
    return result;
}

LockResult whoLocksFile(std::wstring_view path, const QueryOptions& options) {
    LockResult result;
    if (path.empty()) {
        result.status = QueryStatus::InvalidArgument;
        return result;
    }
    // Один путь — тот же вызов: одна реализация проверки, а не две, которые
    // со временем ответят на один вопрос по-разному.
    return whoLocksFiles(std::vector<std::wstring>{std::wstring(path)}, options);
}

LockResult whoLocksFilesUtf8(const std::vector<std::string>& files, const QueryOptions& options) {
    LockResult result;
    if (files.empty()) {
        result.status = QueryStatus::InvalidArgument;
        return result;
    }

    std::vector<std::wstring> paths;
    paths.reserve(files.size());
    for (const std::string& file : files) {
        // Нечитаемый UTF-8 молча выкидывать нельзя: «файл свободен» вместо
        // «путь не удалось прочитать» — это удаление не того файла.
        if (file.empty() || !platform::isValidUtf8(file)) {
            result.status = QueryStatus::InvalidArgument;
            result.win32Error = 0;
            return result;
        }
        paths.push_back(platform::toUtf16(file));
    }
    return whoLocksFiles(paths, options);
}

// ---------------------------------------------------------------------------
// Вспомогательное
// ---------------------------------------------------------------------------

std::vector<core::ProcessRef> toProcessRefs(const LockResult& result) {
    std::vector<core::ProcessRef> refs;
    if (result.processes.empty()) return refs;

    refs.reserve(result.processes.size());
    for (const ProcessInfo& process : result.processes) {
        // PID уже за другим процессом: имя в модели читает интерфейс и отчёт
        // (FR-8), а там мёртвый держатель выглядит как живой.
        if (process.pidReused) continue;
        core::ProcessRef ref;
        ref.pid = process.pid;
        ref.name = platform::toUtf8(process.displayName());
        refs.push_back(std::move(ref));
    }
    return refs;
}

std::string describeRebootReasons(std::uint32_t rebootReasons) {
    if (rebootReasons == 0u) return {};

    // Имена по RM_REBOOT_REASON. Порядок — от самого важного к менее важному,
    // потому что строка идёт в сообщение пользователю, где «нужна перезагрузка»
    // без причины бесполезно.
    const std::uint32_t criticalProcess = static_cast<std::uint32_t>(RmRebootReasonCriticalProcess);
    const std::uint32_t criticalService = static_cast<std::uint32_t>(RmRebootReasonCriticalService);
    const std::uint32_t permissionDenied = static_cast<std::uint32_t>(RmRebootReasonPermissionDenied);
    const std::uint32_t sessionMismatch = static_cast<std::uint32_t>(RmRebootReasonSessionMismatch);
    const std::uint32_t detectedSelf = static_cast<std::uint32_t>(RmRebootReasonDetectedSelf);

    std::string out = "нужна перезагрузка (";
    bool first = true;
    const auto add = [&out, &first](const char* reason) {
        if (!first) out += ", ";
        out += reason;
        first = false;
    };
    if ((rebootReasons & criticalProcess) != 0u) add("критичный процесс");
    if ((rebootReasons & criticalService) != 0u) add("критичная служба");
    if ((rebootReasons & permissionDenied) != 0u) add("нет прав закрыть процесс");
    if ((rebootReasons & sessionMismatch) != 0u) add("процесс чужой сессии");
    if ((rebootReasons & detectedSelf) != 0u) add("держит сам MrProper");
    out += ")";
    return out;
}

std::string describe(const ProcessInfo& process) {
    std::string out = platform::toUtf8(process.displayName());
    out += " (pid=";
    out += std::to_string(process.pid);
    out += ", type=";
    out += toString(process.type);
    appendFlag(out, "self", process.self);
    appendFlag(out, "restartable", process.restartable);
    appendFlag(out, "running", process.running());
    appendFlag(out, "stopped", process.stoppedByManager());
    appendFlag(out, "stopped_outside", process.stoppedOutside());
    appendFlag(out, "error_on_stop", process.errorOnStop());
    appendFlag(out, "error_on_restart", process.errorOnRestart());
    appendFlag(out, "shutdown_masked", process.shutdownMasked());
    appendFlag(out, "restart_masked", process.restartMasked());
    appendFlag(out, "pid_reused", process.pidReused);
    appendFlag(out, "image_unknown", !process.imagePathKnown);
    if (process.tsSessionId >= 0) {
        out += " tsSession=";
        out += std::to_string(process.tsSessionId);
    }
    if (process.imagePathKnown && !process.imagePath.empty()) {
        out += " image=";
        out += platform::toUtf8(process.imagePath);
    }
    return out;
}

std::string describe(const LockResult& result) {
    std::string out = "status=";
    out += toString(result.status);
    out += " resources=";
    out += std::to_string(result.registeredResources);
    out += " processes=";
    out += std::to_string(result.processes.size());
    if (result.win32Error != 0u) {
        out += " win32=";
        out += std::to_string(result.win32Error);
        out += " (";
        out += platform::win32ErrorText(static_cast<DWORD>(result.win32Error));
        out += ")";
    }
    const std::string reboot = describeRebootReasons(result.rebootReasons);
    if (!reboot.empty()) {
        out += " ";
        out += reboot;
    }
    return out;
}

}  // namespace mrproper::platform::restart_manager
