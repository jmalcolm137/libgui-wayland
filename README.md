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
  **unmodified Calculator** builds and runs, decorated and movable, on KDE Plasma. Held keys
  repeat client-side from `wl_keyboard.repeat_info`, as Wayland intends, so repeat works the
  same under every compositor and does not double with toolkits that already repeat.
- **M4** — theme, fonts, decorations, icons, alpha: done. The system theme and fonts come from
  the bundled `Base/res` or the desktop's broker; windows get server-side decorations
  (`zxdg_decoration_manager_v1`) or LibWM's own 22px client-side titlebar with
  close/maximize/minimize; icons use `xdg-toplevel-icon-v1`, with `serenity-window` as the
  fallback for older compositors; windows can have per-pixel
  alpha with a click-through hit threshold; pointers show the SerenityOS cursor theme.
- **M5** — PDFViewer: done. The **unmodified SerenityOS PDFViewer** builds under
  Lagom and renders PDFs on the host. Opening a file goes through an in-process
  **FileSystemAccess** portal (including the real **File → Open** picker, shown on the main
  thread from the portal thread), and it reads/writes its preferences through an in-process
  **Config** portal; both are served by LibWM without touching library source. Verified
  headlessly against `Tests/LibPDF/*.pdf` (window, menubar, page toolbar, the rendered page,
  a typed-path picker open, double-clicking to traverse a directory and open a file, and an
  interactive resize; `scripts/run-pdfviewer-test.sh`). Zooming is asserted from window dumps
  (the page grows when zoomed in). The wheel-scroll measurement is **parked**: CI's runs leave
  the captured frame unchanged although the wheel is delivered, so it is printed but not
  counted until it measures the scrollbar rather than raw pixels ([docs/testing.md](docs/testing.md)).
- **M6** — clipboard and menus: done for `text/plain`, `text/html`, `text/uri-list`, and
  `image/png` ↔ `image/x-serenityos`, including multiple representations on one clipboard item
  (all verified headlessly in both directions), plus a
  server-rendered **menubar and dropdown menus** with hover highlighting (clicking `File → Quit`
  works; `Help → About` opens the dialog), **nested submenus**, **menu item icons**, **long-menu
  scrolling**, and full keyboard navigation (`F10`/`Alt`+accelerator, arrows, `Enter`, `Escape`).
  Window state (`fullscreen`/`maximize`/`minimize`) is in, including **interactive resize**:
  dragging a statusbar `ResizeCorner` becomes `xdg_toplevel.resize`, so the compositor takes the
  pointer and drives the resize (`RESIZING` configures stretch the last frame while the client
  re-renders). Config INI persistence and SerenityOS `/res/...` path redirection are in too. The
  menubar is rendered on the LibWM server thread and handed to the Wayland side as an image, so
  it is never drawn from under the menu model.
- **M7** — testing and CI: done. Eleven CTest suites (input and drag-and-drop, window state and
  portals, cursor warping over `wp_pointer_warp_v1`, clipboard and primary selection, menus,
  PDFViewer, **client-side GL**, the desktop appearance portal, patch series, triage tooling,
  upstream LibGfx) run against a headless Wayland compositor with scripted input and frame
  capture.
  `.github/workflows/ci.yml` builds and runs them in an Arch container on every push, with a
  compiler cache and log artifacts on failure. The GL suite uses LibWM's own test program, so the
  `EGLGPU` backend is covered here; `3DFileViewer` (desktop-provided) remains the full-application
  check.
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
- **M10** — GPU/OpenGL: done for the client-side stack. `EGLGPU` uses Mesa's surfaceless
  platform when there is no default EGL display, so it also runs headless (CI). `LibGL` → `LibGPU`/`LibGLSL` builds on the
  host unchanged, with two `GPU::Device` backends: **`EGLGPU`** (Mesa through EGL, the default)
  and **`LibSoftGPU`** (CPU). `EGLGPU` falls back to `LibSoftGPU` when no EGL context can be
  created, so GL always works. GL renders into an **offscreen `Gfx::Bitmap`** which LibGUI paints
  and LibWM presents as a `wl_shm` buffer, so the Wayland transport is untouched (the
  WindowServer protocol carries no GL messages). The **unmodified `3DFileViewer`** loads and
  animates an OBJ model; the **`Tubes`** demo builds against the same path.
- **M11** — primary selection: done. SerenityOS has one clipboard, native toolkits also have
  PRIMARY (middle-click). LibWM publishes a Serenity copy as both CLIPBOARD and PRIMARY, and
  falls back to PRIMARY when nothing owns CLIPBOARD, so text crosses in both directions between
  Serenity applications and native ones (`zwp_primary_selection_v1`). wlroots-based compositors
  only deliver PRIMARY to the focused client, which is what a paste does.
- **M12** — window icons: done. `GUI::Window::set_icon` reaches taskbars and window switchers
  through the standard `xdg-toplevel-icon-v1` (LibWM uploads the bitmap as an shm icon buffer at
  its scale), falling back to `serenity_toplevel.set_icon` on compositors without it. Verified
  against the SerenityDE compositor, whose Taskbar draws the icon from its WindowManagerServer
  updates.
- **M13** — desktop appearance: done. When no desktop sends a theme over `serenity-window`,
  LibWM reads `org.freedesktop.appearance color-scheme` from the standard
  `org.freedesktop.portal.Settings` and applies the bundled `Dark`, `Light` or `Default` theme,
  following live changes; a desktop-provided theme still wins. Fonts, pointer/keyboard settings
  and the display layout have no portable API and remain the desktop's own.
