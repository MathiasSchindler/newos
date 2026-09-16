#!/usr/bin/env python3
"""Focused tests for the TranslateGemma Stage 3 artifact converter."""

import importlib.util
import json
from pathlib import Path
import struct
import sys
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


def quantize_row_mse(values, bits):
    best, best_scales = EXPORTER.quantize_groups(values, bits)
    best_error = np.sum((values - best * best_scales.astype(np.float32)[:, None]) ** 2, axis=1, dtype=np.float64)
    limit = (1 << (bits - 1)) - 1
    for ratio in (0.95, 0.9, 0.85, 0.8, 0.75, 0.7, 0.65, 0.6, 0.55, 0.5):
        scales = (np.max(np.abs(values), axis=1) * np.float32(ratio / limit)).astype(np.float16)
        scales = np.where(scales > 0, scales, np.float16(1))
        quantized = np.rint(values / scales.astype(np.float32)[:, None]).clip(-limit, limit).astype(np.int8)
        error = np.sum((values - quantized * scales.astype(np.float32)[:, None]) ** 2, axis=1, dtype=np.float64)
        improved = error < best_error
        best[improved] = quantized[improved]; best_scales[improved] = scales[improved]
        best_error[improved] = error[improved]
    return best, best_scales


def quantize_grouped_w4(values, group_size):
    values = np.asarray(values, dtype=np.float32)
    if values.ndim != 2 or group_size <= 0 or group_size % 2 or values.shape[1] == 0 or values.shape[1] % group_size:
        raise ValueError("grouped W4 requires nonempty rows divisible by an even group size")
    quantized, scales = EXPORTER.quantize_groups(values.reshape(-1, group_size), 4)
    return EXPORTER.pack_s4(quantized.reshape(values.shape)), scales.reshape(values.shape[0], -1)


def dequantize_grouped_w4(packed, scales, group_size):
    quantized = np.empty((packed.shape[0], packed.shape[1] * 2), dtype=np.int8)
    quantized[:, 0::2] = (packed << np.uint8(4)).view(np.int8) >> 4
    quantized[:, 1::2] = packed.view(np.int8) >> 4
    grouped = quantized.reshape(packed.shape[0], -1, group_size).astype(np.float32)
    return (grouped * scales.astype(np.float32)[..., None]).reshape(quantized.shape)


