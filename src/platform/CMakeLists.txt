# ---------------------------------------------------------------------------
# Цель mrproper_platform — слой Win32 MrProper. Спека: §6.1 (слои),
# §6.2 (platform::devices/wmi/vfs/procs/sysinfo/net), §9 (src/platform),
# §9.1 ADR-004 (обратной зависимости у Win32 нет).
#
# Это единственное место, где допустимы windows.h, COM, SetupAPI и WinHTTP.
# Ядро платформы не знает, поэтому единица трансляции с WinAPI физически не
# может попасть в mrproper_core: файлы берутся каталогом, а граница проходит
# по границе каталогов.
#
# Зависимость ровно одна и только вниз — на mrproper_core (SPEC §6.2).
# ---------------------------------------------------------------------------

# Исходники появляются волнами W07-W10: win_handle/win_error, devices, layout,
# volumes, size_probe, storage_query, bus_type, trim_cache, gpt_kind, inventory,
# vfs_walk/size/paths/delete/trash, restart_manager, process_control, wmi, net,
# rulesync_client. Каждый агент владеет только своим файлом, поэтому перечень
# берётся каталогом — иначе модуль не соберётся без правки этого файла.
file(GLOB MRPROPER_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Идемпотентность — как в src/core: если цель уже объявлена выше, создаём один
# раз, а контракт (включающие каталоги, стандарт, зависимости) применяем в любом
# случае, чтобы обе точки объявления давали одну и ту же цель.
if(NOT TARGET mrproper_platform)
    if(MRPROPER_PLATFORM_SOURCES)
        add_library(mrproper_platform STATIC ${MRPROPER_PLATFORM_SOURCES})
    else()
        # Исходников ещё нет. Статической библиотеки без единого файла создать
        # нельзя, поэтому до появления первого .cpp цель существует как
        # INTERFACE-заглушка: она держит граф зависимостей (engine, cli, ui,
        # интеграционные тесты) неизменным от волны к волне и превращается в
        # обычную статическую библиотеку на следующей конфигурации, как только
        # появится первый файл. Область видимости свойств ниже выбирается по
        # фактическому типу цели, а не по ветке выше.
        add_library(mrproper_platform INTERFACE)
        message(STATUS
            "[MrProper] цель mrproper_platform: исходников нет, "
            "временно INTERFACE-заглушка (появится STATIC с первым .cpp)")
    endif()
endif()

# У INTERFACE-цели принимаются только INTERFACE-свойства, поэтому область
# видимости выбираем по фактическому типу цели.
get_target_property(MRPROPER_PLATFORM_TYPE mrproper_platform TYPE)
if(MRPROPER_PLATFORM_TYPE STREQUAL "INTERFACE_LIBRARY")
    set(MRPROPER_PLATFORM_SCOPE INTERFACE)
else()
    set(MRPROPER_PLATFORM_SCOPE PUBLIC)
endif()

# Корень src — тот же контракт, что и у ядра (корень задаёт переменную сам).
if(NOT DEFINED MRPROPER_SRC_DIR)
    get_filename_component(MRPROPER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()

# PUBLIC, потому что от платформы зависят engine, cli, ui и интеграционные
# тесты. Второй каталог даёт #include "devices.hpp" внутри самого слоя.
target_include_directories(mrproper_platform ${MRPROPER_PLATFORM_SCOPE}
    "${MRPROPER_SRC_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(mrproper_platform ${MRPROPER_PLATFORM_SCOPE} cxx_std_20)

# Единственная зависимость слоя — переносимое ядро (SPEC §6.2). Обратной нет.
target_link_libraries(mrproper_platform ${MRPROPER_PLATFORM_SCOPE} mrproper_core)

# Системные библиотеки Windows. Для статической библиотеки неиспользуемые
# записи безвредны: компоновщик тянет только реально нужные объекты, поэтому
# список можно держать полным и не пересматривать его от волны к волне.
target_link_libraries(mrproper_platform ${MRPROPER_PLATFORM_SCOPE}
    setupapi   # GUID_DEVINTERFACE_DISK, SetupDiGetClassDevs (FR-1 п.1)
    uuid       # GUID_DEVINTERFACE_DISK, IID_IWbemLocator (FR-1)
    ole32      # CoInitializeEx, CoCreateInstance (WMI, FR-1 п.7)
    oleaut32   # VARIANT, BSTR, SafeArray
    advapi32   # реестр (тема, §5), AdjustTokenPrivileges
    shlwapi    # PathAppendW, StrCmpLogicalW (§10 защита путей)
    winhttp    # WinHttpOpen — только GET (§9.2, ADR-008)
    rstrtmgr   # Restart Manager: RmStartSession, RmGetList (FR-6)
)

# Тот же барьер предупреждений, что и у ядра (SPEC §9.1, ADR-001). Профиль
# WIN32 добавляет определения слоя и снимает C4996; флаги придут из
# cmake/warnings.cmake, как только верхний уровень дойдёт до профилей.
if(NOT MRPROPER_WARNINGS_READY)
    if(COMMAND mrproper_enable_warnings)
        mrproper_enable_warnings(mrproper_platform WIN32)
    elseif(MSVC)
        target_compile_options(mrproper_platform ${MRPROPER_PLATFORM_SCOPE} /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(mrproper_platform ${MRPROPER_PLATFORM_SCOPE} -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

if(MRPROPER_PLATFORM_SOURCES)
    list(LENGTH MRPROPER_PLATFORM_SOURCES _mrproper_platform_count)
    message(STATUS
        "[MrProper] цель mrproper_platform: ${_mrproper_platform_count} модулей "
        "(слой Win32, зависит от mrproper_core)")
endif()
