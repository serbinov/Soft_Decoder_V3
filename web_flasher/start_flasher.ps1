# Local control panel for the ADDITIPUS flasher.
#
# The web page is configuration/status only: the actual flashing is done by the
# project scripts (firmware\flash_standalone.ps1, make_ota.ps1). This server
# serves the page, reports device/files status, runs the selected script and
# streams its output back to the page.
#
# No external dependencies: built-in .NET HttpListener in PowerShell.
#
#   start_flasher.bat                 # serve + open the browser
#   start_flasher.bat -Port 9000      # HTTP port
#   powershell -File start_flasher.ps1 -NoOpen

param(
    [int]$Port = 8765,
    [switch]$NoOpen,
    [switch]$NoIdleExit
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Web

$web    = $PSScriptRoot
$repo   = Split-Path $web -Parent
$fwDir  = Join-Path $repo "release\flash_download_tool"

$sndDir = $null
foreach ($c in @((Join-Path (Split-Path $repo -Parent) "SOUND"), (Join-Path $repo "SOUND"))) {
    if ($c -and (Test-Path -LiteralPath $c)) { $sndDir = (Resolve-Path $c).Path; break }
}

# ---- status helpers -------------------------------------------------------
function Get-DevicePort {
    $p = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like "USB\VID_303A*PID_1001*" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } } |
        Select-Object -First 1
    if ($p) { return $p }
    return $null
}
function Get-DeviceMac {
    # ESP32-S3 USB-Serial-JTAG exposes the base MAC as the USB serial number
    # (USB\VID_303A&PID_1001\AA:BB:CC:DD:EE:FF) - readable without download mode.
    $pnp = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -match 'VID_303A&PID_1001\\' } |
        ForEach-Object { if ($_.PNPDeviceID -match '\\([0-9A-Fa-f:]{17})$') { $Matches[1] } } |
        Select-Object -First 1
    if ($pnp) { return $pnp.ToUpper() }
    return $null
}
function Get-IdfPath {
    $cands = @()
    if ($env:IDF_PATH) { $cands += $env:IDF_PATH }
    $cands += @("C:\esp\v6.0\esp-idf", "C:\esp\esp-idf",
                (Join-Path $env:USERPROFILE "esp\v6.0\esp-idf"),
                (Join-Path $env:USERPROFILE "esp\esp-idf"))
    foreach ($c in $cands) { if ($c -and (Test-Path (Join-Path $c "tools\idf.py"))) { return $c } }
    return $null
}
function Get-EsptoolExe {
    foreach ($e in @((Join-Path $repo "firmware\tools\esptool\esptool.exe"),
                     (Join-Path $repo "firmware\esptool.exe"),
                     (Join-Path $web "esptool.exe"))) {
        if (Test-Path -LiteralPath $e) { return $e }
    }
    return $null
}
# "esptool" when a standalone esptool.exe or ESP-IDF is present, else the ROM fallback.
function Get-Flasher { if ((Get-EsptoolExe) -or (Get-IdfPath)) { return "esptool" } else { return "rom" } }

