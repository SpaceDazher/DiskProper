# cmake/warnings.cmake — профили предупреждений и интерфейсная цель
# (SPEC §9.1, ADR-001: митигация риска C++20 + Win32 — /W4 /WX, изоляция
# WinAPI в platform/, RAII-обёртки).
#
# Правило проекта: ни одна цель не пишет /W4, /WX, -Wall, -Werror руками.
# Подключение модуля ничего не меняет само по себе — он только определяет
# функции и списки флагов, поэтому его безопасно include() из любого
# подкаталога и разбирать через `cmake -P` (проверка синтаксиса).
#
#   include(cmake/warnings.cmake)
#
#   mrproper_enable_warnings(mrproper_core)              # переносимое ядро
#   mrproper_enable_warnings(mrproper_core STRICT)       # + строгий профиль
#   mrproper_enable_warnings(mrproper_platform WIN32)   # слой Win32
#   mrproper_enable_warnings(mrproper_ui WIN32 STRICT)
#
#   # много слоёв (core/engine/platform/ui/...): один раз объявить интерфейс,
#   # дальше линковать его в цель
#   mrproper_warnings_library(mrproper_warnings WIN32)
#   target_link_libraries(mrproper_platform PRIVATE mrproper_warnings)
#
# Профили:
#   WIN32    — определения Win32-слоя (WIN32_LEAN_AND_MEAN, NOMINMAX,
#              _WIN32_WINNT=0x0A00) и снятый C4996. Целевая ОС — Windows 10/11
#              x64 (SPEC §5), поэтому API, помеченный устаревшим именно из-за
#              Win10/11, отключать нельзя. C4996 означает «помечено устаревшим
#              в документации» — это повод для ревью, а не повод валить сборку
#              всего приложения; выбор конкретной функции ревьюится вручную
#              (docs/review-03.md, волна W19).
#   STRICT   — дополнительные проверки сверх базового набора. Включается
#              поимённо либо глобально через -DMRPROPER_STRICT_WARNINGS=ON.
#              Строгий профиль ломает сборку честного, но шумного кода, и это
#              должно быть решение владельца файла, а не сюрприз в CI.
#   NO_ERRORS — локальное отключение /WX и -Werror для отладки. В CI запрещено:
#              CI всегда собирает с MRPROPER_WARNINGS_AS_ERRORS=ON.
#
# Соседние модули (не дублируются этим файлом):
#   cmake/MrProperOptions.cmake   — владеет опциями MRPROPER_* (задача 01);
#   cmake/MrProperWarnings.cmake  — базовый набор /W4 /WX и список /w44xxx для
#     строгого режима MSVC, объявляет MRPROPER_WARNINGS_READY. Этот модуль его
#     не переопределяет: базовые флаги совпадают по значению (повтор одного и
#     того же ключа MSVC ошибкой не является), а STRICT здесь добавляет только
#     то, чего там нет.
#
# ВАЖНО: не задавайте уровень предупреждений вручную (/W1, /W3) — MSVC выдаст
# D9025 «overriding /W3 with /W4», а вместе с /WX это уже ошибка сборки.
include_guard(GLOBAL)

# Опции проекта объявлены в cmake/MrProperOptions.cmake. Здесь они только
# читаются; если модуль используется без него, значение создаётся с тем же
# дефолтом, что и в проекте (ON / OFF). set(CACHE) без FORCE не перебивает
# уже объявленный кэш — два владельца одной опции невозможны.
if(NOT DEFINED MRPROPER_WARNINGS_AS_ERRORS)
    set(MRPROPER_WARNINGS_AS_ERRORS ON CACHE BOOL
        "Предупреждения компилятора — ошибка (/WX, -Werror), SPEC §9.1 ADR-001")
endif()
if(NOT DEFINED MRPROPER_STRICT_WARNINGS)
    set(MRPROPER_STRICT_WARNINGS OFF CACHE BOOL
        "Строгий набор предупреждений сверх /W4 (MSVC: /w44xxx; GCC/Clang: -Wshadow, -Wconversion)")
endif()
option(MRPROPER_ENABLE_ANALYZE "MSVC /analyze (PREfast) — медленно и строго" OFF)
set(MRPROPER_MSVC_WARNING_LEVEL "4" CACHE STRING "Уровень предупреждений MSVC (/W)")

# --- Базовые списки ---------------------------------------------------------
# Базовый набор MSVC совпадает с тем, что уже собрано в CI, и не содержит
# ничего, что могло бы перебить чужой флаг: у /W4, /permissive-, /utf-8 и
# /Zc:_* нет конфликтующей пары.
set(MRPROPER_WARNINGS_MSVC_BASE
    /W${MRPROPER_MSVC_WARNING_LEVEL}
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /Zc:preprocessor
)

set(MRPROPER_WARNINGS_MSVC_WIN32
    /DWIN32_LEAN_AND_MEAN
    /DNOMINMAX
    /D_WIN32_WINNT=0x0A00
    /wd4996
)

