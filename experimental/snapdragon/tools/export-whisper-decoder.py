#!/usr/bin/env python3
"""Export a pinned Whisper decoder to versioned newos runtime artifacts."""

import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

ARTIFACT_MAGIC = 0x3254464152485357
ARTIFACT_VERSION = 2
ARTIFACT_HEADER_SIZE = 96
FNV_OFFSET_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
PAYLOAD_DECODER_WEIGHTS = 1
PAYLOAD_TOKEN_BYTES = 2
PAYLOAD_CROSS_KV_WEIGHTS = 3
PAYLOAD_FRONTEND_WEIGHTS = 5
PAYLOAD_ENCODER_WEIGHTS = 6
ELEMENT_F32 = 1
ELEMENT_F16 = 2
ELEMENT_U8 = 3


class SafeTensorsFile:
    def __init__(self, path):
        self.path = path
        self.size = path.stat().st_size
        with path.open("rb") as stream:
            self.header_size = struct.unpack("<Q", stream.read(8))[0]
            self.header = json.loads(stream.read(self.header_size))
        self.data_start = 8 + self.header_size

    def f32(self, name, shape):
        metadata = self.header[name]
        if metadata["dtype"] != "F32" or metadata["shape"] != list(shape):
            raise ValueError(f"unexpected tensor contract for {name}: {metadata}")
        start, end = metadata["data_offsets"]
        values = np.fromfile(
            self.path, dtype="<f4", count=int(np.prod(shape)),
            offset=self.data_start + start,
        )
        if values.nbytes != end - start:
            raise ValueError(f"invalid tensor byte range for {name}")
        return values.reshape(shape)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fnv1a64_update(value, data):
    for byte in memoryview(data).cast("B"):
        value ^= byte
        value = value * FNV_PRIME & 0xFFFFFFFFFFFFFFFF
    return value


def load_model_spec(catalog_path, model_name, model_dir):
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    if model_name not in catalog:
        raise ValueError(f"model {model_name!r} is not pinned in {catalog_path}")
    pinned = catalog[model_name]
    config = json.loads((model_dir / "config.json").read_text(encoding="utf-8"))
    fields = {
        "model_id": pinned["model_id"],
        "name": model_name,
        "width": config["d_model"],
        "ffn_width": config["decoder_ffn_dim"],
        "attention_heads": config["decoder_attention_heads"],
        "encoder_layers": config["encoder_layers"],
        "decoder_layers": config["decoder_layers"],
        "vocabulary_size": config["vocab_size"],
        "text_context": config["max_target_positions"],
        "mel_bins": config["num_mel_bins"],
        "encoder_frames": config["max_source_positions"],
        "revision": pinned["revision"],
        "sha256": pinned["sha256"],
        "repository": pinned["repository"],
    }
    if config.get("model_type") != "whisper":
        raise ValueError("checkpoint config is not Whisper")
    if fields["width"] % fields["attention_heads"] or \
       fields["width"] // fields["attention_heads"] != 64 or \
       fields["ffn_width"] != fields["width"] * 4:
        raise ValueError("unsupported Whisper architecture")
    return fields


def encode_header(spec, payload_type, element_type, element_count, payload_size, payload_hash):
    values = (
        ARTIFACT_MAGIC, ARTIFACT_VERSION, ARTIFACT_HEADER_SIZE,
        spec["model_id"], payload_type, element_type, 0,
        element_count, payload_size, payload_hash,
        spec["width"], spec["ffn_width"], spec["attention_heads"],
        spec["encoder_layers"], spec["decoder_layers"],
        spec["vocabulary_size"], spec["text_context"], spec["mel_bins"],
        spec["encoder_frames"], 0,
    )
    header = struct.pack("<QIIIIIIQQQIIIIIIIIII", *values)
    if len(header) != ARTIFACT_HEADER_SIZE:
        raise AssertionError(f"artifact header is {len(header)} bytes")
    return header


