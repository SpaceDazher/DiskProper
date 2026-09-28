// MrProper — клиент обновления набора правил: скачивание, проверка,
// атомарная подмена рабочего набора и откат.
// Спека: §9.2 (поток обновления, офлайн-режим и сброс), §9.1 ADR-008
// (правила никогда не применяются без проверки), §4 FR-2, §5 (пути,
// устойчивость), §6.1/§6.2 (границы слоёв), §6.3 (UTF-8 наружу, UTF-16 в WinAPI),
// §6.4 (результаты не мутируются после публикации, отказ — это данные, а не
// исключение), §12 (все отказы в логе с путём и HRESULT).
// Документ с ключом и форматом подписи: tools/rule_keys.md (тот же §9.2).
//
// ---------------------------------------------------------------------------
// Почему модуль устроен так
// ---------------------------------------------------------------------------
// Правила очистки управляют тем, что приложение удаляет, поэтому канал
// обновления — враждебный вход по построению: «скачать JSON и применить» здесь
// означало бы «дать сети указать, что удалять». Отсюда три решения, которые и
// составляют весь модуль.
//
// 1) ПРОВЕРКА НЕ ОБХОДИТСЯ. Ни один путь в этом файле не приводит к применению
//    набора, минуя core::rulesync::verifyRuleSet (схема → minAppVersion →
//    подпись → SHA-256/размер каждого файла) и core::rulesync:
//    loadVerifiedRuleSet (парсер: неизвестное поле и битый JSON — ошибка,
//    SPEC §9.2 п.3). Отказ на любом шаге означает «оставить текущий набор», а не
//    «применить половину».
//
// 2) ПОДПИСЬ ПРОВЕРЯЕТСЯ ВСЕГДА, А ВЕРИФИКАТОР — ВВОДИМАЯ ЗАВИСИМОСТЬ.
//    Реализация Ed25519 (RFC 8032) в этом файле нет сознательно: она требует
//    SHA-512 и арифметики поля, а проверить её в этом модуле нечем —
//    криптографию проверяет tools\sign-rules.ps1 -Action SelfTest, а не код,
//    который сам себя же и проверяет. Вместо этого есть SignaturePolicy: 32
//    байта публичного ключа (core::rules::kRuleSetPublicKey — константа,
//    ревьюится как код) и функция проверки. ПУСТОЙ верификатор — ОТКАЗ, а не
//    «проверка отключена»: забытая зависимость не должна превращаться в
//    успешную проверку (ADR-008).
//
// 3) ПОДМЕНА АТОМАРНА В ПРЕДЕЛАХ ТОМА И ВОССТАНАВЛИВАЕМА. Готовый набор живёт в
//    staging, рабочий — в current, предыдущий рабочий — в previous. Применение
//    это три переименования и журнал apply.journal: пока журнал на месте, любой
//    последующий запуск (в том числе после падения питания) восстанавливает
//    previous в current. Читатель набора — движок очистки — никогда не видит
//    наполовину записанный каталог: он либо видит целый current, либо (если
//    восстановление откатило) целый previous.
//
// ---------------------------------------------------------------------------
// Раскладка на диске (%LOCALAPPDATA%\MrProper\rules)
// ---------------------------------------------------------------------------
//   current/            рабочий набор — только он и читается движком
//   previous/           предыдущий рабочий набор — цель отката
//   staging/            кандидат: сюда качают и здесь проверяют
//   state.json          core::RuleSetStatus (версия, дата, итог проверки)
//   apply.journal       журнал применения; отсутствует — применения не было
//
// ---------------------------------------------------------------------------
// Границы модуля (чтобы работа не расползлась)
// ---------------------------------------------------------------------------
//   * транспорт (сеть) — только GET по HTTPS, без кук и авторизации, таймаут
//     10 с, редиректы не следуются (SPEC §9.2 п.1). Реализация по умолчанию —
//     WinHTTP; интерфейс Transport вводится, чтобы конвейер можно было прогнать
//     без сети (тесты, офлайн-проверка, другой агент со своим net-модулем);
//   * подпись — вводится (см. выше), здесь только байты;
//   * решения «что доверено» — core::rulesync; здесь ввод-вывод и подмена
//     каталогов;
//   * приложение правил к путям, инвентаризация, удаление — не здесь;
//   * запись состояния — core::RuleSetStatus плюс serialize/parse из
//     core::rulesync, своего формата нет.
//
// ---------------------------------------------------------------------------
// Устойчивость
// ---------------------------------------------------------------------------
// Ни одна функция этого модуля не бросает исключений наружу: отказ — это
// заполненный UpdateReport (problems) или строка problem (SPEC §6.4: в горячем
// цикле отказ это флаг, а не исключение). Исключения, которые можно ожидать,
// ровно одни внутри вызываемых ядром и WinAPI (std::bad_alloc, core::RuleError,
// core::RuleSyncError) — они перехватываются на границе и превращаются в
// problem. Согласно SPEC §9.2 п.4 ошибка обновления никогда не должна
// приводить к тому, что у приложения не осталось набора правил.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/rulesync.hpp"

