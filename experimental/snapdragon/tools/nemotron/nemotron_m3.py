"""M3 decode-table export and actual native WAV ASR acceptance; no runtime Python."""
import argparse
import hashlib
from pathlib import Path
import os
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, verify_file, write_json
from nemotron_m2 import resource_records, package_bytes


def tokenizer_bytes(path):
    source = load_json(path)
    expected = {"type": "Metaspace", "replacement": "\u2581", "prepend_scheme": "always", "split": True}
    if source["decoder"] != expected or source["model"]["type"] != "BPE" or \
            digest(path) != "3f3d481deb073b64c2082e8c7860d487a3a62774bf4e9e4faac83007e181f246":
        raise ValueError("Unsupported or unpinned tokenizer decode contract")
    vocab = source["model"]["vocab"]
    if len(vocab) != 13087 or set(vocab.values()) != set(range(13087)):
        raise ValueError("Tokenizer vocabulary holes or model ID mismatch")
    pieces = [None] * 13089
    for text, index in vocab.items():
        pieces[index] = [text.encode("utf-8"), 0]
    for token in source["added_tokens"]:
        index = token["id"]
        if not 0 <= index < len(pieces):
            raise ValueError("Added-token ID outside pinned table")
        encoded = token["content"].encode("utf-8")
        if pieces[index] is not None and pieces[index][0] != encoded:
            raise ValueError("Added-token content disagrees with vocabulary")
        pieces[index] = [encoded, int(token["special"])]
    entries, strings = bytearray(), bytearray()
    for piece in pieces:
        if piece is None or not piece[0] or len(piece[0]) > 256:
            raise ValueError("Missing or oversized tokenizer piece")
        entries.extend(struct.pack("<3I", len(strings), len(piece[0]), piece[1]))
        strings.extend(piece[0])
    prefix = b"NMTOK001" + struct.pack("<6I", 1, len(pieces), 13087, 13087, len(strings), 0)
    return prefix + bytes.fromhex(digest(path)) + entries + strings


def export(data, output):
    if output.exists() or output.parent.resolve() != data.resolve():
        raise ValueError("M3 index must be a new file directly under data-dir")
    records, manifest, snapshot = resource_records(data)
    raw = bytearray(package_bytes(data, records, manifest, snapshot))
    table = tokenizer_bytes(data / "checkpoint" / "tokenizer.json")
    payload_bytes = len(raw) - 256
    raw[256 + 64:256 + 72] = b"NMRT3202"
    struct.pack_into("<2Q", raw, 256 + 480, payload_bytes, len(table))
    raw.extend(table)
    struct.pack_into("<Q", raw, 64, len(raw) - 256)
    raw[136:168] = hashlib.sha256(raw[256:]).digest()
    output.write_bytes(raw)
    write_json(output.with_suffix(".json"), {"schema_version": 2, "file": str(output), "sha256": digest(output),
               "size": len(raw), "resources": records, "m1_baseline": snapshot,
               "tokenizer_source_sha256": digest(data / "checkpoint" / "tokenizer.json"),
               "decode_table_sha256": hashlib.sha256(table).hexdigest(),
               "tokenizer_entries": 13089, "model_vocabulary": 13088, "model_blank_id": 13087})
    print(f"Exported M3 index: {output};{len(raw)}bytes; same v17 DSP model,13089 tokenizer entries", flush=True)


def read_events(path):
    import json

    events = [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines()]
    if not events:
        raise ValueError("Native ASR emitted no events")
    previous_seq = -1
    previous_time = {}
    for event in events:
        if event.get("schema") != "newos.nemotron.asr.v1" or event.get("event") not in ("partial", "final"):
            raise ValueError("Native ASR error or unexpected event schema")
        sequence, utterance = event["seq"], event["utterance"]
        if sequence <= previous_seq or event["audio_samples"] < previous_time.get(utterance, 0):
            raise ValueError("Native ASR event sequence/audio time regressed")
        if event["model_cpu"] or event["hmx"] or event["backend"] != "OEMFastRPC-HVX-FP32":
            raise ValueError("Unexpected model placement/fallback")
        if event["prediction_calls"] != len(event["tokens"]) + 1 or \
                event["joiner_calls"] != len(event["tokens"]) + event["encoder_frames"]:
            raise ValueError("Blank reuse/nonblank commit invocation counts disagree")
        previous_seq = sequence
        previous_time[utterance] = event["audio_samples"]
    return events


def invoke(executable, package, wav, output, log, second=None, cwd=None, environment=None):
    command = [str(executable), "--package", str(package), "--wav", str(wav), "--output", str(output),
               "--language", "de-DE", "--chunk-ms", "320"]
    if second is not None:
        command += ["--reset-wav", str(second)]
    result = subprocess.run(command, cwd=cwd, env=environment, capture_output=True, timeout=600)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Native WAV ASR failed({result.returncode}); see{log}")
    return read_events(output)

