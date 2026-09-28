// MrProper — WMI: COM, BitLocker (Win32_EncryptVolume) и обогащение
// инвентаризации классами хранилища. Спека: §4 FR-1 п.7 («Шифрование: WMI
// `Win32_EncryptVolume.EncryptState`; fallback — `manage-bde -status`.
// `FltGetEncryptionVolumeInfo` отклонён (требует minifilter-драйвера)»),
// §4 FR-1 п.8 («Дополнительно (WMI, только при необходимости): MSFT_PhysicalDisk,
// MSFT_Volume, `Win32_DiskDrive`, `MSFT_StorageFault` — обогащение и проверка
// согласованности с IOCTL-картиной»), §6.2 (platform::wmi — «COM/IWbemServices:
// WMI-подробности, BitLocker, storage fault»), §9.1 ADR-001 (изоляция WinAPI в
// platform/, RAII-обёртки вместо ручных Release), ADR-004 (в ядро WinAPI не
// течёт), §5 (устойчивость: ни один отказ IOCTL/устройства не роняет процесс),
// §10 (риск «источник данных недоступен» → деградирование и понятное сообщение,
// а не падение), §12 (все ошибки в логе с путём и HRESULT).
//
// ---------------------------------------------------------------------------
// Что модуль делает
// ---------------------------------------------------------------------------
//   1) Поднимает COM в рабочем потоке инвентаризации (MTA, см. ComApartment) и
//      один раз на процесс вызывает CoInitializeSecurity — без него
//      ConnectServer к локальному пространству отказывает (0x8001010A,
//      RPC_E_INVALID_AUTHN), и это самая частая причина «WMI не работает» в
//      приложениях, которые про COM забыли.
//   2) Отвечает на вопрос FR-1 п.7 «зашифрован ли том» через
//      `Win32_EncryptVolume`, с честным трёхзначным результатом и, если WMI не
//      ответил, через `manage-bde -status` (тот же fallback, что назван в
//      спеке).
//   3) Отвечает на вопрос FR-1 п.8 «что WMI знает сверх IOCTL» по четырём
//      классам: MSFT_PhysicalDisk, MSFT_Volume, Win32_DiskDrive,
//      MSFT_StorageFault.
//   4) Ничего не склеивает с IOCTL-картиной: это делает инвентаризация
//      (platform::inventory) и модель (core::disk_model), здесь только сырые
//      данные плюс две мелкие функции-сверки.
//
// ---------------------------------------------------------------------------
// Чего модуль сознательно НЕ делает (границы)
// ---------------------------------------------------------------------------
//   * не пишет в core::Volume / core::PhysicalDisk. Мост «прочитал WMI →
//     записал в модель» принадлежит platform::inventory: он единственный знает
//     про device path диска, номер и точки монтирования. Граница объявлена там
//     же («не спрашивает WMI (FR-1 п.7, BitLocker) — platform::wmi»);
//   * не спрашивает про SMART (FR-1 п.8 помечает SMART как v1.1) и не
//     разбирает реестр Device Parameters — только опубликованные классы WMI;
//   * не вызывает manage-bde, если WMI ответил: лишний процесс на каждый обход
//     стоит дороже, чем сам ответ (1–3 с против 0,1 с), а спека называет
//     manage-bdе именно fallback;
//   * не трогает состояние BitLocker. Ни Protect, ни Unlock, ни Manage-BDE с
//     ключами: модуль только читает. Снятие защиты — решение пользователя, и
//     делать его из утилиты очистки было бы неверно по существу;
//   * не работает с удалёнными хостами: только локальные пространства имён
//     ROOT\CIMV2 и ROOT\Microsoft\Windows\Storage. Удалённый WMI потребовал бы
//     аутентификации и не имеет смысла для утилиты, которая смотрит свой
//     диск.
//
// ---------------------------------------------------------------------------
// Устойчивость (FR-1: «ни один отказ не роняет процесс»)
// ---------------------------------------------------------------------------
// Ни одна публичная функция не бросает наружу исключений и не падает: отказ
// виден в QueryError, в счётчиках структур результата и в логе с HRESULT
// (§12). Пустой вектор означает «данных нет», а не «устройств нет», и вызывающий
// обязан трактовать его именно так. Каждому WMI-запросу задан предельный
// срок (kQueryTimeoutMs), а класс, которого нет в системе, опрашивается один
// раз за сессию процесса: повторная попытка стоила бы те же секунды на каждом
// обходе, а класс за время работы приложения не появляется.
//
// ---------------------------------------------------------------------------
// Потоки
// ---------------------------------------------------------------------------
// Экземпляр Session — на один поток. IWbemLocator и IWbemServices не
// потокобезопасны, а IWbemServices вообще привязан к апартменту, в котором
// выполнен ConnectServer. Session сам поднимает COM в своей нити, поэтому
// вызывать её можно из любого рабочего потока инвентаризации (SPEC §6.4), но
// передавать сессию в другой поток нельзя. Один Session на один обход дисков —
// ConnectServer стоит сотни миллисекунд, а запросов на обход четыре.
//
// ---------------------------------------------------------------------------
// Форма результата
// ---------------------------------------------------------------------------
// Наружу выходят перечислители и целые числа, ни одного типа Win32 и COM. С
// учётом SPEC §6.3 (ядро — UTF-8) все строки здесь в UTF-8, а имена классов и
// свойств — литералы внутри .cpp.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mrproper::platform::wmi {

