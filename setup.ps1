# One-time environment setup for Soft Decoder V3 (ESP-IDF 6.0, no PlatformIO).
#
#   .\setup.ps1                  # find ESP-IDF, write firmware\.idf_path, first build
#   .\setup.ps1 -IdfPath C:\esp\v6.0\esp-idf
#   .\setup.ps1 -Install         # clone + install ESP-IDF v6.0 if missing (~2 GB)
#   .\setup.ps1 -NoBuild         # skip the initial build
#   .\setup.ps1 -NoExtensions    # do not touch VS Code extensions
#
# After it finishes, VS Code (.vscode/*) is ready: Build / Flash / Erase /
# Monitor tasks and C/C++ IntelliSense via firmware\build\compile_commands.json.

param(
    [string]$IdfPath,
    [switch]$Install,
    [switch]$NoBuild,
    [switch]$NoExtensions
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$fw = Join-Path $root "firmware"

function Write-Head([string]$t) { Write-Host ""; Write-Host "== $t ==" -ForegroundColor Cyan }
function Find-Idf([string]$explicit) {
    $cands = @()
    if ($explicit) { $cands += $explicit }
    if ($env:IDF_PATH) { $cands += $env:IDF_PATH }
    $ptr = Join-Path $fw ".idf_path"
    if (Test-Path $ptr) { $cands += (Get-Content $ptr -Raw).Trim() }
    $cands += @(
        "C:\esp\v6.0\esp-idf",
        "C:\esp\esp-idf",
        (Join-Path $env:USERPROFILE "esp\v6.0\esp-idf"),
        (Join-Path $env:USERPROFILE "esp\esp-idf")
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c "tools\idf.py"))) { return (Resolve-Path $c).Path }
    }
    return $null
}

Write-Head "Prerequisites"
foreach ($tool in @("git", "python")) {
    $c = Get-Command $tool -ErrorAction SilentlyContinue
    if ($c) { Write-Host ("  [ok]   {0}: {1}" -f $tool, $c.Source) -ForegroundColor Green }
    else { Write-Host ("  [warn] {0} not found in PATH" -f $tool) -ForegroundColor Yellow }
}

Write-Head "ESP-IDF 6.0"
$idf = Find-Idf $IdfPath
if (-not $idf -and $Install) {
    $idf = "C:\esp\v6.0\esp-idf"
    Write-Host "  Cloning ESP-IDF v6.0 to $idf ..."
    git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git $idf
    Write-Host "  Installing tools (esp32s3) ..."
    & "$idf\install.bat" esp32s3
    $idf = Find-Idf $idf
}
if (-not $idf) {
    Write-Host "  [x] ESP-IDF 6.0 not found." -ForegroundColor Red
    Write-Host "      Run:  .\setup.ps1 -Install" -ForegroundColor Yellow
    Write-Host "      or clone manually:" -ForegroundColor Yellow
    Write-Host "        git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git C:\esp\v6.0\esp-idf" -ForegroundColor Yellow
    Write-Host "        C:\esp\v6.0\esp-idf\install.bat esp32s3" -ForegroundColor Yellow
    exit 1
}
Write-Host ("  [ok]   {0}" -f $idf) -ForegroundColor Green

$ptr = Join-Path $fw ".idf_path"
[System.IO.File]::WriteAllText($ptr, $idf + "`n")
Write-Host "  [ok]   wrote firmware\.idf_path"

if (-not $NoExtensions) {
    Write-Head "VS Code extensions"
    $code = Get-Command code -ErrorAction SilentlyContinue
    $exts = @("ms-vscode.cpptools", "espressif.esp-idf-extension")
    if ($code) {
        foreach ($e in $exts) {
            Write-Host "  installing $e ..."
            try { & code --install-extension $e --force | Out-Null } catch { }
        }
        Write-Host "  [ok]   extensions processed" -ForegroundColor Green
    } else {
        Write-Host "  'code' CLI not found. Install these extensions in VS Code:" -ForegroundColor Yellow
        foreach ($e in $exts) { Write-Host "    - $e" }
    }
}

if (-not $NoBuild) {
    Write-Head "Initial build (generates firmware\build\compile_commands.json)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $fw "idf_build.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Host "  [x] build failed" -ForegroundColor Red; exit 1 }
}

Write-Head "Ready"
Write-Host "  Build/flash : .\firmware\idf_build.ps1 -Flash"
Write-Host "  Host tests  : powershell -ExecutionPolicy Bypass -File firmware\test\run_tests.ps1"
Write-Host "  VS Code     : open this folder; use the 'ESP-IDF: ...' tasks (Ctrl+Shift+B = build)"
