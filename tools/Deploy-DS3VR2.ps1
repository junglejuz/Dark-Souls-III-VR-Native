# DS3VR2 deploy: puts the 2.0 build into Dark Souls III's folder, or takes it out again.
#   powershell -ExecutionPolicy Bypass -File ds3vr2\tools\Deploy-DS3VR2.ps1           # install (game must be closed)
#   powershell -ExecutionPolicy Bypass -File ds3vr2\tools\Deploy-DS3VR2.ps1 -Remove   # uninstall
# What it touches (also listed in ds3vr2\FIX_CHANGES.md):
#   <game>\dinput8.dll              our mod (anything already there is saved ONCE to <game>\DS3VR2\backup_prev)
#   <game>\DS3VR2\nvngx_dlss.dll    NVIDIA DLSS (from akvr\native\third_party\ngx\bin)
#   <game>\DS3VR2-Launch-VR.bat     the VR launcher (leaves the VR flag, then starts the game through Steam)
# A plain Steam start stays the normal 2D game.
param([switch]$Remove, [string]$Game = "E:\Games\Steam\steamapps\common\DARK SOULS III\Game")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (Get-Process DarkSoulsIII -ErrorAction SilentlyContinue) { throw "Dark Souls III is running - close it first." }
$data = Join-Path $Game "DS3VR2"
$bak = Join-Path $data "backup_prev"
New-Item -ItemType Directory -Force $bak | Out-Null
$live = Join-Path $Game "dinput8.dll"

if ($Remove) {
    if (Test-Path $live) { Remove-Item $live -Force }
    $prev = Join-Path $bak "dinput8.dll"
    if (Test-Path $prev) { Copy-Item $prev $live -Force; Write-Host "The dinput8.dll that was there before DS3VR2 is back." }
    else { Write-Host "DS3VR2 removed (there was no earlier dinput8.dll)." }
    return
}

$build = Join-Path $root "build\Release\dinput8.dll"
if (-not (Test-Path $build)) { throw "Build first: cmake --build ds3vr2\build --config Release --target ds3vr2" }
if ((Test-Path $live) -and -not (Test-Path (Join-Path $bak "dinput8.dll"))) {
    $desc = (Get-Item $live).VersionInfo.FileDescription
    if ($desc -ne "dinput8.dll" -and (Get-FileHash $live).Hash -ne (Get-FileHash $build).Hash -and -not (Test-Path (Join-Path $bak "dinput8.dll"))) { Copy-Item $live (Join-Path $bak "dinput8.dll"); Write-Host "Saved the dinput8.dll that was there to DS3VR2\backup_prev." }
}
Copy-Item $build $live -Force
$ngx = Join-Path $root "..\akvr\native\third_party\ngx\bin\nvngx_dlss.dll"
if ((Test-Path $ngx) -and -not (Test-Path (Join-Path $data "nvngx_dlss.dll"))) { Copy-Item $ngx $data }
$bat = @"
@echo off
rem Dark Souls III VR 2.0 - starts the game in VR (a plain Steam start stays the normal 2D game).
if not exist "%~dp0DS3VR2" mkdir "%~dp0DS3VR2"
echo vr> "%~dp0DS3VR2\ds3vr2_launch_vr.flag"
start "" "steam://rungameid/374320"
"@
Set-Content (Join-Path $Game "DS3VR2-Launch-VR.bat") $bat -Encoding ascii
$h = (Get-FileHash $live -Algorithm SHA256).Hash.Substring(0, 12)
Write-Host "DS3VR2 installed: dinput8.dll sha256 $h..., launcher DS3VR2-Launch-VR.bat"
