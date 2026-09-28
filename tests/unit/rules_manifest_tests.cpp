// Тесты согласованности набора правил очистки.
//
// Что проверяем (задача 28; спека §4 FR-4 «правила — данные», §9.2 формат
// набора и его проверка, §9 tests/unit — «core, не требует Windows»):
//   1) каждый файл правил разбирается строго (неизвестное поле, битый JSON,
//      чужая версия схемы — ошибка);
//   2) id правил уникальны во всём наборе и следуют соглашению проекта
//      «id = <категория>.<вариант>», категория — строго ID из SPEC §4 FR-3;
//   3) покрыты все категории FR-3 и выдержан минимальный размер набора;
//   4) манифест rules/manifest.json совпадает с файлами на диске: тот же
//      состав, те же SHA-256 и те же размеры (SPEC §9.2, формат набора);
//   5) сверка не пустая: подмена байта, чужой хеш, недостающий и лишний файл
//      ловятся теми же функциями ядра, что и в бою. Правила управляют
//      удалением файлов (ADR-008), поэтому «зелёный» тест на неработающей
//      сверке опаснее красного.
//
// Разделение намеренное. Пункты 1-4 проверяются напрямую по данным на диска —
// это и есть предмет задачи. Пункт 5 и отдельный тест проходят набор через
// штатный конвейер ядра (parseManifest → verifyRuleSet → loadVerifiedRuleSet):
// так тест показывает не только «данные согласованы», но и «ядро умеет этот
// набор применить». Расходятся эти два слоя — значит, чинить надо ядро, а не
// подправлять тест под обход.
//
// Каталог rules/ ищется в рантайме: CMake-определение в этот файл не входит
// (владение файлом у задачи 28), поэтому корень репозитория определяется по
// __FILE__ и по текущему каталогу. Переменные окружения MRPROPER_REPO_ROOT и
// MRPROPER_RULES_DIR позволяют указать набор явно (CI, проверка чужого набора).
#include "harness.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "rules.hpp"
#include "rulesync.hpp"

using mrproper::json::Value;
using namespace mrproper::core;

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------- SPEC §4 FR-3
//
// Категории из таблицы FR-3 — эталон покрытия. Держим списком в коде, а не
// разбором markdown: тест не должен зависеть от парсера SPEC, но требование
// обязано быть видно. Новая категория в SPEC → строка здесь.
const std::vector<std::string>& specCategories() {
    static const std::vector<std::string> kCategories = {
        "temp.user",       "temp.system",   "recycle.bin",     "browser.cache",   "browser.history",
        "firefox.cache",   "ms.update",     "delivery.opt",    "winsxs.report",   "prefetch",
        "crash.dumps",     "wer",           "logs",            "icon.font.cache", "shadercache",
        "installer.cache", "memory.dumps",  "thumbnails",      "npm.pip.cache",   "user.bigfiles",
    };
    return kCategories;
}

// FR-3 для thumbnails гласит «если не в icon.font» — категория слита с
// icon.font.cache, и это зафиксировано в самих правилах (note у
// icon.font.cache.explorer.thumbnails). Поэтому thumbnails не обязана быть
// отдельной категорией, но её glob-прицел обязан встречаться в правилах
// icon.font.cache — иначе миниатюры просто никто не чистит.
const char* kThumbnailsCategory = "thumbnails";
const char* kThumbnailsHostCategory = "icon.font.cache";
const char* kThumbnailsNeedle = "thumbcache";

// Минимальный набор: хотя бы по одному правилу на каждую строку FR-3. Порог
// выводится из списка категорий, а не выдумывается числом: SPEC меняется —
// меняется и планка.
std::size_t minimumRuleCount() { return specCategories().size(); }

// Окружение для подстановки в locator'ы. Не настоящее: правила — данные, и
// тест не должен зависеть от машины, на которой запущен. Состав переменных
// взят из фактического набора (rules/*.json используют ровно эти).
const char* kEnvDump = "LOCALAPPDATA=C:\\Users\\Tester\\AppData\\Local\n"
                      "APPDATA=C:\\Users\\Tester\\AppData\\Roaming\n"
                      "USERPROFILE=C:\\Users\\Tester\n"
                      "TEMP=C:\\Users\\Tester\\AppData\\Local\\Temp\n"
                      "ProgramData=C:\\ProgramData\n"
                      "SystemDrive=C:\n"
                      "ProgramFiles=C:\\Program Files\n";

