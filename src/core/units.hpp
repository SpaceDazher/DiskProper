// Форматирование величин для интерфейса и отчётов (ru по умолчанию).
// Переносимый модуль: без Windows API.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Язык и локализатор процесса — оттуда же, откуда каталог строк: форма
// множественного числа обязана считаться тем же языком, что и подпись рядом.
#include "i18n.hpp"

namespace mrproper::core {

// «1,2 ГБ» / «845 Б». binaryUnits=true даёт KiB/МиБ/ГиБ.
std::string formatBytes(std::uint64_t bytes, int decimals = 1, bool binaryUnits = false);

// «1 234 файла». Существительное здесь зашито: это «файл». Считаешь не файлы —
// бери форму с существительным (formatCount ниже), иначе получишь «2 правил».
std::string formatCount(std::uint64_t count);

// Существительное для формы множественного числа сразу в двух языках: русский
// знает три формы (one|few|many), английский — две (one|other).
//
// Один набор на оба языка — потому что язык здесь не выбирают: его берёт
// локализатор процесса. Если бы вызывающий передавал только русские формы,
// английский экран печатал бы русское слово (D-70: «Rules: 138 файлов»).
struct CountNoun {
    std::string_view ruOne;
    std::string_view ruFew;
    std::string_view ruMany;
    std::string_view enOne;
    std::string_view enPlural;
};

// «2 правила» (ru) / «2 rules» (en). Формы — по правилам CLDR, тем же, что у
// core::pluralFormIndex: 1 → one, 2-4 → few, 0 и 5+ → many; en: 1 → one,
// остальное → other. Число печатается без разделителей тысяч, как и соседние
// строки этих же сводок: разделитель приходит из локали, а не из форматтера
// (§5 «даты/числа через GetLocaleInfoEx» — работа слоя ui).
std::string formatCount(std::uint64_t count, const CountNoun& noun, Language lang = currentLanguage());

// Существительные, которых в коде больше одного. Их не пишут на месте вызова:
// опечатка в форме («файла» вместо «файлов») стоит столько же, сколько её
// отсутствие, а проверить её можно только глазами по выводу.
//
// Счёт правил сюда не попал: в core/cli он печатается подписью рядом («правил:
// 8»), а слой ui печатает его ключом каталога cleanup.hint.ruleCount
// (src/ui/locale.hpp, view_cleanup.cpp: trPlural(kCleanupHintRuleCount, n) →
// «8 правил» / «2 правила»). Если счёт правил появится здесь без подписи, его
// форма — та же тройка: CountNoun{"правило", "правила", "правил", "rule",
// "rules"}; расхождение с ключом ловит проба R2.
namespace nouns {
inline constexpr CountNoun kFile{"файл", "файла", "файлов", "file", "files"};
inline constexpr CountNoun kCandidate{"кандидат", "кандидата", "кандидатов", "candidate", "candidates"};
inline constexpr CountNoun kOperation{"операция", "операции", "операций", "operation", "operations"};
inline constexpr CountNoun kItem{"элемент", "элемента", "элементов", "item", "items"};
inline constexpr CountNoun kCategory{"категория", "категории", "категорий", "category", "categories"};
inline constexpr CountNoun kDay{"день", "дня", "дней", "day", "days"};
}  // namespace nouns

// «12,3 %»
std::string formatPercent(double fraction, int decimals = 1);

// «3 дн.» / «2 ч»
std::string formatAge(std::int64_t seconds);

}  // namespace mrproper::core
