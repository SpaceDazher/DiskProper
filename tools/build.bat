@echo off
REM MrProper: сборка через MSVC.
REM   tools\build.bat <Debug|Release> [слот]
REM Слот — имя каталога сборки. У каждого агента он свой (например a56): пять
REM агентов в одном build\Debug гоняются за кэшем CMake и портят его друг другу.
REM Плюс сама компиляция сериализована tools\build_lock.ps1 — на хосте 7.8 ГБ RAM,
REM и пять параллельных cl.exe вызывают OOM, от которого воркфлоу пишет
REM "Thread interrupted because the connection to the host was lost".
setlocal EnableExtensions
cd /d "%~dp0.."

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"
set "SLOT=%~2"
if "%SLOT%"=="" set "SLOT=main"
set "BUILDDIR=build\%SLOT%"

REM --- поиск vcvars64.bat: сперва D:\BuildTools, затем vswhere, затем известные
REM --- пути. vswhere отдаёт пустоту, если каталог экземпляров Visual Studio
REM --- повреждён, а компилятор при этом жив, поэтому последний шаг не зависит
REM --- ни от того, ни от другого.
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VARS="
if exist "D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VARS=D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VARS if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
      set "VARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
      goto :gotvars
    )
  )
)
if not defined VARS for %%P in (
  "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  "C:\Program Files\Microsoft Visual Studio\2022\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
) do (
  if not defined VARS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VARS=%%~P\VC\Auxiliary\Build\vcvars64.bat"
)
:gotvars
    )
  )
)
:gotvars
if not defined VARS (
  echo [build] КРИТИЧНО: не найден vcvars64.bat — C++ тулчейн не установлен. SPEC.md раздел 8, Этап 0.0
  exit /b 9009
)
call "%VARS%" >nul

echo [build] слот %SLOT%, конфигурация %CFG%: жду очереди на компиляцию
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lock.ps1 -Action acquire
if errorlevel 3 exit /b 3
if errorlevel 1 (
  echo [build] не удалось взять замок сборки
  exit /b 1
)

cmake -S . -B "%BUILDDIR%" -DCMAKE_BUILD_TYPE=%CFG%
if errorlevel 1 goto :failed
cmake --build "%BUILDDIR%" --config %CFG% --parallel
if errorlevel 1 goto :failed

powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lock.ps1 -Action release >nul
echo [build] ok: %BUILDDIR%
exit /b 0

:failed
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lock.ps1 -Action release >nul
echo [build] ОШИБКА в слоте %SLOT%
exit /b 1
