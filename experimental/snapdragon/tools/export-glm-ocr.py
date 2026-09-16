import argparse
import collections
import hashlib
import importlib.metadata
import json
import os
import random
import shutil
import struct
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MODEL = ROOT / "experimental/snapdragon/models/glm-ocr"
OUTPUT = ROOT / "experimental/snapdragon/models/glm-ocr-tokenizer-v2"
PATTERN = r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+"
TASKS = ("Text Recognition:", "Formula Recognition:", "Table Recognition:")


def image_reference():
    sys.path.insert(0, str(ROOT / "experimental/snapdragon/build/ocr-oracle"))
    import torch
    from transformers.models.glm46v.image_processing_glm46v import Glm46VImageProcessor, smart_resize
    versions = {name: importlib.metadata.version(name) for name in ("torch", "torchvision", "transformers", "numpy", "pillow")}
    required = {"torch": "2.14.0", "torchvision": "0.29.0", "transformers": "5.17.0", "numpy": "2.4.3", "pillow": "12.3.0"}
    if versions != required:
        raise ValueError("Image reference versions differ: " + str(versions))
    torch.set_num_threads(1)
    return torch, Glm46VImageProcessor, smart_resize, versions


def export_positions(model, output):
    import inspect as source_inspect
    from types import SimpleNamespace, MethodType
    catalog = verified_sources(model)
    torch, _, _, versions = image_reference()
    from transformers import AutoTokenizer
    from transformers.models.glm_ocr.modeling_glm_ocr import GlmOcrModel
    source_hash = sha(Path(source_inspect.getfile(GlmOcrModel)).read_bytes())
    if source_hash != "aea6387985dad1f0f5124f9344cc98849be8a7f2c26652ac3d914f6eefac6cc6":
        raise ValueError("mRoPE source drift")
    reference = SimpleNamespace(config=SimpleNamespace(vision_config=SimpleNamespace(spatial_merge_size=2)))
    reference.get_vision_position_ids = MethodType(GlmOcrModel.get_vision_position_ids, reference)
    tokenizer = AutoTokenizer.from_pretrained(str(model),local_files_only=True,trust_remote_code=False)
    records = []
    for grid_height,grid_width in [(2,2),(4,8),(8,4),(24,24),(48,32),(2,400),(180,134)]:
        count = grid_height*grid_width//4
        for task,text in enumerate(TASKS):
            for no_think in (0,1):
                kwargs = {"add_generation_prompt":True,"tokenize":False}
                if no_think:
                    kwargs["enable_thinking"] = False
                prompt = tokenizer.apply_chat_template([{"role":"user","content":[{"type":"image"},{"type":"text","text":text}]}],**kwargs)
                prompt = prompt.replace("<|image|>","<|image|>"*count)
                ids = tokenizer.encode(prompt,add_special_tokens=False)
                tensor = torch.tensor([ids])
                modalities = (tensor==59280).to(torch.int64)
                positions,delta = GlmOcrModel.get_rope_index(reference,tensor,modalities,torch.tensor([[1,grid_height,grid_width]]))
                record = struct.pack("<5Ii",task,grid_height,grid_width,no_think,len(ids),delta.item())
                record += struct.pack("<"+"I"*len(ids),*ids)
                record += positions.to(torch.int32).contiguous().numpy().astype("<i4").tobytes()
                record += modalities.to(torch.uint8).contiguous().numpy().tobytes()
                records.append(record)
    data = envelope(struct.pack("<I",len(records))+b"".join(records),4,catalog)
    output.mkdir(parents=True,exist_ok=True)
    target = output/"position-fixtures.got"
    if target.exists() and target.read_bytes() != data:
        raise ValueError("Existing position fixtures differ; choose a new output directory")
    if not target.exists():
        temporary = target.with_suffix(".partial")
        temporary.write_bytes(data)
        os.replace(temporary,target)
    report = {"source_revision":catalog["revision"],"versions":versions,"modeling_glm_ocr_sha256":source_hash,
              "size":len(data),"sha256":sha(data),"cases":len(records),"no_model_instantiation":True}
    (output/"positions-manifest.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print("PASS image position oracle:",json.dumps(report))


def export_images(model, output):
    import inspect as source_inspect
    catalog = verified_sources(model)
    torch, processor_class, smart_resize, versions = image_reference()
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont, ImageOps
    from transformers.image_processing_backends import TorchvisionBackend
    import torchvision.transforms.v2.functional._geometry as geometry_source
    pinned_sources = {
        "image_processing_glm46v.py":"f04651566fc809f9d801cc19359a02591f15066721264cee826010bfa3dbdf8a",
        "image_processing_backends.py":"5f3176903638125ee01af36e8c74741a7bab19581f3c13349a775b093cb1bbb0",
        "_geometry.py":"7eb5589c1997a1c0e933ac28ca5f4e634dd5c12f193293bed2ada32cb16f95f3",
    }
    sources = {}
    for module in (processor_class,TorchvisionBackend,geometry_source):
        path = Path(source_inspect.getfile(module))
        digest = sha(path.read_bytes())
        if digest != pinned_sources[path.name]:
            raise ValueError("Reference source drift: " + path.name)
        sources[path.name] = {"sha256":digest}
    if torch.version.git_version != "08187d9e0fba026dc8217405802ab5381dc88d90":
        raise ValueError("PyTorch source commit drift")
    processor = processor_class.from_pretrained(str(model), local_files_only=True)
    geometry = []
    shapes = [(1,1), (14,42), (28,28), (42,70), (56,56), (112,112), (336,336), (337,515),
              (28,5600), (28,5628), (2000,3000), (10000,10000), (1,201), (27,100), (105,133)]
    rng = np.random.default_rng(20260916)
    shapes += [(int(height), int(width)) for height, width in rng.integers(1,10001,size=(1000,2))]
    for height, width in shapes:
        try:
            resized = smart_resize(2,height,width,min_pixels=12544,max_pixels=9633792)
            geometry.append((height,width,*resized))
        except ValueError:
            geometry.append((height,width,0,0))
    cases = []

    def add_image(image):
        height,width = image.shape[:2]
        tensor = torch.from_numpy(image.copy()).permute(2,0,1).unsqueeze(0)
        resized = processor.resize(tensor,processor.size,processor.resample,28,2)
        result = processor(images=tensor[0],return_tensors="pt",input_data_format="channels_first")
        target_height,target_width = resized.shape[-2:]
        grid = result["image_grid_thw"].tolist()[0]
        if grid != [1,target_height//14,target_width//14]:
            raise ValueError("Unexpected reference grid")
        cases.append((height,width,target_height,target_width,image.tobytes(),
                      resized[0].permute(1,2,0).contiguous().numpy().tobytes(),
                      result["pixel_values"].float().contiguous().numpy().astype("<f4").tobytes()))

    for height,width in [(1,1),(14,42),(28,28),(42,70),(56,56),(112,112),(336,336),(337,515),(513,257),(64,1024)]:
        for pattern in range(3):
            rows, columns = np.indices((height,width))
            if pattern == 0:
                image = np.stack([(rows*17+columns*7)%256, (rows*3+columns*23)%256, (rows*31+columns*5)%256],axis=-1).astype(np.uint8)
            elif pattern == 1:
                image = rng.integers(0,256,size=(height,width,3),dtype=np.uint8)
            else:
                image = np.repeat((((rows//3+columns//5)%2)*255).astype(np.uint8)[...,None],3,axis=-1)
            add_image(image)
    font_path = Path(os.environ.get("WINDIR","C:/Windows"))/"Fonts/segoeui.ttf"
    document = Image.new("RGB",(448,672),"white")
    drawing = ImageDraw.Draw(document)
    font = ImageFont.truetype(str(font_path),18)
    for row,text in enumerate(["OCR Referenz / Reference", "Rechnung 2026-09-16", "Stra\u00dfe 12, M\u00fcnchen", "Preis: 1.234,56 EUR",
                               "Artikel       Menge     Summe", "Papier           12      24,00", "Text, Ziffern und Satzzeichen.", "a + b = 42; (x - 1) / 2"]):
        drawing.text((16,20+row*42),text,font=font,fill=(12,18,24))
    add_image(np.array(document))
    add_image(np.array(document.rotate(90,expand=True)))
    add_image(np.array(ImageOps.expand(document,border=(13,7,19,11),fill="white")))
    add_image(np.array(document.resize((2400,3200),Image.Resampling.NEAREST)))
    payload = bytearray(struct.pack("<II",len(geometry),len(cases)))
    for record in geometry:
        payload += struct.pack("<4I",*record)
    for height,width,target_height,target_width,image,resized,patches in cases:
        payload += struct.pack("<7I",height,width,target_height,target_width,len(image),len(resized),len(patches))
        payload += image + resized + patches
    data = envelope(payload,3,catalog)
    manifest = {"schema_version":1,"source_revision":catalog["revision"],"versions":versions,
                "torch_git_commit":torch.version.git_version,"document_font_sha256":sha(font_path.read_bytes()),
                "reference_sources":sources,"preprocessor_sha256":sha((model/"preprocessor_config.json").read_bytes()),
                "exporter_sha256":sha(Path(__file__).read_bytes()),"geometry_cases":len(geometry),"pixel_cases":len(cases),
                "size":len(data),"sha256":sha(data),"input":"RGB uint8; Torchvision CPU backend; antialiased bicubic",
                "inference_ready":False,"npu_validated":False}
    output.mkdir(parents=True,exist_ok=True)
    target = output/"image-fixtures.got"
    if target.exists() and target.read_bytes() != data:
        raise ValueError("Existing image fixtures differ; choose a new --image-output directory")
    if not target.exists():
        temporary = target.with_suffix(".partial")
        temporary.write_bytes(data)
        os.replace(temporary,target)
    (output/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    print("PASS image oracle export:",json.dumps(manifest))
    export_positions(model,output)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def envelope(payload, kind, catalog):
    sources = {record["name"]: record for record in catalog["files"]}
    header = bytearray(128)
    header[:8] = b"GLMOCR2\0"
    struct.pack_into("<III", header, 8, 1, kind, len(payload))
    header[32:52] = bytes.fromhex(catalog["revision"])
    header[52:72] = bytes.fromhex(sources["preprocessor_config.json" if kind in (3,4) else "tokenizer.json"]["git_blob_sha1"])
    header[72:92] = bytes.fromhex(sources["config.json" if kind in (3,4) else "tokenizer_config.json"]["git_blob_sha1"])
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
    parser.add_argument("--inspect-images", action="store_true")
    parser.add_argument("--export-images", action="store_true")
    parser.add_argument("--export-positions", action="store_true")
    parser.add_argument("--image-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-images-v2")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    if args.inspect_images:
        image_reference()
        import inspect as source_inspect
        for package in ("torch", "torchvision", "transformers", "numpy", "pillow"):
            try:
                print(package, importlib.metadata.version(package))
            except importlib.metadata.PackageNotFoundError:
                print(package, "unavailable")
        from transformers.image_processing_backends import TorchvisionBackend
        print(source_inspect.getsource(TorchvisionBackend.resize))
        print(source_inspect.getsource(TorchvisionBackend.rescale_and_normalize))
    elif args.export_positions:
        export_positions(args.model_dir,args.image_output)
    elif args.export_images:
        export_images(args.model_dir,args.image_output)
    elif args.inspect:
        inspect(args.model_dir)
    elif args.export_tokenizer:
        export(args.model_dir, args.output)
    elif args.wrapper_check:
        wrapper_check(args.model_dir, args.output)
    else:
        parser.error("select --inspect, --export-tokenizer, --wrapper-check, --inspect-images, --export-images or --export-positions")


if __name__ == "__main__":
    main()