# Environment variables

Read at run time by LibWM (all lookups live in `src/Environment.cpp`):

| Variable | Default | Effect |
|---|---|---|
| `LIBWM_RES` | — | Resource tree (fonts, themes, icons, cursors); wins over `SERENITY_RES_ROOT` |
| `SERENITY_RES_ROOT` | build tree's `serenity/Base/res` | Resource tree; LibWM sets it if unset so `/res/...` paths resolve |
| `LIBWM_CONFIG_DIR` | `$XDG_CONFIG_HOME/libgui-wayland` or `~/.config/libgui-wayland` | Where the in-process ConfigServer persists `<domain>.ini` |
| `LIBWM_APP_BIN_DIR` | directory of the running executable | Where service and application binaries are spawned from |
| `LIBWM_CLIENT_SCALE` | follow the compositor | Force a HiDPI scale (`2`, `1.5`, ...); `0` forces 1x |
| `LIBWM_FORCE_CSD` | off | Draw client-side titlebars even if server-side decorations are offered |
| `LIBWM_TRACE` | off | Log protocol traffic and lifecycle events (every frame, pointer event, configure, portal connection); the test suites set it |
| `LIBWM_DUMP` | — | Write each presented window to this PNG path (debugging) |
| `LIBWM_AXIS_DEBUG` | off | Log raw scroll-axis events |
| `WAYLAND_DISPLAY` | session | Target compositor |

Read by the patched SerenityOS libraries:

| Variable | Default | Effect |
|---|---|---|
| `LIBWM_SESSION_DIR` | `/tmp/session/<sid>` | Per-session runtime directory, where session services' portal sockets live (LibCore) |
| `LIBWM_WM_PORTAL_PATH` | `/tmp/portal/wm` | The WindowManagerServer socket the Taskbar connects to (LibCore) |
| `LIBWM_SESSION_BIN_DIR` | — | Searched first when an application runs a SerenityOS `/bin/X`, so a session can provide its own helpers (LibCore) |
| `LIBWM_APP_BIN_DIR` | — | Searched next for `/bin/X` (LibCore) |
| `LIBWM_DEBUG_QUIT` | off | Print a backtrace when an application quits (LibGUI) |

Read by the scripts: `SERENITY_SRC` (`serenity/`), `BUILD_DIR` (`build/lagom`), `HC` (headless
compositor binary), `XLIB_WAYLAND_SRC`/`XLIB_WAYLAND_BUILD`, `ENABLE_LAGOM_CCACHE` (`OFF`),
`KEEP=1` (keep each suite's runtime directory), `RUN_TESTS`.
