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
    def test_numerical_reference_mapping(self):
        spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        reference.self_test()
        name = "language_model.model.layers.0.self_attn.q_proj.weight"
        values = np.arange(512, dtype=np.float32).reshape(4, 128) / 128
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_safetensors(root / "model.safetensors", {name: values})
            (root / "model.safetensors.index.json").write_text(json.dumps({
                "weight_map": {name: "model.safetensors"}
            }), encoding="utf-8")
            weights = reference.Weights(EXPORTER, root, root, "bf16")
            first = weights.rows("layers.0.self_attn.q_proj.weight", 1, 3)
            second = weights.rows("layers.0.self_attn.q_proj.weight", 1, 3)
            np.testing.assert_array_equal(first, second)
            expected = EXPORTER.bf16_to_f32((values.view(np.uint32) >> 16).astype(np.uint16))
            np.testing.assert_array_equal(first, expected[1:3])
            self.assertEqual(len(weights.mappings), 1)
            weights.close()
            self.assertEqual(len(weights.mappings), 0)

    def test_bf16_conversion(self):
        expected = np.array([-7.0, -0.5, 0.0, 1.0, 3.5], dtype=np.float32)
        bits = (expected.view(np.uint32) >> np.uint32(16)).astype("<u2")
        np.testing.assert_array_equal(EXPORTER.bf16_to_f32(bits), expected)

    def test_numerical_manifest_shape_rejection(self):
        spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scalar, _ = reference.scalar_fixtures(EXPORTER, root / "numeric-scalars.gta")
            recorder = reference.Recorder(EXPORTER, root)
            recorder.array("shape-test", np.array([1.0], dtype=np.float32))
            manifest = {"schema_version": 1, "model": {"repository": EXPORTER.REPOSITORY,
                "revision": EXPORTER.REVISION}, "complete": False, "artifacts": [scalar, *recorder.entries]}
            path = root / "manifest.json"
            path.write_bytes(EXPORTER.canonical_json(manifest))
            reference.verify_reference(EXPORTER, root, require_complete=False)
            with self.assertRaises(ValueError):
                reference.verify_reference(EXPORTER, root)
            recorder.entries[0]["array_shape"] = [2 ** 64, 2 ** 64]
            path.write_bytes(EXPORTER.canonical_json(manifest))
            with self.assertRaises(ValueError):
                reference.verify_reference(EXPORTER, root, require_complete=False)

    def test_numerical_quantized_mapping_and_overflow(self):
        spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        name = "language_model.model.layers.0.self_attn.q_proj.weight"
        values = np.resize(np.arange(-7, 8, dtype=np.float32), 512).reshape(4, 128)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "model.safetensors.index.json").write_text(json.dumps({"weight_map": {}}), encoding="utf-8")
            for bits in (4, 8):
                variant = f"w{bits}a16"
                directory = root / variant
                path = directory / "weights.gta"
                source_bits = (values.view(np.uint32) >> 16).astype("<u2")
                spec, digest, metrics = EXPORTER.write_quantized_tensor(source_bits, name, list(values.shape), bits, path)
                entry = EXPORTER.manifest_entry(path, directory, spec, digest, metrics)
                (root / "manifest.json").write_bytes(EXPORTER.canonical_json({
                    "variants": {variant: {"artifacts": [entry]}}
                }))
                weights = reference.Weights(EXPORTER, root, root, variant)
                try:
                    quantized, scales = EXPORTER.quantize_groups(values, bits)
                    expected = quantized.astype(np.float32) * scales.astype(np.float32)[:, None]
                    np.testing.assert_array_equal(weights.rows("layers.0.self_attn.q_proj.weight", 1, 3), expected[1:3])
                    np.testing.assert_array_equal(weights.rows("layers.0.self_attn.q_proj.weight", 0, 1), expected[0:1])
                    self.assertEqual(len(weights.mappings), 1)
                finally:
                    weights.close()
        values = np.array([73520.0, -70000.0], dtype=np.float32)
        with self.assertRaises(reference.ActivationOverflow) as caught:
            reference.activation(values, "w8a16")
        np.testing.assert_array_equal(caught.exception.values, values)
        self.assertTrue(np.isfinite(caught.exception.values).all())
        self.assertTrue(np.isfinite(reference.activation(values, "bf16")).all())

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