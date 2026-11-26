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
| `wl_seat` | 1 | pointer and keyboard | no input |
| `wl_output` | ≤ 2 | screen size and integer scale | default size, scale 1 |
| `zxdg_output_manager_v1` | ≤ 3 | logical size, hence fractional scale | integer scale only |
| `wp_fractional_scale_manager_v1` | 1 | per-surface preferred scale | scale inferred from outputs |
| `wp_viewporter` | 1 | fractional presentation, interactive-resize stretch | integer scale only |
| `zxdg_decoration_manager_v1` | 1 | server-side decorations | client-side titlebar |
| `wl_data_device_manager` | ≤ 3 | clipboard and drag-and-drop | no clipboard or DnD |
| `zwp_text_input_manager_v3` | 1 | input methods (preedit, commit) | no IME |
| `zwlr_layer_shell_v1` | ≤ 4 | Taskbar, Desktop, applets, popup-style windows, menus of layer surfaces | those become ordinary toplevels |
| `serenity_window_manager` (`src/protocol/serenity-window.xml`) | ≤ 4 | a desktop's compositor: host settings, wallpaper, capture, menubar control, placement, icons | defaults; no compositor-driven chrome |

## WindowServer requests (client → LibWM)

`WindowServerConnection` (`src/WindowServerConnection.cpp`,
`src/WindowServerSettings.cpp`) implements 97 of the 109 requests in
`WindowServer.ipc`. The rest fall through to the generated
`WindowServerDefaultStub` (`tools/gen-default-stub.py`), which ignores
asynchronous requests and answers synchronous ones with a valid default
(false, zero, empty). It must never answer with an invalid response: LibIPC
then sends no reply, and the client's `send_sync()` blocks forever.

| Group | Implemented | Notes |
|---|---|---|
| Window lifecycle | `create_window`, `destroy_window`, `set_window_title`, `get_window_title`, `set_window_rect`, `get_window_rect`, `get_window_floating_rect`, `get_window_rect_from_client`, `set_window_minimum_size`, `get_window_minimum_size`, `move_window_to_front`, `set_always_on_top`, `start_window_resize` | the compositor owns placement; layer-surface sizes follow `set_window_rect`; a resize request becomes `xdg_toplevel.resize` |
| Window state | `set_fullscreen`, `set_maximized`, `is_maximized`, `set_minimized`, `is_minimized`, `set_window_parent_from_client`, `set_frameless`, `set_window_modified`, `is_window_modified`, `set_window_has_alpha_channel`, `set_window_alpha_hit_threshold`, `set_window_icon_bitmap`, `set_window_progress` | progress is accepted and ignored |
| Painting | `set_window_backing_store`, `invalidate_rect`, `did_finish_painting`, `enable_display_link`, `disable_display_link` | frame-callback paced |
| Cursor | `set_window_cursor`, `set_window_custom_cursor`, `get_global_cursor_position` | position reconstructed from the surface under the pointer |
| Menus | `create_menu`, `destroy_menu`, `set_menu_name`, `set_menu_minimum_width`, `add_menu`, `add_menu_item`, `add_menu_separator`, `update_menu_item`, `remove_menu_item`, `flash_menubar_menu`, `popup_menu`, `dismiss_menu` | rendered by `MenuController` |
| Drag and drop | `start_drag`, `set_accepts_drag` | `wl_data_device` |
| Screen and capture | `get_screen_layout`, `set_screen_layout`, `save_screen_layout`, `get_desktop_display_scale`, `set_output_scale`, `get_screen_bitmap`, `get_screen_bitmap_around_cursor`, `get_screen_bitmap_around_location`, `get_color_under_cursor`, `get_applet_rect_on_screen` | layout and capture come from the desktop where available; `get_screen_bitmap` waits up to 2 s for the capture, the Magnifier's calls return the latest one |
| Theme, fonts, effects | `set_system_theme`, `get_system_theme`, `get_system_theme_override`, `is_system_theme_overridden`, `get_preferred_color_scheme`, `set_system_theme_override`, `clear_system_theme_override`, `set_system_fonts`, `set_system_effects` | the override (ThemeEditor's preview) restyles this application only |
| Wallpaper and desktop | `set_wallpaper`, `get_wallpaper`, `set_wallpaper_mode`, `set_background_color`, `apply_workspace_settings`, `get_workspace_settings` | stored by the desktop; `get_wallpaper` reads the `wallpaper.file` it names |
| Input settings | `get/set_mouse_acceleration` (+ min/max), `get/set_scroll_step_size` (+ min), `get/set_double_click_speed` (+ min/max), `are/set_mouse_buttons_switched`, `is/set_natural_scroll`, `get/set_keyboard_layout`, `get_keyboard_layout_names`, `get/apply_cursor_theme`, `get/set_cursor_highlight_color`, `get/set_cursor_highlight_radius` | applied by the desktop's compositor where available |

**Not implemented** (default stub):
`set_window_base_size_and_size_increment`, `set_window_resize_aspect_ratio`,
`set_forced_shadow`, `set_global_cursor_position`, `set_global_mouse_tracking`,
`refresh_system_theme`, `show_screen_numbers`, `set_flash_flush`, `pong`,
`add_window_stealing_for_client`, `remove_window_stealing_for_client`,
`remove_window_stealing`.

All are asynchronous or have an empty reply, which LibIPC sends itself, so none
can block the caller. Most are global window-manager duties that belong to the
compositor (DESIGN.md §1.2); window stealing lets another process's windows
parent ours, which has no meaning when every application is its own Wayland
client.

`set_window_base_size_and_size_increment` and `set_window_resize_aspect_ratio`
would constrain the sizes a resize can produce; both are still defaults.
`patches/libgui/0093` lets an application hide the statusbar resize corner if it
does not want one; the corner is functional without it.

`is_minimized` is LibWM's own record, since `xdg_toplevel` never reports
minimization: set by `set_minimized`, cleared when the compositor activates the
window again.

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
