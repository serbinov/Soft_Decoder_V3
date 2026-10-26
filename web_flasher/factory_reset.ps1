# Full factory wipe for the web control panel.
#
# 1. Erases the entire internal flash (bootloader, partitions, NVS, application)
#    and re-flashes the release firmware, so settings/CVs/metadata are gone.
# 2. Enters the firmware provisioning console and sends WIPE, which formats the
#    external NOR: sounds, graph projects, tracks.txt/volumes.txt are removed.
#
# The board ends in the same state as a freshly built/reflashed unit: default
# NVS, no sound library, built-in scheme installed on the next boot.
#
# Run only from the panel's explicit "Стереть всё" confirmation.

param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"

$web  = $PSScriptRoot
$repo = Split-Path $web -Parent

function Write-Info($m) { Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "[OK]   $m" -ForegroundColor Green }
function Write-Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Write-Err($m)  { Write-Host "[ERROR] $m" -ForegroundColor Red }

function Invoke-Child([string]$ScriptPath, [string[]]$ChildArgs) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @ChildArgs 2>&1 |
        ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}

function Find-EspPort {
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like "USB\VID_303A*PID_1001*" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } } |
        Select-Object -First 1
}

if (-not $Port) { $Port = Find-EspPort }
if (-not $Port) {
    Write-Err "ESP32-S3 USB-Serial/JTAG port not found. Check USB."
    exit 1
}

Write-Warn "FACTORY WIPE: internal flash and external sound storage will be erased."
Write-Info "Port : $Port"

# 1. Full internal erase + release firmware. esptool writes with --erase-all.
$code = Invoke-Child (Join-Path $repo "firmware\flash_esptool.ps1") @("-Port", $Port, "-Erase")
if ($code -eq 2) {
    Write-Warn "esptool/ESP-IDF not found; using the ROM flasher fallback."
    $code = Invoke-Child (Join-Path $repo "firmware\flash_standalone.ps1") @("-Port", $Port, "-Erase", "-NoSounds")
}
if ($code -ne 0) {
    Write-Err "Internal flash erase/flash failed (code $code)."
    exit 2
}

# 2. Let the freshly flashed app boot, then format the external NOR over COM.
Write-Info "Waiting for the app, then formatting external storage..."
Start-Sleep -Seconds 2
$code = Invoke-Child (Join-Path $repo "firmware\provision_pack.ps1") @("-Port", $Port, "-Wipe")
if ($code -ne 0) {
    Write-Err "Firmware is flashed, but external storage wipe failed (code $code)."
    exit 3
}

Write-Ok "DONE: factory wipe complete (internal flash + external storage)."
exit 0
