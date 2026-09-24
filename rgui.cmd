@echo off
rem Runs rgui.ps1 without depending on the PowerShell execution policy.
rem   rgui dev     rgui test     rgui full     rgui doctor
setlocal
rem Started from PowerShell 7 (pwsh, as on GitHub's runners), Windows
rem PowerShell 5.1 inherits pwsh's PSModulePath and then cannot load its
rem own modules: Get-FileHash, Get-CimInstance ... go missing.  Cleared,
rem it rebuilds the default.
set "PSModulePath="
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0rgui.ps1" %*
exit /b %ERRORLEVEL%
