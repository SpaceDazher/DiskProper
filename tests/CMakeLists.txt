# ---------------------------------------------------------------------------
# Тесты MrProper. Спека: §9 (tests/unit, tests/integration, tests/e2e),
# §8 Этап 0 (зелёный ctest), §6.1 (ядро тестируется на любом хосте).
#
#   unit        — переносимое ядро, собирается на любом хосте (W06, W16)
#   integration — Win32-слой против реальных дисков (W17)
#   e2e         — сценарии на Pester (W18): цели CMake не имеют, каждый
#                 tests\e2e\Test-*.ps1 регистрируется здесь как отдельная
#                 проверка CTest (метка e2e), а запускает их один и тот же
#                 вызов и локальный прогон, и CI (job e2e)
#
# Файл владеет общей обвязкой наборов: enable_testing(), интерфейсной целью
# mrproper_test_harness, подключением каталогов наборов и регистрацией
# e2e-сценариев. Списки тестовых файлов наборов остаются в каталогах наборов —
# у каждого набора своя цель.
# ---------------------------------------------------------------------------

# Опция верхнего уровня (cmake/MrProperOptions.cmake). Корреня достаточно,
# чтобы он подключил tests/ не создавая целей при выключенных тестах, но
# проверка здесь делает файл корректным и при безусловном add_subdirectory.
# Если опция не объявлена (подключение каталога отдельно) — тесты собираются:
# молча пропустить их по незнанию настройки хуже, чем собрать лишнее.
if(DEFINED MRPROPER_BUILD_TESTS AND NOT MRPROPER_BUILD_TESTS)
    message(STATUS "[MrProper] MRPROPER_BUILD_TESTS=OFF: тесты не собираются")
    return()
endif()

# Допустимо и в подкаталоге: включает реестр тестов для tests/ и всех
# вложенных наборов. Верхний уровень тоже вызывает enable_testing() —
# повторный вызов безвреден.
enable_testing()

