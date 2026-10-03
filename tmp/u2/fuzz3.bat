@echo off
setlocal enabledelayedexpansion
set "MRPROPER_TEST_FILTER=vfsEdge_longPath_tree_beyond_max_path_is_visible"
set /a bad=0
for /L %%i in (1,1,%2) do (
  "%~1" > "D:\Temp\u2tmp\fz.log" 2>&1
  if errorlevel 1 (
    set /a bad+=1
    if !bad! LEQ 2 copy /y "D:\Temp\u2tmp\fz.log" "D:\Temp\u2tmp\fz_red_%%~nx1_%%i.log" >nul
  )
)
echo [u2] BIN=%~1 runs=%2 RED=%bad%
