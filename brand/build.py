#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# rsh brand: writes every SVG and PNG in this directory from the geometry below.
# Usage: python3 -I brand/build.py   (needs rsvg-convert; fontTools and the Inter and
# JetBrains Mono fonts for the outlined words in the wordmark and the social preview).
import math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Colours. All from the WolOS tokens (design/tokens/colors.json) except GARNET, rsh's accent.
GARNET = "#A51C55"          # rsh accent (docs/brand.md)
RED_L = "#CF3A68"           # light.red
RED_D = "#F2678F"           # dark.red
TINT = "#F7AFC3"            # icon.categories.talk.tint
INK = "#181521"             # light.text
PAPER = "#F6F5FB"           # dark.text
BG_D = "#191720"            # dark.bg
SURF_D = "#2A2832"          # dark.surface
TEXT2_D = "#AAA7B6"         # dark.text-2
TEXT2_L = "#686575"         # light.text-2
BG_L = "#F3F1F8"            # light.bg

S = 1024                    # badge grid
INK_BOX = 640               # the glyph's longer side in the badge (Cryswol: 684)
RX = 230                    # corner radius, same as the Cryswol badge


def f(v):
    s = "%.2f" % v
    return s.rstrip("0").rstrip(".") if "." in s else s


def poly(pts):
    return "M" + " L".join("%s %s" % (f(x), f(y)) for x, y in pts) + " Z"


def rrect(x, y, w, h, r):
    # rounded rectangle as a path (arcs), so every file is paths only
    return ("M%s %s H%s A%s %s 0 0 1 %s %s V%s A%s %s 0 0 1 %s %s H%s A%s %s 0 0 1 %s %s V%s A%s %s 0 0 1 %s %s Z"
            % tuple(map(f, (x + r, y, x + w - r, r, r, x + w, y + r, y + h - r, r, r, x + w - r, y + h,
                             x + r, r, r, x, y + h - r, y + r, r, r, x + r, y))))


def glyph():
    """The cut prompt: a '>' carved like a stone (each arm two facets meeting on a ridge),
    and a cut stone in the place of the cursor. Returns facets as (name, points) in the 1024 grid, centred."""
    ax, ay = 292.0, 286.0          # top end of the ridge line
    cx, cy = 600.0, 512.0          # apex of the ridge line
    w = 84.0                       # half thickness of an arm
    dx, dy = cx - ax, cy - ay
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    nx, ny = -uy, ux               # points to the inside (down-left) for the top arm

    def at_x(px, py, x):           # point on the line through (px,py) with direction u at x
        t = (x - px) / ux
        return (x, py + t * uy)

    def at_y(px, py, y):
        t = (y - py) / uy
        return (px + t * ux, y)

    out0 = (ax - nx * w, ay - ny * w)
    inn0 = (ax + nx * w, ay + ny * w)
    O = at_y(out0[0], out0[1], cy)         # outer apex
    I = at_y(inn0[0], inn0[1], cy)         # inner apex
    xe = ax                                 # ends cut upright at the ridge's x
    To = at_x(out0[0], out0[1], xe)
    Ti = at_x(inn0[0], inn0[1], xe)
    Tm = (ax, ay)
    C = (cx, cy)
    mirror = lambda p: (p[0], 2 * cy - p[1])
    Bo, Bi, Bm = mirror(To), mirror(Ti), mirror(Tm)
    # the cursor is a cut stone seen from the side: table on top, girdle, point down
    gw = 310.0                     # girdle width
    gx0 = O[0] + 4
    gx1 = gx0 + gw
    gm = (gx0 + gx1) / 2
    yb = Bo[1]                     # the point sits on the baseline of the lower arm
    yg = yb - 146                  # girdle
    yt = yg - 76                   # table
    inset = 76
    t0, t1 = (gx0 + inset, yt), (gx1 - inset, yt)
    g0, g1 = (gx0, yg), (gx1, yg)
    tm, gmid, cul = (gm, yt), (gm, yg), (gm, yb)
    facets = [
        ("top-out", [To, O, C, Tm]),
        ("top-in", [Tm, C, I, Ti]),
        ("bot-in", [Bi, I, C, Bm]),
        ("bot-out", [Bm, C, O, Bo]),
        ("crown-l", [t0, tm, gmid, g0]),
        ("crown-r", [tm, t1, g1, gmid]),
        ("pav-l", [g0, gmid, cul]),
        ("pav-r", [gmid, g1, cul]),
    ]
    xs = [x for _, p in facets for x, _ in p]
    ys = [y for _, p in facets for _, y in p]
    # centre the ink box; nudge 8 px left because the open side of the '>' reads lighter
    mx, my = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    k = INK_BOX / max(max(xs) - min(xs), max(ys) - min(ys))
    return [(n, [(S / 2 + (x - mx) * k, S / 2 + (y - my) * k) for x, y in p]) for n, p in facets]


