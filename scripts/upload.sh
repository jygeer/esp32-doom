#!/usr/bin/env bash
# Build and flash the firmware to the board.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
pio run --target upload "$@"
