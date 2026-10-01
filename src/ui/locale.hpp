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
    // Экран «Обзор» (SPEC §7.1). Раньше тринадцать пар ru/en лежали в
    // src/ui/view_cleanup.cpp мимо каталога (там же стоял и pick(ru, en)).
    // Теперь они в каталоге — так же, как слова узлов карты у экрана «Диски».
    kOverviewTitle,
    // Названия четырёх плиток: что это за число.
    kOverviewTileDisks,
    kOverviewTileFree,
    kOverviewTileCandidates,
    kOverviewTileReclaimable,
    // Подписи плиток: из чего посчитано число.
    kOverviewCaptionDisksReading,
    kOverviewCaptionFree,
    kOverviewCaptionCandidates,
    kOverviewCaptionReclaimable,
    // Значения, когда данных ещё нет. Ноль после скана и ноль без скана —
    // разные вещи (§4 FR-3), поэтому «нет данных» отдельной строкой.
    kOverviewValueReading,
    kOverviewValueNoScan,
    // Пояснения под плитками.
    kOverviewNoteScanDone,
    kOverviewNoteNeedScan,
    kOverviewNoteRules,
    kOverviewNoteRulesLoading,
    // Кнопка внизу экрана. Вторая кнопка — kActionScan, та же строка.
    kOverviewActionOpenDisks,
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
    // Слова узлов карты. Раньше эти девять слов были зашиты в src/ui/view_disks.cpp
    // как пара ru/en мимо каталога (там же стояла и приписка «ключи для владельца
    // каталога»). Теперь они в каталоге: строку из ресурсов можно перевести без
    // правки исходников, а проверка «сколько ключей используется и сколько
    // объявлено» видит их наравне с остальными.
    kDisksWordDisk,
    kDisksWordPartition,
    kDisksWordVolume,
    kDisksWordSystem,
    kDisksWordBootable,
    kDisksWordHidden,
    kDisksWordRemovable,
    kDisksWordReadOnly,
    kDisksWordUnallocated,
    // Три строки пустой карты, по одной на причину: фильтры скрыли всё, устройства
    // не прочитаны, обход ещё идёт. Раньше тоже были зашиты в экран.
    kDisksEmptyFiltersTitle,
    kDisksEmptyFiltersReason,
    kDisksEmptyFiltersHint,
    kDisksEmptyUnreadTitle,
    kDisksEmptyUnreadReason,
    kDisksEmptyUnreadAction,
    kDisksEmptyLoadingTitle,
    kDisksEmptyLoadingReason,
    kDisksEmptyLoadingAction,
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

    // Словарь экрана «Отчёт». Раньше 133 пары ru/en стояли прямо в коде
    // экрана через pick(ru, en) — их нельзя было переопределить ресурсом и
    // они не попадали в selfCheck(). Теперь это обычные ключи каталога.
    kWordColumnTime,
    kWordColumnKind,
    kWordColumnSource,
    kWordColumnSubject,
    kWordColumnStatus,
    kWordColumnSize,
    kWordOperation,
    kWordError,
    kWordEvent,
    kWordStatusDone,
    kWordStatusSkipped,
    kWordStatusPartial,
    kWordStatusFailed,
    kWordJournalEmpty,
    kWordNoReport,
    kWordHidden,
    kWordProblemsFound,
    kWordDetailTitle,
    kWordMaskSerials,
    kWordOnlyProblems,
    kWordEmptyWhy,
    kWordEmptyWhat,
    kWordUnknown,
    // Уровни журнала словами: core::logLevelName даёт машинное имя, таблице нужно
    // то, что читает человек.
    kLevelDebug,
    kLevelInfo,
    kLevelWarn,
    kLevelError,
    kLevelOff,
    // Имя действия плана. core::toString(PlanAction) в ядре объявлено, но не
    // определено, поэтому имя своё — и переводимое.
    kPlanDelete,
    kPlanTrash,
    kPlanKeep,
    kPlanSkipLocked,
    // Текстовый дамп отчёта (report.export.text, FR-8). Подписи разделов и полей
    // выгружаемого файла — тоже интерфейс: их читает человек, а не программа.
    kDumpReport,
    kDumpKind,
    kDumpGenerated,
    kDumpDuration,
    kDumpApp,
    kDumpPid,
    kDumpRuleset,
    kDumpOs,
    kDumpOsBuild,
    kDumpArch,
    kDumpSerials,
    kDumpSerialsMasked,
    kDumpSerialsPlain,
    kDumpNotes,
    kDumpTotals,
    kDumpDisks,
    kDumpPartitions,
    kDumpVolumes,
    kDumpCandidates,
    kDumpOperations,
    kDumpUntouched,
    kDumpFreed,
    kDumpNotFreed,
    kDumpSucceeded,
    kDumpPartial,
    kDumpFailed,
    kDumpSkipped,
    kDumpErrors,
    kDumpPartitionMap,
    kDumpCandidatesHeading,
    kDumpFiles,
    kDumpProcess,
    kDumpOperationsHeading,
    kDumpUntouchedHeading,
    kDumpErrorsHeading,
    kDumpProblemsHeading,
    kDumpAttempts,
    // Строка состояния и подпись сохранения. Пробел и разделитель « · » входят в
    // строку, поэтому склейка не зависит от языка и не может разъехаться.
    kStatusFreed,
    kStatusOperations,
    kStatusErrors,
    kStatusHidden,
    kStatusReports,
    kSavedPruned,
    // Сводка отчёта (строки «поле: значение»).
    kSummaryKind,
    kSummaryStarted,
    kSummaryFinished,
    kSummaryDuration,
    kSummaryApp,
    kSummaryPid,
    kSummaryRules,
    kSummaryOs,
    kSummaryOsVersion,
    kSummaryArch,
    kSummaryDisks,
    kSummaryPartitions,
    kSummaryVolumes,
    kSummaryCandidates,
    kSummaryLocked,
    kSummaryOperations,
    kSummaryUntouched,
    kSummaryFreed,
    kSummaryNotFreed,
    kSummarySucceeded,
    kSummaryFailed,
    kSummarySkipped,
    kSummaryErrors,
    kSummarySerials,
    // Подробности выбранной строки журнала.
    kDetailKind,
    kDetailEvent,
    kDetailLevel,
    kDetailMessage,
    kDetailScope,
    kDetailCode,
    kDetailOperation,
    kDetailPath,
    kDetailRepeats,
    kDetailCategory,
    kDetailName,
    kDetailStatus,
    kDetailAction,
    kDetailSafety,
    kDetailConfidence,
    kDetailSize,
    kDetailSizeExact,
    kDetailAttempts,
    kDetailStarted,
    kDetailFinished,
    kDetailDuration,
    // Ошибки сохранения. Раньше обе печатались по-русски в обоих языках.
    kSaveEmpty,
    kSaveNoAppData,
    // Пояснение вместо пустого дерева категорий на экране «Очистка».
    kCleanupHintNoScan,
    kCleanupHintScanning,
    kCleanupHintIdle,
    kCleanupHintRuleCount,
    kCleanupHintCategoryCount,
    kCleanupHintRuleSetTail,
    kCleanupHintPressRescan,
    // Экран «Настройки». 47 строк лежали в локальной таблице view_settings.cpp
    // мимо каталога: ключи были, а selfCheck() их не видел.
    kSettingsRules,
    kSettingsRuleColumn,
    kSettingsCategoryColumn,
    kSettingsSafetyColumn,
    kSettingsRulesSummary,
    kSettingsRuleEnabled,
    kSettingsRuleDisabled,
    kSettingsRuleIdLabel,
    kSettingsRulePathLabel,
    kSettingsRuleAgeLabel,
    kSettingsRuleCategoryLabel,
    kSettingsRuleSetLevelLabel,
    kSettingsRuleNotSelected,
    kSettingsDetailsTemplate,
    kSettingsRiskyConfirm,
    kSettingsFilterLabel,
    kSettingsNoRulesFound,
    kSettingsBuiltinSet,
    kSettingsNeverChecked,
    kSettingsChecking,
    kSettingsNoRuleSet,
    kSettingsImportRules,
    kSettingsExportRules,
    kSettingsExportStatistics,
    kSettingsSafetyLevelLabel,
    kSettingsRestoreDone,
    kSettingsOverridesForgotten,
    kSettingsAboutApp,
    kSettingsAboutPrivacy,
    kSettingsAboutAutostart,
    kSettingsStatisticsHeader,
    kSettingsStatSource,
    kSettingsStatVersion,
    kSettingsStatVerified,
    kSettingsStatInstalled,
    kSettingsStatChecked,
    kSettingsStatResult,
    kSettingsStatAutoUpdate,
    kSettingsStatLanguage,
    kSettingsStatRules,
    kSettingsStatEnabled,
    kSettingsStatChanged,
    kSettingsStatFilter,
    kSettingsStatSelected,
    kSettingsEmptyTitle,
    kSettingsEmptyWhy,
    kSettingsEmptyWhat,

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

    // Ключи рельса ровно те, что просит src/ui/nav.hpp (kPages[].titleKey) и
    // резолвит src/ui/app_shell.cpp через tr(key). Пока в каталоге были другие
    // имена, все пять подписей возвращались в рельс «пусто» и молча брались из
    // запасного набора kPages — то есть пять ключей числились в каталоге и не
    // использовались, а пять запрошенных ключей в нём отсутствовали.
    {"nav.page.overview", "Обзор", "Overview"},
    {"nav.page.disks", "Диски", "Disks"},
    {"nav.page.cleanup", "Очистка", "Cleanup"},
    {"nav.page.report", "Отчёт", "Report"},
    {"nav.page.settings", "Настройки", "Settings"},

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

    // Строки экрана «Обзор». Тексты перенесены байт в байт из
    // src/ui/view_cleanup.cpp: экран рисует их сам, и любая правка здесь
    // меняет число пикселей в снимке.
    {"overview.title", "Обзор", "Overview"},
    {"overview.tile.disks", "Диски", "Disks"},
    {"overview.tile.free", "Свободно", "Free"},
    {"overview.tile.candidates", "Кандидаты", "Candidates"},
    {"overview.tile.reclaimable", "Освободится", "Reclaimable"},
    {"overview.caption.disksReading", "дисков: читается в фоне", "disks: read in the background"},
    {"overview.caption.free", "свободно на всех томах", "free on all volumes"},
    {"overview.caption.candidates", "кандидатов найдено", "candidates found"},
    {"overview.caption.reclaimable", "можно освободить", "can be freed"},
    {"overview.value.reading", "читаем...", "reading..."},
    {"overview.value.noScan", "скана не было", "no scan"},
    {"overview.note.scanDone",
     "Скан выполнен. Выберите, что удалить, на странице «Очистка»: без вашего выбора ничего не удаляется.",
     "Scan is done. Choose what to delete on the Cleanup page — nothing is deleted without your selection."},
    {"overview.note.needScan",
     "Пока вы не просканируете, ничего не считается и ничего не удаляется: нажмите «Сканировать», и "
     "категории появятся с реальными размерами.",
     "Nothing is deleted and nothing is counted until you scan: press Scan and the categories appear with "
     "real sizes."},
    {"overview.note.rules", "Набор правил: {0} правил, версия {1}", "Rule set: {0} rules, version {1}"},
    {"overview.note.rulesLoading", "Набор правил ещё читается с диска (фоновый поток).",
     "The rule set is still being read from disk (background thread)."},
    {"overview.action.openDisks", "Открыть диски", "Open disks"},

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

    {"disks.word.disk", "Диск", "Disk"},
    {"disks.word.partition", "Раздел", "Partition"},
    {"disks.word.volume", "Том", "Volume"},
    {"disks.word.system", "системный", "system"},
    {"disks.word.bootable", "загрузочный", "bootable"},
    {"disks.word.hidden", "скрытый", "hidden"},
    {"disks.word.removable", "съёмный", "removable"},
    {"disks.word.readOnly", "только чтение", "read-only"},
    {"disks.word.unallocated", "неразмеченное место", "unallocated space"},

    {"disks.empty.filters.title", "Фильтры скрывают все диски", "Filters hide every disk"},
    {"disks.empty.filters.reason", "Снимите фильтры над картой — и карта вернётся",
     "Clear the filters above the map to see disks again"},
    {"disks.empty.filters.hint", "Подсказка: у диска с буквой есть том с буквой диска",
     "Hint: a disk with a letter has a volume with a drive letter"},
    {"disks.empty.unread.title", "Устройства не прочитаны", "Devices were not read"},
    {"disks.empty.unread.reason",
     "Карта разделов пуста: устройства \\\\.\\PhysicalDriveN открываются только с повышенными правами.",
     "The partition map is empty: \\\\.\\PhysicalDriveN opens with elevated rights only."},
    {"disks.empty.unread.action", "Запустите MrProper от имени администратора, затем нажмите «",
     "Run MrProper as administrator, then press "},
    {"disks.empty.loading.title", "Читаем диски", "Reading disks"},
    {"disks.empty.loading.reason",
     "Обход идёт в фоне: таймаут 2 с на устройство, обычно несколько секунд",
     "The walk runs in the background: 2 s per device, usually a few seconds"},
    {"disks.empty.loading.action",
     "Карта разделов появится здесь сама — нажимать ничего не нужно",
     "The partition map will appear here on its own, no button needed"},

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

    // Словарь экрана «Отчёт». Раньше 133 пары ru/en стояли прямо в коде
    {"report.word.time", "Время", "Time"},
    {"report.word.kind", "Вид", "Kind"},
    {"report.word.source", "Источник", "Source"},
    {"report.word.action", "Что сделано", "Action"},
    {"report.word.status", "Статус", "Status"},
    {"report.word.size", "Объём", "Size"},
    {"report.word.operation", "Операция", "Operation"},
    {"report.word.error", "Ошибка", "Error"},
    {"report.word.event", "Событие", "Event"},
    {"report.word.statusDone", "Выполнено", "Done"},
    {"report.word.statusSkipped", "Пропущено", "Skipped"},
    {"report.word.statusPartial", "Частично", "Partial"},
    {"report.word.statusFailed", "Не выполнено", "Failed"},
    {"report.word.journalEmpty", "Журнал пуст", "The log is empty"},
    {"report.word.noReport", "Отчёт ещё не получен", "No report yet"},
    {"report.word.hiddenByFilter", "Скрыто фильтром", "Hidden by filters"},
    {"report.word.problems", "Замечания к отчёту", "Report problems"},
    {"report.word.rowDetails", "Подробности строки", "Row details"},
    {"report.word.maskSerials", "Маскировать серийники", "Mask serials"},
    {"report.word.onlyProblems", "Только ошибки и предупреждения", "Errors and warnings only"},
    {"report.word.emptyWhy",
     "Операций ещё не было: журнал пишется при очистке и при каждой ошибке.",
     "No operations yet: the log is written during cleanup and on every error."},
    {"report.word.emptyWhat",
     "Запустите очистку на странице «Очистка» — строки появятся здесь сразу после каждой"
     " операции.",
     "Start a cleanup on the Cleanup page, rows appear here right after each operation."},
    {"report.word.unknown", "неизвестно", "unknown"},

    // Уровни журнала словами: core::logLevelName даёт машинное имя, таблице нужно
    {"report.level.debug", "Отладка", "Debug"},
    {"report.level.info", "Инфо", "Info"},
    {"report.level.warn", "Внимание", "Warning"},
    {"report.level.error", "Ошибка", "Error"},
    {"report.level.off", "Выключен", "Off"},

    // Имя действия плана. core::toString(PlanAction) в ядре объявлено, но не
    {"report.plan.delete", "удалить", "delete"},
    {"report.plan.trash", "в корзину", "trash"},
    {"report.plan.keep", "оставить", "keep"},
    {"report.plan.skipLocked", "пропуск (занято)", "skip (locked)"},

    // Текстовый дамп отчёта (report.export.text, FR-8). Подписи разделов и полей
    {"report.dump.report", "отчёт", "report"},
    {"report.dump.kind", "Вид", "Kind"},
    {"report.dump.generated", "Сформирован", "Generated"},
    {"report.dump.duration", "Длительность", "Duration"},
    {"report.dump.app", "Приложение", "App"},
    {"report.dump.pid", "Процесс", "PID"},
    {"report.dump.ruleset", "Набор правил", "Ruleset"},
    {"report.dump.os", "ОС", "OS"},
    {"report.dump.osBuild", "Сборка ОС", "OS build"},
    {"report.dump.arch", "Архитектура", "Arch"},
    {"report.dump.serials", "Серийники", "Serials"},
    {"report.dump.serialsMasked", "замаскированы", "masked"},
    {"report.dump.serialsPlain", "открыто", "plain"},
    {"report.dump.notes", "Заметки", "Notes"},
    {"report.dump.totals", "Итоги", "Totals"},
    {"report.dump.disks", "Дисков", "Disks"},
    {"report.dump.partitions", "Разделов", "Partitions"},
    {"report.dump.volumes", "Томов", "Volumes"},
    {"report.dump.candidates", "Кандидатов", "Candidates"},
    {"report.dump.operations", "Операций", "Operations"},
    {"report.dump.untouched", "Не тронуто", "Untouched"},
    {"report.dump.freed", "Освобождено", "Freed"},
    {"report.dump.notFreed", "Не освобождено", "Not freed"},
    {"report.dump.succeeded", "Выполнено", "Succeeded"},
    {"report.dump.partial", "Частично", "Partial"},
    {"report.dump.failed", "Провалено", "Failed"},
    {"report.dump.skipped", "Пропущено", "Skipped"},
    {"report.dump.errors", "Ошибок", "Errors"},
    {"report.dump.partitionMap", "Карта разделов", "Partition map"},
    {"report.dump.candidatesHeading", "Кандидаты", "Candidates"},
    {"report.dump.files", " файлов", " files"},
    {"report.dump.process", "процесс", "process"},
    {"report.dump.operationsHeading", "Операции", "Operations"},
    {"report.dump.untouchedHeading", "Не тронуто", "Untouched"},
    {"report.dump.errorsHeading", "Ошибки", "Errors"},
    {"report.dump.problemsHeading", "Замечания к отчёту", "Report problems"},
    {"report.dump.attempts", " попыток: ", " attempts: "},

    // Строка состояния и подпись сохранения. Пробел и разделитель « · » входят в
    {"report.status.freed", "Освобождено: ", "Freed: "},
    {"report.status.operations", " · операций: ", " · operations: "},
    {"report.status.errors", " · ошибок: ", " · errors: "},
    {"report.status.hidden", " · скрыто: ", " · hidden: "},
    {"report.status.reports", " · отчётов: ", " · reports: "},
    {"report.saved.pruned", " · удалено старых: ", " · pruned: "},

    // Сводка отчёта (строки «поле: значение»).
    {"report.summary.kind", "Вид", "Kind"},
    {"report.summary.started", "Начало", "Started"},
    {"report.summary.finished", "Конец", "Finished"},
    {"report.summary.duration", "Длительность", "Duration"},
    {"report.summary.app", "Приложение", "App"},
    {"report.summary.pid", "Процесс", "PID"},
    {"report.summary.rules", "Правила", "Rules"},
    {"report.summary.os", "ОС", "OS"},
    {"report.summary.osVersion", "Версия ОС", "OS version"},
    {"report.summary.arch", "Архитектура", "Arch"},
    {"report.summary.disks", "Диски", "Disks"},
    {"report.summary.partitions", "Разделы", "Partitions"},
    {"report.summary.volumes", "Тома", "Volumes"},
    {"report.summary.candidates", "Кандидаты", "Candidates"},
    {"report.summary.locked", "Держат файлы", "Locked"},
    {"report.summary.operations", "Операции", "Operations"},
    {"report.summary.untouched", "Не тронуто", "Untouched"},
    {"report.summary.freed", "Освобождено", "Freed"},
    {"report.summary.notFreed", "Не освобождено", "Not freed"},
    {"report.summary.succeeded", "Выполнено", "Succeeded"},
    {"report.summary.failed", "Провалено", "Failed"},
    {"report.summary.skipped", "Пропущено", "Skipped"},
    {"report.summary.errors", "Ошибки", "Errors"},
    {"report.summary.serials", "Серийники", "Serials"},

    // Подробности выбранной строки журнала.
    {"report.detail.kind", "Вид", "Kind"},
    {"report.detail.event", "Событие", "Event"},
    {"report.detail.level", "Уровень", "Level"},
    {"report.detail.message", "Сообщение", "Message"},
    {"report.detail.scope", "Этап", "Scope"},
    {"report.detail.code", "Код", "Code"},
    {"report.detail.operation", "Операция", "Operation"},
    {"report.detail.path", "Путь", "Path"},
    {"report.detail.repeats", "Повторов", "Attempts"},
    {"report.detail.category", "Категория", "Category"},
    {"report.detail.name", "Название", "Name"},
    {"report.detail.status", "Статус", "Status"},
    {"report.detail.action", "Действие", "Action"},
    {"report.detail.safety", "Уровень риска", "Safety"},
    {"report.detail.confidence", "Уверенность", "Confidence"},
    {"report.detail.size", "Объём", "Size"},
    {"report.detail.sizeExact", " ({0} Б)", " ({0} B)"},
    {"report.detail.attempts", "Попыток", "Attempts"},
    {"report.detail.started", "Начало", "Started"},
    {"report.detail.finished", "Конец", "Finished"},
    {"report.detail.duration", "Длительность", "Duration"},

    // Ошибки сохранения. Раньше обе печатались по-русски в обоих языках.
    {"report.save.empty", "пустой отчёт", "empty report"},
    {"report.save.noAppData",
     "не удалось определить %LOCALAPPDATA%",
     "cannot resolve %LOCALAPPDATA%"},

    // Пояснение вместо пустого дерева категорий на экране «Очистка».
    {"cleanup.hint.noScan", "Скан ещё не выполнялся", "No scan has run yet"},
    {"cleanup.hint.scanning",
     "Идёт обход: файлы и кэши считаются в фоне, дерево появится само.",
     "The walk is running: files and caches are counted in the background, the tree appears on"
     " its own."},
    {"cleanup.hint.idle",
     "Дерево категорий заполняется результатами скана. Ничего не удаляется без вашего выбора.",
     "The category tree is filled from the scan results. Nothing is deleted without your"
     " selection."},
    {"cleanup.hint.ruleCount", "{0} правило|{0} правила|{0} правил", "{0} rule|{0} rules"},
    {"cleanup.hint.categoryCount",
     "{0} категория|{0} категории|{0} категорий",
     "{0} category|{0} categories"},
    {"cleanup.hint.ruleSetTail",
     " — по ним и будет искаться мусор.",
     " — junk will be searched by them."},
    {"cleanup.hint.pressRescan", "Нажмите «{0}».", "Press {0}."},

    // Экран «Настройки». 47 строк лежали в локальной таблице view_settings.cpp
    {"settings.rules", "Правила", "Rules"},
    {"settings.ruleColumn", "Правило", "Rule"},
    {"settings.categoryColumn", "Категория", "Category"},
    {"settings.safetyColumn", "Уровень риска", "Risk level"},
    {"settings.rulesSummary",
     "Правил: {0} · включено: {1} · изменено: {2}",
     "Rules: {0} · enabled: {1} · changed: {2}"},
    {"settings.ruleEnabled", "Включено", "Enabled"},
    {"settings.ruleDisabled", "Выключено", "Disabled"},
    {"settings.ruleIdLabel", "Идентификатор", "Identifier"},
    {"settings.rulePathLabel", "Шаблон пути", "Path pattern"},
    {"settings.ruleAgeLabel", "Минимальный возраст, дней", "Minimum age, days"},
    {"settings.ruleCategoryLabel", "Категория", "Category"},
    {"settings.ruleSetLevelLabel", "Уровень в наборе", "Level in rule set"},
    {"settings.ruleNotSelected", "Правило не выбрано", "No rule selected"},
    {"settings.detailsTemplate",
     "{0} — категория: {1}; путь: {2}; возраст: {3} дн.; уровень: {4} (в наборе: {5}). {6}",
     "{0} — category: {1}; path: {2}; age: {3} d; level: {4} (in rule set: {5}). {6}"},
    {"settings.riskyConfirm",
     "Risky: нажмите ещё раз, чтобы подтвердить",
     "Risky: press again to confirm"},
    {"settings.filterLabel", "Фильтр", "Filter"},
    {"settings.noRulesFound", "Ничего не найдено", "Nothing found"},
    {"settings.builtinSet", "встроенный набор", "built-in rule set"},
    {"settings.neverChecked", "ещё не проверялось", "never checked"},
    {"settings.checking", "Проверка обновлений…", "Checking for updates…"},
    {"settings.noRuleSet", "Набор правил не загружен", "Rule set is not loaded"},
    {"settings.importRules", "Импорт набора правил", "Import rule set"},
    {"settings.exportRules", "Экспортировать набор", "Export rule set"},
    {"settings.exportStatistics", "Экспортировать статистику", "Export statistics"},
    {"settings.safetyLevelLabel", "Уровень риска", "Risk level"},
    {"settings.restoreDone",
     "Возвращён встроенный набор, автообновление выключено, решений сброшено: {0}",
     "Built-in rule set restored, auto update disabled, decisions dropped: {0}"},
    {"settings.overridesForgotten",
     "Решения по правилам возвращены к набору: сброшено {0}",
     "Rule decisions reset to the rule set: {0} dropped"},
    {"settings.aboutApp", "О программе: {0} — {1}", "About: {0} — {1}"},
    {"settings.aboutPrivacy",
     "Никакой телеметрии: статистика собирается только по кнопке",
     "No telemetry: statistics is collected only on demand"},
    {"settings.aboutAutostart",
     "Автозапуск и проверка по расписанию — в v1.2",
     "Autostart and scheduled checks come in v1.2"},
    {"settings.statisticsHeader",
     "MrProper: статистика (собрана вручную, телеметрии нет)",
     "MrProper: statistics (collected on demand, no telemetry)"},
    {"settings.statSource", "Источник", "Source"},
    {"settings.statVersion", "Версия набора", "Rule set version"},
    {"settings.statVerified", "Последняя проверенная версия", "Last verified version"},
    {"settings.statInstalled", "Установлен", "Installed"},
    {"settings.statChecked", "Последняя проверка", "Last check"},
    {"settings.statResult", "Результат", "Result"},
    {"settings.statAutoUpdate", "Автообновление", "Auto update"},
    {"settings.statLanguage", "Язык интерфейса", "Interface language"},
    {"settings.statRules", "Правил в наборе", "Rules in the set"},
    {"settings.statEnabled", "Включено правил", "Enabled rules"},
    {"settings.statChanged", "Изменено правил, всего", "Rules changed, total"},
    {"settings.statFilter", "Фильтр", "Filter"},
    {"settings.statSelected", "Выбранное правило", "Selected rule"},
    {"settings.empty.title", "Набор правил не прочитан", "The rule set was not read"},
    {"settings.empty.why",
     "Правила лежат на диске (рядом с программой или в %LOCALAPPDATA%\\MrProper\\rules) и"
     " читаются в фоновом потоке. Пока они не пришли, список пуст — и это не значит, что"
     " правил нет.",
     "Rules live on disk (next to the program or in %LOCALAPPDATA%\\MrProper\\rules) and are"
     " read in a background thread. Until they arrive the list is empty, which does not mean"
     " there are no rules."},
    {"settings.empty.what",
     "Если список не наполнился за несколько секунд, проверьте каталог правил и нажмите"
     " «Импорт» — причина отказа остаётся в строке состояния и в журнале.",
     "If the list stays empty for a few seconds, check the rules directory and press Import;"
     " the reason stays in the status line and in the log."},
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
