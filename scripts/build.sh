#!/usr/bin/env bash
# Build the firmware.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
pio run "$@"
