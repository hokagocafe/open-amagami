"""Draws a splash screen: a mascot in the middle, a few lines of text and a link, as a PNG.

python -m hokago make_splash MASCOT.svg OUT.png [--font FILE.ttf] [--lines FILE.txt] [--width 960 --height 544]

MASCOT.svg is the picture (its own viewBox is kept). The text file has one line per row of text; the last
one is the link (drawn in the accent colour), the ones before it are the message. Without --lines the built-in
English text is used. The size defaults to the Vita's 960x544. resvg draws the background and the mascot, Pillow the text
(resvg does not take a font file with its system fonts switched off).
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FONT = os.path.join(HERE, "..", "..", "fonts", "MPLUSRounded1c-ExtraBold.ttf")
DEFAULT_LINES = [
    "A non-profit fan project.",
    "Not affiliated with the game's makers.",
    "hokagocafe.com",
]

BACKGROUND = "#f7f6f4"
INK = "#28233e"
ACCENT = "#4b41c2"
MUTED = "#5d5870"


def layout(height):
    """The picture's box and the first baseline, from the height."""
    picture_h = height * 0.56
    return {"picture_h": picture_h, "py": height * 0.06, "text_y": height * 0.06 + picture_h + height * 0.09}


def compose(mascot_svg, width, height):
    """The background and the mascot as SVG text."""
    view = re.search(r'viewBox="([^"]+)"', mascot_svg)
    if not view:
        raise SystemExit("the mascot has no viewBox")
    vx, vy, vw, vh = (float(v) for v in view.group(1).split())
    inner = re.sub(r"^.*?<svg[^>]*>", "", mascot_svg, count=1, flags=re.S)
    inner = re.sub(r"</svg>\s*$", "", inner)
    place = layout(height)
    picture_h = place["picture_h"]
    picture_w = picture_h * vw / vh
    px = (width - picture_w) / 2
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        f'  <rect width="{width}" height="{height}" fill="{BACKGROUND}"/>',
        f'  <svg x="{px:.1f}" y="{place["py"]:.1f}" width="{picture_w:.1f}" height="{picture_h:.1f}" viewBox="{vx} {vy} {vw} {vh}">{inner}</svg>',
        "</svg>",
    ]
    return chr(10).join(parts)


def draw_text(image, lines, font_path, width, height):
    from PIL import ImageDraw, ImageFont
    draw = ImageDraw.Draw(image)
    rest = list(lines)
    link = rest.pop() if rest else ""

    def row(text, size, colour, y):
        font = ImageFont.truetype(font_path, int(size))
        draw.text((width / 2, y), text, font=font, fill=colour, anchor="ms")

    y = layout(height)["text_y"]
    for line in rest:
        row(line, height * 0.05, INK, y)
        y += height * 0.07
    if link:
        row(link, height * 0.058, ACCENT, y + height * 0.015)


def main():
    args = sys.argv[1:]
    options = {"--font": DEFAULT_FONT, "--lines": None, "--width": "960", "--height": "544"}
    for key in list(options):
        if key in args:
            i = args.index(key)
            options[key] = args[i + 1]
            del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    import resvg_py
    from PIL import Image
    with open(args[0], encoding="utf-8") as f:
        mascot = f.read()
    if options["--lines"]:
        with open(options["--lines"], encoding="utf-8") as f:
            lines = [line.rstrip() for line in f if line.strip()]
    else:
        lines = DEFAULT_LINES
    width, height = int(options["--width"]), int(options["--height"])
    png = bytes(resvg_py.svg_to_bytes(svg_string=compose(mascot, width, height)))
    image = Image.open(io.BytesIO(png)).convert("RGB")
    draw_text(image, lines, os.path.abspath(options["--font"]), width, height)
    image.save(args[1])
    print(f"{args[1]}: {os.path.getsize(args[1])} bytes")


if __name__ == "__main__":
    main()
