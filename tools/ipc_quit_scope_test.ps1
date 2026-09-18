# Guards the scope of "WeaselServer.exe /q".
#
# Every installation -- and every isolated test instance -- creates an IPC
# window of the same class "WeaselIPCWindow_1.0" and, unless
# RIME_WEASEL_PIPE_NAME says otherwise, listens on the same per-user pipe name.
# /q used to close the first window with that class and then shut down whatever
# answered the pipe, so a build started from another directory stopped the
# user's installed server: input fell back to plain English letters and stayed
# that way until the service was started again.  A quit must only ever reach a
# server that runs the same executable as the caller.
#
# Checks:
#   1a. /q from another directory, with a clean environment  -> isolated server survives
#   1b. /q from another directory, with the isolated env set -> isolated server survives
#       (this is the pipe-fallback path: it must not steal another install's pipe)
#   2.  /q from the server's own directory                   -> isolated server stops
#
# -CheckInstalled additionally asserts the same for the user's installed server.
# It is off by default because running it against a *reverted* build would stop
# that server; the isolated cases above exercise the same guard.
#
# Usage: pwsh -File ipc_quit_scope_test.ps1 [-CheckInstalled] [-KeepServer]
param(
  [string]$Runtime = "D:\Workspace\rime\third-party\lm-test\runtime",
  [string]$ServerExe = "D:\Workspace\rime\weasel\output\weaselserver.exe",
  [string]$PipeName = "lmquitscope",
  [switch]$CheckInstalled,
  [switch]$KeepServer
)

$ErrorActionPreference = "Stop"

$env:RIME_WEASEL_PIPE_NAME = $PipeName
$env:RIME_WEASEL_USER_DIR = Join-Path $Runtime "user"
$env:RIME_WEASEL_IPC_TIMEOUT_MS = "1500"

$runtimeServer = Join-Path $Runtime "WeaselServer.exe"

function Get-IpcWindows {
  # Every IPC window of the given image path (class+title are shared by all
  # installations, so the image path is what identifies them).
  param([string]$ImagePath)
  $pids = @(Get-Process WeaselServer -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ($_.Path -ieq $ImagePath) } |
    Select-Object -ExpandProperty Id)
  return $pids
}

function Test-Alive([int]$ProcessId) {
  return [bool](Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

function Invoke-Quit {
  param([string]$Exe, [switch]$WithIsolatedEnvironment)
  $saved = @{}
  if (-not $WithIsolatedEnvironment) {
    foreach ($name in @("RIME_WEASEL_PIPE_NAME", "RIME_WEASEL_USER_DIR",
                        "RIME_WEASEL_IPC_TIMEOUT_MS")) {
      $saved[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
      [Environment]::SetEnvironmentVariable($name, $null, "Process")
    }
  }
  $proc = Start-Process -FilePath $Exe -ArgumentList "/q" -PassThru -Wait
  if (-not $WithIsolatedEnvironment) {
    foreach ($name in $saved.Keys) {
      [Environment]::SetEnvironmentVariable($name, $saved[$name], "Process")
    }
  }
  return $proc.ExitCode
}

Write-Host "== staging isolated runtime =="
Copy-Item -Force $ServerExe $runtimeServer
Get-Process WeaselServer -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -and $_.Path.StartsWith($Runtime, "OrdinalIgnoreCase") } |
  ForEach-Object { Write-Host "stopping stale isolated server pid=$($_.Id)"; $_.Kill(); $_.WaitForExit() }

Write-Host "== starting isolated server (pipe=$PipeName, path=$runtimeServer) =="
$server = Start-Process -FilePath $runtimeServer -WorkingDirectory $Runtime -PassThru
Start-Sleep -Seconds 6
if ($server.HasExited) { throw "isolated server exited with $($server.ExitCode)" }
Write-Host "isolated server pid=$($server.Id)"
if (-not (Get-IpcWindows $runtimeServer)) { throw "isolated server has no IPC window" }

$ok = $true

Write-Host "== 1a: /q from the build directory, clean environment (must not quit it) =="
$code = Invoke-Quit -Exe $ServerExe
Start-Sleep -Seconds 2
$alive = Test-Alive $server.Id
Write-Host "   exit=$code alive=$alive"
if (-not $alive) { Write-Host "   FAIL: a foreign server was stopped by /q"; $ok = $false }

Write-Host "== 1b: /q from the build directory, isolated environment (must not quit it) =="
$code = Invoke-Quit -Exe $ServerExe -WithIsolatedEnvironment
Start-Sleep -Seconds 2
$alive = Test-Alive $server.Id
Write-Host "   exit=$code alive=$alive"
if (-not $alive) { Write-Host "   FAIL: the shared pipe was shut down by a foreign /q"; $ok = $false }

if ($CheckInstalled) {
  Write-Host "== 3: /q from the build directory must not stop an installed server =="
  $installed = @(Get-Process WeaselServer -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and -not $_.Path.StartsWith($Runtime, "OrdinalIgnoreCase") })
  if ($installed.Count -eq 0) {
    Write-Host "   skipped: no server outside $Runtime is running"
  } else {
    foreach ($p in $installed) {
      $before = $p.StartTime
      [void](Invoke-Quit -Exe $ServerExe)
      Start-Sleep -Seconds 2
      $still = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
      if ($still -and $still.StartTime -eq $before) {
        Write-Host "   pid=$($p.Id) $($p.Path) survived"
      } else {
        Write-Host "   FAIL: pid=$($p.Id) $($p.Path) was stopped by a foreign /q"
        $ok = $false
      }
    }
  }
}

Write-Host "== 2: /q from its own directory (must quit it) =="
$code = Invoke-Quit -Exe $runtimeServer -WithIsolatedEnvironment
$stopped = $server.WaitForExit(10000)
Write-Host "   exit=$code stopped=$stopped"
if (-not $stopped) { Write-Host "   FAIL: own-server /q did not stop it"; $ok = $false }

if (-not $KeepServer) {
  Get-Process WeaselServer -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($Runtime, "OrdinalIgnoreCase") } |
    ForEach-Object { $_.Kill() }
}

if ($ok) { Write-Host "RESULT ipc_quit_scope_test=pass"; exit 0 }
Write-Host "RESULT ipc_quit_scope_test=fail"; exit 1
