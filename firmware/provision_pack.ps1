# Upload one complete AURA Sound Pack (.asp) over the serial provisioning link.
# With -Wipe, skip the pack and send WIPE instead: the firmware formats the
# external NOR (sounds, graph projects, sidecars) for a factory reset.
# With -EraseChip, send WIPECHIP: every external NOR block is erased and the
# chip stays blank (used by web_flasher\wipe_chips.ps1 before the host erases
# the internal flash).
#
# Protocol (no Wi-Fi, framed pack transfer):
#   PROV / PROV-CONFIRM / PROV-OK
#   PACK <bytes> -> PACK-READY
#   PACK-DATA <n> + n bytes -> PACK-DATA-OK <total>   (32 KB frames)
#   last frame -> PACK-OK <wav_count>
#   DONE -> DONE-OK (mandatory acknowledgement; missing ack is an error)
#   WIPECHIP -> ERASE-PROGRESS ... / WIPECHIP-OK | WIPECHIP-ERR storage
#
# The firmware parses the pack index and writes every payload as an individual
# WAV under /userdata/audio. The pack is built from SOUND/ by
# web_flasher\make_sound_pack.ps1 (or web_flasher\flash_with_sound_pack.ps1).

param(
    [string]$Port = "",
    [string]$Pack = "",
    [int]$Baud = 921600,
    [switch]$Wipe,
    [switch]$EraseChip
)

$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
if (-not $Wipe -and -not $EraseChip) {
    if (-not $Pack) { $Pack = Join-Path $repo "release\ADDITIPUS_sounds.asp" }
    if (-not (Test-Path -LiteralPath $Pack -PathType Leaf)) {
        Write-Host "[ERROR] Sound pack not found: $Pack" -ForegroundColor Red
        Write-Host "[HINT]  Build it first: web_flasher\make_sound_pack.ps1" -ForegroundColor Yellow
        exit 1
    }
    $len = (Get-Item -LiteralPath $Pack).Length
    if ($len -lt 16 -or $len -gt (15 * 1024 * 1024)) {
        Write-Host "[ERROR] Invalid pack size: $len bytes" -ForegroundColor Red
        exit 1
    }
}

function Find-EspPort {
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like "USB\VID_303A*PID_1001*" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } } |
        Select-Object -First 1
}
if (-not $Port) { $Port = Find-EspPort }
if (-not $Port) {
    Write-Host "[ERROR] ESP32-S3 USB-Serial/JTAG port not found. Specify -Port COMx." -ForegroundColor Red
    exit 1
}

if ($Wipe) {
    Write-Host "[INFO] Mode : factory wipe (external storage)"
} elseif ($EraseChip) {
    Write-Host "[INFO] Mode : full external NOR chip erase"
} else {
    Write-Host ("[INFO] Pack : {0} ({1:N0} bytes)" -f $Pack, $len)
}
Write-Host "[INFO] Port : $Port"

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = 115200
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.ReadTimeout = 400
$sp.WriteTimeout = 120000
$sp.WriteBufferSize = 65536
$sp.Encoding = New-Object System.Text.UTF8Encoding($false)

try { $sp.Open() } catch {
    Write-Host "[ERROR] Cannot open $Port : $_" -ForegroundColor Red
    exit 1
}
$sp.DiscardInBuffer()
$sp.DiscardOutBuffer()

# After a full internal erase (or a tool that left the ROM download mode
# strapped), ESP32-S3 may stay in download mode: it accepts flashing but never
# runs the application, so PROV would be sent into the void. Pulse the normal
# boot reset: DTR low keeps GPIO0 high, RTS low->high->low pulses EN.
function Reset-NormalBoot {
    try {
        $sp.DtrEnable = $false
        $sp.RtsEnable = $true
        Start-Sleep -Milliseconds 150
        $sp.RtsEnable = $false
    } catch { }
}
Reset-NormalBoot
Start-Sleep -Milliseconds 150
$sp.DiscardInBuffer()

