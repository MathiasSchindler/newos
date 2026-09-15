"""Offline bounded-memory Gemma 3 reference; never imported by deployed C code."""

import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import platform
import shutil
import struct
import time
import uuid

import numpy as np


PREFIX = "language_model.model."
CORPUS = [
    ("cs", "de-DE", "V nejhor\u0161\u00edm p\u0159\u00edpad\u011b i k prasknut\u00ed \u010do\u010dky."),
    ("en", "ja", "Good morning."),
    ("de", "en", "Der Zug kommt um 15:30 Uhr an."),
]

EXECUTION_CONTRACT = {
    "global_rope": {"type": "linear", "theta": 1000000, "factor": 8},
    "local_rope": {"type": "default", "theta": 10000, "factor": 1},
    "residual": {"storage": "activation-dtype", "bf16_divisor": 1, "quantized_divisor": 32,
                 "pre_norm_epsilon": 9.765625e-10, "post_norm_gain_divisor": 32},
}


def validate_checkpoint_rope(model_dir):
    config = json.loads((Path(model_dir) / "config.json").read_text(encoding="utf-8"))["text_config"]
    if config.get("rope_scaling") is not None or config.get("rope_theta") != 1000000 or \
            config.get("rope_local_base_freq") != 10000 or config.get("rope_parameters") != {
                "full_attention": {"factor": 8.0, "rope_type": "linear"},
                "sliding_attention": {"rope_type": "default"}}:
        raise ValueError("checkpoint RoPE contract changed")


class ActivationOverflow(ValueError):
    def __init__(self, values, variant):
        self.values = np.asarray(values, dtype=np.float32).copy()
        super().__init__(f"{variant} activation overflow: shape={values.shape}, max_abs={float(np.max(np.abs(values))):.9g}")


def bf16(values):
    values = np.asarray(values, dtype=np.float32)
    bits = values.view(np.uint32)
    rounded = bits + np.uint32(0x7fff) + ((bits >> 16) & np.uint32(1))
    return (rounded & np.uint32(0xffff0000)).view(np.float32)


def activation(values, variant):
    values = np.asarray(values, dtype=np.float32)
    if not np.isfinite(values).all():
        raise ValueError("nonfinite reference intermediate")
    with np.errstate(over="ignore", invalid="ignore"):
        result = bf16(values) if variant == "bf16" else values.astype(np.float16).astype(np.float32)
    if not np.isfinite(result).all():
        raise ActivationOverflow(values, variant)
    return result


def generation_status(variant, generated, maximum=64):
    if not generated or len(generated) > maximum or any(token < 0 or token >= 262145 for token in generated):
        raise ValueError("invalid generated reference sequence")
    if any(token in (1, 106) for token in generated[:-1]):
        raise ValueError("reference continued past a stop token")
    if generated[-1] in (1, 106):
        return "ok"
    if variant == "bf16" or len(generated) != maximum:
        raise ValueError("acceptance translation did not reach a stop token")
    return "token_limit"


class Weights:
    def __init__(self, exporter, model_dir, artifact_dir, variant):
        self.exporter = exporter
        self.model_dir = Path(model_dir)
        self.artifact_dir = Path(artifact_dir)
        self.variant = variant
        self.index = json.loads((self.model_dir / "model.safetensors.index.json").read_text())["weight_map"]
        self.shards = {}
        self.mappings = {}
        self.entries = {}
        if variant != "bf16":
            manifest = json.loads((self.artifact_dir / "manifest.json").read_text())
            self.entries = {entry["name"]: entry for entry in manifest["variants"][variant]["artifacts"]}

    def source(self, name):
        if name in self.mappings:
            return self.mappings[name]
        shard_name = self.index[name]
        if shard_name not in self.shards:
            self.shards[shard_name] = self.exporter.SafeTensorShard(self.model_dir / shard_name)
        shard = self.shards[shard_name]
        self.mappings[name] = shard.bf16(name, shard.tensors[name]["shape"])
        return self.mappings[name]

    def close(self):
        for mapping in self.mappings.values():
            self.exporter.close_memmap(mapping)
        self.mappings.clear()

    def rows(self, name, start=None, end=None):
        name = PREFIX + name
        if self.variant == "bf16":
            mapping = self.source(name)
            return self.exporter.bf16_to_f32(mapping if start is None else mapping[start:end])
        entry = self.entries[name]
        path = self.artifact_dir / self.variant / entry["path"]
        if name not in self.mappings:
            self.mappings[name] = np.memmap(path, mode="r", dtype="u1")
        buffer = self.mappings[name]
        shape = entry["shape"]
        if len(shape) == 1:
            mapping = np.ndarray(tuple(shape), dtype="<f2", buffer=buffer, offset=256)
            result = mapping.astype(np.float32)
            return result
        start = 0 if start is None else start
        end = shape[0] if end is None else end
        width = shape[1]
        bits = 4 if self.variant == "w4a16" else 8
        stored_width = width // 2 if bits == 4 else width
        mapping = np.ndarray((end - start, stored_width), dtype="u1", buffer=buffer,
                     offset=256 + start * stored_width)
        if bits == 4:
            unpacked = np.empty((end - start, width), dtype=np.int8)
            unpacked[:, 0::2] = (mapping << np.uint8(4)).view(np.int8) >> 4
            unpacked[:, 1::2] = mapping.view(np.int8) >> 4
        else:
            unpacked = mapping.view(np.int8)
        scales = np.ndarray((end - start,), dtype="<f2", buffer=buffer,
                    offset=256 + entry["data_size"] + start * 2)
        result = unpacked.astype(np.float32) * scales.astype(np.float32)[:, None]
        return result

    def linear(self, values, name, rows):
        output = np.empty((*values.shape[:-1], rows), dtype=np.float32)
        for start in range(0, rows, 2048):
            end = min(rows, start + 2048)
            weights = self.rows(name, start, end)
            output[..., start:end] = values @ weights.T
        try:
            return activation(output, self.variant)
        except ValueError as error:
            raise ValueError(f"projection {name}: {error}") from error

    def embedding(self, ids, residual_scale=1):
        rows = np.stack([self.rows("embed_tokens.weight", int(token), int(token) + 1)[0] for token in ids])
        rows = activation(rows, self.variant)
        scale = activation(np.float32(2560 ** 0.5), self.variant)
        return activation(rows * (scale / np.float32(residual_scale)), self.variant)


