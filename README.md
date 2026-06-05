# ModernCppConverter

ModernCppConverter is a C++20 Qt 6 Widgets desktop application that modernizes legacy C++ code using a conservative, rule-based engine.

It allows you to paste legacy C++ code, convert it, inspect the modernized output, review each change or suggestion, and read an explanation of the resulting code.

## Features

- Qt 6 Widgets desktop UI
- Input and output code editors
- Modernization Options panel with selectable C++11/C++14/C++17/C++20 features
- Detailed conversion change list
- Modern C++ explanation panel
- Conservative rule-based modernization engine
- Interface-driven converter architecture
- Prepared for future text, token, and AST converter rules
- Unit tests for converter behavior
- CMake and VS Code friendly layout
- Architecture prepared for future AI or Clang-based backends

## Initial Rules

Automatic safe rewrites:

- `NULL` to `nullptr`
- Removal of legacy `NULL` and custom `nullptr` macro workarounds
- Simple `typedef` declarations to `using`
- Simple `char[]` plus `std::strncpy` string-buffer patterns to `std::string`
- Simple iterator and index printing loops to range-based for loops
- Simple local `new`/`delete` ownership pairs to `std::make_unique`

Suggestion-only diagnostics:

- Old-style casts
- Raw pointer ownership patterns
- Ambiguous raw pointer ownership, escaping pointers, returned pointers, and conditional deletes
- C-style arrays
- Manual loops that may benefit from range-based loops
- `std::string_view` by default, unless safe auto-apply is explicitly enabled

When uncertain, the converter preserves the original code and emits a suggestion.

## Modernization Options

The `Modern C++ Features` panel lets users choose which concepts are considered during conversion.

Low-risk transformations can be applied automatically:

- `NULL` to `nullptr`
- simple `typedef` declarations to `using`

Higher-risk items produce suggestions instead of changing code, including smart pointers, range-based loops, enum classes, `std::optional`, `std::string_view`, spans, ranges, concepts, and other advanced features.

The `Apply safe raw pointer ownership modernization` option is enabled by default and only rewrites clear local ownership pairs with a matching plain `delete`. The `Apply safe std::string_view modernization` option is disabled by default because string lifetimes can be subtle.

The optional custom modernization instruction is stored in `ModernizationOptions` and displayed in the results. The current rule-based engine does not interpret it with AI.

## Conversion Modes

The application supports three modes:

- Offline Rule-Based: uses only the local converter and is the default.
- Online AI-Assisted: sends code to the configured backend service.
- Hybrid (Offline + AI Review): runs the local converter first, then sends the local result to the backend for review.

If the backend is unavailable, Online and Hybrid modes automatically fall back to Offline Rule-Based mode and show:

```text
AI backend unavailable. Falling back to Offline Mode.
```

The desktop app never asks users for AI provider keys. Provider keys belong only in the backend deployment.

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
