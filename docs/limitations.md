# Limitations and non-goals

Things that are not bugs in LibWM, or not work the framework intends to do. For the gaps the
framework still has, see [status.md](status.md).

## Platform limits

- **GPU presentation and shaders.** `EGLGPU` renders on the GPU but reads the result back with
  `glReadPixels` into a `Gfx::Bitmap` and presents over `wl_shm`; zero-copy
  `zwp_linux_dmabuf_v1` is not implemented, and a compositor cannot render GL for a client.
  `EGLGPU` also cannot run shader programs (`create_shader` returns an error), so programs that
  need GLSL must use `LibSoftGPU`. See [development-history.md](development-history.md) §4.11.
- **WindowServer requests Wayland cannot fully express.** All of them are handled explicitly
  rather than silently dropped: `refresh_system_theme` reloads the theme, base size becomes the
  window's minimum size, `set_global_cursor_position` goes through `wp_pointer_warp_v1` (the
  staging protocol KDE co-authored and SerenityDE implements), `set_global_mouse_tracking`
  observes each toplevel with `ext-input-observation-v1` (a draft, wayland-protocols MR !335,
  implemented by SerenityDE and wlmaker), and `show_screen_numbers` is forwarded to the desktop as
  `effects.show_screen_numbers` (the compositor owns output overlays). Size increments, aspect
  ratios, forced shadows and window stealing have **no** Wayland protocol: `xdg-shell` has no
  increments or aspect constraints, a shadow is drawn outside the surface, and a client owns its
  surfaces while the compositor owns stacking. The per-request rationale is in
  [protocol-mapping.md](protocol-mapping.md). `is_minimized` remains LibWM's own record, set by
  `set_minimized` and cleared when the compositor activates the window.
- **Pointer lock and idle inhibit.** `pointer-constraints`/`relative-pointer` (for mouse-look
  games) and idle inhibition (for video playback) are not used by LibWM, and LibGUI has no
  application API for them; a future application API would bind them.
- **No client protocol for some shell duties.** Screen-number overlays (`show_screen_numbers`)
  and shadows around a surface are compositor features. The shim forwards the screen-number
  request as the `effects.show_screen_numbers` host setting; the desktop draws it.

## Provided by the desktop

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
- **Desktop chrome and session services.** The Taskbar, Desktop, applets, window menus and the
  session's LaunchServer/NotificationServer/AudioServer come from the desktop. **The
  applications themselves are built and tested by the desktop too**; this repository ships the
  framework and its own test programs, not the applications or their assets. (The programs in
  `examples/` are the guide's example applications, documentation for the public API, not the
  SerenityOS applications.)
- **Focus policy.** SerenityDE decides which window is raised and focused when an application
  launches or a workspace changes. Elsewhere the compositor's own policy applies; see
  [status.md](status.md) for the `xdg-activation-v1` gap.

## Upstream or out of scope

- **Audio resampling.** Cross-rate clients use LibAudio's insert/drop resampler (`Resampler.h`
  says "Should do better..."). That is an upstream LibAudio limitation, and mixing is the
  desktop's `AudioServer`.
- **Zero-copy GPU buffers and Vulkan** remain design non-goals ([DESIGN.md](../DESIGN.md) §1.2).