- **Applications** — the framework builds and tests **Calculator**, **PDFViewer** and **Piano**
  from unmodified source. The rest of `Userland/Applications` is built by the desktop that ships
  it (see [Desktops](#desktops)), with GML, icons, menus, dialogs and syntax highlighting intact.
  The web stack (`LibWeb`/`LibWebView`) is built on the host too (`-DENABLE_LAGOM_LIBWEB=ON`).

## Remaining work

Grouped by who can act on it. [SerenityDE](https://github.com/jmalcolm137/serenity-desktop-environment)
is the reference session; what it provides is listed as such, not as a defect. Full method-level
detail is in [docs/protocol-mapping.md](docs/protocol-mapping.md).

### In LibWM (affects the SerenityDE session too)

- **GPU presentation and shaders.** `EGLGPU` renders on the GPU but reads the result back with
  `glReadPixels` into a `Gfx::Bitmap` and presents over `wl_shm`; zero-copy
  `zwp_linux_dmabuf_v1` is not implemented, and a compositor cannot render GL for a client.
  `EGLGPU` also cannot run shader programs (`create_shader` returns an error), so programs that
  need GLSL must use `LibSoftGPU`. See [docs/development-history.md](docs/development-history.md)
  §4.11.
- **WindowServer requests Wayland cannot express.** All of them are handled explicitly rather
  than silently dropped: `refresh_system_theme` reloads the theme, base size becomes the window's
  minimum size, `set_global_cursor_position` goes through `wp_pointer_warp_v1` (the staging
  protocol KDE co-authored and SerenityDE implements), and `show_screen_numbers` is forwarded to
  the desktop as `effects.show_screen_numbers` (the compositor owns output overlays). Size
  increments, aspect ratios, forced shadows, global mouse tracking and window stealing have **no**
  Wayland protocol: `xdg-shell` has no increments or aspect constraints, a shadow is drawn outside
  the surface, `wl_pointer` only reports over the client's own surfaces (Plasma and other
  compositors solve window dragging with compositor-driven `xdg_toplevel.move`/`resize`, which
  LibWM already uses), and a client owns its surfaces while the compositor owns stacking. The
  per-request rationale is in [docs/protocol-mapping.md](docs/protocol-mapping.md). `is_minimized`
  remains LibWM's own record, set by `set_minimized` and cleared when the compositor activates the
  window.
- **Clean-ups.** SerenityDE's file handling now lives in the desktop (the shim only reads the
  host settings and `serenity-window`), and `WaylandClient`'s C protocol handlers are private
  behind the `WaylandEvents` friend, leaving only the API its callers use public. `create_window`
  and `create_popup` are split into their role-specific helpers
  (`create_layer_window`/`create_overlay_window`/`create_toplevel_window`, and
  `create_layer_popup`/`create_xdg_popup`), and `commit_window_content` is now a short
  coordinator over `present_composed`, `present_fractional` and `present_direct`.

### Provided by the desktop, not implemented portably

The SerenityDE session supplies these over `serenity-window` or in its compositor. They are not
gaps in that session, but applications lose them on a third-party compositor (KDE, GNOME, sway),
where the portable protocol or portal is not implemented:

- **Screenshots and the Magnifier.** `get_screen_bitmap` and `get_screen_bitmap_around_*` ask the
  compositor for a capture over `serenity-window`; elsewhere they return an empty image
  (`org.freedesktop.portal.Screenshot` would be the portable route).
- **Fonts, pointer/keyboard and display settings, wallpaper, applet layout.** There is no
  portable API for any of these: the Settings portal exposes appearance only (handled above),
  and input/display configuration is compositor-specific (KDE's KScreen, GNOME's Mutter
  settings, wlroots' output-management protocol). They arrive as `serenity-window` host settings
  in a SerenityDE session; elsewhere the bundled fonts, default input behaviour and
  compositor-owned layout apply. Wallpaper and applet layout are desktop features by design.
- **Raising a window on launch.** `xdg-activation-v1` is not bound. SerenityDE's focus policy
  covers the session; elsewhere a launcher-spawned window may not be raised.
- **Pointer lock** (`pointer-constraints`/`relative-pointer`, for mouse-look games) and **idle
  inhibit** (for video playback) are not used by LibWM, and **touch input** (`wl_touch`) is not
  handled at all.
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
* **Requests with no client protocol.** Screen-number overlays (`show_screen_numbers`), global
  mouse tracking and shadows around a surface are compositor features; the shim forwards the
  screen-number request as the `effects.show_screen_numbers` host setting, and the desktop is the
  right place to draw it. Pointer warping is the exception: it has a protocol
  (`wp_pointer_warp_v1`), implemented by SerenityDE, and the shim uses it.
* **Standard protocols the desktop only has to advertise.** Window icons
  (`xdg-toplevel-icon-v1`), primary selection (`zwp_primary_selection_v1`), the appearance
  preference (`org.freedesktop.portal.Settings`, via `xdg-desktop-portal`) and the usual
  `xdg-shell`/`wl_data_device`/`wl_seat` surfaces mean a desktop's taskbar, window switcher,
  middle-click paste and light/dark mode work with Serenity applications exactly as with native
  ones. SerenityDE's compositor turns the standard icon into its Taskbar button and sends its
  theme directly (which takes precedence); `serenity-window`'s icon event is only used where
  `xdg-toplevel-icon-v1` is absent, and the portal only where no theme is sent at all.

The [Serenity Desktop Environment](https://github.com/jmalcolm137/serenity-desktop-environment)
is that desktop and the reference session: its compositor, Taskbar/Desktop/applets and session
services supply what the *Remaining work* section marks as desktop-provided (screenshots,
theme/input/display settings, wallpaper and applet layout), and they consume the standard
protocols above, so Serenity applications' icons and selections appear in its Taskbar and
clipboard just like native ones'.

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

