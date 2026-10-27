# Run the host (native) unit test suites.
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
# Do not use $isWindows: PowerShell 7 defines it as a read-only automatic
# variable, and assigning to it kills the run on windows-latest.
$onWindows = ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT)

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
    foreach ($name in @("gcc", "x86_64-w64-mingw32-gcc", "clang", "cc")) {
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

# Native stderr (Python unittest writes its report there) must be shown instead
# of being turned into a terminating PowerShell error by $ErrorActionPreference.
function Invoke-TestProcess {
    param([string]$Exe, [string[]]$Arguments, [string]$Label)
    Write-Host "=== $Label ===" -ForegroundColor Cyan
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            # Native stderr becomes ErrorRecord objects under PowerShell 5.1.
            # Print the actual message (including empty lines) instead of the
            # RemoteException type name, so Python unittest output is intact.
            if ($_ -is [System.Management.Automation.ErrorRecord]) {
                Write-Host $_.Exception.Message
            } else {
                Write-Host $_
            }
        }
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
}

# --- Unity framework -------------------------------------------------------
$unitySrc = "$tmp\unity_clone\src"
if (-not (Test-Path "$unitySrc\unity.c")) {
    Write-Host "Fetching Unity test framework..."
    git clone --depth 1 https://github.com/ThrowTheSwitch/Unity.git "$tmp\unity_clone"
    if (-not (Test-Path "$unitySrc\unity.c")) { Write-Host "ERROR: Unity fetch failed"; exit 1 }
}

# --- generated web header --------------------------------------------------
# web.c includes the generated web_html.h. A clean CI checkout has no header
# (the ESP-IDF build creates it), so generate it before compiling C suites.
$webPython = Get-Command python -ErrorAction SilentlyContinue
if (-not $webPython) { $webPython = Get-Command py -ErrorAction SilentlyContinue }
if (-not $webPython) {
    Write-Host "ERROR: Python is required to generate web_html.h" -ForegroundColor Red
    exit 1
}
& $webPython.Source "$root\tools\gen_web_html.py"
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: web_html.h generation failed" -ForegroundColor Red
    exit 1
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
            "test_selftest", "test_web", "test_sound_graph", "test_sound_graph_store",
            "test_audio_pack", "test_main", "test_concurrency",
            "test_motor_concurrency")
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
        $suiteFlags = @()
        if ($suite -eq "test_concurrency" -or $suite -eq "test_motor_concurrency") {
            $suiteFlags += "-DMOCK_FREERTOS_REAL_SYNC"
            if (-not $onWindows) { $suiteFlags += "-pthread" }
        }
        if ($suite -eq "test_motor_concurrency") {
            # Reuse the host LEDC register shim used by test_motor.
            $suiteFlags += "-I$root\test\test_motor"
        }
& $cc $inc $names "$unitySrc\unity.c" $suiteFlags $extra -o $exe 2>&1 | ForEach-Object { Write-Host $_ }
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

# --- non-C suites: discover Node and Python independently ------------------
# Missing tools always make the run incomplete: never print "ALL TEST SUITES
# PASSED" after silently skipping a language.
$node = Get-Command node -ErrorAction SilentlyContinue
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
$missingTools = @()
if (-not $node)   { $missingTools += "Node.js" }
if (-not $python) { $missingTools += "Python" }
foreach ($tool in $missingTools) {
    Write-Host "ERROR: required test tool missing: $tool" -ForegroundColor Red
    $failed++
}

if ($node) {
    if ((Invoke-TestProcess $node.Source @("--test", "$root\test\test_web_ui.js") "test_web_ui (Node.js)") -ne 0) { $failed++ }
    if ((Invoke-TestProcess $node.Source @("--test", "$root\test\test_blocks_editor.js") "test_blocks_editor (Node.js)") -ne 0) { $failed++ }
}
if ($python) {
    if ((Invoke-TestProcess $python.Source @("$root\test\test_sound_editor_assets.py") "test_sound_editor_assets (Python)") -ne 0) { $failed++ }
    if ((Invoke-TestProcess $python.Source @("$root\test\test_release_artifacts.py") "test_release_artifacts (Python)") -ne 0) { $failed++ }
    if ((Invoke-TestProcess $python.Source @("$root\test\test_pack_crypto.py") "test_pack_crypto (Python)") -ne 0) { $failed++ }
}
if ($node -and $python) {
    if ((Invoke-TestProcess $node.Source @("--test", "$root\test\test_pack_integration.js") "test_pack_integration (Node.js + Python)") -ne 0) { $failed++ }
} else {
    Write-Host "SKIP test_pack_integration (requires both Node.js and Python)" -ForegroundColor Yellow
}

Write-Host ""
if ($missingTools.Count -gt 0) {
    Write-Host "INCOMPLETE: missing required tool(s): $($missingTools -join ', ')" -ForegroundColor Red
}
if ($failed -eq 0) { Write-Host "ALL TEST SUITES PASSED" -ForegroundColor Green }
else { Write-Host "$failed suite(s) FAILED" -ForegroundColor Red; exit 1 }
