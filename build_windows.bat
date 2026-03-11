@echo off
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set PLATFORM=%2
if "%PLATFORM%"=="" set PLATFORM=x64

if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo Usage: build_windows.bat [Debug^|Release] [x64^|ARM64]
    exit /b 1
)

if /I not "%PLATFORM%"=="x64" if /I not "%PLATFORM%"=="ARM64" (
    echo Usage: build_windows.bat [Debug^|Release] [x64^|ARM64]
    exit /b 1
)

cmake -S . -B build -G "Visual Studio 17 2022" -A %PLATFORM%
cmake --build build --config %CONFIG%

endlocal
