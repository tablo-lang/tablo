#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional


@dataclass(frozen=True)
class Metric:
    label: str
    kind: str  # "ops" | "bytes"
    count: int
    ms: int
    rate: int


OPS_RE = re.compile(r"^(?P<label>.+): (?P<count>\d+) ops in (?P<ms>\d+) ms \((?P<rate>\d+) ops/s\)\s*$")
BYTES_RE = re.compile(r"^(?P<label>.+): (?P<count>\d+) bytes in (?P<ms>\d+) ms \((?P<rate>\d+) MiB/s\)\s*$")


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


def parse_metrics(stdout: str) -> List[Metric]:
    metrics: List[Metric] = []
    for line in stdout.splitlines():
        m = OPS_RE.match(line)
        if m:
            metrics.append(
                Metric(
                    label=m.group("label"),
                    kind="ops",
                    count=int(m.group("count")),
                    ms=int(m.group("ms")),
                    rate=int(m.group("rate")),
                )
            )
            continue
        m = BYTES_RE.match(line)
        if m:
            metrics.append(
                Metric(
                    label=m.group("label"),
                    kind="bytes",
                    count=int(m.group("count")),
                    ms=int(m.group("ms")),
                    rate=int(m.group("rate")),
                )
            )
            continue
    return metrics


def run_benchmark(tablo: Path, program: Path, args: List[str]) -> Dict[str, Any]:
    if program.suffix.lower() == ".py":
        cmd = [sys.executable, str(program), *args]
    else:
        cmd = [str(tablo), "run", str(program), *args]
    proc = subprocess.run(cmd, cwd=repo_root(), capture_output=True, text=True)
    out = (proc.stdout or "").strip()
    err = (proc.stderr or "").strip()
    metrics = parse_metrics(proc.stdout or "")
    return {
        "cmd": cmd,
        "returncode": proc.returncode,
        "stdout": out,
        "stderr": err,
        "metrics": [m.__dict__ for m in metrics],
    }


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Run TabloLang benchmark programs and emit parseable metrics.")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"], help="CMake build config")
    parser.add_argument("--scale", type=int, default=1, help="Scale factor passed to benchmark programs")
    parser.add_argument(
        "--suite",
        default="all",
        choices=[
            "all",
            "micro",
            "workloads",
            "http-streaming",
            "http-server",
            "io-buffered",
            "observability-stdlib",
            "task-stdlib",
            "process-stdlib",
            "sqlite-stdlib",
            "template-stdlib",
            "context-stdlib",
            "log-stdlib",
            "cold-start",
            "perf-gates",
            "fast-string-baseline",
        ],
        help="Which suite(s) to run",
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON to stdout (default)")
    args = parser.parse_args(argv)

    tablo = find_tablo_exe(args.config)
    root = repo_root()

    programs: List[Path] = []
    if args.suite in ("all", "micro"):
        programs.append(root / "tests" / "benchmark_suite.tblo")
    if args.suite in ("all", "workloads"):
        programs.append(root / "tests" / "benchmark_workloads.tblo")
    if args.suite in ("all", "http-streaming"):
        programs.append(root / "tests" / "benchmark_http_streaming.tblo")
    if args.suite in ("all", "http-server"):
        programs.append(root / "tests" / "benchmark_http_server.tblo")
    if args.suite in ("all", "io-buffered"):
        programs.append(root / "tests" / "benchmark_io_buffered.tblo")
    if args.suite in ("all", "observability-stdlib"):
        programs.append(root / "tests" / "benchmark_observability_stdlib.tblo")
    if args.suite in ("all", "task-stdlib"):
        programs.append(root / "tests" / "benchmark_task_group.tblo")
    if args.suite in ("all", "process-stdlib"):
        programs.append(root / "tests" / "benchmark_process_stdlib.tblo")
    if args.suite in ("all", "sqlite-stdlib"):
        programs.append(root / "tests" / "benchmark_sqlite_stdlib.tblo")
    if args.suite in ("all", "template-stdlib"):
        programs.append(root / "tests" / "benchmark_template_stdlib.tblo")
    if args.suite in ("all", "context-stdlib"):
        programs.append(root / "tests" / "benchmark_context_stdlib.tblo")
    if args.suite in ("all", "log-stdlib"):
        programs.append(root / "tests" / "benchmark_log_stdlib.tblo")
    if args.suite in ("all", "cold-start"):
        programs.append(root / "tools" / "benchmark_cold_start.py")
    if args.suite == "perf-gates":
        programs.append(root / "tests" / "benchmark_perf_gates.tblo")
    if args.suite == "fast-string-baseline":
        programs.append(root / "tools" / "benchmark_fast_string_baseline.py")

    results: Dict[str, Any] = {
        "tablo": str(tablo),
        "config": args.config,
        "scale": args.scale,
        "suites": [],
    }

    for program in programs:
        if program.name in (
            "benchmark_workloads.tblo",
            "benchmark_http_streaming.tblo",
            "benchmark_http_server.tblo",
            "benchmark_io_buffered.tblo",
            "benchmark_observability_stdlib.tblo",
            "benchmark_task_group.tblo",
            "benchmark_process_stdlib.tblo",
            "benchmark_sqlite_stdlib.tblo",
            "benchmark_template_stdlib.tblo",
            "benchmark_context_stdlib.tblo",
            "benchmark_log_stdlib.tblo",
            "benchmark_perf_gates.tblo",
        ):
            suite_args = [str(args.scale)]
        elif program.name == "benchmark_cold_start.py":
            suite_args = ["--config", args.config, "--scale", str(args.scale), "--samples-per-mode", "1"]
        elif program.name == "benchmark_fast_string_baseline.py":
            suite_args = ["--config", args.config, "--scale", str(args.scale)]
        else:
            suite_args = []
        results["suites"].append(run_benchmark(tablo, program, suite_args))

    json.dump(results, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
