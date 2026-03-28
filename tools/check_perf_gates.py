#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple


OPS_RE = re.compile(r"^(?P<label>.+): (?P<count>\d+) ops in (?P<ms>\d+) ms \((?P<rate>\d+) ops/s\)\s*$")
BYTES_RE = re.compile(r"^(?P<label>.+): (?P<count>\d+) bytes in (?P<ms>\d+) ms \((?P<rate>\d+) MiB/s\)\s*$")
TRANSIENT_FAILURE_PATTERNS = (
    "raw request warmup failed",
    "raw request failed",
    "connection failed",
    "thread join timed out",
    "timed out",
)


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


def parse_metrics(stdout: str) -> Dict[str, Dict[str, object]]:
    metrics: Dict[str, Dict[str, object]] = {}
    for line in stdout.splitlines():
        m = OPS_RE.match(line)
        if m:
            metrics[m.group("label")] = {
                "kind": "ops",
                "count": int(m.group("count")),
                "ms": int(m.group("ms")),
                "rate": int(m.group("rate")),
            }
            continue
        m = BYTES_RE.match(line)
        if m:
            metrics[m.group("label")] = {
                "kind": "bytes",
                "count": int(m.group("count")),
                "ms": int(m.group("ms")),
                "rate": int(m.group("rate")),
            }
    return metrics


def contains_any_case_insensitive(text: str, needles: List[str]) -> bool:
    if not needles:
        return True
    hay = (text or "").lower()
    for needle in needles:
        n = (needle or "").lower()
        if n and n in hay:
            return True
    return False


def print_progress(enabled: bool, msg: str) -> None:
    if not enabled:
        return
    print(f"info: {msg}", file=sys.stderr, flush=True)


def run_cmd(cmd: List[str], *, timeout_seconds: int) -> Tuple[int, str, str]:
    effective_timeout: Optional[int] = timeout_seconds if timeout_seconds > 0 else None
    try:
        proc = subprocess.run(
            cmd,
            cwd=repo_root(),
            capture_output=True,
            text=True,
            timeout=effective_timeout,
        )
        return proc.returncode, proc.stdout or "", proc.stderr or ""
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout or ""
        err = exc.stderr or ""
        if err and not err.endswith("\n"):
            err += "\n"
        err += f"error: command timed out after {timeout_seconds}s: {' '.join(cmd)}"
        return 124, out, err


def compile_bench_program(tablo: Path, program: Path, artifact: Path, timeout_seconds: int) -> Tuple[int, str, str]:
    cmd = [str(tablo), "compile", str(program), "-o", str(artifact)]
    return run_cmd(cmd, timeout_seconds=timeout_seconds)


def run_bench_program(tablo: Path, artifact: Path, scale: int, timeout_seconds: int) -> Tuple[int, str, str]:
    cmd = [str(tablo), "run", str(artifact), str(scale)]
    return run_cmd(cmd, timeout_seconds=timeout_seconds)


def run_python_bench_program(program: Path, config: str, scale: int, timeout_seconds: int) -> Tuple[int, str, str]:
    cmd = [sys.executable, str(program), "--config", config, "--scale", str(scale), "--samples-per-mode", "1"]
    return run_cmd(cmd, timeout_seconds=timeout_seconds)


def is_transient_benchmark_failure(stdout: str, stderr: str) -> bool:
    combined = (stdout + "\n" + stderr).lower()
    for pattern in TRANSIENT_FAILURE_PATTERNS:
        if pattern in combined:
            return True
    return False


