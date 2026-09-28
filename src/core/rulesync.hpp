// Проверка целостности набора правил очистки, версия схемы и откат
// (SPEC §9.2, §6.2 «rulesync», ADR-008).
//
// Модуль переносимый: без Windows API, без сети и без файловой системы.
// Файлы и подпись приносят платформенные слои (src/platform/net — WinHTTP,
// src/platform/rulesync_client — скачивание, подмена каталогов), здесь живёт
// только политика: что считается доверенным набором и что делать, если доверия
// нет. Правила управляют удалением файлов, поэтому канал обновления — враждебный
// вход по построению: неизвестное поле, битый JSON, несовпавший хеш или версия
// схемы — это отказ от применения, а не предупреждение (SPEC §9.2 п.3-4).
//
// Порядок проверки зафиксирован SPEC §9.2 п.2 и не переставляется:
//   1) манифест разбирается и проверяется на схему;
//   2) minAppVersion ≤ версии приложения;
//   3) подпись Ed25519 (реализация — в платформенном слое, здесь политика);
//   4) SHA-256 и размер каждого файла из манифеста;
//   5) файлы разбираются парсером правил (loadVerifiedRuleSet).
// Пропуск любого шага — отказ: приложение продолжает работать на предыдущем
// рабочем наборе и никогда не остаётся без набора правил.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rules.hpp"

namespace mrproper::core {

// Проблема с набором правил. Внутри проверки исключение — не способ сообщить
// об отказе (для этого есть RuleSetVerification), а защита от «забытой
// проверки»: parseManifest и подобные функции обязаны быть вызваны явно.
class RuleSyncError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// --------------------------------------------------------------------- SHA-256
//
// Реализация FIPS 180-4 в ядре, а не вызов BCrypt/Windows CNG: ядро обязано
// собираться и проверяться на любом хосте (SPEC §6.1, ADR-004), а хеш набора
// правил обязан считаться одинаково при проверке на хосте разработчика и на
// рабочей машине.

using Sha256Digest = std::array<std::uint8_t, 32>;  // сырой дайджест, 32 байта

// Инкрементальный хеш: большие файлы правил читаются платформой кусками,
// весь файл в память не помещается (SPEC §5 «потоковая обработка»).
class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t size);
    void update(std::string_view text);

    // Дайджест считается один раз; повторный вызов вернёт то же значение.
    Sha256Digest finish();
    static Sha256Digest of(std::string_view text);

private:
    void compress(const std::uint8_t block[64]);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_{};
    std::uint64_t totalBits_{};
    bool finished_{};
    Sha256Digest digest_{};
};

std::string toHex(const Sha256Digest& digest);

// Строгий разбор: ровно 64 hex-символа, регистр не важен, ничего кроме
// шестнадцатеричных цифр быть не может. Возвращает false, а не бросает:
// битый хеш в манифесте — обычная проверяемая ошибка.
bool parseSha256Hex(std::string_view hex, Sha256Digest& out);
std::string sha256Hex(std::string_view text);

// Сверка реализации с эталонными ответами NIST FIPS 180-4 («» и «abc»).
// Модуль, который решает, что удалять, не вправе молча доверять собственной
// криптографии: вызывающий (правила/CLI/тест) прогоняет проверку один раз при
// старте. false означает «не доверять ничему из скачанного».
bool sha256SelfTest();

// -------------------------------------------------------------------- манифест
//
// Формат — SPEC §9.2:
//   { "schema": 1, "version": "2026.02.1",
//     "files": [{"path":"temp.user.json","sha256":"…","size":1234}],
//     "minAppVersion": "1.0.0" }
//
// Ключ схемы принимается и как "schema", и как "schemaVersion" (в файлах
// правил версия называется schemaVersion — не должно быть двух имён одного
// поля), но задать оба с разными значениями нельзя.

struct ManifestFile {
    std::string path;     // относительный путь внутри набора, "\" приведён к "/"
    std::string sha256;   // 64 hex-символа, нормализован в нижний регистр
    std::uint64_t size{}; // размер в байтах
    bool sizeDeclared{};  // false — размер не объявлен и не сверяется
};

struct Manifest {
    int schemaVersion{};
    std::string version;
    std::string minAppVersion;
    std::vector<ManifestFile> files;

