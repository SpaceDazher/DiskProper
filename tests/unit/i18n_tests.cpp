// Юнит-тесты core::i18n: каталог ru/en, переключение языка без перезапуска,
// подстановка параметров, формы множественного числа, локальные числа.
// Спека: §5 «Локализация». main() живёт в core_tests.cpp (tests/unit/CMakeLists.txt).
#include "harness.hpp"

#include <limits>

#include "i18n.hpp"

using mrproper::core::Language;
using mrproper::core::Localizer;
using mrproper::core::NamedArgs;
using mrproper::core::NumberFormat;
using mrproper::core::StringArgs;
using mrproper::core::StringCatalog;
using mrproper::core::TextDirection;

namespace {

// Каталог из двух-трёх строк, на котором проверяется разрешение языка.
StringCatalog sampleCatalog() {
    StringCatalog strings;
    strings.add(Language::Russian, "app.title", "MrProper — очистка диска");
    strings.add(Language::English, "app.title", "MrProper — disk cleanup");
    strings.add(Language::Russian, "cleanup.done", "Готово: {0} категорий");
    strings.add(Language::English, "cleanup.done", "Done: {0} categories");
    strings.add(Language::Russian, "cleanup.files", "{0} файл|{0} файла|{0} файлов");
    strings.add(Language::English, "cleanup.files", "{0} file|{0} files");
    // Ключ без английского перевода: в en должен прийти русский вариант.
    strings.add(Language::Russian, "undo.hint", "Отменить удаление");
    return strings;
}

const NumberFormat kRuFormat{',', ' '};
const NumberFormat kEnFormat{'.', ','};
const NumberFormat kPlainFormat{',', '\0'};

}  // namespace

// --- языки и направление письма ------------------------------------------------

TEST(i18n_languageTags) {
    CHECK_EQ(std::string(mrproper::core::languageTag(Language::Russian)), std::string("ru"));
    CHECK_EQ(std::string(mrproper::core::languageTag(Language::English)), std::string("en"));
    CHECK_EQ(std::string(mrproper::core::languageName(Language::Russian)), std::string("Русский"));
    CHECK_EQ(std::string(mrproper::core::languageName(Language::English)), std::string("English"));
}

TEST(i18n_parseLanguageAcceptsRegionAndCase) {
    CHECK(mrproper::core::parseLanguage("ru") == Language::Russian);
    CHECK(mrproper::core::parseLanguage("RU") == Language::Russian);
    CHECK(mrproper::core::parseLanguage("ru-RU") == Language::Russian);
    CHECK(mrproper::core::parseLanguage("ru_RU") == Language::Russian);
    CHECK(mrproper::core::parseLanguage("en-US") == Language::English);
    CHECK(mrproper::core::parseLanguage("EN") == Language::English);
}

TEST(i18n_parseLanguageRejectsUnknown) {
    CHECK(!mrproper::core::parseLanguage("de").has_value());
    CHECK(!mrproper::core::parseLanguage("").has_value());
    CHECK(!mrproper::core::parseLanguage("-RU").has_value());
}

TEST(i18n_directionIsLtrForRuEn) {
    // SPEC §5 «RTL-ready»: ветка под RTL есть, но ru и en — LTR.
    CHECK(mrproper::core::textDirection(Language::Russian) == TextDirection::LeftToRight);
    CHECK(mrproper::core::textDirection(Language::English) == TextDirection::LeftToRight);
    CHECK(!mrproper::core::isRtl(Language::Russian));
    CHECK(!mrproper::core::isRtl(Language::English));
}

// --- числа в локали ------------------------------------------------------------

