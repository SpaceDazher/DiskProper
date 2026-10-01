// MrProper — экран «Отчёт»: реализация. Разбор решений — в view_report.hpp.
// Здесь только код, в порядке заголовка: словарь и форматирование → модель →
// текстовый дамп → хранилище отчётов → окно.

#include "view_report.hpp"

#include <commctrl.h>
#include <windowsx.h> // GET_X_LPARAM: позиция мыши в WM_LBUTTONDOWN

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <ctime>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/disk_model.hpp"
#include "core/units.hpp"
#include "locale.hpp"
#include "platform/win_error.hpp"
#include "theme.hpp"

namespace mrproper::ui::report {
namespace {

// ---------------------------------------------------------------------------
// Журнал приложения
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

// Каталог строк грузит оболочка (locale.hpp:456 — «до создания окон»), но
// вызова initialize() в проекте нет: без него колонки журнала и подписи
// фильтров остаются ключами («report.column.time», «units.byte»). Экран
// поднимает каталог сам, один раз на процесс: initialize() идемпотентна.
void ensureStrings() noexcept {
    if (!mrproper::ui::isInitialized()) (void)mrproper::ui::initialize();
}

// Отказ с путём: §12 требует, чтобы в журнале ошибка была с HRESULT и путём, а
// не «сохранить не вышло».
void logSave(std::string_view event, const platform::WinErrorInfo& info) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField("api", info.api));
    fields.push_back(core::logField("path", info.path));
    fields.push_back(core::logField("hr", static_cast<std::uint32_t>(info.hr)));
    core::Logger::instance().write(core::LogLevel::Warn, event, info.text, std::move(fields));
}

// ---------------------------------------------------------------------------
// Словарь экрана
// ---------------------------------------------------------------------------
//
// В ui::locale (задача 69, чужой файл) для экрана «Отчёт» есть семь строк
// (report.title/saved/kept/exportHtml/exportJson/exportText/journal), но нет
// подписей колонок журнала, видов строк и статусов операции: каталог закрывает
// FR-8 кнопками и заголовком, а не содержимым таблицы.
//
// Поэтому слова объявлены здесь, на обоих языках: интерфейс остаётся
// двуязычным (§12 «русская и английская локализация полны»), а список ключей
// для владельца каталога — «report.column.time», «report.kind.operation»,
// «report.status.done» и т. д. Всё остальное подписывается штатными ключами
// (safety.*, action.*, common.*) и именами core::toString, которые не
// переводятся и потому одинаковы в обоих языках.
enum class Word : std::uint8_t {
    ColumnTime,
    ColumnKind,
    ColumnSource,
    ColumnSubject,
    ColumnStatus,
    ColumnSize,
    Operation,
    Error,
    Event,
    StatusDone,
    StatusSkipped,
    StatusPartial,
    StatusFailed,
    JournalEmpty,
    NoReport,
    Hidden,
    ProblemsFound,
    DetailTitle,
    MaskSerials,
    OnlyProblems,
    EmptyWhy,
    EmptyWhat,
};

std::string_view pick(std::string_view ru, std::string_view en) {
    return currentLanguage() == core::Language::English ? en : ru;
}

std::string_view wordOf(Word value) noexcept {
    switch (value) {
    case Word::ColumnTime: return pick("Время", "Time");
    case Word::ColumnKind: return pick("Вид", "Kind");
    case Word::ColumnSource: return pick("Источник", "Source");
    case Word::ColumnSubject: return pick("Что сделано", "Action");
    case Word::ColumnStatus: return pick("Статус", "Status");
    case Word::ColumnSize: return pick("Объём", "Size");
    case Word::Operation: return pick("Операция", "Operation");
    case Word::Error: return pick("Ошибка", "Error");
    case Word::Event: return pick("Событие", "Event");
    case Word::StatusDone: return pick("Выполнено", "Done");
    case Word::StatusSkipped: return pick("Пропущено", "Skipped");
    case Word::StatusPartial: return pick("Частично", "Partial");
    case Word::StatusFailed: return pick("Не выполнено", "Failed");
    case Word::JournalEmpty: return pick("Журнал пуст", "The log is empty");
    case Word::NoReport: return pick("Отчёт ещё не получен", "No report yet");
    case Word::Hidden: return pick("Скрыто фильтром", "Hidden by filters");
    case Word::ProblemsFound: return pick("Замечания к отчёту", "Report problems");
    case Word::DetailTitle: return pick("Подробности строки", "Row details");
    // Галочки приватности и фильтра. Подпись «маскировать серийники» — это
    // ровно то действие, о котором говорит §5 («пользователь может исключить их
    // перед отправкой»), и переименовать её в «приватность» значило бы спрятать
    // от человека, что именно он разрешает.
    case Word::MaskSerials: return pick("Маскировать серийники", "Mask serials");
    case Word::OnlyProblems: return pick("Только ошибки и предупреждения", "Errors and warnings only");
    // Пояснение к пустому журналу: причина и действие. Склеивать их в одну
    // строку нельзя — «ничего не найдено» без «что делать» это то же молчание,
    // ради которого экран и переделывается.
    case Word::EmptyWhy:
        return pick("Операций ещё не было: журнал пишется при очистке и при каждой ошибке.",
                    "No operations yet: the log is written during cleanup and on every error.");
    case Word::EmptyWhat:
        return pick("Запустите очистку на странице «Очистка» — строки появятся здесь сразу после каждой "
                    "операции.",
                    "Start a cleanup on the Cleanup page, rows appear here right after each operation.");
    }
    return pick("неизвестно", "unknown");
}

std::string word(Word value) { return std::string(wordOf(value)); }

// Прочерк вместо пустого значения. Пустая ячейка в таблице читается как «забыли
// заполнить», а прочерк — как «значения нет»: в отчёте это разные вещи (§12:
// «0 провалов» против «неизвестно»).
std::string_view dash() noexcept { return "\xE2\x80\x94"; }  // — U+2014, байтами ради кодировки этапа сборки

// Уровень журнала словами. core::logLevelName даёт машинное имя («warn»), а в
// таблице нужно то, что читает человек.
std::string levelText(core::LogLevel level) {
    switch (level) {
    case core::LogLevel::Trace:
    case core::LogLevel::Debug: return std::string(pick("Отладка", "Debug"));
    case core::LogLevel::Info: return std::string(pick("Инфо", "Info"));
    case core::LogLevel::Warn: return std::string(pick("Внимание", "Warning"));
    case core::LogLevel::Error: return std::string(pick("Ошибка", "Error"));
    case core::LogLevel::Off: return std::string(pick("Выключен", "Off"));
    }
    return std::string(pick("неизвестно", "unknown"));
}

std::string kindText(RowKind kind) {
    switch (kind) {
    case RowKind::Operation: return word(Word::Operation);
    case RowKind::Error: return word(Word::Error);
    case RowKind::Event: return word(Word::Event);
    }
    return word(Word::Event);
}

std::string statusTextOf(core::ReportOperationStatus status) {
    switch (status) {
    case core::ReportOperationStatus::Success: return word(Word::StatusDone);
    case core::ReportOperationStatus::Failed: return word(Word::StatusFailed);
    case core::ReportOperationStatus::Skipped: return word(Word::StatusSkipped);
    case core::ReportOperationStatus::Partial: return word(Word::StatusPartial);
    }
    return word(Word::StatusSkipped);
}

// Уровень строки-операции выводится из результата: журнал и список обязаны
// фильтроваться одним порогом, иначе «только проблемы» на двух видах строк
// означала бы разные вещи.
core::LogLevel levelOf(core::ReportOperationStatus status) {
    switch (status) {
    case core::ReportOperationStatus::Success:
    case core::ReportOperationStatus::Skipped: return core::LogLevel::Info;
    case core::ReportOperationStatus::Partial: return core::LogLevel::Warn;
    case core::ReportOperationStatus::Failed: return core::LogLevel::Error;
    }
    return core::LogLevel::Info;
}

std::string safetyTextOf(core::SafetyLevel safety) {
    switch (safety) {
    case core::SafetyLevel::Safe: return tr(StringId::kSafetySafe);
    case core::SafetyLevel::Review: return tr(StringId::kSafetyReview);
    case core::SafetyLevel::Risky: return tr(StringId::kSafetyRisky);
    }
    return tr(StringId::kCommonUnknown);
}

// Время строки журнала в колонке «Время»: только время суток, дата у строк одна
// (последняя операция), а «2026-09-28 19:05:12» в каждой строке съело бы
// половину колонки.
std::string clockText(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return std::string(dash());
    return toUtf8(formatDate(unixSeconds, DateStyle::Time));
}

std::string stampText(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return std::string(dash());
    return toUtf8(formatDate(unixSeconds, DateStyle::DateTime));
}

std::string sizeText(std::uint64_t bytes) {
    if (bytes == 0) return std::string(dash());
    return formatBytes(bytes);
}

// Регистронезависимый поиск подстроки. Сравнение посимвольное, а не через
// towlower: строки в модели UTF-8, а кириллица в towlower для wchar_t зависит от
// локали процесса, и «Ё» находилась бы в одной системе и не находилась в другой.
bool containsFold(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const auto fold = [](char symbol) {
        return (symbol >= 'A' && symbol <= 'Z') ? static_cast<char>(symbol - 'A' + 'a') : symbol;
    };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        std::size_t i = 0;
        while (i < needle.size() && fold(haystack[start + i]) == fold(needle[i])) ++i;
        if (i == needle.size()) return true;
    }
    return false;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& symbol : out) {
        if (symbol >= 'A' && symbol <= 'Z') symbol = static_cast<char>(symbol - 'A' + 'a');
    }
    return out;
}

// Строка «подпись: значение» для карточки деталей и сводки. Пустое значение
// даёт пустую строку: карточка печатает только заполненные поля, а «Категория: »
// с прочерком — это шум в панели, где и так семь строк.
std::string field(std::string_view label, std::string_view value) {
    if (value.empty()) return {};
    std::string out;
    out.reserve(label.size() + value.size() + 2);
    out.append(label);
    out += ": ";
    out.append(value);
    return out;
}

core::HtmlReportLanguage htmlLanguage() noexcept {
    return currentLanguage() == core::Language::English ? core::HtmlReportLanguage::English
                                                         : core::HtmlReportLanguage::Russian;
}

// ---------------------------------------------------------------------------
// Метка времени в имени файла
// ---------------------------------------------------------------------------
//
// Имя файла отчёта начинается с метки времени, поэтому порядок имён совпадает с
// порядком создания и ротация «последние 20» не зависит от mtime (файл могли
// скопировать). Метка — локальное время: человек открывает каталог и ищет
// «сегодняшний отчёт», а не «отчёт за 16:00 UTC».
//
// Метка нужна и модели (имя файла по умолчанию), и хранилищу (реальное имя), и
// расходиться они не должны — поэтому одна функция на обоих.
std::wstring localStamp(std::int64_t unixSeconds) {
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<std::uint64_t>(unixSeconds) * 10000000ULL;
    FILETIME utc{};
    utc.dwLowDateTime = ticks.LowPart;
    utc.dwHighDateTime = ticks.HighPart;
    FILETIME local{};
    if (::FileTimeToLocalFileTime(&utc, &local) == FALSE) return L"00000000-000000";
    SYSTEMTIME parts{};
    if (::FileTimeToSystemTime(&local, &parts) == FALSE) return L"00000000-000000";
    wchar_t buffer[32] = {};
    const int written = std::swprintf(buffer, std::size(buffer), L"%04u%02u%02u-%02u%02u%02u", parts.wYear,
                                      parts.wMonth, parts.wDay, parts.wHour, parts.wMinute, parts.wSecond);
    if (written <= 0) return L"00000000-000000";
    return buffer;
}

// Вид отчёта в имени файла: «scan», «cleanup», «dryrun» — те же токены, что в
// JSON (core::toString(ReportKind)), чтобы по имени файла было видно, что это за
// отчёт, не открывая его.
std::wstring kindToken(core::ReportKind kind) {
    if (kind == core::ReportKind::Scan) return L"scan";
    if (kind == core::ReportKind::DryRun) return L"dryrun";
    return L"cleanup";
}

// Полное имя файла отчёта. И модель, и хранилище зовут эту функцию, поэтому
// подсказка «сохранить как» и реально записанный файл не могут разойтись.
std::wstring reportFileName(ExportFormat format, core::ReportKind kind, std::int64_t unixSeconds) {
    std::wstring name = kReportFilePrefix;
    name += localStamp(unixSeconds);
    name += L"-";
    name += kindToken(kind);
    name += toWide(extensionFor(format));
    return name;
}