# Shade per facet: light from the upper left, like the Cryswol gem (0.95 / 0.75 / 0.5 steps).
WHITE_OP = {"top-out": 1.0, "top-in": 0.72, "bot-in": 0.5, "bot-out": 0.84,
            "crown-l": 1.0, "crown-r": 0.84, "pav-l": 0.72, "pav-r": 0.5}
ON_LIGHT = {"top-out": RED_D, "top-in": RED_L, "bot-in": GARNET, "bot-out": RED_L,
            "crown-l": RED_D, "crown-r": RED_L, "pav-l": RED_L, "pav-r": GARNET}
ON_DARK = {"top-out": TINT, "top-in": RED_D, "bot-in": RED_L, "bot-out": RED_D,
           "crown-l": TINT, "crown-r": RED_D, "pav-l": RED_D, "pav-r": RED_L}

GRAD = ('<linearGradient id="g" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="%s"/><stop offset=".5" stop-color="%s"/>'
        '<stop offset="1" stop-color="%s"/></linearGradient>' % (RED_D, RED_L, GARNET))


def svg(vb, body, w=None, h=None, defs=""):
    vw, vh = vb
    w = w or vw
    h = h or vh
    d = "<defs>%s</defs>" % defs if defs else ""
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s" width="%s" height="%s">%s%s</svg>\n'
            % (f(vw), f(vh), f(w), f(h), d, body))


def glyph_paths(style, g=None, tr=""):
    g = g or glyph()
    out = []
    for n, p in g:
        if style == "white":
            op = WHITE_OP[n]
            out.append('<path d="%s" fill="#fff"%s/>' % (poly(p), "" if op == 1 else ' fill-opacity="%s"' % op))
        elif style == "light":
            out.append('<path d="%s" fill="%s"/>' % (poly(p), ON_LIGHT[n]))
        elif style == "dark":
            out.append('<path d="%s" fill="%s"/>' % (poly(p), ON_DARK[n]))
    body = "".join(out)
    return '<g transform="%s">%s</g>' % (tr, body) if tr else body


def mono_path(g=None):
    # one-colour silhouette: the facets of each arm joined, a hair cut on the ridge omitted
    g = g or glyph()
    p = dict(g)
    top = [p["top-out"][0], p["top-out"][1], p["top-in"][2], p["top-in"][3]]
    bot = [p["bot-in"][0], p["bot-in"][1], p["bot-out"][2], p["bot-out"][3]]
    # one closed '>' outline: outer top end, outer apex, outer bottom end, inner bottom end, inner apex, inner top end
    chev = [top[0], top[1], bot[3], bot[0], top[2], top[3]]
    gem = [p["crown-l"][0], p["crown-r"][1], p["crown-r"][2], p["pav-r"][2], p["crown-l"][3]]
    return poly(chev) + " " + poly(gem)


def badge(style):
    if style == "light":
        ground = '<path d="%s" fill="url(#g)"/>' % rrect(0, 0, S, S, RX)
        return svg((S, S), ground + glyph_paths("white"), defs=GRAD)
    # dark: the stone on the dark tile, garnet facets lit for dark grounds
    ground = '<path d="%s" fill="%s"/>' % (rrect(0, 0, S, S, RX), SURF_D)
    return svg((S, S), ground + glyph_paths("dark"))


def badge16(dark=False):
    # 16 px: drawn for the grid, flat white '>' and cursor, no facets (they blur at this size)
    ground = '<path d="%s" fill="%s"/>' % (rrect(0, 0, 16, 16, 3.6), SURF_D if dark else "url(#g)")
    ink = RED_D if dark else "#fff"
    chev = poly([(2.5, 3), (5, 3), (9, 7.5), (5, 12), (2.5, 12), (6.5, 7.5)])
    bar = poly([(10, 9), (13, 9), (14, 10.5), (11.5, 13), (9, 10.5)])
    return svg((16, 16), ground + '<path d="%s %s" fill="%s"/>' % (chev, bar, ink), defs="" if dark else GRAD)


def symbolic16():
    chev = poly([(1, 2), (3.8, 2), (8.6, 7.5), (3.8, 13), (1, 13), (5.8, 7.5)])
    bar = poly([(10, 9), (14, 9), (15, 10.5), (12, 14), (9, 10.5)])
    return svg((16, 16), '<path d="%s %s" fill="currentColor"/>' % (chev, bar))


# Outlined words (no font is embedded or needed to show the files).
def text_path(fontfile, text, size, x, y, tracking=0.0):
    from fontTools.ttLib import TTFont
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    font = TTFont(fontfile)
    gs = font.getGlyphSet()
    cmap = font.getBestCmap()
    upm = font["head"].unitsPerEm
    k = size / upm
    hmtx = font["hmtx"]
    pen = SVGPathPen(gs)
    cx = x
    for ch in text:
        gn = cmap[ord(ch)]
        tp = TransformPen(pen, (k, 0, 0, -k, cx, y))
        gs[gn].draw(tp)
        cx += hmtx[gn][0] * k + tracking * size
    return pen.getCommands(), cx - x - tracking * size