TEST(i18n_formatIntegerGroupsThousands) {
    CHECK_EQ(mrproper::core::formatInteger(0, kRuFormat), std::string("0"));
    CHECK_EQ(mrproper::core::formatInteger(999, kRuFormat), std::string("999"));
    CHECK_EQ(mrproper::core::formatInteger(1000, kRuFormat), std::string("1 000"));
    CHECK_EQ(mrproper::core::formatInteger(1234567, kRuFormat), std::string("1 234 567"));
    CHECK_EQ(mrproper::core::formatInteger(1234567, kEnFormat), std::string("1,234,567"));
    CHECK_EQ(mrproper::core::formatInteger(1234567, kPlainFormat), std::string("1234567"));
}

TEST(i18n_formatDecimalUsesLocaleSeparators) {
    CHECK_EQ(mrproper::core::formatDecimal(12.34, 1, kRuFormat), std::string("12,3"));
    CHECK_EQ(mrproper::core::formatDecimal(12.34, 1, kEnFormat), std::string("12.3"));
    CHECK_EQ(mrproper::core::formatDecimal(0.0, 2, kRuFormat), std::string("0,00"));
    CHECK_EQ(mrproper::core::formatDecimal(1234.0, 0, kRuFormat), std::string("1 234"));
    CHECK_EQ(mrproper::core::formatDecimal(-1234.5, 1, kRuFormat), std::string("-1 234,5"));
    CHECK_EQ(mrproper::core::formatDecimal(12.34, 0, kEnFormat), std::string("12"));
    CHECK_EQ(mrproper::core::formatDecimal(5.0, -1, kEnFormat), std::string("5"));  // отрицательные знаки невозможны
}

TEST(i18n_formatDecimalDoesNotGroupSpecialValues) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::string nanText = mrproper::core::formatDecimal(nan, 2, kRuFormat);
    CHECK(nanText.find("nan") != std::string::npos || nanText.find("NAN") != std::string::npos);
    const double inf = std::numeric_limits<double>::infinity();
    const std::string infText = mrproper::core::formatDecimal(inf, 2, kRuFormat);
    CHECK(infText.find("inf") != std::string::npos || infText.find("INF") != std::string::npos);
}

// --- подстановка параметров ----------------------------------------------------

TEST(i18n_formatTemplatePositional) {
    CHECK_EQ(mrproper::core::formatTemplate("Диск {0}, свободно {1}", StringArgs{"C:", "12 ГБ"}),
             std::string("Диск C:, свободно 12 ГБ"));
    CHECK_EQ(mrproper::core::formatTemplate("Пусто", StringArgs{}), std::string("Пусто"));
}

TEST(i18n_formatTemplateNamed) {
    const NamedArgs args{{"disk", "D:"}, {"size", "3 ГБ"}};
    CHECK_EQ(mrproper::core::formatTemplate("{disk} свободно {size}", args), std::string("D: свободно 3 ГБ"));
}

TEST(i18n_formatTemplateKeepsUnknownPlaceholders) {
    // Пропущенный параметр обязан быть виден, а не съеден.
    CHECK_EQ(mrproper::core::formatTemplate("Готово: {0} из {1}", StringArgs{"3"}), std::string("Готово: 3 из {1}"));
    CHECK_EQ(mrproper::core::formatTemplate("Готово: {сколько}", StringArgs{"3"}), std::string("Готово: {сколько}"));
}

TEST(i18n_formatTemplateEscapesBraces) {
    CHECK_EQ(mrproper::core::formatTemplate("{{0}} — {0}", StringArgs{"7"}), std::string("{0} — 7"));
    CHECK_EQ(mrproper::core::formatTemplate("{{всё}}", StringArgs{}), std::string("{всё}"));
    CHECK_EQ(mrproper::core::formatTemplate("незакрытое {0", StringArgs{"7"}), std::string("незакрытое {0"));
}

TEST(i18n_formatTemplateIgnoresHugeIndex) {
    // Индекс не влезает в std::size_t — это не параметр, а текст.
    const StringArgs args{"a"};
    CHECK_EQ(mrproper::core::formatTemplate("{99999999999999999999}", args), std::string("{99999999999999999999}"));
}

