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
invoke ninja on the build tree, so CTest runs them one at a time. The clients are
the programs in `src/test/`; the SerenityOS applications are the desktop's and
are tested against the desktop's compositor, not here.

## Suites

| CTest name | Script | Labels | Covers |
|---|---|---|---|
| `libwm.mouse-tracking` | `run-mouse-tracking-test.sh` | integration | global mouse tracking: a `GUI::MouseTracker` receives output positions while the pointer is inside the window and after it moves outside it (`ext-input-observation-v1`), and stops when the tracker is destroyed |
| `libwm.headless` | `run-headless-tests.sh` | integration | presentation, input (pointer, wheel, XKB keys, double-click, held-key repeat), cursors, min size, alpha regions, opaque region, interactive resize (`xdg_toplevel.resize`, and the no-serial/none cases), outgoing and incoming drag-and-drop (including bad and stalled peers), long titles, IME, connection loss |
| `libwm.window-and-portal` | `run-window-and-portal-tests.sh` | integration | `libwm-unit-test` (MenuController, clipboard image transcoding, Config persistence, thread safety, scale inference, cursor-warp surface geometry), fullscreen/maximize/resize, HiDPI at 1x and 2x (including a CSD titlebar composed through the fractional path), CSD, `set_global_cursor_position` end to end over `wp_pointer_warp_v1`, every remaining WindowServer request with no Wayland equivalent (each is traced, including the base-size→minimum-size mapping, aspect/shadow/flash no-ops, screen-number forwarding and a theme refresh), and launching an in-repo target through `Desktop::AppFile` and `Desktop::Launcher` |
| `libwm.clipboard` | `run-clipboard-test.sh` | integration | clipboard and primary selection in both directions with a native peer: text aliases, HTML, URI lists, PNG ↔ Serenity images |
| `libwm.menu` | `run-menu-test.sh` | integration | menubar, popups, submenus, keyboard navigation, item activation, the About dialog, with LibWM's own menubar test app; menubar pixels at 1x, 2x and after a resize |
| `libwm.pdf` | `run-pdf-test.sh` | integration | PDF rendering with LibWM's own app: opening through the FileSystemAccess portal, LibPDF rendering, zoom via `Ctrl+=`, wheel scrolling and a compositor resize, all asserted from the app's traces and the frame dump |
| `libwm.settings-portal` | `run-settings-portal-test.sh` | integration | desktop appearance from `org.freedesktop.portal.Settings` (a private session bus with a fake portal): dark applied, then a live switch to light; **skipped** (exit 77) when the build has no libsystemd |
| `libwm.gl` | `run-gl-test.sh` | integration, gl | client-side GL with LibWM's own program: LibGL renders a solid colour with `EGLGPU` (Mesa surfaceless when headless, no silent fallback) and with `LibSoftGPU`, painted in a LibGUI window and checked in its dump |
| `libwm.3d` | `run-3d-test.sh` | integration, gl | an animated client-side 3D scene with LibWM's own app: LibGL renders frames into an offscreen bitmap, LibGUI paints them and LibWM presents them; the traces and the frame dump are checked |
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
* deterministic configures;
* `wp_pointer_warp_v1` (with the enter-serial validation a real compositor
  does), so `set_global_cursor_position` has an end-to-end test: the pointer
  enters the window, the request warps it, and the client receives the
  resulting motion (the compositor logs `HC: pointer warp to X,Y` with
  `HC_TRACE=1`);
* `ext-input-observation-v1` (signed 16:16 fixed point, surface-relative), so
  `set_global_mouse_tracking` is tested with the pointer inside and outside the
  window.

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
