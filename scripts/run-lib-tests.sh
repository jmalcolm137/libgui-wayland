#!/usr/bin/env bash
# Run the relevant upstream library test suites against our host build.
#
# Upstream has no Tests/LibGUI (LibGUI is validated through real applications),
# so this covers the libraries the rendering stack rests on: LibGfx, and later
# AK/LibCore. GUI behaviour is covered by the app matrix instead.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"

TESTS=(
    TestGfxBitmap
    TestColor
    TestRect
    TestPainter
    TestFontHandling
    TestPath
)

echo "==> Building tests"
ninja -C "$BUILD_DIR" "${TESTS[@]}"

export LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SERENITY_RES_ROOT="$SERENITY_SRC/Base/res"

cd "$SERENITY_SRC/Tests/LibGfx"
failures=0
for test in "${TESTS[@]}"; do
    printf '%-18s ' "$test"
    if "$BUILD_DIR/bin/$test" >/tmp/libwm-$test.log 2>&1; then
        echo PASS
    else
        echo FAIL
        tail -5 "/tmp/libwm-$test.log"
        failures=$((failures + 1))
    fi
done

if [[ $failures -ne 0 ]]; then
    echo "==> $failures test(s) failed"
    exit 1
fi
echo "==> all tests passed"
