// Юнит-тесты переносимого ядра: json, glob, units, rules, scoring.
// Собирается и запускается на любом хосте:
//   g++ -std=c++20 -Wall -Wextra -O1 -Isrc/core -Itests tests/unit/*.cpp src/core/*.cpp -o /tmp/mrp_tests
#include "harness.hpp"

#include "glob.hpp"
#include "json.hpp"
#include "rules.hpp"
#include "scoring.hpp"
#include "units.hpp"

using namespace mrproper::core;
using mrproper::json::Value;

// ---------------------------------------------------------------- json

TEST(json_parcesScalars) {
    CHECK_EQ(mrproper::json::parse("null").type(), mrproper::json::Type::Null);
    CHECK(mrproper::json::parse("true").asBool());
    CHECK_EQ(mrproper::json::parse("false").asBool(), false);
    CHECK_EQ(mrproper::json::parse("42").asNumber(), 42.0);
    CHECK_EQ(mrproper::json::parse("-1.5e2").asNumber(), -150.0);
    CHECK_EQ(mrproper::json::parse("\"привет\"").asString(), std::string("привет"));
}

TEST(json_parsesNestedAndKeepsKeyOrder) {
    const Value v = mrproper::json::parse(R"({"b":1,"a":[true,null,{"x":"y"}]})");
    CHECK(v.isObject());
    const auto& members = v.members();
    CHECK_EQ(members.size(), static_cast<std::size_t>(2));
    CHECK_EQ(members[0].first, std::string("b"));
    CHECK_EQ(members[1].first, std::string("a"));
    CHECK(v.find("a")->isArray());
    CHECK_EQ(v.find("a")->items().size(), static_cast<std::size_t>(3));
    CHECK(v.find("zzz") == nullptr);
}

TEST(json_decodesEscapes) {
    const Value v = mrproper::json::parse(R"("a\nbAя\t\"")");
    CHECK_EQ(v.asString(), std::string("a\nb\x41\xD1\x8F\t\""));
}

TEST(json_rejectsGarbage) {
    CHECK_THROWS(mrproper::json::parse("{"));
    CHECK_THROWS(mrproper::json::parse("{\"a\":}"));
    CHECK_THROWS(mrproper::json::parse("[1,2"));
    CHECK_THROWS(mrproper::json::parse("tru"));
    CHECK_THROWS(mrproper::json::parse("1 2"));
    CHECK_THROWS(mrproper::json::parse("\"незакрытая"));
}

TEST(json_rejectsWrongTypeOnAccess) {
    const Value v = mrproper::json::parse(R"({"n":1})");
    CHECK_THROWS(v.find("n")->asString());
    CHECK_THROWS(v.items());
    CHECK_THROWS(v.require("missing"));
}

TEST(json_dumpIsDeterministic) {
    const Value v = mrproper::json::parse(R"({"b":2,"a":[1,2]})");
    CHECK_EQ(v.dump(), std::string(R"({"b":2,"a":[1,2]})"));
    const Value again = mrproper::json::parse(v.dump(2));
    CHECK_EQ(again.dump(), v.dump());
}

TEST(json_roundTripsUnicodeAndControlChars) {
    Value v = Value::object({{"текст", Value("я\\\"")}, {"n", Value(1.5)}});
    const Value back = mrproper::json::parse(v.dump());
    CHECK_EQ(back.require("текст").asString(), std::string("я\\\""));
    CHECK_EQ(back.require("n").asNumber(), 1.5);
}

// ---------------------------------------------------------------- glob

TEST(glob_starDoesNotCrossSeparator) {
    CHECK(matchPath("C:/Temp/*", "C:/Temp/file.tmp"));
    CHECK(!matchPath("C:/Temp/*", "C:/Temp/sub/file.tmp"));
    CHECK(matchPath("C:/Temp/**", "C:/Temp/sub/deep/file.tmp"));
}

