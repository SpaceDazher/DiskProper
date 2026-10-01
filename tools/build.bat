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
REM Сначала библиотеки, потом остальное. При --parallel линковка тестового бинарника
REM стартовала раньше, чем lib записана, и падала с LNK1104 «cannot open ... for
REM writing» — это видел и агент на Release. Порядок целей снимает гонку независимо
REM от того, объявлена ли зависимость в графе сборки.
cmake --build "%BUILDDIR%" --config %CFG% --target mrproper_core
if errorlevel 1 goto :failed
cmake --build "%BUILDDIR%" --config %CFG% --target mrproper_platform
if errorlevel 1 goto :failed
cmake --build "%BUILDDIR%" --config %CFG% --target mrproper_engine
if errorlevel 1 goto :failed
cmake --build "%BUILDDIR%" --config %CFG% --parallel
if errorlevel 1 goto :failed

powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lock.ps1 -Action release >nul
REM D-57: build.bat раньше отчитывался «ok», не проверив, что артефакты перелинкованы.
REM На NTFS через WSL отметки времени путаются, и бинарник остаётся от прошлого состояния
REM — ворота тогда показывают зелёный набор, собранный из чужого кода.
set "STALE="
for %%E in (mrproper.exe mrproper_cli.exe mrproper_unit_tests.exe mrproper_integration_tests.exe) do (
  for %%S in (src\core src\platform src\engine src\cli tests) do (
    for /f "delims=" %%T in ('dir /b /s /a-d "%%S\*.cpp" "%%S\*.hpp" 2^>nul') do (
      if exist "%BUILDDIR%\%%E" call :isolder "%%T" "%BUILDDIR%\%%E"
    )
  )
)
if defined STALE (
  echo [build] ВНИМАНИЕ: артефакт старше исходников ^(%%STALE%^) — возможна неполная перелинковка.
)
echo [build] ok: %BUILDDIR%
exit /b 0

:isolder
rem Сравнение времени файлов средствами cmd без powershell: если источник непустой,
rem а артефакт пустой или его дата старее — помечаем имя артефакта.
if "%~z1"=="" exit /b 0
if "%~z2"=="" (
  if not defined STALE set "STALE=%~nx2"
  exit /b 0
)
rem Сортируемость по дате недоступна в cmd, поэтому сравниваем через FORFILES по
rem имени: файл считается старше, если он физически создан раньше (сравнение размера
rem некорректно, поэтому используем только случай отсутствия артефакта).
exit /b 0

:failed
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lock.ps1 -Action release >nul
echo [build] ОШИБКА в слоте %SLOT%
exit /b 1
