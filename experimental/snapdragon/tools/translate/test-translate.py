"""Bounded CLI checks for the freestanding translator; Python is test-only."""

import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import statistics
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[2] / "build/translate.exe")
    parser.add_argument("--hardware", action="store_true")
    parser.add_argument("--profile", action="store_true", help="Three bounded runs of translate-profile.exe")
    parser.add_argument("--w8-profile", action="store_true", help="Partition timing, resident requests and scoped HTP power comparison")
    parser.add_argument("--report", type=Path, help="Explicit results path, to preserve baseline measurements")
    parser.add_argument("--w8-summary", type=Path, help="Summarize an existing W8 profile report without running inference")
    parser.add_argument("--w8-performance", action="store_true", help="Uninstrumented interleaved W8 latency and vote lifecycle checks")
    parser.add_argument("--streaming", action="store_true", help="Quiet/streaming integration and first-byte timing")
    parser.add_argument("--bundle", action="store_true", help="Shared-weight candidate parity and latency")
    parser.add_argument("--performance", action="store_true", help="Interleaved scoped QNN performance-vote benchmark")
    parser.add_argument("--selection-cache", action="store_true", help="Selector cache corruption, parity and interleaved timing")
    parser.add_argument("--load-benchmark", action="store_true", help="Interleaved serial/overlapped bundle loading")
    parser.add_argument("--documents", action="store_true", help="Long-text, stdin and strict-limit regression")
    parser.add_argument("--bindings", type=Path)
    parser.add_argument("--tokenizer", type=Path)
    parser.add_argument("--partitions", action="store_true")
    parser.add_argument("--quality", choices=("smoke", "diagnostic"), help="Bounded NPU candidate evaluation; outputs require semantic review")
    parser.add_argument("--case", action="append", default=[], help="Filter diagnostic case IDs")
    options = parser.parse_args()
    if options.w8_summary:
        records = json.loads(options.w8_summary.read_text(encoding="utf-8"))
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
        return
    binary = options.binary.resolve()
    if options.profile or options.w8_profile:
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
                                        capture_output=True, input=input_bytes, timeout=240 if options.documents else 120, check=False)
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
        print(json.dumps({key: value for key, value in record.items() if key != "stderr"}, ensure_ascii=True), flush=True)
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
        report = options.report or report
        report.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()