class Stage3Tests(unittest.TestCase):
    def test_diagnostic_grouped_w4(self):
        values = np.random.default_rng(19).normal(0, 0.1, (2, 64)).astype(np.float32)
        values[0, 0] = 7; values[1, 32:] = 0
        packed, scales = quantize_grouped_w4(values, 32)
        actual = dequantize_grouped_w4(packed, scales, 32)
        self.assertEqual(packed.shape, (2, 32))
        self.assertEqual(packed.dtype, np.uint8)
        self.assertEqual(scales.shape, (2, 2))
        self.assertEqual(scales.dtype, np.float16)
        for row in range(2):
            for start in (0, 32):
                quantized, scale = EXPORTER.quantize_groups(values[row:row + 1, start:start + 32], 4)
                np.testing.assert_array_equal(actual[row:row + 1, start:start + 32], quantized * scale.astype(np.float32)[:, None])
        original, original_scales = EXPORTER.quantize_groups(values, 4)
        before = original * original_scales.astype(np.float32)[:, None]
        full_row_packed, full_row_scales = quantize_grouped_w4(values, values.shape[1])
        np.testing.assert_array_equal(full_row_packed, EXPORTER.pack_s4(original))
        np.testing.assert_array_equal(dequantize_grouped_w4(full_row_packed, full_row_scales, values.shape[1]), before)
        self.assertLess(np.sum((actual - values) ** 2), np.sum((before - values) ** 2))
        np.testing.assert_array_equal(actual[1, 32:], 0)
        for size in (0, 3, 30, 128):
            with self.assertRaises(ValueError):
                quantize_grouped_w4(values, size)

    def test_diagnostic_row_mse(self):
        values = np.random.default_rng(17).normal(0, 0.1, (4, 256)).astype(np.float32)
        values[0, 0] = 2; values[1] = 0
        original, original_scales = EXPORTER.quantize_groups(values, 4)
        candidate, scales = quantize_row_mse(values, 4)
        before = np.sum((values - original * original_scales.astype(np.float32)[:, None]) ** 2, axis=1)
        after = np.sum((values - candidate * scales.astype(np.float32)[:, None]) ** 2, axis=1)
        self.assertTrue(np.all(after <= before))
        self.assertTrue(np.any(after < before))
        self.assertEqual(scales.dtype, np.float16)
        self.assertTrue(np.all((candidate >= -7) & (candidate <= 7)))
        np.testing.assert_array_equal(candidate[1], 0)

    def test_generation_limit_diagnostics(self):
        spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        self.assertEqual(reference.generation_status("w4a16", [42] * 64), "token_limit")
        self.assertEqual(reference.generation_status("w8a16", [42, 106]), "ok")
        self.assertEqual(reference.generation_status("bf16", [42, 1]), "ok")
        for variant, sequence in (("bf16", [42] * 64), ("w4a16", [42]),
                                  ("w4a16", []), ("w4a16", [106, 42]),
                                  ("w4a16", [262145, 106]), ("w4a16", [42] * 65)):
            with self.assertRaises(ValueError):
                reference.generation_status(variant, sequence)

    def test_scaled_residual_normalization(self):
        spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        weights = type("Weights", (), {"variant": "w8a16",
            "rows": lambda self, name: np.array([0.5, -0.5, 2, 0], dtype=np.float32)})()
        unscaled = reference.Reference(weights)
        scaled = reference.Reference(weights, residual_scale=16)
        for magnitude in (1e-5, 1, 73520):
            values = np.array([[1, -2, 3, -4]], dtype=np.float32) * magnitude
            np.testing.assert_array_equal(unscaled.norm(values, "norm"),
                scaled.norm(values / 16, "norm", input_scale=16))
            np.testing.assert_array_equal(unscaled.norm(values, "norm") / 16,
                scaled.norm(values, "norm", output_scale=16))
        np.testing.assert_array_equal(scaled.residual(np.array([73520 / 16], dtype=np.float32)), [4596])
        self.assertEqual(scaled.residual_max, 4595)
        with self.assertRaises(reference.ActivationOverflow):
            unscaled.residual(np.array([73520], dtype=np.float32))
        with self.assertRaises(ValueError):
            reference.Reference(weights, residual_scale=3)

    def test_checkpoint_rope_scaling(self):
        spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        weights = type("Weights", (), {"variant": "w8a16"})()
        current = reference.Reference(weights)
        historical = reference.Reference(weights, global_rope_factor=1)
        positions = np.array([0, 1, 1023, 1024, 2047], dtype=np.float32)
        for local in (True, False):
            theta = 10000.0 if local else 1000000.0
            frequency = theta ** (-np.arange(128, dtype=np.float64) / 128)
            angles = positions[:, None] * frequency / (1 if local else 8)
            expected = np.concatenate((angles, angles), axis=-1)
            cosine, sine = current.rope(positions, local)
            np.testing.assert_allclose(cosine, np.cos(expected), atol=0.0005, rtol=0)
            np.testing.assert_allclose(sine, np.sin(expected), atol=0.0005, rtol=0)
        for actual, expected in zip(current.rope(positions, True), historical.rope(positions, True)):
            np.testing.assert_array_equal(actual, expected)
        for actual, expected in zip(current.rope(positions, False), historical.rope(positions / 8, False)):
            np.testing.assert_array_equal(actual, expected)
        with self.assertRaises(ValueError):
            reference.Reference(weights, global_rope_factor=0)

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
            manifest = {"schema_version": 2, "execution_contract": reference.EXECUTION_CONTRACT,
                "model": {"repository": EXPORTER.REPOSITORY,
                "revision": EXPORTER.REVISION}, "complete": False, "artifacts": [scalar, *recorder.entries]}
            path = root / "manifest.json"
            path.write_bytes(EXPORTER.canonical_json(manifest))
            reference.verify_reference(EXPORTER, root, require_complete=False)
            manifest["schema_version"] = 1
            path.write_bytes(EXPORTER.canonical_json(manifest))
            with self.assertRaises(ValueError):
                reference.verify_reference(EXPORTER, root, require_complete=False)
            manifest["schema_version"] = 2
            manifest["execution_contract"] = dict(reference.EXECUTION_CONTRACT, global_rope={"factor": 1})
            path.write_bytes(EXPORTER.canonical_json(manifest))
            with self.assertRaises(ValueError):
                reference.verify_reference(EXPORTER, root, require_complete=False)
            manifest["execution_contract"] = reference.EXECUTION_CONTRACT
            path.write_bytes(EXPORTER.canonical_json(manifest))
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


