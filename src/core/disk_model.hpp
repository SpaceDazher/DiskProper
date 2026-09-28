// Диски, разделы и тома в переносимой форме: занятость, свободное место,
// поиск тома по пути, проверка согласованности данных и текстовый экспорт
// карты разделов (SPEC §6.3, §4 FR-1, §4 FR-2).
//
// Разделение слоёв (SPEC §6.1, ADR-004): platform::devices/layout/volumes/
// size_probe добывают сырые числа (SetupAPI, IOCTL, GetVolumeInformationW),
// а всё, что после них считается переносимо, живёт здесь. В модуле нет ни
// windows.h, ни COM, ни ввода-вывода — только арифметика по числам, сравнение
// путей и текст. Поэтому юнит-тесты к нему собираются и проходят на любом
// хосте, а на Windows он остаётся обычной переносимой функцией.
//
// Границы модуля (что он сознательно НЕ делает):
//   * не опрашивает устройства — это платформа, FR-1 п.1-7;
//   * не сопоставляет GPT-GUID с PartitionKind — это platform::gpt_kind
//     (задача 39). Здесь живёт только проверка, что разметка не противоречит
//     сама себе, потому что таблица GUID принадлежит платформенному слою;
//   * не печатает JSON карты разделов — это core::report_json (задача 11).
//     Здесь текст для баг-репортов: FR-2 требует экспорт «в JSON и в текст».
//
// Про имена: toString(BusType), toString(PartitionKind), toString(PlanAction) и
// toString(Category) объявлены в model.hpp, но определений не имеют (модуля
// model.cpp в проекте нет), поэтому ссылаться на них из кода нельзя — линкер
// ответит LNK2019. Свои функции здесь названы иначе (busTypeName,
// partitionKindName), чтобы не маскировать чужую дыру под свою.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"

