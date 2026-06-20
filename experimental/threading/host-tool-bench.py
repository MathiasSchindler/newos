#!/usr/bin/env python3
import argparse
import csv
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path


HASH_TOOLS = (
    ("md5", "md5sum", "NEWOS_HASH_WORKERS"),
    ("sha1", "sha1sum", "NEWOS_HASH_WORKERS"),
    ("sha256", "sha256sum", "NEWOS_HASH_WORKERS"),
    ("sha512", "sha512sum", "NEWOS_HASH_WORKERS"),
)

PROJECT_WORKER_VARIANTS = (("project-w1", "1"), ("project-default", None))


class BenchError(Exception):
    pass


def shlex_join(argv):
    try:
        import shlex

        return shlex.join([str(item) for item in argv])
    except Exception:
        return " ".join(str(item) for item in argv)


def default_project_build_dir(root):
    system = platform.system()
    machine = platform.machine().lower()
    arch = "aarch64" if machine in ("arm64", "aarch64") else machine
    if system == "Darwin" and arch == "aarch64":
        return root / "build" / "macos-aarch64"
    if system == "Linux" and arch in ("x86_64", "aarch64"):
        return root / "build" / ("freestanding-linux-" + arch)
    host_os = "macos" if system == "Darwin" else system.lower()
    return root / "build" / ("host-" + host_os + "-" + arch)


def parse_args(argv):
    script = Path(__file__).resolve()
    root = script.parents[2]
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    parser = argparse.ArgumentParser(
        description="Compare host system tools with the project's equivalent tools."
    )
    parser.add_argument("--project-build-dir", default=str(default_project_build_dir(root)), help="directory containing project tool binaries")
    parser.add_argument("--output", default=str(root / "tests" / "tmp" / ("host-tool-bench-" + timestamp + ".csv")), help="CSV report path")
    parser.add_argument("--work-dir", default=str(root / "tests" / "tmp" / ("host-tool-bench-work-" + timestamp)), help="scratch directory")
    parser.add_argument("--repeat", type=int, default=3, help="timed repetitions per case")
    parser.add_argument("--size-mib", type=int, default=32, help="payload size for hash and compression fixtures")
    parser.add_argument("--sort-lines", type=int, default=300000, help="number of lines for sort fixture")
    parser.add_argument("--cases", default="sort,hash,bzip2", help="comma-separated subset: sort,hash,bzip2,all")
    parser.add_argument("--build", action="store_true", help="build the required project tools before running")
    parser.add_argument("--keep-work", action="store_true", help="keep scratch fixtures and outputs")
    parser.add_argument("--list-tools", action="store_true", help="print detected host/project tools and exit")
    return parser.parse_args(argv)


def normalize_cases(text):
    cases = {part.strip() for part in text.split(",") if part.strip()}
    if not cases or "all" in cases:
        return {"sort", "hash", "bzip2"}
    unknown = cases - {"sort", "hash", "bzip2"}
    if unknown:
        raise BenchError("unknown case(s): " + ", ".join(sorted(unknown)))
    return cases


def require_host_tool(names, required=True):
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    if required:
        raise BenchError("missing host tool: " + " or ".join(names))
    return None


def project_tool(project_dir, name):
    path = project_dir / name
    if not path.exists() or not os.access(path, os.X_OK):
        raise BenchError("missing project tool: " + str(path))
    return str(path)


def build_project_tools(root, project_dir, tools):
    targets = []
    for tool in tools:
        path = project_dir / tool
        try:
            targets.append(str(path.relative_to(root)))
        except ValueError:
            raise BenchError("--build requires --project-build-dir to be inside the repository")
    subprocess.run(["make", "-C", str(root)] + targets, check=True)


def write_payload(path, size_mib):
    target = size_mib * 1024 * 1024
    line_index = 0
    with path.open("wb") as handle:
        while handle.tell() < target:
            line = ("newos benchmark payload line %08d abcdefghijklmnopqrstuvwxyz 0123456789\n" % line_index).encode("ascii")
            remaining = target - handle.tell()
            handle.write(line[:remaining])
            line_index += 1


def write_sort_fixture(path, line_count):
    value = 0x12345678
    with path.open("w", encoding="ascii") as handle:
        for index in range(line_count):
            value = (value * 1103515245 + 12345) & 0x7fffffff
            secondary = (value ^ (index * 2654435761)) & 0xffffffff
            handle.write("%010u field-%08x line-%08u\n" % (value, secondary, index))


