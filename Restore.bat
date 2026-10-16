@echo off
cd /d "%~dp0"
if "%~1"=="" (
  echo Drag your Chromatic backup .bin file onto Restore.bat.
  pause
  exit /b 1
)
KortaMod-Installer.exe --restore "%~1"
pause
