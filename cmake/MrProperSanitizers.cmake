# ---------------------------------------------------------------------------
# Санитайзеры. Владелец: W01/задача 01.
# Спека: SPEC §8 Этап 0 (проверяемая сборка), §11 (тесты).
#
# Включаются опцией MRPROPER_SANITIZE и всегда выключены по умолчанию: обычная
# сборка идёт без них, а проверочная (asan-debug / wsl-*) — с ними. Набор
# проверок всегда сверяется с тем, что реально умеет компилятор, иначе флаг
# был бы проглочен молча, а обманчивый «зелёный» прогон хуже явной ошибки.
#
# Совместимость:
#   MSVC   — /fsanitize=address с 19.29 (VS 2019 16.9),
#            /fsanitize=undefined с 19.36 (VS 2022 17.6).
#            ASan у MSVC требует /INCREMENTAL:NO и несовместим с /RTC.
#   GCC/Clang — address и undefined доступны везде.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

# Разбирает MRPROPER_SANITIZE на реально доступный набор флагов.
# Печатает предупреждение о потере части проверок, но не падает: отключённый
# санитайзер — потеря покрытия, а не ошибка сборки. Падает только явный запрос
# на то, чего компилятор не умеет вовсе.
function(mrproper_sanitizer_options out_compile out_link)
    set(${out_compile} "" PARENT_SCOPE)
    set(${out_link} "" PARENT_SCOPE)
    if(MRPROPER_SANITIZE STREQUAL "OFF")
        return()
    endif()

    string(REPLACE "," ";" _requested "${MRPROPER_SANITIZE}")
    set(_compile "")
    set(_link "")
    set(_dropped "")

    if(MSVC)
        foreach(_san IN LISTS _requested)
            if(_san STREQUAL "address")
                if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.29)
                    message(FATAL_ERROR
                        "MRPROPER_SANITIZE=address требует MSVC 19.29+ (VS 2019 16.9+); "
                        "сейчас ${CMAKE_CXX_COMPILER_VERSION}")
                endif()
                list(APPEND _compile /fsanitize=address /INCREMENTAL:NO)
                list(APPEND _link /fsanitize=address /INCREMENTAL:NO)
                # C5072: оптимизатор сообщает, что ASan запретил встраивание
                # функции. Это не дефект кода, а прямое следствие проверки, но
                # проект собирает с /WX, поэтому диагностика гасится явно.
                list(APPEND _compile /wd5072)
            elseif(_san STREQUAL "undefined")
                if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.36)
                    list(APPEND _dropped "undefined (MSVC ${CMAKE_CXX_COMPILER_VERSION} не поддерживает)")
                else()
                    list(APPEND _compile /fsanitize=undefined)
                    list(APPEND _link /fsanitize=undefined)
                endif()
            endif()
        endforeach()
    else()
        foreach(_san IN LISTS _requested)
            list(APPEND _compile -fsanitize=${_san})
            list(APPEND _link -fsanitize=${_san})
        endforeach()
        list(APPEND _compile -fno-omit-frame-pointer -fno-sanitize-recover=all)
    endif()

    if(_dropped)
        string(REPLACE ";" ", " _dropped_text "${_dropped}")
        message(WARNING
            "[MrProper] запрошенные санитайзеры отброшены компилятором: ${_dropped_text}")
    endif()
    if(MSVC AND _compile MATCHES "fsanitize=address")
        # Рантайм ASan в Visual Studio 2019 (19.29-19.35) помечен preview: на
        # проверенной машине (VS 2019 16.11) и отладочный, и динамический
        # рантайм не инициализируются, процесс падает с 0xC0000142 без
        # единого сообщения. Предупреждение на этапе configure честнее, чем
        # запуск собранной программы с пустым выводом.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.36)
            message(WARNING
                "[MrProper] MSVC ${CMAKE_CXX_COMPILER_VERSION} (Visual Studio 2019): "
                "рантайм AddressSanitizer помечен preview и может не инициализироваться "
                "(процесс завершится с 0xC0000142). Надёжная проверка санитайзеров — "
                "пресеты wsl-debug / wsl-release с GCC или Clang, либо Visual Studio 2022 17.x")
        endif()
    endif()
    set(${out_compile} "${_compile}" PARENT_SCOPE)
    set(${out_link} "${_link}" PARENT_SCOPE)
endfunction()

# Включает санитайзеры для всех целей текущего каталога и вложенных. Флаги идут
# и в компиляцию, и в компоновку: без линковки рантайм ASan просто не
# подтянется. Уровень каталога выбран по той же причине, что и у предупреждений:
# новый слой получает проверки без правки собственного CMakeLists.txt.
function(mrproper_enable_sanitizers_to_directory)
    if(MRPROPER_SANITIZE STREQUAL "OFF")
        return()
    endif()
    mrproper_sanitizer_options(_compile _link)
    if(NOT _compile)
        return()
    endif()
    add_compile_options(${_compile})
    if(_link)
        add_link_options(${_link})
    endif()
    if(MSVC AND _compile MATCHES "fsanitize=address")
        # ASan у MSVC требует отключённой инкрементальной компоновки. Флаг
        # /INCREMENTAL:NO в командной строке генератор Visual Studio
        # отбрасывает (он управляет свойством LinkIncremental), поэтому
        # отключаем инкрементальность свойством каталога: иначе линковщик
        # пропускает инициализацию ASan и процесс падает с 0xC0000142.
        set_property(DIRECTORY PROPERTY LINK_INCREMENTAL FALSE)
    endif()
endfunction()
