#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import sys


REPOSITORY = "google/translategemma-4b-it"
REVISION = "10042cb0e6e7fdce748996a71dc3dc432a4e0c89"
DTYPE_BYTES = {
    "BOOL": 1, "U8": 1, "I8": 1, "F8_E4M3": 1, "F8_E5M2": 1,
    "I16": 2, "U16": 2, "F16": 2, "BF16": 2,
    "I32": 4, "U32": 4, "F32": 4,
    "I64": 8, "U64": 8, "F64": 8,
}
GENERATED_FILES = {"source-lock.json", "tensor-audit.json"}
MISSING = object()


class ValidationError(Exception):
    pass


def _object_no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path):
    try:
        with Path(path).open("r", encoding="utf-8") as stream:
            return json.load(stream, object_pairs_hook=_object_no_duplicates)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"cannot parse {path}: {error}") from error


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def validate_catalog(catalog):
    if catalog.get("schema_version") != 1:
        raise ValidationError("unsupported catalog schema")
    model = catalog.get("model")
    if not isinstance(model, dict):
        raise ValidationError("catalog model is missing")
    if model.get("repository") != REPOSITORY or model.get("revision") != REVISION:
        raise ValidationError("catalog repository or revision is not canonical")
    if model.get("license") != "gemma" or model.get("gated") is not True or \
            model.get("requires_license_acceptance") is not True:
        raise ValidationError("catalog does not enforce the Gemma license gate")
    files = catalog.get("files")
    if not isinstance(files, list) or not files:
        raise ValidationError("catalog file list is empty")
    seen = set()
    for entry in files:
        name = entry.get("name") if isinstance(entry, dict) else None
        if not isinstance(name, str) or not name or Path(name).name != name:
            raise ValidationError(f"unsafe catalog filename: {name!r}")
        if name in seen:
            raise ValidationError(f"duplicate catalog file: {name}")
        seen.add(name)
        if not isinstance(entry.get("size"), int) or entry["size"] < 0:
            raise ValidationError(f"invalid catalog size for {name}")
        digest = entry.get("sha256")
        if digest is not None and (
            not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        ):
            raise ValidationError(f"invalid SHA-256 for {name}")
        git_blob = entry.get("git_blob_sha1")
        if git_blob is not None and (
            not isinstance(git_blob, str) or re.fullmatch(r"[0-9a-f]{40}", git_blob) is None
        ):
            raise ValidationError(f"invalid Git blob SHA-1 for {name}")
    return files


def verify_files(catalog, model_dir):
    files = validate_catalog(catalog)
    model_dir = Path(model_dir)
    expected = {entry["name"] for entry in files}
    actual = {path.name for path in model_dir.iterdir() if path.is_file()}
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected - GENERATED_FILES)
    if missing:
        raise ValidationError(f"missing checkpoint files: {', '.join(missing)}")
    if unexpected:
        raise ValidationError(f"unexpected checkpoint files: {', '.join(unexpected)}")
    for entry in files:
        path = model_dir / entry["name"]
        size = path.stat().st_size
        if size != entry["size"]:
            raise ValidationError(
                f"size mismatch for {entry['name']}: expected {entry['size']}, got {size}"
            )
        expected_hash = entry.get("sha256")
        if expected_hash is None:
            raise ValidationError(f"catalog SHA-256 is unresolved for {entry['name']}")
        actual_hash = sha256_file(path)
        if actual_hash.lower() != expected_hash.lower():
            raise ValidationError(f"SHA-256 mismatch for {entry['name']}")


def _expect(config, key, expected):
    actual = config.get(key)
    if actual != expected:
        raise ValidationError(f"config {key} is {actual!r}; expected {expected!r}")


def _expect_default(config, key, expected):
    actual = config.get(key, MISSING)
    if actual is not MISSING and actual != expected:
        raise ValidationError(f"config {key} is {actual!r}; expected {expected!r}")


