#include "units.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace mrproper::core {
namespace {

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

// Форму даёт core::pluralFormIndex — то же правило CLDR, что и в каталоге строк
// (i18n.cpp), поэтому «2 файла» здесь и «2 файла» в переводе получаются из одного
// кода, а не из двух, которые однажды разъедутся.
std::string formatCount(std::uint64_t count, const CountNoun& noun, Language lang) {
    const std::size_t form = pluralFormIndex(lang, count);
    const std::string_view word = (lang == Language::English)
                                       ? ((form == 0) ? noun.enOne : noun.enPlural)
                                       : ((form == 0) ? noun.ruOne : (form == 1 ? noun.ruFew : noun.ruMany));
    // Слово приклеивается по длине, а не как const char*: CountNoun можно собрать
    // и из std::string, а у такой строки data() не оканчивается нулём.
    std::string out = std::to_string(count);
    out += ' ';
    out.append(word.data(), word.size());
    return out;
}

// Обратная совместимость с вызовами без существительного: слово «файл» здесь
// зашито, и это по-прежнему ловушка (D-70) для того, кто считает не файлы.
// Таких мест осталось одиннадцать в четырёх чужих файлах (trash.cpp,
// sizing.cpp, scan_coordinator.cpp, ui/view_report.cpp); из них корректен
// sizing.cpp — он действительно считает файлы. Остальным правильный вызов —
// formatCount с существительным или голое число, если подпись рядом уже называет
// предмет. См. комментарий в units.hpp.
std::string formatCount(std::uint64_t count) {
    return formatCount(count, nouns::kFile);
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
    if (days < 31) return formatCount(static_cast<std::uint64_t>(days), nouns::kDay);
    if (days < 365) return std::to_string(days / 30) + " мес";
    return std::to_string(days / 365) + " г";
}

}  // namespace mrproper::core
