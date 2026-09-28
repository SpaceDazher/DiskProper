// HTML-отчёт MrProper: самодостаточный документ без единой внешней ссылки
// (SPEC §4 FR-8 «Экспорт: HTML (человекочитаемый)», §1.1 «репозиторий публичный»,
// §5 «Приватность: отчёт содержит серийники — пользователь может исключить их
// перед отправкой»).
//
// Переносимый модуль: без Windows API, без ввода-вывода и без локали системы —
// ровно тот же класс, что core::json/units (SPEC §6.1). Модуль ничего не пишет
// на диск: запись файла в %LOCALAPPDATA%\MrProper\reports\ с ограничением
// последних 20 файлов делает платформа/движок, здесь только текст.
//
// Три требования FR-8 «человекочитаемый» и §1.1, ради которых модуль написан:
//   1. Самодостаточность. Один .html-файл, который открывается двойным щелчком
//      без интернета: CSS встроен, шрифты системные, скриптов нет, картинки
//      нарисованы CSS-блоками (карта разделов). Всё, что можно, проверяется
//      функцией isStandaloneHtml — она же используется в тестах.
//   2. Приватность по умолчанию. Серийники дисков маскируются всегда, пока
//      вызывающая сторона не попросит иначе (options.maskSerials = false —
//      осознанный выбор человека, а не дефолт).
//   3. Детерминизм. Вывод — чистая функция входа: ни часов системы, ни локали,
//      ни случайности. Всё время передаётся вызывающей стороной
//      (generatedAtUnix), поэтому golden-тест не «поплывёт» завтра.
//
// Содержимое повторяет FR-8: карта разделов, кандидаты с оценками, выполненные
// операции, ошибки, время, версии ОС и приложения.
//
// Имена модуля намеренно не пересекаются с соседним core::report_json: оба
// живут в пространстве имён mrproper::core и обязаны сосуществовать в одной
// сборке, поэтому здесь префикс Html, а там будет Json (одинаковое имя типа в
// двух заголовках — redefinition, одинаковая свободная функция — LNK2005).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"

namespace mrproper::core {

// Символ маскирования серийника: «•» (U+2022) в UTF-8. Задан байтами, а не
// \u2022, чтобы файл не зависел от кодировки этапа сборки (/utf-8 есть, но
// отчёт читают ещё и глазами через git).
inline constexpr char kHtmlSerialMaskChar[] = "\xE2\x80\xA2";

// Сколько знаков серийника остаётся видимыми с каждой стороны при маскировании.
inline constexpr std::size_t kHtmlSerialMaskKeepEdges = 2;

// Имя генератора в <meta name="generator"> и подвале отчёта.
inline constexpr const char* kHtmlReportGenerator = "MrProper";

// Язык подписей. Спека требует ru + en (SPEC §5 «Локализация»), строки
// приложения живут в ресурсах UI, а у переносимого отчёта own словарь —
// иначе модуль зависел бы от WinAPI.
enum class HtmlReportLanguage { Russian, English };

// «en», «en-US», «en_GB» → English; всё остальное (включая пустое) → Russian.
HtmlReportLanguage htmlLanguageFromString(std::string_view languageTag);

// ---------------------------------------------------------------------------
// Данные отчёта
// ---------------------------------------------------------------------------

// Реквизиты машины и сборки. Всё необязательное: пустая строка не печатается,
// отчёт остаётся читаемым (FR-8 требует версии, но не требует их любой ценой).
struct HtmlReportMeta {
    std::string appVersion;   // "1.0.0"
    std::string appBuild;     // "Debug" / "Release"
    std::string osName;       // "Windows 11 Pro"
    std::string osVersion;    // "10.0.22631"
    std::string hostName;     // имя узла
    std::string userName;     // учётная запись, под которой шла очистка
    std::string localeTag;    // "ru-RU"
    std::string buildConfig;  // профиль сборки, если отличается от appBuild
};

// Одна выполненная (или невыполненная) операция очистки — строка раздела
// «Операции». Поля повторяют сущности SPEC §6.3, чтобы вызывающая сторона
// (движок) не переписывала их в строки заранее.
struct HtmlReportOperation {
    PlanAction action{PlanAction::Keep};
    std::string name;    // «Кэш браузера» — человекочитаемое имя
    std::string path;    // UTF-8 путь, как в модели
    std::uint64_t bytes{};  // освобождено по факту (0 для Keep/SkipLocked)
    SafetyLevel safety{SafetyLevel::Review};
    int confidence{};
    bool success{true};  // false — операция провалилась, error обязателен
    bool skipped{};      // не трогали: SkipLocked, отмена, «нечего делать»
    std::string error;   // текст ошибки от платформы (HRESULT в issues)
    std::int64_t durationMs{};  // 0 — время не измерялось
};

// Ошибка или предупреждение. FR-8 требует раздел «ошибки», §4 FR-6 —
// «остальные операции продолжаются», поэтому ошибка не отменяет отчёт.
struct HtmlReportIssue {
    std::string stage;    // "scan" / "apply" / "trash" / произвольная метка этапа
    std::string subject;  // путь, имя объекта, номер операции
    std::string message;  // текст ошибки
    std::uint32_t code{}; // HRESULT или код возврата; 0 — кода нет
};

// Вход рендера. Заполняет вызывающая сторона; всё, кроме meta и списков,
// имеет осмысленные значения по умолчанию, поэтому минимальный отчёт —
// это один заполненный HtmlReportInput.
//
// Пример минимального вызова:
//     HtmlReportInput in;
//     in.meta.appVersion = "1.0.0";
//     in.meta.osName = "Windows 11 Pro";
//     in.generatedAtUnix = 1790000000;
//     in.disks = disks;          // std::vector<PhysicalDisk>
//     in.operations = operations;
//     const std::string html = renderHtmlReport(in);   // серийники замаскированы
struct HtmlReportInput {
    HtmlReportMeta meta;

