// MrProper — экран «Диски»: карта разделов на Direct2D, дерево на
// SysListView32 с NM_CUSTOMDRAW, карточка деталей, фильтры и экспорт карты.
//
// Разбор решений — в view_disks.hpp. Здесь только код, в порядке заголовка:
// слова и ключи → геометрия карты → раскладка окна → модель → окно.

#include "view_disks.hpp"

#include <commctrl.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM: позиция мыши в WM_LBUTTONUP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/report_json.hpp"
#include "locale.hpp"
#include "mv_bridge.hpp"
#include "renderer.hpp"
#include "theme.hpp"

namespace mrproper::ui::disks {
namespace {

using core::Partition;
using core::PhysicalDisk;
using core::Volume;

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------
//
// Макросы MRP_LOG_* из core/log.hpp непригодны: logFieldList разворачивает
// пакет в вызов logField по одному аргументу, поэтому любое поле даёт C2661, и
// макрос компилируется только вовсе без полей. Собираем поля явно — тем же
// способом, что и соседние экраны.
void logEvent(core::LogLevel level, std::string_view event, std::string_view message) noexcept {
    core::Logger::instance().write(level, event, message, core::LogFields{});
}

void logWin32(std::string_view event, std::string_view where, unsigned long code) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField("where", where));
    fields.push_back(core::logField("code", code));
    core::Logger::instance().write(core::LogLevel::Warn, event, "Win32 call failed", std::move(fields));
}

// Каталог строк должен быть загружен ДО первого контрола: без него любой
// tr(StringId) отдаёт сам ключ, и экран показывает «disks.free», «units.byte»
// и «action.refresh» вместо подписей (проверено снимком окна).
//
// Вызывать initialize() должна оболочка один раз при старте (locale.hpp:456
// прямо требует «до создания окон»), но в проекте у неё нет ни одного вызова —
// проверено поиском по src/. Поэтому экран проверяет флаг сам: initialize()
// идемпотентна, цена проверки — одно чтение bool, а польза — экран остаётся
// осмысленным и без оболочки.
void ensureStrings() noexcept {
    if (!mrproper::ui::isInitialized()) (void)mrproper::ui::initialize();
}

// ---------------------------------------------------------------------------
// Слова узлов и флагов
// ---------------------------------------------------------------------------
//
// Девять слов раньше были объявлены здесь как пары ru/en мимо каталога строк, с
// припиской «список ключей для владельца каталога». Владелец каталога — модуль
// локализации (src/ui/locale.hpp), и ключи добавлены: disks.word.*. Осталось
// тонкое отображение «слово → идентификатор», а не сама строка, поэтому перевод
// из ресурсов и смена языка работают здесь ровно так же, как на остальных
// экранах.
//
// Всё остальное подписывается штатными ключами и нейтральными именами
// core::disk_model (partitionKindName, busTypeName, toString(PartitionScheme)),
// которые не переводятся и потому одинаковы в обоих языках.
enum class Word : std::uint8_t {
    Disk,
    Partition,
    Volume,
    System,
    Bootable,
    Hidden,
    Removable,
    ReadOnly,
    Unallocated,
};

constexpr StringId wordKey(Word value) noexcept {
    switch (value) {
    case Word::Disk: return StringId::kDisksWordDisk;
    case Word::Partition: return StringId::kDisksWordPartition;
    case Word::Volume: return StringId::kDisksWordVolume;
    case Word::System: return StringId::kDisksWordSystem;
    case Word::Bootable: return StringId::kDisksWordBootable;
    case Word::Hidden: return StringId::kDisksWordHidden;
    case Word::Removable: return StringId::kDisksWordRemovable;
    case Word::ReadOnly: return StringId::kDisksWordReadOnly;
    case Word::Unallocated: return StringId::kDisksWordUnallocated;
    }
    return StringId::kCommonUnknown;
}

std::string word(Word value) { return tr(wordKey(value)); }

// ---------------------------------------------------------------------------
// Ключи узлов
// ---------------------------------------------------------------------------
//
// Ключ данных, а не индекс: индекс меняется при новой инвентаризации, а
// выделение и раскрытие обязаны пережить и WM_DEVICECHANGE, и перезапуск
// приложения (PageState, §5, §7.2). Вид ключа разбирается обратно без
// регулярных выражений: это три префикса и цифры, а разбор строки из файла
// настроек не должен иметь граблей.
std::string diskKey(int number) { return "d:" + std::to_string(number); }

std::string partitionKey(int diskNumber, std::uint32_t index) {
    return "d:" + std::to_string(diskNumber) + "p:" + std::to_string(index);
}

std::string volumeKey(int diskNumber, std::uint32_t index) { return partitionKey(diskNumber, index) + "v:0"; }

// Ключ неразмеченного промежутка. В списке такой строки нет (неразмеченное
// место — не раздел), но на карте он виден, и по нему hitTest обязан вернуть
// хоть что-то: иначе клик по пустому месту внутри диска пропадёт.
std::string gapKey(int diskNumber, std::uint64_t offsetBytes) {
    return "d:" + std::to_string(diskNumber) + "u:" + std::to_string(offsetBytes);
}

std::string parentOf(std::string_view key) {
    const std::size_t volume = key.rfind("v:");
    if (volume != std::string_view::npos) return std::string(key.substr(0, volume));
    const std::size_t partition = key.find("p:");
    if (partition != std::string_view::npos) return std::string(key.substr(0, partition));
    return std::string();
}

// ---------------------------------------------------------------------------
// Мелкие текстовые помощники
// ---------------------------------------------------------------------------

// «Нет данных» в узкой колонке. Не перевод и не пустая строка: пустая ячейка в
// таблице размеров читается как «ноль», а это ложь (FR-1: устройство могло не
// ответить). Короткое тире помещается в колонку при 150 % DPI, а «Неизвестно»
// нет (§12: нет обрезанных строк).
constexpr std::string_view kNoData = "—";

std::string valueOrDash(std::string_view value) {
    return value.empty() ? std::string(kNoData) : std::string(value);
}

std::string sizeOrDash(std::uint64_t bytes, bool known) {
    return known ? formatBytes(bytes) : std::string(kNoData);
}

std::string yesNo(bool value) { return value ? tr(StringId::kActionYes) : tr(StringId::kActionNo); }

std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

// Склейка списка: точки монтирования, файловые системы, активные фильтры.
// Разделитель одинаков для обоих языков — это данные, а не проза.
std::string joinWith(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.append(separator);
        out.append(parts[i]);
    }
    return out;
}

void appendUnique(std::vector<std::string>& target, const std::string& value) {
    if (value.empty()) return;
    if (std::find(target.begin(), target.end(), value) != target.end()) return;
    target.push_back(value);
}

// «Метка: значение». Двоеточие не переводится — это разделитель технического
// вывода, а не проза.
std::string field(std::string_view label, std::string_view value) {
    return std::string(label) + ": " + valueOrDash(value);
}

// Точка монтирования, которую имеет смысл открыть в проводнике: сначала та, что
// с буквой диска («C:\»), иначе первая (том, смонтированный в папку, —
// «D:\Data»; проводник откроет именно её).
std::string explorerPathOf(const Volume& volume) {
    for (const std::string& mount : volume.mountPoints) {
        if (core::driveLetterOf(mount).has_value()) return mount;
    }
    return volume.mountPoints.empty() ? std::string() : volume.mountPoints.front();
}

// «Диск 0 · GPT · Samsung SSD 980». Схема разметки (GPT/MBR, FR-2) стоит
// сразу после номера: её видно без клика по разделу. Неизвестная схема молчит —
// иначе в английском интерфейсе появилось бы русское «неизвестно» из core.
std::string diskTitle(const PhysicalDisk& disk) {
    std::string title = word(Word::Disk) + " " + std::to_string(disk.number);
    const core::PartitionScheme scheme = core::schemeOf(disk);
    if (scheme != core::PartitionScheme::Unknown) title += " · " + std::string(core::toString(scheme));
    if (!disk.model.empty()) title += " · " + disk.model;
    return title;
}

std::string partitionTitle(const Partition& partition) {
    std::string title = word(Word::Partition) + " " + std::to_string(partition.index);
    // GPT-имя («Windows», «EFI») узнаваемее служебного имени типа, но и оно может
    // быть пустым, и тогда показываем тип из core — он нейтрален.
    if (!partition.gptName.empty()) {
        title += " · " + partition.gptName;
    } else {
        title += " · " + std::string(core::partitionKindName(partition.kind));
    }
    return title;
}

std::string volumeTitle(const Volume& volume) {
    std::string title;
    for (const std::string& mount : volume.mountPoints) {
        if (const std::optional<char> letter = core::driveLetterOf(mount)) {
            title = std::string(1, *letter) + ":";
            break;
        }
    }
    if (title.empty() && !volume.label.empty()) title = volume.label;
    if (title.empty() && !volume.volumeGuidPath.empty()) {
        // Том без буквы и без метки виден только по GUID: показываем хвост пути
        // («Volume{…}»), а не путь целиком — в строке списка он не помещается.
        const std::size_t slash = volume.volumeGuidPath.find_last_of("\\/");
        title = slash == std::string::npos ? volume.volumeGuidPath : volume.volumeGuidPath.substr(slash + 1);
    }
    if (title.empty()) title = word(Word::Volume);
    if (!volume.label.empty() && title.find(volume.label) == std::string::npos) {
        title += " (" + volume.label + ")";
    }
    if (!volume.fileSystem.empty()) title += " · " + volume.fileSystem;
    return title;
}

// ---------------------------------------------------------------------------
// Признаки узла
// ---------------------------------------------------------------------------

bool diskHasSystemPart(const PhysicalDisk& disk) {
    for (const Partition& partition : disk.partitions) {
        if (core::isSystemPartition(partition)) return true;
    }
    return false;
}

bool diskHasEncryptedVolume(const PhysicalDisk& disk) {
    for (const Partition& partition : disk.partitions) {
        if (partition.hasVolume && partition.volume.encrypted) return true;
    }
    return false;
}

bool diskHasLetteredVolume(const PhysicalDisk& disk) {
    for (const Partition& partition : disk.partitions) {
        if (partition.hasVolume && core::hasDriveLetter(partition.volume)) return true;
    }
    return false;
}

// Мало ли места. Пороги берутся из core (kLowFreeFraction, kLowFreeBytes): свои
// числа в экране означали бы, что он предупреждает о «тесноте» там, где CLI и
// отчёт молчат, а §11.4 требует одних и тех же цифр на всех поверхностях.
bool lowSpaceOn(std::uint64_t totalBytes, std::uint64_t freeBytes, bool known) {
    if (!known) return false;
    if (freeBytes < core::kLowFreeBytes) return true;
    return totalBytes > 0 && core::freeRatio(totalBytes, freeBytes) < core::kLowFreeFraction;
}

}  // namespace

// ---------------------------------------------------------------------------
// Перечисления
// ---------------------------------------------------------------------------

const char* toString(SortKey key) noexcept {
    switch (key) {
    case SortKey::Number: return "number";
    case SortKey::Size: return "size";
    case SortKey::FreeSpace: return "free";
    case SortKey::Model: return "model";
    }
    return "number";
}

const char* toString(ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::Json: return "json";
    case ExportFormat::Text: return "text";
    }
    return "json";
}

bool isDisksControl(WORD controlId) noexcept {
    return controlId >= static_cast<WORD>(ControlId::First) && controlId <= static_cast<WORD>(ControlId::Last);
}

// ---------------------------------------------------------------------------
// Карта: высота и геометрия (SPEC §4 FR-2)
// ---------------------------------------------------------------------------

float mapHeightDipFor(const MapMetrics& metrics, std::size_t diskCount) noexcept {
    if (diskCount == 0) return static_cast<float>(metrics.labelHeightDip + metrics.captionHeightDip);
    const double block = metrics.labelHeightDip + metrics.barHeightDip + metrics.captionHeightDip;
    return static_cast<float>(block * static_cast<double>(diskCount) +
                               metrics.diskGapDip * static_cast<double>(diskCount - 1U) +
                               2.0 * metrics.paddingDip);
}

namespace {

// Насколько можно сжать блоки, прежде чем карта станет «тесной» и об этом
// сообщит строка состояния. Молча пропавшие диски хуже, чем тесная, но
// честная карта (FR-2, §12).
constexpr double kMinBlockScale = 0.55;

struct BlockHeights {
    float label{};
    float bar{};
    float caption{};
    float gap{};
};

BlockHeights blockHeights(const MapMetrics& metrics, double scale) noexcept {
    BlockHeights out;
    out.label = static_cast<float>(metrics.labelHeightDip * scale);
    out.bar = static_cast<float>(metrics.barHeightDip * scale);
    out.caption = static_cast<float>(metrics.captionHeightDip * scale);
    out.gap = static_cast<float>(metrics.diskGapDip * scale);
    return out;
}

}  // namespace

MapLayout computeMap(const MapMetrics& metrics, const std::vector<MapDiskModel>& disks, float widthDip,
                     float heightDip) noexcept {
    MapLayout layout;
    layout.width = std::max(0.0F, widthDip);
    layout.height = std::max(0.0F, heightDip);
    if (disks.empty()) return layout;

    const double needed = static_cast<double>(mapHeightDipFor(metrics, disks.size()));
    double scale = 1.0;
    if (needed > 0.0 && static_cast<double>(layout.height) < needed) {
        scale = std::max(kMinBlockScale, static_cast<double>(layout.height) / needed);
    }
    layout.cramped = scale < 0.999;

    const BlockHeights blocks = blockHeights(metrics, scale);
    const float left = static_cast<float>(metrics.paddingDip);
    const float right = layout.width - static_cast<float>(metrics.paddingDip);
    const float trackWidth = std::max(1.0F, right - left);
    const float gap = static_cast<float>(metrics.segmentGapDip) * static_cast<float>(scale);
    if (trackWidth < static_cast<float>(metrics.minWidthDip)) layout.cramped = true;

    float y = static_cast<float>(metrics.paddingDip);
    for (const MapDiskModel& disk : disks) {
        if (y >= layout.height) {
            // Дальше идти некуда: отмечаем карту тесной, чтобы вызывающий
            // показал это в строке состояния, а не выдал обрезанную карту за
            // полную.
            layout.cramped = true;
            break;
        }
        MapBar bar;
        bar.diskNumber = disk.diskNumber;
        bar.label = disk.label;
        bar.subtitle = disk.subtitle;
        bar.caption = disk.caption;
        bar.sizeBytes = disk.sizeBytes;
        bar.unavailable = disk.unavailable;
        bar.selected = disk.selected;
        bar.left = left;
        bar.width = trackWidth;
        bar.labelTop = y;
        bar.labelHeight = std::max(1.0F, blocks.label);
        y += bar.labelHeight;
        bar.top = y;
        bar.height = std::max(1.0F, blocks.bar);
        y += bar.height;
        bar.captionTop = y;
        bar.captionHeight = std::max(1.0F, blocks.caption);
        y += bar.captionHeight + blocks.gap;

        const std::uint64_t total = disk.sizeBytes;
        const std::size_t count = disk.segments.size();
        for (std::size_t index = 0; index < count; ++index) {
            const MapSegmentModel& segment = disk.segments[index];
            MapSegment drawn;
            drawn.key = segment.key;
            drawn.label = segment.label;
            drawn.unallocated = segment.unallocated;
            drawn.system = segment.system;
            drawn.encrypted = segment.encrypted;
            drawn.selected = segment.selected;
            drawn.unavailable = segment.unavailable;
            drawn.top = bar.top;
            drawn.height = bar.height;
            if (total > 0) {
                const double begin = static_cast<double>(segment.offsetBytes) / static_cast<double>(total);
                const double size = static_cast<double>(segment.lengthBytes) / static_cast<double>(total);
                drawn.fraction = std::clamp(size, 0.0, 1.0);
                drawn.left = left + static_cast<float>(begin) * trackWidth;
                // Последний сегмент дотягиваем до правого края: сумма долей из-за
                // округления не равна ровно единице, иначе справа остаётся щель в
                // доли пикселя, которая читается как «место кончилось».
                if (segment.offsetBytes + segment.lengthBytes >= total) {
                    drawn.width = std::max(1.0F, right - drawn.left);
                } else {
                    drawn.width = std::max(1.0F, static_cast<float>(size) * trackWidth - gap);
                }
            } else {
                // Размер диска неизвестен (FR-1: устройство не ответило) — доли
                // посчитать не от чего, поэтому каждый сегмент занимает равную
                // долю. Это честное «пропорционально не построена», а не
                // выдуманный размер.
                const double size = count > 0 ? 1.0 / static_cast<double>(count) : 0.0;
                drawn.fraction = size;
                drawn.left = left + static_cast<float>(size * static_cast<double>(index) * static_cast<double>(trackWidth));
                drawn.width = std::max(1.0F, static_cast<float>(size) * trackWidth - gap);
            }
            if (drawn.left + drawn.width > right) {
                drawn.width = std::max(1.0F, right - drawn.left);
            }

            if (segment.hasVolume && segment.volume.known && segment.volume.totalBytes > 0) {
                MapVolumeBar volumeBar;
                volumeBar.key = segment.volume.key;
                volumeBar.parent = segment.key;
                volumeBar.known = true;
                volumeBar.lowSpace = segment.volume.lowSpace;
                volumeBar.selected = segment.volume.selected;
                const double used = 1.0 - core::freeRatio(segment.volume.totalBytes, segment.volume.freeBytes);
                volumeBar.usedFraction = std::clamp(used, 0.0, 1.0);
                const float fill =
                    std::min(static_cast<float>(metrics.fillHeightDip) * static_cast<float>(scale),
                             std::max(1.0F, drawn.height - 2.0F));
                volumeBar.top = drawn.top + drawn.height - fill;
                volumeBar.height = fill;
                volumeBar.left = drawn.left + 1.0F;
                volumeBar.width = std::max(1.0F, drawn.width - 2.0F);
                bar.volumes.push_back(volumeBar);
            }
            bar.segments.push_back(drawn);
        }
        layout.bars.push_back(std::move(bar));
    }
    return layout;
}

