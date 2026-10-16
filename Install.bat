@echo off
cd /d "%~dp0"
KortaMod-Installer.exe
if errorlevel 1 (echo Install stopped. Read the message above.) else (echo Install complete.)
pause