// Версия приложения, под которую применяется набор (SPEC §3: MVP = v1.0).
const char* kAppVersion = "1.0.0";

// ------------------------------------------------------------------ утилиты

[[noreturn]] void fail(const std::string& what) { throw mrp::Failure{what}; }

void require(bool ok, const std::string& what) {
    if (!ok) fail(what);
}

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

// Значение переменной окружения или пустая строка. getenv помечен в MSVC как
// устаревший (C4996), а проект собирается с /WX (SPEC §9.1, ADR-001), поэтому
// на Windows берём _dupenv_s; на других хостах — обычный getenv, чтобы тот же
// тест можно было собрать без Windows.
std::string envValue(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
#endif
}

std::string readAllBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("не читается файл " + path.string());
    return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// Каталог с корневыми файлами репозитория: есть CMakeLists.txt и есть rules/.
bool looksLikeRepoRoot(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::exists(dir / "CMakeLists.txt", ec) &&
           fs::is_directory(dir / "rules", ec);
}

fs::path findRepoRoot() {
    std::error_code ec;
    const std::string from_env = envValue("MRPROPER_REPO_ROOT");
    if (!from_env.empty()) {
        const fs::path candidate{from_env};
        if (looksLikeRepoRoot(candidate)) return candidate;
    }
    // От __FILE__: MSVC получает абсолютный путь исходника, и это надёжнее
    // текущего каталога (tools\test.bat запускает exe из корня, но полагаться
    // на это в тесте нельзя).
    for (fs::path dir = fs::path{__FILE__}.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (looksLikeRepoRoot(dir)) return dir;
        if (dir == dir.parent_path()) break;
    }
    for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
        if (looksLikeRepoRoot(dir)) return dir;
        if (dir == dir.parent_path()) break;
    }
    return {};
}

fs::path repoRoot() {
    static const fs::path kRoot = findRepoRoot();
    return kRoot;
}

fs::path rulesDir() {
    const std::string from_env = envValue("MRPROPER_RULES_DIR");
    if (!from_env.empty()) return fs::path{from_env};
    return repoRoot() / "rules";
}

struct RuleSource {
    std::string name;  // имя файла внутри rules/ — ровно как в манифесте
    fs::path path;
    std::string bytes;
};

// Файлы правил: *.json, кроме манифеста (он проверяется отдельно) и всего
// не-JSON (rules.sig — подпись манифеста, элементом набора не является).
std::vector<fs::path> listRuleFiles() {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(rulesDir(), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (path.extension() != ".json") continue;
        if (path.filename() == "manifest.json") continue;
        files.push_back(path);
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b) { return a.generic_string() < b.generic_string(); });
    return files;
}

// Один проход по диску на процесс: тесты остаются быстрыми (SPEC §8 — «зелёный
// ctest за секунды»), а расхождение данных между тестами исключено.
const std::vector<RuleSource>& ruleSources() {
    static const std::vector<RuleSource> kSources = [] {
        std::vector<RuleSource> sources;
        for (const fs::path& path : listRuleFiles()) {
            sources.push_back(RuleSource{path.filename().generic_string(), path, readAllBytes(path)});
        }
        return sources;
    }();
    return kSources;
}

// Правила всех файлов, собранные без validateRuleSet: анализ данных (id,
// покрытие категорий, количество) не должен зависеть от того, как именно ядро
// решает вопрос о «слишком широком» правиле. Строгий разбор каждого файла
// здесь уже сделан — отбрасываются только правила, принятые ядром условно.
const std::vector<Rule>& analysedRules() {
    static const std::vector<Rule> kRules = [] {
        std::vector<Rule> rules;
        for (const RuleSource& source : ruleSources()) {
            const RuleSet part = loadRuleFile(source.bytes, source.name);
            rules.insert(rules.end(), part.rules.begin(), part.rules.end());
        }
        return rules;
    }();
    return kRules;
}