# ---- statistics (persisted to web_flasher\stats.json) ---------------------
$script:statsPath = Join-Path $web "stats.json"
$script:esptoolVer = $null
function Get-EsptoolVersion {
    if ($script:esptoolVer) { return $script:esptoolVer }
    $exe = Get-EsptoolExe
    if ($exe) { try { $script:esptoolVer = (((& $exe version 2>$null | Select-Object -First 1)) -replace '\s+', ' ').Trim() } catch { } }
    if (-not $script:esptoolVer) { $script:esptoolVer = "" }
    return $script:esptoolVer
}
function Read-Stats {
    if (-not (Test-Path -LiteralPath $script:statsPath)) { return @() }
    try { $j = Get-Content -LiteralPath $script:statsPath -Raw -Encoding UTF8 | ConvertFrom-Json; if ($null -eq $j) { return @() }; return @($j) } catch { return @() }
}
function Add-Stat($rec) {
    $arr = @(Read-Stats) + @($rec)
    if ($arr.Count -gt 500) { $arr = $arr[($arr.Count - 500)..($arr.Count - 1)] }
    try {
        $json = ConvertTo-Json -InputObject @($arr) -Depth 6
        [IO.File]::WriteAllText($script:statsPath, $json, (New-Object System.Text.UTF8Encoding($false)))
    } catch { }
}
function Get-RunDetails {
    $ver = ""; $vf = Join-Path $repo "firmware\version.txt"
    if (Test-Path -LiteralPath $vf) { $ver = (Get-Content -LiteralPath $vf -Raw).Trim() }
    $fwBytes = 0; $fwb = Join-Path $fwDir "firmware.bin"
    if (Test-Path -LiteralPath $fwb) { $fwBytes = (Get-Item -LiteralPath $fwb).Length }
    $snd = 0; $sndBytes = 0
    if ($sndDir) {
        $wavs = @(Get-ChildItem -LiteralPath $sndDir -Filter *.wav -ErrorAction SilentlyContinue)
        $snd = $wavs.Count
        if ($snd) { $sndBytes = [int64](($wavs | Measure-Object Length -Sum).Sum) }
    }
    return @{ version = $ver; fwBytes = $fwBytes; sndBytes = $sndBytes; sounds = $snd; flasher = (Get-Flasher); esptool = (Get-EsptoolVersion); idf = (Get-IdfPath) }
}
# Last meaningful error line from a failed run's log (for the statistics table).
function Get-LastError([string]$logPath, $code) {
    if (Test-Path -LiteralPath $logPath) {
        try {
            $lines = @(Get-Content -LiteralPath $logPath -Encoding UTF8)
            for ($i = $lines.Count - 1; $i -ge 0; $i--) {
                $l = ($lines[$i] -replace '\s+', ' ').Trim()
                if ($l -match '\[ERROR\]') { return ($l -replace '^.*?\[ERROR\]\s*', '') }
            }
            for ($i = $lines.Count - 1; $i -ge 0; $i--) {
                $l = ($lines[$i] -replace '\s+', ' ').Trim()
                if ($l -match 'ошибка|failed|error|missing|не удалось|timeout|no put-ok|no file-ok|no prov-ok') { return $l }
            }
            for ($i = $lines.Count - 1; $i -ge 0; $i--) { $l = $lines[$i].Trim(); if ($l) { return $l } }
        } catch { }
    }
    if ($null -ne $code) { return ("код " + $code) }
    return "ошибка"
}

# ---- one run at a time ----------------------------------------------------
$script:cur = $null

function Get-ActionSpec([string]$action) {
    $esptool = Join-Path $repo "firmware\flash_esptool.ps1"
    $standalone = Join-Path $repo "firmware\flash_standalone.ps1"
    if ((Get-EsptoolExe) -or (Get-IdfPath)) {
        # Preferred: esptool writes and self-verifies the hash.
        switch ($action) {
            "flash"        { return @{ script = $esptool; args = @() } }
            "flash_sounds" { return @{ script = $esptool; args = @("-Sounds") } }
        }
    } else {
        # Fallback: pure-PowerShell ROM flasher (no ESP-IDF needed).
        switch ($action) {
            "flash"        { return @{ script = $standalone; args = @("-NoErase", "-NoSounds") } }
            "flash_sounds" { return @{ script = $standalone; args = @("-NoErase") } }
        }
    }
    if ($action -eq "ota") { return @{ script = (Join-Path $web "make_ota.ps1"); args = @() } }
    return $null
}

function Start-Action([string]$action) {
    $spec = Get-ActionSpec $action
    if (-not $spec) { return $null }
    if (-not (Test-Path -LiteralPath $spec.script)) { return $null }
    if ($script:cur -and -not $script:cur.proc.HasExited) {
        try { $script:cur.proc.Kill() } catch { }
    }
    $id  = [guid]::NewGuid().ToString("N")
    $log = Join-Path $env:TEMP "dcc_panel_$id.log"
    $wrapper = Join-Path $env:TEMP "dcc_panel_$id.ps1"
    $argLine = ($spec.args | ForEach-Object { if ($_ -match '\s') { "'" + ($_ -replace "'", "''") + "'" } else { $_ } }) -join " "
    # Capture every stream (Write-Host included) as UTF-8 into the log file and
    # propagate the script's exit code to this wrapper.
    $content = "& '" + ($spec.script -replace "'", "''") + "' $argLine *>&1 | Out-File -LiteralPath '" +
               ($log -replace "'", "''") + "' -Encoding utf8`r`nexit `$LASTEXITCODE`r`n"
    Set-Content -LiteralPath $wrapper -Value $content -Encoding UTF8
    $p = Start-Process -FilePath "powershell" -PassThru -WindowStyle Hidden -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $wrapper)
    $script:cur = @{ id = $id; proc = $p; log = $log; done = $false; code = $null; sent = 0;
                     recorded = $false; action = $action; startedAt = (Get-Date); port = (Get-DevicePort) }
    return $id
}

