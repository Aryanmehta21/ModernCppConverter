#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
CONFIG="${CONFIG:-Debug}"
GENERATOR="${GENERATOR:-Ninja Multi-Config}"
DRY_RUN=0
FAILURES=0
WARNINGS=0

usage() {
    cat <<'USAGE'
Usage: scripts/release_check.sh [--dry-run]

Environment:
  BUILD_DIR   CMake build directory. Default: ./build
  CONFIG      Build configuration. Default: Debug
  GENERATOR   CMake generator for first configure. Default: Ninja Multi-Config
USAGE
}

for arg in "$@"; do
    case "$arg" in
        --dry-run)
            DRY_RUN=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "FAIL unknown option: $arg"
            usage
            exit 2
            ;;
    esac
done

pass() {
    echo "PASS $1"
}

warn() {
    echo "WARN $1"
    WARNINGS=$((WARNINGS + 1))
}

fail() {
    echo "FAIL $1"
    FAILURES=$((FAILURES + 1))
}

run_or_dry() {
    local label="$1"
    shift
    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "PASS $label: dry-run would execute: $*"
        return 0
    fi

    echo "RUN  $label: $*"
    if "$@"; then
        pass "$label"
    else
        fail "$label"
    fi
}

check_file_exists() {
    local label="$1"
    local path="$2"
    if [[ -f "$path" ]]; then
        pass "$label"
    else
        fail "$label missing: $path"
    fi
}

check_version_metadata() {
    local cmake_file="$ROOT_DIR/CMakeLists.txt"
    local app_version_file="$ROOT_DIR/src/utils/AppVersion.cpp"
    local app_version_header="$ROOT_DIR/src/utils/AppVersion.h"

    if grep -q 'set(MODERNCPP_APP_VERSION "1.2.0-rc1"' "$cmake_file" \
        && grep -q 'set(MODERNCPP_RELEASE_CHANNEL "rc"' "$cmake_file" \
        && grep -q 'VERSION 1.2.0' "$cmake_file" \
        && grep -q 'diagnosticLine' "$app_version_file" \
        && grep -q 'startupLogLine' "$app_version_header"; then
        pass "version metadata present"
    else
        fail "version metadata present"
    fi
}

check_placeholder_versions() {
    local cmake_file="$ROOT_DIR/CMakeLists.txt"
    if grep -q 'VERSION 0.1.0' "$cmake_file"; then
        fail "placeholder version strings absent"
        return
    fi
    if grep -qE 'MODERNCPP_APP_VERSION "(0\.0\.0|0\.1\.0|TODO|PLACEHOLDER)"' "$cmake_file"; then
        fail "placeholder version strings absent"
        return
    fi
    pass "placeholder version strings absent"
}

check_committed_secrets() {
    if ! git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        warn "secret scan skipped: not inside a Git work tree"
        return
    fi

    local hits
    hits="$(
        git -C "$ROOT_DIR" grep -n -I -E \
            'sk-[A-Za-z0-9_-]{20,}|AIza[0-9A-Za-z_-]{20,}|-----BEGIN (RSA|OPENSSH|PRIVATE) KEY-----|OPENAI_API_KEY=[A-Za-z0-9_-]{10,}' \
            -- . \
            ':!docs/*' \
            ':!backend/README.md' \
            ':!backend/.env.example' \
            ':!tests/*' \
            2>/dev/null || true
    )"

    if [[ -n "$hits" ]]; then
        echo "$hits"
        fail "no committed API keys/secrets"
    else
        pass "no committed API keys/secrets"
    fi
}

echo "ModernCppConverter release check"
echo "root=$ROOT_DIR"
echo "build_dir=$BUILD_DIR"
echo "config=$CONFIG"
if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "mode=dry-run"
fi
echo

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    run_or_dry "cmake configure" cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "$GENERATOR"
else
    pass "cmake configure: existing build directory"
fi

run_or_dry "cmake build" cmake --build "$BUILD_DIR" --config "$CONFIG"
run_or_dry "ctest full suite" ctest --test-dir "$BUILD_DIR" -C "$CONFIG" --output-on-failure
run_or_dry "ctest release smoke" ctest --test-dir "$BUILD_DIR" -C "$CONFIG" -R ModernCppConverterSmokeTests --output-on-failure

check_version_metadata
check_placeholder_versions
check_committed_secrets
check_file_exists "README exists" "$ROOT_DIR/README.md"

if [[ -f "$ROOT_DIR/CHANGELOG.md" || -f "$ROOT_DIR/CHANGELOG" ]]; then
    pass "CHANGELOG exists"
else
    warn "CHANGELOG missing: add one before final release if release notes are needed"
fi

echo
if [[ "$FAILURES" -eq 0 && "$DRY_RUN" -eq 0 ]]; then
    echo "PASS release readiness: ready for tagging"
elif [[ "$FAILURES" -eq 0 && "$DRY_RUN" -eq 1 ]]; then
    echo "PASS release readiness dry-run: static checks passed; run without --dry-run before tagging"
else
    echo "FAIL release readiness: $FAILURES failing check(s)"
fi

if [[ "$WARNINGS" -gt 0 ]]; then
    echo "WARN release readiness: $WARNINGS warning(s)"
fi

if [[ "$FAILURES" -gt 0 ]]; then
    exit 1
fi
