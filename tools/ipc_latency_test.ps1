# Measures the user-visible latency of the asynchronous LM rerank, for single
# characters, words and full sentences, and separates the phases:
#
#   visible latency (poll_ms = 120, the TSF default)  = pipeline + poll alignment
#   visible latency (poll_ms = 20)                    = pipeline floor
#
# Usage: powershell -File ipc_latency_test.ps1 [-ServerExe <path>] [-RimeDll <path>]
param(
  [string]$Runtime = "D:\Workspace\rime\third-party\lm-test\runtime",
  [string]$ServerExe = "D:\Workspace\rime\weasel\output\weaselserver.exe",
  [string]$RimeDll = "D:\Workspace\rime\weasel\output\rime.dll",
  [string]$ClientExe = "D:\Workspace\rime\third-party\lm-test\weasel_ipc_latency_e2e.exe",
  [string]$PipeName = "lmlatency",
  [int]$Runs = 5,
  [string]$Cases = "ni=char,beijing=word,beijingdaxue=sentence",
  [string]$Polls = "120,20",
  [int]$KeyDelayMs = 150
)

$ErrorActionPreference = "Stop"

$env:RIME_WEASEL_PIPE_NAME = $PipeName
$env:RIME_WEASEL_USER_DIR = Join-Path $Runtime "user"
$env:RIME_WEASEL_IPC_TIMEOUT_MS = "5000"

Write-Host "== staging isolated runtime =="
Copy-Item -Force $ServerExe (Join-Path $Runtime "WeaselServer.exe")
Copy-Item -Force $RimeDll (Join-Path $Runtime "rime.dll")
Get-Process WeaselServer -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "$Runtime*" } |
  ForEach-Object { $_.Kill(); $_.WaitForExit() }

Write-Host "== starting isolated server (pipe=$PipeName) =="
$server = Start-Process -FilePath (Join-Path $Runtime "WeaselServer.exe") `
  -WorkingDirectory $Runtime -PassThru
Start-Sleep -Seconds 12
if ($server.HasExited) { throw "isolated server exited with $($server.ExitCode)" }

# Warm up: first session loads the model (~1-2s).
& $ClientExe "meeting at three pm, " "beijingdaxue" 120 120 6000 2 > $null 2>&1

foreach ($poll in $Polls.Split(",")) {
  foreach ($case in $Cases.Split(",")) {
    $parts = $case.Split("=")
    $keys = $parts[0]
    $kind = $parts[1]
    $line = & $ClientExe "meeting at three pm, " $keys $KeyDelayMs $poll 3000 $Runs 2>&1 |
      Select-String "RESULT" | Select-Object -Last 1
    Write-Host ("[{0,-8} poll={1,3}ms] {2}" -f $kind, $poll, $line)
  }
}

Write-Host "== stopping isolated server =="
$server.Kill()
$server.WaitForExit()
Write-Host "RESULT ipc_latency_test=done"
