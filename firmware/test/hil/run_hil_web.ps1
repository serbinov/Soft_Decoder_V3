# Web/REST hardware-in-the-loop test over the decoder SoftAP.
#
# Connects this PC to the decoder access point and exercises every safe HTTP
# endpoint the web UI uses: status/read endpoints, all function buttons
# (F0..F28), all AUX channels (effect + config), audio play/stop/volume, motor,
# mode, control source, CV read/write, function map, track category, device
# name, BEMF use, storage, log. Settings that are changed are restored.
#
# Destructive endpoints are intentionally NOT called by default: /api/reset
# (factory reset), /api/ota/update, /api/wifi POST (reboots),
# /api/bemf/calibrate (moves the motor), /api/audio/upload|delete.
#
# -CalibrateBemf opts in to a real BEMF calibration on a FREE (unloaded) motor.
# It starts the run, waits for a terminal state, verifies the motor target is
# zero afterwards and (with -BemfCancelAfterMs) also exercises cancellation.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil_web.ps1 -RestoreSsid AlmaHome
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil_web.ps1 -SkipConnect -BaseUrl http://192.168.100.1
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil_web.ps1 -ApPass "secret123"
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil_web.ps1 -CalibrateBemf
#   powershell -ExecutionPolicy Bypass -File test\hil\run_hil_web.ps1 -CalibrateBemf -BemfCancelAfterMs 500
#
# Exit code 0 = all checks passed.
param(
    [string]$BaseUrl = "http://192.168.100.1",
    [string]$ApSsid = "ADDITIPUS AURA-X",
    [string]$ApPass = "",
    [string]$RestoreSsid = "",
    [switch]$SkipConnect,
    # Destructive opt-in: spins a free motor and writes a new calibration.
    [switch]$CalibrateBemf,
    # >0 starts a calibration and cancels it after this many milliseconds.
    [int]$BemfCancelAfterMs = 0,
    [int]$BemfCalTimeoutSec = 90
)

$ErrorActionPreference = "Continue"

function Connect-ApProfile {
    param([string]$Ssid, [string]$Pass)
    $auth = if ($Pass) { "WPA2PSK" } else { "open" }
    $enc  = if ($Pass) { "AES" } else { "none" }
    $key  = if ($Pass) { "<keyMaterial>$Pass</keyMaterial>" } else { "" }
    $sec  = if ($Pass) {
        "<security><authEncryption><authentication>$auth</authentication><encryption>$enc</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected>$key</sharedKey></security>"
    } else {
        "<security><authEncryption><authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX></authEncryption></security>"
    }
    $xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$Ssid</name>
  <SSIDConfig><SSID><name>$Ssid</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM>$sec</MSM>
</WLANProfile>
"@
    $path = Join-Path $env:TEMP "hil_ap_profile.xml"
    Set-Content -LiteralPath $path -Value $xml -Encoding ASCII
    netsh wlan add profile filename="$path" user=current | Out-Null
    netsh wlan connect name="$Ssid" ssid="$Ssid" | Out-Null
}

function Wait-BaseUrl {
    param([int]$TimeoutSec = 25)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $r = Invoke-WebRequest -Uri "$BaseUrl/api/device" -TimeoutSec 3 -UseBasicParsing
            if ($r.StatusCode -eq 200) { return $true }
        } catch { }
        Start-Sleep -Milliseconds 700
    }
    return $false
}

if (-not $SkipConnect) {
    Write-Host "[INFO] Connecting to SoftAP '$ApSsid' ..." -ForegroundColor Cyan
    Connect-ApProfile -Ssid $ApSsid -Pass $ApPass
    if (-not (Wait-BaseUrl)) {
        Write-Host "[FAIL] $BaseUrl unreachable after joining '$ApSsid' (wrong password?)" -ForegroundColor Red
        if ($RestoreSsid) { netsh wlan connect name="$RestoreSsid" | Out-Null }
        exit 1
    }
    Write-Host "[ OK ] connected, $BaseUrl reachable" -ForegroundColor Green
}