// Регрессия D-52: почти все локаторы правил кончаются на «**», а повторная
// сверка с локатором на фазе C отвергала корень СВОЕГО ЖЕ правила. Причина —
// «C:\x\rule\**» не совпадал с «C:\x\rule»: остаток пути пуст, а хвост
// шаблона состоял из разделителя и «**». Цена была не косметическая: 69 из 234
// операций плана (29,5 %) не могли выполниться.
TEST(glob_doubleStarMatchesEmptyRemainder) {
    CHECK(matchPath("C:/x/rule/**", "C:/x/rule"));
    CHECK(matchPath("C:/x/rule/**", "C:/x/rule/a"));
    CHECK(matchPath("C:/x/rule/**", "C:/x/rule/a/b"));
    // Одиночный «*» по-прежнему НЕ совпадает с пустым остатком: это разные вещи.
    CHECK(!matchPath("C:/x/rule/*", "C:/x/rule"));
    // Соседний каталог не должен подхватываться.
    CHECK(!matchPath("C:/x/rule/**", "C:/x/other"));
    // Две группы «**» подряд в хвосте — уже «что-то», а не пустота.
    CHECK(!matchPath("C:/x/**/**", "C:/x"));
    // Обычные случаи не сломались.
    CHECK(matchPath("C:/x/rule/*", "C:/x/rule/a"));
    CHECK(matchPath("C:/x/rule", "C:/x/rule"));
}

TEST(glob_doubleStarMatchesZeroSegments) {
    CHECK(matchPath("C:/a/**/b", "C:/a/b"));
    CHECK(matchPath("C:/a/**/b", "C:/a/x/b"));
    CHECK(matchPath("C:/a/**/b", "C:/a/x/y/b"));
    CHECK(!matchPath("C:/a/**/b", "C:/a/x/b/c"));
}

TEST(glob_questionMarkAndClasses) {
    CHECK(matchPath("file?.tmp", "file1.tmp"));
    CHECK(!matchPath("file?.tmp", "file12.tmp"));
    CHECK(!matchPath("file?.tmp", "dir/file1.tmp"));
    CHECK(matchPath("log[0-9].txt", "log7.txt"));
    CHECK(!matchPath("log[0-9].txt", "logx.txt"));
    CHECK(!matchPath("log[!0-9].txt", "log7.txt"));
    CHECK(matchPath("log[!0-9].txt", "logx.txt"));
}

TEST(glob_isCaseInsensitiveByDefault) {
    CHECK(matchPath("C:/TEMP/*", "C:/temp/file"));
    CHECK(!matchPath("C:/TEMP/*", "C:/temp/file", CaseMode::Sensitive));
}

TEST(glob_backslashesAreNormalized) {
    const std::string pattern = normalizeSeparators(R"(C:\Users\*\AppData\Local\Temp\**)");
    CHECK(matchPath(pattern, R"(C:\Users\Daniil\AppData\Local\Temp\a\b.tmp)"));
    CHECK(matchPath(pattern, "C:/Users/Daniil/AppData/Local/Temp/a/b.tmp"));
}

TEST(glob_validatesPatterns) {
    CHECK(isValidPattern("C:/Temp/**"));
    CHECK(!isValidPattern(""));
    CHECK(!isValidPattern("C:/Temp/[abc"));
}

TEST(glob_expandsEnvironment) {
    const std::string env = "LOCALAPPDATA=C:\\Users\\Daniil\\AppData\\Local\n";
    bool ok = false;
    const std::string out = expandEnvironment("%LOCALAPPDATA%\\Temp\\**", env, &ok);
    CHECK(ok);
    CHECK_EQ(out, std::string("C:\\Users\\Daniil\\AppData\\Local\\Temp\\**"));
    const std::string missing = expandEnvironment("%NO_SUCH_VAR%\\x", env, &ok);
    CHECK(!ok);
    CHECK_EQ(missing, std::string("%NO_SUCH_VAR%\\x"));
}

// ---------------------------------------------------------------- units

TEST(units_formatBytes) {
    CHECK_EQ(formatBytes(0), std::string("0 Б"));
    CHECK_EQ(formatBytes(512), std::string("512 Б"));
    CHECK_EQ(formatBytes(1500), std::string("1,5 КБ"));
    CHECK_EQ(formatBytes(1500000), std::string("1,5 МБ"));
    CHECK_EQ(formatBytes(1500000000), std::string("1,5 ГБ"));
    CHECK_EQ(formatBytes(1024, 1, true), std::string("1,0 КиБ"));
}

