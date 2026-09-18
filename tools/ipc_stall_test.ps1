# Drives an isolated WeaselServer over its public IPC protocol, then repeats the
# test while the server's *message thread* is suspended.
#
# Why: the candidate window is owned by the server message thread, and UI work
# used to run on the pipe worker threads while they held the global api lock.
# A worker that moved or painted that window therefore waited for the message
# thread -- which itself takes the same lock for deferred language-model work.
# The two threads waited for each other forever, and a client timeout could not
# break it (closing a pipe wakes I/O, not a mutex), so input stayed dead until
# the server process was killed.
#
# With UI work posted to the message thread, the request path never touches a
# window: with the message thread frozen the client must still get candidates
# (including the [LM] one) promptly, only the candidate window goes stale.
#
# Usage: pwsh -File ipc_stall_test.ps1 [-ServerExe <path>] [-SkipStall]
param(
  [string]$Runtime = "D:\Workspace\rime\third-party\lm-test\runtime",
  [string]$ServerExe = "D:\Workspace\rime\weasel\output\weaselserver.exe",
  [string]$ClientExe = "D:\Workspace\rime\third-party\lm-test\weasel_ipc_lm_e2e.exe",
  [string]$PipeName = "lmstalltest",
  [int]$IpcTimeoutMs = 1500,
  [switch]$SkipStall
)

$ErrorActionPreference = "Stop"

Add-Type -Namespace Win32 -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern IntPtr FindWindowW(string className, string windowName);
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern int GetWindowTextW(IntPtr hWnd, System.Text.StringBuilder text, int count);
[DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll")]
public static extern bool EnumWindows(EnumProc callback, IntPtr param);
public delegate bool EnumProc(IntPtr hWnd, IntPtr param);
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern int GetClassNameW(IntPtr hWnd, System.Text.StringBuilder name, int count);
[DllImport("kernel32.dll")]
public static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
[DllImport("kernel32.dll")]
public static extern int SuspendThread(IntPtr handle);
[DllImport("kernel32.dll")]
public static extern int ResumeThread(IntPtr handle);
[DllImport("kernel32.dll")]
public static extern bool CloseHandle(IntPtr handle);
[DllImport("user32.dll")]
public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
'@

$env:RIME_WEASEL_PIPE_NAME = $PipeName
$env:RIME_WEASEL_USER_DIR = Join-Path $Runtime "user"
$env:RIME_WEASEL_IPC_TIMEOUT_MS = "$IpcTimeoutMs"

$script:foundWindow = [IntPtr]::Zero
$script:targetPid = 0

function Get-ServerWindow([int]$targetProcessId) {
  $script:targetPid = $targetProcessId
  $script:foundWindow = [IntPtr]::Zero
  $callback = [Win32.Native+EnumProc] {
    param($hwnd, $param)
    $name = New-Object System.Text.StringBuilder 256
    [void][Win32.Native]::GetClassNameW($hwnd, $name, 256)
    if ($name.ToString() -eq "WeaselIPCWindow_1.0") {
      $owner = 0
      [void][Win32.Native]::GetWindowThreadProcessId($hwnd, [ref]$owner)
      if ($owner -eq $script:targetPid) {
        $script:foundWindow = $hwnd
        return $false
      }
    }
    return $true
  }
  [void][Win32.Native]::EnumWindows($callback, [IntPtr]::Zero)
  return $script:foundWindow
}

Write-Host "== staging isolated runtime =="
Copy-Item -Force $ServerExe (Join-Path $Runtime "WeaselServer.exe")
Get-Process WeaselServer -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "$Runtime*" } |
  ForEach-Object { Write-Host "stopping stale isolated server pid=$($_.Id)"; $_.Kill(); $_.WaitForExit() }

Write-Host "== starting isolated server (pipe=$PipeName) =="
$server = Start-Process -FilePath (Join-Path $Runtime "WeaselServer.exe") `
  -WorkingDirectory $Runtime -PassThru
Start-Sleep -Seconds 6
if ($server.HasExited) { throw "isolated server exited with $($server.ExitCode)" }
Write-Host "server pid=$($server.Id)"

function Invoke-Client([string]$label) {
  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  $output = & $ClientExe 2>&1
  $watch.Stop()
  $result = ($output | Select-String -Pattern "RESULT").Line
  Write-Host ("[{0}] {1:N1}s exit={2} {3}" -f $label, $watch.Elapsed.TotalSeconds, $LASTEXITCODE, $result)
  if (-not $result -or $result -notmatch "lm_candidate=yes") {
    Write-Host ($output | Select-Object -Last 6 | Out-String)
    return $false
  }
  return $true
}

$ok = $true
$ok = (Invoke-Client "baseline") -and $ok

$threadHandle = [IntPtr]::Zero
if (-not $SkipStall) {
  $hwnd = Get-ServerWindow $server.Id
  if ($hwnd -eq [IntPtr]::Zero) { throw "server window not found" }
  $ownerPid = 0
  $threadId = [Win32.Native]::GetWindowThreadProcessId($hwnd, [ref]$ownerPid)
  Write-Host "== suspending server message thread tid=$threadId (window $hwnd, pid $ownerPid) =="
  $threadHandle = [Win32.Native]::OpenThread(0x0002, $false, $threadId)  # SUSPEND_RESUME
  if ($threadHandle -eq [IntPtr]::Zero) { throw "OpenThread failed" }
  if ([Win32.Native]::SuspendThread($threadHandle) -lt 0) { throw "SuspendThread failed" }
  $ok = (Invoke-Client "message-thread frozen") -and $ok
  [void][Win32.Native]::ResumeThread($threadHandle)
  [void][Win32.Native]::CloseHandle($threadHandle)
  Write-Host "== resumed =="
  $ok = (Invoke-Client "after resume") -and $ok
}

$watchdog = Join-Path $env:TEMP "rime.weasel\weasel-watchdog.log"
if (Test-Path $watchdog) {
  Write-Host "== watchdog log (tail) =="
  Get-Content $watchdog -Tail 10 | ForEach-Object { Write-Host "  $_" }
} else {
  Write-Host "== no watchdog warnings recorded =="
}

Write-Host "== stopping isolated server =="
# Ask it to quit the way WeaselServer.exe /q does now: WM_CLOSE is handled on
# the message thread without the pipe and without the api lock, so it also works
# while a request is stuck -- previously the only way out was killing the
# process by hand.
$shutdownWindow = Get-ServerWindow $server.Id
$graceful = $false
if ($shutdownWindow -ne [IntPtr]::Zero) {
  [void][Win32.Native]::PostMessageW($shutdownWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
  $graceful = $server.WaitForExit(5000)
} else {
  Write-Host "server window not found; cannot request a graceful shutdown"
}
Write-Host "graceful shutdown: $graceful"
if (-not $graceful) {
  $server.Kill()
  $server.WaitForExit()
}

if ($ok -and $graceful) { Write-Host "RESULT ipc_stall_test=pass"; exit 0 }
Write-Host "RESULT ipc_stall_test=fail (ok=$ok graceful=$graceful)"; exit 1
