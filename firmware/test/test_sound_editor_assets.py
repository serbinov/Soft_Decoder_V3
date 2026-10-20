import gzip
import importlib.util
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "gen_sound_editor.py"
SPEC = importlib.util.spec_from_file_location("editor_assets", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class EditorAssetTests(unittest.TestCase):
    def test_missing_build_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "editor build"):
                MODULE.generate(directory, directory)

    def test_exact_bytes_and_deterministic_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = b'<script>const x = `a\n b`; // keep line\n</script>'
            (root / "blocks.html").write_bytes(original)
            output = root / "out"
            output.mkdir()
            first = MODULE.generate(root, output)
            generated = (output / "sound_editor_assets.c").read_bytes()
            # Do not recurse into output, which is never a child of dist in production.
            (output / "sound_editor_assets.c").unlink()
            self.assertEqual(first, MODULE.generate(root, output))
            self.assertEqual(generated, (output / "sound_editor_assets.c").read_bytes())
            numbers = re.search(rb"\[\] = \{(.*?)\};", generated, re.S).group(1)
            embedded = bytes(int(number) for number in re.findall(rb"\d+", numbers))
            self.assertEqual(original, gzip.decompress(embedded))
            self.assertIn(b"text/html; charset=utf-8", generated)

    def test_unsupported_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "blocks.html").write_text("test", encoding="utf-8")
            (root / "debug.map").write_text("unwanted source map", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                MODULE.generate(root, root)

    def test_repository_editor_build_is_current_and_servable(self):
        root = Path(__file__).resolve().parents[1]
        dist = root / "sound_editor" / "dist"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            total = MODULE.generate(dist, output, root / "sound_editor")
            generated = (output / "sound_editor_assets.c").read_text(encoding="utf-8")
            # Every advertised file is embedded with a safe /sound-editor/ path.
            self.assertIn('"/sound-editor/blocks.html"', generated)
            self.assertIn("text/html; charset=utf-8", generated)
            self.assertIn('"/sound-editor/THIRD_PARTY_NOTICES.txt"', generated)
            self.assertLessEqual(total, 512 * 1024)
            # The embedded gzip stream must decompress to the exact source page.
            numbers = re.search(r"\[\] = \{(.*?)\};", generated, re.S).group(1)
            embedded = bytes(int(number) for number in re.findall(r"\d+", numbers))
            self.assertEqual((dist / "blocks.html").read_bytes(), gzip.decompress(embedded))

    def test_stale_build_and_missing_stamp_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            dist = source / "dist"
            dist.mkdir()
            with self.assertRaisesRegex(ValueError, "stamp missing"):
                MODULE.verify_sources(dist, source)
            files = {}
            for name in ("package.json",):
                path = source / name
                path.write_bytes(b"source")
                files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            (dist / "build-inputs.json").write_text(json.dumps({"files": files}), encoding="utf-8")
            MODULE.verify_sources(dist, source)
            (source / "package.json").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "changed after build"):
                MODULE.verify_sources(dist, source)


if __name__ == "__main__":
    unittest.main()
