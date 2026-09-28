# ---------------------------------------------------------------------------
# Тесты MrProper. Спека: §9 (tests/unit, tests/integration, tests/e2e),
# §8 Этап 0 (зелёный ctest), §6.1 (ядро тестируется на любом хосте).
#
#   unit        — переносимое ядро, собирается на любом хосте (W06, W16)
#   integration — Win32-слой против реальных дисков (W17)
#   e2e         — сценарии на PowerShell (W18); отдельной целью CMake не
#                 являются, их запускает CI напрямую
#
# Файл владеет общей обвязкой наборов: enable_testing(), интерфейсной целью
# mrproper_test_harness и подключением каталогов наборов. Списки тестовых
# файлов остаются в каталогах наборов — у каждого набора своя цель.
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
message(STATUS "[MrProper] тесты: подключены наборы [${_mrproper_test_suites_text}]")
