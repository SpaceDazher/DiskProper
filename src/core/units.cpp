#include "units.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace mrproper::core {
namespace {

// Русские единицы с правильными окончаниями — их придётся менять вместе с локализацией.
const char* pluralRu(std::uint64_t n, const char* one, const char* few, const char* many) {
    const std::uint64_t mod100 = n % 100;
    if (mod100 >= 11 && mod100 <= 14) return many;
    switch (n % 10) {
        case 1: return one;
        case 2:
        case 3:
        case 4: return few;
        default: return many;
    }
}

std::string fixed(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

// Русская десятичная запятая, без локали Windows.
std::string withComma(std::string s) {
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

}  // namespace

std::string formatBytes(std::uint64_t bytes, int decimals, bool binaryUnits) {
    static const std::array<const char*, 6> unitsDec = {"Б", "КБ", "МБ", "ГБ", "ТБ", "ПБ"};
    static const std::array<const char*, 6> unitsBin = {"Б", "КиБ", "МиБ", "ГиБ", "ТиБ", "ПиБ"};
    const double base = binaryUnits ? 1024.0 : 1000.0;

    if (bytes < base) {
        return std::to_string(bytes) + " " + (binaryUnits ? unitsBin[0] : unitsDec[0]);
    }
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= base && unit + 1 < unitsDec.size()) {
        value /= base;
        ++unit;
    }
    // Не показываем «1 000,0 МБ» — округляем хвост до двух значащих цифр после запятой.
    std::string text = withComma(fixed(value, value < 10.0 ? decimals : 0));
    text += " ";
    text += binaryUnits ? unitsBin[unit] : unitsDec[unit];
    return text;
}

// Существительное здесь ЗАШИТО: «файл». Это не мелочь, а ловушка, на которую
// уже наступили (D-70). Счёт правил, томов, дисков, разделов и решений,
// выведенный через formatCount, печатает слово «файлов» — сводка настроек
// показывала «Правил: 138 файлов» о ста тридцати восьми правилах. Правило
// множественного числа тут работает честно, но не для того существительного,
// о котором думает вызывающий.
//
// Что делать вызывающему:
//   * считаешь файлы — formatCount здесь единственно правильный;
//   * существительное уже названо в подписи рядом («Правил:», «Дисков:») —
//     печатай голое число (core::formatInteger с локалью из core/i18n);
//   * существительное в шаблоне есть, и оно не «файл» — формат берётся из
//     каталога строк с правилом множественного числа: core::trPlural(ключ, n)
//     или ui::trPlural(...) в слое интерфейса.
//
// Общий API с существительным параметром (formatCount(n, «правило», ...))
// требует объявления в units.hpp — файл не входит в список моей задачи, см.
// отчёт по Q3 (D-70).
std::string formatCount(std::uint64_t count) {
    return std::to_string(count) + " " + pluralRu(count, "файл", "файла", "файлов");
}

std::string formatPercent(double fraction, int decimals) {
    return withComma(fixed(fraction * 100.0, decimals)) + " %";
}

std::string formatAge(std::int64_t seconds) {
    if (seconds < 0) return "только что";
    if (seconds < 60) return std::to_string(seconds) + " с";
    if (seconds < 3600) return std::to_string(seconds / 60) + " мин";
    if (seconds < 86400) return std::to_string(seconds / 3600) + " ч";
    const std::int64_t days = seconds / 86400;
    if (days < 31) return std::to_string(days) + " " + pluralRu(static_cast<std::uint64_t>(days), "день", "дня", "дней");
    if (days < 365) return std::to_string(days / 30) + " мес";
    return std::to_string(days / 365) + " г";
}

}  // namespace mrproper::core
