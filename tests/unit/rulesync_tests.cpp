// Тесты core::rulesync — политика обновления набора правил очистки.
//
// Задача 79, спека §9.2 («Обновление правил с Git»: формат набора, поток
// обновления, офлайн-режим и сброс), §12 («Обновление правил с GitHub
// работает: подписанный набор применяется, неподписанный/битый откатывается с
// записью в лог, работает офлайн-режим»), §11.1 (юнит-тесты ядра), §6.4 (отказ —
// это данные, а не исключение), §9.1 ADR-008 (правила никогда не применяются
// без проверки).
//
// Что здесь проверяется и почему именно это
//
//   1) ПОДПИСЬ. Реализации Ed25519 в ядре нет и быть не должно (ADR-008):
//      верификатор — вводимая зависимость, приватный ключ офлайн у владельца.
//      Значит проверяется политика: «подпись не та» — отказ; «верификатор не
//      задан» при обязательной подписи — тоже отказ (забытая зависимость не
//      должна превращаться в успешную проверку); подпись относится к
//      конкретным байтам манифеста, а не «вообще к набору»; отчёт называет
//      причину по подписи раньше причин по файлам (порядок §9.2 п.2 не
//      переставляется: подпись доказывает происхождение манифеста, хеши —
//      целостность уже подписанного).
//   2) ЦЕЛОСТНОСТЬ ФАЙЛОВ. Несовпадение SHA-256 (в том числе при неизменном
//      размере) и объявленного размера, файл вне манифеста — то есть правило,
//      которого никто не подписывал, — файл отсутствует, файл передан дважды.
//   3) РАЗБОР ПОСЛЕ ПРОВЕРКИ (шаг 3 §9.2): неизвестное поле в файле правил —
//      ошибка даже при верной подписи и совпавшем хеше, иначе подписанный набор
//      может содержать то, чего ядро не понимает.
//   4) ОТКАТ. Кандидат проверяется, пока не применён, поэтому отказ — это
//      KeepCurrent, а не откат: откатываться не от чего. Откат — случай, когда
//      и текущий набор не подтверждён.
//   5) ОФЛАЙН-РЕЖИМ И СБРОС: проверка не чаще раза в 24 часа, «приложение
//      никогда не остаётся без набора правил», кнопка «вернуть встроенный набор»
//      отключает автообновление, состояние переживает перезапуск, а битое
//      состояние не должно ронять запуск.
//
// Разделение с tests/unit/rules_manifest_tests.cpp: там проверяются ДАННЫЕ
// (согласованность rules/ на диске, совпадение манифеста с файлами), здесь —
// ПОВЕДЕНИЕ ядра на враждебном входе, которого на диске нет и быть не может.
// Поэтому манифесты и файлы правил синтетические и намеренно не берутся с диска:
// тест политики обновления не должен зависеть от того, чем случайно наполнен
// rules/. Реальный набор проверяется отдельно, на настоящих файлах.
#include "harness.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rules.hpp"
#include "rulesync.hpp"

using namespace mrproper::core;

namespace {

[[noreturn]] void fail(const std::string& what) { throw mrp::Failure{what}; }

void require(bool ok, const std::string& what) {
    if (!ok) fail(what);
}

// ------------------------------------------------------------------ константы

// Версия синтетического набора. Сама по себе неважна, важно что версия
// обязательна: на неё опирается откат (SPEC §9.2, откат на последний хороший).
const char* kSetVersion = "2026.02.1-probe";
const char* kPreviousVersion = "2026.01.4-probe";
const char* kAppVersion = "1.0.0";

// Подпись синтетического манифеста. Настоящей криптографии в тесте нет и быть
// не должно: примитив Ed25519 проверяет владелец ключа (tools/sign-rules.ps1,
// ADR-008), а ядро получает готовый вердикт. Тесту важно, что байты подписи и
// байты манифеста доходят до проверки без изменений и что «подпись не та» —
// отказ.
const char* kGoodSignature = "ed25519:probe-signature-v1";

// Окружение для подстановки в locator'ы. Не настоящее: правила — данные, тест
// не должен зависеть от машины, на которой запущен.
const char* kEnvDump = "TEMP=C:\\Users\\Tester\\AppData\\Local\\Temp\n"
                      "LOCALAPPDATA=C:\\Users\\Tester\\AppData\\Local\n"
                      "USERPROFILE=C:\\Users\\Tester\n";

// Момент времени для всех проверок расписания. Реальные часы не используются:
// тест расписания обновлений, зависящий от времени запуска, проверял бы работу
// календаря, а не решение ядра.
constexpr std::int64_t kNow = 1700000000;  // 2023-11-14T22:13:20Z

// ------------------------------------------------------------------- утилиты

std::string replaceFirst(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) fail("replaceFirst: в тексте нет \"" + std::string(from) + "\"");
    text.replace(at, from.size(), to);
    return text;
}

const FileVerdict* verdictFor(const RuleSetVerification& report, std::string_view path) {
    for (const FileVerdict& verdict : report.files) {
        if (verdict.path == path) return &verdict;
    }
    return nullptr;
}

std::string describeChecks(const RuleSetVerification& report) {
    std::string out;
    for (const FileVerdict& verdict : report.files) {
        if (!out.empty()) out += "; ";
        out += verdict.describe();
    }
    return out.empty() ? std::string{"(файлы не проверялись)"} : out;
}

bool allFilesOk(const RuleSetVerification& report) {
    for (const FileVerdict& verdict : report.files) {
        if (verdict.check != FileCheck::Ok) return false;
    }
    return true;
}

bool mentions(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool problemsMention(const RuleSetVerification& report, std::string_view needle) {
    for (const std::string& problem : report.problems) {
        if (mentions(problem, needle)) return true;
    }
    return false;
}

// Позиция первого упоминания в списке причин отказа. Порядок причин важен
// отдельно от их наличия: «есть где-то слово» не говорит, что подпись
// упомянута раньше файлов (SPEC §9.2 п.2).
int problemIndex(const RuleSetVerification& report, std::string_view needle) {
    for (std::size_t i = 0; i < report.problems.size(); ++i) {
        if (mentions(report.problems[i], needle)) return static_cast<int>(i);
    }
    return -1;
}

// Текст отказа: пустая строка означает «отказа не было». Ловит и неверный тип
// исключения, и его отсутствие — молчаливый успех здесь опаснее падения.
std::string rejectionOf(const std::function<void()>& action) {
    try {
        action();
    } catch (const RuleSyncError& e) {
        return std::string{"RuleSyncError: "} + e.what();
    } catch (const RuleError& e) {
        return std::string{"RuleError: "} + e.what();
    } catch (const std::exception& e) {
        return std::string{"std::exception: "} + e.what();
    }
    return {};
}

void requireRejected(const std::function<void()>& action, const std::string& what) {
    require(!rejectionOf(action).empty(), what + ": отказа не было");
}

// Отказ обязан прийти именно через RuleSyncError (ядро), а не через RuleError
// (парсер правил) и не через std::exception: тип ошибки — часть контракта, по
// нему вызывающий отличает «плох канал» от «плох набор».
void requireSyncRejection(const std::function<void()>& action, const std::string& what) {
    try {
        action();
    } catch (const RuleSyncError&) {
        return;
    } catch (const std::exception& e) {
        fail(what + ": отказ пришёл не через RuleSyncError (" + e.what() + ")");
    }
    fail(what + ": отказа не было — враждебный вход принят");
}

// ------------------------------------------------------- синтетический набор

// Файл правил, который ядро обязано разобрать: узкое safe-правило с возрастным
// порогом (SPEC §10 — «safe» не бывает широким). Версия объявляется, когда её
// проверяет набор, — loadVerifiedRuleSet сверяет её с манифестом.
std::string ruleFile(std::string_view id = "temp.user.probe", std::string_view version = "") {
    std::string out = R"json({"schemaVersion":1)json";
    if (!version.empty()) out += R"json(,"version":")json" + std::string(version) + R"json(")json";
    out += R"json(,"rules":[{"id":")json" + std::string(id) +
           R"json(","category":"temp.user","safety":"safe","locator":"%TEMP%/*","minAgeDays":1,)json"
           R"json("title":{"ru":"тест","en":"test"}}]})json";
    return out;
}

struct ManifestEntry {
    std::string path;
    std::string sha256;
    bool withSize{};
    std::size_t size{};
};

// Манифест по формату SPEC §9.2. minAppVersion пустым означает «поле не
// объявлено» — ограничений по нему нет.
std::string buildManifest(const std::vector<ManifestEntry>& files, std::string_view version = kSetVersion,
                          std::string_view minAppVersion = "1.0.0") {
    std::string out = R"json({"schema":1,"version":")json" + std::string(version) + R"json(")json";
    if (!minAppVersion.empty()) out += R"json(,"minAppVersion":")json" + std::string(minAppVersion) + R"json(")json";
    out += R"json(,"files":[)json";
    bool first = true;
    for (const ManifestEntry& entry : files) {
        if (!first) out += ",";
        first = false;
        out += R"json({"path":")json" + entry.path + R"json(","sha256":")json" + entry.sha256 + R"json(")json";
        if (entry.withSize) out += R"json(,"size":)json" + std::to_string(entry.size);
        out += "}";
    }
    out += "]}";
    return out;
}

