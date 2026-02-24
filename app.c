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
#include "shell.h"
#include "debug.h"

#include <Threads.h>
#include <MacMemory.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Sound.h>
#include <Gestalt.h>
#include <Devices.h>
#include <Scrap.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Resources.h>

#include <stdio.h>

// forward declarations
int qd_color_to_menu_item(int qd_color);
int font_size_to_menu_item(int font_size);

// sinful globals
struct console_metrics con = { 0, 0 };
struct session sessions[MAX_SESSIONS] = {0};
struct window_context windows[MAX_WINDOWS] = {0};
int num_windows = 0;
int active_window = 0;
int exit_requested = 0;
struct preferences prefs;

const uint8_t ascii_to_control_code[256] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 27, 28, 29, 30, 31, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};

// maps virtual keycodes to (hopefully) ascii characters
// this will probably be weird for non-letter keypresses
uint8_t keycode_to_ascii[256] = {0};

void generate_key_mapping(void)
{
	// TODO this sucks
	// gets the currently loaded keymap resource
	// passes in the virtual keycode but without any previous state or modifiers
	// ignores the second byte if we're currently using a multibyte lang

	void* kchr = (void*)GetScriptManagerVariable(smKCHRCache);

	for (uint16_t i = 0; i < 256; i++)
	{
		keycode_to_ascii[i] = KeyTranslate(kchr, i, 0) & 0xff;
	}
}

/* ---- window helper functions ---- */

struct window_context* find_window_context(WindowPtr w)
{
	int i;
	for (i = 0; i < MAX_WINDOWS; i++)
	{
		if (windows[i].in_use && windows[i].win == w)
			return &windows[i];
	}
	return NULL;
}

struct window_context* window_for_session(int session_idx)
{
	if (session_idx < 0 || session_idx >= MAX_SESSIONS) return NULL;
	if (!sessions[session_idx].in_use) return NULL;
	int wid = sessions[session_idx].window_id;
	if (wid < 0 || wid >= MAX_WINDOWS) return NULL;
	if (!windows[wid].in_use) return NULL;
	return &windows[wid];
}

int active_session_global(void)
{
	return ACTIVE_WIN.session_ids[ACTIVE_WIN.active_session_idx];
}

void add_session_to_window(struct window_context* wc, int session_idx)
{
	if (wc->num_sessions >= MAX_SESSIONS) return;
	wc->session_ids[wc->num_sessions] = session_idx;
	sessions[session_idx].window_id = (int)(wc - windows);
	wc->num_sessions++;
}

void remove_session_from_window(struct window_context* wc, int session_idx)
{
	int i, found = -1;
	for (i = 0; i < wc->num_sessions; i++)
	{
		if (wc->session_ids[i] == session_idx)
		{
			found = i;
			break;
		}
	}
	if (found < 0) return;

	// shift remaining entries left
	for (i = found; i < wc->num_sessions - 1; i++)
		wc->session_ids[i] = wc->session_ids[i + 1];
	wc->num_sessions--;

	// fix active_session_idx
	if (wc->num_sessions == 0)
	{
		wc->active_session_idx = 0;
	}
	else
	{
		if (wc->active_session_idx >= wc->num_sessions)
			wc->active_session_idx = wc->num_sessions - 1;
	}
}

/* ---- end window helpers ---- */

void set_window_title(WindowPtr w, const char* c_name, size_t length)
{
	Str255 pascal_name;
	strncpy((char *) &pascal_name[1], c_name, 254);
	pascal_name[0] = length < 254 ? length : 254;

	SetWTitle(w, pascal_name);
}

void set_terminal_string(void)
{
	/*
	 * terminal type to send over ssh, determines features etc. some good options:
	 * "vanilla" supports basically nothing
	 * "vt100" just the basics
	 * "xterm" everything
	 * "xterm-mono" everything except color
	 * "xterm-256color" full 256-color + true-color RGB support
	 */
	switch (prefs.display_mode)
	{
		case FASTEST:
			prefs.terminal_string = "xterm-mono";
			break;
		case COLOR:
			prefs.terminal_string = "xterm-256color";
			break;
		default:
			prefs.terminal_string = DEFAULT_TERM_STRING;
			break;
	}
}

/* ---- Resource-based preferences ---- */

/* disk layout for 'PREF' resource — bump DISK_PREFS_VERSION if you change this struct */
#define DISK_PREFS_VERSION 1

struct disk_prefs
{
	short version;
	short auth_type;
	short display_mode;
	short fg_color;
	short bg_color;
	short font_size;
	short theme_loaded;
	short prompt_color;
	RGBColor palette[16];
	RGBColor theme_fg;
	RGBColor theme_bg;
	RGBColor theme_cursor;
	RGBColor orig_theme_fg;
	RGBColor orig_theme_bg;
	char theme_name[64];
	char hostname[512];     /* pascal string: byte 0 = length */
	char username[256];     /* pascal string */
	char port[256];         /* pascal string */
	char privkey_path[1024];
	char pubkey_path[1024];
};

static OSErr get_prefs_spec(FSSpec* spec)
{
	short vRefNum;
	long dirID;
	OSErr e = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vRefNum, &dirID);
	if (e != noErr) return e;
	return FSMakeFSSpec(vRefNum, dirID, PREFERENCES_FILENAME, spec);
}

int save_prefs(void)
{
	FSSpec spec;
	OSErr e;
	short refNum;
	Handle h;
	struct disk_prefs* dp;

	e = get_prefs_spec(&spec);
	if (e != noErr && e != fnfErr) return 0;

	if (e == fnfErr)
	{
		/* no file yet — create new resource file */
		FSpCreateResFile(&spec, 'SSH7', 'SH7p', smSystemScript);
		if (ResError() != noErr) return 0;
	}
	else
	{
		/* file exists — try opening as resource file */
		refNum = FSpOpenResFile(&spec, fsRdWrPerm);
		if (refNum == -1)
		{
			/* old text-based prefs file, replace with resource file */
			FSpDelete(&spec);
			FSpCreateResFile(&spec, 'SSH7', 'SH7p', smSystemScript);
			if (ResError() != noErr) return 0;
		}
		else
		{
			/* already a valid resource file, use it */
			UseResFile(refNum);
			goto have_file;
		}
	}

	refNum = FSpOpenResFile(&spec, fsRdWrPerm);
	if (refNum == -1) return 0;
	UseResFile(refNum);

have_file:
	/* remove old resource if it exists */
	h = Get1Resource('PREF', 128);
	if (h)
	{
		RemoveResource(h);
		DisposeHandle(h);
	}

	/* build disk_prefs */
	h = NewHandleClear(sizeof(struct disk_prefs));
	if (h == NULL) { CloseResFile(refNum); return 0; }

	HLock(h);
	dp = (struct disk_prefs*)*h;

	dp->version = DISK_PREFS_VERSION;
	dp->auth_type = (short)prefs.auth_type;
	dp->display_mode = (short)prefs.display_mode;
	dp->fg_color = (short)prefs.fg_color;
	dp->bg_color = (short)prefs.bg_color;
	dp->font_size = (short)prefs.font_size;
	dp->theme_loaded = (short)prefs.theme_loaded;
	dp->prompt_color = (short)prefs.prompt_color;

	memcpy(dp->palette, prefs.palette, sizeof(prefs.palette));
	dp->theme_fg = prefs.theme_fg;
	dp->theme_bg = prefs.theme_bg;
	dp->theme_cursor = prefs.theme_cursor;
	dp->orig_theme_fg = prefs.orig_theme_fg;
	dp->orig_theme_bg = prefs.orig_theme_bg;

	strncpy(dp->theme_name, prefs.theme_name, 63);
	dp->theme_name[63] = '\0';

	memcpy(dp->hostname, prefs.hostname, 512);
	memcpy(dp->username, prefs.username, 256);
	memcpy(dp->port, prefs.port, 256);

	if (prefs.privkey_path)
		strncpy(dp->privkey_path, prefs.privkey_path, 1023);
	dp->privkey_path[1023] = '\0';

	if (prefs.pubkey_path)
		strncpy(dp->pubkey_path, prefs.pubkey_path, 1023);
	dp->pubkey_path[1023] = '\0';

	HUnlock(h);

	AddResource(h, 'PREF', 128, "\pPreferences");
	if (ResError() != noErr)
	{
		DisposeHandle(h);
		CloseResFile(refNum);
		return 0;
	}

	WriteResource(h);
	CloseResFile(refNum);
	return 1;
}

// check if the main device is black and white
int detect_color_screen(void)
{
	return TestDeviceAttribute(GetMainDevice(), gdDevType);
}

static void make_rgb(RGBColor* c, unsigned short r, unsigned short g, unsigned short b)
{
	c->red   = (r << 8) | r;
	c->green = (g << 8) | g;
	c->blue  = (b << 8) | b;
}

static void init_tango_palette(void)
{
	/* Tango Desktop Project ANSI colors */
	make_rgb(&prefs.palette[0],  0x00, 0x00, 0x00); /* black */
	make_rgb(&prefs.palette[1],  0xCC, 0x00, 0x00); /* red */
	make_rgb(&prefs.palette[2],  0x4E, 0x9A, 0x06); /* green */
	make_rgb(&prefs.palette[3],  0xC4, 0xA0, 0x00); /* yellow */
	make_rgb(&prefs.palette[4],  0x34, 0x65, 0xA4); /* blue */
	make_rgb(&prefs.palette[5],  0x75, 0x50, 0x7B); /* magenta */
	make_rgb(&prefs.palette[6],  0x06, 0x98, 0x9A); /* cyan */
	make_rgb(&prefs.palette[7],  0xD3, 0xD7, 0xCF); /* white */
	/* bright variants */
	make_rgb(&prefs.palette[8],  0x55, 0x57, 0x53); /* bright black */
	make_rgb(&prefs.palette[9],  0xEF, 0x29, 0x29); /* bright red */
	make_rgb(&prefs.palette[10], 0x8A, 0xE2, 0x34); /* bright green */
	make_rgb(&prefs.palette[11], 0xFC, 0xE9, 0x4F); /* bright yellow */
	make_rgb(&prefs.palette[12], 0x73, 0x9F, 0xCF); /* bright blue */
	make_rgb(&prefs.palette[13], 0xAD, 0x7F, 0xA8); /* bright magenta */
	make_rgb(&prefs.palette[14], 0x34, 0xE2, 0xE2); /* bright cyan */
	make_rgb(&prefs.palette[15], 0xEE, 0xEE, 0xEC); /* bright white */
}

