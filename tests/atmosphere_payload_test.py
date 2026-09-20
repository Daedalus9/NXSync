import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "atmosphere_payload", Path(__file__).parents[1] / "scripts/build-atmosphere-payload.py"
)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class AtmospherePayloadTest(unittest.TestCase):
    def test_exefs_order_and_contents_are_reproducible(self):
        nso = b"NSO0" + bytes(range(256))
        npdm = b"META" + b"permissions"
        outputs = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, order in enumerate(((".nso", ".npdm"), (".npdm", ".nso"))):
                directory = root / str(index)
                directory.mkdir()
                target = directory / "dmnt.nsp"
                for suffix in order:
                    target.with_suffix(suffix).write_bytes(nso if suffix == ".nso" else npdm)
                target.write_bytes(b"previous package must not influence the result")
                builder.build_dmnt_nsp(target)
                outputs.append(target.read_bytes())

        self.assertEqual(outputs[0], outputs[1])
        data = outputs[0]
        # Decode the archive independently to verify the loader-visible contents.
        self.assertEqual(data[:16], bytes.fromhex("50465330020000002000000000000000"))
        self.assertEqual(data[64:96], b"main\0main.npdm\0" + bytes(17))
        restored = {}
        for position in (16, 40):
            offset, size, name_offset, reserved = struct.unpack_from("<QQII", data, position)
            name = data[64 + name_offset:96].split(b"\0", 1)[0].decode("ascii")
            self.assertEqual(reserved, 0)
            restored[name] = data[96 + offset:96 + offset + size]
        self.assertEqual(list(restored), ["main", "main.npdm"])
        self.assertEqual(restored, {"main": nso, "main.npdm": npdm})
        self.assertEqual(len(data), 96 + len(nso) + len(npdm))

    def test_invalid_or_missing_inputs_do_not_replace_existing_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "dmnt.nsp"
            original = b"existing package"
            target.write_bytes(original)
            target.with_suffix(".nso").write_bytes(b"NSO0contents")
            with self.assertRaises(FileNotFoundError):
                builder.build_dmnt_nsp(target)
            self.assertEqual(target.read_bytes(), original)
            target.with_suffix(".npdm").write_bytes(b"corrupt metadata")
            with self.assertRaisesRegex(RuntimeError, "Invalid compiled dmnt input"):
                builder.build_dmnt_nsp(target)
            self.assertEqual(target.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
