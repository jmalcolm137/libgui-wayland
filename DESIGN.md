# LibWM — Running SerenityOS LibGUI/LibGfx Applications on Wayland

**Status:** draft / living document
**Target:** build **unmodified** SerenityOS `LibGfx` and `LibGUI` applications (e.g.
`Calculator`, `PDFViewer`) on Linux and run them **natively on a Wayland compositor** —
with no SerenityOS WindowServer, no X server, and no `xlib-wayland`.

**Pinned upstream revision:** `SerenityOS/serenity@80baeb2`
(`Meta: Enable the "vc4-kms-v3d.dtbo" Raspberry Pi devicetree overlay`).
The tree is tracked at `master`; the pin exists so the audit below is reproducible.

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
  Not the six-file `LibGUI` stub that Lagom ships today (§2.4).
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
  project's **pure-Wayland headless compositor** strictly as a test harness (§5.2); it depends
  only on `libwayland-server`, never on X11.
* **GPU/OpenGL.** `LibGL`, `LibSoftGPU`, `LibGPU` are out of scope. LibGfx's CPU painters are
  the only rendering path.
* **Bit-exact core-font metrics vs. a Serenity framebuffer.** Glyph rasterisation is compared
  structurally (§5.3), not pixel-for-pixel, in the same spirit as the XForms project.
* **Multi-application global window management.** Each application process is its own Wayland
  client; the compositor owns global policy (§4.1).

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

## 2. Evidence base

All numbers below were measured from the pinned tree, not assumed.

### 2.1 How big is the problem, really?

| Measurement | Value |
|---|---|
| `LibGUI/*.cpp` files | **117** |
| `LibGUI` lines of code | **44,478** |
| `LibGfx/*.cpp` files | 32 (+ `ImageFormats/`, `Font/`, …) |
| LibGUI translation units referencing Serenity-only headers/APIs | **3** (`Window.cpp`, `Event.h`, `Shortcut.h`) |
| `AK_OS_SERENITY` blocks in all of LibGUI | **2**, both in `Window.cpp` (`madvise` volatile backing store) |
| Distinct `WindowServerEndpoint` methods LibGUI calls | **40** (67 call sites) |
| `WindowClientEndpoint` events LibGUI consumes | ~30 |
| Reference `WindowServer` implementation | ~16,300 lines (25 files) |
| Side portals an app may open | **8** (§2.5) |

The important fact: **LibGUI is pure, portable C++ except for the WindowServer transport and
two `madvise` calls.** There are no X11 calls, no `/dev` framebuffer access, no kernel
syscalls in the widget code. This is a *port and integration* task, not a rewrite.

### 2.2 The transport is a single, cleanly-typed seam

`LibGUI/ConnectionToWindowServer.h` is the entire client-side platform boundary:

```cpp
class ConnectionToWindowServer final
    : public IPC::ConnectionToServer<WindowClientEndpoint, WindowServerEndpoint>
    , public WindowClientEndpoint
{
    IPC_CLIENT_CONNECTION(ConnectionToWindowServer, "/tmp/portal/window"sv)
    ...
    virtual void fast_greet(...) override;
    virtual void paint(...) override;
    virtual void mouse_down(...) override;
    ...   // ~30 server→client events
};
```

`IPC_CLIENT_CONNECTION` expands to exactly one platform action:

```cpp
auto socket = TRY(Core::LocalSocket::connect("/tmp/portal/window"));
```

Everything else in LibGUI goes through the generated, strongly-typed endpoint proxies
(`async_create_window(...)`, etc.). So *one* socket acquisition is the transport seam.

**Startup ordering matters.** `GUI::Application::create()` calls
`ConnectionToWindowServer::the()`, whose constructor **blocks** until the server sends
`fast_greet`:

```cpp
auto message = wait_for_specific_message<Messages::WindowClient::FastGreet>();
set_system_theme_from_anonymous_buffer(message->theme_buffer());
Desktop::the().did_receive_screen_rects({}, message->screen_rects(), ...);
Gfx::FontDatabase::set_default_font_query(message->default_font_query());
...
```

So LibWM must be listening and must send `fast_greet` **before** the application's `main` gets
to `Application::create()`, or the app deadlocks. `Core::AnonymousBuffer` on Linux is a memfd
(`LibCore/System.cpp` uses `memfd_create`), so the theme buffer and the client backing-store
fds cross the socket via `SCM_RIGHTS` with no extra work.

### 2.3 The client backing store is already shared memory

`LibGUI/Window.cpp`:

```cpp
ErrorOr<NonnullOwnPtr<WindowBackingStore>> Window::create_backing_store(Gfx::IntSize size)
{
    auto format = m_has_alpha_channel ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    size_t pitch = Gfx::Bitmap::minimum_pitch(size.width(), format);
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(...));      // memfd
    auto bitmap = TRY(Gfx::Bitmap::create_with_anonymous_buffer(format, buffer, size, 1));
    return make<WindowBackingStore>(bitmap);
}
```

and `set_current_backing_store()` sends the fd with `set_window_backing_store(...)`.
`BGRA8888`/`BGRx8888` with 4-byte pitch is exactly `wl_shm`'s `ARGB8888`/`XRGB8888`. **LibWM
can create a `wl_shm_pool` directly from the received memfd and attach a `wl_buffer` over it —
no copy.** (Fallback: copy into LibWM-owned buffers if the compositor rejects the fd.)

### 2.4 Lagom already host-builds most of the stack

SerenityOS's own **Lagom** target builds a subset of userland natively on Linux. It already
builds:

```
AK  LibCore  LibGfx  LibIPC  LibUnicode  LibCompress  LibPDF  LibSyntax
LibURL  LibThreading  LibImageDecoderClient  LibRegex  LibTextCodec  LibRIFF  …
```

…which covers `LibGfx`'s entire dependency list and almost all of `LibGUI`'s. But Lagom's
`LibGUI` is deliberately a **stub**:

```cmake
# Meta/Lagom/CMakeLists.txt
set(LIBGUI_SOURCES
    GML/Lexer.cpp  GML/Parser.cpp  GML/SyntaxHighlighter.cpp
    Icon.cpp  Model.cpp  ModelIndex.cpp)
```

