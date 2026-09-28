// Строки интерфейса: русский и английский, переключение языка без перезапуска,
// подстановка параметров. Спека: §5 «Локализация | ru + en, RTL-ready, переводы
// в ресурсах, даты/числа через GetLocaleInfoEx», §8 Этап 0 («локализация (ru)»).
//
// Граница слоёв (SPEC §6.1, ADR-004): ядро переносимо, Windows API здесь нет.
// Поэтому «переводы в ресурсах» и GetLocaleInfoEx — работа слоя platform/ui:
// он читает таблицу строк из ресурсов и кормит каталог через
// Localizer::addString/loadCatalog, а разделители чисел — через
// setNumberFormat. Наружу модуль отдаёт готовые строки, направление письма и
// локальные числа, поэтому добавление RTL-языка не потребует правок ядра.
// Формат дат намеренно не реализован: его задаёт platform через
// GetLocaleInfoEx, ядро о нём не знает.
//
// Строки — UTF-8 (как пути в model.hpp). Преобразование в UTF-16 для Win32 и
// DirectWrite делает ui.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mrproper::core {

// --- Языки и направление письма ------------------------------------------------

enum class Language : std::uint8_t { Russian = 0, English = 1 };

enum class TextDirection : std::uint8_t { LeftToRight, RightToLeft };

// Русский — первый полный язык приложения (SPEC §8, Этап 0), он же запасной
// при отсутствии перевода.
inline constexpr Language kDefaultLanguage = Language::Russian;

// BCP-47 тег для Settings/CLI: "ru" / "en".
const char* languageTag(Language lang);

// Название языка для выпадающего списка настроек — на своём же языке.
const char* languageName(Language lang);

// "ru", "ru-RU", "ru_RU", "EN", "en-US" → язык. Неизвестный тег — пусто.
std::optional<Language> parseLanguage(std::string_view tag);

// SPEC §5 «RTL-ready»: ru и en — LTR, ветка под RTL-язык есть уже сейчас,
// чтобы добавление языка не трогало ядро и UI.
TextDirection textDirection(Language lang);
bool isRtl(Language lang);

// --- Числа в локали ------------------------------------------------------------

// Разделители приходят из platform: GetLocaleInfoEx(LOCALE_SDECIMAL) и
// LOCALE_SGROUPING. Значения по умолчанию — русские.
struct NumberFormat {
    char decimalSeparator{','};
    char groupSeparator{' '};  // '\0' — без группировки

    NumberFormat() = default;
    NumberFormat(char decimal, char group) : decimalSeparator(decimal), groupSeparator(group) {}
};

// «1 234 567» (ru) / «1,234,567» (en)
std::string formatInteger(std::uint64_t value, const NumberFormat& fmt);

// «12,3» с нужным числом знаков после запятой
std::string formatDecimal(double value, int decimals, const NumberFormat& fmt);

// --- Подстановка параметров ----------------------------------------------------

// Шаблон: «{0}», «{1}» — параметры по позиции, «{имя}» — по имени,
// «{{» и «}}» — экранированные скобки. Неизвестный плейсхолдер остаётся в
// тексте как есть: молча проглоченный параметр в UI читается как «всё в
// порядке», а видимый «{2}» — как «перевод требует правки».
using StringArgs = std::vector<std::string>;
using NamedArgs = std::vector<std::pair<std::string_view, std::string>>;

std::string formatTemplate(std::string_view tmpl, const StringArgs& args);
std::string formatTemplate(std::string_view tmpl, const NamedArgs& args);

// Индекс формы множественного числа: ru — one|few|many (0|1|2),
// en — one|other (0|1). Русская форма считается по правилам CLDR.
std::size_t pluralFormIndex(Language lang, std::uint64_t count);

// Формы множественного числа хранятся в одной строке через '|' — так перевод
// помещается в один ресурс и остаётся читаемым:
//   ru: «{0} файл|{0} файла|{0} файлов»
//   en: «{0} file|{0} files»
// Если форм не хватило, берётся последняя: пустая строка в UI хуже
// приблизительно верной.
std::string selectPluralForm(std::string_view forms, Language lang, std::uint64_t count);

// --- Каталог строк -------------------------------------------------------------

// Ключ — стабильный идентификатор («cleanup.button.scan»), значение — шаблон.
// Пустая строка означает «перевода нет»: это позволяет принимать файл ресурсов
// с неполным переводом и находить такие ключи через keysMissingIn.
class StringCatalog {
public:
    StringCatalog() = default;

    // false — ключ уже переведён в этом языке (перевод не затирается молча)
    // или шаблон пуст (пустой строки в UI не бывает). Снять перевод можно
    // только через set с пустым шаблоном.
    bool add(Language lang, std::string_view key, std::string_view tmpl);
    void set(Language lang, std::string_view key, std::string_view tmpl);
    void clear();