static void save_theme_originals(void)
{
	prefs.orig_theme_bg = prefs.theme_bg;
	prefs.orig_theme_fg = prefs.theme_fg;
}

static void init_dark_palette(void)
{
	init_tango_palette();
	make_rgb(&prefs.theme_bg, 0x00, 0x00, 0x00);
	make_rgb(&prefs.theme_fg, 0xEE, 0xEE, 0xEC);
	make_rgb(&prefs.theme_cursor, 0xEE, 0xEE, 0xEC);
	save_theme_originals();
	prefs.theme_loaded = 1;
	strcpy(prefs.theme_name, "Default Dark");
}

static void init_light_palette(void)
{
	init_tango_palette();
	make_rgb(&prefs.theme_bg, 0xFF, 0xFF, 0xFF);
	make_rgb(&prefs.theme_fg, 0x00, 0x00, 0x00);
	make_rgb(&prefs.theme_cursor, 0x00, 0x00, 0x00);
	save_theme_originals();
	prefs.theme_loaded = 1;
	strcpy(prefs.theme_name, "Default Light");
}

void init_prefs(void)
{
	/* initialize everything to a safe default */
	prefs.major_version = APP_VERSION_MAJOR;
	prefs.minor_version = APP_VERSION_MINOR;

	memset(&(prefs.hostname), 0, 512);
	memset(&(prefs.username), 0, 256);
	memset(&(prefs.password), 0, 256);
	memset(&(prefs.port), 0, 256);

	/* default port: 22 */
	prefs.port[0] = 2;
	prefs.port[1] = '2';
	prefs.port[2] = '2';

	prefs.pubkey_path = "";
	prefs.privkey_path = "";
	prefs.terminal_string = DEFAULT_TERM_STRING;
	prefs.auth_type = USE_PASSWORD;
	prefs.display_mode = detect_color_screen() ? COLOR : FASTEST;
	prefs.fg_color = COLOR_FROM_THEME;
	prefs.bg_color = COLOR_FROM_THEME;
	prefs.font_size = 9;
	prefs.prompt_color = 4; /* blue */

	init_dark_palette();

	prefs.loaded_from_file = 0;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return 0;
}

static int parse_hex_rgb(const char* s, RGBColor* c)
{
	int i;
	unsigned char r, g, b;
	/* must be exactly 6 hex chars */
	for (i = 0; i < 6; i++)
	{
		char ch = s[i];
		if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')))
			return 0;
	}
	r = (hex_digit(s[0]) << 4) | hex_digit(s[1]);
	g = (hex_digit(s[2]) << 4) | hex_digit(s[3]);
	b = (hex_digit(s[4]) << 4) | hex_digit(s[5]);
	make_rgb(c, r, g, b);
	return 1;
}

/* read one line from buffer, returns pointer past the newline or NULL.
   skips exactly one line ending (\n, \r, or \r\n) so empty lines are preserved. */
static const char* read_line(const char* buf, const char* end, char* out, int max)
{
	int i = 0;
	while (buf < end && *buf != '\n' && *buf != '\r' && i < max - 1)
	{
		out[i++] = *buf++;
	}
	out[i] = '\0';
	/* skip exactly one line ending */
	if (buf < end && *buf == '\r') buf++;
	if (buf < end && *buf == '\n') buf++;
	return buf < end ? buf : NULL;
}

int load_theme_file(FSSpec* spec)
{
	short refNum = 0;
	OSErr e;
	long buf_size = 2048;
	char* buf = NULL;
	char line[128];
	const char* p;
	const char* end;
	RGBColor new_bg, new_fg, new_cursor;
	RGBColor new_palette[16];
	int i;

	e = FSpOpenDF(spec, fsCurPerm, &refNum);
	if (e != noErr) return 0;

	buf = malloc(buf_size);
	if (buf == NULL) { FSClose(refNum); return 0; }

	e = FSRead(refNum, &buf_size, buf);
	FSClose(refNum);

	if (e != noErr && e != eofErr) { free(buf); return 0; }

	end = buf + buf_size;
	p = buf;

	/* line 1: magic */
	p = read_line(p, end, line, sizeof(line));
	if (p == NULL && line[0] == '\0') { free(buf); return 0; }
	if (strncmp(line, "STTY1", 5) != 0) { free(buf); return 0; }

	/* line 2: background */
	p = read_line(p ? p : end, end, line, sizeof(line));
	if (!parse_hex_rgb(line, &new_bg)) { free(buf); return 0; }

	/* line 3: foreground */
	p = read_line(p ? p : end, end, line, sizeof(line));
	if (!parse_hex_rgb(line, &new_fg)) { free(buf); return 0; }

	/* line 4: cursor */
	p = read_line(p ? p : end, end, line, sizeof(line));
	if (!parse_hex_rgb(line, &new_cursor)) { free(buf); return 0; }

	/* lines 5-20: ANSI 0-15 */
	for (i = 0; i < 16; i++)
	{
		p = read_line(p ? p : end, end, line, sizeof(line));
		if (!parse_hex_rgb(line, &new_palette[i])) { free(buf); return 0; }
	}

	/* all parsed OK, commit */
	prefs.theme_bg = new_bg;
	prefs.theme_fg = new_fg;
	prefs.theme_cursor = new_cursor;
	for (i = 0; i < 16; i++)
		prefs.palette[i] = new_palette[i];
	save_theme_originals();
	prefs.theme_loaded = 1;

	/* extract theme name from FSSpec */
	{
		int len = spec->name[0];
		if (len > 63) len = 63;
		memcpy(prefs.theme_name, spec->name + 1, len);
		prefs.theme_name[len] = '\0';
	}

	free(buf);
	return 1;
}

void load_prefs(void)
{
	FSSpec spec;
	OSErr e;
	short refNum;
	Handle h;
	struct disk_prefs* dp;

	e = get_prefs_spec(&spec);
	if (e != noErr) return;

	refNum = FSpOpenResFile(&spec, fsCurPerm);
	if (refNum == -1) return;

	UseResFile(refNum);
	h = Get1Resource('PREF', 128);
	if (h == NULL)
	{
		CloseResFile(refNum);
		return;
	}

	if (GetHandleSize(h) < (long)sizeof(struct disk_prefs))
	{
		ReleaseResource(h);
		CloseResFile(refNum);
		return;
	}

	HLock(h);
	dp = (struct disk_prefs*)*h;

	if (dp->version != DISK_PREFS_VERSION)
	{
		HUnlock(h);
		ReleaseResource(h);
		CloseResFile(refNum);
		return;
	}

	prefs.loaded_from_file = 1;
	prefs.auth_type = dp->auth_type;
	prefs.display_mode = dp->display_mode;
	prefs.fg_color = dp->fg_color;
	prefs.bg_color = dp->bg_color;
	prefs.font_size = dp->font_size;
	prefs.theme_loaded = dp->theme_loaded;
	prefs.prompt_color = dp->prompt_color;
	if (prefs.prompt_color < 1 || prefs.prompt_color > 15)
		prefs.prompt_color = 4; /* default blue; 0 = unset from old prefs */

	memcpy(prefs.palette, dp->palette, sizeof(prefs.palette));
	prefs.theme_fg = dp->theme_fg;
	prefs.theme_bg = dp->theme_bg;
	prefs.theme_cursor = dp->theme_cursor;
	prefs.orig_theme_fg = dp->orig_theme_fg;
	prefs.orig_theme_bg = dp->orig_theme_bg;

	strncpy(prefs.theme_name, dp->theme_name, 63);
	prefs.theme_name[63] = '\0';

	memcpy(prefs.hostname, dp->hostname, 512);
	memcpy(prefs.username, dp->username, 256);
	memcpy(prefs.port, dp->port, 256);

	/* alloc and copy key paths */
	prefs.privkey_path = malloc(1024);
	prefs.pubkey_path = malloc(1024);
	if (prefs.privkey_path)
	{
		strncpy(prefs.privkey_path, dp->privkey_path, 1023);
		prefs.privkey_path[1023] = '\0';
	}
	if (prefs.pubkey_path)
	{
		strncpy(prefs.pubkey_path, dp->pubkey_path, 1023);
		prefs.pubkey_path[1023] = '\0';
	}

	HUnlock(h);
	ReleaseResource(h);
	CloseResFile(refNum);

	/* bounds-check */
	if (prefs.auth_type != USE_KEY && prefs.auth_type != USE_PASSWORD)
		prefs.auth_type = USE_PASSWORD;
	if (prefs.display_mode != FASTEST && prefs.display_mode != COLOR)
		prefs.display_mode = detect_color_screen() ? COLOR : FASTEST;
	if (qd_color_to_menu_item(prefs.fg_color) == 1 && prefs.fg_color != COLOR_FROM_THEME)
		prefs.fg_color = COLOR_FROM_THEME;
	if (qd_color_to_menu_item(prefs.bg_color) == 1 && prefs.bg_color != COLOR_FROM_THEME)
		prefs.bg_color = COLOR_FROM_THEME;
	if (font_size_to_menu_item(prefs.font_size) == 1 && prefs.font_size != 9)
		prefs.font_size = 9;

	if (prefs.theme_loaded)
		save_theme_originals();

	set_terminal_string();
}

// borrowed from Retro68 sample code
// draws the "default" indicator around a button
pascal void ButtonFrameProc(DialogRef dlg, DialogItemIndex itemNo)
{
	DialogItemType type;
	Handle itemH;
	Rect box;

	GetDialogItem(dlg, 1, &type, &itemH, &box);
	InsetRect(&box, -4, -4);
	PenSize(3, 3);
	FrameRoundRect(&box, 16, 16);
}

