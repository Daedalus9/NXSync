import contextlib
import hashlib
import importlib.util
import io
import json
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


source = load("source_release", "source_release.py")
scanner = load("public_tree", "check-public-tree.py")


class SourceReleaseTest(unittest.TestCase):
    def test_unreadable_text_and_restore_markers_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scanner.ROOT = root
            text = root / "notes.md"
            text.write_bytes(b"invalid encoding: \x97")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(scanner.main(), 1)
            text.unlink()
            for name in ("launch.restore-claim", "launch.restore-grant", "launch.restore-guard"):
                marker = root / name
                marker.write_text("profile_uid=private-runtime-identity\n")
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(scanner.main(), 1)
                marker.unlink()

    def test_dependency_source_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dependency = root / ".artifacts/dependency-sources/upstream/lib.tar.gz"
            dependency.parent.mkdir(parents=True)
            dependency.write_bytes(b"exact upstream source fixture")
            lock = {"files": [{"path": "upstream/lib.tar.gz", "sha256": hashlib.sha256(dependency.read_bytes()).hexdigest()}]}
            (root / "dependency-sources.lock.json").write_text(json.dumps(lock))
            (root / "LICENSE").write_text("source license fixture")
            (root / "source-inventory.json").write_text(json.dumps(["LICENSE", "dependency-sources.lock.json"]))
            path, digest = source.create_source_release(root, root / "source.tar.xz")
            self.assertEqual(digest, hashlib.sha256(path.read_bytes()).hexdigest())
            with tarfile.open(path) as archive:
                self.assertIn("NXSync/LICENSE", archive.getnames())
                self.assertIn("dependencies/upstream/lib.tar.gz", archive.getnames())
                self.assertIn("SHA256SUMS.txt", archive.getnames())
            dependency.write_bytes(b"wrong source")
            with self.assertRaisesRegex(RuntimeError, "corresponding"):
                source.create_source_release(root, root / "bad.tar.xz")

    def test_tracked_ignored_bytecode_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", "-b", "main", str(root)], check=True)
            (root / ".gitignore").write_text("__pycache__/\n*.pyc\n")
            (root / "script.py").write_text("print('public')\n")
            cache = root / "__pycache__/private.pyc"
            cache.parent.mkdir()
            cache.write_bytes(b"nonpublic bytecode")
            scanner.ROOT = root
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(scanner.main(), 0)
                subprocess.run(["git", "-C", str(root), "add", "-f", str(cache)], check=True)
                self.assertEqual(scanner.main(), 1)


if __name__ == "__main__":
    unittest.main()
