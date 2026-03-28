#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Tuple


OPS_RE = re.compile(r"^(?P<label>.+): (?P<count>\d+) ops in (?P<ms>\d+) ms \((?P<rate>\d+) ops/s\)\s*$")
TRANSIENT_FAILURE_PATTERNS = (
    "raw request warmup failed",
    "raw request failed",
    "connection failed",
    "thread join timed out",
)

# (fast_label, baseline_label, scenario)
FAST_STRING_SPEEDUPS: List[Tuple[str, str, str]] = [
    (
        "perf map<string,int> fast-string set+has+get",
        "perf map<string,int> set+has+get",
        "map<string,int> set+has+get",
    ),
    (
        "perf map<string,int> fast-string read-heavy has+get",
        "perf map<string,int> read-heavy has+get",
        "map<string,int> read-heavy has+get",
    ),
    (
        "perf set<string> fast-string add+has",
        "perf set<string> add+has",
        "set<string> add+has",
    ),
    (
        "perf map/set churn fast-string del+add+probe",
        "perf map/set churn del+add+probe",
        "map/set churn del+add+probe",
    ),
]

TRACKED_LABELS: List[str] = sorted({item for pair in FAST_STRING_SPEEDUPS for item in pair[:2]})


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def find_tablo_exe(config: str) -> Path:
    root = repo_root()
    candidates = [
        root / "build" / config / "tablo.exe",
        root / "build" / config / "tablo",
        root / "build" / "tablo.exe",
        root / "build" / "tablo",
    ]
    for path in candidates:
        if path.exists():
            return path
    raise FileNotFoundError(f"Could not find tablo binary. Looked in: {', '.join(str(p) for p in candidates)}")


def parse_ops_rates(stdout: str) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for line in stdout.splitlines():
        m = OPS_RE.match(line)
        if m:
            out[m.group("label")] = int(m.group("rate"))
    return out


def run_cmd(cmd: List[str]) -> Tuple[int, str, str]:
    proc = subprocess.run(cmd, cwd=repo_root(), capture_output=True, text=True)
    return proc.returncode, proc.stdout or "", proc.stderr or ""


def is_transient_benchmark_failure(stdout: str, stderr: str) -> bool:
    combined = (stdout + "\n" + stderr).lower()
    return any(pattern in combined for pattern in TRANSIENT_FAILURE_PATTERNS)


def run_with_transient_retries(
    cmd: List[str],
    *,
    transient_retries: int,
    phase: str,
    program_rel: str,
) -> Tuple[int, str, str]:
    attempt = 0
    while True:
        rc, out, err = run_cmd(cmd)
        if rc == 0:
            return rc, out, err
        if attempt >= transient_retries or not is_transient_benchmark_failure(out, err):
            return rc, out, err
        attempt += 1
        print(
            f"warn: transient benchmark {phase} failure ({program_rel}), retrying {attempt}/{transient_retries}",
            file=sys.stderr,
        )