namespace mrproper::platform::rulesync {

// ---------------------------------------------------------------------------
// Имена в раскладке (SPEC §9.2: %LOCALAPPDATA%\MrProper\rules\)
// ---------------------------------------------------------------------------

inline constexpr const char* kAppDirName = "MrProper";
inline constexpr const char* kRuleSetDirName = "rules";
inline constexpr const char* kActiveDirName = "current";
inline constexpr const char* kPreviousDirName = "previous";
inline constexpr const char* kStagingDirName = "staging";
inline constexpr const char* kStateFileName = "state.json";
inline constexpr const char* kJournalFileName = "apply.journal";
inline constexpr const char* kManifestFileName = "manifest.json";
inline constexpr const char* kSignatureFileName = "rules.sig";

// Подпись Ed25519 — 64 сырых байта (R‖S), в файле — base64 (tools/rule_keys.md §1).
inline constexpr std::size_t kSignatureBytes = 64;
// Публичный ключ Ed25519 — ровно 32 сырых байта.
inline constexpr std::size_t kPublicKeyBytes = 32;

using PublicKey = std::array<std::uint8_t, kPublicKeyBytes>;
using RawSignature = std::array<std::uint8_t, kSignatureBytes>;

// Таймаут одного HTTP-запроса (SPEC §9.2 п.1: «таймаут 10 с»).
inline constexpr std::int64_t kDefaultRequestTimeoutMs = 10000;
// Потолок на всё обновление, включая все файлы набора. Набор правил — это
// десятки файлов (core::kMaxManifestFiles = 4096), а не мегабайты: без общего
// потолка проверка на «плохом» канале растягивалась бы на минуты блокировки
// UI. Истёкшее время — обычный отказ, текущий набор остаётся в силе.
inline constexpr std::int64_t kDefaultOverallTimeoutMs = 120000;
// Подпись — 88 символов base64; запас нужен на перевод строки и BOM, но не
// на «что-то ещё».
inline constexpr std::size_t kDefaultMaxSignatureBytes = 4u * 1024u;
// Один файл правил больше 16 МБ — это не файл правил, а ошибка заливки.
inline constexpr std::size_t kDefaultMaxRuleFileBytes = 16u * 1024u * 1024u;
// Потолок на весь набор целиком. Набор — это десятки файлов, а не архив, но
// манифест допускает 4096 записей (core::kMaxManifestFiles), и 4096 × 16 МБ в
// памяти — это 64 ГиБ. Суммарный потолок ограничивает память независимо от
// того, что прислал сервер.
inline constexpr std::size_t kDefaultMaxSetBytes = 128u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// Раскладка каталогов
// ---------------------------------------------------------------------------

// Все пути в UTF-8 (§6.3): наружу модуль говорит как ядро, внутрь обращается к
// WinAPI в UTF-16. Пути задаёт вызывающий (тест может подсунуть свою папку),
// поэтому структура — данные, а не результат вычисления.
struct Layout {
    std::string root;         // %LOCALAPPDATA%\MrProper\rules
    std::string current;      // рабочий набор
    std::string previous;     // предыдущий рабочий набор
    std::string staging;      // кандидат
    std::string stateFile;    // state.json
    std::string journalFile;  // apply.journal

    [[nodiscard]] bool valid() const noexcept;
    // Собрать раскладку под корень rulesRoot: <root>\current, \previous,
    // \staging, state.json, apply.journal.
    [[nodiscard]] static Layout under(std::string_view rulesRoot);
};

// Корень по умолчанию: %LOCALAPPDATA%\MrProper\rules. Пустая строка —
// переменная окружения недоступна (нет профиля, нет окружения): вызывающий на
// этом остаётся на встроенном наборе, поэтому пустой корень — не исключение.
[[nodiscard]] std::string defaultRuleSetRoot();

// То же, но с раскладкой; ok=false означает «раскладку использовать нельзя».
[[nodiscard]] bool defaultLayout(Layout& out);

// ---------------------------------------------------------------------------
// Транспорт
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string url;             // только https:// — иначе отказ (SPEC §9.2 п.1)
    std::size_t maxBytes{};      // потолок тела ответа; превышен — отказ
    std::int64_t timeoutMs{};    // таймаут этого запроса
};

