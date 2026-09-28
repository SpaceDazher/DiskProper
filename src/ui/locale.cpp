// Локализация слоя UI — реализация. Спека: §5 «Локализация | ru + en, RTL-ready,
// переводы в ресурсах, даты/числа через GetLocaleInfoEx», §7, §12.
//
// Здесь и только здесь появляется windows.h. Смысл разделения тот же, что в
// platform/: WinAPI изолирован в одном файле, поэтому перенести его в ядро
// физически не получится (ADR-004), а заголовок остаётся пригодным для кода,
// который собирается без Windows.
//
// Что этот файл делает целиком, по шагам:
//
//   1. initialize() — читает RT_STRING из образа процесса для ru и en, затем
//      дополняет встроенным набором (строки из ресурсов приоритетнее, но не
//      затирают: core::StringCatalog::add отказывается переписывать перевод),
//      публикует каталог в core::Localizer, выбирает язык по локали ОС,
//      ставит SetThreadUILanguage и разделители чисел;
//   2. setLanguage() — те же шаги без чтения ресурсов, поэтому переключение
//      языка не перезапускает и не перечитывает ничего, кроме двух строк
//      настроек NLS;
//   3. RTL-ready — направление письма берётся у локали ОС, а раскладка
//      (края, выравнивание, порядок колонок, WS_EX_*) выводится из него
//      чистыми функциями, которые можно вызвать и проверить без окна.
#include "locale.hpp"

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/i18n.hpp"
#include "core/log.hpp"

