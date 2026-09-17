#!/usr/bin/env python3
"""Calibrate one Whisper Tiny encoder layer with NumPy."""

import argparse
import gc
import hashlib
import json
import math
import shutil
import struct
import tempfile
import time
from pathlib import Path

import numpy as np


MODEL_REVISION = "169d4a4341b33bc18d8881c4b69c2e104e1cc0af"
MODEL_SHA256 = "7ebd0e69e78190ffe1438491fa05cc1f5c1aa3a4c4db3bc1723adbb551ea2395"
CORPUS_REVISION = "70bb2e84b976b7e960aa89f1c648e09c59f894dd"
SAMPLE_RATE = 16000
N_SAMPLES = 480000
N_FFT = 400
HOP_LENGTH = 160
MEL_BINS = 80
HIDDEN_SIZE = 384
FFN_SIZE = 1536
HEADS = 6
ENCODER_LAYER_COUNT = 4
LAYER_NORM_EPSILON = 1e-5
SAMPLE_STRIDE = 17
MAX_SAMPLES_PER_UPDATE = 32768
ATTENTION_EVALUATION_CLIPS = 4


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

    def f32(self, name, shape):
        try:
            metadata = self.header[name]
        except KeyError as error:
            raise ValueError(f"SafeTensors tensor is missing: {name}") from error
        if metadata["dtype"] != "F32" or metadata["shape"] != list(shape):
            raise ValueError(
                f"{name} is {metadata['dtype']} {metadata['shape']}; expected F32 {list(shape)}"
            )
        start, end = metadata["data_offsets"]
        count = math.prod(shape)
        if end - start != count * 4 or self.data_start + end > self.size:
            raise ValueError(f"{name} has invalid data offsets")
        return np.fromfile(
            self.path,
            dtype="<f4",
            count=count,
            offset=self.data_start + start,
        ).reshape(shape)


def load_weights(model_path, target_layer):
    model = SafeTensorsFile(model_path)
    shapes = {
        "model.encoder.conv1.weight": (384, 80, 3),
        "model.encoder.conv1.bias": (384,),
        "model.encoder.conv2.weight": (384, 384, 3),
        "model.encoder.conv2.bias": (384,),
        "model.encoder.embed_positions.weight": (1500, 384),
    }
    for layer in range(target_layer + 1):
        prefix = f"model.encoder.layers.{layer}"
        shapes.update({
            f"{prefix}.self_attn_layer_norm.weight": (384,),
            f"{prefix}.self_attn_layer_norm.bias": (384,),
            f"{prefix}.self_attn.q_proj.weight": (384, 384),
            f"{prefix}.self_attn.q_proj.bias": (384,),
            f"{prefix}.self_attn.k_proj.weight": (384, 384),
            f"{prefix}.self_attn.v_proj.weight": (384, 384),
            f"{prefix}.self_attn.v_proj.bias": (384,),
            f"{prefix}.self_attn.out_proj.weight": (384, 384),
            f"{prefix}.self_attn.out_proj.bias": (384,),
            f"{prefix}.final_layer_norm.weight": (384,),
            f"{prefix}.final_layer_norm.bias": (384,),
            f"{prefix}.fc1.weight": (1536, 384),
            f"{prefix}.fc1.bias": (1536,),
            f"{prefix}.fc2.weight": (384, 1536),
            f"{prefix}.fc2.bias": (384,),
        })
    return {name: model.f32(name, shape) for name, shape in shapes.items()}


