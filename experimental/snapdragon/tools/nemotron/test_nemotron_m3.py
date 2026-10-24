"""Native decoder-table tests against the pinned tokenizer; no model fallback."""
import argparse
import ctypes as ct
from pathlib import Path
import random
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, load_json
from nemotron_m3 import tokenizer_bytes, read_events

LIBRARY = None
DATA = DEFAULT_DATA


class TokenizerTests(unittest.TestCase):
    def test_all_ids_edges_random_and_m0_sequences(self):
        from tokenizers import Tokenizer

        source = DATA / "checkpoint" / "tokenizer.json"
        oracle = Tokenizer.from_file(str(source))
        raw = tokenizer_bytes(source)
        buffer = ct.create_string_buffer(raw)
        state = ct.create_string_buffer(LIBRARY.NmTokenizerSize())
        self.assertEqual(LIBRARY.nm_tokenizer_open(state, buffer, len(raw)), 1)
        output = ct.create_string_buffer(4096)
        size = ct.c_size_t()
        sequences = [[index] for index in range(13089)]
        sequences += [[], [2, 2], [518, 2, 179], [2, 518, 2], [0, 179], [4, 6], [13087, 13088, 2]]
        random_ = random.Random(13089)
        sequences += [[random_.randrange(13089) for _ in range(random_.randrange(50))] for _ in range(1000)]
        for fixture in load_json(DATA / "reference-1" / "manifest.json")["fixtures"]:
            sequences.append(fixture["tokens"])
        for tokens in sequences:
            ids = (ct.c_uint32 * len(tokens))(*tokens)
            for skip in (0, 1):
                self.assertEqual(LIBRARY.nm_tokenizer_decode(state, ids, len(tokens), skip,
                                                           output, len(output), ct.byref(size)), 1)
                actual = bytes(output[:size.value]).decode("utf-8")
                self.assertEqual(actual, oracle.decode(tokens, skip_special_tokens=bool(skip)), tokens[:20])

    def test_invalid_tables_ids_and_output_bounds(self):
        raw = tokenizer_bytes(DATA / "checkpoint" / "tokenizer.json")
        state = ct.create_string_buffer(LIBRARY.NmTokenizerSize())
        for value in (raw[:63], raw[:-1], raw + b"x"):
            buffer = ct.create_string_buffer(value)
            self.assertEqual(LIBRARY.nm_tokenizer_open(state, buffer, len(value)), 0)
            self.assertTrue(LIBRARY.NmTokenizerError(state))
        broken = bytearray(raw)
        struct.pack_into("<I", broken, 64, 9999999)
        buffer = ct.create_string_buffer(bytes(broken))
        self.assertEqual(LIBRARY.nm_tokenizer_open(state, buffer, len(broken)), 0)
        output = ct.create_string_buffer(8)
        size = ct.c_size_t()
        ids = (ct.c_uint32 * 1)(0)
        self.assertEqual(LIBRARY.nm_tokenizer_decode(state, ids, 1, 1, output, len(output), ct.byref(size)), 0)
        buffer = ct.create_string_buffer(raw)
        self.assertEqual(LIBRARY.nm_tokenizer_open(state, buffer, len(raw)), 1)
        output = ct.create_string_buffer(1)
        size = ct.c_size_t()
        ids = (ct.c_uint32 * 1)(13089)
        self.assertEqual(LIBRARY.nm_tokenizer_decode(state, ids, 1, 1, output, 1, ct.byref(size)), 0)
        ids[0] = 179
        self.assertEqual(LIBRARY.nm_tokenizer_decode(state, ids, 1, 1, output, 1, ct.byref(size)), 0)
        self.assertTrue(LIBRARY.NmTokenizerError(state))

    def test_m3_embedded_table_reader_and_checksum_errors(self):
        original = (DATA / "nemotron-runtime-m3.nmp").read_bytes()
        artifact = ct.create_string_buffer(LIBRARY.NmArtifactSize())
        buffer = ct.create_string_buffer(original)
        self.assertEqual(LIBRARY.nm_validate(artifact, buffer, len(original)), 1)
        broken = bytearray(original)
        struct.pack_into("<Q", broken, 256 + 480, 0xffffffffffffffff)
        import hashlib
        broken[136:168] = hashlib.sha256(broken[256:]).digest()
        buffer = ct.create_string_buffer(bytes(broken))
        self.assertEqual(LIBRARY.nm_validate(artifact, buffer, len(broken)), 0)
        self.assertTrue(LIBRARY.NmArtifactError(artifact))


def configure(path):
    from test_nemotron_m2 import configure as frontend_configure

    library = frontend_configure(path)
    library.NmTokenizerSize.restype = ct.c_size_t
    library.NmTokenizerError.argtypes = [ct.c_void_p]
    library.NmTokenizerError.restype = ct.c_char_p
    library.nm_tokenizer_open.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_uint64]
    library.nm_tokenizer_decode.argtypes = [ct.c_void_p, ct.POINTER(ct.c_uint32), ct.c_size_t, ct.c_int,
                                          ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_size_t)]
    return library


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
