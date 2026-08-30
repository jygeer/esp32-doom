#!/usr/bin/env bash
# Remove build artifacts.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
pio run --target clean
