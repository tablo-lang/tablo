#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Tuple


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_path(path_text: str) -> Path:
    path = Path(path_text)
    if not path.is_absolute():
        path = repo_root() / path
    return path.resolve()


def run_cmd(cmd: List[str]) -> Tuple[int, str, str]:
    proc = subprocess.run(cmd, cwd=repo_root(), capture_output=True, text=True)
    return proc.returncode, proc.stdout or "", proc.stderr or ""


def print_output(stdout: str, stderr: str) -> None:
    if stdout.strip():
        print(stdout.strip())
    if stderr.strip():
        print(stderr.strip(), file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run fast-string baseline benchmark and compare against a reference snapshot."
    )
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"], help="Build config containing tablo binary")
    parser.add_argument("--baseline", required=True, help="Baseline JSON from benchmark_fast_string_baseline.py")
    parser.add_argument("--scale", type=int, default=1, help="Scale factor for benchmark program")
    parser.add_argument("--samples", type=int, default=3, help="Measured benchmark samples for candidate snapshot")
    parser.add_argument("--warmup-runs", type=int, default=1, help="Warmup runs before measured samples")
    parser.add_argument("--transient-retries", type=int, default=1, help="Retries for transient benchmark process failures")
    parser.add_argument(
        "--max-metric-regression-pct",
        type=float,
        default=8.0,
        help="Allowed drop (percent) for median_ops_per_s metrics",
    )
    parser.add_argument(
        "--max-speedup-regression-pct",
        type=float,
        default=8.0,
        help="Allowed drop (percent) for speedup values",
    )
    parser.add_argument("--allow-metadata-mismatch", action="store_true", help="Allow config/scale/program mismatch between snapshots")
    parser.add_argument("--candidate-out", default="", help="Optional candidate snapshot output path")
    parser.add_argument("--compare-out", default="", help="Optional comparison report output path")
    args = parser.parse_args()

    if args.scale < 1:
        print("error: --scale must be >= 1", file=sys.stderr)
        return 2
    if args.samples < 1:
        print("error: --samples must be >= 1", file=sys.stderr)
        return 2
    if args.warmup_runs < 0:
        print("error: --warmup-runs must be >= 0", file=sys.stderr)
        return 2
    if args.transient_retries < 0:
        print("error: --transient-retries must be >= 0", file=sys.stderr)
        return 2
    if args.max_metric_regression_pct < 0:
        print("error: --max-metric-regression-pct must be >= 0", file=sys.stderr)
        return 2
    if args.max_speedup_regression_pct < 0:
        print("error: --max-speedup-regression-pct must be >= 0", file=sys.stderr)
        return 2

    baseline_path = resolve_path(args.baseline)
    if not baseline_path.exists():
        print(f"error: baseline snapshot not found: {baseline_path}", file=sys.stderr)
        return 2

    root = repo_root()
    benchmark_script = root / "tools" / "benchmark_fast_string_baseline.py"
    compare_script = root / "tools" / "compare_fast_string_baseline.py"
    if not benchmark_script.exists():
        print(f"error: missing script: {benchmark_script}", file=sys.stderr)
        return 2
    if not compare_script.exists():
        print(f"error: missing script: {compare_script}", file=sys.stderr)
        return 2

    def run_flow(candidate_path: Path) -> int:
        baseline_cmd = [
            sys.executable,
            str(benchmark_script),
            "--config",
            args.config,
            "--scale",
            str(args.scale),
            "--samples",
            str(args.samples),
            "--warmup-runs",
            str(args.warmup_runs),
            "--transient-retries",
            str(args.transient_retries),
            "--out",
            str(candidate_path),
        ]
        rc, out, err = run_cmd(baseline_cmd)
        print_output(out, err)
        if rc != 0:
            print("error: failed to generate candidate fast-string snapshot", file=sys.stderr)
            return rc if rc != 0 else 1

        compare_cmd = [
            sys.executable,
            str(compare_script),
            str(baseline_path),
            str(candidate_path),
            "--max-metric-regression-pct",
            str(args.max_metric_regression_pct),
            "--max-speedup-regression-pct",
            str(args.max_speedup_regression_pct),
        ]
        if args.allow_metadata_mismatch:
            compare_cmd.append("--allow-metadata-mismatch")
        if args.compare_out:
            compare_cmd.extend(["--out", str(resolve_path(args.compare_out))])

        rc, out, err = run_cmd(compare_cmd)
        print_output(out, err)
        return rc if rc != 0 else 0

    if args.candidate_out:
        candidate_path = resolve_path(args.candidate_out)
        candidate_path.parent.mkdir(parents=True, exist_ok=True)
        return run_flow(candidate_path)

    with tempfile.TemporaryDirectory(prefix="tablo_fast_string_candidate_") as tmpdir:
        candidate_path = Path(tmpdir) / "candidate.json"
        return run_flow(candidate_path)


if __name__ == "__main__":
    raise SystemExit(main())
