#!/usr/bin/env python3
"""Focused tests for the TranslateGemma Stage 3 artifact converter."""

import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np


TOOLS = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "export_translategemma", TOOLS / "export-translategemma.py"
)
EXPORTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORTER)


def write_safetensors(path, tensors):
    header = {}
    payload = bytearray()
    for name, values in tensors.items():
        values = np.asarray(values, dtype=np.float32)
        bits = (values.view(np.uint32) >> np.uint32(16)).astype("<u2")
        start = len(payload)
        payload.extend(bits.tobytes())
        header[name] = {
            "dtype": "BF16", "shape": list(values.shape),
            "data_offsets": [start, len(payload)],
        }
    encoded = json.dumps(header, separators=(",", ":")).encode("utf-8")
    path.write_bytes(struct.pack("<Q", len(encoded)) + encoded + payload)


class Stage3Tests(unittest.TestCase):
    def test_bf16_conversion(self):
        expected = np.array([-7.0, -0.5, 0.0, 1.0, 3.5], dtype=np.float32)
        bits = (expected.view(np.uint32) >> np.uint32(16)).astype("<u2")
        np.testing.assert_array_equal(EXPORTER.bf16_to_f32(bits), expected)

    def test_s4_quantization_and_packing(self):
        values = np.resize(np.arange(-7, 8, dtype=np.float32), 128).reshape(1, 128)
        quantized, scales = EXPORTER.quantize_groups(values, 4)
        self.assertEqual(float(scales[0]), 1.0)
        np.testing.assert_array_equal(quantized, values.astype(np.int8))
        packed = EXPORTER.pack_s4(quantized)
        self.assertEqual(int(packed[0, 0]), 0xA9)
        unpacked = np.empty(128, dtype=np.int8)
        unpacked[0::2] = (packed[0] << np.uint8(4)).view(np.int8) >> 4
        unpacked[1::2] = packed[0].view(np.int8) >> 4
        np.testing.assert_array_equal(unpacked, quantized[0])

    def test_zero_group_has_finite_scale(self):
        quantized, scales = EXPORTER.quantize_groups(np.zeros((2, 128)), 4)
        self.assertTrue(np.all(quantized == 0))
        self.assertTrue(np.all(scales == np.float16(1.0)))

    def test_group_and_range_rejection(self):
        with self.assertRaises(EXPORTER.ExportError):
            EXPORTER.quantize_groups(np.zeros((128,)), 4)
        with self.assertRaises(EXPORTER.ExportError):
            EXPORTER.quantize_groups(np.full((1, 128), np.inf), 4)
        with self.assertRaises(EXPORTER.ExportError):
            EXPORTER.pack_s4(np.array([[-8, 0]], dtype=np.int8))

    def test_header_layout_and_corruption(self):
        name = "language_model.model.layers.0.mlp.up_proj.weight"
        spec = EXPORTER.artifact_spec(
            name, [1, 128], EXPORTER.KIND_TENSOR, EXPORTER.ELEMENT_S4,
            EXPORTER.QUANT_SYMMETRIC_GROUP, EXPORTER.LAYOUT_OUTPUT_INPUT_ROW_MAJOR,
            64, 1, 128, 1,
        )
        payload = bytes(range(66))
        header = EXPORTER.encode_header(spec, EXPORTER.sha256_bytes(payload))
        parsed = EXPORTER.decode_header(header)
        self.assertEqual(parsed["shape"], [1, 128])
        self.assertEqual(parsed["data_size"], 64)
        self.assertEqual(parsed["scale_offset"], 64)
        self.assertEqual(parsed["tensor_id"], EXPORTER.tensor_id(name))
        corrupted = bytearray(header)
        corrupted[44] = 1
        with self.assertRaises(EXPORTER.ExportError):
            EXPORTER.decode_header(corrupted)

    def test_streaming_w4_and_f16_artifacts(self):
        matrix_name = "language_model.test.weight"
        vector_name = "language_model.test_norm.weight"
        matrix = np.resize(np.arange(-7, 8, dtype=np.float32), 256).reshape(2, 128)
        vector = np.array([1.0, -0.5, 0.0, 3.5], dtype=np.float32)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shard_path = root / "model.safetensors"
            write_safetensors(shard_path, {matrix_name: matrix, vector_name: vector})
            shard = EXPORTER.SafeTensorShard(shard_path)
            matrix_source = shard.bf16(matrix_name, matrix.shape)
            matrix_path = root / "matrix.gta"
            spec, digest, metrics = EXPORTER.write_quantized_tensor(
                matrix_source, matrix_name, matrix.shape, 4, matrix_path
            )
            entry = EXPORTER.manifest_entry(matrix_path, root, spec, digest, metrics)
            parsed = EXPORTER.verify_artifact(matrix_path, entry)
            self.assertEqual(parsed["payload_size"], 132)
            self.assertEqual(metrics["rmse"], 0.0)
            EXPORTER.close_memmap(matrix_source)
            vector_source = shard.bf16(vector_name, vector.shape)
            vector_path = root / "vector.gta"
            spec, digest = EXPORTER.write_f16_tensor(
                vector_source, vector_name, vector.shape, vector_path
            )
            entry = EXPORTER.manifest_entry(vector_path, root, spec, digest)
            EXPORTER.verify_artifact(vector_path, entry)
            with vector_path.open("r+b") as stream:
                stream.seek(EXPORTER.HEADER_SIZE)
                first = stream.read(1)
                stream.seek(EXPORTER.HEADER_SIZE)
                stream.write(bytes([first[0] ^ 1]))
            with self.assertRaises(EXPORTER.ExportError):
                EXPORTER.verify_artifact(vector_path, entry)
            EXPORTER.close_memmap(vector_source)


if __name__ == "__main__":
    unittest.main()