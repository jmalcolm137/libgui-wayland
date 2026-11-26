# Architecture

How LibWM is put together today. For the reasoning behind the approach, see
[DESIGN.md](../DESIGN.md) §1; for how it got here, [development-history.md](development-history.md).

## Layering

```
┌──────────────────────────────────────────────────────────────────────────┐
│  unmodified SerenityOS applications (Calculator, PDFViewer, Piano, …)    │
├──────────────────────────────────────────────────────────────────────────┤
│  LibGUI / LibGfx / LibCore / LibIPC (upstream + patches/)                │
│     IPC over Core::LocalSocket to "/tmp/portal/window", "…/clipboard", … │
├──────────────────────────────────────────────────────────────────────────┤
│  LibWM (src/, libwm.so) — linked into every application                  │
│     portal servers (LibWM server thread)        Wayland client (main)    │
│     WindowServerConnection ─┐                   WaylandClient            │
│     ClipboardServerConn.    ├── calls ───────►    WaylandOutputs         │
│     ConfigServerConn.       │                     WaylandSeat            │
│     LaunchServerConn.       │◄── callbacks ───    WaylandDataDevice      │
│     FileSystemAccessConn.   │  (on_server_thread) SerenityShell (opt.)   │
│     ImageDecoder (upstream)─┘                                            │
├──────────────────────────────────────────────────────────────────────────┤
│  libwayland-client · xkbcommon            Wayland compositor (any)       │
└──────────────────────────────────────────────────────────────────────────┘
```

Nothing above LibWM knows Wayland exists; nothing in the compositor knows
SerenityOS exists. The one exception is the optional `serenity-window` protocol,
which a desktop's compositor may offer and every other compositor ignores.

The applications in the top box are not part of this repository: the framework
builds them into itself only as test clients (`src/test/`), and the desktop that
ships them owns their build targets, patches and assets (see
[limitations.md](limitations.md)).

## Process model and transport

LibWM is a library in the application process, not a server. Each application
is an ordinary Wayland client with its own `wl_display` connection.

SerenityOS code reaches its services through `Core::LocalSocket::connect()` to a
portal path. The `Core::PortalServer` seam (`patches/libcore/0002`,
`patches/libcore/0008`) lets LibWM answer those connects. `LibWM::initialize()`
runs from a static initializer, so applications need no source changes, and
installs a connector (`make_portal()` in `src/LibWM.cpp`) that, for each path:

1. **defers to a real session service** if a live socket is listening there
   (a stale socket is removed);
2. **spawns an out-of-process service** for the portals that must not be linked
   into every application: `WebContent` and `RequestServer` (one process per
   connection, socket passed with `SOCKET_TAKEOVER`), `SQLServer` and
   `NotificationServer` (multi-client: LibWM binds the listening socket and hands
   it over), and `AudioServer` (both audio portals, one process);
3. otherwise **serves it in-process**: `window`, `clipboard`, `config`, `launch`,
   `filesystemaccess` and `image`. These are socketpairs whose server ends live
   on the LibWM server thread (see [threading.md](threading.md)).

Portal paths and environment lookups are in `src/Environment.{h,cpp}`.

## The Wayland client

`WaylandClient` (`src/WaylandClient.{h,cpp}`) owns the `wl_display`, the registry
and the core globals, the window and popup tables, and event dispatch (a
`Core::Notifier` on the display fd, on the main thread). The protocol areas are
separate classes that share its mutex and window table:

| Class | Protocols | Responsibility |
|---|---|---|
| `WaylandClient` + `WaylandSurface.cpp` | `xdg_wm_base`, `wlr-layer-shell`, `xdg-decoration`, `wp_viewporter`, `wp_fractional_scale_v1`, `wl_shm` | windows: create/destroy, state, presentation, CSD |
| `WaylandClient` + `WaylandPopup.cpp` | `xdg_popup`, layer overlays, `wl_subcompositor` | menu popups, the menubar subsurface |
| `WaylandOutputs` | `wl_output`, `zxdg_output_v1` | screen size, output scale, client (backing-store) scale |
| `WaylandSeat` | `wl_seat`, `wl_pointer`, `wl_keyboard`, `zwp_text_input_v3` | input routing and translation, IME, cursors |
| `WaylandDataDevice` | `wl_data_device` | clipboard and drag-and-drop |
| `SerenityShell` | `serenity-window` (optional) | the desktop's host settings, wallpaper, capture, compositor-driven menubar and placement |