std::int64_t nowUnix() noexcept {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// ---------------------------------------------------------------------------
// Текстовый дамп (FR-8: «текстовый дамп (для баг-репортов)»)
// ---------------------------------------------------------------------------
//
// В отличие от HTML и JSON, текстового дампа в core нет: HTML-рендерер печатает
// документ, JSON — схему, а для баг-репота нужен формат, который читается без
// приложения, без браузера и без jq. Поэтому он живёт здесь.
//
// Два решения, которые видны в результате:
//
//   1. Числа и единицы — core::units, без локали: дамп читают инженеры и
//      скрипты, а «1,2 ГБ» в тексте, который потом вставят в issue публичного
//      репозитория, хуже «1.2 GB». Заголовки разделов, наоборот, на языке
//      интерфейса — их читает человек, разбирающий «почему освободилось не то».
//   2. Серийники маскируются по тем же правилам, что и в HTML/JSON (§5
//      «Приватность»): дамп чаще всего уходит в issue публичного репозитория
//      (ADR-010), и это единственный формат, который человек вставит целиком,
//      не подумав о серийниках.

std::string heading(std::string_view title) {
    std::string out;
    out.append("--- ");
    out.append(title);
    out.append(" ---");
    return out;
}

std::string lineOf(std::string_view label, std::string_view value) {
    std::string out;
    out.append(label);
    if (!value.empty()) {
        out.append(": ");
        out.append(value);
    }
    return out;
}

std::string pad(std::string_view text, std::size_t width) {
    std::string out(text);
    if (out.size() < width) out.append(width - out.size(), ' ');
    return out;
}

// Копия дисков с замаскированными серийниками и идентификаторами томов. Сделано
// копией, а не правкой на месте: отчёт — иммутабельный снимок (§6.4), и
// «маскировать для показа» не должен менять то, что лежит в отчёте.
std::vector<core::PhysicalDisk> maskedDisks(const std::vector<core::PhysicalDisk>& disks,
                                            const core::ReportOptions& options) {
    if (!options.maskSerials && !options.maskVolumeGuids) return disks;
    std::vector<core::PhysicalDisk> copy = disks;
    for (core::PhysicalDisk& disk : copy) {
        if (options.maskSerials && !disk.serial.empty()) disk.serial = core::maskSerial(disk.serial);
        for (core::Partition& partition : disk.partitions) {
            if (options.maskVolumeGuids && !partition.volume.volumeGuidPath.empty()) {
                partition.volume.volumeGuidPath = core::maskVolumeGuid(partition.volume.volumeGuidPath);
            }
        }
    }
    return copy;
}

// Имя действия. core::toString(PlanAction) объявлено в core/model.hpp, но
// определения не имеет (см. предупреждение в core/disk_model.hpp:20 — модуля
// model.cpp в проекте нет), и ссылка на него даёт LNK2019 при первой же
// компоновке экрана «Отчёт». Поэтому имя своё, тем же приёмом, что в
// engine/plan_builder.cpp: это единственный способ показать действия, не
// ожидая чужую правку core.
std::string actionName(core::PlanAction action) {
    switch (action) {
    // std::string, а не std::string_view: pick отдаёт представление, а функция
    // возвращает владеющий тип — C2440 иначе.
    case core::PlanAction::Delete: return std::string(pick("удалить", "delete"));
    case core::PlanAction::Trash: return std::string(pick("в корзину", "trash"));
    case core::PlanAction::Keep: return std::string(pick("оставить", "keep"));
    case core::PlanAction::SkipLocked: return std::string(pick("пропуск (занято)", "skip (locked)"));
    }
    return std::string(pick("оставить", "keep"));
}

// Строка операции в дампе: действие, результат, объём, имя — потом путь и
// подробности отдельной строкой, чтобы длинный путь не сдвигал разбор колонок.
std::string operationLine(const core::ReportOperation& operation) {
    std::string out;
    out += "  ";
    out += pad(actionName(operation.action), 12);
    out += " ";
    out += pad(core::toString(operation.status), 9);
    out += " ";
    out += pad(core::formatBytes(operation.bytes), 10);
    out += "  ";
    out += operation.displayName.empty() ? operation.category : operation.displayName;
    out += "\n";
    if (!operation.path.empty()) {
        out += "      ";
        out += operation.path;
        out += "\n";
    }
    std::string tail = std::string(core::toString(operation.safety));
    if (operation.confidence > 0) {
        tail = std::to_string(operation.confidence) + "% " + tail;
    }
    if (operation.attempts > 1) {
        tail += std::string(pick(" попыток: ", " attempts: ")) + std::to_string(operation.attempts);
    }
    if (!operation.transactionId.empty()) {
        tail += " tx=" + operation.transactionId;
    }
    if (operation.finishedAtUnix > 0 && operation.startedAtUnix > 0) {
        tail += " " + std::to_string((operation.finishedAtUnix - operation.startedAtUnix) * 1000) + " ms";
    }
    if (!operation.detail.empty()) {
        tail += "  ";
        tail += operation.detail;
    }
    if (!tail.empty()) {
        out += "      ";
        out += tail;
        out += "\n";
    }
    return out;
}

std::string renderTextDump(const core::Report& report, const core::ReportOptions& options) {
    std::string out;
    out.reserve(4096);

    // --- Шапка --------------------------------------------------------------
    out += "MrProper";
    out += " \xE2\x80\x94 ";  // — U+2014
    out += pick("отчёт", "report");
    out += "\n";
    out += lineOf(pick("Вид", "Kind"), core::toString(report.kind)) + "\n";
    const std::int64_t generatedAt =
        report.timing.finishedAtUnix > 0 ? report.timing.finishedAtUnix : report.timing.startedAtUnix;
    out += lineOf(pick("Сформирован", "Generated"), core::formatUnixUtc(generatedAt, htmlLanguage())) + "\n";
    if (report.timing.startedAtUnix > 0 && report.timing.finishedAtUnix > 0) {
        const std::int64_t millis = (report.timing.finishedAtUnix - report.timing.startedAtUnix) * 1000;
        out += lineOf(pick("Длительность", "Duration"), std::to_string(millis) + " ms") + "\n";
    }
    out += lineOf(pick("Приложение", "App"), report.environment.appVersion) + "\n";
    out += lineOf(pick("Процесс", "PID"), std::to_string(report.environment.pid)) + "\n";
    if (!report.environment.rulesVersion.empty()) {
        out += lineOf(pick("Набор правил", "Ruleset"), report.environment.rulesVersion) + "\n";
    }
    out += lineOf(pick("ОС", "OS"), report.environment.osCaption + " " + report.environment.osVersion) + "\n";
    if (report.environment.osBuild != 0) {
        out += lineOf(pick("Сборка ОС", "OS build"), std::to_string(report.environment.osBuild)) + "\n";
    }
    out += lineOf(pick("Архитектура", "Arch"), report.environment.architecture) + "\n";
    out += lineOf(pick("Серийники", "Serials"),
                  options.maskSerials ? pick("замаскированы", "masked") : pick("открыто", "plain")) + "\n";
    if (!report.notes.empty()) {
        out += pick("Заметки", "Notes");
        out += ":\n";
        out += report.notes;
        out += "\n";
    }
    out += "\n";

    // --- Итоги --------------------------------------------------------------
    const core::ReportTotals totals = core::summarizeReport(report);
    out += heading(pick("Итоги", "Totals")) + "\n";
    out += lineOf(pick("Дисков", "Disks"), std::to_string(totals.diskCount)) + "\n";
    out += lineOf(pick("Разделов", "Partitions"), std::to_string(totals.partitionCount)) + "\n";
    out += lineOf(pick("Томов", "Volumes"), std::to_string(totals.volumeCount)) + "\n";
    out += lineOf(pick("Кандидатов", "Candidates"), std::to_string(totals.candidateCount)) + "\n";
    out += lineOf(pick("Операций", "Operations"), std::to_string(totals.operationCount)) + "\n";
    out += lineOf(pick("Не тронуто", "Untouched"), std::to_string(totals.untouchedCount)) + "\n";
    out += lineOf(pick("Освобождено", "Freed"), core::formatBytes(totals.freedBytes)) + "\n";
    if (totals.failedBytes > 0) {
        out += lineOf(pick("Не освобождено", "Not freed"), core::formatBytes(totals.failedBytes)) + "\n";
    }
    out += lineOf(pick("Выполнено", "Succeeded"), std::to_string(totals.succeededCount)) + "\n";
    out += lineOf(pick("Частично", "Partial"), std::to_string(totals.partialCount)) + "\n";
    out += lineOf(pick("Провалено", "Failed"), std::to_string(totals.failedCount)) + "\n";
    out += lineOf(pick("Пропущено", "Skipped"), std::to_string(totals.skippedCount)) + "\n";
    out += lineOf(pick("Ошибок", "Errors"), std::to_string(totals.errorCount)) + "\n";
    out += "\n";

    // --- Карта разделов (FR-2, FR-8) ---------------------------------------
    if (options.includeDisks && !report.disks.empty()) {
        out += heading(pick("Карта разделов", "Partition map")) + "\n";
        out += core::toText(maskedDisks(report.disks, options));
        out += "\n";
    }

    // --- Кандидаты с оценками (FR-4, G4) ------------------------------------
    if (options.includeCandidates && !report.candidates.empty()) {
        out += heading(pick("Кандидаты", "Candidates")) + "\n";
        for (const core::CleanupCandidate& candidate : report.candidates) {
            out += "  ";
            out += pad(core::toString(candidate.safety), 8);
            out += " ";
            out += pad(std::to_string(candidate.confidence) + "%", 5);
            out += " ";
            out += pad(core::formatBytes(candidate.allocatedBytes), 10);
            out += " ";
            out += std::to_string(candidate.fileCount);
            out += pick(" файлов", " files");
            out += "  ";
            out += candidate.displayName.empty() ? candidate.category : candidate.displayName;
            out += "\n";
            out += "      ";
            out += candidate.path;
            out += "\n";
            for (const std::string& reason : candidate.reasons) {
                out += "      - ";
                out += reason;
                out += "\n";
            }
            for (const core::ProcessRef& process : candidate.lockedBy) {
                out += "      ! ";
                const std::string_view holder = process.name.empty() ? pick("процесс", "process") : process.name;
                out += lineOf(holder, std::to_string(process.pid));
                out += "\n";
            }
        }
        out += "\n";
    }

    // --- Выполненные операции -----------------------------------------------
    if (options.includeOperations && !report.operations.empty()) {
        out += heading(pick("Операции", "Operations")) + "\n";
        for (const core::ReportOperation& operation : report.operations) {
            out += operationLine(operation);
        }
        out += "\n";
    }

    // --- Намеренно не тронуто -----------------------------------------------
    if (options.includeUntouched && !report.untouched.empty()) {
        out += heading(pick("Не тронуто", "Untouched")) + "\n";
        for (const core::ReportOperation& operation : report.untouched) {
            out += operationLine(operation);
        }
        out += "\n";
    }

    // --- Ошибки (FR-6: не фатальны, но объясняют недобор) -------------------
    if (options.includeErrors && !report.errors.empty()) {
        out += heading(pick("Ошибки", "Errors")) + "\n";
        for (const core::ReportError& error : report.errors) {
            out += "  [";
            out += error.scope;
            out += "] ";
            if (!error.code.empty()) {
                out += error.code;
                out += " ";
            }
            out += error.message;
            if (!error.path.empty()) {
                out += "  ";
                out += error.path;
            }
            if (error.count > 1) {
                out += "  (x";
                out += std::to_string(error.count);
                out += ")";
            }
            out += "\n";
        }
        out += "\n";
    }

    // --- Замечания к самому отчёту ------------------------------------------
    const std::vector<std::string> problems = core::validateReport(report);
    if (!problems.empty()) {
        out += heading(pick("Замечания к отчёту", "Report problems")) + "\n";
        for (const std::string& problem : problems) {
            out += "  - ";
            out += problem;
            out += "\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Сборка HTML-входа из отчёта
// ---------------------------------------------------------------------------
//
// Тот же разбор, что в cli::cmd_report (тот читает JSON и печатает HTML), но
// здесь источник — уже готовый снимок core::Report, а опции берутся из отчёта и с
// галочек экрана. Отчёт при этом не переписывается: маскирование приватности
// передаётся отдельно через HtmlReportOptions (§5).
std::uint32_t parseCode(std::string_view text) {
    if (text.empty()) return 0;
    std::size_t begin = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        begin = 2;
        base = 16;
    }
    std::uint32_t value = 0;
    std::size_t digits = 0;
    for (std::size_t i = begin; i < text.size(); ++i) {
        const char symbol = text[i];
        int digit = -1;
        if (symbol >= '0' && symbol <= '9') {
            digit = symbol - '0';
        } else if (base == 16 && symbol >= 'a' && symbol <= 'f') {
            digit = symbol - 'a' + 10;
        } else if (base == 16 && symbol >= 'A' && symbol <= 'F') {
            digit = symbol - 'A' + 10;
        }
        if (digit < 0) return 0;  // не код, а текст: в issues код не нужен
        value = value * static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(digit);
        ++digits;
    }
    return digits == 0 ? 0u : value;
}

core::HtmlReportInput toHtmlInput(const core::Report& report, const core::ReportOptions& options) {
    core::HtmlReportInput input;
    input.meta.appVersion = report.environment.appVersion;
    input.meta.osName = report.environment.osCaption;
    input.meta.osVersion = report.environment.osVersion;
    input.meta.buildConfig = report.environment.architecture;
    // Время отчёта — из самого отчёта, а не из часов машины: одинаковый вход
    // обязан давать одинаковый файл (SPEC §11.4).
    input.generatedAtUnix =
        report.timing.finishedAtUnix > 0 ? report.timing.finishedAtUnix : report.timing.startedAtUnix;
    input.startedAtUnix = report.timing.startedAtUnix;
    input.finishedAtUnix = report.timing.finishedAtUnix;
    input.dryRun = report.kind != core::ReportKind::Cleanup;

    const core::ReportTotals totals = core::summarizeReport(report);
    input.plannedBytes = totals.candidateBytes;
    input.freedBytes = totals.freedBytes;
    for (const core::ReportOperation& operation : report.operations) {
        if (!operation.transactionId.empty()) {
            input.transactionId = operation.transactionId;
            break;
        }
    }

    // HTML получает уже замаскированную карту: маскирование в самом отчёте
    // меняло бы снимок, а маскирование в рендерере — нет.
    input.disks = maskedDisks(report.disks, options);
    input.candidates = report.candidates;

    // Операции и «не тронуто» идут в один раздел HTML, но помечены по-разному:
    // пропуск — это тоже результат (FR-6: остальное продолжается).
    const auto append = [&input](const core::ReportOperation& operation, bool untouched) {
        core::HtmlReportOperation html;
        html.action = operation.action;
        html.name = operation.displayName.empty() ? operation.category : operation.displayName;
        html.path = operation.path;
        html.bytes = operation.bytes;
        html.safety = operation.safety;
        html.confidence = operation.confidence;
        html.success = operation.status != core::ReportOperationStatus::Failed;
        html.skipped = untouched || operation.status == core::ReportOperationStatus::Skipped;
        html.error = operation.detail;
        html.durationMs = operation.finishedAtUnix > 0 && operation.startedAtUnix > 0
                              ? (operation.finishedAtUnix - operation.startedAtUnix) * 1000
                              : 0;
        input.operations.push_back(std::move(html));
    };
    for (const core::ReportOperation& operation : report.operations) append(operation, false);
    for (const core::ReportOperation& operation : report.untouched) append(operation, true);

    for (const core::ReportError& error : report.errors) {
        core::HtmlReportIssue issue;
        issue.stage = error.scope;
        issue.subject = error.path.empty() ? error.operation : error.path;
        issue.message = error.message;
        issue.code = parseCode(error.code);
        input.issues.push_back(std::move(issue));
    }

    // Заметки отчёта — свободный текст, а HTML печатает их списком.
    std::size_t begin = 0;
    while (begin <= report.notes.size()) {
        const std::size_t end = report.notes.find('\n', begin);
        std::string note = end == std::string::npos ? report.notes.substr(begin)
                                                    : report.notes.substr(begin, end - begin);
        while (!note.empty() && (note.back() == '\r' || note.back() == ' ')) note.pop_back();
        if (!note.empty()) input.notes.push_back(note);
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
    return input;
}

}  // namespace

const char* toString(ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::Html: return "html";
    case ExportFormat::Json: return "json";
    case ExportFormat::Text: return "text";
    }
    return "html";
}

std::string_view extensionFor(ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::Html: return ".html";
    case ExportFormat::Json: return ".json";
    case ExportFormat::Text: return ".txt";
    }
    return ".html";
}

const char* toString(RowKind kind) noexcept {
    switch (kind) {
    case RowKind::Operation: return "operation";
    case RowKind::Error: return "error";
    case RowKind::Event: return "event";
    }
    return "event";
}

bool isReportControl(WORD controlId) noexcept {
    return controlId >= static_cast<WORD>(ControlId::First) && controlId <= static_cast<WORD>(ControlId::Last);
}

// ---------------------------------------------------------------------------
// Модель
// ---------------------------------------------------------------------------

struct ReportViewModel::Impl {
    // Снимок отчёта. shared_ptr<const>, а не значение: отчёт может прийти из
    // фонового потока, и показывать надо ровно тот, который опубликован
    // (§6.4 «результаты не мутируются после публикации»).
    std::shared_ptr<const core::Report> report;
    std::vector<core::LogRecord> journal;
    std::uint64_t journalSequence{};

    ReportFilters filters;
    SortOrder sort;
    core::ReportOptions options;

    std::vector<JournalRow> all;   // всё, что пришло из отчёта и журнала
    std::vector<JournalRow> rows;  // прошедшее фильтры, в порядке сортировки
    std::array<std::size_t, 3> visibleByKind{0, 0, 0};
    std::size_t hidden{};

    std::string selectedKey;
    std::size_t kept{};
    std::size_t pruned{};
    std::string savedPath;
    int cardHeightDip{140};
    int viewLines{20};

    [[nodiscard]] const core::Report* reportOrNull() const noexcept { return report.get(); }

    // --- Сборка строк -------------------------------------------------------

    void rebuild() {
        all.clear();
        if (const core::Report* snapshot = reportOrNull()) {
            appendOperations(*snapshot);
            appendErrors(*snapshot);
        }
        appendJournal();
        applyFilters();
        // Выделение задано ключом, поэтому переживает пересборку списка. Ключ,
        // которого больше нет (отчёт переиздан без этой операции), снимается:
        // молча подсвечивать нечего.
        if (!selectedKey.empty() && indexForKey(selectedKey) == kNoRow) selectedKey.clear();
    }

    void appendOperations(const core::Report& snapshot) {
        const auto append = [this](const core::ReportOperation& operation, std::string_view prefix,
                                   std::size_t position) {
            JournalRow row;
            row.kind = RowKind::Operation;
            row.level = levelOf(operation.status);
            row.atUnix = operation.finishedAtUnix > 0 ? operation.finishedAtUnix : operation.startedAtUnix;
            row.key = std::string(prefix) + std::to_string(position);
            row.timeText = clockText(row.atUnix);
            row.kindText = kindText(RowKind::Operation);
            row.sourceText = operation.category;
            row.subjectText = operation.displayName.empty() ? operation.path : operation.displayName;
            row.statusText = statusTextOf(operation.status);
            row.bytesText = sizeText(operation.bytes);
            row.category = operation.category;
            row.displayName = operation.displayName;
            row.path = operation.path;
            row.detail = operation.detail;
            row.transactionId = operation.transactionId;
            row.safetyText = safetyTextOf(operation.safety);
            row.actionText = actionName(operation.action);
            row.bytes = operation.bytes;
            row.attempts = operation.attempts;
            row.occurrences = 1;
            row.confidence = operation.confidence;
            row.startedAtUnix = operation.startedAtUnix;
            row.finishedAtUnix = operation.finishedAtUnix;
            row.action = operation.action;
            row.safety = operation.safety;
            all.push_back(std::move(row));
        };
        // Ключ — позиция в своём списке, а не candidateIndex: у «операций» и
        // «не тронуто» индексы кандидатов пересекаются, и без префикса выделение
        // прыгало бы между разделами отчёта.
        for (std::size_t i = 0; i < snapshot.operations.size(); ++i) {
            append(snapshot.operations[i], "o", i);
        }
        for (std::size_t i = 0; i < snapshot.untouched.size(); ++i) {
            append(snapshot.untouched[i], "u", i);
        }
    }

    void appendErrors(const core::Report& snapshot) {
        for (std::size_t i = 0; i < snapshot.errors.size(); ++i) {
            const core::ReportError& error = snapshot.errors[i];
            JournalRow row;
            row.kind = RowKind::Error;
            // Ошибка всегда Error: показывать её в журнале по умолчанию обязана
            // всякая, иначе «ошибки собираются в отчёт» (FR-6) останется правдой
            // только для файла.
            row.level = core::LogLevel::Error;
            row.atUnix = error.atUnix;
            row.key = "e" + std::to_string(i);
            row.timeText = clockText(error.atUnix);
            row.kindText = kindText(RowKind::Error);
            row.sourceText = error.scope;
            row.subjectText = error.message;
            row.statusText = error.code.empty() ? word(Word::Error) : error.code;
            row.bytesText = std::string(dash());
            row.scope = error.scope;
            row.code = error.code;
            row.message = error.message;
            row.path = error.path;
            row.detail = error.operation;
            row.occurrences = error.count;
            all.push_back(std::move(row));
        }
    }

    void appendJournal() {
        for (const core::LogRecord& record : journal) {
            JournalRow row;
            row.kind = RowKind::Event;
            row.level = record.level;
            row.atUnix = record.epochMillis / 1000;
            row.key = "v" + std::to_string(record.sequence);
            row.timeText = clockText(row.atUnix);
            row.kindText = kindText(RowKind::Event);
            row.sourceText = record.event;
            row.subjectText = record.message;
            row.statusText = levelText(record.level);
            row.bytesText = std::string(dash());
            row.event = record.event;
            row.message = record.message;
            row.fields = record.fields;
            all.push_back(std::move(row));
        }
    }

    [[nodiscard]] static bool isProblem(const JournalRow& row) noexcept { return row.level >= core::LogLevel::Warn; }

    [[nodiscard]] bool visibleByFilters(const JournalRow& row, std::string_view needle) const {
        switch (row.kind) {
        case RowKind::Operation:
            if (!filters.operations) return false;
            break;
        case RowKind::Error:
            if (!filters.errors) return false;
            break;
        case RowKind::Event:
            if (!filters.events) return false;
            if (filters.minLevel != core::LogLevel::Off && row.level < filters.minLevel) return false;
            break;
        }
        if (filters.problemsOnly && !isProblem(row)) return false;
        if (needle.empty()) return true;
        return containsFold(row.sourceText, needle) || containsFold(row.subjectText, needle) ||
               containsFold(row.statusText, needle) || containsFold(row.path, needle) ||
               containsFold(row.detail, needle) || containsFold(row.message, needle) ||
               containsFold(row.event, needle) || containsFold(row.code, needle);
    }

    void applyFilters() {
        const std::string needle = lowered(filters.search);
        rows.clear();
        visibleByKind = {0, 0, 0};
        for (const JournalRow& row : all) {
            if (!visibleByFilters(row, needle)) continue;
            visibleByKind[static_cast<std::size_t>(row.kind)] += 1u;
            rows.push_back(row);
        }
        hidden = all.size() - rows.size();
        sortRows();
    }

    [[nodiscard]] static std::size_t kindWeight(RowKind kind) noexcept {
        switch (kind) {
        case RowKind::Operation: return 0;
        case RowKind::Error: return 1;
        case RowKind::Event: return 2;
        }
        return 2;
    }

    void sortRows() {
        const SortOrder order = sort;
        // stable_sort, а не sort: строки с одинаковым временем (а время
        // округлено до секунды, поэтому совпадения — правило, а не случайность)
        // обязаны сохранять порядок, в котором их дал отчёт. Иначе журнал
        // перемешивался бы при каждой перерисовке, и человек не мог бы сослаться
        // на «третью строку» в баг-репорте.
        std::stable_sort(rows.begin(), rows.end(), [order](const JournalRow& left, const JournalRow& right) {
            int sign = 0;
            switch (order.key) {
            case SortKey::Time:
                if (left.atUnix != right.atUnix) {
                    sign = left.atUnix < right.atUnix ? -1 : 1;
                } else if (left.key != right.key) {
                    sign = left.key < right.key ? -1 : 1;
                }
                break;
            case SortKey::Kind: {
                const std::size_t a = kindWeight(left.kind);
                const std::size_t b = kindWeight(right.kind);
                if (a != b) {
                    sign = a < b ? -1 : 1;
                } else if (left.atUnix != right.atUnix) {
                    sign = left.atUnix < right.atUnix ? -1 : 1;
                }
                break;
            }
            case SortKey::Size:
                if (left.bytes != right.bytes) {
                    sign = left.bytes < right.bytes ? -1 : 1;
                } else if (left.atUnix != right.atUnix) {
                    sign = left.atUnix < right.atUnix ? -1 : 1;
                }
                break;
            }
            if (sign == 0) return false;
            return order.direction == SortDirection::Ascending ? sign < 0 : sign > 0;
        });
    }

    [[nodiscard]] std::size_t indexForKey(std::string_view key) const {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].key == key) return i;
        }
        return kNoRow;
    }

    [[nodiscard]] std::size_t findSelected() const {
        if (selectedKey.empty()) return kNoRow;
        return indexForKey(selectedKey);
    }
};

