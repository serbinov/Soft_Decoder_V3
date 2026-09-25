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

$agg = @{}

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
    $out = & $gcov -b ($notes | ForEach-Object { $_.Name }) 2>&1 | Out-String
    Pop-Location
    $cur = $null
    foreach ($line in ($out -split "`n")) {
        $l = $line.Trim()
        if ($l -match "^File '(.+)'$") { $cur = $Matches[1] }
        elseif ($cur -and $l -match '^Lines executed:([\d.]+)% of (\d+)') {
            $pct = [double]$Matches[1]
            $tot = [int]$Matches[2]
            $cov = [int][math]::Round($pct / 100.0 * $tot)
            if (-not $agg.ContainsKey($cur)) { $agg[$cur] = @{ cov = 0; tot = 0 } }
            $agg[$cur].cov += $cov
            $agg[$cur].tot += $tot
            $cur = $null
        }
    }
}

Write-Host ""
Write-Host "=== COVERAGE (per file, lines) ==="
$tc = 0
$tt = 0
foreach ($f in ($agg.Keys | Sort-Object)) {
    $a = $agg[$f]
    $p = if ($a.tot) { 100.0 * $a.cov / $a.tot } else { 0 }
    $short = $f.Replace($root, "").TrimStart('\', '/')
    "{0,6:N1}%  {1,5}/{2,-5}  {3}" -f $p, $a.cov, $a.tot, $short
    $tc += $a.cov
    $tt += $a.tot
}
$tp = if ($tt) { 100.0 * $tc / $tt } else { 0 }
Write-Host ""
"TOTAL: {0:N1}%  ({1}/{2})" -f $tp, $tc, $tt
