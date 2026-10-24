import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_inventory import pe_imports
from nemotron_m0 import digest, read_catalog, read_wav, safe_name, verify_file, write_json, write_pcm16
from nemotron_reference import normalized_words, validate_tensor_record, word_errors


class InputTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_committed_catalog(self):
        catalog = read_catalog()
        self.assertEqual(catalog["profile"]["att_context_size"], [56, 3])
        self.assertEqual(catalog["profile"]["prompt_id"], 9)
        self.assertEqual(catalog["reference"]["attention_backend"], "sdpa")

    def test_unsafe_names(self):
        for name in ("", "..", "../x", r"..\x", "C:x", "x.", "x ", "NUL.bin", "con", "LPT1", None):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_name(name)

    def test_hash_and_size(self):
        path = self.root / "fixture"
        path.write_bytes(b"abc")
        entry = {"size": 3, "sha256": hashlib.sha256(b"abc").hexdigest(),
                 "git_blob_sha1": hashlib.sha1(b"blob 3\0abc").hexdigest()}
        self.assertEqual(verify_file(path, entry), entry["sha256"])
        for field, value in (("size", 4), ("sha256", "0" * 64), ("git_blob_sha1", "0" * 40)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                verify_file(path, {**entry, field: value})

    def test_json_atomic_and_no_nan(self):
        path = self.root / "manifest.json"
        write_json(path, {"value": 1})
        self.assertEqual(json.loads(path.read_text()), {"value": 1})
        self.assertFalse(path.with_name(path.name + ".partial").exists())
        with self.assertRaises(ValueError):
            write_json(path, {"value": float("nan")})
        self.assertEqual(json.loads(path.read_text()), {"value": 1})

    def test_pcm16_roundtrip_and_boundaries(self):
        path = self.root / "audio.wav"
        values = [-1, -0.5, 0, 0.5, 1]
        write_pcm16(path, values)
        self.assertEqual(read_wav(path), [-1, -0.5, 0, 0.5, 32767 / 32768])

    def test_invalid_waveforms(self):
        path = self.root / "audio.wav"
        for values in ([float("nan")], [float("inf")], [1.1]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                write_pcm16(path, values)
        write_pcm16(path, [])
        with self.assertRaises(ValueError):
            read_wav(path)

    def test_truncated_and_unsupported_wav(self):
        path = self.root / "audio.wav"
        write_pcm16(path, [0.5] * 10)
        original = path.read_bytes()
        variants = [original[:-1], original + b"x"]
        for offset, value in ((20, 2), (22, 2), (24, 8000), (28, 1), (32, 4), (34, 32)):
            changed = bytearray(original)
            struct.pack_into("<H" if offset in (20, 22, 32, 34) else "<I", changed, offset, value)
            variants.append(changed)
        for variant in variants:
            path.write_bytes(variant)
            with self.subTest(variant=variant[:44]), self.assertRaises(ValueError):
                read_wav(path)

    def test_float_wav_and_nonfinite(self):
        path = self.root / "audio.wav"
        write_pcm16(path, [0] * 2)
        header = bytearray(path.read_bytes()[:44])
        struct.pack_into("<I", header, 4, 44)
        struct.pack_into("<H", header, 20, 3)
        struct.pack_into("<I", header, 28, 64000)
        struct.pack_into("<HH", header, 32, 4, 32)
        struct.pack_into("<I", header, 40, 8)
        path.write_bytes(header + struct.pack("<ff", 0.5, -0.25))
        self.assertEqual(read_wav(path), [0.5, -0.25])
        path.write_bytes(header + struct.pack("<ff", float("nan"), 0))
        with self.assertRaises(ValueError):
            read_wav(path)

    def test_wer_contract(self):
        self.assertEqual(normalized_words("GUTEN, Tag!"), ["guten", "tag"])
        self.assertEqual(normalized_words("O\u0308l"), ["\u00f6l"])
        self.assertEqual(word_errors("eins zwei drei", "eins vier"), {"errors": 2, "reference_words": 3})
        self.assertEqual(word_errors("", "eins"), {"errors": 1, "reference_words": 0})

    def test_tensor_shape_contract(self):
        record = {"dtype": "<f4", "shape": [1, 25, 128], "size": 12800, "file": "features.bin"}
        validate_tensor_record(record)
        for field, value in (("dtype", "object"), ("size", 12801), ("shape", [1, -1]),
                             ("shape", [True]), ("file", "../features.bin")):
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_tensor_record({**record, field: value})


def fake_pe(delay=False):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 240)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<I", data, 0x98 + 108, 16)
    slot = 13 if delay else 1
    struct.pack_into("<II", data, 0x98 + 112 + slot * 8, 0x1000, 64 if delay else 40)
    struct.pack_into("<IIII", data, 0x188 + 8, 512, 0x1000, 512, 512)
    if delay:
        struct.pack_into("<II", data, 512, 1, 0x1080)
    else:
        struct.pack_into("<I", data, 512 + 12, 0x1080)
    data[640:653] = b"KERNEL32.dll\0"
    return data


class ImportTests(unittest.TestCase):
    def test_static_import(self):
        self.assertEqual(pe_imports(fake_pe()), [{"name": "KERNEL32.dll", "kind": "static"}])

    def test_delay_import(self):
        self.assertEqual(pe_imports(fake_pe(True)), [{"name": "KERNEL32.dll", "kind": "delay"}])

    def test_reject_bad_headers_and_ranges(self):
        for data in (b"", b"MZ", fake_pe()[:700]):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                pe_imports(data)
        data = fake_pe()
        struct.pack_into("<I", data, 524, 0x3000)
        with self.assertRaises(ValueError):
            pe_imports(data)

    def test_reject_unterminated_import_directory(self):
        data = fake_pe()
        struct.pack_into("<I", data, 0x98 + 112 + 8 + 4, 20)
        with self.assertRaises(ValueError):
            pe_imports(data)


if __name__ == "__main__":
    unittest.main()
