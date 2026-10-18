@echo off
cd /d "%~dp0"
echo This puts ModRetro's official firmware back on your Chromatic (a backup is made first; your settings stay).
set /p answer=Continue? [y/N] 
if /i not "%answer%"=="y" (
  echo Nothing was changed.
  pause
  exit /b 0
)
KortaMod-Installer.exe --stock
pause
