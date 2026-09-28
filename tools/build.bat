@echo off
REM MrProper: сборка через MSVC. Используется любой доступный vcvars64,
REM сперва 17.x (D:\BuildTools), затем 14.29 (VS 2019 Build Tools).
setlocal EnableDelayedExpansion
cd /d "%~dp0.."

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VARS="

REM 1) Preferred: VS 2022 Build Tools at D:\BuildTools
if exist "D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VARS=D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

REM 2) Fallback: any Build Tools instance that actually ships VC.Tools
if not defined VARS (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
      set "VARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
      goto :got
    )
  )
)

:got
if not defined VARS (
  echo [build] КРИТИЧНО: не найден vcvars64.bat — C++ тулчейн не установлен.
  echo [build] См. SPEC.md раздел 8, Этап 0.0.
  exit /b 9009
)

echo [build] vcvars: %VARS%
call "%VARS%" >nul
if errorlevel 1 (
  echo [build] vcvars64 завершился с ошибкой.
  exit /b 1
)

REM CMake берётся из состава Visual Studio: vcvars добавляет его в PATH.
REM Portable CMake 4.x в D:\Tools использовать нельзя — распаковка на NTFS
REM из WSL недопустимо медленная, а распакованный архив оказался без Modules.
for /f "delims=" %%v in ('cmake --version ^| findstr /b "cmake"') do echo [build] %%v

cmake -S . -B build\%1 -DCMAKE_BUILD_TYPE=%1 || exit /b 1
cmake --build build\%1 --config %1 --parallel || exit /b 1
echo [build] ok
exit /b 0
