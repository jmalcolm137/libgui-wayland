# SerenityOS patches

These are the changes libgui-wayland makes to the pinned SerenityOS checkout
(`scripts/fetch-serenity.sh`) so its libraries and applications build and run on
a Linux host. `scripts/build-libgui.sh` applies them; changes that make sense for
SerenityOS on its own belong in the fork's `fixes` branch instead (see
`fetch-serenity.sh`), and SerenityDE keeps its own series in its repository.

## Layout and order

Each patch is `patches/<group>/NNNN-<name>.patch`. The group is only for
finding things:

| Group | Contents |
|---|---|
| `lagom-and-resources` | the Lagom build (which libraries and services it builds), IPC additions, resource paths and bundled resources |
| `libcore` | host seams in LibCore: portals, processes, event loop teardown, session paths |
| `libipc` | LibIPC: send bounds, connection loss, threading |
| `libgfx` | LibGfx host fixes, resource roots, HiDPI painting |
| `libgui` | LibGUI: HiDPI backing stores, clipboard, damage reporting, teardown |
| `libvt` | LibVT (the terminal widget) correctness and performance |
| `other-libraries` | audio, EGL GPU backend, LibDesktop, LibLaunch, ImageDecoder, LibTLS, LibKeyboard, LibDebug/LibELF, LibAccessibility |
| `applications` | Linux equivalents in individual applications (Settings apps, FileManager, SystemMonitor, debugger, ...) |

The four-digit number is the **global** application order across all groups:
several patches touch the same files, so a patch is written against the tree
with every lower-numbered patch applied. A new patch takes the next unused
number (gaps are patches that were retired or upstreamed; numbers are not
reused). Moving a patch between groups does not change the series.

## Checking and refreshing

```sh
scripts/check-patches.sh          # does the whole series apply, in order, to the pin?
```

It applies the series to a throwaway git index of the pinned revision, so it is
fast and leaves `serenity/` alone. It also runs as the `libwm.patches` CTest
test, so CI catches a patch that drifts.

To change a patch, edit the files in `serenity/` (where the series is already
applied), rewrite that patch's hunks from `git -C serenity diff -- <its files>`,
then:

```sh
scripts/check-patches.sh --mark-applied
```

That checks the series against the pin and, if it applies, records it as the
one applied to `serenity/` (`build-libgui.sh` keeps a hash of the pin and of
every patch's name and contents in `serenity/.git/sde-patch-set-applied`).
Without it, `build-libgui.sh` sees a changed series and re-verifies each patch
in place, which it cannot do for patches whose hunks overlap later ones, so it
stops. `--mark-applied` trusts that your working tree really has the series
applied; it cannot check, since SerenityDE's own series edits some of the same
files.

A fresh checkout needs none of this: `build-libgui.sh` applies the series in
order.
