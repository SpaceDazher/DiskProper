@echo off
D:\Project\MrProper\build\a3\Debug\mrproper_cli.exe scan --json --rules %TEMP%\mrproper-h3-rules --quiet > %TEMP%\h3-loop.json 2> %TEMP%\h3-loop.err
echo EXIT=%errorlevel%