**Six files.** The user requirement is the full toolkit, so the first build task is to teach
Lagom (or a local overlay, §4.1) to compile all 117 source files and the generated GML,
without touching their contents.

### 2.5 Portals the host must provide

`IPC_CLIENT_CONNECTION` sites in the libraries above:

| Portal path | Client library | Needed by |
|---|---|---|
| `/tmp/portal/window` | `LibGUI/ConnectionToWindowServer` | every GUI app |
| `/tmp/session/%sid/portal/clipboard` | `LibGUI/Clipboard` | `Application::create` (always) |
| `/tmp/portal/wm` | `LibGUI/ConnectionToWindowManagerServer` | Taskbar / `make_window_manager` |
| `/tmp/session/%sid/portal/config` | `LibConfig::Client` | window geometry persistence, settings |
| `/tmp/session/%sid/portal/image` | `LibImageDecoderClient::Client` | file-manager thumbnails, image widgets |
| `/tmp/session/%sid/portal/filesystemaccess` | `LibFileSystemAccessClient::Client` | PDFViewer open/save |
| `/tmp/session/%sid/portal/launch` | `LibDesktop/Launcher.cpp` | launching helper apps |
| `/tmp/session/%sid/portal/notify` | `LibGUI/Notification.cpp` | desktop notifications |

All eight must resolve, because `Application::create()` unconditionally initialises the
clipboard and other libraries connect lazily. The good news: six of the eight are trivial to
serve in-process, and only `window` (the main event) and `clipboard` (Wayland data device)
involve real work.

### 2.6 Resources are relocatable by design

`Core::ResourceImplementation::install()` and the default `ResourceImplementationFile("/res")`
mean LibGfx's font/icon/theme loading can be pointed at a bundled Serenity `Base/res`:

* fonts: `FontDatabase::load_all_fonts_from_uri("resource://fonts")` — Katica, Csilla, Liza,
  Satori, … plus Liberation `.ttf`s are all present in `Base/res/fonts` (exact Serenity
  metrics, no fontconfig guesswork);
* theme: `Gfx::load_system_theme("Base/res/themes/Default.ini")` → `AnonymousBuffer`;
* icons/color-schemes: `Base/res/icons`, `Base/res/color-schemes`.

`Gfx::set_system_theme(buffer)` + `Gfx::current_system_theme_buffer()` is exactly what
WindowServer puts in `fast_greet`, so LibWM reuses LibGfx's own theme code for the greeting.

### 2.7 The transport can be made in-process

`Core::LocalSocket::adopt_fd(int fd)` exists, and `socketpair(AF_UNIX, SOCK_STREAM)` yields a
valid local socket. So LibWM can hand LibGUI one end of an in-process socketpair and run the
server end itself — **no WindowServer process** (§3.3).

---

## 3. Architecture

### 3.1 Layering

```
┌──────────────────────────────────────────────────────────────────────┐
│  unmodified apps:  Calculator · PDFViewer · (any LibGUI app)          │
├──────────────────────────────────────────────────────────────────────┤
│  LibGUI (full, unmodified, 117 TUs + generated GML)                    │
│    Window · Widget · Menu · Painter · Application · Clipboard …        │
├──────────────────────────────────────────────────────────────────────┤
│  LibWM  (this project)                                                 │
│    ├─ WM::WindowServer   server side of WindowServerEndpoint           │
│    ├─ WM::Surface        Serenity window ↔ wl_surface/xdg_toplevel     │
│    ├─ WM::Renderer       AnonymousBuffer→wl_buffer, damage, CSD        │
│    ├─ WM::MenuRenderer   server-rendered menus → xdg_popup             │
│    ├─ WM::Input          wl_pointer/wl_keyboard → Mouse/Key events     │
│    ├─ WM::ThemeFonts     bundled Base/res, fast_greet                  │
│    └─ WM::Portals        clipboard · config · image · fsaccess · …     │
├──────────────────────────────────────────────────────────────────────┤
│  LibGfx (unmodified)          │  libwayland-client  · wl_shm          │
├───────────────────────────────┴───────────────────────────────────────┤
│  Wayland compositor  (KDE Plasma / sway / weston / test compositor)   │
└──────────────────────────────────────────────────────────────────────┘
```

`LibWM` is responsible for *all* platform behaviour. Nothing above the line knows Wayland
exists; nothing below the line knows Serenity exists.

### 3.2 Process model — recommended: **in-process LibWM**

On Wayland the compositor *is* the window server, so the correct place for LibWM is **inside
the application process**, as a library (`libwm.so`) linked by the app. Each application opens
its own `wl_display` connection and owns its own `xdg_toplevel`s. This:

* literally "eliminates the need for a dedicated Window Server";
* makes each app a first-class Wayland client (correct focus, stacking, decorations, input
  routing, sandboxing);
* avoids a compositor-in-the-middle copying every frame;
* keeps multi-app coordination where it belongs — in the compositor.

**Alternative (documented, not recommended as default): a LibWM daemon.** A single process
that binds `/tmp/portal/window` and the other portal sockets, exactly as Serenity's
WindowServer does. This needs **zero** LibCore changes and would run truly unmodified binaries,
but it re-introduces a dedicated "window server" and a buffer-forwarding hop, contrary to the
project goal. The same `WM::WindowServer` code supports both; only the connection ownership
differs. This is retained as a fallback for legacy/unmodified binary distribution.

### 3.3 The transport seam

The only change needed to keep LibGUI and the apps unmodified is a way for
`Core::LocalSocket::connect("/tmp/portal/window")` to return a socketpair whose peer is LibWM.
We introduce a small, general **portal registry** in LibCore:

```cpp
namespace Core {
class PortalServer {
public:
    using Factory = Function<ErrorOr<NonnullOwnPtr<LocalSocket>>()>;
    static void register_portal(ByteString path, Factory);   // e.g. "/tmp/portal/window"
};
}
```

`LocalSocket::connect` consults the registry; if a portal matches the (session-expanded) path
it returns the in-process peer, otherwise it falls back to the real Unix socket. LibWM
registers all eight portal paths from a static initialiser before `main`. This is a ~30-line,
upstreamable addition to the platform layer — the equivalent of "the shim lives under the
unmodified library".

