# =============================================================
#  ADDITIPUS AURA-X standalone flasher (no dependencies)
#  Pure PowerShell: enters download mode, erases the internal flash,
#  flashes firmware (bootloader + partitions + ota_data + app) and uploads
#  sounds. No Python / esptool / PlatformIO required.
#
#  Usage:
#    .\flash_standalone.ps1
#    .\flash_standalone.ps1 -FwDir "..\release\flash_download_tool" -SoundDir "..\..\SOUND"
#    .\flash_standalone.ps1 -NoSounds
#    .\flash_standalone.ps1 -NoErase     (skip the full internal-flash erase)
# =============================================================
param(
    [string]$FwDir = (Join-Path $PSScriptRoot "..\release\flash_download_tool"),
    [string]$SoundDir = (Join-Path $PSScriptRoot "..\..\SOUND"),
    [switch]$NoSounds,
    [switch]$NoErase,
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$Host.UI.RawUI.WindowTitle = "ADDITIPUS AURA-X flasher"

function Write-Info($m)  { Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Write-Ok($m)    { Write-Host "[OK]   $m" -ForegroundColor Green }
function Write-Warn($m)  { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Write-Err($m)   { Write-Host "[ERROR] $m" -ForegroundColor Red }

# ---------------- port detection ----------------
function Find-EspPort {
    foreach ($p in [System.IO.Ports.SerialPort]::GetPortNames()) {
        $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -match $p -and $_.DeviceID -match "VID_303A" }
        if ($dev) { return $p }
    }
    return $null
}

function Open-Serial([string]$name, [int]$baud) {
    $sp = New-Object System.IO.Ports.SerialPort
    $sp.PortName = $name
    $sp.BaudRate = $baud
    $sp.Parity = [System.IO.Ports.Parity]::None
    $sp.DataBits = 8
    $sp.StopBits = [System.IO.Ports.StopBits]::One
    $sp.Handshake = [System.IO.Ports.Handshake]::None
    $sp.ReadTimeout = 400
    $sp.WriteTimeout = 15000
    $sp.WriteBufferSize = 65536
    $sp.ReadBufferSize = 65536
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.Open()
    $sp.DiscardInBuffer()
    $sp.DiscardOutBuffer()
    return $sp
}

# ---------------- SLIP / protocol ----------------
$SLIP_END = 0xC0
$SLIP_ESC = 0xDB
$SLIP_ESC_END = 0xDC
$SLIP_ESC_ESC = 0xDD
$script:rxBuffer = New-Object System.Collections.Generic.List[byte]

function Write-SlipCmd($sp, [int]$cmd, [byte[]]$data, [int]$chk) {
    $out = New-Object System.Collections.Generic.List[byte]
    $out.Add($SLIP_END)
    $out.Add(0x00)
    $out.Add([byte]$cmd)
    $out.Add([byte]($data.Count -band 0xFF))
    $out.Add([byte](($data.Count -shr 8) -band 0xFF))
    $out.Add([byte]($chk -band 0xFF))
    $out.Add(0); $out.Add(0); $out.Add(0)
    foreach ($b in $data) {
        if ($b -eq $SLIP_END) { $out.Add($SLIP_ESC); $out.Add($SLIP_ESC_END) }
        elseif ($b -eq $SLIP_ESC) { $out.Add($SLIP_ESC); $out.Add($SLIP_ESC_ESC) }
        else { $out.Add($b) }
    }
    $out.Add($SLIP_END)
    $arr = $out.ToArray()
    # Write in small chunks: some USB-CDC drivers (usbser.sys) drop data on large writes.
    $off = 0
    while ($off -lt $arr.Count) {
        $n = [Math]::Min(512, $arr.Count - $off)
        $sp.Write($arr, $off, $n)
        $off += $n
    }
}

function Read-SlipPacket($sp, [int]$timeoutMs) {
    $deadline = [Environment]::TickCount + $timeoutMs
    $pkt = New-Object System.Collections.Generic.List[byte]
    $inPkt = $false
    $esc = $false
    while ([Environment]::TickCount -lt $deadline) {
        # drain port into the persistent script buffer
        $n = $sp.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n
            [void]$sp.Read($buf, 0, $n)
            foreach ($b in $buf) { $script:rxBuffer.Add($b) }
        }
        # parse packets out of the buffer
        while ($script:rxBuffer.Count -gt 0) {
            $b = $script:rxBuffer[0]
            $script:rxBuffer.RemoveAt(0)
            if (-not $inPkt) {
                if ($b -eq $SLIP_END) { $inPkt = $true; $pkt.Clear() }
            }
            elseif ($esc) {
                if ($b -eq $SLIP_ESC_END) { $pkt.Add($SLIP_END) } else { $pkt.Add($SLIP_ESC) }
                $esc = $false
            }
            elseif ($b -eq $SLIP_ESC) { $esc = $true }
            elseif ($b -eq $SLIP_END) { return $pkt.ToArray() }
            else { $pkt.Add($b) }
        }
        Start-Sleep -Milliseconds 2
    }
    return $null
}

function Send-Cmd($sp, [int]$cmd, [byte[]]$data, [int]$chk, [int]$timeoutMs, [string]$what) {
    Write-SlipCmd $sp $cmd $data $chk
    $p = Read-SlipPacket $sp $timeoutMs
    if ($null -eq $p -or $p.Count -lt 8) { throw "no response to $what" }
    if ($p[0] -ne 0x01) { throw "bad response dir for $what" }
    # status = last 2 bytes of the response data (esptool STATUS_BYTES_LENGTH=2)
    if ($p.Count -ge 10) {
        $s0 = $p[$p.Count - 2]
        $s1 = $p[$p.Count - 1]
        if ($s0 -ne 0) { throw "$what failed: status $s0 $s1" }
    }
    return
}

function To-U32([uint32]$v) {
    return [byte[]](
        [byte]($v -band 0xFF),
        [byte](($v -shr 8) -band 0xFF),
        [byte](($v -shr 16) -band 0xFF),
        [byte](($v -shr 24) -band 0xFF)
    )
}

function Calc-Checksum([byte[]]$data, [int]$state) {
    foreach ($b in $data) { $state = $state -bxor $b }
    return $state
}

function Sync-Chip($sp) {
    $data = [byte[]](0x07,0x07,0x12,0x20) + [byte[]](1..32 | ForEach-Object { 0x55 })
    Write-SlipCmd $sp 0x08 $data 0
    for ($i = 0; $i -lt 8; $i++) {
        $p = Read-SlipPacket $sp 2000
        if ($null -eq $p -or $p.Count -lt 8) { throw "sync failed (response $i)" }
    }
}

function Attach-Spi($sp) {
    # ESP_SPI_ATTACH (0x0D): hspi_arg=0 + is_legacy=0,0,0,0 (ROM takes 8 bytes)
    $arg = [byte[]](0,0,0,0, 0,0,0,0)
    Send-Cmd $sp 0x0D $arg 0 5000 "SPI_ATTACH" | Out-Null
}

# Windows usbser.sys workaround: dummy DTR change forces the control-line-state request
function Set-Rts($sp, [bool]$v) {
    $sp.RtsEnable = $v
    $d = $sp.DtrEnable
    $sp.DtrEnable = (-not $d)
    $sp.DtrEnable = $d
}
function Set-Dtr($sp, [bool]$v) { $sp.DtrEnable = $v }

function Reset-To-Download($sp) {
    # UsbJtagSerialReset (same sequence as esptool / esptool-js)
    Set-Rts $sp $false
    Set-Dtr $sp $false
    Start-Sleep -Milliseconds 100
    Set-Dtr $sp $true
    Set-Rts $sp $false
    Start-Sleep -Milliseconds 100
    Set-Rts $sp $true
    Set-Dtr $sp $false
    Set-Rts $sp $true
    Start-Sleep -Milliseconds 100
    Set-Rts $sp $false
    Set-Dtr $sp $false
}

# Full chip erase through the ROM FLASH_BEGIN command: it erases 'erase_size'
# bytes starting at 'offset'. num_blocks = 0 means no data blocks follow.
function Erase-Chip($sp) {
    Write-Info "erasing entire internal flash (can take ~10-40 s)..."
    $begin = New-Object System.Collections.Generic.List[byte]
    foreach ($v in @([uint32]0x400000, [uint32]0, [uint32]0x400, [uint32]0, [uint32]0)) {
        $begin.AddRange([byte[]](To-U32 $v))
    }
    Send-Cmd $sp 0x02 $begin.ToArray() 0 180000 "FLASH_BEGIN erase" | Out-Null
    Write-Ok "internal flash erased"
}

# ---------------- flash ----------------
function Flash-Image($sp, [string]$path, [uint32]$offset, [string]$label) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $len = $bytes.Length
    $blocks = [int][Math]::Ceiling($len / 0x400)
    $total = $blocks * 0x400
    $padded = New-Object byte[] $total
    [Array]::Copy($bytes, $padded, $len)
    for ($i = $len; $i -lt $total; $i++) { $padded[$i] = 0xFF }

    Write-Info ("Flashing {0} @ 0x{1:X} ({2} bytes, {3} blocks)" -f $label, $offset, $len, $blocks)

    # FLASH_BEGIN (ROM, ESP32-S3): <IIIII erase_size, blocks, 0x400, offset, begin_rom_encrypted=0
    $begin = New-Object System.Collections.Generic.List[byte]
    foreach ($v in @([uint32]$len, [uint32]$blocks, [uint32]0x400, [uint32]$offset, [uint32]0)) {
        $begin.AddRange([byte[]](To-U32 $v))
    }
    Send-Cmd $sp 0x02 $begin.ToArray() 0 15000 "FLASH_BEGIN $label" | Out-Null

    for ($seq = 0; $seq -lt $blocks; $seq++) {
        $block = New-Object byte[] 0x400
        [Array]::Copy($padded, $seq * 0x400, $block, 0, 0x400)
        $payload = New-Object System.Collections.Generic.List[byte]
        foreach ($v in @([uint32]0x400, [uint32]$seq, [uint32]0, [uint32]0)) {
            $payload.AddRange([byte[]](To-U32 $v))
        }
        $payload.AddRange([byte[]]$block)
        $chk = Calc-Checksum $block 0xEF
        # retry like esptool (WRITE_BLOCK_ATTEMPTS=3) on failure
        $written = $false
        for ($attempt = 0; $attempt -lt 3 -and -not $written; $attempt++) {
            try {
                Send-Cmd $sp 0x03 $payload.ToArray() $chk 10000 "FLASH_DATA $label/$seq" | Out-Null
                $written = $true
            } catch {
                if ($attempt -lt 2) {
                    Write-Warn ("block {0} retry {1}" -f $seq, ($attempt + 1))
                    Start-Sleep -Milliseconds 100
                } else {
                    throw
                }
            }
        }
        Start-Sleep -Milliseconds 10
        if (($seq + 1) % 32 -eq 0 -or ($seq + 1) -eq $blocks) {
            Write-Host ("    {0}%" -f [int](($seq + 1) * 100 / $blocks))
        }
    }
    Write-Ok ("{0} done" -f $label)
}

