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

rem Keep the workspace, the history and the user library on the stick, so
rem nothing is left behind on a shared machine and the setup behaves the
rem same on every computer.
set "R_USER=%BASE%\work"
set "HOME=%BASE%\work"
set "R_LIBS_USER=%BASE%\work\library"
if not exist "%R_USER%"      md "%R_USER%"
if not exist "%R_LIBS_USER%" md "%R_LIBS_USER%"

rem R's temporary files go on the stick too, but only if that path has no
rem spaces.  R stops with "'R_TempDir' contains space" otherwise, unless
rem Windows can give it an 8.3 short name without spaces, which it cannot
rem on exFAT sticks or drives with short names turned off.  In that case
rem TMP and TEMP stay as Windows set them, as for any normal R install.
set "RTMP=%BASE%\work\tmp"
if not exist "%RTMP%" md "%RTMP%"
set "RTMP_SHORT="
for %%I in ("%RTMP%") do set "RTMP_SHORT=%%~sI"
set "USE_RTMP="
if defined RTMP_SHORT if "%RTMP_SHORT%"=="%RTMP_SHORT: =%" set "USE_RTMP=%RTMP_SHORT%"
if defined USE_RTMP set "TMPDIR=%USE_RTMP%"
if defined USE_RTMP set "TMP=%USE_RTMP%"
if defined USE_RTMP set "TEMP=%USE_RTMP%"
if not defined USE_RTMP set "TMPDIR="

rem R_ENVIRON_USER and R_PROFILE_USER are pointed at the stick too, so a
rem stray .Renviron in the host's home directory cannot change anything.
set "R_ENVIRON_USER=%R_USER%\.Renviron"
set "R_PROFILE_USER=%R_USER%\.Rprofile"

start "" "%RGUI%" %*
endlocal