def residual_bound_audit():
    spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    root = TOOLS.parent
    report = {"residual_divisor": 32,
        "assumptions": "finite projection/branch inputs; exact-real RMSNorm bound sqrt(width)*abs(gain); FP16 rounding budget included, FP32/HTP normalization error excluded",
        "variants": {}}
    unit_roundoff = 2 ** -11
    subnormal_error = 2 ** -25
    for variant in ("w8a16", "w4a16"):
        weights = reference.Weights(EXPORTER, root / "data/translategemma-4b",
            root / "models/translategemma-4b-stage3", variant)
        try:
            embedding_max = np.zeros(2560, dtype=np.float64)
            for start in range(0, 262145, 2048):
                rows = reference.activation(weights.rows("embed_tokens.weight", start, min(start + 2048, 262145)), variant)
                embedding_max = np.maximum(embedding_max, np.max(np.abs(rows), axis=0))
            multiplier = float(reference.activation(np.float32(2560 ** 0.5), variant)) / 32
            exact = embedding_max * multiplier
            envelope = exact * (1 + unit_roundoff) + subnormal_error
            for layer in range(34):
                for suffix in ("post_attention_layernorm.weight", "post_feedforward_layernorm.weight"):
                    gain = np.abs(1 + weights.rows(f"layers.{layer}." + suffix).astype(np.float64)) / 32
                    branch = np.sqrt(2560) * gain
                    exact += branch
                    rounded_branch = branch * (1 + unit_roundoff) + subnormal_error
                    envelope = (envelope + rounded_branch) * (1 + unit_roundoff) + subnormal_error
            report["variants"][variant] = {"real_arithmetic_bound": float(exact.max()),
                "fp16_rounding_envelope": float(envelope.max()), "limit": 65504}
            if envelope.max() >= 65504:
                raise ValueError("divisor 32 has insufficient residual rounding-envelope headroom")
            print(variant, json.dumps(report["variants"][variant]), flush=True)
        finally:
            weights.close()
    (root / "models/translategemma-residual-bound-audit.json").write_bytes(EXPORTER.canonical_json(report))
    print("PASS residual-only analytical range bound; not a bound on projections or HTP numerical error", flush=True)