FONT_BLACK = "/usr/share/fonts/opentype/inter/Inter-Black.otf"
FONT_MED = "/usr/share/fonts/opentype/inter/Inter-Medium.otf"
FONT_MONO = "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Medium.ttf"
FONT_MONO_B = "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-ExtraBold.ttf"


def wordmark(dark=False):
    # badge and "rsh" in JetBrains Mono ExtraBold, outlined: 440 x 128
    H = 128
    bsz = 112
    k = bsz / S
    ground = '<path d="%s" fill="url(#g)"/>' % rrect(0, 0, S, S, RX)
    mark = '<g transform="translate(8 8) scale(%s)">%s%s</g>' % (f(k), ground, glyph_paths("white"))
    d, wdt = text_path(FONT_MONO_B, "rsh", 104, 144, 98, tracking=-0.02)
    W = math.ceil(144 + wdt + 12)
    col = PAPER if dark else INK
    return svg((W, H), mark + '<path d="%s" fill="%s"/>' % (d, col), defs=GRAD)


def social():
    W, H = 1280, 640
    ground = '<path d="M0 0 H%d V%d H0 Z" fill="%s"/>' % (W, H, BG_D)
    bsz = 300
    k = bsz / S
    bx, by = 120, (H - bsz) / 2 - 78
    mark = ('<g transform="translate(%s %s) scale(%s)"><path d="%s" fill="url(#g)"/>%s</g>'
            % (f(bx), f(by), f(k), rrect(0, 0, S, S, RX), glyph_paths("white")))
    tx = bx + bsz + 72
    d1, _ = text_path(FONT_MONO_B, "rsh", 168, tx, by + 158, tracking=-0.02)
    d2, _ = text_path(FONT_MED, "RubyShell: Ruby with a shell layer", 40, tx + 6, by + 232)
    d3, _ = text_path(FONT_MED, "One script for the logic and the commands.", 30, tx + 6, by + 286)
    code = ['logs = Dir.glob("*.log").select { |f| File.size(f) > 0 }',
            'tar -czf logs.tgz #{logs} && rm #{logs}']
    out = [ground, mark,
           '<path d="%s" fill="%s"/>' % (d1, PAPER),
           '<path d="%s" fill="%s"/>' % (d2, PAPER),
           '<path d="%s" fill="%s"/>' % (d3, TEXT2_D)]
    cy = by + bsz + 92
    panel_h = 116
    out.append('<path d="%s" fill="%s"/>' % (rrect(bx, cy - 50, W - 2 * bx, panel_h, 16), SURF_D))
    for i, line in enumerate(code):
        dd, _ = text_path(FONT_MONO, line, 26, bx + 32, cy + i * 40, )
        out.append('<path d="%s" fill="%s"/>' % (dd, TINT if i == 1 else PAPER))
    return svg((W, H), "".join(out), defs=GRAD)


def write(name, text):
    with open(os.path.join(HERE, name), "w") as fh:
        fh.write(text)


def png(src, dst, w, h=None):
    h = h or w
    subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h), "-o", os.path.join(HERE, dst),
                    os.path.join(HERE, src)], check=True)


def main():
    write("rsh-badge.svg", badge("light"))
    write("rsh-badge-dark.svg", badge("dark"))
    write("rsh-glyph.svg", svg((S, S), glyph_paths("light")))
    write("rsh-glyph-dark.svg", svg((S, S), glyph_paths("dark")))
    write("rsh-glyph-white.svg", svg((S, S), glyph_paths("white")))
    write("rsh-glyph-mono.svg", svg((S, S), '<path d="%s" fill="%s" fill-rule="nonzero"/>' % (mono_path(), INK)))
    write("rsh-badge-16.svg", badge16())
    write("rsh-badge-16-dark.svg", badge16(True))
    write("rsh-symbolic.svg", symbolic16())
    write("rsh-wordmark.svg", wordmark())
    write("rsh-wordmark-dark.svg", wordmark(True))
    write("rsh-social.svg", social())
    os.makedirs(os.path.join(HERE, "png"), exist_ok=True)
    for base in ("rsh-badge", "rsh-badge-dark", "rsh-glyph", "rsh-glyph-dark"):
        for m in (1, 2, 4):
            png(base + ".svg", "png/%s@%dx.png" % (base, m), 128 * m)
    png("rsh-badge.svg", "png/rsh-avatar-512.png", 512)
    png("rsh-social.svg", "png/rsh-social-1280x640.png", 1280, 640)
    for sz in (16, 32, 48, 64):
        for dark in ("", "-dark"):
            src = ("rsh-badge-16%s.svg" if sz == 16 else "rsh-badge%s.svg") % dark
            png(src, "png/rsh-icon%s-%d.png" % (dark, sz), sz)
    png("rsh-wordmark.svg", "png/rsh-wordmark@2x.png", 2 * int(float(open(os.path.join(HERE, "rsh-wordmark.svg")).read().split('width="')[1].split('"')[0])), 256)


if __name__ == "__main__":
    sys.exit(main())
