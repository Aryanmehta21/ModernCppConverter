#!/usr/bin/env bash
set -e

# Run from repo root
echo "Running $(basename "$0") from $(pwd)"
chmod +x "$(basename "$0")" 2>/dev/null || true
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

usage() {
    cat <<EOF
Usage: $(basename "$0") [debug] [clean] [--help]

Positional flags:
  debug       Build/run Debug instead of Release
  clean       Remove the build/<Config> directory (multi-config) or entire build (single-config)
  -h, --help  Show this help message
EOF
}

# parse args
CONFIG="Release"
CLEAN=false
for a in "$@"; do
    case "$a" in
        -h|--help)
            usage
            exit 0
            ;;
        debug)
            CONFIG="Debug"
            ;;
        release)
            CONFIG="Release"
            ;;
        clean)
            CLEAN=true
            ;;
        *)
            # ignore unknown
            ;;
    esac
done

# Detect OS and choose appropriate generator
OS="$(uname -s)"
case "$OS" in
    Darwin|Linux)
        GENERATOR="Ninja Multi-Config"
        ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        #Path to Microsoft's official VS locator tool in Git Bash
        VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"

        if [ -f "$VSWHERE" ]; then
            # Get the major version number (eg. 16,17,18)
            VS_MAJOR=$("$VSWHERE" -latest -property installationVersion | cut -d. -f1)

            case "$VS_MAJOR" in
                15) GENERATOR="Visual Studio 15 2017" ;;
                16) GENERATOR="Visual Studio 16 2019" ;;
                17) GENERATOR="Visual Studio 17 2022" ;;
                18) GENERATOR="Visual Studio 18 2026" ;;
                *) GENERATOR="Visual Studio 17 2022" ;; # default to latest known version
            esac
            echo "[INFO] Detected Visual Studio version $VS_MAJOR, using generator: $GENERATOR" >&2
        else
            #Fallback if vswhere.exe is not found, default to latest known version
            GENERATOR="Visual Studio 17 2022"
            echo "[WARNING] vswhere.exe not found, defaulting to generator: $GENERATOR" >&2
        fi
        ;;
    *)
        GENERATOR="Ninja Multi-Config"
        ;;
esac

# ensure build dir
mkdir -p build
cd build

configure() {
    if [ ! -f CMakeCache.txt ]; then
        cmake .. -G "$GENERATOR"
    else
        # Check if we need to upgrade from single-config to multi-config (on macOS/Linux)
        if ! grep -q "CMAKE_CONFIGURATION_TYPES" CMakeCache.txt 2>/dev/null; then
            if [ "$GENERATOR" = "Ninja Multi-Config" ]; then
                echo "[INFO] Upgrading single-config cache to multi-config..." >&2
                rm -f CMakeCache.txt
                cmake .. -G "$GENERATOR"
            fi
        fi
    fi
    # detect multi-config generator
    if grep -q "CMAKE_CONFIGURATION_TYPES" CMakeCache.txt 2>/dev/null; then
        MULTI_CONFIG=true
    else
        MULTI_CONFIG=false
    fi
}

find_binary() {
    candidates=()
    if [ "$MULTI_CONFIG" = true ]; then
        # only accept binaries that match the requested config
        candidates+=("$ROOT/build/$CONFIG/ModernCppConverter" "$ROOT/$CONFIG/ModernCppConverter" "$ROOT/build/$CONFIG/ModernCppConverter.app/Contents/MacOS/ModernCppConverter" "./$CONFIG/ModernCppConverter")
        for c in "${candidates[@]}"; do
            if [ -x "$c" ]; then
                echo "$c"
                return 0
            fi
        done
        # search for files under paths containing the config name
        LOCATED=$(find "$ROOT" -maxdepth 6 -type f -path "*/$CONFIG/ModernCppConverter" -print -quit || true)
        if [ -n "$LOCATED" ]; then
            echo "$LOCATED"
            return 0
        fi
        # no matching-config binary found
        return 1
    else
        # single-config generators: look for built binary
        candidates=("./ModernCppConverter" "$ROOT/ModernCppConverter")
        for c in "${candidates[@]}"; do
            if [ -x "$c" ]; then
                echo "$c"
                return 0
            fi
        done
        return 1
    fi
}

run_binary() {
    BINPATH="$1"
    [ -x "$BINPATH" ] || chmod +x "$BINPATH" 2>/dev/null || true
    echo "Running $BINPATH"
    "$BINPATH"
}

if [ "$CLEAN" = true ]; then
    # ensure configure to detect multi-config
    configure
    if [ "$MULTI_CONFIG" = true ]; then
        echo "Cleaning build/$CONFIG directory..."
        rm -rf "$ROOT/build/$CONFIG"
        mkdir -p "$ROOT/build"
        cd "$ROOT/build"
    else
        echo "Cleaning entire build directory (single-config generator)..."
        rm -rf "$ROOT/build"
        mkdir -p "$ROOT/build"
        cd "$ROOT/build"
    fi
    configure
    echo "Building ($CONFIG) after clean..."
    if [ "$MULTI_CONFIG" = true ]; then
        cmake --build . --config "$CONFIG"
    else
        cmake .. -DCMAKE_BUILD_TYPE="$CONFIG"
        cmake --build .
    fi
    if BIN=$(find_binary); then
        run_binary "$BIN"
        exit 0
    else
        echo "Error: executable not found after build" >&2
        exit 1
    fi
fi

# normal path: configure, try to run existing binary, otherwise build
configure
if BIN=$(find_binary); then
    run_binary "$BIN"
    exit 0
fi

echo "No existing binary found; building ($CONFIG) and running..."
if [ "$MULTI_CONFIG" = true ]; then
    cmake --build . --config "$CONFIG"
else
    cmake .. -DCMAKE_BUILD_TYPE="$CONFIG"
    cmake --build .
fi
if BIN=$(find_binary); then
    run_binary "$BIN"
    exit 0
else
    echo "Error: executable not found after build" >&2
    exit 1
fi
