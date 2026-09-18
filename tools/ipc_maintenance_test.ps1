# Drives the maintenance round-trip against an isolated WeaselServer:
# warm up (LM candidate) -> StartMaintenance -> EndMaintenance -> typing again.
#
# Regression: EndMaintenance() re-initialized librime inside the request while
# holding the global api lock (join_maintenance_thread can take minutes for a
# deployment), so every other session timed out and a client that gave up could
# leave the service disabled.  The resume now runs on a background thread: the
# request must answer immediately and input must come back on its own.
#
# Usage: powershell -File ipc_maintenance_test.ps1 [-ServerExe <path>]
param(
  [string]$Runtime = "D:\Workspace\rime\third-party\lm-test\runtime",
  [string]$ServerExe = "D:\Workspace\rime\weasel\output\weaselserver.exe",
  [string]$ClientExe = "D:\Workspace\rime\third-party\lm-test\weasel_ipc_maintenance_e2e.exe",
  [string]$PipeName = "lmmaintest",
  [int]$IpcTimeoutMs = 1500
)

$ErrorActionPreference = "Stop"

$env:RIME_WEASEL_PIPE_NAME = $PipeName
$env:RIME_WEASEL_USER_DIR = Join-Path $Runtime "user"
$env:RIME_WEASEL_IPC_TIMEOUT_MS = "$IpcTimeoutMs"

Write-Host "== staging isolated runtime =="
Copy-Item -Force $ServerExe (Join-Path $Runtime "WeaselServer.exe")
Get-Process WeaselServer -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "$Runtime*" } |
  ForEach-Object { Write-Host "stopping stale isolated server pid=$($_.Id)"; $_.Kill(); $_.WaitForExit() }

Write-Host "== starting isolated server (pipe=$PipeName, server=$ServerExe) =="
$server = Start-Process -FilePath (Join-Path $Runtime "WeaselServer.exe") `
  -WorkingDirectory $Runtime -PassThru
Start-Sleep -Seconds 6
if ($server.HasExited) { throw "isolated server exited with $($server.ExitCode)" }
Write-Host "server pid=$($server.Id)"

$watch = [System.Diagnostics.Stopwatch]::StartNew()
$output = & $ClientExe 30000 2>&1
$watch.Stop()
$output | ForEach-Object { Write-Host "  $_" }
$result = ($output | Select-String -Pattern "RESULT").Line
Write-Host ("[maintenance e2e] {0:N1}s exit={1}" -f $watch.Elapsed.TotalSeconds, $LASTEXITCODE)

$watchdog = Join-Path $env:TEMP "rime.weasel\weasel-watchdog.log"
if (Test-Path $watchdog) {
  Write-Host "== watchdog log (tail) =="
  Get-Content $watchdog -Tail 10 | ForEach-Object { Write-Host "  $_" }
} else {
  Write-Host "== no watchdog warnings recorded =="
}

Write-Host "== stopping isolated server =="
$server.Kill()
$server.WaitForExit()

if ($LASTEXITCODE -eq 0) { Write-Host "RESULT ipc_maintenance_test=pass"; exit 0 }
Write-Host "RESULT ipc_maintenance_test=fail"; exit 1