// Целый эталонный набор: один файл правил, манифест с его хешем и размером,
// файл как его принёс бы платформенный слой.
struct ProbeSet {
    std::string ruleBytes;
    std::string manifest;
    std::vector<VerifiedFile> files;
};

const char* kProbeFile = "temp.user.json";

ProbeSet probeSet(std::string_view fileName = kProbeFile, std::string_view ruleId = "temp.user.probe",
                  std::string_view version = kSetVersion) {
    ProbeSet set;
    set.ruleBytes = ruleFile(ruleId, version);
    set.manifest = buildManifest({{std::string(fileName), sha256Hex(set.ruleBytes), true, set.ruleBytes.size()}}, version);
    set.files = {makeVerifiedFile(fileName, set.ruleBytes)};
    return set;
}

// ------------------------------------------------------------ политики ядра

// Политика «подпись проверяет платформенный слой»: ядро получает верификатор и
// байты подписи, примитив Ed25519 остаётся снаружи (ADR-008).
VerificationPolicy signedPolicy(SignatureVerifier verifier, bool requireKnownFilesOnly = true) {
    VerificationPolicy policy;
    policy.verifySignature = std::move(verifier);
    policy.requireSignature = true;
    policy.requireKnownFilesOnly = requireKnownFilesOnly;
    return policy;
}

// Верификатор принимает подпись, только если совпали и подпись, и сами байты
// манифеста. Второе условие — и есть смысл подписи: она относится к конкретному
// документу, а не «к набору вообще».
SignatureVerifier signerOf(std::string_view expectedManifest, std::string_view expectedSignature, int* calls = nullptr) {
    const std::string manifest(expectedManifest);
    const std::string signature(expectedSignature);
    return [manifest, signature, calls](std::string_view manifestBytes, std::string_view signatureBytes) {
        if (calls != nullptr) ++*calls;
        return manifestBytes == manifest && signatureBytes == signature;
    };
}

// Политика без требования подписи: подпись в ядре не проверяется. Ею
// пользуются проверки, которым подпись не нужна ( сверка данных rules/,
// задача 28).
VerificationPolicy unsignedPolicy(bool requireKnownFilesOnly = true) {
    VerificationPolicy policy;
    policy.requireSignature = false;
    policy.requireKnownFilesOnly = requireKnownFilesOnly;
    return policy;
}

// ------------------------------------------------------------ состояние набора

// Состояние «скачанный набор применён и подтверждён» — обычное состояние после
// успешного обновления (SPEC §9.2 п.3).
RuleSetStatus appliedStatus(std::string version = kSetVersion) {
    RuleSetStatus status;
    status.version = version;
    status.verifiedVersion = version;
    status.installedEpochSeconds = kNow;
    status.lastCheckEpochSeconds = kNow;
    status.lastResult = "набор " + version + ": проверен";
    status.lastCheckOk = true;
    status.autoUpdateEnabled = true;
    status.verified = true;
    status.embedded = false;
    return status;
}

// Состояние «набор применён, но не подтверждён»: например, подмена каталогов
// не дошла до конца и откат ещё не отработал. Такой набор нельзя применять
// и нельзя читать — из него выбирается встроенный.
RuleSetStatus unverifiedStatus(std::string version = kSetVersion) {
    RuleSetStatus status = appliedStatus(version);
    status.verified = false;
    return status;
}

const char* actionName(UpdateAction action) {
    switch (action) {
        case UpdateAction::ApplyCandidate: return "ApplyCandidate";
        case UpdateAction::KeepCurrent: return "KeepCurrent";
        case UpdateAction::RollbackToLastGood: return "RollbackToLastGood";
        case UpdateAction::UseEmbeddedSet: return "UseEmbeddedSet";
    }
    return "unknown";
}

std::string decisionText(const UpdateDecision& decision) {
    return std::string{actionName(decision.action)} + ": " + decision.reason;
}

}  // namespace

// ===========================================================================
// 1) Подпись (SPEC §9.2 п.2, шаг 3; ADR-008)
// ===========================================================================

TEST(rulesync_signedSetIsAcceptedAndBrokenSignatureIsRefused) {
    const ProbeSet set = probeSet();
    int calls = 0;

    // Эталон: подпись верна, верификатор вызван ровно один раз.
    const RuleSetVerification good =
        verifyRuleSet(set.manifest, kGoodSignature, set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature, &calls)));
    require(good.ok(), "подписанный набор с верной подписью отвергнут: " + good.summary() + " [" +
                           describeChecks(good) + "]");
    require(good.signatureChecked, "подпись помечена непроверенной, хотя верификатор ответил");
    require(good.signatureOk, "верная подпись не признана верной");
    require(calls == 1, "верификатор подписи вызван " + std::to_string(calls) + " раз вместо одного");
    require(mentions(good.summary(), "подпись"), "итог проверки не упоминает подпись: " + good.summary());
    require(good.problems.empty(), "у принятого набора не должно быть причин отказа");

    // Подделка: подпись от другого набора. Хеши файлов при этом целы — отказ
    // обязан прийти от подписи, а не от совпадения случайных сумм.
    int forgedCalls = 0;
    const RuleSetVerification forged =
        verifyRuleSet(set.manifest, "ed25519:other-signature", set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature, &forgedCalls)));
    require(!forged.ok(), "набор с чужой подписью принят — подпись не проверяется");
    require(forged.signatureChecked, "подпись не отмечена проверенной, хотя верификатор вызван");
    require(!forged.signatureOk, "чужая подпись признана верной");
    require(forgedCalls == 1, "при отказе по подписи верификатор вызван " + std::to_string(forgedCalls) +
                                  " раз вместо одного");
    require(problemsMention(forged, "подпись не прошла"),
            "в отказе нет причины по подписи: " + forged.summary());
    require(mentions(forged.signatureDetail, "подпись"), "в отчёте нет подробностей о подписи: " +
                                                             forged.signatureDetail);
    require(allFilesOk(forged), "файлы объявлены негодными, хотя отказ обязан быть по подписи: " +
                                    describeChecks(forged));
    require(mentions(forged.summary(), "отказ"), "итог не называет отказом: " + forged.summary());

    // Пустая подпись при обязательной — тоже отказ, а не «ну, раз нет подписи».
    const RuleSetVerification noSignature =
        verifyRuleSet(set.manifest, "", set.files, kAppVersion, signedPolicy(signerOf(set.manifest, kGoodSignature)));
    require(!noSignature.ok(), "набор без подписи принят при обязательной подписи");
    require(!noSignature.signatureOk, "отсутствующая подпись признана годной");
    require(problemsMention(noSignature, "отсутствует"),
            "в отказе не сказано, что подписи нет: " + noSignature.summary());
}

TEST(rulesync_signatureIsCheckedAgainstExactManifestBytes) {
    const ProbeSet set = probeSet();
    const VerificationPolicy policy = signedPolicy(signerOf(set.manifest, kGoodSignature));

    // Меняем в манифесте ровно один символ версии набора. Хеши файлов целы, но
    // документ уже другой: подпись, выданная на исходный манифест, его не
    // покрывает.
    const std::string edited = replaceFirst(set.manifest, kSetVersion, "2026.02.9-probe");
    const RuleSetVerification report = verifyRuleSet(edited, kGoodSignature, set.files, kAppVersion, policy);
    require(!report.ok(), "изменённый манифест принят по подписи, выданной на исходный");
    require(report.manifestParsed, "изменённый манифест не разобран — тест проверяет подпись, а не разбор");
    require(report.signatureChecked && !report.signatureOk, "подпись на изменённый манифест не проверялась");
    require(allFilesOk(report),
            "отказ должен быть только по подписи, а файлы помечены: " + describeChecks(report));

    // Обратная сторона: подпись, выданная на изменённый манифест, принимается.
    // Иначе проверка была бы не проверкой, а сравнением с одной строкой.
    const RuleSetVerification resigned =
        verifyRuleSet(edited, kGoodSignature, set.files, kAppVersion, signedPolicy(signerOf(edited, kGoodSignature)));
    require(resigned.ok(), "манифест отвергнут по подписи, выданной ровно на него: " + resigned.summary());

    // Верификатор обязан получать переданные байты как есть: любая нормализация
    // означала бы, что подтверждён не тот документ, который подписан.
    std::string seenManifest;
    std::string seenSignature;
    SignatureVerifier recorder = [&seenManifest, &seenSignature](std::string_view manifestBytes,
                                                                 std::string_view signatureBytes) {
        seenManifest.assign(manifestBytes.data(), manifestBytes.size());
        seenSignature.assign(signatureBytes.data(), signatureBytes.size());
        return true;
    };
    const RuleSetVerification recorded =
        verifyRuleSet(set.manifest, kGoodSignature, set.files, kAppVersion, signedPolicy(std::move(recorder)));
    require(recorded.ok(), "верификатор принял подпись, а набор отвергнут: " + recorded.summary());
    require(seenManifest == set.manifest, "верификатор получил байты манифеста, отличные от переданных");
    require(seenSignature == kGoodSignature, "верификатор получил подпись, отличную от переданной");
}

