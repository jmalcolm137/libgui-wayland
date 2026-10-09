# libgui-wayland

Run **unmodified SerenityOS `LibGUI`/`LibGfx` applications** (e.g. Calculator, PDFViewer)
natively on **Wayland** — no SerenityOS WindowServer, no X server, and no XWayland. LibWM
links none of `xlib-wayland`; we reuse only that project's pure-Wayland headless compositor as
a test harness.

The shim is called **LibWM**. It sits between `LibGUI`'s WindowServer IPC protocol and
`libwayland-client`: the application side is Serenity, the system side is Wayland.

* **Philosophy:** keep `LibGfx`, the full `LibGUI`, and the applications byte-for-byte
  unmodified; implement the WindowServer's server side on top of Wayland.
* **Process model:** in-process — each application is its own Wayland client.
* **Build:** SerenityOS's Lagom host build, extended to compile the **full** LibGUI (not the
  six-file Lagom stub).

## Principles

1. **The compositor is the source of truth.** Query the real Wayland compositor for outputs,
   scale, seat, clipboard and window state, and share client state back — never hardcode
   environment values.
2. **Use AK and SerenityOS conventions.** LibWM is Serenity userland on Linux: AK types,
   `ErrorOr`/`TRY`, `dbgln`, `C_OBJECT`, Serenity style — no `std::` or third-party
   abstractions where AK suffices.

## Status

- **M0** — architecture/design + audit: done (see [DESIGN.md](DESIGN.md)).
- **M1** — full `LibGfx` + **full** `LibGUI` host build: done. `liblagom-gui.so`
  (132 objects, 3063 GUI symbols) builds from unmodified SerenityOS source via small
  Lagom/LibCore patches.
- **M2** — LibWM vertical slice: done. An unmodified-style LibGUI window renders through the
  in-process WindowServer protocol and appears as a real Wayland toplevel. Screen geometry is
  taken from `wl_output` (reported 1280×800 logical @ scale 2 here).
- **M3** — input + Calculator: done. Pointer/keyboard come from `wl_seat` (xkbcommon →
  Serenity `KeyCode`/modifiers), focus/activation/close handled, and the full input path is
  verified headlessly (the compositor injects a click; the widget receives it). The
  **unmodified Calculator** builds and runs, decorated and movable, on KDE Plasma.
- **M5 (partly)** — PDFViewer: done. The **unmodified SerenityOS PDFViewer** builds under
  Lagom and renders PDFs on the host. Opening a file goes through an in-process
  **FileSystemAccess** portal (including the real **File → Open** picker, shown on the main
  thread from the portal thread), and it reads/writes its preferences through an in-process
  **Config** portal; both are served by LibWM without touching library source. Verified
  headlessly against `Tests/LibPDF/*.pdf` (window, menubar, page toolbar, the rendered page,
  a typed-path picker open, and double-clicking to traverse a directory and open a file;
  `scripts/run-pdfviewer-test.sh`). Scroll/animation polish
  remains.
- **M6 (partly)** — clipboard and menus: done for `text/plain`, `text/uri-list`, and
  `image/png` ↔ `image/x-serenityos` (all verified headlessly in both directions), plus a
  server-rendered **menubar and dropdown menus** with hover highlighting (clicking `File → Quit`
  works; `Help → About` opens the dialog) and **nested submenus** with full keyboard navigation
  (`F10`/`Alt`+accelerator, arrows, `Enter`, `Escape`). Window state
  (`fullscreen`/`maximize`/`minimize`) and SerenityOS `/res/...` path redirection are in. Long-menu
  scrolling, menu item icons and config persistence remain.
- **Audio** — a second shim, **`LibSerenityAudio`**: `LibAudio` ↔ PipeWire, the same shape as
  LibWM being LibGUI ↔ Wayland. It serves the AudioServer protocol in-process (the shared ring
  buffer plus an `AudioServer`-style mixer) and feeds a PipeWire playback stream. The
  **unmodified SerenityOS Piano** builds and runs, with multi-client mixing, and is verified
  headlessly (window, AudioServer portal, PipeWire `streaming`, zero underruns) by
  `scripts/run-piano-test.sh`.
- **GPU / OpenGL** — the client-side GL stack (`LibGL` → `LibGPU`/`LibGLSL`) builds on the host
  unchanged, with two `GPU::Device` backends: **`EGLGPU`**, which drives a **Mesa** OpenGL
  compatibility context through EGL (the default), and **`LibSoftGPU`**, the CPU rasterizer.
  `EGLGPU` falls back to `LibSoftGPU` when no EGL context can be created, so GL always works.
  GL renders into an **offscreen `Gfx::Bitmap`**; LibGUI paints that bitmap and LibWM presents it
  as a `wl_shm` buffer, so the Wayland transport is untouched (the WindowServer protocol carries
  no GL messages). The **unmodified `3DFileViewer`** loads and animates an OBJ model, verified
  headlessly by `scripts/run-3dfileviewer-test.sh`; the **`Tubes`** demo builds against the same
  path.
