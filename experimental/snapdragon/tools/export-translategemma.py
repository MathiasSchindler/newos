#!/usr/bin/env python3
"""Export pinned TranslateGemma BF16 tensors to versioned W8/W4 artifacts."""

import argparse
import ast
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import shutil
import struct
import sys
import uuid

try:
    import numpy as np
except ImportError:
    np = None


REPOSITORY = "google/translategemma-4b-it"
REVISION = "10042cb0e6e7fdce748996a71dc3dc432a4e0c89"
MAGIC = 0x33305452414D4547
VERSION = 1
HEADER_SIZE = 256
MAX_RANK = 8
NO_AXIS = 0xFFFFFFFF
KIND_TENSOR = 1
KIND_TOKENIZER = 2
KIND_LAYER_TABLE = 3
KIND_ROPE_TABLE = 4
KIND_FIXTURE = 5
ELEMENT_F16 = 1
ELEMENT_S8 = 2
ELEMENT_S4 = 3
ELEMENT_U8 = 4
QUANT_NONE = 0
QUANT_SYMMETRIC_GROUP = 1
LAYOUT_OUTPUT_INPUT_ROW_MAJOR = 1
LAYOUT_OPAQUE = 2
FNV_OFFSET_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
CHUNK_ELEMENTS = 1024 * 1024


class ExportError(Exception):
    pass


def canonical_json(value):
    return json.dumps(value, indent=2, sort_keys=True).encode("utf-8") + b"\n"


def sha256_bytes(data):
    return hashlib.sha256(data).digest()


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tensor_id(name):
    value = FNV_OFFSET_BASIS
    for byte in name.encode("utf-8"):
        value ^= byte
        value = value * FNV_PRIME & 0xFFFFFFFFFFFFFFFF
    return value


def model_digest():
    return sha256_bytes(f"{REPOSITORY}@{REVISION}".encode("ascii"))


def encode_header(spec, payload_digest):
    dimensions = tuple(spec["shape"]) + (0,) * (MAX_RANK - len(spec["shape"]))
    if len(dimensions) != MAX_RANK:
        raise ExportError("artifact rank exceeds the format limit")
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<QII", header, 0, MAGIC, VERSION, HEADER_SIZE)
    struct.pack_into(
        "<IIIIIII", header, 16,
        spec["kind"], spec["element_type"], spec["quantization"], spec["layout"],
        len(spec["shape"]), spec["group_size"], spec["quantization_axis"],
    )
    struct.pack_into(
        "<QQQQQQ", header, 48,
        spec["element_count"], spec["payload_size"], spec["data_size"],
        spec["scale_count"], spec["data_size"], tensor_id(spec["name"]),
    )
    struct.pack_into("<8Q", header, 96, *dimensions)
    header[160:192] = model_digest()
    header[192:224] = sha256_bytes(spec["name"].encode("utf-8"))
    header[224:256] = payload_digest
    return bytes(header)


def decode_header(header):
    if len(header) != HEADER_SIZE:
        raise ExportError("truncated artifact header")
    magic, version, header_size = struct.unpack_from("<QII", header)
    if (magic, version, header_size) != (MAGIC, VERSION, HEADER_SIZE):
        raise ExportError("unsupported artifact magic, version, or header size")
    if struct.unpack_from("<I", header, 44)[0] != 0:
        raise ExportError("artifact reserved field is nonzero")
    fields = struct.unpack_from("<IIIIIII", header, 16)
    sizes = struct.unpack_from("<QQQQQQ", header, 48)
    rank = fields[4]
    if not 1 <= rank <= MAX_RANK:
        raise ExportError("invalid artifact rank")
    dimensions = struct.unpack_from("<8Q", header, 96)
    if any(dimensions[rank:]):
        raise ExportError("unused artifact dimensions are nonzero")
    return {
        "kind": fields[0], "element_type": fields[1],
        "quantization": fields[2], "layout": fields[3], "rank": rank,
        "group_size": fields[5], "quantization_axis": fields[6],
        "element_count": sizes[0], "payload_size": sizes[1],
        "data_size": sizes[2], "scale_count": sizes[3],
        "scale_offset": sizes[4], "tensor_id": sizes[5],
        "shape": list(dimensions[:rank]), "model_sha256": header[160:192],
        "name_sha256": header[192:224], "payload_sha256": header[224:256],
    }


class ArtifactWriter:
    def __init__(self, path, spec):
        self.path = Path(path)
        self.temporary = self.path.with_name(self.path.name + ".partial")
        self.spec = spec
        self.digest = hashlib.sha256()
        self.written = 0
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = self.temporary.open("w+b")
        self.stream.write(b"\0" * HEADER_SIZE)

    def write(self, data):
        view = memoryview(data).cast("B")
        self.stream.write(view)
        self.digest.update(view)
        self.written += len(view)

    def close(self):
        if self.written != self.spec["payload_size"]:
            raise ExportError(
                f"artifact {self.spec['name']} wrote {self.written} payload bytes; "
                f"expected {self.spec['payload_size']}"
            )
        payload_digest = self.digest.digest()
        self.stream.seek(0)
        self.stream.write(encode_header(self.spec, payload_digest))
        self.stream.close()
        os.replace(self.temporary, self.path)
        return payload_digest.hex()

    def abort(self):
        self.stream.close()
        self.temporary.unlink(missing_ok=True)


