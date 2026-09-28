# ---------------------------------------------------------------------------
# Цель mrproper_core — переносимое ядро MrProper. Спека: §6.1 (слои),
# §6.2 (модули core::model/rules/scoring/json/units/log), §9.1 ADR-004
# («Core без Windows-зависимостей»).
#
# Ядро объявляется здесь, а не в корне: корень только подключает подкаталоги.
# Это единственный слой, который собирается и тестируется на любом хосте,
# поэтому здесь запрещены windows.h, COM и WinHTTP, а внешние зависимости
# (vcpkg/FetchContent) отложены — ADR-001.
#
# От цели зависят platform, engine, scanner, cli, ui и оба набора тестов,
# поэтому контракт зафиксирован целиком (корень дублирует его только как
# страховку на случай, пока этого файла нет):
#   * PUBLIC src      — #include "core/model.hpp";
#   * PUBLIC src/core — #include "json.hpp";
#   * PUBLIC cxx_std_20 — старшие слои стандарт заново не объявляют.
# ---------------------------------------------------------------------------

# Перечень модулей берётся каталогом: волны W02-W18 добавляют ядро по одному
# .cpp, и жёсткий список означал бы, что модуль молча не попадёт в сборку,
# пока кто-то не вспомнит дописать строку. CONFIGURE_DEPENDS заставляет CMake
# перечитывать каталог (шаг «Checking File Globs» при сборке), а tools\build.bat
# переконфигурирует проект при каждом запуске. Порядок файлов на единицы
# трансляции не влияет.
file(GLOB MRPROPER_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Идемпотентность: если цель уже объявлена выше (страховка в корне или повторное
# подключение каталога), add_library упал бы с «target already exists».
# Создаём один раз, а контракт ниже применяем в любом случае — так объявление
# из корня и из этого файла дают одну и ту же цель, а не две разные.
if(NOT TARGET mrproper_core)
    add_library(mrproper_core STATIC ${MRPROPER_CORE_SOURCES})
endif()

# Корень src. Верхний уровень задаёт MRPROPER_SRC_DIR сам; запасной вариант —
# случай, когда каталог подключают сторонним проектом через add_subdirectory.
if(NOT DEFINED MRPROPER_SRC_DIR)
    get_filename_component(MRPROPER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()

# PUBLIC, потому что от ядра зависят все слои выше и тесты: включение заголовка
# ядра обязано работать у них без ручного -I. Второй каталог даёт короткую
# форму #include "json.hpp" внутри самого ядра.
target_include_directories(mrproper_core PUBLIC
    "${MRPROPER_SRC_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(mrproper_core PUBLIC cxx_std_20)

# Планка предупреждений (SPEC §9.1, ADR-001: /W4 /WX как митигация риска
# C++20 + Win32). Флаги раздаёт cmake/MrProperWarnings.cmake на уровне каталога
# и объявляет MRPROPER_WARNINGS_READY — тогда слой ничего не добавляет.
# Профильный модуль cmake/warnings.cmake — следующий источник, ручной набор —
# последний запасной путь, чтобы файл оставался рабочим и без верхнего уровня.
if(NOT MRPROPER_WARNINGS_READY)
    if(COMMAND mrproper_enable_warnings)
        mrproper_enable_warnings(mrproper_core)
    elseif(MSVC)
        target_compile_options(mrproper_core PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(mrproper_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

list(LENGTH MRPROPER_CORE_SOURCES _mrproper_core_count)
message(STATUS
    "[MrProper] цель mrproper_core: ${_mrproper_core_count} модулей "
    "(переносимое ядро, без Windows API)")
