@echo off
setlocal DisableDelayedExpansion
set "DCC_GIT_SYNC_FILE=%~f0"
set "DCC_GIT_SYNC_MODE=interactive"
if "%~1"=="" goto run
if /i "%~1"=="--check" (
    set "DCC_GIT_SYNC_MODE=check"
    goto run
)
if /i "%~1"=="--preview" (
    set "DCC_GIT_SYNC_MODE=preview"
    goto run
)
if /i "%~1"=="--no-pause" goto run
echo Usage: %~nx0 [--check ^| --preview ^| --no-pause]
exit /b 2
:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$text=[IO.File]::ReadAllText($env:DCC_GIT_SYNC_FILE); $marker='#'+' POWERSHELL_PAYLOAD'; $code=$text.Substring($text.LastIndexOf($marker)+$marker.Length); if($env:DCC_GIT_SYNC_MODE -eq 'check'){$tokens=$null;$errors=$null;[void][Management.Automation.Language.Parser]::ParseInput($code,[ref]$tokens,[ref]$errors);if($errors.Count){$errors|ForEach-Object{Write-Error $_.Message};exit 1};Write-Host 'Syntax OK. No Git operations performed.';exit 0}; & ([scriptblock]::Create($code))"
set "DCC_GIT_SYNC_EXIT=%ERRORLEVEL%"
if "%~1"=="" pause
exit /b %DCC_GIT_SYNC_EXIT%

# POWERSHELL_PAYLOAD
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    param([string[]]$Arguments)
    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw ('Git failed: git ' + ($Arguments -join ' '))
    }
}

function Get-CurrentBranch {
    $name = @(Invoke-Git -Arguments @('branch', '--show-current')) -join ''
    if ([string]::IsNullOrWhiteSpace($name)) {
        throw 'Detached HEAD: switch to a branch before sending changes.'
    }
    return $name
}

try {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'Git is not installed or is not available in PATH.'
    }
    Set-Location -LiteralPath ([IO.Path]::GetDirectoryName($env:DCC_GIT_SYNC_FILE))
    $root = @(Invoke-Git -Arguments @('rev-parse', '--show-toplevel')) -join ''
    Set-Location -LiteralPath $root
    $branch = Get-CurrentBranch
    $conflicts = @(Invoke-Git -Arguments @('diff', '--name-only', '--diff-filter=U'))
    if ($conflicts.Count -gt 0) { throw 'Unresolved merge conflicts. Nothing will be sent.' }
    foreach ($state in @('MERGE_HEAD', 'CHERRY_PICK_HEAD', 'REVERT_HEAD', 'rebase-merge', 'rebase-apply')) {
        $statePath = @(Invoke-Git -Arguments @('rev-parse', '--git-path', $state)) -join ''
        if (Test-Path -LiteralPath $statePath) { throw ('Finish or cancel the active Git operation: ' + $state) }
    }

    $remote = 'origin'
    $destination = 'refs/heads/' + $branch
    $configuredRemote = @(& git config --get ('branch.' + $branch + '.remote')) -join ''
    $configuredMerge = @(& git config --get ('branch.' + $branch + '.merge')) -join ''
    if ($configuredRemote -and $configuredMerge.StartsWith('refs/heads/')) {
        if ($configuredRemote -eq '.') { throw 'The upstream is local. Configure a remote upstream first.' }
        $remote = $configuredRemote
        $destination = $configuredMerge
    }
    Invoke-Git -Arguments @('check-ref-format', $destination)
    $urls = @(Invoke-Git -Arguments @('remote', 'get-url', '--push', '--all', '--', $remote))
    if ($urls.Count -ne 1) { throw 'Multiple push URLs are configured. Select one destination explicitly before sending.' }
    $url = $urls[0]
    $changes = @(Invoke-Git -Arguments @('status', '--porcelain=v1', '--untracked-files=normal'))

    Write-Host ('Repository: ' + $root)
    Write-Host ('Branch:     ' + $branch)
    Write-Host ('Destination: ' + $remote + '/' + $destination.Substring(11))
    Write-Host ('Push URL:   ' + $url)
    Invoke-Git -Arguments @('status', '--short')
    Invoke-Git -Arguments @('diff', '--stat')
    Invoke-Git -Arguments @('diff', '--cached', '--stat')
    Invoke-Git -Arguments @('log', '--oneline', '-10')
    Write-Host 'All tracked changes and non-ignored new files will be included.'
    Write-Host 'Review the list above. Do not commit passwords, tokens or private files.'
    if ($env:DCC_GIT_SYNC_MODE -eq 'preview') {
        Write-Host 'Preview complete. Nothing staged, committed or pushed.'
        exit 0
    }

    $message = ''
    if ($changes.Count -gt 0) {
        $message = Read-Host 'Commit message'
        if ([string]::IsNullOrWhiteSpace($message)) { throw 'Commit message cannot be empty.' }
    } else {
        Write-Host 'No local file changes. Existing commits will be pushed; no empty commit will be created.'
    }
    $confirmation = Read-Host 'Type SEND to commit and push (anything else cancels)'
    if ($confirmation -cne 'SEND') { Write-Host 'Cancelled. Git state unchanged.'; exit 0 }
    if ((Get-CurrentBranch) -cne $branch) { throw 'Branch changed during confirmation. Cancelled.' }

    if ($changes.Count -gt 0) {
        $tempRoot = [IO.Path]::GetTempPath()
        if (-not (Test-Path -LiteralPath $tempRoot -PathType Container)) { throw 'Temporary directory is unavailable.' }
        $messageFile = Join-Path $tempRoot ('dcc-git-message-' + [guid]::NewGuid().ToString('N') + '.txt')
        try {
            [IO.File]::WriteAllText($messageFile, $message, [Text.UTF8Encoding]::new($false))
            Invoke-Git -Arguments @('add', '--all', '--', '.')
            Invoke-Git -Arguments @('diff', '--cached', '--check')
            $staged = @(Invoke-Git -Arguments @('diff', '--cached', '--name-only'))
            if ((Get-CurrentBranch) -cne $branch) { throw 'Branch changed before commit. Staged changes remain local.' }
            if ($staged.Count -gt 0) { Invoke-Git -Arguments @('commit', '--file', $messageFile) }
        } finally {
            if ($messageFile -and (Test-Path -LiteralPath $messageFile)) {
                try { [IO.File]::Delete($messageFile) } catch { Write-Warning ('Cannot remove temporary message: ' + $messageFile) }
            }
        }
    }
    if ((Get-CurrentBranch) -cne $branch) { throw 'Branch changed. Push cancelled; any created commit remains local.' }
    $currentUrls = @(Invoke-Git -Arguments @('remote', 'get-url', '--push', '--all', '--', $remote))
    if ($currentUrls.Count -ne 1 -or $currentUrls[0] -cne $url) { throw 'Remote changed. Push cancelled; any created commit remains local.' }
    Invoke-Git -Arguments @('push', '--set-upstream', '--', $remote, ('HEAD:' + $destination))
    Write-Host 'SUCCESS: changes sent to the configured remote branch.' -ForegroundColor Green
    exit 0
} catch {
    Write-Host ('FAILED: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'No force-push or automatic pull/rebase was performed. A created commit remains local if push failed.'
    exit 1
}
