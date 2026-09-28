// mrproper-cli disks — эталонный дамп карты разделов: реализация.
//
// Разбор опций, оркестрация обхода, печать дампа в stdout и человеческого текста
// в stderr. Что за команда и почему она разделена на каналы — в cmd_disks.hpp;
// здесь только код.
//
// ---------------------------------------------------------------------------
// Что этот файл делает и чего не делает
// ---------------------------------------------------------------------------
//
//  * разбирает опции и печатает справку (чистые функции, тестируются без диска);
//  * один раз зовёт обход инвентаризации (makePlatformDisksServices ниже) и
//    печатает то, что тот вернул;
//  * JSON карты строит core::report_json (partitionMapToJson), агрегаты —
//    core::disk_model, текст — core::toText. Ни одного байта размера,
//    занятости или названия этот файл не придумывает: всё, чего нет в
//    инвентаризации, печатается как «нет данных», а не как ноль.
//
// Чего здесь нет сознательно:
//
//  * WMI-обогащения инвентаризации (FR-1 п.8) — им занимается
//    platform::inventory, CLI повторяет его обход только когда его просят явно
//    (--encryption, и то только BitLocker из FR-1 п.7);
//  * кэша и фонового потока: это InventoryCache, а CLI — разовый дамп, и
//    кэш в процессе, который живёт доли секунды, означал бы только лишний
//    поток (SPEC §6.4).
//
// ---------------------------------------------------------------------------
// Исключения
// ---------------------------------------------------------------------------
//
// Наружу из runDisks не летит ничего: SPEC §5 («ни один отказ не роняет процесс»)
// и §12 («0 необработанных исключений»). Обход инвентаризации сам по себе
// noexcept и отказ представляет записью в issues, но std::bad_alloc при печати
// дампа — уже граница команды: ловится здесь и становится кодом возврата с
// текстом в stderr, а не аварийным завершением процесса посреди JSON, который
// скрипт уже начал читать.
#include "cmd_disks.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/disk_model.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/model.hpp"
#include "core/report_json.hpp"
#include "platform/inventory.hpp"
#include "platform/wmi.hpp"

