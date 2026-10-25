#!/usr/bin/env python3
"""Draws the port's button glyphs from scratch (F5-9): one SVG per glyph, and a PNG rendered from that same SVG.

    python -m hokago make_button_art OUT_DIR [--font MPLUSRounded1c-ExtraBold.ttf]

Writes OUT_DIR/svg/<family>/<name>.svg, OUT_DIR/png/<family>/<name>.png and OUT_DIR/sheet.png (a contact sheet).

The art is ours: circles, rounded rectangles, paths and gradients. The letters come from an open font (M PLUS Rounded 1c,
SIL Open Font Licence 1.1: https://github.com/google/fonts/tree/main/ofl/mplusrounded1c) and are turned into plain
outlines, so a finished SVG needs no font and looks the same everywhere. The same font is what the port can use for the
labels of the keyboard keys, so that they match. No third-party icon was traced or copied; the proportions follow what
the game's own 28x28 originals measure at 4 times their size (disc as large as the canvas, symbol half of the disc) and
the usual conventions of controller glyphs (dark face with a slight relief, the symbols of PlayStation in their colours,
the letters of Xbox in theirs, shoulder buttons as bowls, a plus-shaped d-pad).

Sizes: 112x112 for round buttons, the d-pad, the square key and the symbols; 224x112 for shoulders, triggers, Start/Select
of PlayStation and the wide key. Needs `fonttools` and `resvg-py` (pip).
"""
import math
import os
import sys

# --- Palette

FACE_TOP, FACE_BOTTOM = "#4E4E54", "#2A2A2E"      # the dark face, lighter above
RIM_TOP, RIM_BOTTOM = "#8A8A92", "#16161A"        # the edge that catches the light
INK = "#F4F4F6"                                   # symbols and letters on the dark face
ACCENT_TOP, ACCENT_BOTTOM = "#F0645A", "#B5342C"  # a highlighted direction, a pressed button
PS = {"cross": "#7F98DE", "circle": "#E8505A", "square": "#E67FB9", "triangle": "#2DBBA6"}
XBOX = {"a": "#8DCB45", "b": "#EF5B4B", "x": "#4F8BEA", "y": "#F6C12F"}
FONT_PATH = os.environ.get("AMG_BUTTON_FONT", "MPLUSRounded1c-ExtraBold.ttf")


def num(v):
    return ("%.2f" % v).rstrip("0").rstrip(".")


# --- Font outlines

_font = {}


def font():
    if not _font:
        from fontTools.ttLib import TTFont
        f = TTFont(FONT_PATH)
        _font.update(f=f, cmap=f.getBestCmap(), gs=f.getGlyphSet(), upm=f["head"].unitsPerEm,
                     cap=getattr(f["OS/2"], "sCapHeight", 0) or f["head"].unitsPerEm * 0.7)
    return _font


def text_path(s, cx, cy, cap_height, tracking=0.0):
    """The outline of `s` as one SVG path, caps `cap_height` tall, its bounding box centred on (cx, cy)."""
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    F = font()
    sc = cap_height / F["cap"]

    def place(ox, oy, pen_factory):
        x = 0.0
        for ch in s:
            name = F["cmap"][ord(ch)]
            g = F["gs"][name]
            pen = pen_factory()
            g.draw(TransformPen(pen, (sc, 0, 0, -sc, ox + x * sc, oy)))
            yield pen
            x += g.width + tracking * F["upm"]

    bounds = None
    for p in place(0, 0, lambda: BoundsPen(F["gs"])):
        if p.bounds:
            b = p.bounds
            bounds = b if bounds is None else (min(bounds[0], b[0]), min(bounds[1], b[1]), max(bounds[2], b[2]), max(bounds[3], b[3]))
    ox = cx - (bounds[0] + bounds[2]) / 2
    oy = cy - (bounds[1] + bounds[3]) / 2
    parts = []
    for p in place(ox, oy, lambda: SVGPathPen(F["gs"], ntos=lambda v: num(v))):
        parts.append(p.getCommands())
    return " ".join(parts)


