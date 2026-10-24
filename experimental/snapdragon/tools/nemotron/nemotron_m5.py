"""Validate statistics-only Nemotron live evidence; never accept truncated runs."""
import argparse
import json
import pathlib
import re


def read_text(path):
    data = pathlib.Path(path).read_bytes()
    return data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")


def analyze(stats, log, activity, minimum_seconds, microphone=False):
    errors = []
    def check(condition, message):
        if not condition:
            errors.append(message)

    check(stats["schema"] == "newos.nemotron.live.stats.v1", "wrong statistics schema")
    check(stats["passed"] is True and stats["failure"] == 0 and stats["canceled"] == 0,
          "live run failed or was canceled")
    check(stats["duration_s"] >= minimum_seconds and stats["capture_ms"] >= minimum_seconds * 1000,
          "capture did not complete required duration")
    check(stats["accepted_samples"] == stats["consumed_samples"] and stats["queue_last"] == 0,
          "accepted audio not completely drained")
    check(stats["accepted_samples"] >= minimum_seconds * 16000 - 160,
          "audio samples do not cover requested duration")
    check(stats["consumed_samples"] == stats["model_samples"] + stats["endpoint_trimmed_samples"] +
          stats["idle_samples"], "endpoint sample accounting mismatch")
    check(stats["queue_capacity"] == 81920 and 0 <= stats["queue_peak"] < 81920,
          "queue capacity changed or exhausted")
    check(stats["capture_thread"] != stats["inference_thread"] and
          stats["capture_thread"] > 0 and stats["inference_thread"] > 0,
          "capture and inference were not separated")
    check(stats["raw_audio_recorded"] is False, "audio privacy contract violated")
    if microphone:
        check(stats.get("capture_mmcss") == 1, "capture MMCSS Audio scheduling was not registered")
        check(stats.get("source_gap_frames") == 0 and stats.get("source_packets", 0) > 0,
              "microphone source continuity not verified")
        check(stats["input_rate"] in (8000, 16000, 24000, 32000, 44100, 48000, 88200, 96000) and
              1 <= stats["input_channels"] <= 8 and stats["input_format"] in (1, 3),
              "unvalidated microphone format")
    else:
        check((stats["input_rate"], stats["input_channels"], stats["input_format"]) == (16000, 1, 1),
              "wrong paced PCM16 replay format")
    pattern = (r"LIVE_STATS elapsed_s=(\d+) queue_samples=(\d+) queue_peak=(\d+) "
               r"working_set=(\d+) utterances=(\d+)")
    measurements = [tuple(map(int, row)) for row in re.findall(pattern, log)]
    check(len(measurements) == stats["measurements"], "periodic evidence is incomplete")
    steady = [row for row in measurements if row[0] >= 60]
    check(len(steady) >= 4, "insufficient steady-state measurements")
    growth_queue = growth_memory = None
    if len(steady) >= 4:
        quarter = max(1, len(steady) // 4)
        first, last = steady[:quarter], steady[-quarter:]
        growth_queue = sum(row[1] for row in last) / quarter - sum(row[1] for row in first) / quarter
        growth_memory = sum(row[3] for row in last) / quarter - sum(row[3] for row in first) / quarter
        check(growth_queue <= 1600, "steady rolling queue grew by more than100ms")
        check(growth_memory <= 2 * 1024 * 1024, "steady working set grew by more than2MiB")
    check(stats["steady_working_set"] > 0 and stats["end_working_set"] > 0 and
          stats["end_working_set"] <= stats["steady_working_set"] + 2 * 1024 * 1024,
          "end working-set growth exceeded2MiB")
    allowed = {"schema", "event", "utterance", "audio_samples", "capture_samples",
               "endpoint_trimmed_samples", "token_count", "transcript_retained"}
    previous_audio = previous_capture = finals = 0
    for line in activity.splitlines():
        event = json.loads(line)
        if not isinstance(event, dict) or set(event) != allowed:
            check(False, "noncontent event/privacy schema violated or runtime error event present")
            continue
        check(event["schema"] == "newos.nemotron.live.activity.v1" and
              event["transcript_retained"] is False, "noncontent event/privacy schema violated")
        check(event["event"] in ("partial", "final") and event["utterance"] == finals,
              "utterance lifecycle/order mismatch")
        check(previous_audio <= event["audio_samples"] <= event["capture_samples"] and
              previous_capture <= event["capture_samples"] <= stats["consumed_samples"],
              "nonmonotonic or invalid audio positions")
        previous_audio, previous_capture = event["audio_samples"], event["capture_samples"]
        finals += event["event"] == "final"
    check(finals == stats["utterances"], "final/reset event count mismatch")
    return {"schema": "newos.nemotron.m5.live-audit.v1", "passed": not errors,
            "minimum_seconds": minimum_seconds, "microphone": microphone,
            "steady_queue_growth_samples": growth_queue, "steady_memory_growth_bytes": growth_memory,
            "queue_growth_allowance_samples": 1600, "memory_growth_allowance_bytes": 2 * 1024 * 1024,
            "errors": errors, "statistics": stats}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stats", "log", "activity", "output"):
        parser.add_argument("--" + name, required=True, type=pathlib.Path)
    parser.add_argument("--minimum-seconds", required=True, type=int)
    parser.add_argument("--microphone", action="store_true")
    args = parser.parse_args()
    if args.minimum_seconds < 120:
        parser.error("steady-state audit requires at least120 seconds")
    report = analyze(json.loads(read_text(args.stats)), read_text(args.log), read_text(args.activity),
                     args.minimum_seconds, args.microphone)
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"passed": report["passed"], "errors": report["errors"]}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
