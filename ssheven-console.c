/*
 * ssheven
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#include "ssheven.h"
#include "ssheven-console.h"
#include "ssheven-net.h"
#include "ssheven-unicode.h"

#include <string.h>

#include <Sound.h>

#include <vterm.h>

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* active session for a given window context */
#define WC_S(wc) sessions[(wc)->session_ids[(wc)->active_session_idx]]


char key_to_vterm[256] = { VTERM_KEY_NONE };

int font_offset = 0;

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
	MoveTo(r->left + 2 + x * con.cell_width, r->top + 2 - font_offset + ((y+1) * con.cell_height) + top_offset);
	DrawChar(c);
}

void toggle_cursor(struct window_context* wc)
{
	WC_S(wc).cursor_state = !WC_S(wc).cursor_state;
	Rect cursor = cell_rect(wc, WC_S(wc).cursor_x, WC_S(wc).cursor_y, wc->win->portRect);
	InvalRect(&cursor);
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

// convert Quickdraw colors into vterm's ANSI color indexes
int qd2idx(int qdc)
{
	switch (qdc)
	{
		case blackColor: return 0;
		case redColor: return 1;
		case greenColor: return 2;
		case yellowColor: return 3;
		case blueColor: return 4;
		case magentaColor: return 5;
		case cyanColor: return 6;
		case whiteColor: return 7;
		default: return 0;
	}
}

// convert vterm's ANSI color indexes into Quickdraw colors
int idx2qd[16] = { blackColor, redColor, greenColor, yellowColor, blueColor, magentaColor, cyanColor, whiteColor, blackColor, redColor, greenColor, yellowColor, blueColor, magentaColor, cyanColor, whiteColor };

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

	char* output = malloc(sizeof(char) * len);

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

	output[len-1] = '\0';

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
	short save_font_fg   = qd.thePort->fgColor;
	short save_font_bg   = qd.thePort->bkColor;

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
			// active tab: lighter, raised
			BackColor(whiteColor);
			ForeColor(blackColor);
			EraseRect(&tab_rect);

			// draw bottom line only on inactive tabs
			MoveTo(tab_rect.left, tab_rect.top);
			LineTo(tab_rect.left, tab_rect.bottom - 1);
			LineTo(tab_rect.right - 1, tab_rect.bottom - 1);
			LineTo(tab_rect.right - 1, tab_rect.top);
		}
		else
		{
			// inactive tab: darker, recessed
			BackColor(whiteColor);
			ForeColor(blackColor);
			EraseRect(&tab_rect);

			// darken with a gray pattern
			PenPat(&qd.gray);
			PenMode(patOr);
			PaintRect(&tab_rect);
			PenPat(&qd.black);
			PenMode(patCopy);

			// border
			ForeColor(blackColor);
			FrameRect(&tab_rect);
		}

		// draw tab label
		ForeColor(blackColor);
		char* label = sessions[sid].tab_label;
		int label_len = strlen(label);

		// truncate label to fit (leave room for close button)
		int max_chars = (tab_width - 20) / CharWidth('M');
		if (label_len > max_chars) label_len = max_chars;

		MoveTo(tab_rect.left + 4, tab_rect.top + 14);
		DrawText(label, 0, label_len);

		// draw close button (X) if more than one tab
		if (wc->num_sessions > 1)
		{
			short cx1 = tab_rect.right - 15;
			short cy1 = tab_rect.top + 4;
			short cx2 = cx1 + 11;
			short cy2 = cy1 + 11;

			ForeColor(blackColor);
			MoveTo(cx1, cy1);
			LineTo(cx2, cy2);
			MoveTo(cx2, cy1);
			LineTo(cx1, cy2);
		}
	}

	// draw separator line between tab bar and terminal area
	ForeColor(blackColor);
	MoveTo(portRect.left, portRect.top + TAB_BAR_HEIGHT);
	LineTo(portRect.right, portRect.top + TAB_BAR_HEIGHT);

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);
	qd.thePort->fgColor = save_font_fg;
	qd.thePort->bkColor = save_font_bg;
}