// Соглашение об id: строчные буквы, цифры и разделители . _ - (те же правила,
// что у isSafeId в core::rules), плюс форма «сегмент.сегмент» без пустых
// сегментов — это уже соглашение набора, а не синтаксис JSON.
bool isWellFormedId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    if (id.front() == '.' || id.back() == '.') return false;
    if (id.find("..") != std::string::npos) return false;
    for (const char c : id) {
        const auto byte = static_cast<unsigned char>(c);
        const bool ok = std::islower(byte) != 0 || std::isdigit(byte) != 0 || c == '.' || c == '_' ||
                        c == '-';
        if (!ok) return false;
    }
    return true;
}

const FileVerdict* verdictFor(const RuleSetVerification& report, const std::string& path) {
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

bool problemsMention(const RuleSetVerification& report, const std::string& needle) {
    for (const std::string& problem : report.problems) {
        if (problem.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Манифест набора (SPEC §9.2). nullptr, если файла нет: он создаётся отдельной
// задачей, и ронять общий прогон из-за чужого ещё не дописанного артефакта
// нельзя — но молча проходить тоже нельзя, поэтому печатается заметная строка.
const std::string* manifestBytes() {
    static const std::string kBytes = [] {
        std::error_code ec;
        const fs::path manifest = rulesDir() / "manifest.json";
        if (!fs::exists(manifest, ec)) return std::string{};
        try {
            return readAllBytes(manifest);
        } catch (const mrp::Failure&) {
            return std::string{};
        }
    }();
    static bool warned = false;
    if (kBytes.empty()) {
        if (!warned) {
            warned = true;
            std::printf("  [SKIP] rules/manifest.json отсутствует — проверки согласованности "
                        "манифеста не выполнялись (создаёт отдельная задача)\n");
        }
        return nullptr;
    }
    return &kBytes;
}

std::vector<VerifiedFile> verifiedRuleFiles() {
    std::vector<VerifiedFile> files;
    files.reserve(ruleSources().size());
    for (const RuleSource& source : ruleSources()) {
        files.push_back(makeVerifiedFile(source.name, source.bytes));
    }
    return files;
}

// Политика для проверок ядра: подпись (Ed25519) проверяет платформенный слой
// (ADR-008, приватный ключ офлайн у владельца), а «файл вне манифеста» —
// прямой запрет, он и есть предмет этих тестов.
VerificationPolicy corePolicy(bool requireKnownFilesOnly) {
    VerificationPolicy policy;
    policy.requireSignature = false;
    policy.requireKnownFilesOnly = requireKnownFilesOnly;
    return policy;
}

std::string syntheticRuleFile(const std::string& id) {
    return std::string{R"json({"schemaVersion":1,"rules":[{"id":")json"} + id +
           R"json(","category":"temp.user","safety":"safe","locator":"%TEMP%/*",)"json"
           R"json("minAgeDays":1,"title":{"ru":"тест","en":"test"}}]})json";
}

// Минимальный манифест для негативных проверок.
std::string syntheticManifest(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out = R"({"schema":1,"version":"2026.02.1-test","minAppVersion":"1.0.0","files":[)";
    bool first = true;
    for (const auto& [path, hash] : files) {
        if (!first) out += ",";
        first = false;
        out += R"({"path":")" + path + R"(","sha256":")" + hash + R"("})";
    }
    out += "]}";
    return out;
}

// minAppVersion необязателен; пустая строка — «ограничений нет» (SPEC §9.2).
// Поле есть, но не строка — ошибка формата, а не «нет ограничений».
std::string manifestMinAppVersion(const Value& document) {
    const Value* node = document.find("minAppVersion");
    if (node == nullptr) return {};
    require(node->isString(), "rules/manifest.json: minAppVersion должен быть строкой");
    return node->asString();
}

}  // namespace

// ------------------------------------------------------- поиск набора правил

TEST(rules_manifest_testFindsRuleSetOnDisk) {
    require(!repoRoot().empty(),
            "корень репозитория не найден: нет каталога с CMakeLists.txt и rules/ (проверьте "
            "MRPROPER_REPO_ROOT или MRPROPER_RULES_DIR)");
    require(fs::is_directory(rulesDir()), "каталог rules/ не найден: " + rulesDir().string());
    require(!ruleSources().empty(), "в rules/ нет ни одного файла правил (*.json)");
    for (const RuleSource& source : ruleSources()) {
        require(!source.bytes.empty(), "файл правил пуст: " + source.name);
    }
}

// ------------------------------------------------------- 1) разбор набора

TEST(rules_manifest_allRuleFilesParseStrictly) {
    require(!ruleSources().empty(), "набор правил пуст — нечего проверять");
    std::size_t total = 0;
    for (const RuleSource& source : ruleSources()) {
        // Строгий разбор: неизвестное поле или битый JSON — ошибка (SPEC §9.2 п.3).
        const RuleSet part = loadRuleFile(source.bytes, source.name);
        require(part.schemaVersion == 1, source.name + ": schemaVersion должен быть 1, получено " +
                                              std::to_string(part.schemaVersion));
        require(!part.rules.empty(), source.name + ": файл не содержит ни одного правила");
        total += part.rules.size();
    }
    require(total == analysedRules().size(),
            "разбор файлов дал " + std::to_string(total) + " правил, анализ набора видел " +
                std::to_string(analysedRules().size()));
}

// ------------------------------------------- 2) уникальность и форма id

TEST(rules_manifest_ruleIdsAreUniqueAndWellFormed) {
    std::map<std::string, std::string> owner;  // id → файл-владелец
    for (const RuleSource& source : ruleSources()) {
        const RuleSet part = loadRuleFile(source.bytes, source.name);
        for (const Rule& rule : part.rules) {
            require(isWellFormedId(rule.id),
                    source.name + ": недопустимый id " + quoted(rule.id) +
                        " (строчные буквы, цифры, разделители . _ -, без пустых сегментов)");
            require(isWellFormedId(rule.category),
                    source.name + ": недопустимая category " + quoted(rule.category) + " у правила " +
                        rule.id);
            // Соглашение проекта: id = <категория>.<вариант>; равенство категории
            // тоже допустимо — так описано правило на категорию целиком.
            require(rule.id == rule.category || rule.id.rfind(rule.category + ".", 0) == 0,
                    source.name + ": id " + quoted(rule.id) + " не принадлежит своей категории " +
                        quoted(rule.category));
            const auto [it, inserted] = owner.emplace(rule.id, source.name);
            require(inserted, "дублирующийся id " + quoted(rule.id) + ": " + it->second + " и " +
                                  source.name);
        }
    }
    require(owner.size() == analysedRules().size(),
            "уникальных id " + std::to_string(owner.size()) + ", а правил " +
                std::to_string(analysedRules().size()));
}

// ------------------------------------- 3) покрытие категорий и минимум правил

TEST(rules_manifest_coversEverySpecCategory) {
    std::set<std::string> present;
    for (const Rule& rule : analysedRules()) present.insert(rule.category);

    for (const std::string& category : specCategories()) {
        if (present.count(category) != 0) continue;
        if (category != kThumbnailsCategory) {
            fail("категория " + quoted(category) + " из SPEC §4 FR-3 не имеет ни одного правила");
        }
        // Слитая категория: прицел должен покрываться правилами icon.font.cache.
        bool covered = false;
        for (const Rule& rule : analysedRules()) {
            if (rule.category != kThumbnailsHostCategory) continue;
            if (rule.locator.find(kThumbnailsNeedle) != std::string::npos ||
                rule.titleRu.find("миниатюр") != std::string::npos) {
                covered = true;
                break;
            }
        }
        require(covered, "категория " + quoted(category) + " не слита с " +
                             quoted(kThumbnailsHostCategory) + ": нет правила с " +
                             quoted(kThumbnailsNeedle) + " в locator");
    }
}

TEST(rules_manifest_minimumRuleCount) {
    require(analysedRules().size() >= minimumRuleCount(),
            "в наборе " + std::to_string(analysedRules().size()) + " правил, минимум по FR-3 — " +
                std::to_string(minimumRuleCount()));

    // Минимум — не только общий счёт: у каждой категории FR-3 должно быть хотя
    // бы одно правило. Это и есть «минимум правил» из задачи.
    std::map<std::string, std::size_t> perCategory;
    for (const Rule& rule : analysedRules()) ++perCategory[rule.category];
    for (const std::string& category : specCategories()) {
        if (category == kThumbnailsCategory) continue;
        const auto it = perCategory.find(category);
        require(it != perCategory.end() && it->second > 0,
                "у категории " + quoted(category) + " нет ни одного правила");
    }
}

// ---------------------------------------------------- 4) манифест ↔ файлы

TEST(rules_manifest_manifestDeclaresEveryRuleFileWithMatchingHash) {
    const std::string* bytes = manifestBytes();
    if (bytes == nullptr) return;  // предупреждение о пропуске уже напечатано

    // Манифест разбирается здесь же, минимальным JSON ядра: это проверка данных
    // (состав, хеши, размеры), а не проверка парсера манифеста — для парсера
    // есть отдельный тест ниже.
    const Value document = mrproper::json::parse(*bytes);
    require(document.isObject(), "rules/manifest.json: корень должен быть объектом");

    const Value* version = document.find("version");
    require(version != nullptr && version->isString() && !version->asString().empty(),
            "rules/manifest.json: обязательна непустая строковая версия набора (SPEC §9.2)");

    // Схема: ключ называется и «schema» (так в SPEC §9.2), и «schemaVersion»
    // (так в файлах правил) — два имени одного поля.
    const Value* schema = document.find("schema");
    const Value* schemaVersion = document.find("schemaVersion");
    for (const Value* node : {schema, schemaVersion}) {
        require(node == nullptr || node->isNumber(),
                "rules/manifest.json: версия схемы должна быть числом");
    }
    if (const Value* schemaNode = schema != nullptr ? schema : schemaVersion; schemaNode != nullptr) {
        const double raw = schemaNode->asNumber();
        require(raw >= 0.0 && raw <= 2147483647.0 && static_cast<double>(static_cast<int>(raw)) == raw &&
                    static_cast<int>(raw) == supportedManifestSchemaVersion(),
                "rules/manifest.json: схема " + std::to_string(static_cast<int>(raw)) +
                    " не поддерживается, ожидалась " + std::to_string(supportedManifestSchemaVersion()));
    }
    require(isAppVersionSupported(manifestMinAppVersion(document), kAppVersion),
            "rules/manifest.json: minAppVersion " + quoted(manifestMinAppVersion(document)) +
                " выше версии приложения " + kAppVersion);

    const Value* files = document.find("files");
    require(files != nullptr && files->isArray(), "rules/manifest.json: files должен быть массивом");
    require(!files->items().empty(), "rules/manifest.json: пустой список files");

    std::set<std::string> listed;
    std::map<std::string, std::string> declaredHash;
    std::map<std::string, std::uint64_t> declaredSize;
    for (const Value& item : files->items()) {
        require(item.isObject(), "rules/manifest.json: элемент files должен быть объектом");
        const Value* path = item.find("path");
        const Value* hash = item.find("sha256");
        require(path != nullptr && path->isString(), "rules/manifest.json: у файла нет строкового path");
        const std::string name = path->asString();
        require(!name.empty() && name.front() != '/' && name.front() != '\\' &&
                    name.find(':') == std::string::npos && name.find("..") == std::string::npos,
                "rules/manifest.json: недопустимый путь " + quoted(name) +
                    " (нужен относительный путь внутри набора)");
        // Манифест не может перечислять сам себя: он и есть подписываемый документ.
        require(name != "manifest.json", "rules/manifest.json: перечисляет сам себя");
        require(listed.insert(name).second, "rules/manifest.json: путь " + quoted(name) + " указан дважды");

        require(hash != nullptr && hash->isString(), name + ": в манифесте нет строкового sha256");
        Sha256Digest digest{};
        require(parseSha256Hex(hash->asString(), digest),
                name + ": ожидается SHA-256 — 64 hex-символа, получено " + quoted(hash->asString()));
        declaredHash.emplace(name, toHex(digest));

        if (const Value* size = item.find("size"); size != nullptr) {
            require(size->isNumber(), name + ": size должен быть числом");
            const double raw = size->asNumber();
            require(raw >= 0.0 && raw <= 9007199254740992.0 &&
                        static_cast<double>(static_cast<std::uint64_t>(raw)) == raw,
                    name + ": size должен быть неотрицательным целым числом");
            declaredSize.emplace(name, static_cast<std::uint64_t>(raw));
        }
    }

    // Состав в обе стороны: файл на диске без записи в манифесте — правило,
    // которое никто не подписывал, а удалять по нему будут (SPEC §9.2 п.3).
    for (const RuleSource& source : ruleSources()) {
        require(listed.count(source.name) != 0,
                "файл правил " + quoted(source.name) +
                    " есть на диске, но не перечислен в манифесте: по нему будут удалять, а никто его "
                    "не подписал");
    }
    for (const std::string& name : listed) {
        std::error_code ec;
        require(fs::is_regular_file(rulesDir() / name, ec),
                "манифест ссылается на отсутствующий файл " + quoted(name));
    }
    require(listed.size() == ruleSources().size(),
            "манифест перечисляет " + std::to_string(listed.size()) + " файлов, на диске " +
                std::to_string(ruleSources().size()));

    // Хеши и размеры — байт в байт с тем, что лежит в каталоге.
    for (const RuleSource& source : ruleSources()) {
        const std::string actual = sha256Hex(source.bytes);
        const auto expected = declaredHash.find(source.name);
        require(expected != declaredHash.end() && expected->second == actual,
                "SHA-256 файла " + quoted(source.name) + " не совпадает с манифестом: в манифесте " +
                    (expected == declaredHash.end() ? std::string{"нет записи"}
                                                    : expected->second) +
                    ", фактически " + actual);
        const auto size = declaredSize.find(source.name);
        if (size != declaredSize.end()) {
            const auto actualSize = static_cast<std::uint64_t>(source.bytes.size());
            require(size->second == actualSize,
                    "размер файла " + quoted(source.name) + " не совпадает с манифестом: в манифесте " +
                        std::to_string(size->second) + ", фактически " + std::to_string(actualSize));
        }
    }
}

TEST(rules_manifest_coreParsesManifestAndVerifiesFiles) {
    const std::string* bytes = manifestBytes();
    if (bytes == nullptr) return;

    // Тот же набор через штатный разбор ядра: если манифест на диске корректен,
    // а parseManifest его отвергает — это дефект разбора, а не данных, и
    // приложение не сможет применить собственный набор (SPEC §9.2 п.3).
    Manifest manifest;
    try {
        manifest = parseManifest(*bytes, "rules/manifest.json");
    } catch (const RuleSyncError& e) {
        fail(std::string{"ядро не разобрало корректный rules/manifest.json: "} + e.what());
    }
    require(manifest.schemaVersion == supportedManifestSchemaVersion(),
            "манифест: схема " + std::to_string(manifest.schemaVersion) + " не поддерживается");
    require(!manifest.version.empty(), "манифест: пустая версия набора");
    require(!manifest.files.empty(), "манифест: пустой список files");

    const RuleSetVerification report =
        verifyRuleSet(*bytes, "", verifiedRuleFiles(), kAppVersion, corePolicy(true));
    if (!report.ok()) {
        fail("verifyRuleSet отверг собственный набор правил: " + report.summary() + " [" +
             describeChecks(report) + "]");
    }
    require(report.files.size() == verifiedRuleFiles().size(),
            "проверено " + std::to_string(report.files.size()) + " файлов из " +
                std::to_string(verifiedRuleFiles().size()));
}

TEST(rules_manifest_realSetPassesFullVerificationPipeline) {
    const std::string* bytes = manifestBytes();
    if (bytes == nullptr) return;

    // Шаги SPEC §9.2 п.3 целиком: манифест разобран, целостность подтверждена,
    // файлы разобраны парсером правил. Набор, который нельзя применить, —
    // нерабочий набор, даже если каждый файл по отдельности выглядит верным.
    const RuleSetVerification report =
        verifyRuleSet(*bytes, "", verifiedRuleFiles(), kAppVersion, corePolicy(true));
    if (!report.ok()) {
        fail("набор из rules/ не проходит проверку целостности: " + report.summary() + " [" +
             describeChecks(report) + "]");
    }

    std::vector<std::pair<std::string, std::string>> contents;
    contents.reserve(ruleSources().size());
    for (const RuleSource& source : ruleSources()) contents.emplace_back(source.name, source.bytes);
    bool unresolved = false;
    const RuleSet applied = loadVerifiedRuleSet(report, contents, kEnvDump, &unresolved);
    require(!unresolved, "часть правил набора не раскрыла переменные окружения");
    require(!applied.rules.empty(), "проверенный набор оказался пустым");
    require(applied.size() == analysedRules().size(),
            "применяемый набор содержит " + std::to_string(applied.size()) + " правил вместо " +
                std::to_string(analysedRules().size()));
}

TEST(rules_manifest_coreAcceptsMergedSetOfRealFiles) {
    // Тот же путь, каким правила грузит приложение: слияние всех файлов плюс
    // validateRuleSet. Набор из репозитория обязан быть загружаемым — иначе
    // сканер не увидит ни одного правила.
    std::vector<std::pair<std::string, std::string>> files;
    files.reserve(ruleSources().size());
    for (const RuleSource& source : ruleSources()) files.emplace_back(source.name, source.bytes);
    bool unresolved = false;
    try {
        const RuleSet set = loadRuleFiles(files, kEnvDump, &unresolved);
        require(!set.rules.empty(), "loadRuleFiles вернул пустой набор");
    } catch (const RuleError& e) {
        fail(std::string{"ядро не принимает собственный набор правил: "} + e.what());
    }
}

// ------------------------------------------------------- 5) сверка живая

TEST(rules_manifest_sha256ImplementationIsTrustworthy) {
    // Модуль, который решает, что удалять, не вправе молча доверять своей
    // криптографии: сверяем с эталоном FIPS 180-4.
    require(sha256SelfTest(), "sha256SelfTest() не прошёл — хешам манифеста доверять нельзя");
    require(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "SHA-256 пустой строки неверен");
    require(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 строки \"abc\" неверен");

    // Путь сравнения, которым пользуются проверки манифеста: hex из манифеста →
    // байты → обратно в hex. Регистр не должен влиять на результат.
    Sha256Digest digest{};
    const std::string lower = sha256Hex("abc");
    std::string upper = lower;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    require(parseSha256Hex(upper, digest), "parseSha256Hex отверг корректный hex в верхнем регистре");
    require(toHex(digest) == lower, "hex нормализуется не в нижний регистр: " + toHex(digest));
    Sha256Digest broken{};
    require(!parseSha256Hex("zz", broken), "parseSha256Hex принял не-hex");
    require(!parseSha256Hex(lower.substr(0, 63), broken), "parseSha256Hex принял обрезанный хеш");

    // Инкрементальный хеш обязан совпадать с однопроходным: файлы правил
    // читаются платформенным слоем кусками.
    Sha256 chunked;
    chunked.update("a", 1);
    chunked.update("bc", 2);
    require(toHex(chunked.finish()) == lower, "инкрементальный SHA-256 разошёлся с однопроходным");
}

TEST(rules_manifest_detectsTamperedFileContent) {
    const std::string good = syntheticRuleFile("temp.user.probe");
    const std::string tampered = good + "\n";  // лишний байт — хеш обязан измениться
    const std::string manifest = syntheticManifest({{"temp.user.json", sha256Hex(good)}});
    const VerificationPolicy policy = corePolicy(true);

    // 1) честный набор проходит.
    const RuleSetVerification honest =
        verifyRuleSet(manifest, "", {makeVerifiedFile("temp.user.json", good)}, kAppVersion, policy);
    require(honest.ok(), "эталонный набор не прошёл проверку: " + honest.summary() + " [" +
                             describeChecks(honest) + "]");
    // 2) подмена байта ловится и называет файл.
    const RuleSetVerification broken =
        verifyRuleSet(manifest, "", {makeVerifiedFile("temp.user.json", tampered)}, kAppVersion, policy);
    require(!broken.ok(), "изменённый файл принят как целый — сверка хешей не работает");
    require(problemsMention(broken, "temp.user.json"),
            "в отказе не назван файл: " + broken.summary() + " [" + describeChecks(broken) + "]");
    const FileVerdict* verdict = verdictFor(broken, "temp.user.json");
    require(verdict != nullptr && verdict->check == FileCheck::HashMismatch,
            "ожидался HashMismatch по temp.user.json, получено: " + describeChecks(broken));

    // 3) объявленный размер сверяется отдельно от хеша.
    const std::string withSize = std::string{R"({"schema":1,"version":"t","files":[{"path":"temp.user.json","sha256":")"} +
                             sha256Hex(good) + R"(","size":1}]})";
    const RuleSetVerification sized =
        verifyRuleSet(withSize, "", {makeVerifiedFile("temp.user.json", good)}, kAppVersion, policy);
    require(!sized.ok(), "объявленный размер 1 при реальном размере " +
                             std::to_string(good.size()) + " принят как совпадающий");
    const FileVerdict* sizeVerdict = verdictFor(sized, "temp.user.json");
    require(sizeVerdict != nullptr && sizeVerdict->check == FileCheck::SizeMismatch,
            "ядро не сообщило SizeMismatch (разбор манифеста с полем size) — проверьте requireCount "
            "в src/core/rulesync.cpp: " + sized.summary());
}