namespace mrproper::ui {
namespace {

// --- Константы Win32, которых нет в заголовках ------------------------------

// Флаг dwFlags из EnumResourceNamesEx. В Windows SDK 10.0.19041 констант
// ENUM_RESOURCE_NAME/ENUM_RESOURCE_TYPE нет (проверено по заголовкам um), а
// объявление функции без них невозможно, поэтому значение взято из документации
// к API. Ошибка здесь была бы видна, а не опасна: перечисление вернуло бы ноль
// строк, а не мусор, и это видно в отчёте загрузки (resourcesFound = false).
inline constexpr DWORD kEnumResourceName = 2;

// WS_EX_RTLREADING и WS_EX_LEFTSCROLLBAR из WinUser.h. Перечислены здесь, а не
// подключением windows.h, потому что заголовок обязан оставаться без Win32
// (см. начало файла), а числа эти обязаны быть именно такими.
inline constexpr std::uint32_t kWsExRtlReading = 0x00002000u;
inline constexpr std::uint32_t kWsExLeftScrollbar = 0x00004000u;

// LANGID языков приложения (winnls.h): русский и английский.
constexpr std::uint16_t kLangIdNeutral = 0x0000;
constexpr std::uint16_t kLangIdRussian = 0x0419;
constexpr std::uint16_t kLangIdEnglish = 0x0409;

// Разница между unix-эпохой и эпохой FILETIME (1601-01-01) в секундах:
// 11644473600. Через неё модель (core::model, unix-секунды) превращается в
// SYSTEMTIME, который понимают GetDateFormatEx и GetTimeFormatEx.
constexpr std::int64_t kUnixToFileTimeSeconds = 11644473600LL;

// Границы размера строки даты: начинаем с 64 символов, растём вдвое до 1024.
// Бесконечного роста здесь быть не должно — иначе формат, который система
// вдруг вернёт на 5000 символов, превратит открытие экрана в зависание.
constexpr int kDateBufferStart = 64;
constexpr int kDateBufferLimit = 1024;

// core::TextDirection — это {LeftToRight = 0, RightToLeft = 1}, а состояние
// модуля хранит «0 — по локали ОС, 1 — LTR, 2 — RTL»: ноль занят признаком
// «принудительного направления нет», поэтому направление переводится
// отдельной функцией.
constexpr int directionValue(TextDirection direction) noexcept {
    return direction == TextDirection::RightToLeft ? 2 : 1;
}

// --- Состояние модуля --------------------------------------------------------

std::atomic<bool> g_initialized{false};
std::atomic<std::uint64_t> g_layoutRevision{1};

// Принудительное направление: 0 — по локали ОС, 1 — LTR, 2 — RTL.
std::atomic<int> g_directionOverride{0};
// Направление по локали ОС, вычисленное при старте и по WM_SETTINGCHANGE.
std::atomic<int> g_systemDirection{directionValue(TextDirection::LeftToRight)};

// --- RTL: языки с письмом справа налево ---------------------------------------

// Первичные коды языков, которые Windows пишет справа налево. Список — из
// BCP-47 (rtl subtags), а не «все языки страны»: арабский в Саудовской Аравии и
// в Египте пишется одинаково, а курдский (0x21) специально не включён — в
// большинстве систем он латиницей, и ошибочное зеркалирование всего интерфейса
// хуже, чем неверное направление у курдского пользователя. Русский (0x19) и
// английский (0x09) — LTR.
bool isRtlPrimaryLanguage(WORD primary) noexcept {
    switch (primary) {
        case 0x01:  // арабский
        case 0x0D:  // иврит
        case 0x23:  // пушту
        case 0x29:  // персидский
        case 0x2F:  // сирийский
        case 0x3F:  // идиш
        case 0x67:  // балучи
        case 0x7C:  // н'ко
        case 0x80:  // урду
        case 0x83:  // синдхи
        case 0x85:  // уйгурский
        case 0x92:  // дивехи
        case 0xC1:  // курманский (центральный)
            return true;
        default:
            return false;
    }
}

TextDirection querySystemDirection() noexcept {
    LANGID language = ::GetUserDefaultUILanguage();
    if (language == 0) language = ::GetSystemDefaultUILanguage();
    if (language == 0) return TextDirection::LeftToRight;
    if (isRtlPrimaryLanguage(PRIMARYLANGID(language))) return TextDirection::RightToLeft;
    return TextDirection::LeftToRight;
}

TextDirection currentDirection() noexcept {
    const int forced = g_directionOverride.load(std::memory_order_acquire);
    if (forced == 1) return TextDirection::LeftToRight;
    if (forced == 2) return TextDirection::RightToLeft;
    return g_systemDirection.load(std::memory_order_acquire) == 2 ? TextDirection::RightToLeft
                                                                   : TextDirection::LeftToRight;
}

// Обновить кэш направления по локали ОС и, если направление поменялось, поднять
// счётчик раскладки: пересчитывать её должны все открытые экраны, а не тот,
// кто первым заметил (SPEC §6.4 — один UI-поток читает атомики).
void cacheSystemDirection() noexcept {
    const int fresh = directionValue(querySystemDirection());
    const int previous = g_systemDirection.exchange(fresh, std::memory_order_acq_rel);
    if (previous != fresh) ++g_layoutRevision;
}

// --- Локали и числа ----------------------------------------------------------

std::uint16_t langIdOf(Language lang) noexcept {
    return lang == Language::English ? kLangIdEnglish : kLangIdRussian;
}

// Имя локали по BCP-47. Числа и даты берём из локали языка интерфейса, а не из
// локали системы: переключение языка обязано менять и разделители, и образец
// даты, иначе «1.2 GB» в русском интерфейсе выглядит как баг. Обе локали есть в
// NLS-таблицах любой Windows 10/11 независимо от установленных языков
// интерфейса, поэтому подстановки не будет.
const wchar_t* localeNameOf(Language lang) noexcept {
    return lang == Language::English ? L"en-US" : L"ru-RU";
}

bool sameFormat(const NumberFormat& left, const NumberFormat& right) noexcept {
    return left.decimalSeparator == right.decimalSeparator && left.groupSeparator == right.groupSeparator;
}

// Один символ из LOCALE_SDECIMAL и подобных строковых настроек. Длина такой
// строки задана системой, поэтому при нехватке буфера читаем по одному символу
// и не сдаёмся: разделитель нужен всегда.
bool readLocaleChar(const wchar_t* localeName, LCTYPE type, char& out) noexcept {
    std::array<wchar_t, 16> buffer{};
    const int written = ::GetLocaleInfoEx(localeName, type, buffer.data(), static_cast<int>(buffer.size()));
    if (written > 0) {
        out = static_cast<char>(buffer[0]);
        return true;
    }
    buffer[0] = 0;
    if (::GetLocaleInfoEx(localeName, type, buffer.data(), 1) == 1 && buffer[0] != 0) {
        out = static_cast<char>(buffer[0]);
        return true;
    }
    return false;
}

// Разделитель групп для core::i18n (группировка тройками).
//
// LOCALE_SGROUPING отдаёт РАЗМЕРЫ групп, а не символ: у ru-RU это «3;0» (группа
// по три цифры, дальше без группировки), у en-US — просто «3». Символ лежит в
// LOCALE_STHOUSAND, и брать его из LOCALE_SGROUPING нельзя: там после «3;0» стоит
// ноль, а не запятая, а в en-US нет и его — вернулось бы «13 234» вместо
// «1,234».
//
// Размер группы сверяется с тройкой не ради точности, а ради честности:
// core::i18n::groupDigits делит на тройки всегда, и локаль с другой схемой
// получила бы разделитель не там, где его ждут. Лучше «1234567», чем
// «12,34,567».
char readGroupSeparator(const wchar_t* localeName) noexcept {
    std::array<wchar_t, 16> buffer{};
    std::wstring_view text;
    const int written = ::GetLocaleInfoEx(localeName, LOCALE_SGROUPING, buffer.data(),
                                          static_cast<int>(buffer.size()));
    if (written > 0) {
        text = std::wstring_view(buffer.data(), static_cast<std::size_t>(written));
    } else {
        buffer[0] = 0;
        if (::GetLocaleInfoEx(localeName, LOCALE_SGROUPING, buffer.data(), 1) != 1) return '\0';
        text = std::wstring_view(buffer.data(), 1);
    }
    if (text.empty()) return '\0';

    const std::size_t semicolon = text.find(L';');
    const std::wstring_view head = semicolon == std::wstring_view::npos ? text : text.substr(0, semicolon);

    std::size_t groupSize = 0;
    for (const wchar_t digit : head) {
        if (digit < L'0' || digit > L'9') break;
        groupSize = groupSize * 10 + static_cast<std::size_t>(digit - L'0');
        if (groupSize > 9) break;  // группа длиннее девяти цифр не встречается
    }
    if (groupSize == 0) return '\0';  // группировки нет вовсе
    if (groupSize != 3) {
        MRP_LOG_DEBUG("ui.locale.group.unsupported", "схема группировки не тройками, разделитель отключён");
        return '\0';
    }

    char separator = '\0';
    if (!readLocaleChar(localeName, LOCALE_STHOUSAND, separator)) return '\0';
    return separator;
}

// Образец даты локали (LOCALE_SSHORTDATE, LOCALE_SLONGDATE). Пишет в буфер
// вызывающего, а не в общий: короткий и длинный образцы нужны одновременно, и
// одна переменная на оба перетирала бы первый вторым.
const wchar_t* readLocalePattern(const wchar_t* localeName, LCTYPE type,
                                 std::array<wchar_t, 128>& buffer) noexcept {
    const int written = ::GetLocaleInfoEx(localeName, type, buffer.data(), static_cast<int>(buffer.size()));
    if (written <= 0 || written >= static_cast<int>(buffer.size())) return nullptr;
    buffer[static_cast<std::size_t>(written)] = 0;
    return buffer.data();
}

NumberFormat readNumberFormat(Language lang) noexcept {
    const wchar_t* name = localeNameOf(lang);
    NumberFormat format;  // русские значения по умолчанию
    char decimal = format.decimalSeparator;
    if (readLocaleChar(name, LOCALE_SDECIMAL, decimal)) format.decimalSeparator = decimal;
    format.groupSeparator = readGroupSeparator(name);
    return format;
}

// --- Перекодировка -----------------------------------------------------------

std::wstring convertToWide(std::string_view utf8, DWORD flags) noexcept {
    if (utf8.empty()) return std::wstring();
    const int sourceLength = static_cast<int>(utf8.size());
    int needed = ::MultiByteToWideChar(CP_UTF8, flags, utf8.data(), sourceLength, nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, flags, utf8.data(), sourceLength, result.data(), needed);
    return result;
}

// --- Поля журнала ------------------------------------------------------------
//
// Списки полей собираются явно, а не макросом MRP_LOG_*: core::detail::logFieldList
// раскрывает пакет в один вызов logField, и список полей на MSVC не
// разрешается (C2661) — ровно то, из-за чего platform::devices и engine::executor
// пишут так же. Макрос в этом проекте работает только для записей без полей:
// даже одна пара «ключ, значение» даёт C2661.

core::LogFields textField(const char* key, std::string_view value) {
    core::LogFields fields;
    fields.push_back(core::logField(key, value));
    return fields;
}

core::LogFields countField(const char* key, std::size_t value) {
    core::LogFields fields;
    fields.push_back(core::logField(key, value));
    return fields;
}

core::LogFields textAndCountField(const char* textKey, std::string_view text, const char* countKey,
                                  std::size_t count) {
    core::LogFields fields;
    fields.push_back(core::logField(textKey, text));
    fields.push_back(core::logField(countKey, count));
    return fields;
}

// Списки из двух и более полей пишутся через core::logXxx напрямую: макрос
// MRP_LOG_* раскрывается в logFieldList, который на этом MSVC не разрешает
// список полей даже из одной пары. Макрос остаётся для записей без полей.

// Любая функция этого файла, помеченная noexcept, обязана пережить нехватку
// памяти: std::string, std::wstring и core::i18n бросают bad_alloc, а прерванная
// отрисовка из-за нехватки памяти — это std::terminate на весь процесс, то есть
// ровно та поломка, от которой утилита обязана защищать пользователя (SPEC §5,
// «ни один отказ не роняет процесс»). Поэтому тело выполняется под guard и в
// неудаче возвращается безопасный пустой результат: пустая строка в интерфейсе
// видна, падение процесса — нет.
template <typename Result, typename Body>
Result guard(Body&& body, Result fallback) noexcept {
    try {
        return body();
    } catch (...) {
        MRP_LOG_ERROR("ui.locale.outOfMemory", "не удалось построить строку интерфейса");
        return fallback;
    }
}

// --- Ресурсы -----------------------------------------------------------------

// Имя ресурса — это либо MAKEINTRESOURCE(id) (младшие 16 бит указателя), либо
// настоящая строка. Различать обязательно: у RT_STRING числовой идентификатор
// означает блок из 16 строк, а именованный — одну строку целиком.
bool isResourceId(LPCWSTR name, unsigned& id) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(name);
    if ((value & ~static_cast<std::uintptr_t>(0xFFFFu)) != 0) return false;
    id = static_cast<unsigned>(value & 0xFFFFu);
    return id != 0;
}

struct ResourceLoadContext {
    Language language{};
    std::uint16_t langId{};
    core::StringCatalog* catalog{};
    std::size_t resources{};  // ресурсов RT_STRING просмотрено
    std::size_t strings{};    // строк добавлено в каталог
    bool warned{false};       // предупреждение о номере блока уже выдано
};

void addResourceString(std::string_view key, std::wstring_view text, ResourceLoadContext& context) {
    if (key.empty()) return;
    const std::string value = toUtf8(text);
    if (context.catalog->add(context.language, key, value)) {
        ++context.strings;
    }
}

// Классический блок RT_STRING: 16 слов с длинами, за ними сами строки.
// Проверка «сумма длин + 32 байта равна числу слов ресурса» — не оптимизация, а
// способ отличить блок от одной строки: иначе первое слово кириллицы
// («Сканировать» = 0x0441 0x0430 …) было бы принято за длину в 1089 символов.
void addFromBlockResource(const std::uint16_t* words, std::size_t wordCount, unsigned blockId,
                          ResourceLoadContext& context) {
    std::size_t offset = 16;
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t length = offset <= wordCount ? words[index] : 0;
        if (offset + length > wordCount) {
            if (!context.warned) {
                context.warned = true;
                core::LogFields fields = countField("block", blockId);
                fields.push_back(core::logField("index", index));
                fields.push_back(core::logField("length", length));
                core::logWarn("ui.locale.resource.truncated", "строка в блоке RT_STRING выходит за границу",
                              fields);
            }
            return;
        }
        const std::wstring_view text(reinterpret_cast<const wchar_t*>(words + offset), length);
        if (!text.empty()) {
            const unsigned id = (blockId - 1) * 16 + static_cast<unsigned>(index) + 1;
            addResourceString("rs." + std::to_string(id), text, context);
        }
        offset += length;
    }
}

