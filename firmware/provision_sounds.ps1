param(
    [string]$Port = "COM3",
    [string]$SoundDir = (Join-Path $PSScriptRoot "..\..\SOUND")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $SoundDir)) {
    Write-Host "[ERROR] Sound folder not found: $SoundDir" -ForegroundColor Red
    exit 1
}
$files = @(Get-ChildItem -LiteralPath $SoundDir -Filter *.wav | Sort-Object Name)
if ($files.Count -eq 0) {
    Write-Host "[ERROR] No WAV files found in $SoundDir" -ForegroundColor Red
    exit 1
}
Write-Host ("[INFO] {0} sound files -> slots 1..{0}" -f $files.Count)
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
        if ($buf.IndexOf($marker, [StringComparison]::Ordinal) -ge 0) { return $true }
        Start-Sleep -Milliseconds 30
    }
    return $false
}

# 1. Send "PROV" to start provisioning in-place. The firmware's background
#    listener accepts the command over UART0 or the native USB-Serial-JTAG
#    port and runs the erase + format + upload flow without a reboot, so the
#    host link never drops mid-protocol. Because the next step erases the
#    external NOR, the firmware replies with "PROV-CONFIRM?" and waits for an
#    explicit "PROV-CONFIRM" line; "PROV-OK" follows only after that.
Write-Host "[INFO] Sending PROV (starting provisioning)..."
$started = $false
for ($i = 0; $i -lt 240 -and -not $started; $i++) {
    try { $sp.Write("PROV`n") } catch { }
    if (ReadUntil "PROV-CONFIRM?" 1500) {
        try { $sp.Write("PROV-CONFIRM`n") } catch { }
        if (ReadUntil "PROV-OK" 2000) { $started = $true }
    }
}
if (-not $started) {
    Write-Host "[ERROR] Firmware did not enter provisioning (check power/port)." -ForegroundColor Red
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
if (ReadUntil "DONE-OK" 15000) {
    Write-Host "[OK] All sounds uploaded. Device restarting." -ForegroundColor Green
} else {
    Write-Host "[WARN] DONE-OK not received" -ForegroundColor Yellow
}
$sp.Close()