std::optional<std::string> MapLayout::hitTest(float x, float y) const {
    if (x < 0.0F || y < 0.0F || x > width || y > height) return std::nullopt;
    for (const MapBar& bar : bars) {
        // Блок диска целиком, а не только полоса: подпись сверху и «свободно …
        // из …» снизу принадлежат тому же диску, и клик по ним обязан его
        // выбирать. Иначе нажимаема оказывается только треть окна, и человек
        // заключает, что клик по разделу сломан.
        const float blockTop = std::min(bar.labelTop, bar.top);
        const float blockBottom = bar.captionHeight > 0.0F
                                      ? bar.captionTop + bar.captionHeight
                                      : bar.top + bar.height;
        if (y < blockTop || y > blockBottom) continue;
        // От глубокого к широкому: полоса тома лежит внутри сегмента, сегмент —
        // внутри полосы диска. Клик по узкой полосе должен попасть в узел, а не
        // в его родителя.
        for (const MapVolumeBar& volume : bar.volumes) {
            if (x < volume.left || x > volume.left + volume.width) continue;
            if (y < volume.top || y > volume.top + volume.height) continue;
            if (!volume.key.empty()) return volume.key;
        }
        for (const MapSegment& segment : bar.segments) {
            if (x < segment.left || x > segment.left + segment.width) continue;
            if (y < segment.top || y > segment.top + segment.height) continue;
            if (!segment.key.empty()) return segment.key;
        }
        return diskKey(bar.diskNumber);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Раскладка окна
// ---------------------------------------------------------------------------

bool DisksRect::empty() const noexcept { return width <= 0 || height <= 0; }

bool DisksRect::contains(int px, int py) const noexcept {
    return px >= x && px < x + width && py >= y && py < y + height;
}

std::vector<int> DisksLayout::fitWidths(int availablePx, const std::vector<int>& desiredPx, int gapPx,
                                        int minWidthPx) {
    std::vector<int> widths;
    if (desiredPx.empty()) return widths;
    const std::size_t count = desiredPx.size();
    const long long gaps = static_cast<long long>(std::max(0, gapPx)) * static_cast<long long>(count - 1U);
    const long long usable = static_cast<long long>(availablePx) - gaps;
    if (usable <= 0) {
        // Места нет совсем: каждой кнопке — минимум, чтобы их оставалось видно,
        // а лишние кнопки вызывающий спрячет по filterButtonCount().
        widths.assign(count, std::max(1, minWidthPx));
        return widths;
    }

    long long sum = 0;
    for (const int value : desiredPx) sum += std::max(0, value);
    widths.reserve(count);
    if (sum <= usable) {
        for (const int value : desiredPx) widths.push_back(std::max(1, value));
        return widths;
    }

    // Не помещаются — жмём пропорционально, но не ниже минимума: кнопка без
    // подписи хуже кнопки с многоточием, но её всё ещё видно и можно нажать.
    const long long floor = std::min<long long>(std::max(1, minWidthPx), usable);
    long long used = 0;
    for (const int value : desiredPx) {
        long long scaled = static_cast<long long>(std::max(0, value)) * usable / std::max<long long>(1, sum);
        scaled = std::max(floor, scaled);
        used += scaled;
        widths.push_back(static_cast<int>(std::min(scaled, usable)));
    }
    if (used > usable && !widths.empty()) {
        // Излишек отдаём последней кнопке, чтобы сумма не превысила полосу.
        const long long excess = used - usable;
        widths.back() = static_cast<int>(std::max<long long>(1, static_cast<long long>(widths.back()) - excess));
    }
    return widths;
}

DisksLayout DisksLayout::compute(const DisksMetrics& metrics, int dpi, int clientWidthPx, int clientHeightPx,
                                 int mapHeightPx, int cardLineCount,
                                 const std::vector<int>& filterWidthsPx, const std::vector<int>& actionWidthsPx) {
    DisksLayout out;
    out.width_ = std::max(0, clientWidthPx);
    out.height_ = std::max(0, clientHeightPx);

    const int pad = std::max(0, dipToPx(metrics.paddingDip, dpi));
    out.gap_ = std::max(0, dipToPx(metrics.gapDip, dpi));
    out.buttonGap_ = std::max(0, dipToPx(metrics.buttonGapDip, dpi));
    out.buttonHeight_ = std::max(1, dipToPx(metrics.buttonHeightDip, dpi));
    out.contentLeft_ = pad;
    out.contentWidth_ = std::max(0, out.width_ - 2 * pad);
    out.statusHeight_ = std::max(0, dipToPx(metrics.statusHeightDip, dpi));

    // Ширины кнопок: измеренные подписи, а при их отсутствии (первый расчёт до
    // создания контролов) — равные доли. Ровно kDisksFilterCount и
    // kDisksActionCount: раскладка обязана знать число мест заранее, иначе
    // кнопки наезжают друг на друга.
    std::vector<int> wantedFilters = filterWidthsPx;
    if (wantedFilters.empty()) {
        const int share = std::max(1, (out.contentWidth_ - out.buttonGap_ * (kDisksFilterCount - 1)) / kDisksFilterCount);
        wantedFilters.assign(kDisksFilterCount, share);
    }
    std::vector<int> wantedActions = actionWidthsPx;
    if (wantedActions.empty()) {
        const int share = std::max(1, (out.contentWidth_ - out.buttonGap_ * (kDisksActionCount - 1)) / kDisksActionCount);
        wantedActions.assign(kDisksActionCount, share);
    }
    const int minButton = std::max(1, dipToPx(metrics.buttonMinWidthDip, dpi));
    out.filterWidths_ = DisksLayout::fitWidths(out.contentWidth_, wantedFilters, out.buttonGap_, minButton);
    out.actionWidths_ = DisksLayout::fitWidths(out.contentWidth_, wantedActions, out.buttonGap_, minButton);
    out.filterCount_ = static_cast<int>(out.filterWidths_.size());
    out.actionCount_ = static_cast<int>(out.actionWidths_.size());

    const int mapMin = std::max(1, dipToPx(metrics.mapMinHeightDip, dpi));
    const int mapMax = std::max(mapMin, dipToPx(metrics.mapMaxHeightDip, dpi));
    const int listMin = std::max(0, dipToPx(metrics.listMinHeightDip, dpi));
    const int cardMin = std::max(0, dipToPx(metrics.cardMinHeightDip, dpi));
    const int cardDesired = dipToPx(metrics.cardTitleHeightDip, dpi) +
                            dipToPx(metrics.cardLineHeightDip, dpi) * std::max(0, cardLineCount) +
                            2 * dipToPx(metrics.cardPaddingDip, dpi);

    // Что уступает место, решается по важности: сначала карточка (её текст
    // дублируется деревом), потом строка состояния (цифры есть в карточке), и
    // только потом сама карта. Обратный порядок оставил бы пустое дерево — то
    // есть экран, который ничего не показывает.
    int mapHeight = std::clamp(mapHeightPx, mapMin, mapMax);
    bool showStatus = true;
    bool showCard = true;
    // Аргументы, а не захват: и showStatus, и mapHeight в цикле меняются, и
    // захваченные по значению остались бы прежними.
    const auto headerHeight = [&out](bool status, int map) {
        int height = out.buttonHeight_ * 2 + out.gap_ * 2;
        height += status ? out.statusHeight_ + out.gap_ : 0;
        return height + map + out.gap_;
    };
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int available = out.height_ - headerHeight(showStatus, mapHeight);
        const int wantedCard = showCard ? cardDesired : 0;
        if (available >= listMin + wantedCard) break;
        if (showCard) {
            showCard = false;
            continue;
        }
        if (showStatus) {
            showStatus = false;
            continue;
        }
        if (mapHeight > mapMin) {
            mapHeight = std::max(mapMin, mapHeight - std::max(0, listMin - available));
            continue;
        }
        break;
    }

    // Окончательная раскладка: скрытая строка и скрытая карточка не должны
    // оставлять после себя пустоты.
    int y = 0;
    out.filtersTop_ = y;
    y += out.buttonHeight_ + out.gap_;
    out.actionsTop_ = y;
    y += out.buttonHeight_ + out.gap_;
    out.statusTop_ = y;
    if (showStatus) y += out.statusHeight_ + out.gap_;
    out.mapTop_ = y;
    y += mapHeight + out.gap_;
    out.listTop_ = y;

    const int available = out.height_ - y;
    out.mapHeight_ = mapHeight;
    out.statusVisible_ = showStatus;
    out.cardHeight_ = 0;
    if (showCard && available > listMin) {
        out.cardHeight_ = std::min(cardDesired, available - listMin);
    }
    if (out.cardHeight_ > 0 && out.cardHeight_ < cardMin) out.cardHeight_ = 0;
    if (out.cardHeight_ > 0) {
        out.listHeight_ = std::max(0, available - out.cardHeight_ - out.gap_);
        out.cardTop_ = out.listTop_ + out.listHeight_ + out.gap_;
        out.cardVisible_ = true;
    } else {
        out.listHeight_ = std::max(0, available);
        out.cardTop_ = out.listTop_ + out.listHeight_;
        out.cardVisible_ = false;
    }
    out.cramped_ = out.listHeight_ < listMin || out.width_ < dipToPx(metrics.minWidthDip, dpi) ||
                   out.mapHeight_ < mapMin;
    return out;
}

DisksRect DisksLayout::filtersRect() const noexcept {
    return DisksRect{contentLeft_, filtersTop_, contentWidth_, buttonHeight_};
}

DisksRect DisksLayout::actionsRect() const noexcept {
    return DisksRect{contentLeft_, actionsTop_, contentWidth_, buttonHeight_};
}

DisksRect DisksLayout::statusRect() const noexcept {
    return DisksRect{contentLeft_, statusTop_, contentWidth_, statusVisible_ ? statusHeight_ : 0};
}

DisksRect DisksLayout::mapRect() const noexcept { return DisksRect{contentLeft_, mapTop_, contentWidth_, mapHeight_}; }

DisksRect DisksLayout::listRect() const noexcept { return DisksRect{contentLeft_, listTop_, contentWidth_, listHeight_}; }

DisksRect DisksLayout::cardRect() const noexcept {
    return DisksRect{contentLeft_, cardTop_, contentWidth_, cardHeight_};
}

DisksRect DisksLayout::filterButtonRect(int index) const noexcept {
    if (index < 0 || index >= filterCount_ || index >= static_cast<int>(filterWidths_.size())) return DisksRect{};
    int x = contentLeft_;
    for (int i = 0; i < index; ++i) x += filterWidths_[static_cast<std::size_t>(i)] + buttonGap_;
    return DisksRect{x, filtersTop_, filterWidths_[static_cast<std::size_t>(index)], buttonHeight_};
}

DisksRect DisksLayout::actionButtonRect(int index) const noexcept {
    if (index < 0 || index >= actionCount_ || index >= static_cast<int>(actionWidths_.size())) return DisksRect{};
    int x = contentLeft_;
    for (int i = 0; i < index; ++i) x += actionWidths_[static_cast<std::size_t>(i)] + buttonGap_;
    return DisksRect{x, actionsTop_, actionWidths_[static_cast<std::size_t>(index)], buttonHeight_};
}

int DisksLayout::filterButtonCount() const noexcept { return filterCount_; }

int DisksLayout::actionButtonCount() const noexcept { return actionCount_; }

bool DisksLayout::cardVisible() const noexcept { return cardVisible_ && cardHeight_ > 0; }

bool DisksLayout::statusVisible() const noexcept { return statusVisible_ && statusHeight_ > 0; }

bool DisksLayout::cramped() const noexcept { return cramped_; }

int DisksLayout::clientWidthPx() const noexcept { return width_; }

int DisksLayout::clientHeightPx() const noexcept { return height_; }

HitTarget DisksLayout::hitTest(int px, int py) const noexcept {
    if (px < 0 || py < 0 || px >= width_ || py >= height_) return HitTarget::None;
    if (statusVisible() && statusRect().contains(px, py)) return HitTarget::Status;
    if (filtersRect().contains(px, py)) {
        for (int i = 0; i < filterCount_; ++i) {
            if (!filterButtonRect(i).contains(px, py)) continue;
            switch (i) {
            case 0: return HitTarget::FilterLetters;
            case 1: return HitTarget::FilterSystem;
            case 2: return HitTarget::FilterRemovable;
            default: return HitTarget::Filters;
            }
        }
        return HitTarget::Filters;
    }
    if (actionsRect().contains(px, py)) {
        for (int i = 0; i < actionCount_; ++i) {
            if (!actionButtonRect(i).contains(px, py)) continue;
            switch (i) {
            case 0: return HitTarget::ExportMap;
            case 1: return HitTarget::ExportText;
            case 2: return HitTarget::OpenInExplorer;
            case 3: return HitTarget::Refresh;
            case 4: return HitTarget::Check;
            default: return HitTarget::Actions;
            }
        }
        return HitTarget::Actions;
    }
    if (mapRect().contains(px, py)) return HitTarget::Map;
    if (cardVisible() && cardRect().contains(px, py)) return HitTarget::Card;
    if (listRect().contains(px, py)) return HitTarget::List;
    return HitTarget::None;
}

// ---------------------------------------------------------------------------
// Модель
// ---------------------------------------------------------------------------

struct DisksViewModel::Impl {
    std::shared_ptr<const core::DiskInventory> inventory;
    DiskFilters filters;
    SortOrder sort{SortOrder::defaultFor(SortKey::Number)};
    std::vector<TreeRow> rows;
    std::vector<MapDiskModel> map;
    std::string selectedKey;
    // Явные переопределения раскрытия поверх defaultExpanded: у дерева есть
    // состояние по умолчанию (всё раскрыто — узлов мало, а свёрнутый том не
    // видно), и человек может свернуть отдельную ветку или всё сразу.
    std::map<std::string, bool> expansion;
    bool defaultExpanded{true};
    int mapHeightDipFloor{0};

    [[nodiscard]] bool isExpanded(const std::string& key) const {
        const auto found = expansion.find(key);
        return found == expansion.end() ? defaultExpanded : found->second;
    }

    [[nodiscard]] const PhysicalDisk* diskByNumber(int number) const {
        if (!inventory) return nullptr;
        for (const PhysicalDisk& disk : inventory->disks()) {
            if (disk.number == number) return &disk;
        }
        return nullptr;
    }

    [[nodiscard]] bool passesFilters(const PhysicalDisk& disk) const {
        if (filters.removableOnly && !disk.removable) return false;
        if (filters.letteredOnly && !diskHasLetteredVolume(disk)) return false;
        if (filters.systemOnly && !diskHasSystemPart(disk)) return false;
        return true;
    }

    // Сборка дерева и карты из снимка. Один проход на диск: и строки списка, и
    // полосы карты читают одни и те же поля, а две разные сборки рано или поздно
    // разошлись бы числами (FR-2, §12: карта обязана совпадать с эталоном).
    void rebuild();

    // Отметки выделения в карте. Отдельно от rebuild: выбор меняется на каждом
    // щелчке, а пересобирать из-за него тридцать строк и десяток полос — работа
    // впустую.
    void markSelected();

    [[nodiscard]] MapDiskModel buildMapDisk(const PhysicalDisk& disk, const core::DiskUsage& usage) const;
};

namespace {

int compareDisks(const PhysicalDisk& left, const PhysicalDisk& right, SortOrder order) {
    int result = 0;
    switch (order.key) {
    case SortKey::Number:
        result = left.number < right.number ? -1 : (left.number > right.number ? 1 : 0);
        break;
    case SortKey::Size: {
        const std::uint64_t leftSize = core::diskUsage(left).sizeBytes;
        const std::uint64_t rightSize = core::diskUsage(right).sizeBytes;
        result = leftSize < rightSize ? -1 : (leftSize > rightSize ? 1 : 0);
        break;
    }
    case SortKey::FreeSpace: {
        const std::uint64_t leftFree = core::diskUsage(left).freeBytes;
        const std::uint64_t rightFree = core::diskUsage(right).freeBytes;
        result = leftFree < rightFree ? -1 : (leftFree > rightFree ? 1 : 0);
        break;
    }
    case SortKey::Model: {
        // Регистронезависимо по ASCII: «Samsung» и «samsung» — одна модель, иначе
        // сортировка выглядела бы случайной.
        const std::string leftModel = asciiLower(left.model);
        const std::string rightModel = asciiLower(right.model);
        result = leftModel.compare(rightModel);
        break;
    }
    }
    if (order.direction == SortDirection::Descending) result = -result;
    // Вторичный ключ обязателен: без него два диска одного размера менялись бы
    // местами при каждой перечитке инвентаризации, а дерево обязано быть
    // детерминированным (core::sortInventory — тот же принцип).
    if (result == 0) return left.number < right.number ? -1 : (left.number > right.number ? 1 : 0);
    return result;
}

}  // namespace

MapDiskModel DisksViewModel::Impl::buildMapDisk(const PhysicalDisk& disk, const core::DiskUsage& usage) const {
    MapDiskModel out;
    out.diskNumber = disk.number;
    out.label = diskTitle(disk);
    out.subtitle = std::string(core::busTypeName(disk.bus));
    if (usage.freeKnown) {
        out.caption = formatPercent(core::freeRatio(usage.volumeBytes, usage.freeBytes)) + " · " +
                      formatBytes(usage.freeBytes) + " / " + formatBytes(usage.volumeBytes);
    } else {
        out.caption = std::string(kNoData) + " · " + formatBytes(usage.volumeBytes);
    }
    out.sizeBytes = disk.sizeBytes;
    out.unavailable = core::isDiskUnavailable(disk);
    out.selected = selectedKey == diskKey(disk.number);

    // Сегменты — по смещению, от нуля до размера диска. Неразмеченные
    // промежутки между разделами и после последнего тоже сегменты: иначе полоса
    // выглядит размеченной целиком, а это самая частая неправда на глаз.
    std::uint64_t cursor = 0;
    for (const Partition& partition : disk.partitions) {
        if (partition.offsetBytes > cursor) {
            MapSegmentModel gap;
            gap.key = gapKey(disk.number, cursor);
            gap.label = word(Word::Unallocated);
            gap.offsetBytes = cursor;
            gap.lengthBytes = partition.offsetBytes - cursor;
            gap.unallocated = true;
            out.segments.push_back(std::move(gap));
        }
        MapSegmentModel segment;
        segment.key = partitionKey(disk.number, partition.index);
        segment.label = partitionTitle(partition);
        segment.offsetBytes = partition.offsetBytes;
        segment.lengthBytes = partition.lengthBytes;
        segment.system = core::isSystemPartition(partition);
        segment.unavailable = out.unavailable;
        segment.selected = selectedKey == segment.key;
        if (partition.hasVolume) {
            const core::VolumeUsage volumeUsage = core::volumeUsage(partition.volume);
            segment.hasVolume = true;
            segment.encrypted = partition.volume.encrypted;
            segment.volume.key = volumeKey(disk.number, partition.index);
            segment.volume.label = volumeTitle(partition.volume);
            segment.volume.totalBytes = volumeUsage.totalBytes;
            segment.volume.freeBytes = volumeUsage.freeBytes;
            segment.volume.known = volumeUsage.sizesKnown;
            segment.volume.encrypted = partition.volume.encrypted;
            segment.volume.lowSpace = lowSpaceOn(volumeUsage.totalBytes, volumeUsage.freeBytes, volumeUsage.sizesKnown);
            segment.volume.selected = selectedKey == segment.volume.key;
        }
        out.segments.push_back(std::move(segment));
        cursor = std::max(cursor, partition.offsetBytes + partition.lengthBytes);
    }
    if (disk.sizeBytes > cursor) {
        MapSegmentModel gap;
        gap.key = gapKey(disk.number, cursor);
        gap.label = word(Word::Unallocated);
        gap.offsetBytes = cursor;
        gap.lengthBytes = disk.sizeBytes - cursor;
        gap.unallocated = true;
        out.segments.push_back(std::move(gap));
    }
    return out;
}

void DisksViewModel::Impl::rebuild() {
    rows.clear();
    map.clear();
    if (!inventory) return;

    std::vector<const PhysicalDisk*> visible;
    visible.reserve(inventory->disks().size());
    for (const PhysicalDisk& disk : inventory->disks()) {
        if (passesFilters(disk)) visible.push_back(&disk);
    }
    std::sort(visible.begin(), visible.end(), [this](const PhysicalDisk* left, const PhysicalDisk* right) {
        return compareDisks(*left, *right, sort) < 0;
    });

    for (const PhysicalDisk* disk : visible) {
        const core::DiskUsage usage = core::diskUsage(*disk);
        const bool unavailable = core::isDiskUnavailable(*disk);

        TreeRow row;
        row.kind = NodeKind::Disk;
        row.key = diskKey(disk->number);
        row.level = 0;
        row.title = diskTitle(*disk);
        row.diskNumber = disk->number;
        row.sizeBytes = disk->sizeBytes;
        row.freeBytes = usage.freeBytes;
        row.usedBytes = usage.usedBytes;
        row.sizeKnown = usage.sizeKnown;
        row.hasChildren = !disk->partitions.empty();
        row.expanded = isExpanded(row.key);
        row.system = diskHasSystemPart(*disk);
        row.encrypted = diskHasEncryptedVolume(*disk);
        row.removable = disk->removable;
        row.unavailable = unavailable;
        row.lowSpace = lowSpaceOn(usage.volumeBytes, usage.freeBytes, usage.freeKnown);
        row.freeText = sizeOrDash(usage.freeBytes, usage.freeKnown);
        row.usedText = sizeOrDash(usage.usedBytes, usage.freeKnown);
        rows.push_back(row);

        map.push_back(buildMapDisk(*disk, usage));
        if (!row.expanded) continue;

        for (const Partition& partition : disk->partitions) {
            const bool hasVolume = partition.hasVolume;
            const core::VolumeUsage volumeUsage = hasVolume ? core::volumeUsage(partition.volume) : core::VolumeUsage{};
            const bool sizesKnown = hasVolume && volumeUsage.sizesKnown;

            TreeRow partitionRow;
            partitionRow.kind = NodeKind::Partition;
            partitionRow.key = partitionKey(disk->number, partition.index);
            partitionRow.level = 1;
            partitionRow.title = partitionTitle(partition);
            partitionRow.diskNumber = disk->number;
            partitionRow.partitionIndex = partition.index;
            partitionRow.sizeBytes = partition.lengthBytes;
            partitionRow.sizeKnown = partition.lengthBytes > 0;
            partitionRow.freeBytes = volumeUsage.freeBytes;
            partitionRow.usedBytes = volumeUsage.usedBytes;
            partitionRow.hasChildren = hasVolume;
            partitionRow.expanded = isExpanded(partitionRow.key);
            partitionRow.system = core::isSystemPartition(partition);
            partitionRow.encrypted = hasVolume && partition.volume.encrypted;
            partitionRow.removable = disk->removable;
            partitionRow.unavailable = unavailable;
            partitionRow.lowSpace = lowSpaceOn(volumeUsage.totalBytes, volumeUsage.freeBytes, sizesKnown);
            partitionRow.freeText = sizeOrDash(volumeUsage.freeBytes, sizesKnown);
            partitionRow.usedText = sizeOrDash(volumeUsage.usedBytes, sizesKnown);
            rows.push_back(partitionRow);
            if (!partitionRow.expanded) continue;
            if (!hasVolume) continue;

            TreeRow volumeRow = partitionRow;
            volumeRow.kind = NodeKind::Volume;
            volumeRow.key = volumeKey(disk->number, partition.index);
            volumeRow.level = 2;
            volumeRow.title = volumeTitle(partition.volume);
            volumeRow.hasChildren = false;
            volumeRow.sizeBytes = volumeUsage.totalBytes;
            volumeRow.sizeKnown = volumeUsage.sizesKnown;
            volumeRow.explorerPath = explorerPathOf(partition.volume);
            rows.push_back(volumeRow);
        }
    }
    markSelected();
}

void DisksViewModel::Impl::markSelected() {
    for (MapDiskModel& disk : map) {
        disk.selected = selectedKey == diskKey(disk.diskNumber);
        for (MapSegmentModel& segment : disk.segments) {
            segment.selected = !segment.unallocated && selectedKey == segment.key;
            if (segment.hasVolume) segment.volume.selected = selectedKey == segment.volume.key;
        }
    }
}

DisksViewModel::DisksViewModel() : impl_(std::make_unique<Impl>()) {}

void DisksViewModel::publishInventory(std::shared_ptr<const core::DiskInventory> inventory) {
    impl_->inventory = std::move(inventory);
    if (!impl_->inventory) {
        impl_->rows.clear();
        impl_->map.clear();
        return;
    }
    impl_->rebuild();
}

void DisksViewModel::publishDisks(std::vector<PhysicalDisk> disks) {
    // Детерминированный порядок (core::sortInventory) обязателен и здесь: снимок
    // мог прийти из кэша, который собрал другой вызывающий, а карта и дерево
    // обязаны быть одинаковыми между прогонами (§12: скриншот не дрожит).
    core::sortInventory(disks);
    publishInventory(std::make_shared<const core::DiskInventory>(core::DiskInventory::fromDisks(std::move(disks))));
}

void DisksViewModel::clear() {
    impl_->inventory.reset();
    impl_->rows.clear();
    impl_->map.clear();
    impl_->selectedKey.clear();
}

bool DisksViewModel::hasInventory() const noexcept { return impl_->inventory != nullptr; }

const core::DiskInventory* DisksViewModel::inventory() const noexcept { return impl_->inventory.get(); }

std::size_t DisksViewModel::diskCount() const noexcept {
    return impl_->inventory ? impl_->inventory->disks().size() : 0U;
}

std::size_t DisksViewModel::visibleDiskCount() const noexcept {
    std::size_t count = 0;
    for (const TreeRow& row : impl_->rows) {
        if (row.kind == NodeKind::Disk) ++count;
    }
    return count;
}

std::size_t DisksViewModel::unavailableDiskCount() const noexcept {
    if (!impl_->inventory) return 0;
    std::size_t count = 0;
    for (const PhysicalDisk& disk : impl_->inventory->disks()) {
        if (core::isDiskUnavailable(disk)) ++count;
    }
    return count;
}

void DisksViewModel::setFilter(std::string_view which, bool enabled) {
    DiskFilters filters = impl_->filters;
    if (which == "lettered") {
        filters.letteredOnly = enabled;
    } else if (which == "system") {
        filters.systemOnly = enabled;
    } else if (which == "removable") {
        filters.removableOnly = enabled;
    } else {
        // Неизвестный фильтр игнорируется, а не молча включает первый: иначе
        // опечатка в мосте тихо переключила бы «только с буквами».
        return;
    }
    setFilters(filters);
}

void DisksViewModel::setFilters(DiskFilters filters) {
    if (impl_->filters == filters) return;
    impl_->filters = filters;
    impl_->rebuild();
}

DiskFilters DisksViewModel::filters() const noexcept { return impl_->filters; }

bool DisksViewModel::filterEnabled(std::string_view which) const noexcept {
    if (which == "lettered") return impl_->filters.letteredOnly;
    if (which == "system") return impl_->filters.systemOnly;
    if (which == "removable") return impl_->filters.removableOnly;
    return false;
}

void DisksViewModel::setSort(SortKey key, SortDirection direction) {
    const SortOrder wanted{key, direction};
    if (impl_->sort == wanted) return;
    impl_->sort = wanted;
    impl_->rebuild();
}

void DisksViewModel::toggleSort(SortKey key) {
    const SortOrder current = impl_->sort;
    if (current.key != key) {
        impl_->sort = SortOrder::defaultFor(key);
    } else {
        impl_->sort.direction =
            current.direction == SortDirection::Ascending ? SortDirection::Descending : SortDirection::Ascending;
    }
    impl_->rebuild();
}

SortOrder DisksViewModel::sort() const noexcept { return impl_->sort; }

const std::vector<TreeRow>& DisksViewModel::rows() const noexcept { return impl_->rows; }

std::size_t DisksViewModel::rowCount() const noexcept { return impl_->rows.size(); }

const TreeRow* DisksViewModel::rowFor(std::string_view key) const {
    for (const TreeRow& row : impl_->rows) {
        if (row.key == key) return &row;
    }
    return nullptr;
}

std::optional<std::size_t> DisksViewModel::indexFor(std::string_view key) const {
    for (std::size_t i = 0; i < impl_->rows.size(); ++i) {
        if (impl_->rows[i].key == key) return i;
    }
    return std::nullopt;
}

std::string DisksViewModel::keyAt(std::size_t index) const {
    if (index >= impl_->rows.size()) return std::string();
    return impl_->rows[index].key;
}

void DisksViewModel::setSelectedKey(std::string_view key) {
    const std::string wanted(key);
    if (wanted == impl_->selectedKey) return;
    if (!wanted.empty() && rowFor(wanted) == nullptr) return;  // ключа нет в дереве
    impl_->selectedKey = wanted;
    impl_->markSelected();
}

std::string DisksViewModel::selectedKey() const { return impl_->selectedKey; }

void DisksViewModel::selectFirst() {
    if (impl_->rows.empty()) {
        if (impl_->selectedKey.empty()) return;
        impl_->selectedKey.clear();
        impl_->markSelected();
        return;
    }
    setSelectedKey(impl_->rows.front().key);
}

void DisksViewModel::clearSelection() {
    if (impl_->selectedKey.empty()) return;
    impl_->selectedKey.clear();
    impl_->markSelected();
}

std::string DisksViewModel::parentKey(std::string_view key) const { return parentOf(key); }

bool DisksViewModel::selectParent(std::string_view key) {
    const std::string parent = parentOf(key);
    if (parent.empty()) return false;
    setSelectedKey(parent);
    return impl_->selectedKey == parent;
}

bool DisksViewModel::moveSelection(int delta) {
    if (impl_->rows.empty() || delta == 0) return false;
    std::ptrdiff_t index = -1;
    for (std::size_t i = 0; i < impl_->rows.size(); ++i) {
        if (impl_->rows[i].key == impl_->selectedKey) {
            index = static_cast<std::ptrdiff_t>(i);
            break;
        }
    }
    if (index < 0) {
        // Ничего не выбрано: шаг вниз выбирает начало, шаг вверх — конец. Так ведёт
        // себя дерево и список, и человек не обязан знать, выбрано что-то или
        // нет, чтобы получить предсказуемое движение.
        const std::size_t target = delta > 0 ? 0U : impl_->rows.size() - 1U;
        setSelectedKey(impl_->rows[target].key);
        return true;
    }
    const std::ptrdiff_t wanted = index + delta;
    if (wanted < 0 || wanted >= static_cast<std::ptrdiff_t>(impl_->rows.size())) return false;
    setSelectedKey(impl_->rows[static_cast<std::size_t>(wanted)].key);
    return true;
}

bool DisksViewModel::toggleExpanded(std::string_view key) {
    const std::string wanted(key);
    const TreeRow* row = rowFor(wanted);
    if (row == nullptr || !row->hasChildren) return false;
    impl_->expansion[wanted] = !impl_->isExpanded(wanted);
    impl_->rebuild();
    return true;
}

void DisksViewModel::expandAll() {
    impl_->defaultExpanded = true;
    impl_->expansion.clear();
    impl_->rebuild();
}

void DisksViewModel::collapseAll() {
    impl_->defaultExpanded = false;
    impl_->expansion.clear();
    impl_->rebuild();
}

bool DisksViewModel::handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown) {
    (void)shiftDown;
    if (impl_->rows.empty()) return false;
    switch (virtualKey) {
    case VK_ESCAPE:
        if (impl_->selectedKey.empty()) return false;
        clearSelection();
        return true;
    case VK_LEFT:
        // Ctrl+«влево» — к родителю (из тома в раздел, из раздела в диск). Без
        // Ctrl стрелки принадлежат списку: он сам прокручивает и выделяет, и
        // одно нажатие не должно двигать выделение дважды.
        if (!controlDown) return false;
        return selectParent(impl_->selectedKey);
    case VK_RIGHT:
        if (!controlDown) return false;
        if (impl_->selectedKey.empty()) return false;
        if (rowFor(impl_->selectedKey) == nullptr || impl_->isExpanded(impl_->selectedKey)) return false;
        impl_->expansion[impl_->selectedKey] = true;
        impl_->rebuild();
        return true;
    case VK_RETURN:
        // Enter у узла с детьми разворачивает или сворачивает ветку. У тома Enter
        // доходит до экрана: там он открывает проводник.
        if (impl_->selectedKey.empty()) return false;
        return toggleExpanded(impl_->selectedKey);
    default: break;
    }
    return false;
}