// Выглядит ли содержимое ресурса как блок из 16 строк. Используется только для
// именованных ресурсов: числовой блок разбирается всегда, а ошибочно
// «заблокированный» именованный ресурс читается как одна строка с заменой.
bool looksLikeBlock(const std::uint16_t* words, std::size_t wordCount) noexcept {
    if (wordCount < 16) return false;
    std::size_t total = 16;
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t length = words[index];
        if (length > wordCount) return false;
        total += length;
    }
    return total == wordCount;
}

BOOL CALLBACK enumStringResource(HMODULE module, LPCWSTR /*type*/, LPWSTR name, LONG_PTR parameter) {
    auto* context = reinterpret_cast<ResourceLoadContext*>(parameter);
    if (context == nullptr || name == nullptr) return TRUE;  // продолжать перечисление

    unsigned resourceId = 0;
    const bool byId = isResourceId(name, resourceId);

    const HRSRC info = ::FindResourceExW(module, RT_STRING, name, context->langId);
    if (info == nullptr) return TRUE;
    HGLOBAL block = ::LoadResource(module, info);
    if (block == nullptr) return TRUE;
    const DWORD bytes = ::SizeofResource(module, info);
    // LockResource намеренно не освобождается: ресурс живёт, пока жив модуль, а
    // FreeResource здесь означал бы второе обращение к уже освобождённому.
    const auto* words = static_cast<const std::uint16_t*>(::LockResource(block));
    if (words == nullptr || bytes < sizeof(std::uint16_t)) return TRUE;
    ++context->resources;

    const std::size_t wordCount = static_cast<std::size_t>(bytes) / sizeof(std::uint16_t);
    std::wstring_view text(reinterpret_cast<const wchar_t*>(words), wordCount);

    if (byId) {
        addFromBlockResource(words, wordCount, resourceId, *context);
        return TRUE;
    }

    if (looksLikeBlock(words, wordCount)) {
        // Именованный ресурс, записанный как блок: берём первую строку и
        // предупреждаем. Молча прочитанное «длина 1089» дало бы в интерфейсе
        // мусор вместо перевода.
        core::logWarn("ui.locale.resource.namedBlock",
                      "именованная строка RT_STRING записана как блок из 16 строк, взята первая",
                      textField("key", toUtf8(std::wstring_view(name))));
        const std::size_t length = words[0];
        if (length <= wordCount - 16) {
            addResourceString(toUtf8(std::wstring_view(name)),
                              std::wstring_view(reinterpret_cast<const wchar_t*>(words + 16), length), *context);
        }
        return TRUE;
    }

    // Обычная именованная строка. Хвостовой L'\0' в ресурсе не часть строки.
    while (!text.empty() && text.back() == L'\0') text.remove_suffix(1);
    addResourceString(toUtf8(std::wstring_view(name)), text, *context);
    return TRUE;
}

