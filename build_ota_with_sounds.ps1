param(
    [string]$SoundDir = (Join-Path $PSScriptRoot "..\..\SOUND")
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
if (-not (Test-Path $pio)) {
    Write-Host "[ERROR] PlatformIO not found: $pio" -ForegroundColor Red
    exit 1
}

$version = (Get-Content (Join-Path $PSScriptRoot "version.txt") -Raw).Trim()
$src = Join-Path $PSScriptRoot ".pio\build\esp32-s3-devkitc-1\firmware.bin"
$dst = Join-Path $PSScriptRoot "..\release\ADDITIPUS_AURA-X_v$version`_with_sounds.bin"

if (-not (Test-Path $SoundDir)) {
    Write-Host "[ERROR] Sound folder not found: $SoundDir" -ForegroundColor Red
    exit 1
}
$files = @(Get-ChildItem -LiteralPath $SoundDir -Filter *.wav | Sort-Object Name)
if ($files.Count -eq 0) {
    Write-Host "[ERROR] No WAV files in $SoundDir" -ForegroundColor Red
    exit 1
}

Write-Host "================================================"
Write-Host " Build OTA + sounds v$version"
Write-Host "================================================"
& $pio run -e esp32-s3-devkitc-1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[FAIL] Build failed." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $src)) {
    Write-Host "[ERROR] firmware.bin not found: $src" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null

$fw = [System.IO.File]::ReadAllBytes($src)
$out = [System.IO.File]::Create($dst)
$bw = New-Object System.IO.BinaryWriter($out)
try {
    # Header: magic "AURAOTA2" + firmware length + file count (16 B total).
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("AURAOTA2"))
    $bw.Write([uint32]$fw.Length)
    $bw.Write([uint32]$files.Count)
    $bw.Write($fw)

    # Files are named slotN.wav (slot = sorted order) and keep the original
    # file name as the label, exactly like provision_sounds.ps1 does.
    $slot = 1
    foreach ($f in $files) {
        $name = "slot$slot.wav"
        $label = $f.Name
        $data = [System.IO.File]::ReadAllBytes($f.FullName)
        $nb = [System.Text.Encoding]::UTF8.GetBytes($name)
        $lb = [System.Text.Encoding]::UTF8.GetBytes($label)
        $bw.Write([uint16]$nb.Length)
        $bw.Write([uint16]$lb.Length)
        $bw.Write([uint32]$data.Length)
        $bw.Write($nb)
        $bw.Write($lb)
        $bw.Write($data)
        Write-Host ("[OK] slot {0}: {1} ({2} B)" -f $slot, $f.Name, $data.Length) -ForegroundColor Green
        $slot++
    }
} finally {
    $bw.Dispose()
    $out.Dispose()
}

$size = (Get-Item $dst).Length
Write-Host ""
Write-Host ("[OK] Combined OTA created: {0} ({1:N0} bytes, {2} sounds)" -f $dst, $size, $files.Count) -ForegroundColor Green
Write-Host "[INFO] Upload it on the ОБНОВЛЕНИЕ page (firmware + sounds in one file)."
