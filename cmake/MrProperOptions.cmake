# ---------------------------------------------------------------------------
# Опции проекта MrProper. Владелец: W01/задача 01.
# Спека: SPEC §8 Этап 0 (укрепление системы сборки).
# Все опции — с префиксом MRPROPER_, чтобы не конфликтовать с переменными
# CMake и с переменными чужих модулей. Значения по умолчанию подобраны так,
# чтобы tools\build.bat Debug и tools\test.bat Debug работали без аргументов.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

option(MRPROPER_BUILD_TESTS
    "Собирать тесты (tests/) и регистрировать их в CTest" ON)
option(MRPROPER_CHECK_TOOLCHAIN
    "Проверять на этапе configure, что тулчейн умеет нужное (stop_token, format, filesystem, ranges)" ON)
option(MRPROPER_WARNINGS_AS_ERRORS
    "Предупреждения компилятора — ошибка (/WX, -Werror), SPEC §9.1 ADR-001" ON)
option(MRPROPER_STRICT_WARNINGS
    "Строгий набор предупреждений сверх /W4 (-- GCC/Clang: -Wshadow, -Wconversion)" OFF)
option(MRPROPER_INSTALL_TESTS
    "Устанавливать тестовые исполняемые файлы вместе с программой" OFF)
option(MRPROPER_ENABLE_CPACK
    "Готовить конфигурацию CPack (NSIS/ZIP) для сборки установщика" ON)
set(MRPROPER_APP_TARGET "mrproper_app" CACHE STRING
    "Имя цели приложения, к которой подключаются манифест UAC и ресурс версии")

# --- Внешние зависимости (SPEC §8 Этап 0: vcpkg или FetchContent) ---------
# Подробности и точка входа — cmake/MrProperDependencies.cmake.
set(MRPROPER_DEPS_PROVIDER "auto" CACHE STRING
    "Как добываются внешние зависимости: auto | vcpkg | fetchcontent | none")
option(MRPROPER_ALLOW_NETWORK
    "Разрешать FetchContent скачивать зависимости во время configure" OFF)

# Значение можно задать тремя способами, по убыванию приоритета:
#   1) переменная окружения MRPROPER_SANITIZE — нужно, чтобы включить
#      санитайзеры через tools\build.bat, который передаёт только
#      -DCMAKE_BUILD_TYPE;
#   2) -DMRPROPER_SANITIZE=... (командная строка, в том числе из пресета);
#   3) пусто, то есть выключено.
#
# Переменная окружения НЕ записывается в кэш — иначе один прогон
# «set MRPROPER_SANITIZE=address && tools\build.bat Debug» навсегда оставил бы
# каноническое дерево build\Debug под санитайзером: в кэше лежал бы
# «address», ключ set(CACHE) без FORCE его бы не перебил, а следующий
# запуск build.bat уже без переменной окружения собирал бы с санитайзерами
# и никто бы об этом не узнал. Здесь переменная окружения перекрывает кэш
# как обычная переменная, а кэш хранит только то, что задал человек явно.
set(MRPROPER_SANITIZE "" CACHE STRING
    "Санитайзеры: OFF | address | undefined | address,undefined (по умолчанию OFF)")

if(DEFINED ENV{MRPROPER_SANITIZE} AND NOT "$ENV{MRPROPER_SANITIZE}" STREQUAL "")
    set(_mrproper_sanitize_effective "$ENV{MRPROPER_SANITIZE}")
else()
    set(_mrproper_sanitize_effective "${MRPROPER_SANITIZE}")
endif()
# --- Санитайзеры: нормализация значения -------------------------------------
# Заглавные буквы, пробелы и «address+undefined» приводим к «address,undefined»,
# чтобы опечатка в строке не приводила к молчаливой сборке без проверок.
string(STRIP "${_mrproper_sanitize_effective}" _mrproper_sanitize)
if(NOT _mrproper_sanitize)
    set(_mrproper_sanitize "OFF")
endif()
# Пробелы, табуляции и переводы строк убираются точечными заменами, а НЕ
# регулярным выражением «[ \t\r\n]+»: в CMake 3.20 класс символов с \r\n
# внутри не срабатывает — выражение компилируется, но не совпадает ни с
# одним символом, и хвостовой пробел от «set X=address && …» доходил до
# проверки как «address », то есть весь путь через переменную окружения
# (единственный способ включить санитайзеры в tools\build.bat, который
# передаёт только -DCMAKE_BUILD_TYPE) падал с ошибкой. Молча не сработавшая
# нормализация опаснее явной ошибки, поэтому здесь только строковые замены,
# в которых результат предсказуем на любой версии CMake.
string(TOLOWER "${_mrproper_sanitize}" _mrproper_sanitize)
foreach(_ws IN ITEMS " " "\t" "\r" "\n")
    string(REPLACE "${_ws}" "" _mrproper_sanitize "${_mrproper_sanitize}")
endforeach()
string(REPLACE "+" "," "${_mrproper_sanitize}" _mrproper_sanitize)
string(REPLACE ";" "," "${_mrproper_sanitize}" _mrproper_sanitize)
# Итог — обычная переменная: она перекрывает кэш в этой конфигурации, и
# все, кто читает MRPROPER_SANITIZE дальше, видят одно и то же значение.
set(MRPROPER_SANITIZE "${_mrproper_sanitize}")
unset(_mrproper_sanitize_effective)

set(_mrproper_sanitize_ok TRUE)
string(REPLACE "," ";" _mrproper_sanitize_list "${MRPROPER_SANITIZE}")
foreach(_san IN LISTS _mrproper_sanitize_list)
    if(_san)
        if(NOT _san STREQUAL "address" AND NOT _san STREQUAL "undefined")
            set(_mrproper_sanitize_ok FALSE)
        endif()
    endif()
endforeach()
if(NOT _mrproper_sanitize_ok)
    message(FATAL_ERROR
        "MRPROPER_SANITIZE=\"${MRPROPER_SANITIZE}\" недопустимо. "
        "Допустимо: OFF, address, undefined, address,undefined")
endif()
if(NOT MRPROPER_SANITIZE STREQUAL "OFF")
    message(STATUS
        "[MrProper] санитайзеры включены: ${MRPROPER_SANITIZE} "
        "(санитайзированная сборка не годится для поставки, только для проверки; "
        "в кэш значение не попало — обычная tools\\build.bat Debug соберётся без санитайзеров)")
endif()