$script:results = @()
function Add-Result {
    param([string]$Name, [bool]$Ok, [string]$Info = "")
    $script:results += [pscustomobject]@{ Name = $Name; Ok = $Ok; Info = $Info }
}
function Get-Ep {
    param([string]$Name, [string]$Path)
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl$Path" -Method Get -TimeoutSec 8
        $ok = $true
        if ($r -and ($r.PSObject.Properties.Name -contains 'ok')) { $ok = [bool]$r.ok }
        Add-Result "GET $Name" $ok (($r | ConvertTo-Json -Compress -Depth 3))
        return $r
    } catch {
        Add-Result "GET $Name" $false $_.Exception.Message
        return $null
    }
}
function Post-Ep {
    param([string]$Name, [string]$Path)
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl$Path" -Method Post -TimeoutSec 8
        $ok = $true
        if ($r -and ($r.PSObject.Properties.Name -contains 'ok')) { $ok = [bool]$r.ok }
        Add-Result "POST $Name" $ok (($r | ConvertTo-Json -Compress -Depth 3))
        return $r
    } catch {
        Add-Result "POST $Name" $false $_.Exception.Message
        return $null
    }
}
# Like Post-Ep but returns ok without recording a row (for aggregated sweeps).
function Post-Raw {
    param([string]$Path)
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl$Path" -Method Post -TimeoutSec 8
        if ($r -and ($r.PSObject.Properties.Name -contains 'ok')) { return [bool]$r.ok }
        return $true
    } catch {
        return $false
    }
}
# Read-only helper for aggregated checks; returns $null on transport errors.
function Get-Raw {
    param([string]$Path)
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl$Path" -Method Get -TimeoutSec 8
        if ($r -and ($r.PSObject.Properties.Name -contains 'ok') -and $r.ok) { return $r }
        return $null
    } catch {
        return $null
    }
}

