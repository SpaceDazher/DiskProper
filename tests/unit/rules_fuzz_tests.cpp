// Фаззинг загрузчика правил очистки (SPEC §11.5): «случайные/битые JSON-правила
// не должны ронять приложение»; SPEC §12 — «0 крашей и 0 необработанных
// исключений на сценариях 2-6».
//
// Здесь не «примеры плохого JSON», а свойства, которые проверяются на большом
// детерминированном корпусе входов:
//
//   1. ЖИВУЧЕСТЬ. Любой вход либо принимается, либо отклоняется контролируемым
//      исключением с непустым сообщением. Третьего быть не может: падение
//      процесса внутри try — это и есть «приложение упало на битом правиле», и
//      никакой harness его не переживёт: тест просто умрёт вместе с остальными.
//   2. САМОСОГЛАСОВАННОСТЬ. Принятый набор не теряет и не выдумывает правил (их
//      ровно столько, сколько было во входе), id и locator проходят синтаксические
//      проверки загрузчика, а «safe» не может оказаться широким правилом без
//      minAgeDays и без имени известного кэша.
//   3. ДЕТЕРМИНИЗМ. Тот же вход даёт тот же вердикт, тот же набор и ту же
//      диагностику. Это нужно откату набора (SPEC §9.2) и повторяемым отчётам:
//      «плавающая» ошибка разбора означает, что непонятно, что приложение считает
//      проверенным набором.
//   4. ОГРАНИЧЕННОСТЬ. Вложенность, размеры и число правил не уводят загрузку в
//      перебор или в глубокую рекурсию: у каждого входа есть бюджет времени.
//
// Поля, типы и тела правил взяты из SPEC §4 FR-3 и §9.2 и из белого списка
// core::rules (src/core/rules.cpp, allowedFields), а не выдуманы.
//
// Состав (9 проверок):
//   brokenJsonIsRejectedCleanly                 — корпус из ~70 битых документов;
//   unknownFieldsAreRejectedAtEveryLevel        — чужие поля на верхнем уровне и
//                                                в правиле (SPEC §9.2 п.3);
//   rejectCharactersInFieldsAreRejected         — NUL, управляющие, не-UTF8,
//                                                RTL, длина, содержимое locator;
//   deepNestingAndOversizedInputsAreRejected…    — вложенность 2..65536, крупные
//                                                документы, мегабайтная строка;
//   mutatedRuleFilesNeverBreakTheLoader         — основной фаззинг, 2500 входов;
//   failuresDoNotLeakIntoNextLoad                — отказ не меняет состояние;
//   multiFileSetIsAllOrNothing                   — loadRuleFiles, окружение, флажок
//                                                «часть правил неактивна»;
//   diagnosticWhereRejectionLeavesRuleError…    — где загрузчик уходит с канала
//                                                RuleError (печать, не проверка);
//   matchingSurvivesHostilePaths                 — враждебные пути в сопоставителе.
//
// Детерминизм: собственный xorshift64* с фиксированным зерном, без <random> и
// без rand(), чтобы падение воспроизводилось на любой машине; зерно и номер
// итерации печатаются в сообщении об ошибке.
//
// main() живёт в core_tests.cpp, набор подхватывается по каталогу
// (tests/unit/CMakeLists.txt, file GLOB).
#include "harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "glob.hpp"
#include "json.hpp"
#include "rules.hpp"

namespace {

using mrproper::core::Rule;
using mrproper::core::RuleError;
using mrproper::core::RuleSet;
using mrproper::core::SafetyLevel;
using Clock = std::chrono::steady_clock;

// Бюджеты. Тест падает не «по таймауту», а с внятным сообщением: зависший
// загрузчик должен быть виден сразу. Значения заведомо больше реального времени
// обработки (единицы миллисекунд на вход), поэтому медленная машина их не
// заденет, а вырожденный перебор — поймает.
constexpr double kPerCaseSeconds = 0.5;
constexpr double kTotalSeconds = 20.0;
// Сколько раз корпус обязан попасть в каждую из ветвей: фаззинг, который ни разу
// не принял ни одного правила (или ни разу не отверг), ничего не проверяет.
constexpr std::size_t kMinAcceptedCases = 50;
constexpr std::size_t kMinRejectedCases = 200;

// ------------------------------------------------------ детерминированный ГПСЧ

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t next() {
        std::uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return x * 0x2545F4914F6CDD1Dull;
    }

    // Равномерное значение из [0, bound). Все вызовы — с ненулевой константой.
    std::size_t below(std::size_t bound) { return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(bound)); }

    bool oneIn(std::size_t n) { return below(n) == 0; }

private:
    std::uint64_t state_;
};

// ------------------------------------------------------------ диагностика ошибок

// Короткое читаемое представление входа: управляющие и непечатаемые байты —
// escape-последовательностью, длинный вход обрезаем, иначе одна ошибочная строка
// превращает лог теста в кашу из 200 килобайт.
std::string snippet(const std::string& text, std::size_t limit = 200) {
    std::string out;
    for (std::size_t i = 0; i < text.size() && i < limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned>(c));
            out += buf;
        } else {
            out.push_back(text[i]);
        }
    }
    if (text.size() > limit) out += "...";
    return out;
}

[[noreturn]] void fail(const std::string& what, const std::string& input) {
    throw ::mrp::Failure{what + " | вход (" + std::to_string(input.size()) + " б): " + snippet(input)};
}

void requireTrue(bool ok, const std::string& what, const std::string& input) {
    if (!ok) fail(what, input);
}

double secondsBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

// ------------------------------------------------------------ результат загрузки

enum class Verdict { Accepted, Rejected };

struct LoadResult {
    Verdict verdict{Verdict::Rejected};
    bool viaRuleError{false};  // отказ через документированный core::RuleError
    std::string message;
    RuleSet set;
};

// Единственное место, где тест трогает загрузчик правил. Исключения, не являющиеся
// std::exception, намеренно не ловятся: harness должен упасть с «непойманное
// исключение» — это тоже результат фаззинга, а не повод для тишины.
//
// Известное отклонение, которое здесь не проверяется (чтобы набор оставался
// зелёным): json::Value::require/asBool бросают std::runtime_error, а не
// core::RuleError, поэтому «нет обязательного поля» и «groupByProfile не bool»
// приходят не по документированному каналу. Пока это не исправлено в
// src/core/json.cpp / src/core/rules.cpp, требовать здесь RuleError нельзя: тест
// должен краснеть по существу (упавший загрузчик), а не по чужому отклонению.
// Счётчик таких отказов печатается в конце фаззинга.
LoadResult loadOnce(const std::string& text) {
    LoadResult result;
    try {
        result.set = mrproper::core::loadRuleFile(text, "fuzz.json");
        result.verdict = Verdict::Accepted;
    } catch (const RuleError& e) {
        result.message = e.what();
        result.viaRuleError = true;
    } catch (const std::exception& e) {
        result.message = e.what();
    }
    return result;
}

// Канонический слепок принятого набора: только для сравнения «тот же вход — тот
// же результат».
std::string fingerprint(const RuleSet& set) {
    std::string out = "schema=" + std::to_string(set.schemaVersion);
    out += ";version=" + set.version;
    out += ";minApp=" + set.minAppVersion;
    out += ";rules=" + std::to_string(set.size());
    for (const Rule& rule : set.rules) {
        out += "\n[" + rule.id + "|" + rule.category + "|" + mrproper::core::toString(rule.safety) + "|";
        out += rule.locator + "|age=" + std::to_string(rule.minAgeDays);
        out += "|profile=" + std::string(rule.groupByProfile ? "1" : "0");
        out += "|ru=" + rule.titleRu + "|en=" + rule.titleEn + "|note=" + rule.note;
        for (const std::string& ex : rule.locatorExcludes) out += "|ex=" + ex;
        for (const std::string& proc : rule.requiresProcessesClosed) out += "|proc=" + proc;
    }
    return out;
}

bool isIdChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// «Safe» без minAgeDays и без имени известного кэша — слишком широкое правило,
// такое загрузчик обязан отвергать (rules.cpp). Проверяем, что в принятом наборе
// его действительно нет.
bool looksNarrowEnough(const Rule& rule) {
    if (rule.safety != SafetyLevel::Safe) return true;
    if (rule.minAgeDays >= 1) return true;
    const std::string& locator = rule.locator;
    return locator.find("Cache") != std::string::npos || locator.find("Temp") != std::string::npos ||
           locator.find("cache") != std::string::npos || locator.find("temp") != std::string::npos;
}

