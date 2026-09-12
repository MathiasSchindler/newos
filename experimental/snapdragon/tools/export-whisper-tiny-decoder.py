#!/usr/bin/env python3
"""Export pinned Whisper Tiny decoder weights and a runtime token byte table."""

import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

MODEL_SHA256 = "7ebd0e69e78190ffe1438491fa05cc1f5c1aa3a4c4db3bc1723adbb551ea2395"
MODEL_REVISION = "169d4a4341b33bc18d8881c4b69c2e104e1cc0af"
LAYERS = 4
WIDTH = 384
MLP_WIDTH = 1536
VOCAB_SIZE = 51865
MAX_TOKENS = 448
WEIGHT_MAGIC = 0x3154574345445757
TOKEN_MAGIC = 0x314b4f5443454457
CROSS_KV_MAGIC = 0x31564B5143454457


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
            self.path, dtype="<f4", count=np.prod(shape), offset=self.data_start + start
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


def decoder_tensor_contract():
    tensors = [
        ("model.decoder.embed_tokens.weight", (VOCAB_SIZE, WIDTH)),
        ("model.decoder.embed_positions.weight", (MAX_TOKENS, WIDTH)),
        ("model.encoder.layer_norm.weight", (WIDTH,)),
        ("model.encoder.layer_norm.bias", (WIDTH,)),
        ("model.decoder.layer_norm.weight", (WIDTH,)),
        ("model.decoder.layer_norm.bias", (WIDTH,)),
    ]
    for layer in range(LAYERS):
        prefix = f"model.decoder.layers.{layer}"
        tensors.extend([
            (f"{prefix}.self_attn_layer_norm.weight", (WIDTH,)),
            (f"{prefix}.self_attn_layer_norm.bias", (WIDTH,)),
            (f"{prefix}.self_attn.k_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.self_attn.q_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.self_attn.q_proj.bias", (WIDTH,)),
            (f"{prefix}.self_attn.v_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.self_attn.v_proj.bias", (WIDTH,)),
            (f"{prefix}.self_attn.out_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.self_attn.out_proj.bias", (WIDTH,)),
            (f"{prefix}.encoder_attn_layer_norm.weight", (WIDTH,)),
            (f"{prefix}.encoder_attn_layer_norm.bias", (WIDTH,)),
            (f"{prefix}.encoder_attn.k_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.encoder_attn.q_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.encoder_attn.q_proj.bias", (WIDTH,)),
            (f"{prefix}.encoder_attn.v_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.encoder_attn.v_proj.bias", (WIDTH,)),
            (f"{prefix}.encoder_attn.out_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.encoder_attn.out_proj.bias", (WIDTH,)),
            (f"{prefix}.final_layer_norm.weight", (WIDTH,)),
            (f"{prefix}.final_layer_norm.bias", (WIDTH,)),
            (f"{prefix}.fc1.weight", (MLP_WIDTH, WIDTH)),
            (f"{prefix}.fc1.bias", (MLP_WIDTH,)),
            (f"{prefix}.fc2.weight", (WIDTH, MLP_WIDTH)),
            (f"{prefix}.fc2.bias", (WIDTH,)),
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


def export_weights(model, output):
    contract = decoder_tensor_contract()
    float_count = sum(int(np.prod(shape)) for _, shape in contract)
    with output.open("wb") as stream:
        stream.write(struct.pack("<QII", WEIGHT_MAGIC, 1, float_count))
        for name, shape in contract:
            np.asarray(model.f32(name, shape), dtype="<f4").tofile(stream)
    return contract, float_count


def export_tokens(vocab_path, output):
    vocab = json.loads(vocab_path.read_text(encoding="utf-8"))
    by_id = [None] * VOCAB_SIZE
    for token, token_id in vocab.items():
        if 0 <= token_id < VOCAB_SIZE:
            by_id[token_id] = token
    inverse = {character: value for value, character in bytes_to_unicode().items()}
    offsets = [0]
    data = bytearray()
    for token_id, token in enumerate(by_id):
        if token is not None and token_id < 50257:
            data.extend(inverse[character] for character in token)
        offsets.append(len(data))
    with output.open("wb") as stream:
        stream.write(struct.pack("<QIII", TOKEN_MAGIC, 1, VOCAB_SIZE, len(data)))
        stream.write(struct.pack(f"<{len(offsets)}I", *offsets))
        stream.write(data)
    return len(data)


def export_cross_kv(model, output):
    tensors = [
        ("model.encoder.layer_norm.weight", (WIDTH,)),
        ("model.encoder.layer_norm.bias", (WIDTH,)),
    ]
    for layer in range(LAYERS):
        prefix = f"model.decoder.layers.{layer}.encoder_attn"
        tensors.extend([
            (f"{prefix}.k_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.v_proj.weight", (WIDTH, WIDTH)),
            (f"{prefix}.v_proj.bias", (WIDTH,)),
        ])
    value_count = sum(int(np.prod(shape)) for _, shape in tensors)
    with output.open("wb") as stream:
        stream.write(struct.pack("<QII", CROSS_KV_MAGIC, 1, value_count))
        for name, shape in tensors:
            np.asarray(model.f32(name, shape), dtype="<f2").tofile(stream)
    return tensors, value_count


def main():
    script_dir = Path(__file__).resolve().parent
    default_model_dir = script_dir.parent / "models" / "whisper-tiny"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=Path, default=default_model_dir)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    model_dir = args.model_dir.resolve()
    output_dir = (args.output_dir or model_dir / "decoder-f32").resolve()
    model_path = model_dir / "model.safetensors"
    if sha256(model_path) != MODEL_SHA256:
        raise ValueError("model SHA-256 does not match the pinned Whisper Tiny checkpoint")
    output_dir.mkdir(parents=True, exist_ok=True)
    weights_path = output_dir / "weights-f32.bin"
    tokens_path = output_dir / "token-bytes.bin"
    cross_kv_path = output_dir / "cross-kv-fp16.bin"
    model = SafeTensorsFile(model_path)
    contract, float_count = export_weights(model, weights_path)
    token_bytes = export_tokens(model_dir / "vocab.json", tokens_path)
    cross_kv_contract, cross_kv_value_count = export_cross_kv(model, cross_kv_path)
    manifest = {
        "format": "newos.whisper_tiny.decoder_f32.v1",
        "model_revision": MODEL_REVISION,
        "model_sha256": MODEL_SHA256,
        "layers": LAYERS,
        "width": WIDTH,
        "mlp_width": MLP_WIDTH,
        "vocab_size": VOCAB_SIZE,
        "max_tokens": MAX_TOKENS,
        "float_count": float_count,
        "token_bytes": token_bytes,
        "weights_size": weights_path.stat().st_size,
        "weights_sha256": sha256(weights_path),
        "token_table_size": tokens_path.stat().st_size,
        "token_table_sha256": sha256(tokens_path),
        "cross_kv_value_count": cross_kv_value_count,
        "cross_kv_size": cross_kv_path.stat().st_size,
        "cross_kv_sha256": sha256(cross_kv_path),
        "cross_kv_tensor_order": [
            {"name": name, "shape": list(shape)} for name, shape in cross_kv_contract
        ],
        "tensor_order": [{"name": name, "shape": list(shape)} for name, shape in contract],
    }
    (output_dir / "deployment.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    print(f"Wrote {weights_path} ({weights_path.stat().st_size} bytes)")
    print(f"Wrote {tokens_path} ({tokens_path.stat().st_size} bytes)")
    print(f"Wrote {cross_kv_path} ({cross_kv_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