TEST(rulesync_requiredSignatureWithoutVerifierIsRefusedNotSkipped) {
    // Забытая зависимость — самая дорогая ошибка этого модуля: пустой
    // верификатор при requireSignature обязан давать отказ, а не «проверку,
    // которой не было» (ADR-008, комментарий в src/core/rulesync.hpp).
    const ProbeSet set = probeSet();
    const VerificationPolicy noVerifier = signedPolicy(SignatureVerifier{});

    const RuleSetVerification forgot =
        verifyRuleSet(set.manifest, kGoodSignature, set.files, kAppVersion, noVerifier);
    require(!forgot.ok(), "набор принят при обязательной подписи без верификатора");
    require(!forgot.signatureChecked, "подпись помечена проверенной, хотя проверять было нечем");
    require(!forgot.signatureOk, "подпись помечена годной без верификатора");
    require(mentions(forgot.signatureDetail, "верификатор"),
            "в отказе не сказано, что нечем проверять подпись: " + forgot.signatureDetail);
    require(problemsMention(forgot, "верификатор"),
            "причина отказа не попала в список problems: " + forgot.summary());

    // Ядро не должно и падать наружу: отказ — это данные (SPEC §6.4).
    const RuleSetVerification bothAbsent = verifyRuleSet(set.manifest, "", set.files, kAppVersion, noVerifier);
    require(!bothAbsent.ok(), "набор принят без подписи и без верификатора");
    require(mentions(bothAbsent.signatureDetail, "верификатор"),
            "проверка дошла до подписи вместо того, чтобы отказаться на отсутствии верификатора: " +
                bothAbsent.signatureDetail);
}

TEST(rulesync_signatureNotRequiredStaysUncheckedButUsable) {
    // Обратная сторона политики: когда подпись не требуется, набор применяться
    // может — но отчёт обязан честно сказать, что подпись не проверялась.
    // Иначе «проверено» в настройках будет означать «проверено чем-то».
    const ProbeSet set = probeSet();

    const RuleSetVerification absent = verifyRuleSet(set.manifest, "", set.files, kAppVersion, unsignedPolicy());
    require(absent.ok(), "набор без подписи отвергнут политикой, которая подпись не требует: " + absent.summary());
    require(!absent.signatureChecked, "подпись помечена проверенной, хотя её не было");
    require(absent.signatureOk, "отсутствующая подпись помечена негодной при requireSignature=false");
    require(mentions(absent.signatureDetail, "не требуется"),
            "в отчёте не сказано, что подпись не требуется: " + absent.signatureDetail);

    const RuleSetVerification unchecked =
        verifyRuleSet(set.manifest, kGoodSignature, set.files, kAppVersion, unsignedPolicy());
    require(unchecked.ok(), "политика отключила подпись, а набор отвергнут: " + unchecked.summary());
    require(unchecked.signatureOk, "подпись помечена негодной при отключённой проверке");
    require(!unchecked.signatureChecked, "непроверенная подпись помечена проверенной");
    require(mentions(unchecked.signatureDetail, "отключена"),
            "в отчёте не сказано, что проверка подписи отключена: " + unchecked.signatureDetail);
}

TEST(rulesync_signatureProblemIsReportedBeforeFileHashes) {
    // Порядок проверок зафиксирован SPEC §9.2 п.2 и не переставляется. Отчёт
    // читает человек в настройках и CI в логе: если первым он увидит
    // «файл temp.user.json не совпал», он будет чинить не то. Подпись идёт
    // раньше хешей, поэтому и в problems она должна быть раньше.
    const ProbeSet set = probeSet();
    const std::string tampered = set.ruleBytes + "\n";  // лишний байт — целостность нарушена
    const RuleSetVerification report =
        verifyRuleSet(set.manifest, "ed25519:wrong", {makeVerifiedFile(kProbeFile, tampered)}, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));

    require(!report.ok(), "битый набор с чужой подписью принят");
    const int signatureAt = problemIndex(report, "подпись");
    const int fileAt = problemIndex(report, kProbeFile);
    require(signatureAt >= 0, "в отказе нет ни слова о подписи: " + report.summary());
    require(fileAt > signatureAt,
            "файл назван раньше подписи — порядок §9.2 п.2 нарушен: " + report.summary());
    require(static_cast<std::size_t>(fileAt) < report.problems.size(),
            "в отчёте нет ни одной причины по файлу: " + report.summary());
}

TEST(rulesync_verifyOrThrowNamesEveryReasonOfRefusal) {
    // verifyRuleSetOrThrow — вход для CLI («rules verify», задача 65) и для
    // тестов: отказ обязан быть исключением с перечислением причин, а не
    // молчаливым «false».
    const ProbeSet set = probeSet();
    require(rejectionOf([&] {
               verifyRuleSetOrThrow(set.manifest, kGoodSignature, set.files, kAppVersion,
                                    signedPolicy(signerOf(set.manifest, kGoodSignature)));
           })
                .empty(),
            "проверенный набор отвергнут verifyRuleSetOrThrow");

    const std::string message = rejectionOf([&] {
        verifyRuleSetOrThrow(set.manifest, "ed25519:wrong", {makeVerifiedFile(kProbeFile, set.ruleBytes + "\n")},
                             kAppVersion, signedPolicy(signerOf(set.manifest, kGoodSignature)));
    });
    require(!message.empty(), "verifyRuleSetOrThrow не отказал на битом наборе");
    require(mentions(message, "RuleSyncError"), "отказ пришёл не через RuleSyncError: " + message);
    require(mentions(message, kSetVersion), "в отказе нет версии набора: " + message);
    require(mentions(message, "подпись"), "в отказе нет причины по подписи: " + message);
    require(mentions(message, kProbeFile), "в отказе нет имени файла: " + message);
}

// ===========================================================================
// 2) Целостность файлов набора (SPEC §9.2 п.2, шаг 4)
// ===========================================================================

TEST(rulesync_detectsHashMismatchEvenWhenSizeIsUnchanged) {
    const ProbeSet set = probeSet();
    // Подмена байта без изменения длины: сверка «по размеру» её бы пропустила.
    std::string tampered = set.ruleBytes;
    tampered[tampered.size() - 4] = '~';
    const std::size_t tamperedSize = tampered.size();

    const RuleSetVerification report =
        verifyRuleSet(set.manifest, kGoodSignature, {makeVerifiedFile(kProbeFile, tampered)}, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));
    require(!report.ok(), "изменённый файл принят как целый — сверка хешей не работает");
    require(tamperedSize == set.ruleBytes.size(), "подмена не сохранила размер — тест проверяет не то");

    const FileVerdict* verdict = verdictFor(report, kProbeFile);
    require(verdict != nullptr, "в отчёте нет вердикта по файлу: " + describeChecks(report));
    require(verdict->check == FileCheck::HashMismatch,
            "ожидался HashMismatch, получено: " + verdict->describe());
    require(verdict->actualSha256 == sha256Hex(tampered), "в вердикте не фактический хеш файла");
    require(verdict->expectedSha256 == sha256Hex(set.ruleBytes), "в вердикте не ожидаемый хеш из манифеста");
    require(verdict->actualSha256 != verdict->expectedSha256, "в вердикте совпали фактический и ожидаемый хеши");
    require(verdict->actualSize == verdict->expectedSize, "в вердикте разошёлся размер при равной длине файлов");
    require(mentions(verdict->describe(), kProbeFile), "описание вердикта не называет файл: " + verdict->describe());
    require(problemsMention(report, kProbeFile), "в отказе не назван изменённый файл: " + report.summary());
    require(mentions(report.summary(), "отказ"), "итог не называет отказом: " + report.summary());
}

TEST(rulesync_detectsSizeMismatchDeclaredInManifest) {
    // Объявленный размер сверяется отдельно от хеша: файл изменён и в размере,
    // и в содержимом, и отчёт говорит про размер — самая грубая и самая
    // понятная неправда.
    const ProbeSet set = probeSet();
    const RuleSetVerification report =
        verifyRuleSet(set.manifest, kGoodSignature, {makeVerifiedFile(kProbeFile, std::string{})}, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));
    require(!report.ok(), "файл не того размера принят");

    const FileVerdict* verdict = verdictFor(report, kProbeFile);
    require(verdict != nullptr, "в отчёте нет вердикта по файлу: " + describeChecks(report));
    require(verdict->check == FileCheck::SizeMismatch, "ожидался SizeMismatch, получено: " + verdict->describe());
    require(verdict->sizeChecked, "размер объявлен манифестом, но не отмечен проверенным");
    require(verdict->expectedSize == set.ruleBytes.size(), "в вердикте не ожидаемый размер из манифеста");
    require(verdict->actualSize == 0, "в вердикте не фактический размер файла");
    require(problemsMention(report, kProbeFile), "в отказе не назван файл неверного размера: " + report.summary());
}

