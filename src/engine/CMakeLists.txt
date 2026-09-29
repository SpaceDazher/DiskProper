# ---------------------------------------------------------------------------
# Цель mrproper_engine — слой оркестрации MrProper. Спека: §6.1 (слои),
# §6.2 (engine::* — «оркестрация, план, исполнение, транзакции, undo»),
# §6.4 (потоки и отмена), §9 (src/engine), §9.1 ADR-004.
#
# Слой лежит НАД platform и core: scan_coordinator (пул потоков, std::stop_token,
# прогресс, отмена), locks (Restart Manager), candidate_collector, scan_result,
# scoring_bridge, file_system_probe (мост FileSystemProbe → platform::vfs,
# задача F5). Поэтому единственная строка «вниз» — это core и platform;
# обратных зависимостей (windows.h в engine не нужен, оркестрация переносима)
# файл не вводит.
#
# file_system_probe — единственный модуль слоя, который знает про Win32, и ровно
# настолько: он переводит элементы platform::vfs в форму, которую понимает
# engine::collectCandidates, и ничего не решает про мусор. Именно он подключает
# `mrproper-cli scan` к реальному диску: без него ScanServices::createProbe был
# бы пуст и команда возвращала код 4.
# ---------------------------------------------------------------------------

# Исходники появляются волнами W11+ по одному .cpp на модуль: каждый агент
# владеет только своим файлом, поэтому перечень берётся каталогом — иначе
# модуль молча не попал бы в сборку, пока кто-то не вспомнит дописать строку.
# CONFIGURE_DEPENDS заставляет CMake перечитывать каталог (шаг «Checking File
# Globs» при сборке), а tools\build.bat переконфигурирует проект при каждом
# запуске. Порядок файлов на единицы трансляции не влияет.
file(GLOB MRPROPER_ENGINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Идемпотентность — как в src/core и src/platform: если цель уже объявлена выше
# (страховка на верхнем уровне или повторное подключение каталога), add_library
# упал бы с «target already exists».
if(NOT TARGET mrproper_engine)
    if(MRPROPER_ENGINE_SOURCES)
        add_library(mrproper_engine STATIC ${MRPROPER_ENGINE_SOURCES})
    else()
        # Статическую библиотеку без единого файла создать нельзя, поэтому до
        # появления первого .cpp цель существует как INTERFACE-заглушка: она
        # держит граф зависимостей (cli, ui, интеграционные тесты) неизменным
        # от волны к волне. Область видимости свойств ниже выбирается по
        # фактическому типу цели, а не по ветке выше.
        add_library(mrproper_engine INTERFACE)
        message(STATUS
            "[MrProper] цель mrproper_engine: исходников нет, "
            "временно INTERFACE-заглушка (появится STATIC с первым .cpp)")
    endif()
endif()

# У INTERFACE-цели принимаются только INTERFACE-свойства, поэтому область
# видимости выбираем по фактическому типу цели.
get_target_property(MRPROPER_ENGINE_TYPE mrproper_engine TYPE)
if(MRPROPER_ENGINE_TYPE STREQUAL "INTERFACE_LIBRARY")
    set(MRPROPER_ENGINE_SCOPE INTERFACE)
else()
    set(MRPROPER_ENGINE_SCOPE PUBLIC)
endif()

# Корень src — тот же контракт, что у ядра и платформы (корень задаёт переменную
# сам, запасной вариант — случай подключения каталога через add_subdirectory).
if(NOT DEFINED MRPROPER_SRC_DIR)
    get_filename_component(MRPROPER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()

# PUBLIC, потому что от engine зависят cli, ui и интеграционные тесты. Второй
# каталог даёт короткую форму #include "scan_coordinator.hpp" внутри слоя.
target_include_directories(mrproper_engine ${MRPROPER_ENGINE_SCOPE}
    "${MRPROPER_SRC_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(mrproper_engine ${MRPROPER_ENGINE_SCOPE} cxx_std_20)

# Зависимости слоя — только вниз (SPEC §6.2: engine::* зависит от «всего выше»):
# переносимое ядро и Win32-обёртки (engine::locks ходит в platform::procs).
target_link_libraries(mrproper_engine ${MRPROPER_ENGINE_SCOPE}
    mrproper_core
    mrproper_platform
)

# Тот же барьер предупреждений, что у ядра и платформы (SPEC §9.1, ADR-001).
# Флаги раздаёт cmake/MrProperWarnings.cmake на уровне каталога и объявляет
# MRPROPER_WARNINGS_READY — тогда слой ничего не добавляет. Профиль WIN32 снимает
# C4996 и задаёт определения Win32-слоя: слой стоит над platform и включает его
# заголовки. Ручной набор — последний запасной путь.
if(NOT MRPROPER_WARNINGS_READY)
    if(COMMAND mrproper_enable_warnings)
        mrproper_enable_warnings(mrproper_engine WIN32)
    elseif(MSVC)
        target_compile_options(mrproper_engine ${MRPROPER_ENGINE_SCOPE} /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(mrproper_engine ${MRPROPER_ENGINE_SCOPE} -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

if(MRPROPER_ENGINE_SOURCES)
    list(LENGTH MRPROPER_ENGINE_SOURCES _mrproper_engine_count)
    message(STATUS
        "[MrProper] цель mrproper_engine: ${_mrproper_engine_count} модулей "
        "(оркестрация, зависит от mrproper_core и mrproper_platform)")
endif()
