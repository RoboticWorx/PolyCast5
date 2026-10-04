# boot_splash_art.py - polycast5.com
#
# Regenerates components/lcd/include/lcd_boot_splash_art.h, the boot splash art:
# the wordmark (pc5_wordmark.png), "Booting up..." (Montserrat SemiBold, shipped with LVGL)
# and the five radio icons (SVG below) as 4bpp alpha masks that lcd_boot_splash.c tints at draw time
#
# The icons go through a small rasterizer of its own: rect, circle and path (M L H V Q A Z),
# fill and round-capped strokes, rotate() and opacity. Enough for these five, not general SVG
#
# Run from anywhere: python scripts/dev/boot_splash_art.py
# Requires: pip install Pillow

import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Inputs and output, from the repo root so the script runs from anywhere
REPO = Path(__file__).resolve().parents[2]
WORDMARK_PNG = REPO / 'scripts/dev/pc5_wordmark.png'
FONT_TTF = REPO / 'components/lvgl/demos/multilang/assets/fonts/Montserrat-SemiBold.ttf'
OUT_H = REPO / 'components/lcd/include/lcd_boot_splash_art.h'

WORDMARK_H = 46  # px; width follows the PNG's aspect
TEXT = 'Booting up'  # Followed by DOTS dots; the firmware reveals 0..DOTS of them
TEXT_PX = 18  # Font size
DOTS = 3
DOT_GAP = 2  # Extra px between the dots, so they read as dots rather than an ellipsis
ICON_PX = 22  # Icons are square; every SVG viewBox is 28 x 28
ICON_SS = 8  # Supersampling per axis

# Radio icons, left to right on the splash. Pasted SVG, limited to what parse_icon() handles
ICONS = {
    'infrared': '''<svg viewBox="0 0 28 28"><g stroke="#1550A5" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" transform="rotate(-90 14 14)"><rect x="4" y="23" width="20" height="3" fill="#1550A5"></rect><rect x="7" y="19.5" width="14" height="3.5" fill="#1550A5"></rect><path d="M11 19.5 A3 3 0 0 1 17 19.5" fill="#1550A5"></path><path d="M10 13.5 Q14 10.5 18 13.5" fill="none"></path><path d="M7.5 10.5 Q14 5.5 20.5 10.5" fill="none"></path><path d="M5 7.5 Q14 0.5 23 7.5" fill="none"></path></g></svg>''',
    'wifi': '''<svg viewBox="0 0 28 28"><g stroke="#1550A5" stroke-width="2" stroke-linecap="round" fill="none"><path d="M2.5 12 Q14 0 25.5 12"></path><path d="M6 15 Q14 6 22 15"></path><path d="M9.5 18 Q14 13 18.5 18"></path><circle cx="14" cy="22" r="1.8" fill="#1550A5" stroke="none"></circle></g></svg>''',
    'lora': '''<svg viewBox="0 0 28 28"><g fill="#1550A5" stroke="none"><path d="M13 11 L15 11 L16.5 26 L11.5 26 Z"></path><circle cx="14" cy="8.5" r="3"></circle></g><g fill="none" stroke="#1550A5" stroke-width="1.6" stroke-linecap="round"><path d="M9.5 5.5 Q7 8.5 9 12"></path><path d="M6.5 4 Q2.5 8.5 6 14"></path><path d="M3.5 3 Q-2 8.5 3.5 15.5" opacity="0.85"></path><path d="M18.5 5.5 Q21 8.5 19 12"></path><path d="M21.5 4 Q25.5 8.5 22 14"></path><path d="M24.5 3 Q30 8.5 24.5 15.5" opacity="0.85"></path></g></svg>''',
    'bluetooth': '''<svg viewBox="0 0 28 28"><g stroke="#1550A5" stroke-width="2" stroke-linejoin="round" stroke-linecap="round" fill="none"><path d="M14 1.6 V26.4 M14 1.6 L20.4 8 L7.6 20.4 M7.6 7.6 L20.4 20 L14 26.4" stroke-width="2.3"></path></g></svg>''',
    'espnow': '''<svg viewBox="0 0 28 28"><g stroke="#1550A5" stroke-width="2" stroke-linecap="round" fill="none"><rect x="6" y="6" width="16" height="16" rx="2"></rect><path d="M2 10 h4 M2 14 h4 M2 18 h4 M22 10 h4 M22 14 h4 M22 18 h4 M10 2 v4 M14 2 v4 M18 2 v4 M10 22 v4 M14 22 v4 M18 22 v4"></path><circle cx="14" cy="14" r="2.2" fill="#1550A5"></circle></g></svg>''',
}


