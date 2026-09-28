# ---------------------------------------------------------------------------
# Цель mrproper_integration_tests — интеграционные тесты Win32-слоя.
# Спека: §9 (tests/integration — «требует Windows»), §11.2 (обёртки WinAPI против
# реальных путей: длинные пути; reparse-петля; заблокированный файл),
# §6.1 (ядро тестируется на любом хосте, Win32-слой — только здесь).
#
# Отличие от tests/unit принципиально: эта цель линкует mrproper_platform, то
# есть Windows API, и запускается только на Windows. Юнит-тесты этого не могут
# себе позволить (ADR-004: «Core без Windows-зависимостей — ради тестируемости
# на хостовой машине»), поэтому проверки, которым нужен настоящий том,
# оказались бы непроверяемыми.
#
# Состояние набора. Каталог и эта цель появляются волной W17 (задачи 81-85), и
# каждый агент волны владеет ровно одним .cpp. Список файлов поэтому берётся
# каталогом, а не перечислением: иначе чужой тест молча не попал бы в сборку,
# и tools\build.bat показал бы «зелёный» результат без него. Та же причина, по
# которой tests/unit/CMakeLists.txt так не делает.
# ---------------------------------------------------------------------------

file(GLOB MRPROPER_INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(NOT MRPROPER_INTEGRATION_TEST_SOURCES)
    # Пустой add_executable уронил бы конфигурацию. Набор ещё не создан —
    # сообщаем об этом явно и ждём файлы.
    message(STATUS
        "[MrProper] tests/integration: файлов тестов нет, цель mrproper_integration_tests не объявляется")
    return()
endif()

list(SORT MRPROPER_INTEGRATION_TEST_SOURCES)

# ---------------------------------------------------------------------------
# Кто определяет main()
# ---------------------------------------------------------------------------
#
# Конвенция набора (та же, что в tests/unit): main() определён ровно один раз,
# остальные файлы только регистрируют TEST(...) через harness.hpp. Каждый агент
# волны пишет свой .cpp, и полагаться на то, что «кто-то другой уже написал
# main», нельзя — иначе набор не собирается, пока не пришёл нужный агент.
#
# Поэтому main ищется по тексту файлов. Если его нет ни в одном, точку входа
# генерируем здесь (она тривиальна и не принадлежит никому из агентов). Если он
# есть в нескольких — оставляем один, а у остальных переименовываем main через
# COMPILE_DEFINITIONS: файл продолжает компилироваться и его TEST(...) остаются
# в реестре, а символ не двоится. Обойтись без этого нельзя — два main() в
# одной программе дают ошибку линковки и роняют сборку всем сразу.
set(MRPROPER_INTEGRATION_MAIN_SOURCES "")
set(MRPROPER_INTEGRATION_SHADOWED_SOURCES "")
foreach(_mrproper_integration_file IN LISTS MRPROPER_INTEGRATION_TEST_SOURCES)
    file(READ "${_mrproper_integration_file}" _mrproper_integration_text)
    string(FIND "${_mrproper_integration_text}" "int main(" _mrproper_integration_main_pos)
    if(_mrproper_integration_main_pos GREATER_EQUAL 0)
        list(APPEND MRPROPER_INTEGRATION_MAIN_SOURCES "${_mrproper_integration_file}")
    endif()
endforeach()

if(MRPROPER_INTEGRATION_MAIN_SOURCES)
    list(GET MRPROPER_INTEGRATION_MAIN_SOURCES 0 _mrproper_integration_first_main)
    list(REMOVE_ITEM MRPROPER_INTEGRATION_MAIN_SOURCES "${_mrproper_integration_first_main}")
    set(MRPROPER_INTEGRATION_RUNNER_SOURCE "${_mrproper_integration_first_main}")
    list(LENGTH MRPROPER_INTEGRATION_MAIN_SOURCES _mrproper_integration_extra_mains)
    if(_mrproper_integration_extra_mains GREATER 0)
        set(MRPROPER_INTEGRATION_SHADOWED_SOURCES ${MRPROPER_INTEGRATION_MAIN_SOURCES})
        list(LENGTH MRPROPER_INTEGRATION_SHADOWED_SOURCES _mrproper_integration_shadowed_count)
        math(EXPR _mrproper_integration_last "${_mrproper_integration_shadowed_count} - 1")
        foreach(_mrproper_integration_index RANGE 0 ${_mrproper_integration_last})
            list(GET MRPROPER_INTEGRATION_SHADOWED_SOURCES ${_mrproper_integration_index} _mrproper_integration_file)
            set_source_files_properties("${_mrproper_integration_file}" PROPERTIES
                COMPILE_DEFINITIONS "main=mrproper_shadowed_main_${_mrproper_integration_index}")
        endforeach()
    endif()
else()
    set(MRPROPER_INTEGRATION_RUNNER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/mrproper_integration_main.cpp")
    file(WRITE "${MRPROPER_INTEGRATION_RUNNER_SOURCE}"
"// Сгенерировано tests/integration/CMakeLists.txt: точка входа набора.
// Агенты волны владеют каждый своим .cpp и main() не пишут, поэтому он
// появляется здесь — как только в каталоге есть хотя бы один файл.
// Свои записи реестра агент не потеряет: TEST(...) регистрируется на старте.
#include \"harness.hpp\"

int main() { return mrp::runAll(\"MrProper vfs integration\"); }
")
endif()

if(NOT TARGET mrproper_integration_tests)
    add_executable(mrproper_integration_tests
        ${MRPROPER_INTEGRATION_TEST_SOURCES}
        ${MRPROPER_INTEGRATION_RUNNER_SOURCE})
    # Единственная зависимость — Win32-слой; он уже тянет ядро и системные
    # библиотеки (setupapi, ole32, advapi32, shlwapi, winhttp, rstrtmgr).
    target_link_libraries(mrproper_integration_tests PRIVATE mrproper_platform mrproper_test_harness)
    target_compile_features(mrproper_integration_tests PRIVATE cxx_std_20)

    # Регистрация в CTest. Имя теста читается в отчёте ctest и в CI.
    add_test(NAME vfs_integration COMMAND mrproper_integration_tests)
endif()

# Контракт с tools\test.bat тот же, что у юнит-набора: скрипт ищет exe по
# фиксированным путям build\<cfg>\... . По умолчанию цель из подкаталога попала
# бы в build\<cfg>\tests\integration\<cfg>\ — и скрипт её бы не нашёл. Путь
# закрепляем здесь; tools\test.bat не редактируется (правило владения).
# Свойство ставится вне проверки TARGET, чтобы путь действовал и на цель,
# объявленную кем-то ещё.
if(TARGET mrproper_integration_tests)
    set_target_properties(mrproper_integration_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
endif()

# Планка предупреждений приходит из cmake/MrProperWarnings.cmake: она
# раздаётся на уровне каталога верхнего уровня и наследуется всеми
# подкаталогами, а объявленный там MRPROPER_WARNINGS_READY запрещает
# дублировать флаги. Ручной набор — запасной путь для подключения каталога
# без верхнего уровня.
if(NOT MRPROPER_WARNINGS_READY AND TARGET mrproper_integration_tests)
    if(COMMAND mrproper_enable_warnings)
        mrproper_enable_warnings(mrproper_integration_tests WIN32)
    elseif(MSVC)
        target_compile_options(mrproper_integration_tests PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(mrproper_integration_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

list(LENGTH MRPROPER_INTEGRATION_TEST_SOURCES _mrproper_integration_count)
message(STATUS
    "[MrProper] цель mrproper_integration_tests: ${_mrproper_integration_count} "
    "файлов тестов Win32-слоя")