class SafeTensorShard:
    def __init__(self, path):
        self.path = Path(path)
        with self.path.open("rb") as stream:
            prefix = stream.read(8)
            if len(prefix) != 8:
                raise ExportError(f"truncated Safetensors prefix: {self.path.name}")
            self.header_size = struct.unpack("<Q", prefix)[0]
            if not 0 < self.header_size <= 100 * 1024 * 1024:
                raise ExportError(f"invalid Safetensors header size: {self.path.name}")
            header = stream.read(self.header_size)
        try:
            self.tensors = json.loads(header)
        except (UnicodeError, json.JSONDecodeError) as error:
            raise ExportError(f"invalid Safetensors header: {self.path.name}") from error
        self.data_start = 8 + self.header_size

    def bf16(self, name, expected_shape):
        metadata = self.tensors.get(name)
        if not isinstance(metadata, dict) or metadata.get("dtype") != "BF16" or \
                metadata.get("shape") != list(expected_shape):
            raise ExportError(f"unexpected source tensor contract for {name}")
        start, end = metadata.get("data_offsets", [None, None])
        count = math.prod(expected_shape)
        if not isinstance(start, int) or not isinstance(end, int) or end - start != count * 2:
            raise ExportError(f"invalid source byte range for {name}")
        return np.memmap(
            self.path, mode="r", dtype="<u2", offset=self.data_start + start,
            shape=tuple(expected_shape), order="C",
        )


def bf16_to_f32(values):
    expanded = np.asarray(values, dtype="<u2").astype("<u4") << np.uint32(16)
    return expanded.view("<f4")


def close_memmap(values):
    mapping = getattr(values, "_mmap", None)
    if mapping is not None:
        mapping.close()


def quantize_groups(values, bits):
    values = np.asarray(values, dtype=np.float32)
    if values.ndim != 2 or values.shape[1] == 0:
        raise ExportError("quantization input must contain nonempty rows")
    if not np.isfinite(values).all():
        raise ExportError("source tensor contains a non-finite value")
    maximum = np.max(np.abs(values), axis=1)
    limit = (1 << (bits - 1)) - 1
    scales = np.where(maximum == 0.0, np.float32(1.0), maximum / np.float32(limit))
    quantized = np.rint(values / scales[:, None]).clip(-limit, limit).astype(np.int8)
    return quantized, scales.astype("<f2")


def pack_s4(values):
    values = np.asarray(values, dtype=np.int8)
    if values.shape[-1] % 2:
        raise ExportError("S4 packing requires an even final dimension")
    if np.any(values < -7) or np.any(values > 7):
        raise ExportError("S4 value is outside the symmetric deployment range")
    unsigned = values.astype(np.uint8) & np.uint8(0x0F)
    return unsigned[..., 0::2] | (unsigned[..., 1::2] << np.uint8(4))


def artifact_spec(name, shape, kind, element_type, quantization, layout, data_size,
                  scale_count=0, group_size=0, quantization_axis=NO_AXIS):
    element_count = math.prod(shape)
    payload_size = data_size + scale_count * 2
    return {
        "name": name, "shape": list(shape), "kind": kind,
        "element_type": element_type, "quantization": quantization,
        "layout": layout, "group_size": group_size,
        "quantization_axis": quantization_axis, "element_count": element_count,
        "data_size": data_size, "scale_count": scale_count,
        "payload_size": payload_size,
    }