std::string DisksViewModel::statusText() const {
    if (!impl_->inventory) return tr(StringId::kStatusIdle);
    const core::InventoryUsage& usage = impl_->inventory->usage();
    std::string status = tr(StringId::kDisksFree) + ": " + formatBytes(usage.freeBytes) + " (" +
                         formatPercent(usage.freeFraction) + ")";
    status += " · " + tr(StringId::kDisksUsed) + ": " + formatBytes(usage.usedBytes) + " (" +
              formatPercent(usage.usedFraction) + ")";

    std::size_t partitions = 0;
    std::size_t volumes = 0;
    for (const PhysicalDisk& disk : impl_->inventory->disks()) {
        partitions += disk.partitions.size();
        for (const Partition& partition : disk.partitions) {
            if (partition.hasVolume) ++volumes;
        }
    }
    status += " · " + trPlural(StringId::kDisksPartitionCount, static_cast<std::uint64_t>(partitions));
    status += " · " + trPlural(StringId::kDisksVolumeCount, static_cast<std::uint64_t>(volumes));

    const std::size_t unavailable = unavailableDiskCount();
    if (unavailable > 0) {
        status += " · " + tr(StringId::kDisksNotDetected) + " (" + std::to_string(unavailable) + ")";
    }
    if (impl_->filters.any()) {
        std::vector<std::string> active;
        if (impl_->filters.letteredOnly) active.push_back(tr(StringId::kDisksFilterLetters));
        if (impl_->filters.systemOnly) active.push_back(tr(StringId::kDisksFilterSystem));
        if (impl_->filters.removableOnly) active.push_back(tr(StringId::kDisksFilterRemovable));
        status += " · " + joinWith(active, ", ");
    }
    const std::vector<std::string> issues = problems();
    if (!issues.empty()) {
        status += " · " + tr(StringId::kCommonWarning) + " (" + std::to_string(issues.size()) + ")";
    }
    return status;
}

