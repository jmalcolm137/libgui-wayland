# Development history

This is the project's design-time record: the audit that justified the
approach, and the notes written as each milestone landed. It is kept for the
reasoning it captures, not as a description of the current code. Names, file
paths and "remaining work" here are as they were at the time; see
[architecture.md](architecture.md) for how things work now. Section numbers
(§2–§7) are this document's own and match the references in DESIGN.md §1.

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

The change is a single build-system patch, `patches/lagom-and-resources/0001-lagom-build-full-libgui.patch`
(+30/−12 lines in `Meta/Lagom/CMakeLists.txt`), applied idempotently by
`scripts/build-libgui.sh`. No `LibGUI`/`LibGfx` source is edited. Preferred long-term:
contribute the full `LibGUI` target to Lagom upstream (it already builds the GML subset).

### 4.1.1 SerenityOS fixes

`scripts/fetch-serenity.sh` fetches the checkout from our public fork,
[`jmalcolm137/serenity`](https://github.com/jmalcolm137/serenity), branch `fixes`:
the pinned upstream revision (`SERENITY_UPSTREAM_BASE`) plus fixes that make sense
for SerenityOS **on its own**, each an ordinary commit and a candidate for an
upstream PR. A change that only exists because of how this port, or the Serenity
Desktop Environment, uses SerenityOS does not belong there, however small: the
port's host changes stay in `patches/`, and the SDE's in its own `patches/` (whose
`README.md` tracks every change and its upstream status).

Moving the base forward means rebasing `fixes` onto a newer upstream (real commits,
so `git rebase` does the work) and pointing `SERENITY_REVISION` at the new tip.

### 4.2 M2 result — the shim is live

The first vertical slice is implemented and verified on a real KDE Plasma Wayland session:

* **Transport seam.** `patches/libcore/0002-libcore-host-seams.patch` adds the `Core::PortalServer`
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
* **Interactive resize.** The compositor owns the drag (server-side decorations); while it is
  in progress the toplevel configure carries `XDG_TOPLEVEL_STATE_RESIZING`. LibWM then keeps
  the client's **last frame** and stretches it to the new size with `wp_viewporter` instead of
  asking the client to repaint for every configure; when the state clears it delivers a single
  `window_resized` — one re-render for the whole drag. This matters because LibGUI apps can do
  heavy work on resize: `PDFViewer::resize_event` throws away and re-rasterises every rendered
  page, so a live resize would otherwise re-render the document on every pointer move. Presents
  are additionally paced to the compositor with `wl_surface.frame` callbacks, and
  configure-driven repaints are coalesced to one in flight per window. Verified by
  `scripts/run-pdfviewer-test.sh`.

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

* **Representations.** The Serenity clipboard protocol carries a list of representations
  (`GUI::ClipboardEntry`: bytes, mime type, metadata), not a single item. `Clipboard::set_entries`
  publishes the list and `Clipboard::fetch_entries` reads it back. The older single-item
  `set_data`/`set_plain_text`/`set_bitmap` calls are wrappers over one-entry lists, and
  `fetch_data_and_type` returns the preferred entry.
* **Reads.** LibWM watches `wl_data_device.data_offer`/`selection`, records the offered mime
  types, and fetches every canonical representation: one text form
  (`text/plain;charset=utf-8` → `text/plain` → `UTF8_STRING`), `text/html` (plus its charset
  alias), `text/uri-list`, and an image. Text is normalized to `text/plain`; `image/png` is
  transcoded with LibGfx into Serenity's raw `image/x-serenityos` (pixels +
  `width/height/scale/format/pitch` metadata) so `Clipboard::as_bitmap()` works. Other types
  pass through.
* **Writes.** `set_clipboard_entries` builds the Wayland offers from every entry: plain text
  gains the `text/plain;charset=utf-8`/`UTF8_STRING`/`STRING` aliases, `text/html` gains a
  charset alias, `text/uri-list` and other types pass through unchanged, and
  `image/x-serenityos` is encoded to `image/png` (and also offered raw, so LibWM-to-LibWM
  image copies stay lossless). A rich copy therefore offers text/plain, text/html, and the
  image at the same time.
