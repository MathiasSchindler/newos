"""Pinned M2 runtime index export and independent native frontend acceptance."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, read_catalog, verify_file, write_json
from nemotron_m1 import CONTRACT_SHA, header

PREFIX, ENTRY, FILES = 512, 320, 16
HEADER = 256


def resource_records(data):
    approval = load_json(data / "m1" / "provisional-acceptance-v17.json")
    snapshot = approval["snapshot"]
    manifest_path = Path(snapshot["manifest"])
    if approval["status"] != "m1_provisionally_accepted_latency_waiver" or \
            digest(manifest_path) != snapshot["manifest_sha256"]:
        raise ValueError("Missing verified provisional M1 baseline")
    manifest = load_json(manifest_path)
    source = data / "checkpoint"
    graphs = data / "m1" / "fastrpc-fp32-packed-graphs"
    decoders = data / "m1" / "fastrpc-fp32-decoder-graphs"
    gate = load_json(approval["strict_gate_evidence"]["report"])
    if not gate["correctness_passed"] or digest(graphs / "graphs.json") != gate["graph_manifest_sha256"]:
        raise ValueError("M1 graph identity or correctness gate mismatch")
    graph_records = load_json(graphs / "graphs.json")["graphs"]
    decoder_records = load_json(decoders / "graphs.json")["graphs"]
    for root, records in ((graphs, graph_records), (decoders, decoder_records)):
        for record in records:
            verify_file(root / record["file"], record)
    catalog = read_catalog()
    by_name = {record["name"]: record for record in catalog["files"]}
    for name in ("config.json", "processor_config.json", "tokenizer.json", "tokenizer_config.json"):
        verify_file(source / name, by_name[name])
    verify_file(source / catalog["license_artifact"]["file"], catalog["license_artifact"])
    paths = [source / "config.json", source / "processor_config.json", source / "tokenizer.json",
             source / "tokenizer_config.json", data / "reference-1" / "contract.json",
             graphs / "subsampling-first.nmg", graphs / "subsampling-steady.nmg", graphs / "encoder.nmg",
             decoders / "prediction.nmg", decoders / "joiner.nmg", Path(manifest["dsp"]["file"]),
             Path(manifest["dsp"]["catalog_file"]), Path(manifest["host"]["file"]),
             manifest_path, Path(manifest["driver"]["file"]), source / catalog["license_artifact"]["file"]]
    records = []
    for role, path in enumerate(paths, 1):
        absolute = role == 15
        name = str(path.resolve()) if absolute else str(path.resolve().relative_to(data.resolve()))
        encoded = name.encode("utf-8")
        if not encoded or len(encoded) >= 256:
            raise ValueError(f"Runtime resource path exceeds UTF8 capacity: {path}")
        records.append({"role": role, "flags": int(absolute), "path": name,
                        "size": path.stat().st_size, "sha256": digest(path)})
    return records, manifest, snapshot


def package_bytes(data, records, manifest, snapshot):
    if len(records) != FILES:
        raise ValueError("Runtime package requires all16 resources")
    reference = data / "reference-1"
    if digest(reference / "contract.json") != CONTRACT_SHA:
        raise ValueError("M0 contract changed")
    constants = {record["name"]: record for record in load_json(reference / "manifest.json")["frontend_constants"]}
    for record in constants.values():
        verify_file(reference / "frontend" / record["file"], record)
    window = bytes(56 * 4) + (reference / "frontend" / constants["hann-window"]["file"]).read_bytes() + bytes(56 * 4)
    mel = (reference / "frontend" / constants["mel-filters"]["file"]).read_bytes()
    if len(window) != 512 * 4 or len(mel) != 128 * 257 * 4:
        raise ValueError("Frozen frontend table shape mismatch")
    payload = bytearray(PREFIX + FILES * ENTRY)
    label = b"nemotron_runtime_m2"
    payload[:len(label)] = label
    payload[64:72] = b"NMRT3201"
    struct.pack_into("<14I", payload, 72, 1, 16000, 512, 160, 400, 128,
                     25, 32, 56, 3, 9, 13087, 13088, FILES)
    struct.pack_into("<2Q", payload, 128, len(payload), len(payload) + len(window))
    payload[144:176] = hashlib.sha256(window).digest()
    payload[176:208] = hashlib.sha256(mel).digest()
    payload[208:213] = b"de-DE"
    struct.pack_into("<4f4I", payload, 224, .97, 2 ** -24, 2e-5, 2e-5, 1, 1, 0, 0)
    identities = [next(record["sha256"] for record in read_catalog()["files"] if record["name"] == "model.safetensors"),
                  snapshot["manifest_sha256"], manifest["dsp"]["sha256"], manifest["driver"]["sha256"],
                  manifest["dsp"]["catalog_sha256"], manifest["host"]["sha256"], records[2]["sha256"]]
    for index, value in enumerate(identities):
        payload[256 + index * 32:288 + index * 32] = bytes.fromhex(value)
    for index, record in enumerate(records):
        offset = PREFIX + index * ENTRY
        encoded = record["path"].encode("utf-8")
        payload[offset:offset + len(encoded)] = encoded
        struct.pack_into("<IIQ", payload, offset + 256, record["role"], record["flags"], record["size"])
        payload[offset + 272:offset + 304] = bytes.fromhex(record["sha256"])
    payload.extend(window); payload.extend(mel)
    return bytes(header(5, 32, 0, 0, 0, len(payload), hashlib.sha256(payload).digest())) + payload


def export(data, output):
    if output.exists() or output.parent.resolve() != data.resolve():
        raise ValueError("Package must be a new file directly under data-dir; resources remain relative to that root")
    records, manifest, snapshot = resource_records(data)
    package = package_bytes(data, records, manifest, snapshot)
    output.write_bytes(package)
    write_json(output.with_suffix(".json"), {"schema_version": 1, "file": str(output),
               "sha256": digest(output), "size": len(package), "resources": records,
               "model_backend": "OEMFastRPC-HVX-FP32", "m1_baseline": snapshot,
               "scope": "Runtime index with frozen frontend tables; original graph/model resources are not duplicated"})
    print(f"Exported {output}: {len(package)} bytes,16 resources; unchanged v17 model baseline", flush=True)


def feature_chunks(path):
    import numpy as np

    path = Path(path)
    raw = path.read_bytes()
    if len(raw) < 128 or raw[:8] != b"NM2FEAT1":
        raise ValueError("Invalid native feature trace signature")
    version, rate, mels, chunks = struct.unpack_from("<4I", raw, 8)
    samples, frames, size = struct.unpack_from("<3Q", raw, 24)
    if (version, rate, mels) != (1, 16000, 128) or samples < 160 or frames != samples // 160 or \
            not chunks or size != len(raw) - 128 or raw[80:112] != bytes.fromhex(CONTRACT_SHA) or any(raw[112:128]):
        raise ValueError("Invalid native feature trace contract/counts")
    if hashlib.sha256(raw[128:]).digest() != raw[48:80]:
        raise ValueError("Native feature trace checksum mismatch")
    result, cursor, emitted = [], 128, 0
    for index in range(chunks):
        if len(raw) - cursor < 16:
            raise ValueError("Truncated native feature record")
        step, flags, width, valid = struct.unpack_from("<4I", raw, cursor)
        wanted = 25 if index == 0 else 32
        wanted_flags = int(index == 0) | (2 if index == chunks - 1 else 0)
        if step != index or width != wanted or flags != wanted_flags or valid != min(width, frames - emitted) or not valid:
            raise ValueError("Native feature chunk accounting mismatch")
        cursor += 16
        size = width * 128 * 4
        if len(raw) - cursor < size:
            raise ValueError("Truncated native feature values")
        values = np.frombuffer(raw, dtype="<f4", count=width * 128, offset=cursor).reshape(1, width, 128)
        if not np.isfinite(values).all() or np.any(values[:, valid:] != 0):
            raise ValueError("Nonfinite native features or nonzero tail")
        result.append((values, valid)); cursor += size; emitted += valid
    if cursor != len(raw) or emitted != frames:
        raise ValueError("Trailing native feature data or missing frames")
    return result


def invoke(executable, arguments, log):
    command = [str(executable), *map(str, arguments)]
    result = subprocess.run(command, capture_output=True, timeout=300)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Native M2 command failed ({result.returncode}): {command}; see {log}")
    return result.stdout.decode("utf-8", errors="strict")

def negative_checks(executable, package, wav, output):
    def reject(arguments, label):
        result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, timeout=60)
        (output / (label + ".log")).write_bytes(result.stdout + result.stderr)
        if not result.returncode or not (result.stdout or result.stderr):
            raise ValueError(f"Native error was not explicit: {label}")

    target = output / "must-not-exist.nmfe"
    for index, supply in enumerate(("0", "-1", "8193", "", "1,", "1,,2", ",".join(["1"] * 17))):
        reject(["features", package, wav, target, supply], f"bad-partition-{index}")
        if target.exists():
            raise ValueError("Invalid partition created an output")
    original = package.read_bytes()
    for label, raw in (("package-truncated", original[:-1]), ("package-checksum", original[:])):
        raw = bytearray(raw)
        if label == "package-checksum":
            raw[-1] ^= 1
        path = output / (label + ".nmp")
        path.write_bytes(raw)
        reject(["features", path, wav, target, "5120"], label)
    raw = bytearray(original)
    struct.pack_into("<I", raw, 256 + 80, 1024)
    raw[136:168] = hashlib.sha256(raw[256:]).digest()
    path = output / "package-profile.nmp"
    path.write_bytes(raw)
    reject(["features", path, wav, target, "5120"], "package-profile")
    wave = wav.read_bytes()
    for label, raw in (("wav-truncated", wave[:-1]), ("wav-format", wave[:]),
                       ("wav-empty", b"RIFF" + struct.pack("<I", 4) + b"WAVE")):
        raw = bytearray(raw)
        if label == "wav-format":
            struct.pack_into("<H", raw, 20, 3)
        path = output / (label + ".wav")
        path.write_bytes(raw)
        reject(["features", package, path, target, "5120"], label)
    target.write_bytes(b"must not overwrite")
    reject(["features", package, wav, target, "5120"], "existing-output")
    if target.read_bytes() != b"must not overwrite":
        raise ValueError("Native frontend overwrote an existing output")
    target.unlink()
    reject(["features", package, output / "missing.wav", target, "5120"], "missing-input")
    if target.exists():
        raise ValueError("Rejected input left an output file")
    return {"passed": True, "cases": 15, "explicit_exit_and_diagnostics": True, "create_new_preserved": True}


def audit(data, executable, package, output):
    import numpy as np
    import shutil

    if output.exists():
        raise ValueError("M2 evidence directory already exists")
    output.mkdir(parents=True)
    invoke(executable, ["check", package], output / "runtime-check.log")
    reference = data / "reference-1"
    manifest = load_json(reference / "manifest.json")
    acceptance = load_json(data / "m0-acceptance.json")
    if digest(reference / "manifest.json") != acceptance["runs"][0]["manifest_sha256"]:
        raise ValueError("Frozen M0 reference manifest changed")
    tolerance = read_catalog()["profile"]["tolerances"]["frontend"]
    results = []
    for fixture in manifest["fixtures"]:
        identifier = fixture["id"]
        root = reference / identifier
        case = load_json(root / "manifest.json")
        verify_file(root / "manifest.json", {"size": (root / "manifest.json").stat().st_size,
                                            "sha256": fixture["manifest_sha256"]})
        clip = case["clip"]
        wav = data / "corpus" / clip["file"]
        verify_file(wav, clip)
        tensors = {record["name"]: record for record in case["tensors"]}
        expected = []
        for index in range(fixture["chunks"]):
            prefix = f"chunk-{index:04d}"
            record = tensors[prefix + "-features"]
            length = tensors[prefix + "-feature-valid-length"]
            verify_file(root / record["file"], record); verify_file(root / length["file"], length)
            expected.append((np.fromfile(root / record["file"], dtype=record["dtype"]).reshape(record["shape"]),
                             int(np.fromfile(root / length["file"], dtype=length["dtype"])[0])))
        baseline = None
        for partition in ("5120", "1,159,256,4096", "173,8192,37"):
            target = output / f"{identifier}-{partition.replace(',', '-')}.nmfe"
            stdout = invoke(executable, ["features", package, wav, target, partition], target.with_suffix(".log"))
            actual = feature_chunks(target)
            if len(actual) != len(expected):
                raise ValueError(f"Chunk count mismatch: {identifier}")
            violations = elements = 0
            maximum = 0.0
            for (value, valid), (wanted, wanted_valid) in zip(actual, expected):
                if value.shape != wanted.shape or valid != wanted_valid:
                    raise ValueError(f"Feature shape/valid length mismatch: {identifier}")
                error = np.abs(value.astype(np.float64) - wanted.astype(np.float64))
                violations += int(np.count_nonzero(error > tolerance["atol"] + tolerance["rtol"] * np.abs(wanted)))
                elements += value.size
                maximum = max(maximum, float(error.max()))
            if violations:
                raise ValueError(f"Frozen M0 frontend parity failed: {identifier}, {partition}, {violations}")
            if baseline is not None and target.read_bytes() != baseline:
                raise ValueError(f"Supply partition drift: {identifier}")
            baseline = target.read_bytes()
            results.append({"fixture": identifier, "partition": partition, "elements": elements,
                            "violations": violations, "max_abs_error": maximum,
                            "file": str(target), "sha256": digest(target), "native": stdout.strip()})
        print(f"PASS native M2 features: {identifier}, all3 partitions", flush=True)
    unicode_wav = output / "\u97f3\u9891-\u00e4\u00f6\u00fc.wav"
    first = load_json(reference / manifest["fixtures"][0]["id"] / "manifest.json")["clip"]
    shutil.copyfile(data / "corpus" / first["file"], unicode_wav)
    unicode_package = data / "nemotron-\u6a21\u578b-\u00e4-m2.nmp"
    if unicode_package.exists():
        raise ValueError("Unicode test package already exists")
    unicode_package.write_bytes(package.read_bytes())
    try:
        model_root = output / "\u6a21\u578b-\u00e4"
        model_root.mkdir()
        changed = bytearray(package.read_bytes())
        for index, name in ((0, "\u914d\u7f6e.json"), (2, "\u5206\u8bcd\u5668.json")):
            entry = HEADER + PREFIX + index * ENTRY
            old_path = changed[entry:entry + 256].split(b"\0", 1)[0].decode("utf-8")
            new_path = model_root / name
            shutil.copyfile(data / old_path, new_path)
            encoded = str(new_path.resolve().relative_to(data.resolve())).encode("utf-8")
            if len(encoded) >= 256:
                raise ValueError("Unicode model path exceeds format capacity")
            changed[entry:entry + 256] = bytes(256)
            changed[entry:entry + len(encoded)] = encoded
        changed[136:168] = hashlib.sha256(changed[256:]).digest()
        unicode_package.write_bytes(changed)
        invoke(executable, ["check", unicode_package], output / "unicode-model-check.log")
        target = output / "\u7279\u5f81-\u00df.nmfe"
        invoke(executable, ["features", unicode_package, unicode_wav, target, "4096"], output / "unicode.log")
        feature_chunks(target)
        deployment = output / "\u90e8\u7f72-\u00f6"
        deployment.mkdir()
        deployed = deployment / "nemotron-frontend.exe"
        shutil.copyfile(executable, deployed)
        import os
        environment = dict(os.environ)
        environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
        isolated_target = deployment / "isolated.nmfe"
        result = subprocess.run([str(deployed), "features", str(unicode_package), str(unicode_wav),
                                 str(isolated_target), "4096"], cwd=deployment, env=environment,
                                capture_output=True, timeout=300)
        (output / "isolated-deployment.log").write_bytes(result.stdout + result.stderr)
        if result.returncode or feature_chunks(isolated_target)[0][0].shape != (1, 25, 128):
            raise ValueError("Isolated Unicode no-developer-PATH frontend deployment failed")
    finally:
        unicode_package.unlink()
    errors = negative_checks(executable, package, data / "corpus" / first["file"], output)
    write_json(output / "report.json", {
        "schema_version": 1, "status": "m2_native_features_passed",
        "executable": str(executable), "executable_sha256": digest(executable),
        "package": str(package), "package_sha256": digest(package),
        "m0_acceptance_sha256": digest(data / "m0-acceptance.json"),
        "tolerances": tolerance, "fixtures": len(manifest["fixtures"]), "runs": results,
        "partition_parity": "byte-exact native feature traces", "unicode_paths_passed": True,
        "frontend_placement": "CPU", "model_inference": False,
        "runtime_resources_verified": True, "ring_capacity_samples": 512,
        "negative_native_checks": errors, "unicode_model_resources_verified": True,
        "isolated_no_developer_path_deployment_passed": True,
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    sub = parser.add_subparsers(dest="command", required=True)
    export_parser = sub.add_parser("export")
    export_parser.add_argument("--output", type=Path, required=True)
    audit_parser = sub.add_parser("audit")
    audit_parser.add_argument("--executable", type=Path, required=True)
    audit_parser.add_argument("--package", type=Path, required=True)
    audit_parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.command == "export":
        export(args.data_dir, args.output)
    else:
        audit(args.data_dir, args.executable, args.package, args.output)


if __name__ == "__main__":
    main()