TEST(i18n_pluralIndexFollowsCldr) {
    using mrproper::core::pluralFormIndex;
    // ru: one | few | many
    CHECK_EQ(pluralFormIndex(Language::Russian, 1), static_cast<std::size_t>(0));
    CHECK_EQ(pluralFormIndex(Language::Russian, 2), static_cast<std::size_t>(1));
    CHECK_EQ(pluralFormIndex(Language::Russian, 4), static_cast<std::size_t>(1));
    CHECK_EQ(pluralFormIndex(Language::Russian, 5), static_cast<std::size_t>(2));
    CHECK_EQ(pluralFormIndex(Language::Russian, 0), static_cast<std::size_t>(2));
    CHECK_EQ(pluralFormIndex(Language::Russian, 11), static_cast<std::size_t>(2));
    CHECK_EQ(pluralFormIndex(Language::Russian, 14), static_cast<std::size_t>(2));
    CHECK_EQ(pluralFormIndex(Language::Russian, 21), static_cast<std::size_t>(0));
    CHECK_EQ(pluralFormIndex(Language::Russian, 22), static_cast<std::size_t>(1));
    CHECK_EQ(pluralFormIndex(Language::Russian, 112), static_cast<std::size_t>(2));
    // en: one | other
    CHECK_EQ(pluralFormIndex(Language::English, 0), static_cast<std::size_t>(1));
    CHECK_EQ(pluralFormIndex(Language::English, 1), static_cast<std::size_t>(0));
    CHECK_EQ(pluralFormIndex(Language::English, 2), static_cast<std::size_t>(1));
}

TEST(i18n_selectPluralFormPicksAndFallsBackToLast) {
    using mrproper::core::selectPluralForm;
    const std::string ru = "{0} файл|{0} файла|{0} файлов";
    CHECK_EQ(selectPluralForm(ru, Language::Russian, 1), std::string("{0} файл"));
    CHECK_EQ(selectPluralForm(ru, Language::Russian, 3), std::string("{0} файла"));
    CHECK_EQ(selectPluralForm(ru, Language::Russian, 7), std::string("{0} файлов"));
    // Форм меньше, чем просит индекс, — берётся последняя, а не пустая строка.
    const std::string oneForm = "{0} файла";
    CHECK_EQ(selectPluralForm(oneForm, Language::Russian, 5), std::string("{0} файла"));
    const std::string noPipe = "{0} file";
    CHECK_EQ(selectPluralForm(noPipe, Language::English, 9), std::string("{0} file"));
    CHECK_EQ(selectPluralForm("", Language::English, 9), std::string(""));
}

// --- каталог -------------------------------------------------------------------

TEST(i18n_catalogAddDoesNotOverwriteSilently) {
    StringCatalog strings;
    CHECK(strings.add(Language::Russian, "a", "Первый"));
    CHECK(!strings.add(Language::Russian, "a", "Второй"));
    CHECK_EQ(strings.raw("a", Language::Russian), std::string("Первый"));
    CHECK(strings.add(Language::English, "a", "First"));
    CHECK(!strings.add(Language::Russian, "empty", ""));  // пустой строки не бывает
    CHECK(!strings.has(Language::Russian, "empty"));
    strings.set(Language::Russian, "a", "Обновлённый");
    CHECK_EQ(strings.raw("a", Language::Russian), std::string("Обновлённый"));
}

TEST(i18n_catalogCountsKeysAndTranslations) {
    const StringCatalog strings = sampleCatalog();
    CHECK_EQ(strings.size(), static_cast<std::size_t>(4));              // уникальных ключей
    CHECK_EQ(strings.size(Language::Russian), static_cast<std::size_t>(4));
    CHECK_EQ(strings.size(Language::English), static_cast<std::size_t>(3));
    CHECK(strings.has("app.title"));
    CHECK(!strings.has("no.such.key"));
    CHECK(strings.isTranslated("app.title", Language::English));
    CHECK(!strings.isTranslated("undo.hint", Language::English));
    CHECK_EQ(strings.raw("no.such.key", Language::Russian), std::string(""));
}