void display_about_box(void)
{
	DialogRef about = GetNewDialog(DLOG_ABOUT, 0, (WindowPtr)-1);

	UpdateDialog(about, about->visRgn);

	while (!Button()) YieldToAnyThread();
	while (Button()) YieldToAnyThread();

	FlushEvents(everyEvent, 0);
	DisposeWindow(about);
}

static void session_write(int idx, char* buf, size_t len)
{
	if (sessions[idx].type == SESSION_SSH)
		ssh_write_s(idx, buf, len);
	else if (sessions[idx].type == SESSION_TELNET)
	{
		/* telnet protocol: CR must be followed by LF */
		char tbuf[512];
		size_t ti = 0;
		size_t i;
		for (i = 0; i < len; i++)
		{
			tbuf[ti++] = buf[i];
			if (buf[i] == '\r')
				tbuf[ti++] = '\n';
			if (ti >= sizeof(tbuf) - 2)
			{
				tcp_write_s(idx, tbuf, ti);
				ti = 0;
			}
		}
		if (ti > 0)
			tcp_write_s(idx, tbuf, ti);
	}
}

void ssh_paste(void)
{
	int sid = active_session_global();
	if (sessions[sid].type != SESSION_SSH && sessions[sid].type != SESSION_TELNET) return;

	// GetScrap requires a handle, not a raw buffer
	// it will increase the size of the handle if needed
	Handle buf = NewHandle(256);
	int r = GetScrap(buf, 'TEXT', 0);

	if (r > 0)
	{
		session_write(sid, *buf, r);
	}

	DisposeHandle(buf);
}

void ssh_copy(void)
{
	struct window_context* wc = &ACTIVE_WIN;
	char* selection = NULL;
	size_t len = get_selection(wc, &selection);
	if (selection == NULL || len == 0) return;

	OSErr e = ZeroScrap();
	if (e != noErr) printf_i("Failed to ZeroScrap!");

	e = PutScrap(len, 'TEXT', selection);
	if (e != noErr) printf_i("Failed to PutScrap!");

	free(selection);
}

int qd_color_to_menu_item(int qd_color)
{
	switch (qd_color)
	{
		case COLOR_FROM_THEME: return 1;
		case blackColor: return 2;
		case redColor: return 3;
		case greenColor: return 4;
		case yellowColor: return 5;
		case blueColor: return 6;
		case magentaColor: return 7;
		case cyanColor: return 8;
		case whiteColor: return 9;
		default: return 1;
	}
}

int menu_item_to_qd_color(int menu_item)
{
	switch (menu_item)
	{
		case 1: return COLOR_FROM_THEME;
		case 2: return blackColor;
		case 3: return redColor;
		case 4: return greenColor;
		case 5: return yellowColor;
		case 6: return blueColor;
		case 7: return magentaColor;
		case 8: return cyanColor;
		case 9: return whiteColor;
		default: return COLOR_FROM_THEME;
	}
}

static void qd_color_to_rgb_val(int qd_color, RGBColor* out)
{
	switch (qd_color)
	{
		case blackColor:   make_rgb(out, 0x00, 0x00, 0x00); break;
		case redColor:     make_rgb(out, 0xFF, 0x00, 0x00); break;
		case greenColor:   make_rgb(out, 0x00, 0xFF, 0x00); break;
		case yellowColor:  make_rgb(out, 0xFF, 0xFF, 0x00); break;
		case blueColor:    make_rgb(out, 0x00, 0x00, 0xFF); break;
		case magentaColor: make_rgb(out, 0xFF, 0x00, 0xFF); break;
		case cyanColor:    make_rgb(out, 0x00, 0xFF, 0xFF); break;
		case whiteColor:   make_rgb(out, 0xFF, 0xFF, 0xFF); break;
		default:           make_rgb(out, 0x00, 0x00, 0x00); break;
	}
}

/* apply bg/fg overrides: call after theme load or prefs load */
static void apply_color_overrides(void)
{
	if (prefs.bg_color != COLOR_FROM_THEME)
		qd_color_to_rgb_val(prefs.bg_color, &prefs.theme_bg);
	else
		prefs.theme_bg = prefs.orig_theme_bg;

	if (prefs.fg_color != COLOR_FROM_THEME)
		qd_color_to_rgb_val(prefs.fg_color, &prefs.theme_fg);
	else
		prefs.theme_fg = prefs.orig_theme_fg;
}

int font_size_to_menu_item(int font_size)
{
	switch (font_size)
	{
		case 9:  return 1;
		case 10: return 2;
		case 12: return 3;
		case 14: return 4;
		case 18: return 5;
		case 24: return 6;
		case 36: return 7;
		default: return 1;
	}
}

int menu_item_to_font_size(int menu_item)
{
	switch (menu_item)
	{
		case 1:  return 9;
		case 2:  return 10;
		case 3:  return 12;
		case 4:  return 14;
		case 5:  return 18;
		case 6:  return 24;
		case 7:  return 36;
		default: return 9;
	}
}

void preferences_window(void)
{
	struct window_context* wc = &ACTIVE_WIN;

	// modal dialog setup
	TEInit();
	InitDialogs(NULL);
	DialogPtr dlg = GetNewDialog(DLOG_PREFERENCES, 0, (WindowPtr)-1);
	InitCursor();

	DialogItemType type;
	Handle itemH;
	Rect box;

	// draw default button indicator around the connect button
	GetDialogItem(dlg, 2, &type, &itemH, &box);
	SetDialogItem(dlg, 2, type, (Handle)NewUserItemUPP(&ButtonFrameProc), &box);

	// get the handles for each menu, set to current prefs value
	ControlHandle term_type_menu;
	GetDialogItem(dlg, 6, &type, &itemH, &box);
	term_type_menu = (ControlHandle)itemH;
	SetControlValue(term_type_menu, prefs.display_mode + 1);

	ControlHandle bg_color_menu;
	GetDialogItem(dlg, 7, &type, &itemH, &box);
	bg_color_menu = (ControlHandle)itemH;
	SetControlValue(bg_color_menu, qd_color_to_menu_item(prefs.bg_color));

	ControlHandle fg_color_menu;
	GetDialogItem(dlg, 8, &type, &itemH, &box);
	fg_color_menu = (ControlHandle)itemH;
	SetControlValue(fg_color_menu, qd_color_to_menu_item(prefs.fg_color));

	ControlHandle font_size_menu;
	GetDialogItem(dlg, 10, &type, &itemH, &box);
	font_size_menu = (ControlHandle)itemH;
	SetControlValue(font_size_menu, font_size_to_menu_item(prefs.font_size));

	/* set up theme name display (item 13) */
	{
		Handle themeH;
		Rect themeBox;
		GetDialogItem(dlg, 13, &type, &themeH, &themeBox);
		if (prefs.theme_loaded && prefs.theme_name[0] != '\0')
		{
			Str255 tname;
			int tlen = strlen(prefs.theme_name);
			if (tlen > 255) tlen = 255;
			tname[0] = tlen;
			memcpy(tname + 1, prefs.theme_name, tlen);
			SetDialogItemText(themeH, tname);
		}
		else
		{
			SetDialogItemText(themeH, "\pDefault Dark");
		}
	}

	/* let the modalmanager do everything */
	/* stop on ok or cancel; handle Theme button (item 12) inline */
	short item;
	do {
		ModalDialog(NULL, &item);
		if (item == 12)
		{
			/* Theme... button pressed */
			StandardFileReply reply;
			StandardGetFile(NULL, -1, NULL, &reply);
			if (reply.sfGood)
			{
				if (load_theme_file(&reply.sfFile))
				{
					/* update theme name display */
					Handle themeH;
					Rect themeBox;
					GetDialogItem(dlg, 13, &type, &themeH, &themeBox);
					{
						Str255 tname;
						int tlen = strlen(prefs.theme_name);
						if (tlen > 255) tlen = 255;
						tname[0] = tlen;
						memcpy(tname + 1, prefs.theme_name, tlen);
						SetDialogItemText(themeH, tname);
					}
					/* reset bg/fg to "(Theme)" for new theme */
					SetControlValue(bg_color_menu, 1);
					SetControlValue(fg_color_menu, 1);
				}
			}
		}
		else if (item == 14)
		{
			/* Dark button pressed */
			Handle themeH;
			Rect themeBox;
			init_dark_palette();
			GetDialogItem(dlg, 13, &type, &themeH, &themeBox);
			SetDialogItemText(themeH, "\pDefault Dark");
			SetControlValue(bg_color_menu, 1);
			SetControlValue(fg_color_menu, 1);
		}
		else if (item == 15)
		{
			/* Light button pressed */
			Handle themeH;
			Rect themeBox;
			init_light_palette();
			GetDialogItem(dlg, 13, &type, &themeH, &themeBox);
			SetDialogItemText(themeH, "\pDefault Light");
			SetControlValue(bg_color_menu, 1);
			SetControlValue(fg_color_menu, 1);
		}
	} while(item != 1 && item != 11);

	/* save if OK'd */
	if (item == 1)
	{
		/* read menu values into prefs */
		prefs.display_mode = GetControlValue(term_type_menu) - 1;

		prefs.bg_color = menu_item_to_qd_color(GetControlValue(bg_color_menu));
		prefs.fg_color = menu_item_to_qd_color(GetControlValue(fg_color_menu));
		apply_color_overrides();
		int new_font_size = menu_item_to_font_size(GetControlValue(font_size_menu));

		/* resize window if font size changed */
		if (new_font_size != prefs.font_size)
		{
			prefs.font_size = new_font_size;
			font_size_change(wc);
		}

		set_terminal_string();
		save_prefs();
		update_console_colors(wc);
	}

	/* clean it up */
	DisposeDialog(dlg);
	FlushEvents(everyEvent, -1);
}

// session management functions

