#include "report_html.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "units.hpp"

// HTML-отчёт: только текст, без Windows API и без ввода-вывода (SPEC §6.1).
// Числа и размеры форматирует core::units — тот же код, что в UI и в CLI:
// «12,3 ГБ» в отчёте и «12,3 ГБ» в приложении обязаны совпадать, иначе отчёт
// врёт. Язык (ru/en) переключает подписи, а не разделитель разрядов.

namespace mrproper::core {
namespace {

// «—» (em dash) в UTF-8: чем заменяем неизвестное значение.
const char* const kUnknown = "\xE2\x80\x94";

// Пределы длинных списков. Всё, что длиннее, обрезается с явной пометкой:
// молчаливый обрез выглядел бы как «мы всё показали» (FR-8 — отчёт должен
// быть источником правды, в том числе о том, чего в нём нет).
constexpr std::size_t kMaxIssues = 200;
constexpr std::size_t kMaxOperations = 1000;
constexpr std::size_t kMaxExternalReports = 10;

// ---------------------------------------------------------------------------
// Подписи
// ---------------------------------------------------------------------------

// Словарь отчёта. Русский — по умолчанию (§1.1: утилита «для себя» на русском
// пользователе), английский — чтобы отчёт можно было приложить к баг-репорту,
// который читают за пределами этого чата.
struct Labels {
    const char* documentTitle;
    const char* summarySection;
    const char* environmentSection;
    const char* disksSection;
    const char* candidatesSection;
    const char* operationsSection;
    const char* issuesSection;
    const char* notesSection;

    const char* labelVersion;
    const char* labelBuild;
    const char* labelConfig;
    const char* labelOs;
    const char* labelHost;
    const char* labelUser;
    const char* labelLocale;
    const char* labelTransaction;
    const char* labelGenerated;
    const char* labelStarted;
    const char* labelFinished;
    const char* labelDuration;

    const char* tilePlanned;
    const char* tileFreed;
    const char* tileDryRun;
    const char* tileOperations;
    const char* tileIssues;
    const char* tileCandidates;
    const char* tileDisks;

    const char* colDisk;
    const char* colSerial;
    const char* colFirmware;
    const char* colBus;
    const char* colDiskSize;
    const char* colIndex;
    const char* colKind;
    const char* colPartSize;
    const char* colOffset;
    const char* colFileSystem;
    const char* colLabel;
    const char* colMountPoints;
    const char* colFree;
    const char* colShare;
    const char* colGptName;
    const char* colFlags;

    const char* colCategory;
    const char* colCandidate;
    const char* colAllocated;
    const char* colFiles;
    const char* colSafety;
    const char* colConfidence;
    const char* colAge;
    const char* colWhy;
    const char* colLockers;

    const char* colAction;
    const char* colFreed;
    const char* colStatus;
    const char* colTime;
    const char* colError;
    const char* colRisk;  // уровень риска операции (заполняет вызывающая сторона)

    const char* colStage;
    const char* colSubject;
    const char* colCode;
    const char* colMessage;

    const char* statusOk;
    const char* statusFailed;
    const char* statusSkipped;
    const char* safetySafe;
    const char* safetyReview;
    const char* safetyRisky;

    const char* flagRemovable;
    const char* flagReadOnly;
    const char* flagTrim;
    const char* flagSmart;
    const char* flagEncrypted;
    const char* flagDirty;
    const char* flagNoVolume;

    const char* emptyDisks;
    const char* emptyCandidates;
    const char* emptyOperations;
    const char* emptyIssues;