std::string DisksViewModel::cardTitle() const {
    const TreeRow* row = rowFor(impl_->selectedKey);
    if (row == nullptr) return tr(StringId::kDisksTitle);
    return row->title;
}

std::vector<std::string> DisksViewModel::cardLines() const {
    std::vector<std::string> lines;
    const TreeRow* row = rowFor(impl_->selectedKey);
    if (row == nullptr) {
        if (impl_->inventory == nullptr) {
            lines.push_back(tr(StringId::kStatusIdle));
            return lines;
        }
        lines.push_back(tr(StringId::kDisksNotDetected) + ": " + std::to_string(unavailableDiskCount()));
        return lines;
    }
    const PhysicalDisk* disk = impl_->diskByNumber(row->diskNumber);
    if (disk == nullptr) {
        lines.push_back(tr(StringId::kDisksNotDetected));
        return lines;
    }

    if (row->kind == NodeKind::Disk) {
        const core::DiskUsage usage = core::diskUsage(*disk);
        std::vector<std::string> systems;
        for (const Partition& partition : disk->partitions) {
            appendUnique(systems, partition.volume.fileSystem);
        }
        lines.push_back(field(tr(StringId::kDisksModel), disk->model));
        lines.push_back(field(tr(StringId::kDisksSerial), disk->serial));
        lines.push_back(field(tr(StringId::kDisksFirmware), disk->firmware));
        lines.push_back(field(tr(StringId::kDisksBus), std::string(core::busTypeName(disk->bus))));
        lines.push_back(field(tr(StringId::kDisksFileSystem), joinWith(systems, ", ")));
        lines.push_back(field(tr(StringId::kDisksFree), sizeOrDash(usage.freeBytes, usage.freeKnown)));
        lines.push_back(field(tr(StringId::kDisksUsed), sizeOrDash(usage.usedBytes, usage.freeKnown)));
        lines.push_back(field(tr(StringId::kDisksTrim), yesNo(disk->trimSupported)));
        lines.push_back(field(word(Word::Removable), yesNo(disk->removable)));
        lines.push_back(field(word(Word::ReadOnly), yesNo(disk->readOnly)));
        lines.push_back(field(tr(StringId::kDisksEncryption), yesNo(diskHasEncryptedVolume(*disk))));
        lines.push_back(trPlural(StringId::kDisksPartitionCount,
                                 static_cast<std::uint64_t>(disk->partitions.size())) +
                        " · " +
                        trPlural(StringId::kDisksVolumeCount, static_cast<std::uint64_t>(usage.volumeCount)));
        if (core::isDiskUnavailable(*disk)) lines.push_back(tr(StringId::kDisksNotDetected));
        return lines;
    }

    const Partition* partition = core::findPartition(*disk, row->partitionIndex);
    if (partition == nullptr) {
        lines.push_back(tr(StringId::kDisksNotDetected));
        return lines;
    }

    const bool hasVolume = partition->hasVolume;
    const core::VolumeUsage usage = hasVolume ? core::volumeUsage(partition->volume) : core::VolumeUsage{};

    if (row->kind == NodeKind::Partition) {
        std::string kind = core::partitionKindName(partition->kind);
        if (partition->system) kind += " (" + word(Word::System) + ")";
        if (partition->boot) kind += " (" + word(Word::Bootable) + ")";
        if (partition->hidden) kind += " (" + word(Word::Hidden) + ")";
        lines.push_back(field(tr(StringId::kDisksPartitionType), kind));
        if (!partition->gptName.empty()) lines.push_back(field(tr(StringId::kDisksLabel), partition->gptName));
        if (hasVolume) {
            lines.push_back(field(tr(StringId::kDisksFileSystem), partition->volume.fileSystem));
            lines.push_back(field(tr(StringId::kDisksLabel), partition->volume.label));
        }
        lines.push_back(field(tr(StringId::kDisksFree), sizeOrDash(usage.freeBytes, usage.sizesKnown)));
        lines.push_back(field(tr(StringId::kDisksUsed), sizeOrDash(usage.usedBytes, usage.sizesKnown)));
        if (usage.sizesKnown) {
            lines.push_back(field(std::string(tr(StringId::kDisksFree)) + ", %", formatPercent(usage.freeFraction)));
        }
        lines.push_back(field(tr(StringId::kDisksEncryption), hasVolume && partition->volume.encrypted
                                                                    ? tr(StringId::kActionYes)
                                                                    : tr(StringId::kActionNo)));
        lines.push_back(field(word(Word::ReadOnly), yesNo(disk->readOnly || (hasVolume && partition->volume.readOnly))));
        return lines;
    }

    if (!hasVolume) {
        lines.push_back(tr(StringId::kDisksNotDetected));
        return lines;
    }
    const Volume& volume = partition->volume;
    lines.push_back(field(tr(StringId::kDisksLabel), volume.label));
    lines.push_back(field(tr(StringId::kDisksFileSystem), volume.fileSystem));
    lines.push_back(field(tr(StringId::kDisksMountPoint), joinWith(volume.mountPoints, ", ")));
    lines.push_back(field(tr(StringId::kDisksFree), sizeOrDash(usage.freeBytes, usage.sizesKnown)));
    lines.push_back(field(tr(StringId::kDisksUsed), sizeOrDash(usage.usedBytes, usage.sizesKnown)));
    if (usage.sizesKnown) {
        lines.push_back(field(std::string(tr(StringId::kDisksFree)) + ", %", formatPercent(usage.freeFraction)));
    }
    lines.push_back(field(tr(StringId::kDisksEncryption), volume.encrypted ? tr(StringId::kActionYes)
                                                                            : tr(StringId::kActionNo)));
    lines.push_back(field(word(Word::ReadOnly), yesNo(volume.readOnly || disk->readOnly)));
    if (!usage.consistent) lines.push_back(tr(StringId::kCommonWarning));
    return lines;
}

std::string DisksViewModel::explorerPath() const {
    const TreeRow* row = rowFor(impl_->selectedKey);
    if (row == nullptr) return std::string();
    return row->explorerPath;
}

bool DisksViewModel::explorerAvailable() const noexcept {
    const TreeRow* row = rowFor(impl_->selectedKey);
    return row != nullptr && !row->explorerPath.empty();
}

MapExport DisksViewModel::exportBundle() const {
    MapExport out;
    if (!impl_->inventory) return out;
    const std::vector<PhysicalDisk>& disks = impl_->inventory->disks();
    // Опции оставляем теми, что у отчёта по умолчанию: серийники и
    // идентификаторы томов маскируются. Экспорт карты часто идёт в баг-репорт,
    // а полный серийник в чужом баге — это лишнее, чего человек не просил
    // (SPEC §10: приватность по умолчанию).
    out.json = core::partitionMapToJson(disks);
    out.text = core::toText(disks);
    return out;
}

std::vector<std::string> DisksViewModel::problems() const {
    std::vector<std::string> out;
    if (!impl_->inventory) return out;
    for (const core::DiskIssue& issue : impl_->inventory->issues()) {
        out.push_back(std::string(core::toString(issue.severity)) + ": " + issue.message);
    }
    for (const PhysicalDisk& disk : impl_->inventory->disks()) {
        if (core::isDiskUnavailable(disk)) {
            out.push_back(tr(StringId::kDisksNotDetected) + " (" + word(Word::Disk) + " " +
                          std::to_string(disk.number) + ")");
        }
    }
    return out;
}

bool DisksViewModel::hasProblems() const noexcept {
    if (!impl_->inventory) return false;
    if (!impl_->inventory->issues().empty()) return true;
    for (const PhysicalDisk& disk : impl_->inventory->disks()) {
        if (core::isDiskUnavailable(disk)) return true;
    }
    return false;
}

const std::vector<MapDiskModel>& DisksViewModel::mapDisks() const noexcept { return impl_->map; }

int DisksViewModel::mapHeightDip(const MapMetrics& mapMetrics) const noexcept {
    const int needed =
        static_cast<int>(std::lround(static_cast<double>(mapHeightDipFor(mapMetrics, impl_->map.size()))));
    // Запомненная высота из PageState — это нижняя граница, а не заморозка: новый
    // диск обязан увеличить карту, иначе часть разделов просто не попала бы на
    // экран.
    return impl_->mapHeightDipFloor > needed ? impl_->mapHeightDipFloor : needed;
}

void DisksViewModel::applyPageState(const PageState& state) {
    impl_->defaultExpanded = true;
    impl_->expansion.clear();
    for (const std::string& key : state.expandedKeys) {
        if (!key.empty()) impl_->expansion[key] = true;
    }
    impl_->mapHeightDipFloor = state.splitterDip > 0 ? state.splitterDip : 0;
    impl_->selectedKey = state.selectedKey;
    // Ключ из файла настроек мог остаться от прежнего снимка (диск отвалился,
    // том переименовали). Молча подсвечивать нечего — тогда выделение
    // снимается, и это лучше выделения несуществующей строки.
    if (!impl_->selectedKey.empty() && rowFor(impl_->selectedKey) == nullptr) impl_->selectedKey.clear();
    impl_->rebuild();
}

PageState DisksViewModel::pageState() const {
    PageState state;
    state.selectedKey = impl_->selectedKey;
    for (const auto& entry : impl_->expansion) {
        if (entry.second) state.expandedKeys.push_back(entry.first);
    }
    state.splitterDip = mapHeightDip(MapMetrics{});
    return state;
}

// ---------------------------------------------------------------------------
// Окно экрана: SysListView32 + карта Direct2D + карточка деталей
// ---------------------------------------------------------------------------
//
// Почему окно в этом же файле, а не в отдельном view_disks_window.*: владение
// файлами у волн узкое, а экран без модели или модель без окна — половина
// задачи. Разделение выражано типами (модель не знает про HWND), а не
// количеством файлов — как в соседнем экране «Очистка».
namespace detail {

constexpr wchar_t kDisksViewClass[] = L"MrProper.DisksView";
constexpr wchar_t kDisksMapClass[] = L"MrProper.DisksMap";

// «Перерисовать из модели». Номер не совпадает с сообщением экрана «Очистка»
// (WM_APP + 1): окна обоих экранов — дети одного хоста, и совпадающие номера
// означали бы, что отложенная перерисовка одного экрана задевает другой.
constexpr UINT kMsgSyncModel = WM_APP + 21;

// Идентификаторы дочерних окон. Диапазон не пересекается с соседним экраном
// (1…8): WM_DRAWITEM разбирает их по числу, а не по имени.
enum : int {
    kChildMap = 101,
    kChildList = 102,
    kChildStatus = 103,
    kChildCard = 104,
};

// Столбцы списка: узел, «Свободно», «Занято». Порядок важен: он же порядок
// сортировки по клику на заголовок.
inline constexpr int kColumnCount = 3;
inline constexpr int kIndentLevels = 3;

// Подстадии отрисовки подпункта в NM_CUSTOMDRAW списка. В commctrl.h этого SDK
// объявлены только CDDS_PREPAINT/CDDS_ITEMPREPAINT/CDDS_ITEMPOSTPAINT, а
// comctl32 подстадии подпункта шлёт и документирует их (MSDN, Custom Draw
// Controls). Значения взяты оттуда и объявлены здесь явно, с проверкой в журнал:
// если бы константа разошлась с реальностью, список молча потерял бы свои
// числа, а лонг-тест на неизвестной стадии это покажет.
inline constexpr DWORD kSubItemPrePaint = 0x00000100;   // CDDS_SUBITEMPREPAINT
inline constexpr DWORD kSubItemPostPaint = 0x00000200;  // CDDS_SUBITEMPOSTPAINT

LRESULT CALLBACK mapProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

struct ViewState;

ViewState* stateOf(HWND window) {
    return reinterpret_cast<ViewState*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

// Подкласс контрола: исходная процедура и сам контрол. Владеет ими экран.
struct ChildProc {
    HWND hwnd{nullptr};
    WNDPROC prev{nullptr};
};

// Значки состояния строки. Форма обязана отличать их друг от друга, а не только
// цвет: при высокой контрастности три цвета риска вырождаются в цвет текста
// системы, и «зашифровано» с «нет места» обязаны различаться чем-то ещё (§7.2:
// состояние показывается знаком, а не только цветом).
enum class Badge : std::uint8_t { Encrypted, LowSpace, Unavailable };

void paintBadge(HDC dc, const RECT& box, Badge badge, const theme::Palette& palette) {
    theme::Color color = palette.textSecondary;
    switch (badge) {
    case Badge::Encrypted: color = palette.accent; break;
    case Badge::LowSpace: color = palette.warning; break;
    case Badge::Unavailable: color = palette.danger; break;
    }
    const theme::Color edge = theme::ensureContrast(palette.surface, color, 3.0);
    HBRUSH brush = ::CreateSolidBrush(theme::colorRef(color));
    HPEN pen = ::CreatePen(PS_SOLID, 1, theme::colorRef(edge));
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);

    switch (badge) {
    case Badge::Encrypted: {
        // Замок: овальная скоба и корпус. Форма (в отличие от одного цвета)
        // остаётся различимой и при высокой контрастности, где три цвета риска
        // вырождаются в цвет текста системы.
        const int center = (box.left + box.right) / 2;
        const int half = std::max(1, static_cast<int>(box.right - box.left) / 2);
        const int body = box.top + static_cast<int>(box.bottom - box.top) / 3;
        const HGDIOBJ savedBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::Ellipse(dc, center - half, box.top, center + half, body + half);
        ::Rectangle(dc, box.left, body, box.right, box.bottom);
        ::SelectObject(dc, savedBrush);
        break;
    }
    case Badge::LowSpace: {
        POINT triangle[3]{{(box.left + box.right) / 2, box.top}, {box.right, box.bottom}, {box.left, box.bottom}};
        ::Polygon(dc, triangle, 3);
        break;
    }
    case Badge::Unavailable: {
        ::MoveToEx(dc, box.left, box.top, nullptr);
        ::LineTo(dc, box.right, box.bottom);
        ::MoveToEx(dc, box.right, box.top, nullptr);
        ::LineTo(dc, box.left, box.bottom);
        break;
    }
    }
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(brush);
    ::DeleteObject(pen);
}

// Отступы дерева в списке — картинки переменной ширины в списке мелких значков
// (LVSIL_SMALL). Это штатный способ сделать вложенность в SysListView32: контрол
// сам резервирует под картинку ширину, равную её ширине, и рисует подпись правее.
// Вариант «пробелы в тексте» не годится: он не переживает смену шрифта и
// масштаба текста, а именно на них ломается вёрстка при 150 % DPI (§12).
//
// Альфа здесь настоящая (0 или 255), а не цвет-ключ: строка может оказаться
// выделенной, и картинка с ключом цвета вырезала бы в подсветке дыру.
HIMAGELIST buildIndentImages(int indentPx, int rowPx, const theme::Palette& palette) {
    constexpr std::uint32_t kKeyRgb = 0x00FF00FFU;  // метка прозрачности до заливки
    const int indent = std::max(4, indentPx);
    const int row = std::max(6, rowPx);
    // ILC_COLOR32 без ILC_MASK: у списка масок 4 бита на пиксель, и альфа через
    // маску не выражается — это привело бы к тому, что отступ вырезал бы дыру в
    // подсветке выделенной строки.
    HIMAGELIST list = ::ImageList_Create(indent, row, ILC_COLOR32, kIndentLevels, kIndentLevels);
    if (list == nullptr) {
        logWin32("ui.disks.indent", "ImageList_Create", ::GetLastError());
        return nullptr;
    }
    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) {
        logWin32("ui.disks.indent", "GetDC", ::GetLastError());
        ::ImageList_Destroy(list);
        return nullptr;
    }
    for (int level = 0; level < kIndentLevels; ++level) {
        const int width = (level + 1) * indent;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -row;  // сверху вниз: строки идут как в памяти
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC dc = bitmap != nullptr ? ::CreateCompatibleDC(screen) : nullptr;
        if (bitmap == nullptr || dc == nullptr || bits == nullptr) {
            logWin32("ui.disks.indent", "CreateDIBSection", ::GetLastError());
            if (dc != nullptr) ::DeleteDC(dc);
            if (bitmap != nullptr) ::DeleteObject(bitmap);
            break;
        }
        auto* pixels = static_cast<std::uint32_t*>(bits);
        const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(row);
        for (std::size_t i = 0; i < count; ++i) pixels[i] = kKeyRgb;

        const HGDIOBJ old = ::SelectObject(dc, bitmap);
        HBRUSH fill = ::CreateSolidBrush(RGB(255, 0, 255));
        RECT full{0, 0, width, row};
        ::FillRect(dc, &full, fill);
        ::DeleteObject(fill);
        HPEN pen = ::CreatePen(PS_SOLID, 1, theme::colorRef(palette.textSecondary));
        const HGDIOBJ oldPen = ::SelectObject(dc, pen);
        const int cell = width - indent;  // последняя ячейка отступа — под значок
        if (level == 0) {
            // Диск: рамка в последней ячейке отступа.
            const int side = std::max(5, row / 2);
            const int x = cell + (indent - side) / 2;
            const int y = (row - side) / 2;
            const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
            ::Rectangle(dc, x, y, x + side, y + side);
            ::SelectObject(dc, oldBrush);
        } else {
            // Ветка: короткая черта в конце отступа.
            ::MoveToEx(dc, cell + 2, row / 2, nullptr);
            ::LineTo(dc, width - 1, row / 2);
        }
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);
        ::SelectObject(dc, old);
        ::GdiFlush();

