# Builds an AURA Sound Pack (.asp) from the project's SOUND folder, for upload
# on the decoder web page "Загрузка звуков" (POST /api/audio/pack).
#
# Run by the web control panel ("Создать звуковой контейнер"); can also be run
# by hand:
#
#   powershell -File web_flasher\make_sound_pack.ps1
#   powershell -File web_flasher\make_sound_pack.ps1 -SoundDir C:\sounds -Output out.asp
#
# The packer (firmware\tools\pack_sounds.py) is stdlib-only Python 3; the panel
# finds `python`, `py -3` or the ESP-IDF python environment automatically.

param(
    [string]$SoundDir = "",
    [string]$Output = "",
    [int]$Rate = 22050,
    [switch]$Stereo
)

$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent

function Write-Info($m) { Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "[OK]   $m" -ForegroundColor Green }
function Write-Err($m)  { Write-Host "[ERROR] $m" -ForegroundColor Red }

function Find-SoundDir {
    foreach ($c in @((Join-Path $repo "SOUND"), (Join-Path (Split-Path $repo -Parent) "SOUND"))) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Container)) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    return $null
}

# Returns @{ exe = <path>; pre = @(<args before the script>) } or $null.
function Find-Python {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { return @{ exe = $cmd.Source; pre = @() } }
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) { return @{ exe = $py.Source; pre = @("-3") } }
    $toolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { Join-Path $env:USERPROFILE ".espressif" }
    $envDir = Get-ChildItem -Path (Join-Path $toolsPath "python_env") -Directory -Filter "idf*_env" -ErrorAction SilentlyContinue |
              Sort-Object Name | Select-Object -Last 1
    if ($envDir) {
        $exe = Join-Path $envDir.FullName "Scripts\python.exe"
        if (Test-Path -LiteralPath $exe) { return @{ exe = $exe; pre = @() } }
    }
    return $null
}

if (-not $SoundDir) { $SoundDir = Find-SoundDir }
if (-not $SoundDir -or -not (Test-Path -LiteralPath $SoundDir -PathType Container)) {
    Write-Err "Папка SOUND не найдена (ожидается $repo\SOUND)."
    exit 1
}
$wavs = @(Get-ChildItem -LiteralPath $SoundDir -Filter *.wav -ErrorAction SilentlyContinue)
if ($wavs.Count -eq 0) {
    Write-Err "В папке нет WAV-файлов: $SoundDir"
    exit 1
}

$packer = Join-Path $repo "firmware\tools\pack_sounds.py"
if (-not (Test-Path -LiteralPath $packer -PathType Leaf)) {
    Write-Err "Не найден упаковщик: $packer"
    exit 1
}

$python = Find-Python
if (-not $python) {
    Write-Err "Python 3 не найден (нужен только для создания контейнера)."
    Write-Host "        Установите Python 3 или запустите из окружения ESP-IDF."
    exit 1
}

if (-not $Output) { $Output = Join-Path $repo "release\ADDITIPUS_sounds.asp" }
$outDir = Split-Path $Output -Parent
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

Write-Info "Звуки : $SoundDir ($($wavs.Count) шт.)"
Write-Info "Python: $($python.exe)"
Write-Info "Сборка контейнера (.asp)..."
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"
$packArgs = @($python.pre) + @($packer, $SoundDir, "-o", $Output, "--rate", $Rate)
if ($Stereo) { $packArgs += "--stereo" }
& $python.exe @packArgs
if ($LASTEXITCODE -ne 0) { Write-Err "Упаковка не удалась (код $LASTEXITCODE)."; exit $LASTEXITCODE }

if (-not (Test-Path -LiteralPath $Output -PathType Leaf)) { Write-Err "Файл не создан: $Output"; exit 1 }
$size = (Get-Item -LiteralPath $Output).Length
$rel = $Output
if ($Output.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
    $rel = $Output.Substring($repo.Length).TrimStart('\', '/')
}
Write-Ok ("Контейнер создан: {0} ({1:N0} Б)" -f $rel, $size)
Write-Host "[INFO] Загрузите его на странице «Загрузка звуков» декодера (кнопка «Загрузить звуковой пакет»)."