// HRESULT в Windows SDK — это long, но имя типа живёт в <windows.h>, а этот
// заголовок его намеренно не включает: он должен читаться и собираться там, где
// Windows нет (тот же приём, что в bus_type.hpp). На Windows 64 бита под
//тверждением остаётся 32-битная ширина HRESULT.
using Hresult = long;

// Заглушка «отказ» в структурах, которые заполняются по ходу работы. Записана
// числом, а не именем E_FAIL: имя живёт в winerror.h, а заголовок остаётся без
// Windows. Значение — то же, что у E_FAIL (0x80004005).
inline constexpr Hresult kFailedHresult = static_cast<Hresult>(0x80004005L);

// ---------------------------------------------------------------------------
// Пределы и таймауты
// ---------------------------------------------------------------------------

// Предельный срок ожидания данных одного WMI-запроса, мс. Задаётся в
// IEnumWbemClassObject::Next — там он и действует: ExecQuery в Windows 10 SDK
// (WbemCli.h) параметра таймаута не имеет вовсе и с
// WBEM_FLAG_RETURN_IMMEDIATELY отдаёт перечислитель сразу, а данные приходят
// пакетами уже в Next. Три секунды — с запасом относительно двухсекундного
// таймаута на устройство из FR-1: WMI отвечает медленнее IOCTL (провайдер
// хранилища перечисляет PnP), но медленнее него инвентаризация висеть не
// должна — не ответил класс, значит обходимся без него. Случай, когда провайдер
// не отдаёт сам перечислитель, ограничен таймаутом рабочего потока
// инвентаризации.
inline constexpr std::uint32_t kQueryTimeoutMs = 3000;

// Сколько объектов забираем за один Next. IWbemCallResult отдаёт массив
// указателей, поэтому размер выбирается один и переиспользуется: на обход
// томов это единицы выделений вместо сотен.
inline constexpr std::size_t kRowBatch = 64;

// Потолок строк одного запроса. Запрос, который вернул бы миллион строк, — это
// либо сбойный провайдер, либо класс, отвечающий не на тот запрос; в обоих
// случаях правильнее остановиться, чем съесть память отчёта (FR-8).
inline constexpr std::size_t kMaxRows = 4096;

// Предельный срок запуска `manage-bde -status`, мс. Процесс поднимается без
// окна и с предельным ожиданием; не уложился — снимается и попадает в отчёт
// как «источник не ответил».
inline constexpr std::uint32_t kManageBdeTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// COM
// ---------------------------------------------------------------------------

