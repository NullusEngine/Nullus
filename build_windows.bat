@echo off
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set PLATFORM=%2
if "%PLATFORM%"=="" set PLATFORM=x64
set BUILD_DIR=build\windows

if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo Usage: build_windows.bat [Debug^|Release] [x64^|ARM64]
    exit /b 1
)

if /I not "%PLATFORM%"=="x64" if /I not "%PLATFORM%"=="ARM64" (
    echo Usage: build_windows.bat [Debug^|Release] [x64^|ARM64]
    exit /b 1
)

cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A %PLATFORM% -DCMAKE_CONFIGURATION_TYPES=%CONFIG%
if errorlevel 1 exit /b %errorlevel%

if exist %BUILD_DIR%\Tools\MetaParser\src\MetaParser.csproj (
    dotnet restore %BUILD_DIR%\Tools\MetaParser\src\MetaParser.csproj
    if errorlevel 1 exit /b %errorlevel%
)

if defined NLS_BUILD_TARGETS (
    cmake --build %BUILD_DIR% --config %CONFIG% --target %NLS_BUILD_TARGETS%
) else (
    cmake --build %BUILD_DIR% --config %CONFIG%
)
if errorlevel 1 exit /b %errorlevel%

endlocal