# ---- Wordmark and text ----

# 8-bit coverage image to rows of 4-bit coverage (0..15)
def to_a4(im):
    return [[(im.getpixel((x, y)) * 15 + 127) // 255 for x in range(im.width)] for y in range(im.height)]


# Wordmark alpha, trimmed to its ink and scaled to WORDMARK_H tall
def wordmark():
    a = Image.open(WORDMARK_PNG).convert('RGBA').getchannel('A')
    a = a.crop(a.getbbox())
    # Box filter = true area coverage, so the thin wave arcs keep their weight
    return to_a4(a.resize((round(a.width * WORDMARK_H / a.height), WORDMARK_H), Image.BOX))


# "Booting up..." mask, plus the column to clip at for each dot count
def text():
    font = ImageFont.truetype(str(FONT_TTF), TEXT_PX)
    canvas = (TEXT_PX * len(TEXT) * 2, TEXT_PX * 3)

    # Render TEXT plus the given number of dots, DOT_GAP px apart
    def ink(dots):
        im = Image.new('L', canvas, 0)
        draw = ImageDraw.Draw(im)
        draw.text((2, TEXT_PX), TEXT, font=font, fill=255)
        x = 2 + font.getlength(TEXT)
        for _ in range(dots):
            draw.text((x, TEXT_PX), '.', font=font, fill=255)
            x += font.getlength('.') + DOT_GAP
        return im

    full = ink(DOTS)
    box = full.getbbox()
    # Reveal column for 0..DOTS dots: one past the ink with n dots, relative to the crop
    cuts = [ink(n).getbbox()[2] - box[0] for n in range(DOTS)] + [box[2] - box[0]]
    mask = to_a4(full.crop(box))
    # The firmware clips at these columns, so each must fall in clear space between glyphs
    for n in range(DOTS):
        gap = [row[cuts[n]] for row in mask]
        assert not any(gap), f'dot {n + 1} touches the previous glyph; cut column {cuts[n]} has ink'
    return mask, cuts


# ---- Minimal SVG rasterizer ----
# Shapes are flattened to polygons and polylines in viewBox units, then icon() samples coverage

# Flatten a path's d attribute into [(points, closed), ...]; each curve becomes 32 segments
def path_subpaths(d):
    toks = re.findall(r'[MmLlHhVvQqAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?', d)
    subs, pts, cmd, i = [], [], None, 0
    x = y = sx = sy = 0.0

    # Next number token
    def num():
        nonlocal i
        i += 1
        return float(toks[i - 1])

    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]):
            cmd = toks[i]
            i += 1
            # Close the subpath and return to its start
            if cmd in 'Zz':
                subs.append((pts, True))
                pts, x, y = [], sx, sy
                continue
        # Lowercase commands are relative to the current point
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if c == 'M':
            if pts:
                subs.append((pts, False))
            x, y = ox + num(), oy + num()
            sx, sy, pts = x, y, [(x, y)]
            cmd = 'l' if rel else 'L'  # Extra pairs after M are line-tos
        elif c == 'L':
            x, y = ox + num(), oy + num()
            pts.append((x, y))
        elif c == 'H':
            x = ox + num()
            pts.append((x, y))
        elif c == 'V':
            y = oy + num()
            pts.append((x, y))
        elif c == 'Q':
            cx, cy, ex, ey = ox + num(), oy + num(), ox + num(), oy + num()
            x0, y0 = x, y
            for k in range(1, 33):
                t = k / 32
                pts.append(((1 - t) ** 2 * x0 + 2 * t * (1 - t) * cx + t * t * ex,
                            (1 - t) ** 2 * y0 + 2 * t * (1 - t) * cy + t * t * ey))
            x, y = ex, ey
        elif c == 'A':
            rx, ry, rot, large, sweep = num(), num(), num(), num(), num()
            ex, ey = ox + num(), oy + num()
            pts.extend(arc_points(x, y, rx, ry, rot, int(large), int(sweep), ex, ey))
            x, y = ex, ey
        else:
            raise ValueError(f'unsupported path command {cmd}')
    if pts:
        subs.append((pts, False))
    return subs


# Points along an elliptical arc, excluding its start
# SVG 1.1 implementation notes F.6.5: endpoint to centre parameterization
def arc_points(x1, y1, rx, ry, phi_deg, large, sweep, x2, y2):
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx, cy = cp * cxp - sp * cyp + (x1 + x2) / 2, sp * cxp + cp * cyp + (y1 + y2) / 2
    a1 = math.atan2((y1p - cyp) / ry, (x1p - cxp) / rx)
    a2 = math.atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx)
    da = a2 - a1
    if sweep and da < 0:
        da += 2 * math.pi
    elif not sweep and da > 0:
        da -= 2 * math.pi
    out = []
    for k in range(1, 33):
        a = a1 + da * k / 32
        px, py = rx * math.cos(a), ry * math.sin(a)
        out.append((cp * px - sp * py + cx, sp * px + cp * py + cy))
    return out