// Результат инициализации COM в текущем потоке.
enum class ComInitResult : std::uint32_t {
    Ready,          // CoInitializeEx вернул S_OK: квартира наша, её надо закрыть
    AlreadyReady,   // S_FALSE: поток уже в MTA, CoUninitialize не нужен
    AlreadyThread,  // RPC_E_CHANGED_MODE: поток уже в STA (есть UI) — WMI работает
    Failed,         // отказ: COM в этом потоке недоступен
};

// Короткое имя для лога (латиницей — значения не локализуются).
[[nodiscard]] const char* comInitResultName(ComInitResult result) noexcept;

// RAII-обёртка CoInitializeEx/CoUninitialize. CoUninitialize вызывается ровно
// тогда, когда CoInitializeEx вернул S_OK: на S_FALSE и RPC_E_CHANGED_MODE
// вызов счётчика не увеличивает, а лишний CoUninitialize на чужую инициализацию
// снёс бы чужое состояние (в UI-потоке это уронило бы Direct2D).
//
// Квартира — MTA (COINIT_MULTITHREADED), потому что WMI вызывается из рабочих
// потоков инвентаризации (SPEC §6.4), а STA требует насоса сообщений.
// Объект неперемещаем и не копируется намеренно: он обязан жить в том же
// потоке, в котором инициализировал COM.
class ComApartment {
public:
    ComApartment() noexcept;
    ~ComApartment() noexcept;

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&) = delete;
    ComApartment& operator=(ComApartment&&) = delete;

    // Можно ли работать с COM в этом потоке. false у Failed.
    [[nodiscard]] bool ready() const noexcept;

    [[nodiscard]] ComInitResult result() const noexcept;

    // HRESULT, который вернул CoInitializeEx: он попадает в лог, потому что
    // «WMI не работает» без кода — это три разных отказа (нет прав, поток в
    // STA, нет памяти).
    [[nodiscard]] Hresult hr() const noexcept;

private:
    ComInitResult result_{ComInitResult::Failed};
    Hresult hr_{kFailedHresult};
    bool ownsApartment_{false};
};

// CoInitializeSecurity для WMI: один раз на процесс, из любого потока.
// Win32, сработает на первой же попытке, потому что именно на этом коде
// останавливается ConnectServer без CoInitializeSecurity. Вызов делает сессия
// сама, руками звать не нужно.
[[nodiscard]] Hresult ensureComSecurity() noexcept;

// ---------------------------------------------------------------------------
// Диагностика
// ---------------------------------------------------------------------------

// Имя WMI/HRESULT-кода для лога: «WBEM_E_INVALID_CLASS», «HRESULT_FROM_WIN32
// (2)» или «HRESULT 0x80041005». Латиница, без локализации — по нему ищут в
// баг-репортах.
[[nodiscard]] std::string hresultName(Hresult hr);

// Отказ одного шага запроса. Ни одно поле не является обязательным: у
// неподключившейся сессии operation == "ConnectServer", у неразобранного
// перечисления — wql, потому что сбой случился уже после ExecQuery.
struct QueryError {
    Hresult hr{kFailedHresult};  // настоящий код приходит из connectNamespace/query
    std::string operation;  // "CoCreateInstance", "ConnectServer", "ExecQuery", "Next", "Get"
    std::string wql;        // запрос, если он уже был отправлен

    // Однострочная запись для журнала: имя кода, текст системы и сам запрос.
    [[nodiscard]] std::string toString() const;
};

// Нет ли такого класса в этом пространстве имён (WBEM_E_INVALID_CLASS) или нет
// самого пространства (WBEM_E_INVALID_NAMESPACE). Оба ответа означают одно:
// «здесь этого класса не бывает», и повторять запрос бессмысленно.
[[nodiscard]] bool isClassMissing(Hresult hr) noexcept;

// ---------------------------------------------------------------------------
// Шифрование тома (FR-1 п.7)
// ---------------------------------------------------------------------------