**No daemon, no `LD_PRELOAD`, no LibGUI edit.** (If even this seam is unwanted, the fallback is
`LD_PRELOAD` interposition of `Core::LocalSocket::connect`, or the daemon model; both are
strictly worse, so the registry is preferred.)

The server end of each portal runs on **a dedicated thread** owned by LibWM. This is required
because the client constructor blocks on `fast_greet` outside the event loop; a thread also
keeps IPC encoding/decoding off the UI thread. Wayland dispatch runs on the main thread,
integrated with LibGUI's `Core::EventLoop` via the `wl_display` fd (a `Core::Notifier`).

### 3.4 WindowServer-protocol server (`WM::WindowServer`)

Mirrors `WindowServer::ConnectionFromClient` + a trimmed `WindowManager`, but backed by
Wayland instead of a framebuffer. It implements the generated `WindowServerEndpoint::Stub`
and drives the generated `WindowClientEndpoint::Proxy`.

Responsibilities:

* window lifecycle: `create_window` → `wl_surface` + `xdg_toplevel` (+ `xdg_decoration`),
  title, app_id, minimum/size increments, fullscreen/maximized/minimized/frameless;
* backing store: `set_window_backing_store(window_id, bpp, pitch, anon_file, serial, α, size,
  visible_size, flush_immediately)` → wrap the memfd in a `wl_shm_pool` and attach a
  `wl_buffer`; honour the front/back serial protocol;
* paint scheduling: `invalidate_rect` and compositor frame callbacks coalesce into
  `paint(window_id, size, rects)` (the message LibGUI expects to trigger a redraw);
  `did_finish_painting` closes the frame;
* menus: the client sends a *menu model* (`create_menu`, `add_menu_item`, …) and calls
  `popup_menu`; LibWM renders it into an `xdg_popup` and synthesises
  `menu_item_entered`/`menu_item_left`/`menu_item_activated`/`menu_visibility_did_change`;
* input routing: Wayland events → `mouse_move/down/up/double_click/wheel`,
  `key_down/up`, `window_entered/left`, `window_activated/deactivated`,
  `window_close_request`, `window_resized/moved`;
* desktop/global calls (`set_wallpaper*`, `get_screen_bitmap*`,
  `get_global_cursor_position`, `set_global_mouse_tracking`, `start_drag`, …):
  approximated where sensible, otherwise acknowledged as no-ops so apps never hang.

### 3.5 Rendering

* **Surface buffer.** On `set_window_backing_store`, LibWM takes the received memfd and, if
  the size/stride is compatible, creates a `wl_shm_pool` over it and a `wl_buffer` referencing
  the client's pixels. Otherwise it allocates a LibWM pool and `memcpy`s (fallback). Because
  the client and LibWM share the mapping, `wl_surface.damage_buffer` uses the rects from
  `invalidate_rect`.
* **Client-side decorations.** Serenity's WindowServer draws the title bar/shadow; the client
  bitmap is the *content* area. LibWM requests compositor-side decorations via
  `zxdg_decoration_manager_v1`; when unavailable it draws CSD (title bar, border, drop shadow)
  using LibGfx's `ClassicWindowTheme`/theme palette into a larger buffer and sets
  `xdg_surface.set_window_geometry` to the content rect so input coordinates stay correct.
* **HiDPI (documented limitation, and a decision to make).** LibGUI draws into a **logical-size**
  backing store today — `Window::create_backing_store()` carries an explicit
  `// FIXME: Plumb scale factor here eventually` and passes scale `1` to
  `Gfx::Bitmap::create_with_anonymous_buffer`. Unlike an X11 raster shim, LibWM does not own the
  client's drawing, so it cannot simply rasterise at a device scale. The initial behaviour is to
  present the client's logical buffer 1:1 (`wl_surface.set_buffer_scale(1)`: correctly sized,
  slightly soft on a HiDPI panel). Crisp HiDPI requires plumbing an output scale into LibGUI's
  backing store (an upstream LibGUI change, tracked separately), or LibWM advertising a physical
  size and the client allocating a scaled `Gfx::Bitmap`. Deferred past M5; recorded here so it is
  not mistaken for a rendering bug.
* **Alpha.** `set_window_has_alpha_channel` / `set_window_alpha_hit_threshold` select
  `ARGB8888` and, where the compositor supports it, input-region masking for click-through.

### 3.6 Input

* `wl_pointer` motion/enter/leave/button/axis → Serenity `MouseEvent`s. Coordinates are
  surface-local; CSD offsets are subtracted so widgets see content-local points. Button
  numbering, wheel deltas (`axis`) and modifiers are mapped.
* Double-clicks are synthesised like WindowServer's `WindowManager`: a second press/release
  within 250 ms and 4 px of the previous one on the same window is delivered as
  `mouse_double_click` (after the `mouse_up`). GUI views use it for activation, e.g.
  double-clicking a file or directory in the file picker.
* `wl_keyboard` + `xkbcommon`: `keymap` provides an XKB keymap; `enter`/`key`/`modifiers` map
  to `key_down`/`key_up` with `code_point`, Serenity `KeyCode`, modifier mask, and scancode
  via a translation table built from `Kernel/API/KeyCode.h` (which Lagom already installs).
* Focus/activation come from `xdg_toplevel.configure` states and pointer enter/leave.
* `xdg_toplevel.close` → `window_close_request`.

### 3.7 Menus and popups

Serenity menus are **server-rendered**: the client owns only the menu *model*, and the
WindowServer owns placement, hover, selection and dismissal. This is a real component to port
(`WM::MenuRenderer`), but it is self-contained and reuses LibGfx: build a `Gfx::Bitmap`, draw
items/separators/shortcuts/checkmarks with `Gfx::Painter` using the system theme and the
`fast_greet` fonts, present it as an `xdg_popup` positioned by `xdg_positioner`, and translate
pointer events into menu callbacks. Compositor popup grabs provide the modal behaviour that
X11/Serenity got from server pointer grabs.

### 3.8 Auxiliary portals