class Recorder:
    def __init__(self, exporter, root):
        self.exporter, self.root, self.entries = exporter, Path(root), []

    def array(self, name, values, semantic_dtype="float32"):
        values = np.asarray(values)
        if values.dtype.kind == "f":
            values = values.astype("<f4")
            if not np.isfinite(values).all():
                raise ValueError(f"nonfinite fixture: {name}")
            dtype = "float32-le"
        else:
            values = values.astype("<u4")
            dtype = "uint32-le"
        filename = hashlib.sha256(name.encode()).hexdigest()[:24] + ".gta"
        entry = self.exporter.write_blob_artifact(self.root / filename, "fixture/" + name,
                                                  values.tobytes(), self.exporter.KIND_FIXTURE)
        self.exporter.verify_artifact(self.root / filename, entry)
        entry.update({"array_shape": list(values.shape), "storage_dtype": dtype,
                      "semantic_dtype": semantic_dtype})
        self.entries.append(entry)


class Reference:
    def __init__(self, weights, global_rope_factor=8, residual_scale=1):
        if global_rope_factor not in (1, 8):
            raise ValueError("unsupported global RoPE factor")
        if residual_scale not in (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024):
            raise ValueError("residual scale must be a supported power of two")
        self.weights = weights
        self.variant = weights.variant
        self.global_rope_factor = global_rope_factor
        self.residual_scale = residual_scale
        self.residual_max = 0.0
        self.cache = {}

    def cast(self, values):
        return activation(values, self.variant)

    def norm(self, values, name, input_scale=1, output_scale=1):
        weight = self.weights.rows(name)
        epsilon = np.float32(1e-6 / (input_scale * input_scale))
        output = values / np.sqrt(np.mean(values * values, axis=-1, keepdims=True, dtype=np.float32) + epsilon)
        try:
            return self.cast(output * ((np.float32(1) + weight) / np.float32(output_scale)))
        except ValueError as error:
            raise ValueError(f"RMSNorm {name}: {error}") from error

    def rope(self, positions, local):
        theta = np.float32(10000 if local else 1000000)
        inverse = np.float32(1) / theta ** (np.arange(0, 256, 2, dtype=np.float32) / np.float32(256))
        inverse /= np.float32(1 if local else self.global_rope_factor)
        angles = np.asarray(positions, dtype=np.float32)[:, None] * inverse[None, :]
        angles = np.concatenate((angles, angles), axis=-1)
        return self.cast(np.cos(angles)), self.cast(np.sin(angles))

    def rotate(self, values, cosine, sine):
        rotated = np.concatenate((-values[..., 128:], values[..., :128]), axis=-1)
        return self.cast(self.cast(values * cosine[None, :, :]) + self.cast(rotated * sine[None, :, :]))

    def residual(self, values):
        self.residual_max = max(self.residual_max, float(np.max(np.abs(values))))
        return self.cast(values)

    def layer(self, hidden, positions, layer, recorder=None, cache=False):
        stem = f"layers.{layer}."
        label = f"{self.variant}/layer-{layer}"

        def record(name, value):
            if recorder is not None:
                recorder.array(label + "/" + name, value, self.variant)
            return value

        record("input", hidden)
        residual = hidden
        normalized = record("input-rmsnorm", self.norm(hidden, stem + "input_layernorm.weight", input_scale=self.residual_scale))
        query = self.weights.linear(normalized, stem + "self_attn.q_proj.weight", 2048).reshape(-1, 8, 256).transpose(1, 0, 2)
        key = self.weights.linear(normalized, stem + "self_attn.k_proj.weight", 1024).reshape(-1, 4, 256).transpose(1, 0, 2)
        value = self.weights.linear(normalized, stem + "self_attn.v_proj.weight", 1024).reshape(-1, 4, 256).transpose(1, 0, 2)
        record("q-projection", query); record("k-projection", key); record("v-projection", value)
        query = record("q-rmsnorm", self.norm(query, stem + "self_attn.q_norm.weight"))
        key = record("k-rmsnorm", self.norm(key, stem + "self_attn.k_norm.weight"))
        local = (layer + 1) % 6 != 0
        cosine, sine = self.rope(positions, local)
        record("rope-cos", cosine); record("rope-sin", sine)
        query = record("q-rope", self.rotate(query, cosine, sine))
        key = record("k-rope", self.rotate(key, cosine, sine))
        key_positions = np.asarray(positions)
        if cache and layer in self.cache:
            old_key, old_value, old_positions = self.cache[layer]
            key = np.concatenate((old_key, key), axis=1)
            value = np.concatenate((old_value, value), axis=1)
            key_positions = np.concatenate((old_positions, key_positions))
        if cache:
            retained = 1024 if local else 2048
            self.cache[layer] = (key[:, -retained:], value[:, -retained:], key_positions[-retained:])
        expanded_key = np.repeat(key, 2, axis=0)
        expanded_value = np.repeat(value, 2, axis=0)
        raw_scores = query @ expanded_key.transpose(0, 2, 1)
        try:
            scores = self.cast(self.cast(raw_scores) * np.float32(0.0625))
        except ValueError as error:
            raise ValueError(f"layer {layer} attention unscaled QK dot: {error}") from error
        visible = key_positions[None, :] <= np.asarray(positions)[:, None]
        if local:
            visible &= np.asarray(positions)[:, None] - key_positions[None, :] < 1024
        record("mask-visible", visible.astype(np.uint32))
        masked = np.where(visible[None, :, :], scores, np.float32(-3.3895313892515355e38 if self.variant == "bf16" else -65504))
        record("attention-scores", masked)
        probabilities = np.exp(masked - masked.max(axis=-1, keepdims=True))
        probabilities = record("attention-softmax", self.cast(probabilities / probabilities.sum(axis=-1, keepdims=True)))
        attention = record("gqa", self.cast(probabilities @ expanded_value).transpose(1, 0, 2).reshape(-1, 2048))
        projected = record("attention-output", self.weights.linear(attention, stem + "self_attn.o_proj.weight", 2560))
        try:
            hidden = record("attention-residual", self.residual(residual + self.norm(projected,
                stem + "post_attention_layernorm.weight", output_scale=self.residual_scale)))
        except ActivationOverflow as error:
            raise ValueError(f"attention residual addition: {error}") from error
        residual = hidden
        normalized = record("pre-mlp-rmsnorm", self.norm(hidden, stem + "pre_feedforward_layernorm.weight", input_scale=self.residual_scale))
        gate = record("gate-projection", self.weights.linear(normalized, stem + "mlp.gate_proj.weight", 10240))
        up = record("up-projection", self.weights.linear(normalized, stem + "mlp.up_proj.weight", 10240))
        gelu = self.cast(np.float32(0.5) * gate * (np.float32(1) + np.tanh(np.float32((2 / np.pi) ** 0.5) *
                          (gate + np.float32(0.044715) * gate * gate * gate))))
        record("gelu", gelu)
        gated = record("gated-gelu", self.cast(gelu * up))
        down = record("mlp", self.weights.linear(gated, stem + "mlp.down_proj.weight", 2560))
        try:
            return record("output", self.residual(residual + self.norm(down,
                stem + "post_feedforward_layernorm.weight", output_scale=self.residual_scale)))
        except ActivationOverflow as error:
            raise ValueError(f"MLP residual addition: {error}") from error

    def forward(self, ids, positions):
        try:
            hidden = self.residual(self.weights.embedding(ids, self.residual_scale))
        except ValueError as error:
            raise ValueError(f"scaled embedding: {error}") from error
        for layer in range(34):
            try:
                hidden = self.layer(hidden, positions, layer, cache=True)
            except ValueError as error:
                raise ValueError(f"decoder layer {layer}: {error}") from error
        final = self.norm(hidden[-1:], "norm.weight", input_scale=self.residual_scale)
        return self.weights.linear(final, "embed_tokens.weight", 262208)[0]

    def generate(self, prompt, maximum):
        self.cache.clear()
        self.residual_max = 0.0
        generated = []
        logits = self.forward(prompt, np.arange(len(prompt)))
        for step in range(maximum):
            token = int(np.argmax(logits))
            generated.append(token)
            print(f"  {self.variant} step {step + 1}: token {token}", flush=True)
            if token in (1, 106):
                break
            if token >= 262145:
                raise ValueError("model generated an unmapped tokenizer ID")
            if step + 1 < maximum:
                logits = self.forward([token], [len(prompt) + step])
        return generated


