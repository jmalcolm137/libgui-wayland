# Testing

LibWM is tested by running real LibGUI programs (small test programs in
`src/test/` and unmodified SerenityOS applications) against a headless Wayland
compositor, driving them with scripted input, and checking what they report and
what they draw. Upstream has no `Tests/LibGUI`, so there is no lower-level oracle
for GUI behaviour.

## Running

```sh
scripts/check.sh                                        # build, then every suite (what CI runs)
ctest --test-dir build/lagom -L '^libwm$' --output-on-failure
ctest --test-dir build/lagom -L integration              # by label
ctest --test-dir build/lagom -R libwm.menu               # one suite
KEEP=1 scripts/run-headless-tests.sh                     # one suite directly, keeping its logs
```

Each suite is a script in `scripts/` registered with CTest in
`src/test/CMakeLists.txt`. It builds what it needs, starts its own compositor in
a private `XDG_RUNTIME_DIR`, and exits non-zero if any check fails. The suites
invoke ninja on the build tree, so CTest runs them one at a time.

## Suites

| CTest name | Script | Labels | Covers |
|---|---|---|---|
| `libwm.headless` | `run-headless-tests.sh` | integration | presentation, input (pointer, wheel, XKB keys, double-click, held-key repeat), cursors, min size, alpha regions, opaque region, interactive resize (`xdg_toplevel.resize`, and the no-serial/none cases), outgoing and incoming drag-and-drop (including bad and stalled peers), long titles, IME, connection loss |
| `libwm.window-and-portal` | `run-window-and-portal-tests.sh` | integration | `libwm-unit-test` (MenuController, clipboard image transcoding, Config persistence, thread safety, scale inference), fullscreen/maximize/resize, HiDPI at 1x and 2x, CSD, launching through `Desktop::AppFile`, the Calculator |
| `libwm.clipboard` | `run-clipboard-test.sh` | integration | clipboard and primary selection in both directions with a native peer: text aliases, HTML, URI lists, PNG ↔ Serenity images |
| `libwm.menu` | `run-menu-test.sh` | integration | menubar, popups, submenus, keyboard navigation, item activation, the About dialog; menubar pixels at 1x, 2x and after a resize |
| `libwm.pdfviewer` | `run-pdfviewer-test.sh` | integration, apps | the unmodified PDFViewer: rendering, toolbar, Config and FileSystemAccess portals, opening a file, and zooming and wheel scrolling (measured from window dumps: the page grows, then moves) |
| `libwm.settings-portal` | `run-settings-portal-test.sh` | integration | desktop appearance from `org.freedesktop.portal.Settings` (a private session bus with a fake portal): dark applied, then a live switch to light; **skipped** (exit 77) when the build has no libsystemd |
| `libwm.gl` | `run-gl-test.sh` | integration, gl | client-side GL with LibWM's own program: LibGL renders a solid colour with `EGLGPU` (Mesa surfaceless when headless, no silent fallback) and with `LibSoftGPU`, painted in a LibGUI window and checked in its dump |
| `libwm.3dfileviewer` | `run-3dfileviewer-test.sh` | integration, apps, gl | the full 3DFileViewer application; **skipped** (exit 77) when the build has no `3DFileViewer` target, which only a desktop's app set adds |
| `libwm.triage` | `test-triage-failure.sh` | tools | the build-failure triage script, offline |
| `libwm.patches` | `check-patches.sh` | tools | the patch series applies, in order, to the pinned SerenityOS revision |
| `libwm.upstream-libgfx` | `run-lib-tests.sh` | upstream | upstream LibGfx host test suites the rendering stack rests on |

## The headless compositor

The compositor is `headless-compositor` from the
[xlib-wayland](https://github.com/jmalcolm137/xlib-wayland) project, a single-file
`libwayland-server` compositor with no X11 dependency
(`scripts/fetch-headless-compositor.sh`, `scripts/build-headless-compositor.sh`).
It gives the tests:

* a script of pointer and keyboard input, resizes and IME events (`--input`);
* frame capture to PNG (`--output`);
* deterministic configures.

It offers no `xdg-decoration` (so LibWM draws client-side decorations) and no
fractional scale; HiDPI is tested with `LIBWM_CLIENT_SCALE`. Native peers for
clipboard and drag-and-drop tests are `tools/wl-clipboard-peer.c`.

## How checks work

Most checks match lines in the program's log. LibWM's informational lines are
written by `LibWM::trace()` only when `LIBWM_TRACE` is set; every suite exports
`LIBWM_TRACE=1`. Test programs log their own observations (`TestWindow: …`).
Pixel checks read the captured PNG with `scripts/lib/png.py` (standard library
only, since CI has no Pillow).

When adding behaviour, add a check to the closest suite: make the program do the
thing (usually in `src/test/TestWindow.cpp` or a compositor input script), then
match the trace line or program output. A new suite is a script plus one
`libwm_add_script_test()` line.

## CI

`.github/workflows/ci.yml` runs `scripts/check.sh` in an Arch Linux container
(matching the development toolchain), with a compiler cache, on every push and
pull request. On failure it uploads each suite's runtime directory (logs,
captured frames).
