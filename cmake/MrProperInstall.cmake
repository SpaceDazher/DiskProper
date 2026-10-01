# ---------------------------------------------------------------------------
# install-цели и CPack. Владелец: W01/задача 01.
# Спека: SPEC §8 Этап 0 (установщик как следствие этапа 0), §5 (набор правил
# поставляется вместе с программой).
#
# Схема раскладки (per-user, права администратора не нужны — см. §4 FR-9):
#   bin/mrproper*.exe                  программа и CLI
#   lib/libmrproper_core.lib           статическое ядро
#   include/mrproper/…                 заголовки ядра для встраивания
#   share/MrProper/rules/*.json        набор правил (ADR-008, подписанный)
#   share/MrProper/RELEASE_NOTES…      документы, если они появятся
#
# Правила ставятся из каталога rules/ целиком, списком: агенты W04-W06
# добавляют файлы по одному, жёсткий список означал бы, что новый набор
# не попадёт в установку, пока кто-то не вспомнит дописать строку.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

# --- Префикс установки -------------------------------------------------------
# Порядок вычисления важен: проверка выполняется ДО include(GNUInstallDirs),
# потому что тот читает CMAKE_INSTALL_PREFIX и создаёт под неё свои
# подкаталоги. Кроме того, запись CMAKE_INSTALL_PREFIX появляется в кэше
# сама, ещё при первом project(), — по наличию записи «пользователь её задал»
# не определить, и тип записи единственный надёжный признак.
#   1) -DMRPROPER_INSTALL_PREFIX=...  — явный выбор проекта, побеждает всех;
#   2) -DCMAKE_INSTALL_PREFIX=...      — уважаем пользователя;
#   3) иначе per-user путь: программа работает без прав администратора
#      (SPEC §4 FR-9, UAC asInvoker), поэтому Program Files ей не нужен.
set(MRPROPER_INSTALL_PREFIX "" CACHE PATH
    "Префикс установки MrProper; пусто = -DCMAKE_INSTALL_PREFIX или per-user путь по умолчанию")

get_property(_mrproper_prefix_type CACHE CMAKE_INSTALL_PREFIX PROPERTY TYPE)
get_property(_mrproper_prefix_value CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE)
if(WIN32)
    set(_mrproper_platform_prefix
        "C:/Program Files/${PROJECT_NAME}"
        "C:/Program Files (x86)/${PROJECT_NAME}")
else()
    set(_mrproper_platform_prefix "/usr/local")
endif()
set(_mrproper_prefix_is_default FALSE)
if(_mrproper_prefix_type STREQUAL "UNINITIALIZED")
    # Значение, переданное в командной строке (-DCMAKE_INSTALL_PREFIX=...,
    # в том числе из пресета), CMake хранит как UNINITIALIZED: это выбор
    # человека, его не трогаем.
    set(_mrproper_prefix_is_default FALSE)
elseif(_mrproper_prefix_value IN_LIST _mrproper_platform_prefix)
    set(_mrproper_prefix_is_default TRUE)
endif()

if(MRPROPER_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX "${MRPROPER_INSTALL_PREFIX}")
elseif(_mrproper_prefix_is_default AND WIN32)
    # Разделители приводит к прямой слэш: значение попадает в кэш, в отчёт о
    # конфигурации и в сообщения CI, смешанные слэши там только мешают.
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Programs/${PROJECT_NAME}" _mrproper_per_user_prefix)
    set(CMAKE_INSTALL_PREFIX "${_mrproper_per_user_prefix}")
    unset(_mrproper_per_user_prefix)
endif()
unset(_mrproper_prefix_type)
unset(_mrproper_prefix_value)
unset(_mrproper_platform_prefix)
unset(_mrproper_prefix_is_default)

include(GNUInstallDirs)

# --- Программа ---------------------------------------------------------------
# Цели приложения и CLI появляются в волнах W13-W15. Список имён известен
# заранее, и каждый элемент проверяется на существование: до волны W13
# устанавливать нечего, и жёсткий install(TARGETS mrproper) уронил бы
# конфигурацию уже сегодня.
set(MRPROPER_APP_TARGET_CANDIDATES
    ${MRPROPER_APP_TARGET}
    mrproper
    MrProper
    mrproper-cli
    mrproper_cli
    mrproper_app)

