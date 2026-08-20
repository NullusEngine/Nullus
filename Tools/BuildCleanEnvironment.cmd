@echo off
setlocal

rem Run a build command after removing the duplicate Windows PATH/Path key.
rem The PowerShell helper only changes the child process environment; it does
rem not modify the user's system or user PATH settings.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0BuildCleanEnvironment.ps1" %*
exit /b %ERRORLEVEL%
