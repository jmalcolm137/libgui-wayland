#!/usr/bin/env bash
# Fetch the pinned SerenityOS source used by libgui-wayland.
#
# A blobless, sparse checkout keeps the download small: we only materialise the
# trees needed to build LibGfx/LibGUI on the host, plus the shared kernel
# headers that userspace libraries legitimately include.
set -euo pipefail

# The checkout comes from our public fork's `fixes` branch: SerenityOS at the
# pinned upstream revision (see below), plus fixes that make sense for SerenityOS
# on its own, each one a candidate for an upstream PR (DESIGN.md
# §4.1.1). A change that only exists because of how this port or the SDE uses
# SerenityOS does not belong there: it stays in patches/.
SERENITY_REMOTE="${SERENITY_REMOTE:-https://github.com/jmalcolm137/serenity.git}"
# Pinned so the audit and patch in this repo are reproducible. See DESIGN.md.
#
# Advanced 2026-10-15: the previous pin (80baeb29) predates upstream work the port
# and the SDE now depend on — e.g. it has no Userland/Games/Sudoku at all, which
# the SDE's Lagom targets add by name, so a fresh tree failed to configure
# ("add_subdirectory given source .../Userland/Games/Sudoku which is not an
# existing directory"). The development checkouts had drifted ahead of the pin,
# which is why nothing noticed.
#
# The fork's `fixes` branch sits on upstream a43d652b44066ab4d2605e4be4dc0bc9aa8c116e
# (SERENITY_UPSTREAM_BASE below); rebasing it onto a newer upstream is a `git
# rebase` of real commits, and the pin moves to the rebased tip.
SERENITY_UPSTREAM_BASE="a43d652b44066ab4d2605e4be4dc0bc9aa8c116e"
SERENITY_REVISION="${SERENITY_REVISION:-054963e8116a3b13797318c5251313c1b8fa4caf}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"

# Trees required to build the full LibGfx + LibGUI on the host:
#   AK, Userland, Meta, Base/res, Base/home, Base/etc  -> sources, build system, resources
#   Base/usr/share/man                        -> the man pages Help renders
#   Tests                                     -> host unit tests
#   Kernel/API                               -> shared ABI constants (KeyCode, serenity_limits)
#   Kernel/Memory                            -> header-only VirtualAddress/PhysicalAddress (LibELF)
SPARSE_DIRS=(AK Userland Meta Base/res Base/home Base/etc Base/usr/share/man Tests Kernel/API Kernel/Memory)

if [[ -d "$SERENITY_SRC/.git" ]]; then
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