    const char* legendMap;
    const char* truncatedCandidates;
    const char* truncatedOperations;
    const char* truncatedIssues;
    const char* footerMasked;
    const char* footerPrivacy;
    const char* noteAllocated;
};

const Labels kRu = {
    "Отчёт MrProper",
    "Сводка",
    "Среда",
    "Диски и разделы",
    "Кандидаты очистки",
    "Операции",
    "Ошибки",
    "Примечания",

    "Версия приложения",
    "Сборка",
    "Конфигурация",
    "ОС",
    "Узел",
    "Пользователь",
    "Локаль",
    "Транзакция корзины",
    "Сформирован",
    "Начало",
    "Окончание",
    "Длительность",

    "Планировалось освободить",
    "Освобождено",
    "Dry-run: ничего не удалено",
    "Операций",
    "Ошибок",
    "Кандидатов",
    "Дисков",

    "Диск",
    "Серийник",
    "Прошивка",
    "Шина",
    "Объём",
    "№",
    "Тип",
    "Размер",
    "Смещение",
    "ФС",
    "Метка",
    "Точки монтирования",
    "Свободно",
    "Доля",
    "Имя GPT",
    "Признаки",

    "Категория",
    "Объект",
    "По аллоцированному",
    "Файлов",
    "Безопасность",
    "Уверенность",
    "Возраст",
    "Почему это мусор",
    "Кем занято",

    "Действие",
    "Освобождено",
    "Итог",
    "Время",
    "Ошибка",
    "Уровень риска",

    "Этап",
    "Объект",
    "Код",
    "Сообщение",

    "готово",
    "ошибка",
    "пропущено",
    "безопасно",
    "проверить",
    "риск",

    "съёмный",
    "только чтение",
    "TRIM",
    "SMART",
    "шифрование",
    "не конгруэнтен",
    "без тома",

    "Диски не найдены.",
    "Кандидатов нет.",
    "Операций не было.",
    "Ошибок нет.",

    "Полоска — разделы диска пропорционально размеру.",
    "Показаны первые %zu из %zu кандидатов — полный список в отчёте JSON.",
    "Показаны первые %zu из %zu операций — полный список в журнале.",
    "Показаны первые %zu из %zu ошибок — полный список в журнале.",
    "Серийники дисков замаскированы: отчёт предназначен для отправки.",
    "Отчёт формируется локально. MrProper не отправляет данные в сеть.",
    "объёмы — по аллоцированному размеру: это то, что реально освободится",
};

const Labels kEn = {
    "MrProper report",
    "Summary",
    "Environment",
    "Disks and partitions",
    "Cleanup candidates",
    "Operations",
    "Errors",
    "Notes",

    "Application version",
    "Build",
    "Configuration",
    "OS",
    "Host",
    "User",
    "Locale",
    "Trash transaction",
    "Generated",
    "Started",
    "Finished",
    "Duration",

    "Planned to free",
    "Freed",
    "Dry run: nothing was deleted",
    "Operations",
    "Errors",
    "Candidates",
    "Disks",

    "Disk",
    "Serial",
    "Firmware",
    "Bus",
    "Size",
    "#",
    "Kind",
    "Size",
    "Offset",
    "File system",
    "Label",
    "Mount points",
    "Free",
    "Share",
    "GPT name",
    "Flags",

    "Category",
    "Item",
    "Allocated",
    "Files",
    "Safety",
    "Confidence",
    "Age",
    "Why it is junk",
    "Held by",

    "Action",
    "Freed",
    "Status",
    "Time",
    "Error",
    "Risk level",

    "Stage",
    "Subject",
    "Code",
    "Message",

    "done",
    "failed",
    "skipped",
    "safe",
    "review",
    "risky",

    "removable",
    "read-only",
    "TRIM",
    "SMART",
    "encrypted",
    "not clean",
    "no volume",

    "No disks found.",
    "No candidates.",
    "No operations.",
    "No errors.",

    "The bar shows disk partitions proportionally to their size.",
    "Showing the first %zu of %zu candidates — the full list is in the JSON report.",
    "Showing the first %zu of %zu operations — the full list is in the log.",
    "Showing the first %zu of %zu errors — the full list is in the log.",
    "Disk serials are masked: this report is safe to send.",
    "Generated locally. MrProper never sends data over the network.",
    "sizes are allocated sizes: what is actually reclaimed",
};

const Labels& labelsFor(HtmlReportLanguage language) {
    return language == HtmlReportLanguage::English ? kEn : kRu;
}

// Подписи перечислений модели. Свои функции, а не toString() из model.hpp:
// те отдают машинные токены для JSON, а отчёт читает человек (FR-4 —
// «объяснение обязательно у каждого кандидата»).
const char* safetyLabel(SafetyLevel level, HtmlReportLanguage language) {
    const Labels& l = labelsFor(language);
    switch (level) {
        case SafetyLevel::Safe: return l.safetySafe;
        case SafetyLevel::Review: return l.safetyReview;
        case SafetyLevel::Risky: return l.safetyRisky;
    }
    return l.safetyReview;
}

// Класс фишки уровня риска: тот же, что у раздела кандидатов, чтобы «безопасно»
// в обеих таблицах выглядело одинаково.
const char* safetyClass(SafetyLevel level) {
    switch (level) {
        case SafetyLevel::Safe: return "ok";
        case SafetyLevel::Review: return "warn";
        case SafetyLevel::Risky: return "err";
    }
    return "warn";
}

const char* actionLabel(PlanAction action, HtmlReportLanguage language) {
    if (language == HtmlReportLanguage::English) {
        switch (action) {
            case PlanAction::Delete: return "Delete";
            case PlanAction::Trash: return "Trash";
            case PlanAction::Keep: return "Keep";
            case PlanAction::SkipLocked: return "Skip (locked)";
        }
        return "Keep";
    }
    switch (action) {
        case PlanAction::Delete: return "Удалить";
        case PlanAction::Trash: return "В корзину";
        case PlanAction::Keep: return "Оставить";
        case PlanAction::SkipLocked: return "Пропустить (занято)";
    }
    return "Оставить";
}

const char* kindLabel(PartitionKind kind, HtmlReportLanguage language) {
    if (language == HtmlReportLanguage::English) {
        switch (kind) {
            case PartitionKind::BasicData: return "Data";
            case PartitionKind::System: return "System";
            case PartitionKind::Msr: return "MSR";
            case PartitionKind::Recovery: return "Recovery";
            case PartitionKind::Oem: return "OEM";
            case PartitionKind::Efi: return "EFI";
            case PartitionKind::Reserved: return "Reserved";
            case PartitionKind::Unallocated: return "Unallocated";
            case PartitionKind::Unknown: break;
        }
        return "Unknown";
    }
    switch (kind) {
        case PartitionKind::BasicData: return "Данные";
        case PartitionKind::System: return "Система";
        case PartitionKind::Msr: return "MSR";
        case PartitionKind::Recovery: return "Восстановление";
        case PartitionKind::Oem: return "OEM";
        case PartitionKind::Efi: return "EFI";
        case PartitionKind::Reserved: return "Резерв";
        case PartitionKind::Unallocated: return "Не размечено";
        case PartitionKind::Unknown: break;
    }
    return "Неизвестно";
}

// Цвет полоски раздела. Палитра фиксирована и от темы не зависит: тёмная тема
// браузера меняет цвета страницы через CSS-переменные, а эти блоки остаются
// различимыми на обоих фонах.
const char* kindColor(PartitionKind kind) {
    switch (kind) {
        case PartitionKind::BasicData: return "#4c8bf5";
        case PartitionKind::System: return "#d9534f";
        case PartitionKind::Msr: return "#8a8a8a";
        case PartitionKind::Recovery: return "#f0ad4e";
        case PartitionKind::Oem: return "#9b59b6";
        case PartitionKind::Efi: return "#2c3e50";
        case PartitionKind::Reserved: return "#95a5a6";
        case PartitionKind::Unallocated: return "#dfe4ea";
        case PartitionKind::Unknown: break;
    }
    return "#c9ccd1";
}

const char* busLabel(BusType bus) {
    switch (bus) {
        case BusType::Sata: return "SATA";
        case BusType::Nvme: return "NVMe";
        case BusType::Usb: return "USB";
        case BusType::Scsi: return "SCSI";
        case BusType::Sd: return "SD";
        case BusType::Sas: return "SAS";
        case BusType::Virtual: return "Virtual";
        case BusType::Raid: return "RAID";
        case BusType::Unknown: break;
    }
    return kUnknown;
}

// ---------------------------------------------------------------------------
// Время и длительность: только арифметика, без часов системы
// ---------------------------------------------------------------------------

constexpr std::int64_t kSecondsPerDay = 86400;
constexpr std::int64_t kSecondsPerHour = 3600;
constexpr std::int64_t kSecondsPerMinute = 60;

// Год/месяц/день из числа дней с 1970-01-01 (алгоритм civil_from_days
// Howard Hinnant). gmtime завязан на часовой пояс машины и на локаль, а метка
// отчёта должна совпадать на любом хосте и в тестах.
void civilFromDays(std::int64_t daysSinceEpoch, std::int64_t& year, std::int64_t& month, std::int64_t& day) {
    const std::int64_t z = daysSinceEpoch + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;                                      // [0, 146096]
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
    const std::int64_t mp = (5 * doy + 2) / 153;                                     // [0, 11]
    day = doy - (153 * mp + 2) / 5 + 1;                                              // [1, 31]
    month = mp < 10 ? mp + 3 : mp - 9;                                               // [1, 12]
    year = y + (month <= 2 ? 1 : 0);
}

// Деление с округлением вниз и неотрицательным остатком: метка должна быть
// верной и для отметок до 1970 года.
void floorDivMod(std::int64_t value, std::int64_t divisor, std::int64_t& quotient, std::int64_t& rest) {
    quotient = value / divisor;
    rest = value % divisor;
    if (rest < 0) {
        rest += divisor;
        --quotient;
    }
}

// Число с ведущими нулями, с запасом на год длиннее четырёх цифр.
std::string padNumber(std::int64_t value, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*lld", width, static_cast<long long>(value));
    return std::string(buf);
}

// Русская десятичная запятая: snprintf печатает точку, а в русском отчёте дробная
// часть отделяется запятой — так же, как в core::units. Иначе «1.2 с» рядом с
// «1,2 ГБ» в одной таблице выглядит как две разные правды.
std::string withRussianComma(std::string text) {
    for (char& c : text) {
        if (c == '.') c = ',';
    }
    return text;
}

// ---------------------------------------------------------------------------
// Общие куски разметки
// ---------------------------------------------------------------------------

// Инвариант модуля: всё, что попадает в разметку из данных, прошло через
// escapeHtml. Каркас отчёта написан здесь, текст приходит уже экранированным.
void appendRow(std::string& out, const std::initializer_list<std::string>& cells) {
    out += "<tr>";
    for (const std::string& cell : cells) out += cell;
    out += "</tr>";
}

void appendHeaderCell(std::string& out, const char* label, bool numeric) {
    out += "<th scope=\"col\"";
    if (numeric) out += " class=\"num\"";
    out += ">";
    out += escapeHtml(label);
    out += "</th>";
}

void closeTable(std::string& out) { out += "</tbody></table>"; }

// Текст ячейки: пустая строка печатается прочерком, чтобы таблица читалась как
// «неизвестно», а не как «забыли».
std::string cellText(const std::string& value) {
    if (value.empty()) return escapeHtml(kUnknown);
    return escapeHtml(value);
}

std::string cellBytes(std::uint64_t bytes, bool binaryUnits) {
    return escapeHtml(formatBytes(bytes, 1, binaryUnits));
}

std::string cellTime(std::int64_t unixSeconds, HtmlReportLanguage language) {
    if (unixSeconds <= 0) return escapeHtml(kUnknown);
    return escapeHtml(formatUnixUtc(unixSeconds, language));
}

std::string cellDuration(std::int64_t milliseconds, HtmlReportLanguage language) {
    if (milliseconds <= 0) return escapeHtml(kUnknown);
    return escapeHtml(formatDurationMs(milliseconds, language));
}

std::string chip(const std::string& text, const char* cssClass) {
    std::string out = "<span class=\"chip";
    if (cssClass != nullptr) {
        out += " ";
        out += cssClass;
    }
    out += "\">";
    out += escapeHtml(text);
    out += "</span>";
    return out;
}

// Набор признаков (съёмный / TRIM / SMART / …). Пустой набор не печатается вовсе,
// чтобы в таблице не было столбца из пустых ячеек.
std::string chipRow(const std::vector<std::string>& items, const char* cssClass) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += " ";
        out += chip(items[i], cssClass);
    }
    return out;
}

