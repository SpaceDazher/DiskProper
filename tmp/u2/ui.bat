@echo off
cd /d D:\Project\MrProper
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-smoke.ps1 -Exe %1 -Shot %2 -ThemeMode %3 -ExpectBackground dark -Width 900 -Height 600 -Page %4 > %5 2>&1
exit /b %errorlevel%
