# LibWM — Running SerenityOS LibGUI/LibGfx Applications on Wayland

**Target:** build **unmodified** SerenityOS `LibGfx` and `LibGUI` applications (e.g.
`Calculator`, `PDFViewer`) on Linux and run them **natively on a Wayland compositor** —
with no SerenityOS WindowServer, no X server, and no `xlib-wayland`.

The SerenityOS revision is pinned in `scripts/fetch-serenity.sh`; the host changes
to it are the patch series in `patches/` (see `patches/README.md`).

This document is the overview: what LibWM is, its goals and principles, and where
everything lives. The details are in `docs/`:

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | how LibWM is built: layering, components, rendering, input, menus, portals and services |
| [docs/threading.md](docs/threading.md) | the two threads, what runs where, locking rules |
| [docs/protocol-mapping.md](docs/protocol-mapping.md) | the Wayland protocols used, and the status of every WindowServer method |
| [docs/testing.md](docs/testing.md) | the test suites, CTest labels, the headless compositor, CI |
| [docs/environment.md](docs/environment.md) | environment variables |
| [docs/development-history.md](docs/development-history.md) | the original audit and milestone notes (historical; §2–§7) |
| [docs/guide/guide.md](docs/guide/guide.md) | the guide for application developers |

---

## 1. Summary

SerenityOS `LibGUI` is not built on X11. A widget toolkit, an application, and the compositor
all live in one system, and the boundary between an application and the system is a **custom IPC
protocol spoken over a Unix socket to the `WindowServer` service**:

```
   Application (Calculator, PDFViewer, …)
        │   LibGUI  (Window, Widget, Menu, Painter, …)
        │
        │   IPC: WindowServerEndpoint  ──────────────►  /tmp/portal/window
        │◄── IPC: WindowClientEndpoint (events) ──────  WindowServer process
        │
   Gfx::Bitmap over a shared memfd  ───────────────►  WindowServer compositor
                                                      └─ software framebuffer → display
```

The whole platform-specific surface of LibGUI is that protocol plus a handful of auxiliary
portals (clipboard, config, image decode, file access, launch, notify). Therefore the strategy
is the same as any good compatibility shim:

> **Keep `LibGfx`, the full `LibGUI`, and the applications byte-for-byte unmodified. Replace the
> WindowServer — the thing on the far side of the protocol — with `LibWM`, a library that speaks
> the WindowServer protocol on one side and Wayland on the other.**

`LibWM` is thus the direct analogue of an X11 shim, but at a *different boundary*: it does not
emulate Xlib, it emulates **SerenityOS's WindowServer**. It is built directly on
`libwayland-client` + `wl_shm`, not on `xlib-wayland` or any X11 machinery.

Because the Serenity client model is already "render into a shared bitmap, hand it to the
compositor, receive input events", the mapping onto Wayland is unusually direct:

| SerenityOS WindowServer | LibWM / Wayland |
|---|---|
| client `Gfx::Bitmap` shared over memfd | `wl_shm_pool` created from that same fd → `wl_buffer` |
| one WindowServer window | one `wl_surface` + `xdg_toplevel` |
| menu / popup window | `wl_surface` + `xdg_popup` + `xdg_positioner` |
| window frame (server-drawn) | `zxdg_decoration_manager_v1`, or client-side decorations drawn by LibWM |
| mouse/key events from compositor | `wl_pointer` / `wl_keyboard` → `WindowClientEndpoint` events |
| screen geometry | `wl_output` |
| clipboard | `wl_data_device` |
| `invalidate_rect` → `paint` | `wl_surface.frame` callbacks + damage |
| theme/fonts pushed to clients (`FastGreet`) | bundled Serenity `Base/res` + `Gfx::load_system_theme` |

### 1.1 Goals

* **G1** — Build the **full** `LibGfx` and `LibGUI` (all 117 `LibGUI/*.cpp`, plus generated
  GML) from pristine upstream source against the host toolchain, with **zero source edits**.
  Not the six-file `LibGUI` stub that Lagom ships today ([history](docs/development-history.md) §2.4).
* **G2** — Build the unmodified `Calculator` and `PDFViewer` applications and run them
  **directly on a Wayland compositor**: no WindowServer, no XWayland, no `$DISPLAY`.
