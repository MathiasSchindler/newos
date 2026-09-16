import argparse
import collections
import contextlib
import hashlib
import importlib.metadata
import json
import math
import os
import random
import re
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


def export_htp(model, output):
    catalog = verified_sources(model)
    torch, _, _, versions = image_reference()
    if torch.version.git_version != "08187d9e0fba026dc8217405802ab5381dc88d90":
        raise ValueError("PyTorch source commit drift")
    records, descriptions = [], []

    def add(operation, name, inputs, weights, expected, output_width):
        source = inputs.half().contiguous().numpy().astype("<f2").tobytes()
        constants = weights.half().contiguous().numpy().astype("<f2").tobytes()
        reference = expected.float().contiguous().numpy().astype("<f4").tobytes()
        rows, width = inputs.shape
        records.append(struct.pack("<7I",operation,rows,width,output_width,len(source),len(constants),len(reference)) + source + constants + reference)
        descriptions.append({"name":name,"operation":operation,"rows":rows,"width":width,"output_width":output_width})

    for rows,width,output_width,name in [(4,1176,1024,"patch_projection"),(4,1024,3072,"vision_qkv"),(1,1536,2048,"text_query")]:
        inputs = (((torch.arange(rows*width).reshape(rows,width)*7)%29)-14).float()/32
        weights = (((torch.arange(width*output_width).reshape(width,output_width)*11)%23)-11).float()/256
        add(1,name,inputs,weights,inputs.half().float() @ weights.half().float(),output_width)
    for width in (64,128,1024,1536):
        inputs = (((torch.arange(4*width).reshape(4,width)*7)%29)-14).float()/8
        inputs[0] = 0
        inputs[1] *= 0.001
        inputs[3] *= 64
        inputs = inputs.half().float()
        gamma = (torch.arange(width)%7).float()/8 + 0.5
        expected = inputs*torch.rsqrt(inputs.square().mean(-1,keepdim=True)+1e-5)*gamma
        add(2,"rmsnorm_"+str(width),inputs,gamma,expected,width)
    width = 1536
    inputs = (((torch.arange(4*width).reshape(4,width)*7)%29)-14).float()/8
    inputs[0] = 1
    inputs[1] *= 0.001
    inputs[3] *= 64
    inputs = inputs.half().float()
    gamma = (torch.arange(width)%7).float()/8 + 0.5
    beta = (torch.arange(width)%5).float()/16 - 0.125
    add(3,"connector_layernorm",inputs,torch.cat((gamma,beta)),torch.nn.functional.layer_norm(inputs,(width,),gamma,beta,1e-5),width)
    for operation,name,width in [(4,"gated_mlp_silu",4608),(5,"connector_gelu",1536),(6,"attention_softmax",64)]:
        rows = 16 if operation == 6 else 2
        inputs = (((torch.arange(rows*width).reshape(rows,width)*13)%257)-128).float()/16
        expected = torch.nn.functional.silu(inputs) if operation == 4 else torch.nn.functional.gelu(inputs,approximate="none") if operation == 5 else torch.softmax(inputs,dim=-1)
        add(operation,name,inputs,torch.empty(0),expected,width)
    data = envelope(struct.pack("<I",len(records))+b"".join(records),5,catalog)
    output.mkdir(parents=True,exist_ok=True)
    target = output/"htp-fixtures.got"
    if target.exists() and target.read_bytes() != data:
        raise ValueError("Existing HTP fixtures differ; choose a new output directory")
    if not target.exists():
        temporary = target.with_suffix(".partial")
        temporary.write_bytes(data)
        os.replace(temporary,target)
    report = {"source_revision":catalog["revision"],"versions":versions,"torch_git_commit":torch.version.git_version,
              "size":len(data),"sha256":sha(data),"cases":descriptions,"reference":"FP32 operation on FP16-rounded synthetic inputs and weights",
              "original_weight_precision_validated":False,"full_model_inference":False}
    (output/"manifest.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print("PASS HTP oracle export:",len(records),"cases",len(data),"bytes",sha(data))


def export_learned_htp(model, output):
    import inspect as source_inspect
    import numpy as np
    torch, processor_class, _, versions = image_reference()
    from PIL import Image, ImageDraw, ImageFont
    from transformers.models.glm_ocr.modeling_glm_ocr import GlmOcrVisionPatchEmbed
    from transformers.models.glm_ocr.configuration_glm_ocr import GlmOcrVisionConfig
    module_hash = sha(Path(source_inspect.getfile(GlmOcrVisionPatchEmbed)).read_bytes())
    if module_hash != "aea6387985dad1f0f5124f9344cc98849be8a7f2c26652ac3d914f6eefac6cc6" or torch.version.git_version != "08187d9e0fba026dc8217405802ab5381dc88d90":
        raise ValueError("Learned reference source drift")
    state = {}
    with weight_source(model) as (stream,tensors,payload,catalog):
        for suffix in ("weight","bias"):
            name = "model.visual.patch_embed.proj."+suffix
            tensor = tensors[name]
            start,end = tensor["data_offsets"]
            stream.seek(payload+start)
            raw = stream.read(end-start)
            if len(raw) != end-start:
                raise ValueError("Truncated patch tensor")
            values = (np.frombuffer(raw,dtype="<u2").astype(np.uint32) << 16).view(np.float32).reshape(tensor["shape"])
            state["proj."+suffix] = torch.from_numpy(values.copy())
    original_sha = next(item["sha256"] for item in catalog["files"] if item["name"] == "model.safetensors")
    configuration = GlmOcrVisionConfig(**read_json(model/"config.json")["vision_config"])
    original = GlmOcrVisionPatchEmbed(configuration).eval()
    original.load_state_dict(state,strict=True)
    candidate = GlmOcrVisionPatchEmbed(configuration).eval()
    candidate.load_state_dict({name:value.half().float() for name,value in state.items()},strict=True)
    processor = processor_class.from_pretrained(str(model),local_files_only=True)
    row_ids,column_ids = np.indices((112,112))
    pattern = np.stack(((row_ids*17+column_ids*7)%256,(row_ids*3+column_ids*23)%256,(row_ids*31+column_ids*5)%256),axis=-1).astype(np.uint8)
    noise = np.random.default_rng(20260916).integers(0,256,size=(112,112,3),dtype=np.uint8)
    font_path = Path(os.environ["WINDIR"])/"Fonts/segoeui.ttf"
    page = Image.new("RGB",(224,168),"white")
    ImageDraw.Draw(page).multiline_text((8,8),"Rechnung 1042\nBetrag: 19,95 EUR\nDatum: 16.09.2026\nHello, OCR!",font=ImageFont.truetype(str(font_path),18),fill="black",spacing=8)
    weights = candidate.proj.weight.detach().reshape(1024,1176).t().contiguous()
    constants = weights.half().numpy().astype("<f2").tobytes()+candidate.proj.bias.detach().half().numpy().astype("<f2").tobytes()
    records,descriptions = [],[]
    with torch.inference_mode():
        for name,image in (("pattern",pattern),("noise",noise),("text",np.asarray(page))):
            pixels = torch.from_numpy(image.copy()).permute(2,0,1)
            all_patches = processor(images=pixels,return_tensors="pt",input_data_format="channels_first")["pixel_values"]
            indices = torch.linspace(0,all_patches.shape[0]-1,16).long()
            patches = all_patches[indices].contiguous().float()
            rounded = patches.half().float()
            source_result = original(patches)
            candidate_result = candidate(rounded)
            flattened_result = rounded @ weights + candidate.proj.bias
            torch.testing.assert_close(flattened_result,candidate_result,atol=1e-4,rtol=1e-5)
            source = rounded.half().numpy().astype("<f2").tobytes()
            for scope,expected in (("original_fp32",source_result),("candidate_fp32",candidate_result)):
                if not torch.isfinite(expected).all() or not torch.isfinite(rounded).all() or not torch.isfinite(weights).all():
                    raise ValueError("Nonfinite learned reference")
                reference = expected.contiguous().numpy().astype("<f4").tobytes()
                records.append(struct.pack("<7I",7,16,1176,1024,len(source),len(constants),len(reference))+source+constants+reference)
                descriptions.append({"name":name+"_"+scope,"patch_indices":indices.tolist(),"rgb_sha256":sha(image.tobytes()),
                                     "input_min":float(patches.min()),"input_max":float(patches.max()),
                                     "reference_max_magnitude":float(expected.abs().max()),
                                     "conversion_max_absolute_error":float((source_result-candidate_result).abs().max())})
    data = envelope(bytes.fromhex(original_sha)+struct.pack("<I",len(records))+b"".join(records),6,catalog)
    output.mkdir(parents=True,exist_ok=True)
    target = output/"htp-fixtures.got"
    if target.exists() and target.read_bytes() != data:
        raise ValueError("Existing learned HTP fixtures differ; choose a new output directory")
    if not target.exists():
        temporary = target.with_suffix(".partial")
        temporary.write_bytes(data)
        os.replace(temporary,target)
    report = {"schema_version":1,"source_revision":catalog["revision"],"source_sha256":original_sha,
              "exporter_sha256":sha(Path(__file__).read_bytes()),"modeling_sha256":module_hash,"versions":versions,
              "font_sha256":sha(font_path.read_bytes()),"sha256":sha(data),"size":len(data),"cases":descriptions,
              "reference":"Pinned patch Conv3d with original BF16 values expanded to FP32; separately FP16-rounded inputs/weights in FP32 Conv3d",
              "full_model_inference":False,"checkpoint_written":False}
    (output/"manifest.json").write_text(json.dumps(report,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS learned patch oracle:",len(records),"cases",len(data),"bytes",sha(data),flush=True)


def export_vision_attention(model, output, case="pattern", full_block=False, block_index=0, collect_only=False, observed_input=None):
    import inspect as source_inspect
    import numpy as np
    torch, processor_class, _, versions = image_reference()
    from transformers.models.glm_ocr import modeling_glm_ocr as reference
    from transformers.models.glm_ocr.configuration_glm_ocr import GlmOcrVisionConfig
    module_hash = sha(Path(source_inspect.getfile(reference)).read_bytes())
    if module_hash != "aea6387985dad1f0f5124f9344cc98849be8a7f2c26652ac3d914f6eefac6cc6" or torch.version.git_version != "08187d9e0fba026dc8217405802ab5381dc88d90":
        raise ValueError("Attention reference source drift")
    state = {}
    with weight_source(model) as (stream,tensors,payload,catalog):
        for name,tensor in tensors.items():
            if not (name.startswith("model.visual.patch_embed.") or name.startswith("model.visual.blocks.0.attn.") or name == "model.visual.blocks.0.norm1.weight" or
                    (full_block and (name.startswith("model.visual.blocks.0.") or (block_index == 1 and name.startswith("model.visual.blocks.1."))))):
                continue
            start,end = tensor["data_offsets"]
            stream.seek(payload+start)
            raw = stream.read(end-start)
            if len(raw) != end-start:
                raise ValueError("Truncated attention tensor")
            values = (np.frombuffer(raw,dtype="<u2").astype(np.uint32) << 16).view(np.float32).reshape(tensor["shape"])
            state[name.removeprefix("model.visual.")] = torch.from_numpy(values.copy())
    config = GlmOcrVisionConfig(**read_json(model/"config.json")["vision_config"])
    config._attn_implementation = "eager"
    patch = reference.GlmOcrVisionPatchEmbed(config).eval()
    patch.load_state_dict({name.removeprefix("patch_embed."):value for name,value in state.items() if name.startswith("patch_embed.")},strict=True)
    processor = processor_class.from_pretrained(str(model),local_files_only=True)
    row_ids,column_ids = np.indices((112,112))
    image = np.stack(((row_ids*17+column_ids*7)%256,(row_ids*3+column_ids*23)%256,(row_ids*31+column_ids*5)%256),axis=-1).astype(np.uint8)
    font_sha = None
    expected_text = None
    if case == "noise":
        image = np.random.default_rng(20260916).integers(0,256,size=(112,112,3),dtype=np.uint8)
    elif case in ("text","receipt","german"):
        from PIL import Image, ImageDraw, ImageFont
        font_path = Path(os.environ["WINDIR"])/"Fonts/segoeui.ttf"
        expected_text = {"text":"OCR 1042\n19,95 EUR\n16.09.2026\nHello!",
                         "receipt":"BELEG 1042\n16.09.2026\n2 Hefte: 7,90 EUR\n1 Stift: 2,05 EUR\nSumme: 9,95 EUR",
                         "german":"Gr\u00fc\u00dfe aus K\u00f6ln!\nStra\u00dfe 12, 3. Stock\n\u00d6ffnung: 08:30-17:00\nPr\u00fcfung am 16.09.2026\nHello, world!"}[case]
        page = Image.new("RGB",(112 if case == "text" else 224,112),"white")
        draw = ImageDraw.Draw(page)
        font = ImageFont.truetype(str(font_path),14)
        bounds = draw.multiline_textbbox((3,3),expected_text,font=font,spacing=4)
        if bounds[0] < 0 or bounds[1] < 0 or bounds[2] > page.width-3 or bounds[3] > page.height-3:
            raise ValueError("OCR example text exceeds its image")
        draw.multiline_text((3,3),expected_text,font=font,fill="black",spacing=4)
        image = np.asarray(page).copy()
        font_sha = sha(font_path.read_bytes())
    elif case != "pattern":
        raise ValueError("Unknown attention image case")
    processed = processor(images=torch.from_numpy(image).permute(2,0,1),return_tensors="pt",input_data_format="channels_first")
    grid = [1,8,16 if case in ("receipt","german") else 8]
    tokens = grid[1]*grid[2]
    if processed["image_grid_thw"].tolist() != [grid]:
        raise ValueError("Unexpected attention grid")
    positions = reference.get_vision_position_ids(processed["image_grid_thw"],2)
    with torch.inference_mode():
        hidden = patch(processed["pixel_values"].float())
        cos,sin = reference.GlmOcrVisionRotaryEmbedding(config)(hidden,positions)
    preceding = {name.removeprefix("blocks.0."):value for name,value in state.items() if name.startswith("blocks.0.")}
    if block_index == 1:
        state = {name.replace("blocks.1.","blocks.0.",1):value for name,value in state.items() if name.startswith("blocks.1.")}
    scopes,tap_names,statistics = [],None,{}
    for scope in ("original_fp32","candidate_fp32"):
        attention = reference.GlmOcrVisionAttention(config).eval()
        norm = reference.GlmOcrRMSNorm(1024,eps=config.rms_norm_eps).eval()
        weights = {name.removeprefix("blocks.0.attn."):value for name,value in state.items() if name.startswith("blocks.0.attn.")}
        norm_weight = state["blocks.0.norm1.weight"]
        inputs = hidden
        if scope == "candidate_fp32":
            weights = {name:value.half().float() for name,value in weights.items()}
            norm_weight = norm_weight.half().float()
            inputs = hidden.half().float()
        if observed_input is not None:
            if block_index != 1 or not collect_only or observed_input.shape != (tokens,1024) or not np.isfinite(observed_input).all():
                raise ValueError("Invalid conditional oracle input")
            inputs = torch.from_numpy(observed_input.copy()).float()
        elif block_index == 1:
            previous = reference.GlmOcrVisionBlock(config).eval()
            previous.load_state_dict({name:value.half().float() if scope == "candidate_fp32" else value for name,value in preceding.items()},strict=True)
            with torch.inference_mode():
                inputs = previous(inputs,torch.tensor([0,tokens],dtype=torch.int32),position_embeddings=(cos,sin))
        attention.load_state_dict(weights,strict=True)
        norm.load_state_dict({"weight":norm_weight},strict=True)
        block = None
        if full_block:
            block = reference.GlmOcrVisionBlock(config).eval()
            block_weights = {name.removeprefix("blocks.0."):value for name,value in state.items() if name.startswith("blocks.0.")}
            if scope == "candidate_fp32":
                block_weights = {name:value.half().float() for name,value in block_weights.items()}
            block.load_state_dict(block_weights,strict=True)
            attention,norm = block.attn,block.norm1
        captured = {}
        def capture(name):
            def hook(module, arguments, result):
                captured[name] = result.detach().clone()
            return hook
        hooks = [module.register_forward_hook(capture(name)) for name,module in (("qkv",attention.qkv),("q_norm",attention.q_norm),("k_norm",attention.k_norm))]
        def capture_context(module, arguments):
            captured["context"] = arguments[0].detach().clone()
        hooks.append(attention.proj.register_forward_pre_hook(capture_context))
        if full_block:
            hooks += [module.register_forward_hook(capture(name)) for name,module in
                      (("norm1",norm),("projection",attention.proj),("norm2",block.norm2),
                       ("gate",block.mlp.gate_proj),("up",block.mlp.up_proj),("down",block.mlp.down_proj))]
            def capture_gated(module, arguments):
                captured["gated"] = arguments[0].detach().clone()
            hooks.append(block.mlp.down_proj.register_forward_pre_hook(capture_gated))
        try:
            with torch.inference_mode():
                if full_block:
                    block_output = block(inputs,torch.tensor([0,tokens],dtype=torch.int32),position_embeddings=(cos,sin))
                    normalized,projected = captured["norm1"],captured["projection"]
                else:
                    normalized = norm(inputs)
                    projected = attention(normalized,torch.tensor([0,tokens],dtype=torch.int32),position_embeddings=(cos,sin))
                query,key = reference.apply_rotary_pos_emb_vision(captured["q_norm"],captured["k_norm"],cos,sin)
                value = captured["qkv"].reshape(tokens,3,16,64)[:,2].transpose(0,1)
                scores = (query.transpose(0,1) @ key.transpose(0,1).transpose(-1,-2))*0.125
                probabilities = torch.softmax(scores,dim=-1)
                context = (probabilities @ value).transpose(0,1).reshape(tokens,1024)
                torch.testing.assert_close(context,captured["context"],atol=1e-6,rtol=1e-5)
                taps = {"norm1":normalized,"qkv":captured["qkv"],"q_norm":captured["q_norm"],"k_norm":captured["k_norm"],
                        "q_rope":query,"k_rope":key,"scores":scores,"probabilities":probabilities,"context":context,
                        "projection":projected,"residual":inputs+projected}
                if full_block:
                    activated = block.mlp.act_fn(captured["gate"])
                    torch.testing.assert_close(activated*captured["up"],captured["gated"],atol=0,rtol=0)
                    torch.testing.assert_close(taps["residual"]+captured["down"],block_output,atol=0,rtol=0)
                    taps.update(norm2=captured["norm2"],gate=captured["gate"],up=captured["up"],silu=activated,
                                gated=captured["gated"],down=captured["down"],block_output=block_output)
                if any(not torch.isfinite(value).all() for value in taps.values()):
                    raise ValueError("Nonfinite attention oracle")
                tap_names = list(taps)
                statistics[scope] = {name:{"shape":list(value.shape),"min":float(value.min()),"max":float(value.max()),"max_magnitude":float(value.abs().max())} for name,value in taps.items()}
                scopes.append(b"".join(value.contiguous().numpy().astype("<f4").tobytes() for value in taps.values()))
        finally:
            for hook in hooks:
                hook.remove()
    def half_bytes(value):
        return value.detach().half().contiguous().numpy().astype("<f2").tobytes()
    constants = half_bytes(state["blocks.0.norm1.weight"])
    for name in ("qkv.weight","qkv.bias","q_norm.weight","k_norm.weight"):
        value = state["blocks.0.attn."+name]
        constants += half_bytes(value.t() if name.endswith(".weight") and value.ndim == 2 else value)
    constants += cos.contiguous().numpy().astype("<f4").tobytes()+sin.contiguous().numpy().astype("<f4").tobytes()
    constants += half_bytes(state["blocks.0.attn.proj.weight"].t())+half_bytes(state["blocks.0.attn.proj.bias"])
    if full_block:
        constants += half_bytes(state["blocks.0.norm2.weight"])
        for projection in ("gate_proj","up_proj","down_proj"):
            constants += half_bytes(state["blocks.0.mlp."+projection+".weight"].t())+half_bytes(state["blocks.0.mlp."+projection+".bias"])
    source = half_bytes(hidden) if block_index == 0 else b""
    references = b"".join(scopes)
    original_sha = next(item["sha256"] for item in catalog["files"] if item["name"] == "model.safetensors")
    record = struct.pack("<7I",9 if full_block else 8,tokens,1024,1024,len(source),len(constants),len(references))+source+constants+references
    data = envelope(bytes.fromhex(original_sha)+struct.pack("<I",1)+record,8 if full_block else 7,catalog)
    if collect_only:
        return record,{"block_index":block_index,"grid_thw":grid,"tap_order":tap_names,"taps":statistics,
                       "input_bytes":len(source),"constant_bytes":len(constants),"reference_bytes":len(references),
                       "source_revision":catalog["revision"],"source_sha256":original_sha,"modeling_sha256":module_hash,
                       "versions":versions,"rgb_sha256":sha(image.tobytes()),"font_sha256":font_sha},catalog
    output.mkdir(parents=True,exist_ok=True)
    target = output/"htp-fixtures.got"
    if target.exists() and target.read_bytes() != data:
        raise ValueError("Existing attention fixtures differ; choose a new output directory")
    if not target.exists():
        temporary = target.with_suffix(".partial")
        temporary.write_bytes(data)
        os.replace(temporary,target)
    from PIL import Image
    import io
    preview = io.BytesIO()
    Image.fromarray(image).save(preview,format="PNG")
    previews = {"input.png":preview.getvalue()}
    if expected_text is not None:
        previews["expected.txt"] = (expected_text+"\n").encode("utf-8")
    for filename,content in previews.items():
        destination = output/filename
        if destination.exists() and destination.read_bytes() != content:
            raise ValueError("Existing OCR example differs: "+str(destination))
        if not destination.exists():
            destination.write_bytes(content)
    if np.asarray(Image.open(output/"input.png").convert("RGB")).tobytes() != image.tobytes():
        raise ValueError("OCR preview pixel mismatch")
    report = {"schema_version":1,"source_revision":catalog["revision"],"source_sha256":original_sha,"modeling_sha256":module_hash,
              "exporter_sha256":sha(Path(__file__).read_bytes()),"versions":versions,"sha256":sha(data),"size":len(data),
              "input_bytes":len(source),"constant_bytes":len(constants),"reference_bytes":len(references),"tap_order":tap_names,"taps":statistics,
              "rgb_sha256":sha(image.tobytes()),"case":case,"font_sha256":font_sha,"positions":positions.tolist(),"full_model_inference":False,
              "grid_thw":grid,"examples":{filename:sha(content) for filename,content in previews.items()},
              "expected_text_provenance":"Synthetic rendering input, not model output" if expected_text is not None else None,
              "scope":f"Complete vision block 0 on one {grid[1]}x{grid[2]} grid" if full_block else f"Vision block 0 attention branch and first residual on one {grid[1]}x{grid[2]} grid; no MLP"}
    (output/"manifest.json").write_text(json.dumps(report,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS vision block oracle:" if full_block else "PASS vision attention oracle:",len(tap_names),"taps, two references,",len(data),"bytes",sha(data),flush=True)


def export_vision_encoder(model, output, large_images=False):
    import inspect as source_inspect
    import numpy as np
    torch, processor_class, _, versions = image_reference()
    from PIL import Image
    from transformers.models.glm_ocr import modeling_glm_ocr as reference
    from transformers.models.glm_ocr.configuration_glm_ocr import GlmOcrVisionConfig
    module_hash = sha(Path(source_inspect.getfile(reference)).read_bytes())
    if module_hash != "aea6387985dad1f0f5124f9344cc98849be8a7f2c26652ac3d914f6eefac6cc6" or torch.version.git_version != "08187d9e0fba026dc8217405802ab5381dc88d90":
        raise ValueError("Vision reference source drift")
    state = {}
    with weight_source(model) as (stream,tensors,payload,catalog):
        for name,tensor in tensors.items():
            if not name.startswith("model.visual."):
                continue
            start,end = tensor["data_offsets"]
            stream.seek(payload+start)
            raw = stream.read(end-start)
            if len(raw) != end-start:
                raise ValueError("Truncated vision weight")
            values = (np.frombuffer(raw,dtype="<u2").astype(np.uint32) << 16).view(np.float32).reshape(tensor["shape"])
            if not np.isfinite(values).all() or not np.isfinite(values.astype(np.float16)).all():
                raise ValueError("Nonfinite/overflowing vision weight")
            state[name.removeprefix("model.visual.")] = torch.from_numpy(values.copy())
    config = GlmOcrVisionConfig(**read_json(model/"config.json")["vision_config"])
    config._attn_implementation = "eager"
    if (config.depth,config.hidden_size,config.out_hidden_size,config.spatial_merge_size,config.rms_norm_eps) != (24,1024,1536,2,1e-5):
        raise ValueError("Vision geometry drift")
    vision = reference.GlmOcrVisionModel(config).eval()
    vision.load_state_dict(state,strict=True)
    output.mkdir(parents=True,exist_ok=True)
    files = {}
    def store(name,data):
        path = output/name
        if path.exists() and sha(path.read_bytes()) != sha(data):
            raise ValueError("Existing vision artifact differs: "+str(path))
        if not path.exists():
            with path.open("xb") as target:
                target.write(data)
        files[name] = {"bytes":len(data),"sha256":sha(data)}
    def half(value):
        return value.detach().half().contiguous().numpy().astype("<f2").tobytes()
    original_sha = next(item["sha256"] for item in catalog["files"] if item["name"] == "model.safetensors")
    identity = bytes.fromhex(original_sha)
    for block_index in range(24):
        prefix = f"blocks.{block_index}."
        constants = half(state[prefix+"norm1.weight"])
        for name in ("attn.qkv.weight","attn.qkv.bias","attn.q_norm.weight","attn.k_norm.weight",
                     "attn.proj.weight","attn.proj.bias","norm2.weight",
                     "mlp.gate_proj.weight","mlp.gate_proj.bias","mlp.up_proj.weight","mlp.up_proj.bias","mlp.down_proj.weight","mlp.down_proj.bias"):
            value = state[prefix+name]
            constants += half(value.t() if value.ndim == 2 else value)
        if len(constants) != 33585408:
            raise ValueError("Vision block layout drift")
        store(f"block-{block_index:02}.got",envelope(identity+struct.pack("<I",block_index)+constants,10,catalog))
    shared = identity+half(state["patch_embed.proj.weight"].reshape(1024,1176).t())+half(state["patch_embed.proj.bias"])
    for grid in ([1,8,8],[1,8,16]):
        positions = reference.get_vision_position_ids(torch.tensor([grid]),2)
        with torch.inference_mode():
            cosine,sine = vision.rotary_pos_emb(torch.zeros(grid[1]*grid[2],1024),positions)
        shared += cosine.numpy().astype("<f4").tobytes()+sine.numpy().astype("<f4").tobytes()
    store("shared.got",envelope(shared,11,catalog))
    if large_images:
        rotary = identity
        for grid in ([1,16,16],[1,16,32]):
            positions = reference.get_vision_position_ids(torch.tensor([grid]),2)
            with torch.inference_mode():
                cosine,sine = vision.rotary_pos_emb(torch.zeros(grid[1]*grid[2],1024),positions)
            rotary += cosine.numpy().astype("<f4").tobytes()+sine.numpy().astype("<f4").tobytes()
        store("large-rope.got",envelope(rotary,19,catalog))
    tail = identity+half(state["post_layernorm.weight"])
    tail += half(state["downsample.weight"].permute(2,3,1,0).reshape(4096,1536))+half(state["downsample.bias"])
    tail += half(state["merger.proj.weight"].t())
    tail += half(state["merger.post_projection_norm.weight"])+half(state["merger.post_projection_norm.bias"])
    for projection in ("gate_proj","up_proj","down_proj"):
        tail += half(state["merger."+projection+".weight"].t())
    if len(tail) != 59780096+32:
        raise ValueError("Vision tail layout drift: "+str(len(tail)))
    store("tail.got",envelope(tail,12,catalog))
    processor = processor_class.from_pretrained(str(model),local_files_only=True)
    cases = []
    case_records = {}
    for case,columns in (("pattern",112),("receipt",224)):
        if large_images:
            from PIL import ImageDraw, ImageFont
            columns *= 2
            image = Image.new("RGB",(columns,224),"white")
            font_path = Path("C:/Windows/Fonts/segoeui.ttf")
            font = ImageFont.truetype(str(font_path),26)
            expected = (ROOT/"experimental/snapdragon/models/glm-ocr-examples-v1/receipt/expected.txt").read_text(encoding="utf-8") if case == "receipt" else "OCR 1042\n19,95 EUR\n16.09.2026\nHello!\n"
            draw = ImageDraw.Draw(image)
            for index,line in enumerate(expected.splitlines()):
                bounds = draw.textbbox((12,12+index*36),line,font=font)
                if bounds[2] > columns-8 or bounds[3] > 216: raise ValueError("Large image text would be clipped")
                draw.text((12,12+index*36),line,font=font,fill="black")
            store(case+".expected.txt",expected.encode("utf-8"))
        elif case == "receipt":
            image = Image.open(ROOT/"experimental/snapdragon/models/glm-ocr-examples-v1/receipt/input.png").convert("RGB")
        else:
            rows,cols = np.indices((112,columns))
            image = Image.fromarray(np.stack(((rows*17+cols*7)%256,(rows*3+cols*23)%256,(rows*31+cols*5)%256),axis=-1).astype(np.uint8))
        import io
        bitmap = io.BytesIO(); image.save(bitmap,format="BMP")
        store(case+".bmp",bitmap.getvalue())
        if large_images:
            png = io.BytesIO(); image.save(png,format="PNG")
            store(case+".png",png.getvalue())
        processed = processor(images=image,return_tensors="pt")
        grid = processed["image_grid_thw"].tolist()
        if grid != [[1,16 if large_images else 8,columns//14]]:
            raise ValueError("Vision image bucket mismatch")
        case_records[case] = {"grid":grid[0],"input":case+(".png" if large_images else ".bmp"),"rgb_sha256":sha(image.tobytes())}
        if large_images: case_records[case].update({"font_sha256":sha(font_path.read_bytes()),"expected":case+".expected.txt"})
        cases.append((case,processed))
        store(case+".patches.f32",processed["pixel_values"].numpy().astype("<f4").tobytes())
    for scope in ("original","candidate"):
        if scope == "candidate":
            vision.half().float()
        for case,processed in cases:
            captured = []
            def hook(module,arguments,result):
                captured.append(result.detach().clone())
            handles = [vision.patch_embed.register_forward_hook(hook)]
            handles += [block.register_forward_hook(hook) for block in vision.blocks]
            handles += [vision.post_layernorm.register_forward_hook(hook),vision.downsample.register_forward_hook(hook)]
            try:
                inputs = processed["pixel_values"].float()
                if scope == "candidate":
                    inputs = inputs.half().float()
                with torch.inference_mode():
                    result = vision(inputs,processed["image_grid_thw"])
                captured.append(result.pooler_output)
                if len(captured) != 28 or any(not torch.isfinite(value).all() for value in captured):
                    raise ValueError("Vision reference capture mismatch")
                store(case+"."+scope+".f32",b"".join(value.contiguous().numpy().astype("<f4").tobytes() for value in captured))
            finally:
                for handle in handles:
                    handle.remove()
            print("PASS full vision oracle",scope,case,flush=True)
    manifest = {"schema_version":1,"source_sha256":original_sha,"modeling_sha256":module_hash,"versions":versions,
                "blocks":24,"buckets":[[8,8],[8,16]]+([[16,16],[16,32]] if large_images else []),"output_width":1536,"files":files,
                "reference_stages":["patch"]+[f"block-{index}" for index in range(24)]+["postnorm","downsample","merger"],
                "full_text_inference":False}
    if large_images: manifest["cases"] = case_records
    store("manifest.json",(json.dumps(manifest,indent=2)+"\n").encode())
    print("PASS vision encoder export",len(files),"files",flush=True)


def analyze_vision_encoder(output, build):
    import numpy as np
    manifest = read_json(output/"manifest.json")
    if manifest["blocks"] != 24 or manifest["buckets"] not in ([[8,8],[8,16]],[[8,8],[8,16],[16,16],[16,32]]) or len(manifest["reference_stages"]) != 28:
        raise ValueError("Vision manifest geometry mismatch")
    for name,record in manifest["files"].items():
        path = output/name
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream,"sha256").hexdigest()
        if path.stat().st_size != record["bytes"] or digest != record["sha256"]:
            raise ValueError("Vision artifact changed: "+name)
    reports = {}
    def stats(actual,reference):
        error = actual.astype(np.float64)-reference.astype(np.float64)
        limit = 0.003+0.005*np.abs(reference.astype(np.float64))
        return {"elements":int(actual.size),"failures":int((np.abs(error)>limit).sum()),
                "rmse":float(np.sqrt(np.mean(error*error))),"max_absolute_error":float(np.abs(error).max())}
    for case,tokens in (("pattern",64),("receipt",128)):
        record = manifest.get("cases",{}).get(case,{"input":case+".bmp"})
        if "grid" in record: tokens = math.prod(record["grid"])
        run_path = build/(case+"-vision-run.json")
        if not run_path.exists(): run_path = build/(case+"-generate-run.json")
        run = json.loads(run_path.read_text(encoding="utf-8-sig"))
        capture = Path(run["capture"])
        binary = "ocr-generate.exe" if run.get("scope") == "generate" else "ocr-vision.exe"
        if run["case"] != case or run["exit_code"] not in (0,3) or sha((build/binary).read_bytes()).upper() != run["executable_sha256"]:
            raise ValueError("Vision run identity mismatch")
        if "weight_hashes" in run:
            expected_weights = {name:record["sha256"].upper() for name,record in manifest["files"].items() if name.endswith(".got")}
            if run["weight_hashes"] != expected_weights or run["input_sha256"] != manifest["files"][record["input"]]["sha256"].upper():
                raise ValueError("Vision input/weight identity changed")
        captured_hashes = {}
        def captured(name,elements,dtype="<f2"):
            raw = (capture/name).read_bytes()
            values = np.frombuffer(raw,dtype=dtype)
            if values.size != elements or not np.isfinite(values).all():
                raise ValueError("Invalid captured tensor: "+name)
            captured_hashes[name] = sha(raw)
            if "captures" in run and captured_hashes[name].upper() != run["captures"][name]:
                raise ValueError("Capture changed: "+name)
            return values
        patches = captured("0.patches.f32",tokens*1176,"<f4")
        if patches.tobytes() != (output/(case+".patches.f32")).read_bytes():
            raise ValueError("Native image preprocessing differs from actual processor")
        stages = [captured("0.patch.f16",tokens*1024)]
        for index in range(24):
            source = captured(f"{index}.input.f16",tokens*1024)
            if source.tobytes() != stages[-1].tobytes():
                raise ValueError("Vision block handoff changed")
            taps = captured(f"{index}.taps.f16",30720*tokens+32*tokens*tokens)
            stages.append(taps[-tokens*1024:])
        stages += [captured("24.postnorm.f16",tokens*1024),captured("25.downsample.f16",tokens//4*1536),captured("26.features.f16",tokens//4*1536)]
        scopes = {}
        for scope in ("original","candidate"):
            reference = np.frombuffer((output/(case+"."+scope+".f32")).read_bytes(),dtype="<f4")
            if reference.size != sum(value.size for value in stages) or not np.isfinite(reference).all():
                raise ValueError("Invalid full vision reference")
            cursor = 0; result = {}
            for name,actual in zip(manifest["reference_stages"],stages,strict=True):
                result[name] = stats(actual,reference[cursor:cursor+actual.size]); cursor += actual.size
            scopes[scope] = result
            print(case,scope,"stage failures",[(name,data["failures"]) for name,data in result.items()],flush=True)
        reports[case] = {"run":run,"captured_sha256":captured_hashes,"block_handoffs_bit_exact":True,
                         "preprocessing_bit_exact":True,"finite_all_taps":True,"stages":scopes,
                         "numerical_gate_pass":all(not data["failures"] for scope in scopes.values() for data in scope.values())}
    report = {"schema_version":1,"analyzer_sha256":sha(Path(__file__).read_bytes()),"manifest_sha256":sha((output/"manifest.json").read_bytes()),
              "tolerance":{"absolute":0.003,"relative":0.005},"cases":reports,"full_text_inference":False,
              "numerical_gate_pass":all(value["numerical_gate_pass"] for value in reports.values())}
    (build/"vision-analysis.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print("PASS vision structure/identity/handoff analysis; numerical gate:",report["numerical_gate_pass"],flush=True)


def test_png(build):
    import subprocess
    import zlib
    import numpy as np
    image_reference()
    from PIL import Image
    binary = build/"ocr-image.exe"
    def chunk(kind,payload):
        return struct.pack(">I",len(payload))+kind+payload+struct.pack(">I",zlib.crc32(kind+payload))
    signature = b"\x89PNG\r\n\x1a\n"
    checks = 0
    with tempfile.TemporaryDirectory(prefix="ocr-png-",dir=ROOT/"tests/tmp") as scratch:
        scratch = Path(scratch)
        def run(data,valid,expected=None):
            nonlocal checks
            source = scratch/f"input-{checks}.png"; destination = scratch/f"output-{checks}.f32"
            source.write_bytes(data)
            result = subprocess.run([str(binary),"--prepare-image",str(source),str(destination)],capture_output=True)
            if (result.returncode == 0) != valid: raise ValueError("PNG acceptance failure: "+str(checks)+repr(result.stderr))
            if valid and destination.read_bytes() != expected: raise ValueError("PNG/BMP patch mismatch")
            if not valid and destination.exists(): raise ValueError("Rejected PNG created output")
            checks += 1
        for color,channels in ((0,1),(2,3),(4,2),(6,4)):
            rows,columns = 112,224
            values = ((np.arange(rows*columns*channels,dtype=np.uint32)*17+31)%256).astype(np.uint8).reshape(rows,columns,channels)
            rgb = np.repeat(values[:,:,:1],3,axis=2) if color in (0,4) else values[:,:,:3]
            if color in (4,6):
                alpha = values[:,:,-1:].astype(np.uint32)
                rgb = ((rgb.astype(np.uint32)*alpha+255*(255-alpha)+127)//255).astype(np.uint8)
            bitmap = scratch/f"reference-{color}.bmp"; patches = scratch/f"reference-{color}.f32"
            Image.fromarray(rgb).save(bitmap)
            subprocess.run([str(binary),"--prepare-image",str(bitmap),str(patches)],check=True,capture_output=True)
            expected = patches.read_bytes()
            header = chunk(b"IHDR",struct.pack(">IIBBBBB",columns,rows,8,color,0,0,0))
            for mode in range(5):
                encoded = bytearray()
                flat = values.reshape(rows,columns*channels)
                for row in range(rows):
                    encoded.append(mode)
                    for column,value in enumerate(flat[row]):
                        left = int(flat[row,column-channels]) if column >= channels else 0
                        above = int(flat[row-1,column]) if row else 0
                        diagonal = int(flat[row-1,column-channels]) if row and column >= channels else 0
                        estimate = left+above-diagonal
                        paeth = min((left,above,diagonal),key=lambda candidate:abs(estimate-candidate))
                        predictor = (0,left,above,(left+above)//2,paeth)[mode]
                        encoded.append((int(value)-predictor)&255)
                compressed = zlib.compress(encoded)
                middle = len(compressed)//2
                data = signature+header+chunk(b"IDAT",compressed[:middle])+chunk(b"IDAT",compressed[middle:])+chunk(b"IEND",b"")
                run(data,True,expected)
                damaged = bytearray(data); damaged[-1] ^= 1; run(damaged,False)
                run(data[:-1],False); run(data+b"x",False)
                invalid_raw = bytearray(encoded); invalid_raw[0] = 5
                run(signature+header+chunk(b"IDAT",zlib.compress(invalid_raw))+chunk(b"IEND",b""),False)
                run(signature+header+chunk(b"IDAT",compressed+b"x")+chunk(b"IEND",b""),False)
            for depth,kind,interlace in ((16,color,0),(8,3,0),(8,color,1)):
                bad_header = chunk(b"IHDR",struct.pack(">IIBBBBB",columns,rows,depth,kind,0,0,interlace))
                run(signature+bad_header+chunk(b"IDAT",compressed)+chunk(b"IEND",b""),False)
            for forbidden in (b"tRNS",b"eXIf",b"acTL",b"iCCP",b"ABCD"):
                run(signature+header+chunk(forbidden,b"x")+chunk(b"IDAT",compressed)+chunk(b"IEND",b""),False)
            run(signature+header+chunk(b"IDAT",compressed[:middle])+chunk(b"tEXt",b"note\0x")+chunk(b"IDAT",compressed[middle:])+chunk(b"IEND",b""),False)
            for invalid in (b"aaab",b"a1Ab"):
                run(signature+header+chunk(invalid,b"")+chunk(b"IDAT",compressed)+chunk(b"IEND",b""),False)
            run(signature+header+header+chunk(b"IDAT",compressed)+chunk(b"IEND",b""),False)
            run(signature+header+chunk(b"PLTE",b"\0\0\0")*2+chunk(b"IDAT",compressed)+chunk(b"IEND",b""),False)
            for width,height in ((0,112),(10001,112),(4097,4097)):
                invalid_header = chunk(b"IHDR",struct.pack(">IIBBBBB",width,height,8,color,0,0,0))
                run(signature+invalid_header+chunk(b"IDAT",compressed)+chunk(b"IEND",b""),False)
            for raw in (encoded[:-1],encoded+b"x"):
                run(signature+header+chunk(b"IDAT",zlib.compress(raw))+chunk(b"IEND",b""),False)
            bad_adler = bytearray(compressed); bad_adler[-1] ^= 1
            run(signature+header+chunk(b"IDAT",bad_adler)+chunk(b"IEND",b""),False)
            for level in (0,1,9):
                run(signature+header+chunk(b"IDAT",zlib.compress(encoded,level))+chunk(b"IEND",b""),True,expected)
    print("PASS PNG native file-to-patch cases:",checks,"all filters, RGB/gray/alpha, split IDAT, CRC/truncation/deflate/unsupported-format rejection",flush=True)


def reference_generation(model, vision_output, build):
    import inspect as source_inspect
    import time
    import numpy as np
    torch, _, _, versions = image_reference()
    from PIL import Image
    from transformers import AutoProcessor
    from transformers.models.glm_ocr import modeling_glm_ocr as reference
    catalog = verified_sources(model)
    module_hash = sha(Path(source_inspect.getfile(reference)).read_bytes())
    if module_hash != "aea6387985dad1f0f5124f9344cc98849be8a7f2c26652ac3d914f6eefac6cc6":
        raise ValueError("Independent model implementation drift")
    processor = AutoProcessor.from_pretrained(str(model),local_files_only=True,trust_remote_code=False)
    started = time.perf_counter()
    full_model = reference.GlmOcrForConditionalGeneration.from_pretrained(str(model),local_files_only=True,
                    dtype=torch.float32,attn_implementation="eager").eval()
    load_seconds = time.perf_counter()-started
    def distance(actual,expected):
        previous = list(range(len(expected)+1))
        for row,actual_value in enumerate(actual,1):
            current = [row]
            for column,expected_value in enumerate(expected,1):
                current.append(min(current[-1]+1,previous[column]+1,previous[column-1]+(actual_value != expected_value)))
            previous = current
        return previous[-1]
    reports = {}
    vision_manifest = read_json(vision_output/"manifest.json")
    for case in ("pattern","receipt"):
        case_record = vision_manifest.get("cases",{}).get(case,{"input":case+".bmp"})
        image_path = vision_output/case_record["input"]
        run_path = build/(case+"-generate-run.json")
        run = json.loads(run_path.read_text(encoding="utf-8-sig")) if run_path.exists() else None
        if run and (run["exit_code"] not in (0,3) or sha(image_path.read_bytes()).upper() != run["input_sha256"]):
            raise ValueError("Native run/image identity failure")
        if run and sha((build/"ocr-generate.exe").read_bytes()).upper() != run["executable_sha256"]: raise ValueError("Native executable changed")
        with Image.open(image_path) as image:
            rendered = processor.apply_chat_template([{"role":"user","content":[{"type":"image"},{"type":"text","text":TASKS[0]}]}],
                                                       tokenize=False,add_generation_prompt=True)
            inputs = processor(text=[rendered],images=[image.convert("RGB")],return_tensors="pt")
        count = inputs["input_ids"].shape[1]
        if run:
            capture = Path(run["capture"])
            rows = run.get("prefill_context",64)
            def captured(name,dtype,shape):
                raw = (capture/name).read_bytes()
                if sha(raw).upper() != run["captures"][name]: raise ValueError("Changed native input: "+name)
                return np.frombuffer(raw,dtype=dtype).reshape(shape).copy()
            ids = inputs["input_ids"]
            modalities = (ids == 59280).long()
            positions,delta = full_model.model.get_rope_index(ids,modalities,inputs["image_grid_thw"])
            expected_ids = np.full(rows,59246,dtype="<u4"); expected_ids[:count] = ids.numpy()[0]
            expected_positions = np.zeros((3,1,rows),dtype="<i4"); expected_positions[:,:,:count] = positions.numpy()
            expected_modalities = np.zeros(rows,dtype="u1"); expected_modalities[:count] = modalities.numpy()[0]
            expected_mask = np.where((np.arange(rows)[None,:] <= np.arange(rows)[:,None]) & (np.arange(rows)[None,:] < count),0,-65504).astype("<f2")
            image_tokens = int(modalities.sum())
            with torch.inference_mode(): expected_embeddings = full_model.model.language_model.embed_tokens(ids).half().numpy()[0]
            expected_embeddings[modalities.numpy()[0].astype(bool)] = captured("26.features.f16","<f2",(image_tokens,1536))
            padded_embeddings = np.zeros((rows,1536),dtype="<f2"); padded_embeddings[:count] = expected_embeddings
            for name,dtype,expected in (("ids","<u4",expected_ids),("positions","<i4",expected_positions),
                                        ("modalities","u1",expected_modalities),("mask","<f2",expected_mask),("embeddings","<f2",padded_embeddings)):
                extension = {"<u4":"u32","<i4":"i32","u1":"u8","<f2":"f16"}[dtype]
                if captured("0.text-"+name+"."+extension,dtype,expected.shape).tobytes() != expected.tobytes():
                    raise ValueError("Official multimodal input mismatch: "+name)
            layout = captured("0.text-layout.u32","<u4",(7,)).view("<i4")
            if layout.tolist() != [count,image_tokens,0,0,*inputs["image_grid_thw"][0,1:].tolist(),int(delta.item())]:
                raise ValueError("Official multimodal layout mismatch")
            with torch.inference_mode():
                cosine,sine = full_model.model.language_model.rotary_emb(torch.zeros(1,rows,1536),torch.from_numpy(expected_positions.astype(np.int64)))
            for name,expected in (("cosine",cosine),("sine",sine)):
                if captured("0.text-"+name+".f32","<f4",(rows,128)).tobytes() != expected.numpy().astype("<f4").tobytes():
                    raise ValueError("Official prefill RoPE mismatch")
            print("PASS",case,"official image prompt, features, positions, masks and RoPE",flush=True)
        limit = min(256-count,run["max_new_tokens"] if run else 256)
        started = time.perf_counter()
        with torch.inference_mode():
            generated = full_model.generate(**inputs,max_new_tokens=limit,do_sample=False,use_cache=True,return_dict_in_generate=True,output_scores=True)
        elapsed = time.perf_counter()-started
        ids = generated.sequences[0,count:].tolist()
        text = processor.tokenizer.decode(ids,skip_special_tokens=True,clean_up_tokenization_spaces=False)
        native = None
        if run:
            native_path = Path(run["capture"])/"0.generated.txt"
            raw = native_path.read_bytes()
            if sha(raw).upper() != run["captures"]["0.generated.txt"]: raise ValueError("Native output changed")
            native = raw.decode("utf-8")
        margins = []
        for scores in generated.scores:
            values,indices = scores.float().topk(2,dim=-1)
            if not torch.isfinite(scores).all(): raise ValueError("Nonfinite independent logits")
            margins.append({"first":int(indices[0,0]),"second":int(indices[0,1]),"margin":float(values[0,0]-values[0,1])})
        result = {"input_sha256":sha(image_path.read_bytes()),"prompt_tokens":count,"max_new_tokens":limit,"ids":ids,"text":text,
                  "eos":bool(ids and ids[-1] in (59246,59253)),"seconds":elapsed,"top2":margins,"native_text":native,
                  "native_exit_code":run["exit_code"] if run else None,"native_vs_reference_edit_distance":distance(native,text) if run else None,
                  "native_matches_reference":native==text if run else None,"native_run":run}
        if run: result["exact_official_multimodal_input"] = True
        if case == "receipt" or "expected" in case_record:
            expected_path = vision_output/case_record["expected"] if "expected" in case_record else ROOT/"experimental/snapdragon/models/glm-ocr-examples-v1/receipt/expected.txt"
            expected = expected_path.read_text(encoding="utf-8")
            result.update({"expected":expected,"expected_sha256":sha(expected_path.read_bytes()),
                           "native_vs_expected_edit_distance":distance(native,expected) if run else None,"reference_vs_expected_edit_distance":distance(text,expected),
                           "native_cer":distance(native,expected)/max(1,len(expected)) if run else None,"reference_cer":distance(text,expected)/max(1,len(expected))})
        reports[case] = result
        print(case,"independent original EOS",result["eos"],"tokens",len(ids),"native/reference edits",result["native_vs_reference_edit_distance"],
              "text",repr(text),flush=True)
    report = {"schema_version":1,"source_sha256":next(item["sha256"] for item in catalog["files"] if item["name"]=="model.safetensors"),
              "modeling_sha256":module_hash,"analyzer_sha256":sha(Path(__file__).read_bytes()),"versions":versions,"load_seconds":load_seconds,
              "reference":"original BF16 weights expanded to FP32, independent original image processing and autoregressive decisions",
              "cases":reports,"quality_accepted":False}
    (build/"independent-generation.json").write_text(json.dumps(report,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    print("PASS independent full-model image-to-text comparison; no corpus-wide quality acceptance",flush=True)


def analyze_generation(model, output, text_output, build):
    import inspect as source_inspect
    import numpy as np
    torch, _, _, versions = image_reference()
    from transformers import AutoTokenizer
    from transformers.models.glm_ocr import modeling_glm_ocr as reference
    from transformers.models.glm_ocr.configuration_glm_ocr import GlmOcrTextConfig
    module_hash = sha(Path(source_inspect.getfile(reference)).read_bytes())
    text_manifest = read_json(text_output/"manifest.json")
    manifest = read_json(output/"generation-manifest.json")
    if module_hash != text_manifest["modeling_sha256"] or manifest["eos_token_id"] != [59246,59253] or manifest["do_sample"]:
        raise ValueError("Generation reference drift")
    def verify_files(directory,records):
        for name,record in records.items():
            with (directory/name).open("rb") as stream: digest = hashlib.file_digest(stream,"sha256").hexdigest()
            if digest != record["sha256"] or (directory/name).stat().st_size != record["bytes"]:
                raise ValueError("Changed generation reference asset: "+name)
    verify_files(output,manifest["files"]); verify_files(text_output,text_manifest["files"])
    tokenizer = AutoTokenizer.from_pretrained(str(model),local_files_only=True,trust_remote_code=False)
    def metric(actual,target):
        actual = np.asarray(actual,dtype=np.float64); target = np.asarray(target,dtype=np.float64)
        if actual.shape != target.shape or not np.isfinite(actual).all() or not np.isfinite(target).all():
            raise ValueError("Invalid numerical comparison")
        error = actual-target
        return {"elements":int(error.size),"failures":int((np.abs(error)>0.003+0.005*np.abs(target)).sum()),
                "rmse":float(np.sqrt(np.mean(error*error))),"max_absolute_error":float(np.abs(error).max())}
    cases = {}
    for case in ("pattern","receipt"):
        run = json.loads((build/(case+"-generate-run.json")).read_text(encoding="utf-8-sig"))
        if run["scope"] != "generate" or run["exit_code"] not in (0,3) or sha((build/"ocr-generate.exe").read_bytes()).upper() != run["executable_sha256"]:
            raise ValueError("Unverified generation executable/run")
        for records,key in ((manifest["files"],"generation_weight_hashes"),(text_manifest["files"],"text_weight_hashes")):
            if run[key] != {name:item["sha256"].upper() for name,item in records.items() if name.endswith(".got")}:
                raise ValueError("Generation run weight drift")
        capture = Path(run["capture"])
        for name,digest in run["captures"].items():
            if sha((capture/name).read_bytes()).upper() != digest: raise ValueError("Changed capture: "+name)
        def captured(name,shape,dtype="<f2"):
            if name not in run["captures"]: raise ValueError("Unhashed capture: "+name)
            values = np.frombuffer((capture/name).read_bytes(),dtype=dtype)
            if values.size != np.prod(shape) or not np.isfinite(values).all(): raise ValueError("Invalid capture: "+name)
            return values.reshape(shape).copy()
        total,reason,prompt,limit = map(int,captured("0.generation-result.u32",(4,),"<u4"))
        timing = None
        if "0.timing.u64" in run["captures"]:
            values = captured("0.timing.u64",(10,),"<u8")
            if not values[0] or not values[-1]: raise ValueError("Invalid runtime clock")
            names = ("file_read","graph_finalize_subset","graph_execute_subset","generation_context","vision","prefill","head","decode","wall")
            timing = {name:int(value)/int(values[0]) for name,value in zip(names,values[1:],strict=True)}
            print(case,"seconds (overlapping phases)",timing,flush=True)
        context = manifest["context"]
        if not 1 <= total <= limit <= context or prompt+total > context or limit != run["max_new_tokens"]:
            raise ValueError("Invalid generation length")
        ids = captured("0.generated-ids.u32",(total,),"<u4")
        expected_reason = 1 if ids[-1] in (59246,59253) else 2 if total == limit else 3 if prompt+total == context else 0
        if not expected_reason or reason != expected_reason or run["exit_code"] != (0 if reason == 1 else 3) or any(token in (59246,59253) for token in ids[:-1]):
            raise ValueError("EOS/limit control failure")
        decoded = tokenizer.decode(ids.tolist(),skip_special_tokens=True,clean_up_tokenization_spaces=False).encode("utf-8")
        if (capture/"0.generated.txt").read_bytes() != decoded or (capture/"stdout.txt").read_bytes() != decoded:
            raise ValueError("UTF-8 text/stream differs from tokenizer")
        layout = captured("0.text-layout.u32",(7,),"<u4")
        delta = int(layout.view("<i4")[6])
        if int(layout[0]) != prompt: raise ValueError("Prompt count mismatch")
        rows = run.get("prefill_context",64)
        if rows not in (64,256) or prompt > rows: raise ValueError("Unsupported prefill geometry")
        positions = captured("0.text-positions.i32",(3,1,rows),"<i4")[:,:,:prompt]
        embeddings = captured("0.text-embeddings.f16",(rows,1536))[:prompt]
        stages = []; keys = []; values = []
        first = []
        for index in range(16):
            taps = captured(f"{index}.text-taps.f16",(rows*9728+16*rows*rows,))
            first.append(taps[4*rows*1536:5*rows*1536].reshape(rows,1536)[:prompt])
            keys.append(taps[5*rows*1536:5*rows*1536+rows*1024].reshape(rows,8,128)[:prompt].copy())
            values.append(taps[5*rows*1536+rows*1024:rows*9728].reshape(rows,8,128)[:prompt].copy())
        final = captured("16.text-norm.f16",(rows,1536))[prompt-1]
        first.append(final.reshape(1,1536)); stages.append(first)
        head_inputs = []; logits = []
        for step in range(total):
            if step:
                past = prompt+step-1
                if captured(f"{past}.decode-layout.u32",(4,),"<u4").tolist() != [past,past+delta,int(ids[step-1]),prompt]:
                    raise ValueError("Decode token/position/cache offset mismatch")
                previous = np.fromfile(text_output/"embeddings.got",dtype="<f2",count=1536,offset=160+int(ids[step-1])*3072)
                current = []
                for index in range(16):
                    cached_hash = captured(f"{index}.decode-{past:02}-cache.u8",(64,),"u1").tobytes()
                    if cached_hash != bytes.fromhex(sha(keys[index].tobytes())+sha(values[index].tobytes())):
                        raise ValueError("Resident KV cache prefix is not bit-exact")
                    source = captured(f"{index}.decode-{past:02}-input.f16",(1536,))
                    if source.tobytes() != previous.tobytes(): raise ValueError("Decode embedding/layer handoff mismatch")
                    attention_length = manifest["context"] if run.get("reuse_decode") else past+1
                    taps = captured(f"{index}.decode-{past:02}-taps.f16",(9728+16*attention_length,))
                    previous = taps[6144:7680].copy(); current.append(previous.reshape(1,1536))
                    keys[index] = np.concatenate((keys[index],taps[7680:8704].reshape(1,8,128)))
                    values[index] = np.concatenate((values[index],taps[8704:9728].reshape(1,8,128)))
                    probabilities = taps[9728:].reshape(16,attention_length).astype(np.float32)
                    if run.get("reuse_decode") and np.any(probabilities[:,past:-1] != 0): raise ValueError("Retained-cache padding is not masked")
                    if np.any(probabilities < 0) or np.any(probabilities > 1) or np.any(np.abs(probabilities.sum(-1)-1)>0.003):
                        raise ValueError("Decode attention probability failure")
                final = captured(f"{past}.decode-norm.f16",(1536,)); current.append(final.reshape(1,1536)); stages.append(current)
                if f"{past}.decode-cosine.f32" in run["captures"]:
                    config = GlmOcrTextConfig(**read_json(model/"config.json")["text_config"])
                    with torch.inference_mode():
                        cosine,sine = reference.GlmOcrTextRotaryEmbedding(config)(torch.zeros(1,1,1536),torch.full((3,1,1),past+delta,dtype=torch.long))
                    for name,expected in (("cosine",cosine),("sine",sine)):
                        if captured(f"{past}.decode-{name}.f32",(128,),"<f4").tobytes() != expected.numpy().astype("<f4").tobytes():
                            raise ValueError("Decode RoPE differs from model")
            source = captured(f"{step}.head-input.f16",(1536,))
            if source.tobytes() != final.tobytes(): raise ValueError("Head selected wrong hidden-state row")
            scores = captured(f"{step}.logits.f16",(59392,))
            if int(scores.argmax()) != int(ids[step]): raise ValueError("Greedy selection or tie-breaking mismatch")
            head_inputs.append(source); logits.append(scores)
        cases[case] = {"run":run,"ids":ids,"stages":stages,"embeddings":embeddings,"positions":positions,"delta":delta,
                       "head_inputs":head_inputs,"logits":logits,"prompt":prompt,"text":decoded.decode("utf-8"),"stop_reason":reason,"scopes":{},"timing_seconds":timing}
        print("PASS",case,"exact resident KV prefixes, decode positions/handoffs, greedy selection, UTF-8 and stop reason",reason,flush=True)
    state = {}; head = None
    with weight_source(model) as (stream,tensors,payload,catalog):
        identity = next(item["sha256"] for item in catalog["files"] if item["name"] == "model.safetensors")
        if identity != manifest["source_sha256"] or identity != text_manifest["source_sha256"]: raise ValueError("Wrong source weights")
        for name,tensor in tensors.items():
            local = name.removeprefix("model.language_model.")
            if name != "lm_head.weight" and (local == name or local.startswith("layers.16.")): continue
            start,end = tensor["data_offsets"]; stream.seek(payload+start); raw = stream.read(end-start)
            if tensor["dtype"] != "BF16" or len(raw) != end-start: raise ValueError("Invalid reference tensor")
            value = torch.from_numpy(((np.frombuffer(raw,dtype="<u2").astype(np.uint32)<<16).view(np.float32).reshape(tensor["shape"])).copy())
            if name == "lm_head.weight": head = value
            else: state[local] = value
    config = GlmOcrTextConfig(**read_json(model/"config.json")["text_config"]); config._attn_implementation = "eager"
    decoder = reference.GlmOcrTextModel(config).eval(); decoder.load_state_dict(state,strict=True); del state
    if head is None: raise ValueError("Missing head")
    for index in range(8):
        deployed = np.fromfile(output/f"head-{index:02}.got",dtype="<f2",offset=164).reshape(1536,7424)
        if deployed.tobytes() != head[index*7424:(index+1)*7424].half().t().contiguous().numpy().tobytes():
            raise ValueError("Deployed head differs from candidate reference")
    for scope in ("original","candidate"):
        if scope == "candidate": decoder.half().float(); head = head.half().float()
        for case,data in cases.items():
            cache = None; results = []
            for step,token in enumerate(data["ids"]):
                observed = []
                def hook(module,arguments,result): observed.append(result.detach().clone())
                handles = [layer.register_forward_hook(hook) for layer in decoder.layers]
                try:
                    with torch.inference_mode():
                        if not step:
                            inputs = torch.from_numpy(data["embeddings"].astype(np.float32)).unsqueeze(0)
                            positions = torch.from_numpy(data["positions"].astype(np.int64))
                        else:
                            inputs = decoder.embed_tokens(torch.tensor([[int(data["ids"][step-1])]]))
                            positions = torch.full((3,1,1),data["prompt"]+step-1+data["delta"],dtype=torch.long)
                        result = decoder(inputs_embeds=inputs,position_ids=positions,attention_mask=torch.ones(1,data["prompt"]+step,dtype=torch.long),past_key_values=cache,use_cache=True)
                        cache = result.past_key_values
                        hidden = result.last_hidden_state[:,-1]
                        reference_logits = (hidden@head.t()).numpy().reshape(-1)
                        local_logits = (torch.from_numpy(data["head_inputs"][step].astype(np.float32))@head.t()).numpy()
                    layer_metrics = [metric(actual,expected.numpy()[0]) for actual,expected in zip(data["stages"][step][:16],observed,strict=True)]
                    norm_metric = metric(data["stages"][step][16],hidden.numpy())
                    logit_metric = metric(data["logits"][step],reference_logits)
                    local_metric = metric(data["logits"][step],local_logits)
                    results.append({"step":step,"selected":int(token),"reference_selected":int(reference_logits.argmax()),
                                    "selection_agrees":int(token)==int(reference_logits.argmax()),"local_head_selected":int(local_logits.argmax()),
                                    "local_head_selection_agrees":int(token)==int(local_logits.argmax()),"layers":layer_metrics,
                                    "reference_top2_margin":float(np.partition(reference_logits,-2)[-1]-np.partition(reference_logits,-2)[-2]),
                                    "final_norm":norm_metric,"logits":logit_metric,"local_head":local_metric})
                finally:
                    for handle in handles: handle.remove()
            data["scopes"][scope] = results
            print(case,scope,"selected/reference",[(item["selected"],item["reference_selected"]) for item in results],
                  "logit violations",[item["logits"]["failures"] for item in results],flush=True)
    reports = {}
    for case,data in cases.items():
        accepted = all(not metric["failures"] for scope in data["scopes"].values() for step in scope for metric in step["layers"]+[step["final_norm"],step["logits"],step["local_head"]])
        reports[case] = {"run":data["run"],"text":data["text"],"ids":data["ids"].tolist(),"stop_reason":data["stop_reason"],
                         "exact_cache_prefixes":True,"exact_handoffs":True,"exact_greedy_selection":True,"exact_utf8":True,
                         "reference_scopes":data["scopes"],"numerical_gate_pass":accepted,"timing_seconds":data["timing_seconds"]}
    report = {"schema_version":1,"analyzer_sha256":sha(Path(__file__).read_bytes()),"modeling_sha256":module_hash,"versions":versions,
              "conditioning":"native Vision embeddings and native token prefixes; reference caches evolve independently",
              "tolerance":{"absolute":0.003,"relative":0.005},"cases":reports,
              "numerical_gate_pass":all(case["numerical_gate_pass"] for case in reports.values())}
    (build/"generation-analysis.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print("PASS generation structural/reference analysis; numerical gate:",report["numerical_gate_pass"],flush=True)


def export_generation(model, output, tokenizer_output):
    import numpy as np
    torch, _, _, _ = image_reference()
    from transformers.models.glm_ocr.modeling_glm_ocr import GlmOcrTextRotaryEmbedding
    from transformers.models.glm_ocr.configuration_glm_ocr import GlmOcrTextConfig
    generation = read_json(model/"generation_config.json")
    if generation["eos_token_id"] != [59246,59253] or generation["do_sample"]:
        raise ValueError("Generation policy drift")
    output.mkdir(parents=True,exist_ok=True)
    files = {}
    def store(name,data):
        path = output/name
        if path.exists() and path.read_bytes() != data:
            raise ValueError("Existing generation artifact differs: "+name)
        if not path.exists():
            with path.open("xb") as destination:
                destination.write(data)
        files[name] = {"bytes":len(data),"sha256":sha(data)}
    with weight_source(model) as (stream,tensors,payload,catalog):
        tensor = tensors["lm_head.weight"]
        if tensor["dtype"] != "BF16" or tensor["shape"] != [59392,1536]:
            raise ValueError("LM head geometry drift")
        identity = bytes.fromhex(next(item["sha256"] for item in catalog["files"] if item["name"] == "model.safetensors"))
        start,end = tensor["data_offsets"]
        if end-start != 59392*1536*2: raise ValueError("LM head size drift")
        stream.seek(payload+start)
        for index in range(8):
            raw = stream.read(7424*1536*2)
            if len(raw) != 7424*1536*2: raise ValueError("Truncated LM head")
            values = (np.frombuffer(raw,dtype="<u2").astype(np.uint32) << 16).view(np.float32).reshape(7424,1536)
            candidate = values.astype("<f2")
            if not np.isfinite(values).all() or not np.isfinite(candidate).all(): raise ValueError("LM head overflow")
            store(f"head-{index:02}.got",envelope(identity+struct.pack("<I",index)+candidate.T.copy().tobytes(),17,catalog))
    config = GlmOcrTextConfig(**read_json(model/"config.json")["text_config"])
    with torch.inference_mode():
        cosine,sine = GlmOcrTextRotaryEmbedding(config)(torch.zeros(1,256,1536),torch.arange(256).view(1,1,256).expand(3,1,256))
    store("decode-rope.got",envelope(identity+cosine.numpy().astype("<f4").tobytes()+sine.numpy().astype("<f4").tobytes(),18,catalog))
    tokenizer = (tokenizer_output/"tokenizer.got").read_bytes()
    if tokenizer[:8] != b"GLMOCR2\0" or struct.unpack_from("<I",tokenizer,12)[0] != 1 or sha(tokenizer[:96]+tokenizer[128:]) != tokenizer[96:128].hex():
        raise ValueError("Invalid native tokenizer artifact")
    store("tokenizer.got",tokenizer)
    manifest = {"schema_version":1,"source_sha256":identity.hex(),"files":files,"vocab_size":59392,
                "head_chunks":8,"context":256,"prefill_context":64,"eos_token_id":generation["eos_token_id"],"do_sample":False,
                "generation_config_sha256":sha((model/"generation_config.json").read_bytes())}
    store("generation-manifest.json",(json.dumps(manifest,indent=2)+"\n").encode())
    print("PASS generation export: eight untied LM head chunks and native tokenizer",flush=True)


def export_text_decoder(model, output, vision_build):
    import inspect as source_inspect
    import numpy as np
    from types import SimpleNamespace, MethodType
    torch, _, _, versions = image_reference()
    from transformers import AutoTokenizer
    from transformers.models.glm_ocr import modeling_glm_ocr as reference
    from transformers.models.glm_ocr.configuration_glm_ocr import GlmOcrTextConfig
    module_hash = sha(Path(source_inspect.getfile(reference)).read_bytes())
    if module_hash != "aea6387985dad1f0f5124f9344cc98849be8a7f2c26652ac3d914f6eefac6cc6":
        raise ValueError("Text decoder reference source drift")
    state = {}
    with weight_source(model) as (stream,tensors,payload,catalog):
        for name,tensor in tensors.items():
            if not name.startswith("model.language_model."):
                continue
            local = name.removeprefix("model.language_model.")
            if local.startswith("layers.16."):
                continue
            start,end = tensor["data_offsets"]; stream.seek(payload+start); raw = stream.read(end-start)
            if len(raw) != end-start:
                raise ValueError("Truncated text weight")
            values = (np.frombuffer(raw,dtype="<u2").astype(np.uint32) << 16).view(np.float32).reshape(tensor["shape"])
            if not np.isfinite(values).all() or not np.isfinite(values.astype(np.float16)).all():
                raise ValueError("Text FP16 weight overflow")
            state[local] = torch.from_numpy(values.copy())
    config = GlmOcrTextConfig(**read_json(model/"config.json")["text_config"]); config._attn_implementation = "eager"
    if (config.num_hidden_layers,config.hidden_size,config.intermediate_size,config.head_dim,config.num_attention_heads,config.num_key_value_heads,config.vocab_size) != (16,1536,4608,128,16,8,59392):
        raise ValueError("Text geometry drift")
    decoder = reference.GlmOcrTextModel(config).eval(); decoder.load_state_dict(state,strict=True)
    output.mkdir(parents=True,exist_ok=True); files = {}
    def store(name,data):
        path = output/name
        if path.exists() and sha(path.read_bytes()) != sha(data):
            raise ValueError("Existing text artifact differs: "+name)
        if not path.exists():
            with path.open("xb") as stream:
                stream.write(data)
        files[name] = {"bytes":len(data),"sha256":sha(data)}
    def half(value):
        return value.detach().half().contiguous().numpy().astype("<f2").tobytes()
    identity = bytes.fromhex(next(item["sha256"] for item in catalog["files"] if item["name"] == "model.safetensors"))
    store("embeddings.got",envelope(identity+half(state["embed_tokens.weight"]),13,catalog))
    for index in range(16):
        constants = b""
        for name in ("input_layernorm.weight","self_attn.q_proj.weight","self_attn.k_proj.weight","self_attn.v_proj.weight","self_attn.o_proj.weight",
                     "post_self_attn_layernorm.weight","post_attention_layernorm.weight","mlp.gate_up_proj.weight","mlp.down_proj.weight","post_mlp_layernorm.weight"):
            value = state[f"layers.{index}."+name]
            constants += half(value.t() if value.ndim == 2 else value)
        if len(constants) != 61353984:
            raise ValueError("Text block layout drift")
        store(f"text-{index:02}.got",envelope(identity+struct.pack("<I",index)+constants,14,catalog))
    positions = torch.arange(64).view(1,1,64).expand(3,1,64)
    with torch.inference_mode():
        cosine,sine = decoder.rotary_emb(torch.zeros(1,64,1536),positions)
    store("text-shared.got",envelope(identity+half(state["norm.weight"])+cosine.numpy().astype("<f4").tobytes()+sine.numpy().astype("<f4").tobytes(),15,catalog))
    del state
    tokenizer = AutoTokenizer.from_pretrained(str(model),local_files_only=True,trust_remote_code=False)
    position_model = SimpleNamespace(config=SimpleNamespace(vision_config=SimpleNamespace(spatial_merge_size=2)))
    position_model.get_vision_position_ids = MethodType(reference.GlmOcrModel.get_vision_position_ids,position_model)
    cases = []; fixture_records = []
    for case,width in (("pattern",8),("receipt",16)):
        run = json.loads((vision_build/(case+"-vision-run.json")).read_text(encoding="utf-8-sig"))
        raw_features = (Path(run["capture"])/"26.features.f16").read_bytes()
        if run["exit_code"] != 0 or sha(raw_features).upper() != run["captures"]["26.features.f16"]:
            raise ValueError("Unverified vision features")
        features = torch.from_numpy(np.frombuffer(raw_features,dtype="<f2").astype(np.float32)).reshape(width*2,1536)
        store(case+".features.f16",raw_features)
        for task,text in enumerate(TASKS):
            for no_think in (0,1):
                kwargs = {"add_generation_prompt":True,"tokenize":False}
                if no_think: kwargs["enable_thinking"] = False
                rendered = tokenizer.apply_chat_template([{"role":"user","content":[{"type":"image"},{"type":"text","text":text}]}],**kwargs)
                ids = torch.tensor([tokenizer.encode(rendered.replace("<|image|>","<|image|>"*(width*2)),add_special_tokens=False)])
                count = ids.shape[1]; modalities = ids == 59280
                positions,delta = reference.GlmOcrModel.get_rope_index(position_model,ids,modalities.long(),torch.tensor([[1,8,width]]))
                if count > 64: raise ValueError("Text context too long")
                padded_ids = torch.full((1,64),59246,dtype=torch.long); padded_ids[:,:count] = ids
                padded_positions = torch.zeros((3,1,64),dtype=torch.long); padded_positions[:,:,:count] = positions
                attention = torch.zeros((1,64),dtype=torch.long); attention[:,:count] = 1
                with torch.inference_mode():
                    inputs = decoder.embed_tokens(padded_ids).half().float(); inputs[:,count:] = 0
                    inputs[:,:count][modalities] = features
                mask = np.where((np.arange(64)[None,:] <= np.arange(64)[:,None]) & (np.arange(64)[None,:] < count),0,-65504).astype("<f2")
                fixture_records.append(struct.pack("<6Ii",task,no_think,8,width,count,width*2,delta.item())+
                    padded_ids.numpy().astype("<u4").tobytes()+padded_positions.numpy().astype("<i4").tobytes()+
                    np.pad(modalities.numpy().astype(np.uint8).reshape(-1),(0,64-count)).tobytes()+mask.tobytes()+half(inputs))
                if task == 0 and no_think == 0:
                    cases.append((case,padded_ids,padded_positions,attention,features,count))
    store("input-fixtures.got",envelope(struct.pack("<I",len(fixture_records))+b"".join(fixture_records),16,catalog))
    for scope in ("original","candidate"):
        if scope == "candidate": decoder.half().float()
        for case,ids,positions,attention,features,count in cases:
            with torch.inference_mode():
                inputs = decoder.embed_tokens(ids); inputs[:,count:] = 0
                inputs[ids == 59280] = features
            captured = []
            def hook(module,arguments,result): captured.append(result.detach().clone())
            handles = [layer.register_forward_hook(hook) for layer in decoder.layers]+[decoder.norm.register_forward_hook(hook)]
            try:
                with torch.inference_mode(): result = decoder(inputs_embeds=inputs,position_ids=positions,attention_mask=attention,use_cache=False)
                if len(captured) != 17 or any(not torch.isfinite(value).all() for value in captured): raise ValueError("Bad text reference")
                store(case+"."+scope+".f32",b"".join(value.contiguous().numpy().astype("<f4").tobytes() for value in [inputs]+captured))
            finally:
                for handle in handles: handle.remove()
            print("PASS text prefill oracle",case,scope,"tokens",count,flush=True)
    manifest = {"schema_version":1,"source_sha256":identity.hex(),"modeling_sha256":module_hash,"versions":versions,"files":files,
                "context":64,"layers":16,"reference_vision_build":str(vision_build),"prompt_tasks":TASKS,
                "prompt_source":"https://huggingface.co/zai-org/GLM-OCR","generation":False}
    store("manifest.json",(json.dumps(manifest,indent=2)+"\n").encode())
    print("PASS text decoder export",len(files),"files",flush=True)


def analyze_text_prefill(model, output, build):
    import inspect as source_inspect
    import numpy as np
    torch, _, _, _ = image_reference()
    from transformers.models.glm_ocr import modeling_glm_ocr as reference
    from transformers.models.glm_ocr.configuration_glm_ocr import GlmOcrTextConfig
    manifest = read_json(output/"manifest.json")
    if manifest["layers"] != 16 or manifest["context"] != 64 or sha(Path(source_inspect.getfile(reference)).read_bytes()) != manifest["modeling_sha256"]:
        raise ValueError("Text reference identity/geometry changed")
    for name,record in manifest["files"].items():
        with (output/name).open("rb") as stream:
            digest = hashlib.file_digest(stream,"sha256").hexdigest()
        if (output/name).stat().st_size != record["bytes"] or digest != record["sha256"]:
            raise ValueError("Text artifact changed: "+name)
    config = GlmOcrTextConfig(**read_json(model/"config.json")["text_config"])
    fixtures = (output/"input-fixtures.got").read_bytes()
    reports = {}
    for case,record_index in (("pattern",0),("receipt",6)):
        run = json.loads((build/(case+"-prefill-run.json")).read_text(encoding="utf-8-sig"))
        if run["exit_code"] != 0 or run["scope"] != "prefill" or sha((build/"ocr-prefill.exe").read_bytes()).upper() != run["executable_sha256"]:
            raise ValueError("Unverified prefill executable/run")
        expected_hashes = {name:item["sha256"].upper() for name,item in manifest["files"].items() if name.endswith(".got")}
        if run["text_weight_hashes"] != expected_hashes:
            raise ValueError("Prefill weights differ from reference")
        capture = Path(run["capture"])
        def captured(name,elements,dtype="<f2"):
            raw = (capture/name).read_bytes()
            if sha(raw).upper() != run["captures"][name]: raise ValueError("Prefill capture changed: "+name)
            values = np.frombuffer(raw,dtype=dtype)
            if values.size != elements or not np.isfinite(values).all(): raise ValueError("Invalid prefill capture: "+name)
            return values
        record = fixtures[132+record_index*205916:132+(record_index+1)*205916]
        task,no_think,height,width,count,image_tokens,delta = struct.unpack_from("<6Ii",record)
        if captured("0.text-layout.u32",7,"<u4").tobytes() != struct.pack("<7I",count,image_tokens,task,no_think,height,width,delta&0xffffffff):
            raise ValueError("Wrong multimodal layout")
        cursor = 28
        for name,elements,dtype in (("0.text-ids.u32",64,"<u4"),("0.text-positions.i32",192,"<i4"),
                                    ("0.text-modalities.u8",64,"u1"),("0.text-mask.f16",4096,"<f2"),("0.text-embeddings.f16",98304,"<f2")):
            actual = captured(name,elements,dtype); expected_bytes = record[cursor:cursor+actual.nbytes]
            if actual.tobytes() != expected_bytes: raise ValueError("Multimodal boundary differs from oracle: "+name)
            cursor += actual.nbytes
        features = captured("26.features.f16",image_tokens*1536)
        if features.tobytes() != (output/(case+".features.f16")).read_bytes():
            raise ValueError("Vision features differ from conditional text reference")
        positions = torch.from_numpy(captured("0.text-positions.i32",192,"<i4").copy().reshape(3,1,64)).long()
        with torch.inference_mode():
            cosine,sine = reference.GlmOcrTextRotaryEmbedding(config)(torch.zeros(1,64,1536),positions)
        for name,value in (("cosine",cosine),("sine",sine)):
            if captured("0.text-"+name+".f32",64*128,"<f4").tobytes() != value.numpy().astype("<f4").tobytes():
                raise ValueError("Native text mRoPE differs from pinned model")
        stages = [captured("0.text-embeddings.f16",98304).reshape(64,1536)]
        mask = captured("0.text-mask.f16",4096).reshape(64,64)
        for index in range(16):
            source = captured(f"{index}.text-input.f16",98304)
            if source.tobytes() != stages[-1].tobytes(): raise ValueError("Text handoff differs")
            taps = captured(f"{index}.text-taps.f16",688128)
            probabilities = taps[622592:].reshape(16,64,64).astype(np.float32)
            if np.any(probabilities[:,mask != 0] != 0) or np.any(probabilities < 0) or np.any(probabilities > 1) or np.any(np.abs(probabilities.sum(-1)-1) > 0.003):
                raise ValueError("Causal/padding mask failure")
            stages.append(taps[4*98304:5*98304].reshape(64,1536))
        stages.append(captured("16.text-norm.f16",98304).reshape(64,1536))
        stage_names = ["embeddings"]+[f"layer-{index}" for index in range(16)]+["final_norm"]
        scopes = {}
        for scope in ("original","candidate"):
            expected = np.frombuffer((output/(case+"."+scope+".f32")).read_bytes(),dtype="<f4").reshape(18,64,1536)
            results = {}
            for name,actual,target in zip(stage_names,stages,expected,strict=True):
                error = actual[:count].astype(np.float64)-target[:count].astype(np.float64)
                limit = 0.003+0.005*np.abs(target[:count].astype(np.float64))
                results[name] = {"elements":int(error.size),"failures":int((np.abs(error)>limit).sum()),
                                 "last_prompt_token_failures":int((np.abs(error[-1])>limit[-1]).sum()),
                                 "rmse":float(np.sqrt(np.mean(error*error))),"max_absolute_error":float(np.abs(error).max())}
            scopes[scope] = results
            print(case,scope,"prefill failures",[(name,values["failures"]) for name,values in results.items()],flush=True)
        gamma = np.frombuffer((output/"text-shared.got").read_bytes(),dtype="<f2",count=1536,offset=160).astype(np.float64)
        observed = stages[-2][:count].astype(np.float64)
        ideal = observed/np.sqrt(np.mean(observed*observed,axis=-1,keepdims=True)+1e-5)*gamma
        local_error = stages[-1][:count].astype(np.float64)-ideal
        local = {"reference":"FP64 RMSNorm on captured layer-15 output and deployed gamma", "failures":int((np.abs(local_error)>0.003+0.005*np.abs(ideal)).sum()),
                 "rmse":float(np.sqrt(np.mean(local_error*local_error))),"max_absolute_error":float(np.abs(local_error).max())}
        print(case,"final norm local",local,"last prompt token failures",{scope:stages["final_norm"]["last_prompt_token_failures"] for scope,stages in scopes.items()},flush=True)
        reports[case] = {"run":run,"count":count,"task":TASKS[task],"no_think":bool(no_think),"input_boundary_bit_exact":True,
                         "mrope_bit_exact":True,"handoffs_bit_exact":True,"causal_padding_mask_pass":True,"finite_captured_taps":True,
                         "stages":scopes,"final_norm_local":local,"numerical_gate_pass":all(not item["failures"] for scope in scopes.values() for item in scope.values())}
    report = {"schema_version":1,"analyzer_sha256":sha(Path(__file__).read_bytes()),"manifest_sha256":sha((output/"manifest.json").read_bytes()),
              "conditioning":"actual native vision features, not original end-to-end image inference", "tolerance":{"absolute":0.003,"relative":0.005},
              "cases":reports,"numerical_gate_pass":all(case["numerical_gate_pass"] for case in reports.values()),"token_generation":False}
    (build/"prefill-analysis.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print("PASS multimodal boundary/mRoPE/causal mask/handoff analysis; numerical gate:",report["numerical_gate_pass"],flush=True)


def export_vision_chain(model, output, case):
    records,reports = [],[]
    for block_index in range(2):
        record,report,catalog = export_vision_attention(model,output,case,full_block=True,block_index=block_index,collect_only=True)
        records.append(struct.pack("<I",10+block_index)+record[4:])
        reports.append(report)
    if reports[0]["rgb_sha256"] != reports[1]["rgb_sha256"] or reports[0]["grid_thw"] != reports[1]["grid_thw"]:
        raise ValueError("Chain input identity mismatch")
    data = envelope(bytes.fromhex(reports[0]["source_sha256"])+struct.pack("<I",2)+b"".join(records),9,catalog)
    output.mkdir(parents=True,exist_ok=True)
    target = output/"htp-fixtures.got"
    if target.exists() and target.read_bytes() != data:
        raise ValueError("Existing chain fixtures differ; choose a new output directory")
    if not target.exists():
        temporary = target.with_suffix(".partial")
        temporary.write_bytes(data)
        os.replace(temporary,target)
    manifest = {"schema_version":1,"case":case,"sha256":sha(data),"size":len(data),"blocks":reports,
                "exporter_sha256":sha(Path(__file__).read_bytes()),"full_model_inference":False,
                "scope":"Sequential vision blocks 0 and 1; second input comes from preceding block, without oracle reset",
                "candidate_semantics":"FP16-rounded initial input and weights expanded to FP32; no intermediate rounding in oracle"}
    (output/"manifest.json").write_text(json.dumps(manifest,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS vision chain oracle:",len(data),"bytes",sha(data),flush=True)


def analyze_chain_boundary(model, output, build, compare_build=None):
    import numpy as np
    fixture = (output/"htp-fixtures.got").read_bytes()
    manifest = read_json(output/"manifest.json")
    run = json.loads((build/"htp-probe.json").read_text(encoding="utf-8-sig"))
    if sha(fixture) != manifest["sha256"] or sha(fixture) != run["fixtures_sha256"] or sha((build/"ocr-htp-test.exe").read_bytes()) != run["executable_sha256"]:
        raise ValueError("Chain analysis identity mismatch")
    if struct.unpack_from("<I",fixture,12)[0] != 9 or hashlib.sha256(fixture[:96]+fixture[128:]).digest() != fixture[96:128] or struct.unpack_from("<I",fixture,160)[0] != 2:
        raise ValueError("Chain envelope mismatch")
    tokens = struct.unpack_from("<I",fixture,168)[0]
    if tokens not in (64,128):
        raise ValueError("Invalid chain bucket")
    sizes = [16*tokens*tokens if tap in (6,7) else tokens*(3072 if tap == 1 else 4096 if 12 <= tap <= 15 else 1024) for tap in range(18)]
    offsets = np.cumsum([0]+sizes).tolist()
    total = offsets[-1]
    constant_bytes = 33585408+tokens*512
    cursor,records = 164,[]
    for block_index in range(2):
        header = struct.unpack_from("<7I",fixture,cursor)
        input_bytes = tokens*2048 if block_index == 0 else 0
        if header != (10+block_index,tokens,1024,1024,input_bytes,constant_bytes,total*8):
            raise ValueError("Invalid chain record")
        start = cursor+28
        references = np.frombuffer(fixture,dtype="<f4",count=total*2,offset=start+input_bytes+constant_bytes).reshape(2,total).astype(np.float64)
        records.append((start,input_bytes,references))
        cursor = start+input_bytes+constant_bytes+total*8
    if cursor != len(fixture):
        raise ValueError("Invalid chain length")
    captures = {}
    capture_dir = Path(run["capture_directory"])
    if capture_dir.resolve().parent != build.resolve():
        raise ValueError("Capture directory outside run directory")
    for block_index in range(2):
        for suffix,count in (("input",tokens*1024),("taps",total)):
            name = f"{block_index}.{suffix}.f16"
            raw = (capture_dir/name).read_bytes()
            if len(raw) != count*2 or sha(raw) != run["capture_sha256"][name]:
                raise ValueError("Capture identity/length mismatch")
            values = np.frombuffer(raw,dtype="<f2")
            if not np.isfinite(values).all():
                raise ValueError("Nonfinite capture")
            captures[name] = values
    if captures["0.input.f16"].tobytes() != fixture[records[0][0]:records[0][0]+tokens*2048]:
        raise ValueError("Initial input differs from fixture")
    if captures["0.taps.f16"][offsets[17]:].tobytes() != captures["1.input.f16"].tobytes():
        raise ValueError("Block handoff changed bits")
    isolation = None
    late_native = bool(run.get("fuse_down_residual_block1"))
    attention_native = bool(run.get("fuse_attention_residual_block1"))
    block0_late_native = bool(run.get("fuse_down_residual_block0"))
    baseline_captures = {}
    baseline_output = None
    baseline_residual = None
    if attention_native and not late_native:
        raise ValueError("Attention residual analysis requires late down rounding")
    if block0_late_native and (not late_native or attention_native):
        raise ValueError("Block-0 late residual analysis requires isolated late-down settings")
    if late_native and compare_build is None:
        raise ValueError("Late residual analysis requires its unchanged control")
    if compare_build is not None:
        baseline = json.loads((compare_build/"htp-probe.json").read_text(encoding="utf-8-sig"))
        baseline_dir = Path(baseline["capture_directory"])
        if baseline["fixtures_sha256"] != sha(fixture) or baseline_dir.resolve().parent != compare_build.resolve():
            raise ValueError("Isolation baseline identity mismatch")
        if sha((compare_build/"ocr-htp-test.exe").read_bytes()) != baseline["executable_sha256"]:
            raise ValueError("Isolation baseline executable mismatch")
        if late_native:
            if bool(baseline.get("fuse_down_residual_block1")) != (attention_native or block0_late_native) or baseline.get("fuse_down_residual_block0") or baseline.get("fuse_attention_residual_block1") or not run.get("capture_internals"):
                raise ValueError("Invalid late residual control")
            for field in ("runtime_sha256","rope_implementation","rope_block1_only","capture_internals",
                          "refine_divide_block1","refine_silu_only","matrix_residual","matrix_residual_group"):
                if run.get(field) != baseline.get(field):
                    raise ValueError("Late residual control configuration mismatch: "+field)
        for name,values in captures.items():
            raw = (baseline_dir/name).read_bytes()
            if len(raw) != values.nbytes or sha(raw) != baseline["capture_sha256"][name]:
                raise ValueError("Isolation baseline capture mismatch")
            baseline_captures[name] = np.frombuffer(raw,dtype="<f2").astype(np.float64)
            if not np.isfinite(baseline_captures[name]).all():
                raise ValueError("Nonfinite isolation baseline")
            checked_bytes = offsets[10 if attention_native else 17 if late_native else 4]*2 if name == "1.taps.f16" else len(raw)
            if block0_late_native:
                checked_bytes = 0 if name.startswith("1.") else offsets[17]*2 if name == "0.taps.f16" else len(raw)
            if raw[:checked_bytes] != values.tobytes()[:checked_bytes]:
                raise ValueError("Isolation changed upstream tensors: "+name)
            if name == "1.taps.f16":
                baseline_output = np.frombuffer(raw,dtype="<f2")[offsets[17]:].astype(np.float64)
                baseline_residual = np.frombuffer(raw,dtype="<f2")[offsets[10]:offsets[11]].astype(np.float64)
        isolation = {"baseline_run":baseline,"block0_all_taps_bit_exact":not block0_late_native,
                     "block1_input_and_pre_rope_taps_bit_exact":not block0_late_native}
        if baseline_captures["0.taps.f16"][offsets[17]:].tobytes() != baseline_captures["1.input.f16"].tobytes():
            raise ValueError("Baseline block handoff mismatch")
        if late_native:
            for block_index in range(2):
                name = f"{block_index}.internals.f16"
                raw = (capture_dir/name).read_bytes()
                control_raw = (baseline_dir/name).read_bytes()
                count = 32*tokens+32*tokens*tokens+tokens*4096*(6 if run.get("matrix_residual") and block_index == 1 else 4)
                checked_bytes = (32*tokens+32*tokens*tokens)*2 if attention_native and block_index == 1 else count*2
                if block0_late_native and block_index == 1:
                    checked_bytes = 0
                if len(raw) != count*2 or len(control_raw) != count*2 or sha(raw) != run["capture_sha256"][name] or sha(control_raw) != baseline["capture_sha256"][name] or raw[:checked_bytes] != control_raw[:checked_bytes]:
                    raise ValueError("Late residual changed internal capture: "+name)
                if not np.isfinite(np.frombuffer(raw,dtype="<f2")).all():
                    raise ValueError("Nonfinite residual experiment capture")
            if block0_late_native:
                isolation["block0_through_down_and_internals_bit_exact"] = True
                isolation["block1_configuration_unchanged_input_changed"] = True
            else:
                isolation["block1_through_projection_bit_exact" if attention_native else "block1_through_down_bit_exact"] = True
                isolation["block0_and_block1_softmax_internal_captures_bit_exact" if attention_native else "all_internal_captures_bit_exact"] = True
    base_original,base_candidate = records[1][2]
    actual = captures["1.taps.f16"].astype(np.float64)
    actual_input = captures["1.input.f16"].astype(np.float32).reshape(tokens,1024)
    ideal_input = records[0][2][1,offsets[17]:].astype(np.float16).astype(np.float32).reshape(tokens,1024)
    conditional = {}
    def conditional_reference(inputs):
        record,metadata,_ = export_vision_attention(model,output,manifest["case"],full_block=True,block_index=1,collect_only=True,observed_input=inputs)
        if metadata["source_sha256"] != fixture[128:160].hex() or metadata["rgb_sha256"] != manifest["blocks"][1]["rgb_sha256"] or metadata["grid_thw"] != manifest["blocks"][1]["grid_thw"]:
            raise ValueError("Conditional source mismatch")
        if record[28:28+constant_bytes] != fixture[records[1][0]:records[1][0]+constant_bytes]:
            raise ValueError("Conditional weights differ from native fixture")
        return np.frombuffer(record,dtype="<f4",count=total*2,offset=28+constant_bytes).reshape(2,total).astype(np.float64)
    for name,inputs in (("actual_input",actual_input),("rounded_ideal_input",ideal_input)):
        conditional[name] = conditional_reference(inputs)
    def stats(left,right):
        error = left-right
        return {"max_abs":float(np.max(np.abs(error))),"rmse":float(np.sqrt(np.mean(error*error))),
                "out_of_tolerance":int(np.count_nonzero(np.abs(error) > 0.003+0.005*np.abs(right)))}
    observed = conditional["actual_input"][1]
    ideal = conditional["rounded_ideal_input"][1]
    block0_local_error_change = None
    if block0_late_native:
        control_input = baseline_captures["1.input.f16"].astype(np.float32).reshape(tokens,1024)
        control_oracle = conditional_reference(control_input)[1]
        conditional_change = observed-control_oracle
        measured_change = actual-baseline_captures["1.taps.f16"]
        local_change = measured_change-conditional_change
        block0_local_error_change = {}
        for tap,tap_name in ((6,"scores"),(17,"block_output")):
            section = slice(offsets[tap],offsets[tap+1])
            frozen = baseline_captures["1.taps.f16"][section]+conditional_change[section]
            reference = base_original[section]
            indices = np.flatnonzero(np.abs(actual[section]-reference) > 0.003+0.005*np.abs(reference))
            block0_local_error_change[tap_name] = {
                "frozen_control_local_error_vs_original":stats(frozen,reference),
                "local_error_change_rmse":float(np.sqrt(np.mean(local_change[section]**2))),
                "closure_max_abs":float(np.max(np.abs(conditional_change[section]+local_change[section]-measured_change[section]))),
                "actual_failures":[{"index":int(index),"conditional_input_effect":float(conditional_change[offsets[tap]+index]),
                    "local_execution_error_change":float(local_change[offsets[tap]+index]),
                    "measured_output_change":float(measured_change[offsets[tap]+index])} for index in indices],
                "interpretation":"Measured difference minus paired conditional-oracle difference; local execution error changes with the input, not an unchanged error term"}
    terms = {"initial_cast_and_weight_effect":base_candidate-base_original,
             "ideal_handoff_rounding_effect":ideal-base_candidate,
             "block0_execution_propagated":observed-ideal,
             "block1_local_execution":actual-observed}
    closure = float(np.max(np.abs(sum(terms.values())-(actual-base_original))))
    if closure > 1e-12:
        raise ValueError("Chain error decomposition does not close")
    taps,failures = {},{}
    for tap,name in enumerate(manifest["blocks"][1]["tap_order"]):
        section = slice(offsets[tap],offsets[tap+1])
        taps[name] = {"vs_original":stats(actual[section],base_original[section]),
                      "vs_candidate":stats(actual[section],base_candidate[section]),
                      "vs_matched_input_oracle":stats(actual[section],observed[section]),
                      "boundary_effect":stats(observed[section],base_candidate[section])}
        print(name,json.dumps(taps[name]),flush=True)
        indices = np.flatnonzero(np.abs(actual[section]-base_original[section]) > 0.003+0.005*np.abs(base_original[section]))
        failures[name] = [{"index":int(index),"actual":float(actual[offsets[tap]+index]),
                           "original":float(base_original[offsets[tap]+index]),
                           **{term:float(value[offsets[tap]+index]) for term,value in terms.items()}} for index in indices]
    score_section = slice(offsets[6],offsets[7])
    query = captures["1.taps.f16"][offsets[4]:offsets[5]].astype(np.float64).reshape(tokens,16,64).transpose(1,0,2)
    key = captures["1.taps.f16"][offsets[5]:offsets[6]].astype(np.float64).reshape(tokens,16,64).transpose(1,0,2)
    exact_scores = (query@key.transpose(0,2,1)*0.125).reshape(-1)
    qk = stats(actual[score_section],exact_scores)
    weights_start = records[1][0]
    gamma_start = weights_start+2048+6291456+6144
    query_gamma = np.frombuffer(fixture,dtype="<f2",count=64,offset=gamma_start).astype(np.float64)
    key_gamma = np.frombuffer(fixture,dtype="<f2",count=64,offset=gamma_start+128).astype(np.float64)
    cosine = np.frombuffer(fixture,dtype="<f4",count=tokens*64,offset=gamma_start+256).astype(np.float64).reshape(tokens,1,64)
    sine = np.frombuffer(fixture,dtype="<f4",count=tokens*64,offset=gamma_start+256+tokens*256).astype(np.float64).reshape(tokens,1,64)
    def rotated(value):
        swapped = np.concatenate((-value[...,32:],value[...,:32]),axis=-1)
        return value*cosine+swapped*sine
    def scores(query_value,key_value):
        return (query_value.transpose(1,0,2)@key_value.transpose(1,2,0)*0.125).reshape(-1)
    qkv = actual[offsets[1]:offsets[2]].reshape(tokens,3,16,64)
    norm_from_qkv = [qkv[:,branch]/np.sqrt(np.mean(qkv[:,branch]**2,axis=-1,keepdims=True)+1e-5)*gamma for branch,gamma in ((0,query_gamma),(1,key_gamma))]
    scores_from_qkv = scores(rotated(norm_from_qkv[0]),rotated(norm_from_qkv[1]))
    query_norm = actual[offsets[2]:offsets[3]].reshape(tokens,16,64)
    key_norm = actual[offsets[3]:offsets[4]].reshape(tokens,16,64)
    scores_from_norm = scores(rotated(query_norm),rotated(key_norm))
    score_terms = {"local_through_qkv":scores_from_qkv-observed[score_section],
                   "qk_norm_effect":scores_from_norm-scores_from_qkv,
                   "rope_effect":exact_scores-scores_from_norm,
                   "qk_dot_and_output_rounding":actual[score_section]-exact_scores}
    score_closure = float(np.max(np.abs(sum(score_terms.values())-(actual[score_section]-observed[score_section]))))
    if score_closure > 1e-12:
        raise ValueError("Local score decomposition does not close")
    score_failures = np.flatnonzero(np.abs(actual[score_section]-base_original[score_section]) > 0.003+0.005*np.abs(base_original[score_section]))
    score_summary = {name:{"all_rmse":float(np.sqrt(np.mean(value**2))),
                          "original_failure_mean_abs":float(np.mean(np.abs(value[score_failures])))} for name,value in score_terms.items()}
    rope_simulations = {}
    simulated_pairs = {}
    for mode in ("fp32_then_half","half_products_and_sum","half_coefficients_fp32_sum"):
        predictions,observations = [],[]
        for norm,tap in ((query_norm,4),(key_norm,5)):
            values = norm.astype(np.float32)
            swapped = np.concatenate((-values[...,32:],values[...,:32]),axis=-1)
            if mode == "fp32_then_half":
                predicted = (values*cosine.astype(np.float32)+swapped*sine.astype(np.float32)).astype(np.float16)
            elif mode == "half_products_and_sum":
                real = values.astype(np.float16)*cosine.astype(np.float16)
                imaginary = swapped.astype(np.float16)*sine.astype(np.float16)
                predicted = (real+imaginary).astype(np.float16)
            else:
                predicted = (values*cosine.astype(np.float16).astype(np.float32)+swapped*sine.astype(np.float16).astype(np.float32)).astype(np.float16)
            predictions.append(predicted)
            observations.append(captures["1.taps.f16"][offsets[tap]:offsets[tap+1]].reshape(tokens,16,64))
        predicted_flat = np.concatenate([value.reshape(-1) for value in predictions])
        observed_flat = np.concatenate([value.reshape(-1) for value in observations])
        simulated_pairs[mode] = predictions
        rope_simulations[mode] = {"bit_equal":int(np.count_nonzero(predicted_flat.view(np.uint16) == observed_flat.view(np.uint16))),
                                  "elements":len(predicted_flat),"max_abs":float(np.max(np.abs(predicted_flat.astype(np.float64)-observed_flat.astype(np.float64))))}
    rounded_rope_scores = scores(*(value.astype(np.float64) for value in simulated_pairs["fp32_then_half"]))
    rope_counterfactual = stats(rounded_rope_scores,base_original[score_section])
    def rounding_stats(predicted,observed_values):
        rounded = np.asarray(predicted,dtype=np.float16).reshape(-1)
        captured = np.asarray(observed_values,dtype=np.float16).reshape(-1)
        return {"bit_equal":int(np.count_nonzero(rounded.view(np.uint16) == captured.view(np.uint16))),
                "elements":len(rounded),"max_abs":float(np.max(np.abs(rounded.astype(np.float64)-captured.astype(np.float64))))}
    norm_weight = np.frombuffer(fixture,dtype="<f2",count=1024,offset=weights_start).astype(np.float64)
    incoming = actual_input.astype(np.float64)
    exact_norm1 = incoming/np.sqrt(np.mean(incoming**2,axis=-1,keepdims=True)+1e-5)*norm_weight
    observed_norm1 = actual[offsets[0]:offsets[1]].reshape(tokens,1024)
    qkv_weight = np.frombuffer(fixture,dtype="<f2",count=1024*3072,offset=weights_start+2048).astype(np.float64).reshape(1024,3072)
    qkv_bias = np.frombuffer(fixture,dtype="<f2",count=3072,offset=weights_start+2048+6291456).astype(np.float64)
    projected = observed_norm1@qkv_weight
    ideal_qkv = (projected+qkv_bias).reshape(tokens,3,16,64)
    rounded_product_qkv = (projected.astype(np.float16).astype(np.float64)+qkv_bias).reshape(tokens,3,16,64)
    upstream = {"norm1_single_round":rounding_stats(exact_norm1,observed_norm1),
                "qkv_single_round":rounding_stats(ideal_qkv,qkv),
                "qkv_product_then_bias_round":rounding_stats(rounded_product_qkv,qkv),
                "q_norm_single_round":rounding_stats(norm_from_qkv[0],query_norm),
                "k_norm_single_round":rounding_stats(norm_from_qkv[1],key_norm)}
    for branch,tap_name in ((0,"q_norm"),(1,"k_norm")):
        tap = 2+branch
        ideal_norm = norm_from_qkv[branch].reshape(-1)
        for row in failures[tap_name]:
            index = row["index"]
            row["observed_qkv_through_ideal_norm"] = float(ideal_norm[index])
            row["local_norm_effect"] = float(actual[offsets[tap]+index]-ideal_norm[index])
            row["upstream_effect"] = float(ideal_norm[index]-base_original[offsets[tap]+index])
    def ideal_norm_scores(projected_qkv):
        normalized = [projected_qkv[:,branch]/np.sqrt(np.mean(projected_qkv[:,branch]**2,axis=-1,keepdims=True)+1e-5)*gamma for branch,gamma in ((0,query_gamma),(1,key_gamma))]
        return scores(*(rotated(value) for value in normalized))
    upstream_score_counterfactuals = {
        "observed_qkv_ideal_norm_rope_dot":stats(scores_from_qkv,base_original[score_section]),
        "observed_norm1_ideal_qkv_norm_rope_dot":stats(ideal_norm_scores(ideal_qkv),base_original[score_section])}
    for row in failures["scores"]:
        row["allowed_error"] = 0.003+0.005*abs(row["original"])
        row["tolerance_fraction"] = abs(row["actual"]-row["original"])/row["allowed_error"]
        row["local_stages"] = {name:float(value[row["index"]]) for name,value in score_terms.items()}
    residual_sum = captures["1.taps.f16"][offsets[10]:offsets[11]].astype(np.float64)+captures["1.taps.f16"][offsets[16]:offsets[17]].astype(np.float64)
    residual = stats(actual[offsets[17]:],residual_sum)
    for row in failures["block_output"]:
        index = row["index"]
        row["allowed_error"] = 0.003+0.005*abs(row["original"])
        row["last_add_error"] = float(actual[offsets[17]+index]-residual_sum[index])
        row["last_add_comparison_is_separate_operation"] = not late_native
        row["actual_residual_operand"] = float(actual[offsets[10]+index])
        row["actual_down_operand"] = float(actual[offsets[16]+index])
        row["cancellation_factor"] = float((abs(base_original[offsets[10]+index])+abs(base_original[offsets[16]+index]))/abs(row["original"]))
    block0_record,block0_metadata,_ = export_vision_attention(model,output,manifest["case"],full_block=True,collect_only=True)
    block0_start,block0_input_bytes,block0_references = records[0]
    block0_end = block0_start+block0_input_bytes+constant_bytes+total*8
    if block0_record[28:] != fixture[block0_start:block0_end] or block0_metadata["tap_order"] != manifest["blocks"][0]["tap_order"]:
        raise ValueError("Block-0 oracle/fixture reconstruction mismatch")
    block0_original,block0_candidate = block0_references
    block0_actual = captures["0.taps.f16"].astype(np.float64)
    def block0_tap(values,tap):
        return values[offsets[tap]:offsets[tap+1]].reshape(tokens,-1)
    candidate_taps = [block0_tap(block0_candidate,tap) for tap in range(18)]
    actual_taps = [block0_tap(block0_actual,tap) for tap in range(18)]
    block0_input = captures["0.input.f16"].astype(np.float64).reshape(tokens,1024)
    constant_cursor = block0_start+block0_input_bytes+2048+6291456+6144+256+tokens*512
    def constant_half(shape):
        nonlocal constant_cursor
        count = int(np.prod(shape))
        values = np.frombuffer(fixture,dtype="<f2",count=count,offset=constant_cursor).astype(np.float64).reshape(shape)
        constant_cursor += count*2
        return values
    projection_weight,projection_bias = constant_half((1024,1024)),constant_half((1024,))
    norm2_weight = constant_half((1024,))
    gate_weight,gate_bias = constant_half((1024,4096)),constant_half((4096,))
    up_weight,up_bias = constant_half((1024,4096)),constant_half((4096,))
    down_weight,down_bias = constant_half((4096,1024)),constant_half((1024,))
    if constant_cursor != block0_start+block0_input_bytes+constant_bytes:
        raise ValueError("Block-0 constant layout mismatch")
    epsilon = read_json(model/"config.json")["vision_config"]["rms_norm_eps"]
    def normalized2(values):
        return values/np.sqrt(np.mean(values**2,axis=-1,keepdims=True)+epsilon)*norm2_weight
    def activated_gate(values):
        return values*np.exp(np.minimum(values,0))/(1+np.exp(-np.abs(values)))
    def down_from_projections(gate_values,up_values):
        return (activated_gate(gate_values)*up_values)@down_weight+down_bias
    def down_from_norm(values):
        return down_from_projections(values@gate_weight+gate_bias,values@up_weight+up_bias)
    candidate_projection_exact = candidate_taps[8]@projection_weight+projection_bias
    actual_projection_exact = actual_taps[8]@projection_weight+projection_bias
    candidate_down_exact = down_from_norm(normalized2(candidate_taps[10]))
    for name,predicted,expected in (("projection",candidate_projection_exact,candidate_taps[9]),
                                    ("MLP",candidate_down_exact,candidate_taps[16])):
        if not np.isfinite(predicted).all() or not np.allclose(predicted,expected,atol=1e-5,rtol=1e-5):
            raise ValueError("Block-0 FP64 reconstruction differs from pinned candidate "+name)
    down_after_residual = down_from_norm(normalized2(actual_taps[10]))
    down_after_norm = down_from_norm(actual_taps[11])
    down_after_gate = down_from_projections(actual_taps[12],actual_taps[11]@up_weight+up_bias)
    down_after_up = down_from_projections(actual_taps[12],actual_taps[13])
    down_after_silu = (actual_taps[14]*actual_taps[13])@down_weight+down_bias
    down_after_gated = actual_taps[15]@down_weight+down_bias
    block0_terms = {
        "initial_input_and_weight_cast":candidate_taps[17]-block0_tap(block0_original,17),
        "reference_residual_arithmetic":block0_input+candidate_taps[9]+candidate_taps[16]-candidate_taps[17],
        "attention_projection_reference_rounding":candidate_projection_exact-candidate_taps[9],
        "attention_context_propagated":actual_projection_exact-candidate_projection_exact,
        "attention_projection_local":actual_taps[9]-actual_projection_exact,
        "first_residual_add_local":actual_taps[10]-(block0_input+actual_taps[9]),
        "mlp_reference_rounding":candidate_down_exact-candidate_taps[16],
        "mlp_residual_input_propagated":down_after_residual-candidate_down_exact,
        "mlp_norm2_local_propagated":down_after_norm-down_after_residual,
        "mlp_gate_projection_local_propagated":down_after_gate-down_after_norm,
        "mlp_up_projection_local_propagated":down_after_up-down_after_gate,
        "mlp_silu_local_propagated":down_after_silu-down_after_up,
        "mlp_gated_multiply_local_propagated":down_after_gated-down_after_silu,
        "mlp_down_projection_local":actual_taps[16]-down_after_gated,
        "late_output_vs_separate_taps" if block0_late_native else "final_residual_add_local":actual_taps[17]-(actual_taps[10]+actual_taps[16])}
    block0_error = actual_taps[17]-block0_tap(block0_original,17)
    block0_closure = float(np.max(np.abs(sum(block0_terms.values())-block0_error)))
    if not all(np.isfinite(value).all() for value in block0_terms.values()) or block0_closure > 1e-12:
        raise ValueError("Block-0 output decomposition does not close")
    def component_summary(values):
        return {"max_abs":float(np.max(np.abs(values))),"rmse":float(np.sqrt(np.mean(values**2))),
                "mean_abs":float(np.mean(np.abs(values)))}
    block0_components = {name:component_summary(values) for name,values in block0_terms.items()}
    block0_taps = {name:{"vs_original":stats(actual_taps[tap],block0_tap(block0_original,tap)),
                        "vs_candidate":stats(actual_taps[tap],candidate_taps[tap])}
                   for tap,name in enumerate(block0_metadata["tap_order"])}
    block0_rounding = {name:rounding_stats(predicted,actual_taps[tap]) for name,predicted,tap in (
        ("attention_projection_single_round",actual_projection_exact,9),
        ("first_residual_add_single_round",block0_input+actual_taps[9],10),
        ("norm2_single_round",normalized2(actual_taps[10]),11),
        ("gate_projection_single_round",actual_taps[11]@gate_weight+gate_bias,12),
        ("up_projection_single_round",actual_taps[11]@up_weight+up_bias,13),
        ("silu_single_round",activated_gate(actual_taps[12]),14),
        ("gated_multiply_single_round",actual_taps[14]*actual_taps[13],15),
        ("down_projection_single_round",down_after_gated,16),
        ("separate_final_residual_counterfactual" if block0_late_native else "final_residual_add_single_round",actual_taps[10]+actual_taps[16],17))}
    block0_late_reference = {"native_path_enabled":block0_late_native,
        "captured_vs_single_round":rounding_stats(actual_taps[10]+down_after_gated,actual_taps[17]),
        "reference_vs_original":stats((actual_taps[10]+down_after_gated).astype(np.float16).astype(np.float64),block0_tap(block0_original,17))}
    block0_chain_comparison = None
    if block0_late_native:
        control_block0 = baseline_captures["0.taps.f16"][offsets[17]:].reshape(tokens,1024)
        block0_late_reference["control_vs_single_round"] = rounding_stats(actual_taps[10]+down_after_gated,control_block0)
        block0_late_reference["control_vs_original"] = stats(control_block0,block0_tap(block0_original,17))
        block0_late_reference["changed_handoff_elements"] = int(np.count_nonzero(control_block0 != actual_taps[17]))
        block0_chain_comparison = {}
        for tap,tap_name in enumerate(manifest["blocks"][1]["tap_order"]):
            section = slice(offsets[tap],offsets[tap+1])
            control_values = baseline_captures["1.taps.f16"][section]
            actual_values = actual[section]
            scopes = {}
            for scope,reference in (("original",base_original[section]),("candidate",base_candidate[section])):
                allowed = 0.003+0.005*np.abs(reference)
                control_bad = np.abs(control_values-reference) > allowed
                actual_bad = np.abs(actual_values-reference) > allowed
                indices = np.flatnonzero(control_bad | actual_bad)
                scopes[scope] = {"control":stats(control_values,reference),"actual":stats(actual_values,reference),
                    "new_failures":int(np.count_nonzero(actual_bad & ~control_bad)),
                    "fixed_failures":int(np.count_nonzero(control_bad & ~actual_bad)),
                    "failure_union":[{"index":int(index),"reference":float(reference[index]),"allowed":float(allowed[index]),
                        "control":float(control_values[index]),"actual":float(actual_values[index]),
                        "control_failed":bool(control_bad[index]),"actual_failed":bool(actual_bad[index])} for index in indices]}
            block0_chain_comparison[tap_name] = scopes
    normalized_error = np.abs(block0_error)/(0.003+0.005*np.abs(block0_tap(block0_original,17)))
    largest_indices = np.argsort(normalized_error.reshape(-1),kind="stable")[-12:][::-1]
    block0_largest = [{"index":int(index),"actual":float(actual_taps[17].reshape(-1)[index]),
                       "original":float(block0_original[offsets[17]+index]),
                       "tolerance_fraction":float(normalized_error.reshape(-1)[index]),
                       "components":{name:float(values.reshape(-1)[index]) for name,values in block0_terms.items()}}
                      for index in largest_indices]
    def half_round(values):
        return np.asarray(values,dtype=np.float16).astype(np.float64)
    exact_silu = activated_gate(actual_taps[12])
    single_round_silu = half_round(exact_silu)
    numerator = half_round(np.exp(np.minimum(actual_taps[12],0)))
    denominator = half_round(1+half_round(np.exp(-np.abs(actual_taps[12]))))
    staged_silu = half_round(actual_taps[12]*half_round(numerator/denominator))
    silu_components = {
        "final_output_rounding":((single_round_silu-exact_silu)*actual_taps[13])@down_weight,
        "simulated_intermediate_rounding":((staged_silu-single_round_silu)*actual_taps[13])@down_weight,
        "observed_minus_staged_simulation":((actual_taps[14]-staged_silu)*actual_taps[13])@down_weight}
    silu_closure = float(np.max(np.abs(sum(silu_components.values())-block0_terms["mlp_silu_local_propagated"])))
    if silu_closure > 1e-12:
        raise ValueError("Block-0 SiLU decomposition does not close")
    silu_simulations = {"single_round":single_round_silu,"staged_half":staged_silu,
                        "staged_reciprocal":half_round(actual_taps[12]*half_round(numerator*half_round(1/denominator))),
                        "half_exp_fp64_tail":half_round(actual_taps[12]*numerator/(1+half_round(np.exp(-np.abs(actual_taps[12]))))),
                        "half_divisor_fp64_tail":half_round(actual_taps[12]*numerator/denominator)}
    silu_by_sign = {name:{scope:rounding_stats(values[mask],actual_taps[14][mask])
                         for scope,mask in (("negative",actual_taps[12] < 0),("nonnegative",actual_taps[12] >= 0))}
                   for name,values in silu_simulations.items()}
    def softmax64(values):
        exponential = np.exp(values-np.max(values,axis=-1,keepdims=True))
        return exponential/np.sum(exponential,axis=-1,keepdims=True)
    def context64(probabilities,value):
        return (probabilities@value.transpose(1,0,2)).transpose(1,0,2).reshape(tokens,1024)
    candidate_value = candidate_taps[1].reshape(tokens,3,16,64)[:,2]
    actual_value = actual_taps[1].reshape(tokens,3,16,64)[:,2]
    actual_rope = [actual_taps[tap].reshape(tokens,16,64).transpose(1,0,2) for tap in (4,5)]
    exact_block0_scores = actual_rope[0]@actual_rope[1].transpose(0,2,1)*0.125
    candidate_probabilities = softmax64(candidate_taps[6].reshape(16,tokens,tokens))
    reconstructed_context = context64(candidate_probabilities,candidate_value)
    if not np.allclose(reconstructed_context,candidate_taps[8],atol=1e-6,rtol=1e-5):
        raise ValueError("Block-0 attention reconstruction differs from pinned candidate")
    context_stages = {
        "reference_attention_rounding":reconstructed_context,
        "upstream_qk_through_softmax":context64(softmax64(exact_block0_scores),candidate_value),
        "score_dot_rounding_through_softmax":context64(softmax64(actual_taps[6].reshape(16,tokens,tokens)),candidate_value),
        "softmax_local":context64(actual_taps[7].reshape(16,tokens,tokens),candidate_value),
        "value_branch_propagated":context64(actual_taps[7].reshape(16,tokens,tokens),actual_value),
        "context_matmul_local":actual_taps[8]}
    previous_context = candidate_taps[8]
    attention_components = {}
    for name,values in context_stages.items():
        attention_components[name] = (values-previous_context)@projection_weight
        previous_context = values
    attention_closure = float(np.max(np.abs(sum(attention_components.values())-block0_terms["attention_context_propagated"])))
    if attention_closure > 1e-12:
        raise ValueError("Block-0 attention decomposition does not close")
    def replay_silu_tail(values):
        multiplied = half_round(values*actual_taps[13])
        projected_down = half_round(multiplied@down_weight+down_bias)
        return half_round(actual_taps[10]+projected_down)
    replay_input = replay_silu_tail(actual_taps[14])
    intervention_inputs = {
        "observed_silu_tail_replay":replay_input,
        "single_round_silu_tail_replay":replay_silu_tail(single_round_silu),
        "late_down_residual_block0":half_round(actual_taps[10]+down_after_gated),
        "unrounded_final_residual":actual_taps[10]+actual_taps[16]}
    intervention_references = {"rounded_ideal_block0":ideal}
    interventions = {}
    for name,inputs in intervention_inputs.items():
        intervention_references[name] = conditional_reference(inputs.astype(np.float32))[1]
    for name,reference_values in intervention_references.items():
        inputs = ideal_input if name == "rounded_ideal_block0" else intervention_inputs[name]
        delta = reference_values-observed
        frozen_local = actual+delta
        selected_taps = (2,6,17)
        interventions[name] = {
            "input_sha256_fp32":sha(inputs.astype("<f4").tobytes()),
            "input_changed_elements":int(np.count_nonzero(inputs != actual_taps[17])),
            "block0_vs_original":stats(inputs,block0_tap(block0_original,17)),
            "block1_taps":{block0_metadata["tap_order"][tap]:{
                "conditional_vs_original":stats(reference_values[offsets[tap]:offsets[tap+1]],base_original[offsets[tap]:offsets[tap+1]]),
                "delta_vs_observed_input":component_summary(delta[offsets[tap]:offsets[tap+1]]),
                "frozen_local_error_estimate_vs_original":stats(frozen_local[offsets[tap]:offsets[tap+1]],base_original[offsets[tap]:offsets[tap+1]])}
                for tap in selected_taps},
            "original_failures":{tap_name:[{"index":row["index"],
                "conditional_delta":float(delta[offsets[tap]+row["index"]]),
                "frozen_local_error_estimate":float(frozen_local[offsets[tap]+row["index"]]-row["original"])}
                for row in failures[tap_name]] for tap,tap_name in ((2,"q_norm"),(6,"scores"),(17,"block_output"))}}
    silu_paired_delta = intervention_references["single_round_silu_tail_replay"]-intervention_references["observed_silu_tail_replay"]
    silu_paired = {tap_name:{"delta":component_summary(silu_paired_delta[offsets[tap]:offsets[tap+1]]),
                           "original_failures":[{"index":row["index"],"delta":float(silu_paired_delta[offsets[tap]+row["index"]])} for row in failures[tap_name]]}
                   for tap,tap_name in ((2,"q_norm"),(6,"scores"),(17,"block_output"))}
    block0_analysis = {"oracle_fixture_byte_exact":True,"closure_max_abs":block0_closure,
                       "late_down_residual_reference":block0_late_reference,
                       "block1_control_comparison":block0_chain_comparison,
                       "taps":block0_taps,"output_components":block0_components,"rounding_simulations":block0_rounding,
                       "silu_rounding":{"single_round":rounding_stats(single_round_silu,actual_taps[14]),
                                        "staged_half":rounding_stats(staged_silu,actual_taps[14]),
                                        "simulations_by_gate_sign":silu_by_sign,
                                        "output_components":{name:component_summary(values) for name,values in silu_components.items()},
                                        "closure_max_abs":silu_closure},
                       "attention_context_components":{"output_components":{name:component_summary(values) for name,values in attention_components.items()},
                                                       "closure_max_abs":attention_closure},
                       "block1_input_interventions":interventions,"silu_paired_intervention":silu_paired,
                       "intervention_caveat":"Offline conditional oracle only. Frozen local error estimates assume unchanged Block-1 execution error, not hardware predictions or acceptance gates. SiLU replay rounds multiply/down/add separately; compare its control against capture before attributing changes.",
                       "largest_normalized_output_errors":block0_largest,
                       "reference_reconstruction":{"projection":stats(candidate_projection_exact,candidate_taps[9]),
                                                   "mlp":stats(candidate_down_exact,candidate_taps[16])},
                       "method":"Ordered FP64 counterfactual differences at observed boundaries; nonlinear interactions depend on ordering, not independent causal percentages"}
    constant_cursor = weights_start+2048+6291456+6144+256+tokens*512
    failure_projection_weight,failure_projection_bias = constant_half((1024,1024)),constant_half((1024,))
    failure_norm_weight = constant_half((1024,))
    failure_gate_weight,failure_gate_bias = constant_half((1024,4096)),constant_half((4096,))
    failure_up_weight,failure_up_bias = constant_half((1024,4096)),constant_half((4096,))
    failure_down_weight,failure_down_bias = constant_half((4096,1024)),constant_half((1024,))
    if constant_cursor != weights_start+constant_bytes:
        raise ValueError("Block-1 failure constant layout mismatch")
    attention_sum = actual[offsets[8]:offsets[9]].reshape(tokens,1024)@failure_projection_weight+failure_projection_bias+actual_input
    attention_rounded = attention_sum.astype(np.float16).astype(np.float64).reshape(-1)
    attention_observed = actual[offsets[10]:offsets[11]]
    attention_control = baseline_residual if attention_native else attention_observed
    attention_reference = {"native_path_enabled":attention_native,
        "captured_vs_single_round":rounding_stats(attention_sum,attention_observed),
        "control_vs_single_round":rounding_stats(attention_sum,attention_control),
        "captured_vs_original":stats(attention_observed,base_original[offsets[10]:offsets[11]]),
        "control_vs_original":stats(attention_control,base_original[offsets[10]:offsets[11]]),
        "reference_vs_original":stats(attention_rounded,base_original[offsets[10]:offsets[11]]),
        "changed_from_control":int(np.count_nonzero(attention_observed != attention_control)),
        "interpretation":"FP64 projection plus captured incoming residual, then one nearest-even FP16 rounding; native and control outputs compared separately"}
    late_down = actual[offsets[15]:offsets[16]].reshape(tokens,4096)@failure_down_weight+failure_down_bias
    late_sum = actual[offsets[10]:offsets[11]].reshape(tokens,1024)+late_down
    late_rounded = late_sum.astype(np.float16).astype(np.float64).reshape(-1)
    late_reference = {"vs_original":stats(late_rounded,base_original[offsets[17]:]),
                      "vs_candidate":stats(late_rounded,base_candidate[offsets[17]:]),
                      "captured_vs_single_round":rounding_stats(late_sum,actual[offsets[17]:]),
                      "native_path_enabled":late_native,
                      "interpretation":"FP64 from observed gated/residual tensors and fixture weights, one final nearest-even FP16 rounding; offline counterfactual unless the native late-rounding path is enabled"}
    original_output = base_original[offsets[17]:]
    old_output = actual[offsets[17]:]
    control_output = baseline_output if late_native else old_output
    late_reference["control_vs_original"] = stats(control_output,original_output)
    late_reference["changed_from_control"] = int(np.count_nonzero(old_output != control_output))
    late_indices = np.flatnonzero((np.abs(late_rounded-original_output) > 0.003+0.005*np.abs(original_output)) |
                                 (np.abs(old_output-original_output) > 0.003+0.005*np.abs(original_output)) |
                                 (np.abs(control_output-original_output) > 0.003+0.005*np.abs(original_output)))
    late_reference["failure_union"] = [{"index":int(index),"original":float(original_output[index]),
        "allowed":float(0.003+0.005*abs(original_output[index])),"captured":float(old_output[index]),"control":float(control_output[index]),
        "late_rounded":float(late_rounded[index]),"late_error":float(late_rounded[index]-original_output[index])} for index in late_indices]
    for row in failures["block_output"]:
        token_index,channel = divmod(row["index"],1024)
        local_taps = [actual[offsets[tap]:offsets[tap+1]].reshape(tokens,-1)[token_index] for tap in range(18)]
        matched_taps = [observed[offsets[tap]:offsets[tap+1]].reshape(tokens,-1)[token_index] for tap in range(18)]
        def failure_normalized(values):
            return values/np.sqrt(np.mean(values**2)+epsilon)*failure_norm_weight
        def failure_down(gate_values,up_values):
            return float((activated_gate(gate_values)*up_values)@failure_down_weight[:,channel]+failure_down_bias[channel])
        def failure_mlp(values):
            return failure_down(values@failure_gate_weight+failure_gate_bias,values@failure_up_weight+failure_up_bias)
        reference_down = failure_mlp(failure_normalized(matched_taps[10]))
        if not np.isclose(reference_down,matched_taps[16][channel],atol=1e-5,rtol=1e-5):
            raise ValueError("Block-1 failure FP64 MLP differs from matched oracle")
        stages = {
            "mlp_reference_rounding":reference_down,
            "mlp_residual_input_propagated":failure_mlp(failure_normalized(local_taps[10])),
            "mlp_norm2_local_propagated":failure_mlp(local_taps[11]),
            "mlp_gate_projection_local_propagated":failure_down(local_taps[12],local_taps[11]@failure_up_weight+failure_up_bias),
            "mlp_up_projection_local_propagated":failure_down(local_taps[12],local_taps[13]),
            "mlp_silu_local_propagated":float((local_taps[14]*local_taps[13])@failure_down_weight[:,channel]+failure_down_bias[channel]),
            "mlp_gated_multiply_local_propagated":float(local_taps[15]@failure_down_weight[:,channel]+failure_down_bias[channel]),
            "mlp_down_projection_local":float(local_taps[16][channel])}
        local_components = {
            "residual_operand_direct":float(local_taps[10][channel]-matched_taps[10][channel]),
            "reference_output_add":float(matched_taps[10][channel]+matched_taps[16][channel]-matched_taps[17][channel]),
            "late_output_vs_separate_taps" if late_native else "final_output_add":float(local_taps[17][channel]-local_taps[10][channel]-local_taps[16][channel])}
        previous = float(matched_taps[16][channel])
        for name,value in stages.items():
            local_components[name] = value-previous
            previous = value
        local_closure = abs(sum(local_components.values())-row["block1_local_execution"])
        if not all(np.isfinite(value) for value in local_components.values()) or local_closure > 1e-12:
            raise ValueError("Block-1 failure local decomposition does not close")
        row["token"] = token_index
        row["channel"] = channel
        row["tolerance_fraction"] = abs(row["actual"]-row["original"])/row["allowed_error"]
        row["block1_local_components"] = local_components
        row["block1_local_closure_max_abs"] = local_closure
        exact_down = stages["mlp_gated_multiply_local_propagated"]
        rounded_down = np.float16(exact_down)
        row["down_projection_rounding"] = {
            "fp64_from_observed_gated":exact_down,"nearest_fp16":float(rounded_down),
            "actual":float(local_taps[16][channel]),
            **rounding_stats(exact_down,local_taps[16][channel])}
        row["first_residual_add_rounding"] = rounding_stats(
            float(actual_input[token_index,channel])+local_taps[9][channel],local_taps[10][channel])
        row["first_residual_comparison_is_separate_operation"] = not attention_native
        row["block1_local_method"] = "Ordered FP64 MLP counterfactuals from matched oracle and observed taps; signed correlated contributions, not independent percentages"
    result = {"schema_version":1,"run":run,"fixture_sha256":sha(fixture),"analysis_sha256":sha(Path(__file__).read_bytes()),
              "late_down_residual_reference":late_reference,
              "attention_residual_reference":attention_reference,
              "block0_input_local_error_change":block0_local_error_change,
              "block0_output_analysis":block0_analysis,
              "handoff_bit_exact":True,"isolation_comparison":isolation,"closure_max_abs":closure,"taps":taps,"failures":failures,
              "qk_vs_fp64_observed_vectors":qk,"last_add_vs_fp64_observed_operands":residual,
              "last_add_comparison_is_separate_operation":not late_native,
              "local_score_stages":score_summary,"local_score_closure":score_closure,
              "rope_rounding_simulations":rope_simulations,"scores_with_fp32_rope_then_half_counterfactual":rope_counterfactual,
              "upstream_rounding_simulations":upstream,"upstream_score_counterfactuals":upstream_score_counterfactuals,
              "local_score_method":"Telescoping FP64 counterfactuals with observed QKV, observed Q/K norms and observed RoPE; not independent causal percentages or undocumented kernel precision claims",
              "hardware_pass":run["exit_code"] == 0,"interpretation":"Conditional offline oracles, not runtime correction or a hardware pass"}
    (build/"chain-boundary-analysis.json").write_text(json.dumps(result,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS chain boundary analysis; handoff bit exact; closure",closure,"QK",qk,"last add",residual,flush=True)
    print("Local score stages",json.dumps(score_summary),flush=True)
    print("RoPE rounding simulations",json.dumps(rope_simulations),"counterfactual scores",json.dumps(rope_counterfactual),flush=True)
    print("Residual failures",json.dumps(failures["block_output"]),flush=True)
    print("Late down/residual reference",json.dumps(late_reference),flush=True)
    print("Attention residual reference",json.dumps(attention_reference),flush=True)
    print("Upstream rounding",json.dumps(upstream),"counterfactual scores",json.dumps(upstream_score_counterfactuals),flush=True)
    print("Query norm failures",json.dumps(failures["q_norm"]),flush=True)
    print("Block 0 output",json.dumps(block0_taps["block_output"]),"closure",block0_closure,flush=True)
    print("Block 0 components",json.dumps(block0_components),flush=True)
    print("Block 0 rounding",json.dumps(block0_rounding),flush=True)
    print("Block 0 largest normalized errors",json.dumps(block0_largest[:3]),flush=True)
    print("Block 0 SiLU rounding",json.dumps(block0_analysis["silu_rounding"]),flush=True)
    print("Block 0 attention components",json.dumps(block0_analysis["attention_context_components"]),flush=True)
    for name,intervention in interventions.items():
        print("Block 0 intervention",name,"changed inputs",intervention["input_changed_elements"],"output",json.dumps(intervention["block0_vs_original"]),flush=True)
        for tap_name,metrics in intervention["block1_taps"].items():
            print("  Block 1",tap_name,json.dumps(metrics),flush=True)
        print("  Residual failure deltas",json.dumps(intervention["original_failures"]["block_output"]),flush=True)
    print("SiLU paired intervention",json.dumps(silu_paired),flush=True)
    if block0_late_native:
        print("Block 1 input-dependent local error",json.dumps(block0_local_error_change),flush=True)
        print("Block 0 late residual reference",json.dumps(block0_late_reference),flush=True)
        for tap_name in ("scores","block_output"):
            print("Block 0 change propagated",tap_name,json.dumps(block0_chain_comparison[tap_name]),flush=True)
        print("PASS Block 0 isolation: input, through-down taps and all internals bit exact; new handoff bit exact; Block 1 configuration unchanged",flush=True)
    elif isolation is not None:
        print("PASS isolation: Block 0 all taps and Block 1 input/norm1/QKV/Q/K norms bit exact against baseline",flush=True)
        if late_native:
            print("PASS attention residual isolation: Block 1 through projection and softmax internals bit exact" if attention_native else
                  "PASS late residual isolation: Block 1 through down and all internal captures bit exact",flush=True)


def analyze_internal_tensors(output, build, compare_build):
    import numpy as np
    if compare_build is None:
        raise ValueError("Internal analysis requires --chain-compare-build")
    fixture = (output/"htp-fixtures.got").read_bytes()
    manifest = read_json(output/"manifest.json")
    if sha(fixture) != manifest["sha256"] or struct.unpack_from("<I",fixture,12)[0] != 9 or hashlib.sha256(fixture[:96]+fixture[128:]).digest() != fixture[96:128]:
        raise ValueError("Internal analysis fixture identity mismatch")
    tokens = struct.unpack_from("<I",fixture,168)[0]
    if tokens not in (64,128) or struct.unpack_from("<I",fixture,160)[0] != 2:
        raise ValueError("Invalid internal analysis bucket/count")
    sizes = [16*tokens*tokens if tap in (6,7) else tokens*(3072 if tap == 1 else 4096 if 12 <= tap <= 15 else 1024) for tap in range(18)]
    offsets = np.cumsum([0]+sizes).tolist()
    internal_names = ("softmax_max","softmax_shift","softmax_exp","softmax_sum","silu_decay","silu_divisor","silu_numerator","silu_factor")
    internal_shapes = [(16,tokens,1),(16,tokens,tokens),(16,tokens,tokens),(16,tokens,1)]+[(tokens,4096)]*4
    residual_names = ("silu_initial_quotient","silu_correction_residual")
    cursor,reference_sets = 164,[]
    for block_index in range(2):
        input_bytes = tokens*2048 if block_index == 0 else 0
        constant_bytes = 33585408+tokens*512
        if struct.unpack_from("<7I",fixture,cursor) != (10+block_index,tokens,1024,1024,input_bytes,constant_bytes,offsets[-1]*8):
            raise ValueError("Invalid internal analysis record")
        reference_sets.append(np.frombuffer(fixture,dtype="<f4",count=offsets[-1]*2,offset=cursor+28+input_bytes+constant_bytes).reshape(2,-1).astype(np.float64))
        cursor += 28+input_bytes+constant_bytes+offsets[-1]*8
    if cursor != len(fixture):
        raise ValueError("Invalid internal analysis fixture length")
    runs,captures = [],[]
    for directory in (build,compare_build):
        run = json.loads((directory/"htp-probe.json").read_text(encoding="utf-8-sig"))
        capture_directory = Path(run["capture_directory"])
        if run["fixtures_sha256"] != sha(fixture) or sha((directory/"ocr-htp-test.exe").read_bytes()) != run["executable_sha256"] or capture_directory.resolve().parent != directory.resolve():
            raise ValueError("Internal analysis run identity mismatch")
        captured = {}
        for block_index in range(2):
            counts = {"input":tokens*1024,"taps":offsets[-1]}
            if directory == build or run.get("capture_internals"):
                counts["internals"] = sum(int(np.prod(shape)) for shape in internal_shapes)
                if run.get("matrix_residual") and block_index == 1:
                    counts["internals"] += 2*tokens*4096
            for suffix,count in counts.items():
                name = f"{block_index}.{suffix}.f16"
                raw = (capture_directory/name).read_bytes()
                if len(raw) != count*2 or sha(raw) != run["capture_sha256"][name]:
                    raise ValueError("Internal capture identity/length mismatch: "+name)
                values = np.frombuffer(raw,dtype="<f2")
                if not np.isfinite(values).all():
                    raise ValueError("Nonfinite internal analysis capture")
                captured[name] = values
        if captured["0.input.f16"].tobytes() != fixture[192:192+tokens*2048] or captured["0.taps.f16"][offsets[17]:].tobytes() != captured["1.input.f16"].tobytes():
            raise ValueError("Internal analysis input/handoff mismatch")
        runs.append(run)
        captures.append(captured)
    refined = bool(runs[0].get("refine_divide_block1"))
    silu_only = bool(runs[0].get("refine_silu_only"))
    matrix_residual = bool(runs[0].get("matrix_residual"))
    if matrix_residual and not (refined and silu_only):
        raise ValueError("Invalid matrix residual configuration")
    if silu_only and not refined:
        raise ValueError("Invalid SiLU-only correction configuration")
    if not runs[0].get("capture_internals") or runs[1].get("refine_divide_block1") or bool(runs[1].get("capture_internals")) != refined:
        raise ValueError("Internal analysis needs an unrefined matching control")
    for field in ("runtime_sha256","rope_implementation","rope_block1_only"):
        if runs[0].get(field) != runs[1].get(field):
            raise ValueError("Internal analysis control configuration mismatch: "+field)
    for name,values in captures[1].items():
        checked_elements = len(values)
        if refined and name == "1.taps.f16":
            checked_elements = offsets[14] if silu_only else offsets[7]
        elif refined and name == "1.internals.f16":
            checked_elements = sum(int(np.prod(shape)) for shape in internal_shapes[:7 if silu_only else 4])
        if values[:checked_elements].tobytes() != captures[0][name][:checked_elements].tobytes():
            raise ValueError("Instrumentation changed an existing tensor: "+name)
    def half_round(values):
        return np.asarray(values,dtype=np.float16).astype(np.float64)
    def half_away(values):
        rounded = np.asarray(values,dtype=np.float16)
        nearest = rounded.astype(np.float64)
        lower = np.where(nearest <= values,nearest,np.nextafter(rounded,np.full_like(rounded,-np.inf)).astype(np.float64))
        upper = np.where(nearest >= values,nearest,np.nextafter(rounded,np.full_like(rounded,np.inf)).astype(np.float64))
        midpoint = (lower != upper) & (values == (lower+upper)*0.5)
        return np.where(midpoint,np.where(values >= 0,upper,lower),nearest)
    def metrics(observed,expected):
        error = observed-expected
        if not np.isfinite(error).all():
            raise ValueError("Nonfinite internal reference")
        rounded = np.asarray(expected,dtype=np.float16)
        captured = np.asarray(observed,dtype=np.float16)
        nearest = rounded.astype(np.float64)
        lower = np.where(nearest <= expected,nearest,np.nextafter(rounded,np.full_like(rounded,-np.inf)).astype(np.float64))
        upper = np.where(nearest >= expected,nearest,np.nextafter(rounded,np.full_like(rounded,np.inf)).astype(np.float64))
        midpoint = (lower != upper) & (expected == (lower+upper)*0.5)
        away_ties = np.where(midpoint,np.where(expected >= 0,upper,lower),nearest).astype(np.float16)
        toward_zero = np.where(expected >= 0,lower,upper).astype(np.float16)
        nearest_matches = rounded.view(np.uint16) == captured.view(np.uint16)
        return {"elements":int(error.size),"bit_equal_single_round":int(np.count_nonzero(rounded.view(np.uint16) == captured.view(np.uint16))),
            "exact_midpoints":int(np.count_nonzero(midpoint)),
            "nonnearest_at_midpoints":int(np.count_nonzero(midpoint & ~nearest_matches)),
            "bit_equal_nearest_ties_away":int(np.count_nonzero(away_ties.view(np.uint16) == captured.view(np.uint16))),
            "bit_equal_toward_zero":int(np.count_nonzero(toward_zero.view(np.uint16) == captured.view(np.uint16))),
                "outside_adjacent_fp16_values":int(np.count_nonzero((observed < lower) | (observed > upper))),
                "below_exact":int(np.count_nonzero(observed < expected)),"above_exact":int(np.count_nonzero(observed > expected)),
                "rmse":float(np.sqrt(np.mean(error**2))),"mean_signed_error":float(np.mean(error)),
                "max_abs":float(np.max(np.abs(error))),"max_abs_beyond_single_round":float(np.max(np.abs(observed-rounded.astype(np.float64))))}
    blocks = []
    for block_index in range(2):
        taps = captures[0][f"{block_index}.taps.f16"].astype(np.float64)
        raw = captures[0][f"{block_index}.internals.f16"].astype(np.float64)
        internals,cursor = {},0
        block_names = internal_names+residual_names if matrix_residual and block_index == 1 else internal_names
        block_shapes = internal_shapes+[(tokens,4096)]*2 if matrix_residual and block_index == 1 else internal_shapes
        for name,shape in zip(block_names,block_shapes):
            count = int(np.prod(shape))
            internals[name] = raw[cursor:cursor+count].reshape(shape)
            cursor += count
        scores = taps[offsets[6]:offsets[7]].reshape(16,tokens,tokens)
        probabilities = taps[offsets[7]:offsets[8]].reshape(16,tokens,tokens)
        gate = taps[offsets[12]:offsets[13]].reshape(tokens,4096)
        silu = taps[offsets[14]:offsets[15]].reshape(tokens,4096)
        maximum,shifted,exponential,denominator,decay,divisor,numerator,factor = [internals[name] for name in internal_names]
        if np.any(denominator <= 0) or np.any(divisor <= 0):
            raise ValueError("Invalid internal divisor")
        operations = {"softmax_max":metrics(maximum,np.max(scores,axis=-1,keepdims=True)),
                      "softmax_subtract":metrics(shifted,scores-maximum),
                      "softmax_exp":metrics(exponential,np.exp(shifted)),
                      "softmax_sum":metrics(denominator,np.sum(exponential,axis=-1,keepdims=True)),
                      "softmax_divide":metrics(probabilities,exponential/denominator),
                      "silu_decay_exp":metrics(decay,np.exp(-np.abs(gate))),
                      "silu_divisor_add":metrics(divisor,1+decay),
                      "silu_numerator_exp":metrics(numerator,np.exp(np.minimum(gate,0))),
                      "silu_factor_divide":metrics(factor,numerator/divisor),
                      "silu_final_multiply":metrics(silu,gate*factor),
                      "gated_multiply":metrics(taps[offsets[15]:offsets[16]].reshape(tokens,4096),silu*taps[offsets[13]:offsets[14]].reshape(tokens,4096)),
                      "first_residual_add":metrics(taps[offsets[10]:offsets[11]],captures[0][f"{block_index}.input.f16"].astype(np.float64)+taps[offsets[9]:offsets[10]]),
                      "final_residual_add":metrics(taps[offsets[17]:offsets[18]],taps[offsets[10]:offsets[11]]+taps[offsets[16]:offsets[17]])}
        reciprocal_models = {"softmax_divide":metrics(probabilities,half_round(exponential*half_round(1/denominator))),
                             "silu_factor_divide":metrics(factor,half_round(numerator*half_round(1/divisor)))}
        exact_exp = np.exp(scores-np.max(scores,axis=-1,keepdims=True))
        ideal_softmax = exact_exp/np.sum(exact_exp,axis=-1,keepdims=True)
        shifted_exp = np.exp(shifted)
        softmax_stages = {"shift_rounding":shifted_exp/np.sum(shifted_exp,axis=-1,keepdims=True),
                          "exp_local":exponential/np.sum(exponential,axis=-1,keepdims=True),
                          "sum_local":exponential/denominator,"divide_local":probabilities}
        ideal_numerator = np.exp(np.minimum(gate,0))
        ideal_silu = gate*ideal_numerator/(1+np.exp(-np.abs(gate)))
        silu_stages = {"decay_exp_local":gate*ideal_numerator/(1+decay),
                       "numerator_exp_local":gate*numerator/(1+decay),
                       "divisor_add_local":gate*numerator/divisor,
                       "factor_divide_local":gate*factor,"final_multiply_local":silu}
        decompositions = {}
        for name,ideal,stages in (("softmax",ideal_softmax,softmax_stages),("silu",ideal_silu,silu_stages)):
            previous = ideal
            terms = {}
            for stage,values in stages.items():
                terms[stage] = values-previous
                previous = values
            closure = float(np.max(np.abs(sum(terms.values())-(previous-ideal))))
            if closure > 1e-12:
                raise ValueError("Internal error decomposition does not close")
            decompositions[name] = {"closure_max_abs":closure,"components":{stage:{"rmse":float(np.sqrt(np.mean(values**2))),"max_abs":float(np.max(np.abs(values)))} for stage,values in terms.items()}}
        comparison = None
        if refined:
            control_taps = captures[1][f"{block_index}.taps.f16"].astype(np.float64)
            control_raw = captures[1][f"{block_index}.internals.f16"].astype(np.float64)
            control_values,cursor = [],0
            for shape in internal_shapes:
                count = int(np.prod(shape))
                control_values.append(control_raw[cursor:cursor+count].reshape(shape))
                cursor += count
            comparison = {
                "control_softmax_divide":metrics(control_taps[offsets[7]:offsets[8]].reshape(16,tokens,tokens),control_values[2]/control_values[3]),
                "control_silu_factor_divide":metrics(control_values[7],control_values[6]/control_values[5]),
                "silu_operands_bit_exact":bool(np.array_equal(numerator,control_values[6]) and np.array_equal(divisor,control_values[5])),
                "tap_changed_elements":{manifest["blocks"][block_index]["tap_order"][tap]:int(np.count_nonzero(
                    captures[0][f"{block_index}.taps.f16"][offsets[tap]:offsets[tap+1]].view(np.uint16) !=
                    captures[1][f"{block_index}.taps.f16"][offsets[tap]:offsets[tap+1]].view(np.uint16))) for tap in range(18)}}
            residual_models = {}
            pairs = [("softmax",exponential,denominator,control_taps[offsets[7]:offsets[8]].reshape(16,tokens,tokens),probabilities)]
            if comparison["silu_operands_bit_exact"]:
                pairs.append(("silu",numerator,divisor,control_values[7],factor))
            for name,dividend,divisor_values,initial,observed_quotient in pairs:
                exact_residual = dividend-initial*divisor_values
                rounded_residual = half_away(dividend-half_away(initial*divisor_values))
                modeled = half_away(initial+half_round(rounded_residual/divisor_values))
                residual_models[name] = {"applied_in_capture":not matrix_residual and block_index == 1 and (name == "silu" or not silu_only),
                                         "nonzero_exact_residual_lost":int(np.count_nonzero((rounded_residual == 0) & (exact_residual != 0))),
                                         "modeled_result":metrics(observed_quotient,modeled),
                                         "reference_with_exact_residual":metrics(half_away(initial+half_round(exact_residual/divisor_values)),dividend/divisor_values),
                                         "interpretation":"Offline model using control quotient, ties-away product/residual/add and ideal single-rounded correction Divide; not a captured residual or an implemented exact-residual path"}
            comparison["residual_rounding_models"] = residual_models
            if matrix_residual and block_index == 1:
                initial = internals["silu_initial_quotient"]
                residual = internals["silu_correction_residual"]
                if initial.astype(np.float16).tobytes() != control_values[7].astype(np.float16).tobytes():
                    raise ValueError("Matrix residual instrumentation changed the initial quotient")
                exact_residual = numerator-initial*divisor
                separate_residual = half_away(numerator-half_away(initial*divisor))
                comparison["matrix_residual"] = {
                    "initial_quotient_bit_exact":True,
                    "captured_residual":metrics(residual,exact_residual),
                    "separate_product_residual_model":metrics(separate_residual,exact_residual),
                    "nonzero_exact_residual_lost":int(np.count_nonzero((residual == 0) & (exact_residual != 0))),
                    "separate_product_nonzero_residual_lost":int(np.count_nonzero((separate_residual == 0) & (exact_residual != 0))),
                    "result_with_ideal_correction_divide":metrics(factor,half_away(initial+half_round(residual/divisor))),
                    "interpretation":"Captured HTP residual against FP64 from identical FP16 operands; separate-product and correction-Divide models are offline, not physical accumulator precision guarantees"}
        chain_taps = {}
        for tap,tap_name in enumerate(manifest["blocks"][block_index]["tap_order"]):
            section = slice(offsets[tap],offsets[tap+1])
            original,candidate = reference_sets[block_index][:,section]
            values = taps[section]
            summary = {}
            for scope,reference_values in (("original",original),("candidate",candidate)):
                error = values-reference_values
                summary[scope] = {"out_of_tolerance":int(np.count_nonzero(np.abs(error) > 0.003+0.005*np.abs(reference_values))),
                                  "rmse":float(np.sqrt(np.mean(error**2))),"max_abs":float(np.max(np.abs(error)))}
            if tap == 17:
                previous_values = captures[1][f"{block_index}.taps.f16"][section].astype(np.float64)
                indices = np.flatnonzero((np.abs(values-original) > 0.003+0.005*np.abs(original)) |
                                         (np.abs(previous_values-original) > 0.003+0.005*np.abs(original)))
                summary["failure_union"] = [{"index":int(index),"original":float(original[index]),"allowed":float(0.003+0.005*abs(original[index])),
                                              "control":float(previous_values[index]),"actual":float(values[index]),
                                              "actual_error":float(values[index]-original[index])} for index in indices]
            chain_taps[tap_name] = summary
        block = {"block_index":block_index,"internal_order":list(block_names),"internal_shapes":block_shapes,"operations":operations,"half_reciprocal_models":reciprocal_models,"decompositions":decompositions,"correction_comparison":comparison,"chain_taps":chain_taps}
        blocks.append(block)
        print("Block",block_index,"internal operation comparisons",flush=True)
        for name,result in operations.items():
            print(name,json.dumps(result),flush=True)
        print("Reciprocal models",json.dumps(reciprocal_models),flush=True)
        print("Internal decompositions",json.dumps(decompositions),flush=True)
        if comparison is not None:
            print("Correction comparison",json.dumps(comparison),flush=True)
        print("Chain scores",json.dumps(chain_taps["scores"]),"block output",json.dumps(chain_taps["block_output"]),flush=True)
    report = {"schema_version":1,"run":runs[0],"control":runs[1],"fixture_sha256":sha(fixture),
              "analysis_sha256":sha(Path(__file__).read_bytes()),"all_existing_tensors_bit_exact":not refined,
              "block0_and_block1_pre_division_bit_exact":True,
              "silu_only_operands_bit_exact":silu_only,
              "internal_order":list(internal_names),"internal_shapes":internal_shapes,"blocks":blocks,
              "hardware_pass":runs[0]["exit_code"] == 0,
              "interpretation":"Observed operands, FP64 local references, nearest-FP16 comparisons; ordered correlated error components, not undocumented kernel guarantees or chain acceptance"}
    (build/"internal-analysis.json").write_text(json.dumps(report,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS internal analysis: "+("Block 0 and Block 1 through softmax operands bit exact" if refined else "both inputs and all original taps bit exact against uninstrumented control"),flush=True)


def analyze_chain_scores(output, build):
    import numpy as np
    fixture = (output/"htp-fixtures.got").read_bytes()
    manifest = read_json(output/"manifest.json")
    report = json.loads((build/"htp-probe.json").read_text(encoding="utf-8-sig"))
    if sha(fixture) != manifest["sha256"] or sha(fixture) != report["fixtures_sha256"] or sha((build/"ocr-htp-test.exe").read_bytes()) != report["executable_sha256"]:
        raise ValueError("Chain analysis identity mismatch")
    if struct.unpack_from("<I",fixture,12)[0] != 9 or hashlib.sha256(fixture[:96]+fixture[128:]).digest() != fixture[96:128]:
        raise ValueError("Chain envelope mismatch")
    raw = (build/"htp-probe.log").read_bytes()
    text = raw.decode("utf-16" if raw.startswith(b"\xff\xfe") else "utf-8-sig")
    pattern = (r"score_failure_index: 0x([0-9a-f]+)\s+score_actual_fp16_bits: 0x([0-9a-f]+)\s+"
               r"score_expected_fp32_bits: 0x([0-9a-f]+)\s+score_query_fp16_hex: ([0-9a-f]+)\s+score_key_fp16_hex: ([0-9a-f]+)")
    rows = []
    for index,actual,expected,query,key in re.findall(pattern,text):
        if len(query) != 256 or len(key) != 256:
            raise ValueError("Invalid Q/K diagnostic vector")
        vectors = [np.array([int(value[offset:offset+4],16) for offset in range(0,len(value),4)],dtype=np.uint16).view(np.float16).astype(np.float64) for value in (query,key)]
        actual_value = struct.unpack("<e",int(actual,16).to_bytes(2,"little"))[0]
        expected_value = struct.unpack("<f",int(expected,16).to_bytes(4,"little"))[0]
        dot = float(vectors[0]@vectors[1]*0.125)
        row = {"index":int(index,16),"total_error":actual_value-expected_value,
               "propagated_qk_error":dot-expected_value,"qk_execution_error":actual_value-dot}
        rows.append(row)
        print(row,flush=True)
    if not rows:
        raise ValueError("No failing score diagnostics to analyze")
    (build/"chain-score-analysis.json").write_text(json.dumps({"fixtures_sha256":sha(fixture),"run":report,"scores":rows},indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS chain score decomposition:",len(rows),"diagnostics; this does not make the hardware gate pass",flush=True)


def analyze_attention_failures(output, build):
    import numpy as np
    fixture = (output/"htp-fixtures.got").read_bytes()
    manifest = read_json(output/"manifest.json")
    run = json.loads((build/"htp-probe.json").read_text(encoding="utf-8-sig"))
    log_bytes = (build/"htp-probe.log").read_bytes()
    log = log_bytes.decode("utf-16" if log_bytes.startswith((b"\xff\xfe",b"\xfe\xff")) else "utf-8-sig")
    if len(fixture) != 15378880 or sha(fixture) != manifest["sha256"] or sha(fixture) != run["fixtures_sha256"]:
        raise ValueError("Attention fixture/run identity mismatch")
    if hashlib.sha256(fixture[:96]+fixture[128:]).digest() != fixture[96:128] or struct.unpack_from("<I",fixture,12)[0] != 7:
        raise ValueError("Attention envelope mismatch")
    if fixture[128:160].hex() != "a16eb0de98d199293371c560f95f83130d2a2c9612449df16839f08ff9498815":
        raise ValueError("Attention weight identity mismatch")
    if sha((build/"ocr-htp-test.exe").read_bytes()) != run["executable_sha256"]:
        raise ValueError("Attention executable identity mismatch")
    if struct.unpack_from("<8I",fixture,160) != (1,8,64,1024,1024,131072,8431872,6815744):
        raise ValueError("Attention fixture geometry mismatch")
    source = np.frombuffer(fixture,dtype="<f2",count=65536,offset=192).astype(np.float64)
    constants_offset = 192+131072
    proj_offset = constants_offset+6332672
    weights = np.frombuffer(fixture,dtype="<f2",count=1024*1024,offset=proj_offset).reshape(1024,1024)
    bias = np.frombuffer(fixture,dtype="<f2",count=1024,offset=proj_offset+2097152)
    references = np.frombuffer(fixture,dtype="<f4",count=1703936,offset=constants_offset+8431872).reshape(2,851968)
    captures = []
    for line in log.splitlines():
        match = re.fullmatch(r"(\w+): (?:0x)?([0-9a-f]+)",line.strip())
        if not match:
            continue
        name,value = match.groups()
        if name == "residual_failure_index":
            captures.append({name:int(value,16)})
        elif captures and (name.endswith("_bits") or name == "context_row_fp16_hex"):
            captures[-1][name] = value if name == "context_row_fp16_hex" else int(value,16)
    if len(captures) != 2 or len({item["residual_failure_index"] for item in captures}) != 2 or run["exit_code"] != 1:
        raise ValueError("Expected exactly two reproduced residual failures and nonzero hardware gate")
    failures = []
    for capture in captures:
        index = capture["residual_failure_index"]
        if not 0 <= index < 65536:
            raise ValueError("Residual index bounds")
        patch_index,channel = divmod(index,1024)
        def decoded(name,code):
            bits = capture[name]
            return struct.unpack("<"+code,bits.to_bytes(2 if code == "e" else 4,"little"))[0]
        actual_input = decoded("input_fp16_bits","e")
        actual_proj = decoded("projection_fp16_bits","e")
        actual_residual = decoded("residual_fp16_bits","e")
        original_proj = float(references[0,720896+index])
        original_residual = float(references[0,786432+index])
        candidate_proj = float(references[1,720896+index])
        candidate_residual = float(references[1,786432+index])
        for scope in ("original","candidate"):
            for name,offset in (("projection",720896),("residual",786432)):
                expected_bits = struct.unpack("<I",struct.pack("<f",references[int(scope == "candidate"),offset+index]))[0]
                if expected_bits != capture[scope+"_"+name+"_fp32_bits"]:
                    raise ValueError("Logged oracle does not match immutable fixture")
        if actual_input != source[index]:
            raise ValueError("Logged input differs from fixture")
        encoded_row = capture["context_row_fp16_hex"]
        if len(encoded_row) != 4096:
            raise ValueError("Context capture bounds")
        actual_context = np.array([int(encoded_row[start:start+4],16) for start in range(0,4096,4)],dtype=np.uint16).view(np.float16).astype(np.float64)
        candidate_context = references[1,655360+patch_index*1024:655360+(patch_index+1)*1024].astype(np.float64)
        column = weights[:,channel].astype(np.float64)
        exact_actual_projection = float(actual_context @ column+float(bias[channel]))
        exact_candidate_projection = float(candidate_context @ column+float(bias[channel]))
        original_input_reconstructed = original_residual-original_proj
        input_error = actual_input-original_input_reconstructed
        projection_conversion_error = candidate_proj-original_proj
        context_error = exact_actual_projection-exact_candidate_projection
        projection_execution_error = actual_proj-exact_actual_projection
        oracle_fp32_rounding = exact_candidate_projection-candidate_proj
        addition_error = actual_residual-(actual_input+actual_proj)
        residual_error = actual_residual-original_residual
        decomposition = input_error+projection_conversion_error+context_error+projection_execution_error+oracle_fp32_rounding+addition_error
        if abs(decomposition-residual_error) > 1e-12 or not np.isfinite(actual_context).all():
            raise ValueError("Residual error decomposition inconsistent")
        threshold = float(np.float32(0.003)+np.float32(0.005)*abs(np.float32(original_residual)))
        candidate_threshold = float(np.float32(0.003)+np.float32(0.005)*abs(np.float32(candidate_residual)))
        if abs(residual_error) <= threshold or abs(actual_residual-candidate_residual) > candidate_threshold:
            raise ValueError("Expected original-fail/candidate-pass behavior not reproduced")
        result = {"index":index,"patch":patch_index,"channel":channel,
                  "input_fp16":actual_input,"original_input_reconstructed":original_input_reconstructed,
                  "original_projection":original_proj,"candidate_projection":candidate_proj,"htp_projection":actual_proj,
                  "original_residual":original_residual,"candidate_residual":candidate_residual,"htp_residual":actual_residual,
                  "original_absolute_error":abs(residual_error),"original_threshold":threshold,"excess":abs(residual_error)-threshold,
                  "candidate_absolute_error":abs(actual_residual-candidate_residual),"candidate_threshold":candidate_threshold,
                  "cancellation_factor":(abs(original_input_reconstructed)+abs(original_proj))/abs(original_residual),
                  "signed_error_terms":{"input_conversion_with_original_add_roundoff":input_error,
                    "projection_input_weight_conversion":projection_conversion_error,"propagated_context_error":context_error,
                    "projection_execution_vs_fp64_dot":projection_execution_error,"fp32_oracle_dot_roundoff":oracle_fp32_rounding,
                    "final_addition":addition_error},
                  "fp16_addition_exact":addition_error == 0,
                  "counterfactual_error_original_input":abs(original_input_reconstructed+actual_proj-original_residual),
                  "counterfactual_error_exact_projection":abs(actual_input+exact_actual_projection-original_residual)}
        failures.append(result)
        print(json.dumps(result,allow_nan=False),flush=True)
    report = {"schema_version":1,"fixtures_sha256":sha(fixture),"log_sha256":sha(log_bytes),"run":run,
              "analysis_exporter_sha256":sha(Path(__file__).read_bytes()),"failures":failures,
              "limitations":["Original input reconstructed from FP32 residual minus projection; its term includes original addition roundoff.",
                             "Context term aggregates upstream HTP errors; does not identify an individual attention kernel.",
                             "FP32 tensor declarations do not prove physical FP32 execution on HTP.",
                             "One image/grid, attention branch only, no MLP or full-model quality acceptance."],
              "hardware_gate_passed":False}
    (build/"residual-analysis.json").write_text(json.dumps(report,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS residual investigation: two oracle-bound failures decomposed; hardware gate remains FAIL",flush=True)


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
    header[52:72] = bytes.fromhex(sources["preprocessor_config.json" if 3 <= kind <= 19 else "tokenizer.json"]["git_blob_sha1"])
    header[72:92] = bytes.fromhex(sources["config.json" if 3 <= kind <= 19 else "tokenizer_config.json"]["git_blob_sha1"])
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


@contextlib.contextmanager
def weight_source(model):
    catalog = verified_sources(model)
    record = next(item for item in catalog["files"] if item["name"] == "model.safetensors")
    with (model / record["name"]).open("rb") as stream:
        if os.fstat(stream.fileno()).st_size != record["size"] or hashlib.file_digest(stream,"sha256").hexdigest() != record["sha256"]:
            raise ValueError("Original weight identity mismatch")
        stream.seek(0)
        header_size = int.from_bytes(stream.read(8),"little")
        if not 2 <= header_size <= 16*1024*1024:
            raise ValueError("Weight header bounds")
        header = json.loads(stream.read(header_size))
        tensors = {name:value for name,value in header.items() if name != "__metadata__"}
        cursor = 0
        for name,tensor in sorted(tensors.items(),key=lambda item:item[1]["data_offsets"][0]):
            shape,offsets = tensor["shape"],tensor["data_offsets"]
            if tensor["dtype"] != "BF16" or not shape or any(type(size) is not int or size <= 0 for size in shape):
                raise ValueError("Weight geometry: " + name)
            if offsets != [cursor,cursor+math.prod(shape)*2]:
                raise ValueError("Weight coverage: " + name)
            cursor = offsets[1]
        if len(tensors) != 526 or cursor != record["size"]-8-header_size:
            raise ValueError("Weight inventory mismatch")
        yield stream,tensors,8+header_size,catalog


def precision_stats(words):
    import numpy as np
    source = (words.astype(np.uint32) << 16).view(np.float32)
    with np.errstate(over="ignore",invalid="ignore"):
        candidate = source.astype(np.float16).astype(np.float32)
    finite = np.isfinite(source)
    comparable = finite & np.isfinite(candidate)
    subnormal = finite & (candidate != 0) & (np.abs(candidate) < 2**-14)
    error = np.abs(candidate[comparable].astype(np.float64)-source[comparable])
    return {
        "elements":int(source.size), "source_nonfinite":int((~finite).sum()),
        "fp16_overflow":int((finite & ~np.isfinite(candidate)).sum()),
        "nonzero_to_zero":int((finite & (source != 0) & (candidate == 0)).sum()),
        "fp16_subnormal":int(subnormal.sum()),
        "changed_finite":int((candidate[comparable] != source[comparable]).sum()),
        "compared_elements":int(comparable.sum()),
        "max_source_magnitude":float(np.abs(source[finite]).max(initial=0)),
        "max_absolute_error":float(error.max(initial=0)),
        "max_error_if_subnormals_flushed":max(float(error.max(initial=0)),float(np.abs(source[subnormal]).max(initial=0))),
        "squared_error_sum":float(np.dot(error,error)),
    }


def merge_precision(total, current):
    for key,value in current.items():
        total[key] = max(total.get(key,0),value) if key.startswith("max_") else total.get(key,0)+value


def audit_precision(model, output):
    import numpy as np
    boundary = precision_stats(np.array([0x0000,0x3f80,0xbf80,0x4780,0x0001,0x3580,0x7f80,0x7fc0,0x3381],dtype=np.uint16))
    if (boundary["source_nonfinite"],boundary["fp16_overflow"],boundary["nonzero_to_zero"],boundary["fp16_subnormal"],boundary["changed_finite"]) != (2,1,1,2,2) or boundary["max_absolute_error"] != 2**-31 or boundary["max_error_if_subnormals_flushed"] != 2**-20:
        raise ValueError("BF16/FP16 boundary self-test failed")
    print("PASS precision boundary self-test",flush=True)
    totals,groups,results = {},{},{}
    with weight_source(model) as (stream,tensors,payload,catalog):
        print("PASS original checkpoint SHA-256",flush=True)
        for name,tensor in tensors.items():
            start,end = tensor["data_offsets"]
            stream.seek(payload+start)
            remaining,summary = end-start,{}
            while remaining:
                raw = stream.read(min(remaining,2*1024*1024))
                if not raw or len(raw)%2:
                    raise ValueError("Truncated weight tensor: " + name)
                merge_precision(summary,precision_stats(np.frombuffer(raw,dtype="<u2")))
                remaining -= len(raw)
            group = "unused_mtp_layer16" if name.startswith("model.language_model.layers.16.") else "vision" if name.startswith("model.visual.") else "text_and_head"
            merge_precision(totals,summary)
            merge_precision(groups.setdefault(group,{}),summary)
            results[name] = {"shape":tensor["shape"],**summary}
    report = {"schema_version":1,"source_revision":catalog["revision"],
              "source_sha256":next(item["sha256"] for item in catalog["files"] if item["name"] == "model.safetensors"),
              "exporter_sha256":sha(Path(__file__).read_bytes()),"numpy":np.__version__,
              "conversion":"IEEE FP16 round-to-nearest-even; subnormals preserved; no clamping",
              "error_scope":"finite source and finite candidate only; overflows counted separately",
              "totals":totals,"groups":groups,"tensors":results,
              "candidate_all_finite":not (totals["source_nonfinite"] or totals["fp16_overflow"]),
              "full_model_inference":False,"checkpoint_written":False}
    output.mkdir(parents=True,exist_ok=True)
    (output/"precision-audit.json").write_text(json.dumps(report,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    print("PASS original BF16 precision audit:",json.dumps(totals),flush=True)


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
    parser.add_argument("--export-htp", action="store_true")
    parser.add_argument("--audit-precision", action="store_true")
    parser.add_argument("--export-learned-htp", action="store_true")
    parser.add_argument("--export-vision-attention", action="store_true")
    parser.add_argument("--export-vision-block", action="store_true")
    parser.add_argument("--export-vision-encoder", action="store_true")
    parser.add_argument("--large-images", action="store_true")
    parser.add_argument("--export-text-decoder", action="store_true")
    parser.add_argument("--export-generation", action="store_true")
    parser.add_argument("--analyze-generation", action="store_true")
    parser.add_argument("--reference-generation", action="store_true")
    parser.add_argument("--test-png", action="store_true")
    parser.add_argument("--image-build", type=Path, default=ROOT/"experimental/snapdragon/build/ocr-image-files")
    parser.add_argument("--generation-build", type=Path, default=ROOT / "experimental/snapdragon/build/ocr-generate")
    parser.add_argument("--generation-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-generation-v2")
    parser.add_argument("--analyze-text-prefill", action="store_true")
    parser.add_argument("--text-build", type=Path, default=ROOT / "experimental/snapdragon/build/ocr-prefill")
    parser.add_argument("--text-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-text-v1")
    parser.add_argument("--analyze-vision-encoder", action="store_true")
    parser.add_argument("--vision-build", type=Path, default=ROOT / "experimental/snapdragon/build/ocr-vision")
    parser.add_argument("--vision-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-vision-v1")
    parser.add_argument("--analyze-chain-scores", action="store_true")
    parser.add_argument("--analyze-chain-boundary", action="store_true")
    parser.add_argument("--analyze-internal-tensors", action="store_true")
    parser.add_argument("--chain-build", type=Path, default=ROOT / "experimental/snapdragon/build/ocr-chain")
    parser.add_argument("--chain-compare-build", type=Path)
    parser.add_argument("--vision-block-count", type=int, choices=(1,2), default=1)
    parser.add_argument("--vision-block-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-block-v1")
    parser.add_argument("--attention-case", choices=("pattern","noise","text","receipt","german","all","examples"), default="pattern")
    parser.add_argument("--analyze-attention-failures", action="store_true")
    parser.add_argument("--attention-build", type=Path, default=ROOT / "experimental/snapdragon/build/ocr-attention")
    parser.add_argument("--vision-attention-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-attention-v1")
    parser.add_argument("--learned-htp-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-learned-htp-v1")
    parser.add_argument("--precision-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-precision-v1")
    parser.add_argument("--htp-output", type=Path, default=ROOT / "experimental/snapdragon/models/glm-ocr-htp-v1")
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
    elif args.analyze_attention_failures:
        analyze_attention_failures(args.vision_attention_output,args.attention_build)
    elif args.audit_precision:
        audit_precision(args.model_dir,args.precision_output)
    elif args.export_learned_htp:
        export_learned_htp(args.model_dir,args.learned_htp_output)
    elif args.analyze_chain_boundary:
        analyze_chain_boundary(args.model_dir,args.vision_block_output,args.chain_build,args.chain_compare_build)
    elif args.analyze_internal_tensors:
        analyze_internal_tensors(args.vision_block_output,args.chain_build,args.chain_compare_build)
    elif args.analyze_chain_scores:
        analyze_chain_scores(args.vision_block_output,args.chain_build)
    elif args.analyze_text_prefill:
        analyze_text_prefill(args.model_dir,args.text_output,args.text_build)
    elif args.test_png:
        test_png(args.image_build)
    elif args.reference_generation:
        reference_generation(args.model_dir,args.vision_output,args.generation_build)
    elif args.analyze_generation:
        analyze_generation(args.model_dir,args.generation_output,args.text_output,args.generation_build)
    elif args.export_generation:
        export_generation(args.model_dir,args.generation_output,args.output)
    elif args.export_text_decoder:
        export_text_decoder(args.model_dir,args.text_output,args.vision_build)
    elif args.analyze_vision_encoder:
        analyze_vision_encoder(args.vision_output,args.vision_build)
    elif args.export_vision_encoder:
        export_vision_encoder(args.model_dir,args.vision_output,args.large_images)
    elif args.export_vision_block:
        cases = ("pattern","noise","text") if args.attention_case == "all" else ("receipt","german") if args.attention_case == "examples" else (args.attention_case,)
        for case in cases:
            destination = args.vision_block_output if case == "pattern" else args.vision_block_output/case
            if args.vision_block_count == 2:
                export_vision_chain(args.model_dir,destination,case)
            else:
                export_vision_attention(args.model_dir,destination,case,full_block=True)
    elif args.export_vision_attention:
        if args.attention_case in ("all","examples"):
            cases = ("pattern","noise","text") if args.attention_case == "all" else ("receipt","german")
            for case in cases:
                export_vision_attention(args.model_dir,args.vision_attention_output if case == "pattern" else args.vision_attention_output/case,case)
        else:
            export_vision_attention(args.model_dir,args.vision_attention_output,args.attention_case)
    elif args.export_htp:
        export_htp(args.model_dir,args.htp_output)
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
        parser.error("select --inspect, --export-tokenizer, --wrapper-check, --inspect-images, --export-images, --export-positions, --export-htp, --audit-precision, --export-learned-htp, --export-vision-attention, --export-vision-block or --analyze-attention-failures")


if __name__ == "__main__":
    main()