TEST(units_formatCountAndAge) {
    CHECK_EQ(formatCount(1), std::string("1 файл"));
    CHECK_EQ(formatCount(3), std::string("3 файла"));
    CHECK_EQ(formatCount(11), std::string("11 файлов"));
    CHECK_EQ(formatCount(104), std::string("104 файла"));
    CHECK_EQ(formatAge(30), std::string("30 с"));
    CHECK_EQ(formatAge(3600 * 5), std::string("5 ч"));
    CHECK_EQ(formatAge(86400 * 3), std::string("3 дня"));
    CHECK_EQ(formatAge(86400 * 40), std::string("1 мес"));
}

TEST(units_formatPercent) {
    CHECK_EQ(formatPercent(0.1234), std::string("12,3 %"));
}

// ---------------------------------------------------------------- rules

namespace {

const char* kValidRuleFile = R"({
  "schemaVersion": 1,
  "version": "test",
  "rules": [
    {
      "id": "temp.user",
      "category": "temp",
      "safety": "safe",
      "locator": "%LOCALAPPDATA%\\Temp\\**",
      "locatorExcludes": ["%LOCALAPPDATA%\\Temp\\unins*.exe"],
      "minAgeDays": 2,
      "title": {"ru": "Временные файлы", "en": "Temp files"}
    }
  ]
})";

}  // namespace

TEST(rules_loadsValidFile) {
    const RuleSet set = loadRuleFile(kValidRuleFile, "test.json");
    CHECK_EQ(set.schemaVersion, 1);
    CHECK_EQ(set.size(), static_cast<std::size_t>(1));
    const Rule* rule = set.byId("temp.user");
    CHECK(rule != nullptr);
    CHECK(rule->safety == SafetyLevel::Safe);
    CHECK_EQ(rule->minAgeDays, static_cast<std::int64_t>(2));
    CHECK_EQ(rule->title(), std::string("Временные файлы"));
    CHECK_EQ(rule->titleEn, std::string("Temp files"));
}

TEST(rules_rejectsUnknownField) {
    const char* withJunk = R"({"schemaVersion":1,"rules":[{"id":"a.b","category":"temp","safety":"safe",
      "locator":"C:/x","deleteEverything":true}]})";
    CHECK_THROWS(loadRuleFile(withJunk, "junk.json"));
}

TEST(rules_rejectsBadIdAndSafety) {
    const char* badId = R"({"schemaVersion":1,"rules":[{"id":"Bad Id","category":"temp","safety":"safe","locator":"C:/x"}]})";
    CHECK_THROWS(loadRuleFile(badId, "id.json"));
    const char* badSafety =
        R"({"schemaVersion":1,"rules":[{"id":"a.b","category":"temp","safety":"medium","locator":"C:/x"}]})";
    CHECK_THROWS(loadRuleFile(badSafety, "safety.json"));
}

TEST(rules_rejectsWrongSchemaVersion) {
    const char* future = R"({"schemaVersion":2,"rules":[]})";
    CHECK_THROWS(loadRuleFile(future, "future.json"));
}

TEST(rules_rejectsTooBroadSafeRule) {
    const char* broad = R"({"schemaVersion":1,"rules":[{"id":"a.b","category":"other","safety":"safe",
      "locator":"C:\\Users\\*"}]})";
    CHECK_THROWS(loadRuleFile(broad, "broad.json"));
}

TEST(rules_mergesFilesAndExpandsEnvironment) {
    const std::string env = "LOCALAPPDATA=C:\\Users\\Daniil\\AppData\\Local\n";
    const char* second = R"({"schemaVersion":1,"rules":[{"id":"logs.system","category":"logs","safety":"safe",
      "locator":"C:\\Windows\\Logs\\**","minAgeDays":14}]})";
    bool unresolved = false;
    const RuleSet set = loadRuleFiles({{"a.json", kValidRuleFile}, {"b.json", second}}, env, &unresolved);
    CHECK(!unresolved);
    CHECK_EQ(set.size(), static_cast<std::size_t>(2));

    const Rule* temp = set.byId("temp.user");
    CHECK(temp != nullptr);
    CHECK_EQ(temp->resolvedLocator, std::string("C:\\Users\\Daniil\\AppData\\Local\\Temp\\**"));
    CHECK(temp->matches(R"(C:\Users\Daniil\AppData\Local\Temp\x.tmp)"));
    CHECK(!temp->matches(R"(C:\Users\Daniil\AppData\Local\Other\x.tmp)"));
    CHECK(temp->excluded(R"(C:\Users\Daniil\AppData\Local\Temp\unins000.exe)"));
    CHECK(!temp->excluded(R"(C:\Users\Daniil\AppData\Local\Temp\keep.tmp)"));
}