ReportViewModel::ReportViewModel() : impl_(std::make_unique<Impl>()) {}

// --- Снимок отчёта ------------------------------------------------------------

void ReportViewModel::publishReport(core::Report report) {
    publishReport(std::make_shared<const core::Report>(std::move(report)));
}

void ReportViewModel::publishReport(std::shared_ptr<const core::Report> report) {
    impl_->report = std::move(report);
    // Опции берём из отчёта: движок уже решил, что показывать, а экран может
    // их уточнить (маскирование серийников, §5). Смена отчёта не должна забывать
    // о правке приватности, сделанной человеком, — маскирование переносится, а
    // состав разделов берётся от нового отчёта.
    const bool mask = impl_->options.maskSerials;
    if (impl_->report) impl_->options = impl_->report->options;
    impl_->options.maskSerials = mask;
    impl_->rebuild();
}

bool ReportViewModel::hasReport() const noexcept { return impl_->report != nullptr; }

const core::Report* ReportViewModel::report() const noexcept { return impl_->report.get(); }

void ReportViewModel::clearReport() {
    impl_->report.reset();
    impl_->selectedKey.clear();
    impl_->rebuild();
}

// --- Журнал -------------------------------------------------------------------

void ReportViewModel::refreshJournal() {
    core::Logger& logger = core::Logger::instance();
    const std::uint64_t sequence = logger.lastSequence();
    if (sequence == impl_->journalSequence && !impl_->journal.empty()) {
        // Новых записей нет: перечитывать кольцо и перерисовывать список
        // значило бы перерисовывать его впустую дважды в секунду.
        return;
    }
    impl_->journalSequence = sequence;
    impl_->journal = logger.snapshot();
    impl_->rebuild();
}

void ReportViewModel::publishJournal(std::vector<core::LogRecord> records) {
    std::uint64_t highest = 0;
    for (const core::LogRecord& record : records) {
        if (record.sequence > highest) highest = record.sequence;
    }
    impl_->journalSequence = highest;
    impl_->journal = std::move(records);
    impl_->rebuild();
}

std::size_t ReportViewModel::journalCount() const noexcept { return impl_->journal.size(); }

std::uint64_t ReportViewModel::journalSequence() const noexcept { return impl_->journalSequence; }

// --- Фильтры и сортировка ------------------------------------------------------

void ReportViewModel::setFilters(ReportFilters filters) {
    impl_->filters = std::move(filters);
    impl_->applyFilters();
    if (!impl_->selectedKey.empty() && impl_->indexForKey(impl_->selectedKey) == kNoRow) impl_->selectedKey.clear();
}

void ReportViewModel::setFilter(std::string_view which, bool enabled) {
    if (which == "operations") {
        impl_->filters.operations = enabled;
    } else if (which == "errors") {
        impl_->filters.errors = enabled;
    } else if (which == "events") {
        impl_->filters.events = enabled;
    } else {
        // Неизвестный ключ фильтра игнорируется, а не трактуется как «всё
        // включено»: молчаливый фильтр, который не сработал, хуже его отсутствия.
        return;
    }
    impl_->applyFilters();
}

void ReportViewModel::toggleProblemsOnly() {
    impl_->filters.problemsOnly = !impl_->filters.problemsOnly;
    impl_->applyFilters();
}

void ReportViewModel::setSearch(std::string_view text) {
    impl_->filters.search = std::string(text);
    impl_->applyFilters();
}

void ReportViewModel::clearFilters() {
    impl_->filters = ReportFilters{};
    impl_->applyFilters();
}

ReportFilters ReportViewModel::filters() const noexcept { return impl_->filters; }

void ReportViewModel::setSort(SortKey key, SortDirection direction) {
    impl_->sort.key = key;
    impl_->sort.direction = direction;
    impl_->sortRows();
}

void ReportViewModel::toggleSort(SortKey key) {
    const SortOrder current = impl_->sort;
    if (current.key == key) {
        impl_->sort.direction =
            current.direction == SortDirection::Ascending ? SortDirection::Descending : SortDirection::Ascending;
    } else {
        impl_->sort = SortOrder::defaultFor(key);
    }
    impl_->sortRows();
}

SortOrder ReportViewModel::sort() const noexcept { return impl_->sort; }

// --- Строки -------------------------------------------------------------------

const std::vector<JournalRow>& ReportViewModel::rows() const noexcept { return impl_->rows; }

std::size_t ReportViewModel::rowCount() const noexcept { return impl_->rows.size(); }

const JournalRow* ReportViewModel::rowAt(std::size_t index) const {
    if (index >= impl_->rows.size()) return nullptr;
    return &impl_->rows[index];
}

const JournalRow* ReportViewModel::selectedRow() const {
    const std::size_t index = impl_->findSelected();
    if (index == kNoRow) return nullptr;
    return &impl_->rows[index];
}

std::size_t ReportViewModel::indexForKey(std::string_view key) const { return impl_->indexForKey(key); }

std::string ReportViewModel::keyAt(std::size_t index) const {
    const JournalRow* row = rowAt(index);
    return row == nullptr ? std::string() : row->key;
}

std::size_t ReportViewModel::visibleCount(RowKind kind) const noexcept {
    return impl_->visibleByKind[static_cast<std::size_t>(kind)];
}

std::size_t ReportViewModel::hiddenCount() const noexcept { return impl_->hidden; }

// --- Выделение и клавиатура ---------------------------------------------------

void ReportViewModel::setSelectedIndex(std::size_t index) {
    if (index >= impl_->rows.size()) {
        impl_->selectedKey.clear();
        return;
    }
    impl_->selectedKey = impl_->rows[index].key;
}

void ReportViewModel::setSelectedKey(std::string_view key) {
    impl_->selectedKey = std::string(key);
    if (impl_->indexForKey(impl_->selectedKey) == kNoRow) impl_->selectedKey.clear();
}

std::size_t ReportViewModel::selectedIndex() const noexcept { return impl_->findSelected(); }

void ReportViewModel::selectFirst() {
    if (!impl_->rows.empty()) impl_->selectedKey = impl_->rows.front().key;
}

void ReportViewModel::selectLast() {
    if (!impl_->rows.empty()) impl_->selectedKey = impl_->rows.back().key;
}

void ReportViewModel::clearSelection() { impl_->selectedKey.clear(); }

bool ReportViewModel::moveSelection(int delta) {
    if (impl_->rows.empty() || delta == 0) return false;
    const std::size_t current = impl_->findSelected();
    if (current == kNoRow) {
        // Без выделения стрелка выбирает первую строку: так ведут себя и списки
        // Windows, и человек не обязан знать, что «выделения ещё нет».
        selectFirst();
        return true;
    }
    const long long target = static_cast<long long>(current) + delta;
    if (target < 0) {
        impl_->selectedKey = impl_->rows.front().key;
        return true;
    }
    const auto bounded = static_cast<std::size_t>(target);
    impl_->selectedKey = impl_->rows[bounded < impl_->rows.size() ? bounded : impl_->rows.size() - 1u].key;
    return true;
}