def scalar_fixtures(exporter, path):
    cases = []

    def add(operation, count, auxiliary, data):
        cases.append(struct.pack("<4I", operation, count, auxiliary, len(data)) + data)

    for bits in (4, 8):
        signed = np.arange(-(1 << (bits - 1)), 1 << (bits - 1), dtype=np.int16).astype(np.int8)
        raw = signed.view(np.uint8)
        packed = ((raw[0::2] & 15) | ((raw[1::2] & 15) << 4)) if bits == 4 else raw
        for scale in (np.float16(0.125), np.float16(0.003), np.float16(1)):
            data = scale.tobytes() + packed.tobytes() + (signed.astype(np.float32) * np.float32(scale)).astype("<f4").tobytes()
            add(1, len(signed), bits, data)
    for local in (True, False):
        for position in (0, 1, 1023, 1024, 2047):
            frequency = 1.0 / (10000.0 if local else 1000000.0) ** (np.arange(128, dtype=np.float64) * 2 / 256)
            frequency /= 1 if local else 8
            angles = np.tile(position * frequency, 2)
            cosine, sine = np.cos(angles).astype(np.float32), np.sin(angles).astype(np.float32)
            values = (np.arange(256, dtype=np.float32) - 128) / 128
            rotated = np.concatenate((-values[128:], values[:128]))
            expected = values * cosine + rotated * sine
            add(2, 256, 0, b"".join(array.astype("<f4").tobytes() for array in (values, cosine, sine, expected)))
    for layer in (0, 5, 33, 34):
        for query in (0, 1023, 1024, 2047, 2048):
            for key in (0, 1, 1023, 1024, 2047, 2048):
                visible = layer < 34 and query < 2048 and key <= query and ((layer + 1) % 6 == 0 or query - key < 1024)
                add(3, 0, 0, struct.pack("<4I", layer, query, key, int(visible)))
    for layer in (0, 5, 33, 34):
        for head in (0, 3, 4):
            for position in (0, 1023, 1024, 2047, 2048):
                for channel in (0, 255, 256):
                    valid = layer < 34 and head < 4 and position < 2048 and channel < 256
                    capacity = 2048 if (layer + 1) % 6 == 0 else 1024
                    offset = (head * capacity + position % capacity) * 256 + channel if valid else 0
                    add(4, 0, 0, struct.pack("<5IQ", layer, head, position, channel, int(valid), offset))
    for values, expected in (([1, 3, 3, 2], 1), ([-2, -1, -1], 1), ([0, -0.0], 0),
                             ([1, np.nan], 0xffffffff), ([np.inf], 0xffffffff), ([], 0xffffffff)):
        add(5, len(values), expected, np.asarray(values, dtype="<f4").tobytes())
    half_bits = np.arange(65536, dtype="<u2")
    with np.errstate(invalid="ignore"):
        add(6, 65536, 0, half_bits.view("<f2").astype("<f4").tobytes())
    payload = struct.pack("<3I", 0x354e4d47, 1, len(cases)) + b"".join(cases)
    entry = exporter.write_blob_artifact(path, "fixture/numeric-scalars-v1", payload, exporter.KIND_FIXTURE)
    exporter.verify_artifact(path, entry)
    return entry, len(cases)