void init_session(struct session* s)
{
	s->in_use = 0;
	s->type = SESSION_NONE;
	s->tab_label[0] = '\0';
	s->vterm = NULL;
	s->vts = NULL;
	s->cursor_x = 0;
	s->cursor_y = 0;
	s->cursor_state = 0;
	s->last_cursor_blink = 0;
	s->cursor_visible = 1;
	s->select_start_x = 0;
	s->select_start_y = 0;
	s->select_end_x = 0;
	s->select_end_y = 0;
	s->mouse_state = 0;
	s->mouse_mode = CLICK_SELECT;
	s->channel = NULL;
	s->ssh_session = NULL;
	s->endpoint = kOTInvalidEndpointRef;
	s->recv_buffer = NULL;
	s->send_buffer = NULL;
	s->telnet_host[0] = '\0';
	s->telnet_port = 0;
	s->telnet_state = 0;
	s->telnet_sb_len = 0;
	s->thread_command = WAIT;
	s->thread_state = UNINITIALIZED;
	s->shell_vRefNum = 0;
	s->shell_dirID = 0;
	s->shell_line[0] = '\0';
	s->shell_line_len = 0;
	s->shell_cursor_pos = 0;
	s->window_id = -1;
}

/* adjust window size when tab bar appears/disappears, keeping terminal rows the same */
static void adjust_window_for_tabs(struct window_context* wc, int tabs_appeared)
{
	Rect pr = wc->win->portRect;
	int cur_height = pr.bottom - pr.top;
	int cur_width = pr.right - pr.left;
	int delta = tabs_appeared ? TAB_BAR_HEIGHT : -TAB_BAR_HEIGHT;
	int new_height = cur_height + delta;

	/* check if we'd go off-screen; if so, shrink terminal instead */
	{
		GDHandle gd = GetMainDevice();
		Rect screen = (**gd).gdRect;
		Point win_top = {0, 0};
		SetPort(wc->win);
		LocalToGlobal(&win_top);
		int max_height = screen.bottom - win_top.v - 2;

		if (new_height > max_height)
			new_height = max_height;
	}

	SizeWindow(wc->win, cur_width, new_height, true);
	EraseRect(&(wc->win->portRect));
	InvalRect(&(wc->win->portRect));
	wc->needs_redraw = 1;

	/* recalculate terminal size from new window dims */
	{
		Rect npr = wc->win->portRect;
		int tab_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;
		int usable_height = (npr.bottom - npr.top) - tab_offset;
		int new_x = (npr.right - npr.left - 4) / con.cell_width;
		int new_y = (usable_height - 2) / con.cell_height;
		int i;

		if (new_x < 1) new_x = 1;
		if (new_y < 1) new_y = 1;

		wc->size_x = new_x;
		wc->size_y = new_y;

		for (i = 0; i < wc->num_sessions; i++)
		{
			int sid = wc->session_ids[i];
			if (sessions[sid].in_use && sessions[sid].vterm)
			{
				vterm_set_size(sessions[sid].vterm, wc->size_y, wc->size_x);
				if (sessions[sid].type == SESSION_SSH && sessions[sid].channel)
					libssh2_channel_request_pty_size(sessions[sid].channel,
						wc->size_x, wc->size_y);
			}
		}
	}
}

int new_session(struct window_context* wc, enum SESSION_TYPE type)
{
	if (wc->num_sessions >= MAX_SESSIONS) return -1;

	// find a free slot in global sessions array
	int idx = -1;
	int i;
	for (i = 0; i < MAX_SESSIONS; i++)
	{
		if (!sessions[i].in_use &&
			sessions[i].thread_state == DONE)
		{
			idx = i;
			break;
		}
	}
	if (idx < 0) return -1;

	init_session(&sessions[idx]);
	sessions[idx].in_use = 1;
	sessions[idx].type = type;
	add_session_to_window(wc, idx);

	// grow window if tab bar just appeared (1 -> 2 sessions)
	if (wc->num_sessions == 2)
		adjust_window_for_tabs(wc, 1);

	SetPort(wc->win);
	setup_session_vterm(wc, idx);

	if (type == SESSION_SSH)
	{
		snprintf(sessions[idx].tab_label, sizeof(sessions[idx].tab_label), "new connection");
	}
	else if (type == SESSION_LOCAL)
	{
		snprintf(sessions[idx].tab_label, sizeof(sessions[idx].tab_label), "local shell");
	}
	else if (type == SESSION_TELNET)
	{
		snprintf(sessions[idx].tab_label, sizeof(sessions[idx].tab_label), "telnet");
	}
	switch_session(wc, idx);

	// enable Close Tab now that we have multiple sessions
	if (wc->num_sessions > 1)
	{
		void* menu = GetMenuHandle(MENU_FILE);
		EnableItem(menu, FMENU_CLOSE_TAB);
	}

	// start the session
	if (type == SESSION_SSH)
	{
		if (ssh_connect(idx) == 0) ssh_disconnect(idx);
	}
	else if (type == SESSION_LOCAL)
	{
		shell_init(idx);
	}
	/* telnet/nc: connection is started by the shell command after
	   setting host/port on the session. Don't auto-connect here. */

	// redraw since tab bar may have appeared
	InvalRect(&(wc->win->portRect));
	wc->needs_redraw = 1;

	return idx;
}

void close_session(int idx)
{
	if (idx < 0 || idx >= MAX_SESSIONS) return;
	if (!sessions[idx].in_use) return;

	struct window_context* wc = window_for_session(idx);
	if (wc == NULL) return;
	if (wc->num_sessions <= 1) return; // don't close the last session in a window

	// disconnect networked sessions (waits for thread to reach DONE)
	if (sessions[idx].type == SESSION_SSH && sessions[idx].thread_state != DONE)
		ssh_disconnect(idx);
	else if (sessions[idx].type == SESSION_TELNET && sessions[idx].thread_state != DONE)
		telnet_disconnect(idx);

	// null out vterm callbacks to prevent use during teardown
	if (sessions[idx].vterm != NULL)
	{
		VTermScreen* vts = vterm_obtain_screen(sessions[idx].vterm);
		vterm_screen_set_callbacks(vts, NULL, NULL);
		vterm_output_set_callback(sessions[idx].vterm, NULL, NULL);
	}

	/* local sessions have no thread — safe to mark DONE for slot reuse */
	if (sessions[idx].type == SESSION_LOCAL)
		sessions[idx].thread_state = DONE;

	/* free vterm only if thread is confirmed done;
	   if thread timed out, leak rather than use-after-free */
	if (sessions[idx].vterm != NULL && sessions[idx].thread_state == DONE)
	{
		vterm_free(sessions[idx].vterm);
		sessions[idx].vterm = NULL;
	}

	sessions[idx].in_use = 0;
	remove_session_from_window(wc, idx);

	// update window title to reflect the now-active session
	{
		int new_sid = wc->session_ids[wc->active_session_idx];
		set_window_title(wc->win, sessions[new_sid].tab_label,
			strlen(sessions[new_sid].tab_label));
	}

	// shrink window if tab bar just disappeared (2 -> 1 sessions)
	if (wc->num_sessions == 1)
		adjust_window_for_tabs(wc, 0);

	/* update menu state */
	{
		void* menu = GetMenuHandle(MENU_FILE);
		int active_sid = wc->session_ids[wc->active_session_idx];
		if (sessions[active_sid].type == SESSION_SSH ||
			sessions[active_sid].type == SESSION_TELNET)
		{
			if (sessions[active_sid].thread_state == OPEN)
			{
				DisableItem(menu, FMENU_CONNECT);
				EnableItem(menu, FMENU_DISCONNECT);
			}
			else
			{
				EnableItem(menu, FMENU_CONNECT);
				DisableItem(menu, FMENU_DISCONNECT);
			}
		}
		else
		{
			DisableItem(menu, FMENU_CONNECT);
			DisableItem(menu, FMENU_DISCONNECT);
		}

		/* disable Close Tab if only 1 session left */
		if (wc->num_sessions <= 1)
			DisableItem(menu, FMENU_CLOSE_TAB);
	}

	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));
	wc->needs_redraw = 1;
}

void switch_session(struct window_context* wc, int idx)
{
	if (idx < 0 || idx >= MAX_SESSIONS) return;
	if (!sessions[idx].in_use) return;

	// find the index in the window's session_ids
	int i, found = -1;
	for (i = 0; i < wc->num_sessions; i++)
	{
		if (wc->session_ids[i] == idx)
		{
			found = i;
			break;
		}
	}
	if (found < 0) return;
	if (found == wc->active_session_idx) return;

	wc->active_session_idx = found;

	/* update menu enable/disable based on new active session */
	{
		void* menu = GetMenuHandle(MENU_FILE);
		if (sessions[idx].type == SESSION_SSH ||
			sessions[idx].type == SESSION_TELNET)
		{
			if (sessions[idx].thread_state == OPEN)
			{
				DisableItem(menu, FMENU_CONNECT);
				EnableItem(menu, FMENU_DISCONNECT);
			}
			else
			{
				EnableItem(menu, FMENU_CONNECT);
				DisableItem(menu, FMENU_DISCONNECT);
			}
		}
		else
		{
			DisableItem(menu, FMENU_CONNECT);
			DisableItem(menu, FMENU_DISCONNECT);
		}
	}

	/* update window title to match new active session */
	set_window_title(wc->win, sessions[idx].tab_label, strlen(sessions[idx].tab_label));

	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));
	wc->needs_redraw = 1;
}

/* ---- window lifecycle ---- */

int new_window(void)
{
	// find a free slot
	int wid = -1;
	int i;
	for (i = 0; i < MAX_WINDOWS; i++)
	{
		if (!windows[i].in_use)
		{
			wid = i;
			break;
		}
	}
	if (wid < 0) return -1;

	struct window_context* wc = &windows[wid];
	memset(wc, 0, sizeof(struct window_context));
	wc->in_use = 1;
	wc->size_x = 80;
	wc->size_y = 24;

	// stagger window position based on window count
	Rect initial_window_bounds = qd.screenBits.bounds;
	InsetRect(&initial_window_bounds, 20, 20);
	initial_window_bounds.top += 40;

	// offset by 20 pixels for each existing window
	initial_window_bounds.top += num_windows * 20;
	initial_window_bounds.left += num_windows * 20;

	initial_window_bounds.bottom = initial_window_bounds.top + con.cell_height * wc->size_y + 4;
	initial_window_bounds.right = initial_window_bounds.left + con.cell_width * wc->size_x + 4;

	ConstStr255Param title = "\pSevenTTY " APP_VERSION;

	WindowPtr win = NewCWindow(NULL, &initial_window_bounds, title, true, documentProc, (WindowPtr)-1, true, 0);

	SetPort(win);
	EraseRect(&win->portRect);

	wc->win = win;
	num_windows++;
	active_window = wid;

	// create initial local shell session
	new_session(wc, SESSION_LOCAL);

	return wid;
}

