param(
    [string]$SoundDir = ""
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

# Prefer <repo>\SOUND (Soft_Decoder_V3\SOUND); keep the sibling-of-repo fallback.
if (-not $SoundDir) {
    foreach ($c in @((Join-Path $PSScriptRoot "..\SOUND"), (Join-Path $PSScriptRoot "..\..\SOUND"))) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Container)) {
            $SoundDir = (Resolve-Path -LiteralPath $c).Path
            break
        }
    }
}

$version = (Get-Content (Join-Path $PSScriptRoot "version.txt") -Raw).Trim()
$src = Join-Path $PSScriptRoot "build\soft_decoder_v3.bin"
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
if ($files.Count -gt 20) { throw "The decoder accepts at most 20 WAV files." }
foreach ($file in $files) {
    if ($file.Length -eq 0 -or $file.Length -gt 2MB) { throw "WAV exceeds the 2 MiB receiver limit: $($file.Name)" }
    if ([Text.Encoding]::UTF8.GetByteCount($file.Name) -gt 63) { throw "UTF-8 label exceeds 63 bytes: $($file.Name)" }
}

Write-Host "================================================"
Write-Host " Build OTA + sounds v$version"
Write-Host "================================================"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "idf_build.ps1") -Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "[FAIL] Build/publish failed." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $src)) {
    Write-Host "[ERROR] firmware.bin not found: $src" -ForegroundColor Red
    exit 1
}

# Cross-check the application inside the combined OTA against the published
# release binary, so an OTA cannot silently package an unpublished build.
$published = Join-Path $PSScriptRoot "..\release\flash_download_tool\firmware.bin"
$srcHash = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
if (Test-Path -LiteralPath $published -PathType Leaf) {
    $publishedHash = (Get-FileHash -LiteralPath $published -Algorithm SHA256).Hash
    if ($srcHash -ne $publishedHash) {
        throw "Build output differs from the published release firmware; publish it before creating the combined OTA."
    }
} else {
    Write-Host "[WARN] Published release firmware not found; combined OTA is not cross-checked." -ForegroundColor Yellow
}

New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null

$fw = [System.IO.File]::ReadAllBytes($src)
$packageLength = [int64]16 + $fw.Length
$slot = 1
foreach ($file in $files) {
    $packageLength += 8 + [Text.Encoding]::UTF8.GetByteCount("slot$slot.wav") +
                      [Text.Encoding]::UTF8.GetByteCount($file.Name) + $file.Length
    $slot++
}
if ($fw.Length -gt 0x1e0000 -or $packageLength -gt 8MB) { throw "OTA package exceeds decoder limits." }
$temporary = $dst + ".tmp"
$out = [System.IO.File]::Create($temporary)
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

try {
    $infoText = & python (Join-Path $PSScriptRoot "tools\release_artifacts.py") inspect --input $temporary
    if ($LASTEXITCODE -ne 0) { throw "Combined OTA integrity/format verification failed." }
    $info = $infoText | ConvertFrom-Json
    if ($info.role -ne "AURAOTA2" -or $info.version -cne $version) { throw "Stale application in OTA package." }
    if ($info.application_sha256 -ne $srcHash.ToLowerInvariant()) { throw "OTA application hash mismatch." }
    Move-Item -LiteralPath $temporary -Destination $dst -Force
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
$size = (Get-Item $dst).Length
Write-Host ""
Write-Host ("[OK] Combined OTA created: {0} ({1:N0} bytes, {2} sounds)" -f $dst, $size, $files.Count) -ForegroundColor Green
Write-Host "[INFO] Upload it on the ОБНОВЛЕНИЕ page (firmware + sounds in one file)."
