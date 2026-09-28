// Локализация слоя UI: загрузка строк, переключение ru/en без перезапуска,
// RTL-ready разметка, локальные числа и даты. Спека: §5 «Локализация | ru + en,
// RTL-ready, переводы в ресурсах, даты/числа через GetLocaleInfoEx»,
// §7.1 (пять экранов), §6.2 (ui::* — «окна, вкладки, рендер, темы,
// локализация, навигация»), §12 («русская и английская локализации полны»).
//
// Граница слоёв (SPEC §6.1, ADR-004). Всё, что не знает про Windows, уже сделано
// в core::i18n: там каталог строк, подстановка параметров, формы множественного
// числа и правило fallback'а «запрошенный язык → русский → сам ключ». Здесь
// только то, что ядру запрещено знать:
//
//   * чтение строк из ресурсов модуля (RT_STRING) — «переводы в ресурсах» из
//     §5; интерфейс собирается диалогами не из .rc, а в коде, но переводы
//     обязаны лежать в ресурсах, иначе их не переведёт ни редактор ресурсов,
//     ни человек без сборки;
//   * LANGID и SetThreadUILanguage: язык интерфейса потока Win32 меняет
//     подписи системных контролов, меню и подсказок — без него приложение на
//     русском внутри русской Windows всё равно останется с английскими
//     системными строками;
//   * разделители чисел и форматы дат из GetLocaleInfoEx — «1 234,5», а не
//     «1234.5», и дата по образцу локали, а не по ISO;
//   * зеркальная разметка (RTL-ready): направление письма, логические края,
//     порядок колонок, стили WS_EX_*. Русский и английский — оба LTR, поэтому
//     ветка зеркалирования проверяется принудительным направлением
//     (forceTextDirection), а не будущим RTL-языком: код, который нельзя
//     проверить, пишется хуже.
//
// Чего модуль НЕ делает намеренно. Он не владеет дескрипторами, не создаёт
// окон и не знает про DPI: это задачи app_shell, renderer и theme. Заголовок
// поэтому не включает windows.h — значения Win32 (стили окна, LANGID) выходят
// целыми числами, а тип FILETIME заменён на unix-секунды, как в core::model
// (§6.3). Так locale.hpp можно включить в код, который собирается без
// windows.h, и нельзя случайно затащить Win32 в переносимое ядро.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/i18n.hpp"

