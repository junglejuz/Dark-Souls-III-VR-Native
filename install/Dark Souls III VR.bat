@echo off
rem Dark Souls III VR - starts the game in VR (starting it from Steam as usual plays the normal game).
if not exist "%~dp0DS3VR2" mkdir "%~dp0DS3VR2"
echo vr> "%~dp0DS3VR2\ds3vr2_launch_vr.flag"
start "" "steam://rungameid/374320"
