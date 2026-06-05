# Architecture

ModernCppConverter is split into a Qt presentation layer and a standalone conversion core. The presentation layer depends on interfaces, not concrete converter implementations, so new modernization backends can be introduced without changing GUI code.

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

The GUI layer must not contain modernization rules and must not include concrete converter engines. `MainWindow` may build `ModernizationOptions` from checkbox state, but the converter decides whether a selected feature is applied, suggested, or skipped. `main.cpp` is the composition root that chooses the concrete implementation and injects it into `MainWindow`.

`ConversionCoordinator` handles conversion mode selection:

- Offline Rule-Based uses only `IConverterEngine`.
- Online AI-Assisted calls `IBackendClient`.
- Hybrid runs `IConverterEngine` first and then sends the local result to `IBackendClient`.

When the backend is unavailable or returns an error, the coordinator falls back to Offline Rule-Based mode.

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

### Converter Layer

Files:

- `src/converter/CodeRepresentation.h`
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

The key abstractions are:

- `IConverterEngine`: converts legacy code and `ModernizationOptions` into a `ConversionResult`.
- `CodeRepresentation`: exposes the code form a rule operates on.
- `IConversionRule`: applies one conservative rule or emits suggestions against a `CodeRepresentation`.
- `IExplanationGenerator`: generates human-readable explanation text from code and changes.

`RuleBasedConverterEngine` is the first `IConverterEngine` implementation. It composes a list of `IConversionRule` objects and the default `ModernCppExplanationGenerator`.

`ModernCppExplanationGenerator` explains the result in beginner-friendly sections: summary of changes, modern C++ concepts used, and suggested future improvements.

`ModernCppConverter` remains as a small compatibility facade over `IConverterEngine`; new GUI code should depend on `IConverterEngine` directly.

## Modernization Options

`ModernizationOptions` stores the selected C++ modernization features and an optional custom instruction. It is a standard-library-only model so future command-line, AI, Clang, or batch engines can reuse it without Qt dependencies.

The current rule-based engine treats options as follows:

- Low-risk selected rules can rewrite code automatically, such as `NULL` to `nullptr` and simple `typedef` to `using`.
- Clear local `new`/`delete` pairs can be rewritten to `std::make_unique` when `applySafeOwnershipModernization` is enabled.
- `std::string_view` can be applied for simple read-only function parameters only when `applyStringViewWhenSafe` is enabled.
- Higher-risk selected rules emit suggestions only.
- Disabled rules that match code emit skipped entries so users can see what was intentionally not considered.
- `customInstruction` is recorded and displayed, but no AI-based free-form interpretation is performed.

## Code Representations

The converter layer is prepared for multiple views of the same source code:

- `RawTextRepresentation`: stores mutable source text. The current rule-based engine uses this representation for regex and line-oriented conservative rewrites.
- `TokenRepresentation`: stores source text plus a token sequence. Future token rules can reason about lexical structure without needing a full AST.
- `AstRepresentation`: stores source text plus AST placeholder metadata. Future Clang LibTooling integration can replace the placeholder metadata with semantic AST handles while preserving the converter-facing abstraction.

Rules receive `CodeRepresentation&` instead of a raw `std::string&`. A rule can remain representation-specific, such as a text rule that reads and replaces source text, or a future rule can inspect token or AST representations. This lets the project add `ClangAstConverterEngine` or `LLVMToolingConverterEngine` without changing `MainWindow`.

No Clang headers, libraries, or build requirements are introduced yet.

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

Responsibilities:

- Store converter output
- Store selected modernization features
- Keep the converter independent from Qt types

The models use standard library types only.

## Current Rule Strategy

The first version is conservative:

- `NULL` is converted to `nullptr` because this is normally safe in modern C++.
- Legacy `NULL` and custom `nullptr` macro workarounds are removed when the nullptr option is enabled.
- Simple `typedef` declarations are converted to `using` when the pattern is straightforward.
- Simple `char[]` plus `std::strncpy` string-buffer patterns are converted to `std::string`.
- Simple iterator and index-based printing loops are converted to range-based for loops.
- Simple local raw pointer ownership pairs are converted to `std::make_unique` when safe ownership modernization is enabled.
- Old-style casts, raw pointer ownership patterns, C-style arrays, manual index loops, plain enums, nullable returns, and read-only string parameters are detected but not rewritten automatically.

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

AI assistance is represented today by the backend service and `IBackendClient`. The current backend uses `MockAiModernizationService`; no real AI provider is integrated yet. Future real provider integration belongs server-side only.

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
