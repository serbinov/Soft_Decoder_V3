# Reliable USB flashing via esptool.
#
# esptool writes the images and self-verifies the hash, so this is the
# recommended path for the control panel (the ROM-based
# firmware\flash_standalone.ps1 is used only as a fallback).
#
# esptool is looked up in this order:
#   1. a standalone esptool.exe placed next to the project (autonomous, no install):
#        firmware\tools\esptool\esptool.exe  or  firmware\esptool.exe  or  web_flasher\esptool.exe
#   2. the ESP-IDF python environment (python -m esptool)
#
#   flash_esptool.ps1                 # firmware only (settings/NVS kept)
#   flash_esptool.ps1 -Sounds         # firmware + sounds
#   flash_esptool.ps1 -Port COM13
#   flash_esptool.ps1 -Erase          # full chip erase first (wipes NVS)

param(
    [switch]$Sounds,
    [switch]$Erase,
    [string]$Port = "",
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
$fwDir = Join-Path $repo "release\flash_download_tool"

function Write-Info($m) { Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "[OK]   $m" -ForegroundColor Green }
function Write-Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Write-Err($m)  { Write-Host "[ERROR] $m" -ForegroundColor Red }

function Find-Idf {
    $cands = @()
    if ($env:IDF_PATH) { $cands += $env:IDF_PATH }
    $cands += @("C:\esp\v6.0\esp-idf", "C:\esp\esp-idf",
                (Join-Path $env:USERPROFILE "esp\v6.0\esp-idf"),
                (Join-Path $env:USERPROFILE "esp\esp-idf"))
    foreach ($c in $cands) { if ($c -and (Test-Path (Join-Path $c "tools\idf.py"))) { return (Resolve-Path $c).Path } }
    return $null
}
function Find-EspPort {
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like "USB\VID_303A*PID_1001*" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } } |
        Select-Object -First 1
}
# Returns @{ kind='exe'; path=... } or @{ kind='py'; py=... } or $null.
function Find-Esptool {
    $exeCandidates = @(
        (Join-Path $PSScriptRoot "tools\esptool\esptool.exe"),
        (Join-Path $PSScriptRoot "esptool.exe"),
        (Join-Path $repo "web_flasher\esptool.exe"),
        (Join-Path $PSScriptRoot "tools\esptool\esptool-win64\esptool.exe")
    )
    foreach ($e in $exeCandidates) { if (Test-Path -LiteralPath $e) { return @{ kind = "exe"; path = $e } } }
    if (Find-Idf) {
        $toolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { Join-Path $env:USERPROFILE ".espressif" }
        $envDir = Get-ChildItem -Path (Join-Path $toolsPath "python_env") -Directory -Filter "idf*_env" -ErrorAction SilentlyContinue |
                  Sort-Object Name | Select-Object -Last 1
        if ($envDir) {
            $py = Join-Path $envDir.FullName "Scripts\python.exe"
            if (Test-Path -LiteralPath $py) { return @{ kind = "py"; py = $py } }
        }
    }
    return $null
}

$esptool = Find-Esptool
if (-not $esptool) {
    Write-Err "esptool not found."
    Write-Warn "  Put a standalone esptool.exe in firmware\tools\esptool\esptool.exe,"
    Write-Warn "  or install ESP-IDF 6.0, or use firmware\flash_standalone.ps1 (fallback)."
    exit 2
}
if (-not $Port) { $Port = Find-EspPort }
if (-not $Port) { Write-Err "ESP32-S3 (VID_303A) not found. Check USB."; exit 1 }

if ($esptool.kind -eq "exe") { Write-Info "esptool : $($esptool.path) (standalone)" }
else { Write-Info "esptool : $($esptool.py) -m esptool (ESP-IDF python env)" }
Write-Info "Port    : $Port"

$files = @{
    "0x0"     = (Join-Path $fwDir "bootloader.bin")
    "0x8000"  = (Join-Path $fwDir "partitions.bin")
    "0x1a000" = (Join-Path $fwDir "ota_data_initial.bin")
    "0x20000" = (Join-Path $fwDir "firmware.bin")
}
foreach ($k in $files.Keys) {
    if (-not (Test-Path -LiteralPath $files[$k])) {
        Write-Err ("missing file: {0}" -f $files[$k])
        Write-Warn "  build it first: firmware\build_flash_tool_files.bat"
        exit 1
    }
}
if ($Sounds) {
    # Prefer <repo>\SOUND (Soft_Decoder_V3\SOUND); keep the sibling-of-repo
    # location as a fallback.
    $snd = $null
    foreach ($c in @((Join-Path $repo "SOUND"), (Join-Path (Split-Path $repo -Parent) "SOUND"))) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Container)) { $snd = (Resolve-Path -LiteralPath $c).Path; break }
    }
    if (-not $snd) { Write-Err "SOUND folder not found"; exit 1 }
}

$args8 = @("--chip", "esp32s3", "--port", $Port, "--baud", $Baud,
           "--before", "default-reset", "--after", "hard-reset",
           "write-flash", "--flash-mode", "dio", "--flash-size", "4MB", "--flash-freq", "80m")
if ($Erase) { $args8 += "--erase-all" }
foreach ($o in @("0x0", "0x8000", "0x1a000", "0x20000")) { $args8 += $o; $args8 += $files[$o] }

Write-Info "Flashing firmware (esptool, self-verifying)..."
if ($esptool.kind -eq "exe") { & $esptool.path @args8 } else { & $esptool.py @("-m", "esptool") @args8 }
if ($LASTEXITCODE -ne 0) { Write-Err "esptool flashing failed (code $LASTEXITCODE)"; exit $LASTEXITCODE }
Write-Ok "firmware flashed and verified"

if ($Sounds) {
    Write-Info "waiting for app boot..."
    Start-Sleep -Seconds 3
    Write-Info "uploading sounds (PROV)..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "provision_sounds.ps1") -Port $Port -SoundDir $snd
    if ($LASTEXITCODE -ne 0) { Write-Err "sound upload failed (code $LASTEXITCODE)"; exit $LASTEXITCODE }
}

Write-Ok "DONE"
