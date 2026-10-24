"""Windows-only signed FP32 DSP execution and M1 numerical/performance gates."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, write_json
from nemotron_m1_run import numerical_audit, result_report


def package_environment(manifest_path):
    manifest = load_json(manifest_path)
    if manifest.get("schema_version") != 1 or manifest.get("backend") != "OEMFastRPC-HVX-FP32":
        raise ValueError("Expected the explicitly approved Windows FP32 DSP backend")
    base = Path(manifest_path).parent
    resolved = {}
    for name, file_key, hash_key in (
        ("dsp", "file", "sha256"), ("catalog", "catalog_file", "catalog_sha256"),
        ("driver", "file", "sha256"), ("host", "file", "sha256"),
    ):
        entry = manifest[name] if name in ("driver", "host") else manifest["dsp"]
        path = Path(entry[file_key])
        if not path.is_absolute():
            path = base / path
        path = path.resolve(strict=True)
        if digest(path) != entry[hash_key]:
            raise ValueError(f"Signed package/driver identity mismatch: {name}")
        resolved[name] = path
    env = os.environ.copy()
    env.update({
        "NM_FP32_DSP_PACKAGE": str(resolved["dsp"]),
        "NM_FP32_DSP_SHA256": manifest["dsp"]["sha256"],
        "NM_FP32_RPC_DRIVER": str(resolved["driver"]),
        "NM_FP32_RPC_SHA256": manifest["driver"]["sha256"],
    })
    return manifest, env


def invoke(executable, args, log, env, timeout):
    started = time.monotonic()
    with Path(log).open("x", encoding="utf-8", newline="\n") as stream:
        try:
            process = subprocess.run([str(executable.resolve()), *map(str, args)], env=env,
                                     cwd=executable.parent, stdout=stream, stderr=subprocess.STDOUT,
                                     timeout=timeout, check=False)
            code = process.returncode
        except subprocess.TimeoutExpired:
            stream.write(f"\nERROR: DSP command exceeded {timeout} seconds\n")
            code = 124
    return {"command": [str(executable), *map(str, args)], "returncode": code,
            "seconds": time.monotonic() - started, "log": str(log), "log_sha256": digest(log)}


def run(data_dir, executable, package, graphs, cases, output, timeout, repeats, workers=4):
    if output.exists():
        raise ValueError("Preserve existing evidence; select a fresh run directory")
    manifest, env = package_environment(package)
    env["NM_FP32_DSP_WORKERS"] = str(workers)
    if executable is None:
        executable = Path(manifest["host"]["file"])
        if not executable.is_absolute():
            executable = package.parent / executable
        executable = executable.resolve(strict=True)
    if digest(executable) != manifest["host"]["sha256"]:
        raise ValueError("Host executable does not match the signed package's protocol/build pin")
    graph_manifest = load_json(graphs / "graphs.json")
    accepted = load_json(data_dir / "m0-acceptance.json")
    if accepted["status"] != "m0_reference_accepted" or \
            graph_manifest["m0_acceptance_sha256"] != digest(data_dir / "m0-acceptance.json"):
        raise ValueError("Graphs are not bound to the accepted M0 reference")
    for entry in graph_manifest["graphs"]:
        if digest(graphs / entry["file"]) != entry["sha256"]:
            raise ValueError("Graph identity changed since FP32 export")
    if not executable.is_file():
        raise ValueError("Build the native Windows FastRPC FP32 executor first")
    output.mkdir(parents=True)
    commands, numerical, timing, failures = [], [], [], []
    prefix = [graphs / (name + ".nmg") for name in ("subsampling-first", "subsampling-steady", "encoder")]
    for name in ("long", "tail", "silence"):
        target = output / (name + ".nmr")
        print("Signed FP32 DSP correctness:", name, flush=True)
        command = invoke(executable, ["pipeline", *prefix, cases / (name + ".nmf"), target],
                         output / (name + ".log"), env, timeout)
        commands.append(command)
        if not target.is_file():
            failures.append({"case": name, "phase": "execute", "returncode": command["returncode"],
                             "error": "No diagnostic output artifact"})
            break
        try:
            report = result_report(target)
            audit = numerical_audit(cases / (name + ".nmf"), target, output / (name + "-audit.json"))
            numerical.append({"case": name, "native": report, "independent": audit})
            if command["returncode"] or not audit["correctness_passed"]:
                failures.append({"case": name, "phase": "numerical_parity",
                                 "returncode": command["returncode"], "violations": audit["violations"]})
        except (ValueError, KeyError) as error:
            failures.append({"case": name, "phase": "result_invalid", "error": str(error)})
    correctness = not failures and len(numerical) == 3 and numerical[0]["native"]["steps"] >= 65
    if correctness:
        for repeat in range(repeats):
            target = output / f"benchmark-{repeat}.nmr"
            print(f"Signed FP32 DSP timing: {repeat + 1}/{repeats}", flush=True)
            command = invoke(executable, ["pipeline-benchmark", *prefix, cases / "long.nmf", target],
                             output / f"benchmark-{repeat}.log", env, timeout)
            commands.append(command)
            if command["returncode"] or not target.is_file():
                failures.append({"phase": "benchmark", "repeat": repeat, "returncode": command["returncode"]})
                break
            report = result_report(target)
            if report["measurement"] != "timing_only":
                raise ValueError("Expected a timing-only artifact distinct from numerical acceptance")
            timing.append(report)
    performance = bool(len(timing) == repeats and
                       all(item["warm_p95_encoder_and_cache_ms"] is not None and
                           item["warm_p95_model_and_cache_ms"] < 320 for item in timing))
    status = "m1_passed" if correctness and performance else \
        "m1_correctness_passed_performance_pending" if correctness else "m1_not_accepted"
    write_json(output / "report.json", {
        "schema_version": 1, "status": status, "backend": "OEMFastRPC-HVX-FP32",
        "backend_change": "Explicitly approved Windows-only replacement for unavailable QNN custom-package preparation",
        "m0_acceptance_sha256": digest(data_dir / "m0-acceptance.json"),
        "package_manifest_sha256": digest(package), "package": manifest,
        "executable_sha256": digest(executable), "graph_manifest_sha256": digest(graphs / "graphs.json"),
        "commands": commands, "numerical": numerical, "timing": timing, "failures": failures,
        "correctness_passed": correctness, "performance_passed": performance,
        "tolerances": {"atol": 0.005, "rtol": 0.005},
        "encoder_warm_p95_target_ms": 320, "cpu_model_fallback": False,
        "latency_scope": "Subsampling +24-layer encoder +binding/cache copies; no omission of heavy model frontend",
        "dsp_workers": workers,
        "qnn_graph_acceptance_claimed": False, "hmx_execution_claimed": False,
    })
    if status != "m1_passed":
        raise SystemExit(f"{status}: see the persisted numerical/timing evidence")
    print("PASS M1: continued-cache FP32 DSP parity and measured warm latency", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    parser.add_argument("--executable", type=Path,
                        help="Optional identical-hash copy of the host executable pinned in the package manifest")
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--graphs", type=Path, required=True)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--repeats", type=int, choices=range(1, 11), default=3)
    parser.add_argument("--workers", type=int, choices=(1, 4), default=4)
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    run(args.data_dir, args.executable.resolve() if args.executable else None, args.package, args.graphs, args.cases,
        args.output, args.timeout, args.repeats, args.workers)


if __name__ == "__main__":
    main()