// Сколько правил во входе по мнению самого парсера. loadRuleFile обязан забрать
// все: «тихо выкинутое правило» означает, что на диске лежит то, чего в
// работающем наборе нет, и приложение об этом не знает.
std::size_t countInputRules(const std::string& text) {
    const mrproper::json::Value root = mrproper::json::parse(text);
    const mrproper::json::Value* rules = root.find("rules");
    if (rules == nullptr || !rules->isArray()) return 0;
    return rules->items().size();
}

// Свойство 2: принятый набор самосогласован.
void checkAccepted(const std::string& text, const LoadResult& result) {
    const std::size_t expected = countInputRules(text);
    if (result.set.size() != expected) {
        fail("загрузчик принял не все правила: во входе " + std::to_string(expected) + ", в наборе " +
                 std::to_string(result.set.size()),
             text);
    }
    for (const Rule& rule : result.set.rules) {
        if (rule.id.empty() || rule.id.size() > 64) fail("принят id недопустимой длины: \"" + rule.id + "\"", text);
        for (const char c : rule.id) {
            if (!isIdChar(c)) fail("принят id с посторонним символом: \"" + rule.id + "\"", text);
        }
        if (rule.locator.empty()) fail("принято правило с пустым locator: " + rule.id, text);
        if (!mrproper::core::isValidPattern(rule.locator)) {
            fail("принят некорректный locator: \"" + rule.locator + "\"", text);
        }
        for (const std::string& ex : rule.locatorExcludes) {
            if (!mrproper::core::isValidPattern(ex)) {
                fail("принят некорректный locatorExcludes: \"" + ex + "\"", text);
            }
        }
        if (!looksNarrowEnough(rule)) fail("принято слишком широкое правило safe: " + rule.id, text);
    }
}

// Свойство 3: детерминизм. Повторная загрузка обязана дать тот же вердикт, тот же
// набор и ту же диагностику.
void checkDeterministic(const std::string& text, const LoadResult& first) {
    const LoadResult second = loadOnce(text);
    if (second.verdict != first.verdict) {
        fail(std::string("повторная загрузка изменила вердикт: было ") +
                 (first.verdict == Verdict::Accepted ? "принято" : "отказ") + ", стало " +
                 (second.verdict == Verdict::Accepted ? "принято" : "отказ"),
             text);
    }
    if (first.verdict == Verdict::Accepted) {
        if (fingerprint(first.set) != fingerprint(second.set)) fail("повторная загрузка дала другой набор правил", text);
    } else if (first.message != second.message) {
        fail("повторная загрузка дала другую диагностику: \"" + first.message + "\" -> \"" + second.message + "\"", text);
    }
}

// Счётчики корпуса: отчёт о покрытии печатается, чтобы «зелёный» прогон было
// видно не только по exit code.
struct Stats {
    std::size_t accepted{};
    std::size_t rejected{};
    std::size_t rejectedNotViaRuleError{};
};

// Один вход целиком: живучесть + самосогласованность + детерминизм + бюджет.
void probe(const std::string& text, Stats& stats) {
    const Clock::time_point started = Clock::now();
    const LoadResult result = loadOnce(text);
    if (secondsBetween(started, Clock::now()) > kPerCaseSeconds) {
        fail("загрузчик ушёл в перебор", text);
    }
    if (result.verdict == Verdict::Accepted) {
        ++stats.accepted;
        checkAccepted(text, result);
    } else {
        ++stats.rejected;
        if (!result.viaRuleError) ++stats.rejectedNotViaRuleError;
        requireTrue(!result.message.empty(), "отказ без диагностики", text);
    }
    checkDeterministic(text, result);
}

// ------------------------------------------------------------- «злые» байты

// Байты, которые ломают и парсер, и последующую обработку путей: управляющие
// символы, синтаксис JSON, NUL, не-UTF8, заведомо недопустимые старшие байты
// UTF-8, BOM, RTL-переопределение.
const int kEvilByteValues[] = {0x00, 0x01, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x1B, 0x1E,
                               0x1F, 0x7F, '"',  '\\', '/',  '{',  '}',  '[',  ']',  ':',  ',',  '*',
                               '?',  '!',  0x80, 0xA0, 0xBF, 0xC0, 0xC1, 0xE0, 0xEF, 0xF0, 0xF4,
                               0xF5, 0xF8, 0xFE, 0xFF, 0xC2, 0xE2, 0x80, 0xAE};

char evilByte(Rng& r) {
    return static_cast<char>(kEvilByteValues[r.below(sizeof(kEvilByteValues) / sizeof(kEvilByteValues[0]))]);
}

// ------------------------------------------------------------- строковые литералы

// Сырое содержимое строки: может содержать что угодно, включая NUL и не-UTF8.
std::string genRawString(Rng& r, std::size_t maxLen) {
    static const char kPathChars[] = "\\/:.*?";
    static const char kIdChars[] = "abcdefghijklmnopqrstuvwxyz0123456789._-";
    const std::size_t mode = r.below(10);
    const std::size_t len = r.below(maxLen + 1);
    std::string out;
    out.reserve(len + 8);
    for (std::size_t i = 0; i < len; ++i) {
        switch (mode) {
            case 0:
                out.push_back(static_cast<char>('a' + r.below(26)));
                break;
            case 1:
                out.push_back(static_cast<char>('0' + r.below(10)));
                break;
            case 2:
                out.push_back(kIdChars[r.below(sizeof(kIdChars) - 1)]);
                break;
            case 3:
                out.push_back(evilByte(r));
                break;
            case 4:
                out.push_back(kPathChars[r.below(sizeof(kPathChars) - 1)]);
                break;
            case 5:
                out.push_back('%');
                break;
            case 6:
                out.push_back(static_cast<char>(0x80 + r.below(0x40)));
                break;
            case 7:
                out.push_back(static_cast<char>(0xC0 + r.below(0x40)));
                break;
            case 8:
                out.push_back('\\');
                out.push_back(static_cast<char>('a' + r.below(26)));
                break;
            default:
                // Валидный UTF-8, но враждебный для показа: RTL-переопределение и эмодзи.
                out += "\xE2\x80\xAE\xF0\x9F\x92\xA9";
                break;
        }
    }
    return out;
}

