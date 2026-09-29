// Правила очистки: загрузка, валидация, применение к путям (SPEC §4 FR-4, §6.2).
// Правила — данные, а не код: обновляются без пересборки приложения (ADR-002).
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"
#include "model.hpp"

namespace mrproper::core {

class RuleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Rule {
    std::string id;
    std::string category;
    SafetyLevel safety{SafetyLevel::Review};
    std::string locator;                                  // шаблон пути с %ПЕРЕМЕННЫМИ%
    std::vector<std::string> locatorExcludes;
    std::int64_t minAgeDays{};
    std::vector<std::string> requiresProcessesClosed;
    bool groupByProfile{};

    // «Только оценка»: кандидат попадает в отчёт и агрегаты, но не удаляется
    // НИ при каком профиле, подтверждении и уровне (SPEC §4 FR-3: «только в
    // отчёте»). Пока флага не было, обещание «оценка без удаления» держалось
    // на тексте заметки правила, а --allow-risky превращал его в удаление
    // (docs/review-02.md F-04, обход подтверждён прогоном).
    bool estimateOnly{false};

    // Порог размера файла, байты. Категория user.bigfiles по FR-3 — «файлы
    // > 1 ГБ», и без этого поля правило срабатывало на ВСЁ содержимое каталога
    // (docs/review-02.md F-02). 0 — правило порога не объявляет, и тогда
    // элемент пользовательских данных не выбирается вовсе (core::plan).
    std::uint64_t minFileBytes{};
    std::string titleRu;
    std::string titleEn;
    std::string note;

    // Раскрытая версия locator (после подстановки переменных окружения).
    std::string resolvedLocator;
    std::vector<std::string> resolvedExcludes;

    // Разбор одного правила; бросает RuleError с указанием файла и поля.
    static Rule fromJson(const json::Value& value, const std::string& origin);

    bool matches(const std::string& normalizedPath) const;
    bool excluded(const std::string& normalizedPath) const;

    // Правило может что-то отсечь внутри своего корня? min-age, исключения,
    // фильтр листьев и порог размера — всё это уменьшает множество, которое
    // удалит исполнитель. Пока ответ был «нет», корень удалялся целиком вместе
    // со «свежими» и явно исключёнными файлами (docs/review-02.md F-01).
    // Сборщик (engine::candidate_collector) спрашивает это ДО обхода: если
    // «нет», список разрешённых файлов не нужен и кандидат остаётся каталогом.
    [[nodiscard]] bool filtersInsideRoot() const {
        return minAgeDays > 0 || minFileBytes > 0 || !locatorExcludes.empty();
    }
    std::string title() const;  // по текущему языку
};

struct RuleSet {
    int schemaVersion{};
    std::string version;
    std::string minAppVersion;
    std::vector<Rule> rules;

    Rule* byId(const std::string& id);
    const Rule* byId(const std::string& id) const;
    std::size_t size() const { return rules.size(); }
};

// Загрузка одного файла правила (объект с полем "rules": [...]).
RuleSet loadRuleFile(const std::string& text, const std::string& origin);

// Загрузка всего набора сразу: файлы читает платформа, здесь только склейка и
// подстановка переменных окружения. env — дамп "NAME=value\n" (платформа берёт
// его из GetEnvironmentVariableW). Бросает RuleError на любой невалидный набор:
// «битых» правил быть не должно (ADR-008).
RuleSet loadRuleFiles(const std::vector<std::pair<std::string, std::string>>& files, const std::string& env,
                      bool* anyUnresolved = nullptr);

// Кросс-проверки после загрузки: уникальность id, непустой набор, версия схемы.
void validateRuleSet(const RuleSet& set);

}  // namespace mrproper::core
