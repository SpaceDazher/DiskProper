cmake_minimum_required(VERSION 3.20)
project(MrProper VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Ядро проекта: переносимое, без Windows. Именно его можно собирать и тестировать
# на любом хосте (в том числе из WSL через g++) — SPEC §6.1, ADR-004.
# ---------------------------------------------------------------------------
add_library(mrproper_core STATIC
    src/core/json.cpp
    src/core/glob.cpp
    src/core/units.cpp
    src/core/rules.cpp
    src/core/scoring.cpp
)
# Публичные каталоги включений: src — для «core/…», src/core — для «…» (как в g++ -Isrc/core).
target_include_directories(mrproper_core PUBLIC src src/core)
target_compile_features(mrproper_core PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# Тесты ядра: собираются везде, где есть компилятор C++17/20.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(mrproper_unit_tests tests/unit/core_tests.cpp)
target_link_libraries(mrproper_unit_tests PRIVATE mrproper_core)
target_include_directories(mrproper_unit_tests PRIVATE tests)
add_test(NAME core_unit COMMAND mrproper_unit_tests)

# ---------------------------------------------------------------------------
# Предупреждения как ошибки — но только там, где это реально поддерживается.
# MSVC: /W4 /WX. GCC/Clang: -Wall -Wextra -Wpedantic -Werror.
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(mrproper_core PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_options(mrproper_unit_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    target_compile_options(mrproper_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(mrproper_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
