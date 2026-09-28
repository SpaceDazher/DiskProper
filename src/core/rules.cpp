#include "rules.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "glob.hpp"

namespace mrproper::core {
namespace {

const std::vector<std::string>& allowedFields() {
    static const std::vector<std::string> kFields = {
        "rules",     "id",           "category", "safety",          "locator", "locatorExcludes",
        "minAgeDays", "requiresProcessesClosed", "groupByProfile", "title", "note",
        "schemaVersion", "version", "minAppVersion"};
    return kFields;
}

bool isSafeId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char c : id) {
        const bool okChar = std::islower(static_cast<unsigned char>(c)) != 0 || std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                            c == '.' || c == '_' || c == '-';
        if (!okChar) return false;
    }
    return true;
}

SafetyLevel safetyFromString(const std::string& s) {
    if (s == "safe") return SafetyLevel::Safe;
    if (s == "review") return SafetyLevel::Review;
    if (s == "risky") return SafetyLevel::Risky;
    throw RuleError("safety: допустимы safe|review|risky, получено \"" + s + "\"");
}

const char* safetyToString(SafetyLevel level) {
    switch (level) {
        case SafetyLevel::Safe: return "safe";
        case SafetyLevel::Review: return "review";
        case SafetyLevel::Risky: return "risky";
    }
    return "review";
}

std::vector<std::string> stringArray(const json::Value& parent, const char* key, const std::string& origin) {
    std::vector<std::string> out;
    const json::Value* node = parent.find(key);
    if (node == nullptr) return out;
    if (!node->isArray()) throw RuleError(origin + ": поле \"" + key + "\" должно быть массивом строк");
    for (const auto& item : node->items()) {
        if (!item.isString()) throw RuleError(origin + ": поле \"" + key + "\" должно быть массивом строк");
        out.push_back(item.asString());
    }
    return out;
}

void rejectUnknownFields(const json::Value& node, const std::string& origin) {
    for (const auto& member : node.members()) {
        const auto& allowed = allowedFields();
        if (std::find(allowed.begin(), allowed.end(), member.first) == allowed.end()) {
            // Неизвестное поле — ошибка, а не предупреждение: правила приходят из сети (ADR-008).
            throw RuleError(origin + ": неизвестное поле \"" + member.first + "\"");
        }
    }
}

}  // namespace

const char* toString(SafetyLevel level) { return safetyToString(level); }

Rule Rule::fromJson(const json::Value& value, const std::string& origin) {
    if (!value.isObject()) throw RuleError(origin + ": правило должно быть объектом");
    rejectUnknownFields(value, origin);

    Rule rule;
    const json::Value& id = value.require("id");
    if (!id.isString()) throw RuleError(origin + ": id должен быть строкой");
    rule.id = id.asString();
    if (!isSafeId(rule.id)) {
        throw RuleError(origin + ": недопустимый id \"" + rule.id + "\" (строчные буквы, цифры, . _ -)");
    }

    const json::Value& category = value.require("category");
    if (!category.isString()) throw RuleError(origin + ": category должна быть строкой");
    rule.category = category.asString();

    const json::Value& safety = value.require("safety");
    if (!safety.isString()) throw RuleError(origin + ": safety должен быть строкой");
    rule.safety = safetyFromString(safety.asString());

    const json::Value& locator = value.require("locator");
    if (!locator.isString()) throw RuleError(origin + ": locator должен быть строкой");
    rule.locator = locator.asString();
    if (!isValidPattern(rule.locator)) {
        throw RuleError(origin + ": некорректный locator \"" + rule.locator + "\"");
    }

    rule.locatorExcludes = stringArray(value, "locatorExcludes", origin);
    for (const auto& ex : rule.locatorExcludes) {
        if (!isValidPattern(ex)) {
            throw RuleError(origin + ": некорректный locatorExcludes \"" + ex + "\"");
        }
    }

    if (const json::Value* age = value.find("minAgeDays")) {
        if (!age->isNumber()) throw RuleError(origin + ": minAgeDays должно быть числом");
        const double raw = age->asNumber();
        if (raw < 0) throw RuleError(origin + ": minAgeDays не может быть отрицательным");
        rule.minAgeDays = static_cast<std::int64_t>(raw);
    }

    rule.requiresProcessesClosed = stringArray(value, "requiresProcessesClosed", origin);
    rule.groupByProfile = value.find("groupByProfile") != nullptr && value.find("groupByProfile")->asBool();

    if (const json::Value* title = value.find("title")) {
        if (!title->isObject()) throw RuleError(origin + ": title должен быть объектом {ru,en}");
        if (const json::Value* ru = title->find("ru")) {
            if (ru->isString()) rule.titleRu = ru->asString();
        }
        if (const json::Value* en = title->find("en")) {
            if (en->isString()) rule.titleEn = en->asString();
        }
    }
    if (const json::Value* note = value.find("note")) {
        if (note->isString()) rule.note = note->asString();
    }

    // Правило, объявляющее safe, обязано быть узким: возрастной порог либо узнаваемое имя
    // каталога кэша/временной папки. Иначе «safe» — это обещание, которое движок не сможет
    // подтвердить, а удалять данные наугад нельзя (SPEC §10).
    if (rule.safety == SafetyLevel::Safe && rule.minAgeDays < 1 &&
        rule.locator.find("Cache") == std::string::npos &&
        rule.locator.find("Temp") == std::string::npos &&
        rule.locator.find("cache") == std::string::npos &&
        rule.locator.find("temp") == std::string::npos) {
        throw RuleError(origin + ": правило " + rule.id +
                        " объявлено safe, но не имеет ни minAgeDays, ни имени известного "
                        "кэша (Cache/Temp) — укажите review либо сузьте locator");
    }

    return rule;
}