bool ReportViewModel::moveSelectionByPage(int direction, int viewLines) {
    const int lines = viewLines > 0 ? viewLines : impl_->viewLines;
    const int step = lines > 1 ? lines - 1 : 1;
    if (direction > 0) return moveSelection(step);
    if (direction < 0) return moveSelection(-step);
    return false;
}

void ReportViewModel::setViewLines(int lines) {
    if (lines > 0) impl_->viewLines = lines;
}

bool ReportViewModel::handleKeyDown(std::uint32_t virtualKey, bool controlDown, bool shiftDown) {
    if (shiftDown && virtualKey == VK_F3) {
        toggleSort(SortKey::Kind);
        return true;
    }
    switch (virtualKey) {
    case VK_HOME:
        selectFirst();
        return true;
    case VK_END:
        selectLast();
        return true;
    case VK_PRIOR:
        return moveSelectionByPage(-1, impl_->viewLines);
    case VK_NEXT:
        return moveSelectionByPage(1, impl_->viewLines);
    case 'F':
        // Ctrl+F в этом экране сбрасывает фильтры: отдельного поля поиска у
        // списка нет, а фильтр приходит из PageState, и без этого способа
        // вернуть полный список из экрана нечем.
        if (controlDown) {
            clearFilters();
            return true;
        }
        break;
    case 'L':
        if (controlDown) {
            toggleProblemsOnly();
            return true;
        }
        break;
    default: break;
    }
    return false;
}

// --- Раскладка ----------------------------------------------------------------

int ReportViewModel::cardHeightDip() const noexcept { return impl_->cardHeightDip; }

void ReportViewModel::setCardHeightDip(int heightDip) {
    if (heightDip > 0) impl_->cardHeightDip = heightDip;
}

// --- Тексты -------------------------------------------------------------------

std::string ReportViewModel::titleText() const {
    std::string title = tr(StringId::kReportTitle);
    if (const core::Report* snapshot = impl_->reportOrNull()) {
        title += " — ";
        title += core::toString(snapshot->kind);
    }
    return title;
}

std::string ReportViewModel::statusText() const {
    if (impl_->report == nullptr) {
        if (impl_->journal.empty()) return word(Word::NoReport);
        std::string out = tr(StringId::kReportJournal);
        out += ": ";
        out += core::formatCount(static_cast<std::uint64_t>(impl_->journal.size()));
        return out;
    }
    const core::ReportTotals totals = core::summarizeReport(*impl_->report);
    std::string out;
    out += pick("Освобождено: ", "Freed: ");
    out += core::formatBytes(totals.freedBytes);
    out += pick(" · операций: ", " · operations: ");
    out += core::formatCount(static_cast<std::uint64_t>(totals.operationCount));
    out += pick(" · ошибок: ", " · errors: ");
    out += core::formatCount(static_cast<std::uint64_t>(totals.errorCount));
    if (impl_->hidden != 0) {
        out += pick(" · скрыто: ", " · hidden: ");
        out += core::formatCount(static_cast<std::uint64_t>(impl_->hidden));
    }
    if (impl_->kept != 0) {
        out += pick(" · отчётов: ", " · reports: ");
        out += std::to_string(impl_->kept);
    }
    return out;
}

std::vector<std::string> ReportViewModel::summaryLines() const {
    std::vector<std::string> lines;
    const core::Report* snapshot = impl_->reportOrNull();
    if (snapshot == nullptr) {
        lines.push_back(word(Word::NoReport));
        return lines;
    }
    const core::ReportTotals totals = core::summarizeReport(*snapshot);
    const core::ReportEnvironment& env = snapshot->environment;
    lines.push_back(field(pick("Вид", "Kind"), core::toString(snapshot->kind)));
    lines.push_back(field(pick("Начало", "Started"), stampText(snapshot->timing.startedAtUnix)));
    lines.push_back(field(pick("Конец", "Finished"), stampText(snapshot->timing.finishedAtUnix)));
    if (snapshot->timing.durationMs != 0) {
        lines.push_back(field(pick("Длительность", "Duration"),
                              core::formatDurationMs(snapshot->timing.durationMs, htmlLanguage())));
    }
    lines.push_back(field(pick("Приложение", "App"), env.appVersion));
    lines.push_back(field(pick("Процесс", "PID"), std::to_string(env.pid)));
    if (!env.rulesVersion.empty()) lines.push_back(field(pick("Правила", "Rules"), env.rulesVersion));
    lines.push_back(field(pick("ОС", "OS"), env.osCaption));
    if (!env.osVersion.empty()) lines.push_back(field(pick("Версия ОС", "OS version"), env.osVersion));
    lines.push_back(field(pick("Архитектура", "Arch"), env.architecture));
    lines.push_back(field(pick("Диски", "Disks"), std::to_string(totals.diskCount)));
    lines.push_back(field(pick("Разделы", "Partitions"), std::to_string(totals.partitionCount)));
    lines.push_back(field(pick("Тома", "Volumes"), std::to_string(totals.volumeCount)));
    lines.push_back(field(pick("Кандидаты", "Candidates"), std::to_string(totals.candidateCount)));
    if (totals.lockedCandidateCount != 0) {
        lines.push_back(field(pick("Держат файлы", "Locked"), std::to_string(totals.lockedCandidateCount)));
    }
    lines.push_back(field(pick("Операции", "Operations"), std::to_string(totals.operationCount)));
    lines.push_back(field(pick("Не тронуто", "Untouched"), std::to_string(totals.untouchedCount)));
    lines.push_back(field(pick("Освобождено", "Freed"), core::formatBytes(totals.freedBytes)));
    if (totals.failedBytes != 0) {
        lines.push_back(field(pick("Не освобождено", "Not freed"), core::formatBytes(totals.failedBytes)));
    }
    lines.push_back(field(pick("Выполнено", "Succeeded"), std::to_string(totals.succeededCount)));
    lines.push_back(field(pick("Провалено", "Failed"), std::to_string(totals.failedCount)));
    lines.push_back(field(pick("Пропущено", "Skipped"), std::to_string(totals.skippedCount)));
    lines.push_back(field(pick("Ошибки", "Errors"), std::to_string(totals.errorCount)));
    lines.push_back(field(pick("Серийники", "Serials"),
                          impl_->options.maskSerials ? pick("замаскированы", "masked") : pick("открыто", "plain")));
    const std::vector<std::string> issues = problems();
    if (!issues.empty()) {
        lines.push_back(field(word(Word::ProblemsFound), std::to_string(issues.size())));
    }
    return lines;
}

std::vector<std::string> ReportViewModel::detailLines() const {
    std::vector<std::string> lines;
    const JournalRow* row = selectedRow();
    if (row == nullptr) {
        if (impl_->rows.empty()) {
            lines.push_back(impl_->all.empty() ? word(Word::JournalEmpty) : word(Word::Hidden));
        }
        return lines;
    }
    lines.push_back(field(pick("Вид", "Kind"), row->kindText));
    lines.push_back(field(word(Word::ColumnTime), row->timeText));
    if (row->kind == RowKind::Event) {
        lines.push_back(field(pick("Событие", "Event"), row->event));
        lines.push_back(field(pick("Уровень", "Level"), row->statusText));
        lines.push_back(field(pick("Сообщение", "Message"), row->message));
        for (const core::LogField& entry : row->fields) {
            lines.push_back(field(entry.key, entry.value));
        }
        return lines;
    }
    if (row->kind == RowKind::Error) {
        lines.push_back(field(pick("Этап", "Scope"), row->scope));
        lines.push_back(field(tr(StringId::kCommonError), row->message));
        lines.push_back(field(pick("Код", "Code"), row->code));
        lines.push_back(field(pick("Операция", "Operation"), row->detail));
        lines.push_back(field(pick("Путь", "Path"), row->path));
        if (row->occurrences > 1) {
            lines.push_back(field(pick("Повторов", "Attempts"), std::to_string(row->occurrences)));
        }
        return lines;
    }
    lines.push_back(field(pick("Категория", "Category"), row->category));
    lines.push_back(field(pick("Название", "Name"), row->displayName));
    lines.push_back(field(pick("Путь", "Path"), row->path));
    lines.push_back(field(pick("Статус", "Status"), row->statusText));
    lines.push_back(field(pick("Действие", "Action"), row->actionText));
    lines.push_back(field(pick("Уровень риска", "Safety"), row->safetyText));
    if (row->confidence != 0) {
        lines.push_back(field(pick("Уверенность", "Confidence"), std::to_string(row->confidence) + "%"));
    }
    if (row->bytes != 0) {
        // И аллоцированный размер словами, и точный в байтах: «1,2 ГБ (1 234 567
        // Б)» — тот случай, когда отчёт потом считают вручную.
        const std::string size = core::formatBytes(row->bytes) + std::string(pick(" (", " (")) +
                                  std::to_string(row->bytes) + std::string(pick(" Б)", " B)"));
        lines.push_back(field(pick("Объём", "Size"), size));
    }
    if (row->attempts > 1) {
        lines.push_back(field(pick("Попыток", "Attempts"), std::to_string(row->attempts)));
    }
    if (!row->transactionId.empty()) {
        lines.push_back(field(tr(StringId::kCleanupTrashLabel), row->transactionId));
    }
    lines.push_back(field(pick("Начало", "Started"), stampText(row->startedAtUnix)));
    lines.push_back(field(pick("Конец", "Finished"), stampText(row->finishedAtUnix)));
    if (row->finishedAtUnix > 0 && row->startedAtUnix > 0) {
        const std::string duration = std::to_string((row->finishedAtUnix - row->startedAtUnix) * 1000) +
                                     std::string(pick(" мс", " ms"));
        lines.push_back(field(pick("Длительность", "Duration"), duration));
    }
    if (!row->detail.empty()) {
        lines.push_back(field(tr(StringId::kCommonError), row->detail));
    }
    return lines;
}

std::string ReportViewModel::savedText() const {
    if (impl_->savedPath.empty()) return {};
    std::string out = tr(StringId::kReportSaved, impl_->savedPath);
    if (impl_->pruned != 0) {
        out += pick(" · удалено старых: ", " · pruned: ");
        out += std::to_string(impl_->pruned);
    }
    return out;
}

std::size_t ReportViewModel::keptCount() const noexcept { return impl_->kept; }

void ReportViewModel::noteSave(const std::string& pathUtf8, std::size_t kept, std::size_t pruned) {
    impl_->savedPath = pathUtf8;
    impl_->kept = kept;
    impl_->pruned = pruned;
}

std::vector<std::string> ReportViewModel::problems() const {
    if (impl_->report == nullptr) return {};
    return core::validateReport(*impl_->report);
}

bool ReportViewModel::hasProblems() const noexcept { return !problems().empty(); }

// --- Опции и экспорт ----------------------------------------------------------

core::ReportOptions ReportViewModel::reportOptions() const { return impl_->options; }

void ReportViewModel::setReportOptions(const core::ReportOptions& options) {
    // Опции влияют на текст экспорта, а не на уже собранные строки журнала,
    // поэтому список не пересобирается: он показывает факты, а не вид отчёта.
    impl_->options = options;
}

void ReportViewModel::setMaskSerials(bool mask) { impl_->options.maskSerials = mask; }

bool ReportViewModel::maskSerials() const noexcept { return impl_->options.maskSerials; }

core::HtmlReportOptions ReportViewModel::htmlOptions() const {
    core::HtmlReportOptions options;
    options.language = htmlLanguage();
    options.maskSerials = impl_->options.maskSerials;
    options.showDisks = impl_->options.includeDisks;
    options.showPartitionMap = impl_->options.includeDisks;
    options.showCandidates = impl_->options.includeCandidates;
    options.showReasons = impl_->options.includeCandidates;
    return options;
}

bool ReportViewModel::canExport() const noexcept { return impl_->report != nullptr; }

std::string ReportViewModel::exportText(ExportFormat format) const {
    if (impl_->report == nullptr) return {};
    const core::Report& snapshot = *impl_->report;
    if (format == ExportFormat::Html) {
        return core::renderHtmlReport(toHtmlInput(snapshot, impl_->options), htmlOptions());
    }
    if (format == ExportFormat::Text) {
        return renderTextDump(snapshot, impl_->options);
    }
    // JSON. Копия на время экспорта: core::reportToJson берёт опции из самого
    // отчёта, а экран позволяет их менять галочкой приватности. Снимок при этом
    // не трогается — он иммутабелен (§6.4), и «маскирование для показа» не
    // обязано менять содержимое отчёта.
    core::Report copy = snapshot;
    copy.options = impl_->options;
    return core::reportToJson(copy, 2);
}

std::string ReportViewModel::suggestedName(ExportFormat format, std::int64_t atUnix) const {
    if (atUnix <= 0) atUnix = nowUnix();
    const core::ReportKind kind = impl_->report != nullptr ? impl_->report->kind : core::ReportKind::Cleanup;
    return toUtf8(reportFileName(format, kind, atUnix));
}

// --- Состояние страницы --------------------------------------------------------

void ReportViewModel::applyPageState(const PageState& state) {
    if (state.splitterDip > 0) impl_->cardHeightDip = state.splitterDip;
    impl_->filters.search = state.filter;
    impl_->selectedKey = state.selectedKey;
    impl_->applyFilters();
    // Ключ из файла настроек мог остаться от прежнего снимка (операция не
    // попала в новый отчёт). Молча подсвечивать нечего — выделение снимается.
    if (!impl_->selectedKey.empty() && impl_->indexForKey(impl_->selectedKey) == kNoRow) impl_->selectedKey.clear();
}

PageState ReportViewModel::pageState() const {
    PageState state;
    state.selectedKey = impl_->selectedKey;
    state.filter = impl_->filters.search;
    state.splitterDip = impl_->cardHeightDip;
    return state;
}

// ---------------------------------------------------------------------------
// Раскладка (чистая арифметика: метрики, DPI, размеры клиента и ширины подписей)
// ---------------------------------------------------------------------------
//
// Порядок решения задачи тот же, что у CleanupLayout в view_cleanup.cpp, и
// порядок жертв тоже: сначала сжимаются отступы и зазоры, потом высота кнопки —
// последней и только если клиент меньше минимальной раскладки; ширина кнопки до
// минимума нажатия не падает никогда, потому что вместо цели нажатия у экрана
// не осталось бы ничего. Единственное отличие в политике: строки нижнего ряда
// выровнены по левому краю и имеют ширину по подписи, а не делят строку поровну,
// как на «Очистке». Подписи экспорта разной длины («Экспорт в HTML» против
// «Обновить»), и растянутая до общей ячейки кнопка выглядит панелью, которой
// нет.

// Прямоугольник непустой и ни в чём не вышел за клиент. Именно это обещание
// раскладки проверяют ворота окна, поэтому оно выражено функцией, а не
// «рассуждением в комментарии».
bool ReportRect::empty() const noexcept { return width <= 0 || height <= 0; }

int ReportLayout::rowWidth(const std::array<int, kReportBottomControls>& widths, int gap, int from,
                           int to) noexcept {
    if (from < 0 || to > static_cast<int>(kReportBottomControls) || from >= to) return 0;
    int used = 0;
    for (int i = from; i < to; ++i) {
        used += widths[static_cast<std::size_t>(i)];
        if (i > from) used += gap;
    }
    return used;
}