def residual_rounding_audit(divergence=False, embedding_ablation=False, mse_ablation=False, candidate_generation=False, group_size=0):
    from transformers import AutoTokenizer

    spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)

    class WideResidual(reference.Reference):
        def residual(self, values):
            self.residual_max = max(self.residual_max, float(np.max(np.abs(values))))
            return values

        def norm(self, values, name, input_scale=1, output_scale=1):
            if name.endswith(("post_attention_layernorm.weight", "post_feedforward_layernorm.weight")):
                gain = np.float32(1) + self.weights.rows(name)
                return values / np.sqrt(np.mean(values * values, axis=-1, keepdims=True) + np.float32(1e-6)) * gain
            return super().norm(values, name, input_scale, output_scale)

    root = TOOLS.parent
    model_dir = root / "data/translategemma-4b"
    tokenizer = AutoTokenizer.from_pretrained(model_dir, local_files_only=True)
    if divergence:
        candidate_ablation = mse_ablation or group_size != 0
        candidate_label = f"group{group_size}" if group_size else "mse"

        class CandidateWeights(reference.Weights):
            def __init__(self):
                super().__init__(EXPORTER, model_dir, root / "models/translategemma-4b-stage3", "w4a16")
                self.quantized_rows = {}

            def rows(self, name, start=None, end=None):
                full_name = reference.PREFIX + name
                shape = self.entries[full_name]["shape"]
                if len(shape) == 1:
                    return super().rows(name, start, end)
                start = 0 if start is None else start
                end = shape[0] if end is None else end
                key = (name, start, end)
                if key not in self.quantized_rows:
                    if start == 0 and name.endswith("self_attn.q_proj.weight"):
                        print("Quantizing candidate " + name, flush=True)
                    values = EXPORTER.bf16_to_f32(self.source(full_name)[start:end])
                    self.quantized_rows[key] = quantize_grouped_w4(values, group_size) if group_size else quantize_row_mse(values, 4)
                    if end == shape[0]:
                        EXPORTER.close_memmap(self.mappings.pop(full_name))
                quantized, scales = self.quantized_rows[key]
                if group_size:
                    return dequantize_grouped_w4(quantized, scales, group_size)
                return quantized.astype(np.float32) * scales.astype(np.float32)[:, None]

        manifest_path = root / "models/translategemma-4b-stage5-v2/manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        artifact_dir = root / "models/translategemma-4b-stage3"
        report = {"method": "cached teacher-forced common prefix at first divergent decision",
              "complete": False, "deployment_changed": False,
              "source_manifest_sha256": EXPORTER.sha256_file(manifest_path),
              "weight_manifest_sha256": EXPORTER.sha256_file(artifact_dir / "manifest.json"),
              "global_rope_factor": 8, "cases": []}
        report["candidate_group_size"] = group_size or "input-dimension"
        report["deployment_layout_compatible"] = not bool(group_size)
        report_name = f"translategemma-{candidate_label}-divergence-audit.json" if candidate_ablation else "translategemma-embedding-divergence-audit.json" if embedding_ablation else "translategemma-divergence-audit.json"
        report_path = root / "models" / report_name
        report_path.write_bytes(EXPORTER.canonical_json(report))
        candidate_weights = CandidateWeights() if candidate_ablation else None

        def describe(logits, expected, observed):
            if not np.isfinite(logits).all():
                raise ValueError("nonfinite diagnostic logits")
            order = np.argsort(-logits, kind="stable")[:5]
            return {"selected_id": int(np.argmax(logits)),
                    "expected_minus_w4_logit": float(logits[expected] - logits[observed]),
                    "expected_logit": float(logits[expected]), "w4_logit": float(logits[observed]),
                    "expected_rank": int(np.count_nonzero(logits > logits[expected])) + 1,
                    "top5": [{"id": int(token), "text": tokenizer.decode([int(token)]),
                              "logit": float(logits[token])} for token in order]}

        for baseline in manifest["translations"][:3]:
            candidate = next(record for record in manifest["translations"]
                             if record["variant"] == "w4a16" and record["text"] == baseline["text"])
            if candidate["prompt_ids"] != baseline["prompt_ids"]:
                raise ValueError("variant prompts differ")
            first = next(index for index, pair in enumerate(zip(baseline["generated_ids"], candidate["generated_ids"]))
                         if pair[0] != pair[1])
            expected, observed = baseline["generated_ids"][first], candidate["generated_ids"][first]
            prompt = baseline["prompt_ids"]
            case = {"text": baseline["text"], "source": baseline["source"], "target": baseline["target"],
                    "decision": first + 1, "common_prefix": baseline["generated_ids"][:first],
                    "expected_id": expected, "w4_id": observed, "variants": {}}
            variants = (f"w4-{candidate_label}",) if candidate_ablation else ("w4-w8-embedding", "w8-w4-embedding") if embedding_ablation else (
                "bf16", "w8a16", "w4a16", "w4-wide-residual")
            for variant in variants:
                weight_variant = "w4a16" if variant.startswith("w4-") else "w8a16" if variant.startswith("w8-") else variant
                weights = candidate_weights if candidate_ablation else reference.Weights(EXPORTER, model_dir, artifact_dir,
                                            weight_variant)
                embedding_weights = None
                if embedding_ablation:
                    embedding_weights = reference.Weights(EXPORTER, model_dir, artifact_dir,
                        "w8a16" if weight_variant == "w4a16" else "w4a16")
                    weights.embedding = embedding_weights.embedding
                captured = {}
                original_linear = weights.linear

                def capture_linear(values, name, rows):
                    if name == "embed_tokens.weight":
                        captured["final_hidden"] = values.copy()
                    return original_linear(values, name, rows)

                weights.linear = capture_linear
                try:
                    engine = WideResidual(weights) if variant == "w4-wide-residual" else reference.Reference(
                        weights, residual_scale=1 if variant == "bf16" else 32)
                    logits = engine.forward(prompt, np.arange(len(prompt)))
                    for index, token in enumerate(case["common_prefix"]):
                        logits = engine.forward([token], [len(prompt) + index])
                    result = describe(logits, expected, observed)
                    if variant in ("bf16", "w8a16", "w4a16") and result["selected_id"] != (observed if variant == "w4a16" else expected):
                        raise ValueError("diagnostic did not reproduce saved first divergence")
                    case["variants"][variant] = result
                    if variant == "w4a16":
                        head = reference.Weights(EXPORTER, model_dir, artifact_dir, "w8a16")
                        try:
                            head_logits = head.linear(captured["final_hidden"], "embed_tokens.weight", 262208)[0]
                            case["variants"]["w4-hidden-w8-head"] = describe(head_logits, expected, observed)
                        finally:
                            head.close()
                    print(json.dumps({"case": case["text"], "variant": variant, "result": result}), flush=True)
                finally:
                    weights.linear = original_linear
                    if not candidate_ablation:
                        weights.close()
                    if embedding_weights is not None:
                        embedding_weights.close()
            report["cases"].append(case)
            report_path.write_bytes(EXPORTER.canonical_json(report))
        report["complete"] = True
        report_path.write_bytes(EXPORTER.canonical_json(report))
        if candidate_weights is not None:
            try:
                if candidate_generation:
                    generation_report = {
                        "variant": f"W4 {candidate_label} input embeddings, transformer and output head, FP16 activations",
                        "deployment_changed": False, "complete": False,
                        "group_size": group_size or "input-dimension",
                        "deployment_layout_compatible": not bool(group_size),
                        "source_manifest_sha256": EXPORTER.sha256_file(manifest_path),
                        "weight_manifest_sha256": EXPORTER.sha256_file(artifact_dir / "manifest.json"),
                        "global_rope_factor": 8, "residual_divisor": 32,
                        "translations": []}
                    destination = root / "models" / f"translategemma-{candidate_label}-generation-audit.json"
                    destination.write_bytes(EXPORTER.canonical_json(generation_report))
                    engine = reference.Reference(candidate_weights, residual_scale=32)
                    for baseline in manifest["translations"][:3]:
                        generated = engine.generate(baseline["prompt_ids"], 64)
                        record = {"source": baseline["source"], "target": baseline["target"],
                                  "text": baseline["text"], "prompt_ids": baseline["prompt_ids"],
                                  "generated_ids": generated,
                                  "translation": tokenizer.decode(generated, skip_special_tokens=True),
                                  "status": reference.generation_status("w4a16", generated),
                                  "matches_bf16": generated == baseline["generated_ids"]}
                        generation_report["translations"].append(record)
                        destination.write_bytes(EXPORTER.canonical_json(generation_report))
                        print(json.dumps(record), flush=True)
                    generation_report["complete"] = True
                    destination.write_bytes(EXPORTER.canonical_json(generation_report))
            finally:
                candidate_weights.close()
        if candidate_ablation or embedding_ablation:
            print("Completed quantization diagnostic; this is not translation-quality acceptance", flush=True)
        else:
            print("PASS saved first-divergence reproduction; precision ablations recorded", flush=True)
        return
    source, target, text = reference.CORPUS[1]
    prompt = tokenizer.apply_chat_template([{"role": "user", "content": [{"type": "text",
        "source_lang_code": source, "target_lang_code": target, "text": text}]}],
        tokenize=True, add_generation_prompt=True, return_dict=False)
    weights = reference.Weights(EXPORTER, model_dir, root / "models/translategemma-4b-stage3", "w4a16")
    report = {"global_rope_factor": 8, "prompt_ids": prompt, "variants": {}}
    try:
        for label, engine in (("scaled_fp16", reference.Reference(weights, residual_scale=32)),
                              ("wide_fp32", WideResidual(weights))):
            first = int(np.argmax(engine.forward(prompt, np.arange(len(prompt)))))
            second = int(np.argmax(engine.forward([first], [len(prompt)])))
            report["variants"][label] = {"first_two_ids": [first, second], "residual_max": engine.residual_max}
            print(label, json.dumps(report["variants"][label]), flush=True)
    finally:
        weights.close()
    report["same_first_two_ids"] = report["variants"]["scaled_fp16"]["first_two_ids"] == report["variants"]["wide_fp32"]["first_two_ids"]
    (root / "models/translategemma-residual-rounding-audit.json").write_bytes(EXPORTER.canonical_json(report))
    print("Completed diagnostic residual-rounding comparison; no deployment FP32 fallback added", flush=True)


