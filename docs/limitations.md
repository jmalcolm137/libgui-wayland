# Limitations and non-goals

Things that are not bugs in LibWM, or not work the framework intends to do. For the gaps the
framework still has, see [status.md](status.md).

## Platform limits

- **GPU presentation and shaders.** `EGLGPU` renders on the GPU but reads the result back with
  `glReadPixels` into a `Gfx::Bitmap` and presents over `wl_shm`; zero-copy
  `zwp_linux_dmabuf_v1` is not implemented, and a compositor cannot render GL for a client.
  `EGLGPU` also cannot run shader programs (`create_shader` returns an error), so programs that
  need GLSL must use `LibSoftGPU`. See [development-history.md](development-history.md) §4.11.
- **WindowServer requests Wayland cannot express.** Size increments, aspect ratios, forced shadows
  and window stealing have **no** Wayland protocol: `xdg-shell` has no increments or aspect
  constraints, a shadow is drawn outside the surface, and a client owns its surfaces while the
  compositor owns stacking. `show_screen_numbers` has no client protocol either; it is a
  compositor overlay. The per-request rationale is in [protocol-mapping.md](protocol-mapping.md).
- **Pointer lock and idle inhibit.** `pointer-constraints`/`relative-pointer` (for mouse-look
  games) and idle inhibition (for video playback) have no LibGUI application API, so LibWM does
  not bind them.

## Provided by the desktop

The SerenityDE session supplies these over `serenity-window` or in its compositor. They are not
gaps in that session, but outside it an application depends on whatever portable protocol or
portal the compositor implements (if any):

- **Screenshots and the Magnifier.** A capture needs pixels only the display server has. A
  compositor can serve `get_screen_bitmap` and the Magnifier's `get_screen_bitmap_around_*`
  directly over `serenity-window` (SerenityDE does). Otherwise LibWM uses the standard
  `org.freedesktop.portal.Screenshot`, which a desktop with `xdg-desktop-portal` and a
  screenshot backend provides, so `shot`, the Magnifier and the colour picker
  (`get_color_under_cursor`) keep working there. LibWM reuses a portal frame for 100 ms, capping
  the Magnifier at about 10 portal captures a second. Only a session with neither a
  `serenity-window` capture nor a screenshot portal has no capture.
- **Fonts, pointer/keyboard and display settings, wallpaper, applet layout.** There is no
  portable API for any of these: the Settings portal exposes appearance only (see
  [desktops.md](desktops.md)), and input/display configuration is compositor-specific (KDE's
  KScreen, GNOME's Mutter settings, wlroots' output-management protocol). They arrive as
  `serenity-window` host settings in a SerenityDE session; elsewhere the bundled fonts, default
  input behaviour and compositor-owned layout apply. Wallpaper and applet layout are desktop
  features by design.
- **Desktop chrome and session services.** The Taskbar, Desktop, applets, window menus and the
  session's LaunchServer/NotificationServer/AudioServer come from the desktop. **The
  applications themselves are built and tested by the desktop too**; this repository ships the
  framework and its own test programs, not the applications or their assets. (The programs in
  `examples/` are the guide's example applications, documentation for the public API, not the
  SerenityOS applications.)
- **Focus policy.** Which window is raised and focused when an application launches or a
  workspace changes is the desktop's decision. LibWM now requests activation over
  `xdg-activation-v1` (the last input serial, or a launcher's `XDG_ACTIVATION_TOKEN`; see
  [desktops.md](desktops.md)), so a launch can be raised on compositors that honour tokens;
  SerenityDE's own focus policy still applies on top of that.

## Upstream or out of scope

- **Audio resampling.** Cross-rate clients use LibAudio's insert/drop resampler (`Resampler.h`
  says "Should do better..."). That is an upstream LibAudio limitation, and mixing is the
  desktop's `AudioServer`.
- **Zero-copy GPU buffers and Vulkan** remain design non-goals ([DESIGN.md](../DESIGN.md) §1.2).