bool ReportLayout::rowFits(const std::array<int, kReportBottomControls>& widths, int gap, int available,
                           int from, int to) noexcept {
    return rowWidth(widths, gap, from, to) > 0 && rowWidth(widths, gap, from, to) <= available;
}

int ReportLayout::packRows(const std::array<int, kReportBottomControls>& widths, int gap, int available,
                           int& split) noexcept {
    const int count = static_cast<int>(kReportBottomControls);
    if (available <= 0 || gap < 0) return 0;
    // Одна строка: все семь контролов в ряд.
    if (rowFits(widths, gap, available, 0, count)) {
        split = count;
        return 1;
    }
    // Две строки: разрез перебирается целиком и берётся тот, где шире строка
    // меньше. Жадная укладка («набил первую строку до упора») оставила бы одну
    // строку из шести контролов и одинокую галочку внизу; минимум максимума
    // кладёт ряд ровнее и держит обе галочки рядом, когда они одинаковые.
    int best = 0;
    int bestWidest = 0;
    for (int cut = 1; cut < count; ++cut) {
        const int first = rowWidth(widths, gap, 0, cut);
        const int second = rowWidth(widths, gap, cut, count);
        if (first <= 0 || second <= 0) continue;
        if (first > available || second > available) continue;
        const int widest = std::max(first, second);
        if (best == 0 || widest < bestWidest) {
            best = cut;
            bestWidest = widest;
        }
    }
    if (best != 0) {
        split = best;
        return 2;
    }
    split = 0;
    return 0;
}

void ReportLayout::clampRow(std::array<int, kReportBottomControls>& widths, int count, int from, int gap,
                            int available) {
    if (count <= 0 || from < 0 || from + count > static_cast<int>(kReportBottomControls)) return;
    if (available <= 0) {
        for (int i = 0; i < count; ++i) widths[static_cast<std::size_t>(from + i)] = 0;
        return;
    }
    int total = 0;
    for (int i = 0; i < count; ++i) total += widths[static_cast<std::size_t>(from + i)];
    const int want = total + gap * (count - 1);
    if (want <= available) return;
    // Деление по остатку: доли от деления могут дать на 1…count-1 меньше, чем
    // бюджет, и «добавим остаток последней кнопке» выкинуло бы её за край.
    const int budget = std::max(0, available - gap * (count - 1));
    int assigned = 0;
    std::array<int, kReportBottomControls> shares{};  // остаток доли по индексу
    for (int i = 0; i < count; ++i) {
        const long long numerator =
            static_cast<long long>(widths[static_cast<std::size_t>(from + i)]) * budget;
        const int share = static_cast<int>(numerator / std::max(1, total));
        widths[static_cast<std::size_t>(from + i)] = share;
        shares[static_cast<std::size_t>(from + i)] = static_cast<int>(numerator % std::max(1, total));
        assigned += share;
    }
    std::array<int, kReportBottomControls> order{};
    for (int i = 0; i < count; ++i) order[static_cast<std::size_t>(i)] = from + i;
    std::stable_sort(order.begin(), order.begin() + count, [&shares](int a, int b) {
        return shares[static_cast<std::size_t>(a)] > shares[static_cast<std::size_t>(b)];
    });
    int extra = budget - assigned;
    for (int i = 0; i < count && extra > 0; ++i) {
        widths[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])] += 1;
        --extra;
    }
}

ReportLayout ReportLayout::compute(const ReportMetrics& metrics, int dpi, int clientWidthPx, int clientHeightPx,
                                   int cardHeightDip, const std::array<int, kReportBottomControls>& natural) {
    ReportLayout out;
    out.width_ = std::max(0, clientWidthPx);
    out.height_ = std::max(0, clientHeightPx);
    const int count = static_cast<int>(kReportBottomControls);
    const theme::Metrics scale = theme::metricsForDpi(dpi > 0 ? static_cast<unsigned>(dpi) : 96);
    const auto px = [&scale](double dip) { return dip <= 0.0 ? 0 : scale.dip(dip); };

    out.cramped_ = out.width_ < px(metrics.minWidthDip) || out.height_ < px(metrics.minHeightDip);

    // Отступы и зазоры сжимаются первыми: в тесном окне воздух вокруг кнопки
    // важнее самой кнопки, и он ужимается вместе с остальным.
    out.padding_ = std::clamp(px(metrics.paddingDip), 0, out.width_ / 4);
    const int available = std::max(0, out.width_ - 2 * out.padding_);
    out.gap_ = std::clamp(px(metrics.gapDip), 0, std::max(0, available / (2 * count)));

    const int minWidth = std::max(1, px(metrics.minButtonWidthDip));
    std::array<int, kReportBottomControls> widths{};
    for (int i = 0; i < count; ++i) {
        // Подпись короче минимума нажатия — это не подпись, а полоса: такой
        // контрол всё равно получил бы минимум при раскладке.
        widths[static_cast<std::size_t>(i)] = std::max(minWidth, natural[static_cast<std::size_t>(i)]);
    }

    int split = 0;
    int rows = packRows(widths, out.gap_, available, split);
    if (rows == 0) {
        // Ни одна строка не помещается даже в две. Сжимаем пропорционально и
        // ищем наибольшую долю подписей, при которой ряд влезает: доля
        // монотонна (ужимаем — влезает всё больше), поэтому достаточно
        // деления отрезка пополам, а не перебора шагами.
        std::array<int, kReportBottomControls> best{};
        double low = 0.0;
        double high = 1.0;
        std::array<int, kReportBottomControls> probe{};
        const auto scaled = [&](double factor, std::array<int, kReportBottomControls>& into) {
            for (int i = 0; i < count; ++i) {
                const double wanted = static_cast<double>(std::max(0, natural[static_cast<std::size_t>(i)])) * factor;
                into[static_cast<std::size_t>(i)] = std::max(minWidth, static_cast<int>(wanted + 0.5));
            }
        };
        for (int step = 0; step < 16; ++step) {
            const double middle = (low + high) / 2.0;
            scaled(middle, probe);
            int cut = 0;
            if (packRows(probe, out.gap_, available, cut) != 0) {
                low = middle;
                best = probe;
            } else {
                high = middle;
            }
        }
        if (low > 0.0) {
            widths = best;
            rows = packRows(widths, out.gap_, available, split);
        }
    }
    if (rows == 0) {
        // Даже минимумы нажатия шире строки. Тогда ряд всё равно раскладывается в
        // две строки, но ширины делятся жёстко: цель нажатия остаётся на месте,
        // подпись обрезается, и — главное — ничего не выходит за клиент.
        out.squeezed_ = true;
        int bestCut = 0;
        int bestWidest = 0;
        for (int cut = 1; cut < count; ++cut) {
            std::array<int, kReportBottomControls> candidate = widths;
            clampRow(candidate, cut, 0, out.gap_, available);
            clampRow(candidate, count - cut, cut, out.gap_, available);
            const int widest = std::max(rowWidth(candidate, out.gap_, 0, cut),
                                        rowWidth(candidate, out.gap_, cut, count));
            if (bestCut == 0 || widest < bestWidest) {
                bestCut = cut;
                bestWidest = widest;
                widths = candidate;
            }
        }
        split = bestCut;
        rows = 2;
    }
    out.rows_ = std::max(1, rows);
    out.split_ = std::clamp(split, 1, count - 1);

    // Высота: ряд прижат к низу клиента и не наезжает на строку состояния.
    // Места в клиенте может не хватить — тогда ужимается высота кнопки, и
    // только когда и её не хватает, ряд скрывается целиком (пустой
    // прямоугольник — это «спрятать»). Прямоугольники при этом остаются
    // неотрицательными и внутри клиента.
    const int minButtonHeight = std::max(1, px(kMinButtonHeightDip));
    const int wantedHeight = std::max(0, px(metrics.buttonHeightDip));
    const int statusWanted = std::max(0, px(metrics.statusHeightDip));
    // Сверху вниз полос ровно три: список, карточка деталей, строка состояния, и
    // между ними три зазора — в том числе между строкой состояния и рядом
    // кнопок. Зазоров было два в первой версии раскладки, и список с карточкой
    // отдавали рядом недостающий зазор: кнопки нижней строки наезжали на строку
    // состояния (проверено на окне 900x600: строка 475…495, кнопка 473…501).
    const int stackGaps = 3 * out.gap_;
    const int reserve = 2 * out.padding_ + stackGaps + statusWanted;
    int buttonHeight = wantedHeight;
    if (out.rows_ * buttonHeight + (out.rows_ - 1) * out.gap_ + reserve > out.height_) {
        buttonHeight = std::max(0, (out.height_ - reserve - (out.rows_ - 1) * out.gap_) / out.rows_);
    }
    out.buttonHeight_ = buttonHeight > 0 ? std::min(buttonHeight, out.height_) : 0;
    if (out.buttonHeight_ > 0 && out.buttonHeight_ < minButtonHeight) out.buttonHeight_ = minButtonHeight;
    if (out.buttonHeight_ * out.rows_ + (out.rows_ - 1) * out.gap_ + reserve > out.height_) out.rows_ = 0;

    const int bandHeight = out.rows_ * out.buttonHeight_ + std::max(0, out.rows_ - 1) * out.gap_;
    // Когда ряд скрыт, зазор над ним не нужен: иначе пустая полоса съела бы
    // высоту списка в окне, которое и так ниже минимального.
    const int gaps = bandHeight > 0 ? stackGaps : 2 * out.gap_;
    const int budget = std::max(0, out.height_ - 2 * out.padding_ - gaps - bandHeight);
    const int statusHeight = std::min(statusWanted, budget);
    int free = std::max(0, budget - statusHeight);

    // Карточка деталей уступает место журналу: если строк мало, карточка всё
    // равно показывает одну выделенную строку, а журнал без строк — пустой
    // список. Порядок «сначала карточка, потом список» сохранён, но список
    // получает минимум первым.
    const int minCard = std::max(0, px(metrics.minCardHeightDip));
    const int minList = std::max(0, px(metrics.minListHeightDip));
    int cardHeight = std::min(std::max(minCard, px(static_cast<double>(std::max(0, cardHeightDip)))),
                              std::max(minCard, free));
    int listHeight = std::max(0, free - cardHeight);
    if (listHeight < minList && cardHeight > 0) {
        const int give = std::min(cardHeight, minList - listHeight);
        cardHeight -= give;
        listHeight += give;
    }

    int cursor = out.padding_;
    out.list_ = ReportRect{out.padding_, cursor, available, listHeight};
    cursor += listHeight + out.gap_;
    out.card_ = ReportRect{out.padding_, cursor, available, cardHeight};
    cursor += cardHeight + out.gap_;
    out.status_ = ReportRect{out.padding_, cursor, available, statusHeight};
    cursor += statusHeight + out.gap_;

    // Ряд кнопок прижат к низу клиента: его верхняя граница равна ровно
    // cursor — сумма полос выше плюс три зазора плюс отступ, — и совпадает с
    // height_ - padding_ - bandHeight, потому что список забрал ровно остаток
    // бюджета. Нижняя строка поэтому начинается с cursor + bandHeight - height и
    // идёт вверх: каждая выше на кнопку и зазор. Правый край любой кнопки не
    // превышает available по построению (packRows/clampRow), поэтому вылет за
    // правый край клиента невозможен ни при какой ширине и любом языке.
    int rowTop = cursor + bandHeight;
    for (int row = out.rows_ - 1; row >= 0; --row) {
        rowTop -= out.buttonHeight_;
        const int from = (row == 0) ? 0 : out.split_;
        const int to = (row == 0) ? out.split_ : count;
        int x = out.padding_;
        for (int i = from; i < to; ++i) {
            out.children_[static_cast<std::size_t>(i)] =
                ReportRect{x, rowTop, widths[static_cast<std::size_t>(i)], out.buttonHeight_};
            x += widths[static_cast<std::size_t>(i)] + out.gap_;
        }
        if (row > 0) rowTop -= out.gap_;
    }
    return out;
}

ReportRect ReportLayout::listRect() const noexcept { return list_; }
ReportRect ReportLayout::statusRect() const noexcept { return status_; }
ReportRect ReportLayout::cardRect() const noexcept { return card_; }

ReportRect ReportLayout::childRect(int index) const noexcept {
    if (index < 0 || index >= static_cast<int>(kReportBottomControls)) return ReportRect{};
    return children_[static_cast<std::size_t>(index)];
}

int ReportLayout::rows() const noexcept { return rows_; }
bool ReportLayout::squeezed() const noexcept { return squeezed_; }
bool ReportLayout::cramped() const noexcept { return cramped_; }
int ReportLayout::paddingPx() const noexcept { return padding_; }
int ReportLayout::clientWidthPx() const noexcept { return width_; }
int ReportLayout::clientHeightPx() const noexcept { return height_; }

// ---------------------------------------------------------------------------
// Хранилище отчётов (FR-8: «%LOCALAPPDATA%\MrProper\reports\, последние 20»)
// ---------------------------------------------------------------------------

namespace {

// Каталог из %LOCALAPPDATA%. Переменная окружения, а не SHGetKnownFolderPath:
// последняя живёт в shell32, а подключать новую системную библиотеку из слоя
// интерфейса нельзя без правки CMakeLists.txt. Значение у неё то же — каталог
// профиля пользователя, — а при пустой переменной берётся %USERPROFILE%\
// AppData\Local: сломанный профиль не должен оставлять экран без отчётов.
std::wstring environmentDirectory(const wchar_t* name) {
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        value.clear();
        return value;
    }
    value.resize(written);
    while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    return value;
}

std::wstring reportsDirectory() {
    std::wstring root = environmentDirectory(L"LOCALAPPDATA");
    if (root.empty()) {
        const std::wstring profile = environmentDirectory(L"USERPROFILE");
        if (profile.empty()) return {};
        root = profile + L"\\AppData\\Local";
    }
    return root + L"\\MrProper\\reports";
}

// Создать каталог вместе со всеми родителями. Свое перечисление уровней, а не
// SHCreateDirectoryEx: та живёт в shlwapi, а включение ещё одной системной
// библиотеки — правка CMakeLists.txt, которая мне не принадлежит (§6.1).
bool ensureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    std::wstring partial;
    partial.reserve(path.size());
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != L'\\' && path[i] != L'/') {
            partial.push_back(path[i]);
            continue;
        }
        if (partial.size() < 3) {  // «C:» — корень уже существует
            partial.push_back(L'\\');
            continue;
        }
        // Префикс, который уже каталог, не создаём: так не ломается UNC-корень
        // («\\сервер\общая») и не тратится вызов CreateDirectoryW впустую.
        const DWORD existing = ::GetFileAttributesW(partial.c_str());
        if (existing != INVALID_FILE_ATTRIBUTES && (existing & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (i < path.size()) partial.push_back(L'\\');
            continue;
        }
        if (::CreateDirectoryW(partial.c_str(), nullptr) == FALSE) {
            const DWORD code = ::GetLastError();
            if (code != ERROR_ALREADY_EXISTS) {
                logWin32("ui.report.mkdir", "CreateDirectoryW", code);
                return false;
            }
        }
        if (i < path.size()) partial.push_back(L'\\');
    }
    return true;
}

