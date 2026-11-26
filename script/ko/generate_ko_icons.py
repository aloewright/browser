#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""Generates the K-O mark and every icon asset the K-O branding ships.

  generate_ko_icons.py svg     # rewrite SVG + .icon sources from the geometry
  generate_ko_icons.py render  # rasterize the committed SVGs into PNG/ICNS/ICO

`render` needs cairosvg (pip install cairosvg) and ImageMagick (`convert`).
Outputs are committed; the build only copies them. Assets.car is compiled on
macOS from mac/Assets.xcassets (see //brave/build/commands/lib/branding.js).
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

BRAVE_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), os.pardir, os.pardir))
THEME = os.path.join(BRAVE_DIR, 'app', 'theme')
KO = os.path.join(THEME, 'ko')
SOURCE = os.path.join(KO, 'source')
P100 = os.path.join(THEME, 'default_100_percent', 'ko')
P200 = os.path.join(THEME, 'default_200_percent', 'ko')
MAC_BUILD = os.path.join(BRAVE_DIR, 'build', 'mac')
WELCOME_ASSETS = os.path.join(BRAVE_DIR, 'components', 'brave_welcome_ui',
                              'assets')

# Colours. Release: charcoal tile; development: slate blue tile.
INK = '#F4F5F7'
DARK_INK = '#1E1F24'
ACCENT = ('#FF7A45', '#F0284A')
TILES = {
    'release': ('#2B2E37', '#121317'),
    'development': ('#33568F', '#16264A'),
}

# --- Geometry -----------------------------------------------------------------
# The mark is "K-O" set in geometric strokes on a 24-unit grid, centred on
# (12, 12). `compact` drops the hyphen and thickens strokes for <= 32 px.


def _glyphs(compact):
    """Returns (k_polygons, hyphen_rect or None, (cx, cy, r_out, r_in))."""
    top, bottom = 7.2, 16.8
    stroke = 2.6 if compact else 2.2
    slope = 1.2  # |dy/dx| of the K arms.
    # Arm bands are parallel; each starts on the bar and ends flush with the
    # top (upper arm) or bottom (lower arm) edge.
    inner, outer = (10.0, 12.8) if compact else (9.9, 12.6)
    k_width = stroke + (outer - top) / slope
    gap = 1.0 if compact else 0.7
    hyphen_w, hyphen_h = (0, 0) if compact else (3.0, 1.7)
    ring_d = bottom - top
    total = k_width + gap + (hyphen_w + gap if hyphen_w else 0) + ring_d
    x = (24 - total) / 2

    bx = x + stroke
    bar = [(x, top), (bx, top), (bx, bottom), (x, bottom)]
    upper = [(bx, inner), (bx + (inner - top) / slope, top),
             (bx + (outer - top) / slope, top), (bx, outer)]
    lower = [(px, 24 - py) for (px, py) in upper]
    x += k_width + gap

    hyphen = None
    if hyphen_w:
        hyphen = (x, 12 - hyphen_h / 2, x + hyphen_w, 12 + hyphen_h / 2)
        x += hyphen_w + gap

    r_out = ring_d / 2
    ring = (x + r_out, 12.0, r_out, r_out - stroke)

    # Shrink the whole mark about the tile centre to leave a margin.
    g = 0.92 if compact else 0.84
    sc = lambda v: 12 + (v - 12) * g
    k_polys = [[(sc(px), sc(py)) for px, py in poly]
               for poly in (bar, upper, lower)]
    if hyphen:
        hyphen = tuple(sc(v) for v in hyphen)
    ring = (sc(ring[0]), sc(ring[1]), ring[2] * g, ring[3] * g)
    return k_polys, hyphen, ring


def _fmt(v):
    return ('%.3f' % v).rstrip('0').rstrip('.')


def _poly_path(points, scale, ox, oy):
    pts = ['%s,%s' % (_fmt(ox + px * scale), _fmt(oy + py * scale))
           for px, py in points]
    return 'M' + ' L'.join(pts) + ' Z'