// Состояние BitLocker по EncryptState. Unknown — «модуль не знает», и это
// принципиально отличается от NotEncrypted: молча превращать неизвестность в
// «том не зашифрован» нельзя, потому что по этому ползу пользователь решит,
// можно ли трогать файлы на томе.
//
// Известные значения EncryptState (документация Win32_EncryptVolume):
//   1 — зашифрован, 2 — не зашифрован, 3 — идёт шифрование,
//   4 — идёт расшифровка, 5 — шифрование прервано, 6 — расшифровка прервана.
//   Значения 5 и 6 и любые будущие отнесены в Unknown: что именно прервано и
//   в каком состоянии том после этого, модуль не угадывает. Сырое число
//   остаётся в VolumeEncryption::encryptStateCode.
enum class EncryptionState : std::uint32_t {
    Unknown = 0,
    NotEncrypted,
    Encrypted,
    Encrypting,
    Decrypting,
};

[[nodiscard]] const char* encryptionStateName(EncryptionState state) noexcept;

// Том зашифрован? Строго только Encrypted: Encrypting и Unknown дают false,
// но вызывающий обязан смотреть на state, а не на этот флаг.
[[nodiscard]] bool isEncrypted(EncryptionState state) noexcept;

// Откуда взяты данные о шифровании. Поле нужно отчёту (FR-8) и карточке тома
// (FR-2): «BitLocker включён (WMI)» и «BitLocker включён (manage-bde)» —
// разные утверждения с разной точностью, и выдавать одно за другое нельзя.
enum class EncryptionSource : std::uint32_t {
    Unknown = 0,
    Wmi,
    ManageBde,
};

[[nodiscard]] const char* encryptionSourceName(EncryptionSource source) noexcept;

// Защита ключа (ProtectionStatus): 1 — включена, 2 — выключена. Всё прочее —
// Unknown: «выключена» и «не смогли прочитать» — разные вещи, и подменять одну
// другой нельзя.
enum class ProtectionState : std::uint32_t { Unknown = 0, Enabled, Disabled };

// Состояние тома (LockStatus): 0 — разблокирован, 1 — заблокирован.
enum class VolumeLockState : std::uint32_t { Unknown = 0, Unlocked, Locked };

// Ход преобразования (ConversionStatus): 1 — идёт, 2 — полностью зашифрован,
// 3 — полностью расшифрован. Всё прочее — Unknown.
enum class ConversionState : std::uint32_t { Unknown = 0, InProgress, Complete };

// Шифрование одного тома. Заполняется настолько, насколько ответил источник:
// пустой driveLetter при source != Unknown означает «том без буквы, найден по
// пути» и для Win32_EncryptVolume невозможно (класс ключуется буквой).
struct VolumeEncryption {
    std::string driveLetter;  // «C:» — нормализованный вид, как отдаёт WMI
    EncryptionState state{EncryptionState::Unknown};
    EncryptionSource source{EncryptionSource::Unknown};
    ProtectionState protection{ProtectionState::Unknown};
    VolumeLockState lockState{VolumeLockState::Unknown};
    ConversionState conversion{ConversionState::Unknown};
    std::string encryptionMethod;  // «XTS-AES 128»; пусто у незашифрованного

    // Сырые коды классов. Нужны, когда значение не распознано: по ним видно,
    // что прислала система, и не нужно гадать по документации новой сборки.
    std::uint32_t encryptStateCode{0};
    std::uint32_t conversionStatusCode{0};
    std::uint32_t protectionStatusCode{0};
    std::uint32_t lockStatusCode{0};

    // Откуда HRESULT: у WMI — код последнего вызова, у manage-bde — код запуска
    // процесса (0 — процесс отработал, его вывод разбирается в detail).
    Hresult hr{kFailedHresult};
    std::uint32_t exitCode{0};

    // Почему state == Unknown (для лога и карточки): «WMI не ответил:
    // WBEM_E_INVALID_CLASS», «manage-bde: том не найден в выводе» и т. п.
    std::string detail;

    // Данные получены (любой источник).
    [[nodiscard]] bool known() const noexcept {
        return state != EncryptionState::Unknown;
    }
};

// ---------------------------------------------------------------------------
// Обогащение инвентаризации (FR-1 п.8)
// ---------------------------------------------------------------------------

