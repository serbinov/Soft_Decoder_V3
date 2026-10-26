# Upload one complete AURA Sound Pack (.asp) over the serial provisioning link.
# With -Wipe, skip the pack and send WIPE instead: the firmware formats the
# external NOR (sounds, graph projects, sidecars) for a factory reset.
#
# Protocol (no Wi-Fi, framed pack transfer):
#   PROV / PROV-CONFIRM / PROV-OK
#   PACK <bytes> -> PACK-READY
#   PACK-DATA <n> + n bytes -> PACK-DATA-OK <total>   (32 KB frames)
#   last frame -> PACK-OK <wav_count>
#   DONE -> DONE-OK (mandatory acknowledgement; missing ack is an error)
#
# The firmware parses the pack index and writes every payload as an individual
# WAV under /userdata/audio. The pack is built from SOUND/ by
# web_flasher\make_sound_pack.ps1 (or web_flasher\flash_with_sound_pack.ps1).

param(
    [string]$Port = "",
    [string]$Pack = "",
    [int]$Baud = 921600,
    [switch]$Wipe
)

$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
if (-not $Wipe) {
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

# 1. Enter provisioning. The firmware erases/formats the external NOR after the
#    explicit confirmation, so the host must answer PROV-CONFIRM.
Write-Host "[INFO] Entering provisioning (PROV)..."
$started = $false
for ($i = 0; $i -lt 30 -and -not $started; $i++) {
    try { $sp.DiscardInBuffer(); $sp.Write("PROV`n") } catch { }
    $r = ReadUntil @("PROV-CONFIRM?", "PROV-ERR", "FW-ERR", "PROV-ABORT") 1200
    if ($r -eq "PROV-CONFIRM?") {
        try { $sp.Write("PROV-CONFIRM`n") } catch { }
        $r = ReadUntil @("PROV-OK", "PROV-ERR", "FW-ERR", "PROV-ABORT") 6000
        if ($r -eq "PROV-OK") { $started = $true }
        elseif ($r) {
            Write-Host ("[ERROR] Firmware rejected provisioning ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red
            $sp.Close(); exit 1
        } else {
            Write-Host ("[WARN] attempt {0}: no PROV-OK (got: {1})" -f ($i + 1), (Format-Rx)) -ForegroundColor Yellow
        }
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
Write-Host "[OK] Provisioning started (external NOR will be formatted)." -ForegroundColor Green

if ($Wipe) {
    Start-Sleep -Milliseconds 300
    Write-Host "[INFO] Formatting external storage (WIPE)..." -ForegroundColor Cyan
    try { $sp.Write("WIPE`n") } catch {
        Write-Host "[ERROR] WIPE command failed: $_" -ForegroundColor Red
        $sp.Close(); exit 1
    }
    $r = ReadUntil @("WIPE-OK", "WIPE-ERR", "PROV-ERR") 120000
    if ($r -ne "WIPE-OK") {
        if ($r) { Write-Host ("[ERROR] Firmware rejected WIPE ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red }
        else { Write-Host "[ERROR] WIPE acknowledgement timed out." -ForegroundColor Red }
        $sp.Close(); exit 1
    }
    Write-Host "[OK] External storage formatted." -ForegroundColor Green
    try { $sp.Write("DONE`n") } catch { }
    $r = ReadUntil @("DONE-OK", "PROV-ERR") 10000
    if ($r -ne "DONE-OK") {
        if ($r) { Write-Host ("[ERROR] Firmware rejected DONE ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red }
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
$ready = ReadUntil @("PACK-READY", "PACK-ERR", "FW-ERR") 10000
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
$r = ReadUntil @("DONE-OK", "PROV-ERR", "FW-ERR") 8000
if ($r -eq "DONE-OK") {
    Write-Host "[OK] Sound pack installed, device restarting." -ForegroundColor Green
    $sp.Close()
    exit 0
}
if ($r) { Write-Host ("[ERROR] Firmware rejected DONE ({0}): {1}" -f $r, (Format-Rx)) -ForegroundColor Red }
else { Write-Host "[ERROR] no DONE-OK acknowledgement; sound pack installation is not confirmed." -ForegroundColor Red }
$sp.Close()
exit 1
