import argparse
import collections
import hashlib
import importlib.metadata
import json
import os
import random
import shutil
import struct
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MODEL = ROOT / "experimental/snapdragon/models/glm-ocr"
OUTPUT = ROOT / "experimental/snapdragon/models/glm-ocr-tokenizer-v2"
PATTERN = r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+"
TASKS = ("Text Recognition:", "Formula Recognition:", "Table Recognition:")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def envelope(payload, kind, catalog):
    sources = {record["name"]: record for record in catalog["files"]}
    header = bytearray(128)
    header[:8] = b"GLMOCR2\0"
    struct.pack_into("<III", header, 8, 1, kind, len(payload))
    header[32:52] = bytes.fromhex(catalog["revision"])
    header[52:72] = bytes.fromhex(sources["tokenizer.json"]["git_blob_sha1"])
    header[72:92] = bytes.fromhex(sources["tokenizer_config.json"]["git_blob_sha1"])
    header[96:128] = hashlib.sha256(header[:96] + payload).digest()
    return bytes(header) + payload


def reference_tokenizer(model, require_version=True):
    from tokenizers import Tokenizer

    if require_version and importlib.metadata.version("tokenizers") != "0.22.2":
        raise ValueError("Reference export requires tokenizers==0.22.2")
    tokenizer = Tokenizer.from_file(str(model / "tokenizer.json"))
    return tokenizer


def byte_alphabet():
    values = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    characters = values.copy()
    for value in range(256):
        if value not in values:
            values.append(value)
            characters.append(256 + len(characters) - 188)
    return {chr(character): value for value, character in zip(values, characters)}


def tables(model, tokenizer):
    from tokenizers import Regex, pre_tokenizers

    data = json.loads(tokenizer.to_str())
    if data["normalizer"] is not None or data["model"]["ignore_merges"] is not True:
        raise ValueError("Unexpected normalization/BPE mode")
    if data["pre_tokenizer"] != {
        "type": "Sequence", "pretokenizers": [
            {"type": "Split", "pattern": {"Regex": PATTERN}, "behavior": "Isolated", "invert": False},
            {"type": "ByteLevel", "add_prefix_space": False, "trim_offsets": True, "use_regex": False},
        ],
    }:
        raise ValueError("Unsupported pre-tokenizer")
    vocabulary = data["model"]["vocab"]
    alphabet = byte_alphabet()
    pieces = [None] * tokenizer.get_vocab_size()
    flags = [0] * len(pieces)
    for text, token in vocabulary.items():
        pieces[token] = bytes(alphabet[character] for character in text)
    for added in data["added_tokens"]:
        if any(added[key] for key in ("single_word", "lstrip", "rstrip", "normalized")):
            raise ValueError("Unsupported added token flags")
        pieces[added["id"]] = added["content"].encode("utf-8")
        flags[added["id"]] = 1 | (2 if added["special"] else 0)
    if len(vocabulary) != 59246 or len(pieces) != 59282 or any(not piece for piece in pieces):
        raise ValueError("Unexpected vocabulary inventory")
    lexical = sorted(range(len(vocabulary)), key=lambda token: pieces[token])
    byte_ids = {pieces[token][0]: token for token in range(len(vocabulary)) if len(pieces[token]) == 1}
    if len(byte_ids) != 256:
        raise ValueError("Incomplete byte vocabulary")
    pairs = {}
    for rank, (left, right) in enumerate(data["model"]["merges"]):
        pairs[vocabulary[left], vocabulary[right]] = rank, vocabulary[left + right]
    if len(pairs) != 106026:
        raise ValueError("Unexpected or duplicate merge pairs")
    characters = "".join(chr(value) for value in range(0x110000) if not 0xD800 <= value <= 0xDFFF)
    classes = bytearray(0x110000)
    for category, flag in ((r"\p{L}", 1), (r"\p{N}", 2), (r"\s", 4)):
        splitter = pre_tokenizers.Split(Regex("[^" + category + "]+"), behavior="removed")
        for text, _ in splitter.pre_tokenize_str(characters):
            for character in text:
                classes[ord(character)] |= flag
    ranges = []
    begin = 0
    while begin < len(classes):
        end = begin + 1
        while end < len(classes) and classes[end] == classes[begin]:
            end += 1
        if classes[begin]:
            ranges.append((begin, end - 1, classes[begin]))
        begin = end
    strings = b"".join(pieces)
    payload = bytearray(struct.pack("<8I", len(pieces), len(pairs), len(ranges), len(strings), len(vocabulary), 36, 0, 0))
    offset = 0
    for piece, flag in zip(pieces, flags):
        payload += struct.pack("<III", offset, len(piece), flag)
        offset += len(piece)
    payload += struct.pack("<" + "I" * len(lexical), *lexical)
    for (left, right), (rank, merged) in sorted(pairs.items()):
        payload += struct.pack("<4I", left, right, rank, merged)
    for record in ranges:
        payload += struct.pack("<3I", *record)
    payload += struct.pack("<256I", *(byte_ids[value] for value in range(256)))
    payload += strings
    return bytes(payload), pieces, byte_ids, {
        "piece_count": len(pieces), "merge_count": len(pairs), "unicode_ranges": len(ranges),
        "max_piece_bytes": max(map(len, pieces)), "special_ids": [index for index, flag in enumerate(flags) if flag & 2],
    }


