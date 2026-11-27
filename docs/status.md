# Status

The framework targets the pinned SerenityOS revision (`scripts/fetch-serenity.sh`); the host
changes are the patch series in `patches/`, which `scripts/check-patches.sh` applies to that pin
on every run of the test suites. The **applications** are not built here: they belong to the
desktop, which builds and tests them against its real compositor (SerenityDE is the reference;
see [limitations.md](limitations.md) for what it provides and [testing.md](testing.md) for the
suites).

## Milestones

- **M0** — architecture/design + audit: done (see [DESIGN.md](../DESIGN.md)).
- **M1** — full `LibGfx` + **full** `LibGUI` host build: done. `liblagom-gui.so`
  (132 objects, 3063 GUI symbols) builds from unmodified SerenityOS source via small
  Lagom/LibCore patches.
- **M2** — LibWM vertical slice: done. An unmodified-style LibGUI window renders through the
  in-process WindowServer protocol and appears as a real Wayland toplevel. Screen geometry is
  taken from `wl_output`.
- **M3** — input: done. Pointer/keyboard come from `wl_seat` (xkbcommon → Serenity
  `KeyCode`/modifiers), focus/activation/close are handled, and the full input path is verified
  headlessly (the compositor injects clicks and keys; the widget receives them). Held keys repeat
  client-side from `wl_keyboard.repeat_info`, as Wayland intends, so repeat works the same under
  every compositor and does not double with toolkits that already repeat. **Touch** (`wl_touch`)
  is mapped to the primary pointer (LibGUI has no touch event type), so the first finger taps and
  drags exactly like the mouse. Applications (the desktop's Calculator among them) use this path
  unchanged.
- **M4** — theme, fonts, decorations, icons, alpha: done. The system theme and fonts come from the
  bundled `Base/res` or the desktop's broker; windows get server-side decorations
  (`zxdg_decoration_manager_v1`) or LibWM's own 22px client-side titlebar with
  close/maximize/minimize; icons use `xdg-toplevel-icon-v1`, with `serenity-window` as the
  fallback for older compositors; windows can have per-pixel alpha with a click-through hit
  threshold; pointers show the SerenityOS cursor theme.
- **M5** — PDF rendering: done. LibWM's own PDF test app opens a PDF through the in-process
  **FileSystemAccess** portal and renders it with LibPDF into a LibGUI window; zooming, wheel
  scrolling and a compositor resize are asserted from the app's traces
  (`scripts/run-pdf-test.sh`). The desktop's PDFViewer uses the same library and portal path and
  is tested by the desktop.
- **M6** — clipboard and menus: done for `text/plain`, `text/html`, `text/uri-list`, and
  `image/png` ↔ `image/x-serenityos`, including multiple representations on one clipboard item
  (all verified headlessly in both directions), plus a server-rendered **menubar and dropdown
  menus** with hover highlighting (clicking `File → Quit` works; `Help → About` opens the dialog),
  **nested submenus**, **menu item icons**, **long-menu scrolling**, and full keyboard navigation
  (`F10`/`Alt`+accelerator, arrows, `Enter`, `Escape`). Window state
  (`fullscreen`/`maximize`/`minimize`) is in, including **interactive resize**: dragging a
  statusbar `ResizeCorner` becomes `xdg_toplevel.resize`, so the compositor takes the pointer and
  drives the resize (`RESIZING` configures stretch the last frame while the client re-renders).
  Config INI persistence and SerenityOS `/res/...` path redirection are in too. The menubar is
  rendered on the LibWM server thread and handed to the Wayland side as an image, so it is never
  drawn from under the menu model.
- **M7** — testing and CI: done. Thirteen CTest suites (input, touch and drag-and-drop, window
  state and portals, cursor warping over `wp_pointer_warp_v1`, global mouse tracking over
  `ext-input-observation-v1` inside and outside the window, `xdg-activation-v1` with both a
  serial and `XDG_ACTIVATION_TOKEN`, clipboard and primary selection, menus, PDF, **client-side
  GL** and an animated 3D GL app, the desktop appearance portal, patch series, triage tooling,
  upstream LibGfx) run against a headless Wayland compositor with scripted input and frame
  capture. The framework tests itself with the programs in `src/test/`;
  the applications, which the desktop builds, are tested against the desktop's compositor
  (SerenityDE: `scripts/run-libgui-apps-test.sh`).
  `.github/workflows/ci.yml` builds and runs the suites in an Arch container on every push, with
  a compiler cache and log artifacts on failure.
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
  WindowServer protocol carries no GL messages). The framework's GL test programs cover the
  stack; the desktop's `3DFileViewer` and `Tubes` use the same path.
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
- **M14** — window activation: done. LibWM binds `xdg-activation-v1`, uses a launcher's
  `XDG_ACTIVATION_TOKEN` for the first window, and otherwise requests a token with the last input
  serial when a window is opened from a click or menu; the compositor then raises and focuses it.
  Tested against the headless compositor's activation implementation and SerenityDE's compositor.
- **M15** — screen capture: done. `get_screen_bitmap`, the Magnifier's around-cursor and
  around-location calls and the colour picker take the compositor's direct capture when
  `serenity-window` provides it, and otherwise the standard
  `org.freedesktop.portal.Screenshot`. The portal image is in output pixels; a requested
  logical region is scaled and cropped onto it, and the Magnifier's per-frame asks reuse a
  recent capture. Both paths are covered by the headless suites and by a test against the
  SerenityDE compositor (including a capture of a static, non-repainting window).

## Remaining work

No known work items. Platform limits, non-goals and the features a desktop provides are in
[limitations.md](limitations.md).
