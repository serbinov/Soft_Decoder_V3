# Increment the firmware minor version by 1 (e.g. 0.4 -> 0.5).
# Updates version.txt (single source of truth) and the CV7 (decoder version)
# default in settings.c. The web page version is substituted at build time from
# version.txt (tools/gen_web_html.py).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vf = Join-Path $root "version.txt"

if (Test-Path $vf) {
    $cur = (Get-Content $vf -Raw).Trim()
} else {
    $cur = "0.0"
}
if ($cur -notmatch '^\d+\.\d+$') {
    Write-Host "version.txt contains '$cur' (expected MAJOR.MINOR); resetting to 0.0"
    $cur = "0.0"
}
$parts = $cur.Split('.')
$major = [int]$parts[0]
$minor = [int]$parts[1] + 1
$new = "$major.$minor"

# 1) single source of truth
Set-Content -Path $vf -Value $new -NoNewline -Encoding ascii

# 2) NMRA CV7 (decoder version) default -> minor number. Fail loudly if the
#    pattern is gone, otherwise an external/stale source could silently diverge.
$sc = Join-Path $root "components\settings\src\settings.c"
if (-not (Test-Path $sc)) { throw "not found: $sc" }
$t = Get-Content $sc -Raw
$t2 = [regex]::Replace($t, 's_cv\[7\] = \d+;', "s_cv[7] = $minor;")
if ($t2 -eq $t) { throw "CV7 pattern 's_cv[7] = N;' not found in settings.c" }
[System.IO.File]::WriteAllText($sc, $t2)   # UTF-8 without BOM

# 3) Keep the native CV7 expectations in test_settings in sync (also written
#    without BOM so the Tiny C Compiler used by run_tests.ps1 parses it cleanly).
$tc = Join-Path $root "test\test_settings\test_settings.c"
if (-not (Test-Path $tc)) { throw "not found: $tc" }
$tt = Get-Content $tc -Raw
$pat1 = 'TEST_ASSERT_EQUAL_UINT8\(\d+, s_cv\[7\]\);'
$pat2 = 'TEST_ASSERT_EQUAL_UINT8\(\d+, v\); /\* unchanged default \*/'
if (([regex]::Matches($tt, $pat1)).Count -lt 1 -or
    ([regex]::Matches($tt, $pat2)).Count -lt 1) {
    throw "CV7 expectations not found in test_settings.c"
}
$tt = [regex]::Replace($tt, $pat1, "TEST_ASSERT_EQUAL_UINT8($minor, s_cv[7]);")
$tt = [regex]::Replace($tt, $pat2, "TEST_ASSERT_EQUAL_UINT8($minor, v); /* unchanged default */")
[System.IO.File]::WriteAllText($tc, $tt)

# 4) Report.
Write-Host "Version: $cur -> $new"
Write-Host "Updated: version.txt, settings.c CV7, test_settings.c expectations"
Write-Host "Now run build_ota_bin.bat (or pio run) to build ADDITIPUS_AURA-X_v$new.bin"
