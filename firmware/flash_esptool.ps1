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

# Pin a downloaded esptool.exe with a sidecar hash when one is shipped:
#   firmware\tools\esptool\esptool.exe.sha256  or  ...\esptool.sha256
# The file may be a bare digest or sha256sum output ("<hash>  <file>").
function Test-EsptoolHash([string]$exe) {
    $sidecars = @("$exe.sha256", (Join-Path (Split-Path $exe -Parent) "esptool.sha256"))
    $pin = $sidecars | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    $expected = $env:AURA_ESPTOOL_SHA256
    if (-not $expected -and $pin) {
        $text = (Get-Content -LiteralPath $pin -Raw).Trim()
        $expected = (($text -split "\s+") | Select-Object -First 1)
    }
    if (-not $expected) {
        Write-Warn "esptool SHA256 is not pinned (set AURA_ESPTOOL_SHA256 or add esptool.sha256 next to the exe)."
        return
    }
    $expected = $expected.ToLowerInvariant()
    if ($expected -notmatch "^[0-9a-f]{64}$") {
        throw "Invalid esptool SHA256 pin (need 64 hex characters)."
    }
    $actual = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "esptool SHA256 mismatch: $exe (expected $expected, got $actual)"
    }
    Write-Info "esptool SHA256 verified"
}

# Refuse to flash a release whose manifest no longer matches its files, and
# always reject an application larger than the ota_0 slot.
function Test-ReleasePreflight([string]$dir) {
    $app = Join-Path $dir "firmware.bin"
    if (-not (Test-Path -LiteralPath $app)) { return }
    $slotSize = [int64]0x1E0000
    $appLen = (Get-Item -LiteralPath $app).Length
    if ($appLen -gt $slotSize) {
        throw ("firmware.bin is {0:N0} bytes; the ota_0 slot is only {1:N0} bytes" -f $appLen, $slotSize)
    }
    $release = Split-Path $dir -Parent
    $manifestPath = Join-Path $release "manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        Write-Warn "release manifest not found; only the application size was checked"
        return
    }
    $tool = Join-Path $PSScriptRoot "tools\release_artifacts.py"
    $python = if ($esptool.kind -eq "py") { $esptool.py } else {
        $cmd = Get-Command python -ErrorAction SilentlyContinue
        if ($cmd) { $cmd.Source } else { $null }
    }
    if ($python -and (Test-Path -LiteralPath $tool)) {
        Write-Info "verifying release images..."
        & $python $tool verify --release $release
        if ($LASTEXITCODE -ne 0) { throw "release verification failed; rebuild before flashing" }
        Write-Ok "release images verified"
        return
    }
    Write-Warn "Python not found; checking release hashes with PowerShell"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.chip -ne "esp32s3" -or $manifest.flash_size -ne 0x400000) {
        throw "invalid release manifest"
    }
    foreach ($name in @("bootloader.bin", "partitions.bin", "ota_data_initial.bin", "firmware.bin")) {
        $entry = @($manifest.files | Where-Object { $_.path -ceq ("flash_download_tool/" + $name) })
        if ($entry.Count -ne 1) { throw ("release manifest is missing " + $name) }
        $path = Join-Path $dir $name
        if ((Get-Item -LiteralPath $path).Length -ne [int64]$entry[0].size) { throw ("release size mismatch: " + $name) }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry[0].sha256) {
            throw ("release hash mismatch: " + $name)
        }
    }
    Write-Ok "release images verified"
}

$esptool = Find-Esptool
if (-not $esptool) {
    Write-Err "esptool not found."
    Write-Warn "  Put a standalone esptool.exe in firmware\tools\esptool\esptool.exe,"
    Write-Warn "  or install ESP-IDF 6.0, or use firmware\flash_standalone.ps1 (fallback)."
    exit 2
}
if (-not $Port) { $Port = Find-EspPort }
if (-not $Port) { Write-Err "ESP32-S3 (VID_303A/PID_1001) not found. Specify -Port COMx or check USB."; exit 1 }

if ($esptool.kind -eq "exe") {
    Write-Info "esptool : $($esptool.path) (standalone)"
    try { Test-EsptoolHash $esptool.path } catch { Write-Err $_.Exception.Message; exit 1 }
} else {
    Write-Info "esptool : $($esptool.py) -m esptool (ESP-IDF python env)"
}
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
        Write-Warn "  build it first: build_firmware.bat"
        exit 1
    }
}
try { Test-ReleasePreflight $fwDir } catch { Write-Err $_.Exception.Message; exit 1 }
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