TEST(i18n_catalogReportsMissingTranslations) {
    const StringCatalog strings = sampleCatalog();
    const std::vector<std::string> missing = strings.keysMissingIn(Language::English);
    CHECK_EQ(missing.size(), static_cast<std::size_t>(1));
    CHECK_EQ(missing.front(), std::string("undo.hint"));
    CHECK(strings.keysMissingIn(Language::Russian).empty());
}

TEST(i18n_resolveFallsBackToDefaultThenToKey) {
    const StringCatalog strings = sampleCatalog();
    CHECK_EQ(strings.resolve("app.title", Language::English), std::string("MrProper — disk cleanup"));
    CHECK_EQ(strings.resolve("app.title", Language::Russian), std::string("MrProper — очистка диска"));
    // Перевода нет: отдаётся запасной язык, ключ не должен оставаться ключом.
    CHECK_EQ(strings.resolve("undo.hint", Language::English), std::string("Отменить удаление"));
    // Ключа нет нигде: возвращается сам ключ — его видно и можно найти grep'ом.
    CHECK_EQ(strings.resolve("no.such.key", Language::English), std::string("no.such.key"));
    CHECK(strings.findLanguage("no.such.key", Language::English) == std::nullopt);
    CHECK(strings.findLanguage("undo.hint", Language::English) == Language::Russian);
}

TEST(i18n_resolvePluralUsesFoundLanguageForms) {
    const StringCatalog strings = sampleCatalog();
    CHECK_EQ(strings.resolvePlural("cleanup.files", 1, Language::Russian), std::string("{0} файл"));
    CHECK_EQ(strings.resolvePlural("cleanup.files", 2, Language::Russian), std::string("{0} файла"));
    CHECK_EQ(strings.resolvePlural("cleanup.files", 5, Language::Russian), std::string("{0} файлов"));
    CHECK_EQ(strings.resolvePlural("cleanup.files", 1, Language::English), std::string("{0} file"));
    CHECK_EQ(strings.resolvePlural("cleanup.files", 5, Language::English), std::string("{0} files"));
    // Ключа нет — возвращается ключ, форма не выбирается.
    CHECK_EQ(strings.resolvePlural("no.such.key", 5, Language::English), std::string("no.such.key"));
}

// --- переключение языка без перезапуска ----------------------------------------

TEST(i18n_localizerSwitchesLanguageInPlace) {
    Localizer app;
    app.loadCatalog(sampleCatalog());
    CHECK(app.language() == Language::Russian);
    CHECK_EQ(app.tr("app.title"), std::string("MrProper — очистка диска"));

    app.setLanguage(Language::English);
    // Тот же экземпляр, тот же каталог — сменился только язык.
    CHECK(app.language() == Language::English);
    CHECK_EQ(app.tr("app.title"), std::string("MrProper — disk cleanup"));
    CHECK_EQ(app.tr("cleanup.done", StringArgs{"6"}), std::string("Done: 6 categories"));

    app.setLanguage(Language::Russian);
    CHECK_EQ(app.tr("app.title"), std::string("MrProper — очистка диска"));
}

TEST(i18n_localizerRevisionChangesOnlyOnRealChange) {
    Localizer app;
    const std::uint64_t start = app.revision();
    app.setLanguage(app.language());
    CHECK_EQ(app.revision(), start);  // тот же язык — перерисовывать нечего
    app.setLanguage(Language::English);
    CHECK(app.revision() > start);
    const std::uint64_t afterSwitch = app.revision();
    app.addString(Language::Russian, "new.key", "Новая строка");
    CHECK(app.revision() > afterSwitch);  // каталог тоже инвалидирует экран
}

