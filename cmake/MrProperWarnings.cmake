# ---------------------------------------------------------------------------
# Единая настройка предупреждений. Владелец: W01/задача 01.
# Спека: SPEC §9.1 ADR-001 — предупреждения как ошибки.
#
# Модуль объявляет MRPROPER_WARNINGS_READY. Подкаталоги (src/core,
# src/platform, tests) проверяют эту переменную и уступают общему набору
# флагов, если она выставлена, — так флаги не накладываются дважды, а агент,
# создавший новый слой, получает ту же планку автоматически: флаги задаются на
# уровне каталога верхнего уровня и наследуются всеми подкаталогами.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

# Составляет список флагов предупреждений. Функция без побочных эффектов:
# вызывающий сам решает, применять их ко всем целям каталога или к одной.
function(mrproper_warning_options out_var)
    if(MSVC)
        set(_options /W4 /permissive- /Zc:__cplusplus)
        if(MRPROPER_WARNINGS_AS_ERRORS)
            list(APPEND _options /WX)
        endif()
        if(MRPROPER_STRICT_WARNINGS)
            # Преобразования типов и молчаливый fallthrough — источники самых
            # дорогих ошибок в коде, который работает с размерами и WinAPI.
            list(APPEND _options
                /w44242  # возможная потеря данных при преобразовании
                /w44263  # преобразование между знаковым и беззнаковым
                /w44264  # возможная потеря точности при преобразовании
                /w44265  # невыполненный fallthrough
                /w44266  # невыполненный переход в регистр
                /w44267  # неоднозначное преобразование в bool
                /w44268  # неоднозначный оператор сравнения
                /w44269  # неоднозначное смещение
                /w44270  # неоднозначное сложение со знаковым и беззнаковым
                /w44271  # неоднозначное умножение
                /w44272  # неоднозначный сдвиг отрицательного значения
            )
        endif()
    else()
        set(_options -Wall -Wextra -Wpedantic)
        if(MRPROPER_WARNINGS_AS_ERRORS)
            list(APPEND _options -Werror)
        endif()
        if(MRPROPER_STRICT_WARNINGS)
            list(APPEND _options -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wuseless-cast)
        endif()
    endif()
    set(${out_var} "${_options}" PARENT_SCOPE)
endfunction()

# Применяет флаги предупреждений ко всем целям текущего каталога и вложенных.
# Именно это делает общий уровень, а не каждый подкаталог по отдельности.
function(mrproper_apply_warnings_to_directory)
    mrproper_warning_options(_options)
    if(_options)
        add_compile_options(${_options})
    endif()
endfunction()

# Флаг готовности. Подкаталоги читают его в момент своей конфигурации, то есть
# раньше, чем выполнятся отложенные вызовы cmake_language(DEFER).
set(MRPROPER_WARNINGS_READY TRUE)
