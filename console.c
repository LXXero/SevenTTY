/*
 * SevenTTY (based on ssheven by cy384)
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#include "app.h"
#include "console.h"
#include "net.h"
#include "telnet.h"
#include "unicode.h"

#include <string.h>

#include <Sound.h>

#include <vterm.h>

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* active session for a given window context */
#define WC_S(wc) sessions[(wc)->session_ids[(wc)->active_session_idx]]

/* sentinel values for default colors — negative to avoid colliding with
   xterm 256-color palette indices (0-255) */
#define COLOR_DEFAULT_FG (-1)
#define COLOR_DEFAULT_BG (-2)

/* packed RGB: bit 24 set = true-color RGB, bits 16-23=R, 8-15=G, 0-7=B.
   always positive and > 255, so no collision with sentinels or palette. */
#define COLOR_IS_RGB(c)       ((c) > 255)
#define COLOR_PACK_RGB(r,g,b) (0x01000000 | ((r) << 16) | ((g) << 8) | (b))

/* extract abstract color ID from a VTermScreenCell color field.
   returns sentinel for default colors, palette index for indexed colors,
   or packed RGB for true-color. */
static int extract_fg(VTermScreenCell* cell)
{
	if (VTERM_COLOR_IS_DEFAULT_FG(&cell->fg)) return COLOR_DEFAULT_FG;
	if (VTERM_COLOR_IS_INDEXED(&cell->fg))
		return cell->fg.indexed.idx;
	/* true-color RGB — pack into int */
	return COLOR_PACK_RGB(cell->fg.rgb.red, cell->fg.rgb.green, cell->fg.rgb.blue);
}

static int extract_bg(VTermScreenCell* cell)
{
	if (VTERM_COLOR_IS_DEFAULT_BG(&cell->bg)) return COLOR_DEFAULT_BG;
	if (VTERM_COLOR_IS_INDEXED(&cell->bg))
		return cell->bg.indexed.idx;
	/* true-color RGB — pack into int */
	return COLOR_PACK_RGB(cell->bg.rgb.red, cell->bg.rgb.green, cell->bg.rgb.blue);
}

/* compute RGBColor for xterm 256-color palette index 16-255.
   indices 16-231 = 6x6x6 RGB cube, 232-255 = 24-step grayscale ramp.
   Mac RGBColor uses 16-bit per channel (0-65535). */
static void xterm256_to_rgb(int idx, RGBColor* c)
{
	if (idx < 232)
	{
		/* 6x6x6 color cube: index = 16 + 36*r + 6*g + b */
		static const unsigned short cube[] = {0, 0x5F5F, 0x8787, 0xAFAF, 0xD7D7, 0xFFFF};
		int i = idx - 16;
		c->red   = cube[i / 36];
		c->green = cube[(i / 6) % 6];
		c->blue  = cube[i % 6];
	}
	else
	{
		/* grayscale ramp: 232=rgb(8,8,8) .. 255=rgb(238,238,238) */
		unsigned short g = (8 + (idx - 232) * 10) * 257;
		c->red = g;
		c->green = g;
		c->blue = g;
	}
}

/* convert 8-bit RGB to nearest xterm 256-color index (for scrollback storage) */
static unsigned char rgb_to_xterm256(unsigned char r, unsigned char g, unsigned char b)
{
	int ri, gi, bi;
	if (r == g && g == b)
	{
		if (r < 8) return 16;
		if (r > 248) return 231;
		return 232 + (r - 8) / 10;
	}
	ri = (r > 47) ? (r - 35) / 40 : 0;
	gi = (g > 47) ? (g - 35) / 40 : 0;
	bi = (b > 47) ? (b - 35) / 40 : 0;
	return 16 + 36 * ri + 6 * gi + bi;
}

/* extract color byte for scrollback storage (0-255 index) */
static unsigned char vtcolor_to_byte(const VTermColor* vc)
{
	if (VTERM_COLOR_IS_INDEXED(vc))
		return vc->indexed.idx;
	return rgb_to_xterm256(vc->rgb.red, vc->rgb.green, vc->rgb.blue);
}

/* resolve abstract color ID to drawing color.
   sentinels map to theme_fg/theme_bg, 0-15 to palette, 16-255 to xterm cube/gray,
   packed RGB (> 255) to direct true-color. */
static void set_draw_fg(int idx)
{
	RGBColor c;
	if (idx == COLOR_DEFAULT_FG)
		RGBForeColor(&prefs.theme_fg);
	else if (idx == COLOR_DEFAULT_BG)
		RGBForeColor(&prefs.theme_bg);
	else if (COLOR_IS_RGB(idx))
	{
		c.red   = ((idx >> 16) & 0xFF) * 0x0101;
		c.green = ((idx >> 8) & 0xFF) * 0x0101;
		c.blue  = (idx & 0xFF) * 0x0101;
		RGBForeColor(&c);
	}
	else if (idx < 16)
		RGBForeColor(&prefs.palette[idx]);
	else
	{
		xterm256_to_rgb(idx, &c);
		RGBForeColor(&c);
	}
}

static void set_draw_bg(int idx)
{
	RGBColor c;
	if (idx == COLOR_DEFAULT_BG)
		RGBBackColor(&prefs.theme_bg);
	else if (idx == COLOR_DEFAULT_FG)
		RGBBackColor(&prefs.theme_fg);
	else if (COLOR_IS_RGB(idx))
	{
		c.red   = ((idx >> 16) & 0xFF) * 0x0101;
		c.green = ((idx >> 8) & 0xFF) * 0x0101;
		c.blue  = (idx & 0xFF) * 0x0101;
		RGBBackColor(&c);
	}
	else if (idx < 16)
		RGBBackColor(&prefs.palette[idx]);
	else
	{
		xterm256_to_rgb(idx, &c);
		RGBBackColor(&c);
	}
}


static int symbol_font_lookup(uint32_t cp);

/* convert UTF-8 string to Mac Roman, writing to out buffer.
   returns number of bytes written (not including null terminator).
   unknown chars become '?' */
