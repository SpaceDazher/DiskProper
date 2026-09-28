// Реализация определения блокировок кандидатов. Обоснование границ — в
// locks.hpp; здесь код и замечания, которые видны с этого уровня.
//
// Модуль состоит из трёх частей, и каждая из них отвечает на один вопрос:
//
//   1. `queryPath` — «кто держит вот этот путь»: своя сессия Restart Manager на
//      запрос, ответ RM переводится в `LockInfo` с честным состоянием. Повтор с
//      backoff живёт здесь (FR-6) и только для повторяемых отказов: повтор на
//      InvalidArgument или ResourceNotFound даст тот же ответ, просто позже, а
//      ждать «просто позже» в горячем пути сканирования незачем;
//   2. `checkCandidates` — прогон по набору: заполняет `CleanupCandidate::lockedBy`
//      (FR-4) и ведёт счётчики для отчёта, проверяя отмену раз в 256 элементов
//      (§6.4);
//   3. `reasonText` / `describe` — формулировки для reasons (FR-4), журнала и
//      отчёта (FR-8).
//
// Чего модуль не делает и почему это важно. Ни одно состояние, кроме Free, не
// разрешает удаление, и ни одно не разрешает закрыть чужой процесс: RmShutdown с
// подтверждением — `platform::shutdown` (process_control), а решение «Skip
// (locked)» — `engine::plan_builder`. Дублировать то и другое здесь означало бы
// три места с правилом «занято», которые со временем разойдутся, а разойдутся
// они на файлах, которые приложение удаляет.
//
// Про повтор с backoff. Пауза не «спит целиком»: она нарезана на куски по 25 мс
// и прерывается `std::stop_token`, поэтому снятый скан не ждёт остаток backoff.
// Суммарно три попытки с базой 150 мс добавляют к худшему случаю 450 мс на
// КАНДИДАТА, а не на прогон: это цена одного отказа RM, а не системная
// задержка, потому что повторяемый отказ у каждого пута свой.
//
// Про «кто держит» и pidReused. RM отвечает списком PID, и между его ответом и
// чтением времени старта процесс может умереть, а номер — переиспользоваться
// (шапка restart_manager.hpp). Если переиспользованными оказались все
// названные процессы, ответ описывает прошлое, а не текущее состояние файла:
// такой ответ не превращается в Free (это была бы ложь в отчёте) и уходит в
// тот же цикл повторов. Частичное переиспользование просто выбрасывает такие
// PID из списка — как и делает `platform::toProcessRefs`.
//
// Про логи. Журналирование отказов ведёт сам модуль, а не Restart Manager:
// платформенный `logFailures` выключается, потому что движок пишет одну строку
// на кандидат — с его путём, его состоянием, кодом Win32 и числом попыток, — а
// две строки на один отказ в журнале читаются как два разных отказа.
#include "locks.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"

