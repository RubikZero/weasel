# Concurrent IPC stress: several clients type at the same time against one
# isolated WeaselServer while the language model decodes.
#
# Regression coverage for the serialization path: every request takes the global
# api lock, and the new UI/deferred-work routing (window work posted to the
# message thread, language-model notifications coalesced and retried with
# try_lock) must not turn that contention into timeouts or lock-hold warnings.
param(
  [string]$Runtime = "D:\Workspace\rime\third-party\lm-test\runtime",
  [string]$ServerExe = "D:\Workspace\rime\weasel\output\weaselserver.exe",
  [string]$ClientExe = "D:\Workspace\rime\third-party\lm-test\weasel_ipc_lm_e2e.exe",
  [string]$PipeName = "lmstresstest",
  [int]$Clients = 5,
  [int]$Rounds = 2,
  [int]$IpcTimeoutMs = 3000
)

$ErrorActionPreference = "Stop"

$env:RIME_WEASEL_PIPE_NAME = $PipeName
$env:RIME_WEASEL_USER_DIR = Join-Path $Runtime "user"
$env:RIME_WEASEL_IPC_TIMEOUT_MS = "$IpcTimeoutMs"

$keys = @("zheshiyigeceshi", "beijingdaxue", "jintiantianqizenmeyang",
          "womenshizhongguoren", "mingtianhuixiayu")

Get-Process WeaselServer -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "$Runtime*" } |
  ForEach-Object { $_.Kill(); $_.WaitForExit() }
Copy-Item -Force $ServerExe (Join-Path $Runtime "WeaselServer.exe")

Write-Host "== starting isolated server (pipe=$PipeName) =="
$server = Start-Process -FilePath (Join-Path $Runtime "WeaselServer.exe") `
  -WorkingDirectory $Runtime -PassThru
Start-Sleep -Seconds 6
if ($server.HasExited) { throw "isolated server exited with $($server.ExitCode)" }
Write-Host "server pid=$($server.Id)"

$failures = 0
for ($round = 1; $round -le $Rounds; $round++) {
  Write-Host "== round ${round}: $Clients concurrent clients =="
  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  $jobs = @()
  for ($i = 0; $i -lt $Clients; $i++) {
    $key = $keys[$i % $keys.Count]
    $jobs += Start-Job -ScriptBlock {
      param($exe, $k, $pipe, $userDir, $timeout)
      $env:RIME_WEASEL_PIPE_NAME = $pipe
      $env:RIME_WEASEL_USER_DIR = $userDir
      $env:RIME_WEASEL_IPC_TIMEOUT_MS = "$timeout"
      $out = & $exe "meeting at three pm, " $k 15000 2>&1
      [pscustomobject]@{ key = $k; exit = $LASTEXITCODE; lm = [bool]($out | Select-String "lm_candidate=yes") }
    } -ArgumentList $ClientExe, $key, $PipeName, $env:RIME_WEASEL_USER_DIR, $IpcTimeoutMs
  }
  $results = $jobs | Wait-Job | Receive-Job
  $jobs | Remove-Job
  $watch.Stop()
  foreach ($r in $results) {
    if (-not $r.lm) { $failures++ }
    Write-Host ("  key={0,-22} lm={1} exit={2}" -f $r.key, $r.lm, $r.exit)
  }
  Write-Host ("  round {0} finished in {1:N1}s" -f $round, $watch.Elapsed.TotalSeconds)
}

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

if ($failures -eq 0) { Write-Host "RESULT ipc_stress_test=pass"; exit 0 }
Write-Host "RESULT ipc_stress_test=fail ($failures)"; exit 1