// MSFT_PhysicalDisk (пространство ROOT\Microsoft\Windows\Storage). Модель,
// серийник, прошивка, размер, тип носителя и шины, здоровье — то, чего в
// IOCTL-картине либо нет, либо есть в другом виде.
struct PhysicalDiskInfo {
    int number{-1};  // из DeviceId («\\.\PHYSICALDRIVE2» → 2), -1 если не разобрано
    std::string deviceId;  // «\\.\PHYSICALDRIVE2»
    std::string friendlyName;
    std::string model;
    std::string manufacturer;
    std::string serialNumber;
    std::string firmwareVersion;
    std::string partNumber;
    std::uint64_t sizeBytes{0};
    std::uint64_t allocatedBytes{0};
    std::uint32_t busType{0};    // нумерация совпадает с STORAGE_BUS_TYPE (FR-1 п.2)
    std::uint32_t mediaType{0};  // 0 — не указан, 3 — HDD, 4 — SSD
    std::uint16_t healthStatus{0};
    // OperationalStatus приходит массивом; модуль хранит побитовое ИЛИ всех
    // значений (см. hasOperationalStatus), потому что «Lost Communication» и
    // «OK» должны сосуществовать в одной маске.
    std::uint32_t operationalStatus{0};
    bool offline{false};
    bool readOnly{false};
    bool boot{false};
    bool system{false};

    [[nodiscard]] bool known() const noexcept {
        return number >= 0 || !deviceId.empty();
    }
};

// Имя типа носителя по MediaType MSFT_PhysicalDisk. nullptr у значения, для
// которого в схеме нет имени, — вызывающий сам подставит «неизвестно».
[[nodiscard]] const char* mediaTypeName(std::uint32_t mediaType) noexcept;

// Здоровье по HealthStatus классов хранилища: 0 — Healthy, 1 — Warning,
// 2 — Unhealthy (значения MSFT_Disk/MSFT_Volume, msft_*.mof в Windows SDK).
[[nodiscard]] const char* healthStatusName(std::uint16_t healthStatus) noexcept;

// Есть ли в побитовой маске OperationalStatus конкретное значение. Сами
// значения: 2 — OK, 3 — Degraded, 5 — Predictive Failure, 6 — Error,
// 7 — Non-Recoverable Error, 0xD00D — Scan Needed, 0xD00E — Spot Fix Needed,
// 0xD00F — Full Repair Needed.
[[nodiscard]] bool hasOperationalStatus(std::uint32_t mask, std::uint32_t value) noexcept;

// Самое важное из значений в маске OperationalStatus, по убыванию тяжести:
// Non-Recoverable Error → Error → Predictive Failure → Full Repair Needed →
// Spot Fix Needed → Degraded → Scan Needed → OK. nullptr — в маске нет ни
// одного известного статуса, то есть данных недостаточно, чтобы что-то
// утверждать (FR-2 «здоровье», FR-8 отчёт).
[[nodiscard]] const char* operationalStatusDescription(std::uint32_t mask) noexcept;

// MSFT_Volume: путь, файловая система, размер, здоровье. Нужен для проверки
// согласованности с IOCTL-картиной — например, когда том виден в одном
// источнике и не виден в другом.
struct VolumeInfo {
    std::string driveLetter;     // «C:»; пусто у тома без буквы
    std::string path;            // «C:\» или папка монтирования
    std::string volumeGuidPath;  // «\\?\Volume{…}» — мост к core::Volume
    std::string fileSystem;
    std::string fileSystemLabel;
    std::uint64_t sizeBytes{0};
    std::uint64_t sizeRemainingBytes{0};
    std::uint32_t driveType{0};        // 3 — Fixed, 2 — Removable, 4 — Remote …
    std::uint32_t fileSystemType{0};   // 14 — NTFS, 15 — ReFS, 6 — FAT32 …
    std::uint16_t healthStatus{0};
    std::uint32_t operationalStatus{0};

    [[nodiscard]] bool known() const noexcept {
        return !driveLetter.empty() || !volumeGuidPath.empty() || !path.empty();
    }
};

// Имя типа тома по DriveType MSFT_Volume.
[[nodiscard]] const char* driveTypeName(std::uint32_t driveType) noexcept;