# ---- http ----------------------------------------------------------------
$prefix  = "http://127.0.0.1:$Port/"
$pageUrl = $prefix + "web_flasher/index.html"
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add($prefix)
try { $listener.Start() } catch {
    Write-Host "Не удалось открыть порт $Port (занят?). Запустите: start_flasher.bat -Port 9000" -ForegroundColor Red
    exit 1
}

$mime = @{
    ".html" = "text/html; charset=utf-8"; ".js" = "text/javascript; charset=utf-8"
    ".css"  = "text/css; charset=utf-8"; ".json" = "application/json; charset=utf-8"
    ".txt"  = "text/plain; charset=utf-8"; ".md" = "text/plain; charset=utf-8"
    ".bin"  = "application/octet-stream"; ".wav" = "audio/wav"; ".ico" = "image/x-icon"
}
$repoFull = [IO.Path]::GetFullPath($repo)
$sndFull  = if ($sndDir) { [IO.Path]::GetFullPath($sndDir) } else { $null }
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# Idle watchdog: the page sends /api/ping; when it stops, the server exits.
$script:lastPing = $null
$script:sawPing  = $false
$script:startedAt = Get-Date

function Send-Text($ctx, [string]$text, [string]$ctype) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    if ($ctype) { $ctx.Response.ContentType = $ctype }
    $ctx.Response.Headers["Cache-Control"] = "no-store"
    $ctx.Response.ContentLength64 = $bytes.Length
    $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
}

Write-Host "== ADDITIPUS панель прошивки ==" -ForegroundColor Cyan
Write-Host "  репозиторий : $repo"
Write-Host "  прошивка    : $fwDir"
Write-Host "  звуки       : $(if ($sndDir) { $sndDir } else { 'не найдены' })"
Write-Host ""
Write-Host "Открываю: $pageUrl" -ForegroundColor Green
Write-Host "Остановить: закройте это окно (Ctrl+C)." -ForegroundColor DarkGray
if (-not $NoOpen) { Start-Process $pageUrl }

