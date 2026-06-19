# v1.0 Regression Test Plan

## Purpose

This plan defines the v1.0 regression pack for the Legacy-to-Modern C++ Converter. The pack protects the major correctness fixes completed before the v1.0 stabilization point: ownership cleanup, type propagation, C-string cleanup, enum propagation, FILE I/O RAII, loop modernization, map/pair safety, string-view safety, repository mode, and GUI worker lifecycle behavior.

The test pack is intentionally fragment-based. It checks important output properties and syntax-only compile verification instead of requiring exact full-file output, so harmless formatting changes do not destabilize release tests.

## How To Run

```sh
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Run only the v1.0 pack:

```sh
./build/Debug/V1RegressionTests
```

GUI worker lifecycle coverage remains in:

```sh
./build/Debug/GuiConversionSmokeTests
```

Backend tests, when the Python environment is available:

```sh
pytest backend/tests
```

## Test Categories

| Category | Test Coverage | Protected Behavior |
| --- | --- | --- |
| Raw dynamic array to `std::vector` | `V1RegressionTests::testRawDynamicArrayToVector` | Converts heap arrays, removes manual growth fragments, rewrites append logic to `push_back`, and uses `vector.size()`. |
| `char*` / `char[]` to `std::string` | `testCharBuffersBecomeStringsAndCapiCleanup` | Converts clear text ownership to `std::string`, preserves `c_str()` compatibility accessors, and removes manual `new char[]` / `delete[]`. |
| C-string API cleanup | `testCharBuffersBecomeStringsAndCapiCleanup` | Rewrites invalid `strcpy`, `strcat`, `strlen`, and `strcmp` use after string modernization. |
| `new` / `delete` to `std::unique_ptr` | `testUniquePtrOwnershipAndObserverGet` | Converts clear single ownership to `std::make_unique` and removes manual `delete`. |
| Observer pointer preservation | `testUniquePtrOwnershipAndObserverGet` | Keeps non-owning aliases as raw pointers and initializes/passes them with `.get()` when the owner becomes a smart pointer. |
| Enum to `enum class` propagation | `testScopedEnumPropagationAndOutput` | Qualifies enum labels in returns, comparisons, and assignments. Switch/case-specific behavior remains covered by the scoped enum suite. |
| Enum class output/formatting | `testScopedEnumPropagationAndOutput` | Casts scoped enum values for stream and format output without casting switch labels. |
| `FILE*` to `std::ofstream` | `testFileIoRangeLoopsAndMapPairSafety` | Converts simple text-write `FILE*` blocks, rewrites stream state checks, removes `fprintf`/`fclose` artifacts. |
| Range-based loop modernization | `testFileIoRangeLoopsAndMapPairSafety` | Converts safe explicit iterator loops to range-for while preserving read-only semantics. |
| Map structured bindings | `testFileIoRangeLoopsAndMapPairSafety` | Converts map pair access (`item.first` / `item.second`) to structured bindings. |
| `std::pair` streaming safety | `testFileIoRangeLoopsAndMapPairSafety` | Prevents generic print helpers from streaming `std::pair` directly when instantiated with map-like containers. |
| `std::string_view` safety | `testStringViewSafety` | Applies `string_view` only when safe and preserves `std::string` for null-terminated C API boundaries. |
| Clear button / timeout worker cancellation | `GuiConversionSmokeTests` | Verifies Clear cancels/releases active work and timeout does not leave stale workers blocking future conversions. |
| Repository mode smoke | `testRepositoryModeSmoke` | Runs the same converter through repository mode, writes reports, and preserves diagnostics. |

## Per-Test Expectations

Each v1.0 converter test includes:

- legacy input source
- important expected output fragments
- compile verification expectation when a compiler is available
- rollback or warning expectation where the safe behavior is to preserve original code

Examples:

- `std::string_view` unsafe C API usage must remain `const std::string&` or otherwise keep a valid null-terminated boundary.
- Generic `printValues(vector)` must still print elements directly.
- Generic `printValues(map)` must not emit `std::cout << std::pair`.
- Repository mode must report diagnostics and write both text and JSON reports.

## Known Limitations Not Covered

- Full Clang AST equivalence checking is not part of v1.0.
- Complex template metaprogramming and SFINAE-heavy code are not exhaustively validated.
- Complex macros with side effects, token pasting, stringification, or platform compiler tricks are not auto-modernized.
- Binary or complex `FILE*` usage is intentionally suggestion-only.
- Deep cross-file semantic propagation is smoke-tested but not exhaustively proven without a full build graph.
- Runtime behavior is not executed; tests use syntax-only compile verification and output-property assertions.
- The GUI smoke tests validate worker lifecycle and responsiveness, but they do not exhaustively test every visual control state.
