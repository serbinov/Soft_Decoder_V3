# Offline preflight shared by the local panel and release commands.
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
        # The versioned application is the single image for both USB flashing
        # and the device's web OTA page; it must not be duplicated or stale.
        $apps = @($manifest.files | Where-Object { $_.role -eq "application" -and $_.path -notlike "flash_download_tool/*" })
        if ($apps.Count -ne 1) { throw "Release must publish exactly one application BIN." }
        $imageName = "ADDITIPUS_AURA-X_v" + $result.version + ".bin"
        if ($apps[0].path -cne $imageName) { throw ("Unexpected release image name: " + $apps[0].path) }
        $imagePath = Join-Path $release $imageName
        if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) { throw ("Missing release image: " + $imageName) }
        $imageLength = (Get-Item -LiteralPath $imagePath).Length
        $imageHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash
        $firmwareHash = (Get-FileHash -LiteralPath (Join-Path $folder "firmware.bin") -Algorithm SHA256).Hash
        if ($imageLength -ne [int64]$apps[0].size -or $imageHash -ine $apps[0].sha256 -or
            $imageHash -ine $firmwareHash) {
            throw ("Release image changed or differs from firmware.bin: " + $imageName)
        }
        $duplicates = @(Get-ChildItem -LiteralPath $release -Filter "*_OTA.bin" -File -ErrorAction SilentlyContinue)
        if ($duplicates.Count) {
            # Harmless for USB flashing; publish/verify prune them. Warn only so
            # a legacy copy cannot lock the panel out of flashing.
            $result.errors += "Duplicate OTA copies are ignored (not used for flashing): " +
                (($duplicates | ForEach-Object Name) -join ", ")
        }
        # Sound packs published in the manifest are deliverables too: their
        # names, sizes and hashes must match before the panel calls the release ready.
        foreach ($entry in @($manifest.files | Where-Object { $_.role -eq "sound_pack" })) {
            if (-not $entry.path) { throw "Invalid sound pack manifest entry." }
            $path = Join-Path $release ($entry.path -replace '/', '\')
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw ("Missing release pack: " + $entry.path) }
            $length = (Get-Item -LiteralPath $path).Length
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($length -ne [int64]$entry.size -or $hash -ine $entry.sha256) {
                throw ("Release pack changed or is incomplete: " + $entry.path)
            }
        }
        $result.ready = $true
    } catch {
        $result.errors += $_.Exception.Message
    }
    return [pscustomobject]$result
}
