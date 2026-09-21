"""Bounded CLI checks for the freestanding translator; Python is test-only."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import statistics
import subprocess
import tempfile
import threading
import time


def dxcore_adapter_identity(luid_text):
    import ctypes
    import uuid

    pointer = ctypes.c_void_p
    guid_type = ctypes.c_ubyte * 16
    factory_id = guid_type.from_buffer_copy(uuid.UUID("78ee5945-c36e-4b13-a669-005dd11c0f06").bytes_le)
    adapter_id = guid_type.from_buffer_copy(uuid.UUID("f0db4c7f-fe5a-42a2-bd62-f2a6cf6fc83e").bytes_le)
    npu_id = guid_type.from_buffer_copy(uuid.UUID("d46140c4-add7-451b-9e56-06fe8c3b58ed").bytes_le)

    def method(instance, slot, result, *arguments):
        table = ctypes.cast(instance, ctypes.POINTER(ctypes.POINTER(pointer))).contents
        return ctypes.WINFUNCTYPE(result, pointer, *arguments)(table[slot])

    def check(result):
        if result < 0:
            raise OSError(f"DXCore HRESULT 0x{result & 0xffffffff:08x}")

    library = ctypes.WinDLL("dxcore.dll")
    create = library.DXCoreCreateAdapterFactory
    create.argtypes = (pointer, ctypes.POINTER(pointer))
    create.restype = ctypes.c_long
    factory, adapter = pointer(), pointer()
    check(create(factory_id, ctypes.byref(factory)))
    try:
        high, low = (int(part, 16) for part in luid_text.split("_"))
        luid = ctypes.c_uint64((high << 32) | low)
        check(method(factory, 4, ctypes.c_long, pointer, pointer, ctypes.POINTER(pointer))(
            factory, ctypes.byref(luid), adapter_id, ctypes.byref(adapter)))
        size = ctypes.c_size_t()
        check(method(adapter, 7, ctypes.c_long, ctypes.c_uint32, ctypes.POINTER(ctypes.c_size_t))(
            adapter, 2, ctypes.byref(size)))
        description = ctypes.create_string_buffer(size.value)
        check(method(adapter, 6, ctypes.c_long, ctypes.c_uint32, ctypes.c_size_t, pointer)(
            adapter, 2, size.value, description))
        is_npu = method(adapter, 4, ctypes.c_bool, pointer)(adapter, npu_id)
        return dict(luid=luid_text, description=description.value.decode("utf-8"), npu_attribute=is_npu)
    finally:
        if adapter:
            method(adapter, 2, ctypes.c_ulong)(adapter)
        method(factory, 2, ctypes.c_ulong)(factory)


def target_profile_requests(record):
    if record.get("exit_code") != 0 or record.get("timed_out"):
        raise ValueError("Failed or timed-out capture")
    text = record["stderr"]
    instrumented = "PROFILE_REQUEST_BEGIN " in text
    if instrumented != (record.get("mode") in ("timing", "basic", "detailed")):
        raise ValueError("Capture mode does not match instrumentation")
    phase_names = re.findall(r"^PROFILE (\w+) us=\d+ calls=\d+$", text, re.M)
    requests = []
    current = None
    sample = None

    def fresh(identity):
        return dict(id=identity, repeat=record.get("repeat"), capture_mode=record["mode"], tokens=[], partitions=[], phases={})

    for line in text.splitlines():
        if match := re.fullmatch(r"PROFILE_REQUEST_BEGIN id=(\d+)", line):
            if current is not None:
                raise ValueError("Unfinished profiling request")
            current = fresh(int(match[1]))
        elif match := re.fullmatch(r"generated token: (\d+)", line):
            if current is None:
                if instrumented:
                    raise ValueError("Token outside profiling request")
                current = fresh(len(requests) + 1)
            current["tokens"].append(int(match[1]))
        elif match := re.fullmatch(r"request us: (\d+)", line):
            if current is None:
                raise ValueError("Request has no tokens")
            current["request_us"] = int(match[1])
            if not instrumented:
                requests.append(current)
                current = None
        elif match := re.fullmatch(r"PROFILE_PARTITION (\d+) (prefill|decode) us=(\d+)", line):
            if current is None:
                raise ValueError("Partition outside request")
            current["partitions"].append(dict(partition=int(match[1]), mode=match[2], us=int(match[3]), events=[]))
            sample = None
        elif match := re.fullmatch(r"QNN_PARTITION (\d+) (prefill|decode)", line):
            if current is None or not current["partitions"]:
                raise ValueError("QNN sample without partition timing")
            sample = current["partitions"][-1]
            if sample["partition"] != int(match[1]) or sample["mode"] != match[2] or sample.get("sampled"):
                raise ValueError("QNN sample attribution mismatch")
            sample["sampled"] = True
        elif match := re.fullmatch(r"QNN_EVENT (.+) type=(\d+) unit=(\d+) value=(\d+) depth=(\d+)", line):
            if sample is None:
                raise ValueError("Unattributed QNN event")
            sample["events"].append(dict(name=match[1], type=int(match[2]), unit=int(match[3]), value=int(match[4]), depth=int(match[5])))
        elif line.startswith("QNN_SAMPLE "):
            sample = None
        elif line.startswith("PROFILE_REQUEST_PHASE "):
            values = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
            if current is None or values["id"] != current["id"] or values["phase"] >= len(phase_names):
                raise ValueError("Invalid request phase")
            name = phase_names[values["phase"]]
            if name in current["phases"]:
                raise ValueError("Duplicate request phase")
            current["phases"][name] = dict(us=values["us"], calls=values["calls"])
        elif line.startswith("PROFILE_REQUEST_END "):
            values = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
            if current is None or values["id"] != current["id"] or "request_us" not in current:
                raise ValueError("Unmatched request end")
            current.update(values)
            requests.append(current)
            current = sample = None
    expected = 1 if record.get("mode") == "detailed" else 3
    if current is not None or len(requests) != expected:
        raise ValueError("Expected three resident requests or one isolated detailed request")
    if text.count("performance vote: 0") != expected or text.count("performance release: 0") != expected or "cleanup errors: 0" not in text:
        raise ValueError("Invalid power vote or cleanup lifecycle")
    for identity, request in enumerate(requests, 1):
        if request["id"] != identity or not request["tokens"] or request["request_us"] <= 0:
            raise ValueError("Invalid request identity, tokens or duration")
        if instrumented:
            frames = request["partitions"]
            if not frames or len(frames) % 3 or not request["phases"]:
                raise ValueError("Incomplete partition/phase capture")
            for offset in range(0, len(frames), 3):
                triplet = frames[offset:offset + 3]
                if [frame["partition"] for frame in triplet] != [0, 1, 2] or len({frame["mode"] for frame in triplet}) != 1:
                    raise ValueError("Invalid partition order")
            for mode in ("prefill", "decode"):
                phase = request["phases"].get(mode + "_execute", {})
                matching = [frame for frame in frames if frame["mode"] == mode]
                if phase.get("calls") != len(matching) // 3 or phase.get("us", -1) < sum(frame["us"] for frame in matching):
                    raise ValueError("Partition frames do not account for execute phase")
            if record["mode"] in ("basic", "detailed"):
                sampled = {(frame["partition"], frame["mode"]) for frame in frames if frame.get("sampled")}
                if sampled != {(partition, mode) for partition in range(3) for mode in ("prefill", "decode")}:
                    raise ValueError("Missing per-partition QNN samples")
    return requests


def target_profile_summary(records):
    modes = ("release", "timing", "basic")
    if any(record.get("mode") == "detailed" or record.get("operator_requested") for record in records):
        modes += ("detailed",)
    grouped = {mode: [record for record in records if record.get("mode") == mode] for mode in modes}
    repeats = len(grouped["release"])
    if repeats < 1 or len(records) != repeats * len(modes):
        raise ValueError("Incomplete profiling campaign")
    parsed = {}
    reference = None
    source = records[0].get("source")
    for mode in modes:
        if len(grouped[mode]) != repeats or sorted(record["repeat"] for record in grouped[mode]) != list(range(repeats)):
            raise ValueError("Missing or duplicate repeat")
        parsed[mode] = []
        for record in grouped[mode]:
            if record.get("expected_repeats", repeats) != repeats or record.get("operator_requested", "detailed" in modes) != ("detailed" in modes):
                raise ValueError("Capture does not fulfill the requested campaign")
            if record.get("source") != source:
                raise ValueError("Mixed input workloads")
            if any(record.get(key) != grouped[mode][0].get(key) for key in ("binary_sha256", "arguments")):
                raise ValueError("Binary or arguments changed between repeats")
            requests = target_profile_requests(record)
            if mode == "detailed":
                if requests[0]["tokens"] != reference[1][1] or not record["stdout"] or not reference[0].endswith(record["stdout"] * 2):
                    raise ValueError("Isolated detailed token/output parity failed")
                parsed[mode].extend(requests)
                continue
            signature = (record["stdout"], [request["tokens"] for request in requests])
            if reference is None:
                reference = signature
            if signature != reference or requests[1]["tokens"] != requests[2]["tokens"]:
                raise ValueError("Cross-mode token/output parity failed")
            parsed[mode].extend(requests[1:])

    def distribution(values):
        values = sorted(values)
        if not values:
            raise ValueError("Empty timing population")
        return dict(count=len(values), min=min(values), median=statistics.median(values),
                    p95=values[(95 * len(values) + 99) // 100 - 1], max=max(values))

    latency = {mode: distribution([request["request_us"] for request in parsed[mode]]) for mode in modes}
    requests = parsed["timing"]
    frames = [frame for request in requests for frame in request["partitions"]]
    partitions = {mode: {str(partition): distribution([frame["us"] for frame in frames if frame["mode"] == mode and frame["partition"] == partition])
                        for partition in range(3)} for mode in ("prefill", "decode")}
    phases = {}
    elapsed = sum(request["us"] for request in requests)
    for name in requests[0]["phases"]:
        values = [request["phases"][name]["us"] for request in requests]
        phases[name] = dict(mean_us=statistics.mean(values), percent=100 * sum(values) / elapsed)
    phase_sum = sum(phase["mean_us"] for phase in phases.values())
    if phase_sum > elapsed / len(requests) + 1000:
        raise ValueError("Phase accounting exceeds request duration")
    groups = {mode: Counter() for mode in ("prefill", "decode")}
    layers = Counter()
    samples = []
    for request in parsed["basic"] + parsed.get("detailed", []):
        for frame in request["partitions"]:
            if not frame.get("sampled"):
                continue
            measurements = {event["type"]: event["value"] for event in frame["events"] if event["unit"] == 1 and event["depth"] == 0}
            if 3012 not in measurements:
                raise ValueError("Missing sampled accelerator timing")
            samples.append(dict(capture_mode=request["capture_mode"], repeat=request["repeat"], request=request["id"],
                                partition=frame["partition"], mode=frame["mode"], graph_wall_us=frame["us"],
                                accelerator_excluding_wait_us=measurements[3012],
                                yield_wait_us=measurements.get(3007), vtcm_acquire_us=measurements.get(3010),
                                resource_power_up_us=measurements.get(3011)))
            for event in frame["events"]:
                if request["capture_mode"] == "detailed" and (event["type"], event["unit"], event["depth"]) == (404, 3, 1):
                    name = event["name"].split(":OpId_", 1)[0]
                    groups[frame["mode"]][re.sub(r"^layer-\d+\.", "", name)] += event["value"]
                    if frame["mode"] == "decode" and (layer := re.match(r"layer-(\d+)\.", name)):
                        layers[int(layer[1])] += event["value"]
            if request["capture_mode"] == "detailed" and not any((event["type"], event["unit"], event["depth"]) == (404, 3, 1) for event in frame["events"]):
                raise ValueError("Missing partition operator cycles")
    if not samples or ("detailed" in parsed and any(not group for group in groups.values())):
        raise ValueError("Missing detailed operator samples")
    ranked = {mode: [dict(name=name, cycles=cycles, percent_of_sampled_node_cycles=100 * cycles / sum(group.values()))
                     for name, cycles in group.most_common()] for mode, group in groups.items()}
    return dict(complete=True, source=source,
                campaign_contract_verified=all("expected_repeats" in record and "operator_requested" in record for record in records),
                binaries={mode: dict(path=grouped[mode][0].get("binary"), sha256=grouped[mode][0].get("binary_sha256")) for mode in modes},
                operator_capture="isolated_first_request" if "detailed" in parsed else "not_requested",
                repeats_per_mode=repeats, warmup_requests_excluded=repeats * 3, token_parity=True,
                request_us=latency, overhead_percent={mode: 100 * (latency[mode]["median"] / latency["release"]["median"] - 1) for mode in ("timing", "basic")},
                timing_phases=phases, timing_unassigned_mean_us=elapsed / len(requests) - phase_sum,
                partition_us=partitions, operators=ranked, decode_layer_cycles=dict(layers.most_common()), qnn_samples=samples,
                limitations=["Node cycles are sampled attribution, not wall-time shares; GELU may include a fused gate projection.",
                             "Detailed timings include profiling overhead; use release for performance and timing mode for phase/partition costs.",
                             "Optional detailed captures use one fresh-process request, without warmup; their latency is not comparable to resident requests and is excluded from overhead estimates.",
                             "Accelerator excluding wait can still include internal stalls; no HMX/HVX occupancy, actual clocks, DDR bandwidth or PMU stall counters are measured.",
                             "Nearest-rank p95 and overhead are descriptive small-sample statistics, not significance tests."])


def binding_comparison_summary(records):
    timings = {}
    reference = None
    for variant in ("baseline", "candidate"):
        captures = [record for record in records if record.get("variant") == variant]
        if len(captures) != 3 or sorted(record["repeat"] for record in captures) != [0, 1, 2]:
            raise ValueError("Incomplete binding comparison")
        values = []
        for record in captures:
            if "TranslateGemma 4B W8" not in record["stderr"] or record["stderr"].count("bundle restore: 0") != 3:
                raise ValueError("Binding comparison requires three-partition W8 contexts")
            requests = target_profile_requests(record)
            signature = (record["stdout"], [request["tokens"] for request in requests])
            if reference is None:
                reference = signature
            if signature != reference or requests[1]["tokens"] != requests[2]["tokens"]:
                raise ValueError("Candidate changed translation output or tokens")
            values.extend(request["request_us"] for request in requests[1:])
        timings[variant] = dict(count=len(values), median=statistics.median(values), min=min(values), max=max(values), values=values)
    return dict(complete=True, token_parity=True, warmup_requests_excluded=6, request_us=timings,
                reduction_percent=100 * (1 - timings["candidate"]["median"] / timings["baseline"]["median"]),
                nonoverlapping_ranges=timings["candidate"]["max"] < timings["baseline"]["min"])


def target_profile_selftest():
    def fixture(mode, repeat):
        lines = ["TranslateGemma 4B W8 NPU prototype", "bundle restore: 0", "bundle restore: 0", "bundle restore: 0"]
        for identity in range(1, 2 if mode == "detailed" else 4):
            if mode != "release":
                lines.append(f"PROFILE_REQUEST_BEGIN id={identity}")
            lines.append("performance vote: 0")
            if mode != "release":
                for phase in ("prefill", "decode"):
                    for partition in range(3):
                        lines.append(f"PROFILE_PARTITION {partition} {phase} us=100")
                        if mode in ("basic", "detailed"):
                            lines.extend([f"QNN_PARTITION {partition} {phase}",
                                          "QNN_EVENT Accelerator type=3012 unit=1 value=90 depth=0"])
                        if mode == "detailed":
                            lines.append(f"QNN_EVENT layer-{partition}.mlp:OpId_1 type=404 unit=3 value=100 depth=1")
                    if mode in ("basic", "detailed"):
                        lines.append(f"QNN_SAMPLE {phase}")
            lines.extend(["generated token: 42", "generated token: 106", "performance release: 0", "request us: 1000"])
            if mode != "release":
                lines.extend([f"PROFILE_REQUEST_PHASE id={identity} phase=0 us=300 calls=1",
                          f"PROFILE_REQUEST_PHASE id={identity} phase=1 us=300 calls=1",
                              f"PROFILE_REQUEST_END id={identity} us=1000 first_output_us=300"])
            lines.extend(["PROFILE prefill_execute us=900 calls=3", "PROFILE decode_execute us=900 calls=3", "cleanup errors: 0"])
        return dict(mode=mode, repeat=repeat, expected_repeats=2, operator_requested=True,
                exit_code=0, stdout="ok\n" * (1 if mode == "detailed" else 3), stderr="\n".join(lines))

    records = [fixture(mode, repeat) for repeat in range(2) for mode in ("release", "timing", "basic", "detailed")]
    summary = target_profile_summary(records)
    assert summary["request_us"]["release"]["count"] == 4
    assert summary["operators"]["decode"][0]["cycles"] == 600
    assert summary["partition_us"]["decode"]["2"]["median"] == 100
    assert len(summary["qnn_samples"]) == 36 and summary["timing_unassigned_mean_us"] == 400
    baseline = target_profile_summary([dict(record, operator_requested=False) for record in records if record["mode"] != "detailed"])
    assert baseline["operator_capture"] == "not_requested" and not baseline["operators"]["decode"]
    corruptions = [("QNN_PARTITION 0 prefill", "QNN_PARTITION 1 prefill"),
                   ("PROFILE_REQUEST_END id=1", "BROKEN_END id=1"),
                   ("generated token: 42", "generated token: 43"),
                   ("performance release: 0", "performance release: 1"),
                   ("type=3012 unit=1", "type=3012 unit=3"),
                   ("PROFILE_PARTITION 1 decode", "PROFILE_PARTITION 0 decode"),
                   ("phase=0 us=300", "phase=9 us=300"),
                   ("us=300 calls=1", "us=300 calls=2")]
    for before, after in corruptions:
        changed = [dict(record) for record in records]
        changed[-1]["stderr"] = changed[-1]["stderr"].replace(before, after)
        try:
            target_profile_summary(changed)
        except ValueError:
            continue
        raise AssertionError(f"Accepted invalid capture: {before}")
    invalid = [records[:-1], [], [dict(record) for record in records],
               [dict(record) for record in records], [dict(record) for record in records],
               [dict(record) for record in records], [dict(record) for record in records],
               records[:4], [record for record in records if record["mode"] != "detailed"]]
    invalid[2][-1]["repeat"] = 0
    invalid[3][-1]["exit_code"] = 1
    invalid[4][-1]["source"] = "different workload"
    invalid[5][-1]["binary_sha256"] = "changed"
    invalid[6][-1]["timed_out"] = True
    for changed in invalid:
        try:
            target_profile_summary(changed)
        except ValueError:
            continue
        raise AssertionError("Accepted incomplete or inconsistent campaign")
    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "capture.json"
        command = [os.sys.executable, str(Path(__file__).resolve()), "--target-profile", "run", "--report", str(report)]
        failed = subprocess.run(command + ["--profile-input", str(Path(directory) / "missing.txt")], capture_output=True, timeout=10)
        assert failed.returncode != 0 and not report.exists()
        report.write_text("preserve", encoding="utf-8")
        failed = subprocess.run(command, capture_output=True, timeout=10)
        assert failed.returncode != 0 and report.read_text(encoding="utf-8") == "preserve"
        report.write_text(json.dumps(records), encoding="utf-8")
        offline = subprocess.run(command[:-4] + ["--target-profile", "summary", "--report", str(report)], capture_output=True, timeout=10)
        assert offline.returncode == 0, offline.stderr
        assert json.loads(report.with_suffix(".summary.json").read_text(encoding="utf-8"))["complete"]
    comparison = []
    for repeat in range(3):
        for variant in ("baseline", "candidate"):
            record = dict(fixture("release", repeat), variant=variant)
            if variant == "candidate":
                record["stderr"] = record["stderr"].replace("request us: 1000", "request us: 900")
            comparison.append(record)
    compared = binding_comparison_summary(comparison)
    assert abs(compared["reduction_percent"] - 10) < .001 and compared["nonoverlapping_ranges"]
    wrong_precision = [dict(record) for record in comparison]
    wrong_precision[-1]["stderr"] = wrong_precision[-1]["stderr"].replace("4B W8", "4B W4")
    try:
        binding_comparison_summary(wrong_precision)
    except ValueError:
        pass
    else:
        raise AssertionError("Binding comparison accepted different precision")
    comparison[-1]["stdout"] = "different\n"
    try:
        binding_comparison_summary(comparison)
    except ValueError:
        pass
    else:
        raise AssertionError("Binding comparison accepted changed translation")
    print("PASS targeted profiler: warmup exclusion, attribution, 17 corruption checks, 3 offline CLI checks and binding comparison", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[2] / "build/translate.exe")
    parser.add_argument("--hardware", action="store_true")
    parser.add_argument("--profile", action="store_true", help="Three bounded runs of translate-profile.exe")
    parser.add_argument("--w8-profile", action="store_true", help="Partition timing, resident requests and scoped HTP power comparison")
    parser.add_argument("--report", type=Path, help="Explicit results path, to preserve baseline measurements")
    parser.add_argument("--w8-summary", type=Path, help="Summarize an existing W8 profile report without running inference")
    parser.add_argument("--w8-performance", action="store_true", help="Uninstrumented interleaved W8 latency and vote lifecycle checks")
    parser.add_argument("--embedding-load", action="store_true", help="Interleaved serial/overlapped embedding verification benchmark")
    parser.add_argument("--document-profile", choices=("describe", "release", "timing", "basic", "counters", "summary"), help="Resident roughly 200-word translation and accelerator activity profile")
    parser.add_argument("--target-profile", choices=("run", "summary", "selftest"), help="Dependency-free targeted profile, offline report, or parser checks")
    parser.add_argument("--profile-input", type=Path, help="Target profiler German UTF-8 single-line source (default: Hase sentence)")
    parser.add_argument("--profile-repeats", type=int, choices=range(1, 6), default=3, help="Target profiler process repetitions per mode")
    parser.add_argument("--profile-operators", action="store_true", help="Add isolated single-request detailed node-cycle captures; never reuse a detailed profile across requests")
    parser.add_argument("--streaming", action="store_true", help="Quiet/streaming integration and first-byte timing")
    parser.add_argument("--bundle", action="store_true", help="Shared-weight candidate parity and latency")
    parser.add_argument("--performance", action="store_true", help="Interleaved scoped QNN performance-vote benchmark")
    parser.add_argument("--selection-cache", action="store_true", help="Selector cache corruption, parity and interleaved timing")
    parser.add_argument("--load-benchmark", action="store_true", help="Interleaved serial/overlapped bundle loading")
    parser.add_argument("--documents", action="store_true", help="Long-text, stdin and strict-limit regression")
    parser.add_argument("--bindings", type=Path)
    parser.add_argument("--compare-bindings", type=Path, help="Interleaved W8 candidate vs baseline bindings with release timing and decode parity")
    parser.add_argument("--tokenizer", type=Path)
    parser.add_argument("--partitions", action="store_true")
    parser.add_argument("--quality", choices=("smoke", "diagnostic"), help="Bounded NPU candidate evaluation; outputs require semantic review")
    parser.add_argument("--case", action="append", default=[], help="Filter diagnostic case IDs")
    options = parser.parse_args()
    if options.target_profile == "selftest":
        target_profile_selftest()
        return
    if options.target_profile == "summary":
        report = options.report or options.binary.resolve().parent / "translate-target-profile.json"
        summary = target_profile_summary(json.loads(report.read_text(encoding="utf-8")))
        output = report.with_suffix(".summary.json")
        output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: summary[key] for key in ("request_us", "overhead_percent", "timing_phases", "limitations")}, indent=2))
        print("DECODE_TOP " + json.dumps(summary["operators"]["decode"][:8]))
        print("PASS complete capture, token parity and phase attribution; " + str(output))
        return
    if options.w8_summary:
        records = json.loads(options.w8_summary.read_text(encoding="utf-8"))
        if records and records[0]["name"].startswith("embedding-"):
            summary = []
            for serial in (0, 1):
                subset = [record for record in records if record["name"].startswith(f"embedding-{serial}-")]
                assert len(subset) == 5 and all(record["exit_code"] == 0 for record in subset)
                summary.append(dict(serial=bool(serial),
                    wall_seconds=statistics.median(record["wall_seconds"] for record in subset),
                    first_byte_seconds=statistics.median(record["first_byte_seconds"] for record in subset),
                    phases_us={name: statistics.median(record["phases"][name]["us"] for record in subset)
                               for name in subset[0]["phases"]}))
                print(json.dumps(summary[-1]), flush=True)
            if options.report:
                options.report.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
            return
        for power in (0, 1):
            subset = [record for record in records if record["name"].startswith(f"resident-{power}-")]
            assert len(subset) == 3
            phases = {name: statistics.median(record["phases"][name]["us"] for record in subset)
                      for name in subset[0]["phases"]}
            print(json.dumps(dict(power=power,
                wall_seconds=statistics.median(record["wall_seconds"] for record in subset),
                request_us=statistics.median(value for record in subset for value in record["requests_us"]),
                decode_step_us=statistics.median(record["phases"]["decode_execute"]["us"] / record["phases"]["decode_execute"]["calls"] for record in subset),
                partition_decode_us=[statistics.median(int(elapsed) for record in subset for index, mode, elapsed in record["partitions"]
                    if int(index) == partition and mode == "decode") for partition in range(3)], phases=phases)), flush=True)
        for record in records:
            if record["name"] == "partition-qnn-detail":
                print("\n".join(line for line in record["stderr"].splitlines()
                                if "QNN_PARTITION" in line or "Accelerator" in line or "RPC" in line), flush=True)
                mode = None
                operators = Counter()
                formats = Counter()
                for line in record["stderr"].splitlines():
                    if line.startswith("QNN_PARTITION "):
                        mode = line.split()[-1]
                    event = re.fullmatch(r"QNN_EVENT (.+) type=(\d+) unit=(\d+) value=(\d+) depth=(\d+)", line)
                    if event and mode == "decode":
                        name, kind, unit, value, depth = event.groups()
                        formats[(kind, unit, depth)] += 1
                        if depth == "1" and kind == "404" and unit == "3":
                            operators[name] += int(value)
                print("DECODE_EVENT_FORMATS " + str(dict(formats)), flush=True)
                print("DECODE_TOP_EVENTS " + json.dumps(operators.most_common(30)), flush=True)
                grouped = Counter()
                for name, cycles in operators.items():
                    grouped[re.sub(r"^layer-\d+\.", "", name.split(":OpId_", 1)[0])] += cycles
                total_cycles = sum(operators.values())
                groups = [
                    dict(name=name, cycles=cycles, percent=100 * cycles / total_cycles)
                    for name, cycles in grouped.most_common()]
                print("DECODE_GROUPS " + json.dumps(groups), flush=True)
                if options.report:
                    options.report.write_text(json.dumps(dict(traced_decode_cycles=total_cycles, groups=groups), indent=2) + "\n", encoding="utf-8")
        return
    binary = options.binary.resolve()
    if options.document_profile == "summary":
        records = {mode: json.loads((binary.parent / f"translate-document-{mode}-results.json").read_text(encoding="utf-8"))[0]
                   for mode in ("release", "timing", "basic", "counters")}
        reference = records["release"]["stdout"].splitlines()[1]
        release_tokens = re.findall(r"generated token: (\d+)", records["release"]["stderr"])
        assert release_tokens == re.findall(r"generated token: (\d+)", records["timing"]["stderr"])
        basic_tokens = re.findall(r"generated token: (\d+)", records["basic"]["stderr"])
        assert basic_tokens == release_tokens[:len(basic_tokens)]
        counter_tokens = re.findall(r"generated token: (\d+)", records["counters"]["stderr"])
        assert counter_tokens == basic_tokens
        for record in records.values():
            assert record["exit_code"] == 0 and not record["counter_diagnostics"]
            assert all(output == reference for output in record["stdout"].splitlines()[1:])
        requests = records["timing"]["requests"][1:]
        assert len(requests) == 2
        for request in requests:
            assert len(request["pieces"]) == 12 and all(piece["stopped"] == 1 for piece in request["pieces"])
            assert len(request["partitions"]) == 3 * (request["phases"]["prefill_execute"]["calls"] + request["phases"]["decode_execute"]["calls"])
        elapsed = statistics.mean(request["us"] for request in requests)
        phases = {name: statistics.mean(request["phases"][name]["us"] for request in requests) for name in requests[0]["phases"]}
        partitions = []
        for mode in ("prefill", "decode"):
            for partition in range(3):
                values = [part["us"] for request in requests for part in request["partitions"] if part["mode"] == mode and part["partition"] == partition]
                partitions.append(dict(mode=mode, partition=partition, calls=len(values), mean_us=statistics.mean(values),
                    median_us=statistics.median(values), p95_us=sorted(values)[int((len(values) - 1) * .95)]))
        samples = records["basic"]["requests"][1]["partitions"]
        accelerator = []
        for mode in ("prefill", "decode"):
            selected = [part for part in samples if part["mode"] == mode and "accelerator_us" in part]
            assert len(selected) == 36
            active = sum(part["accelerator_us"] for part in selected)
            wall = sum(part["us"] for part in selected)
            accelerator.append(dict(mode=mode, calls=len(selected), active_us=active, graph_wall_us=wall,
                                    active_fraction_of_sampled_graph_wall=active / wall))
        counters = {}
        for mode, record in records.items():
            rows = []
            for line in Path(record["counter_file"]).read_text(encoding="utf-8-sig").splitlines():
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
            identities = sorted({process["Id"] for row in rows for process in row.get("processes", [])})
            instances = {sample["InstanceName"] for row in rows for sample in row["samples"]}
            matched = sorted(instance for instance in instances if any(instance.startswith(f"pid_{pid}_") for pid in identities))
            counters[mode] = dict(samples=len(rows), engine_types=sorted({instance.split("engtype_", 1)[-1] for instance in instances}),
                adapter_luids=sorted({re.search(r"luid_(.+?)_phys", instance)[1] for instance in instances}),
                translator_pids=identities, translator_instances=matched)
            if mode == "counters":
                assert len(matched) == 1
                counters[mode]["adapter"] = dxcore_adapter_identity(re.search(r"luid_(.+?)_phys", matched[0])[1])
                request = record["requests"][1]
                values = []
                for previous, row in zip(rows, rows[1:]):
                    if previous["tick"] < request["start_tick"] or row["tick"] > request["end_tick"]:
                        continue
                    engines = [sample["CookedValue"] for sample in row["samples"]
                               if sample["InstanceName"] in matched and sample["Status"] in (0, 1)]
                    assert len(engines) == 1
                    values.append(dict(seconds=(row["tick"] - request["start_tick"]) / request["frequency"],
                                       interval_seconds=(row["tick"] - previous["tick"]) / request["frequency"], percent=engines[0]))
                assert len(values) >= 30
                busy = [value["percent"] for value in values]
                counters[mode]["document"] = dict(samples=values, sample_count=len(values), mean_percent=statistics.mean(busy),
                    duration_weighted_mean_percent=sum(value["percent"] * value["interval_seconds"] for value in values) / sum(value["interval_seconds"] for value in values),
                    out_of_range_samples=sum(value < 0 or value > 100 for value in busy),
                    median_percent=statistics.median(busy), min_percent=min(busy), max_percent=max(busy),
                    p10_percent=sorted(busy)[int((len(busy)-1)*.1)], p90_percent=sorted(busy)[int((len(busy)-1)*.9)])
        pieces = [dict(piece=index + 1, prompt_tokens=requests[0]["pieces"][index]["prompt_tokens"],
                       generated_tokens_including_stop=requests[0]["pieces"][index]["generated_tokens"],
                       seconds=statistics.mean(request["piece_times"][index]["us"] for request in requests) / 1e6)
                  for index in range(12)]
        quartiles = []
        for request in requests:
            steps = [sum(request["partitions"][offset + index]["us"] for index in range(3))
                     for offset in range(0, len(request["partitions"]), 3) if request["partitions"][offset]["mode"] == "decode"]
            quartiles.append([statistics.mean(steps[len(steps) * index // 4:len(steps) * (index + 1) // 4]) for index in range(4)])
        generated = sum(piece["generated_tokens"] for piece in requests[0]["pieces"])
        stop_tokens = sum(piece["stopped"] for piece in requests[0]["pieces"])
        summary = dict(source_words=records["release"]["source_words"], output_words=len(reference.split()),
            release_request_seconds=[value / 1e6 for value in records["release"]["requests_us"][1:]],
            timing_request_seconds=elapsed / 1e6, first_sentence_seconds=statistics.mean(request["first_output_us"] for request in requests) / 1e6,
            generated_tokens_including_stops=generated, content_tokens=generated - stop_tokens, prompt_tokens=sum(piece["prompt_tokens"] for piece in requests[0]["pieces"]),
            phases={name: dict(seconds=value / 1e6, percent=value * 100 / elapsed) for name, value in phases.items()},
            unassigned_seconds=(elapsed - sum(phases.values())) / 1e6,
            main_graph_wall_fraction=(phases["prefill_execute"] + phases["decode_execute"]) / elapsed,
            partitions=partitions, pieces=pieces, decode_quartile_mean_us=quartiles, accelerator_samples=accelerator, windows_counters=counters)
        report = options.report or binary.parent / "translate-document-summary.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        if report.parent.resolve() != binary.parent:
            for mode, record in records.items():
                shutil.copy2(binary.parent / f"translate-document-{mode}-results.json", report.parent)
                shutil.copy2(record["counter_file"], report.parent)
        report.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(summary, indent=2), flush=True)
        print("PASS cross-run token parity, phase accounting, partition counts and accelerator samples", flush=True)
        return
    if options.profile or options.w8_profile or options.document_profile in ("timing", "basic", "counters"):
        binary = binary.with_name("translate-profile.exe")
    results = []

    def run(name, arguments, expected_code, expected_text=None, input_bytes=None, observe=False):
        arguments = list(arguments)
        if options.bindings:
            arguments = ["--bindings", str(options.bindings.resolve()), *arguments]
        if options.tokenizer:
            arguments = ["--tokenizer", str(options.tokenizer.resolve()), *arguments]
        if options.partitions:
            arguments = ["--partitions", *arguments]
        started = time.perf_counter()
        arrivals = []
        if observe:
            assert input_bytes is None
            with tempfile.TemporaryFile() as errors:
                with subprocess.Popen([str(binary), *arguments], cwd=binary.parent.parent,
                                      stdout=subprocess.PIPE, stderr=errors, bufsize=0) as process:
                    watchdog = threading.Timer(240 if options.documents else 120, process.kill)
                    watchdog.start()
                    try:
                        chunks = []
                        while chunk := process.stdout.read(1):
                            chunks.append(chunk)
                            arrivals.append(time.perf_counter() - started)
                        code = process.wait(timeout=10)
                    finally:
                        watchdog.cancel()
                    errors.seek(0)
                    result = subprocess.CompletedProcess(process.args, code, b"".join(chunks), errors.read())
        else:
            try:
                result = subprocess.run([str(binary), *arguments], cwd=binary.parent.parent,
                                        capture_output=True, input=input_bytes, timeout=600 if options.document_profile or options.target_profile else 240 if options.documents else 120, check=False)
            except subprocess.TimeoutExpired as error:
                results.append(dict(name=name, arguments=arguments, exit_code=124,
                                    stdout=(error.stdout or b"").decode("utf-8", errors="replace"),
                                    stderr=(error.stderr or b"").decode("utf-8", errors="replace"),
                                    wall_seconds=time.perf_counter() - started, timed_out=True))
                raise
        output = result.stdout.decode("utf-8", errors="strict")
        diagnostics = result.stderr.decode("utf-8", errors="strict")
        record = dict(name=name, arguments=arguments, exit_code=result.returncode,
                      stdout=output, stderr=diagnostics, wall_seconds=time.perf_counter() - started)
        results.append(record)
        if arrivals:
            record["first_byte_seconds"] = arrivals[0]
            record["last_byte_seconds"] = arrivals[-1]
            record["byte_arrivals_seconds"] = arrivals
        print(json.dumps({key: value for key, value in record.items() if key not in ("stderr", "byte_arrivals_seconds")}, ensure_ascii=True), flush=True)
        assert result.returncode in (expected_code if isinstance(expected_code, tuple) else (expected_code,)), name
        if expected_text is not None:
            assert output == expected_text, (name, output)
        if "--quiet" in arguments:
            assert not diagnostics, (name, diagnostics)
        assert "DSP_INFO" not in output and "Gemma Stage" not in output, (name, output)
        if expected_code == 1 and not input_bytes:
            assert "prompt restore:" not in diagnostics, name
        if "prompt restore:" in diagnostics or "bundle restore:" in diagnostics:
            assert "cleanup errors: 0" in diagnostics, name
        return record

    try:
        if options.compare_bindings:
            if any((options.profile, options.w8_profile, options.document_profile, options.target_profile,
                    options.quality, options.hardware, options.embedding_load, options.w8_performance,
                    options.documents, options.bundle, options.performance, options.selection_cache,
                    options.load_benchmark, options.streaming)):
                raise ValueError("Run binding comparison separately from other modes")
            report = options.report or binary.parent / "translate-binding-comparison.json"
            report.parent.mkdir(parents=True, exist_ok=True)
            if report.exists() or report.with_suffix(".summary.json").exists():
                raise ValueError("Choose a new binding comparison report")
            bindings = dict(baseline=(options.bindings or binary.parent / "gemma-block/prompt-512.gmb").resolve(),
                            candidate=options.compare_bindings.resolve())
            if bindings["baseline"] == bindings["candidate"] or not all(path.is_file() for path in bindings.values()):
                raise ValueError("Comparison needs two distinct existing bindings")
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            common = ["--from", "de", "--to", "en", "--max-tokens", "96"]
            for repeat in range(3):
                for variant in ("baseline", "candidate") if repeat % 2 == 0 else ("candidate", "baseline"):
                    options.bindings = bindings[variant]
                    record = run(f"binding-{variant}-{repeat}", [*common, "--batch"], 0,
                                 input_bytes=("Guten Morgen.\n" + (sentence + "\n") * 2).encode("utf-8"))
                    record.update(mode="release", repeat=repeat, variant=variant, binding=str(options.bindings))
                    target_profile_requests(record)
                    report.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            summary = binding_comparison_summary(results)
            for variant, binding in bindings.items():
                options.bindings = binding
                record = run("binding-parity-" + variant, [*common, "--verify-decode", sentence], 0)
                if "PASS decode KV/logits parity position:" not in record["stderr"]:
                    raise ValueError("Missing decode parity check")
            summary.update(decode_parity=True, binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                           bindings={variant: str(path) for variant, path in bindings.items()})
            report.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(summary, indent=2), flush=True)
            return
        if options.target_profile == "run":
            if any((options.profile, options.w8_profile, options.document_profile, options.quality, options.hardware,
                    options.embedding_load, options.w8_performance, options.documents, options.bundle,
                    options.performance, options.selection_cache, options.load_benchmark, options.streaming)):
                raise ValueError("Run the target profiler separately from other test modes")
            report = options.report or binary.parent / "translate-target-profile.json"
            report.parent.mkdir(parents=True, exist_ok=True)
            summary_path = report.with_suffix(".summary.json")
            if report.exists() or summary_path.exists():
                raise ValueError("Choose a new --report path to preserve previous captures")
            source = options.profile_input.read_text(encoding="utf-8").strip() if options.profile_input else "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            if not source or any(character in source for character in "\r\n\0") or len(source.encode("utf-8")) > 4096:
                raise ValueError("Profile input must be one nonempty UTF-8 line, at most 4096 bytes")
            if options.profile_operators and len(source.encode("utf-8")) > 128:
                raise ValueError("Isolated operator capture requires a source of at most 128 UTF-8 bytes; use resident profiling for documents")
            release = binary
            profile = release.with_name("translate-profile.exe")
            if release == profile or not release.is_file() or not profile.is_file():
                raise ValueError("Supply the release --binary beside a built translate-profile.exe")
            identities = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (release, profile)}
            for repeat in range(options.profile_repeats):
                modes = ["release", "timing", "basic"]
                if repeat % 2:
                    modes.reverse()
                if options.profile_operators:
                    modes.append("detailed")
                for mode in modes:
                    binary = release if mode == "release" else profile
                    arguments = ["--from", "de", "--to", "en", "--batch"]
                    input_bytes = ("Guten Morgen.\n" + (source + "\n") * 2).encode("utf-8")
                    if mode == "basic":
                        arguments.append("--qnn-profile")
                    if mode == "detailed":
                        arguments = ["--from", "de", "--to", "en", "--qnn-profile-detailed", "--max-tokens", "256", source]
                        input_bytes = None
                    previous_count = len(results)
                    try:
                        record = run(f"target-{mode}-{repeat}", arguments, 0, input_bytes=input_bytes)
                    finally:
                        if len(results) > previous_count:
                            results[-1].update(mode=mode, repeat=repeat, source=source,
                                               expected_repeats=options.profile_repeats, operator_requested=options.profile_operators,
                                               source_sha256=hashlib.sha256(source.encode("utf-8")).hexdigest(),
                                               binary=str(binary), binary_sha256=identities[str(binary)],
                                               captured_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
                            report.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
                    target_profile_requests(record)
            for path, digest in identities.items():
                if hashlib.sha256(Path(path).read_bytes()).hexdigest() != digest:
                    raise ValueError("Binary changed during profiling")
            summary = target_profile_summary(results)
            summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(dict(request_us=summary["request_us"], overhead_percent=summary["overhead_percent"],
                                  decode_top=summary["operators"]["decode"][:8]), indent=2), flush=True)
            print("PASS targeted profile; raw " + str(report) + "; summary " + str(summary_path), flush=True)
            return
        if options.document_profile:
            document = (
                "Unser Team bereitet die Einf\u00fchrung eines neuen digitalen Dienstes vor, der den Arbeitsalltag in mehreren Abteilungen erleichtern soll. "
                "In der ersten Woche sammeln wir R\u00fcckmeldungen von Mitarbeitern und pr\u00fcfen, welche Aufgaben besonders viel Zeit beanspruchen. "
                "Dabei achten wir darauf, dass auch seltene Probleme und unterschiedliche technische Erfahrungen angemessen ber\u00fccksichtigt werden. "
                "Anschlie\u00dfend erstellt die Projektleitung einen klaren Ablaufplan mit verantwortlichen Personen, erreichbaren Zielen und verbindlichen Terminen. "
                "Die Entwicklung beginnt mit einer kleinen Version, damit wichtige Annahmen fr\u00fch unter realistischen Bedingungen getestet werden k\u00f6nnen. "
                "Alle Teilnehmer erhalten eine kurze Einf\u00fchrung und k\u00f6nnen Fragen direkt an das zust\u00e4ndige Team richten. "
                "W\u00e4hrend der Testphase dokumentieren wir Fehler sorgf\u00e4ltig und unterscheiden zwischen notwendigen Korrekturen und zus\u00e4tzlichen W\u00fcnschen. "
                "Pers\u00f6nliche Daten werden nur gespeichert, wenn sie f\u00fcr die jeweilige Aufgabe wirklich erforderlich sind. "
                "Vor der Freigabe kontrollieren unabh\u00e4ngige Kollegen die wichtigsten Abl\u00e4ufe sowie die Verst\u00e4ndlichkeit der angezeigten Informationen. "
                "Nach erfolgreichem Abschluss erweitern wir den Einsatz schrittweise, statt alle bisherigen Verfahren gleichzeitig zu ersetzen. "
                "Ein regelm\u00e4\u00dfiger Austausch hilft uns, unerwartete Schwierigkeiten schnell zu erkennen und gemeinsam tragf\u00e4hige L\u00f6sungen zu finden. "
                "Am Ende vergleichen wir die Ergebnisse mit den urspr\u00fcnglichen Zielen und halten konkrete Verbesserungen f\u00fcr die n\u00e4chste Arbeitsphase fest."
            )
            words = len(document.split())
            assert 180 <= words <= 220, words
            if options.document_profile == "describe":
                print(json.dumps(dict(words=words, utf8_bytes=len(document.encode()), text=document), ensure_ascii=True), flush=True)
                return
            common = ["--from", "de", "--to", "en", "--batch"]
            if options.document_profile in ("basic", "counters"):
                common.append("--qnn-profile")
            repetitions = 1 if options.document_profile in ("basic", "counters") else 2
            monitor = None
            telemetry = binary.parent / ("document-" + options.document_profile + "-engines.jsonl")
            with telemetry.open("wb") as samples:
                monitor = subprocess.Popen([
                    str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"),
                    "-NoProfile", "-Command",
                    "Get-Counter '\\GPU Engine(*)\\Utilization Percentage' -SampleInterval 1 -Continuous -ErrorAction Stop | "
                    "ForEach-Object { [pscustomobject]@{utc=$_.Timestamp.ToUniversalTime().ToString('o'); "
                    "tick=[Diagnostics.Stopwatch]::GetTimestamp(); "
                    "processes=@(Get-Process translate,translate-profile -ErrorAction SilentlyContinue | Select-Object Id,CPU,WorkingSet64); "
                    "samples=@($_.CounterSamples | Select-Object InstanceName,CookedValue,Status)} | ConvertTo-Json -Depth 5 -Compress }"
                ], stdout=samples, stderr=subprocess.PIPE)
                try:
                    record = run("document-" + options.document_profile, common, 0,
                        input_bytes=("Guten Morgen.\n" + (document + "\n") * repetitions).encode())
                finally:
                    monitor.terminate()
                    _, monitor_error = monitor.communicate(timeout=15)
            record["counter_diagnostics"] = monitor_error.decode("utf-8", errors="replace")
            record["counter_file"] = str(telemetry)
            record["source"] = document
            record["source_words"] = words
            outputs = record["stdout"].splitlines()
            assert len(outputs) == repetitions + 1 and outputs[0] == "Good morning."
            assert all(output == outputs[1] for output in outputs[1:])
            request_times = [int(value) for value in re.findall(r"request us: (\d+)", record["stderr"])]
            assert len(request_times) == repetitions + 1
            assert "Retrying smaller translation pieces" not in record["stderr"]
            record["requests_us"] = request_times
            names = re.findall(r"PROFILE (\w+) us=\d+ calls=\d+", record["stderr"])
            request = None
            requests = []
            for line in record["stderr"].splitlines():
                if match := re.fullmatch(r"PROFILE_REQUEST_BEGIN id=(\d+)", line):
                    request = dict(id=int(match[1]), phases={}, pieces=[], piece_times=[], partitions=[], accelerator_us=[])
                    requests.append(request)
                elif request is not None:
                    if match := re.fullmatch(r"PROFILE_REQUEST_PHASE id=\d+ phase=(\d+) us=(\d+) calls=(\d+)", line):
                        request["phases"][names[int(match[1])]] = dict(us=int(match[2]), calls=int(match[3]))
                    elif line.startswith("PROFILE_PIECE "):
                        request["pieces"].append({key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)})
                    elif line.startswith("PROFILE_PIECE_TIME "):
                        request["piece_times"].append({key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)})
                    elif match := re.fullmatch(r"PROFILE_PARTITION (\d+) (prefill|decode) us=(\d+)", line):
                        request["partitions"].append(dict(partition=int(match[1]), mode=match[2], us=int(match[3])))
                    elif match := re.search(r"Accelerator \(execute excluding wait\) time type=3012 unit=1 value=(\d+)", line):
                        request["accelerator_us"].append(int(match[1]))
                        request["partitions"][-1]["accelerator_us"] = int(match[1])
                    elif line.startswith("PROFILE_REQUEST_END "):
                        request.update({key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)})
                        request = None
            record["requests"] = requests
            for request in requests[1:]:
                generated = sum(piece["generated_tokens"] for piece in request["pieces"])
                request["generated_tokens_including_stops"] = generated
                request["tokens_per_second_including_stops"] = generated * 1e6 / request["us"]
                print(json.dumps({key: value for key, value in request.items() if key not in ("partitions", "accelerator_us")}), flush=True)
            print(json.dumps(dict(source_words=words, output_words=len(outputs[1].split()), requests_us=request_times,
                translation_words_per_second=words * 1e6 / statistics.median(request_times[1:]), counter_file=str(telemetry))), flush=True)
            print("PASS resident document profile and repeat output parity", flush=True)
            return
        if options.embedding_load:
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            common = ["--from", "de", "--to", "en", "--max-tokens", "96"]
            reference = None
            samples = [[], []]
            for repeat in range(5):
                for serial in (True, False) if repeat % 2 == 0 else (False, True):
                    record = run(f"embedding-{int(serial)}-{repeat}",
                        [*common, *(["--serial-embedding-load"] if serial else []), sentence], 0, observe=True)
                    tokens = re.findall(r"generated token: (\d+)", record["stderr"])
                    if reference is None:
                        reference = (record["stdout"], tokens)
                    assert tokens and (record["stdout"], tokens) == reference
                    record["phases"] = {name: dict(us=int(elapsed), calls=int(calls))
                        for name, elapsed, calls in re.findall(r"PROFILE (\w+) us=(\d+) calls=(\d+)", record["stderr"])}
                    assert record["stderr"].count("performance release: 0") == 1
                    samples[int(serial)].append(record)
            for serial, subset in enumerate(samples):
                print(json.dumps(dict(serial=bool(serial),
                    wall_seconds=statistics.median(record["wall_seconds"] for record in subset),
                    first_byte_seconds=statistics.median(record["first_byte_seconds"] for record in subset),
                    embedding_us=statistics.median(record["phases"]["embedding_load"]["us"] for record in subset)
                        if all("embedding_load" in record["phases"] for record in subset) else None)), flush=True)
            print("PASS embedding load timing and token parity", flush=True)
            return
        if options.w8_performance:
            common = ["--from", "de", "--to", "en", "--max-tokens", "96"]
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            reference_tokens = None
            samples = [[], []]
            for repeat in range(3):
                for optimized in (False, True) if repeat % 2 == 0 else (True, False):
                    record = run(f"release-{int(optimized)}-{repeat}",
                        [*common, *([] if optimized else ["--balanced"]), sentence], 0, observe=True)
                    tokens = re.findall(r"generated token: (\d+)", record["stderr"])
                    if reference_tokens is None:
                        reference_tokens = tokens
                    assert tokens and tokens == reference_tokens
                    assert record["stderr"].count("performance vote: 0") == int(optimized)
                    assert record["stderr"].count("performance release: 0") == int(optimized)
                    record["request_us"] = int(re.search(r"request us: (\d+)", record["stderr"])[1])
                    samples[int(optimized)].append(record)
            parity = run("release-parity", [*common, "--verify-decode", sentence], 0)
            assert "PASS decode KV/logits parity position:" in parity["stderr"]
            batch = run("release-batch", [*common, "--batch"], 0,
                        input_bytes=((sentence + "\n") * 2).encode())
            assert batch["stdout"] == samples[1][0]["stdout"] * 2
            assert batch["stderr"].count("performance release: 0") == 2
            limited = run("release-limited", ["--from", "de", "--to", "en", "--max-tokens", "1", "Guten Morgen."], 2)
            assert limited["stderr"].count("performance release: 0") == 1
            run("release-quiet", [*common, "--quiet", sentence], 0, samples[1][0]["stdout"])
            for optimized, subset in enumerate(samples):
                print(json.dumps(dict(optimized=bool(optimized),
                    median_wall_seconds=statistics.median(record["wall_seconds"] for record in subset),
                    median_first_byte_seconds=statistics.median(record["first_byte_seconds"] for record in subset),
                    median_request_us=statistics.median(record["request_us"] for record in subset))), flush=True)
            print("PASS uninstrumented W8 timing, token/decode parity, batch, limit and quiet checks", flush=True)
            return
        if options.w8_profile:
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            common = ["--from", "de", "--to", "en", "--max-tokens", "96"]
            reference_tokens = None
            for repeat in range(3):
                for power in (False, True) if repeat % 2 == 0 else (True, False):
                    record = run(f"resident-{int(power)}-{repeat}",
                                 [*common, "--batch", *([] if power else ["--balanced"])], 0,
                                 input_bytes=((sentence + "\n") * 3).encode())
                    tokens = re.findall(r"generated token: (\d+)", record["stderr"])
                    assert tokens
                    if reference_tokens is None:
                        reference_tokens = tokens
                    assert tokens == reference_tokens
                    assert record["stderr"].count("bundle restore: 0") == 3
                    assert record["stderr"].count("performance vote: 0") == (3 if power else 0)
                    assert record["stderr"].count("performance release: 0") == (3 if power else 0)
                    record["phases"] = {name: dict(us=int(elapsed), calls=int(calls)) for name, elapsed, calls in
                        re.findall(r"^PROFILE (\w+) us=(\d+) calls=(\d+)$", record["stderr"], re.MULTILINE)}
                    record["requests_us"] = [int(value) for value in re.findall(r"request us: (\d+)", record["stderr"])]
                    record["partitions"] = re.findall(r"PROFILE_PARTITION (\d+) (\w+) us=(\d+)", record["stderr"])
                    assert len(record["requests_us"]) == 3 and record["partitions"]
            run("partition-qnn-detail", [*common, "--qnn-profile-detailed", "Guten Morgen."], 0, "Good morning.\n")
            print("PASS W8 resident profiling and power-vote token parity", flush=True)
            return
        if options.quality:
            if not options.case:
                smoke = run("greeting-decode-parity", ["--from", "de", "--to", "en", "--max-tokens", "16",
                            "--verify-decode", "Guten Morgen."], 0, "Good morning.\n")
                assert "PASS decode KV/logits parity position:" in smoke["stderr"]
                if options.partitions:
                    assert smoke["stderr"].count("bundle restore: 0") == 3
                    assert "TranslateGemma 4B W8" in smoke["stderr"]
            if options.quality == "diagnostic":
                corpus = json.loads(Path(__file__).with_name("translategemma-quality.json").read_text(encoding="utf-8"))
                for case in corpus["cases"]:
                    if case["split"] != "diagnostic":
                        continue
                    if options.case and case["id"] not in options.case:
                        continue
                    record = run(case["id"], ["--from", case["source"], "--to", case["target"],
                                 "--max-tokens", "128", case["text"]], (0, 2))
                    record.update(source=case["text"], meaning=case["meaning"], references=case["references"])
            print("PASS candidate execution checks; diagnostic translations require semantic review", flush=True)
            return
        if options.documents:
            common = ["--from", "de", "--to", "en"]
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            expected = "Good day, my name is Hase. I know nothing."
            run("short-auto", [*common, sentence], 0, expected + "\n")
            paragraph = " ".join([sentence] * 9)
            long_result = run("long-paragraph", [*common, paragraph], 0, observe=True)
            assert long_result["stdout"].count("Hase") == 9
            assert long_result["stderr"].count("translated source bytes:") > 1
            assert long_result["stderr"].count("bundle restore: 0") == 1
            assert len(re.findall(r"generated token:", long_result["stderr"])) > 64
            assert long_result["last_byte_seconds"] - long_result["first_byte_seconds"] > 1
            document = "\t" + paragraph + "\r\n\r\n" + sentence + "\r\n"
            streamed = run("multiline-stdin", [*common, "--stdin"], 0, input_bytes=document.encode())
            assert streamed["stdout"].count("Hase") == 10
            assert streamed["stdout"].startswith("\t") and "\r\n\r\n" in streamed["stdout"]
            assert streamed["stdout"].endswith("\r\n\n")
            run("buffered-document", [*common, "--no-stream", "--stdin"], 0, streamed["stdout"], document.encode())
            run("strict-limit", [*common, "--max-tokens", "1", "Guten Tag."], 2, "Good\n")
            run("strict-budget", [*common, "--max-tokens", "64", "Hallo " * 700], 1, "")
            run("invalid-stdin", [*common, "--stdin"], 1, "", b"\xff")
            run("oversized-stdin", [*common, "--stdin"], 1, "", b"a" * 196608)
            run("conflicting-stdin", [*common, "--stdin", "--batch"], 1, "", b"Hello")
            run("batch-documents", [*common, "--batch"], 0, long_result["stdout"] + expected + "\n",
                (paragraph + "\n" + sentence + "\n").encode())
            print("PASS long documents, >64 output tokens, paragraph boundaries, stdin, buffering and batch isolation", flush=True)
            return
        if options.load_benchmark:
            assets = Path(__file__).resolve().parents[2]
            common = ["--bundle", "--bindings", str(assets / "build/gemma-block/prompt-512.gmb"),
                      "--tokenizer", str(assets / "models/translategemma-4b-stage4/tokenizer.gta"),
                      "--from", "de", "--to", "en"]
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            expected = "Good day, my name is Hase. I know nothing.\n"
            samples = [[], []]
            reference_tokens = None
            for repeat in range(3):
                for overlap in (0, 1) if repeat % 2 == 0 else (1, 0):
                    record = run(("overlap-" if overlap else "serial-") + str(repeat + 1),
                                 [*common, *([] if overlap else ["--serial-load"]), sentence], 0, expected, observe=True)
                    tokens = re.findall(r"generated token: (\d+)", record["stderr"])
                    assert tokens
                    if reference_tokens is None:
                        reference_tokens = tokens
                    assert tokens == reference_tokens
                    elapsed = int(re.search(r"bundle read/hash us: (\d+)", record["stderr"])[1])
                    record["read_hash_us"] = elapsed
                    samples[overlap].append(elapsed)
            print(json.dumps({"serial_median_us": statistics.median(samples[0]),
                              "overlap_median_us": statistics.median(samples[1])}), flush=True)
            return
        if options.selection_cache:
            common = ["--bundle", "--from", "de", "--to", "en"]
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            expected = "Good day, my name is Hase. I know nothing.\n"
            setup_samples = [[], []]
            reference_tokens = None
            for repeat in range(3):
                for cached in (0, 1) if repeat % 2 == 0 else (1, 0):
                    record = run(("cached-" if cached else "compiled-") + str(repeat + 1),
                                 [*common, *([] if cached else ["--compile-selection"]), "--batch"],
                                 0, expected * 3, ((sentence + "\n") * 3).encode())
                    assert ("selection cache restore: 0" in record["stderr"]) == bool(cached)
                    tokens = re.findall(r"generated token: (\d+)", record["stderr"])
                    assert tokens
                    if reference_tokens is None:
                        reference_tokens = tokens
                    assert tokens == reference_tokens
                    record["selection_setup_us"] = int(re.search(r"selection setup us: (\d+)", record["stderr"])[1])
                    setup_samples[cached].append(record["selection_setup_us"])
                    record["request_us"] = [int(value) for value in re.findall(r"request us: (\d+)", record["stderr"])]
                    assert len(record["request_us"]) == 3
            print(json.dumps({"compiled_setup_median_us": statistics.median(setup_samples[0]),
                              "cached_setup_median_us": statistics.median(setup_samples[1])}), flush=True)
            source_binding = binary.parent / "gemma-block/prompt-512.gmb"
            original = Path(str(source_binding) + ".selection.context").read_bytes()
            with tempfile.TemporaryDirectory(dir=binary.parent) as temporary:
                binding = Path(temporary) / "test.gmb"
                shutil.copyfile(source_binding, binding)
                Path(str(binding) + ".bundle.context").hardlink_to(Path(str(source_binding) + ".bundle.context"))
                context = Path(str(binding) + ".selection.context")
                args = [*common, "--bindings", str(binding), "Guten Tag."]
                missing = run("selection-cache-missing", args, 0, "Good day.\n")
                assert "selection cache restore:" not in missing["stderr"]
                corruptions = {"empty": b"", "short": original[:40], "truncated": original[:-1],
                               "trailing": original + b"x"}
                for name, offset in (("magic", 0), ("version", 4), ("runtime", 8), ("digest", 52), ("payload", len(original) - 1)):
                    changed = bytearray(original)
                    changed[offset] ^= 1
                    corruptions[name] = changed
                oversized = bytearray(original)
                struct.pack_into("<Q", oversized, 32, 16777217)
                corruptions["oversized"] = oversized
                duplicate = bytearray(original)
                duplicate[44:48] = duplicate[40:44]
                corruptions["duplicate-id"] = duplicate
                for name, payload in corruptions.items():
                    context.write_bytes(payload)
                    rejected = run("selection-cache-" + name, args, 1, "")
                    assert "selection cache restore:" not in rejected["stderr"]
                    assert "Invalid selection cache" in rejected["stderr"]
                context.write_bytes(original)
                run("selection-cache-restored", args, 0, "Good day.\n")
            return
        if options.performance:
            assets = Path(__file__).resolve().parents[2]
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            expected = "Good day, my name is Hase. I know nothing.\n"
            common = ["--bundle", "--tokenizer", str(assets / "models/translategemma-4b-stage4/tokenizer.gta"),
                      "--from", "de", "--to", "en", "--batch"]
            for repeat in range(3):
                for enabled in (False, True) if repeat % 2 == 0 else (True, False):
                    args = [*common, *(["--performance"] if enabled else [])]
                    record = run(("performance-" if enabled else "default-") + str(repeat + 1), args, 0,
                                 expected * 3, ((sentence + "\n") * 3).encode())
                    if enabled:
                        assert "performance vote: 0" in record["stderr"]
                    record["request_us"] = [int(value) for value in re.findall(r"request us: (\d+)", record["stderr"])]
                    assert len(record["request_us"]) == 3
                    print(json.dumps({"name": record["name"], "request_us": record["request_us"]}), flush=True)
            return
        if options.bundle:
            assets = Path(__file__).resolve().parents[2]
            common = ["--bundle", "--tokenizer", str(assets / "models/translategemma-4b-stage4/tokenizer.gta"),
                      "--from", "de", "--to", "en"]
            source_binding = binary.parent / "gemma-block/prompt-512.gmb"
            source_context = Path(str(source_binding) + ".bundle.context")
            with tempfile.TemporaryDirectory(dir=binary.parent) as temporary:
                binding = Path(temporary) / "invalid.gmb"
                context = Path(str(binding) + ".bundle.context")
                shutil.copyfile(source_binding, binding)
                for size in (0, 16, 65536):
                    with source_context.open("rb") as original:
                        context.write_bytes(original.read(size))
                    rejected = run("bundle-truncated-" + str(size), [*common, "--bindings", str(binding), "Guten Tag."], 1, "")
                    assert "bundle restore:" not in rejected["stderr"]
                shutil.copyfile(source_context, context)
                with context.open("r+b") as corrupt:
                    corrupt.seek(-1, 2)
                    value = corrupt.read(1)
                    corrupt.seek(-1, 2)
                    corrupt.write(bytes([value[0] ^ 1]))
                rejected = run("bundle-corrupt-payload", [*common, "--bindings", str(binding), "Guten Tag."], 1, "")
                assert "bundle restore:" not in rejected["stderr"]
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            expected = "Good day, my name is Hase. I know nothing.\n"
            parity = run("bundle-parity", [*common, "--verify-decode", sentence], 0, expected)
            assert parity["stderr"].count("PASS decode KV/logits parity position") == 12
            reference_args = ["--bindings", str(assets / "build/gemma-block/prompt-512.gmb"),
                              "--tokenizer", str(assets / "models/translategemma-4b-stage4/tokenizer.gta"),
                              "--from", "de", "--to", "en", "--padded-decode"]
            reference = run("bundle-original-reference", [*reference_args, sentence], 0, expected)
            assert re.findall(r"generated token: (\d+)", parity["stderr"]) == re.findall(r"generated token: (\d+)", reference["stderr"])
            long_text = "Guten Tag. " * 40
            long_result = run("bundle-multichunk", [*common, "--verify-decode", "--max-tokens", "4", long_text], 2)
            run("bundle-original-multichunk", [*reference_args, "--max-tokens", "4", long_text], 2, long_result["stdout"])
            unicode_args = ["--bundle", "--tokenizer", str(assets / "models/translategemma-4b-stage4/tokenizer.gta"),
                            "--from", "en", "--to", "de", "--verify-decode", "The door is open."]
            run("bundle-unicode", unicode_args, 0, "Die T\u00fcr ist offen.\n")
            for repeat in range(3):
                run("bundle-timing-" + str(repeat + 1), [*common, sentence], 0, expected, observe=True)
            run("bundle-batch", [*common, "--batch"], 0, expected * 3, ((sentence + "\n") * 3).encode())
            run("bundle-quiet", [*common, "--quiet", "Guten Tag."], 0, "Good day.\n")
            sampled = run("bundle-detail", [*common, "--qnn-profile-detailed", "Guten Tag."], 0, "Good day.\n")
            assert "QNN_SAMPLE decode" in sampled["stderr"]
            print("PASS shared-weight bundle parity and timing", flush=True)
            return
        if options.streaming:
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            expected = "Good day, my name is Hase. I know nothing.\n"
            for repeat in range(3):
                for buffered in (False, True) if repeat % 2 == 0 else (True, False):
                    arguments = ["--quiet", "--from", "de", "--to", "en"]
                    if buffered:
                        arguments.append("--no-stream")
                    record = run(("buffered-" if buffered else "streamed-") + str(repeat + 1),
                                 [*arguments, sentence], 0, expected, observe=True)
                    span = record["last_byte_seconds"] - record["first_byte_seconds"]
                    assert span < 0.5, record
            unicode_args = ["--quiet", "--from", "en", "--to", "de", "The door is open."]
            unicode_result = run("quiet-unicode", unicode_args, 0, observe=True)
            assert "\u00fc" in unicode_result["stdout"]
            run("buffered-unicode", ["--no-stream", *unicode_args], 0, unicode_result["stdout"])
            run("quiet-invalid", ["--quiet", "--from", "invalid", "--to", "en", "Hello"], 1, "")
            run("quiet-limit", ["--quiet", "--from", "de", "--to", "en", "--max-tokens", "1", "Guten Tag."], 2, "Good\n")
            run("quiet-batch", ["--quiet", "--from", "de", "--to", "en", "--batch"], 0,
                "Good day.\n" + expected + "Good day.\n", ("Guten Tag.\n" + sentence + "\nGuten Tag.").encode())
            detailed = run("qnn-detailed", ["--from", "de", "--to", "en", "--decode", "--qnn-profile-detailed", "Guten Tag."], 0, "Good day.\n")
            assert "QNN_SAMPLE decode" in detailed["stderr"] and "QNN_EVENT Accelerator" in detailed["stderr"]
            print("PASS quiet, streaming latency, UTF-8 parity, batch isolation and detailed profiling", flush=True)
            return
        if options.profile:
            phase_runs = []
            token_runs = []
            for repeat in range(3):
                record = run("profile-" + str(repeat + 1), ["--from", "de", "--to", "en", "--decode",
                    "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."], 0,
                    "Good day, my name is Hase. I know nothing.\n", observe=True)
                phases = {name: dict(us=int(elapsed), calls=int(calls)) for name, elapsed, calls in
                          re.findall(r"^PROFILE (\w+) us=(\d+) calls=(\d+)$", record["stderr"], re.MULTILINE)}
                assert len(phases) == 24, phases
                assert phases["decode_execute"]["calls"] == 12, phases
                assert phases["prefill_execute"]["calls"] == 1, phases
                exclusive = ("arguments", "qnn_init", "restore_total", "rpc", "embedding_load", "buffers",
                             "prefill_prepare", "prefill_execute", "prefill_kv", "decode_prepare", "decode_execute",
                             "decode_kv", "argmax", "output", "cleanup", "decode_setup", "cache_reset", "cache_transfer")
                accounted = sum(phases[name]["us"] for name in exclusive)
                assert 0.98 <= accounted / phases["total"]["us"] <= 1.01, (accounted, phases)
                assert abs(record["wall_seconds"] - phases["total"]["us"] / 1e6) < 0.5, record
                record["phases"] = phases
                record["accounted_us"] = accounted
                phase_runs.append(phases)
                token_runs.append(re.findall(r"^generated token: (\d+)$", record["stderr"], re.MULTILINE))
            assert token_runs[0] == token_runs[1] == token_runs[2], token_runs
            medians = {name: statistics.median(phases[name]["us"] for phases in phase_runs) for name in phase_runs[0]}
            print("PROFILE median microseconds: " + json.dumps(medians, sort_keys=True), flush=True)
            print("PASS profile accounting, deterministic tokens, UTF-8 output and cleanup", flush=True)
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            for repeat in range(3):
                record = run("profile-auto-" + str(repeat + 1), ["--from", "de", "--to", "en", sentence], 0,
                             "Good day, my name is Hase. I know nothing.\n")
                phases = {name: dict(us=int(elapsed), calls=int(calls)) for name, elapsed, calls in
                          re.findall(r"^PROFILE (\w+) us=(\d+) calls=(\d+)$", record["stderr"], re.MULTILINE)}
                assert len(phases) == 24 and phases["decode_execute"]["calls"] == 12
                accounted = sum(phases[name]["us"] for name in exclusive)
                assert 0.98 <= accounted / phases["total"]["us"] <= 1.01, (accounted, phases)
                if "bundle restore: 0" in record["stderr"]:
                    assert "bundle context_read includes overlapped SHA-256" in record["stderr"]
                    assert phases["context_read"]["calls"] == 1
                record["phases"] = phases
                record["accounted_us"] = accounted
            run("profile-batch", ["--from", "de", "--to", "en", "--batch"], 0,
                "Good day, my name is Hase. I know nothing.\n" * 3, ((sentence + "\n") * 3).encode("utf-8"))
            return
        run("help", ["--help"], 0, "")
        run("missing", [], 1, "")
        run("invalid-language", ["--from", "invalid", "--to", "en", "Hello"], 1, "")
        run("missing-value", ["--from"], 1, "")
        run("invalid-limit", ["--from", "de", "--to", "en", "--max-tokens", "513", "Hallo"], 1, "")
        run("over-budget", ["--from", "de", "--to", "en", "--max-tokens", "64", "Hallo " * 700], 1, "")
        run("duplicate-text", ["--from", "de", "--to", "en", "Hallo", "Welt"], 1, "")
        if options.hardware:
            run("greeting", ["--from", "de", "--to", "en", "--max-tokens", "16", "Guten Tag."], 0, "Good day.\n")
            sentence = "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."
            optimized = run("user-sentence", ["--from", "de", "--to", "en", "--decode", sentence], 0,
                "Good day, my name is Hase. I know nothing.\n")
            padded = run("padded-reference", ["--from", "de", "--to", "en", "--padded-decode", sentence], 0, optimized["stdout"])
            assert re.findall(r"generated token: (\d+)", optimized["stderr"]) == re.findall(r"generated token: (\d+)", padded["stderr"])
            parity = run("decode-numerical-parity", ["--from", "de", "--to", "en", "--verify-decode", sentence], 0, optimized["stdout"])
            assert parity["stderr"].count("PASS decode KV/logits parity position") == 12, parity
            long_text = "Guten Tag. " * 40
            long_parity = run("multichunk-parity", ["--from", "de", "--to", "en", "--max-tokens", "1", long_text], 2)
            long_decode = run("multichunk-decode", ["--from", "de", "--to", "en", "--max-tokens", "4", "--verify-decode", long_text], 2)
            assert long_decode["stdout"].startswith(long_parity["stdout"].rstrip("\n")), long_decode
            positions = [int(value) for value in re.findall(r"PASS decode KV/logits parity position: (\d+)", long_decode["stderr"])]
            assert len(positions) == 3 and positions[0] > 128, positions
            run("utf8-quotes", ["--from", "en", "--to", "de", "--max-tokens", "32",
                'He said "Good morning."'], 0)
            unicode_result = run("utf8-output", ["--from", "en", "--to", "de", "--max-tokens", "16",
                "The door is open."], 0)
            assert "\u00fc" in unicode_result["stdout"], unicode_result
            run("limit", ["--from", "de", "--to", "en", "--max-tokens", "1", "Guten Tag."], 2, "Good\n")
            batch = run("batch-isolation", ["--from", "de", "--to", "en", "--batch"], 0,
                "Good day.\n" + optimized["stdout"] + "Good day.\n",
                ("Guten Tag.\n" + sentence + "\nGuten Tag.").encode("utf-8"))
            assert (batch["stderr"].count("prompt restore: 0") == 2 or
                    batch["stderr"].count("bundle restore: 0") == 1), batch
            run("batch-limit", ["--from", "de", "--to", "en", "--batch", "--max-tokens", "1"], 2,
                "Good\nGood\n", b"Guten Tag.\nGuten Tag.\n")
            run("batch-invalid-next", ["--from", "de", "--to", "en", "--batch"], 1,
                "Good day.\n", b"Guten Tag.\n\xff\n")
            sampled = run("qnn-profile", ["--from", "de", "--to", "en", "--decode", "--qnn-profile", "Guten Tag."], 0, "Good day.\n")
            assert "QNN_SAMPLE decode" in sampled["stderr"] and "QNN_EVENT Accelerator" in sampled["stderr"], sampled
        print("PASS translator CLI" + (" and NPU integration" if options.hardware else ""), flush=True)
    finally:
        report = binary.parent / ("translate-w8-profile-results.json" if options.w8_profile else "translate-quality-" + options.quality + ("-" + "-".join(options.case) if options.case else "") + "-results.json" if options.quality else "translate-document-results.json" if options.documents else "translate-load-results.json" if options.load_benchmark else "translate-selection-cache-results.json" if options.selection_cache else "translate-performance-results.json" if options.performance else "translate-bundle-results.json" if options.bundle else "translate-streaming-profile-results.json" if options.profile else
                      "translate-streaming-results.json" if options.streaming else "translate-streaming-test-results.json")
        if options.w8_performance:
            report = binary.parent / "translate-w8-performance-results.json"
        if options.embedding_load:
            report = binary.parent / "translate-embedding-load-results.json"
        if options.document_profile:
            report = binary.parent / ("translate-document-" + options.document_profile + "-results.json")
        if options.target_profile:
            report = binary.parent / "translate-target-profile.json"
        if options.compare_bindings:
            report = binary.parent / "translate-binding-comparison.json"
        report = options.report or report
        if not (options.target_profile or options.compare_bindings) or results:
            report.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()