    bool has(Language lang, std::string_view key) const;
    bool has(std::string_view key) const;  // есть хотя бы в одном языке

    std::size_t size() const;               // уникальных ключей
    std::size_t size(Language lang) const;  // переведённых строк в одном языке

    std::string raw(std::string_view key, Language lang) const;  // "" если нет
    bool isTranslated(std::string_view key, Language lang) const;

    // Ключи без перевода в этом языке (для приёмки §12 «локализации полны»).
    std::vector<std::string> keysMissingIn(Language lang) const;

    // Шаблон с разрешением языка: запрошенный → kDefaultLanguage → остальные →
    // сам ключ (его видно в UI, он ищется в исходниках).
    std::string resolve(std::string_view key, Language lang) const;

    // То же, но с выбором формы множественного числа. Форму берёт язык, на
    // котором строка реально нашлась: у ru форм три, у en две, и подстановка
    // индекса от запрошенного языка дала бы несуществующую форму.
    std::string resolvePlural(std::string_view key, std::uint64_t count, Language lang) const;

    // Язык, на котором реально нашлась строка (с тем же порядком fallback).
    std::optional<Language> findLanguage(std::string_view key, Language lang) const;

private:
    using Entry = std::array<std::string, 2>;  // индекс — static_cast<std::size_t>(Language)
    std::map<std::string, Entry> entries_;
};

// --- Процессное состояние локали ----------------------------------------------

// Язык приложения и каталог строк. Смена языка действует немедленно и без
// перезапуска: следующий вызов tr() уже отдаёт новый язык, а revision()
// меняется, чтобы UI перерисовал открытые экраны (SPEC §5, §4 FR «настройки»).
//
// Потокобезопасность: язык и revision — атомарные, каталог публикуется
// неизменяемым снимком (копирование при записи), поэтому отрисовка читает
// строки без блокировок и никогда не видит каталок наполовину.
class Localizer {
public:
    Localizer();
    ~Localizer();
    Localizer(const Localizer&) = delete;
    Localizer& operator=(const Localizer&) = delete;

    // Снимок каталога: держится, пока читателю нужны строки. UI может взять
    // один снимок на кадр и не брать блокировку на каждую строку.
    std::shared_ptr<const StringCatalog> catalog() const;

    bool addString(Language lang, std::string_view key, std::string_view tmpl);
    void setString(Language lang, std::string_view key, std::string_view tmpl);
    void loadCatalog(StringCatalog catalog);  // перезагрузка ресурсов целиком
    void clearStrings();

    Language language() const;
    void setLanguage(Language lang);   // меняет revision, только если язык изменился
    Language toggleLanguage();         // ru ↔ en; возвращает новый язык

    // Растёт при смене языка и при любом изменении каталога. UI запоминает
    // значение и перерисовывается, когда оно не совпало.
    std::uint64_t revision() const;

    NumberFormat numberFormat() const;
    void setNumberFormat(const NumberFormat& fmt);

    // Строка по ключу. Отсутствующий ключ отдаётся как есть — это видно.
    std::string tr(std::string_view key) const;
    std::string tr(std::string_view key, std::string_view arg0) const;
    std::string tr(std::string_view key, const StringArgs& args) const;
    std::string tr(std::string_view key, const NamedArgs& args) const;

    // Форма множественного числа; {0} — число в локальном формате, {1} и далее —
    // дополнительные параметры (имя диска, путь каталога).
    std::string trPlural(std::string_view key, std::uint64_t count) const;
    std::string trPlural(std::string_view key, std::uint64_t count, const StringArgs& extra) const;

    // Строки, для которых в текущем языке нет перевода (сверка полноты).
    std::vector<std::string> missingTranslations() const;

private:
    void mutate(const std::function<void(StringCatalog&)>& change);

    std::shared_ptr<StringCatalog> catalog_;
    mutable std::mutex catalogMutex_;

    std::atomic<Language> language_{kDefaultLanguage};
    std::atomic<std::uint64_t> revision_{1};

    mutable std::mutex numberMutex_;
    NumberFormat numberFormat_;
};

// Единственный экземпляр на процесс: язык — свойство приложения, а не модуля.
Localizer& localizer();

Language currentLanguage();
void setLanguage(Language lang);
std::uint64_t localizationRevision();

std::string tr(std::string_view key);
std::string tr(std::string_view key, std::string_view arg0);
std::string tr(std::string_view key, const StringArgs& args);
std::string trPlural(std::string_view key, std::uint64_t count);

// Ключи, для которых в текущем языке нет перевода.
std::vector<std::string> missingTranslations();

}  // namespace mrproper::core
