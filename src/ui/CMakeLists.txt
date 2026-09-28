# ---------------------------------------------------------------------------
# Цель mrproper_ui — слой интерфейса MrProper. Спека: §6.1 (слои), §6.2
# (`ui::*` — «окна, вкладки, рендер, темы, локализация, навигация»), §7
# (ADR-003: гибрид Direct2D и нативных контролов), §9 (src/ui: «app-shell,
# views, d2d renderer, theme, i18n, a11y»), §9.1 ADR-004.
#
# ВЛАДЕЛЕЦ ЭТОГО ФАЙЛА. Ни одна задача волн W14-W15 в списке работ не закреплена
# за src/ui/CMakeLists.txt, а верхний уровень подключает подкаталог только при
# наличии собственного CMakeLists.txt (файл верхнего уровня перечисляет модули
# по каталогам и не содержит жёсткого списка исходников). Без этого файла слой
# интерфейса молча не попадает в сборку: задачи 66-75 компилируются, ничего не
# проверяя. Файл написан по образцу src/engine/CMakeLists.txt и src/platform/
# CMakeLists.txt и правки других агентов не требует — каждый новый .cpp в
# src/ui подхватывается сам.
#
# Слой лежит НАД platform и core и НАД engine: рендер (Direct2D, GDI) берёт
# ошибки и переводы строк из platform::win_error и пишет в core::log, экраны
# (71-75) будут брать модель у engine. Обратных зависимостей у ui нет: ни один
# файл этого каталога не должен включать <d2d1.h> вне .cpp и не должен
# обращаться к WinAPI без обёртки из platform.
# ---------------------------------------------------------------------------