void close_window(int wid)
{
	if (wid < 0 || wid >= MAX_WINDOWS) return;
	struct window_context* wc = &windows[wid];
	if (!wc->in_use) return;

	// close all sessions in this window (disconnect SSH first)
	while (wc->num_sessions > 0)
	{
		int sid = wc->session_ids[0];

		// disconnect networked sessions (waits for thread to reach DONE)
		if (sessions[sid].type == SESSION_SSH && sessions[sid].thread_state != DONE)
			ssh_disconnect(sid);
		else if (sessions[sid].type == SESSION_TELNET && sessions[sid].thread_state != DONE)
			telnet_disconnect(sid);

		// null out vterm callbacks to prevent use during teardown
		if (sessions[sid].vterm != NULL)
		{
			VTermScreen* vts = vterm_obtain_screen(sessions[sid].vterm);
			vterm_screen_set_callbacks(vts, NULL, NULL);
			vterm_output_set_callback(sessions[sid].vterm, NULL, NULL);
		}

		/* local sessions have no thread — safe to mark DONE for slot reuse */
		if (sessions[sid].type == SESSION_LOCAL)
			sessions[sid].thread_state = DONE;

		/* free vterm only if thread is confirmed done */
		if (sessions[sid].vterm != NULL && sessions[sid].thread_state == DONE)
		{
			vterm_free(sessions[sid].vterm);
			sessions[sid].vterm = NULL;
		}

		sessions[sid].in_use = 0;
		remove_session_from_window(wc, sid);
	}

	if (wc->win != NULL)
	{
		DisposeWindow(wc->win);
		wc->win = NULL;
	}

	wc->in_use = 0;
	num_windows--;

	// switch active_window to another open window
	if (num_windows > 0)
	{
		int i;
		for (i = 0; i < MAX_WINDOWS; i++)
		{
			if (windows[i].in_use)
			{
				active_window = i;
				SetPort(windows[i].win);
				SelectWindow(windows[i].win);
				break;
			}
		}
	}
}

// returns 1 if quit selected, else 0
int process_menu_select(int32_t result)
{
	int exit = 0;
	int16_t menu = (result & 0xFFFF0000) >> 16;
	int16_t item = (result & 0x0000FFFF);
	Str255 name;
	struct window_context* wc = &ACTIVE_WIN;

	switch (menu)
	{
		case MENU_APPLE:
			if (item == 1)
			{
				display_about_box();
			}
			else
			{
				GetMenuItemText(GetMenuHandle(menu), item, name);
				OpenDeskAcc(name);
			}
			break;

		case MENU_FILE:
			if (item == FMENU_CONNECT)
			{
				int sid = active_session_global();
				if (ssh_connect(sid) == 0) ssh_disconnect(sid);
			}
			if (item == FMENU_DISCONNECT)
			{
				int sid = active_session_global();
				if (sessions[sid].type == SESSION_SSH)
					ssh_disconnect(sid);
				else if (sessions[sid].type == SESSION_TELNET)
					telnet_disconnect(sid);
			}
			if (item == FMENU_NEW_WINDOW) new_window();
			if (item == FMENU_NEW_LOCAL) new_session(wc, SESSION_LOCAL);
			if (item == FMENU_NEW_SSH) new_session(wc, SESSION_SSH);
			if (item == FMENU_CLOSE_TAB) close_session(active_session_global());
			if (item == FMENU_PREFS) preferences_window();
			if (item == FMENU_QUIT) exit = 1;
			break;

		case MENU_EDIT:
			if (item == 4) ssh_copy();
			if (item == 5) ssh_paste();
			break;

		default:
			break;
	}

	HiliteMenu(0);
	return exit;
}

void resize_con_window(struct window_context* wc, EventRecord event)
{
	clear_selection(wc);

	int tab_offset = (wc->num_sessions > 1) ? TAB_BAR_HEIGHT : 0;

	// limits on window size
	Rect window_limits = { .top = con.cell_height*2 + 2 + tab_offset,
		.bottom = con.cell_height*100 + 2 + tab_offset,
		.left = con.cell_width*10 + 4,
		.right = con.cell_width*200 + 4 };

	long growResult = GrowWindow(wc->win, event.where, &window_limits);

	if (growResult != 0)
	{
		int height = growResult >> 16;
		int width = growResult & 0xFFFF;
		int usable_height = height - tab_offset;

		// 'snap' to a size that won't have extra pixels not in a cell
		int next_height = height - ((usable_height - 4) % con.cell_height);
		int next_width = width - ((width - 4) % con.cell_width);

		SizeWindow(wc->win, next_width, next_height, true);
		EraseRect(&(wc->win->portRect));
		InvalRect(&(wc->win->portRect));
		wc->needs_redraw = 1;

		wc->size_x = (next_width - 4)/con.cell_width;
		wc->size_y = (usable_height - 2)/con.cell_height;

		// update vterm size for all sessions in this window
		int i;
		for (i = 0; i < wc->num_sessions; i++)
		{
			int sid = wc->session_ids[i];
			if (sessions[sid].in_use && sessions[sid].vterm)
			{
				vterm_set_size(sessions[sid].vterm, wc->size_y, wc->size_x);
				if (sessions[sid].type == SESSION_SSH && sessions[sid].channel)
					libssh2_channel_request_pty_size(sessions[sid].channel, wc->size_x, wc->size_y);
				if (sessions[sid].type == SESSION_TELNET)
					telnet_send_naws(sid);
			}
		}
	}
}

int handle_keypress(EventRecord* event)
{
	unsigned char c = event->message & charCodeMask;
	struct window_context* wc = &ACTIVE_WIN;
	int sid = active_session_global();

	// if we have a key and command, and it's not autorepeating
	if (c && event->what != autoKey && event->modifiers & cmdKey)
	{
		switch (c)
		{
			case 'k':
				if (sessions[sid].type != SESSION_LOCAL &&
					(sessions[sid].thread_state == UNINITIALIZED || sessions[sid].thread_state == DONE))
				{
					if (ssh_connect(sid) == 0) ssh_disconnect(sid);
				}
				break;
			case 'd':
				if (sessions[sid].type == SESSION_SSH && sessions[sid].thread_state == OPEN)
				{
					ssh_disconnect(sid);
					if (wc->num_sessions > 1)
						close_session(sid);
					else if (num_windows > 1)
						close_window(active_window);
					else
						return 1;
				}
				else if ((sessions[sid].type == SESSION_TELNET)
						 && sessions[sid].thread_state != DONE
						 && sessions[sid].thread_state != UNINITIALIZED)
				{
					telnet_disconnect(sid);
					if (wc->num_sessions > 1)
						close_session(sid);
					else if (num_windows > 1)
						close_window(active_window);
					else
						return 1;
				}
				else if (wc->num_sessions > 1)
					close_session(sid);
				else if (num_windows > 1)
					close_window(active_window);
				else
					return 1; // last window, last tab -> quit
				break;
			case 'q':
				return 1;
				break;
			case 'v':
				ssh_paste();
				break;
			case 'c':
				ssh_copy();
				break;
			case 't':
				new_session(wc, SESSION_LOCAL);
				break;
			case 's':
				new_session(wc, SESSION_SSH);
				break;
			case 'n':
				new_window();
				break;
			case 'w':
				if (wc->num_sessions > 1)
					close_session(sid);
				else if (num_windows > 1)
					close_window(active_window);
				else
					return 1; // last window, last tab -> quit
				break;
			case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8':
			{
				int target = c - '1';
				if (target < wc->num_sessions)
				{
					switch_session(wc, wc->session_ids[target]);
				}
				break;
			}
			default:
				break;
		}
	}
	else if (c)
	{
		// shift+page up/down: scroll through scrollback buffer
		if (event->modifiers & (shiftKey | rightShiftKey))
		{
			if (c == kPageUpCharCode)
			{
				scroll_up(wc);
				return 0;
			}
			else if (c == kPageDownCharCode)
			{
				scroll_down(wc);
				return 0;
			}
		}

		// reset cursor to solid on keypress (stays visible during typing)
		sessions[sid].cursor_state = 1;
		sessions[sid].last_cursor_blink = TickCount();

		// any other key snaps back to live view
		if (sessions[sid].scroll_offset > 0)
			scroll_reset(wc);

		// for local shell sessions, keypresses go to shell handler
		if (sessions[sid].type == SESSION_LOCAL)
		{
			shell_input(sid, c, event->modifiers, (event->message & keyCodeMask) >> 8);
			return 0;
		}

		/* telnet escape: Ctrl+] cancels connecting or disconnects
		   check virtual keycode 0x1E (] key) AND either controlKey
		   modifier (left-Ctrl) or charcode 0x1D (right-Ctrl via QEMU,
		   which sends the control code without the modifier flag) */
		if (sessions[sid].type == SESSION_TELNET &&
		    ((event->message & keyCodeMask) >> 8) == 0x1E &&
		    ((event->modifiers & controlKey) || c == 0x1D) &&
		    sessions[sid].thread_state != DONE &&
		    sessions[sid].thread_state != UNINITIALIZED)
		{
			printf_s(sid, "\r\nConnection closed.\r\n");
			telnet_disconnect(sid);
			if (wc->num_sessions > 1)
				close_session(sid);
			else if (num_windows > 1)
				close_window(active_window);
			else
				return 1;
			return 0;
		}

		/* network session keypress handling (SSH, telnet) */
		if ((sessions[sid].type != SESSION_SSH &&
			 sessions[sid].type != SESSION_TELNET) ||
			sessions[sid].thread_state != OPEN)
			return 0;

		/* get the unmodified version of the keypress */
		{
			uint8_t unmodified_key = keycode_to_ascii[(event->message & keyCodeMask)>>8];
			uint8_t vkeycode = (event->message & keyCodeMask) >> 8;

			/* numpad Enter (vkeycode 0x4C): treat as Return */
			if (vkeycode == 0x4C)
			{
				sessions[sid].send_buffer[0] = '\r';
				session_write(sid, sessions[sid].send_buffer, 1);
			}
			/* right-Ctrl+key: QEMU sends charcode as control code but no
			   controlKey modifier.  Detect by checking that the virtual
			   keycode maps to a letter (not numpad Enter 0x4C) and the
			   charcode is a control code (< 32). */
			else if (!(event->modifiers & controlKey) &&
			    c < 32 && c != '\r' && c != '\t' && c != '\033' &&
			    ascii_to_control_code[unmodified_key] == c)
			{
				sessions[sid].send_buffer[0] = c;
				session_write(sid, sessions[sid].send_buffer, 1);
			}
			/* if we have a control code for this key */
			else if (event->modifiers & controlKey && ascii_to_control_code[unmodified_key] != 255)
			{
				sessions[sid].send_buffer[0] = ascii_to_control_code[unmodified_key];
				session_write(sid, sessions[sid].send_buffer, 1);
			}
			else
			{
				if (event->modifiers & optionKey && c >= 32 && c <= 126)
				{
					sessions[sid].send_buffer[0] = c;
					session_write(sid, sessions[sid].send_buffer, 1);
				}
				else
				{
					if (event->modifiers & optionKey)
					{
						sessions[sid].send_buffer[0] = '\e';
						session_write(sid, sessions[sid].send_buffer, 1);
					}

					if (key_to_vterm[c] != VTERM_KEY_NONE)
					{
						vterm_keyboard_key(sessions[sid].vterm, key_to_vterm[c], VTERM_MOD_NONE);
					}
					else
					{
						sessions[sid].send_buffer[0] = event->modifiers & optionKey ? unmodified_key : c;
						session_write(sid, sessions[sid].send_buffer, 1);
					}
				}
			}
		}
	}

	return 0;
}

