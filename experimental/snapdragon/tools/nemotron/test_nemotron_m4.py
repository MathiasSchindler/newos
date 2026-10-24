"""M4 exact packing/schema tests; hardware numerical and speed gates are separate."""
import argparse
import ctypes as ct
import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA
from test_nemotron_m2 import configure

DATA = DEFAULT_DATA
LIBRARY = None


def graph_parts(path):
    import numpy as np

    raw = Path(path).read_bytes()
    tensors, nodes = struct.unpack_from("<2I", raw, 52)
    records, values = [], {}
    for index in range(tensors):
        offset = 256 + 64 + index * 160
        name = raw[offset:offset + 64].split(b"\0", 1)[0].decode()
        rank = struct.unpack_from("<I", raw, offset + 72)[0]
        shape = struct.unpack_from("<4I", raw, offset + 80)[:rank]
        type_, dtype = struct.unpack_from("<2I", raw, offset + 64)
        blob, size = struct.unpack_from("<2Q", raw, offset + 96)
        records.append({"name": name, "offset": offset, "blob": blob, "shape": shape, "dtype": dtype})
        if type_ == 4 and dtype == 0x0232:
            values[name] = np.frombuffer(raw, dtype="<f4", count=size // 4, offset=256 + blob).reshape(shape).copy()
    packed = set()
    for index in range(nodes):
        offset = 256 + 64 + tensors * 160 + index * 672
        meta_index = struct.unpack_from("<I", raw, offset + 144)[0]
        metadata = 256 + records[meta_index]["blob"]
        if struct.unpack_from("<I", raw, metadata + 4)[0] == 2:
            weight_index = struct.unpack_from("<I", raw, offset + 152)[0]
            packed.add(records[weight_index]["name"])
    return raw, records, values, packed


class M4Tests(unittest.TestCase):
    def test_packed_decoders_retain_every_coefficient_bit(self):
        import numpy as np

        for name in ("prediction", "joiner"):
            _, _, expected, _ = graph_parts(DATA / "m1" / "fastrpc-fp32-decoder-graphs" / (name + ".nmg"))
            _, _, actual, packed = graph_parts(DATA / "m4" / "packed-decoder-graphs" / (name + "-packed.nmg"))
            self.assertTrue(packed)
            for key, reference in expected.items():
                value = actual[key]
                if key in packed:
                    k, n = reference.shape[-2:]
                    tiles = value.reshape(*reference.shape[:-2], n // 32, k, 32)
                    permutation = list(range(reference.ndim - 2)) + [reference.ndim - 1, reference.ndim - 2, reference.ndim]
                    value = tiles.transpose(permutation).copy().reshape(reference.shape)
                np.testing.assert_array_equal(value.view(np.uint32), reference.view(np.uint32), key)

    def test_fused_graph_and_schema4_validate(self):
        raw = (DATA / "nemotron-runtime-m4.nmp").read_bytes()
        buffer = ct.create_string_buffer(raw)
        artifact = ct.create_string_buffer(LIBRARY.NmArtifactSize())
        self.assertEqual(LIBRARY.nm_validate(artifact, buffer, len(raw)), 1)
        changed = bytearray(raw)
        struct.pack_into("<I", changed, 256 + 496, 2)
        changed[136:168] = hashlib.sha256(changed[256:]).digest()
        buffer = ct.create_string_buffer(bytes(changed))
        self.assertEqual(LIBRARY.nm_validate(artifact, buffer, len(changed)), 0)
        graph = DATA / "m4" / "packed-decoder-graphs" / "prediction-joiner-packed.nmg"
        raw, descriptors, _, _ = graph_parts(graph)
        buffer = ct.create_string_buffer(raw)
        self.assertEqual(LIBRARY.nm_validate(artifact, buffer, len(raw)), 1)
        self.assertEqual(LIBRARY.nm_runtime_graph_contract(artifact, 9), 1)
        descriptor = next(item for item in descriptors if item["name"] == "encoder")
        changed = bytearray(raw)
        struct.pack_into("<I", changed, descriptor["offset"] + 84, 1)
        changed[136:168] = hashlib.sha256(changed[256:]).digest()
        buffer = ct.create_string_buffer(bytes(changed))
        self.assertFalse(LIBRARY.nm_validate(artifact, buffer, len(changed)) and
                         LIBRARY.nm_runtime_graph_contract(artifact, 9))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    args, remaining = parser.parse_known_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    DATA = args.data_dir
    LIBRARY = configure(args.library)
    unittest.main(argv=[sys.argv[0], *remaining])
