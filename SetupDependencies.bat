@echo off
setlocal

set "SCRIPT_DIR=%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
    py -3 "%SCRIPT_DIR%Tools\SetupDependencies\setup_dependencies.py" %*
    exit /b %errorlevel%
)

where python >nul 2>nul
if %errorlevel%==0 (
    python "%SCRIPT_DIR%Tools\SetupDependencies\setup_dependencies.py" %*
    exit /b %errorlevel%
)

echo Python 3.8 or newer is required to run SetupDependencies. Install Python 3.8+ or add it to PATH.
exit /b 1