namespace mrproper::ui {

using core::Language;
using core::NumberFormat;
using core::TextDirection;

// ---------------------------------------------------------------------------
// Строки интерфейса
// ---------------------------------------------------------------------------

// Идентификатор строки. Тип, а не строковый литерал, потому что опечатка в
// литерале даёт пустую строку в интерфейсе (тот же ключ, но с чужим
// начертанием), а опечатка в имени — ошибку компиляции. Ключ при этом остаётся
// строкой: он нужен ресурсам, отчёту о непереведённом и поиску по исходникам.
enum class StringId : std::uint16_t {
    // Приложение
    kAppName,
    kAppTagline,
    // Страницы навигации (SPEC §7.1)
    kNavOverview,
    kNavDisks,
    kNavCleanup,
    kNavReport,
    kNavSettings,
    // Действия
    kActionScan,
    kActionRescan,
    kActionClean,
    kActionDryRun,
    kActionCancel,
    kActionClose,
    kActionOk,
    kActionYes,
    kActionNo,
    kActionRetry,
    kActionRefresh,
    kActionExport,
    kActionOpenInExplorer,
    kActionSelectAll,
    kActionClearSelection,
    kActionShowAll,
    kActionUndo,
    kActionCopy,
    kActionDetails,
    kActionCheck,
    // Состояние операции
    kStatusScanning,
    kStatusCleaning,
    kStatusProgress,
    kStatusElapsed,
    kStatusCancelRequested,
    kStatusIdle,
    // Уровень риска (SPEC §4 FR-4)
    kSafetySafe,
    kSafetyReview,
    kSafetyRisky,
    kSafetySafeHint,
    kSafetyReviewHint,
    kSafetyRiskyHint,
    // Экран «Очистка» (SPEC §4 FR-3, FR-5, §7.2)
    kCleanupSummary,
    kCleanupSelected,
    kCleanupSafeOnly,
    kCleanupDryRunNotice,
    kCleanupLockedBy,
    kCleanupLockedCount,
    kCleanupUndoAvailable,
    kCleanupTrashOverflow,
    kCleanupWhyJunk,
    kCleanupConfirmTitle,
    kCleanupFiles,
    kCleanupCandidates,
    kCleanupOlder,
    kCleanupNoCandidates,
    kCleanupTrashLabel,
    kCleanupDeleteLabel,
    // Категории мусора (SPEC §4 FR-3). Запасное название: заголовок из набора
    // правил (FR-4, у правила есть ru и en) первее, но правило может быть
    // без заголовка, и тогда дерево категорий обязано остаться читаемым.
    kCategoryTempUser,
    kCategoryTempSystem,
    kCategoryRecycleBin,
    kCategoryBrowserCache,
    kCategoryBrowserHistory,
    kCategoryFirefoxCache,
    kCategoryMsUpdate,
    kCategoryDeliveryOptimization,
    kCategoryWinsxs,
    kCategoryPrefetch,
    kCategoryCrashDumps,
    kCategoryWer,
    kCategoryLogs,
    kCategoryIconFontCache,
    kCategoryShaderCache,
    kCategoryInstallerCache,
    kCategoryMemoryDumps,
    kCategoryThumbnails,
    kCategoryNpmPipCache,
    kCategoryUserBigFiles,
    kCategoryDevCaches,
    kCategoryWslVhdx,
    // Единицы объёма
    kUnitByte,
    kUnitKilobyte,
    kUnitMegabyte,
    kUnitGigabyte,
    kUnitTerabyte,
    kUnitPetabyte,
    // Возраст файла
    kAgeNow,
    kAgeSeconds,
    kAgeMinutes,
    kAgeHours,
    kAgeDays,
    kAgeMonths,
    kAgeYears,
    // Экран «Диски» (SPEC §4 FR-2)
    kDisksTitle,
    kDisksModel,
    kDisksSerial,
    kDisksFirmware,
    kDisksBus,
    kDisksTrim,
    kDisksEncryption,
    kDisksFileSystem,
    kDisksLabel,
    kDisksMountPoint,
    kDisksFree,
    kDisksUsed,
    kDisksPartitionType,
    kDisksExportMap,
    kDisksFilterLetters,
    kDisksFilterSystem,
    kDisksFilterRemovable,
    kDisksNotDetected,
    kDisksPartitionCount,
    kDisksVolumeCount,
    // Экран «Отчёт» (SPEC §4 FR-8)
    kReportTitle,
    kReportSaved,
    kReportKept,
    kReportExportHtml,
    kReportExportJson,
    kReportExportText,
    kReportJournal,
    // Экран «Настройки» (SPEC §4 FR-9, §9.2)
    kSettingsLanguage,
    kSettingsRulesetVersion,
    kSettingsRulesetChecked,
    kSettingsCheckNow,
    kSettingsRestoreRules,
    kSettingsAutoUpdate,
    kSettingsAbout,
    kSettingsSafety,
    kSettingsLanguageChanged,
    // Общее
    kCommonNone,
    kCommonUnknown,
    kCommonError,
    kCommonWarning,
    kCommonAllocatedHint,
    kCommonReclaimHint,
    kCommonLocked,
    kCommonAccessDenied,
    kCommonNotFound,
    kCommonOperationFailed,
    kCommonWorking,
    kCommonLastScan,

