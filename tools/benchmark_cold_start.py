#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import List, Tuple


FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
FNV64_MASK = (1 << 64) - 1


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


def runtime_temp_dir() -> Path:
    if os.name == "nt":
        base = os.environ.get("TEMP") or os.environ.get("TMP") or "."
    else:
        base = os.environ.get("TMPDIR") or "/tmp"
    return Path(base)


def runtime_cache_hash(file_path: str, typecheck_flags: int) -> int:
    h = FNV64_OFFSET
    for b in file_path.encode("utf-8"):
        h ^= b
        h = (h * FNV64_PRIME) & FNV64_MASK
    for i in range(4):
        h ^= (typecheck_flags >> (i * 8)) & 0xFF
        h = (h * FNV64_PRIME) & FNV64_MASK
    return h


def runtime_cache_path_for_source(source_path: Path, typecheck_flags: int = 0) -> Path:
    digest = runtime_cache_hash(str(source_path), typecheck_flags)
    return runtime_temp_dir() / f"tablo-{digest:016x}.tbcc"


def write_benchmark_sources(workdir: Path, helper_count: int) -> Path:
    dep_path = workdir / "cold_dep.tblo"
    dep_path.write_text(
        "func depValue(x: int): int {\n"
        "    return x + 1;\n"
        "}\n",
        encoding="utf-8",
    )

    main_path = workdir / "cold_main.tblo"
    with main_path.open("w", encoding="utf-8") as f:
        f.write('import "cold_dep.tblo";\n\n')
        for i in range(helper_count):
            f.write(f"func helper{i}(x: int): int {{\n")
            f.write(f"    return x + {i};\n")
            f.write("}\n\n")
        f.write("func main(): void {\n")
        f.write("    var x: int = helper0(1);\n")
        f.write("    var y: int = depValue(x);\n")
        f.write("    if (y < 0) {\n")
        f.write('        panic("unreachable");\n')
        f.write("    }\n")
        f.write("}\n")
    return main_path


def run_cmd_and_measure_ms(cmd: List[str]) -> Tuple[int, str, str, int]:
    start = time.perf_counter()
    proc = subprocess.run(cmd, cwd=repo_root(), capture_output=True, text=True)
    elapsed_ms = int(max(1, round((time.perf_counter() - start) * 1000.0)))
    return proc.returncode, proc.stdout or "", proc.stderr or "", elapsed_ms


def emit_metric(label: str, count: int, total_ms: int) -> None:
    elapsed = max(1, total_ms)
    rate = int((count * 1000) // elapsed)
    print(f"{label}: {count} ops in {elapsed} ms ({rate} ops/s)")


def ensure_ok(rc: int, out: str, err: str, context: str) -> None:
    if rc == 0:
        return
    sys.stderr.write(f"error: {context} failed with code {rc}\n")
    if out.strip():
        sys.stderr.write(out.strip() + "\n")
    if err.strip():
        sys.stderr.write(err.strip() + "\n")
    raise RuntimeError(context)


def measure_source(tablo: Path, source_path: Path, cache_path: Path, samples: int) -> int:
    total_ms = 0
    for _ in range(samples):
        if cache_path.exists():
            cache_path.unlink()
        rc, out, err, ms = run_cmd_and_measure_ms([str(tablo), "run", str(source_path)])
        ensure_ok(rc, out, err, "source cold-start run")
        total_ms += ms
    return total_ms


def measure_cache(tablo: Path, source_path: Path, cache_path: Path, samples: int) -> int:
    if cache_path.exists():
        cache_path.unlink()
    rc, out, err, _ = run_cmd_and_measure_ms([str(tablo), "run", str(source_path)])
    ensure_ok(rc, out, err, "cache warmup run")

    total_ms = 0
    for _ in range(samples):
        rc, out, err, ms = run_cmd_and_measure_ms([str(tablo), "run", str(source_path)])
        ensure_ok(rc, out, err, "cache-hit run")
        total_ms += ms
    return total_ms


def measure_artifact(tablo: Path, source_path: Path, artifact_path: Path, samples: int) -> int:
    rc, out, err, _ = run_cmd_and_measure_ms([str(tablo), "compile", str(source_path), "-o", str(artifact_path)])
    ensure_ok(rc, out, err, "artifact compile")

    total_ms = 0
    for _ in range(samples):
        rc, out, err, ms = run_cmd_and_measure_ms([str(tablo), "run", str(artifact_path)])
        ensure_ok(rc, out, err, "artifact run")
        total_ms += ms
    return total_ms


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Cold-start benchmark for source/cache/artifact startup.")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"], help="Build config containing tablo binary")
    parser.add_argument("--scale", type=int, default=1, help="Scale factor for benchmark source size")
    parser.add_argument("--samples-per-mode", type=int, default=1, help="Process runs per mode in this invocation")
    parser.add_argument("--helper-count-base", type=int, default=900, help="Base helper function count before scaling")
    args = parser.parse_args(argv)

    if args.scale < 1:
        print("error: --scale must be >= 1", file=sys.stderr)
        return 2
    if args.samples_per_mode < 1:
        print("error: --samples-per-mode must be >= 1", file=sys.stderr)
        return 2
    if args.helper_count_base < 1:
        print("error: --helper-count-base must be >= 1", file=sys.stderr)
        return 2

    helper_count = args.helper_count_base * args.scale
    tablo = find_tablo_exe(args.config)

    try:
        with tempfile.TemporaryDirectory(prefix="tablo_cold_start_") as tmp:
            tmpdir = Path(tmp)
            source_path = write_benchmark_sources(tmpdir, helper_count).resolve()
            cache_path = runtime_cache_path_for_source(source_path)
            artifact_path = (tmpdir / "cold_main.tbc").resolve()

            source_ms = measure_source(tablo, source_path, cache_path, args.samples_per_mode)
            cache_ms = measure_cache(tablo, source_path, cache_path, args.samples_per_mode)
            artifact_ms = measure_artifact(tablo, source_path, artifact_path, args.samples_per_mode)

            emit_metric("perf coldStart source", args.samples_per_mode, source_ms)
            emit_metric("perf coldStart cache", args.samples_per_mode, cache_ms)
            emit_metric("perf coldStart artifact", args.samples_per_mode, artifact_ms)

            if cache_path.exists():
                cache_path.unlink()
    except RuntimeError:
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