* **G3** — Correct behaviour: widget painting, anti-aliased text, icons, alpha, DPI scaling,
  window lifecycle, menus/popups, pointer/keyboard/wheel input, focus, clipboard.
* **G4** — Interactive use under a real session compositor (KDE Plasma here), plus a headless,
  pixel-asserted regression run driven by scripted input.
* **G5** — Reusable integration: scripts that build the stack and run the app matrix, in the
  style of the sibling `xforms-wayland` project.

### 1.2 Non-goals

* **Reimplementing the WindowServer's global duties.** Workspaces, taskbar, applets,
  alt-tab/window-switcher, system-wide wallpaper drawing, global mouse tracking, screen
  capture, cursor warping over other applications, and window-manager keybindings are the
  **compositor's** job on Wayland. LibWM does not try to reproduce them; it stubs the
  corresponding protocol calls.
* **A dedicated compositor or an X11/Wayland bridge.** LibWM is a *client* of the running
  compositor. It is not based on `xlib-wayland` and links none of its code. We do reuse that
  project's **pure-Wayland headless compositor** strictly as a test harness ([history](docs/development-history.md) §5.2); it depends
  only on `libwayland-server`, never on X11.
* **Zero-copy GPU presentation and Vulkan.** A Mesa/EGL `GPU::Device` (**`EGLGPU`**) is built and
  is the default GL backend ([history](docs/development-history.md) §4.11), but it reads back into a `Gfx::Bitmap` and presents over
  `wl_shm`; zero-copy `zwp_linux_dmabuf_v1` hand-off and a Vulkan backend are out of scope. This
  is *not* about the compositor: a Wayland compositor can share buffers but cannot render GL for
  a client.
* **Bit-exact core-font metrics vs. a Serenity framebuffer.** Glyph rasterisation is compared
  structurally ([history](docs/development-history.md) §5.3), not pixel-for-pixel, in the same spirit as the XForms project.
* **Multi-application global window management.** Each application process is its own Wayland
  client; the compositor owns global policy ([history](docs/development-history.md) §4.1).

### 1.3 Guiding principles

These are load-bearing for the whole project and apply to every milestone.

* **P1 — The Wayland compositor is the source of truth.** LibWM exists to translate, not to
  invent. Whenever SerenityOS expects environmental state, LibWM must obtain it from the *real*
  compositor and share client state back, rather than hardcoding or guessing. Concretely:
  * **From the compositor:** output geometry and logical size (`wl_output` + `zxdg_output_v1`),
    fractional/integer scale, seat capabilities, pointer/keyboard state and keymap
    (`wl_seat`/`xkbcommon`), clipboard and drag-and-drop (`wl_data_device`), decoration mode
    (`zxdg_decoration_manager_v1`), tiling/maximized/fullscreen/activated state.
  * **Into the Serenity protocol:** `fast_greet` screen rects and display scale, mouse/key
    events, clipboard contents, theme updates.
  * **Back to the compositor:** window title, `app_id`, minimum size, resize increments,
    alpha/click-through, icon, progress, and `xdg_surface.set_window_geometry`.
  * **No fabricated environment values.** Placeholder constants such as a fixed 1280×800
    screen are acceptable only as a temporary scaffold and must be replaced by compositor
    data before a milestone is called done.
* **P2 — Use AK and SerenityOS conventions in our code.** LibWM is Serenity userland that
  happens to run on Linux, not a generic C++ project. Use `AK` types and algorithms
  (`ByteString`, `String`, `Vector`, `HashMap`, `Optional`, `NonnullOwnPtr`/`RefPtr`,
  `ErrorOr`/`TRY`), Serenity idioms (`dbgln`, `C_OBJECT`, `MUST` only where failure is truly
  fatal), and Serenity style (snake_case methods, west `const`, 4-space indent). Do **not**
  pull in the C++ standard library or third-party abstractions where AK already provides the
  facility; avoid `std::` containers/string/optional/function. The code should read as if it
  belonged in `Userland/`, and any generally-useful change (the `Core::PortalServer` seam,
  the relocatable resource root) should be written to be upstreamable.

---

