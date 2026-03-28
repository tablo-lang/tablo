# Fuzzing Targets

This directory contains libFuzzer harnesses and seed corpora for core front-end components:

- `fuzz_lexer.c`: tokenization fuzz target
- `fuzz_parser.c`: parsing fuzz target
- `fuzz_compile.c`: parse + typecheck + compile target
- `fuzz_http.c`: HTTP request/response parsing target plus chunked-body decoder coverage
- `fuzz_artifact.c`: bytecode artifact parsing target
- `fuzz_corpus_runner.c`: normal executable that replays the checked-in corpora through those same harness entrypoints

## Build

Use Clang and enable fuzzing:

```bash
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DENABLE_FUZZING=ON -DUSE_SANITIZERS=ON
cmake --build build-fuzz --target fuzz_lexer fuzz_parser fuzz_compile fuzz_http fuzz_artifact
```

For deterministic corpus replay on toolchains without libFuzzer:

```bash
cmake -S . -B build
cmake --build build --target fuzz_corpus_runner
./build/fuzz_corpus_runner
```

## Run

```bash
./build-fuzz/fuzz_lexer tests/fuzz/corpus/lexer -max_total_time=60
./build-fuzz/fuzz_parser tests/fuzz/corpus/parser -max_total_time=60
./build-fuzz/fuzz_compile tests/fuzz/corpus/compile -max_total_time=60
./build-fuzz/fuzz_http tests/fuzz/corpus/http -max_total_time=60
./build-fuzz/fuzz_artifact tests/fuzz/corpus/artifact -max_total_time=60
```

The default CTest matrix also runs `fuzz_corpus_runner`, so the seed corpora stay exercised even when `ENABLE_FUZZING` is unavailable.

The checked-in corpora are not only smoke samples; they also include minimized malformed HTTP, chunked-body, and artifact regression fixtures so known parser/loader failure surfaces stay replayable on every toolchain.
