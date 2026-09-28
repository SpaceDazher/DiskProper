@echo off
REM MrProper: запуск юнит-тестов.
REM   tools\test.bat <Debug|Release> [слот]   — слот тот же, что и в build.bat
setlocal EnableExtensions
cd /d "%~dp0.."

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"
set "SLOT=%~2"
if "%SLOT%"=="" set "SLOT=main"

set "EXE=build\%SLOT%\%CFG%\%CFG%\mrproper_unit_tests.exe"
if not exist "%EXE%" set "EXE=build\%SLOT%\%CFG%\mrproper_unit_tests.exe"
if not exist "%EXE%" (
  echo [test] не найден %EXE% — сначала tools\build.bat %CFG% %SLOT%
  exit /b 2
)

"%EXE%"
exit /b %errorlevel%
