@echo off
rem Runs bootstrap.ps1 without changing the system's PowerShell execution policy.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bootstrap.ps1" %*
exit /b %ERRORLEVEL%
