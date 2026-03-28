# Contributing to TabloLang

Thanks for contributing to TabloLang.

## Development Setup

Build locally with CMake:

```bash
cmake -S . -B build
cmake --build build --config Debug
cmake --build build --config Release
```

Run programs:

```bash
./build/Debug/tablo run path/to/program.tblo
./build/Debug/tablo compile path/to/program.tblo -o bytecode.out
```

## Testing

Run Tablo tests:

```bash
./build/Debug/tablo test
```

Run C/CTest tests:

```bash
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build -C Release --output-on-failure
```

When changing compiler/VM/runtime behavior, add or update tests in:
- `tests/`
- `tests/native_integration_tests/`
- `tests/tablo_tests/`

## Coding Conventions

- C code: C11, 4-space indentation, same-line braces, `snake_case` names.
- TabloLang code: record types in `PascalCase`, constants in `UPPER_SNAKE`, functions in `lowerCamel`.
- Keep changes consistent with surrounding code style.

## Pull Requests

PRs should include:
- A clear problem/solution summary.
- Test commands run and outcomes.
- Any benchmark/perf-gate impact when relevant.
- Updates to docs (`README.md`) when builtins, stdlib modules, CLI flags, or language behavior change.

Commit subjects should be short imperative phrases, for example:
- `Add RSA stdlib docs`
- `Fix bitset rank hot path`

## Repository Notes

- `build/` and `Testing/Temporary/` are generated outputs and should not be committed.
- If you touch `lib/*.tblo` behavior that is mirrored in integration fixtures, keep corresponding fixture modules aligned.