void event_loop(void)
{
	int exit_event_loop = 0;
	EventRecord event;
	WindowPtr eventWin;
	Point local_mouse_position;

	// maximum length of time to sleep (in ticks)
	// GetCaretTime gets the number of ticks between system caret on/off time
	long int sleep_time = GetCaretTime() / 4;

	do
	{
		// wait to get a GUI event
		while (!WaitNextEvent(everyEvent, &event, sleep_time, NULL))
		{
			YieldToAnyThread();

			// iterate all windows for idle tasks
			int i;
			for (i = 0; i < MAX_WINDOWS; i++)
			{
				if (!windows[i].in_use) continue;
				SetPort(windows[i].win);
				check_cursor(&windows[i]);

				if (windows[i].needs_redraw)
				{
					windows[i].needs_redraw = 0;
					BeginUpdate(windows[i].win);
					draw_screen(&windows[i], &(windows[i].win->portRect));
					EndUpdate(windows[i].win);
				}
			}
		}

		// might need to toggle our cursor even if we got an event
		{
			int i;
			for (i = 0; i < MAX_WINDOWS; i++)
			{
				if (!windows[i].in_use) continue;
				SetPort(windows[i].win);
				check_cursor(&windows[i]);
			}
		}

		// handle any GUI events
		switch (event.what)
		{
			case updateEvt:
				eventWin = (WindowPtr)event.message;
				{
					struct window_context* wc = find_window_context(eventWin);
					if (wc != NULL)
					{
						SetPort(eventWin);
						BeginUpdate(eventWin);
						draw_screen(wc, &(eventWin->portRect));
						EndUpdate(eventWin);
					}
				}
				break;

			case keyDown:
			case autoKey: // autokey means we're repeating a held down key event
				exit_event_loop = handle_keypress(&event);
				break;

			case mouseDown:
				switch(FindWindow(event.where, &eventWin))
				{
					case inDrag:
						DragWindow(eventWin, event.where, &(*(GetGrayRgn()))->rgnBBox);
						break;

					case inGrow:
						{
							struct window_context* wc = find_window_context(eventWin);
							if (wc != NULL)
							{
								SetPort(eventWin);
								resize_con_window(wc, event);
							}
						}
						break;

					case inGoAway:
						{
							if (TrackGoAway(eventWin, event.where))
							{
								struct window_context* wc = find_window_context(eventWin);
								if (wc != NULL)
								{
									int wid = (int)(wc - windows);
									close_window(wid);
									if (num_windows == 0)
										exit_event_loop = 1;
								}
							}
						}
						break;

					case inMenuBar:
						exit_event_loop = process_menu_select(MenuSelect(event.where));
						break;

					case inSysWindow:
						SystemClick(&event, eventWin);
						break;

					case inContent:
						{
							struct window_context* wc = find_window_context(eventWin);
							if (wc != NULL)
							{
								// activate this window if not already active
								int wid = (int)(wc - windows);
								if (wid != active_window)
								{
									active_window = wid;
									SelectWindow(wc->win);
								}
								SetPort(eventWin);
								GetMouse(&local_mouse_position);
								mouse_click(wc, local_mouse_position, true);
							}
						}
						break;
				}
				break;
			case mouseUp:
				// only tell the console to lift the mouse if we clicked through it
				{
					struct window_context* wc = &ACTIVE_WIN;
					int sid = active_session_global();
					if (sessions[sid].mouse_state)
					{
						SetPort(wc->win);
						GetMouse(&local_mouse_position);
						mouse_click(wc, local_mouse_position, false);
					}
				}
				break;

			case activateEvt:
				eventWin = (WindowPtr)event.message;
				{
					struct window_context* wc = find_window_context(eventWin);
					if (wc != NULL && (event.modifiers & activeFlag))
					{
						active_window = (int)(wc - windows);
					}
				}
				break;
		}

		YieldToAnyThread();
	} while (!exit_event_loop && !exit_requested);
}

// from the ATS password sample code
pascal Boolean TwoItemFilter(DialogPtr dlog, EventRecord *event, short *itemHit)
{
	DialogPtr evtDlog;
	short selStart, selEnd;
	long unsigned ticks;
	Handle itemH;
	DialogItemType type;
	Rect box;

	// TODO: this should be declared somewhere? include it?
	int kVisualDelay = 8;

	if (event->what == keyDown || event->what == autoKey)
	{
		char c = event->message & charCodeMask;
		switch (c)
		{
			case kEnterCharCode: // select the ok button
			case kReturnCharCode: // we have to manually blink it...
			case kLineFeedCharCode:
				GetDialogItem(dlog, 1, &type, &itemH, &box);
				HiliteControl((ControlHandle)(itemH), kControlButtonPart);
				Delay(kVisualDelay, &ticks);
				HiliteControl((ControlHandle)(itemH), 1);
				*itemHit = 1;
				return true;

			case kTabCharCode: // cancel out tab events
				event->what = nullEvent;
				return false;

			case kEscapeCharCode: // hit cancel on esc or cmd-period
			case '.':
				if ((event->modifiers & cmdKey) || c == kEscapeCharCode)
				{
					GetDialogItem(dlog, 6, &type, &itemH, &box);
					HiliteControl((ControlHandle)(itemH), kControlButtonPart);
					Delay(kVisualDelay, &ticks);
					HiliteControl((ControlHandle)(itemH), 6);
					*itemHit = 6;
					return true;
				}
				__attribute__ ((fallthrough)); // fall through in case of a plain '.'

			default: // TODO: this is dumb and assumes everything else is a valid text character
				selStart = (**((DialogPeek)dlog)->textH).selStart; // Get the selection in the visible item
				selEnd = (**((DialogPeek)dlog)->textH).selEnd;
				SelectDialogItemText(dlog, 5, selStart, selEnd); // Select text in invisible item
				DialogSelect(event, &evtDlog, itemHit); // Input key
				SelectDialogItemText(dlog, 4, selStart, selEnd); // Select same area in visible item
				if ((event->message & charCodeMask) != kBackspaceCharCode) // If it's not a backspace (backspace is the only key that can affect both the text and the selection- thus we need to process it in both fields, but not change it for the hidden field.
					event->message = 0xa5; // Replace with character to use (the bullet)
				DialogSelect(event, &evtDlog, itemHit); // Put in fake character
				return true;

			case kLeftArrowCharCode:
			case kRightArrowCharCode:
			case kUpArrowCharCode:
			case kDownArrowCharCode:
			case kHomeCharCode:
			case kEndCharCode:
				return false; // don't handle them
		}
	}

	return false; // pass on other (non-keypress) events
}

// from the ATS password sample code
// 1 for ok, 0 for cancel
int password_dialog(int dialog_resource)
{
	int ret = 1;

	DialogPtr dlog;
	Handle itemH;
	short item;
	Rect box;
	DialogItemType type;

	dlog = GetNewDialog(dialog_resource, 0, (WindowPtr) - 1);

	// draw default button indicator around the connect button
	GetDialogItem(dlog, 2, &type, &itemH, &box);
	SetDialogItem(dlog, 2, type, (Handle)NewUserItemUPP(&ButtonFrameProc), &box);

	do {
		ModalDialog(NewModalFilterUPP(TwoItemFilter), &item);
	} while (item != 1 && item != 6); // until OK or cancel

	if (6 == item) ret = 0;

	// read out of the hidden text box
	GetDialogItem(dlog, 5, &type, &itemH, &box);
	GetDialogItemText(itemH, (unsigned char*)prefs.password);
	prefs.auth_type = USE_PASSWORD;

	DisposeDialog(dlog);

	return ret;
}

