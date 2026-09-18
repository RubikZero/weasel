# Context-window evaluation on real cases: takes pinyin inputs and their real
# contexts from the v2 reranker request set and compares the shipped plugin's
# output with the trained window (256) against a short window.
#
# Reports top-1 agreement, full-order agreement, and mean rank displacement,
# plus the per-job scoring time from the plugin's own diagnostics.
#
# Usage: powershell -File ctx_window_eval.ps1 [-Window 32] [-Limit 30]
param(
  [string]$E2ECheck = "D:\Workspace\rime\third-party\lm-test\e2e-check",
  [string]$Testset = "D:\Workspace\rime\third-party\lm-test\testset",
  [string]$Shared = "D:\Workspace\rime\weasel\output\data",
  [int]$Window = 32,
  [int]$Limit = 30,
  [string]$Types = "char,word"
)

$ErrorActionPreference = "Continue"
$exe = Join-Path $E2ECheck "weasel_lm_e2e.exe"
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Read-Tsv([string]$path, [int]$max) {
  $rows = @()
  foreach ($line in [System.IO.File]::ReadAllLines($path, [System.Text.Encoding]::UTF8)) {
    if ($line.Trim().Length -eq 0) { continue }
    $rows += ,($line -split "`t")
    if ($rows.Count -ge $max) { break }
  }
  return $rows
}

$requests = @{}
foreach ($p in (Read-Tsv (Join-Path $Testset "requests_v2.tsv") 100000)) {
  if ($p.Count -ge 3) { $requests[$p[0]] = @{ keys = $p[1]; target = $p[2] } }
}
$meta = @{}
foreach ($p in (Read-Tsv (Join-Path $Testset "requests_v2_meta.tsv") 100000)) {
  if ($p.Count -ge 4) { $meta[$p[0]] = @{ type = $p[1]; context = $p[3] } }
}

$wanted = $Types -split ","
$cases = @()
foreach ($id in ($requests.Keys | Sort-Object { [int]$_ })) {
  if (-not $meta.ContainsKey($id)) { continue }
  if ($wanted -notcontains $meta[$id].type) { continue }
  $cases += [pscustomobject]@{ id = $id; keys = $requests[$id].keys; context = $meta[$id].context; type = $meta[$id].type }
  if ($cases.Count -ge $Limit) { break }
}
Write-Host "cases=$($cases.Count) window=$Window types=$Types"

function Invoke-Case([string]$userDir, [string]$keys, [string]$context) {
  $out = & $exe --shared $Shared --user $userDir --context $context --keys $keys --timeout-ms 15000 2>&1
  $groups = New-Object System.Collections.ArrayList
  $current = New-Object System.Collections.ArrayList
  foreach ($line in $out) {
    if ($line -match "^\s+(\d+)\s+([^\s]+)\s+([^\s]*)\s*(.*)$") {
      if ([int]$matches[1] -eq 1 -and $current.Count -gt 0) {
        [void]$groups.Add(@($current)); $current = New-Object System.Collections.ArrayList
      }
      [void]$current.Add($matches[2])
    }
  }
  if ($current.Count -gt 0) { [void]$groups.Add(@($current)) }
  $score = @()
  foreach ($line in $out) { if ($line -match "rank scored\+publishing in ([\d.]+) ms") { $score += [double]$matches[1] } }
  [pscustomobject]@{
    final = if ($groups.Count -gt 0) { $groups[$groups.Count - 1] } else { @() }
    score_ms = if ($score.Count -gt 0) { ($score | Measure-Object -Average).Average } else { -1 }
  }
}

$userDefault = Join-Path $E2ECheck "user"
$userShort = Join-Path $E2ECheck "user-ctx$Window"
if (Test-Path $userShort) { Remove-Item $userShort -Recurse -Force }
Copy-Item $userDefault $userShort -Recurse
$configPath = Join-Path $userShort "lm_ranker.yaml"
$config = [System.IO.File]::ReadAllText($configPath)
if ($config -notmatch "max_context") {
  $config = $config -replace "(?m)^  prior_beta:", "  max_context: $Window`r`n  prior_beta:"
}
[System.IO.File]::WriteAllText($configPath, $config, $utf8)

$top1_same = 0; $order_same = 0; $total = 0; $displacement = 0.0
$score_long = @(); $score_short = @()
foreach ($case in $cases) {
  $a = Invoke-Case $userDefault $case.keys $case.context
  $b = Invoke-Case $userShort $case.keys $case.context
  if ($a.final.Count -eq 0 -or $b.final.Count -eq 0) { continue }
  $total++
  if ($a.final[0] -eq $b.final[0]) { $top1_same++ }
  if (($a.final -join " ") -eq ($b.final -join " ")) { $order_same++ }
  # mean absolute rank displacement over the candidates present in both lists
  $sum = 0.0; $n = 0
  for ($i = 0; $i -lt $a.final.Count; $i++) {
    $j = [Array]::IndexOf($b.final, $a.final[$i])
    if ($j -ge 0) { $sum += [Math]::Abs($i - $j); $n++ }
  }
  if ($n -gt 0) { $displacement += $sum / $n }
  if ($a.score_ms -gt 0) { $score_long += $a.score_ms }
  if ($b.score_ms -gt 0) { $score_short += $b.score_ms }
  $mark = if ($a.final[0] -eq $b.final[0]) { "same" } else { "DIFF" }
  Write-Host ("  {0,-7} {1,-18} top1 {2,-12} -> {3,-12} [{4}]" -f $case.type, $case.keys, $a.final[0], $b.final[0], $mark)
}

$avgLong = if ($score_long.Count) { ($score_long | Measure-Object -Average).Average } else { 0 }
$avgShort = if ($score_short.Count) { ($score_short | Measure-Object -Average).Average } else { 0 }
Write-Host ("score/job: ctx=256 {0:N1}ms  ctx={1} {2:N1}ms" -f $avgLong, $Window, $avgShort)
Write-Host ("RESULT ctx_window_eval cases={0} top1_same={1} order_same={2} mean_displacement={3:N2} score256={4:N1}ms score{5}={6:N1}ms" -f `
  $total, $top1_same, $order_same, ($(if ($total) { $displacement / $total } else { 0 })), $avgLong, $Window, $avgShort)
