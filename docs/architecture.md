# Architecture

ModernCppConverter is split into a Qt presentation layer and a standalone conversion core. Offline Rule-Based mode is the primary feature. The presentation layer depends on interfaces, not concrete converter implementations, so optional AI and future Clang modernization backends can be introduced without changing GUI code.

Online AI-assisted support is optional. The desktop app talks only to a configurable backend service; it never talks directly to an AI provider and never stores provider keys.

## Layers

### GUI Layer

Files:

- `src/main.cpp`
- `src/app/MainWindow.h`
- `src/app/MainWindow.cpp`

Responsibilities:

- Own the Qt application window
- Collect input text
- Collect selected `ModernizationOptions`
- Call `ConversionCoordinator`
- Render modernized code, changes, explanation, and status messages
- Display syntax-only compile verification results
- Keep primary actions in a top toolbar and organize larger workflows into tabs sized for laptop screens

The GUI layer must not contain modernization rules and must not include concrete converter engines. `MainWindow` may build `ModernizationOptions` from checkbox state, but the converter decides whether a selected feature is applied, suggested, or skipped. `main.cpp` is the composition root that chooses the concrete implementation and injects it into `MainWindow`.

`ConversionCoordinator` handles conversion mode selection:

- Offline Rule-Based uses only `IConverterEngine`.
- Online AI-Assisted calls `IBackendClient`.
- Hybrid runs `IConverterEngine` first and then sends the local result to `IBackendClient`.

When the backend is unavailable or returns an error, the coordinator falls back to Offline Rule-Based mode.

The GUI exposes an Offline Modernization Level selector. It stores the selected value in `ModernizationOptions`; the GUI does not decide which transformations are safe.

The main window is organized around a `QToolBar` and central `QTabWidget`:

- Code Converter: input/output editors plus resizable details, explanation, and compile-verification tabs.
- Repository Modernization: clone, scan, modernize, and report controls.
- Options: scrollable modernization feature and advanced settings panels.
- Logs / Diagnostics: app logs, backend checks, compile status, and last conversion metadata.

Startup sizing uses the primary screen's available geometry so the default 1200x800 target is capped to the screen and centered. Long option lists live inside a `QScrollArea` instead of expanding the whole window.

### Backend Client Layer

Files:

- `src/backend/IBackendClient.h`
- `src/backend/BackendClient.h`
- `src/backend/BackendClient.cpp`
- `src/backend/BackendConfig.h`
- `src/backend/BackendConfig.cpp`
- `config/app_config.json`

Responsibilities:

- Load backend URL and timeout configuration.
- Perform health checks.
- Send conversion requests to the backend.
- Handle timeouts and network errors.
- Serialize and deserialize JSON.
- Keep provider secrets out of the desktop app.

### Repository Modernization Layer

Files:

- `src/repository/RepositoryCloneService.h`
- `src/repository/RepositoryCloneService.cpp`
- `src/repository/RepositoryScanner.h`
- `src/repository/RepositoryScanner.cpp`
- `src/repository/RepositoryBackupService.h`
- `src/repository/RepositoryBackupService.cpp`
- `src/repository/RepositoryModernizationService.h`
- `src/repository/RepositoryModernizationService.cpp`
- `src/repository/RepositoryReportWriter.h`
- `src/repository/RepositoryReportWriter.cpp`
- `src/repository/RepositoryVerificationService.h`
- `src/repository/RepositoryVerificationService.cpp`

Responsibilities:

- Validate public `https://github.com/...` URLs and safe branch names.
- Clone repositories with `git clone` using a `QProcess` argument list, not shell string concatenation.
- Refuse to overwrite existing clone folders unless the user explicitly confirms reuse of an existing git clone.
- Scan only supported C/C++ source and header files.
- Ignore `.git`, build folders, third-party/vendor/external dependency folders, Node and Python virtualenv folders.
- Create `.legacy_backup` files before writing modernized code.
- Reuse `IConverterEngine` for file-level modernization.
- Generate text and JSON reports.
- Perform syntax-only per-file verification without executing project binaries.