    kCount,  // граница перечисления и длина таблицы ниже
};

inline constexpr std::size_t kStringCount = static_cast<std::size_t>(StringId::kCount);

// Одна строка в двух языках. Формы множественного числа хранятся в одном поле
// через '|' — так перевод помещается в одну строку ресурса и остаётся
// читаемым (core::i18n::selectPluralForm). «{0}», «{1}» — параметры по
// позиции, «{имя}» — по имени.
struct Translation {
    std::string_view key;
    std::string_view ru;
    std::string_view en;
};

// Встроенный набор строк. Он не «запасной вариант на случай поломки», а
// источник по умолчанию: файл .rc с переводами появится вместе с задачей
// локализации Win32-каркаса, а интерфейс должен быть на русском уже сейчас и
// на любой машине без единого файла ресурсов. Строки из ресурсов читаются
// первыми и не затираются: ключ, который уже переведён, повторно не пишется
// (core::StringCatalog::add), поэтому плохой .rc не испортит рабочий набор, а
// отсутствующий перевод в .rc не испортит этот.
//
// Порядок элементов таблицы значения не имеет: строки ищутся по ключу, а
// StringId — только ручка в неё. Перестановка строк в таблице или в
// перечислении не меняет ни одного результата (см. keyOf).
inline constexpr std::array<Translation, kStringCount> kTranslations{{
    {"app.name", "MrProper", "MrProper"},
    {"app.tagline", "Очистка диска с объяснением каждого удаления",
     "Disk cleanup that explains every deletion"},

    {"nav.overview", "Обзор", "Overview"},
    {"nav.disks", "Диски", "Disks"},
    {"nav.cleanup", "Очистка", "Cleanup"},
    {"nav.report", "Отчёт", "Report"},
    {"nav.settings", "Настройки", "Settings"},

    {"action.scan", "Сканировать", "Scan"},
    {"action.rescan", "Сканировать заново", "Scan again"},
    {"action.clean", "Очистить сейчас", "Clean now"},
    {"action.dryRun", "Пробный запуск", "Dry run"},
    {"action.cancel", "Отмена", "Cancel"},
    {"action.close", "Закрыть", "Close"},
    {"action.ok", "ОК", "OK"},
    {"action.yes", "Да", "Yes"},
    {"action.no", "Нет", "No"},
    {"action.retry", "Повторить", "Retry"},
    {"action.refresh", "Обновить", "Refresh"},
    {"action.export", "Экспорт", "Export"},
    {"action.openInExplorer", "Открыть в проводнике", "Open in Explorer"},
    {"action.selectAll", "Выбрать всё", "Select all"},
    {"action.clearSelection", "Снять выбор", "Clear selection"},
    {"action.showAll", "Показать все", "Show all"},
    {"action.undo", "Отменить очистку", "Undo cleanup"},
    {"action.copy", "Копировать", "Copy"},
    {"action.details", "Подробнее", "Details"},
    {"action.check", "Проверить", "Check"},

    {"status.scanning", "Сканирование…", "Scanning…"},
    {"status.cleaning", "Очистка…", "Cleaning…"},
    {"status.progress", "{0} из {1}", "{0} of {1}"},
    {"status.elapsed", "Прошло: {0}", "Elapsed: {0}"},
    {"status.cancelRequested", "Отмена запрошена, останавливаемся…",
     "Cancellation requested, stopping…"},
    {"status.idle", "Готово", "Done"},

    {"safety.safe", "Безопасно", "Safe"},
    {"safety.review", "Требует подтверждения", "Review"},
    {"safety.risky", "Рискованно", "Risky"},
    {"safety.safeHint", "Удаляется без подтверждения", "Removed without confirmation"},
    {"safety.reviewHint", "Удаляется только с подтверждения", "Removed only after confirmation"},
    {"safety.riskyHint", "Скрыт по умолчанию, нужно два подтверждения",
     "Hidden by default; needs two confirmations"},

    {"cleanup.summary", "Будет освобождено: {0}", "Will free: {0}"},
    {"cleanup.selected", "Выбрано: {0}", "Selected: {0}"},
    {"cleanup.safeOnly", "Только безопасное ({0})", "Safe only ({0})"},
    {"cleanup.dryRunNotice", "Пробный запуск: файлы не удаляются", "Dry run: no files will be deleted"},
    {"cleanup.lockedBy", "Занято процессами: {0}", "Locked by: {0}"},
    {"cleanup.lockedCount", "Пропущено занятых: {0}", "Skipped because in use: {0}"},
    {"cleanup.undoAvailable", "Последнюю очистку можно отменить (Ctrl+Z)",
     "The last cleanup can be undone (Ctrl+Z)"},
    {"cleanup.trashOverflow",
     "Корзина переполнена: часть файлов будет удалена безвозвратно. Продолжить?",
     "The trash is full: some files will be deleted permanently. Continue?"},
    {"cleanup.whyJunk", "Почему это мусор", "Why this is junk"},
    {"cleanup.confirmTitle", "Подтвердите очистку", "Confirm cleanup"},
    {"cleanup.files", "{0} файл|{0} файла|{0} файлов", "{0} file|{0} files"},
    {"cleanup.candidates", "{0} кандидат|{0} кандидата|{0} кандидатов",
     "{0} candidate|{0} candidates"},
    {"cleanup.older", "Старше {0}", "Older than {0}"},
    {"cleanup.noCandidates", "Мусор не найдено", "No junk found"},
    {"cleanup.trashLabel", "В корзину приложения", "To the app trash"},
    {"cleanup.deleteLabel", "Удалить безвозвратно", "Delete permanently"},

    {"cleanup.category.temp.user", "Временные файлы пользователя", "User temporary files"},
    {"cleanup.category.temp.system", "Временные файлы системы", "System temporary files"},
    {"cleanup.category.recycle.bin", "Корзина", "Recycle Bin"},
    {"cleanup.category.browser.cache", "Кэш браузеров", "Browser caches"},
    {"cleanup.category.browser.history", "История браузеров", "Browser history"},
    {"cleanup.category.firefox.cache", "Кэш Firefox и Thunderbird", "Firefox and Thunderbird caches"},
    {"cleanup.category.ms.update", "Файлы обновления Windows", "Windows Update files"},
    {"cleanup.category.delivery.opt", "Оптимизация доставки", "Delivery Optimization"},
    {"cleanup.category.winsxs.report", "Хранилище компонентов WinSxS", "WinSxS component store"},
    {"cleanup.category.prefetch", "Prefetch", "Prefetch"},
    {"cleanup.category.crash.dumps", "Дампы сбоев", "Crash dumps"},
    {"cleanup.category.wer", "Отчёты об ошибках Windows", "Windows error reports"},
    {"cleanup.category.logs", "Журналы", "Logs"},
    {"cleanup.category.icon.font.cache", "Кэш значков и шрифтов", "Icon and font caches"},
    {"cleanup.category.shadercache", "Кэш шейдеров", "Shader caches"},
    {"cleanup.category.installer.cache", "Кэш установщиков (только оценка)",
     "Installer cache (estimate only)"},
    {"cleanup.category.memory.dumps", "Файлы *.dmp", "*.dmp files"},
    {"cleanup.category.thumbnails", "Миниатюры", "Thumbnails"},
    {"cleanup.category.npm.pip.cache", "Кэши npm и pip", "npm and pip caches"},
    {"cleanup.category.user.bigfiles", "Крупные файлы пользователя", "Large user files"},
    {"cleanup.category.dev.caches", "Кэши инструментов разработки", "Developer tool caches"},
    {"cleanup.category.wsl.vhdx.report", "Диски WSL (только оценка)", "WSL disks (estimate only)"},

    {"units.byte", "Б", "B"},
    {"units.kilobyte", "КБ", "KB"},
    {"units.megabyte", "МБ", "MB"},
    {"units.gigabyte", "ГБ", "GB"},
    {"units.terabyte", "ТБ", "TB"},
    {"units.petabyte", "ПБ", "PB"},

    {"units.age.now", "только что", "just now"},
    {"units.age.seconds", "{0} с", "{0} s"},
    {"units.age.minutes", "{0} мин", "{0} min"},
    {"units.age.hours", "{0} ч|{0} ч|{0} ч", "{0} h"},
    {"units.age.days", "{0} день|{0} дня|{0} дней", "{0} day|{0} days"},
    {"units.age.months", "{0} мес|{0} мес|{0} мес", "{0} mo"},
    {"units.age.years", "{0} г|{0} г|{0} лет", "{0} y"},

    {"disks.title", "Диски", "Disks"},
    {"disks.model", "Модель", "Model"},
    {"disks.serial", "Серийный номер", "Serial number"},
    {"disks.firmware", "Прошивка", "Firmware"},
    {"disks.bus", "Шина", "Bus"},
    {"disks.trim", "TRIM", "TRIM"},
    {"disks.encryption", "Шифрование", "Encryption"},
    {"disks.fileSystem", "Файловая система", "File system"},
    {"disks.label", "Метка", "Label"},
    {"disks.mountPoint", "Точка монтирования", "Mount point"},
    {"disks.free", "Свободно", "Free"},
    {"disks.used", "Занято", "Used"},
    {"disks.partitionType", "Тип раздела", "Partition type"},
    {"disks.exportMap", "Экспорт карты", "Export map"},
    {"disks.filterLetters", "Только тома с буквами", "Only lettered volumes"},
    {"disks.filterSystem", "Только системные", "System only"},
    {"disks.filterRemovable", "Съёмные", "Removable"},
    {"disks.notDetected", "Диск недоступен", "Disk unavailable"},
    {"disks.partitionCount", "{0} раздел|{0} раздела|{0} разделов", "{0} partition|{0} partitions"},
    {"disks.volumeCount", "{0} том|{0} тома|{0} томов", "{0} volume|{0} volumes"},

    {"report.title", "Отчёт", "Report"},
    {"report.saved", "Отчёт сохранён: {0}", "Report saved: {0}"},
    {"report.kept", "Хранятся последние отчёты: {0}", "Keeping the last reports: {0}"},
    {"report.exportHtml", "Экспорт в HTML", "Export as HTML"},
    {"report.exportJson", "Экспорт в JSON", "Export as JSON"},
    {"report.exportText", "Текстовый дамп", "Text dump"},
    {"report.journal", "Журнал операций", "Operation log"},

    {"settings.language", "Язык интерфейса", "Interface language"},
    {"settings.rulesetVersion", "Версия набора правил", "Rule set version"},
    {"settings.rulesetChecked", "Последняя проверка: {0}", "Last checked: {0}"},
    {"settings.checkNow", "Проверить сейчас", "Check now"},
    {"settings.restoreRules", "Вернуть встроенный набор", "Restore built-in rule set"},
    {"settings.autoUpdate", "Обновлять правила автоматически", "Update rules automatically"},
    {"settings.about", "О программе", "About"},
    {"settings.safety", "Безопасность", "Safety"},
    {"settings.languageChanged", "Язык интерфейса: {0}", "Interface language: {0}"},

    {"common.none", "Нет", "None"},
    {"common.unknown", "Неизвестно", "Unknown"},
    {"common.error", "Ошибка", "Error"},
    {"common.warning", "Предупреждение", "Warning"},
    {"common.allocatedHint", "Освобождаемое место считается по аллоцированному размеру",
     "Reclaimable space is measured by allocated size"},
    {"common.reclaimHint", "По аллоцированному размеру", "By allocated size"},
    {"common.locked", "Занят приложением", "In use by an app"},
    {"common.accessDenied", "Нет доступа", "Access denied"},
    {"common.notFound", "Не найдено", "Not found"},
    {"common.operationFailed", "Не удалось выполнить операцию: {0}", "Operation failed: {0}"},
    {"common.working", "{0}…", "{0}…"},
    {"common.lastScan", "Последнее сканирование: {0}", "Last scan: {0}"},
}};

// Ключ строки по её идентификатору. Порядок таблицы и перечисления может
// разойтись — поиск идёт по ключу, поэтому такая рассинхронизация не меняет
// результат, а стоит только времени на один индекс.
constexpr std::string_view keyOf(StringId id) noexcept {
    return kTranslations[static_cast<std::size_t>(id)].key;
}

// Проверка целостности встроенного набора. Это измеритель пункта «локализации
// полны» из SPEC §12: не «вроде перевели», а сколько ключей пустых, без
// перевода или с дубликатом. Вызывается при загрузке (результат уходит в лог)
// и доступна приёмке.
struct SelfCheck {
    std::size_t keys{};             // строк в таблице
    std::size_t emptyRu{};          // пустой русский перевод
    std::size_t emptyEn{};          // пустой английский перевод
    std::size_t untranslatedRu{};   // есть en, нет ru
    std::size_t untranslatedEn{};   // есть ru, нет en
    std::size_t duplicateKeys{};    // ключ встретился дважды

