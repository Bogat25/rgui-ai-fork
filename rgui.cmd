@echo off
rem Runs rgui.ps1 without depending on the PowerShell execution policy.
rem   rgui dev     rgui test     rgui full     rgui doctor
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0rgui.ps1" %*
exit /b %ERRORLEVEL%