namespace mrproper::engine::locks {
namespace {

namespace rm = platform::restart_manager;

// Имена событий журнала. Отдельные константы, а не склейка «префикс + суффикс»
// по месту: grep по журналу должен находить все записи модуля одним словом, а
// склейка строк на каждом вызове стоила бы аллокации в горячем пути.
constexpr std::string_view kEventQuery{"engine.locks.query"};
constexpr std::string_view kEventRetry{"engine.locks.retry"};
constexpr std::string_view kEventSummary{"engine.locks.summary"};
constexpr std::string_view kEventCancelled{"engine.locks.cancelled"};

// Частота проверки отмены: раз в 256 кандидатов (SPEC §6.4, «проверка каждые
// 256 элементов»). Проверка на каждом шаге стоила бы лишнего чтения stop_token на
// пустом месте, а 256 вызовов RM подряд — это секунды неотзывчивой отмены.
constexpr std::size_t kStopCheckStride{256};

// Кусок паузы при ожидании backoff: чем короче, тем быстрее отмена, тем больше
// системных вызовов сна при полной паузе. 25 мс — обычная величина для
// «отзывчиво, но не busy-wait».
constexpr std::chrono::milliseconds kSleepSlice{25};

// Сколько имён держателей перечислять в reasons, прежде чем свернуться в
// «и ещё N». Причина не в длине строки: список из двадцати имён в карточке
// кандидата перестаёт быть читаемым, а решение «показать все» остаётся за
// интерфейсом, у которого есть место.
constexpr std::size_t kNamesInReason{3};

// ---------------------------------------------------------------------------
// Вспомогательное (внутреннее)
// ---------------------------------------------------------------------------

// Пауза перед следующей попыткой: линейный рост с потолком. Рост нужен, чтобы
// три попытки подряд не легли на одну секунду, а потолок — чтобы он не стал
// «минутой ожидания» на сотне кандидатов, каждый из которых отказал.
[[nodiscard]] std::chrono::milliseconds backoffFor(int attempt, const Options& options) noexcept {
    if (options.backoff.count() <= 0 || attempt <= 0) return std::chrono::milliseconds::zero();
    const std::chrono::milliseconds pause = options.backoff * attempt;
    if (options.maxBackoff.count() > 0 && pause > options.maxBackoff) return options.maxBackoff;
    return pause;
}

// Пауза с прерыванием. false — пришла отмена, ждать дальше незачем.
[[nodiscard]] bool sleepInterruptible(std::chrono::milliseconds total, std::stop_token token) noexcept {
    std::chrono::milliseconds left = total;
    while (left.count() > 0) {
        if (token.stop_requested()) return false;
        const std::chrono::milliseconds slice = (left < kSleepSlice) ? left : kSleepSlice;
        std::this_thread::sleep_for(slice);
        left -= slice;
    }
    return !token.stop_requested();
}

// Параметры для вызова Restart Manager. Журналирование платформенного слоя
// выключается намеренно: строку об отказе пишет сам модуль (см. шапку .cpp), и
// вторая строка на тот же отказ в журнале читалась бы как два отказа.
[[nodiscard]] rm::QueryOptions queryOptionsFor(const Options& options) noexcept {
    rm::QueryOptions query = options.query;
    query.logFailures = false;
    return query;
}

[[nodiscard]] Holder toHolder(const rm::ProcessInfo& process) {
    Holder holder;
    holder.pid = process.pid;
    holder.name = core::toUtf8(process.displayName());
    // Путь образа — только когда его правда прочитали: у процесса чужого
    // пользователя он пуст, и «прочитали, что пути нет» — это другой текст
    // отчёта, чем «не смогли прочитать» (imagePathKnown).
    if (process.imagePathKnown) holder.imagePath = core::toUtf8(process.imagePath);
    holder.self = process.self;
    holder.critical = process.critical();
    holder.masked = process.shutdownMasked();
    holder.restartable = process.restartable;
    holder.service = process.service();
    return holder;
}

// Перевод ответа RM в `LockInfo`.
//
// Возвращает false — ответ принят, цикл попыток можно завершать. true — ответ
// непригоден (отказ RM или устаревший список PID), и повтор имеет смысл.
// Решение «стоит ли повторять» принимается здесь, а не в цикле, потому что
// половина повторяемых случаев (устаревшие PID) приходит со статусом Ok, и по
// одному статусу её не отличить.
[[nodiscard]] bool absorb(const rm::LockResult& result, LockInfo& info) {
    info.status = result.status;
    info.win32Error = result.win32Error;
    info.rebootReasons = result.rebootReasons;
    info.holders.clear();
    // Текст отказа обнуляется на каждой попытке: ответ предыдущей попытки не
    // должен пережить успешную. Иначе после повтора карточка кандидата
    // показывала бы «state=free … (Restart Manager не ответил: …)» — отчёт,
    // в котором отказ и результат противоречат друг другу (INV: statusText
    // пуст при успехе).
    info.statusText.clear();

    if (!result.ok()) {
        info.state = LockState::Unknown;
        info.statusText = rm::formatStatus(result.status, result.win32Error);
        return rm::isRetryable(result.status);
    }

    std::size_t valid{0};
    info.holders.reserve(result.processes.size());
    for (const rm::ProcessInfo& process : result.processes) {
        // PID уже за другим процессом: имя из ответа RM относится к процессу,
        // которого больше нет, и показывать его в карточке кандидата нельзя
        // (FR-4, FR-8).
        if (process.pidReused) continue;
        info.holders.push_back(toHolder(process));
        ++valid;
    }

    if (result.processes.empty()) {
        info.state = LockState::Free;
        return false;
    }
    if (valid == 0u) {
        // RM ответил, но все названные процессы — не те, что он назвал. Ответ
        // описывает прошлое состояние, и «свободно» из него не следует: файл
        // мог по-прежнему удерживаться процессом, который RM не увидел.
        info.state = LockState::Unknown;
        info.statusText = "Restart Manager назвал только уже несуществующие процессы — ответ устарел";
        return true;
    }

    // Свой процесс идёт первым в общем случае, но состояние выбирается по
    // всему списку: если кроме MrProper держит кто-то ещё, вопрос «закрыть эти
    // приложения» уже не про движок.
    const bool onlySelf = std::all_of(info.holders.begin(), info.holders.end(),
                                      [](const Holder& holder) { return holder.self; });
    info.state = onlySelf ? LockState::SelfHeld : LockState::Locked;
    return false;
}

// Журнал: отказ RM или устаревший ответ по конкретному пути (SPEC §12: путь и
// HRESULT в строке ошибки).
void logQuery(std::string_view path, const LockInfo& info, const Options& options) noexcept {
    if (!options.logFailures) return;
    core::LogFields fields;
    fields.push_back(core::logField("path", path));
    fields.push_back(core::logField("state", toString(info.state)));
    fields.push_back(core::logField("rmStatus", rm::toString(info.status)));
    fields.push_back(core::logField("win32", static_cast<std::uint64_t>(info.win32Error)));
    fields.push_back(core::logField("attempts", info.attempts));
    core::logWarn(kEventQuery, "Restart Manager не дал ответа о блокировке, удаление кандидата запрещено", fields);
}

// Журнал: попытка будет повторена. Отдельным событием, чтобы по журналу было
// видно, где именно время тратится на backoff.
void logRetry(std::string_view path, const LockInfo& info, const Options& options,
              std::chrono::milliseconds pause) noexcept {
    if (!options.logFailures) return;
    core::LogFields fields;
    fields.push_back(core::logField("path", path));
    fields.push_back(core::logField("attempt", info.attempts));
    fields.push_back(core::logField("pauseMs", static_cast<std::int64_t>(pause.count())));
    fields.push_back(core::logField("rmStatus", rm::toString(info.status)));
    core::logDebug(kEventRetry, "повтор проверки блокировки после backoff", fields);
}

void logCancelled(std::size_t checked, std::size_t total) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField("checked", checked));
    fields.push_back(core::logField("total", total));
    core::logWarn(kEventCancelled, "проверка блокировок прервана отменой", fields);
}

