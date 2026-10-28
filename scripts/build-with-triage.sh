#!/usr/bin/env bash
# Run a build with its output captured, and triage the log if it fails.
#
# Usage:
#   scripts/build-with-triage.sh [build-libgui.sh arguments...]
#
# Environment:
#   BUILD_SCRIPT  build to run (default: scripts/build-libgui.sh)
#   LOG_DIR       directory for captured logs (default: build/logs)
#   LOG_FILE      exact log path, overriding LOG_DIR
#   TRIAGE=0      capture the log but skip the Jev triage card
#
# The build's exit status is preserved. A triage failure never masks it.
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_SCRIPT="${BUILD_SCRIPT:-$PROJECT_ROOT/scripts/build-libgui.sh}"
LOG_DIR="${LOG_DIR:-$PROJECT_ROOT/build/logs}"
mkdir -p "$LOG_DIR"
LOG="${LOG_FILE:-$LOG_DIR/$(basename "$BUILD_SCRIPT" .sh)-$(date -u +%Y%m%dT%H%M%SZ)-$$.log}"

echo "==> Capturing output to $LOG"
"$BUILD_SCRIPT" "$@" 2>&1 | tee "$LOG"
status=${PIPESTATUS[0]}

if [[ $status -eq 0 ]]; then
    echo "==> Build succeeded (log: $LOG)"
    exit 0
fi

echo
echo "==> Build failed (exit $status); log: $LOG"
if [[ "${TRIAGE:-1}" != 0 ]]; then
    "$PROJECT_ROOT/scripts/triage-failure.py" "$LOG" \
        --job "$(basename "$BUILD_SCRIPT" .sh)" \
        --exit-code "$status" || true
else
    echo "    TRIAGE=0: skipping Jev triage"
fi
exit "$status"