std::size_t loadFromModule(Language lang, core::StringCatalog& catalog) noexcept {
    const HMODULE module = ::GetModuleHandleW(nullptr);
    if (module == nullptr) return 0;

    std::size_t total = 0;
    // Две попытки: язык приложения и ресурсы без явного LANGUAGE. Обе заполняют
    // один каталог, а add() не переписывает уже переведённое, поэтому при двух
    // успешных проходах выигрывает первый.
    for (const std::uint16_t langId : {langIdOf(lang), kLangIdNeutral}) {
        ResourceLoadContext context;
        context.language = lang;
        context.langId = langId;
        context.catalog = &catalog;
        ::EnumResourceNamesExW(module, RT_STRING, enumStringResource, reinterpret_cast<LONG_PTR>(&context),
                                kEnumResourceName, langId);
        if (context.resources > 0) {
            core::LogFields fields = countField("langId", static_cast<std::size_t>(langId));
            fields.push_back(core::logField("language", core::languageTag(lang)));
            fields.push_back(core::logField("resources", context.resources));
            fields.push_back(core::logField("strings", context.strings));
            core::logDebug("ui.locale.resource.loaded", "строки прочитаны из ресурсов модуля", fields);
        }
        total += context.strings;
    }
    return total;
}

std::size_t addBuiltInStrings(core::StringCatalog& catalog) noexcept {
    std::size_t added = 0;
    for (const Translation& entry : kTranslations) {
        if (catalog.add(Language::Russian, entry.key, entry.ru)) ++added;
        if (catalog.add(Language::English, entry.key, entry.en)) ++added;
    }
    return added;
}

}  // namespace