TEST(rulesync_checksHashWhenManifestDeclaresNoSize) {
    // size в манифесте необязателен. Тогда сверяется только хеш, и вердикт
    // обязан честно сказать, что размер не проверялся (иначе UI покажет
    // «размер сходится» по полю, которое никто не сверял).
    const std::string rule = ruleFile();
    const std::string manifest = buildManifest({{kProbeFile, sha256Hex(rule)}}, kSetVersion, "");

    const Manifest parsed = parseRuleSetManifest(manifest);
    const ManifestFile* declared = parsed.byPath(kProbeFile);
    require(declared != nullptr, "разбор манифеста потерял файл");
    require(!declared->sizeDeclared, "разбор объявил размер, которого в манифесте нет");

    const RuleSetVerification honest =
        verifyRuleSet(manifest, kGoodSignature, {makeVerifiedFile(kProbeFile, rule)}, kAppVersion,
                      signedPolicy(signerOf(manifest, kGoodSignature)));
    require(honest.ok(), "целый набор без объявленного размера отвергнут: " + honest.summary() + " [" +
                             describeChecks(honest) + "]");
    const FileVerdict* good = verdictFor(honest, kProbeFile);
    require(good != nullptr && good->check == FileCheck::Ok, "ожидался FileCheck::Ok: " + describeChecks(honest));
    require(good != nullptr && !good->sizeChecked, "размер помечен проверенным, хотя не объявлен");

    std::string tampered = rule;
    tampered[4] = '!';  // тот же размер, другой байт
    const RuleSetVerification broken =
        verifyRuleSet(manifest, kGoodSignature, {makeVerifiedFile(kProbeFile, tampered)}, kAppVersion,
                      signedPolicy(signerOf(manifest, kGoodSignature)));
    require(!broken.ok(), "изменённый файл принят при сверке только по хешу");
    const FileVerdict* bad = verdictFor(broken, kProbeFile);
    require(bad != nullptr && bad->check == FileCheck::HashMismatch,
            "ожидался HashMismatch при отсутствующем размере: " + describeChecks(broken));
}

TEST(rulesync_unlistedFileIsRefusedEvenWithValidSignature) {
    // Главное свойство канала: верная подпись манифеста не делает доверенным
    // файл, которого в манифесте нет. По нему будут удалять, а подписывал его
    // никто.
    const ProbeSet set = probeSet();
    std::vector<VerifiedFile> withExtra = set.files;
    withExtra.push_back(makeVerifiedFile("wer.json", ruleFile("wer.probe")));

    const RuleSetVerification strict =
        verifyRuleSet(set.manifest, kGoodSignature, withExtra, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));
    require(!strict.ok(), "файл вне манифеста принят вместе с верной подписью манифеста");
    require(problemsMention(strict, "wer.json"), "в отказе не назван лишний файл: " + strict.summary());
    const FileVerdict* extra = verdictFor(strict, "wer.json");
    require(extra != nullptr && extra->check == FileCheck::Unlisted,
            "лишний файл не помечен Unlisted: " + describeChecks(strict));

    // Мягкая политика: лишний файл перестаёт быть отказом, но остаётся
    // помеченным — это запись для журнала (SPEC §9.2 п.4).
    const RuleSetVerification soft =
        verifyRuleSet(set.manifest, kGoodSignature, withExtra, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature), false));
    require(soft.ok(), "мягкая политика всё ещё отвергает лишний файл: " + soft.summary());
    const FileVerdict* reported = verdictFor(soft, "wer.json");
    require(reported != nullptr && reported->check == FileCheck::Unlisted,
            "при мягкой политике лишний файл не помечен Unlisted: " + describeChecks(soft));
    require(mentions(reported->describe(), "не перечислен"), "описание лишнего файла не вводит в заблуждение: " +
                                                                reported->describe());
}

TEST(rulesync_missingAndDuplicatedFilesAreRefused) {
    const ProbeSet first = probeSet(kProbeFile, "temp.user.probe");
    const ProbeSet second = probeSet("logs.json", "logs.system.probe");
    const std::string manifest = buildManifest({{kProbeFile, sha256Hex(first.ruleBytes), true, first.ruleBytes.size()},
                                                 {"logs.json", sha256Hex(second.ruleBytes), true,
                                                  second.ruleBytes.size()}});
    const std::vector<VerifiedFile> onlyFirst = {makeVerifiedFile(kProbeFile, first.ruleBytes)};

    // Файл манифеста не доехал — отказ при любой политике: набор без одного из
    // объявленных правил не тот набор, который подписали.
    const RuleSetVerification missing =
        verifyRuleSet(manifest, kGoodSignature, onlyFirst, kAppVersion,
                      signedPolicy(signerOf(manifest, kGoodSignature), false));
    require(!missing.ok(), "набор с недостающим файлом принят");
    const FileVerdict* absent = verdictFor(missing, "logs.json");
    require(absent != nullptr && absent->check == FileCheck::Missing,
            "отсутствующий файл не помечен Missing: " + describeChecks(missing));
    require(problemsMention(missing, "logs.json"), "в отказе не назван отсутствующий файл: " + missing.summary());

    // Один и тот же файл передан дважды: две версии одного правила под одним
    // именем неразличимы, и проверить, какая из них применена, нельзя.
    std::vector<VerifiedFile> twice = onlyFirst;
    twice.push_back(makeVerifiedFile(kProbeFile, first.ruleBytes));
    const RuleSetVerification duplicated =
        verifyRuleSet(manifest, kGoodSignature, twice, kAppVersion, signedPolicy(signerOf(manifest, kGoodSignature)));
    require(!duplicated.ok(), "файл, переданный дважды, принят");
    require(problemsMention(duplicated, kProbeFile), "в отказе не назван продублированный файл: " + duplicated.summary());
    require(verdictFor(duplicated, kProbeFile) != nullptr, "нет вердикта по продублированному файлу");
}

TEST(rulesync_unparsableManifestIsReportedAsDataNotThrown) {
    // verifyRuleSet не бросает наружу: движок и UI получают отчёт (SPEC §6.4 —
    // отказ это данные). «Тихий отказ с записью в лог» начинается ровно здесь,
    // и настройки должны показать причину, а не упасть на экране.
    const RuleSetVerification report = verifyRuleSet("{не json", "ed25519:whatever", {}, kAppVersion,
                                                      signedPolicy(signerOf("ignored", kGoodSignature)));
    require(!report.ok(), "битый манифест принят");
    require(!report.manifestParsed, "битый манифест отмечен разобранным");
    require(!report.schemaOk, "схема не может быть в порядке при неразобранном манифесте");
    require(!report.problems.empty(), "в отчёте нет ни одной причины отказа");
    require(report.files.empty(), "проверялись файлы набора, которого не существует: " + describeChecks(report));
    require(report.summary() == "манифест не разобран", "неверный итог по неразобранному манифесту: " + report.summary());
    // summary() короткий и намеренно общий («манифест не разобран»): причина
    // обязана быть в problems с названием места — иначе в логе будет строка
    // без указания, что именно пришло из сети и где искать.
    require(mentions(report.problems.front(), "manifest.json") && mentions(report.problems.front(), "JSON"),
            "в причине отказа не названы источник и суть: " + report.problems.front());

    // Решение по такому отчёту — «оставить текущий набор», а не откат и не
    // попытка применить половину.
    const UpdateDecision decision = decideUpdate(report, appliedStatus());
    require(decision.action == UpdateAction::KeepCurrent,
            "на неразобранном кандидате принято неверное решение: " + decisionText(decision));
    require(!decision.appliesCandidate(), "неразобранный кандидат помечен как применяемый");
    require(mentions(decision.reason, "манифест"), "в решении не сказано, что манифест не разобран: " + decision.reason);

    // И бросающая обёртка обязана упасть на этом отчёте, а не пройти.
    requireRejected(
        [&] { verifyRuleSetOrThrow("{не json", "", {}, kAppVersion, unsignedPolicy()); },
        "verifyRuleSetOrThrow на неразобранном манифесте");
}