// Журнал: итог прогона. Пишется один раз на весь набор, а не на каждый
// кандидат: «из 400 кандидатов 7 заняты» — это строка, ради которой журнал и
// читают, и на 400 строк её никто бы не прочитал.
void logSummary(const CheckResult& result) noexcept {
    core::LogFields fields;
    fields.push_back(core::logField("total", result.locks.size()));
    fields.push_back(core::logField("checked", result.checked));
    fields.push_back(core::logField("free", result.free));
    fields.push_back(core::logField("locked", result.locked));
    fields.push_back(core::logField("closable", result.closable));
    fields.push_back(core::logField("unknown", result.unknown));
    fields.push_back(core::logField("cancelled", result.cancelled));
    core::logInfo(kEventSummary, "проверка блокировок кандидатов завершена", fields);
}

}  // namespace

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

const char* toString(LockState state) noexcept {
    switch (state) {
        case LockState::Free:
            return "free";
        case LockState::Locked:
            return "locked";
        case LockState::SelfHeld:
            return "self_held";
        case LockState::Unknown:
            return "unknown";
    }
    return "unknown";
}

bool LockInfo::canClose() const noexcept {
    return std::any_of(holders.begin(), holders.end(), [](const Holder& holder) { return holder.closable(); });
}

// ---------------------------------------------------------------------------
// Проверка одного кандидата
// ---------------------------------------------------------------------------

