# ModernCppConverter Agents Guide

## Project Purpose

ModernCppConverter is a C++20 Qt 6 Widgets desktop application for converting legacy C++ snippets into safer C++17/C++20-style code.

The current implementation is intentionally rule-based. It does not call AI APIs and does not depend on Clang, but the converter architecture is split so those backends can be added later.

## Development Rules

- Keep GUI code inside `src/app`.
- Keep modernization logic inside `src/converter`.
- Keep shared result models inside `src/models`.
- Do not add modernization logic to `MainWindow`.
- Keep GUI dependencies on converter interfaces such as `IConverterEngine`.
- Add future backends as `IConverterEngine` implementations instead of changing GUI code.
- Keep text, token, and AST modernization concerns behind `CodeRepresentation`.
- Keep feature selection data in `ModernizationOptions`; the GUI may collect options but must not decide modernization behavior.
- Prefer conservative transformations. If a rule is uncertain, preserve the original code and emit a suggestion.
- Avoid unnecessary dependencies. Qt is only required for the desktop GUI.

## Always Apply Generic Modernization Guardrails

For every future task, automatically follow these rules even if the user prompt is short:

- Do not hardcode solutions for a specific test file.
- Do not target specific class names, function names, variable names, comments, or exact snippets.
- Build reusable modernization passes.
- Prefer general root-cause fixes over one-off patches.
- Pasted-code mode and Repository mode must use the same modernization pipeline.
- Offline mode is the primary feature.
- AI mode is optional and must never be required.
- Do not break existing GUI, tests, backend, repo mode, or offline conversion.
- Do not perform declaration-only modernization.
- When a type changes, all dependent usages must be updated or the transformation must be rolled back.
- Run invalid-leftover detection before compile verification.
- Run syntax-only compile verification after conversion where compiler exists.
- Never execute user code.
- Add or update tests for every modernization fix.
- Do not claim success unless build/tests and compile verification pass where available.

## Type Change Enforcement Rule

If a symbol changes from a pointer-like type to a value-type standard library object (`std::vector`, `std::string`, `std::array`, etc.), the converter must remove or rewrite all pointer-specific operations before returning converted code.

Examples of invalid leftovers:

- `symbol == nullptr`
- `symbol != nullptr`
- `symbol = nullptr`
- `delete symbol`
- `delete[] symbol`

Converted code must never contain these operations for value-type symbols.

If any remain:

- automatically fix them,
- or roll back the transformation,
- or fail compile verification.

Do not return known-invalid code.

## Enterprise Semantic Modernization Rules

For Phase 4 and all future modernization work:

- Never modernize only part of an ownership graph.
- Never modernize a container without modernizing all interactions with that container.
- Never convert storage types without propagating API changes.
- Never introduce `std::unique_ptr` or `std::shared_ptr` without ownership validation.
- Every polymorphic hierarchy must be validated.
- Every transformation must pass semantic validation.
- Prefer Rule of Zero whenever ownership becomes automatic.
- Prefer STL algorithms over manual loops when semantics are equivalent.
- Prefer range-based loops over iterator loops when safe.
- Reject partial refactors.

## Build

```sh
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Debug
```

## Test

```sh
ctest --test-dir build -C Debug --output-on-failure
```
