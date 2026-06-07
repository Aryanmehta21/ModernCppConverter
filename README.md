# ModernCppConverter

ModernCppConverter is a C++20 Qt 6 Widgets desktop application that modernizes legacy C++ code using a powerful offline rule-based engine.

It allows you to paste legacy C++ code, convert it, inspect the modernized output, review each change or suggestion, and read an explanation of the resulting code.

## Features

- Qt 6 Widgets desktop UI
- Input and output code editors
- Modernization Options panel with selectable C++11/C++14/C++17/C++20 features
- Detailed conversion change list
- Modern C++ explanation panel
- Offline-first rule-based modernization engine
- Lightweight structural analysis layer for preprocessor, type, class-resource, ownership, and loop rewrites
- Type-change tracking with dependent usage cleanup to prevent partially modernized code
- Compiler-error-driven cleanup for known fallout after type-changing rewrites
- Optional syntax-only compile verification for converted code
- Interface-driven converter architecture
- Prepared for future text, token, and AST converter rules
- Unit tests for converter behavior
- CMake and VS Code friendly layout
- Architecture prepared for future AI or Clang-based backends

## Offline Rule-Based Modernization

Offline Rule-Based mode is the primary feature. It runs locally, does not require the backend, does not use AI, and can directly apply deterministic modern C++17/C++20 transformations when the pattern is reasonably safe.

Automatic safe rewrites:

- `NULL` to `nullptr`
- Removal of legacy `NULL` and custom `nullptr` macro workaround blocks without leaving dangling preprocessor directives
- Simple numeric, string, character, and boolean object-like macros to `inline constexpr auto`
- C-style `typedef struct { ... } Name;` declarations to normal C++ `struct Name`
- Simple `typedef` declarations to `using`
- Simple old-style scalar casts to `static_cast`
- Simple local and member `char[]` text-buffer patterns using `std::strcpy`, `std::strncpy`, `std::strcmp`, or `std::strlen` to `std::string`
- Simple raw dynamic arrays with matching `delete[]` cleanup to `std::vector`
- Manual dynamic-array growth emulation to `std::vector::reserve`, `push_back`, and `size()` where safe
- Vector emulation elimination that removes temporary growth buffers, copy loops, raw-buffer assignments, and orphaned `delete[]` statements after vector modernization
- Orphaned growth symbol cleanup that removes stale temp-buffer/temp-capacity references reported by compile diagnostics
- Method-level vector append rewriting that turns append-style `vector[count]` writes into `push_back` and fixes reserve-vs-resize hazards
- Vector paradigm rewriting that coordinates growth elimination and append rewriting before compile verification, preventing declaration-only vector modernization
- Follow-up cleanup after type changes, including stale `new[]`, `delete[]`, nullptr checks, manual copy loops, C-string copies/comparisons, manual null termination, and invalid `sizeof(buffer)` usage
- Value-type pointer-operation scanning so converted `std::vector`, `std::string`, `std::array`, and similar symbols cannot keep `nullptr` checks or manual deletes
- Compiler-diagnostic cleanup for known leftover patterns such as `std::vector` compared with `nullptr` or `std::string` passed to C-string write APIs
- Rule of Zero cleanup after standard-library ownership/container modernization
- Simple iterator, const-iterator, and index loops to range-based for loops
- Simple local `new`/`delete` ownership pairs to `std::make_unique`
- Matching `delete` removal after RAII conversion
- Simple helper/functor patterns to lambdas
- Obvious repeated type construction to `auto`
- Simple compile-time constants/functions to `constexpr`
- Clear overriding declarations to `override`
- Unscoped enums to `enum class` only when all visible enumerator references can be updated safely and no integer-conversion dependency is detected
- Clear pair-like loops to structured bindings in Aggressive Safe mode
- Required includes such as `<memory>`, `<string>`, and `<string_view>` when a rule needs them

Suggestion-only diagnostics:

- Ambiguous old-style casts
- Ambiguous raw pointer ownership patterns
- Ambiguous raw pointer ownership, escaping pointers, returned pointers, and conditional deletes
- C-style arrays that look like binary buffers or C interop storage
- Function-like macros and ABI-sensitive typedefs
- Manual loops that may benefit from range-based loops
- `std::string_view` by default, unless safe auto-apply is explicitly enabled
- `std::optional`, unsafe enum-class migrations, move semantics, ranges, spans, and other cases that need more context

When uncertain, the converter preserves the original code and emits a suggestion.

## Modernization Options

The `Modern C++ Features` panel lets users choose which concepts are considered during conversion. The `Offline Modernization Level` selector controls how much deterministic rewriting the local converter attempts:

- Conservative: only very safe changes such as `nullptr`, `using`, simple ownership, and simple loops.
- Balanced: the default. Adds safe string modernization, lambdas, `auto`, `override`, and `constexpr`.
- Aggressive Safe: applies broader deterministic cleanup, including clear structured bindings and stronger local refactoring, while still preserving code when uncertain.
- AI-Style Aggressive Rewrite: runs the safe rules first, then an aggressive offline rewrite pass that can reshape local helper logic into lambdas, modernize simple loops into algorithms, use C++20 ranges when selected, and run compile verification automatically.

