#!/usr/bin/env bash
# Fetch the pinned SerenityOS source used by libgui-wayland.
#
# A blobless, sparse checkout keeps the download small: we only materialise the
# trees needed to build LibGfx/LibGUI on the host, plus the shared kernel
# headers that userspace libraries legitimately include.
set -euo pipefail

# The checkout comes from our public fork's `fixes` branch: SerenityOS at the
# pinned upstream revision (see below), plus fixes that make sense for SerenityOS
# on its own, each one a candidate for an upstream PR (docs/development-history.md
# §4.1.1). A change that only exists because of how this port or a desktop uses
# SerenityOS does not belong there: it stays in patches/.
SERENITY_REMOTE="${SERENITY_REMOTE:-https://github.com/jmalcolm137/serenity.git}"
# Pinned so the patch series in this repo is reproducible (see patches/README.md).
#
# Advanced 2026-11-25: rebased the fork's `fixes` branch onto upstream
# 835d22663febc14427577fc91468c1e4c1fcc914 (the latest SerenityOS master at the
# time). The pin is the commit before the branch's blue-grid wallpaper commit:
# the wallpaper is an SDE asset, and the libgui-wayland patch series adds it
# itself (patches/lagom-and-resources/0066-...), so pinning at the wallpaper
# commit would make that patch fail because the file already exists.
#
# The fork's `fixes` branch sits on SERENITY_UPSTREAM_BASE below; rebasing it onto
# a newer upstream is a `git rebase` of real commits, and the pin moves to the
# same point on the rebased branch (one commit before the wallpaper).
SERENITY_UPSTREAM_BASE="835d22663febc14427577fc91468c1e4c1fcc914"
SERENITY_REVISION="${SERENITY_REVISION:-584ceca57dbaca5f3b304915c6b449a69100ba78}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"

# Trees required to build the full LibGfx + LibGUI on the host:
#   AK, Userland, Meta, Base/res, Base/home, Base/etc  -> sources, build system, resources
#   Base/usr/share/man                        -> the man pages Help renders
#   Tests                                     -> host unit tests
#   Kernel/API                               -> shared ABI constants (KeyCode, serenity_limits)
#   Kernel/Memory                            -> header-only VirtualAddress/PhysicalAddress (LibELF)
SPARSE_DIRS=(AK Userland Meta Base/res Base/home Base/etc Base/usr/share/man Tests Kernel/API Kernel/Memory)

if git -C "$SERENITY_SRC" rev-parse --git-dir >/dev/null 2>&1; then
    # A checkout that has drifted ahead of the pin is the state that hides a stale
    # pin: every patch still "applies" to the newer tree, so nothing complains, and
    # a later build fails somewhere unrelated. Say it out loud.
    previous="$(git -C "$SERENITY_SRC" rev-parse HEAD 2>/dev/null || true)"
    echo "==> Updating existing checkout at $SERENITY_SRC"
    if [[ -n "$previous" && "$previous" != "$SERENITY_REVISION" ]]; then
        echo "    (it was at ${previous:0:12}, moving to the pinned ${SERENITY_REVISION:0:12})"
    fi
    git -C "$SERENITY_SRC" fetch --depth 1 "$SERENITY_REMOTE" "$SERENITY_REVISION"
else
    echo "==> Cloning SerenityOS (blobless) into $SERENITY_SRC"
    git clone --filter=blob:none --no-checkout "$SERENITY_REMOTE" "$SERENITY_SRC"
    git -C "$SERENITY_SRC" fetch --depth 1 "$SERENITY_REMOTE" "$SERENITY_REVISION"
fi

git -C "$SERENITY_SRC" checkout --quiet "$SERENITY_REVISION"
git -C "$SERENITY_SRC" sparse-checkout init --cone
git -C "$SERENITY_SRC" sparse-checkout set "${SPARSE_DIRS[@]}"
git -C "$SERENITY_SRC" checkout --quiet

echo "==> SerenityOS ready at $SERENITY_SRC ($(git -C "$SERENITY_SRC" rev-parse --short HEAD))"