// ---------------------------------------------------------------------------
// Проверка встроенного набора
// ---------------------------------------------------------------------------

SelfCheck selfCheck() noexcept {
    return guard<SelfCheck>(
        [] {
            SelfCheck result;
            result.keys = kTranslations.size();

            std::vector<std::string_view> seen;
            seen.reserve(kTranslations.size());

            for (const Translation& entry : kTranslations) {
                if (entry.key.empty()) {
                    ++result.emptyRu;
                    ++result.emptyEn;
                    continue;
                }
                if (std::find(seen.begin(), seen.end(), entry.key) != seen.end()) ++result.duplicateKeys;
                seen.push_back(entry.key);

                if (entry.ru.empty()) ++result.emptyRu;
                if (entry.en.empty()) ++result.emptyEn;
                if (entry.ru.empty() && !entry.en.empty()) ++result.untranslatedRu;
                if (entry.en.empty() && !entry.ru.empty()) ++result.untranslatedEn;
            }
            return result;
        },
        SelfCheck{});
}

// ---------------------------------------------------------------------------
// Загрузка и переключение языка
// ---------------------------------------------------------------------------

namespace {

// Общая часть initialize() и reloadStrings(): читает строки, публикует каталог и
// применяет язык. applyLanguage = false — язык не трогаем, он уже выбран
// пользователем (кнопка «Вернуть встроенный набор» не должна сбрасывать его на
// язык локали ОС).
LocaleReport loadInto(Language language, bool applyLanguage) noexcept {
    LocaleReport report;
    try {
        core::StringCatalog catalog;
        report.resourceStrings = loadFromModule(Language::Russian, catalog);
        report.resourceStrings += loadFromModule(Language::English, catalog);
        report.resourcesFound = report.resourceStrings > 0;
        report.builtInStrings = addBuiltInStrings(catalog);

        core::localizer().loadCatalog(std::move(catalog));
        const std::shared_ptr<const core::StringCatalog> loaded = core::localizer().catalog();
        report.translatedRu = loaded->size(Language::Russian);
        report.translatedEn = loaded->size(Language::English);
        report.missingRu = loaded->keysMissingIn(Language::Russian);
        report.missingEn = loaded->keysMissingIn(Language::English);

        if (applyLanguage) mrproper::ui::setLanguage(language);
        report.language = currentLanguage();
        report.win32Language = ::GetThreadUILanguage();
        if (report.win32Language == 0) report.lastError = ::GetLastError();
        g_initialized.store(true, std::memory_order_release);

        const SelfCheck check = selfCheck();
        core::LogFields fields;
        fields.push_back(core::logField("language", core::languageTag(report.language)));
        fields.push_back(core::logField("langId", static_cast<int>(report.win32Language)));
        fields.push_back(core::logField("fromResources", report.resourceStrings));
        fields.push_back(core::logField("builtIn", report.builtInStrings));
        fields.push_back(core::logField("translatedRu", report.translatedRu));
        fields.push_back(core::logField("translatedEn", report.translatedEn));
        fields.push_back(core::logField("missingRu", report.missingRu.size()));
        fields.push_back(core::logField("missingEn", report.missingEn.size()));
        fields.push_back(core::logField("tableKeys", check.keys));
        fields.push_back(core::logField("tableComplete", check.complete()));
        core::logInfo("ui.locale.loaded", "локализация загружена", fields);

        if (!report.missingRu.empty() || !report.missingEn.empty()) {
            core::LogFields missing = textAndCountField("language", core::languageTag(report.language),
                                                        "missing", report.missingRu.size() +
                                                                         report.missingEn.size());
            core::logWarn("ui.locale.incomplete", "часть ключей не переведена (SPEC §12 «локализации полны»)",
                          missing);
        }
    } catch (...) {
        report.ok = false;
        // Код Win32 здесь неприменим: сбой пришёл из памяти или контейнера
        // строк, а не из системного вызова. Поле остаётся нулевым намеренно,
        // чтобы в отчёте не было чужого GetLastError.
        MRP_LOG_ERROR("ui.locale.load.failed", "не удалось загрузить строки интерфейса");
    }
    return report;
}

}  // namespace

LocaleReport initialize() noexcept {
    cacheSystemDirection();
    return loadInto(startupLanguage(), true);
}

LocaleReport reloadStrings() noexcept {
    return loadInto(currentLanguage(), false);
}

bool isInitialized() noexcept {
    return g_initialized.load(std::memory_order_acquire);
}

Language startupLanguage() noexcept {
    LANGID language = ::GetUserDefaultUILanguage();
    if (language == 0) language = ::GetSystemDefaultUILanguage();
    switch (language) {
        case kLangIdEnglish:
            return Language::English;
        case kLangIdRussian:
            return Language::Russian;
        default:
            break;
    }
    // Неизвестная локаль: русский, потому что он первый полный язык приложения
    // (core::i18n::kDefaultLanguage) и запасной при отсутствии перевода.
    return core::kDefaultLanguage;
}