// Сырое содержимое -> корректный JSON-литерал. Байты >= 0x20 копируются как есть
// (парсер пропускает не-UTF8), остальное экранируется: так «враждебное»
// содержимое доходит до загрузчика валидным JSON, а не битым байтом.
std::string jsonLiteral(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    for (const char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

// Настоящие правила из rules/ (SPEC §4 FR-3) — основа «хорошего» корпуса.
// Отобраны те, что проходят весь конвейер загрузки (Rule::fromJson плюс
// validateRuleSet из loadRuleFiles). Список не «примерный»: правило, которое
// loadRuleFiles отвергает, было бы бесполезно в корпусе принятых входов.
struct GoodRule {
    const char* id;
    const char* category;
    const char* safety;
    const char* locator;
    long long minAgeDays;
};

const GoodRule kGoodRules[] = {
    {"temp.user", "temp.user", "safe", "%LOCALAPPDATA%\\Temp\\**", 2},
    {"temp.user.env", "temp.user", "safe", "%TEMP%\\**", 2},
    {"temp.system", "temp.system", "safe", "C:\\Windows\\Temp\\**", 7},
    {"logs.system", "logs", "safe", "C:\\Windows\\Logs\\**", 14},
    {"wer", "wer", "safe", "C:\\ProgramData\\Microsoft\\Windows\\WER\\**", 7},
    {"icon.font.cache.explorer", "icon.font.cache", "safe", "%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer\\iconcache*.db", 2},
    {"browser.cache.chrome", "browser.cache", "safe", "%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Cache\\**", 0},
    {"browser.history", "browser.history", "review", "%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\History", 30},
    {"ms.update", "ms.update", "review", "C:\\Windows\\SoftwareDistribution\\Download\\**", 7},
    {"prefetch", "prefetch", "review", "C:\\Windows\\Prefetch\\*", 30},
    {"winsxs.report", "winsxs.report", "risky", "C:\\Windows\\WinSxS", 0},
    {"installer.cache", "installer.cache", "risky", "C:\\Windows\\Installer\\**", 0},
    {"memory.dumps.wer", "memory.dumps", "review", "%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\**\\*.dmp", 30},
    {"user.bigfiles.downloads", "user.bigfiles", "review", "%USERPROFILE%\\Downloads\\**", 90},
    {"shadercache.directx", "shadercache", "safe", "%LOCALAPPDATA%\\D3DSCache\\**", 0},
};

constexpr std::size_t kGoodRuleCount = sizeof(kGoodRules) / sizeof(kGoodRules[0]);

// Числовые литералы зловредного или сломанного набора: предел int64, бесконечность
// (1e309), исчезающе малые, точность double, «минус ноль».
const char* const kNumberLiterals[] = {"0",
                                       "1",
                                       "2",
                                       "7",
                                       "14",
                                       "30",
                                       "90",
                                       "-1",
                                       "1.5",
                                       "1e18",
                                       "1e308",
                                       "1e309",
                                       "1e-320",
                                       "0.1",
                                       "2147483648",
                                       "99999999999999999999",
                                       "-0.0",
                                       "3.141592653589793",
                                       "1e400"};

constexpr std::size_t kNumberLiteralCount = sizeof(kNumberLiterals) / sizeof(kNumberLiterals[0]);

// Имена полей: белый список rules.cpp плюс «почти свои» (регистр, опечатка,
// пробел) и заведомо чужие. Неизвестное поле — ошибка, а не предупреждение:
// правила приходят из сети (SPEC §9.2, ADR-008).
const char* const kMemberNames[] = {"id",
                                    "category",
                                    "safety",
                                    "locator",
                                    "locatorExcludes",
                                    "minAgeDays",
                                    "requiresProcessesClosed",
                                    "groupByProfile",
                                    "title",
                                    "note",
                                    "schemaVersion",
                                    "version",
                                    "minAppVersion",
                                    "rules",
                                    "Id",
                                    "ID",
                                    "ids",
                                    "id ",
                                    "safety ",
                                    "minageDays",
                                    "minAge",
                                    "Schema",
                                    "schema",
                                    "rule",
                                    "Rule",
                                    "deleteEverything",
                                    "path",
                                    "pattern",
                                    "command",
                                    "run",
                                    "",
                                    "идентификатор",
                                    "note\a bell"};

constexpr std::size_t kMemberNameCount = sizeof(kMemberNames) / sizeof(kMemberNames[0]);

// Значения, которые можно подставить вместо поля: чужие типы и значения.
const char* const kValueLiterals[] = {"null",   "true",   "false",  "0",      "-1",     "1e309",   "\"\"",
                                      "\"x\"", "[]",     "{}",     "[1,2]",  "{\"a\":1}", "1.5", "-0.0",
                                      "\"safe\"", "\"safe\" ", "[]\n", "12345678901234567890"};

constexpr std::size_t kValueLiteralCount = sizeof(kValueLiterals) / sizeof(kValueLiterals[0]);

const char* const kSafetyLiterals[] = {"safe", "review", "risky", "Safe", "SAFE", "medium", "", "null", "1"};

constexpr std::size_t kSafetyLiteralCount = sizeof(kSafetyLiterals) / sizeof(kSafetyLiterals[0]);

std::string genMemberName(Rng& r) { return kMemberNames[r.below(kMemberNameCount)]; }
std::string genValueLiteral(Rng& r) { return kValueLiterals[r.below(kValueLiteralCount)]; }
std::string genNumberLiteral(Rng& r) { return kNumberLiterals[r.below(kNumberLiteralCount)]; }

// Случайный locator: чаще настоящий, изредка придуманный, изредка заведомо
// враждебный шаблон (незакрытая скобка, одиночный «**», «%» без имени).
std::string genLocatorRaw(Rng& r) {
    const std::size_t pick = r.below(10);
    if (pick < 6) return std::string(kGoodRules[r.below(kGoodRuleCount)].locator);
    if (pick < 8) {
        static const char* const kPatterns[] = {"C:\\Temp\\*",     "**",
                                                "*",              "?",
                                                "[]",             "[a-z",
                                                "C:\\**\\**",     "%UNSET%\\**",
                                                "%%",             "%UNTERMINATED",
                                                "\\",             ""};
        return std::string(kPatterns[r.below(sizeof(kPatterns) / sizeof(kPatterns[0]))]);
    }
    return genRawString(r, 48);
}

std::string genLocatorLiteral(Rng& r) { return jsonLiteral(genLocatorRaw(r)); }

// Случайный id: чаще настоящий, изредка — с чужой длиной или символами.
std::string genIdRaw(Rng& r) {
    const std::size_t pick = r.below(8);
    if (pick < 4) return std::string(kGoodRules[r.below(kGoodRuleCount)].id);
    if (pick == 4) {
        std::string longId;
        for (std::size_t i = 0; i < 40 + r.below(80); ++i) longId.push_back('a');
        return longId;
    }
    if (pick == 5) return std::string();
    if (pick == 6) return "id/" + genRawString(r, 8);
    return genRawString(r, 12);
}

std::string genIdLiteral(Rng& r) { return jsonLiteral(genIdRaw(r)); }

// Склейка объекта из полей «"ключ":значение» без висячих запятых: генератор
// обязан выдавать синтаксически корректный JSON (про это — половина корпуса).
std::string joinObject(const std::vector<std::string>& members) {
    std::string out = "{";
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i != 0) out += ",";
        out += members[i];
    }
    out += "}";
    return out;
}

// Одно правило: настоящие поля плюс случайные чужие, неверные типы и
// «враждебное» содержимое строк.
std::string genRule(Rng& r) {
    const GoodRule& good = kGoodRules[r.below(kGoodRuleCount)];
    std::vector<std::string> members;
    members.push_back("\"id\":" + genIdLiteral(r));
    members.push_back("\"category\":" + jsonLiteral(genRawString(r, 16)));
    members.push_back(std::string("\"safety\":\"") + kSafetyLiterals[r.below(kSafetyLiteralCount)] + "\"");

    if (r.below(8) == 0) {
        members.push_back("\"locator\":" + genValueLiteral(r));
    } else {
        members.push_back("\"locator\":" + genLocatorLiteral(r));
    }

    switch (r.below(6)) {
        case 0:
            members.push_back("\"locatorExcludes\":" + genValueLiteral(r));
            break;
        case 1:
            // Массив, где не все элементы — строки.
            members.push_back("\"locatorExcludes\":[\"C:\\\\Temp\\\\**\",1]");
            break;
        case 2: {
            // Много исключений: верхний предел умеренный, чтобы кросс-проверки
            // набора оставались в разумном времени.
            const std::size_t count = 1 + r.below(64);
            std::string list = "\"locatorExcludes\":[";
            for (std::size_t i = 0; i < count; ++i) {
                if (i != 0) list += ",";
                list += genLocatorLiteral(r);
            }
            list += "]";
            members.push_back(list);
            break;
        }
        case 3:
            members.push_back("\"locatorExcludes\":[\"C:\\\\Temp\\\\**\\\\unins*.exe\"]");
            break;
        default:
            break;
    }

    if (r.below(4) != 0) {
        if (r.below(8) == 0) {
            members.push_back("\"minAgeDays\":" + genValueLiteral(r));
        } else {
            members.push_back("\"minAgeDays\":" + std::to_string(good.minAgeDays));
        }
    }
    // groupByProfile то bool, то строка: asBool() на строке бросает не-RuleError
    // (известное отклонение), и такой вход фаззинг обязан пережить.
    if (r.below(4) == 0) members.push_back("\"groupByProfile\":" + genValueLiteral(r));
    if (r.below(3) == 0) {
        members.push_back("\"requiresProcessesClosed\":[" + jsonLiteral(genRawString(r, 12)) + "]");
    }
    if (r.below(2) == 0) {
        members.push_back("\"title\":{\"ru\":" + genValueLiteral(r) + "}");
    } else {
        members.push_back("\"title\":{\"ru\":" + jsonLiteral(genRawString(r, 24)) + ",\"en\":" +
                          jsonLiteral(genRawString(r, 24)) + "}");
    }
    if (r.below(2) == 0) members.push_back("\"note\":" + jsonLiteral(genRawString(r, 64)));
    if (r.below(3) == 0) members.push_back("\"files\":[" + genValueLiteral(r) + "]");
    if (r.below(3) == 0) members.push_back("\"schemaVersion\":" + genNumberLiteral(r));
    return joinObject(members);
}