set(MRPROPER_WARNINGS_GCC_BASE
    -Wall
    -Wextra
    -Wpedantic
)

# Только то, чего нет в cmake/MrProperWarnings.cmake: там строгий режим —
# /w44xxx для MSVC и {-Wshadow, -Wconversion, -Wsign-conversion,
# -Wold-style-cast, -Wuseless-cast} для GCC/Clang.
set(MRPROPER_WARNINGS_GCC_STRICT
    -Wnon-virtual-dtor
    -Wcast-align
    -Wdouble-promotion
    -Wformat=2
    -Wnull-dereference
    -Woverloaded-virtual
)

# --- Внутреннее: собрать флаги под компилятор -------------------------------
function(_mrproper_collect_flags out_var win32 strict)
    set(flags "")

    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl: предупреждения GCC-стиля, но с префиксом /clang:-.
        list(APPEND flags /clang:-Wall /clang:-Wextra /clang:-Wpedantic)
        if(MRPROPER_WARNINGS_AS_ERRORS)
            list(APPEND flags /WX)
        endif()
    elseif(MSVC)
        list(APPEND flags ${MRPROPER_WARNINGS_MSVC_BASE})
        if(MRPROPER_WARNINGS_AS_ERRORS)
            list(APPEND flags /WX)
        endif()
        if(MRPROPER_ENABLE_ANALYZE)
            list(APPEND flags /analyze)
        endif()
        # Для MSVC базовый набор /W4 /WX уже является политикой проекта, а
        # список /w44xxx строгого режима держит cmake/MrProperWarnings.cmake —
        # здесь он намеренно не повторяется, чтобы не было двух правд.
        if(win32)
            list(APPEND flags ${MRPROPER_WARNINGS_MSVC_WIN32})
        endif()
    else()
        list(APPEND flags ${MRPROPER_WARNINGS_GCC_BASE})
        if(MRPROPER_WARNINGS_AS_ERRORS)
            list(APPEND flags -Werror)
        endif()
        if(strict)
            list(APPEND flags ${MRPROPER_WARNINGS_GCC_STRICT})
            if(NOT win32)
                # C-приведения допустимы в переносимом ядре и запрещены в
                # Win32-слое, где они приходят из API, а не из небрежности.
                list(APPEND flags -Wold-style-cast)
            endif()
        endif()
    endif()

    if(flags)
        list(REMOVE_DUPLICATES flags)
    endif()

    set(${out_var} "${flags}" PARENT_SCOPE)
endfunction()

# --- Основная точка входа ---------------------------------------------------
#
# mrproper_enable_warnings(<target> [WIN32] [STRICT] [NO_ERRORS])
#
# Применяет политику предупреждений к цели. NO_ERRORS ослабляет /WX только
# для этой цели и только при явном вызове; общий дефолт берётся из
# MRPROPER_WARNINGS_AS_ERRORS.
function(mrproper_enable_warnings target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "mrproper_enable_warnings: цель «${target}» не найдена. "
            "Проверьте имя в add_library()/add_executable() до вызова.")
    endif()

    cmake_parse_arguments(W "WIN32;STRICT;NO_ERRORS" "" "" ${ARGN})

    set(strict ${W_STRICT})
    if(NOT strict AND MRPROPER_STRICT_WARNINGS)
        set(strict TRUE)
    endif()

    _mrproper_collect_flags(flags "${W_WIN32}" "${strict}")
    if(W_NO_ERRORS)
        list(REMOVE_ITEM flags /WX -Werror)
    endif()

    if(flags)
        target_compile_options(${target} PRIVATE ${flags})
    endif()

    # Последний применённый набор — чтобы вызывающий напечатал его в лог
    # конфигурации и не гадал, что именно включено.
    set(MRPROPER_WARNINGS_LAST_FLAGS "${flags}" PARENT_SCOPE)
endfunction()

# --- Интерфейсная цель для многомодульного дерева ---------------------------
#
# mrproper_warnings_library(<name> [WIN32] [STRICT] [NO_ERRORS])
#
# Создаёт INTERFACE-цель с теми же флагами: политика живёт в одном месте, а
# подключение в новом слое — одна строка target_link_libraries().
function(mrproper_warnings_library name)
    cmake_parse_arguments(W "WIN32;STRICT;NO_ERRORS" "" "" ${ARGN})

    if(NOT TARGET ${name})
        add_library(${name} INTERFACE)
    endif()

    set(strict ${W_STRICT})
    if(NOT strict AND MRPROPER_STRICT_WARNINGS)
        set(strict TRUE)
    endif()

    _mrproper_collect_flags(flags "${W_WIN32}" "${strict}")
    if(W_NO_ERRORS)
        list(REMOVE_ITEM flags /WX -Werror)
    endif()
    if(flags)
        target_compile_options(${name} INTERFACE ${flags})
    endif()
endfunction()