# Общий тест-харнесс tests/harness.hpp — свой, без внешних зависимостей:
# подключение vcpkg/Catch2 в Этапе 0 сознательно отложено (ADR-001), а ядро
# должно проверяться прямо здесь, без сети. INTERFACE-цель: харнесс
# компилируется вместе с тестами, отдельных объектов у него нет.
if(NOT TARGET mrproper_test_harness)
    add_library(mrproper_test_harness INTERFACE)
    target_include_directories(mrproper_test_harness INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(mrproper_test_harness INTERFACE cxx_std_20)
endif()

# Наборы подключаются по мере появления их CMakeLists.txt: жёсткий
# add_subdirectory на несуществующий каталог уронил бы конфигурацию уже сегодня.
# Порядок в списке — порядок подключения; unit идёт первым, потому что он не
# зависит ни от чего, кроме ядра.
set(MRPROPER_TEST_SUITES "")
foreach(MRPROPER_TEST_SUITE unit integration)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${MRPROPER_TEST_SUITE}/CMakeLists.txt")
        add_subdirectory(${MRPROPER_TEST_SUITE})
        list(APPEND MRPROPER_TEST_SUITES ${MRPROPER_TEST_SUITE})
    endif()
endforeach()
string(REPLACE ";" ", " _mrproper_test_suites_text "${MRPROPER_TEST_SUITES}")
message(STATUS "[MrProper] тесты: цели наборов [${_mrproper_test_suites_text}]")

# ---------------------------------------------------------------------------
# Набор e2e: сценарии на Pester (SPEC §11 п.3, §12.3, §12.7, §12.8, §12.11)
# ---------------------------------------------------------------------------
#
# Пробел, который эта регистрация закрывает: сценарии лежали в tests\e2e и не
# были подключены ни к CMake, ни к CI, поэтому «все e2e-сценарии зелёные» и
# «0 крашей на сценариях 2–6» из §12 проверять было нечем — ворота проходили,
# не выполнив ни одного сценария.
#
# Формат: отдельной целью CMake они быть не могут (это .ps1, а не C++), но
# каждая из них — честная отдельная проверка CTest с меткой e2e:
#
#   ctest --test-dir build\main -C Debug -L e2e --output-on-failure
#
# Локально и в CI гоняется один и тот же ctest, а не «дома скрипт, в CI
# другой»: иначе расхождение между «проверено» и «зелёное» снова становится
# возможным, и это ровно то, что ворота обязаны исключать.
#
# Про Pester: сценарии используют конструкции, которых нет в той версии, что
# стоит в Windows PowerShell 5.1 из коробки (3.4.0):
#   * `Describe ... -Tag 'e2e'` — в 3.4 параметр называется $Tags и ключа -Tag
#     не существует, блок падает с ArgumentTransformationMetadataException;
#   * `Should Be` (без дефисов) — в 5.x «legacy syntax is not supported»;
#   * `Set-ItResult -Skipped` — есть начиная с 4.1.
# Проверено на этой машине: 3.4.0 — падает на первом Describe, 5.7.1 — падает
# на первой проверке, 4.10.1 — проходит все применённые в сценариях формы.
# Поэтому ниже стоит явная проверка версии с внятной ошибкой. Подключить
# нужный модуль можно переменной окружения MRPROPER_PESTER_MODULE (путь к
# Pester.psd1 версии 4.x) — так это делает job e2e в .github/workflows/ci.yml.
#
# Ключ -Destructive у сценариев намеренно НЕ передаётся: он разрешает
# apply --execute по кандидатам настоящего скана, а на машине разработчика это
# его реальный кэш, а не мусор теста. На эталонной VM (§12) прогон с этим
# ключом делают руками, отдельным решением, — и он не имеет права
# включаться сам собой ни локально, ни в CI.
#
# Каталог не имеет своего CMakeLists.txt: набор e2e не имеет целей, и
# заводить ради него файл с одним списком значило бы прятать перечень
# сценариев от tools\build.bat и от обзора каталога. Список берётся глобом,
# поэтому новый Test-*.ps1 попадает в ворота сам, без правки этого файла.

# Вне Windows сценариев не существует: Pester и PowerShell 5.1 — часть
# Windows-образа, и на хосте без него регистрировать нечего. Молчаливый
# пропуск здесь означал бы «e2e проверены» на машине, где они не запускались.
if(NOT WIN32)
    message(STATUS
        "[MrProper] tests/e2e: хост не Windows — сценарии Pester не регистрируются "
        "(§11 п.3 требует Windows-образ)")
    return()
endif()

# Своё выключатель есть, потому что сценарии идут в минуты и требуют Pester
# 4.x: агент, которому нужен только быстрый юнит-прогон, отключает их явно
# (cmake -DMRPROPER_BUILD_E2E=OFF ...), а не молча получает красный ctest.
option(MRPROPER_BUILD_E2E "Регистрировать e2e-сценарии (tests/e2e) в CTest" ON)

set(MRPROPER_E2E_DIR "${CMAKE_CURRENT_SOURCE_DIR}/e2e")
if(NOT MRPROPER_BUILD_E2E)
    message(STATUS "[MrProper] tests/e2e: MRPROPER_BUILD_E2E=OFF — сценарии не регистрируются")
    return()
endif()

# Windows PowerShell 5.1, а не pwsh: сценарии объявляют #Requires -Version 5.1
# и рассчитаны на inbox-модули этой версии. Ищем в System32 явно, чтобы на
# машине без PowerShell шаг был понятным отказом, а не «тест прошёл».
find_program(MRPROPER_POWERSHELL_EXE
    NAMES powershell.exe
    HINTS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0"
    NO_DEFAULT_PATH
)
if(NOT MRPROPER_POWERSHELL_EXE)
    find_program(MRPROPER_POWERSHELL_EXE NAMES powershell.exe)
endif()
if(NOT MRPROPER_POWERSHELL_EXE)
    message(WARNING
        "[MrProper] tests/e2e: не найден powershell.exe — сценарии не регистрируются. "
        "Проверка §12.3 на этой машине невозможна, а не «зелёная».")
    return()
endif()

file(GLOB MRPROPER_E2E_SCENARIOS CONFIGURE_DEPENDS "${MRPROPER_E2E_DIR}/Test-*.ps1")
list(SORT MRPROPER_E2E_SCENARIOS)
if(NOT MRPROPER_E2E_SCENARIOS)
    message(WARNING
        "[MrProper] tests/e2e: в ${MRPROPER_E2E_DIR} нет ни одного Test-*.ps1 — "
        "зарегистрировать нечего.")
    return()
endif()

# Путь к CLI передаём сценарию явно (внутри набора есть поиск по слотам, но он
# перебирает build\ и может взять чужую сборку). Генераторное выражение нужно
# только при существующей цели: подключение tests/ без src/cli в конфигурации
# не должно ронять генерацию на неизвестном целевом файле.
if(TARGET mrproper_cli)
    set(_mrproper_e2e_cli "$<TARGET_FILE:mrproper_cli>")
else()
    set(_mrproper_e2e_cli "")
endif()

# Сценарию — ровно тест CTest. Имя из имени файла: Test-FullCycle.ps1 даёт
# e2e_Test_FullCycle, Test-UndoRestore.ps1 — e2e_Test_UndoRestore. Метка e2e
# отделяет сценарии от юнит- и интеграционных проверок, поэтому обычный
# «быстрый» прогон остаётся `ctest -LE e2e`, а полный — `ctest`.
set(_mrproper_e2e_names "")
foreach(_mrproper_e2e_scenario IN LISTS MRPROPER_E2E_SCENARIOS)
    get_filename_component(_mrproper_e2e_stem "${_mrproper_e2e_scenario}" NAME_WE)
    # Точка перед Test убирается: e2e_Test_FullCycle читается лучше, чем
    # e2e_.Test_FullCycle, а имя теста — это то, что человек видит в отчёте.
    string(REGEX REPLACE "^Test[-.]" "" _mrproper_e2e_name "${_mrproper_e2e_stem}")
    set(_mrproper_e2e_test "e2e_${_mrproper_e2e_name}")

    # Команда целиком на PowerShell. Одинарные кавычки в строках — чтобы не
    # воевать с кавычками CMake; $ передаётся как есть, CMake переменных $ не
    # подставляет, а генераторное выражение $&lt;CONFIG&gt; подставляется при
    # генерации CTestTestfile.cmake.
    #
    # -EnableExit заканчивает процесс кодом числа проваленных проверок: без
    # него CTest увидел бы «процесс завершился успешно» после красного
    # прогона, то есть ворота прошли бы, ничего не проверив.
    set(_mrproper_e2e_command
        "$ErrorActionPreference = 'Stop'; "
        "if ($env:MRPROPER_PESTER_MODULE) { "
        "Import-Module -Name $env:MRPROPER_PESTER_MODULE -Force -ErrorAction Stop "
        "} else { "
        "Import-Module -Name Pester -Force -ErrorAction Stop "
        "}; "
        "$mrproperPesterVersion = (Get-Module -Name Pester).Version; "
        "if ($mrproperPesterVersion.Major -ne 4) { "
        "throw ('Сценарии tests/e2e требуют Pester 4.x: в этой машине Pester ' + $mrproperPesterVersion + '. "
        "Встроенный в Windows PowerShell 5.1 Pester 3.4.0 не знает ключа -Tag у Describe, "
        "а Pester 5 не понимает Should Be. Укажите модуль 4.x через переменную окружения "
        "MRPROPER_PESTER_MODULE (путь к Pester.psd1).') "
        "}; "
        "$mrproperScenarioParameters = @{ Configuration = '$<CONFIG>' }; "
        "$mrproperCli = '${_mrproper_e2e_cli}'; "
        "if ($mrproperCli -and $mrproperCli -ne '') { $mrproperScenarioParameters['CliPath'] = $mrproperCli }; "
        "Invoke-Pester -Script @{ Path = '${_mrproper_e2e_scenario}'; "
        "Parameters = $mrproperScenarioParameters } -EnableExit"
    )

    add_test(NAME "${_mrproper_e2e_test}" COMMAND "${MRPROPER_POWERSHELL_EXE}"
        -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "${_mrproper_e2e_command}")
    set_tests_properties("${_mrproper_e2e_test}" PROPERTIES
        LABELS "e2e"
        # Сценарии пишут отчёты о пропусках в build\e2e-artifacts (общий
        # каталог), поэтому параллельный прогон затирал бы журналы соседей.
        RUN_SERIAL TRUE
        RESOURCE_LOCK mrproper_e2e
        # Порог по времени — страховка от «сценарий висит вечно»: ctest по
        # умолчанию ждёт бесконечно, и зависший CI job стоит дороже красного.
        TIMEOUT 2700
    )
    list(APPEND _mrproper_e2e_names "${_mrproper_e2e_test}")
endforeach()

string(REPLACE ";" ", " _mrproper_e2e_text "${_mrproper_e2e_names}")
message(STATUS
    "[MrProper] tests/e2e: зарегистрировано ${_mrproper_e2e_text} (метка e2e, "
    "PowerShell ${MRPROPER_POWERSHELL_EXE})")
