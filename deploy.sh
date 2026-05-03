#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

CONFIG="${1:-config/default.json}"
ACCOUNT="${2:-}"

# Pre-flight checks
command -v cmake >/dev/null 2>&1 || { echo "ERROR: cmake not found"; exit 1; }
command -v g++ >/dev/null 2>&1 || { echo "ERROR: g++ not found"; exit 1; }

echo "=== Pulling latest changes ==="
git pull --ff-only

echo "=== Configuring build ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release

echo "=== Building ==="
cmake --build build -j"$(nproc)"

echo "=== Running tests ==="
ctest --test-dir build --output-on-failure --timeout 60 || {
    echo "WARNING: Tests failed. Aborting deploy."
    exit 1
}

echo "=== Restarting engine ==="
if systemctl is-active --quiet polymarket-engine 2>/dev/null; then
    sudo systemctl restart polymarket-engine
    echo "Engine restarted via systemd"
else
    pkill -f "engine config/" 2>/dev/null || true
    sleep 1
    if [ -n "$ACCOUNT" ]; then
        nohup ./build/engine "$CONFIG" "$ACCOUNT" > /dev/null 2>&1 &
    else
        nohup ./build/engine "$CONFIG" > /dev/null 2>&1 &
    fi
    echo "Engine restarted (PID $!)"
fi
