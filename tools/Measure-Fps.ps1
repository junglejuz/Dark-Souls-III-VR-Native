# Measure-Fps.ps1 - frame rate of Dark Souls III measured from OUTSIDE the mod (Intel PresentMon + nvidia-smi), so two
# different mods can be compared the same way (DS3VR2 vs third-party DS3-VR, JJ 2026-11-01).
#   powershell -File ds3vr2\tools\Measure-Fps.ps1 -Tag ours_balanced -Seconds 60
# Writes <evidence>\fps_<Tag>.csv (PresentMon) + fps_<Tag>_gpu.csv (nvidia-smi, 1 s) and prints a summary:
# average fps, 1 % low, median frame time, GPU busy per frame, GPU load and video memory.
param([Parameter(Mandatory = $true)][string]$Tag, [int]$Seconds = 60,
      [string]$Out = "D:\Documents\Antigravity\VR_Modding\ds3vr2\evidence\fps_compare")
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null
$pm = "C:\Program Files\Intel\PresentMon\PresentMonConsoleApplication\PresentMon-2.3.1-x64.exe"
$csv = Join-Path $Out "fps_$Tag.csv"; $gpu = Join-Path $Out "fps_${Tag}_gpu.csv"
if (-not (Get-Process DarkSoulsIII -ErrorAction SilentlyContinue)) { throw "Dark Souls III is not running" }
$smi = Start-Process nvidia-smi -ArgumentList "--query-gpu=timestamp,utilization.gpu,memory.used,clocks.gr,power.draw --format=csv,noheader,nounits -l 1 -f `"$gpu`"" -PassThru -WindowStyle Hidden
& $pm --process_name DarkSoulsIII.exe --output_file $csv --timed $Seconds --terminate_after_timed --no_console_stats --v2_metrics --stop_existing_session | Out-Null
Stop-Process -Id $smi.Id -ErrorAction SilentlyContinue
$rows = Import-Csv $csv
if (-not $rows) { throw "PresentMon recorded nothing (needs admin or the Performance Log Users group?)" }
$ft = $rows | ForEach-Object { [double]$_.FrameTime } | Where-Object { $_ -gt 0 } | Sort-Object
$busy = $rows | ForEach-Object { [double]$_.GPUBusy } | Where-Object { $_ -gt 0 }
$n = $ft.Count
$avg = 1000.0 / (($ft | Measure-Object -Average).Average)
$low1 = 1000.0 / $ft[[int][math]::Floor($n * 0.99)]
$med = $ft[[int]($n / 2)]
$g = Get-Content $gpu | ForEach-Object { $p = $_.Split(','); [pscustomobject]@{ util = [double]$p[1]; mem = [double]$p[2] } }
"{0}: {1} frames in {2} s | average {3:N1} fps, 1% low {4:N1} fps, median frame {5:N2} ms | GPU busy per frame {6:N2} ms | GPU load {7:N0} % | video memory {8:N0} MB" -f `
    $Tag, $n, $Seconds, $avg, $low1, $med, (($busy | Measure-Object -Average).Average), (($g.util | Measure-Object -Average).Average), (($g.mem | Measure-Object -Maximum).Maximum)