def validate_config(path):
    config = load_json(path)
    _expect(config, "model_type", "gemma3")
    _expect(config, "dtype", "bfloat16")
    architectures = config.get("architectures")
    if architectures != ["Gemma3ForConditionalGeneration"]:
        raise ValidationError("config architectures do not identify Gemma3ForConditionalGeneration")
    text = config.get("text_config")
    if not isinstance(text, dict):
        raise ValidationError("config text_config is missing")
    expected = {
        "model_type": "gemma3_text",
        "vocab_size": 262208,
        "hidden_size": 2560,
        "intermediate_size": 10240,
        "num_hidden_layers": 34,
        "num_attention_heads": 8,
        "num_key_value_heads": 4,
        "head_dim": 256,
        "hidden_activation": "gelu_pytorch_tanh",
        "max_position_embeddings": 131072,
        "rms_norm_eps": 0.000001,
        "query_pre_attn_scalar": 256,
        "sliding_window": 1024,
        "_sliding_window_pattern": 6,
        "dtype": "bfloat16",
        "attention_bias": False,
        "attention_dropout": 0.0,
        "use_bidirectional_attention": False,
    }
    for key, value in expected.items():
        _expect(text, key, value)
    for key, value in {
        "pad_token_id": 0,
        "eos_token_id": 1,
        "bos_token_id": 2,
        "tie_word_embeddings": True,
    }.items():
        _expect_default(text, key, value)
    expected_layers = [
        "full_attention" if (layer + 1) % 6 == 0 else "sliding_attention"
        for layer in range(34)
    ]
    _expect(text, "layer_types", expected_layers)
    global_theta = text.get("rope_theta")
    local_theta = text.get("rope_local_base_freq")
    rope = text.get("rope_parameters")
    if isinstance(rope, dict):
        full = rope.get("full_attention")
        sliding = rope.get("sliding_attention")
        if global_theta is None and isinstance(full, dict):
            global_theta = full.get("rope_theta")
        if local_theta is None and isinstance(sliding, dict):
            local_theta = sliding.get("rope_theta")
    if global_theta != 1000000.0 or local_theta != 10000.0:
        raise ValidationError("config RoPE frequencies do not match TranslateGemma 4B")
    if not isinstance(rope, dict) or rope.get("full_attention") != {
        "factor": 8.0, "rope_type": "linear"
    } or rope.get("sliding_attention") != {"rope_type": "default"}:
        raise ValidationError("config RoPE scaling does not match TranslateGemma 4B")


def read_safetensors_header(path):
    path = Path(path)
    try:
        with path.open("rb") as stream:
            prefix = stream.read(8)
            if len(prefix) != 8:
                raise ValidationError(f"truncated Safetensors prefix: {path.name}")
            header_size = struct.unpack("<Q", prefix)[0]
            if header_size == 0 or header_size > 100 * 1024 * 1024:
                raise ValidationError(f"invalid Safetensors header size: {path.name}")
            header_bytes = stream.read(header_size)
            if len(header_bytes) != header_size:
                raise ValidationError(f"truncated Safetensors header: {path.name}")
    except OSError as error:
        raise ValidationError(f"cannot read {path}: {error}") from error
    try:
        header = json.loads(
            header_bytes.decode("utf-8"), object_pairs_hook=_object_no_duplicates
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"invalid Safetensors JSON in {path.name}: {error}") from error
    if not isinstance(header, dict):
        raise ValidationError(f"Safetensors header is not an object: {path.name}")
    tensors = {}
    intervals = []
    for name, entry in header.items():
        if name == "__metadata__":
            if not isinstance(entry, dict):
                raise ValidationError(f"invalid Safetensors metadata in {path.name}")
            continue
        if not isinstance(name, str) or not name or not isinstance(entry, dict):
            raise ValidationError(f"invalid tensor entry in {path.name}")
        if set(entry) != {"dtype", "shape", "data_offsets"}:
            raise ValidationError(f"invalid tensor metadata for {name}")
        dtype = entry["dtype"]
        shape = entry["shape"]
        offsets = entry["data_offsets"]
        if dtype != "BF16":
            raise ValidationError(f"tensor {name} has dtype {dtype!r}; expected BF16")
        if dtype not in DTYPE_BYTES or not isinstance(shape, list) or \
                any(not isinstance(dim, int) or dim < 0 for dim in shape) or \
                not isinstance(offsets, list) or len(offsets) != 2 or \
                any(not isinstance(offset, int) or offset < 0 for offset in offsets) or \
                offsets[1] < offsets[0]:
            raise ValidationError(f"invalid tensor shape, dtype, or offsets for {name}")
        parameters = math.prod(shape)
        raw_bytes = parameters * DTYPE_BYTES[dtype]
        if offsets[1] - offsets[0] != raw_bytes:
            raise ValidationError(f"tensor byte span does not match shape for {name}")
        tensors[name] = {
            "dtype": dtype,
            "shape": shape,
            "parameters": parameters,
            "raw_bytes": raw_bytes,
        }
        intervals.append((offsets[0], offsets[1], name))
    cursor = 0
    for start, end, name in sorted(intervals):
        if start != cursor:
            raise ValidationError(f"non-contiguous or overlapping tensor data at {name}")
        cursor = end
    if 8 + header_size + cursor != path.stat().st_size:
        raise ValidationError(f"Safetensors payload is truncated or has trailing data: {path.name}")
    return tensors


def classify_tensor(name):
    if name.startswith("language_model."):
        return "retained"
    if name.startswith("vision_tower.") or name.startswith("multi_modal_projector."):
        return "omitted"
    raise ValidationError(f"unexpected tensor namespace: {name}")


