<#
  Dark Souls III VR (native stereo) uninstaller. The installer copies it into the game folder (next to DarkSoulsIII.exe).
  Puts your own graphics settings back, removes the mod (its settings, logs and pictures in DS3VR2 too) and the shortcuts,
  and puts back anything the installer moved aside (another 3D or VR setup).
  Optional: -GameDir "<folder with DarkSoulsIII.exe>" -Yes (no questions) -KeepShortcuts -KeepSelf (practice runs)
#>
param([string]$GameDir, [switch]$Yes, [switch]$KeepShortcuts, [switch]$KeepSelf)

$ErrorActionPreference = 'Stop'
function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Fail([string]$text) { Write-Host ''; Write-Host "  STOPPED: $text" -ForegroundColor Red; Write-Host ''; exit 1 }
$Here = [IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)
$Marker = 'DS3VR2-M'
$Exe = 'DarkSoulsIII.exe'

Write-Host ''
Write-Host '  Dark Souls III VR - uninstaller' -ForegroundColor White
Write-Host '  -------------------------------'
if (-not $GameDir) {
    if (Test-Path -LiteralPath ([IO.Path]::Combine($Here, $Exe))) { $GameDir = $Here }
    else {
        try {
            $root = ((Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath) -replace '/', '\'
            $libs = @($root)
            $vdf = [IO.Path]::Combine($root, 'steamapps\libraryfolders.vdf')
            if (Test-Path -LiteralPath $vdf) { foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\\\', '\') } }
            foreach ($l in $libs) {
                $d = [IO.Path]::Combine($l, 'steamapps\common\DARK SOULS III\Game')
                if (Test-Path -LiteralPath ([IO.Path]::Combine($d, $Exe))) { $GameDir = $d; break }
            }
        } catch { }
    }
}
if (-not $GameDir -or -not (Test-Path -LiteralPath ([IO.Path]::Combine($GameDir, $Exe)))) { Fail 'the game was not found. Run this from the game folder (next to DarkSoulsIII.exe).' }
if (Get-Process -Name 'DarkSoulsIII' -ErrorAction SilentlyContinue) { Fail 'Dark Souls III is running. Close it first.' }
$GameDir = [IO.Path]::GetFullPath($GameDir)
$GameRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($GameDir, '..'))
$bk = [IO.Path]::Combine($GameRoot, 'DS3VR-Native_backup')
$data = [IO.Path]::Combine($GameDir, 'DS3VR2')
Say "  Game: $GameDir"
if (-not $Yes) {
    $a = Read-Host '  Remove Dark Souls III VR? (y/n)'
    if ($a -notmatch '^[yY]') { Say '  Nothing changed.'; exit 0 }
}

# ---- your graphics settings: the normal game's copy goes back in place ----------------------------------
# The mod swaps the game's GraphicsConfig.xml between a VR copy and your own (DS3VR2\profiles). If the VR copy is the one in
# place (the last start was in VR), your own goes back: the mod's saved copy of it, else the one saved at install.
$live = [IO.Path]::Combine($env:APPDATA, 'DarkSoulsIII\GraphicsConfig.xml')
$last = [IO.Path]::Combine($data, 'profiles\last.txt')
$liveIsVr = (Test-Path -LiteralPath $last) -and ((Get-Content -LiteralPath $last -Raw) -match '^\s*vr')
if ($liveIsVr) {
    $own = [IO.Path]::Combine($data, 'profiles\2d\GraphicsConfig.xml')
    if (-not (Test-Path -LiteralPath $own)) { $own = [IO.Path]::Combine($bk, 'GraphicsConfig.xml') }
    if (Test-Path -LiteralPath $own) {
        try { Copy-Item -LiteralPath $own -Destination $live -Force }
        catch { Fail ("Windows did not allow your graphics settings file $live to be put back. Your own copy is $own - " +
                      'copy it back by hand, then run the uninstaller again. Nothing was removed.') }
        Say '  Your own graphics settings are back.' Green
    } else {
        Remove-Item -LiteralPath $live -Force -ErrorAction SilentlyContinue
        Say '  No copy of your own graphics settings was found: the game makes fresh ones at its next start.' Yellow
    }
} else {
    Say '  Your own graphics settings are already in place (the last start was a normal one).'
}

# ---- the mod's files ------------------------------------------------------------------------------
$din = [IO.Path]::Combine($GameDir, 'dinput8.dll')
if ((Test-Path -LiteralPath $din) -and ([IO.File]::ReadAllText($din, [Text.Encoding]::GetEncoding(28591))).Contains($Marker)) { Remove-Item -LiteralPath $din -Force }
foreach ($f in 'Dark Souls III VR.bat', 'DS3VR2-Launch-VR.bat') { $p = [IO.Path]::Combine($GameDir, $f); if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }
if (Test-Path -LiteralPath $data) { Remove-Item -LiteralPath $data -Recurse -Force }
Say '  Mod files, its VR settings, logs and pictures removed.' Green

# ---- what the installer moved aside ---------------------------------------------------------------
$prev = [IO.Path]::Combine($bk, 'previous_setup')
if (Test-Path -LiteralPath $prev) {
    foreach ($item in Get-ChildItem -LiteralPath $prev) {
        $dst = [IO.Path]::Combine($GameDir, $item.Name)
        if (-not (Test-Path -LiteralPath $dst)) { Move-Item -LiteralPath $item.FullName -Destination $dst }
    }
    Say '  The 3D / VR setup that was in the game folder before is back.' Green
}

# ---- shortcuts and the backup folder ------------------------------------------------------------------
if (-not $KeepShortcuts) {
    $p = [IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), 'Dark Souls III VR.lnk'); if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    $start = [IO.Path]::Combine([Environment]::GetFolderPath('Programs'), 'Dark Souls III VR')
    if (Test-Path -LiteralPath $start) { Remove-Item -LiteralPath $start -Recurse -Force }
}
if (Test-Path -LiteralPath $bk) { Remove-Item -LiteralPath $bk -Recurse -Force }

Write-Host ''
Write-Host '  Dark Souls III VR is removed. Starting the game from Steam plays the normal game.' -ForegroundColor White
Write-Host ''
if (-not $KeepSelf -and $Here -eq $GameDir) { Remove-Item -LiteralPath $MyInvocation.MyCommand.Path -Force -ErrorAction SilentlyContinue }
