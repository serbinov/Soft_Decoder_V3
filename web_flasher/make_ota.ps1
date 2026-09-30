# Creates the OTA image for the device's web interface from the already built
# firmware (release\flash_download_tool\firmware.bin), named with the version.
# Run by the web control panel ("Создать OTA"); can also be run by hand.

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

$src = Join-Path $repo "release\flash_download_tool\firmware.bin"
if (-not (Test-Path -LiteralPath $src)) {
    Write-Host "[ERROR] нет release\flash_download_tool\firmware.bin"
    Write-Host "        сначала соберите: firmware\build_flash_tool_files.bat"
    exit 1
}

$verFile = Join-Path $repo "firmware\version.txt"
$ver = if (Test-Path -LiteralPath $verFile) { (Get-Content -LiteralPath $verFile -Raw).Trim() } else { "" }
$rel = Join-Path $repo "release"
if (-not (Test-Path -LiteralPath $rel)) { New-Item -ItemType Directory -Path $rel | Out-Null }
$dst = Join-Path $rel ("ADDITIPUS_AURA-X" + ($(if ($ver) { "_v$ver" } else { "" })) + "_OTA.bin")

Copy-Item -LiteralPath $src -Destination $dst -Force
Write-Host ("[OK]   OTA-файл создан: release\{0} ({1} Б)" -f (Split-Path $dst -Leaf), (Get-Item $dst).Length)