# Outline of a rect, each rx corner a 9-point quarter circle
def rect_points(x, y, w, h, rx):
    if rx <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    pts = []
    for cx, cy, a0 in ((x + w - rx, y + rx, -90), (x + w - rx, y + h - rx, 0),
                       (x + rx, y + h - rx, 90), (x + rx, y + rx, 180)):
        for k in range(9):
            a = math.radians(a0 + 90 * k / 8)
            pts.append((cx + rx * math.cos(a), cy + rx * math.sin(a)))
    return pts


# 64-point circle outline
def circle_points(cx, cy, r):
    return [(cx + r * math.cos(2 * math.pi * k / 64), cy + r * math.sin(2 * math.pi * k / 64)) for k in range(64)]


# Every fill and stroke in an icon as (kind, points, closed, width, opacity), in viewBox units
def parse_icon(svg):
    prims = []

    # Depth first, inheriting fill/stroke/width/opacity and composing rotate() into xf
    def walk(el, style, xf):
        style = dict(style)
        for k in ('fill', 'stroke', 'stroke-width', 'opacity'):
            if k in el.attrib:
                style[k] = el.attrib[k]
        m = re.match(r'rotate\(([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)', el.attrib.get('transform', ''))
        if m:
            a, ox, oy = (float(v) for v in m.groups())
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            prev = xf
            xf = lambda p, prev=prev: prev((ca * (p[0] - ox) - sa * (p[1] - oy) + ox,
                                            sa * (p[0] - ox) + ca * (p[1] - oy) + oy))
        tag = el.tag.split('}')[-1]
        if tag == 'rect':
            g = {k: float(el.attrib.get(k, 0)) for k in ('x', 'y', 'width', 'height', 'rx')}
            subs = [(rect_points(g['x'], g['y'], g['width'], g['height'], g['rx']), True)]
        elif tag == 'circle':
            subs = [(circle_points(*(float(el.attrib[k]) for k in ('cx', 'cy', 'r'))), True)]
        elif tag == 'path':
            subs = path_subpaths(el.attrib['d'])
        else:
            subs = []
        op = float(style.get('opacity', 1))
        for pts, closed in subs:
            pts = [xf(p) for p in pts]
            # SVG fills by default (black), so anything not 'none' fills
            if style.get('fill', 'black') != 'none':
                prims.append(('fill', pts, True, 0, op))
            if style.get('stroke', 'none') != 'none':
                prims.append(('stroke', pts, closed, float(style.get('stroke-width', 1)), op))
        for child in el:
            walk(child, style, xf)

    walk(ET.fromstring(svg), {}, lambda p: p)
    return prims


# Squared distance from a point to segment ab
def seg_dist2(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    l2 = vx * vx + vy * vy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / l2))
    qx, qy = ax + t * vx - px, ay + t * vy - py
    return qx * qx + qy * qy


# Non-zero winding, SVG's default fill-rule
def inside(px, py, pts):
    wn = 0
    for (ax, ay), (bx, by) in zip(pts, pts[1:] + pts[:1]):
        if ay <= py < by and (bx - ax) * (py - ay) - (px - ax) * (by - ay) > 0:
            wn += 1
        elif by <= py < ay and (bx - ax) * (py - ay) - (px - ax) * (by - ay) < 0:
            wn -= 1
    return wn != 0


# Rasterize one icon to px_size x px_size rows of 4-bit coverage, ICON_SS^2 samples per pixel
def icon(svg, px_size=None):
    px_size = px_size or ICON_PX
    # Pad each shape's bounding box by half its stroke, so samples outside it skip the shape
    prims = []
    for kind, pts, closed, w, op in parse_icon(svg):
        pad = w / 2
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        segs = list(zip(pts, pts[1:] + (pts[:1] if closed else [])))
        prims.append((kind, pts, segs, w, op, (min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad)))
    scale = 28 / px_size
    out = []
    for y in range(px_size):
        row = []
        for x in range(px_size):
            acc = 0.0
            for sy in range(ICON_SS):
                for sx in range(ICON_SS):
                    px = (x + (sx + 0.5) / ICON_SS) * scale
                    py = (y + (sy + 0.5) / ICON_SS) * scale
                    clear = 1.0  # Product of (1 - opacity) over every shape covering this sample
                    for kind, pts, segs, w, op, (x0, y0, x1, y1) in prims:
                        if not (x0 <= px <= x1 and y0 <= py <= y1):
                            continue
                        if kind == 'fill':
                            hit = inside(px, py, pts)
                        else:
                            # Round caps and joins: a stroke is exactly the points within w/2 of its polyline
                            hit = any(seg_dist2(px, py, *a, *b) <= (w / 2) ** 2 for a, b in segs)
                        if hit:
                            clear *= 1 - op
                    acc += 1 - clear
            row.append(round(acc / (ICON_SS * ICON_SS) * 15))
        out.append(row)
    return out


