# Firmware + full AURA Sound Pack flashing for the web control panel.
#
# Everything goes over the USB/COM port; no Wi-Fi, no 20-slot PROV PUT path:
#   1. builds release\ADDITIPUS_sounds.asp from SOUND\ (up to 400 sounds), or
#      with -DefaultScheme only the sounds referenced by the built-in project,
#   2. flashes the release firmware with esptool (NVS is kept),
#   3. streams the pack as flow-controlled 32 KB PACK-DATA frames over the same
#      port; the device unpacks each payload into /userdata/audio.
#
# Only one ACK per 32 KB frame is used (not per file or per 4 KB), so the
# transfer is fast while the small USB/UART RX buffers cannot overflow.

param(
    [string]$SoundDir = "",
    [string]$Pack = "",
    [string]$Port = "",
    [switch]$DefaultScheme
)

$ErrorActionPreference = "Stop"

$web  = $PSScriptRoot
$repo = Split-Path $web -Parent

function Write-Info($m) { Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "[OK]   $m" -ForegroundColor Green }
function Write-Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Write-Err($m)  { Write-Host "[ERROR] $m" -ForegroundColor Red }

function Invoke-Child([string]$ScriptPath, [string[]]$ChildArgs) {
    # Write-Host keeps the child's text out of the function's return value;
    # returning the pipeline would make $code an array instead of an exit code.
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

if (-not $SoundDir) {
    foreach ($c in @((Join-Path $repo "SOUND"), (Join-Path (Split-Path $repo -Parent) "SOUND"))) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Container)) {
            $SoundDir = (Resolve-Path -LiteralPath $c).Path
            break
        }
    }
}
if (-not $SoundDir -or -not (Test-Path -LiteralPath $SoundDir -PathType Container)) {
    Write-Err "SOUND folder not found (expected $repo\SOUND)."
    exit 1
}
$wavs = @(Get-ChildItem -LiteralPath $SoundDir -Filter *.wav -ErrorAction SilentlyContinue)
if ($wavs.Count -eq 0) {
    Write-Err "No WAV files in $SoundDir"
    exit 1
}
$manifest = Join-Path $repo "firmware\sound_editor\default_scheme_sounds.txt"
if (-not $Pack) {
    if ($DefaultScheme) { $Pack = Join-Path $repo "release\ADDITIPUS_scheme_sounds.asp" }
    else { $Pack = Join-Path $repo "release\ADDITIPUS_sounds.asp" }
}
if ($DefaultScheme -and -not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
    Write-Err "Default scheme sound manifest not found: $manifest"
    exit 1
}
if (-not $Port) { $Port = Find-EspPort }
if (-not $Port) {
    Write-Err "ESP32-S3 USB-Serial/JTAG port not found. Check USB."
    exit 1
}

Write-Info "Sounds : $SoundDir ($($wavs.Count) files)"
if ($DefaultScheme) { Write-Info "Mode   : default sound scheme only ($manifest)" }
Write-Info "Pack   : $Pack"
Write-Info "Port   : $Port"

# 1. Build the .asp pack before touching the device. In scheme mode the packer
#    keeps only sounds referenced by the built-in default project.
$packArgs = @("-SoundDir", $SoundDir, "-Output", $Pack)
if ($DefaultScheme) { $packArgs += @("-OnlyList", $manifest) }
$code = Invoke-Child (Join-Path $web "make_sound_pack.ps1") $packArgs
if ($code -ne 0) {
    Write-Err "Sound pack build failed (code $code)."
    exit 1
}
if (-not (Test-Path -LiteralPath $Pack -PathType Leaf)) {
    Write-Err "Sound pack was not created: $Pack"
    exit 1
}
$packLen = (Get-Item -LiteralPath $Pack).Length
if ($packLen -gt (15 * 1024 * 1024)) {
    Write-Err ("Sound pack is {0:N2} MB; the decoder accepts at most 15 MB." -f ($packLen / 1MB))
    exit 1
}

# 2. Flash firmware only. NVS is preserved; the external NOR is formatted by
#    the PACK command below.
$flashArgs = @("-Port", $Port)
$code = Invoke-Child (Join-Path $repo "firmware\flash_esptool.ps1") $flashArgs
if ($code -eq 2) {
    Write-Warn "esptool/ESP-IDF not found; using the ROM flasher fallback."
    $code = Invoke-Child (Join-Path $repo "firmware\flash_standalone.ps1") @("-Port", $Port, "-NoErase", "-NoSounds")
}
if ($code -ne 0) {
    Write-Err "Firmware flashing failed (code $code)."
    exit 2
}

# 3. Stream the pack over the same COM port. provision_pack.ps1 retries PROV
#    while the app is booting, so no fixed sleep is required.
Write-Info "Waiting for the app and uploading the pack over COM..."
Start-Sleep -Seconds 2
$code = Invoke-Child (Join-Path $repo "firmware\provision_pack.ps1") @("-Port", $Port, "-Pack", $Pack)
if ($code -ne 0) {
    Write-Err "Sound pack upload failed (code $code); firmware is already flashed."
    exit 3
}

if ($DefaultScheme) {
    Write-Ok "DONE: firmware flashed and default-scheme sounds installed over COM."
} else {
    Write-Ok "DONE: firmware flashed and full sound pack installed over COM."
}
exit 0