TEST(rulesync_hostileManifestShapesAreRefusedWithReason) {
    // Манифест приходит из сети и указывает движку, что читать. Каждая форма
    // ниже обязана быть отказом с указанием места: неизвестное поле, отсутствующее
    // поле, неверный тип, повтор пути, путь вне набора, не-hex хеш, чужая схема.
    const std::string hash = sha256Hex("проба");
    // Сборка строки начинается со std::string, а не со строкового литерала:
    // литерал + const char* — это арифметика указателей, а не конкатенация.
    const std::string one = R"json({"path":")json" + std::string(kProbeFile) + R"json(","sha256":")json" + hash +
                            R"json("})json";

    const std::vector<std::pair<std::string, std::string>> hostile = {
        {"не JSON", "{не json"},
        {"корень не объект", R"json([{"path":"a.json"}])json"},
        {"нет version", R"json({"schema":1,"files":[)json" + one + R"json(]})json"},
        {"пустая version", R"json({"schema":1,"version":"","files":[)json" + one + R"json(]})json"},
        {"нет files", R"json({"schema":1,"version":"v"})json"},
        {"files пуст", R"json({"schema":1,"version":"v","files":[]})json"},
        {"files не массив", R"json({"schema":1,"version":"v","files":{}})json"},
        {"неизвестное поле манифеста", R"json({"schema":1,"version":"v","files":[)json" + one + R"json(],"mirror":1})json"},
        {"чужая схема", R"json({"schema":2,"version":"v","files":[)json" + one + R"json(]})json"},
        {"schema и schemaVersion расходятся", R"json({"schema":1,"schemaVersion":2,"version":"v","files":[)json" + one +
                                        R"json(]})json"},
        {"путь вне набора", R"json({"schema":1,"version":"v","files":[{"path":"../evil.json","sha256":")json" + hash +
                            R"json("}]})json"},
        {"абсолютный путь", R"json({"schema":1,"version":"v","files":[{"path":"/etc/passwd","sha256":")json" + hash +
                           R"json("}]})json"},
        {"диск и двоеточие", R"json({"schema":1,"version":"v","files":[{"path":"C:/evil.json","sha256":")json" + hash +
                           R"json("}]})json"},
        {"двойной разделитель", R"json({"schema":1,"version":"v","files":[{"path":"sub//a.json","sha256":")json" + hash +
                              R"json("}]})json"},
        {"хвостовая точка", R"json({"schema":1,"version":"v","files":[{"path":"a.json.","sha256":")json" + hash +
                            R"json("}]})json"},
        {"пустой путь", R"json({"schema":1,"version":"v","files":[{"path":"","sha256":")json" + hash + R"json("}]})json"},
        {"неизвестное поле элемента", R"json({"schema":1,"version":"v","files":[{"path":"a.json","sha256":")json" + hash +
                                R"json(","mode":"strict"}]})json"},
        {"повтор пути", R"json({"schema":1,"version":"v","files":[)json" + one + "," + one + R"json(]})json"},
        {"size дробный", R"json({"schema":1,"version":"v","files":[{"path":"a.json","sha256":")json" + hash +
                          R"json(","size":1.5}]})json"},
        {"size отрицательный", R"json({"schema":1,"version":"v","files":[{"path":"a.json","sha256":")json" + hash +
                              R"json(","size":-1}]})json"},
        {"minAppVersion не строка", R"json({"schema":1,"version":"v","minAppVersion":1,"files":[)json" + one + R"json(]})json"},
        {"minAppVersion не числовая", R"json({"schema":1,"version":"v","minAppVersion":"1.0.0-бета","files":[)json" + one +
                                       R"json(]})json"},
    };

    for (const auto& [what, text] : hostile) {
        requireSyncRejection([&] { (void)parseRuleSetManifest(text); }, "манифест «" + what + "» принят");
        // Тот же враждебный манифест обязан быть отказом и на уровне отчёта:
        // verifyRuleSet не имеет права на исключение наружу.
        const RuleSetVerification report = verifyRuleSet(text, "", {}, kAppVersion, unsignedPolicy());
        require(!report.ok(), "враждебный манифест «" + what + "» прошёл проверку");
        require(!report.manifestParsed, "враждебный манифест «" + what + "» отмечен разобранным");
        require(!report.problems.empty(), "нет причины отказа для манифеста «" + what + "»");
    }
}

TEST(rulesync_minAppVersionGateBlocksSetForNewerApp) {
    // Шаг 2 §9.2 п.2: набор, требующий более нового приложения, не применяется.
    // Это не «предупреждение» — движок удаляет по этим правилам.
    const ProbeSet set = probeSet();
    const std::string manifest = buildManifest({{kProbeFile, sha256Hex(set.ruleBytes)}}, kSetVersion, "2.0.0");
    const VerificationPolicy policy = signedPolicy(signerOf(manifest, kGoodSignature));

    const RuleSetVerification tooNew = verifyRuleSet(manifest, kGoodSignature, set.files, kAppVersion, policy);
    require(!tooNew.ok(), "набор для более нового приложения применён к 1.0.0");
    require(!tooNew.versionOk, "совместимость помечена в порядке при minAppVersion=2.0.0");
    require(mentions(tooNew.versionDetail, "2.0.0"), "в отчёте не названа требуемая версия: " + tooNew.versionDetail);
    require(problemsMention(tooNew, "2.0.0"), "причина по версии не попала в problems: " + tooNew.summary());
    require(tooNew.signatureOk, "подпись должна проверяться и при отказе по версии — порядок §9.2 п.2");

    // Ровно та же версия — совместимо.
    const std::string exact = buildManifest({{kProbeFile, sha256Hex(set.ruleBytes)}}, kSetVersion, kAppVersion);
    const RuleSetVerification equal =
        verifyRuleSet(exact, kGoodSignature, set.files, kAppVersion, signedPolicy(signerOf(exact, kGoodSignature)));
    require(equal.ok(), "набор с равной версией отвергнут: " + equal.summary() + " [" + describeChecks(equal) + "]");

    // minAppVersion не объявлен — ограничений нет, даже если версия приложения
    // неизвестна (запуск до инициализации окружения).
    const std::string withoutMin = buildManifest({{kProbeFile, sha256Hex(set.ruleBytes)}}, kSetVersion, "");
    const RuleSetVerification noGate =
        verifyRuleSet(withoutMin, kGoodSignature, set.files, "", signedPolicy(signerOf(withoutMin, kGoodSignature)));
    require(noGate.ok(), "набор без minAppVersion отвергнут при неизвестной версии приложения: " + noGate.summary());

    // Неизвестная версия приложения при заявленном ограничении — не «проходит
    // на всякий случай», а отказ с записью в отчёт.
    const RuleSetVerification unknownApp =
        verifyRuleSet(manifest, kGoodSignature, set.files, "", policy);
    require(!unknownApp.ok(), "набор применён при неизвестной версии приложения и заявленном minAppVersion");
    require(mentions(unknownApp.versionDetail, "неизвестна"),
            "в отчёте не сказано, что версия приложения неизвестна: " + unknownApp.versionDetail);
}

TEST(rulesync_versionComparisonIsNumericNotLexicographic) {
    // Сравнение версий обязано быть числовым: «1.10» новее «1.9», а лексикографически
    // наоборот, и тогда набор, который автор считает новым, молча откатился бы.
    require(compareVersions("1.2", "1.2.0") == 0, "«1.2» и «1.2.0» должны совпадать");
    require(compareVersions("1.10", "1.9") > 0, "«1.10» не новей «1.9»");
    require(compareVersions("2.0", "10.0") < 0, "«2.0» не старше «10.0»");
    require(compareVersions("2026.02.1", "2026.02.1") == 0, "одинаковые версии не равны");
    requireSyncRejection([] { (void)compareVersions("1.0.0-бета", "1.0.0"); }, "версия с суффиксом принята");
    requireSyncRejection([] { (void)compareVersions("1.0.", "1.0"); }, "версия с хвостовой точкой принята");
}

// ===========================================================================
// 3) Разбор набора после проверки (SPEC §9.2 п.3)
// ===========================================================================

TEST(rulesync_verifiedSetIsParsedByRuleLoader) {
    // Шаг 3 §9.2: после проверки целостности файлы разбираются парсером правил.
    // Набор, который не разбирается, применять нельзя, даже если подпись и
    // хеши в порядке.
    const std::string rule = ruleFile("temp.user.probe", kSetVersion);
    const std::string manifest = buildManifest({{kProbeFile, sha256Hex(rule), true, rule.size()}});
    const RuleSetVerification report =
        verifyRuleSet(manifest, kGoodSignature, {makeVerifiedFile(kProbeFile, rule)}, kAppVersion,
                      signedPolicy(signerOf(manifest, kGoodSignature)));
    require(report.ok(), "эталонный набор не прошёл проверку: " + report.summary() + " [" + describeChecks(report) + "]");

    bool unresolved = true;
    const std::vector<std::pair<std::string, std::string>> contents = {{kProbeFile, rule}};
    const RuleSet applied = loadVerifiedRuleSet(report, contents, kEnvDump, &unresolved);
    require(!unresolved, "правило не раскрыло переменные окружения — тест передал неполный env");
    require(applied.size() == 1, "применённый набор содержит " + std::to_string(applied.size()) + " правил вместо одного");
    require(applied.schemaVersion == report.manifest.schemaVersion, "разбор вернул чужую версию схемы");
    require(applied.version == kSetVersion, "разбор вернул чужую версию набора: " + applied.version);
    const Rule* loaded = applied.byId("temp.user.probe");
    require(loaded != nullptr, "применённый набор не содержит правила temp.user.probe");
    require(!loaded->resolvedLocator.empty(), "locator правила не раскрыт: " + loaded->resolvedLocator);
    require(loaded->safety == SafetyLevel::Safe, "уровень безопасности правила потерян при разборе");
}

