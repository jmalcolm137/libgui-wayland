# Desktops

libgui-wayland is desktop-agnostic: the applications run on any Wayland compositor.
What a desktop adds is its own business, built on the same pinned SerenityOS source:

* **Session services.** A service listening on a portal path (LaunchServer,
  NotificationServer, AudioServer, ...) may take over from LibWM's in-process fallback;
  [environment.md](environment.md) lists the variables that place them.
* **Desktop-owned settings and chrome.** Pointer, keyboard, cursor, theme, fonts, display
  layout and wallpaper settings, and compositor-drawn window chrome, travel over the
  optional [`serenity-window`](../src/protocol/serenity-window.xml) protocol, which this
  repository owns. A compositor that implements it keeps a copy; without it,
  applications run with defaults.
* **Requests with no client protocol.** Screen-number overlays (`show_screen_numbers`)
  and shadows around a surface are compositor features; the shim forwards the
  screen-number request as the `effects.show_screen_numbers` host setting, and the
  desktop is the right place to draw it. Pointer warping and global mouse tracking have
  protocols (`wp_pointer_warp_v1`, `ext-input-observation-v1`), implemented by
  SerenityDE, and the shim uses both.
* **Activation.** `xdg-activation-v1` raises and focuses a window: LibWM uses the last input
  serial for a window opened from a click or menu, and honours `XDG_ACTIVATION_TOKEN` from a
  launcher. SerenityDE's compositor also focuses newly mapped windows itself, so a launch is
  raised even without a token.
* **Standard protocols the desktop only has to advertise.** Window icons
  (`xdg-toplevel-icon-v1`), primary selection (`zwp_primary_selection_v1`), the
  appearance preference (`org.freedesktop.portal.Settings`, via `xdg-desktop-portal`)
  and the usual `xdg-shell`/`wl_data_device`/`wl_seat` surfaces mean a desktop's
  taskbar, window switcher, middle-click paste and light/dark mode work with Serenity
  applications exactly as with native ones. SerenityDE's compositor turns the standard
  icon into its Taskbar button and sends its theme directly (which takes precedence);
  `serenity-window`'s icon event is only used where `xdg-toplevel-icon-v1` is absent, and
  the portal only where no theme is sent at all.

The [Serenity Desktop Environment](https://github.com/jmalcolm137/serenity-desktop-environment)
is the reference desktop and session: its compositor, Taskbar/Desktop/applets and session
services supply what [limitations.md](limitations.md) lists as desktop-provided
(screenshots, theme/input/display settings, wallpaper and applet layout), and they consume
the standard protocols above.