TEST(rules_reportsUnresolvedEnvironment) {
    bool unresolved = false;
    const RuleSet set = loadRuleFiles({{"a.json", kValidRuleFile}}, "OTHER=1\n", &unresolved);
    CHECK(unresolved);
    CHECK_EQ(set.size(), static_cast<std::size_t>(1));
}

TEST(rules_rejectsDuplicateIds) {
    const std::string env = "LOCALAPPDATA=C:\\Temp\n";
    CHECK_THROWS(loadRuleFiles({{"a.json", kValidRuleFile}, {"b.json", kValidRuleFile}}, env));
}

// ---------------------------------------------------------------- scoring

namespace {

ScoreInput baseInput() {
    ScoreInput in;
    in.ruleId = "temp.user";
    in.declaredSafety = SafetyLevel::Safe;
    in.minAgeDays = 2;
    in.now = 1700000000;
    in.newestWrite = 1700000000 - 10LL * 86400;  // 10 дней назад
    in.allocatedBytes = 64ull * 1024 * 1024;
    in.insideUserProfile = false;
    in.processClosedStateKnown = true;
    return in;
}

bool hasReason(const Score& s, const std::string& needle) {
    for (const auto& reason : s.reasons) {
        if (reason.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(scoring_freshFilesAreNotSafe) {
    ScoreInput in = baseInput();
    in.newestWrite = in.now - 3600;  // час назад, а правило требует 2 дня
    const Score score = scoreCandidate(in);
    CHECK(score.confidence < kDefaultConfidenceThreshold);
    CHECK(hasReason(score, "моложе порога"));
}

TEST(scoring_oldFilesAreConfident) {
    const Score score = scoreCandidate(baseInput());
    CHECK(score.confidence >= kDefaultConfidenceThreshold);
    CHECK(hasReason(score, "возраст"));
    CHECK(hasReason(score, "закрыты"));
}

TEST(scoring_lockedProcessesLowerConfidence) {
    ScoreInput in = baseInput();
    in.lockedProcesses = 2;
    const Score score = scoreCandidate(in);
    CHECK(hasReason(score, "работающих приложения"));
    CHECK(score.confidence < 100);
}

TEST(scoring_systemDirectoryNeverStaysSafe) {
    ScoreInput in = baseInput();
    in.inSystemDirectory = true;
    const Score score = scoreCandidate(in);
    CHECK(score.safety != SafetyLevel::Safe);
    CHECK(score.safety == SafetyLevel::Review);
}

TEST(scoring_userProfileCostsConfidence) {
    ScoreInput in = baseInput();
    in.insideUserProfile = true;
    const Score score = scoreCandidate(in);
    CHECK(hasReason(score, "внутри профиля пользователя"));
    CHECK(score.confidence < 100);
}

TEST(scoring_tinyVolumeIsNotWorthIt) {
    ScoreInput in = baseInput();
    in.allocatedBytes = 1024;
    const Score score = scoreCandidate(in);
    CHECK(hasReason(score, "слишком малый объём"));
}

TEST(scoring_confidenceStaysInRange) {
    ScoreInput in = baseInput();
    in.newestWrite = in.now;
    in.lockedProcesses = 5;
    in.inSystemDirectory = true;
    in.insideUserProfile = true;
    in.patternIsBroad = true;
    in.allocatedBytes = 0;
    const Score score = scoreCandidate(in);
    CHECK(score.confidence >= 0);
    CHECK(score.confidence <= 100);
    CHECK(score.safety == SafetyLevel::Review);
}

int main() { return mrp::runAll("MrProper core"); }