// derived from More Files sample code
OSErr
FSpPathFromLocation(
FSSpec *spec,		/* The location we want a path for. */
int *length,		/* Length of the resulting path. */
Handle *fullPath)		/* Handle to path. */
{
	OSErr err;
	FSSpec tempSpec;
	CInfoPBRec pb;

	*fullPath = NULL;

	/*
	* Make a copy of the input FSSpec that can be modified.
	*/
	BlockMoveData(spec, &tempSpec, sizeof(FSSpec));

	if (tempSpec.parID == fsRtParID) {
		/*
		* The object is a volume.  Add a colon to make it a full
		* pathname.  Allocate a handle for it and we are done.
		*/
		tempSpec.name[0] += 2;
		tempSpec.name[tempSpec.name[0] - 1] = ':';
		tempSpec.name[tempSpec.name[0]] = '\0';

		err = PtrToHand(&tempSpec.name[1], fullPath, tempSpec.name[0]);
	} else {
		/*
		* The object isn't a volume.  Is the object a file or a directory?
		*/
		pb.dirInfo.ioNamePtr = tempSpec.name;
		pb.dirInfo.ioVRefNum = tempSpec.vRefNum;
		pb.dirInfo.ioDrDirID = tempSpec.parID;
		pb.dirInfo.ioFDirIndex = 0;
		err = PBGetCatInfoSync(&pb);

		if ((err == noErr) || (err == fnfErr)) {
			/*
			* If the file doesn't currently exist we start over.  If the
			* directory exists everything will work just fine.  Otherwise we
			* will just fail later.  If the object is a directory, append a
			* colon so full pathname ends with colon.
			*/
			if (err == fnfErr) {
			BlockMoveData(spec, &tempSpec, sizeof(FSSpec));
			} else if ( (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0 ) {
			tempSpec.name[0] += 1;
			tempSpec.name[tempSpec.name[0]] = ':';
			}

			/*
			* Create a new Handle for the object - make it a C string
			*/
			tempSpec.name[0] += 1;
			tempSpec.name[tempSpec.name[0]] = '\0';
			err = PtrToHand(&tempSpec.name[1], fullPath, tempSpec.name[0]);
			if (err == noErr) {
				/*
				* Get the ancestor directory names - loop until we have an
				* error or find the root directory.
				*/
				pb.dirInfo.ioNamePtr = tempSpec.name;
				pb.dirInfo.ioVRefNum = tempSpec.vRefNum;
				pb.dirInfo.ioDrParID = tempSpec.parID;
				do {
					pb.dirInfo.ioFDirIndex = -1;
					pb.dirInfo.ioDrDirID = pb.dirInfo.ioDrParID;
					err = PBGetCatInfoSync(&pb);
					if (err == noErr) {
						/*
						* Append colon to directory name and add
						* directory name to beginning of fullPath
						*/
						++tempSpec.name[0];
						tempSpec.name[tempSpec.name[0]] = ':';

						(void) Munger(*fullPath, 0, NULL, 0, &tempSpec.name[1],
						tempSpec.name[0]);
						err = MemError();
					}
				} while ( (err == noErr) && (pb.dirInfo.ioDrDirID != fsRtDirID) );
			}
		}
	}

	/*
	* On error Dispose the handle, set it to NULL & return the err.
	* Otherwise, set the length & return.
	*/
	if (err == noErr) {
	*length = GetHandleSize(*fullPath) - 1;
	} else {
	if ( *fullPath != NULL ) {
	DisposeHandle(*fullPath);
	}
	*fullPath = NULL;
	*length = 0;
	}

	return err;
}

int key_dialog(void)
{
	Handle full_path = NULL;
	int path_length = 0;

	// if we don't have a saved pubkey path, ask for one
	if (prefs.pubkey_path == NULL || prefs.pubkey_path[0] == '\0')
	{
		NoteAlert(ALRT_PUBKEY, nil);
		StandardFileReply pubkey;
		StandardGetFile(NULL, 0, NULL, &pubkey);
		if (!pubkey.sfGood) return 0;
		if (FSpPathFromLocation(&pubkey.sfFile, &path_length, &full_path) != noErr
			|| full_path == NULL || path_length <= 0)
		{
			if (full_path != NULL) DisposeHandle(full_path);
			return 0;
		}
		prefs.pubkey_path = malloc(path_length+1);
		if (prefs.pubkey_path == NULL) { DisposeHandle(full_path); return 0; }
		strncpy(prefs.pubkey_path, (char*)(*full_path), path_length+1);
		DisposeHandle(full_path);
	}

	path_length = 0;
	full_path = NULL;

	// if we don't have a saved privkey path, ask for one
	if (prefs.privkey_path == NULL || prefs.privkey_path[0] == '\0')
	{
		NoteAlert(ALRT_PRIVKEY, nil);
		StandardFileReply privkey;
		StandardGetFile(NULL, 0, NULL, &privkey);
		if (!privkey.sfGood) return 0;
		if (FSpPathFromLocation(&privkey.sfFile, &path_length, &full_path) != noErr
			|| full_path == NULL || path_length <= 0)
		{
			if (full_path != NULL) DisposeHandle(full_path);
			return 0;
		}
		prefs.privkey_path = malloc(path_length+1);
		if (prefs.privkey_path == NULL) { DisposeHandle(full_path); return 0; }
		strncpy(prefs.privkey_path, (char*)(*full_path), path_length+1);
		DisposeHandle(full_path);
	}

	// get the key decryption password
	if (!password_dialog(DLOG_KEY_PASSWORD)) return 0;

	prefs.auth_type = USE_KEY;
	return 1;
}

int intro_dialog(void)
{
	// modal dialog setup
	TEInit();
	InitDialogs(NULL);
	DialogPtr dlg = GetNewDialog(DLOG_CONNECT, 0, (WindowPtr)-1);
	InitCursor();

	DialogItemType type;
	Handle itemH;
	Rect box;

	// draw default button indicator around the connect button
	GetDialogItem(dlg, 2, &type, &itemH, &box);
	SetDialogItem(dlg, 2, type, (Handle)NewUserItemUPP(&ButtonFrameProc), &box);

	// get the handles for each of the text boxes, and load preference data in
	ControlHandle address_text_box;
	GetDialogItem(dlg, 4, &type, &itemH, &box);
	address_text_box = (ControlHandle)itemH;
	// only show hostname part (before the :port suffix) in the dialog
	{
		Str255 host_only;
		int hlen = prefs.hostname[0];
		if (hlen > 255) hlen = 255;
		host_only[0] = hlen;
		memcpy(host_only + 1, prefs.hostname + 1, hlen);
		SetDialogItemText((Handle)address_text_box, host_only);
	}

	// select all text in hostname dialog item
	SelectDialogItemText(dlg, 4, 0, 32767);

	ControlHandle port_text_box;
	GetDialogItem(dlg, 5, &type, &itemH, &box);
	port_text_box = (ControlHandle)itemH;
	SetDialogItemText((Handle)port_text_box, (ConstStr255Param)prefs.port);

	ControlHandle username_text_box;
	GetDialogItem(dlg, 7, &type, &itemH, &box);
	username_text_box = (ControlHandle)itemH;
	SetDialogItemText((Handle)username_text_box, (ConstStr255Param)prefs.username);

	ControlHandle password_radio;
	GetDialogItem(dlg, 9, &type, &itemH, &box);
	password_radio = (ControlHandle)itemH;
	SetControlValue(password_radio, 0);

	ControlHandle key_radio;
	GetDialogItem(dlg, 10, &type, &itemH, &box);
	key_radio = (ControlHandle)itemH;
	SetControlValue(key_radio, 0);

	// recall last-used connection type
	if (prefs.auth_type == USE_PASSWORD)
	{
		SetControlValue(password_radio, 1);
	}
	else
	{
		SetControlValue(key_radio, 1);
	}

	// let the modalmanager do everything
	// stop when the connect button is hit
	short item;
	do {
		ModalDialog(NULL, &item);
		if (item == 9)
		{
			SetControlValue(key_radio, 0);
			SetControlValue(password_radio, 1);
		}
		else if (item == 10)
		{
			SetControlValue(key_radio, 1);
			SetControlValue(password_radio, 0);
		}
	} while(item != 1 && item != 8);

	// copy the text out of the boxes
	GetDialogItemText((Handle)address_text_box, (unsigned char *)prefs.hostname);
	GetDialogItemText((Handle)username_text_box, (unsigned char *)prefs.username);

	// sanitize hostname: only allow valid DNS chars (a-z, 0-9, '.', '-')
	{
		int src, dst;
		int len = (unsigned char)prefs.hostname[0];
		dst = 1;
		for (src = 1; src <= len; src++)
		{
			char c = prefs.hostname[src];
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			    (c >= '0' && c <= '9') || c == '.' || c == '-')
				prefs.hostname[dst++] = c;
		}
		prefs.hostname[0] = dst - 1;
	}

	// read port into a temp pascal string, sanitize to digits only
	{
		Str255 raw_port;
		GetDialogItemText((Handle)port_text_box, raw_port);
		int src, dst;
		int len = (unsigned char)raw_port[0];
		dst = 1;
		for (src = 1; src <= len; src++)
		{
			char c = raw_port[src];
			if (c >= '0' && c <= '9')
				raw_port[dst++] = c;
		}
		raw_port[0] = dst - 1;

		// copy sanitized port into prefs.port
		prefs.port[0] = raw_port[0];
		memcpy(prefs.port + 1, raw_port + 1, raw_port[0]);
	}

	// build combined "hostname:port" C string in prefs.hostname buffer
	{
		int hlen = (unsigned char)prefs.hostname[0];
		int plen = (unsigned char)prefs.port[0];
		if (hlen + 2 + plen >= (int)sizeof(prefs.hostname))
			plen = sizeof(prefs.hostname) - hlen - 3;
		prefs.hostname[hlen + 1] = ':';
		memcpy(prefs.hostname + hlen + 2, prefs.port + 1, plen);
		prefs.hostname[hlen + 2 + plen] = '\0';
	}

	int use_password = GetControlValue(password_radio);

	// clean it up
	DisposeDialog(dlg);
	FlushEvents(everyEvent, -1);

	// if we hit cancel, 0
	if (item == 8) return 0;

	if (use_password)
	{
		return password_dialog(DLOG_PASSWORD);
	}
	else
	{
		return key_dialog();
	}
}