# --- SVG pieces

def svg_doc(w, h, body, defs=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">\n'
            f"  <defs>\n{defs}  </defs>\n{body}</svg>\n")


def lin(id_, top, bottom, x1=0, y1=0, x2=0, y2=1):
    return (f'    <linearGradient id="{id_}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">'
            f'<stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient>\n')


STD_DEFS = (lin("face", FACE_TOP, FACE_BOTTOM) + lin("rim", RIM_TOP, RIM_BOTTOM) + lin("accent", ACCENT_TOP, ACCENT_BOTTOM)
            + lin("keytop", "#55555B", "#3A3A3F") + lin("dish", "#1E1E22", "#34343A")
            + '    <linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFFFFF" stop-opacity="0.11"/>'
              '<stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>\n'
            + '    <linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFFFFF" stop-opacity="0.16"/>'
              '<stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>\n')


def rrect_path(x, y, w, h, tl, tr, br, bl):
    """A rectangle with its own radius at each corner, as a path."""
    return (f"M{num(x + tl)},{num(y)} H{num(x + w - tr)} A{num(tr)},{num(tr)} 0 0 1 {num(x + w)},{num(y + tr)} "
            f"V{num(y + h - br)} A{num(br)},{num(br)} 0 0 1 {num(x + w - br)},{num(y + h)} H{num(x + bl)} "
            f"A{num(bl)},{num(bl)} 0 0 1 {num(x)},{num(y + h - bl)} V{num(y + tl)} A{num(tl)},{num(tl)} 0 0 1 {num(x + tl)},{num(y)} Z")


def disc():
    """The round face of a button: a dark disc as large as the canvas, with a rim that catches the light."""
    return ('  <circle cx="56" cy="56" r="56" fill="url(#face)"/>\n'
            '  <ellipse cx="56" cy="30" rx="42" ry="25" fill="url(#gloss)"/>\n'
            '  <circle cx="56" cy="56" r="54.75" fill="none" stroke="url(#rim)" stroke-width="2.5" opacity="0.9"/>\n')


def shadowed(symbol_svg, dy=2.5, opacity=0.4):
    """A symbol with a soft dark copy under it, so that it reads as raised from the face."""
    import re
    shadow = re.sub(r'(fill|stroke)="#[0-9A-Fa-f]{3,8}"', r'\1="#000000"', symbol_svg)
    return (f'  <g transform="translate(0,{num(dy)})" opacity="{num(opacity)}">\n' + shadow + '  </g>\n' + symbol_svg)


def sym_line(d, color, width=10, extra=""):
    return f'  <path d="{d}" fill="none" stroke="{color}" stroke-width="{num(width)}" stroke-linecap="round" stroke-linejoin="round"{extra}/>\n'


# --- Round face buttons

def ps_symbol(name, color):
    if name == "cross":
        return sym_line("M31,31 L81,81 M81,31 L31,81", color)
    if name == "circle":
        return f'  <circle cx="56" cy="56" r="24" fill="none" stroke="{color}" stroke-width="10"/>\n'
    if name == "square":
        return sym_line("M32,32 H80 V80 H32 Z", color)
    if name == "triangle":
        return sym_line("M56,29 L82,74 H30 Z", color)


def face_ps(name, white=False):
    color = INK if white else PS[name]
    sym = ps_symbol(name, color)
    return svg_doc(112, 112, disc() + shadowed(sym), STD_DEFS)


def face_xbox(letter):
    path = text_path(letter.upper(), 56, 56, 48)
    sym = f'  <path d="{path}" fill="{XBOX[letter]}"/>\n'
    return svg_doc(112, 112, disc() + shadowed(sym), STD_DEFS)


