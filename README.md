# libgui-wayland

Run **unmodified SerenityOS `LibGUI`/`LibGfx` applications** (e.g. Calculator, PDFViewer)
natively on **Wayland** (no SerenityOS WindowServer).

The shim is called **LibWM**. It sits between `LibGUI`'s WindowServer IPC protocol and
`libwayland-client`: the application side is Serenity, the system side is Wayland.

* **Philosophy:** keep `LibGfx`, the full `LibGUI`, and the applications byte-for-byte
  unmodified; implement the WindowServer's server side on top of Wayland.
* **Process model:** in-process — each application is its own Wayland client.
* **Build:** SerenityOS's Lagom host build, extended to compile the **full** LibGUI (not the
  six-file Lagom stub).

## Guide

[**Building GUI Applications for Wayland**](docs/guide/LibGUI-Wayland-Guide.pdf) is a
hands-on, application-focused guide to writing LibGUI applications that run as native
Wayland clients on any compositor. It includes four complete example applications
(`examples/`), does not rely on any particular desktop, and treats the platform
layer as an in-process implementation detail rather than part of the application
programming model.

## Principles

1. **The compositor is the source of truth.** Query the real Wayland compositor for outputs,
   scale, seat, clipboard and window state, and share client state back — never hardcode
   environment values.
2. **Use AK and SerenityOS conventions.** LibWM is Serenity userland on Linux: AK types,
   `ErrorOr`/`TRY`, `dbgln`, `C_OBJECT`, Serenity style — no `std::` or third-party
   abstractions where AK suffices.

## Status

Everything below is for the pinned SerenityOS revision (`scripts/fetch-serenity.sh`); the host
changes are the patch series in `patches/`, which `scripts/check-patches.sh` applies to that pin
on every run of the test suites.

- **M0** — architecture/design + audit: done (see [DESIGN.md](DESIGN.md)).
- **M1** — full `LibGfx` + **full** `LibGUI` host build: done. `liblagom-gui.so`
  (132 objects, 3063 GUI symbols) builds from unmodified SerenityOS source via small
  Lagom/LibCore patches.
- **M2** — LibWM vertical slice: done. An unmodified-style LibGUI window renders through the
  in-process WindowServer protocol and appears as a real Wayland toplevel. Screen geometry is
  taken from `wl_output`.
- **M3** — input + Calculator: done. Pointer/keyboard come from `wl_seat` (xkbcommon →
  Serenity `KeyCode`/modifiers), focus/activation/close handled, and the full input path is
  verified headlessly (the compositor injects a click; the widget receives it). The
  **unmodified Calculator** builds and runs, decorated and movable, on KDE Plasma.
- **M4** — theme, fonts, decorations, icons, alpha: done. The system theme and fonts come from
  the bundled `Base/res` or the desktop's broker; windows get server-side decorations
  (`zxdg_decoration_manager_v1`) or LibWM's own 22px client-side titlebar with
  close/maximize/minimize; icons are sent over `serenity-window`; windows can have per-pixel
  alpha with a click-through hit threshold; pointers show the SerenityOS cursor theme.
- **M5** — PDFViewer: done. The **unmodified SerenityOS PDFViewer** builds under
  Lagom and renders PDFs on the host. Opening a file goes through an in-process
  **FileSystemAccess** portal (including the real **File → Open** picker, shown on the main
  thread from the portal thread), and it reads/writes its preferences through an in-process
  **Config** portal; both are served by LibWM without touching library source. Verified
  headlessly against `Tests/LibPDF/*.pdf` (window, menubar, page toolbar, the rendered page,
  a typed-path picker open, double-clicking to traverse a directory and open a file, and an
  interactive resize; `scripts/run-pdfviewer-test.sh`).
