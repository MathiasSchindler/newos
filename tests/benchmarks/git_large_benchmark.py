#!/usr/bin/env python3
import argparse
import csv
import os
import shutil
import statistics
import subprocess
import time
from pathlib import Path


def run(cmd, cwd=None, allowed=(0,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL):
    completed = subprocess.run(cmd, cwd=cwd, stdout=stdout, stderr=stderr)
    if completed.returncode not in allowed:
        raise RuntimeError(f"command failed rc={completed.returncode}: {' '.join(cmd)}")
    return completed


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


def write_file(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def create_fixture(repo, files, dirs, commits, dirty_files, untracked_files):
    if repo.exists():
        shutil.rmtree(repo)
    repo.mkdir(parents=True)
    run(["git", "init", str(repo)])
    run(["git", "-C", str(repo), "config", "user.name", "Bench"])
    run(["git", "-C", str(repo), "config", "user.email", "bench@example.invalid"])

    for i in range(files):
        directory = repo / f"dir-{i % dirs:04d}"
        write_file(directory / f"file-{i:06d}.txt", f"base file {i:06d}\nline {i % 97}\n")
    run(["git", "-C", str(repo), "add", "."])
    run(["git", "-C", str(repo), "commit", "-m", "base"])

    if commits > 1:
        stride = max(1, files // max(1, commits - 1))
        for commit in range(1, commits):
            start = (commit * stride) % files
            for j in range(8):
                idx = (start + j) % files
                path = repo / f"dir-{idx % dirs:04d}" / f"file-{idx:06d}.txt"
                with path.open("a", encoding="utf-8") as handle:
                    handle.write(f"commit {commit:04d} update {j}\n")
            run(["git", "-C", str(repo), "add", "."])
            run(["git", "-C", str(repo), "commit", "-m", f"update-{commit:04d}"])

    for i in range(min(dirty_files, files)):
        path = repo / f"dir-{i % dirs:04d}" / f"file-{i:06d}.txt"
        with path.open("a", encoding="utf-8") as handle:
            handle.write(f"dirty update {i:06d}\n")
    for i in range(untracked_files):
        write_file(repo / "scratch" / f"scratch-{i:06d}.txt", f"scratch {i:06d}\n")


def measure(cmd, cwd, reps, allowed=(0,)):
    for _ in range(2):
        run(cmd, cwd=cwd, allowed=allowed)
    samples = []
    for _ in range(reps):
        start = time.perf_counter()
        run(cmd, cwd=cwd, allowed=allowed)
        samples.append(time.perf_counter() - start)
    return {
        "reps": reps,
        "min": min(samples),
        "median": statistics.median(samples),
        "mean": statistics.mean(samples),
        "max": max(samples),
    }


def main():
    parser = argparse.ArgumentParser(description="Benchmark in-tree git against system git on deterministic large local fixtures.")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--work-dir", default=None)
    parser.add_argument("--files", type=int, default=5000)
    parser.add_argument("--dirs", type=int, default=100)
    parser.add_argument("--commits", type=int, default=40)
    parser.add_argument("--dirty-files", type=int, default=200)
    parser.add_argument("--untracked-files", type=int, default=200)
    parser.add_argument("--reps", type=int, default=7)
    parser.add_argument("--recreate", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    work = Path(args.work_dir).resolve() if args.work_dir else root / "tests" / "tmp" / "benchmarks" / "git-large"
    repo = work / "repo"
    out_csv = work / "results.csv"
    newos_git = find_newos_git(root)

    if args.recreate or not (repo / ".git").exists():
        create_fixture(repo, args.files, args.dirs, args.commits, args.dirty_files, args.untracked_files)

    tasks = [
        ("rev-parse-head", ["rev-parse", "HEAD"], (0,)),
        ("branch-current", ["branch", "--show-current"], (0,)),
        ("ls-files", ["ls-files"], (0,)),
        ("log-oneline-40", ["log", "--oneline", "-n", "40"], (0,)),
        ("status-short", ["status", "--short"], (0,)),
        ("status-z", ["status", "--porcelain=v1", "-z"], (0,)),
        ("ls-modified-stage", ["ls-files", "-z", "--modified", "--deleted", "--stage"], (0,)),
        ("ls-others", ["ls-files", "--others", "--exclude-standard"], (0,)),
        ("diff-name-status", ["diff", "--name-status"], (0,)),
        ("diff-stat", ["diff", "--stat"], (0,)),
        ("diff-quiet", ["diff", "--quiet"], (0, 1)),
    ]

    rows = []
    print("task,impl,median_ms,ratio")
    by_task = {}
    for task, git_args, allowed in tasks:
        by_task[task] = {}
        for impl, git_cmd in (("newos", newos_git), ("canonical", "git")):
            result = measure([git_cmd] + git_args, repo, args.reps, allowed=allowed)
            row = {"task": task, "impl": impl, **result}
            rows.append(row)
            by_task[task][impl] = row
        ratio = by_task[task]["newos"]["median"] / by_task[task]["canonical"]["median"]
        print(f"{task},newos,{by_task[task]['newos']['median'] * 1000.0:.3f},{ratio:.2f}x")
        print(f"{task},canonical,{by_task[task]['canonical']['median'] * 1000.0:.3f},1.00x")

    work.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["task", "impl", "reps", "min", "median", "mean", "max"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {out_csv}")


if __name__ == "__main__":
    main()