    const ManifestFile* byPath(std::string_view path) const;
};

int supportedManifestSchemaVersion();

// Границы защиты от враждебного входа: манифест правил — это десятки файлов,
// а не мегабайты (SPEC §9.2).
inline constexpr std::size_t kMaxManifestBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxManifestFiles = 4096u;

// Разбор манифеста. Обязательны version (без него не на что опереться при
// откате) и непустой files[]; schema (или schemaVersion) необязателен, но
// обязан быть равен 1, minAppVersion и size в элементе files необязательны.
// Любое нарушение формата — RuleSyncError с указанием места: неизвестное поле,
// отсутствующий файл, неверный тип, повтор пути, путь вне набора («../»,
// абсолютный, диск), не-шестнадцатеричный хеш, схема не из поддерживаемых.
Manifest parseManifest(std::string_view text, std::string_view origin = "manifest.json");

// Сравнение версий «1.2.3»: числовые компоненты, недостающие считаются
// нулями, поэтому «1.2» == «1.2.0». Не-числовая компонента — RuleSyncError
// (тихо превращать «1.0.0-бета» в ноль нельзя: это осознанное решение автора
// правил, а не опечатка).
int compareVersions(std::string_view left, std::string_view right);

// minAppVersion ≤ appVersion. Пустая minAppVersion — ограничений нет.
bool isAppVersionSupported(std::string_view minAppVersion, std::string_view appVersion);

// ------------------------------------------------------------- проверка набора

// Проверка подписи: реализация Ed25519 остаётся в платформенном слое (ADR-008 —
// приватный ключ офлайн у владельца, публичный встроен в бинарник), ядро задаёт
// политику: подпись проверяется всегда и раньше хешей. Сырые байты подписи
// передаются как есть: base64/hex декодирует вызывающий, ядро от формата
// подписи не зависит.
using SignatureVerifier = std::function<bool(std::string_view manifestBytes, std::string_view signatureBytes)>;

struct VerificationPolicy {
    SignatureVerifier verifySignature;  // пусто при requireSignature — это отказ
    bool requireSignature{true};
    // Файл, не перечисленный в манифесте, — отказ (правила вне манифеста
    // никто не подписывал, а удалять по ним будут). true — отказ,
    // false — только запись в отчёт, ok() при этом остаётся истинным.
    bool requireKnownFilesOnly{true};
};

// Файл набора, уже прочитанный платформой и посчитанный ядром.
struct VerifiedFile {
    std::string path;
    std::uint64_t size{};
    Sha256Digest digest{};
};

// Удобная сборка VerifiedFile из содержимого (встроенный набор, тесты, отчёт).
VerifiedFile makeVerifiedFile(std::string_view path, std::string_view content);

enum class FileCheck { Ok, Missing, SizeMismatch, HashMismatch, Unlisted, Duplicate };

struct FileVerdict {
    FileCheck check;
    std::string path;
    std::string expectedSha256;
    std::string actualSha256;
    std::uint64_t expectedSize{};
    std::uint64_t actualSize{};
    bool sizeChecked{};

    std::string describe() const;
};

struct RuleSetVerification {
    Manifest manifest;
    std::vector<FileVerdict> files;

    bool manifestParsed{};
    bool schemaOk{};
    bool versionOk{};
    bool signatureChecked{};
    bool signatureOk{};
    std::string signatureDetail;
    std::string versionDetail;
    // Конкретные причины отказа в порядке обнаружения: сюда же попадает текст
    // ошибки разбора манифеста. Пустой список — набор пригоден к применению.
    std::vector<std::string> problems;

    bool ok() const;                          // набор можно применять
    std::vector<std::string> errors() const;  // причины отказа — для лога и UI
    std::string summary() const;              // одна строка: что и чем закончилось
};

// Проверка без исключений: возвращает полный отчёт, где видно и что сломано,
// и в каком файле. Именно этот вызов делает движок и UI.
RuleSetVerification verifyRuleSet(std::string_view manifestBytes, std::string_view signatureBytes,
                                  const std::vector<VerifiedFile>& files, std::string_view appVersion,
                                  const VerificationPolicy& policy = VerificationPolicy{});

