// Restart Manager: кто держит файл. RmStartSession → RmRegisterResources →
// RmGetList. Спека: §4 FR-6 («Блокировки: определение через Restart Manager,
// опция "закрыть эти приложения" (RmShutdown) с подтверждением; повтор с
// backoff 3×»), §4 FR-4 (поле `LockedBy` у кандидата: «процессы, удерживающие
// файлы (Restart Manager: RmStartSession → RmRegisterResources → RmGetList)»),
// §6.3 (`LockedBy: std::vector<ProcessRef>`), §5 (приложение стартует без
// повышения прав), §9.1 ADR-004 (windows.h только в platform/).
//
// Зачем именно Restart Manager, а не «просто попробовать удалить». На NTFS
// файл, который держит чужой процесс, отличается от «файла нет» одним кодом
// ERROR_SHARING_VIOLATION, и по нему нельзя сказать пользователю, ЧТО мешает.
// «Отказано в доступе» и «файл держит приложение, которое можно закрыть» — это
// разные решения с разными последствиями: первое лечится правами и повтором,
// второе — вопросом к пользователю. FR-4 требует показывать `LockedBy`
// пользователю, а значит модуль обязан назвать процесс, а не код ошибки.
//
// Чего модуль НЕ делает и кто делает это вместо него.
//
//   * `RmShutdown` / `RmRestart` («закрыть эти приложения» с подтверждением,
//     FR-6) — соседний модуль `platform::process_control`. Закрытие чужих
//     процессов — отдельное решение, отдельная проверка и отдельный отчёт, а
//     не побочный эффект «спросить, кто держит». Здесь только чтение:
//     RmGetList не закрывает и не меняет ничего;
//   * повтор с backoff 3× (FR-6) — движок очистки. Модуль даёт причину отказа и
//     готовый признак «повтор имеет смысл» (`isRetryable`), но крутит цикл
//     тот, кому видно, что именно повторяется;
//   * «файл освободился» — это проверка в момент удаления, её делает
//     `platform::vfs::deleteEntry` (vfs_delete), где и живут коды блокировки;
//   * сбор путей-кандидатов и решение «проверять ли этот кандидат» — движок.
//     Здесь ровно один вызов: «кто держит вот эти N путей».
//
// Протокол вызова (документация Restart Manager). Сессия открывается
// RmStartSession, ресурсы в неё кладутся RmRegisterResources, ответ читается
// RmGetList, сессия закрывается RmEndSession. Три свойства этой API и есть
// весь смысл модуля, поэтому они здесь записаны, а не спрятаны в .cpp:
//
//  1. Сессия обязана закрываться. Незакрытая сессия остаётся в диспетчере RM
//     до выхода процесса: это утечка внутренних ресурсов RM и записи о
//     занятости, которые увидит чужой установщик. Поэтому RmEndSession стоит в
//     деструкторе `Session`, а «вспомнил — закрыл» в модуле не существует;
//  2. Ресурсы в сессии ЗАМЕНЯЮТСЯ, а не накапливаются: повторный
//     RmRegisterResources регистрирует новый набор целиком. Считать, что можно
//     «дозакинуть» путь в уже зарегистрированный набор, нельзя — предыдущий
//     набор перестанет проверяться. Отсюда решение «одна сессия на одну
//     проверку» в `whoLocksFiles`;
//  3. RmGetList — двухпроходная: первый вызов с nullptr сообщает, сколько
//     процессов затронуто, второй и третий заполняют структуры. Ответ приходит
//     в три разных размера, и «забытый второй проход» — это пустой список при
//     занятом файле, то есть ровно та ложь, ради которой модуль написан.
//
// Про каталоги. RM различает файл и каталог по завершающему обратному слэшу:
// имя «C:\Users\me\AppData\Local\Temp» без слэша он ищет как файл с таким
// именем и не находит никого, хотя каталог открыт в редакторе. `resourcePath()`
// добавляет слэш там, где путь действительно каталог (одна проверка
// GetFileAttributesW), а `whoLocksFiles` по умолчанию нормализует пути сам,
// чтобы вызывающий не мог получить «не заблокировано» там, где заблокировано.
//
// Прохладная обёртка поверх RM. У Restart Manager есть одна ловушка, из-за
// которой его стоит звать аккуратно: сам модуль не знает, какие из найденных
// процессов ещё живы, а PID переиспользуются. Пока модуль возвращает имя
// процесса и путь к образу, ответ «закрыть вот это» остаётся осмысленным
// секунды на одну проверку; долгоживущая карточка «кто держит» в отчёте
// (FR-8) обязана сверяться со временем старта процесса, поэтому `ProcessInfo`
// несёт `startTime` (время создания, а не «сейчас») и флаг `pidReused`.
//
// Заголовок намеренно не включает windows.h: все типы переносимые, поэтому его
// можно включить оттуда, где Win32 нет (тесты, дампы, CLI), а слой Win32
// добавляет <windows.h> и <RestartManager.h> сам — так граница слоёв не
// расплывается по одному заголовку (то же решение, что в devices.hpp,
// trim_cache.hpp).
//
// Устойчивость (FR-6: «ошибки не фатальны: собираются в отчёт, остальные
// операции продолжаются»). Ни одна функция модуля не бросает наружу
// исключений, кроме std::bad_alloc: отказ — это `status` плюс код Win32 в
// `LockResult`, а не бросок. Причина одна: этот вызов стоит в горячем пути
// сканирования (§6.4), где исключение из «кто держит файл» погасило бы
// половину оценки кандидатов, и пользователь увидел бы «оценка неизвестна».
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"