Repository mode never commits, pushes, modifies remotes, deletes files, stores GitHub credentials, runs install commands, or runs generated binaries.

### Converter Layer

Files:

- `src/converter/CodeRepresentation.h`
- `src/converter/CodeStructure.h`
- `src/converter/ICodeStructureAnalyzer.h`
- `src/converter/TokenBasedStructureAnalyzer.h`
- `src/converter/TokenBasedStructureAnalyzer.cpp`
- `src/converter/ClangAstStructureAnalyzer.h`
- `src/converter/ClangAstStructureAnalyzer.cpp`
- `src/converter/StructuralAnalyzers.h`
- `src/converter/StructuralAnalyzers.cpp`
- `src/converter/IncludeManager.h`
- `src/converter/IncludeManager.cpp`
- `src/converter/TransformationContext.h`
- `src/converter/TransformationContext.cpp`
- `src/converter/DependentUsageRewritePass.h`
- `src/converter/DependentUsageRewritePass.cpp`
- `src/converter/ImpactCascadingCleanupPass.h`
- `src/converter/ImpactCascadingCleanupPass.cpp`
- `src/converter/CompilerDiagnosticCleanupPass.h`
- `src/converter/CompilerDiagnosticCleanupPass.cpp`
- `src/converter/ValueTypePointerOperationScanner.h`
- `src/converter/ValueTypePointerOperationScanner.cpp`
- `src/converter/VectorAppendMethodRewritePass.h`
- `src/converter/VectorAppendMethodRewritePass.cpp`
- `src/converter/VectorEmulationEliminationPass.h`
- `src/converter/VectorEmulationEliminationPass.cpp`
- `src/converter/VectorGrowthEmulationCleanupPass.h`
- `src/converter/VectorGrowthEmulationCleanupPass.cpp`
- `src/converter/VectorParadigmRewritePass.h`
- `src/converter/VectorParadigmRewritePass.cpp`
- `src/converter/SafeReplacementEngine.h`
- `src/converter/SafeReplacementEngine.cpp`
- `src/converter/RawTextRepresentation.h`
- `src/converter/RawTextRepresentation.cpp`
- `src/converter/TokenRepresentation.h`
- `src/converter/TokenRepresentation.cpp`
- `src/converter/AstRepresentation.h`
- `src/converter/AstRepresentation.cpp`
- `src/converter/IConverterEngine.h`
- `src/converter/IConversionRule.h`
- `src/converter/IExplanationGenerator.h`
- `src/converter/RuleBasedConverterEngine.h`
- `src/converter/RuleBasedConverterEngine.cpp`
- `src/converter/OfflineModernizationPipeline.h`
- `src/converter/OfflineModernizationPipeline.cpp`
- `src/converter/OrphanedGrowthSymbolCleanupPass.h`
- `src/converter/OrphanedGrowthSymbolCleanupPass.cpp`
- `src/converter/StructuralModernizationEngine.h`
- `src/converter/StructuralModernizationEngine.cpp`
- `src/converter/AggressiveRewriteEngine.h`
- `src/converter/AggressiveRewriteEngine.cpp`
- `src/converter/CompileVerifier.h`
- `src/converter/CompileVerifier.cpp`
- `src/converter/ModernCppExplanationGenerator.h`
- `src/converter/ModernCppExplanationGenerator.cpp`
- `src/converter/ModernCppConverter.h`
- `src/converter/ModernCppConverter.cpp`
- `src/converter/ConversionRule.h`
- `src/converter/ConversionRule.cpp`

Responsibilities:

- Apply safe transformations
- Detect risky legacy patterns
- Emit suggestions when automatic conversion could break code
- Return a complete `ConversionResult`
- Track every applied transformation, unsafe suggestion, and disabled-option skip as a `ConversionChange`
- Syntax-check converted code with a temporary file when verification is requested

The key abstractions are:

- `IConverterEngine`: converts legacy code and `ModernizationOptions` into a `ConversionResult`.
- `CodeRepresentation`: exposes the code form a rule operates on.
- `IConversionRule`: applies one conservative rule or emits suggestions against a `CodeRepresentation`.
- `IExplanationGenerator`: generates human-readable explanation text from code and changes.
- `ICodeStructureAnalyzer`: discovers lightweight structural blocks such as preprocessor blocks, typedef structs, classes, and loops.

`RuleBasedConverterEngine` is the first `IConverterEngine` implementation. It composes a list of `IConversionRule` objects, then passes the safe-rule output through `OfflineModernizationPipeline`.

`OfflineModernizationPipeline` is the shared offline pipeline for pasted snippets, individual files, and repository modernization. It performs post-rule structural analysis, applies deterministic structural rewrites, applies aggressive AI-like deterministic rewrites when requested, runs impact cascading cleanup for every recorded type change, manages includes, records changes, and runs syntax-only compile verification.

Type-changing modernization follows a no declaration-only modernization rule: a declaration rewrite is not considered complete until incompatible dependent usages are rewritten, removed safely, or left with a warning instead of returning mixed legacy/modern code.

`TokenBasedStructureAnalyzer` is the current lightweight implementation of `ICodeStructureAnalyzer`. It is token/block oriented rather than a full parser: it balances braces and preprocessor pairs, discovers C-style typedef structs, finds class blocks, and extracts simple loop blocks. `ClangAstStructureAnalyzer` is a stub for future LibTooling integration and currently delegates to the token analyzer so callers can depend on the interface today.

`StructuralModernizationEngine` contains reusable structural passes for preprocessor cleanup, constant macro conversion, C-style typedef struct modernization, C-string buffer modernization, raw dynamic array to `std::vector`, safe unscoped enum to `enum class` conversion, optional simple `std::format` stream conversion, Rule of Zero cleanup after resource conversion, and safer iterator/index loop rewrites.

`TransformationContext` records declaration/type changes made by structural passes. Each `TypeChangeRecord` stores the symbol, old type, new type, scope/class context, whether it is a member, the rule that changed it, required follow-up cleanup actions, warnings, and verification status.

`ImpactCascadingCleanupPass` is the post-type-change safety gate. It consumes `TransformationContext` records and requires every declaration/type modernization to cascade into dependent usage cleanup before compile verification. It delegates concrete symbol rewrites to `DependentUsageRewritePass`, then records vector/string cascade actions for reports.

`ValueTypePointerOperationScanner` enforces pointer-to-value type transitions. For symbols recorded as converted to standard value types such as `std::vector`, `std::string`, `std::array`, or `std::optional`, it removes or rewrites invalid pointer-only operations such as `== nullptr`, `!= nullptr`, `= nullptr`, `delete`, and `delete[]` before compile verification. Cleanup-only blocks and empty destructors are removed under Rule of Zero.

`VectorParadigmRewritePass` is the top-level vector paradigm cleanup stage. It runs after raw-array-to-vector modernization and before compile verification. It coordinates vector emulation elimination and method-level append rewriting so converted vector storage no longer carries fragments of the old manual allocation/copy/delete growth system.

`OrphanedGrowthSymbolCleanupPass` removes stale temporary growth-buffer and temporary-capacity references after vector modernization. It runs before verification through `VectorParadigmRewritePass` and again during compiler-diagnostic cleanup when syntax-only compile output reports undeclared identifiers. It removes obsolete copy loops, raw-buffer assignments, capacity updates tied only to allocation growth, empty blocks, and self-assignment fallout instead of renaming missing variables.

`VectorEmulationEliminationPass` removes legacy dynamic-array emulation after raw arrays become `std::vector`. It delegates to `VectorGrowthEmulationCleanupPass`, which removes temporary raw growth buffers, manual element-copy loops, stale raw-pointer assignments, and growth-buffer `delete[]` calls, then rewrites clear indexed append/count patterns to `push_back`, `reserve`, and `size()`. The pass is context-driven by `TypeChangeRecord`s, so pasted-code conversion and repository modernization share the same behavior.

