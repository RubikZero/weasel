# Context-window experiment: the reranker always encodes the context into a
# fixed 256-token window (left padded, as in training), so every candidate row
# costs ~260 tokens even when the user's context is 20 characters.  This script
# compares the shipped default (256) with a short window on two axes:
#   * ranking parity: the candidate order and the [LM] placement per input
#   * cost: the plugin's own "rank scored+publishing in X ms" lines
#
# Usage: powershell -File ctx_window_parity.ps1 [-Window 32] [-KeysCSV "ni,wo,..."]
param(
  [string]$E2ECheck = "D:\Workspace\rime\third-party\lm-test\e2e-check",
  [string]$Shared = "D:\Workspace\rime\weasel\output\data",
  [int]$Window = 32,
  [string]$KeysCSV = "ni,wo,ta,de,beijing,zhongguo,daxue,renmin,beijingdaxue,womenshizhongguoren",
  [string]$Context = "meeting at three pm, "
)

# The harness writes plugin diagnostics to stderr; keep going when merging it.
$ErrorActionPreference = "Continue"
$exe = Join-Path $E2ECheck "weasel_lm_e2e.exe"
$userDefault = Join-Path $E2ECheck "user"
$userShort = Join-Path $E2ECheck "user-ctx$Window"

# Short-window copy of the harness user dir (same model, same build output).
if (Test-Path $userShort) { Remove-Item $userShort -Recurse -Force }
Copy-Item $userDefault $userShort -Recurse
$configPath = Join-Path $userShort "lm_ranker.yaml"
$config = Get-Content $configPath -Raw
if ($config -notmatch "max_context") {
  $config = $config -replace "(?m)^  prior_beta:", "  max_context: $Window`r`n  prior_beta:"
}
[System.IO.File]::WriteAllText($configPath, $config, (New-Object System.Text.UTF8Encoding($false)))

function Get-Candidates([string]$userDir, [string]$keys) {
  $out = & $exe --shared $Shared --user $userDir --context $Context --keys $keys --timeout-ms 15000 2>&1
  # The harness prints the menu after every refresh; keep only the last group
  # (the final state), which is what parity has to compare.
  $groups = New-Object System.Collections.ArrayList
  $current = New-Object System.Collections.ArrayList
  foreach ($line in $out) {
    if ($line -match "^\s+(\d+)\s+([^\s]+)\s+([^\s]*)\s*(.*)$") {
      $index = [int]$matches[1]
      if ($index -eq 1 -and $current.Count -gt 0) {
        [void]$groups.Add(($current -join " | "))
        $current = New-Object System.Collections.ArrayList
      }
      [void]$current.Add(("{0}:{1}:{2}" -f $index, $matches[2], $matches[4].Trim()))
    }
  }
  if ($current.Count -gt 0) { [void]$groups.Add(($current -join " | ")) }
  $score = @()
  foreach ($line in $out) {
    if ($line -match "rank scored\+publishing in ([\d.]+) ms") {
      $score += [double]$matches[1]
    }
  }
  [pscustomobject]@{
    candidates = if ($groups.Count -gt 0) { $groups[$groups.Count - 1] } else { "" }
    menu_count = $groups.Count
    score_ms = if ($score.Count -gt 0) { ($score | Measure-Object -Average).Average } else { -1 }
  }
}

$diff = 0
foreach ($keys in $KeysCSV.Split(",")) {
  $a = Get-Candidates $userDefault $keys
  $b = Get-Candidates $userShort $keys
  $same = $a.candidates -eq $b.candidates
  if (-not $same) { $diff++ }
  Write-Host ("[ctx=256] {0,-24} {1,7:N1}ms  {2}" -f $keys, $a.score_ms, $a.candidates)
  Write-Host ("[ctx={0,-3}] {1,-24} {2,7:N1}ms  {3}" -f $Window, $keys, $b.score_ms, $b.candidates)
  if (-not $same) { Write-Host "         ^ candidate sets differ" }
}

Write-Host "RESULT ctx_window_parity window=$Window inputs=$($KeysCSV.Split(',').Count) differing=$diff"