namespace mrproper::platform::restart_manager {

// ---------------------------------------------------------------------------
// Пределы
// ---------------------------------------------------------------------------

// Сколько процессов мы готовы принять от RM за один ответ. Число не выведено из
// наблюдений, а взято как предохранитель: ответ RM — это массив структур,
// выделенный по числу, которое сообщил сам RM, и на мусорном числе память
// выделяется не по нашей воле. Обычно ответ — единицы, десятки; сотня
// процессов, держащих один кэш, — уже аномалия, и она должна быть видна в
// логе, а не приводить к аллокации на 256 КБ.
inline constexpr std::size_t kMaxProcessesFromRm = 256;

// Сколько раз повторяется цикл «спросить размер — спросить список» внутри одной
// проверки. Норма — три вызова (nullptr, ключи, описания). Больше трёх нужно
// только тогда, когда процессы уходят и приходят прямо во время проверки, и
// это уже гонка с реальностью, а не метод подбора. Больше — не «на всякий
// случай», а способ зависнуть на минуту.
inline constexpr int kMaxListPasses = 4;

// ---------------------------------------------------------------------------
// Процесс
// ---------------------------------------------------------------------------

// Тип приложения по классификации RM. Своё перечисление, а не RM_APP_TYPE:
// в заголовке нет windows.h, а числовое значение из SDK в интерфейс слоя
// утекать не должно. Unknown означает и «RM не смог классифицировать», и
// «пришло значение, которого в этом SDK нет» — оба случая требуют от
// интерфейса одного и того же: показать имя и не выдавать видовых утверждений.
enum class AppType : std::uint8_t {
    Unknown,      // RmUnknownApp или незнакомое значение
    MainWindow,   // приложение с главным окном (RmMainWindow)
    OtherWindow,  // оконное без главного окна (RmOtherWindow)
    Service,      // служба (RmService)
    Explorer,     // проводник (RmExplorer)
    Console,      // консольное (RmConsole)
    Critical,     // критичный системный процесс: закрывать нельзя (RmCritical)
};

// Стабильное имя типа: не локализуется, идёт в лог и в JSON-отчёт (SPEC §8).
[[nodiscard]] const char* toString(AppType type) noexcept;

// То же в UTF-16 — для UI и текстового дампа, где строки широкие (§6.3).
[[nodiscard]] const wchar_t* toWideString(AppType type) noexcept;

// RM отдаёт AppStatus битовой маской, а не перечислением: состояния
// накладываются друг на друга («работает» и «остановлен другим» одновременно).
// Поэтому здесь биты, а предикаты в ProcessInfo — единственный способ их
// читать.
enum class AppStatusFlag : std::uint32_t {
    Running = 0x0001,        // приложение работает
    Stopped = 0x0002,        // остановлено самим RM (актуально после RmShutdown)
    StoppedOther = 0x0004,   // остановлено извне (пользователем, другим ПО)
    Restarted = 0x0008,      // перезапущено RM
    ErrorOnStop = 0x0010,    // не удалось остановить
    ErrorOnRestart = 0x0020, // не удалось перезапустить
    ShutdownMasked = 0x0040, // закрытие запрещено фильтром (антивирус и т. п.)
    RestartMasked = 0x0080,  // перезапуск запрещён фильтром
};

// Есть ли бит в маске. Отдельная функция, а не сравнение, потому что маска
// приходит извне и обязана читаться без приведения типов на стороне вызова.
[[nodiscard]] constexpr bool hasStatusFlag(std::uint32_t rawStatus, AppStatusFlag flag) noexcept {
    return (rawStatus & static_cast<std::uint32_t>(flag)) != 0u;
}

// Один держатель. Всё, что здесь есть, пришло от RM или прочитано по PID
// напрямую; ничего не вычислено «на глаз».
struct ProcessInfo {
    std::uint32_t pid{};        // Process.dwProcessId
    std::uint64_t startTime{};  // Process.ProcessStartTime (FILETIME, 0 — не пришло)