Language currentLanguage() noexcept {
    return core::localizer().language();
}

Language setLanguage(Language lang) noexcept {
    try {
        core::localizer().setLanguage(lang);

        const LANGID previous = ::SetThreadUILanguage(langIdOf(lang));
        if (previous == 0) {
            core::LogFields fields;
            fields.push_back(core::logField("language", core::languageTag(lang)));
            fields.push_back(core::logField("error", ::GetLastError()));
            core::logWarn("ui.locale.win32.failed", "SetThreadUILanguage не принял язык", fields);
        }
        const NumberFormat format = readNumberFormat(lang);
        if (!sameFormat(format, core::localizer().numberFormat())) {
            core::localizer().setNumberFormat(format);
        }
        core::LogFields switched;
        switched.push_back(core::logField("language", core::languageTag(lang)));
        switched.push_back(core::logField("threadLangId", static_cast<int>(::GetThreadUILanguage())));
        core::logDebug("ui.locale.language", "язык интерфейса переключён", switched);
    } catch (...) {
        MRP_LOG_ERROR("ui.locale.language.failed", "смена языка интерфейса прервана");
    }
    return core::localizer().language();
}

Language toggleLanguage() noexcept {
    // Вызов квалифицирован: аргумент типа core::Language приносит через ADL
    // core::setLanguage, и без имени слоя вызов был бы неоднозначным.
    return mrproper::ui::setLanguage(core::localizer().language() == Language::Russian ? Language::English
                                                                                        : Language::Russian);
}

std::uint64_t revision() noexcept {
    return core::localizer().revision();
}

std::uint64_t layoutRevision() noexcept {
    return g_layoutRevision.load(std::memory_order_acquire);
}

std::vector<std::string> missingTranslations() noexcept {
    return guard<std::vector<std::string>>([] { return core::localizer().missingTranslations(); },
                                           std::vector<std::string>{});
}

// ---------------------------------------------------------------------------
// Строки
// ---------------------------------------------------------------------------

std::string tr(StringId id) noexcept {
    return guard<std::string>([id] { return core::localizer().tr(keyOf(id)); }, std::string());
}

std::string tr(std::string_view key) noexcept {
    return guard<std::string>([key] { return core::localizer().tr(key); }, std::string());
}

std::string tr(StringId id, std::string_view arg0) noexcept {
    return guard<std::string>([id, arg0] { return core::localizer().tr(keyOf(id), arg0); }, std::string());
}

std::string tr(StringId id, const core::StringArgs& args) noexcept {
    return guard<std::string>([id, &args] { return core::localizer().tr(keyOf(id), args); }, std::string());
}

std::string tr(StringId id, const core::NamedArgs& args) noexcept {
    return guard<std::string>([id, &args] { return core::localizer().tr(keyOf(id), args); }, std::string());
}

std::wstring trWide(StringId id) noexcept {
    return toWide(tr(id));
}

std::wstring trWide(std::string_view key) noexcept {
    return toWide(tr(key));
}

std::wstring trPluralWide(StringId id, std::uint64_t count) noexcept {
    return toWide(trPlural(id, count));
}

std::string trPlural(StringId id, std::uint64_t count) noexcept {
    return guard<std::string>([id, count] { return core::localizer().trPlural(keyOf(id), count); },
                              std::string());
}

std::string trPlural(StringId id, std::uint64_t count, const core::StringArgs& extra) noexcept {
    return guard<std::string>(
        [id, count, &extra] { return core::localizer().trPlural(keyOf(id), count, extra); }, std::string());
}

std::string categoryKey(std::string_view categoryId) noexcept {
    return guard<std::string>(
        [categoryId] {
            if (categoryId.empty()) return std::string();
            const std::string full = "cleanup.category." + std::string(categoryId);
            for (const Translation& entry : kTranslations) {
                if (entry.key == full) return full;
            }
            return std::string();
        },
        std::string());
}

std::string categoryName(std::string_view ruleId, std::string_view ruleCategory,
                         std::string_view ruleTitle) noexcept {
    return guard<std::string>(
        [ruleId, ruleCategory, ruleTitle] {
            if (!ruleTitle.empty()) return std::string(ruleTitle);  // заголовок правила (FR-4)
            const std::string key = categoryKey(ruleCategory);
            if (!key.empty()) return tr(key);
            if (!ruleCategory.empty()) return std::string(ruleCategory);
            return std::string(ruleId);
        },
        std::string());
}

// ---------------------------------------------------------------------------
// Числа, объёмы и даты
// ---------------------------------------------------------------------------

NumberFormat numberFormat() noexcept {
    return core::localizer().numberFormat();
}

std::wstring localeName() noexcept {
    return guard<std::wstring>([] { return std::wstring(localeNameOf(core::localizer().language())); },
                               std::wstring());
}