struct HttpResponse {
    bool ok{};                   // удобно, чтобы не смотреть на код вручную
    long status{};               // HTTP-код ответа; 0 — ответа не было
    std::string body;            // тело ответа (валидно только при ok)
    std::string error;           // причина отказа: путь, вызов, HRESULT (§12)

    [[nodiscard]] static HttpResponse failure(std::string reason);
    [[nodiscard]] static HttpResponse success(long statusCode, std::string payload);
};

// Единственный способ, которым модуль ходит в сеть. Вводится интерфейсом по
// двум причинам: конвейер должен проверяться без сети, а реализация HTTP
// принадлежит не этому модулю (в репозитории есть отдельный слой platform::net,
// §6.2) — клиент не должен зависеть от его темпа появления.
class Transport {
public:
    virtual ~Transport() = default;
    // Один GET. Реализация обязана: не ходить по http, не слать куки и
    // авторизацию, не следовать редиректам и оборвать тело на maxBytes.
    virtual HttpResponse get(const HttpRequest& request) = 0;
};

// Транспорт по умолчанию: WinHTTP, только GET, HTTPS обязателен, редиректы не
// следуются, таймауты из запроса, без авторизации. userAgent — идентификация
// приложения в журналах сервера; useSystemProxy по умолчанию false: WPAD-авто-
// определение прокси рассылает запрос во внутреннюю сеть, а правила удаления
// не обязаны этим делиться.
[[nodiscard]] std::unique_ptr<Transport> makeWinHttpTransport(std::wstring_view userAgent, bool useSystemProxy = false);

// ---------------------------------------------------------------------------
// Проверка подписи (вводится вызывающим — см. верх файла)
// ---------------------------------------------------------------------------

// Подпись манифеста: RFC 8032 §6.1, без кофактора, 0 ≤ S < L (tools/rule_keys.md
// §6). Возвращает true только при полном совпадении: любое «почти», включая
// подпись чужим ключом и подмену байтов манифеста, — false.
using SignatureVerify = std::function<bool(std::string_view manifestBytes, const RawSignature& signature)>;

struct SignaturePolicy {
    PublicKey publicKey{};  // все нули — ключ не задан
    SignatureVerify verify; // пусто — верификатора нет

    [[nodiscard]] bool available() const noexcept {
        return static_cast<bool>(verify) && !isZeroKey();
    }
    [[nodiscard]] bool isZeroKey() const noexcept;

    // Готовый верификатор для core::rulesync::VerificationPolicy.
    [[nodiscard]] core::SignatureVerifier toCoreVerifier() const;
};

// ---------------------------------------------------------------------------
// Конфигурация обновления
// ---------------------------------------------------------------------------

struct UpdateConfig {
    // Прямой URL манифеста с пином тега (SPEC §9.2 п.1: «тег rules/vX.Y в
    // репозитории»). Собирать его должен вызывающий: адрес репозитория — это
    // конфигурация сборки, а не константа кода.
    std::string manifestUrl;
    // URL подписи. Пусто — берётся каталог manifestUrl и к нему
    // «rules.sig» (так подпись лежит рядом с манифестом, tools/rule_keys.md §1).
    std::string signatureUrl;
    // Версия приложения — для minAppVersion (§9.2 п.2).
    std::string appVersion;
    SignaturePolicy signature;

    std::int64_t requestTimeoutMs{kDefaultRequestTimeoutMs};
    std::int64_t overallTimeoutMs{kDefaultOverallTimeoutMs};
    std::size_t maxManifestBytes{core::kMaxManifestBytes};
    std::size_t maxSignatureBytes{kDefaultMaxSignatureBytes};
    std::size_t maxRuleFileBytes{kDefaultMaxRuleFileBytes};
    // Потолок на суммарный объём скачанного набора (включая манифест и подпись).
    std::size_t maxSetBytes{kDefaultMaxSetBytes};
    bool useSystemProxy{false};
};