TEST(i18n_localizerSnapshotSurvivesReload) {
    Localizer app;
    app.loadCatalog(sampleCatalog());
    const std::shared_ptr<const StringCatalog> before = app.catalog();
    app.setString(Language::Russian, "app.title", "Другое название");
    // Снимок, который UI взял на кадр, остаётся целым и неповреждённым.
    CHECK_EQ(before->raw("app.title", Language::Russian), std::string("MrProper — очистка диска"));
    CHECK_EQ(app.catalog()->raw("app.title", Language::Russian), std::string("Другое название"));
    CHECK_EQ(app.tr("app.title"), std::string("Другое название"));
}

TEST(i18n_localizerToggleLanguage) {
    Localizer app;
    app.loadCatalog(sampleCatalog());
    CHECK(app.toggleLanguage() == Language::English);
    CHECK_EQ(app.tr("app.title"), std::string("MrProper — disk cleanup"));
    CHECK(app.toggleLanguage() == Language::Russian);
    CHECK_EQ(app.tr("app.title"), std::string("MrProper — очистка диска"));
}

TEST(i18n_localizerNumberFormatFeedsTemplates) {
    Localizer app;
    app.loadCatalog(sampleCatalog());
    app.setNumberFormat(kRuFormat);
    CHECK_EQ(app.numberFormat().groupSeparator, ' ');
    // {0} — число в локали: ru «1 234 файла», en «1,234 files».
    CHECK_EQ(app.trPlural("cleanup.files", 1234), std::string("1 234 файла"));
    // Форма множественного числа берётся по языку строки, число — по NumberFormat:
    // переключаем язык, иначе форма осталась бы русской.
    app.setNumberFormat(kEnFormat);
    app.setLanguage(Language::English);
    CHECK_EQ(app.trPlural("cleanup.files", 1234), std::string("1,234 files"));
    app.setNumberFormat(kRuFormat);
    app.setLanguage(Language::Russian);
}

TEST(i18n_localizerTrPluralWithExtraArguments) {
    Localizer app;
    app.loadCatalog(sampleCatalog());
    app.setNumberFormat(kRuFormat);
    // Дополнительный параметр сдвигается: {1} — имя тома.
    StringCatalog strings;
    strings.add(Language::Russian, "trash.moved", "{0} файлов перемещено на {1}");
    app.loadCatalog(std::move(strings));
    CHECK_EQ(app.trPlural("trash.moved", 5, StringArgs{"C:"}), std::string("5 файлов перемещено на C:"));
}

TEST(i18n_localizerMissingTranslationsFollowLanguage) {
    Localizer app;
    app.loadCatalog(sampleCatalog());
    CHECK(app.missingTranslations().empty());
    app.setLanguage(Language::English);
    const std::vector<std::string> missing = app.missingTranslations();
    CHECK_EQ(missing.size(), static_cast<std::size_t>(1));
    CHECK_EQ(missing.front(), std::string("undo.hint"));
}

TEST(i18n_processWideLocalizerServesFreeFunctions) {
    const Language saved = mrproper::core::currentLanguage();
    mrproper::core::localizer().loadCatalog(sampleCatalog());
    mrproper::core::setLanguage(Language::Russian);
    CHECK_EQ(mrproper::core::tr("app.title"), std::string("MrProper — очистка диска"));
    CHECK_EQ(mrproper::core::tr("cleanup.done", StringArgs{"2"}), std::string("Готово: 2 категорий"));
    CHECK_EQ(mrproper::core::tr("undo.hint", "x"), std::string("Отменить удаление"));
    mrproper::core::setLanguage(Language::English);
    CHECK_EQ(mrproper::core::tr("app.title"), std::string("MrProper — disk cleanup"));
    CHECK_EQ(mrproper::core::trPlural("cleanup.files", 4), std::string("4 files"));
    CHECK_EQ(mrproper::core::missingTranslations().size(), static_cast<std::size_t>(1));
    const std::uint64_t revision = mrproper::core::localizationRevision();
    CHECK(revision > 0);
    mrproper::core::setLanguage(saved);
}