TEST(rulesync_ruleFileWithUnknownFieldIsRefusedAfterVerification) {
    // Подпись и хеш сходятся, а правило содержит поле, которого ядро не знает.
    // Такой набор применять нельзя: движок удаляет по правилам, и неизвестное
    // поле — это неизвестное намерение автора (SPEC §9.2 п.3).
    const std::string rule =
        R"json({"schemaVersion":1,"version":")json" + std::string(kSetVersion) +
        R"json(","rules":[{"id":"temp.user.probe","category":"temp.user","safety":"safe","locator":"%TEMP%/*",)json"
        R"json("minAgeDays":1,"deleteRoot":true,"title":{"ru":"тест","en":"test"}}]})json";
    const std::string manifest = buildManifest({{kProbeFile, sha256Hex(rule), true, rule.size()}});
    const RuleSetVerification report =
        verifyRuleSet(manifest, kGoodSignature, {makeVerifiedFile(kProbeFile, rule)}, kAppVersion,
                      signedPolicy(signerOf(manifest, kGoodSignature)));
    require(report.ok(), "целостность набора не подтверждена — тест проверяет не разбор: " + report.summary());

    const std::vector<std::pair<std::string, std::string>> contents = {{kProbeFile, rule}};
    const std::string rejection = rejectionOf([&] { (void)loadVerifiedRuleSet(report, contents, kEnvDump); });
    require(!rejection.empty(), "файл правил с неизвестным полем принят парсером");
    require(mentions(rejection, "deleteRoot"), "в отказе не названо неизвестное поле: " + rejection);
}

TEST(rulesync_loadVerifiedRuleSetRefusesUnverifiedAndIncompleteInput) {
    const std::string rule = ruleFile("temp.user.probe", kSetVersion);
    const std::string manifest = buildManifest({{kProbeFile, sha256Hex(rule), true, rule.size()}});
    const RuleSetVerification good =
        verifyRuleSet(manifest, kGoodSignature, {makeVerifiedFile(kProbeFile, rule)}, kAppVersion,
                      signedPolicy(signerOf(manifest, kGoodSignature)));
    require(good.ok(), "эталонный набор не прошёл проверку: " + good.summary());
    const std::vector<std::pair<std::string, std::string>> contents = {{kProbeFile, rule}};

    // Набор, не прошедший проверку, разбирать нельзя: это единственная точка,
    // где движок берёт правила, и обойти её нечем.
    const RuleSetVerification refused =
        verifyRuleSet(manifest, "ed25519:wrong", {makeVerifiedFile(kProbeFile, rule)}, kAppVersion,
                      signedPolicy(signerOf(manifest, kGoodSignature)));
    requireSyncRejection([&] { (void)loadVerifiedRuleSet(refused, contents, kEnvDump); },
                         "набор с чужой подписью разобран парсером правил");

    // Файл, не перечисленный в манифесте, — отказ даже при верной подписи.
    const std::vector<std::pair<std::string, std::string>> withExtra = {
        {kProbeFile, rule}, {"wer.json", ruleFile("wer.probe", kSetVersion)}};
    const std::string extraRejection = rejectionOf([&] { (void)loadVerifiedRuleSet(good, withExtra, kEnvDump); });
    require(!extraRejection.empty(), "файл вне манифеста принят на разбор");
    require(mentions(extraRejection, "wer.json"), "в отказе не назван лишний файл: " + extraRejection);

    // Файл манифеста не передан — набор неполон, разбирать нечего.
    const std::vector<std::pair<std::string, std::string>> withoutFile;
    requireSyncRejection([&] { (void)loadVerifiedRuleSet(good, withoutFile, kEnvDump); },
                         "набор без содержимого файла разобран");

    // Версия в файле правил не совпадает с манифестом: подписан был другой набор.
    const std::string other = ruleFile("temp.user.probe", "2026.02.9-probe");
    const std::string otherManifest = buildManifest({{kProbeFile, sha256Hex(other), true, other.size()}});
    const RuleSetVerification mismatched =
        verifyRuleSet(otherManifest, kGoodSignature, {makeVerifiedFile(kProbeFile, other)}, kAppVersion,
                      signedPolicy(signerOf(otherManifest, kGoodSignature)));
    const std::string versionRejection =
        rejectionOf([&] { (void)loadVerifiedRuleSet(mismatched, {{kProbeFile, other}}, kEnvDump); });
    require(!versionRejection.empty(), "набор с чужой версией в файле правил принят");
    require(mentions(versionRejection, kSetVersion) && mentions(versionRejection, "2026.02.9-probe"),
            "в отказе не названы обе версии: " + versionRejection);
}

// ===========================================================================
// 4) Откат (SPEC §9.2 п.3-4)
// ===========================================================================

TEST(rulesync_verifiedCandidateIsAppliedAndStateRecordsIt) {
    const ProbeSet set = probeSet();
    const RuleSetVerification report =
        verifyRuleSet(set.manifest, kGoodSignature, set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));
    require(report.ok(), "эталонный набор не прошёл проверку: " + report.summary());

    const RuleSetStatus before = appliedStatus(kPreviousVersion);
    const RuleSetStatus after = withAppliedCandidate(before, report, kNow + 60);

    require(after.version == kSetVersion, "после применения активна не новая версия: " + after.version);
    require(after.verifiedVersion == kSetVersion, "последняя проверенная версия не обновилась: " + after.verifiedVersion);
    require(after.installedEpochSeconds == kNow + 60, "дата установки не записана: " +
                                                        std::to_string(after.installedEpochSeconds));
    require(after.lastCheckEpochSeconds == kNow + 60, "время последней проверки не записано");
    require(after.lastCheckOk, "после успешного применения проверка помечена неуспешной");
    require(after.verified, "применённый набор помечен неподтверждённым");
    require(!after.embedded, "скачанный набор помечен встроенным");
    require(after.lastResult == report.summary(), "в состояние записан не тот итог: " + after.lastResult);
    require(after.autoUpdateEnabled, "автообновление выключилось само по себе");

    // Исходное состояние не мутируется: платформа держит его в JSON и читает
    // снова (SPEC §6.4 — результаты не мутируются после публикации).
    require(before.version == kPreviousVersion, "исходное состояние изменилось при применении: " + before.version);
    require(before.verifiedVersion == kPreviousVersion, "исходное состояние изменилось при применении");
    require(before.installedEpochSeconds == kNow, "исходное состояние изменилось при применении");
}

TEST(rulesync_unverifiedCandidateIsNeverApplied) {
    const ProbeSet set = probeSet();
    const RuleSetVerification refused =
        verifyRuleSet(set.manifest, "ed25519:wrong", set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));
    require(!refused.ok(), "эталонный отказ не отказал — тест бессмыслен");

    requireSyncRejection([&] { (void)withAppliedCandidate(appliedStatus(), refused, kNow); },
                         "непроверенный набор применён");
    requireSyncRejection([&] { (void)withAppliedCandidate(RuleSetStatus{}, refused, kNow); },
                         "непроверенный набор применен к состоянию по умолчанию");
}

TEST(rulesync_rejectedCandidateKeepsCurrentSetInsteadOfRollingBack) {
    // Кандидат проверяется, пока не применён, поэтому его отказ — это «оставить
    // как есть», а не откат: откатываться не от чего (SPEC §9.2 п.3-4).
    const ProbeSet set = probeSet();
    const RuleSetVerification refused =
        verifyRuleSet(set.manifest, "ed25519:wrong", set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));
    const RuleSetStatus current = appliedStatus();

    const UpdateDecision decision = decideUpdate(refused, current);
    require(decision.action == UpdateAction::KeepCurrent,
            "на отказ кандидата принято решение " + decisionText(decision) + ", а не KeepCurrent");
    require(!decision.appliesCandidate(), "отвергнутый кандидат помечен применяемым");
    require(mentions(decision.reason, "текущем наборе"), "в решении не сказано, что остаёмся на текущем наборе: " +
                                                          decision.reason);
    require(mentions(decision.reason, refused.summary()) || mentions(decision.reason, "отказ"),
            "в решении нет ссылки на причину отказа: " + decision.reason);

    // Состояние после неудачной проверки: версия на месте (офлайн-режим), но
    // видно, что последняя проверка не удалась (SPEC §9.2 п.5 — результат
    // последней проверки показывается в настройках).
    const RuleSetStatus after = withFailedCheck(current, refused, kNow + 60);
    require(after.version == kSetVersion, "после отказа потеряна активная версия: " + after.version);
    require(after.verified, "подтверждённый набор помечен неподтверждённым после отказа кандидата");
    require(!after.embedded, "подтверждённый набор помечен встроенным после отказа кандидата");
    require(!after.lastCheckOk, "неудачная проверка помечена успешной");
    require(after.lastCheckEpochSeconds == kNow + 60, "время неудачной проверки не записано");
    require(after.lastResult == refused.summary(), "в состояние записан не тот итог: " + after.lastResult);
    require(after.verifiedVersion == kSetVersion, "последняя проверенная версия потеряна");
}