def validate_inventory(catalog, model_dir):
    model_dir = Path(model_dir)
    index_entries = [entry for entry in catalog["files"] if entry.get("role") == "weights_index"]
    shard_entries = [entry for entry in catalog["files"] if entry.get("role") == "weights"]
    if len(index_entries) != 1 or not shard_entries:
        raise ValidationError("catalog must contain one weights index and at least one shard")
    index = load_json(model_dir / index_entries[0]["name"])
    weight_map = index.get("weight_map")
    metadata = index.get("metadata")
    if not isinstance(weight_map, dict) or not weight_map:
        raise ValidationError("Safetensors index weight_map is missing")
    if not isinstance(metadata, dict) or not isinstance(metadata.get("total_size"), int):
        raise ValidationError("Safetensors index total_size is missing")
    expected_shards = {entry["name"] for entry in shard_entries}
    mapped_shards = set(weight_map.values())
    if mapped_shards != expected_shards:
        raise ValidationError("Safetensors index shard set does not match the catalog")
    tensors = {}
    tensor_shards = {}
    for entry in shard_entries:
        shard_name = entry["name"]
        for name, tensor in read_safetensors_header(model_dir / shard_name).items():
            if name in tensors:
                raise ValidationError(f"tensor appears in multiple shards: {name}")
            tensors[name] = tensor
            tensor_shards[name] = shard_name
    if set(weight_map) != set(tensors):
        missing = sorted(set(weight_map) - set(tensors))
        extra = sorted(set(tensors) - set(weight_map))
        raise ValidationError(
            f"Safetensors index/header mismatch; missing={missing[:3]}, extra={extra[:3]}"
        )
    for name, shard_name in weight_map.items():
        if tensor_shards[name] != shard_name:
            raise ValidationError(f"Safetensors index maps {name} to the wrong shard")
    audit_tensors = []
    totals = {
        "stored_tensor_count": 0,
        "stored_parameter_count": 0,
        "stored_raw_bytes": 0,
        "retained_tensor_count": 0,
        "retained_parameter_count": 0,
        "retained_raw_bytes": 0,
        "omitted_tensor_count": 0,
        "omitted_parameter_count": 0,
        "omitted_raw_bytes": 0,
    }
    for name in sorted(tensors):
        tensor = tensors[name]
        disposition = classify_tensor(name)
        totals["stored_tensor_count"] += 1
        totals["stored_parameter_count"] += tensor["parameters"]
        totals["stored_raw_bytes"] += tensor["raw_bytes"]
        totals[f"{disposition}_tensor_count"] += 1
        totals[f"{disposition}_parameter_count"] += tensor["parameters"]
        totals[f"{disposition}_raw_bytes"] += tensor["raw_bytes"]
        audit_tensors.append({
            "name": name,
            "shard": tensor_shards[name],
            "dtype": tensor["dtype"],
            "shape": tensor["shape"],
            "parameters": tensor["parameters"],
            "raw_bytes": tensor["raw_bytes"],
            "disposition": disposition,
        })
    if metadata["total_size"] != totals["stored_raw_bytes"]:
        raise ValidationError("Safetensors index total_size does not match shard headers")
    expected_inventory = catalog.get("inventory")
    if not isinstance(expected_inventory, dict):
        raise ValidationError("catalog inventory is missing")
    for key, actual in totals.items():
        if expected_inventory.get(key) != actual:
            raise ValidationError(
                f"inventory {key} is {actual}; expected {expected_inventory.get(key)!r}"
            )
    return totals, audit_tensors, metadata


def write_json_atomic(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + ".partial")
    temporary.parent.mkdir(parents=True, exist_ok=True)
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
    os.replace(temporary, path)


def validate_checkpoint(catalog_path, model_dir, audit_path):
    catalog = load_json(catalog_path)
    verify_files(catalog, model_dir)
    validate_config(Path(model_dir) / "config.json")
    totals, tensors, metadata = validate_inventory(catalog, model_dir)
    audit = {
        "schema_version": 1,
        "source": catalog["model"],
        "safetensors_metadata": metadata,
        "totals": totals,
        "tensors": tensors,
    }
    write_json_atomic(audit_path, audit)
    return audit


def main():
    parser = argparse.ArgumentParser(description="Validate the pinned TranslateGemma checkpoint")
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--audit", required=True)
    args = parser.parse_args()
    try:
        audit = validate_checkpoint(args.catalog, args.model_dir, args.audit)
    except ValidationError as error:
        print(f"TranslateGemma validation failed: {error}", file=sys.stderr)
        return 1
    totals = audit["totals"]
    print(
        "TranslateGemma validation passed: "
        f"{totals['retained_tensor_count']} retained tensors, "
        f"{totals['retained_parameter_count']} parameters, "
        f"{totals['retained_raw_bytes']} raw bytes"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())