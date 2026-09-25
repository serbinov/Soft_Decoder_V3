# Hardware-in-the-loop smoke test over the USB-Serial-JTAG (or UART0) console.
#
# Uses the firmware's built-in SELFTEST command (components/selftest), which
# runs non-destructive checks (heap, pinmap, CV store, NVS scratch, LittleFS
# scratch, ADC path, audio volume, AUX state, DCC API) and prints:
#     SELFTEST-BEGIN
#     TEST <name> PASS|FAIL|SKIP
#     SELFTEST-END <passed>/<total>
# The script also asks BEMF? as a second liveness check unless -SkipBemf.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1 -Port COM5
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1 -SkipBemf
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1 -Actuate
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1 -Actuate -MotorSpeed 30
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1 -Sweep
#
# Actuation is opt-in: -Actuate briefly turns AUX on and plays a sound,
# -Sweep presses every F0..F28 and switches every AUX channel on/off,
# -MotorSpeed >0 also spins the motor. These drive real hardware (and can move
# a loco); run them only when that is intended.
# Exit code 0 = every reported check passed (FAIL fails, SKIP is allowed),
# 1 = no response / a FAIL / a malformed report.
param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [int]$BootTimeoutMs = 20000,
    [switch]$SkipBemf,
    # Actuating commands are OFF by default: they drive lights/sound/motor.
    [switch]$Actuate,
    [int]$AuxChannel = 2,
    [int]$SoundSlot = 1,
    # >0 also runs HIL-MOTOR (moves a loco); needs rail/VM power.
    [int]$MotorSpeed = 0,
    # Press every F0..F28 and switch every AUX channel on/off.
    [switch]$Sweep,
    [int]$SweepMs = 150
)

$ErrorActionPreference = "Stop"

function Find-HilPort {
    $hw = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like 'USB\VID_303A*PID_1001*' -and $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } }
    if ($hw) { return @($hw)[0] }
    $all = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($all.Count -gt 0) { return @($all)[0] }
    return ""
}

if (-not $Port) { $Port = Find-HilPort }
if (-not $Port) {
    Write-Host "[FAIL] no serial port found. Pass -Port COMx explicitly." -ForegroundColor Red
    exit 1
}
Write-Host "[INFO] HIL port $Port @ $Baud"

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = $Baud
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.ReadTimeout = 200
$sp.WriteTimeout = 2000
$sp.Encoding = New-Object System.Text.UTF8Encoding($false)

try { $sp.Open() } catch {
    Write-Host "[FAIL] cannot open ${Port}: $_" -ForegroundColor Red
    exit 1
}
$sp.DiscardInBuffer()
$sp.DiscardOutBuffer()

function Read-For([int]$Milliseconds) {
    $deadline = [Environment]::TickCount + $Milliseconds
    $sb = New-Object System.Text.StringBuilder
    while ([Environment]::TickCount -lt $deadline) {
        try { [void]$sb.Append($sp.ReadExisting()) } catch { }
        Start-Sleep -Milliseconds 15
    }
    return $sb.ToString()
}

