# ---------------------------------------------------------------------------
# Форматирование и статический анализ как цели CMake (SPEC §8, Этап 0:
# «clang-format + clang-tidy в CI», §9.1).
# Владелец: W01/задача 01.
#
# Почему это CMake, а не только CI. .clang-format и .clang-tidy (W01, задача 03)
# читают любой инструмент, но «прогнать» их должен кто-то: CI это делает
# шагом пайплайна, а локально — команда, которую можно набрать руками до
# коммита. Здесь объявляются три цели:
#
#   mrproper-format          clang-format -i по исходникам проекта
#   mrproper-format-check    то же в режиме проверки: ненулевой код возврата
#                            означает «формат нарушен» (ворота для CI)
#   mrproper-tidy            clang-tidy по compile_commands.json
#
# Обе проверки объявлены с EXCLUDE_FROM_ALL: обычная сборка, а значит и
# tools\build.bat, их никогда не запускает. Цель без инструмента падает с
# объяснением, а не молча ничего не делает: ворота, которые не могут
# провалиться, ничем не отличаются от отсутствующих.
#
# Требования к инструментам:
#   clang-format >= 10  — ради --dry-run --Werror в mrproper-format-check
#   clang-tidy          — любой, поддерживающий -p <каталог с compile_commands.json>
#
# ВАЖНО: эти цели не дублируют .clang-format и .clang-tidy и не имеют права
# их менять. Формат задаёт задача 03, здесь только вызов.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

find_program(MRPROPER_CLANG_FORMAT NAMES clang-format
    DOC "clang-format для целей mrproper-format / mrproper-format-check")
find_program(MRPROPER_CLANG_TIDY NAMES clang-tidy
    DOC "clang-tidy для цели mrproper-tidy")

# Версия нужна, чтобы не предлагать --dry-run, которого нет в clang-format 9 и
# старше: там ключ трактуется как имя файла, и проверка молча проходит.
set(MRPROPER_CLANG_FORMAT_VERSION "")
if(MRPROPER_CLANG_FORMAT)
    execute_process(COMMAND "${MRPROPER_CLANG_FORMAT}" --version
        OUTPUT_VARIABLE _fmt_version_output
        ERROR_VARIABLE  _fmt_version_error
        RESULT_VARIABLE _fmt_version_code
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_fmt_version_code EQUAL 0)
        # «clang-format version 17.0.6»
        string(REGEX MATCH "[0-9]+(\\.[0-9]+)*" _fmt_version "${_fmt_version_output}")
        set(MRPROPER_CLANG_FORMAT_VERSION "${_fmt_version}")
    endif()
endif()

# Файлы берутся каталогом, как и везде в проекте: каждый агент владеет своим
# .cpp/.hpp и не должен дописывать его в чужой список. CONFIGURE_DEPENDS здесь
# не нужен: списком пользуются только цели проверки, запускаемые руками, и
# лишняя перепроверка каталога на каждой сборке ничего не добавляет.
file(GLOB_RECURSE MRPROPER_STYLE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
)
list(SORT MRPROPER_STYLE_SOURCES)

# Формат версионируется числом, чтобы сравнение не зависело от того, как
# clang-format печатает свою строку.
set(MRPROPER_CLANG_FORMAT_HAS_DRY_RUN FALSE)
if(MRPROPER_CLANG_FORMAT_VERSION AND NOT MRPROPER_CLANG_FORMAT_VERSION VERSION_LESS "10")
    set(MRPROPER_CLANG_FORMAT_HAS_DRY_RUN TRUE)
endif()

