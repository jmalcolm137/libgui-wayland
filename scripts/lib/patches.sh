# Shared helpers for the SerenityOS patch series (sourced, not executed).
#
# Patches live in patches/<group>/NNNN-<name>.patch. The group is only for
# finding things; the four-digit number is the global application order, which
# matters because several patches touch the same files.

# Print the patch series of the project at $1, one path per line, in
# application order (sorted by file name, i.e. by number, across all groups).
libwm_patch_series() {
    find "$1/patches" -mindepth 2 -maxdepth 2 -name '[0-9][0-9][0-9][0-9]-*.patch' -printf '%f\t%p\n' \
        | LC_ALL=C sort | cut -f2-
}

# The pinned SerenityOS revision from scripts/fetch-serenity.sh.
libwm_pinned_serenity_revision() {
    grep -m1 '^SERENITY_REVISION=' "$1/scripts/fetch-serenity.sh" | grep -oE '[0-9a-f]{40}' | head -1
}

# Where build-libgui.sh records which series is applied to the checkout at $1.
libwm_patch_marker() {
    echo "$(git -C "$1" rev-parse --absolute-git-dir)/sde-patch-set-applied"
}

# A hash of the checkout's HEAD ($1) and of every patch's name and contents
# (project at $2): it changes whenever the series or the pin does. Names, not
# paths, so the development tree and a packaging $srcdir agree.
libwm_patch_set_fingerprint() {
    {
        git -C "$1" rev-parse HEAD
        local patch
        while IFS= read -r patch; do
            printf '%s ' "$(basename "$patch")"
            sha256sum "$patch" | cut -d' ' -f1
        done < <(libwm_patch_series "$2")
    } | sha256sum | cut -d' ' -f1
}