def self_test():
    values = np.array([1.00390625, 1.01171875, -1.00390625, 0], dtype=np.float32)
    np.testing.assert_array_equal(bf16(values), [1, 1.015625, -1, 0])
    for variant in ("bf16", "w8a16", "w4a16"):
        np.testing.assert_array_equal(activation(np.array([0, 1, -2], dtype=np.float32), variant), [0, 1, -2])

    class SyntheticWeights:
        variant = "bf16"

        def rows(self, name):
            return np.zeros(256 if "self_attn" in name else 2560, dtype=np.float32)

        def linear(self, hidden, name, rows):
            result = np.zeros((hidden.shape[0], rows), dtype=np.float32)
            if name.endswith("v_proj.weight"):
                for token in range(hidden.shape[0]):
                    for head in range(4):
                        result[token, head * 256:(head + 1) * 256] = token * 4 + head
            return result

    class Capture:
        def __init__(self):
            self.values = {}

        def array(self, name, values, semantic_dtype):
            self.values[name.rsplit("/", 1)[1]] = values.copy()

    reference = Reference(SyntheticWeights())
    sample = np.arange(1, 257, dtype=np.float32).reshape(1, 256) / 256
    high_precision = sample.astype(np.float64) / np.sqrt(np.mean(sample.astype(np.float64) ** 2) + 1e-6)
    np.testing.assert_array_equal(reference.norm(sample, "self_attn.q_norm.weight"), bf16(high_precision))
    for layer in (0, 5):
        capture = Capture()
        hidden = np.ones((3, 2560), dtype=np.float32)
        result = reference.layer(hidden, np.array([0, 1023, 1024]), layer, capture)
        np.testing.assert_array_equal(result, hidden)
        expected_mask = [[1, 0, 0], [1, 1, 0], [0 if layer == 0 else 1, 1, 1]]
        np.testing.assert_array_equal(capture.values["mask-visible"], expected_mask)
        for token, visible in enumerate(expected_mask):
            probabilities = bf16(np.array(visible, dtype=np.float32) / sum(visible))
            for query_head in range(8):
                value_head = query_head // 2
                expected = bf16(sum(probabilities[row] * np.float32(row * 4 + value_head) for row in range(3)))
                np.testing.assert_array_equal(capture.values["gqa"][token, query_head * 256:(query_head + 1) * 256],
                                              np.full(256, expected, dtype=np.float32))
    class CacheWeights(SyntheticWeights):
        def linear(self, hidden, name, rows):
            result = np.zeros((hidden.shape[0], rows), dtype=np.float32)
            if name.endswith("v_proj.weight"):
                for head in range(4):
                    result[:, head * 256:(head + 1) * 256] = bf16(hidden[:, :1] + head)
            return result

    cache_input = np.tile(np.arange(1, 2561, dtype=np.float32)[None, :], (3, 1))
    cache_input[1] = cache_input[1, ::-1]
    cache_input[2] = 1
    positions = np.array([0, 1023, 1024])
    for layer in (0, 5):
        full_capture = Capture()
        Reference(CacheWeights()).layer(cache_input, positions, layer, full_capture)
        cached = Reference(CacheWeights())
        cached.layer(cache_input[:2], positions[:2], layer, cache=True)
        step_capture = Capture()
        cached.layer(cache_input[2:], positions[2:], layer, step_capture, cache=True)
        np.testing.assert_array_equal(step_capture.values["gqa"], full_capture.values["gqa"][-1:])
        np.testing.assert_array_equal(step_capture.values["mask-visible"], full_capture.values["mask-visible"][-1:])
    print("PASS numerical rounding, RMSNorm, grouped-head, window-boundary and cached-attention checks", flush=True)


