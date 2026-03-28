#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Tuple


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except FileNotFoundError:
        raise ValueError(f"file not found: {path}") from None
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON in {path}: {exc}") from None


def extract_metric_medians(doc: dict) -> Dict[str, int]:
    metrics = doc.get("metrics")
    if not isinstance(metrics, list):
        raise ValueError("missing 'metrics' array")

    out: Dict[str, int] = {}
    for item in metrics:
        if not isinstance(item, dict):
            continue
        label = item.get("label")
        median = item.get("median_ops_per_s")
        if not isinstance(label, str):
            continue
        if isinstance(median, int):
            out[label] = median
    if not out:
        raise ValueError("no metric medians found under 'metrics[].median_ops_per_s'")
    return out


def extract_speedups(doc: dict) -> Dict[str, float]:
    speedups = doc.get("speedups")
    if not isinstance(speedups, list):
        raise ValueError("missing 'speedups' array")

    out: Dict[str, float] = {}
    for item in speedups:
        if not isinstance(item, dict):
            continue
        scenario = item.get("scenario")
        speedup = item.get("speedup")
        if not isinstance(scenario, str):
            continue
        if isinstance(speedup, (int, float)):
            out[scenario] = float(speedup)
    if not out:
        raise ValueError("no speedup values found under 'speedups[].speedup'")
    return out


def percent_delta(base: float, candidate: float) -> float:
    if base <= 0:
        if candidate <= 0:
            return 0.0
        return 100.0
    return ((candidate - base) / base) * 100.0


def compare_maps(
    *,
    baseline: Dict[str, float],
    candidate: Dict[str, float],
    max_regression_pct: float,
    value_suffix: str,
) -> Tuple[List[dict], List[str]]:
    rows: List[dict] = []
    failures: List[str] = []

    missing = sorted(set(baseline.keys()) - set(candidate.keys()))
    for key in missing:
        failures.append(f"missing key in candidate: {key}")

    for key in sorted(set(baseline.keys()) & set(candidate.keys())):
        base = float(baseline[key])
        cand = float(candidate[key])
        delta = percent_delta(base, cand)
        ok = delta >= -max_regression_pct
        rows.append(
            {
                "key": key,
                "baseline": base,
                "candidate": cand,
                "delta_pct": delta,
                "ok": ok,
            }
        )
        if not ok:
            failures.append(
                f"regression {key}: {cand:.6g} {value_suffix} vs {base:.6g} {value_suffix} ({delta:.2f}%)"
            )

    return rows, failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare two fast-string baseline snapshots and fail on regressions beyond configured thresholds."
    )
    parser.add_argument("baseline", help="Path to baseline JSON from benchmark_fast_string_baseline.py")
    parser.add_argument("candidate", help="Path to candidate JSON from benchmark_fast_string_baseline.py")
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
        help="Allowed drop (percent) for fast-vs-generic speedup values",
    )
    parser.add_argument(
        "--allow-metadata-mismatch",
        action="store_true",
        help="Do not fail if config/scale/program differ between snapshots",
    )
    parser.add_argument("--out", default="", help="Optional output JSON report path")
    args = parser.parse_args()

    if args.max_metric_regression_pct < 0:
        print("error: --max-metric-regression-pct must be >= 0", file=sys.stderr)
        return 2
    if args.max_speedup_regression_pct < 0:
        print("error: --max-speedup-regression-pct must be >= 0", file=sys.stderr)
        return 2

    root = repo_root()
    baseline_path = (root / args.baseline).resolve()
    candidate_path = (root / args.candidate).resolve()

    try:
        baseline_doc = load_json(baseline_path)
        candidate_doc = load_json(candidate_path)
        baseline_metrics = extract_metric_medians(baseline_doc)
        candidate_metrics = extract_metric_medians(candidate_doc)
        baseline_speedups = extract_speedups(baseline_doc)
        candidate_speedups = extract_speedups(candidate_doc)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    metadata_failures: List[str] = []
    for key in ("config", "scale", "program"):
        if baseline_doc.get(key) != candidate_doc.get(key):
            metadata_failures.append(
                f"metadata mismatch for '{key}': baseline={baseline_doc.get(key)!r}, candidate={candidate_doc.get(key)!r}"
            )

    metric_rows, metric_failures = compare_maps(
        baseline=baseline_metrics,
        candidate=candidate_metrics,
        max_regression_pct=float(args.max_metric_regression_pct),
        value_suffix="ops/s",
    )
    speedup_rows, speedup_failures = compare_maps(
        baseline=baseline_speedups,
        candidate=candidate_speedups,
        max_regression_pct=float(args.max_speedup_regression_pct),
        value_suffix="x",
    )

    failures: List[str] = []
    if metadata_failures and not args.allow_metadata_mismatch:
        failures.extend(metadata_failures)
    failures.extend(metric_failures)
    failures.extend(speedup_failures)

    report = {
        "baseline": str(baseline_path),
        "candidate": str(candidate_path),
        "thresholds": {
            "max_metric_regression_pct": float(args.max_metric_regression_pct),
            "max_speedup_regression_pct": float(args.max_speedup_regression_pct),
            "allow_metadata_mismatch": bool(args.allow_metadata_mismatch),
        },
        "metadata_mismatches": metadata_failures,
        "metrics": metric_rows,
        "speedups": speedup_rows,
        "ok": len(failures) == 0,
        "failures": failures,
    }

    print("Fast-string baseline comparison")
    print(f"  baseline : {baseline_path}")
    print(f"  candidate: {candidate_path}")
    print(
        "  thresholds: "
        + f"metrics -{args.max_metric_regression_pct:.2f}% , "
        + f"speedups -{args.max_speedup_regression_pct:.2f}%"
    )

    if metadata_failures:
        print("Metadata:")
        for line in metadata_failures:
            print(f"  {'WARN' if args.allow_metadata_mismatch else 'FAIL'} {line}")

    print("Metric medians:")
    for row in report["metrics"]:
        status = "PASS" if row["ok"] else "FAIL"
        print(
            f"  {status} {row['key']}: {int(row['candidate'])} vs {int(row['baseline'])} ops/s "
            f"({row['delta_pct']:+.2f}%)"
        )

    print("Speedups:")
    for row in report["speedups"]:
        status = "PASS" if row["ok"] else "FAIL"
        print(
            f"  {status} {row['key']}: {row['candidate']:.3f}x vs {row['baseline']:.3f}x "
            f"({row['delta_pct']:+.2f}%)"
        )

    if args.out:
        out_path = (root / args.out).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote {out_path}")

    if failures:
        print("Comparison result: FAIL")
        for line in failures:
            print(f"  - {line}")
        return 1

    print("Comparison result: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