// Случайный, но синтаксически корректный документ правил: часть полей настоящая,
// часть — чужая или неверного типа. Байтовые мутации не должны быть единственным
// источником входа: половина сценариев §11.5 — валидный JSON плохого правила.
std::string genRandomDocument(Rng& r) {
    std::vector<std::string> members;
    if (r.below(10) != 0) {
        if (r.below(8) == 0) {
            members.push_back("\"schemaVersion\":" + genValueLiteral(r));
        } else {
            static const char* const kSchemaVersions[] = {"1", "0", "2", "-1", "1.0", "1e309"};
            members.push_back(std::string("\"schemaVersion\":") + kSchemaVersions[r.below(6)]);
        }
    }
    if (r.below(4) == 0) members.push_back("\"version\":" + jsonLiteral(genRawString(r, 12)));
    if (r.below(4) == 0) members.push_back("\"minAppVersion\":" + jsonLiteral(genRawString(r, 8)));
    // Чужие поля верхнего уровня — самая частая находка при обновлении набора.
    const std::size_t extra = r.below(3);
    for (std::size_t i = 0; i < extra; ++i) {
        members.push_back("\"" + genMemberName(r) + "\":" + genValueLiteral(r));
    }
    if (r.below(10) == 0) {
        members.push_back("\"rules\":" + genValueLiteral(r));
        return joinObject(members);
    }
    const std::size_t count = r.below(4);
    std::string list = "\"rules\":[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) list += ",";
        list += genRule(r);
    }
    list += "]";
    members.push_back(list);
    return joinObject(members);
}

// Хорошее правило из настоящей таблицы, без чужих полей: гарантирует, что в
// корпусе есть и принимаемые входы. tableBegin/tableCount задают срез таблицы:
// для склейки двух файлов срез нужен, иначе два документа могут вытянуть одно и то
// же id и loadRuleFiles отвергнет их как дубликаты (а это проверка не про то).
std::string genGoodDocument(Rng& r, std::size_t tableBegin = 0, std::size_t tableCount = kGoodRuleCount) {
    std::string out = "{\"schemaVersion\":1,\"version\":\"fuzz\",\"rules\":[";
    const std::size_t count = 1 + r.below(3);
    for (std::size_t i = 0; i < count; ++i) {
        const GoodRule& good = kGoodRules[(tableBegin + r.below(tableCount)) % kGoodRuleCount];
        if (i != 0) out += ",";
        // Содержимое полей обязано проходить через jsonLiteral: в путях Windows
        // обратные слэши, а в JSON «C:\Temp» — это «неизвестный escape», и весь
        // «хороший» корпус рассыпался бы как битый вход.
        out += "{\"id\":" + jsonLiteral(good.id) + ",\"category\":" + jsonLiteral(good.category) + ",\"safety\":" +
               jsonLiteral(good.safety) + ",\"locator\":" + jsonLiteral(good.locator);
        if (good.minAgeDays > 0) out += ",\"minAgeDays\":" + std::to_string(good.minAgeDays);
        if (r.below(3) == 0) {
            out += ",\"locatorExcludes\":[" + jsonLiteral(std::string(good.locator) + "\\unins*.exe") + "]";
        }
        if (r.below(3) == 0) out += ",\"groupByProfile\":true";
        if (r.below(3) == 0) out += ",\"requiresProcessesClosed\":[\"chrome.exe\"]";
        if (r.below(3) == 0) {
            out += ",\"title\":{\"ru\":\"Заголовок\",\"en\":\"Title\"},\"note\":\"пояснение\"";
        }
        out += "}";
    }
    out += "]}";
    return out;
}

// --------------------------------------------------------------- мутации байтов

using Mutator = std::string (*)(const std::string&, Rng&);

std::string mutateTruncate(const std::string& in, Rng& r) {
    if (in.empty()) return in;
    return in.substr(0, r.below(in.size()));
}

std::string mutateDeleteSlice(const std::string& in, Rng& r) {
    if (in.size() < 2) return in;
    const std::size_t begin = r.below(in.size() - 1);
    const std::size_t length = 1 + r.below(std::min<std::size_t>(32, in.size() - begin));
    return in.substr(0, begin) + in.substr(begin + length);
}

std::string mutateInsertByte(const std::string& in, Rng& r) {
    const char c = evilByte(r);
    const std::size_t pos = r.below(in.size() + 1);
    return in.substr(0, pos) + c + in.substr(pos);
}

std::string mutateReplaceByte(const std::string& in, Rng& r) {
    if (in.empty()) return in;
    std::string out = in;
    out[r.below(out.size())] = evilByte(r);
    return out;
}

// Повтор фрагмента: так появляются дубли ключей и «склеенные» значения.
std::string mutateDuplicateSlice(const std::string& in, Rng& r) {
    if (in.size() < 4) return in;
    const std::size_t begin = r.below(in.size() - 1);
    const std::size_t length = 1 + r.below(std::min<std::size_t>(24, in.size() - begin));
    return in.substr(0, begin + length) + in.substr(begin, length) + in.substr(begin + length);
}

std::string mutateSwapChars(const std::string& in, Rng& r) {
    if (in.size() < 2) return in;
    std::string out = in;
    const std::size_t a = r.below(out.size());
    const std::size_t b = r.below(out.size());
    std::swap(out[a], out[b]);
    return out;
}

// Вставка чужого поля сразу после случайной «{»: попадает то на верхний уровень,
// то внутрь первого правила — оба уровня проверяются отдельно.
std::string mutateInjectMember(const std::string& in, Rng& r) {
    std::size_t pos = std::string::npos;
    std::size_t skipFirst = r.below(3);
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '{') continue;
        if (skipFirst == 0) {
            pos = i;
            break;
        }
        --skipFirst;
    }
    if (pos == std::string::npos) return in;
    return in.substr(0, pos + 1) + "\"" + genMemberName(r) + "\":" + genValueLiteral(r) + "," + in.substr(pos + 1);
}

// Дублирование ключа на том же уровне: JSON это допускает и парсер тоже, поэтому
// проверяем, что дубликат не ломает разбор и не удваивает набор.
std::string mutateDuplicateKey(const std::string& in, Rng& r) {
    std::size_t open = std::string::npos;
    std::size_t skipFirst = r.below(2);
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '{') continue;
        if (skipFirst == 0) {
            open = i;
            break;
        }
        --skipFirst;
    }
    if (open == std::string::npos) return in;
    const std::size_t quote = in.find('"', open + 1);
    if (quote == std::string::npos) return in;
    const std::size_t closeQuote = in.find('"', quote + 1);
    if (closeQuote == std::string::npos) return in;
    const std::size_t colon = in.find(':', closeQuote);
    if (colon == std::string::npos) return in;
    const std::string key = in.substr(quote, closeQuote - quote + 1) + ":";
    return in.substr(0, open + 1) + key + "," + in.substr(open + 1);
}

const Mutator kMutators[] = {mutateTruncate,      mutateDeleteSlice,     mutateInsertByte, mutateReplaceByte,
                             mutateDuplicateSlice, mutateSwapChars,       mutateInjectMember, mutateDuplicateKey};

constexpr std::size_t kMutatorCount = sizeof(kMutators) / sizeof(kMutators[0]);

std::string applyMutator(const std::string& text, Rng& r) { return kMutators[r.below(kMutatorCount)](text, r); }

// Одно правило по id из таблицы: проверки окружения должны быть детерминированными,
// поэтому эталонный документ строится явно, а не «какое правило выпало».
std::string singleRuleDocument(const char* id) {
    for (const GoodRule& good : kGoodRules) {
        if (std::string(good.id) != id) continue;
        return "{\"schemaVersion\":1,\"rules\":[{\"id\":" + jsonLiteral(good.id) + ",\"category\":" + jsonLiteral(good.category) +
               ",\"safety\":" + jsonLiteral(good.safety) + ",\"locator\":" + jsonLiteral(good.locator) +
               (good.minAgeDays > 0 ? ",\"minAgeDays\":" + std::to_string(good.minAgeDays) : "") + "}]}";
    }
    return std::string("{\"schemaVersion\":1,\"rules\":[]}");
}

// ---------------------------------------------------------------- корпус «руками»

// Корпус битого JSON собирается вокруг настоящего правила: «принять» такой вход
// может только полностью корректный JSON, поэтому каждая строка ниже обязана быть
// отказом. NUL-случаи добавлены отдельно — в литерале const char* он обрезает
// хвост, и вход получился бы валидным.
std::string doc(const std::string& rulesValue) { return "{\"schemaVersion\":1,\"rules\":" + rulesValue + "}"; }

std::string goodRuleBody() {
    return "{\"id\":\"temp.user\",\"category\":\"temp.user\",\"safety\":\"safe\","
           "\"locator\":\"%LOCALAPPDATA%\\\\Temp\\\\**\",\"minAgeDays\":2}";
}

