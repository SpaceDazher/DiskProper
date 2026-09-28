// Юнит-тесты отчётов MrProper: core::report_json и core::report_html.
//
// Задача 80; спека §4 FR-8 (экспорт HTML/JSON/текст, состав отчёта), §5
// «Приватность: отчёт содержит серийники — пользователь может исключить их
// перед отправкой», §9 tests/unit — «core, не требует Windows».
//
// Три свойства, вокруг которых построен весь файл, — и все три бьют по
// пользователю, а не по тесту:
//
//   1. ДЕТЕРМИЗМ. Отчёт идёт в баг-репорт и в diff: «тот же вход → побайтово
//      тот же файл» обязано быть правдой, иначе завтрашний golden-тест начнёт
//      мигать, а Reviewer перестанет доверять разделу «Приватность». Никаких
//      часов системы, локали и случайности: всё время приходит полем. Проверяем
//      трижды — два вызова подряд совпадают, заново собранный равный вход даёт
//      тот же файл, и разобранный обратно документ сериализуется так же
//      (round-trip не плодит «микроразницы»).
//   2. ЭКРАНИРОВАНИЕ. В отчёт попадают пути, имена дисков и текст ошибок
//      платформы — то есть произвольные строки. JSON обязан вернуть их
//      посимвольно (проверяем round-trip через json::parse), HTML обязан не
//      дать им закрыть тег (проверяем, что «<script>» из данных в разметке не
//      появился, а появился &lt;script&gt;).
//   3. МАСКИРОВАНИЕ СЕРИЙНИКОВ. Приватность по умолчанию: серийник не должен
//      уехать в баг-репорт открытым текстом ни в JSON, ни в HTML, пока человек
//      не попросил обратное явно. Проверяем и саму маску, и то, что с
//      выключенной маской значение возвращается целиком, и то, что отчёт сам
//      объявляет маскирование (privacy.serialsMasked и подпись в подвале).
//
// Модули переносимые (SPEC §6.1): здесь нет ни Windows API, ни ввода-вывода,
// ни файловой системы — тест держит весь вход в памяти и сравнивает строки.
#include "harness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"
#include "report_html.hpp"
#include "report_json.hpp"

using mrproper::json::Value;
using namespace mrproper::core;

namespace {

// ------------------------------------------------------------------ помощники

bool has(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

std::size_t occurrences(const std::string& hay, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos <= hay.size()) {
        const std::size_t found = hay.find(needle, pos);
        if (found == std::string::npos) break;
        ++count;
        pos = found + 1;
    }
    return count;
}

// Знаки (кодовые точки) UTF-8-строки. Символ маски серийника «•» занимает три
// байта, поэтому длина в байтах со списком значимых знаков серийника не
// сравнивается: сравнивать надо знаки.
std::size_t charCount(std::string_view text) {
    std::size_t count = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) ++count;
    }
    return count;
}

bool containsAny(const std::vector<std::string>& haystack, std::string_view needle) {
    for (const std::string& item : haystack) {
        if (item.find(needle) != std::string::npos) return true;
    }
    return false;
}

// N символов маски серийника по умолчанию подряд.
std::string maskRun(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) out += kHtmlSerialMaskChar;
    return out;
}

// Секция верхнего уровня в разобранном отчёте.
const Value& section(const Value& root, std::string_view key) { return root.require(key); }

// ------------------------------------------------------------- образцы данных

// Байты в том порядке, в каком их пишет платформа: перестановку делает владелец
// данных (diskioctls пишет GUID в смешанном порядке), тест фиксирует именно эту
// договорённость и не «чинит» её за него.
Guid guidFromHex(std::string_view hex) {
    Guid guid{};
    for (std::size_t i = 0; i < guid.size(); ++i) {
        guid[i] = static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i * 2, 2)), nullptr, 16));
    }
    return guid;
}

const Guid kGptBasicData = guidFromHex("EBD0A0A2A5A8ABAEB1B4B7BABDC0C3C6");

PhysicalDisk makeSampleDisk() {
    PhysicalDisk disk;
    disk.number = 0;
    disk.model = "Samsung SSD 990 PRO 2TB";
    disk.serial = "S6Y2NJ0T512345";
    disk.firmware = "2B2QGXE7";
    disk.bus = BusType::Nvme;
    disk.sizeBytes = 2000000000000ULL;
    disk.trimSupported = true;
    disk.smartAvailable = true;
    disk.devicePath = "\\\\?\\#disk&ven_samsung&prod_nvme#4&1a2b3c4d&0&000000";

    Partition system;
    system.index = 0;
    system.offsetBytes = 1048576ULL;
    system.lengthBytes = 53687091200ULL;
    system.kind = PartitionKind::System;
    system.system = true;
    system.boot = true;
    system.hasVolume = false;

    Partition data;
    data.index = 1;
    data.offsetBytes = 53688145920ULL;
    data.lengthBytes = 1000000000000ULL;
    data.kind = PartitionKind::BasicData;
    data.gptType = kGptBasicData;
    data.hasGptType = true;
    data.gptName = "Windows";
    data.hasVolume = true;
    data.volume.volumeGuidPath = "\\\\?\\Volume{6f1b2c3d-1111-2222-3333-444455556666}\\";
    data.volume.label = "Система";
    data.volume.fileSystem = "NTFS";
    data.volume.totalBytes = 900000000000ULL;
    data.volume.freeBytes = 300000000000ULL;
    data.volume.mountPoints.push_back("C:\\");
    data.volume.diskExtents.emplace_back(0, 53688145920ULL);

    Partition recovery;
    recovery.index = 2;
    recovery.offsetBytes = 1053881830400ULL;
    recovery.lengthBytes = 1073741824ULL;
    recovery.kind = PartitionKind::Recovery;
    recovery.hasVolume = false;

    disk.partitions.push_back(system);
    disk.partitions.push_back(data);
    disk.partitions.push_back(recovery);
    return disk;
}

CleanupCandidate makeCandidate(std::string ruleId, std::string category, std::string path, std::string displayName,
                               std::uint64_t allocated, SafetyLevel safety, int confidence) {
    CleanupCandidate candidate;
    candidate.ruleId = std::move(ruleId);
    candidate.category = std::move(category);
    candidate.path = std::move(path);
    candidate.displayName = std::move(displayName);
    candidate.logicalBytes = allocated + 1000000ULL;
    candidate.allocatedBytes = allocated;
    candidate.fileCount = 120;
    candidate.oldestWrite = 1780000000;
    candidate.newestWrite = 1790000000;
    candidate.lastAccess = 1790000000;
    candidate.safety = safety;
    candidate.confidence = confidence;
    candidate.reasons.push_back("старше 7 дней");
    return candidate;
}

std::vector<CleanupCandidate> makeSampleCandidates() {
    std::vector<CleanupCandidate> candidates;
    candidates.push_back(makeCandidate("temp.old", "Temp", "C:\\Users\\Daniil\\AppData\\Local\\Temp\\",
                                       "Временные файлы", 4000000ULL, SafetyLevel::Safe, 95));
    candidates.push_back(makeCandidate("browser.cache", "Browser", "C:\\Chrome\\User Data\\Default\\Cache\\",
                                       "Кэш браузера", 2500000ULL, SafetyLevel::Review, 80));
    candidates.back().lockedBy.push_back(ProcessRef{1234, "chrome.exe"});
    candidates.push_back(makeCandidate("logs.old", "Logs", "C:\\Windows\\Logs\\old\\", "Старые журналы", 1000000ULL,
                                       SafetyLevel::Safe, 90));
    return candidates;
}