int safety_checks(void)
{
	OSStatus err = noErr;

	// check for thread manager
	long int thread_manager_gestalt = 0;
	err = Gestalt(gestaltThreadMgrAttr, &thread_manager_gestalt);

	// bit one is prescence of thread manager
	if (err != noErr || (thread_manager_gestalt & (1 << gestaltThreadMgrPresent)) == 0)
	{
		StopAlert(ALRT_TM, nil);
		printf_i("Thread Manager not available!\r\n");
		return 0;
	}

	// check for Open Transport

	// for some reason, the docs say you shouldn't check for OT via the gestalt
	// in an application, and should just try to init, but checking seems more
	// user-friendly, so...

	long int open_transport_any_version = 0;
	long int open_transport_new_version = 0;
	err = Gestalt(gestaltOpenTpt, &open_transport_any_version);

	if (err != noErr)
	{
		printf_i("Failed to check for Open Transport!\r\n");
		return 0;
	}

	err = Gestalt(gestaltOpenTptVersions, &open_transport_new_version);

	if (err != noErr)
	{
		printf_i("Failed to check for Open Transport!\r\n");
		return 0;
	}

	if (open_transport_any_version == 0 && open_transport_new_version == 0)
	{
		printf_i("Open Transport required but not found!\r\n");
		StopAlert(ALRT_OT, nil);
		return 0;
	}

	if (open_transport_any_version != 0 && open_transport_new_version == 0)
	{
		printf_i("Early version of Open Transport detected!");
		printf_i("  Attempting to continue anyway.\r\n");
	}

	// check for CPU type and display warning if it's going to be too slow
	long int cpu_type = 0;
	int cpu_slow = 0;
	int cpu_bad = 0;

	err = Gestalt(gestaltNativeCPUtype, &cpu_type);

	if (err != noErr || cpu_type == 0)
	{
		// earlier than 7.5, need to use other gestalt
		err = Gestalt(gestaltProcessorType, &cpu_type);
		if (err != noErr || cpu_type == 0)
		{
			cpu_slow = 1;
			printf_i("Failed to detect CPU type, continuing anyway.\r\n");
		}
		else
		{
			if (cpu_type <= gestalt68010) cpu_bad = 1;
			if (cpu_type <= gestalt68020) cpu_slow = 1;
		}
	}
	else
	{
		if (cpu_type <= gestaltCPU68010) cpu_bad = 1;
		if (cpu_type <= gestaltCPU68020) cpu_slow = 1;
	}

	// if we don't have at least a 68020, stop
	if (cpu_bad)
	{
		StopAlert(ALRT_CPU_BAD, nil);
		return 0;
	}

	// if we don't have at least a 68030, warn
	if (cpu_slow)
	{
		CautionAlert(ALRT_CPU_SLOW, nil);
	}

	return 1;
}

int ssh_connect(int session_idx)
{
	struct session* s = &sessions[session_idx];
	struct window_context* wc = window_for_session(session_idx);
	OSStatus err = noErr;
	int ok = 1;

	ok = safety_checks();

	// reset the console if we have any crap from earlier
	if (wc != NULL && s->thread_state == DONE) reset_console(wc, session_idx);

	if (wc != NULL)
	{
		SetPort(wc->win);
		BeginUpdate(wc->win);
		draw_screen(wc, &(wc->win->portRect));
		EndUpdate(wc->win);
	}

	ok = intro_dialog();

	if (!ok) printf_s(session_idx, "Cancelled, not connecting.\r\n");

	if (ok)
	{
		if (InitOpenTransport() != noErr)
		{
			printf_s(session_idx, "Failed to initialize Open Transport.\r\n");
			ok = 0;
		}
	}

	if (ok)
	{
		s->recv_buffer = OTAllocMem(SSH_BUFFER_SIZE);
		s->send_buffer = OTAllocMem(SSH_BUFFER_SIZE);

		if (s->recv_buffer == NULL || s->send_buffer == NULL)
		{
			printf_s(session_idx, "Failed to allocate network data buffers.\r\n");
			ok = 0;
		}
	}

	// create the network read/print thread
	s->thread_command = WAIT;
	ThreadID read_thread_id = 0;

	if (ok)
	{
		err = NewThread(kCooperativeThread, read_thread, (void*)(intptr_t)session_idx, 100000, kCreateIfNeeded, NULL, &read_thread_id);

		if (err < 0)
		{
			printf_s(session_idx, "Failed to create network read thread.\r\n");
			ok = 0;
		}
	}

	// if we got the thread, tell it to begin operation
	if (ok)
	{
		s->thread_command = READ;
		s->type = SESSION_SSH;
		snprintf(s->tab_label, sizeof(s->tab_label), "%s", prefs.hostname+1);
		if (wc != NULL && wc->num_sessions > 1)
		{
			SetPort(wc->win);
			draw_tab_bar(wc);
		}
	}

	// allow disconnecting if we're ok
	if (ok)
	{
		void* menu = GetMenuHandle(MENU_FILE);
		DisableItem(menu, FMENU_CONNECT);
		EnableItem(menu, FMENU_DISCONNECT);
	}
	else if (read_thread_id == 0)
	{
		/* no thread was created — mark DONE so ssh_disconnect
		   won't wait 5s for a thread that doesn't exist */
		s->thread_state = DONE;
	}

	return ok;
}

void ssh_disconnect(int session_idx)
{
	struct session* s = &sessions[session_idx];
	struct window_context* wc = window_for_session(session_idx);

	// tell the read thread to finish, then let it run to actually do so
	s->thread_command = EXIT;

	if (s->thread_state != DONE)
	{
		// force-break any stuck OT calls
		if (s->endpoint != kOTInvalidEndpointRef)
		{
			OTCancelSynchronousCalls(s->endpoint, kOTCanceledErr);
		}

		// wait for the thread to finish, with a timeout (5 seconds)
		{
			long timeout = TickCount() + 300;
			while (s->thread_state != DONE && TickCount() < timeout)
			{
				if (wc != NULL && wc->win != NULL)
				{
					SetPort(wc->win);
					BeginUpdate(wc->win);
					draw_screen(wc, &(wc->win->portRect));
					EndUpdate(wc->win);
				}
				YieldToAnyThread();
			}
		}
	}

	/* only free buffers if thread actually finished — if it timed out
	   the thread may still be using them; leak rather than use-after-free */
	if (s->thread_state == DONE)
	{
		if (s->recv_buffer != NULL)
		{
			OTFreeMem(s->recv_buffer);
			s->recv_buffer = NULL;
		}
		if (s->send_buffer != NULL)
		{
			OTFreeMem(s->send_buffer);
			s->send_buffer = NULL;
		}
	}

	// update tab label
	snprintf(s->tab_label, sizeof(s->tab_label), "disconnected");

	// allow connecting if this is the active session
	if (wc != NULL && session_idx == wc->session_ids[wc->active_session_idx])
	{
		void* menu = GetMenuHandle(MENU_FILE);
		EnableItem(menu, FMENU_CONNECT);
		DisableItem(menu, FMENU_DISCONNECT);
	}

	if (wc != NULL && wc->num_sessions > 1)
	{
		SetPort(wc->win);
		draw_tab_bar(wc);
	}
}

int main(int argc, char** argv)
{
	// mark all session slots as safe to reuse (no thread running)
	{
		int i;
		for (i = 0; i < MAX_SESSIONS; i++)
			sessions[i].thread_state = DONE;
	}

	// expands the application heap to its maximum requested size
	// supposedly good for performance
	// also required before creating threads!
	MaxApplZone();

	// "Call the MoreMasters procedure several times at the beginning of your program"
	MoreMasters();
	MoreMasters();

	// set default preferences, then load from preferences file if possible
	init_prefs();
	load_prefs();
	apply_color_overrides();

	// general gui setup
	InitGraf(&qd.thePort);
	InitFonts();
	InitWindows();
	InitMenus();

	void* menu = GetNewMBar(MBAR_MAIN);
	SetMenuBar(menu);
	AppendResMenu(GetMenuHandle(MENU_APPLE), 'DRVR');

	// disable stuff in edit menu until we implement it
	menu = GetMenuHandle(MENU_EDIT);
	DisableItem(menu, 1);
	DisableItem(menu, 3);
	//DisableItem(menu, 4);
	DisableItem(menu, 5);
	DisableItem(menu, 6);
	DisableItem(menu, 7);
	DisableItem(menu, 9);

	// set up file menu
	menu = GetMenuHandle(MENU_FILE);
	EnableItem(menu, FMENU_CONNECT);
	DisableItem(menu, FMENU_DISCONNECT);
	EnableItem(menu, FMENU_NEW_WINDOW);
	EnableItem(menu, FMENU_NEW_LOCAL);
	EnableItem(menu, FMENU_NEW_SSH);
	DisableItem(menu, FMENU_CLOSE_TAB); // can't close with only 1 session

	DrawMenuBar();

	generate_key_mapping();

	// initialize font metrics (shared across all windows)
	init_font_metrics();

	// create the first window (which creates initial local shell session)
	new_window();

	// show startup logo in the first session (only at launch, not every new tab)
	{
		int sid = active_session_global();
		char* logo =
			"\033[2J\033[H"
			"  ____                      _____ _______   __\r\n"
			" / ___|  _____   _____ _ __|_   _|_   _\\ \\ / /\r\n"
			" \\___ \\ / _ \\ \\ / / _ \\ '_ \\ | |   | |  \\ V /\r\n"
			"  ___) |  __/\\ V /  __/ | | || |   | |   | |\r\n"
			" |____/ \\___| \\_/ \\___|_| |_||_|   |_|   |_|\r\n"
			"version " APP_VERSION ", based on ssheven by cy384\r\n"
#if defined(__ppc__)
			"running in PPC mode.\r\n"
#else
			"running in 68k mode.\r\n"
#endif
			"type 'help' for commands\r\n\r\n";
		vterm_input_write(sessions[sid].vterm, logo, strlen(logo));
		shell_prompt(sid);
	}

	struct window_context* wc = &ACTIVE_WIN;
	SetPort(wc->win);
	BeginUpdate(wc->win);
	draw_screen(wc, &(wc->win->portRect));
	EndUpdate(wc->win);

	event_loop();

	// cleanup all windows and sessions
	{
		int i;
		for (i = 0; i < MAX_WINDOWS; i++)
		{
			if (windows[i].in_use)
				close_window(i);
		}
	}

	if (prefs.pubkey_path != NULL && prefs.pubkey_path[0] != '\0') free(prefs.pubkey_path);
	if (prefs.privkey_path != NULL && prefs.privkey_path[0] != '\0') free(prefs.privkey_path);
}
