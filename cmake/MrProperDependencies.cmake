# ---------------------------------------------------------------------------
# Внешние зависимости: vcpkg или FetchContent (SPEC §8, Этап 0).
# Владелец: W01/задача 01.
#
# Модуль ничего не ищет и ничего не качает сам. Он объявляет два способа
# добавить зависимость в проект и одну функцию отчёта:
#
#   mrproper_find_dependency(NAME fmt [VERSION 11] [COMPONENTS std])
#       Обычный find_package с честным сообщением, если пакет не найден:
#       перечисляются оба пути (vcpkg и FetchContent) и точная команда.
#
#   mrproper_fetch_dependency(NAME nameof
#       GIT_REPOSITORY https://github.com/… GIT_TAG <тег>
#       [FIND_PACKAGE_ARGS NAMES …] [EXCLUDE_FROM_ALL])
#       FetchContent с закреплённым тегом. Если пакет уже есть в системе,
#       FIND_PACKAGE_ARGS позволяет им воспользоваться и не качать ничего.
#
#   mrproper_report_dependencies()
#       Блок STATUS для отчёта о конфигурации: провайдер, найден ли vcpkg,
#       разрешена ли сеть.
#
# Важные свойства, ради которых модуль написан так, а не иначе:
#
#   * Каноническая сборка tools\build.bat не ходит в сеть. Скачивание
#     включается явно (MRPROPER_ALLOW_NETWORK=ON или
#     FETCHCONTENT_FULLY_DISCONNECTED=ON), иначе configure падает с текстом
#     «нужна сеть», а не собирает проект без зависимости.
#   * Провайдер выбирается один: auto | vcpkg | fetchcontent | none. «auto»
#     означает «сначала find_package, при неудаче — понятная ошибка», а не
#     «попробовать оба способа и молча взять первый упавший».
#   * Список зависимостей не хранится в модуле. Он объявляется в
#     CMakeLists.txt того слоя, которому зависимость нужна: у слоя
#     src/core их не бывает вовсе (SPEC §6.1, ADR-004), и он обязан
#     собираться на любом хосте без сети.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

# Значение по умолчанию берётся из cmake/MrProperOptions.cmake; модуль
# используется и отдельно (например, в cmake -P), поэтому при отсутствии
# опции создаётся то же значение, а не пустая строка.
if(NOT DEFINED MRPROPER_DEPS_PROVIDER)
    set(MRPROPER_DEPS_PROVIDER "auto" CACHE STRING
        "Как добываются внешние зависимости: auto | vcpkg | fetchcontent | none")
endif()
if(NOT DEFINED MRPROPER_ALLOW_NETWORK)
    option(MRPROPER_ALLOW_NETWORK
        "Разрешать FetchContent скачивать зависимости во время configure" OFF)
endif()

set_property(CACHE MRPROPER_DEPS_PROVIDER
    PROPERTY STRINGS auto vcpkg fetchcontent none)

string(TOLOWER "${MRPROPER_DEPS_PROVIDER}" _mrproper_deps_provider)
if(NOT _mrproper_deps_provider STREQUAL "auto"
   AND NOT _mrproper_deps_provider STREQUAL "vcpkg"
   AND NOT _mrproper_deps_provider STREQUAL "fetchcontent"
   AND NOT _mrproper_deps_provider STREQUAL "none")
    message(FATAL_ERROR
        "MRPROPER_DEPS_PROVIDER=\"${MRPROPER_DEPS_PROVIDER}\" недопустимо. "
        "Допустимо: auto, vcpkg, fetchcontent, none")
endif()

# Сеть запрещена по умолчанию. Пользовательский FETCHCONTENT_FULLY_DISCONNECTED
# уважается: он означает «источник уже скачан, из сети не брать».
function(_mrproper_network_allowed out)
    if(MRPROPER_ALLOW_NETWORK)
        set(${out} TRUE PARENT_SCOPE)
    elseif(FETCHCONTENT_FULLY_DISCONNECTED)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Состояние vcpkg. Ничего не ищет на диске: достаточно признаков того, что