- **Applications** — 44 of the 49 apps in `Userland/Applications` build and run on the host,
  including **Spreadsheet**, **TextEditor**, **PixelPaint**, **FileManager**, **Browser**,
  **Mail**, **Maps**, **Piano**, **Calculator**, **PDFViewer**, **3DFileViewer**, and all the
  settings/utility apps, with GML, icons, menus, dialogs and syntax highlighting intact. The
  web stack (`LibWeb`/`LibWebView`) is built on the host too (`-DENABLE_LAGOM_LIBWEB=ON`). The
  five that remain are Serenity service/kernel-facing and excluded (see below).

## Remaining work

- **Crisp fractional HiDPI (M8).** LibWM already binds `wp_viewporter` and takes the real
  logical size from `zxdg_output_v1`. Rendering the client's backing store at the output's
  device scale (and declaring it via `wp_viewport`/buffer scale) would remove the softness on
  the fractional (1.75×) panel; today the logical buffer is presented 1:1 and the compositor
  upscales it.
- **Menus (M6 remainder).** Scrolling for long menus, and drawing menu-item icons (the model
  already carries them; only the renderer ignores them).
- **Config persistence.** The in-process Config portal is in memory only; values are not
  written to disk between runs.
- **Audio polish.** `LibSerenityAudio` has no AudioManager portal (no system mixer/volume
  integration), and cross-rate clients use LibAudio's naive resampler.
- **GPU presentation and shaders.** `EGLGPU` renders on the GPU (Mesa) but reads the result
  back with `glReadPixels` into a `Gfx::Bitmap` and presents it over `wl_shm`; zero-copy
  `zwp_linux_dmabuf_v1` hand-off is not implemented, and the compositor still cannot render GL on
  a client's behalf. `EGLGPU` also does not support GLSL shaders (`glCreateShader`); programs
  that need them must use `LibSoftGPU`. See [DESIGN.md](DESIGN.md) §4.11.
- **Serenity-only applications and settings (host exclusions).** A handful of apps and
  settings target SerenityOS *system services* rather than a portable protocol; they are
  deferred until a **Serenity Desktop Environment on Linux** exists — see
  [Deferred: a Serenity Desktop Environment](#deferred-a-serenity-desktop-environment-on-linux).
  Every other application in `Userland/Applications` builds and runs.
- **Theme/font parity, decorations, icons, alpha (M4)** — remaining polish.

## Deferred: a Serenity Desktop Environment on Linux

Some applications and settings target SerenityOS *system services* instead of a portable
protocol. Rather than shim each one piecemeal, they are deferred until there is a **Serenity
Desktop Environment on Linux**: a Serenity-shaped session providing the corresponding services
and per-compositor backends.

Deferred applications (in `Userland/Applications`):

| App | Needs | Why deferred |
|---|---|---|
| `Terminal` | a Serenity pty and `LibVT`'s Serenity terminal backend | Linux has ptys (`posix_openpt`), but `LibVT`'s plumbing is Serenity-specific |
| `SystemMonitor` | Serenity `/proc` and `LibDebug`/`LibSymbolication` (`sys/arch/regs.h`, ptrace) | Linux `/proc` and the process model differ |
| `Debugger` | Serenity's ptrace ABI, `sys/arch/regs.h`, and core/ELF model | a hostile, non-portable mismatch |
| `CrashReporter` | Serenity's core-dump format, `LibCoredump`, `LibSymbolication` | same |
| `MouseSettings` | WindowServer *service* internals plus compositor-specific pointer settings | see below |

Compositor-specific settings (no Wayland standard exists):

* **Pointer acceleration, scroll step, double-click speed, cursor highlight** — libinput /
  compositor settings, configured per desktop (KDE KConfig/KWin, GNOME GSettings/Mutter).
* **Display configuration** (resolution/refresh/scale/rotation) — no adopted Wayland protocol;
  each desktop has its own (`wlr-output-management-unstable-v1`, `org.gnome.Mutter.DisplayConfig`,
  `org.kde.KScreen`).
* **Cursor theme/size** — the *transport* is standard (`org.freedesktop.portal.Settings`), but
  the keys are desktop-specific.

The **portable path** (not deferred) is a portal service layer: a provider behind
`PortalServer` that proxies `org.freedesktop.portal.Settings` for appearance/theme settings
(color scheme, accent, cursor theme/size with a small key map), plus the other portals a GUI
framework wants (`Screenshot`, `Notification`, `FileChooser`, `OpenURI`, ...). The
compositor-specific capabilities above would sit behind a thin **per-compositor backend
interface** (one implementation per desktop, KDE first), so the applications stay portable while
still working where a backend exists.

```sh
scripts/fetch-serenity.sh              # pinned, blobless, sparse checkout
scripts/fetch-headless-compositor.sh   # pinned clone of xlib-wayland (pure-Wayland compositor)
scripts/build-libgui.sh                # patches + LibGfx/LibGUI/LibWM/Calculator + all tests
```

`build-libgui.sh` runs the upstream LibGfx test suite and the headless integration tests
(input, clipboard, menus, PDFViewer, 3DFileViewer GL, Piano) automatically; pass `--no-tests` to
skip.