// Имя файловой системы по FileSystemType MSFT_Volume. У тома без файловой
// системы значение 0 (Unknown) — это «нет ФС», а не «неизвестно».
[[nodiscard]] const char* fileSystemTypeName(std::uint32_t fileSystemType) noexcept;

// Win32_DiskDrive: дубль картины IOCTL из другого источника. Нужен не для
// показа, а для сверки: если WMI и IOCTL разошлись по размеру или серийнику,
// это повод написать в баг-репорт, а не молча показать одно из двух.
struct DiskDriveInfo {
    int index{-1};  // Win32_DiskDrive.Index
    std::string deviceId;      // «\\.\PHYSICALDRIVE0»
    std::string pnpDeviceId;   // PNPDeviceID
    std::string model;
    std::string serialNumber;
    std::string firmwareRevision;
    std::string interfaceType;  // «SCSI», «USB», «NVMe»… как отдаёт класс
    std::string mediaType;      // «Fixed hard disk media», как отдаёт класс
    std::string status;         // «OK» либо текст проблемы
    std::uint64_t sizeBytes{0};
    std::uint32_t partitions{0};

    [[nodiscard]] bool known() const noexcept {
        return index >= 0 || !deviceId.empty();
    }
};

// MSFT_StorageFault: события отдачи и деградации носителя (RAID, пулы, NVMe в
// Storage Spaces). Источник, которого нет у IOCTL-картины вообще, поэтому
// StorageFault — главная причина держать WMI-подключение, а не только
// BitLocker-запрос.
//
// Схема MSFT_StorageFault отличается между сборками Windows, поэтому поля
// заполняются «первым найденным именем» из короткого списка известных
// вариантов, а пустое поле означает «такого свойства здесь нет» — не отказ.
struct StorageFaultInfo {
    std::string instanceId;   // ключ экземпляра класса
    std::string deviceId;     // «\\.\PHYSICALDRIVE0»
    std::string faultType;    // строка вендора, не локализуется
    std::string description;  // текст причины
    std::uint32_t severity{0};
    std::string occurrenceDate;  // строка DMTF, как её отдал WMI

    [[nodiscard]] bool known() const noexcept {
        return !instanceId.empty() || !deviceId.empty() || !description.empty();
    }
};

// ---------------------------------------------------------------------------
// Сессия WMI
// ---------------------------------------------------------------------------

// Одно подключение к WMI на один обход инвентаризации. Экземпляр не
// потокобезопасен и привязан к потоку, в котором ConnectServer отработал; сам
// он поднимает COM в этой нити (ComApartment внутри), поэтому вызывающему не
// нужно ничего готовить.
//
// Подключение ленивое и частичное: root\cimv2 поднимается первым же запросом,
// ROOT\Microsoft\Windows\Storage — первым же обращением к MSFT_классам. Разделение
// нужно потому, что подключение к пространству хранилища медленнее, а на
// машине без Storage Management оно вовсе может быть недоступно — и запрос
// BitLocker не должен зависеть от него.
//
// Ни один метод не бросает исключений. Отказ виден в lastError() и в журнале.
class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;

    // Подключиться к root\cimv2 (идемпотентно: повторный вызов ничего не
    // делает). true — можно выполнять запросы.
    [[nodiscard]] bool connect() noexcept;

    [[nodiscard]] bool connected() const noexcept;

    // Последний отказ сессии. Сбрасывается при каждом успешном шаге запроса,
    // поэтому после удачного volumeEncryption() он описывает что-то другое.
    [[nodiscard]] const QueryError& lastError() const noexcept;

    // FR-1 п.7. Один запрос на все тома: Win32_EncryptVolume отдаёт по строке на
    // том, и поштучный запрос на каждый том был бы в разы дороже.
    [[nodiscard]] std::vector<VolumeEncryption> volumeEncryption() noexcept;

    // FR-1 п.8.
    [[nodiscard]] std::vector<PhysicalDiskInfo> physicalDisks() noexcept;
    [[nodiscard]] std::vector<VolumeInfo> volumes() noexcept;
    [[nodiscard]] std::vector<StorageFaultInfo> storageFaults() noexcept;
    [[nodiscard]] std::vector<DiskDriveInfo> diskDrives() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Разовые запросы
