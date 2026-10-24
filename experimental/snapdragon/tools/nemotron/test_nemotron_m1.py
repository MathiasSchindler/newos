import argparse
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m1 import Graph, HEADER, NODE, TENSOR, F16, F32, U16, S16, DATA, CARRY, ZERO
from nemotron_m1 import header, fixture_write, name_bytes, tensor_record
from nemotron_m1_run import result_report


class FormatTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_header_fingerprints_and_shape(self):
        data = header(1, 16, 2, 1, 0, 2048, hashlib.sha256(b"test").digest())
        self.assertEqual(len(data), 256)
        self.assertEqual(struct.unpack_from("<4I", data, 48), (16, 2, 1, 0))
        self.assertEqual(struct.unpack_from("<Q", data, 64)[0], 2048)
        self.assertEqual(data[136:168], hashlib.sha256(b"test").digest())

    def test_name_and_tensor_bounds(self):
        for name in ("", "x" * 64, "a\0b", "\u00f6"):
            with self.subTest(name=name), self.assertRaises((ValueError, UnicodeError)):
                name_bytes(name)
        for shape in ([], [0], [-1], [True], [1] * 5):
            with self.subTest(shape=shape), self.assertRaises(ValueError):
                tensor_record({"name": "a", "type": 0, "dtype": F16, "id": 1, "shape": shape})

    def test_graph_serialization_and_evaluation(self):
        import numpy as np
        import torch

        graph = Graph("tiny", 32)
        a = graph.input("input", [2, 2])
        w = graph.constant("weight", np.asarray([[1, 2], [3, 4]], dtype=np.float32))
        b = graph.matmul("product", a, w)
        graph.output(b, "output")
        metadata = graph.write(self.root / "tiny.nmg")
        data = (self.root / "tiny.nmg").read_bytes()
        self.assertEqual(hashlib.sha256(data[HEADER:]).digest(), data[136:168])
        tensor_count, node_count = struct.unpack_from("<2I", data, 52)
        self.assertEqual((tensor_count, node_count), (3, 1))
        self.assertEqual(metadata["operators"], ["MatMul"])
        self.assertEqual(data[HEADER + 64 + 3 * TENSOR:HEADER + 64 + 3 * TENSOR + 64],
                         name_bytes("product"))
        expected = torch.tensor([[7.0, 10.0], [15.0, 22.0]])
        torch.testing.assert_close(graph.evaluate({"input": torch.tensor([[1.0, 2.0], [3.0, 4.0]])})["output"],
                                   expected, atol=0, rtol=0)

    def test_fixture_carry_and_float32_expectations(self):
        import numpy as np

        graph = {"io": [{"name": "in", "type": 0, "dtype": F16, "id": 1, "shape": [2]},
                        {"name": "out", "type": 1, "dtype": F16, "id": 2, "shape": [2]}]}
        steps = [
            {"flags": 1, "cells": {(0, "in"): np.asarray([1, 2]), (0, "out"): np.asarray([3, 4])}},
            {"flags": 2, "cells": {(0, "in"): (0, "out", 1), (0, "out"): np.asarray([5, 6])}},
        ]
        path = self.root / "case.nmf"
        fixture_write(path, [graph], steps, "case", 16)
        data = path.read_bytes()
        base = HEADER + 64 + 2 * TENSOR
        self.assertEqual(struct.unpack_from("<I", data, base + 16)[0], DATA)
        self.assertEqual(struct.unpack_from("<Q", data, base + 16 + 32 + 16)[0], 8)
        second = base + 16 + 2 * 32
        self.assertEqual(struct.unpack_from("<II", data, second + 16), (CARRY, 1))
        self.assertEqual(struct.unpack_from("<I", data, second + 16 + 24)[0], 1)

    def test_reject_invalid_carried_state(self):
        graph = {"io": [{"name": "in", "type": 0, "dtype": F16, "id": 1, "shape": [2]},
                        {"name": "out", "type": 1, "dtype": F16, "id": 2, "shape": [3]}]}
        with self.assertRaises(ValueError):
            fixture_write(self.root / "bad.nmf", [graph],
                          [{"flags": 3, "cells": {(0, "in"): (0, "out", 1)}}], "bad", 16)

    def test_qnn_depthwise_filter_layout(self):
        import torch

        torch.manual_seed(0)
        module = torch.nn.Conv2d(4, 4, 3, groups=4, bias=False)
        graph = Graph("depthwise", 32)
        source = graph.input("input", [1, 5, 5, 4])
        target = graph.conv("depthwise", source, module, True)
        graph.output(target, "output")
        weight = graph.tensors[graph.names["depthwise-weight"]]
        self.assertEqual(weight["shape"], [3, 3, 1, 4])
        data = torch.randn((1, 5, 5, 4))
        expected = module(data.permute(0, 3, 1, 2)).permute(0, 2, 3, 1)
        torch.testing.assert_close(graph.evaluate({"input": data})["output"], expected)

    def test_centered_norm_preserves_cpu_semantics(self):
        import torch

        graph = Graph("norm", 32, centered_norm=True)
        source = graph.input("input", [4, 8])
        module = torch.nn.LayerNorm(8)
        target = graph.norm("norm", source, module)
        graph.output(target, "output")
        data = torch.arange(32, dtype=torch.float32).reshape(4, 8)
        torch.testing.assert_close(graph.evaluate({"input": data})["output"], module(data),
                                   atol=1e-5, rtol=1e-5)

    def test_result_audit_and_checksum_rejection(self):
        payload = bytearray(name_bytes("case"))
        payload.extend(tensor_record({"name": "out", "type": 1, "dtype": F16, "id": 1, "shape": [1]}))
        for index in range(3):
            payload.extend(struct.pack("<IIQQQQfIQQ", index, 1 if index == 0 else 2 if index == 2 else 0,
                                       10, 20, 5, 40, 0.001, 0, 1, 0))
            payload.extend(struct.pack("<IIQQII", ZERO, 0, 0, 0, 0, 0))
        data = header(4, 16, 1, 0, 3, len(payload), hashlib.sha256(payload).digest())
        struct.pack_into("<4Q", data, 200, 1000, 100, 4096, 123)
        path = self.root / "result.nmr"
        path.write_bytes(data + payload)
        report = result_report(path)
        self.assertTrue(report["correctness_passed"])
        self.assertEqual(report["warm_p95_encoder_and_cache_ms"], 25)
        self.assertEqual(report["peak_working_set_bytes"], 4096)
        payload[-1] = 1
        path.write_bytes(data + payload)
        with self.assertRaises(ValueError):
            result_report(path)

    def test_quantized_per_axis_roundtrip_and_interpreter(self):
        import numpy as np
        import torch
        from nemotron_trace import load_graph

        graph = Graph("quant-matmul", 32)
        source = graph.input("input", [1, 2], U16)
        graph.tensors[source]["quantization"] = {"scale": 0.01, "offset": -1000}
        weight = graph.constant("weight", np.asarray([[1, 2], [3, 4]], dtype=np.float32))
        graph.tensors[weight]["dtype"] = S16
        graph.tensors[weight]["value"] = np.asarray([[10, 200], [30, 400]], dtype="<i2")
        graph.tensors[weight]["quantization"] = {"axis": 1, "scales": [0.1, 0.01]}
        result = graph.matmul("matmul", source, weight)
        graph.output(result, "output")
        graph.tensors[result]["dtype"] = U16
        graph.tensors[result]["quantization"] = {"scale": 0.02, "offset": -1000}
        path = self.root / "quant.nmg"
        graph.write(path)
        data = path.read_bytes()
        self.assertEqual(struct.unpack_from("<I", data, 8)[0], 2)
        record = HEADER + 64 + TENSOR
        self.assertEqual(struct.unpack_from("<3I", data, record + 116), (2, 1, 2))
        loaded = load_graph(path)
        actual = loaded.evaluate({"input": torch.tensor([[0.2, 0.3]])})["output"]
        torch.testing.assert_close(actual, torch.tensor([[1.1, 1.6]]), atol=1e-5, rtol=1e-5)

    def test_quantized_input_packing_and_carry_scale_rejection(self):
        import numpy as np

        graph = {"io": [{"name": "in", "type": 0, "dtype": U16, "id": 1, "shape": [2],
                         "quantization": {"scale": 0.1, "offset": -10}},
                        {"name": "out", "type": 1, "dtype": U16, "id": 2, "shape": [2],
                         "quantization": {"scale": 0.1, "offset": -10}}]}
        path = self.root / "quant.nmf"
        fixture_write(path, [graph], [{"flags": 3, "cells": {
            (0, "in"): np.asarray([-0.5, 0.5]), (0, "out"): np.asarray([-0.5, 0.5]),
        }}], "case", 32)
        data = path.read_bytes()
        base = HEADER + 64 + 2 * TENSOR + 16
        offset, size = struct.unpack_from("<QQ", data, base + 8)
        self.assertEqual(size, 4)
        self.assertEqual(struct.unpack_from("<2H", data, HEADER + offset), (5, 15))
        graph["io"][1]["quantization"]["scale"] = 0.2
        with self.assertRaises(ValueError):
            fixture_write(self.root / "bad.nmf", [graph],
                          [{"flags": 3, "cells": {(0, "in"): (0, "out", 1)}}], "bad", 32)

    def test_timing_only_is_never_correctness_acceptance(self):
        payload = bytearray(name_bytes("timing"))
        payload.extend(tensor_record({"name": "out", "type": 1, "dtype": F16, "id": 1, "shape": [1]}))
        payload.extend(struct.pack("<IIQQQQfIQQ", 0, 3, 1, 10, 2, 14, 0, 0, 0, 0))
        payload.extend(struct.pack("<IIQQII", ZERO, 0, 0, 0, 0, 0))
        data = header(4, 16, 1, 0, 1, len(payload), hashlib.sha256(payload).digest())
        struct.pack_into("<4Q", data, 200, 1000, 100, 4096, 0)
        path = self.root / "timing.nmr"
        path.write_bytes(data + payload)
        report = result_report(path)
        self.assertEqual(report["measurement"], "timing_only")
        self.assertFalse(report["correctness_passed"])

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--deps-dir", type=Path)
    args, remaining = parser.parse_known_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    unittest.main(argv=[sys.argv[0], *remaining])