| Portal | LibWM implementation |
|---|---|
| `window` | `WM::WindowServer` (the core of this project) |
| `clipboard` | native Wayland `wl_data_device`/`wl_data_source`/`wl_data_offer`; Serenity text mime ↔ Wayland text mimes (§4.5) |
| `config` | file-backed ConfigServer (`~/.config/…` INI), serving `ConfigServerEndpoint` |
| `image` | in-process `LibGfx` image decoders, serving `ImageDecoderServerEndpoint` |
| `filesystemaccess` | `xdg-desktop-portal` (D-Bus) where present, else a LibGUI `FilePicker` fallback; command-line file arguments work without it |
| `wm` | thin shim so taskbar-style apps that call `make_window_manager` do not fail; otherwise unused |
| `launch` / `notify` | best-effort: `xdg-open`/D-Bus notifications, or stubs |

Only `window` and `clipboard` are on the critical path for `Calculator`/`PDFViewer` startup;
the rest are needed lazily and can be delivered milestone-by-milestone.

---

## 4. Build and integration

### 4.1 Host build

Use SerenityOS's **Lagom** as the host build system (it already proves `LibGfx`, `LibCore`,
`LibIPC`, `LibPDF`, `LibUnicode` on Linux), extended to build the **full LibGUI**:

1. Add a `LibGUI` host target compiling **all 117 `SOURCES`** from
   `Userland/Libraries/LibGUI/CMakeLists.txt` (not the six-file stub), including the generated
   `*GML.cpp` and the IPC-generated `WindowServerEndpoint.h` / `WindowClientEndpoint.h` /
   `WindowManager*Endpoint.h`.
2. Add the host libraries LibGUI/most apps need that Lagom does not yet build:
   `LibConfig`, `LibDesktop`, `LibFileSystemAccessClient`, and `LibKeyboard` if required.
   (`LibImageDecoderClient`, `LibPDF`, `LibSyntax`, `LibELF` are already built.)
3. `compile_gml` and `compile_ipc` already work under Lagom (`Lagom::GMLCompiler`,
   `Lagom::IPCCompiler`), so unmodified app `.gml` and `.ipc` files are consumed as-is.
4. Bundle `Base/res` (fonts, themes, icons, color-schemes) and have LibWM call
   `Core::ResourceImplementation::install(ResourceImplementationFile(res_root))`.

**M1 result (implemented).** The full LibGUI now builds on the host:
`build/lagom/lib/liblagom-gui.so` — **132 objects, 17.8 MB, 3063 GUI symbols** — together with
`LibGfx`, `LibCore`, `LibIPC`, `LibConfig`, `LibSyntax`, `LibUnicode`. Two host-build facts
were required, both handled in the build system rather than in library source:

* **SERENITYOS-gated services do not run in a Lagom build.** `Userland/Services/CMakeLists.txt`
  wraps WindowServer, Clipboard, LaunchServer, NotificationServer, FileSystemAccessServer, … in
  `if (SERENITYOS)`. Their IPC endpoints must be generated explicitly (as Lagom already does for
  RequestServer), and the libraries that consume them (`LibConfig`, `LibGUI`, `LibDesktop`) must
  be added *after* those endpoints exist, because `add_dependencies` validates target existence.
* **Shared kernel headers are compile-time only.** `LibGUI/Event.h` and `Shortcut.h` need
  `Kernel/API/KeyCode.h`; `LibGUI/FileIconProvider.cpp` → `LibELF/Image.h` needs
  `Kernel/Memory/VirtualAddress.h`. These are portable ABI constants and header-only AK
  templates (no kernel calls); `serenity_limits.h` already has a non-`__serenity__` branch.
  `Kernel/API` and `Kernel/Memory` are therefore part of the host checkout. Genuinely
  Serenity-specific kernel headers (syscall numbers, VM internals) are **not** touched — the
  shim boundary is the WindowServer protocol, not the kernel ABI.

The change is a single build-system patch, `patches/0001-lagom-build-full-libgui.patch`
(+30/−12 lines in `Meta/Lagom/CMakeLists.txt`), applied idempotently by
`scripts/build-libgui.sh`. No `LibGUI`/`LibGfx` source is edited. Preferred long-term:
contribute the full `LibGUI` target to Lagom upstream (it already builds the GML subset).

### 4.2 M2 result — the shim is live

The first vertical slice is implemented and verified on a real KDE Plasma Wayland session:

* **Transport seam.** `patches/0002-libcore-host-seams.patch` adds the `Core::PortalServer`
  connector to LibCore (consulted by `LocalSocket::connect`) and makes the default
  `resource://` root configurable via `SERENITY_RES_ROOT`, so namespace-scope users such as
  `LibGUI/Calendar.cpp` can load fonts from their static initializers. Both are small and
  written to be upstreamable; LibGUI and applications are untouched.
* **Protocol server.** `LibWM::WindowServerConnection` implements the WindowServer endpoint.
  A generated `WindowServerDefaultStub` (from `tools/gen-default-stub.py`) satisfies all 100
  pure-virtual methods, so LibWM overrides only the subset LibGUI uses.
* **Dedicated server thread.** Because `ConnectionToWindowServer`'s constructor and pre-loop
  synchronous calls (e.g. `get_window_rect`) block, the server runs on its own thread with its
  own `Core::EventLoop` — as §3.3 specified.
* **Real compositor data.** On connect, LibWM binds `wl_compositor`, `wl_shm`, `wl_seat`,
  `wl_output` and `xdg_wm_base`, and takes the logical screen size from `wl_output` mode and
  scale. On the test panel it correctly reports **1280×800 logical at output scale 2**
  (physical 2560×1600).
* **Presentation.** Each Serenity window maps to a `wl_surface` + `xdg_toplevel`; the client's
  shared memfd is exposed through `wl_shm` and attached as a `wl_buffer`. A minimal LibGUI
  window (a painted widget) renders and appears as a real Wayland window on Plasma.