    std::string title;               // пусто — «Отчёт MrProper»
    std::int64_t generatedAtUnix{};  // время формирования; 0 — «неизвестно»
    std::int64_t startedAtUnix{};    // начало операции; 0 — не измерялось
    std::int64_t finishedAtUnix{};   // конец операции; 0 — не завершена

    bool dryRun{true};               // FR-5: dry-run обязателен, по умолчанию включён
    std::uint64_t plannedBytes{};    // планировалось освободить
    std::uint64_t freedBytes{};      // освобождено по факту
    std::string transactionId;       // txId корзины (FR-7); пусто — без корзины

    std::vector<PhysicalDisk> disks;             // карта разделов
    std::vector<CleanupCandidate> candidates;   // кандидаты с оценками
    std::vector<HtmlReportOperation> operations; // выполненные операции
    std::vector<HtmlReportIssue> issues;         // ошибки
    std::vector<std::string> notes;              // пояснения, которых отчёт не знает сам
};

// Что и как печатать. Влияет только на вид, не на данные: отчёт с выключенным
// разделом не теряет ни одной строки, он её просто не показывает.
struct HtmlReportOptions {
    HtmlReportLanguage language{HtmlReportLanguage::Russian};

    // Приватность (SPEC §5): серийники по умолчанию не попадают в отчёт открытым
    // текстом. Тот, кто действительно шлёт отчёт владельцу железа, включает
    // исходные значения осознанно.
    bool maskSerials{true};
    std::size_t keepSerialEdges{kHtmlSerialMaskKeepEdges};
    std::string serialMaskChar{kHtmlSerialMaskChar};

    bool showDisks{true};          // карта разделов
    bool showPartitionMap{true};   // цветная полоса разделов над таблицей
    bool showCandidates{true};     // список кандидатов с оценками
    bool showReasons{true};        // колонка «почему это мусор» (FR-4: G4)

    // 0 — печатаем всех кандидатов. Иначе — первые N по аллоцированному размеру
    // (по убыванию), а про остальных пишем в подвале раздела: молча обрезать
    // список значит выглядеть полнее, чем он есть.
    std::size_t maxCandidates{0};

    bool binaryUnits{false};  // 1000 (по умолчанию, как в units) или 1024
};

// ---------------------------------------------------------------------------
// Рендер
// ---------------------------------------------------------------------------

// Самодостаточный HTML-документ целиком: <!DOCTYPE html>, встроенный CSS,
// ни одной внешней ссылки. Возвращает строку UTF-8; файл на диск пишет
// вызывающая сторона.
std::string renderHtmlReport(const HtmlReportInput& input, const HtmlReportOptions& options = HtmlReportOptions{});

// ---------------------------------------------------------------------------
// Кирпичики (вынесены, потому что ими пользуются и тесты, и соседние модули:
// JSON-отчёт тоже должен экранировать и маскировать так же)
// ---------------------------------------------------------------------------

// Экранирование текста для HTML: & < > " ' и управляющие символы (кроме
// пробела, табуляции и переводов строк). Работает и в атрибутах, поэтому
// отдельного escapeHtmlAttribute не существует. Не-ASCII (кириллица) не
// трогается: документ объявляет <meta charset="utf-8">.
std::string escapeHtml(std::string_view text);

// Маскирование серийника: по умолчанию видны первые и последние два знака,
// середина заменена на маску. Пустая строка даёт пустую строку (рисовать маску
// неизвестной длины незачем). Слишком короткий серийник (короче 2*keepEdges+1)
// маскируется целиком. Знаки пробела по краям не несут информации и у реальных
// дисков есть — они убираются до решения.
std::string maskSerialNumber(std::string_view serial, std::size_t keepEdges = kHtmlSerialMaskKeepEdges,
                             std::string_view maskChar = kHtmlSerialMaskChar);

// Проверка самодостаточности: есть ли в документе внешние ссылки. Ресурсные
// атрибуты (src, href, data, poster, srcset) ищутся только внутри тегов — имя
// атрибута в тексте («data=» в сообщении об ошибке) зависимости не создаёт.
// Голые «http(s)://» и «//host» ищутся по всему документу: отчёт идёт в
// баг-репорт, и ссылка в тексте ошибки должна быть видна тому, кто его читает.
// Пространства имён XML (xmlns=«http://www.w3.org/…») внешними не считаются —
// они ничего не грузят. Используется в тестах и в CI: регрессия «случайно
// подключили CDN-шрифт» должна ломать сборку.
bool isStandaloneHtml(std::string_view html);

// Человекочитаемый список внешних ссылок (пустая строка — документ чист).
// Нужен, чтобы падение isStandaloneHtml объясняло себя, а не молчало.
std::string findExternalReferences(std::string_view html);

// Время UTC для показа: "27.09.2026 23:10:00 UTC" в русском отчёте и
// "2026-09-27 23:10:00 UTC" в английском. Только арифметика, без gmtime: метка
// отчёта не должна зависеть от часового пояса машины, на которой он построен.
// Ноль — это 1970-01-01, а не «неизвестно»: неизвестность решает вызывающая
// сторона (см. HtmlReportInput::generatedAtUnix).
std::string formatUnixUtc(std::int64_t unixSeconds, HtmlReportLanguage language = HtmlReportLanguage::Russian);

// Длительность для показа: "840 мс", "1,2 с", "2 мин 5 с", "— " при неизвестной.
std::string formatDurationMs(std::int64_t milliseconds, HtmlReportLanguage language = HtmlReportLanguage::Russian);

}  // namespace mrproper::core
