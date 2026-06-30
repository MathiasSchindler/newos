#!/usr/bin/env python3
import argparse
import csv
import os
import shutil
import statistics
import subprocess
import time
from pathlib import Path


DEFAULT_REPOS = ["https://github.com/octocat/Hello-World.git"]


def find_newos_git(root):
    env = os.environ.get("NEWOS_GIT")
    candidates = [
        Path(env) if env else None,
        root / "build" / "git",
        root / "build" / "host-linux-x86_64" / "git",
        root / "build" / "host-macos-aarch64" / "git",
    ]
    for candidate in candidates:
        if candidate and candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    raise RuntimeError("cannot find in-tree git; set NEWOS_GIT=/path/to/git")


def run(cmd, cwd=None, timeout=None, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL):
    start = time.perf_counter()
    completed = subprocess.run(cmd, cwd=cwd, stdout=stdout, stderr=stderr, timeout=timeout)
    return completed.returncode, time.perf_counter() - start


def run_text(cmd, cwd=None, timeout=None):
    try:
        completed = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout, text=True)
    except subprocess.TimeoutExpired:
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout


def clean_destination(path):
    if path.exists():
        shutil.rmtree(path)


def clone_command(git_cmd, repo, args, dest):
    return [git_cmd, "clone"] + args + [repo, str(dest)]


def collect_clone_metadata(git_cmd, dest, timeout):
    head = run_text([git_cmd, "-C", str(dest), "rev-parse", "HEAD"], timeout=timeout)
    status = run_text([git_cmd, "-C", str(dest), "status", "--short"], timeout=timeout)
    files = run_text([git_cmd, "-C", str(dest), "ls-files"], timeout=timeout)
    return {
        "head": (head or "").strip(),
        "status_clean": "1" if status == "" else "0",
        "file_count": str(len([line for line in (files or "").splitlines() if line])),
    }


def post_clone_checks():
    return [
        ("post-rev-parse-head", ["rev-parse", "HEAD"], (0,)),
        ("post-status-short", ["status", "--short"], (0,)),
        ("post-ls-files", ["ls-files"], (0,)),
    ]


def record_row(rows, repo, scenario, impl, operation, run_index, elapsed, rc, metadata=None, error=""):
    metadata = metadata or {}
    rows.append({
        "repo": repo,
        "scenario": scenario,
        "impl": impl,
        "operation": operation,
        "run": run_index,
        "seconds": f"{elapsed:.6f}",
        "exit_code": str(rc),
        "head": metadata.get("head", ""),
        "status_clean": metadata.get("status_clean", ""),
        "file_count": metadata.get("file_count", ""),
        "error": error,
    })


def record_post_clone_checks(rows, repo, scenario, impl, git_cmd, dest, run_index, timeout, sleep_seconds):
    for operation, args, allowed in post_clone_checks():
        rc = -1
        elapsed = 0.0
        error = ""
        try:
            rc, elapsed = run([git_cmd, "-C", str(dest)] + args, timeout=timeout)
        except subprocess.TimeoutExpired:
            error = "timeout"
            elapsed = float(timeout or 0)
        if rc not in allowed:
            error = error or "unexpected-exit"
        record_row(rows, repo, scenario, impl, operation, run_index, elapsed, rc, error=error)
        if sleep_seconds > 0:
            time.sleep(sleep_seconds)


def summarize(rows):
    groups = {}
    for row in rows:
        if row["exit_code"] != "0":
            continue
        key = (row["repo"], row["scenario"], row["operation"], row["impl"])
        groups.setdefault(key, []).append(float(row["seconds"]))

    print("repo,scenario,operation,impl,reps,min_s,median_s,mean_s,max_s")
    for key in sorted(groups):
        values = groups[key]
        print(
            f"{key[0]},{key[1]},{key[2]},{key[3]},"
            f"{len(values)},{min(values):.6f},{statistics.median(values):.6f},"
            f"{statistics.mean(values):.6f},{max(values):.6f}"
        )


def compare_compatibility(rows):
    by_run = {}
    for row in rows:
        if row["operation"] != "clone" or row["exit_code"] != "0":
            continue
        key = (row["repo"], row["scenario"], row["run"])
        by_run.setdefault(key, {})[row["impl"]] = row

    problems = []
    for key, impls in sorted(by_run.items()):
        if "newos" not in impls or "canonical" not in impls:
            continue
        newos = impls["newos"]
        canonical = impls["canonical"]
        for field in ("head", "status_clean", "file_count"):
            if newos[field] != canonical[field]:
                problems.append((key, field, newos[field], canonical[field]))
    return problems


