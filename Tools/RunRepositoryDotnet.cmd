@echo off
setlocal

rem The first argument is the repository-local dotnet.exe. Remaining arguments
rem are forwarded unchanged after Path/PATH normalization.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunRepositoryDotnet.ps1" %*
exit /b %ERRORLEVEL%