// Записать файл целиком. Одна запись без CreateFileW-цикла: отчёт — сотни
// килобайт, а FILE_SHARE_READ позволяет читать уже записанное, пока пишется
// остальное.
bool writeFile(const std::wstring& path, std::string_view text) {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const char* data = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        // Ограничение WinAPI — DWORD: остаток больше 4 ГБ невозможен (файл
        // больше 512 МБ — это не отчёт, а чужой файл), но приведение всё равно
        // проверяется, чтобы «молча обрезанный отчёт» не появился.
        const DWORD chunk = static_cast<DWORD>(left > 0x10000000u ? 0x10000000u : left);
        DWORD written = 0;
        if (::WriteFile(handle, data, chunk, &written, nullptr) == FALSE || written == 0) {
            ::CloseHandle(handle);
            return false;
        }
        data += written;
        left -= written;
    }
    return ::CloseHandle(handle) != FALSE;
}

bool isOurReportName(std::wstring_view name) {
    constexpr std::size_t prefixLength = (sizeof(kReportFilePrefix) / sizeof(wchar_t)) - 1u;
    if (name.size() < prefixLength + 5u) return false;  // префикс + метка + «.html»
    return name.compare(0, prefixLength, kReportFilePrefix) == 0;
}

std::wstring joinPath(const std::wstring& directory, std::wstring_view name) {
    std::wstring path(directory);
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    path.append(name);
    return path;
}

// Наши отчёты, новые первыми. Порядок — по имени, а оно начинается с метки
// времени, поэтому «новые первыми» получается без чтения атрибутов каждого
// файла. Сортировка убывающая: список для человека, который ищет «последний
// отчёт».
std::vector<std::wstring> ourReports(const std::wstring& directory) {
    std::vector<std::wstring> names;
    const std::wstring pattern = joinPath(directory, L"*");
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return names;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const std::wstring_view name(found.cFileName);
        if (!isOurReportName(name)) continue;
        names.emplace_back(name);
    } while (::FindNextFileW(search, &found) != FALSE);
    ::FindClose(search);
    std::sort(names.begin(), names.end(), [](const std::wstring& left, const std::wstring& right) {
        return left > right;  // убывание: самый свежий первым
    });
    return names;
}

}  // namespace

std::string reportsDirectoryUtf8() {
    const std::wstring path = reportsDirectory();
    return path.empty() ? std::string() : platform::toUtf8(path);
}

std::vector<std::string> listReportsUtf8() {
    const std::wstring directory = reportsDirectory();
    std::vector<std::string> out;
    if (directory.empty()) return out;
    for (const std::wstring& name : ourReports(directory)) {
        out.push_back(platform::toUtf8(name));
    }
    return out;
}

SaveOutcome saveReport(std::string_view text, ExportFormat format, core::ReportKind kind, std::int64_t atUnix) {
    SaveOutcome outcome;
    if (text.empty()) {
        outcome.errorText = "пустой отчёт";
        return outcome;
    }
    const std::wstring directory = reportsDirectory();
    if (directory.empty()) {
        outcome.errorText = "не удалось определить %LOCALAPPDATA%";
        logEvent(core::LogLevel::Warn, "ui.report.save", outcome.errorText);
        return outcome;
    }
    if (!ensureDirectory(directory)) {
        const platform::WinErrorInfo info = platform::lastErrorInfo("CreateDirectoryW", platform::toUtf8(directory));
        outcome.errorText = info.toString();
        logSave("ui.report.save", info);
        return outcome;
    }
    if (atUnix <= 0) atUnix = nowUnix();

    const std::wstring baseName = reportFileName(format, kind, atUnix);
    std::wstring path = joinPath(directory, baseName);
    // Коллизия по имени (два сохранения в одну секунду) разрешается суффиксом
    // со счётчиком: перезаписывать отчёт, который человек уже открыл, нельзя.
    for (int attempt = 1; attempt < 100; ++attempt) {
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        path = joinPath(directory, baseName + L"." + std::to_wstring(attempt));
    }

    if (!writeFile(path, text)) {
        const platform::WinErrorInfo info = platform::lastErrorInfo("CreateFileW", platform::toUtf8(path));
        outcome.errorText = info.toString();
        logSave("ui.report.save", info);
        return outcome;
    }
    outcome.ok = true;
    outcome.pathUtf8 = platform::toUtf8(path);

    // Ротация сразу после записи: «последние 20» должно быть правдой и на диске,
    // а не «когда-нибудь при следующем запуске». kept считается после удаления, и
    // потому совпадает с тем, что человек увидит в каталоге.
    outcome.pruned = pruneReports(kMaxKeptReports);
    outcome.kept = ourReports(directory).size();

    core::LogFields fields;
    fields.push_back(core::logField("path", outcome.pathUtf8));
    fields.push_back(core::logField("format", toString(format)));
    fields.push_back(core::logField("bytes", static_cast<std::uint64_t>(text.size())));
    fields.push_back(core::logField("kept", static_cast<std::uint64_t>(outcome.kept)));
    fields.push_back(core::logField("pruned", static_cast<std::uint64_t>(outcome.pruned)));
    core::Logger::instance().write(core::LogLevel::Info, "ui.report.saved", "report written", std::move(fields));
    return outcome;
}

std::size_t pruneReports(std::size_t keep) {
    const std::wstring directory = reportsDirectory();
    if (directory.empty()) return 0;
    const std::vector<std::wstring> names = ourReports(directory);
    if (names.size() <= keep) return 0;
    std::size_t pruned = 0;
    for (std::size_t i = keep; i < names.size(); ++i) {
        const std::wstring path = joinPath(directory, names[i]);
        if (::DeleteFileW(path.c_str()) != FALSE) {
            ++pruned;
            continue;
        }
        const DWORD code = ::GetLastError();
        // Файл мог быть открыт человеком (отчёт держат в блокноте). Это не
        // ошибка ротации, но и не повод молчать: причина уходит в журнал.
        logWin32("ui.report.prune", "DeleteFileW", code);
    }
    return pruned;
}

// ---------------------------------------------------------------------------
// Окно экрана
// ---------------------------------------------------------------------------

namespace detail {

constexpr wchar_t kReportViewClass[] = L"MrProper.ReportView";

// «Перерисовать из модели». Номер не совпадает с сообщениями соседних экранов
// (WM_APP + 1 у «Очистки», WM_APP + 21 у «Дисков»): окна всех экранов — дети
// одного хоста, и совпадающие номера означали бы, что отложенная перерисовка
// одного экрана задевает другой.
constexpr UINT kMsgSyncModel = WM_APP + 41;

// Как часто экран дочитывает кольцо журнала. §6.4 говорит «раз в 100 мс» про
// счётчики прогресса; здесь данные меняются медленно (одна запись на операцию), а
// цена — перестроение строк, поэтому полсекунды: глаз не видит разницы, а список
// не дёргается зря.
constexpr UINT_PTR kTimerJournal = 1;
constexpr UINT kJournalIntervalMs = 500;

// Подстадия отрисовки подпункта в NM_CUSTOMDRAW списка. В commctrl.h этого SDK
// объявлены только CDDS_PREPAINT/CDDS_ITEMPREPAINT/CDDS_ITEMPOSTPAINT, а
// comctl32 подстадии подпункта шлёт и документирует их (MSDN, Custom Draw
// Controls). Значение взято оттуда и объявлено явно, с проверкой в журнал: если
// бы константа разошлась с реальностью, список молча потерял бы свои числа.
inline constexpr DWORD kSubItemPrePaint = 0x00000100;  // CDDS_SUBITEMPREPAINT

// Идентификаторы дочерних окон. Диапазон не пересекается с соседними экранами:
// WM_DRAWITEM разбирает их по числу, а не по имени.
enum : int {
    kChildList = 51,
    kChildStatus = 52,
    kChildCard = 53,
    kChildHint = 54,
};

// Колонки журнала. Порядок — от «когда» к «чему»: журнал читают слева направо по
// времени, и объём в конце уже не мешает.
enum : int {
    kColumnTime = 0,
    kColumnKind,
    kColumnSource,
    kColumnSubject,
    kColumnStatus,
    kColumnSize,
    kColumnCount,
};

// Ширина колонки в DIP. Объём и вид — фиксированные, источник и «что сделано»
// тянутся на остаток: длинный путь важнее, чем ровные колонки.
constexpr double kColumnWidthDip[kColumnCount] = {92.0, 84.0, 132.0, 320.0, 120.0, 104.0};

struct ViewState;

// Процедуры окон объявлены здесь, а определены ниже: ViewState ссылается на них
// в createChild, и объявление после структуры дало бы «childProc: необъявленный
// идентификатор» в самом невинном месте.
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

ViewState* stateOf(HWND window) {
    return reinterpret_cast<ViewState*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

// Подкласс контрола: исходная процедура и сам контрол. Владеет ими экран.
struct ChildProc {
    HWND hwnd{nullptr};
    WNDPROC prev{nullptr};
};

struct ViewState {
    ReportScreen::Callbacks callbacks;
    ReportViewModel model;
    theme::Theme theme;
    // Раскладка живёт в метриках экрана, а не в константах layout(): на неё
    // смотрят и шрифты (высота строки), и ворота окна. Значения по умолчанию
    // те же, что были константами до выделения раскладки.
    ReportMetrics metrics;
    int dpi{96};

    HWND window{nullptr};
    HWND list{nullptr};
    HWND status{nullptr};
    HWND card{nullptr};
    HWND hint{nullptr};
    std::array<HWND, 5> buttons{};
    std::array<HWND, 2> checks{};

    // Шрифты: список (полужирный — строки журнала заметнее подписей), кнопки и
    // строки карточки (обычный), заголовок карточки (крупный).
    std::array<HFONT, 3> fonts{};
    HBRUSH surfaceBrush{nullptr};
    std::vector<ChildProc> children;

    // Последняя неудача сохранения. Показывается в строке состояния, а не в
    // MessageBox: модальное окно поверх экрана, который человек открыл из
    // разговора об ошибке, только мешает. В журнале причина уже есть.
    std::string lastProblem;
    // Каталог отчётов для кнопки «Открыть папку». Считается один раз при
    // создании окна: GetEnvironmentVariable и сборка пути на каждую перерисовку
    // — это работа, которой не видно, но которая повторяется на каждом
    // обновлении списка.
    std::string reportsDirectory;
    bool syncing{false};
    bool controlsReady{false};
    bool columnsReady{false};
    bool drawStageLogged{false};
    bool squeezeLogged{false};
    bool timerRunning{false};

    ~ViewState() {
        for (HFONT& font : fonts) {
            if (font != nullptr) ::DeleteObject(font);
        }
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

    void place(HWND child, const RECT& rect, bool visible) {
        if (child == nullptr) return;
        if (!visible || rect.right <= rect.left || rect.bottom <= rect.top) {
            ::ShowWindow(child, SW_HIDE);
            return;
        }
        ::SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(child, SW_SHOW);
    }

    // Тот же place для прямоугольника раскладки. Отдельная функция, а не перевод
    // в RECT на месте вызова: у раскладки своя геометрия (x/y/width/height), и
    // пересчитывать её вручную в семи местах — это семь chances забыть одно
    // поле. Пустой прямоугольник раскладки означает «спрятать» ровно так же,
    // как пустой RECT.
    void placeRect(HWND child, const ReportRect& rect, bool visible) {
        if (child == nullptr) return;
        if (!visible || rect.empty()) {
            ::ShowWindow(child, SW_HIDE);
            return;
        }
        place(child, RECT{rect.x, rect.y, rect.right(), rect.bottom()}, true);
    }

    // Прямоугольник, вписанный в другой на отступ. Пустой прямоугольник в place()
    // означает «спрятать», поэтому пустота отступа должна быть явно отрицательной,
    // а не «нулевой».
    static RECT insetRect(const RECT& outer, int pad) noexcept {
        RECT inset{outer.left + pad, outer.top + pad, outer.right - pad, outer.bottom - pad};
        if (inset.right <= inset.left) inset.right = outer.right;
        if (inset.bottom <= inset.top) inset.bottom = outer.bottom;
        return inset;
    }

    // --- Раскладка ----------------------------------------------------------

    // Ширина кнопки по её подписи. Подписи локализованы и разной длины
    // («Экспорт в HTML» против «Текстовый дамп»), а одинаковые кнопки в Windows
    // выглядят как самодельная панель: ширина по тексту стоит одного GetTextExtent.
    int buttonWidth(HWND button, int fallback) const {
        if (button == nullptr) return fallback;
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const int pad = scale.dip(metrics.buttonTextPadDip);
        HDC dc = ::GetDC(button);
        if (dc == nullptr) return fallback;
        const int length = ::GetWindowTextLengthW(button);
        RECT measured{};
        int width = fallback;
        if (length > 0) {
            std::wstring buffer(static_cast<std::size_t>(length) + 1u, L'\0');
            const int copied = ::GetWindowTextW(button, buffer.data(), length + 1);
            const int measuredWidth =
                ::DrawTextW(dc, buffer.c_str(), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            if (copied > 0 && measuredWidth != 0) {
                width = measured.right - measured.left + pad;
            }
        }
        ::ReleaseDC(button, dc);
        return width;
    }

    void layout() {
        if (window == nullptr) return;
        RECT client{};
        if (::GetClientRect(window, &client) == FALSE) return;
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        const int clientWidth = static_cast<int>(client.right) - static_cast<int>(client.left);
        const int clientHeight = static_cast<int>(client.bottom) - static_cast<int>(client.top);

        // Ширина подписи — единственное, что здесь меряется шрифтом (нужен
        // HWND). Дальше считает чистая раскладка, как на «Очистке»: она
        // переносит ряд на две строки, сжимает пропорционально и не даёт ни
        // одной кнопке выйти за клиент ни при какой ширине, языке и DPI.
        std::array<int, kReportBottomControls> natural{};
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            natural[i] = buttonWidth(buttons[i], scale.dip(120.0));
        }
        for (std::size_t i = 0; i < checks.size(); ++i) {
            natural[buttons.size() + i] = buttonWidth(checks[i], scale.dip(150.0));
        }
        const ReportLayout layout = ReportLayout::compute(metrics, dpi, clientWidth, clientHeight,
                                                          model.cardHeightDip(), natural);

        placeRect(list, layout.listRect(), true);
        placeRect(status, layout.statusRect(), true);
        placeRect(card, layout.cardRect(), true);
        // Пояснение живёт поверх списка ровно тогда, когда строк нет. Поверх, а
        // не вместо: список остаётся на месте и наполняется, когда приходят
        // строки, — вёрстка не прыгает при первом же событии журнала.
        const bool rows = model.rowCount() > 0;
        const ReportRect journal = layout.listRect();
        const RECT listRect{journal.x, journal.y, journal.right(), journal.bottom()};
        place(hint, rows ? RECT{} : insetRect(listRect, layout.paddingPx()), !rows);
        if (!rows) ::SetWindowPos(hint, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        for (std::size_t i = 0; i < buttons.size(); ++i) {
            placeRect(buttons[i], layout.childRect(static_cast<int>(i)), true);
        }
        for (std::size_t i = 0; i < checks.size(); ++i) {
            placeRect(checks[i], layout.childRect(static_cast<int>(buttons.size() + i)), true);
        }

        // Перенос на вторую строку — это не авария: он молчалив и не пишет в
        // журнал. А вот сжатие до минимума, где подпись уже обрезана, — да:
        // человек должен знать, почему «Экспорт в HTML» стал «Экспорт в…».
        // Одно сообщение на переход, а не на каждый WM_SIZE (их при
        // перетаскивании окна сотни).
        if (layout.squeezed() != squeezeLogged) {
            squeezeLogged = layout.squeezed();
            if (squeezeLogged) {
                logEvent(core::LogLevel::Warn, "ui.report.layout",
                         "bottom row labels squeezed to minimum width");
            }
        }
    }

    // --- Тема ---------------------------------------------------------------

    void applyPalette() {
        const theme::Palette& palette = theme.palette();
        if (surfaceBrush != nullptr) ::DeleteObject(surfaceBrush);
        surfaceBrush = ::CreateSolidBrush(theme::colorRef(palette.surface));
        if (list != nullptr) {
            ListView_SetBkColor(list, theme::colorRef(palette.surface));
            ListView_SetTextColor(list, theme::colorRef(palette.textPrimary));
            // Подтемы «тёмного» у нативных контролов нет в документированном
            // API (ADR-003); модуль темы делает это через безопасные вызовы
            // uxtheme, а отказ — не повод оставлять список белым.
            (void)theme::enableDarkModeForWindow(list, theme.scheme());
        }
    }

    void applyFonts() {
        // Сначала создаём новые шрифты, потом перевешиваем их на контролы и
        // только потом удаляем старые: удалить HFONT, висящий на контроле, —
        // значит оставить контрол со шрифтом в никуда.
        const std::array<theme::FontRole, 3> roles{theme::FontRole::BodyStrong, theme::FontRole::Caption,
                                                  theme::FontRole::Subtitle};
        std::array<HFONT, 3> next{};
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const LOGFONTW description = theme.font(roles[i]).toLogFont(static_cast<unsigned>(dpi));
            next[i] = ::CreateFontIndirectW(&description);
        }
        if (list != nullptr) ::SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(next[0]), TRUE);
        if (status != nullptr) ::SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        if (card != nullptr) ::SendMessageW(card, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        for (const HWND button : buttons) {
            if (button != nullptr) ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        }
        for (const HWND check : checks) {
            if (check != nullptr) ::SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(next[1]), TRUE);
        }
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (fonts[i] != nullptr) ::DeleteObject(fonts[i]);
            fonts[i] = next[i];
        }
    }

    // --- Наполнение контролов ------------------------------------------------

    // Ширина и выравнивание задаются один раз: вставка шести колонок на каждой
    // перерисовке пересчитывала бы заголовки впустую. Подписи колонок, наоборот,
    // обновляются в syncTexts — они зависят от языка интерфейса.
    void syncColumns() {
        if (list == nullptr || columnsReady) return;
        const theme::Metrics scale = theme::metricsForDpi(static_cast<unsigned>(dpi));
        for (int column = 0; column < kColumnCount; ++column) {
            LVCOLUMNW entry{};
            entry.mask = LVCF_FMT | LVCF_WIDTH;
            entry.fmt = (column == kColumnSize) ? LVCFMT_RIGHT : LVCFMT_LEFT;
            entry.cx = scale.dip(kColumnWidthDip[static_cast<std::size_t>(column)]);
            ListView_InsertColumn(list, column, &entry);
        }
        columnsReady = true;
    }

    void syncTexts() {
        if (list != nullptr) {
            const std::array<std::string, kColumnCount> titles{word(Word::ColumnTime), word(Word::ColumnKind),
                                                               word(Word::ColumnSource), word(Word::ColumnSubject),
                                                               word(Word::ColumnStatus), word(Word::ColumnSize)};
            for (int column = 0; column < kColumnCount; ++column) {
                const std::wstring wide = toWide(titles[static_cast<std::size_t>(column)]);
                LVCOLUMNW entry{};
                entry.mask = LVCF_TEXT;
                entry.pszText = const_cast<wchar_t*>(wide.c_str());
                ListView_SetColumn(list, column, &entry);
            }
        }
        // Подписи кнопок идут по порядку массивов, который совпадает с порядком
        // ControlId: иначе «Экспорт в HTML» оказался бы на кнопке JSON.
        const std::array<StringId, 5> buttonLabels{StringId::kReportExportHtml, StringId::kReportExportJson,
                                                   StringId::kReportExportText, StringId::kActionRefresh,
                                                   StringId::kActionOpenInExplorer};
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            setChildText(buttons[i], tr(buttonLabels[i]));
        }
        // Галочки приватности и фильтра подписаны словами экрана: в каталоге
        // локализации таких ключей нет, а «Хранятся последние отчёты: 20» в
        // качестве подписи галочки читалось бы как отчёт, а не как действие.
        if (checks[0] != nullptr) setChildText(checks[0], word(Word::MaskSerials));
        if (checks[1] != nullptr) setChildText(checks[1], word(Word::OnlyProblems));
    }

