# Building GUI Applications for Wayland

A hands-on introduction to writing desktop applications with SerenityOS's
**LibGUI** toolkit and running them as native Wayland clients on any compositor.

- **Read the guide:** [LibGUI-Wayland-Guide.pdf](LibGUI-Wayland-Guide.pdf)
- **Editable source:** [guide.md](guide.md)
- **Runnable examples:** [../../examples](../../examples)

The guide is application-focused. It does not require the Serenity Desktop
Environment, a WindowServer service, X11, or XWayland. It uses only LibGUI's
public API: windows, widgets, layouts, painting, models, views, actions, menus,
the clipboard, and file dialogs.

## Building the PDF

The PDF is generated from `guide.md` with ReportLab and Pygments:

```bash
python3 -m pip install reportlab pygments pillow
python3 build_pdf.py
```

The images in `images/` are generated from the four example applications and
the architecture diagram script. To rebuild the architecture diagram:

```bash
cd images && python3 make_diagrams.py
```

## Building the examples

```bash
cd ../../examples
make
SERENITY_RES_ROOT="$PWD/../serenity/Base/res" ./bin/notes
```

See chapter 2 of the guide for prerequisites and troubleshooting.