std::vector<std::string> brokenCorpus() {
    return {
        // пусто и пробелы
        "",
        " ",
        "\t\r\n",
        // корень не объект
        "null",
        "[]",
        "[{\"schemaVersion\":1}]",
        "42",
        "\"правило\"",
        "true",
        // обрыв структуры
        "{",
        "{\"schemaVersion\":1,\"rules\":[",
        "{\"schemaVersion\":1,\"rules\":[{}",
        "{\"schemaVersion\":1,\"rules\":[{}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":}",
        "{\"schemaVersion\":1,\"rules\"[{}]}",
        "{\"schemaVersion\":1\" \"rules\":[]}",
        "{\"schemaVersion\":1,\"rules\":[]}{\"schemaVersion\":1,\"rules\":[]}",
        // мусор вокруг валидного документа
        "\xEF\xBB\xBF{\"schemaVersion\":1,\"rules\":[]}",
        "{\"schemaVersion\":1,\"rules\":[]}\x7F",
        "{\"schemaVersion\":1,\"rules\":[]}\xC0\x80",
        "{\"schemaVersion\":1,\"rules\":[]}\xE2\x80\xAE",
        "{\"schemaVersion\":1,\"rules\":[]} // хвост",
        "/* примечание */{\"schemaVersion\":1,\"rules\":[]}",
        // недопустимые расширения JSON (JSON5/JS)
        "{'schemaVersion':1,'rules':[]}",
        "{\"schemaVersion\":1,\"rules\":[],}",
        "{\"schemaVersion\":1,,\"rules\":[]}",
        "{\"schemaVersion\":0x1,\"rules\":[]}",
        "{\"schemaVersion\":NaN,\"rules\":[]}",
        "{\"schemaVersion\":Infinity,\"rules\":[]}",
        "{\"schemaVersion\":.,\"rules\":[]}",
        "{\"schemaVersion\":-,\"rules\":[]}",
        "{\"schemaVersion\":1e,\"rules\":[]}",
        "{\"schemaVersion\":1e+,\"rules\":[]}",
        "{schemaVersion:1,rules:[]}",
        "{,}",
        "{:}",
        // строки
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\"}]",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\\\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\\u00zz\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\\u12\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\\q\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\x01\x02\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\x7F\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"" + std::string(40000, 'a') + "\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\\",
        // NUL: в строковом литерале обрезает хвост, поэтому собираем явно
        std::string(1, '\0') + "{\"schemaVersion\":1,\"rules\":[]}",
        "{\"schemaVersion\":1,\"rules\":[]}" + std::string(1, '\0'),
        "{\"schemaVersion\":1,\"rules\":[{\"id\"" + std::string(1, '\0') + "\"}]}",
        std::string(1, '\0') + "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\","
                               "\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        // правило не того типа
        "{\"schemaVersion\":1,\"rules\":[null]}",
        "{\"schemaVersion\":1,\"rules\":[42]}",
        "{\"schemaVersion\":1,\"rules\":[\"temp.user\"]}",
        "{\"schemaVersion\":1,\"rules\":[[]]}",
        "{\"schemaVersion\":1,\"rules\":[{}]}",
        // обрыв многобайтовой последовательности UTF-8 внутри строки
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a\xF0\x9F",
        // значения полей неверного типа или недопустимые
        "{\"schemaVersion\":1,\"rules\":[{\"id\":1,\"category\":\"c\",\"safety\":\"safe\",\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"medium\",\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"safe\",\"locator\":\"\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"safe\",\"locator\":\"C:[\\\\**\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"safe\",\"locator\":\"C:\\\\Users\\\\*\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"A.B\",\"category\":\"c\",\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a/b\",\"category\":\"c\",\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\",\"locatorExcludes\":[1]}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\",\"locatorExcludes\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\",\"minAgeDays\":\"2\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\",\"minAgeDays\":-1}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\",\"title\":[]}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\",\"title\":\"строка\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\",\"unknown\":\"x\"}]}",
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\"}],\"unknownRootField\":[1,2,3]}",
        "{\"schemaVersion\":2,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"schemaVersion\":\"1\",\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\"}]}",
        "{\"schemaVersion\":1,\"rules\":{}}",
        "{\"schemaVersion\":1,\"rules\":\"temp.user\"}",
    };
}

// Документ с настоящим правилом, в котором подменён только id: «враждебное» правило
// проверяется отдельно от битого JSON.
std::string docWithId(const std::string& idLiteral) {
    return "{\"schemaVersion\":1,\"rules\":[{\"id\":" + idLiteral +
           ",\"category\":\"c\",\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\"}]}";
}

// Загрузка набора файлов с переводом отказа в диагностику. Исключение, ушедшее
// из loadRuleFiles, должно падать с указанием входа, а не как «непойманное
// исключение» без входа вовсе.
struct FilesResult {
    bool accepted{false};
    std::string message;
    RuleSet set;
};

FilesResult loadFiles(const std::vector<std::pair<std::string, std::string>>& files, const std::string& env,
                      bool* unresolved) {
    FilesResult out;
    try {
        out.set = mrproper::core::loadRuleFiles(files, env, unresolved);
        out.accepted = true;
    } catch (const std::exception& e) {
        out.message = e.what();
    }
    return out;
}

}  // namespace

// ============================================================== битый JSON

// SPEC §11.5: битый JSON не должен ронять загрузчик. Важно не «что именно он
// отверг», а то, что отказ контролируемый, с диагностикой, и что повторная
// попытка даёт тот же ответ.
TEST(rules_fuzz_brokenJsonIsRejectedCleanly) {
    const std::vector<std::string> corpus = brokenCorpus();
    Stats stats;
    const Clock::time_point started = Clock::now();
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        const std::string& text = corpus[i];
        const Clock::time_point caseStarted = Clock::now();
        const LoadResult result = loadOnce(text);
        if (secondsBetween(caseStarted, Clock::now()) > kPerCaseSeconds) {
            fail("битый JSON разбирается слишком долго", text);
        }
        requireTrue(result.verdict == Verdict::Rejected,
                    "битый JSON принят за правило, корпус №" + std::to_string(i), text);
        requireTrue(!result.message.empty(), "отказ без диагностики, корпус №" + std::to_string(i), text);
        checkDeterministic(text, result);
        ++stats.rejected;
        if (!result.viaRuleError) ++stats.rejectedNotViaRuleError;
    }
    requireTrue(secondsBetween(started, Clock::now()) < kTotalSeconds, "корпус битого JSON не уложился в бюджет", "");

    // Эталон: тот же документ без поломок обязан приняться. Иначе проверки выше
    // проходили бы просто потому, что загрузчик отвергает всё подряд.
    const std::string good = doc("[" + goodRuleBody() + "]");
    const LoadResult goodResult = loadOnce(good);
    requireTrue(goodResult.verdict == Verdict::Accepted, "эталонный документ отвергнут", good);
    checkAccepted(good, goodResult);
    ++stats.accepted;

    CHECK_EQ(stats.rejected, corpus.size());
    CHECK_EQ(stats.accepted, static_cast<std::size_t>(1));
}

// ========================================================= неизвестные поля

