#!/usr/bin/env bash
# Fetch the pinned SerenityOS source used by libgui-wayland.
#
# A blobless, sparse checkout keeps the download small: we only materialise the
# trees needed to build LibGfx/LibGUI on the host, plus the shared kernel
# headers that userspace libraries legitimately include.
set -euo pipefail

SERENITY_REMOTE="${SERENITY_REMOTE:-https://github.com/SerenityOS/serenity.git}"
# Pinned so the audit and patch in this repo are reproducible. See DESIGN.md.
SERENITY_REVISION="${SERENITY_REVISION:-80baeb29708ae10f4a4cc8a07ca57e9876c3e3d8}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"

# Trees required to build the full LibGfx + LibGUI on the host:
#   AK, Userland, Meta, Base/res, Base/home, Base/etc  -> sources, build system, resources
#   Tests                                     -> host unit tests
#   Kernel/API                               -> shared ABI constants (KeyCode, serenity_limits)
#   Kernel/Memory                            -> header-only VirtualAddress/PhysicalAddress (LibELF)
SPARSE_DIRS=(AK Userland Meta Base/res Base/home Base/etc Tests Kernel/API Kernel/Memory)

if [[ -d "$SERENITY_SRC/.git" ]]; then
    echo "==> Updating existing checkout at $SERENITY_SRC"
    git -C "$SERENITY_SRC" fetch --depth 1 origin "$SERENITY_REVISION"
else
    echo "==> Cloning SerenityOS (blobless) into $SERENITY_SRC"
    git clone --filter=blob:none --no-checkout "$SERENITY_REMOTE" "$SERENITY_SRC"
    git -C "$SERENITY_SRC" fetch --depth 1 origin "$SERENITY_REVISION"
fi

git -C "$SERENITY_SRC" checkout --quiet "$SERENITY_REVISION"
git -C "$SERENITY_SRC" sparse-checkout init --cone
git -C "$SERENITY_SRC" sparse-checkout set "${SPARSE_DIRS[@]}"
git -C "$SERENITY_SRC" checkout --quiet

echo "==> SerenityOS ready at $SERENITY_SRC ($(git -C "$SERENITY_SRC" rev-parse --short HEAD))"