ReportOperation makeOperation(std::size_t candidateIndex, PlanAction action, ReportOperationStatus status,
                              std::string category, std::string displayName, std::string path, std::uint64_t bytes,
                              std::uint32_t attempts, std::string transactionId) {
    ReportOperation op;
    op.candidateIndex = candidateIndex;
    op.action = action;
    op.status = status;
    op.category = std::move(category);
    op.displayName = std::move(displayName);
    op.path = std::move(path);
    op.bytes = bytes;
    op.safety = SafetyLevel::Safe;
    op.confidence = 95;
    op.attempts = attempts;
    op.transactionId = std::move(transactionId);
    op.startedAtUnix = 1790000000;
    op.finishedAtUnix = 1790000010;
    return op;
}

// Согласованный отчёт: validateReport на нём молчит. Все проверки «ломаных»
// отчётов стартуют с копии этого образца и портят ровно один инвариант.
Report makeSampleReport() {
    Report report;
    report.kind = ReportKind::Cleanup;
    report.environment.appVersion = "1.0.0";
    report.environment.pid = 4242;
    report.environment.rulesVersion = "2026.09.1";
    report.environment.osCaption = "Microsoft Windows 11 Pro";
    report.environment.osVersion = "10.0.22631";
    report.environment.osBuild = 22631;
    report.environment.architecture = "x64";
    report.timing.startedAtUnix = 1790000000;
    report.timing.finishedAtUnix = 1790000042;
    report.timing.durationMs = 42000;
    report.notes = "ручная очистка";
    report.disks.push_back(makeSampleDisk());
    report.candidates = makeSampleCandidates();

    report.operations.push_back(makeOperation(0, PlanAction::Delete, ReportOperationStatus::Success, "Temp",
                                              "Временные файлы", "C:\\Users\\Daniil\\AppData\\Local\\Temp\\", 4000000ULL,
                                              1, ""));
    report.operations.push_back(makeOperation(1, PlanAction::Trash, ReportOperationStatus::Partial, "Browser",
                                              "Кэш браузера", "C:\\Chrome\\User Data\\Default\\Cache\\", 1000000ULL, 2,
                                              "tx-0001"));
    report.operations.back().detail = "0x00000005";
    report.operations.push_back(makeOperation(2, PlanAction::SkipLocked, ReportOperationStatus::Skipped, "Logs",
                                              "Старые журналы", "C:\\Windows\\Logs\\old\\", 0, 1, ""));
    report.operations.back().detail = "held by MpDefender";

    report.untouched.push_back(makeOperation(1, PlanAction::Keep, ReportOperationStatus::Skipped, "Browser",
                                             "Кэш браузера", "C:\\Chrome\\User Data\\Default\\Cache\\", 0, 1, ""));

    ReportError denied;
    denied.scope = "execute";
    denied.code = "0x80070005";
    denied.message = "Отказано в доступе";
    denied.path = "C:\\Windows\\Temp\\locked.tmp";
    denied.operation = "delete";
    denied.atUnix = 1790000015;
    denied.count = 3;
    report.errors.push_back(denied);

    ReportError noSpace;
    noSpace.scope = "trash";
    noSpace.code = "tx-disk-full";
    noSpace.message = "Нет места в корзине";
    noSpace.operation = "move";
    noSpace.atUnix = 1790000020;
    noSpace.count = 1;
    report.errors.push_back(noSpace);

    return report;
}

HtmlReportInput makeHtmlInput() {
    HtmlReportInput in;
    in.meta.appVersion = "1.0.0";
    in.meta.appBuild = "Debug";
    in.meta.osName = "Windows 11 Pro";
    in.meta.osVersion = "10.0.22631";
    in.meta.hostName = "WORKSTATION";
    in.meta.userName = "Daniil";
    in.meta.localeTag = "ru-RU";
    in.generatedAtUnix = 1790000042;
    in.startedAtUnix = 1790000000;
    in.finishedAtUnix = 1790000042;
    in.dryRun = false;
    in.plannedBytes = 7500000ULL;
    in.freedBytes = 5000000ULL;
    in.transactionId = "tx-0001";
    in.disks.push_back(makeSampleDisk());
    in.candidates = makeSampleCandidates();
    in.notes.push_back("Запущен после повторной копии");
    return in;
}

HtmlReportOperation makeHtmlOperation(PlanAction action, std::string name, std::string path, std::uint64_t bytes,
                                      bool success, bool skipped, std::string error, std::int64_t durationMs) {
    HtmlReportOperation op;
    op.action = action;
    op.name = std::move(name);
    op.path = std::move(path);
    op.bytes = bytes;
    op.safety = SafetyLevel::Safe;
    op.confidence = 95;
    op.success = success;
    op.skipped = skipped;
    op.error = std::move(error);
    op.durationMs = durationMs;
    return op;
}

}  // namespace

// =====================================================================
// core::report_json: маскирование
// =====================================================================

TEST(reportJson_maskSerialKeepsTailOfFour) {
    // Хвост из 4 знаков — контракт kSerialTailKept: по нему владелец железа
    // узнаёт свой диск, а по остатку серийника его не восстановить.
    // Маска не короче и не длиннее исходника: на каждый закрытый знак — ровно
    // одна звёздочка, иначе длина отчёта по серийникам «плыла» бы.
    const std::string masked = maskSerial("S6Y2NJ0T512345");
    CHECK_EQ(masked, std::string("**********2345"));
    CHECK_EQ(masked.size(), std::string("S6Y2NJ0T512345").size());
    CHECK(has(masked, "2345"));
    CHECK(!has(masked, "S6Y2"));
    CHECK(!has(masked, "6Y2NJ0T5"));
}

TEST(reportJson_maskSerialHidesShortAndEmptySerials) {
    // Нет диска — нет серийника, а не «****»: пустая строка остаётся пустой.
    CHECK_EQ(maskSerial(""), std::string(""));
    // Короткий серийник не показываем целиком — иначе маскировка показывает
    // больше, чем скрывает.
    CHECK_EQ(maskSerial("ABC"), std::string("***"));
    CHECK_EQ(maskSerial("1234"), std::string("****"));
    CHECK_EQ(maskSerial("A"), std::string("*"));
    // Ровно четыре знака — тоже целиком: такой «хвост» неотличим от серийника.
    CHECK_EQ(maskSerial("WXYZ"), std::string("****"));
    CHECK_EQ(maskSerial("WX"), std::string("**"));
}

TEST(reportJson_maskVolumeGuidKeepsPathShape) {
    // Идентификатор тома не читается, но путь в отчёте остаётся копируемым:
    // хвостовой разделитель теряться не должен.
    CHECK_EQ(maskVolumeGuid("\\\\?\\Volume{6f1b2c3d-1111-2222-3333-444455556666}\\"),
             std::string("\\\\?\\Volume{****}\\"));
    CHECK_EQ(maskVolumeGuid("\\\\?\\Volume{6f1b2c3d-1111-2222-3333-444455556666}"),
             std::string("\\\\?\\Volume{****}"));
    // Не-GUID пути не трогаем: маскировать нечего, а испорченный путь хуже.
    CHECK_EQ(maskVolumeGuid("C:\\"), std::string("C:\\"));
    CHECK_EQ(maskVolumeGuid("D:\\Data"), std::string("D:\\Data"));
    CHECK_EQ(maskVolumeGuid(""), std::string(""));
    // Незакрытая скобка — тоже не GUID: молча «замаскированный» путь вводил бы
    // в заблуждение того, кто чинит по отчёту.
    CHECK_EQ(maskVolumeGuid("\\\\?\\Volume{abc"), std::string("\\\\?\\Volume{abc"));
}

