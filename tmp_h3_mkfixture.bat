@echo off
setlocal
set "ROOT=%TEMP%\mrproper-h3"
if exist "%ROOT%" rmdir /s /q "%ROOT%" >nul 2>&1
mkdir "%ROOT%\sub" 2>nul
mkdir "%ROOT%\sub2" 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -Command "$b=New-Object byte[] 4096; (New-Object System.Random 7).NextBytes($b); [IO.File]::WriteAllBytes(\"$env:TEMP\mrproper-h3\a.bin\",$b); [IO.File]::WriteAllBytes(\"$env:TEMP\mrproper-h3\sub\b.bin\",$b); [IO.File]::WriteAllBytes(\"$env:TEMP\mrproper-h3\sub2\c.bin\",$b)"
cmd.exe /c mklink /J "%ROOT%\loop" "%ROOT%"
cmd.exe /c mklink /J "%ROOT%\sub\loop" "%ROOT%"
cmd.exe /c mklink /J "%ROOT%\sub2\loop" "%ROOT%\sub"
echo ROOT=%ROOT%
dir /a "%ROOT%"