// ---------------------------------------------------------------------------
//
// Функции создают сессию на вызов. Для одного обхода дисков это плохо (одно
// ConnectServer на функцию), поэтому инвентаризация должна держать свой Session
// и вызывать его методы; разовые функции — для редких мест вроде карточки
// тома и отчёта (FR-8), где лишнее подключение незаметно.

// Шифрование всех томов одним запросом.
[[nodiscard]] std::vector<VolumeEncryption> queryVolumeEncryption() noexcept;

// Шифрование одного тома по букве («C», «C:», «C:\» — все три формы
// понимаются). Пустая буква даёт результат с driveLetter == "" и
// EncryptionState::Unknown: том не опрошен, а не «не зашифрован».
//
// Порядок источников: WMI, затем — если по этой букве нет данных —
// manage-bde (fallback из FR-1 п.7). Итоговый VolumeEncryption::source говорит,
// что именно ответило.
[[nodiscard]] VolumeEncryption queryVolumeEncryption(std::string_view driveLetterUtf8) noexcept;

// Только WMI, без fallback: нужно, когда вызывающий хочет знать, отвечает ли
// WMI вообще, не подменяя это процессом.
[[nodiscard]] VolumeEncryption queryVolumeEncryptionFromWmi(std::string_view driveLetterUtf8) noexcept;

// Fallback FR-1 п.7 — `manage-bde -status` на один том. Разбор вывода: том
// ищется по строке-корню («C:\»), состояние — по проценту шифрования. Процент
// с числом не локализуется, поэтому разбор переживает русскую установку;
// неудачный разбор даёт Unknown с пояснением в detail, а не «не зашифрован».
[[nodiscard]] VolumeEncryption queryVolumeEncryptionFromManageBde(std::string_view driveLetterUtf8) noexcept;

// FR-1 п.8, разовые формы.
[[nodiscard]] std::vector<PhysicalDiskInfo> queryPhysicalDisks() noexcept;
[[nodiscard]] std::vector<VolumeInfo> queryVolumes() noexcept;
[[nodiscard]] std::vector<StorageFaultInfo> queryStorageFaults() noexcept;
[[nodiscard]] std::vector<DiskDriveInfo> queryDiskDrives() noexcept;

// ---------------------------------------------------------------------------
// Мелкие помощники
// ---------------------------------------------------------------------------

// Привести путь или букву к виду Win32_EncryptVolume: «c», «C:», «C:\»,
// «C:\Folder» → «C:». Точка монтирования в папке и том без буквы дают пустую
// строку — у них в Win32_EncryptVolume нет ключа.
[[nodiscard]] std::string normalizeDriveLetter(std::string_view utf8) noexcept;

// Номер физического диска из пути устройства: «\\.\PHYSICALDRIVE3» → 3.
// Число вида «\\.\PHYSICALDRIVE3\» тоже понимается, любой другой путь даёт -1.
// Функция нужна, чтобы склеить MSFT_PhysicalDisk (у него только DeviceId) с
// core::PhysicalDisk (у него номер).
[[nodiscard]] int physicalDriveNumber(std::string_view deviceIdUtf8) noexcept;

// Найти состояние шифрования по букве в уже собранной выборке. nullptr —
// тома в выборке нет (или выборка пуста, потому что WMI не ответил).
[[nodiscard]] const VolumeEncryption* findEncryption(const std::vector<VolumeEncryption>& states,
                                                     std::string_view driveLetterUtf8) noexcept;

// Мелкая сверка WMI и IOCTL-картины по размеру диска. Пустая строка —
// расхождения нет, непустая — чем именно расходятся данные (для лога и
// отчёта). Порог: 1 МиБ или 0,5 % от большего значения, что отсекает шум
// округления и разного состояния диска в момент выборки, но не прячет
// настоящее расхождение на десятки гигабайт.
[[nodiscard]] std::string comparePhysicalDiskSize(const PhysicalDiskInfo& wmi, std::uint64_t ioctlSizeBytes) noexcept;

}  // namespace mrproper::platform::wmi