function ReadUntil([string[]]$markers, [int]$timeoutMs) {
    $deadline = [Environment]::TickCount + $timeoutMs
    $buf = ""
    while ([Environment]::TickCount -lt $deadline) {
        try { $buf += $sp.ReadExisting() } catch { }
        foreach ($m in $markers) {
            if ($buf.IndexOf($m, [StringComparison]::Ordinal) -ge 0) {
                $script:lastText = $buf
                return $m
            }
        }
        Start-Sleep -Milliseconds 2
    }
    $script:lastText = $buf
    return $null
}
function Format-Rx {
    $t = ($script:lastText -replace "[\r\n]+", " ").Trim()
    if ($t.Length -gt 200) { $t = $t.Substring($t.Length - 200) }
    if (-not $t) { return "<empty>" }
    return $t
}

# Wait for an acknowledgement while echoing the device's own progress.
# Validation, publication and metadata on a slow/unknown external NOR may take
# well over the old 8 s window; "DONE-PROGRESS"/"ERASE-PROGRESS" lines prove the
# device is alive. A ROM boot banner before the ack means the app restarted
# (panic or watchdog) and the operation was not confirmed.
function Wait-Progress([string[]]$markers, [string]$progressPrefix, [int]$timeoutMs) {
    $deadline = [Environment]::TickCount + $timeoutMs
    $buf = ""
    $pending = ""
    while ([Environment]::TickCount -lt $deadline) {
        try { $text = $sp.ReadExisting() } catch { $text = "" }
        if ($text) {
            $buf += $text
            $pending += $text
            while (($idx = $pending.IndexOf("`n")) -ge 0) {
                $line = $pending.Substring(0, $idx).TrimEnd("`r")
                $pending = $pending.Substring($idx + 1)
                if ($line.StartsWith($progressPrefix)) {
                    Write-Host ("[INFO] " + $line) -ForegroundColor Cyan
                } elseif ($line.Length -gt 0 -and $line -notmatch "^(PROV|FW|PACK|WIPECHIP)-") {
                    Write-Host ("[DEV]  " + $line) -ForegroundColor DarkGray
                }
            }
        }
        foreach ($m in $markers) {
            if ($buf.IndexOf($m, [StringComparison]::Ordinal) -ge 0) {
                $script:lastText = $buf
                return $m
            }
        }
        if ($buf -match "ESP-ROM:esp32s3|Soft Decoder V3 boot|rst:0x") {
            $script:lastText = $buf
            return "REBOOT"
        }
        Start-Sleep -Milliseconds 2
    }
    $script:lastText = $buf
    return $null
}
function Wait-Done([int]$timeoutMs = 180000) {
    return (Wait-Progress @("DONE-OK", "PROV-ERR", "FW-ERR") "DONE-PROGRESS" $timeoutMs)
}
function Wait-ChipErase([int]$timeoutMs = 600000) {
    return (Wait-Progress @("WIPECHIP-OK", "WIPECHIP-ERR", "PROV-ERR", "FW-ERR") "ERASE-PROGRESS" $timeoutMs)
}