TEST(reportJson_guidToStringIsCanonicalLowercase) {
    // Порядок байт — как записала платформа; дефисы на местах 8-4-4-4-12,
    // регистр всегда нижний (SPEC §9 — один канонический вид на оба артефакта).
    CHECK_EQ(guidToString(kGptBasicData), std::string("ebd0a0a2-a5a8-abae-b1b4-b7babdc0c3c6"));

    const Guid zero{};
    CHECK_EQ(guidToString(zero), std::string("00000000-0000-0000-0000-000000000000"));
    CHECK_EQ(guidToString(zero).size(), static_cast<std::size_t>(36));
    CHECK_EQ(occurrences(guidToString(zero), "-"), static_cast<std::size_t>(4));
    // Байты 0xAB и 0xAE не переставляются: вывод — ровно тот порядок, в каком
    // GUID записала платформа (report_json.cpp, guidToString).
    CHECK_EQ(guidToString(guidFromHex("AABBCCDDEEFF00112233445566778899")),
             std::string("aabbccdd-eeff-0011-2233-445566778899"));
}

TEST(reportJson_reportTokensAreStable) {
    // Схему читают скрипты: переименование токена ломает их молча.
    CHECK_EQ(std::string(toString(ReportKind::Scan)), std::string("scan"));
    CHECK_EQ(std::string(toString(ReportKind::Cleanup)), std::string("cleanup"));
    CHECK_EQ(std::string(toString(ReportKind::DryRun)), std::string("dry-run"));
    CHECK_EQ(std::string(toString(ReportOperationStatus::Success)), std::string("success"));
    CHECK_EQ(std::string(toString(ReportOperationStatus::Failed)), std::string("failed"));
    CHECK_EQ(std::string(toString(ReportOperationStatus::Skipped)), std::string("skipped"));
    CHECK_EQ(std::string(toString(ReportOperationStatus::Partial)), std::string("partial"));
}

// =====================================================================
// core::report_json: детерминизм
// =====================================================================

TEST(reportJson_isByteForByteDeterministic) {
    const Report report = makeSampleReport();
    const std::string first = reportToJson(report, 2);
    const std::string second = reportToJson(report, 2);
    CHECK(!first.empty());
    CHECK_EQ(first, second);

    // Заново собранный, но равный по содержимому отчёт даёт тот же файл:
    // детерминизм не должен зависеть от того, один ли это объект.
    CHECK_EQ(reportToJson(makeSampleReport(), 2), first);
}

TEST(reportJson_endsWithNewlineAndHasFixedKeyOrder) {
    const std::string text = reportToJson(makeSampleReport(), 2);
    CHECK(!text.empty());
    CHECK_EQ(text.back(), '\n');
    // Порядок ключей — контракт golden-теста: перестановка «ничего не значит»
    // для человека и ломает diff у того, кто сравнивает два отчёта.
    const Value root = mrproper::json::parse(text);
    const std::vector<std::string> expected{"schema",    "kind",   "app",        "os",         "timing",
                                            "privacy",   "totals", "disks",      "candidates", "operations",
                                            "untouched", "errors", "notes"};
    const std::vector<std::pair<std::string, Value>>& members = root.members();
    CHECK_EQ(members.size(), expected.size());
    for (std::size_t i = 0; i < expected.size() && i < members.size(); ++i) {
        CHECK_EQ(members[i].first, expected[i]);
    }
    CHECK_EQ(root.require("schema").asNumber(), static_cast<double>(kReportJsonSchema));
    CHECK_EQ(std::string(root.require("kind").asString()), std::string("cleanup"));
}

TEST(reportJson_indentOnlyChangesLayout) {
    const Report report = makeSampleReport();
    const std::string pretty = reportToJson(report, 2);
    const std::string compact = reportToJson(report, -1);
    const std::string wide = reportToJson(report, 4);

    // Данные те же — отличается только отступ. Сравниваем разобранное, а не
    // текст: иначе тест повторял бы реализацию dump(), а не контракт.
    const Value prettyValue = mrproper::json::parse(pretty);
    CHECK_EQ(mrproper::json::parse(compact).dump(2), prettyValue.dump(2));
    CHECK_EQ(mrproper::json::parse(wide).dump(2), prettyValue.dump(2));

    // Компактный вывод — одна строка плюс завершающий перевод строки: файл без
    // перевода ломает вывод в консоли и половину «показать diff» в CI.
    CHECK_EQ(occurrences(compact, "\n"), static_cast<std::size_t>(1));
    CHECK_EQ(compact.back(), '\n');
    CHECK(occurrences(pretty, "\n") > static_cast<std::size_t>(10));
}

TEST(reportJson_roundTripIsStable) {
    // Разбор собственного вывода и повторная сериализация не должны «мигать»:
    // именно эту операцию делает скрипт баг-репортов, сравнивая два отчёта.
    const std::string once = reportToJson(makeSampleReport(), 2);
    const std::string twice = mrproper::json::parse(once).dump(2) + "\n";
    const std::string thrice = mrproper::json::parse(twice).dump(2) + "\n";
    CHECK_EQ(twice, once);
    CHECK_EQ(thrice, twice);
}

TEST(reportJson_partitionMapIsDeterministicAndSharesDiskSchema) {
    const std::vector<PhysicalDisk> disks{makeSampleDisk()};
    const std::string first = partitionMapToJson(disks, ReportOptions{}, 2);
    const std::string second = partitionMapToJson(disks, ReportOptions{}, 2);
    CHECK_EQ(first, second);
    CHECK_EQ(first.back(), '\n');

    const Value map = mrproper::json::parse(first);
    CHECK_EQ(std::string(map.require("kind").asString()), std::string("disk-map"));
    CHECK_EQ(map.require("schema").asNumber(), static_cast<double>(kReportJsonSchema));

    // Схема диска в карте и в отчёте одна: два артефакта о дисках не должны
    // разойтись полями (FR-2 «экспорт карты в JSON»).
    const Value report = mrproper::json::parse(reportToJson(makeSampleReport(), 2));
    const Value mapDisk = section(map, "disks").items().at(0);
    const Value reportDisk = section(report, "disks").items().at(0);
    CHECK_EQ(mapDisk.members().size(), reportDisk.members().size());
    for (std::size_t i = 0; i < mapDisk.members().size(); ++i) {
        CHECK_EQ(mapDisk.members()[i].first, reportDisk.members()[i].first);
    }
    // Итоги по дискам совпадают с итогами полного отчёта.
    CHECK_EQ(section(map, "totals").require("diskCount").asNumber(),
             section(report, "totals").require("diskCount").asNumber());
    CHECK_EQ(section(map, "totals").require("volumeBytes").asNumber(),
             section(report, "totals").require("volumeBytes").asNumber());
}

// =====================================================================
// core::report_json: экранирование и приватность
// =====================================================================

TEST(reportJson_survivesHostileStringsUnchanged) {
    // Пути Windows — худший случай для строкового формата: обратный слэш,
    // кавычка, табуляция, перевод строки, управляющий символ и кириллица.
    Report report = makeSampleReport();
    std::string hostile = "C:\\Temp\\\"кавычки\"\\файл\t";
    hostile.push_back('\n');
    hostile += "вторая строка ";
    hostile.push_back('\x01');
    hostile += "хвост";
    report.candidates[0].path = hostile;
    report.candidates[0].displayName = "<b>жирный</b> & \\u0442\\u0435\\u0433";
    report.notes = "{\"looks\":\"like json\"}";
    report.errors[0].message = "ошибка\tс\tтабами\nи переводом строки";

    const std::string text = reportToJson(report, 2);
    const Value root = mrproper::json::parse(text);
    CHECK_EQ(section(root, "candidates").items().at(0).require("path").asString(), hostile);
    CHECK_EQ(section(root, "candidates").items().at(0).require("displayName").asString(),
             std::string("<b>жирный</b> & \\u0442\\u0435\\u0433"));
    CHECK_EQ(root.require("notes").asString(), std::string("{\"looks\":\"like json\"}"));
    CHECK_EQ(section(root, "errors").items().at(0).require("message").asString(),
             std::string("ошибка\tс\tтабами\nи переводом строки"));

    // Управляющий символ не должен попасть в файл «как есть» — иначе «файл
    // побайтово равен предыдущему» перестаёт быть проверяемым в diff.
    CHECK(!has(text, "\x01"));
    CHECK(has(text, "\\u0001"));
    // Сырые управляющие символы в JSON недопустимы: файл обязан парситься.
    CHECK_EQ(mrproper::json::parse(text).require("kind").asString(), std::string("cleanup"));
}