LockInfo queryPath(std::string_view utf8Path, const Options& options, std::stop_token token) {
    LockInfo info;
    if (utf8Path.empty()) {
        // Проверять нечего, и это НЕ «файл свободен»: пустой путь в кандидате —
        // ошибка сборки кандидатов, и молчаливый Free означал бы «удалять
        // можно» для пути, которого нет.
        info.statusText = "пустой путь: проверять нечего";
        return info;
    }

    const int maxAttempts = options.maxAttempts < 1 ? 1 : options.maxAttempts;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (token.stop_requested()) {
            info.cancelled = true;
            break;
        }
        info.attempts = attempt;

        // Своя сессия RM на один путь: ресурсы сессии ЗАМЕНЯЮТСЯ при каждой
        // регистрации, поэтому пакетная проверка дала бы объединённый список
        // держателей — а FR-4 требует LockedBy у конкретного кандидата.
        const rm::LockResult result =
            rm::whoLocksFilesUtf8(std::vector<std::string>{std::string(utf8Path)}, queryOptionsFor(options));
        if (!absorb(result, info)) {
            // Отказ, который не повторяется (AccessDenied, NoSession,
            // AnswerTooLarge, ResourceNotFound), уходит из absorb() тем же
            // false, что и успешный ответ, — и без этой строки он не был бы
            // записан в журнал вообще. А отказ обязан попасть в журнал с
            // путём и HRESULT (SPEC §12), иначе кандидат молча уедет в
            // «неизвестно» без следа.
            if (info.unknown()) logQuery(utf8Path, info, options);
            return info;
        }

        logQuery(utf8Path, info, options);
        if (attempt >= maxAttempts) break;

        const std::chrono::milliseconds pause = backoffFor(attempt, options);
        if (pause.count() > 0) {
            logRetry(utf8Path, info, options, pause);
            if (!sleepInterruptible(pause, token)) {
                info.cancelled = true;
                break;
            }
        }
    }

    if (info.cancelled) info.holders.clear();
    return info;
}

LockInfo queryPath(const std::wstring& path, const Options& options, std::stop_token token) {
    // Перевод делает whoLocksFilesUtf8 внутри (он же проверяет, что UTF-8
    // читается, и отвечает InvalidArgument на битом пути). Здесь нужен только
    // UTF-8 для лога и пустой путь, чтобы не открывать сессию RM ради «».
    LockInfo info;
    if (path.empty()) {
        info.statusText = "пустой путь: проверять нечего";
        return info;
    }
    return queryPath(core::toUtf8(path), options, token);
}

// ---------------------------------------------------------------------------
// Прогон по кандидатам
// ---------------------------------------------------------------------------

void applyToCandidate(core::CleanupCandidate& candidate, const LockInfo& info, const Options& options) {
    // Список очищается всегда: старый lockedBy, оставшийся от прошлого прогона,
    // читался бы в плане и отчёте как ответ на сегодняшний вопрос.
    candidate.lockedBy.clear();
    // Ответа нет — заполнять нечем, и выдумывать нельзя: пустой lockedBy здесь
    // означает «не знаю», а состояние Unknown остаётся в LockInfo.
    if (!info.known()) return;

    candidate.lockedBy.reserve(info.holders.size());
    for (const Holder& holder : info.holders) {
        core::ProcessRef ref;
        ref.pid = holder.pid;
        ref.name = holder.name;
        candidate.lockedBy.push_back(std::move(ref));
    }

    if (options.appendReason) candidate.reasons.push_back(reasonText(info));
}

