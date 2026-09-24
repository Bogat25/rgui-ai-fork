@echo off
rem ---------------------------------------------------------------
rem  Portable launcher for RGui with the local AI assistant.
rem
rem  Copy this file to the ROOT of the pendrive, so that it sits next
rem  to the R folder:
rem
rem      E:\Start-R.cmd     <- this file
rem      E:\R\              <- R_HOME (bin, etc, library, ai, ...)
rem      E:\work\           <- created on first run
rem
rem  Then double-click it.  Nothing is written to the machine's
rem  registry or user profile, and no administrator rights are needed.
rem ---------------------------------------------------------------
setlocal enableextensions

rem Folder this script lives in, without the trailing backslash.
set "BASE=%~dp0"
if "%BASE:~-1%"=="\" set "BASE=%BASE:~0,-1%"

set "R_HOME=%BASE%\R"
set "RGUI=%R_HOME%\bin\x64\Rgui.exe"

if not exist "%RGUI%" (
  echo.
  echo   Could not find:
  echo      %RGUI%
  echo.
  echo   Put Start-R.cmd in the same folder as the R directory.
  echo.
  pause
  exit /b 1
)

rem Keep the workspace, the history, the user library and R's temporary
rem files on the stick, so nothing is left behind on a shared machine
rem and the setup behaves the same on every computer.
set "R_USER=%BASE%\work"
set "HOME=%BASE%\work"
set "R_LIBS_USER=%BASE%\work\library"
set "TMPDIR=%BASE%\work\tmp"
set "TEMP=%TMPDIR%"
set "TMP=%TMPDIR%"

rem If the stick is slow and you do not mind temporary files on the host,
rem delete the three lines above that set TMPDIR, TEMP and TMP.

if not exist "%R_USER%"      md "%R_USER%"
if not exist "%R_LIBS_USER%" md "%R_LIBS_USER%"
if not exist "%TMPDIR%"      md "%TMPDIR%"

rem R_ENVIRON_USER and R_PROFILE_USER are pointed at the stick too, so a
rem stray .Renviron in the host's home directory cannot change anything.
set "R_ENVIRON_USER=%R_USER%\.Renviron"
set "R_PROFILE_USER=%R_USER%\.Rprofile"

start "" "%RGUI%" %*
endlocal