int utf8_to_macroman(const char* utf8, int utf8_len, char* out, int out_max)
{
	int i = 0, o = 0;
	while (i < utf8_len && o < out_max - 1)
	{
		unsigned char b = (unsigned char)utf8[i];
		uint32_t cp;
		if (b < 0x80)
		{
			cp = b;
			i++;
		}
		else if ((b & 0xE0) == 0xC0 && i + 1 < utf8_len)
		{
			cp = ((b & 0x1F) << 6) | (utf8[i+1] & 0x3F);
			i += 2;
		}
		else if ((b & 0xF0) == 0xE0 && i + 2 < utf8_len)
		{
			cp = ((b & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F);
			i += 3;
		}
		else if ((b & 0xF8) == 0xF0 && i + 3 < utf8_len)
		{
			cp = ((b & 0x07) << 18) | ((utf8[i+1] & 0x3F) << 12) |
			     ((utf8[i+2] & 0x3F) << 6) | (utf8[i+3] & 0x3F);
			i += 4;
		}
		else
		{
			i++;
			continue;
		}

		if (cp < 128)
			out[o++] = (char)cp;
		else if (cp <= 0xFFFF)
			out[o++] = UNICODE_BMP_NORMALIZER[cp];

		else
			out[o++] = '?';
	}
	out[o] = '\0';
	return o;
}

/* symbol font lookup: returns char code (0x20-0x7E) or -1 if not in font */
static int symbol_font_lookup(uint32_t cp)
{
	/* box drawing: U+2500-U+257F */
	if (cp >= 0x2500 && cp <= 0x257F)
	{
		switch (cp)
		{
			case 0x2500: return 0x9B;  /* ─ (not 0x20 — space char doesn't render) */
			case 0x2501: return 0x4C;  /* ━ */
			case 0x2502: return 0x21;  /* │ */
			case 0x2503: return 0x4D;  /* ┃ */
			case 0x2504: return 0x4F;  /* ┄ */
			case 0x250C: return 0x22;  /* ┌ */
			case 0x2510: return 0x23;  /* ┐ */
			case 0x2514: return 0x24;  /* └ */
			case 0x2518: return 0x25;  /* ┘ */
			case 0x251C: return 0x26;  /* ├ */
			case 0x2524: return 0x27;  /* ┤ */
			case 0x252C: return 0x28;  /* ┬ */
			case 0x2534: return 0x29;  /* ┴ */
			case 0x253C: return 0x2A;  /* ┼ */
			case 0x254B: return 0x4E;  /* ╋ */
			case 0x2550: return 0x2B;  /* ═ */
			case 0x2551: return 0x2C;  /* ║ */
			case 0x2552: return 0x36;  /* ╒ */
			case 0x2553: return 0x37;  /* ╓ */
			case 0x2554: return 0x2D;  /* ╔ */
			case 0x2555: return 0x38;  /* ╕ */
			case 0x2556: return 0x39;  /* ╖ */
			case 0x2557: return 0x2E;  /* ╗ */
			case 0x2558: return 0x3A;  /* ╘ */
			case 0x2559: return 0x3B;  /* ╙ */
			case 0x255A: return 0x2F;  /* ╚ */
			case 0x255B: return 0x3C;  /* ╛ */
			case 0x255C: return 0x3D;  /* ╜ */
			case 0x255D: return 0x30;  /* ╝ */
			case 0x255E: return 0x3E;  /* ╞ */
			case 0x255F: return 0x3F;  /* ╟ */
			case 0x2560: return 0x31;  /* ╠ */
			case 0x2561: return 0x40;  /* ╡ */
			case 0x2562: return 0x41;  /* ╢ */
			case 0x2563: return 0x32;  /* ╣ */
			case 0x2564: return 0x42;  /* ╤ */
			case 0x2565: return 0x43;  /* ╥ */
			case 0x2566: return 0x33;  /* ╦ */
			case 0x2567: return 0x44;  /* ╧ */
			case 0x2568: return 0x45;  /* ╨ */
			case 0x2569: return 0x34;  /* ╩ */
			case 0x256A: return 0x46;  /* ╪ */
			case 0x256B: return 0x47;  /* ╫ */
			case 0x256C: return 0x35;  /* ╬ */
			case 0x2574: return 0x48;  /* ╴ */
			case 0x2575: return 0x49;  /* ╵ */
			case 0x2576: return 0x4A;  /* ╶ */
			case 0x2577: return 0x4B;  /* ╷ */
			/* rounded corners -> same as regular corners */
			case 0x256D: return 0x22;  /* ╭ -> ┌ */
			case 0x256E: return 0x23;  /* ╮ -> ┐ */
			case 0x256F: return 0x25;  /* ╯ -> ┘ */
			case 0x2570: return 0x24;  /* ╰ -> └ */
			/* dashed horizontal variants -> triple dash */
			case 0x2505: return 0x4F;  /* ┅ -> ┄ */
			case 0x2508: return 0x4F;  /* ┈ -> ┄ */
			case 0x2509: return 0x4F;  /* ┉ -> ┄ */
			case 0x254C: return 0x4F;  /* ╌ -> ┄ */
			case 0x254D: return 0x4F;  /* ╍ -> ┄ */
			/* dashed vertical variants -> vertical line */
			case 0x2506: return 0x21;  /* ┆ -> │ */
			case 0x2507: return 0x4D;  /* ┇ -> ┃ */
			case 0x250A: return 0x21;  /* ┊ -> │ */
			case 0x250B: return 0x4D;  /* ┋ -> ┃ */
			case 0x254E: return 0x21;  /* ╎ -> │ */
			case 0x254F: return 0x4D;  /* ╏ -> ┃ */
			/* heavy half lines */
			case 0x2578: return 0x48;  /* ╸ -> ╴ */
			case 0x2579: return 0x49;  /* ╹ -> ╵ */
			case 0x257A: return 0x4A;  /* ╺ -> ╶ */
			case 0x257B: return 0x4B;  /* ╻ -> ╷ */
			case 0x257C: return 0x9B;  /* ╼ -> ─ */
			case 0x257D: return 0x21;  /* ╽ -> │ */
			case 0x257E: return 0x9B;  /* ╾ -> ─ */
			case 0x257F: return 0x21;  /* ╿ -> │ */
			/* bold corners -> regular corners */
			case 0x2512: return 0x23;  /* ┒ -> ┐ */
			case 0x2513: return 0x23;  /* ┓ -> ┐ */
			case 0x2516: return 0x24;  /* ┖ -> └ */
			case 0x2517: return 0x24;  /* ┗ -> └ */
			case 0x251A: return 0x25;  /* ┚ -> ┘ */
			case 0x251B: return 0x25;  /* ┛ -> ┘ */
			case 0x250E: return 0x22;  /* ┎ -> ┌ */
			case 0x250F: return 0x22;  /* ┏ -> ┌ */
			case 0x2511: return 0x23;  /* ┑ -> ┐ */
			default: return -1;
		}
	}
	/* block elements: U+2580-U+259F */
	if (cp >= 0x2580 && cp <= 0x259F)
	{
		if (cp <= 0x258F) return 0x50 + (cp - 0x2580);
		if (cp == 0x2590) return 0x60;
		if (cp >= 0x2591 && cp <= 0x2595)
		{
			switch (cp)
			{
				case 0x2591: return 0x61;
				case 0x2592: return 0x62;
				case 0x2593: return 0x63;
				case 0x2594: return 0x64;
				case 0x2595: return 0x65;
			}
		}
		if (cp >= 0x2596 && cp <= 0x259F)
			return 0x90 + (cp - 0x2596);
		return -1;
	}
	/* geometric shapes */
	switch (cp)
	{
		case 0x25A0: return 0x66;
		case 0x25A1: return 0x67;
		case 0x25AA: return 0x68;
		case 0x25AB: return 0x69;
		case 0x25B2: return 0x6A;
		case 0x25B6: return 0x6B;
		case 0x25BC: return 0x6C;
		case 0x25C0: return 0x6D;
		case 0x25CB: return 0x6E;
		case 0x25CF: return 0x6F;
	}
	/* card suits, smileys, music notes, etc */
	switch (cp)
	{
		case 0x2660: return 0x70;
		case 0x2663: return 0x71;
		case 0x2665: return 0x72;
		case 0x2666: return 0x73;
		case 0x263A: return 0x74;
		case 0x263B: return 0x75;
		case 0x266A: return 0x76;
		case 0x266B: return 0x77;
		case 0x2713: return 0x78;
		case 0x2717: return 0x79;
		case 0x2190: return 0x7A;
		case 0x2191: return 0x7B;
		case 0x2192: return 0x7C;
		case 0x2193: return 0x7D;
		case 0x2194: return 0x7E;
	}
	/* small triangle variants -> use same glyph as full-size */
	switch (cp)
	{
		case 0x25B4: return 0x6A;  /* ▴ small up triangle -> ▲ */
		case 0x25B8: return 0x6B;  /* ▸ small right triangle -> ▶ */
		case 0x25BE: return 0x6C;  /* ▾ small down triangle -> ▼ */
		case 0x25C2: return 0x6D;  /* ◂ small left triangle -> ◀ */
	}
	/* Claude CLI: prompt marker + search icon */
	if (cp == 0x23F5) return 0x7F;  /* ⏵ */
	if (cp == 0x276F) return 0x9A;  /* ❯ pointer / prompt marker */
	if (cp == 0x2315) return 0x8F;  /* ⌕ telephone recorder / search icon */
	/* Claude CLI: spinner dingbats */
	switch (cp)
	{
		case 0x2217: return 0x80;  /* ∗ */
		case 0x2722: return 0x81;  /* ✢ */
		case 0x2733: return 0x82;  /* ✳ */
		case 0x2736: return 0x82;  /* ✶ six-pointed star (Claude spinner) */
		case 0x273B: return 0x83;  /* ✻ */
		case 0x273D: return 0x84;  /* ✽ */
	}
	/* Claude CLI: braille spinner */
	switch (cp)
	{
		case 0x280B: return 0x85;  /* ⠋ */
		case 0x2819: return 0x86;  /* ⠙ */
		case 0x2839: return 0x87;  /* ⠹ */
		case 0x2838: return 0x88;  /* ⠸ */
		case 0x283C: return 0x89;  /* ⠼ */
		case 0x2834: return 0x8A;  /* ⠴ */
		case 0x2826: return 0x8B;  /* ⠦ */
		case 0x2827: return 0x8C;  /* ⠧ */
		case 0x2807: return 0x8D;  /* ⠇ */
		case 0x280F: return 0x8E;  /* ⠏ */
	}
	/* middle dot / separator */
	switch (cp)
	{
		case 0x00B7: return 0xAD;  /* · middle dot */
		case 0x22C5: return 0xAD;  /* ⋅ dot operator */
		case 0x2027: return 0xAD;  /* ‧ hyphenation point */
		case 0x2024: return 0xAD;  /* ․ one dot leader */
		case 0x2219: return 0xAD;  /* ∙ bullet operator */
	}
	/* figures/UI chars: diamonds, checks, boxes, stars, etc. */
	switch (cp)
	{
		case 0x25C6: return 0x9C;  /* ◆ black diamond (lozenge) */
		case 0x25C7: return 0x9D;  /* ◇ white diamond */
		case 0x2714: return 0x9E;  /* ✔ heavy check mark (tick) */
		case 0x2718: return 0x9F;  /* ✘ heavy ballot X (cross) */
		case 0x25FB: return 0xA0;  /* ◻ white medium square */
		case 0x25FC: return 0xA1;  /* ◼ black medium square */
		case 0x25C9: return 0xA2;  /* ◉ fisheye (radioOn) */
		case 0x25EF: return 0xA3;  /* ◯ large circle (radioOff) */
		case 0x2610: return 0xA4;  /* ☐ ballot box (checkboxOff) */
		case 0x2612: return 0xA5;  /* ☒ ballot box with X (checkboxOn) */
		case 0x2605: return 0xA6;  /* ★ black star */
		case 0x2630: return 0xA7;  /* ☰ trigram/hamburger */
		case 0x25B3: return 0xA8;  /* △ white up triangle */
		case 0x26A0: return 0xA9;  /* ⚠ warning sign */
		case 0x25CE: return 0xAA;  /* ◎ bullseye */
		case 0x25CC: return 0xAB;  /* ◌ dotted circle */
		case 0x2139: return 0xAC;  /* ℹ info */
	}
	return -1;
}

char key_to_vterm[256] = { VTERM_KEY_NONE };

int font_ascent = 0;

void setup_key_translation(void)
{
	// TODO: figure out how to translate the rest of these
	key_to_vterm[kEnterCharCode] = VTERM_KEY_ENTER;
	key_to_vterm[kTabCharCode] = VTERM_KEY_TAB;
	key_to_vterm[kBackspaceCharCode] = VTERM_KEY_BACKSPACE;
	key_to_vterm[kEscapeCharCode] = VTERM_KEY_ESCAPE;
	key_to_vterm[kUpArrowCharCode] = VTERM_KEY_UP;
	key_to_vterm[kDownArrowCharCode] = VTERM_KEY_DOWN;
	key_to_vterm[kLeftArrowCharCode] = VTERM_KEY_LEFT;
	key_to_vterm[kRightArrowCharCode] = VTERM_KEY_RIGHT;
	//key_to_vterm[0] = VTERM_KEY_INS;
	key_to_vterm[kDeleteCharCode] = VTERM_KEY_DEL;
	key_to_vterm[kHomeCharCode] = VTERM_KEY_HOME;
	key_to_vterm[kEndCharCode] = VTERM_KEY_END;
	key_to_vterm[kPageUpCharCode] = VTERM_KEY_PAGEUP;
	key_to_vterm[kPageDownCharCode] = VTERM_KEY_PAGEDOWN;
}

inline Rect cell_rect(struct window_context* wc, int x, int y, Rect bounds)
{
	short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT + 2 : 2;
	Rect r = { (short) (bounds.top + y * con.cell_height + top_offset), (short) (bounds.left + x * con.cell_width + 2),
		(short) (bounds.top + (y+1) * con.cell_height + top_offset), (short) (bounds.left + (x+1) * con.cell_width + 2) };
	return r;
}

static void print_string_v(VTerm* vt, const char* c)
{
	vterm_input_write(vt, c, strlen(c));
}

void print_string(const char* c)
{
	print_string_v(ACTIVE_S.vterm, c);
}

inline void draw_char(struct window_context* wc, int x, int y, Rect* r, char c)
{
	short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	MoveTo(r->left + 2 + x * con.cell_width, r->top + 2 + font_ascent + (y * con.cell_height) + top_offset);
	DrawChar(c);
}

void toggle_cursor(struct window_context* wc)
{
	WC_S(wc).cursor_state = !WC_S(wc).cursor_state;
	Rect cursor = cell_rect(wc, WC_S(wc).cursor_x, WC_S(wc).cursor_y, wc->win->portRect);
	InvalRect(&cursor);
	wc->needs_redraw = 1;
}

void check_cursor(struct window_context* wc)
{
	long unsigned int now = TickCount();
	if ((now - WC_S(wc).last_cursor_blink) > GetCaretTime())
	{
		toggle_cursor(wc);
		WC_S(wc).last_cursor_blink = now;
	}
}


void point_to_cell(struct window_context* wc, Point p, int* x, int* y)
{
	short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	*x = p.h / con.cell_width;
	*y = (p.v - top_offset) / con.cell_height;

	if (*x > wc->size_x) *x = wc->size_x;
	if (*y > wc->size_y) *y = wc->size_y;
	if (*y < 0) *y = 0;
}

void clear_selection(struct window_context* wc)
{
	WC_S(wc).select_start_x = -1;
	WC_S(wc).select_start_y = -1;
	WC_S(wc).select_end_x = -1;
	WC_S(wc).select_end_y = -1;
}

void damage_selection(struct window_context* wc)
{
	// damage all rows that have part of the selection (TODO make this better)
	Rect topleft = cell_rect(wc, 0, MIN(WC_S(wc).select_start_y, WC_S(wc).select_end_y), (wc->win->portRect));
	Rect bottomright = cell_rect(wc, wc->size_x, MAX(WC_S(wc).select_start_y, WC_S(wc).select_end_y), (wc->win->portRect));

	UnionRect(&topleft, &bottomright, &topleft);
	InvalRect(&topleft);
	wc->needs_redraw = 1;
}

void update_selection_end(struct window_context* wc)
{
	static int last_mouse_cell_x = -1;
	static int last_mouse_cell_y = -1;

	int new_mouse_cell_x;
	int new_mouse_cell_y;

	Point new_mouse;

	GetMouse(&new_mouse);
	point_to_cell(wc, new_mouse, &new_mouse_cell_x, &new_mouse_cell_y);

	// only damage the selection if the mouse has moved outside of the last cell
	if (last_mouse_cell_x != new_mouse_cell_x || last_mouse_cell_y != new_mouse_cell_y)
	{
		// damage the old selection
		damage_selection(wc);

		WC_S(wc).select_end_x = new_mouse_cell_x;
		WC_S(wc).select_end_y = new_mouse_cell_y;

		last_mouse_cell_x = new_mouse_cell_x;
		last_mouse_cell_y = new_mouse_cell_y;

		// damage the new selection
		damage_selection(wc);
	}
}

inline void prev(int size_x, int* x, int* y)
{
	if (*x == 0)
	{
		*x = size_x - 1;
		*y = (*y) - 1;
	}
	else
	{
		*x = (*x) - 1;
	}
}

inline void next(int size_x, int* x, int* y)
{
	if (*x == size_x -1)
	{
		*x = 0;
		*y = (*y) + 1;
	}
	else
	{
		*x = (*x) + 1;
	}
}

void select_word(struct window_context* wc)
{
	Point mouse;
	GetMouse(&mouse);

	VTermPos pos;
	point_to_cell(wc, mouse, &pos.col, &pos.row);
	pos.col++;

	char c;

	VTermScreenCell vtsc = {0};

	// scan backwards from mouse click point
	while (!(pos.col == 0 && pos.row == 0))
	{
		prev(wc->size_x, &pos.col, &pos.row);

		int ok = vterm_screen_get_cell(WC_S(wc).vts, pos, &vtsc);

		// TODO FIXME not really unicode safe
		c = (char)vtsc.chars[0];
		if (c == '\0' || c == ' ')
		{
			next(wc->size_x, &pos.col, &pos.row);
			break;
		}
	}

	WC_S(wc).select_start_x = pos.col;
	WC_S(wc).select_start_y = pos.row;

	// scan forwards from mouse click point
	point_to_cell(wc, mouse, &pos.col, &pos.row);
	pos.col--;

	while (!(pos.col == wc->size_x - 1 && pos.row == wc->size_y -1))
	{
		next(wc->size_x, &pos.col, &pos.row);

		int ok = vterm_screen_get_cell(WC_S(wc).vts, pos, &vtsc);

		// TODO FIXME not really unicode safe
		c = (char)vtsc.chars[0];
		if (c == '\0' || c == ' ')
		{
			break;
		}
	}

	WC_S(wc).select_end_x = pos.col;
	WC_S(wc).select_end_y = pos.row;

	damage_selection(wc);
}

// returns 1 if click was in tab bar, 0 otherwise
int tab_bar_click(struct window_context* wc, Point p)
{
	if (wc->num_sessions <= 1) return 0;
	if (p.v >= TAB_BAR_HEIGHT) return 0;

	// figure out which tab was clicked
	int tab_width = (wc->win->portRect.right - wc->win->portRect.left) / wc->num_sessions;
	int clicked_tab = p.h / tab_width;

	if (clicked_tab >= wc->num_sessions) clicked_tab = wc->num_sessions - 1;

	// map to actual session index via window's session_ids
	if (clicked_tab < wc->num_sessions)
	{
		int sid = wc->session_ids[clicked_tab];

		// check if click is in close button area
		int tab_left = clicked_tab * tab_width;
		int close_left = tab_left + tab_width - 15;
		if (wc->num_sessions > 1 && p.h >= close_left && p.h <= close_left + 11
			&& p.v >= 4 && p.v <= 15)
		{
			close_session(sid);
		}
		else if (sid != wc->session_ids[wc->active_session_idx])
		{
			switch_session(wc, sid);
		}
		return 1;
	}

	return 1;
}

// p is in window local coordinates
void mouse_click(struct window_context* wc, Point p, int click)
{
	static Point last_click;
	static unsigned last_click_time = 0;

	// check if click is in tab bar
	if (click && tab_bar_click(wc, p)) return;

	WC_S(wc).mouse_state = click;

	if (WC_S(wc).mouse_mode == CLICK_SEND)
	{
		short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
		int row = (p.v - top_offset) / con.cell_height;
		int col = p.h / con.cell_width;
		if (row < 0) row = 0;
		vterm_mouse_move(WC_S(wc).vterm, row, col, VTERM_MOD_NONE);
		vterm_mouse_button(WC_S(wc).vterm, 1, click, VTERM_MOD_NONE);
	}
	else if (WC_S(wc).mouse_mode == CLICK_SELECT)
	{
		if (click)
		{
			// damage the old selection so it gets wiped from the screen
			damage_selection(wc);
			last_click = p;

			// if it's a double click
			if (TickCount() - last_click_time < GetDblTime())
			{
				select_word(wc);
			}
			else
			{
				point_to_cell(wc, p, &WC_S(wc).select_start_x, &WC_S(wc).select_start_y);
				point_to_cell(wc, p, &WC_S(wc).select_end_x, &WC_S(wc).select_end_y);

				update_selection_end(wc);
			}

			last_click_time = TickCount();
		}
		else
		{
			int a, b, c, d;
			point_to_cell(wc, last_click, &a, &b);
			point_to_cell(wc, p, &c, &d);
		}
	}
}

size_t get_selection(struct window_context* wc, char** selection)
{
	if (WC_S(wc).select_start_x == -1 || WC_S(wc).select_start_y == -1)
	{
		*selection = NULL;
		return 0;
	}
	int a = WC_S(wc).select_start_x + WC_S(wc).select_start_y * wc->size_x;
	int b = WC_S(wc).select_end_x + WC_S(wc).select_end_y * wc->size_x;

	ssize_t len = MAX(a,b) - MIN(a,b) + 1;

	char* output = malloc(len + 1);
	if (output == NULL) { *selection = NULL; return 0; }

	int start_row = MIN(WC_S(wc).select_start_y, WC_S(wc).select_end_y);
	int start_col = MIN(WC_S(wc).select_start_x, WC_S(wc).select_end_x);

	VTermPos pos = {.row = start_row, .col = start_col-1};
	VTermScreenCell vtsc = {0};

	for(int i = 0; i < len; i++)
	{
		next(wc->size_x, &pos.col, &pos.row);

		int ok = vterm_screen_get_cell(WC_S(wc).vts, pos, &vtsc);

		// TODO FIXME not really unicode safe
		output[i] = (char)vtsc.chars[0];
	}

	output[len] = '\0';

	*selection = output;

	return len;
}

// TODO: find a way to render this once and then just paste it on
inline void draw_resize_corner(struct window_context* wc)
{
	// draw the grow icon in the bottom right corner, but not the scroll bars
	// yes, this is really awkward
	MacRegion bottom_right_corner = { 10, wc->win->portRect};
	MacRegion* brc = &bottom_right_corner;
	MacRegion** old = wc->win->clipRgn;

	bottom_right_corner.rgnBBox.top = bottom_right_corner.rgnBBox.bottom - 15;
	bottom_right_corner.rgnBBox.left = bottom_right_corner.rgnBBox.right - 15;

	wc->win->clipRgn = &brc;
	DrawGrowIcon(wc->win);
	wc->win->clipRgn = old;
}

void draw_tab_bar(struct window_context* wc)
{
	if (wc->num_sessions <= 1) return;

	short save_font      = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	RGBColor save_rgb_fg, save_rgb_bg;
	GetForeColor(&save_rgb_fg);
	GetBackColor(&save_rgb_bg);

	RGBColor rgb_white   = {0xFFFF, 0xFFFF, 0xFFFF};
	RGBColor rgb_black   = {0, 0, 0};
	RGBColor rgb_gray    = {0x9999, 0x9999, 0x9999};
	RGBColor rgb_dimtext = {0x5555, 0x5555, 0x5555};
	RGBColor rgb_shadow  = {0x5555, 0x5555, 0x5555};
	RGBColor rgb_hilite  = {0xDDDD, 0xDDDD, 0xDDDD};

	TextFont(kFontIDGeneva);
	TextSize(9);
	TextFace(normal);

	Rect portRect = wc->win->portRect;
	int total_width = portRect.right - portRect.left;
	int tab_width = total_width / wc->num_sessions;

	int active_sid = wc->session_ids[wc->active_session_idx];

	int tab_idx;
	for (tab_idx = 0; tab_idx < wc->num_sessions; tab_idx++)
	{
		int sid = wc->session_ids[tab_idx];

		Rect tab_rect;
		tab_rect.left = portRect.left + tab_idx * tab_width;
		tab_rect.right = (tab_idx == wc->num_sessions - 1) ? portRect.right : tab_rect.left + tab_width;
		tab_rect.top = portRect.top;
		tab_rect.bottom = portRect.top + TAB_BAR_HEIGHT;

		if (sid == active_sid)
		{
			/* active tab: white background */
			RGBBackColor(&rgb_white);
			RGBForeColor(&rgb_black);
			EraseRect(&tab_rect);

			MoveTo(tab_rect.left, tab_rect.top);
			LineTo(tab_rect.left, tab_rect.bottom - 1);
			LineTo(tab_rect.right - 1, tab_rect.bottom - 1);
			LineTo(tab_rect.right - 1, tab_rect.top);
		}
		else
		{
			/* inactive tab: darker gray with sunken inset border */
			RGBBackColor(&rgb_gray);
			EraseRect(&tab_rect);

			/* sunken border: dark on top-left, light on bottom-right */
			RGBForeColor(&rgb_shadow);
			MoveTo(tab_rect.left, tab_rect.bottom - 1);
			LineTo(tab_rect.left, tab_rect.top);
			LineTo(tab_rect.right - 1, tab_rect.top);

			RGBForeColor(&rgb_hilite);
			MoveTo(tab_rect.right - 1, tab_rect.top + 1);
			LineTo(tab_rect.right - 1, tab_rect.bottom - 1);
			LineTo(tab_rect.left + 1, tab_rect.bottom - 1);
		}

		/* draw tab label */
		RGBForeColor(sid == active_sid ? &rgb_black : &rgb_dimtext);
		{
			char* label = sessions[sid].tab_label;
			int label_len = strlen(label);
			int max_chars = (tab_width - 20) / CharWidth('M');
			if (label_len > max_chars) label_len = max_chars;

			MoveTo(tab_rect.left + 4, tab_rect.top + 14);
			DrawText(label, 0, label_len);
		}

		/* draw close button (X) */
		if (wc->num_sessions > 1)
		{
			short cx1 = tab_rect.right - 15;
			short cy1 = tab_rect.top + 4;
			short cx2 = cx1 + 11;
			short cy2 = cy1 + 11;

			RGBForeColor(sid == active_sid ? &rgb_black : &rgb_dimtext);
			MoveTo(cx1, cy1);
			LineTo(cx2, cy2);
			MoveTo(cx2, cy1);
			LineTo(cx1, cy2);
		}
	}

	/* separator line between tab bar and terminal area */
	RGBForeColor(&rgb_black);
	MoveTo(portRect.left, portRect.top + TAB_BAR_HEIGHT);
	LineTo(portRect.right, portRect.top + TAB_BAR_HEIGHT);

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);
	RGBForeColor(&save_rgb_fg);
	RGBBackColor(&save_rgb_bg);
}