## 2. Repository layout


```
DESIGN.md                     this document (overview)
docs/                         architecture, threading, protocol mapping, testing, environment, history, guide
README.md                     quick start, status, limitations
LICENSE                       BSD-2-Clause (matches SerenityOS)
.github/workflows/ci.yml      build + all LibWM suites on every push (Arch container)
cmake/LibWMHelpers.cmake      libwm_wayland_protocol(), libwm_gui_executable()
patches/<group>/NNNN-*.patch  host seams applied to the SerenityOS checkout (see patches/README.md)
src/                          LibWM (libwm.so); src/CMakeLists.txt is the Lagom entry point
  LibWM.{h,cpp}               portal registration, server thread, on-demand service spawning
  Environment.{h,cpp}         every environment variable LibWM reads, and the portal path table
  WindowServerConnection.{h,cpp}, WindowServerSettings.cpp   WindowServer protocol server
  WaylandClient.{h,cpp}       display, registry, core globals, window table, dispatch
    WaylandSurface.cpp        xdg_toplevel/layer windows, CSD, presentation, fractional scale
    WaylandPopup.cpp          menu popups, menubar subsurface
  WaylandOutputs.{h,cpp}      wl_output/zxdg_output: screen size, output and client scales
  WaylandSeat.{h,cpp}         seat, pointer, keyboard/XKB, text-input, cursors
  WaylandDataDevice.{h,cpp}   clipboard and drag-and-drop
  SerenityShell.{h,cpp}       optional serenity-window protocol: the desktop's host settings,
                              wallpaper, capture, compositor-driven chrome
  MenuController.{h,cpp}      server-rendered menubar and menus
  {Clipboard,Config,Launch,FileSystemAccess}ServerConnection.{h,cpp}   in-process portals
  protocol/                   wlr-layer-shell and serenity-window protocol XML
  test/                       LibWM test programs + CTest registration of the scripts below
services/                     upstream WebContent, RequestServer, SQLServer (spawned on demand)
apps/                         upstream Calculator, PDFViewer, Piano (driven by the integration tests)
libeglgpu/                    Mesa/EGL GPU::Device backend for LibGL apps
scripts/fetch-serenity.sh     pinned, blobless, sparse checkout of SerenityOS
scripts/build-libgui.sh       apply patches + build the framework (+ run tests unless --no-tests)
scripts/check.sh              build + `ctest -L libwm`; what CI runs
scripts/check-patches.sh      verify the patch series applies to the pinned revision
scripts/run-*.sh              the integration suites (each registered with CTest)
scripts/fetch-headless-compositor.sh, build-headless-compositor.sh   xlib-wayland test compositor
serenity/                     pinned SerenityOS checkout (gitignored)
third_party/xlib-wayland/     pinned checkout, used only for its headless compositor (gitignored)
build/                        host build outputs (gitignored)
tools/gen-default-stub.py     generates the per-endpoint *DefaultStub.h at build time (build/lagom/libwm/stubs)
tools/wl-clipboard-peer.c     native libwayland-client clipboard/DnD peer (tests)
```

Changes to files under `third_party/xlib-wayland/` (e.g. compositor improvements) are made in
that checkout and **pushed back to `github.com/jmalcolm137/xlib-wayland`**, not vendored here.

## 3. References


* SerenityOS — `Userland/Libraries/LibGUI/ConnectionToWindowServer.{h,cpp}`,
  `LibGUI/Window.cpp`, `LibGUI/Application.cpp`
* Protocol — `Userland/Services/WindowServer/{WindowServer,WindowClient}.ipc`,
  `LibIPC/Connection*.h`, `Meta/Lagom/Tools/CodeGenerators/IPCCompiler`
* Host portability — `Meta/Lagom/CMakeLists.txt`, `LibCore/{AnonymousBuffer,System,
  ResourceImplementation,SessionManagement}.cpp`, `LibCore/Socket.{h,cpp}`
* Resources/theme — `LibGfx/Font/FontDatabase.cpp`, `LibGfx/SystemTheme.cpp`,
  `Base/res/{fonts,themes,color-schemes,icons}`
* Applications — `Userland/Applications/{Calculator,PDFViewer}`