set(_mrproper_installed_targets "")
foreach(_candidate IN LISTS MRPROPER_APP_TARGET_CANDIDATES)
    if(TARGET ${_candidate})
        # Только исполняемые файлы: статические библиотеки ядра ставятся ниже.
        get_target_property(_type ${_candidate} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            install(TARGETS ${_candidate}
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                BUNDLE  DESTINATION "${CMAKE_INSTALL_BINDIR}")
            list(APPEND _mrproper_installed_targets ${_candidate})
        endif()
    endif()
endforeach()
if(_mrproper_installed_targets)
    string(REPLACE ";" ", " _installed_text "${_mrproper_installed_targets}")
    message(STATUS "[MrProper] install: исполняемые файлы — ${_installed_text}")
else()
    message(STATUS "[MrProper] install: исполняемых файлов пока нет (появятся в W13-W15)")
endif()

# --- Ядро и заголовки --------------------------------------------------------
if(TARGET mrproper_core)
    install(TARGETS mrproper_core
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    # Заголовки раскладываются плоско в include/mrproper, чтобы
    # «-Iinclude/mrproper» повторял исходные «-Isrc/core» и «-Isrc».
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/core/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mrproper"
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
endif()

# --- Набор правил ------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rules")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rules/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/MrProper/rules"
        FILES_MATCHING
            PATTERN "*.json"
            PATTERN "*.md"
            PATTERN ".signature" PATTERN "*.sig")
endif()

# --- Документы ---------------------------------------------------------------
# Ставятся только те, что действительно есть: иначе установка падает на
# несуществующем файле.
foreach(_doc LICENSE LICENSE.txt README.md CHANGELOG.md)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/MrProper")
    endif()
endforeach()

# --- Тесты (по требованию) ---------------------------------------------------
if(MRPROPER_INSTALL_TESTS)
    foreach(_test_target mrproper_unit_tests mrproper_integration_tests)
        if(TARGET ${_test_target})
            install(TARGETS ${_test_target}
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
        endif()
    endforeach()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/MrProper/tests/fixtures")
    endif()
endif()

# --- Ресурсы приложения: манифест UAC и версия (W01-05) ----------------------
# Файлы появляются в волне W01 (задача 05), цель приложения — в W14.
# Подключаются через отложенный вызов, поэтому сегодня функция ничего не
# делает, а через несколько волн манифест и версия попадут в сборку сами.
function(mrproper_apply_app_resources)
    if(NOT TARGET ${MRPROPER_APP_TARGET})
        return()
    endif()
    set(_version_rc "${CMAKE_SOURCE_DIR}/packaging/version.rc")
    if(EXISTS "${_version_rc}")
        target_sources(${MRPROPER_APP_TARGET} PRIVATE "${_version_rc}")
    endif()
    set(_manifest "${CMAKE_SOURCE_DIR}/packaging/app.manifest")
    if(EXISTS "${_manifest}")
        target_link_options(${MRPROPER_APP_TARGET} PRIVATE
            "/MANIFEST:EMBED" "/MANIFESTINPUT:${_manifest}")
        set_property(TARGET ${MRPROPER_APP_TARGET}
            PROPERTY LINK_FLAGS "/MANIFEST:NODEFAULTLIB")
    endif()
endfunction()

# --- CPack -------------------------------------------------------------------
# Конфигурация генераторов установщика. Сама по себе ничего не собирает:
# установщик получается явной командой cpack, а не побочным эффектом сборки.
if(MRPROPER_ENABLE_CPACK)
    set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
    set(CPACK_PACKAGE_VENDOR "MrProper")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
    set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
    set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "MrProper")
    if(TARGET ${MRPROPER_APP_TARGET})
        set(CPACK_PACKAGE_EXECUTABLES "${MRPROPER_APP_TARGET}" "MrProper")
    endif()
    set(CPACK_PACKAGE_CONTACT "https://github.com/SpaceDazher/MrProper")
    # include(CPack) в конце перезаписывает CPACK_GENERATOR значением для
    # source-пакета, поэтому генераторы запоминаем отдельно: иначе в отчёте о
    # конфигурации показывается не то, что записано в CPackConfig.cmake.
    set(_mrproper_cpack_generators "NSIS;ZIP")
    set(CPACK_GENERATOR "${_mrproper_cpack_generators}")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_VERBATIM_VARIABLES ON)
    set(CPACK_SET_DONT_CREATE_PACKAGE_SCRIPTS OFF)
    include(CPack)
    message(STATUS "[MrProper] CPack: генераторы ${_mrproper_cpack_generators}, "
        "установщик собирается явно: cpack -C Release --config CPackConfig.cmake")
