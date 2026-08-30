#!/usr/bin/env bash
# Open the serial monitor.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
pio device monitor "$@"