- **M6** — clipboard and menus: done for `text/plain`, `text/html`, `text/uri-list`, and
  `image/png` ↔ `image/x-serenityos`, including multiple representations on one clipboard item
  (all verified headlessly in both directions), plus a
  server-rendered **menubar and dropdown menus** with hover highlighting (clicking `File → Quit`
  works; `Help → About` opens the dialog), **nested submenus**, **menu item icons**, **long-menu
  scrolling**, and full keyboard navigation (`F10`/`Alt`+accelerator, arrows, `Enter`, `Escape`).
  Window state (`fullscreen`/`maximize`/`minimize`), Config INI persistence, and SerenityOS
  `/res/...` path redirection are in. The menubar is rendered on the LibWM server thread and
  handed to the Wayland side as an image, so it is never drawn from under the menu model.
- **M7** — testing and CI: done. Nine CTest suites (input and drag-and-drop, window state and
  portals, clipboard, menus, PDFViewer, 3DFileViewer/GL, patch series, triage tooling, upstream
  LibGfx) run against a headless Wayland compositor with scripted input and frame capture.
  `.github/workflows/ci.yml` builds and runs them in an Arch container on every push, with a
  compiler cache and log artifacts on failure. The GL suite is skipped in CI because
  `3DFileViewer` is added to the build by the desktop's application set, not by this repository
  (see Remaining work).
- **M8** — HiDPI and fractional scaling: done. LibWM binds `wp_fractional_scale_v1`
  (`preferred_scale` in `1/120` units), infers fractional ratios from `wl_output` physical mode +
  `zxdg_output_v1` logical size, and supports explicit integer or fractional overrides via
  `LIBWM_CLIENT_SCALE` (e.g. `1.5`, `1.75`, `2`, or `0` to force `1x`). At integer scales the
  client backing store is presented directly with `wl_surface.set_buffer_scale`. At fractional
  scales the client renders its backing store at `ceil(scale)` and LibWM resamples into a
  fractional physical buffer presented with `wl_surface.set_buffer_scale(1)` and
  `wp_viewport_set_destination(logical_width, logical_height)`.
- **M9** — audio: done as far as LibWM is concerned. `LibAudio` stays upstream: a
  compositor/desktop-agnostic client library that on Linux does its own output over
  **PulseAudio** for `Audio::PlaybackStream` clients. Applications that speak the SerenityOS
  **AudioServer / AudioManager protocols** directly (Piano, SoundPlayer, `aplay`, the Audio
  applet) are served by an out-of-process **`AudioServer`** that the desktop provides; LibWM
  spawns it on demand from `LIBWM_APP_BIN_DIR`.
- **M10** — GPU/OpenGL: done for the client-side stack. `LibGL` → `LibGPU`/`LibGLSL` builds on the
  host unchanged, with two `GPU::Device` backends: **`EGLGPU`** (Mesa through EGL, the default)
  and **`LibSoftGPU`** (CPU). `EGLGPU` falls back to `LibSoftGPU` when no EGL context can be
  created, so GL always works. GL renders into an **offscreen `Gfx::Bitmap`** which LibGUI paints
  and LibWM presents as a `wl_shm` buffer, so the Wayland transport is untouched (the
  WindowServer protocol carries no GL messages). The **unmodified `3DFileViewer`** loads and
  animates an OBJ model; the **`Tubes`** demo builds against the same path.