/* Get a cell accounting for scroll offset.
 * display_row is the row on screen (0..size_y-1).
 * If scrolled back, top rows come from the scrollback buffer,
 * remaining rows from the live vterm screen. */
static int get_cell_scrolled(struct window_context* wc, int display_row, int col, VTermScreenCell* cell)
{
	int sid = wc->session_ids[wc->active_session_idx];
	struct session* s = &sessions[sid];

	if (s->scroll_offset > 0 && display_row < s->scroll_offset)
	{
		/* this row comes from scrollback */
		int sb_idx = s->sb_head - s->scroll_offset + display_row;
		if (sb_idx < 0) sb_idx += SCROLLBACK_LINES;

		memset(cell, 0, sizeof(VTermScreenCell));

		if (col < SCROLLBACK_COLS)
		{
			struct sb_cell* sc = &s->scrollback[sb_idx][col];
			cell->chars[0] = sc->ch;
			cell->width = 1;
			if (sc->attrs & 32)
				cell->fg.type = VTERM_COLOR_DEFAULT_FG;
			else
			{
				cell->fg.type = VTERM_COLOR_INDEXED;
				cell->fg.indexed.idx = sc->fg;
			}
			if (sc->attrs & 64)
				cell->bg.type = VTERM_COLOR_DEFAULT_BG;
			else
			{
				cell->bg.type = VTERM_COLOR_INDEXED;
				cell->bg.indexed.idx = sc->bg;
			}
			cell->attrs.bold = (sc->attrs & 1) ? 1 : 0;
			cell->attrs.reverse = (sc->attrs & 2) ? 1 : 0;
			cell->attrs.underline = (sc->attrs & 4) ? 1 : 0;
			cell->attrs.italic = (sc->attrs & 8) ? 1 : 0;
			cell->attrs.strike = (sc->attrs & 16) ? 1 : 0;
		}
		else
		{
			cell->chars[0] = ' ';
			cell->width = 1;
		}
		return 1;
	}
	else
	{
		/* live screen row */
		VTermPos pos;
		pos.row = display_row - s->scroll_offset;
		pos.col = col;
		return vterm_screen_get_cell(s->vts, pos, cell);
	}
}