def deployment_package(data, package, directory):
    import shutil

    raw = bytearray(package.read_bytes())
    for index in range(16):
        offset = 256 + 512 + index * 320
        role, flags = struct.unpack_from("<2I", raw, offset + 256)
        if flags:
            continue
        relative = raw[offset:offset + 256].split(b"\0", 1)[0].decode("utf-8")
        source = package.parent / relative
        folder = directory / "resources" / ("dsp" if role in (11, 12) else str(role))
        folder.mkdir(parents=True, exist_ok=True)
        target = folder / source.name
        os.link(source, target)
        encoded = str(target.relative_to(directory)).encode("utf-8")
        if len(encoded) >= 256:
            raise ValueError("Deployment resource path exceeds table capacity")
        raw[offset:offset + 256] = bytes(256)
        raw[offset:offset + len(encoded)] = encoded
    raw[136:168] = hashlib.sha256(raw[256:]).digest()
    target = directory / "model.nmp"
    target.write_bytes(raw)
    return target


def negative_checks(executable, package, wav, output):
    import json

    count = 0
    def reject(arguments, name):
        nonlocal count
        result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, timeout=90)
        (output / (name + ".log")).write_bytes(result.stdout + result.stderr)
        if not result.returncode or not result.stderr:
            raise ValueError(f"ASR negative check missing explicit failure: {name}")
        count += 1

    prefix = ["--package", package, "--wav", wav]
    reject(prefix + ["--language", "en-US"], "bad-language")
    reject(prefix + ["--chunk-ms", "160"], "unsupported-profile")
    reject(prefix + ["--chunk-ms", "not-a-number"], "invalid-profile")
    reject(prefix + ["--wav", wav], "duplicate-option")
    reject(prefix + ["--unknown", "value"], "unknown-option")
    reject(["--package"], "missing-value")
    reject(["--package", package, "--wav", output / "missing.wav"], "missing-wav")
    bad_wav = output / "truncated.wav"
    bad_wav.write_bytes(wav.read_bytes()[:-1])
    reject(["--package", package, "--wav", bad_wav], "truncated-wav")
    bad_package = output / "bad-package.nmp"
    raw = bytearray(package.read_bytes()); raw[-1] ^= 1
    bad_package.write_bytes(raw)
    reject(["--package", bad_package, "--wav", wav], "bad-package-hash")
    existing = output / "existing.jsonl"
    existing.write_bytes(b"preserve existing output")
    reject(prefix + ["--output", existing], "existing-output")
    if existing.read_bytes() != b"preserve existing output":
        raise ValueError("ASR overwrote existing output")
    return {"passed": True, "cases": count, "explicit_nonzero_exit_and_diagnostics": True}