# ---------------- PROV sounds ----------------
function Read-Text-Until($sp, [string]$marker, [int]$timeoutMs) {
    $deadline = [Environment]::TickCount + $timeoutMs
    $buf = ""
    while ([Environment]::TickCount -lt $deadline) {
        try { $buf += $sp.ReadExisting() } catch { }
        if ($buf.IndexOf($marker, [StringComparison]::Ordinal) -ge 0) { return $true }
        Start-Sleep -Milliseconds 30
    }
    return $false
}

function Upload-Sounds($sp, [string]$dir) {
    if (-not (Test-Path $dir)) { Write-Warn "sound folder not found: $dir"; return }
    $files = @(Get-ChildItem -LiteralPath $dir -Filter *.wav | Sort-Object Name)
    if ($files.Count -eq 0) { Write-Warn "no WAV files in $dir"; return }
    Write-Info ("{0} sound files -> slots 1..{0}" -f $files.Count)

    $started = $false
    for ($i = 0; $i -lt 240 -and -not $started; $i++) {
        try { $sp.Write("PROV`n") } catch { }
        if (Read-Text-Until $sp "PROV-OK" 500) { $started = $true }
    }
    if (-not $started) { Write-Err "firmware did not enter provisioning"; return }

    Write-Ok "provisioning started"
    Start-Sleep -Milliseconds 300
    $sp.BaudRate = 921600
    $sp.DiscardInBuffer()

    $slot = 1
    foreach ($f in $files) {
        $len = $f.Length
        Write-Info ("slot {0}: {1} ({2} B)" -f $slot, $f.Name, $len)
        $sp.Write(("PUT {0} {1} {2}`n" -f $slot, $len, $f.Name))
        if (-not (Read-Text-Until $sp "PUT-OK" 60000)) { Write-Err "no PUT-OK slot $slot"; return }

        $fs = [System.IO.File]::OpenRead($f.FullName)
        $chunk = New-Object byte[] 4096
        $remaining = $len
        try {
            while ($remaining -gt 0) {
                $n = $fs.Read($chunk, 0, [Math]::Min(4096, [int]$remaining))
                $sp.Write($chunk, 0, $n)
                $remaining -= $n
                if ($remaining -gt 0 -and -not (Read-Text-Until $sp "CHUNK" 30000)) {
                    Write-Err "data ack timeout slot $slot"; $fs.Dispose(); return
                }
            }
        } finally { $fs.Dispose() }
        if (-not (Read-Text-Until $sp "FILE-OK" 60000)) { Write-Err "no FILE-OK slot $slot"; return }
        Write-Ok ("slot {0}: {1}" -f $slot, $f.Name)
        $slot++
    }

    try { $sp.Write("DONE`n") } catch { }
    if (Read-Text-Until $sp "DONE-OK" 15000) { Write-Ok "sounds uploaded, device restarting" }
    else { Write-Warn "DONE-OK not received" }
}