`VectorAppendMethodRewritePass` runs after vector emulation elimination. It handles method-level append fallout by rewriting remaining append-style `vector[count] = value` patterns to `push_back`, preserving logical maximum-capacity checks with `vector.size()`, and converting reserve-only initialization to resize when fixed indexed writes require constructed elements. If an indexed write cannot be proven safe, the invalid-leftover scanner reports it before verification.

`DependentUsageRewritePass` updates dependent usages for converted symbols, such as `new[]`/`delete[]`/nullptr cleanup after vector modernization, C-string API cleanup after `std::string` modernization, smart-pointer delete cleanup, Rule of Zero cleanup for obsolete special members, and invalid-leftover scanning before compile verification.

`CompilerDiagnosticCleanupPass` runs only after syntax verification fails and the compiler output matches a known fallout category from a recorded type change. It uses the compiler diagnostics plus `TypeChangeRecord`s to trigger one more targeted dependency cleanup for cases such as `std::vector`/`nullptr` comparisons, `std::string` passed to `strncpy`/`strcpy`, leftover `new[]`/`delete[]`, and manual smart-pointer deletion.

`SafeReplacementEngine` performs line-aware rewrites that avoid inserting generated code into comments, block comments, or preprocessor lines. It also preserves trailing line comments separately from generated replacement code where possible.

`IncludeManager` centralizes adding missing standard library includes and removing includes only when their corresponding APIs are no longer used.

`AggressiveRewriteEngine` contains reusable aggressive rewrite passes. These passes are category-oriented rather than sample-specific: local computation blocks, small helper functions used once, simple predicate functors, and obvious loop-to-algorithm rewrites.

`CompileVerifier` performs syntax-only compile verification with `g++`, `clang++`, or `cl` when available. It never executes user code, never links or runs binaries, and only writes temporary files.

`ModernCppExplanationGenerator` explains the result in beginner-friendly sections: summary of changes, modern C++ concepts used, and suggested future improvements.

`ModernCppConverter` remains as a small compatibility facade over `IConverterEngine`; new GUI code should depend on `IConverterEngine` directly.

## Modernization Options

`ModernizationOptions` stores the selected C++ modernization features, offline modernization level, compile-verification preference, and an optional custom instruction. It is a standard-library-only model so future command-line, AI, Clang, or batch engines can reuse it without Qt dependencies.

The current rule-based engine treats options as follows:

- Conservative mode applies only very safe rewrites such as `NULL` to `nullptr`, simple `typedef` to `using`, simple ownership conversion, and simple loop modernization.
- Balanced mode is the default and adds safe string modernization, simple lambdas, obvious `auto`, clear `override`, and simple `constexpr`.
- Aggressive Safe mode applies broader deterministic cleanup such as clear structured bindings and stronger local refactoring, but still emits suggestions when uncertain.
- AI-Style Aggressive Rewrite mode runs existing safe rules first and then applies `OfflineModernizationPipeline` for local helper lambdas, predicate functor removal, algorithm/ranges modernization, and include management. It runs syntax-only compile verification automatically and attempts one missing-include auto-fix pass when verification fails.
- `OfflineRewriteStyle` lets the UI explicitly request Safe Modernization or Aggressive AI-like Rewrite without changing AI/backend availability.
- Clear local `new`/`delete` pairs can be rewritten to `std::make_unique` when `applySafeOwnershipModernization` is enabled.
- `std::string_view` can be applied for simple read-only function parameters only when `applyStringViewWhenSafe` is enabled.
- Higher-risk selected rules emit suggestions when semantic context is insufficient.
- Disabled rules that match code emit skipped entries so users can see what was intentionally not considered.
- `customInstruction` is recorded and displayed, but no AI-based free-form interpretation is performed.

## Code Representations

The converter layer is prepared for multiple views of the same source code:

- `RawTextRepresentation`: stores mutable source text. The current rule-based engine uses this representation for regex and line-oriented conservative rewrites.
- `TokenRepresentation`: stores source text plus a token sequence. Future token rules can reason about lexical structure without needing a full AST.
- `AstRepresentation`: stores source text plus AST placeholder metadata. Future Clang LibTooling integration can replace the placeholder metadata with semantic AST handles while preserving the converter-facing abstraction.

Rules receive `CodeRepresentation&` instead of a raw `std::string&`. A rule can remain representation-specific, such as a text rule that reads and replaces source text, or a future rule can inspect token or AST representations. This lets the project add `ClangAstConverterEngine` or `LLVMToolingConverterEngine` without changing `MainWindow`.

No Clang headers, libraries, or build requirements are introduced yet.

## Structural Analysis Layer

The offline pipeline now has a lightweight structure pass before transformations. It is intentionally not a full C++ parser, but it avoids whole-file keyword replacement by operating on discovered blocks:

- `PreprocessorAnalyzer` behavior is implemented in `TokenBasedStructureAnalyzer` and `StructuralModernizationEngine`: obsolete NULL/nullptr workaround blocks are removed as whole blocks, empty conditionals are cleaned up, simple constant macros become `inline constexpr auto`, and balance validation prevents dangling `#endif` lines introduced by cleanup.
- `TypeDeclarationAnalyzer` behavior detects C-style `typedef struct { ... } Name;` declarations and rewrites them to normal C++ `struct Name { ... };` when the body is simple.
- `ClassResourceAnalyzer` and `OwnershipAnalyzer` behavior modernizes clear ownership patterns such as raw pointer members, dynamic array members, constructor allocation, destructor cleanup, and aliasing risk.
- `LoopAnalyzer` behavior rewrites structurally simple iterator and size-index loops while preserving constness and avoiding erase/mutating-iterator cases.
- `IncludeManager` applies required includes for standard library types introduced by structural rewrites.
- `TransformationContext`, `ImpactCascadingCleanupPass`, `ValueTypePointerOperationScanner`, `VectorParadigmRewritePass`, `OrphanedGrowthSymbolCleanupPass`, `VectorEmulationEliminationPass`, `VectorAppendMethodRewritePass`, `DependentUsageRewritePass`, and `CompilerDiagnosticCleanupPass` make declaration changes dependency-aware, so type modernization does not leave stale cleanup, manual vector-growth emulation, orphaned temp identifiers, unsafe reserved-vector indexing, or pointer/string API usages behind. If compile verification still reports a known fallout pattern, the compiler-diagnostic pass runs once before the final verification result is returned.

Limitations remain deliberate until Clang LibTooling is integrated: overload resolution, macro expansion, cross-translation-unit ownership, templates with complex dependent names, custom allocators, ABI-sensitive declarations, and public API compatibility are not fully knowable from the token analyzer. In those cases the converter should preserve code and emit a suggestion.

## Conversion Tracking

Every converter engine returns a `ConversionResult` with a chronological list of `ConversionChange` entries. Each entry records:

- Rule name
- Original code snippet
- Transformed code snippet, when a safe rewrite was applied
- Reason
- Applied status
- Skipped status

Unsafe or uncertain transformations must leave `modernCode` unchanged for that item and must create a suggestion entry with `applied = false` and `skipped = false`. Disabled matching rules must create a skipped entry with `applied = false` and `skipped = true`.

### Model Layer

Files:

- `src/models/ConversionChange.h`
- `src/models/ConversionResult.h`
- `src/models/ModernizationOptions.h`
- `src/models/RepositoryModernizationModels.h`

Responsibilities:

- Store converter output
- Store selected modernization features
- Keep the converter independent from Qt types
- Store repository modernization options, per-file results, and report summaries

The models use standard library types only.

## Current Rule Strategy

The offline engine is deterministic and applies direct transformations where local text patterns are clear:

