# Measure-Server.ps1 - runs ELEVATED once (one UAC prompt), then measures on request so PresentMon can read frame timing.
# A request = a file <evidence>\fps_compare\request.txt holding "<tag> <seconds>"; the result line goes to results.txt.
# Stops after 2 hours or when request.txt holds "quit".
$dir = "D:\Documents\Antigravity\VR_Modding\ds3vr2\evidence\fps_compare"
$req = Join-Path $dir "request.txt"; $res = Join-Path $dir "results.txt"
$me = "D:\Documents\Antigravity\VR_Modding\ds3vr2\tools\Measure-Fps.ps1"
"server started (elevated) $(Get-Date -Format HH:mm:ss)" | Add-Content $res
$t0 = Get-Date
while (((Get-Date) - $t0).TotalHours -lt 2) {
    if (Test-Path $req) {
        $line = (Get-Content $req -Raw).Trim(); Remove-Item $req -Force
        if ($line -eq "quit") { "server stopped $(Get-Date -Format HH:mm:ss)" | Add-Content $res; break }
        $p = $line.Split(' '); $sec = if ($p.Count -gt 1) { [int]$p[1] } else { 60 }
        "started $($p[0]) $(Get-Date -Format HH:mm:ss)" | Add-Content $res
        try { & $me -Tag $p[0] -Seconds $sec 2>&1 | Out-String | Add-Content $res } catch { "ERROR $($p[0]): $_" | Add-Content $res }
    }
    Start-Sleep -Milliseconds 300
}
