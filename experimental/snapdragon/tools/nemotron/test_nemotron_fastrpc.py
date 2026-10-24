import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_fastrpc import package_environment
from nemotron_m0 import digest


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("module.so", "module.cat", "driver.dll", "host.exe"):
            (self.root / name).write_bytes(name.encode("ascii"))
        self.manifest = {
            "schema_version": 1, "backend": "OEMFastRPC-HVX-FP32",
            "dsp": {"file": "module.so", "sha256": digest(self.root / "module.so"),
                    "catalog_file": "module.cat", "catalog_sha256": digest(self.root / "module.cat")},
            "driver": {"file": "driver.dll", "sha256": digest(self.root / "driver.dll")},
            "host": {"file": "host.exe", "sha256": digest(self.root / "host.exe")},
        }
        self.path = self.root / "manifest.json"

    def write(self):
        self.path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_verified_paths_and_process_local_environment(self):
        self.write()
        original = os.environ.copy()
        manifest, env = package_environment(self.path)
        self.assertEqual(manifest, self.manifest)
        self.assertEqual(env["NM_FP32_DSP_PACKAGE"], str((self.root / "module.so").resolve()))
        self.assertEqual(env["NM_FP32_RPC_DRIVER"], str((self.root / "driver.dll").resolve()))
        self.assertEqual(env["NM_FP32_RPC_SHA256"], self.manifest["driver"]["sha256"])
        self.assertEqual(os.environ.copy(), original)

    def test_module_catalog_and_driver_corruption(self):
        for name in ("module.so", "module.cat", "driver.dll", "host.exe"):
            self.write()
            path = self.root / name
            original = path.read_bytes()
            path.write_bytes(original + b"changed")
            with self.subTest(file=name), self.assertRaisesRegex(ValueError, "identity mismatch"):
                package_environment(self.path)
            path.write_bytes(original)

    def test_wrong_backend_is_rejected(self):
        self.manifest["backend"] = "host-CPU"
        self.write()
        with self.assertRaises(ValueError):
            package_environment(self.path)

    def test_missing_package_is_explicit_failure(self):
        self.write()
        (self.root / "module.so").unlink()
        with self.assertRaises(FileNotFoundError):
            package_environment(self.path)


if __name__ == "__main__":
    unittest.main()
