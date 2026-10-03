@echo off
setlocal enabledelayedexpansion
set /a bad=0
for /L %%i in (1,1,%2) do (
  "%~1" > "D:\Temp\u2tmp\full.log" 2>&1
  if errorlevel 1 (
    set /a bad+=1
    if !bad! LEQ 1 copy /y "D:\Temp\u2tmp\full.log" "D:\Temp\u2tmp\full_red.log" >nul
  )
)
echo [u2] BIN=%~1 FULLRUNS=%2 RED=%bad%