def _glyph_svg(compact, scale, ox, oy, k_fill, o_fill):
    k_polys, hyphen, (cx, cy, r_out, r_in) = _glyphs(compact)
    parts = []
    for poly in k_polys:
        parts.append('<path fill="%s" d="%s"/>' %
                     (k_fill, _poly_path(poly, scale, ox, oy)))
    if hyphen:
        x0, y0, x1, y1 = hyphen
        parts.append(
            '<rect fill="%s" x="%s" y="%s" width="%s" height="%s"/>' %
            (k_fill, _fmt(ox + x0 * scale), _fmt(oy + y0 * scale),
             _fmt((x1 - x0) * scale), _fmt((y1 - y0) * scale)))
    c = (_fmt(ox + cx * scale), _fmt(oy + cy * scale))
    stroke = (r_out - r_in) * scale
    parts.append(
        '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" '
        'stroke-width="%s"/>' %
        (c[0], c[1], _fmt((r_out + r_in) / 2 * scale), o_fill, _fmt(stroke)))
    return '\n  '.join(parts)


_DEFS = '''<defs>
    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{t0}"/>
      <stop offset="1" stop-color="{t1}"/>
    </linearGradient>
    <linearGradient id="accent" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="{a0}"/>
      <stop offset="1" stop-color="{a1}"/>
    </linearGradient>
  </defs>'''


def tile_svg(variant='release', compact=False, inset=False):
    """Square K-O tile. `inset` follows the macOS app-icon grid (824 body)."""
    size = 1024
    body = 824 if inset else size
    off = (size - body) / 2
    radius = body * (0.225 if inset else 0.23)
    t0, t1 = TILES[variant]
    scale = body / 24
    return '\n'.join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" '
        'viewBox="0 0 1024 1024">',
        '  ' + _DEFS.format(t0=t0, t1=t1, a0=ACCENT[0], a1=ACCENT[1]),
        '  <rect x="%s" y="%s" width="%s" height="%s" rx="%s" '
        'fill="url(#tile)"/>' % (_fmt(off), _fmt(off), _fmt(body),
                                 _fmt(body), _fmt(radius)),
        '  ' + _glyph_svg(compact, scale, off, off, INK, 'url(#accent)'),
        '</svg>', ''
    ])


def wordmark_svg(white=False, mono=False):
    """Glyphs only, cropped to the mark's bounding box."""
    k_fill = '#FFFFFF' if white or mono else DARK_INK
    o_fill = k_fill if mono else 'url(#accent)'
    scale = 40
    k_polys, _, (cx, cy, r_out, _) = _glyphs(False)
    left = min(p[0] for poly in k_polys for p in poly)
    top = cy - r_out
    width = (cx + r_out - left) * scale
    height = 2 * r_out * scale
    return '\n'.join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%s" height="%s" '
        'viewBox="0 0 %s %s">' % (_fmt(width), _fmt(height), _fmt(width),
                                  _fmt(height)),
        '  ' + _DEFS.format(t0='#000', t1='#000', a0=ACCENT[0], a1=ACCENT[1]),
        '  ' + _glyph_svg(False, scale, -left * scale, -top * scale, k_fill,
                          o_fill), '</svg>', ''
    ])


def mono_svg():
    """22 px status-tray glyph: white strokes with a dark outline."""
    k_polys, _, (cx, cy, r_out, r_in) = _glyphs(True)
    scale = 22 / 24
    out = []
    for poly in k_polys:
        out.append('<path d="%s"/>' % _poly_path(poly, scale, 0, 0))
    ring = '<circle cx="%s" cy="%s" r="%s" fill="none" stroke-width="%s"/>' % (
        _fmt(cx * scale), _fmt(cy * scale), _fmt((r_out + r_in) / 2 * scale),
        _fmt((r_out - r_in) * scale))
    return '\n'.join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" '
        'viewBox="0 0 22 22">',
        '  <g fill="#FFFFFF" stroke="#262626" stroke-width="0.6" '
        'stroke-linejoin="round">',
        '    ' + '\n    '.join(out), '  </g>',
        '  <g stroke="#262626">' + ring.replace(
            'stroke-width="%s"' % _fmt((r_out - r_in) * scale),
            'stroke-width="%s"' % _fmt((r_out - r_in) * scale + 1.2)) + '</g>',
        '  <g stroke="#FFFFFF">' + ring + '</g>', '</svg>', ''
    ])