def verify_reference(exporter, output, require_complete=True):
    root = Path(output)
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 2 or manifest.get("execution_contract") != EXECUTION_CONTRACT or manifest.get("model") != {
        "repository": exporter.REPOSITORY, "revision": exporter.REVISION
    } or (require_complete and manifest.get("complete") is not True):
        raise ValueError("incomplete or wrong-model numerical reference manifest")
    expected = {"manifest.json"}
    names = set()
    arrays = {}
    for entry in manifest["artifacts"]:
        path = Path(entry["path"])
        if path.is_absolute() or len(path.parts) != 1 or entry["name"] in names:
            raise ValueError("invalid numerical artifact path or duplicate name")
        names.add(entry["name"])
        expected.add(path.as_posix())
        exporter.verify_artifact(root / path, entry)
        if "array_shape" in entry:
            shape = entry["array_shape"]
            if not shape or len(shape) > 8 or any(not isinstance(dimension, int) or dimension <= 0 for dimension in shape):
                raise ValueError("invalid numerical array dimensions")
            if entry["storage_dtype"] not in ("float32-le", "uint32-le") or math.prod(shape) * 4 != entry["payload_size"]:
                raise ValueError("numerical array shape/byte mismatch")
            if entry["storage_dtype"] == "float32-le":
                values = np.memmap(root / path, mode="r", dtype="<f4", offset=256)
                finite = all(np.isfinite(values[start:start + 1048576]).all() for start in range(0, len(values), 1048576))
                exporter.close_memmap(values)
                if not finite:
                    raise ValueError("nonfinite recorded layer output")
            arrays[entry["name"]] = entry
    if "fixture/numeric-scalars-v1" not in names:
        raise ValueError("missing scalar fixture artifact")
    if "source_audit_sha256" in manifest:
        expected.add("source-audit.json")
        if exporter.sha256_file(root / "source-audit.json") != manifest["source_audit_sha256"]:
            raise ValueError("source audit hash mismatch")
    if manifest.get("complete"):
        if "source_audit_sha256" not in manifest:
            raise ValueError("missing numerical source audit")
        required = {"input", "input-rmsnorm", "q-projection", "k-projection", "v-projection",
                    "q-rmsnorm", "k-rmsnorm", "rope-cos", "rope-sin", "q-rope", "k-rope",
                    "mask-visible", "attention-scores", "attention-softmax", "gqa", "attention-output",
                    "attention-residual", "pre-mlp-rmsnorm", "gate-projection", "up-projection", "gelu",
                    "gated-gelu", "mlp", "output", "final-rmsnorm", "vocabulary-projection"}
        for variant in ("bf16", "w8a16", "w4a16"):
            for layer in (0, 5):
                if any(f"fixture/{variant}/layer-{layer}/{name}" not in names for name in required):
                    raise ValueError("incomplete primitive/layer numerical inventory")
            for name in ("embedding", "embedding-ids", "positions"):
                if f"fixture/{variant}/{name}" not in names:
                    raise ValueError("missing embedding or position fixture")
        translations = manifest["translations"]
        if len(translations) != 3 * len(CORPUS):
            raise ValueError("incomplete translation corpus")
        ready = all(record.get("status") == "ok" for record in translations)
        if manifest.get("quantized_generation_ready") is not ready:
            raise ValueError("quantized readiness flag contradicts numerical results")
        for index, record in enumerate(translations):
            variant = ("bf16", "w8a16", "w4a16")[index // len(CORPUS)]
            source, target, text = CORPUS[index % len(CORPUS)]
            if (record["variant"], record["source"], record["target"], record["text"]) != (variant, source, target, text):
                raise ValueError("translation corpus identity drift")
            if record.get("status") == "activation_overflow":
                if variant == "bf16" or not record.get("error") or record.get("generated_ids") != []:
                    raise ValueError("invalid quantized numerical failure record")
                entry = arrays[f"fixture/{variant}/corpus-{index % len(CORPUS)}/overflow-fp32"]
                values = np.frombuffer((root / entry["path"]).read_bytes()[256:], dtype="<f4")
                if float(np.max(np.abs(values))) <= 65504 or not np.isfinite(values).all():
                    raise ValueError("overflow diagnostic does not demonstrate FP16 range failure")
                entry = arrays[f"fixture/{variant}/corpus-{index % len(CORPUS)}/prompt"]
                prompt = np.frombuffer((root / entry["path"]).read_bytes()[256:], dtype="<u4")
                if prompt.tolist() != record["prompt_ids"]:
                    raise ValueError("failed-case prompt artifact differs from manifest")
                continue
            if record.get("status") not in ("ok", "token_limit") or \
                    generation_status(variant, record["generated_ids"]) != record["status"]:
                raise ValueError("unknown translation reference status")
            maximum = record.get("stored_residual_max")
            if not isinstance(maximum, (int, float)) or not math.isfinite(maximum) or maximum < 0 or \
                    (variant != "bf16" and maximum > 65504):
                raise ValueError("invalid successful residual range record")
            if len(record["prompt_ids"]) + len(record["generated_ids"]) > 2048 or \
                    (variant == "bf16" and not record["bf16_repeated_exactly"]):
                raise ValueError("translation termination or reproducibility gate failed")
            if hashlib.sha256(record["translation"].encode()).hexdigest() != record["output_sha256"]:
                raise ValueError("translation byte hash mismatch")
            for field, suffix in (("prompt_ids", "prompt"), ("generated_ids", "generated")):
                entry = arrays[f"fixture/{variant}/corpus-{index % len(CORPUS)}/{suffix}"]
                values = np.frombuffer((root / entry["path"]).read_bytes()[256:], dtype="<u4")
                if values.tolist() != record[field]:
                    raise ValueError("translation token artifact differs from manifest")
        for variant in ("w8a16", "w4a16"):
            changed = 0
            exact_tokens = 0
            failures = 0
            for index in range(len(CORPUS)):
                baseline = translations[index]
                candidate = translations[(1 if variant == "w8a16" else 2) * len(CORPUS) + index]
                if candidate["status"] != "ok":
                    failures += 1
                    print(f"  {variant} corpus {index}: {candidate['error']}", flush=True)
                    continue
                changed += baseline["translation"] != candidate["translation"]
                exact_tokens += baseline["generated_ids"] == candidate["generated_ids"]
            print(f"{variant}: {failures} numerical failures, {changed} changed translations; {exact_tokens}/{len(CORPUS)} exact token sequences", flush=True)
            for layer in (0, 5):
                for name in ("output", "vocabulary-projection"):
                    baseline_entry = arrays[f"fixture/bf16/layer-{layer}/{name}"]
                    candidate_entry = arrays[f"fixture/{variant}/layer-{layer}/{name}"]
                    if baseline_entry["array_shape"] != candidate_entry["array_shape"]:
                        raise ValueError("cross-variant fixture shapes differ")
                    baseline = np.frombuffer((root / baseline_entry["path"]).read_bytes()[256:], dtype="<f4").astype(np.float64)
                    candidate = np.frombuffer((root / candidate_entry["path"]).read_bytes()[256:], dtype="<f4").astype(np.float64)
                    if name == "output":
                        candidate *= EXECUTION_CONTRACT["residual"]["quantized_divisor"]
                    difference = candidate - baseline
                    rmse = float(np.sqrt(np.mean(difference ** 2)))
                    maximum = float(np.max(np.abs(difference)))
                    print(f"  layer {layer} {name}: RMSE={rmse:.7g}, max_abs={maximum:.7g}", flush=True)
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}
    if actual != expected:
        raise ValueError("numerical artifact inventory differs from manifest")
    print(f"Verified {len(names)} numerical artifacts; complete={manifest['complete']}", flush=True)
    return manifest