// ---------------------------------------------------------------------------
// Итог проверки
// ---------------------------------------------------------------------------

struct UpdateReport {
    bool checked{};               // до сети дошли (интервал/автообновление не мешали)
    bool downloaded{};            // манифест и файлы набора скачаны в staging
    bool applied{};               // рабочий набор подменён
    bool rolledBack{};            // откат выполнен (восстановление после сбоя или явный)
    bool skippedByInterval{};     // прошло меньше суток с прошлой проверки
    bool skippedAutoUpdateOff{};  // автообновление выключено («вернуть встроенный»)
    bool recovered{};             // перед проверкой восстановлено прерванное применение

    std::string version;              // версия рабочего набора после операции
    std::string candidateVersion;     // версия кандидата, если его удалось разобрать
    std::int64_t elapsedMs{};
    std::size_t filesApplied{};       // файлов скачано и проверено
    std::int64_t bytesApplied{};
    // Отчёт ядра: схема, версия, подпись, пософайловый вердикт по каждому файлу.
    core::RuleSetVerification verification;
    // Причины отказа по шагам, по порядку обнаружения. Пусто — набор применён.
    std::vector<std::string> problems;

    [[nodiscard]] bool ok() const noexcept { return problems.empty() && applied; }
    // Одна строка для журнала и для строки настроек «результат последней
    // проверки» (SPEC §9.2 п.5).
    [[nodiscard]] std::string summary() const;
};

// ---------------------------------------------------------------------------
// Что лежит на диске: рабочий набор
// ---------------------------------------------------------------------------

// Прочитанный с диска набор в форме, которую уже понимает ядро: байты
// манифеста, байты подписи и список файлов с посчитанными хешами. contents —
// те же файлы текстом (нужны core::loadVerifiedRuleSet для проверки парсером).
struct InstalledSet {
    std::string version;
    std::string manifestBytes;
    std::string signatureBytes;
    std::vector<core::VerifiedFile> files;
    std::vector<std::pair<std::string, std::string>> contents;
    std::int64_t modifiedEpochSeconds{};
    bool signaturePresent{};
};

// Прочитать рабочий набор (current). Отсутствие каталога — не ошибка, а
// «скачанного набора нет»: ok=false, problem описывает причину, приложение
// остаётся на встроенном.
[[nodiscard]] bool readInstalledSet(const Layout& layout, InstalledSet& out, std::string& problem);

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

// state.json. Отсутствующий или битый файл — безопасный дефолт из ядра
// (встроенный набор, автообновление включено): приложение обязано стартовать,
// а не падать на экране настроек.
[[nodiscard]] core::RuleSetStatus readStatus(const Layout& layout);
[[nodiscard]] bool writeStatus(const Layout& layout, const core::RuleSetStatus& status, std::string& problem);

// ---------------------------------------------------------------------------
// Восстановление, откат, сброс
// ---------------------------------------------------------------------------

struct RecoveryReport {
    bool journalFound{};
    bool restored{};    // previous вернулся в current
    bool stagingDropped{};  // незавершённый кандидат выброшен
    std::string version;    // версия из журнала, если её удалось прочитать
    std::string detail;
};

// Продолжить прерванное применение: журнал на месте, current нет, previous
// есть → previous становится current. Вызывается сам (перед каждой проверкой)
// и доступен отдельно — приложение зовёт его на старте, ДО чтения набора.
[[nodiscard]] RecoveryReport recoverInterruptedApply(const Layout& layout);

// Откат на предыдущий рабочий набор (кнопка в настройках и аварийный путь,
// когда текущий набор перестал подтверждаться). false — откатываться не
// чему, problem объясняет.
[[nodiscard]] bool rollbackToLastGood(const Layout& layout, const core::RuleSetStatus& current, std::string& problem);

// «Вернуть встроенный набор» (SPEC §9.2, обязательная кнопка): скачанные
// правила удаляются, автообновление выключается до следующего явного включения.
[[nodiscard]] bool resetToEmbeddedSet(const Layout& layout, std::string& problem);

// Включить/выключить автообновление (следующее явное включение после сброса).
[[nodiscard]] bool setAutoUpdateEnabled(const Layout& layout, bool enabled, std::string& problem);

// ---------------------------------------------------------------------------
// base64 (подпись приезжает в base64 — tools/rule_keys.md §1)
// ---------------------------------------------------------------------------