// Список «первые N по аллоцированному размеру»: индексы кандидатов в порядке
// показа. При равном размере порядок исходный — иначе golden-тест «поплывёт».
std::vector<std::size_t> candidateOrder(const std::vector<CleanupCandidate>& candidates, std::size_t limit) {
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (limit == 0 || order.size() <= limit) return order;
    std::stable_sort(order.begin(), order.end(), [&candidates](std::size_t a, std::size_t b) {
        return candidates[a].allocatedBytes > candidates[b].allocatedBytes;
    });
    order.resize(limit);
    return order;
}

// Подстановка двух чисел в текст про обрезку. Собственная замена, а не snprintf с
// variadic: шаблон из словаря, а локаль — из std::string, типы известны.
std::string formatCountNote(const char* pattern, std::size_t shown, std::size_t total) {
    std::string out(pattern != nullptr ? pattern : "");
    const std::string first = std::to_string(shown);
    const std::string second = std::to_string(total);
    std::size_t pos = out.find("%zu");
    if (pos != std::string::npos) out.replace(pos, 3, first);
    pos = out.find("%zu");
    if (pos != std::string::npos) out.replace(pos, 3, second);
    return escapeHtml(out);
}

std::size_t countSerials(const std::vector<PhysicalDisk>& disks) {
    std::size_t total = 0;
    for (const PhysicalDisk& disk : disks) {
        if (!disk.serial.empty()) ++total;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Разделы отчёта
// ---------------------------------------------------------------------------

// Стили. Встроены намеренно: отчёт открывают двойным щелчком без сети, и ссылка
// на CDN-шрифт сделала бы его битым. Ни одного url(), ни одного внешнего
// шрифта, ни одного скрипта — иначе isStandaloneHtml перестанет быть правдой.
const char* const kHtmlStyle =
    ":root{color-scheme:light dark;--bg:#f5f6f8;--fg:#1b1f24;--muted:#5b6472;--card:#fff;--line:#dcdfe4;"
    "--ok:#1f7a45;--warn:#8a6100;--err:#b3261e;--chip:#eceff3}\n"
    "@media (prefers-color-scheme:dark){:root{--bg:#14171a;--fg:#e6e9ee;--muted:#98a2b0;--card:#1c2024;"
    "--line:#2b3138;--ok:#6fce97;--warn:#e2b341;--err:#ff8a80;--chip:#252b31}}\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;padding:24px;background:var(--bg);color:var(--fg);"
    "font:14px/1.5 system-ui,\"Segoe UI\",Roboto,Arial,sans-serif}\n"
    "main{max-width:1200px;margin:0 auto}\n"
    "h1{font-size:22px;margin:0 0 4px}\n"
    "h2{font-size:17px;margin:30px 0 10px;padding-bottom:6px;border-bottom:1px solid var(--line)}\n"
    "h3{font-size:15px;margin:18px 0 6px}\n"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}\n"
    ".sub,.map-legend{color:var(--muted);font-size:12px;margin:4px 0 8px}\n"
    ".tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}\n"
    ".tile{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 12px}\n"
    ".tile .k{color:var(--muted);font-size:12px}\n"
    ".tile .v{font-size:20px;font-weight:600;word-break:break-word}\n"
    "table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);"
    "border-radius:10px;margin:8px 0 4px}\n"
    "caption{text-align:left;padding:6px 10px;color:var(--muted);font-size:12px}\n"
    "th,td{padding:6px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top;"
    "word-break:break-word}\n"
    "th{font-size:12px;color:var(--muted);font-weight:600}\n"
    "th.key{width:220px}\n"
    "tr:last-child td{border-bottom:none}\n"
    "td.num,th.num{text-align:right;white-space:nowrap}\n"
    ".path{font-family:ui-monospace,Consolas,monospace;font-size:12px;color:var(--muted)}\n"
    ".chip{display:inline-block;padding:1px 8px;border-radius:999px;background:var(--chip);font-size:12px;"
    "white-space:nowrap}\n"
    ".ok{color:var(--ok)}.warn{color:var(--warn)}.err{color:var(--err)}\n"
    ".map{display:flex;height:22px;border:1px solid var(--line);border-radius:6px;overflow:hidden;"
    "background:var(--card)}\n"
    ".map span{display:block;min-width:3px}\n"
    ".notes{margin:6px 0 0;padding-left:20px}\n"
    ".notes li{margin:2px 0}\n"
    ".empty{color:var(--muted);font-style:italic}\n"
    "footer{color:var(--muted);font-size:12px;margin-top:30px;border-top:1px solid var(--line);padding-top:10px}\n"
    "footer p{margin:4px 0}\n"
    "@media print{body{padding:0;background:#fff}.card,.tile,table{border-color:#c8ccd2}}\n";

void renderHead(std::string& out, const HtmlReportInput& in, const HtmlReportOptions& opt, const Labels& labels) {
    out += "<!DOCTYPE html>\n<html lang=\"";
    out += (opt.language == HtmlReportLanguage::English ? "en" : "ru");
    out += "\">\n<head>\n<meta charset=\"utf-8\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<meta name=\"generator\" content=\"";
    out += kHtmlReportGenerator;
    if (!in.meta.appVersion.empty()) {
        out += " ";
        out += escapeHtml(in.meta.appVersion);
    }
    out += "\">\n<title>";
    out += escapeHtml(in.title.empty() ? std::string(kHtmlReportGenerator) + " — " + labels.documentTitle : in.title);
    out += "</title>\n<style>\n";
    out += kHtmlStyle;
    out += "</style>\n</head>\n<body>\n<main>\n";
}

void renderHeader(std::string& out, const HtmlReportInput& in, const Labels& labels) {
    const std::string title =
        in.title.empty() ? std::string(kHtmlReportGenerator) + " — " + labels.documentTitle : in.title;

    out += "<header class=\"card\">\n<h1>";
    out += escapeHtml(title);
    out += "</h1>\n<p class=\"sub\">";
    if (in.generatedAtUnix > 0) {
        out += escapeHtml(formatUnixUtc(in.generatedAtUnix));
    } else {
        out += escapeHtml(kUnknown);
    }
    if (!in.meta.osName.empty()) {
        out += " · ";
        out += escapeHtml(in.meta.osName);
    }
    if (!in.meta.appVersion.empty()) {
        out += " · ";
        out += escapeHtml(in.meta.appVersion);
    }
    out += "</p>\n";
    if (in.dryRun) {
        out += "<p><span class=\"chip warn\">";
        out += escapeHtml(labels.tileDryRun);
        out += "</span></p>\n";
    }
    out += "</header>\n";
}

void renderTile(std::string& out, const char* key, const std::string& value) {
    out += "<div class=\"tile\"><div class=\"k\">";
    out += escapeHtml(key);
    out += "</div><div class=\"v\">";
    out += value;
    out += "</div></div>\n";
}

void renderSummary(std::string& out, const HtmlReportInput& in, const HtmlReportOptions& opt, const Labels& labels) {
    out += "<section aria-labelledby=\"summary\"><h2 id=\"summary\">";
    out += escapeHtml(labels.summarySection);
    out += "</h2>\n<div class=\"tiles\">\n";

    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const HtmlReportOperation& op : in.operations) {
        if (op.skipped) ++skipped;
        if (!op.success && !op.skipped) ++failed;
    }

    renderTile(out, labels.tilePlanned, cellBytes(in.plannedBytes, opt.binaryUnits));
    // При dry-run ничего не освобождалось: «—» честнее, чем ноль, который
    // читается как «освободил, но не смог посчитать».
    renderTile(out, labels.tileFreed, in.dryRun ? escapeHtml(kUnknown) : cellBytes(in.freedBytes, opt.binaryUnits));

    std::string operations = escapeHtml(std::to_string(in.operations.size()));
    if (failed != 0) {
        operations += " <span class=\"err\">(";
        operations += escapeHtml(std::to_string(failed));
        operations += " " + escapeHtml(labels.statusFailed) + ")</span>";
    }
    if (skipped != 0) {
        operations += " <span class=\"warn\">(";
        operations += escapeHtml(std::to_string(skipped));
        operations += " " + escapeHtml(labels.statusSkipped) + ")</span>";
    }
    renderTile(out, labels.tileOperations, operations);

    renderTile(out, labels.tileIssues, escapeHtml(std::to_string(in.issues.size())));
    renderTile(out, labels.tileCandidates, escapeHtml(std::to_string(in.candidates.size())));
    renderTile(out, labels.tileDisks, escapeHtml(std::to_string(in.disks.size())));

    out += "</div>\n<p class=\"sub\">";
    out += escapeHtml(labels.noteAllocated);
    out += "</p>\n</section>\n";
}