std::string formatBytes(std::uint64_t bytes, int decimals) noexcept {
    return guard<std::string>(
        [bytes, decimals] {
            int places = decimals;
            if (places < 0) places = 0;
            if (places > 6) places = 6;

            static constexpr std::array<StringId, 6> units{
                StringId::kUnitByte,    StringId::kUnitKilobyte, StringId::kUnitMegabyte,
                StringId::kUnitGigabyte, StringId::kUnitTerabyte, StringId::kUnitPetabyte};
            constexpr double kStep = 1000.0;

            if (bytes < 1000) {
                return core::formatInteger(bytes, core::localizer().numberFormat()) + " " + tr(units[0]);
            }
            double value = static_cast<double>(bytes);
            std::size_t unit = 0;
            while (value >= kStep && unit + 1 < units.size()) {
                value /= kStep;
                ++unit;
            }
            // «1 000,0 МБ» хуже, чем «1 000 МБ»: после десяти знак перед
            // запятой — предел точности измерения, а не точность.
            const int shown = value < 10.0 ? places : 0;
            return core::formatDecimal(value, shown, core::localizer().numberFormat()) + " " + tr(units[unit]);
        },
        std::string());
}

std::string formatCount(std::uint64_t count) noexcept {
    return trPlural(StringId::kCleanupFiles, count);
}

std::string formatPercent(double fraction, int decimals) noexcept {
    return guard<std::string>(
        [fraction, decimals] {
            int places = decimals;
            if (places < 0) places = 0;
            if (places > 6) places = 6;
            return core::formatDecimal(fraction * 100.0, places, core::localizer().numberFormat()) + " %";
        },
        std::string());
}

std::string formatAge(std::int64_t seconds) noexcept {
    return guard<std::string>(
        [seconds] {
            const NumberFormat format = core::localizer().numberFormat();
            if (seconds < 0) return tr(StringId::kAgeNow);
            if (seconds < 60) {
                return tr(StringId::kAgeSeconds,
                          core::formatInteger(static_cast<std::uint64_t>(seconds), format));
            }
            if (seconds < 3600) {
                return tr(StringId::kAgeMinutes,
                          core::formatInteger(static_cast<std::uint64_t>(seconds / 60), format));
            }
            if (seconds < 86400) {
                return trPlural(StringId::kAgeHours, static_cast<std::uint64_t>(seconds / 3600));
            }
            const std::int64_t days = seconds / 86400;
            if (days < 31) return trPlural(StringId::kAgeDays, static_cast<std::uint64_t>(days));
            if (days < 365) return trPlural(StringId::kAgeMonths, static_cast<std::uint64_t>(days / 30));
            return trPlural(StringId::kAgeYears, static_cast<std::uint64_t>(days / 365));
        },
        std::string());
}

std::wstring formatDate(std::int64_t unixSecondsUtc, DateStyle style) noexcept {
    try {
        // unix-секунды → FILETIME → SYSTEMTIME (UTC) → локальное время. Всё в
        // long long: 2038 год не должен ломать дату в отчёте.
        const LONGLONG fileTime =
            static_cast<LONGLONG>(unixSecondsUtc + kUnixToFileTimeSeconds) * 10000000LL;
        if (fileTime < 0) return std::wstring();

        FILETIME utcFileTime{};
        utcFileTime.dwLowDateTime = static_cast<DWORD>(fileTime & 0xFFFFFFFFLL);
        utcFileTime.dwHighDateTime = static_cast<DWORD>((fileTime >> 32) & 0xFFFFFFFFLL);

        SYSTEMTIME utc{};
        if (::FileTimeToSystemTime(&utcFileTime, &utc) == 0) return std::wstring();
        SYSTEMTIME local{};
        if (::SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local) == 0) local = utc;

        const wchar_t* name = localeNameOf(core::localizer().language());

        // Образец даты берём у локали явно (LOCALE_SSHORTDATE / LOCALE_SLONGDATE)
        // и передаём его в lpFormat, а не полагаемся на «формат по умолчанию»:
        // так поведение одинаково на всех сборках Windows. Если образец по
        // какой-то причине не прочитан, остаётся запасной путь с nullptr.
        std::array<wchar_t, 128> shortBuffer{};
        std::array<wchar_t, 128> longBuffer{};
        const wchar_t* shortPattern = readLocalePattern(name, LOCALE_SSHORTDATE, shortBuffer);
        const wchar_t* longPattern = readLocalePattern(name, LOCALE_SLONGDATE, longBuffer);

        // Форматирующая функция возвращает длину результата или 0; сначала
        // спрашиваем размер, потом пишем. Рост буфера ограничен, чтобы
        // неожиданно длинный формат не превращал отрисовку в зависание.
        // Порядок аргументов GetDateFormatEx/GetTimeFormatEx: буфер идёт ПЕРЕД
        // его размером (lpDateStr, cchDate) — наоборот компилятор не ловит.
        const auto call = [name](auto&& formatter) -> std::wstring {
            int size = kDateBufferStart;
            for (;;) {
                std::wstring buffer(static_cast<std::size_t>(size), L'\0');
                const int written = formatter(buffer.data(), size);
                if (written <= 0) {
                    if (size < kDateBufferLimit) {
                        size *= 2;
                        continue;
                    }
                    MRP_LOG_WARN("ui.locale.date.failed", "система не отдала дату для локали");
                    return std::wstring();
                }
                buffer.resize(static_cast<std::size_t>(written));
                return buffer;
            }
        };

        switch (style) {
            case DateStyle::ShortDate:
                return call([name, &local, shortPattern](wchar_t* buffer, int size) {
                    const int written = ::GetDateFormatEx(name, 0, &local, shortPattern, buffer, size, nullptr);
                    return written > 0 ? written
                                      : ::GetDateFormatEx(name, 0, &local, nullptr, buffer, size, nullptr);
                });
            case DateStyle::LongDate:
                return call([name, &local, longPattern](wchar_t* buffer, int size) {
                    const int written = ::GetDateFormatEx(name, 0, &local, longPattern, buffer, size, nullptr);
                    return written > 0 ? written
                                      : ::GetDateFormatEx(name, 0, &local, nullptr, buffer, size, nullptr);
                });
            case DateStyle::Time:
                return call([name, &local](wchar_t* buffer, int size) {
                    return ::GetTimeFormatEx(name, 0, &local, nullptr, buffer, size);
                });
            case DateStyle::DateTime: {
                const std::wstring date = formatDate(unixSecondsUtc, DateStyle::ShortDate);
                const std::wstring time = formatDate(unixSecondsUtc, DateStyle::Time);
                if (date.empty() || time.empty()) return std::wstring();
                return date + L" " + time;
            }
        }
        return std::wstring();
    } catch (...) {
        return std::wstring();
    }
}