TEST(reportJson_serialIsMaskedByDefaultAndVisibleOnRequest) {
    Report report = makeSampleReport();
    const std::string plain = reportToJson(report, 2);
    CHECK(!has(plain, "S6Y2NJ0T512345"));
    CHECK(has(plain, "**********2345"));
    // Отчёт сам объявляет маскирование: по privacy видно, что серийник не
    // «потерялся», а намеренно скрыт (SPEC §5 «Приватность»).
    const Value plainRoot = mrproper::json::parse(plain);
    CHECK(section(plainRoot, "privacy").require("serialsMasked").asBool());
    CHECK(section(plainRoot, "privacy").require("volumeGuidsMasked").asBool());
    CHECK_EQ(section(plainRoot, "disks").items().at(0).require("serial").asString(),
             std::string("**********2345"));
    CHECK_EQ(section(plainRoot, "disks")
                 .items()
                 .at(0)
                 .require("partitions")
                 .items()
                 .at(1)
                 .require("volume")
                 .require("guidPath")
                 .asString(),
             std::string("\\\\?\\Volume{****}\\"));
    CHECK(!has(plain, "6f1b2c3d"));

    // Явный отказ от маскирования — осознанный выбор человека (SPEC §5).
    report.options.maskSerials = false;
    report.options.maskVolumeGuids = false;
    const std::string open = reportToJson(report, 2);
    CHECK(has(open, "S6Y2NJ0T512345"));
    CHECK(has(open, "6f1b2c3d-1111-2222-3333-444455556666"));
    const Value openRoot = mrproper::json::parse(open);
    CHECK(!section(openRoot, "privacy").require("serialsMasked").asBool());
    CHECK_EQ(section(openRoot, "disks").items().at(0).require("serial").asString(),
             std::string("S6Y2NJ0T512345"));
}

TEST(reportJson_maskingOnlyChangesPrivacyFields) {
    // Выключение маскирования не должно трогать ничего, кроме самих
    // серийников: тот же отчёт с той же картой дисков и теми же итогами.
    const Report masked = makeSampleReport();
    Report open = masked;
    open.options.maskSerials = false;
    open.options.maskVolumeGuids = false;

    const Value maskedRoot = mrproper::json::parse(reportToJson(masked, 2));
    const Value openRoot = mrproper::json::parse(reportToJson(open, 2));
    CHECK_EQ(section(openRoot, "totals").dump(2), section(maskedRoot, "totals").dump(2));
    CHECK_EQ(section(openRoot, "candidates").dump(2), section(maskedRoot, "candidates").dump(2));
    CHECK_EQ(section(openRoot, "operations").dump(2), section(maskedRoot, "operations").dump(2));
    CHECK_EQ(section(openRoot, "errors").dump(2), section(maskedRoot, "errors").dump(2));
    // А у диска отличаются ровно serial и guidPath.
    const Value maskedDisk = section(maskedRoot, "disks").items().at(0);
    const Value openDisk = section(openRoot, "disks").items().at(0);
    CHECK(maskedDisk.require("serial").asString() != openDisk.require("serial").asString());
    CHECK(maskedDisk.require("partitions").items().at(1).require("volume").require("guidPath").asString() !=
          openDisk.require("partitions").items().at(1).require("volume").require("guidPath").asString());
    CHECK_EQ(maskedDisk.require("model").asString(), openDisk.require("model").asString());
}