    [[nodiscard]] bool complete() const noexcept {
        return emptyRu == 0 && emptyEn == 0 && duplicateKeys == 0;
    }
};

SelfCheck selfCheck() noexcept;

// ---------------------------------------------------------------------------
// Загрузка и переключение
// ---------------------------------------------------------------------------

// Что удалось загрузить. Отчёт нужен не для красоты: пустой ресурсный набор и
// неполный перевод — это разные поломки с разными последствиями, и обе должны
// быть видны в логе и на экране «О программе».
struct LocaleReport {
    bool ok{true};                    // false — загрузка сорвалась (память)
    bool resourcesFound{};            // в модуле есть строки RT_STRING
    std::size_t resourceStrings{};    // строк взято из ресурсов (оба языка)
    std::size_t builtInStrings{};     // строк встроенного набора
    std::size_t translatedRu{};       // ключей с русским переводом
    std::size_t translatedEn{};       // ключей с английским переводом
    std::vector<std::string> missingRu;  // ключи без русского перевода
    std::vector<std::string> missingEn;  // ключи без английского перевода
    Language language{};                 // язык, применённый при загрузке
    std::uint16_t win32Language{};       // LANGID, который реально установился
    int lastError{};                     // код последней неудачи Win32, 0 — нет
};

// Загрузка строк и применение языка. Вызывается один раз при старте, до
// создания окон и из UI-потока: язык интерфейса потока и разделители чисел
// должны быть готовы до первого отрисованного контрола. Идемпотентна — повторный вызов
// перечитывает всё заново и ничего не дублирует. Ресурсы берутся из образа
// процесса (GetModuleHandleW(nullptr)), отдельный hModule не нужен: у EXE он
// один, а если появится DLL с переводами, добавить параметр проще, чем
// разбираться, почему строки из неё не видны.
LocaleReport initialize() noexcept;

// То же, что initialize(), но язык остаётся текущим (а не выбирается заново
// по локали ОС). Вызывается после «Вернуть встроенный набор» и при смене
// ресурсов.
LocaleReport reloadStrings() noexcept;

[[nodiscard]] bool isInitialized() noexcept;

// Язык по локали ОС: русская Windows → русский интерфейс, английская →
// английский, всё остальное → русский (приложение двуязычное, а не «как
// угодно»). Пользовательская настройка перекрывает это при первом переключении
// языка, и переживает перезапуск — её хранит settings.
[[nodiscard]] Language startupLanguage() noexcept;

[[nodiscard]] Language currentLanguage() noexcept;

// Переключение языка без перезапуска (SPEC §5, §8 Этап 0 «локализация (ru)»).
// Три вещи делаются разом, потому что по отдельности они расходятся:
//   * core::Localizer — tr() сразу отдаёт новый язык, revision() растёт;
//   * SetThreadUILanguage — системные строки Win32 (меню, подсказки, «ОК»
//     в диалогах ОС) переключаются в том же окне;
//   * разделители чисел и образец даты — из локали нового языка.
// Вызывать из UI-потока: SetThreadUILanguage действует на вызывающий поток.
// Возвращает применённый язык интерфейса. Если система не знает LANGID
// языка, SetThreadUILanguage вернёт 0 и подписи системных контролов останутся
// прежними — это пишется в лог, а интерфейс приложения всё равно переключится.
Language setLanguage(Language lang) noexcept;
Language toggleLanguage() noexcept;  // ru ↔ en

// Растёт при смене языка, при загрузке строк и при смене разделителей чисел.
// Экраны запоминают значение и перерисовываются, когда оно разошлось с их
// последним (SPEC §6.4 — один UI-поток читает атомики).
[[nodiscard]] std::uint64_t revision() noexcept;

// Отдельный счётчик раскладки: он растёт при смене направления письма. Текст
// после смены языка достаточно перерисовать, а раскладку (порядок колонок,
// края, стили WS_EX_*) надо пересчитать — иначе список останется с прежними
// колонками, пока текст в них уже на другом языке.
[[nodiscard]] std::uint64_t layoutRevision() noexcept;

// Ключи, для которых в текущем языке нет перевода (приёмка §12).
[[nodiscard]] std::vector<std::string> missingTranslations() noexcept;

// ---------------------------------------------------------------------------
// Строки
// ---------------------------------------------------------------------------

// Отсутствующий ключ отдаётся как есть: он виден на экране и находится поиском
// по исходникам. Молчаливая пустая строка в интерфейсе читается как «всё в
// порядке» — это худший вид поломки в утилите, которая обещает объяснять
// каждое своё действие.
[[nodiscard]] std::string tr(StringId id) noexcept;
[[nodiscard]] std::string tr(std::string_view key) noexcept;
[[nodiscard]] std::string tr(StringId id, std::string_view arg0) noexcept;
[[nodiscard]] std::string tr(StringId id, const core::StringArgs& args) noexcept;
[[nodiscard]] std::string tr(StringId id, const core::NamedArgs& args) noexcept;

// Готовая строка для Win32 (UTF-16). Слой UI рисует через DirectWrite и
// нативные контролы, то есть нужен UTF-16; перекодировка живёт здесь, чтобы у
// каждого экрана не было своей копии этого кода.
[[nodiscard]] std::wstring trWide(StringId id) noexcept;
[[nodiscard]] std::wstring trWide(std::string_view key) noexcept;
[[nodiscard]] std::wstring trPluralWide(StringId id, std::uint64_t count) noexcept;

// Форма множественного числа выбирается по текущему языку: «1 файл», «2 файла»,
// «5 файлов» и «1 file», «5 files».
[[nodiscard]] std::string trPlural(StringId id, std::uint64_t count) noexcept;
[[nodiscard]] std::string trPlural(StringId id, std::uint64_t count,
                                   const core::StringArgs& extra) noexcept;

// Ключ вида «cleanup.category.temp.user» по идентификатору категории
// правила. Категория в правиле — строка (FR-4), а не перечисление, поэтому
// соответствие «идентификатор → ключ» живёт здесь; неизвестная категория
// возвращается как есть, и дерево показывает её идентификатор, а не пустоту.
[[nodiscard]] std::string categoryKey(std::string_view categoryId) noexcept;

// Название категории с учётом заголовка из правила: заголовок правила
// (ru/en в JSON, FR-4) первее встроенного ключа, дальше — сам идентификатор.
[[nodiscard]] std::string categoryName(std::string_view ruleId, std::string_view ruleCategory,
                                       std::string_view ruleTitle) noexcept;

// ---------------------------------------------------------------------------
// Числа, объёмы и даты
// ---------------------------------------------------------------------------

// Разделители из GetLocaleInfoEx для локали текущего языка приложения
// (ru-RU → «1 234,5», en-US → «1,234.5»). Числа идут по языку интерфейса, а не
// по локали системы: пользователь, переключивший интерфейс на русский на
// английской Windows, ожидает русские разделители в русском интерфейсе.
[[nodiscard]] NumberFormat numberFormat() noexcept;

// Имя локали, из которой берутся числа и даты: «ru-RU» / «en-US».
[[nodiscard]] std::wstring localeName() noexcept;

// «1,2 ГБ» / «1.2 GB». Единицы — строки из таблицы (units.*), число — в локали.
// Единицы десятичные (КБ/МБ/ГБ), как в Проводнике: расхождение с тем, что
// показывает сама Windows, хуже, чем отсутствие «КиБ», а в SPEC §4 FR-4
// считается аллоцированный размер, который Проводник тоже показывает в
// десятичных единицах.
[[nodiscard]] std::string formatBytes(std::uint64_t bytes, int decimals = 1) noexcept;

// «1 234 файла» / «1,234 files» — форма множественного числа из строки.
[[nodiscard]] std::string formatCount(std::uint64_t count) noexcept;

// «12,3 %»
[[nodiscard]] std::string formatPercent(double fraction, int decimals = 1) noexcept;

// «3 дн.» / «2 ч» / «только что». Русский вариант есть в core::units, но он
// зашит в ядро и потому не переводится (ADR-004: ядро не знает про язык
// выбранный пользователем); здесь та же величина собирается из строк.
[[nodiscard]] std::string formatAge(std::int64_t seconds) noexcept;

enum class DateStyle : std::uint8_t {
    ShortDate,  // «28.09.2026»
    LongDate,   // «28 сентября 2026 г.»
    Time,       // «14:03:21»
    DateTime,   // «28.09.2026 14:03»
};

// Дата и время по образцу локали языка интерфейса (GetDateFormatEx /
// GetTimeFormatEx), из unix-секунд UTC — так же, как модель хранит
// oldestWrite/newestWrite (core::model, §6.3). Время переводится в местное
// зону машины, иначе «последнее сканирование» показывало бы время UTC и
// пугало бы пользователя, у которого оно на семь часов меньше.
[[nodiscard]] std::wstring formatDate(std::int64_t unixSecondsUtc, DateStyle style) noexcept;

// Перекодировки строк. Строки в ядре — UTF-8 (как пути в модели), Win32 и
// DirectWrite ждут UTF-16. Битый UTF-8 не даёт пустую строку (в интерфейсе
// это невидимая поломка): вместо отказа ставится U+FFFD, в лог уходит
// предупреждение с размером.
[[nodiscard]] std::wstring toWide(std::string_view utf8) noexcept;
[[nodiscard]] std::string toUtf8(std::wstring_view utf16) noexcept;

// LANGID для SetThreadUILanguage и для проверки, что переключение прошло.
[[nodiscard]] std::uint16_t win32LanguageId(Language lang) noexcept;

// ---------------------------------------------------------------------------
// RTL-ready разметка
// ---------------------------------------------------------------------------

// Направление письма НЕ следует за языком приложения. Русский и английский —
// оба LTR, но Windows зеркалит интерфейс по локали пользователя, и русский
// интерфейс на арабской Windows обязан быть зеркальным: иначе полосы прокрутки,
// порядок колонок и подсказки разойдутся с системными диалогами, которые
// Windows рисует зеркально. Поэтому направление берётся у локали ОС, а язык
// интерфейса на него не влияет.
[[nodiscard]] TextDirection textDirection() noexcept;
[[nodiscard]] bool isRtl() noexcept;

// Направление по локали ОС без учёта принудительного (см. forceTextDirection).
[[nodiscard]] bool systemLocaleIsRtl() noexcept;

// Принудительное направление. Нужно, чтобы ветка зеркалирования проверялась
// сегодня, а не когда-нибудь: в приложении нет RTL-языка, а код зеркализации,
// который нельзя выполнить, через год окажется нерабочим. Ноль — «снова по
// локали ОС».
void forceTextDirection(int direction) noexcept;  // 0 — по локали, 1 — LTR, 2 — RTL
void resetTextDirection() noexcept;

// Перечитать локаль ОС. Вызывается при WM_SETTINGCHANGE: пользователь может
// сменить язык интерфейса Windows, не перезапуская приложение (SPEC §5 — про
// переключение без перезапуска).
void refreshSystemDirection() noexcept;

// Отступы в логических краях. Физические left/right задавать нельзя: при
// зеркалировании они меняются местами, и любое место, где про них забыли,
// рисует поля в другую сторону.
struct Spacing {
    int start{};  // ведущий край: слева в LTR, справа в RTL
    int end{};    // завершающий край
    int top{};
    int bottom{};
};

[[nodiscard]] int leading(const Spacing& spacing, TextDirection direction) noexcept;
[[nodiscard]] int trailing(const Spacing& spacing, TextDirection direction) noexcept;

// Выравнивание текста — тоже логическое: «ведущий» край строки в RTL справа.
enum class TextAlign : std::uint8_t { Leading, Center, Trailing };

[[nodiscard]] TextAlign resolveAlign(TextAlign align, TextDirection direction) noexcept;

// Прямоугольник в логических краях; зеркальный меняет left и right местами.
// Им пользуется рендер (карта разделов, полосы прогресса, шкалы) и рисует
// результат, а не решает на лету, с какой стороны что-то начинается.
struct Rect {
    long left{};
    long top{};
    long right{};
    long bottom{};