void appendMetaRow(std::string& out, const char* key, const std::string& value) {
    appendRow(out, {"<th scope=\"row\" class=\"key\">" + escapeHtml(key) + "</th>", "<td>" + value + "</td>"});
}

void renderEnvironment(std::string& out, const HtmlReportInput& in, const HtmlReportOptions& opt,
                       const Labels& labels) {
    out += "<section aria-labelledby=\"env\"><h2 id=\"env\">";
    out += escapeHtml(labels.environmentSection);
    out += "</h2>\n<table><tbody>\n";

    std::string os = in.meta.osName;
    if (!in.meta.osVersion.empty()) {
        if (!os.empty()) os += " ";
        os += in.meta.osVersion;
    }

    appendMetaRow(out, labels.labelVersion, cellText(in.meta.appVersion));
    appendMetaRow(out, labels.labelBuild, cellText(in.meta.appBuild));
    if (!in.meta.buildConfig.empty() && in.meta.buildConfig != in.meta.appBuild) {
        appendMetaRow(out, labels.labelConfig, cellText(in.meta.buildConfig));
    }
    appendMetaRow(out, labels.labelOs, cellText(os));
    appendMetaRow(out, labels.labelHost, cellText(in.meta.hostName));
    appendMetaRow(out, labels.labelUser, cellText(in.meta.userName));
    appendMetaRow(out, labels.labelLocale, cellText(in.meta.localeTag));
    appendMetaRow(out, labels.labelTransaction, cellText(in.transactionId));
    appendMetaRow(out, labels.labelGenerated, cellTime(in.generatedAtUnix, opt.language));
    if (in.startedAtUnix > 0) appendMetaRow(out, labels.labelStarted, cellTime(in.startedAtUnix, opt.language));
    if (in.finishedAtUnix > 0) {
        appendMetaRow(out, labels.labelFinished, cellTime(in.finishedAtUnix, opt.language));
        if (in.startedAtUnix > 0 && in.finishedAtUnix > in.startedAtUnix) {
            const std::int64_t ms = (in.finishedAtUnix - in.startedAtUnix) * 1000;
            appendMetaRow(out, labels.labelDuration, cellDuration(ms, opt.language));
        }
    }

    out += "</tbody></table>\n</section>\n";
}

// Карта разделов: полоса из flex-блоков. Пропорции задаются flex-grow, а не
// процентными ширинами: сумма процентов после округления уплыла бы за 100 % и
// полоса перестала бы быть картой.
void renderPartitionMap(std::string& out, const PhysicalDisk& disk, HtmlReportLanguage language, const Labels& labels) {
    out += "<div class=\"map\" role=\"img\" aria-label=\"";
    out += escapeHtml(labels.disksSection);
    out += "\">";
    for (const Partition& part : disk.partitions) {
        out += "<span style=\"flex:";
        out += std::to_string(part.lengthBytes);
        out += " 1 0;background:";
        out += kindColor(part.kind);
        out += "\" title=\"";
        out += escapeHtml(kindLabel(part.kind, language));
        out += " · ";
        out += escapeHtml(formatBytes(part.lengthBytes, 1, false));
        out += "\"></span>";
    }
    out += "</div>\n<p class=\"map-legend\">";
    out += escapeHtml(labels.legendMap);
    out += "</p>\n";
}

