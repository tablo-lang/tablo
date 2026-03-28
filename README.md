# TabloLang - Fast, Simple Interpreted Language and VM

TabloLang is a statically typed programming language implemented in C and executed by a stack-based bytecode VM.
It is designed for practical backend and systems scripting workloads: predictable syntax, strong runtime safety,
high-throughput builtins, and a batteries-included standard library.

## Overview

TabloLang consists of a complete toolchain:
- **Lexer**: Tokenizes source code
- **Parser**: Recursive descent parser generating AST
- **Typechecker**: Strong static type checking
- **Compiler**: AST to bytecode translation (bytecode artifacts via `tablo compile -o`)
- **VM**: Stack-based virtual machine with reference-counted memory management and runtime safety guards
- **Runtime**: Thread-safe channels/shared state/thread APIs and typed concurrency helpers
- **Stdlib**: HTTP client/server helpers, SQLite/process APIs, structured observability, rich data/utility modules, and native extension loading

## Project Status (2026)

Current implementation status:
- Interpreted execution with reusable bytecode artifacts and dependency-aware cache for faster cold starts.
- Production-oriented backend primitives (HTTP, process, SQLite, file/streaming I/O).
- Concurrency runtime with message passing, shared state cells, typed wrappers, and Arc-style guard APIs.
- Versioned native extension ABI for shared-library functions, opaque handles, same-thread callbacks, and queued foreign-thread callback posting.
- Broad standard library coverage (`lib/*.tblo`) with dedicated benchmark and perf-gate suites.

Project docs for collaboration:
- Contribution guide: `CONTRIBUTING.md`
- Security reporting: `SECURITY.md`

## Native Extensions

TabloLang can load native extensions at runtime and compile time with repeatable `--ext <library>` flags on:
- `tablo run`
- `tablo compile`
- `tablo debug`
- `tablo test`

Extension libraries expose a C ABI entry point named `tablo_extension_init`. Version 13 of the ABI supports:
- global native functions
- opaque handle types backed by native payload pointers
- one-dimensional arrays of supported ABI element types
- flat tuples of supported non-sequence ABI element types
- `map<string, any>`-style payloads with flat scalar values plus direct flat array/tuple values
- one-dimensional event-batch shapes like `array<map<string, any>>`
- synchronous same-thread callbacks that extensions can retain and invoke later, including top-level one-dimensional array parameters
- structured same-thread callback results for one-dimensional arrays, flat tuples, and flat `map<string, any>` payloads
- queued callback posting from foreign threads, drained and invoked later on the owning VM thread
- explicit TabloLang-side and host-side callback pump controls for host/game-loop integration

Supported ABI value kinds are:
- `void`
- `int`
- `bool`
- `double`
- `string`
- `bytes`
- `handle`
- `map<string, any>` with values drawn from `nil`, the scalar kinds above, `bytes`, opaque handles, direct flat array/tuple values, and one direct nested `map<string, any>` layer
- `func(...) -> ...` callback parameters, including one-dimensional array parameters, flat tuple parameters, and callback results that can be scalars, opaque handles, one-dimensional arrays, flat tuples, or flat `map<string, any>` values
- `array<T>` where `T` is one of the supported non-array ABI kinds above, including `map<string, any>`
- `(T1, T2, ...)` where each `Ti` is one of the supported non-sequence ABI kinds above

Opaque handles are printed as `<TypeName>` at runtime and are intentionally not user-constructible from TabloLang source. They must be created by extension callbacks.

Current limitations:
- v13 is functions, opaque handles, one-dimensional arrays, flat tuples, `map<string, any>` payloads with one direct nested map layer, same-thread callbacks, queued foreign-thread callback posting, explicit VM-side callback pumping, and structured same-thread callback results
- no third-party custom VM object kinds
- callback results support scalar ABI values, opaque handles, one-dimensional arrays, flat tuples, and `map<string, any>` values with one direct nested map layer, but still do not support arbitrary recursive container result types
- extension functions cannot return callbacks yet
- queued foreign-thread callback posting currently supports scalar callback arguments, top-level one-dimensional array arguments, flat tuple arguments, and `map<string, any>` arguments composed of supported scalar values, `bytes`, opaque handles, direct flat array/tuple values, and one direct nested map layer; this includes batched event payloads like `array<map<string, any>>`
- queued foreign-thread callback results are ignored after invocation
- foreign threads do not reenter the VM directly; they only enqueue callback work for later draining on the owning VM thread
- no arbitrary recursive container values in the extension ABI yet: arrays and tuples are still flat, and maps support at most one direct nested map layer; `array<map<string, any>>` is supported, but not deeper recursive chains
- source cache loading/writing is disabled while any `--ext` library is active
- bytecode artifacts that depend on extensions must be run again with the required `--ext` libraries
- `tablo test --ctest` does not forward `--ext`; use the built-in Tablo test runner for extension-backed suites

Posted callback helpers available from TabloLang:
- `extPostedCallbackPendingCount(): int`
- `extDrainPostedCallbacks(maxCount: int): int`
- `extSetPostedCallbackAutoDrain(enabled: bool): bool`

By default, queued foreign-thread callbacks auto-drain at VM safe points. Set `extSetPostedCallbackAutoDrain(false)` to switch to explicit pump mode, then drive delivery from a GUI/game loop with `extDrainPostedCallbacks(...)`.

Embedder-facing pump helpers available from `src/runtime.h`:
- `runtime_has_posted_callbacks(rt)`
- `runtime_posted_callback_pending_count(rt)`
- `runtime_drain_posted_callbacks(rt, maxCallbacks)`
- `runtime_drain_posted_callbacks_for_ms(rt, maxCallbacks, maxMillis)`
- `runtime_wait_for_posted_callbacks(rt, timeoutMillis)`
- `runtime_wait_and_drain_posted_callbacks(rt, maxCallbacks, timeoutMillis)`
- `runtime_get_posted_callback_auto_drain(rt)`
- `runtime_set_posted_callback_auto_drain(rt, enabled)`
- `runtime_post_input_event(rt, callbackName, &event, errorBuf, errorBufSize)`
- `runtime_post_window_event(rt, callbackName, &event, errorBuf, errorBufSize)`
- `runtime_post_frame_event(rt, callbackName, &event, errorBuf, errorBufSize)`
- `runtime_post_input_event_batch(rt, callbackName, events, eventCount, errorBuf, errorBufSize)`
- `runtime_post_window_event_batch(rt, callbackName, events, eventCount, errorBuf, errorBufSize)`
- `runtime_post_frame_event_batch(rt, callbackName, events, eventCount, errorBuf, errorBufSize)`
- `runtime_post_mixed_event_batch(rt, callbackName, events, eventCount, errorBuf, errorBufSize)`

These let a host embedder keep posted callbacks queued until an explicit pump step on the owning VM thread. `runtime_has_posted_callbacks(...)` and `runtime_wait_for_posted_callbacks(...)` provide non-draining poll/wait hooks for event loops that want to decide exactly when dispatch happens. `runtime_drain_posted_callbacks_for_ms(...)` adds a time budget for frame-oriented host loops; non-positive `maxMillis` means no time budget. `runtime_wait_and_drain_posted_callbacks(...)` blocks until work arrives or the timeout expires, then drains up to `maxCallbacks`. When a host is entering shutdown and wants native producers to stop enqueueing immediately, `runtime_close_posted_callback_queue(...)` closes delivery explicitly and `runtime_is_posted_callback_queue_open(...)` exposes that state to the host loop. The typed `runtime_post_*_event(...)` helpers let an embedder queue common GUI/game event families directly to named TabloLang callbacks without hand-assembling raw `map<string, any>` payloads, the `*_batch(...)` variants do the same for single-family `array<map<string, any>>` callback shapes, and `runtime_post_mixed_event_batch(...)` lets one queued callback carry input, window, and frame events together in a single batch. Each posted event struct also accepts an optional `meta_override` pointer so hosts can override the default `source` / `priority` / `phases` metadata, plus optional `extra_fields` carrying top-level typed payload values. The supported extra field values are scalars (`int`, `bool`, `double`, `string`), flat arrays, flat tuples, and one direct `map<string, any>` layer whose entries can themselves contain scalars plus flat arrays/tuples. Nested maps and deeper container recursion are still rejected, and extra field names cannot collide with the preset event keys like `kind`, `meta`, `device`, `event`, `phase`, and their family-specific built-ins. `runtime.h` now also exposes inline helper constructors like `runtime_posted_event_extra_int(...)`, `runtime_posted_event_extra_array(...)`, and `runtime_posted_event_extra_map(...)`, top-level field wrappers like `runtime_posted_event_payload_field(...)` and `runtime_posted_event_context_field(...)`, stack-backed builders `RuntimePostedEventExtraArrayBuilder` and `RuntimePostedEventExtraMapBuilder`, preset payload-field helpers like `runtime_posted_event_build_input_state_payload_field(...)`, `runtime_posted_event_build_input_combo_payload_field(...)`, `runtime_posted_event_build_window_rect_payload_field(...)`, and `runtime_posted_event_build_frame_marker_payload_field(...)`, typed mixed-event constructors like `runtime_posted_typed_input_combo_event(...)`, `runtime_posted_typed_window_rect_event(...)`, and `runtime_posted_typed_frame_marker_event(...)`, typed family batch constructors like `runtime_posted_typed_input_combo_batch(...)`, `runtime_posted_typed_window_rect_batch(...)`, and `runtime_posted_typed_frame_marker_batch(...)`, higher-level frame-envelope wrappers like `runtime_posted_frame_envelope_batch(...)` and `runtime_post_frame_envelope_batch(...)`, the append-style stack-backed `RuntimePostedFrameEnvelopeBuilder` plus `runtime_post_frame_envelope_builder(...)`, the growable heap-backed `RuntimePostedFrameEnvelopeHeapBuilder` plus `runtime_post_frame_envelope_heap_builder(...)`, and the reusable `RuntimeHostEventLoopSession` API so embedders can combine queue polling/drain helpers with one per-frame accumulator and flush point instead of manually wiring those pieces together each frame. When a host loop wants the common end-of-frame path in one call, `runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks_for_ms(...)` queues the current accumulated frame callback and immediately pumps queued work with the same bounded drain budget, `runtime_host_event_loop_session_configure_end_frame(...)` plus `runtime_host_event_loop_session_end_frame(...)` let the loop store that callback name and drain policy once and then reuse a single end-of-frame call every tick, and `runtime_host_event_loop_session_tick(...)` reuses the same stored drain policy to optionally wait for queued callback work and pump it between frames. For hosts that do not want to pass wait behavior every tick, `runtime_host_event_loop_session_configure_tick(...)` plus `runtime_host_event_loop_session_tick_default(...)` now store one of three wait presets once: `no-wait`, `wait`, or `wait-and-pump-until-budget`. When the host loop wants to drive one whole iteration through a single entry point, `runtime_host_event_loop_session_step(...)` combines either a configured `end_frame(...)` or the configured tick preset and returns a small result struct reporting `frame_posted` and `callbacks_drained`. If one iteration needs to bend those defaults without mutating the session, `RuntimeHostEventLoopSessionStepOptions` plus `runtime_host_event_loop_session_step_with_options(...)` can override frame posting, redirect the frame callback name for a single posted frame, force a wait timeout for one tick iteration, cap the drain count for that one step, or tighten the drain time budget for a single step.

The public ABI header `src/tablo_ext.h` also exposes inline helpers for extension authors building payloads by hand:
- `tablo_ext_make_*_value(...)` constructors for scalar, array, tuple, map, and handle values
- `tablo_ext_make_map_entry(...)`
- `tablo_ext_find_map_entry(...)`
- `TabloExtArrayBuilder` and `TabloExtMapBuilder` with typed `add_*` helpers for assembling event payloads without manually filling raw `TabloExtValue` / `TabloExtMapEntry` fields
- higher-level event helpers like `tablo_ext_build_event_meta_map(...)` and `tablo_ext_build_named_event_map(...)` for common `name/delta/meta` event shapes
- GUI/game-oriented event presets like `tablo_ext_build_input_event_map(...)`, `tablo_ext_build_window_event_map(...)`, and `tablo_ext_build_frame_event_map(...)`

Reference points:
- public ABI: `src/tablo_ext.h`
- loader/runtime bridge: `src/native_extension.c`
- host pump regression: `tests/extension_host_pump_tests.c`
- sample extension: `tests/tablo_test_extension.c`
- sample TabloLang program: `tests/tablo_tests/native_extension_smoke.tblo`
- sample TabloLang test suite: `tests/tablo_tests/native_extension_suite.tblo`

### Optional GLFW Reference Extension

The tree now also carries an optional reference native extension for real window/input integration:
- target: `tablo_glfw_extension`
- source: `tests/tablo_glfw_extension.c`
- smoke: `tests/tablo_tests/glfw_extension_smoke.tblo`

Default builds stay unchanged. To enable the reference integration, configure CMake with:
- `-DTABLO_ENABLE_GLFW_EXTENSION=ON`
- optionally `-DTABLO_FETCH_GLFW=ON` to fetch GLFW 3.4 automatically when `find_package(glfw3)` is not already configured

Example:

```bash
cmake -S . -B build-glfw -DTABLO_ENABLE_GLFW_EXTENSION=ON -DTABLO_FETCH_GLFW=ON
cmake --build build-glfw --config Release --target tablo tablo_glfw_extension
build-glfw/Release/tablo run --ext build-glfw/Release/tablo_glfw_extension.dll tests/tablo_tests/glfw_extension_smoke.tblo
```

The GLFW extension currently provides:
- initialization and teardown helpers
- initialization state helpers:
  - `glfwIsInitialized()`
  - `glfwGetLiveWindowCount()`
  - `glfwCanTerminate()`
  - `glfwForceTerminate()`
- hidden or visible native window creation
- basic window state helpers such as size, framebuffer size, window/monitor content scale, primary monitor workarea, window position, title, visibility, iconify/maximize/restore/focus control, cursor position/mode, raw mouse motion support/control, context-capable window creation, current-context queries, buffer swap, and key/button state
- explicit callback lifecycle helpers:
  - `glfwSetInputCallback(...)`
  - `glfwClearInputCallback(...)`
  - `glfwHasInputCallback(...)`
  - `glfwSetWindowCallback(...)`
  - `glfwClearWindowCallback(...)`
  - `glfwHasWindowCallback(...)`
- opt-in callback queue backpressure helpers:
  - `glfwSetCallbackQueueLimit(...)`
  - `glfwGetCallbackQueueLimit(...)`
  - `glfwGetCallbackQueuePendingCount(...)`
  - `glfwGetDroppedCallbackCount(...)`
  - `glfwResetDroppedCallbackCount(...)`
- callback queue diagnostics:
  - `glfwCanPostCallbacks(...)`
  - `glfwGetInvalidatedCallbackCount(...)`
  - `glfwResetInvalidatedCallbackCount(...)`
  - `glfwGetRejectedCallbackCount(...)`
  - `glfwResetRejectedCallbackCount(...)`
- queued input/window callbacks routed through the VM-owned posted-callback queue, including keyboard, mouse-button, cursor-motion, scroll, cursor-enter, and character/text events on the input path
- clearing, replacing, or destroying a GLFW callback target invalidates older queued callback work before it reaches TabloLang code, and that invalidation is now observable through the GLFW helper counters
- queue-limit drops, lifecycle invalidations, and queue-closed posting rejections are tracked separately
- GLFW callback queue limits and pending-count reporting are now aggregate per `GlfwWindow` across both input and window callback families, not per callback slot
- the optional GLFW host regression now also covers isolation across multiple live windows, so drops, invalidations, rejections, and pending counts are pinned per window rather than only within one mixed-family window
- the loader now honors an optional `tablo_extension_shutdown` hook; the GLFW reference extension uses it to force-close any remaining live windows and terminate GLFW during runtime teardown
- runtime teardown now runs extension shutdown hooks before VM teardown, so extensions can safely release retained callbacks and close live handles during `runtime_free(...)`; the GLFW reference coverage now exercises both live input-callback and live window-callback unload paths

For deterministic smoke coverage in this `tests/` reference extension, the same DLL also exposes test-only emit helpers:
- `glfwTestEmitScroll(...)`
- `glfwTestEmitCursorEnter(...)`
- `glfwTestEmitChar(...)`
- `glfwTestEmitKey(...)`
- `glfwTestEmitMouseButton(...)`

Those helpers queue the same input payload shapes through the posted-callback path without depending on platform-specific desktop input synthesis.

Those callbacks are intentionally posted, not reentered directly. Drive delivery with:
- `extDrainPostedCallbacks(...)` from TabloLang code, or
- the host/session pump APIs from `src/runtime.h`

## Language Features

### Types
- `int` - 64-bit integer
- `bool` - Boolean
- `double` - Floating point number
- `bigint` - Arbitrary-precision integer
- `string` - UTF-8 string
- `bytes` - Raw byte buffer
- `array<T>` - Array of any type (heterogeneous elements supported)
- `any` - Dynamic type (assignable to/from any type)
- `record` - User-defined record types with named fields
- `tuple` - Fixed-size heterogeneous tuple types `(Type1, Type2, ...)`
- `map<K, V>` - Hash map with key type K and value type V
- `set<T>` - Hash set of unique elements of type T
- `nil` - Null value (assignable to `T?` and `any`)
- `Future[T]` - Asynchronous result placeholder returned by `async func` declarations
- `T?` - Nullable version of any type
- `void` - No return value

### Statements
- Variable declaration: `var name: type = value;`
- Constant declaration: `const name: type = value;`
- Visibility modifiers on declarations: `public ...`, `private ...` (top-level only, default is `public`)
- Assignment: `name = expr;`, `name += expr;`, `name -= expr;`, `name *= expr;`, `name /= expr;`, `name %= expr;`
- If/Else: `if (condition) { ... } else { ... }` and pattern form `if let Pattern = value { ... } else { ... }` (uses the same tuple, enum payload, and record destructuring patterns as `match`, including `PatternA | PatternB` alternation; bound names in `|` alternatives must match)
- If expression: `if (condition) { stmt; ... expr } else if (other) { expr } else { expr }` (always requires `else`; branch braces are expression blocks in this context; if you want a record/map/set literal branch, wrap it in parentheses like `({ field: value })`)
- Match statement: `match (value) { 1: ..., 2 if guard: ..., else: ... }` (supports type-compatible int/bool/double/bigint/string/nil/tuple/record patterns, `PatternA | PatternB` alternation, tuple patterns like `(left, right)`, record patterns like `{ x: px, y: py }` and `Point { x, .. }`, enum payload destructuring, and Rust-style per-arm guards; tuple/record destructuring requires a non-`any` subject type; typed record patterns may omit fields only with `..`; unguarded covering tuple/record/binding patterns now make later arms and `else` unreachable, and TabloLang also recognizes recursive tuple/record and enum-payload partitions such as `(true, 1)` + `(true, value)` + `(false, 1)` + `(false, value)`, `Flagged { flag: true, code: 1 }` + `Flagged { flag: true, code }` + `Flagged { flag: false, code: 1 }` + `Flagged { flag: false, code }`, or `Result.Ok(1)` + `Result.Ok(value)` + `Result.Err(err)`; non-exhaustive enum diagnostics now report uncovered payload witnesses like `Result.Ok(_)` or `Response.Ok(false)`; `|` alternatives must bind the same names with compatible types; guarded arms do not count toward exhaustiveness)
- Switch statement: `switch (value) { case 1, 2: ..., case type int, Point, Formatter: ..., case type Point as point: ..., default: ... }` (value cases lower to `match`; `case type` currently supports primitive/nil/record/interface targets; `case type T as name` binds a branch-local value narrowed to `T`; bindings currently require exactly one target type; interface cases use the same runtime dispatch rules as interface method calls; no fallthrough)
- While loop: `while (condition) { ... }` and pattern form `while let Pattern = value { ... }` (supports tuple, enum payload, and record destructuring patterns, `PatternA | PatternB` alternation, re-checks the pattern each iteration, stops on mismatch, and requires matching bindings across `|` alternatives)
- Foreach: `foreach (var in iterable) { ... }` and range form `foreach (i in start..end) { ... }` (`end` exclusive)
- Break: `break;`
- Continue: `continue;`
- Return: `return expr;`
- Function declaration: `func name(params): return_type { ... }` and async form `async func name(params): T { ... }` (async functions return `Future[T]`)
- Record declaration: `record Name { field1: type1, field2: type2 };` and generic form `record Name[T] { field: T };`
- Interface declaration: `interface Name { method(arg: type): returnType; ... };`
- Impl declaration: `impl InterfaceName as RecordName { methodName = functionName; ... };` (generic records may use `impl InterfaceName as RecordName[T] { ... };`)
- Enum declaration: `enum Name { MemberA = 1, MemberB, MemberC = 10 };`
- Enum payload variants support constructor calls, value-pattern matching, and payload destructuring bindings in `match` (`enum Name { A(int), B(string, int) }`, `Name.A(1)`, `Name.B(msg, _)` where binding names start with lowercase and `_` discards). Payload patterns can nest tuple and record destructuring (`Result.Ok((left, right))`, `Wrapped.Pointed({ x: px, y: py })`, `Wrapped.Pointed(Point { x, .. })`). In `PatternA | PatternB` alternations, the same bound names must appear in every alternative and resolve to compatible payload types.
- Generic enums are supported in declarations/types and constructor calls (`enum Result[T, E] { Ok(T), Err(E) }`, `var r: Result[int, string] = Result.Ok<int, string>(1);`), including contextual constructor inference from expected type (variable declarations/assignments, function arguments, and returns), plus match-pattern payload inference (`match (r) { Result.Ok(value): ...; Result.Err(_): ... }`).
- Type alias declaration: `type AliasName = ExistingType;` and generic form `type AliasName[T] = ExistingType;`
- Import: `import "path";`

### Expressions
- Literals: integers (`123`, `0x2a`, `123n`, `0x2an` for bigint), doubles, strings, booleans (`true`, `false`), arrays `[1, 2, 3]`, `nil`
- Match expression: `match (value) { 1: expr, 2 if guard: expr, else: expr }` (comma-separated arms; `PatternA | PatternB` expands to multiple arms; supports tuple/record/enum payload destructuring patterns, including typed partial record patterns like `Point { x, .. }`; `else` may be omitted for exhaustive bool/enum matches, any unguarded covering pattern like `(left, right)`, `Point { x, .. }`, or `value`, and recursive tuple/record or enum-payload partitions such as `(true, 1)` + `(true, value)` + `(false, 1)` + `(false, value)` or `Result.Ok(1)` + `Result.Ok(value)` + `Result.Err(err)`; a single payload-constrained enum arm such as `Result.Ok(1)` still does not cover the whole variant by itself, and non-exhaustive diagnostics now report uncovered witnesses like `Result.Ok(_)`, `Response.Ok(false)`, `(false, _)`, or `Point { x: 0, y: _ }`; all arms must produce a compatible non-`void` value; block-valued arms use `pattern: { stmt; ... expr }`; plain brace literals like `{ field: value }` remain record/map/set literals outside pattern position)
- Record literals: `{ field1: value1, field2: value2 }` (must include all declared fields)
- Tuple literals: `(value1, value2, ...)`
- Map literals: `{key1: value1, key2: value2}`
- Set literals: `{value1, value2, value3}`
- Variables: `identifier`
- Binary operators: `+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`
- Unary operators: `-`, `!`, `~`
- Function calls: `function(arg1, arg2)`
- Generic function declarations: `func identity[T](value: T): T { return value; }` with optional constraints (`func keep[T: Formatter](value: T): T { ... }`), call-site type inference (`identity(42)`), and explicit type arguments (`identity<int>(42)`)
- Generic type application in annotations: `Box[int]`, `Pair[string]`
- Anonymous function literals: `func(a: int, b: int): int { return a + b; }` with lexical capture of outer locals (captured by value when the literal is evaluated)
- Array indexing: `array[index]`
- Field/element access: `record.field`, `tuple.0`, `tuple.1`
- Enum member access: `EnumName.Member` (currently backed by generated `int` constants)
- Method syntax sugar: `record.method(arg1, arg2)` lowers to `method(record, arg1, arg2)`
- Interface conformance: explicit `impl Interface as Record { method = function; ... }` mappings are preferred; when no `impl` exists for a pair, TabloLang falls back to matching global method names (`m(record, ...)`)
- Interface dispatch note: conformance is checked statically; at runtime, `iface.method(...)` dispatches by concrete receiver record type using `impl` mappings, with global method-name fallback when no explicit mapping is registered
- Type cast: `expr as type`
- Await: `await expr` (only inside `async func`; `expr` must be `Future[T]`, and the result type is `T`; if the awaited async task panicked, the panic is rethrown in the awaiting task after running deferred cleanup in the panicking task; built-in awaitables now include `asyncSleep(ms)` and async channel send/receive operations, and `lib/async.tblo` provides higher-level `asyncAwaitAll*` / `asyncAwaitAny*` combinators)
- String interpolation: `"Hello ${name}, count=${n + 1}"`

## Language Specification

### 1. Lexical Structure

#### 1.1 Tokens
TabloLang uses the following token categories:

- **Keywords**: `int`, `bool`, `double`, `bigint`, `string`, `bytes`, `array`, `any`, `nil`, `var`, `const`, `public`, `private`, `type`, `interface`, `impl`, `func`, `async`, `await`, `if`, `else`, `match`, `switch`, `case`, `default`, `while`, `foreach`, `break`, `continue`, `return`, `import`, `record`, `enum`, `true`, `false`, `as`, `in`, `void`
- **Identifiers**: Alphanumeric sequences starting with a letter, underscore, or UTF-8 letter bytes
- **Literals**:
  - Integer: `123`, `-456`, `0`, `0x2a`
  - BigInt: `123n`, `-456n`, `0x2an` (or any integer literal too large for `int`)
  - Double: `3.14`, `-0.5`, `1.0`
  - String: `"hello world"` (supports escapes like `\n`, `\t`, `\"`, `\\`, `\u263A`, plus interpolation via `${expr}`)
  - Boolean: `true`, `false`
  - Nil: `nil`
  - Record: `{ field: value, ... }`
- **Operators**: `+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`, `~`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `!`, `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `as`, `?`, `.`
- **Delimiters**: `(`, `)`, `{`, `}`, `[`, `]`, `;`, `:`, `,`, `<`, `>`

#### 1.2 Comments
- Single-line comments: `// comment until end of line`
- Multi-line comments: `/* comment block */`

#### 1.3 Whitespace
Whitespace (spaces, tabs, newlines) is ignored except for separating tokens.

### 2. Type System

#### 2.1 Primitive Types

| Type | Description | Size | Example |
|------|-------------|------|---------|
| `int` | 64-bit signed integer | 8 bytes | `42`, `-17`, `0` |
| `bool` | Boolean | 1 byte | `true`, `false` |
| `double` | IEEE 754 double-precision float | 8 bytes | `3.14159`, `-0.5` |
| `bigint` | Arbitrary-precision integer | variable | `123n`, `-999999999999999999` |
| `string` | UTF-8 encoded string | variable | `"hello"`, `""` |
| `bytes` | Raw byte buffer | variable | `stringToBytes("Hi").0` |
| `nil` | Null value | - | `nil` |
| `void` | Absence of value | - | Return type only |

#### 2.2 Composite Types

| Type | Description | Example |
|------|-------------|---------|
| `array<T>` | Array of type T | `[1, 2, 3]`, `[]` |
| `record` | User-defined record type | `{ x: 1, y: 2 }` |
| `tuple` | Fixed-size heterogeneous tuple | `(int, string)`, `(1, 2, 3)` |
| `map<K, V>` | Hash map from keys K to values V | `{key: value}`, `map<string, int>` |
| `set<T>` | Hash set of unique elements | `{1, 2, 3}`, `set<string>` |
| `T?` | Nullable type T | `int?`, `string?` |
| `func(args): ret` | Function type | `func(int, int): int` |

#### 2.3 Record Types

Records are user-defined composite types with named fields.

**Declaration:**
```tblo
record Point {
    x: int,
    y: int
};

record Person {
    name: string,
    age: int
};
```

**Creation:**
```tblo
var p: Point = { x: 10, y: 20 };
var person: Person = { name: "Alice", age: 30 };
```

**Field Access:**
```tblo
var x_coord: int = p.x;
var person_name: string = person.name;
```

Records use reference semantics - assignment copies the reference, not the data.

#### 2.4 Tuple Types

Tuples are fixed-size, heterogeneous collections for grouping values without naming them. Perfect for returning multiple values from functions.

**Type Syntax:**
```tblo
(int, string)           // Pair of int and string
(int, int, int)         // Triple of three ints
(string, int, double)   // Mixed types
()                      // Empty tuple (unit type)
```

**Creation:**
```tblo
var pair: (int, string) = (42, "answer");
var triple: (int, int, int) = (1, 2, 3);
var mixed: (string, int) = ("Alice", 30);
var unit: () = ();
```

**Element Access:**
Access tuple elements using zero-based positional indexing with dot notation:
```tblo
var pair: (int, string) = (42, "answer");
var num: int = pair.0;      // First element: 42
var str: string = pair.1;   // Second element: "answer"

// Nested tuple access
var nested: ((int, int), string) = ((1, 2), "point");
var x: int = nested.0.0;    // 1
var y: int = nested.0.1;    // 2
```

**Multiple Return Values:**
```tblo
func divide(dividend: int, divisor: int): (int, int) {
    var quotient: int = dividend / divisor;
    var remainder: int = dividend % divisor;
    return (quotient, remainder);
}

var result: (int, int) = divide(17, 5);
println(result.0);  // 3
println(result.1);  // 2
```

Tuples are immutable - you cannot modify elements after creation. Create a new tuple if you need different values.

#### 2.5 Map Types

Maps are hash-based key-value collections with O(1) lookup time. Keys can be strings or integers.

**Type Syntax:**
```tblo
map<string, int>        // Map from string keys to int values
map<int, string>        // Map from int keys to string values
map<string, any>        // Map with heterogeneous values
```

**Creation:**
```tblo
var scores: map<string, int> = {"Alice": 95, "Bob": 87};
var config: map<string, string> = {"host": "localhost", "port": "8080"};
var empty: map<string, int> = {};
```

**Operations:**
```tblo
// Access (returns nullable - nil if key not found)
var score: int? = scores["Alice"];
if (score != nil) {
    println(str(score));
}

// Insert/Update
scores["Carol"] = 92;
scores["Alice"] = 98;  // Update existing

// Check existence
if (mapHas(scores, "Alice")) { ... }

// Delete
mapDelete(scores, "Bob");

// Size
var count: int = mapCount(scores);
```

Maps use reference semantics and are unordered (iteration order not guaranteed).

#### 2.6 Set Types

Sets are collections of unique elements backed by hash tables. Supports string and int elements.

**Type Syntax:**
```tblo
set<string>             // Set of strings
set<int>                // Set of integers
```

**Creation:**
```tblo
var tags: set<string> = {"bug", "feature", "docs"};
var numbers: set<int> = {1, 2, 3, 3, 3};  // Stores {1, 2, 3}
var empty: set<string> = {};
```

**Operations:**
```tblo
// Add/Remove
setAdd(tags, "urgent");      // Add element
setAddString(tags, "urgent"); // Fast path for string sets
setRemove(tags, "bug");      // Remove element
setRemoveString(tags, "bug"); // Fast path for string sets

// Check membership
if (setHas(tags, "feature")) { ... }
if (setHasString(tags, "feature")) { ... }

// Size
var count: int = setCount(tags);

// Convert to array
var tag_array: array<string> = setToArray(tags);
```

Sets use reference semantics and are unordered.

#### 2.7 Type Conversions
Explicit conversions using `as`:
- `int as double` - Integer to double
- `double as int` - Truncates toward zero
- `int as bool` - Zero is false, non-zero is true
- `bool as int` - false becomes 0, true becomes 1
- `int as bigint` - Integer to bigint
- `string as bigint` - Parses decimal or `0x`-prefixed hex string, returns 0 on failure
- `int as string` - Decimal representation
- `double as string` - Decimal representation
- `string as int` - Parses decimal or `0x`-prefixed hex integer, returns 0 on failure
- `string as double` - Parses double, returns 0.0 on failure
- `bool as string` - "true" or "false"
- `string as bool` - "true"/"1" become true, otherwise false
- `double as bool` - NaN and 0.0 become false, otherwise true
- `bool as double` - false becomes 0.0, true becomes 1.0
- `bigint as bool` - 0n becomes false, otherwise true
- `bool as bigint` - false becomes 0n, true becomes 1n

### 3. Statements

#### 3.1 Variable Declaration
```tblo
var name: type = expression;
const name: type = expression;
```
Examples:
```tblo
var x: int = 42;
var y: double = 3.14;
var s: string = "hello";
var arr: array<int> = [1, 2, 3];
var maybe: int? = nil;
var p: Point = { x: 0, y: 0 };
const MAX_RETRIES: int = 5;
```

`const` bindings are immutable, require an initializer, and that initializer must be a compile-time constant expression (literals, literal arithmetic/logic, casts, and literal container expressions).

Top-level declarations can be annotated with visibility:
```tblo
public func exportedName(): void { ... }
private const INTERNAL_LIMIT: int = 64;
```
`private` symbols are only accessible from the same source file; `public` is the default.

#### 3.2 Assignment
```tblo
identifier = expression;
identifier += expression;
identifier -= expression;
identifier *= expression;
identifier /= expression;
identifier %= expression;
array[index] = expression;
array[index] += expression;
record.field = expression;
record.field += expression;
```
Compound assignment is supported for identifiers, array elements, and record fields.

#### 3.3 Control Flow

**If Statement:**
```tblo
if (condition) {
    // statements
} else {
    // statements
}
```
Conditions must be `bool`. Other types are compile-time errors.

**Match Statement (int/enum-backed int patterns):**
```tblo
match (statusCode) {
    Status.Ok: println("ok");
    404: println("not found");
    else: println("other");
}
```

**While Loop:**
```tblo
while (condition) {
    // statements
}
```

**Foreach Loop:**
```tblo
foreach (var item in array) {
    // statements
}
```

**Foreach Range Loop (end exclusive):**
```tblo
foreach (i in 0..n) {
    // i = 0, 1, ..., n-1
}
```

**Break and Continue:**
```tblo
while (condition) {
    if (condition) break;      // Exit loop
    if (condition) continue;   // Skip to next iteration
}
```

