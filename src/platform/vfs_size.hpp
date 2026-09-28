// Размеры элемента файловой системы: логический против аллоцированного, sparse
// и NTFS-сжатие. Спека: §4 FR-4 («LogicalBytes и AllocatedBytes, освобождаемое
// место считаем по аллоцированному размеру, т.к. free space изменяется им»),
// §6.3 (CleanupCandidate::logicalBytes/allocatedBytes и инвариант
// allocatedBytes ≤ logicalBytes с оговоркой на sparse/сжатие), §4 FR-6
// (reparse points не обходятся).
//
// Разделение слоёв (то же, что у core::sizing): здесь — только числа и признаки
// одного элемента, там — переносимый подсчёт по кандидатам. Поэтому модуль не
// знает ни про правила, ни про оценку освобождения: он отдаёт пару
// «сколько данных / сколько занято на диске» и признак allocatedKnown, а что
// из неё считать — решает core::estimateReclaim. Знать это важно и здесь:
// аллоцированный размер бывает неизвестен (нет прав, файла уже нет, том не
// поддерживает запрос), и тогда честная пара — «логический известен,
// аллоцированный нет», а не выдуманный ноль.
//
// Три способа измерить элемент, и разница между ними — не вкус, а цена:
//
//   1) measureHandle  — элемент уже открыт обходом. GetFileInformationByHandleEx
//      (FileStandardInfo) отдаёт EndOfFile и AllocationSize одним вызовом по
//      дескриптору: пара размеров согласована (файл не мог измениться между
//      двумя чтениями), разрешение пути не повторяется. Основной путь.
//   2) measurePath    — есть только путь. Атрибуты из GetFileAttributesExW,
//      аллоцированный размер из GetCompressedFileSizeW. Дёшево, но это два
//      чтения с разных моментов времени.
//   3) measureFindData — обход уже держит WIN32_FIND_DATAW из FindNextFileW:
//      логический размер в нём есть, поэтому остаётся один вызов на элемент
//      (GetCompressedFileSizeW). Для полумиллиона файлов это половина syscalls.
//
// Про GetCompressedFileSizeW: он и есть причина, по которой «аллоцированный»
// и «логический» — разные числа. Функция возвращает размер, который файл
// занимает на диске: с учётом разреженности, NTFS-сжатия и округления до
// кластера. У сжатого файла она меньше логического размера, у несжатого —
// больше (округление), у sparse — намного меньше. Именно эта величина, а не
// длина файла, меняет свободное место тома, поэтому её и кладёт
// CleanupCandidate::allocatedBytes, а план по ней и считает reclaimBytes.
//
// Чего модуль сознательно не делает.
//
//   * Не следует reparse points. У ссылки и её цели разные размеры, а обход
//     (FR-6) ссылки не спускается: измерять цель здесь означало бы посчитать
//     чужое дерево. Для reparse point аллоцированный размер не запрашивается
//     (allocatedKnown == false), признак ReparsePoint ставится в flags.
//   * Не заводит поток на элемент. size_probe так делает ради таймаута на
//     IOCTL к «мёртвому» диску; здесь запрос метаданных уже открытого файла,
//     а обход каталогов считает сотни тысяч элементов (SPEC §6.4) — поток на
//     каждый элемент дороже самой работы. Границу времени держит обход:
//     stop_token проверяется поштучно, а каталог целиком берётся под общий
//     дедлайн сессии сканирования.
//   * Не ходит в core::sizing за арифметикой и не решает, что показывать в UI:
//     наполнение Reasons и агрегатов — работа переносимого ядра.
#pragma once

#include <windows.h>  // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mrproper::platform::vfs {

// Таймаутов здесь нет намеренно (см. заголовок файла): измерение элемента —
// это один-два запроса метаданных, а не операция с отвалившимся устройством.

