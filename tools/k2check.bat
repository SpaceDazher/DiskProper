@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl.exe /nologo /Zs /std:c++20 /EHsc /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /wd4996 /wd5105 /DUNICODE /D_UNICODE /I src /I src/ui %*
