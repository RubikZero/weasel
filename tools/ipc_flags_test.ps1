# Verifies the client-side signal that the input-method defence depends on:
# a stalled server must be reported as a *failed IPC transaction*, not as a
# normal "the server did not handle this key".
#
# Phase 1 talks to a healthy isolated server; then every server thread is
# suspended and phase 2 must fail fast (within the transaction budget).
#
# Usage: powershell -File ipc_flags_test.ps1
param(
  [string]$Runtime = "D:\Workspace\rime\third-party\lm-test\runtime",
  [string]$ServerExe = "D:\Workspace\rime\weasel\output\weaselserver.exe",
  [string]$ClientExe = "D:\Workspace\rime\third-party\lm-test\weasel_ipc_flags_e2e.exe",
  [string]$PipeName = "lmflagstest",
  [int]$IpcTimeoutMs = 2000,
  [int]$PauseMs = 4000
)

$ErrorActionPreference = "Stop"

Add-Type -Namespace Win32Flags -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")]
public static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
[DllImport("kernel32.dll")]
public static extern int SuspendThread(IntPtr handle);
[DllImport("kernel32.dll")]
public static extern int ResumeThread(IntPtr handle);
[DllImport("kernel32.dll")]
public static extern bool CloseHandle(IntPtr handle);
'@

$env:RIME_WEASEL_PIPE_NAME = $PipeName
$env:RIME_WEASEL_USER_DIR = Join-Path $Runtime "user"
$env:RIME_WEASEL_IPC_TIMEOUT_MS = "$IpcTimeoutMs"

Write-Host "== staging isolated runtime =="
Copy-Item -Force $ServerExe (Join-Path $Runtime "WeaselServer.exe")
Get-Process WeaselServer -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "$Runtime*" } |
  ForEach-Object { $_.Kill(); $_.WaitForExit() }

Write-Host "== starting isolated server (pipe=$PipeName, ipc timeout=${IpcTimeoutMs}ms) =="
$server = Start-Process -FilePath (Join-Path $Runtime "WeaselServer.exe") `
  -WorkingDirectory $Runtime -PassThru
Start-Sleep -Seconds 12
if ($server.HasExited) { throw "isolated server exited with $($server.ExitCode)" }

# Warm up the language model so phase 1 measures the IPC path, not model loading.
& "D:\Workspace\rime\third-party\lm-test\weasel_ipc_lm_e2e.exe" "meeting at three pm, " "beijingdaxue" 6000 120 > $null 2>&1

Write-Host "== running flags client (phase 2 after ${PauseMs}ms) =="
$client = Start-Process -FilePath $ClientExe -ArgumentList "$PauseMs" -PassThru `
  -RedirectStandardOutput (Join-Path $env:TEMP "flags_e2e.out") `
  -RedirectStandardError (Join-Path $env:TEMP "flags_e2e.err")

# Stall the server while the client waits for phase 2.
Start-Sleep -Milliseconds 1500
$handles = @()
$threadIds = @()
foreach ($t in $server.Threads) {
  $h = [Win32Flags.Native]::OpenThread(0x0002, $false, [uint32]$t.Id)
  if ($h -ne [IntPtr]::Zero) {
    [void][Win32Flags.Native]::SuspendThread($h)
    $handles += $h
    $threadIds += $t.Id
  }
}
Write-Host "suspended $($handles.Count) server threads"

$client.WaitForExit(60000) | Out-Null
$output = Get-Content (Join-Path $env:TEMP "flags_e2e.out") -ErrorAction SilentlyContinue
$output | ForEach-Object { Write-Host "  $_" }

foreach ($h in $handles) { [void][Win32Flags.Native]::ResumeThread($h); [void][Win32Flags.Native]::CloseHandle($h) }
Write-Host "resumed server threads"

$server.Kill()
$server.WaitForExit()

if ($LASTEXITCODE -eq 0 -and ($output -join "`n") -match "flags_e2e=pass") {
  Write-Host "RESULT ipc_flags_test=pass"
  exit 0
}
Write-Host "RESULT ipc_flags_test=fail (exit=$LASTEXITCODE)"
exit 1