def dmg_background_svg():
    """602x320 Finder window backdrop; icons sit at (150,155) and (430,150)."""
    return '\n'.join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="602" height="320" '
        'viewBox="0 0 602 320">',
        '  <rect width="602" height="320" fill="#ECEDF0"/>',
        '  <rect x="24" y="24" width="554" height="272" rx="10" '
        'fill="#FFFFFF" stroke="#DADCE0"/>',
        '  <path d="M270 153 H310 M294 137 L310 153 L294 169" fill="none" '
        'stroke="#5F6368" stroke-width="3.5" stroke-linecap="round" '
        'stroke-linejoin="round"/>', '</svg>', ''
    ])


def vector_icon(compact):
    """Chromium .icon (24 dip) version of the tile, for omnibox/menus."""
    k_polys, hyphen, (cx, cy, r_out, r_in) = _glyphs(compact)
    t0 = TILES['release'][1]
    lines = [
        '// Copyright (c) 2026 The Brave Authors. All rights reserved.',
        '// This Source Code Form is subject to the terms of the Mozilla '
        'Public',
        '// License, v. 2.0. If a copy of the MPL was not distributed with '
        'this file,', '// You can obtain one at https://mozilla.org/MPL/2.0/.',
        '// Generated by //brave/script/ko/generate_ko_icons.py.', '',
        'CANVAS_DIMENSIONS, 24,', 'NEW_PATH,',
        'PATH_COLOR_ARGB, 0xFF, 0x%s, 0x%s, 0x%s,' %
        (t0[1:3], t0[3:5], t0[5:7]), 'ROUND_RECT, 0, 0, 24, 24, 5.5f,'
    ]

    def f(v):
        # The commands are pasted into C++: "12f" is not a valid literal.
        s = _fmt(v)
        return s + 'f' if '.' in s else s

    for poly in k_polys:
        lines += ['NEW_PATH,', 'PATH_COLOR_ARGB, 0xFF, 0xF4, 0xF5, 0xF7,']
        lines.append('MOVE_TO, %s, %s,' % (f(poly[0][0]), f(poly[0][1])))
        for px, py in poly[1:]:
            lines.append('LINE_TO, %s, %s,' % (f(px), f(py)))
        lines.append('CLOSE,')
    if hyphen:
        x0, y0, x1, y1 = hyphen
        lines += [
            'NEW_PATH,', 'PATH_COLOR_ARGB, 0xFF, 0xF4, 0xF5, 0xF7,',
            'MOVE_TO, %s, %s,' % (f(x0), f(y0)),
            'H_LINE_TO, %s,' % f(x1), 'V_LINE_TO, %s,' % f(y1),
            'H_LINE_TO, %s,' % f(x0), 'CLOSE,'
        ]
    lines += [
        'NEW_PATH,', 'PATH_COLOR_ARGB, 0xFF, 0xF7, 0x4F, 0x45,',
        'CIRCLE, %s, %s, %s,' % (f(cx), f(cy), f(r_out)), 'NEW_PATH,',
        'PATH_COLOR_ARGB, 0xFF, 0x%s, 0x%s, 0x%s,' %
        (t0[1:3], t0[3:5], t0[5:7]),
        'CIRCLE, %s, %s, %s' % (f(cx), f(cy), f(r_in))
    ]
    return '\n'.join(lines) + '\n'


def write_sources():
    os.makedirs(SOURCE, exist_ok=True)
    files = {
        os.path.join(KO, 'product_logo.svg'): tile_svg(),
        os.path.join(KO, 'product_logo_animation.svg'): tile_svg(),
        os.path.join(SOURCE, 'product_logo_development.svg'):
        tile_svg('development'),
        os.path.join(SOURCE, 'product_logo_compact.svg'):
        tile_svg(compact=True),
        os.path.join(SOURCE, 'product_logo_compact_development.svg'):
        tile_svg('development', compact=True),
        os.path.join(SOURCE, 'app_icon.svg'): tile_svg(inset=True),
        os.path.join(SOURCE, 'app_icon_development.svg'):
        tile_svg('development', inset=True),
        os.path.join(SOURCE, 'wordmark.svg'): wordmark_svg(),
        os.path.join(SOURCE, 'wordmark_white.svg'): wordmark_svg(white=True),
        os.path.join(SOURCE, 'mono_22.svg'): mono_svg(),
        os.path.join(SOURCE, 'dmg_background.svg'): dmg_background_svg(),
    }
    icons = {
        ('components', 'vector_icons', 'ko', 'product.icon'): True,
        ('components', 'vector_icons', 'ko', 'product_refresh.icon'): True,
        ('components', 'omnibox', 'browser', 'vector_icons', 'ko',
         'product.icon'): True,
        ('components', 'omnibox', 'browser', 'vector_icons', 'ko',
         'product_chrome_refresh.icon'): True,
    }
    for parts, compact in icons.items():
        files[os.path.join(BRAVE_DIR, *parts)] = vector_icon(compact)
    for path, content in files.items():
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(content)