- `NULL` is converted to `nullptr` because this is normally safe in modern C++.
- Legacy `NULL` and custom `nullptr` macro workarounds are removed when the nullptr option is enabled.
- Empty preprocessor blocks left by macro cleanup are removed and preprocessor balance is validated.
- Simple object-like numeric/string/boolean macros can become `inline constexpr auto` values when constexpr modernization is enabled.
- C-style `typedef struct` declarations are converted to normal C++ struct declarations when safe.
- Simple `typedef` declarations are converted to `using` when the pattern is straightforward.
- Simple local and member `char[]` text-buffer patterns using C-string copy/compare APIs are converted to `std::string`.
- Simple local and member raw dynamic arrays with matching `delete[]` cleanup are converted to `std::vector`.
- Dependent cleanup rewrites stale allocation, deallocation, null-check, manual copy, and manual null-termination logic after type changes.
- Cleanup-only destructors are removed after standard-library ownership conversion to prefer Rule of Zero.
- Simple iterator, const-iterator, and index-based loops are converted to range-based for loops when the iterator/index only selects the current element.
- Simple local raw pointer ownership pairs are converted to `std::make_unique` when safe ownership modernization is enabled.
- Matching manual `delete` statements are removed after RAII conversion.
- Simple scalar old-style casts are converted to `static_cast`.
- Simple helper computations and stateless functors can be converted to lambdas.
- AI-Style Aggressive Rewrite mode can move small helper functions used once into local lambdas.
- AI-Style Aggressive Rewrite mode can turn simple output loops into `std::ranges::for_each` for C++20 or `std::for_each` for C++17.
- AI-Style Aggressive Rewrite mode can inline stateless predicate functors into `std::ranges::count_if` or `std::count_if`.
- Obvious repeated construction can use `auto`.
- Simple compile-time constants/functions can use `constexpr`.
- Clear overriding declarations can receive `override`.
- Clear pair-like loops can use structured bindings in Aggressive Safe mode.
- Ambiguous casts, escaping ownership, C-style arrays, plain enum migrations, nullable returns, move semantics, and risky string-view cases remain suggestion-only.

## Compile Verification

Compile verification is optional and syntax-only. The verifier tries:

```sh
g++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only converted.cpp
```

It can also use `clang++` with equivalent flags or MSVC `cl /std:c++20 /EHsc /nologo /Zs converted.cpp`. The app never executes user code and never runs generated binaries.

## Future Extension Points

Future engines should implement `IConverterEngine` and return the same `ConversionResult` model:

- `RuleBasedConverterEngine`
- `ClangAstConverterEngine`
- `LLVMToolingConverterEngine`
- `AiAssistedConverterEngine`

Because `MainWindow` receives `std::unique_ptr<IConverterEngine>`, these engines can be swapped in the composition root without changing presentation logic.

### Clang AST Backend

`ClangAstConverterEngine` can construct an `AstRepresentation` and use semantic AST checks to make safer rewrites than text-based rules. It should still preserve the same `IConverterEngine` contract and emit structured `ConversionChange` entries.

### LLVM Tooling Backend

`LLVMToolingConverterEngine` can integrate with clang-tidy-style checks, fix-it hints, token streams, and formatting. It should remain isolated from the GUI and model Qt types out of the conversion layer.

### AI Backend

AI assistance is represented by the backend service and `IBackendClient`. It is optional and does not replace Offline Rule-Based mode. Real provider integration belongs server-side only, and API billing is only relevant when a backend developer/deployer enables AI mode.

Security recommendations before a production AI backend:

- Store AI provider keys only on the backend.
- Add authentication between desktop clients and backend.
- Add rate limiting.
- Validate request sizes and payloads.
- Deploy over HTTPS.

### Formatting

Code formatting can be added as a post-processing step. A future formatter should be optional and should not obscure the converter's recorded changes.

## SOLID Boundaries

- Single Responsibility: `MainWindow` renders UI, converter engines convert code, explanation generators write explanations.
- Open/Closed: new engines and rules can be added through interfaces.
- Liskov Substitution: any `IConverterEngine` can be injected into the GUI if it honors the result contract.
- Interface Segregation: code representations, rules, engines, and explanation generation are separate interfaces.
- Dependency Inversion: high-level GUI code depends on `IConverterEngine`, not `RuleBasedConverterEngine`.