namespace mrproper::cli {
namespace {

// ---------------------------------------------------------------------------
// Константы команды
// ---------------------------------------------------------------------------

// Имена событий в журнале: по префиксу в логе виден весь прогон CLI, не читая
// сообщений. Те же имена, что у соседних команд ("cli.scan.*"), ради greppable
// лога.
constexpr std::string_view kEventStart = "cli.disks.start";
constexpr std::string_view kEventFinish = "cli.disks.finish";
constexpr std::string_view kEventDegraded = "cli.disks.degraded";

// Сутки — потолок таймаутов. Больше ждать нечего: FR-1 требует 2 с на
// устройство, и «--device-timeout 1h» — опечатка, а не настройка.
constexpr std::uint64_t kMaxDurationMilliseconds = 24ull * 60 * 60 * 1000;

// Сколько замечаний обхода печатается в stderr и в текстовый экспорт. Дамп не
// режется: там замечания идут все, и разрыв между «что в файле» и «что в
// консоли» хуже длинного списка. А вот консоль на 200 строк отказов от одного
// устройства перестаёт быть полезной, поэтому в stderr идёт начало списка и
// счётчик остатка.
constexpr std::size_t kMaxIssuesInStderr = 20;

// Единственная точка, где целое становится числом JSON (та же, что в
// core::report_json): приведение делается явно один раз, чтобы опечатка в
// static_cast не проскочила мимо глаз. Значения до 2^53 печатаются точно, а
// диск на 9 петабайт в разрядности JSON не помещается — об этом честно
// предупреждает core::validateReport для отчётов, здесь тот же предел молча не
// расширяется.
template <typename T>
json::Value num(T value) {
    return json::Value(static_cast<double>(static_cast<std::int64_t>(value)));
}

// ---------------------------------------------------------------------------
// Разбор аргументов
// ---------------------------------------------------------------------------

// «--имя» или «--имя=значение». Одиночный дефис (ключ POSIX вида -j) команда не
// принимает намеренно: в проекте ключи длинные (--with-disks, --progress-interval),
// и короткие формы пришлось бы запоминать отдельно.
[[nodiscard]] bool splitOption(const std::string& argument, std::string& name, std::optional<std::string>& value) {
    if (argument.size() < 3 || argument[0] != '-' || argument[1] != '-') return false;
    const std::size_t equals = argument.find('=');
    if (equals == std::string::npos) {
        name = argument.substr(2);
        value.reset();
        return true;
    }
    name = argument.substr(2, equals - 2);
    value = argument.substr(equals + 1);
    return true;
}

// Число без знака. Лишние символы — ошибка формата: «--device-timeout 2xs» не
// должен молча превратиться в 2.
[[nodiscard]] bool parseUnsigned(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();
    for (const char symbol : text) {
        if (symbol < '0' || symbol > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(symbol - '0');
        if (value > (kMax - digit) / 10) return false;  // переполнение — ошибка формата
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// Длительность: «1500», «1500ms», «2s», «3m». Без суффикса — миллисекунды.
// Пустая строка и мусор — ошибка формата.
[[nodiscard]] bool parseDuration(const std::string& text, std::chrono::milliseconds& out) {
    if (text.empty()) return false;
    std::size_t digits = text.size();
    std::uint64_t multiplier = 1;
    const char suffix = text[digits - 1];
    if (suffix == 's' || suffix == 'S') {
        multiplier = 1000;
        --digits;
    } else if (suffix == 'm' || suffix == 'M') {
        multiplier = 60 * 1000;
        --digits;
    }
    std::uint64_t value = 0;
    if (digits == 0 || !parseUnsigned(text.substr(0, digits), value)) return false;
    if (value > kMaxDurationMilliseconds / multiplier) return false;
    out = std::chrono::milliseconds(static_cast<std::int64_t>(value * multiplier));
    return true;
}

// ---------------------------------------------------------------------------
// Значения в JSON
// ---------------------------------------------------------------------------

// Агрегат по всей инвентаризации. Только целые числа: доли в процентах считает
// потребитель из байт, а округление внутри дампа сделало бы сверку с Get-Disk
// зависящей от того, где посчитали.
[[nodiscard]] json::Value usageValue(const core::InventoryUsage& usage) {
    return json::Value::object({
        {"totalBytes", num(usage.totalBytes)},
        {"volumeBytes", num(usage.volumeBytes)},
        {"unallocatedBytes", num(usage.unallocatedBytes)},
        {"usedBytes", num(usage.usedBytes)},
        {"freeBytes", num(usage.freeBytes)},
        {"encryptedBytes", num(usage.encryptedBytes)},
        {"readOnlyBytes", num(usage.readOnlyBytes)},
        {"removableBytes", num(usage.removableBytes)},
        {"diskCount", num(usage.diskCount)},
        {"partitionCount", num(usage.partitionCount)},
        {"volumeCount", num(usage.volumeCount)},
        {"knownVolumeCount", num(usage.knownVolumeCount)},
        {"unknownVolumeCount", num(usage.unknownVolumeCount)},
        {"spanningVolumeCount", num(usage.spanningVolumeCount)},
        {"freeKnown", json::Value(usage.freeKnown)},
    });
}

// Агрегат по одному диску. Нужен в карте рядом с разделом самого диска: в
// JSON отчёта (core::report_json) этих чисел нет, а без них «сумма размеров»
// из §8 Этап 1 приходится считать вручную по вложенным разделам.
[[nodiscard]] json::Value diskUsageValue(const core::DiskUsage& usage) {
    return json::Value::object({
        {"sizeBytes", num(usage.sizeBytes)},
        {"partitionedBytes", num(usage.partitionedBytes)},
        {"unallocatedBytes", num(usage.unallocatedBytes)},
        {"volumeBytes", num(usage.volumeBytes)},
        {"usedBytes", num(usage.usedBytes)},
        {"freeBytes", num(usage.freeBytes)},
        {"readOnlyBytes", num(usage.readOnlyBytes)},
        {"spanningFreeBytes", num(usage.spanningFreeBytes)},
        {"partitionCount", num(usage.partitionCount)},
        {"volumeCount", num(usage.volumeCount)},
        {"spanningVolumeCount", num(usage.spanningVolumeCount)},
        {"sizeKnown", json::Value(usage.sizeKnown)},
        {"freeKnown", json::Value(usage.freeKnown)},
    });
}

// Том, который не привязался к разделу. Отдельный раздел не «повторяет»
// том из карты: в карте его нет, иначе он считался бы в агрегатах дважды.
[[nodiscard]] json::Value unboundVolumeValue(const core::Volume& volume, bool maskGuids) {
    std::vector<json::Value> mountPoints;
    mountPoints.reserve(volume.mountPoints.size());
    for (const std::string& mountPoint : volume.mountPoints) mountPoints.push_back(json::Value(mountPoint));

    return json::Value::object({
        {"volumeGuidPath", json::Value(maskGuids ? core::maskVolumeGuid(volume.volumeGuidPath) : volume.volumeGuidPath)},
        {"label", json::Value(volume.label)},
        {"fileSystem", json::Value(volume.fileSystem)},
        {"totalBytes", num(volume.totalBytes)},
        {"freeBytes", num(volume.freeBytes)},
        {"encrypted", json::Value(volume.encrypted)},
        {"readOnly", json::Value(volume.readOnly)},
        {"dirty", json::Value(volume.dirty)},
        {"extentCount", num(volume.diskExtents.size())},
        {"mountPoints", json::Value::array(std::move(mountPoints))},
    });
}

[[nodiscard]] json::Value encryptionValue(const std::vector<VolumeEncryption>& encryption) {
    std::vector<json::Value> items;
    items.reserve(encryption.size());
    for (const VolumeEncryption& item : encryption) {
        items.push_back(json::Value::object({
            {"driveLetter", json::Value(item.driveLetter)},
            {"state", json::Value(item.state)},
            {"method", json::Value(item.method)},
            {"source", json::Value(item.source)},
            {"protection", json::Value(item.protectedVolume)},
            {"known", json::Value(item.known)},
            {"detail", json::Value(item.detail)},
        }));
    }
    return json::Value::array(std::move(items));
}

// Замечания core::validateInventory — те самые «расхождения», ради которых
// дамп и сверяют с эталоном. Коды машинные (severity, code, diskNumber), текст
// по-русски.
[[nodiscard]] json::Value consistencyValue(const std::vector<core::PhysicalDisk>& disks) {
    std::vector<json::Value> items;
    for (const core::DiskIssue& issue : core::validateInventory(disks)) {
        items.push_back(json::Value::object({
            {"severity", json::Value(core::toString(issue.severity))},
            {"diskNumber", num(issue.diskNumber)},
            {"partitionIndex", issue.hasPartition ? json::Value(num(issue.partitionIndex)) : json::Value()},
            {"code", json::Value(issue.code)},
            {"message", json::Value(issue.message)},
        }));
    }
    return json::Value::array(std::move(items));
}

// Неполна ли карта. Один вопрос из нескольких фактов, чтобы --strict, stderr и
// раздел inventory не решали по-разному: решение «карта полная» иначе
// означало бы, что неполноты нет, а не то, что про неё забыли.
[[nodiscard]] bool isDegraded(const DisksInventory& inventory) noexcept {
    return !inventory.degradedReasons.empty() || !inventory.issues.empty() || inventory.timedOutDevices > 0 ||
           inventory.unavailableDevices > 0 || !inventory.disksComplete || !inventory.volumesComplete;
}

// Состояние обхода: сколько заняло, что не ответило, чем карта неполна. Здесь
// живут числа, которых нет в core-модели, — без них «0 байт» и «не спросили» не
// различить.
[[nodiscard]] json::Value inventoryValue(const DisksInventory& inventory) {
    std::vector<json::Value> issues;
    issues.reserve(inventory.issues.size());
    for (const std::string& issue : inventory.issues) issues.push_back(json::Value(issue));

    std::vector<json::Value> reasons;
    reasons.reserve(inventory.degradedReasons.size());
    for (const std::string& reason : inventory.degradedReasons) reasons.push_back(json::Value(reason));

    return json::Value::object({
        {"collectedAtUnix", num(inventory.collectedAtUnix)},
        {"durationMs", num(inventory.durationMs)},
        {"disksComplete", json::Value(inventory.disksComplete)},
        {"volumesComplete", json::Value(inventory.volumesComplete)},
        {"degraded", json::Value(isDegraded(inventory))},
        {"degradedReasons", json::Value::array(std::move(reasons))},
        {"timedOutDevices", num(inventory.timedOutDevices)},
        {"unavailableDevices", num(inventory.unavailableDevices)},
        {"issues", json::Value::array(std::move(issues))},
    });
}

// Разряд разметки для JSON. Своя функция, а не core::toString(PartitionScheme):
// та отдаёт «неизвестно» по-русски, а ключи и значения разделов в дампе не
// локализуются (те же стабильные токены, что у bus в core::report_json) —
// иначе скрипт, ищущий "unknown", развалился бы на русской сборке.
[[nodiscard]] const char* schemeToken(core::PartitionScheme scheme) noexcept {
    switch (scheme) {
        case core::PartitionScheme::Gpt:
            return "gpt";
        case core::PartitionScheme::Mbr:
            return "mbr";
        case core::PartitionScheme::Unknown:
            break;
    }
    return "unknown";
}

// К одному диску добавляем то, чего в карте отчёта нет: разряд разметки
// (FR-2 «GPT/MBR»), признак «платформа вообще ничего не знает о диске» и
// агрегат по диску. Схема самих дисков при этом остаётся ровно той, что у
// core::report_json: два артефакта о дисках, которые не должны разойтись.
[[nodiscard]] json::Value withDiskExtras(const json::Value& diskValue, const core::PhysicalDisk& disk) {
    std::vector<std::pair<std::string, json::Value>> members = diskValue.members();
    members.emplace_back("scheme", json::Value(schemeToken(core::schemeOf(disk))));
    members.emplace_back("hasDiskData", json::Value(core::hasDiskData(disk)));
    members.emplace_back("usage", diskUsageValue(core::diskUsage(disk)));
    return json::Value::object(std::move(members));
}

}  // namespace

// ---------------------------------------------------------------------------
// Коды и справка
// ---------------------------------------------------------------------------

const char* toString(DisksExit code) noexcept {
    switch (code) {
        case DisksExit::Ok:
            return "ok";
        case DisksExit::Usage:
            return "usage";
        case DisksExit::InventoryUnavailable:
            return "inventory-unavailable";
        case DisksExit::Degraded:
            return "degraded";
        case DisksExit::Failed:
            return "failed";
    }
    return "unknown";
}

std::string disksUsageText() {
    return "Использование: mrproper-cli disks [опции]\n"
           "\n"
           "Печатает карту разделов: диски, разделы, тома, занятость, разряд разметки.\n"
           "Это эталонный дамп для сверки с `diskpart list disk` и Get-Disk (SPEC §8 Этап 1).\n"
           "JSON в stdout, разбор и сводка в stderr — stdout остаётся машинным.\n"
           "\n"
           "Опции:\n"
           "  --json                      JSON в stdout (по умолчанию)\n"
           "  --text                      текстовый экспорт карты вместо JSON (FR-2)\n"
           "  --compact                   JSON одной строкой (indent = -1); по умолчанию с отступами\n"
           "  --show-serials              не маскировать серийники и GUID томов (по умолчанию маскируются, §5)\n"
           "  --unallocated               включить неразмеченные промежутки как разделы (FR-2)\n"
           "  --no-space                  не спрашивать свободное место на томах (быстрее, но сумм занятости не будет)\n"
           "  --encryption                добавить раздел encryption: BitLocker через WMI (FR-1 п.7)\n"
           "  --device-timeout <длит>     таймаут на одно устройство (по умолчанию 2s, FR-1)\n"
           "  --volume-timeout <длит>     таймаут на перечисление всех томов (по умолчанию 5s)\n"
           "  --strict                    неполная карта (таймауты, отказы) → код возврата 4\n"
           "  --quiet                     не печатать ход обхода и сводку в stderr\n"
           "  -h, --help                  эта справка\n"
           "\n"
           "Длительность: 1500, 1500ms, 2s, 3m.\n"
           "Коды возврата: 0 — карта напечатана, 2 — ошибка аргументов,\n"
           "3 — обход не дал ни одного диска с данными, 4 — карта неполна (--strict), 5 — отказ на границе команды.\n"
           "\n"
           "Замечания: серийники и идентификаторы томов маскируются; --encryption поднимает WMI,\n"
           "который не имеет таймаута, поэтому выключен по умолчанию.\n";
}

bool parseDisksOptions(const std::vector<std::string>& args, DisksOptions& options, std::string& error) {
    options = DisksOptions{};
    error.clear();

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];

        if (argument == "-h" || argument == "--help") {
            options.help = true;
            return true;
        }
        if (argument == "--") {
            // Разделитель: после него позиционные аргументы, а команда их не
            // принимает. Молча проигнорировать их нельзя — скрипт с опечаткой
            // получил бы «успешную» пустую карту не того, что хотел.
            if (index + 1 < args.size()) {
                error = "команда disks не принимает позиционных аргументов: " + args[index + 1];
                return false;
            }
            return true;
        }

        std::string name;
        std::optional<std::string> inlineValue;
        if (!splitOption(argument, name, inlineValue)) {
            error = "неожиданный аргумент: " + argument;
            return false;
        }

        // Флаги значения не принимают: «--strict=1» — это опечатка, а не
        // «строгий режим», и молчать о ней нельзя.
        const auto rejectValue = [&error, &name](const std::optional<std::string>& value) {
            if (!value.has_value()) return true;
            error = "опция --" + name + " не принимает значение";
            return false;
        };
        // Значение опции: «--имя=значение» или следующий аргумент.
        const auto takeValue = [&args, &index, &error, &name](const std::optional<std::string>& inlineValueValue) {
            if (inlineValueValue.has_value()) return inlineValueValue;
            if (index + 1 >= args.size()) {
                error = "опции --" + name + " нужно значение";
                return std::optional<std::string>{};
            }
            ++index;
            return std::optional<std::string>(args[index]);
        };

        if (name == "json") {
            if (!rejectValue(inlineValue)) return false;
            options.json = true;
            options.text = false;
        } else if (name == "text") {
            if (!rejectValue(inlineValue)) return false;
            options.json = false;
            options.text = true;
        } else if (name == "compact") {
            if (!rejectValue(inlineValue)) return false;
            options.pretty = false;
        } else if (name == "show-serials") {
            if (!rejectValue(inlineValue)) return false;
            options.showSerials = true;
        } else if (name == "unallocated") {
            if (!rejectValue(inlineValue)) return false;
            options.includeUnallocated = true;
        } else if (name == "no-space") {
            if (!rejectValue(inlineValue)) return false;
            options.querySpace = false;
        } else if (name == "encryption") {
            if (!rejectValue(inlineValue)) return false;
            options.encryption = true;
        } else if (name == "strict") {
            if (!rejectValue(inlineValue)) return false;
            options.strict = true;
        } else if (name == "quiet") {
            if (!rejectValue(inlineValue)) return false;
            options.quiet = true;
        } else if (name == "device-timeout" || name == "volume-timeout") {
            const std::optional<std::string> value = takeValue(inlineValue);
            if (!value.has_value()) return false;
            std::chrono::milliseconds duration{};
            if (!parseDuration(*value, duration)) {
                error = "--" + name + ": нужно число миллисекунд или длительность вида 2s, 3m";
                return false;
            }
            if (name == "device-timeout") {
                options.deviceTimeout = duration;
            } else {
                options.volumeTimeout = duration;
            }
        } else {
            error = "неизвестная опция: --" + name;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Дамп
// ---------------------------------------------------------------------------

std::string formatDisksJson(const DisksInventory& inventory, const DisksOptions& options,
                            const std::vector<VolumeEncryption>& encryption) {
    const int indent = options.pretty ? 2 : -1;

    // Схема дисков, разделов и томов печатается ровно та, что у отчёта
    // (core::report_json): свой writer здесь означал бы вторую правду о тех же
    // данных, и два артефакта рано или поздно разошлись бы полями. Разбор
    // обратно в Value — способ добавить к документу разделы инвентаризации,
    // не заведя второго писателя.
    core::ReportOptions effective;
    effective.maskSerials = !options.showSerials;
    effective.maskVolumeGuids = !options.showSerials;

    const json::Value map = json::parse(core::partitionMapToJson(inventory.disks, effective, indent));

    // К дискам добавляем разряд разметки, признак «нет данных» и агрегат по
    // диску. Порядок элементов в массиве дисков менять нельзя: он задаёт
    // core::sortInventory, и перестановка сделала бы два прогона неразличимыми
    // только по счастливой случайности.
    std::vector<json::Value> diskValues;
    diskValues.reserve(inventory.disks.size());
    const json::Value& mapDisks = map.require("disks");
    if (mapDisks.isArray() && mapDisks.items().size() == inventory.disks.size()) {
        for (std::size_t index = 0; index < inventory.disks.size(); ++index) {
            diskValues.push_back(withDiskExtras(mapDisks.items()[index], inventory.disks[index]));
        }
    } else {
        // Расхождение размеров — не повод выбрасывать карту: печатаем как
        // пришла. Случиться это может только при чужом сборщике инвентаризации.
        diskValues = mapDisks.items();
    }

    std::vector<json::Value> unbound;
    unbound.reserve(inventory.unboundVolumes.size());
    for (const core::Volume& volume : inventory.unboundVolumes) {
        unbound.push_back(unboundVolumeValue(volume, effective.maskVolumeGuids));
    }

    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("schema", map.require("schema"));
    members.emplace_back("kind", map.require("kind"));
    members.emplace_back("command", json::Value("disks"));
    members.emplace_back("privacy", map.require("privacy"));
    members.emplace_back("totals", map.require("totals"));
    members.emplace_back("usage", usageValue(core::inventoryUsage(inventory.disks)));
    members.emplace_back("consistency", consistencyValue(inventory.disks));
    members.emplace_back("inventory", inventoryValue(inventory));
    members.emplace_back("unboundVolumes", json::Value::array(std::move(unbound)));
    // Раздел шифрования печатается только когда его наполнили: пустой массив
    // означал бы «томов не зашифровано ни одного», а не «не спрашивали».
    if (!encryption.empty()) members.emplace_back("encryption", encryptionValue(encryption));
    members.emplace_back("disks", json::Value::array(std::move(diskValues)));

    std::string text = json::Value::object(std::move(members)).dump(indent);
    text.push_back('\n');
    return text;
}

std::string formatDisksText(const DisksInventory& inventory, const DisksOptions& options,
                            const std::vector<VolumeEncryption>& encryption) {
    // Карту печатает core::disk_model: тот же текст, что в приложении и в
    // баг-репортах, и агрегаты в нём не пересчитываются здесь второй раз.
    std::string text = core::toText(inventory.disks);

    text += "Сбор карты:\n";
    text += "  обход: " + std::to_string(inventory.durationMs) + " мс";
    text += ", диски: " + std::string(inventory.disksComplete ? "перечислены все" : "перечисление прервано");
    text += ", тома: " + std::string(inventory.volumesComplete ? "перечислены все" : "перечисление прервано");
    text += "\n";
    text += "  устройств без ответа за таймаут: " + std::to_string(inventory.timedOutDevices) + "\n";
    text += "  устройств без данных: " + std::to_string(inventory.unavailableDevices) + "\n";
    if (inventory.degradedReasons.empty()) {
        text += "  карта полная\n";
    } else {
        text += "  карта НЕПОЛНАЯ:\n";
        for (const std::string& reason : inventory.degradedReasons) text += "    - " + reason + "\n";
    }

    if (!inventory.issues.empty()) {
        text += "Замечания обхода (" + std::to_string(inventory.issues.size()) + "):\n";
        for (const std::string& issue : inventory.issues) text += "  - " + issue + "\n";
    }

    if (!inventory.unboundVolumes.empty()) {
        text += "Тома вне разделов (" + std::to_string(inventory.unboundVolumes.size()) +
                "): динамический диск, RAID или отказ привязки (FR-1 п.5)\n";
        for (const core::Volume& volume : inventory.unboundVolumes) {
            // Том без точки монтирования печатается по пути \\?\Volume{…}, то
            // есть по GUID. Маскирование серийников и идентификаторов томов
            // включено по умолчанию (SPEC §5), и текстовый экспорт не должен
            // быть исключением из него.
            core::Volume printable = volume;
            if (!options.showSerials) printable.volumeGuidPath = core::maskVolumeGuid(printable.volumeGuidPath);
            text += "  " + core::describeVolume(printable) + "\n";
        }
    }

    if (!encryption.empty()) {
        text += "Шифрование томов (FR-1 п.7):\n";
        for (const VolumeEncryption& item : encryption) {
            text += "  " + (item.driveLetter.empty() ? std::string("(без буквы)") : item.driveLetter);
            text += ": " + item.state;
            if (!item.method.empty()) text += ", " + item.method;
            if (!item.source.empty()) text += ", источник " + item.source;
            if (!item.detail.empty()) text += " (" + item.detail + ")";
            text += "\n";
        }
    }

    return text;
}

// ---------------------------------------------------------------------------
// Обход инвентаризации поверх platform
// ---------------------------------------------------------------------------

namespace {

// Замечание обхода в одну строку: «уровень: этап: сообщение [win32=0x…]».
// Формат строковый и русский — это stderr и текстовый экспорт для баг-репорта,
// а не машинный разбор (машинные коды лежат в разделе issues JSON как строки
// ровно в этом виде, а HRESULT — отдельным числом в самом снимке платформы).
[[nodiscard]] std::string formatIssue(const platform::inventory::Issue& issue) {
    std::string text(platform::inventory::issueLevelName(issue.level));
    text += ": ";
    text += platform::inventory::stageName(issue.stage);
    text += ": ";
    text += issue.message;
    if (issue.win32Error != 0) {
        char code[16] = {};
        std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned int>(issue.win32Error));
        text += " [win32=";
        text += code;
        text += "]";
    }
    return text;
}

[[nodiscard]] std::int64_t toUnixSeconds(std::chrono::system_clock::time_point moment) noexcept {
    if (moment.time_since_epoch().count() == 0) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(moment.time_since_epoch()).count();
}

}  // namespace

DisksServices makePlatformDisksServices() {
    DisksServices services;

    // Один обход FR-1 целиком. Опции сбора берём из разобранной командной
    // строки: таймаут на устройство и includeUnallocated меняют СОДЕРЖИМОЕ
    // карты, и собирать её с настройками по умолчанию, когда скрипт попросил
    // другие, — это отвечать не на тот вопрос.
    services.collect = [](const DisksOptions& options) -> DisksInventory {
        platform::inventory::Options collectOptions;
        collectOptions.deviceTimeout = options.deviceTimeout;
        collectOptions.volumeEnumerationTimeout = options.volumeTimeout;
        collectOptions.includeUnallocated = options.includeUnallocated;
        collectOptions.queryVolumeSpace = options.querySpace;

        const std::shared_ptr<const platform::inventory::Snapshot> snapshot =
            platform::inventory::collect(collectOptions, platform::inventory::RefreshReason::Manual);

        DisksInventory result;
        if (!snapshot) return result;

        // Копия, а не ссылка на снимок: наружу отдаётся переносимая структура,
        // а платформенный снимок живёт внутри статической библиотеки.
        result.disks = snapshot->inventory.disks();
        result.unboundVolumes = snapshot->unboundVolumes;
        for (const platform::inventory::Issue& issue : snapshot->issues) result.issues.push_back(formatIssue(issue));
        result.degradedReasons = snapshot->degradedReasons;
        result.timedOutDevices = snapshot->timedOutCount();
        result.unavailableDevices = snapshot->unavailableCount();
        result.durationMs = static_cast<std::uint64_t>(snapshot->duration.count());
        result.collectedAtUnix = toUnixSeconds(snapshot->collectedAt);
        result.disksComplete = snapshot->disksComplete;
        result.volumesComplete = snapshot->volumesComplete;
        return result;
    };

    // FR-1 п.7. Один запрос на все тома (Win32_EncryptVolume отдаёт по строке на
    // том). Метод noexcept и сам разбирает свои отказы, поэтому наружу приходит
    // либо список, либо пустой список с detail в каждой записи.
    services.queryEncryption = []() -> std::vector<VolumeEncryption> {
        std::vector<VolumeEncryption> result;
        for (const platform::wmi::VolumeEncryption& item : platform::wmi::queryVolumeEncryption()) {
            VolumeEncryption entry;
            entry.driveLetter = item.driveLetter;
            entry.state = platform::wmi::encryptionStateName(item.state);
            entry.method = item.encryptionMethod;
            const std::string_view source = platform::wmi::encryptionSourceName(item.source);
            entry.source = source == "unknown" ? std::string{} : std::string(source);
            entry.protectedVolume = item.protection == platform::wmi::ProtectionState::Enabled;
            entry.known = item.known();
            entry.detail = item.detail;
            result.push_back(std::move(entry));
        }
        return result;
    };

    return services;
}

// ---------------------------------------------------------------------------
// Запуск команды
// ---------------------------------------------------------------------------

int runDisks(const std::vector<std::string>& args, const DisksServices& services, std::ostream& out,
             std::ostream& err) {
    DisksOptions options;
    std::string error;
    if (!parseDisksOptions(args, options, error)) {
        err << "[disks] " << error << "\n[disks] " << toString(DisksExit::Usage) << " — справка: mrproper-cli disks --help\n";
        return static_cast<int>(DisksExit::Usage);
    }
    if (options.help) {
        err << disksUsageText();
        return static_cast<int>(DisksExit::Ok);
    }
    if (!services.collect) {
        err << "[disks] не передан обход инвентаризации (DisksServices::collect): печатать нечего.\n"
               "[disks] Соберите сервисы через makePlatformDisksServices() — он и есть мост к platform::inventory.\n";
        return static_cast<int>(DisksExit::InventoryUnavailable);
    }

    core::LogFields startFields;
    startFields.push_back(core::logField("deviceTimeoutMs", options.deviceTimeout.count()));
    startFields.push_back(core::logField("volumeTimeoutMs", options.volumeTimeout.count()));
    startFields.push_back(core::logField("text", options.text));
    startFields.push_back(core::logField("showSerials", options.showSerials));
    core::logInfo(kEventStart, "сбор карты разделов запущен", startFields);

    if (!options.quiet) {
        err << "[disks] обход инвентаризации: таймаут на устройство " << options.deviceTimeout.count() << " мс, на тома "
            << options.volumeTimeout.count() << " мс\n";
        if (!services.appVersion.empty()) err << "[disks] mrproper-cli " << services.appVersion << "\n";
    }

    // Обход не бросает наружу, но печать большого дампа может не хватить памяти,
    // а WMI при ответе не забывает упасть. Граница команды ловит и то и другое
    // и превращает в код возврата с текстом в stderr.
    DisksInventory inventory;
    try {
        inventory = services.collect(options);
    } catch (const std::exception& failure) {
        err << "[disks] обход инвентаризации не удался: " << failure.what() << "\n"
            << "[disks] " << toString(DisksExit::Failed) << " — карта не напечатана: печатать нечего, и подменять её\n"
               "[disks] пустым объектом значило бы выдать «дисков нет».\n";
        return static_cast<int>(DisksExit::Failed);
    } catch (...) {
        err << "[disks] обход инвентаризации упал с неизвестным отказом\n"
            << "[disks] " << toString(DisksExit::Failed) << " — карта не напечатана.\n";
        return static_cast<int>(DisksExit::Failed);
    }

    std::vector<VolumeEncryption> encryption;
    if (options.encryption) {
        if (!services.queryEncryption) {
            // Просили, а спросить нечем. Молча пропустить раздел нельзя: в
            // дампе было бы написано «томов не зашифровано», а на самом деле
            // «не спрошено» (FR-1 п.7, §5 приватность).
            err << "[disks] --encryption: запрос BitLocker не передан (DisksServices::queryEncryption), раздела "
                   "encryption в дампе не будет\n";
        } else {
            try {
                encryption = services.queryEncryption();
            } catch (const std::exception& failure) {
                err << "[disks] --encryption: запрос BitLocker не удался: " << failure.what()
                    << " (FR-1 п.7: WMI без таймаута, поэтому раздел может отсутствовать)\n";
            } catch (...) {
                err << "[disks] --encryption: запрос BitLocker упал с неизвестным отказом\n";
            }
        }
    }

    // Дамп печатается всегда, даже когда карта пустая или неполная: в JSON
    // должны быть видны и таймауты, и «ни одного диска», иначе скрипт увидит
    // пустой stdout и решит, что команда отработала вхолостую.
    try {
        out << (options.text ? formatDisksText(inventory, options, encryption)
                             : formatDisksJson(inventory, options, encryption));
    } catch (const std::exception& failure) {
        err << "[disks] печать дампа не удалась: " << failure.what() << "\n"
            << "[disks] " << toString(DisksExit::Failed) << " — вывод в stdout оборван, не читайте его как валидный JSON.\n";
        return static_cast<int>(DisksExit::Failed);
    }
    out.flush();

    const core::InventoryUsage usage = core::inventoryUsage(inventory.disks);
    const std::size_t disksWithData = static_cast<std::size_t>(std::count_if(
        inventory.disks.begin(), inventory.disks.end(),
        [](const core::PhysicalDisk& disk) { return core::hasDiskData(disk); }));
    const bool degraded = isDegraded(inventory);

    core::LogFields finishFields;
    finishFields.push_back(core::logField("diskCount", usage.diskCount));
    finishFields.push_back(core::logField("disksWithData", disksWithData));
    finishFields.push_back(core::logField("partitionCount", usage.partitionCount));
    finishFields.push_back(core::logField("volumeCount", usage.volumeCount));
    finishFields.push_back(core::logField("durationMs", inventory.durationMs));
    finishFields.push_back(core::logField("timedOutDevices", inventory.timedOutDevices));
    finishFields.push_back(core::logField("unavailableDevices", inventory.unavailableDevices));
    finishFields.push_back(core::logField("degraded", degraded));
    core::logInfo(kEventFinish, "карта разделов собрана", finishFields);

    if (!options.quiet) {
        err << "[disks] дисков: " << usage.diskCount << " (с данными: " << disksWithData << "), разделов: "
            << usage.partitionCount << ", томов: " << usage.volumeCount << ", обход: " << inventory.durationMs << " мс\n";
    }

    if (degraded) {
        if (!options.quiet) {
            err << "[disks] ВНИМАНИЕ: карта неполная";
            if (inventory.timedOutDevices > 0) {
                err << "; не ответили за таймаут: " << inventory.timedOutDevices;
            }
            if (inventory.unavailableDevices > 0) {
                err << "; без данных: " << inventory.unavailableDevices;
            }
            if (!inventory.disksComplete) err << "; перечисление дисков прервано";
            if (!inventory.volumesComplete) err << "; перечисление томов прервано";
            if (!inventory.issues.empty()) err << "; замечаний: " << inventory.issues.size();
            err << "\n";
        }
        // Замечания печатаются и под --quiet: это ошибки обхода, а не прогресс.
        // Список обрезан — 200 отказов от одного устройства в консоли не читаются,
        // а полный список всё равно в JSON и в --text.
        const std::size_t shown = (std::min)(inventory.issues.size(), kMaxIssuesInStderr);
        for (std::size_t index = 0; index < shown; ++index) err << "[disks] " << inventory.issues[index] << "\n";
        if (shown < inventory.issues.size()) {
            err << "[disks] … ещё замечаний в дампе: " << (inventory.issues.size() - shown) << "\n";
        }
        core::LogFields degradedFields;
        degradedFields.push_back(core::logField("reasons", static_cast<std::uint64_t>(inventory.degradedReasons.size())));
        degradedFields.push_back(core::logField("issues", static_cast<std::uint64_t>(inventory.issues.size())));
        core::logWarn(kEventDegraded, "карта разделов неполна", degradedFields);

        // «Ничего не собрано» важнее, чем «неполно», поэтому --strict уступает
        // коду 3: скрипту полезнее узнать, что данных нет вовсе, чем что их
        // мало. Сравнение с эталоном при пустой карте бессмысленно.
        if (options.strict && disksWithData > 0) {
            if (!options.quiet) {
                err << "[disks] " << toString(DisksExit::Degraded) << " — --strict: неполная карта (SPEC §8 Этап 1: "
                << "расхождения с эталоном считаются, а не прощаются)\n";
            }
            return static_cast<int>(DisksExit::Degraded);
        }
    }

    if (disksWithData == 0) {
        if (!options.quiet) {
            err << "[disks] " << toString(DisksExit::InventoryUnavailable) << " — ни об одном диске нет данных: размер "
                << "или разделы не получены.\n"
                << "[disks] Дамп напечатан: в нём видно, что именно не ответило (FR-1: устройство помечается "
                   "недоступным, а не выбрасывается).\n";
        }
        return static_cast<int>(DisksExit::InventoryUnavailable);
    }

    if (!options.quiet && core::hasErrors(core::validateInventory(inventory.disks))) {
        err << "[disks] ВНИМАНИЕ: в карте есть противоречия (раздел consistency в JSON, коды вида "
               "volume.free_exceeds_total)\n";
    }
    return static_cast<int>(DisksExit::Ok);
}

}  // namespace mrproper::cli
