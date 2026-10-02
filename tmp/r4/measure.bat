@echo off
REM R4: замер Rule::excluded на реальном прогоне. Профиль включается переменной
REM окружения MRPROPER_SCAN_PROFILE, сборщик дописывает <путь>.collect.
setlocal
set "OUT=%~1"
set "MRPROPER_SCAN_PROFILE=%OUT%"
echo [%OUT%] start %DATE% %TIME%
build\a4\Release\mrproper_cli.exe scan --json --quiet --rules rules > "%OUT%.json" 2> "%OUT%.err"
echo [%OUT%] scan exit %ERRORLEVEL%
echo [%OUT%] done %DATE% %TIME%
endlocal