$failed = $false
try {
    # --- SELFTEST (retry until the console listener is up) ---
    $resp = ""
    $deadline = [Environment]::TickCount + $BootTimeoutMs
    while ([Environment]::TickCount -lt $deadline -and $resp -notmatch "SELFTEST-BEGIN") {
        try { $sp.Write("SELFTEST`n") } catch { }
        $resp += Read-For 700
    }
    if ($resp -notmatch "SELFTEST-BEGIN") {
        Write-Host "[FAIL] no SELFTEST response (device off / wrong port / not booted)." -ForegroundColor Red
        Write-Host $resp
        exit 1
    }

    $endDeadline = [Environment]::TickCount + 5000
    while ($resp -notmatch "SELFTEST-END" -and [Environment]::TickCount -lt $endDeadline) {
        $resp += Read-For 300
    }

    # Keep only the last report block (the boot-wait may have triggered several).
    $lastBegin = $resp.LastIndexOf("SELFTEST-BEGIN")
    if ($lastBegin -ge 0) { $resp = $resp.Substring($lastBegin) }

    Write-Host ""
    Write-Host "SELFTEST report:" -ForegroundColor Cyan
    $results = @()
    foreach ($line in ($resp -split "`r?`n")) {
        if ($line -match '^\s*TEST\s+(\S+)\s+(PASS|FAIL|SKIP)\s*$') {
            $results += [pscustomobject]@{ Name = $Matches[1]; State = $Matches[2] }
        }
    }
    foreach ($r in $results) {
        $color = if ($r.State -eq "PASS") { "Green" } elseif ($r.State -eq "FAIL") { "Red" } else { "Yellow" }
        Write-Host ("  {0,-12} {1}" -f $r.Name, $r.State) -ForegroundColor $color
    }
    $endLine = ($resp -split "`r?`n" | Where-Object { $_ -match 'SELFTEST-END' } | Select-Object -First 1)
    if ($endLine) { Write-Host "  $($endLine.Trim())" -ForegroundColor Cyan }

    if ($results.Count -eq 0) {
        Write-Host "[FAIL] SELFTEST produced no TEST lines." -ForegroundColor Red
        $failed = $true
    }
    if (-not $endLine) {
        Write-Host "[FAIL] SELFTEST-END not received." -ForegroundColor Red
        $failed = $true
    }
    $failCount = @($results | Where-Object { $_.State -eq "FAIL" }).Count
    if ($failCount -gt 0) {
        Write-Host "[FAIL] $failCount self-test check(s) failed." -ForegroundColor Red
        $failed = $true
    }

    # --- BEMF? liveness (motor/ADC/calibration store) ---
    if (-not $SkipBemf) {
        $bemf = ""
        try { $sp.Write("BEMF?`n") } catch { }
        $d = [Environment]::TickCount + 4000
        while ([Environment]::TickCount -lt $d -and $bemf -notmatch "BEMF-OK") {
            $bemf += Read-For 250
        }
        $okLine = ($bemf -split "`r?`n" | Where-Object { $_ -match '^BEMF-OK ' } | Select-Object -First 1)
        if ($okLine) {
            Write-Host "BEMF: $($okLine.Trim())" -ForegroundColor Green
        } else {
            Write-Host "[FAIL] BEMF? did not answer BEMF-OK." -ForegroundColor Red
            $failed = $true
        }
    }

    # --- Actuating commands (opt-in: -Actuate / -MotorSpeed) ---
    if ($Actuate) {
        Write-Host "Actuating AUX channel $AuxChannel and sound slot $SoundSlot..." -ForegroundColor Yellow
        $acts = @(
            @{ Text = "HIL-AUX $AuxChannel 400"; Ok = "HIL-AUX-OK"; Desc = "AUX $AuxChannel" },
            @{ Text = "HIL-SOUND $SoundSlot 1500"; Ok = "HIL-SOUND-OK"; Desc = "sound slot $SoundSlot" }
        )
        foreach ($a in $acts) {
            try { $sp.Write($a.Text + "`n") } catch { }
            $r = ""
            $d = [Environment]::TickCount + 6000
            while ([Environment]::TickCount -lt $d -and $r -notmatch [regex]::Escape($a.Ok) -and $r -notmatch "-ERR") {
                $r += Read-For 200
            }
            if ($r -match [regex]::Escape($a.Ok)) {
                Write-Host "  [ OK ] $($a.Desc)" -ForegroundColor Green
            } else {
                Write-Host "  [FAIL] $($a.Desc): $($r.Trim())" -ForegroundColor Red
                $failed = $true
            }
        }
    }
    if ($MotorSpeed -gt 0) {
        Write-Host "Actuating motor at $MotorSpeed for 800 ms..." -ForegroundColor Yellow
        try { $sp.Write("HIL-MOTOR $MotorSpeed 800`n") } catch { }
        $r = ""
        $d = [Environment]::TickCount + 6000
        while ([Environment]::TickCount -lt $d -and $r -notmatch "HIL-MOTOR-OK" -and $r -notmatch "HIL-MOTOR-ERR") {
            $r += Read-For 200
        }
        if ($r -match "HIL-MOTOR-OK") {
            Write-Host "  [ OK ] motor" -ForegroundColor Green
        } else {
            Write-Host "  [FAIL] motor: $($r.Trim())" -ForegroundColor Red
            $failed = $true
        }
    }
    if ($Sweep) {
        Write-Host "Sweeping all F0..F28 and all AUX channels ($SweepMs ms each)..." -ForegroundColor Yellow
        $sweeps = @(
            @{ Text = "HIL-FN-SWEEP $SweepMs"; Ok = "HIL-FN-SWEEP-OK"; Desc = "all F0..F28" },
            @{ Text = "HIL-AUX-SWEEP $SweepMs"; Ok = "HIL-AUX-SWEEP-OK"; Desc = "all AUX channels" }
        )
        foreach ($sw in $sweeps) {
            try { $sp.Write($sw.Text + "`n") } catch { }
            $r = ""
            $d = [Environment]::TickCount + 60000
            while ([Environment]::TickCount -lt $d -and $r -notmatch [regex]::Escape($sw.Ok) -and $r -notmatch "-ERR") {
                $r += Read-For 300
            }
            $hit = ($r -split "`r?`n" | Where-Object { $_ -match [regex]::Escape($sw.Ok) } | Select-Object -First 1)
            if ($hit) {
                Write-Host "  [ OK ] $($sw.Desc): $($hit.Trim())" -ForegroundColor Green
            } else {
                Write-Host "  [FAIL] $($sw.Desc): $($r.Trim())" -ForegroundColor Red
                $failed = $true
            }
        }
    }
} finally {
    $sp.Close()
}

Write-Host ""
if ($failed) {
    Write-Host "HIL: FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "HIL: PASSED" -ForegroundColor Green
exit 0