Events reach the WindowServer side through `WaylandClient::InputCallbacks` and a
few named callbacks; `WindowServerConnection` re-posts every one of them to the
server thread.

## Windows

`WindowServerConnection::create_window` → `WaylandClient::create_window` picks a
role from the Serenity window type:

| Window type | Wayland role |
|---|---|
| Normal, Dialog, … | `xdg_toplevel` (+ server-side decoration if offered, else CSD), transient parent for dialogs |
| Taskbar | bottom layer-shell panel with an exclusive zone |
| Desktop | background layer surface filling the output |
| Applet | top layer surface; a desktop's compositor lays it out in the applet area |
| Popup, Autocomplete, Tooltip, Menu, Notification | overlay layer surface at the client's position |

**Interactive resize.** Dragging a statusbar `ResizeCorner` sends
`start_window_resize`; LibWM maps the direction to `xdg_toplevel.resize` with the
seat serial of the press that started it, and the compositor takes over. Its
configures carry `RESIZING`, so LibWM stretches the last frame through
`wp_viewport` instead of re-rendering for every step. A request without a recent
input serial, from a non-resizable window, or for `ResizeDirection::None` is
ignored rather than sent.

Window icons use `xdg-toplevel-icon-v1` when the compositor offers it (an shm buffer
at the icon's scale, attached to the toplevel), so taskbars and window switchers
draw them like any native application's; `serenity_toplevel.set_icon` remains the
fallback for older SerenityDE compositors.

State that is presented on Wayland (title, minimum size, alpha, maximized) is
owned by the `WindowSurface`; `WindowServerConnection::Window` keeps only the
protocol view (the client-visible rect, the modified flag, the last backing
store, resize coalescing) and answers queries from the Wayland side.

## Rendering and presentation

LibGUI paints into a shared-memory backing store and hands LibWM its fd with
`set_window_backing_store`. `commit_window_content` presents it in one of three
ways:

* **Direct** (the common case): a `wl_shm_pool` over the client's own fd and a
  `wl_buffer` referencing its pixels; no copy. Damage is the rects LibGUI
  reported with `did_finish_painting`.
* **Composed**: when something must be drawn above the content (a client-side
  titlebar, or the menubar on compositors without the subsurface path), the
  damaged rows are copied into a LibWM buffer below the inset.
* **Fractional scale**: the client renders at `ceil(scale)` and LibWM resamples to
  `round(logical × scale)` pixels, presented at buffer scale 1 with
  `wp_viewport.set_destination` giving the logical size.

Presentation is paced by frame callbacks: while a frame is in flight only the
newest content is kept. The opaque region is declared for windows without alpha
(only when its size changes), and windows with an alpha hit threshold get an
input region built from their alpha channel, so transparent pixels click through.

**Menubar.** With `serenity-window` (`serenity_toplevel` v2) and a subcompositor, the
menubar is a subsurface above the content and the compositor offsets the window;
otherwise it is composed into the window buffer. `MenuController` renders it into
an image on the server thread whenever the menubar changes or the window is
resized; the Wayland side only copies that image when it presents a frame.

**Client-side decorations** (22 px titlebar with close/maximize/minimize) are
drawn when the compositor offers no `xdg-decoration`, asks for client-side mode,
or `LIBWM_FORCE_CSD` is set.

**OpenGL** is client-side and invisible here: LibGL renders through a
`GPU::Device` (`libeglgpu/` over Mesa, or LibSoftGPU) into a bitmap the widget
paints like any other.

## Input

`WaylandSeat` routes each pointer event to a popup, a menubar (subsurface or
inset), a CSD titlebar (buttons, move, window menu) or the window content, and
subtracts insets so widgets see content-local coordinates. Buttons, fractional
axis deltas (accumulated into whole steps), XKB key codes, code points
(Ctrl+letter becomes the control character, as Serenity's WindowServer did) and
modifiers are translated. Double-clicks are synthesised in
`WindowServerConnection`. `zwp_text_input_v3` relays preedit and commit text to
the application. Held keys repeat: Wayland leaves repeat to the client, so LibWM arms a timer
from `wl_keyboard.repeat_info` (delay, then 1/rate) and re-sends the press.
Cursors are the bundled SerenityOS cursor theme (decoded once and cached) or a custom bitmap,
set with `wl_pointer.set_cursor`.

## Menus

Serenity menus are server-rendered: the client sends only the model.
`MenuController` (`src/MenuController.{h,cpp}`) keeps the models, lays them out,
renders popups and the menubar with LibGfx and the system theme, and handles
hover, keyboard navigation, submenus and scrolling. Popups are `xdg_popup`s with a
grab for toplevel parents, and overlay layer surfaces for layer-surface parents
(the Desktop and Taskbar have no xdg parent).

## Clipboard and drag-and-drop

`ClipboardServerConnection` bridges Serenity's multi-representation clipboard to
`wl_data_device` offers: text under its usual aliases, HTML, URI lists, and images
as `image/png` (transcoded from Serenity's raw format, which is also offered
for lossless LibWM-to-LibWM copies). SerenityOS has one clipboard and native
toolkits two selections, so the same representations are also published as the
**primary selection** (`zwp_primary_selection_v1`), and reads fall back to PRIMARY
when nothing owns CLIPBOARD: a copy in either world can be pasted in the other
(middle-click into a native application, Ctrl+V in a Serenity one).

Two wlroots/SDE specifics shape this. wlroots delivers PRIMARY only to the
**keyboard-focused** client (`wlr_primary_selection_v1` filters by
`seat->keyboard_state.focused_client`), so a Serenity application sees the native
selection while it is focused — which is when a paste happens — but not in the
background. And `set_selection` there is checked against a serial from a recent
input event (`wlr_seat_client_validate_event_serial`), so a client without focus
or without a seat cannot publish; the SDE's headless tests therefore give the
application a keyboard (see the SDE's `run-primary-selection-test.sh`). Drag-and-drop works in both directions; the
drop target's widget decides acceptance. Offers from other clients are read with
timeouts, so a stuck source cannot freeze the application.

## Desktop appearance

A desktop that speaks `serenity-window` sends its theme (`theme.path`,
`theme.name`), and that wins. Without one, LibWM follows the standard
`org.freedesktop.portal.Settings` on the session bus:
`org.freedesktop.appearance color-scheme` (0 no preference, 1 prefer dark,
2 prefer light) selects the bundled `Default`, `Dark` or `Light` theme, and the
portal's `SettingChanged` signal re-applies when the user switches. Only the
appearance preference has a standard API; fonts, pointer/keyboard settings and
the display layout have none, so outside a SerenityDE session those stay at the
bundled defaults and the compositor's own configuration.

## Other in-process portals

| Portal | Implementation |
|---|---|
| `config` | `ConfigServerConnection`: typed values, persisted to `<config dir>/<domain>.ini` |
| `launch` | `LaunchServerConnection`: `Launch::HandlerDatabase` resolution, spawning host binaries |
| `filesystemaccess` | `FileSystemAccessServerConnection`: approved requests open directly; prompts show LibGUI's `FilePicker` on the main thread |
| `image` | upstream `ImageDecoder::ConnectionFromClient`, decoding on the server thread |

## Desktop integration

`SerenityShell` is the only code that speaks `serenity-window`. Without it (any
other compositor) settings fall back to defaults and windows get no
compositor-driven chrome. With it, the desktop owns its settings: LibWM answers
Display Settings and friends from the `host_settings` it sends and forwards their
changes with `set_host_setting`, never knowing where or how the desktop stores
them. The keys, including the theme, fonts and the on-demand wallpaper file, are
documented in the protocol XML.
