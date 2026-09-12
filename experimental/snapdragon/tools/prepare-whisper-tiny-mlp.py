#!/usr/bin/env python3
"""Extract and quantize one Whisper Tiny encoder layer's weights."""

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path


MODEL_REVISION = "169d4a4341b33bc18d8881c4b69c2e104e1cc0af"
MODEL_SHA256 = "7ebd0e69e78190ffe1438491fa05cc1f5c1aa3a4c4db3bc1723adbb551ea2395"
MODEL_SIZE = 151061672
ENCODER_LAYER_COUNT = 4
ATTENTION_SCALE = 0.125


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


class SafeTensorsFile:
    def __init__(self, path):
        self.path = path
        self.size = path.stat().st_size
        with path.open("rb") as stream:
            length_bytes = stream.read(8)
            if len(length_bytes) != 8:
                raise ValueError("SafeTensors file is missing its header length")
            self.header_size = struct.unpack("<Q", length_bytes)[0]
            if self.header_size > self.size - 8:
                raise ValueError("SafeTensors header extends beyond the file")
            self.header = json.loads(stream.read(self.header_size))
        self.data_start = 8 + self.header_size

    def read_f32(self, name, expected_shape):
        try:
            metadata = self.header[name]
        except KeyError as error:
            raise ValueError(f"SafeTensors tensor is missing: {name}") from error
        if metadata["dtype"] != "F32":
            raise ValueError(f"{name} has dtype {metadata['dtype']}; expected F32")
        if metadata["shape"] != expected_shape:
            raise ValueError(f"{name} has shape {metadata['shape']}; expected {expected_shape}")
        start, end = metadata["data_offsets"]
        count = math.prod(expected_shape)
        if start < 0 or end - start != count * 4 or self.data_start + end > self.size:
            raise ValueError(f"{name} has invalid data offsets")
        with self.path.open("rb") as stream:
            stream.seek(self.data_start + start)
            data = stream.read(end - start)
        if len(data) != end - start:
            raise ValueError(f"{name} data is truncated")
        return struct.unpack(f"<{count}f", data)


def quantize_per_output(values, output_features, input_features):
    quantized = bytearray(input_features * output_features)
    scales = []
    max_error = 0.0
    squared_error = 0.0
    source_squared = 0.0

    for output_index in range(output_features):
        source_start = output_index * input_features
        source = values[source_start:source_start + input_features]
        maximum = max(abs(value) for value in source)
        scale = maximum / 127.0 if maximum != 0.0 else 1.0
        scales.append(scale)
        for input_index, value in enumerate(source):
            integer = max(-127, min(127, round(value / scale)))
            quantized[output_index * input_features + input_index] = integer & 0xFF
            error = value - integer * scale
            max_error = max(max_error, abs(error))
            squared_error += error * error
            source_squared += value * value

    return quantized, scales, {
        "max_absolute_error": max_error,
        "relative_l2_error": math.sqrt(squared_error / source_squared),
    }


def write_f32(path, values):
    with path.open("wb") as stream:
        stream.write(struct.pack(f"<{len(values)}f", *values))