- **Applications** — the framework builds and tests **Calculator**, **PDFViewer** and **Piano**
  from unmodified source. The rest of `Userland/Applications` is built by the desktop that ships
  it (see [Desktops](#desktops)), with GML, icons, menus, dialogs and syntax highlighting intact.
  The web stack (`LibWeb`/`LibWebView`) is built on the host too (`-DENABLE_LAGOM_LIBWEB=ON`).

## Remaining work

Grouped by who can act on it. [SerenityDE](https://github.com/jmalcolm137/serenity-desktop-environment)
is the reference session; what it provides is listed as such, not as a defect. Full method-level
detail is in [docs/protocol-mapping.md](docs/protocol-mapping.md).

### In LibWM (affects the SerenityDE session too)

- **Interactive resize.** Dragging LibGUI's statusbar resize corner sends
  `start_window_resize`, which LibWM does not implement, so the corner does nothing. The fix is to
  map it to `xdg_toplevel.resize` and let the compositor drive the resize;
  `set_window_base_size_and_size_increment` and `set_window_resize_aspect_ratio` could constrain
  the resulting configures. `patches/libgui/0093` lets an application hide the corner in the
  meantime, but nothing calls it yet.
- **GPU presentation and shaders.** `EGLGPU` renders on the GPU but reads the result back with
  `glReadPixels` into a `Gfx::Bitmap` and presents over `wl_shm`; zero-copy
  `zwp_linux_dmabuf_v1` is not implemented, and a compositor cannot render GL for a client.
  `EGLGPU` also cannot run shader programs (`create_shader` returns an error), so programs that
  need GLSL must use `LibSoftGPU`. See [docs/development-history.md](docs/development-history.md)
  §4.11.
- **No GL coverage in CI.** `3DFileViewer` is added by the desktop's application set, so this
  repository's CI skips the GL suite. A small in-repo LibGL program that renders into a bitmap
  would exercise `EGLGPU` here.
- **The remaining WindowServer stubs:** `set_forced_shadow` (no CSD drop shadow),
  `set_global_mouse_tracking`, `set_global_cursor_position`, `refresh_system_theme`,
  `show_screen_numbers`, `set_flash_flush`, and the window-stealing calls (which have no meaning
  when each application is its own Wayland client). `is_minimized` is LibWM's own record, set by
  `set_minimized` and cleared when the compositor activates the window.
- **Test gaps.** The PDFViewer suite opens, renders and resizes but does not assert scrolling or
  zooming.
- **Clean-ups.** SerenityDE's file handling (the theme and font broker files, the wallpaper
  store) still lives in `src/WindowServerSettings.cpp` rather than `SerenityShell`;
  `commit_window_content`, `create_window` and `create_popup` are long; the protocol event
  handlers remain public on `WaylandClient`.

### Provided by the desktop, not implemented portably

The SerenityDE session supplies these over `serenity-window` or in its compositor. They are not
gaps in that session, but applications lose them on a third-party compositor (KDE, GNOME, sway),
where the portable protocol or portal is not implemented:

- **Key repeat.** SerenityDE's compositor repeats a held key itself (wlroots 0.20 stopped doing
  it). LibWM does no client-side repeat, so holding a key types once under compositors that leave
  repeat to the client (KDE, GNOME, and wlroots 0.20+); implementing it in LibWM would double up
  under SerenityDE, so it needs a switch.
- **Window icons.** Sent with `serenity_toplevel.set_icon`; `xdg-toplevel-icon-v1` is not bound.
- **Screenshots and the Magnifier.** `get_screen_bitmap` and `get_screen_bitmap_around_*` ask the
  compositor for a capture over `serenity-window`; elsewhere they return an empty image
  (`org.freedesktop.portal.Screenshot` would be the portable route).
- **Theme, fonts, input and display settings, wallpaper, applet layout.** Desktop-owned values
  arrive as `serenity-window` host settings; elsewhere the bundled defaults apply, and there is
  no `org.freedesktop.portal.Settings` bridge.
- **Raising a window on launch.** `xdg-activation-v1` is not bound. SerenityDE's focus policy
  covers the session; elsewhere a launcher-spawned window may not be raised.
- **Pointer lock** (`pointer-constraints`/`relative-pointer`, for mouse-look games) and **idle
  inhibit** (for video playback) are not used by LibWM, and **touch input** (`wl_touch`) is not
  handled at all.
- **Primary selection.** LibWM neither offers nor reads it, so middle-click paste between
  Serenity and native applications does not work; compositors, SerenityDE's included, support
  it. Serenity applications paste explicitly (Ctrl+V), so this matters only for interoperability.

### Upstream or out of scope

- **Audio resampling.** Cross-rate clients use LibAudio's insert/drop resampler (`Resampler.h`
  says "Should do better..."). That is an upstream LibAudio limitation, and mixing is the
  desktop's `AudioServer`.
