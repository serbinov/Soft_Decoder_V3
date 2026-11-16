# Full-chip wipe for the web control panel.
#
# Leaves NOTHING on the decoder:
#   - every block of the external NOR (sounds, graph projects, sidecars, NVS
#     copies stored on LittleFS) is erased by the running firmware via WIPECHIP,
#   - the whole internal flash (bootloader, partition table, otadata, NVS,
#     settings/CVs, application) is erased by esptool/ROM and NOT rewritten.
#
# The decoder ends blank: it does not boot until a release image is flashed
# again. This is intentionally more destructive than the factory reset.
#
# Run only from the panel's explicit "Стереть чипы полностью" confirmation.
#
# Order matters: the external NOR can only be erased by firmware, so a release
# image is flashed first (without erasing the external storage). The internal
# flash is erased last, which is what makes the board blank.

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

Write-Warn "FULL CHIP WIPE: the decoder will be left without firmware, settings or sounds."
Write-Info "Port : $Port"

# 1. Put the release application on the board WITHOUT erasing anything, so it
#    can erase its own external NOR over the provisioning console.
$code = Invoke-Child (Join-Path $repo "firmware\flash_esptool.ps1") @("-Port", $Port)
if ($code -eq 2) {
    Write-Warn "esptool/ESP-IDF not found; using the ROM flasher fallback."
    $code = Invoke-Child (Join-Path $repo "firmware\flash_standalone.ps1") @("-Port", $Port, "-NoErase", "-NoSounds")
}
if ($code -ne 0) {
    Write-Err "cannot boot the release firmware (code $code); nothing was erased."
    exit 2
}

# 2. Erase every block of the external NOR. This is the slow part on large or
#    unfamiliar chips; the script reports ERASE-PROGRESS and waits up to 10 min.
Write-Info "Waiting for the app, then erasing the external NOR chip..."
Start-Sleep -Seconds 2
$code = Invoke-Child (Join-Path $repo "firmware\provision_pack.ps1") @("-Port", $Port, "-EraseChip")
if ($code -ne 0) {
    Write-Err "external NOR erase failed (code $code); internal firmware was NOT erased."
    exit 3
}

# 3. Erase the whole internal flash and write nothing: the board is now blank.
Start-Sleep -Seconds 1
$code = Invoke-Child (Join-Path $repo "firmware\flash_esptool.ps1") @("-Port", $Port, "-EraseOnly")
if ($code -eq 2) {
    Write-Warn "esptool/ESP-IDF not found; using the ROM flasher fallback."
    $code = Invoke-Child (Join-Path $repo "firmware\flash_standalone.ps1") @("-Port", $Port, "-EraseOnly")
}
if ($code -ne 0) {
    Write-Err "external NOR is blank, but the internal flash erase failed (code $code)."
    exit 4
}

Write-Ok "DONE: both chips are blank. Flash a release image to use the decoder again."
exit 0
