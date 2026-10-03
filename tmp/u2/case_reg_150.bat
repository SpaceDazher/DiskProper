@echo off
cd /d D:\Project\MrProper
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-smoke.ps1 -Exe build\u2\src\ui\Debug\mrproper.exe -Shot D:\Temp\u2tmp\reg_150.png -ThemeMode dark -ExpectBackground dark -Width 1280 -Height 720 -DpiPercent 150 > D:\Project\MrProper\tmp\u2\reg_150.log 2>&1
exit /b %errorlevel%
