#!/usr/bin/env python3
"""
Generate SevenTTY Symbols font as Rez source (FOND + NFNT resources).

Each glyph is defined algorithmically based on cell dimensions.
Box drawing = lines at cell center extending to edges.
Block elements = filled rectangles at fractional positions.
Shading = dither patterns.

Usage: python3 generate_symbolfont.py > ../symbolfont.r
"""

import sys
import math

# Monaco metrics measured from real Mac OS (QEMU Quadra 800, System 8.1)
# Format: (pt_size, cell_width, cell_height, ascent, descent, leading)
MONACO_METRICS = [
    (9,  6,  12,  9,  2, 0),
    (10, 6,  14, 10,  2, 1),
    (12, 7,  17, 12,  3, 1),
    (14, 8,  19, 14,  3, 1),
    (18, 11, 25, 18,  4, 2),
    (24, 14, 33, 24,  6, 2),
    (36, 22, 49, 36,  9, 3),
]

# Resource IDs
FOND_ID = 200
NFNT_BASE_ID = 200  # 200-206 for sizes 9,10,12,14,18,24,36

# Font character range
FIRST_CHAR = 0x20
LAST_CHAR = 0xAD

# ---------------------------------------------------------------------------
# Glyph definitions: Unicode codepoint -> symbol font char code
# ---------------------------------------------------------------------------

# Box Drawing (0x20-0x4F)
GLYPH_MAP = {
    # Single line box drawing
    0x2500: 0x9B,  # ─ horizontal (NOT 0x20 — Font Manager skips space char)
    0x2502: 0x21,  # │ vertical
    0x250C: 0x22,  # ┌ down-right
    0x2510: 0x23,  # ┐ down-left
    0x2514: 0x24,  # └ up-right
    0x2518: 0x25,  # ┘ up-left
    0x251C: 0x26,  # ├ vertical-right
    0x2524: 0x27,  # ┤ vertical-left
    0x252C: 0x28,  # ┬ horizontal-down
    0x2534: 0x29,  # ┴ horizontal-up
    0x253C: 0x2A,  # ┼ cross

    # Double line box drawing
    0x2550: 0x2B,  # ═ double horizontal
    0x2551: 0x2C,  # ║ double vertical
    0x2554: 0x2D,  # ╔ double down-right
    0x2557: 0x2E,  # ╗ double down-left
    0x255A: 0x2F,  # ╚ double up-right
    0x255D: 0x30,  # ╝ double up-left
    0x2560: 0x31,  # ╠ double vertical-right
    0x2563: 0x32,  # ╣ double vertical-left
    0x2566: 0x33,  # ╦ double horizontal-down
    0x2569: 0x34,  # ╩ double horizontal-up
    0x256C: 0x35,  # ╬ double cross

    # Mixed single/double
    0x2552: 0x36,  # ╒ down-single right-double
    0x2553: 0x37,  # ╓ down-double right-single
    0x2555: 0x38,  # ╕ down-single left-double
    0x2556: 0x39,  # ╖ down-double left-single
    0x2558: 0x3A,  # ╘ up-single right-double
    0x2559: 0x3B,  # ╙ up-double right-single
    0x255B: 0x3C,  # ╛ up-single left-double
    0x255C: 0x3D,  # ╜ up-double left-single
    0x255E: 0x3E,  # ╞ vertical-single right-double
    0x255F: 0x3F,  # ╟ vertical-double right-single
    0x2561: 0x40,  # ╡ vertical-single left-double
    0x2562: 0x41,  # ╢ vertical-double left-single
    0x2564: 0x42,  # ╤ down-single horizontal-double
    0x2565: 0x43,  # ╥ down-double horizontal-single
    0x2567: 0x44,  # ╧ up-single horizontal-double
    0x2568: 0x45,  # ╨ up-double horizontal-single
    0x256A: 0x46,  # ╪ vertical-single horizontal-double
    0x256B: 0x47,  # ╫ vertical-double horizontal-single

    # Half lines and heavy
    0x2574: 0x48,  # ╴ light left
    0x2575: 0x49,  # ╵ light up
    0x2576: 0x4A,  # ╶ light right
    0x2577: 0x4B,  # ╷ light down
    0x2501: 0x4C,  # ━ heavy horizontal
    0x2503: 0x4D,  # ┃ heavy vertical
    0x254B: 0x4E,  # ╋ heavy cross
    0x2504: 0x4F,  # ┄ triple dash horizontal

    # Block Elements (0x50-0x5F)
    0x2580: 0x50,  # ▀ upper half
    0x2581: 0x51,  # ▁ lower 1/8
    0x2582: 0x52,  # ▂ lower 1/4
    0x2583: 0x53,  # ▃ lower 3/8
    0x2584: 0x54,  # ▄ lower half
    0x2585: 0x55,  # ▅ lower 5/8
    0x2586: 0x56,  # ▆ lower 3/4
    0x2587: 0x57,  # ▇ lower 7/8
    0x2588: 0x58,  # █ full block
    0x2589: 0x59,  # ▉ left 7/8
    0x258A: 0x5A,  # ▊ left 3/4
    0x258B: 0x5B,  # ▋ left 5/8
    0x258C: 0x5C,  # ▌ left half
    0x258D: 0x5D,  # ▍ left 3/8
    0x258E: 0x5E,  # ▎ left 1/4
    0x258F: 0x5F,  # ▏ left 1/8

    # Shading + geometric (0x60-0x6F)
    0x2590: 0x60,  # ▐ right half
    0x2591: 0x61,  # ░ light shade
    0x2592: 0x62,  # ▒ medium shade
    0x2593: 0x63,  # ▓ dark shade
    0x2594: 0x64,  # ▔ upper 1/8
    0x2595: 0x65,  # ▕ right 1/8
    0x25A0: 0x66,  # ■ black square
    0x25A1: 0x67,  # □ white square
    0x25AA: 0x68,  # ▪ black small square
    0x25AB: 0x69,  # ▫ white small square
    0x25B2: 0x6A,  # ▲ black up triangle
    0x25B6: 0x6B,  # ▶ black right triangle
    0x25BC: 0x6C,  # ▼ black down triangle
    0x25C0: 0x6D,  # ◀ black left triangle
    0x25CB: 0x6E,  # ○ white circle
    0x25CF: 0x6F,  # ● black circle

    # CP437 classics + misc (0x70-0x7E)
    0x2660: 0x70,  # ♠ spade
    0x2663: 0x71,  # ♣ club
    0x2665: 0x72,  # ♥ heart
    0x2666: 0x73,  # ♦ diamond
    0x263A: 0x74,  # ☺ white smiley
    0x263B: 0x75,  # ☻ black smiley
    0x266A: 0x76,  # ♪ eighth note
    0x266B: 0x77,  # ♫ beamed eighth notes
    0x2713: 0x78,  # ✓ check mark
    0x2717: 0x79,  # ✗ ballot x
    0x2190: 0x7A,  # ← leftwards arrow
    0x2191: 0x7B,  # ↑ upwards arrow
    0x2192: 0x7C,  # → rightwards arrow
    0x2193: 0x7D,  # ↓ downwards arrow
    0x2194: 0x7E,  # ↔ left-right arrow

    # Claude CLI characters (0x7F-0x84)
    0x23F5: 0x7F,  # ⏵ prompt marker (black medium right-pointing triangle)
    0x2217: 0x80,  # ∗ asterisk operator (spinner)
    0x2722: 0x81,  # ✢ four teardrop-spoked asterisk (spinner)
    0x2733: 0x82,  # ✳ eight spoked asterisk (spinner)
    0x273B: 0x83,  # ✻ teardrop-spoked asterisk (spinner)
    0x273D: 0x84,  # ✽ heavy teardrop-spoked pinwheel asterisk (spinner)

    # Braille patterns for Claude spinner (0x85-0x8E)
    0x280B: 0x85,  # ⠋
    0x2819: 0x86,  # ⠙
    0x2839: 0x87,  # ⠹
    0x2838: 0x88,  # ⠸
    0x283C: 0x89,  # ⠼
    0x2834: 0x8A,  # ⠴
    0x2826: 0x8B,  # ⠦
    0x2827: 0x8C,  # ⠧
    0x2807: 0x8D,  # ⠇
    0x280F: 0x8E,  # ⠏

    # Search icon
    0x2315: 0x8F,  # ⌕ telephone recorder / magnifying glass

    # Quadrant block characters (0x90-0x99)
    0x2596: 0x90,  # ▖ quadrant lower left
    0x2597: 0x91,  # ▗ quadrant lower right
    0x2598: 0x92,  # ▘ quadrant upper left
    0x2599: 0x93,  # ▙ quadrant upper left and lower left and lower right
    0x259A: 0x94,  # ▚ quadrant upper left and lower right
    0x259B: 0x95,  # ▛ quadrant upper left and upper right and lower left
    0x259C: 0x96,  # ▜ quadrant upper left and upper right and lower right
    0x259D: 0x97,  # ▝ quadrant upper right
    0x259E: 0x98,  # ▞ quadrant upper right and lower left
    0x259F: 0x99,  # ▟ quadrant upper right and lower left and lower right

    # Pointer / prompt marker
    0x276F: 0x9A,  # ❯ heavy right-pointing angle quotation mark ornament

    # Figures / UI characters (0x9C-0xAC)
    0x25C6: 0x9C,  # ◆ black diamond (lozenge)
    0x25C7: 0x9D,  # ◇ white diamond (lozengeOutline)
    0x2714: 0x9E,  # ✔ heavy check mark (tick)
    0x2718: 0x9F,  # ✘ heavy ballot X (cross)
    0x25FB: 0xA0,  # ◻ white medium square (squareSmall)
    0x25FC: 0xA1,  # ◼ black medium square (squareSmallFilled)
    0x25C9: 0xA2,  # ◉ fisheye (radioOn/circleFilled)
    0x25EF: 0xA3,  # ◯ large circle (radioOff/circle)
    0x2610: 0xA4,  # ☐ ballot box (checkboxOff)
    0x2612: 0xA5,  # ☒ ballot box with X (checkboxOn)
    0x2605: 0xA6,  # ★ black star
    0x2630: 0xA7,  # ☰ trigram for heaven (hamburger)
    0x25B3: 0xA8,  # △ white up-pointing triangle
    0x26A0: 0xA9,  # ⚠ warning sign
    0x25CE: 0xAA,  # ◎ bullseye (circleDouble)
    0x25CC: 0xAB,  # ◌ dotted circle
    0x2139: 0xAC,  # ℹ information source (info)

    # Middle dot / separator (0xAD)
    0x00B7: 0xAD,  # · middle dot
}