def small_disc_icon(kind):
    """Xbox Menu and View: a round face with its own icon."""
    if kind == "menu":
        sym = sym_line("M32,40 H80 M32,56 H80 M32,72 H80", INK, 8)
    else:
        sym = (sym_line("M30,32 H64 V58 H30 Z", INK, 7)
               + '  <rect x="46" y="46" width="38" height="28" rx="2" fill="#35353A" stroke="#F4F4F6" stroke-width="7" stroke-linejoin="round"/>\n')
    return svg_doc(112, 112, disc() + shadowed(sym), STD_DEFS)


# --- Shoulders, triggers, Start/Select

# Each console's own look, the left-hand shapes (the inner end, towards the middle of the pad, is on the right); the
# right-hand ones are mirrored. PlayStation: a bumper with a flat top that swells into a bowl at the inner end, and a bulky
# trigger. Xbox: a long, low bumper with an arched top and a slanted inner end, and a leaner trigger that narrows towards
# the bottom like the real one.
SHAPES = {
    "ps_bumper": ("M40,16 H170 A22,22 0 0 1 192,38 V54 C192,80 170,98 138,98 H40 A26,26 0 0 1 14,72 V42 A26,26 0 0 1 40,16 Z", -4, 57),
    "ps_trigger": ("M54,6 H128 C168,6 194,30 194,64 C194,92 168,106 132,106 H54 A24,24 0 0 1 30,82 V30 A24,24 0 0 1 54,6 Z", -6, 46),
    "xbox_bumper": ("M38,38 Q112,10 188,24 Q206,28 208,46 L206,78 Q205,92 190,92 H40 Q22,92 20,74 V56 Q20,42 38,38 Z", -2, 60),
    "xbox_trigger": ("M50,10 H134 Q176,10 190,48 Q198,72 186,90 Q172,106 140,106 H78 Q48,106 40,80 Q30,50 34,26 Q36,10 50,10 Z", -2, 50),
}


def bumper(label, side, shape, cap=36):
    """A shoulder button or a trigger: the dark face of the discs, with its rim and a sheen along the top."""
    w, h = 224, 112
    path, dx, text_y = SHAPES[shape]
    mirror = f'translate({w} 0) scale(-1 1)' if side == "R" else ""
    text_x = 112 + (dx if side == "L" else -dx)
    t = text_path(label, text_x, text_y, cap, tracking=0.02)
    body = (f'  <g transform="{mirror}">\n'
            f'    <path d="{path}" fill="url(#face)" stroke="url(#rim)" stroke-width="3" stroke-linejoin="round"/>\n'
            f'    <path d="{path}" fill="url(#sheen)"/>\n'
            f'  </g>\n'
            f'  <g transform="translate(0,2.5)" opacity="0.4"><path d="{t}" fill="#000000"/></g>\n'
            f'  <path d="{t}" fill="{INK}"/>\n')
    return svg_doc(w, h, body, STD_DEFS)


def pill_icon(kind):
    """PlayStation Options (three lines) and Share/Create (a tray with an arrow): the small pills beside the pad."""
    w, h = 224, 112
    x, y, pw, ph = 32, 18, 160, 76
    body = (f'  <rect x="{x}" y="{y}" width="{pw}" height="{ph}" rx="{ph / 2}" fill="url(#rim)"/>\n'
            f'  <rect x="{x + 1.5}" y="{y + 1.5}" width="{pw - 3}" height="{ph - 3}" rx="{ph / 2 - 1.5}" fill="#1B1B1F"/>\n'
            f'  <rect x="{x + 5}" y="{y + 5}" width="{pw - 10}" height="{ph - 10}" rx="{ph / 2 - 5}" fill="url(#face)"/>\n')
    if kind == "options":
        icon = sym_line("M86,40 H138 M86,56 H138 M86,72 H138", INK, 8)
    else:
        icon = (sym_line("M90,54 V74 H134 V54", INK, 8) + sym_line("M112,62 V32", INK, 8) + sym_line("M99,44 L112,31 L125,44", INK, 8))
    return svg_doc(w, h, body + shadowed(icon, 2, 0.35), STD_DEFS)