enum class SizeStatus : std::uint8_t {
    Ok,              // размеры получены
    NotApplicable,   // аллоцированного размера у элемента нет by design (каталог, reparse point)
    InvalidArgument, // пустой путь или непригодный дескриптор
    NotFound,        // элемента нет: удалён между обходом и измерением
    AccessDenied,    // нет прав (нет повышения, файл заблокирован)
    Unsupported,     // драйвер или файловая система не отдаёт размер
    Unavailable,     // прочие ошибки Win32
};

// Стабильное имя состояния для лога, JSON-дампа и UI: не локализуется.
[[nodiscard]] const wchar_t* toString(SizeStatus status) noexcept;

// Состояние плюс текст системного сообщения по коду Win32. Пустая строка при Ok.
[[nodiscard]] std::wstring formatSizeError(SizeStatus status, std::uint32_t win32Error);

// Признаки элемента, важные для оценки. Источник — FILE_ATTRIBUTE_* плюс сама
// пара размеров; «скрытый» и «системный» сюда не попали: на оценку они не
// влияют, а раздувать структуру под отчётку незачем.
enum class FileFlags : std::uint32_t {
    None = 0u,
    Directory = 1u << 0,            // FILE_ATTRIBUTE_DIRECTORY
    ReparsePoint = 1u << 1,         // FILE_ATTRIBUTE_REPARSE_POINT (symlink, junction, точка монтирования)
    Sparse = 1u << 2,               // FILE_ATTRIBUTE_SPARSE_FILE
    Compressed = 1u << 3,           // FILE_ATTRIBUTE_COMPRESSED (NTFS-сжатие)
    ReadOnly = 1u << 4,             // FILE_ATTRIBUTE_READONLY
    Offline = 1u << 5,              // FILE_ATTRIBUTE_OFFLINE (файл в облаке, данных на томе нет)
    RecallOnOpen = 1u << 6,         // FILE_ATTRIBUTE_RECALL_ON_OPEN
    RecallOnDataAccess = 1u << 7,   // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
};

