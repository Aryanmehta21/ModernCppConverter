# Roadmap

## Phase 1: Rule-Based Modernization

Goal: provide conservative, deterministic modernization for common legacy C++ patterns.

Technical recommendations:

- Keep rules small, isolated, and covered by unit tests.
- Prefer safe rewrites such as `NULL` to `nullptr` and simple `typedef` to `using`.
- Emit suggestion-only changes for ambiguous transformations.
- Track every applied change and suggestion in `ConversionResult`.
- Build a regression corpus of legacy snippets before expanding rule coverage.

## Phase 2: Token-Based Modernization

Goal: improve accuracy by operating on lexical tokens rather than raw text.

Technical recommendations:

- Implement a tokenizer that preserves source locations and comments.
- Add token-aware rules for casts, aliases, arrays, and loop patterns.
- Keep token rules behind `IConversionRule` and `CodeRepresentation`.
- Ensure rewritten output preserves formatting where possible.
- Add tests for comments, string literals, macros, and unusual whitespace.

## Phase 3: Clang AST Modernization

Goal: use semantic analysis to perform safer, context-aware rewrites.

Technical recommendations:

- Introduce Clang LibTooling in a separate backend target.
- Implement `ClangAstConverterEngine` without changing GUI code.
- Use AST matchers for ownership, loop, cast, and type-alias detection.
- Preserve `ConversionResult` as the public contract.
- Keep Clang-specific types out of the Qt layer and generic models.

## Phase 4: AI-Assisted Modernization

Goal: assist with explanation, migration planning, and complex refactoring suggestions.

Technical recommendations:

- Keep AI support optional and behind `IConverterEngine` or a dedicated advisory interface.
- Never apply AI-generated rewrites without structured validation.
- Pair AI suggestions with rule, token, or AST checks before modifying code.
- Record confidence, rationale, and validation status for each suggestion.
- Provide offline rule-based behavior when AI support is unavailable.

## Phase 5: Large-Scale Legacy Code Migration

Goal: support project-wide modernization workflows for real codebases.

Technical recommendations:

- Add batch processing for files, directories, and compile databases.
- Integrate formatting, diff generation, and review workflows.
- Support migration reports with risk levels and rule summaries.
- Add performance benchmarks for large repositories.
- Provide CI-friendly command-line tooling that shares the same converter core.