def pill_icon_small(kind):
    """Options and Share as a small pill in a square canvas, for the places where the game puts a 28x28 glyph for Start and Select."""
    x, y, pw, ph = 6, 28, 100, 56
    body = (f'  <rect x="{x}" y="{y}" width="{pw}" height="{ph}" rx="{ph / 2}" fill="url(#rim)"/>\n'
            f'  <rect x="{x + 1.5}" y="{y + 1.5}" width="{pw - 3}" height="{ph - 3}" rx="{ph / 2 - 1.5}" fill="#1B1B1F"/>\n'
            f'  <rect x="{x + 4}" y="{y + 4}" width="{pw - 8}" height="{ph - 8}" rx="{ph / 2 - 4}" fill="url(#face)"/>\n')
    if kind == "options":
        icon = sym_line("M38,44 H74 M38,56 H74 M38,68 H74", INK, 6)
    else:
        icon = sym_line("M40,58 V70 H72 V58", INK, 6) + sym_line("M56,64 V42", INK, 6) + sym_line("M47,50 L56,41 L65,50", INK, 6)
    return svg_doc(112, 112, body + shadowed(icon, 1.5, 0.35), STD_DEFS)


# --- D-pad

def _rot(points, quarter_turns):
    """Points about (56, 56), turned a quarter turn clockwise that many times."""
    out = []
    for x, y in points:
        for _ in range(quarter_turns % 4):
            x, y = 112 - y, x
        out.append((x, y))
    return out


def _pts(points):
    return " ".join(f"{num(x)},{num(y)}" for x, y in points)


# The arm pointing up, the others are turns of it.
_ARM = [(38, 6), (74, 6), (74, 34), (56, 48), (38, 34)]
_ARROW = [(56, 14), (46, 28), (66, 28)]
_SIDES = {"up": 0, "right": 1, "down": 2, "left": 3}


def dpad_ps(highlight=()):
    """PlayStation: four separate arms that meet at the middle, each with its arrow."""
    body = ""
    for name, turns in _SIDES.items():
        poly = _pts(_rot(_ARM, turns))
        face = "url(#accent)" if name in highlight else "url(#face)"
        body += (f'  <polygon points="{poly}" fill="url(#rim)" stroke="url(#rim)" stroke-width="7" stroke-linejoin="round"/>\n'
                 f'  <polygon points="{poly}" fill="{face}" stroke="{face}" stroke-width="3" stroke-linejoin="round"/>\n'
                 f'  <polygon points="{poly}" fill="url(#sheen)" stroke="none"/>\n'
                 f'  <polygon points="{_pts(_rot(_ARROW, turns))}" fill="{"#FFFFFF" if name in highlight else "#B9B9C2"}"/>\n')
    return svg_doc(112, 112, body, STD_DEFS)


_PLUS = "M38,12 H74 V38 H100 V74 H74 V100 H38 V74 H12 V38 H38 Z"
_PLUS_ARMS = {"up": "M38,12 H74 V38 H38 Z", "right": "M74,38 H100 V74 H74 Z", "down": "M38,74 H74 V100 H38 Z", "left": "M12,38 H38 V74 H12 Z"}


def dpad_xbox(highlight=()):
    """Xbox: a round dish with a plus-shaped raised pad in it."""
    body = disc()
    body += (f'  <path d="{_PLUS}" fill="url(#dish)" stroke="url(#dish)" stroke-width="9" stroke-linejoin="round" transform="translate(0 2)"/>\n'
             f'  <path d="{_PLUS}" fill="url(#keytop)" stroke="url(#keytop)" stroke-width="7" stroke-linejoin="round"/>\n'
             f'  <path d="{_PLUS}" fill="url(#sheen)" stroke="none"/>\n')
    for name, turns in _SIDES.items():
        if name in highlight:
            body += f'  <path d="{_PLUS_ARMS[name]}" fill="url(#accent)" stroke="url(#accent)" stroke-width="7" stroke-linejoin="round"/>\n'
        body += (f'  <polygon points="{_pts(_rot([(56, 20), (47, 32), (65, 32)], turns))}" '
                 f'fill="{"#FFFFFF" if name in highlight else "#C9C9D0"}"/>\n')
    return svg_doc(112, 112, body, STD_DEFS)