def write_quantized_tensor(source, name, shape, bits, output):
    rows, columns = shape
    element_count = rows * columns
    data_size = element_count if bits == 8 else element_count // 2
    scale_count = rows
    spec = artifact_spec(
        name, shape, KIND_TENSOR, ELEMENT_S8 if bits == 8 else ELEMENT_S4,
        QUANT_SYMMETRIC_GROUP, LAYOUT_OUTPUT_INPUT_ROW_MAJOR, data_size,
        scale_count, columns, 1,
    )
    scale_path = output.with_name(output.name + ".scales.partial")
    writer = ArtifactWriter(output, spec)
    squared_error = 0.0
    maximum_error = 0.0
    try:
        with scale_path.open("wb") as scales_stream:
            row_chunk = max(1, CHUNK_ELEMENTS // columns)
            for row in range(0, rows, row_chunk):
                values = bf16_to_f32(source[row:row + row_chunk]).reshape(-1, columns)
                quantized, scales = quantize_groups(values, bits)
                deployed = quantized.astype(np.float32) * scales.astype(np.float32)[:, None]
                error = deployed - values
                squared_error += float(np.sum(error * error, dtype=np.float64))
                maximum_error = max(maximum_error, float(np.max(np.abs(error))))
                writer.write(quantized if bits == 8 else pack_s4(quantized))
                scales_stream.write(scales.tobytes())
        with scale_path.open("rb") as scales_stream:
            for block in iter(lambda: scales_stream.read(1024 * 1024), b""):
                writer.write(block)
        payload_sha256 = writer.close()
    except Exception:
        writer.abort()
        raise
    finally:
        scale_path.unlink(missing_ok=True)
    return spec, payload_sha256, {
        "rmse": math.sqrt(squared_error / element_count),
        "max_absolute_error": maximum_error,
    }


def write_f16_tensor(source, name, shape, output):
    element_count = math.prod(shape)
    spec = artifact_spec(
        name, shape, KIND_TENSOR, ELEMENT_F16, QUANT_NONE,
        LAYOUT_OUTPUT_INPUT_ROW_MAJOR, element_count * 2,
    )
    writer = ArtifactWriter(output, spec)
    try:
        flat = source.reshape(-1)
        for offset in range(0, element_count, CHUNK_ELEMENTS):
            values = bf16_to_f32(flat[offset:offset + CHUNK_ELEMENTS])
            if not np.isfinite(values).all():
                raise ExportError(f"source tensor {name} contains a non-finite value")
            writer.write(values.astype("<f2"))
        payload_sha256 = writer.close()
    except Exception:
        writer.abort()
        raise
    return spec, payload_sha256


def write_blob_artifact(path, name, payload, kind):
    spec = artifact_spec(name, [len(payload)], kind, ELEMENT_U8, QUANT_NONE,
                         LAYOUT_OPAQUE, len(payload))
    writer = ArtifactWriter(path, spec)
    try:
        writer.write(payload)
        payload_sha256 = writer.close()
    except Exception:
        writer.abort()
        raise
    return manifest_entry(path, path.parent, spec, payload_sha256)


def artifact_filename(name):
    return f"{tensor_id(name):016x}.gta"


def manifest_entry(path, root, spec, payload_sha256, metrics=None):
    entry = {
        "path": path.relative_to(root).as_posix(), "name": spec["name"],
        "tensor_id": f"{tensor_id(spec['name']):016x}", "shape": spec["shape"],
        "element_type": spec["element_type"], "quantization": spec["quantization"],
        "group_size": spec["group_size"], "quantization_axis": spec["quantization_axis"],
        "data_size": spec["data_size"], "scale_count": spec["scale_count"],
        "payload_size": spec["payload_size"], "payload_sha256": payload_sha256,
    }
    if metrics is not None:
        entry["error"] = metrics
    return entry


def verify_artifact(path, entry):
    path = Path(path)
    with path.open("rb") as stream:
        parsed = decode_header(stream.read(HEADER_SIZE))
        digest = hashlib.sha256()
        size = 0
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
            size += len(block)
    expected_name = entry["name"]
    if parsed["model_sha256"] != model_digest() or \
            parsed["name_sha256"] != sha256_bytes(expected_name.encode("utf-8")) or \
            parsed["tensor_id"] != tensor_id(expected_name):
        raise ExportError(f"artifact identity mismatch: {path}")
    if parsed["shape"] != entry["shape"] or parsed["payload_size"] != size or \
            digest.digest() != parsed["payload_sha256"] or \
            digest.hexdigest() != entry["payload_sha256"]:
        raise ExportError(f"artifact payload mismatch: {path}")
    for field in (
        "element_type", "quantization", "group_size", "quantization_axis",
        "data_size", "scale_count",
    ):
        if parsed[field] != entry[field]:
            raise ExportError(f"artifact {field} mismatch: {path}")
    if parsed["element_count"] != math.prod(parsed["shape"]) or \
            parsed["scale_offset"] != parsed["data_size"]:
        raise ExportError(f"artifact dimensions or offsets are invalid: {path}")
    if parsed["quantization"] == QUANT_NONE:
        element_size = 2 if parsed["element_type"] == ELEMENT_F16 else \
            1 if parsed["element_type"] == ELEMENT_U8 else 0
        if element_size == 0 or parsed["group_size"] != 0 or \
                parsed["quantization_axis"] != NO_AXIS or parsed["scale_count"] != 0 or \
                parsed["data_size"] != parsed["element_count"] * element_size:
            raise ExportError(f"artifact unquantized contract is invalid: {path}")
    elif parsed["quantization"] == QUANT_SYMMETRIC_GROUP:
        expected_data = parsed["element_count"] if parsed["element_type"] == ELEMENT_S8 \
            else (parsed["element_count"] + 1) // 2 \
            if parsed["element_type"] == ELEMENT_S4 else 0
        if parsed["rank"] != 2 or parsed["group_size"] != parsed["shape"][1] or \
            parsed["quantization_axis"] != 1 or \
                parsed["data_size"] != expected_data or \
            parsed["scale_count"] != parsed["shape"][0]:
            raise ExportError(f"artifact quantization contract is invalid: {path}")
    else:
        raise ExportError(f"unsupported artifact quantization: {path}")
    return parsed


def validate_artifact_set(root):
    root = Path(root).resolve()
    manifest_path = root / "manifest.json"
    if not manifest_path.is_file():
        raise ExportError(f"artifact manifest is missing: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ExportError(f"artifact manifest is invalid: {error}") from error
    if manifest.get("schema_version") != 1 or manifest.get("model") != {
        "identity_sha256": model_digest().hex(),
        "repository": REPOSITORY,
        "revision": REVISION,
    }:
        raise ExportError("artifact manifest model identity is invalid")
    audit_path = root / "source-audit.json"
    if not audit_path.is_file() or sha256_file(audit_path) != manifest.get("source_audit_sha256"):
        raise ExportError("artifact source audit is missing or corrupt")
    expected_paths = {"manifest.json", "source-audit.json"}
    for variant_name, variant in manifest.get("variants", {}).items():
        entries = variant.get("artifacts", [])
        if variant_name not in {"w4a16", "w8a16"} or len(entries) != 444 or \
                variant.get("tensor_count") != len(entries) or \
                variant.get("payload_size") != sum(entry["payload_size"] for entry in entries):
            raise ExportError(f"artifact variant inventory is invalid: {variant_name}")
        weight_and_scale_size = sum(
            entry["payload_size"] for entry in entries
            if entry["quantization"] == QUANT_SYMMETRIC_GROUP
        )
        if variant.get("weight_and_scale_size") != weight_and_scale_size:
            raise ExportError(f"artifact variant size is invalid: {variant_name}")
        if variant_name == "w4a16" and not 1_900_000_000 <= weight_and_scale_size <= 2_200_000_000:
            raise ExportError("W4 weight and scale storage is outside 1.9-2.2 GB")
        for entry in entries:
            relative = Path(variant_name) / entry["path"]
            expected_paths.add(relative.as_posix())
            verify_artifact(root / relative, entry)
    for entry in manifest.get("metadata", []):
        expected_paths.add(entry["path"])
        verify_artifact(root / entry["path"], entry)
    actual_paths = {
        path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()
    }
    if actual_paths != expected_paths:
        missing = sorted(expected_paths - actual_paths)
        unexpected = sorted(actual_paths - expected_paths)
        raise ExportError(
            f"artifact set inventory mismatch; missing={missing[:3]}, unexpected={unexpected[:3]}"
        )
    return manifest


def load_stage2_validator(path):
    spec = importlib.util.spec_from_file_location("translategemma_stage2", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_source(tools_dir, catalog, model_dir, audit_path):
    validator = load_stage2_validator(tools_dir / "validate-translategemma.py")
    try:
        return validator.validate_checkpoint(catalog, model_dir, audit_path)
    except validator.ValidationError as error:
        raise ExportError(str(error)) from error


def export_variant(model_dir, audit, output, bits):
    entries = []
    shards = {}
    seen_ids = {}
    retained = [tensor for tensor in audit["tensors"] if tensor["disposition"] == "retained"]
    for tensor_index, tensor in enumerate(retained, 1):
        name = tensor["name"]
        if tensor_index == 1 or tensor_index % 10 == 0 or tensor_index == len(retained):
            print(f"w{bits}a16 export {tensor_index}/{len(retained)}: {name}", flush=True)
        stable_id = tensor_id(name)
        if stable_id in seen_ids:
            raise ExportError(f"tensor ID collision: {name} and {seen_ids[stable_id]}")
        seen_ids[stable_id] = name
        shard_name = tensor["shard"]
        shard = shards.setdefault(shard_name, SafeTensorShard(model_dir / shard_name))
        source = shard.bf16(name, tensor["shape"])
        path = output / "tensors" / artifact_filename(name)
        try:
            if len(tensor["shape"]) == 2:
                spec, payload_sha256, metrics = write_quantized_tensor(
                    source, name, tensor["shape"], bits, path
                )
                entry = manifest_entry(path, output, spec, payload_sha256, metrics)
            elif len(tensor["shape"]) == 1:
                spec, payload_sha256 = write_f16_tensor(
                    source, name, tensor["shape"], path
                )
                entry = manifest_entry(path, output, spec, payload_sha256)
            else:
                raise ExportError(f"unsupported retained tensor rank for {name}")
        finally:
            close_memmap(source)
        entries.append(entry)
    return entries


def export_metadata(model_dir, output):
    entries = []
    tokenizer_files = [
        "tokenizer.model", "tokenizer.json", "tokenizer_config.json",
        "special_tokens_map.json", "added_tokens.json", "chat_template.jinja",
    ]
    for filename in tokenizer_files:
        payload = (model_dir / filename).read_bytes()
        path = output / "metadata" / f"{filename}.gta"
        entry = write_blob_artifact(path, f"tokenizer/{filename}", payload, KIND_TOKENIZER)
        entry["path"] = path.relative_to(output).as_posix()
        entries.append(entry)
    layer_payload = bytes(1 if (layer + 1) % 6 == 0 else 0 for layer in range(34))
    path = output / "metadata" / "layer-types.gta"
    entry = write_blob_artifact(path, "metadata/layer-types", layer_payload, KIND_LAYER_TABLE)
    entry["path"] = path.relative_to(output).as_posix()
    entries.append(entry)
    rope_payload = struct.pack("<Iddd", 2048, 1000000.0, 10000.0, 8.0)
    path = output / "metadata" / "rope.gta"
    entry = write_blob_artifact(path, "metadata/rope", rope_payload, KIND_ROPE_TABLE)
    entry["path"] = path.relative_to(output).as_posix()
    entries.append(entry)
    fixture = {
        "grouping": "per-output-channel", "group_size": "input-dimension",
        "packing": "low-nibble-first-twos-complement",
        "rounding": "nearest-ties-to-even", "s4_range": [-7, 7],
        "s8_range": [-127, 127], "scale_type": "float16",
        "zero_group_scale": 1.0,
    }
    path = output / "metadata" / "quantization-contract.gta"
    entry = write_blob_artifact(
        path, "fixture/quantization-contract", canonical_json(fixture), KIND_FIXTURE
    )
    entry["path"] = path.relative_to(output).as_posix()
    entries.append(entry)
    return entries


def publish_directory(staging, output, replace):
    if output.exists() and not replace:
        raise ExportError(f"output already exists: {output}; pass --replace to replace it")
    backup = output.with_name(output.name + ".previous")
    if backup.exists():
        shutil.rmtree(backup)
    if output.exists():
        os.replace(output, backup)
    try:
        os.replace(staging, output)
    except Exception:
        if backup.exists() and not output.exists():
            os.replace(backup, output)
        raise
    if backup.exists():
        shutil.rmtree(backup)


def export_all(model_dir, catalog, output, tools_dir, variants, replace=False):
    model_dir = Path(model_dir).resolve()
    output = Path(output).resolve()
    staging = output.with_name(f"{output.name}.partial-{uuid.uuid4().hex}")
    staging.mkdir(parents=True)
    try:
        print("Validating pinned TranslateGemma source checkpoint...", flush=True)
        audit = validate_source(tools_dir, catalog, model_dir, staging / "source-audit.json")
        print("Source validation passed; beginning bounded-memory conversion.", flush=True)
        manifest = {
            "schema_version": 1,
            "format": {"magic": "GEMART03", "version": VERSION,
                       "header_size": HEADER_SIZE},
            "model": {"repository": REPOSITORY, "revision": REVISION,
                      "identity_sha256": model_digest().hex()},
            "conversion": {"grouping": "per-output-channel",
                           "packing": "low-nibble-first-twos-complement",
                           "rounding": "nearest-ties-to-even",
                           "source_dtype": "BF16"},
            "variants": {},
            "metadata": [],
            "deferred": [
                "prompt-processor-qnn-context", "token-generator-qnn-context",
                "prompt-layer-logits-token-reference-fixtures",
                "freestanding-tokenizer-lookup-tables",
            ],
        }
        for bits in variants:
            variant_name = f"w{bits}a16"
            entries = export_variant(model_dir, audit, staging / variant_name, bits)
            if len(entries) != 444:
                raise ExportError(f"{variant_name} exported {len(entries)} tensors; expected 444")
            print(f"Verifying {variant_name} artifact hashes...", flush=True)
            for entry in entries:
                verify_artifact(staging / variant_name / entry["path"], entry)
            manifest["variants"][variant_name] = {
                "artifacts": entries,
                "tensor_count": len(entries),
                "payload_size": sum(entry["payload_size"] for entry in entries),
                "weight_and_scale_size": sum(
                    entry["payload_size"] for entry in entries
                    if entry["quantization"] == QUANT_SYMMETRIC_GROUP
                ),
            }
            if bits == 4 and not 1_900_000_000 <= \
                    manifest["variants"][variant_name]["weight_and_scale_size"] <= 2_200_000_000:
                raise ExportError("W4 weight and scale storage is outside 1.9-2.2 GB")
        metadata = export_metadata(model_dir, staging)
        for entry in metadata:
            verify_artifact(staging / entry["path"], entry)
        manifest["metadata"] = metadata
        manifest["source_audit_sha256"] = sha256_file(staging / "source-audit.json")
        manifest_bytes = canonical_json(manifest)
        (staging / "manifest.json.partial").write_bytes(manifest_bytes)
        os.replace(staging / "manifest.json.partial", staging / "manifest.json")
        print("All artifacts verified; publishing manifest-backed set...", flush=True)
        publish_directory(staging, output, replace)
        return manifest
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def export_tokenizer(model_dir, catalog_path, output):
    model_dir = Path(model_dir)
    catalog = json.loads(Path(catalog_path).read_text(encoding="utf-8"))
    source = json.loads((model_dir / "source-lock.json").read_text(encoding="utf-8"))
    if source != catalog:
        raise ExportError("tokenizer source lock differs from the pinned catalog")
    if catalog["model"]["revision"] != REVISION or catalog["model"]["repository"] != REPOSITORY:
        raise ExportError("wrong tokenizer source identity")
    for entry in catalog["files"]:
        if entry["role"] == "tokenizer" or entry["name"] == "generation_config.json":
            path = model_dir / entry["name"]
            if path.stat().st_size != entry["size"] or sha256_file(path) != entry["sha256"]:
                raise ExportError(f"tokenizer source hash mismatch: {entry['name']}")
    tokenizer_path = model_dir / "tokenizer.json"
    template_path = model_dir / "chat_template.jinja"
    tokenizer = json.loads(tokenizer_path.read_text(encoding="utf-8"))
    tokenizer_config = json.loads((model_dir / "tokenizer_config.json").read_text(encoding="utf-8"))
    extra_special = set(tokenizer_config["extra_special_tokens"].values())
    model = tokenizer["model"]
    if model["type"] != "BPE" or not model["byte_fallback"] or model["ignore_merges"]:
        raise ExportError("unsupported tokenizer model")
    if tokenizer["normalizer"] != {
        "type": "Replace", "pattern": {"String": " "}, "content": "\u2581"
    }:
        raise ExportError("unsupported tokenizer normalization")
    vocabulary = model["vocab"]
    pieces = {token_id: text for text, token_id in vocabulary.items()}
    flags = {}
    for token in tokenizer["added_tokens"]:
        if any(token[key] for key in ("single_word", "lstrip", "rstrip", "normalized")):
            raise ExportError("unsupported added-token flags")
        token_id = token["id"]
        if token_id in pieces and pieces[token_id] != token["content"]:
            raise ExportError("added token disagrees with vocabulary")
        pieces[token_id] = token["content"]
        flags[token_id] = 1 | (2 if token["special"] or token["content"] in extra_special else 0)
    if set(pieces) != set(range(262145)):
        raise ExportError("unexpected tokenizer ID inventory")
    for byte in range(256):
        token_id = vocabulary[f"<0x{byte:02X}>"]
        flags[token_id] = flags.get(token_id, 0) | 4 | (byte << 8)
    template = template_path.read_text(encoding="utf-8")
    languages = ast.literal_eval(template.split("{%- set languages = ", 1)[1].split("\n-%}", 1)[0])
    strings = bytearray()

    def store(text):
        encoded = text.encode("utf-8")
        offset = len(strings)
        strings.extend(encoded)
        return offset, len(encoded)

    records = bytearray()
    for token_id in range(len(pieces)):
        records.extend(struct.pack("<III", *store(pieces[token_id]), flags.get(token_id, 0)))
    lexical = sorted(vocabulary.values(), key=lambda token_id: pieces[token_id].encode("utf-8"))
    added = sorted(flags.keys() & {token["id"] for token in tokenizer["added_tokens"]},
                   key=lambda token_id: pieces[token_id].encode("utf-8"))
    merges = []
    for rank, (left, right) in enumerate(model["merges"]):
        merges.append((vocabulary[left], vocabulary[right], vocabulary[left + right], rank))
    merges.sort()
    language_records = bytearray()
    for code, name in sorted(languages.items()):
        language_records.extend(struct.pack("<IIII", *store(code), *store(name)))
    payload = bytearray(struct.pack(
        "<8I", 0x34544D47, 1, len(pieces), len(lexical), len(added),
        len(merges), len(languages), len(strings)
    ))
    payload.extend(records)
    payload.extend(struct.pack(f"<{len(lexical)}I", *lexical))
    payload.extend(struct.pack(f"<{len(added)}I", *added))
    for merge in merges:
        payload.extend(struct.pack("<4I", *merge))
    payload.extend(language_records)
    payload.extend(strings)
    output = Path(output)
    entry = write_blob_artifact(output, "tokenizer/bpe-tables-v1", payload, KIND_TOKENIZER)
    verify_artifact(output, entry)
    print(f"Tokenizer: {len(pieces)} IDs, {len(merges)} merges, {len(languages)} languages, {len(payload)} bytes")
    return entry


def export_tokenizer_fixtures(model_dir, output):
    import importlib.metadata
    from transformers import AutoTokenizer

    versions = {name: importlib.metadata.version(name)
                for name in ("transformers", "tokenizers", "jinja2")}
    if versions != {"transformers": "4.57.3", "tokenizers": "0.22.2", "jinja2": "3.1.6"}:
        raise ExportError(f"unexpected tokenizer reference versions: {versions}")
    tokenizer = AutoTokenizer.from_pretrained(str(model_dir), local_files_only=True)
    source = json.loads((Path(model_dir) / "tokenizer.json").read_text(encoding="utf-8"))
    config = json.loads((Path(model_dir) / "tokenizer_config.json").read_text(encoding="utf-8"))
    for token in source["added_tokens"]:
        if token["content"] in config["extra_special_tokens"].values():
            token["special"] = True
    actual = json.loads(tokenizer.backend_tokenizer.to_str())
    for key in ("model", "normalizer", "pre_tokenizer", "decoder", "added_tokens"):
        if actual[key] != source[key]:
            raise ExportError(f"reference silently changed tokenizer {key}")
    cases = []

    def add(kind, text=b"", tokens=(), decoded=b"", flags=0, source_code="", target_code="", maximum=256):
        source_bytes, target_bytes = source_code.encode(), target_code.encode()
        cases.append(struct.pack("<8I", kind, flags, maximum, len(source_bytes),
                                 len(target_bytes), len(text), len(tokens), len(decoded)) +
                     source_bytes + target_bytes + text +
                     struct.pack(f"<{len(tokens)}I", *tokens) + decoded)

    def encode(text, bos=True, skip=True):
        tokens = tokenizer.encode(text, add_special_tokens=bos)
        if len(tokens) > 2048:
            raise ExportError("accepted tokenizer fixture exceeds the deployment context")
        add(1, text.encode("utf-8"), tokens,
            tokenizer.decode(tokens, skip_special_tokens=skip).encode("utf-8"),
            int(bos) | (int(skip) << 1))

    corpus = ["", "Hello world!", " a  b ", "\r\n\t", "\x00x\x00", "e\u0301 \u00e9",
              "\u010cesk\u00fd text: Dobr\u00fd den!", "Stra\u00dfe, Gr\u00fc\u00dfe!",
              "\u4f60\u597d\uff0c\u4e16\u754c\uff01", "\u3053\u3093\u306b\u3061\u306f",
              "\uc548\ub155\ud558\uc138\uc694", "\u0645\u0631\u062d\u0628\u0627",
              "\u0939\u093f\u0928\u094d\u0926\u0940", "\u0e44\u0e17\u0e22",
              "\u2581literal\u2581", "\U0001f469\u200d\U0001f4bb", "\U0010ffff",
              "<bos><start_of_turn>user\n<end_of_turn>", "<0xFF>", "\u00a0\u2003 x \u3000"]
    for text in corpus:
        for bos, skip in ((False, False), (True, True)):
            encode(text, bos, skip)
    for token in source["added_tokens"]:
        encode("x" + token["content"] + "y", False, False)
        if token["special"]:
            encode("x" + token["content"] + "y", True, True)
    generator = random.Random(0x474d5434)
    alphabet = list("ab AB09\t\n!?_<>-\u00e9\u0301\u2581\u4e2d\u0645\u0939") + [
        "<bos>", "<end_of_turn>", "\U0001f600", "\u00a0", "\r\n"]
    for index in range(400):
        text = "".join(generator.choice(alphabet) for _ in range(generator.randrange(1, 150)))
        if index % 3 == 0:
            codepoint = generator.randrange(0x10000, 0x110000)
            text += chr(codepoint)
        encode(text, index % 2 == 0, index % 3 == 0)
    for text in (" " * 60000, "\n" * 32000, "abc " * 500):
        encode(text)
    template = (Path(model_dir) / "chat_template.jinja").read_text(encoding="utf-8")
    languages = ast.literal_eval(template.split("{%- set languages = ", 1)[1].split("\n-%}", 1)[0])
    for code in sorted(languages):
        source_code = code.replace("-", "_")
        text = "\u2003\tHello, e\u0301!\u00a0\n"
        messages = [{"role": "user", "content": [{"type": "text", "source_lang_code": source_code,
                     "target_lang_code": "de-DE", "text": text}]}]
        tokens = tokenizer.apply_chat_template(messages, tokenize=True, add_generation_prompt=True)
        add(2, text.encode(), tokens, tokenizer.decode(tokens, skip_special_tokens=False).encode(),
            source_code=source_code, target_code="de-DE")
    for data in (b"\x80", b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe2\x82"):
        add(4, data)
    add(4, b"a " * 2049)
    add(4, b" " * 65536, flags=1)
    add(4, b" " * 196609)
    for source_code, target_code, maximum, text in (
        ("xx-INVALID", "de", 256, b"text"), ("en", "DE", 256, b"text"),
        ("en", "de", 0, b"text"), ("en", "de", 2048, b"text"),
        ("en", "de", 2047, b"text"), ("en", "de", 256, b"\xff"),
        ("en", "de", 256, b"a " * 2040),
    ):
        add(5, text, source_code=source_code, target_code=target_code, maximum=maximum)
    for values in ([0xff], [0xe2, 0x82, 0xac], [0x41, 0xff], [0xf0, 0x9f], [0, 65], [0xc0, 0xaf]):
        tokens = [source["model"]["vocab"][f"<0x{value:02X}>"] for value in values]
        add(3, tokens=tokens, decoded=tokenizer.decode(tokens, skip_special_tokens=False).encode())
    add(6, tokens=[262145])
    payload = struct.pack("<3I", 0x34524647, 1, len(cases)) + b"".join(cases)
    entry = write_blob_artifact(output, "fixture/tokenizer-v1", payload, KIND_FIXTURE)
    verify_artifact(output, entry)
    print(f"Tokenizer reference: {len(cases)} cases")
    return entry, versions


def export_tokenizer_set(model_dir, catalog, output, reference, replace):
    output = Path(output).resolve()
    staging = output.with_name(f"{output.name}.partial-{uuid.uuid4().hex}")
    staging.mkdir(parents=True)
    try:
        entry = export_tokenizer(model_dir, catalog, staging / "tokenizer.gta")
        manifest = {"schema_version": 1, "model": {"repository": REPOSITORY, "revision": REVISION},
                    "artifacts": [entry], "tokenizer_ids": 262145, "model_output_ids": 262208,
                    "maximum_input_bytes": 196608, "deployment_context": 2048,
                    "stop_token_ids": [1, 106], "do_sample": False}
        if reference:
            fixture, versions = export_tokenizer_fixtures(model_dir, staging / "tokenizer-fixtures.gta")
            manifest["artifacts"].append(fixture)
            manifest["reference_versions"] = versions
        (staging / "manifest.json").write_bytes(canonical_json(manifest))
        publish_directory(staging, output, replace)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", default="experimental/snapdragon/data/translategemma-4b")
    parser.add_argument("--catalog", default="experimental/snapdragon/tools/translategemma-models.json")
    parser.add_argument("--output")
    parser.add_argument("--variant", choices=("w4", "w8", "both"), default="both")
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--tokenizer-only", action="store_true")
    parser.add_argument("--tokenizer-reference", action="store_true")
    parser.add_argument("--numerical-reference", action="store_true")
    parser.add_argument("--block-reference", action="store_true")
    parser.add_argument("--prompt-reference", action="store_true")
    parser.add_argument("--primitives-only", action="store_true")
    parser.add_argument("--weights-dir", default="experimental/snapdragon/models/translategemma-4b-stage3")
    parser.add_argument("--reference-variant", choices=("all", "bf16", "w8a16", "w4a16"), default="all")
    args = parser.parse_args()
    if args.tokenizer_reference and not args.tokenizer_only:
        parser.error("--tokenizer-reference requires --tokenizer-only")
    if sum((args.numerical_reference, args.block_reference, args.prompt_reference, args.tokenizer_only)) > 1:
        parser.error("select only one reference/export mode")
    if (args.numerical_reference or args.block_reference or args.prompt_reference) and args.tokenizer_only:
        parser.error("numerical reference cannot be combined with tokenizer-only")
    if args.primitives_only and not args.numerical_reference:
        parser.error("--primitives-only requires --numerical-reference")
    if args.output is None:
        args.output = "experimental/snapdragon/models/translategemma-4b-stage" + ("7" if args.prompt_reference else "6" if args.block_reference else "5-v2" if args.numerical_reference else "4" if args.tokenizer_only else "3")
    tools_dir = Path(__file__).resolve().parent
    variants = (8, 4) if args.variant == "both" else (int(args.variant[1:]),)
    try:
        if args.numerical_reference or args.block_reference or args.prompt_reference:
            module_path = tools_dir / "translategemma-reference.py"
            spec = importlib.util.spec_from_file_location("translategemma_reference", module_path)
            reference = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(reference)
            if args.block_reference or args.prompt_reference:
                if args.verify_only:
                    reference.verify_reference(sys.modules[__name__], args.output, require_complete=False)
                else:
                    reference.export_block_reference(sys.modules[__name__], args.model_dir, args.catalog,
                                                     args.weights_dir, args.output, args.replace, args.prompt_reference)
                return 0
            if args.verify_only:
                reference.verify_reference(sys.modules[__name__], args.output,
                                           not args.primitives_only and args.reference_variant == "all")
                return 0
            reference.export_reference(sys.modules[__name__], args.model_dir, args.catalog,
                                       args.weights_dir, args.output, args.replace, args.primitives_only,
                                       args.reference_variant)
            return 0
        if args.tokenizer_only:
            export_tokenizer_set(args.model_dir, args.catalog, args.output, args.tokenizer_reference, args.replace)
            return 0
        if np is None:
            raise ExportError("weight conversion requires NumPy; tokenizer export uses only Python's standard library")
        if args.verify_only:
            manifest = validate_artifact_set(args.output)
        else:
            manifest = export_all(
                args.model_dir, Path(args.catalog).resolve(), args.output, tools_dir,
                variants, args.replace,
            )
    except (ExportError, OSError, ValueError) as error:
        print(f"TranslateGemma export failed: {error}", file=sys.stderr)
        return 1
    for name, variant in manifest["variants"].items():
        print(
            f"{name}: {variant['tensor_count']} tensors, "
            f"{variant['payload_size']} payload bytes"
        )
    print(f"{'Verified' if args.verify_only else 'Published'} {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())