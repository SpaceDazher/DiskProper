# ---------------------------------------------------------------------------
# MrProper — верхний уровень сборки. Владелец: W01/задача 01.
# Спека: SPEC §8 Этап 0 (укрепление системы сборки), §6.1 (слои), §9.1 ADR-001
# (предупреждения как ошибки), §11 (тесты).
#
# Обязанности этого файла ровно три:
#   1) единая база проекта (C++20, экспорт compile_commands.json, /utf-8);
#   2) подключение подкаталогов, у которых есть собственный CMakeLists.txt,
#      и безопасная замена их отсутствия;
#   3) install-цели, CPack и финальный проход по общим флагам.
#
# Списки исходников намеренно НЕ перечисляются: волны W02-W18 добавляют
# модули по одному файлу, и жёсткий список означал бы, что модуль молча не
# попадёт в сборку, пока кто-то не вспомнит дописать строку. Перечень берётся
# каталогом (CONFIGURE_DEPENDS), а tools\build.bat переконфигурирует проект при
# каждом запуске, поэтому новый файл подхватывается без ручных действий.
#
# Проверка — только через tools\build.bat <Debug|Release> и tools\test.bat
# <Debug|Release>: это MSVC из состава Visual Studio, и результат сборки из
# WSL напрямую (cl.exe, g++, cmake) доказательством не считается.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(MrProper
    VERSION 0.1.0
    DESCRIPTION "MrProper — десктоп-утилита очистки диска для Windows 10/11 x64"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 1. Единая база проекта
# ---------------------------------------------------------------------------
# Всё задаётся через «если не задано»: команда -D должна побеждать проект.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
if(NOT DEFINED CMAKE_CXX_STANDARD_REQUIRED)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
if(NOT DEFINED CMAKE_CXX_EXTENSIONS)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()
# Нужен clang-tidy (SPEC §9.1) и IDE: без него статический анализ не запускается.
if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()
# CMake 3.20 выдаёт compile_commands.json только генераторам Makefiles и Ninja,
# а на генераторе Visual Studio опция молча игнорируется: clang-tidy и IDE
# ждут файл, которого не будет. Предупреждение называет рабочую альтернативу.
if(CMAKE_EXPORT_COMPILE_COMMANDS
   AND NOT CMAKE_GENERATOR MATCHES "Ninja"
   AND NOT CMAKE_GENERATOR MATCHES "Makefiles")
    message(WARNING
        "[MrProper] CMAKE_EXPORT_COMPILE_COMMANDS включён, но генератор «${CMAKE_GENERATOR}» "
        "его не поддерживает (CMake 3.20 умеет только Makefiles и Ninja): "
        "compile_commands.json не появится, clang-tidy и IDE запускать нечем. "
        "Рабочий путь — пресет ninja-debug (генератор Ninja из состава Visual Studio)")
endif()

# tools\build.bat передаёт -DCMAKE_BUILD_TYPE, а генератор Visual Studio
# multi-config: конфигурацию выбирает --config, значение CMAKE_BUILD_TYPE на неё
# не влияет. Переменная читается здесь намеренно — иначе CMake сообщает о
# «неиспользованном» параметре командной строки, а разработчик ищет причину.
if(CMAKE_BUILD_TYPE)
    message(STATUS
        "[MrProper] CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} проигнорирован: "
        "генератор ${CMAKE_GENERATOR} выбирает конфигурацию через --config")
endif()

# /utf-8 обязателен для всего проекта: исходники содержат кириллицу в
# комментариях, строковых литералах и сообщениях тестов, а MSVC без этой
# опции выдаёт C4819, который вместе с /WX становится ошибкой сборки.
if(MSVC)
    add_compile_options(/utf-8)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

include(MrProperOptions)     # опции проекта
include(MrProperHelpers)     # общие функции (финальный проход, отчёт)
include(MrProperWarnings)    # /W4 /WX, -Wall -Wextra -Wpedantic, MRPROPER_WARNINGS_READY
include(MrProperSanitizers)  # ASan/UBSan по опции MRPROPER_SANITIZE

# Политика предупреждений в двух частях по разным причинам:
#   cmake/MrProperWarnings.cmake — базовая планка /W4 /WX, раздаётся на уровне
#     каталога, чтобы ни одна цель не осталась без неё, и объявляет
#     MRPROPER_WARNINGS_READY, чтобы подкаталоги не дублировали флаги;
#   cmake/warnings.cmake (W01, задача 03) — именованные профили WIN32/STRICT/
#     NO_ERRORS и интерфейсная цель. Подключается, если файл есть, иначе его
#     функции mrproper_enable_warnings / mrproper_warnings_library были бы видны
#     только самому подкаталогу, который их вызывает.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/warnings.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/warnings.cmake")
endif()