def fixture_data(model, tokenizer, pieces, byte_ids):
    import jinja2

    if importlib.metadata.version("jinja2") != "3.1.6":
        raise ValueError("Reference export requires jinja2==3.1.6")
    cases = []

    def encode_case(text):
        ids = tokenizer.encode(text, add_special_tokens=False).ids
        cases.append((0, text.encode("utf-8"), ids, tokenizer.decode(ids, skip_special_tokens=False).encode("utf-8")))

    samples = ["", "Text Recognition:", "Formula Recognition:", "Table Recognition:",
               "Guten Tag! Stra\u00dfe, \u00c4\u00d6\u00dc \u00e4\u00f6\u00fc \u00df \u20ac 1.234,56",
               "\u65e5\u672c\u8a9e \u4e2d\u6587 \ud55c\uad6d\uc5b4", "\u0627\u0644\u0639\u0631\u0628\u064a\u0629",
               "\u043f\u0440\u0438\u0432\u0435\u0442", "\U0001f600\u200d\U0001f4bb e\u0301 \u00e9",
               "can't I'VE you're 'S '\u017f \r\n\t", "123456789012345 \u00b2\u2160\u0661\u0968",
               "a\u0000b\u001fc", "  \r\n  \t x  ", "<table><tr><td>1</td></tr></table>",
               "\\frac{a_1}{b^2} = 3.14", "A" * 2048]
    for value in range(128):
        samples.extend([chr(value), "a" + chr(value) + "b", " " + chr(value) + "12\n"])
    for record in read_json(model / "tokenizer.json")["added_tokens"]:
        samples.extend([record["content"], "x" + record["content"] + "y"])
    randomizer = random.Random(20260916)
    alphabet = "abZ019 -_'\r\n\t.,:!\u00e4\u00df\u00a0\u2003\u2028\u0301\u65e5\u672c\u0661\u2160\U0001f600\u017f"
    for _ in range(1200):
        samples.append("".join(randomizer.choice(alphabet) for _ in range(randomizer.randrange(1, 180))))
    for _ in range(1000):
        values = [randomizer.randrange(0x110000) for _ in range(randomizer.randrange(1, 15))]
        samples.append("a " + "".join(chr(value) for value in values if not 0xD800 <= value <= 0xDFFF) + " 123")
    for piece in pieces[:59246]:
        try:
            text = piece.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if len(text) and len(text) <= 8192:
            encode_case(text)
    for text in samples:
        encode_case(text)
    for token in range(len(pieces)):
        for skip in (False, True):
            ids = [token]
            cases.append((2 if skip else 1, b"", ids, tokenizer.decode(ids, skip_special_tokens=skip).encode("utf-8")))
    for _ in range(1000):
        ids = [byte_ids[randomizer.randrange(256)] for _ in range(randomizer.randrange(1, 24))]
        cases.append((1, b"", ids, tokenizer.decode(ids, skip_special_tokens=False).encode("utf-8")))
    template = jinja2.Environment(trim_blocks=True, lstrip_blocks=True).from_string((model / "chat_template.jinja").read_text(encoding="utf-8"))
    prompt_examples = []
    for task, text in enumerate(TASKS):
        for count in (1, 4, 16, 144, 512):
            for no_think in (0, 1):
                kwargs = dict(messages=[{"role": "user", "content": [{"type": "image"}, {"type": "text", "text": text}]}],
                              tools=None, add_generation_prompt=True)
                if no_think:
                    kwargs["enable_thinking"] = False
                rendered = template.render(**kwargs).replace("<|image|>", "<|image|>" * count)
                ids = tokenizer.encode(rendered, add_special_tokens=False).ids
                cases.append((3, struct.pack("<3I", task, count, no_think), ids, rendered.encode("utf-8")))
                if count == 1:
                    prompt_examples.append({"task": task, "no_think": no_think, "text": rendered, "ids": ids})
    payload = bytearray(struct.pack("<I", len(cases)))
    for mode, text, ids, decoded in cases:
        payload += struct.pack("<4I", mode, len(text), len(ids), len(decoded))
        payload += text
        payload += struct.pack("<" + "I" * len(ids), *ids)
        payload += decoded
    return bytes(payload), len(cases), prompt_examples


