# Offline preflight shared by the local panel and OTA-copy command.
function Get-FirmwareRelease {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $result = [ordered]@{ ready = $false; version = ""; files = 0; missing = @(); errors = @() }
    $release = Join-Path $RepoRoot "release"
    $folder = Join-Path $release "flash_download_tool"
    $required = [ordered]@{
        "bootloader.bin" = @{ role = "bootloader"; offset = 0 }
        "partitions.bin" = @{ role = "partitions"; offset = 0x8000 }
        "ota_data_initial.bin" = @{ role = "otadata"; offset = 0x1a000 }
        "firmware.bin" = @{ role = "application"; offset = 0x20000 }
    }
    try {
        foreach ($name in $required.Keys) {
            if (Test-Path -LiteralPath (Join-Path $folder $name) -PathType Leaf) { $result.files++ }
            else { $result.missing += $name }
        }
        if ($result.missing.Count) { throw ("Missing release files: " + ($result.missing -join ", ")) }
        $manifest = Get-Content -LiteralPath (Join-Path $release "manifest.json") -Raw | ConvertFrom-Json
        if ($manifest.schema -ne 1 -or $manifest.chip -ne "esp32s3" -or $manifest.flash_size -ne 4194304) {
            throw "Invalid release manifest. Run build_firmware.bat."
        }
        $result.version = [string]$manifest.version
        $expectedVersion = (Get-Content -LiteralPath (Join-Path $RepoRoot "firmware\version.txt") -Raw).Trim()
        if ($result.version -cne $expectedVersion) { throw "Release version is stale. Run build_firmware.bat." }
        foreach ($name in $required.Keys) {
            $entry = @($manifest.files | Where-Object { $_.path -ceq ("flash_download_tool/" + $name) })
            if ($entry.Count -ne 1 -or $entry[0].role -ne $required[$name].role -or
                $entry[0].offset -ne $required[$name].offset) { throw ("Invalid release entry: " + $name) }
            $path = Join-Path $folder $name
            $length = (Get-Item -LiteralPath $path).Length
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($length -ne $entry[0].size -or $hash -ine $entry[0].sha256) {
                throw ("Release BIN changed or is incomplete: " + $name)
            }
            if ($name -eq "firmware.bin" -and ($entry[0].version -cne $result.version -or $length -gt 0x1e0000)) {
                throw "Application identity/size mismatch."
            }
        }
        $result.ready = $true
    } catch {
        $result.errors += $_.Exception.Message
    }
    return [pscustomobject]$result
}
