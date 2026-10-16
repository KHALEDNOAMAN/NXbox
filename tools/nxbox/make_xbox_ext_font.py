# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Xbox shared extension font, without firmware or external artwork.

Run: uv run --with fonttools python tools/nxbox/make_xbox_ext_font.py

Cmap inspection and an outline contact sheet of the repository's substitute confirm:
  E0A0/E0E0 A, E0A1/E0E1 B, E0A2/E0E2 X, E0A3/E0E3 Y
  E0A4/E0E4 L -> LB, E0A5/E0E5 R -> RB
  E0A6/E0E6 ZL -> LT, E0A7/E0E7 ZR -> RT
  E0B3/E0EF + -> Menu, E0B4/E0F0 - -> View
The first codepoint is outlined, the second filled. Both become Xbox designs.
E0A8/E0E8 and E0A9/E0E9 are SL/SR, NOT stick clicks; leave them untouched.

The substitute has NO Home or stick-click glyphs. Add these canonical mappings:
  E0B9/E0F4 Home -> Xbox Guide, E0C4/E104 left click -> LS,
  E0C5/E105 right click -> RS.
Mapping reference (code, not font data):
https://gist.github.com/LauraWebdev/f73aab5b1ff2b2f9136d131b1b020907
Added glyphs borrow A's advance, bearing and vertical bounds; existing metrics stay
unchanged. The preview explicitly labels missing originals. No proprietary font,
lettering, artwork, raster library, or system font is needed.
"""

import io
import math
import re
import struct
import zlib
from itertools import pairwise
from pathlib import Path

from fontTools.pens.basePen import BasePen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import Glyph

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "src/core/file_sys/system_archive/data"
REPLACEMENTS = {
    **{0xE0A0 + i: label for i, label in enumerate(("A", "B", "X", "Y", "LB", "RB", "LT", "RT"))},
    **{0xE0E0 + i: label for i, label in enumerate(("A", "B", "X", "Y", "LB", "RB", "LT", "RT"))},
    0xE0B3: "Menu",
    0xE0EF: "Menu",
    0xE0B4: "View",
    0xE0F0: "View",
}
ADDITIONS = {0xE0B9: "Home", 0xE0F4: "Home", 0xE0C4: "LS", 0xE104: "LS", 0xE0C5: "RS", 0xE105: "RS"}
BUTTONS = dict(sorted(REPLACEMENTS.items() | ADDITIONS.items()))


def read_array(path):
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", path.read_text()))


def polygon(pen, points):
    pen.moveTo(points[0])
    for point in points[1:]:
        pen.lineTo(point)
    pen.closePath()


def rounded(pen, x, y, w, h, r):
    # Clockwise outer contour, with quadratic rounded corners.
    pen.moveTo((x + r, y))
    pen.qCurveTo((x, y), (x, y + r))
    pen.lineTo((x, y + h - r))
    pen.qCurveTo((x, y + h), (x + r, y + h))
    pen.lineTo((x + w - r, y + h))
    pen.qCurveTo((x + w, y + h), (x + w, y + h - r))
    pen.lineTo((x + w, y + r))
    pen.qCurveTo((x + w, y), (x + w - r, y))
    pen.closePath()


def circle(pen):
    # Four quadratic pairs per circle approximate a smooth sphere.
    radius = 480
    pen.moveTo((512, 388 - radius))
    for i in range(8):
        angle = -math.pi / 2 - (i + 1) * math.pi / 4
        middle = angle + math.pi / 8
        pen.qCurveTo(
            (
                512 + radius / math.cos(math.pi / 8) * math.cos(middle),
                388 + radius / math.cos(math.pi / 8) * math.sin(middle),
            ),
            (512 + radius * math.cos(angle), 388 + radius * math.sin(angle)),
        )
    pen.closePath()


# Original geometric sans lettering in a 100 x 140 design grid. Outer contours
# are clockwise; counters are counterclockwise. Cutouts reverse both together.
LETTERS = {
    "A": [
        [(0, 0), (38, 140), (62, 140), (100, 0), (75, 0), (65, 38), (35, 38), (25, 0)],
        [(41, 61), (59, 61), (50, 102)],
    ],
    "B": [
        [(0, 0), (0, 140), (65, 140), (91, 120), (91, 88), (77, 73), (98, 54), (98, 22), (72, 0)],
        [(24, 83), (61, 83), (68, 92), (68, 110), (59, 117), (24, 117)],
        [(24, 23), (62, 23), (74, 31), (74, 51), (62, 60), (24, 60)],
    ],
    "X": [
        [
            (0, 0),
            (34, 72),
            (2, 140),
            (29, 140),
            (50, 94),
            (71, 140),
            (98, 140),
            (66, 72),
            (100, 0),
            (73, 0),
            (50, 49),
            (27, 0),
        ]
    ],
    "Y": [
        [(38, 0), (38, 58), (0, 140), (27, 140), (50, 86), (73, 140), (100, 140), (62, 58), (62, 0)]
    ],
    "L": [[(0, 0), (0, 140), (25, 140), (25, 24), (100, 24), (100, 0)]],
    "R": [
        [
            (0, 0),
            (0, 140),
            (66, 140),
            (96, 117),
            (96, 82),
            (73, 62),
            (100, 0),
            (72, 0),
            (44, 58),
            (24, 58),
            (24, 0),
        ],
        [(24, 82), (59, 82), (72, 92), (72, 108), (59, 117), (24, 117)],
    ],
    "T": [[(38, 0), (38, 116), (0, 116), (0, 140), (100, 140), (100, 116), (62, 116), (62, 0)]],
    "S": [
        [
            (0, 0),
            (0, 24),
            (68, 24),
            (76, 32),
            (76, 48),
            (66, 57),
            (26, 57),
            (0, 80),
            (0, 116),
            (26, 140),
            (100, 140),
            (100, 116),
            (34, 116),
            (24, 108),
            (24, 91),
            (34, 81),
            (74, 81),
            (100, 59),
            (100, 24),
            (74, 0),
        ]
    ],
}


def lettering(pen, label):
    scale = 3.7 if len(label) == 1 else 2.45
    width = (len(label) * 100 + (len(label) - 1) * 18) * scale
    for i, letter in enumerate(label):
        target = TransformPen(
            ReverseContourPen(pen),
            (scale, 0, 0, scale, 512 - width / 2 + i * 118 * scale, 388 - 70 * scale),
        )
        for points in LETTERS[letter]:
            polygon(target, points)


def make_glyph(label, bounds):
    pen = TTGlyphPen(None)
    xmin, ymin, xmax, ymax = bounds
    # Match each original icon's vertical footprint, not just the font ascender.
    pen_at_bounds = TransformPen(
        pen,
        (
            (xmax - xmin) / 984,
            0,
            0,
            (ymax - ymin) / 984,
            xmin - 20 * (xmax - xmin) / 984,
            ymin + 104 * (ymax - ymin) / 984,
        ),
    )
    p = pen_at_bounds
    if label in ("LB", "RB", "LT", "RT"):
        rounded(p, 20, 88, 984, 600, 300)
    else:
        circle(p)
    cut = ReverseContourPen(p)
    if label in LETTERS or label in ("LB", "RB", "LT", "RT", "LS", "RS"):
        lettering(p, label)
    elif label == "Menu":
        for y in (188, 348, 508):
            rounded(cut, 262, y, 500, 80, 28)
    elif label == "View":
        # Two overlapping window frames, cut as a single union (no fill overlap).
        polygon(
            cut,
            list(
                reversed(
                    [
                        (232, 318),
                        (352, 318),
                        (352, 198),
                        (792, 198),
                        (792, 558),
                        (672, 558),
                        (672, 678),
                        (232, 678),
                    ]
                )
            ),
        )
        rounded(p, 292, 378, 60, 240, 0)
        rounded(p, 412, 258, 320, 240, 0)
        rounded(p, 412, 558, 200, 60, 0)
    else:
        # Swept X cut into a sphere, with curved arms and a broad lower opening.
        cut.moveTo((222, 718))
        cut.qCurveTo((350, 718), (512, 548))
        cut.qCurveTo((674, 718), (802, 718))
        cut.qCurveTo((670, 650), (580, 442))
        cut.qCurveTo((750, 220), (792, 68))
        cut.qCurveTo((650, 225), (512, 350))
        cut.qCurveTo((374, 225), (232, 68))
        cut.qCurveTo((274, 220), (444, 442))
        cut.qCurveTo((354, 650), (222, 718))
        cut.closePath()
    return pen.glyph()


def build_font():
    font = TTFont(
        io.BytesIO(read_array(DATA / "font_nintendo_extended.cpp")), recalcTimestamp=False
    )
    # The source pads every glyf record to four bytes. Preserve those bytes.
    font["glyf"].padding = 4
    cmap = font.getBestCmap()
    replaced_names = {cmap[cp] for cp in REPLACEMENTS}
    offsets = font["loca"].locations
    source_glyf = font.reader["glyf"]
    preserved_glyphs = {
        name: source_glyf[offsets[i] : offsets[i + 1]]
        for i, name in enumerate(font.getGlyphOrder())
        if name not in replaced_names
    }
    for cp, label in BUTTONS.items():
        if cp in cmap:
            name = cmap[cp]
            old = font["glyf"][name]
            bounds = old.xMin, old.yMin, old.xMax, old.yMax
        else:
            name = f"uni{cp:04X}"
            font.setGlyphOrder([*font.getGlyphOrder(), name])
            font["hmtx"].metrics[name] = font["hmtx"].metrics[cmap[0xE0A0]]
            bounds = (20, -104, 1004, 880)
            for table in font["cmap"].tables:
                if table.isUnicode():
                    table.cmap[cp] = name
        font["glyf"][name] = make_glyph(label, bounds)
    font["OS/2"].usLastCharIndex = max(font.getBestCmap())
    # Recompute glyph limits, but retain the source's global layout box.
    head_bounds = {key: getattr(font["head"], key) for key in ("xMin", "yMin", "xMax", "yMax")}
    for cp in BUTTONS:
        font["glyf"][font.getBestCmap()[cp]].recalcBounds(font["glyf"])
    font["maxp"].recalc(font)
    for key, value in head_bounds.items():
        setattr(font["head"], key, value)
    # maxp inspection expands every glyph. Restore the original serialized records
    # afterward so fontTools cannot optimize flags or contour encodings in them.
    for name, data in preserved_glyphs.items():
        font["glyf"][name] = Glyph(data)
    font.recalcBBoxes = False
    output = io.BytesIO()
    font.save(output)
    return output.getvalue()


class FlattenPen(BasePen):
    """Flatten quadratic TrueType contours for the dependency-free PNG preview."""

    def __init__(self, glyph_set):
        super().__init__(glyph_set)
        self.contours = []

    def _moveTo(self, point):
        self.contours.append([point])

    def _lineTo(self, point):
        self.contours[-1].append(point)

    def _qCurveToOne(self, control, end):
        start = self._getCurrentPoint()
        for i in range(1, 25):
            t = i / 24
            self._lineTo(
                tuple(
                    (1 - t) ** 2 * start[j] + 2 * (1 - t) * t * control[j] + t * t * end[j]
                    for j in (0, 1)
                )
            )

    def _closePath(self):
        self.contours[-1].append(self.contours[-1][0])


def preview(original, generated, path):
    # Render at 2x and box-filter to keep small curves legible.
    width, height, zoom = 1200, 80 + 120 * len(BUTTONS), 2
    pixels = bytearray([255]) * (width * height * zoom * zoom)
    stride = width * zoom

    def draw(font, name, x, y, scale):
        pen = FlattenPen(font.getGlyphSet())
        font.getGlyphSet()[name].draw(pen)
        edges = []
        for contour in pen.contours:
            points = [
                (x * zoom + a * scale * zoom, y * zoom - b * scale * zoom) for a, b in contour
            ]
            edges.extend(pairwise(points))
        for row in range(
            max(0, int(min(a[1] for a, b in edges))),
            min(height * zoom, math.ceil(max(a[1] for a, b in edges))),
        ):
            crossings = []
            for (ax, ay), (bx, by) in edges:
                if min(ay, by) <= row + 0.5 < max(ay, by):
                    crossings.append(
                        (ax + (row + 0.5 - ay) * (bx - ax) / (by - ay), 1 if by > ay else -1)
                    )
            winding, previous = 0, 0
            for xcross, delta in sorted(crossings):
                if winding:
                    left, right = (
                        max(0, math.ceil(previous - 0.5)),
                        min(stride, math.ceil(xcross - 0.5)),
                    )
                    pixels[row * stride + left : row * stride + right] = bytes(right - left)
                winding += delta
                previous = xcross

    # Tiny vector text uses a built-in 5x7 bitmap alphabet, encoded as row bits.
    alphabet = {
        "0": [14, 17, 19, 21, 25, 17, 14],
        "1": [4, 12, 4, 4, 4, 4, 14],
        "2": [14, 17, 1, 2, 4, 8, 31],
        "3": [30, 1, 1, 14, 1, 1, 30],
        "4": [2, 6, 10, 18, 31, 2, 2],
        "5": [31, 16, 16, 30, 1, 1, 30],
        "6": [14, 16, 16, 30, 17, 17, 14],
        "7": [31, 1, 2, 4, 8, 8, 8],
        "8": [14, 17, 17, 14, 17, 17, 14],
        "9": [14, 17, 17, 15, 1, 1, 14],
        "A": [14, 17, 17, 31, 17, 17, 17],
        "B": [30, 17, 17, 30, 17, 17, 30],
        "C": [14, 17, 16, 16, 16, 17, 14],
        "D": [30, 17, 17, 17, 17, 17, 30],
        "E": [31, 16, 16, 30, 16, 16, 31],
        "F": [31, 16, 16, 30, 16, 16, 16],
        "G": [14, 17, 16, 23, 17, 17, 15],
        "H": [17, 17, 17, 31, 17, 17, 17],
        "I": [14, 4, 4, 4, 4, 4, 14],
        "L": [16, 16, 16, 16, 16, 16, 31],
        "M": [17, 27, 21, 21, 17, 17, 17],
        "N": [17, 25, 25, 21, 19, 19, 17],
        "O": [14, 17, 17, 17, 17, 17, 14],
        "R": [30, 17, 17, 30, 20, 18, 17],
        "S": [15, 16, 16, 14, 1, 1, 30],
        "T": [31, 4, 4, 4, 4, 4, 4],
        "U": [17, 17, 17, 17, 17, 17, 14],
        "V": [17, 17, 17, 17, 17, 10, 4],
        "W": [17, 17, 17, 21, 21, 21, 10],
        "X": [17, 17, 10, 4, 10, 17, 17],
        "Y": [17, 17, 10, 4, 4, 4, 4],
    }

    def text(x, y, value):
        for char in value.upper():
            for row, bits in enumerate(alphabet.get(char, [0] * 7)):
                for col in range(5):
                    if bits & (16 >> col):
                        for dy in range(4):
                            start = (y * zoom + row * 4 + dy) * stride + x * zoom + col * 4
                            pixels[start : start + 4] = bytes(4)
            x += 14

    text(20, 25, "CODE / BUTTON")
    text(460, 25, "ORIGINAL")
    text(880, 25, "XBOX")
    for row, (cp, label) in enumerate(BUTTONS.items()):
        y = 80 + row * 120
        text(20, y + 35, f"{cp:04X} {label}")
        if cp in original.getBestCmap():
            draw(original, original.getBestCmap()[cp], 480, y + 90, 0.09)
        else:
            text(440, y + 35, "MISSING")
        draw(generated, generated.getBestCmap()[cp], 880, y + 90, 0.09)
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            index = y * 2 * stride + x * 2
            rows.append(
                sum(pixels[index + dy * stride + dx] for dy in (0, 1) for dx in (0, 1)) // 4
            )

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 9))
        + chunk(b"IEND", b"")
    )


def main():
    data = build_font()
    symbol = "FONT_NINTENDO_EXTENDED_XBOX"
    prefix = "// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project\n// SPDX-License-Identifier: GPL-2.0-or-later\n// Generated by tools/nxbox/make_xbox_ext_font.py; do not edit.\n\n"
    namespace = "FileSys::SystemArchive::SharedFontData"
    (DATA / "font_nintendo_extended_xbox.h").write_text(
        prefix
        + "#pragma once\n\n#include <array>\n\n"
        + f"namespace {namespace} {{\n\n"
        + f"extern const std::array<unsigned char, {len(data)}> {symbol};\n\n"
        + f"}} // namespace {namespace}\n"
    )
    lines = [
        "    " + ", ".join(f"0x{b:02X}" for b in data[i : i + 16]) + ","
        for i in range(0, len(data), 16)
    ]
    (DATA / "font_nintendo_extended_xbox.cpp").write_text(
        prefix
        + '#include "core/file_sys/system_archive/data/font_nintendo_extended_xbox.h"\n\n'
        + f"namespace {namespace} {{\n\nconst std::array<unsigned char, {len(data)}> {symbol}"
        + "{{\n"
        + "\n".join(lines)
        + "\n}};\n\n"
        + f"}} // namespace {namespace}\n"
    )
    preview(
        TTFont(io.BytesIO(read_array(DATA / "font_nintendo_extended.cpp"))),
        TTFont(io.BytesIO(data)),
        ROOT / "xbox_icons_preview.png",
    )
    print(
        f"Generated {len(data)} bytes, {len(REPLACEMENTS)} replaced and {len(ADDITIONS)} added glyphs."
    )


if __name__ == "__main__":
    main()
