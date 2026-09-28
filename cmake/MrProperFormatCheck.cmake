# ---------------------------------------------------------------------------
# Проверка формата для clang-format старше 10 (нет --dry-run --Werror).
# Запускается как cmake -P, то есть без проекта и без компилятора:
#   cmake -DCLANG_FORMAT_EXE=<путь> -DSOURCE_LIST=<a;b;c> -P MrProperFormatCheck.cmake
#
# Логика: clang-format --output-replacements-xml печатает XML, в котором
# <replacement …/> — это правка, которую формат предлагает внести. Ноль
# правок означает «файл уже отформатирован». Наличие хотя бы одной правки
# означает нарушение формата, и скрипт завершается с ненулевым кодом:
# ворота обязаны уметь падать, иначе это не ворота.
# ---------------------------------------------------------------------------

if(NOT CLANG_FORMAT_EXE)
    message(FATAL_ERROR "CLANG_FORMAT_EXE не задан: нечем проверять формат")
endif()
if(NOT EXISTS "${CLANG_FORMAT_EXE}")
    message(FATAL_ERROR "clang-format не найден: ${CLANG_FORMAT_EXE}")
endif()

set(_bad_files "")
set(_checked 0)

foreach(_file IN LISTS SOURCE_LIST)
    if(NOT EXISTS "${_file}")
        continue()
    endif()
    execute_process(
        COMMAND "${CLANG_FORMAT_EXE}" --output-replacements-xml "${_file}"
        OUTPUT_VARIABLE _xml
        ERROR_VARIABLE  _err
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "clang-format упал на «${_file}» (код ${_rc}):\n${_err}")
    endif()
    math(EXPR _checked "${_checked} + 1")
    # Считаем открывающие теги <replacement: они есть только когда
    # формат предлагает правку, то есть ровно в том случае, когда файл
    # отформатирован неверно.
    string(REGEX MATCHALL "<replacement " _replacements "${_xml}")
    list(LENGTH _replacements _count)
    if(_count GREATER 0)
        list(APPEND _bad_files "${_file}")
    endif()
endforeach()

if(_bad_files)
    string(REPLACE ";" "\n  " _bad_text "${_bad_files}")
    message(FATAL_ERROR
        "Формат нарушен в ${_checked} проверенных файлах:\n  ${_bad_text}\n"
        "Исправить: cmake --build <каталог> --target mrproper-format")
endif()

message(STATUS "Формат в порядке: ${_checked} файлов")
