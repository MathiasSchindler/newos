"""Pinned, development-only Transformers streaming reference and M0 traces."""

import argparse
import importlib.metadata
import inspect
import json
import os
from pathlib import Path
import platform
import re
import sys
import unicodedata

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, read_catalog, read_wav
from nemotron_m0 import safe_name, verify_checkpoint, verify_file, write_json


class TensorWriter:
    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.records = []

    def tensor(self, name, value):
        array = value.detach().cpu().contiguous().numpy()
        if array.dtype.kind == "f":
            import numpy as np
            if not np.isfinite(array).all():
                raise ValueError(f"Nonfinite reference tensor: {name}")
        array = array.astype(array.dtype.newbyteorder("<"), copy=False)
        path = self.root / (safe_name(name) + ".bin")
        if path.exists():
            raise ValueError(f"Duplicate tensor name: {name}")
        path.write_bytes(array.tobytes())
        record = {"name": name, "file": path.name, "dtype": array.dtype.str,
                  "shape": list(array.shape), "size": path.stat().st_size, "sha256": digest(path)}
        self.records.append(record)
        return record


class FeatureStream:
    """Bounded centered STFT with persistent preemphasis and explicit EOF."""

    def __init__(self, extractor, torch):
        self.extractor = extractor
        self.torch = torch
        self.fft = extractor.n_fft
        self.hop = extractor.hop_length
        self.buffer = torch.zeros(self.fft // 2, dtype=torch.float32)
        self.previous = 0.0
        self.total = 0
        self.next_frame = 0
        self.closed = False
        self.peak_samples = len(self.buffer)

    def feed(self, samples, eof=False):
        if self.closed:
            raise ValueError("Frontend already finalized")
        torch = self.torch
        raw = torch.tensor(samples, dtype=torch.float32)
        if not torch.isfinite(raw).all():
            raise ValueError("Nonfinite audio")
        if raw.numel():
            prior = torch.cat((raw.new_tensor([self.previous]), raw[:-1]))
            emphasized = raw - self.extractor.preemphasis * prior
            self.previous = float(raw[-1])
            self.buffer = torch.cat((self.buffer, emphasized))
            self.total += raw.numel()
        if eof:
            self.buffer = torch.cat((self.buffer, self.buffer.new_zeros(self.fft // 2)))
            self.closed = True
        self.peak_samples = max(self.peak_samples, self.buffer.numel())
        available = max(0, (self.buffer.numel() - self.fft) // self.hop + 1)
        count = min(available, max(0, self.total // self.hop - self.next_frame))
        if count == 0:
            return self.buffer.new_empty((1, 0, self.extractor.feature_size))
        length = self.fft + (count - 1) * self.hop
        result = self.extractor._torch_extract_fbank_features(self.buffer[None, :length], center=False)
        self.buffer = self.buffer[count * self.hop:].clone()
        self.next_frame += count
        return result


def features_for_audio(samples, extractor, torch, partition):
    frontend = FeatureStream(extractor, torch)
    frames = []
    offset = 0
    index = 0
    while offset < len(samples):
        size = partition[index % len(partition)]
        if type(size) is not int or size <= 0:
            raise ValueError("Invalid audio feed partition")
        end = min(len(samples), offset + size)
        frames.append(frontend.feed(samples[offset:end], eof=end == len(samples)))
        offset = end
        index += 1
    if not frames:
        raise ValueError("Empty audio fixture")
    return torch.cat(frames, dim=1), frontend.peak_samples


def chunk_features(features, torch, first=25, steady=32):
    if features.shape[1] == 0:
        raise ValueError("Audio is shorter than one valid feature frame")
    chunks = []
    cursor = 0
    while cursor < features.shape[1]:
        width = first if cursor == 0 else steady
        count = min(width, features.shape[1] - cursor)
        chunk = features[:, cursor:cursor + count]
        if count != width:
            chunk = torch.nn.functional.pad(chunk, (0, 0, 0, width - count))
        chunks.append((chunk, count))
        cursor += count
    return chunks


def cache_tensors(attention, padding):
    result = {}
    if attention is not None:
        for index, layer in enumerate(attention.layers):
            for key in ("keys", "values"):
                value = getattr(layer, key, None)
                if value is not None:
                    result[f"attention-{index}-{key}"] = value
        if attention.layers:
            result["attention-total-seen"] = attention.layers[0].keys.new_tensor(
                [attention.get_seq_length()], dtype=__import__("torch").int64
            )
    if padding is not None:
        for key, layer in sorted(padding.layers.items()):
            result["convolution-" + key.replace(".", "-")] = layer.cache
    return result


def save_caches(writer, prefix, attention, padding):
    return [writer.tensor(prefix + "-" + name, value)
            for name, value in cache_tensors(attention, padding).items()]


def decoder_state(writer, prefix, cache):
    return [writer.tensor(prefix + "-" + name, getattr(cache, name))
            for name in ("hidden_state", "cell_state", "cache")]


def greedy_trace(model, processor, chunks, writer, torch):
    from transformers.models.nemotron3_5_asr.generation_nemotron3_5_asr import Nemotron3_5AsrRNNTDecoderCache

    decoder_cache = Nemotron3_5AsrRNNTDecoderCache(model.config)
    blank = model.config.blank_token_id
    prediction = model.decoder(torch.tensor([[blank]]), cache=decoder_cache)
    decoder_state(writer, "prediction-initial", decoder_cache)
    attention = padding = None
    tokens = []
    steps = []
    frame_base = 0
    for index, (features, valid_features) in enumerate(chunks):
        prefix = f"chunk-{index:04d}"
        input_caches = save_caches(writer, prefix + "-input", attention, padding)
        writer.tensor(prefix + "-features", features)
        writer.tensor(prefix + "-feature-valid-length", torch.tensor([valid_features]))
        encoded = model.get_audio_features(
            features, prompt_ids=torch.tensor([9]), num_lookahead_tokens=3,
            past_key_values=attention, padding_cache=padding, use_cache=True,
            output_attention_mask=False,
        )
        attention = encoded.past_key_values
        padding = encoded.padding_cache
        raw = writer.tensor(prefix + "-encoder-raw", encoded.last_hidden_state)
        one_hot = torch.nn.functional.one_hot(torch.tensor([9]), model.config.num_prompts).to(model.dtype)
        fused = model.prompt_projector(torch.cat((
            encoded.last_hidden_state,
            one_hot[:, None, :].expand(-1, encoded.last_hidden_state.shape[1], -1),
        ), dim=-1))
        writer.tensor(prefix + "-conditioned", fused)
        writer.tensor(prefix + "-joiner-encoder-projection", encoded.pooler_output)
        output_caches = save_caches(writer, prefix + "-output", attention, padding)
        frame_count = encoded.pooler_output.shape[1]
        decisions = []
        for frame in range(frame_count):
            symbols = 0
            while True:
                step_name = prefix + f"-frame-{frame:02d}-symbol-{symbols:02d}"
                writer.tensor(step_name + "-prediction", prediction)
                logits = model.joint(
                    decoder_hidden_states=prediction[:, None, :, :],
                    encoder_hidden_states=encoded.pooler_output[:, frame:frame + 1, None, :],
                )
                if not torch.isfinite(logits).all():
                    raise ValueError("Nonfinite joiner logits")
                writer.tensor(step_name + "-logits", logits)
                token = int(logits.reshape(-1).argmax())
                decisions.append({"frame": frame_base + frame, "symbol": symbols, "token": token})
                if token == blank:
                    break
                symbols += 1
                if symbols >= model.config.max_symbols_per_step:
                    raise ValueError(f"RNN-T symbol limit reached at frame {frame_base + frame}")
                tokens.append(token)
                prediction = model.decoder(torch.tensor([[token]]), cache=decoder_cache)
                decoder_state(writer, step_name + "-committed", decoder_cache)
        text = processor.decode(tokens, skip_special_tokens=True)
        steps.append({"index": index, "valid_features": valid_features,
                      "padded_features": features.shape[1] - valid_features,
                      "encoder_frames": frame_count, "raw_encoder": raw,
                      "input_caches": input_caches, "output_caches": output_caches,
                      "decisions": decisions, "tokens": list(tokens), "text": text})
        frame_base += frame_count
        print(f"  chunk {index + 1}/{len(chunks)} frames={frame_count} tokens={len(tokens)}", flush=True)
    decoder_state(writer, "prediction-final", decoder_cache)
    return {"tokens": tokens, "text": processor.decode(tokens, skip_special_tokens=True),
            "text_with_special_tokens": processor.decode(tokens, skip_special_tokens=False),
            "steps": steps}


def normalized_words(text):
    text = unicodedata.normalize("NFC", text).casefold()
    return "".join(" " if unicodedata.category(ch).startswith("P") else ch for ch in text).split()


def word_errors(reference, hypothesis):
    left, right = normalized_words(reference), normalized_words(hypothesis)
    previous = list(range(len(right) + 1))
    for i, word in enumerate(left, 1):
        current = [i]
        for j, other in enumerate(right, 1):
            current.append(min(current[-1] + 1, previous[j] + 1, previous[j - 1] + (word != other)))
        previous = current
    return {"errors": previous[-1], "reference_words": len(left)}


def environment(catalog):
    import torch

    packages = {}
    for name, expected in catalog["reference"]["packages"].items():
        actual = importlib.metadata.version(name)
        if actual != expected:
            raise ValueError(f"Reference version mismatch: {name}={actual}, expected {expected}")
        packages[name] = actual
    if torch.__version__ != catalog["reference"]["torch_runtime"]:
        raise ValueError("PyTorch runtime build differs from the reference pin")
    from transformers.models.nemotron3_5_asr import modeling_nemotron3_5_asr, processing_nemotron3_5_asr
    from transformers.models.nemotron_asr_streaming import modeling_nemotron_asr_streaming
    from transformers.models.nemotron_asr_streaming import feature_extraction_nemotron_asr_streaming
    from transformers.models.nemotron_asr_streaming import generation_nemotron_asr_streaming
    from transformers.models.parakeet import generation_parakeet
    modules = [modeling_nemotron3_5_asr, processing_nemotron3_5_asr,
               modeling_nemotron_asr_streaming, feature_extraction_nemotron_asr_streaming,
               generation_nemotron_asr_streaming, generation_parakeet]
    return {"python": sys.version, "platform": platform.platform(), "machine": platform.machine(),
            "packages": packages,
            "all_packages": dict(sorted((item.metadata["Name"], item.version)
                                        for item in importlib.metadata.distributions())),
            "implementation_sources": [{"module": item.__name__, "sha256": digest(inspect.getfile(item))}
                                       for item in modules]}


def model_contract(model, processor, data_dir, catalog):
    from transformers.models.nemotron_asr_streaming.feature_extraction_nemotron_asr_streaming import LOG_ZERO_GUARD_VALUE

    nemo = load_json(data_dir / "nemo-config.json")
    if nemo["model"] != catalog["model"] or \
            nemo["source_lock_sha256"] != digest(data_dir / "checkpoint" / "source-lock.json"):
        raise ValueError("NeMo config inspection does not match checkpoint source lock")
    original = nemo["model_config"]
    if original["preprocessor"]["normalize"] != "NA" or \
            original["model_defaults"]["prompt_dictionary"]["de-DE"] != 9:
        raise ValueError("NeMo frontend/conditioning configuration changed")
    tokenizer = load_json(data_dir / "checkpoint" / "tokenizer.json")
    config = model.config
    return {
        "schema_version": 1, "model": catalog["model"], "reference": catalog["reference"],
        "frontend": {
            "sample_rate": 16000, "window_samples": 400, "hop_samples": 160, "fft_size": 512,
            "mel_bins": 128, "window": "symmetric Hann (periodic=False), centered in FFT window",
            "mel": "librosa 0.11.0 float32 Slaney scale and area normalization, 0..8000 Hz",
            "preemphasis": 0.97, "preemphasis_initial_previous_sample": 0,
            "dither_eval": 0, "nemo_config_dither": original["preprocessor"]["dither"],
            "power": "torch.stft complex magnitudes: sqrt(sum(real_imag_squared)) squared",
            "log": "natural log of mel power plus guard", "log_guard": LOG_ZERO_GUARD_VALUE,
            "normalization": "none", "padding": "constant zero waveform at utterance edges",
            "valid_frame_count": "floor(audio_samples / 160); exclude the masked terminal STFT frame",
            "lookahead_samples": 256, "retained_samples_after_emit_max": 511,
            "input_partition_invariance": "tested against full-utterance extraction and irregular sample feeds",
        },
        "streaming": {
            "att_context_size": [56, 3], "subsampling_factor": 8,
            "first_feature_shape": [1, processor.num_mel_frames_first_audio_chunk, 128],
            "steady_feature_shape": [1, processor.num_mel_frames_per_audio_chunk, 128],
            "encoder_output_shape": [1, 4, 1024],
            "steady_new_audio_samples": 5120,
            "first_required_audio_samples": 24 * 160 + 256,
            "first_valid_features": 25,
            "final": "Fixed first/steady shape, pad feature tail with zeros, record valid length. Upstream generate decodes every resulting encoder frame; native parity must use this same policy.",
            "frontend_overlap": "Persistent preemphasis previous sample plus bounded FFT sample history, not reconsumed audio",
            "reset": "New FeatureStream and absent encoder caches; initialize prediction from blank with zero LSTM state",
            "encoder_cache": "24 layers of sliding attention K/V plus causal subsampling/depthwise-convolution caches; names, shapes, dtypes and length snapshots in each fixture",
            "buffer_ownership": "Reference caches mutate in place; input snapshots saved before call. Native graph input/output aliasing forbidden until separately validated.",
        },
        "conditioning": {
            "language": "de-DE", "prompt_id": 9, "prompt_dictionary": processor.prompt_dictionary,
            "one_hot_shape": [1, 1, config.num_prompts],
            "graph": "encoder -> concatenate broadcast one-hot -> Linear(1152,2048) -> ReLU -> Linear(2048,1024) -> encoder_projector Linear(1024,640)",
            "weights": {name: list(value.shape) for name, value in model.named_parameters()
                        if name.startswith(("prompt_projector.", "encoder_projector."))},
            "output_language_tags": "Tokenizer special tokens; not decoder initialization prompts",
        },
        "prediction": {
            "embedding_shape": [13088, 640], "lstm_layers": 2, "hidden_size": 640,
            "state_shape": [2, 1, 640], "initial_h_and_c": "zero",
            "initial_input_token": config.blank_token_id,
            "blank": "No state update; reuse prediction output",
            "nonblank": "Run accepted token through prediction and commit returned h/c/output",
            "weights": {name: list(value.shape) for name, value in model.named_parameters()
                        if name.startswith("decoder.")},
        },
        "joiner": {
            "input_shape": [1, 1, 1, 640], "operation": "ReLU(encoder_projection + prediction_projection) -> Linear(640,13088)",
            "blank_id": config.blank_token_id, "tie_break": "argmax first index",
            "symbol_limit": config.max_symbols_per_step,
            "symbol_limit_policy": "Error; never silently force frame advancement",
            "nonfinite_logits": "Error",
        },
        "tokenizer": {
            "vocab_size": config.vocab_size, "blank_id": config.blank_token_id,
            "model_type": tokenizer["model"]["type"], "decoder": tokenizer["decoder"],
            "normalizer": tokenizer["normalizer"], "pre_tokenizer": tokenizer["pre_tokenizer"],
            "special_tokens": tokenizer["added_tokens"],
            "decode": "Pinned processor.decode with skip_special_tokens true for text and false for raw tag-preserving text; no cleanup of tokenization spaces",
            "native_rules": "Implement tokenizer decoder rules, not a naive independent ID-to-byte table",
        },
        "tolerances": catalog["profile"]["tolerances"],
        "tolerance_status": "Frontend empirically checked by captures; other budgets predeclared for M1/M3, not proven QNN bounds",
        "nemo_config_sha256": digest(data_dir / "nemo-config.json"),
        "nemo_parity": "not_verified",
    }


def run(data_dir, output, selection, threads):
    import numpy as np
    import torch
    from transformers import Nemotron3_5AsrForRNNT, Nemotron3_5AsrProcessor

    catalog, _ = verify_checkpoint(data_dir / "checkpoint", ("transformers_weights",))
    env = environment(catalog)
    if output.exists():
        raise ValueError("Reference output already exists; choose a fresh directory")
    torch.set_num_threads(threads)
    torch.set_num_interop_threads(1)
    torch.manual_seed(0)
    torch.use_deterministic_algorithms(True)
    processor = Nemotron3_5AsrProcessor.from_pretrained(data_dir / "checkpoint", local_files_only=True)
    processor.set_num_lookahead_tokens(3)
    if processor.prompt_dictionary["de-DE"] != 9 or processor.streaming_latency_ms != 320:
        raise ValueError("Unexpected language or streaming profile")
    print("Loading pinned FP32 model", flush=True)
    model = Nemotron3_5AsrForRNNT.from_pretrained(
        data_dir / "checkpoint", local_files_only=True, dtype=torch.float32,
        attn_implementation=catalog["reference"]["attention_backend"],
    ).eval()
    config = model.config
    if config.blank_token_id != 13087 or config.vocab_size != 13088 or \
            config.encoder_config.sliding_window != 57 or config.encoder_config.subsampling_factor != 8:
        raise ValueError("Unexpected pinned model dimensions")
    output.mkdir(parents=True)
    write_json(output / "contract.json", model_contract(model, processor, data_dir, catalog))
    constants = TensorWriter(output / "frontend")
    constants.tensor("hann-window", torch.hann_window(processor.feature_extractor.win_length, periodic=False))
    constants.tensor("mel-filters", processor.feature_extractor.mel_filters)
    manifest = load_json(data_dir / "corpus" / "manifest.json")
    verify_file(data_dir / "corpus" / manifest["dataset_card"]["file"], manifest["dataset_card"])
    chosen = set(selection.split(",")) if selection else {clip["id"] for clip in manifest["clips"]}
    known = {clip["id"] for clip in manifest["clips"]}
    if not chosen or not chosen.issubset(known):
        raise ValueError("Unknown or empty fixture selection")
    results = []
    with torch.inference_mode():
        for clip in manifest["clips"]:
            if clip["id"] not in chosen:
                continue
            print(f"Reference: {clip['id']}", flush=True)
            audio = data_dir / "corpus" / safe_name(clip["file"])
            verify_file(audio, clip)
            samples = read_wav(audio)
            features, peak = features_for_audio(samples, processor.feature_extractor, torch, [5120])
            alternate, alternate_peak = features_for_audio(
                samples, processor.feature_extractor, torch, [1, 159, 512, 777, 4096]
            )
            offline = processor.feature_extractor(
                np.asarray(samples, dtype=np.float32), sampling_rate=16000, return_tensors="pt"
            )
            expected = offline.input_features[:, :features.shape[1]]
            tolerance = catalog["profile"]["tolerances"]["frontend"]
            torch.testing.assert_close(features, expected, **tolerance)
            torch.testing.assert_close(features, alternate, **tolerance)
            chunks = chunk_features(features, torch)
            writer = TensorWriter(output / clip["id"])
            trace = greedy_trace(model, processor, chunks, writer, torch)
            upstream = model.generate(
                input_features=(chunk for chunk, _ in chunks),
                prompt_ids=torch.tensor([9]), num_lookahead_tokens=3,
                return_dict_in_generate=True,
            )
            upstream_tokens = [int(token) for token in upstream.sequences[0]
                               if int(token) != config.blank_token_id]
            if upstream_tokens != trace["tokens"]:
                raise ValueError(f"Greedy reference disagrees with upstream generate: {clip['id']}")
            writer.tensor("token-ids", torch.tensor(trace["tokens"], dtype=torch.int32))
            result = {
                "clip": clip, "audio_sha256": digest(audio),
                "frontend_max_abs_error": float((features - expected).abs().max()),
                "partition_max_abs_error": float((features - alternate).abs().max()),
                "frontend_peak_buffer_samples": max(peak, alternate_peak),
                "upstream_token_parity": True, "trace": trace, "tensors": writer.records,
            }
            if clip["kind"] == "quality":
                result["wer"] = word_errors(clip["transcript"], trace["text"])
            write_json(output / clip["id"] / "manifest.json", result)
            results.append({"id": clip["id"], "manifest_sha256": digest(output / clip["id"] / "manifest.json"),
                            "tokens": trace["tokens"], "text": trace["text"],
                            "wer": result.get("wer"), "chunks": len(chunks)})
    write_json(output / "manifest.json", {
        "schema_version": 1, "status": "reference_captured",
        "model": catalog["model"], "reference": catalog["reference"], "profile": catalog["profile"],
        "environment": env, "threads": threads, "seed": 0,
        "source_lock_sha256": digest(data_dir / "checkpoint" / "source-lock.json"),
        "corpus_manifest_sha256": digest(data_dir / "corpus" / "manifest.json"),
        "contract_sha256": digest(output / "contract.json"),
        "frontend_constants": constants.records, "fixtures": results,
        "complete_fixture_selection": chosen == known,
        "tail_policy": "EOF pads final fixed-shape feature chunk with zero features, as upstream generate requires; all resulting encoder frames are decoded. Valid and padded feature counts are recorded. No claim of NeMo tail parity.",
        "cache_policy": "Initial attention/convolution caches absent (upstream lazy zero init); returned objects retained across chunks; immutable on-disk input snapshots precede mutation; native graph I/O must use distinct buffers unless separately validated.",
        "decoder_policy": "Initialize prediction with blank once; reuse on blank, commit state after accepted nonblank; torch argmax first-index tie break; symbol-limit error rather than upstream forced advance.",
        "nemo_parity": "not_verified", "npu_status": "not_tested",
    })


def compare(left, right):
    a, b = load_json(left / "manifest.json"), load_json(right / "manifest.json")
    for key in ("model", "reference", "profile", "source_lock_sha256", "corpus_manifest_sha256",
                "contract_sha256", "fixtures"):
        if a[key] != b[key]:
            raise ValueError(f"Reference runs differ: {key}")
    for fixture in a["fixtures"]:
        name = fixture["id"]
        safe_name(name)
        first, second = load_json(left / name / "manifest.json"), load_json(right / name / "manifest.json")
        if digest(left / name / "manifest.json") != fixture["manifest_sha256"] or \
                digest(right / name / "manifest.json") != fixture["manifest_sha256"]:
            raise ValueError(f"Fixture manifest identity mismatch: {name}")
        if first != second:
            raise ValueError(f"Reference tensors/metadata differ: {name}")
        for record in first["tensors"]:
            validate_tensor_record(record)
            verify_file(left / name / safe_name(record["file"]), record)
            verify_file(right / name / safe_name(record["file"]), record)
        print(f"Verified {name}: {len(first['tensors'])} tensors", flush=True)
    for root, manifest in ((left, a), (right, b)):
        if digest(root / "contract.json") != manifest["contract_sha256"]:
            raise ValueError("Model contract identity mismatch")
        for record in manifest["frontend_constants"]:
            validate_tensor_record(record)
            verify_file(root / "frontend" / safe_name(record["file"]), record)
    print("PASS exact reproducibility: tokens, text, state and tensor SHA-256")


def validate_tensor_record(record):
    dtype = record["dtype"]
    if not re.fullmatch(r"[<|][fiu][1248]", dtype):
        raise ValueError("Unsupported reference tensor dtype")
    elements = 1
    for dimension in record["shape"]:
        if type(dimension) is not int or dimension < 0:
            raise ValueError("Invalid reference tensor dimension")
        elements *= dimension
    if elements * int(dtype[-1]) != record["size"]:
        raise ValueError("Tensor shape/size mismatch")
    safe_name(record["file"])


def accept(data_dir, left, right, output):
    compare(left, right)
    manifest = load_json(left / "manifest.json")
    catalog = read_catalog()
    corpus = load_json(data_dir / "corpus" / "manifest.json")
    if manifest["model"] != catalog["model"] or manifest["reference"] != catalog["reference"] or \
            manifest["profile"] != catalog["profile"]:
        raise ValueError("Reference run does not match committed M0 pins")
    if not manifest["complete_fixture_selection"] or \
            {item["id"] for item in manifest["fixtures"]} != {item["id"] for item in corpus["clips"]}:
        raise ValueError("M0 requires all corpus/diagnostic fixtures")
    if manifest["corpus_manifest_sha256"] != digest(data_dir / "corpus" / "manifest.json") or \
            manifest["source_lock_sha256"] != digest(data_dir / "checkpoint" / "source-lock.json"):
        raise ValueError("M0 input identities changed")
    kinds = {clip["kind"] for clip in corpus["clips"]}
    required = {"quality", "speech_crop", "pause", "silence", "chunk_boundary", "incomplete_tail", "long_sequence"}
    if not required.issubset(kinds):
        raise ValueError("M0 diagnostic coverage incomplete")
    long = next(item for item in manifest["fixtures"] if item["id"] == "long")
    if long["chunks"] < catalog["profile"]["long_sequence_chunks"]:
        raise ValueError("Long-sequence fixture misses required chunk count")
    for clip in corpus["clips"]:
        verify_file(data_dir / "corpus" / safe_name(clip["file"]), clip)
    inventory = load_json(data_dir / "target" / "dlls.json")
    if inventory["unresolved"]:
        raise ValueError("Unresolved QNN DLL inventory")
    evidence = []
    for name in ("hardware.txt", "qnn-runtime-version.txt", "dlls.json"):
        path = data_dir / "target" / name
        evidence.append({"file": str(path), "sha256": digest(path), "size": path.stat().st_size})
    scores = [item["wer"] for item in manifest["fixtures"] if item["wer"] is not None]
    total_words = sum(item["reference_words"] for item in scores)
    if len(scores) < 10 or total_words <= 0:
        raise ValueError("M0 requires the ten-clip German corpus baseline")
    errors = sum(item["errors"] for item in scores)
    write_json(output, {
        "schema_version": 1, "status": "m0_reference_accepted",
        "model": catalog["model"], "profile": catalog["profile"],
        "reference_backend": catalog["reference"],
        "runs": [{"directory": str(root), "manifest_sha256": digest(root / "manifest.json")}
                 for root in (left, right)],
        "contract_sha256": manifest["contract_sha256"],
        "fixture_count": len(manifest["fixtures"]), "long_sequence_chunks": long["chunks"],
        "reproducibility": "Exact tensor, token and text hashes across independent processes",
        "target_evidence": evidence,
        "quality": {"clips": len(scores), "errors": errors, "reference_words": total_words,
                    "wer_percent": errors * 100 / total_words,
                    "scope": corpus["quality_scope"], "normalization": corpus["wer_normalization"]},
        "nemo_parity": "not_verified",
        "npu_status": "not_tested",
        "remaining_m1_work": "HTP feasibility, actual loaded DLL/driver-candidate selection, native tensor/cache layout and numerical parity",
    })
    print(f"PASS M0 reference: {len(manifest['fixtures'])} fixtures, German WER {errors * 100 / total_words:.3f}%")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    subparsers = parser.add_subparsers(dest="command", required=True)
    capture = subparsers.add_parser("capture")
    capture.add_argument("--output", type=Path, required=True)
    capture.add_argument("--fixtures", default="")
    capture.add_argument("--threads", type=int, choices=range(1, 33), default=4)
    comparison = subparsers.add_parser("compare")
    comparison.add_argument("left", type=Path)
    comparison.add_argument("right", type=Path)
    acceptance = subparsers.add_parser("accept")
    acceptance.add_argument("left", type=Path)
    acceptance.add_argument("right", type=Path)
    acceptance.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    if args.command == "capture":
        run(args.data_dir, args.output, args.fixtures, args.threads)
    elif args.command == "compare":
        compare(args.left, args.right)
    else:
        accept(args.data_dir, args.left, args.right, args.output)


if __name__ == "__main__":
    main()