TEST(reportJson_emptySectionsArePresentEvenWhenSwitchedOff) {
    Report report = makeSampleReport();
    report.options.includeDisks = false;
    report.options.includeCandidates = false;
    report.options.includeOperations = false;
    report.options.includeUntouched = false;
    report.options.includeErrors = false;

    const Value root = mrproper::json::parse(reportToJson(report, 2));
    // Ключ есть всегда, значение — пустой массив: читателю не приходится гадать,
    // «а есть ли такой раздел».
    for (const char* key : {"disks", "candidates", "operations", "untouched", "errors"}) {
        const Value& value = root.require(key);
        CHECK(value.isArray());
        CHECK_EQ(value.items().size(), static_cast<std::size_t>(0));
    }
    // Итоги считаются по тому, что реально попало в отчёт: усечённый отчёт —
    // нули, а не «мы нашли и не показали».
    const ReportTotals totals = summarizeReport(report);
    CHECK_EQ(totals.diskCount, static_cast<std::size_t>(0));
    CHECK_EQ(totals.diskBytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(totals.candidateCount, static_cast<std::size_t>(0));
    CHECK_EQ(totals.freedBytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(totals.errorCount, static_cast<std::size_t>(0));
}

// =====================================================================
// core::report_json: итоги и самопроверка
// =====================================================================

TEST(reportJson_totalsCountOnlyWhatFreedSpace) {
    const ReportTotals totals = summarizeReport(makeSampleReport());
    CHECK_EQ(totals.diskCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.partitionCount, static_cast<std::size_t>(3));
    CHECK_EQ(totals.volumeCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.diskBytes, static_cast<std::uint64_t>(2000000000000ULL));
    CHECK_EQ(totals.volumeBytes, static_cast<std::uint64_t>(900000000000ULL));
    CHECK_EQ(totals.candidateCount, static_cast<std::size_t>(3));
    CHECK_EQ(totals.candidateBytes, static_cast<std::uint64_t>(7500000ULL));
    CHECK_EQ(totals.lockedCandidateCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.operationCount, static_cast<std::size_t>(3));
    CHECK_EQ(totals.untouchedCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.succeededCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.partialCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.failedCount, static_cast<std::size_t>(0));
    CHECK_EQ(totals.skippedCount, static_cast<std::size_t>(1));
    // Частично освобождённое тоже освобождено — до скольки дошло, до того и
    // освободилось; пропущенное не освобождало ничего.
    CHECK_EQ(totals.freedBytes, static_cast<std::uint64_t>(5000000ULL));
    CHECK_EQ(totals.failedBytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(totals.errorCount, static_cast<std::size_t>(2));
    CHECK_EQ(totals.errorOccurrences, static_cast<std::uint64_t>(4));
}

TEST(reportJson_onlyRemovingOperationsFreeSpace) {
    // Правило модуля (report_json.cpp, countsAsFreed): освобождёнными идут
    // байты операции, которая что-то удаляет (Delete или Trash) и не была
    // пропущена; «частично» тоже засчитывается — до скольки дошло, до того и
    // освободилось. Итог «освобождено» в отчёте не должен расти от того, что
    // операцию решили не выполнять.
    Report report = makeSampleReport();
    report.operations.clear();

    report.operations.push_back(makeOperation(0, PlanAction::Delete, ReportOperationStatus::Success, "Temp", "a",
                                              "C:\\a", 300000ULL, 1, ""));
    report.operations.push_back(makeOperation(1, PlanAction::Trash, ReportOperationStatus::Partial, "Browser", "b",
                                              "C:\\b", 100000ULL, 2, "tx-1"));
    report.operations.push_back(makeOperation(2, PlanAction::Trash, ReportOperationStatus::Skipped, "Browser", "c",
                                              "C:\\c", 700000ULL, 1, "tx-2"));

    const ReportTotals totals = summarizeReport(report);
    CHECK_EQ(totals.freedBytes, static_cast<std::uint64_t>(400000ULL));
    CHECK_EQ(totals.failedBytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(totals.succeededCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.partialCount, static_cast<std::size_t>(1));
    CHECK_EQ(totals.skippedCount, static_cast<std::size_t>(1));
    // Пропущенная операция не освободила ничего: её байты не попали ни в
    // freedBytes, ни в failedBytes — файл не тронули, и ошибки не было.
    CHECK_EQ(totals.failedCount, static_cast<std::size_t>(0));
}

TEST(reportJson_failedRemovalIsCountedByActionNotBySuccess) {
    // Зафиксированное поведение модуля, а не желаемое: освобождение считается
    // по действию и статусу «пропущено», поэтому неудачный Delete с фактически
    // освобождёнными байтами попадает в freedBytes, а failedBytes остаётся
    // нулём. Неудачу видно по failedCount и разделу errors. Тест держит это
    // правило, чтобы его смена была осознанным изменением отчёта, а не
    // побочным эффектом правки.
    Report report = makeSampleReport();
    report.operations.clear();

    report.operations.push_back(makeOperation(0, PlanAction::Delete, ReportOperationStatus::Failed, "Temp", "a",
                                              "C:\\a", 300000ULL, 3, ""));
    report.operations.push_back(makeOperation(1, PlanAction::Keep, ReportOperationStatus::Failed, "Browser", "b",
                                              "C:\\b", 0, 1, ""));

    const ReportTotals totals = summarizeReport(report);
    CHECK_EQ(totals.freedBytes, static_cast<std::uint64_t>(300000ULL));
    CHECK_EQ(totals.failedBytes, static_cast<std::uint64_t>(0));
    CHECK_EQ(totals.failedCount, static_cast<std::size_t>(2));
}

TEST(reportJson_sampleReportPassesValidation) {
    // Эталон согласованности. Если этот тест падает, то «валидный отчёт» из
    // остальных тестов невалиден и все их ожидания ничего не значат.
    const std::vector<std::string> problems = validateReport(makeSampleReport());
    for (const std::string& problem : problems) {
        std::printf("         проблема: %s\n", problem.c_str());
    }
    CHECK_EQ(problems.size(), static_cast<std::size_t>(0));
}

TEST(reportJson_validationCatchesBrokenInvariants) {
    // Каждая проверка ломает ровно один инвариант и ждёт осмысленного текста:
    // «число проблем» ничего не говорит, «о чём именно» — говорит.
    {
        Report report = makeSampleReport();
        report.schemaVersion = kReportJsonSchema + 7;
        CHECK(containsAny(validateReport(report), "схем"));
    }
    {
        Report report = makeSampleReport();
        report.timing.finishedAtUnix = report.timing.startedAtUnix - 1;
        CHECK(containsAny(validateReport(report), "раньше"));
    }
    {
        Report report = makeSampleReport();
        report.timing.durationMs = -5;
        CHECK(containsAny(validateReport(report), "длительность"));
    }
    {
        Report report = makeSampleReport();
        report.candidates[0].confidence = 101;
        CHECK(containsAny(validateReport(report), "уверенность"));
    }
    {
        Report report = makeSampleReport();
        report.candidates[0].allocatedBytes = report.candidates[0].logicalBytes + 1;
        CHECK(containsAny(validateReport(report), "больше logicalBytes"));
    }
    {
        Report report = makeSampleReport();
        report.candidates[0].oldestWrite = report.candidates[0].newestWrite + 10;
        CHECK(containsAny(validateReport(report), "mtime"));
    }
    {
        Report report = makeSampleReport();
        report.operations[0].candidateIndex = 99;
        CHECK(containsAny(validateReport(report), "вне списка кандидатов"));
    }
    {
        Report report = makeSampleReport();
        report.operations[1].candidateIndex = report.operations[0].candidateIndex;
        CHECK(containsAny(validateReport(report), "две записи на кандидата"));
    }
    {
        // §6.3: Keep ничего не освобождает, и у него в отчёте — ноль байт.
        Report report = makeSampleReport();
        report.operations[2].action = PlanAction::Keep;
        report.operations[2].status = ReportOperationStatus::Success;
        report.operations[2].bytes = 500;
        CHECK(containsAny(validateReport(report), "освобождается 0 байт"));
    }
    {
        // Действие, которое не выполняется, обязано иметь статус «пропущено».
        Report report = makeSampleReport();
        report.operations[2].action = PlanAction::Keep;
        report.operations[2].status = ReportOperationStatus::Success;
        CHECK(containsAny(validateReport(report), "не выполняется"));
    }
    {
        Report report = makeSampleReport();
        report.operations[0].attempts = 0;
        CHECK(containsAny(validateReport(report), "попыток"));
    }
    {
        Report report = makeSampleReport();
        report.operations[0].finishedAtUnix = report.operations[0].startedAtUnix - 1;
        CHECK(containsAny(validateReport(report), "раньше"));
    }
    {
        Report report = makeSampleReport();
        report.untouched[0].bytes = 4096;
        CHECK(containsAny(validateReport(report), "освобождается 0 байт"));
    }
    {
        Report report = makeSampleReport();
        report.errors[0].message.clear();
        CHECK(containsAny(validateReport(report), "пустой текст ошибки"));
    }
    {
        Report report = makeSampleReport();
        report.errors[0].scope.clear();
        CHECK(containsAny(validateReport(report), "пустой раздел"));
    }
    {
        Report report = makeSampleReport();
        report.errors[1].count = 0;
        CHECK(containsAny(validateReport(report), "счётчик повторов"));
    }
    {
        // Размер выше 2^53 в double не представим точно: такой отчёт в
        // баг-репорте врал бы числом, поэтому валидация его отвергает.
        Report report = makeSampleReport();
        report.candidates[0].allocatedBytes = 9007199254740992ULL;
        CHECK(containsAny(validateReport(report), "не представимо точно"));
    }
    {
        Report report = makeSampleReport();
        report.disks[0].partitions[1].volume.totalBytes = report.disks[0].partitions[1].lengthBytes + 1;
        CHECK(containsAny(validateReport(report), "том больше раздела"));
    }
}

TEST(reportJson_validationIgnoresChecksWhenCandidatesListIsAbsent) {
    // Отчёт «что нашли» может прийти одной картой разделов без кандидатов:
    // границы candidateIndex тогда проверять не к чему, и такой отчёт должен
    // проходить валидацию, а не сыпать жалобами на каждый индекс.
    Report report = makeSampleReport();
    report.candidates.clear();
    report.operations[0].candidateIndex = 42;
    report.operations[1].candidateIndex = 43;
    report.operations[2].candidateIndex = 44;
    report.untouched[0].candidateIndex = 45;
    CHECK_EQ(validateReport(report).size(), static_cast<std::size_t>(0));
}

// =====================================================================
// core::report_html: экранирование
// =====================================================================

TEST(reportHtml_escapesMarkupQuotesAndControlChars) {
    CHECK_EQ(escapeHtml("a&b<c>d\"e'f"), std::string("a&amp;b&lt;c&gt;d&quot;e&#39;f"));
    // Не-ASCII не трогаем: документ объявляет <meta charset="utf-8">, а
    // кириллица в путях — норма для этой машины.
    CHECK_EQ(escapeHtml("путь"), std::string("путь"));
    // Пробел, таб и переводы строк осмысленны и остаются как есть.
    CHECK_EQ(escapeHtml("a\tb\nc\rd e"), std::string("a\tb\nc\rd e"));
    // Управляющие символы, включая NUL, в разметке ничего не значат и ломают
    // её, попав в атрибут, — заменяем пробелом.
    std::string controls = "a";
    controls.push_back('\x01');
    controls.push_back('\x1F');
    controls.push_back('b');
    controls.push_back('\0');
    controls += "c";
    CHECK_EQ(escapeHtml(controls), std::string("a  b c"));
    CHECK_EQ(escapeHtml(""), std::string(""));
    // Символ маскирования серийника переживает экранирование как есть.
    CHECK_EQ(escapeHtml(kHtmlSerialMaskChar), std::string(kHtmlSerialMaskChar));
}

TEST(reportHtml_userDataCannotCloseTags) {
    HtmlReportInput in = makeHtmlInput();
    in.title = "<script>alert('title')</script>";
    in.notes.push_back("<img src=x onerror=alert(1)>");
    in.issues.push_back(HtmlReportIssue{"execute", "<b>объект</b>", "попытка закрыть <div>", 0x80070005U});
    in.operations.push_back(makeHtmlOperation(PlanAction::Delete, "<i>имя</i>", "C:\\Temp\\\"цитата\"\\файл", 1024,
                                              false, false, "0x80070005 & доступ закрыт", 1500));
    in.candidates[0].path = "C:\\Temp\\</td></tr><script>alert(1)</script>";
    in.candidates[0].displayName = "\"кавычки\" & <b>теги</b>";
    in.disks[0].model = "SSD <script>alert(2)</script>";
    in.disks[0].partitions[1].volume.mountPoints.push_back("D:\\\"><script>alert(3)</script>");

    const std::string html = renderHtmlReport(in);

    // Ни один «полезный» кусок разметки из данных не должен появиться как
    // разметка: это ровно то, что прилетает с отчётом о баге от пользователя.
    CHECK(!has(html, "<script>"));
    // Закрывающая пара тегов из данных не должна вырваться в разметку. Проверяем
    // не саму пару «</td></tr>» — она есть и у настоящих строк таблицы, — а
    // сырой путь из данных: вырваться может только он.
    CHECK(!has(html, "C:\\Temp\\</td>"));
    CHECK(!has(html, "onerror=alert(1)>"));
    CHECK(!has(html, "\"><script"));
    CHECK(has(html, "&lt;script&gt;"));
    CHECK(has(html, "&lt;/td&gt;&lt;/tr&gt;"));
    CHECK(has(html, "&quot;"));
    CHECK(has(html, "&amp;"));
    // Кавычка и угловые скобки в данных не должны выпустить читателя за пределы
    // ячейки: точка монтирования печатается рядом с обычными путями тома.
    CHECK(has(html, "D:\\&quot;&gt;&lt;script&gt;"));
    // Текст ошибки с «разметкой» остаётся текстом, а не элементом отчёта.
    CHECK(has(html, "&lt;img src=x onerror=alert(1)&gt;"));
    // Экранированный текст ошибки остаётся читаемым — и при этом не тянет
    // ресурс: имя атрибута в тексте ошибки не создаёт зависимости от сети.
    CHECK(isStandaloneHtml(html));
    CHECK_EQ(findExternalReferences(html), std::string(""));
}

// =====================================================================
// core::report_html: маскирование серийников
// =====================================================================

TEST(reportHtml_maskSerialNumberKeepsTwoEdges) {
    const std::string serial = "S6Y2NJ0T512345";
    const std::string masked = maskSerialNumber(serial);
    // Видны первые и последние два знака, середина — маска. Длина строки в
    // знаках не меняется: маска «съедает» ровно по одному знаку на символ
    // (в байтах «•» — три, поэтому длины сравниваем в знаках).
    CHECK_EQ(charCount(masked), serial.size());
    CHECK_EQ(masked.substr(0, 2), std::string("S6"));
    CHECK_EQ(masked.substr(masked.size() - 2), std::string("45"));
    CHECK(!has(masked, "2NJ0T51"));
    CHECK_EQ(occurrences(masked, kHtmlSerialMaskChar), serial.size() - 4);

    // Число видимых знаков настраивается, но маска всегда одного размера.
    CHECK_EQ(maskSerialNumber(serial, 3), std::string("S6Y") + maskRun(serial.size() - 6) + "345");
    CHECK_EQ(maskSerialNumber(serial, 1), std::string("S") + maskRun(serial.size() - 2) + "5");
    CHECK_EQ(maskSerialNumber(serial, 0), maskRun(serial.size()));
}

TEST(reportHtml_maskSerialNumberEdgeCases) {
    // Пустой серийник остаётся пустым: рисовать маску неизвестной длины
    // незачем, и в таблице это выглядело бы как «серийник из точек».
    CHECK_EQ(maskSerialNumber(""), std::string(""));
    CHECK_EQ(maskSerialNumber("   "), std::string(""));
    // Пробелы по краям ничего не значат и в решение не входят.
    CHECK_EQ(maskSerialNumber("  S6Y2NJ0T512345  "), maskSerialNumber("S6Y2NJ0T512345"));
    // Серийник короче 2*keep+1 прячется целиком — иначе «маска» была бы сама
    // собой и маскировка показывала бы больше, чем скрывает.
    CHECK_EQ(maskSerialNumber("ABCDE", 2), maskRun(5));
    CHECK_EQ(maskSerialNumber("AB", 0), maskRun(2));
    CHECK_EQ(maskSerialNumber("ABCDE", 1), std::string("A") + maskRun(3) + "E");
    // Своя маска — на случай другой локали или отладочного вывода.
    CHECK_EQ(maskSerialNumber("ABCDEFGH", 2, "#"), std::string("AB####GH"));
    // Пустая маска не оставляет исходник: подставляется маска по умолчанию,
    // иначе «выключенная» маска тихо раскрыла бы серийник.
    CHECK_EQ(maskSerialNumber("ABCDEFGH", 2, ""), std::string("AB") + maskRun(4) + "GH");
}

TEST(reportHtml_serialsAreMaskedInReportByDefault) {
    const std::string html = renderHtmlReport(makeHtmlInput());
    CHECK(!has(html, "S6Y2NJ0T512345"));
    CHECK(has(html, maskSerialNumber("S6Y2NJ0T512345")));
    // Подвал говорит, что маскирование включено: пользователь должен понимать,
    // почему серийника нет (SPEC §5 «Приватность»).
    CHECK(has(html, "Серийники дисков замаскированы"));
    CHECK(isStandaloneHtml(html));
}

TEST(reportHtml_unmaskedSerialsAppearOnlyOnExplicitRequest) {
    HtmlReportOptions options;
    options.maskSerials = false;
    const std::string html = renderHtmlReport(makeHtmlInput(), options);
    CHECK(has(html, "S6Y2NJ0T512345"));
    CHECK(!has(html, maskSerialNumber("S6Y2NJ0T512345")));
    // И подвал об этом не врёт: обещать маскирование, когда его не было, —
    // значит солгать в том самом разделе, который для приватности и написан.
    CHECK(!has(html, "Серийники дисков замаскированы"));
}

TEST(reportHtml_maskedFooterOnlyWhenThereIsSomethingToMask) {
    // Нет серийников — нет и обещания «они замаскированы»: подпись без предмета
    // вводит в заблуждение.
    HtmlReportInput in = makeHtmlInput();
    in.disks.clear();
    CHECK(has(renderHtmlReport(makeHtmlInput()), "Серийники дисков замаскированы"));
    const std::string withoutDisks = renderHtmlReport(in);
    CHECK(!has(withoutDisks, "Серийники дисков замаскированы"));
    // Раздел дисков всё равно печатается — с пометкой «пусто», чтобы читатель не
    // гадал, «нашлось ноль дисков» или «раздел забыли».
    CHECK(has(withoutDisks, "Диски не найдены."));
}

// =====================================================================
// core::report_html: детерминизм и самодостаточность
// =====================================================================

TEST(reportHtml_renderIsByteForByteDeterministic) {
    const HtmlReportInput in = makeHtmlInput();
    const std::string first = renderHtmlReport(in);
    const std::string second = renderHtmlReport(in);
    CHECK(!first.empty());
    CHECK_EQ(first, second);
    // То же для равного по содержимому, но заново собранного входа: детерминизм
    // не зависит от того, один ли это объект.
    CHECK_EQ(renderHtmlReport(makeHtmlInput()), first);
    // Язык подписи выбирает вызывающая сторона; при фиксированных опциях вывод
    // обязан повторяться и в другом языке.
    HtmlReportOptions english;
    english.language = HtmlReportLanguage::English;
    CHECK_EQ(renderHtmlReport(in, english), renderHtmlReport(makeHtmlInput(), english));
}

TEST(reportHtml_documentIsSelfContained) {
    const std::string html = renderHtmlReport(makeHtmlInput());
    CHECK(isStandaloneHtml(html));
    // Падение должно объяснять себя, а не молчать (ADR-007, баг-репорт).
    CHECK_EQ(findExternalReferences(html), std::string(""));
    // Самодостаточность по факту, а не по мнению одной проверки: ни скриптов,
    // ни внешних ресурсов, CSS встроен.
    CHECK(!has(html, "<script"));
    CHECK(!has(html, " src="));
    CHECK(!has(html, " href="));
    CHECK(!has(html, "url("));
    CHECK(has(html, "<style>"));
    CHECK(has(html, "charset=\"utf-8\""));
    CHECK(has(html, "generator\" content=\"MrProper 1.0.0\""));
    CHECK(has(html, "<!DOCTYPE html>\n<html lang=\"ru\">"));
    CHECK_EQ(html.substr(html.size() - 8), std::string("</html>\n"));
}

TEST(reportHtml_externalReferencesAreDetected) {
    // Регрессия «случайно подключили CDN-шрифт» обязана ломать сборку, а не
    // всплывать в баг-репорте, когда отчёт открыли без интернета.
    const std::string cdn = "<img src=\"https://cdn.example.com/a.png\">";
    CHECK(!isStandaloneHtml(cdn));
    CHECK(containsAny({findExternalReferences(cdn)}, "src="));
    CHECK(!isStandaloneHtml("<a href=\"http://example.com/\">x</a>"));
    CHECK(!isStandaloneHtml("<html><body>см. https://example.com/bug</body></html>"));
    // Ссылка в тексте ошибки тоже видна тому, кто читает баг-репорт: молча
    // вычищенный адрес — это уже другой отчёт.
    CHECK(!isStandaloneHtml("<p>ошибка: см. https://example.com/e/1</p>"));
    // Пространства имён XML ничего не грузят.
    CHECK(isStandaloneHtml("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>ok</body></html>"));
    // Якоря и mailto — внутренние адреса.
    CHECK(isStandaloneHtml("<a href=\"#ops\">операции</a><a href=\"mailto:dev@example.com\">почта</a>"));
    // Имя атрибута в тексте ошибки зависимости не создаёт: ищем только в тегах.
    CHECK(isStandaloneHtml("<p>имя атрибута data= в тексте ошибки</p>"));
    CHECK(isStandaloneHtml(""));
}

TEST(reportHtml_languageSwitchesSignsOnly) {
    const std::string russian = renderHtmlReport(makeHtmlInput());
    HtmlReportOptions english;
    english.language = HtmlReportLanguage::English;
    const std::string latin = renderHtmlReport(makeHtmlInput(), english);

    CHECK(has(russian, "<html lang=\"ru\">"));
    CHECK(has(latin, "<html lang=\"en\">"));
    CHECK(russian != latin);
    // Данные не переводятся: модель диска, версия и имя узла остаются как есть.
    CHECK(has(latin, "Samsung SSD 990 PRO 2TB"));
    CHECK(has(latin, "WORKSTATION"));
    // Подписи разделов — по языку.
    CHECK(has(russian, "Среда"));
    CHECK(has(latin, "Environment"));
    CHECK(!has(latin, "Среда"));
    CHECK(isStandaloneHtml(latin));
}

TEST(reportHtml_languageIsDerivedFromTag) {
    CHECK(htmlLanguageFromString("en") == HtmlReportLanguage::English);
    CHECK(htmlLanguageFromString("en-US") == HtmlReportLanguage::English);
    CHECK(htmlLanguageFromString("EN_GB") == HtmlReportLanguage::English);
    CHECK(htmlLanguageFromString("English") == HtmlReportLanguage::English);
    // Всё остальное — русский: лучше русская подпись, чем пустой отчёт.
    CHECK(htmlLanguageFromString("ru") == HtmlReportLanguage::Russian);
    CHECK(htmlLanguageFromString("ru-RU") == HtmlReportLanguage::Russian);
    CHECK(htmlLanguageFromString("de-DE") == HtmlReportLanguage::Russian);
    CHECK(htmlLanguageFromString("") == HtmlReportLanguage::Russian);
    CHECK(htmlLanguageFromString("e") == HtmlReportLanguage::Russian);
}

// =====================================================================
// core::report_html: время и длительность
// =====================================================================

TEST(reportHtml_timeAndDurationDoNotDependOnHost) {
    // Метка отчёта не должна зависеть от часового пояса и локали машины, на
    // которой он построен: тесты обязаны проходить на любом хосте.
    CHECK_EQ(formatUnixUtc(0), std::string("01.01.1970 00:00:00 UTC"));
    CHECK_EQ(formatUnixUtc(0, HtmlReportLanguage::English), std::string("1970-01-01 00:00:00 UTC"));
    CHECK_EQ(formatUnixUtc(1790000042), std::string("21.09.2026 14:14:02 UTC"));
    CHECK_EQ(formatUnixUtc(1790000042, HtmlReportLanguage::English), std::string("2026-09-21 14:14:02 UTC"));
    // До эпохи: деление округляется вниз, метка не «уезжает» на сутки назад.
    CHECK_EQ(formatUnixUtc(-1, HtmlReportLanguage::English), std::string("1969-12-31 23:59:59 UTC"));
    CHECK_EQ(formatUnixUtc(-1), std::string("31.12.1969 23:59:59 UTC"));
}

TEST(reportHtml_durationFormattingMatchesTheReportStyle) {
    CHECK_EQ(formatDurationMs(0), std::string("0 мс"));
    CHECK_EQ(formatDurationMs(840, HtmlReportLanguage::English), std::string("840 ms"));
    CHECK_EQ(formatDurationMs(840), std::string("840 мс"));
    // Русская десятичная запятая — как в core::units, иначе «1.2 с» рядом с
    // «1,2 ГБ» в одной таблице выглядит как две разные правды.
    CHECK_EQ(formatDurationMs(1200), std::string("1,2 с"));
    CHECK_EQ(formatDurationMs(1200, HtmlReportLanguage::English), std::string("1.2 s"));
    CHECK_EQ(formatDurationMs(59000), std::string("59,0 с"));
    CHECK_EQ(formatDurationMs(60000), std::string("1 мин 0 с"));
    CHECK_EQ(formatDurationMs(125000, HtmlReportLanguage::English), std::string("2 min 5 s"));
    CHECK_EQ(formatDurationMs(3600000), std::string("1 ч 0 мин"));
    // Отрицательное время — «неизвестно», а не минус в таблице.
    CHECK_EQ(formatDurationMs(-1), std::string("\xE2\x80\x94"));
}

// =====================================================================
// core::report_html: состав отчёта (FR-8)
// =====================================================================

TEST(reportHtml_containsEveryRequiredSection) {
    HtmlReportInput in = makeHtmlInput();
    in.operations.push_back(makeHtmlOperation(PlanAction::Delete, "Временные файлы", "C:\\Temp\\a.tmp", 4000000ULL,
                                              true, false, "", 2100));
    in.operations.push_back(makeHtmlOperation(PlanAction::Trash, "Кэш браузера", "C:\\Chrome\\Cache", 1000000ULL,
                                              false, false, "0x80070005", 840));
    in.operations.push_back(makeHtmlOperation(PlanAction::SkipLocked, "Старые журналы", "C:\\Windows\\Logs", 0, true,
                                              true, "", 0));
    in.issues.push_back(HtmlReportIssue{"execute", "C:\\Windows\\Temp", "Отказано в доступе", 0x80070005U});
    in.issues.push_back(HtmlReportIssue{"trash", "tx-0001", "Нет места в корзине", 0U});

    const std::string html = renderHtmlReport(in);

    // FR-8: карта разделов, кандидаты с оценками, выполненные операции, ошибки,
    // время, версии ОС и приложения — каждый раздел на своём месте.
    CHECK(has(html, "id=\"summary\""));
    CHECK(has(html, "id=\"env\""));
    CHECK(has(html, "id=\"disks\""));
    CHECK(has(html, "id=\"candidates\""));
    CHECK(has(html, "id=\"operations\""));
    CHECK(has(html, "id=\"issues\""));
    CHECK(has(html, "id=\"notes\""));
    CHECK(has(html, "1.0.0"));
    CHECK(has(html, "Windows 11 Pro"));
    CHECK(has(html, "10.0.22631"));
    CHECK(has(html, "WORKSTATION"));
    CHECK(has(html, "Daniil"));
    CHECK(has(html, "tx-0001"));
    CHECK(has(html, "21.09.2026 14:14:02 UTC"));
    CHECK(has(html, "42,0 с"));  // длительность всей операции: finished - started

    // Карта разделов — CSS-блоки, а не картинка (самодостаточность).
    CHECK(has(html, "class=\"map\""));
    CHECK(has(html, "flex:1000000000000 1 0"));
    CHECK(has(html, "NTFS"));
    CHECK(has(html, "Система"));
    CHECK(has(html, "Samsung SSD 990 PRO 2TB"));

    // Кандидаты с оценками: безопасность, уверенность и «почему это мусор» (FR-4).
    CHECK(has(html, "class=\"chip ok\">безопасно"));
    CHECK(has(html, "95 %"));
    CHECK(has(html, "старше 7 дней"));
    CHECK(has(html, "chrome.exe"));

    // Операции: действие, итог, освобождено, время и текст ошибки.
    CHECK(has(html, "Удалить"));
    CHECK(has(html, "В корзину"));
    CHECK(has(html, "Пропустить (занято)"));
    CHECK(has(html, "4,0 МБ"));
    CHECK(has(html, "2,1 с"));
    // HRESULT печатается и шестнадцатеричным, и десятичным: 0x80070005 = 2147942405.
    CHECK(has(html, "0x80070005 (2147942405)"));
    CHECK(has(html, "Отказано в доступе"));
    CHECK(isStandaloneHtml(html));
}

TEST(reportHtml_dryRunSaysSoAndHidesFreedBytes) {
    // FR-5: dry-run обязателен, и в отчёте нельзя показывать «освобождено» там,
    // где ничего не освобождали, — иначе отчёт врёт.
    HtmlReportInput in = makeHtmlInput();
    in.dryRun = true;
    in.freedBytes = 5000000ULL;
    const std::string dry = renderHtmlReport(in);
    CHECK(has(dry, "Dry-run: ничего не удалено"));
    // Плитка «Освобождено» показывает «—», а не 5,0 МБ, которых не существует.
    CHECK(has(dry, "<div class=\"k\">Освобождено</div><div class=\"v\">—</div>"));
    CHECK(!has(dry, "<div class=\"k\">Освобождено</div><div class=\"v\">5,0 МБ</div>"));

    const std::string real = renderHtmlReport(makeHtmlInput());
    CHECK(!has(real, "Dry-run: ничего не удалено"));
    CHECK(has(real, "<div class=\"k\">Освобождено</div><div class=\"v\">5,0 МБ</div>"));
}

TEST(reportHtml_truncatedListsSayHowManyWereShown) {
    // Молчаливый обрез выглядел бы как «мы всё показали» (FR-8: отчёт —
    // источник правды, в том числе о том, чего в нём нет).
    HtmlReportInput in = makeHtmlInput();
    HtmlReportOptions options;
    options.maxCandidates = 1;
    const std::string html = renderHtmlReport(in, options);
    CHECK(has(html, "Показаны первые 1 из 3 кандидатов"));
    // Первым показан самый крупный кандидат (4 МБ), а не первый по списку.
    CHECK(has(html, "Временные файлы"));
    CHECK(!has(html, "Кэш браузера"));
    CHECK(!has(html, "Старые журналы"));
    // Без ограничения показываются все трое.
    CHECK(!has(renderHtmlReport(in), "Показаны первые"));
}

TEST(reportHtml_emptySectionsAreRenderedAsEmptyNotOmitted) {
    HtmlReportInput in = makeHtmlInput();
    in.candidates.clear();
    in.operations.clear();
    in.issues.clear();
    in.notes.clear();
    const std::string html = renderHtmlReport(in);
    CHECK(has(html, "Кандидатов нет."));
    CHECK(has(html, "Операций не было."));
    CHECK(has(html, "Ошибок нет."));
    // Раздел без содержимого не печатается вовсе, а не печатается пустым.
    CHECK(!has(html, "id=\"notes\""));
    CHECK(isStandaloneHtml(html));
}

TEST(reportHtml_hiddenSectionsAreNotRendered) {
    // Флаг «не показывать» убирает раздел целиком — отчёт с выключенным
    // разделом не теряет ни одной строки данных, он её просто не показывает.
    HtmlReportInput in = makeHtmlInput();
    HtmlReportOptions options;
    options.showDisks = false;
    options.showCandidates = false;
    options.showPartitionMap = false;
    const std::string html = renderHtmlReport(in, options);
    CHECK(!has(html, "id=\"disks\""));
    CHECK(!has(html, "id=\"candidates\""));
    CHECK(!has(html, "class=\"map\""));
    CHECK(has(html, "id=\"operations\""));
    CHECK(has(html, "id=\"env\""));
    CHECK(!has(html, "Samsung SSD 990 PRO 2TB"));
}

TEST(reportHtml_minimalInputStillRendersAWholeDocument) {
    // Отчёт собирается и из одного заполненного HtmlReportInput: пустые
    // реквизиты не печатаются, документ остаётся читаемым.
    HtmlReportInput in;
    const std::string html = renderHtmlReport(in);
    CHECK(has(html, "<!DOCTYPE html>"));
    CHECK(has(html, "<title>MrProper — Отчёт MrProper</title>"));
    CHECK(has(html, "</html>\n"));
    CHECK(isStandaloneHtml(html));
    // Неизвестные значения печатаются прочерком, а не пустой ячейкой.
    CHECK(has(html, "\xE2\x80\x94"));
    CHECK_EQ(renderHtmlReport(in), html);
}