# =============================================================
# main
# =============================================================
if (-not $Port) { $Port = Find-EspPort }
if (-not $Port) { Write-Err "ESP32-S3 (VID_303A) not found. Check USB."; exit 1 }
Write-Info ("Port: {0}" -f $Port)

if (-not (Test-Path $FwDir)) { Write-Err "firmware folder not found: $FwDir"; exit 1 }
$bootloader = Join-Path $FwDir "bootloader.bin"
$partitions = Join-Path $FwDir "partitions.bin"
$ota        = Join-Path $FwDir "ota_data_initial.bin"
$firmware   = Join-Path $FwDir "firmware.bin"
foreach ($f in @($bootloader, $partitions, $ota, $firmware)) {
    if (-not (Test-Path $f)) { Write-Err "missing file: $f"; exit 1 }
}

$sp = Open-Serial $Port 115200

try {
    Write-Info "entering download mode..."
    Reset-To-Download $sp
    Start-Sleep -Milliseconds 400

    # After reset the chip re-enumerates: reopen fresh port
    $newPort = $null
    for ($i = 0; $i -lt 30 -and -not $newPort; $i++) {
        Start-Sleep -Milliseconds 300
        $newPort = Find-EspPort
    }
    if (-not $newPort) { throw "device did not reappear after reset" }
    if ($newPort -ne $Port) {
        Write-Info ("port changed {0} -> {1}" -f $Port, $newPort)
        $sp.Close()
        $sp = Open-Serial $newPort 115200
        $Port = $newPort
    }
    $script:rxBuffer.Clear()

    Write-Info "syncing..."
    $synced = $false
    for ($attempt = 0; $attempt -lt 5 -and -not $synced; $attempt++) {
        try {
            $script:rxBuffer.Clear()
            Sync-Chip $sp
            $synced = $true
        } catch {
            Write-Warn ("sync attempt {0}: {1}" -f ($attempt + 1), $_.Exception.Message)
            Start-Sleep -Milliseconds 300
        }
    }
    if (-not $synced) { throw "could not sync with chip" }
    Write-Ok "chip in download mode"

    Attach-Spi $sp
    Write-Ok "SPI flash attached"

    if (-not $NoErase) {
        # Best-effort: if the ROM rejects the erase we still proceed to flash.
        try { Erase-Chip $sp } catch { Write-Warn "chip erase skipped: $($_.Exception.Message)" }
    }

    Flash-Image $sp $bootloader 0x0       "bootloader"
    Flash-Image $sp $partitions 0x8000    "partitions"
    Flash-Image $sp $ota       0x10000    "ota_data"
    Flash-Image $sp $firmware  0x20000    "firmware"

    # FLASH_END reboot: <I 0>
    Write-Info "rebooting chip..."
    Send-Cmd $sp 0x04 ([byte[]](0,0,0,0)) 0 5000 "FLASH_END" | Out-Null
    Write-Ok "firmware flashed"

    if (-not $NoSounds) {
        Write-Info "waiting for app boot..."
        Start-Sleep -Milliseconds 2500
        # chip rebooted -> port re-enumerated, reopen fresh
        try { $sp.Close() } catch { }
        $np = $null
        for ($i = 0; $i -lt 30 -and -not $np; $i++) {
            Start-Sleep -Milliseconds 300
            $np = Find-EspPort
        }
        if (-not $np) { throw "device did not reappear after reboot" }
        $sp = Open-Serial $np 115200
        $script:rxBuffer.Clear()
        Write-Info "uploading sounds..."
        Upload-Sounds $sp $SoundDir
    }

    Write-Ok "DONE"
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}