def remove_path(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def run_timed(argv, repeat, stdout_path=None, cwd=None, env=None, before=None):
    times = []
    for _ in range(repeat):
        if before is not None:
            before()
        start = time.perf_counter()
        if stdout_path is None:
            completed = subprocess.run(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        else:
            with stdout_path.open("wb") as stdout:
                completed = subprocess.run(argv, cwd=cwd, env=env, stdout=stdout, stderr=subprocess.PIPE)
        elapsed = time.perf_counter() - start
        if completed.returncode != 0:
            stderr = completed.stderr.decode("utf-8", "replace").strip()
            raise BenchError("command failed (%d): %s\n%s" % (completed.returncode, shlex_join(argv), stderr))
        times.append(elapsed)
    times.sort()
    return {
        "median": times[len(times) // 2],
        "min": times[0],
        "max": times[-1],
    }


def command_output(argv, env=None):
    completed = subprocess.run(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode != 0:
        raise BenchError("command failed (%d): %s" % (completed.returncode, shlex_join(argv)))
    return completed.stdout.decode("utf-8", "replace")


def first_hex_word(text):
    for token in text.replace("\n", " ").split():
        if all(ch in "0123456789abcdefABCDEF" for ch in token):
            return token.lower()
    return ""


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def files_equal(left, right):
    if not left.exists() or not right.exists():
        return False
    return file_sha256(left) == file_sha256(right)


def project_bunzip2_output_path(compressed_path):
    text = str(compressed_path)
    if text.endswith(".bz2"):
        return Path(text[:-4])
    return Path(text + ".out")


def row(case, variant, tool, timings, repeat, input_bytes, input_items, output_path, ratio, ok, note, command):
    output_bytes = output_path.stat().st_size if output_path is not None and output_path.exists() else ""
    return {
        "case": case,
        "variant": variant,
        "tool": tool,
        "median_real_s": "%.6f" % timings["median"],
        "min_real_s": "%.6f" % timings["min"],
        "max_real_s": "%.6f" % timings["max"],
        "repeat": str(repeat),
        "input_bytes": str(input_bytes),
        "input_items": str(input_items),
        "output_bytes": str(output_bytes),
        "project_time_over_host": "" if ratio is None else "%.3f" % ratio,
        "ok": "yes" if ok else "no",
        "note": note,
        "command": shlex_join(command),
    }


def env_with_worker(knob, value):
    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    if value is None:
        env.pop(knob, None)
    else:
        env[knob] = value
    return env


def bench_sort(project_dir, work_dir, repeat, line_count):
    host_sort = require_host_tool(["sort"])
    project_sort = project_tool(project_dir, "sort")
    fixture = work_dir / "sort-input.txt"
    host_out = work_dir / "sort-host.out"
    project_out = work_dir / "sort-project.out"
    write_sort_fixture(fixture, line_count)
    input_bytes = fixture.stat().st_size

    host_cmd = [host_sort, str(fixture)]
    host_timings = run_timed(host_cmd, repeat, stdout_path=host_out, env=env_with_worker("NEWOS_SORT_WORKERS", None))
    rows = [row("sort", "host", host_sort, host_timings, repeat, input_bytes, line_count, host_out, None, True, "LC_ALL=C", host_cmd)]

    for variant, worker_value in PROJECT_WORKER_VARIANTS:
        project_cmd = [project_sort, str(fixture)]
        env = env_with_worker("NEWOS_SORT_WORKERS", worker_value)
        timings = run_timed(project_cmd, repeat, stdout_path=project_out, env=env)
        ok = files_equal(host_out, project_out)
        note = "NEWOS_SORT_WORKERS=" + (worker_value if worker_value is not None else "default")
        rows.append(row("sort", variant, project_sort, timings, repeat, input_bytes, line_count, project_out, timings["median"] / host_timings["median"], ok, note, project_cmd))
    return rows


def host_hash_command(algorithm, path):
    if algorithm == "md5":
        md5sum = shutil.which("md5sum")
        if md5sum:
            return [md5sum, str(path)]
        md5 = require_host_tool(["md5"])
        return [md5, "-q", str(path)]
    sum_tool = shutil.which(algorithm + "sum")
    if sum_tool:
        return [sum_tool, str(path)]
    shasum = require_host_tool(["shasum"])
    return [shasum, "-a", algorithm[3:], str(path)]


def bench_hash(project_dir, work_dir, repeat, payload):
    rows = []
    input_bytes = payload.stat().st_size
    for algorithm, project_name, knob in HASH_TOOLS:
        host_cmd = host_hash_command(algorithm, payload)
        host_out = work_dir / (algorithm + "-host.out")
        host_timings = run_timed(host_cmd, repeat, stdout_path=host_out, env=env_with_worker(knob, None))
        host_digest = first_hex_word(host_out.read_text(encoding="utf-8"))
        rows.append(row(algorithm, "host", host_cmd[0], host_timings, repeat, input_bytes, 1, host_out, None, bool(host_digest), "", host_cmd))

        project = project_tool(project_dir, project_name)
        for variant, worker_value in PROJECT_WORKER_VARIANTS:
            project_cmd = [project, str(payload)]
            project_out = work_dir / (algorithm + "-" + variant + ".out")
            env = env_with_worker(knob, worker_value)
            timings = run_timed(project_cmd, repeat, stdout_path=project_out, env=env)
            project_digest = first_hex_word(project_out.read_text(encoding="utf-8"))
            ok = host_digest != "" and project_digest == host_digest
            note = knob + "=" + (worker_value if worker_value is not None else "default")
            rows.append(row(algorithm, variant, project, timings, repeat, input_bytes, 1, project_out, timings["median"] / host_timings["median"], ok, note, project_cmd))
    return rows


def host_bzip2_decompress_command(compressed_path):
    bunzip2 = shutil.which("bunzip2")
    if bunzip2:
        return [bunzip2, "-c", str(compressed_path)]
    bzip2 = require_host_tool(["bzip2"])
    return [bzip2, "-d", "-c", str(compressed_path)]


def make_host_standard_bzip2(host_bzip2, payload, compressed_path):
    remove_path(compressed_path)
    with compressed_path.open("wb") as stdout:
        completed = subprocess.run([host_bzip2, "-c", str(payload)], stdout=stdout, stderr=subprocess.PIPE)
    if completed.returncode != 0:
        raise BenchError("host bzip2 could not create standard fixture")


def make_project_bzip2(project_bzip2, input_path):
    output = Path(str(input_path) + ".bz2")
    remove_path(output)
    completed = subprocess.run([project_bzip2, str(input_path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode != 0:
        raise BenchError("project bzip2 could not create project-format fixture")
    return output


def bench_bzip2(project_dir, work_dir, repeat, payload):
    rows = []
    input_bytes = payload.stat().st_size
    host_bzip2 = require_host_tool(["bzip2"])
    project_bzip2 = project_tool(project_dir, "bzip2")
    project_bunzip2 = project_tool(project_dir, "bunzip2")

    host_compressed = work_dir / "payload.host.bz2"
    host_compress_cmd = [host_bzip2, "-c", str(payload)]
    host_compress_timings = run_timed(host_compress_cmd, repeat, stdout_path=host_compressed)
    rows.append(row("bzip2-compress", "host-standard", host_bzip2, host_compress_timings, repeat, input_bytes, 1, host_compressed, None, host_compressed.exists(), "standard .bz2 output", host_compress_cmd))

    project_input = work_dir / "payload.project-input"
    shutil.copyfile(payload, project_input)
    project_compressed = Path(str(project_input) + ".bz2")
    project_compress_cmd = [project_bzip2, str(project_input)]

    def before_project_compress():
        remove_path(project_compressed)

    project_compress_timings = run_timed(project_compress_cmd, repeat, before=before_project_compress)
    project_roundtrip = project_bunzip2_output_path(project_compressed)
    remove_path(project_roundtrip)
    completed = subprocess.run([project_bunzip2, str(project_compressed)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ok = completed.returncode == 0 and files_equal(payload, project_roundtrip)
    rows.append(row("bzip2-compress", "project-format", project_bzip2, project_compress_timings, repeat, input_bytes, 1, project_compressed, project_compress_timings["median"] / host_compress_timings["median"], ok, "project BZh0 format; throughput ratio is not standard-bzip2 equivalent", project_compress_cmd))

    standard_compressed = work_dir / "payload.standard.bz2"
    make_host_standard_bzip2(host_bzip2, payload, standard_compressed)
    host_decompress_out = work_dir / "payload.host-decompressed"
    host_decompress_cmd = host_bzip2_decompress_command(standard_compressed)
    host_decompress_timings = run_timed(host_decompress_cmd, repeat, stdout_path=host_decompress_out)
    host_ok = files_equal(payload, host_decompress_out)
    rows.append(row("bzip2-decompress-standard", "host", host_decompress_cmd[0], host_decompress_timings, repeat, standard_compressed.stat().st_size, 1, host_decompress_out, None, host_ok, "standard .bz2 input", host_decompress_cmd))

    for variant, worker_value in PROJECT_WORKER_VARIANTS:
        project_standard = work_dir / ("payload.standard." + variant + ".bz2")
        shutil.copyfile(standard_compressed, project_standard)
        project_decompress_out = project_bunzip2_output_path(project_standard)
        project_decompress_cmd = [project_bunzip2, str(project_standard)]
        env = env_with_worker("NEWOS_BUNZIP2_WORKERS", worker_value)

        def before_project_decompress(path=project_decompress_out):
            remove_path(path)

        timings = run_timed(project_decompress_cmd, repeat, env=env, before=before_project_decompress)
        ok = files_equal(payload, project_decompress_out)
        note = "standard .bz2 input; NEWOS_BUNZIP2_WORKERS=" + (worker_value if worker_value is not None else "default")
        rows.append(row("bzip2-decompress-standard", variant, project_bunzip2, timings, repeat, standard_compressed.stat().st_size, 1, project_decompress_out, timings["median"] / host_decompress_timings["median"], ok, note, project_decompress_cmd))

    project_minimal = make_project_bzip2(project_bzip2, project_input)
    minimal_out = project_bunzip2_output_path(project_minimal)
    project_minimal_cmd = [project_bunzip2, str(project_minimal)]
    for variant, worker_value in PROJECT_WORKER_VARIANTS:
        env = env_with_worker("NEWOS_BUNZIP2_WORKERS", worker_value)

        def before_minimal():
            remove_path(minimal_out)

        timings = run_timed(project_minimal_cmd, repeat, env=env, before=before_minimal)
        ok = files_equal(payload, minimal_out)
        note = "project BZh0 input; no host equivalent"
        rows.append(row("bzip2-decompress-project-format", variant, project_bunzip2, timings, repeat, project_minimal.stat().st_size, 1, minimal_out, None, ok, note, project_minimal_cmd))
    return rows


def write_report(path, metadata, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "case",
        "variant",
        "tool",
        "median_real_s",
        "min_real_s",
        "max_real_s",
        "repeat",
        "input_bytes",
        "input_items",
        "output_bytes",
        "project_time_over_host",
        "ok",
        "note",
        "command",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        for key, value in metadata.items():
            handle.write("# %s=%s\n" % (key, value))
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for item in rows:
            writer.writerow(item)


def print_summary(rows, output_path):
    print("wrote", output_path)
    print("case,variant,median_s,project_time_over_host,ok,note")
    for item in rows:
        ratio = item["project_time_over_host"]
        if item["variant"].startswith("project") or item["variant"] == "project-format":
            print(
                "%s,%s,%s,%s,%s,%s"
                % (item["case"], item["variant"], item["median_real_s"], ratio, item["ok"], item["note"])
            )


def detect_tool_report(project_dir):
    names = ["sort", "bzip2", "bunzip2", "md5sum", "sha1sum", "sha256sum", "sha512sum"]
    print("project_build_dir", project_dir)
    for name in names:
        path = project_dir / name
        print("project", name, path, "ok" if path.exists() and os.access(path, os.X_OK) else "missing")
    for name in ["sort", "bzip2", "bunzip2", "md5sum", "md5", "sha1sum", "sha256sum", "sha512sum", "shasum"]:
        print("host", name, shutil.which(name) or "missing")


def main(argv):
    args = parse_args(argv)
    root = Path(__file__).resolve().parents[2]
    project_dir = Path(args.project_build_dir)
    if not project_dir.is_absolute():
        project_dir = root / project_dir
    work_dir = Path(args.work_dir)
    if not work_dir.is_absolute():
        work_dir = root / work_dir
    output = Path(args.output)
    if not output.is_absolute():
        output = root / output
    cases = normalize_cases(args.cases)
    repeat = max(1, args.repeat)

    if args.list_tools:
        detect_tool_report(project_dir)
        return 0

    required_tools = set()
    if "sort" in cases:
        required_tools.add("sort")
    if "hash" in cases:
        required_tools.update(name for _, name, _ in HASH_TOOLS)
    if "bzip2" in cases:
        required_tools.update(["bzip2", "bunzip2"])
    if args.build:
        build_project_tools(root, project_dir, sorted(required_tools))

    for tool in sorted(required_tools):
        project_tool(project_dir, tool)

    if work_dir.exists() and not args.keep_work:
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)

    payload = work_dir / "payload.txt"
    write_payload(payload, args.size_mib)

    rows = []
    if "sort" in cases:
        rows.extend(bench_sort(project_dir, work_dir, repeat, args.sort_lines))
    if "hash" in cases:
        rows.extend(bench_hash(project_dir, work_dir, repeat, payload))
    if "bzip2" in cases:
        rows.extend(bench_bzip2(project_dir, work_dir, repeat, payload))

    metadata = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "root": root,
        "project_build_dir": project_dir,
        "work_dir": work_dir,
        "repeat": repeat,
        "size_mib": args.size_mib,
        "sort_lines": args.sort_lines,
        "ratio_note": "project_time_over_host < 1 means the project tool was faster than the host tool",
    }
    write_report(output, metadata, rows)
    print_summary(rows, output)
    if not args.keep_work:
        shutil.rmtree(work_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except BenchError as error:
        print("host-tool-bench: " + str(error), file=sys.stderr)
        raise SystemExit(1)