# --- mrproper-format --------------------------------------------------------
if(MRPROPER_CLANG_FORMAT)
    add_custom_target(mrproper-format
        COMMAND "${MRPROPER_CLANG_FORMAT}" -i ${MRPROPER_STYLE_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "clang-format -i: ${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM
        USES_TERMINAL)
    # Порог, при котором формат считается нарушенным, а не подсказкой:
    # -Werror есть только в clang-format >= 10, иначе проверка заменяется на
    # подсчёт замен — иначе она всегда «зелёная».
    if(MRPROPER_CLANG_FORMAT_HAS_DRY_RUN)
        add_custom_target(mrproper-format-check
            COMMAND "${MRPROPER_CLANG_FORMAT}" --dry-run --Werror
                    ${MRPROPER_STYLE_SOURCES}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "clang-format --dry-run --Werror: формат должен совпадать"
            VERBATIM
            USES_TERMINAL)
    else()
        add_custom_target(mrproper-format-check
            COMMAND "${CMAKE_COMMAND}"
                    -DCLANG_FORMAT_EXE=${MRPROPER_CLANG_FORMAT}
                    -DSOURCE_LIST=${MRPROPER_STYLE_SOURCES}
                    -P "${CMAKE_CURRENT_LIST_DIR}/MrProperFormatCheck.cmake"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "clang-format ${MRPROPER_CLANG_FORMAT_VERSION}: проверка через --output-replacements-xml"
            VERBATIM
            USES_TERMINAL)
    endif()
else()
    add_custom_target(mrproper-format
        COMMAND "${CMAKE_COMMAND}" -E echo
            "[MrProper] clang-format не найден в PATH. Формат задаёт .clang-format (SPEC §9.1); поставить: winget install LLVM.LLVM"
        VERBATIM)
    add_custom_target(mrproper-format-check
        COMMAND "${CMAKE_COMMAND}" -E echo
            "[MrProper] clang-format не найден в PATH — ворота формата не могут быть применены. Это ошибка конфигурации CI, а не «всё в порядке»."
        VERBATIM)
endif()

# --- mrproper-tidy ----------------------------------------------------------
# База компиляции — отдельный каталог: генератор Visual Studio не пишет
# compile_commands.json (CMake 3.20 умеет его только для Makefiles и Ninja),
# поэтому tidy всегда смотрит в отдельное дерево, созданное пресетом
# ninja-* или шагом clang-tidy в CI (build/tidy).
set(MRPROPER_COMPILE_COMMANDS "" CACHE PATH
    "Каталог с compile_commands.json для цели mrproper-tidy; пусто = найти автоматически")

function(_mrproper_find_compile_commands out)
    if(MRPROPER_COMPILE_COMMANDS AND EXISTS "${MRPROPER_COMPILE_COMMANDS}/compile_commands.json")
        set(${out} "${MRPROPER_COMPILE_COMMANDS}" PARENT_SCOPE)
        return()
    endif()
    file(GLOB _candidates
        "${CMAKE_CURRENT_SOURCE_DIR}/build/*/compile_commands.json"
        "${CMAKE_BINARY_DIR}/compile_commands.json")
    foreach(_db IN LISTS _candidates)
        get_filename_component(_dir "${_db}" DIRECTORY)
        set(${out} "${_dir}" PARENT_SCOPE)
        return()
    endforeach()
    set(${out} "" PARENT_SCOPE)
endfunction()

_mrproper_find_compile_commands(_mrproper_cc_dir)
set(MRPROPER_COMPILE_COMMANDS_DIR "${_mrproper_cc_dir}" CACHE INTERNAL
    "Найденный каталог с compile_commands.json (заполняется на configure)")

if(MRPROPER_CLANG_TIDY)
    if(MRPROPER_COMPILE_COMMANDS_DIR)
        add_custom_target(mrproper-tidy
            COMMAND "${MRPROPER_CLANG_TIDY}" -p "${MRPROPER_COMPILE_COMMANDS_DIR}"
                    ${MRPROPER_STYLE_SOURCES}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "clang-tidy по ${MRPROPER_COMPILE_COMMANDS_DIR}/compile_commands.json"
            VERBATIM
            USES_TERMINAL)
    else()
        add_custom_target(mrproper-tidy
            COMMAND "${CMAKE_COMMAND}" -E echo
                "[MrProper] compile_commands.json не найден — clang-tidy нечего проверять. Создать базу: cmake --preset ninja-debug (нужен Developer Command Prompt) либо -DMRPROPER_COMPILE_COMMANDS=<каталог>"
            VERBATIM)
    endif()
else()
    add_custom_target(mrproper-tidy
        COMMAND "${CMAKE_COMMAND}" -E echo
            "[MrProper] clang-tidy не найден в PATH. Правила анализа задаёт .clang-tidy (SPEC §9.1); поставить: winget install LLVM.LLVM"
        VERBATIM)
endif()