* **Host paths.** `Core::System::openat` redirects SerenityOS's absolute `/res/...` paths to
  the bundled resource root (`SERENITY_RES_ROOT`), so GML assets and themes that hardcode
  `/res/...` (e.g. the About dialog's brand banner) load unmodified. `Core::Version` reports
  `Version 1.0 (Wayland)` on the host.
* **Window state.** `set_fullscreen`/`set_maximized`/`set_minimized` map to the matching
  `xdg_toplevel` requests; the compositor's resulting `configure` size resizes the client
  content (accounting for the menubar inset), verified by `libwm-fullscreen-test`.
* **HiDPI / fractional scaling.** The test panel is **fractionally scaled (1.75×)**: physical
  2560×1600 with a logical output of **1463×914**, which `wl_output.mode/scale` alone reports
  wrongly as 1280×800. LibWM takes the real logical size from `zxdg_output_v1`. It also honours
  the client's `visible_size` (LibGUI over-allocates its backing store by 64 px per axis during
  interactive resize), so the correct region is presented. Crisp rendering at a fractional
  scale still requires plumbing a device scale into LibGUI's backing store (M8); today the
  logical buffer is presented 1:1 and the compositor upscales it.

Still deferred (next milestones): pointer/keyboard input (M3), CSD/decoration policy and DPI
polish (M4), menus/popups and clipboard (M6).

### 4.3 M3 result — input

Input is taken from the real compositor (principle P1) and translated into Serenity events:

* **Seat.** LibWM binds `wl_seat` and reacts to `capabilities`, creating `wl_pointer` and
  `wl_keyboard`. Verified live: `seat capabilities 0x3 (pointer=true, keyboard=true)`.
* **Keyboard.** The compositor's `wl_keyboard.keymap` is mapped and compiled with `xkbcommon`
  (verified: a 35 KB keymap, state created). Key events map evdev codes → Serenity `KeyCode`
  (a table built from `Kernel/API/KeyCode.h`), `xkb_state_key_get_utf32` → `code_point`, and
  xkb modifier state → `Mod_Shift/Ctrl/Alt/Super/AltGr`. These become `key_down`/`key_up`.
* **Pointer.** `enter`/`leave`/`motion`/`button`/`axis` become `mouse_move`/`mouse_down`/
  `mouse_up`/`mouse_wheel` and `window_entered`/`window_left`; Linux `BTN_*` codes map to
  `GUI::MouseButton` (`Primary`/`Secondary`/`Middle`/`Backward`/`Forward`).
* **Focus & lifecycle.** `xdg_toplevel` configure `ACTIVATED` → `window_activated`/
  `window_deactivated`; `xdg_toplevel.close` → `window_close_request`.

**Verification.** The full input path is now tested headlessly: the `xlib-wayland`
headless-compositor (§5.2) injects a synthetic `motion` + left `button` press/release, and the
test asserts that the event reaches the LibGUI widget (`TestWindow: mouse down at 160,120
button 1`), with the frame captured to PNG. This runs as part of the build
(`scripts/run-headless-tests.sh`).

### 4.4 Unmodified Calculator

The upstream `Userland/Applications/Calculator` sources build and run **unmodified** on
Wayland (`main.cpp`, `Calculator.cpp`, `CalculatorWidget.cpp`, `Keypad.cpp`, and a precompiled
`CalculatorWindow.gml`), rendering correctly — decorated and movable — on the live Plasma
session. Startup required two more in-process portals besides the WindowServer:

* **LaunchServer** — `Desktop::Launcher` registers and seals its allowlist synchronously
  during `serenity_main`.
* **Clipboard** — `LibGUI::TextBox` (via `TextEditor`) reads the clipboard while initialising.

### 4.5 Native clipboard

The clipboard bridges the SerenityOS clipboard protocol to Wayland's data device:

* **Reads.** LibWM watches `wl_data_device.data_offer`/`selection`, records the offered mime
  types, and receives the best one into a pipe. Preference is text
  (`text/plain;charset=utf-8` → `text/plain` → `UTF8_STRING`) then `text/uri-list` then
  `image/png`; text is normalized to `text/plain`, `text/uri-list` passes through, and
  `image/png` is transcoded with LibGfx into Serenity's raw `image/x-serenityos` (pixels +
  `width/height/scale/format/pitch` metadata) so `Clipboard::as_bitmap()` works.
* **Writes.** `set_clipboard_data` builds the set of representations to offer: `text/plain`
  (plus `text/plain;charset=utf-8`/`UTF8_STRING`/`STRING` aliases), `text/uri-list` and other
  types pass through unchanged, and `image/x-serenityos` is encoded to `image/png` (and also
  offered raw, so LibWM-to-LibWM image copies stay lossless).
* **Same thread as input, on purpose.** `set_selection` requires a serial from a *recent input
  event on the same connection*, so the data device lives on the connection that owns the
  seat. LibWM therefore serves all portals from a single server thread with one
  `Core::EventLoop` and one `WaylandClient`; the last input serial is tracked from
  pointer/keyboard events. (Interactive Copy — Ctrl+C or a menu action — supplies a serial;
  a selection set with no prior input is ignored by real compositors, which is correct Wayland
  behaviour.)

Both directions are verified headlessly for **text**, **`image/png` ↔ `image/x-serenityos`**,
and **`text/uri-list`** against a native `libwayland-client` peer
(`tools/wl-clipboard-peer.c`): the peer owns the selection, our app reads it (logging what
LibGUI sees), then the peer reads what our app published. This runs as part of the build
(`scripts/run-clipboard-test.sh`).

### 4.6 Menus

In SerenityOS the **menubar and dropdown menus are drawn by the WindowServer**, not the
client: LibGUI sends only the menu model (and, for context menus, a `popup_menu` request).
LibWM therefore renders them itself with LibGfx:

* **Model.** `MenuController` keeps the menus and items (`create_menu`, `add_menu_item`,
  separators, checkable/checked/default, shortcuts, submenu ids).
* **Menubar.** Windows with menus get a 20 px top **inset**. The client's content is composited
  below it into one buffer, and the menubar (`File Edit …`) is drawn into the strip. Pointer
  events over the strip are handled by LibWM; content coordinates are shifted down by the
  inset before being sent to the client.
* **Dropdowns.** A menubar click opens the menu in an `xdg_popup` (`xdg_positioner` anchored at
  the menubar item), with an input grab. Hover highlights items and sends
  `menu_item_entered`/`menu_item_left`; clicking sends `menu_item_activated`; dismissal sends
  `menu_visibility_did_change`. Context menus (`popup_menu`) use the active window as the parent.
