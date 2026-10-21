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

if ! git -C "$SERENITY_SRC" rev-parse --git-dir >/dev/null 2>&1; then
    echo "error: SerenityOS checkout not found at $SERENITY_SRC" >&2
    echo "       run scripts/fetch-serenity.sh first" >&2
    exit 1
fi

# Apply the build-system patches idempotently. --recount tolerates approximate
# hunk line counts, which keeps hand-maintained patches easy to edit.
#
# The third branch is the honest one. Several patches edit the same files (e.g.
# Meta/Lagom/CMakeLists.txt), so once earlier ones are in, `git apply` can neither
# apply nor reverse this one — and no per-patch check can tell "already applied"
# from "written against a different revision", because overlapping sets make the
# reverse-check fail in both cases. (GNU patch does not help: measured on a
# correctly patched tree, it rejects all of them.) So the loop no longer claims to
# know: it counts what it could not verify and says so at the end, which is the
# one line that makes a *later* failure legible. A stale SerenityOS pin once made
# every SDE patch unverifiable and the build failed much later, in CMake, with no
# hint of the cause.
# A patch set applied to this checkout is recorded in the git dir. The framework
# build runs this script twice (generated endpoint headers, then the full library
# list); without the marker the second run re-checks its own already-applied
# patches, and the ones whose hunks overlap later patches can neither apply nor
# reverse-apply. They are not stale -- the tree is exactly the result the patch
# set produces -- so do not turn an idempotency detail into a rebuild failure.
patch_set_fingerprint() {
    {
        git -C "$SERENITY_SRC" rev-parse HEAD
        sha256sum "$PROJECT_ROOT"/patches/*.patch
    } | sha256sum | cut -d' ' -f1
}
PATCH_MARKER="$(git -C "$SERENITY_SRC" rev-parse --absolute-git-dir)/sde-patch-set-applied"
PATCH_FINGERPRINT="$(patch_set_fingerprint)"
applied_count=0
already_count=0
unverified_count=0
unverified_names=""
if [[ -f "$PATCH_MARKER" && "$(cat "$PATCH_MARKER")" == "$PATCH_FINGERPRINT" ]]; then
    echo "==> patch set already applied for $(git -C "$SERENITY_SRC" rev-parse --short HEAD)"
else
    for patch in "$PROJECT_ROOT"/patches/*.patch; do
        name="$(basename "$patch")"
        if git -C "$SERENITY_SRC" apply --recount -C1 --reverse --check "$patch" >/dev/null 2>&1; then
            echo "==> $name already applied"
            already_count=$((already_count + 1))
        elif git -C "$SERENITY_SRC" apply --recount -C1 --check "$patch" >/dev/null 2>&1; then
            echo "==> Applying $name"
            git -C "$SERENITY_SRC" apply --recount -C1 "$patch"
            applied_count=$((applied_count + 1))
        else
            echo "==> $name assumed applied (overlapping context; unverifiable)"
            unverified_count=$((unverified_count + 1))
            unverified_names="$unverified_names $name"
        fi
    done
    echo "==> patches: $applied_count applied, $already_count already applied, $unverified_count unverifiable"
fi
if [[ -n "$unverified_names" ]]; then
    # A patch the loop cannot verify may simply not be in effect, and the failure it
    # causes appears much later and elsewhere -- as a test failure with no obvious
    # cause. Naming them in one build beats discovering them one 40-minute build at a
    # time, and *stopping* beats both: this branch is how three patches (0017, 0042,
    # 0045) silently went missing from every CI image while the developer tree had
    # them, so `/res//color-schemes` worked locally and not in CI. There is no
    # "assume": refresh the patch against the current tree, or the build does not
    # proceed.
    echo "    unverifiable (they would not be applied):$unverified_names" >&2
    echo "    A patch that neither applies nor reverse-applies no longer matches the tree." >&2
    echo "    Refresh it (see docs/26-ci.md): regenerate the hunks against the current" >&2
    echo "    revision, then the patch set and the tree agree again." >&2
    if [[ "${ALLOW_UNVERIFIED_PATCHES:-0}" != 1 ]]; then
        exit 1
    fi
    echo "    ALLOW_UNVERIFIED_PATCHES=1: continuing with them unapplied." >&2
fi
# Say more only when it is actionable: a checkout that differs from the pin is
# the case where "unassumable" patches mean something.
if [[ "$unverified_count" -gt 0 ]]; then
    # The pin line is SERENITY_REVISION="${SERENITY_REVISION:-<hash>}"; take
    # the first hash-looking token on it.
    pinned="$(grep -m1 '^SERENITY_REVISION=' "${PROJECT_ROOT}/scripts/fetch-serenity.sh" 2>/dev/null | grep -oE '[0-9a-f]{7,40}' | head -1)"
    head_rev="$(git -C "$SERENITY_SRC" rev-parse HEAD 2>/dev/null || true)"
    if [[ -n "$pinned" && -n "$head_rev" && "$head_rev" != "$pinned" ]]; then
        echo "    WARNING: this checkout is at ${head_rev:0:12}, not the pinned ${pinned:0:12}." >&2
        echo "             $unverified_count patch(es) could not be verified against it; if a" >&2
        echo "             build step below fails unexpectedly, that is the likely reason." >&2
        echo "             scripts/fetch-serenity.sh pins it." >&2
    fi
fi
if [[ "$unverified_count" -eq 0 ]]; then
    printf '%s\n' "$PATCH_FINGERPRINT" > "$PATCH_MARKER"
fi

echo "==> Configuring Lagom"
cmake -S "$SERENITY_SRC/Meta/Lagom" -B "$BUILD_DIR" -GNinja \
    -DBUILD_LAGOM=ON \
    -DENABLE_LAGOM_LIBWEB=ON \
    -DENABLE_LAGOM_LADYBIRD=OFF \
    -DENABLE_CLANG_PLUGINS=OFF \
    -DENABLE_LAGOM_CCACHE=OFF \
    -DCMAKE_BUILD_TYPE=Release

echo "==> Building: ${TARGETS[*]}"
# The shim goes last, whatever was asked for: it is the one target whose headers
# include generated endpoint headers without declaring they are inputs.
PHASE_ONE=()
PHASE_TWO=()
for target in "${TARGETS[@]}"; do
    case "$target" in
        windowserver | eglgpu) PHASE_TWO+=("$target") ;;
        *) PHASE_ONE+=("$target") ;;
    esac
done
if [[ "${#PHASE_TWO[@]}" -gt 0 ]]; then
    [[ "${#PHASE_ONE[@]}" -gt 0 ]] && ninja -C "$BUILD_DIR" -j"$JOBS" "${PHASE_ONE[@]}" "${GENERATED[@]}"
    ninja -C "$BUILD_DIR" -j"$JOBS" "${PHASE_TWO[@]}"
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
