#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -d build ]]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
cmake --build build --parallel

./build/bin/test_core
echo
./build/bin/test_render /tmp
echo
if command -v xvfb-run >/dev/null 2>&1; then
    cd build && xvfb-run -a ctest --output-on-failure
else
    cd build && ctest --output-on-failure
fi