# --- Keyboard

def key(wide=False):
    """A keycap: a darker body with a thicker lower lip, a face with a slight gradient and a thin light edge. The face
    stretches sideways: nothing inside the 24 px borders depends on the width."""
    w = 224 if wide else 112
    body = (f'  <rect x="6" y="12" width="{w - 12}" height="92" rx="20" fill="#121215"/>\n'
            f'  <rect x="6" y="6" width="{w - 12}" height="92" rx="20" fill="#26262A"/>\n'
            f'  <rect x="10" y="9" width="{w - 20}" height="82" rx="16" fill="url(#keytop)"/>\n'
            f'  <rect x="10" y="9" width="{w - 20}" height="44" rx="16" fill="url(#sheen)"/>\n'
            f'  <rect x="11.5" y="10.5" width="{w - 23}" height="79" rx="14.5" fill="none" stroke="#FFFFFF" stroke-opacity="0.14" stroke-width="1.5"/>\n')
    return svg_doc(w, 112, body, STD_DEFS)


def key_symbol(kind):
    """Symbols for keys whose name is not a letter, to put over a keycap (white, on transparent)."""
    P = {
        "up": sym_line("M56,84 V32 M36,52 L56,30 L76,52", INK, 9),
        "down": sym_line("M56,28 V80 M36,60 L56,82 L76,60", INK, 9),
        "left": sym_line("M84,56 H32 M52,36 L30,56 L52,76", INK, 9),
        "right": sym_line("M28,56 H80 M60,36 L82,56 L60,76", INK, 9),
        "enter": sym_line("M82,30 V62 H34 M52,44 L34,62 L52,80", INK, 9),
        "backspace": sym_line("M24,56 L44,32 H88 V80 H44 Z M56,46 L72,66 M72,46 L56,66", INK, 7),
        "shift": sym_line("M56,28 L82,58 H68 V82 H44 V58 H30 Z", INK, 8),
        "tab": sym_line("M24,56 H78 M60,38 L80,56 L60,74 M90,34 V78", INK, 8),
        "space": sym_line("M28,50 V70 H84 V50", INK, 9),
    }[kind]
    return svg_doc(112, 112, P, STD_DEFS)


def mouse(button):
    """A mouse seen from above with one part lit: "left", "right" or "wheel"."""
    lit = "url(#accent)"
    body = ('  <rect x="26" y="4" width="60" height="104" rx="30" fill="url(#rim)"/>\n'
            '  <rect x="27.5" y="5.5" width="57" height="101" rx="28.5" fill="#1B1B1F"/>\n'
            '  <rect x="31" y="9" width="50" height="94" rx="25" fill="url(#face)"/>\n'
            '  <rect x="31" y="9" width="50" height="47" rx="25" fill="url(#sheen)"/>\n')
    if button == "left":
        body += f'  <path d="M55,9 V56 H31 V34 A25,25 0 0 1 55,9 Z" fill="{lit}"/>\n'
    elif button == "right":
        body += f'  <path d="M57,9 V56 H81 V34 A25,25 0 0 0 57,9 Z" fill="{lit}"/>\n'
    body += ('  <path d="M31,56 H81 M56,9 V30 M56,48 V56" fill="none" stroke="#0B0B0D" stroke-opacity="0.6" stroke-width="2.5"/>\n'
             f'  <rect x="51" y="26" width="10" height="24" rx="5" fill="{lit if button == "wheel" else "#0E0E11"}"/>\n'
             '  <rect x="51" y="26" width="10" height="24" rx="5" fill="none" stroke="#FFFFFF" stroke-opacity="0.18" stroke-width="1.2"/>\n')
    return svg_doc(112, 112, body, STD_DEFS)