void renderDiskFlags(std::string& out, const PhysicalDisk& disk, const Labels& labels) {
    std::vector<std::string> items;
    if (disk.removable) items.emplace_back(labels.flagRemovable);
    if (disk.readOnly) items.emplace_back(labels.flagReadOnly);
    if (disk.trimSupported) items.emplace_back(labels.flagTrim);
    if (disk.smartAvailable) items.emplace_back(labels.flagSmart);
    if (items.empty()) return;
    out += chipRow(items, "ok");
}

void renderVolumeFlags(std::string& out, const Partition& part, const Labels& labels) {
    if (!part.hasVolume) {
        out += chip(labels.flagNoVolume, "warn");
        return;
    }
    const Volume& volume = part.volume;
    std::vector<std::string> items;
    if (volume.encrypted) items.emplace_back(labels.flagEncrypted);
    if (volume.dirty) items.emplace_back(labels.flagDirty);
    if (volume.readOnly) items.emplace_back(labels.flagReadOnly);
    if (items.empty()) return;
    out += chipRow(items, "warn");
}

void renderDisks(std::string& out, const HtmlReportInput& in, const HtmlReportOptions& opt, const Labels& labels) {
    out += "<section aria-labelledby=\"disks\"><h2 id=\"disks\">";
    out += escapeHtml(labels.disksSection);
    out += " (";
    out += escapeHtml(std::to_string(in.disks.size()));
    out += ")</h2>\n";

    if (in.disks.empty()) {
        out += "<p class=\"empty\">";
        out += escapeHtml(labels.emptyDisks);
        out += "</p>\n</section>\n";
        return;
    }

    for (const PhysicalDisk& disk : in.disks) {
        // Приватность по умолчанию (SPEC §5): серийник в открытом виде попадает
        // в отчёт только если человек это явно попросил.
        const std::string serial = opt.maskSerials
                                       ? maskSerialNumber(disk.serial, opt.keepSerialEdges, opt.serialMaskChar)
                                       : disk.serial;

        out += "<h3>";
        out += escapeHtml(labels.colDisk);
        out += " ";
        out += escapeHtml(std::to_string(disk.number));
        if (!disk.model.empty()) {
            out += " — ";
            out += escapeHtml(disk.model);
        }
        out += "</h3>\n";

        if (opt.showPartitionMap) renderPartitionMap(out, disk, opt.language, labels);

        out += "<table>";
        if (!disk.devicePath.empty()) {
            out += "<caption>";
            out += escapeHtml(disk.devicePath);
            out += "</caption>";
        }
        out += "<thead><tr>";
        appendHeaderCell(out, labels.colSerial, false);
        appendHeaderCell(out, labels.colFirmware, false);
        appendHeaderCell(out, labels.colBus, false);
        appendHeaderCell(out, labels.colDiskSize, true);
        appendHeaderCell(out, labels.colFlags, false);
        out += "</tr></thead><tbody>\n";

        std::string flags;
        renderDiskFlags(flags, disk, labels);

        appendRow(out,
                  {"<td>" + cellText(serial) + "</td>",
                   "<td>" + cellText(disk.firmware) + "</td>",
                   "<td>" + escapeHtml(busLabel(disk.bus)) + "</td>",
                   "<td class=\"num\">" + cellBytes(disk.sizeBytes, opt.binaryUnits) + "</td>",
                   "<td>" + (flags.empty() ? cellText(std::string()) : flags) + "</td>"});
        closeTable(out);

        // Разделы диска: без таблицы полоса выше остаётся украшением, а FR-8
        // требует именно «карту разделов».
        out += "<table><thead><tr>";
        appendHeaderCell(out, labels.colIndex, true);
        appendHeaderCell(out, labels.colKind, false);
        appendHeaderCell(out, labels.colPartSize, true);
        appendHeaderCell(out, labels.colShare, true);
        appendHeaderCell(out, labels.colOffset, true);
        appendHeaderCell(out, labels.colFileSystem, false);
        appendHeaderCell(out, labels.colLabel, false);
        appendHeaderCell(out, labels.colMountPoints, false);
        appendHeaderCell(out, labels.colFree, true);
        appendHeaderCell(out, labels.colGptName, false);
        appendHeaderCell(out, labels.colFlags, false);
        out += "</tr></thead><tbody>\n";

        for (const Partition& part : disk.partitions) {
            std::string mounts;
            if (part.hasVolume) {
                for (std::size_t i = 0; i < part.volume.mountPoints.size(); ++i) {
                    if (i != 0) mounts += "<br>";
                    mounts += escapeHtml(part.volume.mountPoints[i]);
                }
            }
            if (mounts.empty()) mounts = escapeHtml(kUnknown);

            std::string freeCell;
            if (part.hasVolume && part.volume.totalBytes > 0) {
                const double fraction =
                    static_cast<double>(part.volume.freeBytes) / static_cast<double>(part.volume.totalBytes);
                freeCell = escapeHtml(formatBytes(part.volume.freeBytes, 1, opt.binaryUnits)) + "<br>" +
                           escapeHtml(formatPercent(fraction, 1));
            } else {
                freeCell = escapeHtml(kUnknown);
            }

            std::string share = escapeHtml(kUnknown);
            if (disk.sizeBytes > 0) {
                const double fraction =
                    static_cast<double>(part.lengthBytes) / static_cast<double>(disk.sizeBytes);
                share = escapeHtml(formatPercent(fraction, 1));
            }

            std::string fileSystem = escapeHtml(kUnknown);
            std::string label = escapeHtml(kUnknown);
            if (part.hasVolume) {
                fileSystem = cellText(part.volume.fileSystem);
                label = cellText(part.volume.label);
            }

            std::string volumeFlags;
            renderVolumeFlags(volumeFlags, part, labels);

            appendRow(out,
                      {"<td class=\"num\">" + escapeHtml(std::to_string(part.index)) + "</td>",
                       "<td>" + escapeHtml(kindLabel(part.kind, opt.language)) + "</td>",
                       "<td class=\"num\">" + cellBytes(part.lengthBytes, opt.binaryUnits) + "</td>",
                       "<td class=\"num\">" + share + "</td>",
                       "<td class=\"num\">" + cellBytes(part.offsetBytes, opt.binaryUnits) + "</td>",
                       "<td>" + fileSystem + "</td>",
                       "<td>" + label + "</td>",
                       "<td>" + mounts + "</td>",
                       "<td class=\"num\">" + freeCell + "</td>",
                       "<td>" + cellText(part.gptName) + "</td>",
                       "<td>" + volumeFlags + "</td>"});
        }

        closeTable(out);
    }

    out += "</section>\n";
}