    [[nodiscard]] long width() const noexcept { return right - left; }
    [[nodiscard]] long height() const noexcept { return bottom - top; }
};

[[nodiscard]] Rect mirrorRect(const Rect& rect, TextDirection direction) noexcept;

// Стили окна и контрола для чтения в текущем направлении: в RTL добавляются
// WS_EX_RTLREADING (порядок чтения и выравнивание текста) и
// WS_EX_LEFTSCROLLBAR (полоса прокрутки слева). Значения совпадают с Win32,
// поэтому подпись и есть источник истины; тип std::uint32_t — чтобы заголовок
// оставался без windows.h.
[[nodiscard]] std::uint32_t readingOrderStyles(std::uint32_t baseStyles = 0) noexcept;

// Порядок вывода колонок: в RTL столбцы идут справа налево. Возвращает
// перестановку «логический индекс → физический», поэтому список и дерево
// (SysListView32, SysTreeView32) печатаются в правильном порядке без
// пересоздания колонок при смене направления.
[[nodiscard]] std::vector<std::size_t> columnOrder(std::size_t columns, TextDirection direction) noexcept;

// Зеркалить ли графику: стрелки, шевроны, полосы прогресса, иконки «вперёд».
// Текст и цифры не зеркалятся никогда — зеркальные буквы нечитаемы, а
// двунаправленный текст (путь с латиницей внутри кириллицы) Win32 и
// DirectWrite разбирают сами по правилам Unicode.
[[nodiscard]] bool mirrorVisuals(TextDirection direction) noexcept;

}  // namespace mrproper::ui
