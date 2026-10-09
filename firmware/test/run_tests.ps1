# Run the host (native) unit test suites without PlatformIO.
# Uses the portable Tiny C Compiler (TCC) + Unity, matching the V0 setup.
#
# Compiler discovery order:
#   1) $env:DCC_TEST_CC   - explicit path to a C compiler
#   2) $env:TEMP\tcc\tcc\tcc.exe  - portable Tiny C Compiler
#   3) gcc / clang / cc in PATH
#
# -Sanitize adds -fsanitize=address,undefined (needs a toolchain that ships
# libasan/libubsan; the MinGW gcc used here does not, so it is opt-in).

param([switch]$Sanitize, [string[]]$Only = @())

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$tmp  = $env:TEMP

# --- locate compiler -------------------------------------------------------
$cc = $env:DCC_TEST_CC
if (-not $cc) {
    $tccCandidates = @(
        "$tmp\tcc\tcc\tcc.exe",
        "$PSScriptRoot\tcc\tcc.exe"
    )
    foreach ($c in $tccCandidates) {
        if (Test-Path $c) { $cc = $c; break }
    }
}
if (-not $cc) {
    foreach ($name in @("gcc", "clang", "cc")) {
        $found = Get-Command $name -ErrorAction SilentlyContinue
        if ($found) { $cc = $found.Source; break }
    }
}
if (-not $cc) {
    Write-Host "ERROR: no C compiler found. Install MinGW/LLVM or download portable TCC:" -ForegroundColor Red
    Write-Host "  Invoke-WebRequest -Uri https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win32-bin.zip -OutFile `"$tmp\tcc.zip`"" -ForegroundColor Yellow
    Write-Host "  Expand-Archive `"$tmp\tcc.zip`" -DestinationPath `"$tmp\tcc`"" -ForegroundColor Yellow
    exit 1
}
Write-Host "Compiler: $cc"

# --- Unity framework -------------------------------------------------------
$unitySrc = "$tmp\unity_clone\src"
if (-not (Test-Path "$unitySrc\unity.c")) {
    Write-Host "Fetching Unity test framework..."
    git clone --depth 1 https://github.com/ThrowTheSwitch/Unity.git "$tmp\unity_clone"
    if (-not (Test-Path "$unitySrc\unity.c")) { Write-Host "ERROR: Unity fetch failed"; exit 1 }
}

# --- include paths ---------------------------------------------------------
$inc = @(
    "-I$unitySrc",
    "-I$root\test_libs\teststubs\include",
    "-I$root\components\dcc\include",
    "-I$root\components\pinmap\include",
    "-I$root\components\settings\include",
    "-I$root\components\motor\include",
    "-I$root\components\auxio\include",
    "-I$root\components\audio\include",
    "-I$root\components\sound\include",
    "-I$root\components\track\include",
    "-I$root\components\web\include",
    "-I$root\components\storage\include",
    "-I$root\components\provision\include",
    "-I$root\components\selftest\include",
    "-I$root\main"
)

$suites = @("test_dcc", "test_settings", "test_motor", "test_auxio",
            "test_web_util", "test_track", "test_audio", "test_pinmap",
            "test_track_manifest", "test_storage", "test_track_recover", "test_provision",
            "test_selftest", "test_web", "test_sound", "test_sound_graph", "test_sound_graph_store", "test_audio_pack", "test_main")
$failed = 0
if ($Only.Count -gt 0) {
    foreach ($name in $Only) {
        if ($name -notin $suites) { throw "Unknown test suite: $name" }
    }
    $suites = $Only
}

foreach ($suite in $suites) {
    $srcs = @(Get-ChildItem "$root\test\$suite\*.c" -ErrorAction SilentlyContinue)
    if ($srcs.Count -eq 0) { Write-Host "SKIP $suite (no sources)"; continue }
    $exe = "$tmp\$suite.exe"
    Remove-Item $exe -ErrorAction SilentlyContinue
    Write-Host "=== $suite ===" -ForegroundColor Cyan
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $buildExit = 1
    try {
        # No global -Dstatic=: the white-box tests strip `static` for the
        # component translation unit only, so Unity keeps its own statics.
        $names = $srcs | ForEach-Object { $_.FullName }
        $extra = @()
if ($Sanitize) { $extra = @("-fsanitize=address", "-fsanitize=undefined", "-g") }
& $cc $inc $names "$unitySrc\unity.c" $extra -o $exe 2>&1 | ForEach-Object { Write-Host $_ }
        $buildExit = $LASTEXITCODE
    } catch {
        Write-Host "BUILD FAILED: $_"
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if ($buildExit -ne 0 -or -not (Test-Path $exe)) { Write-Host "BUILD FAILED"; $failed++; continue }
    & $exe 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { $failed++ }
}

$node = Get-Command node -ErrorAction SilentlyContinue
if ($node) {
    Write-Host "=== test_web_ui (Node.js) ===" -ForegroundColor Cyan
    & $node.Source --test "$root\test\test_web_ui.js"
    if ($LASTEXITCODE -ne 0) { $failed++ }
} else {
    Write-Warning "Node.js not found: JavaScript regressions were NOT run."
}

Write-Host ""
if ($failed -eq 0) { Write-Host "ALL TEST SUITES PASSED" -ForegroundColor Green }
else { Write-Host "$failed suite(s) FAILED" -ForegroundColor Red; exit 1 }