def audit(data, executable, package, output, selected=None):
    from nemotron_reference import word_errors
    import shutil

    if output.exists():
        raise ValueError("M3 evidence directory must not exist")
    output.mkdir(parents=True)
    reference = data / "reference-1"
    manifest = load_json(reference / "manifest.json")
    accepted = load_json(data / "m0-acceptance.json")
    if digest(reference / "manifest.json") != accepted["runs"][0]["manifest_sha256"]:
        raise ValueError("Frozen M0 manifest changed")
    results, baseline_words, baseline_errors, native_errors = [], 0, 0, 0
    fixtures = manifest["fixtures"]
    if selected:
        fixtures = [fixture for fixture in fixtures if fixture["id"] in selected]
        if len(fixtures) != len(set(selected)):
            raise ValueError("Unknown fixture selection")
    for fixture in fixtures:
        identifier = fixture["id"]
        root = reference / identifier
        case = load_json(root / "manifest.json")
        verify_file(root / "manifest.json", {"size": (root / "manifest.json").stat().st_size,
                                            "sha256": fixture["manifest_sha256"]})
        wav = data / "corpus" / case["clip"]["file"]
        verify_file(wav, case["clip"])
        events = invoke(executable, package, wav, output / (identifier + ".jsonl"), output / (identifier + ".log"))
        finals = [event for event in events if event["event"] == "final"]
        if len(finals) != 1 or finals[0]["audio_samples"] != case["clip"]["samples"]:
            raise ValueError("WAV final event/sample count mismatch")
        final = finals[0]
        tokens_equal = final["tokens"] == fixture["tokens"]
        text_equal = final["text"] == fixture["text"]
        quality = case["clip"]["kind"] == "quality"
        if not quality and (not tokens_equal or not text_equal):
            raise ValueError(f"Deterministic diagnostic token/text parity failed: {identifier}")
        score = word_errors(case["clip"]["transcript"], final["text"]) if quality else None
        if quality:
            baseline_words += case["wer"]["reference_words"]
            baseline_errors += case["wer"]["errors"]
            native_errors += score["errors"]
        results.append({"fixture": identifier, "token_parity": tokens_equal, "text_parity": text_equal,
                        "tokens": final["tokens"], "text": final["text"], "wer": score,
                        "wall_ms": final["wall_ms"], "host_cpu_user_ms": final["host_cpu_user_ms"],
                        "host_cpu_kernel_ms": final["host_cpu_kernel_ms"],
                        "output": str(output / (identifier + ".jsonl"))})
        print(f"Native WAV ASR {identifier}: tokens_equal={tokens_equal} text_equal={text_equal}", flush=True)
    delta = (native_errors - baseline_errors) * 100 / baseline_words if baseline_words else 0
    if delta > 1.0:
        raise ValueError(f"Native WER regressed{delta:.6f} absolute percentage points")
    complete = not selected
    if complete:
        diagnostic = next(fixture for fixture in fixtures if fixture["id"] == "tail")
        wav = data / "corpus" / load_json(reference / "tail" / "manifest.json")["clip"]["file"]
        events = invoke(executable, package, wav, output / "reset.jsonl", output / "reset.log", second=wav)
        finals = [event for event in events if event["event"] == "final"]
        if len(finals) != 2 or any(event["tokens"] != diagnostic["tokens"] or event["text"] != diagnostic["text"] for event in finals):
            raise ValueError("Reset/second utterance differs from fresh process")
        unicode_dir = output / "\u90e8\u7f72-\u6a21\u578b-\u00e4"
        unicode_dir.mkdir()
        deployed = unicode_dir / "nemotron-asr.exe"
        shutil.copyfile(executable, deployed)
        unicode_wav = unicode_dir / "\u97f3\u9891-\u00f6.wav"
        shutil.copyfile(wav, unicode_wav)
        environment = dict(os.environ)
        environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
        local_package = deployment_package(data, package, unicode_dir)
        isolated = invoke(deployed, Path(local_package.name), Path(unicode_wav.name), unicode_dir / "\u7ed3\u679c.jsonl",
                          output / "isolated.log", cwd=unicode_dir, environment=environment)
        if isolated[-1]["tokens"] != diagnostic["tokens"] or isolated[-1]["text"] != diagnostic["text"]:
            raise ValueError("Isolated Unicode WAV deployment token/text mismatch")
        if not isolated[-1].get("vendor_ascii_module_staging"):
            raise ValueError("Unicode vendor boundary exception was not reported")
        staging_lines = [line[7:] for line in (output / "isolated.log").read_text(encoding="utf-8").splitlines()
                         if line.startswith("staged=")]
        if len(staging_lines) != 1 or Path(staging_lines[0]).exists() or Path(staging_lines[0]).parent.exists():
            raise ValueError("Owned vendor staging files/directory were not cleaned")
        errors = negative_checks(executable, package, wav, output)
    write_json(output / "report.json", {
        "schema_version": 1, "status": "m3_wav_gates_passed" if complete else "m3_selected_diagnostic_passed",
        "full_fixture_selection": complete, "fixtures": len(results), "results": results,
        "all_token_parity": all(result["token_parity"] for result in results),
        "all_text_parity": all(result["text_parity"] for result in results),
        "baseline_wer_percent": baseline_errors * 100 / baseline_words if baseline_words else None,
        "native_wer_percent": native_errors * 100 / baseline_words if baseline_words else None,
        "wer_delta_absolute_percentage_points": delta, "allowed_wer_delta": 1.0,
        "wer_normalization": "Frozen M0 NFC/casefold/punctuation-to-space/whitespace; UTF8 text compared separately",
        "reset_fresh_parity": complete, "isolated_unicode_deployment": complete,
        "executable": str(executable), "executable_sha256": digest(executable),
        "package": str(package), "package_sha256": digest(package),
        "frontend": "CPU", "model": "DSP-HVX-FP32+DSP-scalar; no HMX/GPU/host model inference",
        "m1_latency_waiver_unchanged": True,
        "negative_cli_checks": errors if complete else None,
        "relative_model_paths": complete, "unicode_model_and_signed_module_paths": complete,
        "vendor_unicode_module_exception": {
            "user_approved": True, "approved_on": "2026-10-14",
            "mechanism": "Hash-identical signed module/catalog copied to owned temporary ASCII directory",
            "model_and_wav_paths_remain_unicode": True, "cleanup_verified": complete,
            "driver_boot_signing_trust_changed": False,
        },
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    exporter = commands.add_parser("export")
    exporter.add_argument("--output", type=Path, required=True)
    runner = commands.add_parser("audit")
    runner.add_argument("--executable", type=Path, required=True)
    runner.add_argument("--package", type=Path, required=True)
    runner.add_argument("--output", type=Path, required=True)
    runner.add_argument("--fixtures", nargs="+")
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.command == "export":
        export(args.data_dir, args.output)
    else:
        audit(args.data_dir, args.executable, args.package, args.output, args.fixtures)


if __name__ == "__main__":
    main()