# --- The set

def make_glyphs():
    """Every glyph as SVG text, by family and name. Reads the font, so FONT_PATH must be set first."""
    G = {"ps": {}, "xbox": {}, "kbd": {}}
    for n in ("cross", "circle", "square", "triangle"):
        G["ps"][n] = face_ps(n)
    G["ps"].update({
        "l1": bumper("L1", "L", "ps_bumper"), "r1": bumper("R1", "R", "ps_bumper"),
        "l2": bumper("L2", "L", "ps_trigger"), "r2": bumper("R2", "R", "ps_trigger"),
        "options": pill_icon("options"), "share": pill_icon("share"),
        "options_small": pill_icon_small("options"), "share_small": pill_icon_small("share"),
    })
    for n in ("a", "b", "x", "y"):
        G["xbox"][n] = face_xbox(n)
    G["xbox"].update({
        "lb": bumper("LB", "L", "xbox_bumper"), "rb": bumper("RB", "R", "xbox_bumper"),
        "lt": bumper("LT", "L", "xbox_trigger"), "rt": bumper("RT", "R", "xbox_trigger"),
        "menu": small_disc_icon("menu"), "view": small_disc_icon("view"),
    })
    lit = {"dpad": (), "dpad_up": ("up",), "dpad_down": ("down",), "dpad_left": ("left",), "dpad_right": ("right",),
           "dpad_ud": ("up", "down"), "dpad_lr": ("left", "right")}
    for name, which in lit.items():
        G["ps"][name] = dpad_ps(which)
        G["xbox"][name] = dpad_xbox(which)
    G["kbd"].update({"key": key(), "key_wide": key(True), "mouse_left": mouse("left"), "mouse_right": mouse("right"),
                     "mouse_wheel": mouse("wheel")})
    for n in ("up", "down", "left", "right", "enter", "backspace", "shift", "tab", "space"):
        G["kbd"]["sym_" + n] = key_symbol(n)
    # the alternative PlayStation set with white symbols, as the game's own
    for n in ("cross", "circle", "square", "triangle"):
        G.setdefault("ps_white", {})[n] = face_ps(n, white=True)
    return G


def main():
    args = list(sys.argv[1:])
    global FONT_PATH
    if "--font" in args:
        i = args.index("--font")
        FONT_PATH = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        raise SystemExit(__doc__)
    return build(args[0], make_glyphs())


def build(out, glyphs_by_family):
    import io

    import resvg_py
    from PIL import Image
    made = []
    for family, glyphs in glyphs_by_family.items():
        for name, svg in glyphs.items():
            os.makedirs(os.path.join(out, "svg", family), exist_ok=True)
            os.makedirs(os.path.join(out, "png", family), exist_ok=True)
            with open(os.path.join(out, "svg", family, name + ".svg"), "w", encoding="utf-8", newline="\n") as f:
                f.write(svg)
            png_bytes = bytes(resvg_py.svg_to_bytes(svg_string=svg))
            with open(os.path.join(out, "png", family, name + ".png"), "wb") as f:
                f.write(png_bytes)
            made.append((family, name, Image.open(io.BytesIO(png_bytes)).convert("RGBA")))
    cols = 8
    cell_w, cell_h = 240, 128
    rows = (len(made) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell_w, rows * cell_h), (150, 120, 120, 255))
    for i, (family, name, png) in enumerate(made):
        sheet.alpha_composite(png, ((i % cols) * cell_w + 8, (i // cols) * cell_h + 8))
    sheet.save(os.path.join(out, "sheet.png"))
    print(f"{len(made)} glyphs written to {out}")


if __name__ == "__main__":
    main()