endif()

# --- Установщик Inno Setup (владелец: J2) -----------------------------------
# Почему не через CPack: у CPack есть генераторы NSIS, WiX, IFW, ZIP, 7Z, TGZ,
# но генератора Inno Setup у него нет и не предвиден. Поэтому .iss
# компилируется собственным вызовом ISCC, а цель mrproper-installer даёт ту же
# «сборку установщика одной командой», о которой говорит SPEC §8 Этап 5:
#
#   tools\build.bat Release a12
#   cmake --build build\a12 --config Release --target mrproper-installer
#
# В сборку по умолчанию цель не входит: ISCC — внешний инструмент, и его
# отсутствие не должно ломать ни build.bat, ни test.bat.
#
# Источник истины по составу установки один: packaging/mrproper.iss. Формат
# WiX (.wxs) в репозитории нет и не добавляется — обоснование в шапке .iss.
option(MRPROPER_ENABLE_INSTALLER
    "Объявлять цель mrproper-installer (сборка установщика через Inno Setup)" ON)
set(MRPROPER_INNO_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mrproper.iss"
    CACHE FILEPATH "Скрипт Inno Setup: единственный источник истины по составу установки")
set(MRPROPER_ISCC_EXE "" CACHE FILEPATH
    "Путь к ISCC.exe (Inno Setup 6.3+); пусто = найти в Program Files и в PATH")
set(MRPROPER_INSTALLER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH
    "Каталог, куда кладётся готовый MrProper-<версия>-win64.exe")
set(MRPROPER_INSTALLER_FALLBACK_CONFIG "Release" CACHE STRING
    "Конфигурация сборки, если генератор не сообщил (--config не передан)")

# Поиск компилятора. find_program считывает переменные окружения, которые
# WSL/cmd не всегда пробрасывают одинаково, поэтому каталоги установки Inno
# Setup перечислены явно: дефолтный каталог — «%ProgramFiles%\Inno Setup 6».
if(NOT MRPROPER_ISCC_EXE)
    find_program(_mrproper_iscc_found
        NAMES ISCC ISCC.exe
        PATHS
            "$ENV{ProgramFiles}/Inno Setup 6"
            "$ENV{ProgramW6432}/Inno Setup 6"
            "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
            "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6 (x64)"
        DOC "Inno Setup 6 command line compiler (ISCC.exe)")
    if(_mrproper_iscc_found)
        # FORCE здесь уместен: значение по умолчанию, а не выбор человека —
        # если человек задал путь, он попал в MRPROPER_ISCC_EXE и эта ветка
        # не выполняется вовсе.
        set(MRPROPER_ISCC_EXE "${_mrproper_iscc_found}"
            CACHE FILEPATH "Путь к ISCC.exe (Inno Setup 6.3+)" FORCE)
    endif()
    unset(_mrproper_iscc_found)
endif()

if(NOT MRPROPER_ENABLE_INSTALLER)
    message(STATUS
        "[MrProper] установщик: выключен опцией MRPROPER_ENABLE_INSTALLER=OFF")