CheckResult checkCandidates(std::vector<core::CleanupCandidate>& candidates, const Options& options,
                            std::stop_token token, const ProgressFn& progress) {
    CheckResult result;
    // Ответ на каждый кандидат, включая непроверенные: по индексу видно, где
    // закончился прогон, и «нет ответа» не выглядит как «нет блокировки».
    result.locks.resize(candidates.size());

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index % kStopCheckStride == 0u && token.stop_requested()) {
            result.cancelled = true;
            break;
        }

        LockInfo info = queryPath(candidates[index].path, options, token);
        applyToCandidate(candidates[index], info, options);

        ++result.checked;
        switch (info.state) {
            case LockState::Free:
                ++result.free;
                break;
            case LockState::Locked:
            case LockState::SelfHeld:
                ++result.locked;
                if (info.canClose()) ++result.closable;
                break;
            case LockState::Unknown:
                ++result.unknown;
                break;
        }

        if (progress) progress(result.checked, candidates.size(), info);
        // Отмена внутри запроса: остаток набора не трогаем, но прогон уже не
        // про «свободные файлы», и вызывающий должен об этом знать. Признак
        // читается ДО переноса info в результат: после std::move список
        // holders пуст, а счётчики и колбэк выше уже отработали по нему.
        const bool cancelled = info.cancelled;
        result.locks[index] = std::move(info);
        if (cancelled) {
            result.cancelled = true;
            break;
        }
    }

    if (options.logFailures) {
        if (result.cancelled) logCancelled(result.checked, result.locks.size());
        logSummary(result);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Формулировки
// ---------------------------------------------------------------------------

std::string reasonText(const LockInfo& info) {
    switch (info.state) {
        case LockState::Free:
            return "блокирующих процессов не найдено";
        case LockState::SelfHeld:
            return "файл держит сам MrProper";
        case LockState::Locked: {
            if (info.holders.empty()) return "файлы держат процессы приложений";
            std::string out = "файлы держат ";
            out += std::to_string(info.holders.size());
            out += (info.holders.size() == 1u) ? " процесс: " : " процессов: ";
            const std::size_t shown = std::min(info.holders.size(), kNamesInReason);
            for (std::size_t i = 0; i < shown; ++i) {
                if (i != 0u) out += ", ";
                out += info.holders[i].name;
            }
            if (info.holders.size() > shown) {
                out += " и ещё ";
                out += std::to_string(info.holders.size() - shown);
            }
            return out;
        }
        case LockState::Unknown: {
            std::string out = "не удалось определить блокирующие процессы";
            if (!info.statusText.empty()) {
                out += ": ";
                out += info.statusText;
            }
            return out;
        }
    }
    return "блокировка не определена";
}

std::string describe(const LockInfo& info) {
    std::string out = "state=";
    out += toString(info.state);
    out += " attempts=";
    out += std::to_string(info.attempts);
    out += " holders=";
    out += std::to_string(info.holders.size());
    for (const Holder& holder : info.holders) {
        out += " ";
        out += holder.name;
        out += "(pid=";
        out += std::to_string(holder.pid);
        if (holder.self) out += ",self";
        if (holder.critical) out += ",critical";
        if (holder.masked) out += ",masked";
        if (!holder.closable()) out += ",not_closable";
        if (holder.restartable) out += ",restartable";
        if (holder.service) out += ",service";
        out += ")";
    }
    if (info.win32Error != 0u) {
        out += " win32=";
        out += std::to_string(info.win32Error);
    }
    if (!info.statusText.empty()) {
        out += " (";
        out += info.statusText;
        out += ")";
    }
    if (const std::string reboot = rm::describeRebootReasons(info.rebootReasons); !reboot.empty()) {
        out += " ";
        out += reboot;
    }
    return out;
}

std::string describe(const CheckResult& result) {
    std::string out = "total=";
    out += std::to_string(result.locks.size());
    out += " checked=";
    out += std::to_string(result.checked);
    out += " free=";
    out += std::to_string(result.free);
    out += " locked=";
    out += std::to_string(result.locked);
    out += " closable=";
    out += std::to_string(result.closable);
    out += " unknown=";
    out += std::to_string(result.unknown);
    if (result.cancelled) {
        out += " cancelled=true skipped=";
        out += std::to_string(result.skipped());
    }
    return out;
}

}  // namespace mrproper::engine::locks
