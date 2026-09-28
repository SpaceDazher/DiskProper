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
#            ASan у MSVC требует отключённой инкрементальной компоновки.
#            В линковку /fsanitize=address не передаётся: link.exe его не
#            знает (LNK4044). Рантайм подтягивает линкер сам.
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
                # /fsanitize=address — флаг КОМПИЛЯТОРА, и только его. В компоновку
                # он не передаётся: link.exe 19.29 его не знает и печатает
                # «LNK4044: нераспознанный параметр /fsanitize=address;
                # игнорируется» на каждый линкующий проект. Рантайм ASan и так
                # подтягивается автоматически: компилятор встраивает ссылку на
                # него в объектные файлы, а линкер разрешает её сам — при условии
                # отключённой инкрементальной компоновки.
                list(APPEND _compile /fsanitize=address)
                # /INCREMENTAL:NO — реальное требование линковщика: с
                # инкрементальной компоновкой ASan не инициализируется.
                list(APPEND _link /INCREMENTAL:NO)
                # C5072: оптимизатор сообщает, что ASan запретил встраивание
                # функции. Это не дефект кода, а прямое следствие проверки, но
                # проект собирает с /WX, поэтому диагностика гасится явно.
                list(APPEND _compile /wd5072)
            elseif(_san STREQUAL "undefined")
                if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.36)
                    list(APPEND _dropped "undefined (MSVC ${CMAKE_CXX_COMPILER_VERSION} не поддерживает)")
                else()
                    list(APPEND _compile /fsanitize=undefined)
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
        # проверенной машине (MSVC 19.29, VS 2019 16.11) проект собирается, но
        # процесс не стартует — выход с кодом 66 (STATUS_DLL_INIT_FAILED) и
        # пустым выводом, даже если подложить clang_rt.asan*.dll рядом с exe.
        # Предупреждение на этапе configure честнее, чем запуск собранной
        # программы с пустым выводом, который выглядит как «тесты не нашлись».
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.36)
            message(WARNING
                "[MrProper] MSVC ${CMAKE_CXX_COMPILER_VERSION} (Visual Studio 2019): "
                "рантайм AddressSanitizer помечен preview. Сборка пройдёт, но процесс "
                "не запустится — код 66 (STATUS_DLL_INIT_FAILED) и пустой вывод "
                "(проверено на этой машине: tools\\test.bat Debug, 19.29.30159.0). "
                "Надёжная проверка санитайзеров — пресеты wsl-debug / wsl-release "
                "с GCC или Clang, либо Visual Studio 2022 17.x")
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
        # ASan у MSVC требует отключённой инкрементальной компоновки.
        #
        # Свойство ставится на цели явно, а не через свойство каталога
        # LINK_INCREMENTAL и не флагом /INCREMENTAL:NO. На этой машине
        # (CMake 3.20, генератор Visual Studio 16 2019) проверено, что оба
        # способа не доходят до .vcxproj — в Link/AdditionalOptions флага нет,
        # а <LinkIncremental> остаётся пустым элементом, значение которого
        # зависит от того, как его трактует MSBuild, то есть от версии
        # MSBuild. Цикл по фактическим целям бьёт по свойству цели напрямую —
        # это единственный путь, который генератор записывает в файл как есть.
        # Инерционность включительно, а не выключительно: если у слоя
        # переопределена своя, её решение должно выиграть.
        get_property(_mrproper_asan_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _mrproper_asan_targets)
            get_target_property(_target_incremental ${_target} LINK_INCREMENTAL)
            if(NOT _target_incremental)
                set_target_properties(${_target} PROPERTIES LINK_INCREMENTAL OFF)
            endif()
        endforeach()
    endif()
endfunction()
