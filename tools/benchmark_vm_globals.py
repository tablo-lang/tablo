#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def find_benchmark_exe(config: str) -> Path:
    root = repo_root()
    candidates = [
        root / "build" / config / "benchmark_global_lookup.exe",
        root / "build" / config / "benchmark_global_lookup",
        root / "build" / "benchmark_global_lookup.exe",
        root / "build" / "benchmark_global_lookup",
    ]
    for path in candidates:
        if path.exists():
            return path
    raise FileNotFoundError(f"Could not find benchmark_global_lookup binary. Looked in: {', '.join(str(p) for p in candidates)}")


def build_benchmark_exe(config: str) -> None:
    cmd = ["cmake", "--build", "build", "--config", config, "--target", "benchmark_global_lookup"]
    proc = subprocess.run(cmd, cwd=repo_root(), capture_output=True, text=True)
    if proc.returncode == 0:
        return
    if proc.stdout.strip():
        print(proc.stdout.strip(), file=sys.stderr)
    if proc.stderr.strip():
        print(proc.stderr.strip(), file=sys.stderr)
    raise RuntimeError(f"failed to build benchmark_global_lookup target (config={config})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run VM globals/string-pool microbenchmark.")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"], help="Build config containing benchmark binary")
    parser.add_argument("--scale", type=int, default=1, help="Scale factor passed to benchmark binary")
    parser.add_argument("--samples-per-mode", type=int, default=1, help="Compatibility argument used by perf-gate runner")
    args = parser.parse_args(argv)

    if args.scale < 1:
        print("error: --scale must be >= 1", file=sys.stderr)
        return 2
    if args.samples_per_mode < 1:
        print("error: --samples-per-mode must be >= 1", file=sys.stderr)
        return 2

    try:
        bench_exe = find_benchmark_exe(args.config)
    except FileNotFoundError as exc:
        try:
            build_benchmark_exe(args.config)
            bench_exe = find_benchmark_exe(args.config)
        except (RuntimeError, FileNotFoundError):
            print(f"error: {exc}", file=sys.stderr)
            return 2

    cmd = [str(bench_exe), str(args.scale)]
    proc = subprocess.run(cmd, cwd=repo_root(), capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"error: benchmark_global_lookup failed with code {proc.returncode}", file=sys.stderr)
        if proc.stdout.strip():
            print(proc.stdout.strip(), file=sys.stderr)
        if proc.stderr.strip():
            print(proc.stderr.strip(), file=sys.stderr)
        return proc.returncode

    if proc.stdout:
        print(proc.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
