#!/usr/bin/env python3
"""Generate the diagrams used by the guide (Pillow, at 2x for print)."""

from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 1600, 900
S = 2  # supersampling

FONT = "/usr/share/fonts/noto/NotoSans-Regular.ttf"
FONT_MED = "/usr/share/fonts/noto/NotoSans-Medium.ttf"
FONT_BOLD = "/usr/share/fonts/noto/NotoSans-Bold.ttf"


def font(path, size):
    return ImageFont.truetype(path, size * S)


def rounded(draw, box, radius, fill, outline=None, width=1):
    scaled = tuple(v * S for v in box)
    draw.rounded_rectangle(scaled, radius=radius * S, fill=fill, outline=outline, width=width * S)


def text(draw, xy, s, f, fill):
    draw.text((xy[0] * S, xy[1] * S), s, font=f, fill=fill)


def main():
    img = Image.new("RGB", (W * S, H * S), "white")
    d = ImageDraw.Draw(img)

    shadow = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)

    layers = [
        dict(tag="APPLICATION", title="Your application",
             detail="serenity_main()  ·  Window  ·  Widget  ·  Model  ·  GML",
             fill="#e8f0f7", edge="#a9c1d5", ink="#183b56", tagink="#5b7a94"),
        dict(tag="TOOLKIT", title="LibGUI  ·  LibGfx  ·  AK",
             detail="widgets  ·  layouts  ·  painting  ·  menus  ·  models",
             fill="#eef4f8", edge="#b7c9d8", ink="#183b56", tagink="#5b7a94"),
        dict(tag="WAYLAND", title="libwayland-client",
             detail="xdg-shell  ·  wl_shm  ·  wl_seat  ·  wl_data_device  ·  xdg-decoration",
             fill="#e9f5f1", edge="#9ccfbe", ink="#1d4d3e", tagink="#4d8170"),
        dict(tag="SYSTEM", title="Wayland compositor",
             detail="KDE Plasma  ·  GNOME  ·  Sway  ·  Hyprland  ·  Wayfire  ·  ...",
             fill="#1f4e3d", edge="#1f4e3d", ink="white", tagink="#9fd0bd"),
    ]

    x, w = 60, 1480
    y, h, gap = 55, 150, 36

    for layer in layers:
        box = (x, y, x + w, y + h)
        sd.rounded_rectangle(((x + 6) * S, (y + 10) * S, (x + w + 6) * S, (y + h + 10) * S),
                             radius=22 * S, fill=(20, 35, 50, 34))
        rounded(d, box, 22, layer["fill"], layer["edge"], 2)

        text(d, (x + 34, y + 30), layer["tag"], font(FONT_BOLD, 19), layer["tagink"])
        d.line((x + 250 * S, y + 30 * S, x + 250 * S, y + (h - 30) * S),
               fill=layer["edge"], width=2 * S)

        text(d, (x + 286, y + 22), layer["title"], font(FONT_BOLD, 46), layer["ink"])
        text(d, (x + 286, y + 86), layer["detail"], font(FONT, 27), layer["ink"])

        if layer["tag"] == "TOOLKIT":
            chip = (x + 988, y + 94, x + 1442, y + 136)
            rounded(d, chip, 14, "#ffffff", "#b7c9d8", 2)
            text(d, (chip[0] + 16, chip[1] + 10),
                 "Wayland support: linked in, not a service",
                 font(FONT_MED, 21), "#36546d")

        y += h + gap

    shadow = shadow.filter(ImageFilter.GaussianBlur(10 * S))
    img = Image.alpha_composite(shadow, img.convert("RGBA")).convert("RGB")
    d = ImageDraw.Draw(img)

    for i in range(len(layers) - 1):
        top = 55 + i * (h + gap) + h
        cx = x + w // 2
        d.line((cx * S, (top + 5) * S, cx * S, (top + gap - 5) * S),
               fill="#8b9aa8", width=2 * S)
        d.polygon([((cx - 6) * S, (top + gap - 12) * S),
                   ((cx + 6) * S, (top + gap - 12) * S),
                   (cx * S, (top + gap - 2) * S)], fill="#8b9aa8")
        d.polygon([((cx - 6) * S, (top + 12) * S),
                   ((cx + 6) * S, (top + 12) * S),
                   (cx * S, (top + 2) * S)], fill="#8b9aa8")

    note = ("No WindowServer  ·  no daemon  ·  no X11  ·  one process per application")
    text(d, (x + 6, H - 74), note, font(FONT_MED, 23), "#52616f")
    text(d, (x + 6, H - 40),
         "The compositor owns window placement, focus, decorations and global shortcuts.",
         font(FONT, 21), "#7a8794")

    img = img.resize((W, H), Image.LANCZOS)
    img.save("architecture.png")
    print("wrote architecture.png", img.size)


if __name__ == "__main__":
    main()