void renderCandidates(std::string& out, const HtmlReportInput& in, const HtmlReportOptions& opt, const Labels& labels) {
    out += "<section aria-labelledby=\"candidates\"><h2 id=\"candidates\">";
    out += escapeHtml(labels.candidatesSection);
    out += " (";
    out += escapeHtml(std::to_string(in.candidates.size()));
    out += ")</h2>\n";

    if (in.candidates.empty()) {
        out += "<p class=\"empty\">";
        out += escapeHtml(labels.emptyCandidates);
        out += "</p>\n</section>\n";
        return;
    }

    const std::vector<std::size_t> order = candidateOrder(in.candidates, opt.maxCandidates);

    out += "<table><thead><tr>";
    appendHeaderCell(out, labels.colCategory, false);
    appendHeaderCell(out, labels.colCandidate, false);
    appendHeaderCell(out, labels.colAllocated, true);
    appendHeaderCell(out, labels.colFiles, true);
    appendHeaderCell(out, labels.colSafety, false);
    appendHeaderCell(out, labels.colConfidence, true);
    appendHeaderCell(out, labels.colAge, true);
    if (opt.showReasons) appendHeaderCell(out, labels.colWhy, false);
    appendHeaderCell(out, labels.colLockers, false);
    out += "</tr></thead><tbody>\n";

    for (std::size_t index : order) {
        const CleanupCandidate& candidate = in.candidates[index];

        std::string ageCell;
        if (in.generatedAtUnix > 0 && candidate.newestWrite > 0) {
            ageCell = escapeHtml(formatAge(in.generatedAtUnix - candidate.newestWrite));
        } else {
            ageCell = escapeHtml(kUnknown);
        }

        std::string reasons;
        if (opt.showReasons) {
            for (std::size_t i = 0; i < candidate.reasons.size(); ++i) {
                if (i != 0) reasons += "<br>";
                reasons += escapeHtml(candidate.reasons[i]);
            }
            if (reasons.empty()) reasons = escapeHtml(kUnknown);
        }

        std::string lockers;
        for (const ProcessRef& process : candidate.lockedBy) {
            if (!lockers.empty()) lockers += ", ";
            lockers += escapeHtml(process.name.empty() ? std::to_string(process.pid) : process.name);
        }
        if (lockers.empty()) lockers = escapeHtml(kUnknown);

        std::vector<std::string> cells;
        cells.push_back("<td>" + cellText(candidate.category) + "</td>");
        cells.push_back("<td>" + cellText(candidate.displayName) + "<div class=\"path\">" +
                        (candidate.path.empty() ? std::string(escapeHtml(kUnknown)) : escapeHtml(candidate.path)) +
                        "</div></td>");
        cells.push_back("<td class=\"num\">" + cellBytes(candidate.allocatedBytes, opt.binaryUnits) + "</td>");
        cells.push_back("<td class=\"num\">" + escapeHtml(std::to_string(candidate.fileCount)) + "</td>");
        cells.push_back("<td>" + chip(safetyLabel(candidate.safety, opt.language), safetyClass(candidate.safety)) +
                        "</td>");
        cells.push_back("<td class=\"num\">" + escapeHtml(formatPercent(candidate.confidence / 100.0, 0)) + "</td>");
        cells.push_back("<td class=\"num\">" + ageCell + "</td>");
        if (opt.showReasons) cells.push_back("<td>" + reasons + "</td>");
        cells.push_back("<td>" + lockers + "</td>");

        out += "<tr>";
        for (const std::string& cell : cells) out += cell;
        out += "</tr>";
    }

    closeTable(out);

    if (order.size() < in.candidates.size()) {
        out += "<p class=\"sub\">";
        out += formatCountNote(labels.truncatedCandidates, order.size(), in.candidates.size());
        out += "</p>\n";
    }
    out += "</section>\n";
}

void renderOperations(std::string& out, const HtmlReportInput& in, const HtmlReportOptions& opt, const Labels& labels) {
    out += "<section aria-labelledby=\"operations\"><h2 id=\"operations\">";
    out += escapeHtml(labels.operationsSection);
    out += " (";
    out += escapeHtml(std::to_string(in.operations.size()));
    out += ")</h2>\n";

    if (in.operations.empty()) {
        out += "<p class=\"empty\">";
        out += escapeHtml(labels.emptyOperations);
        out += "</p>\n</section>\n";
        return;
    }

    out += "<table><thead><tr>";
    appendHeaderCell(out, labels.colAction, false);
    appendHeaderCell(out, labels.colCandidate, false);
    appendHeaderCell(out, labels.colFreed, true);
    // FR-4/§12: уровень риска операции — не украшение, а часть ответа на вопрос
    // «что именно было удалено». Поле заполняет вызывающая сторона (cmd_report,
    // view_report), а раньше оно молча терялось: колонки не было вовсе.
    appendHeaderCell(out, labels.colRisk, false);
    appendHeaderCell(out, labels.colStatus, false);
    appendHeaderCell(out, labels.colTime, true);
    appendHeaderCell(out, labels.colError, false);
    out += "</tr></thead><tbody>\n";

    const std::size_t shown = std::min(in.operations.size(), kMaxOperations);
    for (std::size_t i = 0; i < shown; ++i) {
        const HtmlReportOperation& op = in.operations[i];

        const char* statusClass = "ok";
        const char* statusText = labels.statusOk;
        if (op.skipped) {
            statusClass = "warn";
            statusText = labels.statusSkipped;
        } else if (!op.success) {
            statusClass = "err";
            statusText = labels.statusFailed;
        }

        appendRow(out,
                  {"<td>" + escapeHtml(actionLabel(op.action, opt.language)) + "</td>",
                   "<td>" + cellText(op.name) + "<div class=\"path\">" +
                       (op.path.empty() ? std::string(escapeHtml(kUnknown)) : escapeHtml(op.path)) + "</div></td>",
                   "<td class=\"num\">" + cellBytes(op.bytes, opt.binaryUnits) + "</td>",
                   "<td>" + chip(safetyLabel(op.safety, opt.language), safetyClass(op.safety)) + "</td>",
                   "<td>" + chip(statusText, statusClass) + "</td>",
                   "<td class=\"num\">" + cellDuration(op.durationMs, opt.language) + "</td>",
                   "<td>" + (op.error.empty() ? std::string(escapeHtml(kUnknown)) : escapeHtml(op.error)) + "</td>"});
    }

    closeTable(out);

    if (shown < in.operations.size()) {
        out += "<p class=\"sub\">";
        out += formatCountNote(labels.truncatedOperations, shown, in.operations.size());
        out += "</p>\n";
    }
    out += "</section>\n";
}