class ArtifactWriter:
    def __init__(self, path, spec, payload_type, element_type, element_count):
        self.path = path
        self.temporary = path.with_suffix(path.suffix + ".tmp")
        self.spec = spec
        self.payload_type = payload_type
        self.element_type = element_type
        self.element_count = element_count
        self.payload_size = 0
        self.payload_hash = FNV_OFFSET_BASIS
        self.stream = self.temporary.open("w+b")
        self.stream.write(b"\0" * ARTIFACT_HEADER_SIZE)

    def write(self, data):
        view = memoryview(data).cast("B")
        self.stream.write(view)
        self.payload_hash = fnv1a64_update(self.payload_hash, view)
        self.payload_size += len(view)

    def close(self):
        self.stream.seek(0)
        self.stream.write(encode_header(
            self.spec, self.payload_type, self.element_type,
            self.element_count, self.payload_size, self.payload_hash,
        ))
        self.stream.close()
        self.temporary.replace(self.path)

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        if exception_type is None:
            self.close()
        else:
            self.stream.close()
            self.temporary.unlink(missing_ok=True)


def decoder_tensor_contract(spec):
    width = spec["width"]
    tensors = [
        ("model.decoder.embed_tokens.weight", (spec["vocabulary_size"], width)),
        ("model.decoder.embed_positions.weight", (spec["text_context"], width)),
        ("model.encoder.layer_norm.weight", (width,)),
        ("model.encoder.layer_norm.bias", (width,)),
        ("model.decoder.layer_norm.weight", (width,)),
        ("model.decoder.layer_norm.bias", (width,)),
    ]
    for layer in range(spec["decoder_layers"]):
        prefix = f"model.decoder.layers.{layer}"
        tensors.extend([
            (f"{prefix}.self_attn_layer_norm.weight", (width,)),
            (f"{prefix}.self_attn_layer_norm.bias", (width,)),
            (f"{prefix}.self_attn.k_proj.weight", (width, width)),
            (f"{prefix}.self_attn.q_proj.weight", (width, width)),
            (f"{prefix}.self_attn.q_proj.bias", (width,)),
            (f"{prefix}.self_attn.v_proj.weight", (width, width)),
            (f"{prefix}.self_attn.v_proj.bias", (width,)),
            (f"{prefix}.self_attn.out_proj.weight", (width, width)),
            (f"{prefix}.self_attn.out_proj.bias", (width,)),
            (f"{prefix}.encoder_attn_layer_norm.weight", (width,)),
            (f"{prefix}.encoder_attn_layer_norm.bias", (width,)),
            (f"{prefix}.encoder_attn.k_proj.weight", (width, width)),
            (f"{prefix}.encoder_attn.q_proj.weight", (width, width)),
            (f"{prefix}.encoder_attn.q_proj.bias", (width,)),
            (f"{prefix}.encoder_attn.v_proj.weight", (width, width)),
            (f"{prefix}.encoder_attn.v_proj.bias", (width,)),
            (f"{prefix}.encoder_attn.out_proj.weight", (width, width)),
            (f"{prefix}.encoder_attn.out_proj.bias", (width,)),
            (f"{prefix}.final_layer_norm.weight", (width,)),
            (f"{prefix}.final_layer_norm.bias", (width,)),
            (f"{prefix}.fc1.weight", (spec["ffn_width"], width)),
            (f"{prefix}.fc1.bias", (spec["ffn_width"],)),
            (f"{prefix}.fc2.weight", (width, spec["ffn_width"])),
            (f"{prefix}.fc2.bias", (width,)),
        ])
    return tensors


def bytes_to_unicode():
    byte_values = list(range(ord("!"), ord("~") + 1))
    byte_values += list(range(ord("¡"), ord("¬") + 1))
    byte_values += list(range(ord("®"), ord("ÿ") + 1))
    unicode_values = byte_values.copy()
    extra = 0
    for value in range(256):
        if value not in byte_values:
            byte_values.append(value)
            unicode_values.append(256 + extra)
            extra += 1
    return dict(zip(byte_values, map(chr, unicode_values)))


def export_weights(model, spec, output):
    contract = decoder_tensor_contract(spec)
    float_count = sum(int(np.prod(shape)) for _, shape in contract)
    with ArtifactWriter(output, spec, PAYLOAD_DECODER_WEIGHTS, ELEMENT_F32, float_count) as artifact:
        for name, shape in contract:
            artifact.write(np.asarray(model.f32(name, shape), dtype="<f4"))
    return contract, float_count