        // Альфа после рисования: меткаkey → 0, всё нарисованное → 255. GDI альфу
        // не трогает и не гарантирует её значение, поэтому решаем по цвету.
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t rgb = pixels[i] & 0x00FFFFFFU;
            pixels[i] = (rgb == kKeyRgb) ? 0x00000000U : (0xFF000000U | rgb);
        }
        (void)::ImageList_Add(list, bitmap, nullptr);
        ::DeleteObject(bitmap);
        ::DeleteDC(dc);
    }
    ::ReleaseDC(nullptr, screen);
    return list;
}

// Кисти и шрифты карты. Живут вместе с окном и пересоздаются на смену темы:
// цвет из палитры — значение, а не ресурс, поэтому в кадре ничего создавать не
// надо (то же правило, что у соседнего экрана с чекбоксами).
struct MapResources {
    render::BrushId track{render::kInvalidBrush};
    render::BrushId segment{render::kInvalidBrush};
    render::BrushId segmentSystem{render::kInvalidBrush};
    render::BrushId unallocated{render::kInvalidBrush};
    render::BrushId used{render::kInvalidBrush};
    render::BrushId lowSpace{render::kInvalidBrush};
    render::BrushId border{render::kInvalidBrush};
    render::BrushId text{render::kInvalidBrush};
    render::BrushId textOnSegment{render::kInvalidBrush};
    render::BrushId subtitle{render::kInvalidBrush};
    render::BrushId selection{render::kInvalidBrush};
    render::FontId label{render::kInvalidFont};
    render::FontId caption{render::kInvalidFont};

    [[nodiscard]] bool complete() const noexcept {
        return track != render::kInvalidBrush && segment != render::kInvalidBrush && used != render::kInvalidBrush &&
               text != render::kInvalidBrush && label != render::kInvalidFont &&
               caption != render::kInvalidFont;
    }

    // Освобождение требует рендерера-владельца, поэтому ресурс передаётся явно:
    // иначе пришлось бы хранить в структуре ещё и владельца, а она и так живёт
    // внутри окна, у которого рендерер и так под рукой.
    void releaseWith(render::Renderer& owner) {
        render::BrushId* brushes[] = {&track,   &segment, &segmentSystem, &unallocated, &used,
                                      &lowSpace, &border,  &text,          &textOnSegment,
                                      &subtitle, &selection};
        for (render::BrushId* brush : brushes) {
            if (*brush != render::kInvalidBrush) owner.releaseBrush(*brush);
            *brush = render::kInvalidBrush;
        }
        if (label != render::kInvalidFont) owner.releaseFont(label);
        if (caption != render::kInvalidFont) owner.releaseFont(caption);
        label = render::kInvalidFont;
        caption = render::kInvalidFont;
    }
};

struct ViewState {
    DisksScreen::Callbacks callbacks;
    DisksViewModel model;
    theme::Theme theme;
    DisksMetrics metrics{};
    MapMetrics mapMetrics{};
    int dpi{kDefaultDpi};

    HWND window{nullptr};
    HWND map{nullptr};
    HWND list{nullptr};
    HWND status{nullptr};
    HWND card{nullptr};
    std::array<HWND, kDisksFilterCount> filterButtons{};
    std::array<HWND, kDisksActionCount> actionButtons{};

    // Шрифты: список (полужирный — дерево заметнее подписей), строка состояния и
    // строки карточки (обычный), заголовок карточки (крупный).
    std::array<HFONT, 4> fonts{};
    HIMAGELIST indentImages{nullptr};
    HBRUSH surfaceBrush{nullptr};
    std::vector<std::string> rowKeys;
    std::vector<ChildProc> children;
    std::shared_ptr<mv::ScreenEndpoint> feed;
    render::Renderer renderer;
    MapResources mapResources;
    bool syncing{false};
    bool controlsReady{false};
    bool columnsReady{false};
    bool rendererFailed{false};
    bool drawStageLogged{false};

    ~ViewState() {
        mapResources.releaseWith(renderer);
        for (HFONT& font : fonts) {
            if (font != nullptr) ::DeleteObject(font);
        }
        if (indentImages != nullptr) ::ImageList_Destroy(indentImages);
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
    }

    // --- Текст в контролы ---------------------------------------------------

    void setChildText(HWND child, std::string_view text) {
        if (child == nullptr) return;
        const std::wstring wide = toWide(text);
        ::SetWindowTextW(child, wide.c_str());
    }

