#!/usr/bin/env python3
"""Offline Windows memory measurement for the existing full-model QNN restore gate."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import subprocess
import time


class MemoryStatus(ctypes.Structure):
    _fields_ = [("length", wintypes.DWORD), ("load", wintypes.DWORD)] + [
        (name, ctypes.c_ulonglong) for name in
        ("total_physical", "available_physical", "total_pagefile", "available_pagefile",
         "total_virtual", "available_virtual", "available_extended_virtual")]


class ProcessMemory(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("page_fault_count", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in
        ("peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
         "peak_nonpaged_pool", "nonpaged_pool", "pagefile_usage", "peak_pagefile_usage", "private_bytes")]


def windows_api():
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(MemoryStatus)]
    api.GlobalMemoryStatusEx.restype = wintypes.BOOL
    api.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemory), wintypes.DWORD]
    api.K32GetProcessMemoryInfo.restype = wintypes.BOOL
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.CloseHandle.restype = wintypes.BOOL
    return api


def system_memory(api):
    status = MemoryStatus()
    status.length = ctypes.sizeof(status)
    if not api.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise ctypes.WinError(ctypes.get_last_error())
    return {"total_physical_bytes": status.total_physical,
            "available_physical_bytes": status.available_physical,
            "commit_limit_bytes": status.total_pagefile,
            "commit_available_bytes": status.available_pagefile}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path(__file__).resolve().parents[2] / "build/gemma-block")
    parser.add_argument("--bucket", type=int, choices=(512, 1024, 2048), default=512)
    parser.add_argument("--minimum-free-gib", type=float, default=8)
    parser.add_argument("--measure", action="store_true", help="Run the restore gate only if memory headroom permits")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 8 <= args.minimum_free_gib <= 128:
        parser.error("minimum headroom must be 8..128 GiB")
    log_path = Path(str(args.output) + ".log")
    if args.output.exists() or log_path.exists():
        parser.error("use a new output path")
    binary = (args.build_dir / "test-gemma-block.exe").resolve()
    binding = (args.build_dir / f"prompt-{args.bucket}.gmb").resolve()
    context = Path(str(binding) + ".context")
    for path in (binary, binding, context):
        if not path.is_file():
            parser.error("missing existing restore artifact: " + str(path))
    api = windows_api()
    before = system_memory(api)
    report = {"schema_version": 1, "bucket": args.bucket, "mode": "preflight",
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "context_file_bytes": context.stat().st_size, "before": before,
              "minimum_free_bytes": int(args.minimum_free_gib * 2**30),
              "headroom_ok": before["available_physical_bytes"] >= args.minimum_free_gib * 2**30,
              "measurement_complete": False, "samples": [],
              "limitations": "Process counters do not isolate DSP/driver allocations. System deltas include other processes. Restore/prompt gate only, not graph construction or autoregressive decode."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.measure and report["headroom_ok"]:
        report["mode"] = "restore-and-prompt"
        started = time.monotonic()
        with log_path.open("xb") as log:
            process = subprocess.Popen([str(binary), str(binding), f"restore-{args.bucket}"],
                                       cwd=binary.parent, stdout=log, stderr=subprocess.STDOUT)
            handle = api.OpenProcess(0x0410, False, process.pid)
            try:
                if not handle:
                    raise ctypes.WinError(ctypes.get_last_error())
                while True:
                    memory = ProcessMemory()
                    memory.size = ctypes.sizeof(memory)
                    if not api.K32GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.size):
                        raise ctypes.WinError(ctypes.get_last_error())
                    report["samples"].append({"seconds": time.monotonic() - started,
                        "working_set_bytes": memory.working_set, "peak_working_set_bytes": memory.peak_working_set,
                        "private_bytes": memory.private_bytes, "peak_commit_bytes": memory.peak_pagefile_usage,
                        "page_fault_count": memory.page_fault_count, **system_memory(api)})
                    try:
                        process.wait(timeout=0.25)
                        break
                    except subprocess.TimeoutExpired:
                        pass
                report["exit_code"] = process.returncode
            except BaseException as error:
                process.terminate()
                process.wait()
                report["error"] = str(error)
            finally:
                if handle:
                    api.CloseHandle(handle)
        output = log_path.read_text(encoding="utf-8", errors="replace")
        report["gate_passed"] = report.get("exit_code") == 0 and "PASS prompt restore, KV/logits, padding, determinism and throughput" in output
        report["measurement_complete"] = report["gate_passed"] and bool(report["samples"])
        if report["samples"]:
            report["observed_peak_working_set_bytes"] = max(sample["peak_working_set_bytes"] for sample in report["samples"])
            report["observed_peak_commit_bytes"] = max(sample["peak_commit_bytes"] for sample in report["samples"])
            report["minimum_available_physical_bytes"] = min(sample["available_physical_bytes"] for sample in report["samples"])
        report["after"] = system_memory(api)
    elif args.measure:
        report["blocked_reason"] = "Insufficient memory headroom; no QNN process started. Finish the offline campaign first."
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "samples"}, indent=2))
    if report.get("error") or (report["mode"] == "restore-and-prompt" and not report["measurement_complete"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()