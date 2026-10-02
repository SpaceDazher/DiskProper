// Проба R2: формы множественного числа в core::units.
// Проверяет, что одна функция печатает правильную форму в обоих языках на шести
// числах из задания (0, 1, 2, 5, 21, 101) и что формы ключа каталога
// cleanup.hint.ruleCount совпадают с тройкой «правило|правила|правил».
//
// Сборка (из корня репозитория, cl.exe VS 2019 Build Tools):
//   tools\k2check.bat src\core\units.cpp            // только проверка синтаксиса
//   cl /nologo /std:c++20 /EHsc /W4 /WX /permissive- /utf-8 /Zc:__cplusplus \
//      /I src /Fe:tmp\r2_plural_probe.exe tmp\r2_plural_probe.cpp src\core\units.cpp src\core\i18n.cpp
// Код возврата 0 — все формы совпали с ожидаемыми, любое другое значение — нет.
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "core/i18n.hpp"
#include "core/units.hpp"

#if defined(R2_RULE_KEY_HEADER)
#include R2_RULE_KEY_HEADER
#else
#error "R2_RULE_KEY_HEADER не задан: проба сверяется с ключом каталога из src/ui/locale.hpp"
#endif

namespace {

int g_failed = 0;
int g_checked = 0;

void checkEq(const std::string& got, const std::string& want, const char* what) {
    ++g_checked;
    if (got == want) {
        std::printf("  ok   %-46s %s\n", what, got.c_str());
        return;
    }
    ++g_failed;
    std::printf("  FAIL %-46s получено «%s», ожидалось «%s»\n", what, got.c_str(), want.c_str());
}

using mrproper::core::CountNoun;
using mrproper::core::Language;

const std::uint64_t kCounts[] = {0, 1, 2, 5, 21, 101};

// Ожидаемая русская форма: 1 → one, 2-4 → few, 0 и 5+ → many (CLDR).
const char* expectRu(std::uint64_t n, const char* one, const char* few, const char* many) {
    const std::uint64_t mod100 = n % 100;
    if (mod100 >= 11 && mod100 <= 14) return many;
    const std::uint64_t mod10 = n % 10;
    if (mod10 == 1) return one;
    if (mod10 >= 2 && mod10 <= 4) return few;
    return many;
}

void probeNoun(const char* name, const CountNoun& noun, const char* ruOne, const char* ruFew, const char* ruMany,
               const char* enOne, const char* enPlural) {
    std::printf("%s:\n", name);
    for (const std::uint64_t n : kCounts) {
        const std::string ruWant = std::to_string(n) + " " + expectRu(n, ruOne, ruFew, ruMany);
        const std::string enWant = std::to_string(n) + " " + (n == 1 ? enOne : enPlural);
        checkEq(mrproper::core::formatCount(n, noun, Language::Russian), ruWant, "ru");
        checkEq(mrproper::core::formatCount(n, noun, Language::English), enWant, "en");
    }
}

}  // namespace

int main() {
    std::printf("R2: формы множественного числа, ru/en, числа 0/1/2/5/21/101\n");

    probeNoun("файл (kFile)", mrproper::core::nouns::kFile, "файл", "файла", "файлов", "file", "files");
    probeNoun("кандидат (kCandidate)", mrproper::core::nouns::kCandidate, "кандидат", "кандидата", "кандидатов",
              "candidate", "candidates");
    probeNoun("операция (kOperation)", mrproper::core::nouns::kOperation, "операция", "операции", "операций",
              "operation", "operations");
    probeNoun("элемент (kItem)", mrproper::core::nouns::kItem, "элемент", "элемента", "элементов", "item", "items");
    probeNoun("категория (kCategory)", mrproper::core::nouns::kCategory, "категория", "категории", "категорий",
              "category", "categories");
    probeNoun("день (kDay)", mrproper::core::nouns::kDay, "день", "дня", "дней", "day", "days");

    // Вызов без существительного остался файловым — им пользуются чужие файлы,
    // и его вывод менять нельзя (юит-тест units_formatCountAndAge).
    std::printf("файл (formatCount без существительного — совместимость):\n");
    for (const std::uint64_t n : kCounts) {
        const std::string want = std::to_string(n) + " " + expectRu(n, "файл", "файла", "файлов");
        checkEq(mrproper::core::formatCount(n), want, "ru (как в юит-тесте)");
    }

    // formatAge: дни по той же форме, месяцы и годы — без существительного.
    std::printf("formatAge (дни):\n");
    checkEq(mrproper::core::formatAge(0), std::string("0 с"), "0 секунд → секунды");
    checkEq(mrproper::core::formatAge(86400), std::string("1 день"), "1 сутки → день");
    checkEq(mrproper::core::formatAge(3 * 86400), std::string("3 дня"), "3 суток → дня");
    checkEq(mrproper::core::formatAge(21 * 86400), std::string("21 день"), "21 сутки → день");
    checkEq(mrproper::core::formatAge(30 * 86400), std::string("30 дней"), "30 суток → дней");
    checkEq(mrproper::core::formatAge(86400 * 40), std::string("1 мес"), "40 суток → месяц (форма не plural)");

    // Ключ каталога cleanup.hint.ruleCount (строки взяты из src/ui/locale.hpp на
    // этапе сборки пробы) против core-формы того же слова: слой ui печатает счёт
    // правил ключом, слой core печатает его тройкой «правило|правила|правил» —
    // разойтись они не должны ни на одном числе.
    std::printf("ключ каталога cleanup.hint.ruleCount против core-формы:\n");
    mrproper::core::StringCatalog catalog;
    catalog.add(Language::Russian, "cleanup.hint.ruleCount", r2probe::kRuleCountRu);
    catalog.add(Language::English, "cleanup.hint.ruleCount", r2probe::kRuleCountEn);
    mrproper::core::Localizer strings;
    strings.loadCatalog(std::move(catalog));
    const CountNoun rule{"правило", "правила", "правил", "rule", "rules"};
    for (const std::uint64_t n : kCounts) {
        strings.setLanguage(Language::Russian);
        checkEq(strings.trPlural("cleanup.hint.ruleCount", n),
                mrproper::core::formatCount(n, rule, Language::Russian), "ru: ключ == core");
        strings.setLanguage(Language::English);
        checkEq(strings.trPlural("cleanup.hint.ruleCount", n),
                mrproper::core::formatCount(n, rule, Language::English), "en: ключ == core");
    }

    std::printf("\nпроверок %d, провалов %d\n", g_checked, g_failed);
    return g_failed == 0 ? 0 : 1;
}