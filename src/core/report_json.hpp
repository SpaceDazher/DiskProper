// Детерминированный JSON-отчёт MrProper: карта разделов, кандидаты с оценками,
// выполненные операции, ошибки, время и версии (SPEC §4 FR-8, §8 Этап 4,
// §11.4 golden-тесты, §5 «Приватность»).
//
// Единственное жёсткое свойство модуля: одинаковый вход даёт побайтово
// одинаковый выход. Отсюда три правила, которые нельзя нарушать «на потом»:
//   * ключи идут в фиксированном порядке; ни map/unordered_map, ни сортировки
//     «как получится» — порядок разделов, кандидатов и операций сохраняется
//     таким, каким пришёл от движка;
//   * в JSON попадают только целые числа и строки: ни formatBytes, ни локали, ни
//     часовых поясов, иначе golden-тест «тот же вход → тот же файл» начнёт мигать;
//   * модуль ничего не пишет на диск и ничего не печатает: возвращает строку, а
//     запись, ротацию (последние 20 файлов, FR-8) и HTML/текстовый отчёт делает
//     платформа.
//
// Переносимый модуль (SPEC §6.1, ADR-004): ни Windows API, ни ввода-вывода, ни
// зависимостей кроме core::model и core::json. Всё, что знает о машине (версия
// ОС, PID, серийники, пути), приходит от платформы заполненной структурой
// Report — ядро добывает это только в engine/platform.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"
#include "plan.hpp"