def load_float32_wav(path):
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a RIFF WAVE file")
    offset = 12
    format_info = None
    samples = None
    while offset + 8 <= len(data):
        chunk_type = data[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        if chunk_end > len(data):
            raise ValueError(f"{path} has a truncated RIFF chunk")
        if chunk_type == b"fmt ":
            format_info = struct.unpack_from("<HHIIHH", data, chunk_start)
        elif chunk_type == b"data":
            samples = np.frombuffer(data, dtype="<f4", offset=chunk_start, count=chunk_size // 4).copy()
        offset = chunk_end + (chunk_size & 1)
    if format_info is None or samples is None:
        raise ValueError(f"{path} is missing fmt or data")
    audio_format, channels, rate, _byte_rate, block_align, bits = format_info
    if (audio_format, channels, rate, block_align, bits) != (3, 1, SAMPLE_RATE, 4, 32):
        raise ValueError(f"{path} is not 16 kHz mono IEEE float32")
    if not np.isfinite(samples).all():
        raise ValueError(f"{path} contains non-finite samples")
    return samples


def mel_filter_bank():
    min_log_hertz = 1000.0
    min_log_mel = 15.0
    logstep = np.log(6.4) / 27.0
    mel_min = 0.0
    mel_max = min_log_mel + np.log(8000.0 / min_log_hertz) / logstep
    mels = np.linspace(mel_min, mel_max, MEL_BINS + 2)
    frequencies = 200.0 * mels / 3.0
    logarithmic = mels >= min_log_mel
    frequencies[logarithmic] = min_log_hertz * np.exp(
        logstep * (mels[logarithmic] - min_log_mel)
    )
    fft_frequencies = np.linspace(0.0, SAMPLE_RATE // 2, N_FFT // 2 + 1)
    differences = np.diff(frequencies)
    slopes = frequencies[np.newaxis, :] - fft_frequencies[:, np.newaxis]
    down = -slopes[:, :-2] / differences[:-1]
    up = slopes[:, 2:] / differences[1:]
    filters = np.maximum(0.0, np.minimum(down, up))
    filters *= 2.0 / (frequencies[2:] - frequencies[:-2])
    return filters


def log_mel_spectrogram(samples):
    waveform = np.zeros(N_SAMPLES, dtype=np.float32)
    length = min(samples.size, N_SAMPLES)
    waveform[:length] = samples[:length]
    waveform = np.pad(waveform, N_FFT // 2, mode="reflect").astype(np.float64)
    frames = np.lib.stride_tricks.sliding_window_view(waveform, N_FFT)[::HOP_LENGTH]
    window = np.hanning(N_FFT + 1)[:-1]
    spectrum = np.fft.rfft(frames * window, axis=1).astype(np.complex64)
    magnitudes = np.abs(spectrum, dtype=np.float64) ** 2
    mel = np.maximum(1e-10, mel_filter_bank().T @ magnitudes.T)
    log_spec = np.log10(mel)[:, :-1]
    log_spec = np.maximum(log_spec, log_spec.max() - 8.0)
    return np.asarray((log_spec + 4.0) / 4.0, dtype=np.float32)


def gelu(values):
    absolute = np.abs(values / np.float32(math.sqrt(2.0)))
    coefficient = np.float32(0.3275911)
    factor = np.float32(1.0) / (np.float32(1.0) + coefficient * absolute)
    polynomial = np.float32(1.061405429) * factor + np.float32(-1.453152027)
    polynomial = polynomial * factor + np.float32(1.421413741)
    polynomial = polynomial * factor + np.float32(-0.284496736)
    polynomial = polynomial * factor + np.float32(0.254829592)
    erf = np.float32(1.0) - polynomial * factor * np.exp(-absolute * absolute)
    erf = np.copysign(erf, values)
    return values * np.float32(0.5) * (np.float32(1.0) + erf)


def conv1d(inputs, weight, bias, stride):
    padded = np.pad(inputs, ((0, 0), (1, 1)))
    windows = np.lib.stride_tricks.sliding_window_view(padded, 3, axis=1)
    matrix = windows.transpose(1, 0, 2).reshape(windows.shape[1], -1)
    return matrix[::stride] @ weight.reshape(weight.shape[0], -1).T + bias


def layer_norm(values, weight, bias):
    mean = values.mean(axis=-1, keepdims=True)
    variance = ((values - mean) ** 2).mean(axis=-1, keepdims=True)
    return (values - mean) / np.sqrt(variance + LAYER_NORM_EPSILON) * weight + bias


def encoder_frontend(features, weights):
    conv1 = gelu(conv1d(features, weights["model.encoder.conv1.weight"], weights["model.encoder.conv1.bias"], 1))
    conv2 = gelu(conv1d(conv1.T, weights["model.encoder.conv2.weight"], weights["model.encoder.conv2.bias"], 2))
    return conv2 + weights["model.encoder.embed_positions.weight"]


def encoder_layer(hidden, weights, layer, attention_statistics=None):
    prefix = f"model.encoder.layers.{layer}"
    normalized = layer_norm(
        hidden,
        weights[f"{prefix}.self_attn_layer_norm.weight"],
        weights[f"{prefix}.self_attn_layer_norm.bias"],
    )
    query = normalized @ weights[f"{prefix}.self_attn.q_proj.weight"].T
    query += weights[f"{prefix}.self_attn.q_proj.bias"]
    query *= np.float32((HIDDEN_SIZE // HEADS) ** -0.5)
    key = normalized @ weights[f"{prefix}.self_attn.k_proj.weight"].T
    value = normalized @ weights[f"{prefix}.self_attn.v_proj.weight"].T
    value += weights[f"{prefix}.self_attn.v_proj.bias"]
    query_output = query
    key_output = key
    value_output = value
    query = query.reshape(1500, HEADS, HIDDEN_SIZE // HEADS)
    key = key.reshape(1500, HEADS, HIDDEN_SIZE // HEADS)
    value = value.reshape(1500, HEADS, HIDDEN_SIZE // HEADS)
    attended = np.empty_like(query)
    for head in range(HEADS):
        scores = query[:, head, :] @ key[:, head, :].T
        if attention_statistics is not None:
            attention_statistics["attention_scores"].add(scores)
        scores -= scores.max(axis=-1, keepdims=True)
        np.exp(scores, out=scores)
        scores /= scores.sum(axis=-1, keepdims=True)
        if attention_statistics is not None:
            attention_statistics["attention_weights"].add(scores)
        attended[:, head, :] = scores @ value[:, head, :]
    attention_output = attended.reshape(1500, HIDDEN_SIZE)
    attention_output = attention_output @ weights[f"{prefix}.self_attn.out_proj.weight"].T
    attention_output += weights[f"{prefix}.self_attn.out_proj.bias"]
    attention_residual = hidden + attention_output
    mlp_input = layer_norm(
        attention_residual,
        weights[f"{prefix}.final_layer_norm.weight"],
        weights[f"{prefix}.final_layer_norm.bias"],
    )
    fc1_output = mlp_input @ weights[f"{prefix}.fc1.weight"].T
    fc1_output += weights[f"{prefix}.fc1.bias"]
    gelu_output = gelu(fc1_output)
    fc2_output = gelu_output @ weights[f"{prefix}.fc2.weight"].T
    fc2_output += weights[f"{prefix}.fc2.bias"]
    block_output = attention_residual + fc2_output
    return {
        "encoder_input": hidden,
        "attention_input": normalized,
        "q_proj_output": query_output,
        "k_proj_output": key_output,
        "v_proj_output": value_output,
        "attended_output": attended.reshape(1500, HIDDEN_SIZE),
        "attention_output": attention_output,
        "attention_residual": attention_residual,
        "fc1_input": mlp_input,
        "fc1_output": fc1_output,
        "gelu_output": gelu_output,
        "fc2_output": fc2_output,
        "block_output": block_output,
    }


class RangeStats:
    def __init__(self, maximum_samples_per_update=0):
        self.minimum = math.inf
        self.maximum = -math.inf
        self.count = 0
        self.samples = []
        self.maximum_samples_per_update = maximum_samples_per_update

    def add(self, values):
        if not np.isfinite(values).all():
            raise ValueError("activation contains non-finite values")
        self.minimum = min(self.minimum, float(values.min()))
        self.maximum = max(self.maximum, float(values.max()))
        self.count += values.size
        stride = SAMPLE_STRIDE
        if self.maximum_samples_per_update:
            stride = max(stride, math.ceil(values.size / self.maximum_samples_per_update))
        self.samples.append(values.reshape(-1)[::stride].copy())

    @staticmethod
    def encoding(minimum, maximum):
        minimum = min(minimum, 0.0)
        maximum = max(maximum, 0.0)
        scale = (maximum - minimum) / 255.0
        zero_point = max(0, min(255, round(-minimum / scale)))
        return {
            "scale": scale,
            "zero_point": zero_point,
            "qnn_offset": -zero_point,
        }

    def summarize(self):
        sample = np.concatenate(self.samples)
        clip_minimum, clip_maximum = np.quantile(sample, [0.0001, 0.9999])
        result = {
            "count": self.count,
            "minimum_sample_stride": SAMPLE_STRIDE,
            "sample_count": sample.size,
            "observed_minimum": self.minimum,
            "observed_maximum": self.maximum,
            "minmax_uint8": self.encoding(self.minimum, self.maximum),
            "percentile_0.01_99.99_uint8": {
                "clip_minimum": float(clip_minimum),
                "clip_maximum": float(clip_maximum),
                "sampled_low_saturation_fraction": float(np.mean(sample < clip_minimum)),
                "sampled_high_saturation_fraction": float(np.mean(sample > clip_maximum)),
                **self.encoding(float(clip_minimum), float(clip_maximum)),
            },
        }
        if self.maximum_samples_per_update:
            result["maximum_samples_per_update"] = self.maximum_samples_per_update
        return result


class ErrorStats:
    def __init__(self):
        self.count = 0
        self.maximum = 0.0
        self.absolute_sum = 0.0
        self.squared_sum = 0.0
        self.reference_squared_sum = 0.0

    def add(self, actual, reference):
        error = actual.astype(np.float64) - reference.astype(np.float64)
        self.count += error.size
        self.maximum = max(self.maximum, float(np.max(np.abs(error))))
        self.absolute_sum += float(np.sum(np.abs(error)))
        self.squared_sum += float(np.sum(error * error))
        self.reference_squared_sum += float(np.sum(reference.astype(np.float64) ** 2))

    def summarize(self):
        return {
            "count": self.count,
            "maximum_absolute_error": self.maximum,
            "mean_absolute_error": self.absolute_sum / self.count,
            "root_mean_square_error": math.sqrt(self.squared_sum / self.count),
            "relative_l2_error": math.sqrt(self.squared_sum / self.reference_squared_sum),
        }


def load_prepared_weights(directory, expected_layer):
    manifest_path = directory / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="ascii"))
    if manifest["source"]["sha256"] != MODEL_SHA256:
        raise ValueError("prepared weights do not reference the pinned Whisper Tiny checkpoint")
    if manifest["encoder_layer"] != expected_layer:
        raise ValueError(
            f"prepared weights are for encoder layer {manifest['encoder_layer']}; "
            f"expected layer {expected_layer}"
        )
    result = {}
    dimensions_by_name = {
        "q_proj": (384, 384),
        "k_proj": (384, 384),
        "v_proj": (384, 384),
        "out_proj": (384, 384),
        "fc1": (1536, 384),
        "fc2": (384, 1536),
    }
    for name, dimensions in dimensions_by_name.items():
        tensor = manifest["tensors"][name]
        weight_path = directory / tensor["weight"]["file"]
        scale_path = directory / tensor["weight_quantization"]["scale"]["file"]
        bias_path = directory / tensor["bias"]["file"]
        for path, metadata in (
            (weight_path, tensor["weight"]),
            (scale_path, tensor["weight_quantization"]["scale"]),
            (bias_path, tensor["bias"]),
        ):
            if sha256_file(path) != metadata["sha256"]:
                raise ValueError(f"prepared weight artifact SHA-256 mismatch: {path}")
        quantized = np.fromfile(weight_path, dtype=np.int8).reshape(dimensions)
        scales = np.fromfile(scale_path, dtype="<f4")
        result[name] = {
            "matrix": (quantized.astype(np.float32) * scales[:, np.newaxis]).T,
            "scales": scales,
            "bias": np.fromfile(bias_path, dtype="<f4"),
        }
    return result


def quantize_uint8(values, encoding):
    quantized = np.rint(values / encoding["scale"] + encoding["zero_point"])
    np.clip(quantized, 0, 255, out=quantized)
    return quantized.astype(np.uint8)


def dequantize_uint8(values, encoding):
    return (values.astype(np.float32) - encoding["zero_point"]) * encoding["scale"]


def quantized_layer_norm(values, input_encoding, output_encoding, weight, bias):
    scale_encoding = RangeStats.encoding(float(weight.min()), float(weight.max()))
    quantized_scale = quantize_uint8(weight, scale_encoding)
    dequantized_scale = dequantize_uint8(quantized_scale, scale_encoding)
    bias_scale = input_encoding["scale"] * scale_encoding["scale"]
    quantized_bias = np.rint(bias / bias_scale).astype(np.int32)
    dequantized_bias = quantized_bias.astype(np.float32) * bias_scale
    normalized = layer_norm(
        dequantize_uint8(values, input_encoding), dequantized_scale, dequantized_bias
    )
    quantized = quantize_uint8(normalized, output_encoding)
    return quantized, dequantize_uint8(quantized, output_encoding)


def quantized_encoder_block(values, encodings, weights, prepared_weights, layer):
    prefix = f"model.encoder.layers.{layer}"
    attention_input_quantized, attention_input = quantized_layer_norm(
        values,
        encodings["encoder_input"],
        encodings["attention_input"],
        weights[f"{prefix}.self_attn_layer_norm.weight"],
        weights[f"{prefix}.self_attn_layer_norm.bias"],
    )
    del attention_input_quantized
    projections = {}
    for name in ("q_proj", "k_proj", "v_proj"):
        output_name = f"{name}_output"
        projected = attention_input @ prepared_weights[name]["matrix"]
        projected += prepared_weights[name]["bias"]
        projections[output_name] = dequantize_uint8(
            quantize_uint8(projected, encodings[output_name]), encodings[output_name]
        )
    query = projections["q_proj_output"].reshape(1500, HEADS, HIDDEN_SIZE // HEADS)
    key = projections["k_proj_output"].reshape(1500, HEADS, HIDDEN_SIZE // HEADS)
    value = projections["v_proj_output"].reshape(1500, HEADS, HIDDEN_SIZE // HEADS)
    attended = np.empty_like(query)
    for head in range(HEADS):
        scores = query[:, head, :] @ key[:, head, :].T
        scores = dequantize_uint8(
            quantize_uint8(scores, encodings["attention_scores"]),
            encodings["attention_scores"],
        )
        scores -= scores.max(axis=-1, keepdims=True)
        np.exp(scores, out=scores)
        scores /= scores.sum(axis=-1, keepdims=True)
        scores = dequantize_uint8(
            quantize_uint8(scores, encodings["attention_weights"]),
            encodings["attention_weights"],
        )
        attended[:, head, :] = scores @ value[:, head, :]
    attended = dequantize_uint8(
        quantize_uint8(attended, encodings["attended_output"]),
        encodings["attended_output"],
    ).reshape(1500, HIDDEN_SIZE)
    attention_output = attended @ prepared_weights["out_proj"]["matrix"]
    attention_output += prepared_weights["out_proj"]["bias"]
    attention_output = quantize_uint8(attention_output, encodings["attention_output"])
    attention_residual = dequantize_uint8(values, encodings["encoder_input"])
    attention_residual += dequantize_uint8(
        attention_output, encodings["attention_output"]
    )
    attention_residual = quantize_uint8(
        attention_residual, encodings["attention_residual"]
    )
    fc1_input_quantized, fc1_input = quantized_layer_norm(
        attention_residual,
        encodings["attention_residual"],
        encodings["fc1_input"],
        weights[f"{prefix}.final_layer_norm.weight"],
        weights[f"{prefix}.final_layer_norm.bias"],
    )
    del fc1_input_quantized
    fc1_output = fc1_input @ prepared_weights["fc1"]["matrix"]
    fc1_output += prepared_weights["fc1"]["bias"]
    fc1_output = dequantize_uint8(
        quantize_uint8(fc1_output, encodings["fc1_output"]),
        encodings["fc1_output"],
    )
    gelu_output = dequantize_uint8(
        quantize_uint8(gelu(fc1_output), encodings["gelu_output"]),
        encodings["gelu_output"],
    )
    fc2_output = gelu_output @ prepared_weights["fc2"]["matrix"]
    fc2_output += prepared_weights["fc2"]["bias"]
    fc2_output = quantize_uint8(fc2_output, encodings["fc2_output"])
    output = dequantize_uint8(attention_residual, encodings["attention_residual"])
    output += dequantize_uint8(fc2_output, encodings["fc2_output"])
    return quantize_uint8(output, encodings["block_output"])


def write_encoder_stack_fixture(model_path, corpus_manifest_path, model_root, output_dir):
    if sha256_file(model_path) != MODEL_SHA256:
        raise ValueError("model SHA-256 does not match the pinned Whisper Tiny checkpoint")
    corpus = json.loads(corpus_manifest_path.read_text(encoding="utf-8"))
    if corpus["revision"] != CORPUS_REVISION:
        raise ValueError("calibration corpus revision does not match the pinned FLEURS revision")
    clip = corpus["clips"][0]
    audio_path = corpus_manifest_path.parent / clip["file"]
    if sha256_file(audio_path) != clip["sha256"]:
        raise ValueError(f"calibration audio SHA-256 mismatch: {audio_path}")
    weights = load_weights(model_path, ENCODER_LAYER_COUNT - 1)
    float_hidden = encoder_frontend(
        log_mel_spectrogram(load_float32_wav(audio_path)), weights
    )
    encoder_input = None
    quantized = None
    previous_output_encoding = None
    layer_records = []
    layer_outputs = []
    for layer in range(ENCODER_LAYER_COUNT):
        directory = model_root / f"encoder-layer-{layer}-mlp-int8"
        deployment_path = directory / "deployment.json"
        deployment = json.loads(deployment_path.read_text(encoding="ascii"))
        if deployment.get("encoder_layer", 0) != layer:
            raise ValueError(f"deployment metadata does not match encoder layer {layer}")
        encodings = deployment["activations"]
        if previous_output_encoding is not None:
            for field in ("scale", "qnn_offset"):
                if previous_output_encoding[field] != encodings["encoder_input"][field]:
                    raise ValueError(f"encoder layer {layer} input encoding is discontinuous")
        if layer == 0:
            quantized = quantize_uint8(float_hidden, encodings["encoder_input"])
            encoder_input = quantized.copy()
        prepared_weights = load_prepared_weights(directory, layer)
        quantized = quantized_encoder_block(
            quantized, encodings, weights, prepared_weights, layer
        )
        layer_outputs.append(quantized.copy())
        float_hidden = encoder_layer(float_hidden, weights, layer)["block_output"]
        previous_output_encoding = encodings["block_output"]
        layer_records.append({
            "layer": layer,
            "deployment": deployment_path.name,
            "deployment_sha256": sha256_file(deployment_path),
        })
    output_dir.mkdir(parents=True, exist_ok=True)
    input_path = output_dir / "fixture-input-uint8.bin"
    output_path = output_dir / "fixture-output-uint8.bin"
    input_path.write_bytes(encoder_input.tobytes())
    output_path.write_bytes(quantized.tobytes())
    for layer, layer_output in enumerate(layer_outputs):
        layer_output_path = output_dir / f"fixture-layer-{layer}-output-uint8.bin"
        layer_output_path.write_bytes(layer_output.tobytes())
        layer_records[layer]["output"] = artifact_record(
            layer_output_path, "UINT8", (1500, 384)
        )
    error = ErrorStats()
    error.add(dequantize_uint8(quantized, previous_output_encoding), float_hidden)
    manifest = {
        "format": "newos.whisper_tiny.encoder_stack_deployment.v1",
        "model_revision": MODEL_REVISION,
        "source_config": clip["config"],
        "source_id": clip["id"],
        "source_sha256": clip["sha256"],
        "layers": layer_records,
        "input": artifact_record(input_path, "UINT8", (1500, 384)),
        "output": artifact_record(output_path, "UINT8", (1500, 384)),
        "output_encoding": previous_output_encoding,
        "quantized_stack_evaluation": error.summarize(),
    }
    manifest_path = output_dir / "deployment.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii")
    return manifest


def evaluate_quantization(clips, cache_dir, weights, prepared_weights, summaries, layer):
    schemes = ("minmax_uint8", "percentile_0.01_99.99_uint8")
    errors = {scheme: ErrorStats() for scheme in schemes}
    projection_names = ("q_proj", "k_proj", "v_proj")
    projection_errors = {
        scheme: {name: ErrorStats() for name in projection_names}
        for scheme in schemes
    }
    attention_scheme = "percentile_0.01_99.99_uint8"
    attention_errors = {attention_scheme: ErrorStats()}
    block_errors = {attention_scheme: ErrorStats()}
    fixture = None
    for index, clip in enumerate(clips, 1):
        references = {
            name: np.load(cache_dir / f"{index}-{name}.npy", mmap_mode="r")
            for name in (
                "attention_input",
                "q_proj_output",
                "k_proj_output",
                "v_proj_output",
                "fc1_input",
                "fc2_output",
            )
        }
        if index <= ATTENTION_EVALUATION_CLIPS:
            for name in ("encoder_input", "attended_output", "block_output"):
                references[name] = np.load(cache_dir / f"{index}-{name}.npy", mmap_mode="r")
        for scheme in schemes:
            attention_input_quantized = quantize_uint8(
                references["attention_input"], summaries["attention_input"][scheme]
            )
            attention_input = dequantize_uint8(
                attention_input_quantized, summaries["attention_input"][scheme]
            )
            projection_outputs = {}
            projection_dequantized = {}
            for name in projection_names:
                output_name = f"{name}_output"
                projection_output = attention_input @ prepared_weights[name]["matrix"]
                projection_output += prepared_weights[name]["bias"]
                output_quantized = quantize_uint8(projection_output, summaries[output_name][scheme])
                output_dequantized = dequantize_uint8(output_quantized, summaries[output_name][scheme])
                projection_errors[scheme][name].add(output_dequantized, references[output_name])
                projection_outputs[output_name] = output_quantized
                projection_dequantized[output_name] = output_dequantized
            if scheme == attention_scheme and index <= ATTENTION_EVALUATION_CLIPS:
                query = projection_dequantized["q_proj_output"].reshape(
                    1500, HEADS, HIDDEN_SIZE // HEADS
                )
                key = projection_dequantized["k_proj_output"].reshape(
                    1500, HEADS, HIDDEN_SIZE // HEADS
                )
                value = projection_dequantized["v_proj_output"].reshape(
                    1500, HEADS, HIDDEN_SIZE // HEADS
                )
                attended = np.empty_like(query)
                for head in range(HEADS):
                    scores = query[:, head, :] @ key[:, head, :].T
                    scores = dequantize_uint8(
                        quantize_uint8(scores, summaries["attention_scores"][scheme]),
                        summaries["attention_scores"][scheme],
                    )
                    scores -= scores.max(axis=-1, keepdims=True)
                    np.exp(scores, out=scores)
                    scores /= scores.sum(axis=-1, keepdims=True)
                    scores = dequantize_uint8(
                        quantize_uint8(scores, summaries["attention_weights"][scheme]),
                        summaries["attention_weights"][scheme],
                    )
                    attended[:, head, :] = scores @ value[:, head, :]
                attended_quantized = quantize_uint8(
                    attended, summaries["attended_output"][scheme]
                )
                attended_dequantized = dequantize_uint8(
                    attended_quantized, summaries["attended_output"][scheme]
                )
                attention_errors[scheme].add(
                    attended_dequantized.reshape(1500, HIDDEN_SIZE),
                    references["attended_output"],
                )
                prefix = f"model.encoder.layers.{layer}"
                encoder_input_quantized = quantize_uint8(
                    references["encoder_input"], summaries["encoder_input"][scheme]
                )
                block_attention_input_quantized, block_attention_input = quantized_layer_norm(
                    encoder_input_quantized,
                    summaries["encoder_input"][scheme],
                    summaries["attention_input"][scheme],
                    weights[f"{prefix}.self_attn_layer_norm.weight"],
                    weights[f"{prefix}.self_attn_layer_norm.bias"],
                )
                block_projections = {}
                for name in projection_names:
                    output_name = f"{name}_output"
                    projected = block_attention_input @ prepared_weights[name]["matrix"]
                    projected += prepared_weights[name]["bias"]
                    block_projections[output_name] = dequantize_uint8(
                        quantize_uint8(projected, summaries[output_name][scheme]),
                        summaries[output_name][scheme],
                    )
                block_query = block_projections["q_proj_output"].reshape(
                    1500, HEADS, HIDDEN_SIZE // HEADS
                )
                block_key = block_projections["k_proj_output"].reshape(
                    1500, HEADS, HIDDEN_SIZE // HEADS
                )
                block_value = block_projections["v_proj_output"].reshape(
                    1500, HEADS, HIDDEN_SIZE // HEADS
                )
                block_attended = np.empty_like(block_query)
                for head in range(HEADS):
                    block_scores = block_query[:, head, :] @ block_key[:, head, :].T
                    block_scores = dequantize_uint8(
                        quantize_uint8(block_scores, summaries["attention_scores"][scheme]),
                        summaries["attention_scores"][scheme],
                    )
                    block_scores -= block_scores.max(axis=-1, keepdims=True)
                    np.exp(block_scores, out=block_scores)
                    block_scores /= block_scores.sum(axis=-1, keepdims=True)
                    block_scores = dequantize_uint8(
                        quantize_uint8(block_scores, summaries["attention_weights"][scheme]),
                        summaries["attention_weights"][scheme],
                    )
                    block_attended[:, head, :] = block_scores @ block_value[:, head, :]
                block_attended_quantized = quantize_uint8(
                    block_attended, summaries["attended_output"][scheme]
                )
                block_attended_flat = dequantize_uint8(
                    block_attended_quantized, summaries["attended_output"][scheme]
                ).reshape(1500, HIDDEN_SIZE)
                block_attention_output = block_attended_flat @ prepared_weights["out_proj"]["matrix"]
                block_attention_output += prepared_weights["out_proj"]["bias"]
                block_attention_output_quantized = quantize_uint8(
                    block_attention_output, summaries["attention_output"][scheme]
                )
                block_attention_residual = dequantize_uint8(
                    encoder_input_quantized, summaries["encoder_input"][scheme]
                ) + dequantize_uint8(
                    block_attention_output_quantized, summaries["attention_output"][scheme]
                )
                block_attention_residual_quantized = quantize_uint8(
                    block_attention_residual, summaries["attention_residual"][scheme]
                )
                block_fc1_input_quantized, block_fc1_input = quantized_layer_norm(
                    block_attention_residual_quantized,
                    summaries["attention_residual"][scheme],
                    summaries["fc1_input"][scheme],
                    weights[f"{prefix}.final_layer_norm.weight"],
                    weights[f"{prefix}.final_layer_norm.bias"],
                )
                block_fc1 = block_fc1_input @ prepared_weights["fc1"]["matrix"]
                block_fc1 += prepared_weights["fc1"]["bias"]
                block_fc1 = dequantize_uint8(
                    quantize_uint8(block_fc1, summaries["fc1_output"][scheme]),
                    summaries["fc1_output"][scheme],
                )
                block_gelu = dequantize_uint8(
                    quantize_uint8(gelu(block_fc1), summaries["gelu_output"][scheme]),
                    summaries["gelu_output"][scheme],
                )
                block_fc2 = block_gelu @ prepared_weights["fc2"]["matrix"]
                block_fc2 += prepared_weights["fc2"]["bias"]
                block_fc2_quantized = quantize_uint8(
                    block_fc2, summaries["fc2_output"][scheme]
                )
                block_output = dequantize_uint8(
                    block_attention_residual_quantized,
                    summaries["attention_residual"][scheme],
                ) + dequantize_uint8(
                    block_fc2_quantized, summaries["fc2_output"][scheme]
                )
                block_output_quantized = quantize_uint8(
                    block_output, summaries["block_output"][scheme]
                )
                block_errors[scheme].add(
                    dequantize_uint8(block_output_quantized, summaries["block_output"][scheme]),
                    references["block_output"],
                )
            input_quantized = quantize_uint8(references["fc1_input"], summaries["fc1_input"][scheme])
            fc1_input = dequantize_uint8(input_quantized, summaries["fc1_input"][scheme])
            fc1_output = fc1_input @ prepared_weights["fc1"]["matrix"]
            fc1_output += prepared_weights["fc1"]["bias"]
            fc1_output = dequantize_uint8(
                quantize_uint8(fc1_output, summaries["fc1_output"][scheme]),
                summaries["fc1_output"][scheme],
            )
            gelu_output = dequantize_uint8(
                quantize_uint8(gelu(fc1_output), summaries["gelu_output"][scheme]),
                summaries["gelu_output"][scheme],
            )
            fc2_output = gelu_output @ prepared_weights["fc2"]["matrix"]
            fc2_output += prepared_weights["fc2"]["bias"]
            output_quantized = quantize_uint8(fc2_output, summaries["fc2_output"][scheme])
            output_dequantized = dequantize_uint8(output_quantized, summaries["fc2_output"][scheme])
            errors[scheme].add(output_dequantized, references["fc2_output"])
            if index == 1 and scheme == attention_scheme:
                fixture = {
                    "clip": clip,
                    "attention_input": attention_input_quantized,
                    **projection_outputs,
                    "attention_core_q": projection_outputs["q_proj_output"].reshape(
                        1500, HEADS, HIDDEN_SIZE // HEADS
                    ).transpose(1, 0, 2).copy(),
                    "attention_core_k": projection_outputs["k_proj_output"].reshape(
                        1500, HEADS, HIDDEN_SIZE // HEADS
                    ).transpose(1, 2, 0).copy(),
                    "attention_core_v": projection_outputs["v_proj_output"].reshape(
                        1500, HEADS, HIDDEN_SIZE // HEADS
                    ).transpose(1, 0, 2).copy(),
                    "attention_core_output": attended_quantized.transpose(1, 0, 2).copy(),
                    "encoder_block_input": encoder_input_quantized,
                    "encoder_block_attention_input": block_attention_input_quantized,
                    "encoder_block_output": block_output_quantized,
                    "input": input_quantized,
                    "output": output_quantized,
                }
        print(f"[quantization {index}/{len(clips)}] {clip['config']} {clip['language']}")
        del (
            references,
            attention_input_quantized,
            attention_input,
            projection_outputs,
            projection_dequantized,
            input_quantized,
            fc1_input,
            fc1_output,
            gelu_output,
            fc2_output,
            output_quantized,
            output_dequantized,
        )
        if index <= ATTENTION_EVALUATION_CLIPS:
            del (
                query, key, value, attended, scores, attended_quantized, attended_dequantized,
                encoder_input_quantized, block_attention_input_quantized, block_attention_input,
                block_projections, block_query, block_key, block_value, block_attended,
                block_scores, block_attended_quantized, block_attended_flat,
                block_attention_output, block_attention_output_quantized,
                block_attention_residual, block_attention_residual_quantized,
                block_fc1_input_quantized, block_fc1_input, block_fc1, block_gelu,
                block_fc2, block_fc2_quantized, block_output, block_output_quantized,
            )
        gc.collect()
    return (
        {scheme: stats.summarize() for scheme, stats in errors.items()},
        {
            scheme: {name: stats.summarize() for name, stats in scheme_errors.items()}
            for scheme, scheme_errors in projection_errors.items()
        },
        {scheme: stats.summarize() for scheme, stats in attention_errors.items()},
        {scheme: stats.summarize() for scheme, stats in block_errors.items()},
        fixture,
    )


def artifact_record(path, dtype, shape):
    return {
        "file": path.name,
        "dtype": dtype,
        "shape": list(shape),
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def write_fp16_frontend_bundle(model_path, corpus_manifest_path, output_dir):
    if sha256_file(model_path) != MODEL_SHA256:
        raise ValueError("model SHA-256 does not match the pinned Whisper Tiny checkpoint")
    corpus = json.loads(corpus_manifest_path.read_text(encoding="utf-8"))
    if corpus["revision"] != CORPUS_REVISION:
        raise ValueError("calibration corpus revision does not match the pinned FLEURS revision")
    clip = corpus["clips"][0]
    audio_path = corpus_manifest_path.parent / clip["file"]
    if sha256_file(audio_path) != clip["sha256"]:
        raise ValueError(f"calibration audio SHA-256 mismatch: {audio_path}")

    weights = load_weights(model_path, 0)
    log_mel = log_mel_spectrogram(load_float32_wav(audio_path))
    conv1 = gelu(conv1d(
        log_mel, weights["model.encoder.conv1.weight"],
        weights["model.encoder.conv1.bias"], 1
    ))
    padded_log_mel = np.pad(log_mel, ((0, 0), (1, 1)))
    conv1_input = np.lib.stride_tricks.sliding_window_view(
        padded_log_mel, 3, axis=1
    ).transpose(1, 0, 2).reshape(log_mel.shape[1], -1)
    output = gelu(conv1d(
        conv1.T, weights["model.encoder.conv2.weight"],
        weights["model.encoder.conv2.bias"], 2
    )) + weights["model.encoder.embed_positions.weight"]
    angles = -2.0 * np.pi * np.arange(N_FFT, dtype=np.float64) / N_FFT
    tensors = {
        "fixture-log-mel-f32.bin": (log_mel, "FLOAT32"),
        "fixture-conv1-input-fp16.bin": (conv1_input, "FLOAT16"),
        "fixture-conv1-output-fp16.bin": (conv1, "FLOAT16"),
        "fixture-output-fp16.bin": (output, "FLOAT16"),
        "hann-window-f64.bin": (np.hanning(N_FFT + 1)[:-1], "FLOAT64"),
        "dft-roots-f64.bin": (np.stack((np.cos(angles), np.sin(angles)), axis=1), "FLOAT64"),
        "mel-filters-f64.bin": (mel_filter_bank(), "FLOAT64"),
        "conv1-weight-fp16.bin": (weights["model.encoder.conv1.weight"], "FLOAT16"),
        "conv1-bias-fp16.bin": (weights["model.encoder.conv1.bias"], "FLOAT16"),
        "conv2-weight-fp16.bin": (weights["model.encoder.conv2.weight"], "FLOAT16"),
        "conv2-bias-fp16.bin": (weights["model.encoder.conv2.bias"], "FLOAT16"),
        "embed-positions-fp16.bin": (weights["model.encoder.embed_positions.weight"], "FLOAT16"),
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    records = {}
    for name, (values, dtype) in tensors.items():
        storage_dtype = {"FLOAT16": "<f2", "FLOAT32": "<f4", "FLOAT64": "<f8"}[dtype]
        stored = np.asarray(values, dtype=storage_dtype)
        path = output_dir / name
        stored.tofile(path)
        records[name] = artifact_record(path, dtype, stored.shape)
    manifest = {
        "format": "newos.whisper_tiny.frontend_fp16.v1",
        "model_revision": MODEL_REVISION,
        "model_sha256": MODEL_SHA256,
        "source_config": clip["config"],
        "source_file": clip["file"],
        "source_id": clip["id"],
        "source_sha256": clip["sha256"],
        "sample_rate": SAMPLE_RATE,
        "sample_count": N_SAMPLES,
        "n_fft": N_FFT,
        "hop_length": HOP_LENGTH,
        "mel_bins": MEL_BINS,
        "tensors": records,
    }
    manifest_path = output_dir / "deployment.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii")
    return manifest


def write_fp16_encoder_bundle(model_path, corpus_manifest_path, output_dir, layer):
    if layer not in range(ENCODER_LAYER_COUNT):
        raise ValueError(f"encoder layer must be between 0 and {ENCODER_LAYER_COUNT - 1}")
    if sha256_file(model_path) != MODEL_SHA256:
        raise ValueError("model SHA-256 does not match the pinned Whisper Tiny checkpoint")
    corpus = json.loads(corpus_manifest_path.read_text(encoding="utf-8"))
    if corpus["revision"] != CORPUS_REVISION:
        raise ValueError("calibration corpus revision does not match the pinned FLEURS revision")
    clip = corpus["clips"][0]
    audio_path = corpus_manifest_path.parent / clip["file"]
    if sha256_file(audio_path) != clip["sha256"]:
        raise ValueError(f"calibration audio SHA-256 mismatch: {audio_path}")

    weights = load_weights(model_path, layer)
    hidden = encoder_frontend(log_mel_spectrogram(load_float32_wav(audio_path)), weights)
    for previous_layer in range(layer):
        hidden = encoder_layer(hidden, weights, previous_layer)["block_output"]
    fixture = encoder_layer(hidden, weights, layer)
    prefix = f"model.encoder.layers.{layer}"
    tensors = {
        "fixture-input-fp16.bin": fixture["encoder_input"],
        "fixture-output-fp16.bin": fixture["block_output"],
        "self_attn_layer_norm-scale-fp16.bin": weights[f"{prefix}.self_attn_layer_norm.weight"],
        "self_attn_layer_norm-bias-fp16.bin": weights[f"{prefix}.self_attn_layer_norm.bias"],
        "q_proj-weight-fp16.bin": weights[f"{prefix}.self_attn.q_proj.weight"] * np.float32(0.125),
        "q_proj-bias-fp16.bin": weights[f"{prefix}.self_attn.q_proj.bias"] * np.float32(0.125),
        "k_proj-weight-fp16.bin": weights[f"{prefix}.self_attn.k_proj.weight"],
        "k_proj-bias-fp16.bin": np.zeros(HIDDEN_SIZE, dtype=np.float32),
        "v_proj-weight-fp16.bin": weights[f"{prefix}.self_attn.v_proj.weight"],
        "v_proj-bias-fp16.bin": weights[f"{prefix}.self_attn.v_proj.bias"],
        "out_proj-weight-fp16.bin": weights[f"{prefix}.self_attn.out_proj.weight"],
        "out_proj-bias-fp16.bin": weights[f"{prefix}.self_attn.out_proj.bias"],
        "final_layer_norm-scale-fp16.bin": weights[f"{prefix}.final_layer_norm.weight"],
        "final_layer_norm-bias-fp16.bin": weights[f"{prefix}.final_layer_norm.bias"],
        "fc1-weight-fp16.bin": weights[f"{prefix}.fc1.weight"],
        "fc1-bias-fp16.bin": weights[f"{prefix}.fc1.bias"],
        "fc2-weight-fp16.bin": weights[f"{prefix}.fc2.weight"],
        "fc2-bias-fp16.bin": weights[f"{prefix}.fc2.bias"],
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    records = {}
    for name, values in tensors.items():
        values = np.asarray(values, dtype="<f2")
        path = output_dir / name
        values.tofile(path)
        records[name] = artifact_record(path, "FLOAT16", values.shape)
    manifest = {
        "format": "newos.whisper_tiny.encoder_layer_fp16.v1",
        "model_revision": MODEL_REVISION,
        "model_sha256": MODEL_SHA256,
        "encoder_layer": layer,
        "source_config": clip["config"],
        "source_id": clip["id"],
        "source_sha256": clip["sha256"],
        "attention_scale": 0.125,
        "tensors": records,
    }
    manifest_path = output_dir / "deployment.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii")
    return manifest


def write_deployment_bundle(
    directory, fixture, weights, prepared_weights, summaries, calibration_path, layer
):
    encoding_name = "percentile_0.01_99.99_uint8"
    activation_encodings = {name: values[encoding_name] for name, values in summaries.items()}
    directory.mkdir(parents=True, exist_ok=True)
    encoding_path = directory / "activation-encodings-f32-i32.bin"
    input_path = directory / "fixture-input-uint8.bin"
    output_path = directory / "fixture-output-uint8.bin"
    attention_encoding_path = directory / "attention-projection-encodings-f32-i32.bin"
    attention_input_path = directory / "fixture-attention-input-uint8.bin"
    with encoding_path.open("wb") as stream:
        for name in ("fc1_input", "fc1_output", "gelu_output", "fc2_output"):
            encoding = activation_encodings[name]
            stream.write(struct.pack("<fi", encoding["scale"], encoding["qnn_offset"]))
    input_path.write_bytes(fixture["input"].tobytes())
    output_path.write_bytes(fixture["output"].tobytes())
    attention_encoding_order = (
        "attention_input",
        "q_proj_output",
        "k_proj_output",
        "v_proj_output",
    )
    with attention_encoding_path.open("wb") as stream:
        for name in attention_encoding_order:
            encoding = activation_encodings[name]
            stream.write(struct.pack("<fi", encoding["scale"], encoding["qnn_offset"]))
    attention_input_path.write_bytes(fixture["attention_input"].tobytes())
    attention_output_records = {}
    for name in ("q_proj", "k_proj", "v_proj"):
        path = directory / f"fixture-{name}-output-uint8.bin"
        path.write_bytes(fixture[f"{name}_output"].tobytes())
        attention_output_records[name] = artifact_record(path, "UINT8", (1500, 384))
    attention_core_encoding_order = (
        "q_proj_output",
        "k_proj_output",
        "v_proj_output",
        "attention_scores",
        "attention_weights",
        "attended_output",
    )
    attention_core_encoding_path = directory / "attention-core-encodings-f32-i32.bin"
    with attention_core_encoding_path.open("wb") as stream:
        for name in attention_core_encoding_order:
            encoding = activation_encodings[name]
            stream.write(struct.pack("<fi", encoding["scale"], encoding["qnn_offset"]))
    attention_core_paths = {
        "q": directory / "fixture-attention-core-q-uint8.bin",
        "k": directory / "fixture-attention-core-k-uint8.bin",
        "v": directory / "fixture-attention-core-v-uint8.bin",
        "output": directory / "fixture-attention-core-output-uint8.bin",
    }
    for name, path in attention_core_paths.items():
        path.write_bytes(fixture[f"attention_core_{name}"].tobytes())
    block_encoding_order = (
        "encoder_input",
        "attention_input",
        "q_proj_output",
        "k_proj_output",
        "v_proj_output",
        "attention_scores",
        "attention_weights",
        "attended_output",
        "attention_output",
        "attention_residual",
        "fc1_input",
        "fc1_output",
        "gelu_output",
        "fc2_output",
        "block_output",
    )
    block_encoding_path = directory / "encoder-block-encodings-f32-i32.bin"
    with block_encoding_path.open("wb") as stream:
        for name in block_encoding_order:
            encoding = activation_encodings[name]
            stream.write(struct.pack("<fi", encoding["scale"], encoding["qnn_offset"]))
    block_input_path = directory / "fixture-encoder-block-input-uint8.bin"
    block_attention_input_path = directory / "fixture-encoder-block-attention-input-uint8.bin"
    block_output_path = directory / "fixture-encoder-block-output-uint8.bin"
    block_input_path.write_bytes(fixture["encoder_block_input"].tobytes())
    block_attention_input_path.write_bytes(fixture["encoder_block_attention_input"].tobytes())
    block_output_path.write_bytes(fixture["encoder_block_output"].tobytes())
    bias_records = {}
    for name, input_name in (
        ("q_proj", "attention_input"),
        ("k_proj", "attention_input"),
        ("v_proj", "attention_input"),
        ("out_proj", "attended_output"),
        ("fc1", "fc1_input"),
        ("fc2", "gelu_output"),
    ):
        bias = prepared_weights[name]["bias"]
        bias_scales = activation_encodings[input_name]["scale"] * prepared_weights[name]["scales"]
        quantized_bias = np.rint(bias / bias_scales).astype("<i4")
        bias_path = directory / f"{name}-bias-int32.bin"
        quantized_bias.tofile(bias_path)
        bias_records[name] = {
            **artifact_record(bias_path, "INT32", bias.shape),
            "quantization": {
                "scheme": "symmetric_per_output_channel",
                "axis": 0,
                "scale": f"{input_name}.scale * {name}.weight.scale",
                "zero_point": 0,
            },
        }
    normalization_records = {}
    prefix = f"model.encoder.layers.{layer}"
    for name, input_name, weight_name, bias_name in (
        (
            "self_attn_layer_norm",
            "encoder_input",
            f"{prefix}.self_attn_layer_norm.weight",
            f"{prefix}.self_attn_layer_norm.bias",
        ),
        (
            "final_layer_norm",
            "attention_residual",
            f"{prefix}.final_layer_norm.weight",
            f"{prefix}.final_layer_norm.bias",
        ),
    ):
        scale_values = weights[weight_name]
        scale_encoding = RangeStats.encoding(float(scale_values.min()), float(scale_values.max()))
        quantized_scale = quantize_uint8(scale_values, scale_encoding)
        bias_scale = activation_encodings[input_name]["scale"] * scale_encoding["scale"]
        quantized_bias = np.rint(weights[bias_name] / bias_scale).astype("<i4")
        scale_path = directory / f"{name}-scale-uint8.bin"
        bias_path = directory / f"{name}-bias-int32.bin"
        encoding_path = directory / f"{name}-scale-encoding-f32-i32.bin"
        scale_path.write_bytes(quantized_scale.tobytes())
        quantized_bias.tofile(bias_path)
        encoding_path.write_bytes(struct.pack("<fi", scale_encoding["scale"], scale_encoding["qnn_offset"]))
        normalization_records[name] = {
            "scale": artifact_record(scale_path, "UINT8", scale_values.shape),
            "scale_encoding": artifact_record(encoding_path, "F32,I32", (1,)),
            "bias": {
                **artifact_record(bias_path, "INT32", weights[bias_name].shape),
                "quantization_scale": f"{input_name}.scale * {name}.scale",
            },
            "epsilon": LAYER_NORM_EPSILON,
            "axes": [-1],
        }
    deployment = {
        "format": "newos.whisper_tiny.encoder_layer_deployment.v2",
        "model_revision": MODEL_REVISION,
        "encoder_layer": layer,
        "calibration": {
            "file": calibration_path.name,
            "sha256": sha256_file(calibration_path),
            "encoding": encoding_name,
        },
        "activations": activation_encodings,
        "activation_encoding_table": {
            **artifact_record(encoding_path, "F32,I32", (4,)),
            "order": ["fc1_input", "fc1_output", "gelu_output", "fc2_output"],
        },
        "attention_projection_activation_encoding_table": {
            **artifact_record(attention_encoding_path, "F32,I32", (4,)),
            "order": list(attention_encoding_order),
        },
        "fixture": {
            "source_config": fixture["clip"]["config"],
            "source_id": fixture["clip"]["id"],
            "source_sha256": fixture["clip"]["sha256"],
            "input": artifact_record(input_path, "UINT8", (1500, 384)),
            "output": artifact_record(output_path, "UINT8", (1500, 384)),
        },
        "attention_projection_fixture": {
            "source_config": fixture["clip"]["config"],
            "source_id": fixture["clip"]["id"],
            "source_sha256": fixture["clip"]["sha256"],
            "input": artifact_record(attention_input_path, "UINT8", (1500, 384)),
            "outputs": attention_output_records,
        },
        "attention_core_activation_encoding_table": {
            **artifact_record(attention_core_encoding_path, "F32,I32", (6,)),
            "order": list(attention_core_encoding_order),
        },
        "attention_core_fixture": {
            "source_config": fixture["clip"]["config"],
            "source_id": fixture["clip"]["id"],
            "source_sha256": fixture["clip"]["sha256"],
            "q": artifact_record(attention_core_paths["q"], "UINT8", (6, 1500, 64)),
            "k": artifact_record(attention_core_paths["k"], "UINT8", (6, 64, 1500)),
            "v": artifact_record(attention_core_paths["v"], "UINT8", (6, 1500, 64)),
            "output": artifact_record(attention_core_paths["output"], "UINT8", (6, 1500, 64)),
        },
        "encoder_block_activation_encoding_table": {
            **artifact_record(block_encoding_path, "F32,I32", (len(block_encoding_order),)),
            "order": list(block_encoding_order),
        },
        "encoder_block_fixture": {
            "source_config": fixture["clip"]["config"],
            "source_id": fixture["clip"]["id"],
            "source_sha256": fixture["clip"]["sha256"],
            "input": artifact_record(block_input_path, "UINT8", (1500, 384)),
            "attention_input": artifact_record(
                block_attention_input_path, "UINT8", (1500, 384)
            ),
            "output": artifact_record(block_output_path, "UINT8", (1500, 384)),
        },
        "normalizations": normalization_records,
        "biases": bias_records,
    }
    path = directory / "deployment.json"
    path.write_text(json.dumps(deployment, indent=2, sort_keys=True) + "\n", encoding="ascii")


def calibrate(model_path, corpus_manifest_path, prepared_dir, output_path, limit, layer):
    if layer not in range(ENCODER_LAYER_COUNT):
        raise ValueError(f"encoder layer must be between 0 and {ENCODER_LAYER_COUNT - 1}")
    if sha256_file(model_path) != MODEL_SHA256:
        raise ValueError("model SHA-256 does not match the pinned Whisper Tiny checkpoint")
    corpus_hash = sha256_file(corpus_manifest_path)
    corpus = json.loads(corpus_manifest_path.read_text(encoding="utf-8"))
    if corpus["revision"] != CORPUS_REVISION:
        raise ValueError("calibration corpus revision does not match the pinned FLEURS revision")
    clips = corpus["clips"][:limit] if limit else corpus["clips"]
    weights = load_weights(model_path, layer)
    activation_names = (
        "encoder_input",
        "attention_input",
        "q_proj_output",
        "k_proj_output",
        "v_proj_output",
        "attention_scores",
        "attention_weights",
        "attended_output",
        "attention_output",
        "attention_residual",
        "fc1_input",
        "fc1_output",
        "gelu_output",
        "fc2_output",
        "block_output",
    )
    statistics = {
        name: RangeStats(
            MAX_SAMPLES_PER_UPDATE
            if name in (
                "encoder_input",
                "attention_scores",
                "attention_weights",
                "attended_output",
                "attention_output",
                "attention_residual",
                "block_output",
            )
            else 0
        )
        for name in activation_names
    }
    cache_dir = Path(tempfile.mkdtemp(prefix="newos-whisper-calibration-"))
    started = time.perf_counter()
    for index, clip in enumerate(clips, 1):
        path = corpus_manifest_path.parent / clip["file"]
        if sha256_file(path) != clip["sha256"]:
            raise ValueError(f"calibration audio SHA-256 mismatch: {path}")
        samples = load_float32_wav(path)
        if samples.size != clip["samples"]:
            raise ValueError(f"calibration audio sample count mismatch: {path}")
        hidden = encoder_frontend(log_mel_spectrogram(samples), weights)
        for previous_layer in range(layer):
            hidden = encoder_layer(hidden, weights, previous_layer)["block_output"]
        activations = encoder_layer(hidden, weights, layer, statistics)
        for name, stats in statistics.items():
            if name not in ("attention_scores", "attention_weights"):
                stats.add(activations[name])
        cache_names = [
            "attention_input",
            "q_proj_output",
            "k_proj_output",
            "v_proj_output",
            "fc1_input",
            "fc2_output",
        ]
        if index <= ATTENTION_EVALUATION_CLIPS:
            cache_names.extend(("encoder_input", "attended_output", "block_output"))
        for name in cache_names:
            np.save(cache_dir / f"{index}-{name}.npy", activations[name], allow_pickle=False)
        print(f"[{index}/{len(clips)}] {clip['config']} {clip['language']}")

    summaries = {name: stats.summarize() for name, stats in statistics.items()}
    prepared_weights = load_prepared_weights(prepared_dir, layer)
    evaluation, projection_evaluation, attention_evaluation, block_evaluation, fixture = evaluate_quantization(
        clips,
        cache_dir,
        weights,
        prepared_weights,
        summaries,
        layer,
    )
    shutil.rmtree(cache_dir)
    result = {
        "format": "newos.whisper_tiny.activation_calibration.v1",
        "model": {
            "name": "openai/whisper-tiny",
            "revision": MODEL_REVISION,
            "sha256": MODEL_SHA256,
        },
        "encoder_layer": layer,
        "corpus": {
            "name": corpus["dataset"],
            "revision": corpus["revision"],
            "license": corpus["license"],
            "manifest_sha256": corpus_hash,
            "clips": len(clips),
            "samples": sum(clip["samples"] for clip in clips),
        },
        "method": {
            "implementation": f"NumPy Whisper encoder frontend through layer {layer}",
            "numpy": np.__version__,
            "minimum_sample_stride": SAMPLE_STRIDE,
            "maximum_samples_per_update": MAX_SAMPLES_PER_UPDATE,
            "recommended_encoding": "percentile_0.01_99.99_uint8",
        },
        "activations": summaries,
        "quantized_mlp_evaluation": evaluation,
        "quantized_attention_projection_evaluation": projection_evaluation,
        "quantized_attention_core_evaluation": attention_evaluation,
        "quantized_attention_core_evaluation_clips": min(ATTENTION_EVALUATION_CLIPS, len(clips)),
        "quantized_encoder_block_evaluation": block_evaluation,
        "quantized_encoder_block_evaluation_clips": min(ATTENTION_EVALUATION_CLIPS, len(clips)),
        "elapsed_seconds": time.perf_counter() - started,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="ascii")
    write_deployment_bundle(
        prepared_dir, fixture, weights, prepared_weights, summaries, output_path, layer
    )
    return result


def main():
    script_dir = Path(__file__).resolve().parent
    snapdragon_dir = script_dir.parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=snapdragon_dir / "models/whisper-tiny/model.safetensors")
    parser.add_argument("--corpus", type=Path, default=snapdragon_dir / "models/calibration/fleurs/manifest.json")
    parser.add_argument("--prepared", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--layer", type=int, choices=range(ENCODER_LAYER_COUNT), default=0)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--stack-only", action="store_true")
    parser.add_argument("--fp16-only", action="store_true")
    parser.add_argument("--frontend-only", action="store_true")
    parser.add_argument("--frontend-output", type=Path)
    parser.add_argument("--fp16-output", type=Path)
    parser.add_argument("--stack-output", type=Path)
    args = parser.parse_args()
    if args.frontend_only:
        output = args.frontend_output or (
            snapdragon_dir / "models/whisper-tiny/frontend-fp16"
        )
        manifest = write_fp16_frontend_bundle(
            args.model.resolve(), args.corpus.resolve(), output.resolve()
        )
        print(f"Wrote FP16 frontend for {manifest['source_file']} to {output.resolve()}")
        return
    if args.fp16_only:
        output = args.fp16_output or (
            snapdragon_dir / f"models/whisper-tiny/encoder-layer-{args.layer}-fp16"
        )
        manifest = write_fp16_encoder_bundle(
            args.model.resolve(), args.corpus.resolve(), output.resolve(), args.layer
        )
        print(f"Wrote FP16 encoder layer {manifest['encoder_layer']} to {output.resolve()}")
        return
    if args.stack_only:
        stack_output = args.stack_output or (
            snapdragon_dir / "models/whisper-tiny/encoder-stack-int8"
        )
        stack = write_encoder_stack_fixture(
            args.model.resolve(),
            args.corpus.resolve(),
            (snapdragon_dir / "models/whisper-tiny").resolve(),
            stack_output.resolve(),
        )
        error = stack["quantized_stack_evaluation"]
        print(f"Wrote {stack_output.resolve()}")
        print(f"  output relative L2={error['relative_l2_error']:.8g}")
        return
    prepared = args.prepared or (
        snapdragon_dir / f"models/whisper-tiny/encoder-layer-{args.layer}-mlp-int8"
    )
    output = args.output or (
        snapdragon_dir / f"models/calibration/whisper-tiny-layer-{args.layer}-mlp.json"
    )
    result = calibrate(
        args.model.resolve(),
        args.corpus.resolve(),
        prepared.resolve(),
        output.resolve(),
        args.limit,
        args.layer,
    )
    print(f"Wrote {output.resolve()} in {result['elapsed_seconds']:.1f} seconds")
    for name, values in result["activations"].items():
        encoding = values["percentile_0.01_99.99_uint8"]
        print(f"  {name}: scale={encoding['scale']:.9g}, offset={encoding['qnn_offset']}")
    for scheme, error in result["quantized_mlp_evaluation"].items():
        print(f"  {scheme}: output relative L2={error['relative_l2_error']:.8g}")


if __name__ == "__main__":
    main()