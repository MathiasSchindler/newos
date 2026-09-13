#!/usr/bin/env python3
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).with_name("validate-translategemma.py")
SPEC = importlib.util.spec_from_file_location("validate_translategemma", SCRIPT)
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def config_fixture():
    return {
        "architectures": ["Gemma3ForConditionalGeneration"],
        "model_type": "gemma3",
        "dtype": "bfloat16",
        "text_config": {
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
            "rope_theta": 1000000.0,
            "rope_local_base_freq": 10000.0,
            "rope_parameters": {
                "full_attention": {"factor": 8.0, "rope_type": "linear"},
                "sliding_attention": {"rope_type": "default"},
            },
            "layer_types": [
                "full_attention" if (layer + 1) % 6 == 0 else "sliding_attention"
                for layer in range(34)
            ],
        },
    }


def write_json(path, value):
    Path(path).write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")


def write_safetensors(path, tensors):
    header = {}
    payload = bytearray()
    for name, shape in tensors:
        size = 2
        for dimension in shape:
            size *= dimension
        start = len(payload)
        payload.extend(b"\0" * size)
        header[name] = {"dtype": "BF16", "shape": shape, "data_offsets": [start, len(payload)]}
    encoded = json.dumps(header, separators=(",", ":")).encode("utf-8")
    Path(path).write_bytes(struct.pack("<Q", len(encoded)) + encoded + payload)


