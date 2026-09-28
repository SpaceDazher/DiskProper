// Размер физического диска и свободное место на томе, плюс таймауты на
// «мёртвые» устройства. Спека: §4 FR-1 п.3 (IOCTL_DISK_GET_LENGTH_INFO на
// \\.\PhysicalDriveN, открывается с GENERIC_READ|GENERIC_WRITE), п.6
// (GetDiskFreeSpaceExW) и требование к результату FR-1: «устойчивость к
// "отказавшим" дискам (таймаут 2 с на устройство, устройство помечается
// недоступным, приложение не падает)». §10 — риск «Диск/раздел отваливается в
// IOCTL» → таймауты, изоляция на устройство, degraded-режим.
//
// Модуль намеренно самодостаточен: он не включает заголовки соседних модулей
// src/platform — все они пишутся параллельно, и size_probe обязан собираться и
// проверяться без них. Зависимость ровно одна и только вниз: core (лог, §6.2).
// Смежные обязанности живут в других модулях: перечисление дисков —
// platform::devices, метка/ФС/флаги тома — platform::volumes, кэш картины и
// реакция на WM_DEVICECHANGE — DiskInventory.
//
// Как устроен таймаут: граница держится на стороне вызывающего, а не отменой
// вызова Win32. Вызов выполняется в отдельном потоке, вызывающий ждёт результат
// не дольше своего таймаута и по его истечении помечает устройство недоступным.
// Это не зависит от того, умеет ли драйвер overlapped-операции, и сохраняет
// изоляцию устройств: зависший диск не отравляет опрос следующего (поток
// предыдущего остаётся внутри драйвера и убирает состояние сам, когда вызов
// вернётся). Следствие, о котором стоит знать: на таймауте остаётся висящий
// поток в ядре — это плата за «приложение не падает», её видно в
// probeTimeoutStats().
#pragma once

#include "devices.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mrproper::platform {

// Таймаут на одно устройство (SPEC §4 FR-1: «таймаут 2 с на устройство»).
inline constexpr std::chrono::milliseconds kDefaultDeviceTimeout{2000};

// Итог одной попытки опроса устройства. Ok — единственное состояние, при
// котором данные можно класть в модель; всё остальное — это «устройство
// помечено недоступным» из требования к результату FR-1.
enum class ProbeStatus : std::uint8_t {
    Ok,             // данные получены
    TimedOut,       // вызов не уложился в таймаут: устройство не отвечает
    AccessDenied,   // нет прав на дескриптор (обычно — процесс не повышен)
    NotFound,       // устройства или тома нет: диск отключён, буква не назначена
    Unsupported,    // драйвер/том не отдаёт запрошенные данные
    InvalidArgument,// пустой путь или отрицательный номер диска
    Unavailable,    // прочие ошибки Win32
};

// Стабильное имя состояния для лога, JSON-дампа дисков и UI: не локализуется.
const wchar_t* toString(ProbeStatus status) noexcept;

// Человекочитаемое описание результата одним предложением: имя состояния плюс
// текст системного сообщения по коду Win32. Пустая строка только при Ok.
std::wstring formatProbeError(ProbeStatus status, std::uint32_t win32Error);

// Размер физического диска (SPEC §4 FR-1 п.3).
struct DiskSizeResult {
    std::uint64_t lengthBytes{};                    // GET_LENGTH_INFORMATION::Length
    ProbeStatus status{ProbeStatus::Unavailable};
    std::uint32_t win32Error{};                     // код Win32; при TimedOut — WAIT_TIMEOUT
    std::chrono::milliseconds elapsed{};             // сколько ждали результат

    [[nodiscard]] bool ok() const noexcept { return status == ProbeStatus::Ok; }
};

// Свободное место на томе (SPEC §4 FR-1 п.6, GetDiskFreeSpaceExW).
struct VolumeSpace {
    std::uint64_t totalBytes{};         // размер тома
    std::uint64_t freeBytesTotal{};     // свободно всего на томе
    std::uint64_t freeBytesToCaller{};  // свободно с учётом квоты пользователя

    // Занято = размер минус свободно. Порядок вычитания защищает от
    // «занято больше тома», когда значения пришли в разные моменты времени.
    [[nodiscard]] std::uint64_t usedBytes() const noexcept {
        return freeBytesTotal >= totalBytes ? std::uint64_t{0} : totalBytes - freeBytesTotal;
    }

    // Доля занятого места, 0.0 на пустом томе — иначе деление на ноль в UI.
    [[nodiscard]] double usedFraction() const noexcept {
        if (totalBytes == 0) {
            return 0.0;
        }
        return static_cast<double>(usedBytes()) / static_cast<double>(totalBytes);
    }
};

struct VolumeSpaceResult {
    VolumeSpace space;
    ProbeStatus status{ProbeStatus::Unavailable};
    std::uint32_t win32Error{};
    std::chrono::milliseconds elapsed{};

    [[nodiscard]] bool ok() const noexcept { return status == ProbeStatus::Ok; }
};

// Счётчики таймаутов — только диагностика (§12: в отчёте видно, сколько
// устройств не ответило). На поведение запросов не влияет.
struct ProbeTimeoutStats {
    std::uint32_t abandonedCalls{};  // вызовы, не уложившиеся в таймаут
};

[[nodiscard]] ProbeTimeoutStats probeTimeoutStats() noexcept;

// «\\.\PhysicalDriveN» из номера диска (0, 1, 2…); пустая строка при
// отрицательном номере. Нужен вызывающему и для логов, и для собственных
// вызовов, чтобы номер диска превращался в путь в одном месте.
// Реализация живёт в devices (canonical), здесь только переиспользование:
// своя копия раньше конфликтовала с layout (C2371).
using devices::physicalDrivePath;

// Опрашивающие функции ниже — noexcept: нехватка памяти и любая ошибка Win32
// возвращаются статусом, а не исключением (SPEC §4 FR-1: «приложение не
// падает»). timeout <= 0 означает явный отказ от ожидания: вызов выполняется в
// потоке вызывателя, и зависание устройства ничем не ограничено.

// Размер физического диска по номеру: сам собирает «\\.\PhysicalDriveN».
// Именно этот вызов заполняет PhysicalDisk::sizeBytes (SPEC §6.3).
[[nodiscard]] DiskSizeResult queryDiskSize(int diskNumber,
                                           std::chrono::milliseconds timeout = kDefaultDeviceTimeout) noexcept;

// Размер по готовому device path — как его отдаёт platform::devices
// («\\?\X#&…» после удаления завершающего «\»), либо «\\.\PhysicalDriveN».
[[nodiscard]] DiskSizeResult queryDiskSize(std::wstring_view devicePath,
                                           std::chrono::milliseconds timeout = kDefaultDeviceTimeout) noexcept;

// Свободное место по пути: корень тома («C:\», «\\?\Volume{…}\»), буква
// или любой каталог — GetDiskFreeSpaceExW отвечает за том, содержащий путь.
// Именно этот вызов заполняет Volume::totalBytes и Volume::freeBytes (§6.3);
// в модель кладётся freeBytesTotal, а freeBytesToCaller остаётся для UI,
// где полезно показать квоту пользователя отдельно.
[[nodiscard]] VolumeSpaceResult queryVolumeSpace(std::wstring_view rootPath,
                                                 std::chrono::milliseconds timeout = kDefaultDeviceTimeout) noexcept;

}  // namespace mrproper::platform
