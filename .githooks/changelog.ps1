# Auto-records each commit in the root CHANGELOG.md.
#
# Invoked by the post-commit hook (.githooks/post-commit). It inserts the line
# "- <date> (v<version>) - <commit subject>" right after the <!-- AUTO-LOG -->
# marker, stages the file and folds it into the just-made commit with
# --amend --no-edit (recursion-guarded). All errors are swallowed so a commit
# is never blocked.
param()

$ErrorActionPreference = "SilentlyContinue"

# Recursion guard: the --amend below triggers this hook again; skip it.
if ($env:DCC_AUTOCHANGELOG -eq "1") { exit 0 }

try {
    $root = (git rev-parse --show-toplevel 2>$null | Select-Object -First 1)
    if (-not $root) { exit 0 }
    $root = $root.Trim()

    # Never touch merges or rebases.
    $gitdir = (git rev-parse --git-dir 2>$null | Select-Object -First 1)
    if ($gitdir) {
        $gitdir = $gitdir.Trim()
        if ((Test-Path -LiteralPath (Join-Path $gitdir "MERGE_HEAD")) -or
            (Test-Path -LiteralPath (Join-Path $gitdir "rebase-merge")) -or
            (Test-Path -LiteralPath (Join-Path $gitdir "rebase-apply"))) { exit 0 }
    }

    $log = Join-Path $root "CHANGELOG.md"
    if (-not (Test-Path -LiteralPath $log)) { exit 0 }
    $marker = "<!-- AUTO-LOG -->"

    $subject = (git log -1 --pretty=%s 2>$null | Select-Object -First 1)
    if (-not $subject) { exit 0 }
    $subject = $subject.Trim()

    $ver = ""
    $vpath = Join-Path $root "VERSION"
    if (Test-Path -LiteralPath $vpath) {
        $ver = (Get-Content -LiteralPath $vpath -Raw).Trim()
    }

    $date = Get-Date -Format "yyyy-MM-dd"
    $verpart = if ($ver) { " (v$ver)" } else { "" }
    $entry = "- $date$verpart " + [char]0x2014 + " $subject"

    $text = [System.IO.File]::ReadAllText($log, [System.Text.Encoding]::UTF8)
    if (-not $text.Contains($marker)) { exit 0 }
    if ($text.Contains($entry)) { exit 0 }

    $text = $text.Replace($marker, $marker + "`n" + $entry)
    [System.IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($true)))

    git add -- "$log"
    $env:DCC_AUTOCHANGELOG = "1"
    git commit --amend --no-edit --no-verify 2>$null | Out-Null
} catch { }
exit 0