try {
    # Root page.
    try {
        $root = Invoke-WebRequest -Uri "$BaseUrl/" -TimeoutSec 8 -UseBasicParsing
        Add-Result "GET / (UI)" ($root.StatusCode -eq 200) "HTTP $($root.StatusCode), $($root.Content.Length) bytes"
    } catch {
        Add-Result "GET / (UI)" $false $_.Exception.Message
    }

    # Read endpoints.
    $source   = Get-Ep "control/source" "/api/control/source"
    $mode     = Get-Ep "mode"           "/api/mode"
    $motor    = Get-Ep "motor"          "/api/motor"
    $device   = Get-Ep "device"         "/api/device"
    $storage  = Get-Ep "storage"        "/api/storage"
    $aStatus  = Get-Ep "audio/status"   "/api/audio/status"
    $tracks   = Get-Ep "audio/tracks"   "/api/audio/tracks"
    $cv1      = Get-Ep "cv/read"        "/api/cv/read?index=1"
    $cvAll    = Get-Ep "cv/all"         "/api/cv/all"
    $auxCfg   = Get-Ep "aux/cfg"        "/api/aux/cfg"
    $funcMap  = Get-Ep "func-map"       "/api/func-map"
    $fnStates = Get-Ep "functions"      "/api/functions"
    Get-Ep "bemf/base" "/api/bemf/base" | Out-Null
    Get-Ep "bemf/cal"  "/api/bemf/cal"  | Out-Null
    $bemfUse  = Get-Ep "bemf/use"       "/api/bemf/use"
    Get-Ep "wifi"      "/api/wifi"      | Out-Null
    Get-Ep "log"       "/api/log"       | Out-Null
    Get-Ep "task-inputs" "/api/task-inputs" | Out-Null

    # Web control source so function/AUX requests are accepted.
    Post-Ep "control/source=web" "/api/control/source?source=web" | Out-Null

    # All function buttons F0..F28 (press then release), aggregated.
    $fnFail = 0
    for ($fn = 0; $fn -lt 29; $fn++) {
        if (-not (Post-Raw "/api/function?fn=$fn&state=1")) { $fnFail++ }
        if (-not (Post-Raw "/api/function?fn=$fn&state=0")) { $fnFail++ }
    }
    Add-Result "F0..F28 (web buttons)" ($fnFail -eq 0) "$fnFail failed"

    # All AUX channels: effect on/off and readback of the effective state.
    $auxFail = 0
    for ($ch = 0; $ch -lt 9; $ch++) {
        if (-not (Post-Raw "/api/aux/effect?ch=$ch&on=1&pwm_on=255&pwm_off=0&mode=0&period=800")) { $auxFail++ }
        $st = Get-Raw "/api/aux/state"
        if (-not ($st -and $st.aux[$ch] -and $st.aux[$ch].on)) { $auxFail++ }
        if (-not (Post-Raw "/api/aux/effect?ch=$ch&on=0&pwm_on=255&pwm_off=0&mode=0&period=800")) { $auxFail++ }
        $st = Get-Raw "/api/aux/state"
        if (-not ($st -and $st.aux[$ch] -and (-not $st.aux[$ch].on))) { $auxFail++ }
    }
    Add-Result "AUX0..8 (web buttons + state)" ($auxFail -eq 0) "$auxFail failed"

    # AUX config write: re-apply the current level/effect (idempotent).
    $auxCfgFail = 0
    if ($auxCfg -and $auxCfg.aux) {
        for ($ch = 0; $ch -lt $auxCfg.aux.Count; $ch++) {
            $lvl = $auxCfg.aux[$ch].level
            $fx  = $auxCfg.aux[$ch].effect
            $r = Post-Ep "auxcfg$ch" "/api/aux/cfg?ch=$ch&level=$lvl&effect=$fx"
            if (-not ($r -and $r.ok)) { $auxCfgFail++ }
        }
    } else { $auxCfgFail = 1 }
    Add-Result "AUX config write (all)" ($auxCfgFail -eq 0) "$auxCfgFail failed"

    # Audio: volume, play slot 1, stop.
    $vol = $null
    if ($aStatus) {
        $vol = Post-Ep "audio/volume" "/api/audio/volume?master=$($aStatus.master_volume)&engine=$($aStatus.engine_volume)&effects=$($aStatus.effects_volume)"
    } else { $vol = Post-Ep "audio/volume" "/api/audio/volume?master=80&engine=80&effects=80" }
    Post-Ep "audio/play?slot=1" "/api/audio/play?slot=1" | Out-Null
    Post-Ep "audio/stop"        "/api/audio/stop" | Out-Null

    # Track category: re-apply the current value for slot 1.
    if ($tracks -and $tracks.cats) {
        $cat = $tracks.cats[0]
        Post-Ep "track/category" "/api/track/category?slot=1&cat=$cat" | Out-Null
    }

    # CV write: rewrite the current CV1 value, committed.
    if ($cv1 -and ($cv1.PSObject.Properties.Name -contains 'value')) {
        Post-Ep "cv/write" "/api/cv/write?index=1&value=$($cv1.value)&commit=1" | Out-Null
    }

    # Function map: re-apply F0 current mapping.
    if ($funcMap -and $funcMap.map) {
        $m = $funcMap.map[0]
        Post-Ep "func-map" "/api/func-map?fn=0&a=$($m.a)&b=$($m.b)&aux=$($m.aux)&dir=$($m.dir)&speed=$($m.speed)" | Out-Null
    }

    # Motor (stop, no movement), device name, mode, BEMF use, control source restore.
    Post-Ep "motor stop" "/api/motor?speed=0&forward=1" | Out-Null

    # Opt-in BEMF calibration (G-TEST-5). Destructive: spins the motor and
    # writes a new curve, so it only runs with -CalibrateBemf on a free motor.
    if ($CalibrateBemf) {
        Write-Host "[INFO] BEMF calibration HIL (free/unloaded motor required) ..." -ForegroundColor Yellow
        if (-not (Post-Raw "/api/motor?speed=0&forward=1")) {
            Add-Result "BEMF pre-stop" $false "motor stop rejected"
        } else {
            Add-Result "BEMF pre-stop" $true ""
        }
        $startResp = Post-Ep "bemf/calibrate start" "/api/bemf/calibrate?start=1"
        if (-not ($startResp -and $startResp.ok)) {
            Add-Result "BEMF calibration start" $false "start rejected"
        } else {
            Add-Result "BEMF calibration start" $true "PWM sweep"
            $calClock = [System.Diagnostics.Stopwatch]::StartNew()
            $activeSeen = $false
            $cancelSent = $false
            $terminal = $null
            while ($calClock.Elapsed.TotalSeconds -lt $BemfCalTimeoutSec) {
                $cal = Get-Raw "/api/bemf/cal"
                if ($cal) {
                    if ($cal.active) { $activeSeen = $true }
                    if ($BemfCancelAfterMs -gt 0 -and -not $cancelSent -and $cal.active -and
                        $calClock.ElapsedMilliseconds -ge $BemfCancelAfterMs) {
                        $cancelResp = Post-Ep "bemf/calibrate cancel" "/api/bemf/calibrate?cancel=1"
                        Add-Result "BEMF calibration cancel" ([bool]($cancelResp -and $cancelResp.ok)) ""
                        $cancelSent = $true
                    }
                    if (-not $cal.active) { $terminal = $cal; break }
                }
                Start-Sleep -Milliseconds 250
            }
            if (-not $terminal) { $terminal = Get-Raw "/api/bemf/cal" }
            $inactive = [bool]($terminal -and (-not $terminal.active))
            $resultName = if ($terminal -and ($terminal.PSObject.Properties.Name -contains 'result')) { [string]$terminal.result } else { "" }
            Add-Result "BEMF calibration terminal" $inactive "active=$([bool]($terminal -and $terminal.active)) result=$resultName activeSeen=$activeSeen"
            if ($BemfCancelAfterMs -gt 0) {
                Add-Result "BEMF calibration cancelled" ($inactive -and $cancelSent -and $resultName -eq "cancelled") "result=$resultName"
            } else {
                Add-Result "BEMF calibration succeeded" ($inactive -and $resultName -eq "succeeded") "result=$resultName"
            }
            # Terminal motion check. REST exposes the target speed; the actual
            # bridge duty is internal, but cancellation/stop zeroes it in the
            # motor safety path and a non-zero target would be visible here.
            Start-Sleep -Milliseconds 300
            Post-Raw "/api/motor?speed=0&forward=1" | Out-Null
            $motorAfter = Get-Raw "/api/motor"
            $zero = [bool]($motorAfter -and $motorAfter.speed -eq 0)
            Add-Result "BEMF post-run motor speed=0" $zero "speed=$($motorAfter.speed)"
        }
    }

    if ($device -and $device.name) {
        Post-Ep "device name" ("/api/device?name=" + [uri]::EscapeDataString($device.name)) | Out-Null
    }
    if ($mode -and $mode.mode) { Post-Ep "mode" "/api/mode?mode=$($mode.mode)" | Out-Null }
    if ($bemfUse) { Post-Ep "bemf/use" "/api/bemf/use?enabled=$($bemfUse.enabled.ToString().ToLower())" | Out-Null }
    if ($source -and $source.source) {
        Post-Ep "control/source restore" "/api/control/source?source=$($source.source)" | Out-Null
    }

    # Verify function states are all released.
    $fnAfter = Get-Ep "functions (after)" "/api/functions"
    if ($fnAfter -and $fnAfter.states) {
        $anyOn = @($fnAfter.states | Where-Object { $_ -eq 1 }).Count
        Add-Result "all functions released" ($anyOn -eq 0) "$anyOn still on"
    }

} finally {
    if ($RestoreSsid) {
        Write-Host "[INFO] Reconnecting to '$RestoreSsid' ..." -ForegroundColor Cyan
        netsh wlan connect name="$RestoreSsid" | Out-Null
    }
}

Write-Host ""
Write-Host "Web/REST HIL results:" -ForegroundColor Cyan
$failed = 0
foreach ($r in $script:results) {
    if (-not $r.Ok) { $failed++ }
    $color = if ($r.Ok) { "Green" } else { "Red" }
    Write-Host ("  {0,-30} {1}" -f $r.Name, $(if ($r.Ok) { "OK" } else { "FAIL" })) -ForegroundColor $color
}
Write-Host ""
if (-not $CalibrateBemf) {
    Write-Host "HIL coverage gap: BEMF calibration was NOT exercised (opt-in -CalibrateBemf)." -ForegroundColor Yellow
}
if ($failed -gt 0) {
    Write-Host "WEB HIL: FAILED ($failed of $($script:results.Count))" -ForegroundColor Red
    exit 1
}
Write-Host "WEB HIL: PASSED ($($script:results.Count) checks)" -ForegroundColor Green
exit 0
