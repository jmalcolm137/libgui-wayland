#!/usr/bin/env bash
# Verify that the whole patch series applies, in order, to the pinned SerenityOS
# revision.
#
# The series is applied to a throwaway git index built from the pinned commit,
# so neither the serenity/ working tree nor its applied-patch marker is touched,
# and it works whatever state that checkout is in. Run it after editing or
# adding a patch, or after moving the pin (scripts/fetch-serenity.sh); see
# patches/README.md for how to refresh a patch that no longer applies.
#
#   scripts/check-patches.sh                 check the series against the pin
#   scripts/check-patches.sh <rev>           check it against another revision
#   scripts/check-patches.sh --mark-applied  check, then record the series as
#                                            applied to serenity/ (see below)
#
# --mark-applied is for after you have edited a patch whose change is already in
# the serenity/ working tree: build-libgui.sh cannot re-verify overlapping
# patches in place, so it would refuse the changed series. This trusts that the
# working tree has the series applied (it cannot check, because a desktop's own
# series may edit some of the same files) and updates build-libgui.sh's marker.
set -euo pipefail

mark_applied=0
if [[ "${1:-}" == "--mark-applied" ]]; then
    mark_applied=1
    shift
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
# shellcheck source=lib/patches.sh
source "$PROJECT_ROOT/scripts/lib/patches.sh"

if ! git -C "$SERENITY_SRC" rev-parse --git-dir >/dev/null 2>&1; then
    echo "error: SerenityOS checkout not found at $SERENITY_SRC (run scripts/fetch-serenity.sh)" >&2
    exit 1
fi

revision="${1:-$(libwm_pinned_serenity_revision "$PROJECT_ROOT")}"
if ! git -C "$SERENITY_SRC" cat-file -e "$revision^{commit}" 2>/dev/null; then
    echo "error: revision $revision is not in $SERENITY_SRC (fetch it first)" >&2
    exit 1
fi

mapfile -t patches < <(libwm_patch_series "$PROJECT_ROOT")

# Every number must be unique: it is the global application order.
duplicates="$(printf '%s\n' "${patches[@]}" | xargs -n1 basename | cut -c1-4 | sort | uniq -d)"
if [[ -n "$duplicates" ]]; then
    echo "error: duplicate patch numbers: $duplicates" >&2
    exit 1
fi

index="$(mktemp "${TMPDIR:-/tmp}/libwm-patch-index.XXXXXX")"
trap 'rm -f "$index"' EXIT
export GIT_INDEX_FILE="$index"
git -C "$SERENITY_SRC" read-tree "$revision"

echo "==> applying ${#patches[@]} patches to $(git -C "$SERENITY_SRC" rev-parse --short "$revision")"
failures=0
for patch in "${patches[@]}"; do
    name="${patch#"$PROJECT_ROOT"/patches/}"
    if output="$(git -C "$SERENITY_SRC" apply --cached --recount -C1 "$patch" 2>&1)"; then
        printf '%-70s ok\n' "$name"
    else
        printf '%-70s FAILED\n' "$name"
        printf '%s\n' "$output" | sed 's/^/    /'
        failures=$((failures + 1))
    fi
done

if [[ "$failures" -gt 0 ]]; then
    echo "==> $failures patch(es) do not apply; later patches were checked against a tree without them" >&2
    exit 1
fi
echo "==> all ${#patches[@]} patches apply cleanly"

if [[ "$mark_applied" == 1 ]]; then
    unset GIT_INDEX_FILE
    if [[ "$(git -C "$SERENITY_SRC" rev-parse HEAD)" != "$(git -C "$SERENITY_SRC" rev-parse "$revision^{commit}")" ]]; then
        echo "error: --mark-applied: $SERENITY_SRC is not checked out at $revision" >&2
        exit 1
    fi
    libwm_patch_set_fingerprint "$SERENITY_SRC" "$PROJECT_ROOT" > "$(libwm_patch_marker "$SERENITY_SRC")"
    echo "==> recorded the series as applied to $SERENITY_SRC"
fi
