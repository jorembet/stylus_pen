#!/usr/bin/env bash
# Builds on first run, then launches Stylus Pen.
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$APP_DIR/build/bin/stylus-pen"

if [[ ! -x "$BIN" ]]; then
  echo "Building Stylus Pen..." >&2
  cmake -S "$APP_DIR" -B "$APP_DIR/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build "$APP_DIR/build" --parallel
fi

exec "$BIN" "$@"
