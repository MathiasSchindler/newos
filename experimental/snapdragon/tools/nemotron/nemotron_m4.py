"""Exact FP32 decoder packing/fusion and paired end-to-end offload verification."""
import argparse
import hashlib
from pathlib import Path
import statistics
import struct
import sys
import subprocess
import os

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, write_json
from nemotron_m1 import prediction, joiner, fixture_write
from nemotron_fp32 import convert
from nemotron_m3 import invoke
from nemotron_trace import load_model


def fixture_steps(path):
    import numpy as np

    raw = Path(path).read_bytes()
    tensors, _, steps = struct.unpack_from("<3I", raw, 52)
    descriptors = []
    for index in range(tensors):
        offset = 256 + 64 + index * 160
        rank = struct.unpack_from("<I", raw, offset + 72)[0]
        descriptors.append({"name": raw[offset:offset + 64].split(b"\0", 1)[0].decode(),
                            "dtype": struct.unpack_from("<I", raw, offset + 68)[0],
                            "shape": struct.unpack_from("<4I", raw, offset + 80)[:rank]})
    result = []
    for step in range(steps):
        offset = 256 + 64 + tensors * 160 + step * (16 + tensors * 32)
        values, cells = {}, {}
        for index, descriptor in enumerate(descriptors):
            mode, source, data, size, age, _ = struct.unpack_from("<IIQQII", raw, offset + 16 + index * 32)
            dtype = "<i4" if descriptor["dtype"] == 0x0032 else "<f4"
            if mode == 0:
                value = np.frombuffer(raw, dtype=dtype, count=size // 4, offset=256 + data).reshape(descriptor["shape"]).copy()
                cells[(0, descriptor["name"])] = value
            elif mode == 2:
                value = np.zeros(descriptor["shape"], dtype=dtype)
                cells[(0, descriptor["name"])] = "zero"
            elif mode == 1 and age == 1 and step:
                value = result[step - 1]["values"][descriptors[source]["name"]]
                cells[(0, descriptor["name"])] = (0, descriptors[source]["name"], 1)
            else:
                raise ValueError("Unsupported frozen decoder fixture carry")
            values[descriptor["name"]] = value
        result.append({"flags": struct.unpack_from("<I", raw, offset)[0], "values": values, "cells": cells})
    return result


def numerical(data, graphs, output):
    import torch
    from nemotron_m1_run import numerical_audit

    if output.exists():
        raise ValueError("M4 numerical evidence directory exists")
    output.mkdir(parents=True)
    records = load_json(graphs / "graphs.json")["graphs"]
    pred_path = data / "m1" / "fastrpc-fp32-decoder-cases" / "prediction.nmf"
    join_path = data / "m1" / "fastrpc-fp32-decoder-cases" / "joiner.nmf"
    pred_steps, join_steps = fixture_steps(pred_path), fixture_steps(join_path)
    model = load_model(data)
    fused_steps = []
    with torch.inference_mode():
        for index, step in enumerate(pred_steps):
            cells = dict(step["cells"])
            enc = join_steps[index % len(join_steps)]["values"]["encoder"]
            pred = step["values"]["prediction"]
            cells[(0, "encoder")] = enc
            cells[(0, "logits")] = model.joint.head(torch.relu(torch.from_numpy(enc) + torch.from_numpy(pred)))
            fused_steps.append({"flags": step["flags"], "cells": cells})
    fused_path = output / "fused-cpu-reference.nmf"
    fixture_write(fused_path, [records[2]], fused_steps, "fused-decoder-cpu-reference", 32)
    manifest = load_json(data / "m1" / "fastrpc-fp32-pad-v17" / "manifest.json")
    executable = Path(manifest["host"]["file"])
    if digest(executable) != manifest["host"]["sha256"]:
        raise ValueError("Pinned numerical-run host changed")
    environment = dict(os.environ)
    environment.update({"NM_FP32_DSP_PACKAGE": manifest["dsp"]["file"], "NM_FP32_DSP_SHA256": manifest["dsp"]["sha256"],
                        "NM_FP32_RPC_DRIVER": manifest["driver"]["file"], "NM_FP32_RPC_SHA256": manifest["driver"]["sha256"],
                        "NM_FP32_DSP_WORKERS": "4", "NM_FP32_DSP_POWER_PROFILE": "baseline",
                        "NM_FP32_RPC_ALIGN_COEFFICIENTS": "1"})
    audits = []
    for name, graph, fixture in (("packed-prediction", records[0], pred_path), ("packed-joiner", records[1], join_path),
                                ("fused-prediction-joiner", records[2], fused_path)):
        result = output / (name + ".nmr")
        process = subprocess.run([str(executable), "run", str(graphs / graph["file"]), str(fixture), str(result)],
                                 env=environment, capture_output=True, timeout=600)
        (output / (name + ".log")).write_bytes(process.stdout + process.stderr)
        if process.returncode:
            raise RuntimeError(f"M4 DSP numerical execution failed:{name}")
        audit = numerical_audit(fixture, result, output / (name + "-audit.json"))
        if not audit["correctness_passed"]:
            raise ValueError(f"M4 FP32 decoder parity failed:{name}")
        audits.append({"name": name, "steps": len(pred_steps) if name != "packed-joiner" else len(join_steps),
                       "elements": audit["elements"], "violations": audit["violations"],
                       "reference": "frozen M0 decoder values" if name != "fused-prediction-joiner" else
                       "frozen M0 prediction/state values plus same pinned CPU joiner on real captured encoder inputs"})
        print(f"PASS M4 numerical:{name},{audit['elements']}elements,zero violations", flush=True)
    write_json(output / "report.json", {"schema_version": 1, "status": "m4_decoder_numerical_passed",
               "audits": audits, "tolerances": {"atol": .005, "rtol": .005},
               "changed_M0": False, "DSP_module": "unchanged original v17 FP32"})


def export(data, output, graphs):
    import torch

    if output.exists() or graphs.exists() or output.parent.resolve() != data.resolve():
        raise ValueError("M4 index/graphs must be new; index belongs directly under data-dir")
    baseline = data / "nemotron-runtime-m3.nmp"
    accepted = load_json(data / "m3" / "acceptance-final-1" / "report.json")
    if accepted["status"] != "m3_accepted" or digest(baseline) != accepted["package_sha256"]:
        raise ValueError("M4 requires unchanged accepted M3 package")
    graphs.mkdir(parents=True)
    model = load_model(data)
    records = []
    with torch.inference_mode():
        pred = prediction(model, 32)
        records.append(convert(pred, True).write(graphs / "prediction-packed.nmg"))
        joint = joiner(model, 32)
        records.append(convert(joint, True).write(graphs / "joiner-packed.nmg"))
        fused = prediction(model, 32)
        encoder = fused.input("encoder", [1, 640])
        combined = fused.binary("fused-joint-add", "ElementWiseAdd", encoder, fused.names["prediction"])
        activated = fused.unary("fused-joint-relu", "Relu", combined)
        fused.output(fused.linear("fused-joint-head", activated, model.joint.head), "logits")
        records.append(convert(fused, True).write(graphs / "prediction-joiner-packed.nmg"))
    write_json(graphs / "graphs.json", {"schema_version": 1, "graphs": records,
               "precision": "exact FP32 values; column-tiles32; no quantization",
               "m3_package_sha256": digest(baseline), "decoder_fusion": "prediction then joiner for same encoder frame"})
    raw = bytearray(baseline.read_bytes())
    raw[256 + 64:256 + 72] = b"NMRT3204"
    struct.pack_into("<I", raw, 256 + 496, 1)
    for role, record in ((9, records[2]), (10, records[1])):
        offset = 256 + 512 + (role - 1) * 320
        source = graphs / record["file"]
        relative = str(source.resolve().relative_to(data.resolve())).encode("utf-8")
        if len(relative) >= 256:
            raise ValueError("Packed decoder resource path too long")
        raw[offset:offset + 256] = bytes(256)
        raw[offset:offset + len(relative)] = relative
        struct.pack_into("<Q", raw, offset + 264, source.stat().st_size)
        raw[offset + 272:offset + 304] = bytes.fromhex(digest(source))
    raw[136:168] = hashlib.sha256(raw[256:]).digest()
    output.write_bytes(raw)
    write_json(output.with_suffix(".json"), {"schema_version": 4, "file": str(output), "sha256": digest(output),
               "graphs": str(graphs), "m3_package_sha256": digest(baseline),
               "decoder_fusion": True, "precision": 32, "quantized": False,
               "DSP_module": "unchanged accepted v17", "HMX": False})
    print(f"Exported M4 index{output}: same FP32 coefficients and signed v17 module", flush=True)


def benchmark(data, executable, package, output):
    if output.exists():
        raise ValueError("M4 benchmark evidence directory already exists")
    output.mkdir(parents=True)
    m3 = load_json(data / "m3" / "acceptance-final-1" / "report.json")
    baseline = data / "m3" / "acceptance-final-1" / "native" / "\u90e8\u7f72-\u6a21\u578b-\u00e4" / "nemotron-asr.exe"
    if digest(baseline) != m3["executable_sha256"]:
        raise ValueError("Immutable M3 executable fingerprint mismatch")
    old_package = Path(m3["package"])
    cases = ("de_de-1586-pcm16", "silence", "long")
    samples = []
    for repeat in range(3):
        for identifier in cases:
            root = data / "reference-1" / identifier
            case = load_json(root / "manifest.json")
            wav = data / "corpus" / case["clip"]["file"]
            variants = (("m3", baseline, old_package), ("m4", executable, package))
            if repeat & 1:
                variants = tuple(reversed(variants))
            for name, binary, index in variants:
                prefix = output / f"{identifier}-{repeat}-{name}"
                events = invoke(binary, index, wav, prefix.with_suffix(".jsonl"), prefix.with_suffix(".log"), second=wav)
                finals = [event for event in events if event["event"] == "final"]
                if len(finals) != 2 or finals[1]["tokens"] != case["trace"]["tokens"] or finals[1]["text"] != case["trace"]["text"]:
                    raise ValueError("Paired warmed benchmark changed transcript")
                value = finals[1]
                if name == "m4":
                    chunks = value["encoder_frames"] // 4
                    if not value["shared_rpc_io"] or not value["fused_decoder"] or value["host_rpc_copy_bytes"] != 0 or \
                            value["fused_joiner_reuses"] != len(value["tokens"]) or \
                            value["rpc_graph_calls"] != chunks * 6 + len(value["tokens"]) + 1:
                        raise ValueError("M4 residency/fusion work-count gate failed")
                samples.append({"case": identifier, "repeat": repeat, "variant": name,
                                "wall_ms": value["wall_ms"],
                                "host_cpu_ms": value["host_cpu_user_ms"] + value["host_cpu_kernel_ms"],
                                "rpc_graph_calls": value.get("rpc_graph_calls"),
                                "host_rpc_copy_bytes": value.get("host_rpc_copy_bytes"),
                                "shared_rpc_binding_bytes": value.get("shared_rpc_binding_bytes"),
                                "fused_joiner_reuses": value.get("fused_joiner_reuses"),
                                "encoder_frames": value["encoder_frames"], "tokens": len(value["tokens"])})
                print(f"Warm{identifier}/{repeat} {name}: {value['wall_ms']}ms", flush=True)
    comparisons = []
    for identifier in cases:
        before = [s["wall_ms"] for s in samples if s["case"] == identifier and s["variant"] == "m3"]
        after = [s["wall_ms"] for s in samples if s["case"] == identifier and s["variant"] == "m4"]
        comparisons.append({"case": identifier, "m3_warm_ms": before, "m4_warm_ms": after,
                            "m3_median_ms": statistics.median(before), "m4_median_ms": statistics.median(after),
                            "nonregressing": statistics.median(after) <= statistics.median(before)})
    passed = all(case["nonregressing"] for case in comparisons)
    write_json(output / "report.json", {"schema_version": 1, "status": "m4_paired_speed_passed" if passed else "m4_speed_pending",
               "warm_scope": "Second reset utterance, package/staging excluded; alternating M3/M4 order",
               "settings": "4DSPworkers,baselinepower,aligned128coefficients,originalv17FP32module",
               "comparisons": comparisons, "samples": samples, "performance_passed": passed,
               "baseline_sha256": digest(baseline), "candidate_sha256": digest(executable),
               "package_sha256": digest(package), "M1_final_320ms_gate_reclassified": False})
    if not passed:
        raise SystemExit("M4 paired warm end-to-end performance regressed; see retained evidence")

def quality(data, executable, package, output):
    from nemotron_m3 import audit, read_events

    audit(data, executable, package, output)
    report = load_json(output / "report.json")
    work = []
    for result in report["results"]:
        event = read_events(result["output"])[-1]
        chunks = event["encoder_frames"] // 4
        if not event["shared_rpc_io"] or not event["fused_decoder"] or event["host_rpc_copy_bytes"] != 0 or \
                event["fused_joiner_reuses"] != len(event["tokens"]) or \
                event["rpc_graph_calls"] != chunks * 6 + len(event["tokens"]) + 1:
            raise ValueError("Full corpus M4 IO/fusion thresholds failed")
        work.append({"fixture": result["fixture"], "rpc_graph_calls": event["rpc_graph_calls"],
                     "m3_equivalent_rpc_calls": chunks * 6 + len(event["tokens"]) * 2 + 1,
                     "saved_rpc_calls": event["fused_joiner_reuses"],
                     "host_rpc_copy_bytes": event["host_rpc_copy_bytes"],
                     "shared_binding_bytes": event["shared_rpc_binding_bytes"]})
    report["status"] = "m4_quality_offload_passed"
    report["work_counts"] = work
    report["cpu_model_operations"] = []
    report["host_operations"] = [
        {"operation": "PCM/preemphasis/STFT/mel/log", "reason": "validated bounded CPU frontend; M4 prioritizes model math",
         "next": "offload only after measured total benefit"},
        {"operation": "greedy argmax/control", "reason": "no argmax/control ABI in original signed v17 module",
         "next": "validated device decoder loop rather than unverified HMX/quantization"},
        {"operation": "cache carry between disjoint shared regions", "reason": "existing no-alias contract; persistent mapped DDR is not an on-chip state-commit API",
         "next": "device-side carry with explicit ordering and reset proof"},
        {"operation": "UTF8/text/events/files", "reason": "host I/O and required user output", "next": "retain host-side"}
    ]
    report["resident_memory_scope"] = "Persistent device-mapped RPC DDR workspace views, not claimed on-chip state or zero CPU carry copies"
    write_json(output / "report.json", report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("export")
    builder.add_argument("--output", type=Path, required=True)
    builder.add_argument("--graphs", type=Path, required=True)
    timing = commands.add_parser("benchmark")
    timing.add_argument("--executable", type=Path, required=True)
    timing.add_argument("--package", type=Path, required=True)
    timing.add_argument("--output", type=Path, required=True)
    numeric = commands.add_parser("numerical")
    numeric.add_argument("--graphs", type=Path, required=True)
    numeric.add_argument("--output", type=Path, required=True)
    full = commands.add_parser("quality")
    full.add_argument("--executable", type=Path, required=True)
    full.add_argument("--package", type=Path, required=True)
    full.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.command == "export":
        export(args.data_dir, args.output, args.graphs)
    elif args.command == "benchmark":
        benchmark(args.data_dir, args.executable, args.package, args.output)
    elif args.command == "numerical":
        numerical(args.data_dir, args.graphs, args.output)
    else:
        quality(args.data_dir, args.executable, args.package, args.output)


if __name__ == "__main__":
    main()
