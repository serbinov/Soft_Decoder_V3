# Line-coverage run for the host unit tests (gcov).
#
# Compiles each suite with gcov instrumentation, runs it and aggregates per-file
# line coverage across all suites. Files that no suite compiles (e.g. hardware
# bound components without host stubs) simply do not appear.
#
#   powershell -ExecutionPolicy Bypass -File test\coverage.ps1
#   powershell -ExecutionPolicy Bypass -File test\coverage.ps1 -Only test_settings
param([string]$Only = "")

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path $env:TEMP "dcc_cov"
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$cc = $env:DCC_TEST_CC
if (-not $cc) {
    foreach ($n in @("gcc", "clang", "cc")) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { $cc = $c.Source; break }
    }
}
if (-not $cc) { Write-Host "ERROR: no C compiler found" -ForegroundColor Red; exit 1 }
$gcov = (Get-Command gcov -ErrorAction SilentlyContinue)
if (-not $gcov) { Write-Host "ERROR: gcov not found (needs gcc)" -ForegroundColor Red; exit 1 }
$gcov = $gcov.Source
Write-Host "Compiler: $cc"

$unityDir = Join-Path $env:TEMP "unity_clone"
if (-not (Test-Path "$unityDir\src\unity.c")) {
    git clone --depth 1 https://github.com/ThrowTheSwitch/Unity.git $unityDir | Out-Null
}
$unitySrc = "$unityDir\src"

$inc = @(
    "-I$unitySrc",
    "-I$root\test_libs\teststubs\include",
    "-I$root\components\dcc\include",
    "-I$root\components\pinmap\include",
    "-I$root\components\settings\include",
    "-I$root\components\motor\include",
    "-I$root\components\auxio\include",
    "-I$root\components\audio\include",
    "-I$root\components\track\include",
    "-I$root\components\web\include",
    "-I$root\components\storage\include",
    "-I$root\components\provision\include",
    "-I$root\main"
)

$suites = @("test_dcc", "test_settings", "test_motor", "test_auxio",
            "test_web_util", "test_track", "test_audio", "test_pinmap",
            "test_track_manifest")
if ($Only) { $suites = @($Only) }

$lineCov = @{}

foreach ($suite in $suites) {
    $dir = Join-Path $root "test\$suite"
    $srcs = @(Get-ChildItem "$dir\*.c" -ErrorAction SilentlyContinue)
    if ($srcs.Count -eq 0) { continue }
    $work = Join-Path $tmp $suite
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $names = $srcs | ForEach-Object { $_.FullName }
    $exe = Join-Path $work "$suite.exe"
    Push-Location $work
    & $cc $inc $names "$unitySrc\unity.c" --coverage -w -o $exe *> $null
    if (-not (Test-Path $exe)) { Pop-Location; Write-Host "BUILD FAILED: $suite"; continue }
    & $exe *> $null
    $notes = @(Get-ChildItem $work -Filter "*.gcno" | Where-Object { $_.Name -notlike "*unity*" })
    if ($notes.Count -eq 0) { Pop-Location; Write-Host "no gcov notes: $suite"; continue }
    & $gcov -b ($notes | ForEach-Object { $_.Name }) *> $null
    Pop-Location

    # Union the per-line coverage of the .gcov files this suite produced.
    foreach ($g in (Get-ChildItem $work -Filter "*.gcov" -ErrorAction SilentlyContinue)) {
        $src = $null
        foreach ($line in (Get-Content -LiteralPath $g.FullName)) {
            if ($line -match ':\s*0:Source:(.+)$') {
                $src = $Matches[1].Trim()
                if (-not $lineCov.ContainsKey($src)) { $lineCov[$src] = @{} }
                continue
            }
            if (-not $src) { continue }
            if ($line -match '^\s*([0-9]+|#####|=====|\*+|-)\s*:\s*([0-9]+):') {
                $cnt = $Matches[1]
                if ($cnt -eq '-') { continue }
                $ln = [int]$Matches[2]
                $covered = $false
                if ($cnt -match '^\d') { $covered = ([int64]($cnt -replace '\*', '')) -gt 0 }
                elseif ($cnt -match '\*') { $covered = $true }
                if (-not $lineCov[$src].ContainsKey($ln) -or $covered) {
                    $lineCov[$src][$ln] = $covered
                }
            }
        }
    }
}

Write-Host ""
Write-Host "=== COVERAGE: first-party sources (components/ + main/) ==="
$tc = 0
$tt = 0
foreach ($src in ($lineCov.Keys | Sort-Object)) {
    if ($src -notmatch '[\\/](components|main)[\\/]') { continue }
    $m = $lineCov[$src]
    $tot = $m.Count
    $cov = (@($m.Values | Where-Object { $_ -eq $true })).Count
    $p = if ($tot) { 100.0 * $cov / $tot } else { 0 }
    $short = $src.Replace($root, "").TrimStart('\', '/')
    "{0,6:N1}%  {1,5}/{2,-5}  {3}" -f $p, $cov, $tot, $short
    $tc += $cov
    $tt += $tot
}
$tp = if ($tt) { 100.0 * $tc / $tt } else { 0 }
Write-Host ""
"TOTAL (first-party): {0:N1}%  ({1}/{2})" -f $tp, $tc, $tt