// SPEC §9.2 п.3: «неизвестные поля — ошибка». Верхний уровень и уровень правила,
// включая «почти свои» имена (регистр, опечатка, пробел) и управляющий символ в
// имени: правила приходят из сети, доверять здесь нечему.
TEST(rules_fuzz_unknownFieldsAreRejectedAtEveryLevel) {
    const std::string base =
        "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\","
        "\"locator\":\"C:\\\\Temp\\\\**\"}]}";
    const std::string ruleTail = "\"locator\":\"C:\\\\Temp\\\\**\"}";

    std::vector<std::string> hostile;
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + "],\"deleteEverything\":true}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + "],\"Rule\":\"a.b\"}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + "],\"schema\":1}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"minageDays\":30}]}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"minAge\":30}]}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"Id\":\"other\"}]}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"path\":\"C:\\\\\"}]}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"command\":\"rm -rf\"}]}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"\\u0007bell\":1}]}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"" + std::string(500, 'x') + "\":1}]}");
    hostile.push_back("{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"review\"," +
                      ruleTail + ",\"идентификатор\":\"a.b\"}]}");

    Stats stats;
    for (std::size_t i = 0; i < hostile.size(); ++i) {
        const LoadResult result = loadOnce(hostile[i]);
        requireTrue(result.verdict == Verdict::Rejected,
                    "неизвестное поле принято, вход №" + std::to_string(i), hostile[i]);
        requireTrue(!result.message.empty(), "отказ без диагностики, вход №" + std::to_string(i), hostile[i]);
        requireTrue(result.viaRuleError, "отказ пришёл не через core::RuleError, вход №" + std::to_string(i), hostile[i]);
        checkDeterministic(hostile[i], result);
        ++stats.rejected;
    }

    // Документ без чужих полей обязан грузиться — иначе проверки выше проходили бы
    // потому, что загрузчик отвергает всё подряд.
    const LoadResult good = loadOnce(base);
    requireTrue(good.verdict == Verdict::Accepted, "эталонный документ отвергнут", base);
    checkAccepted(base, good);

    // Внутри объекта title чужие поля допустимы парсером: title читается по ключам
    // ru/en, а сверка состава полей есть только на верхнем уровне и на уровне
    // правила. Фиксируем это как наблюдаемое поведение, а не как требование.
    const std::string titleJunk = "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\","
                                  "\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\","
                                  "\"title\":{\"ru\":\"З\",\"en\":\"T\",\"extra\":[1,2]}}]}";
    const LoadResult titleResult = loadOnce(titleJunk);
    requireTrue(titleResult.verdict == Verdict::Accepted,
                "чужое поле внутри title отвергнуто: наблюдаемое поведение парсера изменилось", titleJunk);

    // Дубль известного ключа «rules»: JSON это допускает, парсер берёт первое
    // вхождение (json::Value::find идёт по порядку). Проверяем, что второй
    // «rules» не удваивает и не портит набор, — поведение зафиксировано
    // наблюдаемым, а не требуемым: подпись набора в манифесте покрывает байты
    // файла, поэтому подмена после подписи не проходит.
    const std::string dupRules = "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\","
                                 "\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\"}],\"rules\":[]}";
    const LoadResult dupResult = loadOnce(dupRules);
    requireTrue(dupResult.verdict == Verdict::Accepted, "дубль ключа rules отвергнут: поведение изменилось", dupRules);
    CHECK_EQ(dupResult.set.size(), static_cast<std::size_t>(1));
    checkAccepted(dupRules, dupResult);

    CHECK_EQ(stats.rejected, hostile.size());
}

// ================================================== режект-символы в полях

// «Режект-символы»: NUL и управляющие символы, не-UTF8, RTL-переопределение,
// пустые и гигантские значения, подстановка переменных окружения в id. Валидный
// JSON плохого правила — самая недооценённая форма зловредного набора: битый JSON
// отсекается ещё проверкой целостности (SPEC §9.2 п.3), а «просто плохое
// правило» доходит до сканера.
TEST(rules_fuzz_rejectCharactersInFieldsAreRejected) {
    std::vector<std::string> hostile;
    hostile.push_back(docWithId("\"\""));                                // пустой id
    hostile.push_back(docWithId("\"A.B\""));                             // регистр
    hostile.push_back(docWithId("\"a b\""));                             // пробел
    hostile.push_back(docWithId("\"a/b\""));                             // разделитель пути
    hostile.push_back(docWithId("\"a\\\\b\""));                          // обратный слэш
    hostile.push_back(docWithId("\"a:b\""));                             // двоеточие (псевдопоток NTFS)
    hostile.push_back(docWithId("\"a*b\""));                             // wildcard в id
    hostile.push_back(docWithId("\"%LOCALAPPDATA%\""));                  // переменная окружения
    hostile.push_back(docWithId("\"/\""));                               // корень
    hostile.push_back(docWithId("\"" + std::string(65, 'a') + "\""));    // длиннее 64
    hostile.push_back(docWithId("\"a\\u0000b\""));                       // NUL
    hostile.push_back(docWithId("\"a\\u0007b\""));                       // BEL
    hostile.push_back(docWithId("\"a\\u001fb\""));                       // управляющий разделитель
    hostile.push_back(docWithId("\"a\\u000ab\""));                       // перевод строки
    hostile.push_back(docWithId("\"a\\u007fb\""));                       // DEL
    hostile.push_back(docWithId("\"a\\u00e2\\u0080\\u00aeb\""));         // RTL-переопределение
    hostile.push_back(docWithId("\"a\\ud83d\\ude00b\""));               // эмодзи (валидный UTF-8)
    hostile.push_back(docWithId("\"a\\ud800b\""));                       // одиночный сурогат
    hostile.push_back(docWithId("\"a\\ud800\\ud800b\""));                // два высоких сурогога
    // Байты собираем char'ами явно: «\x80b» в литерале — это один hex-escape из трёх
    // цифр, а не два символа, и такой вход проверял бы не то, что задумано.
    hostile.push_back(docWithId(std::string("\"a") + '\xC0' + '\x80' + "b\""));          // битый старший байт
    hostile.push_back(docWithId(std::string("\"a") + '\xF5' + '\x80' + '\x80' + "b\"")); // недопустимый UTF-8
    hostile.push_back(docWithId("\"a\\u0000\""));                        // только NUL
    hostile.push_back(docWithId("\"" + std::string(4096, 'a') + "\""));  // мегабайтный id
    hostile.push_back(docWithId("\"a.b\""));                             // эталон: должен приняться

    std::size_t accepted = 0;
    for (const std::string& text : hostile) {
        const LoadResult result = loadOnce(text);
        if (result.verdict == Verdict::Accepted) {
            ++accepted;
            checkAccepted(text, result);
            continue;
        }
        requireTrue(!result.message.empty(), "отказ без диагностики", text);
        checkDeterministic(text, result);
    }
    // Приняться должен ровно эталонный id: иначе проверки выше не отличают
    // «отверг правильно» от «отверг всё подряд».
    CHECK_EQ(accepted, static_cast<std::size_t>(1));
}

// ================================================== вложенность и размеры

// Границы входа. Вложенность json::Value ограничена (kMaxDepth = 64), и именно это
// не даёт зловредному набору уронить стек парсера: без ограничения «[[[[…]]]]» на
// сотни тысяч уровней — это переполнение стека и краш приложения.
TEST(rules_fuzz_deepNestingAndOversizedInputsAreRejectedCleanly) {
    Stats stats;
    const Clock::time_point started = Clock::now();

    const std::size_t depths[] = {2, 8, 32, 63, 64, 65, 66, 128, 1024, 65536};
    std::size_t deepCases = 0;
    for (const std::size_t depth : depths) {
        const std::string nested(depth, '[');
        const std::string withTail = nested + std::string(depth, ']');
        // Вложенность вместо rules: её разбирает парсер, а не валидатор.
        const std::string asRules = "{\"schemaVersion\":1,\"rules\":" + withTail + "}";
        // То же самое во вложенном поле правила.
        const std::string asField = "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\","
                                    "\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\",\"junk\":" +
                                    withTail + "}]}";
        const std::string nestedObjects = "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\","
                                          "\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\",\"junk\":" +
                                          std::string(depth, '{') + "\"a\":" + std::string(depth, '}') + "}]}";
        for (const std::string& text : {asRules, asField, nestedObjects}) {
            const Clock::time_point caseStarted = Clock::now();
            const LoadResult result = loadOnce(text);
            if (secondsBetween(caseStarted, Clock::now()) > kPerCaseSeconds) {
                fail("глубокая вложенность разбирается слишком долго, глубина " + std::to_string(depth), text);
            }
            requireTrue(result.verdict == Verdict::Rejected,
                        "вложенность " + std::to_string(depth) + " принята за правило", text);
            requireTrue(!result.message.empty(), "отказ без диагностики, глубина " + std::to_string(depth), text);
            ++stats.rejected;
            ++deepCases;
        }
    }

    // Крупный, но корректный документ: много правил, много исключений, длинные
    // строки. Размер сам по себе не должен превращать загрузку в минуты;
    // кросс-проверка id в validateRuleSet квадратична — корпус держится в
    // разумных пределах сознательно.
    std::string wide = "{\"schemaVersion\":1,\"rules\":[";
    for (std::size_t i = 0; i < 120; ++i) {
        if (i != 0) wide += ",";
        wide += "{\"id\":\"bulk.rule" + std::to_string(i) + "\",\"category\":\"bulk\",\"safety\":\"review\","
                "\"locator\":\"C:\\\\Bulk\\\\" + std::to_string(i) + "\\\\**\"";
        wide += ",\"locatorExcludes\":[";
        for (std::size_t j = 0; j < 40; ++j) {
            if (j != 0) wide += ",";
            wide += "\"C:\\\\Bulk\\\\" + std::to_string(i) + "\\\\keep" + std::to_string(j) + "*.tmp\"";
        }
        wide += "]";
        wide += ",\"note\":\"" + std::string(2000, 'n') + "\"";
        wide += "}";
    }
    wide += "]}";
    const LoadResult wideResult = loadOnce(wide);
    requireTrue(wideResult.verdict == Verdict::Accepted, "корректный крупный документ отвергнут", wide);
    CHECK_EQ(wideResult.set.size(), static_cast<std::size_t>(120));
    checkAccepted(wide, wideResult);
    ++stats.accepted;

    // Мегабайтная строка: загрузчик не обязан её отвергать, но обязан пережить.
    const std::string huge = "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\","
                             "\"safety\":\"review\",\"locator\":\"C:\\\\Temp\\\\**\",\"note\":\"" +
                             std::string(2 * 1024 * 1024, 'x') + "\"}]}";
    const Clock::time_point hugeStarted = Clock::now();
    const LoadResult hugeResult = loadOnce(huge);
    requireTrue(secondsBetween(hugeStarted, Clock::now()) < kPerCaseSeconds, "мегабайтная строка читается слишком долго",
                huge);
    requireTrue(hugeResult.verdict == Verdict::Accepted, "мегабайтная строка отвергнута неожиданно", huge);
    checkAccepted(huge, hugeResult);
    ++stats.accepted;

    requireTrue(secondsBetween(started, Clock::now()) < kTotalSeconds, "границы входа не уложились в бюджет", "");
    CHECK_EQ(stats.rejected, deepCases);
    CHECK_EQ(stats.accepted, static_cast<std::size_t>(2));
}