namespace mrproper::core {

// Версия схемы отчёта. Повышается только при несовместимой смене формата:
// отчёт читают люди, скрипты баг-репортов и будущие версии приложения.
inline constexpr int kReportJsonSchema = 1;

// Тип отчёта: «что нашли» (до очистки) и «что сделали» (после).
enum class ReportKind { Scan, Cleanup, DryRun };
const char* toString(ReportKind kind);

// Итог одной операции. Skilled — «файл держат приложения» (FR-5 Skip (locked)),
// Failed — операция не дала результата, Partial — часть элементов не удалена
// (ошибки при этом попадают в Report::errors, операция не фатальна — FR-6).
enum class ReportOperationStatus { Success, Failed, Skipped, Partial };
const char* toString(ReportOperationStatus status);

// Версии приложения и ОС (FR-8: «версии ОС/приложения»). Ядро их не знает —
// заполняет platform::sysinfo.
struct ReportEnvironment {
    std::string appVersion;
    std::uint32_t pid{};
    std::string rulesVersion;  // версия набора правил, по которому шёл скан
    std::string osCaption;     // «Microsoft Windows 11 Pro»
    std::string osVersion;     // «10.0.22631»
    std::uint32_t osBuild{};
    std::string architecture;  // «x64»
};

// Время операции (FR-8: «время»). Ноль означает «платформа не передала»:
// различать 1970 год и «неизвестно» в отчёте не нужно, а выдумывать время нельзя.
struct ReportTiming {
    std::int64_t startedAtUnix{};
    std::int64_t finishedAtUnix{};
    std::int64_t durationMs{};
};

// Одна выполненная (или пропущенная) операция. Повторяет PlanOperation из
// core::plan, но с результатом: что получилось, сколько попыток, в какую
// транзакцию корзины легло (FR-7 — по этому отчёту ищут, что отменять).
struct ReportOperation {
    std::size_t candidateIndex{};
    PlanAction action{PlanAction::Keep};
    ReportOperationStatus status{ReportOperationStatus::Success};
    std::string category;
    std::string displayName;
    std::string path;
    std::uint64_t bytes{};
    SafetyLevel safety{SafetyLevel::Review};
    int confidence{};
    std::uint32_t attempts{1};
    std::string transactionId;  // txId корзины для Trash; пусто для Delete
    std::int64_t startedAtUnix{};
    std::int64_t finishedAtUnix{};
    std::string detail;  // HRESULT/текст платформы, если есть
};

// Ошибка. Их не фатальны (FR-6): остальные операции продолжаются, а отчёт
// объясняет пользователю, почему освободилось меньше, чем показал план.
struct ReportError {
    std::string scope;      // «scan», «plan», «execute», «trash», «restore», «report»
    std::string code;       // HRESULT строкой («0x80070005») или код правила
    std::string message;    // человекочитаемый текст (ru) — пустым быть не может
    std::string path;       // путь, на котором упало; может быть пустым (не путь)
    std::string operation;  // имя операции/этапа, для группировки в UI
    std::int64_t atUnix{};
    std::uint32_t count{1};  // сколько раз повторилось (FR-6: retry с backoff)
};

// Что именно попадает в файл. Всё по умолчанию, и это осознанно: отчёт —
// главный артефакт для баг-репорта (ADR-007), молчаливый «урезанный» отчёт хуже
// полного. Маскирование серийников, наоборот, по умолчанию включено (SPEC §5:
// «отчёт содержит серийники — пользователь может исключить их перед отправкой»).
struct ReportOptions {
    bool includeDisks{true};
    bool includeCandidates{true};
    bool includeOperations{true};
    bool includeUntouched{true};
    bool includeErrors{true};
    // Манифест удаления рядом с кандидатом (docs/review-02.md F-01): что
    // правило разрешило удалить. По умолчанию включено — без него сохранённый
    // скан нельзя превратить в план, а «пустой план» хуже отсутствия плана.
    bool includeManifests{true};
    bool maskSerials{true};       // серийники дисков → хвост из 4 символов
    bool maskVolumeGuids{true};   // \\?\Volume{…} → \\?\Volume{****}
};

// Итоги считаются по тому, что реально попало в отчёт (то есть с учётом флагов
// ReportOptions) — иначе числа в отчёте противоречили бы его содержимому.
struct ReportTotals {
    std::size_t diskCount{};
    std::size_t partitionCount{};
    std::size_t volumeCount{};
    std::uint64_t diskBytes{};
    std::uint64_t volumeBytes{};
    std::size_t candidateCount{};
    std::uint64_t candidateBytes{};
    std::size_t lockedCandidateCount{};
    std::size_t operationCount{};
    std::size_t untouchedCount{};
    std::size_t succeededCount{};
    std::size_t partialCount{};
    std::size_t failedCount{};
    std::size_t skippedCount{};
    std::uint64_t freedBytes{};
    std::uint64_t failedBytes{};
    std::size_t errorCount{};
    std::uint64_t errorOccurrences{};  // сумма счётчиков повторов
};

// Собранный отчёт. Одна структура на все три вида (скан, dry-run, очистка) —
// отличаются они заполненными секциями и Report::kind.
struct Report {
    int schemaVersion{kReportJsonSchema};
    ReportKind kind{ReportKind::Cleanup};
    ReportEnvironment environment;
    ReportTiming timing;
    ReportOptions options;
    std::vector<PhysicalDisk> disks;            // карта разделов (FR-2, FR-8)
    std::vector<CleanupCandidate> candidates;   // что нашли и с какой оценкой
    // Манифесты удаления — по одному на кандидата из candidates, в том же
    // порядке. Секция кандидата несёт свой манифест (ключ «manifest»), потому
    // что манифест — продолжение кандидата, а не отдельный документ
    // (core::plan.hpp, docs/review-02.md F-01): сохранённый скан без него не
    // даёт плана — `plan --candidates` приходится перечислять корень заново, а
    // это уже не то, что правило разрешило.
    std::vector<CandidateManifest> manifests;
    std::vector<ReportOperation> operations;     // что выполнили
    std::vector<ReportOperation> untouched;      // что оставили и почему
    std::vector<ReportError> errors;
    std::string notes;  // свободный текст: сводка, причина запроса отчёта
};

// Итоги по секциям отчёта с учётом флагов ReportOptions.
ReportTotals summarizeReport(const Report& report);

// Весь отчёт. indent = 2 — человек и diff; -1 — компактно, для отправки.
// Строка заканчивается переводом строки: файл, который не переведён на новую
// строку, ломает вывод в консоли и половину «показать diff» в CI.
std::string reportToJson(const Report& report, int indent = 2);

// Карта разделов отдельно — «Экспорт карты в JSON и в текст» (FR-2). Та же
// схема дисков, что и в отчёте: два артефакта о дисках, которые не должны
// расходиться между собой.
std::string partitionMapToJson(const std::vector<PhysicalDisk>& disks, const ReportOptions& options = ReportOptions{},
                               int indent = 2);

// Нарушения инвариантов отчёта; пустой список — отчёт согласован. Те же проверки,
// что и в validatePlan (§6.3), плюс границы candidateIndex, «у Keep и SkipLocked
// ноль байт» и представимость целых в JSON.
std::vector<std::string> validateReport(const Report& report);

// Сколько значащих цифр серийника остаётся при маскировании (FR-8, §5 приватность).
inline constexpr std::size_t kSerialTailKept = 4;

// Предел путей в манифесте внутри отчёта. Список бывает на десятки тысяч
// путей, а отчёт — это ещё и баг-репорт: полный перечень в него не влезает.
// Обрезанный список помечается неполным, и читатель обязан трактовать его как
// «удалять нельзя» (CandidateManifest::deletable), а не как «удалять начало».
inline constexpr std::size_t kMaxReportManifestPaths = 4096;

// Маскирование серийника: хвост из 4 символов, остальное — звёздочки.
// Пустая строка остаётся пустой (нет диска — нет серийника, а не «****»).
std::string maskSerial(std::string_view serial);

// Маскирование идентификатора тома: \\?\Volume{6f1b…-…} → \\?\Volume{****}.
// Том не опознаётся, но форма пути в отчёте остаётся читаемой.
std::string maskVolumeGuid(std::string_view volumeGuidPath);

// GPT-тип в каноничном виде «xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx», нижний
// регистр. Байты выводятся в том порядке, в каком их записала платформа:
// перестановку делает владелец данных (diskioctls пишет GUID в смешанном
// порядке, и «правильный» текст читается только с её стороны.
std::string guidToString(const Guid& guid);

}  // namespace mrproper::core
