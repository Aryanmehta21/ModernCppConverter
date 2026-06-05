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

## Build

```sh
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Debug
```

## Test

```sh
ctest --test-dir build -C Debug --output-on-failure
```