- **Zero-copy GPU buffers and Vulkan** remain design non-goals ([DESIGN.md](DESIGN.md) §1.2).

## Desktops

libgui-wayland is desktop-agnostic: the applications run on any Wayland compositor. What a
desktop adds is its own business, built on the same pinned SerenityOS source:

* **The applications it ships.** Applications that read SerenityOS kernel interfaces
  (SystemMonitor, Debugger, CrashReporter, the Settings apps, ...) need host ports, and the
  desktop that builds them carries those patches. The libraries they rely on are ported here
  (LibCore's `ProcessStatisticsReader` on `/proc`, LibDebug on `ptrace`, LibCoredump on native
  core dumps), so every desktop shares them.
* **Session services.** A service listening on a portal path (LaunchServer, NotificationServer,
  AudioServer, ...) takes over from LibWM's in-process fallback; `docs/environment.md` lists the
  variables that place them.
* **Desktop-owned settings and chrome.** Pointer, keyboard, cursor, theme, fonts, display layout
  and wallpaper settings, and compositor-drawn window chrome, travel over the optional
  [`serenity-window`](src/protocol/serenity-window.xml) protocol, which this repository owns. A
  compositor that implements it keeps a copy; without it, applications run with defaults.

The [Serenity Desktop Environment](https://github.com/jmalcolm137/serenity-desktop-environment)
is that desktop and the reference session: its compositor, Taskbar/Desktop/applets and session
services supply what the *Remaining work* section marks as desktop-provided, including key
repeat, window icons, screenshots, theme and input settings, and the wallpaper.

## Building

```sh
scripts/fetch-serenity.sh              # pinned, blobless, sparse checkout
scripts/fetch-headless-compositor.sh   # pinned clone of xlib-wayland (pure-Wayland compositor)
scripts/check.sh                       # patches + build + every LibWM test suite (what CI runs)
```

The suites are registered with CTest (`src/test/CMakeLists.txt`), so after a build they can also
be run selectively:

```sh
ctest --test-dir build/lagom -L '^libwm$' --output-on-failure   # everything
ctest --test-dir build/lagom -L integration                      # by label: integration, apps, gl, tools, upstream
ctest --test-dir build/lagom -R libwm.menu                       # one suite
```

`scripts/build-libgui.sh` builds without testing when passed `--no-tests`. Build prerequisites
(Arch package names, as installed by `.github/workflows/ci.yml`): `base-devel git cmake ninja
meson python wayland wayland-protocols libxkbcommon mesa libglvnd systemd-libs elfutils
libunwind`, plus `cairo freetype2 fontconfig pixman libxrender` for the headless test compositor.
Runtime environment variables are listed in [docs/environment.md](docs/environment.md); [docs/testing.md](docs/testing.md) describes the suites and [DESIGN.md](DESIGN.md) links the rest of the documentation.

To keep a log and get a Jev triage card when the build or a test fails:

```sh
scripts/build-with-triage.sh          # same arguments as build-libgui.sh
```

The wrapper tees the run to `build/logs/`, preserves the build's exit status, and on failure
sends a reduced excerpt (errors, failed checks, patch problems, and the tail) to Jev. The card
classifies the failure, names the likely subsystem, and suggests a next action. It needs an
authenticated `jev` (`jev auth set`); when Jev is unavailable the build result is unchanged and
triage is skipped. Set `TRIAGE=0` to capture the log without triage.

`scripts/triage-failure.py` can also triage an existing log directly, with `--json` for
machine-readable output. `scripts/test-triage-failure.sh` checks the extraction, redaction and
Jev-call paths offline (no API key required). Jev is an advisory classifier: treat
low-confidence answers as "investigate", and keep patch application, `grep` checks, and image
comparisons deterministic.

