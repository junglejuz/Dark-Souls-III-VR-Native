@echo off
REM Dark Souls III VR uninstaller (lives in the game folder after installing).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-DS3VR-Native.ps1" %*
pause
if not exist "%~dp0Uninstall-DS3VR-Native.ps1" (goto) 2>nul & del "%~f0"
