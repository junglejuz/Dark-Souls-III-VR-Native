@echo off
REM Dark Souls III VR installer: runs Install-DS3VR-Native.ps1 from this folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-DS3VR-Native.ps1" %*
pause