namespace mrproper::core {

// ---------------------------------------------------------------------------
// Пороги предупреждения о нехватке места (SPEC §4 FR-2: «свободно», «мало
// места» в дереве дисков). Числа — политика интерфейса, а не измерение, и
// поэтому живут здесь, рядом с местом, где они применяются.
// ---------------------------------------------------------------------------

// Доля свободного места, ниже которой том считается тесным.
inline constexpr double kLowFreeFraction = 0.10;

// Абсолютный запас: 2 ГиБ. Нужен для тома в 8 ГБ, где 10 % — это 800 МБ и
// «тесно» там, где приложению ещё есть где расти.
inline constexpr std::uint64_t kLowFreeBytes = 2147483648ull;

// ---------------------------------------------------------------------------
// Пути
//
// Сравнение путей Windows: регистронезависимо, «/» и «\» — один разделитель,
// повторы разделителей схлопываются, хвостовой разделитель незначим
// («C:\Mount» = «C:\Mount\»). Схлопывание не трогает пару разделителей в
// начале — это UNC-адрес «\\server\share», а не пустые разделители.
//
// Чего сравнение НЕ делает намеренно: pathIsWithin не считает относительный
// путь лежащим в корне тома. «C:Windows» — это текущий каталог диска C, а не
// содержимое корня, и приравнять его к «C:\» значило бы разрешить выход за
// пределы тома в проверке «путь внутри корня» (SPEC §10).
// ---------------------------------------------------------------------------

// Канонический вид пути с сохранением регистра: для показа и для ключей,
// где регистр не важен (pathKey).
std::string normalizePath(std::string_view path);

// Нормализованный путь, приведённый к нижнему регистру ASCII: ключ для
// поиска и для группировки. Регистронезависимость Windows — не «почти»:
// «C:\Windows» и «c:\windows» — один и тот же путь.
std::string pathKey(std::string_view path);

// Равенство путей по правилам выше, включая хвостовой разделитель.
bool samePath(std::string_view a, std::string_view b) noexcept;

// Путь внутри root (включая сам root). Продолжение после корня допускается
// только через разделитель: «C:\Windows» вмещает «C:\Windows\Temp», но не
// вмещает «C:\Windows2». Корнем вида «C:» (текущий каталог диска) не
// считается ничего, кроме него самого. Корнем обычно служит точка
// монтирования тома или корень правила очистки: проверка «не выйти за свой
// корень» из SPEC §10 держится на этом.
bool pathIsWithin(std::string_view path, std::string_view root) noexcept;

// Точка монтирования вида «C:\» — ровно корень тома, без вложенной папки.
bool isVolumeRootPath(std::string_view path) noexcept;

// Буква диска в начале пути: «C:\», «c:», «D:\Data» → 'C', 'C', 'D'. Нет
// буквы (том без монтирования, UNC, путь-папка) — пусто. То, что путь
// указывает именно на корень тома, проверяет isVolumeRootPath.
std::optional<char> driveLetterOf(std::string_view mountPoint) noexcept;

// Есть ли у тома точка монтирования с буквой диска — фильтр FR-2 «только с
// буквами» (том, смонтированный в папку «D:\Data», под фильтр тоже подходит).
bool hasDriveLetter(const Volume& volume) noexcept;

// ---------------------------------------------------------------------------
// Занятость и свободное место
//
// Главное, что здесь считается, — свободное место (FR-1 п.6, FR-2). Разница
// между «размер тома», «размер раздела» и «размер диска» принципиальна:
// сложение их даёт цифру, которой на диске нет. Поэтому агрегаты держат все
// три отдельно, а проценты считаются от того, что действительно принадлежит
// файловым системам.
// ---------------------------------------------------------------------------

// Занятость тома: то, что видно в строке «C:\ — занято/свободно».
struct VolumeUsage {
    std::uint64_t totalBytes{};
    std::uint64_t freeBytes{};
    // Насыщающая разность total − free. При противоречивых данных (free > total)
    // получается 0, а факт противоречия остаётся видимым в consistent и в
    // validateInventory — молчаливый ноль здесь был бы ложью.
    std::uint64_t usedBytes{};
    double usedFraction{};  // [0, 1]
    double freeFraction{};  // [0, 1]
    // totalBytes > 0: платформа ответила размером тома. У тома без ФС
    // (метка раздела, RAW) размер нулевой — это «нет данных», а не «пусто».
    bool sizesKnown{};
    // freeBytes <= totalBytes.
    bool consistent{};
};

VolumeUsage volumeUsage(const Volume& volume) noexcept;

// Занятость физического диска. Том учитывается на том диске, в разметке
// которого он перечислен, и больше ни на каком: иначе сумма по дискам
// считала бы одно место дважды. Тома, идущие по нескольким дискам
// (динамические, спаны, RAID), помечаются счётчиком и отдельной суммой,
// чтобы интерфейс мог показать «динамический том» (FR-1 п.5).
//
// Оговорка: если платформа перечислит один и тот же том на разделах двух
// дисков, в сумме по дискам он посчитается дважды, а в inventoryUsage — один
// раз (там дедупликация по volumeGuidPath). Такой повтор ловит
// validateInventory как volume.duplicate_guid.
struct DiskUsage {
    std::uint64_t sizeBytes{};         // размер диска
    std::uint64_t partitionedBytes{};  // сумма длин разделов
    // sizeBytes − partitionedBytes. Неразмеченное место не занято и не свободно.
    std::uint64_t unallocatedBytes{};
    std::uint64_t volumeBytes{};       // сумма размеров томов, учтённых на диске
    std::uint64_t usedBytes{};
    std::uint64_t freeBytes{};
    std::uint64_t spanningFreeBytes{};  // часть freeBytes из томов через несколько дисков
    std::uint64_t readOnlyBytes{};      // тома, в которые писать нельзя
    std::uint64_t partitionCount{};
    std::uint64_t volumeCount{};
    std::uint64_t spanningVolumeCount{};
    bool sizeKnown{};  // размер диска получен
    // Есть хотя бы один том, и каждый из них ответил согласованным размером.
    bool freeKnown{};
};

DiskUsage diskUsage(const PhysicalDisk& disk) noexcept;

// Агрегат по инвентаризации. Каждый том учитывается один раз, даже если
// виден на нескольких разделах.
struct InventoryUsage {
    std::uint64_t totalBytes{};  // сумма размеров дисков
    std::uint64_t volumeBytes{}; // сколько из них отдано под файловые системы
    std::uint64_t unallocatedBytes{};
    std::uint64_t usedBytes{};
    std::uint64_t freeBytes{};
    std::uint64_t encryptedBytes{};   // BitLocker и подобное (FR-1 п.7)
    std::uint64_t readOnlyBytes{};
    std::uint64_t removableBytes{};
    // Доли считаются от volumeBytes: неразмеченное место иначе выглядело бы
    // как занятое, и полоса на 50 % вводила бы в заблуждение.
    double usedFraction{};
    double freeFraction{};
    std::uint64_t diskCount{};
    std::uint64_t partitionCount{};
    std::uint64_t volumeCount{};
    std::uint64_t knownVolumeCount{};   // размер ответили
    std::uint64_t unknownVolumeCount{}; // размер не пришёл: показать «н/д»
    std::uint64_t spanningVolumeCount{};
    bool freeKnown{};  // ни одного тома без размера
};

InventoryUsage inventoryUsage(const std::vector<PhysicalDisk>& disks);

// Доли в [0, 1] от totalBytes. Нулевой размер — ноль долей, а не «занято всё».
double freeRatio(std::uint64_t totalBytes, std::uint64_t freeBytes) noexcept;
double usedRatio(std::uint64_t totalBytes, std::uint64_t freeBytes) noexcept;

// Свободное место на томах, куда запись разрешена: FR-7 (своя корзина)
// складывается в файлы на одном из томов, обещать место на томе readOnly
// или на readOnly-диске нельзя.
std::uint64_t writableFreeBytes(const std::vector<PhysicalDisk>& disks);

// Хватает ли свободного места под указанный объём. Запас не закладывается:
// порог — политика вызывающего (см. trash::TrashLimits).
bool hasRoomFor(std::uint64_t freeBytes, std::uint64_t requiredBytes) noexcept;

// Мало ли места: доля свободного ниже порога ИЛИ абсолютный запас ниже
// порога. Для UI достаточно одного вопроса «показать предупреждение».
bool needsFreeSpaceWarning(const InventoryUsage& usage) noexcept;

// ---------------------------------------------------------------------------
// Перечисление и поиск
// ---------------------------------------------------------------------------

// Разряд разметки диска (FR-2: «GPT/MBR» в карточке раздела). Определяется по
// наличию GPT-типа у разделов; MBR — когда GPT-типов нет, а MBR-код есть.
enum class PartitionScheme { Unknown, Mbr, Gpt };
const char* toString(PartitionScheme scheme) noexcept;
PartitionScheme schemeOf(const PhysicalDisk& disk) noexcept;

// Том в плоском списке: указатели действительны, пока жив переданный вектор
// дисков. Снимок инвентаризации иммутабелен (SPEC §6.4), поэтому «живёт
// вектор» = «живёт снимок».
struct VolumeEntry {
    const Volume* volume{};
    const Partition* partition{};
    const PhysicalDisk* disk{};
    int diskNumber{-1};
    std::uint32_t partitionIndex{};
    std::size_t extentCount{};
    bool spansMultipleDisks{};
};

// Плоский список томов в порядке «диск → раздел». Том с непустым
// volumeGuidPath попадает в список один раз: один и тот же том бывает виден
// на нескольких разделах (спан-том, повторный опрос), и в агрегатах он должен
// учитываться один раз. Порядок — как во входном списке; для детерминированного
// вывода сначала вызови sortInventory.
std::vector<VolumeEntry> collectVolumes(const std::vector<PhysicalDisk>& disks);

// Приводит инвентарь к детерминированному порядку: диски по номеру (затем по
// пути устройства), разделы по смещению, точки монтирования и extent'ы — по
// возрастанию. Ничего не удаляет и не добавляет: это порядок представления,
// чтобы отчёт и скриншот не дрожали между прогонами.
void sortInventory(std::vector<PhysicalDisk>& disks);

const PhysicalDisk* findDiskByNumber(const std::vector<PhysicalDisk>& disks, int number) noexcept;
const Partition* findPartition(const PhysicalDisk& disk, std::uint32_t index) noexcept;
const Volume* findVolumeByGuidPath(const std::vector<PhysicalDisk>& disks,
                                   std::string_view volumeGuidPath) noexcept;
const Volume* findVolumeByMountPoint(const std::vector<PhysicalDisk>& disks,
                                     std::string_view mountPoint) noexcept;
// Том, в который попадает путь: из всех подходящих точек монтирования берётся
// самая длинная («C:\Mount\Data» важнее «C:\»).
const Volume* findVolumeForPath(const std::vector<PhysicalDisk>& disks, std::string_view path) noexcept;

// Раздел системный (SPEC §6.3: PartitionKind::System). Флаги system/boot в
// данных тоже учитываются: платформа обязана их выставить, но если молчала —
// раздел всё равно системный по назначению.
bool isSystemPartition(const Partition& partition) noexcept;
// Том системный: ищем по volumeGuidPath, а не по адресу — указатель из чужого
// контейнера сравнивать нельзя.
bool isSystemVolume(const std::vector<PhysicalDisk>& disks, std::string_view volumeGuidPath) noexcept;

// Платформа вообще ничего не знает о диске: ни размера, ни разделов, ни пути
// устройства (FR-1: таймаут 2 с на устройство, «устройство помечается
// недоступным, приложение не падает»). Такой диск показывают серым и не
// используют для оценки места.
bool hasDiskData(const PhysicalDisk& disk) noexcept;
bool isDiskUnavailable(const PhysicalDisk& disk) noexcept;

// ---------------------------------------------------------------------------
// Проверка согласованности (SPEC §6.3 — инварианты проверяются в тестах)
//
// Инвентаризация приходит с диска и с зовёт диск, который может не ответить,
// вернуть нули или половину данных. Молча рисовать такие цифры нельзя, но и
// ронять приложение из-за них нельзя (FR-1), поэтому платформа собирает
// данные как есть, а эта проверка перечисляет, где они противоречат сами себе.
// Коды машинные (по ним фильтруют в CI), тексты — по-русски для UI и отчёта.
// ---------------------------------------------------------------------------

enum class IssueSeverity { Info, Warning, Error };
const char* toString(IssueSeverity severity) noexcept;

struct DiskIssue {
    IssueSeverity severity{IssueSeverity::Info};
    int diskNumber{-1};        // -1 — проблема уровня инвентаризации
    bool hasPartition{false};  // иначе замечание про диск целиком
    std::uint32_t partitionIndex{};
    std::string code;    // например "volume.free_exceeds_total"
    std::string message; // по-русски, одним предложением
};

// Проверка одного диска: границы разделов, перекрытия, согласованность
// томов, флаги, которые меняют поведение приложения.
std::vector<DiskIssue> validateDisk(const PhysicalDisk& disk);
// Проверка инвентаризации целиком: плюс дубли номеров дисков, конфликты точек
// монтирования и повторы volumeGuidPath.
std::vector<DiskIssue> validateInventory(const std::vector<PhysicalDisk>& disks);

// Есть ли среди замечаний ошибки (для политики «показать данные, но
// предупредить»).
bool hasErrors(const std::vector<DiskIssue>& issues) noexcept;

// ---------------------------------------------------------------------------
// Текст для интерфейса, CLI и баг-репортов (FR-2)
// ---------------------------------------------------------------------------

std::string describeVolume(const Volume& volume);
std::string describeDisk(const PhysicalDisk& disk);
std::string describeIssue(const DiskIssue& issue);
// Вся карта разделов одним текстом: сводка, диски, разделы, тома и замечания.
std::string toText(const std::vector<PhysicalDisk>& disks);

// Названия шины и типа раздела для текста и отчётов. Отдельные функции, а не
// toString из model.hpp: те объявлены, но не определены (см. шапку файла).
const char* busTypeName(BusType bus) noexcept;
const char* partitionKindName(PartitionKind kind) noexcept;

// ---------------------------------------------------------------------------
// Снимок инвентаризации
//
// Результат не мутируется после публикации (SPEC §6.4: shared_ptr<const
// ScanResult>), поэтому снимок хранит данные копией и один раз пересчитывает
// агрегат и замечания. Указатели на элементы намеренно НЕ кэшируются: список
// томов пересобирается по требованию, и копирование снимка остаётся
// безопасным. Дисков и томов десятки, пересчёт для отрисовки не заметен.
// ---------------------------------------------------------------------------

class DiskInventory {
public:
    DiskInventory() = default;

    // Принимает данные платформы, приводит к детерминированному порядку и
    // пересчитывает агрегат с замечаниями.
    static DiskInventory fromDisks(std::vector<PhysicalDisk> disks);

    const std::vector<PhysicalDisk>& disks() const noexcept { return disks_; }
    const std::vector<DiskIssue>& issues() const noexcept { return issues_; }
    const InventoryUsage& usage() const noexcept { return usage_; }

    std::vector<VolumeEntry> volumes() const;
    const PhysicalDisk* diskByNumber(int number) const noexcept;
    // Занятость конкретного диска; sizeKnown == false, если диска нет.
    DiskUsage usageOf(int number) const noexcept;
    const Volume* findVolumeByGuidPath(std::string_view volumeGuidPath) const noexcept;
    const Volume* findVolumeForPath(std::string_view path) const noexcept;
    std::uint64_t writableFree() const;
    bool needsFreeSpaceWarning() const noexcept { return mrproper::core::needsFreeSpaceWarning(usage_); }
    std::string toText() const;

private:
    std::vector<PhysicalDisk> disks_;
    std::vector<DiskIssue> issues_;
    InventoryUsage usage_{};
};

}  // namespace mrproper::core