def run_with_transient_retries(
    run_once,
    *,
    transient_retries: int,
    program_rel: str,
    phase: str,
) -> Tuple[int, str, str]:
    attempt = 0
    while True:
        rc, out, err = run_once()
        if rc == 0:
            return rc, out, err
        if attempt >= transient_retries or not is_transient_benchmark_failure(out, err):
            return rc, out, err
        attempt += 1
        print(
            f"warn: transient benchmark {phase} failure ({program_rel}), "
            f"retrying {attempt}/{transient_retries}",
            file=sys.stderr,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run TabloLang perf-gate benchmarks and enforce minimum throughput thresholds.")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"], help="Build config containing tablo binary")
    parser.add_argument("--scale", type=int, default=1, help="Scale factor passed to benchmark program(s)")
    parser.add_argument("--samples", type=int, default=None, help="Measured benchmark samples (default from gate file, else 3)")
    parser.add_argument("--warmup-runs", type=int, default=None, help="Warmup runs before measured samples (default from gate file, else 1)")
    parser.add_argument("--transient-retries", type=int, default=None, help="Retries for transient benchmark-process failures (default from gate file, else 1)")
    parser.add_argument("--min-samples", type=int, default=None, help="Minimum passing samples per metric (overrides gate file defaults)")
    parser.add_argument("--cmd-timeout-seconds", type=int, default=180, help="Per-subprocess timeout in seconds (0 disables)")
    parser.add_argument(
        "--match-program",
        action="append",
        default=[],
        help="Only run gates whose program path contains this substring (repeatable, case-insensitive)",
    )
    parser.add_argument(
        "--match-label",
        action="append",
        default=[],
        help="Only run gates whose label contains this substring (repeatable, case-insensitive)",
    )
    parser.add_argument("--no-progress", action="store_true", help="Disable per-program progress messages")
    parser.add_argument("--gates", default="tools/perf_gates.json", help="Path to perf gate definition JSON")
    args = parser.parse_args()

    if args.scale < 1:
        print("error: --scale must be >= 1", file=sys.stderr)
        return 2

    gates_path = repo_root() / args.gates
    if not gates_path.exists():
        print(f"error: gate file not found: {gates_path}", file=sys.stderr)
        return 2

    gates = json.loads(gates_path.read_text(encoding="utf-8"))
    defaults = gates.get("defaults", {})
    gate_metrics: List[dict] = gates.get("metrics", [])
    if not gate_metrics:
        print("error: no gate metrics configured", file=sys.stderr)
        return 2
    default_program = str(defaults.get("program", "tests/benchmark_perf_gates.tblo"))

    selected_metrics: List[dict] = []
    for gate in gate_metrics:
        label = str(gate.get("label", ""))
        program_rel = str(gate.get("program", default_program))
        if not contains_any_case_insensitive(program_rel, args.match_program):
            continue
        if not contains_any_case_insensitive(label, args.match_label):
            continue
        selected_metrics.append(gate)

    if not selected_metrics:
        print("error: no gate metrics matched the provided filters", file=sys.stderr)
        return 2

    gate_programs: Dict[str, Path] = {}
    for gate in selected_metrics:
        program_rel = str(gate.get("program", default_program))
        program_path = repo_root() / program_rel
        if not program_path.exists():
            print(f"error: benchmark program not found: {program_path}", file=sys.stderr)
            return 2
        gate_programs[program_rel] = program_path

    samples = int(args.samples if args.samples is not None else defaults.get("samples", 3))
    warmup_runs = int(args.warmup_runs if args.warmup_runs is not None else defaults.get("warmup_runs", 1))
    transient_retries = int(args.transient_retries if args.transient_retries is not None else defaults.get("transient_retries", 1))
    default_min_samples = int(defaults.get("min_samples", samples))
    forced_min_samples = int(args.min_samples) if args.min_samples is not None else None
    if samples < 1:
        print("error: --samples must be >= 1", file=sys.stderr)
        return 2
    if warmup_runs < 0:
        print("error: --warmup-runs must be >= 0", file=sys.stderr)
        return 2
    if transient_retries < 0:
        print("error: --transient-retries must be >= 0", file=sys.stderr)
        return 2
    if forced_min_samples is not None and forced_min_samples < 1:
        print("error: --min-samples must be >= 1", file=sys.stderr)
        return 2
    if args.cmd_timeout_seconds < 0:
        print("error: --cmd-timeout-seconds must be >= 0", file=sys.stderr)
        return 2
    if forced_min_samples is not None:
        if forced_min_samples > samples:
            print("error: --min-samples must be <= effective sample count", file=sys.stderr)
            return 2
    else:
        if default_min_samples < 1 or default_min_samples > samples:
            print("error: defaults.min_samples must be between 1 and effective sample count", file=sys.stderr)
            return 2

    tablo = find_tablo_exe(args.config)
    sample_outputs_by_program: Dict[str, List[Dict[str, Dict[str, object]]]] = {}
    with tempfile.TemporaryDirectory(prefix="tablo_perf_gate_") as tmpdir:
        progress_enabled = not args.no_progress
        program_items = list(gate_programs.items())
        if args.match_program or args.match_label:
            print_progress(
                progress_enabled,
                f"selected {len(selected_metrics)}/{len(gate_metrics)} gate metrics across {len(program_items)} program(s)",
            )

        for program_index, (program_rel, program_path) in enumerate(program_items, start=1):
            sample_outputs: List[Dict[str, Dict[str, object]]] = []
            suffix = program_path.suffix.lower()
            print_progress(progress_enabled, f"[{program_index}/{len(program_items)}] {program_rel}")

            if suffix == ".tblo":
                artifact = Path(tmpdir) / (Path(program_rel).stem + ".tbc")
                print_progress(progress_enabled, f"compile {program_rel}")
                rc, out, err = compile_bench_program(tablo, program_path, artifact, args.cmd_timeout_seconds)
                if rc != 0:
                    print(f"error: benchmark compile failed ({program_rel})", file=sys.stderr)
                    if out.strip():
                        print(out.strip(), file=sys.stderr)
                    if err.strip():
                        print(err.strip(), file=sys.stderr)
                    return rc if rc != 0 else 1
                for i in range(warmup_runs):
                    print_progress(progress_enabled, f"{program_rel} warmup {i + 1}/{warmup_runs}")
                    rc, out, err = run_with_transient_retries(
                        lambda: run_bench_program(tablo, artifact, args.scale, args.cmd_timeout_seconds),
                        transient_retries=transient_retries,
                        program_rel=program_rel,
                        phase="warmup",
                    )
                    if rc != 0:
                        print(f"error: benchmark warmup run failed ({program_rel})", file=sys.stderr)
                        if out.strip():
                            print(out.strip(), file=sys.stderr)
                        if err.strip():
                            print(err.strip(), file=sys.stderr)
                        return rc if rc != 0 else 1
                for i in range(samples):
                    print_progress(progress_enabled, f"{program_rel} sample {i + 1}/{samples}")
                    rc, out, err = run_with_transient_retries(
                        lambda: run_bench_program(tablo, artifact, args.scale, args.cmd_timeout_seconds),
                        transient_retries=transient_retries,
                        program_rel=program_rel,
                        phase="sample",
                    )
                    if rc != 0:
                        print(f"error: benchmark run failed ({program_rel})", file=sys.stderr)
                        if out.strip():
                            print(out.strip(), file=sys.stderr)
                        if err.strip():
                            print(err.strip(), file=sys.stderr)
                        return rc if rc != 0 else 1
                    sample_outputs.append(parse_metrics(out))
            elif suffix == ".py":
                for i in range(warmup_runs):
                    print_progress(progress_enabled, f"{program_rel} warmup {i + 1}/{warmup_runs}")
                    rc, out, err = run_with_transient_retries(
                        lambda: run_python_bench_program(program_path, args.config, args.scale, args.cmd_timeout_seconds),
                        transient_retries=transient_retries,
                        program_rel=program_rel,
                        phase="warmup",
                    )
                    if rc != 0:
                        print(f"error: benchmark warmup run failed ({program_rel})", file=sys.stderr)
                        if out.strip():
                            print(out.strip(), file=sys.stderr)
                        if err.strip():
                            print(err.strip(), file=sys.stderr)
                        return rc if rc != 0 else 1
                for i in range(samples):
                    print_progress(progress_enabled, f"{program_rel} sample {i + 1}/{samples}")
                    rc, out, err = run_with_transient_retries(
                        lambda: run_python_bench_program(program_path, args.config, args.scale, args.cmd_timeout_seconds),
                        transient_retries=transient_retries,
                        program_rel=program_rel,
                        phase="sample",
                    )
                    if rc != 0:
                        print(f"error: benchmark run failed ({program_rel})", file=sys.stderr)
                        if out.strip():
                            print(out.strip(), file=sys.stderr)
                        if err.strip():
                            print(err.strip(), file=sys.stderr)
                        return rc if rc != 0 else 1
                    sample_outputs.append(parse_metrics(out))
            else:
                print(f"error: unsupported benchmark program type for perf gates: {program_path}", file=sys.stderr)
                return 2

            sample_outputs_by_program[program_rel] = sample_outputs
    failures: List[str] = []

    print(f"Perf gate results (samples={samples}, warmup={warmup_runs}):")
    for gate in selected_metrics:
        label = str(gate.get("label", ""))
        program_rel = str(gate.get("program", default_program))
        expected_kind = str(gate.get("kind", "ops"))
        min_rate = int(gate.get("min_rate", 0))
        min_samples = forced_min_samples if forced_min_samples is not None else int(gate.get("min_samples", default_min_samples))
        program_samples = sample_outputs_by_program.get(program_rel, [])
        sample_count = len(program_samples)
        if min_samples < 1 or min_samples > sample_count:
            failures.append(f"[{program_rel}] {label}: min_samples {min_samples} outside valid range 1..{sample_count}")
            print(f"  FAIL [{program_rel}] {label}: invalid min_samples={min_samples}")
            continue

        sample_rates: List[int] = []
        missing_count = 0
        kind_mismatch = False
        bad_kind_seen = ""
        for sample in program_samples:
            metric = sample.get(label)
            if metric is None:
                missing_count += 1
                continue
            actual_kind = str(metric["kind"])
            if actual_kind != expected_kind:
                kind_mismatch = True
                bad_kind_seen = actual_kind
                continue
            sample_rates.append(int(metric["rate"]))

        if missing_count > 0:
            failures.append(f"[{program_rel}] {label}: missing metric in {missing_count}/{sample_count} samples")
            print(f"  FAIL [{program_rel}] {label}: missing in {missing_count}/{sample_count} samples")
            continue
        if kind_mismatch:
            failures.append(f"[{program_rel}] {label}: kind mismatch (expected {expected_kind}, got {bad_kind_seen})")
            print(f"  FAIL [{program_rel}] {label}: kind mismatch (expected {expected_kind}, got {bad_kind_seen})")
            continue
        if not sample_rates:
            failures.append(f"[{program_rel}] {label}: no usable samples")
            print(f"  FAIL [{program_rel}] {label}: no usable samples")
            continue

        median_rate = int(statistics.median_low(sample_rates))
        passing_samples = sum(1 for rate in sample_rates if rate >= min_rate)
        ok = (median_rate >= min_rate) and (passing_samples >= min_samples)
        status = "PASS" if ok else "FAIL"
        print(
            f"  {status} [{program_rel}] {label}: median {median_rate} {expected_kind}/s "
            f"(>= {min_rate}), passing samples {passing_samples}/{sample_count} (>= {min_samples})"
        )
        if not ok:
            failures.append(
                f"[{program_rel}] {label}: median {median_rate} and passing samples {passing_samples}/{sample_count} "
                f"did not satisfy min_rate {min_rate} and min_samples {min_samples}"
            )

    if failures:
        print("\nPerf gate failures:")
        for line in failures:
            print(f"  - {line}")
        return 1

    print("\nAll perf gates passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
