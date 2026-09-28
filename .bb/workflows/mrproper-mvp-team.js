// Команда агентов MrProper: 100 задач, волны по 5, одна модель.
// Каждая задача владеет непересекающимся набором файлов и обязана закончить
// машинно-проверяемой строкой RVI. Проверка — только через tools\build.bat
// и tools\test.bat (обёртки над MSVC), иначе результат не считается доказанным.
export const meta = {
  name: "mrproper-mvp-team",
  description: "MrProper MVP: 20 волн по 5 агентов, каждая задача с проверяемым RVI",
  phases: [
    { title: "W01 Сборка", detail: "CMake, пресеты, линтеры, CI, установщик-обвязка" },
    { title: "W02 Ядро: планирование", detail: "plan, trash, undo, sizing, rulesync" },
    { title: "W03 Ядро: отчёт и модель", detail: "JSON/HTML отчёт, лог, i18n, модель носителей" },
    { title: "W04 Правила: система", detail: "системные категории мусора" },
    { title: "W05 Правила: браузеры", detail: "профили и кэши браузеров" },
    { title: "W06 Правила: поставка", detail: "манифест, подпись, валидация, тесты правил" },
    { title: "W07 Платформа: разделы", detail: "диски, разделы, тома" },
    { title: "W08 Платформа: хранилище", detail: "свойства устройства, TRIM, GPT-типы" },
    { title: "W09 Платформа: ФС", detail: "обход каталогов, размеры, удаление" },
    { title: "W10 Платформа: процессы", detail: "Restart Manager, WMI, сеть, правила-sync" },
    { title: "W11 Движок: скан", detail: "координатор, сбор кандидатов" },
    { title: "W12 Движок: план", detail: "исполнитель, корзина, undo, журнал" },
    { title: "W13 CLI", detail: "headless-режим для CI и e2e" },
    { title: "W14 UI: каркас", detail: "окно, Direct2D, тема, локализация" },
    { title: "W15 UI: экраны", detail: "диски, очистка, отчёт, настройки" },
    { title: "W16 Тесты: ядро", detail: "юнит-тесты переносимых модулей" },
    { title: "W17 Тесты: платформа", detail: "интеграционные тесты Win32-слоя" },
    { title: "W18 E2E", detail: "сценарии на PowerShell, фикстуры" },
    { title: "W19 Ворота: безопасность", detail: "адверсарное ревью удаления данных" },
    { title: "W20 Ворота: приёмка", detail: "сборка всего, DoD из SPEC §12" },
  ],
};