* **Same thread as input, on purpose.** `set_selection` requires a serial from a *recent input
  event on the same connection*, so the data device lives on the connection that owns the
  seat. LibWM therefore serves all portals from a single server thread with one
  `Core::EventLoop` and one `WaylandClient`; the last input serial is tracked from
  pointer/keyboard events. (Interactive Copy — Ctrl+C or a menu action — supplies a serial;
  a selection set with no prior input is ignored by real compositors, which is correct Wayland
  behaviour.)

Both directions are verified headlessly for **text**, **`image/png` ↔ `image/x-serenityos`**,
**`text/uri-list`**, and **multiple representations at once** against a native `libwayland-client` peer
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

### 4.8.2 LibSerenityAudio — LibAudio ↔ PipeWire

`LibSerenityAudio` is a **second shim**, shaped exactly like LibWM: it implements a
SerenityOS service protocol in-process and translates it to the native host API. Where LibWM
speaks the WindowServer protocol to Wayland, LibSerenityAudio speaks the **AudioServer**
protocol to **PipeWire**.

* **Protocol.** SerenityOS apps stream audio by opening `ConnectionToServer` on
  `/tmp/session/%sid/portal/audio`, then handing the server a **shared single-producer
  circular queue** (`Audio::AudioQueue`, lock-free) that they fill from a high-priority
  thread. LibSerenityAudio registers an `AudioServer` endpoint server for that portal and
  attaches to the client's queue (`AudioQueue::create(fd)`).
* **Data plane.** A `Mixer` holds one `ClientAudioStream` per connection (queue + resampler +
  volume/mute), exactly like the AudioServer's mixer. PipeWire's real-time `process` callback
  pulls mixed samples from all streams and writes interleaved `f32` stereo into the stream's
  buffer. Because the queue is SPSC, the RT thread never locks the client.
* **Latency.** The callback fills only the frames PipeWire requests
  (`pw_buffer::requested`), not the whole (much larger) buffer — filling `maxsize` added
  ~280 ms and over-produced. The remaining latency is the client's own ring buffer, so its
  depth is tuned host-side (`AUDIO_BUFFERS_COUNT` 128 → 16 ≈ 12 ms); producer and consumer
  share the constant, and underruns stay at zero.
* **Registration.** `Core::PortalServer` was generalised from a single connector to a small
  registry (`add_connector`), so LibWM and LibSerenityAudio each register their own portals
  independently and neither owns the seam.
* **Host seams** (`patches/other-libraries/0003-*`): build `LibAudio`'s `ConnectionToServer` on the host,
  define `THREAD_PRIORITY_MAX`, and let `LibThreading::Thread::set_priority` be called before
  `start()` (glibc faults on the unset handle).

Verified by `scripts/run-piano-test.sh`: the unmodified **Piano** app builds and renders, the
AudioServer portal is served, and (when PipeWire is present) the stream reaches `streaming`
with the process callback running and zero underruns.

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

### 4.11 LibGL / EGLGPU / LibSoftGPU — client-side GL

SerenityOS's OpenGL implementation is not part of the WindowServer protocol and required no
LibWM work. The stack is:

* **`LibGPU`** — the abstract `GPU::Device` backend seam plus `GPU::Driver`, which `dlopen()`s a
  backend and resolves `serenity_gpu_create_device()`.
* **`EGLGPU`** (this project) — a `GPU::Device` implemented over a **desktop OpenGL compatibility
  context** obtained through EGL (Mesa), rendering into an offscreen FBO.
* **`LibSoftGPU`** — the CPU rasterizer, kept as the universal fallback (LibWeb still links it
  directly).
* **`LibGL`** / **`LibGLSL`** — an OpenGL 1.x API (`GL::GLContext`) over a `GPU::Device`, with
  GLSL compiled by `LibGLSL`.

`GL::create_context(Gfx::Bitmap&)` selects the backend from `$LIBGL_GPU_DRIVER`. The default is
`eglgpu`; if the driver library cannot be loaded the code falls back to `softgpu`, and `EGLGPU`
*itself* returns a `SoftGPU::Device` when no EGL context can be created — so GL always works,
with or without a GPU. Rendering targets an **app-owned `Gfx::Bitmap`**: `GLContext::present()`
blits the device's colour buffer into it (`LibGL/GLContext.cpp:835`), and the widget's
`paint_event` draws that bitmap with `GUI::Painter` — e.g. `3DFileViewer`'s
`GLContextWidget::paint_event`. From there it is an ordinary client bitmap: window backing store
→ `wl_shm` → compositor. The WindowServer IPC (`WindowServer.ipc`) has no GL/GPU messages, which
is why the GL stack "just works" through LibWM.