[[nodiscard]] constexpr FileFlags operator|(FileFlags left, FileFlags right) noexcept {
    return static_cast<FileFlags>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr FileFlags operator&(FileFlags left, FileFlags right) noexcept {
    return static_cast<FileFlags>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}

// Проверка признака. Пустой признак не «имеется у всего» — иначе вызов с
// FileFlags::None молча отвечал бы true на что угодно.
[[nodiscard]] constexpr bool hasFlag(FileFlags value, FileFlags flag) noexcept {
    const std::uint32_t bits = static_cast<std::uint32_t>(flag);
    if (bits == 0u) {
        return false;
    }
    return (static_cast<std::uint32_t>(value) & bits) == bits;
}

// «признаки: sparse, compressed» — для лога и отчёта. Не локализуется, как
// и toString(SizeStatus).
[[nodiscard]] std::wstring describeFlags(FileFlags flags);

// Одна пара размеров элемента.
struct FileSize {
    std::uint64_t logicalBytes{};   // сколько данных в файле (EndOfFile / nFileSize)
    std::uint64_t allocatedBytes{}; // сколько занято на томе (AllocationSize / GetCompressedFileSizeW)
    bool allocatedKnown{false};     // false — allocatedBytes не имеет смысла, оценка по логическому
    FileFlags flags{FileFlags::None};
    // Итог измерения: удалось ли узнать элемент.
    SizeStatus status{SizeStatus::Unavailable};
    // Почему аллоцированный размер неизвестен (при allocatedKnown — Ok, при
    // каталоге или reparse point — NotApplicable).
    SizeStatus allocatedStatus{SizeStatus::Unavailable};
    std::uint32_t win32Error{};         // код Win32 по status
    std::uint32_t allocatedWin32Error{}; // код Win32 по allocatedStatus
    std::chrono::milliseconds elapsed{};

    // Элемент измерен: логический размер известен. allocatedKnown при этом
    // может быть false — это оценка, а не отказ (core::estimateReclaim).
    [[nodiscard]] bool ok() const noexcept { return status == SizeStatus::Ok; }

    // Оценка освобождаемого по аллоцированному, а при его неизвестности — по
    // логическому. Дублирует core::estimateReclaim осознанно: вызывающий в
    // горячем цикле обхода не должен тянуть переносимое ядро ради одной пары
    // чисел; политика «аллоцированный важнее логического» оттуда же.
    [[nodiscard]] std::uint64_t reclaimBytes() const noexcept {
        return allocatedKnown ? allocatedBytes : logicalBytes;
    }
};

// Только аллоцированный размер по пути — GetCompressedFileSizeW.
struct AllocatedSizeResult {
    std::uint64_t bytes{};
    SizeStatus status{SizeStatus::Unavailable};
    std::uint32_t win32Error{};

    [[nodiscard]] bool ok() const noexcept { return status == SizeStatus::Ok; }
};

// Размер кластера тома: сколько байт занимает один блок. Нужен вызывающему
// для core::SizeAccumulator::addDirectory — каталог сам по себе занимает
// кластер, и без этой величины его стоимость была бы гипотезой.
struct ClusterSize {
    std::uint64_t bytesPerCluster{};
    std::uint32_t bytesPerSector{};
    std::uint32_t sectorsPerCluster{};
    SizeStatus status{SizeStatus::Unavailable};
    std::uint32_t win32Error{};

    [[nodiscard]] bool ok() const noexcept { return status == SizeStatus::Ok; }
};

// Счётчики модуля — только диагностика (SPEC §12: по журналу видно, сколько
// элементов не измерилось). На результат запросов не влияют.
struct SizeProbeStats {
    std::uint64_t measured{};          // успешно измеренных элементов
    std::uint64_t failed{};            // не измерилось ничего
    std::uint64_t allocatedUnknown{};  // логический известен, аллоцированный нет
    std::uint64_t logThrottled{};      // подавлено записей в лог
};

[[nodiscard]] SizeProbeStats sizeProbeStats() noexcept;

// Обнулить счётчики: новая серия сканирования начинается с нуля, иначе счётчик
// прошлой серии сделает отчёт о текущей бессмысленным.
void resetSizeProbeStats() noexcept;

// Измерение элемента. Все функции noexcept: отказ Win32 и нехватка памяти —
// это значения в результате, а не исключение (SPEC §5, §4 FR-1 «приложение не
// падает»). Ни одна из них не бросает даже std::bad_alloc.

// По открытому дескриптору: FileStandardInfo, одна согласованная пара
// EndOfFile/AllocationSize. Основной путь для обхода. path нужен только для
// лога; дескриптор открыт самим вызывающим, поэтому переоткрывать элемент не
// происходит. Про reparse point: измеряется то, на что указывает дескриптор;
// чтобы увидеть саму ссылку, обход открывает её с FILE_FLAG_OPEN_REPARSE_POINT.
[[nodiscard]] FileSize measureHandle(HANDLE file, std::wstring_view pathForLog = {}) noexcept;

// По пути: атрибуты плюс GetCompressedFileSizeW. Для каталога и reparse point
// аллоцированный размер не запрашивается: каталог учитывает вызывающий через
// addDirectory, а reparse point обход не спускает.
[[nodiscard]] FileSize measurePath(std::wstring_view path) noexcept;

// По данным FindNextFileW: логический размер уже в структуре, поэтому остаётся
// один вызов на элемент. Пара размеров может описывать разные моменты времени,
// если файл меняется прямо во время обхода, — это цена одного syscall на
// элемент, и её видно по флагам и времени в результате.
[[nodiscard]] FileSize measureFindData(const WIN32_FIND_DATAW& findData, std::wstring_view path) noexcept;

// Только аллоцированный размер (GetCompressedFileSizeW) — когда логический уже
// известен и нужен ровно один вызов.
[[nodiscard]] AllocatedSizeResult queryAllocatedSize(std::wstring_view path) noexcept;

// Кластер тома, которому принадлежит путь (GetDiskFreeSpaceW). Принимается
// любой путь на томе — корень, каталог или файл; определяется том, а не элемент.
[[nodiscard]] ClusterSize queryClusterSize(std::wstring_view pathOnVolume) noexcept;

}  // namespace mrproper::platform::vfs