    bool setSubitemText(HWND control, int item, int subitem, std::string_view text) {
        if (control == nullptr) return false;
        const std::wstring wide = toWide(text);
        LVITEMW entry{};
        entry.iItem = item;
        entry.iSubItem = subitem;
        entry.pszText = const_cast<wchar_t*>(wide.c_str());
        return ::SendMessageW(control, LVM_SETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&entry)) != FALSE;
    }

    void setColumnText(int column, std::string_view text) {
        if (list == nullptr) return;
        const std::wstring wide = toWide(text);
        LVCOLUMNW info{};
        info.mask = LVCF_TEXT;
        info.pszText = const_cast<wchar_t*>(wide.c_str());
        ::SendMessageW(list, LVM_SETCOLUMNW, static_cast<WPARAM>(column), reinterpret_cast<LPARAM>(&info));
    }

    // --- Дерево в списке ----------------------------------------------------

    [[nodiscard]] const TreeRow* rowAt(int index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= rowKeys.size()) return nullptr;
        return model.rowFor(rowKeys[static_cast<std::size_t>(index)]);
    }

    [[nodiscard]] int indexOfKey(const std::string& key) const {
        const std::optional<std::size_t> found = model.indexFor(key);
        return found.has_value() ? static_cast<int>(*found) : -1;
    }

    void syncColumns() {
        if (list == nullptr) return;
        RECT client{};
        if (::GetClientRect(list, &client) == FALSE) return;
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const int numeric = std::max(40, scale.dip(metrics.numericColumnDip));
        const int badges = std::max(16, scale.dip(28.0));
        if (!columnsReady) {
            // Столбцы создаются один раз: LVM_INSERTITEM без столбца не вставит
            // строку, а создавать их заново на каждый щелчок — лишняя работа
            // comctl32 при каждой перерисовке.
            for (int i = 0; i < kColumnCount; ++i) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                column.fmt = LVCFMT_LEFT;
                column.cx = i == 0 ? 200 : numeric;
                column.pszText = const_cast<wchar_t*>(L"");
                if (ListView_InsertColumn(list, i, &column) == -1) {
                    logWin32("ui.disks.list", "ListView_InsertColumn", ::GetLastError());
                    return;
                }
            }
            columnsReady = true;
        }
        ListView_SetColumnWidth(list, 0, std::max(60, static_cast<int>(client.right) - 2 * numeric - badges - 8));
        ListView_SetColumnWidth(list, 1, numeric);
        ListView_SetColumnWidth(list, 2, numeric);
    }

    void syncHeaders() {
        if (list == nullptr || !columnsReady) return;
        const SortOrder order = model.sort();
        const std::array<SortKey, kColumnCount> keys{
            SortKey::Number, SortKey::FreeSpace, SortKey::Size};
        for (int i = 0; i < kColumnCount; ++i) {
            std::string text = i == 0 ? tr(StringId::kDisksTitle)
                                      : (i == 1 ? tr(StringId::kDisksFree) : tr(StringId::kDisksUsed));
            if (order.key == keys[static_cast<std::size_t>(i)]) {
                // Стрелка сортировки — знак, а не слово: она одинакова в обоих
                // языках и не требует ключа в каталоге строк.
                text += order.direction == SortDirection::Ascending ? " \xE2\x96\xB2" : " \xE2\x96\xBC";
            }
            setColumnText(i, text);
        }
    }

    [[nodiscard]] SortKey sortKeyOfColumn(int column) const noexcept {
        switch (column) {
        case 0: return SortKey::Number;
        case 1: return SortKey::FreeSpace;
        case 2: return SortKey::Size;
        default: break;
        }
        return SortKey::Number;
    }

    void syncList() {
        if (list == nullptr) return;
        // Положение прокрутки переживает перестроение: без этого щелчок по
        // карточке внизу списка прыгал бы дерево наверх.
        std::string firstVisible;
        if (const int top = ListView_GetTopIndex(list); top >= 0) {
            if (top < static_cast<int>(rowKeys.size())) firstVisible = rowKeys[static_cast<std::size_t>(top)];
        }

        syncing = true;
        ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        rowKeys.clear();

        const std::vector<TreeRow>& rows = model.rows();
        rowKeys.reserve(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const TreeRow& row = rows[i];
            const std::wstring title = toWide(row.title);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
            item.iItem = static_cast<int>(i);
            // Картинка-отступ: ширина изображения и есть отступ уровня. Уровень
            // — из данных строки, а не из порядка вставки: сортировка меняет
            // порядок, вложенность — нет.
            item.iImage = std::clamp(row.level, 0, kIndentLevels - 1);
            item.lParam = static_cast<LPARAM>(i);
            item.pszText = const_cast<wchar_t*>(title.c_str());
            if (ListView_InsertItem(list, &item) == -1) {
                logWin32("ui.disks.list", "ListView_InsertItem", ::GetLastError());
                break;
            }
            rowKeys.push_back(row.key);
            (void)setSubitemText(list, static_cast<int>(i), 1, row.freeText);
            (void)setSubitemText(list, static_cast<int>(i), 2, row.usedText);
        }
        ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        syncing = false;

        if (!firstVisible.empty()) {
            const int index = indexOfKey(firstVisible);
            // Возвращаем строку в видимую часть списка: точный пиксель прокрутки
            // comctl32 после перестроения не хранит, а «прыжок наверх» при
            // перечитке инвентаризации читался бы как потерянное место.
            if (index >= 0) ListView_EnsureVisible(list, index, FALSE);
        }
        syncListSelection();
    }

    void syncListSelection() {
        if (list == nullptr) return;
        syncing = true;
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        const int index = indexOfKey(model.selectedKey());
        if (index >= 0) {
            ListView_SetItemState(list, static_cast<WPARAM>(index), LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, index, FALSE);
        }
        syncing = false;
    }

    // --- Кнопки и подписи ----------------------------------------------------

    void setButton(HWND button, std::string_view text, bool enabled) {
        if (button == nullptr) return;
        setChildText(button, text);
        if ((::IsWindowEnabled(button) != 0) == enabled) return;
        ::EnableWindow(button, enabled ? TRUE : FALSE);
    }

    void setCheck(HWND button, bool checked) {
        if (button == nullptr) return;
        const LRESULT state = ::SendMessageW(button, BM_GETCHECK, 0, 0);
        const LRESULT wanted = checked ? BST_CHECKED : BST_UNCHECKED;
        if (state == wanted) return;
        ::SendMessageW(button, BM_SETCHECK, static_cast<WPARAM>(wanted), 0);
    }

    void syncButtons() {
        setCheck(filterButtons[0], model.filterEnabled("lettered"));
        setCheck(filterButtons[1], model.filterEnabled("system"));
        setCheck(filterButtons[2], model.filterEnabled("removable"));
        setChildText(filterButtons[0], tr(StringId::kDisksFilterLetters));
        setChildText(filterButtons[1], tr(StringId::kDisksFilterSystem));
        setChildText(filterButtons[2], tr(StringId::kDisksFilterRemovable));

        setButton(actionButtons[0], tr(StringId::kDisksExportMap), model.hasInventory());
        setButton(actionButtons[1], tr(StringId::kReportExportText), model.hasInventory());
        setButton(actionButtons[2], tr(StringId::kActionOpenInExplorer), model.explorerAvailable());
        setButton(actionButtons[3], tr(StringId::kActionRefresh), true);
        // «Проверить» — кнопка v1.1 (FR-2). Она создаётся и подписана, но
        // выключена: обещание проверки, которой ещё нет, хуже её отсутствия.
        setButton(actionButtons[4], tr(StringId::kActionCheck), false);
    }

    void syncTexts() {
        setChildText(status, model.statusText());
        setChildText(list, tr(StringId::kDisksTitle));  // доступное имя списка для UIA
    }

    // --- Раскладка -----------------------------------------------------------

    // Будет ли карта пустой. Считается по МОДЕЛИ, а не по раскладке: от
    // раскладки здесь нечего взять, она сама спрашивает модель, и обращение
    // было бы рекурсией. Диск без размерных сегментов (не ответил, нет прав)
    // карты не даёт — рисовать для него нечего.
    [[nodiscard]] bool mapWillBeEmpty() const {
        for (const MapDiskModel& disk : model.mapDisks()) {
            if (disk.unavailable) continue;
            for (const MapSegmentModel& segment : disk.segments) {
                if (!segment.unavailable && segment.lengthBytes > 0) return false;
            }
        }
        return true;
    }

    [[nodiscard]] DisksLayout currentLayout() const {
        RECT client{};
        if (window == nullptr || ::GetClientRect(window, &client) == FALSE) return DisksLayout{};
        // Подписи кнопок живут до конца выражения, а string_view смотрит в них —
        // поэтому строки здесь именованные, а не временные в списке инициализации.
        const std::string filterLabels[kDisksFilterCount] = {
            tr(StringId::kDisksFilterLetters), tr(StringId::kDisksFilterSystem),
            tr(StringId::kDisksFilterRemovable)};
        const std::string actionLabels[kDisksActionCount] = {
            tr(StringId::kDisksExportMap),   tr(StringId::kReportExportText),
            tr(StringId::kActionOpenInExplorer), tr(StringId::kActionRefresh),
            tr(StringId::kActionCheck)};
        const std::vector<int> filters = measureButtons(filterButtons, filterLabels);
        const std::vector<int> actions = measureButtons(actionButtons, actionLabels);
        // Карте без полос нужна высота состояния, а не высота полосы: заголовок,
        // причина и действие занимают около 72 DIP, а полоса недоступного диска —
        // вдвое меньше. Без этой надбавки подпись обрезается по нижней кромке и
        // экран снова выглядит пустым (проверено снимком окна).
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        int mapHeightPx = static_cast<int>(model.mapHeightDip(mapMetrics));
        if (mapWillBeEmpty()) mapHeightPx += scale.dip(72.0);
        return DisksLayout::compute(metrics, dpi, static_cast<int>(client.right), static_cast<int>(client.bottom),
                                    mapHeightPx, static_cast<int>(model.cardLines().size()),
                                    filters, actions);
    }

    // Ширина кнопки по её подписи: раскладка не знает шрифтов, а фиксированная
    // ширина означала бы либо обрезанную подпись (§12), либо пустое место.
    template <std::size_t N>
    [[nodiscard]] std::vector<int> measureButtons(const std::array<HWND, N>& buttons,
                                                  const std::string (&labels)[N]) const {
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const int padding = std::max(4, scale.dip(metrics.buttonPaddingDip));
        std::vector<int> widths;
        widths.reserve(N);
        HDC dc = window != nullptr ? ::GetDC(window) : nullptr;
        for (std::size_t i = 0; i < N; ++i) {
            int width = 40;
            const std::wstring wide = toWide(labels[i]);
            if (dc != nullptr && i < buttons.size() && buttons[i] != nullptr) {
                if (fonts[0] != nullptr) ::SelectObject(dc, fonts[0]);
                SIZE measured{};
                ::GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &measured);
                width = measured.cx + padding;
            }
            widths.push_back(std::max(1, width));
        }
        if (dc != nullptr) ::ReleaseDC(window, dc);
        return widths;
    }

    void place(HWND child, const DisksRect& rect, bool visible) {
        if (child == nullptr) return;
        if (!visible || rect.empty()) {
            ::ShowWindow(child, SW_HIDE);
            return;
        }
        ::SetWindowPos(child, nullptr, rect.x, rect.y, rect.width, rect.height, SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(child, SW_SHOW);
    }

    void layout() {
        const DisksLayout layout = currentLayout();
        place(map, layout.mapRect(), true);
        place(list, layout.listRect(), true);
        place(status, layout.statusRect(), layout.statusVisible());
        place(card, layout.cardRect(), layout.cardVisible());
        for (std::size_t i = 0; i < filterButtons.size(); ++i) {
            place(filterButtons[i], layout.filterButtonRect(static_cast<int>(i)),
                  i < static_cast<std::size_t>(layout.filterButtonCount()));
        }
        for (std::size_t i = 0; i < actionButtons.size(); ++i) {
            place(actionButtons[i], layout.actionButtonRect(static_cast<int>(i)),
                  i < static_cast<std::size_t>(layout.actionButtonCount()));
        }
    }

    // --- Тема ----------------------------------------------------------------

    void applyPalette() {
        const theme::Palette& palette = theme.palette();
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
        surfaceBrush = ::CreateSolidBrush(theme::colorRef(palette.surface));
        if (list != nullptr) {
            ListView_SetBkColor(list, theme::colorRef(palette.surface));
            ListView_SetTextColor(list, theme::colorRef(palette.textPrimary));
            // Подтема «тёмного» у нативных контролов нет в документированном API
            // (ADR-003); модуль темы делает это через безопасные вызовы uxtheme, а
            // отказ — не повод оставлять список белым.
            (void)theme::enableDarkModeForWindow(list, theme.scheme());
        }
        if (indentImages != nullptr) ::ImageList_Destroy(indentImages);
        // Высота строки оценивается в DIP, а не измеряется по шрифту: список
        // подстраивает высоту строки сам, а оценка нужна только для ширины
        // отступа, и на пару пикселей ошибки она не влияет.
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        indentImages = buildIndentImages(std::max(6, scale.dip(16.0)), std::max(12, scale.dip(18.0)), palette);
        if (indentImages != nullptr && list != nullptr) {
            ListView_SetImageList(list, indentImages, LVSIL_SMALL);
        }
        applyMapResources();
    }

    void applyFonts() {
        // Сначала создаём новые шрифты, потом перевешиваем их на контролы и
        // только потом удаляем старые: удалить HFONT, висящий на контроле, —
        // значит оставить контрол со шрифтом в никуда.
        const std::array<theme::FontRole, 4> roles{theme::FontRole::BodyStrong, theme::FontRole::Caption,
                                                  theme::FontRole::Caption, theme::FontRole::Subtitle};
        std::array<HFONT, 4> next{};
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const LOGFONTW description = theme.font(roles[i]).toLogFont(static_cast<unsigned>(dpi));
            next[i] = ::CreateFontIndirectW(&description);
        }
        if (list != nullptr) ::SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(next[0]), TRUE);
        if (status != nullptr) ::SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        if (card != nullptr) {
            ::SendMessageW(card, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        }
        for (const HWND button : filterButtons) {
            if (button != nullptr) ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        }
        for (const HWND button : actionButtons) {
            if (button != nullptr) ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        }
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (fonts[i] != nullptr) ::DeleteObject(fonts[i]);
            fonts[i] = next[i];
        }
    }

    // --- Карта: Direct2D -----------------------------------------------------

    void applyMapResources() {
        mapResources.releaseWith(renderer);
        if (!renderer.ready()) return;  // создавать нечего: ресурсы живут в рендерере
        const theme::Palette& palette = theme.palette();
        // Полоса неразмеченного места — фон дорожки, занятого тома — акцент,
        // системный раздел — плотный акцент. Плотность вместо новых цветов:
        // палитра не расползается, а различие видно и при чёрно-белой печати
        // (плотность и толщина).
        mapResources.track = renderer.createSolidBrush(toRender(palette.surfaceAlt));
        mapResources.segment = renderer.createSolidBrush(withAlpha(palette.accent, 0.55F));
        mapResources.segmentSystem = renderer.createSolidBrush(toRender(palette.accent));
        mapResources.unallocated = renderer.createSolidBrush(toRender(palette.surfaceAlt));
        mapResources.used = renderer.createSolidBrush(toRender(palette.accent));
        mapResources.lowSpace = renderer.createSolidBrush(toRender(palette.danger));
        mapResources.border = renderer.createSolidBrush(toRender(palette.border));
        mapResources.text = renderer.createSolidBrush(toRender(palette.textPrimary));
        mapResources.textOnSegment =
            renderer.createSolidBrush(toRender(theme::ensureContrast(palette.textPrimary, palette.accent, 4.5)));
        mapResources.subtitle = renderer.createSolidBrush(toRender(palette.textSecondary));
        mapResources.selection = renderer.createSolidBrush(toRender(palette.accentHover));
        mapResources.label = renderer.createFont(toFontSpec(theme.font(theme::FontRole::BodyStrong)));
        mapResources.caption = renderer.createFont(toFontSpec(theme.font(theme::FontRole::Caption)));
        if (!mapResources.complete()) {
            logEvent(core::LogLevel::Warn, "ui.disks.map.resources", "Direct2D resources incomplete");
        }
    }

    static render::Color toRender(const theme::Color& color) noexcept {
        return render::Color{static_cast<float>(color.r) / 255.0F, static_cast<float>(color.g) / 255.0F,
                             static_cast<float>(color.b) / 255.0F,
                             static_cast<float>(color.a) / 255.0F};
    }

    // Полупрозрачный цвет приходит в блендер уже premultiplied (D2D1_ALPHA_MODE_
    // PREMULTIPLIED), поэтому каналы домножаются здесь, а не в рендерере.
    static render::Color withAlpha(const theme::Color& color, float alpha) noexcept {
        const render::Color base = toRender(color);
        return render::Color{base.red * alpha, base.green * alpha, base.blue * alpha, alpha};
    }

    render::FontSpec toFontSpec(const theme::FontDesc& description) const {
        render::FontSpec spec;
        spec.family = toUtf8(description.family);
        // Локаль важна не для перевода (текст уже переведён), а для правил
        // подстановки и ширины цифр: без неё один и тот же экран на D2D и GDI
        // рисуется по-разному.
        spec.localeName = toUtf8(localeName());
        spec.sizeDip = static_cast<float>(description.sizeDip);
        spec.weight = description.weight;
        spec.italic = description.italic;
        return spec;
    }

    [[nodiscard]] MapLayout mapLayoutNow() const {
        RECT client{};
        if (map == nullptr || ::GetClientRect(map, &client) == FALSE) return MapLayout{};
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const float width = static_cast<float>(scale.undo(client.right));
        const float height = static_cast<float>(scale.undo(client.bottom));
        return computeMap(mapMetrics, model.mapDisks(), width, height);
    }

    // ------------------------------------------------------------------------
    // Состояние без данных
    // ------------------------------------------------------------------------
    //
    // Три состояния, и склеивать их в одно нельзя: человек должен понимать,
    // что делать дальше, а «пустая карта» не говорит ни слова ни о том, что
    // данные ещё идут, ни о том, что их не удалось получить.
    //
    //   1) инвентаризации ещё нет  — обход идёт (2 с на устройство, §4 FR-1),
    //      ждать нужно секунды, а не нажимать «Обновить»;
    //   2) снимок есть, но данных о дисках нет — не хватило прав: именно это
    //      состояние и требовалось показать словами, а не пустым прямоугольником;
    //   3) диски есть, но отфильтрованы все — фильтры, а не отказ.
    struct MapEmptyState {
        std::string headline;
        std::string reason;
        std::string action;
    };

    // Есть ли на карте что рисовать. Не «есть ли диски», а «есть ли хоть один
    // размерный сегмент на доступном диске»: диск, который не ответил, тоже
    // даёт полосу MapBar, но сегментов в ней нет — рисовать там нечего, и
    // вместо этого экран обязан сказать словами, что устройства не прочитаны.
    // Проверено снимком: с одним недоступным диском карта оставалась пустой.
    [[nodiscard]] bool mapHasContent(const MapLayout& layout) const {
        for (const MapBar& bar : layout.bars) {
            if (bar.unavailable) continue;
            for (const MapSegment& segment : bar.segments) {
                if (segment.width > 0.0F) return true;
            }
        }
        return false;
    }

    [[nodiscard]] MapEmptyState mapEmptyState() const {
        MapEmptyState state;
        const bool filtered = model.filters().any();
        if (filtered && model.hasInventory() && model.diskCount() > 0) {
            // Данные есть, показывать нечего только из-за фильтров.
            state.headline = tr(StringId::kDisksEmptyFiltersTitle);
            state.reason = tr(StringId::kDisksEmptyFiltersReason);
            state.action = tr(StringId::kDisksEmptyFiltersHint);
            return state;
        }
        if (model.hasInventory()) {
            state.headline = tr(StringId::kDisksEmptyUnreadTitle);
            state.reason = tr(StringId::kDisksEmptyUnreadReason);
            state.action = tr(StringId::kDisksEmptyUnreadAction) + tr(StringId::kActionRefresh) +
                          std::string("»");
            return state;
        }
        state.headline = tr(StringId::kDisksEmptyLoadingTitle);
        state.reason = tr(StringId::kDisksEmptyLoadingReason);
        state.action = tr(StringId::kDisksEmptyLoadingAction);
        return state;
    }

    // Пустая карта на Direct2D: рамка «здесь будет карта», заголовок, причина и
    // что делать. Три строки текста — это ~2000 пикселей «чернил» даже на самом
    // маленьком окне, то есть экран перестаёт быть «пустым» даже без данных.
    void paintMapEmptyWithRenderer(const MapResources& res) {
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        RECT client{};
        if (::GetClientRect(map, &client) == FALSE) return;
        const MapEmptyState state = mapEmptyState();

        const float width = static_cast<float>(client.right);
        const float height = static_cast<float>(client.bottom);
        const float pad = static_cast<float>(scale.dip(16.0));
        const float headlineHeight = static_cast<float>(scale.dip(20.0));
        const float lineHeight = static_cast<float>(scale.dip(16.0));

        // Плашка с пунктирной рамкой: пустое место должно выглядеть как «место под
        // карту», а не как обрыв интерфейса.
        const render::Rect frame{pad, pad, std::max(pad + 1.0F, width - pad), std::max(pad + 1.0F, height - pad)};
        renderer.strokeRect(frame, res.border, 1.0F);
        renderer.strokeLine(render::Point{frame.left, frame.top}, render::Point{frame.left + 24.0F, frame.top},
                            res.border, 1.0F);

        const float textLeft = frame.left + static_cast<float>(scale.dip(12.0));
        const float textWidth = std::max(1.0F, frame.right - textLeft - static_cast<float>(scale.dip(12.0)));
        float y = frame.top + static_cast<float>(scale.dip(10.0));

        render::TextOptions options;
        options.vertical = render::TextVerticalAlign::Center;
        options.ellipsis = true;
        const render::Rect headlineArea{textLeft, y, textLeft + textWidth, y + headlineHeight};
        renderer.drawText(state.headline, headlineArea, res.label, res.text, options);
        y += headlineHeight + static_cast<float>(scale.dip(4.0));

        for (const std::string* line : {&state.reason, &state.action}) {
            const render::Size measured = renderer.measureText(res.caption, *line, textWidth);
            const float height2 = std::max(lineHeight, measured.height + 2.0F);
            const render::Rect lineArea{textLeft, y, textLeft + textWidth, y + height2};
            render::TextOptions wrapped = options;
            wrapped.wordWrap = true;
            renderer.drawText(*line, lineArea, res.caption, res.subtitle, wrapped);
            y += height2 + static_cast<float>(scale.dip(2.0));
            if (y > frame.bottom) break;
        }
    }

    // Рендерер создаётся лениво: в WM_NCCREATE клиентская область ещё нулевая, а
    // create() на нулевом размере честно отказывается (§7 renderer.hpp). Попытка
    // повторяется на каждом WM_SIZE, то есть как только окно получило размер, и
    // логируется один раз — иначе неудачный D3D11 забил бы журнал на каждый кадр.
    void ensureRenderer() {
        if (renderer.ready() || rendererFailed) return;
        if (map == nullptr) return;
        RECT client{};
        if (::GetClientRect(map, &client) == FALSE) return;
        if (client.right <= 0 || client.bottom <= 0) return;
        if (renderer.create(map)) {
            applyMapResources();
            return;
        }
        rendererFailed = true;
        logEvent(core::LogLevel::Warn, "ui.disks.map.create", renderer.lastError());
    }

    void paintMapWithRenderer() {
        const MapResources& res = mapResources;
        const MapLayout layout = mapLayoutNow();
        // Нет ни одного диска — рисуем не пустоту, а состояние с причиной.
        // Иначе окно «Дисков» было бы единственным экраном волны, который
        // выглядит как программа сломанной (см. также paintMapEmptyWithGdi).
        if (layout.bars.empty() || !mapHasContent(layout)) {
            paintMapEmptyWithRenderer(res);
            return;
        }
        const float minLabel = static_cast<float>(mapMetrics.minLabelSegmentDip);
        for (const MapBar& bar : layout.bars) {
            render::TextOptions options;
            options.vertical = render::TextVerticalAlign::Center;
            options.ellipsis = true;

            // Подпись диска слева, характеристика устройства справа — тем же
            // приёмом, каким устройство показывает модель и шину.
            if (bar.labelHeight > 0.0F) {
                const render::Rect labelArea{bar.left, bar.labelTop, bar.left + bar.width * 0.65F,
                                             bar.labelTop + bar.labelHeight};
                renderer.drawText(bar.label, labelArea, res.label, res.text, options);
                const render::Rect subArea{bar.left + bar.width * 0.65F, bar.labelTop, bar.left + bar.width,
                                           bar.labelTop + bar.labelHeight};
                options.horizontal = render::TextAlign::Trailing;
                renderer.drawText(bar.subtitle, subArea, res.caption, res.subtitle, options);
                options.horizontal = render::TextAlign::Leading;
            }

            const render::Rect track{bar.left, bar.top, bar.left + bar.width, bar.top + bar.height};
            renderer.fillRect(track, res.track);
            for (const MapSegment& segment : bar.segments) {
                const render::Rect box{segment.left, segment.top, segment.left + segment.width,
                                       segment.top + segment.height};
                if (box.isEmpty()) continue;
                render::BrushId brush = segment.unallocated
                                            ? res.unallocated
                                            : (segment.system ? res.segmentSystem : res.segment);
                if (segment.unavailable) brush = res.border;
                renderer.fillRect(box, brush);
                // Полоса занятости тома — вложенная полоса внутри сегмента: это и
                // есть требуемая FR-2 вложенность «диск → разделы → том».
                for (const MapVolumeBar& volume : bar.volumes) {
                    if (volume.parent != segment.key) continue;
                    const float usedRight = volume.left + volume.width * static_cast<float>(volume.usedFraction);
                    renderer.fillRect(render::Rect{volume.left, volume.top, usedRight, volume.top + volume.height},
                                      volume.lowSpace ? res.lowSpace : res.used);
                    renderer.fillRect(
                        render::Rect{usedRight, volume.top, volume.left + volume.width, volume.top + volume.height},
                        res.unallocated);
                    break;
                }
                if (segment.width >= minLabel && !segment.unallocated) {
                    const render::Rect labelArea{box.left + 2.0F, box.top, box.right - 2.0F, box.bottom};
                    options.vertical = render::TextVerticalAlign::Center;
                    renderer.drawText(segment.label, labelArea, res.caption, res.textOnSegment, options);
                }
                if (segment.selected) renderer.strokeRect(box, res.selection, 1.5F);
            }
            if (bar.selected) renderer.strokeRect(track, res.selection, 1.0F);

            if (bar.captionHeight > 0.0F) {
                const render::Rect captionArea{bar.left, bar.captionTop, bar.left + bar.width,
                                               bar.captionTop + bar.captionHeight};
                renderer.drawText(bar.caption, captionArea, res.caption, res.subtitle, options);
            }
        }
    }

    // Последний рубеж: рендерер не создался вовсе (нет D3D11, нет GDI-устройства
    // контекста). Рисуем ту же карту средствами GDI — грубее, но карта обязана
    // быть, а не пустое окно (§5: ни один отказ не роняет приложение, и утилита,
    // которая обещает объяснять каждое действие, не может молчать).
    void paintMapFallback(HDC dc) {
        const theme::Palette& palette = theme.palette();
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const MapLayout layout = mapLayoutNow();
        RECT client{};
        ::GetClientRect(map, &client);
        HBRUSH surface = ::CreateSolidBrush(theme::colorRef(palette.surface));
        ::FillRect(dc, &client, surface);
        ::DeleteObject(surface);
        if (layout.bars.empty() || !mapHasContent(layout)) {
            paintMapEmptyWithGdi(dc, scale);
            return;
        }

        HBRUSH track = ::CreateSolidBrush(theme::colorRef(palette.surfaceAlt));
        HBRUSH segment = ::CreateSolidBrush(theme::colorRef(palette.accent));
        HBRUSH used = ::CreateSolidBrush(theme::colorRef(palette.accent));
        HBRUSH danger = ::CreateSolidBrush(theme::colorRef(palette.danger));
        const auto px = [&scale](float dip) {
            return static_cast<LONG>(std::lround(static_cast<double>(dip) * scale.scale));
        };
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
        for (const MapBar& bar : layout.bars) {
            if (fonts[0] != nullptr) ::SelectObject(dc, fonts[0]);
            RECT label{px(bar.left), px(bar.labelTop), px(bar.left + bar.width), px(bar.labelTop + bar.labelHeight)};
            const std::wstring wide = toWide(bar.label);
            ::DrawTextW(dc, wide.c_str(), -1, &label,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            RECT trackRect{px(bar.left), px(bar.top), px(bar.left + bar.width), px(bar.top + bar.height)};
            ::FillRect(dc, &trackRect, track);
            for (const MapSegment& item : bar.segments) {
                RECT box{px(item.left), px(item.top), px(item.left + item.width), px(item.top + item.height)};
                if (box.right <= box.left || box.bottom <= box.top) continue;
                ::FillRect(dc, &box, item.unallocated ? track : segment);
            }
            for (const MapVolumeBar& volume : bar.volumes) {
                RECT box{px(volume.left), px(volume.top),
                         px(volume.left + volume.width * static_cast<float>(volume.usedFraction)),
                         px(volume.top + volume.height)};
                ::FillRect(dc, &box, volume.lowSpace ? danger : used);
            }
            if (bar.captionHeight > 0.0F && fonts[1] != nullptr) {
                ::SelectObject(dc, fonts[1]);
                RECT caption{px(bar.left), px(bar.captionTop), px(bar.left + bar.width),
                             px(bar.captionTop + bar.captionHeight)};
                const std::wstring captionText = toWide(bar.caption);
                ::DrawTextW(dc, captionText.c_str(), -1, &caption,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        ::DeleteObject(track);
        ::DeleteObject(segment);
        ::DeleteObject(used);
        ::DeleteObject(danger);
    }

    // Тот же экран состояния, что и на Direct2D, но на GDI. Он нужен не для
    // красоты, а для честности: если рендерер не поднялся (нет D3D11, отказ
    // драйвера), экран всё равно обязан объяснять, что происходит, — §5 «ни один
    // отказ не оставляет окно пустым».
    void paintMapEmptyWithGdi(HDC dc, const theme::Metrics& scale) {
        const theme::Palette& palette = theme.palette();
        const MapEmptyState state = mapEmptyState();
        RECT client{};
        ::GetClientRect(map, &client);
        // Фон — из темы, а не из кисти класса окна: карта рисуется этим путём
        // всегда (см. paintMap), и белая полоса посреди тёмной темы была бы
        // не состоянием, а обрывом интерфейса.
        HBRUSH surface = ::CreateSolidBrush(theme::colorRef(palette.surface));
        ::FillRect(dc, &client, surface);
        ::DeleteObject(surface);
        const auto px = [&scale](float dip) {
            return static_cast<LONG>(std::lround(static_cast<double>(dip) * scale.scale));
        };
        const LONG pad = px(16.0F);
        RECT frame{pad, pad, std::max(pad + 1L, client.right - pad), std::max(pad + 1L, client.bottom - pad)};

        HPEN border = ::CreatePen(PS_DOT, 1, theme::colorRef(palette.border));
        const HGDIOBJ oldPen = ::SelectObject(dc, border);
        const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        (void)::Rectangle(dc, frame.left, frame.top, frame.right, frame.bottom);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(border);

        ::SetBkMode(dc, TRANSPARENT);
        const LONG textLeft = frame.left + px(12.0F);
        const LONG textWidth = std::max(1L, frame.right - textLeft - px(12.0F));
        LONG y = frame.top + px(10.0F);
        const DWORD flags = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;

        ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
        if (fonts[0] != nullptr) ::SelectObject(dc, fonts[0]);
        RECT headline{textLeft, y, textLeft + textWidth, y + px(20.0F)};
        const std::wstring headlineText = toWide(state.headline);
        (void)::DrawTextW(dc, headlineText.c_str(), -1, &headline, flags);
        y = headline.bottom + px(4.0F);

        ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
        if (fonts[1] != nullptr) ::SelectObject(dc, fonts[1]);
        for (const std::string* line : {&state.reason, &state.action}) {
            const std::wstring wide = toWide(*line);
            RECT area{textLeft, y, textLeft + textWidth, frame.bottom};
            (void)::DrawTextW(dc, wide.c_str(), -1, &area, flags | DT_CALCRECT);
            const LONG height = std::max(area.bottom, y + px(16.0F)) - y;
            area.bottom = y + height;
            (void)::DrawTextW(dc, wide.c_str(), -1, &area, flags);
            y = area.bottom + px(2.0F);
            if (y > frame.bottom) break;
        }
    }

    LRESULT paintMap() {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(map, &paint);
        // Пустая карта рисуется GDI, а не Direct2D — и это не запасной путь, а
        // требование задачи: подпись «устройства не прочитаны» обязана быть
        // видна всегда. Обмен с DXGI на дочернем окне (карта — child хоста
        // содержимого) presents не всегда: проверено снимком, окно карты
        // оставалось белым при живом рендерере, то есть текст, нарисованный
        // блендером, просто не доезжал до экрана. GDI пишет прямо в DC окна и
        // доезжает всегда; когда диски есть, карта по-прежнему рисуется
        // рендерером (ADR-003: полосы пропорционально размеру).
        if (mapLayoutNow().bars.empty() || !mapHasContent(mapLayoutNow())) {
            paintMapEmptyWithGdi(dc, theme::metricsForDpi(static_cast<unsigned>(dpi)));
            ::EndPaint(map, &paint);
            return 0;
        }
        ensureRenderer();
        if (renderer.ready() && mapResources.complete()) {
            // GDI-путь сам выводит кадр в клиентскую область, поэтому там
            // endFrame() не зовётся второй раз (renderer.hpp, blitToHdc).
            const bool gdi = renderer.backend() == render::Backend::GdiWindow;
            if (renderer.beginFrame(toRender(theme.palette().surface))) {
                paintMapWithRenderer();
                const bool shown = gdi ? renderer.blitToHdc(dc) : renderer.endFrame();
                if (!shown && !gdi && renderer.recreateAttempts() < render::kMaxRecreateAttempts) {
                    // D2DERR_RECREATE_TARGET / DXGI_ERROR_DEVICE_REMOVED: ресурсы
                    // пересоздаются, кадр — на следующем проходе (§5: отказ не
                    // роняет процесс и не оставляет окно пустым).
                    if (renderer.recreate()) applyMapResources();
                    ::InvalidateRect(map, nullptr, FALSE);
                }
            }
        } else {
            paintMapFallback(dc);
        }
        ::EndPaint(map, &paint);
        return 0;
    }

    void onMapClick(int px, int py) {
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const MapLayout layout = mapLayoutNow();
        const std::optional<std::string> key =
            layout.hitTest(static_cast<float>(scale.undo(px)), static_cast<float>(scale.undo(py)));
        if (!key.has_value()) return;
        std::string wanted = *key;
        // Неразмеченный промежуток не является узлом дерева: выбираем диск, в
        // котором он лежит, — иначе клик по нему выбирал бы пустоту.
        const std::size_t gap = wanted.find(":u:");
        if (gap != std::string::npos) wanted = wanted.substr(0, gap);
        if (wanted.empty()) return;
        model.setSelectedKey(wanted);
        refreshSelection();
        if (list != nullptr) ::SetFocus(list);
    }

    // --- Команды -------------------------------------------------------------

    void doExport(ExportFormat format) {
        if (!model.hasInventory()) {
            logEvent(core::LogLevel::Warn, "ui.disks.export", "no inventory");
            return;
        }
        if (!callbacks.onExport) {
            logEvent(core::LogLevel::Warn, "ui.disks.export", "no export handler");
            return;
        }
        const MapExport bundle = model.exportBundle();
        std::string text = format == ExportFormat::Json ? bundle.json : bundle.text;
        if (text.empty()) {
            logEvent(core::LogLevel::Warn, "ui.disks.export", "empty export");
            return;
        }
        callbacks.onExport(format, text);
    }

    void doOpenInExplorer() {
        if (!model.explorerAvailable()) return;
        const std::string path = model.explorerPath();
        if (path.empty() || !callbacks.onOpenInExplorer) return;
        callbacks.onOpenInExplorer(path);
    }

    void doRefresh() {
        if (callbacks.onRefresh) callbacks.onRefresh();
    }

    void command(ControlId id) {
        switch (id) {
        case ControlId::FilterLetters: model.setFilter("lettered", !model.filterEnabled("lettered")); break;
        case ControlId::FilterSystem: model.setFilter("system", !model.filterEnabled("system")); break;
        case ControlId::FilterRemovable: model.setFilter("removable", !model.filterEnabled("removable")); break;
        case ControlId::ExportMap: doExport(ExportFormat::Json); break;
        case ControlId::ExportText: doExport(ExportFormat::Text); break;
        case ControlId::OpenInExplorer: doOpenInExplorer(); break;
        case ControlId::Refresh: doRefresh(); break;
        case ControlId::Check:
            // Кнопка v1.1 выключена; обработчик объявлен, чтобы мост не менялся,
            // когда проверка появится.
            if (callbacks.onCheck) callbacks.onCheck();
            break;
        }
    }

    // --- Подкласс и перерисовка ---------------------------------------------

    void createChild(HWND child) {
        if (child == nullptr || window == nullptr) return;
        const WNDPROC prev = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&childProc)));
        if (prev == nullptr) {
            logWin32("ui.disks.subclass", "SetWindowLongPtrW(GWLP_WNDPROC)", ::GetLastError());
            return;
        }
        ::SetWindowLongPtrW(child, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        children.push_back(ChildProc{child, prev});
    }

    void removeChild(HWND child) {
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (children[i].hwnd != child) continue;
            const WNDPROC prev = children[i].prev;
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
            ::SetWindowLongPtrW(child, GWLP_USERDATA, 0);
            if (prev != nullptr) ::SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev));
            return;
        }
    }

    // Отложенная перерисовка модели. Клавиши приходят в родителя из подкласса
    // списка, то есть дерево перестраивается, не выйдя из собственного обработчика
    // сообщения; вставка и удаление строк внутри LVN — изменение списка из его же
    // обработчика, на которое comctl32 не рассчитан. Поэтому визуальное обновление
    // всегда на следующем витке очереди, а модель меняется сразу: она и есть
    // источник истины.
    void requestSync() {
        if (window != nullptr) ::PostMessageW(window, kMsgSyncModel, 0, 0);
    }

    void refreshAll() {
        if (!controlsReady) return;
        syncColumns();
        syncList();
        syncHeaders();
        syncButtons();
        syncTexts();
        layout();
        if (card != nullptr) ::InvalidateRect(card, nullptr, FALSE);
        if (map != nullptr) ::InvalidateRect(map, nullptr, FALSE);
    }

    // Выделение меняет только подсветку на карте и карточку: пересобирать весь
    // список на каждый щелчок значило бы мигать на нём.
    void refreshSelection() {
        if (!controlsReady) return;
        syncListSelection();
        refreshCardAndMap();
    }

    // ------------------------------------------------------------------------
    // Приём кадров из фонового потока
    // ------------------------------------------------------------------------
    //
    // Экран подписан на раздачу сам, в своём create(), и больше ни от кого не
    // зависит: обход дисков (2 с на устройство, §4 FR-1) идёт в фоне, а сюда
    // приезжает уже готовый неизменяемый снимок (§6.4). Раньше подписки не было
    // ни у одного экрана, и модель молча оставалась пустой — это и есть «экран
    // умеет рисовать пустоту».
    void attachFeed() {
        if (feed != nullptr || window == nullptr) return;
        feed = mv::StartupFeed::instance().subscribe(window);
        // Если снимок успел приехать до подписки (окно пересоздали), берём его
        // сразу: ждать следующего обхода ради уже известных данных нельзя.
        if (const std::shared_ptr<const core::DiskInventory> ready = mv::StartupFeed::instance().inventory()) {
            model.publishInventory(ready);
        }
        mv::StartupFeed::instance().start();
    }

    void detachFeed() noexcept {
        if (feed == nullptr) return;
        mv::StartupFeed::instance().unsubscribe(feed);
        feed.reset();
    }

    // Вызывается из обработчика mv::kFeedMessage. Один кадр за раз: очередь
    // короткая, а на каждый кадр пересобирать весь список рано.
    void applyFeedFrames() {
        if (feed == nullptr) return;
        bool changed = false;
        mv::Event event;
        while (feed->take(event)) {
            switch (event.kind()) {
            case mv::EventKind::Inventory: {
                if (const auto inventory = event.as<core::DiskInventory>()) {
                    model.publishInventory(inventory);
                    changed = true;
                }
                break;
            }
            case mv::EventKind::Disks: {
                if (const auto disks = event.as<std::vector<core::PhysicalDisk>>()) {
                    model.publishDisks(*disks);
                    changed = true;
                }
                break;
            }
            case mv::EventKind::Notice:
            case mv::EventKind::Error: {
                const std::shared_ptr<const std::string> text = event.as<std::string>();
                if (text) {
                    setChildText(status, *text);
                    changed = true;
                }
                break;
            }
            default:
                break;
            }
        }
        if (changed) refreshAll();
    }

    // Только содержимое выбранного узла. Вызывается прямо из LVN_ITEMCHANGED —
    // то есть изнутри обработки уведомления списка, где вставка и удаление строк
    // недопустимы (на этом comctl32 не рассчитан). Перестроение списка оттуда
    // делается отложенно, через requestSync.
    void refreshCardAndMap() {
        if (card != nullptr) ::InvalidateRect(card, nullptr, FALSE);
        if (map != nullptr) ::InvalidateRect(map, nullptr, FALSE);
    }

    bool createControls(HINSTANCE instance) {
        const DWORD childVisible = WS_CHILD | WS_VISIBLE;
        map = ::CreateWindowExW(0, kDisksMapClass, nullptr, childVisible, 0, 0, 0, 0, window,
                                reinterpret_cast<HMENU>(kChildMap), instance, this);
        // LVS_EX_FULLROWSELECT — выделяется вся строка (легче нажать и легче
        // прочитать). LVS_EX_DOUBLEBUFFER — без него список мигает при каждой
        // перерисовке, а мы перерисовываем его на каждом щелчке. Подпись
        // LVS_EX_LABELTIP не включаем: подсказка недоступна экранному диктору, а
        // ту же подсказку показывает карточка (§5 доступность — как у соседнего
        // экрана, где по той же причине выключены подсказки дерева).
        list = ::CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                                 childVisible | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                     LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                                 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kChildList), instance, this);
        status = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0,
                                   0, 0, 0, window, reinterpret_cast<HMENU>(kChildStatus), instance, this);
        card = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_OWNERDRAW, 0, 0, 0, 0, window,
                                 reinterpret_cast<HMENU>(kChildCard), instance, this);
        const std::array<ControlId, kDisksFilterCount> filterIds{
            ControlId::FilterLetters, ControlId::FilterSystem, ControlId::FilterRemovable};
        for (std::size_t i = 0; i < filterButtons.size(); ++i) {
            filterButtons[i] = ::CreateWindowExW(0, L"BUTTON", nullptr,
                                                childVisible | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, window,
                                                reinterpret_cast<HMENU>(filterIds[i]), instance, this);
        }
        const std::array<ControlId, kDisksActionCount> actionIds{
            ControlId::ExportMap, ControlId::ExportText, ControlId::OpenInExplorer, ControlId::Refresh,
            ControlId::Check};
        for (std::size_t i = 0; i < actionButtons.size(); ++i) {
            actionButtons[i] = ::CreateWindowExW(0, L"BUTTON", nullptr,
                                                childVisible | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window,
                                                reinterpret_cast<HMENU>(actionIds[i]), instance, this);
        }
        for (const HWND child : {map, list, status, card}) {
            if (child == nullptr) {
                logWin32("ui.disks.create", "CreateWindowExW(child)", ::GetLastError());
                return false;
            }
        }
        for (const HWND button : filterButtons) {
            if (button == nullptr) {
                logWin32("ui.disks.create", "CreateWindowExW(filter)", ::GetLastError());
                return false;
            }
        }
        for (const HWND button : actionButtons) {
            if (button == nullptr) {
                logWin32("ui.disks.create", "CreateWindowExW(action)", ::GetLastError());
                return false;
            }
        }
        // Подкласс нужен списку и кнопкам: они едят клавиши, и без подкласса
        // Ctrl+E и Enter до модели не дошли бы (§5 «Клавиатурная навигация,
        // фокус»). Карта и строка состояния клавиши не едят.
        createChild(list);
        for (const HWND button : filterButtons) createChild(button);
        for (const HWND button : actionButtons) createChild(button);
        controlsReady = true;
        return true;
    }
};