/* draw a text run, splitting at symbol font boundaries */
static void draw_run_with_symbols(char* text, char* is_sym, int start, int len)
{
	int i = 0;
	while (i < len)
	{
		int sub_start = i;
		int sub_sym = is_sym[start + i];
		while (i < len && is_sym[start + i] == sub_sym)
			i++;
		if (sub_sym)
			TextFont(SYMF_FAMILY_ID);
		else
			TextFont(kFontIDMonaco);
		DrawText(text, start + sub_start, i - sub_start);
	}
}

void draw_screen_color(struct window_context* wc, Rect* r)
{
	/* save/restore font and color state */
	short save_font      = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	RGBColor save_fg, save_bg;
	GetForeColor(&save_fg);
	GetBackColor(&save_bg);

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);
	RGBBackColor(&prefs.theme_bg);
	RGBForeColor(&prefs.theme_fg);
	TextMode(srcOr);

	int select_start = -1;
	int select_end = -1;
	int i = 0;

	// only draw selection if we're not in clicky mode
	if (WC_S(wc).mouse_mode == CLICK_SELECT)
	{
		if (WC_S(wc).mouse_state) update_selection_end(wc);

		if (WC_S(wc).select_start_x != -1)
		{
			int a = WC_S(wc).select_start_x + WC_S(wc).select_start_y * wc->size_x;
			int b = WC_S(wc).select_end_x + WC_S(wc).select_end_y * wc->size_x;

			if (a < b)
			{
				select_start = a;
				select_end = b;
			}
			else
			{
				select_start = b;
				select_end = a;
			}
		}
	}

	VTermScreenCell vtsc = {0};
	VTermScreenCell run_start = {0};
	VTermPos pos = {.row = 0, .col = 0};

	char row_text[wc->size_x];
	char row_is_symbol[wc->size_x];
	Rect run_rect;
	short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	int vertical_offset = r->top + font_ascent + 2 + top_offset;
	int run_start_col, run_length;
	short run_face, next_face;
	int run_inverted;
	int run_fg, run_bg;
	int ok = 0;

	for (pos.row = 0; pos.row < wc->size_y; pos.row++)
	{
		pos.col = 0;
		ok = get_cell_scrolled(wc, pos.row, 0, &run_start);
		run_length = 0;
		run_start_col = 0;

		run_rect = cell_rect(wc, 0, pos.row, wc->win->portRect);

		run_inverted = run_start.attrs.reverse ^ (i < select_end && i >= select_start);

		run_fg = extract_fg(&run_start);
		run_bg = extract_bg(&run_start);

		run_face = normal;
		if (run_start.attrs.bold) run_face |= (condense|bold);
		if (run_start.attrs.italic) run_face |= (condense|italic);
		if (run_start.attrs.underline) run_face |= underline;

		for (pos.col = 0; pos.col < wc->size_x; pos.col++)
		{
			int cell_fg, cell_bg;
			int sym_code;
			ok = get_cell_scrolled(wc, pos.row, pos.col, &vtsc);

			uint32_t glyph = vtsc.chars[0];
			uint32_t orig_cp = glyph;

			if (glyph == 0) glyph = ' ';

			/* For scrollback cells, strike bit = symbol font flag (we set it).
			   For live cells, strike = actual strikethrough — ignore it. */
			if (WC_S(wc).scroll_offset > 0 && pos.row < WC_S(wc).scroll_offset)
				row_is_symbol[pos.col] = vtsc.attrs.strike ? 1 : 0;
			else
				row_is_symbol[pos.col] = 0;

			// normalize some unicode (only for live cells — scrollback is pre-normalized)
			if (glyph > 127 && !(WC_S(wc).scroll_offset > 0 && pos.row < WC_S(wc).scroll_offset))
			{
				/* try symbol font first — handles chars like U+00B7 that
				   have Mac Roman equivalents but render poorly in Monaco */
				sym_code = symbol_font_lookup(orig_cp);
				if (sym_code >= 0)
				{
					glyph = sym_code;
					row_is_symbol[pos.col] = 1;
				}
				else if (glyph <= 0xFFFF)
				{
					glyph = UNICODE_BMP_NORMALIZER[glyph];
					/* if we got lozenge fallback, try Mac Roman overrides */
					if ((char)glyph == MAC_ROMAN_LOZENGE && orig_cp > 127)
					{
						switch (orig_cp)
						{
							case 0x2014: glyph = 0xD1; break; /* em dash */
							case 0x2013: glyph = 0xD0; break; /* en dash */
							case 0x2018: glyph = 0xD4; break; /* left single quote */
							case 0x2019: glyph = 0xD5; break; /* right single quote */
							case 0x201C: glyph = 0xD2; break; /* left double quote */
							case 0x201D: glyph = 0xD3; break; /* right double quote */
							case 0x2026: glyph = 0xC9; break; /* ellipsis */
							case 0x2122: glyph = 0xAA; break; /* trademark */
						}
					}
				}
				else
				{
					glyph = MAC_ROMAN_LOZENGE;
				}
			}

			row_text[pos.col] = glyph;

			next_face = normal;
			if (vtsc.attrs.bold) next_face |= (condense|bold);
			if (vtsc.attrs.italic) next_face |= (condense|italic);
			if (vtsc.attrs.underline) next_face |= underline;

			cell_fg = extract_fg(&vtsc);
			cell_bg = extract_bg(&vtsc);

			/* if we cannot add this cell to the run */
			if (cell_fg != run_fg || cell_bg != run_bg || next_face != run_face || (vtsc.attrs.reverse ^ (i < select_end && i >= select_start)) != run_inverted)
			{
				/* draw what we've got so far */
				if (run_inverted)
				{
					set_draw_bg(run_fg);
					set_draw_fg(run_bg);
				}
				else
				{
					set_draw_bg(run_bg);
					set_draw_fg(run_fg);
				}

				EraseRect(&run_rect);
				MoveTo(run_rect.left, vertical_offset);
				TextFace(run_face);
				TextFont(kFontIDMonaco);
				draw_run_with_symbols(row_text, row_is_symbol, run_start_col, run_length);

				/* then reset everything to start a new run */
				run_inverted = vtsc.attrs.reverse ^ (i < select_end && i >= select_start);
				run_fg = cell_fg;
				run_bg = cell_bg;
				run_face = next_face;
				run_start_col = pos.col;
				run_rect = cell_rect(wc, pos.col, pos.row, wc->win->portRect);
				run_length = 1;
			}
			else
			{
				run_length++;
			}

			/* if we're at the last cell in the row, draw the run */
			if (pos.col == wc->size_x - 1)
			{
				if (run_inverted)
				{
					set_draw_bg(run_fg);
					set_draw_fg(run_bg);
				}
				else
				{
					set_draw_bg(run_bg);
					set_draw_fg(run_fg);
				}

				EraseRect(&run_rect);
				MoveTo(run_rect.left, vertical_offset);
				TextFace(run_face);
				TextFont(kFontIDMonaco);
				draw_run_with_symbols(row_text, row_is_symbol, run_start_col, run_length);
			}
			else
			{
				run_rect.right += con.cell_width;
			}

			i++;
		}

		vertical_offset += con.cell_height;
	}

	TextFont(kFontIDMonaco);

	/* do the cursor if needed */
	if (WC_S(wc).cursor_state && WC_S(wc).cursor_visible)
	{
		Rect cursor = cell_rect(wc, WC_S(wc).cursor_x, WC_S(wc).cursor_y, wc->win->portRect);
		InvertRect(&cursor);
	}

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);
	RGBForeColor(&save_fg);
	RGBBackColor(&save_bg);

	draw_resize_corner(wc);
}

