# Native ESP-IDF build/flash for Soft Decoder V3 - no PlatformIO required.
#
#   .\idf_build.ps1                     # build
#   .\idf_build.ps1 -Flash              # build + flash (COM auto-detect)
#   .\idf_build.ps1 -Flash -Port COM7   # explicit port
#   .\idf_build.ps1 -Flash -Monitor     # build + flash + serial monitor
#   .\idf_build.ps1 -Erase -Flash       # full chip erase first (wipes NVS)
#   .\idf_build.ps1 -Clean              # fullclean before build
#
# Requires ESP-IDF 6.0.x. ESP-IDF is located via -IdfPath, $env:IDF_PATH,
# a firmware\.idf_path file, or a few common install locations.

param(
    [string]$IdfPath,
    [string]$Port,
    [switch]$Flash,
    [switch]$Erase,
    [switch]$Monitor,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$fw = $PSScriptRoot

# --- locate ESP-IDF --------------------------------------------------------
$candidates = @()
if ($IdfPath) { $candidates += $IdfPath }
if ($env:IDF_PATH) { $candidates += $env:IDF_PATH }
$pointer = Join-Path $fw ".idf_path"
if (Test-Path $pointer) { $candidates += (Get-Content $pointer -Raw).Trim() }
$candidates += @(
    "C:\esp\v6.0\esp-idf",
    "C:\esp\esp-idf",
    (Join-Path $env:USERPROFILE "esp\v6.0\esp-idf"),
    (Join-Path $env:USERPROFILE "esp\esp-idf")
)
$idf = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c "tools\idf.py"))) {
        $idf = (Resolve-Path $c).Path
        break
    }
}
if (-not $idf) {
    Write-Host "ERROR: ESP-IDF not found." -ForegroundColor Red
    Write-Host "  Pass -IdfPath C:\path\to\esp-idf, set `$env:IDF_PATH, or create firmware\.idf_path." -ForegroundColor Yellow
    exit 1
}
if (-not $env:IDF_TOOLS_PATH) {
    $env:IDF_TOOLS_PATH = Join-Path $env:USERPROFILE ".espressif"
}

# --- COM port auto-detect (ESP32-S3 USB-Serial-JTAG: VID_303A PID_1001) -----
function Get-EspPort {
    $p = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like "USB\VID_303A*PID_1001*" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } } |
        Select-Object -First 1
    if (-not $p) { $p = "COM3" }
    return $p
}

if (($Flash -or $Monitor) -and -not $Port) {
    $Port = Get-EspPort
    Write-Host "Detected ESP32-S3 on $Port" -ForegroundColor Cyan
}

# --- build the idf.py command sequence -------------------------------------
$steps = @()
if (-not (Test-Path (Join-Path $fw "build\CMakeCache.txt"))) {
    $steps += "idf.py set-target esp32s3"
}
if ($Clean) { $steps += "idf.py fullclean" }
$steps += "idf.py build"
if ($Erase) { $steps += "idf.py -p $Port erase-flash" }
if ($Flash) { $steps += "idf.py -p $Port flash" }
if ($Monitor) { $steps += "idf.py -p $Port monitor" }

Write-Host "ESP-IDF : $idf"
Write-Host "Tools   : $env:IDF_TOOLS_PATH"
if ($Port) { Write-Host "Port    : $Port" }
Write-Host ""

# Activate the toolchain and run in a child PowerShell so a failure inside
# ESP-IDF's export.ps1 cannot terminate this wrapper prematurely.
$inner = @(
    "`$env:IDF_PATH = '$idf'",
    "`$env:IDF_TOOLS_PATH = '$env:IDF_TOOLS_PATH'",
    ". '$idf\export.ps1'",
    "Set-Location '$fw'",
    ($steps -join "; ")
) -join "`n"

& powershell -NoProfile -ExecutionPolicy Bypass -Command $inner
exit $LASTEXITCODE