TEST(rulesync_rollsBackToLastGoodOnlyWhenCurrentSetIsNotVerified) {
    // Откат — случай, когда и текущий набор не подтверждён: он не проверен, и
    // откатываться есть на что, только если есть предыдущий проверенный.
    const ProbeSet set = probeSet();
    const RuleSetVerification refused =
        verifyRuleSet(set.manifest, "ed25519:wrong", set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));

    RuleSetStatus broken = unverifiedStatus(kSetVersion);
    broken.verifiedVersion = kPreviousVersion;
    const UpdateDecision rollback = decideUpdate(refused, broken);
    require(rollback.action == UpdateAction::RollbackToLastGood,
            "на неподтверждённый текущий набор принято решение " + decisionText(rollback));
    require(!rollback.appliesCandidate(), "откат помечен как применение кандидата");
    require(mentions(rollback.reason, kPreviousVersion), "в решении не назван набор, на который откатываемся: " +
                                                             rollback.reason);

    // Не подтверждён ни один набор — откатываться не на что, работаем на
    // встроенном (SPEC §9.2 п.4: приложение никогда не остаётся без правил).
    RuleSetStatus never = unverifiedStatus(kSetVersion);
    never.verifiedVersion.clear();
    const UpdateDecision embedded = decideUpdate(refused, never);
    require(embedded.action == UpdateAction::UseEmbeddedSet,
            "при отсутствии проверенных наборов принято решение " + decisionText(embedded));
    require(mentions(embedded.reason, "встроенн"), "в решении не сказано про встроенный набор: " + embedded.reason);

    // Тот же отчёт, но текущий набор подтверждён: никакого отката, KeepCurrent.
    const UpdateDecision keep = decideUpdate(refused, appliedStatus());
    require(keep.action == UpdateAction::KeepCurrent, "подтверждённый набор откатили: " + decisionText(keep));
}

TEST(rulesync_failedCheckFallsBackToEmbeddedWhenNothingIsVerified) {
    // Ни текущий набор, ни кандидат не подтверждены: остаётся встроенный — он цел
    // по построению. Это и есть требование «приложение никогда не остаётся без
    // набора правил» (SPEC §9.2 п.4).
    const ProbeSet set = probeSet();
    const RuleSetVerification refused =
        verifyRuleSet(set.manifest, "ed25519:wrong", set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));

    RuleSetStatus broken = unverifiedStatus(kSetVersion);
    broken.verifiedVersion = kPreviousVersion;
    const RuleSetStatus after = withFailedCheck(broken, refused, kNow + 60);

    require(after.embedded, "после отказа неподтверждённого набора активен не встроенный");
    require(after.version.empty(), "после отката осталась неприменённая версия: " + after.version);
    require(after.verified, "встроенный набор помечен неподтверждённым");
    require(!after.lastCheckOk, "неудачная проверка помечена успешной");
    require(after.lastResult == refused.summary(), "в состояние записан не тот итог: " + after.lastResult);
    // Знание о последнем хорошем наборе не теряется: оно нужно для отката в
    // следующий раз и для показа в настройках.
    require(after.verifiedVersion == kPreviousVersion, "последняя проверенная версия стёрта откатом");
    require(after.autoUpdateEnabled, "автообновление выключено само по себе после отката");

    const StartupChoice startup = chooseStartupRuleSet(after);
    require(startup.useEmbedded, "после отката приложение стартует не на встроенном наборе");
    require(startup.reason.find("встроенн") != std::string::npos, "в выборе набора не сказано про встроенный: " +
                                                                     startup.reason);
}

// ===========================================================================
// 5) Офлайн-режим и сброс (SPEC §9.2 «Офлайн-режим и сброс», п.1 и п.5)
// ===========================================================================

TEST(rulesync_updateCheckHappensAtMostOncePerDay) {
    // SPEC §9.2 п.1: при запуске, но не чаще раза в 24 часа. Часы пошли назад —
    // проверку разрешаем, иначе автообновление встало бы навсегда.
    RuleSetStatus fresh;  // состояние по умолчанию: встроенный набор, автообновление включено
    require(fresh.shouldCheck(kNow), "после первой установки проверка не запланирована");
    require(fresh.shouldCheck(0), "с нулевым временем проверка не разрешена");

    RuleSetStatus checked = appliedStatus();
    require(!checked.shouldCheck(kNow), "проверка повторилась в ту же секунду");
    require(!checked.shouldCheck(kNow + kUpdateCheckIntervalSeconds - 1),
            "проверка повторилась раньше суток");
    require(checked.shouldCheck(kNow + kUpdateCheckIntervalSeconds), "проверка не разрешена через сутки");
    require(checked.shouldCheck(kNow + 2 * kUpdateCheckIntervalSeconds), "проверка не разрешена через двое суток");
    require(checked.shouldCheck(kNow - 10), "часы ушли назад, а проверка не разрешена");

    // Выключенное автообновление — не проверяем вовсе, даже по расписанию.
    RuleSetStatus manual = appliedStatus();
    manual.autoUpdateEnabled = false;
    require(!manual.shouldCheck(kNow + 10 * kUpdateCheckIntervalSeconds),
            "при выключенном автообновлении проверка всё равно разрешена");

    // Неудачная проверка тоже сдвигает расписание: иначе битый канал опрашивался
    // бы на каждом запуске.
    const ProbeSet set = probeSet();
    const RuleSetVerification refused =
        verifyRuleSet(set.manifest, "ed25519:wrong", set.files, kAppVersion,
                      signedPolicy(signerOf(set.manifest, kGoodSignature)));
    const RuleSetStatus afterFailure = withFailedCheck(appliedStatus(), refused, kNow);
    require(!afterFailure.shouldCheck(kNow), "после неудачной проверки следующая разрешена немедленно");
    require(afterFailure.shouldCheck(kNow + kUpdateCheckIntervalSeconds), "после неудачной проверки следующая не "
                                                                        "разрешена через сутки");
}

TEST(rulesync_startupNeverLeavesAppWithoutRuleSet) {
    // SPEC §9.2 п.4: «приложение никогда не остаётся без набора правил».
    // Инвариант проверяется на всех достижимых состояниях сразу: для каждого
    // старт либо берёт встроенный набор, либо называет подтверждённую версию.
    const std::vector<std::pair<std::string, RuleSetStatus>> states = {
        {"свежая установка", RuleSetStatus{}},
        {"подтверждённый скачанный набор", appliedStatus()},
        {"неподтверждённый скачанный набор", unverifiedStatus()},
        {"после отката на встроенный", [] {
             RuleSetStatus status = unverifiedStatus();
             status.verifiedVersion.clear();
             return status;
         }()},
        {"после кнопки «вернуть встроенный набор»", resetToEmbeddedSet(appliedStatus(), kNow)},
    };

    for (const auto& [what, status] : states) {
        const StartupChoice choice = chooseStartupRuleSet(status);
        if (choice.useEmbedded) {
            require(choice.version.empty(), "встроенный набор выбран, но названа версия: " + choice.version);
        } else {
            require(!choice.version.empty(), "выбран скачанный набор без версии: " + what);
        }
        require(!choice.reason.empty(), "выбор набора без объяснения: " + what);
    }

    // Явные ожидания по двум состояниям, которые чаще всего и ломаются.
    const StartupChoice embedded = chooseStartupRuleSet(RuleSetStatus{});
    require(embedded.useEmbedded, "свежая установка стартует не на встроенном наборе");
    const StartupChoice downloaded = chooseStartupRuleSet(appliedStatus());
    require(!downloaded.useEmbedded, "подтверждённый скачанный набор заменён встроенным");
    require(downloaded.version == kSetVersion, "назван не тот набор: " + downloaded.version);
    const StartupChoice unverified = chooseStartupRuleSet(unverifiedStatus());
    require(unverified.useEmbedded, "неподтверждённый набор выбран для работы");
    require(unverified.version.empty(), "неподтверждённый набор назван по версии: " + unverified.version);
}

