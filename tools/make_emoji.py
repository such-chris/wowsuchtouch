#!/usr/bin/env python3
"""Turns Twemoji emoji into LVGL images for the knob: writes firmware/src/emoji.c / .h

Needs Pillow (pip3 install pillow). Run from the repo folder:  python3 tools/make_emoji.py
To add an emoji: find its code (e.g. 2b50 for the star) at https://github.com/jdecked/twemoji
(assets/72x72/<code>.png), add a line to EMOJI, run this, then use it in display_task.cpp.
Twemoji is CC-BY 4.0 (credit: Twitter / jdecked).
"""
import io, os, urllib.request
from PIL import Image

EMOJI = [  # (code, name in the firmware, size in pixels)
    ('1f534', 'red', 30), ('1f7e1', 'yellow', 30), ('1f7e2', 'green', 30), ('1f535', 'blue', 30),
    ('1f7e3', 'purple', 30), ('2b50', 'star', 30), ('2b50', 'star_small', 22), ('1f3a8', 'palette_small', 22),
]
URL = 'https://raw.githubusercontent.com/jdecked/twemoji/main/assets/72x72/{}.png'
OUT = os.path.join(os.path.dirname(__file__), '..', 'firmware', 'src')

c = ['/* Emoji images (Twemoji by Twitter / jdecked, CC-BY 4.0: https://github.com/jdecked/twemoji)',
     ' * converted to LVGL ARGB8888 images. Made by tools/make_emoji.py; edit that, not this file. */',
     '#include "lvgl.h"', '#include "emoji.h"', '']
h = ['#pragma once', '', '#include "lvgl.h"', '', '#ifdef __cplusplus', 'extern "C" {', '#endif', '']
for code, name, size in EMOJI:
    png = urllib.request.urlopen(URL.format(code)).read()
    im = Image.open(io.BytesIO(png)).convert('RGBA').resize((size, size), Image.LANCZOS)
    data = bytearray()
    for r, g, b, a in im.getdata():
        data += bytes((b, g, r, a))   # LVGL ARGB8888 is stored B, G, R, A
    c.append(f'static const uint8_t emoji_{name}_map[] = {{')
    for i in range(0, len(data), 24):
        c.append('    ' + ', '.join(f'0x{x:02x}' for x in data[i:i + 24]) + ',')
    c += ['};', f'const lv_image_dsc_t emoji_{name} = {{',
          '    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .flags = 0,',
          f'                .w = {size}, .h = {size}, .stride = {size * 4} }},',
          f'    .data_size = {len(data)},', f'    .data = emoji_{name}_map,', '};', '']
    h.append(f'extern const lv_image_dsc_t emoji_{name};  // {size} px')
h += ['', '#ifdef __cplusplus', '}', '#endif', '']
open(os.path.join(OUT, 'emoji.c'), 'w').write('\n'.join(c))
open(os.path.join(OUT, 'emoji.h'), 'w').write('\n'.join(h))
print('wrote firmware/src/emoji.c and emoji.h')