#### 3.4 Function Declaration
```tblo
func name(param1: type1, param2: type2): return_type {
    // statements
    return expression;
}
```

Example:
```tblo
func add(a: int, b: int): int {
    return a + b;
}

func greet(name: string): void {
    println("Hello, " + name);
}
```

#### 3.5 Record Declaration
```tblo
record Name {
    field1: type1,
    field2: type2,
    ...
};
```

Generic records use bracketed type parameters:
```tblo
record Box[T] {
    value: T
};
```

Example:
```tblo
record Rectangle {
    width: int,
    height: int
};

func area(rect: Rectangle): int {
    return rect.width * rect.height;
}
```
Record literals must provide all declared fields; missing fields are compile-time errors.

#### 3.6 Return Statement
```tblo
return expression;  // In non-void functions
return;             // In void functions
```

#### 3.7 Import Statement
```tblo
import "path/to/module.tblo";
```
- Imports must be at the top level of a file
- Circular imports are detected and rejected
- Cannot import a file that defines `main()`

#### 3.8 Type Alias Declaration
```tblo
type UserId = int;
type NameList = array<string>;
type Pair[T] = (T, T);
```
Type aliases introduce reusable names for existing types.
Generic aliases are applied with `Name[...]` in type annotations (for example, `var p: Pair[int] = (1, 2);`).

### 4. Expressions

#### 4.1 Literals
- Integer: `42`, `-17`, `0`
- Double: `3.14`, `-0.5`
- String: `"hello"`
- Interpolated string: `"hello ${name}"` (embedded expressions are converted using `as string`)
- Boolean: `true`, `false`
- Nil: `nil`
- Array: `[1, 2, 3]`, `[]`
- Record: `{ x: 1, y: 2 }`
- Tuple: `(42, "answer")`, `(1, 2, 3)`, `()`
- Map: `{"key": value}`, `{}`
- Set: `{1, 2, 3}`, `{}`

#### 4.2 Operators

**Arithmetic:**
| Operator | Description | Types |
|----------|-------------|-------|
| `+` | Addition | int, double, string (concatenation) |
| `-` | Subtraction | int, double |
| `*` | Multiplication | int, double |
| `/` | Division | int, double |
| `%` | Modulo | int |
| `-` | Unary negation | int, double |

**Bitwise:**
| Operator | Description | Types |
|----------|-------------|-------|
| `&` | Bitwise AND | int, bigint |
| `\|` | Bitwise OR | int, bigint |
| `^` | Bitwise XOR | int, bigint |
| `~` | Bitwise NOT | int, bigint |
Bitwise operators require `int`/`bigint` operands. If either operand is a `bigint`, the other operand is promoted to `bigint` and the result is a `bigint`. For `bigint`, operations use two's complement semantics with infinite sign extension.

**Comparison:**
| Operator | Description | Types |
|----------|-------------|-------|
| `==` | Equal | all types |
| `!=` | Not equal | all types |
| `<` | Less than | int, double, string |
| `<=` | Less than or equal | int, double, string |
| `>` | Greater than | int, double, string |
| `>=` | Greater than or equal | int, double, string |

**Logical:**
| Operator | Description |
|----------|-------------|
| `&&` | Logical AND |
| `\|\|` | Logical OR |
| `!` | Logical NOT |
Logical operators require `bool` operands.
`&&` and `||` use short-circuit evaluation (the right-hand side is evaluated only when needed).

**Field/Element Access:**
| Operator | Description | Example |
|----------|-------------|---------|
| `.` | Record field access | `record.field` |
| `.N` | Tuple element access (N = 0, 1, 2...) | `tuple.0`, `tuple.1` |

#### 4.3 Operator Precedence (High to Low)
1. `()` - Grouping
2. `[]` - Array indexing
3. `.` - Field access
4. `as` - Type cast
5. `!`, `~`, `-` - Unary operators
6. `*`, `/`, `%` - Multiplicative
7. `+`, `-` - Additive
8. `&` - Bitwise AND
9. `^` - Bitwise XOR
10. `\|` - Bitwise OR
11. `<`, `<=`, `>`, `>=` - Relational
12. `==`, `!=` - Equality
13. `&&` - Logical AND
14. `\|\|` - Logical OR
15. `=`, `+=`, `-=`, `*=`, `/=`, `%=` - Assignment

### 5. Scoping Rules

- **Global scope**: Top-level declarations
- **Function scope**: Parameters and locals within a function
- **Block scope**: Variables declared in `{}` blocks

Shadowing rules:
- Local variables can shadow global variables
- Inner block variables can shadow outer block variables
- Built-in functions cannot be shadowed
- Record type names cannot be shadowed

### 6. Execution Model

- Programs start execution at the `main()` function
- Functions can be forward-declared (used before definition)
- Global initialization code runs before `main()`
- The VM is stack-based with a separate call stack

## Standard Library Reference

### Built-in Variables

#### `argv: array<string>`
Command-line arguments passed to the program. Arguments provided after the program file on the command line are available in this array.

```tblo
// Run with: tablo run program.tblo hello world 123

func main(): void {
    println("Argument count: " + str(len(argv)));  // 3

    var i: int = 0;
    while (i < len(argv)) {
        println("argv[" + str(i) + "] = " + argv[i]);
        i = i + 1;
    }
    // Output:
    // argv[0] = hello
    // argv[1] = world
    // argv[2] = 123
}
```