# ---- Header ----

# C array for one mask: two pixels per byte, high nibble on the left, rows padded to whole bytes
def emit(name, mask):
    w, h = len(mask[0]), len(mask)
    stride = (w + 1) // 2
    lines = [f'static const uint8_t {name}[{h} * {stride}] = {{']
    for row in mask:
        row = row + [0] * (stride * 2 - w)
        packed = [(row[i] << 4) | row[i + 1] for i in range(0, len(row), 2)]
        for i in range(0, stride, 20):
            lines.append('    ' + ' '.join(f'0x{b:02X},' for b in packed[i:i + 20]))
    lines.append('};')
    return w, h, stride, '\n'.join(lines)


# Every icon in one 2-D array, one block per icon in ICONS order
def emit_icons(masks):
    stride = (ICON_PX + 1) // 2
    lines = [f'static const uint8_t s_art_icons[ART_ICON_COUNT][ART_ICON_PX * ART_ICON_STRIDE] = {{']
    for name, mask in masks.items():
        lines.append(f'    {{ // {name}')
        for row in mask:
            row = row + [0] * (stride * 2 - ICON_PX)
            packed = [(row[i] << 4) | row[i + 1] for i in range(0, len(row), 2)]
            lines.append('        ' + ' '.join(f'0x{b:02X},' for b in packed))
        lines.append('    },')
    lines.append('};')
    return stride, '\n'.join(lines)


# Build every mask and write the header
def main():
    wm = wordmark()
    txt, cuts = text()
    icons = {name: icon(svg) for name, svg in ICONS.items()}
    wm_w, wm_h, wm_s, wm_c = emit('s_art_wordmark', wm)
    tx_w, tx_h, tx_s, tx_c = emit('s_art_text', txt)
    ic_s, ic_c = emit_icons(icons)
    OUT_H.write_text(f'''#ifndef LCD_BOOT_SPLASH_ART_H
#define LCD_BOOT_SPLASH_ART_H

#include <stdint.h>

/**
 * @brief Alpha masks for the boot splash. GENERATED by scripts/dev/boot_splash_art.py, do not edit.
 *
 * 4bpp coverage, two pixels per byte with the high nibble on the left, rows padded to whole bytes.
 * Wordmark: scripts/dev/pc5_wordmark.png box-filtered to {WORDMARK_H} px tall.
 * Text: "{TEXT}{'.' * DOTS}" in Montserrat SemiBold {TEXT_PX} px (OFL), cropped to its ink.
 * Icons: {', '.join(ICONS)}, from the SVGs in the script, {ICON_PX} x {ICON_PX} px.
 */

#define ART_WORDMARK_W      {wm_w}
#define ART_WORDMARK_H      {wm_h}
#define ART_WORDMARK_STRIDE {wm_s}

#define ART_TEXT_W      {tx_w}
#define ART_TEXT_H      {tx_h}
#define ART_TEXT_STRIDE {tx_s}
#define ART_TEXT_DOTS   {DOTS}

#define ART_ICON_COUNT  {len(ICONS)}
#define ART_ICON_PX     {ICON_PX}
#define ART_ICON_STRIDE {ic_s}

// Columns to clip the text at to show 0..ART_TEXT_DOTS trailing dots
static const uint8_t s_art_text_cut[ART_TEXT_DOTS + 1] = {{{', '.join(map(str, cuts))}}};

// Wordmark, ART_WORDMARK_H rows of ART_WORDMARK_STRIDE bytes
{wm_c}

// Text with every dot; lcd_boot_splash.c clips it at s_art_text_cut[]
{tx_c}

// Radio icons, left to right on the splash
{ic_c}

#endif // LCD_BOOT_SPLASH_ART_H
''', newline='\n')
    print(f'wordmark {wm_w}x{wm_h}, text {tx_w}x{tx_h}, cuts {cuts}, icons {len(ICONS)} x {ICON_PX}px '
          f'-> {OUT_H.relative_to(REPO)}')


if __name__ == '__main__':
    main()
