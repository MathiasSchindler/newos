"""Development ctypes checks; native ARM64/corpus gates are separate."""
import argparse
import ctypes as ct
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, load_json, verify_file

LIBRARY = None
DATA = DEFAULT_DATA
SINK = ct.CFUNCTYPE(ct.c_int, ct.c_void_p, ct.POINTER(ct.c_float),
                   ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.c_uint64)
READ = ct.CFUNCTYPE(ct.c_int, ct.c_void_p, ct.c_uint64, ct.c_void_p, ct.c_uint32)


class Input(ct.Structure):
    _fields_ = [("context", ct.c_void_p), ("bytes", ct.c_uint64), ("read_at", READ)]


class Frontend:
    def __init__(self):
        import numpy as np

        reference = DATA / "reference-1"
        records = {item["name"]: item for item in load_json(reference / "manifest.json")["frontend_constants"]}
        for item in records.values():
            verify_file(reference / "frontend" / item["file"], item)
        self.window = np.pad(np.fromfile(reference / "frontend" / records["hann-window"]["file"], dtype="<f4"), (56, 56))
        self.mel = np.fromfile(reference / "frontend" / records["mel-filters"]["file"], dtype="<f4").reshape(128, 257)
        self.state = ct.create_string_buffer(LIBRARY.NmFrontendSize())
        self.chunks = []
        self.ends = 0

        def consume(context, features, width, valid, flags, index):
            if flags == 4:
                self.ends += 1
                return int(width == valid == 0 and index == len(self.chunks))
            values = np.ctypeslib.as_array(features, shape=(width * 128,)).copy().reshape(width, 128)
            self.chunks.append((values, valid, flags))
            return int(index == len(self.chunks) - 1)

        self.sink = SINK(consume)
        if not LIBRARY.nm_frontend_init(self.state, self.window.ctypes.data, self.mel.ctypes.data,
                                        self.sink, None):
            raise ValueError(self.error())

    def error(self):
        return LIBRARY.NmFrontendError(self.state).decode()

    def feed(self, values):
        import numpy as np

        values = np.ascontiguousarray(values, dtype="<f4")
        if not LIBRARY.nm_frontend_feed(self.state, values.ctypes.data, values.size):
            raise ValueError(self.error())

    def finish(self):
        if not LIBRARY.nm_frontend_finish(self.state):
            raise ValueError(self.error())

    def reset(self):
        if not LIBRARY.nm_frontend_reset(self.state):
            raise ValueError(self.error())
        self.chunks.clear()
        self.ends = 0

    def info(self):
        result = (ct.c_uint64 * 5)()
        LIBRARY.NmFrontendInfo(self.state, result)
        return list(result)