def export_block_reference(exporter, model_dir, catalog, weights_dir, output, replace, prompt=False):
    if np.__version__ != "2.4.3":
        raise ValueError("block reference requires NumPy 2.4.3")
    validate_checkpoint_rope(model_dir)
    self_test()
    output = Path(output).resolve()
    staging = output.with_name(f"{output.name}.partial-{uuid.uuid4().hex}")
    staging.mkdir(parents=True)
    try:
        exporter.validate_source(Path(exporter.__file__).parent, catalog, model_dir, staging / "source-audit.json")
        exporter.validate_artifact_set(weights_dir)
        scalar, count = scalar_fixtures(exporter, staging / "numeric-scalars.gta")
        recorder = Recorder(exporter, staging)
        for variant in (("w4a16",) if prompt else ("w8a16", "w4a16")):
            weights = Weights(exporter, model_dir, weights_dir, variant)
            try:
                if prompt:
                    engine = Reference(weights, residual_scale=32)
                    token_ids = ([2, 9259, 1902] * 86)[:256]
                    hidden = weights.embedding(token_ids, 32)
                    recorder.array("prompt/input", hidden, variant)
                    recorder.array("prompt/token-ids", token_ids, "absolute-positions")
                    for local in (True, False):
                        cosine, sine = engine.rope(np.arange(2048), local)
                        kind = "local" if local else "global"
                        recorder.array(f"prompt/{kind}-cos", cosine[:, :128], variant)
                        recorder.array(f"prompt/{kind}-sin", sine[:, :128], variant)
                    class PromptRecorder:
                        def array(self, name, values, semantic_dtype):
                            if name.rsplit("/", 1)[1] in ("k-rope", "v-projection"):
                                recorder.array("prompt/" + name.split("/", 1)[1], values, semantic_dtype)
                    for layer in range(34):
                        hidden = engine.layer(hidden, np.arange(256), layer, PromptRecorder())
                        print(f"Recorded prompt layer {layer}", flush=True)
                    for valid in (253, 256):
                        normalized = engine.norm(hidden[valid - 1:valid], "norm.weight", input_scale=32)
                        logits = weights.linear(normalized, "embed_tokens.weight", 262208)
                        recorder.array(f"prompt/logits-{valid}", logits, variant)
                    continue
                embedded = weights.embedding([2, 9259, 1902], 32)
                for layer in (0, 5):
                    engine = Reference(weights, residual_scale=32)
                    prefix = f"{variant}/layer-{layer}/"
                    cosine, sine = engine.rope(np.arange(2048), layer == 0)
                    recorder.array(prefix + "cos-table", cosine[:, :128], variant)
                    recorder.array(prefix + "sin-table", sine[:, :128], variant)
                    for step, positions in enumerate(([0, 1023, 1024], [1025, 1026, 2047])):
                        class StepRecorder:
                            def array(self, name, values, semantic_dtype):
                                recorder.array(prefix + f"step-{step}/" + name.rsplit("/", 1)[1], values, semantic_dtype)
                        recorder.array(prefix + f"step-{step}/positions", positions, "absolute-positions")
                        engine.layer(embedded, positions, layer, StepRecorder(), cache=True)
                    print(f"Recorded cached block {variant} layer {layer}", flush=True)
            finally:
                weights.close()
        manifest = {"schema_version": 2, "execution_contract": EXECUTION_CONTRACT,
            "model": {"repository": exporter.REPOSITORY, "revision": exporter.REVISION},
            "purpose": "stage7-prompt" if prompt else "stage6-block-cache", "complete": False, "scalar_cases": count,
            "source_sha256": exporter.sha256_file(__file__),
            "source_audit_sha256": exporter.sha256_file(staging / "source-audit.json"),
            "weights_manifest_sha256": exporter.sha256_file(Path(weights_dir) / "manifest.json"),
            "artifacts": [scalar, *recorder.entries]}
        (staging / "manifest.json").write_bytes(exporter.canonical_json(manifest))
        verify_reference(exporter, staging, require_complete=False)
        exporter.publish_directory(staging, output, replace)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def export_reference(exporter, model_dir, catalog, weights_dir, output, replace,
                     primitives_only=False, selected_variant="all"):
    if np.__version__ != "2.4.3":
        raise ValueError("numerical reference requires pinned NumPy 2.4.3")
    self_test()
    output = Path(output).resolve()
    staging = output.with_name(f"{output.name}.partial-{uuid.uuid4().hex}")
    staging.mkdir(parents=True)
    started = time.perf_counter()
    try:
        scalar, scalar_count = scalar_fixtures(exporter, staging / "numeric-scalars.gta")
        manifest = {"schema_version": 2, "execution_contract": EXECUTION_CONTRACT,
                "model": {"repository": exporter.REPOSITORY, "revision": exporter.REVISION},
                    "backend": "NumPy float32 accumulation with explicit BF16/FP16 operation-boundary rounding",
                    "numpy": np.__version__, "python": platform.python_version(), "machine": platform.machine(),
                    "blas_threads": os.environ.get("OPENBLAS_NUM_THREADS"),
                    "source_sha256": exporter.sha256_file(__file__), "scalar_cases": scalar_count,
                    "artifacts": [scalar], "complete": False,
                    "arithmetic": {"rmsnorm": "float32, (1 + weight), round output",
                        "rope": "split-half; local theta=10000 factor=1; global theta=1000000 linear factor=8",
                        "rope_discrepancy": "checkpoint rope_parameters is authoritative; historical schema 1 reproduced Transformers 4.57.3's ignored factor",
                        "quantized": "actual Stage 3 S4/S8 payloads with stored FP16 per-output-channel scales; FP32 dot then FP16 output",
                        "residual": "quantized hidden = original hidden / 32; embedding and post-norm gains divided before FP16 cast; pre/final RMSNorm epsilon = 1e-6 / 1024",
                        "bf16": "original BF16 weights; FP32 dot then BF16 output; not native PyTorch kernel bit parity",
                        "attention": "8 query / 4 KV heads, score scale 1/16, causal local distance <1024",
                        "greedy": "first maximum, do_sample=False, stop IDs [1,106], output excludes prompt"}}
        if not primitives_only:
            validate_checkpoint_rope(model_dir)
            print("Validating source and W4/W8 artifacts...", flush=True)
            exporter.validate_source(Path(exporter.__file__).parent, catalog, model_dir, staging / "source-audit.json")
            exporter.validate_artifact_set(weights_dir)
            from transformers import AutoTokenizer
            if importlib.metadata.version("transformers") != "4.57.3" or importlib.metadata.version("tokenizers") != "0.22.2":
                raise ValueError("unexpected tokenizer reference version")
            tokenizer = AutoTokenizer.from_pretrained(str(model_dir), local_files_only=True)
            recorder = Recorder(exporter, staging)
            generation_records = []
            variants = ("bf16", "w8a16", "w4a16") if selected_variant == "all" else (selected_variant,)
            for variant in variants:
                print(f"Recording {variant} primitives and local/global layers...", flush=True)
                residual_scale = 1 if variant == "bf16" else EXECUTION_CONTRACT["residual"]["quantized_divisor"]
                reference = Reference(Weights(exporter, model_dir, weights_dir, variant), residual_scale=residual_scale)
                ids = tokenizer.encode("Hello world!", add_special_tokens=True)[:3]
                embedded = reference.weights.embedding(ids, residual_scale)
                recorder.array(variant + "/embedding-ids", ids, "token-ids")
                recorder.array(variant + "/embedding", embedded, variant)
                positions = np.array([0, 1023, 1024], dtype=np.uint32)
                recorder.array(variant + "/positions", positions, "absolute-positions")
                for layer in (0, 5):
                    result = reference.layer(embedded.copy(), positions, layer, recorder)
                    final = reference.norm(result, "norm.weight", input_scale=residual_scale)
                    recorder.array(f"{variant}/layer-{layer}/final-rmsnorm", final, variant)
                    logits = reference.weights.linear(final[-1:], "embed_tokens.weight", 262208)
                    recorder.array(f"{variant}/layer-{layer}/vocabulary-projection", logits, variant)
                for case_index, (source, target, text) in enumerate(CORPUS):
                    messages = [{"role": "user", "content": [{"type": "text", "source_lang_code": source,
                                 "target_lang_code": target, "text": text}]}]
                    prompt = tokenizer.apply_chat_template(messages, tokenize=True, add_generation_prompt=True)
                    maximum = 64
                    if len(prompt) + maximum > 2048:
                        raise ValueError("reference prompt exceeds deployment context")
                    print(f"Generating {variant} corpus {case_index + 1}/{len(CORPUS)}...", flush=True)
                    try:
                        generated = reference.generate(prompt, maximum)
                    except ValueError as error:
                        cause = error
                        while cause is not None and not isinstance(cause, ActivationOverflow):
                            cause = cause.__cause__
                        if variant == "bf16" or cause is None:
                            raise
                        recorder.array(f"{variant}/corpus-{case_index}/prompt", prompt, "token-ids")
                        recorder.array(f"{variant}/corpus-{case_index}/overflow-fp32", cause.values, "pre-cast-fp32-overflow")
                        generation_records.append({"variant": variant, "source": source, "target": target,
                            "text": text, "prompt_ids": prompt, "generated_ids": [],
                            "status": "activation_overflow", "error": str(error), "translation": None,
                            "bf16_repeated_exactly": False})
                        print(f"  RECORDED NUMERICAL FAILURE: {error}", flush=True)
                        continue
                    status = generation_status(variant, generated, maximum)
                    if variant == "bf16":
                        repeated = reference.generate(prompt, maximum)
                        if repeated != generated:
                            raise ValueError("BF16 greedy reference is not reproducible")
                    recorder.array(f"{variant}/corpus-{case_index}/prompt", prompt, "token-ids")
                    recorder.array(f"{variant}/corpus-{case_index}/generated", generated, "token-ids")
                    translation = tokenizer.decode(generated, skip_special_tokens=True)
                    generation_records.append({"variant": variant, "source": source, "target": target,
                        "text": text, "prompt_ids": prompt, "generated_ids": generated, "translation": translation,
                        "status": status,
                        "error": "generation reached 64-token limit without a stop token" if status == "token_limit" else None,
                        "stored_residual_max": reference.residual_max,
                        "output_sha256": hashlib.sha256(translation.encode()).hexdigest(),
                        "bf16_repeated_exactly": variant == "bf16"})
                    print(f"  {len(generated)} tokens: {json.dumps(translation, ensure_ascii=True)}", flush=True)
                reference.weights.close()
            manifest["artifacts"].extend(recorder.entries)
            manifest["translations"] = generation_records
            manifest["quantized_generation_ready"] = selected_variant == "all" and all(record["status"] == "ok" for record in generation_records)
            manifest["source_audit_sha256"] = exporter.sha256_file(staging / "source-audit.json")
            manifest["weights_manifest_sha256"] = exporter.sha256_file(Path(weights_dir) / "manifest.json")
            manifest["complete"] = selected_variant == "all"
        manifest["elapsed_seconds"] = time.perf_counter() - started
        (staging / "manifest.json").write_bytes(exporter.canonical_json(manifest))
        verify_reference(exporter, staging, require_complete=manifest["complete"])
        exporter.publish_directory(staging, output, replace)
        print(f"Published numerical references: {output}; complete={manifest['complete']}", flush=True)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise