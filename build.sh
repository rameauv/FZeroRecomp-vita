#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Ensure VITASDK is set
export VITASDK=${VITASDK:-}
if [ -z "$VITASDK" ]; then
    echo "Error: VITASDK environment variable must be set"
    echo "Example: export VITASDK=/path/to/vitasdk"
    exit 1
fi

# Build with Ninja for Vita using the VitaSDK toolchain
cmake -S "$ROOT" -B "$ROOT/build-vita" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$ROOT/build-vita" --config Release --parallel
printf '\nBuilt: %s\n' "$ROOT/build-vita/fzero_recomp.vpk"
