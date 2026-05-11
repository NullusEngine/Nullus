@echo off
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set PLATFORM=%2
if "%PLATFORM%"=="" set PLATFORM=x64
set BUILD_DIR=build\windows
set MSBUILD_JOBS=%NUMBER_OF_PROCESSORS%
if "%MSBUILD_JOBS%"=="" set MSBUILD_JOBS=4
set MSBUILD_PARALLEL=/m:%MSBUILD_JOBS%
if defined NLS_BUILD_JOBS set MSBUILD_PARALLEL=/m:%NLS_BUILD_JOBS%
echo [Build] MSBuild parallelism: %MSBUILD_PARALLEL%

set CMAKE_BUILD_OPTIONS=-DCMAKE_CONFIGURATION_TYPES=%CONFIG%
if defined NLS_MSVC_MP_JOBS (
    set CMAKE_BUILD_OPTIONS=%CMAKE_BUILD_OPTIONS% -DNLS_MSVC_MP_JOBS=%NLS_MSVC_MP_JOBS%
    echo [Build] MSVC /MP jobs: %NLS_MSVC_MP_JOBS%
)

if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo Usage: build_windows.bat [Debug^|Release] [x64^|ARM64]
    exit /b 1
)

if /I not "%PLATFORM%"=="x64" if /I not "%PLATFORM%"=="ARM64" (
    echo Usage: build_windows.bat [Debug^|Release] [x64^|ARM64]
    exit /b 1
)

cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A %PLATFORM% %CMAKE_BUILD_OPTIONS%
if errorlevel 1 exit /b %errorlevel%

if exist %BUILD_DIR%\Tools\MetaParser\src\MetaParser.csproj (
    dotnet restore %BUILD_DIR%\Tools\MetaParser\src\MetaParser.csproj
    if errorlevel 1 exit /b %errorlevel%
)

if defined NLS_BUILD_TARGETS (
    cmake --build %BUILD_DIR% --config %CONFIG% --target %NLS_BUILD_TARGETS% -- %MSBUILD_PARALLEL%
) else (
    cmake --build %BUILD_DIR% --config %CONFIG% -- %MSBUILD_PARALLEL%
)
if errorlevel 1 exit /b %errorlevel%

endlocal