def head_generation_audit():
    from transformers import AutoTokenizer

    spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    root = TOOLS.parent
    model_dir = root / "data/translategemma-4b"
    artifact_dir = root / "models/translategemma-4b-stage3"
    tokenizer = AutoTokenizer.from_pretrained(model_dir, local_files_only=True)
    manifest = json.loads((root / "models/translategemma-4b-stage5-v2/manifest.json").read_text(encoding="utf-8"))
    weights = reference.Weights(EXPORTER, model_dir, artifact_dir, "w4a16")
    head = reference.Weights(EXPORTER, model_dir, artifact_dir, "w8a16")
    original_linear = weights.linear

    def mixed_linear(values, name, rows):
        if name == "embed_tokens.weight":
            return head.linear(values, name, rows)
        return original_linear(values, name, rows)

    weights.linear = mixed_linear
    report = {"variant": "W4 input embeddings and transformer, W8 output head, FP16 activations",
              "deployment_changed": False, "translations": []}
    try:
        engine = reference.Reference(weights, residual_scale=32)
        for baseline in manifest["translations"][:3]:
            generated = engine.generate(baseline["prompt_ids"], 64)
            record = {"source": baseline["source"], "target": baseline["target"], "text": baseline["text"],
                      "generated_ids": generated, "translation": tokenizer.decode(generated, skip_special_tokens=True),
                      "status": reference.generation_status("w4a16", generated),
                      "matches_bf16": generated == baseline["generated_ids"]}
            report["translations"].append(record)
            (root / "models/translategemma-w8-head-generation-audit.json").write_bytes(EXPORTER.canonical_json(report))
            print(json.dumps(record), flush=True)
    finally:
        weights.close(); head.close()
    print("Completed W8-head generation experiment; inspect quality before deployment", flush=True)