# тулчейн уже подключён, иначе — подсказка с точной командой.
# ---------------------------------------------------------------------------
function(mrproper_vcpkg_status out_state out_hint)
    set(_state "не используется")
    if(DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
        set(_state "подключён, триплет «${VCPKG_TARGET_TRIPLET}»")
    elseif(CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\\.cmake$")
        set(_state "подключён через ${CMAKE_TOOLCHAIN_FILE}")
    elseif(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        set(_state "VCPKG_ROOT=${'$'}{ENV{VCPKG_ROOT}}")
    endif()

    set(_hint "")
    if(_state STREQUAL "не используется")
        set(_hint
            "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows")
    endif()
    set(${out_state} "${_state}" PARENT_SCOPE)
    set(${out_hint} "${_hint}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# mrproper_find_dependency(<PackageName> [VERSION v] [COMPONENTS ...] [QUIET])
#
# Тонкая обёртка над find_package. Единственное отличие от прямого вызова —
# сообщение об ошибке: оно перечисляет оба поддерживаемых пути, потому что
# «Could not find a package configuration file provided by …» в проекте с
# 20 волнами читается как тупик, а на деле означает две минуты работы.
# ---------------------------------------------------------------------------
function(mrproper_find_dependency package_name)
    cmake_parse_arguments(ARG "QUIET" "VERSION" "COMPONENTS" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "mrproper_find_dependency: неизвестные аргументы "
            "${ARG_UNPARSED_ARGUMENTS}. Ожидается: [VERSION v] [COMPONENTS ...] [QUIET]")
    endif()

    set(_args "")
    if(ARG_VERSION)
        list(APPEND _args ${ARG_VERSION})
    endif()
    if(ARG_COMPONENTS)
        list(APPEND _args COMPONENTS ${ARG_COMPONENTS})
    endif()
    if(ARG_QUIET)
        list(APPEND _args QUIET)
    endif()

    # QUIET на время поиска: собственное сообщение об ошибке полезнее
    # стандартного «NOTFOUND», который печатается даже при успешном find.
    find_package(${package_name} ${_args} QUIET)
    if(${package_name}_FOUND)
        set(${package_name}_MRPROPER_PROVIDER "система" PARENT_SCOPE)
        return()
    endif()

    mrproper_vcpkg_status(_vcpkg_state _vcpkg_hint)
    if(_vcpkg_hint STREQUAL "")
        set(_vcpkg_advice "vcpkg подключён (${_vcpkg_state}), но пакета в нём нет: vcpkg install ${package_name}")
    else()
        set(_vcpkg_advice "указать тулчейн при конфигурации: cmake ${_vcpkg_hint}")
    endif()
    message(FATAL_ERROR
        "[MrProper] зависимость «${package_name}» не найдена.\n"
        "  Провайдер: MRPROPER_DEPS_PROVIDER=${MRPROPER_DEPS_PROVIDER}\n"
        "  vcpkg:     ${_vcpkg_state}\n"
        "  Два пути:\n"
        "    1) vcpkg — установить пакет и ${_vcpkg_advice}\n"
        "    2) FetchContent — объявить зависимость через\n"
        "         mrproper_fetch_dependency(${package_name} GIT_REPOSITORY … GIT_TAG <тег>)\n"
        "         и разрешить сеть: -DMRPROPER_ALLOW_NETWORK=ON")
endfunction()

# ---------------------------------------------------------------------------
# mrproper_fetch_dependency(<name> GIT_REPOSITORY <url> GIT_TAG <tag>
#                           [FIND_PACKAGE_ARGS <args...>] [EXCLUDE_FROM_ALL])
#
# GIT_TAG — это не украшение: без закреплённого тега configure тянет ветку
# master, и однажды «зелёная» сборка перестаёт воспроизводиться без правки
# в репозитории. Пустой GIT_TAG поэтому считается ошибкой.
# ---------------------------------------------------------------------------
function(mrproper_fetch_dependency name)
    cmake_parse_arguments(ARG "EXCLUDE_FROM_ALL" "GIT_REPOSITORY;GIT_TAG" "FIND_PACKAGE_ARGS" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "mrproper_fetch_dependency: неизвестные аргументы ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT ARG_GIT_REPOSITORY OR NOT ARG_GIT_TAG)
        message(FATAL_ERROR
            "mrproper_fetch_dependency(${name}): обязательны GIT_REPOSITORY и GIT_TAG. "
            "Тег закрепляет версию: ветка master сделает сборку невоспроизводимой")
    endif()

    set(_net_allowed FALSE)
    _mrproper_network_allowed(_net_allowed)
    if(NOT _net_allowed)
        message(FATAL_ERROR
            "[MrProper] зависимость «${name}» объявлена через FetchContent, но сеть "
            "запрещена: MRPROPER_ALLOW_NETWORK=OFF и FETCHCONTENT_FULLY_DISCONNECTED не заданы.\n"
            "  Скачивание включается явно, чтобы каноническая сборка tools\\build.bat "
            "никогда не зависела от сети:\n"
            "    cmake -DMRPROPER_ALLOW_NETWORK=ON …\n"
            "  Если источник уже скачан в каталог зависимости — "
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
    endif()

    # Сначала ищем в системе: если пакет уже есть (vcpkg, conda, системная
    # установка), качать вторую копию незачем, и версии начинают расходиться.
    if(ARG_FIND_PACKAGE_ARGS)
        find_package(${ARG_FIND_PACKAGE_ARGS} QUIET)
        if(${name}_FOUND)
            set(${name}_MRPROPER_PROVIDER "система" PARENT_SCOPE)
            return()
        endif()
    endif()

    include(FetchContent)
    set(_fetch_args "")
    if(ARG_EXCLUDE_FROM_ALL)
        list(APPEND _fetch_args EXCLUDE_FROM_ALL)
    endif()
    FetchContent_Declare(${name}
        GIT_REPOSITORY "${ARG_GIT_REPOSITORY}"
        GIT_TAG        "${ARG_GIT_TAG}"
        GIT_SHALLOW    TRUE
        ${_fetch_args}
    )
    FetchContent_MakeAvailable(${name})
    set(${name}_MRPROPER_PROVIDER "FetchContent" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Отчёт для mrproper_print_config_summary. Ничего не проверяет: только
# показывает, что именно сработает, если зависимость появится.
# ---------------------------------------------------------------------------
function(mrproper_report_dependencies)
    mrproper_vcpkg_status(_vcpkg_state _vcpkg_hint)
    _mrproper_network_allowed(_net_allowed)
    if(_net_allowed)
        set(_net "разрешена")
    else()
        set(_net "запрещена (MRPROPER_ALLOW_NETWORK=OFF)")
    endif()
    if(DEFINED FETCHCONTENT_FULLY_DISCONNECTED AND FETCHCONTENT_FULLY_DISCONNECTED)
        set(_fc "полностью отключена (FETCHCONTENT_FULLY_DISCONNECTED=ON)")
    else()
        set(_fc "обычный режим")
    endif()
    message(STATUS "[MrProper] зависимости: провайдер ${MRPROPER_DEPS_PROVIDER}, "
        "сеть ${_net}, FetchContent ${_fc}, vcpkg ${_vcpkg_state}")
endfunction()