    void syncList() {
        if (list == nullptr) return;
        syncing = true;
        const std::size_t selected = model.selectedIndex();
        const std::size_t count = model.rowCount();
        // LVSICF_NOINVALIDATEALL здесь не нужен: строки всё равно заполняются
        // заново, и без перерисовки список остался бы с прошлыми подписями на
        // фоне новых данных.
        ListView_SetItemCountEx(list, static_cast<int>(count), LVSICF_NOSCROLL);
        for (std::size_t i = 0; i < count; ++i) {
            const JournalRow* row = model.rowAt(i);
            if (row == nullptr) continue;
            setSubitemText(list, static_cast<int>(i), kColumnTime, row->timeText);
            setSubitemText(list, static_cast<int>(i), kColumnKind, row->kindText);
            setSubitemText(list, static_cast<int>(i), kColumnSource, row->sourceText);
            setSubitemText(list, static_cast<int>(i), kColumnSubject, row->subjectText);
            setSubitemText(list, static_cast<int>(i), kColumnStatus, row->statusText);
            setSubitemText(list, static_cast<int>(i), kColumnSize, row->bytesText);
        }
        if (selected == kNoRow || selected >= count) {
            ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        } else {
            ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            const UINT state = LVIS_SELECTED | LVIS_FOCUSED;
            ListView_SetItemState(list, static_cast<int>(selected), state, state);
            ListView_EnsureVisible(list, static_cast<int>(selected), FALSE);
        }
        // Модель должна знать размер страницы: PgUp/PgDn прыгают на столько
        // строк, сколько влезло в список, а не на фиксированные девятнадцать.
        const int visible = static_cast<int>(ListView_GetCountPerPage(list));
        model.setViewLines(visible > 0 ? visible : 20);
        syncing = false;
    }

    void syncButtons() {
        const bool canExport = model.canExport();
        if (buttons[0] != nullptr) ::EnableWindow(buttons[0], canExport ? TRUE : FALSE);
        if (buttons[1] != nullptr) ::EnableWindow(buttons[1], canExport ? TRUE : FALSE);
        if (buttons[2] != nullptr) ::EnableWindow(buttons[2], canExport ? TRUE : FALSE);
        if (buttons[3] != nullptr) {
            // Обновить без обработчика нечего: кнопка выключена, а не «молча
            // ничего не делает».
            const bool canRefresh = static_cast<bool>(callbacks.onRefresh);
            ::EnableWindow(buttons[3], canRefresh ? TRUE : FALSE);
        }
        if (buttons[4] != nullptr) {
            const bool canOpen = static_cast<bool>(callbacks.onOpenFolder) && !reportsDirectory.empty();
            ::EnableWindow(buttons[4], canOpen ? TRUE : FALSE);
        }
        if (checks[0] != nullptr) {
            // BS_AUTOCHECKBOX снимает галочку сам по клику; состояние из модели
            // переносим сюда, иначе строка состояния врала бы о приватности.
            ::SendMessageW(checks[0], BM_SETCHECK, model.maskSerials() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (checks[1] != nullptr) {
            const bool only = model.filters().problemsOnly;
            ::SendMessageW(checks[1], BM_SETCHECK, only ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    void syncStatus() {
        std::string text = model.statusText();
        const std::string saved = model.savedText();
        if (!saved.empty()) {
            text += pick(" · ", " · ");
            text += saved;
        }
        if (!lastProblem.empty()) {
            text += pick(" · ", " · ");
            text += lastProblem;
        }
        setChildText(status, text);
    }

    void refreshAll() {
        if (!controlsReady) return;
        syncColumns();
        syncTexts();
        syncList();
        syncButtons();
        syncStatus();
        layout();
        if (card != nullptr) ::InvalidateRect(card, nullptr, FALSE);
    }

    // Выделение меняет только карточку: пересобирать весь список на каждый щелчок
    // значило бы мигать на нём.
    void refreshSelection() {
        if (!controlsReady) return;
        if (card != nullptr) ::InvalidateRect(card, nullptr, FALSE);
        syncStatus();
    }

    bool createControls(HINSTANCE instance) {
        const DWORD childVisible = WS_CHILD | WS_VISIBLE;
        list = ::CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                                 childVisible | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                     LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                                 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kChildList), instance, this);
        const DWORD staticStyle = childVisible | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX;
        status = ::CreateWindowExW(0, L"STATIC", nullptr, staticStyle, 0, 0, 0, 0, window,
                                   reinterpret_cast<HMENU>(kChildStatus), instance, this);
        card = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_OWNERDRAW, 0, 0, 0, 0, window,
                                 reinterpret_cast<HMENU>(kChildCard), instance, this);
        // Пояснение вместо пустого списка. Создаётся всегда, показывается только
        // когда строк нет: пустой SysListView32 выглядит как «сломанная
        // программа», а «ничего не записано + что делать» — это содержимое
        // (SPEC §7.1, и требование волны: запрещена пустота).
        hint = ::CreateWindowExW(0, L"STATIC", nullptr, childVisible | SS_OWNERDRAW, 0, 0, 0, 0, window,
                                 reinterpret_cast<HMENU>(kChildHint), instance, this);
        // Порядок кнопок совпадает с ControlId: и то и другое — один перечень,
        // и синхронизировать их вручную нельзя.
        // Идентификатор кнопки — её ControlId (2201…): WM_COMMAND приносит
        // LOWORD(wParam) из идентификатора дочернего окна, и подставлять
        // отдельный диапазон значило бы развести две нумерации, которые потом
        // пришлось бы синхронизировать вручную. Порядок массивов совпадает с
        // порядком ControlId.
        const std::array<ControlId, 5> buttonIds{ControlId::ExportHtml, ControlId::ExportJson, ControlId::ExportText,
                                                 ControlId::RefreshJournal, ControlId::OpenFolder};
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            buttons[i] = ::CreateWindowExW(0, L"BUTTON", nullptr, childVisible | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0,
                                           0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(buttonIds[i])),
                                           instance, this);
        }
        const std::array<ControlId, 2> checkIds{ControlId::MaskSerials, ControlId::ProblemsOnly};
        for (std::size_t i = 0; i < checks.size(); ++i) {
            checks[i] = ::CreateWindowExW(0, L"BUTTON", nullptr, childVisible | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0,
                                          0, 0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(checkIds[i])),
                                          instance, this);
        }
        for (const HWND child : {list, status, card, hint}) {
            if (child == nullptr) {
                logWin32("ui.report.create", "CreateWindowExW(child)", ::GetLastError());
                return false;
            }
        }
        for (const HWND button : buttons) {
            if (button == nullptr) {
                logWin32("ui.report.create", "CreateWindowExW(button)", ::GetLastError());
                return false;
            }
        }
        for (const HWND check : checks) {
            if (check == nullptr) {
                logWin32("ui.report.create", "CreateWindowExW(check)", ::GetLastError());
                return false;
            }
        }
        // Подкласс нужен списку и кнопкам: они едят клавиши, и без подкласса
        // Ctrl+E и Enter до модели не дошли бы (§5 «Клавиатурная навигация,
        // фокус»). Строка состояния и карточка клавиши не едят.
        createChild(list);
        for (const HWND button : buttons) createChild(button);
        for (const HWND check : checks) createChild(check);
        controlsReady = true;
        return true;
    }

    // --- Подкласс и отложенная перерисовка -------------------------------------

    void createChild(HWND child) {
        if (child == nullptr || window == nullptr) return;
        const WNDPROC prev = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&childProc)));
        if (prev == nullptr) {
            logWin32("ui.report.subclass", "SetWindowLongPtrW(GWLP_WNDPROC)", ::GetLastError());
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
    // списка, то есть список перестраивается, не выйдя из собственного обработчика
    // сообщения; вставка и удаление строк внутри LVN — изменение списка из его же
    // обработчика, на которое comctl32 не рассчитан. Поэтому визуальное обновление
    // всегда на следующем витке очереди, а модель меняется сразу: она и есть
    // источник истины.
    void requestSync() {
        if (window != nullptr) ::PostMessageW(window, kMsgSyncModel, 0, 0);
    }

    void startTimer() {
        if (timerRunning || window == nullptr) return;
        ::SetTimer(window, kTimerJournal, kJournalIntervalMs, nullptr);
        timerRunning = true;
    }

    void stopTimer() {
        if (!timerRunning || window == nullptr) return;
        ::KillTimer(window, kTimerJournal);
        timerRunning = false;
    }

    // --- Действия --------------------------------------------------------------

    void doExport(ExportFormat format) {
        if (!model.canExport()) {
            logEvent(core::LogLevel::Warn, "ui.report.export", "no report");
            return;
        }
        const std::int64_t at = nowUnix();
        const std::string text = model.exportText(format);
        if (text.empty()) {
            // Пустой текст означает «отчёт нечего писать», а не «запись удалась»:
            // файл на двести байт с одним заголовком в каталоге отчётов — это
            // мусор, который человек потом отправляет в баг-репорт.
            lastProblem = tr(StringId::kCommonError) + ": " + word(Word::NoReport);
            logEvent(core::LogLevel::Warn, "ui.report.export", "empty report text");
            refreshAll();
            return;
        }
        const std::string name = model.suggestedName(format, at);
        if (callbacks.onExport) {
            callbacks.onExport(format, name, text);
            return;
        }
        const SaveOutcome outcome =
            saveReport(text, format, model.report() != nullptr ? model.report()->kind : core::ReportKind::Cleanup, at);
        if (!outcome.ok) {
            lastProblem = outcome.errorText;
            logEvent(core::LogLevel::Warn, "ui.report.export", "save failed");
            refreshAll();
            return;
        }
        lastProblem.clear();
        model.noteSave(outcome.pathUtf8, outcome.kept, outcome.pruned);
        refreshAll();
    }

    void doOpenFolder() {
        if (!callbacks.onOpenFolder || reportsDirectory.empty()) return;
        callbacks.onOpenFolder(reportsDirectory);
    }

    void doRefresh() {
        model.refreshJournal();
        if (callbacks.onRefresh) callbacks.onRefresh();
    }

    void command(ControlId id) {
        switch (id) {
        case ControlId::ExportHtml: doExport(ExportFormat::Html); return;
        case ControlId::ExportJson: doExport(ExportFormat::Json); return;
        case ControlId::ExportText: doExport(ExportFormat::Text); return;
        case ControlId::RefreshJournal: doRefresh(); return;
        case ControlId::OpenFolder: doOpenFolder(); return;
        case ControlId::MaskSerials:
            model.setMaskSerials(!model.maskSerials());
            syncButtons();
            return;
        case ControlId::ProblemsOnly:
            model.toggleProblemsOnly();
            requestSync();
            return;
        }
        logEvent(core::LogLevel::Debug, "ui.report.command", "unknown command");
    }

    [[nodiscard]] SortKey sortKeyOfColumn(int column) const noexcept {
        switch (column) {
        case kColumnTime: return SortKey::Time;
        case kColumnKind: return SortKey::Kind;
        case kColumnSize: return SortKey::Size;
        default: return SortKey::Time;
        }
    }

    [[nodiscard]] const JournalRow* rowAt(int index) const {
        if (index < 0) return nullptr;
        return model.rowAt(static_cast<std::size_t>(index));
    }
};