def export_tokens(vocab_path, spec, output):
    vocab = json.loads(vocab_path.read_text(encoding="utf-8"))
    by_id = [None] * spec["vocabulary_size"]
    for token, token_id in vocab.items():
        if 0 <= token_id < len(by_id):
            by_id[token_id] = token
    inverse = {character: value for value, character in bytes_to_unicode().items()}
    offsets = [0]
    data = bytearray()
    for token_id, token in enumerate(by_id):
        if token is not None and token_id < 50257:
            data.extend(inverse[character] for character in token)
        offsets.append(len(data))
    offset_data = struct.pack(f"<{len(offsets)}I", *offsets)
    payload_size = len(offset_data) + len(data)
    with ArtifactWriter(
        output, spec, PAYLOAD_TOKEN_BYTES, ELEMENT_U8, payload_size
    ) as artifact:
        artifact.write(offset_data)
        artifact.write(data)
    return len(data)


def export_cross_kv(model, spec, output):
    width = spec["width"]
    tensors = [
        ("model.encoder.layer_norm.weight", (width,)),
        ("model.encoder.layer_norm.bias", (width,)),
    ]
    for layer in range(spec["decoder_layers"]):
        prefix = f"model.decoder.layers.{layer}.encoder_attn"
        tensors.extend([
            (f"{prefix}.k_proj.weight", (width, width)),
            (f"{prefix}.v_proj.weight", (width, width)),
            (f"{prefix}.v_proj.bias", (width,)),
        ])
    value_count = sum(int(np.prod(shape)) for _, shape in tensors)
    with ArtifactWriter(output, spec, PAYLOAD_CROSS_KV_WEIGHTS, ELEMENT_F16, value_count) as artifact:
        for name, shape in tensors:
            artifact.write(np.asarray(model.f32(name, shape), dtype="<f2"))
    return tensors, value_count


def export_frontend(model, spec, output):
    width = spec["width"]
    tensors = [
        ("model.encoder.conv1.weight", (width, spec["mel_bins"], 3)),
        ("model.encoder.conv1.bias", (width,)),
        ("model.encoder.conv2.weight", (width, width, 3)),
        ("model.encoder.conv2.bias", (width,)),
        ("model.encoder.embed_positions.weight", (spec["encoder_frames"], width)),
    ]
    value_count = sum(int(np.prod(shape)) for _, shape in tensors)
    with ArtifactWriter(
        output, spec, PAYLOAD_FRONTEND_WEIGHTS, ELEMENT_F16, value_count
    ) as artifact:
        for name, shape in tensors:
            artifact.write(np.asarray(model.f32(name, shape), dtype="<f2"))
    return tensors, value_count


def encoder_tensor_contract(spec):
    width = spec["width"]
    tensors = []
    for layer in range(spec["encoder_layers"]):
        prefix = f"model.encoder.layers.{layer}"
        tensors.extend([
            (f"{prefix}.self_attn_layer_norm.weight", (width,), 1.0),
            (f"{prefix}.self_attn_layer_norm.bias", (width,), 1.0),
            (f"{prefix}.self_attn.q_proj.weight", (width, width), 0.125),
            (f"{prefix}.self_attn.q_proj.bias", (width,), 0.125),
            (f"{prefix}.self_attn.k_proj.weight", (width, width), 1.0),
            (None, (width,), 0.0),
            (f"{prefix}.self_attn.v_proj.weight", (width, width), 1.0),
            (f"{prefix}.self_attn.v_proj.bias", (width,), 1.0),
            (f"{prefix}.self_attn.out_proj.weight", (width, width), 1.0),
            (f"{prefix}.self_attn.out_proj.bias", (width,), 1.0),
            (f"{prefix}.final_layer_norm.weight", (width,), 1.0),
            (f"{prefix}.final_layer_norm.bias", (width,), 1.0),
            (f"{prefix}.fc1.weight", (spec["ffn_width"], width), 1.0),
            (f"{prefix}.fc1.bias", (spec["ffn_width"],), 1.0),
            (f"{prefix}.fc2.weight", (width, spec["ffn_width"]), 1.0),
            (f"{prefix}.fc2.bias", (width,), 1.0),
        ])
    return tensors


