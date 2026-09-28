// Тома и точки монтирования: FindFirstVolumeW/FindNextVolumeW,
// FindFirstVolumeMountPointW/FindNextVolumeMountPointW, GetVolumeInformationW
// (и его надёжный вариант по хендлу — GetVolumeInformationByHandleW) плюс
// IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS. Спека: §4 FR-1 п.5 и п.6.
//
// Зачем модуль нужен именно в таком виде.
//
// 1) Перебор букв дисков (C:\, D:\…) не перечисляет тома. Он не видит скрытые
//    разделы без буквы (Recovery, OEM, MSR) и тома, смонтированные в папку
//    («D:\Data»). Инвентаризация без них неполна, а полнота — критерий приёмки
//    (§2 G1, §12: «100 % дисков, разделов и томов»). Поэтому тома берутся
//    перечислением томов по GUID-пути, а не буквами.
//
// 2) Привязка тома к диску нужна не для красоты: только она отличает том на
//    одном разделе от динамического/спанового тома, а для спана сумма
//    «размер тома по разделу» посчитала бы одно место несколько раз
//    (core::disk_model::diskUsage это учитывает — по числу extent'ов).
//
// 3) Точки монтирования нужны и интерфейсу (дерево «диск → раздел → том»,
//    FR-2), и движку: путь кандидата определяется в том, в который он попал.
//
// Чего модуль НЕ делает и кто делает это вместо него.
//
//   * размер тома и свободное место (FR-1 п.3, GetDiskFreeSpaceExW) —
//     platform::size_probe (задача 35). Поля core::Volume::totalBytes и
//     core::Volume::freeBytes здесь остаются нулевыми: писать их в двух
//     модулях — значит однажды получить два разных ответа на один вопрос;
//   * шифрование (FR-1 п.7) — WMI, platform::wmi, поэтому
//     core::Volume::encrypted остаётся false;
//   * «грязный» том (core::Volume::dirty, поле есть в §6.3) — единственный
//     дешёвый источник, FSCTL_IS_VOLUME_DIRTY, требует GENERIC_WRITE на томе,
//     то есть повышения прав. Приложение по SPEC §5 и §12 стартует без
//     повышения, а elevation helper в зону этого модуля не входит. Поле
//     остаётся false, и это значит «не проверено», а не «том чист»: вызывающий
//     обязан трактовать его как неизвестное, пока флаг не появится;
//   * привязка тома к разделу и сама разметка (FR-1 п.4) — platform::layout;
//   * модель, агрегаты, проверка согласованности (SPEC §6.3) — core::disk_model.
//     Здесь нет ни арифметики, ни текстов для отчёта: только сырые Win32-данные.
//
// Устойчивость (FR-1: «ни один отказ IOCTL/устройства не роняет процесс»).
// Ни одна функция модуля не бросает наружу исключений и не падает на отказе:
// отказ виден в Probe::errors и в логе с путём и кодом Windows. Том, который
// не ответил, попадает в результат частично заполненным — интерфейс покажет
// его серым, а не выбросит из карты разделов молча.
//
// Таймаут. IOCTL выполняется с предельным ожиданием kDeviceTimeoutMs (2 с по
// FR-1) и отменой через CancelIoEx, чтобы «отвалившийся» диск не держал
// инвентаризацию. Граница модуля: запросы, у которых в Win32 нет OVERLAPPED
// (GetVolumeInformationW, GetVolumeInformationByHandleW, сам CreateFileW на
// том), прервать нельзя — их таймаут держит вызывающий (platform::inventory,
// задача 40). Это осознанный раздел ответственности, а не пропуск: прерывать
// такой вызов можно только потоком, а лишний поток на том — это уже не
// «устойчивость», а новый источник зависаний.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"