bool Rule::matches(const std::string& normalizedPath) const {
    const std::string target = normalizeSeparators(normalizedPath);
    return matchPath(resolvedLocator.empty() ? locator : resolvedLocator, target);
}

bool Rule::excluded(const std::string& normalizedPath) const {
    const std::string target = normalizeSeparators(normalizedPath);
    for (const auto& pattern : resolvedExcludes) {
        if (matchPath(pattern, target)) return true;
    }
    return false;
}

std::string Rule::title() const { return titleRu.empty() ? titleEn : titleRu; }

Rule* RuleSet::byId(const std::string& id) {
    for (auto& rule : rules) {
        if (rule.id == id) return &rule;
    }
    return nullptr;
}

const Rule* RuleSet::byId(const std::string& id) const {
    for (const auto& rule : rules) {
        if (rule.id == id) return &rule;
    }
    return nullptr;
}

RuleSet loadRuleFile(const std::string& text, const std::string& origin) {
    json::Value document;
    try {
        document = json::parse(text);
    } catch (const json::ParseError& e) {
        throw RuleError(origin + ": невалидный JSON — " + e.what());
    }
    if (!document.isObject()) throw RuleError(origin + ": корень должен быть объектом");
    rejectUnknownFields(document, origin);

    RuleSet set;
    if (const json::Value* schema = document.find("schemaVersion")) {
        if (!schema->isNumber()) throw RuleError(origin + ": schemaVersion должен быть числом");
        set.schemaVersion = static_cast<int>(schema->asNumber());
    }
    if (set.schemaVersion != 1) {
        throw RuleError(origin + ": поддерживается только schemaVersion=1");
    }
    if (const json::Value* version = document.find("version")) {
        if (version->isString()) set.version = version->asString();
    }
    if (const json::Value* minApp = document.find("minAppVersion")) {
        if (minApp->isString()) set.minAppVersion = minApp->asString();
    }
    const json::Value& rules = document.require("rules");
    if (!rules.isArray()) throw RuleError(origin + ": rules должен быть массивом");
    for (const auto& item : rules.items()) {
        set.rules.push_back(Rule::fromJson(item, origin));
    }
    return set;
}

RuleSet loadRuleFiles(const std::vector<std::pair<std::string, std::string>>& files, const std::string& env,
                      bool* anyUnresolved) {
    RuleSet merged;
    bool first = true;
    for (const auto& [origin, content] : files) {
        if (content.empty()) continue;
        RuleSet part = loadRuleFile(content, origin);
        if (first) {
            merged.schemaVersion = part.schemaVersion;
            merged.version = part.version;
            merged.minAppVersion = part.minAppVersion;
            first = false;
        }
        merged.rules.insert(merged.rules.end(), part.rules.begin(), part.rules.end());
    }

    bool allResolved = true;
    for (auto& rule : merged.rules) {
        bool okLocator = true;
        bool okExclude = true;
        rule.resolvedLocator = expandEnvironment(rule.locator, env, &okLocator);
        allResolved = allResolved && okLocator;
        for (const auto& pattern : rule.locatorExcludes) {
            bool ok = true;
            rule.resolvedExcludes.push_back(expandEnvironment(pattern, env, &ok));
            okExclude = okExclude && ok;
        }
        allResolved = allResolved && okExclude;
    }
    validateRuleSet(merged);
    if (anyUnresolved != nullptr) {
        // Правило с отсутствующей переменной окружения не сработает — не ошибка,
        // но UI должен показать «часть правил неактивна».
        *anyUnresolved = !allResolved;
    }
    return merged;
}

void validateRuleSet(const RuleSet& set) {
    if (set.rules.empty()) throw RuleError("набор правил пуст");
    std::vector<std::string> ids;
    ids.reserve(set.rules.size());
    for (const auto& rule : set.rules) {
        if (std::find(ids.begin(), ids.end(), rule.id) != ids.end()) {
            throw RuleError("дублирующийся id правила: " + rule.id);
        }
        ids.push_back(rule.id);
        if (rule.safety == SafetyLevel::Safe && rule.minAgeDays < 1 &&
            rule.locator.find("Cache") == std::string::npos &&
            rule.locator.find("Temp") == std::string::npos) {
            throw RuleError("правило " + rule.id +
                            ": safe без minAgeDays и без вхождения Cache/Temp — слишком широкое, "
                            "используйте review");
        }
    }
}

}  // namespace mrproper::core