**EGLGPU.** `libeglgpu.so` (`libeglgpu/`) requests a desktop GL **compatibility-profile** context
— the `GPU::Device` interface is fixed-function, so it maps almost 1:1 onto GL 1.x/2.x calls —
renders into an FBO, and reads the result back with `glReadPixels`. Texture storage is delegated
to `LibSoftGPU::Image` (RGBA `FloatVector4`, with full `PixelConverter` format handling) and
uploaded into a GL texture on demand, keeping the format-conversion surface tiny. Two integrity
details matter:

* **Symbol interposition.** Serenity's `LibGL` exports the same `gl*` symbols as Mesa's `libGL`,
  so `GPU::Driver` loads every backend with `RTLD_DEEPBIND`; without it `EGLGPU`'s GL calls would
  bind to LibGL's implementation instead of Mesa's (`patches/other-libraries/0007-*`).
* **Driver default.** `GLContext::create_context()` prefers `eglgpu`, honours an explicit
  `$LIBGL_GPU_DRIVER`, and falls back to `softgpu` if the `eglgpu` library is missing.

**Host build (works today).** Lagom's `lagom_standard_libraries` already builds `GL`, `GLSL`,
`GPU`, `SoftGPU` and `AccelGfx`; this project adds `eglgpu`, guarded by `find_package(OpenGL)`,
so a host without GL development files simply keeps `softgpu`.

**Verified.** `scripts/run-3dfileviewer-test.sh` runs the unmodified **3DFileViewer** under the
headless compositor with a bundled OBJ model: it loads the mesh, renders it with lighting and
texturing, and presents it over `wl_shm` as a real Wayland toplevel (frame dumped to PNG). This
passes with `EGLGPU` (Mesa) and, by explicit override, `LibSoftGPU`; forcing EGL to fail
(`__EGL_VENDOR_LIBRARY_FILENAMES=/nonexistent`) exercises `EGLGPU`'s internal fallback and still
renders. The `Tubes` demo builds against the same path.

**Separate host GPU path.** `LibAccelGfx` is unrelated to `LibGL`: it is a 2D/GLES renderer that
LibWeb uses when `HAS_ACCELERATED_GRAPHICS` is on (host EGL on Linux, CGL on macOS), including
WebGL (`glReadPixels` back into the canvas bitmap). It is enabled in this build.

**Not done.** `EGLGPU` renders on the GPU but reads back to a `Gfx::Bitmap` and presents through
`wl_shm`; zero-copy `zwp_linux_dmabuf_v1` hand-off is not implemented. It has no GLSL shader path
(`create_shader()` returns an error), so shader-based programs need `LibSoftGPU`. LibWM's only
possible addition here is importing a dmabuf buffer instead of (or alongside) `wl_shm`.

## 5. Test strategy (as originally planned; see testing.md for the current suites)

### 5.1 First targets

| App | Exercises |
|---|---|
| `Calculator` | `Application`/`Window`, GML widget tree, buttons, keyboard input, theme, fonts, `LibConfig`, clipboard init |
| `PDFViewer` | everything above + `LibPDF` rasterisation, `ImageWidget`/`AbstractScrollableWidget`, scrollbars, toolbars, `FileSystemAccess`, file I/O, larger window |
| `3DFileViewer` | client-side **`LibGL`/`LibGPU`/`LibSoftGPU`**: GL context over an offscreen `Gfx::Bitmap`, per-frame animation, OBJ loading, depth/lighting/texture, presentation over `wl_shm` (§4.11) |

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

## 6. Risks and mitigations

