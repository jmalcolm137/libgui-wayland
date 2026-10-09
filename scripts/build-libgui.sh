#!/usr/bin/env bash
# Build the *full* LibGfx + LibGUI from unmodified SerenityOS source on the host,
# using Lagom as the build system.
#
# The only change we make to upstream is a build-system patch that teaches Lagom
# to compile all of LibGUI (instead of its historical 6-file stub) and to
# generate the IPC endpoint headers for the SERENITYOS-gated services
# (WindowServer, Clipboard, LaunchServer, NotificationServer,
# FileSystemAccessServer) that LibGUI and its friends need. No library or
# application source is modified.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
JOBS="${JOBS:-$(nproc)}"

TARGETS=()
RUN_TESTS="${RUN_TESTS:-1}"
for arg in "$@"; do
    case "$arg" in
        --no-tests) RUN_TESTS=0 ;;
        *) TARGETS+=("$arg") ;;
    esac
done
if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(LibGfx LibGUI wm Calculator PDFViewer libwm-test-window)
fi

if [[ ! -d "$SERENITY_SRC/.git" ]]; then
    echo "error: SerenityOS checkout not found at $SERENITY_SRC" >&2
    echo "       run scripts/fetch-serenity.sh first" >&2
    exit 1
fi

# Apply the build-system patches idempotently.
for patch in "$PROJECT_ROOT"/patches/*.patch; do
    name="$(basename "$patch")"
    if git -C "$SERENITY_SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "==> $name already applied"
    else
        echo "==> Applying $name"
        git -C "$SERENITY_SRC" apply "$patch"
    fi
done

echo "==> Configuring Lagom"
cmake -S "$SERENITY_SRC/Meta/Lagom" -B "$BUILD_DIR" -GNinja \
    -DBUILD_LAGOM=ON \
    -DENABLE_LAGOM_LIBWEB=OFF \
    -DENABLE_LAGOM_LADYBIRD=OFF \
    -DENABLE_CLANG_PLUGINS=OFF \
    -DENABLE_LAGOM_CCACHE=OFF \
    -DCMAKE_BUILD_TYPE=Release

echo "==> Building: ${TARGETS[*]}"
ninja -C "$BUILD_DIR" -j"$JOBS" "${TARGETS[@]}"

if [[ "$RUN_TESTS" == "1" ]]; then
    echo "==> Running upstream library test suites (set RUN_TESTS=0 or pass --no-tests to skip)"
    SERENITY_SRC="$SERENITY_SRC" BUILD_DIR="$BUILD_DIR" "$PROJECT_ROOT/scripts/run-lib-tests.sh"
    echo "==> Running headless integration test"
    SERENITY_SRC="$SERENITY_SRC" BUILD_DIR="$BUILD_DIR" "$PROJECT_ROOT/scripts/run-headless-tests.sh"
    echo "==> Running clipboard integration test"
    SERENITY_SRC="$SERENITY_SRC" BUILD_DIR="$BUILD_DIR" "$PROJECT_ROOT/scripts/run-clipboard-test.sh"
    echo "==> Running menu integration test"
    SERENITY_SRC="$SERENITY_SRC" BUILD_DIR="$BUILD_DIR" "$PROJECT_ROOT/scripts/run-menu-test.sh"
    echo "==> Running PDFViewer integration test"
    SERENITY_SRC="$SERENITY_SRC" BUILD_DIR="$BUILD_DIR" "$PROJECT_ROOT/scripts/run-pdfviewer-test.sh"
fi

echo "==> Done. Libraries in $BUILD_DIR/lib"