def residual_audit():
    from transformers import AutoTokenizer

    spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    root = TOOLS.parent
    model_dir = root / "data/translategemma-4b"
    tokenizer = AutoTokenizer.from_pretrained(model_dir, local_files_only=True)
    prompts = [tokenizer.apply_chat_template([{"role": "user", "content": [{"type": "text",
        "source_lang_code": source, "target_lang_code": target, "text": text}]}],
        tokenize=True, add_generation_prompt=True, return_dict=False)
        for source, target, text in reference.CORPUS]
    report = {"global_rope_factor": 8, "variants": {}}
    scale = 1
    for variant in ("bf16", "w8a16", "w4a16"):
        weights = reference.Weights(EXPORTER, model_dir, root / "models/translategemma-4b-stage3", variant)
        records = []
        try:
            for index, prompt in enumerate(prompts):
                engine = reference.Reference(weights, residual_scale=scale)
                logits = engine.forward(prompt, np.arange(len(prompt)))
                record = {"corpus": index, "residual_scale": scale,
                    "stored_residual_max": engine.residual_max,
                    "first_token": int(np.argmax(logits)), "finite": bool(np.isfinite(logits).all())}
                records.append(record)
                print(variant, json.dumps(record), flush=True)
        finally:
            weights.close()
        report["variants"][variant] = records
        if variant == "bf16":
            maximum = max(record["stored_residual_max"] for record in records)
            while maximum / scale > 65504 / 4:
                scale *= 2
            if scale > 1024:
                raise ValueError("residual range exceeds supported scaling experiment")
            report["selected_scale"] = scale
            print(f"Selected power-of-two scale {scale} with at least 4x BF16 prompt headroom", flush=True)
    (root / "models/translategemma-residual-audit.json").write_bytes(EXPORTER.canonical_json(report))
    print("PASS full-depth scaled residual prompt audit", flush=True)


