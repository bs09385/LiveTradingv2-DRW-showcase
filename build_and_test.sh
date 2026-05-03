#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

echo "============================================"
echo " LiveTradingv2 - Build and Test (Linux)"
echo "============================================"

echo "[1/3] Configuring CMake..."
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "[2/3] Building..."
cmake --build build -j"$(nproc)"

echo "[3/3] Running tests..."
cd build/tests
./tests
cd ../..

echo "============================================"
echo " BUILD + TESTS PASSED"
echo "============================================"
