# Increment the firmware minor version by 1 (e.g. 0.10 -> 0.11).
# version.txt is the single source of truth; CV7 mirrors its minor number.
# The web page version is substituted at build time from version.txt
# (tools/gen_web_html.py).
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

Set-Content -Path $vf -Value $new -NoNewline -Encoding ascii

$sc = Join-Path $root "components\settings\src\settings.c"
if (-not (Test-Path $sc)) { throw "not found: $sc" }
$t = [System.IO.File]::ReadAllText($sc)
$pattern = '#define SETTINGS_CV7_VERSION \d+'
if ($t -notmatch $pattern) {
    throw "SETTINGS_CV7_VERSION pattern not found in settings.c"
}
$t2 = [regex]::Replace($t, $pattern, "#define SETTINGS_CV7_VERSION $minor")
[System.IO.File]::WriteAllText($sc, $t2)

# Older test revisions hard-coded the CV7 value. Keep those in sync when the
# patterns still exist; current tests derive it from version.txt.
$tc = Join-Path $root "test\test_settings\test_settings.c"
if (Test-Path $tc) {
    $tt = [System.IO.File]::ReadAllText($tc)
    $pat1 = 'TEST_ASSERT_EQUAL_UINT8\(\d+, s_cv\[7\]\);'
    $pat2 = 'TEST_ASSERT_EQUAL_UINT8\(\d+, v\); /\* unchanged default \*/'
    if (([regex]::Matches($tt, $pat1)).Count -gt 0) {
        $tt = [regex]::Replace($tt, $pat1, "TEST_ASSERT_EQUAL_UINT8($minor, s_cv[7]);")
        [System.IO.File]::WriteAllText($tc, $tt)
    }
    if (([regex]::Matches($tt, $pat2)).Count -gt 0) {
        $tt = [regex]::Replace($tt, $pat2, "TEST_ASSERT_EQUAL_UINT8($minor, v); /* unchanged default */")
        [System.IO.File]::WriteAllText($tc, $tt)
    }
}

Write-Host "Version: $cur -> $new"
Write-Host "Updated: version.txt and SETTINGS_CV7_VERSION"
Write-Host "Run build_firmware.bat (repo root) to build ADDITIPUS_AURA-X_v$new.bin"
