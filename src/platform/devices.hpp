// Перечисление физических дисков: SetupAPI GUID_DEVINTERFACE_DISK плюс
// IOCTL_STORAGE_GET_DEVICE_NUMBER (SPEC §4 FR-1 п.1).
//
// Слой Win32 (SPEC §6.1, ADR-004): единственное место проекта, где допустимы
// windows.h, SetupAPI и IOCTL. Ядро (core::model) о таких вещах не знает, и
// поэтому единица трансляции с WinAPI физически не может попасть в
// mrproper_core: граница проходит по границе каталогов.
//
// Границы модуля (что он сознательно НЕ делает):
//   * не собирает инвентарь — core::PhysicalDisk, разделы, тома и агрегаты
//     собирает модуль инвентаризации (FR-1 целиком), этот файл даёт ему сырые
//     данные: device path и номер диска;
//   * не спрашивает размер (FR-1 п.3, IOCTL_DISK_GET_LENGTH_INFO), свойства
//     хранилища (FR-1 п.2, IOCTL_STORAGE_QUERY_PROPERTY) и разметку (FR-1 п.4)
//     — это соседние модули. Здесь нужен только номер: без него нельзя открыть
//     \\.\PhysicalDriveN, а без открытия нельзя задать ни один следующий IOCTL;
//   * не классифицирует устройство (CD-ROM, RAM-диск, removable) по типу из
//     ответа IOCTL: значение DeviceType трактуется драйвером как STORAGE_DEVICE_TYPE,
//     а заголовок в установленном SDK описывает это поле как DEVICE_TYPE, то есть
//     как FILE_DEVICE_*. Признаки «съёмный» и «шина» приходят из FR-1 п.2
//     (StorageDeviceProperty.MediaRemovable, StorageAdapterProperty.BusType).
//
// Файл не включает windows.h — намеренно. Объявления используют только
// переносимые типы, поэтому включить этот заголовок может и слой без Win32
// (тесты, отчёт, CLI), а слой Win32, где windows.h нужен по существу,
// добавляет его сам. Так граница слоёв не расплывается по одному заголовку.
//
// Реализация (devices.cpp) опирается на общие для слоя win_handle.hpp (RAII
// для дескрипторов, ADR-001) и win_error.hpp (код и текст ошибки): своих копий
// этих механизмов слой не заводит.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mrproper::platform::devices {

// Таймаут на одно устройство (SPEC §4 FR-1: «устойчивость к "отказавшим"
// дискам (таймаут 2 с на устройство, устройство помечается недоступным,
// приложение не падает)»). Ждём именно завершения IOCTL, а не открытия
// устройства: открытие CreateFileW синхронно, и ограничить его изнутри
// модуля нельзя (см. комментарий у openForQuery в devices.cpp).
inline constexpr std::uint32_t kDeviceTimeoutMs = 2000;

// Сколько ждать после CancelIoEx, прежде чем признать отмену неудачной.
// Не ноль: без этого ожидания буфер ответа мог бы освободиться под драйвером.
inline constexpr std::uint32_t kCancelGraceMs = 1000;

// Ответ на IOCTL_STORAGE_GET_DEVICE_NUMBER — ровно те три поля, которые
// приходят от драйвера, и ничего больше.
//
// Про собственную структуру вместо STORAGE_DEVICE_NUMBER из <winioctl.h>:
// в установленном здесь SDK (10.0.19041.0) структура описана как
// { DEVICE_TYPE DeviceType; DWORD DeviceNumber; DWORD PartitionNumber; } —
// это вариант WDK, тогда как драйвер disk.sys заполняет третье поле как
// STORAGE_DEVICE_CHARACTERISTICS (RemovableMedia/ReadOnlyMedia/WriteCacheEnabled).
// Оба варианта по 12 байт, поэтому размер и раскладка совпадают, а вот имя
// третьего поля — нет: сослаться на него как на Characteristics нельзя
// (не компилируется), а объявить свой тип молча значит спрятать это
// расхождение. Поэтому третье поле названо нейтрально, а разбор его смысла —
// задача FR-1 п.2, где он читается вместе с остальными свойствами.
struct RawDeviceNumber {
    std::uint32_t deviceType{};   // первое поле ответа, трактуется драйвером как STORAGE_DEVICE_TYPE
    std::uint32_t deviceNumber{}; // второе поле: номер физического диска, 0,1,2…
    std::uint32_t flags{};        // третье поле: у драйвера Characteristics, в SDK описано как PartitionNumber
};
static_assert(sizeof(RawDeviceNumber) == 12, "IOCTL_STORAGE_GET_DEVICE_NUMBER возвращает три DWORD");