The `Offline Rewrite Style` selector makes the intent explicit:

- Safe Modernization: use the deterministic rule set selected by the modernization level.
- Aggressive AI-like Rewrite: run the shared `OfflineModernizationPipeline`, which analyzes local code structure, applies broad safe rewrites, manages includes, records changes, and runs syntax-only verification.

The `Apply safe raw pointer ownership modernization` option is enabled by default and only rewrites clear local ownership pairs with a matching plain `delete`. The `Apply safe std::string_view modernization` option is disabled by default because string lifetimes can be subtle.

The `Use std::format for simple stream formatting` option is disabled by default. When enabled for a C++20 target, it converts only simple `std::cout << "text" << value << std::endl` chains where an equivalent format string can be generated without preserving complex stream state.

The optional custom modernization instruction is stored in `ModernizationOptions` and displayed in the results. The rule-based engine does not use AI to interpret free-form text.

## Desktop Layout

The main window uses a laptop-friendly Qt layout:

- A top toolbar contains common actions: conversion mode, offline modernization level, Convert, Verify, Clear, Copy Output, and Check Backend.
- The central area is a tab widget with Code Converter, Repository Modernization, Options, and Logs / Diagnostics tabs.
- The Code Converter tab uses splitters so input/output editors and conversion details can be resized.
- The Options tab uses a scroll area so long C++ feature lists do not force the whole window taller than the screen.
- The status bar shows current mode, backend status, compile result, and last conversion source.

Startup sizing is screen-aware: the app targets a 1200x800 default window, caps itself to the available screen area, and uses a minimum size around laptop-friendly dimensions.

## Compile Verification

The app can verify converted code with a syntax-only compiler check. It writes the converted code to a temporary file, invokes an available compiler, captures output, and deletes the temporary file when finished.

Preferred command shape:

```sh
g++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only converted.cpp
```

The verifier can also use `clang++` with equivalent syntax-only flags or MSVC `cl /std:c++20 /EHsc /nologo /Zs converted.cpp`.

The app never executes user code and never runs a generated binary.

In AI-Style Aggressive Rewrite mode, compile verification runs automatically after conversion. If syntax verification fails and a missing standard include appears likely, the converter attempts one include-only auto-fix pass and verifies again.

For all offline structural conversions, declaration/type rewrites follow a no declaration-only modernization rule. If a type changes, the pipeline cascades dependent usage cleanup before verification. If syntax-only compile output still identifies a known leftover pattern, such as a converted `std::vector` being compared with `nullptr` or a converted `std::string` being passed to `strncpy`, the compiler-diagnostic cleanup pass runs once and the code is verified again.

## Repository Modernization Mode

Repository Modernization Mode lets you provide a public GitHub repository URL, clone it into a separate workspace, scan supported C++ files, apply the same offline structural modernization pipeline file by file, and generate reports.

Supported files:

- `.cpp`
- `.cc`
- `.cxx`
- `.hpp`
- `.h`
- `.hh`
- `.hxx`

Ignored folders:

- `.git/`
- `build/`
- `cmake-build-*/`
- `out/`
- `third_party/`
- `external/`
- `vendor/`
- `node_modules/`
- `.venv/`

Safety behavior:

- The app clones into a separate workspace folder, defaulting to `~/ModernCppConverterWorkspaces`.
- It does not modify your original local files.
- It creates `.legacy_backup` files before changing any scanned source/header file.
- It does not commit, push, change remotes, delete files, install dependencies, run project binaries, or store GitHub credentials.
- Public `https://github.com/...` clone URLs are supported for now.

Reports are written into the cloned repository folder:

- `modernization_report.txt`
- `modernization_report.json`

The report includes repository URL, branch, clone path, timestamp, modernization level, files scanned/modified/skipped, applied changes, suggestions, warnings, per-file changes, and syntax verification output.

Repository verification currently uses syntax-only per-file checks for safety. It does not run CMake configure scripts or project binaries.

## Conversion Modes

The application supports three modes:

- Offline Rule-Based: uses only the local converter and is the default primary mode.
- Online AI-Assisted: sends code to the configured backend service.
- Hybrid (Offline + AI Review): runs the local converter first, then sends the local result to the backend for review.

If the backend is unavailable, Online and Hybrid modes automatically fall back to Offline Rule-Based mode and show:

```text
AI backend unavailable. Falling back to Offline Mode.
```

The desktop app never asks users for AI provider keys. Provider keys belong only in the backend deployment. API billing is only relevant when a backend developer/deployer enables AI mode.

## Backend Startup

The backend is a FastAPI service. It uses `MockAiModernizationService` by default and can use OpenAI when explicitly enabled in the backend environment.