class Stage2ValidationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.model_dir = self.root / "model"
        self.model_dir.mkdir()
        self.catalog_path = self.root / "catalog.json"
        self.audit_path = self.root / "audit.json"
        self.tensors = [
            ("language_model.model.embed_tokens.weight", [2, 2]),
            ("multi_modal_projector.mm_soft_emb_norm.weight", [2]),
            ("vision_tower.vision_model.post_layernorm.bias", [1]),
        ]
        self.write_fixture()

    def tearDown(self):
        self.temporary.cleanup()

    def write_fixture(self, tensors=None, weight_map=None):
        tensors = self.tensors if tensors is None else tensors
        write_json(self.model_dir / "config.json", config_fixture())
        write_safetensors(self.model_dir / "model.safetensors", tensors)
        if weight_map is None:
            weight_map = {name: "model.safetensors" for name, _ in tensors}
        raw_bytes = sum(2 * math_product(shape) for _, shape in tensors)
        write_json(self.model_dir / "model.safetensors.index.json", {
            "metadata": {"total_size": raw_bytes},
            "weight_map": weight_map,
        })
        files = []
        for name, role in [
            ("config.json", "config"),
            ("model.safetensors", "weights"),
            ("model.safetensors.index.json", "weights_index"),
        ]:
            data = (self.model_dir / name).read_bytes()
            files.append({
                "name": name,
                "role": role,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            })
        retained = [(name, shape) for name, shape in tensors if name.startswith("language_model.")]
        omitted = [(name, shape) for name, shape in tensors if name.startswith(("vision_tower.", "multi_modal_projector."))]
        catalog = {
            "schema_version": 1,
            "model": {
                "name": "translategemma-4b-it",
                "repository": VALIDATOR.REPOSITORY,
                "revision": VALIDATOR.REVISION,
                "license": "gemma",
                "gated": True,
                "requires_license_acceptance": True,
            },
            "inventory": inventory(tensors, retained, omitted),
            "files": files,
        }
        write_json(self.catalog_path, catalog)

    def refresh_catalog_file(self, name):
        catalog = json.loads(self.catalog_path.read_text(encoding="utf-8"))
        data = (self.model_dir / name).read_bytes()
        entry = next(item for item in catalog["files"] if item["name"] == name)
        entry["size"] = len(data)
        entry["sha256"] = hashlib.sha256(data).hexdigest()
        write_json(self.catalog_path, catalog)

    def assert_validation_fails(self, text):
        with self.assertRaisesRegex(VALIDATOR.ValidationError, text):
            VALIDATOR.validate_checkpoint(self.catalog_path, self.model_dir, self.audit_path)

    def test_valid_checkpoint_emits_auditable_selection(self):
        audit = VALIDATOR.validate_checkpoint(self.catalog_path, self.model_dir, self.audit_path)
        self.assertEqual(audit["totals"]["retained_tensor_count"], 1)
        self.assertEqual(audit["totals"]["omitted_tensor_count"], 2)
        self.assertEqual([item["disposition"] for item in audit["tensors"]], ["retained", "omitted", "omitted"])

    def test_config_drift_fails_after_integrity_check(self):
        config = config_fixture()
        config["text_config"]["hidden_size"] = 2304
        write_json(self.model_dir / "config.json", config)
        self.refresh_catalog_file("config.json")
        self.assert_validation_fails("config hidden_size")

    def test_explicit_null_for_omitted_default_fails(self):
        config = config_fixture()
        config["text_config"]["pad_token_id"] = None
        write_json(self.model_dir / "config.json", config)
        self.refresh_catalog_file("config.json")
        self.assert_validation_fails("config pad_token_id")

    def test_hash_mismatch_fails(self):
        path = self.model_dir / "config.json"
        data = bytearray(path.read_bytes())
        data[-2] = ord(" ") if data[-2] != ord(" ") else ord("x")
        path.write_bytes(data)
        self.assert_validation_fails("SHA-256 mismatch")

    def test_truncated_shard_fails(self):
        path = self.model_dir / "model.safetensors"
        path.write_bytes(path.read_bytes()[:-1])
        self.refresh_catalog_file("model.safetensors")
        self.assert_validation_fails("truncated or has trailing data")

    def test_wrong_shard_mapping_fails(self):
        index = json.loads((self.model_dir / "model.safetensors.index.json").read_text(encoding="utf-8"))
        first = next(iter(index["weight_map"]))
        index["weight_map"][first] = "other.safetensors"
        write_json(self.model_dir / "model.safetensors.index.json", index)
        self.refresh_catalog_file("model.safetensors.index.json")
        self.assert_validation_fails("shard set")

    def test_unexpected_tensor_namespace_fails(self):
        tensors = copy.deepcopy(self.tensors)
        tensors[0] = ("unknown.weight", [2, 2])
        self.write_fixture(tensors=tensors)
        self.assert_validation_fails("unexpected tensor namespace")

    def test_non_bf16_tensor_fails(self):
        path = self.model_dir / "model.safetensors"
        data = path.read_bytes().replace(b'"BF16"', b'"F16 "', 1)
        path.write_bytes(data)
        self.refresh_catalog_file("model.safetensors")
        self.assert_validation_fails("expected BF16")

    def test_truncated_safetensors_prefix_fails(self):
        path = self.model_dir / "model.safetensors"
        path.write_bytes(b"short")
        self.refresh_catalog_file("model.safetensors")
        self.assert_validation_fails("truncated Safetensors prefix")

    def test_duplicate_tensor_header_key_fails(self):
        path = self.model_dir / "model.safetensors"
        entry = '{"dtype":"BF16","shape":[1],"data_offsets":[0,2]}'
        encoded = ('{"language_model.a":' + entry + ',"language_model.a":' + entry + '}').encode("utf-8")
        path.write_bytes(struct.pack("<Q", len(encoded)) + encoded + b"\0\0")
        self.refresh_catalog_file("model.safetensors")
        self.assert_validation_fails("duplicate JSON key")

    def test_unexpected_file_fails(self):
        (self.model_dir / "foreign.bin").write_bytes(b"foreign")
        self.assert_validation_fails("unexpected checkpoint files")

    def test_missing_file_fails(self):
        (self.model_dir / "config.json").unlink()
        self.assert_validation_fails("missing checkpoint files")


def math_product(shape):
    result = 1
    for dimension in shape:
        result *= dimension
    return result


def inventory(tensors, retained, omitted):
    def parameters(items):
        return sum(math_product(shape) for _, shape in items)

    return {
        "stored_tensor_count": len(tensors),
        "stored_parameter_count": parameters(tensors),
        "stored_raw_bytes": parameters(tensors) * 2,
        "retained_tensor_count": len(retained),
        "retained_parameter_count": parameters(retained),
        "retained_raw_bytes": parameters(retained) * 2,
        "omitted_tensor_count": len(omitted),
        "omitted_parameter_count": parameters(omitted),
        "omitted_raw_bytes": parameters(omitted) * 2,
    }


if __name__ == "__main__":
    unittest.main()