// ---------------------------------------------------------------------------
// Таблица задач. files — исключительная собственность агента: чужие файлы
// нельзя трогать, их пишут параллельно.
// ---------------------------------------------------------------------------
const WAVES = [
  ["W01 Сборка", [
    { id: "01", title: "Укрепить систему сборки: пресеты, санитайзеры, install-цели", spec: "§8 Этап 0", files: "CMakeLists.txt, CMakePresets.json, cmake/*", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "02", title: "Вынести core/tests/engine/platform в отдельные CMake-цели", spec: "§6.1, §9", files: "src/core/CMakeLists.txt, tests/CMakeLists.txt, tests/unit/CMakeLists.txt, src/platform/CMakeLists.txt", verify: "tools\\build.bat Debug" },
    { id: "03", title: "Стиль и статический анализ: .clang-format, .clang-tidy, .editorconfig, конфиг warnings-as-errors", spec: "§9.1 ADR-001", files: ".clang-format, .clang-tidy, .editorconfig, cmake/warnings.cmake", verify: "cmake --build build/Debug --config Debug (через build.bat) без предупреждений /W4 /WX" },
    { id: "04", title: "CI: GitHub Actions windows-latest — configure, build, test, tidy, SBOM", spec: "§11, §9.1 ADR-010", files: ".github/workflows/ci.yml, .github/dependabot.yml", verify: "python3 -c \"import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml'))\"" },
    { id: "05", title: "Манифест приложения и версия: UAC asInvoker, ресурсы версии, метаданные", spec: "§4 FR-9, §5", files: "packaging/app.manifest, packaging/version.rc, packaging/README.md", verify: "файлы существуют, манифест — валидный XML (python3 xml.etree)" },
  ]],
  ["W02 Ядро: планирование", [
    { id: "06", title: "core/plan: построение плана очистки, агрегаты, порог отбора, dry-run", spec: "§4 FR-5", files: "src/core/plan.hpp, src/core/plan.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "07", title: "core/sizing: учёт allocated против logical, sparse, агрегация reclaimable", spec: "§4 FR-4", files: "src/core/sizing.hpp, src/core/sizing.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "08", title: "core/trash: манифест корзины, лимиты (2 ГБ / 7 дней), транзакции", spec: "§4 FR-7, §9.1 ADR-005", files: "src/core/trash.hpp, src/core/trash.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "09", title: "core/undo: восстановление из корзины, разрешение конфликтов, частичное восстановление", spec: "§4 FR-7", files: "src/core/undo.hpp, src/core/undo.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "10", title: "core/rulesync: проверка манифеста, SHA-256 каждого файла, версия схемы, откат", spec: "§9.2, ADR-008", files: "src/core/rulesync.hpp, src/core/rulesync.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
  ]],
  ["W03 Ядро: отчёт и модель", [
    { id: "11", title: "core/report_json: детерминированный JSON-отчёт (карта разделов, кандидаты, операции, ошибки)", spec: "§4 FR-8", files: "src/core/report_json.hpp, src/core/report_json.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "12", title: "core/report_html: самодостаточный HTML без внешних зависимостей, маскирование серийников", spec: "§4 FR-8, §1.1", files: "src/core/report_html.hpp, src/core/report_html.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "13", title: "core/log: структурный лог в файл плюс кольцевой буфер для UI", spec: "§6.2", files: "src/core/log.hpp, src/core/log.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "14", title: "core/i18n: строки ru/en без перезапуска, подстановка параметров", spec: "§5 локализация", files: "src/core/i18n.hpp, src/core/i18n.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "15", title: "core/disk_model: диски/разделы/тома в переносимой форме, вычисление свободного места", spec: "§6.3", files: "src/core/disk_model.hpp, src/core/disk_model.cpp", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
  ]],
  ["W04 Правила: система", [
    { id: "16", title: "Правила: temp.user, temp.system, logs, wer", spec: "§4 FR-3", files: "rules/temp.user.json, rules/temp.system.json, rules/logs.json, rules/wer.json", verify: "tools\\test.bat Debug (загрузчик правил принимает набор)" },
    { id: "17", title: "Правила: crash.dumps, memory.dumps, prefetch (risky, выкл. по умолчанию)", spec: "§4 FR-3", files: "rules/crash.dumps.json, rules/memory.dumps.json, rules/prefetch.json", verify: "tools\\test.bat Debug" },
    { id: "18", title: "Правила: ms.update, delivery.opt, winsxs (только оценка, никакого удаления руками)", spec: "§4 FR-3, §10", files: "rules/ms.update.json, rules/delivery.opt.json, rules/winsxs.report.json", verify: "tools\\test.bat Debug" },
    { id: "19", title: "Правила: icon.font.cache, shadercache, recycle.bin", spec: "§4 FR-3", files: "rules/icon.font.cache.json, rules/shadercache.json, rules/recycle.bin.json", verify: "tools\\test.bat Debug" },
    { id: "20", title: "Правила: installer.cache (risky, только оценка), user.bigfiles (review)", spec: "§4 FR-3", files: "rules/installer.cache.json, rules/user.bigfiles.json", verify: "tools\\test.bat Debug" },
  ]],
  ["W05 Правила: браузеры", [
    { id: "21", title: "Правила кэша Chrome и Edge: Cache, Code Cache, GPUCache, Service Worker", spec: "§4 FR-3", files: "rules/browser.cache.chrome.json, rules/browser.cache.edge.json", verify: "tools\\test.bat Debug" },
    { id: "22", title: "Правила кэша Firefox и Thunderbird: cache2, startupCache, offlineCache, shader-cache", spec: "§4 FR-3", files: "rules/browser.cache.firefox.json, rules/browser.cache.thunderbird.json", verify: "tools\\test.bat Debug" },
    { id: "23", title: "Правила Brave, Vivaldi, Opera (Chromium-профили), Яндекс.Браузер", spec: "§4 FR-3", files: "rules/browser.cache.chromium.json, rules/browser.cache.yandex.json", verify: "tools\\test.bat Debug" },
    { id: "24", title: "Правила истории и cookies браузеров — строго review, выключены по умолчанию", spec: "§4 FR-3, §2", files: "rules/browser.history.json", verify: "tools\\test.bat Debug" },
    { id: "25", title: "Правила кэшей инструментов разработки: npm, pip, NuGet, WSL/Docker (только оценка)", spec: "§4 FR-3", files: "rules/dev.caches.json, rules/wsl.vhdx.report.json", verify: "tools\\test.bat Debug" },
  ]],
  ["W06 Правила: поставка", [
    { id: "26", title: "Манифест набора правил: schemaVersion, версия, minAppVersion, SHA-256 каждого файла", spec: "§9.2", files: "rules/manifest.json", verify: "python3 -m json.tool rules/manifest.json > /dev/null" },
    { id: "27", title: "Скрипт подписи набора правил: Ed25519, приватный ключ вне репозитория", spec: "§9.2, ADR-008", files: "tools/sign-rules.ps1, tools/rule_keys.md", verify: "PowerShell -Command без ошибок парсинга скрипта" },
    { id: "28", title: "Тест согласованности: манифест ↔ файлы правил, хеши, уникальность id, минимум правил", spec: "§4 FR-4, §9.2", files: "tests/unit/rules_manifest_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "29", title: "Фаззинг правил: битый JSON, неизвестные поля, режект-символы не роняют загрузчик", spec: "§11.5", files: "tests/unit/rules_fuzz_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "30", title: "Документ авторства правил: как добавить категорию, что можно чистить безопасно", spec: "§4 FR-4, §9.2", files: "docs/rules-authoring.md", verify: "файл существует и содержит все поля схемы правила" },
  ]],
  ["W07 Платформа: разделы", [
    { id: "31", title: "RAII-обёртки Win32: unique_handle, ошибки с HRESULT и текстом, UTF-8↔UTF-16", spec: "§9.1 ADR-001", files: "src/platform/win_handle.hpp, src/platform/win_error.hpp", verify: "tools\\build.bat Debug" },
    { id: "32", title: "Перечисление физических дисков: SetupAPI GUID_DEVINTERFACE_DISK, STORAGE_GET_DEVICE_NUMBER", spec: "§4 FR-1 п.1", files: "src/platform/devices.cpp, src/platform/devices.hpp", verify: "tools\\build.bat Debug" },
    { id: "33", title: "Разметка и разделы: IOCTL_DISK_GET_DRIVE_LAYOUT_EX, GPT-типы, MBR-типы, атрибуты", spec: "§4 FR-1 п.4", files: "src/platform/layout.cpp, src/platform/layout.hpp", verify: "tools\\build.bat Debug" },
    { id: "34", title: "Тома и точки монтирования: FindFirstVolumeW, VOLUME_GET_VOLUME_DISK_EXTENTS, GetVolumeInformationW", spec: "§4 FR-1 п.5,6", files: "src/platform/volumes.cpp, src/platform/volumes.hpp", verify: "tools\\build.bat Debug" },
    { id: "35", title: "Размер диска и свободное место: GET_LENGTH_INFO, GetDiskFreeSpaceExW, таймауты на мёртвые устройства", spec: "§4 FR-1 п.3", files: "src/platform/size_probe.cpp, src/platform/size_probe.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W08 Платформа: хранилище", [
    { id: "36", title: "Свойства устройства: StorageDeviceProperty, модель, серийник, прошивка, removable", spec: "§4 FR-1 п.2", files: "src/platform/storage_query.cpp, src/platform/storage_query.hpp", verify: "tools\\build.bat Debug" },
    { id: "37", title: "Свойства адаптера: тип шины (NVMe/SATA/USB/SCSI/RAID) по BusType", spec: "§4 FR-1 п.2", files: "src/platform/bus_type.cpp, src/platform/bus_type.hpp", verify: "tools\\build.bat Debug" },
    { id: "38", title: "TRIM и кэш записи: StorageDeviceTrimProperty, WriteCacheProperty, IoCapabilityProperty", spec: "§4 FR-1 п.2", files: "src/platform/trim_cache.cpp, src/platform/trim_cache.hpp", verify: "tools\\build.bat Debug" },
    { id: "39", title: "Маппинг GPT-типа на PartitionKind (BasicData, MSR, EFI, Recovery, OEM, Reserved)", spec: "§6.3", files: "src/platform/gpt_kind.cpp, src/platform/gpt_kind.hpp", verify: "tools\\build.bat Debug" },
    { id: "40", title: "Кэш инвентаризации и реакция на WM_DEVICECHANGE, деград-режим при отказе устройства", spec: "§4 FR-1, §10", files: "src/platform/inventory.cpp, src/platform/inventory.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W09 Платформа: ФС", [
    { id: "41", title: "Обход каталогов: reparse points, длинные пути через \\\\, не-ASCII, отмена", spec: "§4 FR-6", files: "src/platform/vfs_walk.cpp, src/platform/vfs_walk.hpp", verify: "tools\\build.bat Debug" },
    { id: "42", title: "Размеры: аллоцированный против логического, sparse, GetCompressedFileSizeW", spec: "§4 FR-4, §6.3", files: "src/platform/vfs_size.cpp, src/platform/vfs_size.hpp", verify: "tools\\build.bat Debug" },
    { id: "43", title: "Нормализация путей: GetFinalPathNameByHandleW, проверка попадания внутрь корня правила", spec: "§4 FR-6, §10", files: "src/platform/vfs_paths.cpp, src/platform/vfs_paths.hpp", verify: "tools\\build.bat Debug" },
    { id: "44", title: "Удаление: DeleteFileW/RemoveDirectoryW, retry с backoff, пропуск занятых, запрет на защищённые пути", spec: "§4 FR-6, §10", files: "src/platform/vfs_delete.cpp, src/platform/vfs_delete.hpp", verify: "tools\\build.bat Debug" },
    { id: "45", title: "Файловая корзина приложения: перемещение, кросс-томовый случай, манифест на диске", spec: "§4 FR-7, ADR-005", files: "src/platform/vfs_trash.cpp, src/platform/vfs_trash.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W10 Платформа: процессы", [
    { id: "46", title: "Restart Manager: RmStartSession/RmRegisterResources/RmGetList, кто держит файл", spec: "§4 FR-6", files: "src/platform/restart_manager.cpp, src/platform/restart_manager.hpp", verify: "tools\\build.bat Debug" },
    { id: "47", title: "Закрытие процессов по запросу: RmShutdown с явным подтверждением пользователя", spec: "§4 FR-6", files: "src/platform/process_control.cpp, src/platform/process_control.hpp", verify: "tools\\build.bat Debug" },
    { id: "48", title: "WMI: COM-инициализация, Win32_EncryptVolume (BitLocker), MSFT_PhysicalDisk, StorageFault", spec: "§4 FR-1 п.7", files: "src/platform/wmi.cpp, src/platform/wmi.hpp", verify: "tools\\build.bat Debug" },
    { id: "49", title: "Сеть: WinHTTP только GET, таймауты, TLS, никаких куки и авторизации", spec: "§9.2", files: "src/platform/net.cpp, src/platform/net.hpp", verify: "tools\\build.bat Debug" },
    { id: "50", title: "Клиент rulesync: скачать манифест+подпись, проверить, применить атомарно, откат при ошибке", spec: "§9.2, ADR-008", files: "src/platform/rulesync_client.cpp, src/platform/rulesync_client.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W11 Движок: скан", [
    { id: "51", title: "engine/scan_coordinator: пул потоков, std::stop_token, прогресс, отмена", spec: "§6.4", files: "src/engine/scan_coordinator.cpp, src/engine/scan_coordinator.hpp", verify: "tools\\build.bat Debug" },
    { id: "52", title: "engine/candidate_collector: обход локаторов правил, min-age, excludes, группировка по профилю", spec: "§4 FR-3, FR-4", files: "src/engine/candidate_collector.cpp, src/engine/candidate_collector.hpp", verify: "tools\\build.bat Debug" },
    { id: "53", title: "engine/scoring_bridge: подстановка данных в core::scoreCandidate, порог отбора 50", spec: "§4 FR-4, scoring.hpp", files: "src/engine/scoring_bridge.cpp, src/engine/scoring_bridge.hpp", verify: "tools\\build.bat Debug" },
    { id: "54", title: "engine/locks: определение блокировок Restart Manager для кандидатов", spec: "§4 FR-6", files: "src/engine/locks.cpp, src/engine/locks.hpp", verify: "tools\\build.bat Debug" },
    { id: "55", title: "engine/scan_result: снимок результата, неизменяемый после публикации, статистика прогона", spec: "§6.4", files: "src/engine/scan_result.cpp, src/engine/scan_result.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W12 Движок: план", [
    { id: "56", title: "engine/plan_builder: выборы пользователя, агрегаты, dry-run по умолчанию", spec: "§4 FR-5", files: "src/engine/plan_builder.cpp, src/engine/plan_builder.hpp", verify: "tools\\build.bat Debug" },
    { id: "57", title: "engine/executor: параллельное выполнение, чек-лист, Skip для занятых, сбор ошибок", spec: "§4 FR-6", files: "src/engine/executor.cpp, src/engine/executor.hpp", verify: "tools\\build.bat Debug" },
    { id: "58", title: "engine/journal: журнал транзакций, снимок плана перед удалением", spec: "§4 FR-5, FR-6", files: "src/engine/journal.cpp, src/engine/journal.hpp", verify: "tools\\build.bat Debug" },
    { id: "59", title: "engine/trash_service: ФС-реализация поверх core/trash с лимитами и очисткой", spec: "§4 FR-7", files: "src/engine/trash_service.cpp, src/engine/trash_service.hpp", verify: "tools\\build.bat Debug" },
    { id: "60", title: "engine/undo_service: восстановление, частичное, конфликты без перезаписи", spec: "§4 FR-7", files: "src/engine/undo_service.cpp, src/engine/undo_service.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W13 CLI", [
    { id: "61", title: "mrproper-cli: каркас, --version, --help, разбор аргументов, коды возврата", spec: "§6.2", files: "src/cli/main.cpp, src/cli/args.cpp, src/cli/args.hpp", verify: "tools\\build.bat Debug и запуск exe --version" },
    { id: "62", title: "CLI: disks --json (эталонный дамп карты разделов)", spec: "§8 Этап 1", files: "src/cli/cmd_disks.cpp, src/cli/cmd_disks.hpp", verify: "tools\\build.bat Debug и exe disks --json | head -5" },
    { id: "63", title: "CLI: scan --json с прогрессом в stderr, чтобы stdout оставался машинным", spec: "§8 Этап 2", files: "src/cli/cmd_scan.cpp, src/cli/cmd_scan.hpp", verify: "tools\\build.bat Debug" },
    { id: "64", title: "CLI: plan --json и apply с обязательным подтверждением, --dry-run по умолчанию", spec: "§4 FR-5", files: "src/cli/cmd_apply.cpp, src/cli/cmd_apply.hpp", verify: "tools\\build.bat Debug" },
    { id: "65", title: "CLI: rules validate и rules verify (подпись/хеш), report --html, exit-коды для CI", spec: "§9.2, §11", files: "src/cli/cmd_rules.cpp, src/cli/cmd_report.cpp", verify: "tools\\build.bat Debug и exe rules validate (код 0 на текущем наборе)" },
  ]],
  ["W14 UI: каркас", [
    { id: "66", title: "Каркас приложения: окно, регистрация классов, WM_DPICHANGED, per-monitor v2, закрытие", spec: "§7, §5", files: "src/ui/app_shell.cpp, src/ui/app_shell.hpp", verify: "tools\\build.bat Debug" },
    { id: "67", title: "Direct2D-рендерер: DXGI swap chain, D2D/DirectWrite, feature probe и fallback на GDI", spec: "§7, ADR-003, §10", files: "src/ui/renderer.cpp, src/ui/renderer.hpp", verify: "tools\\build.bat Debug" },
    { id: "68", title: "Тема: светлая/тёмная из реестра, палитра, шрифты с учётом DPI", spec: "§5", files: "src/ui/theme.cpp, src/ui/theme.hpp", verify: "tools\\build.bat Debug" },
    { id: "69", title: "Локализация в UI: загрузка строк, переключение ru/en без перезапуска, RTL-ready разметка", spec: "§5", files: "src/ui/locale.cpp, src/ui/locale.hpp", verify: "tools\\build.bat Debug" },
    { id: "70", title: "Навигация и страницы: рельс, 5 страниц, переключение, сохранение состояния", spec: "§7.1", files: "src/ui/nav.cpp, src/ui/nav.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W15 UI: экраны", [
    { id: "71", title: "Экран «Диски»: карта разделов на Direct2D плюс дерево на ListView с NM_CUSTOMDRAW", spec: "§7, §4 FR-2, ADR-003", files: "src/ui/view_disks.cpp, src/ui/view_disks.hpp", verify: "tools\\build.bat Debug" },
    { id: "72", title: "Экран «Очистка»: дерево категорий, чекбоксы, агрегаты, прогресс и отмена", spec: "§4 FR-3, FR-5", files: "src/ui/view_cleanup.cpp, src/ui/view_cleanup.hpp", verify: "tools\\build.bat Debug" },
    { id: "73", title: "Экран «Отчёт»: журнал операций, ошибки, экспорт HTML/JSON", spec: "§4 FR-8", files: "src/ui/view_report.cpp, src/ui/view_report.hpp", verify: "tools\\build.bat Debug" },
    { id: "74", title: "Экран «Настройки»: правила, уровни риска, версия набора, проверка обновлений, сброс", spec: "§4 FR-9, §9.2", files: "src/ui/view_settings.cpp, src/ui/view_settings.hpp", verify: "tools\\build.bat Debug" },
    { id: "75", title: "Мост модель-представление: наблюдатели, маршалинг фонового потока в UI-поток, отсутствие блокировок", spec: "§6.4", files: "src/ui/mv_bridge.cpp, src/ui/mv_bridge.hpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W16 Тесты: ядро", [
    { id: "76", title: "Юнит-тесты core/plan: агрегаты, порог отбора, поведение при пустых кандидатах", spec: "§11.1", files: "tests/unit/plan_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "77", title: "Юнит-тесты core/sizing: sparse, нули, переполнение, агрегация", spec: "§11.1", files: "tests/unit/sizing_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "78", title: "Юнит-тесты core/trash и core/undo: лимиты, конфликты, частичное восстановление", spec: "§11.1", files: "tests/unit/trash_undo_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "79", title: "Юнит-тесты core/rulesync: битая подпись, несовпадение хеша, откат, офлайн-режим", spec: "§9.2", files: "tests/unit/rulesync_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "80", title: "Юнит-тесты core/report_json и report_html: детерминизм, экранирование, маскирование серийников", spec: "§4 FR-8", files: "tests/unit/report_tests.cpp", verify: "tools\\test.bat Debug" },
  ]],
  ["W17 Тесты: платформа", [
    { id: "81", title: "Интеграционный тест обхода ФС: создать дерево каталогов, просканировать, сверить сумму", spec: "§11.2", files: "tests/integration/vfs_walk_tests.cpp", verify: "tools\\build.bat Debug и exe интеграционных тестов" },
    { id: "82", title: "Интеграционный тест длинных путей, юникода и reparse-петли", spec: "§11.2", files: "tests/integration/vfs_edge_tests.cpp", verify: "tools\\build.bat Debug" },
    { id: "83", title: "Интеграционный тест инвентаризации против эталонного дампа, зумф на отказ устройства", spec: "§11.2, §11.4", files: "tests/integration/inventory_tests.cpp", verify: "tools\\build.bat Debug" },
    { id: "84", title: "Тест защищённых путей: ни одна операция не выходит за корень своего правила", spec: "§10, §12", files: "tests/integration/guard_tests.cpp", verify: "tools\\build.bat Debug" },
    { id: "85", title: "Тест отмены: stop_token прерывает скан и удаление, состояние остаётся согласованным", spec: "§6.4, §11.3", files: "tests/integration/cancel_tests.cpp", verify: "tools\\build.bat Debug" },
  ]],
  ["W18 E2E", [
    { id: "86", title: "E2E-фикстуры: генератор синтетического мусора заданного объёма и категорий", spec: "§11.3", files: "tests/e2e/New-JunkFixture.ps1", verify: "PowerShell -Command парсинг файла без ошибок" },
    { id: "87", title: "E2E-сценарий полного цикла: скан, план, очистка, отчёт, проверка освобождения", spec: "§11.3", files: "tests/e2e/Test-FullCycle.ps1", verify: "парсинг PowerShell без ошибок" },
    { id: "88", title: "E2E-сценарий отмены и восстановления: 100 процентов файлов возвращены", spec: "§11.3, §12", files: "tests/e2e/Test-UndoRestore.ps1", verify: "парсинг PowerShell без ошибок" },
    { id: "89", title: "E2E-сценарий битых правил: приложение стартует на предыдущем рабочем наборе", spec: "§9.2, ADR-008", files: "tests/e2e/Test-BadRules.ps1", verify: "парсинг PowerShell без ошибок" },
    { id: "90", title: "E2E-сценарий защиты: битые секторы, занятые файлы, зашифрованный том, отсутствие прав", spec: "§11.3", files: "tests/e2e/Test-EdgeCases.ps1", verify: "парсинг PowerShell без ошибок" },
  ]],
  ["W19 Ворота: безопасность", [
    { id: "91", title: "Адверсное ревью удаления: может ли кандидат выйти за корень правила? Ищи обходы", spec: "§10, §12", files: "ревью без правок кода, отчёт в docs/review-01.md", verify: "docs/review-01.md содержит вердикт по каждой находке" },
    { id: "92", title: "Адверсное ревью правил: может ли правило удалить пользовательские данные?", spec: "§10, §4 FR-4", files: "ревью, отчёт docs/review-02.md", verify: "docs/review-02.md существует" },
    { id: "93", title: "Ревью Win32-обёрток: утечки HANDLE, неверные коды ошибок, таймауты", spec: "§9.1, §10", files: "отчёт docs/review-03.md", verify: "docs/review-03.md существует" },
    { id: "94", title: "Ревью потоков и отмены: гонки, публикация результатов, отсутствие блокировок в UI", spec: "§6.4", files: "отчёт docs/review-04.md", verify: "docs/review-04.md существует" },
    { id: "95", title: "Ревью UX безопасности: ни один элемент не удаляется без объяснения и уровня риска", spec: "§2 G4, §12", files: "отчёт docs/review-05.md", verify: "docs/review-05.md существует" },
  ]],
  ["W20 Ворота: приёмка", [
    { id: "96", title: "Свести находки ревью в список дефектов с приоритетом и владельцем файла", spec: "§12", files: "docs/defects.md", verify: "docs/defects.md существует и не пуст" },
    { id: "97", title: "Починить дефекты безопасности из docs/defects.md с приоритетом critical и high", spec: "§12", files: "только файлы, перечисленные в docs/defects.md для critical/high", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "98", title: "Полная сборка Release и прогон всех тестов, отчёт с числами", spec: "§12", files: "docs/build-report.md", verify: "tools\\build.bat Release && tools\\test.bat Release" },
    { id: "99", title: "Чек-лист DoD из SPEC §12: пройти пункт за пунктом, честно отметить невыполненное", spec: "§12", files: "docs/dod-checklist.md", verify: "docs/dod-checklist.md покрывает все пункты §12" },
    { id: "100", title: "Итоговый отчёт команды: что сделано, что проверено, что осталось и почему", spec: "§8, §12", files: "docs/team-report.md", verify: "docs/team-report.md существует, содержит сводку RVI по волнам" },
  ]],
];

const PREAMBLE = [
  "Ты — субагент команды MrProper. Проект: C++20, Windows 10/11 x64, десктоп-утилита",
  "очистки диска. Репозиторий: D:\\Project\\MrProper (из WSL: /mnt/d/Project/MrProper).",
  "Спецификация: docs/SPEC.md — прочитай свои разделы ДО написания кода, не выдумывай API.",
  "Она уже есть в репозитории; если файла нет — напиши это в RVI и работай по описанию задачи.",
  "",
  "ЖЁСТКИЕ ПРАВИЛА:",
  "1. Владеешь ТОЛЬКО перечисленными файлами. Чужие файлы трогать нельзя — их пишут параллельно.",
  "2. Никогда не редактируй docs/SPEC.md, CMakeLists.txt верхнего уровня, tools/build.bat,",
  "   tools/test.bat — иначе сломаешь чужие задачи и общий тулчейн.",
  "3. Сборка и тесты — ТОЛЬКО через tools\\build.bat <Debug|Release> и tools\\test.bat <Debug|Release>.",
  "   Из WSL это вызывается так: cmd.exe /c \"tools\\build.bat Debug\" из каталога репозитория.",
  "   Прямой вызов cl.exe, cmake из WSL или g++ для Windows-кода запрещён: такой результат не считается проверенным.",
  "4. Не делай git commit — коммитит оркестратор.",
  "5. Последняя строка твоего ответа — ровно: RVI: ok|fail|skip | <команда, которую ты выполнил и её вывод>",
  "   ok ставится ТОЛЬКО если ты сам выполнил указанную команду и она прошла. Иначе fail или skip с причиной.",
  "6. Не притворяйся успехом. Частично сделанная задача — это skip с честным описанием.",
  "7. Код без собранной команды не считается результатом. Стиль: C++20, 4 пробела, RAII, без исключений в горячих циклах.",
  "8. Комментарии и тексты в коде — по-русски, если это не противоречит стилю существующих файлов.",
].join("\n");

// Схема намеренно минимальна: чем меньше обязательных полей, тем реже модель
// нарушает формат. На практике строгий массив строк ронял ~40% агентов.
const resultSchema = {
  type: "object",
  required: ["id", "rvi", "summary"],
  properties: {
    id: { type: "string" },
    rvi: { enum: ["ok", "fail", "skip"] },
    summary: { type: "string" },
    verificationCommand: { type: "string" },
    filesTouched: { type: "string", description: "список файлов через запятую" },
    blocker: { type: "string" },
  },
};

const maxTasks = args && args.maxTasks ? args.maxTasks : 100;
const budgetInfo = budget();
log("лимиты запуска: " + JSON.stringify(budgetInfo));
log("планируется задач: " + maxTasks + ", одновременно 5");

const flat = [];
for (const [phaseTitle, tasks] of WAVES) {
  for (const t of tasks) flat.push({ phaseTitle, t });
}
const selected = flat.slice(0, maxTasks);

const byPhase = new Map();
for (const item of selected) {
  if (!byPhase.has(item.phaseTitle)) byPhase.set(item.phaseTitle, []);
  byPhase.get(item.phaseTitle).push(item.t);
}

const report = [];
for (const [phaseTitle, tasks] of byPhase) {
  phase(phaseTitle);
  log("волна " + phaseTitle + ": " + tasks.length + " задач");
  for (let i = 0; i < tasks.length; i += 5) {
    const chunk = tasks.slice(i, i + 5);
    const done = await parallel(
      chunk.map((t) => () =>
        agent(
          PREAMBLE +
            "\n\nЗАДАЧА " + t.id + ": " + t.title +
            "\nСпека: " + t.spec +
            "\nТвои файлы: " + t.files +
            "\nКритерий проверки: " + t.verify +
            "\n\nСделай задачу полностью. Если критерий требует недоступного (например, прав админа или второй системы) — верни skip с точной причиной.",
          { label: t.id + " " + t.title.slice(0, 40), phase: phaseTitle, schema: resultSchema },
        ).then((r) => (r ? Object.assign(r, { id: t.id }) : { id: t.id, rvi: "fail", summary: "агент упал или нарушил схему ответа", filesTouched: "", verificationCommand: "", blocker: "worker error" })),
      ),
    );
    for (const r of done.filter(Boolean)) {
      report.push(r);
      log("  " + r.id + " → " + r.rvi + (r.verificationCommand ? " (" + r.verificationCommand + ")" : ""));
    }
  }
}

const okCount = report.filter((r) => r.rvi === "ok").length;
log("итого задач: " + report.length + ", ok: " + okCount);
return { total: report.length, ok: okCount, results: report };