// Этап обхода, на котором что-то не сработало. Нужен не для красоты: номер этапа
// в логе отличает «диск не ответил» (FR-1: устройство помечается недоступным)
// от «SetupAPI не отдал путь устройства» (конфигурация системы, а не диск).
enum class Stage {
    ClassDevs,      // SetupDiGetClassDevsW — набор устройств не получен
    EnumInterfaces, // SetupDiEnumDeviceInterfaces — обход прерван или упёрся в предел
    InterfaceDetail,// SetupDiGetDeviceInterfaceDetailW — путь устройства не получен
    OpenDevice,     // CreateFileW не открыл устройство ни с одним набором прав
    DeviceNumber,   // IOCTL_STORAGE_GET_DEVICE_NUMBER не вернул номер
    InstanceId      // SetupDiGetDeviceInstanceIdW — диагностический идентификатор
};

const char* stageName(Stage stage) noexcept;

// Замечание обхода. Не фатально: FR-1 требует, чтобы ни один отказ не ронял
// приложение, поэтому неудача — это запись в списке, а не исключение.
// error == 0 означает «замечание без системного кода» (например, предел обхода
// или два интерфейса на один номер диска); иначе это GetLastError().
struct Issue {
    Stage stage{Stage::ClassDevs};
    std::string devicePath;  // пусто, если устройство ещё не известно
    std::uint32_t error{};
    std::string message;  // по-русски, одно предложение
};

// Один интерфейс диска: что удалось узнать на FR-1 п.1.
struct DiskInterface {
    int number{-1};                 // номер физического диска; -1, если номер не получен
    std::uint32_t deviceType{};     // первое поле ответа IOCTL, «как есть»
    std::uint32_t flags{};          // третье поле ответа IOCTL, «как есть»
    std::string devicePath;         // UTF-8, "\\?\X#&…" с ОДНИМ завершающим '\'
    std::string instanceId;         // SetupDiGetDeviceInstanceIdW, UTF-8; пусто, если не получили
    bool numberKnown{false};        // номер получен: с этого диска можно открыть \\.\PhysicalDriveN
    bool accessible{false};         // устройство открылось и ответило на IOCTL (таймаут не сработал)
};

struct EnumerateOptions {
    // DIGCF_PRESENT: только присутствующие устройства. false полезно для
    // диагностики (увидеть исчезнувшие устройства), но в инвентарь такие
    // диски попадать не должны.
    bool presentOnly{true};
    // Предохранитель на число интерфейсов. Нормально обход заканчивается на
    // ERROR_NO_MORE_ITEMS, но сломанный драйвер может отдавать индексы вечно —
    // FR-1 требует, чтобы обход не был бесконечным.
    std::uint32_t maxDevices{64};
};

struct EnumerateResult {
    std::vector<DiskInterface> disks;
    std::vector<Issue> issues;
    // Обход SetupAPI состоялся и доведён до конца. Не означает, что все диски
    // ответили: неудачи по отдельным устройствам лежат в issues, а диск без
    // номера остаётся в disks с number == -1 и numberKnown == false.
    bool ok{false};
};

// Основная функция модуля. Ничего не бросает и не требует повышения прав:
// номер диска — запрос без прав доступа (IOCTL_STORAGE_GET_DEVICE_NUMBER имеет
// FILE_ANY_ACCESS), поэтому список получается и в непривилегированном UI.
//
// Порядок результата детерминирован: известные номера по возрастанию, затем
// диски без номера по devicePath. Иначе карта дисков (FR-2) и отчёт (FR-8)
// дрожали бы между прогонами (SPEC §6.4 — снимок не мутируется после публикации).
EnumerateResult enumerateDiskInterfaces(const EnumerateOptions& options = {});

// Отправить IOCTL_STORAGE_GET_DEVICE_NUMBER по device path вида "\\?\X#&…\".
// Открывает устройство, ждёт ответа не дольше kDeviceTimeoutMs и закрывает
// дескриптор. false — устройство не ответило; win32Error получает код (или
// ERROR_TIMEOUT). Нужна соседним модулям (FR-1 п.2-4), которые открывают
// устройство своими правами доступа и хотят тот же таймаут.
//
// Win32-ошибки возвращаются кодом, а не исключением: единственное, что может
// вылететь, — std::bad_alloc на буфер ответа (12 байт) и на путь устройства.
bool queryDeviceNumber(const std::wstring& devicePath, RawDeviceNumber& out, std::uint32_t& win32Error);

// Путь "\\.\PhysicalDriveN" — форма, которую требует FR-1 п.3 для
// IOCTL_DISK_GET_LENGTH_INFO. Диска с таким номером может не оказаться
// (уехал, не отвечает), поэтому результат не проверяется: проверка — это
// открытие устройства, а оно в другом модуле.
std::wstring physicalDrivePath(int number);

// То же в UTF-8: модель хранит пути как UTF-8 (§6.3).
std::string physicalDrivePathUtf8(int number);

}  // namespace mrproper::platform::devices
