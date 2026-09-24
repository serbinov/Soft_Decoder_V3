param(
    [string]$Port = "COM3",
    [switch]$WriteHeader,
    [string]$HeaderPath = (Join-Path $PSScriptRoot "..\components\motor\include\bemf_cal_base.h")
)

$ErrorActionPreference = "Stop"

Write-Host "[INFO] Port: $Port  baud: 115200"

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = 115200
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.ReadTimeout = 400
$sp.WriteTimeout = 5000
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
        if ($buf.IndexOf($marker, [StringComparison]::Ordinal) -ge 0) { return $buf }
        Start-Sleep -Milliseconds 30
    }
    return $buf
}

# 1. Dump the active coefficients.
try { $sp.Write("BEMF?`n") } catch {
    Write-Host "[ERROR] write failed: $_" -ForegroundColor Red
    $sp.Close(); exit 1
}
$resp = ReadUntil "BEMF-OK" 4000
$okLine = ($resp -split "`n" | Where-Object { $_ -match "^BEMF-OK " } | Select-Object -First 1)
if (-not $okLine) {
    Write-Host "[ERROR] No BEMF reply (check power/port). Raw:" -ForegroundColor Red
    Write-Host $resp
    $sp.Close(); exit 1
}
$tokens = $okLine.Trim() -split "\s+"
$count = [int]$tokens[1]
Write-Host "[OK] Coefficients ($count points):" -ForegroundColor Green
$pairs = @()
for ($i = 2; $i + 1 -lt $tokens.Count; $i += 2) {
    $speed = [int]$tokens[$i]
    $frac = [int]$tokens[$i + 1]
    $pct = [math]::Round($frac * 100.0 / 1024, 1)
    $pairs += , @($speed, $frac)
    Write-Host ("  speed {0,-4} frac {1,-5} ({2} %)" -f $speed, $frac, $pct)
}

# 2. Optionally write the firmware base header with these coefficients.
if ($WriteHeader) {
    Write-Host "[INFO] Sending BEMF-HDR ..."
    try { $sp.Write("BEMF-HDR`n") } catch {
        Write-Host "[ERROR] write failed: $_" -ForegroundColor Red
        $sp.Close(); exit 1
    }
    $buf = ReadUntil "BEMF-HDR-END" 4000
    $lines = $buf -split "`n"
    $start = -1
    $end = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $l = $lines[$i].Trim()
        if ($l -eq "#ifndef BEMF_CAL_BASE_H" -and $start -lt 0) { $start = $i }
        if ($l -eq "#endif" -or $l -eq "#endif /* BEMF_CAL_BASE_H */") { $end = $i }
    }
    if ($start -lt 0 -or $end -lt 0) {
        Write-Host "[ERROR] BEMF-HDR reply not parsed. Raw:" -ForegroundColor Red
        Write-Host $buf
        $sp.Close(); exit 1
    }
    $hdr = $lines[$start..$end] -join "`r`n" + "`r`n"
    $dir = Split-Path -Parent $HeaderPath
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Set-Content -LiteralPath $HeaderPath -Value $hdr -Encoding ascii -NoNewline
    Write-Host "[OK] Base header written: $HeaderPath" -ForegroundColor Green
    Write-Host "    Rebuild the firmware and flash it to make these coefficients the base defaults."
}

$sp.Close()