try {
$iar = $listener.BeginGetContext($null, $null)
while ($listener.IsListening) {
    if ($iar.AsyncWaitHandle.WaitOne(1000)) {
    $ctx = $listener.EndGetContext($iar)
    $iar = $listener.BeginGetContext($null, $null)
    try {
        $pathQ = $ctx.Request.Url.AbsolutePath
        $q = [System.Web.HttpUtility]::ParseQueryString($ctx.Request.Url.Query)

        if ($pathQ -eq "/api/ping") {
            $script:lastPing = Get-Date; $script:sawPing = $true
            Send-Text $ctx '{"ok":true}' "application/json; charset=utf-8"
        }
        elseif ($pathQ -eq "/api/status") {
            $fwCount = @(Get-ChildItem -LiteralPath $fwDir -Filter *.bin -ErrorAction SilentlyContinue).Count
            $sndCount = if ($sndDir) { @(Get-ChildItem -LiteralPath $sndDir -Filter *.wav -ErrorAction SilentlyContinue).Count } else { 0 }
            $obj = [ordered]@{
                port = (Get-DevicePort); idf = (Get-IdfPath)
                firmware = $fwCount; sounds = $sndCount
                fwDir = "release\flash_download_tool"; sndDir = $sndDir
                flasher = (Get-Flasher)
                busy = [bool]($script:cur -and -not $script:cur.proc.HasExited)
            }
            Send-Text $ctx ($obj | ConvertTo-Json -Compress) "application/json; charset=utf-8"
        }
        elseif ($pathQ -eq "/api/stats") {
            $arr = @(Read-Stats)
            $okCount = @($arr | Where-Object { $_.ok }).Count
            $last = if ($arr.Count) { $arr[-1] } else { $null }
            $items = @($arr | Select-Object -Last 100)
            $obj = [ordered]@{ total = $arr.Count; ok = $okCount; fail = ($arr.Count - $okCount); last = $last; items = $items }
            Send-Text $ctx ($obj | ConvertTo-Json -Depth 6 -Compress) "application/json; charset=utf-8"
        }
        elseif ($pathQ -eq "/api/run") {
            $action = $q["action"]
            $id = Start-Action $action
            if (-not $id) { Send-Text $ctx '{"error":"unknown action"}' "application/json; charset=utf-8" }
            else { Send-Text $ctx ('{"id":"' + $id + '"}') "application/json; charset=utf-8" }
        }
        elseif ($pathQ -eq "/api/state") {
            $pos = 0; [void][int]::TryParse($q["pos"], [ref]$pos)
            $cur = $script:cur
            if (-not $cur) { Send-Text $ctx '{"text":"","pos":0,"done":true,"code":null}' "application/json; charset=utf-8" }
            else {
                if (-not $cur.done -and $cur.proc.HasExited) { $cur.done = $true; $cur.code = $cur.proc.ExitCode }
                if ($cur.done -and -not $cur.recorded) {
                    $cur.recorded = $true
                    try {
                        $d = Get-RunDetails
                        $fin = Get-Date
                        Add-Stat ([ordered]@{
                            ts = $fin.ToString("yyyy-MM-dd HH:mm:ss")
                            action = $cur.action
                            ok = ($cur.code -eq 0)
                            code = $cur.code
                            error = if ($cur.code -ne 0) { Get-LastError $cur.log $cur.code } else { "" }
                            port = $cur.port
                            mac = (Get-DeviceMac)
                            durationSec = [int]($fin - $cur.startedAt).TotalSeconds
                            version = $d.version
                            fwBytes = $d.fwBytes
                            sndBytes = $d.sndBytes
                            sounds = $d.sounds
                            flasher = $d.flasher
                            esptool = $d.esptool
                            idf = $d.idf
                        })
                    } catch { }
                }
                $text = ""
                if (Test-Path -LiteralPath $cur.log) {
                    try {
                        $fs = [IO.File]::Open($cur.log, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
                        if ($pos -lt $fs.Length) {
                            [void]$fs.Seek($pos, [IO.SeekOrigin]::Begin)
                            $buf = New-Object byte[] ($fs.Length - $pos)
                            $read = $fs.Read($buf, 0, $buf.Length)
                            [void]$fs.Close()
                            $text = [Text.Encoding]::UTF8.GetString($buf, 0, $read)
                            if ($pos -eq 0) { $text = $text.TrimStart([char]0xFEFF) }
                            $pos += $read
                        } else { [void]$fs.Close() }
                    } catch { }
                }
                if ($cur.done -and $text -notmatch "\[код") { $text += "`n[завершено, код $($cur.code)]`n" }
                $obj = [ordered]@{ text = $text; pos = $pos; done = $cur.done; code = $cur.code }
                Send-Text $ctx ($obj | ConvertTo-Json -Compress) "application/json; charset=utf-8"
            }
        }
        else {
            # static files
            $rel = [Uri]::UnescapeDataString($pathQ).TrimStart("/") -replace "/", "\"
            $full = $null; $root = $repoFull
            if ($sndFull -and $rel -like "SOUND\*") { $full = Join-Path $sndFull $rel.Substring(6); $root = $sndFull }
            else { $full = Join-Path $repoFull $rel }
            $full = [IO.Path]::GetFullPath($full)
            if (-not $full.StartsWith($root)) { $ctx.Response.StatusCode = 403 }
            elseif (Test-Path -LiteralPath $full -PathType Leaf) {
                $ext = [IO.Path]::GetExtension($full).ToLower()
                if ($mime.ContainsKey($ext)) { $ctx.Response.ContentType = $mime[$ext] }
                $ctx.Response.Headers["Cache-Control"] = "no-store"
                $bytes = [IO.File]::ReadAllBytes($full)
                $ctx.Response.ContentLength64 = $bytes.Length
                $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            }
            else { $ctx.Response.StatusCode = 404 }
        }
    } catch {
        try { $ctx.Response.StatusCode = 500 } catch { }
    } finally {
        try { $ctx.Response.OutputStream.Close() } catch { }
    }
    }
    if (-not $NoIdleExit) {
        if ($script:sawPing) {
            if (((Get-Date) - $script:lastPing).TotalSeconds -gt 12) {
                Write-Host "Страница закрыта - останавливаю сервер." -ForegroundColor DarkGray; break
            }
        } elseif (((Get-Date) - $script:startedAt).TotalSeconds -gt 300) {
            Write-Host "Страница не подключилась - останавливаю сервер." -ForegroundColor DarkGray; break
        }
    }
}
} finally {
    # Do not leave a flashing child holding the serial port after the panel exits.
    if ($script:cur -and -not $script:cur.proc.HasExited) { try { $script:cur.proc.Kill() } catch { } }
    try { $listener.Stop() } catch { }
}
