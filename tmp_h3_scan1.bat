@echo off
D:\Project\MrProper\build\a3\Debug\mrproper_cli.exe scan --json --rules D:\Project\MrProper\rules --quiet > %TEMP%\h3-scan-1.json 2> %TEMP%\h3-scan-1.err
echo EXIT=%errorlevel%