TEST(rules_manifest_detectsUnlistedAndMissingFiles) {
    const std::string present = syntheticRuleFile("temp.user.probe");
    const std::string absent = syntheticRuleFile("logs.system.probe");
    const std::string extra = syntheticRuleFile("wer.probe");
    const std::string manifest = syntheticManifest({{"logs.json", sha256Hex(absent)},
                                                    {"temp.user.json", sha256Hex(present)}});
    const std::vector<VerifiedFile> onlyPresent = {makeVerifiedFile("temp.user.json", present)};

    // Обязательный файл отсутствует — это отказ при любой политике.
    const RuleSetVerification missing =
        verifyRuleSet(manifest, "", onlyPresent, kAppVersion, corePolicy(false));
    require(!missing.ok(), "набор с недостающим файлом принят");
    require(problemsMention(missing, "logs.json"),
            "в отказе не назван отсутствующий файл: " + missing.summary());
    const FileVerdict* absentVerdict = verdictFor(missing, "logs.json");
    require(absentVerdict != nullptr && absentVerdict->check == FileCheck::Missing,
            "ожидался Missing по logs.json, получено: " + describeChecks(missing));

    // Лишний файл, не перечисленный в манифесте, — отказ: по нему будут
    // удалять, а никто его не подписывал.
    const std::string shortManifest = syntheticManifest({{"temp.user.json", sha256Hex(present)}});
    std::vector<VerifiedFile> withExtra = onlyPresent;
    withExtra.push_back(makeVerifiedFile("wer.json", extra));
    const RuleSetVerification unlisted =
        verifyRuleSet(shortManifest, "", withExtra, kAppVersion, corePolicy(true));
    require(!unlisted.ok(), "файл вне манифеста принят как доверенный");
    require(problemsMention(unlisted, "wer.json"),
            "в отказе не назван лишний файл: " + unlisted.summary());

    // При мягкой политике лишний файл перестаёт быть отказом, но остаётся
    // помеченным Unlisted — запись для журнала (SPEC §9.2 п.4).
    const RuleSetVerification reported =
        verifyRuleSet(shortManifest, "", withExtra, kAppVersion, corePolicy(false));
    const FileVerdict* extraVerdict = verdictFor(reported, "wer.json");
    require(extraVerdict != nullptr && extraVerdict->check == FileCheck::Unlisted,
            "лишний файл не помечен Unlisted при мягкой политике: " + describeChecks(reported));

    // Файл, переданный дважды, — тоже отказ: две версии одного правила под
    // одним именем неразличимы.
    std::vector<VerifiedFile> duplicated = onlyPresent;
    duplicated.push_back(makeVerifiedFile("temp.user.json", present));
    const RuleSetVerification twice =
        verifyRuleSet(shortManifest, "", duplicated, kAppVersion, corePolicy(false));
    require(!twice.ok(), "файл, переданный дважды, принят");
}

TEST(rules_manifest_detectsTamperingInRealRuleSet) {
    const std::string* bytes = manifestBytes();
    if (bytes == nullptr) return;
    require(!ruleSources().empty(), "нечего подменять: набор правил пуст");

    // Проверка на настоящих данных: если бы сверка с манифестом была нечувствительна
    // к правке файла, этот тест её бы поймал.
    const RuleSetVerification report =
        verifyRuleSet(*bytes, "", verifiedRuleFiles(), kAppVersion, corePolicy(true));
    require(report.ok(), "набор из rules/ не проходит проверку целостности: " + report.summary() + " [" +
                             describeChecks(report) + "]");

    std::vector<VerifiedFile> tampered = verifiedRuleFiles();
    tampered[0].size += 1;  // платформенный слой принёс файл на байт длиннее
    const RuleSetVerification broken =
        verifyRuleSet(*bytes, "", tampered, kAppVersion, corePolicy(true));
    require(!broken.ok(), "изменённый файл принят как целый — сверка с манифестом не работает");
    require(problemsMention(broken, tampered[0].path),
            "в отказе не назван изменённый файл " + tampered[0].path + ": " + broken.summary());
}