void erase_row(struct window_context* wc, int i)
{
	Rect left = cell_rect(wc, 0, i, (wc->win->portRect));
	Rect right = cell_rect(wc, wc->size_x, i, (wc->win->portRect));
	right.right -= 8;

	UnionRect(&left, &right, &left);
	EraseRect(&left);
}

void draw_screen_fast(struct window_context* wc, Rect* r)
{
	// don't clobber font settings
	short save_font      = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	RGBColor save_font_fg, save_font_bg;
	GetForeColor(&save_font_fg);
	GetBackColor(&save_font_bg);

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);

	{
		RGBColor fast_bg, fast_fg;
		int is_dark;

		if (prefs.bg_color == COLOR_FROM_THEME)
		{
			int bright = (prefs.theme_bg.red + prefs.theme_bg.green + prefs.theme_bg.blue) / 3;
			is_dark = (bright <= 0x7FFF);
		}
		else
			is_dark = (prefs.bg_color == blackColor);

		if (is_dark)
		{
			fast_bg.red = fast_bg.green = fast_bg.blue = 0x0000;
			fast_fg.red = fast_fg.green = fast_fg.blue = 0xFFFF;
		}
		else
		{
			fast_bg.red = fast_bg.green = fast_bg.blue = 0xFFFF;
			fast_fg.red = fast_fg.green = fast_fg.blue = 0x0000;
		}

		RGBBackColor(&fast_bg);
		RGBForeColor(&fast_fg);

		if (!is_dark)
			TextMode(srcOr);
		else
			TextMode(srcCopy);
	}

	int select_start = -1;
	int select_end = -1;
	int i = 0;

	if (WC_S(wc).mouse_mode == CLICK_SELECT)
	{
		if (WC_S(wc).mouse_state) update_selection_end(wc);

		if (WC_S(wc).select_start_x != -1)
		{
			int a = WC_S(wc).select_start_x + WC_S(wc).select_start_y * wc->size_x;
			int b = WC_S(wc).select_end_x + WC_S(wc).select_end_y * wc->size_x;

			if (a < b)
			{
				select_start = a;
				select_end = b;
			}
			else
			{
				select_start = b;
				select_end = a;
			}
		}
	}

	VTermPos pos = {.row = 0, .col = 0};

	char row_text[wc->size_x];
	char row_invert[wc->size_x];
	char row_is_symbol[wc->size_x];
	Rect cr;

	VTermScreenCell here = {0};

	short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	int vertical_offset = r->top + font_ascent + 2 + top_offset;

	for (pos.row = 0; pos.row < wc->size_y; pos.row++)
	{
		erase_row(wc, pos.row);

		for (pos.col = 0; pos.col < wc->size_x; pos.col++)
		{
			int ret = get_cell_scrolled(wc, pos.row, pos.col, &here);

			uint32_t glyph = here.chars[0];
			uint32_t orig_cp = glyph;
			int sym_code;

			if (glyph == 0) glyph = ' ';

			row_is_symbol[pos.col] = 0;

			// normalize some unicode
			if (glyph > 127)
			{
				sym_code = symbol_font_lookup(orig_cp);
				if (sym_code >= 0)
				{
					glyph = sym_code;
					row_is_symbol[pos.col] = 1;
				}
				else if (glyph <= 0xFFFF)
				{
					glyph = UNICODE_BMP_NORMALIZER[glyph];
				}
				else
				{
					glyph = MAC_ROMAN_LOZENGE;
				}
			}

			row_text[pos.col] = glyph;

			bool invert = here.attrs.reverse ^ (i < select_end && i >= select_start);
			row_invert[pos.col] = invert;

			i++;
		}

		MoveTo(r->left + 2, vertical_offset);
		TextFont(kFontIDMonaco);
		draw_run_with_symbols(row_text, row_is_symbol, 0, wc->size_x);

		for (int i = 0; i < wc->size_x; i++)
		{
			if (row_invert[i])
			{
				cr = cell_rect(wc, i, pos.row, wc->win->portRect);
				InvertRect(&cr);
			}
		}
		vertical_offset += con.cell_height;
	}

	// do the cursor if needed
	if (WC_S(wc).cursor_state && WC_S(wc).cursor_visible)
	{
		Rect cursor = cell_rect(wc, WC_S(wc).cursor_x, WC_S(wc).cursor_y, wc->win->portRect);
		InvertRect(&cursor);
	}

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);
	RGBForeColor(&save_font_fg);
	RGBBackColor(&save_font_bg);

	//draw_resize_corner(wc);
}

