# v1.0 Release Checklist

Use this checklist before creating the GitHub v1.0 release tag.

## 1. Build Verification

- [ ] Start from a clean checkout or a clearly reviewed release branch.
- [ ] Configure with the supported CMake preset/command:

  ```sh
  cmake -S . -B build -G "Ninja Multi-Config"
  ```

- [ ] Build Debug:

  ```sh
  cmake --build build --config Debug
  ```

- [ ] Optionally build Release:

  ```sh
  cmake --build build --config Release
  ```

- [ ] Confirm no unexpected compiler errors or new warnings appear.

## 2. Test Verification

- [ ] Run the full CTest suite:

  ```sh
  ctest --test-dir build -C Debug --output-on-failure
  ```

- [ ] Confirm all C++ test targets pass.
- [ ] Confirm `GuiConversionSmokeTests` passes with `QT_QPA_PLATFORM=offscreen`.
- [ ] If backend work changed or backend mode is part of the release notes, run:

  ```sh
  backend/.venv/bin/pytest backend/tests
  ```

## 3. Regression Pack Verification

- [ ] Run the v1.0 regression pack directly:

  ```sh
  ./build/Debug/V1RegressionTests
  ```

- [ ] Confirm the pack covers:
  - raw dynamic arrays to `std::vector`
  - owned C strings to `std::string`
  - C-string API cleanup
  - raw owning pointers to `std::unique_ptr`
  - observer pointer `.get()` preservation
  - enum class propagation and output safety
  - simple `FILE*` to `std::ofstream`
  - range-for and structured-binding modernization
  - `std::pair` streaming safety
  - `std::string_view` safety
  - repository-mode smoke behavior

- [ ] Review `docs/V1_REGRESSION_TEST_PLAN.md`.

## 4. README Verification

- [ ] README has a clear project summary.
- [ ] README explains Offline Rule-Based mode as the primary feature.
- [ ] README explains backend/AI modes as optional.
- [ ] README includes build instructions for CMake/Ninja Multi-Config.
- [ ] README includes run instructions for the app.
- [ ] README includes test commands.
- [ ] README links or mentions the v1.0 regression test plan.
- [ ] README includes security notes and known limitations.

## 5. Screenshots Placeholder

- [ ] Confirm README screenshots render on GitHub.
- [ ] If screenshots are replaced, verify:
  - Code Converter tab
  - Options tab
  - Repository Modernization tab
  - Compile Verification / Diagnostics tab
- [ ] Do not include local filesystem paths or private data in screenshots.

## 6. Known Limitations Check

- [ ] Known limitations are documented in README and/or release notes.
- [ ] Release notes mention that v1.0 is rule-based, not Clang-AST-based.
- [ ] Release notes mention that complex macros, advanced templates, and complex binary file I/O may need manual review.
- [ ] Release notes mention that compile verification is syntax-only and never executes converted code.

## 7. Security Check

- [ ] Confirm no API keys or secrets are tracked:

  ```sh
  git grep -n -I -E "sk-[A-Za-z0-9_-]{20,}|OPENAI_API_KEY=.*[A-Za-z0-9_-]{10,}" -- . ':!backend/.venv' ':!build'
  ```

- [ ] Confirm `.env` files are ignored.
- [ ] Confirm `backend/.env` is ignored.
- [ ] Confirm only `.env.example` templates are present in Git.
- [ ] Confirm backend provider keys remain server-side only.
- [ ] Confirm compile verification does not execute generated binaries.

## 8. Config Check

- [ ] Review `config/app_config.json`.
- [ ] Confirm `backendUrl` points to the expected local/default backend URL.
- [ ] Confirm `requestTimeoutMs` is reasonable for desktop use.
- [ ] Confirm config contains no credentials.

## 9. CMake Build Check

- [ ] Root `CMakeLists.txt` configures cleanly.
- [ ] `tests/CMakeLists.txt` includes `V1RegressionTests`.
- [ ] Qt Widgets, Network, and Concurrent link correctly.
- [ ] Multi-config Debug and Release layouts are preserved.
- [ ] Runtime DLL/app bundle handling still works on the target platform.

## 10. Backend Optional Mode Check

- [ ] Offline mode works with backend stopped.
- [ ] Online mode shows a controlled fallback/error when backend is unavailable.
- [ ] Hybrid mode runs local offline conversion first.
- [ ] Backend health endpoint works when backend is running.
- [ ] Backend tests pass in the backend virtual environment.

## 11. Packaging Notes

- [ ] Do not package `build/`, `build-*`, `.pytest_cache/`, or local virtual environments.
- [ ] Do not package `.env` files.
- [ ] Include README, docs, config templates, and license file when available.
- [ ] macOS packaging should include the Qt runtime dependencies or documented deployment steps.
- [ ] Windows packaging should include required Qt DLLs or documented deployment steps.
- [ ] Linux packaging should document Qt 6 runtime requirements.

## 12. Final Manual Smoke Test

1. Launch the app.
2. Paste a small legacy snippet with `NULL`, `typedef`, and an iterator loop.
3. Convert in Offline Rule-Based mode.
4. Confirm output appears and diagnostics are populated.
5. Enable compile verification and convert again.
6. Paste the CanBuffer sample from the GUI smoke test and convert.
7. Confirm conversion finishes and does not hit the convergence guard.
8. Click Clear during or after a conversion.
9. Confirm the spinner/progress state stops and another conversion can start immediately.
10. Open Repository Modernization mode.
11. Run a small repository smoke test if network access is available.
12. Verify reports are created and no changes are committed automatically.

## Release Decision

- [ ] Build passed.
- [ ] CTest passed.
- [ ] Backend tests passed or were explicitly marked not applicable.
- [ ] v1.0 regression pack passed.
- [ ] README and docs reviewed.
- [ ] Security check passed.
- [ ] Manual smoke test passed.
- [ ] Release notes prepared.
- [ ] Tag can be created.
