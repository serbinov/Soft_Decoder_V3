param([switch]$InstallDependencies)

$ErrorActionPreference = "Stop"
$editor = Join-Path $PSScriptRoot "sound_editor"
if (-not (Test-Path -LiteralPath (Join-Path $editor "package-lock.json"))) {
    throw "Sound editor source/lockfile missing."
}
if (-not (Get-Command node -ErrorAction SilentlyContinue) -or
    -not (Get-Command npm.cmd -ErrorAction SilentlyContinue)) {
    throw "Node.js and npm are required on the build computer."
}
Push-Location -LiteralPath $editor
try {
    if ($InstallDependencies) {
        & npm.cmd ci
        if ($LASTEXITCODE -ne 0) { throw "npm ci failed." }
    } elseif (-not (Test-Path -LiteralPath "node_modules")) {
        throw "Dependencies missing. Run build_sound_editor.ps1 -InstallDependencies explicitly."
    }
    & npm.cmd run check
    if ($LASTEXITCODE -ne 0) { throw "Sound editor type checks failed." }
    & npm.cmd run build
    if ($LASTEXITCODE -ne 0) { throw "Sound editor production build failed." }
} finally {
    Pop-Location
}