void draw_screen(struct window_context* wc, Rect* r)
{
	draw_tab_bar(wc);

	if (prefs.display_mode == FASTEST)
	{
		draw_screen_fast(wc, r);
	}
	else
	{
		draw_screen_color(wc, r);
	}
}

void ruler(struct window_context* wc, Rect* r)
{
	char itoc[] = {'0','1','2','3','4','5','6','7','8','9'};

	for (int x = 0; x < wc->size_x; x++)
		for (int y = 0; y < wc->size_y; y++)
			draw_char(wc, x, y, r, itoc[x%10]);
}

int is_printable(char c)
{
	if (c >= 32 && c <= 126) return 1; else return 0;
}

static void print_int_v(VTerm* vt, int d)
{
	char itoc[] = {'0','1','2','3','4','5','6','7','8','9'};

	char buffer[12] = {0};
	int i = 10;
	int negative = 0;

	if (d == 0)
	{
		buffer[0] = '0';
		i = -1;
	}

	if (d < 0)
	{
		negative = 1;
		d *= -1;
	}

	for (; d > 0; i--)
	{
		buffer[i] = itoc[d % 10];
		d /= 10;
	}

	if (negative) buffer[i] = '-';

	print_string_v(vt, buffer+i+1-negative);
}

void print_int(int d)
{
	print_int_v(ACTIVE_S.vterm, d);
}

