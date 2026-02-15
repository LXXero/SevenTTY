/*
 * ssheven
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#include "ssheven.h"
#include "ssheven-console.h"
#include "ssheven-net.h"
#include "ssheven-shell.h"
#include "ssheven-debug.h"

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

#include <stdio.h>

// forward declarations
int qd_color_to_menu_item(int qd_color);
int font_size_to_menu_item(int font_size);

// sinful globals
struct ssheven_console con = { 0, 0 };
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
	 * "xterm-16color" classic 16 ANSI colors only
	 */
	switch (prefs.display_mode)
	{
		case FASTEST:
			prefs.terminal_string = "xterm-mono";
			break;
		case COLOR:
			prefs.terminal_string = "xterm-16color";
			break;
		default:
			prefs.terminal_string = SSHEVEN_DEFAULT_TERM_STRING;
			break;
	}
}

int save_prefs(void)
{
	int ok = 1;
	short foundVRefNum = 0;
	long foundDirID = 0;
	FSSpec pref_file;
	short prefRefNum = 0;

	OSType pref_type = 'SH7p';
	OSType creator_type = 'SSH7';

	// find the preferences folder on the system disk, create folder if needed
	OSErr e = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &foundVRefNum, &foundDirID);
	if (e != noErr) ok = 0;

	// make an FSSpec for the new file we want to make
	if (ok)
	{
		e = FSMakeFSSpec(foundVRefNum, foundDirID, PREFERENCES_FILENAME, &pref_file);

		// if the file exists, delete it
		if (e == noErr) FSpDelete(&pref_file);

		// and then make it
		e = FSpCreate(&pref_file, creator_type, pref_type, smSystemScript);
		if (e != noErr) ok = 0;
	}

	// open the file
	if (ok)
	{
		e = FSpOpenDF(&pref_file, fsRdWrPerm, &prefRefNum);
		if (e != noErr) ok = 0;
	}

	// write prefs to the file
	if (ok)
	{
		// TODO: choose buffer size more effectively
		size_t write_length = 8192;

		char* output_buffer = malloc(write_length);
		memset(output_buffer, 0, write_length);

		long int i = snprintf(output_buffer, write_length, "%d\n%d\n", prefs.major_version, prefs.minor_version);
		i += snprintf(output_buffer+i, write_length-i, "%d\n%d\n%d\n%d\n%d\n", (int)prefs.auth_type, (int)prefs.display_mode, (int)prefs.fg_color, (int)prefs.bg_color, (int)prefs.font_size);

		i += snprintf(output_buffer+i, write_length-i, "%s\n", prefs.hostname+1);
		i += snprintf(output_buffer+i, write_length-i, "%s\n", prefs.username+1);
		i += snprintf(output_buffer+i, write_length-i, "%s\n", prefs.port+1);

		if (prefs.privkey_path && prefs.privkey_path[0] != '\0')
		{
			i += snprintf(output_buffer+i, write_length-i, "%s\n%s\n", prefs.privkey_path, prefs.pubkey_path);
		}
		else
		{
			i += snprintf(output_buffer+i, write_length-i, "\n\n");
		}

		// tell it to write all bytes
		long int bytes = i;
		e = FSWrite(prefRefNum, &bytes, output_buffer);
		// FSWrite sets bytes to the actual number of bytes written

		if (e != noErr || (bytes != i)) ok = 0;
	}

	// close the file
	if (prefRefNum != 0)
	{
		e = FSClose(prefRefNum);
		if (e != noErr) ok = 0;
	}

	return ok;
}

// check if the main device is black and white
int detect_color_screen(void)
{
	return TestDeviceAttribute(GetMainDevice(), gdDevType);
}

void init_prefs(void)
{
	// initialize everything to a safe default
	prefs.major_version = SSHEVEN_VERSION_MAJOR;
	prefs.minor_version = SSHEVEN_VERSION_MINOR;

	memset(&(prefs.hostname), 0, 512);
	memset(&(prefs.username), 0, 256);
	memset(&(prefs.password), 0, 256);
	memset(&(prefs.port), 0, 256);

	// default port: 22
	prefs.port[0] = 2;
	prefs.port[1] = '2';
	prefs.port[2] = '2';

	prefs.pubkey_path = "";
	prefs.privkey_path = "";
	prefs.terminal_string = SSHEVEN_DEFAULT_TERM_STRING;
	prefs.auth_type = USE_PASSWORD;
	prefs.display_mode = detect_color_screen() ? COLOR : FASTEST;
	prefs.fg_color = blackColor;
	prefs.bg_color = whiteColor;
	prefs.font_size = 9;

	prefs.loaded_from_file = 0;
}

