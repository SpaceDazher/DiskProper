// Реализация локализации: см. i18n.hpp. Спека: §5 «Локализация».
#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

namespace mrproper::core {
namespace {

// Индекс языка в массиве переводов.
std::size_t langIndex(Language lang) {
    return static_cast<std::size_t>(lang);
}

// Второй из двух языков: им закрывается fallback, когда ключа нет в запрошенном.
Language otherLanguage(Language lang) {
    return lang == Language::Russian ? Language::English : Language::Russian;
}

char lowerAscii(char ch) {
    // Только ASCII: кириллица в теге языка не встречается, а locale-функции
    // здесь были бы лишней зависимостью.
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool isAsciiDigit(char ch) {
    return ch >= '0' && ch <= '9';
}

// «0» → 0, «12» → 12. Имя параметра не является числом или не влезает в
// std::size_t — пусто: такой плейсхолдер останется в тексте видимым.
std::optional<std::size_t> parseIndex(std::string_view name) {
    if (name.empty()) return std::nullopt;
    std::size_t value = 0;
    const std::size_t limit = (std::numeric_limits<std::size_t>::max)();
    for (const char ch : name) {
        if (!isAsciiDigit(ch)) return std::nullopt;
        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (limit - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

// Общий разбор шаблона для обоих видов параметров: lookup возвращает значение
// по имени плейсхолдера или nullptr, если такого параметра нет.
template <typename Lookup>
std::string formatWith(std::string_view tmpl, const Lookup& lookup) {
    std::string out;
    out.reserve(tmpl.size());
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        const char ch = tmpl[i];
        if (ch == '{') {
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') {  // «{{» — экранированная скобка
                out.push_back('{');
                ++i;
                continue;
            }
            const std::size_t close = tmpl.find('}', i + 1);
            if (close == std::string_view::npos) {  // «{» без закрытия — печатаем как есть
                out.push_back(ch);
                continue;
            }
            const std::string_view name = tmpl.substr(i + 1, close - i - 1);
            const std::string* value = lookup(name);
            if (value != nullptr) {
                out.append(*value);
            } else {
                // Параметра нет: оставляем «{2}» в тексте, чтобы пропуск перевода
                // был виден, а не выглядел как «всё в порядке».
                out.append(tmpl.substr(i, close - i + 1));
            }
            i = close;
            continue;
        }
        if (ch == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
            out.push_back('}');
            ++i;
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

const std::string* lookupPositional(std::string_view name, const StringArgs& args) {
    const std::optional<std::size_t> index = parseIndex(name);
    if (!index.has_value() || *index >= args.size()) return nullptr;
    return &args[*index];
}

const std::string* lookupNamed(std::string_view name, const NamedArgs& args) {
    for (const std::pair<std::string_view, std::string>& named : args) {
        if (named.first == name) return &named.second;
    }
    return nullptr;
}

// Разделители тысяч, вставленные справа налево по три цифры.
std::string groupDigits(std::string digits, char separator) {
    if (separator == '\0') return digits;
    for (std::size_t pos = digits.size(); pos > 3; pos -= 3) {
        digits.insert(pos - 3, 1, separator);
    }
    return digits;
}

}  // namespace

// --- Языки и направление письма ------------------------------------------------

const char* languageTag(Language lang) {
    switch (lang) {
        case Language::Russian:
            return "ru";
        case Language::English:
            return "en";
    }
    return "ru";
}

const char* languageName(Language lang) {
    switch (lang) {
        case Language::Russian:
            return "Русский";
        case Language::English:
            return "English";
    }
    return "Русский";
}

std::optional<Language> parseLanguage(std::string_view tag) {
    // Первичный субтег BCP-47: «ru-RU», «ru_RU» и «RU» → русский.
    std::size_t end = 0;
    while (end < tag.size() && tag[end] != '-' && tag[end] != '_') {
        ++end;
    }
    if (end == 0) return std::nullopt;
    const std::string primary(tag.substr(0, end));
    std::string folded;
    folded.reserve(primary.size());
    for (const char ch : primary) {
        folded.push_back(lowerAscii(ch));
    }
    if (folded == "ru") return Language::Russian;
    if (folded == "en") return Language::English;
    return std::nullopt;
}

TextDirection textDirection(Language lang) {
    switch (lang) {
        case Language::Russian:
        case Language::English:
            return TextDirection::LeftToRight;
    }
    return TextDirection::LeftToRight;
}

bool isRtl(Language lang) {
    return textDirection(lang) == TextDirection::RightToLeft;
}

// --- Числа в локали ------------------------------------------------------------

std::string formatInteger(std::uint64_t value, const NumberFormat& fmt) {
    return groupDigits(std::to_string(value), fmt.groupSeparator);
}

std::string formatDecimal(double value, int decimals, const NumberFormat& fmt) {
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;  // «%.12f» в UI осмысленности не имеет

    // Разделитель дробной части приходит от GetLocaleInfoEx, а snprintf печатает
    // по C-локали (приложение setlocale не вызывает). Ищем и то и другое, чтобы
    // вывод не зависел от того, успел ли кто-то сменить локаль процесса.
    char buffer[512];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    if (written <= 0) return std::string();
    std::string text(buffer);

    std::size_t mark = text.find_last_of('.');
    if (mark == std::string::npos) mark = text.find_last_of(',');
    const std::string integerPart = (mark == std::string::npos) ? text : text.substr(0, mark);
    const std::string fractionPart = (mark == std::string::npos) ? std::string() : text.substr(mark + 1);

    // «nan»/«inf» и прочая не-числовая строка группировать нельзя.
    const bool numeric = !integerPart.empty() &&
                         std::all_of(integerPart.begin(), integerPart.end(),
                                     [](char ch) { return isAsciiDigit(ch) || ch == '-' || ch == '+'; });
    if (!numeric) return text;

    std::string sign;
    std::string digits = integerPart;
    if (!digits.empty() && (digits.front() == '-' || digits.front() == '+')) {
        sign.assign(1, digits.front());
        digits.erase(digits.begin());
    }
    std::string out = sign + groupDigits(std::move(digits), fmt.groupSeparator);
    if (!fractionPart.empty()) {
        out.push_back(fmt.decimalSeparator);
        out.append(fractionPart);
    }
    return out;
}

// --- Подстановка параметров ----------------------------------------------------

std::string formatTemplate(std::string_view tmpl, const StringArgs& args) {
    return formatWith(tmpl, [&args](std::string_view name) { return lookupPositional(name, args); });
}

std::string formatTemplate(std::string_view tmpl, const NamedArgs& args) {
    return formatWith(tmpl, [&args](std::string_view name) { return lookupNamed(name, args); });
}

std::size_t pluralFormIndex(Language lang, std::uint64_t count) {
    if (lang == Language::English) {
        return count == 1 ? 0u : 1u;  // one | other
    }
    // Русский по правилам CLDR: 1 файл, 2-4 файла, 5+ файлов; 11-14 → many.
    const std::uint64_t mod100 = count % 100;
    if (mod100 >= 11 && mod100 <= 14) return 2;
    const std::uint64_t mod10 = count % 10;
    if (mod10 == 1) return 0;
    if (mod10 >= 2 && mod10 <= 4) return 1;
    return 2;
}

std::string selectPluralForm(std::string_view forms, Language lang, std::uint64_t count) {
    const std::size_t want = pluralFormIndex(lang, count);
    std::size_t index = 0;
    std::size_t start = 0;
    std::string_view picked = forms;
    for (;;) {
        const std::size_t sep = forms.find('|', start);
        const std::string_view part = (sep == std::string_view::npos) ? forms.substr(start)
                                                                     : forms.substr(start, sep - start);
        picked = part;
        if (index == want || sep == std::string_view::npos) break;  // форм не хватило — последняя
        start = sep + 1;
        ++index;
    }
    return std::string(picked);
}

// --- Каталог строк -------------------------------------------------------------

bool StringCatalog::add(Language lang, std::string_view key, std::string_view tmpl) {
    // operator[] создаёт запись с пустыми переводами: ключ появляется в каталоге
    // сразу, даже если пока заполнен только один из языков.
    if (tmpl.empty()) return false;  // пустая строка — это «перевода нет»
    std::array<std::string, 2>& slot = entries_[std::string(key)];
    const std::size_t index = langIndex(lang);
    if (!slot[index].empty()) return false;  // перевод не затираем молча
    slot[index] = std::string(tmpl);
    return true;
}

void StringCatalog::set(Language lang, std::string_view key, std::string_view tmpl) {
    std::array<std::string, 2>& slot = entries_[std::string(key)];
    slot[langIndex(lang)] = std::string(tmpl);
}

void StringCatalog::clear() {
    entries_.clear();
}

bool StringCatalog::has(Language lang, std::string_view key) const {
    const auto it = entries_.find(std::string(key));
    return it != entries_.end() && !it->second[langIndex(lang)].empty();
}

bool StringCatalog::has(std::string_view key) const {
    return entries_.find(std::string(key)) != entries_.end();
}

std::size_t StringCatalog::size() const {
    return entries_.size();
}

std::size_t StringCatalog::size(Language lang) const {
    const std::size_t index = langIndex(lang);
    std::size_t count = 0;
    for (const std::pair<const std::string, Entry>& entry : entries_) {
        if (!entry.second[index].empty()) {
            ++count;
        }
    }
    return count;
}

std::string StringCatalog::raw(std::string_view key, Language lang) const {
    const auto it = entries_.find(std::string(key));
    if (it == entries_.end()) return std::string();
    return it->second[langIndex(lang)];
}

bool StringCatalog::isTranslated(std::string_view key, Language lang) const {
    return has(lang, key);
}

std::vector<std::string> StringCatalog::keysMissingIn(Language lang) const {
    const std::size_t index = langIndex(lang);
    std::vector<std::string> missing;
    for (const std::pair<const std::string, Entry>& entry : entries_) {
        if (entry.second[index].empty()) {
            missing.push_back(entry.first);
        }
    }
    return missing;  // std::map уже отсортирован по ключу
}

std::optional<Language> StringCatalog::findLanguage(std::string_view key, Language lang) const {
    const auto it = entries_.find(std::string(key));
    if (it == entries_.end()) return std::nullopt;
    if (!it->second[langIndex(lang)].empty()) return lang;
    if (!it->second[langIndex(kDefaultLanguage)].empty()) return kDefaultLanguage;
    const Language fallback = otherLanguage(lang);
    if (!it->second[langIndex(fallback)].empty()) return fallback;
    return std::nullopt;
}

std::string StringCatalog::resolve(std::string_view key, Language lang) const {
    const std::optional<Language> found = findLanguage(key, lang);
    if (!found.has_value()) return std::string(key);  // ключ виден и ищется grep'ом
    return raw(key, *found);
}

std::string StringCatalog::resolvePlural(std::string_view key, std::uint64_t count, Language lang) const {
    const std::optional<Language> found = findLanguage(key, lang);
    if (!found.has_value()) return std::string(key);
    // Форму выбирает язык найденного перевода: у ru форм три, у en две.
    return selectPluralForm(raw(key, *found), *found, count);
}

// --- Процессное состояние локали ----------------------------------------------

Localizer::Localizer() : catalog_(std::make_shared<StringCatalog>()) {}

Localizer::~Localizer() = default;

void Localizer::mutate(const std::function<void(StringCatalog&)>& change) {
    // Копирование при записи: читатели держат свой снимок каталога и не
    // блокируются, поэтому перезагрузка ресурсов не останавливает отрисовку.
    // Правки предполагаются из одного потока (UI при старте и при перезагрузке).
    std::shared_ptr<StringCatalog> next;
    {
        const std::lock_guard<std::mutex> lock(catalogMutex_);
        next = std::make_shared<StringCatalog>(*catalog_);
    }
    change(*next);
    {
        const std::lock_guard<std::mutex> lock(catalogMutex_);
        catalog_ = std::move(next);
    }
    ++revision_;
}

std::shared_ptr<const StringCatalog> Localizer::catalog() const {
    const std::lock_guard<std::mutex> lock(catalogMutex_);
    return catalog_;
}

bool Localizer::addString(Language lang, std::string_view key, std::string_view tmpl) {
    bool added = false;
    mutate([&](StringCatalog& strings) { added = strings.add(lang, key, tmpl); });
    return added;
}

void Localizer::setString(Language lang, std::string_view key, std::string_view tmpl) {
    mutate([&](StringCatalog& strings) { strings.set(lang, key, tmpl); });
}

void Localizer::loadCatalog(StringCatalog catalog) {
    std::shared_ptr<StringCatalog> next = std::make_shared<StringCatalog>(std::move(catalog));
    {
        const std::lock_guard<std::mutex> lock(catalogMutex_);
        catalog_ = std::move(next);
    }
    ++revision_;
}

void Localizer::clearStrings() {
    mutate([](StringCatalog& strings) { strings.clear(); });
}

Language Localizer::language() const {
    return language_.load(std::memory_order_acquire);
}

void Localizer::setLanguage(Language lang) {
    if (language_.exchange(lang, std::memory_order_acq_rel) == lang) {
        return;  // язык не изменился — перерисовывать нечего
    }
    ++revision_;
}

Language Localizer::toggleLanguage() {
    const Language next = otherLanguage(language());
    setLanguage(next);
    return next;
}

std::uint64_t Localizer::revision() const {
    return revision_.load(std::memory_order_acquire);
}

NumberFormat Localizer::numberFormat() const {
    const std::lock_guard<std::mutex> lock(numberMutex_);
    return numberFormat_;
}

void Localizer::setNumberFormat(const NumberFormat& fmt) {
    {
        const std::lock_guard<std::mutex> lock(numberMutex_);
        numberFormat_ = fmt;
    }
    ++revision_;  // в строках может печататься число
}

std::string Localizer::tr(std::string_view key) const {
    return catalog()->resolve(key, language());
}

std::string Localizer::tr(std::string_view key, std::string_view arg0) const {
    StringArgs args;
    args.emplace_back(arg0);
    return tr(key, args);
}

std::string Localizer::tr(std::string_view key, const StringArgs& args) const {
    return formatTemplate(catalog()->resolve(key, language()), args);
}

std::string Localizer::tr(std::string_view key, const NamedArgs& args) const {
    return formatTemplate(catalog()->resolve(key, language()), args);
}

std::string Localizer::trPlural(std::string_view key, std::uint64_t count) const {
    return trPlural(key, count, StringArgs{});
}

std::string Localizer::trPlural(std::string_view key, std::uint64_t count, const StringArgs& extra) const {
    const std::shared_ptr<const StringCatalog> strings = catalog();
    StringArgs args;
    args.reserve(extra.size() + 1);
    args.push_back(formatInteger(count, numberFormat()));  // {0} — число в локали
    for (const std::string& value : extra) {
        args.push_back(value);
    }
    return formatTemplate(strings->resolvePlural(key, count, language()), args);
}

std::vector<std::string> Localizer::missingTranslations() const {
    return catalog()->keysMissingIn(language());
}

Localizer& localizer() {
    static Localizer instance;  // с потокобезопасной инициализацией
    return instance;
}

Language currentLanguage() {
    return localizer().language();
}

void setLanguage(Language lang) {
    localizer().setLanguage(lang);
}

std::uint64_t localizationRevision() {
    return localizer().revision();
}

std::string tr(std::string_view key) {
    return localizer().tr(key);
}

std::string tr(std::string_view key, std::string_view arg0) {
    return localizer().tr(key, arg0);
}

std::string tr(std::string_view key, const StringArgs& args) {
    return localizer().tr(key, args);
}

std::string trPlural(std::string_view key, std::uint64_t count) {
    return localizer().trPlural(key, count);
}

std::vector<std::string> missingTranslations() {
    return localizer().missingTranslations();
}

}  // namespace mrproper::core
