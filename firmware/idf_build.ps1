# Native ESP-IDF build/flash for Soft Decoder V3.
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
    [switch]$Clean,
    [switch]$Release
)

$ErrorActionPreference = "Stop"
$fw = $PSScriptRoot

# Single-quote a value for the child PowerShell command line.
function Quote-PS([string]$Value) {
    return "'" + ($Value -replace "'", "''") + "'"
}

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
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like "USB\VID_303A*PID_1001*" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } } |
        Select-Object -First 1
}

if (($Flash -or $Erase -or $Monitor) -and -not $Port) {
    $Port = Get-EspPort
    if (-not $Port) {
        Write-Host "ERROR: ESP32-S3 (VID_303A/PID_1001) not found. Specify -Port COMx." -ForegroundColor Red
        exit 1
    }
    Write-Host "Detected ESP32-S3 on $Port" -ForegroundColor Cyan
}

# --- build the idf.py command sequence -------------------------------------
$toolsDir = Join-Path $fw "tools"
$releaseTool = Join-Path $toolsDir "release_artifacts.py"
$appBin = Join-Path $fw "build\soft_decoder_v3.bin"
$steps = @()
if (-not (Test-Path (Join-Path $fw "build\CMakeCache.txt"))) {
    $steps += "idf.py set-target esp32s3"
}
if ($Clean) { $steps += "idf.py fullclean" }
$steps += "idf.py build"
if ($Flash -or $Erase) {
    # Refuse to flash an oversized app: inspect also enforces the ota_0 slot.
    $steps += "python " + (Quote-PS $releaseTool) + " inspect --input " + (Quote-PS $appBin) + " | Out-Null"
}
if ($Erase) { $steps += "idf.py -p " + (Quote-PS $Port) + " erase-flash" }
if ($Flash) { $steps += "idf.py -p " + (Quote-PS $Port) + " flash" }
if ($Monitor) { $steps += "idf.py -p " + (Quote-PS $Port) + " monitor" }
if ($Release) {
    if ($Flash -or $Erase -or $Monitor) {
        throw "Release packaging is build-only; do not combine it with device actions."
    }
    $releaseDir = Join-Path (Split-Path $fw -Parent) "release"
    $steps += "python " + (Quote-PS $releaseTool) + " publish --build " + (Quote-PS (Join-Path $fw "build")) +
              " --release " + (Quote-PS $releaseDir) + " --version " + (Quote-PS (Join-Path $fw "version.txt")) +
              " --partitions " + (Quote-PS (Join-Path $fw "partitions.csv"))
    $steps += "powershell -NoProfile -ExecutionPolicy Bypass -File " + (Quote-PS (Join-Path $toolsDir "gen_flash_readme.ps1")) +
              " -Out " + (Quote-PS (Join-Path $releaseDir "flash_download_tool\README.txt"))
}

# Reproducible builds: pin the embedded timestamp to the last commit (or the
# version file mtime when git is unavailable) instead of the current time.
$epoch = 0L
try {
    $commitTime = & git -C $fw log -1 --format=%ct 2>$null
    if ($LASTEXITCODE -eq 0 -and $commitTime) { $epoch = [int64]$commitTime }
} catch { }
if ($epoch -le 0) {
    $stamp = (Get-Item -LiteralPath (Join-Path $fw "version.txt")).LastWriteTimeUtc
    $epoch = [int64]([DateTimeOffset]$stamp).ToUnixTimeSeconds()
}
$env:SOURCE_DATE_EPOCH = [string]$epoch

Write-Host "ESP-IDF : $idf"
Write-Host "Tools   : $env:IDF_TOOLS_PATH"
if ($Port) { Write-Host "Port    : $Port" }
Write-Host "Epoch   : $epoch (SOURCE_DATE_EPOCH)"
Write-Host ""

# Activate the toolchain and run in a child PowerShell so a failure inside
# ESP-IDF's export.ps1 cannot terminate this wrapper prematurely.
$inner = @(
    ("`$env:IDF_PATH = " + (Quote-PS $idf)),
    ("`$env:IDF_TOOLS_PATH = " + (Quote-PS $env:IDF_TOOLS_PATH)),
    ("`$env:SOURCE_DATE_EPOCH = " + (Quote-PS $env:SOURCE_DATE_EPOCH)),
    (". " + (Quote-PS (Join-Path $idf "export.ps1"))),
    ("Set-Location " + (Quote-PS $fw)),
    ((($steps | ForEach-Object { $_ + '; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }' }) -join "`n"))
) -join "`n"

# ESP-IDF's export.ps1 writes its activation banner to stderr. With the
# wrapper's ErrorActionPreference=Stop, PowerShell 5.1 turns that harmless
# native-command stderr into a terminating NativeCommandError, so relax it
# around the child and propagate the child's real exit code.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& powershell -NoProfile -ExecutionPolicy Bypass -Command $inner 2>&1 |
    ForEach-Object { Write-Host $_ }
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap
exit $code