# Исходники появляются по одному .cpp на модуль (renderer, app_shell, theme,
# locale, nav, пять экранов, мост модель-представление), и каждый агент владеет
# только своим файлом — поэтому перечень берётся каталогом. CONFIGURE_DEPENDS
# заставляет CMake перечитывать каталог (шаг «Checking File Globs»), а
# tools\build.bat переконфигурирует проект при каждом запуске.
file(GLOB MRPROPER_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Идемпотентность — как в src/core, src/platform и src/engine: если цель уже
# объявлена выше (страховка верхнего уровня или повторное подключение каталога),
# add_library упал бы с «target already exists».
if(NOT TARGET mrproper_ui)
    if(MRPROPER_UI_SOURCES)
        add_library(mrproper_ui STATIC ${MRPROPER_UI_SOURCES})
    else()
        # Статическую библиотеку без единого файла создать нельзя, поэтому до
        # появления первого .cpp цель существует как INTERFACE-заглушка: она
        # держит граф зависимостей (приложение, интеграционные и e2e-проверки)
        # неизменным от волны к волне.
        add_library(mrproper_ui INTERFACE)
        message(STATUS
            "[MrProper] цель mrproper_ui: исходников нет, "
            "временно INTERFACE-заглушка (появится STATIC с первым .cpp)")
    endif()
endif()

# У INTERFACE-цели принимаются только INTERFACE-свойства, поэтому область
# видимости выбираем по фактическому типу цели.
get_target_property(MRPROPER_UI_TYPE mrproper_ui TYPE)
if(MRPROPER_UI_TYPE STREQUAL "INTERFACE_LIBRARY")
    set(MRPROPER_UI_SCOPE INTERFACE)
else()
    set(MRPROPER_UI_SCOPE PUBLIC)
endif()

# Корень src — тот же контракт, что у ядра, платформы и движка.
if(NOT DEFINED MRPROPER_SRC_DIR)
    get_filename_component(MRPROPER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()

# PUBLIC, потому что от ui зависит исполняемый файл приложения и e2e-проверки.
# Второй каталог даёт короткую форму #include "renderer.hpp" внутри слоя.
target_include_directories(mrproper_ui ${MRPROPER_UI_SCOPE}
    "${MRPROPER_SRC_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(mrproper_ui ${MRPROPER_UI_SCOPE} cxx_std_20)

# Зависимости — только вниз (SPEC §6.2: ui::* зависит от engine и core).
# platform подключён ради win_error/win_handle: слой интерфейса ходит по тем же
# WinAPI, что и остальной Win32-код, и дублировать RAII-обёртки ради одного
# каталога означало бы завести два определения unique_handle в сборке.
target_link_libraries(mrproper_ui ${MRPROPER_UI_SCOPE}
    mrproper_core
    mrproper_platform
)

# Системные библиотеки графики и окон. Для статической библиотеки неиспользуемые
# записи безвредны: компоновщик тянет только реально нужные объекты, поэтому
# список можно держать полным и не пересматривать его от волны к волне.
#
#   d3d11, dxgi  — устройство и swap chain (ADR-003: Direct2D поверх DXGI)
#   d2d1         — сам рендерер и D2D-поверхность
#   dwrite       — DirectWrite: раскладка, метрики, многоточие
#   gdi32, user32 — откат на GDI и DPI окна (GetDpiForWindow)
#   ole32, uuid  — COM: D2D1CreateFactory и DWriteCreateFactory
target_link_libraries(mrproper_ui ${MRPROPER_UI_SCOPE}
    d3d11       # D3D11CreateDevice, feature-probe (§10)
    dxgi        # IDXGIDevice, IDXGISwapChain1
    d2d1        # ID2D1Factory1, ID2D1Device, ID2D1DeviceContext
    dwrite      # IDWriteFactory, IDWriteTextFormat, IDWriteTextLayout
    gdi32       # fallback на GDI (§5)
    user32      # окно, GetDpiForWindow, GetClientRect
    ole32       # COM-инициализация
    uuid        # IID_ID2D1Factory1, IID_IDWriteFactory
)

# Тот же барьер предупреждений, что и у ядра, платформы и движка (SPEC §9.1,
# ADR-001). Флаги раздаёт cmake/MrProperWarnings.cmake на уровне каталога и
# объявляет MRPROPER_WARNINGS_READY — тогда слой ничего не добавляет. Профиль
# WIN32 снимает C4996 и задаёт определения Win32-слоя (_WIN32_WINNT=0x0A00 —
# Windows 10 22H2, §5). Ручной набор — последний запасной путь.
if(NOT MRPROPER_WARNINGS_READY)
    if(COMMAND mrproper_enable_warnings)
        mrproper_enable_warnings(mrproper_ui WIN32)
    elseif(MSVC)
        target_compile_options(mrproper_ui ${MRPROPER_UI_SCOPE} /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(mrproper_ui ${MRPROPER_UI_SCOPE} -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

if(MRPROPER_UI_SOURCES)
    list(LENGTH MRPROPER_UI_SOURCES _mrproper_ui_count)
    message(STATUS
        "[MrProper] цель mrproper_ui: ${_mrproper_ui_count} модулей "
        "(слой интерфейса, зависит от mrproper_core и mrproper_platform)")
endif()

# ---------------------------------------------------------------------------
# Программа — задача 66 (app_shell.*), добавлено дополнением к этому файлу
# ---------------------------------------------------------------------------
# Блок ничего не меняет выше. Три решения, каждое с причиной:
#
# 1) Имя цели — mrproper, а не MRPROPER_APP_TARGET (mrproper_app).
#
#    cmake/MrProperInstall.cmake::mrproper_apply_app_resources() подключает к цели
#    с именем ${MRPROPER_APP_TARGET} ресурсы приложения и, кроме того, одну строку:
#        set_property(TARGET ... PROPERTY LINK_FLAGS "/MANIFEST:NODEFAULTLIB")
#    Этот ключ не является ключом компоновщика link.exe, и как только цель с таким
#    именем появляется в проекте, компоновка падает:
#        LINK : fatal error LNK1117: недопустимый ключ "MANIFEST:NODEFAULTLIB"
#    Проверено сборкой в слоте a66. Манифест при этом встраивается правильно
#    (строки выше постой функции делают /MANIFEST:EMBED и /MANIFESTINPUT:),
#    ломается только лишняя строка. Файл cmake/MrProperInstall.cmake — чужой
#    (владелец W01/задачи 01-02), править его из слоя ui нельзя, поэтому программа
#    названа именем mrproper: его нет в MRPROPER_APP_TARGET, отложенный вызов
#    возвращается сразу, и дерево собирается. Имя mrproper есть в списке
#    MRPROPER_APP_TARGET_CANDIDATES того же файла, поэтому install(TARGETS) и
#    установщик её видят. Правка, которую стоит сделать владельцу файла: убрать
#    строку LINK_FLAGS (либо заменить её на target_link_options, где ключ дойдёт
#    до компоновщика как есть) и вернуть имя цели mrproper_app.
#
# 2) Ресурс версии подключается здесь, манифест UAC — НЕ подключается. Так и
#    оставлено намеренно: манифест в .exe встраивается линкером, и на этом хосте
#    любой переданный через /MANIFESTINPUT манифест даёт не запускающийся файл
#    (ERROR_SXS_CANT_GEN_ACTCTX), даже пересобранный через mt.exe. Подробности,
#    шаги проверки и то, что именно должен исправить владелец
#    cmake/MrProperInstall.cmake, — в комментарии «Дефект манифеста» ниже.
#    Сейчас приложение стартует с манифестом по умолчанию
#    (requestedExecutionLevel=asInvoker), то есть UAC-запроса нет, как и требует
#    §5; не хватает объявлений longPathAware и PerMonitorV2.
#
# 3) Точка входа (wWinMain) живёт в app_shell.cpp и обязана попасть в
#    исполняемый файл. app_shell.cpp остаётся и в mrproper_ui (перечень берётся
#    каталогом, и список экранов 71-75 его не трогает), поэтому он компилируется
#    дважды: объектный файл программы определяет все символы AppShell и wWinMain,
#    а одноимённый член библиотеки компоновщик не подтягивает — из .lib берутся
#    только те члены, которые закрывают ещё не определённые символы. Дублирующихся
#    определений в финальной сборке не возникает.
#
# comctl32 подключается здесь, а не в списке выше: ICC_STANDARD_CLASSES нужен
# SysListView32/SysTreeView32 из §7 и app_shell (InitCommonControlsEx), и обе
# вещи живут именно в программе.
if(NOT TARGET mrproper AND NOT TARGET mrproper_app)
    set(_mrproper_app_entry "${CMAKE_CURRENT_SOURCE_DIR}/app_shell.cpp")
    add_executable(mrproper WIN32 "${_mrproper_app_entry}")
    target_link_libraries(mrproper PRIVATE
        mrproper_ui
        comctl32  # SysListView32, SysTreeView32, ICC_STANDARD_CLASSES (§7)
    )

    # Ресурс версии: обычный RT_VERSION, он компилируется rc.exe и на запуск не
    # влияет (проверено: приложение стартует). Манифест packaging/app.manifest
    # НЕ подключается здесь — см. длинный комментарий ниже.
    set(_mrproper_version_rc "${CMAKE_SOURCE_DIR}/packaging/version.rc")
    if(EXISTS "${_mrproper_version_rc}")
        target_sources(mrproper PRIVATE "${_mrproper_version_rc}")
    endif()
    message(STATUS
        "[MrProper] цель mrproper: программа, точка входа в app_shell.cpp, "
        "ресурс версии из packaging/version.rc")
endif()

# ---------------------------------------------------------------------------
# Дефект манифеста, который нужно закрыть владельцу cmake/MrProperInstall.cmake
# ---------------------------------------------------------------------------
# Факты, собранные сборкой в слоте a66 (07:28-07:35), ничего не додумывая:
#
#  1. Как только в проекте появляется цель с именем ${MRPROPER_APP_TARGET}
#     (mrproper_app), отложенная функция mrproper_apply_app_resources() ломает
#     компоновку:
#         LINK : fatal error LNK1117: недопустимый ключ "MANIFEST:NODEFAULTLIB"
#     Строка set_property(TARGET ... PROPERTY LINK_FLAGS "/MANIFEST:NODEFAULTLIB")
#     не является ключом компоновщика link.exe.
#
#  2. Если этот ключ убрать и подключить манифест по документации
#     (packaging/README.md §1) — /MANIFEST:EMBED + /MANIFESTINPUT: —
#     файл собирается, но НЕ ЗАПУСКАЕТСЯ: Win32 14001
#     (ERROR_SXS_CANT_GEN_ACTCTX). Проверено с минимальным манифестом, с манифестом
#     проекта, с пересборкой ресурса через mt.exe (BOM убран, XML проверен),
#     с отключённой генерацией манифеста средствами MSBuild. Во всех случаях в
#     .exe ровно ОДИН ресурс RT_MANIFEST, имя — числовое 1, содержимое — корректный
#     XML, и mt.exe читает его без ошибок.
#
#  3. Собранный с манифестом по умолчанию (линкер/MSBuild пишут asInvoker сами)
#     mrproper.exe запускается, показывает окно и закрывается по WM_CLOSE.
#     То есть дело не в коде каркаса, а в том, как манифест попадает в файл.
#
# Что сейчас в сборке: манифест не подключается, и приложение стартует с
# манифестом по умолчанию — requestedExecutionLevel=asInvoker, то есть §5
# «рантайм-повышения нет, повышение — один раз при старте» выполняется, и
# Windows не показывает запрос UAC. Не хватает объявлений longPathAware и
# PerMonitorV2 из packaging/app.manifest: длинные пути (>260 символов, §5 «Пути»)
# и декларативное объявление DPI. Второе не критично: каркас выставляет
# Per-Monitor v2 вызовом SetProcessDpiAwarenessContext на старте, и манифест здесь
# только подстраховка. Первое — реальный пробел, и закрыть его должен тот, кто
# владеет манифестом и его встраиванием.