void renderIssues(std::string& out, const HtmlReportInput& in, const Labels& labels) {
    out += "<section aria-labelledby=\"issues\"><h2 id=\"issues\">";
    out += escapeHtml(labels.issuesSection);
    out += " (";
    out += escapeHtml(std::to_string(in.issues.size()));
    out += ")</h2>\n";

    if (in.issues.empty()) {
        out += "<p class=\"empty\">";
        out += escapeHtml(labels.emptyIssues);
        out += "</p>\n</section>\n";
        return;
    }

    out += "<table><thead><tr>";
    appendHeaderCell(out, labels.colStage, false);
    appendHeaderCell(out, labels.colSubject, false);
    appendHeaderCell(out, labels.colCode, true);
    appendHeaderCell(out, labels.colMessage, false);
    out += "</tr></thead><tbody>\n";

    const std::size_t shown = std::min(in.issues.size(), kMaxIssues);
    for (std::size_t i = 0; i < shown; ++i) {
        const HtmlReportIssue& issue = in.issues[i];
        std::string code = escapeHtml(kUnknown);
        if (issue.code != 0) {
            // HRESULT печатаем и шестнадцатеричным, и десятичным: в журнале оно
            // числом, в разговоре о баге — «0x80070005».
            char buf[48];
            std::snprintf(buf, sizeof(buf), "0x%08lX (%llu)", static_cast<unsigned long>(issue.code),
                          static_cast<unsigned long long>(issue.code));
            code = escapeHtml(buf);
        }
        appendRow(out,
                  {"<td>" + cellText(issue.stage) + "</td>", "<td>" + cellText(issue.subject) + "</td>",
                   "<td class=\"num\">" + code + "</td>", "<td>" + cellText(issue.message) + "</td>"});
    }

    closeTable(out);

    if (shown < in.issues.size()) {
        out += "<p class=\"sub\">";
        out += formatCountNote(labels.truncatedIssues, shown, in.issues.size());
        out += "</p>\n";
    }
    out += "</section>\n";
}

void renderNotes(std::string& out, const HtmlReportInput& in, const Labels& labels) {
    if (in.notes.empty()) return;
    out += "<section aria-labelledby=\"notes\"><h2 id=\"notes\">";
    out += escapeHtml(labels.notesSection);
    out += "</h2>\n<ul class=\"notes\">\n";
    for (const std::string& note : in.notes) {
        out += "<li>";
        out += escapeHtml(note);
        out += "</li>\n";
    }
    out += "</ul>\n</section>\n";
}

void renderFooter(std::string& out, const HtmlReportInput& in, const HtmlReportOptions& opt, const Labels& labels) {
    out += "<footer>\n<p>";
    out += escapeHtml(labels.footerPrivacy);
    out += "</p>\n";
    if (opt.maskSerials && countSerials(in.disks) > 0) {
        out += "<p>";
        out += escapeHtml(labels.footerMasked);
        out += "</p>\n";
    }
    out += "<p>";
    out += escapeHtml(kHtmlReportGenerator);
    if (!in.meta.appVersion.empty()) {
        out += " ";
        out += escapeHtml(in.meta.appVersion);
    }
    out += " · ";
    out += escapeHtml(labels.documentTitle);
    out += "</p>\n</footer>\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Публичный интерфейс
// ---------------------------------------------------------------------------

HtmlReportLanguage htmlLanguageFromString(std::string_view languageTag) {
    // «en», «en-US», «en_GB» → English; ru и всё остальное — русский: лучше
    // русская подпись, чем пустой отчёт.
    if (languageTag.size() >= 2) {
        const bool latin = (languageTag[0] == 'e' || languageTag[0] == 'E') &&
                           (languageTag[1] == 'n' || languageTag[1] == 'N');
        if (latin) return HtmlReportLanguage::English;
    }
    return HtmlReportLanguage::Russian;
}

std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (raw) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            case '\t':
            case '\n':
            case '\r': out += raw; break;
            default:
                // Управляющие символы (включая \0) в HTML не имеют смысла и
                // ломают разметку, попав в атрибут: заменяем пробелом.
                if (c < 0x20) {
                    out += ' ';
                } else {
                    out += raw;
                }
                break;
        }
    }
    return out;
}

std::string maskSerialNumber(std::string_view serial, std::size_t keepEdges, std::string_view maskChar) {
    // Пробелы по краям у серийников бывают и ничего не значат: без их учёта
    // « S3Z1  » маскировался бы по краям пустоты.
    while (!serial.empty() && (serial.front() == ' ' || serial.front() == '\t')) serial.remove_prefix(1);
    while (!serial.empty() && (serial.back() == ' ' || serial.back() == '\t')) serial.remove_suffix(1);
    if (serial.empty()) return std::string();
    if (maskChar.empty()) maskChar = kHtmlSerialMaskChar;

    const std::size_t length = serial.size();
    if (length <= keepEdges * 2 + 1) {
        std::string out;
        out.reserve(length * maskChar.size());
        for (std::size_t i = 0; i < length; ++i) out += maskChar;
        return out;
    }

    std::string out;
    out.reserve(length);
    out += serial.substr(0, keepEdges);
    for (std::size_t i = keepEdges * 2; i < length; ++i) out += maskChar;
    out += serial.substr(length - keepEdges, keepEdges);
    return out;
}

std::string formatUnixUtc(std::int64_t unixSeconds, HtmlReportLanguage language) {
    std::int64_t days = 0;
    std::int64_t rest = 0;
    floorDivMod(unixSeconds, kSecondsPerDay, days, rest);

    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;
    civilFromDays(days, year, month, day);

    const std::int64_t hour = rest / kSecondsPerHour;
    const std::int64_t minute = (rest % kSecondsPerHour) / kSecondsPerMinute;
    const std::int64_t second = rest % kSecondsPerMinute;

    const std::string date = language == HtmlReportLanguage::English
                                 ? padNumber(year, 4) + "-" + padNumber(month, 2) + "-" + padNumber(day, 2)
                                 : padNumber(day, 2) + "." + padNumber(month, 2) + "." + padNumber(year, 4);
    return date + " " + padNumber(hour, 2) + ":" + padNumber(minute, 2) + ":" + padNumber(second, 2) + " UTC";
}

std::string formatDurationMs(std::int64_t milliseconds, HtmlReportLanguage language) {
    if (milliseconds < 0) return kUnknown;
    const bool english = language == HtmlReportLanguage::English;
    if (milliseconds < 1000) return std::to_string(milliseconds) + (english ? " ms" : " мс");

    // Разделитель дробной части — как в core::units: в русском отчёте «1,2 с».
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(milliseconds) / 1000.0);
    const std::string value = english ? std::string(buf) : withRussianComma(buf);
    if (milliseconds < 60000) return value + (english ? " s" : " с");

    const std::int64_t totalSeconds = milliseconds / 1000;
    const std::int64_t totalMinutes = totalSeconds / kSecondsPerMinute;
    const std::int64_t hours = totalMinutes / 60;
    const std::int64_t minutes = totalMinutes % 60;
    const std::int64_t seconds = totalSeconds % kSecondsPerMinute;

    if (hours == 0) {
        return std::to_string(minutes) + (english ? " min " : " мин ") + std::to_string(seconds) +
               (english ? " s" : " с");
    }
    return std::to_string(hours) + (english ? " h " : " ч ") + std::to_string(minutes) + (english ? " min" : " мин");
}

// ---------------------------------------------------------------------------
// Проверка самодостаточности
// ---------------------------------------------------------------------------

