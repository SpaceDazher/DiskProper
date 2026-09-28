export const meta = {
  name: "mrproper-mvp-r3",
  description: "MrProper MVP, третий запуск: 41 задача (56-66 и 71-100), слот сборки на агента, сериализация компиляции, параллелизм 2",
  phases: [
    { title: "W12 Движок: план", detail: "исполнитель, корзина, undo, журнал" },
    { title: "W13 CLI", detail: "headless-режим для CI и e2e" },
    { title: "W14 UI: каркас", detail: "окно, Direct2D, тема, локализация" },
    { title: "W15 UI: экраны", detail: "диски, очистка, отчёт, настройки" },
    { title: "W16 Тесты: ядро", detail: "юнит-тесты переносимых модулей" },
    { title: "W17 Тесты: платформа", detail: "интеграционные тесты Win32-слоя" },
    { title: "W18 E2E", detail: "сценарии на PowerShell, фикстуры" },
    { title: "W19 Ворота: безопасность", detail: "адверсарное ревью удаления данных" },
    { title: "W20 Ворота: приёмка", detail: "сборка всего, DoD из SPEC §12" }
  ],
};

const WAVES = [
  ["W12 Движок: план", [
    { id: "56", title: "engine/plan_builder: выборы пользователя, агрегаты, dry-run по умолчанию", spec: "§4 FR-5", files: "src/engine/plan_builder.cpp, src/engine/plan_builder.hpp", verify: "tools\\build.bat Debug" },
    { id: "57", title: "engine/executor: параллельное выполнение, чек-лист, Skip для занятых, сбор ошибок", spec: "§4 FR-6", files: "src/engine/executor.cpp, src/engine/executor.hpp", verify: "tools\\build.bat Debug" },
    { id: "58", title: "engine/journal: журнал транзакций, снимок плана перед удалением", spec: "§4 FR-5, FR-6", files: "src/engine/journal.cpp, src/engine/journal.hpp", verify: "tools\\build.bat Debug" },
    { id: "59", title: "engine/trash_service: ФС-реализация поверх core/trash с лимитами и очисткой", spec: "§4 FR-7", files: "src/engine/trash_service.cpp, src/engine/trash_service.hpp", verify: "tools\\build.bat Debug" },
    { id: "60", title: "engine/undo_service: восстановление, частичное, конфликты без перезаписи", spec: "§4 FR-7", files: "src/engine/undo_service.cpp, src/engine/undo_service.hpp", verify: "tools\\build.bat Debug" }
  ]],
  ["W13 CLI", [
    { id: "61", title: "mrproper-cli: каркас, --version, --help, разбор аргументов, коды возврата", spec: "§6.2", files: "src/cli/main.cpp, src/cli/args.cpp, src/cli/args.hpp", verify: "tools\\build.bat Debug и запуск exe --version" },
    { id: "62", title: "CLI: disks --json (эталонный дамп карты разделов)", spec: "§8 Этап 1", files: "src/cli/cmd_disks.cpp, src/cli/cmd_disks.hpp", verify: "tools\\build.bat Debug и exe disks --json | head -5" },
    { id: "63", title: "CLI: scan --json с прогрессом в stderr, чтобы stdout оставался машинным", spec: "§8 Этап 2", files: "src/cli/cmd_scan.cpp, src/cli/cmd_scan.hpp", verify: "tools\\build.bat Debug" },
    { id: "64", title: "CLI: plan --json и apply с обязательным подтверждением, --dry-run по умолчанию", spec: "§4 FR-5", files: "src/cli/cmd_apply.cpp, src/cli/cmd_apply.hpp", verify: "tools\\build.bat Debug" },
    { id: "65", title: "CLI: rules validate и rules verify (подпись/хеш), report --html, exit-коды для CI", spec: "§9.2, §11", files: "src/cli/cmd_rules.cpp, src/cli/cmd_report.cpp", verify: "tools\\build.bat Debug и exe rules validate (код 0 на текущем наборе)" }
  ]],
  ["W14 UI: каркас", [
    { id: "66", title: "Каркас приложения: окно, регистрация классов, WM_DPICHANGED, per-monitor v2, закрытие", spec: "§7, §5", files: "src/ui/app_shell.cpp, src/ui/app_shell.hpp", verify: "tools\\build.bat Debug" }
  ]],
  ["W15 UI: экраны", [
    { id: "71", title: "Экран «Диски»: карта разделов на Direct2D плюс дерево на ListView с NM_CUSTOMDRAW", spec: "§7, §4 FR-2, ADR-003", files: "src/ui/view_disks.cpp, src/ui/view_disks.hpp", verify: "tools\\build.bat Debug" },
    { id: "72", title: "Экран «Очистка»: дерево категорий, чекбоксы, агрегаты, прогресс и отмена", spec: "§4 FR-3, FR-5", files: "src/ui/view_cleanup.cpp, src/ui/view_cleanup.hpp", verify: "tools\\build.bat Debug" },
    { id: "73", title: "Экран «Отчёт»: журнал операций, ошибки, экспорт HTML/JSON", spec: "§4 FR-8", files: "src/ui/view_report.cpp, src/ui/view_report.hpp", verify: "tools\\build.bat Debug" },
    { id: "74", title: "Экран «Настройки»: правила, уровни риска, версия набора, проверка обновлений, сброс", spec: "§4 FR-9, §9.2", files: "src/ui/view_settings.cpp, src/ui/view_settings.hpp", verify: "tools\\build.bat Debug" },
    { id: "75", title: "Мост модель-представление: наблюдатели, маршалинг фонового потока в UI-поток, отсутствие блокировок", spec: "§6.4", files: "src/ui/mv_bridge.cpp, src/ui/mv_bridge.hpp", verify: "tools\\build.bat Debug" }
  ]],
  ["W16 Тесты: ядро", [
    { id: "76", title: "Юнит-тесты core/plan: агрегаты, порог отбора, поведение при пустых кандидатах", spec: "§11.1", files: "tests/unit/plan_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "77", title: "Юнит-тесты core/sizing: sparse, нули, переполнение, агрегация", spec: "§11.1", files: "tests/unit/sizing_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "78", title: "Юнит-тесты core/trash и core/undo: лимиты, конфликты, частичное восстановление", spec: "§11.1", files: "tests/unit/trash_undo_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "79", title: "Юнит-тесты core/rulesync: битая подпись, несовпадение хеша, откат, офлайн-режим", spec: "§9.2", files: "tests/unit/rulesync_tests.cpp", verify: "tools\\test.bat Debug" },
    { id: "80", title: "Юнит-тесты core/report_json и report_html: детерминизм, экранирование, маскирование серийников", spec: "§4 FR-8", files: "tests/unit/report_tests.cpp", verify: "tools\\test.bat Debug" }
  ]],
  ["W17 Тесты: платформа", [
    { id: "81", title: "Интеграционный тест обхода ФС: создать дерево каталогов, просканировать, сверить сумму", spec: "§11.2", files: "tests/integration/vfs_walk_tests.cpp", verify: "tools\\build.bat Debug и exe интеграционных тестов" },
    { id: "82", title: "Интеграционный тест длинных путей, юникода и reparse-петли", spec: "§11.2", files: "tests/integration/vfs_edge_tests.cpp", verify: "tools\\build.bat Debug" },
    { id: "83", title: "Интеграционный тест инвентаризации против эталонного дампа, зумф на отказ устройства", spec: "§11.2, §11.4", files: "tests/integration/inventory_tests.cpp", verify: "tools\\build.bat Debug" },
    { id: "84", title: "Тест защищённых путей: ни одна операция не выходит за корень своего правила", spec: "§10, §12", files: "tests/integration/guard_tests.cpp", verify: "tools\\build.bat Debug" },
    { id: "85", title: "Тест отмены: stop_token прерывает скан и удаление, состояние остаётся согласованным", spec: "§6.4, §11.3", files: "tests/integration/cancel_tests.cpp", verify: "tools\\build.bat Debug" }
  ]],
  ["W18 E2E", [
    { id: "86", title: "E2E-фикстуры: генератор синтетического мусора заданного объёма и категорий", spec: "§11.3", files: "tests/e2e/New-JunkFixture.ps1", verify: "PowerShell -Command парсинг файла без ошибок" },
    { id: "87", title: "E2E-сценарий полного цикла: скан, план, очистка, отчёт, проверка освобождения", spec: "§11.3", files: "tests/e2e/Test-FullCycle.ps1", verify: "парсинг PowerShell без ошибок" },
    { id: "88", title: "E2E-сценарий отмены и восстановления: 100 процентов файлов возвращены", spec: "§11.3, §12", files: "tests/e2e/Test-UndoRestore.ps1", verify: "парсинг PowerShell без ошибок" },
    { id: "89", title: "E2E-сценарий битых правил: приложение стартует на предыдущем рабочем наборе", spec: "§9.2, ADR-008", files: "tests/e2e/Test-BadRules.ps1", verify: "парсинг PowerShell без ошибок" },
    { id: "90", title: "E2E-сценарий защиты: битые секторы, занятые файлы, зашифрованный том, отсутствие прав", spec: "§11.3", files: "tests/e2e/Test-EdgeCases.ps1", verify: "парсинг PowerShell без ошибок" }
  ]],
  ["W19 Ворота: безопасность", [
    { id: "91", title: "Адверсное ревью удаления: может ли кандидат выйти за корень правила? Ищи обходы", spec: "§10, §12", files: "ревью без правок кода, отчёт в docs/review-01.md", verify: "docs/review-01.md содержит вердикт по каждой находке" },
    { id: "92", title: "Адверсное ревью правил: может ли правило удалить пользовательские данные?", spec: "§10, §4 FR-4", files: "ревью, отчёт docs/review-02.md", verify: "docs/review-02.md существует" },
    { id: "93", title: "Ревью Win32-обёрток: утечки HANDLE, неверные коды ошибок, таймауты", spec: "§9.1, §10", files: "отчёт docs/review-03.md", verify: "docs/review-03.md существует" },
    { id: "94", title: "Ревью потоков и отмены: гонки, публикация результатов, отсутствие блокировок в UI", spec: "§6.4", files: "отчёт docs/review-04.md", verify: "docs/review-04.md существует" },
    { id: "95", title: "Ревью UX безопасности: ни один элемент не удаляется без объяснения и уровня риска", spec: "§2 G4, §12", files: "отчёт docs/review-05.md", verify: "docs/review-05.md существует" }
  ]],
  ["W20 Ворота: приёмка", [
    { id: "96", title: "Свести находки ревью в список дефектов с приоритетом и владельцем файла", spec: "§12", files: "docs/defects.md", verify: "docs/defects.md существует и не пуст" },
    { id: "97", title: "Починить дефекты безопасности из docs/defects.md с приоритетом critical и high", spec: "§12", files: "только файлы, перечисленные в docs/defects.md для critical/high", verify: "tools\\build.bat Debug && tools\\test.bat Debug" },
    { id: "98", title: "Полная сборка Release и прогон всех тестов, отчёт с числами", spec: "§12", files: "docs/build-report.md", verify: "tools\\build.bat Release && tools\\test.bat Release" },
    { id: "99", title: "Чек-лист DoD из SPEC §12: пройти пункт за пунктом, честно отметить невыполненное", spec: "§12", files: "docs/dod-checklist.md", verify: "docs/dod-checklist.md покрывает все пункты §12" },
    { id: "100", title: "Итоговый отчёт команды: что сделано, что проверено, что осталось и почему", spec: "§8, §12", files: "docs/team-report.md", verify: "docs/team-report.md существует, содержит сводку RVI по волнам" }
  ]]
];

const PREAMBLE = [
  "Ты — субагент команды MrProper. Проект: C++20, Windows 10/11 x64, десктоп-утилита",
  "очистки диска. Репозиторий: D:\\Project\\MrProper (из WSL: /mnt/d/Project/MrProper).",
  "Спецификация: docs/SPEC.md — прочитай свои разделы ДО написания кода, не выдумывай API.",
  "ВНИМАНИЕ: два предыдущих прогона оборвались (OOM, затем обрыв heartbeat на длинных",
  "агентах). В дереве может лежать недописанный код от оборванных агентов — в первую очередь",
  "src/engine/* и src/cli/*. Первая работа — добиться, чтобы проект собирался в твоём слоте.",
  "Чужие файлы не трогай, свои чини. Задачи 01-55 и 67-70 закрыты — их не переписывай.",
  "Она уже есть в репозитории; если файла нет — напиши это в RVI и работай по описанию задачи.",
  "",
  "ЖЁСТКИЕ ПРАВИЛА:",
  "1. Владеешь ТОЛЬКО перечисленными файлами. Чужие файлы трогать нельзя — их пишут параллельно.",
  "2. Никогда не редактируй docs/SPEC.md, CMakeLists.txt верхнего уровня, tools/build.bat,",
  "   tools/test.bat — иначе сломаешь чужие задачи и общий тулчейн.",
  "3. Сборка и тесты — ТОЛЬКО через tools\\build.bat <Debug|Release> a<твоя задача> и",
   "   tools\\test.bat <Debug|Release> a<твоя задача>. Слот (второй аргумент) ОБЯЗАТЕЛЕН.",
   "   Пример для задачи 63: tools\\build.bat Debug a63. Без слота будет общий build\\main.",
   "   Компиляция сериализована замком, ожидание до 12 минут: это нормально, не перезапускай.",
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

const maxTasks = args && args.maxTasks ? args.maxTasks : 41;
// Размер волны: 5 одновременных агентов роняют хост (host-daemon теряет heartbeat,
// агенты обрываются на 25-30 минутах), поэтому по умолчанию 2.
const concurrency = args && args.concurrency ? args.concurrency : 2;
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
  for (let i = 0; i < tasks.length; i += concurrency) {
    const chunk = tasks.slice(i, i + concurrency);
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
