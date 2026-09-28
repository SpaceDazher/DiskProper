# ---------------------------------------------------------------------------
# Цель mrproper_unit_tests — юнит-тесты переносимого ядра. Спека: §9
# (tests/unit — «core, не требует Windows»), §8 Этап 0 (зелёный ctest),
# §6.1 (быстрые переносимые тесты ядра).
#
# Тесты Win32-слоя сюда не попадают: им нужен настоящий Windows, они живут в
# tests/integration (SPEC §9). Поэтому цель линкует только ядро и харнесс —
# и остаётся собираемой на хосте без Windows API.
# ---------------------------------------------------------------------------

# Файлы тестов добавляются волнами W06 (rules_manifest_tests, rules_fuzz_tests)
# и W16 (plan, sizing, trash_undo, rulesync, report). Каждый агент владеет
# только своим .cpp, поэтому список берём каталогом: иначе чужой тест молча не
# попадёт в сборку и tools\test.bat покажет «зелёный» результат без него.
# Конвенция набора: main() определён ровно один раз — в core_tests.cpp,
# остальные файлы только регистрируют TEST(...) через harness.hpp.
file(GLOB MRPROPER_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(NOT MRPROPER_UNIT_TEST_SOURCES)
    # Пустой add_executable уронил бы конфигурацию. Набор ещё не создан —
    # сообщаем об этом явно и ждём файлы.
    message(STATUS
        "[MrProper] tests/unit: файлов тестов нет, цель mrproper_unit_tests не объявляется")
elseif(NOT TARGET mrproper_unit_tests)
    add_executable(mrproper_unit_tests ${MRPROPER_UNIT_TEST_SOURCES})
    target_link_libraries(mrproper_unit_tests PRIVATE mrproper_core mrproper_test_harness)
    target_compile_features(mrproper_unit_tests PRIVATE cxx_std_20)

    # Регистрация в CTest. Имя теста — core_unit: оно читается в отчёте
    # ctest и в CI, менять его нужно вместе с этим файлом.
    add_test(NAME core_unit COMMAND mrproper_unit_tests)
endif()

# Контракт с tools\test.bat: скрипт ищет exe по фиксированным путям
# build\<cfg>\mrproper_unit_tests.exe, затем build\<cfg>\<cfg>\... .
# По умолчанию цель из подкаталога попала бы в build\<cfg>\tests\unit\<cfg>\ —
# и скрипт её бы не нашёл. Путь закрепляем здесь; tools\test.bat не
# редактируется (правило владения). Свойство ставится вне проверки TARGET,
# чтобы путь действовал и на цель, объявленную кем-то ещё.
if(TARGET mrproper_unit_tests)
    set_target_properties(mrproper_unit_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
endif()

# Планка предупреждений та же, что у ядра (SPEC §9.1, ADR-001). Флаги придут
# из cmake/MrProperWarnings.cmake или из профилей cmake/warnings.cmake; ручной
# набор — запасной путь для подключения каталога без верхнего уровня.
if(NOT MRPROPER_WARNINGS_READY AND TARGET mrproper_unit_tests)
    if(COMMAND mrproper_enable_warnings)
        mrproper_enable_warnings(mrproper_unit_tests)
    elseif(MSVC)
        target_compile_options(mrproper_unit_tests PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(mrproper_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

if(MRPROPER_UNIT_TEST_SOURCES)
    list(LENGTH MRPROPER_UNIT_TEST_SOURCES _mrproper_unit_test_count)
    message(STATUS
        "[MrProper] цель mrproper_unit_tests: ${_mrproper_unit_test_count} "
        "файлов тестов ядра (хост без Windows)")
endif()