namespace mrproper::platform::volumes {

// Предельное ожидание одной операции с устройством, мс (FR-1: «таймаут 2 с на
// устройство, устройство помечается недоступным»).
inline constexpr std::uint32_t kDeviceTimeoutMs = 2000;

// Что именно не получилось. Не одна «ошибка», а набор: у тома могут не
// ответить и точки монтирования, и файловая система, и extent'ы, и это три
// разные новости для интерфейса и для журнала. Константы — биты, поэтому
// ошибки складываются через |.
enum class Error : std::uint32_t {
    None = 0u,
    Enumerate = 1u << 0,   // FindFirstVolumeW / FindNextVolumeW
    MountPoints = 1u << 1, // FindFirstVolumeMountPointW / FindNextVolumeMountPointW
    Information = 1u << 2, // GetVolumeInformationW / GetVolumeInformationByHandleW
    Open = 1u << 3,        // CreateFileW на том не дал хендл
    Extents = 1u << 4,     // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
    Timeout = 1u << 5,     // IOCTL не уложился в kDeviceTimeoutMs
    Path = 1u << 6,        // путь не удалось привести из UTF-8 в UTF-16
    Internal = 1u << 7,    // непредвиденный отказ:bad_alloc, обрыв выделения
};

constexpr Error operator|(Error left, Error right) noexcept {
    return static_cast<Error>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr Error& operator|=(Error& left, Error right) noexcept {
    left = left | right;
    return left;
}

// Есть ли в наборе конкретный отказ.
constexpr bool has(Error set, Error flag) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0u;
}

constexpr bool failed(Error set) noexcept { return set != Error::None; }

// Короткое имя отказа для лога и для UI: «extents», «timeout»…
const char* errorName(Error error) noexcept;

// Том, собранный из одной выборки. Поля заполнены настолько, насколько
// ответили вызовы: частично заполненный том — норма, полностью пустой —
// сигнал, что том не виден вовсе.
struct Probe {
    core::Volume volume;
    // Том существует и о нём что-то известно. Пустой volumeGuidPath при
    // found == true означает «вызвать перечисление с другим путём».
    bool found{false};
    // Точки монтирования опрошены полностью (а не «перечисление оборвалось»).
    bool mountPointsKnown{false};
    // GetVolumeInformation ответил: метка, ФС и флаги достоверны.
    bool informationKnown{false};
    // Файловой системы на томе нет (RAW, метка раздела, только что
    // отформатированный том). Это не отказ, поэтому informationKnown при этом
    // остаётся true, а fileSystem пуст.
    bool fileSystemAbsent{false};
    // Extent'ы получены целиком. При spanning тома их несколько.
    bool extentsKnown{false};
    // Том лежит больше чем на одном диске: заметно интерфейсу (FR-2) и
    // проверке согласованности в core::disk_model.
    bool spansMultipleDisks{false};
    Error errors{Error::None};
    // Последний код Windows по этому тому. Не заменяет errors: у тома может
    // быть несколько разных отказов, а поле хранит один.
    std::uint32_t lastError{0};

    bool ok() const noexcept { return !failed(errors); }
};

// Результат перечисления томов системы.
struct Enumeration {
    // В порядке, который вернул FindNextVolumeW. Порядок для показа и отчёта
    // задаёт core::disk_model::sortInventory — там же он детерминирован.
    std::vector<core::Volume> volumes;
    // Сколько томов встретилось (volumes.size() и probed совпадают: том,
    // который не удалось опросить, тоже попадает в список).
    std::uint32_t probed{0};
    // true — перечисление дошло до ERROR_NO_MORE_FILES, то есть прошло по
    // всей системе. false — оборвалось на ошибке, и список неполон.
    bool completed{false};
    Error errors{Error::None};
    std::uint32_t lastError{0};
};

// Все тома системы, включая те, у которых нет буквы диска.
[[nodiscard]] Enumeration enumerate() noexcept;

// Один том по GUID-пути. Путь принимается в UTF-8 в виде модели
// («\\?\Volume{GUID}», завершающий разделитель не обязателен). Нулевая строка
// даёт Probe с errors = Error::Path.
[[nodiscard]] Probe query(std::string_view volumeGuidPathUtf8) noexcept;

// Только точки монтирования тома («C:\», «D:\Data», «\\server\share»).
// Отдельная функция нужна там, где остальное о томе не требуется: при
// построении дерева разделов и при поиске тома под путь.
[[nodiscard]] std::vector<std::string> mountPoints(std::string_view volumeGuidPathUtf8) noexcept;

// Приводит путь устройства к виду модели: без завершающего разделителя.
// «\\?\Volume{GUID}\» → «\\?\Volume{GUID}». Точка монтирования обрабатывается
// иначе (корень «C:\» без разделителя — это «C:», то есть текущий каталог),
// поэтому normalizeMountPoint живёт в .cpp и наружу не выставлен.
[[nodiscard]] std::string normalizeGuidPath(std::string_view devicePathUtf8) noexcept;

// Буква диска корня тома, если том смонтирован буквой: «C:\» → 'C'.
// Пустая строка на входе и том, смонтированный только в папку, дают 0.
// Тип — char, а не wchar_t: наружу из модуля выходит UTF-8 (core::Volume), и
// смешивать здесь две кодировки незачем.
[[nodiscard]] char volumeDriveLetter(std::string_view mountPointUtf8) noexcept;

}  // namespace mrproper::platform::volumes