static void vprintf_to_vterm(VTerm* vt, const char* str, va_list args)
{
	while (*str != '\0')
	{
		if (*str == '%')
		{
			int is_long = 0;
			str++;
			if (*str == 'l') { is_long = 1; str++; }
			switch (*str)
			{
				case 'd':
					if (is_long)
						print_int_v(vt, (int)va_arg(args, long));
					else
						print_int_v(vt, va_arg(args, int));
					break;
				case 's':
					print_string_v(vt, va_arg(args, char*));
					break;
				case '\0':
					vterm_input_write(vt, str-1, 1);
					break;
				default:
					va_arg(args, int);
					vterm_input_write(vt, str-1-is_long, 2+is_long);
					break;
			}
		}
		else
		{
			vterm_input_write(vt, str, 1);
		}

		str++;
	}
}

void printf_i(const char* str, ...)
{
	va_list args;
	va_start(args, str);
	vprintf_to_vterm(ACTIVE_S.vterm, str, args);
	va_end(args);
}

void printf_s(int session_idx, const char* str, ...)
{
	if (sessions[session_idx].vterm == NULL) return;
	va_list args;
	va_start(args, str);
	vprintf_to_vterm(sessions[session_idx].vterm, str, args);
	va_end(args);
}

int bell(void* user)
{
	SysBeep(30);

	return 1;
}

int movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	int idx = (int)(intptr_t)user;
	struct session* s = &sessions[idx];
	struct window_context* wc = window_for_session(idx);

	if (wc != NULL && idx == wc->session_ids[wc->active_session_idx]
		&& wc->win != NULL)
	{
		wc->needs_redraw = 1;
		SetPort(wc->win);
		/* invalidate old and new cursor positions */
		{
			Rect old_r = cell_rect(wc, s->cursor_x, s->cursor_y, wc->win->portRect);
			Rect new_r = cell_rect(wc, pos.col, pos.row, wc->win->portRect);
			InvalRect(&old_r);
			InvalRect(&new_r);
		}
	}

	s->cursor_x = pos.col;
	s->cursor_y = pos.row;

	return 1;
}

int damage(VTermRect rect, void *user)
{
	int idx = (int)(intptr_t)user;
	struct window_context* wc = window_for_session(idx);

	if (wc == NULL || idx != wc->session_ids[wc->active_session_idx] || wc->win == NULL) return 1;

	// invalidate so BeginUpdate/EndUpdate has a valid update region
	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));
	wc->needs_redraw = 1;

	return 1;
}

int settermprop(VTermProp prop, VTermValue *val, void *user)
{
	int idx = (int)(intptr_t)user;
	struct session* s = &sessions[idx];
	struct window_context* wc = window_for_session(idx);

	switch (prop)
	{
		case VTERM_PROP_TITLE: // string
			{
				char mr_title[256];
				int mr_len = utf8_to_macroman(val->string.str, val->string.len, mr_title, sizeof(mr_title));
				if (wc != NULL && idx == wc->session_ids[wc->active_session_idx])
					set_window_title(wc->win, mr_title, mr_len);
				/* also update tab label */
				if (mr_len > 63) mr_len = 63;
				strncpy(s->tab_label, mr_title, mr_len);
				s->tab_label[mr_len] = '\0';
			}
			return 1;
		case VTERM_PROP_CURSORVISIBLE: // bool
			s->cursor_visible = val->boolean;
			return 1;
		case VTERM_PROP_MOUSE: // number
			s->mouse_mode = (val->number == VTERM_PROP_MOUSE_CLICK) ? CLICK_SEND : CLICK_SELECT;
			if (wc != NULL && idx == wc->session_ids[wc->active_session_idx])
			{
				damage_selection(wc);
				clear_selection(wc);
			}
			return 1;
		case VTERM_PROP_ALTSCREEN: // bool
		case VTERM_PROP_ICONNAME: // string
		case VTERM_PROP_REVERSE: //bool
		case VTERM_PROP_CURSORSHAPE: // number
		case VTERM_PROP_CURSORBLINK: // bool
		default:
			break;
	}

	return 0;
}

void output_callback(const char *s, size_t len, void *user)
{
	int idx = (int)(intptr_t)user;
	ssh_write_s(idx, (char*)s, len);
}

// local shell sessions don't send vterm output anywhere
void local_output_callback(const char *s, size_t len, void *user)
{
	// no-op: local shell handles I/O directly
}

static int sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
	int idx = (int)(intptr_t)user;
	struct session* s = &sessions[idx];
	int store_cols = cols < SCROLLBACK_COLS ? cols : SCROLLBACK_COLS;
	int i;

	/* convert VTermScreenCell to compact sb_cell */
	for (i = 0; i < store_cols; i++)
	{
		uint32_t ch = cells[i].chars[0];
		int is_sym = 0;
		if (ch == 0) ch = ' ';
		if (ch > 127)
		{
			uint32_t orig = ch;
			int sc = symbol_font_lookup(orig);
			if (sc >= 0) { ch = sc; is_sym = 1; }
			else if (ch <= 0xFFFF)
				ch = UNICODE_BMP_NORMALIZER[ch];
			else
				ch = MAC_ROMAN_LOZENGE;
		}
		s->scrollback[s->sb_head][i].ch = (unsigned char)ch;
		s->scrollback[s->sb_head][i].fg = VTERM_COLOR_IS_DEFAULT_FG(&cells[i].fg) ? 0 : vtcolor_to_byte(&cells[i].fg);
		s->scrollback[s->sb_head][i].bg = VTERM_COLOR_IS_DEFAULT_BG(&cells[i].bg) ? 0 : vtcolor_to_byte(&cells[i].bg);
		s->scrollback[s->sb_head][i].attrs =
			(cells[i].attrs.bold ? 1 : 0) |
			(cells[i].attrs.reverse ? 2 : 0) |
			(cells[i].attrs.underline ? 4 : 0) |
			(cells[i].attrs.italic ? 8 : 0) |
			(is_sym ? 16 : 0) |
			(VTERM_COLOR_IS_DEFAULT_FG(&cells[i].fg) ? 32 : 0) |
			(VTERM_COLOR_IS_DEFAULT_BG(&cells[i].bg) ? 64 : 0);
	}
	for (i = store_cols; i < SCROLLBACK_COLS; i++)
	{
		s->scrollback[s->sb_head][i].ch = ' ';
		s->scrollback[s->sb_head][i].fg = 0;
		s->scrollback[s->sb_head][i].bg = 0;
		s->scrollback[s->sb_head][i].attrs = 32 | 64; /* default fg + default bg */
	}

	s->sb_head = (s->sb_head + 1) % SCROLLBACK_LINES;
	if (s->sb_count < SCROLLBACK_LINES) s->sb_count++;

	/* if user is scrolled back, keep their position stable */
	if (s->scroll_offset > 0 && s->scroll_offset < s->sb_count)
		s->scroll_offset++;

	return 1;
}

static int sb_popline(int cols, VTermScreenCell *cells, void *user)
{
	int idx = (int)(intptr_t)user;
	struct session* s = &sessions[idx];
	int i;

	if (s->sb_count == 0) return 0;

	s->sb_count--;
	s->sb_head = (s->sb_head + SCROLLBACK_LINES - 1) % SCROLLBACK_LINES;

	int copy_cols = cols < SCROLLBACK_COLS ? cols : SCROLLBACK_COLS;

	/* convert compact sb_cell back to VTermScreenCell */
	for (i = 0; i < copy_cols; i++)
	{
		unsigned char sa = s->scrollback[s->sb_head][i].attrs;
		memset(&cells[i], 0, sizeof(VTermScreenCell));
		cells[i].chars[0] = s->scrollback[s->sb_head][i].ch;
		cells[i].width = 1;
		if (sa & 32)
			cells[i].fg.type = VTERM_COLOR_DEFAULT_FG;
		else
			vterm_color_indexed(&cells[i].fg, s->scrollback[s->sb_head][i].fg);
		if (sa & 64)
			cells[i].bg.type = VTERM_COLOR_DEFAULT_BG;
		else
			vterm_color_indexed(&cells[i].bg, s->scrollback[s->sb_head][i].bg);
		cells[i].attrs.bold = (sa & 1) ? 1 : 0;
		cells[i].attrs.reverse = (sa & 2) ? 1 : 0;
		cells[i].attrs.underline = (sa & 4) ? 1 : 0;
		cells[i].attrs.italic = (sa & 8) ? 1 : 0;
	}
	for (i = copy_cols; i < cols; i++)
	{
		memset(&cells[i], 0, sizeof(VTermScreenCell));
		cells[i].chars[0] = ' ';
		cells[i].width = 1;
	}

	if (s->scroll_offset > 0) s->scroll_offset--;

	return 1;
}

