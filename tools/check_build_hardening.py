#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, List, Tuple


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def normalize_whitespace(text: str) -> str:
    return " ".join((text or "").split())


def find_compile_command(build_dir: Path, target: str) -> Tuple[str, Path]:
    flags_candidates = [
        build_dir / "CMakeFiles" / f"{target}.dir" / "flags.make",
        build_dir / "CMakeFiles" / f"{target}.dir" / "Release" / "flags.make",
    ]
    for flags_path in flags_candidates:
        if not flags_path.exists():
            continue
        text = read_text(flags_path)
        match = re.search(r"^C_FLAGS\s*=\s*(.*)$", text, flags=re.MULTILINE)
        if match:
            return match.group(1).strip(), flags_path

    compile_commands_path = build_dir / "compile_commands.json"
    if compile_commands_path.exists():
        entries = json.loads(read_text(compile_commands_path))
        if not isinstance(entries, list) or not entries:
            raise RuntimeError(f"compile_commands.json was empty: {compile_commands_path}")

        preferred = None
        for entry in entries:
            file_path = str(entry.get("file", "")).replace("\\", "/")
            if file_path.endswith("/src/cli.c"):
                preferred = entry
                break
        if preferred is None:
            preferred = entries[0]

        command = preferred.get("command")
        if not command:
            arguments = preferred.get("arguments", [])
            command = " ".join(str(arg) for arg in arguments)
        if not command:
            raise RuntimeError(
                f"Could not extract compile command from {compile_commands_path}"
            )
        return command, compile_commands_path

    raise FileNotFoundError(
        "Could not locate compile flags; expected flags.make or compile_commands.json in build dir"
    )


def find_link_command(build_dir: Path, target: str, config: str) -> Tuple[str, Path]:
    link_candidates = [
        build_dir / "CMakeFiles" / f"{target}.dir" / "link.txt",
        build_dir / "CMakeFiles" / f"{target}.dir" / config / "link.txt",
    ]
    for link_path in link_candidates:
        if link_path.exists():
            return read_text(link_path).strip(), link_path
    raise FileNotFoundError(
        f"Could not locate linker command for target '{target}' in {build_dir}"
    )


def assert_contains_flags(label: str, line: str, required_flags: Iterable[str]) -> List[str]:
    normalized = normalize_whitespace(line)
    missing: List[str] = []
    for flag in required_flags:
        if flag not in normalized:
            missing.append(flag)
    if missing:
        print(f"error: missing {label} flags: {', '.join(missing)}", file=sys.stderr)
    return missing


def run_checked(cmd: List[str]) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        stderr = (proc.stderr or "").strip()
        stdout = (proc.stdout or "").strip()
        details = stderr or stdout or "<no output>"
        raise RuntimeError(f"command failed ({' '.join(cmd)}): {details}")
    return (proc.stdout or "") + (proc.stderr or "")


def find_built_binary(build_dir: Path, target: str, config: str) -> Path:
    candidates = [
        build_dir / target,
        build_dir / config / target,
        build_dir / f"{target}.exe",
        build_dir / config / f"{target}.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        f"Could not find built binary for target '{target}' in {build_dir} (config {config})"
    )


def verify_elf_hardening(binary_path: Path) -> List[str]:
    if shutil.which("readelf") is None:
        raise RuntimeError("readelf is required for ELF hardening checks but was not found in PATH")

    missing: List[str] = []
    header = run_checked(["readelf", "-hW", str(binary_path)])
    program_headers = run_checked(["readelf", "-lW", str(binary_path)])
    dynamic = run_checked(["readelf", "-dW", str(binary_path)])

    if "Type:" not in header or "DYN" not in header:
        missing.append("ELF type DYN (PIE executable)")
    if "GNU_RELRO" not in program_headers:
        missing.append("GNU_RELRO segment")
    if "BIND_NOW" not in dynamic:
        missing.append("BIND_NOW dynamic flag")

    return missing


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify Linux hardening compile/link flags and ELF properties for a CMake build."
    )
    parser.add_argument("--build-dir", default="build", help="CMake build directory")
    parser.add_argument("--config", default="Release", help="Build config name for multi-config generators")
    parser.add_argument("--target", default="tablo", help="Executable target to validate")
    parser.add_argument(
        "--skip-binary-check",
        action="store_true",
        help="Only verify compile/link flags and skip ELF readelf checks",
    )
    args = parser.parse_args()

    system = platform.system().lower()
    if system != "linux":
        print(f"info: hardening verification currently targets Linux only; skipping on {platform.system()}.")
        return 0

    build_dir = Path(args.build_dir).resolve()
    if not build_dir.exists():
        print(f"error: build directory does not exist: {build_dir}", file=sys.stderr)
        return 2

    failures: List[str] = []

    try:
        compile_line, compile_source = find_compile_command(build_dir, args.target)
        print(f"info: compile flags source: {compile_source}")
        failures.extend(
            assert_contains_flags(
                "compile",
                compile_line,
                ["-fstack-protector-strong", "-D_FORTIFY_SOURCE=2", "-fPIE"],
            )
        )
    except Exception as exc:
        print(f"error: compile flag verification failed: {exc}", file=sys.stderr)
        return 1

    try:
        link_line, link_source = find_link_command(build_dir, args.target, args.config)
        print(f"info: link flags source: {link_source}")
        failures.extend(
            assert_contains_flags("link", link_line, ["-Wl,-z,relro,-z,now", "-pie"])
        )
    except Exception as exc:
        print(f"error: link flag verification failed: {exc}", file=sys.stderr)
        return 1

    if not args.skip_binary_check:
        try:
            binary_path = find_built_binary(build_dir, args.target, args.config)
            print(f"info: binary for ELF checks: {binary_path}")
            missing_binary = verify_elf_hardening(binary_path)
            if missing_binary:
                failures.extend(missing_binary)
                print(
                    "error: missing ELF hardening features: "
                    + ", ".join(missing_binary),
                    file=sys.stderr,
                )
        except Exception as exc:
            print(f"error: binary hardening verification failed: {exc}", file=sys.stderr)
            return 1

    if failures:
        print("Hardening verification: FAIL", file=sys.stderr)
        return 1

    print("Hardening verification: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