// Тот же разбор, но отказ бросается RuleSyncError: удобно CLI (`rules verify`,
// задача 65) и тестам, где любой отказ — ненулевой код возврата.
void verifyRuleSetOrThrow(std::string_view manifestBytes, std::string_view signatureBytes,
                          const std::vector<VerifiedFile>& files, std::string_view appVersion,
                          const VerificationPolicy& policy = VerificationPolicy{});

// Шаг 3 §9.2: после проверки целостности файлы разбираются парсером правил —
// неизвестное поле или битый JSON здесь уже ошибка. contents — пары
// (путь из манифеста, содержимое); каждый файл манифеста обязан присутствовать,
// каждый лишний файл — отказ. env — дамп «NAME=value\n» окружения.
RuleSet loadVerifiedRuleSet(const RuleSetVerification& verification,
                            const std::vector<std::pair<std::string, std::string>>& contents,
                            std::string_view env, bool* anyUnresolved = nullptr);

// ------------------------------------------------------- откат и офлайн (§9.2)

enum class UpdateAction {
    ApplyCandidate,     // набор проверен — можно подменять рабочий
    KeepCurrent,        // кандидат отвергнут, текущий набор остаётся в силе
    RollbackToLastGood, // текущий набор тоже не подтверждён — откат
    UseEmbeddedSet,     // работаем на встроенном в бинарник наборе
};

struct UpdateDecision {
    UpdateAction action;
    std::string reason;

    bool appliesCandidate() const { return action == UpdateAction::ApplyCandidate; }
};

// Проверка обновлений не чаще раза в 24 часа (SPEC §9.2 п.1).
inline constexpr std::int64_t kUpdateCheckIntervalSeconds = 24 * 60 * 60;

struct RuleSetStatus {
    std::string version;          // "" → активен встроенный набор
    std::string verifiedVersion;  // последняя версия, прошедшая проверку
    std::int64_t installedEpochSeconds{};
    std::int64_t lastCheckEpochSeconds{};
    std::string lastResult;  // человекочитаемый итог последней проверки
    bool lastCheckOk{};
    bool autoUpdateEnabled{true};
    // Активный набор подтверждён. По умолчанию true: состояние по умолчанию —
    // свежая установка на встроенном наборе, а он цел по построению. Скачанный
    // набор помечается проверенным только после успешной проверки.
    bool verified{true};
    bool embedded{true};

    bool shouldCheck(std::int64_t nowEpochSeconds) const;
};

// Решение по результату проверки. Кандидат проверяется, пока не применён, —
// поэтому отказ означает KeepCurrent, а не откат: откатываться не от чего.
// Откат (RollbackToLastGood) — случай, когда и текущий набор не подтверждён.
UpdateDecision decideUpdate(const RuleSetVerification& verification, const RuleSetStatus& current);

RuleSetStatus withAppliedCandidate(const RuleSetStatus& current, const RuleSetVerification& verification,
                                   std::int64_t nowEpochSeconds);
RuleSetStatus withFailedCheck(const RuleSetStatus& current, const RuleSetVerification& verification,
                               std::int64_t nowEpochSeconds);

// «Вернуть встроенный набор» (SPEC §9.2, обязательная кнопка настроек):
// скачанные правила больше не используются, автообновление выключено до
// следующего явного включения.
RuleSetStatus resetToEmbeddedSet(const RuleSetStatus& current, std::int64_t nowEpochSeconds);

// Стартовый выбор набора (SPEC §9.2 п.4): подтверждённый текущий — иначе
// встроенный. «Приложение никогда не остаётся без набора правил».
struct StartupChoice {
    bool useEmbedded{};
    std::string version;
    std::string reason;
};
StartupChoice chooseStartupRuleSet(const RuleSetStatus& current);

// Состояние переживает перезапуск: платформа пишет его в
// %LOCALAPPDATA%\MrProper\rules\state.json. Формат — тот же минимальный JSON,
// что и у отчётов, неизвестные поля игнорируются, битое состояние — не ошибка:
// возвращается безопасный дефолт (встроенный набор, автообновление включено).
std::string serializeRuleSetStatus(const RuleSetStatus& status);
RuleSetStatus parseRuleSetStatus(std::string_view text);

}  // namespace mrproper::core