def torch_oracle():
    import hashlib
    import torch
    import transformers
    from tokenizers import Tokenizer
    from transformers import Gemma3TextConfig, AutoTokenizer
    from transformers.models.gemma3 import modeling_gemma3

    if torch.__version__.split("+")[0] != "2.14.0" or transformers.__version__ != "5.17.0":
        raise ValueError("independent oracle requires torch 2.14.0 and transformers 5.17.0")
    torch.set_num_threads(4)
    spec = importlib.util.spec_from_file_location("translategemma_reference", TOOLS / "translategemma-reference.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    root = TOOLS.parent
    model_dir = root / "data/translategemma-4b"
    config_data = json.loads((model_dir / "config.json").read_text(encoding="utf-8"))["text_config"]
    config = Gemma3TextConfig(**config_data)
    config._attn_implementation = "eager"
    tokenizer = AutoTokenizer.from_pretrained(model_dir, local_files_only=True)
    source, target, text = reference.CORPUS[0]
    messages = [{"role": "user", "content": [{"type": "text",
        "source_lang_code": source, "target_lang_code": target, "text": text}]}]
    prompt = tokenizer.apply_chat_template(messages, tokenize=True, add_generation_prompt=True, return_dict=False)
    rendered = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
    pinned_tokens = Tokenizer.from_file(str(model_dir / "tokenizer.json")).encode(rendered, add_special_tokens=False).ids
    if prompt != pinned_tokens:
        raise ValueError("current upstream tokenizer changed the pinned prompt IDs")
    rotary = modeling_gemma3.Gemma3RotaryEmbedding(config)
    report = {"torch": torch.__version__, "transformers": transformers.__version__,
        "upstream_module_sha256": hashlib.sha256(Path(modeling_gemma3.__file__).read_bytes()).hexdigest(),
        "config_sha256": hashlib.sha256((model_dir / "config.json").read_bytes()).hexdigest(),
        "weights_manifest_sha256": EXPORTER.sha256_file(root / "models/translategemma-4b-stage3/manifest.json"),
        "pinned_prompt_ids_match": True,
        "rope_parameters": config.rope_parameters, "prompt_ids": prompt,
        "quantized_weight_rounding": "Stage 3 dequantization then FP16 weights for stock torch Linear; not HTP bit parity",
        "variants": {}}
    positions = torch.arange(len(prompt)).unsqueeze(0)
    query = torch.arange(len(prompt))[:, None]
    key = torch.arange(len(prompt))[None, :]
    with torch.no_grad():
        for variant in ("bf16", "w8a16", "w4a16"):
            dtype = torch.bfloat16 if variant == "bf16" else torch.float16
            weights = reference.Weights(EXPORTER, model_dir, root / "models/translategemma-4b-stage3", variant)
            try:
                embedded = np.stack([weights.rows("embed_tokens.weight", token, token + 1)[0] for token in prompt])
                hidden = torch.from_numpy(embedded).to(dtype).unsqueeze(0)
                hidden = hidden * torch.tensor(2560 ** 0.5, dtype=dtype)
                expected = reference.Reference(weights)
                position_embeddings = {}
                for kind in ("sliding_attention", "full_attention"):
                    position_embeddings[kind] = rotary(hidden, positions, kind)
                    oracle_values = rotary(hidden, torch.tensor([[0, 1, 1023, 1024, 2047]]), kind)
                    numpy_values = expected.rope([0, 1, 1023, 1024, 2047], kind == "sliding_attention")
                    for actual, wanted in zip(oracle_values, numpy_values):
                        np.testing.assert_allclose(actual.float().numpy()[0], wanted, atol=0.008, rtol=0)
                layers = []
                for layer_index in range(6):
                    with torch.device("meta"):
                        block = modeling_gemma3.Gemma3DecoderLayer(config, layer_index)
                    state = {name: torch.from_numpy(weights.rows(f"layers.{layer_index}." + name)).to(dtype)
                             for name in block.state_dict()}
                    block.load_state_dict(state, assign=True)
                    del state
                    block.eval()
                    captured = {}

                    def capture(name):
                        def hook(module, inputs, output):
                            captured[name] = output.detach().float()
                        return hook

                    block.post_attention_layernorm.register_forward_hook(capture("attention_branch"))
                    block.post_feedforward_layernorm.register_forward_hook(capture("mlp_branch"))
                    kind = config.layer_types[layer_index]
                    visible = key <= query
                    if kind == "sliding_attention":
                        visible = visible & (query - key < config.sliding_window)
                    mask = torch.where(visible, 0.0, torch.finfo(dtype).min).to(dtype)[None, None]
                    output = block(hidden, position_embeddings=position_embeddings[kind], attention_mask=mask)
                    attention_sum = hidden.float() + captured["attention_branch"]
                    mlp_sum = attention_sum.to(dtype).float() + captured["mlp_branch"]
                    record = {"layer": layer_index, "attention_sum_max": float(attention_sum.abs().max()),
                        "mlp_sum_max": float(mlp_sum.abs().max()), "finite": bool(torch.isfinite(output).all())}
                    layers.append(record)
                    print(variant, json.dumps(record), flush=True)
                    hidden = output
                    del block
                    if not record["finite"]:
                        break
                report["variants"][variant] = layers
            finally:
                weights.close()
    destination = root / "models/translategemma-precision-oracle.json"
    destination.write_bytes(EXPORTER.canonical_json(report))
    if not all(record["finite"] for record in report["variants"]["bf16"]):
        raise ValueError("upstream BF16 oracle failed")
    for variant in ("w8a16", "w4a16"):
        if report["variants"][variant][-1]["finite"]:
            raise ValueError("upstream oracle did not reproduce the expected FP16 overflow")
    print("PASS independent upstream RoPE and FP16 overflow reproduction", flush=True)


if __name__ == "__main__":
    if sys.argv[1:] == ["--torch-oracle"]:
        torch_oracle()
    elif sys.argv[1:] == ["--residual-audit"]:
        residual_audit()
    elif sys.argv[1:] == ["--residual-rounding-audit"]:
        residual_rounding_audit()
    elif sys.argv[1:] == ["--divergence-audit"]:
        residual_rounding_audit(divergence=True)
    elif sys.argv[1:] == ["--embedding-divergence-audit"]:
        residual_rounding_audit(divergence=True, embedding_ablation=True)
    elif sys.argv[1:] == ["--mse-divergence-audit"]:
        residual_rounding_audit(divergence=True, mse_ablation=True)
    elif sys.argv[1:] == ["--mse-generation-audit"]:
        residual_rounding_audit(divergence=True, mse_ablation=True, candidate_generation=True)
    elif sys.argv[1:] == ["--group32-generation-audit"]:
        residual_rounding_audit(divergence=True, group_size=32, candidate_generation=True)
    elif sys.argv[1:] == ["--head-generation-audit"]:
        head_generation_audit()
    elif sys.argv[1:] == ["--residual-bound-audit"]:
        residual_bound_audit()
    else:
        unittest.main()