<#
  Dark Souls III VR (native stereo) installer.

  Double-click Install-DS3VR-Native.bat. It finds the game, moves any other 3D / VR setup out of the game folder (geo-11, a
  3D fix, another mod's dinput8.dll - after asking; the uninstaller puts them back), copies the mod, copies the tested
  VR settings (first install only), sets the game's graphics options for VR in the mod's own VR copy of the game's
  settings (your normal 2D game keeps yours), and makes the "Dark Souls III VR" shortcuts.
  Uninstall-DS3VR-Native.bat (copied into the game folder) undoes everything.

  Optional: -GameDir "<folder with DarkSoulsIII.exe>" -Yes (no questions) -NoShortcuts (practice runs)
#>
param([string]$GameDir, [switch]$Yes, [switch]$NoShortcuts)

$Mod = @{
    Name      = 'Dark Souls III VR'
    Exe       = 'DarkSoulsIII.exe'
    ExeSha256 = 'EF5E07C55222F14FFDDECF2C724A0C2A95CEF0D4DA0E075B0DE0BB108B69498C'   # Steam, App ver. 1.15.2 - the tested build
    Folder    = 'DARK SOULS III\Game'                                                  # under steamapps\common
    Marker    = 'DS3VR2-M'                                                             # text inside our dinput8.dll
    Backup    = 'DS3VR-Native_backup'                                                  # next to the Game folder
}
# The graphics options the mod was tested with (JJ, 2026-11-01). Written into the mod's VR copy of GraphicsConfig.xml on
# every install, so a player whose settings were lowered for another VR mod gets the tested ones. Window mode and size are
# set by the mod itself at each VR start.
$Graphics = [ordered]@{
    'Auto-detectBestRenderingSettings' = 'OFF'; 'QualitySetting' = 'CUSTOM'; 'TextureQuality' = 'MAX'; 'Antialiasing' = 'ON'
    'SSAO' = 'HIGH'; 'DepthOfField' = 'MAX'; 'MotionBlur' = 'DISABLE'; 'ShadowQuality' = 'MAX'; 'LightingQuality' = 'MAX'
    'EffectsQuality' = 'MAX'; 'ReflectionQuality' = 'MAX'; 'WaterSurfaceQuality' = 'HIGH'; 'ShadeQuality' = 'MAX'
}
# Files of other 3D / VR setups that stop this mod working (geo-11 and Helifax's 3D fix, the geo-11 based DS3 VR mod).
$OtherSetup = @('d3d11.dll', 'dxgi.dll', 'nvapi64.dll', 'd3dcompiler_47.dll', 'd3dx.ini', 'd3dxdm.ini', 'uninstall.bat',
                'ShaderFixes', 'ShaderFixesDM', 'ShaderCache', 'ShaderCacheDM', 'ds3vr', 'ds3vr.ini', 'ds3vr_backup',
                'install-ds3vr.bat', 'uninstall-ds3vr.bat', 'Dark_Souls_3_geo11_fix.7z', 'FixFiles.7z')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Step([string]$text) { Write-Host ''; Write-Host "  $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host ''; Write-Host "  STOPPED: $text" -ForegroundColor Red; Write-Host ''; exit 1 }
$Here = [IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)
function Is-Ours([string]$path) { if (-not (Test-Path -LiteralPath $path)) { return $false }; ([IO.File]::ReadAllText($path, [Text.Encoding]::GetEncoding(28591))).Contains($Mod.Marker) }

function Find-SteamGameDir {
    $roots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty $key -ErrorAction Stop
            if ($p.SteamPath) { $roots += ($p.SteamPath -replace '/', '\') }
            if ($p.InstallPath) { $roots += $p.InstallPath }
        } catch { }
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = [IO.Path]::Combine($r, 'steamapps\libraryfolders.vdf')
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\\\', '\') }
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $dir = [IO.Path]::Combine($lib, 'steamapps\common\' + $Mod.Folder)
        if (Test-Path -LiteralPath ([IO.Path]::Combine($dir, $Mod.Exe))) { return $dir }
    }
    return $null
}
function Select-Folder([string]$description) {
    $f = New-Object System.Windows.Forms.Form; $f.TopMost = $true
    $d = New-Object System.Windows.Forms.FolderBrowserDialog; $d.Description = $description
    if ($d.ShowDialog($f) -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    return $null
}
# GraphicsConfig.xml is UTF-16 LE; keep the file's own byte-order mark (or its absence) exactly.
function Read-U16([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $bom = $b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE
    $start = 0; if ($bom) { $start = 2 }
    return @{ text = [Text.Encoding]::Unicode.GetString($b, $start, $b.Length - $start); bom = $bom }
}
function Write-U16([string]$path, [string]$text, [bool]$bom) {
    $body = [Text.Encoding]::Unicode.GetBytes($text)
    if ($bom) { $body = [byte[]](0xFF, 0xFE) + $body }
    [IO.File]::WriteAllBytes($path, $body)
}
function Set-Graphics([string]$path) {
    $f = Read-U16 $path
    $t = $f.text
    foreach ($k in $Graphics.Keys) {
        $rx = '<' + [regex]::Escape($k) + '>[^<]*</' + [regex]::Escape($k) + '>'
        $new = '<' + $k + '>' + $Graphics[$k] + '</' + $k + '>'
        if ([regex]::IsMatch($t, $rx)) { $t = [regex]::Replace($t, $rx, $new) }
        else { $t = $t.Replace('</config>', $new + "`r`n</config>") }
    }
    Write-U16 $path $t $f.bom
}

# =================================================================================================
Write-Host ''
Write-Host '  Dark Souls III VR - installer' -ForegroundColor White
Write-Host '  -----------------------------'

# ---- 1. the game ---------------------------------------------------------------------------------
Step '1. Finding Dark Souls III'
if (-not $GameDir) { $GameDir = Find-SteamGameDir }
while (-not $GameDir -or -not (Test-Path -LiteralPath ([IO.Path]::Combine($GameDir, $Mod.Exe)))) {
    if ($Yes) { Fail 'the game was not found. Run the installer again with -GameDir "<folder with DarkSoulsIII.exe>".' }
    Say '  Could not find the game by itself. Please pick its Game folder (...\steamapps\common\DARK SOULS III\Game).' Yellow
    $GameDir = Select-Folder 'Pick the DARK SOULS III\Game folder (the one with DarkSoulsIII.exe)'
    if (-not $GameDir) { Fail 'no game folder was picked.' }
}
if (Get-Process -Name 'DarkSoulsIII' -ErrorAction SilentlyContinue) { Fail 'Dark Souls III is running. Close it, then run the installer again.' }
$GameDir = [IO.Path]::GetFullPath($GameDir)
$GameRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($GameDir, '..'))
Say "  Game: $GameDir"
if ((Get-FileHash -LiteralPath ([IO.Path]::Combine($GameDir, $Mod.Exe))).Hash -ne $Mod.ExeSha256) {
    Fail 'this copy of the game is a different version from the one the mod was made for (Steam, App ver. 1.15.2). Nothing was changed.'
}
$live = [IO.Path]::Combine($env:APPDATA, 'DarkSoulsIII\GraphicsConfig.xml')
if (-not (Test-Path -LiteralPath $live)) {
    Fail 'the game has not made its settings yet. Start Dark Souls III once from Steam as normal, wait for the title screen, quit, then run this installer again.'
}
$bk = [IO.Path]::Combine($GameRoot, $Mod.Backup)

# ---- 2. other 3D / VR setups in the game folder ---------------------------------------------------
Step '2. Checking the game folder'
$found = @()
foreach ($n in $OtherSetup) { if (Test-Path -LiteralPath ([IO.Path]::Combine($GameDir, $n))) { $found += $n } }
$din = [IO.Path]::Combine($GameDir, 'dinput8.dll')
if ((Test-Path -LiteralPath $din) -and -not (Is-Ours $din)) { $found += 'dinput8.dll' }
if ($found.Count) {
    Say '  The game folder holds files of another 3D or VR setup (geo-11, a 3D fix or another mod):' Yellow
    Say ('    ' + ($found -join ', ')) Yellow
    Say '  They stop this mod from working. They are moved (not deleted) into' Yellow
    Say "    $bk\previous_setup" Yellow
    Say '  and the uninstaller puts them back.' Yellow
    if (-not $Yes) {
        $a = Read-Host '  Move them now? (y/n)'
        if ($a -notmatch '^[yY]') { Fail 'nothing was changed. Remove the other setup with its own uninstaller, then run this installer again.' }
    }
    $prev = [IO.Path]::Combine($bk, 'previous_setup')
    New-Item -ItemType Directory -Force $prev | Out-Null
    foreach ($n in $found) {
        $src = [IO.Path]::Combine($GameDir, $n); $dst = [IO.Path]::Combine($prev, $n)
        if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $src -Recurse -Force }   # a copy is already saved from an earlier install
        else { Move-Item -LiteralPath $src -Destination $dst }
    }
    Say '  Moved.' Green
} else {
    Say '  Ready.' Green
}

# ---- 3. the mod ---------------------------------------------------------------------------------
Step '3. Copying the mod into the game folder'
$files = @('dinput8.dll', 'Dark Souls III VR.bat', 'Uninstall-DS3VR-Native.bat', 'Uninstall-DS3VR-Native.ps1')
foreach ($f in $files) { if (-not (Test-Path -LiteralPath ([IO.Path]::Combine($Here, $f)))) { Fail "$f is missing from the download. Unpack the whole zip and run the installer from there." } }
if (-not (Test-Path -LiteralPath ([IO.Path]::Combine($Here, 'files\DS3VR2\nvngx_dlss.dll')))) { Fail 'files\DS3VR2 is missing from the download. Unpack the whole zip and run the installer from there.' }
foreach ($f in $files) { Copy-Item -LiteralPath ([IO.Path]::Combine($Here, $f)) -Destination ([IO.Path]::Combine($GameDir, $f)) -Force }
$data = [IO.Path]::Combine($GameDir, 'DS3VR2')
New-Item -ItemType Directory -Force $data | Out-Null
Get-ChildItem -LiteralPath ([IO.Path]::Combine($Here, 'files\DS3VR2')) -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination ([IO.Path]::Combine($data, $_.Name)) -Force }
Say '  Mod files copied.' Green

# ---- 4. the mod's tested settings (first install only: never over the player's own) --------------------------
$ini = [IO.Path]::Combine($data, 'settings.ini')
if (-not (Test-Path -LiteralPath $ini)) {
    Copy-Item -LiteralPath ([IO.Path]::Combine($Here, 'files\settings.ini')) -Destination $ini
    Say '  Tested VR settings copied (change them later in the in-game VR menu).' Green
} else {
    Say '  Your VR settings from an earlier install are kept.'
}

# ---- 5. the game's graphics options for VR ---------------------------------------------------------
Step '4. Setting the game''s graphics options for VR'
New-Item -ItemType Directory -Force $bk | Out-Null
$saved = [IO.Path]::Combine($bk, 'GraphicsConfig.xml')
if (-not (Test-Path -LiteralPath $saved)) { Copy-Item -LiteralPath $live -Destination $saved }   # the player's own, first install only
# The mod keeps one copy for VR starts and one for normal starts (DS3VR2\profiles) and swaps them; last.txt says which
# one is in the game's settings folder right now.
$profVr = [IO.Path]::Combine($data, 'profiles\vr')
New-Item -ItemType Directory -Force $profVr | Out-Null
$vrCopy = [IO.Path]::Combine($profVr, 'GraphicsConfig.xml')
$last = [IO.Path]::Combine($data, 'profiles\last.txt')
$liveIsVr = (Test-Path -LiteralPath $last) -and ((Get-Content -LiteralPath $last -Raw) -match '^\s*vr')
if (-not (Test-Path -LiteralPath $vrCopy)) { Copy-Item -LiteralPath $live -Destination $vrCopy }
Set-Graphics $vrCopy
if ($liveIsVr) { Set-Graphics $live }
Say '  VR starts use: textures, shadows, lighting, effects, reflections and shading at maximum, ambient occlusion' Green
Say '  high, water high, the game''s anti-aliasing on, motion blur off. Your normal (non-VR) game keeps your own settings.' Green

# ---- 6. shortcuts -----------------------------------------------------------------------------------
if (-not $NoShortcuts) {
    Step '5. Making the shortcuts'
    $ws = New-Object -ComObject WScript.Shell
    $places = @([Environment]::GetFolderPath('Desktop'), [IO.Path]::Combine([Environment]::GetFolderPath('Programs'), 'Dark Souls III VR'))
    New-Item -ItemType Directory -Force $places[1] | Out-Null
    foreach ($p in $places) {
        foreach ($s in @(@('Dark Souls III VR', 'Dark Souls III VR.bat'), @('Uninstall Dark Souls III VR', 'Uninstall-DS3VR-Native.bat'))) {
            if ($p -eq $places[0] -and $s[0] -like 'Uninstall*') { continue }   # the desktop gets the launcher only
            $lnk = $ws.CreateShortcut([IO.Path]::Combine($p, $s[0] + '.lnk'))
            $lnk.TargetPath = [IO.Path]::Combine($GameDir, $s[1])
            $lnk.WorkingDirectory = $GameDir
            $lnk.IconLocation = [IO.Path]::Combine($GameDir, $Mod.Exe) + ',0'
            $lnk.Save()
        }
    }
    Say '  "Dark Souls III VR" is on your desktop and in the Start menu.' Green
}

Write-Host ''
Write-Host '  Done. Start your PC VR software, then start the game with the "Dark Souls III VR" shortcut.' -ForegroundColor White
Write-Host '  Starting the game from Steam as usual plays the normal game. In VR, press F10 or click both thumbsticks for the VR menu.'
Write-Host ''