* **Submenus.** An item with a `submenu_id` shows a right-pointing arrow; hovering it (mouse) or
  pressing `Right`/`Enter` on it (keyboard) opens the child menu as another `xdg_popup` parented to
  the parent popup's `xdg_surface`, anchored at the item's right edge. `MenuController` keeps a
  *stack* of open menus (`m_open_menus`, root → deepest) so ancestor menus stay open and only the
  deepest one receives navigation; `Left` closes the deepest and returns to its parent. Only the
  root popup grabs input (nested popups inherit it).
* **Keyboard.** `F10` opens the first menubar menu; `Alt`+accelerator opens/switches to a menu;
  `Up`/`Down` move within the deepest menu; `Left`/`Right` close/open submenus (or move between
  menubar menus at the root); `Enter` activates a leaf or opens a submenu; `Escape` closes all.

Verified headlessly: clicking `File` opens a popup, clicking `Quit` in it activates the item (the
app exits), and `F10 → Down → Right → Enter` navigates into the nested `New` submenu and activates
its item — `scripts/run-menu-test.sh`. Known gaps: scrolling long menus is not implemented yet,
and menu item icons are not drawn.

### 4.7 PDFViewer

The unmodified **PDFViewer** (LibPDF + LibGUI) builds under Lagom and renders PDFs on the
host. `main.cpp` opens a path from the command line through
`FileSystemAccessClient::Client::request_file_read_only_approved`, then
`PDFViewerWidget::open_file` rasterises pages with LibPDF into an
`AbstractScrollableWidget`. Verified headlessly against `Tests/LibPDF/complex.pdf`
(3 pages): the window, the `File View Debug Help` menubar, the page toolbar and the
rendered "Page One" all appear — `scripts/run-pdfviewer-test.sh`, which also dumps the
rendered window to `build/pdfviewer-window.png`.

### 4.8 Config and FileSystemAccess portals

PDFViewer touches two more SERENITYOS services that Lagom does not build. Both are served
in-process by LibWM, so the app stays unmodified:

* **ConfigServer** — `Config::pledge_domain` and `read_*`/`write_*` back window geometry,
  recent files and the render-preference toggles. `ConfigServerConnection` keeps an
  in-memory `domain → group → key` store (persistence to disk is still pending).
* **FileSystemAccessServer** — `request_file_read_only_approved` asks for access to a
  named file and receives its fd over IPC. `FileSystemAccessServerConnection` opens the
  path directly, and for `prompt_open_file`/`prompt_save_file` it shows the real
  `GUI::FilePicker`, so `File -> Open` works.

Both are enabled by generating their IPC endpoints in the Lagom build and building
`LibFileSystemAccessClient`; its `add_dependencies(... WindowServer)` line is satisfied by
a no-op placeholder target. No library source is modified.

### 4.8.1 Showing GUI from the server thread