TEST(rulesync_resetToEmbeddedDisablesAutoUpdate) {
    // Обязательная кнопка настроек (SPEC §9.2): удаляет скачанные правила,
    // оставляет встроенные и отключает автообновление до следующего явного
    // включения.
    const RuleSetStatus before = appliedStatus();
    const RuleSetStatus after = resetToEmbeddedSet(before, kNow + 120);

    require(after.embedded, "после сброса набор не встроенный");
    require(after.version.empty(), "после сброса осталась активная версия: " + after.version);
    require(after.verified, "встроенный набор помечен неподтверждённым");
    require(!after.autoUpdateEnabled, "автообновление не отключено кнопкой «вернуть встроенный набор»");
    require(!after.shouldCheck(kNow + 120), "после сброса автопроверка всё ещё разрешена");
    require(after.lastCheckEpochSeconds == kNow + 120, "сброс не записал время действия");
    require(!after.lastResult.empty(), "сброс не оставил human-readable итога в настройках");
    require(mentions(after.lastResult, "встроенн"), "итог сброса не говорит про встроенный набор: " + after.lastResult);

    // Знание о последнем проверенном наборе сохраняется: сброс не «забывает»
    // версию, он лишь перестаёт её применять.
    require(after.verifiedVersion == kSetVersion, "сброс стёр сведения о последнем проверенном наборе");

    // Исходное состояние не тронуто.
    require(before.version == kSetVersion, "исходное состояние изменилось при сбросе");
    require(!before.embedded, "исходное состояние изменилось при сбросе");
    require(before.autoUpdateEnabled, "исходное состояние изменилось при сбросе");
}

TEST(rulesync_statusSurvivesRestartAndBrokenStateIsNotFatal) {
    // Состояние переживает перезапуск (state.json в
    // %LOCALAPPDATA%\MrProper\rules): полный круг сериализация → разбор должен
    // вернуть то же самое, иначе офлайн-режим забывает набор при первом же
    // перезапуске.
    // Состояние намеренно не const: тест задаёт собственный итог проверки
    // (в appliedStatus он короче) и проверяет, что он пережил перезапуск
    // вместе с остальными полями.
    RuleSetStatus original = appliedStatus();
    original.lastResult = "набор 2026.02.1-probe: проверен, подпись верна";
    const RuleSetStatus restored = parseRuleSetStatus(serializeRuleSetStatus(original));

    require(restored.version == original.version, "версия набора не пережила перезапуск: " + restored.version);
    require(restored.verifiedVersion == original.verifiedVersion, "последняя проверенная версия потеряна");
    require(restored.installedEpochSeconds == original.installedEpochSeconds, "дата установки потеряна");
    require(restored.lastCheckEpochSeconds == original.lastCheckEpochSeconds, "время проверки потеряно");
    require(restored.lastResult == original.lastResult, "итог проверки потерян: " + restored.lastResult);
    require(restored.lastCheckOk == original.lastCheckOk, "результат проверки потерян");
    require(restored.autoUpdateEnabled == original.autoUpdateEnabled, "состояние автообновления потеряно");
    require(restored.verified == original.verified, "подтверждённость набора потеряна");
    require(restored.embedded == original.embedded, "признак встроенного набора потерян");
    require(restored.shouldCheck(original.lastCheckEpochSeconds + kUpdateCheckIntervalSeconds),
            "после перезапуска расписание проверок сбито");

    // Битое состояние — не ошибка: приложение обязано стартовать на встроенном
    // наборе, а не падать на экране настроек.
    const std::vector<std::pair<std::string, std::string>> broken = {
        {"пустой файл", ""},
        {"не JSON", "{не json"},
        {"не объект", "[]"},
        {"обрезанный объект", "{\"version\":\"2026.02.1-probe\""},
    };
    for (const auto& [what, text] : broken) {
        const RuleSetStatus status = parseRuleSetStatus(text);
        require(status.embedded, "битое состояние «" + what + "» оставило приложение без встроенного набора");
        require(status.version.empty(), "битое состояние «" + what + "» выдало версию: " + status.version);
        require(status.verified, "битое состояние «" + what + "» оставило набор неподтверждённым");
        require(status.autoUpdateEnabled, "битое состояние «" + what + "» выключило автообновление");
    }

    // Неизвестные поля игнорируются: state.json дописывается новыми версиями
    // приложения, и старая не должна его отвергать.
    const std::string extended =
        R"json({"version":"2026.02.1-probe","verifiedVersion":"2026.02.1-probe",)json"
        R"json("installed":1700000000,"lastCheck":1700000000,"lastCheckOk":true,)json"
        R"json("autoUpdate":true,"verified":true,"embedded":false,"mirror":"sha256:..."})json";
    const RuleSetStatus tolerant = parseRuleSetStatus(extended);
    require(tolerant.version == kSetVersion, "расширенное состояние не прочитано: " + tolerant.version);
    require(!tolerant.embedded, "расширенное состояние прочитано неверно");
    require(tolerant.lastCheckOk, "расширенное состояние прочитано неверно");
}

TEST(rulesync_offlineLifecycleKeepsAppSuppliedWithRuleSet) {
    // Сквозной сценарий §9.2 п.1-5 и §12: подписанный набор применяется;
    // неподписанный отвергается с записью в лог, и приложение продолжает
    // работать на последнем проверенном наборе (офлайн); если и текущий набор
    // не подтверждён — остаётся встроенный; кнопка сброса отключает
    // автообновление. Ни на одном шаге у приложения не остаётся набора.
    const ProbeSet set = probeSet();
    const VerificationPolicy signedOk = signedPolicy(signerOf(set.manifest, kGoodSignature));
    const RuleSetStatus fresh;  // встроенный набор, автообновление включено

    // 1) Первая проверка разрешена, подписанный набор применяется.
    require(fresh.shouldCheck(kNow), "первая проверка не разрешена");
    const RuleSetVerification good =
        verifyRuleSet(set.manifest, kGoodSignature, set.files, kAppVersion, signedOk);
    require(good.ok(), "эталонный набор не прошёл проверку: " + good.summary());
    require(decideUpdate(good, fresh).appliesCandidate(), "проверенный кандидат не признан применяемым");
    const RuleSetStatus installed = withAppliedCandidate(fresh, good, kNow);
    require(installed.version == kSetVersion, "набор не применён");

    // 2) Сутки прошло, сеть вернула подделанный набор: отказ с записью в лог,
    //    работа продолжается на последнем проверенном (офлайн-режим).
    require(installed.shouldCheck(kNow + kUpdateCheckIntervalSeconds), "через сутки проверка не разрешена");
    const RuleSetVerification forged =
        verifyRuleSet(set.manifest, "ed25519:forged", set.files, kAppVersion, signedOk);
    require(!forged.ok(), "подделанный набор принят");
    const UpdateDecision forgedDecision = decideUpdate(forged, installed);
    require(forgedDecision.action == UpdateAction::KeepCurrent,
            "на подделанный кандидат принято решение " + decisionText(forgedDecision));
    const RuleSetStatus afterForged = withFailedCheck(installed, forged, kNow + kUpdateCheckIntervalSeconds);
    require(afterForged.version == kSetVersion, "офлайн-режим потерял набор после отказа: " + afterForged.version);
    require(!afterForged.embedded, "офлайн-режим вернул встроенный набор вместо проверенного");
    require(!afterForged.lastCheckOk, "отказ не записан в состояние");
    require(mentions(afterForged.lastResult, forged.summary()) || mentions(afterForged.lastResult, "отказ"),
            "в состоянии нет записи об отказе: " + afterForged.lastResult);
    const StartupChoice offline = chooseStartupRuleSet(afterForged);
    require(!offline.useEmbedded && offline.version == kSetVersion,
            "после отказа приложение стартует не на последнем проверенном наборе");

    // 3) Текущий набор тоже не подтверждён (подмена каталогов не дошла до
    //    конца) и сеть снова прислала отвергнутый набор: остаётся встроенный.
    RuleSetStatus brokenCurrent = unverifiedStatus(kSetVersion);
    brokenCurrent.lastCheckEpochSeconds = kNow + kUpdateCheckIntervalSeconds;
    const UpdateDecision lastResort = decideUpdate(forged, brokenCurrent);
    require(lastResort.action == UpdateAction::RollbackToLastGood,
            "при неподтверждённом текущем наборе принято решение " + decisionText(lastResort));
    const RuleSetStatus afterRollback = withFailedCheck(brokenCurrent, forged, kNow + 2 * kUpdateCheckIntervalSeconds);
    const StartupChoice afterBreak = chooseStartupRuleSet(afterRollback);
    require(afterBreak.useEmbedded, "после отката приложение осталось без набора правил");
    require(afterRollback.verified, "встроенный набор помечен неподтверждённым");

    // 4) Кнопка «вернуть встроенный набор»: автообновление выключено, и
    //    приложение остаётся работоспособным.
    const RuleSetStatus reset = resetToEmbeddedSet(afterRollback, kNow + 3 * kUpdateCheckIntervalSeconds);
    require(reset.embedded && reset.version.empty(), "после сброса активен не встроенный набор");
    require(!reset.shouldCheck(kNow + 4 * kUpdateCheckIntervalSeconds), "после сброса автопроверка не выключена");
    require(chooseStartupRuleSet(reset).useEmbedded, "после сброса стартуем не на встроенном наборе");
    require(parseRuleSetStatus(serializeRuleSetStatus(reset)).embedded, "состояние сброса не пережило перезапуск");
}