# --- Rasterization ------------------------------------------------------------


def _png(svg_path, out, width, height=None):
    import cairosvg  # pylint: disable=import-outside-toplevel
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cairosvg.svg2png(url=svg_path,
                     write_to=out,
                     output_width=width,
                     output_height=height or width)
    # Drop timestamps/metadata so re-renders are byte-stable.
    subprocess.check_call(['convert', out, '-strip', 'PNG32:' + out])


def _wordmark(svg, out, box):
    """Renders the wordmark at ~68% of the box height, left aligned."""
    w, h = box
    glyph_h = round(h * 0.68)
    # Wordmark aspect ratio from its SVG viewBox.
    with open(svg, encoding='utf-8') as f:
        head = f.read(200)
    vw = float(head.split('width="')[1].split('"')[0])
    vh = float(head.split('height="')[1].split('"')[0])
    glyph_w = min(round(glyph_h * vw / vh), w - 2)
    tmp = out + '.tmp.png'
    _png(svg, tmp, glyph_w, glyph_h)
    subprocess.check_call([
        'convert', '-size',
        '%dx%d' % box, 'xc:none', tmp, '-gravity', 'west', '-geometry',
        '+%d+0' % max(1, round(h * 0.08)), '-composite', '-strip',
        'PNG32:' + out
    ])
    os.remove(tmp)


def _icns(entries, out):
    """Packs PNG-encoded entries {ostype: png_path} into an .icns file."""
    chunks = b''
    for ostype, path in entries:
        with open(path, 'rb') as f:
            data = f.read()
        chunks += ostype.encode('ascii') + struct.pack('>I',
                                                       len(data) + 8) + data
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(b'icns' + struct.pack('>I', len(chunks) + 8) + chunks)


ICNS_TYPES = [('icp4', 16), ('icp5', 32), ('ic11', 32), ('icp6', 64),
              ('ic12', 64), ('ic07', 128), ('ic08', 256), ('ic13', 256),
              ('ic09', 512), ('ic14', 512), ('ic10', 1024)]

APPICONSET = [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2),
              (256, 1), (256, 2), (512, 1), (512, 2)]


def _mac_icons(svg, icns_out, xcassets_dir, tmp):
    rendered = {}
    for _, px in ICNS_TYPES:
        if px not in rendered:
            rendered[px] = os.path.join(tmp, '%s_%d.png' %
                                        (os.path.basename(svg), px))
            _png(svg, rendered[px], px)
    _icns([(t, rendered[px]) for t, px in ICNS_TYPES], icns_out)
    if not xcassets_dir:
        return
    appiconset = os.path.join(xcassets_dir, 'AppIcon.appiconset')
    if os.path.isdir(appiconset):
        shutil.rmtree(appiconset)
    os.makedirs(appiconset)
    images = []
    for size, scale in APPICONSET:
        name = 'icon_%dx%d%s.png' % (size, size, '@2x' if scale == 2 else '')
        _png(svg, os.path.join(appiconset, name), size * scale)
        images.append({
            'idiom': 'mac',
            'size': '%dx%d' % (size, size),
            'scale': '%dx' % scale,
            'filename': name
        })
    contents = {'images': images, 'info': {'version': 1, 'author': 'xcode'}}
    with open(os.path.join(appiconset, 'Contents.json'), 'w') as f:
        json.dump(contents, f, indent=2)
        f.write('\n')
    with open(os.path.join(xcassets_dir, 'Contents.json'), 'w') as f:
        json.dump({'info': {'version': 1, 'author': 'xcode'}}, f, indent=2)
        f.write('\n')