void load_prefs(void)
{
	// now try to load preferences from the file
	short foundVRefNum = 0;
	long foundDirID = 0;
	FSSpec pref_file;
	short prefRefNum = 0;

	// find the preferences folder on the system disk
	OSErr e = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &foundVRefNum, &foundDirID);
	if (e != noErr) return;

	// make an FSSpec for the preferences file location and check if it exists
	// TODO: if I just put PREFERENCES_FILENAME it doesn't work, wtf
	e = FSMakeFSSpec(foundVRefNum, foundDirID, PREFERENCES_FILENAME, &pref_file);

	if (e == fnfErr) // file not found, nothing to load
	{
		return;
	}
	else if (e != noErr) return;

	e = FSpOpenDF(&pref_file, fsCurPerm, &prefRefNum);
	if (e != noErr) return;

	// actually read and parse the file
	long int buffer_size = 8192;
	char* buffer = NULL;
	buffer = malloc(buffer_size);
	prefs.privkey_path = malloc(2048);
	prefs.pubkey_path = malloc(2048);
	prefs.pubkey_path[0] = '\0';
	prefs.privkey_path[0] = '\0';

	prefs.hostname[0] = 0;
	prefs.username[0] = 0;
	prefs.port[0] = 0;

	e = FSRead(prefRefNum, &buffer_size, buffer);
	e = FSClose(prefRefNum);

	// check the version (first two numbers)
	int items_got = sscanf(buffer, "%d\n%d", &prefs.major_version, &prefs.minor_version);
	if (items_got != 2)
	{
		free(buffer);
		return;
	}

	// only load a prefs file if the saved version number matches ours
	if ((prefs.major_version == SSHEVEN_VERSION_MAJOR) && (prefs.minor_version == SSHEVEN_VERSION_MINOR))
	{
		prefs.loaded_from_file = 1;
		items_got = sscanf(buffer, "%d\n%d\n%d\n%d\n%d\n%d\n%d\n%255[^\n]\n%255[^\n]\n%255[^\n]\n%[^\n]\n%[^\n]", &prefs.major_version, &prefs.minor_version, (int*)&prefs.auth_type, (int*)&prefs.display_mode, &prefs.fg_color, &prefs.bg_color, &prefs.font_size, prefs.hostname+1, prefs.username+1, prefs.port+1, prefs.privkey_path, prefs.pubkey_path);

		// need at least the core fields (version, auth, display, colors, font, host, user, port)
		if (items_got < 10)
		{
			prefs.loaded_from_file = 0;
			init_prefs();
			free(buffer);
			return;
		}

		// bounds-check loaded values
		if (prefs.auth_type != USE_KEY && prefs.auth_type != USE_PASSWORD)
			prefs.auth_type = USE_PASSWORD;

		if (prefs.display_mode != FASTEST && prefs.display_mode != COLOR)
			prefs.display_mode = detect_color_screen() ? COLOR : FASTEST;

		if (qd_color_to_menu_item(prefs.fg_color) == 1 && prefs.fg_color != blackColor)
			prefs.fg_color = blackColor;

		if (qd_color_to_menu_item(prefs.bg_color) == 1 && prefs.bg_color != blackColor)
			prefs.bg_color = whiteColor;

		if (font_size_to_menu_item(prefs.font_size) == 1 && prefs.font_size != 9)
			prefs.font_size = 9;

		// add the size for the pascal strings
		// hostname buffer contains "host:port" as C string; hostname[0] = host length only
		{
			char* colon = strchr(prefs.hostname + 1, ':');
			if (colon)
				prefs.hostname[0] = (unsigned char)(colon - (prefs.hostname + 1));
			else
				prefs.hostname[0] = (unsigned char)strlen(prefs.hostname + 1);
		}
		prefs.username[0] = (unsigned char)strlen(prefs.username+1);
		prefs.port[0] = (unsigned char)strlen(prefs.port+1);

		set_terminal_string();
	}
	else
	{
		// version mismatch: free malloc'd paths before restoring defaults
		// (init_prefs sets these to "" literals, leaking the buffers)
		if (prefs.privkey_path) free(prefs.privkey_path);
		if (prefs.pubkey_path) free(prefs.pubkey_path);
		prefs.privkey_path = NULL;
		prefs.pubkey_path = NULL;
		init_prefs();
	}

	if (buffer) free(buffer);
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

