#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

CONFIG="${1:-config/default.json}"
ACCOUNT="${2:-}"

# Build if engine binary doesn't exist
if [ ! -f build/engine ]; then
    echo "Engine binary not found, building..."
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build build -j"$(nproc)"
fi

echo "Starting engine with config: $CONFIG"
if [ -n "$ACCOUNT" ]; then
    echo "Account: $ACCOUNT"
    exec ./build/engine "$CONFIG" "$ACCOUNT"
else
    exec ./build/engine "$CONFIG"
fi
