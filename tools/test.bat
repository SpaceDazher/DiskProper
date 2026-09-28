@echo off
REM MrProper: запуск юнит-тестов. Использует тот же vcvars, что и build.bat,
REM иначе ctest.exe не оказывается в PATH.
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
if "%1"=="" set "1=Debug"

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VARS="
if exist "D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VARS=D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
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
  echo [test] КРИТИЧНО: не найден vcvars64.bat — C++ тулчейн не установлен.
  exit /b 9009
)
call "%VARS%" >nul

set "EXE=build\%1\%1\mrproper_unit_tests.exe"
if not exist "%EXE%" set "EXE=build\%1\mrproper_unit_tests.exe"
if not exist "%EXE%" (
  echo [test] не найден %EXE% — сначала tools\build.bat %1
  exit /b 2
)

"%EXE%"
exit /b %errorlevel%