void ssh_paste(void)
{
	int sid = active_session_global();
	if (sessions[sid].type != SESSION_SSH) return;

	// GetScrap requires a handle, not a raw buffer
	// it will increase the size of the handle if needed
	Handle buf = NewHandle(256);
	int r = GetScrap(buf, 'TEXT', 0);

	if (r > 0)
	{
		ssh_write_s(sid, *buf, r);
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
}

int qd_color_to_menu_item(int qd_color)
{
	switch (qd_color)
	{
		case blackColor: return 1;
		case redColor: return 2;
		case greenColor: return 3;
		case yellowColor: return 4;
		case blueColor: return 5;
		case magentaColor: return 6;
		case cyanColor: return 7;
		case whiteColor: return 8;
		default: return 1;
	}
}

int menu_item_to_qd_color(int menu_item)
{
	switch (menu_item)
	{
		case 1: return blackColor;
		case 2: return redColor;
		case 3: return greenColor;
		case 4: return yellowColor;
		case 5: return blueColor;
		case 6: return magentaColor;
		case 7: return cyanColor;
		case 8: return whiteColor;
		default: return 1;
	}
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

	// let the modalmanager do everything
	// stop on ok or cancel
	short item;
	do {
		ModalDialog(NULL, &item);
	} while(item != 1 && item != 11);

	// save if OK'd
	if (item == 1)
	{
		// read menu values into prefs
		prefs.display_mode = GetControlValue(term_type_menu) - 1;

		prefs.bg_color = menu_item_to_qd_color(GetControlValue(bg_color_menu));
		prefs.fg_color = menu_item_to_qd_color(GetControlValue(fg_color_menu));
		int new_font_size = menu_item_to_font_size(GetControlValue(font_size_menu));

		// resize window if font size changed
		if (new_font_size != prefs.font_size)
		{
			prefs.font_size = new_font_size;
			font_size_change(wc);
		}

		set_terminal_string();
		save_prefs();
		update_console_colors(wc);
	}

	// clean it up
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
		if (!sessions[i].in_use)
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

	switch_session(wc, idx);

	// enable Close Tab now that we have multiple sessions
	if (wc->num_sessions > 1)
	{
		void* menu = GetMenuHandle(MENU_FILE);
		EnableItem(menu, FMENU_CLOSE_TAB);
	}

	// if this is an SSH session, start connecting
	if (type == SESSION_SSH)
	{
		if (ssh_connect(idx) == 0) ssh_disconnect(idx);
	}
	else if (type == SESSION_LOCAL)
	{
		shell_init(idx);
	}

	// redraw since tab bar may have appeared
	InvalRect(&(wc->win->portRect));

	return idx;
}

void close_session(int idx)
{
	if (idx < 0 || idx >= MAX_SESSIONS) return;
	if (!sessions[idx].in_use) return;

	struct window_context* wc = window_for_session(idx);
	if (wc == NULL) return;
	if (wc->num_sessions <= 1) return; // don't close the last session in a window

	// disconnect if SSH and still connected
	if (sessions[idx].type == SESSION_SSH &&
		sessions[idx].thread_state != DONE &&
		sessions[idx].thread_state != UNINITIALIZED)
	{
		ssh_disconnect(idx);
	}

	// null out vterm callbacks before freeing to prevent damage
	// callbacks from accessing the window during teardown
	if (sessions[idx].vterm != NULL)
	{
		VTermScreen* vts = vterm_obtain_screen(sessions[idx].vterm);
		vterm_screen_set_callbacks(vts, NULL, NULL);
	}

	// only free vterm if the thread is actually done (or was never started)
	if (sessions[idx].vterm != NULL &&
		(sessions[idx].thread_state == DONE ||
		 sessions[idx].thread_state == UNINITIALIZED ||
		 sessions[idx].type == SESSION_LOCAL))
	{
		vterm_free(sessions[idx].vterm);
		sessions[idx].vterm = NULL;
	}

	sessions[idx].in_use = 0;
	remove_session_from_window(wc, idx);

	// shrink window if tab bar just disappeared (2 -> 1 sessions)
	if (wc->num_sessions == 1)
		adjust_window_for_tabs(wc, 0);

	// update menu state
	void* menu = GetMenuHandle(MENU_FILE);
	int active_sid = wc->session_ids[wc->active_session_idx];
	if (sessions[active_sid].type == SESSION_SSH)
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

	// disable Close Tab if only 1 session left
	if (wc->num_sessions <= 1)
		DisableItem(menu, FMENU_CLOSE_TAB);

	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));
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

	// update menu enable/disable based on new active session
	void* menu = GetMenuHandle(MENU_FILE);
	if (sessions[idx].type == SESSION_SSH)
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

	SetPort(wc->win);
	InvalRect(&(wc->win->portRect));
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

	ConstStr255Param title = "\pSevenTTY " SSHEVEN_VERSION;

	WindowPtr win = NewWindow(NULL, &initial_window_bounds, title, true, documentProc, (WindowPtr)-1, true, 0);

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

		// disconnect SSH if needed
		if (sessions[sid].type == SESSION_SSH &&
			sessions[sid].thread_state != DONE &&
			sessions[sid].thread_state != UNINITIALIZED)
		{
			ssh_disconnect(sid);
		}

		// null out vterm callbacks
		if (sessions[sid].vterm != NULL)
		{
			VTermScreen* vts = vterm_obtain_screen(sessions[sid].vterm);
			vterm_screen_set_callbacks(vts, NULL, NULL);
		}

		// free vterm
		if (sessions[sid].vterm != NULL &&
			(sessions[sid].thread_state == DONE ||
			 sessions[sid].thread_state == UNINITIALIZED ||
			 sessions[sid].type == SESSION_LOCAL))
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
			if (item == FMENU_DISCONNECT) ssh_disconnect(active_session_global());
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
						return 1; // last window, last tab -> quit
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
		// for local shell sessions, keypresses go to shell handler
		if (sessions[sid].type == SESSION_LOCAL)
		{
			shell_input(sid, c, event->modifiers);
			return 0;
		}

		// SSH session keypress handling
		if (sessions[sid].type != SESSION_SSH || sessions[sid].thread_state != OPEN)
			return 0;

		// get the unmodified version of the keypress
		uint8_t unmodified_key = keycode_to_ascii[(event->message & keyCodeMask)>>8];

		// if we have a control code for this key
		if (event->modifiers & controlKey && ascii_to_control_code[unmodified_key] != 255)
		{
			sessions[sid].send_buffer[0] = ascii_to_control_code[unmodified_key];
			ssh_write_s(sid, sessions[sid].send_buffer, 1);
		}
		else
		{
			// if we have alt and the character would be printable ascii, use it
			if (event->modifiers & optionKey && c >= 32 && c <= 126)
			{
				sessions[sid].send_buffer[0] = c;
				ssh_write_s(sid, sessions[sid].send_buffer, 1);
			}
			else
			{
				// otherwise manually send an escape if we have alt held
				if (event->modifiers & optionKey)
				{
					sessions[sid].send_buffer[0] = '\e';
					ssh_write_s(sid, sessions[sid].send_buffer, 1);
				}

				if (key_to_vterm[c] != VTERM_KEY_NONE)
				{
					// doesn't seem like vterm does modifiers properly, so don't bother
					vterm_keyboard_key(sessions[sid].vterm, key_to_vterm[c], VTERM_MOD_NONE);
				}
				else
				{
					sessions[sid].send_buffer[0] = event->modifiers & optionKey ? unmodified_key : c;
					ssh_write_s(sid, sessions[sid].send_buffer, 1);
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

				BeginUpdate(windows[i].win);
				draw_screen(&windows[i], &(windows[i].win->portRect));
				EndUpdate(windows[i].win);
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
		FSpPathFromLocation(&pubkey.sfFile, &path_length, &full_path);
		prefs.pubkey_path = malloc(path_length+1);
		strncpy(prefs.pubkey_path, (char*)(*full_path), path_length+1);
		DisposeHandle(full_path);

		// if the user hit cancel, 0
		if (!pubkey.sfGood) return 0;
	}

	path_length = 0;
	full_path = NULL;

	// if we don't have a saved privkey path, ask for one
	if (prefs.privkey_path == NULL || prefs.privkey_path[0] == '\0')
	{
		NoteAlert(ALRT_PRIVKEY, nil);
		StandardFileReply privkey;
		StandardGetFile(NULL, 0, NULL, &privkey);
		FSpPathFromLocation(&privkey.sfFile, &path_length, &full_path);
		prefs.privkey_path = malloc(path_length+1);
		strncpy(prefs.privkey_path, (char*)(*full_path), path_length+1);
		DisposeHandle(full_path);

		// if the user hit cancel, 0
		if (!privkey.sfGood) return 0;
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
		s->recv_buffer = OTAllocMem(SSHEVEN_BUFFER_SIZE);
		s->send_buffer = OTAllocMem(SSHEVEN_BUFFER_SIZE);

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

	return ok;
}

void ssh_disconnect(int session_idx)
{
	struct session* s = &sessions[session_idx];
	struct window_context* wc = window_for_session(session_idx);

	// tell the read thread to finish, then let it run to actually do so
	s->thread_command = EXIT;

	if (s->thread_state != UNINITIALIZED && s->thread_state != DONE)
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
				if (wc != NULL)
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

	// general gui setup
	InitGraf(&qd.thePort);
	InitFonts();
	InitWindows();
	InitMenus();

	void* menu = GetNewMBar(MBAR_SSHEVEN);
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
			"version " SSHEVEN_VERSION ", based on ssheven by cy384\r\n"
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
