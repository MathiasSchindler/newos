"""Run the freestanding M1 probes and retain honest correctness/timing evidence."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, write_json
from nemotron_m1 import CONTRACT_SHA, HEADER, MAGIC, RUNTIME_SHA, TENSOR, name_bytes, read_catalog


def result_report(path):
    path = Path(path)
    with path.open("rb") as stream:
        header = stream.read(HEADER)
        if len(header) != HEADER or header[:8] != MAGIC:
            raise ValueError("Invalid result header")
        version, kind, header_bytes = struct.unpack_from("<3I", header, 8)
        precision, tensors, nodes, steps = struct.unpack_from("<4I", header, 48)
        size = struct.unpack_from("<Q", header, 64)[0]
        if version not in (1, 2) or (kind, header_bytes) != (4, HEADER) or nodes or \
                not 1 <= tensors <= 512 or not 1 <= steps <= 512 or precision not in (16, 32) or \
                path.stat().st_size != HEADER + size:
            raise ValueError("Invalid result dimensions or file size")
        catalog = read_catalog()
        model = catalog["model"]
        identity = hashlib.sha256((model["repository"] + "@" + model["revision"]).encode()).digest()
        if header[72:104] != identity or header[104:136] != bytes.fromhex(CONTRACT_SHA) or \
                header[168:200] != bytes.fromhex(RUNTIME_SHA):
            raise ValueError("Result provenance mismatch")
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
        if value.digest() != header[136:168]:
            raise ValueError("Result payload checksum mismatch")
        frequency, restore, rss, cycles = struct.unpack_from("<4Q", header, 200)
        if frequency == 0:
            raise ValueError("Result has no valid QPC frequency")
        stream.seek(HEADER + 64 + tensors * TENSOR)
        metrics = []
        stride = 64 + tensors * 32
        for index in range(steps):
            data = stream.read(64)
            if len(data) != 64:
                raise ValueError("Truncated result metrics")
            step, flags, front, core, copies, wall, maximum, violations, compared, reserved = \
                struct.unpack("<IIQQQQfIQQ", data)
            if step != index or reserved or not math.isfinite(maximum) or maximum < 0:
                raise ValueError("Invalid result metrics")
            metrics.append({"step": step, "flags": flags, "front_ms": front * 1000 / frequency,
                            "encoder_ms": core * 1000 / frequency,
                            "cache_copy_ms": copies * 1000 / frequency,
                            "wall_ms": wall * 1000 / frequency,
                            "max_abs_error": maximum, "violations": violations,
                            "compared_elements": compared})
            stream.seek(HEADER + 64 + tensors * TENSOR + (index + 1) * stride)
    warm = [item["wall_ms"] for item in metrics[2:]]
    warm_encoder = [item["encoder_ms"] + item["cache_copy_ms"] for item in metrics[2:]]
    warm_model = [item["front_ms"] + item["encoder_ms"] + item["cache_copy_ms"] for item in metrics[2:]]
    def percentile(values, proportion):
        if not values:
            return None
        ordered = sorted(values)
        return ordered[max(0, math.ceil(len(ordered) * proportion) - 1)]
    return {
        "file": str(path), "sha256": digest(path), "steps": steps,
        "precision": precision, "restore_ms": restore * 1000 / frequency,
        "peak_working_set_bytes": rss, "profile_cycles": cycles,
        "correctness_passed": all(item["violations"] == 0 and item["compared_elements"] > 0 for item in metrics),
        "violations": sum(item["violations"] for item in metrics),
        "maximum_absolute_error": max(item["max_abs_error"] for item in metrics),
        "warmup_chunks": 2, "warm_p50_wall_ms": percentile(warm, 0.5),
        "warm_p95_wall_ms": percentile(warm, 0.95),
        "warm_p95_encoder_and_cache_ms": percentile(warm_encoder, 0.95),
        "warm_p95_model_and_cache_ms": percentile(warm_model, 0.95),
        "metrics": metrics,
        "measurement": "timing_only" if all(item["compared_elements"] == 0 for item in metrics) else "correctness_and_timing",
    }


def numerical_audit(fixture, result, output=None):
    import numpy as np
    from nemotron_trace import read_outputs

    expected, actual = read_outputs(fixture), read_outputs(result)
    if set(expected) != set(actual):
        raise ValueError("Result did not produce exactly the expected active output tensors")
    by_tensor = {}
    for key in expected:
        left, right = expected[key], actual[key]
        if left.shape != right.shape:
            raise ValueError("Actual/reference tensor shape mismatch")
        error = np.abs(left - right)
        bad = (~np.isfinite(right)) | (error > 0.005 + 0.005 * np.abs(left))
        record = by_tensor.setdefault(key[1], {"elements": 0, "violations": 0, "max_abs_error": 0.0})
        record["elements"] += int(error.size)
        record["violations"] += int(bad.sum())
        record["max_abs_error"] = max(record["max_abs_error"], float(error.max()))
    metrics = result_report(result)
    count = sum(record["violations"] for record in by_tensor.values())
    elements = sum(record["elements"] for record in by_tensor.values())
    if count != metrics["violations"] or elements != sum(row["compared_elements"] for row in metrics["metrics"]):
        raise ValueError("Independent numerical audit disagrees with native comparison counters")
    audit = {"fixture_sha256": digest(fixture), "result_sha256": metrics["sha256"],
             "violations": count, "elements": elements, "correctness_passed": count == 0,
             "tolerances": {"atol": 0.005, "rtol": 0.005}, "tensors": by_tensor}
    if output:
        write_json(output, audit)
    print(f"Independent audit: {count}/{elements} violations", flush=True)
    return audit


def benchmark(data_dir, build_dir, contexts, fixture, output, repeats):
    if output.exists():
        raise ValueError("Benchmark output already exists")
    output.mkdir(parents=True)
    commands, runs = [], []
    reference = load_json(data_dir / "reference-1" / "long" / "manifest.json")
    for repeat in range(repeats):
        result = output / f"long-{repeat}.nmr"
        command = invoke(build_dir / "nemotron-probe.exe", [
            "pipeline-benchmark", contexts / "subsampling-first.nmc", contexts / "subsampling-steady.nmc",
            contexts / "encoder.nmc", fixture, result,
        ], output / f"long-{repeat}.log", 900)
        commands.append(command)
        if command["returncode"] != 0:
            write_json(output / "failure.json", {"commands": commands, "status": "benchmark_failed"})
            raise RuntimeError("Native benchmark failed; see saved command log")
        run = result_report(result)
        if run["measurement"] != "timing_only" or run["steps"] < 65:
            raise ValueError("Expected timing-only long-sequence result; never substitute for correctness")
        duration = reference["clip"]["samples"] / 16000
        run["rtf_graph_and_io"] = sum(item["wall_ms"] for item in run["metrics"]) / (1000 * duration)
        runs.append(run)
        print(f"Benchmark {repeat + 1}/{repeats}: p95wall={run['warm_p95_wall_ms']:.3f}ms "
              f"p95encoder+copies={run['warm_p95_encoder_and_cache_ms']:.3f}ms", flush=True)
    write_json(output / "report.json", {
        "schema_version": 1, "status": "timing_measured_not_correctness_acceptance",
        "fixture_sha256": digest(fixture),
        "contexts": {name: digest(contexts / (name + ".nmc")) for name in
                     ("subsampling-first", "subsampling-steady", "encoder")},
        "commands": commands, "runs": runs, "correctness_assessed": False,
        "scope": "Three full native warmup passes; excludes output comparisons, result copies and profiling reset. Includes input validation, binding/cache copies, front and encoder graph execution.",
        "target_met_all_repeats": all(item["warm_p95_encoder_and_cache_ms"] < 320 for item in runs),
    })


def investigation(data_dir, output):
    root = data_dir / "m1"
    timing = load_json(root / "benchmark-fp16" / "report.json")
    if timing["correctness_assessed"] or any(run["measurement"] != "timing_only" for run in timing["runs"]):
        raise ValueError("Expected separately measured benchmark evidence")
    numerical = load_json(root / "quant16-axis-full-run" / "independent-audit.json")
    trials = {}
    for label, relative in (
        ("original-fp16", "full-v2-validation/long.nmr"),
        ("mixed-fixed16-per-tensor-risk", "mixed-risk-run/long.nmr"),
        ("mixed-fixed16-per-channel-risk", "quant16-axis-risk-run/long.nmr"),
        ("mixed-fixed16-per-channel-full", "quant16-axis-full-run/long.nmr"),
    ):
        trials[label] = result_report(root / relative)
    traces = {}
    for label, relative in (
        ("float-encoder", "trace-encoder-fp32/comparison.json"),
        ("float-subsampling", "trace-subsampling-fp32/comparison.json"),
        ("mixed-local-operators", "trace-mixed-risk-v2/local-operators.json"),
    ):
        path = root / relative
        traces[label] = {"file": str(path), "sha256": digest(path), "report": load_json(path)}
    write_json(output, {
        "schema_version": 1, "status": "m1_blocked_numerical_parity",
        "model": read_catalog()["model"], "m0_acceptance_sha256": digest(data_dir / "m0-acceptance.json"),
        "timing": timing, "numeric_trials": trials, "independent_numeric_audit": numerical,
        "intermediate_traces": traces, "tolerances_changed": False, "cpu_model_fallback": False,
        "findings": [
            "The previous wall statistic included comparison/result snapshots and per-call profiling administration; it was not a clean steady inference measurement.",
            f"{len(timing['runs'])} benchmark processes with three full warmup passes each measured p95 wall "
            + "/".join(f"{run['warm_p95_wall_ms']:.3f}" for run in timing["runs"]) + " ms; "
            + f"320-ms target met in all repeats: {timing['target_met_all_repeats']}.",
            "FP32 declarations execute FP16 math on pinned V73 HTP; native32-bit outputs do not restore precision lost inside graph kernels.",
            "Intermediate tracing locates the first float subsampling failure in stemConv2d and first encoder failure in the first feed-forward down-projection; later attention/convolution/cache errors accumulate.",
            "Official HTP precision-compensation/high-precision-sigmoid flags did not fix the traced float graph.",
            f"Fixed-point and mixed/per-channel trials also fail the unchanged FP32 reference; independent "
            f"dequantized comparison confirms {numerical['violations']}/{numerical['elements']} violations "
            "for the full per-channel run.",
        ],
        "gate_passed": False,
        "next_step": "A materially different validated precision strategy is required before M1 can pass; neither timing-only success, changed reference tensors nor larger tolerances can substitute for the current correctness gate.",
    })
    print(f"Persisted verified investigation; M1 correctness remains blocked: {output}")


def invoke(executable, arguments, log, timeout):
    started = time.monotonic()
    with Path(log).open("x", encoding="utf-8", newline="\n") as stream:
        try:
            completed = subprocess.run([str(executable.resolve()), *map(str, arguments)],
                                       cwd=executable.parent, stdout=stream, stderr=subprocess.STDOUT,
                                       timeout=timeout, check=False)
            returncode = completed.returncode
        except subprocess.TimeoutExpired:
            stream.write(f"\nERROR: native command exceeded {timeout} seconds\n")
            returncode = 124
    return {"command": [str(executable), *map(str, arguments)], "returncode": returncode,
            "elapsed_seconds": time.monotonic() - started, "log": str(log),
            "log_sha256": digest(log)}


def run(data_dir, build_dir, output, timeout, full, graph_dir=None, fixture_dir=None, decoder_dir=None):
    if output.exists():
        raise ValueError("Run directory already exists; preserve earlier evidence")
    output.mkdir(parents=True)
    prepare = build_dir / "nemotron-prepare.exe"
    probe = build_dir / "nemotron-probe.exe"
    if not prepare.is_file() or not probe.is_file():
        raise ValueError("Build the native prepare and probe executables first")
    acceptance = load_json(data_dir / "m0-acceptance.json")
    if acceptance["status"] != "m0_reference_accepted":
        raise ValueError("M0 acceptance is required")
    graphs = graph_dir or data_dir / "m1" / ("full-fp16" if full else "risk-fp16")
    names = ("subsampling-first", "subsampling-steady", "encoder") if full else \
        ("encoder-risk", "prediction", "joiner")
    commands, contexts = [], {}
    failures = []
    for name in names:
        context = output / (name + ".nmc")
        print(f"HTP prepare: {name}", flush=True)
        command = invoke(prepare, ["prepare", graphs / (name + ".nmg"), context],
                         output / (name + "-prepare.log"), timeout)
        commands.append(command)
        if command["returncode"] != 0:
            failures.append({"phase": "prepare", "graph": name, "returncode": command["returncode"]})
            continue
        if not context.is_file():
            raise ValueError("Native prepare returned success without a context")
        contexts[name] = context
    results = []
    def collect_result(result, command, fixture):
        if result.is_file():
            try:
                results.append(result_report(result))
            except ValueError as error:
                failures.append({"phase": "result_invalid", "fixture": fixture, "error": str(error)})
        elif command["returncode"] == 0:
            failures.append({"phase": "result_missing", "fixture": fixture,
                             "error": "Native run returned success without a result artifact"})
        if command["returncode"] != 0:
            failures.append({"phase": "execute", "fixture": fixture, "returncode": command["returncode"]})
    if full:
        if len(contexts) == 3:
            for name in ("long", "tail", "silence"):
                result = output / (name + ".nmr")
                print(f"HTP carried-cache pipeline: {name}", flush=True)
                command = invoke(probe, [
                    "pipeline", contexts["subsampling-first"], contexts["subsampling-steady"],
                    contexts["encoder"], (fixture_dir or data_dir / "m1" / "full-cases-fp16") / (name + ".nmf"), result,
                ], output / (name + "-run.log"), timeout)
                commands.append(command)
                collect_result(result, command, name)
    else:
        for name in names:
            if name not in contexts:
                continue
            fixture = (fixture_dir or data_dir / "m1" / "risk-cases-fp16") / "long.nmf" if name == "encoder-risk" else \
                (decoder_dir or data_dir / "m1" / "decoder-cases-64-fp16") / (name + ".nmf")
            result = output / (name + ".nmr")
            print(f"HTP risk execution: {name}", flush=True)
            command = invoke(probe, ["run", contexts[name], fixture, result],
                             output / (name + "-run.log"), timeout)
            commands.append(command)
            collect_result(result, command, name)
    complete = full and not failures and len(results) == 3 and \
        all(item["correctness_passed"] for item in results) and results[0]["steps"] >= 65
    risk_complete = not full and not failures and len(results) == 3 and all(item["correctness_passed"] for item in results)
    write_json(output / "report.json", {
        "schema_version": 1, "status": "m1_correctness_passed" if complete else
        "m1_risk_probes_passed" if risk_complete else "m1_not_accepted",
        "scope": "full_encoder_pipeline" if full else "risk_and_decoder_probes",
        "m0_acceptance_sha256": digest(data_dir / "m0-acceptance.json"),
        "graph_manifest_sha256": digest(graphs / "graphs.json"),
        "backend": "QnnHtp.dll direct freestanding ABI; no external inference framework",
        "commands": commands, "contexts": {name: {"file": str(path), "sha256": digest(path)}
                                        for name, path in contexts.items()},
        "results": results, "failures": failures,
        "encoder_performance_target_ms": 320,
        "encoder_performance_passed": bool(full and results and results[0]["warm_p95_encoder_and_cache_ms"] is not None and
                                           results[0]["warm_p95_encoder_and_cache_ms"] < 320),
        "performance_is_not_correctness_acceptance": True,
        "cpu_model_fallback": False,
        "nemo_parity": "not_verified",
    })
    if failures or (full and not complete) or (not full and not risk_complete):
        raise SystemExit("M1 gate not accepted; see persisted report and native logs")


def summarize(data_dir, output):
    root = data_dir / "m1"
    full_paths = [root / "full-v2-validation" / (name + ".nmr") for name in ("long", "tail", "silence")]
    full = [result_report(path) for path in full_paths]
    variants = {}
    paths = {
        "encoder-risk-fp16": "early-htp-v2-fp16/encoder-risk.nmr",
        "encoder-risk-fp32-interface": "early-htp-v3-fp32/encoder-risk.nmr",
        "encoder-risk-fully-connected": "early-htp-v4-fc-fp16/encoder-risk.nmr",
        "encoder-risk-centered-variance": "early-htp-v5-centered-fp32/encoder-risk.nmr",
        "prediction-fp16": "early-htp-fp16/prediction.nmr",
        "prediction-fused-fp16": "early-htp-v2-fp16/prediction-fused.nmr",
        "prediction-fp32-interface": "early-htp-v3-fp32/prediction.nmr",
        "joiner-fp16": "early-htp-fp16/joiner.nmr",
    }
    for label, relative in paths.items():
        variants[label] = result_report(root / relative)
    correctness = all(item["correctness_passed"] for item in full) and full[0]["steps"] >= 65
    archive = root / "early-htp-full-v2-fp16" / "encoder.nmc"
    with archive.open("rb") as stream:
        package_header = stream.read(HEADER)
    payload_bytes = struct.unpack_from("<Q", package_header, 64)[0]
    io = struct.unpack_from("<I", package_header, 52)[0]
    opaque_offset = (64 + io * TENSOR + 15) & ~15
    value = {
        "schema_version": 1, "status": "m1_correctness_passed" if correctness else "m1_blocked_numerical_parity",
        "implementation": "Freestanding ARM64 direct-QNN prepare/restore/pipeline, validated immutable envelopes and carried cache fixtures",
        "m0_acceptance_sha256": digest(data_dir / "m0-acceptance.json"),
        "reference_cpu_adapter": load_json(root / "cpu-adapter-long.json"),
        "model_revision": read_catalog()["model"]["revision"],
        "full_encoder": {"layers": 24, "conditioned_language": "de-DE",
                         "context_bytes": payload_bytes - opaque_offset,
                         "context_file": str(archive), "sha256": digest(archive),
                         "results": full, "correctness_passed": correctness},
        "bounded_trials": variants,
        "encoder_warm_p95_with_cache_ms": full[0]["warm_p95_encoder_and_cache_ms"],
        "encoder_intermediate_target_ms": 320,
        "encoder_intermediate_target_met": full[0]["warm_p95_encoder_and_cache_ms"] < 320,
        "performance_is_not_correctness_acceptance": True,
        "placement": "All exported model graph operations submitted to QnnHtp.dll; native CPU performs scheduling, checked buffer copies and numerical comparisons. No CPU model fallback.",
        "profiling": "QPC wall/execute/copy measurements; BASIC QNN cycle events unavailable (zero is not proof of zero work). Operator-level HMX/spill/fill costs remain unmeasured.",
        "runtime": {"qnn_core": "2.39.0", "htp_provider": "5.50.0",
                    "sdk_yaml_backend_api": "2.18", "qairt": "2.50.0.260828",
                    "loaded_fastrpc": r"C:\Windows\System32\DriverStore\FileRepository\qcnspmcdm8380.inf_arm64_553f7b0d71497f2c\libcdsprpc.dll",
                    "restore_requires_prepare_dll": False},
        "preserved_failures": [
            "Initial depthwise filter layout was invalid; corrected to SDK [H,W,1,channel_out], and native preflight now rejects it before QNN.",
            "Initial expected subsampling cache tensors were mutable upstream views; corrected to immutable per-step snapshots before final pipeline validation.",
        ],
        "tolerances": read_catalog()["profile"]["tolerances"],
        "blocker": None if correctness else
            "Encoder/cache numerical comparisons against the fixed FP32 M0 reference fail. FP32 interfaces, dense FullyConnected and centered-variance rewrites did not pass; prediction also misses the frozen state tolerance. Do not advance through the M1 correctness gate or relax tolerances silently.",
        "next_bounded_step": "Capture per-operator intermediate tensors to isolate HTP normalization/projection differences; evaluate validated mixed precision without CPU-heavy fallback.",
        "nemo_parity": "not_verified",
    }
    write_json(output, value)
    print(f"Persisted {value['status']}: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--build-dir", type=Path,
                        default=Path(__file__).resolve().parents[2] / "build" / "nemotron-asr")
    commands = parser.add_subparsers(dest="command", required=True)
    run_parser = commands.add_parser("run")
    run_parser.add_argument("--output", type=Path, required=True)
    run_parser.add_argument("--timeout", type=int, default=900)
    run_parser.add_argument("--full", action="store_true")
    run_parser.add_argument("--graphs-dir", type=Path)
    run_parser.add_argument("--fixtures-dir", type=Path)
    run_parser.add_argument("--decoder-fixtures-dir", type=Path)
    audit = commands.add_parser("result")
    audit.add_argument("path", type=Path)
    summary = commands.add_parser("summarize")
    summary.add_argument("--output", type=Path, required=True)
    timing = commands.add_parser("benchmark")
    timing.add_argument("--contexts", type=Path, required=True)
    timing.add_argument("--fixture", type=Path, required=True)
    timing.add_argument("--output", type=Path, required=True)
    timing.add_argument("--repeats", type=int, choices=range(1, 11), default=3)
    numerical = commands.add_parser("audit")
    numerical.add_argument("--fixture", type=Path, required=True)
    numerical.add_argument("--result", type=Path, required=True)
    numerical.add_argument("--output", type=Path, required=True)
    findings = commands.add_parser("investigation")
    findings.add_argument("--output", type=Path, required=True)
    parser.add_argument("--deps-dir", type=Path)
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.command == "run":
        run(args.data_dir, args.build_dir.resolve(), args.output, args.timeout, args.full,
            args.graphs_dir, args.fixtures_dir, args.decoder_fixtures_dir)
    elif args.command == "result":
        print(json.dumps(result_report(args.path), indent=2))
    elif args.command == "summarize":
        summarize(args.data_dir, args.output)
    elif args.command == "benchmark":
        benchmark(args.data_dir, args.build_dir.resolve(), args.contexts, args.fixture, args.output, args.repeats)
    elif args.command == "audit":
        numerical_audit(args.fixture, args.result, args.output)
    else:
        investigation(args.data_dir, args.output)


if __name__ == "__main__":
    main()
