#!/usr/bin/env bash
set -euo pipefail

echo "=== Installing build dependencies ==="
sudo apt update
sudo apt install -y build-essential cmake libssl-dev git

# Ubuntu 22.04 ships GCC 11; uncomment below for GCC 13:
# sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
# sudo apt install -y gcc-13 g++-13
# sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100
# sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

echo "=== Dependencies installed ==="
gcc --version
cmake --version