def render():
    src = lambda name: os.path.join(SOURCE, name)
    full = {
        'release': os.path.join(KO, 'product_logo.svg'),
        'development': src('product_logo_development.svg'),
    }
    compact = {
        'release': src('product_logo_compact.svg'),
        'development': src('product_logo_compact_development.svg'),
    }

    def pick(px, variant):
        return (compact if px <= 32 else full)[variant]

    channels = ['', '_beta', '_dev', '_development', '_nightly']

    def variant_of(channel):
        return 'development' if channel == '_development' else 'release'

    # Unscaled logos in app/theme/ko and app/theme/ko/linux.
    for px in (24, 48, 64, 128, 256):
        _png(pick(px, 'release'), os.path.join(KO, 'product_logo_%d.png' % px),
             px)
    for ch in channels[1:]:
        _png(full[variant_of(ch)],
             os.path.join(KO, 'product_logo_128%s.png' % ch), 128)
    for px in (24, 48, 64, 128, 256):
        for ch in channels:
            if ch == '_development' and px != 128:
                continue
            _png(pick(px, variant_of(ch)),
                 os.path.join(KO, 'linux', 'product_logo_%d%s.png' % (px, ch)),
                 px)
    _png(src('mono_22.svg'), os.path.join(KO, 'product_logo_22_mono.png'), 22)

    # Scaled logos.
    for scale, root in ((1, P100), (2, P200)):
        for base in (16, 32):
            for ch in channels:
                if scale == 1 and base == 16 and ch:
                    continue  # Only the plain 16 px exists at 100%.
                _png(pick(base * scale, variant_of(ch)),
                     os.path.join(root, 'product_logo_%d%s.png' % (base, ch)),
                     base * scale)
        if scale == 1:
            for base in (16, 32):
                for ch in channels:
                    if base == 16 and ch == '_development':
                        continue
                    _png(pick(base, variant_of(ch)),
                         os.path.join(root, 'linux',
                                      'product_logo_%d%s.png' % (base, ch)),
                         base)
        boxes = {
            'product_logo_name_22.png': (70, 22),
            'product_logo_name_22_white.png': (70, 22),
            'product_logo_name_48.png': (154, 48),
            'product_logo_white.png': (184, 58),
        }
        for name, (w, h) in boxes.items():
            svg = src('wordmark_white.svg' if 'white' in name else
                      'wordmark.svg')
            _wordmark(svg, os.path.join(root, name), (w * scale, h * scale))

    with tempfile.TemporaryDirectory() as tmp:
        # macOS app icons and their asset-catalog sources.
        _mac_icons(src('app_icon.svg'), os.path.join(KO, 'mac', 'app.icns'),
                   os.path.join(KO, 'mac', 'Assets.xcassets'), tmp)
        os.makedirs(os.path.join(KO, 'mac', 'development'), exist_ok=True)
        _mac_icons(src('app_icon_development.svg'),
                   os.path.join(KO, 'mac', 'development', 'app.icns'),
                   os.path.join(KO, 'mac', 'development', 'Assets.xcassets'),
                   tmp)
        # DMG volume icons.
        _mac_icons(src('app_icon.svg'), os.path.join(MAC_BUILD, 'dmg.ko.icns'),
                   None, tmp)
        _mac_icons(src('app_icon_development.svg'),
                   os.path.join(MAC_BUILD, 'dmg-development.ko.icns'), None,
                   tmp)

        # Windows .ico (not wired: K-O branding is macOS/Linux only).
        ico_pngs = []
        for px in (16, 24, 32, 48, 64, 128, 256):
            p = os.path.join(tmp, 'ico_%d.png' % px)
            _png(pick(px, 'release'), p, px)
            ico_pngs.append(p)
        os.makedirs(os.path.join(KO, 'win'), exist_ok=True)
        subprocess.check_call(['convert'] + ico_pngs +
                              [os.path.join(KO, 'win', 'ko.ico')])

    _png(src('dmg_background.svg'),
         os.path.join(MAC_BUILD, 'dmg-background.ko.png'), 602, 320)

    # First-run welcome page logo (shown at 150 px).
    welcome = os.path.join(WELCOME_ASSETS, 'ko_logo@2x.webp')
    _png(full['release'], welcome + '.png', 300)
    subprocess.check_call([
        'convert', welcome + '.png', '-strip', '-define', 'webp:lossless=true',
        welcome
    ])
    os.remove(welcome + '.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('command', choices=['svg', 'render', 'all'])
    args = parser.parse_args()
    if args.command in ('svg', 'all'):
        write_sources()
    if args.command in ('render', 'all'):
        render()
    return 0


if __name__ == '__main__':
    sys.exit(main())