```sh
cd backend
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

The desktop backend URL is configured in `config/app_config.json`.

To enable OpenAI, the backend developer/deployer creates `backend/.env` manually and sets `OPENAI_API_KEY` there. End users never enter API keys in the desktop app.

For GitHub sharing and public distribution guidance, see `docs/github_distribution.md`.

## Requirements

- C++20 compiler
- CMake 3.21 or newer
- Qt 6 Widgets
- Ninja, recommended for the documented build commands

## Build

```sh
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Debug
```

## Run Tests

```sh
ctest --test-dir build -C Debug --output-on-failure
```

## Run Application

After building, run the `ModernCppConverter` executable from the build output directory for your platform and configuration.

Examples:

```sh
./build/Debug/ModernCppConverter
```

or on some generators/platforms:

```sh
./build/src/Debug/ModernCppConverter
```

## Project Structure

```text
ModernCppConverter/
├── AGENTS.md
├── README.md
├── CMakeLists.txt
├── .gitignore
├── docs/
│   └── architecture.md
├── backend/
│   ├── README.md
│   ├── requirements.txt
│   ├── .env.example
│   └── src/
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── MainWindow.h
│   │   ├── MainWindow.cpp
│   │   ├── ConversionCoordinator.h
│   │   └── ConversionCoordinator.cpp
│   ├── backend/
│   │   ├── IBackendClient.h
│   │   ├── BackendClient.h
│   │   ├── BackendClient.cpp
│   │   ├── BackendConfig.h
│   │   └── BackendConfig.cpp
│   ├── converter/
│   │   ├── CodeRepresentation.h
│   │   ├── CodeStructure.h
│   │   ├── ICodeStructureAnalyzer.h
│   │   ├── TokenBasedStructureAnalyzer.h
│   │   ├── TokenBasedStructureAnalyzer.cpp
│   │   ├── ClangAstStructureAnalyzer.h
│   │   ├── ClangAstStructureAnalyzer.cpp
│   │   ├── StructuralAnalyzers.h
│   │   ├── StructuralAnalyzers.cpp
│   │   ├── IncludeManager.h
│   │   ├── IncludeManager.cpp
│   │   ├── TransformationContext.h
│   │   ├── TransformationContext.cpp
│   │   ├── ImpactCascadingCleanupPass.h
│   │   ├── ImpactCascadingCleanupPass.cpp
│   │   ├── CompilerDiagnosticCleanupPass.h
│   │   ├── CompilerDiagnosticCleanupPass.cpp
│   │   ├── ValueTypePointerOperationScanner.h
│   │   ├── ValueTypePointerOperationScanner.cpp
│   │   ├── VectorAppendMethodRewritePass.h
│   │   ├── VectorAppendMethodRewritePass.cpp
│   │   ├── VectorEmulationEliminationPass.h
│   │   ├── VectorEmulationEliminationPass.cpp
│   │   ├── VectorGrowthEmulationCleanupPass.h
│   │   ├── VectorGrowthEmulationCleanupPass.cpp
│   │   ├── VectorParadigmRewritePass.h
│   │   ├── VectorParadigmRewritePass.cpp
│   │   ├── DependentUsageRewritePass.h
│   │   ├── DependentUsageRewritePass.cpp
│   │   ├── SafeReplacementEngine.h
│   │   ├── SafeReplacementEngine.cpp
│   │   ├── RawTextRepresentation.h
│   │   ├── RawTextRepresentation.cpp
│   │   ├── TokenRepresentation.h
│   │   ├── TokenRepresentation.cpp
│   │   ├── AstRepresentation.h
│   │   ├── AstRepresentation.cpp
│   │   ├── IConverterEngine.h
│   │   ├── IConversionRule.h
│   │   ├── IExplanationGenerator.h
│   │   ├── RuleBasedConverterEngine.h
│   │   ├── RuleBasedConverterEngine.cpp
│   │   ├── OfflineModernizationPipeline.h
│   │   ├── OfflineModernizationPipeline.cpp
│   │   ├── OrphanedGrowthSymbolCleanupPass.h
│   │   ├── OrphanedGrowthSymbolCleanupPass.cpp
│   │   ├── StructuralModernizationEngine.h
│   │   ├── StructuralModernizationEngine.cpp
│   │   ├── AggressiveRewriteEngine.h
│   │   ├── AggressiveRewriteEngine.cpp
│   │   ├── CompileVerifier.h
│   │   ├── CompileVerifier.cpp
│   │   ├── ModernCppExplanationGenerator.h
│   │   ├── ModernCppExplanationGenerator.cpp
│   │   ├── ModernCppConverter.h
│   │   ├── ModernCppConverter.cpp
│   │   ├── ConversionRule.h
│   │   └── ConversionRule.cpp
│   └── models/
│       ├── ConversionResult.h
│       ├── ConversionChange.h
│       └── ModernizationOptions.h
├── include/
│   └── ModernCppConverter/
└── tests/
    ├── CMakeLists.txt
    └── ConverterTests.cpp
```
