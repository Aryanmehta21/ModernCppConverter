# ModernCppConverter

ModernCppConverter is a Qt 6 desktop application that converts legacy C++ snippets and repositories into safer C++17/C++20-style code using an offline-first rule-based modernization engine.

The project focuses on practical refactoring safety: transformations are conservative, dependent usages are cleaned up after type changes, and syntax-only compile verification is available so generated code is not returned blindly.

## Features

- Qt 6 Widgets desktop app with input/output C++ code editors.
- Offline rule-based modernization engine.
- Optional online and hybrid backend-assisted modes.
- Repository modernization mode for public GitHub repositories.
- C++17/C++20 target selection.
- Syntax-only compile verification with `g++`, `clang++`, or MSVC `cl`.
- Detailed change list, explanations, diagnostics, and compile output.
- Type-change tracking with cleanup and semantic validation passes.
- Rule of Zero cleanup after safe ownership modernization.
- Laptop-friendly tabbed UI with scrollable options and diagnostics.

Supported modernization categories include:

- `NULL` to `nullptr`
- simple macro constants to `constexpr`
- `typedef` cleanup
- unscoped enum to `enum class` when safe
- raw owning pointers to smart pointers
- raw dynamic arrays to `std::vector`
- owned C-string buffers to `std::string`
- C-string API cleanup after string modernization
- `malloc`/`free` ownership cleanup where safe
- cleanup-only destructor/copy boilerplate removal
- polymorphic virtual destructor and `override` fixes
- simple iterator/index loop modernization
- simple functor-to-lambda conversion
- safe `printf` and `FILE*` modernization suggestions/conversions

## Screenshots

<img width="1207" height="818" alt="Screenshot 2026-06-17 at 8 32 12 PM" src="https://github.com/user-attachments/assets/432864b8-ad29-4048-8691-bfeecb96bcc3" />
- Code Converter tab

  <img width="1470" height="956" alt="image" src="https://github.com/user-attachments/assets/ba91d561-d555-413c-9f9b-9ec395097588" />
- Options tab
  <img width="1470" height="956" alt="image" src="https://github.com/user-attachments/assets/d590104b-efa5-40d6-a4a3-d06852fec9e5" />
- Repository Modernization tab
  <img width="1328" height="222" alt="image" src="https://github.com/user-attachments/assets/3a506096-92ab-407c-9312-d0934dc84a34" />
- Compile Verification and Diagnostics tab

## Tech Stack

- C++20
- Qt 6 Widgets, Network, and Concurrent
- CMake 3.21+
- Ninja Multi-Config, recommended
- Optional Python FastAPI backend
- CTest for C++ tests
- Pytest for backend tests

## Requirements

- CMake 3.21 or newer
- A C++20-capable compiler
- Qt 6 with Widgets, Network, and Concurrent components
- Ninja, Make, Visual Studio, or another CMake-supported generator
- Optional: Python 3.10+ for backend development

## Build

Configure:

```sh
cmake -S . -B build -G "Ninja Multi-Config"
```

Build Debug from the repository root:

```sh
cmake --build build --config Debug
```

Or build from inside the configured build directory:

```sh
cd build
cmake --build . --config Debug
```

The same project should build on macOS, Linux, and Windows when Qt 6 and a supported compiler are available.

## Running the App

After building, run the generated `ModernCppConverter` executable from the build tree.

Common macOS/Linux Ninja Multi-Config path:

```sh
./build/Debug/ModernCppConverter
```

For single-config generators, the executable may be at:

```sh
./build/ModernCppConverter
```

We also have a script in /scripts folder named RunTheApp.sh, we can use that to directly run the app without the need of cmake commands:

```sh
chmod 777 RunTheApp.sh
./scripts/RunTheApp.sh            #without arguments it will run the release mode
./scripts/RunTheApp.sh debug      # Debug
./scripts/RunTheApp.sh release    # Release
./scripts/RunTheApp.sh clean      # clean Release
./scripts/RunTheApp.sh debug clean
./scripts/RunTheApp.sh release clean
./scripts/RunTheApp.sh --help
```

## Offline Mode

Offline Rule-Based mode is the primary feature.

It:

- runs locally
- does not require network access
- does not require API keys
- does not call AI APIs
- applies deterministic modernization passes
- validates known semantic fallout after type-changing transformations
- can run syntax-only compile verification