The picker needs GUI, which belongs to the main thread, but the FS server runs on the LibWM
server thread — and the server thread must not block, because the picker it is waiting on
talks back to the WindowServer that same thread owns. `MainThreadInvoker` therefore hands
work across asynchronously in both directions (`EventLoop::deferred_invoke()` on the other
thread's loop is thread-safe and wakes it): the FS handler posts "show the picker" to the
main loop, and the picker's continuation posts "send the reply" back to the server loop.
This needs the client to be pumping its loop, which it is: `Core::Promise::await()` pumps
(whereas synchronous calls block on the socket). The sync `expose_window_server_client_id`
call that precedes the picker request rules out simply moving the whole FS server to the
main thread.

`GUI::FilePicker::get_filepath` — the API the real FileSystemAccessServer uses — is gated by
`Badge<FileSystemAccessServer::ConnectionFromClient>`, a private-constructor passkey that
only that class can construct, so LibWM uses the public `get_open_filepath`/`get_save_filepath`
instead (losing only automatic parent-centering).

Headless testing this surfaced a compositor bug: the headless compositor sent
`wl_keyboard.enter` only once, so after opening a dialog it kept routing keys to the old
surface. That is fixed in xlib-wayland (pin bumped in `scripts/fetch-headless-compositor.sh`).

### 4.9 Building the applications

Serenity app `CMakeLists.txt` use `serenity_app`, `serenity_component`, `compile_gml`,
`serenity_bin`, and `embed_resource`. Under Lagom these resolve through `Meta/CMake/`, so
**app CMake files and sources can be consumed verbatim**, with at most build-system
accommodations (e.g. `embed_resource`'s assembly step on non-Serenity hosts, or a no-op icon
embed). Any such accommodation is a *build flag or shim macro*, never an app source edit.

`Calculator` needs `LibCore LibCrypto LibDesktop LibGfx LibGUI LibMain LibURL`.
`PDFViewer` adds `LibPDF LibFileSystemAccessClient LibConfig`.

### 4.10 Prefixes and running

Two user-local prefixes, no root (mirroring the sibling project):

| Prefix | Contents |
|---|---|
| `$LIBWM_PREFIX` = `~/.local/libgui-wayland` | `libwm.so`, bundled `res/`, `include/` |
| `$SERENITY_PREFIX` = `~/.local/libgui-wayland/serenity` | host-built `LibGfx`/`LibGUI`/deps |

```sh
# on the live session (KDE Plasma)
LD_LIBRARY_PATH="$LIBWM_PREFIX/lib:$SERENITY_PREFIX/lib" \
  "$SERENITY_PREFIX/bin/Calculator"

# headless regression
scripts/run-apps.sh
```

No `DISPLAY` and no `xlib-wayland`. LibWM connects to `$WAYLAND_DISPLAY`.

---

## 5. Test strategy

### 5.1 First targets

| App | Exercises |
|---|---|
| `Calculator` | `Application`/`Window`, GML widget tree, buttons, keyboard input, theme, fonts, `LibConfig`, clipboard init |
| `PDFViewer` | everything above + `LibPDF` rasterisation, `ImageWidget`/`AbstractScrollableWidget`, scrollbars, toolbars, `FileSystemAccess`, file I/O, larger window |

### 5.2 Headless, pixel-asserted

Upstream ships **no `Tests/LibGUI`** — LibGUI is validated only through real applications.
The libraries beneath it do have host suites (LibGfx: ~20 tests; also AK, LibCore, LibPDF,
LibUnicode, …), and `scripts/build-libgui.sh` runs the LibGfx suite as part of every build
(`scripts/run-lib-tests.sh`, skippable with `--no-tests`). That validates the rendering stack
our host build stands on. GUI behaviour itself is covered by the app matrix below.

A headless Wayland compositor, taken from the `xlib-wayland` project
(`tools/headless-compositor.c`; pinned clone via `scripts/fetch-headless-compositor.sh`, built
with Meson by `scripts/build-headless-compositor.sh`). It is a single-file, wlroots-free
compositor on `libwayland-server` — **no X11 dependency** — providing exactly what the tests
need:

* frame capture to PNG,
* scripted pointer/keyboard injection,
* deterministic surface/configure handling.

Classification per run:

| Result | Meaning |
|---|---|
| `ok` | started, painted, frame has structure; scripted input produced the expected state change |
| `ran` | started and exited with no missing symbol, but painted nothing |
| `gap` | needs a symbol/behaviour LibWM does not provide → becomes a LibWM task |
| `crash` | died on a signal → always a LibWM defect |

Beyond "did it paint", assertions target behaviour: a scripted click sequence on the
Calculator keypad must produce the expected display string; PDFViewer must page/zoom.

### 5.3 Reference comparison

Upstream ships no reference screenshots for these apps. As in the sibling project, the oracle
is a second implementation of the same thing: run the **same application** against SerenityOS
(captured under QEMU, or a future Serenity build with the same renderer) and compare
**layout**, not glyph antialiasing — reduce frames to a coarse ink grid and require cell-level
agreement. Where Serenity capture is not yet available, assert structure (control bounding
boxes, text strings, colour counts) and rely on manual review.

### 5.4 Interactive verification

Menus, popups, clipboard, IME and focus cannot be seen headlessly. After the matrix passes, the
apps are run in the live Plasma session and exercised by hand: keypad, scroll/zoom, open a PDF,
open menus, copy/paste, resize/maximise, HiDPI.

---

## 6. Risks and mitigations

| Risk | Why plausible | Mitigation |
|---|---|---|
| Full LibGUI does not host-compile cleanly | Lagom only ever built 6 files; `LibCore`/`LibGfx` host paths differ subtly from Serenity | **Resolved for M1**: full LibGUI builds via `patches/0001-lagom-build-full-libgui.patch`; only 3 TUs touch Serenity-only headers (§2.1) |
| Missing host libraries (`LibConfig`, `LibDesktop`, `LibFileSystemAccessClient`) | Not in Lagom's list | Add minimal host targets; serve their portals in-process |
| Startup deadlock on `fast_greet` | Constructor blocks before the event loop | LibWM server thread sends `fast_greet` on accept, before `Application::create` returns |
| Menus are server-rendered | More logic than a pure transport | `WM::MenuRenderer` is self-contained and reuses LibGfx + theme; xdg_popup + compositor grab |
| Key mapping mismatches | Serenity `KeyCode`/scancodes vs `xkbcommon` | Explicit table from `Kernel/API/KeyCode.h`; unit tests per key |
| Decorations vary by compositor | `xdg-decoration` is not universal | Prefer server-side; fall back to LibWM CSD with `set_window_geometry` |
| Absolute window position is compositor-owned | Wayland has no client-set global position | Approximate `window_moved`/`get_global_cursor_position`; accept that persisted window coordinates are advisory |
| `wl_shm` rejects the client memfd | Pool format/stride constraints | Fallback copy path into LibWM buffers |
| HiDPI is soft until scale is plumbed | LibWM does not own the client's drawing, and LibGUI's backing store is logical-size (`Window::create_backing_store` FIXME) | Present 1:1 initially; later upstream a scale factor into LibGUI's backing store (§3.5) |
| Clipboard/config portals fail at startup | `Application::create` initialises clipboard unconditionally | Serve all eight portals before `main`; portal registry returns a valid in-process socket |
| App build system coupling | `serenity_app`/`embed_resource` assume Serenity | Provide Lagom-compatible macros; accommodations are build-only |
| Hidden Serenity assumptions in app code | e.g. `/res` paths, `pledge`/`unveil` | `ResourceImplementation` remap; Lagom already no-ops `pledge`/`unveil` |

---

## 7. Milestones

| # | Milestone | State |
|---|---|---|
| M0 | Architecture/design + protocol & build audit (this document) | ✅ done |
| M1 | Full host build of `LibGfx` + **full** `LibGUI` (no windows yet) | ✅ done — 132 objects, `liblagom-gui.so` (§4.1) |
| M2 | `LibWM` skeleton: portal registry, socketpair transport, `fast_greet`, one `xdg_toplevel`, backing-store blit — headless | ✅ done (in-process transport; live Plasma toplevel, §4.2) |
| M3 | Pointer/keyboard input from `wl_seat`, focus/activation, close request; then **Calculator** | ✅ input + unmodified **Calculator** renders & runs on Plasma (§4.3) |
| M4 | Theme/font parity; decorations; icons; alpha | ⬜ |
| M5 | **PDFViewer** (LibPDF, scrolling, toolbars, file access) | 🟡 unmodified **PDFViewer** builds and renders a PDF on the host (§4.7); toolbar + menubar work, file access via a stub portal |
| M6 | Menus/popups, clipboard, config persistence | 🟡 native clipboard + server-rendered menubar/popups/submenus + keyboard nav done (§4.5, §4.6); Config + FileSystemAccess portals in-process (§4.8); config persistence pending |
| M7 | Live Plasma session, headless test harness, CI matrix | 🟡 headless compositor + input test integrated (§5.2); CI matrix pending |
| M8 | Crisp HiDPI (plumb an output scale into LibGUI's backing store) | ⬜ |

The state column is kept honest as work proceeds; §6 records every gap found, whether fixed in
LibWM or shown to be an upstream/host issue.

---

## 8. Appendix

### 8.1 Repository layout

```
DESIGN.md                     this document
README.md                     quick start, status, limitations
LICENSE                       BSD-2-Clause (matches SerenityOS)
patches/                      build-system and LibCore host seams applied to the checkout
  0001-lagom-build-full-libgui.patch
  0002-libcore-host-seams.patch
scripts/fetch-serenity.sh     pinned, blobless, sparse checkout of SerenityOS
scripts/build-libgui.sh       apply patches + build LibGfx/LibGUI/LibWM/apps + run tests
scripts/run-lib-tests.sh      upstream LibGfx test suite
scripts/fetch-headless-compositor.sh   pinned clone of xlib-wayland (pure-Wayland compositor)
scripts/build-headless-compositor.sh   Meson-build just the headless-compositor target
scripts/run-headless-tests.sh headless app + synthetic-input integration test
scripts/run-clipboard-test.sh native clipboard integration test (read + write)
src/                          LibWM
  LibWM.{h,cpp}               portal registration + server threads
  WindowServerConnection.{h,cpp}  WindowServer protocol server
  WaylandClient.{h,cpp}       real compositor: registry, output, xdg_toplevel, wl_shm, seat/input
  LaunchServerConnection.h    minimal LaunchServer (startup allowlist)
  ClipboardServerConnection.h minimal ClipboardServer (empty data)
  *DefaultStub.h              GENERATED per-endpoint stubs (tools/gen-default-stub.py)
  test/TestWindow.cpp         minimal unmodified-style LibGUI test app
serenity/                     pinned SerenityOS checkout (gitignored)
third_party/xlib-wayland/     pinned checkout, used only for its headless compositor (gitignored)
build/                        host build outputs (gitignored)
tools/gen-default-stub.py     generate a default stub for any IPC endpoint
tools/wl-clipboard-peer.c     native libwayland-client clipboard peer (tests)
```

Changes to files under `third_party/xlib-wayland/` (e.g. compositor improvements) are made in
that checkout and **pushed back to `github.com/jmalcolm137/xlib-wayland`**, not vendored here.

### 8.2 Environment variables

| Variable | Default | Used by |
|---|---|---|
| `LIBWM_PREFIX` | `~/.local/libgui-wayland` | build/run scripts |
| `SERENITY_PREFIX` | `$LIBWM_PREFIX/serenity` | host-built LibGfx/LibGUI |
| `SERENITY_SRC` | `serenity/` | pinned upstream source |
| `SERENITY_RES_ROOT` | (unset → `/res`) | LibCore resource root; must point at `serenity/Base/res` |
| `LIBWM_RES` | `$LIBWM_PREFIX/share/serenity/res` | bundled fonts/themes/icons |
| `XLIB_WAYLAND_SRC` | `third_party/xlib-wayland` | headless-compositor checkout |
| `XLIB_WAYLAND_BUILD` | `build/xlib-wayland` | headless-compositor build dir |
| `LIBWM_TRACE` | unset | protocol/verbose logging |
| `WAYLAND_DISPLAY` | session | target compositor |

### 8.3 WindowServer method disposition (client→server, as called by LibGUI)

| Group | Methods | LibWM |
|---|---|---|
| Window lifecycle | `create_window`, `destroy_window`, `set_window_title`, `set_window_rect`, `get_window_rect`, `get_window_floating_rect`, `set_window_minimum_size`, `get_window_minimum_size`, `set_window_parent_from_client`, `move_window_to_front`, `set_always_on_top`, `start_window_resize` | implemented on xdg_toplevel |
| Window state | `set_maximized`, `set_minimized`, `set_fullscreen`, `set_frameless`, `set_forced_shadow`, `set_window_has_alpha_channel`, `set_window_alpha_hit_threshold`, `set_window_base_size_and_size_increment`, `set_window_resize_aspect_ratio`, `set_window_modified`, `is_window_modified`, `set_window_progress`, `set_window_icon_bitmap` | implemented / approximated |
| Paint | `set_window_backing_store`, `invalidate_rect`, `did_finish_painting` | implemented (wl_shm + frame callbacks) |
| Menus | `create_menu`, `destroy_menu`, `set_menu_name`, `set_menu_minimum_width`, `add_menu`, `add_menu_item`, `add_menu_separator`, `update_menu_item`, `remove_menu_item`, `flash_menubar_menu`, `popup_menu`, `dismiss_menu` | `WM::MenuRenderer` |
| Cursor/global | `set_window_cursor`, `set_window_custom_cursor`, `set_global_mouse_tracking`, `get_global_cursor_position`, `get_color_under_cursor`, `get_applet_rect_on_screen`, `get_window_rect_from_client` | approximated / stubbed |
| Desktop | `set_background_color`, `set_wallpaper_mode`, `set_wallpaper`, `get_wallpaper`, `refresh_system_theme` | stubbed (compositor owns desktop) |
| DnD | `start_drag`, `set_accepts_drag` | `wl_data_device` (later) |

Server→client events consumed by LibGUI: `fast_greet`, `paint`, `mouse_*`, `key_down/up`,
`window_entered/left`, `window_activated/deactivated`, `window_input_preempted/restored`,
`window_close_request`, `window_resized/moved`, `window_state_changed`,
`menu_item_activated/entered/left`, `menu_visibility_did_change`, `screen_rects_changed`,
`applet_area_rect_changed`, `drag_*`, `update_system_theme/fonts/effects`,
`display_link_notification`, `track_mouse_move`, `ping`.

### 8.4 References

* SerenityOS — `Userland/Libraries/LibGUI/ConnectionToWindowServer.{h,cpp}`,
  `LibGUI/Window.cpp`, `LibGUI/Application.cpp`
* Protocol — `Userland/Services/WindowServer/{WindowServer,WindowClient}.ipc`,
  `LibIPC/Connection*.h`, `Meta/Lagom/Tools/CodeGenerators/IPCCompiler`
* Host portability — `Meta/Lagom/CMakeLists.txt`, `LibCore/{AnonymousBuffer,System,
  ResourceImplementation,SessionManagement}.cpp`, `LibCore/Socket.{h,cpp}`
* Resources/theme — `LibGfx/Font/FontDatabase.cpp`, `LibGfx/SystemTheme.cpp`,
  `Base/res/{fonts,themes,color-schemes,icons}`
* Applications — `Userland/Applications/{Calculator,PDFViewer}`
