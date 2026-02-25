/*
 * SevenTTY (based on ssheven by cy384)
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#pragma once

#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <StandardFile.h>
#include <Folders.h>
#include <Quickdraw.h>
#include <Threads.h>

#define COLOR_FROM_THEME -1

#include <libssh2.h>

#include <vterm.h>
#include <vterm_keycodes.h>

#include "constants.r"

#define MAX_SESSIONS 8
#define MAX_WINDOWS 8
#define TAB_BAR_HEIGHT 20
#define SCROLLBACK_LINES 100
#define SCROLLBACK_COLS 80

/* compact scrollback cell: 4 bytes instead of ~36 for VTermScreenCell */
struct sb_cell {
	unsigned char ch;    /* character (Mac Roman) */
	unsigned char fg;    /* foreground color index */
	unsigned char bg;    /* background color index */
	unsigned char attrs; /* bit0=bold, bit1=reverse, bit2=underline, bit3=italic, bit4=symbol font, bit5=default fg, bit6=default bg */
};

enum MOUSE_MODE { CLICK_SEND, CLICK_SELECT };
enum SESSION_TYPE { SESSION_NONE, SESSION_SSH, SESSION_LOCAL, SESSION_TELNET };
enum THREAD_COMMAND { WAIT, READ, EXIT };
enum THREAD_STATE { UNINITIALIZED, OPEN, CLEANUP, DONE };

// per-session state (terminal + connection + thread)
struct session
{
	int in_use;
	enum SESSION_TYPE type;
	char tab_label[64]; // C string for tab display

	// terminal state
	VTerm* vterm;
	VTermScreen* vts;

	// cursor
	int cursor_x;
	int cursor_y;
	int cursor_state;
	long int last_cursor_blink;
	int cursor_visible;

	// selection
	int select_start_x;
	int select_start_y;
	int select_end_x;
	int select_end_y;
	int mouse_state;
	enum MOUSE_MODE mouse_mode;

	// SSH connection (SESSION_SSH only)
	LIBSSH2_CHANNEL* channel;
	LIBSSH2_SESSION* ssh_session;
	EndpointRef endpoint;
	char* recv_buffer;
	char* send_buffer;

	// telnet/nc connection (SESSION_TELNET/SESSION_NETCAT only)
	char telnet_host[256];
	unsigned short telnet_port;
	unsigned char telnet_state;       /* IAC parser state */
	unsigned char telnet_sb_buf[64];  /* subnegotiation buffer */
	int telnet_sb_len;

	// thread state
	enum THREAD_COMMAND thread_command;
	enum THREAD_STATE thread_state;
	ThreadID thread_id;

	// local shell state (SESSION_LOCAL only)
	short shell_vRefNum;
	long shell_dirID;
	char shell_line[256]; // line buffer
	int shell_line_len;
	int shell_cursor_pos; // cursor position within line buffer

	// command history
	#define SHELL_HISTORY_SIZE 32
	char shell_history[SHELL_HISTORY_SIZE][256];
	int shell_history_count; // total entries stored
	int shell_history_pos;   // current browse position (-1 = editing new line)
	char shell_saved_line[256]; // saved line when browsing history
	int shell_saved_len;

	// scrollback buffer (ring buffer of compact rows)
	struct sb_cell scrollback[SCROLLBACK_LINES][SCROLLBACK_COLS];
	int sb_head;    // next write position in ring
	int sb_count;   // total lines stored (max SCROLLBACK_LINES)
	int scroll_offset; // how many lines scrolled back (0 = live)

	// which window owns this session
	int window_id;
};

extern struct session sessions[MAX_SESSIONS];

// per-window state
struct window_context
{
	int in_use;
	WindowPtr win;
	int size_x;                    // terminal cols
	int size_y;                    // terminal rows
	int session_ids[MAX_SESSIONS]; // indices into sessions[]
	int num_sessions;
	int active_session_idx;        // index into session_ids[] array
	int needs_redraw;              // dirty flag: set when content changes
};

extern struct window_context windows[MAX_WINDOWS];
extern int num_windows;
extern int active_window;
extern int exit_requested;

// global font metrics (same across all windows)
struct console_metrics
{
	int cell_height;
	int cell_width;
};

extern struct console_metrics con;

// convenience macros
#define ACTIVE_WIN windows[active_window]
#define ACTIVE_S sessions[ACTIVE_WIN.session_ids[ACTIVE_WIN.active_session_idx]]

// window helpers
struct window_context* find_window_context(WindowPtr w);
struct window_context* window_for_session(int session_idx);
int active_session_global(void);  // returns sessions[] index of active session in active window
void add_session_to_window(struct window_context* wc, int session_idx);
void remove_session_from_window(struct window_context* wc, int session_idx);

struct preferences
{
	int major_version;
	int minor_version;

	int loaded_from_file;

	// pascal strings
	char hostname[512]; // of the form: "hostname:portnumber", size is first only
	char username[256];
	char password[256];
	char port[256];

	// malloc'd c strings
	char* pubkey_path;
	char* privkey_path;

	const char* terminal_string;

	enum { USE_KEY, USE_PASSWORD } auth_type;

	enum { FASTEST, COLOR } display_mode;
	int fg_color;
	int bg_color;

	int font_size;

	RGBColor palette[16];
	RGBColor theme_fg;
	RGBColor theme_bg;
	RGBColor theme_cursor;
	RGBColor orig_theme_bg;
	RGBColor orig_theme_fg;
	int theme_loaded;
	int prompt_color; /* ANSI color index 0-15, default 4 (blue) */
	int bold_is_bright; /* 1 = bold promotes color 0-7 to 8-15 (default) */
	char theme_name[64];
};

extern struct preferences prefs;

extern char key_to_vterm[256];

int save_prefs(void);
int load_theme_file(FSSpec* spec);
void set_window_title(WindowPtr w, const char* c_name, size_t length);
void set_terminal_string(void);

OSErr FSpPathFromLocation(FSSpec* spec, int* length, Handle* fullPath);

pascal void ButtonFrameProc(DialogRef dlg, DialogItemIndex itemNo);

// session management
int new_session(struct window_context* wc, enum SESSION_TYPE type);
void close_session(int idx);
void switch_session(struct window_context* wc, int idx);
void init_session(struct session* s);
int session_reap_thread(int session_idx, int force_stop);

// window management
int new_window(void);
void close_window(int wid);

// connection (operates on a session)
int ssh_connect(int session_idx);
void ssh_disconnect(int session_idx);
int telnet_connect(int session_idx);
void telnet_disconnect(int session_idx);
int nc_inline_connect(int session_idx);
void nc_inline_disconnect(int session_idx);