Notes:
- `argv` is a read-only global variable of type `array<string>`
- If no arguments are provided, `argv` is an empty array (`len(argv) == 0`)
- The program filename is NOT included in `argv` (unlike C's argv)
- Tool options like `--dump-bytecode` must come before the filename and are not passed to the program

#### `envGet(name: string): (string?, Error?)`
Reads a process environment variable by name.
- Returns `(value, nil)` when set
- Returns `(nil, nil)` when not set
- Returns `(nil, err)` for invalid names or runtime failures

```tblo
func main(): void {
    var r = envGet("PATH");
    if (r.1 != nil) {
        println("envGet failed: " + r.1.message);
        return;
    }

    if (r.0 == nil) {
        println("PATH is not set");
    } else {
        println("PATH length: " + str(len(r.0 as string)));
    }
}
```

### I/O Functions

#### `print(value: any): void`
Prints a value to stdout without a trailing newline.
```tblo
print("Hello");
print(42);
```

#### `println(value: any): void`
Prints a value to stdout with a trailing newline.
```tblo
println("Hello, World!");
println(3.14159);
```

#### `panic(message: string): void`
Aborts the program with a fatal runtime error.
```tblo
panic("unreachable");
```

#### `must(result: (T, Error?)): T`
Unwraps a `(value, err)` tuple. If `err != nil`, aborts the program (like `panic`).
```tblo
var n = must(toBigInt("42"));
println(n);
```

#### `wrapError(err: Error?, context: string): Error?`
Adds context to an error while preserving the original error as the cause (stored in `Error.data`).
- If `err == nil`, returns `nil`.
- Otherwise returns a new `Error` with the same `code`, `message = context + ": " + err.message`, and `data = err`.
```tblo
var r = toBigInt("12x");
var err = wrapError(r.1, "parsing user input");
if (err != nil) {
    println(err.message);
    var cause = err.data as Error;
    println(cause.message);
}
```

### Type Conversion Functions

#### `toInt(value: any): int`
Converts a value to an integer.
- Double: Truncates toward zero
- String: Parses as decimal or `0x`-prefixed hex integer, returns 0 on failure
- Other: Returns 0
```tblo
toInt(3.7);        // Returns 3
toInt("42");       // Returns 42
toInt("0x2a");     // Returns 42
toInt("hello");    // Returns 0
toInt(nil);        // Returns 0
```

#### `toDouble(value: any): double`
Converts a value to a double.
- Int: Converts to double
- String: Parses as double, returns 0.0 on failure
- Other: Returns 0.0
```tblo
toDouble(42);      // Returns 42.0
toDouble("3.14");  // Returns 3.14
toDouble("abc");   // Returns 0.0
```

#### `toBigInt(value: any): (bigint, Error?)`
Converts a value to a bigint.
- Int: Converts to bigint
- Bool: Converts `true` to `1n` and `false` to `0n`
- Double: Rounds to the nearest integer (using `%.0f`)
- String: Parses as decimal or `0x`-prefixed hex bigint (invalid strings return an error)
- Other: Returns `0n`
```tblo
var r = toBigInt("0x2a");
if (r.1 != nil) {
    println("toBigInt failed: " + r.1.message);
    return;
}
println(r.0); // 42n
```

#### `toHexBigInt(value: int | bigint): string`
Converts an integer/bigint value to a hexadecimal string (lowercase, `0x`-prefixed).
```tblo
toHexBigInt(255n);   // Returns "0xff"
toHexBigInt(-255n);  // Returns "-0xff"
```

#### `bytesToHex(data: bytes): (string, Error?)`
Encodes bytes into a lowercase hex string.
```tblo
var b = bytesWithSize(4, 0).0;
b[0] = 0xDE;
b[1] = 0xAD;
b[2] = 0xBE;
b[3] = 0xEF;

var r = bytesToHex(b);
println(r.0); // "deadbeef"
```

#### `hexToBytes(hex: string): (bytes, Error?)`
Decodes a hex string into bytes. Accepts an optional `0x` prefix and allows whitespace/`_`/`:`/`-` separators.
```tblo
var r = hexToBytes("0xdeadbeef");
if (r.1 == nil) {
    println(len(r.0));           // 4
    println(bytesToHex(r.0).0);  // "deadbeef"
}
```

#### `stringToBytes(text: string): (bytes, Error?)`
Converts a string to UTF-8 bytes.
```tblo
var r = stringToBytes("Hi");
println(r.0[0]); // 72
println(r.0[1]); // 105
```

#### `bytesToString(data: bytes): (string, Error?)`
Converts bytes to a string without re-encoding.
```tblo
var r = bytesToString([72, 105]);
if (r.1 == nil) println(r.0); // "Hi"
```

#### `sha256Bytes(data: bytes): (bytes, Error?)`
Computes a 32-byte SHA-256 digest.
```tblo
var raw = stringToBytes("abc");
if (raw.1 == nil) {
    var digest = sha256Bytes(raw.0);
    if (digest.1 == nil) println(bytesToHex(digest.0).0);
}
```

#### `hmacSha256Bytes(key: bytes, data: bytes): (bytes, Error?)`
Computes an HMAC-SHA256 authentication tag.
```tblo
var key_r = stringToBytes("key");
var data_r = stringToBytes("payload");
if (key_r.1 == nil && data_r.1 == nil) {
    var mac = hmacSha256Bytes(key_r.0, data_r.0);
    if (mac.1 == nil) println(len(mac.0)); // 32
}
```

#### `pbkdf2HmacSha256Bytes(password: bytes, salt: bytes, iterations: int, derivedKeyBytes: int): (bytes, Error?)`
Derives key material with PBKDF2-HMAC-SHA256.
```tblo
var password_r = stringToBytes("password");
var salt_r = stringToBytes("salt");
if (password_r.1 == nil && salt_r.1 == nil) {
    var dk = pbkdf2HmacSha256Bytes(password_r.0, salt_r.0, 4096, 32);
    if (dk.1 == nil) println(bytesToHex(dk.0).0);
}
```

#### `hkdfHmacSha256Bytes(ikm: bytes, salt: bytes, info: bytes, derivedKeyBytes: int): (bytes, Error?)`
Derives key material with HKDF-HMAC-SHA256.
- `derivedKeyBytes` must be in `0..8160`
```tblo
var ikm = stringToBytes("secret").0;
var salt = stringToBytes("salt").0;
var info = stringToBytes("info").0;
var okm = hkdfHmacSha256Bytes(ikm, salt, info, 32);
if (okm.1 == nil) println(len(okm.0)); // 32
```

#### `constantTimeBytesEqual(left: bytes, right: bytes): (bool, Error?)`
Compares two byte slices without early exit once lengths match.
```tblo
var a = stringToBytes("secret").0;
var b = stringToBytes("secret").0;
var eq = constantTimeBytesEqual(a, b);
if (eq.1 == nil) println(eq.0);
```

#### `aesCtrBytes(key: bytes, counter: bytes, input: bytes): (bytes, Error?)`
Transforms bytes with AES-CTR. The same function encrypts and decrypts.
- `key` must be 16, 24, or 32 bytes
- `counter` must be 16 bytes
```tblo
var key = hexToBytes("2b7e151628aed2a6abf7158809cf4f3c").0;
var ctr = hexToBytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff").0;
var msg = stringToBytes("hello").0;
var out = aesCtrBytes(key, ctr, msg);
if (out.1 == nil) println(len(out.0));
```

#### `aesGcmSealBytes(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes): (bytes, Error?)`
Encrypts with AES-GCM and returns `ciphertext || 16-byte tag`.
- `key` must be 16, 24, or 32 bytes
- `nonce` must be 12 bytes
```tblo
var key = hexToBytes("00000000000000000000000000000000").0;
var nonce = hexToBytes("000000000000000000000000").0;
var sealed = aesGcmSealBytes(key, nonce, bytesWithSize(0, 0).0, bytesWithSize(0, 0).0);
if (sealed.1 == nil) println(bytesToHex(sealed.0).0);
```

#### `aesGcmOpenBytes(key: bytes, nonce: bytes, sealed: bytes, aad: bytes): (bytes, Error?)`
Decrypts and authenticates AES-GCM data previously produced by `aesGcmSealBytes`.
```tblo
var opened = aesGcmOpenBytes(key, nonce, sealed.0, bytesWithSize(0, 0).0);
if (opened.1 == nil) println(len(opened.0));
```

#### `bytesJoin(parts: array<bytes>): (bytes, Error?)`
Concatenates many byte slices into one bytes value.
```tblo
var r = bytesJoin([hexToBytes("00 01").0, hexToBytes("aa ff").0]);
if (r.1 == nil) println(bytesToHex(r.0).0); // "0001aaff"
```

#### `urlEncode(text: string): (string, Error?)`
Percent-encodes a string using RFC 3986 unreserved-character rules.
- Unreserved characters (`A-Z`, `a-z`, `0-9`, `-`, `.`, `_`, `~`) are left unchanged
- All other bytes are encoded as uppercase `%HH`
```tblo
var r = urlEncode("hello world + /");
if (r.1 == nil) println(r.0); // "hello%20world%20%2B%20%2F"
```

#### `urlDecode(text: string): (string, Error?)`
Decodes percent-encoded text.
- Accepts `%HH` (hex) escape sequences
- Returns `ERR_PARSE` for malformed escapes
- Rejects decoded NUL (`%00`) with `ERR_PARSE`
```tblo
var r = urlDecode("hello%20world%20%2B%20%2F");
if (r.1 == nil) println(r.0); // "hello world + /"
```

#### `str(value: any): string`
Converts a value to a string representation.
```tblo
str(42);           // Returns "42"
str(3.14);         // Returns "3.14"
str(123n);         // Returns "123"
str(nil);          // Returns "nil"
str([1, 2, 3]);    // Returns "[1, 2, 3]"
str({x: 1, y: 2}); // Returns "{x: 1, y: 2}"
str((1, 2));       // Returns "(1, 2)"
```

#### `formatDouble(value: double | int, decimals: int): string`
Formats a number with a fixed number of decimal places (similar to `printf("%.Nf")`).
- `decimals` is clamped to the range `[0, 308]`
- `NaN` / `Infinity` return `"nan"` / `"inf"` / `"-inf"`
```tblo
formatDouble(3.5, 2);     // Returns "3.50"
formatDouble(3.75, 0);    // Returns "4"
formatDouble(1.125, 3);   // Returns "1.125"
formatDouble(42, 3);      // Returns "42.000"
```

#### `typeOf(value: any): string`
Returns the type name of a value as a string. Named records return their concrete runtime record name when available.
```tblo
typeOf(42);        // Returns "int"
typeOf(3.14);      // Returns "double"
typeOf(123n);      // Returns "bigint"
typeOf("hello");   // Returns "string"
typeOf([1, 2]);    // Returns "array"
typeOf(nil);       // Returns "nil"
typeOf(point);     // Returns "Point" when point has concrete record type Point
typeOf({x: 1});    // Returns "record" when no concrete record name is attached
typeOf((1, 2));    // Returns "tuple"
typeOf({"a": 1});  // Returns "map"
typeOf({1, 2});    // Returns "set"
typeOf(futurePending<int>()); // Returns "Future"
```

#### Experimental Future Core Builtins
These builtins provide the current runtime core for `async func` / `await`. Async functions now return `Future[T]`, `await` resumes when a pending future is completed through the runtime future API, `asyncSleep(ms)` provides timer-backed suspension, and async channel operations expose the existing sync channel runtime through awaitable futures.
```tblo
futurePending<T>(): Future<T>
futureResolved<T>(value: T): Future<T>
futureIsReady<T>(task: Future<T>): bool
futureComplete<T>(task: Future<T>, value: T): bool
futureGet<T>(task: Future<T>): T
asyncSleep(ms: int): Future<void>
asyncChannelSend(channelId: int, value: any): Future[(bool, Error?)]
asyncChannelSendTyped(channelId: int, value: any, schema: any): Future[(bool, Error?)]
asyncChannelRecv(channelId: int): Future[(any, Error?)]
asyncChannelRecvTyped(channelId: int, schema: any): Future[(any, Error?)]
```
`futureGet(...)` raises a runtime error if called on a pending future or on a future whose async task panicked. Async-task panics are stored in the future and rethrown when observed via `await` or `futureGet(...)`.
`asyncSleep(ms)` resolves with `nil`; non-positive delays complete immediately without suspending.
Async channel operations resolve with the same `(valueOrBool, Error?)` tuples as their synchronous channel counterparts. If the channel operation can complete immediately, the returned future is already ready; otherwise it resolves once send/receive progress becomes possible or the channel closes.

#### Async Stdlib Module (`lib/async.tblo`)
This module is the first user-facing layer above the raw future builtins. It keeps the current async surface small and typed where that adds clear value.
```tblo
const ASYNC_DEFAULT_POLL_INTERVAL_MS: int

record AsyncAwaitAnyResult {
    index: int,
    ready: bool,
    timedOut: bool,
    value: any,
    error: Error?
}

record AsyncAwaitWithTimeoutResult {
    ready: bool,
    timedOut: bool,
    value: any,
    error: Error?
}

asyncAwaitAll[T](tasks: array<Future[T]>): Future<array<T>>
asyncAwaitAll2[A, B](first: Future[A], second: Future[B]): Future<(A, B)>
asyncAwaitAll3[A, B, C](first: Future[A], second: Future[B], third: Future[C]): Future<(A, B, C)>
asyncAwaitAny[T](tasks: array<Future[T]>, pollIntervalMs: int): Future<AsyncAwaitAnyResult>
asyncAwaitAnyDefault[T](tasks: array<Future[T]>): Future<AsyncAwaitAnyResult>
asyncAwaitWithTimeout[T](task: Future[T], timeoutMs: int, pollIntervalMs: int): Future<AsyncAwaitWithTimeoutResult>
asyncAwaitWithTimeoutDefault[T](task: Future[T], timeoutMs: int): Future<AsyncAwaitWithTimeoutResult>
asyncAwaitAnyWithTimeout[T](tasks: array<Future[T]>, timeoutMs: int, pollIntervalMs: int): Future<AsyncAwaitAnyResult>
asyncAwaitAnyWithTimeoutDefault[T](tasks: array<Future[T]>, timeoutMs: int): Future<AsyncAwaitAnyResult>
```

Notes:
- `asyncAwaitAll*` preserves task order in the returned array/tuple.
- `asyncAwaitAny*` returns the first ready task by index scan order.
- Timeout helpers return `timedOut=true`, `ready=false`, `error=nil` on deadline expiry.
- `asyncAwaitAny*` currently uses cooperative polling with `asyncSleep(...)`; it is correct, but not yet the final high-volume implementation shape.
- `asyncAwaitAny*` returns `ERR_INVALID_ARGUMENT` in `result.error` when called with an empty task list or `pollIntervalMs < 1`.
- `asyncAwaitWithTimeout*` returns `ERR_INVALID_ARGUMENT` in `result.error` when `timeoutMs < 0` or `pollIntervalMs < 1`.

Example:
```tblo
import "lib/async.tblo";

async func demo(): (int, string) {
    var pair = await asyncAwaitAll2(futureResolved(7), futureResolved("ok"));
    return pair;
}
```

### JSON Functions

#### `jsonParse(text: string): (any, Error?)`
Parses JSON into TabloLang values.
- Objects become `map<string, any>`
- Arrays become `array<any>`
- Strings become `string`
- Numbers become `int` (int64) or `double`
- `true`/`false` become `bool` values
- `null` becomes `nil`
Invalid JSON returns `(nil, err)`.
For parse failures, `err.data` includes:
- `path: "$"`
- `offset: int` (0-based byte offset)
- `line: int` (1-based)
- `column: int` (1-based)
- `near: string` (snippet near the failure point)
- `span: map<string, int>` with `start`/`end` byte offsets
```tblo
var r = jsonParse("{\"name\":\"Ada\",\"active\":true,\"count\":2}");
if (r.1 != nil) {
    println("jsonParse failed: " + r.1.message);
    return;
}
var data: any = r.0;
println(mapGet(data, "name"));   // "Ada"
println(mapGet(data, "active")); // true
```

#### `jsonStringify(value: any): (string, Error?)`
Converts a TabloLang value to JSON.
- Supports `nil`, `bool`, `int`, `double`, `bigint` (int64 range), `string`, `array`, and `map`
- Map keys must be `string` or `int` (int keys are stringified)
Unsupported types return an error.
```tblo
var r1 = jsonStringify([true, false, 2, "x", nil]);
if (r1.1 == nil) println(r1.0); // [true,false,2,"x",null]

var r2 = jsonStringify({"ok": true});
if (r2.1 == nil) println(r2.0); // {"ok":true}
```

#### `jsonStringifyPretty(value: any): (string, Error?)`
Converts a TabloLang value to pretty-printed JSON (with newlines and indentation).
```tblo
var r = jsonStringifyPretty({"ok": 1, "name": "Ada"});
if (r.1 == nil) println(r.0);
```

#### `jsonDecode(value: any, schema: any): (any, Error?)`
Validates/transforms JSON-like values (typically from `jsonParse`) against a schema.

Schema forms:
- Primitive schema string: `"int"`, `"double"`, `"number"`, `"bool"`, `"string"`, `"bytes"`, `"nil"`, `"array"`, `"map"`, `"any"`.
- Nullable primitive: append `?` (for example `"string?"`).
- Descriptor maps:
  - `{"type": "array", "items": <schema>}`
  - `{"type": "map", "keys": "string" | "int", "values": <schema>}`
  - `{"type": "record", "fields": map<string, schema>, "allowExtra": bool}`
  - `{"type": "nullable", "inner": <schema>}`
  - `{"type": "literal", "value": <json-like value>}`
  - `{"type": "enum", "values": array<json-like value>}`
  - `{"type": "oneOf", "variants": array<schema>}`
- Record shorthand: `map<string, schema>` (equivalent to strict `record` with `allowExtra=false`).

Decode notes:
- `"bytes"` accepts either a bytes value or a JSON array of ints in `0..255` (converted to bytes).
- `{"type":"map","keys":"int",...}` accepts int keys and decimal-string keys (for JSON objects), coercing them to int.
- If multiple source keys coerce to the same int key, decode fails with `ERR_PARSE`.

Type mismatches return `ERR_PARSE` and set `Error.data` to:
`{"path": string, "schemaPath": string, "expectedType": string, "actualType": string, "kind": "type_mismatch"}`.
Compatibility aliases `expected`/`actual` are also populated.
Invalid schema descriptors return `ERR_INVALID_ARGUMENT` and set `Error.data` to:
`{"path": string, "schemaPath": string, "expectedType": "valid schema", "actualType": "invalid schema", "kind": "schema_error", "detail": string}`.
Other decode failures may include `kind` values such as `missing_field`, `unexpected_field`, `invalid_key`, `duplicate_key`, `range_error`, `limit_exceeded`, `literal_mismatch`, `enum_mismatch`, and `no_variant_match`.
When source-position data is available (for example `jsonParse` failures), `line` and `column` are included in `Error.data`.
```tblo
var parsed = jsonParse("{\"user\":{\"name\":\"Ada\",\"age\":37},\"tags\":[\"x\",\"y\"]}").0;
var schema: map<string, any> = {
    "type": "record",
    "fields": {
        "user": {
            "type": "record",
            "fields": {
                "name": "string",
                "age": "int"
            }
        },
        "tags": {"type": "array", "items": "string"}
    }
};

var r = jsonDecode(parsed, schema);
if (r.1 != nil) {
    println(r.1.message);
    println(mapGet(r.1.data as map<string, any>, "path"));
}
```

#### `jsonDecodeAsX` Wrappers (`lib/json.tblo`)
Import:
```tblo
import "lib/json.tblo";
```

Common backend payload helpers:
- `jsonDecodeAsHttpHeaders(value): (map<string, string>, Error?)`
- `jsonDecodeAsHttpRequest(value): (JsonHttpRequestPayload, Error?)`
- `jsonDecodeAsHttpResponse(value): (JsonHttpResponsePayload, Error?)`
- `jsonDecodeAsApiError(value): (JsonApiErrorPayload, Error?)`
- `jsonDecodeAsApiEnvelope(value): (JsonApiEnvelope, Error?)`

These wrappers call `jsonDecode` with built-in schemas, return typed records/maps, and preserve decode context in structured `Error.data` (including wrapper `context`, merged `path`, type expectations, and nested `cause`).
```tblo
import "lib/json.tblo";

var req_any = jsonParse("{\"method\":\"GET\",\"path\":\"/health\",\"headers\":{}}").0;
var req_r = jsonDecodeAsHttpRequest(req_any);
if (req_r.1 == nil) {
    println(req_r.0.method);
    println(req_r.0.path);
}
```

### Observability Functions

#### `timeSinceMillis(start: int): int`
Returns elapsed monotonic milliseconds since `start` (typically from `timeMonotonicMillis()`).
```tblo
var start = timeMonotonicMillis();
// ...work...
println(timeSinceMillis(start));
```

#### `logJson(level: string, message: string, fields: map<string, any>): (bool, Error?)`
Writes one structured JSON log line to `stderr` with this envelope:
`{"tsMillis": int, "level": string, "message": string, "fields": map}`.
```tblo
var r = logJson("info", "request complete", {"path": "/health", "latencyMs": 12});
if (r.1 != nil) println("logJson failed: " + r.1.message);
```

#### Metrics helpers (`lib/observability.tblo`)
Import:
```tblo
import "lib/observability.tblo";
```

Functions:
- `metricsInc(name: string, delta: int): (bool, Error?)`
- `metricsObserveMs(name: string, valueMs: int): (bool, Error?)`
- `metricsObserveSinceMs(name: string, startMillis: int): (bool, Error?)`
- `metricsSnapshot(): (map<string, any>, Error?)`
- `metricsSnapshotJson(pretty: bool): (string, Error?)`
- `metricsReset(): (bool, Error?)`

Snapshot shape:
- `counters: map<string, int>`
- `timers: map<string, {"count": int, "totalMs": int, "minMs": int, "maxMs": int}>`

```tblo
import "lib/observability.tblo";

var _ = metricsReset();
var _ = metricsInc("jobs.completed_total", 1);
var start = timeMonotonicMillis();
// ...work...
var _ = metricsObserveSinceMs("jobs.latency_ms", start);

var snap = metricsSnapshot();
if (snap.1 == nil) {
    println(mapGet(mapGet(snap.0, "counters") as map<string, any>, "jobs.completed_total"));
}
```

### String Functions

#### `substring(s: string, start: int, len: int): string`
Extracts a substring starting at index `start` with length `len`. Includes bounds checking - if `start` or `len` are out of bounds, returns as much of the string as possible.
```tblo
substring("hello world", 0, 5);     // Returns "hello"
substring("hello world", 6, 5);     // Returns "world"
substring("hello", 1, 3);           // Returns "ell"
substring("hello", 10, 5);          // Returns "" (out of bounds)
```

#### `find(s: string, pattern: string): int`
Finds the first occurrence of `pattern` in `s`. Returns the index of the first character of the match, or -1 if not found.
```tblo
find("hello world", "world");       // Returns 6
find("hello world", "hello");       // Returns 0
find("hello world", "xyz");         // Returns -1 (not found)
find("banana", "na");               // Returns 2 (first occurrence)
```

#### `split(s: string, delimiter: string): array<string>`
Splits a string into an array of substrings using the delimiter. Empty strings between delimiters are included.
```tblo
split("a,b,c", ",");                // Returns ["a", "b", "c"]
split("hello world", " ");          // Returns ["hello", "world"]
split("a,,c", ",");                 // Returns ["a", "", "c"]
split("hello", "");                 // Returns ["", "h", "e", "l", "l", "o", ""]
```

#### `trim(s: string): string`
Removes leading and trailing whitespace from a string.
```tblo
trim("  hello world  ");            // Returns "hello world"
trim("\t\nhello\r\n");              // Returns "hello"
trim("no spaces");                  // Returns "no spaces"
```

#### `startsWith(s: string, prefix: string): bool`
Checks if string `s` starts with `prefix`. Returns true if it matches, false otherwise.
```tblo
startsWith("hello world", "hello"); // Returns true
startsWith("hello world", "world"); // Returns false
startsWith("test", "");             // Returns true (empty prefix)
```

#### `endsWith(s: string, suffix: string): bool`
Checks if string `s` ends with `suffix`. Returns true if it matches, false otherwise.
```tblo
endsWith("hello world", "world");   // Returns true
endsWith("hello world", "hello");   // Returns false
endsWith("test", "");               // Returns true (empty suffix)
```

#### `replace(s: string, old: string, new: string): string`
Replaces all occurrences of `old` with `new` in string `s`.
```tblo
replace("hello world", "world", "universe");  // Returns "hello universe"
replace("aaa", "a", "b");                     // Returns "bbb"
replace("hello", "xyz", "abc");               // Returns "hello" (no matches)
replace("banana", "na", "NA");                // Returns "bNANANA"
```

### Math Functions

#### `absInt(n: int): int`
Returns the absolute value of an integer.
```tblo
absInt(-42);           // Returns 42
absInt(42);            // Returns 42
absInt(0);             // Returns 0
```

#### `absDouble(n: double): double`
Returns the absolute value of a double.
```tblo
absDouble(-3.14);      // Returns 3.14
absDouble(3.14);       // Returns 3.14
absDouble(-0.5);       // Returns 0.5
```

#### `min(a: T, b: T): T`
Returns the minimum of two values. Supports int and double types with automatic type promotion (if one is double, result is double).
```tblo
min(3, 5);             // Returns 3
min(5, 3);             // Returns 3
min(3.5, 2);           // Returns 2.0 (double)
min(-10, -5);          // Returns -10
```

#### `max(a: T, b: T): T`
Returns the maximum of two values. Supports int and double types with automatic type promotion.
```tblo
max(3, 5);             // Returns 5
max(5, 3);             // Returns 5
max(3.5, 2);           // Returns 3.5 (double)
max(-10, -5);          // Returns -5
```

#### `floor(n: double): double`
Returns the largest integer value less than or equal to `n` (as double).
```tblo
floor(3.7);            // Returns 3.0
floor(3.2);            // Returns 3.0
floor(-3.7);           // Returns -4.0
floor(3.0);            // Returns 3.0
```

#### `ceil(n: double): double`
Returns the smallest integer value greater than or equal to `n` (as double).
```tblo
ceil(3.2);             // Returns 4.0
ceil(3.7);             // Returns 4.0
ceil(-3.7);            // Returns -3.0
ceil(3.0);             // Returns 3.0
```

#### `round(n: double): double`
Rounds `n` to the nearest integer (as double). Values halfway between integers round away from zero.
```tblo
round(3.2);            // Returns 3.0
round(3.5);            // Returns 4.0
round(3.7);            // Returns 4.0
round(-3.5);           // Returns -4.0
```

#### `sqrt(n: double): double`
Returns the square root of `n`. Returns NaN for negative inputs.
```tblo
sqrt(16.0);            // Returns 4.0
sqrt(2.0);             // Returns 1.4142...
sqrt(0.0);             // Returns 0.0
```

#### `pow(base: double, exp: double): double`
Returns `base` raised to the power of `exp`.
```tblo
pow(2.0, 3.0);         // Returns 8.0
pow(2.0, 0.5);         // Returns 1.4142... (square root)
pow(10.0, -1.0);       // Returns 0.1
pow(2.0, -2.0);        // Returns 0.25
```

#### `random(): double`
Returns a random double in the range [0.0, 1.0).
```tblo
var r: double = random();   // e.g., 0.753421...
```

#### `randomSeed(seed: int | bigint): void`
Seeds the standard RNG for reproducible results. If never called, the RNG is automatically seeded on first use.
```tblo
randomSeed(123456789);
randomSeed(12345678901234567890n);
```

#### `randomInt(min: int, max: int): int`
Returns a random integer in the range [min, max] (inclusive). The range includes both endpoints.
```tblo
randomInt(1, 6);       // Returns 1, 2, 3, 4, 5, or 6
randomInt(0, 100);     // Returns 0 to 100
randomInt(5, 5);       // Always returns 5
```

#### `randomDouble(min: double | int, max: double | int): double`
Returns a random double in the range [min, max). If min > max, the bounds are swapped.
```tblo
randomDouble(1.0, 2.0);   // Returns >= 1.0 and < 2.0
```

#### `randomBigIntBits(bits: int): bigint`
Returns a non-negative bigint in the range [0, 2^bits - 1]. If bits <= 0, returns 0n.
```tblo
randomBigIntBits(128);    // 0n .. (2^128 - 1)
```

#### `randomBigIntRange(min: bigint, max: bigint): bigint`
Returns a bigint in the inclusive range [min, max]. If min > max, the bounds are swapped.
```tblo
randomBigIntRange(1000n, 5000n);
```

#### `randomFillInt(arr: array<int>, min: int, max: int): array<int>`
Fills an int array in place with random values in [min, max]. Returns the array.
```tblo
var xs: array<int> = [0, 0, 0];
randomFillInt(xs, -3, 3);
```

#### `randomFillDouble(arr: array<double>, min: double | int, max: double | int): array<double>`
Fills a double array in place with random values in [min, max). Returns the array.
```tblo
var xs: array<double> = [0.0, 0.0];
randomFillDouble(xs, 0.0, 1.0);
```

#### `randomFillBigIntBits(arr: array<bigint>, bits: int): array<bigint>`
Fills a bigint array in place with random values in [0, 2^bits - 1]. Returns the array.
```tblo
var xs: array<bigint> = [0n, 0n];
randomFillBigIntBits(xs, 64);
```

#### `randomFillBigIntRange(arr: array<bigint>, min: bigint, max: bigint): array<bigint>`
Fills a bigint array in place with random values in [min, max]. Returns the array.
```tblo
var xs: array<bigint> = [0n, 0n, 0n];
randomFillBigIntRange(xs, 10n, 20n);
```

#### `secureRandom(): (double, Error?)`
Returns a CSPRNG-backed random double in the range [0.0, 1.0).
```tblo
var r = secureRandom();
if (r.1 == nil) println(r.0);
```

#### `secureRandomInt(min: int, max: int): (int, Error?)`
Returns a CSPRNG-backed random int in [min, max].
```tblo
var r = secureRandomInt(1, 6);
if (r.1 == nil) println(r.0);
```

#### `secureRandomDouble(min: double | int, max: double | int): (double, Error?)`
Returns a CSPRNG-backed random double in [min, max).
```tblo
var r = secureRandomDouble(1.0, 2.0);
if (r.1 == nil) println(r.0);
```

#### `secureRandomBigIntBits(bits: int): (bigint, Error?)`
Returns a CSPRNG-backed random bigint in [0, 2^bits - 1].
```tblo
var r = secureRandomBigIntBits(256);
if (r.1 == nil) println(r.0);
```

#### `secureRandomBigIntRange(min: bigint, max: bigint): (bigint, Error?)`
Returns a CSPRNG-backed random bigint in [min, max].
```tblo
var r = secureRandomBigIntRange(100n, 200n);
if (r.1 == nil) println(r.0);
```

#### `secureRandomFillInt(arr: array<int>, min: int, max: int): (array<int>, Error?)`
Fills an int array in place with CSPRNG values in [min, max]. Returns `(arr, err)`.

#### `secureRandomFillDouble(arr: array<double>, min: double | int, max: double | int): (array<double>, Error?)`
Fills a double array in place with CSPRNG values in [min, max). Returns `(arr, err)`.

#### `secureRandomFillBigIntBits(arr: array<bigint>, bits: int): (array<bigint>, Error?)`
Fills a bigint array in place with CSPRNG values in [0, 2^bits - 1]. Returns `(arr, err)`.

#### `secureRandomFillBigIntRange(arr: array<bigint>, min: bigint, max: bigint): (array<bigint>, Error?)`
Fills a bigint array in place with CSPRNG values in [min, max]. Returns `(arr, err)`.

### BigInt Functions

#### `absBigInt(n: bigint): bigint`
Returns the absolute value of a bigint.
```tblo
absBigInt(-123n);      // Returns 123
absBigInt(0n);         // Returns 0
```

#### `signBigInt(n: bigint): int`
Returns the sign of a bigint: -1 for negative, 0 for zero, 1 for positive.
```tblo
signBigInt(-5n);       // Returns -1
signBigInt(0n);        // Returns 0
signBigInt(5n);        // Returns 1
```

#### `digitsBigInt(n: bigint): int`
Returns the number of decimal digits. `digitsBigInt(0n)` returns 1.
```tblo
digitsBigInt(0n);      // Returns 1
digitsBigInt(99999n);  // Returns 5
```

#### `isEvenBigInt(n: bigint): bool`
Returns true if the bigint is even, otherwise false.
```tblo
isEvenBigInt(42n);     // Returns true
isEvenBigInt(41n);     // Returns false
```

#### `isOddBigInt(n: bigint): bool`
Returns true if the bigint is odd, otherwise false.
```tblo
isOddBigInt(41n);      // Returns true
isOddBigInt(42n);      // Returns false
```

#### `powBigInt(base: bigint, exp: int): bigint`
Returns `base` raised to a non-negative integer exponent. Negative exponents return 0n.
```tblo
powBigInt(2n, 10);     // Returns 1024
powBigInt(-2n, 3);     // Returns -8
```

#### `gcdBigInt(a: bigint, b: bigint): bigint`
Returns the greatest common divisor (always non-negative).
```tblo
gcdBigInt(48n, 18n);   // Returns 6
gcdBigInt(-48n, 18n);  // Returns 6
```

#### `lcmBigInt(a: bigint, b: bigint): bigint`
Returns the least common multiple (always non-negative). If either value is 0, returns 0.
```tblo
lcmBigInt(6n, 8n);     // Returns 24
lcmBigInt(0n, 5n);     // Returns 0
```

#### `modPowBigInt(base: bigint, exp: int | bigint, mod: bigint): bigint`
Returns `(base ^ exp) % mod` using fast modular exponentiation. `exp` must be non-negative; negative exponents or a zero modulus return 0.
```tblo
modPowBigInt(2n, 10, 1000n);   // Returns 24
modPowBigInt(5n, 0, 7n);       // Returns 1
modPowBigInt(4n, 13n, 497n);   // Returns 445
```

#### `modInverseBigInt(a: bigint, mod: bigint): bigint`
Returns the modular inverse of `a` modulo `mod`, or 0n if no inverse exists (or if `mod` is 0).
```tblo
modInverseBigInt(3n, 11n);    // Returns 4
modInverseBigInt(2n, 4n);     // Returns 0
```

#### `isProbablePrimeBigInt(n: bigint, rounds: int): bool`
Returns true if `n` is probably prime using Miller-Rabin with the given number of rounds, otherwise false. If `rounds <= 0`, returns false for composite checks (except small primes).
```tblo
isProbablePrimeBigInt(17n, 5);  // Returns true
isProbablePrimeBigInt(21n, 5);  // Returns false
```

#### `compareBigInt(a: bigint, b: bigint): int`
Compares two bigints. Returns -1 if `a < b`, 0 if equal, 1 if `a > b`.
```tblo
compareBigInt(5n, 10n); // Returns -1
compareBigInt(7n, 7n);  // Returns 0
compareBigInt(9n, 3n);  // Returns 1
```

#### `absCmpBigInt(a: bigint, b: bigint): int`
Compares magnitudes. Returns -1 if `|a| < |b|`, 0 if equal, 1 if `|a| > |b|`.
```tblo
absCmpBigInt(-5n, 3n);   // Returns 1
absCmpBigInt(-5n, 5n);   // Returns 0
absCmpBigInt(2n, 10n);   // Returns -1
```

#### `clampBigInt(value: bigint, min: bigint, max: bigint): bigint`
Clamps a bigint to the inclusive range `[min, max]`. If `min > max`, the bounds are swapped.
```tblo
clampBigInt(5n, 0n, 10n);    // Returns 5
clampBigInt(15n, 0n, 10n);   // Returns 10
```

#### `isZeroBigInt(value: bigint): bool`
Returns true if the value is zero, otherwise false.
```tblo
isZeroBigInt(0n);      // Returns true
isZeroBigInt(10n);     // Returns false
```

#### `isNegativeBigInt(value: bigint): bool`
Returns true if the value is negative, otherwise false.
```tblo
isNegativeBigInt(-1n); // Returns true
isNegativeBigInt(0n);  // Returns false
```

### Array Functions

#### `len(value: array<T> | string | bytes): int`
Returns the length of an array, string, or bytes.
```tblo
len([1, 2, 3]);    // Returns 3
len("hello");      // Returns 5
len(stringToBytes("Hi").0); // Returns 2
len([]);           // Returns 0
```

#### `arrayWithSize(size: int, defaultValue: T): (array<T>, Error?)`
Creates an array of length `size`, initializing every element to `defaultValue`.
```tblo
var r = arrayWithSize(5, 0);
if (r.1 != nil) {
    println("arrayWithSize failed: " + r.1.message);
    return;
}
var arr: array<int> = r.0;  // [0, 0, 0, 0, 0]
arr[2] = 42;
println(arr[2]);            // 42
```

#### `bytesWithSize(size: int, fill: int): (bytes, Error?)`
Creates bytes of length `size`, initializing every byte to `fill` (`0..255`).
```tblo
var r = bytesWithSize(3, 0);
if (r.1 != nil) {
    println("bytesWithSize failed: " + r.1.message);
    return;
}
var b: bytes = r.0;
b[1] = 255;
println(bytesToHex(b).0); // "00ff00"
```

#### `push(array: array<T>, value: T): void`
Appends a value to the end of an array.
```tblo
var arr: array<int> = [1, 2];
push(arr, 3);      // arr is now [1, 2, 3]
```

#### `pop(array: array<T>): T`
Removes and returns the last element of an array. Returns `nil` if array is empty.
```tblo
var arr: array<int> = [1, 2, 3];
var x: int = pop(arr);  // x is 3, arr is now [1, 2]
```

#### `keys(array: array<T>): array<int>`
Returns an array of indices `[0, 1, 2, ..., len-1]` for the input array.
```tblo
keys(["a", "b", "c"]);  // Returns [0, 1, 2]
```

#### `values(array: array<T>): array<T>`
Returns a shallow copy of the array.
```tblo
var arr: array<int> = [1, 2, 3];
var copy: array<int> = values(arr);
```

#### `sort(array: array<T>): void`
Sorts an array in place in ascending order. **Note:** Only works on primitive types (int, double, string). Returns without modification for non-primitive types.
```tblo
var arr: array<int> = [3, 1, 4, 1, 5];
sort(arr);                 // arr is now [1, 1, 3, 4, 5]

var words: array<string> = ["banana", "apple", "cherry"];
sort(words);               // words is now ["apple", "banana", "cherry"]
```

#### `reverse(array: array<T>): void`
Reverses an array in place.
```tblo
var arr: array<int> = [1, 2, 3, 4, 5];
reverse(arr);              // arr is now [5, 4, 3, 2, 1]

var words: array<string> = ["a", "b", "c"];
reverse(words);            // words is now ["c", "b", "a"]
```

#### `copyInto(dst: array<T>, src: array<T>): void`
Copies the contents of `src` into `dst` in place. Both arrays must have the same length.
```tblo
var src: array<int> = [1, 2, 3];
var dst: array<int> = [0, 0, 0];
copyInto(dst, src);        // dst is now [1, 2, 3]
```

#### `reversePrefix(array: array<T>, hi: int): void`
Reverses the prefix `array[0..hi]` (inclusive) in place.
```tblo
var arr: array<int> = [0, 1, 2, 3, 4];
reversePrefix(arr, 3);     // arr is now [3, 2, 1, 0, 4]
```

#### `rotatePrefixLeft(array: array<T>, hi: int): void`
Rotates the prefix `array[0..hi]` (inclusive) left by 1 element.
```tblo
var arr: array<int> = [0, 1, 2, 3, 4];
rotatePrefixLeft(arr, 3);  // arr is now [1, 2, 3, 0, 4]
```

#### `rotatePrefixRight(array: array<T>, hi: int): void`
Rotates the prefix `array[0..hi]` (inclusive) right by 1 element.
```tblo
var arr: array<int> = [0, 1, 2, 3, 4];
rotatePrefixRight(arr, 3); // arr is now [3, 0, 1, 2, 4]
```

#### `findArray(array: array<T>, value: T): int`
Finds the index of the first occurrence of `value` in the array. Returns -1 if not found.
```tblo
var arr: array<int> = [10, 20, 30, 20, 40];
findArray(arr, 30);        // Returns 2
findArray(arr, 20);        // Returns 1 (first occurrence)
findArray(arr, 100);       // Returns -1 (not found)
```

#### `contains(array: array<T>, value: T): int`
Checks if the array contains the value. Returns 1 if found, 0 if not found.
```tblo
var arr: array<int> = [1, 2, 3, 4, 5];
contains(arr, 3);          // Returns 1
contains(arr, 10);         // Returns 0
```

#### `slice(array: array<T>, start: int, end: int): array<T>`
Extracts a subarray from `start` index (inclusive) to `end` index (exclusive). Negative indices are treated as 0. If `end` exceeds array length, it's clamped to length.
```tblo
var arr: array<int> = [0, 1, 2, 3, 4, 5];
slice(arr, 1, 4);          // Returns [1, 2, 3]
slice(arr, 2, 10);         // Returns [2, 3, 4, 5] (clamped)
slice(arr, 0, 3);          // Returns [0, 1, 2]
```

#### `join(array: array<T>, delimiter: string): string`
Concatenates all array elements into a string, separated by `delimiter`. Elements are automatically converted to strings.
```tblo
var arr: array<int> = [1, 2, 3, 4, 5];
join(arr, ", ");           // Returns "1, 2, 3, 4, 5"

var words: array<string> = ["hello", "world"];
join(words, " ");          // Returns "hello world"

join(arr, "-");            // Returns "1-2-3-4-5"
```

### Map Functions

#### `mapGet(map: map<K, V>, key: K): V?`
Retrieves a value from a map. Returns `nil` if the key doesn't exist.
```tblo
var scores: map<string, int> = {"Alice": 95};
var score: int? = mapGet(scores, "Alice");  // Returns 95
var missing: int? = mapGet(scores, "Bob");   // Returns nil
```

#### `mapGetString(map: map<string, V>, key: string): V?`
Fast path for string-key maps. Returns `nil` if the key doesn't exist.
```tblo
var score: int? = mapGetString(scores, "Alice"); // Returns 95
```

#### `mapSet(map: map<K, V>, key: K, value: V): void`
Inserts or updates a key-value pair in the map.
```tblo
var config: map<string, string> = {};
mapSet(config, "host", "localhost");
mapSet(config, "port", "8080");
```

#### `mapSetString(map: map<string, V>, key: string, value: V): void`
Fast path for string-key maps.
```tblo
mapSetString(config, "host", "localhost");
```

#### `mapHas(map: map<K, V>, key: K): bool`
Returns true if the key exists in the map, false otherwise.
```tblo
if (mapHas(scores, "Alice")) {
    println("Alice has a score");
}
```

#### `mapHasString(map: map<string, V>, key: string): bool`
Fast path for string-key maps.
```tblo
if (mapHasString(scores, "Alice")) {
    println("Alice has a score");
}
```

#### `mapDelete(map: map<K, V>, key: K): void`
Removes a key-value pair from the map. Does nothing if the key doesn't exist.
```tblo
mapDelete(scores, "Alice");  // Removes Alice's score
```

#### `mapDeleteString(map: map<string, V>, key: string): void`
Fast path for deleting from string-key maps.
```tblo
mapDeleteString(scores, "Alice");
```

#### `mapCount(map: map<K, V>): int`
Returns the number of key-value pairs in the map.
```tblo
var n: int = mapCount(scores);  // Returns the number of entries
```

### Set Functions

#### `setAdd(set: set<T>, value: T): void`
Adds an element to the set. Does nothing if the element already exists.
```tblo
var tags: set<string> = {"bug"};
setAdd(tags, "feature");  // tags is now {"bug", "feature"}
setAdd(tags, "bug");      // No change, "bug" already exists
```

#### `setAddString(set: set<string>, value: string): void`
Fast path for adding string values to a string set.
```tblo
setAddString(tags, "feature");
```

#### `setHas(set: set<T>, value: T): bool`
Returns true if the element exists in the set, false otherwise.
```tblo
if (setHas(tags, "bug")) {
    println("This is a bug");
}
```

#### `setHasString(set: set<string>, value: string): bool`
Fast path for membership checks in string sets.
```tblo
if (setHasString(tags, "bug")) {
    println("This is a bug");
}
```

#### `setRemove(set: set<T>, value: T): void`
Removes an element from the set. Does nothing if the element doesn't exist.
```tblo
setRemove(tags, "bug");  // Removes "bug" from the set
```

#### `setRemoveString(set: set<string>, value: string): void`
Fast path for removing string values from string sets.
```tblo
setRemoveString(tags, "bug");
```

#### `setCount(set: set<T>): int`
Returns the number of elements in the set.
```tblo
var n: int = setCount(tags);  // Returns the number of unique tags
```

#### `setToArray(set: set<T>): array<T>`
Converts the set to an array. Order is not guaranteed.
```tblo
var tag_array: array<string> = setToArray(tags);
foreach (var tag in tag_array) {
    println(tag);
}
```

### Networking Functions

TabloLang provides both high-level HTTP client functions and low-level TCP socket access for network communication. All network operations have a 10-second timeout.

#### HTTP Client Functions

The HTTP client functions provide simple interfaces for making HTTP requests. Legacy helpers (`httpGet*`/`httpPost*`) return `(body, err)` and treat HTTP 4xx/5xx as errors; `httpRequest` returns a structured response and leaves status handling to the caller.

#### `httpRequest(method: string, url: string, body: string?, headers: map<string, string>?, timeoutMs: int): (map<string, any>, Error?)`
Performs a generic HTTP request and returns a structured response map with:
- `status: int`
- `body: string`
- `headers: map<string, string>`

Behavior:
- Supports `http://` and `https://` URLs.
- HTTPS requires TLS socket support (`tlsIsAvailable()`).
- `timeoutMs` must be in `[1, 120000]`.
- Returns `err != nil` only for request/transport/parse failures (not for HTTP status codes like 404/500).

```tblo
var r = httpRequest("GET", "http://api.example.com/health", nil, nil, 5000);
if (r.1 != nil) {
    println("request failed: " + r.1.message);
    return;
}

var resp: map<string, any> = r.0;
println(mapGet(resp, "status"));
println(mapGet(resp, "body"));
```

#### `httpRequestWithOptions(method: string, url: string, body: string?, headers: map<string, string>?, timeoutMs: int, requestOptions: map<string, any>?): (map<string, any>, Error?)`
Same behavior as `httpRequest`, with explicit per-request transport options.

Supported `requestOptions` keys:
- `tlsInsecureSkipVerify: bool` (alias: `insecureSkipVerify`)
  For HTTPS requests, skips certificate verification (intended for local/dev self-signed endpoints).
- `keepAlive: bool` (alias: `connectionPool`)
  Enables opt-in HTTP/1.1 connection reuse for sequential requests to the same origin.
  Default is `false` (connections are closed after each request unless you set this option).
- `gzip: bool`
  Defaults to `true` for full-body requests. When enabled, the client sends `Accept-Encoding: gzip`
  unless you already provided that header, and automatically decodes `Content-Encoding: gzip`
  responses before returning the body.

```tblo
// Dev-only local HTTPS example with self-signed certificate.
var opts: map<string, any> = {"tlsInsecureSkipVerify": true};
var r = httpRequestWithOptions(
    "GET",
    "https://localhost:8443/health",
    nil,
    nil,
    5000,
    opts
);
if (r.1 != nil) {
    println("request failed: " + r.1.message);
} else {
    println(mapGet(r.0, "status"));
}
```

#### `httpRequestHead(method: string, url: string, body: string?, headers: map<string, string>?, timeoutMs: int): (map<string, any>, Error?)`
Performs an HTTP request but returns after parsing response status + headers, leaving the response body on an open socket for streaming.

Returned map fields:
- `status: int`
- `headers: map<string, string>`
- `contentLength: int` (`-1` when absent/unknown)
- `socket: any` (socket handle for reading body)

Behavior:
- Supports `http://` and `https://` URLs.
- HTTPS requires TLS socket support (`tlsIsAvailable()`).
- `timeoutMs` must be in `[1, 120000]`.
- Caller is responsible for consuming the body (`tcpReceive` / stdlib stream helpers) and closing `socket` (`tcpClose`).

```tblo
var h = httpRequestHead("GET", "http://api.example.com/data", nil, nil, 5000);
if (h.1 != nil) {
    println(h.1.message);
    return;
}
var head: map<string, any> = h.0;
println(mapGet(head, "status"));
println(mapGet(head, "contentLength"));
```

#### `httpRequestHeadWithOptions(method: string, url: string, body: string?, headers: map<string, string>?, timeoutMs: int, requestOptions: map<string, any>?): (map<string, any>, Error?)`
Same behavior as `httpRequestHead`, with the same `requestOptions` support as `httpRequestWithOptions`.
Unlike the full-body request helpers, the HEAD/streaming path keeps `gzip` disabled by default so the
returned socket still exposes the raw response body unless you opt in explicitly.

#### HTTP Server Helper Functions

These helpers are built on top of TCP sockets and are intended for small synchronous servers.

#### `httpReadRequest(socket: any, maxBytes: int): (map<string, any>, Error?)`
Reads and parses a single HTTP request from a connected socket.

The returned map contains:
- `method: string`
- `path: string`
- `version: string`
- `headers: map<string, string>`
- `body: string`

Behavior:
- Reads until the full header is received.
- If `Content-Length` is present, waits for that many body bytes.
- Fails with `ERR_LIMIT` if request size exceeds `maxBytes`.

```tblo
var rr = httpReadRequest(clientSocket, 65536);
if (rr.1 != nil) {
    println("read failed: " + rr.1.message);
    return;
}
var req: map<string, any> = rr.0;
println(mapGet(req, "method"));
println(mapGet(req, "path"));
```

#### `httpWriteResponse(socket: any, statusCode: int, body: string, headers: map<string, string>?): (bool, Error?)`
Writes an HTTP/1.1 response to a connected socket.

Behavior:
- `statusCode` must be in `100..999`.
- Automatically writes `Content-Length` and `Connection: close`.
- Writes a default `Content-Type: text/plain; charset=utf-8` when not provided.
- If headers include `Content-Encoding: gzip`, the helper gzip-compresses `body` before sending it and
  computes `Content-Length` from the compressed payload.

```tblo
var headers: map<string, string> = {"Content-Type": "text/plain"};
var wr = httpWriteResponse(clientSocket, 200, "ok", headers);
if (wr.1 != nil) {
    println("write failed: " + wr.1.message);
}
```

#### Standard library module: `lib/http.tblo`
For higher-level synchronous servers, use the stdlib wrapper module built on top of `tcpListen`/`tcpAccept`/`httpReadRequest`/`httpWriteResponse`.

It provides:
- `HttpServer` record (`listener`, `host`, `port`, `acceptTimeoutMs`, `maxRequestBytes`)
- `httpServerStart`, `httpServerClose`
- `httpServerRouteKey`, `httpServerRoute`
- `httpServerResponse`
- `httpServerGzipResponse` for negotiated gzip responses on compressible string bodies
- `httpServerServeOne`, `httpServerServeMany`, `httpServerServeConcurrent`
- `httpServerServeThreadPool` for multithreaded request processing
- `httpServerServeThreadPoolWithDeadline` for request-deadline enforcement in thread-pool mode
- `httpServerServeThreadPoolControlled` for thread-pool serving with stop-channel control
- `httpServerServeThreadPoolControlledWithDeadline` for stop-channel + deadline control
- `httpServerServeLoop` for long-running thread-pool serving (idle/close shutdown)
- `httpServerServeLoopWithDeadline` for long-running serving with per-request deadlines
- `httpServerServeLoopControlled` for long-running thread-pool serving with explicit stop signals
- `httpServerServeLoopControlledWithDeadline` for stop-channel + deadline controlled loops
- `HttpClientResponse` + `httpClient*` wrappers for typed client responses and JSON decode flows
- `HttpClientStreamResponse` + `httpClient*Stream` wrappers for head-first status/header reads with chunked body consumption
- worker helpers: `httpServerWorkerRecv`, `httpServerWorkerSendOk`, `httpServerWorkerSendErr`
- worker adapters: `httpServerWorkerHandlePayload`, `httpServerWorkerServeLoop`
- middleware pipeline support (`array<any>` of request-transform functions)
- query helpers: `httpQueryGetString`, `httpQueryGetStringDefault`, `httpQueryGetInt`, `httpQueryGetIntDefault`, `httpQueryGetIntRange`, `httpQueryGetIntRangeDefault`, `httpQueryGetBool`, `httpQueryGetBoolDefault`

Route handlers are functions with shape `handler(req: map<string, any>) -> map<string, any>`.
In stdlib server flows (`httpServerServeOne`/`Many`/`Concurrent`/thread-pool variants), the request map includes:
- `method: string`
- `path: string` (normalized route path, query string removed)
- `rawPath: string` (original request path including `?query` when present)
- `queryRaw: string` (raw query text without leading `?`)
- `query: map<string, string>` (parsed query parameters; empty map when absent or parse failed)
- `queryError: Error?` (present only when query parsing fails)
- `headers: map<string, string>`
- `body: string`

Response maps use keys:
- `status: int` (optional, default `200`)
- `body: string` (optional, default `""`)
- `headers: map<string, string>` (optional)

#### Query Helper APIs
Use these helpers in route handlers to read parsed query parameters safely:

- `httpQueryGetString(req, key): (string?, Error?)`
- `httpQueryGetStringDefault(req, key, defaultValue): (string, Error?)`
- `httpQueryGetInt(req, key): (int?, Error?)`
- `httpQueryGetIntDefault(req, key, defaultValue): (int, Error?)`
- `httpQueryGetIntRange(req, key, minValue, maxValue): (int?, Error?)`
- `httpQueryGetIntRangeDefault(req, key, defaultValue, minValue, maxValue): (int, Error?)`
- `httpQueryGetBool(req, key): (bool?, Error?)`
- `httpQueryGetBoolDefault(req, key, defaultValue): (bool, Error?)`

Behavior:
- Missing key returns `(nil, nil)` for nullable getters.
- Default getters return `defaultValue` when the key is missing.
- Invalid values return `ERR_PARSE`.
- Invalid request/query shape returns `ERR_INVALID_ARGUMENT`.
- Range helpers enforce `minValue <= value <= maxValue`.

#### `httpServerServeConcurrent(server, routes, middleware, maxRequests, workerCount, queueCapacity): (int, Error?)`
Serves up to `maxRequests` using a bounded pending-connection queue.

Behavior:
- `workerCount` must be in `1..256`
- `queueCapacity` must be in `1..4096`
- Accept batch size is `workerCount * queueCapacity` (capped by remaining requests)
- Stops early on accept timeout and returns `(servedCount, nil)`

Current implementation note:
- Request handling still executes on the current VM thread; this API batches accepts and processes queued clients in FIFO order.

#### `httpServerServeThreadPool(server, workerFunctionName, maxRequests, workerCount, queueCapacity): (int, Error?)`
Runs accept/read/write on the main server loop and dispatches request processing to worker runtimes via channels.

Worker contract:
- `workerFunctionName` must be a top-level zero-argument function.
- Worker reads jobs from `syncThreadInbox()` and writes results to `syncThreadOutbox()`.
- Use stdlib helpers:
  - `httpServerWorkerRecv(timeoutMs): (any, Error?)` (returns `nil` payload for shutdown)
  - `httpServerWorkerSendOk(requestId, response, timeoutMs): (bool, Error?)`
  - `httpServerWorkerSendErr(requestId, code, message, timeoutMs): (bool, Error?)`
  - `httpServerWorkerHandlePayload(routes, middleware, payload, timeoutMs): (bool, Error?)`
  - `httpServerWorkerServeLoop(routes, middleware, timeoutMs): (int, Error?)`

Notes:
- If a worker reports an error, the server returns `500` with the worker message.
- This model keeps socket ownership on the server loop and parallelizes route/middleware/user logic.
- `maxRequests` may be `0` for "run until idle timeout or listener close".
- When the queue is saturated, newly accepted connections are rejected explicitly with `503 Server Busy`.
- Timeout handling is non-fatal during active load (the loop keeps waiting for worker results).

#### `httpServerServeThreadPoolWithDeadline(server, workerFunctionName, maxRequests, workerCount, queueCapacity, requestDeadlineMs): (int, Error?)`
Same as `httpServerServeThreadPool`, but enforces a per-request in-flight deadline.

Behavior:
- `requestDeadlineMs` must be in `0..600000` (`0` disables deadline enforcement).
- When a request exceeds deadline, the server responds with `504 Gateway Timeout`, closes that client, and keeps serving.
- Timed-out requests count toward the returned `servedCount`.

Worker convenience pattern:
- Build `routes` and `middleware` inside the worker function.
- Call `httpServerWorkerServeLoop(routes, middleware, timeoutMs)` to handle request payloads until shutdown.

#### `httpServerServeThreadPoolControlled(server, workerFunctionName, maxRequests, workerCount, queueCapacity, stopChannelId, stopPollTimeoutMs): (int, Error?)`
Adds explicit message-passing shutdown control to thread-pool serving.

Behavior:
- `stopChannelId` uses `syncChannelCreate`; pass `-1` to disable external stop control.
- Sending any value to `stopChannelId` requests graceful stop.
- Closing `stopChannelId` also requests graceful stop.
- On stop request, the server loop stops accepting new clients, drains in-flight requests, and returns `(servedCount, nil)`.
- `stopPollTimeoutMs` controls shutdown responsiveness (`1..120000` ms).

#### `httpServerServeThreadPoolControlledWithDeadline(server, workerFunctionName, maxRequests, workerCount, queueCapacity, stopChannelId, stopPollTimeoutMs, requestDeadlineMs): (int, Error?)`
Combines stop-channel control and per-request deadline enforcement.

Behavior:
- Same stop-channel semantics as `httpServerServeThreadPoolControlled`.
- Same deadline semantics as `httpServerServeThreadPoolWithDeadline`.

#### `httpServerServeLoop(server, workerFunctionName, workerCount, queueCapacity): (int, Error?)`
Production-oriented long-running wrapper over `httpServerServeThreadPool(..., maxRequests=0, ...)`.

Behavior:
- Runs until idle timeout (based on `server.acceptTimeoutMs`) when no requests are in flight.
- Also stops cleanly when the listener is closed.
- Returns `(servedCount, nil)` on graceful stop.

#### `httpServerServeLoopWithDeadline(server, workerFunctionName, workerCount, queueCapacity, requestDeadlineMs): (int, Error?)`
Long-running wrapper over `httpServerServeThreadPoolWithDeadline(..., maxRequests=0, ...)`.

#### `httpServerServeLoopControlled(server, workerFunctionName, workerCount, queueCapacity, stopChannelId, stopPollTimeoutMs): (int, Error?)`
Long-running serve loop with explicit stop-channel control.

Behavior:
- Equivalent to `httpServerServeThreadPoolControlled(..., maxRequests=0, ...)`.
- Use this when shutdown must be driven by runtime coordination (supervisor thread, health manager, etc.) instead of idle timeout alone.
- Preserves the same worker contract as `httpServerServeLoop`.

#### `httpServerServeLoopControlledWithDeadline(server, workerFunctionName, workerCount, queueCapacity, stopChannelId, stopPollTimeoutMs, requestDeadlineMs): (int, Error?)`
Long-running stop-channel controlled serve loop with per-request deadline enforcement.

```tblo
import "lib/http.tblo";

func addRequestId(req: map<string, any>): map<string, any> {
    mapSet(req, "requestId", "r-1");
    return req;
}

func health(req: map<string, any>): map<string, any> {
    var headers: map<string, string> = {"Content-Type": "text/plain"};
    return httpServerResponse(200, "ok", headers);
}

func main(): void {
    var start = httpServerStart("127.0.0.1", 8080, 250, 65536);
    if (start.1 != nil) return;

    var server: HttpServer = start.0;
    var routes: map<string, any> = {};
    httpServerRoute(routes, "GET", "/health", health as any);
    var middleware: array<any> = [addRequestId as any];

    // Serve up to 100 requests; stops early on accept timeout.
    var r = httpServerServeMany(server, routes, middleware, 100);
    if (r.1 != nil) println(r.1.message);

    httpServerClose(server);
}
```

#### Client Wrappers In `lib/http.tblo`
Use the `httpClient*` helpers when you want typed response handling with explicit timeout and status-policy control.

`lib/http.tblo` also emits metrics through `lib/observability.tblo` (best-effort; request behavior is unchanged if metric writes fail).

Common counters/timers:
- Server: `http.server.requests_total`, `http.server.responses_total`, `http.server.errors_total`, `http.server.accept_errors_total`, `http.server.backpressure_total`, `http.server.deadline_timeouts_total`, `http.server.request_latency_ms` (timer)
- Client: `http.client.requests_total`, `http.client.stream_requests_total`, `http.client.responses_total`, `http.client.stream_responses_total`, `http.client.errors_total`, `http.client.stream_body_bytes_total`, `http.client.status_<class>_total`, `http.client.request_latency_ms` (timer)

#### `httpClientRequest(method, url, body, headers, timeoutMs): (HttpClientResponse, Error?)`
Returns a typed `HttpClientResponse` record:
- `status: int`
- `headers: map<string, string>`
- `body: string`

#### `httpClientRequestWithOptions(method, url, body, headers, timeoutMs, requestOptions?): (HttpClientResponse, Error?)`
Like `httpClientRequest`, but accepts per-request transport options.

Supported options:
- `tlsInsecureSkipVerify: bool` (alias: `insecureSkipVerify`) for local/dev self-signed HTTPS endpoints.
- `keepAlive: bool` (alias: `connectionPool`) for opt-in connection reuse to the same origin.
- `gzip: bool` to control `Accept-Encoding: gzip` advertisement and transparent gzip response decoding.

```tblo
var opts: map<string, any> = {"tlsInsecureSkipVerify": true};
var r = httpClientRequestWithOptions(
    "GET",
    "https://localhost:8443/api/status",
    nil,
    nil,
    5000,
    opts
);
if (r.1 != nil) {
    println(r.1.message);
    return;
}
println(r.0.status);
println(r.0.body);
```

#### `httpClientRequest2xx(method, url, body, headers, timeoutMs): (HttpClientResponse, Error?)`
Like `httpClientRequest`, but returns `ERR_HTTP` when status is not `2xx`. Error `data` includes `status`, `headers`, and `body`.

#### `httpClientRequest2xxWithOptions(method, url, body, headers, timeoutMs, requestOptions?): (HttpClientResponse, Error?)`
Same as `httpClientRequest2xx`, with the same `requestOptions` support as `httpClientRequestWithOptions`.

#### `httpClientRequestStream(method, url, body, headers, timeoutMs): (HttpClientStreamResponse, Error?)`
Returns a typed streaming head record:
- `status: int`
- `headers: map<string, string>`
- `socket: any`
- `contentLength: int` (`-1` if unknown)
- `remainingBodyBytes: int` (`-1` if unknown)

Stream helpers:
- `httpClientStreamReadChunk(stream, maxBytes): (HttpClientStreamResponse, string, Error?)`
- `httpClientStreamReadAll(stream, chunkBytes): (HttpClientStreamResponse, string, Error?)`
- `httpClientStreamReadChunkBytes(stream, maxBytes): (HttpClientStreamResponse, bytes, Error?)`
- `httpClientStreamReadAllBytes(stream, chunkBytes): (HttpClientStreamResponse, bytes, Error?)`
- `httpClientStreamClose(stream): void`
- Convenience variants: `httpClientGetStream`, `httpClientPostStream`, and `...Default` timeout wrappers
- Option-aware variant: `httpClientRequestStreamWithOptions(method, url, body, headers, timeoutMs, requestOptions?)`
- Option-aware convenience variants: `httpClientGetStreamWithOptions(url, headers?, timeoutMs, requestOptions?)`, `httpClientPostStreamWithOptions(url, body, headers?, timeoutMs, requestOptions?)`

```tblo
var opts: map<string, any> = {"tlsInsecureSkipVerify": true};
var s = httpClientGetStreamWithOptions(
    "https://localhost:8443/stream",
    nil,
    5000,
    opts
);
if (s.1 != nil) {
    println(s.1.message);
    return;
}

var stream = s.0;
while (true) {
    var r = httpClientStreamReadChunk(stream, 1024);
    if (r.2 != nil) {
        println(r.2.message);
        break;
    }
    stream = r.0;
    if (r.1 == "") break;
    println(r.1);
}
httpClientStreamClose(stream);
```

#### `httpClientGet` / `httpClientPost`
- `httpClientGet(url, headers?, timeoutMs)`
- `httpClientPost(url, body, headers?, timeoutMs)`
- Default-timeout variants: `httpClientRequestDefault`, `httpClientGetDefault`, `httpClientPostDefault`

#### `httpClientRequestJson(method, url, requestValue, responseSchema, headers?, timeoutMs): (any, Error?)`
JSON convenience helper:
- Encodes `requestValue` with `jsonStringify` (when non-nil).
- Applies default JSON headers (`Content-Type` and `Accept`) if missing.
- Enforces `2xx` status.
- Parses response with `jsonParse` and validates with `jsonDecode(responseSchema)`.

Convenience variants:
- `httpClientGetJson(url, responseSchema, headers?, timeoutMs)`
- `httpClientPostJson(url, requestValue, responseSchema, headers?, timeoutMs)`

Option-aware variants:
- `httpClientRequestJsonWithOptions(method, url, requestValue, responseSchema, headers?, timeoutMs, requestOptions?)`
- `httpClientGetJsonWithOptions(url, responseSchema, headers?, timeoutMs, requestOptions?)`
- `httpClientPostJsonWithOptions(url, requestValue, responseSchema, headers?, timeoutMs, requestOptions?)`

#### `httpGet(url: string): (string, Error?)`
Performs a simple HTTP GET request.

- Supports `http://` and `https://` URLs
- HTTPS requires TLS socket support (`tlsIsAvailable()`)
- Has a 10-second timeout
- Returns an error for connection failures, timeouts, unsupported TLS builds, or HTTP error responses

```tblo
// Simple GET request with error handling
var r = httpGet("http://api.example.com/data");
if (r.1 == nil) {
    println("Response: " + r.0);
} else {
    println("Failed to fetch data: " + r.1.message);
}
```

#### `httpGetWithHeaders(url: string, headers: map<string, string>): (string, Error?)`
Performs an HTTP GET request with custom headers. Useful for APIs requiring authentication or specific content type headers.

```tblo
// GET request with authorization header
var headers: map<string, string> = {
    "Authorization": "Bearer token123",
    "Accept": "application/json"
};
var r = httpGetWithHeaders("http://api.example.com/user", headers);
if (r.1 == nil) {
    println("User data: " + r.0);
}
```

#### `httpPost(url: string, body: string): (string, Error?)`
Performs an HTTP POST request with the given body. Content-Type defaults to `text/plain`.

```tblo
// Simple POST request
var body: string = "name=John&age=30";
var r = httpPost("http://api.example.com/submit", body);
if (r.1 == nil) {
    println("Server response: " + r.0);
} else {
    println("POST request failed: " + r.1.message);
}
```

#### `httpPostWithHeaders(url: string, body: string, headers: map<string, string>): (string, Error?)`
Performs an HTTP POST request with custom headers and body. Use this for JSON APIs or other content types.

```tblo
// POST JSON data with proper headers
var jsonBody: string = '{"username": "alice", "password": "secret123"}';
var headers: map<string, string> = {
    "Content-Type": "application/json",
    "Authorization": "Bearer token123"
};
var r = httpPostWithHeaders("http://api.example.com/login", jsonBody, headers);
if (r.1 == nil) {
    println("Login response: " + r.0);
} else {
    println("Login failed: " + r.1.message);
}
```

#### TCP Socket Functions

The TCP socket functions provide low-level network access for custom protocols or interactive connections. Sockets are opaque handles (runtime type `socket`, assignable to `any`).

#### `tcpListen(host: string, port: int): (any, Error?)`
Creates a listening TCP socket bound to `host:port`.

- `port` must be in `0..65535`
- `host` may be `"127.0.0.1"` for loopback-only listeners
- returns `(socket, nil)` on success

```tblo
var l = tcpListen("127.0.0.1", 8080);
if (l.1 != nil) {
    println("listen failed: " + l.1.message);
    return;
}
var listener: any = l.0;
```

#### `tcpAccept(listener: any, timeoutMs: int): (any, Error?)`
Accepts one incoming connection from a listening socket.

- `timeoutMs` must be in `[0, 120000]` (`0` = non-blocking poll)
- returns `(socket, nil)` for the accepted client connection

```tblo
var a = tcpAccept(listener, 5000);
if (a.1 != nil) {
    println("accept failed: " + a.1.message);
    return;
}
var client: any = a.0;
```

#### `tcpConnect(host: string, port: int): (any, Error?)`
Establishes a TCP connection to the specified host and port.

- Has a 10-second connection timeout
- Returns `(nil, err)` if the host is unreachable or connection fails

```tblo
// Connect to a TCP server
var r = tcpConnect("example.com", 8080);
if (r.1 == nil) {
    var socket: any = r.0;
    println("Connected successfully");
    // Use socket for communication...
} else {
    println("Failed to connect to server: " + r.1.message);
}
```

#### `tcpSend(socket: any, data: string): (int, Error?)`
Sends data over an established TCP connection. Returns the number of bytes sent.

```tblo
// Send data over TCP
var r = tcpConnect("example.com", 8080);
if (r.1 == nil) {
    var socket: any = r.0;
    var message: string = "Hello, Server!\n";
    var sent = tcpSend(socket, message);
    if (sent.1 == nil) println("Sent " + str(sent.0) + " bytes");
}
```

#### `tcpReceive(socket: any, maxBytes: int): (string, Error?)`
Receives up to `maxBytes` bytes from a TCP socket.

- Blocks until data is available or timeout occurs
- Returns an error if the connection is closed or an error occurs

```tblo
// Receive data from TCP server
var r = tcpConnect("example.com", 8080);
if (r.1 == nil) {
    var socket: any = r.0;
    // Send a request
    tcpSend(socket, "GET /data HTTP/1.1\r\nHost: example.com\r\n\r\n");

    // Receive response (up to 4096 bytes)
    var response = tcpReceive(socket, 4096);
    if (response.1 == nil) {
        println("Received: " + response.0);
    } else {
        println("Connection closed or error occurred: " + response.1.message);
    }

    tcpClose(socket);
}
```

#### `tcpClose(socket: any): void`
Closes an established TCP connection and releases associated resources. Always call this when done with a socket to prevent resource leaks.

```tblo
// Complete TCP client example with proper cleanup
func fetchDataFromServer(host: string, port: int, request: string): (string, Error?) {
    var c = tcpConnect(host, port);
    if (c.1 != nil) return ("", c.1);

    var socket: any = c.0;
    var sent = tcpSend(socket, request);
    if (sent.1 != nil) {
        tcpClose(socket);
        return ("", sent.1);
    }

    var response = tcpReceive(socket, 8192);
    tcpClose(socket);
    return response;
}

func main(): void {
    var request: string = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    var response = fetchDataFromServer("example.com", 80, request);
    if (response.1 == nil) {
        println("Server responded with " + str(len(response.0)) + " bytes");
    }
}
```

#### TLS Socket Functions (Windows)

For HTTPS-style encrypted transports, TabloLang provides explicit TLS socket builtins. These currently target Windows builds (Schannel backend) and return `ERR_UNSUPPORTED` on other platforms.

#### `tlsIsAvailable(): bool`
Returns whether TLS socket builtins are available in the current runtime build.

#### `tlsConnect(host: string, port: int, timeoutMs: int): (any, Error?)`
Establishes a TCP connection and performs a TLS client handshake.

- `host` must be non-empty.
- `port` must be in `1..65535`.
- `timeoutMs` must be in `1..120000`.
- Returns `(socket, nil)` on success, `(nil, err)` on failure.
- Certificate validation is enabled by default.
- For HTTP client requests, use `httpRequestWithOptions`/`httpRequestHeadWithOptions` with `{"tlsInsecureSkipVerify": true}` for local/dev self-signed endpoints (and optionally `keepAlive: true` to reuse connections).

#### `tlsSend(socket: any, data: string): (int, Error?)`
Sends plaintext data over a TLS socket created by `tlsConnect`.

#### `tlsReceive(socket: any, maxBytes: int): (string, Error?)`
Receives and decrypts up to `maxBytes` bytes from a TLS socket created by `tlsConnect`.

#### `tlsClose(socket: any): (bool, Error?)`
Closes a TLS socket created by `tlsConnect` and releases its transport resources.

### Concurrency Runtime (Thread-Safe Foundation)

TabloLang now provides process-level concurrency primitives with ownership isolation:
- Each spawned thread runs in its own runtime/VM instance.
- Cross-thread data exchange uses synchronized channels (message passing).
- Shared mutable state uses explicit synchronized shared cells.

Current shareable payload types are:
- `nil`, `int`, `bool`, `double`, `string`, `bytes`
- Typed variants also support JSON-compatible payloads (`array`, `map`, record-like maps) validated with `jsonDecode` schema descriptors.

#### `syncChannelCreate(capacity: int): (int, Error?)`
Creates a bounded channel and returns a channel id.

#### `syncChannelSend(channelId: int, value: any, timeoutMs: int): (bool, Error?)`
Sends a shareable value to a channel, waiting up to `timeoutMs`.

#### `syncChannelRecv(channelId: int, timeoutMs: int): (any, Error?)`
Receives one value from a channel, waiting up to `timeoutMs`.

#### `syncChannelClose(channelId: int): (bool, Error?)`
Closes a channel and wakes blocked send/recv operations.

#### `syncSharedCreate(initial: any): (int, Error?)`
Creates a synchronized shared cell and returns its id.

#### `syncSharedGet(cellId: int): (any, Error?)`
Reads the current value from a shared cell.

#### `syncSharedSet(cellId: int, value: any): (bool, Error?)`
Atomically replaces the value in a shared cell.

#### `syncThreadSpawn(functionName: string, inboxChannelId: int, outboxChannelId: int): (int, Error?)`
Spawns a runtime thread that executes a zero-argument top-level function.

#### `syncThreadJoin(threadId: int, timeoutMs: int): (bool, Error?)`
Joins a thread with timeout.

#### `syncThreadInbox(): int` / `syncThreadOutbox(): int`
Returns the channel ids bound to the current thread (`-1` on the main thread).

#### Typed channel/shared/thread APIs
These APIs use the same schema language as `jsonDecode(value, schema)`.

#### `syncChannelSendTyped(channelId: int, value: any, schema: any, timeoutMs: int): (bool, Error?)`
Validates `value` against `schema`, then sends it.

#### `syncChannelRecvTyped(channelId: int, schema: any, timeoutMs: int): (any, Error?)`
Receives one message and decodes it against `schema`.

#### `syncSharedCreateTyped(initial: any, schema: any): (int, Error?)`
Creates a synchronized shared cell initialized with a schema-validated value.

#### `syncSharedGetTyped(cellId: int, schema: any): (any, Error?)`
Reads and decodes a shared value against `schema`.

#### `syncSharedSetTyped(cellId: int, value: any, schema: any): (bool, Error?)`
Validates and atomically writes a shared value.

#### `syncThreadSpawnTyped(functionName: string, arg: any, argSchema: any, inboxChannelId: int, outboxChannelId: int): (int, Error?)`
Spawns a thread and passes one typed argument payload to the worker.

#### `syncThreadArgTyped(schema: any): (any, Error?)`
Inside a worker thread, decodes the spawn argument with `schema`.

#### `syncThreadJoinTyped(threadId: int, schema: any, timeoutMs: int): (any, Error?)`
Joins a thread and decodes the worker function return value with `schema`.

#### Arc-style shared state guards
Arc ids are shared-cell ids with explicit guard acquisition/release for lock ownership.

#### `syncArcCreate(initial: any): (int, Error?)`
Creates an Arc-shared value and returns an arc id.

#### `syncArcClone(arcId: int): (int, Error?)`
Returns another handle to the same arc id.

#### `syncArcGuardAcquire(arcId: int): (int, Error?)`
Locks the Arc value and returns a guard id.

#### `syncArcGuardRead(guardId: int): (any, Error?)`
Reads the guarded value.

#### `syncArcGuardWrite(guardId: int, value: any): (bool, Error?)`
Writes the guarded value.

#### `syncArcGuardRelease(guardId: int): (bool, Error?)`
Releases the guard lock. Guards are single-owner and must be released by the acquiring thread.

### Process Execution Functions

Use these low-level builtins to launch subprocesses with captured output and explicit lifecycle control.

#### `processSpawn(command: string, args: array<string>, captureStdout: bool, captureStderr: bool): (int, Error?)`
Starts a subprocess and returns a process id.
- `command` is the executable name/path
- `args` are positional arguments (excluding `command`)
- `captureStdout`/`captureStderr` enable output capture buffers

#### `processWriteStdin(processId: int, data: string): (int, Error?)`
Writes bytes to subprocess stdin.
- Returns the number of bytes written
- Returns `ERR_IO` if stdin is closed or the process is gone

#### `processCloseStdin(processId: int): (bool, Error?)`
Closes subprocess stdin to signal EOF.

#### `processReadStdout(processId: int, maxBytes: int): (string?, Error?)`
Reads and consumes up to `maxBytes` bytes from captured stdout.
- `maxBytes` must be in `1..1048576`
- Returns `nil` when no chunk is currently available
- Process must be started with `captureStdout=true`

#### `processReadStderr(processId: int, maxBytes: int): (string?, Error?)`
Reads and consumes up to `maxBytes` bytes from captured stderr.
- `maxBytes` must be in `1..1048576`
- Returns `nil` when no chunk is currently available
- Process must be started with `captureStderr=true`

#### `processWait(processId: int, timeoutMs: int): (map<string, any>, Error?)`
Polls/waits for process completion (`timeoutMs` in `0..120000`) and returns a result map:
- `finished: bool`
- `timedOut: bool`
- `exitCode: int`
- `stdout: string`
- `stderr: string`
- `killed: bool`
- `stdoutTruncated: bool`
- `stderrTruncated: bool`

#### `processKill(processId: int): (bool, Error?)`
Terminates a subprocess.

#### Standard library module: `lib/process.tblo`
High-level process helpers built on top of `processSpawn`/`processWait` for safer backend/CLI orchestration.

It provides:
- `record ProcessHandle { id }`
- `record ProcessOptions { captureStdout, captureStderr }`
- `record ProcessResult { finished, timedOut, exitCode, stdout, stderr, killed, stdoutTruncated, stderrTruncated }`
- `record ProcessPipelineResult { left, right }`
- `processStart(program: string, args: array<string>, options: ProcessOptions?): (ProcessHandle, Error?)`
- `processWriteInput(handle: ProcessHandle, data: string): (int, Error?)`
- `processReadStdoutChunk(handle: ProcessHandle, maxBytes: int): (string?, Error?)`
- `processReadStderrChunk(handle: ProcessHandle, maxBytes: int): (string?, Error?)`
- `processCloseInput(handle: ProcessHandle): Error?`
- `processTerminate(handle: ProcessHandle): Error?`
- `processWaitFor(handle: ProcessHandle, timeoutMs: int): (ProcessResult, Error?)`
- `processWaitWithContext(handle: ProcessHandle, ctx: Context, pollMs: int): (ProcessResult, Error?)`
- `processRunWithInput(program: string, args: array<string>, input: string, timeoutMs: int, options: ProcessOptions?): (ProcessResult, Error?)`
- `processRunWithInputDefault(program: string, args: array<string>, input: string, timeoutMs: int): (ProcessResult, Error?)`
- `processRunPipeline(leftProgram: string, leftArgs: array<string>, rightProgram: string, rightArgs: array<string>, timeoutMs: int, leftOptions: ProcessOptions?, rightOptions: ProcessOptions?): (ProcessPipelineResult, Error?)`
- `processRunPipelineDefault(leftProgram: string, leftArgs: array<string>, rightProgram: string, rightArgs: array<string>, timeoutMs: int): (ProcessPipelineResult, Error?)`
- `processRunPipelineStream(leftProgram: string, leftArgs: array<string>, rightProgram: string, rightArgs: array<string>, timeoutMs: int, leftOptions: ProcessOptions?, rightOptions: ProcessOptions?, chunkBytes: int): (ProcessPipelineResult, Error?)`
- `processRunPipelineStreamDefault(leftProgram: string, leftArgs: array<string>, rightProgram: string, rightArgs: array<string>, timeoutMs: int, chunkBytes: int): (ProcessPipelineResult, Error?)`
- `processRun(program: string, args: array<string>, timeoutMs: int, options: ProcessOptions?): (ProcessResult, Error?)`
- `processRunDefault(program: string, args: array<string>, timeoutMs: int): (ProcessResult, Error?)`

Behavior notes:
- Wrapper errors preserve cause data and add contextual metadata (`context`, `detail`).
- `processWaitWithContext` kills the subprocess when context cancellation/deadline is reached.
- `processReadStdoutChunk`/`processReadStderrChunk` are non-blocking chunk reads; `nil` means "no chunk currently available".
- `processRunPipeline` is a buffered two-stage pipeline (`left.stdout` becomes `right` stdin).
- `processRunPipelineStream` forwards stdout incrementally in chunks to avoid buffering full left output in memory.
- Left-stage stdout capture is forced on in pipeline mode so forwarding always works.
- Timeouts are validated to `0..120000` ms (or `1..120000` for polling intervals).

```tblo
import "lib/process.tblo";

func isWindowsHost(): bool {
    var lower = envGet("ComSpec");
    if (lower.1 == nil && lower.0 != nil) return true;
    var upper = envGet("COMSPEC");
    if (upper.1 == nil && upper.0 != nil) return true;
    return false;
}

func echoCommand(text: string): (string, array<string>) {
    if (isWindowsHost()) return ("cmd", ["/c", "echo " + text]);
    return ("sh", ["-c", "echo " + text]);
}

func main(): void {
    var cmd = echoCommand("hello");
    var r = processRunDefault(cmd.0, cmd.1, 2000);
    if (r.1 != nil) {
        println("process failed: " + r.1.message);
        return;
    }
    println(r.0.exitCode == 0);
    println(find(r.0.stdout, "hello") >= 0);
}
```

### SQLite Database Functions

Use these low-level builtins for embedded SQLite workflows. All operations use the standard `(value, Error?)` convention.

#### `sqliteIsAvailable(): bool`
Returns whether SQLite is currently available in this runtime.

#### `sqliteOpen(path: string): (int, Error?)`
Opens (or creates) a SQLite database and returns a database id.

#### `sqliteClose(dbId: int): (bool, Error?)`
Closes a previously opened database id.

#### `sqliteExec(dbId: int, sql: string): (bool, Error?)`
Executes a non-row SQL statement (`CREATE`, `INSERT`, `UPDATE`, `DELETE`, DDL).

#### `sqliteQuery(dbId: int, sql: string): (array<map<string, any>>, Error?)`
Executes a query and returns all rows as maps keyed by column name.

#### `sqlitePrepare(dbId: int, sql: string): (int, Error?)`
Prepares a query statement and returns a statement id.

#### `sqliteBindInt(stmtId: int, index: int, value: int): (bool, Error?)`
Binds an integer parameter (`index` is 1-based).

#### `sqliteBindDouble(stmtId: int, index: int, value: double|int): (bool, Error?)`
Binds a numeric parameter as floating-point.

#### `sqliteBindString(stmtId: int, index: int, value: string): (bool, Error?)`
Binds a text parameter.

#### `sqliteBindBytes(stmtId: int, index: int, value: bytes): (bool, Error?)`
Binds a blob parameter.

#### `sqliteBindNull(stmtId: int, index: int): (bool, Error?)`
Binds a SQL `NULL` parameter.

#### `sqliteReset(stmtId: int): (bool, Error?)`
Resets a prepared statement so it can be executed again.

#### `sqliteClearBindings(stmtId: int): (bool, Error?)`
Clears all currently bound parameters from a prepared statement.

#### `sqliteChanges(dbId: int): (int, Error?)`
Returns the number of rows changed by the most recent write statement on the connection.

#### `sqliteLastInsertRowId(dbId: int): (int, Error?)`
Returns the last inserted row id for the connection.

#### `sqliteStep(stmtId: int): (map<string, any>?, Error?)`
Steps a prepared statement once.
- Returns `(row, nil)` while rows remain
- Returns `(nil, nil)` when iteration is complete

#### `sqliteFinalize(stmtId: int): (bool, Error?)`
Finalizes a prepared statement id.

#### Standard library module: `lib/sqlite.tblo`
Higher-level wrappers for common backend data access patterns.

It provides:
- `record SqliteDb { id, path }`
- `record SqliteStatement { id, db }`
- `sqliteOpenDb(path: string): (SqliteDb, Error?)`
- `sqliteOpenMemory(): (SqliteDb, Error?)`
- `sqliteCloseDb(db: SqliteDb): Error?`
- `sqliteExecSql(db: SqliteDb, sql: string): (bool, Error?)`
- `sqliteExecBatch(db: SqliteDb, statements: array<string>): (int, Error?)`
- `sqliteExecBatchTx(db: SqliteDb, statements: array<string>): (int, Error?)`
- `sqliteBeginTx(db: SqliteDb): Error?`
- `sqliteCommitTx(db: SqliteDb): Error?`
- `sqliteRollbackTx(db: SqliteDb): Error?`
- `sqliteQueryAll(db: SqliteDb, sql: string): (array<map<string, any>>, Error?)`
- `sqliteQueryOne(db: SqliteDb, sql: string): (map<string, any>?, Error?)`
- `sqlitePrepareQuery(db: SqliteDb, sql: string): (SqliteStatement, Error?)`
- `sqliteBindIntParam(stmt: SqliteStatement, index: int, value: int): Error?`
- `sqliteBindDoubleParam(stmt: SqliteStatement, index: int, value: any): Error?`
- `sqliteBindStringParam(stmt: SqliteStatement, index: int, value: string): Error?`
- `sqliteBindBytesParam(stmt: SqliteStatement, index: int, value: bytes): Error?`
- `sqliteBindNullParam(stmt: SqliteStatement, index: int): Error?`
- `sqliteResetStmt(stmt: SqliteStatement): Error?`
- `sqliteClearStmtBindings(stmt: SqliteStatement): Error?`
- `sqliteChangesCount(db: SqliteDb): (int, Error?)`
- `sqliteLastInsertId(db: SqliteDb): (int, Error?)`
- `sqliteBindParam(stmt: SqliteStatement, index: int, value: any): Error?`
- `sqliteBindParams(stmt: SqliteStatement, params: array<any>): Error?`
- `sqliteExecPrepared(stmt: SqliteStatement, params: array<any>): Error?`
- `sqliteExecPreparedMany(stmt: SqliteStatement, rows: array<array<any>>): (int, Error?)`
- `sqliteQueryPreparedAll(stmt: SqliteStatement, params: array<any>): (array<map<string, any>>, Error?)`
- `sqliteQueryPreparedOne(stmt: SqliteStatement, params: array<any>): (map<string, any>?, Error?)`
- `sqliteQueryPreparedAllAs(stmt: SqliteStatement, params: array<any>, schema: any): (array<any>, Error?)`
- `sqliteQueryPreparedOneAs(stmt: SqliteStatement, params: array<any>, schema: any): (any, Error?)`
- `sqliteStepRow(stmt: SqliteStatement): (map<string, any>?, Error?)`
- `sqliteFinalizeStmt(stmt: SqliteStatement): Error?`
- `sqliteQueryAllAs(db: SqliteDb, sql: string, schema: any): (array<any>, Error?)`
- `sqliteQueryOneAs(db: SqliteDb, sql: string, schema: any): (any, Error?)`

Behavior notes:
- Returns `ERR_UNSUPPORTED` when no SQLite runtime library is available on the host build.
- `sqliteQueryAllAs`/`sqliteQueryOneAs` and prepared variants `sqliteQueryPreparedAllAs`/`sqliteQueryPreparedOneAs` validate row shapes with `jsonDecode` schemas.
- Wrapper errors keep structured context via `Error.data`.
- `sqliteBindParam` maps `bool` to `0/1`, supports `int/double/string/bytes/nil`, and rejects unsupported types.
- `sqliteExecPrepared` expects non-row statements; `sqliteQueryPreparedAll`/`sqliteQueryPreparedOne` are row-oriented prepared helpers.
- `sqliteExecBatchTx` wraps `begin/commit` with rollback-on-error semantics for statement batches.
- Runtime loading probes `winsqlite3.dll`/`sqlite3.dll` on Windows and common `libsqlite3` names on Linux/macOS.
- To force build-time linking instead of runtime loading, configure CMake with `-DTABLO_SQLITE_STATIC_LINK=ON`.

```tblo
import "lib/sqlite.tblo";

func main(): void {
    var opened = sqliteOpenMemory();
    if (opened.1 != nil) return;

    var db: SqliteDb = opened.0;
    sqliteExecSql(db, "create table users(id integer primary key, name text);");
    sqliteExecSql(db, "insert into users(id, name) values (1, 'alice');");

    var one = sqliteQueryOne(db, "select id, name from users where id = 1;");
    if (one.1 == nil && one.0 != nil) {
        var row: map<string, any> = one.0 as map<string, any>;
        println(mapGetString(row, "name") as string);
    }

    sqliteCloseDb(db);
}
```

### File I/O Functions

**Note:** File I/O is sandboxed. All file paths are resolved relative to the sandbox root (the directory containing the main program). Path traversal attempts (e.g., `../file.txt`) are rejected. On POSIX platforms, secure sandbox open also rejects regular files with multiple hardlinks to prevent hardlink alias escapes.

#### Streaming file handles
For efficient NDJSON/CSV processing, use streaming file handles instead of reopening a file for every line. File handles are opaque handles (runtime type `file`, assignable to `any`).

#### `file_open(path: string, mode: string): (any, Error?)`
Opens a file and returns a handle.
- On success: returns `(file, nil)`
- On error: returns `(nil, err)`

Allowed modes: `r`, `rb`, `w`, `wb`, `a`, `ab`, `r+`, `rb+`, `w+`, `wb+`, `a+`, `ab+`.

#### `file_read_line(file: any): (string?, Error?)`
Reads the next line from an open file handle.
- On success: returns `(line, nil)` (without the trailing newline)
- On EOF: returns `(nil, nil)`
- On error: returns `(nil, err)`

This allows you to distinguish **EOF** from an **empty line** (`""`).

#### `file_close(file: any): void`
Closes an open file handle. Safe to call multiple times.

```tblo
// NDJSON example: stream line-by-line without loading the entire file.
func main(): void {
    var r = file_open("data.ndjson", "r");
    if (r.1 != nil) {
        println("file_open failed: " + r.1.message);
        return;
    }

    var f: any = r.0;
    defer file_close(f);

    while (true) {
        var lr = file_read_line(f);
        if (lr.1 != nil) {
            println("file_read_line failed: " + lr.1.message);
            return;
        }
        if (lr.0 == nil) break; // EOF

        var line: string = lr.0 as string;
        var (obj, err) = jsonParse(line);
        if (err != nil) {
            println("bad json: " + err.message);
            return;
        }

        // process obj...
    }
}
```

#### Handle-based streaming helpers
These helpers operate on open file handles (`file_open`) and TCP sockets (`tcpConnect`/`tcpAccept`).

#### `ioReadLine(handle: any): (string?, Error?)`
Reads one line from a file/socket handle.
- Returns `(line, nil)` when a line is available (without trailing newline)
- Returns `(nil, nil)` on EOF
- Returns `(nil, err)` on read failure

#### `ioReadAll(handle: any): (string, Error?)`
Reads all remaining data from a file/socket handle.
- For sockets, reading continues until EOF
- Returns `("", err)` on failure

#### `ioReadChunk(handle: any, maxBytes: int): (string?, Error?)`
Reads up to `maxBytes` bytes from a file/socket handle as a string chunk.
- `maxBytes` must be in `1..1048576`
- Returns `(nil, nil)` on EOF

#### `ioReadChunkBytes(handle: any, maxBytes: int): (bytes?, Error?)`
Reads up to `maxBytes` bytes from a file/socket handle as bytes.
- `maxBytes` must be in `1..1048576`
- Returns `(nil, nil)` on EOF

#### `ioReadExactlyBytes(handle: any, byteCount: int): (bytes, Error?)`
Reads exactly `byteCount` bytes from a file/socket handle.
- `byteCount` must be `>= 0`
- Returns an error if EOF/connection close happens before all requested bytes are read

#### `ioWriteAll(handle: any, data: string): (int, Error?)`
Writes the entire string to a file/socket handle.
- Returns `(bytesWritten, nil)` on success
- Returns partial byte count with an error on failure

#### `ioWriteBytesAll(handle: any, data: bytes): (int, Error?)`
Writes the entire bytes payload to a file/socket handle.
- Returns `(bytesWritten, nil)` on success
- Returns partial byte count with an error on failure

#### `ioCopy(reader: any, writer: any, chunkBytes: int): (int, Error?)`
Copies bytes from one handle to another in chunks.
- `chunkBytes` must be in `1..1048576`
- Returns total copied byte count

```tblo
func main(): void {
    var src = file_open("input.txt", "rb");
    if (src.1 != nil) return;
    var dst = file_open("output.txt", "wb");
    if (dst.1 != nil) {
        file_close(src.0);
        return;
    }

    var copied = ioCopy(src.0, dst.0, 4096);
    if (copied.1 != nil) {
        println("copy failed: " + copied.1.message);
    } else {
        println("copied bytes: " + str(copied.0));
    }

    file_close(src.0);
    file_close(dst.0);
}
```

#### Standard library module: `lib/url.tblo`
For query-string handling built on top of `urlEncode`/`urlDecode`.

It provides:
- `queryParse(rawQuery: string): (map<string, string>, Error?)`
- `queryStringify(params: map<string, string>): (string, Error?)`

Behavior notes:
- `queryParse` accepts optional leading `?`
- `+` is decoded as space for form-style query inputs
- Missing `=` is treated as empty value (`key` -> `key=""`)
- Empty keys are rejected with `ERR_PARSE`
- Duplicate keys keep the last value
- `queryStringify` encodes both keys and values and rejects empty keys

```tblo
import "lib/url.tblo";

func main(): void {
    var p = queryParse("?name=Alice+Doe&city=New%20York");
    if (p.1 != nil) return;

    println(mapGetString(p.0, "name")); // "Alice Doe"
    println(mapGetString(p.0, "city")); // "New York"

    var q = queryStringify(p.0);
    if (q.1 == nil) {
        println(q.0); // e.g. "name=Alice%20Doe&city=New%20York"
    }
}
```

#### Standard library module: `lib/path.tblo`
Cross-platform path helpers for CLI/tools/server code.

It provides:
- `pathClean(path: string): string`
- `pathJoin(parts: array<string>): string`
- `pathIsAbs(path: string): bool`
- `pathBase(path: string): string`
- `pathDir(path: string): string`
- `pathExt(path: string): string`
- `pathStem(path: string): string`
- `pathSplit(path: string): (string, string)`

Behavior notes:
- Accepts both `/` and `\` as separators
- Normalizes separators to `/`
- Resolves `.` and `..` segments (`pathClean`)
- Preserves Windows drive prefixes (`C:`) when present
- Empty input to `pathClean` becomes `.`

```tblo
import "lib/path.tblo";

func main(): void {
    println(pathClean("C:\\tmp\\..\\logs\\app.txt")); // "C:/logs/app.txt"
    println(pathJoin(["api", "v1", "users"]));        // "api/v1/users"

    var split = pathSplit("/srv/app/config.json");
    println(split.0); // "/srv/app"
    println(split.1); // "config.json"
}
```

#### Standard library module: `lib/glob.tblo`
Glob pattern matching helpers for CLI tooling, file discovery, and config-driven includes/excludes.

It provides:
- `globMatch(pattern: string, pathValue: string): (bool, Error?)`
- `globMatchAny(patterns: array<string>, pathValue: string): (bool, Error?)`
- `globFilter(pattern: string, pathValues: array<string>): (array<string>, Error?)`
- `globFilterAny(patterns: array<string>, pathValues: array<string>): (array<string>, Error?)`

Behavior notes:
- Supports `*` and `?` within a single path segment.
- Supports `**` for zero-or-more full path segments.
- Normalizes `\` and `/` separators and cleans `.` / `..` segments before matching.
- Empty patterns are rejected with `ERR_INVALID_ARGUMENT`.

```tblo
import "lib/glob.tblo";

func main(): void {
    var matched = globMatch("src/**/*.tblo", "src/http/server.tblo");
    if (matched.1 == nil) {
        println(matched.0); // true
    }

    var files: array<string> = [
        "src/main.tblo",
        "README.md",
        "tests/tablo_tests/testing_assertions_test.tblo"
    ];
    var filtered = globFilterAny(["**/*.tblo", "**/*.md"], files);
    if (filtered.1 == nil) {
        println(len(filtered.0)); // 3
    }
}
```

#### Standard library module: `lib/fs.tblo`
Filesystem convenience helpers for common app/backend workflows.

It provides:
- `fsReadText(path: string): (string, Error?)`
- `fsWriteText(path: string, text: string): (bool, Error?)`
- `fsAppendText(path: string, text: string): (bool, Error?)`
- `fsReadBytes(path: string): (bytes, Error?)`
- `fsWriteBytes(path: string, data: bytes): (bool, Error?)`
- `fsAppendBytes(path: string, data: bytes): (bool, Error?)`
- `fsFileExists(path: string): (bool, Error?)`
- `fsEnsureDeleted(path: string): Error?`
- `fsReadLines(path: string, maxLines: int): (array<string>, Error?)`
- `fsWriteLines(path: string, lines: array<string>, trailingNewline: bool): (bool, Error?)`
- `fsReadJson(path: string): (any, Error?)`
- `fsReadJsonAs(path: string, schema: any): (any, Error?)`
- `fsWriteJson(path: string, value: any): (bool, Error?)`
- `fsWriteJsonPretty(path: string, value: any): (bool, Error?)`
- `fsCopyFile(srcPath: string, dstPath: string, chunkBytes: int): (int, Error?)`
- `fsMoveReplace(srcPath: string, dstPath: string, chunkBytes: int): (int, Error?)`

Behavior notes:
- Wrappers include contextual error metadata (`context`, `filePath`) and preserve parse/decode details (`line`, `column`, `path`, `expectedType`, `actualType`) when available.
- `fsReadLines` treats `maxLines <= 0` as "read until EOF".
- `fsCopyFile` validates `chunkBytes` in `1..1048576`.
- `fsMoveReplace` is copy-then-delete for portable replace behavior.

```tblo
import "lib/fs.tblo";

func main(): void {
    var w = fsWriteJson("user.json", { "name": "Ada", "age": 37 } as any);
    if (w.1 != nil) return;

    var r = fsReadJsonAs("user.json", {
        "type": "record",
        "fields": {
            "name": "string",
            "age": "int"
        }
    });
    if (r.1 != nil) return;

    var user: map<string, any> = r.0 as map<string, any>;
    println(mapGetString(user, "name")); // "Ada"
}
```

#### Standard library module: `lib/uuid.tblo`
UUID helpers for service/backend identifiers and storage keys.

It provides:
- `uuidNil(): string`
- `uuidIsValid(value: string): bool`
- `uuidIsCanonical(value: string): bool`
- `uuidNormalize(value: string): (string, Error?)`
- `uuidToBytes(value: string): (bytes, Error?)`
- `uuidFromBytes(data: bytes): (string, Error?)`
- `uuidVersion(value: string): (int, Error?)`
- `uuidVariant(value: string): (string, Error?)`
- `uuidV4(): (string, Error?)`
- `uuidV4Batch(count: int): (array<string>, Error?)`

Behavior notes:
- `uuidIsValid` accepts both dashed (36 chars) and compact (32 hex chars) forms.
- `uuidNormalize` returns lowercase canonical dashed form.
- `uuidV4` sets version/variant bits per RFC4122 (`version=4`, `variant=rfc4122`).
- `uuidVariant` returns one of `ncs`, `rfc4122`, `microsoft`, `future`.

```tblo
import "lib/uuid.tblo";

func main(): void {
    var id_r = uuidV4();
    if (id_r.1 != nil) return;

    var id: string = id_r.0;
    println(id);
    println(uuidIsCanonical(id));

    var bytes_r = uuidToBytes(id);
    if (bytes_r.1 != nil) return;

    var back_r = uuidFromBytes(bytes_r.0);
    if (back_r.1 == nil) {
        println(back_r.0 == id);
    }
}
```

#### Standard library module: `lib/netip.tblo`
IPv4/CIDR helpers for backend networking and ACL logic.

It provides:
- `record NetIpCidr { network, prefix, mask, broadcast }`
- `netipIsIpv4(value: string): bool`
- `netipParseIpv4(value: string): (int, Error?)`
- `netipFormatIpv4(value: int): (string, Error?)`
- `netipNormalizeIpv4(value: string): (string, Error?)`
- `netipMaskFromPrefix(prefix: int): (int, Error?)`
- `netipPrefixFromMask(mask: int): (int, Error?)`
- `netipPrefixToMask(prefix: int): (string, Error?)`
- `netipMaskToPrefix(mask: string): (int, Error?)`
- `netipParseCidr(value: string): (NetIpCidr, Error?)`
- `netipCidrContains(cidr: NetIpCidr, ip: string): (bool, Error?)`
- `netipCidrContainsInt(cidr: NetIpCidr, ip: int): (bool, Error?)`
- `netipCidrHostCount(cidr: NetIpCidr): (int, Error?)`
- `netipCidrNetworkText(cidr: NetIpCidr): (string, Error?)`
- `netipCidrBroadcastText(cidr: NetIpCidr): (string, Error?)`
- `netipCidrRangeText(cidr: NetIpCidr): (string, string, Error?)`

Behavior notes:
- IPv4 parsing accepts dotted decimal with optional leading zeros.
- CIDR parse normalizes network/broadcast boundaries from host IP + prefix.
- Prefix/mask helpers validate contiguous masks and prefix range `0..32`.

```tblo
import "lib/netip.tblo";

func main(): void {
    var cidr_r = netipParseCidr("10.8.0.0/16");
    if (cidr_r.1 != nil) return;

    var in_r = netipCidrContains(cidr_r.0, "10.8.12.33");
    if (in_r.1 == nil) println(in_r.0); // true

    var range_r = netipCidrRangeText(cidr_r.0);
    if (range_r.2 == nil) {
        println(range_r.0); // 10.8.0.0
        println(range_r.1); // 10.8.255.255
    }
}
```

#### Standard library module: `lib/semver.tblo`
SemVer helpers for package/runtime compatibility checks.

It provides:
- `record SemverVersion { major, minor, patch, prerelease, build }`
- `semverParse(text: string): (SemverVersion, Error?)`
- `semverString(version: SemverVersion): string`
- `semverNormalize(text: string): (string, Error?)`
- `semverIsValid(text: string): bool`
- `semverCompare(left: string, right: string): (int, Error?)`
- `semverSatisfies(versionText: string, constraintText: string): (bool, Error?)`
- `semverBumpMajor(text: string): (string, Error?)`
- `semverBumpMinor(text: string): (string, Error?)`
- `semverBumpPatch(text: string): (string, Error?)`

Constraint support in `semverSatisfies`:
- Comparators: `>`, `>=`, `<`, `<=`, `=`, `==`, `!=`
- Compatibility ranges: `^`, `~`
- Wildcards/partials: `*`, `x`, `1`, `1.2`, `1.x`, `1.2.x`
- Hyphen ranges: `1.2.3 - 2.0.0`
- OR groups: `||`

```tblo
import "lib/semver.tblo";

func main(): void {
    var ok = semverSatisfies("1.5.2", "^1.2.0");
    if (ok.1 == nil) println(ok.0); // true

    var cmp = semverCompare("1.0.0-alpha", "1.0.0");
    if (cmp.1 == nil) println(cmp.0 < 0); // true

    var next = semverBumpMinor("2.4.9");
    if (next.1 == nil) println(next.0); // "2.5.0"
}
```

#### Standard library module: `lib/regexp.tblo`
Practical regular-expression helpers for parsing and transformation.

Supported syntax (subset):
- Anchors: `^`, `$`
- Quantifiers: `*`, `+`, `?`
- Wildcard: `.`
- Character classes: `[abc]`, `[a-z]`, `[^0-9]`
- Escapes: `\d`, `\w`, `\s`, and escaped metacharacters

It provides:
- `record RegexpMatch { start, end, text }`
- `regexpCompile(pattern: string): (RegexpPattern, Error?)`
- `regexpIsMatch(text: string, pattern: string): (bool, Error?)`
- `regexpFindFirst(text: string, pattern: string): (RegexpMatch?, Error?)`
- `regexpFindAll(text: string, pattern: string, maxMatches: int): (array<RegexpMatch>, Error?)`
- `regexpReplaceAll(text: string, pattern: string, replacement: string): (string, Error?)`
- `regexpSplit(text: string, pattern: string, maxParts: int): (array<string>, Error?)`
- `regexpQuoteMeta(text: string): string`

```tblo
import "lib/regexp.tblo";

func main(): void {
    var m = regexpIsMatch("hello123", "^hello\\d+$");
    if (m.1 == nil) println(m.0); // true

    var first = regexpFindFirst("abc123def", "\\d+");
    if (first.1 == nil && first.0 != nil) {
        println((first.0 as RegexpMatch).text); // "123"
    }

    var replaced = regexpReplaceAll("a1 b22 c333", "\\d+", "#");
    if (replaced.1 == nil) println(replaced.0); // "a# b# c#"
}
```

#### Standard library module: `lib/toml.tblo`
TOML helpers for modern config files (tooling/service settings).

Supported subset:
- Key/value entries with comments
- Dotted keys and table headers (`[server]`, `[server.tls]`)
- Strings (`"..."`, `'...'`), integers, booleans, arrays (including nested arrays)
- Quoted key segments (`quoted."dotted.key"`)

It provides:
- `tomlParse(text: string): (map<string, any>, Error?)`
- `tomlStringify(doc: map<string, any>): (string, Error?)`
- `tomlGet(doc: map<string, any>, path: string): (any, Error?)`
- `tomlGetString(doc: map<string, any>, path: string): (string?, Error?)`
- `tomlGetInt(doc: map<string, any>, path: string): (int?, Error?)`
- `tomlGetBool(doc: map<string, any>, path: string): (bool?, Error?)`
- `tomlGetArray(doc: map<string, any>, path: string): (array<any>?, Error?)`
- `tomlGetTable(doc: map<string, any>, path: string): (map<string, any>?, Error?)`

```tblo
import "lib/toml.tblo";

func main(): void {
    var cfg = tomlParse("name=\"tablo\"\n[server]\nport=8080\n");
    if (cfg.1 != nil) return;

    var port = tomlGetInt(cfg.0, "server.port");
    if (port.1 == nil && port.0 != nil) println(port.0); // 8080
}
```

#### Standard library module: `lib/yaml.tblo`
YAML helpers for service/tooling configuration (strict subset).

Supported subset:
- Mappings (`key: value`) and nested mappings by indentation
- Sequences (`- value`) with nested blocks
- Scalars: strings, integers, booleans
- Inline arrays (`[a, b, "c"]`)
- Quoted keys and dotted-path getters

It provides:
- `yamlParse(text: string): (map<string, any>, Error?)`
- `yamlStringify(doc: map<string, any>): (string, Error?)`
- `yamlGet(doc: map<string, any>, path: string): (any, Error?)`
- `yamlGetString(doc: map<string, any>, path: string): (string?, Error?)`
- `yamlGetInt(doc: map<string, any>, path: string): (int?, Error?)`
- `yamlGetBool(doc: map<string, any>, path: string): (bool?, Error?)`
- `yamlGetArray(doc: map<string, any>, path: string): (array<any>?, Error?)`
- `yamlGetTable(doc: map<string, any>, path: string): (map<string, any>?, Error?)`

```tblo
import "lib/yaml.tblo";

func main(): void {
    var cfg = yamlParse("server:\n  port: 8080\n");
    if (cfg.1 != nil) return;

    var port = yamlGetInt(cfg.0, "server.port");
    if (port.1 == nil && port.0 != nil) println(port.0); // 8080
}
```

#### Standard library module: `lib/ini.tblo`
INI helpers for legacy and cross-tooling configuration.

It provides:
- `record IniDocument { global: map<string, string>, sections: map<string, any> }`
- `iniParse(text: string): (IniDocument, Error?)`
- `iniStringify(doc: IniDocument): (string, Error?)`
- `iniGetSection(doc: IniDocument, section: string): (map<string, string>?, Error?)`
- `iniGet(doc: IniDocument, section: string, key: string): (string?, Error?)`
- `iniHas(doc: IniDocument, section: string, key: string): bool`
- `iniGetInt(doc: IniDocument, section: string, key: string): (int?, Error?)`
- `iniGetBool(doc: IniDocument, section: string, key: string): (bool?, Error?)`

```tblo
import "lib/ini.tblo";

func main(): void {
    var doc = iniParse("[server]\nport=8080\n");
    if (doc.1 != nil) return;

    var port = iniGetInt(doc.0, "server", "port");
    if (port.1 == nil && port.0 != nil) println(port.0); // 8080
}
```

#### Standard library module: `lib/msgpack.tblo`
MessagePack helpers for compact binary payloads.

Supported subset:
- `nil`, `bool`
- `int` (up to `int32`/`uint32` range in this subset)
- `string` (UTF-8 encode; ASCII decode in current subset)
- `bytes`
- `array<any>`
- `map<string, any>`

It provides:
- `msgpackEncode(value: any): (bytes, Error?)`
- `msgpackEncodeMap(doc: map<string, any>): (bytes, Error?)`
- `msgpackEncodeArray(items: array<any>): (bytes, Error?)`
- `msgpackEncodeToHex(value: any): (string, Error?)`
- `msgpackDecode(data: bytes): (any, Error?)`
- `msgpackDecodeAsMap(data: bytes): (map<string, any>, Error?)`
- `msgpackDecodeAsArray(data: bytes): (array<any>, Error?)`
- `msgpackDecodeFromHex(hexText: string): (any, Error?)`

```tblo
import "lib/msgpack.tblo";

func main(): void {
    var payload: map<string, any> = { "ok": true, "port": 8080 };
    var encoded = msgpackEncodeMap(payload);
    if (encoded.1 != nil) return;

    var decoded = msgpackDecodeAsMap(encoded.0);
    if (decoded.1 == nil) println(mapGetString(decoded.0, "port")); // 8080
}
```

#### Standard library module: `lib/csv.tblo`
CSV helpers for common data-pipeline/backend tasks.

It provides:
- `csvParseLine(line: string): (array<string>, Error?)`
- `csvStringifyLine(fields: array<string>): (string, Error?)`
- `csvParse(text: string): (array<array<string>>, Error?)`
- `csvStringify(rows: array<array<string>>): (string, Error?)`
- `csvParseWithHeader(text: string): (array<map<string, string>>, Error?)`
- `csvReadAll(path: string): (array<array<string>>, Error?)`
- `csvReadWithHeader(path: string): (array<map<string, string>>, Error?)`
- `csvWriteAll(path: string, rows: array<array<string>>): (bool, Error?)`

Behavior notes:
- Delimiter is comma (`,`)
- Supports quoted fields and escaped quotes (`""`)
- `csvParse` accepts `\n` and `\r\n` line endings
- Multiline quoted fields are currently rejected (`ERR_PARSE`)
- `csvParseWithHeader` validates non-empty unique header names and row column counts
- Parse errors return `ERR_PARSE` with structured context in `Error.data` (`context`, `line`, `column`, `near`)

```tblo
import "lib/csv.tblo";

func main(): void {
    var parsed = csvParseWithHeader("id,name\n1,Alice\n2,Bob");
    if (parsed.1 != nil) return;

    println(mapGetString(parsed.0[0], "name")); // "Alice"

    var out = csvStringify([["id", "name"], ["3", "Charlie"]]);
    if (out.1 == nil) {
        println(out.0);
    }
}
```

#### Standard library module: `lib/config.tblo`
Configuration loading/merging helpers (dotenv + environment + typed reads).

It provides:
- `configParseDotEnv(text: string): (map<string, string>, Error?)`
- `configLoadDotEnv(path: string): (map<string, string>, Error?)`
- `configFromEnv(keys: array<string>): (map<string, string>, Error?)`
- `configMerge(base: map<string, string>, overlay: map<string, string>): map<string, string>`
- `configApplyDotEnv(base: map<string, string>, path: string): (map<string, string>, Error?)`
- `configApplyEnv(base: map<string, string>, keys: array<string>): (map<string, string>, Error?)`
- `configGetString/configGetRequiredString`
- `configGetInt/configGetRequiredInt`
- `configGetBool/configGetRequiredBool`

Behavior notes:
- Dotenv parser accepts `KEY=value`, `export KEY=value`, comments, and quoted values
- Double-quoted values support escapes (`\n`, `\r`, `\t`, `\"`, `\\`)
- Typed getters return `(value, Error?)` for explicit parse/required-key handling

```tblo
import "lib/config.tblo";

func main(): void {
    var base: map<string, string> = { "PORT": "8080" };
    var merged = configApplyDotEnv(base, ".env");
    if (merged.1 != nil) return;

    var port = configGetInt(merged.0, "PORT", 8080);
    if (port.1 == nil) {
        println("port=" + str(port.0));
    }
}
```

#### Standard library module: `lib/cli.tblo`
CLI argument parsing helpers for scripts and command-line apps.

It provides:
- `cliParse(args: array<string>): (CliArgs, Error?)`
- `cliHas(parsed: CliArgs, name: string): bool`
- `cliGet(parsed: CliArgs, name: string, defaultValue: string): string`
- `cliGetRequired(parsed: CliArgs, name: string): (string, Error?)`
- `cliGetInt(parsed: CliArgs, name: string, defaultValue: int): (int, Error?)`
- `cliGetBool(parsed: CliArgs, name: string, defaultValue: bool): (bool, Error?)`
- `cliPositionalAt(parsed: CliArgs, index: int): (string?, Error?)`

Behavior notes:
- Supports long flags: `--name`, `--name=value`, `--name value`
- Supports short flags: `-a`, `-p 8080`, and clusters like `-abc`
- `--` stops flag parsing and treats remaining tokens as positionals

```tblo
import "lib/cli.tblo";

func main(): void {
    var parsed = cliParse(argv);
    if (parsed.1 != nil) return;

    var port = cliGetInt(parsed.0, "port", 8080);
    if (port.1 == nil) {
        println("port=" + str(port.0));
    }
}
```

#### Standard library module: `lib/collections.tblo`
Collection helpers for common array/map transformations.

It provides:
- `collectionsUniqueStrings(items: array<string>): array<string>`
- `collectionsCountStrings(items: array<string>): map<string, int>`
- `collectionsMostFrequentString(items: array<string>): (string?, int)`
- `collectionsChunkStrings(items: array<string>, size: int): (array<array<string>>, Error?)`
- `collectionsIntersectUniqueStrings(left: array<string>, right: array<string>): array<string>`
- `collectionsDifferenceUniqueStrings(left: array<string>, right: array<string>): array<string>`
- `collectionsJoinNonEmpty(items: array<string>, sep: string): string`

Behavior notes:
- Dedup/intersection/difference preserve first-seen order
- `collectionsMostFrequentString` returns `(nil, 0)` for empty input
- `collectionsChunkStrings` returns `ERR_INVALID_ARGUMENT` when `size < 1`

```tblo
import "lib/collections.tblo";

func main(): void {
    var unique = collectionsUniqueStrings(["go", "rust", "go", "tablo"]);
    println(join(unique, ",")); // "go,rust,tablo"

    var counts = collectionsCountStrings(["GET", "POST", "GET"]);
    println(str(mapGetString(counts, "GET"))); // "2"
}
```

#### Standard library module: `lib/time.tblo`
Time utilities for duration parsing/formatting, deadline handling, and date-time formatting.

It provides:
- `timeParseDuration(text: string): (int, Error?)`
- `timeFormatDuration(durationMs: int): string`
- `timeDeadlineAfter(timeoutMs: int): (int, Error?)`
- `timeDeadlineRemaining(deadlineMs: int): int`
- `timeDeadlineExceeded(deadlineMs: int): bool`
- `timeFormatDateTime(parts: array<int>): (string, Error?)`
- `timeNowIsoUtc(): (string, Error?)`
- `timeNowIsoLocal(): (string, Error?)`

Behavior notes:
- Duration parser accepts segments with units: `ms`, `s`, `m`, `h`, `d`
- Combined forms are supported: `1m30s`, `2h 5m 10s 25ms`, `-1s`
- Date-time format uses `YYYY-MM-DDTHH:MM:SS.mmm`
- Deadline helpers are based on `timeMonotonicMillis()` semantics

```tblo
import "lib/time.tblo";

func main(): void {
    var d = timeParseDuration("1m30s");
    if (d.1 == nil) {
        println(str(d.0)); // 90000
    }

    var nowUtc = timeNowIsoUtc();
    if (nowUtc.1 == nil) {
        println(nowUtc.0); // e.g. 2026-02-16T12:34:56.789
    }
}
```

#### Standard library module: `lib/encoding.tblo`
Encoding helpers for hex, Base64, and Base64URL workflows.

It provides:
- `encodingHexEncode(text: string): (string, Error?)`
- `encodingHexEncodeBytes(data: bytes): (string, Error?)`
- `encodingHexDecodeToBytes(hexText: string): (bytes, Error?)`
- `encodingBase64Encode(text: string): (string, Error?)`
- `encodingBase64EncodeBytes(data: bytes): (string, Error?)`
- `encodingBase64DecodeToBytes(input: string): (bytes, Error?)`
- `encodingBase64UrlEncode(text: string, withPadding: bool): (string, Error?)`
- `encodingBase64UrlEncodeBytes(data: bytes, withPadding: bool): (string, Error?)`
- `encodingBase64UrlDecodeToBytes(input: string): (bytes, Error?)`

Behavior notes:
- Base64 decode ignores ASCII whitespace and validates padding placement
- Base64URL decode accepts missing padding (`=`) and normalizes automatically
- Decode APIs return `bytes` to remain binary-safe

```tblo
import "lib/encoding.tblo";

func main(): void {
    var h = encodingHexEncode("Hi");
    if (h.1 == nil) println(h.0); // "4869"

    var b64 = encodingBase64Encode("foo");
    if (b64.1 == nil) println(b64.0); // "Zm9v"

    var raw = encodingBase64UrlDecodeToBytes("__8");
    if (raw.1 == nil) println(bytesToHex(raw.0).0); // "ffff"
}
```

#### Standard library module: `lib/text.tblo`
Text shaping helpers for identifiers, slugs, and presentation strings.

It provides:
- `textToLowerAscii(input: string): string`
- `textToUpperAscii(input: string): string`
- `textNormalizeSpaces(input: string): string`
- `textSplitWordsAscii(input: string): array<string>`
- `textWordCountAscii(input: string): int`
- `textSlugifyAscii(input: string, separator: string): (string, Error?)`
- `textSnakeCaseAscii(input: string): string`
- `textKebabCaseAscii(input: string): string`
- `textCamelCaseAscii(input: string): string`
- `textPascalCaseAscii(input: string): string`
- `textTitleCaseAscii(input: string): string`
- `textTruncateAscii(input: string, maxLen: int): (string, Error?)`

Behavior notes:
- Word splitting handles delimiters and common camel/Pascal/acronym boundaries
- Case helpers are ASCII-oriented; non-ASCII bytes are preserved as-is
- `textSlugifyAscii` returns `ERR_INVALID_ARGUMENT` for invalid separators

```tblo
import "lib/text.tblo";

func main(): void {
    println(textSnakeCaseAscii("HelloWorld API Server")); // "hello_world_api_server"
    println(textCamelCaseAscii("hello world api response")); // "helloWorldApiResponse"

    var slug = textSlugifyAscii("TabloLang: Fast & Simple!", "-");
    if (slug.1 == nil) println(slug.0); // "tablo-fast-simple"
}
```

#### Standard library module: `lib/log.tblo`
Leveled structured logging helpers built on `logJson`.

It provides:
- `record Logger { minLevel, fields }`
- `logDefault(): Logger`
- `logNew(minLevel: string): (Logger, Error?)`
- `logWithMinLevel(logger: Logger, minLevel: string): (Logger, Error?)`
- `logWithField(logger: Logger, key: string, value: any): (Logger, Error?)`
- `logWithFields(logger: Logger, fields: map<string, any>): Logger`
- `logWouldEmit(logger: Logger, level: string): (bool, Error?)`
- `logEmit(logger: Logger, level: string, message: string, fields: map<string, any>): (bool, Error?)`
- `logDebug(logger: Logger, message: string, fields: map<string, any>): (bool, Error?)`
- `logInfo(logger: Logger, message: string, fields: map<string, any>): (bool, Error?)`
- `logWarn(logger: Logger, message: string, fields: map<string, any>): (bool, Error?)`
- `logError(logger: Logger, message: string, fields: map<string, any>): (bool, Error?)`

Behavior notes:
- Accepted levels are `debug`, `info`, `warn`, `error` (case-insensitive).
- Entries below `logger.minLevel` return `(false, nil)` without emitting.
- `logger.fields` and per-call `fields` are merged; per-call fields override on key collisions.

```tblo
import "lib/log.tblo";

func main(): void {
    var logger_r = logNew("info");
    if (logger_r.1 != nil) return;

    var logger = logWithField(logger_r.0, "service", "api").0;
    var _ = logInfo(logger, "startup complete", {"port": 8080});
}
```

#### Standard library module: `lib/template.tblo`
String templating helpers for CLI/backend rendering with explicit placeholder validation.

It provides:
- `templateExtractKeys(templateText: string): (array<string>, Error?)`
- `templateValidate(templateText: string): Error?`
- `templateRender(templateText: string, values: map<string, any>): (string, Error?)`
- `templateRenderWithDefault(templateText: string, values: map<string, any>, missingDefault: string): (string, Error?)`

Behavior notes:
- Placeholders use `{{key}}` syntax; keys are trimmed before lookup.
- Allowed key chars are ASCII letters/digits plus `_`, `.`, `-`.
- `templateRender` is strict: missing/nil keys return `ERR_INVALID_ARGUMENT`.
- `templateRenderWithDefault` substitutes missing/nil keys with `missingDefault`.
- Unmatched delimiters (`{{`/`}}`) return `ERR_PARSE`.

```tblo
import "lib/template.tblo";

func main(): void {
    var rendered = templateRender("Hello {{name}} from {{env}}",
                                  {"name": "Ada", "env": "prod"});
    if (rendered.1 == nil) {
        println(rendered.0); // "Hello Ada from prod"
    }
}
```

#### Standard library module: `lib/testing.tblo`
Assertion helpers for writing TabloLang tests and validation scripts.

It provides:
- `TESTING_ASSERT_FAILED_CODE` (`1001`)
- `fail(message: string): Error`
- `assertTrue(condition: bool, message: string): Error?`
- `assertEq(actual: any, expected: any, message: string): Error?`
- `assertNil(value: any, message: string): Error?`
- `assertErrorCode(err: Error?, expectedCode: int, message: string): Error?`

Behavior notes:
- Assertion failures return `Error` with code `TESTING_ASSERT_FAILED_CODE`.
- `assertEq` compares by type first, then JSON/hex comparable forms.
- `assertEq` supports primitives, arrays/maps/records/tuples/sets, and bytes.
- `assertEq` fails for non-comparable values (for example function values).
- Return-style API works naturally with `?` in `(value, Error?)` flows.

```tblo
import "lib/testing.tblo";

func runCase(): Error? {
    var err = assertEq({"id": 7}, {"id": 7}, "payload should match");
    if (err != nil) return err;

    err = assertTrue(2 + 2 == 4, "math still works");
    if (err != nil) return err;
    return nil;
}
```

#### Standard library module: `lib/validate.tblo`
Validation helpers for common backend payload fields.

It provides:
- `validateEmail(value: string): bool`
- `validateUrlHttp(value: string): bool`
- `validateUuid(value: string): bool`
- `validateRequiredString(value: string, fieldName: string): Error?`
- `validateStringMinLen(value: string, fieldName: string, minLen: int): Error?`
- `validateStringMaxLen(value: string, fieldName: string, maxLen: int): Error?`
- `validateIntRange(value: int, fieldName: string, minValue: int, maxValue: int): Error?`
- `validateRequiredEmail(value: string, fieldName: string): Error?`
- `validateRequiredHttpUrl(value: string, fieldName: string): Error?`
- `validateRequiredUuid(value: string, fieldName: string): Error?`
- `validateRequiredOneOf(value: string, fieldName: string, allowed: array<string>): Error?`
- `validateRequiredKeys(record: map<string, any>, requiredKeys: array<string>, context: string): Error?`

Behavior notes:
- Boolean validators (`validateEmail`, `validateUrlHttp`, `validateUuid`) are fast checks without allocations
- Required-field validators return structured `Error.data` with `context`, `field`, and `detail`
- URL validator accepts `http://` and `https://` with domain/localhost/IPv4 host forms

```tblo
import "lib/validate.tblo";

func main(): void {
    println(validateEmail("ops@tablo.dev")); // true

    var err = validateRequiredHttpUrl("https://api.tblo.dev/v1", "endpoint");
    println(err == nil); // true
}
```

#### Standard library module: `lib/datetime.tblo`
RFC3339-focused date-time parsing/formatting and local/UTC helpers.

It provides:
- `record DateTimeParts { year, month, day, hour, minute, second, millis, offsetMinutes }`
- `datetimeIsLeapYear(year: int): bool`
- `datetimeDaysInMonth(year: int, month: int): (int, Error?)`
- `datetimeParseRfc3339(text: string): (DateTimeParts, Error?)`
- `datetimeFormatRfc3339(parts: DateTimeParts): (string, Error?)`
- `datetimeNowUtc(): (DateTimeParts, Error?)`
- `datetimeNowLocal(): (DateTimeParts, Error?)`
- `datetimeNowUtcRfc3339(): (string, Error?)`
- `datetimeNowLocalRfc3339(): (string, Error?)`

Behavior notes:
- Parser accepts `YYYY-MM-DDTHH:MM:SS[.fraction](Z|Â±HH:MM)` (case-insensitive `Z`)
- Fractional seconds are normalized to milliseconds (1-2 digits padded, 4+ digits truncated)
- `datetimeNowLocal` computes offset minutes by comparing local and UTC builtins

```tblo
import "lib/datetime.tblo";

func main(): void {
    var parsed = datetimeParseRfc3339("2026-02-17T10:20:30.123+02:30");
    if (parsed.1 == nil) {
        println(str(parsed.0.offsetMinutes)); // 150
    }

    var now = datetimeNowUtcRfc3339();
    if (now.1 == nil) println(now.0);
}
```

#### Standard library module: `lib/concurrency.tblo`
Ergonomic wrappers for thread-safe channels, shared cells, worker threads, and arc guards.

It provides:
- `record ConcurrencyChannel { id, schema }`
- `record ConcurrencyShared { id, schema }`
- `record ConcurrencyThread { id, resultSchema }`
- `record ConcurrencyArc { id }`
- `record ConcurrencyGuard { id }`
- `record ConcurrencyMailbox { inboxChannelId, outboxChannelId }`
- `concurrencyChannelOpen(capacity: int): (ConcurrencyChannel, Error?)`
- `concurrencyChannelOpenTyped(capacity: int, schema: any): (ConcurrencyChannel, Error?)`
- `concurrencyChannelSend(channel: ConcurrencyChannel, value: any, timeoutMs: int): Error?`
- `concurrencyChannelRecv(channel: ConcurrencyChannel, timeoutMs: int): (any, Error?)`
- `concurrencyChannelClose(channel: ConcurrencyChannel): (bool, Error?)`
- `concurrencySharedCreate(initial: any): (ConcurrencyShared, Error?)`
- `concurrencySharedCreateTyped(initial: any, schema: any): (ConcurrencyShared, Error?)`
- `concurrencySharedGet(shared: ConcurrencyShared): (any, Error?)`
- `concurrencySharedSet(shared: ConcurrencyShared, value: any): (bool, Error?)`
- `concurrencyThreadSpawn(functionName: string, inboxChannelId: int, outboxChannelId: int): (ConcurrencyThread, Error?)`
- `concurrencyThreadSpawnDetached(functionName: string): (ConcurrencyThread, Error?)`
- `concurrencyThreadSpawnTyped(functionName: string, arg: any, argSchema: any, inboxChannelId: int, outboxChannelId: int, resultSchema: any): (ConcurrencyThread, Error?)`
- `concurrencyThreadJoinDone(thread: ConcurrencyThread, timeoutMs: int): (bool, Error?)`
- `concurrencyThreadJoinValue(thread: ConcurrencyThread, timeoutMs: int): (any, Error?)`
- `concurrencyChannelSendWithContext(channel: ConcurrencyChannel, value: any, ctx: Context): Error?`
- `concurrencyChannelRecvWithContext(channel: ConcurrencyChannel, ctx: Context): (any, Error?)`
- `concurrencyThreadJoinDoneWithContext(thread: ConcurrencyThread, ctx: Context): (bool, Error?)`
- `concurrencyThreadJoinValueWithContext(thread: ConcurrencyThread, ctx: Context): (any, Error?)`
- `concurrencyArcCreate(initial: any): (ConcurrencyArc, Error?)`
- `concurrencyArcClone(arc: ConcurrencyArc): (ConcurrencyArc, Error?)`
- `concurrencyArcAcquire(arc: ConcurrencyArc): (ConcurrencyGuard, Error?)`
- `concurrencyArcRead(guard: ConcurrencyGuard): (any, Error?)`
- `concurrencyArcWrite(guard: ConcurrencyGuard, value: any): (bool, Error?)`
- `concurrencyArcRelease(guard: ConcurrencyGuard): (bool, Error?)`
- `concurrencyMailboxOpen(capacity: int): (ConcurrencyMailbox, Error?)`
- `concurrencyMailboxClose(mailbox: ConcurrencyMailbox): Error?`

Behavior notes:
- Typed channels/shared/thread-joins reuse the same schema validation model as `jsonDecode`.
- Wrappers return contextual `Error.data` payloads (`context`, `detail`, and propagated cause fields).
- Message passing stays explicit and shared-state mutation remains guard-based.

```tblo
import "lib/concurrency.tblo";

func workerMain(): int {
    var arg = syncThreadArgTyped("int");
    if (arg.1 != nil) return -1;
    return (arg.0 as int) + 1;
}

func main(): void {
    var t = concurrencyThreadSpawnTyped("workerMain", 41 as any, "int", -1, -1, "int");
    if (t.1 != nil) return;

    var joined = concurrencyThreadJoinValue(t.0, 5000);
    if (joined.1 == nil) println(joined.0); // 42
}
```

#### Standard library module: `lib/context.tblo`
Cancellation/deadline propagation helpers for structured concurrency and bounded operations.

It provides:
- `record Context { cancelStateId, deadlineMs, parent }`
- `contextBackground(): Context`
- `contextWithCancel(parent: Context): (Context, Error?)`
- `contextWithTimeoutMs(parent: Context, timeoutMs: int): (Context, Error?)`
- `contextWithDeadlineMs(parent: Context, deadlineMs: int): (Context, Error?)`
- `contextCancel(ctx: Context, reason: Error?): Error?`
- `contextDone(ctx: Context): bool`
- `contextErr(ctx: Context): Error?`
- `contextDeadlineMs(ctx: Context): int`
- `contextRemainingMs(ctx: Context): int`
- `contextTimeoutSliceMs(ctx: Context, maxMs: int): int`

Behavior notes:
- Cancellation state is stored in a typed shared cell (`syncShared*`) for cross-thread visibility.
- Child contexts observe parent cancellation/deadline automatically.
- `contextWithTimeoutMs(..., 0)` creates an immediately-expired deadline context.

```tblo
import "lib/context.tblo";

func main(): void {
    var bg: Context = contextBackground();
    var with_cancel = contextWithCancel(bg);
    if (with_cancel.1 != nil) return;

    var ctx: Context = with_cancel.0;
    var _ = contextCancel(ctx, nil);
    println(contextDone(ctx));
}
```

#### Standard library module: `lib/task.tblo`
Structured task-group helpers for thread workers with cancel-on-first-error semantics.

It provides:
- `record TaskGroup { context, threads, results, firstError, joinTimeoutMs }`
- `taskGroupNew(parent: Context): (TaskGroup, Error?)`
- `taskGroupSetJoinTimeoutMs(group: TaskGroup, timeoutMs: int): (TaskGroup, Error?)`
- `taskGroupContext(group: TaskGroup): Context`
- `taskGroupSpawn(group: TaskGroup, functionName: string, arg: any, argSchema: any): (TaskGroup, Error?)`
- `taskGroupWait(group: TaskGroup): (TaskGroup, Error?)`
- `taskGroupFirstError(group: TaskGroup): Error?`
- `taskGroupResults(group: TaskGroup): array<any>`
- Task worker envelope helpers:
- `taskResultOk(value: any): map<string, any>`
- `taskResultErr(code: int, message: string, data: any): map<string, any>`
- `taskResultErrFromError(err: Error): map<string, any>`

Behavior notes:
- Spawned workers use `syncThreadSpawnTyped`.
- Workers should return task envelopes from `taskResultOk` / `taskResultErr`.
- `taskGroupWait` records first failure, cancels group context, then joins all workers.

```tblo
import "lib/task.tblo";

func worker(): map<string, any> {
    var arg = syncThreadArgTyped("int");
    if (arg.1 != nil) return taskResultErr(ERR_PARSE, "bad arg", arg.1 as any);
    return taskResultOk(((arg.0 as int) + 1) as any);
}

func main(): void {
    var g = taskGroupNew(contextBackground());
    if (g.1 != nil) return;

    var s = taskGroupSpawn(g.0, "worker", 41 as any, "int");
    if (s.1 != nil) return;

    var w = taskGroupWait(s.0);
    if (w.1 == nil) println(taskGroupResults(w.0)[0]); // 42
}
```

#### Standard library module: `lib/retry.tblo`
Retry backoff and circuit-breaker helpers for resilient client/server workflows.

It provides:
- `record RetryPolicy { maxAttempts, initialDelayMs, maxDelayMs, multiplierPercent, jitterPercent }`
- `retryDefaultPolicy(): RetryPolicy`
- `retryValidatePolicy(policy: RetryPolicy): Error?`
- `retryCanAttempt(policy: RetryPolicy, attemptNumber: int): bool`
- `retryComputeDelayMs(policy: RetryPolicy, retryNumber: int): (int, Error?)`
- `retryNextDelayMs(policy: RetryPolicy, attemptsMade: int): (int, Error?)`
- `retryIsTransientError(err: Error): bool`
- `retryShouldRetry(policy: RetryPolicy, attemptsMade: int, err: Error?): bool`
- `record RetryCircuit { failureThreshold, cooldownMs, halfOpenSuccessThreshold, state, failureCount, halfOpenSuccesses, openUntilMs }`
- `retryCircuitNew(failureThreshold: int, cooldownMs: int, halfOpenSuccessThreshold: int): (RetryCircuit, Error?)`
- `retryCircuitAllow(circuit: RetryCircuit, nowMs: int): (RetryCircuit, bool)`
- `retryCircuitOnSuccess(circuit: RetryCircuit): RetryCircuit`
- `retryCircuitOnFailure(circuit: RetryCircuit, nowMs: int): RetryCircuit`
- `retryCircuitOpenRemainingMs(circuit: RetryCircuit, nowMs: int): int`

Behavior notes:
- Backoff uses integer exponential growth with clamp-to-`maxDelayMs` and optional jitter.
- `retryShouldRetry` treats `ERR_NETWORK`, `ERR_IO`, and `ERR_LIMIT` as transient.
- Circuit state transitions are explicit (`closed` -> `open` -> `half_open` -> `closed`).

```tblo
import "lib/retry.tblo";

func main(): void {
    var p = retryDefaultPolicy();
    var d = retryComputeDelayMs(p, 3); // ~400ms with default policy
    if (d.1 == nil) println(d.0);

    var c = retryCircuitNew(3, 1000, 2);
    if (c.1 == nil) {
        var allow = retryCircuitAllow(c.0, timeMonotonicMillis());
        println(allow.1);
    }
}
```

#### Standard library module: `lib/cache.tblo`
In-memory cache helpers with TTL expiry, bounded capacity, and typed decode wrappers.

It provides:
- `record CacheStore { entries, maxEntries, defaultTtlMs, evictionPolicy, nextWriteSeq, nextExpiryMs }`
- `cacheNew(maxEntries: int, defaultTtlMs: int): (CacheStore, Error?)`
- `cacheNewWithPolicy(maxEntries: int, defaultTtlMs: int, evictionPolicy: string): (CacheStore, Error?)`
- `cacheSet(store: CacheStore, key: string, value: any): (CacheStore, Error?)`
- `cacheSetWithTtl(store: CacheStore, key: string, value: any, ttlMs: int): (CacheStore, Error?)`
- `cacheSetWithTtlAt(store: CacheStore, key: string, value: any, ttlMs: int, nowMs: int): (CacheStore, Error?)`
- `cacheSetTyped(store: CacheStore, key: string, value: any, schema: any): (CacheStore, Error?)`
- `cacheSetTypedWithTtl(store: CacheStore, key: string, value: any, schema: any, ttlMs: int): (CacheStore, Error?)`
- `cacheSetTypedWithTtlAt(store: CacheStore, key: string, value: any, schema: any, ttlMs: int, nowMs: int): (CacheStore, Error?)`
- `cacheGet(store: CacheStore, key: string): (CacheStore, any, bool)`
- `cacheGetAt(store: CacheStore, key: string, nowMs: int): (CacheStore, any, bool)`
- `cacheGetTyped(store: CacheStore, key: string, schema: any): (CacheStore, any, bool, Error?)`
- `cacheGetTypedAt(store: CacheStore, key: string, schema: any, nowMs: int): (CacheStore, any, bool, Error?)`
- `cacheSetWithContext(store: CacheStore, key: string, value: any, ctx: Context): (CacheStore, Error?)`
- `cacheSetTypedWithContext(store: CacheStore, key: string, value: any, schema: any, ctx: Context): (CacheStore, Error?)`
- `cacheGetWithContext(store: CacheStore, key: string, ctx: Context): (CacheStore, any, bool, Error?)`
- `cacheGetTypedWithContext(store: CacheStore, key: string, schema: any, ctx: Context): (CacheStore, any, bool, Error?)`
- `cachePeek(store: CacheStore, key: string): (CacheStore, any, bool)`
- `cachePeekAt(store: CacheStore, key: string, nowMs: int): (CacheStore, any, bool)`
- `cacheDelete(store: CacheStore, key: string): (CacheStore, bool)`
- `cacheHas(store: CacheStore, key: string): (CacheStore, bool)`
- `cacheCount(store: CacheStore): (CacheStore, int)`
- `cacheKeys(store: CacheStore): (CacheStore, array<string>)`
- `cachePurgeExpired(store: CacheStore): (CacheStore, int)`
- `cachePurgeExpiredAt(store: CacheStore, nowMs: int): (CacheStore, int)`
- `cacheClear(store: CacheStore): CacheStore`

Behavior notes:
- `ttlMs=0` means no expiry; positive TTL entries expire when `nowMs >= expiresAtMs`.
- `lru` policy evicts least recently accessed entries; `fifo` uses insertion/write order.
- Typed set/get helpers validate payloads via the same schema model as `jsonDecode`.

```tblo
import "lib/cache.tblo";

func main(): void {
    var opened = cacheNewWithPolicy(1024, 60000, "lru");
    if (opened.1 != nil) return;

    var cache: CacheStore = opened.0;
    cache = cacheSet(cache, "user:42", {"id": 42, "name": "alice"} as any).0;

    var got = cacheGetTyped(cache, "user:42", {"id": "int", "name": "string"} as any);
    if (got.2 && got.3 == nil) {
        println("cache hit");
    }
}
```

#### Standard library module: `lib/lru.tblo`
Capacity-bounded least-recently-used container for predictable in-memory eviction behavior.

It provides:
- `record LruEntry { key, value, prevKey, nextKey, touchSeq }`
- `record LruStore { entries, capacity, count, lruKey, mruKey, nextTouchSeq, totalSets, totalEvictions }`
- `record LruSetResult { inserted, updated, evicted, evictedKey, evictedValue, count }`
- `record LruStats { capacity, count, lruKey, mruKey, totalSets, totalEvictions }`
- `lruNew(capacity: int): (LruStore, Error?)`
- `lruSet(store: LruStore, key: string, value: any): (LruStore, LruSetResult, Error?)`
- `lruGet(store: LruStore, key: string): (LruStore, any, bool, Error?)`
- `lruPeek(store: LruStore, key: string): (LruStore, any, bool, Error?)`
- `lruDelete(store: LruStore, key: string): (LruStore, bool, Error?)`
- `lruHas(store: LruStore, key: string): (LruStore, bool, Error?)`
- `lruCount(store: LruStore): int`
- `lruKeysLruToMru(store: LruStore): array<string>`
- `lruKeysMruToLru(store: LruStore): array<string>`
- `lruClear(store: LruStore): LruStore`
- `lruStats(store: LruStore): LruStats`

Behavior notes:
- Capacity is strict; inserting a new key at capacity evicts exactly one least-recently-used entry.
- `lruSet` returns eviction metadata (`evictedKey/evictedValue`) when an entry is displaced.
- `lruGet`, `lruPeek`, and `lruHas` all touch matching keys and move them to MRU position.
- `lruKeysLruToMru` / `lruKeysMruToLru` expose deterministic recency snapshots.

```tblo
import "lib/lru.tblo";

func main(): void {
    var opened = lruNew(2);
    if (opened.1 != nil) return;

    var lru: LruStore = opened.0;
    lru = lruSet(lru, "a", 1 as any).0;
    lru = lruSet(lru, "b", 2 as any).0;
    lru = lruGet(lru, "a").0; // touch "a"

    var set_c = lruSet(lru, "c", 3 as any);
    if (set_c.2 == nil && set_c.1.evicted) {
        println("evicted: " + set_c.1.evictedKey);
    }
}
```

#### Standard library module: `lib/rate_limit.tblo`
Rate-limiting helpers with token-bucket and fixed-window algorithms.

It provides:
- `record RateLimitTokenBucket { capacity, refillPerSecond, tokens, lastRefillMs }`
- `rateLimitTokenBucketNew(capacity: int, refillPerSecond: int): (RateLimitTokenBucket, Error?)`
- `rateLimitTokenBucketNewAt(capacity: int, refillPerSecond: int, initialTokens: int, nowMs: int): (RateLimitTokenBucket, Error?)`
- `rateLimitTokenBucketTryConsume(bucket: RateLimitTokenBucket, permits: int): (RateLimitTokenBucket, bool, int, Error?)`
- `rateLimitTokenBucketTryConsumeAt(bucket: RateLimitTokenBucket, permits: int, nowMs: int): (RateLimitTokenBucket, bool, int, Error?)`
- `rateLimitTokenBucketAvailable(bucket: RateLimitTokenBucket): (RateLimitTokenBucket, int)`
- `rateLimitTokenBucketAvailableAt(bucket: RateLimitTokenBucket, nowMs: int): (RateLimitTokenBucket, int)`
- `record RateLimitFixedWindow { limit, windowMs, windowStartMs, used }`
- `rateLimitFixedWindowNew(limit: int, windowMs: int): (RateLimitFixedWindow, Error?)`
- `rateLimitFixedWindowNewAt(limit: int, windowMs: int, nowMs: int): (RateLimitFixedWindow, Error?)`
- `rateLimitFixedWindowTryConsume(window: RateLimitFixedWindow, permits: int): (RateLimitFixedWindow, bool, int, Error?)`
- `rateLimitFixedWindowTryConsumeAt(window: RateLimitFixedWindow, permits: int, nowMs: int): (RateLimitFixedWindow, bool, int, Error?)`
- `rateLimitFixedWindowRemaining(window: RateLimitFixedWindow): (RateLimitFixedWindow, int, int)`
- `rateLimitFixedWindowRemainingAt(window: RateLimitFixedWindow, nowMs: int): (RateLimitFixedWindow, int, int)`

Behavior notes:
- Token-bucket refill uses integer math (`elapsedMs * refillPerSecond / 1000`) and returns `retryAfterMs` when denied.
- Fixed-window counters align to window boundaries and return remaining/reset information.
- `...At` variants are deterministic and ideal for tests/simulation; non-`At` variants use `timeMonotonicMillis()`.

```tblo
import "lib/rate_limit.tblo";

func main(): void {
    var bucket_r = rateLimitTokenBucketNew(20, 10);
    if (bucket_r.1 != nil) return;

    var consume = rateLimitTokenBucketTryConsume(bucket_r.0, 5);
    if (!consume.1) {
        println("retry after ms: " + str(consume.2));
    }
}
```

#### Standard library module: `lib/pool.tblo`
Generic object-pool helpers with explicit lease/release lifecycle.

It provides:
- `record PoolLease { id, value }`
- `record PoolStore { idle, inUse, maxIdle, maxTotal, createdCount, acquiredCount, releasedCount, droppedCount, nextLeaseId }`
- `record PoolStats { idleCount, inUseCount, totalCount, maxIdle, maxTotal, createdCount, acquiredCount, releasedCount, droppedCount }`
- `poolNew(maxIdle: int, maxTotal: int): (PoolStore, Error?)`
- `poolNewWithSeed(maxIdle: int, maxTotal: int, seedIdle: array<any>): (PoolStore, Error?)`
- `poolAcquire(store: PoolStore, fallbackValue: any): (PoolStore, PoolLease, bool, Error?)`
- `poolAcquireTyped(store: PoolStore, fallbackValue: any, schema: any): (PoolStore, PoolLease, bool, Error?)`
- `poolRelease(store: PoolStore, lease: PoolLease, returnedValue: any): (PoolStore, bool, Error?)`
- `poolReleaseTyped(store: PoolStore, lease: PoolLease, returnedValue: any, schema: any): (PoolStore, bool, Error?)`
- `poolDiscard(store: PoolStore, lease: PoolLease): (PoolStore, Error?)`
- `poolIdleCount(store: PoolStore): int`
- `poolInUseCount(store: PoolStore): int`
- `poolTotalCount(store: PoolStore): int`
- `poolStats(store: PoolStore): PoolStats`
- `poolClearIdle(store: PoolStore): (PoolStore, int)`
- `poolBorrowedLeaseIds(store: PoolStore): array<int>`

Behavior notes:
- Pool capacity is bounded by `maxTotal`; `maxIdle` controls how many released values are retained.
- `poolAcquire` returns `reused=true` when a value comes from idle storage.
- Typed wrappers validate values via the `jsonDecode` schema model.

```tblo
import "lib/pool.tblo";

func main(): void {
    var opened = poolNew(64, 256);
    if (opened.1 != nil) return;

    var store: PoolStore = opened.0;
    var acquired = poolAcquire(store, {"buf": ""} as any);
    if (acquired.3 != nil) return;

    store = acquired.0;
    var lease: PoolLease = acquired.1;

    var released = poolRelease(store, lease, {"buf": "reused"} as any);
    if (released.2 == nil) {
        println("pooled: " + str(released.1));
    }
}
```

#### Standard library module: `lib/queue.tblo`
Queue/deque helpers with optional bounded capacity and typed wrappers.

It provides:
- `record QueueStore { slots, head, tail, size, maxSize, droppedCount }`
- `record QueueStats { size, maxSize, droppedCount, head, tail }`
- `queueNew(maxSize: int): (QueueStore, Error?)`
- `queueNewWithSeed(maxSize: int, seedItems: array<any>): (QueueStore, Error?)`
- `queueEnqueue(store: QueueStore, value: any): (QueueStore, Error?)`
- `queuePushBack(store: QueueStore, value: any): (QueueStore, Error?)`
- `queueTryEnqueue(store: QueueStore, value: any): (QueueStore, bool)`
- `queueEnqueueDropOldest(store: QueueStore, value: any): (QueueStore, bool)`
- `queuePushFront(store: QueueStore, value: any): (QueueStore, Error?)`
- `queueDequeue(store: QueueStore): (QueueStore, any, bool)`
- `queuePopFront(store: QueueStore): (QueueStore, any, bool)`
- `queuePopBack(store: QueueStore): (QueueStore, any, bool)`
- `queuePeekFront(store: QueueStore): (any, bool)`
- `queuePeekBack(store: QueueStore): (any, bool)`
- `queueEnqueueTyped(store: QueueStore, value: any, schema: any): (QueueStore, Error?)`
- `queuePushFrontTyped(store: QueueStore, value: any, schema: any): (QueueStore, Error?)`
- `queueDequeueTyped(store: QueueStore, schema: any): (QueueStore, any, bool, Error?)`
- `queueLen(store: QueueStore): int`
- `queueIsEmpty(store: QueueStore): bool`
- `queueClear(store: QueueStore): (QueueStore, int)`
- `queueStats(store: QueueStore): QueueStats`
- `queueToArray(store: QueueStore): array<any>`

Behavior notes:
- `maxSize=0` means unbounded; otherwise operations that exceed capacity return `ERR_LIMIT`.
- `queueTryEnqueue` increments `droppedCount` on rejection.
- `queueEnqueueDropOldest` keeps the newest value and evicts front when bounded/full.
- Typed wrappers validate payloads with `jsonDecode` schema rules.

```tblo
import "lib/queue.tblo";

func main(): void {
    var opened = queueNew(1024);
    if (opened.1 != nil) return;

    var store: QueueStore = opened.0;
    var en = queueEnqueue(store, {"id": 1} as any);
    if (en.1 != nil) return;
    store = en.0;

    var de = queueDequeue(store);
    if (de.2) {
        println("dequeued id: " + str((mapGetString(de.1 as map<string, any>, "id") as int)));
    }
}
```

#### Standard library module: `lib/heap.tblo`
Priority queue helpers backed by a binary min-heap.

It provides:
- `record HeapEntry { priority, value }`
- `record HeapStore { items, maxSize, pushedCount, poppedCount, droppedCount }`
- `record HeapStats { size, maxSize, pushedCount, poppedCount, droppedCount, minPriority }`
- `heapNew(maxSize: int): (HeapStore, Error?)`
- `heapNewWithSeed(maxSize: int, seedItems: array<HeapEntry>): (HeapStore, Error?)`
- `heapPush(store: HeapStore, priority: int, value: any): (HeapStore, Error?)`
- `heapTryPush(store: HeapStore, priority: int, value: any): (HeapStore, bool)`
- `heapPushKeepHighest(store: HeapStore, priority: int, value: any): (HeapStore, bool)`
- `heapPushTyped(store: HeapStore, priority: int, value: any, schema: any): (HeapStore, Error?)`
- `heapPeek(store: HeapStore): (HeapEntry, bool)`
- `heapPeekPriority(store: HeapStore): (int, bool)`
- `heapPop(store: HeapStore): (HeapStore, HeapEntry, bool)`
- `heapPopTyped(store: HeapStore, schema: any): (HeapStore, any, int, bool, Error?)`
- `heapLen(store: HeapStore): int`
- `heapIsEmpty(store: HeapStore): bool`
- `heapClear(store: HeapStore): (HeapStore, int)`
- `heapStats(store: HeapStore): HeapStats`
- `heapItems(store: HeapStore): array<HeapEntry>`

Behavior notes:
- The heap is a min-heap ordered by `priority` (`heapPop` returns the smallest priority first).
- `maxSize=0` means unbounded; bounded `heapPush` returns `ERR_LIMIT` when full.
- `heapPushKeepHighest` is useful for top-K tracking: it keeps larger priorities and drops lower ones.
- Typed wrappers validate payloads through `jsonDecode`.

```tblo
import "lib/heap.tblo";

func main(): void {
    var opened = heapNew(128);
    if (opened.1 != nil) return;

    var store: HeapStore = opened.0;
    store = heapPush(store, 10, {"id": 1} as any).0;
    store = heapPush(store, 2, {"id": 2} as any).0;

    var popped = heapPop(store);
    if (popped.2) {
        println("priority: " + str(popped.1.priority));
    }
}
```

#### Standard library module: `lib/graph.tblo`
Directed graph helpers for traversal and DAG scheduling.

It provides:
- `record GraphStore { adjacency, edgeCount }`
- `record GraphStats { nodeCount, edgeCount, isolatedCount }`
- `graphNew(): GraphStore`
- `graphNodeCount(store: GraphStore): int`
- `graphHasNode(store: GraphStore, node: string): bool`
- `graphNodeNames(store: GraphStore): array<string>`
- `graphAddNode(store: GraphStore, node: string): (GraphStore, bool, Error?)`
- `graphAddEdge(store: GraphStore, fromNode: string, toNode: string): (GraphStore, bool, Error?)`
- `graphRemoveEdge(store: GraphStore, fromNode: string, toNode: string): (GraphStore, bool, Error?)`
- `graphRemoveNode(store: GraphStore, node: string): (GraphStore, bool, Error?)`
- `graphHasEdge(store: GraphStore, fromNode: string, toNode: string): (bool, Error?)`
- `graphNeighbors(store: GraphStore, node: string): (array<string>, Error?)`
- `graphOutDegree(store: GraphStore, node: string): (int, Error?)`
- `graphInDegree(store: GraphStore, node: string): (int, Error?)`
- `graphBfsOrder(store: GraphStore, startNode: string, maxNodes: int): (array<string>, Error?)`
- `graphShortestPath(store: GraphStore, startNode: string, goalNode: string, maxNodes: int): (array<string>, bool, Error?)`
- `graphTopologicalSort(store: GraphStore, maxNodes: int): (array<string>, bool, Error?)`
- `graphStats(store: GraphStore): GraphStats`

Behavior notes:
- Edges are directed; adding an edge auto-creates missing endpoint nodes.
- `graphShortestPath` uses unweighted BFS and returns `(path, found, err)`.
- `graphTopologicalSort` returns `isAcyclic=false` when cycles are present.
- `maxNodes` bounds traversal work and can return `ERR_LIMIT` for safety.

```tblo
import "lib/graph.tblo";

func main(): void {
    var g: GraphStore = graphNew();
    g = graphAddEdge(g, "compile", "test").0;
    g = graphAddEdge(g, "test", "deploy").0;

    var topo = graphTopologicalSort(g, 100);
    if (topo.2 == nil && topo.1) {
        println("steps: " + str(len(topo.0)));
    }
}
```

#### Standard library module: `lib/mathx.tblo`
Numeric helpers for analytics workloads and data processing pipelines.

It provides:
- `record MathxSummary { count, sum, min, max, mean }`
- `record MathxVariance { population, sample }`
- `record MathxRollingWindow { values, maxSize, nextIndex, count, sum }`
- `record MathxRollingStats { count, maxSize, sum, mean, min, max }`
- `mathxSum(items: array<double>): double`
- `mathxMin(items: array<double>): (double, Error?)`
- `mathxMax(items: array<double>): (double, Error?)`
- `mathxMean(items: array<double>): (double, Error?)`
- `mathxSummary(items: array<double>): (MathxSummary, Error?)`
- `mathxVariance(items: array<double>): (MathxVariance, Error?)`
- `mathxStdDev(items: array<double>): (MathxVariance, Error?)`
- `mathxPercentile(items: array<double>, percentile: double): (double, Error?)`
- `mathxMedian(items: array<double>): (double, Error?)`
- `mathxHistogram(items: array<double>, bucketCount: int, minValue: double, maxValue: double): (array<int>, Error?)`
- `mathxHistogramAuto(items: array<double>, bucketCount: int): (array<int>, double, double, Error?)`
- `mathxRollingNew(maxSize: int): (MathxRollingWindow, Error?)`
- `mathxRollingPush(window: MathxRollingWindow, value: double): MathxRollingWindow`
- `mathxRollingCount(window: MathxRollingWindow): int`
- `mathxRollingSnapshot(window: MathxRollingWindow): array<double>`
- `mathxRollingMean(window: MathxRollingWindow): (double, Error?)`
- `mathxRollingMinMax(window: MathxRollingWindow): (double, double, Error?)`
- `mathxRollingStats(window: MathxRollingWindow): (MathxRollingStats, Error?)`

Behavior notes:
- Percentiles use sorted linear interpolation between neighboring ranks.
- Histogram ranges are clamped: below `minValue` goes to first bucket, above `maxValue` to last.
- Rolling windows are fixed-size ring buffers with `O(1)` push and mean updates.
- Functions that require data return `ERR_INVALID_ARGUMENT` for empty inputs.

```tblo
import "lib/mathx.tblo";

func main(): void {
    var samples: array<double> = [10.0, 12.0, 8.0, 15.0, 11.0];
    var p95 = mathxPercentile(samples, 95.0);
    if (p95.1 == nil) {
        println("p95: " + str(p95.0));
    }
}
```

#### Standard library module: `lib/bitset.tblo`
Dense bitset helpers for membership checks, rank/select queries, and set algebra.

It provides:
- `record BitsetStore { words, bitLength, setCount }`
- `record BitsetStats { bitLength, wordCount, setCount, densityPermille }`
- `bitsetNew(bitLength: int): (BitsetStore, Error?)`
- `bitsetFromIndices(bitLength: int, indices: array<int>): (BitsetStore, Error?)`
- `bitsetClone(store: BitsetStore): BitsetStore`
- `bitsetLen(store: BitsetStore): int`
- `bitsetWordCount(store: BitsetStore): int`
- `bitsetCount(store: BitsetStore): int`
- `bitsetIsEmpty(store: BitsetStore): bool`
- `bitsetHas(store: BitsetStore, index: int): (bool, Error?)`
- `bitsetSet(store: BitsetStore, index: int): (BitsetStore, bool, Error?)`
- `bitsetClear(store: BitsetStore, index: int): (BitsetStore, bool, Error?)`
- `bitsetToggle(store: BitsetStore, index: int): (BitsetStore, bool, Error?)`
- `bitsetSetRange(store: BitsetStore, startIndex: int, endIndexExclusive: int): (BitsetStore, int, Error?)`
- `bitsetClearRange(store: BitsetStore, startIndex: int, endIndexExclusive: int): (BitsetStore, int, Error?)`
- `bitsetSetMany(store: BitsetStore, indices: array<int>): (BitsetStore, int, Error?)`
- `bitsetClearMany(store: BitsetStore, indices: array<int>): (BitsetStore, int, Error?)`
- `bitsetClearAll(store: BitsetStore): (BitsetStore, int)`
- `bitsetUnion(left: BitsetStore, right: BitsetStore): (BitsetStore, Error?)`
- `bitsetIntersect(left: BitsetStore, right: BitsetStore): (BitsetStore, Error?)`
- `bitsetDifference(left: BitsetStore, right: BitsetStore): (BitsetStore, Error?)`
- `bitsetXor(left: BitsetStore, right: BitsetStore): (BitsetStore, Error?)`
- `bitsetComplement(store: BitsetStore): BitsetStore`
- `bitsetEquals(left: BitsetStore, right: BitsetStore): (bool, Error?)`
- `bitsetRank(store: BitsetStore, indexExclusive: int): (int, Error?)`
- `bitsetSelect(store: BitsetStore, rankIndex: int): (int, bool, Error?)`
- `bitsetToIndices(store: BitsetStore, maxItems: int): (array<int>, bool, Error?)`
- `bitsetStats(store: BitsetStore): BitsetStats`

Behavior notes:
- Internal storage uses 30-bit words to keep arithmetic and bitwise operations stable on signed ints.
- `bitsetRank` returns the number of set bits in `[0, indexExclusive)`.
- `bitsetSelect` resolves the 0-based rank of set bits (`found=false` when rank is out of range).
- Set algebra helpers require equal `bitLength` and return `ERR_INVALID_ARGUMENT` on mismatch.

```tblo
import "lib/bitset.tblo";

func main(): void {
    var opened = bitsetNew(128);
    if (opened.1 != nil) return;

    var bs: BitsetStore = opened.0;
    bs = bitsetSet(bs, 5).0;
    bs = bitsetSet(bs, 42).0;

    var rank = bitsetRank(bs, 50);
    if (rank.1 == nil) {
        println("bits before 50: " + str(rank.0));
    }
}
```

#### Standard library module: `lib/bloom.tblo`
Bloom-filter helpers for fast probabilistic membership checks at low memory cost.

It provides:
- `record BloomStore { bits, bitLength, hashCount, itemCount }`
- `record BloomStats { bitLength, hashCount, itemCount, setBitCount, fillPermille, approxFalsePositivePermille }`
- `bloomNew(bitLength: int, hashCount: int): (BloomStore, Error?)`
- `bloomMaybeHas(store: BloomStore, value: any): (bool, Error?)`
- `bloomAdd(store: BloomStore, value: any): (BloomStore, bool, Error?)`
- `bloomAddMany(store: BloomStore, entries: array<any>): (BloomStore, int, Error?)`
- `bloomClear(store: BloomStore): (BloomStore, int)`
- `bloomUnion(left: BloomStore, right: BloomStore): (BloomStore, Error?)`
- `bloomSetBitCount(store: BloomStore): int`
- `bloomSetBitIndices(store: BloomStore, maxItems: int): (array<int>, bool, Error?)`
- `bloomApproxFalsePositivePermille(store: BloomStore): int`
- `bloomStats(store: BloomStore): BloomStats`

Behavior notes:
- Membership is probabilistic: `false` means definitely absent, `true` means maybe present.
- `bloomAdd` returns `maybeNew=true` when at least one backing bit changed.
- `bloomUnion` requires matching `bitLength/hashCount` across both filters.
- The false-positive estimate is derived from current fill ratio (`setBitCount / bitLength`).

```tblo
import "lib/bloom.tblo";

func main(): void {
    var opened = bloomNew(4096, 5);
    if (opened.1 != nil) return;

    var bf: BloomStore = opened.0;
    bf = bloomAdd(bf, "alice@example.com" as any).0;

    var q = bloomMaybeHas(bf, "alice@example.com" as any);
    if (q.1 == nil) println("maybe present: " + str(q.0));
}
```

#### Standard library module: `lib/random.tblo`
High-level random helpers for tokens, byte buffers, shuffling, and sampling.

It provides:
- `randomPseudoBytes(byteCount: int): (bytes, Error?)`
- `randomSecureBytes(byteCount: int): (bytes, Error?)`
- `randomTokenHex(byteCount: int): (string, Error?)`
- `randomTokenHexPseudo(byteCount: int): (string, Error?)`
- `randomShuffleInts(items: array<int>): array<int>`
- `randomShuffleStrings(items: array<string>): array<string>`
- `randomPickInt(items: array<int>): (int, bool)`
- `randomPickString(items: array<string>): (string?, bool)`
- `randomSampleDistinctInts(items: array<int>, count: int): (array<int>, Error?)`
- `randomSampleDistinctStrings(items: array<string>, count: int): (array<string>, Error?)`

```tblo
import "lib/random.tblo";

func main(): void {
    var token = randomTokenHex(16);
    if (token.1 != nil) {
        println("token generation failed: " + token.1.message);
        return;
    }

    var shuffled = randomShuffleInts([1, 2, 3, 4, 5, 6]);
    println("token hex chars: " + str(len(token.0)));
    println("first shuffled value: " + str(shuffled[0]));
}
```

#### Standard library module: `lib/rsa.tblo`
RSA keygen/encrypt/sign helpers built on TabloLang bigint and random primitives.

It provides:
- Key records: `RSAPublicKey`, `RSAPrivateKey`, `RSAKeyPair`
- Keygen: `rsaGenerateKeyPair(keyBits)`, `rsaGenerateKeyPairWithExponent(keyBits, e)`
- Encrypt/decrypt: `rsaEncrypt(message, publicKey)`, `rsaDecrypt(ciphertext, privateKey)`
- Sign/verify: `rsaSign(messageHash, privateKey)`, `rsaVerify(messageHash, signature, publicKey)`
- Key utilities: `rsaValidateKeyPair`, `rsaExportPublicKey`, `rsaExportPrivateKey`, `rsaCreateKeyPair`
- Optional helpers: `rsaPad`, `rsaUnpad`

Security notes:
- `rsaPad`/`rsaUnpad` are simplified helpers, not full PKCS#1 OAEP/PSS implementations.
- `rsaRandomBigIntBits` falls back to pseudo-random generation if secure random is unavailable.
- Treat `lib/rsa.tblo` as educational/prototyping crypto unless you add audited padding and key-handling policies.

```tblo
import "lib/rsa.tblo";

func main(): void {
    var keys = rsaGenerateKeyPair(512);
    if (rsaValidateKeyPair(keys) != 1) return;

    var messageHash: bigint = 123456789n;
    var signature: bigint = rsaSign(messageHash, keys.private);
    println(str(rsaVerify(messageHash, signature, keys.public))); // 1
}
```

#### Standard library module: `lib/hash.tblo`
Stable non-cryptographic hash and checksum helpers for cache keys and content fingerprints.

It provides:
- `hashStableBytes(data: bytes): int`
- `hashStableString(text: string): (int, Error?)`
- `hashStableHexString(text: string): (string, Error?)`
- `hashAdler32Bytes(data: bytes): int`
- `hashAdler32String(text: string): (int, Error?)`
- `hashAdler32HexString(text: string): (string, Error?)`
- `hashCombine(seed: int, nextHash: int): int`
- `hashFingerprintStrings(items: array<string>): (int, Error?)`
- `hashFingerprintHexStrings(items: array<string>): (string, Error?)`
- `hashHex32(value: int): string`

```tblo
import "lib/hash.tblo";

func main(): void {
    var stable = hashStableHexString("tenant=acme|user=42");
    if (stable.1 != nil) return;

    var fp = hashFingerprintHexStrings([
        "name=svc-api",
        "region=us-east-1",
        "version=2026.02.19"
    ]);
    if (fp.1 != nil) return;

    println(stable.0);
    println(fp.0);
}
```

#### Standard library module: `lib/crypto.tblo`
Cryptographic digest, MAC, KDF, verification, and authenticated-encryption helpers built on the native crypto primitives.

It provides:
- `cryptoSha256Bytes(data: bytes): (bytes, Error?)`
- `cryptoSha256Hex(data: bytes): (string, Error?)`
- `cryptoSha256Text(text: string): (bytes, Error?)`
- `cryptoSha256TextHex(text: string): (string, Error?)`
- `cryptoHmacSha256Bytes(key: bytes, data: bytes): (bytes, Error?)`
- `cryptoHmacSha256Hex(key: bytes, data: bytes): (string, Error?)`
- `cryptoHmacSha256Text(keyText: string, dataText: string): (bytes, Error?)`
- `cryptoHmacSha256TextHex(keyText: string, dataText: string): (string, Error?)`
- `cryptoHmacSha256VerifyBytes(key: bytes, data: bytes, expectedTag: bytes): (bool, Error?)`
- `cryptoHmacSha256VerifyHex(key: bytes, data: bytes, expectedHex: string): (bool, Error?)`
- `cryptoHmacSha256VerifyTextHex(keyText: string, dataText: string, expectedHex: string): (bool, Error?)`
- `cryptoPbkdf2HmacSha256Bytes(password: bytes, salt: bytes, iterations: int, derivedKeyBytes: int): (bytes, Error?)`
- `cryptoPbkdf2HmacSha256Hex(password: bytes, salt: bytes, iterations: int, derivedKeyBytes: int): (string, Error?)`
- `cryptoPbkdf2HmacSha256Text(password: string, salt: string, iterations: int, derivedKeyBytes: int): (bytes, Error?)`
- `cryptoPbkdf2HmacSha256TextHex(password: string, salt: string, iterations: int, derivedKeyBytes: int): (string, Error?)`
- `cryptoHkdfHmacSha256Bytes(ikm: bytes, salt: bytes, info: bytes, derivedKeyBytes: int): (bytes, Error?)`
- `cryptoHkdfHmacSha256Hex(ikm: bytes, salt: bytes, info: bytes, derivedKeyBytes: int): (string, Error?)`
- `cryptoHkdfHmacSha256Text(ikm: string, salt: string, info: string, derivedKeyBytes: int): (bytes, Error?)`
- `cryptoHkdfHmacSha256TextHex(ikm: string, salt: string, info: string, derivedKeyBytes: int): (string, Error?)`
- `cryptoConstantTimeBytesEqual(left: bytes, right: bytes): (bool, Error?)`
- `cryptoConstantTimeTextEqual(left: string, right: string): (bool, Error?)`
- `cryptoAesCtrTransform(key: bytes, counter: bytes, input: bytes): (bytes, Error?)`
- `cryptoAesCtrEncrypt(key: bytes, counter: bytes, plaintext: bytes): (bytes, Error?)`
- `cryptoAesCtrDecrypt(key: bytes, counter: bytes, ciphertext: bytes): (bytes, Error?)`
- `cryptoAesCtrEncryptText(key: bytes, counter: bytes, plaintext: string): (bytes, Error?)`
- `cryptoAesCtrEncryptTextHex(key: bytes, counter: bytes, plaintext: string): (string, Error?)`
- `cryptoAesGcmSeal(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes): (bytes, Error?)`
- `cryptoAesGcmOpen(key: bytes, nonce: bytes, sealed: bytes, aad: bytes): (bytes, Error?)`
- `cryptoAesGcmSealDetached(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes): (bytes, bytes, Error?)`
- `cryptoAesGcmOpenDetached(key: bytes, nonce: bytes, ciphertext: bytes, tag: bytes, aad: bytes): (bytes, Error?)`
- `cryptoAesGcmSealDetachedHex(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes): (string, string, Error?)`
- `cryptoAesGcmSealText(key: bytes, nonce: bytes, plaintext: string, aad: bytes): (bytes, Error?)`
- `cryptoAesGcmSealTextHex(key: bytes, nonce: bytes, plaintext: string, aad: bytes): (string, Error?)`
- `cryptoAesGcmSealBase64(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes): (string, Error?)`
- `cryptoAesGcmOpenBase64(key: bytes, nonce: bytes, sealedBase64: string, aad: bytes): (bytes, Error?)`
- `cryptoAesGcmSealTextBase64(key: bytes, nonce: bytes, plaintext: string, aad: bytes): (string, Error?)`
- `cryptoAesGcmSealBase64Url(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes, withPadding: bool): (string, Error?)`
- `cryptoAesGcmOpenBase64Url(key: bytes, nonce: bytes, sealedBase64Url: string, aad: bytes): (bytes, Error?)`
- `cryptoAesGcmSealTextBase64Url(key: bytes, nonce: bytes, plaintext: string, aad: bytes, withPadding: bool): (string, Error?)`
- `record CryptoPasswordAesGcmEnvelope { version, iterations, salt, nonce, sealed }`
- `CRYPTO_PASSWORD_AES_GCM_DEFAULT_ITERATIONS: int`
- `cryptoPasswordAesGcmSeal(password: string, plaintext: bytes, aad: bytes, iterations: int): (CryptoPasswordAesGcmEnvelope, Error?)`
- `cryptoPasswordAesGcmSealDefault(password: string, plaintext: bytes, aad: bytes): (CryptoPasswordAesGcmEnvelope, Error?)`
- `cryptoPasswordAesGcmOpen(password: string, envelope: CryptoPasswordAesGcmEnvelope, aad: bytes): (bytes, Error?)`
- `cryptoPasswordAesGcmEncodeEnvelope(envelope: CryptoPasswordAesGcmEnvelope, withPadding: bool): (string, Error?)`
- `cryptoPasswordAesGcmDecodeEnvelope(encoded: string): (CryptoPasswordAesGcmEnvelope, Error?)`
- `cryptoPasswordAesGcmSealBase64Url(password: string, plaintext: bytes, aad: bytes, iterations: int, withPadding: bool): (string, Error?)`
- `cryptoPasswordAesGcmSealBase64UrlDefault(password: string, plaintext: bytes, aad: bytes): (string, Error?)`
- `cryptoPasswordAesGcmOpenBase64Url(password: string, encoded: string, aad: bytes): (bytes, Error?)`
- `cryptoPasswordAesGcmSealTextBase64Url(password: string, plaintext: string, aad: bytes, iterations: int, withPadding: bool): (string, Error?)`
- `cryptoPasswordAesGcmSealTextBase64UrlDefault(password: string, plaintext: string, aad: bytes): (string, Error?)`
- `record CryptoPasswordHash { scheme, version, iterations, salt, digest }`
- `CRYPTO_PASSWORD_HASH_DEFAULT_ITERATIONS: int`
- `cryptoPasswordHashCreate(password: string, iterations: int): (CryptoPasswordHash, Error?)`
- `cryptoPasswordHashCreateDefault(password: string): (CryptoPasswordHash, Error?)`
- `cryptoPasswordHashVerify(password: string, hash: CryptoPasswordHash): (bool, Error?)`
- `cryptoPasswordHashNeedsRehash(hash: CryptoPasswordHash, minimumIterations: int): (bool, Error?)`
- `cryptoPasswordHashEncode(hash: CryptoPasswordHash, withPadding: bool): (string, Error?)`
- `cryptoPasswordHashDecode(encoded: string): (CryptoPasswordHash, Error?)`
- `cryptoPasswordHashCreateBase64Url(password: string, iterations: int, withPadding: bool): (string, Error?)`
- `cryptoPasswordHashCreateBase64UrlDefault(password: string): (string, Error?)`
- `cryptoPasswordHashVerifyBase64Url(password: string, encoded: string): (bool, Error?)`
- `cryptoPasswordHashVerifyAndUpgrade(password: string, hash: CryptoPasswordHash, minimumIterations: int): (bool, bool, CryptoPasswordHash, Error?)`
- `cryptoPasswordHashVerifyAndUpgradeDefault(password: string, hash: CryptoPasswordHash): (bool, bool, CryptoPasswordHash, Error?)`
- `cryptoPasswordHashVerifyAndUpgradeBase64Url(password: string, encoded: string, minimumIterations: int, withPadding: bool): (bool, bool, string, Error?)`
- `cryptoPasswordHashVerifyAndUpgradeBase64UrlDefault(password: string, encoded: string): (bool, bool, string, Error?)`
- `record CryptoSignedTokenEnvelope { version, issuedAtMillis, expiresAtMillis, payload }`
- `cryptoSignedTokenSeal(secret: bytes, payload: bytes, issuedAtMillis: int, expiresAtMillis: int): (string, Error?)`
- `cryptoSignedTokenSealWithTtl(secret: bytes, payload: bytes, ttlMs: int): (string, Error?)`
- `cryptoSignedTokenSealText(secretText: string, payloadText: string, issuedAtMillis: int, expiresAtMillis: int): (string, Error?)`
- `cryptoSignedTokenSealTextWithTtl(secretText: string, payloadText: string, ttlMs: int): (string, Error?)`
- `cryptoSignedTokenDecode(token: string): (CryptoSignedTokenEnvelope, Error?)`
- `cryptoSignedTokenVerify(secret: bytes, token: string, nowMillis: int): (bool, bool, CryptoSignedTokenEnvelope, Error?)`
- `cryptoSignedTokenVerifyNow(secret: bytes, token: string): (bool, bool, CryptoSignedTokenEnvelope, Error?)`
- `cryptoSignedTokenVerifyText(secretText: string, token: string, nowMillis: int): (bool, bool, CryptoSignedTokenEnvelope, Error?)`
- `cryptoSignedTokenVerifyTextNow(secretText: string, token: string): (bool, bool, CryptoSignedTokenEnvelope, Error?)`
- `record CryptoSignedJsonClaimsEnvelope { version, issuedAtMillis, expiresAtMillis, keyId, claims }`
- `record CryptoSignedJsonClaimsValidationPolicy { issuer, audience, requireIssuer, requireAudience, requireNotBefore, requireJti, clockSkewMillis }`
- `record CryptoSignedJsonClaimsRevocationStore { entries, nextExpiryMillis }`
- `record CryptoSessionTokenConfig { issuer, audience, keyId, ttlMillis, clockSkewMillis, includeNotBefore }`
- `record CryptoSessionTokenIssueResult { token, jwtId, subject, issuedAtMillis, expiresAtMillis, keyId }`
- `record CryptoSessionTokenVerifyResult { ok, expired, revoked, keyId, jwtId, subject, sessionId, tokenKind, claims, revocations }`
- `record CryptoSessionTokenPairConfig { access, refresh }`
- `record CryptoSessionTokenPairIssueResult { sessionId, access, refresh }`
- `record CryptoSessionTokenPairRotateResult { ok, expired, revoked, sessionId, previousRefreshJwtId, pair, revocations }`
- `cryptoSignedJsonClaimsNew(): map<string, any>`
- `cryptoSignedJsonClaimsSetSubject(claims: map<string, any>, subject: string): (map<string, any>, Error?)`
- `cryptoSignedJsonClaimsSetIssuer(claims: map<string, any>, issuer: string): (map<string, any>, Error?)`
- `cryptoSignedJsonClaimsSetAudience(claims: map<string, any>, audience: string): (map<string, any>, Error?)`
- `cryptoSignedJsonClaimsSetAudiences(claims: map<string, any>, audiences: array<string>): (map<string, any>, Error?)`
- `cryptoSignedJsonClaimsSetNotBefore(claims: map<string, any>, notBeforeMillis: int): (map<string, any>, Error?)`
- `cryptoSignedJsonClaimsSetJwtId(claims: map<string, any>, jwtId: string): (map<string, any>, Error?)`
- `cryptoSignedJsonClaimsEnsureJwtId(claims: map<string, any>): (map<string, any>, string, Error?)`
- `cryptoSignedJsonClaimsDefaultValidationPolicy(): CryptoSignedJsonClaimsValidationPolicy`
- `cryptoSignedJsonClaimsRevocationStoreNew(): CryptoSignedJsonClaimsRevocationStore`
- `cryptoSessionTokenDefaultConfig(): CryptoSessionTokenConfig`
- `cryptoSignedJsonClaimsRevocationStoreCount(store: CryptoSignedJsonClaimsRevocationStore): int`
- `cryptoSignedJsonClaimsRevocationStorePurgeExpiredAt(store: CryptoSignedJsonClaimsRevocationStore, nowMillis: int): (CryptoSignedJsonClaimsRevocationStore, int)`
- `cryptoSignedJsonClaimsRevocationStorePurgeExpired(store: CryptoSignedJsonClaimsRevocationStore): (CryptoSignedJsonClaimsRevocationStore, int)`
- `cryptoSignedJsonClaimsRevocationStoreRevokeId(store: CryptoSignedJsonClaimsRevocationStore, jwtId: string, expiresAtMillis: int): (CryptoSignedJsonClaimsRevocationStore, Error?)`
- `cryptoSignedJsonClaimsRevocationStoreRevoke(store: CryptoSignedJsonClaimsRevocationStore, claims: CryptoSignedJsonClaimsEnvelope): (CryptoSignedJsonClaimsRevocationStore, Error?)`
- `cryptoSignedJsonClaimsRevocationStoreHasJwtIdAt(store: CryptoSignedJsonClaimsRevocationStore, jwtId: string, nowMillis: int): (CryptoSignedJsonClaimsRevocationStore, bool, Error?)`
- `cryptoSignedJsonClaimsRevocationStoreHasJwtId(store: CryptoSignedJsonClaimsRevocationStore, jwtId: string): (CryptoSignedJsonClaimsRevocationStore, bool, Error?)`
- `cryptoSignedJsonClaimsRevocationStoreIsRevokedAt(store: CryptoSignedJsonClaimsRevocationStore, claims: CryptoSignedJsonClaimsEnvelope, nowMillis: int): (CryptoSignedJsonClaimsRevocationStore, bool, Error?)`
- `cryptoSignedJsonClaimsRevocationStoreIsRevoked(store: CryptoSignedJsonClaimsRevocationStore, claims: CryptoSignedJsonClaimsEnvelope): (CryptoSignedJsonClaimsRevocationStore, bool, Error?)`
- `cryptoSignedJsonClaimsRevocationStoreDelete(store: CryptoSignedJsonClaimsRevocationStore, jwtId: string): (CryptoSignedJsonClaimsRevocationStore, bool, Error?)`
- `cryptoSignedJsonClaimsSeal(secret: bytes, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, Error?)`
- `cryptoSignedJsonClaimsSealWithAutoJwtId(secret: bytes, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, string, Error?)`
- `cryptoSignedJsonClaimsSealWithTtl(secret: bytes, claims: map<string, any>, ttlMs: int): (string, Error?)`
- `cryptoSignedJsonClaimsSealWithAutoJwtIdTtl(secret: bytes, claims: map<string, any>, ttlMs: int): (string, string, Error?)`
- `cryptoSignedJsonClaimsSealWithKeyId(secret: bytes, keyId: string, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, Error?)`
- `cryptoSignedJsonClaimsSealWithKeyIdAndAutoJwtId(secret: bytes, keyId: string, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, string, Error?)`
- `cryptoSignedJsonClaimsSealText(secretText: string, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, Error?)`
- `cryptoSignedJsonClaimsSealTextWithAutoJwtId(secretText: string, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, string, Error?)`
- `cryptoSignedJsonClaimsSealTextWithTtl(secretText: string, claims: map<string, any>, ttlMs: int): (string, Error?)`
- `cryptoSignedJsonClaimsSealTextWithAutoJwtIdTtl(secretText: string, claims: map<string, any>, ttlMs: int): (string, string, Error?)`
- `cryptoSignedJsonClaimsSealTextWithKeyId(secretText: string, keyId: string, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, Error?)`
- `cryptoSignedJsonClaimsSealTextWithKeyIdAndAutoJwtId(secretText: string, keyId: string, claims: map<string, any>, issuedAtMillis: int, expiresAtMillis: int): (string, string, Error?)`
- `cryptoSignedJsonClaimsSealTextWithKeyIdTtl(secretText: string, keyId: string, claims: map<string, any>, ttlMs: int): (string, Error?)`
- `cryptoSignedJsonClaimsSealTextWithKeyIdAndAutoJwtIdTtl(secretText: string, keyId: string, claims: map<string, any>, ttlMs: int): (string, string, Error?)`
- `cryptoSignedJsonClaimsDecodeUnverified(token: string): (CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsValidateRegistered(claims: CryptoSignedJsonClaimsEnvelope, policy: CryptoSignedJsonClaimsValidationPolicy, nowMillis: int): (bool, Error?)`
- `cryptoSignedJsonClaimsValidateRegisteredNow(claims: CryptoSignedJsonClaimsEnvelope, policy: CryptoSignedJsonClaimsValidationPolicy): (bool, Error?)`
- `cryptoSignedJsonClaimsVerify(secret: bytes, token: string, nowMillis: int): (bool, bool, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsVerifyText(secretText: string, token: string, nowMillis: int): (bool, bool, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsVerifyWithPolicy(secret: bytes, token: string, nowMillis: int, policy: CryptoSignedJsonClaimsValidationPolicy): (bool, bool, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsVerifyTextWithPolicy(secretText: string, token: string, nowMillis: int, policy: CryptoSignedJsonClaimsValidationPolicy): (bool, bool, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsVerifyTextWithPolicyAndRevocation(secretText: string, token: string, nowMillis: int, policy: CryptoSignedJsonClaimsValidationPolicy, revocations: CryptoSignedJsonClaimsRevocationStore): (bool, bool, bool, CryptoSignedJsonClaimsEnvelope, CryptoSignedJsonClaimsRevocationStore, Error?)`
- `cryptoSignedJsonClaimsVerifyKeyRing(keysById: map<string, bytes>, token: string, nowMillis: int): (bool, bool, string?, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsVerifyTextKeyRing(keysById: map<string, string>, token: string, nowMillis: int): (bool, bool, string?, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsVerifyKeyRingWithPolicy(keysById: map<string, bytes>, token: string, nowMillis: int, policy: CryptoSignedJsonClaimsValidationPolicy): (bool, bool, string?, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSignedJsonClaimsVerifyTextKeyRingWithPolicy(keysById: map<string, string>, token: string, nowMillis: int, policy: CryptoSignedJsonClaimsValidationPolicy): (bool, bool, string?, CryptoSignedJsonClaimsEnvelope, Error?)`
- `cryptoSessionTokenIssueText(secretText: string, config: CryptoSessionTokenConfig, subject: string, extraClaims: map<string, any>, nowMillis: int): (CryptoSessionTokenIssueResult, Error?)`
- `cryptoSessionTokenIssueTextNow(secretText: string, config: CryptoSessionTokenConfig, subject: string, extraClaims: map<string, any>): (CryptoSessionTokenIssueResult, Error?)`
- `cryptoSessionTokenVerifyText(secretText: string, config: CryptoSessionTokenConfig, token: string, nowMillis: int, revocations: CryptoSignedJsonClaimsRevocationStore): (CryptoSessionTokenVerifyResult, Error?)`
- `cryptoSessionTokenVerifyTextNow(secretText: string, config: CryptoSessionTokenConfig, token: string, revocations: CryptoSignedJsonClaimsRevocationStore): (CryptoSessionTokenVerifyResult, Error?)`
- `cryptoSessionTokenVerifyTextKeyRing(keysById: map<string, string>, config: CryptoSessionTokenConfig, token: string, nowMillis: int, revocations: CryptoSignedJsonClaimsRevocationStore): (CryptoSessionTokenVerifyResult, Error?)`
- `cryptoSessionTokenVerifyTextKeyRingNow(keysById: map<string, string>, config: CryptoSessionTokenConfig, token: string, revocations: CryptoSignedJsonClaimsRevocationStore): (CryptoSessionTokenVerifyResult, Error?)`
- `cryptoSessionTokenRevokeIssued(store: CryptoSignedJsonClaimsRevocationStore, issued: CryptoSessionTokenIssueResult): (CryptoSignedJsonClaimsRevocationStore, Error?)`
- `cryptoSessionTokenRevokeVerified(store: CryptoSignedJsonClaimsRevocationStore, verified: CryptoSessionTokenVerifyResult): (CryptoSignedJsonClaimsRevocationStore, Error?)`
- `cryptoSessionTokenPairDefaultConfig(): CryptoSessionTokenPairConfig`
- `cryptoSessionTokenPairIssueText(accessSecretText: string, refreshSecretText: string, config: CryptoSessionTokenPairConfig, subject: string, accessClaims: map<string, any>, refreshClaims: map<string, any>, nowMillis: int): (CryptoSessionTokenPairIssueResult, Error?)`
- `cryptoSessionTokenPairIssueTextNow(accessSecretText: string, refreshSecretText: string, config: CryptoSessionTokenPairConfig, subject: string, accessClaims: map<string, any>, refreshClaims: map<string, any>): (CryptoSessionTokenPairIssueResult, Error?)`
- `cryptoSessionTokenPairValidateAccess(verified: CryptoSessionTokenVerifyResult): CryptoSessionTokenVerifyResult`
- `cryptoSessionTokenPairValidateRefresh(verified: CryptoSessionTokenVerifyResult): CryptoSessionTokenVerifyResult`
- `cryptoSessionTokenPairVerifyAccessText(secretText: string, config: CryptoSessionTokenPairConfig, token: string, nowMillis: int, revocations: CryptoSignedJsonClaimsRevocationStore): (CryptoSessionTokenVerifyResult, Error?)`
- `cryptoSessionTokenPairVerifyRefreshText(secretText: string, config: CryptoSessionTokenPairConfig, token: string, nowMillis: int, revocations: CryptoSignedJsonClaimsRevocationStore): (CryptoSessionTokenVerifyResult, Error?)`
- `cryptoSessionTokenPairRevokeIssued(store: CryptoSignedJsonClaimsRevocationStore, issued: CryptoSessionTokenPairIssueResult): (CryptoSignedJsonClaimsRevocationStore, Error?)`
- `cryptoSessionTokenPairRotateRefreshText(accessSecretText: string, refreshSecretText: string, config: CryptoSessionTokenPairConfig, refreshToken: string, nowMillis: int, revocations: CryptoSignedJsonClaimsRevocationStore, accessClaims: map<string, any>, refreshClaimOverrides: map<string, any>): (CryptoSessionTokenPairRotateResult, Error?)`
- `cryptoSessionTokenPairRotateRefreshTextNow(accessSecretText: string, refreshSecretText: string, config: CryptoSessionTokenPairConfig, refreshToken: string, revocations: CryptoSignedJsonClaimsRevocationStore, accessClaims: map<string, any>, refreshClaimOverrides: map<string, any>): (CryptoSessionTokenPairRotateResult, Error?)`

Notes:
- AES-CTR and AES-GCM are available.
- AES-GCM currently uses 12-byte nonces and appends a 16-byte tag to the sealed output.
- Detached AES-GCM wrappers split the combined `ciphertext || tag` form into explicit `(ciphertext, tag, err)` tuples.
- Base64 and Base64URL wrappers are available for transport-friendly sealed payloads.
- Password-envelope helpers derive a 32-byte AES-256-GCM key with PBKDF2-HMAC-SHA256, generate a 16-byte salt and 12-byte nonce, and serialize as `v1.<iterations>.<saltB64Url>.<nonceB64Url>.<sealedB64Url>`.
- `CRYPTO_PASSWORD_AES_GCM_DEFAULT_ITERATIONS` is the stdlib default for the highest-level password seal helpers.
- Password-hash helpers serialize as `pbkdf2-sha256.v1.<iterations>.<saltB64Url>.<digestB64Url>`, return `false` on password mismatch, and reserve errors for malformed input or crypto failures.
- Verify-and-upgrade helpers return `(ok, upgraded, replacement, err)` so callers can persist a replacement hash only when the password matched and policy requires rehashing.
- `CRYPTO_PASSWORD_HASH_DEFAULT_ITERATIONS` is the stdlib default for the highest-level password-hash helpers.
- Signed tokens serialize as `v1.<issuedAtMs>.<expiresAtMs>.<payloadB64Url>.<tagB64Url>`, use HMAC-SHA256 over the signed body, and return `(ok, expired, envelope, err)` from verify helpers.
- `cryptoSignedTokenVerify*` is tolerant for auth paths: malformed/signature-invalid tokens return `ok=false` without raising an error; use `cryptoSignedTokenDecode` if you need strict parse errors.
- Signed JSON claims wrap the token payload as `{"claims": <map>, "kid": <string>?}` and reuse the same outer signed-token format.
- Key-ring verification prefers the embedded `kid` when present and otherwise falls back to trying each configured key until one validates.
- Claim-construction helpers let callers build standard `sub`/`iss`/`aud`/`nbf`/`jti` fields without hand-writing raw claim maps; `aud` can be either a single string or an array of strings.
- `cryptoSignedJsonClaimsEnsureJwtId` preserves a valid existing `jti` and otherwise generates a UUID v4, making it practical to issue revocable session tokens without a separate ID-generation step.
- Registered-claim policy helpers validate `iss`, `aud`, `nbf`, and `jti`. `nbf` is interpreted in milliseconds to match TabloLang token timestamps, and `aud` accepts either a string or an array of strings.
- Policy-validated verify helpers keep the same auth-friendly contract: policy mismatch returns `ok=false` without raising an error, while invalid policy configuration still returns an error.
- Revocation helpers keep an in-memory `jti -> expiresAtMillis` store. Expired revocations are purged lazily, and revocation requires a non-empty `jti` claim.
- The `*WithAutoJwtId*` seal helpers return `(token, jti, err)` so callers can store the generated `jti` directly for logout/session revocation flows.
- `cryptoSignedJsonClaimsVerifyTextWithPolicyAndRevocation` returns `(ok, expired, revoked, claims, updatedStore, err)` so session/logout flows can check revocation and carry the lazily-purged store forward.
- `cryptoSessionTokenDefaultConfig()` is intentionally opinionated for auth flows: 15-minute TTL, 30-second clock skew, and `nbf` included by default.
- Session-token issue helpers centralize reserved claims: they always set `sub`, optionally apply configured `iss`/`aud`, set `nbf` when enabled, and auto-generate or preserve `jti`.
- Session-token verify helpers require a valid `sub` and `jti` in addition to signature/policy checks, return a typed result record, and carry the updated revocation store forward.
- Key-ring session verification returns the matched key id in `CryptoSessionTokenVerifyResult.keyId`, even when the token omitted `kid` and verification succeeded via fallback probing.
- `cryptoSessionTokenPairDefaultConfig()` pairs the 15-minute access-token default with a 30-day refresh-token default.
- Session-token pairs add two reserved claims on top of the base session layer: `sid` for a shared session UUID and `tokenKind` set to `access` or `refresh`.
- `cryptoSessionTokenPairValidateAccess` and `cryptoSessionTokenPairValidateRefresh` compose with the ordinary session verify helpers, including key-ring verification, by enforcing `sid` shape and the expected token kind without turning malformed auth tokens into hard errors.
- `cryptoSessionTokenPairRevokeIssued` revokes both issued JWT IDs in one call, which is the intended logout path when you treat access and refresh tokens as a single session pair.
- Refresh rotation preserves the existing `sid`, revokes only the presented refresh tokenâ€™s `jti`, and returns a replacement pair plus the updated revocation store.
- `cryptoSessionTokenPairRotateRefreshText*` inherits non-managed custom claims from the verified refresh token into the replacement refresh token, then applies `refreshClaimOverrides`; access-token claims stay caller-controlled through `accessClaims`.
- Rotation remains auth-friendly: a bad, expired, revoked, or wrong-kind refresh token returns `ok=false`/`expired`/`revoked` in `CryptoSessionTokenPairRotateResult` instead of raising an error.
- `cryptoSignedJsonClaimsVerify*` remains auth-friendly: malformed or non-matching tokens return `ok=false`; use `cryptoSignedJsonClaimsDecodeUnverified` only for tooling or key selection.
- Use `lib/hash.tblo` for stable non-cryptographic fingerprints and `lib/crypto.tblo` for security-sensitive digests.

```tblo
import "lib/crypto.tblo";

func main(): void {
    var digest = cryptoSha256TextHex("abc");
    var mac = cryptoHmacSha256TextHex("key", "payload");
    var verify = cryptoHmacSha256VerifyTextHex("key", "payload", "5d98b45c90a207fa998ce639fea6f02ecc8cc3f36fef81d694fb856b4d0a28ca");
    var derived = cryptoPbkdf2HmacSha256TextHex("password", "salt", 4096, 32);
    var hkdf = cryptoHkdfHmacSha256TextHex("secret", "salt", "info", 32);
    var key = hexToBytes("2b7e151628aed2a6abf7158809cf4f3c").0;
    var ctr = hexToBytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff").0;
    var sealed = cryptoAesCtrEncryptTextHex(key, ctr, "hello");
    var gcm_key = hexToBytes("00000000000000000000000000000000").0;
    var gcm_nonce = hexToBytes("000000000000000000000000").0;
    var gcm = cryptoAesGcmSealTextHex(gcm_key, gcm_nonce, "", bytesWithSize(0, 0).0);
    var gcm_b64 = cryptoAesGcmSealTextBase64Url(gcm_key, gcm_nonce, "", bytesWithSize(0, 0).0, false);
    var password_token = cryptoPasswordAesGcmSealTextBase64UrlDefault("hunter2", "hello", bytesWithSize(0, 0).0);
    var password_hash = cryptoPasswordHashCreateBase64UrlDefault("hunter2");
    var password_upgrade = cryptoPasswordHashVerifyAndUpgradeBase64UrlDefault("hunter2", password_hash.0);
    var signed = cryptoSignedTokenSealTextWithTtl("secret", "hello", 60000);
    var claims = cryptoSignedJsonClaimsSealTextWithKeyIdTtl("secret",
                                                            "k1",
                                                            {"sub": "u1", "role": "admin"},
                                                            60000);
    var same = cryptoConstantTimeTextEqual("a", "a");

    if (digest.1 == nil) println(digest.0);
    if (mac.1 == nil) println(mac.0);
    if (verify.1 == nil) println(verify.0);
    if (derived.1 == nil) println(derived.0);
    if (hkdf.1 == nil) println(hkdf.0);
    if (sealed.1 == nil) println(sealed.0);
    if (gcm.1 == nil) println(gcm.0);
    if (gcm_b64.1 == nil) println(gcm_b64.0);
    if (password_token.1 == nil) println(password_token.0);
    if (password_hash.1 == nil) println(password_hash.0);
    if (password_upgrade.3 == nil && password_upgrade.1) println(password_upgrade.2);
    if (signed.1 == nil) println(signed.0);
    if (claims.1 == nil) println(claims.0);
    if (same.1 == nil) println(same.0);
}
```

#### Standard library module: `lib/sort.tblo`
Typed sorting and search helpers for everyday backend and data workflows.

It provides:
- `sortIntsAsc(items: array<int>): array<int>`
- `sortIntsDesc(items: array<int>): array<int>`
- `sortDoublesAsc(items: array<double>): array<double>`
- `sortDoublesDesc(items: array<double>): array<double>`
- `sortIsSortedIntsAsc(items: array<int>): bool`
- `sortIsSortedDoublesAsc(items: array<double>): bool`
- `sortLowerBoundInts(sorted_items: array<int>, target: int): int`
- `sortUpperBoundInts(sorted_items: array<int>, target: int): int`
- `sortBinarySearchInts(sorted_items: array<int>, target: int): (int, bool)`
- `sortTopKSmallestInts(items: array<int>, k: int): (array<int>, Error?)`
- `sortTopKLargestInts(items: array<int>, k: int): (array<int>, Error?)`
- `sortUniqueInts(items: array<int>): array<int>`

Behavior notes:
- Sort helpers return copied arrays and do not mutate the input.
- `sortIntsAsc` / `sortDoublesAsc` use builtin `sort(...)` as the fast path, then fall back to pure TabloLang quicksort if validation fails.
- `sortIntsDesc` / `sortDoublesDesc` use builtin `reverse(...)` as the fast path, then fall back to manual reverse if validation fails.
- `sortTopK*` validates `k` and returns `ERR_INVALID_ARGUMENT` when `k` is out of range.
- `sortLowerBoundInts`/`sortUpperBoundInts` assume input is already sorted ascending.

```tblo
import "lib/sort.tblo";

func main(): void {
    var nums: array<int> = [9, 1, 7, 3, 5, 2];
    var asc = sortIntsAsc(nums);
    var top = sortTopKLargestInts(nums, 3);

    println(str(asc[0]) + "," + str(asc[len(asc) - 1]));
    if (top.1 == nil) {
        println(str(top.0[0]) + "," + str(top.0[1]) + "," + str(top.0[2]));
    }
}
```

#### Standard library module: `lib/io.tblo`
For higher-level code, use the stdlib wrapper module:
- `Reader` / `Writer` records
- `BufferedReader` / `BufferedWriter` records
- `ioOpenReader`, `ioOpenWriter`, `ioOpenAppender`
- `ioReadLineFrom`, `ioReadAllFrom`, `ioReadChunkFrom`, `ioReadChunkBytesFrom`, `ioReadExactlyBytesFrom`
- `ioWriteAllTo`, `ioWriteBytesAllTo`
- `ioLines` (line iteration helper)
- `ioCopyAll`, `ioCopyFile`
- `ioLinesWithContext`, `ioCopyAllWithContext`, `ioCopyFileWithContext`
- `ioBufferedReaderFrom`, `ioBufferedWriterFrom`, `ioOpenBufferedReader`, `ioOpenBufferedWriter`
- `ioBufferedReadLineFrom`, `ioBufferedReadAllFrom`, `ioBufferedLinesFrom`
- `ioBufferedWriteAllTo`, `ioBufferedFlushTo`, `ioBufferedCopyAll`, `ioBufferedCopyFile`

```tblo
import "lib/io.tblo";

func main(): void {
    var r = ioOpenReader("input.txt");
    if (r.1 != nil) return;

    var w = ioOpenWriter("output.txt");
    if (w.1 != nil) {
        ioCloseReader(r.0);
        return;
    }

    var copied = ioCopyAll(r.0, w.0, 4096);
    if (copied.1 == nil) {
        println("copied " + str(copied.0) + " bytes");
    }

ioCloseReader(r.0);
ioCloseWriter(w.0);
}
```

Buffered writer example:

```tblo
import "lib/io.tblo";

func main(): void {
    var opened = ioOpenWriter("output.txt");
    if (opened.1 != nil) return;

    var bw = ioBufferedWriterFrom(opened.0, 8192);
    if (bw.1 != nil) {
        ioCloseWriter(opened.0);
        return;
    }

    var w1 = ioBufferedWriteAllTo(bw.0, "hello ");
    var w2 = ioBufferedWriteAllTo(w1.0, "world\n");
    var flushed = ioBufferedFlushTo(w2.0);
    if (flushed.2 == nil) {
        println("flushed bytes: " + str(flushed.1));
    }

    ioCloseWriter(opened.0);
}
```

#### `read_line(path: string): (string, Error?)`
Reads a single line from a file.
- On success: returns `(line, nil)`
- On error: returns `("", err)`
- On EOF: returns `("", nil)`
```tblo
var r = read_line("data.txt");
if (r.1 != nil) {
    println("read_line failed: " + r.1.message);
    return;
}
println(r.0);
```

#### `read_all(path: string): (string, Error?)`
Reads the entire contents of a file as a string.
```tblo
var r = read_all("data.txt");
if (r.1 != nil) {
    println("read_all failed: " + r.1.message);
    return;
}
println(r.0);
```

#### `write_line(path: string, content: string): (bool, Error?)`
Appends a line to a file (creates file if it doesn't exist).
```tblo
var r = write_line("output.txt", "Hello, World!");
if (r.1 != nil) println("write_line failed: " + r.1.message);
```

#### `write_all(path: string, content: string): (bool, Error?)`
Writes content to a file, replacing existing content (creates file if it doesn't exist).
```tblo
var r = write_all("output.txt", "This replaces any existing content");
if (r.1 != nil) println("write_all failed: " + r.1.message);
```

#### `readBytes(path: string): (bytes, Error?)`
Reads the entire contents of a file as bytes.
```tblo
var r = readBytes("data.bin");
if (r.1 == nil) println("Read " + str(len(r.0)) + " bytes");
```

#### `writeBytes(path: string, data: bytes): (bool, Error?)`
Writes bytes to a file (overwriting).
```tblo
var r = writeBytes("data.bin", hexToBytes("00 01 02 ff").0);
if (r.1 != nil) println("writeBytes failed: " + r.1.message);
```

#### `appendBytes(path: string, data: bytes): (bool, Error?)`
Appends bytes to a file (creates file if it doesn't exist).
```tblo
var r = appendBytes("data.bin", hexToBytes("0a 14 1e").0);
if (r.1 != nil) println("appendBytes failed: " + r.1.message);
```

#### `stdoutWriteBytes(data: bytes): (bool, Error?)`
Writes raw bytes to stdout.
- On Windows, this temporarily switches stdout to binary mode during the write, then restores the previous mode.
```tblo
var r = stdoutWriteBytes(stringToBytes("P4\n").0);
if (r.1 != nil) println("stdoutWriteBytes failed: " + r.1.message);
```

#### `exists(path: string): (bool, Error?)`
Returns true if the file exists, false otherwise. Returns an error if the path is not allowed (sandbox) or the check fails.
```tblo
var r = exists("data.txt");
if (r.1 == nil && r.0) {
    println("File exists");
}
```

#### `delete(path: string): (bool, Error?)`
Deletes a file.
```tblo
var r = delete("temp.txt");
if (r.1 != nil) println("delete failed: " + r.1.message);
```

### String Operations

Strings support the following operations:

#### Concatenation: `+`
```tblo
var greeting: string = "Hello, " + "World!";  // "Hello, World!"
var combined: string = "Number: " + str(42);   // "Number: 42"
```

#### Length: `len()`
```tblo
len("hello");  // Returns 5
```

#### Comparison
Strings can be compared using `<`, `<=`, `>`, `>=`, `==`, `!=` (lexicographic order).
```tblo
"apple" < "banana";  // true
"hello" == "hello";  // true
```

#### Array Indexing
Access individual characters by index (returns single-character string).
```tblo
var s: string = "hello";
// s[0] would return "h" if string indexing was fully implemented
```

### Constants

The following boolean constants are defined:
- `true`
- `false`

### Error Handling

TabloLang uses two error mechanisms:
- **Recoverable errors**: fallible operations return `(value, err)` where `err` is `Error?` (`nil` on success).
- **Runtime errors (traps)**: reserved for programmer errors and VM invariants (division by zero, array index out of bounds, stack overflow, memory allocation failures). Runtime errors terminate the program. You can also abort explicitly with `panic("message")`.
  Runtime trap output includes a stack trace with call frames and best-effort source locations.

Conveniences:
- **Postfix `?`**: `expr?` expects `expr` to be a `(value, err)` tuple and either unwraps the value (when `err == nil`) or returns early from the current function with the tuple (when `err != nil`). This is only valid inside functions returning a compatible `(T, Error?)` tuple. (This is different from nullable types like `T?`.)
- **`must(...)`**: unwraps `(value, err)` and aborts the program when `err != nil`.
- **Tuple destructuring**: `var (value, err) = expr;` binds tuple elements to variables. Use `_` to discard an element (`var (_, err) = expr;`).
- **`wrapError(err, context)`**: adds a context string to an error; stores the original error as the cause in `Error.data`.
- **`defer`**: `defer f(args...);` runs cleanup calls in LIFO order when the current function returns (including early returns from `?`). Arguments are evaluated at the point of the `defer` statement.

The builtin `Error` type is:
```tblo
record Error {
    code: int;
    message: string;
    data: any
}
```

Builtin error codes (subject to change):
- `ERR_INVALID_ARGUMENT` (`1`) invalid argument
- `ERR_PARSE` (`2`) parse
- `ERR_PERMISSION` (`3`) permission
- `ERR_IO` (`4`) I/O
- `ERR_LIMIT` (`5`) limit
- `ERR_UNSUPPORTED` (`6`) unsupported
- `ERR_NETWORK` (`7`) network
- `ERR_HTTP` (`8`) HTTP
- `ERR_CRYPTO` (`9`) crypto
- `ERR_INTERNAL` (`10`) internal

`Error.data` is optional extra context; for example, `toBigInt("12x")` sets `err.data` to `"12x"`, and `jsonParse` sets `err.data` to a context map with `path`, `offset`, `line`, `column`, `near`, and `span`.

Example:
```tblo
var r = read_all("data.txt");
if (r.1 != nil) {
    println("read failed: " + r.1.message);
    return;
}
println(r.0);
```

## Entry Point

Programs must have a `main()` function:
```tblo
func main(): void {
    // Program code here
}
```

## Imports

Multi-file programs use:
```tblo
import "utils.tblo";
```

Paths are resolved relative to the importing file. Circular imports are detected and reported as errors.

## Modules And Dependencies (CLI)

Use the module manager to track dependencies with deterministic lock files:

```bash
tablo mod init example/app
tablo mod add acme/util@1.2.3 --source path:../dep_pkg
tablo mod add acme/clock@^0.1.0 --source registry:acme/clock
tablo mod update
tablo mod fetch
tablo mod verify
tablo mod list
tablo mod vendor
tablo mod tidy
```

`tablo mod` writes:
- `tablo.mod`: module metadata + declared dependencies
- `tablo.lock`: deterministic dependency lock + checksums + source provenance (`sourceHash`, `sourceSignature`)

For registry dependencies, `tablo.mod` stores both:
- `constraint` (declared range, for example `^0.1.0`)
- `version` (currently resolved concrete version, updated by `mod update`)

Sources currently supported:
- `path:<dir>` local directory dependencies
- `registry:<name>` registry dependencies resolved from `TABLO_REGISTRY_ROOT` (or `.tablo_registry` by default)

`mod fetch` materializes registry dependencies into `vendor/` and validates both checksums and source provenance against `tablo.lock` (it does not rewrite the lock file). `mod update` resolves newer registry versions that still satisfy declared constraints and rewrites `tablo.mod`/`tablo.lock`. `mod verify` recomputes checksums and source provenance and validates them against `tablo.lock`.

For source provenance, TabloLang uses:
- `sourceHash`: hash of dependency source identity (including source path/root and lock metadata)
- `sourceSignature`: either `"unsigned"` (no signature file) or a canonical signed marker `sig:hmac-sha256:<keyId>:<signatureHex>`

Detached signatures are loaded from `.tablo.sig` in each dependency root:

```json
{
  "keyId": "acme-release",
  "algorithm": "hmac-sha256",
  "signature": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

Trusted keys are loaded from `tablo.keys` in the current module directory (override with `TABLO_KEYS_FILE`):

```json
{
  "schemaVersion": 1,
  "keys": [
    {
      "id": "acme-release",
      "algorithm": "hmac-sha256",
      "secret": "replace-with-your-shared-secret"
    }
  ]
}
```

`hmac-sha256` signatures cover dependency metadata plus dependency checksum (`name`, `version`, `constraint`, `source`, `checksum`). The `.tablo.sig` metadata file is excluded from package checksum hashing to avoid circular signature invalidation.

Vendored files are copied under `vendor/<dependency-name>/...`, so imports follow:

```tblo
import "vendor/acme/util/util.tblo";
```

Lock-aware import resolution also supports dependency-style paths when the dependency is declared in `tablo.lock` and present in `vendor/`:

```tblo
import "acme/util/util.tblo";
```

TabloLang uses a "module root" for dependency resolution and sandboxing: when running or compiling a program, it walks up from the entry file's directory to find `tablo.mod` or `tablo.lock` and uses the first directory found as the module root. If no module files are found, the entry file's directory is used.

## Building

```bash
cmake -S . -B build
cmake --build build --config Debug
```

Optional SQLite build-time linking (instead of runtime loading):
```bash
cmake -S . -B build -DTABLO_SQLITE_STATIC_LINK=ON
cmake --build build --config Release
```

Build hardening is enabled by default (`-DENABLE_HARDENING=ON`):
- Linux: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-fPIE`, and linker `-Wl,-z,relro,-z,now -pie`
- Windows (MSVC): `/GS /guard:cf` and linker `/guard:cf /DYNAMICBASE /NXCOMPAT`

To validate Linux Release hardening in CI/local builds:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
python3 tools/check_build_hardening.py --build-dir build --config Release --target tablo
```

## Running Programs

```bash
tablo run program.tblo
```

Print version information:
```bash
tablo --version
```

Run a compiled bytecode artifact directly:
```bash
tablo run program.tbc
```

With command-line arguments:
```bash
tablo run program.tblo arg1 arg2 arg3
```

With bytecode dump (options must come before the file):
```bash
tablo run --dump-bytecode program.tblo arg1 arg2
```

With unused-error linting:
```bash
tablo run --warn-unused-error program.tblo
tablo run --strict-errors program.tblo
```

With runtime file/socket descriptor limits:
```bash
tablo run --max-open-files 64 --max-open-sockets 32 program.tblo
tablo test --max-open-files 64 --max-open-sockets 32
```

With experimental JIT hotness profiling groundwork:
```bash
tablo run --profile-jit --jit-hot-threshold 25 program.tblo
tablo run --dump-jit-queue --jit-hot-threshold 25 program.tblo
tablo run --profile-jit --dump-jit-queue --drain-jit-queue --jit-hot-threshold 25 program.tblo
tablo run --profile-jit --jit-auto-compile --jit-hot-threshold 25 program.tblo
```
This records per-function entry counts, marks functions as `hot=yes` once they cross the configured threshold, and enqueues newly hot functions into a per-run JIT work queue. Profile and queue dumps now include a `reason=` field so the current backend decision is explicit: `queued-hot`, `native-hint`, `native-exact`, `stub-fallback`, or `unsupported-async`, plus a coarse `support=` field from the compiler-produced `JitFunctionProfile`: `none`, `stub`, or `stub+native-summary`, and a `family=` field showing which native leaf family the compiler tagged for exact-summary recovery: `none`, `arithmetic`, `compare`, or `selector`. `--drain-jit-queue` runs the current experimental backend stack, which advances queued functions through `queued -> compiling -> compiled-native`, `compiled-stub`, or `failed` depending on what the backend can lower. Today the native path is intentionally narrow: tiny sync integer leaf shapes such as `return n + const`, `return n - const`, `return n * const`, `return n / const`, `return n % const`, `return n & const`, `return n | const`, `return n ^ const`, plain two-argument integer leaves such as `return a + b`, `return a - b`, `return a * b`, `return a & b`, `return a | b`, `return a ^ b`, exact two-argument integer comparison leaves returning `bool` such as `return a < b`, `return a <= b`, `return a == b`, `return a != b`, `return a > b`, `return a >= b`, one-argument integer selector leaves such as `if (n < const) return n; return const` or `if (n < const) return const; return n`, guarded comparison leaves such as `if (a < const) return false; return a < b` or `if (a < const) return true; return a == b`, and guarded selector forms such as `if (a < const) return a; return a <op> b`, `if (n < guard) return n; if (n < const) return n; return const`, `if (n < guard) return n; if (n < const) return const; return n`, or `if (a < const) return a; if (a < b) return a; return b` can bypass interpreter frame setup through a compiled entry hook. The compiler now records a broader `JitFunctionProfile` on each compiled function, carrying async/capture flags, support bits, function counts, the current leaf summary, and the coarse native family mask used to gate exact-summary recovery. The current native leaf summary path then lowers arithmetic, comparison, and selector families from that profile into generic `op`-driven native plans instead of one compiled-plan enum per leaf operator. Compiler summary/hint metadata is now the primary native path: if either is present, the JIT trusts that compiler-produced metadata and does not fall through to exact bytecode rematching. The exact fallback is shrinking too: cleared-metadata arithmetic, compare, and selector leaves now first recover a generic `JitFunctionSummary` from bytecode and then reuse that same summary-to-plan path instead of hand-emitting native plans directly. `native-exact` is now reserved for the narrower fallback where a `support=stub+native-summary` function has its summary/hint metadata cleared or unavailable and the backend has to recover from bytecode shape alone. `support=stub` and `support=none` bypass that rematcher, and the new `family=` metadata keeps even `support=stub+native-summary` recovery from probing irrelevant exact-summary families. Unsupported sync functions still fall back to `compiled-stub`, and async functions are still reported as unsupported. `--jit-auto-compile` auto-drains newly hot functions immediately, which is enough to prove later global, interface, and dynamic `OP_CALL` call sites can dispatch through per-function compiled entries.

## Compiling (CLI)

```bash
tablo compile program.tblo -o bytecode.out
```

`-o` now emits a runnable bytecode artifact that can be executed with `tablo run <artifact>`.

When running source files, TabloLang also keeps a dependency-aware bytecode cache in the OS temp directory to speed up cold starts. Cache entries are invalidated when any module mtime changes or typecheck strictness flags differ.

Cold-start behavior is covered in `RuntimeSafetyTests`, including load-mode checks (`source`/`cache`/`artifact`) and median startup regression hints.

## LSP (CLI)

```bash
tablo lsp symbols tests/tablo_tests/lsp_symbols_test.tblo
tablo lsp --stdio
```

The initial LSP server speaks stdio JSON-RPC and currently supports:

- `initialize`
- `textDocument/didOpen`
- `textDocument/didChange`
- `textDocument/didClose`
- `textDocument/didSave`
- `shutdown`
- `exit`
- `textDocument/documentSymbol`
- `textDocument/hover`
- `textDocument/definition`

`tablo lsp symbols <file>` is a one-shot helper that prints the same document-symbol JSON used by the LSP path, which makes it easy to script and test.

The initial hover/definition support is semantic rather than text-only:

- `hover` returns typed details for top-level declaration names, identifier references, and field/member accesses
- `definition` resolves top-level declarations plus record-field and enum-member declarations reached through semantic field/member access
- local variables and function parameters now resolve in `hover`/`definition`
- named type references in annotations now resolve to top-level `record` / `interface` / `enum` / `type` declarations
- generic type-parameter references now resolve in generic `record`, `type`, `enum`, and `func` declarations, including constrained function type parameters
- diagnostics are published on document open/change/save/close using full-text sync, currently reporting the first parse or type error in the document
- `documentSymbol`, `hover`, and `definition` read from the open-document store when a document is open, so unsaved edits are reflected instead of falling back to on-disk file contents

## Debugger (CLI)

```bash
tablo debug --break 12 tests/tablo_tests/debug_breakpoint_test.tblo
tablo debug --break tests/tablo_tests/fixtures/debug_breakpoint_helper.tblo:2 tests/tablo_tests/debug_breakpoint_multifile_test.tblo
tablo debug --break 8 --step-in tests/tablo_tests/debug_breakpoint_test.tblo
```

The first debugger slice is intentionally narrow:

- one-shot source-line breakpoints through repeatable `tablo debug --break <line|file:line> <file>`
- resumable debug actions through repeatable `--continue`, `--step-in`, and `--step-over`
- breakpoint stops are resolved from VM bytecode `debug_info`
- the stop report prints file/line/function plus the current call stack

Current limit:

- `--break <line>` targets the entry file, while `--break <file>:<line>` targets any compiled source file in the run
- the current CLI surface is still one-shot rather than interactive; actions are supplied up front and run in order

## Debugger (DAP)

```bash
tablo dap --stdio
```

The first DAP slice is intentionally narrow and reuses the same VM breakpoint/step engine as `tablo debug`.

Currently supported requests/events:

- requests: `initialize`, `launch`, `setBreakpoints`, `setExceptionBreakpoints`, `configurationDone`, `threads`, `stackTrace`, `exceptionInfo`, `scopes`, `variables`, `setVariable`, `evaluate`, `pause`, `continue`, `next`, `stepIn`, `stepOut`, `disconnect`
- events: `initialized`, `stopped`, `output`, `terminated`

Current limits:

- launch-only, stdio-only DAP transport
- `launch.stopOnEntry=true` is supported and stops before the first instruction of `main`
- `pause` is supported for running programs; queued inspection requests wait for the next stop before reading VM state
- source line breakpoints only
- `setBreakpoints` supports per-line `condition` expressions over the current stopped-frame `evaluate` subset
- `setBreakpoints` supports `hitCondition` values in exact-count form (`"3"`) plus `">=N"`, `"==N"`, and `"%N"` variants
- one exception breakpoint filter: `panic`
- single-thread model
- arguments, locals, and globals are available through `scopes` / `variables`
- nested expansion is available for arrays, tuples, records, futures, maps, and sets
- record instances without preserved field-definition metadata currently fall back to debugger labels like `field0`, `field1`
- `evaluate` is available for stopped frames over identifier roots with chained `.field` and `[index]` selectors
- `setVariable` can edit argument, local, and global root variables at a stop using literals or the same identifier/selector expression subset as `evaluate`
- `setExceptionBreakpoints` can stop on runtime panics/errors as DAP `exception` stops; `continue` from that stop terminates rather than recovering execution
- `exceptionInfo` exposes the panic summary and full runtime stack-trace text for the current exception stop

## Testing

```bash
tablo test
# Explicit files/directories:
tablo test tests/tablo_tests/testing_assertions_test.tblo
tablo test --fail-fast tests/tablo_tests
# List and filter:
tablo test --list
tablo test --match assertions
tablo test --list --match errorcode
# Machine-readable output:
tablo test --json
# JUnit report:
tablo test --junit build/tablo_tests.junit.xml
# Per-test hard timeout and parallel jobs:
tablo test --timeout-ms 250
tablo test --jobs 4 --timeout-ms 1000
# Retry and sharding (CI):
tablo test --rerun-failed 1
tablo test --shard 1/4 --jobs 4 --rerun-failed 1
# Typecheck strictness while running tests:
tablo test --warn-unused-error
tablo test --strict-errors
```

`tablo test` discovers `*_test.tblo` files recursively under `tests/tablo_tests/` by default. In that default-discovery mode it runs zero-argument `test*` functions only, so helper/debugger fixtures that happen to end in `_test.tblo` are not executed through legacy `main()` fallback. If you pass explicit file targets, `tablo test` still falls back to `main` for legacy compatibility when a file has no zero-argument `test*` functions.

Each test runs in an isolated child process, so crashes are reported as test failures instead of crashing the full runner. `--timeout-ms` enforces a hard per-test timeout by terminating overdue child processes.

`--rerun-failed` retries only failing tests up to N additional attempts, `--shard i/n` deterministically partitions tests for parallel CI workers, and `--junit` writes a JUnit XML report for CI test UIs.

To run the C/CTest suite:

```bash
ctest --test-dir build -C Debug --output-on-failure
# Or from CLI:
tablo test --ctest
```

The CTest suite includes `TabloLangCliRunnerJUnit`, which runs `tablo test --junit ... --rerun-failed 1 --shard 1/1` and writes `build/tablo_tests.junit.xml` for CI artifact collection.

## Fuzzing

LLVM libFuzzer harnesses are available under `tests/fuzz/` for:
- lexer tokenization
- parser front-end parsing
- parse + typecheck + bytecode compilation
- HTTP request/response parsing plus chunked-body decoding
- bytecode artifact loading from in-memory bytes

```bash
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DENABLE_FUZZING=ON -DUSE_SANITIZERS=ON
cmake --build build-fuzz --target fuzz_lexer fuzz_parser fuzz_compile fuzz_http fuzz_artifact
./build-fuzz/fuzz_lexer -max_total_time=60 tests/fuzz/corpus/lexer
./build-fuzz/fuzz_parser -max_total_time=60 tests/fuzz/corpus/parser
./build-fuzz/fuzz_compile -max_total_time=60 tests/fuzz/corpus/compile
./build-fuzz/fuzz_http -max_total_time=60 tests/fuzz/corpus/http
./build-fuzz/fuzz_artifact -max_total_time=60 tests/fuzz/corpus/artifact
```

The default build also provides `fuzz_corpus_runner`, which replays the checked-in seed corpora through the same harness entrypoints without libFuzzer. That keeps the seed coverage executable on normal toolchains, including MSVC:

```bash
cmake -S . -B build
cmake --build build --target fuzz_corpus_runner
./build/fuzz_corpus_runner
```

When `ENABLE_FUZZING=ON`, CTest also exposes short libFuzzer smoke runs (`FuzzLexerSmoke`, `FuzzParserSmoke`, `FuzzCompileSmoke`, `FuzzHttpSmoke`, `FuzzArtifactSmoke`). The default CTest matrix now also runs `FuzzCorpusReplay`, so the checked-in corpora stay verified even when the libFuzzer toolchain is unavailable.

## Benchmarking

Run benchmarks in `Release` for more realistic numbers:

```bash
./build/Release/tablo run tests/benchmark_suite.tblo
./build/Release/tablo run tests/benchmark_workloads.tblo
./build/Release/tablo run tests/benchmark_workloads.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_http_streaming.tblo
./build/Release/tablo run tests/benchmark_http_streaming.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_http_server.tblo
./build/Release/tablo run tests/benchmark_http_server.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_io_buffered.tblo
./build/Release/tablo run tests/benchmark_io_buffered.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_observability_stdlib.tblo
./build/Release/tablo run tests/benchmark_observability_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_task_group.tblo
./build/Release/tablo run tests/benchmark_task_group.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_process_stdlib.tblo
./build/Release/tablo run tests/benchmark_process_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_sqlite_stdlib.tblo
./build/Release/tablo run tests/benchmark_sqlite_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_template_stdlib.tblo
./build/Release/tablo run tests/benchmark_template_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_context_stdlib.tblo
./build/Release/tablo run tests/benchmark_context_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_log_stdlib.tblo
./build/Release/tablo run tests/benchmark_log_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_fs_stdlib.tblo
./build/Release/tablo run tests/benchmark_uuid_stdlib.tblo
./build/Release/tablo run tests/benchmark_netip_stdlib.tblo
./build/Release/tablo run tests/benchmark_semver_stdlib.tblo
./build/Release/tablo run tests/benchmark_regexp_stdlib.tblo
./build/Release/tablo run tests/benchmark_hash_stdlib.tblo
./build/Release/tablo run tests/benchmark_hash_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_random_stdlib.tblo
./build/Release/tablo run tests/benchmark_random_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_sort_stdlib.tblo
./build/Release/tablo run tests/benchmark_sort_stdlib.tblo 2   # scale up
./build/Release/tablo run tests/benchmark_toml_stdlib.tblo
./build/Release/tablo run tests/benchmark_yaml_stdlib.tblo
./build/Release/tablo run tests/benchmark_ini_stdlib.tblo
./build/Release/tablo run tests/benchmark_msgpack_stdlib.tblo
./build/Release/tablo run tests/benchmark_glob_stdlib.tblo
python tools/benchmark_cold_start.py --config Release --scale 1 --samples-per-mode 3
python tools/benchmark_fast_string_baseline.py --config Release --samples 5 --warmup-runs 1
python tools/benchmark_fast_string_baseline.py --config Release --samples 5 --warmup-runs 1 --out tools/baselines/fast_string_release.json
python tools/compare_fast_string_baseline.py build/fast_string_base.json build/fast_string_candidate.json
```

Or run benchmark suites and emit JSON metrics:

```bash
python tools/run_benchmarks.py --config Release --suite all --scale 2
python tools/run_benchmarks.py --config Release --suite http-streaming --scale 1
python tools/run_benchmarks.py --config Release --suite http-server --scale 1
python tools/run_benchmarks.py --config Release --suite io-buffered --scale 1
python tools/run_benchmarks.py --config Release --suite observability-stdlib --scale 1
python tools/run_benchmarks.py --config Release --suite task-stdlib --scale 1
python tools/run_benchmarks.py --config Release --suite process-stdlib --scale 1
python tools/run_benchmarks.py --config Release --suite sqlite-stdlib --scale 1
python tools/run_benchmarks.py --config Release --suite template-stdlib --scale 1
python tools/run_benchmarks.py --config Release --suite context-stdlib --scale 1
python tools/run_benchmarks.py --config Release --suite log-stdlib --scale 1
python tools/run_benchmarks.py --config Release --suite cold-start --scale 1
python tools/run_benchmarks.py --config Release --suite fast-string-baseline --scale 1
```

Run focused perf-gate benchmarks:

```bash
python tools/run_benchmarks.py --config Release --suite perf-gates --scale 1
python tools/check_perf_gates.py --config Release --samples 5 --warmup-runs 1 --transient-retries 1 --cmd-timeout-seconds 180
# While iterating, filter to a subset:
python tools/check_perf_gates.py --config Release --match-program benchmark_http --samples 1 --min-samples 1 --warmup-runs 0
python tools/check_perf_gates.py --config Release --match-label "perf jsonParse" --samples 1 --min-samples 1 --warmup-runs 0
```

`tools/check_perf_gates.py` compiles TabloLang benchmark program(s) referenced by `tools/perf_gates.json` to temporary artifacts (and runs any referenced Python benchmark scripts directly), executes warmups, and applies bounded retries for known transient process failures before enforcing gates using median throughput and minimum passing-sample counts.

`tools/benchmark_fast_string_baseline.py` captures repeated samples for string hot-path metrics and reports fast-vs-generic median speedups for map/set operations (optionally writing a JSON snapshot via `--out`).

`tools/compare_fast_string_baseline.py` compares two such snapshots and fails when median throughput or speedup regressions exceed configured thresholds.

`tools/check_fast_string_regression.py` runs both steps (capture candidate snapshot + compare to baseline) and is used by the optional CTest perf gate.

Set `TABLO_HASH_PROBE_STATS=1` to emit aggregate map/set probe statistics to `stderr` at process exit (useful for collision analysis during perf tuning).

To wire perf gates into CTest:

```bash
cmake -S . -B build -DENABLE_PERF_GATES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -R PerfGates --output-on-failure
# Use -V to stream progress output while the test runs:
ctest --test-dir build -C Release -R PerfGates -V
```

To wire the fast-string baseline regression gate into CTest:

```bash
python tools/benchmark_fast_string_baseline.py --config Release --samples 5 --warmup-runs 1 --out tools/baselines/fast_string_release.json
cmake -S . -B build -DENABLE_FAST_STRING_BASELINE_GATE=ON -DFAST_STRING_BASELINE_FILE=tools/baselines/fast_string_release.json
cmake --build build --config Release
ctest --test-dir build -C Release -R FastStringBaselineGate --output-on-failure
```

Optional threshold/tuning flags:
- `-DFAST_STRING_BASELINE_SAMPLES=3`
- `-DFAST_STRING_BASELINE_WARMUP_RUNS=1`
- `-DFAST_STRING_BASELINE_TRANSIENT_RETRIES=1`
- `-DFAST_STRING_MAX_METRIC_REGRESSION_PCT=8.0`
- `-DFAST_STRING_MAX_SPEEDUP_REGRESSION_PCT=8.0`

## Error Reporting

All errors are reported in the format:
```
<file>:<line>:<col>: <error message>
```

Example:
```
error.tblo:5:10: Type error: Cannot assign string to int variable
```

## Bytecode Format

Bytecode artifacts (`.tbc`) are treated as trusted input. The VM does not currently include a hardened bytecode verifier; running untrusted or corrupted artifacts may crash.

The compiler performs recursive constant folding for literal-only expressions (arithmetic/comparison/boolean/string concatenation), applies safe identity rewrites such as `x + 0`, `x * 1`, `flag && true`, and `flag || false`, and removes redundant no-op casts like `value as int` when `value` is already `int`.
Before bytecode lowering, a lightweight statement IR pipeline runs over the program root, outermost function bodies, nested block statements, and block-valued expression statement lists. The IR is segmented into explicit basic blocks with structured CFG edges for `if`/`match`/loop exits plus a fixed-point worklist over per-block IN/OUT facts instead of a single linear carry. Within a block, the reuse pass matches normalized pure expressions by value rather than raw syntax alone, so commutative forms such as `a + b` vs `b + a` and copy-equivalent aliases can be reused without crossing control-flow boundaries. Across blocks, the available-expression reuse pass preserves facts across compatible `if`/`else`, `match`, simple loop exits, invariant loop backedges, and branchy loop backedges whose `if`/`match`/`continue` paths all preserve the same pure value facts, while copy-propagation preserves aliases across compatible joins, branchy loop exits, loop backedges, and nested loops only when every reaching path keeps the same alias fact. Loop preheaders are modeled separately from loop headers, so backedge analysis no longer replays setup statements from before the loop. Unreachable blocks no longer contribute outgoing facts just because they appear later in source order. The pipeline still drops facts across effectful expression barriers, explicit writes, clobbering loop backedges, and incompatible branch exits. It prunes unreachable control-flow when conditions are compile-time booleans (including short-circuit forms like `false && expr` and `true || expr`, in addition to `if (false) { ... }` and `while (false) { ... }`), lowers `while(true)` loops whose body must `return` into straight-line returns, drops empty `foreach` loops for constant ranges and known-empty iterables (for example `foreach (i in 7..3) { ... }` and `foreach (x in []) { ... }`), resolves `match` statements with compile-time constant subjects (including enum constructor subjects with constant payload patterns and tag-incompatible payload-binding arms) to their selected arm, reuses repeated pure subexpressions across straight-line blocks, propagates local copy chains across straight-line statements, removes dead local stores when their right-hand side is side-effect-free for locals declared inside the current statement list (including nested block-local temporaries), drops statements that appear after guaranteed terminators (`return`, `break`, `continue`) within a block, and removes redundant branch-jump scaffolding when a taken `if` branch or `match` arm cannot fall through.

The VM uses a stack-based instruction set. Key opcodes include:

- `OP_CONST` - Push constant from pool
- `OP_LOAD_LOCAL`/`OP_STORE_LOCAL` - Access local variables
- `OP_LOAD_GLOBAL`/`OP_STORE_GLOBAL` - Access global variables
- `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD` - Arithmetic
- `OP_NEG` - Unary negation
- `OP_EQ`, `OP_NE`, `OP_LT`, `OP_LE`, `OP_GT`, `OP_GE` - Comparisons
- `OP_NOT`, `OP_AND`, `OP_OR` - Boolean operations
- `OP_JUMP`, `OP_JUMP_IF_FALSE` - Control flow
- `OP_CALL`, `OP_RET` - Function calls
- `OP_ARRAY_NEW`, `OP_ARRAY_GET`, `OP_ARRAY_SET`, `OP_ARRAY_LEN`, `OP_ARRAY_PUSH`, `OP_ARRAY_POP` - Array operations
- `OP_STRING_LEN`, `OP_STRING_CONCAT` - String operations
- `OP_TYPEOF`, `OP_CAST_INT`, `OP_CAST_DOUBLE`, `OP_CAST_STRING`, `OP_CAST_BIGINT` - Type operations
- `OP_RECORD_NEW`, `OP_RECORD_SET_FIELD`, `OP_RECORD_GET_FIELD` - Record operations
- `OP_TUPLE_NEW`, `OP_TUPLE_GET`, `OP_TUPLE_SET` - Tuple operations
- `OP_MAP_NEW`, `OP_MAP_SET`, `OP_MAP_GET`, `OP_MAP_HAS`, `OP_MAP_DELETE`, `OP_MAP_COUNT` - Map operations
- `OP_SET_NEW`, `OP_SET_ADD`, `OP_SET_HAS`, `OP_SET_REMOVE`, `OP_SET_COUNT`, `OP_SET_TO_ARRAY` - Set operations
- `OP_PRINT`, `OP_PRINTLN` - Output

## Memory Management

The VM uses reference counting for memory management, with a cycle collector for container objects:
- Strings, arrays, records, tuples, maps, and sets are heap objects
- BigInts are heap objects
- Each object maintains a reference count
- Objects are freed when the count reaches zero
- Cyclic data structures involving arrays/records/tuples/maps are reclaimed by the cycle collector
- You can force a cycle-collection pass with `gcCollect()`, which returns the number of reclaimed container objects

## Implementation Notes

- Written in C (C11 standard)
- Portable to Windows (MSVC) and Unix-like systems
- Built with CMake
- Test suite using CTest

## License

TabloLang is licensed under the MIT License. See `LICENSE`.
Third-party notices for vendored dependencies are in `THIRD_PARTY_NOTICES.md`.