// Значки строки справа от последнего столбца. Рисуются в CDDS_ITEMPOSTPAINT —
// после того, как контрол нарисовал строку: в CDDS_ITEMPREPAINT заливка фона
// стёрла бы всё, что нарисовано рядом с подписью (то же решение, что у соседнего
// экрана с иконкой риска).
void paintRowBadges(ViewState& state, HDC dc, int index) {
    const TreeRow* row = state.rowAt(index);
    if (row == nullptr || state.list == nullptr) return;
    std::array<Badge, 3> badges{};
    std::size_t count = 0;
    if (row->unavailable) badges[count++] = Badge::Unavailable;
    if (row->lowSpace) badges[count++] = Badge::LowSpace;
    if (row->encrypted) badges[count++] = Badge::Encrypted;
    if (count == 0) return;

    RECT bounds{};
    bounds.left = kColumnCount - 1;   // индекс подпункта
    bounds.top = LVIR_BOUNDS;         // что именно вернуть
    bounds.bottom = 0;
    if (::SendMessageW(state.list, LVM_GETSUBITEMRECT, static_cast<WPARAM>(index),
                       reinterpret_cast<LPARAM>(&bounds)) == FALSE) {
        return;
    }
    RECT client{};
    if (::GetClientRect(state.list, &client) == FALSE) return;
    const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(state.dpi));
    const int side = std::max(6, scale.dip(9.0));
    const int need = static_cast<int>(count) * (side + 2);
    int right = client.right - 4;
    // Если значки не помещаются в свободную полосу, не рисуем ни одного: половина
    // значков читалась бы как «состояние неизвестно», а состояние известно и
    // показано в карточке.
    if (right - need <= bounds.right) return;
    const int center = (bounds.top + bounds.bottom) / 2 - side / 2;
    for (std::size_t i = count; i > 0; --i) {
        const RECT box{right - side, center, right, center + side};
        right -= side + 2;
        if (box.top < bounds.top || box.bottom > bounds.bottom) continue;
        paintBadge(dc, box, badges[i - 1], state.theme.palette());
    }
}