void draw_screen_color(struct window_context* wc, Rect* r)
{
	// don't clobber font settings
	short save_font      = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	short save_font_fg   = qd.thePort->fgColor;
	short save_font_bg   = qd.thePort->bkColor;

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);
	BackColor(prefs.bg_color);
	ForeColor(prefs.fg_color);
	TextMode(srcOr); // or mode is faster for some common cases

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
	Rect run_rect;
	short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	int vertical_offset = r->top + con.cell_height - font_offset + 2 + top_offset;
	int run_start_col, run_length;
	short run_face, next_face;
	int run_inverted;
	int run_fg, run_bg;
	int ok = 0;

	for (pos.row = 0; pos.row < wc->size_y; pos.row++)
	{
		pos.col = 0;
		ok = vterm_screen_get_cell(WC_S(wc).vts, pos, &run_start);
		run_length = 0;
		run_start_col = 0;

		run_rect = cell_rect(wc, 0, pos.row, wc->win->portRect);

		run_inverted = run_start.attrs.reverse ^ (i < select_end && i >= select_start);

		run_fg = run_start.fg.indexed.idx;
		run_bg = run_start.bg.indexed.idx;

		run_face = normal;
		if (run_start.attrs.bold) run_face |= (condense|bold);
		if (run_start.attrs.italic) run_face |= (condense|italic);
		if (run_start.attrs.underline) run_face |= underline;

		for (pos.col = 0; pos.col < wc->size_x; pos.col++)
		{
			ok = vterm_screen_get_cell(WC_S(wc).vts, pos, &vtsc);

			uint32_t glyph = vtsc.chars[0];

			if (glyph == 0) glyph = ' ';

			// normalize some unicode
			if (glyph > 127)
			{
				if (glyph <= 0xFFFF)
				{
					glyph = UNICODE_BMP_NORMALIZER[glyph];
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

			// if we cannot add this cell to the run
			if (vtsc.fg.indexed.idx != run_fg || vtsc.bg.indexed.idx != run_bg || next_face != run_face || (vtsc.attrs.reverse ^ (i < select_end && i >= select_start)) != run_inverted)
			{
				// draw what we've got so far
				if (run_inverted)
				{
					BackColor(idx2qd[run_fg]);
					ForeColor(idx2qd[run_bg]);
				}
				else
				{
					BackColor(idx2qd[run_bg]);
					ForeColor(idx2qd[run_fg]);
				}

				EraseRect(&run_rect);
				MoveTo(run_rect.left, vertical_offset);
				TextFace(run_face);
				DrawText(row_text, run_start_col, run_length);

				// then reset everything to start a new run
				run_inverted = vtsc.attrs.reverse ^ (i < select_end && i >= select_start);
				run_fg = vtsc.fg.indexed.idx;
				run_bg = vtsc.bg.indexed.idx;
				run_face = next_face;
				run_start_col = pos.col;
				run_rect = cell_rect(wc, pos.col, pos.row, wc->win->portRect);
				run_length = 1;
			}
			else
			{
				run_length++;
			}

			// if we're at the last cell in the row, draw the run
			if (pos.col == wc->size_x - 1)
			{
				if (run_inverted)
				{
					BackColor(idx2qd[run_fg]);
					ForeColor(idx2qd[run_bg]);
				}
				else
				{
					BackColor(idx2qd[run_bg]);
					ForeColor(idx2qd[run_fg]);
				}

				EraseRect(&run_rect);
				MoveTo(run_rect.left, vertical_offset);
				TextFace(run_face);
				DrawText(row_text, run_start_col, run_length);
			}
			else
			{
				run_rect.right += con.cell_width;
			}

			i++;
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
	qd.thePort->fgColor = save_font_fg;
	qd.thePort->bkColor = save_font_bg;

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
	short save_font_fg   = qd.thePort->fgColor;
	short save_font_bg   = qd.thePort->bkColor;

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);
	qd.thePort->bkColor = prefs.bg_color;
	qd.thePort->fgColor = prefs.fg_color;
	TextMode(srcOr); // or mode is faster for drawing black on white

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
	Rect cr;

	VTermScreenCell here = {0};

	short top_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	int vertical_offset = r->top + con.cell_height - font_offset + 2 + top_offset;

	for (pos.row = 0; pos.row < wc->size_y; pos.row++)
	{
		erase_row(wc, pos.row);

		for (pos.col = 0; pos.col < wc->size_x; pos.col++)
		{
			int ret = vterm_screen_get_cell(WC_S(wc).vts, pos, &here);

			uint32_t glyph = here.chars[0];

			if (glyph == 0) glyph = ' ';

			// normalize some unicode
			if (glyph > 127)
			{
				if (glyph <= 0xFFFF)
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
		DrawText(row_text, 0, wc->size_x);

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
	qd.thePort->fgColor = save_font_fg;
	qd.thePort->bkColor = save_font_bg;

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
			str++;
			switch (*str)
			{
				case 'd':
					print_int_v(vt, va_arg(args, int));
					break;
				case 's':
					print_string_v(vt, va_arg(args, char*));
					break;
				case '\0':
					vterm_input_write(vt, str-1, 1);
					break;
				default:
					va_arg(args, int); // ignore
					vterm_input_write(vt, str-1, 2);
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

	// if active and cursor is dark, invalidate old location
	if (wc != NULL && idx == wc->session_ids[wc->active_session_idx]
		&& wc->win != NULL && s->cursor_state == 1)
	{
		SetPort(wc->win);
		Rect inval = cell_rect(wc, s->cursor_x, s->cursor_y, (wc->win->portRect));
		InvalRect(&inval);
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

	// always invalidate the full terminal area
	// partial invalidation causes display corruption during scrolling
	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));

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
			if (wc != NULL && idx == wc->session_ids[wc->active_session_idx])
				set_window_title(wc->win, val->string.str, val->string.len);
			// also update tab label
			{
				int len = val->string.len < 63 ? val->string.len : 63;
				strncpy(s->tab_label, val->string.str, len);
				s->tab_label[len] = '\0';
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

const VTermScreenCallbacks vtscrcb =
{
	.damage = damage,
	.moverect = NULL,
	.movecursor = movecursor,
	.settermprop = settermprop,
	.bell = bell,
	.resize = NULL,
	.sb_pushline = NULL,
	.sb_popline = NULL
};

void font_size_change(struct window_context* wc)
{
	clear_selection(wc);

	// don't clobber font settings
	short save_font = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	short save_font_fg   = qd.thePort->fgColor;
	short save_font_bg   = qd.thePort->bkColor;

	BackColor(prefs.bg_color);
	ForeColor(prefs.fg_color);

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);

	FontInfo fi = {0};
	GetFontInfo(&fi);

	con.cell_height = fi.ascent + fi.descent + fi.leading + 1;
	font_offset = fi.descent;

	con.cell_width = CharWidth(' ');

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);

	short top_extra = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
	SizeWindow(wc->win, con.cell_width * wc->size_x + 4, con.cell_height * wc->size_y + 4 + top_extra, true);
	EraseRect(&(wc->win->portRect));
	InvalRect(&(wc->win->portRect));

	BackColor(save_font_bg);
	ForeColor(save_font_fg);
}

void reset_console(struct window_context* wc, int session_idx)
{
	struct session* s = &sessions[session_idx];

	if (session_idx == wc->session_ids[wc->active_session_idx])
	{
		short save_fg = qd.thePort->fgColor;
		short save_bg = qd.thePort->bkColor;

		ForeColor(prefs.fg_color);
		BackColor(prefs.bg_color);

		SetPort(wc->win);
		EraseRect(&wc->win->portRect);

		ForeColor(save_fg);
		BackColor(save_bg);
	}

	s->cursor_x = 0;
	s->cursor_y = 0;

	VTermState* vtermstate = vterm_obtain_state(s->vterm);
	vterm_state_reset(vtermstate, 1);

	VTermColor fg = { .type = VTERM_COLOR_INDEXED };
	fg.indexed.idx = qd2idx(prefs.fg_color);

	VTermColor bg  = { .type = VTERM_COLOR_INDEXED };
	bg.indexed.idx = qd2idx(prefs.bg_color);

	vterm_state_set_default_colors(vtermstate, &fg, &bg);

	if (s->type == SESSION_SSH)
		vterm_output_set_callback(s->vterm, output_callback, (void*)(intptr_t)session_idx);
	else
		vterm_output_set_callback(s->vterm, local_output_callback, (void*)(intptr_t)session_idx);

	s->vts = vterm_obtain_screen(s->vterm);
	vterm_screen_reset(s->vts, 1);
	vterm_screen_set_callbacks(s->vts, &vtscrcb, (void*)(intptr_t)session_idx);
}

void setup_session_vterm(struct window_context* wc, int session_idx)
{
	struct session* s = &sessions[session_idx];

	s->vterm = vterm_new(wc->size_y, wc->size_x);
	vterm_set_utf8(s->vterm, 1);
	VTermState* vtermstate = vterm_obtain_state(s->vterm);
	vterm_state_reset(vtermstate, 1);

	VTermColor fg = { .type = VTERM_COLOR_INDEXED };
	fg.indexed.idx = qd2idx(prefs.fg_color);

	VTermColor bg  = { .type = VTERM_COLOR_INDEXED };
	bg.indexed.idx = qd2idx(prefs.bg_color);

	vterm_state_set_default_colors(vtermstate, &fg, &bg);

	if (s->type == SESSION_SSH)
		vterm_output_set_callback(s->vterm, output_callback, (void*)(intptr_t)session_idx);
	else
		vterm_output_set_callback(s->vterm, local_output_callback, (void*)(intptr_t)session_idx);

	s->vts = vterm_obtain_screen(s->vterm);
	vterm_screen_reset(s->vts, 1);
	vterm_screen_set_callbacks(s->vts, &vtscrcb, (void*)(intptr_t)session_idx);
}

void init_font_metrics(void)
{
	// don't clobber font settings
	short save_font = qd.thePort->txFont;
	short save_font_size = qd.thePort->txSize;
	short save_font_face = qd.thePort->txFace;
	short save_font_fg   = qd.thePort->fgColor;
	short save_font_bg   = qd.thePort->bkColor;

	BackColor(prefs.bg_color);
	ForeColor(prefs.fg_color);

	TextFont(kFontIDMonaco);
	TextSize(prefs.font_size);
	TextFace(normal);

	FontInfo fi = {0};
	GetFontInfo(&fi);

	con.cell_height = fi.ascent + fi.descent + fi.leading + 1;
	font_offset = fi.descent;
	con.cell_width = CharWidth(' ');

	TextFont(save_font);
	TextSize(save_font_size);
	TextFace(save_font_face);

	BackColor(save_font_bg);
	ForeColor(save_font_fg);

	setup_key_translation();
}

void update_console_colors(struct window_context* wc)
{
	VTermState* vtermstate = vterm_obtain_state(WC_S(wc).vterm);

	VTermColor fg = { .type = VTERM_COLOR_INDEXED };
	fg.indexed.idx = qd2idx(prefs.fg_color);

	VTermColor bg  = { .type = VTERM_COLOR_INDEXED };
	bg.indexed.idx = qd2idx(prefs.bg_color);

	vterm_state_set_default_colors(vtermstate, &fg, &bg);

	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));
}