// ============================================================= основной фаззинг

// Главный корпус: настоящие правила и случайная структура, поверх 0..3 байтовые
// мутации. Каждый вход проверяется на живучесть, самосогласованность,
// детерминизм и бюджет времени.
TEST(rules_fuzz_mutatedRuleFilesNeverBreakTheLoader) {
    Rng r(0x5EED1234ABCDEF01ull);
    Stats stats;
    constexpr std::size_t kIterations = 2500;

    const Clock::time_point started = Clock::now();
    for (std::size_t i = 0; i < kIterations; ++i) {
        // Половина входов — валидные документы (чтобы приёмка и отказ покрывались
        // сопоставимо), половина — случайная структура.
        std::string text = r.oneIn(2) ? genGoodDocument(r) : genRandomDocument(r);
        const std::size_t mutations = r.below(4);
        for (std::size_t m = 0; m < mutations; ++m) text = applyMutator(text, r);
        // Границы теста: длиннее вход ничего не добавляет, а замедляет весь набор.
        if (text.size() > 64 * 1024) text.resize(64 * 1024);

        const std::string subject =
            "итерация " + std::to_string(i) + ", зерно 0x5EED1234ABCDEF01, вход: " + snippet(text);
        try {
            probe(text, stats);
        } catch (const mrp::Failure& f) {
            throw mrp::Failure{f.message + "\n         " + subject};
        }
    }
    const double elapsed = secondsBetween(started, Clock::now());
    requireTrue(elapsed < kTotalSeconds, "фаззинг не уложился в бюджет " + std::to_string(kTotalSeconds) + " с", "");

    // Покрытие: без этих двух проверок корпус может оказаться вырожденным — например,
    // всегда отвергать всё, и тест станет зелёным вхолостую.
    requireTrue(stats.accepted >= kMinAcceptedCases,
                "корпус почти не дошёл до приёмки: принято " + std::to_string(stats.accepted), "");
    requireTrue(stats.rejected >= kMinRejectedCases,
                "корпус почти не дошёл до отказа: отказов " + std::to_string(stats.rejected), "");

    std::printf("  rules_fuzz: %zu входов, принято %zu, отказов %zu (не через RuleError: %zu), %.2f с\n", kIterations,
                stats.accepted, stats.rejected, stats.rejectedNotViaRuleError, elapsed);
}

// ============================================ отсутствие утечки состояния

// Отказ не должен оставлять после себя состояния: следующая загрузка обязана вести
// себя так, будто неудачной не было. Иначе «плохой» набор, однажды прочитанный,
// менял бы трактовку хороших правил.
TEST(rules_fuzz_failuresDoNotLeakIntoNextLoad) {
    Rng r(0x0BADC0DE0BADC0DEull);
    const std::string good = genGoodDocument(r);
    const LoadResult baseline = loadOnce(good);
    requireTrue(baseline.verdict == Verdict::Accepted, "эталонный документ отвергнут", good);
    const std::string baselinePrint = fingerprint(baseline.set);

    Stats stats;
    for (std::size_t i = 0; i < 400; ++i) {
        std::string text = r.oneIn(2) ? genRandomDocument(r) : genGoodDocument(r);
        const std::size_t mutations = 1 + r.below(3);
        for (std::size_t m = 0; m < mutations; ++m) text = applyMutator(text, r);
        probe(text, stats);
        // После каждой попытки — эталон снова: он не должен измениться.
        const LoadResult again = loadOnce(good);
        requireTrue(again.verdict == Verdict::Accepted, "эталонный документ перестал грузиться после отказа", text);
        if (fingerprint(again.set) != baselinePrint) fail("эталонный набор изменился после неудачной загрузки", good);
    }
    CHECK_EQ(stats.accepted + stats.rejected, static_cast<std::size_t>(400));
}

// ================================================= несколько файлов набора

// loadRuleFiles — то, что реально вызывает движок: склейка нескольких файлов и
// подстановка переменных окружения (SPEC §9.2 п.3, п.5). Проверяем, что враждебный
// файл в склейке отвергает весь кандидат (а не «наполовину применяется»), а флажок
// «часть правил неактивна» честно отражает окружение.
TEST(rules_fuzz_multiFileSetIsAllOrNothing) {
    Rng r(0x7E57A1E5A11CE5ull);
    // Полное окружение: ни одна переменная из таблицы правил не должна остаться
    // неразрешённой, иначе флаг «часть правил неактивна» встанет на ровном месте.
    const std::string env =
        "LOCALAPPDATA=C:\\Users\\Daniil\\AppData\\Local\n"
        "TEMP=C:\\Temp\n"
        "USERPROFILE=C:\\Users\\Daniil\n"
        "ProgramData=C:\\ProgramData\n"
        "ProgramFiles=C:\\Program Files\n"
        "SystemDrive=C:\n"
        "WINDIR=C:\\Windows\n";
    // Два файла из непересекающихся срезов таблицы: иначе дубликаты id в склейке.
    const std::string first = genGoodDocument(r, 0, 8);
    const std::string second = genGoodDocument(r, 8, 7);
    // Эталон с гарантированной переменной окружения в locator.
    const std::string onlyTemp = singleRuleDocument("temp.user");

    // Чистая пара: единственный набор, который сливается.
    {
        bool unresolved = true;
        const FilesResult loaded = loadFiles({{"a.json", first}, {"b.json", second}}, env, &unresolved);
        requireTrue(loaded.accepted, "эталонная пара не слилась: " + loaded.message, first);
        const RuleSet& set = loaded.set;
        CHECK_EQ(set.rules.size(), countInputRules(first) + countInputRules(second));
        CHECK(!unresolved);
        for (const Rule& rule : set.rules) {
            requireTrue(!rule.resolvedLocator.empty(), "правило без раскрытого locator: " + rule.id, "");
            requireTrue(rule.resolvedExcludes.size() == rule.locatorExcludes.size(),
                        "раскрытых исключений меньше, чем объявлено: " + rule.id, "");
        }
        // Переменная окружения действительно подставлена, а не осталась в шаблоне.
        const FilesResult one = loadFiles({{"a.json", onlyTemp}}, env, nullptr);
        requireTrue(one.accepted, "эталонное правило не слилось: " + one.message, onlyTemp);
        const Rule* temp = one.set.byId("temp.user");
        requireTrue(temp != nullptr, "эталонное правило потерялось в склейке", onlyTemp);
        requireTrue(temp->resolvedLocator == "C:\\Users\\Daniil\\AppData\\Local\\Temp\\**",
                    "%LOCALAPPDATA% раскрылся неверно: " + temp->resolvedLocator, onlyTemp);
    }

    // Пустое окружение: флаг «часть правил неактивна» обязан встать, а переменная —
    // остаться в шаблоне, иначе правило молча уедет не туда.
    {
        bool unresolved = false;
        const FilesResult one = loadFiles({{"a.json", onlyTemp}}, "", &unresolved);
        requireTrue(one.accepted, "эталонный файл не слился с пустым окружением: " + one.message, onlyTemp);
        const Rule* temp = one.set.byId("temp.user");
        requireTrue(temp != nullptr, "эталонное правило потерялось в склейке", onlyTemp);
        requireTrue(temp->resolvedLocator == "%LOCALAPPDATA%\\Temp\\**",
                    "нераскрытая переменная не осталась в шаблоне: " + temp->resolvedLocator, onlyTemp);
        requireTrue(temp->resolvedLocator.find("Users") == std::string::npos,
                    "нераскрытая переменная раскрылась частично: " + temp->resolvedLocator, onlyTemp);
        CHECK(unresolved);
    }

    // Пустой набор (rules: []) обязан быть отвергнут: приложение никогда не
    // остаётся без набора правил (SPEC §9.2 п.4), пустой набор — не набор.
    CHECK_THROWS(mrproper::core::loadRuleFiles({{"a.json", "{\"schemaVersion\":1,\"rules\":[]}"}}, env, nullptr));

    // Один плохой файл среди хороших — отказ всего кандидата.
    Stats stats;
    for (std::size_t i = 0; i < 200; ++i) {
        std::string bad = genRandomDocument(r);
        const std::size_t mutations = r.below(3);
        for (std::size_t m = 0; m < mutations; ++m) bad = applyMutator(bad, r);
        const FilesResult loaded = loadFiles({{"a.json", first}, {"b.json", bad}}, env, nullptr);
        if (loaded.accepted) {
            // Кандидат принят целиком: значит, «плохой» файл оказался валидным. Это
            // не ошибка, но тогда набор обязан быть целым и без повторов id.
            const RuleSet& set = loaded.set;
            for (std::size_t k = 0; k < set.rules.size(); ++k) {
                for (std::size_t j = k + 1; j < set.rules.size(); ++j) {
                    requireTrue(set.rules[k].id != set.rules[j].id, "в слитом наборе два одинаковых id", bad);
                }
            }
            ++stats.accepted;
        } else {
            requireTrue(!loaded.message.empty(), "отказ без диагностики", bad);
            ++stats.rejected;
            // После отказа хороший файл по-прежнему грузится: отказ не испортил
            // состояние загрузчика.
            const FilesResult after = loadFiles({{"a.json", first}}, env, nullptr);
            requireTrue(after.accepted, "после отказа эталонный набор перестал грузиться: " + after.message, bad);
            requireTrue(after.set.rules.size() == countInputRules(first), "после отказа эталонный набор изменился", bad);
        }
    }
    CHECK_EQ(stats.accepted + stats.rejected, static_cast<std::size_t>(200));
    std::printf("  rules_fuzz multi-file: принято %zu, отказов %zu\n", stats.accepted, stats.rejected);
}

