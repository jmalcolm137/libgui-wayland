#!/usr/bin/env bash
# Build the framework and run the LibWM test suites through CTest.
#
#   scripts/check.sh                 every libwm suite
#   scripts/check.sh -L integration  only suites with that label
#   scripts/check.sh -R menu         only suites whose name matches
#
# Extra arguments are passed to ctest. Labels: integration, gl, tools,
# upstream (see src/test/CMakeLists.txt). The default excludes nothing; CI runs
# the same command.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"

if [[ ! -x "$HC" ]]; then
    if [[ ! -d "$PROJECT_ROOT/third_party/xlib-wayland/.git" ]]; then
        "$PROJECT_ROOT/scripts/fetch-headless-compositor.sh"
    fi
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi

"$PROJECT_ROOT/scripts/build-libgui.sh" --no-tests

ctest --test-dir "$BUILD_DIR" -L '^libwm$' --output-on-failure "$@"
