# libgui-wayland

Run **unmodified SerenityOS `LibGUI`/`LibGfx` applications** natively on **Wayland** (no
SerenityOS WindowServer). The applications (Calculator, PDFViewer, …) are built and tested by
the desktop that ships them; this repository is the framework they run on.

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

Status, milestones and remaining work are in [docs/status.md](docs/status.md); platform
limitations, non-goals and what the desktop provides are in
[docs/limitations.md](docs/limitations.md).

## Desktops

libgui-wayland is desktop-agnostic: the applications run on any Wayland compositor. What a
desktop adds is its own business, built on the same pinned SerenityOS source:

* **The applications it ships.** The applications themselves are the desktop's: it defines
  their build targets, carries their host patches and assets, and tests them against its own
  compositor (SerenityDE: `patches/0008-sde-lagom-targets.patch`, `scripts/build-apps.sh`,
  `scripts/run-libgui-apps-test.sh`). Applications that read SerenityOS kernel interfaces
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
* **Requests with no client protocol.** Screen-number overlays (`show_screen_numbers`) and shadows
  around a surface are compositor features; the shim forwards the screen-number request as the
  `effects.show_screen_numbers` host setting, and the desktop is the right place to draw it. Pointer
  warping and global mouse tracking are the exceptions: they have protocols
  (`wp_pointer_warp_v1`, `ext-input-observation-v1`), implemented by SerenityDE, and the shim uses
  both.
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
services supply what [docs/limitations.md](docs/limitations.md) lists as desktop-provided
(screenshots, theme/input/display settings, wallpaper and applet layout), and they consume the
standard protocols above, so Serenity applications' icons and selections appear in its Taskbar
and clipboard just like native ones'.

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
ctest --test-dir build/lagom -L integration                      # by label: integration, gl, tools, upstream
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

