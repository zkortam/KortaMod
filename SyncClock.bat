@echo off
cd /d "%~dp0"
KortaMod-Installer.exe --sync-clock
if errorlevel 1 (echo Clock sync stopped. Read the message above.) else (echo Clock synced.)
pause
