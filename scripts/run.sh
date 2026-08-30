#!/usr/bin/env bash
# Build, flash, and open the serial monitor.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
pio run --target upload --target monitor "$@"