def file_record(path, dtype, shape):
    return {
        "file": path.name,
        "dtype": dtype,
        "shape": shape,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def prepare(model_path, output_dir, layer):
    if layer not in range(ENCODER_LAYER_COUNT):
        raise ValueError(f"encoder layer must be between 0 and {ENCODER_LAYER_COUNT - 1}")
    if model_path.stat().st_size != MODEL_SIZE:
        raise ValueError(f"model size is {model_path.stat().st_size}; expected {MODEL_SIZE}")
    model_hash = sha256_file(model_path)
    if model_hash != MODEL_SHA256:
        raise ValueError(f"model SHA-256 is {model_hash}; expected {MODEL_SHA256}")

    model = SafeTensorsFile(model_path)
    prefix = f"model.encoder.layers.{layer}"
    tensors = {
        "q_proj": {
            "weight": tuple(
                value * ATTENTION_SCALE
                for value in model.read_f32(f"{prefix}.self_attn.q_proj.weight", [384, 384])
            ),
            "bias": tuple(
                value * ATTENTION_SCALE
                for value in model.read_f32(f"{prefix}.self_attn.q_proj.bias", [384])
            ),
            "source_weight": f"{prefix}.self_attn.q_proj.weight",
            "source_bias": f"{prefix}.self_attn.q_proj.bias",
            "source_transform": "multiply weight and bias by 0.125 attention scale",
            "input_features": 384,
            "output_features": 384,
        },
        "k_proj": {
            "weight": model.read_f32(f"{prefix}.self_attn.k_proj.weight", [384, 384]),
            "bias": (0.0,) * 384,
            "source_weight": f"{prefix}.self_attn.k_proj.weight",
            "source_bias": None,
            "input_features": 384,
            "output_features": 384,
        },
        "v_proj": {
            "weight": model.read_f32(f"{prefix}.self_attn.v_proj.weight", [384, 384]),
            "bias": model.read_f32(f"{prefix}.self_attn.v_proj.bias", [384]),
            "source_weight": f"{prefix}.self_attn.v_proj.weight",
            "source_bias": f"{prefix}.self_attn.v_proj.bias",
            "input_features": 384,
            "output_features": 384,
        },
        "out_proj": {
            "weight": model.read_f32(f"{prefix}.self_attn.out_proj.weight", [384, 384]),
            "bias": model.read_f32(f"{prefix}.self_attn.out_proj.bias", [384]),
            "source_weight": f"{prefix}.self_attn.out_proj.weight",
            "source_bias": f"{prefix}.self_attn.out_proj.bias",
            "input_features": 384,
            "output_features": 384,
        },
        "fc1": {
            "weight": model.read_f32(f"{prefix}.fc1.weight", [1536, 384]),
            "bias": model.read_f32(f"{prefix}.fc1.bias", [1536]),
            "source_weight": f"{prefix}.fc1.weight",
            "source_bias": f"{prefix}.fc1.bias",
            "input_features": 384,
            "output_features": 1536,
        },
        "fc2": {
            "weight": model.read_f32(f"{prefix}.fc2.weight", [384, 1536]),
            "bias": model.read_f32(f"{prefix}.fc2.bias", [384]),
            "source_weight": f"{prefix}.fc2.weight",
            "source_bias": f"{prefix}.fc2.bias",
            "input_features": 1536,
            "output_features": 384,
        },
    }

    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_tensors = {}
    for name, tensor in tensors.items():
        quantized, scales, error = quantize_per_output(
            tensor["weight"], tensor["output_features"], tensor["input_features"]
        )
        weight_path = output_dir / f"{name}-weight-int8.bin"
        scale_path = output_dir / f"{name}-scale-f32.bin"
        bias_path = output_dir / f"{name}-bias-f32.bin"
        weight_path.write_bytes(quantized)
        write_f32(scale_path, scales)
        write_f32(bias_path, tensor["bias"])
        manifest_tensors[name] = {
            "source_weight": tensor["source_weight"],
            "source_bias": tensor["source_bias"],
            "weight": file_record(
                weight_path,
                "INT8",
                [tensor["output_features"], tensor["input_features"]],
            ),
            "weight_layout": "output_features,input_features",
            "weight_quantization": {
                "scheme": "symmetric_per_output_channel",
                "axis": 0,
                "zero_point": 0,
                "rounding": "nearest_ties_to_even",
                "scale": file_record(scale_path, "F32", [tensor["output_features"]]),
            },
            "bias": file_record(bias_path, "F32", [tensor["output_features"]]),
            "reconstruction": error,
        }
        if "source_transform" in tensor:
            manifest_tensors[name]["source_transform"] = tensor["source_transform"]

    manifest = {
        "format": "newos.whisper_tiny.encoder_layer.v3",
        "source": {
            "model": "openai/whisper-tiny",
            "revision": MODEL_REVISION,
            "file": model_path.name,
            "size": MODEL_SIZE,
            "sha256": MODEL_SHA256,
            "license": "Apache-2.0",
        },
        "encoder_layer": layer,
        "tensors": manifest_tensors,
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii")
    return manifest


def main():
    script_dir = Path(__file__).resolve().parent
    snapdragon_dir = script_dir.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model",
        type=Path,
        default=snapdragon_dir / "models/whisper-tiny/model.safetensors",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="output directory (defaults to the selected layer's model directory)",
    )
    parser.add_argument("--layer", type=int, choices=range(ENCODER_LAYER_COUNT), default=0)
    args = parser.parse_args()
    output = args.output or (
        snapdragon_dir / f"models/whisper-tiny/encoder-layer-{args.layer}-mlp-int8"
    )
    manifest = prepare(args.model.resolve(), output.resolve(), args.layer)
    print(f"Prepared encoder layer {manifest['encoder_layer']} weights in {output.resolve()}")
    for name, tensor in manifest["tensors"].items():
        error = tensor["reconstruction"]
        print(
            f"  {name}: {tensor['weight']['shape']}, "
            f"max error {error['max_absolute_error']:.8g}, "
            f"relative L2 {error['relative_l2_error']:.8g}"
        )


if __name__ == "__main__":
    main()