def export(model, output):
    catalog = verified_sources(model)
    tokenizer = reference_tokenizer(model)
    payload, pieces, byte_ids, info = tables(model, tokenizer)
    fixtures, count, examples = fixture_data(model, tokenizer, pieces, byte_ids)
    artifacts = {"tokenizer.got": envelope(payload, 1, catalog), "tokenizer-fixtures.got": envelope(fixtures, 2, catalog)}
    manifest = {
        "schema_version": 1, "source_revision": catalog["revision"], "source_catalog_sha256": sha(Path(__file__).with_name("glm-ocr-model.json").read_bytes()),
        "exporter_sha256": sha(Path(__file__).read_bytes()), "tokenizers": "0.22.2", "jinja2": "3.1.6",
        "reference_contract": "raw tokenizer.json; wrapper preserves added-token flags; no automatic BOS/EOS",
        "fixture_count": count, "table": info, "prompt_examples": examples,
        "inference_ready": False, "npu_validated": False,
        "files": {name: {"size": len(data), "sha256": sha(data)} for name, data in artifacts.items()},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="glm-ocr-export-", dir=output.parent))
    try:
        for name, data in artifacts.items():
            (stage / name).write_bytes(data)
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
        if output.exists():
            for name, data in artifacts.items():
                if (output / name).read_bytes() != data:
                    raise ValueError("Existing export differs; choose a new output directory")
            os.replace(stage / "manifest.json", output / "manifest.json")
        else:
            os.replace(stage, output)
        print("PASS exported tokenizer:", info, "fixtures:", count, "path:", output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def wrapper_check(model, output):
    from transformers import AutoTokenizer

    if importlib.metadata.version("transformers") != "5.17.0":
        raise ValueError("Wrapper check requires transformers==5.17.0")
    verified_sources(model)
    tokenizer = AutoTokenizer.from_pretrained(str(model), local_files_only=True, trust_remote_code=False)
    expected = reference_tokenizer(model, require_version=False)
    actual = tokenizer.backend_tokenizer
    if json.loads(actual.to_str()) != json.loads(expected.to_str()):
        raise ValueError("AutoTokenizer backend differs from export contract")
    manifest = read_json(output / "manifest.json")
    data = (output / "tokenizer-fixtures.got").read_bytes()
    if sha(data) != manifest["files"]["tokenizer-fixtures.got"]["sha256"]:
        raise ValueError("Fixture identity mismatch")
    count = struct.unpack_from("<I", data, 128)[0]
    offset = 132
    for index in range(count):
        mode, text_size, id_count, decoded_size = struct.unpack_from("<4I", data, offset)
        offset += 16
        text = data[offset:offset + text_size]
        offset += text_size
        ids = list(struct.unpack_from("<" + "I" * id_count, data, offset))
        offset += id_count * 4
        decoded = data[offset:offset + decoded_size]
        offset += decoded_size
        if mode == 0 and tokenizer.encode(text.decode("utf-8"), add_special_tokens=False) != ids:
            raise ValueError("Wrapper encode mismatch at fixture " + str(index))
        if tokenizer.decode(ids, skip_special_tokens=mode == 2, clean_up_tokenization_spaces=False).encode("utf-8") != decoded:
            raise ValueError("Wrapper decode mismatch at fixture " + str(index))
    if offset != len(data):
        raise ValueError("Trailing fixture bytes")
    for example in manifest["prompt_examples"]:
        kwargs = dict(conversation=[{"role": "user", "content": [{"type": "image"}, {"type": "text", "text": TASKS[example["task"]]}]}],
                      tokenize=False, add_generation_prompt=True)
        if example["no_think"]:
            kwargs["enable_thinking"] = False
        rendered = tokenizer.apply_chat_template(**kwargs)
        if rendered != example["text"] or tokenizer.encode(rendered, add_special_tokens=False) != example["ids"]:
            raise ValueError("AutoTokenizer prompt mismatch")
    report = {"transformers": "5.17.0", "tokenizers": importlib.metadata.version("tokenizers"),
              "source_revision": manifest["source_revision"], "backend_equal": True, "prompts_equal": len(manifest["prompt_examples"]),
              "fixtures_equal": count, "fixtures_sha256": sha(data)}
    (output / "wrapper-check.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS Transformers wrapper parity:", report)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def verified_sources(model):
    catalog = read_json(Path(__file__).with_name("glm-ocr-model.json"))
    for record in catalog["files"]:
        if record["name"] == "model.safetensors":
            continue
        data = (model / record["name"]).read_bytes()
        if len(data) != record["size"]:
            raise ValueError("Source size mismatch: " + record["name"])
        blob = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
        if hashlib.sha1(blob).hexdigest() != record["git_blob_sha1"]:
            raise ValueError("Source identity mismatch: " + record["name"])
    return catalog


def inspect(model):
    catalog = verified_sources(model)
    tokenizer = read_json(model / "tokenizer.json")
    print("Revision:", catalog["revision"])
    for key in ("normalizer", "pre_tokenizer", "post_processor", "decoder"):
        print(key, json.dumps(tokenizer[key], ensure_ascii=True))
    print("BPE config:", {key: value for key, value in tokenizer["model"].items() if key not in ("vocab", "merges")})
    print("Vocabulary:", len(tokenizer["model"]["vocab"]), "merges:", len(tokenizer["model"]["merges"]))
    print("Added:", json.dumps(tokenizer["added_tokens"], ensure_ascii=True))
    for package in ("tokenizers", "jinja2", "regex", "transformers"):
        try:
            print(package, importlib.metadata.version(package))
        except importlib.metadata.PackageNotFoundError:
            print(package, "unavailable")
    with (model / "model.safetensors").open("rb") as stream:
        header = json.loads(stream.read(int.from_bytes(stream.read(8), "little")))
    groups = collections.Counter()
    for name, tensor in header.items():
        if name == "__metadata__":
            continue
        if name.startswith("model.language_model.layers.16."):
            group = "unused_mtp_layer16"
        elif name.startswith("model.visual."):
            group = "vision"
        elif name.startswith("model.language_model.") or name == "lm_head.weight":
            group = "text_and_head"
        else:
            group = "UNKNOWN:" + name
        elements = 1
        for dimension in tensor["shape"]:
            elements *= dimension
        groups[group] += elements
    print("Stored elements by reference role:", json.dumps(groups))


def main():
    parser = argparse.ArgumentParser(description="Offline GLM-OCR artifact preparation; never used by production inference")
    parser.add_argument("--model-dir", type=Path, default=MODEL)
    parser.add_argument("--inspect", action="store_true")
    parser.add_argument("--export-tokenizer", action="store_true")
    parser.add_argument("--wrapper-check", action="store_true")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    if args.inspect:
        inspect(args.model_dir)
    elif args.export_tokenizer:
        export(args.model_dir, args.output)
    elif args.wrapper_check:
        wrapper_check(args.model_dir, args.output)
    else:
        parser.error("select --inspect, --export-tokenizer or --wrapper-check")


if __name__ == "__main__":
    main()