| Risk | Why plausible | Mitigation |
|---|---|---|
| Full LibGUI does not host-compile cleanly | Lagom only ever built 6 files; `LibCore`/`LibGfx` host paths differ subtly from Serenity | **Resolved for M1**: full LibGUI builds via `patches/lagom-and-resources/0001-lagom-build-full-libgui.patch`; only 3 TUs touch Serenity-only headers (§2.1) |
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
| M9 | Audio: **LibSerenityAudio** (LibAudio ↔ PipeWire) and **Piano** | ✅ unmodified **Piano** builds, renders and plays through PipeWire (§4.8.2) |
| M10 | GPU: client-side **LibGL** with an **EGLGPU**/Mesa backend and a **LibSoftGPU** fallback | ✅ unmodified **3DFileViewer** renders through `EGLGPU` (falling back to `LibSoftGPU`) and presents over `wl_shm` (§4.11); zero-copy `dmabuf`/Vulkan deferred (§1.2) |

The state column is kept honest as work proceeds; §6 records every gap found, whether fixed in
LibWM or shown to be an upstream/host issue.

### 7.1 Remaining work

In rough priority order:

* **Crisp fractional HiDPI (M8).** `wp_viewporter` is already bound and `zxdg_output_v1` gives
  the real logical size; the remaining step is to render the client's backing store at the
  output's device scale and declare it (`wp_viewport` / buffer scale), so text is sharp on the
  1.75× panel instead of being upscaled by the compositor (§3.4, §4.2).
* **Menus (M6 remainder).** Scrolling for long menus, and drawing menu-item icons (the model
  already carries them; `MenuController::render_popup` ignores them).
* **Config persistence.** The in-process Config portal (`ConfigServerConnection`) is an
  in-memory store; it does not persist to disk between runs.
* **Audio polish.** `LibSerenityAudio` has no AudioManager portal (so no system
  mixer/volume integration), and cross-rate clients use LibAudio's naive insert/drop
  resampler. Multi-client mixing itself is done.
* **M4 polish.** Theme/font parity, decorations, icons, alpha.
* **M7.** A CI matrix around the headless suites.

### 7.2 Deferred: a Serenity Desktop Environment on Linux

A handful of applications and settings target SerenityOS **system services** rather than a
portable protocol. Shimming each one piecemeal would bake Serenity's kernel/service APIs into
the compatibility layer, so they are deferred until there is a **Serenity Desktop Environment
on Linux** — a Serenity-shaped session that provides the missing services and per-compositor
backends.

**Applications** (in `Userland/Applications`, deliberately not built):

| App | Needs | Why deferred |
|---|---|---|
| `Terminal` | a Serenity pty and `LibVT`'s Serenity terminal backend | Linux has ptys (`posix_openpt`), but `LibVT`'s plumbing is Serenity-specific |
| `SystemMonitor` | Serenity `/proc` and `LibDebug`/`LibSymbolication` (`sys/arch/regs.h`, ptrace) | Linux `/proc` and the process model differ |
| `Debugger` | Serenity's ptrace ABI, `sys/arch/regs.h`, core/ELF model | non-portable |
| `CrashReporter` | Serenity's core-dump format, `LibCoredump`, `LibSymbolication` | non-portable |
| `MouseSettings` | WindowServer *service* internals plus compositor-specific pointer settings | see below |

**Settings with no Wayland standard.** These are *compositor/desktop* configuration, not
client-facing protocol:

* **Pointer acceleration, scroll step, double-click speed, cursor highlight.**
* **Display configuration** (resolution/refresh/scale/rotation): no adopted Wayland protocol —
  `wlr-output-management-unstable-v1` (wlroots only), `org.gnome.Mutter.DisplayConfig`,
  `org.kde.KScreen`.
* **Cursor theme/size**: the *transport* is standard (`org.freedesktop.portal.Settings`) but the
  keys are desktop-specific; `LibEDID` (parsing) is portable, *applying* a mode is not.

**The portable path (not deferred)** is a **portal service layer**: a provider behind
`Core::PortalServer` that proxies `org.freedesktop.portal.Settings` (appearance/theme: color
scheme, accent, cursor theme/size with a small key map) and, for the wider framework,
`org.freedesktop.portal.{Screenshot,Notification,FileChooser,OpenURI,...}`. The
compositor-specific capabilities above sit behind a thin **per-compositor backend interface**
(one implementation per desktop — KDE first for the reference session — then GNOME/wlroots), so
the applications stay portable and functional where a backend exists.

The general rule: **read** compositor state from standard Wayland (`wl_output`/`xdg_output_v1`,
`wl_seat`, `cursor-shape-v1`) and the portals; **write** system settings only through a backend.