def metrics_summary(label: str, rates: List[int]) -> Dict[str, object]:
    ordered = list(rates)
    ordered.sort()
    return {
        "label": label,
        "samples": rates,
        "min_ops_per_s": ordered[0],
        "max_ops_per_s": ordered[-1],
        "median_ops_per_s": int(statistics.median_low(ordered)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Capture repeatable fast-string benchmark baselines and fast-vs-generic speedups."
    )
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"], help="Build config containing tablo binary")
    parser.add_argument("--scale", type=int, default=1, help="Scale passed to benchmark_perf_gates.tblo")
    parser.add_argument("--samples", type=int, default=5, help="Measured sample runs")
    parser.add_argument("--warmup-runs", type=int, default=1, help="Warmup runs before measured samples")
    parser.add_argument("--transient-retries", type=int, default=1, help="Retries for transient benchmark process failures")
    parser.add_argument(
        "--program",
        default="tests/benchmark_perf_gates.tblo",
        help="Benchmark program path (must emit standard '<label>: ... (N ops/s)' lines)",
    )
    parser.add_argument("--out", default="", help="Optional JSON output file path")
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

    root = repo_root()
    program_path = (root / args.program).resolve()
    if not program_path.exists():
        print(f"error: benchmark program not found: {program_path}", file=sys.stderr)
        return 2

    try:
        tablo = find_tablo_exe(args.config)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    samples_by_label: Dict[str, List[int]] = {label: [] for label in TRACKED_LABELS}
    program_rel = str(program_path.relative_to(root))
    with tempfile.TemporaryDirectory(prefix="tablo_fast_string_") as tmpdir:
        artifact = Path(tmpdir) / (program_path.stem + ".tbc")

        compile_cmd = [str(tablo), "compile", str(program_path), "-o", str(artifact)]
        rc, out, err = run_cmd(compile_cmd)
        if rc != 0:
            print(f"error: benchmark compile failed ({program_rel})", file=sys.stderr)
            if out.strip():
                print(out.strip(), file=sys.stderr)
            if err.strip():
                print(err.strip(), file=sys.stderr)
            return rc if rc != 0 else 1

        run_cmdline = [str(tablo), "run", str(artifact), str(args.scale)]
        for _ in range(args.warmup_runs):
            rc, out, err = run_with_transient_retries(
                run_cmdline,
                transient_retries=args.transient_retries,
                phase="warmup",
                program_rel=program_rel,
            )
            if rc != 0:
                print(f"error: benchmark warmup failed ({program_rel})", file=sys.stderr)
                if out.strip():
                    print(out.strip(), file=sys.stderr)
                if err.strip():
                    print(err.strip(), file=sys.stderr)
                return rc if rc != 0 else 1

        for i in range(args.samples):
            rc, out, err = run_with_transient_retries(
                run_cmdline,
                transient_retries=args.transient_retries,
                phase="sample",
                program_rel=program_rel,
            )
            if rc != 0:
                print(f"error: benchmark sample run failed ({program_rel})", file=sys.stderr)
                if out.strip():
                    print(out.strip(), file=sys.stderr)
                if err.strip():
                    print(err.strip(), file=sys.stderr)
                return rc if rc != 0 else 1

            rates = parse_ops_rates(out)
            missing = [label for label in TRACKED_LABELS if label not in rates]
            if missing:
                print(
                    f"error: benchmark output missing tracked labels in sample {i + 1}/{args.samples}: {', '.join(missing)}",
                    file=sys.stderr,
                )
                return 1
            for label in TRACKED_LABELS:
                samples_by_label[label].append(rates[label])

    metric_summaries = [metrics_summary(label, samples_by_label[label]) for label in TRACKED_LABELS]
    by_label = {item["label"]: item for item in metric_summaries}

    speedups: List[Dict[str, object]] = []
    for fast_label, base_label, scenario in FAST_STRING_SPEEDUPS:
        fast_median = int(by_label[fast_label]["median_ops_per_s"])
        base_median = int(by_label[base_label]["median_ops_per_s"])
        ratio = 0.0 if base_median <= 0 else float(fast_median) / float(base_median)
        speedups.append(
            {
                "scenario": scenario,
                "fast_label": fast_label,
                "baseline_label": base_label,
                "median_fast_ops_per_s": fast_median,
                "median_baseline_ops_per_s": base_median,
                "speedup": ratio,
            }
        )

    result = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "config": args.config,
        "scale": args.scale,
        "program": program_rel,
        "samples": args.samples,
        "warmup_runs": args.warmup_runs,
        "tracked_labels": TRACKED_LABELS,
        "metrics": metric_summaries,
        "speedups": speedups,
    }

    print(f"Fast-string baseline ({program_rel})")
    print(f"  samples={args.samples}, warmup_runs={args.warmup_runs}, scale={args.scale}, config={args.config}")
    for item in speedups:
        print(
            "  "
            + item["scenario"]
            + ": "
            + f"{item['median_fast_ops_per_s']} / {item['median_baseline_ops_per_s']} ops/s"
            + f" = {item['speedup']:.3f}x"
        )

    if args.out:
        out_path = (root / args.out).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote {out_path}")
    else:
        print(json.dumps(result, indent=2))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