def benchmark_clone(rows, root_work, repo, scenario_name, scenario_args, git_impls, reps, timeout, sleep_seconds):
    for run_index in range(reps):
        order = git_impls if run_index % 2 == 0 else list(reversed(git_impls))
        for impl, git_cmd in order:
            dest = root_work / f"{scenario_name}-{impl}-{run_index}"
            clean_destination(dest)
            rc = -1
            elapsed = 0.0
            error = ""
            metadata = {}
            try:
                rc, elapsed = run(clone_command(git_cmd, repo, scenario_args, dest), timeout=timeout)
                if rc == 0:
                    metadata = collect_clone_metadata(git_cmd, dest, timeout)
            except subprocess.TimeoutExpired:
                error = "timeout"
                elapsed = float(timeout or 0)
            record_row(rows, repo, scenario_name, impl, "clone", run_index, elapsed, rc, metadata, error)
            if rc == 0:
                record_post_clone_checks(rows, repo, scenario_name, impl, git_cmd, dest, run_index, timeout, sleep_seconds)
            if sleep_seconds > 0:
                time.sleep(sleep_seconds)


def benchmark_fetch_noop(rows, root_work, repo, scenario_name, scenario_args, git_impls, reps, timeout, sleep_seconds):
    for impl, git_cmd in git_impls:
        dest = root_work / f"{scenario_name}-{impl}-fetch-base"
        clean_destination(dest)
        setup_elapsed = 0.0
        try:
            rc, setup_elapsed = run(clone_command(git_cmd, repo, scenario_args, dest), timeout=timeout)
        except subprocess.TimeoutExpired:
            rc = -1
            setup_elapsed = float(timeout or 0)
        record_row(rows, repo, scenario_name, impl, "fetch-setup-clone", -1, setup_elapsed, rc, error="" if rc == 0 else "setup-failed")
        if rc != 0:
            record_row(rows, repo, scenario_name, impl, "fetch-noop-setup", -1, 0.0, rc, error="setup-failed")
            continue
        for run_index in range(reps):
            rc = -1
            elapsed = 0.0
            error = ""
            try:
                rc, elapsed = run([git_cmd, "-C", str(dest), "fetch"], timeout=timeout)
            except subprocess.TimeoutExpired:
                error = "timeout"
                elapsed = float(timeout or 0)
            record_row(rows, repo, scenario_name, impl, "fetch-noop", run_index, elapsed, rc, error=error)
            if sleep_seconds > 0:
                time.sleep(sleep_seconds)


def parse_scenarios(text):
    definitions = {
        "clone-shallow": ["--depth", "1"],
        "clone-filtered": ["--depth", "1", "--filter=blob:none"],
        "fetch-noop-shallow": ["--depth", "1"],
    }
    selected = []
    for name in [part.strip() for part in text.split(",") if part.strip()]:
        if name not in definitions:
            raise ValueError(f"unknown scenario: {name}")
        selected.append((name, definitions[name]))
    return selected


def main():
    parser = argparse.ArgumentParser(description="Opt-in remote Git benchmark for in-tree git versus canonical git.")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--work-dir", default=None)
    parser.add_argument("--repo", action="append", default=[], help="Remote repository URL; can be repeated.")
    parser.add_argument("--scenarios", default="clone-shallow,clone-filtered", help="Comma-separated: clone-shallow, clone-filtered, fetch-noop-shallow")
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--sleep", type=float, default=1.0)
    parser.add_argument("--keep-clones", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    work = Path(args.work_dir).resolve() if args.work_dir else root / "tests" / "tmp" / "benchmarks" / "git-remote"
    work.mkdir(parents=True, exist_ok=True)
    repos = args.repo or DEFAULT_REPOS
    scenarios = parse_scenarios(args.scenarios)
    newos_git = find_newos_git(root)
    git_impls = [("newos", newos_git), ("canonical", "git")]
    rows = []

    for repo in repos:
        repo_work = work / repo.replace("://", "_").replace("/", "_").replace(":", "_")
        clean_destination(repo_work)
        repo_work.mkdir(parents=True, exist_ok=True)
        for scenario_name, scenario_args in scenarios:
            if scenario_name.startswith("fetch-noop"):
                benchmark_fetch_noop(rows, repo_work, repo, scenario_name, scenario_args, git_impls, args.reps, args.timeout, args.sleep)
            else:
                benchmark_clone(rows, repo_work, repo, scenario_name, scenario_args, git_impls, args.reps, args.timeout, args.sleep)
        if not args.keep_clones:
            for child in repo_work.iterdir():
                if child.is_dir():
                    shutil.rmtree(child)

    out_csv = work / "results.csv"
    with out_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["repo", "scenario", "impl", "operation", "run", "seconds", "exit_code", "head", "status_clean", "file_count", "error"])
        writer.writeheader()
        writer.writerows(rows)

    summarize(rows)
    problems = compare_compatibility(rows)
    if problems:
        print("compatibility_mismatches:")
        for key, field, newos, canonical in problems:
            print(f"{key}: {field}: newos={newos!r} canonical={canonical!r}")
    print(f"wrote {out_csv}")


if __name__ == "__main__":
    main()