When a transformation is uncertain, the engine should preserve the original code and emit a suggestion instead of returning risky output.

## Optional AI / Backend Mode

The app also supports optional backend-assisted modes:

- Online AI-Assisted
- Hybrid Offline + AI Review

The backend is separate from the desktop app. Offline conversion continues to work when the backend is unavailable.

Start the backend for development:

```sh
cd backend
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

Backend settings are read from `config/app_config.json`.

## Repository Modernization Mode

Repository mode lets the app clone a public GitHub repository into a separate workspace, scan supported C++ files, run the same offline modernization pipeline file by file, and write modernization reports.

Supported file extensions include:

- `.cpp`
- `.cc`
- `.cxx`
- `.hpp`
- `.h`
- `.hh`
- `.hxx`

The repository workflow is intentionally conservative:

- clones into a separate workspace
- does not modify the original local repository
- creates backup files before rewriting scanned files
- does not commit or push changes
- does not run generated binaries
- writes text and JSON reports

## Testing

Build first:

```sh
cmake --build build --config Debug
```

Run C++ tests:

```sh
ctest --test-dir build -C Debug --output-on-failure
```

Run backend tests when working on the Python backend:

```sh
backend/.venv/bin/pytest backend/tests
```

The v1.0 release regression plan lives in `docs/V1_REGRESSION_TEST_PLAN.md`. It documents the release gate for raw arrays, string modernization, ownership propagation, enum output, FILE I/O cleanup, loop/container modernization, GUI worker lifecycle, and repository-mode smoke coverage.

Major test areas include:

- ownership modernization
- raw array and vector cleanup
- string modernization
- Rule of Zero cleanup
- polymorphic safety
- functor/lambda modernization
- iterator and container modernization
- scoped enum output propagation
- compile verification
- repository mode
- GUI conversion smoke tests

## Project Layout

```text
ModernCPPConvertor/
  CMakeLists.txt
  README.md
  AGENTS.md
  config/
  docs/
  scripts/
  backend/
  src/
    app/
    backend/
    converter/
    editor/
    models/
    repository/
    modernization/
    pipeline/
    services/
    ui/
    utils/
  tests/
    samples/
```

The production source files currently remain in their established folders:

- `src/app` contains the Qt main window and conversion coordinator.
- `src/editor` contains the reusable C++ code editor widgets.
- `src/converter` contains the modernization engine, passes, validators, and compile verifier.
- `src/backend` contains the desktop backend client.
- `src/models` contains shared result and option models.
- `src/repository` contains repository modernization services.
- `src/utils` is reserved for future cross-cutting utility code.

The build is modularized with folder-level CMake files:

- `src/CMakeLists.txt`
- `src/editor/CMakeLists.txt`
- `src/modernization/CMakeLists.txt`
- `src/pipeline/CMakeLists.txt`
- `src/services/CMakeLists.txt`
- `src/ui/CMakeLists.txt`
- `tests/CMakeLists.txt`

## Security Notes

- Do not commit API keys.
- Do not commit `.env` files.
- Keep backend secrets in local environment files or deployment secrets.
- The desktop app does not need API keys for offline mode.
- Compile verification uses syntax-only compiler checks and does not execute converted code.

## Known Limitations

- The converter is rule-based and intentionally conservative; uncertain transformations are skipped with diagnostics.
- Full Clang AST integration is not part of v1.0.
- Complex template metaprogramming, SFINAE-heavy code, and advanced macro systems may need manual review.
- Complex or binary `FILE*` usage is suggestion-only unless the text I/O pattern is simple and safe.
- Repository mode works file by file and does not replace a full project-aware refactoring tool with complete build graph knowledge.
- Compile verification is syntax-only. Converted programs are never executed by the app.

## Roadmap

Short term:

- Improve diagnostics and pass reporting.
- Add more targeted rollback messages.
- Expand repository-mode reports.

Mid term:

- Improve semantic validation and type propagation.
- Add richer source-range tracking.
- Add optional parser-backed analysis behind the existing converter interfaces.

Long term:

- Introduce Clang tooling where it clearly improves correctness.
- Support project-wide build-aware modernization.
- Grow the offline engine into an enterprise-grade semantic refactoring system.

## License

License placeholder. Add the final project license before public release.