# Build reverse map: char_code -> unicode
CODE_TO_UNICODE = {v: k for k, v in GLYPH_MAP.items()}


class Bitmap:
    """A mutable pixel grid for rendering glyphs."""

    def __init__(self, width, height):
        self.w = width
        self.h = height
        self.pixels = [[0] * width for _ in range(height)]

    def set(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.pixels[y][x] = 1

    def fill_rect(self, x0, y0, x1, y1):
        """Fill rectangle inclusive of x0,y0 and exclusive of x1,y1."""
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.pixels[y][x] = 1

    def hline(self, y, x0, x1):
        """Horizontal line from x0 to x1 (exclusive) at row y."""
        for x in range(max(0, x0), min(self.w, x1)):
            if 0 <= y < self.h:
                self.pixels[y][x] = 1

    def vline(self, x, y0, y1):
        """Vertical line from y0 to y1 (exclusive) at column x."""
        for y in range(max(0, y0), min(self.h, y1)):
            if 0 <= x < self.w:
                self.pixels[y][x] = 1

    def dither(self, density):
        """Fill with dither pattern. density: 0.25, 0.5, 0.75."""
        for y in range(self.h):
            for x in range(self.w):
                if density >= 0.75:
                    # dark shade: all on except checkerboard holes
                    if not (x % 2 == 0 and y % 2 == 0):
                        self.pixels[y][x] = 1
                elif density >= 0.5:
                    # medium shade: checkerboard
                    if (x + y) % 2 == 0:
                        self.pixels[y][x] = 1
                elif density >= 0.25:
                    # light shade: sparse dots
                    if x % 2 == 0 and y % 2 == 0:
                        self.pixels[y][x] = 1

    def fill_circle(self, cx, cy, r):
        """Filled circle centered at (cx, cy) with radius r."""
        for y in range(self.h):
            for x in range(self.w):
                dx = x - cx
                dy = y - cy
                if dx * dx + dy * dy <= r * r:
                    self.pixels[y][x] = 1

    def outline_circle(self, cx, cy, r):
        """Outline circle."""
        for y in range(self.h):
            for x in range(self.w):
                dx = x - cx
                dy = y - cy
                d = dx * dx + dy * dy
                if abs(d - r * r) <= r * 1.5:
                    self.pixels[y][x] = 1

    def fill_triangle(self, pts):
        """Fill triangle given 3 points [(x,y), ...]."""
        # Simple scanline fill
        min_y = max(0, min(p[1] for p in pts))
        max_y = min(self.h - 1, max(p[1] for p in pts))
        for y in range(min_y, max_y + 1):
            intersections = []
            for i in range(3):
                p0 = pts[i]
                p1 = pts[(i + 1) % 3]
                if p0[1] == p1[1]:
                    continue
                if min(p0[1], p1[1]) <= y <= max(p0[1], p1[1]):
                    t = (y - p0[1]) / (p1[1] - p0[1])
                    x = p0[0] + t * (p1[0] - p0[0])
                    intersections.append(x)
            if len(intersections) >= 2:
                intersections.sort()
                for x in range(max(0, int(intersections[0])),
                               min(self.w, int(intersections[-1]) + 1)):
                    self.pixels[y][x] = 1


def render_glyph(unicode_cp, w, h):
    """Render a single glyph to a Bitmap of size w x h."""
    bm = Bitmap(w, h)
    cx = w // 2      # center x
    cy = h // 2      # center y

    # Determine what kind of glyph this is based on Unicode codepoint

    # --- Box drawing (single line) ---
    if unicode_cp == 0x2500:  # ─ horizontal
        bm.hline(cy, 0, w)
    elif unicode_cp == 0x2502:  # │ vertical
        bm.vline(cx, 0, h)
    elif unicode_cp == 0x250C:  # ┌ down-right
        bm.hline(cy, cx, w)
        bm.vline(cx, cy, h)
    elif unicode_cp == 0x2510:  # ┐ down-left
        bm.hline(cy, 0, cx + 1)
        bm.vline(cx, cy, h)
    elif unicode_cp == 0x2514:  # └ up-right
        bm.hline(cy, cx, w)
        bm.vline(cx, 0, cy + 1)
    elif unicode_cp == 0x2518:  # ┘ up-left
        bm.hline(cy, 0, cx + 1)
        bm.vline(cx, 0, cy + 1)
    elif unicode_cp == 0x251C:  # ├ vertical-right
        bm.vline(cx, 0, h)
        bm.hline(cy, cx, w)
    elif unicode_cp == 0x2524:  # ┤ vertical-left
        bm.vline(cx, 0, h)
        bm.hline(cy, 0, cx + 1)
    elif unicode_cp == 0x252C:  # ┬ horizontal-down
        bm.hline(cy, 0, w)
        bm.vline(cx, cy, h)
    elif unicode_cp == 0x2534:  # ┴ horizontal-up
        bm.hline(cy, 0, w)
        bm.vline(cx, 0, cy + 1)
    elif unicode_cp == 0x253C:  # ┼ cross
        bm.hline(cy, 0, w)
        bm.vline(cx, 0, h)

    # --- Double line box drawing ---
    elif unicode_cp == 0x2550:  # ═ double horizontal
        bm.hline(cy - 1, 0, w)
        bm.hline(cy + 1, 0, w)
    elif unicode_cp == 0x2551:  # ║ double vertical
        bm.vline(cx - 1, 0, h)
        bm.vline(cx + 1, 0, h)
    elif unicode_cp == 0x2554:  # ╔ double down-right
        bm.hline(cy - 1, cx + 1, w)
        bm.hline(cy + 1, cx - 1, w)
        bm.vline(cx - 1, cy + 1, h)
        bm.vline(cx + 1, cy - 1, h)
    elif unicode_cp == 0x2557:  # ╗ double down-left
        bm.hline(cy - 1, 0, cx)
        bm.hline(cy + 1, 0, cx + 2)
        bm.vline(cx + 1, cy + 1, h)
        bm.vline(cx - 1, cy - 1, h)
    elif unicode_cp == 0x255A:  # ╚ double up-right
        bm.hline(cy - 1, cx - 1, w)
        bm.hline(cy + 1, cx + 1, w)
        bm.vline(cx - 1, 0, cy - 1 + 1)
        bm.vline(cx + 1, 0, cy + 1)
    elif unicode_cp == 0x255D:  # ╝ double up-left
        bm.hline(cy - 1, 0, cx + 2)
        bm.hline(cy + 1, 0, cx)
        bm.vline(cx + 1, 0, cy - 1 + 1)
        bm.vline(cx - 1, 0, cy + 1 + 1)
    elif unicode_cp == 0x2560:  # ╠ double vertical-right
        bm.vline(cx - 1, 0, h)
        bm.vline(cx + 1, 0, cy - 1 + 1)
        bm.vline(cx + 1, cy + 1, h)
        bm.hline(cy - 1, cx + 1, w)
        bm.hline(cy + 1, cx + 1, w)
    elif unicode_cp == 0x2563:  # ╣ double vertical-left
        bm.vline(cx + 1, 0, h)
        bm.vline(cx - 1, 0, cy - 1 + 1)
        bm.vline(cx - 1, cy + 1, h)
        bm.hline(cy - 1, 0, cx)
        bm.hline(cy + 1, 0, cx)
    elif unicode_cp == 0x2566:  # ╦ double horizontal-down
        bm.hline(cy - 1, 0, w)
        bm.hline(cy + 1, 0, cx)
        bm.hline(cy + 1, cx + 2, w)
        bm.vline(cx - 1, cy - 1, h)
        bm.vline(cx + 1, cy - 1, h)
    elif unicode_cp == 0x2569:  # ╩ double horizontal-up
        bm.hline(cy + 1, 0, w)
        bm.hline(cy - 1, 0, cx)
        bm.hline(cy - 1, cx + 2, w)
        bm.vline(cx - 1, 0, cy + 1 + 1)
        bm.vline(cx + 1, 0, cy + 1 + 1)
    elif unicode_cp == 0x256C:  # ╬ double cross
        bm.hline(cy - 1, 0, cx)
        bm.hline(cy - 1, cx + 2, w)
        bm.hline(cy + 1, 0, cx)
        bm.hline(cy + 1, cx + 2, w)
        bm.vline(cx - 1, 0, cy)
        bm.vline(cx - 1, cy + 2, h)
        bm.vline(cx + 1, 0, cy)
        bm.vline(cx + 1, cy + 2, h)

    # --- Mixed single/double ---
    elif unicode_cp == 0x2552:  # ╒ down-single right-double
        bm.vline(cx, cy - 1, h)
        bm.hline(cy - 1, cx, w)
        bm.hline(cy + 1, cx, w)
    elif unicode_cp == 0x2553:  # ╓ down-double right-single
        bm.hline(cy, cx - 1, w)
        bm.vline(cx - 1, cy, h)
        bm.vline(cx + 1, cy, h)
    elif unicode_cp == 0x2555:  # ╕ down-single left-double
        bm.vline(cx, cy - 1, h)
        bm.hline(cy - 1, 0, cx + 1)
        bm.hline(cy + 1, 0, cx + 1)
    elif unicode_cp == 0x2556:  # ╖ down-double left-single
        bm.hline(cy, 0, cx + 2)
        bm.vline(cx - 1, cy, h)
        bm.vline(cx + 1, cy, h)
    elif unicode_cp == 0x2558:  # ╘ up-single right-double
        bm.vline(cx, 0, cy + 2)
        bm.hline(cy - 1, cx, w)
        bm.hline(cy + 1, cx, w)
    elif unicode_cp == 0x2559:  # ╙ up-double right-single
        bm.hline(cy, cx - 1, w)
        bm.vline(cx - 1, 0, cy + 1)
        bm.vline(cx + 1, 0, cy + 1)
    elif unicode_cp == 0x255B:  # ╛ up-single left-double
        bm.vline(cx, 0, cy + 2)
        bm.hline(cy - 1, 0, cx + 1)
        bm.hline(cy + 1, 0, cx + 1)
    elif unicode_cp == 0x255C:  # ╜ up-double left-single
        bm.hline(cy, 0, cx + 2)
        bm.vline(cx - 1, 0, cy + 1)
        bm.vline(cx + 1, 0, cy + 1)
    elif unicode_cp == 0x255E:  # ╞ vertical-single right-double
        bm.vline(cx, 0, h)
        bm.hline(cy - 1, cx, w)
        bm.hline(cy + 1, cx, w)
    elif unicode_cp == 0x255F:  # ╟ vertical-double right-single
        bm.vline(cx - 1, 0, h)
        bm.vline(cx + 1, 0, h)
        bm.hline(cy, cx + 1, w)
    elif unicode_cp == 0x2561:  # ╡ vertical-single left-double
        bm.vline(cx, 0, h)
        bm.hline(cy - 1, 0, cx + 1)
        bm.hline(cy + 1, 0, cx + 1)
    elif unicode_cp == 0x2562:  # ╢ vertical-double left-single
        bm.vline(cx - 1, 0, h)
        bm.vline(cx + 1, 0, h)
        bm.hline(cy, 0, cx)
    elif unicode_cp == 0x2564:  # ╤ down-single horizontal-double
        bm.hline(cy - 1, 0, w)
        bm.hline(cy + 1, 0, w)
        bm.vline(cx, cy + 1, h)
    elif unicode_cp == 0x2565:  # ╥ down-double horizontal-single
        bm.hline(cy, 0, w)
        bm.vline(cx - 1, cy, h)
        bm.vline(cx + 1, cy, h)
    elif unicode_cp == 0x2567:  # ╧ up-single horizontal-double
        bm.hline(cy - 1, 0, w)
        bm.hline(cy + 1, 0, w)
        bm.vline(cx, 0, cy)
    elif unicode_cp == 0x2568:  # ╨ up-double horizontal-single
        bm.hline(cy, 0, w)
        bm.vline(cx - 1, 0, cy + 1)
        bm.vline(cx + 1, 0, cy + 1)
    elif unicode_cp == 0x256A:  # ╪ vertical-single horizontal-double
        bm.vline(cx, 0, h)
        bm.hline(cy - 1, 0, w)
        bm.hline(cy + 1, 0, w)
    elif unicode_cp == 0x256B:  # ╫ vertical-double horizontal-single
        bm.vline(cx - 1, 0, h)
        bm.vline(cx + 1, 0, h)
        bm.hline(cy, 0, w)

    # --- Half lines ---
    elif unicode_cp == 0x2574:  # ╴ light left
        bm.hline(cy, 0, cx + 1)
    elif unicode_cp == 0x2575:  # ╵ light up
        bm.vline(cx, 0, cy + 1)
    elif unicode_cp == 0x2576:  # ╶ light right
        bm.hline(cy, cx, w)
    elif unicode_cp == 0x2577:  # ╷ light down
        bm.vline(cx, cy, h)

    # --- Heavy lines ---
    elif unicode_cp == 0x2501:  # ━ heavy horizontal
        bm.hline(cy - 1, 0, w)
        bm.hline(cy, 0, w)
        bm.hline(cy + 1, 0, w)
    elif unicode_cp == 0x2503:  # ┃ heavy vertical
        bm.vline(cx - 1, 0, h)
        bm.vline(cx, 0, h)
        bm.vline(cx + 1, 0, h)
    elif unicode_cp == 0x254B:  # ╋ heavy cross
        bm.hline(cy - 1, 0, w)
        bm.hline(cy, 0, w)
        bm.hline(cy + 1, 0, w)
        bm.vline(cx - 1, 0, h)
        bm.vline(cx, 0, h)
        bm.vline(cx + 1, 0, h)

    # --- Triple dash ---
    elif unicode_cp == 0x2504:  # ┄ triple dash horizontal
        seg = max(1, w // 5)
        for i in range(0, w, seg * 2):
            bm.hline(cy, i, min(i + seg, w))

    # --- Block elements ---
    elif unicode_cp == 0x2580:  # ▀ upper half
        bm.fill_rect(0, 0, w, h // 2)
    elif unicode_cp == 0x2581:  # ▁ lower 1/8
        bm.fill_rect(0, h - max(1, h // 8), w, h)
    elif unicode_cp == 0x2582:  # ▂ lower 1/4
        bm.fill_rect(0, h - max(1, h // 4), w, h)
    elif unicode_cp == 0x2583:  # ▃ lower 3/8
        bm.fill_rect(0, h - max(1, h * 3 // 8), w, h)
    elif unicode_cp == 0x2584:  # ▄ lower half
        bm.fill_rect(0, h // 2, w, h)
    elif unicode_cp == 0x2585:  # ▅ lower 5/8
        bm.fill_rect(0, h - max(1, h * 5 // 8), w, h)
    elif unicode_cp == 0x2586:  # ▆ lower 3/4
        bm.fill_rect(0, h - max(1, h * 3 // 4), w, h)
    elif unicode_cp == 0x2587:  # ▇ lower 7/8
        bm.fill_rect(0, h - max(1, h * 7 // 8), w, h)
    elif unicode_cp == 0x2588:  # █ full block
        bm.fill_rect(0, 0, w, h)
    elif unicode_cp == 0x2589:  # ▉ left 7/8
        bm.fill_rect(0, 0, max(1, w * 7 // 8), h)
    elif unicode_cp == 0x258A:  # ▊ left 3/4
        bm.fill_rect(0, 0, max(1, w * 3 // 4), h)
    elif unicode_cp == 0x258B:  # ▋ left 5/8
        bm.fill_rect(0, 0, max(1, w * 5 // 8), h)
    elif unicode_cp == 0x258C:  # ▌ left half
        bm.fill_rect(0, 0, w // 2, h)
    elif unicode_cp == 0x258D:  # ▍ left 3/8
        bm.fill_rect(0, 0, max(1, w * 3 // 8), h)
    elif unicode_cp == 0x258E:  # ▎ left 1/4
        bm.fill_rect(0, 0, max(1, w // 4), h)
    elif unicode_cp == 0x258F:  # ▏ left 1/8
        bm.fill_rect(0, 0, max(1, w // 8), h)

    # --- Shading ---
    elif unicode_cp == 0x2590:  # ▐ right half
        bm.fill_rect(w // 2, 0, w, h)
    elif unicode_cp == 0x2591:  # ░ light shade
        bm.dither(0.25)
    elif unicode_cp == 0x2592:  # ▒ medium shade
        bm.dither(0.5)
    elif unicode_cp == 0x2593:  # ▓ dark shade
        bm.dither(0.75)
    elif unicode_cp == 0x2594:  # ▔ upper 1/8
        bm.fill_rect(0, 0, w, max(1, h // 8))
    elif unicode_cp == 0x2595:  # ▕ right 1/8
        bm.fill_rect(w - max(1, w // 8), 0, w, h)

    # --- Geometric shapes ---
    elif unicode_cp == 0x25A0:  # ■ black square
        m = max(1, min(w, h) // 6)
        bm.fill_rect(m, m, w - m, h - m)
    elif unicode_cp == 0x25A1:  # □ white square
        m = max(1, min(w, h) // 6)
        bm.fill_rect(m, m, w - m, m + 1)       # top
        bm.fill_rect(m, h - m - 1, w - m, h - m)  # bottom
        bm.fill_rect(m, m, m + 1, h - m)       # left
        bm.fill_rect(w - m - 1, m, w - m, h - m)  # right
    elif unicode_cp == 0x25AA:  # ▪ black small square
        m = max(1, min(w, h) // 4)
        bm.fill_rect(m, m, w - m, h - m)
    elif unicode_cp == 0x25AB:  # ▫ white small square
        m = max(1, min(w, h) // 4)
        bm.fill_rect(m, m, w - m, m + 1)
        bm.fill_rect(m, h - m - 1, w - m, h - m)
        bm.fill_rect(m, m, m + 1, h - m)
        bm.fill_rect(w - m - 1, m, w - m, h - m)

    # --- Triangles ---
    elif unicode_cp == 0x25B2:  # ▲ up triangle
        bm.fill_triangle([(cx, 1), (1, h - 2), (w - 2, h - 2)])
    elif unicode_cp == 0x25B6:  # ▶ right triangle
        bm.fill_triangle([(1, 1), (1, h - 2), (w - 2, cy)])
    elif unicode_cp == 0x25BC:  # ▼ down triangle
        bm.fill_triangle([(cx, h - 2), (1, 1), (w - 2, 1)])
    elif unicode_cp == 0x25C0:  # ◀ left triangle
        bm.fill_triangle([(w - 2, 1), (w - 2, h - 2), (1, cy)])

    # --- Circles ---
    elif unicode_cp == 0x25CB:  # ○ white circle
        r = min(cx, cy) - 1
        bm.outline_circle(cx, cy, r)
    elif unicode_cp == 0x25CF:  # ● black circle
        r = min(cx, cy) - 1
        bm.fill_circle(cx, cy, r)

    # --- Card suits ---
    elif unicode_cp == 0x2660:  # ♠ spade
        bm.fill_triangle([(cx, 1), (1, cy + 1), (w - 2, cy + 1)])
        bm.fill_circle(cx - w // 4, cy + 1, w // 4)
        bm.fill_circle(cx + w // 4, cy + 1, w // 4)
        bm.vline(cx, cy + 1, h - 1)
    elif unicode_cp == 0x2663:  # ♣ club
        r = max(1, w // 4)
        bm.fill_circle(cx, 2, r)
        bm.fill_circle(cx - r - 1, cy, r)
        bm.fill_circle(cx + r + 1, cy, r)
        bm.vline(cx, cy, h - 1)
    elif unicode_cp == 0x2665:  # ♥ heart
        r = max(1, w // 4)
        bm.fill_circle(cx - r, 2 + r, r)
        bm.fill_circle(cx + r, 2 + r, r)
        bm.fill_triangle([(cx, h - 2), (0, 2 + r), (w - 1, 2 + r)])
    elif unicode_cp == 0x2666:  # ♦ diamond
        bm.fill_triangle([(cx, 1), (1, cy), (cx, h - 2)])
        bm.fill_triangle([(cx, 1), (w - 2, cy), (cx, h - 2)])

    # --- Smileys ---
    elif unicode_cp == 0x263A:  # ☺ white smiley
        r = min(cx, cy) - 1
        bm.outline_circle(cx, cy, r)
        if w >= 6:
            bm.set(cx - 1, cy - 1)
            bm.set(cx + 1, cy - 1)
            bm.set(cx, cy + 1)
    elif unicode_cp == 0x263B:  # ☻ black smiley
        r = min(cx, cy) - 1
        bm.fill_circle(cx, cy, r)

    # --- Music notes ---
    elif unicode_cp == 0x266A:  # ♪ eighth note
        bm.fill_circle(cx - 1, h - 3, max(1, w // 4))
        bm.vline(cx + max(1, w // 4) - 1, 2, h - 3)
        bm.hline(2, cx + max(1, w // 4) - 1, w - 1)
    elif unicode_cp == 0x266B:  # ♫ beamed eighth notes
        x0 = max(1, w // 4)
        x1 = w - max(1, w // 4) - 1
        bm.fill_circle(x0, h - 3, max(1, w // 5))
        bm.fill_circle(x1, h - 3, max(1, w // 5))
        bm.vline(x0 + max(1, w // 5), 2, h - 3)
        bm.vline(x1 + max(1, w // 5), 2, h - 3)
        bm.hline(2, x0 + max(1, w // 5), x1 + max(1, w // 5) + 1)
        bm.hline(3, x0 + max(1, w // 5), x1 + max(1, w // 5) + 1)

    # --- Check/X marks ---
    elif unicode_cp == 0x2713:  # ✓ check mark
        for i in range(min(w, h) // 2):
            bm.set(cx // 2 + i, cy + i)
        for i in range(min(w, h) * 2 // 3):
            bm.set(cx // 2 + min(w, h) // 2 + i, cy + min(w, h) // 2 - 1 - i)
    elif unicode_cp == 0x2717:  # ✗ ballot x
        for i in range(min(w, h) - 2):
            bm.set(1 + i * (w - 2) // (min(w, h) - 2), 1 + i)
            bm.set(w - 2 - i * (w - 2) // (min(w, h) - 2), 1 + i)

    # --- Arrows ---
    elif unicode_cp == 0x2190:  # ← left arrow
        bm.hline(cy, 0, w)
        for i in range(min(3, h // 3)):
            bm.set(i, cy - i - 1)
            bm.set(i, cy + i + 1)
    elif unicode_cp == 0x2191:  # ↑ up arrow
        bm.vline(cx, 0, h)
        for i in range(min(3, w // 3)):
            bm.set(cx - i - 1, i)
            bm.set(cx + i + 1, i)
    elif unicode_cp == 0x2192:  # → right arrow
        bm.hline(cy, 0, w)
        for i in range(min(3, h // 3)):
            bm.set(w - 1 - i, cy - i - 1)
            bm.set(w - 1 - i, cy + i + 1)
    elif unicode_cp == 0x2193:  # ↓ down arrow
        bm.vline(cx, 0, h)
        for i in range(min(3, w // 3)):
            bm.set(cx - i - 1, h - 1 - i)
            bm.set(cx + i + 1, h - 1 - i)
    elif unicode_cp == 0x2194:  # ↔ left-right arrow
        bm.hline(cy, 0, w)
        for i in range(min(3, h // 3)):
            bm.set(i, cy - i - 1)
            bm.set(i, cy + i + 1)
            bm.set(w - 1 - i, cy - i - 1)
            bm.set(w - 1 - i, cy + i + 1)

    # --- Claude prompt marker ---
    elif unicode_cp == 0x23F5:  # ⏵ right-pointing triangle
        m = max(1, min(w, h) // 6)
        bm.fill_triangle([(m, m), (m, h - m - 1), (w - m - 1, cy)])

    # --- Spinner asterisk/star characters ---
    elif unicode_cp == 0x2217:  # ∗ asterisk operator
        r = min(cx, cy) - 1
        # Simple 6-pointed asterisk: vertical + two diagonals through center
        bm.vline(cx, cy - r, cy + r + 1)
        for i in range(-r, r + 1):
            dx = i * r // (r if r else 1) // 2
            bm.set(cx + i, cy - dx)
            bm.set(cx + i, cy + dx)
    elif unicode_cp == 0x2722:  # ✢ four teardrop-spoked asterisk
        r = min(cx, cy) - 1
        bm.vline(cx, cy - r, cy + r + 1)
        bm.hline(cy, cx - r, cx + r + 1)
        # Teardrop bulges near center
        bm.set(cx - 1, cy - 1)
        bm.set(cx + 1, cy - 1)
        bm.set(cx - 1, cy + 1)
        bm.set(cx + 1, cy + 1)
    elif unicode_cp == 0x2733:  # ✳ eight spoked asterisk
        r = min(cx, cy) - 1
        bm.vline(cx, cy - r, cy + r + 1)
        bm.hline(cy, cx - r, cx + r + 1)
        for i in range(-r, r + 1):
            bm.set(cx + i, cy + i)
            bm.set(cx + i, cy - i)
    elif unicode_cp == 0x273B:  # ✻ teardrop-spoked asterisk
        r = min(cx, cy) - 1
        bm.vline(cx, cy - r, cy + r + 1)
        bm.hline(cy, cx - r, cx + r + 1)
        for i in range(-r, r + 1):
            bm.set(cx + i, cy + i)
            bm.set(cx + i, cy - i)
        # Extra weight near center
        if w >= 6:
            bm.set(cx - 1, cy)
            bm.set(cx + 1, cy)
            bm.set(cx, cy - 1)
            bm.set(cx, cy + 1)
    elif unicode_cp == 0x273D:  # ✽ heavy teardrop-spoked pinwheel
        r = min(cx, cy) - 1
        bm.vline(cx, cy - r, cy + r + 1)
        bm.hline(cy, cx - r, cx + r + 1)
        for i in range(-r, r + 1):
            bm.set(cx + i, cy + i)
            bm.set(cx + i, cy - i)
        # Heavy: fill the inner area
        ir = max(1, r // 2)
        bm.fill_circle(cx, cy, ir)

    # --- Search / magnifying glass icon ---
    elif unicode_cp == 0x2315:  # ⌕ telephone recorder (used as search icon)
        # Circle in upper-left area + diagonal handle to lower-right
        lens_r = max(2, min(w, h) // 3)
        lens_cx = cx - lens_r // 3
        lens_cy = cy - lens_r // 3
        bm.outline_circle(lens_cx, lens_cy, lens_r)
        # Handle: diagonal line from circle edge to bottom-right
        for i in range(lens_r):
            bm.set(lens_cx + lens_r + i, lens_cy + lens_r + i)
            if w >= 8:
                bm.set(lens_cx + lens_r + i + 1, lens_cy + lens_r + i)

    # --- Braille patterns ---
    elif 0x2800 <= unicode_cp <= 0x28FF:
        # Braille: 2 columns x 4 rows of dots
        # Bit layout: bit0=dot1(r0c0), bit1=dot2(r1c0), bit2=dot3(r2c0),
        #   bit3=dot4(r0c1), bit4=dot5(r1c1), bit5=dot6(r2c1),
        #   bit6=dot7(r3c0), bit7=dot8(r3c1)
        pattern = unicode_cp - 0x2800
        # Dot positions as fractions of cell
        dot_r = max(1, min(w, h) // 8)
        # Column x positions: ~1/3 and ~2/3 of cell width
        col_x = [w // 3, w * 2 // 3]
        # Row y positions: divide cell into 5 rows (4 dots + margins)
        row_y = [h * (i + 1) // 5 for i in range(4)]
        dot_map = [
            (0, 0, 0), (1, 0, 1), (2, 0, 2),  # dots 1,2,3 (left col)
            (0, 1, 3), (1, 1, 4), (2, 1, 5),  # dots 4,5,6 (right col)
            (3, 0, 6), (3, 1, 7),              # dots 7,8 (bottom row)
        ]
        for row, col, bit in dot_map:
            if pattern & (1 << bit):
                bm.fill_circle(col_x[col], row_y[row], dot_r)

    # --- Pointer / angle bracket ---
    elif unicode_cp == 0x276F:  # ❯ heavy right-pointing angle
        # Right-pointing chevron/angle bracket >
        for i in range(h):
            # Map row i to x position: goes right for top half, left for bottom half
            if i <= cy:
                x = cx - cx + (i * cx) // max(cy, 1)
            else:
                x = cx - ((i - cy) * cx) // max(h - 1 - cy, 1)
            bm.set(x, i)
            if w >= 6:  # thicken for readability
                bm.set(x + 1, i)

    # --- Quadrant block characters ---
    elif unicode_cp == 0x2596:  # ▖ quadrant lower left
        bm.fill_rect(0, h // 2, w // 2, h)
    elif unicode_cp == 0x2597:  # ▗ quadrant lower right
        bm.fill_rect(w // 2, h // 2, w, h)
    elif unicode_cp == 0x2598:  # ▘ quadrant upper left
        bm.fill_rect(0, 0, w // 2, h // 2)
    elif unicode_cp == 0x2599:  # ▙ quadrant upper left + lower left + lower right
        bm.fill_rect(0, 0, w // 2, h // 2)
        bm.fill_rect(0, h // 2, w, h)
    elif unicode_cp == 0x259A:  # ▚ quadrant upper left + lower right
        bm.fill_rect(0, 0, w // 2, h // 2)
        bm.fill_rect(w // 2, h // 2, w, h)
    elif unicode_cp == 0x259B:  # ▛ quadrant upper left + upper right + lower left
        bm.fill_rect(0, 0, w, h // 2)
        bm.fill_rect(0, h // 2, w // 2, h)
    elif unicode_cp == 0x259C:  # ▜ quadrant upper left + upper right + lower right
        bm.fill_rect(0, 0, w, h // 2)
        bm.fill_rect(w // 2, h // 2, w, h)
    elif unicode_cp == 0x259D:  # ▝ quadrant upper right
        bm.fill_rect(w // 2, 0, w, h // 2)
    elif unicode_cp == 0x259E:  # ▞ quadrant upper right + lower left
        bm.fill_rect(w // 2, 0, w, h // 2)
        bm.fill_rect(0, h // 2, w // 2, h)
    elif unicode_cp == 0x259F:  # ▟ quadrant upper right + lower left + lower right
        bm.fill_rect(w // 2, 0, w, h // 2)
        bm.fill_rect(0, h // 2, w, h)

    # --- Figures / UI characters ---
    elif unicode_cp == 0x25C6:  # ◆ black diamond
        bm.fill_triangle([(cx, 1), (1, cy), (cx, h - 2)])
        bm.fill_triangle([(cx, 1), (w - 2, cy), (cx, h - 2)])
    elif unicode_cp == 0x25C7:  # ◇ white diamond
        for i in range(cy):
            bm.set(cx - i, cy - i)
            bm.set(cx + i, cy - i)
            bm.set(cx - i, cy + i)
            bm.set(cx + i, cy + i)
    elif unicode_cp == 0x2714:  # ✔ heavy check mark
        for d in range(-1, 2):
            for i in range(h // 3):
                bm.set(w // 4 + i, cy + i + d)
            for i in range(h * 2 // 3):
                bm.set(w // 4 + h // 3 + i, cy + h // 3 - 1 - i + d)
    elif unicode_cp == 0x2718:  # ✘ heavy ballot X
        for d in range(-1, 2):
            for i in range(h - 2):
                bm.set(1 + i * (w - 3) // max(h - 3, 1) + d, 1 + i)
                bm.set(w - 2 - i * (w - 3) // max(h - 3, 1) + d, 1 + i)
    elif unicode_cp == 0x25FB:  # ◻ white medium square
        m = max(1, min(w, h) // 5)
        bm.fill_rect(m, m, w - m, m + 1)
        bm.fill_rect(m, h - m - 1, w - m, h - m)
        bm.fill_rect(m, m, m + 1, h - m)
        bm.fill_rect(w - m - 1, m, w - m, h - m)
    elif unicode_cp == 0x25FC:  # ◼ black medium square
        m = max(1, min(w, h) // 5)
        bm.fill_rect(m, m, w - m, h - m)
    elif unicode_cp == 0x25C9:  # ◉ fisheye (filled circle with dot)
        r = min(cx, cy) - 1
        bm.outline_circle(cx, cy, r)
        bm.fill_circle(cx, cy, max(1, r // 2))
    elif unicode_cp == 0x25EF:  # ◯ large circle
        r = min(cx, cy) - 1
        bm.outline_circle(cx, cy, r)
    elif unicode_cp == 0x2610:  # ☐ ballot box
        m = max(1, min(w, h) // 6)
        bm.fill_rect(m, m, w - m, m + 1)
        bm.fill_rect(m, h - m - 1, w - m, h - m)
        bm.fill_rect(m, m, m + 1, h - m)
        bm.fill_rect(w - m - 1, m, w - m, h - m)
    elif unicode_cp == 0x2612:  # ☒ ballot box with X
        m = max(1, min(w, h) // 6)
        bm.fill_rect(m, m, w - m, m + 1)
        bm.fill_rect(m, h - m - 1, w - m, h - m)
        bm.fill_rect(m, m, m + 1, h - m)
        bm.fill_rect(w - m - 1, m, w - m, h - m)
        for i in range(h - 2 * m - 2):
            x = m + 1 + i * (w - 2 * m - 3) // max(h - 2 * m - 3, 1)
            bm.set(x, m + 1 + i)
            bm.set(w - m - 2 - i * (w - 2 * m - 3) // max(h - 2 * m - 3, 1), m + 1 + i)
    elif unicode_cp == 0x2605:  # ★ black star
        # 5-pointed star: fill a rough star shape
        bm.fill_triangle([(cx, 0), (cx - cx // 2, cy), (cx + cx // 2, cy)])
        bm.fill_triangle([(0, h // 3), (w - 1, h // 3), (cx, h - 2)])
        bm.fill_rect(cx - cx // 2, h // 3, cx + cx // 2 + 1, cy + 1)
    elif unicode_cp == 0x2630:  # ☰ trigram (hamburger menu)
        t = max(1, h // 8)
        for row in range(3):
            y = h // 6 + row * h // 3
            bm.fill_rect(1, y, w - 1, y + t)
    elif unicode_cp == 0x25B3:  # △ white up-pointing triangle
        for i in range(h - 2):
            hw = (i * (w - 2)) // (2 * max(h - 3, 1))
            bm.set(cx - hw, h - 2 - i)
            bm.set(cx + hw, h - 2 - i)
        bm.hline(h - 2, cx - (w - 2) // 2, cx + (w - 2) // 2 + 1)
    elif unicode_cp == 0x26A0:  # ⚠ warning sign
        # Triangle outline with ! inside
        for i in range(h - 2):
            hw = (i * (w - 2)) // (2 * max(h - 3, 1))
            bm.set(cx - hw, h - 2 - i)
            bm.set(cx + hw, h - 2 - i)
        bm.hline(h - 2, cx - (w - 2) // 2, cx + (w - 2) // 2 + 1)
        # Exclamation mark inside
        bm.vline(cx, h // 4 + 1, h * 2 // 3)
        bm.set(cx, h * 3 // 4)
    elif unicode_cp == 0x25CE:  # ◎ bullseye (double circle)
        r = min(cx, cy) - 1
        bm.outline_circle(cx, cy, r)
        if r >= 3:
            bm.outline_circle(cx, cy, r - 2)
    elif unicode_cp == 0x25CC:  # ◌ dotted circle
        r = min(cx, cy) - 1
        # Draw dots around circle perimeter
        import math
        for angle_deg in range(0, 360, 30):
            angle = math.radians(angle_deg)
            x = int(cx + r * math.cos(angle))
            y = int(cy + r * math.sin(angle))
            if 0 <= x < w and 0 <= y < h:
                bm.set(x, y)
    elif unicode_cp == 0x2139:  # ℹ information source
        # Serif lowercase i
        bm.set(cx, 2)  # dot
        bm.vline(cx, 4, h - 2)  # stem
        bm.hline(h - 2, cx - 1, cx + 2)  # base serif
        bm.hline(4, cx - 1, cx + 2)  # top serif

    elif unicode_cp == 0x00B7:  # · middle dot
        # Centered dot, 2x2 at larger sizes, 1x1 at smallest
        dot_size = max(1, min(w, h) // 6)
        for dy in range(dot_size):
            for dx in range(dot_size):
                bm.set(cx - dot_size // 2 + dx, cy - dot_size // 2 + dy)

    else:
        # Unknown glyph — fill with lozenge-like pattern
        bm.set(cx, 1)
        bm.set(cx - 1, cy)
        bm.set(cx + 1, cy)
        bm.set(cx, h - 2)

    return bm


def pack_nfnt(pt_size, cell_w, cell_h, ascent, descent, leading):
    """Generate NFNT resource data as Rez hex strings."""
    num_chars = LAST_CHAR - FIRST_CHAR + 1  # 95 characters
    # Plus 2 for "missing" and "last+1" entries
    table_entries = num_chars + 2

    # Each glyph is cell_w pixels wide. We lay them all out in a single
    # bitmap strip. The strip is (num_chars + 1) * cell_w pixels wide
    # (extra for the "missing" glyph).
    total_glyphs = num_chars + 1  # +1 for missing glyph
    strip_width = total_glyphs * cell_w
    # rowWords = ceil(strip_width / 16)
    row_words = (strip_width + 15) // 16
    row_bits = row_words * 16

    # Render all glyphs
    glyph_bitmaps = []
    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        ucp = CODE_TO_UNICODE.get(code, None)
        if ucp is not None:
            bm = render_glyph(ucp, cell_w, cell_h)
        else:
            # Undefined slot — empty
            bm = Bitmap(cell_w, cell_h)
        glyph_bitmaps.append(bm)

    # Missing glyph (drawn as a small box)
    missing = Bitmap(cell_w, cell_h)
    missing.fill_rect(0, 0, cell_w, 1)
    missing.fill_rect(0, cell_h - 1, cell_w, cell_h)
    missing.fill_rect(0, 0, 1, cell_h)
    missing.fill_rect(cell_w - 1, 0, cell_w, cell_h)
    glyph_bitmaps.append(missing)

    # Build the bitmap strike
    # Each row of the strike is row_bits wide
    strike_rows = []
    for y in range(cell_h):
        row = [0] * row_bits
        for gi, bm in enumerate(glyph_bitmaps):
            x_offset = gi * cell_w
            for x in range(cell_w):
                if y < bm.h and x < bm.w and bm.pixels[y][x]:
                    row[x_offset + x] = 1
        strike_rows.append(row)

    # Pack bits into bytes, then into hex
    strike_hex_rows = []
    for row in strike_rows:
        row_bytes = []
        for i in range(0, row_bits, 8):
            byte = 0
            for bit in range(8):
                if i + bit < len(row) and row[i + bit]:
                    byte |= (0x80 >> bit)
            row_bytes.append(byte)
        strike_hex_rows.append(''.join(f'{b:02X}' for b in row_bytes))

    # Location table: offset in pixels for each glyph
    # entries = firstChar..lastChar, then missing, then lastChar+1
    loc_table = []
    for i in range(num_chars):
        loc_table.append(i * cell_w)
    loc_table.append(num_chars * cell_w)  # missing glyph
    loc_table.append((num_chars + 1) * cell_w)  # end sentinel

    # Offset/Width table: high byte = offset within glyph rect, low byte = width
    # For fixed-width: offset = 0, width = cell_w for all
    # Encoded as a single 16-bit value: (offset << 8) | width
    ow_table = []
    for i in range(num_chars):
        ucp = CODE_TO_UNICODE.get(FIRST_CHAR + i, None)
        if ucp is not None:
            ow_table.append((0 << 8) | cell_w)
        else:
            # Undefined: use missing glyph
            ow_table.append(0xFFFF)  # -1 = missing
    ow_table.append((0 << 8) | cell_w)  # missing glyph
    ow_table.append(0xFFFF)  # end sentinel

    # owTLoc: word offset from owTLoc field itself to the offset/width table
    # Header layout: 13 words (26 bytes), owTLoc is word 8 = byte 16
    # Data after header: strike, loc_table, ow_table
    strike_bytes = row_words * 2 * cell_h
    loc_bytes = table_entries * 2
    ow_table_byte = 26 + strike_bytes + loc_bytes
    owTLoc_byte = 16  # word 8 in the header
    owTLoc = (ow_table_byte - owTLoc_byte) // 2

    return {
        'cell_w': cell_w,
        'cell_h': cell_h,
        'ascent': ascent,
        'descent': descent,
        'leading': leading,
        'row_words': row_words,
        'owTLoc': owTLoc,
        'strike_hex': strike_hex_rows,
        'loc_table': loc_table,
        'ow_table': ow_table,
    }


def emit_rez():
    """Output complete Rez source for FOND + NFNT resources."""
    print("/* SevenTTY Symbols font - AUTO-GENERATED, do not edit */")
    print("/* Generated by tools/generate_symbolfont.py */")
    print("")
    print('#include "constants.r"')
    print("")

    # Generate each NFNT
    for i, (pt, cw, ch, asc, dsc, lead) in enumerate(MONACO_METRICS):
        nfnt_id = NFNT_BASE_ID + i
        data = pack_nfnt(pt, cw, ch, asc, dsc, lead)

        print(f"/* NFNT for {pt}pt: {cw}x{ch} cells, ascent={asc} descent={dsc} leading={lead} */")
        print(f"data 'NFNT' ({nfnt_id}, \"SevenTTY Symbols {pt}\", purgeable) {{")

        # Font header (13 words = 26 bytes)
        # Word 0: fontType — 0x9000 = fixedWidth(0x2000) | dontExpand(0x1000) | prop-unused bits
        #   Actually the bits are:
        #   bit 0: reserved=1 (0x0001) -- NO, bit0=1 reserved
        #   bit 1: dontExpand=1 (0x0002) -- uh
        # Let me just use 0x9000 which is what fixed-width bitmap fonts typically use
        # fontType = 0xB000: fixedWidth=1, dontExpand=1, synthetic=0, oneBit
        # Standard for a simple 1-bit fixed-width font: 0x9000
        font_type = 0x9000

        neg_descent = -dsc
        # fRectWidth = total pixel width of one glyph
        fRectWidth = cw
        fRectHeight = ch
        kern_max = 0

        header_words = [
            font_type,                          # fontType
            FIRST_CHAR,                         # firstChar
            LAST_CHAR,                          # lastChar
            cw,                                 # widMax
            kern_max,                           # kernMax
            neg_descent & 0xFFFF,               # nDescent (signed, as unsigned)
            fRectWidth,                         # fRectWidth
            fRectHeight,                        # fRectHeight
            data['owTLoc'] & 0xFFFF,            # owTLoc
            asc,                                # ascent
            dsc,                                # descent
            lead,                               # leading
            data['row_words'],                  # rowWords
        ]

        # Emit header as hex
        header_hex = ''.join(f'{w & 0xFFFF:04X}' for w in header_words)
        # Split into 32-byte lines
        print(f'    $"{header_hex}"')

        # Emit strike data
        for row_hex in data['strike_hex']:
            # Split long lines
            for j in range(0, len(row_hex), 64):
                chunk = row_hex[j:j+64]
                print(f'    $"{chunk}"')

        # Emit location table
        loc_hex = ''.join(f'{v:04X}' for v in data['loc_table'])
        for j in range(0, len(loc_hex), 64):
            chunk = loc_hex[j:j+64]
            print(f'    $"{chunk}"')

        # Emit offset/width table
        ow_hex = ''.join(f'{v & 0xFFFF:04X}' for v in data['ow_table'])
        for j in range(0, len(ow_hex), 64):
            chunk = ow_hex[j:j+64]
            print(f'    $"{chunk}"')

        print("};")
        print("")

    # Generate FOND resource
    # The FOND ties all sizes together under one font family
    # We'll emit it as raw data since the Rez FOND template is complex
    print("/* Font family resource */")
    print(f"data 'FOND' ({FOND_ID}, \"SevenTTY Symbols\", purgeable) {{")

    # FOND resource structure (Inside Macintosh: Text, Chapter 4)
    # All offsets are LongInt (4 bytes), arrays are proper sizes
    import struct

    fond_flags = 0x8000  # bit 15 = fixed-width font

    # Use 12pt metrics as default
    default_asc = 12
    default_dsc = 3
    default_lead = 1
    default_wid = 7

    fond_data = b''
    fond_data += struct.pack('>H', fond_flags)      # ffFlags (word, unsigned)
    fond_data += struct.pack('>h', FOND_ID)          # ffFamID (word)
    fond_data += struct.pack('>h', FIRST_CHAR)       # ffFirstChar (word)
    fond_data += struct.pack('>h', LAST_CHAR)        # ffLastChar (word)
    fond_data += struct.pack('>h', default_asc)      # ffAscent (word)
    fond_data += struct.pack('>h', default_dsc)      # ffDescent (word)
    fond_data += struct.pack('>h', default_lead)     # ffLeading (word)
    fond_data += struct.pack('>h', default_wid)      # ffWidMax (word)
    fond_data += struct.pack('>i', 0)                # ffWTabOff (long) - no width table
    fond_data += struct.pack('>i', 0)                # ffKernOff (long) - no kern table
    fond_data += struct.pack('>i', 0)                # ffStylOff (long) - no style mapping
    for _ in range(9):
        fond_data += struct.pack('>h', 0)            # ffProperty[0..8] (9 words)
    fond_data += struct.pack('>h', 0)                # ffIntl[0] (word)
    fond_data += struct.pack('>h', 0)                # ffIntl[1] (word)
    fond_data += struct.pack('>h', 2)                # ffVersion (word)

    # Font Association Table
    num_entries = len(MONACO_METRICS)
    fond_data += struct.pack('>h', num_entries - 1)  # count - 1

    for i, (pt, cw, ch, asc, dsc, lead) in enumerate(MONACO_METRICS):
        fond_data += struct.pack('>h', pt)               # point size
        fond_data += struct.pack('>h', 0)                # style (0 = plain)
        fond_data += struct.pack('>h', NFNT_BASE_ID + i) # NFNT resource ID

    fond_hex = fond_data.hex().upper()
    for j in range(0, len(fond_hex), 64):
        chunk = fond_hex[j:j+64]
        print(f'    $"{chunk}"')

    print("};")
    print("")

    # Also output the C lookup function as a comment for reference
    print("/* Unicode -> symbol font char code mapping (for console.c) */")
    print("/*")
    for ucp in sorted(GLYPH_MAP.keys()):
        code = GLYPH_MAP[ucp]
        print(f"   U+{ucp:04X} -> 0x{code:02X}")
    print("*/")


if __name__ == '__main__':
    emit_rez()
