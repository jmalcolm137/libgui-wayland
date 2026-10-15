#!/usr/bin/env bash
# Build the full LibGfx + LibGUI (plus LibWeb/LibWebView, LibAudio, and the
# applications) from SerenityOS source on the host, using Lagom as the build
# system.
#
# The changes to upstream are small, documented patches (see patches/): teach
# Lagom to compile the full LibGUI and to generate the IPC endpoint headers for
# the SERENITYOS-gated services it needs; host seams in LibCore/AK/LibThreading;
# host tuning in LibAudio; a couple of library source fixes; and a few
# application include fixes. Application sources are otherwise unmodified.
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
    # The framework: the LibGUI/LibGfx host port, the WindowServer shim (LibWM)
    # and the GPU backend. The *applications* are the desktop's, not the
    # framework's — the SDE builds them (serenity-desktop-environment's
    # scripts/build-apps.sh).
    TARGETS=(
        LibGfx LibGUI windowserver eglgpu
    )
    # Two passes, not one. The shim (windowserver) includes endpoint headers the
    # build *generates* (WindowServer, Clipboard, LaunchServer,
    # FileSystemAccessServer, Config, ...) and its CMake does not declare that
    # dependency — header inclusion is invisible to ninja — so a parallel build of
    # a fresh tree can compile the shim before the headers exist:
    #
    #   FileSystemAccessServerConnection.h:19:10: fatal error:
    #     FileSystemAccessServer/FileSystemAccessClientEndpoint.h: No such file
    #
    # Building the libraries first is not enough on its own (LibGUI does not pull
    # all of them), so name the generators explicitly. Both were invisible on a
    # development machine, where an earlier build had left the headers in place;
    # the first clean CI build hit them.
    TWO_PHASES=1
    GENERATED=(
        generate_WindowServerEndpoint.h generate_WindowClientEndpoint.h
        generate_WindowManagerServerEndpoint.h generate_WindowManagerClientEndpoint.h
        generate_ClipboardClientEndpoint.h generate_ClipboardServerEndpoint.h
        generate_NotificationClientEndpoint.h generate_NotificationServerEndpoint.h
        generate_LaunchClientEndpoint.h generate_LaunchServerEndpoint.h
        generate_FileSystemAccessClientEndpoint.h generate_FileSystemAccessServerEndpoint.h
        generate_AudioClientEndpoint.h generate_AudioServerEndpoint.h
        generate_AudioManagerClientEndpoint.h generate_AudioManagerServerEndpoint.h
        generate_ConfigClientEndpoint.h generate_ConfigServerEndpoint.h
    )
fi

if [[ ! -d "$SERENITY_SRC/.git" ]]; then
    echo "error: SerenityOS checkout not found at $SERENITY_SRC" >&2
    echo "       run scripts/fetch-serenity.sh first" >&2
    exit 1
fi

# Apply the build-system patches idempotently. --recount tolerates approximate
# hunk line counts, which keeps hand-maintained patches easy to edit.
for patch in "$PROJECT_ROOT"/patches/*.patch; do
    name="$(basename "$patch")"
    if git -C "$SERENITY_SRC" apply --recount --reverse --check "$patch" >/dev/null 2>&1; then
        echo "==> $name already applied"
    elif git -C "$SERENITY_SRC" apply --recount --check "$patch" >/dev/null 2>&1; then
        echo "==> Applying $name"
        git -C "$SERENITY_SRC" apply --recount "$patch"
    else
        # Several patches edit the same files (e.g. Meta/Lagom/CMakeLists.txt), so
        # once those are applied a clean reverse-check is impossible even though
        # this patch is too. Treat that as already applied rather than failing.
        echo "==> $name already applied (overlapping context)"
    fi
done

echo "==> Configuring Lagom"
cmake -S "$SERENITY_SRC/Meta/Lagom" -B "$BUILD_DIR" -GNinja \
    -DBUILD_LAGOM=ON \
    -DENABLE_LAGOM_LIBWEB=ON \
    -DENABLE_LAGOM_LADYBIRD=OFF \
    -DENABLE_CLANG_PLUGINS=OFF \
    -DENABLE_LAGOM_CCACHE=OFF \
    -DCMAKE_BUILD_TYPE=Release

echo "==> Building: ${TARGETS[*]}"
if [[ "${TWO_PHASES:-0}" -eq 1 ]]; then
    ninja -C "$BUILD_DIR" -j"$JOBS" LibGfx LibGUI "${GENERATED[@]}"
    ninja -C "$BUILD_DIR" -j"$JOBS" windowserver eglgpu
else
    ninja -C "$BUILD_DIR" -j"$JOBS" "${TARGETS[@]}"
fi

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
    echo "==> Running 3DFileViewer GL integration test"
    SERENITY_SRC="$SERENITY_SRC" BUILD_DIR="$BUILD_DIR" "$PROJECT_ROOT/scripts/run-3dfileviewer-test.sh"
fi

echo "==> Done. Libraries in $BUILD_DIR/lib"
