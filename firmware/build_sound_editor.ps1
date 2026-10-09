$ErrorActionPreference = "Stop"
$editor = Join-Path $PSScriptRoot "sound_editor"
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw "Node.js is required on the build computer."
}
Push-Location -LiteralPath $editor
try {
    & node "scripts\package-assets.mjs"
    if ($LASTEXITCODE -ne 0) { throw "Sound block editor build failed." }
} finally {
    Pop-Location
}
