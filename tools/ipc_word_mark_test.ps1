# Assertion for the lm_ranker [LM] marker on a *char/word* candidate.
#
# The generated sentence candidate has always carried [LM].  This test types a
# two-syllable input ("beijing"), which is below sentence.min_syllables (4), so
# the sentence path cannot produce a mark: any [LM] in the response must come
# from the reranker marking the candidate it scored highest.
#
# Usage:
#   powershell -File ipc_word_mark_test.ps1                     # expect a mark
#   powershell -File ipc_word_mark_test.ps1 -RimeDll <old.dll> -ExpectMark:$false
param(
  [string]$Runtime = "D:\Workspace\rime\third-party\lm-test\runtime",
  [string]$ServerExe = "D:\Workspace\rime\weasel\output\weaselserver.exe",
  [string]$RimeDll = "D:\Workspace\rime\weasel\output\rime.dll",
  [string]$ClientExe = "D:\Workspace\rime\third-party\lm-test\weasel_ipc_lm_e2e.exe",
  [string]$PipeName = "lmwordmark",
  [string]$Keys = "beijing",
  [int]$KeyDelayMs = 150,
  # Negative control for an older rime.dll that has no mark_top support.
  [switch]$NoMark
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

Write-Host "== starting isolated server (keys=$Keys, delay=${KeyDelayMs}ms) =="
$server = Start-Process -FilePath (Join-Path $Runtime "WeaselServer.exe") `
  -WorkingDirectory $Runtime -PassThru
Start-Sleep -Seconds 12
if ($server.HasExited) { throw "isolated server exited with $($server.ExitCode)" }

# Warm-up: the first session loads the model (~1-2s); the measured run must not
# race with that, otherwise no LM result can exist at all.
& $ClientExe "meeting at three pm, " "beijingdaxue" 8000 $KeyDelayMs > $null 2>&1

$out = & $ClientExe "meeting at three pm, " $Keys 8000 $KeyDelayMs 2>&1
$marks = ($out | Select-String -Pattern "\[LM\]" -AllMatches).Matches.Count
$result = ($out | Select-String -Pattern "RESULT").Line
Write-Host "client: $result"
Write-Host "[LM] occurrences in responses: $marks"

$server.Kill()
$server.WaitForExit()

if (-not $NoMark -and $marks -gt 0) {
  Write-Host "RESULT ipc_word_mark_test=pass"
  exit 0
}
if ($NoMark -and $marks -eq 0) {
  Write-Host "RESULT ipc_word_mark_test=pass (no mark, as expected for this build)"
  exit 0
}
Write-Host "RESULT ipc_word_mark_test=fail (marks=$marks noMark=$NoMark)"
exit 1