const VTermScreenCallbacks vtscrcb =
{
	.damage = damage,
	.moverect = NULL,
	.movecursor = movecursor,
	.settermprop = settermprop,
	.bell = bell,
	.resize = NULL,
	.sb_pushline = sb_pushline,
	.sb_popline = sb_popline
};

void scroll_up(struct window_context* wc)
{
	int sid = wc->session_ids[wc->active_session_idx];
	struct session* s = &sessions[sid];
	int page = wc->size_y / 2;

	if (s->scroll_offset + page > s->sb_count)
		s->scroll_offset = s->sb_count;
	else
		s->scroll_offset += page;

	SetPort(wc->win);
	InvalRect(&wc->win->portRect);
	wc->needs_redraw = 1;
}

void scroll_down(struct window_context* wc)
{
	int sid = wc->session_ids[wc->active_session_idx];
	struct session* s = &sessions[sid];
	int page = wc->size_y / 2;

	if (s->scroll_offset < page)
		s->scroll_offset = 0;
	else
		s->scroll_offset -= page;

	SetPort(wc->win);
	InvalRect(&wc->win->portRect);
	wc->needs_redraw = 1;
}

void scroll_up_line(struct window_context* wc)
{
	int sid = wc->session_ids[wc->active_session_idx];
	struct session* s = &sessions[sid];

	if (s->scroll_offset < s->sb_count)
		s->scroll_offset++;

	SetPort(wc->win);
	InvalRect(&wc->win->portRect);
	wc->needs_redraw = 1;
}

void scroll_down_line(struct window_context* wc)
{
	int sid = wc->session_ids[wc->active_session_idx];
	struct session* s = &sessions[sid];

	if (s->scroll_offset > 0)
		s->scroll_offset--;

	SetPort(wc->win);
	InvalRect(&wc->win->portRect);
	wc->needs_redraw = 1;
}

void scroll_reset(struct window_context* wc)
{
	int sid = wc->session_ids[wc->active_session_idx];
	sessions[sid].scroll_offset = 0;

	SetPort(wc->win);
	InvalRect(&wc->win->portRect);
	wc->needs_redraw = 1;
}

void font_size_change(struct window_context* wc)
{
	clear_selection(wc);

	short save_font = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	RGBColor save_fg, save_bg;
	GetForeColor(&save_fg);
	GetBackColor(&save_bg);

	RGBBackColor(&prefs.theme_bg);
	RGBForeColor(&prefs.theme_fg);

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);

	FontInfo fi = {0};
	GetFontInfo(&fi);

	con.cell_height = fi.ascent + fi.descent + fi.leading + 1;
	font_ascent = fi.ascent;

	con.cell_width = CharWidth(' ');

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);

	short top_extra = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	SizeWindow(wc->win, con.cell_width * wc->size_x + 4, con.cell_height * wc->size_y + 4 + top_extra, true);
	EraseRect(&(wc->win->portRect));
	InvalRect(&(wc->win->portRect));
	wc->needs_redraw = 1;

	RGBForeColor(&save_fg);
	RGBBackColor(&save_bg);
}

void reset_console(struct window_context* wc, int session_idx)
{
	struct session* s = &sessions[session_idx];

	if (session_idx == wc->session_ids[wc->active_session_idx])
	{
		RGBColor save_fg, save_bg;
		GetForeColor(&save_fg);
		GetBackColor(&save_bg);

		RGBForeColor(&prefs.theme_fg);
		RGBBackColor(&prefs.theme_bg);

		SetPort(wc->win);
		EraseRect(&wc->win->portRect);

		RGBForeColor(&save_fg);
		RGBBackColor(&save_bg);
	}

	s->cursor_x = 0;
	s->cursor_y = 0;

	if (s->type == SESSION_SSH)
		vterm_output_set_callback(s->vterm, output_callback, (void*)(intptr_t)session_idx);
	else if (s->type == SESSION_TELNET)
		vterm_output_set_callback(s->vterm, tcp_output_callback, (void*)(intptr_t)session_idx);
	else
		vterm_output_set_callback(s->vterm, local_output_callback, (void*)(intptr_t)session_idx);

	s->vts = vterm_obtain_screen(s->vterm);
	vterm_screen_set_callbacks(s->vts, &vtscrcb, (void*)(intptr_t)session_idx);
	vterm_screen_reset(s->vts, 1);

	/* set default colors and sync palette AFTER screen reset */
	{
		VTermState* vtermstate = vterm_obtain_state(s->vterm);
		VTermColor fg, bg;
		int ci;
		vterm_color_indexed(&fg, 7);
		vterm_color_indexed(&bg, 0);
		vterm_screen_set_default_colors(s->vts, &fg, &bg);

		for (ci = 0; ci < 16; ci++)
		{
			VTermColor pc;
			vterm_color_rgb(&pc,
				prefs.palette[ci].red >> 8,
				prefs.palette[ci].green >> 8,
				prefs.palette[ci].blue >> 8);
			vterm_state_set_palette_color(vtermstate, ci, &pc);
		}

		vterm_state_set_bold_highbright(vtermstate, prefs.bold_is_bright);
	}
}

void setup_session_vterm(struct window_context* wc, int session_idx)
{
	struct session* s = &sessions[session_idx];

	s->vterm = vterm_new(wc->size_y, wc->size_x);
	vterm_set_utf8(s->vterm, 1);

	if (s->type == SESSION_SSH)
		vterm_output_set_callback(s->vterm, output_callback, (void*)(intptr_t)session_idx);
	else if (s->type == SESSION_TELNET)
		vterm_output_set_callback(s->vterm, tcp_output_callback, (void*)(intptr_t)session_idx);
	else
		vterm_output_set_callback(s->vterm, local_output_callback, (void*)(intptr_t)session_idx);

	s->vts = vterm_obtain_screen(s->vterm);
	vterm_screen_set_callbacks(s->vts, &vtscrcb, (void*)(intptr_t)session_idx);
	vterm_screen_reset(s->vts, 1);

	/* set default colors and sync palette AFTER screen reset */
	{
		VTermState* vtermstate = vterm_obtain_state(s->vterm);
		VTermColor fg, bg;
		int ci;
		vterm_color_indexed(&fg, 7);
		vterm_color_indexed(&bg, 0);
		vterm_screen_set_default_colors(s->vts, &fg, &bg);

		for (ci = 0; ci < 16; ci++)
		{
			VTermColor pc;
			vterm_color_rgb(&pc,
				prefs.palette[ci].red >> 8,
				prefs.palette[ci].green >> 8,
				prefs.palette[ci].blue >> 8);
			vterm_state_set_palette_color(vtermstate, ci, &pc);
		}

		/* bold + color 0-7 promotes to bright 8-15 (standard BBS behavior) */
		vterm_state_set_bold_highbright(vtermstate, prefs.bold_is_bright);
	}
}

void init_font_metrics(void)
{
	short save_font = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	RGBColor save_fg, save_bg;
	GetForeColor(&save_fg);
	GetBackColor(&save_bg);

	RGBBackColor(&prefs.theme_bg);
	RGBForeColor(&prefs.theme_fg);

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);

	FontInfo fi = {0};
	GetFontInfo(&fi);

	con.cell_height = fi.ascent + fi.descent + fi.leading + 1;
	font_ascent = fi.ascent;
	con.cell_width = CharWidth(' ');

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);

	RGBForeColor(&save_fg);
	RGBBackColor(&save_bg);

	setup_key_translation();
}

void update_console_colors(struct window_context* wc)
{
	VTermState* vtermstate = vterm_obtain_state(WC_S(wc).vterm);

	{
		VTermColor fg, bg;
		int ci;
		vterm_color_indexed(&fg, 7);
		vterm_color_indexed(&bg, 0);
		vterm_screen_set_default_colors(WC_S(wc).vts, &fg, &bg);

		/* sync palette */
		for (ci = 0; ci < 16; ci++)
		{
			VTermColor pc;
			vterm_color_rgb(&pc,
				prefs.palette[ci].red >> 8,
				prefs.palette[ci].green >> 8,
				prefs.palette[ci].blue >> 8);
			vterm_state_set_palette_color(vtermstate, ci, &pc);
		}

		vterm_state_set_bold_highbright(vtermstate, prefs.bold_is_bright);
	}

	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));
	wc->needs_redraw = 1;
}