class NativeFrontendTests(unittest.TestCase):
    def test_runtime_package_validation_and_mutations(self):
        original = (DATA / "nemotron-runtime-m2.nmp").read_bytes()
        artifact = ct.create_string_buffer(LIBRARY.NmArtifactSize())

        def validate(raw):
            value = ct.create_string_buffer(bytes(raw))
            return LIBRARY.nm_validate(artifact, value, len(raw))

        self.assertEqual(validate(original), 1)
        mutations = []
        for offset, value in ((8, 9), (36, 79), (200, 1), (256 + 80, 1024),
                              (256 + 96, 26), (256 + 124, 15), (256 + 248, 1),
                              (256 + 512 + 256, 99), (256 + 512 + 260, 1)):
            raw = bytearray(original)
            struct.pack_into("<I", raw, offset, value)
            mutations.append(raw)
        raw = bytearray(original)
        struct.pack_into("<Q", raw, 256 + 128, 16)
        mutations.append(raw)
        for name in (b"..\\checkpoint\\config.json", b"\\absolute", b"\xff", b"checkpoint\\name."):
            raw = bytearray(original)
            raw[768:1024] = bytes(256)
            raw[768:768 + len(name)] = name
            mutations.append(raw)
        raw = bytearray(original)
        raw[256 + 512 + 272:256 + 512 + 304] = bytes(32)
        mutations.append(raw)
        raw = bytearray(original)
        mel = 256 + struct.unpack_from("<Q", raw, 256 + 136)[0]
        struct.pack_into("<I", raw, mel, 0x7fc00000)
        raw[256 + 176:256 + 208] = hashlib.sha256(raw[mel:]).digest()
        mutations.append(raw)
        for raw in mutations:
            raw[136:168] = hashlib.sha256(raw[256:]).digest()
            self.assertEqual(validate(raw), 0)
            self.assertTrue(LIBRARY.NmArtifactError(artifact))
        self.assertEqual(validate(original[:-1]), 0)
        self.assertEqual(validate(original + b"\0"), 0)
        self.assertEqual(validate(original[:128]), 0)

    def test_reader_allocation_failure(self):
        from nemotron_m1 import Graph

        graph = Graph("allocation-test", 32)
        source = graph.input("input", [1])
        graph.output(graph.op("identity", "Identity", [source], [1]), "output")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "identity.nmg"
            graph.write(path)
            raw = path.read_bytes()
        data = ct.create_string_buffer(raw)
        artifact = ct.create_string_buffer(LIBRARY.NmArtifactSize())
        self.assertEqual(LIBRARY.nm_validate(artifact, data, len(raw)), 1)
        LIBRARY.NmFailAllocation(1)
        try:
            self.assertEqual(LIBRARY.nm_validate(artifact, data, len(raw)), 0)
            self.assertIn(b"allocation", LIBRARY.NmArtifactError(artifact))
        finally:
            LIBRARY.NmFailAllocation(0)

    def test_operator_metadata_matches_graph_before_restore(self):
        from nemotron_m1 import Graph
        from nemotron_fp32 import convert

        graph = Graph("joiner-contract-test", 32)
        encoder = graph.input("encoder", [1, 640])
        prediction = graph.input("prediction", [1, 640])
        combined = graph.binary("combined", "ElementWiseAdd", encoder, prediction)
        graph.output(graph.pad("logits-pad", combined, [[0, 0], [0, 12448]]), "logits")
        convert(graph)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "graph.nmg"
            graph.write(path)
            raw = bytearray(path.read_bytes())
        artifact = ct.create_string_buffer(LIBRARY.NmArtifactSize())
        buffer = ct.create_string_buffer(bytes(raw))
        self.assertEqual(LIBRARY.nm_validate(artifact, buffer, len(raw)), 1)
        self.assertEqual(LIBRARY.nm_runtime_graph_contract(artifact, 10), 1)
        tensors = struct.unpack_from("<I", raw, 52)[0]
        first_node = 256 + 64 + tensors * 160
        metadata_index = struct.unpack_from("<I", raw, first_node + 144)[0]
        metadata = 256 + struct.unpack_from("<Q", raw, 256 + 64 + metadata_index * 160 + 96)[0]
        struct.pack_into("<I", raw, metadata + 16, 4)
        raw[136:168] = hashlib.sha256(raw[256:]).digest()
        buffer = ct.create_string_buffer(bytes(raw))
        self.assertEqual(LIBRARY.nm_validate(artifact, buffer, len(raw)), 1)
        self.assertEqual(LIBRARY.nm_runtime_graph_contract(artifact, 10), 0)
        self.assertTrue(LIBRARY.NmArtifactError(artifact))

    def test_partition_parity_and_chunk_boundary(self):
        import numpy as np
        import torch
        from transformers import NemotronAsrStreamingFeatureExtractor
        from nemotron_reference import features_for_audio

        torch.set_num_threads(1)
        extractor = NemotronAsrStreamingFeatureExtractor(feature_size=128)
        for length in (160, 173, 4000, 4096, 5120, 10240, 10413):
            samples = (np.sin(np.arange(length, dtype=np.float32) * np.float32(.03)) * np.float32(.2))
            expected, _ = features_for_audio(samples, extractor, torch, [5120])
            previous = None
            for partition in ([5120], [1, 159, 256, 4096]):
                frontend = Frontend()
                offset = index = 0
                while offset < length:
                    end = min(length, offset + partition[index % len(partition)])
                    frontend.feed(samples[offset:end])
                    self.assertLessEqual(frontend.info()[3], 511)
                    offset = end; index += 1
                if length == 4096:
                    self.assertEqual(len(frontend.chunks), 1)
                    self.assertEqual(frontend.chunks[0][1], 25)
                frontend.finish()
                actual = np.concatenate([values[:valid] for values, valid, _ in frontend.chunks])
                np.testing.assert_allclose(actual, expected.numpy()[0], atol=2e-5, rtol=2e-5)
                if previous is not None:
                    np.testing.assert_array_equal(actual, previous)
                previous = actual
                self.assertEqual(frontend.chunks[0][0].shape, (25, 128))
                self.assertEqual(frontend.info()[1], length // 160)
                self.assertEqual(frontend.ends, 1)
                for values, valid, _ in frontend.chunks:
                    self.assertTrue(np.all(values[valid:] == 0))
                frontend.reset()
                frontend.feed(samples)
                frontend.finish()
                reset = np.concatenate([values[:valid] for values, valid, _ in frontend.chunks])
                np.testing.assert_array_equal(reset, actual)

    def test_errors_and_silence(self):
        import numpy as np

        frontend = Frontend()
        frontend.feed(np.zeros(4000, dtype=np.float32))
        frontend.finish()
        self.assertTrue(np.isfinite(frontend.chunks[0][0]).all())
        with self.assertRaisesRegex(ValueError, "closed"):
            frontend.feed([0])
        with self.assertRaisesRegex(ValueError, "closed"):
            frontend.finish()
        frontend.reset()
        with self.assertRaisesRegex(ValueError, "finite normalized"):
            frontend.feed([np.nan])
        frontend.reset()
        frontend.feed([0] * 159)
        with self.assertRaisesRegex(ValueError, "shorter"):
            frontend.finish()

    def test_wav_bounds_formats_and_reset(self):
        import numpy as np

        raw = struct.pack("<3h", -32768, 0, 32767)
        body = b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
        body += b"JUNK" + struct.pack("<I", 1) + b"x\0data" + struct.pack("<I", len(raw)) + raw
        good = b"RIFF" + struct.pack("<I", len(body)) + body
        variants = [good[:11], good[:-1], good + b"x"]
        for offset, value in ((20, 3), (22, 2), (24, 8000), (28, 123), (32, 4), (34, 32)):
            bad = bytearray(good)
            struct.pack_into("<I" if offset in (24, 28) else "<H", bad, offset, value)
            variants.append(bytes(bad))
        for data in [good, *variants]:
            def read(context, offset, target, count):
                if offset + count > len(data):
                    return 0
                ct.memmove(target, data[offset:offset + count], count)
                return 1
            callback = READ(read)
            source = Input(None, len(data), callback)
            wav = ct.create_string_buffer(LIBRARY.NmWavSize())
            result = LIBRARY.nm_wav_open(wav, ct.byref(source))
            if data != good:
                self.assertEqual(result, 0)
                self.assertTrue(LIBRARY.NmWavError(wav))
                continue
            self.assertEqual(result, 1)
            samples = np.zeros(7, dtype="<f4")
            count = ct.c_uint32()
            self.assertEqual(LIBRARY.nm_wav_read(wav, samples.ctypes.data, 7, ct.byref(count)), 1)
            self.assertEqual(count.value, 3)
            np.testing.assert_array_equal(samples[:3], [-1, 0, 32767 / 32768])
            self.assertEqual(LIBRARY.nm_wav_reset(wav), 1)
            self.assertEqual(LIBRARY.nm_wav_read(wav, samples.ctypes.data, 7, ct.byref(count)), 1)
            self.assertEqual(count.value, 3)


def configure(path):
    library = ct.CDLL(str(Path(path).resolve()))
    library.NmFrontendSize.restype = library.NmWavSize.restype = ct.c_size_t
    library.NmFrontendError.argtypes = library.NmWavError.argtypes = [ct.c_void_p]
    library.NmFrontendError.restype = library.NmWavError.restype = ct.c_char_p
    library.NmFrontendInfo.argtypes = [ct.c_void_p, ct.POINTER(ct.c_uint64)]
    library.nm_frontend_init.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_void_p, SINK, ct.c_void_p]
    library.nm_frontend_feed.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_size_t]
    library.nm_frontend_finish.argtypes = library.nm_frontend_reset.argtypes = [ct.c_void_p]
    library.nm_wav_open.argtypes = [ct.c_void_p, ct.POINTER(Input)]
    library.nm_wav_read.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_uint32, ct.POINTER(ct.c_uint32)]
    library.nm_wav_reset.argtypes = [ct.c_void_p]
    library.NmArtifactSize.restype = ct.c_size_t
    library.NmArtifactError.argtypes = [ct.c_void_p]
    library.NmArtifactError.restype = ct.c_char_p
    library.NmFailAllocation.argtypes = [ct.c_int]
    library.nm_validate.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_uint64]
    library.nm_runtime_graph_contract.argtypes = [ct.c_void_p, ct.c_uint32]
    return library


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--deps-dir", type=Path)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    args, remaining = parser.parse_known_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    DATA = args.data_dir
    LIBRARY = configure(args.library)
    unittest.main(argv=[sys.argv[0], *remaining])