namespace {

char asciiLower(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

std::size_t indexOfFold(std::string_view hay, std::string_view needle, std::size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string_view::npos;
    const std::size_t limit = hay.size() - needle.size();
    for (std::size_t i = from; i <= limit; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (asciiLower(hay[i + j]) != asciiLower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::string_view::npos;
}

bool equalsFoldPrefix(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLower(value[i]) != asciiLower(prefix[i])) return false;
    }
    return true;
}

// Значение атрибута начиная с позиции после «name=»: в кавычках или голое слово.
std::string attributeValueAt(std::string_view html, std::size_t pos) {
    const auto skipSpace = [&html](std::size_t at) {
        while (at < html.size() && (html[at] == ' ' || html[at] == '\t' || html[at] == '\n' || html[at] == '\r')) ++at;
        return at;
    };
    pos = skipSpace(pos);
    if (pos >= html.size() || html[pos] != '=') return std::string();
    pos = skipSpace(pos + 1);
    if (pos >= html.size()) return std::string();

    const char quote = html[pos];
    if (quote == '"' || quote == '\'') {
        ++pos;
        const std::size_t end = html.find(quote, pos);
        if (end == std::string_view::npos) return std::string();
        return std::string(html.substr(pos, end - pos));
    }

    const std::size_t begin = pos;
    while (pos < html.size() && !isNameChar(html[pos]) && html[pos] != '/' && html[pos] != '>') ++pos;
    return std::string(html.substr(begin, pos - begin));
}

// Внутренние адреса: якорь документа и data:/about:/javascript:/mailto: —
// ничего из этого не тянет из сети.
bool isInternalTarget(std::string_view value) {
    if (value.empty()) return true;
    if (value.front() == '#') return true;
    return equalsFoldPrefix(value, "data:") || equalsFoldPrefix(value, "about:") || equalsFoldPrefix(value, "cid:") ||
           equalsFoldPrefix(value, "blob:") || equalsFoldPrefix(value, "javascript:") ||
           equalsFoldPrefix(value, "mailto:");
}

// Пространства имён XML объявляют адрес, но ничего по нему не грузят:
// w3.org в xmlns= не делает отчёт зависимым от сети.
bool insideXmlns(std::string_view html, std::size_t pos) {
    const std::size_t windowStart = pos > 48 ? pos - 48 : 0;
    return indexOfFold(html.substr(windowStart, pos - windowStart), "xmlns", 0) != std::string_view::npos;
}

void addUnique(std::vector<std::string>& found, std::string text) {
    if (text.empty()) return;
    for (const std::string& item : found) {
        if (item == text) return;
    }
    found.push_back(std::move(text));
}

// Атрибуты, которые что-то тянут: картинки, таблицы стилей, скрипты, видео.
const std::array<const char*, 5> kResourceAttributes = {"src", "href", "data", "poster", "srcset"};

}  // namespace

std::string findExternalReferences(std::string_view html) {
    std::vector<std::string> found;

    // 1) Ресурсные атрибуты — только внутри тегов: имя атрибута в тексте («data=»
    //    в сообщении об ошибке) зависимости не создаёт.
    std::size_t pos = 0;
    while (pos < html.size()) {
        const std::size_t tagStart = html.find('<', pos);
        if (tagStart == std::string_view::npos) break;

        std::size_t tagEnd = tagStart + 1;
        char quote = '\0';
        while (tagEnd < html.size()) {
            const char c = html[tagEnd];
            if (quote != '\0') {
                if (c == quote) quote = '\0';
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
            ++tagEnd;
        }
        if (tagEnd >= html.size()) break;  // незакрытый тег — разбирать нечего

        const std::string_view tag = html.substr(tagStart + 1, tagEnd - tagStart - 1);
        for (const char* attribute : kResourceAttributes) {
            const std::size_t attrLen = std::char_traits<char>::length(attribute);
            std::size_t at = indexOfFold(tag, attribute, 0);
            while (at != std::string_view::npos) {
                const bool boundaryOk = at == 0 || !isNameChar(tag[at - 1]);
                if (boundaryOk) {
                    const std::string value = attributeValueAt(tag, at + attrLen);
                    if (!isInternalTarget(value)) addUnique(found, std::string(attribute) + "=\"" + value + "\"");
                }
                at = indexOfFold(tag, attribute, at + attrLen);
            }
        }

        pos = tagEnd + 1;
    }

    // 2) Голые внешние URL. Проверка намеренно строгая и по всему документу, а не
    //    только по тегам: отчёт идёт в баг-репорт, и ссылка в тексте ошибки
    //    должна быть видна тому, кто его читает. Пространства имён вычеркнуты.
    pos = 0;
    while (pos < html.size()) {
        const std::size_t scheme = html.find("://", pos);
        if (scheme == std::string_view::npos) break;
        if (!insideXmlns(html, scheme)) {
            const std::size_t start = html.rfind(' ', scheme);
            const std::size_t begin = (start == std::string_view::npos) ? 0 : start + 1;
            const std::size_t end = (scheme > 40 && begin < scheme - 40) ? scheme - 40 : begin;
            addUnique(found, std::string(html.substr(end, scheme - end)) + "://…");
        }
        pos = scheme + 3;
    }

    // 3) Протокол-относительные адреса «//host/…» — сеть без схемы.
    pos = 0;
    while (pos < html.size()) {
        const std::size_t found2 = html.find("//", pos);
        if (found2 == std::string_view::npos) break;
        const char previous = found2 > 0 ? html[found2 - 1] : ' ';
        const char next = found2 + 2 < html.size() ? html[found2 + 2] : ' ';
        const bool isSchemeTail = previous == ':';
        const bool looksLikeHost = next != '/' && next != ' ' && next != '\t' && next != '\n' && next != '\r' &&
                                   next != '>' && next != '"' && next != '\'';
        if (!isSchemeTail && looksLikeHost && !insideXmlns(html, found2)) {
            const std::size_t tail = std::min<std::size_t>(24, html.size() - found2 - 2);
            addUnique(found, std::string("//") + std::string(html.substr(found2 + 2, tail)) + "…");
        }
        pos = found2 + 2;
    }

    std::string out;
    const std::size_t shown = std::min(found.size(), kMaxExternalReports);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) out += "; ";
        out += found[i];
    }
    if (found.size() > shown) {
        out += "; … ещё ";
        out += std::to_string(found.size() - shown);
    }
    return out;
}

bool isStandaloneHtml(std::string_view html) { return findExternalReferences(html).empty(); }

std::string renderHtmlReport(const HtmlReportInput& in, const HtmlReportOptions& opt) {
    const Labels& labels = labelsFor(opt.language);
    std::string out;
    out.reserve(32 * 1024);

    renderHead(out, in, opt, labels);
    renderHeader(out, in, labels);
    renderSummary(out, in, opt, labels);
    renderEnvironment(out, in, opt, labels);
    if (opt.showDisks) renderDisks(out, in, opt, labels);
    if (opt.showCandidates) renderCandidates(out, in, opt, labels);
    renderOperations(out, in, opt, labels);
    renderIssues(out, in, labels);
    renderNotes(out, in, labels);
    renderFooter(out, in, opt, labels);

    out += "</main>\n</body>\n</html>\n";
    return out;
}

}  // namespace mrproper::core
