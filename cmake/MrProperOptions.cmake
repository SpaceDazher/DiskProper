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
set(MRPROPER_SANITIZE "$ENV{MRPROPER_SANITIZE}" CACHE STRING
    "Санитайзеры: OFF | address | undefined | address,undefined (по умолчанию OFF)")
option(MRPROPER_INSTALL_TESTS
    "Устанавливать тестовые исполняемые файлы вместе с программой" OFF)
option(MRPROPER_ENABLE_CPACK
    "Готовить конфигурацию CPack (NSIS/ZIP) для сборки установщика" ON)
set(MRPROPER_APP_TARGET "mrproper_app" CACHE STRING
    "Имя цели приложения, к которой подключаются манифест UAC и ресурс версии")

# Значение можно задать тремя способами, по убыванию приоритета:
#   1) -DMRPROPER_SANITIZE=...   (командная строка, в том числе из пресета);
#   2) переменная окружения MRPROPER_SANITIZE — нужно, чтобы включить
#      санитайзеры через tools\build.bat, который передаёт только -DCMAKE_BUILD_TYPE;
#   3) пусто, то есть выключено.
# --- Санитайзеры: нормализация значения -------------------------------------
# Заглавные буквы, пробелы и «address+undefined» приводим к «address,undefined»,
# чтобы опечатка в строке не приводила к молчаливой сборке без проверок.
if(NOT MRPROPER_SANITIZE)
    set(MRPROPER_SANITIZE "OFF")
endif()
# Пробелы и перевод строки убираются регулярным выражением, а не заменой
# одного символа: значение может прийти из переменной окружения, где
# «set X=address &&» оставляет хвостовой пробел, и такой мусор иначе тихо
# превращается в «санитайзеры выключены».
string(TOLOWER "${MRPROPER_SANITIZE}" _mrproper_sanitize)
string(REGEX REPLACE "[ \t\r\n]+" "" "${_mrproper_sanitize}" _mrproper_sanitize)
string(REPLACE "+" "," "${_mrproper_sanitize}" _mrproper_sanitize)
string(REPLACE ";" "," "${_mrproper_sanitize}" _mrproper_sanitize)
set(MRPROPER_SANITIZE "${_mrproper_sanitize}")

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
        "(санитайзированная сборка не годится для поставки, только для проверки)")
endif()