// Карточка деталей выбранного узла (FR-2: «клик по разделу — карточка с
// деталями»). Рисуется GDI, а не Direct2D: это текст в рамке, а не графика
// собственной природы — ровно то, что ADR-003 оставляет нативным контролам.
LRESULT drawCard(ViewState& state, const DRAWITEMSTRUCT& draw) {
    const theme::Palette& palette = state.theme.palette();
    const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(state.dpi));
    HDC dc = draw.hDC;
    RECT box = draw.rcItem;

    HBRUSH surface = ::CreateSolidBrush(theme::colorRef(palette.surface));
    ::FillRect(dc, &box, surface);
    ::DeleteObject(surface);
    HBRUSH border = ::CreateSolidBrush(theme::colorRef(palette.border));
    ::FrameRect(dc, &box, border);
    ::DeleteObject(border);

    const int pad = std::max(2, scale.dip(state.metrics.cardPaddingDip));
    const int titleHeight = std::max(8, scale.dip(20.0));
    const int lineHeight = std::max(8, scale.dip(16.0));
    RECT inner{box.left + pad, box.top + pad / 2, box.right - pad, box.bottom - pad / 2};
    if (inner.bottom <= inner.top) return TRUE;

    const std::wstring title = toWide(state.model.cardTitle());
    if (state.fonts[3] != nullptr) ::SelectObject(dc, state.fonts[3]);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
    RECT titleRect{inner.left, inner.top, inner.right, std::min(inner.bottom, inner.top + titleHeight)};
    ::DrawTextW(dc, title.c_str(), -1, &titleRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const std::vector<std::string> lines = state.model.cardLines();
    if (state.fonts[1] != nullptr) ::SelectObject(dc, state.fonts[1]);
    const int top = titleRect.bottom;
    const int available = inner.bottom - top;
    const std::size_t capacity =
        available > 0 ? static_cast<std::size_t>(available / std::max(1, lineHeight)) : 0U;
    if (capacity == 0) return TRUE;

    const std::size_t shown = std::min(capacity, lines.size());
    for (std::size_t i = 0; i < shown; ++i) {
        std::string text = lines[i];
        // Обрезанный список полей — это молчание о недостающих свойствах, поэтому
        // последняя видимая строка честно помечается многоточием.
        if (i + 1 == shown && shown < lines.size()) {
            text += "…";
        }
        RECT line{inner.left, top + static_cast<int>(i) * lineHeight, inner.right,
                  top + static_cast<int>(i + 1) * lineHeight};
        if (line.bottom > inner.bottom) break;
        const std::size_t separator = text.find(": ");
        if (separator == std::string::npos) {
            ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
            const std::wstring wide = toWide(text);
            ::DrawTextW(dc, wide.c_str(), -1, &line,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }
        const std::wstring label = toWide(text.substr(0, separator + 1));
        SIZE measured{};
        ::GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &measured);
        const int labelWidth = std::min(measured.cx + 8, (line.right - line.left) / 2);
        RECT labelRect{line.left, line.top, line.left + labelWidth, line.bottom};
        RECT valueRect{line.left + labelWidth, line.top, line.right, line.bottom};
        ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
        ::DrawTextW(dc, label.c_str(), -1, &labelRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
        const std::wstring value = toWide(text.substr(separator + 2));
        ::DrawTextW(dc, value.c_str(), -1, &valueRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    return TRUE;
}

// Обработчик окна карты. Собственная процедура, а не подкласс: у неё своя
// отрисовка (Direct2D или GDI-откат) и своя обработка клика по сегменту.
LRESULT CALLBACK mapProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ViewState* state = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = const_cast<ViewState*>(static_cast<const ViewState*>(create->lpCreateParams));
        if (state != nullptr) state->map = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    } else {
        state = stateOf(window);
    }
    if (state == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);

    try {
        switch (message) {
        case WM_CREATE:
            return 0;
        case WM_SIZE: {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            if (state->renderer.ready()) {
                if (width > 0 && height > 0) state->renderer.resize(static_cast<std::uint32_t>(width),
                                                                      static_cast<std::uint32_t>(height));
            } else {
                // Окно только что получило размер — единственный момент, когда
                // рендерер может быть создан: в WM_NCCREATE он нулевой.
                state->ensureRenderer();
                ::InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            if (state->renderer.ready() && HIWORD(wParam) > 0) {
                const float value = static_cast<float>(HIWORD(wParam));
                state->renderer.setDpi(value, value);
            }
            return 0;
        }
        case WM_PAINT:
            return state->paintMap();
        case WM_ERASEBKGND:
            // Свою поверхность мы очищаем сами (beginFrame или FillRect), а лишнее
            // стирание мигает при каждой перерисовке.
            return 1;
        case WM_LBUTTONUP:
            state->onMapClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_SETCURSOR: {
            // Рука над картой: клик по сегменту что-то делает, и это ожидаемо.
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        case WM_DESTROY:
            state->mapResources.releaseWith(state->renderer);
            state->renderer.destroy();
            state->map = nullptr;
            return 0;
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default: break;
        }
    } catch (const std::exception& error) {
        logEvent(core::LogLevel::Error, "ui.disks.map.exception", error.what());
        return 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

// Обработчик окна экрана. Исключение не пересекает границу Win32 (§5): ловим
// здесь и пишем в журнал, иначе std::terminate внутри пользовательского режима
// не дал бы узнать, что произошло.
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ViewState* state = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        // lpCreateParams — const void*: разыменования тут нет, указатель живёт
        // дольше окна, поэтому снимаем const один раз здесь.
        state = const_cast<ViewState*>(static_cast<const ViewState*>(create->lpCreateParams));
        // Свой HWND известен уже здесь, а WM_CREATE создаёт детей именно от него:
        // без этой строки CreateWindowExW получил бы пустого родителя.
        if (state != nullptr) state->window = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    } else {
        state = stateOf(window);
    }
    if (state == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);

    try {
        switch (message) {
        case WM_CREATE:
            if (!state->createControls(::GetModuleHandleW(nullptr))) return -1;
            return 0;
        case WM_SIZE:
            state->layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (info != nullptr) {
                const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(state->dpi));
                info->ptMinTrackSize.x = scale.dip(state->metrics.minWidthDip);
                info->ptMinTrackSize.y = scale.dip(state->metrics.minHeightDip);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (HIWORD(wParam) > 0) state->dpi = HIWORD(wParam);
            state->theme.setDpi(static_cast<unsigned>(state->dpi));
            if (suggested != nullptr) {
                ::SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            state->applyPalette();
            state->applyFonts();
            state->refreshAll();
            return 0;
        }
        case WM_SETFOCUS:
            // §5 «Клавиатурная навигация, фокус»: фокус должен быть виден, и он
            // должен быть на дереве, а не на пустом окне.
            if (state->list != nullptr) ::SetFocus(state->list);
            return 0;
        case WM_KEYDOWN: {
            const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (state->model.handleKeyDown(static_cast<std::uint32_t>(wParam), control, shift)) {
                state->requestSync();
                return 1;
            }
            switch (wParam) {
            case VK_RETURN:
                if (state->model.explorerAvailable()) {
                    state->doOpenInExplorer();
                    return 1;
                }
                break;
            case 'E':
                if (control) {
                    state->doExport(ExportFormat::Json);
                    return 1;
                }
                break;
            case 'T':
                if (control) {
                    state->doExport(ExportFormat::Text);
                    return 1;
                }
                break;
            case 'F5':
                state->doRefresh();
                return 1;
            default: break;
            }
            break;
        }
        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) break;
            const WORD id = LOWORD(wParam);
            if (!isDisksControl(id)) break;
            state->command(static_cast<ControlId>(id));
            state->refreshAll();
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header == nullptr) break;
            if (header->hwndFrom != state->list) break;
            switch (header->code) {
            case LVN_ITEMCHANGED: {
                const auto* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if (change == nullptr || state->syncing) break;
                if ((change->uChanged & LVIF_STATE) == 0) break;
                if ((change->uNewState & LVIS_SELECTED) == 0) break;
                if (change->iItem < 0) break;
                const std::string key = state->model.keyAt(static_cast<std::size_t>(change->iItem));
                if (key.empty() || key == state->model.selectedKey()) break;
                state->model.setSelectedKey(key);
                state->refreshCardAndMap();
                break;
            }
            case LVN_COLUMNCLICK: {
                const auto* headerClick = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if (headerClick == nullptr) break;
                state->model.toggleSort(state->sortKeyOfColumn(headerClick->iSubItem));
                state->refreshAll();
                break;
            }
            case NM_DBLCLK: {
                state->doOpenInExplorer();
                break;
            }
            case NM_CUSTOMDRAW: {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                if (draw == nullptr) break;
                const theme::Palette& palette = state->theme.palette();
                switch (draw->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: {
                    const int index = static_cast<int>(draw->nmcd.dwItemSpec);
                    const TreeRow* row = state->rowAt(index);
                    const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
                    const bool unavailable = row != nullptr && row->unavailable;
                    HDC dc = draw->nmcd.hdc;
                    // Цвет подписи и фона — из темы, а не системный: на тёмной
                    // палитре системный чёрный текст нечитаем, а «зелёная полоса
                    // выделения» посреди тёмной темы выглядит дырой.
                    ::SetBkMode(dc, TRANSPARENT);
                    ::SetBkColor(dc, theme::colorRef(unavailable ? palette.surfaceAlt : palette.surface));
                    const theme::Color text = selected ? palette.textOnAccent
                                                       : (unavailable ? palette.textDisabled : palette.textPrimary);
                    ::SetTextColor(dc, theme::colorRef(text));
                    return CDRF_NOTIFYSUBITEMDRAW;
                }
                case kSubItemPrePaint: {
                    // Числа рисуем сами: так они выравниваются вправо, колонки
                    // читаются как числа, а не как текст, и мы не зависим от
                    // системного шрифта списка. Прямоугольник подпункта на этой
                    // стадии лежит в nmcd.rc.
                    if (draw->iSubItem == 0) return CDRF_DODEFAULT;
                    const int index = static_cast<int>(draw->nmcd.dwItemSpec);
                    const TreeRow* row = state->rowAt(index);
                    RECT box = draw->nmcd.rc;
                    HDC dc = draw->nmcd.hdc;
                    if (state->fonts[0] != nullptr) ::SelectObject(dc, state->fonts[0]);
                    ::SetBkMode(dc, TRANSPARENT);
                    const std::string text = row == nullptr
                                                 ? std::string()
                                                 : (draw->iSubItem == 1 ? row->freeText : row->usedText);
                    const std::wstring wide = toWide(text);
                    ::DrawTextW(dc, wide.c_str(), -1, &box,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    return CDRF_SKIPDEFAULT;
                }
                case CDDS_ITEMPOSTPAINT:
                    paintRowBadges(*state, draw->nmcd.hdc, static_cast<int>(draw->nmcd.dwItemSpec));
                    return CDRF_DODEFAULT;
                default: break;
                }
                // Неизвестная стадия: сообщаем один раз. Если подстадии подпункта
                // придут с другими значениями, это будет видно в журнале, а не
                // молчаливым отсутствием чисел в списке.
                if (!state->drawStageLogged) {
                    state->drawStageLogged = true;
                    logEvent(core::LogLevel::Warn, "ui.disks.list.customdraw", "unknown draw stage");
                }
                return CDRF_DODEFAULT;
            }
            default: break;
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw == nullptr) break;
            if (draw->CtlType == ODT_STATIC && draw->CtlID == static_cast<UINT>(kChildCard) &&
                draw->hwndItem == state->card) {
                return drawCard(*state, *draw);
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const theme::Palette& palette = state->theme.palette();
            ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
            ::SetBkColor(dc, theme::colorRef(palette.surface));
            if (state->surfaceBrush == nullptr) {
                state->surfaceBrush = ::CreateSolidBrush(theme::colorRef(palette.surface));
            }
            return reinterpret_cast<LRESULT>(state->surfaceBrush);
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE: {
            if (theme::classifyMessage(message, wParam, lParam) == theme::Change::None) break;
            state->theme.reload();
            state->applyPalette();
            state->applyFonts();
            state->refreshAll();
            return 0;
        }
        case kMsgSyncModel:
            state->refreshAll();
            return 0;
        case mv::kFeedMessage:
            // Кадр из фонового обхода: снимок инвентаризации или причина, по
            // которой её нет. Разбор здесь, а не в наблюдателе: модель меняется
            // только в UI-потоке (§6.1).
            state->applyFeedFrames();
            return 0;
        case WM_ERASEBKGND:
            // Дети перекрывают окно целиком; стирать собственную поверхность
            // незачем.
            return 1;
        case WM_DESTROY:
            state->controlsReady = false;
            return 0;
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default: break;
        }
    } catch (const std::exception& error) {
        logEvent(core::LogLevel::Error, "ui.disks.exception", error.what());
        return message == WM_CREATE ? -1 : 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

// Подкласс контрола. Нужен для одного: клавиши уходят родителю, где их
// разбирает модель (§5 «Клавиатурная навигация»). SysListView32 и BUTTON свои
// клавиши едят, и Ctrl+E на кнопке молча пропадал бы.
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = stateOf(window);
    if (state == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        state->removeChild(window);
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    if (message == WM_KEYDOWN && state->window != nullptr) {
        const LRESULT handled = ::SendMessageW(state->window, WM_KEYDOWN, wParam, lParam);
        if (handled != 0) return handled;
    }
    for (const ChildProc& entry : state->children) {
        if (entry.hwnd == window && entry.prev != nullptr) {
            return ::CallWindowProcW(entry.prev, window, message, wParam, lParam);
        }
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// DisksScreen
// ---------------------------------------------------------------------------

struct DisksScreen::Impl : detail::ViewState {
    Impl() { dpi = static_cast<int>(theme.metrics().dpi); }
};

DisksScreen::DisksScreen(Callbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->callbacks = std::move(callbacks);
}

DisksScreen::~DisksScreen() { destroy(); }

HWND DisksScreen::create(HWND parent, int dpi) {
    auto& state = *impl_;
    if (parent == nullptr) return nullptr;
    ensureStrings();
    if (state.window != nullptr) return state.window;
    if (dpi > 0) state.dpi = dpi;
    state.theme.setDpi(static_cast<unsigned>(state.dpi));

    // SysListView32 без ICC_* не создаётся вовсе, а §7 требует именно его.
    // Повторный вызов безвреден.
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    if (::InitCommonControlsEx(&controls) == FALSE) {
        logWin32("ui.disks.create", "InitCommonControlsEx", ::GetLastError());
    }

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW mapClass{};
    mapClass.cbSize = sizeof(mapClass);
    mapClass.style = CS_HREDRAW | CS_VREDRAW;
    mapClass.lpfnWndProc = &detail::mapProc;
    mapClass.hInstance = instance;
    mapClass.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
    mapClass.hbrBackground = nullptr;
    mapClass.lpszClassName = detail::kDisksMapClass;
    if (::RegisterClassExW(&mapClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logWin32("ui.disks.create", "RegisterClassExW(map)", ::GetLastError());
        return nullptr;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &detail::viewProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = detail::kDisksViewClass;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logWin32("ui.disks.create", "RegisterClassExW(view)", ::GetLastError());
        return nullptr;
    }

    state.window = ::CreateWindowExW(0, detail::kDisksViewClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                     parent, nullptr, instance, &state);
    if (state.window == nullptr) {
        logWin32("ui.disks.create", "CreateWindowExW(view)", ::GetLastError());
        return nullptr;
    }
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
    state.attachFeed();
    state.refreshAll();
    return state.window;
}

HWND DisksScreen::window() const noexcept { return impl_->window; }

void DisksScreen::destroy() noexcept {
    auto& state = *impl_;
    if (state.window == nullptr) return;
    HWND window = state.window;
    state.window = nullptr;
    state.controlsReady = false;
    // Детей снимает DestroyWindow: каждое пришлёт WM_NCDESTROY и вернёт свою
    // исходную процедуру. Список children здесь не чистим — иначе подкласс
    // остался бы на уничтоженном контроле.
    ::DestroyWindow(window);
    state.rowKeys.clear();
    state.children.clear();
    state.detachFeed();
    state.map = nullptr;
    state.list = nullptr;
    state.status = nullptr;
    state.card = nullptr;
    state.filterButtons.fill(nullptr);
    state.actionButtons.fill(nullptr);
}

void DisksScreen::setDpi(int dpi) {
    auto& state = *impl_;
    if (dpi > 0) state.dpi = dpi;
    state.theme.setDpi(static_cast<unsigned>(state.dpi));
    if (state.window == nullptr) return;
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
}

DisksViewModel& DisksScreen::model() noexcept { return impl_->model; }

const DisksViewModel& DisksScreen::model() const noexcept { return impl_->model; }

void DisksScreen::refresh() { impl_->refreshAll(); }

void DisksScreen::reloadTheme() {
    auto& state = *impl_;
    state.theme.reload();
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
}

void DisksScreen::publishInventory(std::shared_ptr<const core::DiskInventory> inventory) {
    impl_->model.publishInventory(std::move(inventory));
    impl_->refreshAll();
}

void DisksScreen::publishDisks(std::vector<PhysicalDisk> disks) {
    impl_->model.publishDisks(std::move(disks));
    impl_->refreshAll();
}

}  // namespace mrproper::ui::disks