// Строгий разбор: допустимы только A–Z a–z 0–9 + / и выравнивание '=';
// пробельные символы по краям отбрасываются (файл подписи мог прийти с
// переводом строки), внутри — отказ. Выход должен быть ровно 64 байта для
// подписи и ровно 32 для ключа: любая другая длина — не подпись.
[[nodiscard]] bool decodeBase64(std::string_view text, std::vector<std::uint8_t>& out);
[[nodiscard]] std::optional<RawSignature> decodeSignature(std::string_view base64);
[[nodiscard]] std::optional<PublicKey> decodePublicKey(std::string_view base64);
// base64 без переводов строк — так пишет tools\sign-rules.ps1.
[[nodiscard]] std::string toBase64(const std::uint8_t* data, std::size_t size);

// ---------------------------------------------------------------------------
// Окружение
// ---------------------------------------------------------------------------

// Дамп окружения «NAME=value\n» для core::loadVerifiedRuleSet (подстановка
// %LOCALAPPDATA% и прочих переменных в локаторы правил). Скрытые переменные
// Windows («=C:=C:\...») в дамп не попадают: имя начинается с «=» и не
// соответствует ни одному локатору.
[[nodiscard]] std::string environmentDump();

// Текущее время в Unix-секундах: границы интервала в 24 ч (§9.2 п.1) и метки
// установки считаются по нему.
[[nodiscard]] std::int64_t nowEpochSeconds() noexcept;

// ---------------------------------------------------------------------------
// Клиент
// ---------------------------------------------------------------------------

// Готовый к использованию конвейер: раскладка, транспорт и конфигурация в
// одном объекте, время — подменяемое (тесты не должны ждать сутки).
class Client {
public:
    Client(UpdateConfig config, Layout layout, std::unique_ptr<Transport> transport,
           std::function<std::int64_t()> clock = nullptr);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    [[nodiscard]] const UpdateConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] const Layout& layout() const noexcept {
        return layout_;
    }

    // Основной вызов: восстановление → проверка интервала → скачивание в
    // staging → сверка → применение → запись состояния. Никогда не бросает;
    // отказ виден в отчёте.
    [[nodiscard]] UpdateReport checkAndApply(bool force = false);

    // Кнопка «проверить сейчас» (SPEC §9.2 п.5): то же, но без интервала в
    // 24 часа и без учёта выключенного автообновления (пользователь спросил
    // явно).
    [[nodiscard]] UpdateReport checkNow() {
        return checkAndApply(true);
    }

    // Продолжить прерванное применение. Приложение зовёт это на старте до
    // чтения набора — иначе после падения питания движок увидит отсутствие
    // current там, где набор есть.
    [[nodiscard]] RecoveryReport recover() {
        return recoverInterruptedApply(layout_);
    }

    [[nodiscard]] core::RuleSetStatus status() const {
        return readStatus(layout_);
    }

    // Рабочий набор для движка. builtInFiles — файлы встроенного набора
    // (их приложение собирает при сборке бинарника) в том же формате, что
    // contents у InstalledSet. Выбирается по core::chooseStartupRuleSet:
    // подтверждённый скачанный набор, иначе встроенный — «приложение никогда
    // не остаётся без набора правил» (SPEC §9.2 п.4). ok=false — оба набора
    // непригодны (встроенный битый: это уже ошибка сборки), problem с этим.
    // Метод const не потому, что он ничего не делает: неподтверждённый рабочий
    // набор один раз откатывается на предыдущий (writeStatus и переименования
    // каталогов), и это осознанное решение, а не чтение с побочным эффектом.
    [[nodiscard]] bool loadActiveRuleSet(const std::vector<std::pair<std::string, std::string>>& builtInFiles,
                                         core::RuleSet& out, std::string& origin, std::string& problem) const;

    [[nodiscard]] bool rollback(std::string& problem) {
        return rollbackToLastGood(layout_, readStatus(layout_), problem);
    }
    [[nodiscard]] bool resetToEmbedded(std::string& problem) {
        return resetToEmbeddedSet(layout_, problem);
    }
    [[nodiscard]] bool enableAutoUpdate(bool enabled, std::string& problem) {
        return setAutoUpdateEnabled(layout_, enabled, problem);
    }

private:
    UpdateReport runCheck(bool force);

    UpdateConfig config_;
    Layout layout_;
    std::unique_ptr<Transport> transport_;
    std::function<std::int64_t()> clock_;
};

}  // namespace mrproper::platform::rulesync