# 1. Enter provisioning. The firmware erases/formats the external NOR after the
#    explicit confirmation, so the host must answer PROV-CONFIRM.
Write-Host "[INFO] Entering provisioning (PROV)..."
$started = $false
for ($i = 0; $i -lt 4 -and -not $started; $i++) {
    if ($i -gt 0) {
        Write-Host ("[INFO] attempt {0}: resetting the board and retrying..." -f ($i + 1))
        Reset-NormalBoot
    }
    # Wait for the app's background listener before sending PROV. Repeated
    # PROV lines while the chip is still booting stay in its USB FIFO; the
    # confirmation reader would consume a stale PROV and abort the session.
    Start-Sleep -Milliseconds 3500
    try { $sp.DiscardInBuffer(); $sp.Write("PROV`n") } catch { }
    $r = ReadUntil @("PROV-CONFIRM?", "PROV-ERR", "FW-ERR", "PROV-ABORT") 8000
    if ($r -eq "PROV-CONFIRM?") {
        try { $sp.Write("PROV-CONFIRM`n") } catch { }
        $r = ReadUntil @("PROV-OK", "PROV-ERR", "FW-ERR", "PROV-ABORT") 8000
        if ($r -eq "PROV-OK") { $started = $true }
        elseif ($r -eq "PROV-ABORT") {
            Write-Host ("[WARN] attempt {0}: firmware aborted before confirmation; retrying." -f ($i + 1)) -ForegroundColor Yellow
        } elseif ($r) {
            Write-Host ("[ERROR] Firmware rejected provisioning ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red
            $sp.Close(); exit 1
        } else {
            Write-Host ("[WARN] attempt {0}: no PROV-OK (got: {1})" -f ($i + 1), (Format-Rx)) -ForegroundColor Yellow
        }
    } elseif ($r -eq "PROV-ABORT") {
        Write-Host ("[WARN] attempt {0}: unexpected PROV-ABORT; retrying." -f ($i + 1)) -ForegroundColor Yellow
    } elseif ($r) {
        Write-Host ("[ERROR] Firmware rejected provisioning ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red
        $sp.Close(); exit 1
    } else {
        Write-Host ("[INFO] attempt {0}: no answer yet (got: {1})" -f ($i + 1), (Format-Rx))
    }
}
if (-not $started) {
    Write-Host "[ERROR] Firmware did not enter provisioning (is the app running? is the port free?)." -ForegroundColor Red
    $sp.Close(); exit 1
}
if ($EraseChip) { Write-Host "[OK] Provisioning started (external NOR will be fully erased)." -ForegroundColor Green }
elseif ($Wipe) { Write-Host "[OK] Provisioning started (external NOR will be formatted)." -ForegroundColor Green }
else { Write-Host "[OK] Provisioning started (external NOR will be formatted)." -ForegroundColor Green }

if ($EraseChip) {
    Start-Sleep -Milliseconds 300
    Write-Host "[INFO] Erasing every block of the external NOR chip..." -ForegroundColor Cyan
    try { $sp.Write("WIPECHIP`n") } catch {
        Write-Host "[ERROR] WIPECHIP command failed: $_" -ForegroundColor Red
        $sp.Close(); exit 1
    }
    $r = Wait-ChipErase 600000
    if ($r -ne "WIPECHIP-OK") {
        if ($r -eq "REBOOT") {
            Write-Host "[ERROR] device rebooted during the external chip erase; chip is not confirmed blank." -ForegroundColor Red
        } elseif ($r) {
            Write-Host ("[ERROR] Firmware rejected WIPECHIP ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red
        } else {
            Write-Host "[ERROR] WIPECHIP acknowledgement timed out; chip is not confirmed blank." -ForegroundColor Red
        }
        $sp.Close(); exit 1
    }
    Write-Host "[OK] External NOR chip erased (blank, unmounted)." -ForegroundColor Green
    # Do not send DONE: the host erases the internal flash next, so there is
    # nothing to persist and the device may restart at any moment.
    $sp.Close(); exit 0
}

if ($Wipe) {
    Start-Sleep -Milliseconds 300
    Write-Host "[INFO] Formatting external storage (WIPE)..." -ForegroundColor Cyan
    try { $sp.Write("WIPE`n") } catch {
        Write-Host "[ERROR] WIPE command failed: $_" -ForegroundColor Red
        $sp.Close(); exit 1
    }
    $r = ReadUntil @("WIPE-OK", "WIPE-ERR", "PROV-ERR") 300000
    if ($r -ne "WIPE-OK") {
        if ($r) { Write-Host ("[ERROR] Firmware rejected WIPE ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red }
        else { Write-Host "[ERROR] WIPE acknowledgement timed out." -ForegroundColor Red }
        $sp.Close(); exit 1
    }
    Write-Host "[OK] External storage formatted." -ForegroundColor Green
    try { $sp.Write("DONE`n") } catch { }
    $r = Wait-Done 120000
    if ($r -ne "DONE-OK") {
        if ($r -eq "REBOOT") { Write-Host "[ERROR] device rebooted before confirming DONE; factory wipe is not confirmed." -ForegroundColor Red }
        elseif ($r) { Write-Host ("[ERROR] Firmware rejected DONE ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red }
        else { Write-Host "[ERROR] no DONE-OK acknowledgement; factory wipe is not confirmed." -ForegroundColor Red }
        $sp.Close(); exit 1
    }
    Write-Host "[OK] Factory wipe complete, device restarting." -ForegroundColor Green
    $sp.Close()
    exit 0
}

# 2. Switch both sides to the fast baud. USB-Serial/JTAG ignores the baud rate;
#    UART0 uses it for the bulk transfer.
Start-Sleep -Milliseconds 300
$sp.BaudRate = $Baud
$sp.DiscardInBuffer()

# 3. Ask for the pack and stream the whole file without per-chunk acks.
$cmd = "PACK $len"
try { $sp.Write($cmd + "`n") } catch {
    Write-Host "[ERROR] PACK command failed: $_" -ForegroundColor Red
    $sp.Close(); exit 1
}
$ready = ReadUntil @("PACK-READY", "PACK-ERR", "FW-ERR") 120000
if ($ready -ne "PACK-READY") {
    if ($ready) {
        Write-Host ("[ERROR] Device rejected PACK ({0}): {1}" -f $ready, (Format-Rx)) -ForegroundColor Red
    } else {
        Write-Host "[ERROR] No PACK-READY (firmware without PACK support?)." -ForegroundColor Red
    }
    $sp.Close(); exit 1
}

Write-Host ("[INFO] Sending {0:N0} bytes in 32 KB frames..." -f $len)
$sw = [Diagnostics.Stopwatch]::StartNew()
$fs = [System.IO.File]::OpenRead($Pack)
$chunk = New-Object byte[] 32768
$frames = 0
$sent = 0
try {
    while (($n = $fs.Read($chunk, 0, $chunk.Length)) -gt 0) {
        $sp.Write("PACK-DATA $n`n")
        $sp.Write($chunk, 0, $n)
        $sent += $n
        $frames++
        if ($sent -lt $len) {
            # Intermediate frames are acked after the parser consumes them.
            $ack = ReadUntil @("PACK-DATA-OK", "PACK-ERR", "FW-ERR") 30000
            if ($ack -ne "PACK-DATA-OK") {
                throw ("frame {0} rejected ({1}): {2}" -f $frames, $ack, (Format-Rx))
            }
        }
        if (($frames % 16) -eq 0) {
            Write-Host ("[INFO] sent {0:N0}/{1:N0} bytes" -f $sent, $len)
        }
    }
} catch {
    Write-Host "[ERROR] Serial pack transfer failed: $_" -ForegroundColor Red
    $fs.Dispose(); $sp.Close(); exit 1
} finally {
    $fs.Dispose()
}

$result = ReadUntil @("PACK-OK", "PACK-ERR", "FW-ERR") 300000
$sw.Stop()
if ($result -ne "PACK-OK") {
    if ($result) { Write-Host ("[ERROR] Pack unpack failed ({0}): {1}" -f $result, (Format-Rx)) -ForegroundColor Red }
    else { Write-Host "[ERROR] Pack unpack acknowledgement timed out." -ForegroundColor Red }
    $sp.Close(); exit 1
}
Write-Host ("[OK] Pack received in {0:N1} s: {1}" -f $sw.Elapsed.TotalSeconds, (Format-Rx)) -ForegroundColor Green

# 4. Finish; the app saves metadata and restarts. DONE-OK is mandatory: without
#    it the host cannot claim that the installed pack was committed.
try { $sp.Write("DONE`n") } catch { }
$r = Wait-Done 180000
if ($r -eq "DONE-OK") {
    Write-Host "[OK] Sound pack installed, device restarting." -ForegroundColor Green
    $sp.Close()
    exit 0
}
if ($r -eq "REBOOT") {
    Write-Host "[ERROR] device rebooted before confirming DONE; the pack was not committed (see [DEV] log above)." -ForegroundColor Red
} elseif ($r) {
    Write-Host ("[ERROR] Firmware rejected DONE ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red
} else {
    Write-Host "[ERROR] no DONE-OK acknowledgement; sound pack installation is not confirmed." -ForegroundColor Red
}
$sp.Close()
exit 1