def export_encoder(model, spec, output):
    tensors = encoder_tensor_contract(spec)
    value_count = sum(int(np.prod(shape)) for _, shape, _ in tensors)
    with ArtifactWriter(
        output, spec, PAYLOAD_ENCODER_WEIGHTS, ELEMENT_F16, value_count
    ) as artifact:
        for name, shape, scale in tensors:
            values = np.zeros(shape, dtype="<f2") if name is None else np.asarray(
                model.f32(name, shape) * np.float32(scale), dtype="<f2"
            )
            artifact.write(values)
    return tensors, value_count


def main():
    script_dir = Path(__file__).resolve().parent
    snapdragon_dir = script_dir.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default="tiny")
    parser.add_argument("--catalog", type=Path, default=script_dir / "whisper-models.json")
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    model_dir = (args.model_dir or snapdragon_dir / "models" / f"whisper-{args.model}").resolve()
    output_dir = (args.output_dir or model_dir / "decoder-f32").resolve()
    model_path = model_dir / "model.safetensors"
    spec = load_model_spec(args.catalog.resolve(), args.model, model_dir)
    if sha256(model_path) != spec["sha256"]:
        raise ValueError("model SHA-256 does not match the pinned catalog entry")
    output_dir.mkdir(parents=True, exist_ok=True)
    weights_path = output_dir / "weights-f32.bin"
    tokens_path = output_dir / "token-bytes.bin"
    cross_kv_path = output_dir / "cross-kv-fp16.bin"
    encoder_dir = model_dir / "encoder-fp16"
    encoder_dir.mkdir(parents=True, exist_ok=True)
    frontend_path = encoder_dir / "frontend-fp16.bin"
    encoder_path = encoder_dir / "encoder-fp16.bin"
    model = SafeTensorsFile(model_path)
    contract, float_count = export_weights(model, spec, weights_path)
    token_bytes = export_tokens(model_dir / "vocab.json", spec, tokens_path)
    cross_contract, cross_count = export_cross_kv(model, spec, cross_kv_path)
    frontend_contract, frontend_count = export_frontend(model, spec, frontend_path)
    encoder_contract, encoder_count = export_encoder(model, spec, encoder_path)
    manifest = {
        "format": "newos.whisper.decoder.v2",
        "model": spec,
        "float_count": float_count,
        "token_bytes": token_bytes,
        "weights_size": weights_path.stat().st_size,
        "weights_sha256": sha256(weights_path),
        "token_table_size": tokens_path.stat().st_size,
        "token_table_sha256": sha256(tokens_path),
        "cross_kv_value_count": cross_count,
        "cross_kv_size": cross_kv_path.stat().st_size,
        "cross_kv_sha256": sha256(cross_kv_path),
        "cross_kv_tensor_order": [
            {"name": name, "shape": list(shape)} for name, shape in cross_contract
        ],
        "frontend_weight_count": frontend_count,
        "frontend_size": frontend_path.stat().st_size,
        "frontend_sha256": sha256(frontend_path),
        "frontend_tensor_order": [
            {"name": name, "shape": list(shape)} for name, shape in frontend_contract
        ],
        "encoder_weight_count": encoder_count,
        "encoder_size": encoder_path.stat().st_size,
        "encoder_sha256": sha256(encoder_path),
        "encoder_tensor_order": [
            {"name": name or "zero", "shape": list(shape), "scale": scale}
            for name, shape, scale in encoder_contract
        ],
        "tensor_order": [
            {"name": name, "shape": list(shape)} for name, shape in contract
        ],
    }
    (output_dir / "deployment.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    print(f"Wrote {weights_path} ({weights_path.stat().st_size} bytes)")
    print(f"Wrote {tokens_path} ({tokens_path.stat().st_size} bytes)")
    print(f"Wrote {cross_kv_path} ({cross_kv_path.stat().st_size} bytes)")
    print(f"Wrote {frontend_path} ({frontend_path.stat().st_size} bytes)")
    print(f"Wrote {encoder_path} ({encoder_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