    // Дружественное имя приложения из ресурсов EXE: именно его видит
    // пользователь, поэтому оно первое в списке выбора «закрыть».
    std::wstring appName;  // strAppName
    // Короткое имя службы, если процесс — служба: для служб это единственное
    // осмысленное имя, дружественного имени у них часто нет.
    std::wstring serviceName;  // strServiceShortName

    AppType type{AppType::Unknown}; // ApplicationType
    std::uint32_t appStatus{};      // AppStatus, маска AppStatusFlag
    std::int32_t tsSessionId{-1};   // TSSessionId; -1 — «неприменимо»
    bool restartable{};             // bRestartable: RM умеет его перезапустить

    // Полный путь образа, прочитанный по PID (best effort). RM его не
    // отдаёт, а имя процесса без пути не позволяет показать пользователю
    // «какое именно приложение» и не годится для правила `requiresProcessesClosed`
    // из набора правил (FR-3/FR-4), где написано имя exe. Может остаться пустым:
    // у процесса чужого пользователя или после его смерти. Флаг
    // imagePathKnown отличает «прочитали и такого пути нет» от «не смогли
    // прочитать» — это разные строки в отчёте.
    std::wstring imagePath;
    bool imagePathKnown{};

    // Это сам MrProper. Отдельный флаг, а не сравнение PID вызывающим:
    // «файл держит наша же программа» — не то же самое, что «файл свободен»,
    // и по этому случаю решение принимает движок (обычно закрыть собственные
    // дескрипторы и повторить), а не пользователь.
    bool self{};

    // PID уже занят другим процессом: время старта, прочитанное у процесса
    // сейчас, не совпало с тем, что сообщил RM. Пока флаг снят, PID нельзя
    // предлагать закрывать, а имя нельзя показывать как «держателя»: оба
    // относятся к уже мёртвому процессу. Единственное честное действие —
    // повторить проверку.
    bool pidReused{};

    // Предикаты по маске AppStatus. Именно предикаты, а не разбор маски в
    // коде интерфейса: «остановлено другим» и «работает» могут стоять
    // одновременно, и решение «можно предлагать закрыть» принимает модуль,
    // а не UI.
    [[nodiscard]] bool running() const noexcept { return hasStatusFlag(appStatus, AppStatusFlag::Running); }
    [[nodiscard]] bool stoppedByManager() const noexcept {
        return hasStatusFlag(appStatus, AppStatusFlag::Stopped);
    }
    [[nodiscard]] bool stoppedOutside() const noexcept {
        return hasStatusFlag(appStatus, AppStatusFlag::StoppedOther);
    }
    [[nodiscard]] bool restarted() const noexcept { return hasStatusFlag(appStatus, AppStatusFlag::Restarted); }
    [[nodiscard]] bool errorOnStop() const noexcept { return hasStatusFlag(appStatus, AppStatusFlag::ErrorOnStop); }
    [[nodiscard]] bool errorOnRestart() const noexcept {
        return hasStatusFlag(appStatus, AppStatusFlag::ErrorOnRestart);
    }
    [[nodiscard]] bool shutdownMasked() const noexcept {
        return hasStatusFlag(appStatus, AppStatusFlag::ShutdownMasked);
    }
    [[nodiscard]] bool restartMasked() const noexcept {
        return hasStatusFlag(appStatus, AppStatusFlag::RestartMasked);
    }

    [[nodiscard]] bool service() const noexcept { return type == AppType::Service; }
    [[nodiscard]] bool critical() const noexcept { return type == AppType::Critical; }

    // Имя для интерфейса в порядке убывания полезности: дружественное имя
    // приложения, короткое имя службы, имя файла образа, и только потом
    // «PID N» — как последний вариант, потому что номер процесса пользователю
    // ничего не говорит.
    [[nodiscard]] std::wstring displayName() const;
};

// ---------------------------------------------------------------------------
// Статусы
// ---------------------------------------------------------------------------

// Итог RmRegisterResources. Отдельное перечисление, а не общий статус:
// регистрация и опрос отвечают на разные вопросы, и «файл не найден» на
// регистрации — это не «файл занят».
enum class RegisterStatus : std::uint8_t {
    Ok,               // ресурсы зарегистрированы
    NoSession,        // сессия не открыта (или уже закрыта) — ошибка вызывающего
    InvalidArgument,  // пустой набор ресурсов
    ResourceNotFound, // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND: пути уже нет
    AccessDenied,     // ERROR_ACCESS_DENIED: RM не смог зарегистрировать путь
    SessionCritical,  // ERROR_SESSION_CRITICAL: сессия не может работать
    SessionEnded,     // ERROR_SESSION_ENDED: сессию закрыли, нужен новый запрос
    Failed,           // прочие отказы RM
};

// Итог всей проверки «кто держит». Единственное состояние, при котором
// processes можно читать как ответ; всё остальное означает «RM не ответил» и
// обязано быть видно вызывающему, а не молчать (FR-6: отказ собирается в
// отчёт, остальные операции продолжаются).
enum class QueryStatus : std::uint8_t {
    Ok,               // RmGetList ответил; processes — полный ответ (возможно пустой)
    NoSession,        // сессия не открыта
    InvalidArgument,  // пустой набор путей: проверять нечего
    ResourceNotFound, // один из путей не существует — удалять там нечего
    AccessDenied,     // ERROR_ACCESS_DENIED
    SessionEnded,     // сессию закрыли во время проверки
    AnswerTooLarge,   // RM ответил числом процессов выше kMaxProcessesFromRm
    Failed,           // прочие отказы RM
    OutOfMemory,      // не хватило памяти на ответ
};

// Стабильные имена состояний: не локализуются, идут в лог и в JSON (§8).
[[nodiscard]] const char* toString(QueryStatus status) noexcept;
[[nodiscard]] const char* toString(RegisterStatus status) noexcept;
[[nodiscard]] const wchar_t* toWideString(QueryStatus status) noexcept;

// Причина одним предложением: имя состояния плюс текст системы по коду Win32.
// Пустая строка только при Ok. Широкая форма — для UI и отчёта, узкая (UTF-8) —
// для лога; текст берётся у самой системы, поэтому соответствует языку
// установки, а не захардкожанному словарю (SPEC §5, §12).
[[nodiscard]] std::wstring formatStatusWide(QueryStatus status, std::uint32_t win32Error);
[[nodiscard]] std::string formatStatus(QueryStatus status, std::uint32_t win32Error);

// Статус регистрации в статусе проверки: одна ошибка — одно поле результата,
// а не «регистрация провалилась, а в статусе Ok».
[[nodiscard]] QueryStatus toQueryStatus(RegisterStatus status) noexcept;

// Имеет ли смысл повторить проверку (FR-6: «повтор с backoff 3×»). Повтор
// имеет смысл там, где отказ про процессы и время: сессия могла закрыться,
// файл мог освободиться. Повтор не имеет смысла на InvalidArgument и на
// ResourceNotFound — там повтор даст тот же ответ, просто позже.
[[nodiscard]] bool isRetryable(QueryStatus status) noexcept;

// ---------------------------------------------------------------------------
// Параметры проверки
// ---------------------------------------------------------------------------

struct QueryOptions {
    // Дописывать завершающий '\\' там, где путь — каталог (см. resourcePath в
    // шапке файла). Да по умолчанию: цена — один GetFileAttributesW на путь,
    // а неверный ответ стоит дороже, потому что выглядит как «файлы свободны».
    // Для одиночного файла проверки не меняет ничего.
    bool markDirectories{true};

    // Дочитывать по PID полный путь образа (QueryFullProcessImageNameW). Да по
    // умолчанию: без пути имя процесса не годится для правила
    // `requiresProcessesClosed` и для сообщения пользователю. Отдельный вызов
    // на каждый процесс; при большом числе держателей его можно выключить.
    bool resolveImagePaths{true};

    // Писать ли неудачи в лог. Да по умолчанию (SPEC §5, §12: отказ виден в
    // логе с путём и кодом), но дампы и CI, где «RM не ответил» — норма,
    // выключают это явно.
    bool logFailures{true};

    // Сколько процессов принять от RM (предохранитель, см.
    // kMaxProcessesFromRm). Ноль означает «предохранитель по умолчанию».
    std::size_t maxProcesses{kMaxProcessesFromRm};
};

// ---------------------------------------------------------------------------
// Ответ
// ---------------------------------------------------------------------------

struct LockResult {
    QueryStatus status{QueryStatus::InvalidArgument};
    std::uint32_t win32Error{};  // код Win32; 0 — не Win32 (ERROR_SUCCESS)
    std::vector<ProcessInfo> processes;

    // Флаги «нужна перезагрузка» из RmGetList: RM сообщает их, когда среди
    // держателей есть критичный процесс или процесс чужой сессии. Для
    // очистки это ответ на вопрос «можно ли обойтись без перезагрузки», и
    // он обязан быть в отчёте (FR-8), а не потерян.
    std::uint32_t rebootReasons{};

    // Сколько путей фактически зарегистрировано в сессии. Меньше, чем просили,
    // означает, что вызывающий получил не тот набор, о котором думал, и это
    // видно в логе.
    std::size_t registeredResources{};

    // RM ответил, и ответ можно читать. Только это состояние делает
    // processes ответом на вопрос «кто держит».
    [[nodiscard]] bool ok() const noexcept { return status == QueryStatus::Ok; }

    // Файл занят: RM ответил и вернул хотя бы одного держателя. Именно это
    // состояние переводит действие плана в Skip (locked) (FR-5).
    [[nodiscard]] bool locked() const noexcept { return ok() && !processes.empty(); }

    // Файл свободен: RM ответил и не нашёл никого. Отличается от locked()
    // и от любого отказа: только это состояние разрешает удалять без вопросов.
    [[nodiscard]] bool free() const noexcept { return ok() && processes.empty(); }

    [[nodiscard]] bool rebootRequired() const noexcept { return rebootReasons != 0u; }

    [[nodiscard]] const ProcessInfo* findPid(std::uint32_t pid) const noexcept;
};

// ---------------------------------------------------------------------------
// Сессия
// ---------------------------------------------------------------------------

// Владение сессией RM. Единственное место в проекте, где живёт RmEndSession:
// забыть его нельзя, потому что закрывает деструктор (см. пункт 1 в шапке
// файла). Копирования запрещены — два владельца одной сессии закрыли бы её
// дважды, второй RmEndSession вернул бы ERROR_INVALID_HANDLE в никуда;
// перемещение разрешено, потому что перемещение владения и есть его смысл.
class Session {
public:
    // Открывает сессию сразу (RmStartSession, dwSessionFlags всегда 0 —
    // параметр зарезервирован самой RM). Конструктор ничего не бросает: отказ
    // виден через started()/startError(), потому что вызывающий всё равно
    // обязан записать его в отчёт и продолжить (FR-6).
    Session() noexcept;
    ~Session() noexcept;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;

    // Сессия открыта. Единственный признак, по которому можно звать
    // registerResources/list: они на закрытой сессии вернут NoSession, а не
    // упадут.
    [[nodiscard]] bool started() const noexcept { return sessionKey_ != 0u; }

    // Ключ сессии (0 — сессии нет). Нужен ровно для диагностики: сам ключ
    // бесполезен, его нельзя ни показать, ни передать.
    [[nodiscard]] std::uint32_t sessionKey() const noexcept { return sessionKey_; }

    // Код RmStartSession, если сессия не открыта; 0 — открылась.
    [[nodiscard]] std::uint32_t startError() const noexcept { return startError_; }

    // Код последнего RmRegisterResources, если регистрация не удалась; 0 —
    // удалась. Отдельный от startError() намеренно: у сессии два независимых
    // отказа с разными последствиями, и «последняя ошибка» в одном поле
    // означала бы, что в логе нельзя сказать, что именно не сработало.
    [[nodiscard]] std::uint32_t registerError() const noexcept { return registerError_; }

    // Зарегистрировать файлы и (необязательно) короткие имена служб.
    // Возвращаемое значение — не «ошибка», а состояние: ResourceNotFound
    // означает, что путь уже исчез, и это не блокировка.
    //
    // Пути передаются как есть: нормализацию каталогов делает вызывающая
    // функция верхнего уровня (QueryOptions::markDirectories) или
    // resourcePath(). Здесь любая проверка была бы второй проверкой одного и
    // того же, а расхождение двух ответов — источником «то заблокировано, то
    // нет» между двумя вызовами.
    [[nodiscard]] RegisterStatus registerResources(const std::vector<std::wstring>& files,
                                                   const std::vector<std::wstring>& services = {}) noexcept;

    // Только файлы, без служб. services пустым передавать необязательно, но
    // короткая форма читается в местах вызова лучше.
    [[nodiscard]] RegisterStatus registerFiles(const std::vector<std::wstring>& files) noexcept;

    // Кто держит зарегистрированное. out очищается всегда, даже при отказе:
    // иначе в отчёте остался бы список от прошлого успешного вызова, который
    // выглядел бы как ответ на текущий вопрос.
    [[nodiscard]] QueryStatus list(LockResult& out, const QueryOptions& options = {}) noexcept;

    // То же самое возвратом — короткая форма для одной проверки.
    [[nodiscard]] LockResult list(const QueryOptions& options = {});

    // Закрыть сессию явно, не дожидаясь деструктора. Повторный вызов и вызов
    // на неоткрытой сессии — no-op.
    void end() noexcept;

private:
    std::uint32_t sessionKey_{};
    std::uint32_t startError_{};
    std::uint32_t registerError_{};
    std::size_t registeredResources_{};
};

// ---------------------------------------------------------------------------
// Проверка одним вызовом
// ---------------------------------------------------------------------------

// «Кто держит эти N путей»: своя сессия на одну проверку, регистрация, опрос,
// закрытие. Именно эта форма нужна движку: ресурсы в сессии заменяются при
// каждой регистрации (пункт 2 в шапке), поэтому один набор путей на одну
// сессию — единственный способ, при котором нельзя проверить не то.
//
// Ничего не бросает: отказ в status/win32Error. Пустой набор путей даёт
// InvalidArgument, а не «файлы свободны» — разница принципиальная, потому что
// «свободны» разрешают удалять (FR-5).
[[nodiscard]] LockResult whoLocksFiles(const std::vector<std::wstring>& files, const QueryOptions& options = {});

// Одиночный путь — та же проверка. Каталоги нормализуются по
// QueryOptions::markDirectories.
[[nodiscard]] LockResult whoLocksFile(std::wstring_view path, const QueryOptions& options = {});

// То же для путей в UTF-8: модель кандидата (§6.3) хранит пути в UTF-8, и
// ядро обязано собираться без Windows. Неудачный перевод даёт
// InvalidArgument, а не пустой список: молча выкинуть нечитаемый путь —
// значит сообщить «файл свободен».
[[nodiscard]] LockResult whoLocksFilesUtf8(const std::vector<std::string>& files, const QueryOptions& options = {});

// ---------------------------------------------------------------------------
// Вспомогательное
// ---------------------------------------------------------------------------

// Путь ресурса для RmRegisterResources: каталог получает завершающий '\\',
// файл остаётся как есть. Несуществующий путь возвращается без изменений —
// RmRegisterResources честно ответит ERROR_FILE_NOT_FOUND, и это правильный
// ответ, а не догадка модуля.
[[nodiscard]] std::wstring resourcePath(std::wstring_view path);

// Прямая проекция ответа в модель: `CleanupCandidate::lockedBy`
// (`std::vector<core::ProcessRef>`, §6.3). Имена берутся из displayName() в
// UTF-8, как их ждёт ядро. Процессы с pidReused в список не попадают: поле
// модели читает интерфейс и отчёт, а показывать там имя уже мёртвого процесса
// нельзя.
[[nodiscard]] std::vector<core::ProcessRef> toProcessRefs(const LockResult& result);

// Расшифровка флагов rebootReasons: «нужна перезагрузка (критичный процесс)».
// Пустая строка при нулевых флагах.
[[nodiscard]] std::string describeRebootReasons(std::uint32_t rebootReasons);

// Однострочные описания для лога, текстового дампа (FR-8) и сообщения
// пользователю. Не локализация: строки русские, как и остальные сообщения
// слоя (см. devices::Issue::message), а UI переводит свои подписи отдельно
// через core::i18n.
[[nodiscard]] std::string describe(const ProcessInfo& process);
[[nodiscard]] std::string describe(const LockResult& result);

}  // namespace mrproper::platform::restart_manager