elseif(NOT EXISTS "${MRPROPER_INNO_SCRIPT}")
    message(STATUS
        "[MrProper] установщик: ${MRPROPER_INNO_SCRIPT} не найден, "
        "цель mrproper-installer не объявляется")
elseif(MRPROPER_ISCC_EXE AND EXISTS "${MRPROPER_ISCC_EXE}")
    # Цели программы в DEPENDS: без них установщик собрался бы из того, что
    # лежит в build\ на момент вызова, и мог бы уехать на сборку позже.
    set(_mrproper_installer_deps "")
    foreach(_mrproper_installer_target mrproper mrproper_cli)
        if(TARGET ${_mrproper_installer_target})
            list(APPEND _mrproper_installer_deps ${_mrproper_installer_target})
        endif()
    endforeach()

    # /V2 — уровень подробности 2: в вывод попадает строка «Compressing: …»
    # на каждый файл, то есть перечень установки виден прямо в логе сборки.
    # /V0 молчит, /V3-V4 засоряют вывод построчным разбором .iss.
    add_custom_target(mrproper-installer
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MRPROPER_INSTALLER_OUTPUT_DIR}"
        COMMAND "${MRPROPER_ISCC_EXE}"
            "/V2"
            "/DMrVersion=${PROJECT_VERSION}"
            "/DMrVersionFull=${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0"
            "/DMrSourceRoot=${CMAKE_CURRENT_SOURCE_DIR}"
            "/DMrBuildRoot=${CMAKE_BINARY_DIR}"
            "/DMrBuildConfig=$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,${MRPROPER_INSTALLER_FALLBACK_CONFIG}>"
            "/DMrOutputDir=${MRPROPER_INSTALLER_OUTPUT_DIR}"
            "/O${MRPROPER_INSTALLER_OUTPUT_DIR}"
            "/F${PROJECT_NAME}-${PROJECT_VERSION}-win64"
            "${MRPROPER_INNO_SCRIPT}"
        DEPENDS ${_mrproper_installer_deps} "${MRPROPER_INNO_SCRIPT}"
        COMMENT "MrProper: установщик (Inno Setup, неподписанный — ADR-009) -> ${MRPROPER_INSTALLER_OUTPUT_DIR}"
        VERBATIM)
    unset(_mrproper_installer_deps)

    message(STATUS "[MrProper] установщик: цель mrproper-installer, ISCC ${MRPROPER_ISCC_EXE}")
else()
    # ISCC не найден. Цель всё равно объявляется — иначе «нет такой цели» в
    # выводе cmake --build выглядит как опечатка в команде, а не как
    # «поставьте Inno Setup». Ошибка с инструкцией выдаётся в момент сборки.
    set(_mrproper_installer_hint "${CMAKE_BINARY_DIR}/mrproper-installer-noiscc.cmake")
    file(WRITE "${_mrproper_installer_hint}" [==[
# Пишется cmake/MrProperInstall.cmake при configure, когда ISCC.exe не найден.
# Запускается целью mrproper-installer: -P исполняет файл и превращает
# message(FATAL_ERROR) в ненулевой код возврата цели.
set(_msg "MrProper: цель mrproper-installer требует ISCC.exe — компилятора Inno Setup 6.3 или новее.")
string(APPEND _msg "\n  Скачать:        https://jrsoftware.org/isdl.php")
string(APPEND _msg "\n  Указать путь:   cmake -S . -B build\\a12 -DMRPROPER_ISCC_EXE=\"C:\\Program Files\\Inno Setup 6\\ISCC.exe\"")
string(APPEND _msg "\n  Отключить цель: cmake -S . -B build\\a12 -DMRPROPER_ENABLE_INSTALLER=OFF")
message(FATAL_ERROR "${_msg}")
]==])
    add_custom_target(mrproper-installer
        COMMAND "${CMAKE_COMMAND}" -P "${_mrproper_installer_hint}"
        VERBATIM)
    message(STATUS
        "[MrProper] установщик: ISCC.exe не найден, цель mrproper-installer "
        "объявлена и упадёт с инструкцией (см. -DMRPROPER_ISCC_EXE=...)")
endif()
