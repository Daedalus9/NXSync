import hashlib
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("installer_romfs", Path(__file__).parents[1] / "scripts/installer_romfs.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
NONE = 0xFFFFFFFF


def romfs(name=b"payload", contents=b"current", metadata_after_data=False):
    directory = struct.pack("<6I", 0, NONE, NONE, 0, NONE, 0)
    file = struct.pack("<IIQQII", 0, NONE, 0, len(contents), NONE, len(name)) + name
    if metadata_after_data:
        dirs = 80 + len(contents)
        files = dirs + len(directory)
        return struct.pack("<10Q", 80, dirs, 0, dirs, len(directory), files, 0, files, len(file), 80) + contents + directory + file
    payload = 80 + len(directory) + len(file)
    return struct.pack("<10Q", 80, 80, 0, 80, len(directory), 104, 0, 104, len(file), payload) + directory + file + contents


class RomfsTest(unittest.TestCase):
    def test_metadata_after_payload(self):
        self.assertEqual(module.romfs_hashes(romfs(metadata_after_data=True)),
                         module.romfs_hashes(romfs()))
        image = bytearray(romfs(metadata_after_data=True))
        file_offset = struct.unpack_from("<Q", image, 56)[0]
        struct.pack_into("<Q", image, file_offset + 8, 7)  # file now points into directory metadata
        with self.assertRaisesRegex(ValueError, "overlaps metadata"):
            module.romfs_hashes(image)

    def test_content_and_stale_installer(self):
        image = romfs()
        self.assertEqual(module.romfs_hashes(image), {"payload": hashlib.sha256(b"current").hexdigest()})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "payload").write_bytes(b"current")
            module.verify_staged_romfs(image, root)
            (root / "payload").write_bytes(b"next")
            with self.assertRaisesRegex(ValueError, "stale"):
                module.verify_staged_romfs(image, root)

    def test_corrupt_metadata(self):
        for image in (romfs()[:-1], romfs(b"../escape"), b"invalid"):
            with self.assertRaises(ValueError):
                module.romfs_hashes(image)
        image = bytearray(romfs())
        struct.pack_into("<I", image, 108, 0)  # circular file sibling
        with self.assertRaisesRegex(ValueError, "cyclic"):
            module.romfs_hashes(image)


if __name__ == "__main__":
    unittest.main()
