# Protocol mapping

What LibWM speaks on each side: the Wayland protocols it binds, and how each
SerenityOS WindowServer request is handled.

## Wayland protocols

Bound in `registry_global` (`src/WaylandClient.cpp`). Only the first three are
required; without the others LibWM falls back as noted.

| Global | Version | Used for | Without it |
|---|---|---|---|
| `wl_compositor` | ≤ 4 | surfaces, regions | required |
| `xdg_wm_base` | 1 | toplevels and popups | required |
| `wl_shm` | 1 | buffers (zero-copy over the client's memfd) | required |
| `wl_subcompositor` | 1 | menubar subsurface | menubar composed into the window buffer |
| `wl_seat` | ≤ 4 | pointer, keyboard, touch, and the repeat rate (`repeat_info`, since v4) | no input, and no key repeat |
| `wl_output` | ≤ 2 | screen size and integer scale | default size, scale 1 |
| `zxdg_output_manager_v1` | ≤ 3 | logical size, hence fractional scale | integer scale only |
| `wp_fractional_scale_manager_v1` | 1 | per-surface preferred scale | scale inferred from outputs |
| `wp_viewporter` | 1 | fractional presentation, interactive-resize stretch | integer scale only |
| `zxdg_decoration_manager_v1` | 1 | server-side decorations | client-side titlebar |
| `wl_data_device_manager` | ≤ 3 | clipboard and drag-and-drop | no clipboard or DnD |
| `zwp_primary_selection_device_manager_v1` | 1 | the middle-click selection, bridged both ways with the clipboard | no primary selection; clipboard only. wlroots only delivers it to the focused client, and validates `set_selection` serials, so a paste (focused) works and background polling does not |
| `xdg_toplevel_icon_manager_v1` | 1 | the standard window icon (taskbars, switchers) | icons only through `serenity-window`, if present |
| `wp_pointer_warp_v1` | 1 | `set_global_cursor_position` (the Taskbar puts the cursor back on itself) | the request is traced and dropped; the pointer does not move |
| `xdg_activation_v1` | 1 | raise/focus a window (the last input serial, or `XDG_ACTIVATION_TOKEN` from a launcher) | windows are shown without asking to be raised; the compositor's own focus policy applies |
| `ext_input_observation_manager_v1` | 1 | `set_global_mouse_tracking` (GUI::MouseTracker, the Eyes/CatDog demos): the pointer position relative to each toplevel, inside or outside it and regardless of focus | tracking is unavailable; the request is traced and nothing is sent. A draft (wayland-protocols MR !335) implemented by SerenityDE and wlmaker, so the XML is vendored in `src/protocol/` |
| `zwp_text_input_manager_v3` | 1 | input methods (preedit, commit) | no IME |
| `zwlr_layer_shell_v1` | ≤ 4 | Taskbar, Desktop, applets, popup-style windows, menus of layer surfaces | those become ordinary toplevels |
| `serenity_window_manager` (`src/protocol/serenity-window.xml`) | ≤ 4 | a desktop's compositor: host settings, wallpaper, capture, menubar control, placement, icons | defaults; no compositor-driven chrome |

Touch (`wl_touch`) has no WindowServer counterpart: LibWM maps the first touch
point to the primary pointer (press, motion, release), so LibGUI's existing mouse
path handles taps and drags. Extra fingers are ignored.

Screen capture is answered off the Wayland path. When the compositor implements
`serenity-window`'s `request_capture`, LibWM asks it and reads the pushed bitmap
of the requested region. Otherwise it uses the standard
`org.freedesktop.portal.Screenshot`: a non-interactive request, cached briefly,
whose PNG is in output pixels; the requested logical region is scaled by the
output factor and cropped out of it. That covers `get_screen_bitmap`, both
Magnifier calls and `get_color_under_cursor`, which samples one pixel from the
same path.

## WindowServer requests (client → LibWM)

`WindowServerConnection` (`src/WindowServerConnection.cpp`,
`src/WindowServerSettings.cpp`) overrides 108 of the 109 requests in
`WindowServer.ipc`; six of those are deliberate no-ops with no Wayland
equivalent, and the rest map as far as the protocol goes (the table below).
Only `pong` still falls through to the generated
`WindowServerDefaultStub` (`tools/gen-default-stub.py`), which ignores
asynchronous requests and answers synchronous ones with a valid default
(false, zero, empty). It must never answer with an invalid response: LibIPC
then sends no reply, and the client's `send_sync()` blocks forever.

| Group | Implemented | Notes |
|---|---|---|
| Window lifecycle | `create_window`, `destroy_window`, `set_window_title`, `get_window_title`, `set_window_rect`, `get_window_rect`, `get_window_floating_rect`, `get_window_rect_from_client`, `set_window_minimum_size`, `get_window_minimum_size`, `move_window_to_front`, `set_always_on_top`, `start_window_resize` | the compositor owns placement; layer-surface sizes follow `set_window_rect`; a resize request becomes `xdg_toplevel.resize` |
| Window state | `set_fullscreen`, `set_maximized`, `is_maximized`, `set_minimized`, `is_minimized`, `set_window_parent_from_client`, `set_frameless`, `set_window_modified`, `is_window_modified`, `set_window_has_alpha_channel`, `set_window_alpha_hit_threshold`, `set_window_icon_bitmap`, `set_window_progress` | progress is accepted and ignored |
| Painting | `set_window_backing_store`, `invalidate_rect`, `did_finish_painting`, `enable_display_link`, `disable_display_link` | frame-callback paced |
| Cursor | `set_window_cursor`, `set_window_custom_cursor`, `get_global_cursor_position`, `set_global_cursor_position`, `set_global_mouse_tracking` | position reconstructed from the surface under the pointer; warps go through `wp_pointer_warp_v1`; tracking observes each toplevel with `ext-input-observation-v1` |
| Menus | `create_menu`, `destroy_menu`, `set_menu_name`, `set_menu_minimum_width`, `add_menu`, `add_menu_item`, `add_menu_separator`, `update_menu_item`, `remove_menu_item`, `flash_menubar_menu`, `popup_menu`, `dismiss_menu` | rendered by `MenuController` |
| Drag and drop | `start_drag`, `set_accepts_drag` | `wl_data_device` |
| Screen and capture | `get_screen_layout`, `set_screen_layout`, `save_screen_layout`, `get_desktop_display_scale`, `set_output_scale`, `get_screen_bitmap`, `get_screen_bitmap_around_cursor`, `get_screen_bitmap_around_location`, `get_color_under_cursor`, `get_applet_rect_on_screen` | layout comes from the desktop where available; capture is direct (`serenity-window`) or the `org.freedesktop.portal.Screenshot` fallback. `get_screen_bitmap` waits up to 2 s for its capture, the Magnifier's calls return the latest one |
| Theme, fonts, effects | `set_system_theme`, `get_system_theme`, `get_system_theme_override`, `is_system_theme_overridden`, `get_preferred_color_scheme`, `set_system_theme_override`, `clear_system_theme_override`, `set_system_fonts`, `set_system_effects` | the override (ThemeEditor's preview) restyles this application only |
| Wallpaper and desktop | `set_wallpaper`, `get_wallpaper`, `set_wallpaper_mode`, `set_background_color`, `apply_workspace_settings`, `get_workspace_settings` | stored by the desktop; `get_wallpaper` reads the `wallpaper.file` it names |
| Input settings | `get/set_mouse_acceleration` (+ min/max), `get/set_scroll_step_size` (+ min), `get/set_double_click_speed` (+ min/max), `are/set_mouse_buttons_switched`, `is/set_natural_scroll`, `get/set_keyboard_layout`, `get_keyboard_layout_names`, `get/apply_cursor_theme`, `get/set_cursor_highlight_color`, `get/set_cursor_highlight_radius` | applied by the desktop's compositor where available |

**Handled as far as Wayland can, otherwise deliberately ignored.** Each of these
is implemented on the LibWM side (so the caller never blocks), but several have
no Wayland equivalent; the table says why, and which protocol would be needed if
one existed.

| Request | What LibWM does | Wayland equivalent |
|---|---|---|
| `set_window_base_size_and_size_increment` | base size becomes the window's minimum size (via `xdg_toplevel.set_min_size`); the increment is recorded | **none**: `xdg_toplevel` has min/max size but no X11 resize increments, so a compositor cannot snap to cell-sized steps |
| `set_window_resize_aspect_ratio` | ignored (traced) | **none**: no aspect-ratio constraint in `xdg-shell`; the client keeps its own aspect |
| `set_forced_shadow` | ignored (traced) | **none**: a shadow is drawn outside the surface; `xdg-decoration` exposes only SSD/CSD, and server-side decorations bring the compositor's own shadow |
| `set_global_cursor_position` | maps the output position onto one of this client's surfaces and calls `wp_pointer_warp_v1.warp_pointer` | **`wp_pointer_warp_v1`** (staging, co-authored by KDE and implemented by SerenityDE). It only moves the pointer within the client's own surfaces and validates the `wl_pointer.enter` serial; a position on another client's window cannot be requested, by design |
| `refresh_system_theme` | reloads the theme in effect (host settings, then the Settings portal, then bundled) and notifies clients | n/a (local) |
| `show_screen_numbers` | forwarded to the desktop as `effects.show_screen_numbers` | **compositor-owned**: it owns the outputs and can draw an identification overlay; there is no client protocol for it (SerenityDE's compositor is the right place) |
| `set_flash_flush` | no-op | n/a: LibWM flushes every commit, and frame callbacks pace presentation |
| `add/remove_window_stealing*` | no-op | **none**: a client owns its surfaces and the compositor owns stacking, so one application cannot parent another's windows. Legitimate transients within a client use `set_window_parent_from_client` (`xdg_toplevel.set_parent`) |

`ext-input-observation-v1` is a draft (wayland-protocols MR !335, Google 2024),
so the XML is vendored in `src/protocol/` and duplicated in SerenityDE. Positions
are signed 16:16 fixed point, as the draft specifies; wlmaker's implementation
divides by 256 instead (24:8) despite carrying the same text, so the shim is not
yet wire-compatible with wlmaker's clients. The observer is per toplevel; the
relative position is turned into output coordinates with the origin the
compositor reports (`serenity_toplevel.position`), and `instance` is accepted but
currently always 0.

`pong` remains a default stub (the connection is in-process; nothing pings it).
`is_minimized` is LibWM's own record, since `xdg_toplevel` never reports
minimization: set by `set_minimized`, cleared when the compositor activates the
window again.

`patches/libgui/0093` lets an application hide the statusbar resize corner if it
does not want one; the corner is functional without it.

## WindowClient events (LibWM → client)

Sent from the server thread: `fast_greet`, `paint`, `mouse_move`, `mouse_down`,
`mouse_up`, `mouse_double_click` (synthesised), `mouse_wheel`, `key_down`,
`key_up`, `window_entered`, `window_left`, `window_activated`,
`window_deactivated`, `window_close_request`, `window_resized`,
`menu_item_activated`, `menu_item_entered`, `menu_item_left`,
`menu_visibility_did_change`, `drag_accepted`, `drag_cancelled`, `drag_moved`,
`drag_dropped`, `display_link_notification`, `update_system_theme`,
`update_system_fonts`, `update_system_effects`. (`screen_rects_changed` is not
sent: screen geometry is delivered once, in `fast_greet`.)
