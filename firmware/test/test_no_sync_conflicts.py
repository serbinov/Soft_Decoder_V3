"""Guard against OneDrive/OwnCloud sync conflict artifacts in the repository.

The repository may live inside a OneDrive/OwnCloud-synced folder. When two
machines write the same file, the sync client leaves "conflicted copy" files
next to the original and can silently revert a source file to an older
revision. That is how web_func.c/app_main.c were rolled back during HIL 0.32,
so the intended git revision must be restored and the artifacts deleted.
"""

import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent

SKIP_DIR_NAMES = {".git", "node_modules"}
SKIP_DIR_REL = Path("firmware") / "build"

CONFLICT_MESSAGE = (
    "Sync conflict artifacts detected; restore the intended git revision "
    "and delete them."
)


def is_conflict_artifact(name):
    lowered = name.lower()
    return (
        "conflicted copy" in lowered
        or name.startswith("~$")
        or lowered.endswith((".orig", ".rej"))
        or ".sync-conflict" in lowered
    )


def is_skipped_dir(child, repo):
    if child.name in SKIP_DIR_NAMES:
        return True
    try:
        return child.relative_to(repo) == SKIP_DIR_REL
    except ValueError:
        return False


def find_conflict_artifacts(repo=REPO):
    offenders = []
    for dirpath, dirnames, filenames in os.walk(repo):
        current = Path(dirpath)
        kept = []
        for name in sorted(dirnames):
            child = current / name
            if is_skipped_dir(child, repo):
                continue
            if is_conflict_artifact(name):
                offenders.append(child)
                continue
            kept.append(name)
        dirnames[:] = kept
        for name in sorted(filenames):
            if is_conflict_artifact(name):
                offenders.append(current / name)
    return sorted(offenders)


def format_offenders(offenders, repo=REPO):
    try:
        paths = "\n".join("  " + str(p.relative_to(repo)) for p in offenders)
    except ValueError:
        paths = "\n".join("  " + str(p) for p in offenders)
    return CONFLICT_MESSAGE + "\n" + paths


class NoSyncConflictsTest(unittest.TestCase):
    def test_clean_repo_has_no_conflict_artifacts(self):
        offenders = find_conflict_artifacts()
        self.assertEqual(offenders, [], format_offenders(offenders))

    def test_detector_matches_known_artifact_names(self):
        for name in (
            "web_func.c (conflicted copy 2026-11-24)",
            "app_main.c (Conflicted Copy)",
            "~$report.docx",
            "app_main.c.orig",
            "web_func.c.rej",
            "app_main.c.sync-conflict-20261124-120000-ABCDEFG",
        ):
            self.assertTrue(is_conflict_artifact(name), name)

        for name in (
            "app_main.c",
            "web_func.h",
            "orig",
            "conflicted.txt",
            "test_no_sync_conflicts.py",
        ):
            self.assertFalse(is_conflict_artifact(name), name)

    def test_walker_reports_artifacts_but_skips_build_and_git(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "firmware" / "main").mkdir(parents=True)
            (repo / "firmware" / "main" / "app_main.c").write_text("ok", encoding="utf-8")
            artifact = repo / "firmware" / "main" / "app_main.c.orig"
            artifact.write_text("stale", encoding="utf-8")
            ignored = [
                repo / ".git" / "x.orig",
                repo / "node_modules" / "y.rej",
                repo / "firmware" / "build" / "z.sync-conflict",
            ]
            for path in ignored:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("ignored", encoding="utf-8")

            self.assertEqual(find_conflict_artifacts(repo), [artifact])


if __name__ == "__main__":
    unittest.main()