std::wstring toWide(std::string_view utf8) noexcept {
    try {
        std::wstring result = convertToWide(utf8, MB_ERR_INVALID_CHARS);
        if (result.empty() && !utf8.empty()) {
            // Строка не прошла строгую проверку UTF-8. Пустая строка в
            // интерфейсе — невидимая поломка, поэтому подставляем U+FFFD.
            MRP_LOG_WARN("ui.locale.utf8", "некорректный UTF-8 в строке интерфейса, символы заменены");
            result = convertToWide(utf8, 0);
        }
        return result;
    } catch (...) {
        return std::wstring();
    }
}

std::string toUtf8(std::wstring_view utf16) noexcept {
    try {
        if (utf16.empty()) return std::string();
        const int sourceLength = static_cast<int>(utf16.size());
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), sourceLength, nullptr, 0, nullptr,
                                                 nullptr);
        if (needed <= 0) return std::string();
        std::string result(static_cast<std::size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), sourceLength, result.data(), needed, nullptr, nullptr);
        return result;
    } catch (...) {
        return std::string();
    }
}

std::uint16_t win32LanguageId(Language lang) noexcept {
    return langIdOf(lang);
}

// ---------------------------------------------------------------------------
// RTL-ready разметка
// ---------------------------------------------------------------------------

TextDirection textDirection() noexcept {
    return currentDirection();
}

bool isRtl() noexcept {
    return currentDirection() == TextDirection::RightToLeft;
}

bool systemLocaleIsRtl() noexcept {
    return querySystemDirection() == TextDirection::RightToLeft;
}

void forceTextDirection(int direction) noexcept {
    const int wanted = direction == 1 || direction == 2 ? direction : 0;
    const int previous = g_directionOverride.exchange(wanted, std::memory_order_acq_rel);
    if (previous != wanted) ++g_layoutRevision;
}

void resetTextDirection() noexcept {
    forceTextDirection(0);
}

void refreshSystemDirection() noexcept {
    cacheSystemDirection();
}

int leading(const Spacing& spacing, TextDirection direction) noexcept {
    return direction == TextDirection::RightToLeft ? spacing.end : spacing.start;
}

int trailing(const Spacing& spacing, TextDirection direction) noexcept {
    return direction == TextDirection::RightToLeft ? spacing.start : spacing.end;
}

TextAlign resolveAlign(TextAlign align, TextDirection direction) noexcept {
    if (direction == TextDirection::LeftToRight) return align;
    switch (align) {
        case TextAlign::Leading:
            return TextAlign::Trailing;
        case TextAlign::Trailing:
            return TextAlign::Leading;
        case TextAlign::Center:
        default:
            return TextAlign::Center;
    }
}

Rect mirrorRect(const Rect& rect, TextDirection direction) noexcept {
    if (direction == TextDirection::LeftToRight) return rect;
    Rect mirrored = rect;
    mirrored.left = rect.right;
    mirrored.right = rect.left;
    return mirrored;
}

std::uint32_t readingOrderStyles(std::uint32_t baseStyles) noexcept {
    if (currentDirection() == TextDirection::LeftToRight) return baseStyles;
    return baseStyles | kWsExRtlReading | kWsExLeftScrollbar;
}

std::vector<std::size_t> columnOrder(std::size_t columns, TextDirection direction) noexcept {
    return guard<std::vector<std::size_t>>(
        [columns, direction] {
            std::vector<std::size_t> order;
            order.reserve(columns);
            if (direction == TextDirection::LeftToRight) {
                for (std::size_t index = 0; index < columns; ++index) order.push_back(index);
            } else {
                for (std::size_t index = columns; index > 0; --index) order.push_back(index - 1);
            }
            return order;
        },
        std::vector<std::size_t>{});
}

bool mirrorVisuals(TextDirection direction) noexcept {
    return direction == TextDirection::RightToLeft;
}

}  // namespace mrproper::ui
