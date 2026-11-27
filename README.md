# libgui-wayland

Run **unmodified SerenityOS `LibGUI`/`LibGfx` applications** natively on **Wayland** (no
SerenityOS WindowServer).

The [Serenity Desktop Environment](https://github.com/jmalcolm137/serenity-desktop-environment)
and its applications (Calculator, PDFViewer, …) run on this framework.

The shim is called **LibWM**. It sits between `LibGUI`'s WindowServer IPC protocol and
`libwayland-client`: the application side is Serenity, the system side is Wayland.

* **Philosophy:** keep `LibGfx`, the full `LibGUI`, and the applications byte-for-byte
  unmodified; implement the WindowServer's server side on top of Wayland.
* **Process model:** in-process — each application is its own Wayland client.
* **Build:** SerenityOS's Lagom host build, extended to compile the **full** LibGUI (not the
  six-file Lagom stub).

A good demo application is the Ladybird SDE web browser [Flatpak avaiable](https://github.com/jmalcolm137/ladybird-sde/releases)

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
[docs/limitations.md](docs/limitations.md); what a desktop adds (session services,
settings and chrome, the protocols it advertises) is in [docs/desktops.md](docs/desktops.md).

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