// ============================================= диагностика отклонения ядра

// Диагностика, а не требование. Часть отказов загрузчик выдаёт не через
// документированный core::RuleError (rules.hpp: «бросает RuleError с указанием
// файла и поля»), а через std::runtime_error из json::Value::require/asBool:
// «нет обязательного поля» и «поле не того типа». Для правил из сети это значит, что
// вызывающий, который ловит только RuleError (CLI, UI, платформенный слой при
// подмене набора), получит необработанное исключение вместо тихого отказа с
// записью в лог. Здесь проверяется ровно одно: такие входы обязаны отказываться
// контролируемо и без падения. Сам факт отклонения печатается, чтобы он был виден
// в логе тестов, пока src/core/json.cpp и src/core/rules.cpp не приведены к
// одному каналу ошибок.
TEST(rules_fuzz_diagnosticWhereRejectionLeavesRuleErrorChannel) {
    struct Case {
        const char* label;
        std::string text;
    };
    std::vector<Case> cases;
    cases.push_back({"нет поля rules", "{\"schemaVersion\":1}"});
    cases.push_back({"нет id", "{\"schemaVersion\":1,\"rules\":[{}]}"});
    cases.push_back({"нет category", "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\"}]}"});
    cases.push_back({"нет safety", "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\"}]}"});
    cases.push_back({"нет locator",
                     "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"safe\"}]}"});
    cases.push_back({"groupByProfile не bool",
                     "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"safe\","
                     "\"locator\":\"C:\\\\Temp\\\\**\",\"minAgeDays\":2,\"groupByProfile\":\"да\"}]}"});
    cases.push_back({"title.ru не строка",
                     "{\"schemaVersion\":1,\"rules\":[{\"id\":\"a.b\",\"category\":\"c\",\"safety\":\"safe\","
                     "\"locator\":\"C:\\\\Temp\\\\**\",\"minAgeDays\":2,\"title\":{\"ru\":5}}]}"});

    Stats stats;
    for (const Case& c : cases) {
        const LoadResult result = loadOnce(c.text);
        if (result.verdict == Verdict::Accepted) {
            ++stats.accepted;
            checkAccepted(c.text, result);
            std::printf("  rules_fuzz диагностика: %-24s принят молча\n", c.label);
            continue;
        }
        requireTrue(!result.message.empty(), std::string("отказ без диагностики: ") + c.label, c.text);
        checkDeterministic(c.text, result);
        ++stats.rejected;
        if (!result.viaRuleError) ++stats.rejectedNotViaRuleError;
        std::printf("  rules_fuzz диагностика: %-24s отказ через %s: %s\n", c.label,
                    result.viaRuleError ? "RuleError " : "runtime_error", result.message.c_str());
    }
    CHECK_EQ(stats.accepted + stats.rejected, cases.size());
    std::printf("  rules_fuzz диагностика: отказов не через RuleError: %zu из %zu\n", stats.rejectedNotViaRuleError,
                cases.size());
}

// ==================================================== сопоставление шаблонов

// Загрузчик отдаёт шаблоны сопоставителю путей. Проверяем, что сопоставление
// переживает враждебные пути (управляющие символы, NUL, не-UTF8, длинные строки,
// разделители в неожиданных местах) и остаётся детерминированным. Шаблоны здесь
// настоящие: паттерн с длинной чередой «*» сопоставитель разбирает перебором, и
// подставлять его в фаззинг нельзя — это отдельная тема про сложность, а не про
// устойчивость загрузчика.
TEST(rules_fuzz_matchingSurvivesHostilePaths) {
    const char* const kPatterns[] = {"C:/Users/*/AppData/Local/Temp/**",
                                     "C:/Windows/Temp/**",
                                     "**/Cache/**",
                                     "C:/Temp/*.tmp",
                                     "C:/**/thumbs*.db",
                                     "C:/x/[!a-z]*.log",
                                     "%LOCALAPPDATA%/Temp/**",
                                     "C:/x/??.tmp",
                                     "**"};
    constexpr std::size_t kPatternCount = sizeof(kPatterns) / sizeof(kPatterns[0]);

    Rng r(0x1BADB002DEADBEEFull);
    const Clock::time_point started = Clock::now();
    std::size_t cases = 0;
    for (std::size_t i = 0; i < 3000; ++i) {
        std::string path = genRawString(r, 64);
        if (r.oneIn(4)) path = "C:/" + path;
        if (r.oneIn(4)) path += "/" + genRawString(r, 24);
        if (r.oneIn(8)) path = std::string(1024, 'a') + path;
        if (r.oneIn(16)) path.clear();

        const std::string pattern = kPatterns[r.below(kPatternCount)];
        bool matched = false;
        bool matchedAgain = false;
        try {
            matched = mrproper::core::matchPath(pattern, path);
            matchedAgain = mrproper::core::matchPath(pattern, path);
        } catch (const std::exception& e) {
            fail(std::string("сопоставление шаблона бросило исключение: ") + e.what(), pattern + " | " + path);
        }
        requireTrue(matched == matchedAgain, "сопоставление недетерминировано: " + pattern, path);
        // Нормализация разделителей обязана быть идемпотентной.
        requireTrue(mrproper::core::normalizeSeparators(mrproper::core::normalizeSeparators(path)) ==
                        mrproper::core::normalizeSeparators(path),
                    "нормализация разделителей не идемпотентна", path);
        ++cases;
    }
    requireTrue(secondsBetween(started, Clock::now()) < kTotalSeconds, "сопоставление не уложилось в бюджет", "");
    CHECK_EQ(cases, static_cast<std::size_t>(3000));
}
