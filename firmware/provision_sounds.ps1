param(
    [string]$Port = "COM3",
    [string]$SoundDir = ""
)

$ErrorActionPreference = "Stop"

# The sounds normally live in <repo>\SOUND (Soft_Decoder_V3\SOUND); keep the
# older sibling-of-repo location as a fallback.
if (-not $SoundDir) {
    foreach ($c in @((Join-Path $PSScriptRoot "..\SOUND"), (Join-Path $PSScriptRoot "..\..\SOUND"))) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Container)) {
            $SoundDir = (Resolve-Path -LiteralPath $c).Path
            break
        }
    }
}
if (-not $SoundDir -or -not (Test-Path -LiteralPath $SoundDir)) {
    Write-Host "[ERROR] Sound folder not found: $SoundDir" -ForegroundColor Red
    exit 1
}
$files = @(Get-ChildItem -LiteralPath $SoundDir -Filter *.wav | Sort-Object Name)
if ($files.Count -eq 0) {
    Write-Host "[ERROR] No WAV files found in $SoundDir" -ForegroundColor Red
    exit 1
}
if ($files.Count -gt 20) {
    Write-Host ("[ERROR] {0} WAV files: the UART provisioner supports at most 20 slots." -f $files.Count) -ForegroundColor Red
    Write-Host "[HINT]  Build a sound container (.asp) in the panel and upload it on the device web page." -ForegroundColor Yellow
    exit 1
}
Write-Host ("[INFO] {0} sound files from {1} -> slots 1..{0}" -f $files.Count, $SoundDir)
Write-Host "[INFO] Port: $Port  (erase external flash + upload sounds)"

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = 115200
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.ReadTimeout = 400
$sp.WriteTimeout = 15000
$sp.Encoding = New-Object System.Text.UTF8Encoding($false)

try { $sp.Open() } catch {
    Write-Host "[ERROR] Cannot open $Port : $_" -ForegroundColor Red
    exit 1
}
$sp.DiscardInBuffer()
$sp.DiscardOutBuffer()

function ReadUntil([string]$marker, [int]$timeoutMs) {
    $deadline = [Environment]::TickCount + $timeoutMs
    $buf = ""
    while ([Environment]::TickCount -lt $deadline) {
        try { $buf += $sp.ReadExisting() } catch { }
        if ($buf.IndexOf($marker, [StringComparison]::Ordinal) -ge 0) { $script:lastText = $buf; return $true }
        Start-Sleep -Milliseconds 30
    }
    $script:lastText = $buf
    return $false
}
function Format-Rx {
    $t = ($script:lastText -replace "[\r\n]+", " ").Trim()
    if ($t.Length -gt 160) { $t = $t.Substring($t.Length - 160) }
    if (-not $t) { return "<empty>" }
    return $t
}

# 1. Send "PROV" to start provisioning in-place. The firmware's background
#    listener accepts the command over UART0 or the native USB-Serial-JTAG
#    port and runs the erase + format + upload flow without a reboot, so the
#    host link never drops mid-protocol. Because the next step erases the
#    external NOR, the firmware replies with "PROV-CONFIRM?" and waits for an
#    explicit "PROV-CONFIRM" line; "PROV-OK" follows only after that.
Write-Host "[INFO] Asking the app to enter provisioning (PROV)..."
$started = $false
for ($i = 0; $i -lt 30 -and -not $started; $i++) {
    try { $sp.DiscardInBuffer(); $sp.Write("PROV`n") } catch { }
    if (ReadUntil "PROV-CONFIRM?" 1200) {
        Write-Host "[INFO]   app requires confirmation -> sending PROV-CONFIRM"
        try { $sp.Write("PROV-CONFIRM`n") } catch { }
        if (ReadUntil "PROV-OK" 4000) { $started = $true }
        else { Write-Host ("[WARN]   attempt {0}: no PROV-OK (got: {1})" -f ($i + 1), (Format-Rx)) -ForegroundColor Yellow }
    } else {
        Write-Host ("[INFO]   attempt {0}: no answer yet (got: {1})" -f ($i + 1), (Format-Rx))
    }
}
if (-not $started) {
    Write-Host "[ERROR] Firmware did not enter provisioning (is the app running? is the port free?)." -ForegroundColor Red
    $sp.Close(); exit 1
}
Write-Host "[OK] Provisioning started (erase + format)" -ForegroundColor Green

# 2. Both sides switch to the fast baud for the bulk transfer.
Start-Sleep -Milliseconds 300
$sp.BaudRate = 921600
$sp.DiscardInBuffer()

# 3. Upload each file to its slot.
$slot = 1
foreach ($file in $files) {
    $len = $file.Length
    $label = $file.Name
    $cmd = "PUT $slot $len $label"
    try { $sp.Write($cmd + "`n") } catch {
        Write-Host "[ERROR] write PUT failed: $_" -ForegroundColor Red; $sp.Close(); exit 1
    }
    if (-not (ReadUntil "PUT-OK" 60000)) {
        Write-Host "[ERROR] no PUT-OK for slot $slot (firmware busy?)" -ForegroundColor Red
        $sp.Close(); exit 1
    }

    $fs = [System.IO.File]::OpenRead($file.FullName)
    $chunk = New-Object byte[] 4096
    $remaining = $len
    $ok = $true
    try {
        while ($remaining -gt 0) {
            $n = $fs.Read($chunk, 0, [Math]::Min(4096, [int]$remaining))
            $sp.Write($chunk, 0, $n)
            $remaining -= $n
            if ($remaining -gt 0) {
                if (-not (ReadUntil "CHUNK" 30000)) {
                    Write-Host "[ERROR] data ack timeout slot $slot" -ForegroundColor Red
                    $ok = $false; break
                }
            }
        }
    } finally {
        $fs.Dispose()
    }
    if (-not $ok) { $sp.Close(); exit 1 }

    if (-not (ReadUntil "FILE-OK" 60000)) {
        Write-Host "[ERROR] file ack timeout slot $slot" -ForegroundColor Red
        $sp.Close(); exit 1
    }
    Write-Host ("[OK] slot {0}: {1} ({2} B)" -f $slot, $file.Name, $len) -ForegroundColor Green
    $slot++
}

# 4. Done.
try { $sp.Write("DONE`n") } catch { }
# On DONE the firmware acks and immediately restarts; the ack can be lost to the
# USB re-enumeration. That is not an error - every slot was already FILE-OK'd.
if (ReadUntil "DONE-OK" 8000) {
    Write-Host "[OK] Sounds uploaded, device restarting." -ForegroundColor Green
} else {
    Write-Host "[INFO] Sounds uploaded; device restarts (DONE-OK not seen - ack may be lost on reboot)." -ForegroundColor Cyan
}
$sp.Close()
