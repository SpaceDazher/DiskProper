@echo off
REM R2: сборка и запуск пробы форм множественного числа.
REM Формы ключа каталога вырезаны из src\ui\locale.hpp в tmp\r2_rule_key_gen.hpp.
setlocal EnableExtensions
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl.exe /nologo /std:c++20 /EHsc /W4 /WX /permissive- /utf-8 /Zc:__cplusplus ^
  /DR2_RULE_KEY_HEADER=\"r2_rule_key_gen.hpp\" ^
  /I tmp /I src /Fo:tmp\ /Fe:tmp\r2_plural_probe.exe ^
  tmp\r2_plural_probe.cpp src\core\units.cpp src\core\i18n.cpp
if errorlevel 1 exit /b 1
tmp\r2_plural_probe.exe
exit /b %errorlevel%