# Общие флаги раздаются на уровне каталога: они наследуются всеми
# подкаталогами, поэтому новый слой (engine, cli, ui) получает ту же планку
# без правки собственного CMakeLists.txt, а объявленный выше
# MRPROPER_WARNINGS_READY заставляет его не дублировать флаги у себя.
mrproper_apply_warnings_to_directory()
mrproper_enable_sanitizers_to_directory()
mrproper_check_toolchain()

set(MRPROPER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MRPROPER_BUILD_TESTS)
    # Верхний уровень включает реестр тестов до подключения tests/, чтобы
    # add_test() работал и в подкаталогах.
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# 2. Слои и тесты
# ---------------------------------------------------------------------------
# Подкаталоги подключаются по мере появления их CMakeLists.txt: жёсткий
# add_subdirectory на несуществующий каталог уронил бы конфигурацию уже сегодня.
# Ядро идёт первым — от него зависят platform, engine, cli, ui и тесты.
file(GLOB MRPROPER_SRC_CMAKE_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "src/*/CMakeLists.txt")
list(SORT MRPROPER_SRC_CMAKE_FILES)
set(MRPROPER_SRC_SUBDIRS "")
foreach(_mrproper_entry IN LISTS MRPROPER_SRC_CMAKE_FILES)
    get_filename_component(_mrproper_dir "${_mrproper_entry}" DIRECTORY)
    list(APPEND MRPROPER_SRC_SUBDIRS "${_mrproper_dir}")
endforeach()
if("src/core" IN_LIST MRPROPER_SRC_SUBDIRS)
    list(REMOVE_ITEM MRPROPER_SRC_SUBDIRS "src/core")
    list(INSERT MRPROPER_SRC_SUBDIRS 0 "src/core")
endif()
foreach(_mrproper_subdir IN LISTS MRPROPER_SRC_SUBDIRS)
    add_subdirectory("${_mrproper_subdir}")
endforeach()

# Страховка на случай, пока src/core/CMakeLists.txt ещё не написан: цель ядра
# объявляется здесь, с тем же контрактом (включающие каталоги, C++20).
if(NOT TARGET mrproper_core)
    file(GLOB MRPROPER_CORE_SOURCES CONFIGURE_DEPENDS "${MRPROPER_SRC_DIR}/core/*.cpp")
    add_library(mrproper_core STATIC ${MRPROPER_CORE_SOURCES})
    target_include_directories(mrproper_core PUBLIC "${MRPROPER_SRC_DIR}" "${MRPROPER_SRC_DIR}/core")
    target_compile_features(mrproper_core PUBLIC cxx_std_20)
    message(STATUS "[MrProper] src/core/CMakeLists.txt отсутствует: цель mrproper_core объявлена на верхнем уровне")
endif()

if(MRPROPER_BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    endif()
    # Страховка на случай, пока tests/CMakeLists.txt ещё не написан.
    # Имя цели mrproper_unit_tests закреплено за tools\test.bat: скрипт ищет
    # build\<Debug|Release>\mrproper_unit_tests.exe.
    if(NOT TARGET mrproper_unit_tests)
        file(GLOB MRPROPER_UNIT_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
        if(MRPROPER_UNIT_TEST_SOURCES)
            add_executable(mrproper_unit_tests ${MRPROPER_UNIT_TEST_SOURCES})
            target_link_libraries(mrproper_unit_tests PRIVATE mrproper_core)
            target_include_directories(mrproper_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
            add_test(NAME core_unit COMMAND mrproper_unit_tests)
            message(STATUS "[MrProper] tests/CMakeLists.txt отсутствует: цель mrproper_unit_tests объявлена на верхнем уровне")
        endif()
    endif()
    if(NOT TARGET mrproper_unit_tests)
        message(WARNING "[MrProper] тесты не собраны: нет ни tests/CMakeLists.txt, ни tests/unit/*.cpp")
    endif()
endif()

# ---------------------------------------------------------------------------
# 3. install-цели, CPack и отчёт о конфигурации
# ---------------------------------------------------------------------------
include(MrProperInstall)

# Финальный проход выполняется в конце обработки этого каталога, когда цели всех
# подкаталогов уже объявлены: ресурсы приложения подключаются автоматически, как
# только появится и цель, и файл манифеста.
cmake_language(DEFER CALL mrproper_finalize_configure)
