@echo off
REM Glazeline build script. Run from a "Developer Command Prompt for VS"
REM (or run vcvarsall.bat x64 first) so that cl.exe is on PATH.

setlocal

if "%VSINSTALLDIR%"=="" (
    echo.
    echo ERROR: Visual Studio environment is not initialized.
    echo Open a "Developer Command Prompt for VS 2022" and re-run this script,
    echo or run "vcvarsall.bat x64" first.
    echo.
    exit /b 1
)

if not exist build mkdir build

cl /nologo /EHsc /std:c++17 ^
   /Fo:build\main.obj /Fe:build\glazeline.exe ^
   src\main.cpp ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib

if errorlevel 1 (
    echo.
    echo Build FAILED.
    exit /b 1
)

echo.
echo Build OK: build\glazeline.exe