// Карточка: заголовок плюс строки «подпись: значение». Когда строка не выбрана,
// показывается сводка отчёта — карточка не должна пустовать на пустом экране.
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

    const int pad = std::max(2, scale.dip(10.0));
    const int titleHeight = std::max(10, scale.dip(20.0));
    const int lineHeight = std::max(10, scale.dip(17.0));
    RECT inner{box.left + pad, box.top + pad / 2, box.right - pad, box.bottom - pad / 2};
    if (inner.bottom <= inner.top) return TRUE;

    const bool hasSelection = state.model.selectedRow() != nullptr;
    const std::wstring title =
        toWide(hasSelection ? word(Word::DetailTitle) : state.model.titleText());
    if (state.fonts[2] != nullptr) ::SelectObject(dc, state.fonts[2]);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
    RECT titleRect{inner.left, inner.top, inner.right, std::min(inner.bottom, inner.top + titleHeight)};
    constexpr UINT single = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER;
    ::DrawTextW(dc, title.c_str(), -1, &titleRect, DT_LEFT | single);

    const std::vector<std::string> lines = hasSelection ? state.model.detailLines() : state.model.summaryLines();
    if (state.fonts[1] != nullptr) ::SelectObject(dc, state.fonts[1]);
    const int top = titleRect.bottom;
    const int available = inner.bottom - top;
    const std::size_t capacity = available > 0 ? static_cast<std::size_t>(available / std::max(1, lineHeight)) : 0U;
    if (capacity == 0) return TRUE;

    const std::size_t shown = std::min(capacity, lines.size());
    for (std::size_t i = 0; i < shown; ++i) {
        std::string text = lines[i];
        if (text.empty()) continue;
        // Обрезанный список — это молчание о недостающих свойствах, поэтому
        // последняя видимая строка честно помечается многоточием.
        if (i + 1 == shown && shown < lines.size()) text += "…";
        RECT line{inner.left, top + static_cast<int>(i) * lineHeight, inner.right,
                  top + static_cast<int>(i + 1) * lineHeight};
        if (line.bottom > inner.bottom) break;
        const std::size_t separator = text.find(": ");
        if (separator == std::string::npos) {
            ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
            const std::wstring wide = toWide(text);
            ::DrawTextW(dc, wide.c_str(), -1, &line, DT_LEFT | single);
            continue;
        }
        const std::wstring label = toWide(text.substr(0, separator + 1));
        SIZE measured{};
        ::GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &measured);
        const int labelWidth = std::min(measured.cx + scale.dip(8.0), (line.right - line.left) / 2);
        RECT labelRect{line.left, line.top, line.left + labelWidth, line.bottom};
        RECT valueRect{line.left + labelWidth, line.top, line.right, line.bottom};
        ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
        ::DrawTextW(dc, label.c_str(), -1, &labelRect, DT_LEFT | single);
        ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
        const std::wstring value = toWide(text.substr(separator + 2));
        ::DrawTextW(dc, value.c_str(), -1, &valueRect, DT_LEFT | single);
    }
    return TRUE;
}

// Пояснение вместо пустого списка: рамка, заголовок, причина и действие.
//
// Отдельная функция, а не переиспользование drawCard: у карточки деталей есть
// заголовок и строки «поле: значение», а здесь нужен один крупный заголовок и
// две строки прозы с переносом. Смешивать их — значит получить текст, который
// в одном случае обрезается, в другом выглядит как отчёт об ошибке.
LRESULT drawEmptyState(ViewState& state, const DRAWITEMSTRUCT& draw) {
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

    const int pad = std::max(2, scale.dip(12.0));
    const int titleHeight = std::max(10, scale.dip(22.0));
    RECT inner{box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
    if (inner.bottom <= inner.top) return TRUE;

    ::SetBkMode(dc, TRANSPARENT);
    if (state.fonts[2] != nullptr) ::SelectObject(dc, state.fonts[2]);
    ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
    RECT title{inner.left, inner.top, inner.right, std::min(inner.bottom, inner.top + titleHeight)};
    const std::wstring headline = toWide(word(Word::JournalEmpty));
    ::DrawTextW(dc, headline.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    if (state.fonts[1] != nullptr) ::SelectObject(dc, state.fonts[1]);
    ::SetTextColor(dc, theme::colorRef(palette.textSecondary));
    constexpr DWORD wrap = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;
    int y = title.bottom + scale.dip(4.0);
    for (const Word which : {Word::EmptyWhy, Word::EmptyWhat}) {
        const std::wstring text = toWide(word(which));
        RECT line{inner.left, y, inner.right, inner.bottom};
        (void)::DrawTextW(dc, text.c_str(), -1, &line, wrap | DT_CALCRECT);
        if (line.bottom <= y) break;
        (void)::DrawTextW(dc, text.c_str(), -1, &line, wrap);
        y = line.bottom + scale.dip(4.0);
        if (y >= inner.bottom) break;
    }
    return TRUE;
}

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
                // Минимум подобран так, чтобы влезали шесть колонок журнала и
                // ряд кнопок: на более узком окне список бесполезен.
                info->ptMinTrackSize.x = scale.dip(640.0);
                info->ptMinTrackSize.y = scale.dip(360.0);
            }
            return 0;
        }
        case WM_SHOWWINDOW:
            // Таймер журнала живёт только пока окно видимо: невидимый экран,
            // читающий кольцо лога дважды в секунду, греет процесс впустую (§5
            // «idle по CPU ≤ 2 %»).
            if (wParam != FALSE) {
                state->startTimer();
            } else {
                state->stopTimer();
            }
            return 0;
        case WM_TIMER:
            if (wParam == kTimerJournal && ::IsWindowVisible(window) == TRUE) {
                const std::uint64_t before = state->model.journalSequence();
                state->model.refreshJournal();
                if (state->model.journalSequence() != before) state->refreshAll();
            }
            return 0;
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
            // должен быть на журнале, а не на пустом окне.
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
            case 'H':
                if (control) {
                    state->doExport(ExportFormat::Html);
                    return 1;
                }
                break;
            case 'J':
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
            case VK_ESCAPE:
                if (state->model.selectedRow() != nullptr) {
                    state->model.clearSelection();
                    state->requestSync();
                    return 1;
                }
                break;
            default: break;
            }
            break;
        }
        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) break;
            const WORD id = LOWORD(wParam);
            if (!isReportControl(id)) break;
            state->command(static_cast<ControlId>(id));
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
                if (key.empty() || key == state->model.keyAt(state->model.selectedIndex())) break;
                state->model.setSelectedKey(key);
                state->refreshSelection();
                break;
            }
            case LVN_COLUMNCLICK: {
                const auto* headerClick = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if (headerClick == nullptr) break;
                state->model.toggleSort(state->sortKeyOfColumn(headerClick->iSubItem));
                state->requestSync();
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
                    const JournalRow* row = state->rowAt(static_cast<int>(draw->nmcd.dwItemSpec));
                    const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
                    const bool zebra = (static_cast<int>(draw->nmcd.dwItemSpec) % 2) == 1;
                    // Цвет подписи и фона — из темы, а не системный: на тёмной
                    // палитре системный чёрный текст нечитаем, а «зелёная полоса
                    // выделения» посреди тёмной темы выглядит дырой. Строка об
                    // ошибке и предупреждении подкрашивается сама — иначе
                    // «журнал ошибок» пришлось бы вычитывать глазами построчно.
                    theme::Color background = zebra ? palette.surfaceAlt : palette.surface;
                    theme::Color foreground = palette.textPrimary;
                    if (!selected && row != nullptr) {
                        if (row->level >= core::LogLevel::Error) {
                            foreground = palette.danger;
                        } else if (row->level == core::LogLevel::Warn) {
                            foreground = palette.warning;
                        }
                    }
                    HDC dc = draw->nmcd.hdc;
                    ::SetBkMode(dc, TRANSPARENT);
                    ::SetBkColor(dc, theme::colorRef(selected ? palette.surfaceSelected : background));
                    ::SetTextColor(dc, theme::colorRef(selected ? palette.textOnAccent : foreground));
                    return CDRF_NOTIFYSUBITEMDRAW;
                }
                case kSubItemPrePaint: {
                    // Колонку объёма рисуем сами: так она выравнивается вправо и
                    // читается как число, а не как текст, и мы не зависим от
                    // системного шрифта списка. Прямоугольник подпункта на этой
                    // стадии лежит в nmcd.rc.
                    if (draw->iSubItem != kColumnSize) return CDRF_DODEFAULT;
                    const JournalRow* row = state->rowAt(static_cast<int>(draw->nmcd.dwItemSpec));
                    if (row == nullptr) return CDRF_DODEFAULT;
                    RECT box = draw->nmcd.rc;
                    HDC dc = draw->nmcd.hdc;
                    if (state->fonts[0] != nullptr) ::SelectObject(dc, state->fonts[0]);
                    ::SetBkMode(dc, TRANSPARENT);
                    const std::wstring wide = toWide(row->bytesText);
                    ::DrawTextW(dc, wide.c_str(), -1, &box,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    return CDRF_SKIPDEFAULT;
                }
                case CDDS_ITEMPOSTPAINT:
                    return CDRF_DODEFAULT;
                default: break;
                }
                // Неизвестная стадия: сообщаем один раз. Если подстадии подпункта
                // придут с другими значениями, это будет видно в журнале, а не
                // молчаливым отсутствием чисел в списке.
                if (!state->drawStageLogged) {
                    state->drawStageLogged = true;
                    logEvent(core::LogLevel::Warn, "ui.report.list.customdraw", "unknown draw stage");
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
            if (draw->CtlType == ODT_STATIC && draw->CtlID == static_cast<UINT>(kChildHint) &&
                draw->hwndItem == state->hint) {
                return drawEmptyState(*state, *draw);
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
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const theme::Palette& palette = state->theme.palette();
            ::SetTextColor(dc, theme::colorRef(palette.textPrimary));
            ::SetBkColor(dc, theme::colorRef(palette.surface));
            return static_cast<LRESULT>(static_cast<LONG_PTR>(COLOR_BTNFACE) + 1);
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
        case WM_ERASEBKGND:
            // Дети перекрывают окно целиком; стирать собственную поверхность
            // незачем.
            return 1;
        case WM_DESTROY:
            state->stopTimer();
            state->controlsReady = false;
            return 0;
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default: break;
        }
    } catch (const std::exception& error) {
        logEvent(core::LogLevel::Error, "ui.report.exception", error.what());
        return message == WM_CREATE ? -1 : 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

// Подкласс контрола. Нужен для одного: клавиши уходят родителю, где их
// разбирает модель (§5 «Клавиатурная навигация»). SysListView32 и BUTTON свои
// клавиши едят, и Ctrl+H на кнопке молча пропадал бы.
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
// ReportScreen
// ---------------------------------------------------------------------------

struct ReportScreen::Impl : detail::ViewState {
    Impl() { dpi = static_cast<int>(theme.metrics().dpi); }
};

ReportScreen::ReportScreen(Callbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->callbacks = std::move(callbacks);
}

ReportScreen::~ReportScreen() { destroy(); }

HWND ReportScreen::create(HWND parent, int dpi) {
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
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    if (::InitCommonControlsEx(&controls) == FALSE) {
        logWin32("ui.report.create", "InitCommonControlsEx", ::GetLastError());
    }

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &detail::viewProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = detail::kReportViewClass;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logWin32("ui.report.create", "RegisterClassExW(view)", ::GetLastError());
        return nullptr;
    }

    // Каталог отчётов нужен кнопке «Открыть папку» и случаю «отчёт без
    // обработчика экспорта»; он не меняется за сеанс.
    state.reportsDirectory = reportsDirectoryUtf8();

    state.window = ::CreateWindowExW(0, detail::kReportViewClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                                     nullptr, instance, &state);
    if (state.window == nullptr) {
        logWin32("ui.report.create", "CreateWindowExW(view)", ::GetLastError());
        return nullptr;
    }
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
    return state.window;
}

HWND ReportScreen::window() const noexcept { return impl_->window; }

void ReportScreen::destroy() noexcept {
    auto& state = *impl_;
    if (state.window == nullptr) return;
    HWND window = state.window;
    state.window = nullptr;
    state.controlsReady = false;
    state.stopTimer();
    // Детей снимает DestroyWindow: каждое пришлёт WM_NCDESTROY и вернёт свою
    // исходную процедуру. Список children здесь не чистим — иначе подкласс
    // остался бы на уничтоженном контроле.
    ::DestroyWindow(window);
    state.children.clear();
    state.columnsReady = false;
    state.list = nullptr;
    state.status = nullptr;
    state.card = nullptr;
    state.hint = nullptr;
    state.buttons.fill(nullptr);
    state.checks.fill(nullptr);
}

void ReportScreen::setDpi(int dpi) {
    auto& state = *impl_;
    if (dpi > 0) state.dpi = dpi;
    state.theme.setDpi(static_cast<unsigned>(state.dpi));
    if (state.window == nullptr) return;
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
}

ReportViewModel& ReportScreen::model() noexcept { return impl_->model; }

const ReportViewModel& ReportScreen::model() const noexcept { return impl_->model; }

void ReportScreen::refresh() { impl_->refreshAll(); }

void ReportScreen::reloadTheme() {
    auto& state = *impl_;
    state.theme.reload();
    state.applyPalette();
    state.applyFonts();
    state.refreshAll();
}

void ReportScreen::publishReport(core::Report report) {
    impl_->model.publishReport(std::move(report));
    impl_->refreshAll();
}

void ReportScreen::publishReport(std::shared_ptr<const core::Report> report) {
    impl_->model.publishReport(std::move(report));
    impl_->refreshAll();
}

void ReportScreen::publishJournal(std::vector<core::LogRecord> records) {
    impl_->model.publishJournal(std::move(records));
    impl_->refreshAll();
}

void ReportScreen::refreshJournal() {
    impl_->model.refreshJournal();
    impl_->refreshAll();
}

}  // namespace mrproper::ui::report
