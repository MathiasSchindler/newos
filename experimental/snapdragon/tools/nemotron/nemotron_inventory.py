"""Inspect PE import chains without executing vendor DLLs."""

import argparse
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import digest, write_json


def pe_imports(data):
    def integer(offset, fmt):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError("Truncated PE structure")
        return struct.unpack_from(fmt, data, offset)[0]

    if data[:2] != b"MZ":
        raise ValueError("Not a PE image")
    pe = integer(0x3C, "<I")
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    sections = integer(pe + 6, "<H")
    optional_size = integer(pe + 20, "<H")
    optional = pe + 24
    magic = integer(optional, "<H")
    if magic == 0x20B:
        base = integer(optional + 24, "<Q")
        directories = optional + 112
        directory_count = integer(optional + 108, "<I")
    elif magic == 0x10B:
        base = integer(optional + 28, "<I")
        directories = optional + 96
        directory_count = integer(optional + 92, "<I")
    else:
        raise ValueError("Unsupported PE optional header")
    if directories + min(directory_count, 16) * 8 > optional + optional_size:
        raise ValueError("Truncated PE data directory")
    table = optional + optional_size
    ranges = []
    for index in range(sections):
        offset = table + index * 40
        rva = integer(offset + 12, "<I")
        size = integer(offset + 16, "<I")
        raw = integer(offset + 20, "<I")
        if raw + size > len(data):
            raise ValueError("Truncated PE section")
        ranges.append((rva, size, raw))

    def position(rva, count):
        for start, size, raw in ranges:
            if start <= rva and rva + count <= start + size:
                return raw + rva - start
        raise ValueError("PE RVA outside file-backed section")

    def name(rva):
        result = bytearray()
        for index in range(260):
            value = data[position(rva + index, 1)]
            if value == 0:
                text = result.decode("ascii")
                if not text or "/" in text or "\\" in text:
                    raise ValueError("Invalid PE import name")
                return text
            result.append(value)
        raise ValueError("Unterminated PE import name")

    imports = []
    for slot, descriptor_size in ((1, 20), (13, 32)):
        if slot >= directory_count:
            continue
        rva = integer(directories + slot * 8, "<I")
        size = integer(directories + slot * 8 + 4, "<I")
        if not rva and not size:
            continue
        if not rva or size < descriptor_size:
            raise ValueError("Invalid PE import directory")
        terminated = False
        for offset in range(0, size - descriptor_size + 1, descriptor_size):
            descriptor = position(rva + offset, descriptor_size)
            values = struct.unpack_from("<" + "I" * (descriptor_size // 4), data, descriptor)
            if not any(values):
                terminated = True
                break
            name_rva = values[3] if slot == 1 else values[1]
            if slot == 13 and not values[0] & 1:
                name_rva -= base
            imports.append({"name": name(name_rva), "kind": "static" if slot == 1 else "delay"})
        if not terminated:
            raise ValueError("Unterminated PE import directory")
    return imports


def inventory(qnn_dir, driver_dirs=()):
    qnn_dir = Path(qnn_dir).resolve()
    system = Path(os.environ["SystemRoot"]) / "System32"
    pending = [qnn_dir / name for name in ("QnnHtp.dll", "QnnHtpPrepare.dll", "QnnHtpV73Stub.dll")]
    records = {}
    unresolved = set()
    while pending:
        path = pending.pop()
        key = str(path).casefold()
        if key in records:
            continue
        data = path.read_bytes()
        imports = pe_imports(data)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        machine = struct.unpack_from("<H", data, pe + 4)[0]
        if machine != 0xAA64:
            raise ValueError(f"Expected ARM64 vendor DLL: {path}")
        record = {"file": str(path), "machine": "ARM64", "size": len(data),
                  "sha256": digest(path), "imports": imports}
        records[key] = record
        for entry in imports:
            dll = entry["name"]
            if dll.lower().startswith(("api-ms-", "ext-ms-")):
                entry["resolution"] = "Windows API-set contract"
                continue
            matches = [root / dll for root in (qnn_dir, system, *driver_dirs) if (root / dll).is_file()]
            if not matches:
                unresolved.add(dll)
                entry["resolution"] = "unresolved"
                continue
            entry["candidate_files"] = [str(path) for path in matches]
            vendor = matches[0].parent == qnn_dir or dll.lower().startswith(("qnn", "libcdsp", "libadsp"))
            entry["resolution"] = ("vendor candidates; loaded selection not verified" if len(matches) > 1
                                   else "vendor") if vendor else "Windows/system runtime"
            if vendor:
                pending.extend(matches)
    payloads = []
    for name in ("libQnnHtpV73Skel.so", "libqnnhtpv73.cat", "qnn-runtime-version.txt"):
        path = qnn_dir / name
        payloads.append({"file": name, "size": path.stat().st_size, "sha256": digest(path)})
    return {"schema_version": 1, "method": "Static and delay PE imports; Qualcomm descendants recursively inspected",
            "scope": "Includes development-only QnnHtpPrepare.dll; no vendor DLL was loaded",
            "limitations": "Conditional LoadLibrary dependencies and selection between driver-store candidates require M1 process audit. OS/system import subtrees are classified, not recursively expanded.",
            "dlls": list(records.values()), "payloads": payloads, "unresolved": sorted(unresolved)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qnn-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--driver-dir", type=Path, action="append", default=[])
    args = parser.parse_args()
    result = inventory(args.qnn_dir, args.driver_dir)
    write_json(args.output, result)
    if result["unresolved"]:
        raise SystemExit("Unresolved imports: " + ", ".join(result["unresolved"]))
    print("PASS QNN static/delay import inventory")


if __name__ == "__main__":
    main()
