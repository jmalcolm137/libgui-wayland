# libgui-wayland

Run **unmodified SerenityOS `LibGUI`/`LibGfx` applications** (e.g. Calculator, PDFViewer)
natively on **Wayland** — no SerenityOS WindowServer, no X server, and no XWayland. LibWM
links none of `xlib-wayland`; we reuse only that project's pure-Wayland headless compositor as
a test harness.

The shim is called **LibWM**. It sits between `LibGUI`'s WindowServer IPC protocol and
`libwayland-client`: the application side is Serenity, the system side is Wayland.

* **Philosophy:** keep `LibGfx`, the full `LibGUI`, and the applications byte-for-byte
  unmodified; implement the WindowServer's server side on top of Wayland.
* **Process model:** in-process — each application is its own Wayland client.
* **Build:** SerenityOS's Lagom host build, extended to compile the **full** LibGUI (not the
  six-file Lagom stub).

## Principles

1. **The compositor is the source of truth.** Query the real Wayland compositor for outputs,
   scale, seat, clipboard and window state, and share client state back — never hardcode
   environment values.
2. **Use AK and SerenityOS conventions.** LibWM is Serenity userland on Linux: AK types,
   `ErrorOr`/`TRY`, `dbgln`, `C_OBJECT`, Serenity style — no `std::` or third-party
   abstractions where AK suffices.

## Status

- **M0** — architecture/design + audit: done (see [DESIGN.md](DESIGN.md)).
- **M1** — full `LibGfx` + **full** `LibGUI` host build: done. `liblagom-gui.so`
  (132 objects, 3063 GUI symbols) builds from unmodified SerenityOS source via small
  Lagom/LibCore patches.
- **M2** — LibWM vertical slice: done. An unmodified-style LibGUI window renders through the
  in-process WindowServer protocol and appears as a real Wayland toplevel. Screen geometry is
  taken from `wl_output` (reported 1280×800 logical @ scale 2 here).
- **M3** — input + Calculator: done. Pointer/keyboard come from `wl_seat` (xkbcommon →
  Serenity `KeyCode`/modifiers), focus/activation/close handled, and the full input path is
  verified headlessly (the compositor injects a click; the widget receives it). The
  **unmodified Calculator** builds and runs, decorated and movable, on KDE Plasma.
- **M6 (partly)** — clipboard and menus: done for `text/plain`, `text/uri-list`, and
  `image/png` ↔ `image/x-serenityos` (all verified headlessly in both directions), plus a
  server-rendered **menubar and dropdown menus** with hover highlighting (clicking `File → Quit`
  works; `Help → About` opens the dialog). Window state (`fullscreen`/`maximize`/`minimize`)
  and SerenityOS `/res/...` path redirection are in. Submenus, keyboard menu navigation and
  config persistence remain.

```sh
scripts/fetch-serenity.sh              # pinned, blobless, sparse checkout
scripts/fetch-headless-compositor.sh   # pinned clone of xlib-wayland (pure-Wayland compositor)
scripts/build-libgui.sh                # patches + LibGfx/LibGUI/LibWM/Calculator + all tests
```

`build-libgui.sh` runs the upstream LibGfx test suite and the headless input integration test
automatically; pass `--no-tests` to skip.

Next: M4 — theme/font parity, decorations, icons, alpha.

