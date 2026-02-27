/*
 * SevenTTY - local shell command interpreter
 *
 * Maps Unix-like commands to classic Mac OS Toolbox equivalents.
 * Runs inside a vterm session tab with no network connection.
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#include "app.h"
#include "shell.h"
#include "console.h"
#include "debug.h"
#include "net.h"
#include "telnet.h"

#include <Files.h>
#include <Folders.h>
#include <Devices.h>
#include <Gestalt.h>
#include <DateTimeUtils.h>
#include <MacMemory.h>
#include <Processes.h>
#include <TextUtils.h>
#include <Threads.h>

#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* timeout notifier for blocking OT calls in main thread (ping, host) */
#define OT_TIMEOUT_TICKS  600  /* 10 seconds at 60 ticks/sec */

static unsigned long ot_timeout_deadline = 0;
static ProviderRef ot_timeout_provider = nil;

pascal void shell_ot_timeout_notifier(void* context, OTEventCode event,
                                      OTResult result, void* cookie)
{
	(void)context;
	(void)result;
	(void)cookie;
	if (event == kOTSyncIdleEvent)
	{
		YieldToAnyThread();
		if (ot_timeout_deadline > 0 &&
		    TickCount() > ot_timeout_deadline &&
		    ot_timeout_provider != nil)
		{
			OTCancelSynchronousCalls(ot_timeout_provider, kOTCanceledErr);
		}
	}
}

/* max arguments for a command */
#define MAX_ARGS 32

/* forward declarations */
void shell_prompt(int idx);
static void shell_execute(int idx, char* line);
static void shell_complete(int idx);
static void ls_show_file(int idx, FSSpec* tspec, int long_fmt);
static void ls_show_dir(int idx, short vRef, long dID, int long_fmt, int show_all);

/* ------------------------------------------------------------------ */
/* utility: write a C string into the session's vterm                 */
/* ------------------------------------------------------------------ */

static void vt_write(int idx, const char* s)
{
	if (sessions[idx].vterm)
		vterm_input_write(sessions[idx].vterm, s, strlen(s));
}

static void copy_cstr_trunc(char* dst, size_t dst_size, const char* src)
{
	size_t n;
	if (dst_size == 0) return;
	if (src == NULL) { dst[0] = '\0'; return; }
	n = strlen(src);
	if (n >= dst_size) n = dst_size - 1;
	if (n > 0) memcpy(dst, src, n);
	dst[n] = '\0';
}

static void vt_write_n(int idx, const char* s, size_t n)
{
	if (sessions[idx].vterm)
		vterm_input_write(sessions[idx].vterm, s, n);
}

static void vt_char(int idx, char c)
{
	if (sessions[idx].vterm)
		vterm_input_write(sessions[idx].vterm, &c, 1);
}

/* simple integer-to-string for printf_s %d (which exists but let's
   have our own for formatting with commas etc.) */
static void vt_print_long(int idx, long val)
{
	char buf[32];
	int neg = 0;
	int i = 30;

	buf[31] = '\0';

	if (val == 0) { vt_write(idx, "0"); return; }
	if (val < 0) { neg = 1; val = -val; }

	while (val > 0 && i > 0)
	{
		buf[i--] = '0' + (val % 10);
		val /= 10;
	}
	if (neg) buf[i--] = '-';

	vt_write(idx, buf + i + 1);
}

/* ------------------------------------------------------------------ */
/* path helpers                                                       */
/* ------------------------------------------------------------------ */

/* get the name of the current directory as a C string */
static void get_dir_name(short vRefNum, long dirID, char* out, int maxlen)
{
	CInfoPBRec pb;
	Str255 name;

	memset(&pb, 0, sizeof(pb));
	name[0] = 0;
	pb.dirInfo.ioNamePtr = name;
	pb.dirInfo.ioVRefNum = vRefNum;
	pb.dirInfo.ioDrDirID = dirID;
	pb.dirInfo.ioFDirIndex = -1;

	if (PBGetCatInfoSync(&pb) == noErr)
	{
		int len = name[0];
		if (len > maxlen - 1) len = maxlen - 1;
		memcpy(out, name + 1, len);
		out[len] = '\0';
	}
	else
	{
		strncpy(out, "???", maxlen);
	}
}

/* build a full HFS path for the current directory */
static void get_full_path(short vRefNum, long dirID, char* out, int maxlen)
{
	/* walk parent chain and build path */
	char parts[32][64];
	int depth = 0;
	CInfoPBRec pb;
	Str255 name;
	long curDir = dirID;
	OSErr err;

	while (depth < 32)
	{
		memset(&pb, 0, sizeof(pb));
		name[0] = 0;
		pb.dirInfo.ioNamePtr = name;
		pb.dirInfo.ioVRefNum = vRefNum;
		pb.dirInfo.ioDrDirID = curDir;
		pb.dirInfo.ioFDirIndex = -1;

		err = PBGetCatInfoSync(&pb);
		if (err != noErr) break;

		{
			int len = name[0];
			if (len > 63) len = 63;
			memcpy(parts[depth], name + 1, len);
			parts[depth][len] = '\0';
		}
		depth++;

		if (pb.dirInfo.ioDrDirID == fsRtDirID) break;
		curDir = pb.dirInfo.ioDrParID;
	}

	/* assemble in reverse */
	out[0] = '\0';
	{
		int i;
		for (i = depth - 1; i >= 0; i--)
		{
			if (strlen(out) + strlen(parts[i]) + 2 >= (size_t)maxlen) break;
			strcat(out, parts[i]);
			strcat(out, ":");
		}
	}
}

/* resolve a user-typed path (may use / as separator) to vRefNum+dirID+name.
   returns noErr on success, populates spec. */
static OSErr resolve_path(int idx, const char* path, FSSpec* spec)
{
	struct session* s = &sessions[idx];
	Str255 pname;
	char hfs_path[512];
	int i, j;

	if (path == NULL || path[0] == '\0')
		return fnfErr;

	/* strip leading "./" — HFS has no "." for current directory */
	while (path[0] == '.' && path[1] == '/')
		path += 2;

	/* bare "." means current directory */
	if (path[0] == '.' && path[1] == '\0')
		return FSMakeFSSpec(s->shell_vRefNum, s->shell_dirID, "\p", spec);

	/* convert / to : for HFS, handle leading / as volume root */
	j = 0;
	for (i = 0; path[i] != '\0' && j < 510; i++)
	{
		if (path[i] == '/')
			hfs_path[j++] = ':';
		else
			hfs_path[j++] = path[i];
	}
	hfs_path[j] = '\0';

	/* handle ".." -> parent */
	if (strcmp(hfs_path, "..") == 0)
		strcpy(hfs_path, "::");

	/* strip trailing : unless the path IS just ":" or "::" */
	{
		int plen = strlen(hfs_path);
		if (plen > 1 && hfs_path[plen - 1] == ':' && strcmp(hfs_path, "::") != 0)
			hfs_path[--plen] = '\0';
	}

	/* if the path contains a colon but doesn't start with one, it would be
	   treated by FSMakeFSSpec as a FULL path (volume:folder:file) instead of
	   relative.  prepend ':' to make it a relative partial path. */
	if (hfs_path[0] != ':' && strchr(hfs_path, ':') != NULL)
	{
		int plen = strlen(hfs_path);
		if (plen < 510)
		{
			memmove(hfs_path + 1, hfs_path, plen + 1);
			hfs_path[0] = ':';
		}
	}

	/* convert to pascal string */
	{
		int len = strlen(hfs_path);
		if (len > 255) len = 255;
		pname[0] = len;
		memcpy(pname + 1, hfs_path, len);
	}

	return FSMakeFSSpec(s->shell_vRefNum, s->shell_dirID, pname, spec);
}

/* check if an FSSpec points to a directory */
static int is_directory(FSSpec* spec)
{
	CInfoPBRec pb;
	memset(&pb, 0, sizeof(pb));
	pb.hFileInfo.ioNamePtr = spec->name;
	pb.hFileInfo.ioVRefNum = spec->vRefNum;
	pb.hFileInfo.ioDirID = spec->parID;
	pb.hFileInfo.ioFDirIndex = 0;

	if (PBGetCatInfoSync(&pb) != noErr) return 0;
	return (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
}

/* get the dirID for a directory FSSpec */
static long get_dir_id(FSSpec* spec)
{
	CInfoPBRec pb;
	memset(&pb, 0, sizeof(pb));
	pb.dirInfo.ioNamePtr = spec->name;
	pb.dirInfo.ioVRefNum = spec->vRefNum;
	pb.dirInfo.ioDrDirID = spec->parID;
	pb.dirInfo.ioFDirIndex = 0;

	if (PBGetCatInfoSync(&pb) != noErr) return 0;
	return pb.dirInfo.ioDrDirID;
}

/* ------------------------------------------------------------------ */
/* argument parsing                                                   */
/* ------------------------------------------------------------------ */

/* parse a command line into argc/argv, handling quoting and backslash escapes.
   modifies line in-place to collapse escape sequences. */
static int parse_args(char* line, char* argv[], int max_args)
{
	int argc = 0;
	char* p = line;

	while (*p && argc < max_args)
	{
		/* skip whitespace */
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0') break;

		if (*p == '"')
		{
			/* quoted arg */
			p++;
			argv[argc++] = p;
			while (*p && *p != '"') p++;
			if (*p == '"') *p++ = '\0';
		}
		else if (*p == '\'')
		{
			p++;
			argv[argc++] = p;
			while (*p && *p != '\'') p++;
			if (*p == '\'') *p++ = '\0';
		}
		else
		{
			/* unquoted arg: handle backslash-escaped spaces */
			char* dst = p;
			argv[argc++] = dst;
			while (*p)
			{
				if (*p == '\\' && *(p+1) == ' ')
				{
					/* escaped space: collapse to literal space */
					*dst++ = ' ';
					p += 2;
				}
				else if (*p == ' ' || *p == '\t')
				{
					break; /* unescaped whitespace = end of arg */
				}
				else
				{
					*dst++ = *p++;
				}
			}
			if (*p) { *dst = '\0'; p++; }
			else { *dst = '\0'; }
		}
	}

	return argc;
}

/* ------------------------------------------------------------------ */
/* date formatting helper                                             */
/* ------------------------------------------------------------------ */

static void format_date(unsigned long secs, char* out, int maxlen)
{
	DateTimeRec dt;
	static const char* months[] = {
		"Jan","Feb","Mar","Apr","May","Jun",
		"Jul","Aug","Sep","Oct","Nov","Dec"
	};

	SecondsToDate(secs, &dt);

	if (dt.month < 1 || dt.month > 12) dt.month = 1;

	snprintf(out, maxlen, "%s %2d %02d:%02d %d",
		months[dt.month - 1], dt.day,
		dt.hour, dt.minute, dt.year);
}

/* ------------------------------------------------------------------ */
/* type/creator helper                                                */
/* ------------------------------------------------------------------ */

static void ostype_to_str(OSType t, char* out)
{
	out[0] = (t >> 24) & 0xFF;
	out[1] = (t >> 16) & 0xFF;
	out[2] = (t >> 8)  & 0xFF;
	out[3] = t & 0xFF;
	out[4] = '\0';

	/* replace non-printable with '?' */
	{
		int i;
		for (i = 0; i < 4; i++)
			if (out[i] < 32 || out[i] > 126) out[i] = '?';
	}
}

static OSType str_to_ostype(const char* s)
{
	char buf[4] = { ' ', ' ', ' ', ' ' };
	int i;
	for (i = 0; i < 4 && s[i]; i++) buf[i] = s[i];
	return ((OSType)buf[0] << 24) | ((OSType)buf[1] << 16) |
	       ((OSType)buf[2] << 8) | (OSType)buf[3];
}

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */

/* return ANSI color escape for a file entry */
static const char* ls_color(int is_dir, int is_locked, int is_invis, OSType ftype)
{
	if (is_dir)                return "\033[1;34m"; /* bold blue */
	if (is_invis)              return "\033[2m";     /* dim */
	if (is_locked)             return "\033[1;31m"; /* bold red */
	if (ftype == 0x4150504C)   return "\033[1;32m"; /* APPL: bold green */
	return "";
}

/* simple glob match: supports * and ? only, case-insensitive */
static int glob_match(const char* pattern, const char* str)
{
	while (*pattern)
	{
		if (*pattern == '*')
		{
			pattern++;
			if (!*pattern) return 1; /* trailing * matches everything */
			while (*str)
			{
				if (glob_match(pattern, str)) return 1;
				str++;
			}
			return 0;
		}
		else if (*pattern == '?')
		{
			if (!*str) return 0;
			pattern++;
			str++;
		}
		else
		{
			if (tolower((unsigned char)*pattern) != tolower((unsigned char)*str))
				return 0;
			pattern++;
			str++;
		}
	}
	return *str == '\0';
}

/* ls with glob pattern: enumerate directory, filter by pattern */
/* resolve glob base directory + pattern; returns 0 on success */
static int glob_resolve_dir(int idx, const char* target,
	short* out_vRef, long* out_dID, const char** out_pattern)
{
	struct session* s = &sessions[idx];
	const char* last_sep = strrchr(target, ':');
	if (last_sep)
	{
		char dir_path[256];
		int dlen = (int)(last_sep - target);
		if (dlen > 255) dlen = 255;
		memcpy(dir_path, target, dlen);
		dir_path[dlen] = '\0';

		FSSpec dir_spec;
		OSErr e = resolve_path(idx, dir_path, &dir_spec);
		if (e != noErr || !is_directory(&dir_spec)) return -1;
		*out_vRef = dir_spec.vRefNum;
		*out_dID = get_dir_id(&dir_spec);
		*out_pattern = last_sep + 1;
	}
	else
	{
		*out_vRef = s->shell_vRefNum;
		*out_dID = s->shell_dirID;
		*out_pattern = target;
	}
	return 0;
}

/* ls glob: mode 0 = files only, mode 1 = expand dirs with headers */
static void cmd_ls_glob(int idx, const char* target, int long_fmt,
	int show_all, int mode)
{
	short vRef;
	long dID;
	const char* pattern;

	if (glob_resolve_dir(idx, target, &vRef, &dID, &pattern) != 0)
	{
		if (mode == 0)
		{
			vt_write(idx, "ls: cannot access '");
			vt_write(idx, target);
			vt_write(idx, "': not found\r\n");
		}
		return;
	}

	CInfoPBRec pb;
	Str255 name;
	short index;

	for (index = 1; ; index++)
	{
		memset(&pb, 0, sizeof(pb));
		name[0] = 0;
		pb.hFileInfo.ioNamePtr = name;
		pb.hFileInfo.ioVRefNum = vRef;
		pb.hFileInfo.ioDirID = dID;
		pb.hFileInfo.ioFDirIndex = index;

		if (PBGetCatInfoSync(&pb) != noErr) break;

		char name_c[256];
		{
			int nl = name[0];
			if (nl > 255) nl = 255;
			memcpy(name_c, name + 1, nl);
			name_c[nl] = '\0';
		}

		int is_dir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
		int is_invis = 0;
		if (is_dir)
			is_invis = (pb.dirInfo.ioDrUsrWds.frFlags & kIsInvisible) != 0;
		else
			is_invis = (pb.hFileInfo.ioFlFndrInfo.fdFlags & kIsInvisible) != 0;

		if (!show_all && (is_invis || name_c[0] == '.')) continue;
		if (!glob_match(pattern, name_c)) continue;

		if (mode == 0 && is_dir) continue;  /* files pass: skip dirs */
		if (mode == 1 && !is_dir) continue; /* dirs pass: skip files */

		if (mode == 0)
		{
			/* show as file entry with parent path prefix */
			FSSpec fspec;
			fspec.vRefNum = vRef;
			fspec.parID = dID;
			fspec.name[0] = name[0];
			memcpy(fspec.name + 1, name + 1, name[0]);

			if (long_fmt)
			{
				/* long format: show metadata then prefixed name */
				ls_show_file(idx, &fspec, long_fmt);
			}
			else
			{
				/* short format: prefix with parent path */
				const char* last_sep = strrchr(target, ':');
				if (last_sep)
				{
					char prefix[256];
					int plen = (int)(last_sep - target) + 1;
					if (plen > 255) plen = 255;
					memcpy(prefix, target, plen);
					prefix[plen] = '\0';

					int is_locked2 = (pb.hFileInfo.ioFlAttrib & 0x01) != 0;
					OSType ftype2 = pb.hFileInfo.ioFlFndrInfo.fdType;
					const char* color = ls_color(0, is_locked2, is_invis, ftype2);
					if (*color) vt_write(idx, color);
					vt_write(idx, prefix);
					vt_write(idx, name_c);
					if (*color) vt_write(idx, "\033[0m");
					vt_write(idx, "\r\n");
				}
				else
				{
					ls_show_file(idx, &fspec, 0);
				}
			}
		}
		else
		{
			/* mode 1: expand directory with full path header */
			long sub_dID = pb.dirInfo.ioDrDirID;
			vt_write(idx, "\r\n");
			/* include parent path prefix from glob target */
			{
				const char* last_sep = strrchr(target, ':');
				if (last_sep)
				{
					char prefix[256];
					int plen = (int)(last_sep - target) + 1; /* include the : */
					if (plen > 255) plen = 255;
					memcpy(prefix, target, plen);
					prefix[plen] = '\0';
					vt_write(idx, prefix);
				}
			}
			vt_write(idx, name_c);
			vt_write(idx, ":\r\n");
			ls_show_dir(idx, vRef, sub_dID, long_fmt, show_all);
		}
	}
}

/* check if a glob has any matches of a given type (0=files, 1=dirs) */
static int glob_has_type(int idx, const char* target, int show_all, int want_dir)
{
	short vRef;
	long dID;
	const char* pattern;

	if (glob_resolve_dir(idx, target, &vRef, &dID, &pattern) != 0)
		return 0;

	CInfoPBRec pb;
	Str255 name;
	short index;

	for (index = 1; ; index++)
	{
		memset(&pb, 0, sizeof(pb));
		name[0] = 0;
		pb.hFileInfo.ioNamePtr = name;
		pb.hFileInfo.ioVRefNum = vRef;
		pb.hFileInfo.ioDirID = dID;
		pb.hFileInfo.ioFDirIndex = index;

		if (PBGetCatInfoSync(&pb) != noErr) break;

		char name_c[256];
		{
			int nl = name[0];
			if (nl > 255) nl = 255;
			memcpy(name_c, name + 1, nl);
			name_c[nl] = '\0';
		}

		int is_dir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
		int is_invis = 0;
		if (is_dir)
			is_invis = (pb.dirInfo.ioDrUsrWds.frFlags & kIsInvisible) != 0;
		else
			is_invis = (pb.hFileInfo.ioFlFndrInfo.fdFlags & kIsInvisible) != 0;

		if (!show_all && (is_invis || name_c[0] == '.')) continue;
		if (!glob_match(pattern, name_c)) continue;

		if (want_dir && is_dir) return 1;
		if (!want_dir && !is_dir) return 1;
	}
	return 0;
}

/* list a single file entry with optional long format */
static void ls_show_file(int idx, FSSpec* tspec, int long_fmt)
{
	CInfoPBRec pb;
	memset(&pb, 0, sizeof(pb));
	pb.hFileInfo.ioNamePtr = tspec->name;
	pb.hFileInfo.ioVRefNum = tspec->vRefNum;
	pb.hFileInfo.ioDirID = tspec->parID;
	pb.hFileInfo.ioFDirIndex = 0;

	if (PBGetCatInfoSync(&pb) != noErr) return;

	int is_dir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
	int is_invis = 0;
	int is_locked = 0;
	OSType ftype = 0;

	if (is_dir)
		is_invis = (pb.dirInfo.ioDrUsrWds.frFlags & kIsInvisible) != 0;
	else
	{
		is_invis = (pb.hFileInfo.ioFlFndrInfo.fdFlags & kIsInvisible) != 0;
		is_locked = (pb.hFileInfo.ioFlAttrib & 0x01) != 0;
		ftype = pb.hFileInfo.ioFlFndrInfo.fdType;
	}

	if (long_fmt)
	{
		char perm[5], type_s[5], crea_s[5], date_s[32];
		char sizebuf[24];

		if (is_dir)
		{
			strcpy(perm, "drw-");
			strcpy(type_s, "fold");
			strcpy(crea_s, "MACS");
			format_date(pb.dirInfo.ioDrMdDat, date_s, sizeof(date_s));
			snprintf(sizebuf, sizeof(sizebuf), "%7d", (int)pb.dirInfo.ioDrNmFls);
		}
		else
		{
			perm[0] = '-';
			perm[1] = 'r';
			perm[2] = is_locked ? '-' : 'w';
			perm[3] = is_invis ? 'i' : '-';
			perm[4] = '\0';
			ostype_to_str(ftype, type_s);
			ostype_to_str(pb.hFileInfo.ioFlFndrInfo.fdCreator, crea_s);
			format_date(pb.hFileInfo.ioFlMdDat, date_s, sizeof(date_s));
			snprintf(sizebuf, sizeof(sizebuf), "%7ld",
				pb.hFileInfo.ioFlLgLen + pb.hFileInfo.ioFlRLgLen);
		}

		vt_write(idx, perm);
		vt_write(idx, "  ");
		vt_write(idx, type_s);
		vt_write(idx, "/");
		vt_write(idx, crea_s);
		vt_write(idx, " ");
		vt_write(idx, sizebuf);
		vt_write(idx, "  ");
		vt_write(idx, date_s);
		vt_write(idx, "  ");
	}

	{
		char name_c[64];
		int len = tspec->name[0];
		if (len > 63) len = 63;
		memcpy(name_c, tspec->name + 1, len);
		name_c[len] = '\0';

		const char* color = ls_color(is_dir, is_locked, is_invis, ftype);
		if (*color) vt_write(idx, color);
		vt_write(idx, name_c);
		if (is_dir) vt_write(idx, "/");
		if (*color) vt_write(idx, "\033[0m");
	}
	vt_write(idx, "\r\n");
}

/* list contents of a directory */
static void ls_show_dir(int idx, short vRef, long dID, int long_fmt, int show_all)
{
	CInfoPBRec pb;
	Str255 name;
	short index;
	int size_width = 7; /* minimum width */

	/* first pass for long format: find max size to auto-size column */
	if (long_fmt)
	{
		for (index = 1; ; index++)
		{
			memset(&pb, 0, sizeof(pb));
			name[0] = 0;
			pb.hFileInfo.ioNamePtr = name;
			pb.hFileInfo.ioVRefNum = vRef;
			pb.hFileInfo.ioDirID = dID;
			pb.hFileInfo.ioFDirIndex = index;

			if (PBGetCatInfoSync(&pb) != noErr) break;

			int is_dir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
			int is_invis = 0;
			if (is_dir)
				is_invis = (pb.dirInfo.ioDrUsrWds.frFlags & kIsInvisible) != 0;
			else
				is_invis = (pb.hFileInfo.ioFlFndrInfo.fdFlags & kIsInvisible) != 0;

			if (!show_all && (is_invis || name[1] == '.')) continue;

			long sz;
			if (is_dir)
				sz = (long)pb.dirInfo.ioDrNmFls;
			else
				sz = pb.hFileInfo.ioFlLgLen + pb.hFileInfo.ioFlRLgLen;

			/* count digits */
			{
				long tmp = sz;
				int digits = 1;
				while (tmp >= 10) { tmp /= 10; digits++; }
				if (digits > size_width) size_width = digits;
			}
		}
	}

	/* main pass: display entries */
	for (index = 1; ; index++)
	{
		memset(&pb, 0, sizeof(pb));
		name[0] = 0;
		pb.hFileInfo.ioNamePtr = name;
		pb.hFileInfo.ioVRefNum = vRef;
		pb.hFileInfo.ioDirID = dID;
		pb.hFileInfo.ioFDirIndex = index;

		if (PBGetCatInfoSync(&pb) != noErr) break;

		char name_c[256];
		{
			int len = name[0];
			if (len > 255) len = 255;
			memcpy(name_c, name + 1, len);
			name_c[len] = '\0';
		}

		int is_dir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
		int is_invis = 0;

		if (is_dir)
			is_invis = (pb.dirInfo.ioDrUsrWds.frFlags & kIsInvisible) != 0;
		else
			is_invis = (pb.hFileInfo.ioFlFndrInfo.fdFlags & kIsInvisible) != 0;

		if (!show_all && (is_invis || name_c[0] == '.')) continue;

		if (long_fmt)
		{
			char perm[5];
			char type_s[5], crea_s[5], date_s[32];
			char sizebuf[24];
			char fmt[16];

			snprintf(fmt, sizeof(fmt), "%%%dld", size_width);

			if (is_dir)
			{
				strcpy(perm, "drw-");
				strcpy(type_s, "fold");
				strcpy(crea_s, "MACS");
				format_date(pb.dirInfo.ioDrMdDat, date_s, sizeof(date_s));
				snprintf(sizebuf, sizeof(sizebuf), fmt, (long)pb.dirInfo.ioDrNmFls);
			}
			else
			{
				perm[0] = '-';
				perm[1] = 'r';
				perm[2] = (pb.hFileInfo.ioFlAttrib & 0x01) ? '-' : 'w';
				perm[3] = is_invis ? 'i' : '-';
				perm[4] = '\0';

				ostype_to_str(pb.hFileInfo.ioFlFndrInfo.fdType, type_s);
				ostype_to_str(pb.hFileInfo.ioFlFndrInfo.fdCreator, crea_s);
				format_date(pb.hFileInfo.ioFlMdDat, date_s, sizeof(date_s));
				snprintf(sizebuf, sizeof(sizebuf), fmt,
					pb.hFileInfo.ioFlLgLen + pb.hFileInfo.ioFlRLgLen);
			}

			vt_write(idx, perm);
			vt_write(idx, "  ");
			vt_write(idx, type_s);
			vt_write(idx, "/");
			vt_write(idx, crea_s);
			vt_write(idx, " ");
			vt_write(idx, sizebuf);
			vt_write(idx, "  ");
			vt_write(idx, date_s);
			vt_write(idx, "  ");
		}

		{
			int is_locked = !is_dir && (pb.hFileInfo.ioFlAttrib & 0x01);
			OSType ftype = is_dir ? 0 : pb.hFileInfo.ioFlFndrInfo.fdType;
			const char* color = ls_color(is_dir, is_locked, is_invis, ftype);
			if (color[0]) vt_write(idx, color);
			vt_write(idx, name_c);
			if (is_dir) vt_write(idx, "/");
			if (color[0]) vt_write(idx, "\033[0m");
		}

		vt_write(idx, "\r\n");
	}
}

static int is_glob(const char* s)
{
	return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

static void cmd_ls(int idx, int argc, char* argv[])
{
	struct session* s = &sessions[idx];
	int long_fmt = 0;
	int show_all = 0;
	int argi;
	int path_count = 0;

	/* parse flags, count paths */
	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-')
		{
			char* f = argv[argi] + 1;
			while (*f)
			{
				if (*f == 'l') long_fmt = 1;
				else if (*f == 'a') show_all = 1;
				f++;
			}
		}
		else
		{
			path_count++;
		}
	}

	if (path_count == 0)
	{
		ls_show_dir(idx, s->shell_vRefNum, s->shell_dirID, long_fmt, show_all);
		return;
	}

	/* PASS 1: errors for nonexistent non-glob paths + no-match globs */
	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-') continue;
		if (is_glob(argv[argi]))
		{
			/* check if glob has any matches at all */
			if (!glob_has_type(idx, argv[argi], show_all, 0) &&
				!glob_has_type(idx, argv[argi], show_all, 1))
			{
				vt_write(idx, "ls: no match for '");
				vt_write(idx, argv[argi]);
				vt_write(idx, "'\r\n");
			}
		}
		else
		{
			FSSpec tspec;
			OSErr e = resolve_path(idx, argv[argi], &tspec);
			if (e != noErr)
			{
				vt_write(idx, "ls: cannot access '");
				vt_write(idx, argv[argi]);
				vt_write(idx, "': not found\r\n");
			}
		}
	}

	/* PASS 2: file results (non-directory) */
	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-') continue;

		if (is_glob(argv[argi]))
		{
			cmd_ls_glob(idx, argv[argi], long_fmt, show_all, 0);
		}
		else
		{
			FSSpec tspec;
			OSErr e = resolve_path(idx, argv[argi], &tspec);
			if (e != noErr) continue;
			if (!is_directory(&tspec))
				ls_show_file(idx, &tspec, long_fmt);
		}
	}

	/* PASS 3: directory results with headers, each preceded by blank line */
	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-') continue;

		if (is_glob(argv[argi]))
		{
			cmd_ls_glob(idx, argv[argi], long_fmt, show_all, 1);
		}
		else
		{
			FSSpec tspec;
			OSErr e = resolve_path(idx, argv[argi], &tspec);
			if (e != noErr) continue;
			if (is_directory(&tspec))
			{
				vt_write(idx, "\r\n");
				vt_write(idx, argv[argi]);
				vt_write(idx, ":\r\n");
				ls_show_dir(idx, tspec.vRefNum, get_dir_id(&tspec),
							long_fmt, show_all);
			}
		}
	}
}

static void cmd_cd(int idx, int argc, char* argv[])
{
	struct session* s = &sessions[idx];

	if (argc < 2)
	{
		/* cd with no args goes to system disk root */
		short vRef;
		long dID;
		if (FindFolder(kOnSystemDisk, kDesktopFolderType, kDontCreateFolder, &vRef, &dID) == noErr)
		{
			s->shell_vRefNum = vRef;
			s->shell_dirID = fsRtDirID;
		}
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[1], &spec);

	/* fnfErr with parID set means the parent exists but name doesn't,
	   but for ".." resolve_path returns "::" which FSMakeFSSpec handles */
	if (e == fnfErr)
	{
		/* check if it resolves to parent via "::" */
		if (strcmp(argv[1], "..") == 0)
		{
			CInfoPBRec pb;
			Str255 name;
			memset(&pb, 0, sizeof(pb));
			name[0] = 0;
			pb.dirInfo.ioNamePtr = name;
			pb.dirInfo.ioVRefNum = s->shell_vRefNum;
			pb.dirInfo.ioDrDirID = s->shell_dirID;
			pb.dirInfo.ioFDirIndex = -1;

			if (PBGetCatInfoSync(&pb) == noErr)
			{
				if (pb.dirInfo.ioDrDirID != fsRtDirID)
				{
					s->shell_dirID = pb.dirInfo.ioDrParID;
				}
			}
			return;
		}

		vt_write(idx, "cd: no such directory: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	if (e != noErr)
	{
		vt_write(idx, "cd: error accessing path\r\n");
		return;
	}

	if (!is_directory(&spec))
	{
		vt_write(idx, "cd: not a directory: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	s->shell_vRefNum = spec.vRefNum;
	s->shell_dirID = get_dir_id(&spec);
}

static void cmd_pwd(int idx, int argc, char* argv[])
{
	struct session* s = &sessions[idx];
	char path[1024];

	get_full_path(s->shell_vRefNum, s->shell_dirID, path, sizeof(path));
	vt_write(idx, path);
	vt_write(idx, "\r\n");
}

static void cmd_cat(int idx, int argc, char* argv[])
{
	if (argc < 2)
	{
		vt_write(idx, "usage: cat <file>\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "cat: file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	short refNum;
	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "cat: cannot open file\r\n");
		return;
	}

	char buf[512];
	long count;

	while (1)
	{
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);

		if (count > 0)
		{
			/* convert \r to \r\n for terminal display */
			long i;
			for (i = 0; i < count; i++)
			{
				if (buf[i] == '\r')
					vt_write(idx, "\r\n");
				else if (buf[i] == '\n')
					; /* skip bare LF (CR already handled) */
				else
					vt_char(idx, buf[i]);
			}
		}

		if (e == eofErr || e != noErr) break;
	}

	FSClose(refNum);
	vt_write(idx, "\r\n");
}

static void cmd_mkdir(int idx, int argc, char* argv[])
{
	struct session* s = &sessions[idx];

	if (argc < 2)
	{
		vt_write(idx, "usage: mkdir <name>\r\n");
		return;
	}

	Str255 pname;
	{
		int len = strlen(argv[1]);
		if (len > 255) len = 255;
		pname[0] = len;
		memcpy(pname + 1, argv[1], len);
	}

	long newDirID;
	OSErr e = DirCreate(s->shell_vRefNum, s->shell_dirID, pname, &newDirID);
	if (e != noErr)
	{
		vt_write(idx, "mkdir: failed to create directory\r\n");
	}
}

/* wait for y/n + Enter, pumping events to keep UI alive */
static int shell_confirm(int idx)
{
	EventRecord evt;
	char answer = 0; /* 0=no input yet, 'y' or 'n' */

	while (1)
	{
		if (WaitNextEvent(everyEvent, &evt, 10, NULL))
		{
			if (evt.what == keyDown)
			{
				char c = evt.message & charCodeMask;
				if (c == 13 || c == 3) /* Return or Enter */
				{
					if (answer == 'y') { vt_write(idx, "\r\n"); return 1; }
					if (answer == 'n') { vt_write(idx, "\r\n"); return 0; }
					/* no valid input yet, default to no */
					vt_write(idx, "n\r\n");
					return 0;
				}
				else if (c == 27) /* Escape = cancel */
				{
					vt_write(idx, "n\r\n");
					return 0;
				}
				else if (c == 'y' || c == 'Y')
				{
					answer = 'y';
					vt_write(idx, "y");
				}
				else if (c == 'n' || c == 'N')
				{
					answer = 'n';
					vt_write(idx, "n");
				}
				else if (c == 8 || c == 127) /* backspace/delete */
				{
					if (answer)
					{
						vt_write(idx, "\b \b");
						answer = 0;
					}
				}
			}
			else if (evt.what == updateEvt)
			{
				WindowPtr evtWin = (WindowPtr)evt.message;
				struct window_context* ewc = find_window_context(evtWin);
				if (ewc)
				{
					SetPort(evtWin);
					BeginUpdate(evtWin);
					draw_screen(ewc, &(evtWin->portRect));
					EndUpdate(evtWin);
				}
			}
		}
		YieldToAnyThread();
	}
}

/* simple --more-- pager: returns 0 to continue, 1 if user quit (q/Esc) */
static int shell_more_prompt(int idx)
{
	EventRecord evt;
	vt_write(idx, "\033[7m--more--\033[0m");
	while (1)
	{
		if (WaitNextEvent(everyEvent, &evt, 10, NULL))
		{
			if (evt.what == keyDown)
			{
				char c = evt.message & charCodeMask;
				/* erase the --more-- prompt */
				vt_write(idx, "\r\033[K");
				if (c == 'q' || c == 'Q' || c == 27) /* q or Escape = quit */
					return 1;
				return 0; /* any other key = next page */
			}
			else if (evt.what == updateEvt)
			{
				WindowPtr evtWin = (WindowPtr)evt.message;
				struct window_context* ewc = find_window_context(evtWin);
				if (ewc)
				{
					SetPort(evtWin);
					BeginUpdate(evtWin);
					draw_screen(ewc, &(evtWin->portRect));
					EndUpdate(evtWin);
				}
			}
		}
		YieldToAnyThread();
	}
}

/* paginated write: outputs lines with --more-- prompt every screenful */
static void shell_paged_write(int idx, const char** lines, int nlines)
{
	struct window_context* wc = window_for_session(idx);
	int page_size = wc ? wc->size_y - 2 : 22; /* leave room for prompt */
	int line_count = 0;
	int i;

	for (i = 0; i < nlines; i++)
	{
		vt_write(idx, lines[i]);
		vt_write(idx, "\r\n");
		line_count++;

		if (line_count >= page_size && i + 1 < nlines)
		{
			if (shell_more_prompt(idx))
				return; /* user pressed q */
			line_count = 0;
		}
	}
}

static void cmd_rm(int idx, int argc, char* argv[])
{
	int i;
	int interactive = 0;
	int start = 1;

	if (argc < 2)
	{
		vt_write(idx, "usage: rm [-i] <file ...>\r\n");
		return;
	}

	if (argc >= 2 && strcmp(argv[1], "-i") == 0)
	{
		interactive = 1;
		start = 2;
	}

	if (start >= argc)
	{
		vt_write(idx, "usage: rm [-i] <file ...>\r\n");
		return;
	}

	for (i = start; i < argc; i++)
	{
		if (is_glob(argv[i]))
		{
			/* expand glob against current directory */
			struct session* s = &sessions[idx];
			CInfoPBRec pb;
			Str255 name;
			short gi;
			int matched = 0;

			for (gi = 1; ; gi++)
			{
				FSSpec fspec;
				OSErr e;
				char name_c[256];
				int nl;

				memset(&pb, 0, sizeof(pb));
				pb.hFileInfo.ioNamePtr = name;
				pb.hFileInfo.ioVRefNum = s->shell_vRefNum;
				pb.hFileInfo.ioDirID = s->shell_dirID;
				pb.hFileInfo.ioFDirIndex = gi;

				if (PBGetCatInfoSync(&pb) != noErr) break;

				nl = name[0];
				if (nl > 255) nl = 255;
				memcpy(name_c, name + 1, nl);
				name_c[nl] = '\0';

				/* skip directories */
				if (pb.hFileInfo.ioFlAttrib & ioDirMask) continue;

				if (!glob_match(argv[i], name_c)) continue;

				matched++;

				if (interactive)
				{
					vt_write(idx, "rm: delete ");
					vt_write(idx, name_c);
					vt_write(idx, "? (y/n) ");
					if (!shell_confirm(idx)) continue;
				}

				fspec.vRefNum = s->shell_vRefNum;
				fspec.parID = s->shell_dirID;
				fspec.name[0] = name[0];
				memcpy(fspec.name + 1, name + 1, name[0]);

				e = FSpDelete(&fspec);
				if (e == noErr)
				{
					gi--; /* deleted entry shifts directory indices down */
				}
				else if (e == fLckdErr)
				{
					vt_write(idx, "rm: locked: ");
					vt_write(idx, name_c);
					vt_write(idx, "\r\n");
				}
				else if (e == fBsyErr)
				{
					vt_write(idx, "rm: busy: ");
					vt_write(idx, name_c);
					vt_write(idx, "\r\n");
				}
				else if (e != noErr)
				{
					vt_write(idx, "rm: failed: ");
					vt_write(idx, name_c);
					vt_write(idx, "\r\n");
				}
			}

			if (!matched)
			{
				vt_write(idx, "rm: no match: ");
				vt_write(idx, argv[i]);
				vt_write(idx, "\r\n");
			}
		}
		else
		{
			FSSpec spec;
			OSErr e = resolve_path(idx, argv[i], &spec);
			if (e != noErr)
			{
				vt_write(idx, "rm: not found: ");
				vt_write(idx, argv[i]);
				vt_write(idx, "\r\n");
				continue;
			}

			if (interactive)
			{
				vt_write(idx, "rm: delete ");
				vt_write(idx, argv[i]);
				vt_write(idx, "? (y/n) ");
				if (!shell_confirm(idx)) continue;
			}

			e = FSpDelete(&spec);
			if (e == fLckdErr)
				vt_write(idx, "rm: file is locked\r\n");
			else if (e == fBsyErr)
				vt_write(idx, "rm: file is busy\r\n");
			else if (e != noErr)
				vt_write(idx, "rm: delete failed\r\n");
		}
	}
}

static void cmd_rmdir(int idx, int argc, char* argv[])
{
	if (argc < 2)
	{
		vt_write(idx, "usage: rmdir <dir>\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "rmdir: not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	if (!is_directory(&spec))
	{
		vt_write(idx, "rmdir: not a directory\r\n");
		return;
	}

	e = FSpDelete(&spec);
	if (e == fBsyErr)
		vt_write(idx, "rmdir: directory not empty\r\n");
	else if (e != noErr)
		vt_write(idx, "rmdir: failed\r\n");
}

static void cmd_touch(int idx, int argc, char* argv[])
{
	if (argc < 2)
	{
		vt_write(idx, "usage: touch <file>\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[1], &spec);

	if (e == fnfErr)
	{
		/* create the file */
		e = FSpCreate(&spec, '????', 'TEXT', smSystemScript);
		if (e != noErr)
			vt_write(idx, "touch: create failed\r\n");
	}
	else if (e == noErr)
	{
		/* file exists, update modification date */
		CInfoPBRec pb;
		memset(&pb, 0, sizeof(pb));
		pb.hFileInfo.ioNamePtr = spec.name;
		pb.hFileInfo.ioVRefNum = spec.vRefNum;
		pb.hFileInfo.ioDirID = spec.parID;
		pb.hFileInfo.ioFDirIndex = 0;

		if (PBGetCatInfoSync(&pb) == noErr)
		{
			unsigned long now;
			GetDateTime(&now);
			pb.hFileInfo.ioFlMdDat = now;
			pb.hFileInfo.ioDirID = spec.parID;
			PBSetCatInfoSync(&pb);
		}
	}
}

static void cmd_mv(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: mv <source> <dest>\r\n");
		return;
	}

	FSSpec src_spec;
	OSErr e = resolve_path(idx, argv[1], &src_spec);
	if (e != noErr)
	{
		vt_write(idx, "mv: source not found\r\n");
		return;
	}

	/* try rename (same directory) */
	Str255 new_name;
	{
		int len = strlen(argv[2]);
		if (len > 255) len = 255;
		new_name[0] = len;
		memcpy(new_name + 1, argv[2], len);
	}

	e = FSpRename(&src_spec, new_name);
	if (e == dupFNErr)
		vt_write(idx, "mv: destination already exists\r\n");
	else if (e != noErr)
		vt_write(idx, "mv: rename failed\r\n");
}

static void cmd_cp(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: cp <source> <dest>\r\n");
		return;
	}

	FSSpec src_spec, dst_spec;
	OSErr e = resolve_path(idx, argv[1], &src_spec);
	if (e != noErr)
	{
		vt_write(idx, "cp: source not found\r\n");
		return;
	}

	/* get source FInfo */
	FInfo finfo;
	e = FSpGetFInfo(&src_spec, &finfo);
	if (e != noErr)
	{
		vt_write(idx, "cp: cannot read source info\r\n");
		return;
	}

	/* create dest */
	e = resolve_path(idx, argv[2], &dst_spec);
	if (e != noErr && e != fnfErr)
	{
		vt_write(idx, "cp: invalid destination\r\n");
		return;
	}

	e = FSpCreate(&dst_spec, finfo.fdCreator, finfo.fdType, smSystemScript);
	if (e == dupFNErr)
	{
		/* dest exists, overwrite */
	}
	else if (e != noErr)
	{
		vt_write(idx, "cp: create failed\r\n");
		return;
	}

	/* copy data fork */
	{
		short srcRef, dstRef;
		e = FSpOpenDF(&src_spec, fsRdPerm, &srcRef);
		if (e != noErr) { vt_write(idx, "cp: cannot open source\r\n"); return; }

		e = FSpOpenDF(&dst_spec, fsWrPerm, &dstRef);
		if (e != noErr) { FSClose(srcRef); vt_write(idx, "cp: cannot open dest\r\n"); return; }

		char buf[4096];
		long count;
		while (1)
		{
			count = sizeof(buf);
			e = FSRead(srcRef, &count, buf);
			if (count > 0) FSWrite(dstRef, &count, buf);
			if (e == eofErr || e != noErr) break;
		}

		FSClose(srcRef);
		FSClose(dstRef);
	}

	/* copy resource fork */
	{
		short srcRef, dstRef;
		if (FSpOpenRF(&src_spec, fsRdPerm, &srcRef) == noErr)
		{
			if (FSpOpenRF(&dst_spec, fsWrPerm, &dstRef) == noErr)
			{
				char buf[4096];
				long count;
				while (1)
				{
					count = sizeof(buf);
					e = FSRead(srcRef, &count, buf);
					if (count > 0) FSWrite(dstRef, &count, buf);
					if (e == eofErr || e != noErr) break;
				}
				FSClose(dstRef);
			}
			FSClose(srcRef);
		}
	}
}

static void cmd_echo(int idx, int argc, char* argv[])
{
	int i;
	for (i = 1; i < argc; i++)
	{
		if (i > 1) vt_write(idx, " ");
		vt_write(idx, argv[i]);
	}
	vt_write(idx, "\r\n");
}

static void cmd_clear(int idx, int argc, char* argv[])
{
	vt_write(idx, "\033[2J\033[H");
}

static void cmd_getinfo(int idx, int argc, char* argv[])
{
	if (argc < 2)
	{
		vt_write(idx, "usage: getinfo <file|dir>\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "getinfo: not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	CInfoPBRec pb;
	memset(&pb, 0, sizeof(pb));
	pb.hFileInfo.ioNamePtr = spec.name;
	pb.hFileInfo.ioVRefNum = spec.vRefNum;
	pb.hFileInfo.ioDirID = spec.parID;
	pb.hFileInfo.ioFDirIndex = 0;

	if (PBGetCatInfoSync(&pb) != noErr)
	{
		vt_write(idx, "getinfo: cannot get info\r\n");
		return;
	}

	int is_dir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;

	/* name */
	{
		char name_c[256];
		int len = spec.name[0];
		if (len > 255) len = 255;
		memcpy(name_c, spec.name + 1, len);
		name_c[len] = '\0';
		vt_write(idx, "Name:     ");
		vt_write(idx, name_c);
		vt_write(idx, "\r\n");
	}

	vt_write(idx, "Kind:     ");
	vt_write(idx, is_dir ? "Folder" : "File");
	vt_write(idx, "\r\n");

	if (!is_dir)
	{
		char type_s[5], crea_s[5];
		ostype_to_str(pb.hFileInfo.ioFlFndrInfo.fdType, type_s);
		ostype_to_str(pb.hFileInfo.ioFlFndrInfo.fdCreator, crea_s);

		vt_write(idx, "Type:     ");
		vt_write(idx, type_s);
		vt_write(idx, "\r\nCreator:  ");
		vt_write(idx, crea_s);
		vt_write(idx, "\r\n");

		vt_write(idx, "Data:     ");
		vt_print_long(idx, pb.hFileInfo.ioFlLgLen);
		vt_write(idx, " bytes\r\n");

		vt_write(idx, "Resource: ");
		vt_print_long(idx, pb.hFileInfo.ioFlRLgLen);
		vt_write(idx, " bytes\r\n");

		vt_write(idx, "Locked:   ");
		vt_write(idx, (pb.hFileInfo.ioFlAttrib & 0x01) ? "Yes" : "No");
		vt_write(idx, "\r\n");

		vt_write(idx, "Invisible:");
		vt_write(idx, (pb.hFileInfo.ioFlFndrInfo.fdFlags & kIsInvisible) ? " Yes" : " No");
		vt_write(idx, "\r\n");

		{
			char date_s[32];
			format_date(pb.hFileInfo.ioFlCrDat, date_s, sizeof(date_s));
			vt_write(idx, "Created:  ");
			vt_write(idx, date_s);
			vt_write(idx, "\r\n");

			format_date(pb.hFileInfo.ioFlMdDat, date_s, sizeof(date_s));
			vt_write(idx, "Modified: ");
			vt_write(idx, date_s);
			vt_write(idx, "\r\n");
		}

		/* Finder label color */
		{
			int label = (pb.hFileInfo.ioFlFndrInfo.fdFlags >> 1) & 0x07;
			static const char* label_names[] = {
				"None", "Orange", "Red", "Pink",
				"Blue", "Purple", "Green", "Gray"
			};
			vt_write(idx, "Label:    ");
			vt_write(idx, label_names[label]);
			vt_write(idx, "\r\n");
		}
	}
	else
	{
		vt_write(idx, "Items:    ");
		vt_print_long(idx, pb.dirInfo.ioDrNmFls);
		vt_write(idx, "\r\n");

		{
			char date_s[32];
			format_date(pb.dirInfo.ioDrCrDat, date_s, sizeof(date_s));
			vt_write(idx, "Created:  ");
			vt_write(idx, date_s);
			vt_write(idx, "\r\n");

			format_date(pb.dirInfo.ioDrMdDat, date_s, sizeof(date_s));
			vt_write(idx, "Modified: ");
			vt_write(idx, date_s);
			vt_write(idx, "\r\n");
		}
	}
}

static void cmd_chown(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: chown TYPE:CREATOR <file>\r\n");
		vt_write(idx, "  sets file type and creator codes (4 chars each)\r\n");
		return;
	}

	/* parse TYPE:CREATOR */
	char* colon = strchr(argv[1], ':');
	if (!colon)
	{
		vt_write(idx, "chown: format is TYPE:CREATOR (e.g. TEXT:ttxt)\r\n");
		return;
	}
	*colon = '\0';
	char* type_str = argv[1];
	char* crea_str = colon + 1;

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[2], &spec);
	if (e != noErr)
	{
		vt_write(idx, "chown: file not found\r\n");
		return;
	}

	FInfo finfo;
	e = FSpGetFInfo(&spec, &finfo);
	if (e != noErr)
	{
		vt_write(idx, "chown: cannot get file info\r\n");
		return;
	}

	finfo.fdType = str_to_ostype(type_str);
	finfo.fdCreator = str_to_ostype(crea_str);

	e = FSpSetFInfo(&spec, &finfo);
	if (e != noErr)
		vt_write(idx, "chown: failed to set type/creator\r\n");
}

static void cmd_settype(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: settype TYPE <file>\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[2], &spec);
	if (e != noErr) { vt_write(idx, "settype: file not found\r\n"); return; }

	FInfo finfo;
	e = FSpGetFInfo(&spec, &finfo);
	if (e != noErr) { vt_write(idx, "settype: cannot read info\r\n"); return; }

	finfo.fdType = str_to_ostype(argv[1]);
	FSpSetFInfo(&spec, &finfo);
}

static void cmd_setcreator(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: setcreator CREA <file>\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[2], &spec);
	if (e != noErr) { vt_write(idx, "setcreator: file not found\r\n"); return; }

	FInfo finfo;
	e = FSpGetFInfo(&spec, &finfo);
	if (e != noErr) { vt_write(idx, "setcreator: cannot read info\r\n"); return; }

	finfo.fdCreator = str_to_ostype(argv[1]);
	FSpSetFInfo(&spec, &finfo);
}

static void cmd_chmod(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: chmod +w|-w <file>  (unlock/lock)\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[2], &spec);
	if (e != noErr) { vt_write(idx, "chmod: file not found\r\n"); return; }

	if (strcmp(argv[1], "-w") == 0)
		e = FSpSetFLock(&spec);
	else if (strcmp(argv[1], "+w") == 0)
		e = FSpRstFLock(&spec);
	else
	{
		vt_write(idx, "chmod: use +w (unlock) or -w (lock)\r\n");
		return;
	}

	if (e != noErr)
		vt_write(idx, "chmod: operation failed\r\n");
}

static void cmd_chattr(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: chattr [+-][li] <file>\r\n");
		vt_write(idx, "  l = locked, i = invisible\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[2], &spec);
	if (e != noErr) { vt_write(idx, "chattr: file not found\r\n"); return; }

	FInfo finfo;
	e = FSpGetFInfo(&spec, &finfo);
	if (e != noErr) { vt_write(idx, "chattr: cannot read info\r\n"); return; }

	char* flags = argv[1];
	int adding = 1; /* +flag or -flag */
	int i;

	for (i = 0; flags[i]; i++)
	{
		if (flags[i] == '+') adding = 1;
		else if (flags[i] == '-') adding = 0;
		else if (flags[i] == 'l')
		{
			if (adding)
				FSpSetFLock(&spec);
			else
				FSpRstFLock(&spec);
		}
		else if (flags[i] == 'i')
		{
			if (adding)
				finfo.fdFlags |= kIsInvisible;
			else
				finfo.fdFlags &= ~kIsInvisible;
		}
	}

	FSpSetFInfo(&spec, &finfo);
}

static void cmd_label(int idx, int argc, char* argv[])
{
	if (argc < 3)
	{
		vt_write(idx, "usage: label <0-7> <file>\r\n");
		vt_write(idx, "  0=None 1=Orange 2=Red 3=Pink\r\n");
		vt_write(idx, "  4=Blue 5=Purple 6=Green 7=Gray\r\n");
		return;
	}

	int lab = atoi(argv[1]);
	if (lab < 0 || lab > 7)
	{
		vt_write(idx, "label: value must be 0-7\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[2], &spec);
	if (e != noErr) { vt_write(idx, "label: file not found\r\n"); return; }

	FInfo finfo;
	e = FSpGetFInfo(&spec, &finfo);
	if (e != noErr) { vt_write(idx, "label: cannot read info\r\n"); return; }

	/* label is bits 1-3 of fdFlags */
	finfo.fdFlags = (finfo.fdFlags & ~0x0E) | ((lab & 0x07) << 1);
	FSpSetFInfo(&spec, &finfo);
}

/* format bytes as human-readable string: "1.5 MB", "320 KB", etc. */
static void fmt_human(char* buf, int bufsz, long bytes)
{
	if (bytes >= 1024L * 1024L * 1024L)
		snprintf(buf, bufsz, "%ld.%ld GB",
			bytes / (1024L * 1024L * 1024L),
			(bytes / (1024L * 1024L * 100L)) % 10);
	else if (bytes >= 1024L * 1024L)
		snprintf(buf, bufsz, "%ld.%ld MB",
			bytes / (1024L * 1024L),
			(bytes / (1024L * 100L)) % 10);
	else
		snprintf(buf, bufsz, "%ld KB", bytes / 1024L);
}

static void cmd_df(int idx, int argc, char* argv[])
{
	struct session* s = &sessions[idx];
	HParamBlockRec pb;
	Str255 name;
	int mode = 0; /* 0=KB, 1=MB, 2=human */

	if (argc > 1 && strcmp(argv[1], "-m") == 0) mode = 1;
	if (argc > 1 && strcmp(argv[1], "-h") == 0) mode = 2;

	memset(&pb, 0, sizeof(pb));
	name[0] = 0;
	pb.volumeParam.ioNamePtr = name;
	pb.volumeParam.ioVRefNum = s->shell_vRefNum;
	pb.volumeParam.ioVolIndex = 0;

	if (PBHGetVInfoSync(&pb) != noErr)
	{
		vt_write(idx, "df: cannot get volume info\r\n");
		return;
	}

	/* volume name */
	{
		char vname[256];
		int len = name[0];
		if (len > 255) len = 255;
		memcpy(vname, name + 1, len);
		vname[len] = '\0';

		vt_write(idx, "Volume:    ");
		vt_write(idx, vname);
		vt_write(idx, "\r\n");
	}

	{
		long total_bytes = (long)pb.volumeParam.ioVNmAlBlks *
		                   (long)pb.volumeParam.ioVAlBlkSiz;
		long free_bytes  = (long)pb.volumeParam.ioVFrBlk *
		                   (long)pb.volumeParam.ioVAlBlkSiz;
		long used_bytes  = total_bytes - free_bytes;
		char buf[32];

		if (mode == 2)
		{
			vt_write(idx, "Total:     ");
			fmt_human(buf, sizeof(buf), total_bytes);
			vt_write(idx, buf);
			vt_write(idx, "\r\nUsed:      ");
			fmt_human(buf, sizeof(buf), used_bytes);
			vt_write(idx, buf);
			vt_write(idx, "\r\nAvailable: ");
			fmt_human(buf, sizeof(buf), free_bytes);
			vt_write(idx, buf);
			vt_write(idx, "\r\n");
		}
		else
		{
			long divisor = mode ? (1024L * 1024L) : 1024L;
			const char* unit = mode ? "MB" : "KB";

			vt_write(idx, "Total:     ");
			vt_print_long(idx, total_bytes / divisor);
			vt_write(idx, " ");
			vt_write(idx, unit);
			vt_write(idx, "\r\n");

			vt_write(idx, "Used:      ");
			vt_print_long(idx, used_bytes / divisor);
			vt_write(idx, " ");
			vt_write(idx, unit);
			vt_write(idx, "\r\n");

			vt_write(idx, "Available: ");
			vt_print_long(idx, free_bytes / divisor);
			vt_write(idx, " ");
			vt_write(idx, unit);
			vt_write(idx, "\r\n");
		}
	}
}

static void cmd_date(int idx, int argc, char* argv[])
{
	unsigned long secs;
	GetDateTime(&secs);

	DateTimeRec dt;
	SecondsToDate(secs, &dt);

	static const char* days[] = {
		"Sun","Mon","Tue","Wed","Thu","Fri","Sat"
	};
	static const char* months[] = {
		"Jan","Feb","Mar","Apr","May","Jun",
		"Jul","Aug","Sep","Oct","Nov","Dec"
	};

	if (dt.dayOfWeek < 1 || dt.dayOfWeek > 7) dt.dayOfWeek = 1;
	if (dt.month < 1 || dt.month > 12) dt.month = 1;

	char buf[64];
	snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %d\r\n",
		days[dt.dayOfWeek - 1],
		months[dt.month - 1],
		dt.day, dt.hour, dt.minute, dt.second, dt.year);

	vt_write(idx, buf);
}

static void cmd_uname(int idx, int argc, char* argv[])
{
	long cpu_type = 0;
	long sys_version = 0;
	long machine = 0;

	Gestalt(gestaltNativeCPUtype, &cpu_type);
	Gestalt(gestaltSystemVersion, &sys_version);
	Gestalt(gestaltMachineType, &machine);

	vt_write(idx, "Mac OS ");

	/* decode system version BCD */
	{
		int major = (sys_version >> 8) & 0xFF;
		int minor = (sys_version >> 4) & 0x0F;
		int patch = sys_version & 0x0F;
		char buf[16];
		snprintf(buf, sizeof(buf), "%d.%d.%d", major, minor, patch);
		vt_write(idx, buf);
	}

	/* CPU */
	vt_write(idx, " ");
	if (cpu_type >= 0x100)
		vt_write(idx, "PowerPC");
	else if (cpu_type >= gestaltCPU68040)
		vt_write(idx, "MC68040");
	else if (cpu_type >= gestaltCPU68030)
		vt_write(idx, "MC68030");
	else if (cpu_type >= gestaltCPU68020)
		vt_write(idx, "MC68020");
	else
		vt_write(idx, "MC680x0");

	vt_write(idx, "\r\n");
}

static void cmd_free(int idx, int argc, char* argv[])
{
	long app_total, app_free, app_largest;
	long sys_total, sys_free;
	long temp_free;
	char buf[128];
	int mode = 0; /* 0=KB, 1=MB, 2=human */
	long divisor;
	const char* unit;
	THz appZone;
	THz sysZone;

	if (argc > 1 && strcmp(argv[1], "-m") == 0) mode = 1;
	if (argc > 1 && strcmp(argv[1], "-h") == 0) mode = 2;

	appZone = ApplicationZone();
	sysZone = SystemZone();

	app_total = (long)appZone->bkLim - (long)(Ptr)appZone;
	app_free = FreeMem();
	app_largest = MaxBlock();

	sys_total = (long)sysZone->bkLim - (long)(Ptr)sysZone;
	sys_free = FreeMemSys();

	temp_free = TempFreeMem();

	if (mode == 2)
	{
		char h1[32], h2[32], h3[32];

		vt_write(idx, "              Total      Free   Largest\r\n");

		fmt_human(h1, sizeof(h1), app_total);
		fmt_human(h2, sizeof(h2), app_free);
		fmt_human(h3, sizeof(h3), app_largest);
		snprintf(buf, sizeof(buf), "App heap:  %9s %9s %9s\r\n", h1, h2, h3);
		vt_write(idx, buf);

		fmt_human(h1, sizeof(h1), sys_total);
		fmt_human(h2, sizeof(h2), sys_free);
		snprintf(buf, sizeof(buf), "Sys heap:  %9s %9s\r\n", h1, h2);
		vt_write(idx, buf);

		fmt_human(h1, sizeof(h1), temp_free);
		snprintf(buf, sizeof(buf), "Temp mem:            %9s\r\n", h1);
		vt_write(idx, buf);
	}
	else
	{
		divisor = mode ? (1024L * 1024L) : 1024L;
		unit = mode ? "MB" : "KB";

		vt_write(idx, "              Total      Free   Largest\r\n");

		snprintf(buf, sizeof(buf), "App heap:  %7ld %s %7ld %s %7ld %s\r\n",
			app_total / divisor, unit, app_free / divisor, unit, app_largest / divisor, unit);
		vt_write(idx, buf);

		snprintf(buf, sizeof(buf), "Sys heap:  %7ld %s %7ld %s\r\n",
			sys_total / divisor, unit, sys_free / divisor, unit);
		vt_write(idx, buf);

		snprintf(buf, sizeof(buf), "Temp mem:            %7ld %s\r\n",
			temp_free / divisor, unit);
		vt_write(idx, buf);
	}
}

static void cmd_ps(int idx, int argc, char* argv[])
{
	ProcessSerialNumber psn;
	ProcessSerialNumber my_psn;
	ProcessInfoRec info;
	Str255 name;
	char cname[256];
	char buf[320];
	int pid = 0;
	char type_str[5];
	char crea_str[5];

	MacGetCurrentProcess(&my_psn);

	vt_write(idx, "  PID   SIZE     TYPE CREA  NAME\r\n");

	psn.highLongOfPSN = kNoProcess;
	psn.lowLongOfPSN = kNoProcess;

	while (GetNextProcess(&psn) == noErr)
	{
		Boolean is_me = false;

		pid++;

		memset(&info, 0, sizeof(info));
		info.processInfoLength = sizeof(ProcessInfoRec);
		info.processName = name;
		info.processAppSpec = NULL;

		if (GetProcessInformation(&psn, &info) != noErr)
			continue;

		SameProcess(&psn, &my_psn, &is_me);

		/* Pascal string to C string */
		{
			int len = name[0];
			if (len > 255) len = 255;
			memcpy(cname, name + 1, len);
			cname[len] = '\0';
		}

		/* OSType to 4-char string */
		type_str[0] = (info.processType >> 24) & 0xFF;
		type_str[1] = (info.processType >> 16) & 0xFF;
		type_str[2] = (info.processType >> 8) & 0xFF;
		type_str[3] = info.processType & 0xFF;
		type_str[4] = '\0';

		crea_str[0] = (info.processSignature >> 24) & 0xFF;
		crea_str[1] = (info.processSignature >> 16) & 0xFF;
		crea_str[2] = (info.processSignature >> 8) & 0xFF;
		crea_str[3] = info.processSignature & 0xFF;
		crea_str[4] = '\0';

		snprintf(buf, sizeof(buf), "%s%4d %6ld KB  %s %s  %s%s\r\n",
			is_me ? "\033[1m" : "",
			pid,
			info.processSize / 1024,
			type_str,
			crea_str,
			cname,
			is_me ? "\033[0m" : "");
		vt_write(idx, buf);
	}
}

static void cmd_open(int idx, int argc, char* argv[])
{
	FSSpec spec;
	OSErr e;
	FInfo finfo;
	LaunchParamBlockRec lpb;

	if (argc < 2) { vt_write(idx, "usage: open <path>\r\n"); return; }

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr) { vt_write(idx, "open: file not found\r\n"); return; }

	e = FSpGetFInfo(&spec, &finfo);
	if (e != noErr) { vt_write(idx, "open: cannot get file info\r\n"); return; }

	if (finfo.fdType != 'APPL')
	{
		vt_write(idx, "open: not an application\r\n");
		return;
	}

	memset(&lpb, 0, sizeof(lpb));
	lpb.launchBlockID = extendedBlock;
	lpb.launchEPBLength = extendedBlockLen;
	lpb.launchAppSpec = &spec;
	lpb.launchControlFlags = launchContinue | launchNoFileFlags;

	e = LaunchApplication(&lpb);
	if (e != noErr)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "open: launch failed (error %d)\r\n", (int)e);
		vt_write(idx, buf);
	}
}

static void cmd_ssh(int idx, int argc, char* argv[])
{
	/* parse: ssh [user@]host[:port] */
	if (argc > 1)
	{
		char arg[256];
		strncpy(arg, argv[1], sizeof(arg) - 1);
		arg[255] = '\0';

		char* user = NULL;
		char* host = arg;
		char* port = NULL;

		/* split user@host */
		char* at = strchr(arg, '@');
		if (at)
		{
			*at = '\0';
			user = arg;
			host = at + 1;
		}

		/* split host:port */
		char* colon = strchr(host, ':');
		if (colon)
		{
			*colon = '\0';
			port = colon + 1;
		}

		/* fill in prefs as pascal strings */
		if (user)
		{
			int ulen = strlen(user);
			if (ulen > 254) ulen = 254;
			prefs.username[0] = ulen;
			memcpy(prefs.username + 1, user, ulen);
		}

		{
			int hlen = strlen(host);
			if (hlen > 254) hlen = 254;
			prefs.hostname[0] = hlen;
			memcpy(prefs.hostname + 1, host, hlen);
		}

		if (port)
		{
			int plen = strlen(port);
			if (plen > 254) plen = 254;
			prefs.port[0] = plen;
			memcpy(prefs.port + 1, port, plen);
		}
		else
		{
			/* default port 22 */
			prefs.port[0] = 2;
			prefs.port[1] = '2';
			prefs.port[2] = '2';
		}

	}

	/* intro_dialog() will build the combined "hostname:port" C string */
	{
		struct window_context* wc = window_for_session(idx);
		if (wc) new_session(wc, SESSION_SSH);
	}
}

static void cmd_telnet(int idx, int argc, char* argv[])
{
	/* telnet host [port] */
	struct window_context* wc;
	int new_idx;

	if (argc < 2)
	{
		vt_write(idx, "usage: telnet <host> [port]\r\n");
		return;
	}

	wc = window_for_session(idx);
	if (wc == NULL) return;

	new_idx = new_session(wc, SESSION_TELNET);
	if (new_idx < 0) return;

	/* set host/port BEFORE starting the connection thread */
	strncpy(sessions[new_idx].telnet_host, argv[1],
	        sizeof(sessions[new_idx].telnet_host) - 1);
	sessions[new_idx].telnet_host[255] = '\0';

	if (argc >= 3)
		sessions[new_idx].telnet_port = (unsigned short)atoi(argv[2]);
	else
		sessions[new_idx].telnet_port = 23;

	snprintf(sessions[new_idx].tab_label,
	         sizeof(sessions[new_idx].tab_label),
	         "telnet %s", argv[1]);

	if (telnet_connect(new_idx) == 0)
		telnet_disconnect(new_idx);
}

static void cmd_nc(int idx, int argc, char* argv[])
{
	/* nc host port — runs inline in the current session */
	struct session* s = &sessions[idx];

	if (argc < 3)
	{
		vt_write(idx, "usage: nc <host> <port>\r\n");
		return;
	}

	strncpy(s->telnet_host, argv[1], sizeof(s->telnet_host) - 1);
	s->telnet_host[255] = '\0';
	s->telnet_port = (unsigned short)atoi(argv[2]);

	if (nc_inline_connect(idx) == 0)
	{
		nc_inline_disconnect(idx);
		vt_write(idx, "nc: connection failed\r\n");
	}
}

/* ------------------------------------------------------------------ */
/* network diagnostic commands                                        */
/* ------------------------------------------------------------------ */

static int is_ip_address(const char* s)
{
	/* quick check: all chars are digits or dots */
	int dots = 0;
	const char* p;
	if (*s == '\0') return 0;
	for (p = s; *p; p++)
	{
		if (*p == '.') dots++;
		else if (*p < '0' || *p > '9') return 0;
	}
	return dots == 3;
}

static void cmd_host(int idx, int argc, char* argv[])
{
	/* DNS lookup using OT Internet Services */
	InetSvcRef inet_svc;
	InetHostInfo host_info;
	OSStatus err;
	int i;
	char ip_str[16];

	if (argc < 2)
	{
		vt_write(idx, "usage: host <hostname|ip>\r\n");
		return;
	}

	/* reverse lookup if it's an IP address */
	if (is_ip_address(argv[1]))
	{
		InetHost addr;
		InetDomainName name;

		if (InitOpenTransport() != noErr)
		{
			vt_write(idx, "host: Open Transport not available\r\n");
			return;
		}
		err = OTInetStringToHost(argv[1], &addr);
		if (err != noErr)
		{
			printf_s(idx, "host: invalid address \"%s\"\r\n", argv[1]);
			return;
		}

		inet_svc = OTOpenInternetServices(kDefaultInternetServicesPath, 0, &err);
		if (err != noErr || inet_svc == NULL)
		{
			printf_s(idx, "host: failed to open internet services (err=%d)\r\n", (int)err);
			return;
		}

		OTSetSynchronous(inet_svc);
		OTSetBlocking(inet_svc);
		OTInstallNotifier(inet_svc, shell_ot_timeout_notifier, nil);
		OTUseSyncIdleEvents(inet_svc, true);

		printf_s(idx, "Reverse lookup %s... ", argv[1]);

		ot_timeout_provider = inet_svc;
		ot_timeout_deadline = TickCount() + OT_TIMEOUT_TICKS;

		err = OTInetAddressToName(inet_svc, addr, name);

		ot_timeout_deadline = 0;
		ot_timeout_provider = nil;

		if (err == noErr)
			printf_s(idx, "%s\r\n", name);
		else if (err == kOTCanceledErr)
			vt_write(idx, "timed out\r\n");
		else
			printf_s(idx, "failed (err=%d)\r\n", (int)err);

		OTCloseProvider(inet_svc);
		return;
	}

	if (InitOpenTransport() != noErr)
	{
		vt_write(idx, "host: Open Transport not available\r\n");
		return;
	}

	inet_svc = OTOpenInternetServices(kDefaultInternetServicesPath, 0, &err);
	if (err != noErr || inet_svc == NULL)
	{
		printf_s(idx, "host: failed to open internet services (err=%d)\r\n", (int)err);
		return;
	}

	OTSetSynchronous(inet_svc);
	OTSetBlocking(inet_svc);
	OTInstallNotifier(inet_svc, shell_ot_timeout_notifier, nil);
	OTUseSyncIdleEvents(inet_svc, true);

	printf_s(idx, "Resolving \"%s\"... ", argv[1]);

	ot_timeout_provider = inet_svc;
	ot_timeout_deadline = TickCount() + OT_TIMEOUT_TICKS;

	err = OTInetStringToAddress(inet_svc, argv[1], &host_info);

	ot_timeout_deadline = 0;
	ot_timeout_provider = nil;

	if (err == kOTCanceledErr)
	{
		vt_write(idx, "timed out\r\n");
	}
	else if (err != noErr)
	{
		printf_s(idx, "failed (err=%d)\r\n", (int)err);
	}
	else
	{
		vt_write(idx, "\r\n");
		for (i = 0; i < kMaxHostAddrs; i++)
		{
			if (host_info.addrs[i] == 0) break;
			OTInetHostToString(host_info.addrs[i], ip_str);
			printf_s(idx, "  %s has address %s\r\n", host_info.name, ip_str);
		}
	}

	OTCloseProvider(inet_svc);
}

static void cmd_ifconfig(int idx, int argc, char* argv[])
{
	/* show network interface info */
	InetInterfaceInfo info;
	OSStatus err;
	SInt32 i;
	char ip_str[16];

	(void)argc;
	(void)argv;

	if (InitOpenTransport() != noErr)
	{
		vt_write(idx, "ifconfig: Open Transport not available\r\n");
		return;
	}

	for (i = 0; ; i++)
	{
		err = OTInetGetInterfaceInfo(&info, i);
		if (err != noErr) break;

		printf_s(idx, "Interface %d:\r\n", (int)i);

		OTInetHostToString(info.fAddress, ip_str);
		printf_s(idx, "  address:   %s\r\n", ip_str);

		OTInetHostToString(info.fNetmask, ip_str);
		printf_s(idx, "  netmask:   %s\r\n", ip_str);

		OTInetHostToString(info.fBroadcastAddr, ip_str);
		printf_s(idx, "  broadcast: %s\r\n", ip_str);

		OTInetHostToString(info.fDefaultGatewayAddr, ip_str);
		printf_s(idx, "  gateway:   %s\r\n", ip_str);

		OTInetHostToString(info.fDNSAddr, ip_str);
		printf_s(idx, "  dns:       %s\r\n", ip_str);

		vt_write(idx, "\r\n");
	}

	if (i == 0)
		vt_write(idx, "No network interfaces found.\r\n");
}

static void cmd_ping(int idx, int argc, char* argv[])
{
	/* TCP connect test — measures DNS + TCP handshake time */
	EndpointRef ep;
	OSStatus err;
	TCall sndCall;
	DNSAddress hostDNSAddress;
	char hostport[280];
	long start_ticks, elapsed;
	unsigned short port;

	if (argc < 2)
	{
		vt_write(idx, "usage: ping <host> [port]  (TCP connect test, default port 80)\r\n");
		return;
	}

	port = (argc >= 3) ? (unsigned short)atoi(argv[2]) : 80;
	snprintf(hostport, sizeof(hostport), "%s:%d", argv[1], (int)port);

	if (InitOpenTransport() != noErr)
	{
		vt_write(idx, "ping: Open Transport not available\r\n");
		return;
	}

	ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, nil, &err);
	if (err != noErr)
	{
		printf_s(idx, "ping: failed to open endpoint (err=%d)\r\n", (int)err);
		return;
	}

	OTSetSynchronous(ep);
	OTSetBlocking(ep);
	OTInstallNotifier(ep, shell_ot_timeout_notifier, nil);
	OTUseSyncIdleEvents(ep, true);

	err = OTBind(ep, nil, nil);
	if (err != noErr)
	{
		printf_s(idx, "ping: bind failed (err=%d)\r\n", (int)err);
		OTCloseProvider(ep);
		return;
	}

	OTMemzero(&sndCall, sizeof(TCall));
	sndCall.addr.buf = (UInt8 *) &hostDNSAddress;
	sndCall.addr.len = OTInitDNSAddress(&hostDNSAddress, hostport);

	printf_s(idx, "Connecting to %s... ", hostport);

	ot_timeout_provider = ep;
	ot_timeout_deadline = TickCount() + OT_TIMEOUT_TICKS;

	start_ticks = TickCount();
	err = OTConnect(ep, &sndCall, nil);
	elapsed = TickCount() - start_ticks;

	ot_timeout_deadline = 0;
	ot_timeout_provider = nil;

	if (err == noErr)
	{
		printf_s(idx, "connected (%ld ticks, ~%ldms)\r\n",
		         elapsed, elapsed * 1000 / 60);
		OTSndOrderlyDisconnect(ep);
	}
	else if (err == kOTCanceledErr)
	{
		vt_write(idx, "timed out\r\n");
	}
	else
	{
		printf_s(idx, "failed (err=%d, %ld ticks)\r\n", (int)err, elapsed);
	}

	OTUnbind(ep);
	OTCloseProvider(ep);
}

static void cmd_colors(int idx, int argc, char* argv[])
{
	int i;
	char buf[80];

	/* palette hex dump */
	vt_write(idx, "  Palette (hex RGB):\r\n");
	for (i = 0; i < 16; i++)
	{
		snprintf(buf, sizeof(buf), "    %2d: %02X%02X%02X\r\n",
			i,
			prefs.palette[i].red >> 8,
			prefs.palette[i].green >> 8,
			prefs.palette[i].blue >> 8);
		vt_write(idx, buf);
	}
	snprintf(buf, sizeof(buf), "    fg: %04X%04X%04X  bg: %04X%04X%04X\r\n",
		prefs.theme_fg.red, prefs.theme_fg.green, prefs.theme_fg.blue,
		prefs.theme_bg.red, prefs.theme_bg.green, prefs.theme_bg.blue);
	vt_write(idx, buf);
	snprintf(buf, sizeof(buf), "  orig_fg: %04X%04X%04X  orig_bg: %04X%04X%04X\r\n",
		prefs.orig_theme_fg.red, prefs.orig_theme_fg.green, prefs.orig_theme_fg.blue,
		prefs.orig_theme_bg.red, prefs.orig_theme_bg.green, prefs.orig_theme_bg.blue);
	vt_write(idx, buf);
	snprintf(buf, sizeof(buf), "  fg_ovr: %d  bg_ovr: %d  loaded: %d\r\n",
		prefs.fg_color, prefs.bg_color, prefs.theme_loaded);
	vt_write(idx, buf);
	snprintf(buf, sizeof(buf), "  theme: %s\r\n", prefs.theme_name);
	vt_write(idx, buf);
	vt_write(idx, "\r\n");

	/* normal colors 0-7: SGR 40-47 background blocks */
	vt_write(idx, "  Normal colors:\r\n  ");
	for (i = 0; i < 8; i++)
	{
		char esc[32];
		snprintf(esc, sizeof(esc), "\033[%dm %2d  \033[0m", 40 + i, i);
		vt_write(idx, esc);
	}
	vt_write(idx, "\r\n");
	/* bright colors 8-15: SGR 100-107 */
	vt_write(idx, "  Bright colors:\r\n  ");
	for (i = 0; i < 8; i++)
	{
		char esc[32];
		snprintf(esc, sizeof(esc), "\033[%dm %2d  \033[0m", 100 + i, 8 + i);
		vt_write(idx, esc);
	}
	vt_write(idx, "\r\n\r\n");
	/* foreground text samples */
	vt_write(idx, "  Foreground text:\r\n  ");
	for (i = 0; i < 8; i++)
	{
		char esc[32];
		if (i >= 2)
			snprintf(esc, sizeof(esc), "\033[%dmColor%-2d \033[0m", 30 + i, i);
		else
			snprintf(esc, sizeof(esc), "\033[%dmColor%d \033[0m", 30 + i, i);
		vt_write(idx, esc);
	}
	vt_write(idx, "\r\n  ");
	for (i = 0; i < 8; i++)
	{
		char esc[32];
		if (i >= 2)
			snprintf(esc, sizeof(esc), "\033[%dmColor%-2d \033[0m", 90 + i, 8 + i);
		else
			snprintf(esc, sizeof(esc), "\033[%dmColor%d \033[0m", 90 + i, 8 + i);
		vt_write(idx, esc);
	}
	vt_write(idx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* CRC32 (standard, same polynomial as zlib/cksfv)                    */
/* ------------------------------------------------------------------ */

static unsigned long crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_init_table(void)
{
	unsigned long poly = 0xEDB88320UL;
	int i, j;
	for (i = 0; i < 256; i++)
	{
		unsigned long c = (unsigned long)i;
		for (j = 0; j < 8; j++)
		{
			if (c & 1)
				c = poly ^ (c >> 1);
			else
				c = c >> 1;
		}
		crc32_table[i] = c;
	}
	crc32_table_ready = 1;
}

static unsigned long crc32_update(unsigned long crc, const unsigned char* buf, long len)
{
	long i;
	if (!crc32_table_ready) crc32_init_table();
	for (i = 0; i < len; i++)
		crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
	return crc;
}

/* ------------------------------------------------------------------ */
/* hash commands: md5sum, sha1sum, sha256sum, sha512sum, crc32        */
/* ------------------------------------------------------------------ */

enum hash_type { HASH_MD5, HASH_SHA1, HASH_SHA256, HASH_SHA512, HASH_CRC32 };

static void cmd_hash(int idx, int argc, char** argv, enum hash_type type)
{
	const char* name;
	unsigned char digest[64];
	int digest_len;
	mbedtls_md5_context md5_ctx;
	mbedtls_sha1_context sha1_ctx;
	mbedtls_sha256_context sha256_ctx;
	mbedtls_sha512_context sha512_ctx;
	unsigned long crc;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	char hex[129];
	int i;

	switch (type)
	{
		case HASH_MD5:    name = "md5sum"; break;
		case HASH_SHA1:   name = "sha1sum"; break;
		case HASH_SHA256: name = "sha256sum"; break;
		case HASH_SHA512: name = "sha512sum"; break;
		case HASH_CRC32:  name = "crc32"; break;
		default:          name = "hash"; break;
	}

	if (argc < 2)
	{
		vt_write(idx, "usage: ");
		vt_write(idx, name);
		vt_write(idx, " <file>\r\n");
		return;
	}

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, name);
		vt_write(idx, ": file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, name);
		vt_write(idx, ": cannot open file\r\n");
		return;
	}

	/* init */
	switch (type)
	{
		case HASH_MD5:    mbedtls_md5_init(&md5_ctx);     mbedtls_md5_starts(&md5_ctx);       break;
		case HASH_SHA1:   mbedtls_sha1_init(&sha1_ctx);   mbedtls_sha1_starts(&sha1_ctx);     break;
		case HASH_SHA256: mbedtls_sha256_init(&sha256_ctx); mbedtls_sha256_starts(&sha256_ctx, 0); break;
		case HASH_SHA512: mbedtls_sha512_init(&sha512_ctx); mbedtls_sha512_starts(&sha512_ctx, 0); break;
		case HASH_CRC32:  crc = 0xFFFFFFFFUL; break;
	}

	/* read and update */
	while (1)
	{
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		if (count > 0)
		{
			switch (type)
			{
				case HASH_MD5:    mbedtls_md5_update(&md5_ctx, buf, count);       break;
				case HASH_SHA1:   mbedtls_sha1_update(&sha1_ctx, buf, count);     break;
				case HASH_SHA256: mbedtls_sha256_update(&sha256_ctx, buf, count); break;
				case HASH_SHA512: mbedtls_sha512_update(&sha512_ctx, buf, count); break;
				case HASH_CRC32:  crc = crc32_update(crc, buf, count);            break;
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	FSClose(refNum);

	/* finish */
	switch (type)
	{
		case HASH_MD5:
			mbedtls_md5_finish(&md5_ctx, digest);
			mbedtls_md5_free(&md5_ctx);
			digest_len = 16;
			break;
		case HASH_SHA1:
			mbedtls_sha1_finish(&sha1_ctx, digest);
			mbedtls_sha1_free(&sha1_ctx);
			digest_len = 20;
			break;
		case HASH_SHA256:
			mbedtls_sha256_finish(&sha256_ctx, digest);
			mbedtls_sha256_free(&sha256_ctx);
			digest_len = 32;
			break;
		case HASH_SHA512:
			mbedtls_sha512_finish(&sha512_ctx, digest);
			mbedtls_sha512_free(&sha512_ctx);
			digest_len = 64;
			break;
		case HASH_CRC32:
			crc ^= 0xFFFFFFFFUL;
			digest[0] = (unsigned char)(crc >> 24);
			digest[1] = (unsigned char)(crc >> 16);
			digest[2] = (unsigned char)(crc >> 8);
			digest[3] = (unsigned char)(crc);
			digest_len = 4;
			break;
		default:
			digest_len = 0;
			break;
	}

	/* format hex string */
	for (i = 0; i < digest_len; i++)
	{
		static const char hx[] = "0123456789abcdef";
		hex[i * 2]     = hx[(digest[i] >> 4) & 0x0F];
		hex[i * 2 + 1] = hx[digest[i] & 0x0F];
	}
	hex[digest_len * 2] = '\0';

	vt_write(idx, hex);
	vt_write(idx, "  ");
	vt_write(idx, argv[1]);
	vt_write(idx, "\r\n");
}

static void cmd_md5sum(int idx, int argc, char** argv)    { cmd_hash(idx, argc, argv, HASH_MD5); }
static void cmd_sha1sum(int idx, int argc, char** argv)   { cmd_hash(idx, argc, argv, HASH_SHA1); }
static void cmd_sha256sum(int idx, int argc, char** argv) { cmd_hash(idx, argc, argv, HASH_SHA256); }
static void cmd_sha512sum(int idx, int argc, char** argv) { cmd_hash(idx, argc, argv, HASH_SHA512); }
static void cmd_crc32(int idx, int argc, char** argv)     { cmd_hash(idx, argc, argv, HASH_CRC32); }

/* ------------------------------------------------------------------ */
/* wc - line/word/byte count                                          */
/* ------------------------------------------------------------------ */

static void cmd_wc(int idx, int argc, char** argv)
{
	int show_lines = 0, show_words = 0, show_bytes = 0;
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	long lines = 0, words = 0, bytes = 0;
	int in_word = 0;
	int prev_cr = 0;
	long i;
	char num[16];

	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-' && argv[argi][1] != '\0')
		{
			const char* f = &argv[argi][1];
			while (*f)
			{
				if (*f == 'l') show_lines = 1;
				else if (*f == 'w') show_words = 1;
				else if (*f == 'c') show_bytes = 1;
				f++;
			}
		}
		else
		{
			filename = argv[argi];
		}
	}

	if (!filename)
	{
		vt_write(idx, "usage: wc [-lwc] <file>\r\n");
		return;
	}

	/* no flags = show all */
	if (!show_lines && !show_words && !show_bytes)
	{
		show_lines = 1;
		show_words = 1;
		show_bytes = 1;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "wc: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "wc: cannot open file\r\n");
		return;
	}

	while (1)
	{
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		if (count > 0)
		{
			bytes += count;
			for (i = 0; i < count; i++)
			{
				unsigned char c = buf[i];

				/* count lines: \n, or \r not followed by \n */
				if (c == '\n')
				{
					lines++;
					prev_cr = 0;
				}
				else
				{
					if (prev_cr) lines++;
					prev_cr = (c == '\r');
				}

				/* count words: transitions from whitespace to non-whitespace */
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
					in_word = 0;
				else if (!in_word)
				{
					words++;
					in_word = 1;
				}
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	/* trailing CR counts as a line */
	if (prev_cr) lines++;

	FSClose(refNum);

	if (show_lines)
	{
		snprintf(num, sizeof(num), "%7ld", lines);
		vt_write(idx, num);
	}
	if (show_words)
	{
		snprintf(num, sizeof(num), "%8ld", words);
		vt_write(idx, num);
	}
	if (show_bytes)
	{
		snprintf(num, sizeof(num), "%8ld", bytes);
		vt_write(idx, num);
	}
	vt_write(idx, " ");
	vt_write(idx, filename);
	vt_write(idx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* rot13 - ROT13 encode/decode file                                   */
/* ------------------------------------------------------------------ */

static void cmd_rot13(int idx, int argc, char** argv)
{
	FSSpec spec;
	OSErr e;
	short refNum;
	char buf[512];
	long count, i;

	if (argc < 2)
	{
		vt_write(idx, "usage: rot13 <file>\r\n");
		return;
	}

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "rot13: file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "rot13: cannot open file\r\n");
		return;
	}

	while (1)
	{
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		if (count > 0)
		{
			for (i = 0; i < count; i++)
			{
				char c = buf[i];
				if (c >= 'A' && c <= 'Z')
					c = 'A' + ((c - 'A' + 13) % 26);
				else if (c >= 'a' && c <= 'z')
					c = 'a' + ((c - 'a' + 13) % 26);

				if (c == '\r')
					vt_write(idx, "\r\n");
				else if (c == '\n')
					; /* skip bare LF (CR already handled) */
				else
					vt_char(idx, c);
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	FSClose(refNum);
	vt_write(idx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* grep - fixed-string search in files                                */
/* ------------------------------------------------------------------ */

/* case-insensitive strstr */
static const char* stristr(const char* haystack, const char* needle)
{
	size_t nlen;
	if (!*needle) return haystack;
	nlen = strlen(needle);
	while (*haystack)
	{
		size_t i;
		int match = 1;
		for (i = 0; i < nlen; i++)
		{
			if (haystack[i] == '\0') return NULL;
			if (tolower((unsigned char)haystack[i]) != tolower((unsigned char)needle[i]))
			{
				match = 0;
				break;
			}
		}
		if (match) return haystack;
		haystack++;
	}
	return NULL;
}

static void cmd_grep(int idx, int argc, char** argv)
{
	int opt_i = 0, opt_v = 0, opt_n = 0, opt_c = 0;
	const char* pattern = NULL;
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	/* line assembly buffer */
	char line[1024];
	int line_len = 0;
	long line_num = 0;
	long match_count = 0;
	char num[16];
	int prev_cr = 0;

	/* parse flags */
	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-' && argv[argi][1] != '\0')
		{
			const char* f = &argv[argi][1];
			while (*f)
			{
				if (*f == 'i') opt_i = 1;
				else if (*f == 'v') opt_v = 1;
				else if (*f == 'n') opt_n = 1;
				else if (*f == 'c') opt_c = 1;
				f++;
			}
		}
		else if (!pattern)
			pattern = argv[argi];
		else if (!filename)
			filename = argv[argi];
	}

	if (!pattern || !filename)
	{
		vt_write(idx, "usage: grep [-ivnc] <pattern> <file>\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "grep: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "grep: cannot open file\r\n");
		return;
	}

	while (1)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count; i++)
		{
			unsigned char c = buf[i];

			/* skip \n that follows \r from previous buffer */
			if (prev_cr && c == '\n')
			{
				prev_cr = 0;
				continue;
			}
			prev_cr = 0;

			/* end of line? (\r, \n, or \r\n) */
			if (c == '\r' || c == '\n')
			{
				const char* found;
				int matched;

				/* skip \n after \r within same buffer */
				if (c == '\r')
				{
					if (i + 1 < count && buf[i + 1] == '\n')
						i++;
					else if (i + 1 == count)
						prev_cr = 1;
				}

				line[line_len] = '\0';
				line_num++;

				found = opt_i ? stristr(line, pattern) : strstr(line, pattern);
				matched = found ? 1 : 0;
				if (opt_v) matched = !matched;

				if (matched)
				{
					match_count++;
					if (!opt_c)
					{
						if (opt_n)
						{
							snprintf(num, sizeof(num), "%ld:", line_num);
							vt_write(idx, num);
						}
						vt_write(idx, line);
						vt_write(idx, "\r\n");
					}
				}

				line_len = 0;
			}
			else
			{
				if (line_len < (int)sizeof(line) - 1)
					line[line_len++] = c;
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	/* handle last line without trailing newline */
	if (line_len > 0)
	{
		const char* found;
		int matched;
		line[line_len] = '\0';
		line_num++;

		found = opt_i ? stristr(line, pattern) : strstr(line, pattern);
		matched = found ? 1 : 0;
		if (opt_v) matched = !matched;

		if (matched)
		{
			match_count++;
			if (!opt_c)
			{
				if (opt_n)
				{
					snprintf(num, sizeof(num), "%ld:", line_num);
					vt_write(idx, num);
				}
				vt_write(idx, line);
				vt_write(idx, "\r\n");
			}
		}
	}

	if (opt_c)
	{
		snprintf(num, sizeof(num), "%ld", match_count);
		vt_write(idx, num);
		vt_write(idx, "\r\n");
	}

	FSClose(refNum);
}

/* ------------------------------------------------------------------ */
/* head / tail - show first/last N lines of a file                    */
/* ------------------------------------------------------------------ */

static void cmd_head(int idx, int argc, char** argv)
{
	int num_lines = 10;
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	int cur_line = 0;

	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-' && argv[argi][1] >= '0' && argv[argi][1] <= '9')
			num_lines = atoi(&argv[argi][1]);
		else if (strcmp(argv[argi], "-n") == 0 && argi + 1 < argc)
			num_lines = atoi(argv[++argi]);
		else
			filename = argv[argi];
	}

	if (!filename)
	{
		vt_write(idx, "usage: head [-n N] <file>\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "head: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "head: cannot open file\r\n");
		return;
	}

	while (cur_line < num_lines)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count && cur_line < num_lines; i++)
		{
			unsigned char c = buf[i];
			if (c == '\r')
			{
				vt_write(idx, "\r\n");
				cur_line++;
				/* skip \n after \r */
				if (i + 1 < count && buf[i + 1] == '\n')
					i++;
			}
			else if (c == '\n')
			{
				vt_write(idx, "\r\n");
				cur_line++;
			}
			else
			{
				vt_char(idx, c);
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	FSClose(refNum);
}

static void cmd_tail(int idx, int argc, char** argv)
{
	int num_lines = 10;
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	/* ring buffer of line start offsets */
	#define TAIL_MAX 256
	long offsets[TAIL_MAX + 1]; /* stores start-of-line file offsets */
	int off_head = 0; /* next write slot */
	int off_count = 0;
	long file_pos = 0;

	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-' && argv[argi][1] >= '0' && argv[argi][1] <= '9')
			num_lines = atoi(&argv[argi][1]);
		else if (strcmp(argv[argi], "-n") == 0 && argi + 1 < argc)
			num_lines = atoi(argv[++argi]);
		else
			filename = argv[argi];
	}

	if (num_lines < 0) num_lines = 0;
	if (num_lines > TAIL_MAX) num_lines = TAIL_MAX;

	if (!filename)
	{
		vt_write(idx, "usage: tail [-n N] <file>\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "tail: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "tail: cannot open file\r\n");
		return;
	}

	/* first pass: record line-start offsets in a ring buffer */
	offsets[0] = 0; /* file starts at a line */
	off_count = 1;
	off_head = 1;

	while (1)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count; i++)
		{
			unsigned char c = buf[i];
			file_pos++;
			if (c == '\r')
			{
				/* skip \n after \r */
				if (i + 1 < count && buf[i + 1] == '\n')
				{
					i++;
					file_pos++;
				}
				offsets[off_head] = file_pos;
				off_head = (off_head + 1) % (TAIL_MAX + 1);
				if (off_count < TAIL_MAX + 1) off_count++;
			}
			else if (c == '\n')
			{
				offsets[off_head] = file_pos;
				off_head = (off_head + 1) % (TAIL_MAX + 1);
				if (off_count < TAIL_MAX + 1) off_count++;
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	/* if last recorded offset == file size, it's a phantom empty line after
	   a trailing newline — remove it so tail -n 1 doesn't show nothing */
	if (off_count > 1)
	{
		int last = (off_head - 1 + (TAIL_MAX + 1)) % (TAIL_MAX + 1);
		if (offsets[last] == file_pos)
		{
			off_head = last;
			off_count--;
		}
	}

	/* seek to the right line and output */
	if (num_lines == 0)
	{
		FSClose(refNum);
		return;
	}
	{
		long seek_off;
		int start_idx;

		if (off_count <= num_lines)
			seek_off = 0;
		else
		{
			start_idx = (off_head - num_lines + (TAIL_MAX + 1)) % (TAIL_MAX + 1);
			seek_off = offsets[start_idx];
		}

		SetFPos(refNum, fsFromStart, seek_off);

		while (1)
		{
			long i;
			count = sizeof(buf);
			e = FSRead(refNum, &count, buf);
			for (i = 0; i < count; i++)
			{
				unsigned char c = buf[i];
				if (c == '\r')
				{
					vt_write(idx, "\r\n");
					if (i + 1 < count && buf[i + 1] == '\n')
						i++;
				}
				else if (c == '\n')
				{
					vt_write(idx, "\r\n");
				}
				else
				{
					vt_char(idx, c);
				}
			}
			if (e == eofErr || e != noErr) break;
		}
	}

	FSClose(refNum);
	#undef TAIL_MAX
}

/* ------------------------------------------------------------------ */
/* hexdump - hex + ASCII dump of a file                               */
/* ------------------------------------------------------------------ */

static void cmd_hexdump(int idx, int argc, char** argv)
{
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[16];
	long count;
	long offset = 0;
	char hex[8];
	int i;

	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] != '-')
		{
			filename = argv[argi];
			break;
		}
	}

	if (!filename)
	{
		vt_write(idx, "usage: hexdump <file>\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "hexdump: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "hexdump: cannot open file\r\n");
		return;
	}

	while (1)
	{
		count = 16;
		e = FSRead(refNum, &count, buf);
		if (count <= 0) break;

		/* offset */
		snprintf(hex, sizeof(hex), "%07lx", offset);
		vt_write(idx, hex);
		vt_write(idx, "  ");

		/* hex bytes */
		for (i = 0; i < 16; i++)
		{
			if (i < count)
			{
				static const char hx[] = "0123456789abcdef";
				char h[3];
				h[0] = hx[(buf[i] >> 4) & 0x0F];
				h[1] = hx[buf[i] & 0x0F];
				h[2] = '\0';
				vt_write(idx, h);
			}
			else
			{
				vt_write(idx, "  ");
			}
			if (i == 7)
				vt_write(idx, "  ");
			else
				vt_write(idx, " ");
		}

		vt_write(idx, " |");

		/* ASCII */
		for (i = 0; i < count; i++)
		{
			if (buf[i] >= 0x20 && buf[i] <= 0x7E)
				vt_char(idx, buf[i]);
			else
				vt_char(idx, '.');
		}

		vt_write(idx, "|\r\n");
		offset += count;

		if (e == eofErr || e != noErr) break;
	}

	/* final offset line (like hexdump -C) */
	snprintf(hex, sizeof(hex), "%07lx", offset);
	vt_write(idx, hex);
	vt_write(idx, "\r\n");

	FSClose(refNum);
}

/* ------------------------------------------------------------------ */
/* strings - extract printable strings from a file                    */
/* ------------------------------------------------------------------ */

static void cmd_strings(int idx, int argc, char** argv)
{
	int min_len = 4;
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	char cur[256];
	int cur_len = 0;

	for (argi = 1; argi < argc; argi++)
	{
		if (argv[argi][0] == '-' && argv[argi][1] == 'n' && argi + 1 < argc)
			min_len = atoi(argv[++argi]);
		else if (argv[argi][0] == '-' && argv[argi][1] >= '0' && argv[argi][1] <= '9')
			min_len = atoi(&argv[argi][1]);
		else
			filename = argv[argi];
	}

	if (min_len < 1) min_len = 1;

	if (!filename)
	{
		vt_write(idx, "usage: strings [-n N] <file>\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "strings: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "strings: cannot open file\r\n");
		return;
	}

	while (1)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count; i++)
		{
			unsigned char c = buf[i];
			if (c >= 0x20 && c <= 0x7E)
			{
				if (cur_len < (int)sizeof(cur) - 1)
					cur[cur_len++] = c;
			}
			else
			{
				if (cur_len >= min_len)
				{
					cur[cur_len] = '\0';
					vt_write(idx, cur);
					vt_write(idx, "\r\n");
				}
				cur_len = 0;
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	/* flush last string */
	if (cur_len >= min_len)
	{
		cur[cur_len] = '\0';
		vt_write(idx, cur);
		vt_write(idx, "\r\n");
	}

	FSClose(refNum);
}

/* ------------------------------------------------------------------ */
/* uptime - show time since boot                                      */
/* ------------------------------------------------------------------ */

static void cmd_uptime(int idx, int argc, char** argv)
{
	unsigned long ticks = TickCount();
	unsigned long secs = ticks / 60;
	unsigned long days = secs / 86400;
	unsigned long hours = (secs % 86400) / 3600;
	unsigned long mins = (secs % 3600) / 60;
	char buf[64];

	(void)argc;
	(void)argv;

	if (days > 0)
		snprintf(buf, sizeof(buf), "up %lu day%s, %lu:%02lu\r\n",
		         days, days == 1 ? "" : "s", hours, mins);
	else
		snprintf(buf, sizeof(buf), "up %lu:%02lu\r\n", hours, mins);
	vt_write(idx, buf);
}

/* ------------------------------------------------------------------ */
/* cal - display calendar for current month                           */
/* ------------------------------------------------------------------ */

static const char* cal_month_names[] = {
	"", "January", "February", "March", "April",
	"May", "June", "July", "August", "September",
	"October", "November", "December"
};
static const int cal_mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};

static int cal_days_in_month(int month, int year)
{
	int d = cal_mdays[month];
	if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
		d = 29;
	return d;
}

static int cal_dow_first(int month, int year)
{
	int y = year, m = month, z;
	if (m < 3) { m += 12; y--; }
	z = (1 + (13 * (m + 1)) / 5 + y + y / 4 - y / 100 + y / 400) % 7;
	return (z + 6) % 7; /* 0=Sun */
}

/* render one month into a grid: rows x 20 chars.
   grid[row] is a 22-char buffer. Returns number of rows used.
   today_row/today_col: set to the grid position of today's date (col = char offset). */
static int cal_render_month(int month, int year, int today,
                            char grid[8][22], int* today_row, int* today_col)
{
	int dow = cal_dow_first(month, year);
	int dim = cal_days_in_month(month, year);
	int row = 0, col, day;
	char hdr[22];
	int hlen, pad;

	*today_row = -1;
	*today_col = -1;

	/* row 0: centered month name */
	snprintf(hdr, sizeof(hdr), "%s %d", cal_month_names[month], year);
	hlen = strlen(hdr);
	pad = (20 - hlen) / 2;
	if (pad < 0) pad = 0;
	memset(grid[row], ' ', 20);
	grid[row][20] = '\0';
	memcpy(grid[row] + pad, hdr, hlen);
	row++;

	/* row 1: day-of-week header */
	strcpy(grid[row], "Su Mo Tu We Th Fr Sa");
	row++;

	/* init day grid row */
	memset(grid[row], ' ', 20);
	grid[row][20] = '\0';

	day = 1;
	col = dow;
	while (day <= dim)
	{
		grid[row][col * 3]     = (day >= 10) ? ('0' + day / 10) : ' ';
		grid[row][col * 3 + 1] = '0' + day % 10;

		if (day == today)
		{
			*today_row = row;
			*today_col = col * 3;
		}

		col++;
		if (col == 7 && day < dim)
		{
			col = 0;
			row++;
			memset(grid[row], ' ', 20);
			grid[row][20] = '\0';
		}
		day++;
	}
	row++;
	return row;
}

static void cmd_cal(int idx, int argc, char** argv)
{
	unsigned long secs;
	DateTimeRec dt;
	int year, month;
	char buf[32];

	GetDateTime(&secs);
	SecondsToDate(secs, &dt);

	if (argc >= 2)
	{
		int val = atoi(argv[1]);
		if (argc == 2 && val > 12 && val <= 9999)
		{
			/* cal <year> — show full year, 3 months per row */
			int row_group;
			char line[128];

			year = val;

			/* year header */
			snprintf(buf, sizeof(buf), "%d", year);
			{
				int blen = strlen(buf);
				int pad = (64 - blen) / 2;
				int i;
				for (i = 0; i < pad; i++) line[i] = ' ';
				memcpy(line + pad, buf, blen);
				line[pad + blen] = '\0';
			}
			vt_write(idx, line);
			vt_write(idx, "\r\n\r\n");

			for (row_group = 0; row_group < 4; row_group++)
			{
				char g0[8][22], g1[8][22], g2[8][22];
				int r0, r1, r2, maxr, r;
				int t0r, t0c, t1r, t1c, t2r, t2c;
				int m0 = row_group * 3 + 1;
				int m1 = m0 + 1;
				int m2 = m0 + 2;

				r0 = cal_render_month(m0, year,
					(year == dt.year && m0 == dt.month) ? dt.day : 0, g0, &t0r, &t0c);
				r1 = cal_render_month(m1, year,
					(year == dt.year && m1 == dt.month) ? dt.day : 0, g1, &t1r, &t1c);
				r2 = cal_render_month(m2, year,
					(year == dt.year && m2 == dt.month) ? dt.day : 0, g2, &t2r, &t2c);

				maxr = r0;
				if (r1 > maxr) maxr = r1;
				if (r2 > maxr) maxr = r2;

				/* pad shorter grids with blank rows */
				for (r = r0; r < maxr; r++)
				{
					memset(g0[r], ' ', 20);
					g0[r][20] = '\0';
				}
				for (r = r1; r < maxr; r++)
				{
					memset(g1[r], ' ', 20);
					g1[r][20] = '\0';
				}
				for (r = r2; r < maxr; r++)
				{
					memset(g2[r], ' ', 20);
					g2[r][20] = '\0';
				}

				for (r = 0; r < maxr; r++)
				{
					/* emit each month column, highlighting today */
					char* grids[3];
					int trows[3], tcols[3];
					int gi;
					grids[0] = g0[r]; grids[1] = g1[r]; grids[2] = g2[r];
					trows[0] = t0r; trows[1] = t1r; trows[2] = t2r;
					tcols[0] = t0c; tcols[1] = t1c; tcols[2] = t2c;

					for (gi = 0; gi < 3; gi++)
					{
						if (gi > 0) vt_write(idx, "  ");
						if (trows[gi] == r && tcols[gi] >= 0)
						{
							/* print chars before today */
							char tmp[22];
							int tc = tcols[gi];
							memcpy(tmp, grids[gi], tc);
							tmp[tc] = '\0';
							vt_write(idx, tmp);
							/* today in reverse */
							tmp[0] = grids[gi][tc];
							tmp[1] = grids[gi][tc + 1];
							tmp[2] = '\0';
							vt_write(idx, "\033[7m");
							vt_write(idx, tmp);
							vt_write(idx, "\033[0m");
							/* rest of row */
							if (tc + 2 < 20)
							{
								memcpy(tmp, grids[gi] + tc + 2, 20 - tc - 2);
								tmp[20 - tc - 2] = '\0';
								vt_write(idx, tmp);
							}
						}
						else
						{
							char tmp[22];
							memcpy(tmp, grids[gi], 20);
							tmp[20] = '\0';
							vt_write(idx, tmp);
						}
					}
					vt_write(idx, "\r\n");
				}
				if (row_group < 3)
					vt_write(idx, "\r\n");
			}
			return;
		}

		/* cal <month> [year] */
		month = val;
		if (month < 1 || month > 12)
		{
			vt_write(idx, "cal: invalid month\r\n");
			return;
		}
		year = (argc >= 3) ? atoi(argv[2]) : dt.year;
	}
	else
	{
		year = dt.year;
		month = dt.month;
	}

	/* single month display */
	{
		int dow = cal_dow_first(month, year);
		int dim = cal_days_in_month(month, year);
		int day, s;

		snprintf(buf, sizeof(buf), "   %s %d", cal_month_names[month], year);
		vt_write(idx, buf);
		vt_write(idx, "\r\n");
		vt_write(idx, "Su Mo Tu We Th Fr Sa\r\n");

		for (s = 0; s < dow; s++)
			vt_write(idx, "   ");

		for (day = 1; day <= dim; day++)
		{
			if (day == dt.day && month == dt.month && year == dt.year)
				snprintf(buf, sizeof(buf), "\033[7m%2d\033[0m", day);
			else
				snprintf(buf, sizeof(buf), "%2d", day);
			vt_write(idx, buf);

			if ((dow + day) % 7 == 0)
				vt_write(idx, "\r\n");
			else
				vt_write(idx, " ");
		}

		if ((dow + dim) % 7 != 0)
			vt_write(idx, "\r\n");
	}
}

/* ------------------------------------------------------------------ */
/* history - show shell command history                                */
/* ------------------------------------------------------------------ */

static void cmd_history(int idx, int argc, char** argv)
{
	struct session* s = &sessions[idx];
	int total = s->shell_history_count;
	int avail = total < SHELL_HISTORY_SIZE ? total : SHELL_HISTORY_SIZE;
	int start = total - avail;
	int i;
	char num[16];

	(void)argc;
	(void)argv;

	for (i = 0; i < avail; i++)
	{
		int hi = (start + i) % SHELL_HISTORY_SIZE;
		snprintf(num, sizeof(num), "%4d  ", start + i + 1);
		vt_write(idx, num);
		vt_write(idx, s->shell_history[hi]);
		vt_write(idx, "\r\n");
	}
}

/* ------------------------------------------------------------------ */
/* basename / dirname - path component extraction                     */
/* ------------------------------------------------------------------ */

static void cmd_basename(int idx, int argc, char** argv)
{
	const char* p;
	const char* last;

	if (argc < 2)
	{
		vt_write(idx, "usage: basename <path>\r\n");
		return;
	}

	/* find last separator (/ or :) */
	p = argv[1];
	last = p;
	while (*p)
	{
		if (*p == '/' || *p == ':')
			last = p + 1;
		p++;
	}
	vt_write(idx, last);
	vt_write(idx, "\r\n");
}

static void cmd_dirname(int idx, int argc, char** argv)
{
	const char* p;
	int last_sep = -1;
	int i;

	if (argc < 2)
	{
		vt_write(idx, "usage: dirname <path>\r\n");
		return;
	}

	p = argv[1];
	for (i = 0; p[i]; i++)
	{
		if (p[i] == '/' || p[i] == ':')
			last_sep = i;
	}

	if (last_sep < 0)
		vt_write(idx, ".");
	else if (last_sep == 0)
		vt_char(idx, p[0]);
	else
		vt_write_n(idx, p, last_sep);

	vt_write(idx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* sleep - wait N seconds                                             */
/* ------------------------------------------------------------------ */

static void cmd_sleep(int idx, int argc, char** argv)
{
	long secs;
	unsigned long dummy;

	if (argc < 2)
	{
		vt_write(idx, "usage: sleep <seconds>\r\n");
		return;
	}

	secs = atol(argv[1]);
	if (secs > 0)
		Delay(secs * 60, &dummy);
}

/* ------------------------------------------------------------------ */
/* nl - number lines                                                  */
/* ------------------------------------------------------------------ */

static void cmd_nl(int idx, int argc, char** argv)
{
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	long line_num = 1;
	int at_start = 1;
	char num[16];

	if (argc < 2)
	{
		vt_write(idx, "usage: nl <file>\r\n");
		return;
	}

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "nl: file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "nl: cannot open file\r\n");
		return;
	}

	while (1)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count; i++)
		{
			unsigned char c = buf[i];

			if (at_start)
			{
				snprintf(num, sizeof(num), "%6ld\t", line_num);
				vt_write(idx, num);
				at_start = 0;
			}

			if (c == '\r')
			{
				vt_write(idx, "\r\n");
				line_num++;
				at_start = 1;
				if (i + 1 < count && buf[i + 1] == '\n')
					i++;
			}
			else if (c == '\n')
			{
				vt_write(idx, "\r\n");
				line_num++;
				at_start = 1;
			}
			else
			{
				vt_char(idx, c);
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	FSClose(refNum);
	if (!at_start)
		vt_write(idx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* cmp - compare two files byte-by-byte                               */
/* ------------------------------------------------------------------ */

static void cmd_cmp(int idx, int argc, char** argv)
{
	FSSpec spec1, spec2;
	OSErr e1, e2;
	short ref1, ref2;
	unsigned char buf1[512], buf2[512];
	long c1, c2, count;
	long offset = 0;
	long line = 1;

	if (argc < 3)
	{
		vt_write(idx, "usage: cmp <file1> <file2>\r\n");
		return;
	}

	e1 = resolve_path(idx, argv[1], &spec1);
	if (e1 != noErr)
	{
		vt_write(idx, "cmp: file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e2 = resolve_path(idx, argv[2], &spec2);
	if (e2 != noErr)
	{
		vt_write(idx, "cmp: file not found: ");
		vt_write(idx, argv[2]);
		vt_write(idx, "\r\n");
		return;
	}

	e1 = FSpOpenDF(&spec1, fsRdPerm, &ref1);
	if (e1 != noErr)
	{
		vt_write(idx, "cmp: cannot open ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e2 = FSpOpenDF(&spec2, fsRdPerm, &ref2);
	if (e2 != noErr)
	{
		FSClose(ref1);
		vt_write(idx, "cmp: cannot open ");
		vt_write(idx, argv[2]);
		vt_write(idx, "\r\n");
		return;
	}

	while (1)
	{
		long i;
		c1 = sizeof(buf1);
		c2 = sizeof(buf2);
		e1 = FSRead(ref1, &c1, buf1);
		e2 = FSRead(ref2, &c2, buf2);
		count = c1 < c2 ? c1 : c2;

		for (i = 0; i < count; i++)
		{
			if (buf1[i] != buf2[i])
			{
				char msg[80];
				snprintf(msg, sizeof(msg),
				         "%s %s differ: byte %ld, line %ld\r\n",
				         argv[1], argv[2], offset + i + 1, line);
				vt_write(idx, msg);
				FSClose(ref1);
				FSClose(ref2);
				return;
			}
			if (buf1[i] == '\n' || buf1[i] == '\r')
				line++;
			offset++;
		}

		if (c1 != c2)
		{
			vt_write(idx, "cmp: EOF on ");
			vt_write(idx, c1 < c2 ? argv[1] : argv[2]);
			vt_write(idx, "\r\n");
			break;
		}

		if ((e1 == eofErr || e1 != noErr) && (e2 == eofErr || e2 != noErr))
			break;
	}

	FSClose(ref1);
	FSClose(ref2);
}

/* ------------------------------------------------------------------ */
/* seq - print number sequence                                        */
/* ------------------------------------------------------------------ */

static void cmd_seq(int idx, int argc, char** argv)
{
	long first = 1, inc = 1, last;
	long i;
	char num[16];

	if (argc < 2)
	{
		vt_write(idx, "usage: seq [first [inc]] last\r\n");
		return;
	}

	if (argc == 2)
	{
		last = atol(argv[1]);
	}
	else if (argc == 3)
	{
		first = atol(argv[1]);
		last = atol(argv[2]);
	}
	else
	{
		first = atol(argv[1]);
		inc = atol(argv[2]);
		last = atol(argv[3]);
	}

	if (inc == 0) { vt_write(idx, "seq: zero increment\r\n"); return; }

	if (inc > 0)
	{
		for (i = first; i <= last; i += inc)
		{
			snprintf(num, sizeof(num), "%ld", i);
			vt_write(idx, num);
			vt_write(idx, "\r\n");
		}
	}
	else
	{
		for (i = first; i >= last; i += inc)
		{
			snprintf(num, sizeof(num), "%ld", i);
			vt_write(idx, num);
			vt_write(idx, "\r\n");
		}
	}
}

/* ------------------------------------------------------------------ */
/* rev - reverse each line of a file                                  */
/* ------------------------------------------------------------------ */

static void cmd_rev(int idx, int argc, char** argv)
{
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	char line[1024];
	int line_len = 0;

	if (argc < 2)
	{
		vt_write(idx, "usage: rev <file>\r\n");
		return;
	}

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "rev: file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "rev: cannot open file\r\n");
		return;
	}

	while (1)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count; i++)
		{
			unsigned char c = buf[i];
			if (c == '\r' || c == '\n')
			{
				int j;
				if (c == '\r' && i + 1 < count && buf[i + 1] == '\n')
					i++;
				for (j = line_len - 1; j >= 0; j--)
					vt_char(idx, line[j]);
				vt_write(idx, "\r\n");
				line_len = 0;
			}
			else
			{
				if (line_len < (int)sizeof(line) - 1)
					line[line_len++] = c;
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	/* flush last line */
	if (line_len > 0)
	{
		int j;
		for (j = line_len - 1; j >= 0; j--)
			vt_char(idx, line[j]);
		vt_write(idx, "\r\n");
	}

	FSClose(refNum);
}

/* ------------------------------------------------------------------ */
/* line ending converters: dos2unix, unix2dos, mac2unix, unix2mac     */
/* ------------------------------------------------------------------ */

enum lineconv_mode { LC_DOS2UNIX, LC_UNIX2DOS, LC_MAC2UNIX, LC_UNIX2MAC };

static void cmd_lineconv(int idx, int argc, char** argv, enum lineconv_mode mode)
{
	const char* name;
	FSSpec spec;
	OSErr e;
	short refNum;
	long fsize, wpos;
	unsigned char* data;
	unsigned char* out;
	long out_len = 0;
	long i;

	switch (mode)
	{
		case LC_DOS2UNIX: name = "dos2unix"; break;
		case LC_UNIX2DOS: name = "unix2dos"; break;
		case LC_MAC2UNIX: name = "mac2unix"; break;
		case LC_UNIX2MAC: name = "unix2mac"; break;
		default:          name = "lineconv"; break;
	}

	if (argc < 2)
	{
		vt_write(idx, "usage: ");
		vt_write(idx, name);
		vt_write(idx, " <file>\r\n");
		return;
	}

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, name);
		vt_write(idx, ": file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdWrPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, name);
		vt_write(idx, ": cannot open file\r\n");
		return;
	}

	/* get file size */
	GetEOF(refNum, &fsize);
	if (fsize == 0 || fsize > 262144L)
	{
		if (fsize > 262144L)
		{
			vt_write(idx, name);
			vt_write(idx, ": file too large (256K max)\r\n");
		}
		FSClose(refNum);
		return;
	}

	data = (unsigned char*)NewPtr(fsize);
	if (!data)
	{
		vt_write(idx, name);
		vt_write(idx, ": out of memory\r\n");
		FSClose(refNum);
		return;
	}

	/* worst case: every byte becomes 2 (LF→CRLF) */
	out = (unsigned char*)NewPtr(fsize * 2);
	if (!out)
	{
		DisposePtr((Ptr)data);
		vt_write(idx, name);
		vt_write(idx, ": out of memory\r\n");
		FSClose(refNum);
		return;
	}

	/* read entire file */
	{
		long rcount = fsize;
		FSRead(refNum, &rcount, data);
	}

	/* convert */
	for (i = 0; i < fsize; i++)
	{
		switch (mode)
		{
			case LC_DOS2UNIX:
				/* CRLF → LF (strip CR before LF) */
				if (data[i] == '\r' && i + 1 < fsize && data[i + 1] == '\n')
					; /* skip CR, the LF will be written next iteration */
				else
					out[out_len++] = data[i];
				break;

			case LC_UNIX2DOS:
				/* LF → CRLF (add CR before LF, skip if already CRLF) */
				if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r'))
				{
					out[out_len++] = '\r';
					out[out_len++] = '\n';
				}
				else
					out[out_len++] = data[i];
				break;

			case LC_MAC2UNIX:
				/* CR → LF (but not CR that's part of CRLF) */
				if (data[i] == '\r')
				{
					if (i + 1 < fsize && data[i + 1] == '\n')
						out[out_len++] = data[i]; /* keep CR in CRLF */
					else
						out[out_len++] = '\n';
				}
				else
					out[out_len++] = data[i];
				break;

			case LC_UNIX2MAC:
				/* LF → CR (but not LF that's part of CRLF) */
				if (data[i] == '\n')
				{
					if (i > 0 && data[i - 1] == '\r')
						out[out_len++] = data[i]; /* keep LF in CRLF */
					else
						out[out_len++] = '\r';
				}
				else
					out[out_len++] = data[i];
				break;
		}
	}

	/* write back */
	SetFPos(refNum, fsFromStart, 0);
	wpos = out_len;
	FSWrite(refNum, &wpos, out);
	SetEOF(refNum, out_len);

	FSClose(refNum);
	DisposePtr((Ptr)data);
	DisposePtr((Ptr)out);
}

static void cmd_dos2unix(int idx, int argc, char** argv) { cmd_lineconv(idx, argc, argv, LC_DOS2UNIX); }
static void cmd_unix2dos(int idx, int argc, char** argv) { cmd_lineconv(idx, argc, argv, LC_UNIX2DOS); }
static void cmd_mac2unix(int idx, int argc, char** argv) { cmd_lineconv(idx, argc, argv, LC_MAC2UNIX); }
static void cmd_unix2mac(int idx, int argc, char** argv) { cmd_lineconv(idx, argc, argv, LC_UNIX2MAC); }

/* ------------------------------------------------------------------ */
/* fold - wrap lines to a given width                                 */
/* ------------------------------------------------------------------ */

static void cmd_fold(int idx, int argc, char** argv)
{
	int width = 80;
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	int col = 0;

	for (argi = 1; argi < argc; argi++)
	{
		if (strcmp(argv[argi], "-w") == 0 && argi + 1 < argc)
			width = atoi(argv[++argi]);
		else if (argv[argi][0] == '-' && argv[argi][1] == 'w')
			width = atoi(&argv[argi][2]);
		else
			filename = argv[argi];
	}

	if (width < 1) width = 80;

	if (!filename)
	{
		vt_write(idx, "usage: fold [-w N] <file>\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "fold: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "fold: cannot open file\r\n");
		return;
	}

	while (1)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count; i++)
		{
			unsigned char c = buf[i];
			if (c == '\r')
			{
				vt_write(idx, "\r\n");
				col = 0;
				if (i + 1 < count && buf[i + 1] == '\n')
					i++;
			}
			else if (c == '\n')
			{
				vt_write(idx, "\r\n");
				col = 0;
			}
			else
			{
				if (col >= width)
				{
					vt_write(idx, "\r\n");
					col = 0;
				}
				vt_char(idx, c);
				col++;
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	FSClose(refNum);
	if (col > 0) vt_write(idx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* xxd - hex dump with optional reverse mode                          */
/* ------------------------------------------------------------------ */

static void cmd_xxd(int idx, int argc, char** argv)
{
	int reverse = 0;
	const char* filename = NULL;
	int argi;
	FSSpec spec;
	OSErr e;
	short refNum;

	for (argi = 1; argi < argc; argi++)
	{
		if (strcmp(argv[argi], "-r") == 0)
			reverse = 1;
		else
			filename = argv[argi];
	}

	if (!filename)
	{
		vt_write(idx, "usage: xxd [-r] <file>\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "xxd: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	if (!reverse)
	{
		/* forward: xxd-style hex dump */
		unsigned char buf[16];
		long count;
		long offset = 0;
		char hex[12];
		int i;

		e = FSpOpenDF(&spec, fsRdPerm, &refNum);
		if (e != noErr) { vt_write(idx, "xxd: cannot open file\r\n"); return; }

		while (1)
		{
			count = 16;
			e = FSRead(refNum, &count, buf);
			if (count <= 0) break;

			snprintf(hex, sizeof(hex), "%08lx: ", offset);
			vt_write(idx, hex);

			for (i = 0; i < 16; i++)
			{
				if (i < count)
				{
					static const char hx[] = "0123456789abcdef";
					char h[3];
					h[0] = hx[(buf[i] >> 4) & 0x0F];
					h[1] = hx[buf[i] & 0x0F];
					h[2] = '\0';
					vt_write(idx, h);
				}
				else
					vt_write(idx, "  ");

				if (i % 2 == 1) vt_write(idx, " ");
			}

			vt_write(idx, " ");

			for (i = 0; i < count; i++)
			{
				if (buf[i] >= 0x20 && buf[i] <= 0x7E)
					vt_char(idx, buf[i]);
				else
					vt_char(idx, '.');
			}

			vt_write(idx, "\r\n");
			offset += count;

			if (e == eofErr || e != noErr) break;
		}

		FSClose(refNum);
	}
	else
	{
		/* reverse: parse xxd output back to binary */
		/* read xxd text, write binary to <file>.bin */
		unsigned char buf[512];
		long count;
		FSSpec out_spec;
		short out_ref;
		Str255 out_name;

		e = FSpOpenDF(&spec, fsRdPerm, &refNum);
		if (e != noErr) { vt_write(idx, "xxd: cannot open file\r\n"); return; }

		/* build output filename: <name>.bin */
		{
			int slen;
			memcpy(out_name, spec.name, spec.name[0] + 1);
			slen = out_name[0];
			if (slen + 4 <= 31)
			{
				out_name[slen + 1] = '.';
				out_name[slen + 2] = 'b';
				out_name[slen + 3] = 'i';
				out_name[slen + 4] = 'n';
				out_name[0] = slen + 4;
			}
		}

		e = FSMakeFSSpec(spec.vRefNum, spec.parID, out_name, &out_spec);
		if (e == fnfErr)
			HCreate(spec.vRefNum, spec.parID, out_name, '????', '????');

		e = FSpOpenDF(&out_spec, fsRdWrPerm, &out_ref);
		if (e != noErr)
		{
			FSClose(refNum);
			vt_write(idx, "xxd: cannot create output file\r\n");
			return;
		}
		SetEOF(out_ref, 0);

		/* parse line by line */
		{
			char line[256];
			int line_len = 0;

			while (1)
			{
				long i;
				count = sizeof(buf);
				e = FSRead(refNum, &count, buf);
				for (i = 0; i < count; i++)
				{
					if (buf[i] == '\r' || buf[i] == '\n')
					{
						if (buf[i] == '\r' && i + 1 < count && buf[i + 1] == '\n')
							i++;

						/* parse hex after the colon */
						if (line_len > 0)
						{
							char* p = line;
							char* colon;
							unsigned char outbuf[16];
							long out_count = 0;

							line[line_len] = '\0';
							colon = strchr(p, ':');
							if (colon) p = colon + 1;

							while (*p)
							{
								int hi, lo, sp;
								/* skip spaces; two or more consecutive = ASCII column separator */
								sp = 0;
								while (*p == ' ') { p++; sp++; }
								if (*p == '\0' || sp >= 2) break;

								hi = -1; lo = -1;
								if (*p >= '0' && *p <= '9') hi = *p - '0';
								else if (*p >= 'a' && *p <= 'f') hi = *p - 'a' + 10;
								else if (*p >= 'A' && *p <= 'F') hi = *p - 'A' + 10;
								else break;
								p++;

								if (*p >= '0' && *p <= '9') lo = *p - '0';
								else if (*p >= 'a' && *p <= 'f') lo = *p - 'a' + 10;
								else if (*p >= 'A' && *p <= 'F') lo = *p - 'A' + 10;
								else break;
								p++;

								if (hi >= 0 && lo >= 0)
									outbuf[out_count++] = (unsigned char)((hi << 4) | lo);
							}

							if (out_count > 0)
								FSWrite(out_ref, &out_count, outbuf);
						}
						line_len = 0;
					}
					else
					{
						if (line_len < (int)sizeof(line) - 1)
							line[line_len++] = buf[i];
					}
				}
				if (e == eofErr || e != noErr) break;
			}
		}

		FSClose(out_ref);
		FSClose(refNum);

		/* print output filename */
		{
			char name_buf[32];
			int ni;
			for (ni = 0; ni < out_name[0]; ni++)
				name_buf[ni] = out_name[ni + 1];
			name_buf[out_name[0]] = '\0';
			vt_write(idx, "wrote: ");
			vt_write(idx, name_buf);
			vt_write(idx, "\r\n");
		}
	}
}

/* ------------------------------------------------------------------ */
/* cut - extract fields from lines                                    */
/* ------------------------------------------------------------------ */

static void cmd_cut(int idx, int argc, char** argv)
{
	char delim = '\t';
	const char* field_spec = NULL;
	const char* filename = NULL;
	int argi;
	/* parse field spec into a bitmask (fields 1-32) */
	unsigned long field_mask = 0;
	FSSpec spec;
	OSErr e;
	short refNum;
	unsigned char buf[512];
	long count;
	char line[1024];
	int line_len = 0;

	for (argi = 1; argi < argc; argi++)
	{
		if (strcmp(argv[argi], "-d") == 0 && argi + 1 < argc)
		{
			delim = argv[++argi][0];
		}
		else if (argv[argi][0] == '-' && argv[argi][1] == 'd')
		{
			delim = argv[argi][2];
		}
		else if (strcmp(argv[argi], "-f") == 0 && argi + 1 < argc)
		{
			field_spec = argv[++argi];
		}
		else if (argv[argi][0] == '-' && argv[argi][1] == 'f')
		{
			field_spec = &argv[argi][2];
		}
		else
		{
			filename = argv[argi];
		}
	}

	if (!field_spec || !filename)
	{
		vt_write(idx, "usage: cut -d<delim> -f<fields> <file>\r\n");
		vt_write(idx, "  fields: 1,3,5-7\r\n");
		return;
	}

	/* parse field spec: comma-separated, supports N-M ranges */
	{
		const char* p = field_spec;
		while (*p)
		{
			long a = 0, b = 0;
			while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
			if (*p == '-')
			{
				p++;
				while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; }
				if (b == 0) b = 32;
			}
			else
			{
				b = a;
			}
			if (a < 1) a = 1;
			if (b > 32) b = 32;
			{
				long f;
				for (f = a; f <= b; f++)
					field_mask |= (1UL << (f - 1));
			}
			if (*p == ',') p++;
		}
	}

	if (field_mask == 0)
	{
		vt_write(idx, "cut: invalid field spec\r\n");
		return;
	}

	e = resolve_path(idx, filename, &spec);
	if (e != noErr)
	{
		vt_write(idx, "cut: file not found: ");
		vt_write(idx, filename);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpOpenDF(&spec, fsRdPerm, &refNum);
	if (e != noErr)
	{
		vt_write(idx, "cut: cannot open file\r\n");
		return;
	}

	while (1)
	{
		long i;
		count = sizeof(buf);
		e = FSRead(refNum, &count, buf);
		for (i = 0; i < count; i++)
		{
			unsigned char c = buf[i];
			if (c == '\r' || c == '\n')
			{
				if (c == '\r' && i + 1 < count && buf[i + 1] == '\n')
					i++;

				/* process line */
				line[line_len] = '\0';
				{
					int field_num = 1;
					int first_out = 1;
					const char* p = line;

					while (*p || field_num <= 32)
					{
						const char* fstart = p;
						const char* fend;
						/* find end of field */
						while (*p && *p != delim) p++;
						fend = p;
						if (*p == delim) p++;

						if (field_num <= 32 && (field_mask & (1UL << (field_num - 1))))
						{
							if (!first_out)
								vt_char(idx, delim);
							vt_write_n(idx, fstart, fend - fstart);
							first_out = 0;
						}
						field_num++;
						if (*fend == '\0') break;
					}
				}
				vt_write(idx, "\r\n");
				line_len = 0;
			}
			else
			{
				if (line_len < (int)sizeof(line) - 1)
					line[line_len++] = c;
			}
		}
		if (e == eofErr || e != noErr) break;
	}

	/* handle last line */
	if (line_len > 0)
	{
		line[line_len] = '\0';
		{
			int field_num = 1;
			int first_out = 1;
			const char* p = line;

			while (*p || field_num <= 32)
			{
				const char* fstart = p;
				const char* fend;
				while (*p && *p != delim) p++;
				fend = p;
				if (*p == delim) p++;

				if (field_num <= 32 && (field_mask & (1UL << (field_num - 1))))
				{
					if (!first_out)
						vt_char(idx, delim);
					vt_write_n(idx, fstart, fend - fstart);
					first_out = 0;
				}
				field_num++;
				if (*fend == '\0') break;
			}
		}
		vt_write(idx, "\r\n");
	}

	FSClose(refNum);
}

/* ------------------------------------------------------------------ */
/* wget - HTTP file download with Mac type/creator detection          */
/* ------------------------------------------------------------------ */

/* extension -> type/creator mapping for common Mac files */
struct ext_typecreator {
	const char* ext;
	OSType type;
	OSType creator;
};

static const struct ext_typecreator ext_map[] = {
	/* archives */
	{ "sit",   'SIT!', 'SITx' },
	{ "sea",   'APPL', 'aust' },
	{ "hqx",   'TEXT', 'SITx' },
	{ "bin",   'mBIN', 'SITx' },
	{ "zip",   'ZIP ', 'SITx' },
	{ "gz",    'Gzip', 'SITx' },
	{ "tar",   'TARF', 'SITx' },
	{ "tgz",   'Gzip', 'SITx' },
	{ "z",     'ZIVU', 'SITx' },
	{ "uu",    'TEXT', 'SITx' },
	{ "cpt",   'PACT', 'CPCT' },
	{ "dd",    'DDf1', 'DDsk' },
	/* disk images */
	{ "img",   'rohd', 'ddsk' },
	{ "dsk",   'dImg', 'ddsk' },
	{ "dmg",   'dImg', 'ddsk' },
	{ "smi",   'APPL', 'ddsk' },
	{ "toast", 'CDr3', 'TOAS' },
	{ "iso",   'rodh', 'ddsk' },
	/* text */
	{ "txt",   'TEXT', 'ttxt' },
	{ "text",  'TEXT', 'ttxt' },
	{ "htm",   'TEXT', 'MOSS' },
	{ "html",  'TEXT', 'MOSS' },
	{ "css",   'TEXT', 'ttxt' },
	{ "js",    'TEXT', 'ttxt' },
	{ "xml",   'TEXT', 'ttxt' },
	{ "csv",   'TEXT', 'ttxt' },
	{ "c",     'TEXT', 'ttxt' },
	{ "h",     'TEXT', 'ttxt' },
	/* images */
	{ "gif",   'GIFf', 'ogle' },
	{ "jpg",   'JPEG', 'ogle' },
	{ "jpeg",  'JPEG', 'ogle' },
	{ "png",   'PNGf', 'ogle' },
	{ "bmp",   'BMPf', 'ogle' },
	{ "tif",   'TIFF', 'ogle' },
	{ "tiff",  'TIFF', 'ogle' },
	{ "pict",  'PICT', 'ogle' },
	/* audio */
	{ "aif",   'AIFF', 'SCPL' },
	{ "aiff",  'AIFF', 'SCPL' },
	{ "wav",   'WAVE', 'SCPL' },
	{ "mp3",   'MPG3', 'TVOD' },
	{ "mid",   'Midi', 'TVOD' },
	{ "midi",  'Midi', 'TVOD' },
	/* documents */
	{ "pdf",   'PDF ', 'CARO' },
	{ "doc",   'W8BN', 'MSWD' },
	{ "xls",   'XLS8', 'XCEL' },
	{ "ppt",   'SLD8', 'PPT3' },
	{ "rtf",   'TEXT', 'MSWD' },
	/* applications */
	{ "rsrc",  'rsrc', 'RSED' },
	{ "rom",   'ROM ', 'ttxt' },
	{ NULL, 0, 0 }
};

/* case-insensitive extension lookup */
static int lookup_ext_type(const char* filename, OSType* type, OSType* creator)
{
	const char* dot;
	const struct ext_typecreator* m;
	char ext[16];
	int i, len;

	/* find last dot */
	dot = NULL;
	{
		const char* p = filename;
		while (*p) { if (*p == '.') dot = p; p++; }
	}
	if (!dot) return 0;
	dot++; /* skip the dot */

	len = strlen(dot);
	if (len == 0 || len > 15) return 0;
	for (i = 0; i < len; i++)
		ext[i] = tolower((unsigned char)dot[i]);
	ext[len] = '\0';

	for (m = ext_map; m->ext; m++)
	{
		if (strcmp(ext, m->ext) == 0)
		{
			*type = m->type;
			*creator = m->creator;
			return 1;
		}
	}
	return 0;
}

/* check for MacBinary header and extract type/creator */
static int check_macbinary(const unsigned char* hdr, int hdr_len,
                           OSType* type, OSType* creator, long* data_len, long* rsrc_len)
{
	int name_len;

	if (hdr_len < 128) return 0;

	/* MacBinary I/II/III checks */
	if (hdr[0] != 0) return 0;        /* version must be 0 */
	name_len = hdr[1];
	if (name_len < 1 || name_len > 63) return 0;
	if (hdr[74] != 0) return 0;       /* must be zero */
	if (hdr[82] != 0) return 0;       /* must be zero */

	/* data fork length (bytes 83-86, big-endian) */
	*data_len = ((long)hdr[83] << 24) | ((long)hdr[84] << 16) |
	            ((long)hdr[85] << 8) | (long)hdr[86];
	/* resource fork length (bytes 87-90) */
	*rsrc_len = ((long)hdr[87] << 24) | ((long)hdr[88] << 16) |
	            ((long)hdr[89] << 8) | (long)hdr[90];

	/* sanity: lengths should be reasonable */
	if (*data_len < 0 || *rsrc_len < 0) return 0;
	if (*data_len > 16777216L || *rsrc_len > 16777216L) return 0;

	/* type (bytes 65-68) and creator (bytes 69-72) */
	*type    = ((OSType)hdr[65] << 24) | ((OSType)hdr[66] << 16) |
	           ((OSType)hdr[67] << 8) | (OSType)hdr[68];
	*creator = ((OSType)hdr[69] << 24) | ((OSType)hdr[70] << 16) |
	           ((OSType)hdr[71] << 8) | (OSType)hdr[72];

	return 1;
}

/* find a header value (case-insensitive); returns pointer into hdr or NULL */
static const char* http_header_find(const char* headers, const char* name)
{
	int name_len = strlen(name);
	const char* p = headers;
	while (*p)
	{
		/* start of a line */
		int i;
		int match = 1;
		for (i = 0; i < name_len; i++)
		{
			if (p[i] == '\0' ||
			    tolower((unsigned char)p[i]) != tolower((unsigned char)name[i]))
			{
				match = 0;
				break;
			}
		}
		if (match && p[name_len] == ':')
		{
			const char* val = p + name_len + 1;
			while (*val == ' ') val++;
			return val;
		}
		/* advance to next line */
		while (*p && *p != '\n') p++;
		if (*p == '\n') p++;
	}
	return NULL;
}

/* extract filename from URL path or Content-Disposition header */
static void extract_filename(const char* url_path, const char* headers,
                             char* out, int maxlen)
{
	const char* p;
	const char* name_start;

	/* try Content-Disposition first */
	p = http_header_find(headers, "content-disposition");
	if (p)
	{
		const char* fn = strstr(p, "filename=");
		if (!fn) fn = strstr(p, "filename*=");
		if (fn)
		{
			fn = strchr(fn, '=') + 1;
			if (*fn == '"') fn++;
			{
				int i = 0;
				while (*fn && *fn != '"' && *fn != '\r' && *fn != '\n' && i < maxlen - 1)
					out[i++] = *fn++;
				out[i] = '\0';
				if (i > 0) return;
			}
		}
	}

	/* fall back to URL path */
	name_start = url_path;
	p = url_path;
	while (*p)
	{
		if (*p == '/') name_start = p + 1;
		p++;
	}

	{
		int i = 0;
		while (*name_start && *name_start != '?' && *name_start != '#' && i < maxlen - 1)
			out[i++] = *name_start++;
		out[i] = '\0';
	}

	if (out[0] == '\0')
		strncpy(out, "download", maxlen - 1);
}

/* ---- TLS support for HTTPS ---- */

#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>

/* I/O callbacks for mbedtls over Open Transport */
static int wget_tls_send(void* ctx, const unsigned char* buf, size_t len)
{
	OTResult r;
	EndpointRef ep = (EndpointRef)ctx;
	r = OTSnd(ep, (void*)buf, len, 0);
	if (r == kOTFlowErr) return MBEDTLS_ERR_SSL_WANT_WRITE;
	if (r < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
	return (int)r;
}

static int wget_tls_recv(void* ctx, unsigned char* buf, size_t len)
{
	OTResult r;
	EndpointRef ep = (EndpointRef)ctx;
	r = OTRcv(ep, buf, len, nil);
	if (r == kOTNoDataErr) return MBEDTLS_ERR_SSL_WANT_READ;
	if (r == kOTLookErr)
	{
		/* check for orderly disconnect */
		OTResult ev = OTLook(ep);
		if (ev == T_ORDREL)
		{
			OTRcvOrderlyDisconnect(ep);
			return 0; /* clean EOF */
		}
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	if (r == 0) return 0; /* clean EOF */
	if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
	return (int)r;
}

static long wget_percent(long total_written, long content_length)
{
	if (content_length <= 0) return 0;
	if (total_written <= 0) return 0;
	if (total_written >= content_length) return 100;

	/* Avoid overflow from total_written * 100 on 32-bit long. */
	if (content_length >= 100)
	{
		long unit = content_length / 100;
		long pct = total_written / unit;
		if (pct > 100) pct = 100;
		return pct;
	}

	return (total_written * 100L) / content_length;
}

static int transfer_progress_step(int idx,
                                  long total_written,
                                  long content_length,
                                  long* next_progress_bytes,
                                  int* progress_live,
                                  int show_progress,
                                  int uploading)
{
	char pbuf[96];
	const long step = 524288L; /* 512 KB: lower UI overhead, higher throughput */
	const char* verb = uploading ? "Uploading" : "Downloading";

	if (total_written < *next_progress_bytes)
		return 0;

	*next_progress_bytes = total_written + step;

	if (!show_progress)
		return 1;

	if (content_length > 0)
	{
		long pct = wget_percent(total_written, content_length);
		snprintf(pbuf, sizeof(pbuf),
		         "\r%s %ld / %ld bytes (%ld%%)",
		         verb, total_written, content_length, pct);
	}
	else
	{
		snprintf(pbuf, sizeof(pbuf), "\r%s %ld bytes", verb, total_written);
	}

	vt_write(idx, pbuf);
	vt_write(idx, "\033[K");
	*progress_live = 1;
	return 1;
}

static void cmd_wget_run(int idx, const char* initial_url, int no_progress)
{
	const char* url = initial_url;
	char host[256];
	char path[512];
	unsigned short port;
	int use_tls = 0;
	EndpointRef ep;
	OSStatus err;
	TCall sndCall;
	DNSAddress hostDNSAddress;
	char hostport[280];
	char request[1024];
	int req_len;
	/* TLS state */
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config ssl_conf;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_entropy_context entropy;
	int tls_initialized = 0;
	/* response parsing */
	char resp_buf[4096]; /* header accumulation buffer */
	int resp_len = 0;
	int header_done = 0;
	int status_code = 0;
	long content_length = -1;
	char headers_str[2048];
	/* file output */
	char filename[64];
	char local_name[32]; /* HFS 31 char limit */
	Str255 pname;
	FSSpec out_spec;
	short out_ref = 0;
	struct session* s = &sessions[idx];
	long total_written = 0;
	int progress_live = 0;
	/* type/creator */
	OSType ftype = 'BINA';
	OSType fcreator = '????';
	/* MacBinary detection */
	unsigned char first_bytes[128];
	int first_bytes_len = 0;
	int checked_macbinary = 0;
	int download_ok = 0;
	/* redirect */
	int redirect_count = 0;
	char redirect_url[512];
	unsigned long download_start_tick = 0;
	unsigned long download_elapsed_ticks = 0;

retry_redirect:
	/* parse URL */
	{
		const char* p = url;
		int hi = 0;

		if (strncmp(p, "https://", 8) == 0)
		{
			use_tls = 1;
			port = 443;
			p += 8;
		}
		else if (strncmp(p, "http://", 7) == 0)
		{
			use_tls = 0;
			port = 80;
			p += 7;
		}
		else
		{
			use_tls = 0;
			port = 80;
		}

		/* extract host[:port] */
		host[0] = '\0';
		while (*p && *p != '/' && *p != ':' && hi < (int)sizeof(host) - 1)
			host[hi++] = *p++;
		host[hi] = '\0';

		if (*p == ':')
		{
			unsigned long pv = 0;
			int ndigits = 0;
			p++;
			while (*p >= '0' && *p <= '9')
			{
				pv = pv * 10 + (*p - '0');
				ndigits++;
				p++;
			}
			if (ndigits == 0 || pv == 0 || pv > 65535)
			{
				vt_write(idx, "wget: invalid port\r\n");
				return;
			}
			port = (unsigned short)pv;
		}

		/* path (default /) */
		if (*p == '/')
			strncpy(path, p, sizeof(path) - 1);
		else
			strcpy(path, "/");
		path[sizeof(path) - 1] = '\0';
	}

	if (host[0] == '\0')
	{
		vt_write(idx, "wget: invalid URL\r\n");
		return;
	}

	/* connect */
	if (InitOpenTransport() != noErr)
	{
		vt_write(idx, "wget: Open Transport not available\r\n");
		return;
	}

	ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, nil, &err);
	if (err != noErr)
	{
		printf_s(idx, "wget: failed to open endpoint (err=%d)\r\n", (int)err);
		return;
	}
	s->endpoint = ep;

	OTSetSynchronous(ep);
	OTSetBlocking(ep);
	OTInstallNotifier(ep, shell_ot_timeout_notifier, nil);
	OTUseSyncIdleEvents(ep, true);

	err = OTBind(ep, nil, nil);
	if (err != noErr)
	{
		printf_s(idx, "wget: bind failed (err=%d)\r\n", (int)err);
		OTCloseProvider(ep);
		s->endpoint = kOTInvalidEndpointRef;
		return;
	}

	snprintf(hostport, sizeof(hostport), "%s:%d", host, (int)port);

	OTMemzero(&sndCall, sizeof(TCall));
	sndCall.addr.buf = (UInt8*) &hostDNSAddress;
	sndCall.addr.len = OTInitDNSAddress(&hostDNSAddress, hostport);

	printf_s(idx, "Connecting to %s%s... ",
	         use_tls ? "(TLS) " : "", hostport);

	ot_timeout_provider = ep;
	ot_timeout_deadline = TickCount() + OT_TIMEOUT_TICKS;

	err = OTConnect(ep, &sndCall, nil);

	ot_timeout_deadline = 0;
	ot_timeout_provider = nil;

	if (err != noErr)
	{
		if (err == kOTCanceledErr)
			vt_write(idx, "timed out\r\n");
		else
			printf_s(idx, "failed (err=%d)\r\n", (int)err);
		OTUnbind(ep);
		OTCloseProvider(ep);
		s->endpoint = kOTInvalidEndpointRef;
		return;
	}

	vt_write(idx, "connected.\r\n");

	/* switch to non-blocking for handshake and recv */
	OTSetNonBlocking(ep);
	OTUseSyncIdleEvents(ep, false);

	/* TLS handshake if HTTPS */
	if (use_tls)
	{
		int ret;
		unsigned long hs_deadline = TickCount() + 1800; /* 30 sec */

		vt_write(idx, "TLS handshake... ");

		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&ssl_conf);
		mbedtls_ctr_drbg_init(&ctr_drbg);
		mbedtls_entropy_init(&entropy);

		mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

		mbedtls_ssl_config_defaults(&ssl_conf,
		                            MBEDTLS_SSL_IS_CLIENT,
		                            MBEDTLS_SSL_TRANSPORT_STREAM,
		                            MBEDTLS_SSL_PRESET_DEFAULT);

		/* skip cert verification (no CA store on classic Mac) */
		mbedtls_ssl_conf_authmode(&ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_rng(&ssl_conf, mbedtls_ctr_drbg_random, &ctr_drbg);

		/* force TLS 1.2 max — 1.3 is unreliable on 68K */
		mbedtls_ssl_conf_max_tls_version(&ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_min_tls_version(&ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);

		mbedtls_ssl_setup(&ssl, &ssl_conf);
		mbedtls_ssl_set_hostname(&ssl, host);

		/* set I/O callbacks */
		mbedtls_ssl_set_bio(&ssl, (void*)ep, wget_tls_send, wget_tls_recv, NULL);

		/* handshake (may need multiple round-trips) */
		while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
		{
			if (s->thread_command == EXIT || !s->in_use)
			{
				vt_write(idx, "cancelled\r\n");
				mbedtls_ssl_free(&ssl);
				mbedtls_ssl_config_free(&ssl_conf);
				mbedtls_ctr_drbg_free(&ctr_drbg);
				mbedtls_entropy_free(&entropy);
				OTSndOrderlyDisconnect(ep);
				OTUnbind(ep);
				OTCloseProvider(ep);
				s->endpoint = kOTInvalidEndpointRef;
				return;
			}

			if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			{
				if (TickCount() > hs_deadline)
				{
					vt_write(idx, "timed out\r\n");
					mbedtls_ssl_free(&ssl);
					mbedtls_ssl_config_free(&ssl_conf);
					mbedtls_ctr_drbg_free(&ctr_drbg);
					mbedtls_entropy_free(&entropy);
					OTSndOrderlyDisconnect(ep);
					OTUnbind(ep);
					OTCloseProvider(ep);
					s->endpoint = kOTInvalidEndpointRef;
					return;
				}
				YieldToAnyThread();
				continue;
			}
			printf_s(idx, "failed (err=-0x%04x)\r\n", (unsigned int)-ret);
			mbedtls_ssl_free(&ssl);
			mbedtls_ssl_config_free(&ssl_conf);
			mbedtls_ctr_drbg_free(&ctr_drbg);
			mbedtls_entropy_free(&entropy);
			OTSndOrderlyDisconnect(ep);
			OTUnbind(ep);
			OTCloseProvider(ep);
			s->endpoint = kOTInvalidEndpointRef;
			return;
		}

		vt_write(idx, "ok.\r\n");
		tls_initialized = 1;
	}

	/* send HTTP request */
	snprintf(request, sizeof(request),
	         "GET %s HTTP/1.0\r\n"
	         "Host: %s\r\n"
	         "User-Agent: SevenTTY/1.1\r\n"
	         "Connection: close\r\n"
	         "\r\n",
	         path, host);
	req_len = strlen(request);

	{
		int send_ok = 0;
		if (use_tls)
		{
			int sent = 0;
			while (sent < req_len)
			{
				if (s->thread_command == EXIT || !s->in_use) break;
				int ret = mbedtls_ssl_write(&ssl,
				          (unsigned char*)request + sent, req_len - sent);
				if (ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
				    ret == MBEDTLS_ERR_SSL_WANT_READ) { YieldToAnyThread(); continue; }
				if (ret <= 0) break;
				sent += ret;
			}
			send_ok = (sent == req_len);
		}
		else
		{
			int sent = 0;
			while (sent < req_len)
			{
				if (s->thread_command == EXIT || !s->in_use) break;
				OTResult r = OTSnd(ep, request + sent, req_len - sent, 0);
				if (r == kOTFlowErr) { YieldToAnyThread(); continue; }
				if (r < 0) break;
				sent += r;
			}
			send_ok = (sent == req_len);
		}

		if (!send_ok)
		{
			vt_write(idx, "wget: send failed\r\n");
			if (tls_initialized)
			{
				mbedtls_ssl_close_notify(&ssl);
				mbedtls_ssl_free(&ssl);
				mbedtls_ssl_config_free(&ssl_conf);
				mbedtls_ctr_drbg_free(&ctr_drbg);
				mbedtls_entropy_free(&entropy);
			}
			OTSndOrderlyDisconnect(ep);
			OTUnbind(ep);
			OTCloseProvider(ep);
			s->endpoint = kOTInvalidEndpointRef;
			return;
		}
	}

	printf_s(idx, "GET %s\r\n", path);

	/* already non-blocking from after connect */
	resp_len = 0;
	header_done = 0;
	headers_str[0] = '\0';

	{
		long next_progress_bytes = 524288L;
		long bytes_since_yield = 0;
		const long yield_step = 524288L; /* 512KB */
		unsigned long recv_deadline = TickCount() + 1800; /* 30 sec inactivity timeout */
		download_start_tick = TickCount();
		while (1)
		{
			int r;
			char buf[32768];
			unsigned long now = TickCount();

			if (s->thread_command == EXIT || !s->in_use)
			{
				vt_write(idx, "\r\nwget: cancelled\r\n");
				break;
			}

			if (now > recv_deadline)
			{
				vt_write(idx, "\r\nwget: receive timed out\r\n");
				break;
			}

			if (use_tls)
			{
				r = mbedtls_ssl_read(&ssl, (unsigned char*)buf, sizeof(buf));
				if (r == MBEDTLS_ERR_SSL_WANT_READ ||
				    r == MBEDTLS_ERR_SSL_WANT_WRITE)
				{ YieldToAnyThread(); continue; }
				if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0)
				{ download_ok = 1; break; }
				if (r < 0) break;
			}
			else
			{
				OTResult otr = OTRcv(ep, buf, sizeof(buf), nil);
				if (otr == kOTNoDataErr) { YieldToAnyThread(); continue; }
				if (otr == kOTLookErr)
				{
					OTResult ev = OTLook(ep);
					if (ev == T_ORDREL)
					{
						OTRcvOrderlyDisconnect(ep);
						download_ok = 1;
					}
					break;
				}
				if (otr == 0) { download_ok = 1; break; }
				if (otr < 0) break;
				r = (int)otr;
			}

			recv_deadline = TickCount() + 1800; /* reset on data received */

			if (!header_done)
			{
				/* accumulate into resp_buf to find end of headers */
				int copy = r;
				int overflow = 0; /* bytes from buf that didn't fit in resp_buf */
				if (resp_len + copy > (int)sizeof(resp_buf) - 1)
				{
					copy = (int)sizeof(resp_buf) - 1 - resp_len;
					overflow = r - copy;
				}
				if (copy <= 0)
				{
					vt_write(idx, "\r\nwget: response headers too large\r\n");
					break;
				}
				memcpy(resp_buf + resp_len, buf, copy);
				resp_len += copy;
				resp_buf[resp_len] = '\0';

				/* look for \r\n\r\n */
				{
					char* end = strstr(resp_buf, "\r\n\r\n");
					if (end)
					{
						int header_len = (end - resp_buf) + 4;
						int body_start = header_len;
						int body_bytes = resp_len - body_start;

						/* parse status line */
						{
							const char* sp = strchr(resp_buf, ' ');
							if (sp) status_code = atoi(sp + 1);
						}

						/* copy headers for later inspection */
						{
							int hcopy = header_len;
							if (hcopy > (int)sizeof(headers_str) - 1)
								hcopy = (int)sizeof(headers_str) - 1;
							memcpy(headers_str, resp_buf, hcopy);
							headers_str[hcopy] = '\0';
						}

						/* handle redirects */
						if (status_code >= 300 && status_code < 400)
						{
							const char* loc = http_header_find(headers_str, "location");
							if (loc && redirect_count < 5)
							{
								int li = 0;
								while (*loc && *loc != '\r' && *loc != '\n' && li < (int)sizeof(redirect_url) - 1)
									redirect_url[li++] = *loc++;
								redirect_url[li] = '\0';

								/* handle relative redirects */
								if (redirect_url[0] == '/' && redirect_url[1] == '/')
								{
									/* scheme-relative: //host/path */
									char abs_url[800];
									snprintf(abs_url, sizeof(abs_url), "%s%s",
									         use_tls ? "https:" : "http:",
									         redirect_url);
									strncpy(redirect_url, abs_url, sizeof(redirect_url) - 1);
									redirect_url[sizeof(redirect_url) - 1] = '\0';
								}
								else if (redirect_url[0] == '/')
								{
									/* absolute path: /path */
									char abs_url[800];
									snprintf(abs_url, sizeof(abs_url), "%s%s:%d%s",
									         use_tls ? "https://" : "http://",
									         host, (int)port, redirect_url);
									strncpy(redirect_url, abs_url, sizeof(redirect_url) - 1);
									redirect_url[sizeof(redirect_url) - 1] = '\0';
								}
								else if (strncmp(redirect_url, "http://", 7) != 0 &&
								         strncmp(redirect_url, "https://", 8) != 0)
								{
									/* bare relative: foo/bar -> resolve against current path */
									char abs_url[800];
									char base_path[512];
									char* last_slash;
									strncpy(base_path, path, sizeof(base_path) - 1);
									base_path[sizeof(base_path) - 1] = '\0';
									last_slash = strrchr(base_path, '/');
									if (last_slash) last_slash[1] = '\0';
									else strcpy(base_path, "/");
									snprintf(abs_url, sizeof(abs_url), "%s%s:%d%s%s",
									         use_tls ? "https://" : "http://",
									         host, (int)port, base_path, redirect_url);
									strncpy(redirect_url, abs_url, sizeof(redirect_url) - 1);
									redirect_url[sizeof(redirect_url) - 1] = '\0';
								}

								printf_s(idx, "Redirect %d -> %s\r\n", status_code, redirect_url);

								if (tls_initialized)
								{
									mbedtls_ssl_close_notify(&ssl);
									mbedtls_ssl_free(&ssl);
									mbedtls_ssl_config_free(&ssl_conf);
									mbedtls_ctr_drbg_free(&ctr_drbg);
									mbedtls_entropy_free(&entropy);
									tls_initialized = 0;
								}

								OTSndOrderlyDisconnect(ep);
								OTUnbind(ep);
								OTCloseProvider(ep);
								s->endpoint = kOTInvalidEndpointRef;

								url = redirect_url;
								redirect_count++;
								resp_len = 0;
								goto retry_redirect;
							}
						}

						if (status_code != 200)
						{
							printf_s(idx, "HTTP %d\r\n", status_code);
							break;
						}

						/* get content-length */
						{
							const char* cl = http_header_find(headers_str, "content-length");
							if (cl) content_length = atol(cl);
						}

						/* figure out filename */
						extract_filename(path, headers_str, filename, sizeof(filename));

						/* truncate to 31 chars for HFS */
						{
							int nlen = strlen(filename);
							if (nlen > 31) nlen = 31;
							memcpy(local_name, filename, nlen);
							local_name[nlen] = '\0';
						}

						printf_s(idx, "Saving to: %s", local_name);
						if (content_length >= 0)
							printf_s(idx, " (%ld bytes)", content_length);
						vt_write(idx, "\r\n");

						/* create output file */
						{
							int nlen = strlen(local_name);
							pname[0] = nlen;
							memcpy(pname + 1, local_name, nlen);
						}

						/* delete if exists */
						{
							FSSpec tmp;
							if (FSMakeFSSpec(s->shell_vRefNum, s->shell_dirID, pname, &tmp) == noErr)
								HDelete(s->shell_vRefNum, s->shell_dirID, pname);
						}

						HCreate(s->shell_vRefNum, s->shell_dirID, pname, ftype, fcreator);
						FSMakeFSSpec(s->shell_vRefNum, s->shell_dirID, pname, &out_spec);

						err = FSpOpenDF(&out_spec, fsRdWrPerm, &out_ref);
						if (err != noErr)
						{
							vt_write(idx, "wget: cannot create output file\r\n");
							break;
						}

						header_done = 1;

						/* write any body data already received */
						if (body_bytes > 0)
						{
							unsigned char* body_data = (unsigned char*)(resp_buf + body_start);
							long wcount = body_bytes;

							/* save first bytes for MacBinary check */
							if (first_bytes_len < 128)
							{
								int grab = 128 - first_bytes_len;
								if (grab > body_bytes) grab = body_bytes;
								memcpy(first_bytes + first_bytes_len, body_data, grab);
								first_bytes_len += grab;
							}

							FSWrite(out_ref, &wcount, body_data);
							total_written += wcount;
							bytes_since_yield += wcount;
							if (transfer_progress_step(idx, total_written, content_length,
							                           &next_progress_bytes, &progress_live,
							                           !no_progress, 0) ||
							    bytes_since_yield >= yield_step)
							{
								YieldToAnyThread();
								bytes_since_yield = 0;
							}
						}

						/* write overflow bytes that didn't fit in resp_buf */
						if (overflow > 0)
						{
							unsigned char* ovf_data = (unsigned char*)(buf + copy);
							long wcount = overflow;

							if (first_bytes_len < 128)
							{
								int grab = 128 - first_bytes_len;
								if (grab > overflow) grab = overflow;
								memcpy(first_bytes + first_bytes_len, ovf_data, grab);
								first_bytes_len += grab;
							}

							FSWrite(out_ref, &wcount, ovf_data);
							total_written += wcount;
							bytes_since_yield += wcount;
							if (transfer_progress_step(idx, total_written, content_length,
							                           &next_progress_bytes, &progress_live,
							                           !no_progress, 0) ||
							    bytes_since_yield >= yield_step)
							{
								YieldToAnyThread();
								bytes_since_yield = 0;
							}
						}
					}
				}
			}
			else
			{
				/* body data */
				long wcount = r;

				/* save first bytes for MacBinary check */
				if (first_bytes_len < 128)
				{
					int grab = 128 - first_bytes_len;
					if (grab > r) grab = r;
					memcpy(first_bytes + first_bytes_len, buf, grab);
					first_bytes_len += grab;
				}

				FSWrite(out_ref, &wcount, buf);
				total_written += wcount;
				bytes_since_yield += wcount;

				if (transfer_progress_step(idx, total_written, content_length,
				                           &next_progress_bytes, &progress_live,
				                           !no_progress, 0) ||
				    bytes_since_yield >= yield_step)
				{
					YieldToAnyThread();
					bytes_since_yield = 0;
				}
			}
		}
		download_elapsed_ticks = TickCount() - download_start_tick;
	} /* recv_deadline scope */

	if (progress_live)
		vt_write(idx, "\r\n");

	/* clean up TLS */
	if (tls_initialized)
	{
		mbedtls_ssl_close_notify(&ssl);
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&ssl_conf);
		mbedtls_ctr_drbg_free(&ctr_drbg);
		mbedtls_entropy_free(&entropy);
	}

	/* clean up connection */
	OTSndOrderlyDisconnect(ep);
	OTUnbind(ep);
	OTCloseProvider(ep);
	s->endpoint = kOTInvalidEndpointRef;

	if (out_ref)
	{
		FSClose(out_ref);

		/* detect type/creator */

		/* first: check for MacBinary header */
		if (first_bytes_len >= 128)
		{
			OSType mb_type, mb_creator;
			long mb_data, mb_rsrc;
			if (check_macbinary(first_bytes, first_bytes_len,
			                    &mb_type, &mb_creator, &mb_data, &mb_rsrc))
			{
				ftype = mb_type;
				fcreator = mb_creator;
				checked_macbinary = 1;
				{
					char tb[5], cb[5];
					memcpy(tb, &mb_type, 4); tb[4] = '\0';
					memcpy(cb, &mb_creator, 4); cb[4] = '\0';
					printf_s(idx, "\r\nMacBinary detected: type='%s' creator='%s'\r\n",
					         tb, cb);
				}
			}
		}

		/* second: try extension mapping */
		if (!checked_macbinary)
		{
			if (lookup_ext_type(filename, &ftype, &fcreator))
			{
				char tb[5], cb[5];
				memcpy(tb, &ftype, 4); tb[4] = '\0';
				memcpy(cb, &fcreator, 4); cb[4] = '\0';
				printf_s(idx, "\r\nType from extension: '%s'/'%s'\r\n", tb, cb);
			}
		}

		/* set type/creator */
		{
			FInfo finfo;
			if (FSpGetFInfo(&out_spec, &finfo) == noErr)
			{
				finfo.fdType = ftype;
				finfo.fdCreator = fcreator;
				FSpSetFInfo(&out_spec, &finfo);
			}
		}

		if (download_ok && content_length >= 0 && total_written < content_length)
			download_ok = 0;

		if (download_ok)
			printf_s(idx, "\r\n%ld bytes saved to %s\r\n", total_written, local_name);
		else
			printf_s(idx, "\r\n%ld bytes saved to %s (INCOMPLETE)\r\n",
			         total_written, local_name);

		if (download_elapsed_ticks > 0)
		{
			long kb = total_written / 1024L;
			long kbps = (kb * 60L) / (long)download_elapsed_ticks;
			printf_s(idx, "Average speed: %ld KB/s\r\n", kbps);
		}
	}
	else if (status_code == 200)
	{
		vt_write(idx, "wget: no data received\r\n");
	}

}

static int local_shell_worker_active(const struct session* s)
{
	return (s->type == SESSION_LOCAL &&
	        s->worker_mode != WORKER_NC &&
	        s->thread_id != kNoThreadID &&
	        s->thread_state != DONE &&
	        s->thread_state != UNINITIALIZED);
}

static void* wget_worker_thread(void* arg)
{
	int idx = (int)(long)arg;
	struct session* s = &sessions[idx];

	cmd_wget_run(idx, s->wget_url, s->wget_no_progress ? 1 : 0);

	s->worker_mode = WORKER_NONE;
	s->thread_state = DONE;
	s->thread_command = WAIT;

	if (s->in_use && s->type == SESSION_LOCAL && s->worker_mode != WORKER_NC)
		shell_prompt(idx);

	return 0;
}

static void cmd_wget(int idx, int argc, char** argv)
{
	struct session* s = &sessions[idx];
	ThreadID tid = kNoThreadID;
	OSErr err = noErr;
	int argi = 1;
	int no_progress = 0;

	if (argc < 2)
	{
		vt_write(idx, "usage: wget [-n] <url>\r\n");
		return;
	}

	if (strcmp(argv[argi], "-n") == 0 || strcmp(argv[argi], "--no-progress") == 0)
	{
		no_progress = 1;
		argi++;
	}

	if (argi >= argc)
	{
		vt_write(idx, "usage: wget [-n] <url>\r\n");
		return;
	}

	if (argi + 1 < argc)
	{
		vt_write(idx, "wget: too many arguments\r\n");
		vt_write(idx, "usage: wget [-n] <url>\r\n");
		return;
	}

	if (s->worker_mode == WORKER_NC)
	{
		vt_write(idx, "wget: unavailable while nc session is active\r\n");
		return;
	}

	if (s->thread_state == DONE && s->thread_id != kNoThreadID)
	{
		session_reap_thread(idx, 0);
		if (s->thread_id != kNoThreadID)
		{
			vt_write(idx, "wget: previous worker thread could not be reclaimed\r\n");
			return;
		}
	}

	if (local_shell_worker_active(s))
	{
		vt_write(idx, "wget: another local command is already running\r\n");
		return;
	}

	strncpy(s->wget_url, argv[argi], sizeof(s->wget_url) - 1);
	s->wget_url[sizeof(s->wget_url) - 1] = '\0';
	s->wget_no_progress = no_progress ? 1 : 0;

	s->thread_command = READ;
	s->thread_state = OPEN;
	s->endpoint = kOTInvalidEndpointRef;

	err = NewThread(kCooperativeThread, wget_worker_thread,
	                (void*)(long)idx, 100000,
	                kCreateIfNeeded, NULL, &tid);
	if (err != noErr)
	{
		s->thread_command = WAIT;
		s->thread_state = DONE;
		s->thread_id = kNoThreadID;
		printf_s(idx, "wget: failed to create worker thread (err=%d)\r\n", (int)err);
		return;
	}

	s->thread_id = tid;
	s->worker_mode = WORKER_WGET;
}

/* ------------------------------------------------------------------ */
/* scp - SSH file copy (download and upload)                          */
/* ------------------------------------------------------------------ */

/* parse user@host:/path or user@host:port:/path into components.
   returns 1 if arg matches remote format, 0 otherwise. */
static int parse_scp_spec(const char* arg,
                           char* user, int user_max,
                           char* host, int host_max,
                           char* port, int port_max,
                           char* remote_path, int path_max)
{
	const char* at;
	const char* colon;
	const char* second_colon;
	int ulen, hlen;

	at = strchr(arg, '@');
	if (at == NULL) return 0;

	colon = strchr(at + 1, ':');
	if (colon == NULL) return 0;

	/* user */
	ulen = at - arg;
	if (ulen <= 0 || ulen >= user_max) return 0;
	memcpy(user, arg, ulen);
	user[ulen] = '\0';

	/* host */
	hlen = colon - (at + 1);
	if (hlen <= 0 || hlen >= host_max) return 0;
	memcpy(host, at + 1, hlen);
	host[hlen] = '\0';

	/* check for port: user@host:port:/path */
	second_colon = strchr(colon + 1, ':');
	if (second_colon != NULL && second_colon > colon + 1)
	{
		/* chars between first colon and second colon should be digits */
		const char* p;
		int all_digits = 1;
		int plen;

		for (p = colon + 1; p < second_colon; p++)
		{
			if (*p < '0' || *p > '9') { all_digits = 0; break; }
		}

		if (all_digits)
		{
			plen = second_colon - (colon + 1);
			if (plen >= port_max) return 0;
			memcpy(port, colon + 1, plen);
			port[plen] = '\0';
			colon = second_colon; /* advance past port */
		}
		else
		{
			strncpy(port, "22", port_max - 1);
			port[port_max - 1] = '\0';
		}
	}
	else
	{
		strncpy(port, "22", port_max - 1);
		port[port_max - 1] = '\0';
	}

	/* remote path: everything after the (final) colon */
	if (colon[1] == '\0') return 0; /* empty path */
	strncpy(remote_path, colon + 1, path_max - 1);
	remote_path[path_max - 1] = '\0';

	return 1;
}

/* extract basename from a remote path, truncate to 31 chars for HFS */
static void scp_basename(const char* remote_path, char* out, int out_max)
{
	const char* slash;
	int len;

	slash = strrchr(remote_path, '/');
	if (slash != NULL && slash[1] != '\0')
		slash++;
	else if (slash == NULL)
		slash = remote_path;
	else
		slash = remote_path; /* trailing slash — use whole thing */

	len = strlen(slash);
	if (len > 31) len = 31; /* HFS limit */
	if (len >= out_max) len = out_max - 1;
	memcpy(out, slash, len);
	out[len] = '\0';
}

/* Normalize remote path so ~ expansion isn't needed by the remote shell.
   SCP sink (scp -t) runs in the user's home, so ~ = "." and ~/foo = "foo".
   This lets us keep QUOTE_PATHS enabled for proper space/metachar escaping. */
static void scp_normalize_remote(char* path)
{
	if (path[0] == '~' && path[1] == '/')
		memmove(path, path + 2, strlen(path + 2) + 1);
	else if (path[0] == '~' && path[1] == '\0')
	{
		path[0] = '.';
	}
}

static void scp_download(int idx)
{
	struct session* s = &sessions[idx];
	struct ssh_auth_params auth;
	char hostname_buf[280]; /* "host:port" */
	libssh2_struct_stat sb;
	long io_buf_size = 32768L; /* larger SCP I/O chunks for better throughput */
	long total_read = 0;
	long next_progress_bytes = 0;
	long bytes_since_yield = 0;
	const long yield_step = 524288L; /* 512KB */
	int progress_live = 0;
	short out_ref = 0;
	int file_open = 0;
	unsigned char first_bytes[128];
	int first_bytes_len = 0;

	/* build auth params from session's snapshotted fields */
	snprintf(hostname_buf, sizeof(hostname_buf), "%s:%s", s->scp_host, s->scp_port);
	auth.hostname = hostname_buf;
	auth.host_only = s->scp_host;
	auth.port = atoi(s->scp_port);
	auth.username = s->scp_user;
	auth.password = s->scp_password;
	auth.pubkey_path = s->scp_pubkey_path;
	auth.privkey_path = s->scp_privkey_path;
	auth.use_key = s->scp_use_key;

	/* initialize Open Transport (idempotent, required before any OT calls) */
	if (InitOpenTransport() != noErr)
	{
		printf_s(idx, "scp: failed to initialize Open Transport\r\n");
		return;
	}

	/* allocate OT buffers for SSH transport */
	s->recv_buffer = OTAllocMem(io_buf_size);
	s->send_buffer = OTAllocMem(io_buf_size);
	if (s->recv_buffer == NULL || s->send_buffer == NULL)
	{
		if (s->recv_buffer) { OTFreeMem(s->recv_buffer); s->recv_buffer = NULL; }
		if (s->send_buffer) { OTFreeMem(s->send_buffer); s->send_buffer = NULL; }
		printf_s(idx, "scp: failed to allocate buffers\r\n");
		return;
	}

	/* connect + authenticate */
	if (!ssh_connect_and_auth(idx, &auth))
	{
		/* ssh_connect_and_auth cleaned up on failure */
		OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
		OTFreeMem(s->send_buffer); s->send_buffer = NULL;
		return;
	}

	/* non-blocking mode: we handle EAGAIN retries ourselves */
	libssh2_session_set_blocking(s->ssh_session, 0);

	/* normalize ~ in remote path (SCP sink runs in home dir) so we can
	   keep QUOTE_PATHS enabled for proper space/metachar escaping */
	scp_normalize_remote(s->scp_remote_path);

	/* open SCP receive channel */
	memset(&sb, 0, sizeof(sb));
	while (1)
	{
		s->channel = libssh2_scp_recv2(s->ssh_session, s->scp_remote_path, &sb);
		if (s->channel != NULL) break;
		if (libssh2_session_last_errno(s->ssh_session) == LIBSSH2_ERROR_EAGAIN)
		{
			YieldToAnyThread();
			if (s->thread_command == EXIT) break;
			continue;
		}
		printf_s(idx, "scp: failed to open remote file: %s\r\n",
		         libssh2_error_string(libssh2_session_last_errno(s->ssh_session)));
		break;
	}

	if (s->channel == NULL)
	{
		end_connection(idx);
		OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
		OTFreeMem(s->send_buffer); s->send_buffer = NULL;
		return;
	}

	/* create local file */
	{
		Str255 pname;
		int nlen = strlen(s->scp_local_path);
		OSErr ferr;

		if (nlen > 31) nlen = 31;
		pname[0] = nlen;
		memcpy(pname + 1, s->scp_local_path, nlen);

		ferr = HCreate(s->shell_vRefNum, s->shell_dirID, pname, 'SeT7', 'TEXT');
		if (ferr == dupFNErr)
		{
			/* file exists — overwrite */
			ferr = noErr;
		}
		if (ferr != noErr && ferr != dupFNErr)
		{
			printf_s(idx, "scp: failed to create file (err=%d)\r\n", (int)ferr);
			end_connection(idx);
			OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
			OTFreeMem(s->send_buffer); s->send_buffer = NULL;
			return;
		}

		ferr = HOpenDF(s->shell_vRefNum, s->shell_dirID, pname, fsWrPerm, &out_ref);
		if (ferr != noErr)
		{
			printf_s(idx, "scp: failed to open file for writing (err=%d)\r\n", (int)ferr);
			end_connection(idx);
			OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
			OTFreeMem(s->send_buffer); s->send_buffer = NULL;
			return;
		}
		file_open = 1;
	}

	/* read loop */
	{
		long file_size = (long)sb.st_size;
		long remaining = file_size;

		while (remaining > 0 && s->thread_command != EXIT)
		{
			long to_read = io_buf_size;
			ssize_t rc;
			long wcount;

			if (to_read > remaining) to_read = remaining;

			rc = libssh2_channel_read(s->channel, s->recv_buffer, to_read);
			if (rc == LIBSSH2_ERROR_EAGAIN)
			{
				YieldToAnyThread();
				continue;
			}
			if (rc < 0)
			{
				printf_s(idx, "\r\nscp: read error: %s\r\n", libssh2_error_string(rc));
				break;
			}
			if (rc == 0) break;

			/* capture first 128 bytes for type detection */
			if (first_bytes_len < 128)
			{
				int grab = 128 - first_bytes_len;
				if (grab > rc) grab = rc;
				memcpy(first_bytes + first_bytes_len, s->recv_buffer, grab);
				first_bytes_len += grab;
			}

			wcount = rc;
			FSWrite(out_ref, &wcount, s->recv_buffer);
			total_read += rc;
			remaining -= rc;
			bytes_since_yield += rc;

			if (transfer_progress_step(idx, total_read, file_size,
			                           &next_progress_bytes, &progress_live,
			                           !s->scp_no_progress, 0) ||
			    bytes_since_yield >= yield_step)
			{
				YieldToAnyThread();
				bytes_since_yield = 0;
			}
		}
	}

	/* close local file */
	if (file_open)
	{
		SetEOF(out_ref, total_read);
		FSClose(out_ref);
	}

	/* detect file type/creator */
	if (total_read > 0)
	{
		OSType ftype = 'TEXT';
		OSType fcreator = 'SeT7';
		Str255 pname;
		FInfo finfo;
		int nlen = strlen(s->scp_local_path);

		if (nlen > 31) nlen = 31;
		pname[0] = nlen;
		memcpy(pname + 1, s->scp_local_path, nlen);

		{
			long mb_data_len = 0, mb_rsrc_len = 0;
			if (!check_macbinary(first_bytes, first_bytes_len, &ftype, &fcreator,
			                     &mb_data_len, &mb_rsrc_len))
				lookup_ext_type(s->scp_local_path, &ftype, &fcreator);
		}

		if (HGetFInfo(s->shell_vRefNum, s->shell_dirID, pname, &finfo) == noErr)
		{
			finfo.fdType = ftype;
			finfo.fdCreator = fcreator;
			HSetFInfo(s->shell_vRefNum, s->shell_dirID, pname, &finfo);
		}
	}

	if (progress_live)
		vt_write(idx, "\r\n");

	if (s->thread_command == EXIT)
		printf_s(idx, "scp: cancelled\r\n");
	else
		printf_s(idx, "scp: downloaded %ld bytes -> %s\r\n", total_read, s->scp_local_path);

	/* cleanup: caller owns connection after successful auth */
	end_connection(idx);
	OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
	OTFreeMem(s->send_buffer); s->send_buffer = NULL;
}

static void scp_upload(int idx)
{
	struct session* s = &sessions[idx];
	struct ssh_auth_params auth;
	char hostname_buf[280];
	long io_buf_size = 32768L; /* larger SCP I/O chunks for better throughput */
	short in_ref = 0;
	long total_written = 0;
	long remaining = 0;
	long next_progress_bytes = 0;
	long bytes_since_yield = 0;
	const long yield_step = 524288L; /* 512KB */
	int progress_live = 0;
	int upload_ok = 1;

	/* open local file from pre-resolved FSSpec */
	{
		OSErr ferr = FSpOpenDF(&s->scp_local_spec, fsRdPerm, &in_ref);
		if (ferr != noErr)
		{
			printf_s(idx, "scp: failed to open local file (err=%d)\r\n", (int)ferr);
			return;
		}
	}

	/* build auth params */
	snprintf(hostname_buf, sizeof(hostname_buf), "%s:%s", s->scp_host, s->scp_port);
	auth.hostname = hostname_buf;
	auth.host_only = s->scp_host;
	auth.port = atoi(s->scp_port);
	auth.username = s->scp_user;
	auth.password = s->scp_password;
	auth.pubkey_path = s->scp_pubkey_path;
	auth.privkey_path = s->scp_privkey_path;
	auth.use_key = s->scp_use_key;

	/* initialize Open Transport (idempotent, required before any OT calls) */
	if (InitOpenTransport() != noErr)
	{
		FSClose(in_ref);
		printf_s(idx, "scp: failed to initialize Open Transport\r\n");
		return;
	}

	/* allocate OT buffers */
	s->recv_buffer = OTAllocMem(io_buf_size);
	s->send_buffer = OTAllocMem(io_buf_size);
	if (s->recv_buffer == NULL || s->send_buffer == NULL)
	{
		if (s->recv_buffer) { OTFreeMem(s->recv_buffer); s->recv_buffer = NULL; }
		if (s->send_buffer) { OTFreeMem(s->send_buffer); s->send_buffer = NULL; }
		FSClose(in_ref);
		printf_s(idx, "scp: failed to allocate buffers\r\n");
		return;
	}

	/* connect + authenticate */
	if (!ssh_connect_and_auth(idx, &auth))
	{
		OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
		OTFreeMem(s->send_buffer); s->send_buffer = NULL;
		FSClose(in_ref);
		return;
	}

	/* non-blocking mode: we handle EAGAIN retries ourselves */
	libssh2_session_set_blocking(s->ssh_session, 0);

	/* normalize ~ in remote path (SCP sink runs in home dir) so we can
	   keep QUOTE_PATHS enabled for proper space/metachar escaping */
	scp_normalize_remote(s->scp_remote_path);

	/* open SCP send channel */
	while (1)
	{
		s->channel = libssh2_scp_send_ex(s->ssh_session, s->scp_remote_path,
		                                  0644, s->scp_local_file_size, 0, 0);
		if (s->channel != NULL) break;
		if (libssh2_session_last_errno(s->ssh_session) == LIBSSH2_ERROR_EAGAIN)
		{
			YieldToAnyThread();
			if (s->thread_command == EXIT) break;
			continue;
		}
		printf_s(idx, "scp: failed to open remote path: %s\r\n",
		         libssh2_error_string(libssh2_session_last_errno(s->ssh_session)));
		break;
	}

	if (s->channel == NULL)
	{
		end_connection(idx);
		OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
		OTFreeMem(s->send_buffer); s->send_buffer = NULL;
		FSClose(in_ref);
		return;
	}

	/* write loop */
	remaining = s->scp_local_file_size;
	while (remaining > 0 && s->thread_command != EXIT)
	{
		long to_read = io_buf_size;
		long rcount;
		char* ptr;
		long left;
		OSErr ferr;

		if (to_read > remaining) to_read = remaining;
		rcount = to_read;
		ferr = FSRead(in_ref, &rcount, s->send_buffer);
		if (ferr != noErr && ferr != eofErr)
		{
			printf_s(idx, "\r\nscp: local read error (err=%d)\r\n", (int)ferr);
			upload_ok = 0;
			break;
		}
		if (rcount == 0)
		{
			upload_ok = 0;
			break;
		}

		ptr = s->send_buffer;
		left = rcount;
		while (left > 0 && s->thread_command != EXIT)
		{
			ssize_t rc = libssh2_channel_write(s->channel, ptr, left);
			if (rc == LIBSSH2_ERROR_EAGAIN || rc == 0)
			{
				YieldToAnyThread();
				continue;
			}
			if (rc < 0)
			{
				printf_s(idx, "\r\nscp: write error: %s\r\n", libssh2_error_string(rc));
				upload_ok = 0;
				goto upload_done;
			}
			ptr += rc;
			left -= rc;
			total_written += rc;
			remaining -= rc;
			bytes_since_yield += rc;

			if (bytes_since_yield >= yield_step)
			{
				YieldToAnyThread();
				bytes_since_yield = 0;
			}
		}

		if (transfer_progress_step(idx, total_written, s->scp_local_file_size,
		                           &next_progress_bytes, &progress_live,
		                           !s->scp_no_progress, 1))
		{
			YieldToAnyThread();
			bytes_since_yield = 0;
		}
	}

upload_done:
	{
		int eof_rc;
		do {
			eof_rc = libssh2_channel_send_eof(s->channel);
			if (eof_rc == LIBSSH2_ERROR_EAGAIN) YieldToAnyThread();
		} while (eof_rc == LIBSSH2_ERROR_EAGAIN);
	}
	FSClose(in_ref);

	if (progress_live)
		vt_write(idx, "\r\n");

	if (s->thread_command == EXIT)
		printf_s(idx, "scp: cancelled\r\n");
	else if (!upload_ok || remaining > 0)
		printf_s(idx, "scp: upload incomplete (%ld bytes sent)\r\n", total_written);
	else
		printf_s(idx, "scp: uploaded %ld bytes -> %s\r\n", total_written, s->scp_remote_path);

	end_connection(idx);
	OTFreeMem(s->recv_buffer); s->recv_buffer = NULL;
	OTFreeMem(s->send_buffer); s->send_buffer = NULL;
}

static void scp_upload_glob(int idx)
{
	struct session* s = &sessions[idx];
	CInfoPBRec pb;
	Str255 name;
	short gi;
	int count = 0;
	int fail_count = 0;
	char base_remote[512];

	copy_cstr_trunc(base_remote, sizeof(base_remote), s->scp_remote_path);

	for (gi = 1; s->thread_command != EXIT; gi++)
	{
		char name_c[256];
		int nl;
		FSSpec spec;
		short ref;
		long eof_size;
		OSErr ferr;
		int rlen;

		memset(&pb, 0, sizeof(pb));
		pb.hFileInfo.ioNamePtr = name;
		pb.hFileInfo.ioVRefNum = s->scp_glob_vRefNum;
		pb.hFileInfo.ioDirID = s->scp_glob_dirID;
		pb.hFileInfo.ioFDirIndex = gi;
		if (PBGetCatInfoSync(&pb) != noErr) break;

		/* skip directories */
		if (pb.hFileInfo.ioFlAttrib & ioDirMask) continue;

		nl = name[0];
		if (nl > 255) nl = 255;
		memcpy(name_c, name + 1, nl);
		name_c[nl] = '\0';

		if (!glob_match(s->scp_glob_pattern, name_c)) continue;

		/* resolve file size */
		spec.vRefNum = s->scp_glob_vRefNum;
		spec.parID = s->scp_glob_dirID;
		spec.name[0] = name[0];
		memcpy(spec.name + 1, name + 1, name[0]);

		ferr = FSpOpenDF(&spec, fsRdPerm, &ref);
		if (ferr != noErr)
		{
			printf_s(idx, "scp: cannot open %s (err=%d)\r\n", name_c, (int)ferr);
			fail_count++;
			continue;
		}
		GetEOF(ref, &eof_size);
		FSClose(ref);

		s->scp_local_spec = spec;
		s->scp_local_file_size = eof_size;

		/* construct remote path: base + "/" + filename */
		copy_cstr_trunc(s->scp_remote_path, sizeof(s->scp_remote_path), base_remote);
		rlen = strlen(s->scp_remote_path);
		if (rlen > 0 && s->scp_remote_path[rlen - 1] != '/')
		{
			s->scp_remote_path[rlen++] = '/';
			s->scp_remote_path[rlen] = '\0';
		}
		copy_cstr_trunc(s->scp_remote_path + rlen,
		                sizeof(s->scp_remote_path) - rlen, name_c);

		printf_s(idx, "scp: [%d] %s (%ld bytes)\r\n", count + 1, name_c, eof_size);
		scp_upload(idx);
		count++;
	}

	if (s->thread_command == EXIT)
		printf_s(idx, "scp: cancelled after %d file(s)\r\n", count);
	else if (count == 0)
		printf_s(idx, "scp: no matching files\r\n");
	else
		printf_s(idx, "scp: %d file(s) uploaded (%d failed)\r\n", count, fail_count);
}

static void* scp_worker_thread(void* arg)
{
	int idx = (int)(long)arg;
	struct session* s = &sessions[idx];

	if (s->scp_direction == 0)
		scp_download(idx);
	else if (s->scp_glob_pattern[0] != '\0')
		scp_upload_glob(idx);
	else
		scp_upload(idx);

	s->worker_mode = WORKER_NONE;
	s->thread_state = DONE;
	s->thread_command = WAIT;

	if (s->in_use && s->type == SESSION_LOCAL)
		shell_prompt(idx);

	return 0;
}

/* show SCP auth dialog: pick password/key, then prompt for credentials.
   populates s->scp_password, scp_pubkey_path, scp_privkey_path, scp_use_key.
   returns 1=ok 0=cancel. */
static int scp_auth_prompt(struct session* s)
{
	DialogPtr dlg;
	DialogItemType type;
	Handle itemH;
	Rect box;
	short item;
	ControlHandle pw_radio, key_radio;
	int use_password;
	Str255 saved_pw;
	int saved_auth;
	char* saved_pubkey;
	char* saved_privkey;
	int ok;
	unsigned int plen;

	/* TEInit + InitDialogs may not have been called yet if the user
	   went straight to local shell without opening any dialog first */
	TEInit();
	InitDialogs(NULL);
	InitCursor();

	/* show auth method chooser */
	dlg = GetNewDialog(DLOG_SCP_AUTH, 0, (WindowPtr)-1);
	if (!dlg) return 0;

	/* draw default button indicator */
	GetDialogItem(dlg, 2, &type, &itemH, &box);
	SetDialogItem(dlg, 2, type, (Handle)NewUserItemUPP(&ButtonFrameProc), &box);

	/* get radio button handles */
	GetDialogItem(dlg, 4, &type, &itemH, &box);
	pw_radio = (ControlHandle)itemH;
	GetDialogItem(dlg, 5, &type, &itemH, &box);
	key_radio = (ControlHandle)itemH;

	/* default to password, or key if prefs has keys configured */
	if (prefs.auth_type == USE_KEY &&
	    prefs.pubkey_path && prefs.pubkey_path[0] != '\0' &&
	    prefs.privkey_path && prefs.privkey_path[0] != '\0')
	{
		SetControlValue(key_radio, 1);
		SetControlValue(pw_radio, 0);
	}
	else
	{
		SetControlValue(pw_radio, 1);
		SetControlValue(key_radio, 0);
	}

	do {
		ModalDialog(NULL, &item);
		if (item == 4)
		{
			SetControlValue(pw_radio, 1);
			SetControlValue(key_radio, 0);
		}
		else if (item == 5)
		{
			SetControlValue(pw_radio, 0);
			SetControlValue(key_radio, 1);
		}
	} while (item != 1 && item != 6);

	use_password = GetControlValue(pw_radio);
	DisposeDialog(dlg);
	FlushEvents(everyEvent, -1);

	if (item == 6) return 0; /* cancel */

	/* save prefs state — password_dialog/key_dialog mutate prefs */
	memcpy(saved_pw, prefs.password, sizeof(Str255));
	saved_auth = prefs.auth_type;
	saved_pubkey = prefs.pubkey_path;
	saved_privkey = prefs.privkey_path;

	if (use_password)
	{
		/* clear prefs.password so dialog starts empty */
		prefs.password[0] = 0;
		ok = password_dialog(DLOG_PASSWORD);
		if (ok)
		{
			plen = (unsigned char)prefs.password[0];
			if (plen > sizeof(s->scp_password) - 1)
				plen = sizeof(s->scp_password) - 1;
			memcpy(s->scp_password, prefs.password + 1, plen);
			s->scp_password[plen] = '\0';
		}
		s->scp_use_key = 0;
		s->scp_pubkey_path[0] = '\0';
		s->scp_privkey_path[0] = '\0';
	}
	else
	{
		/* key_dialog: prompts for key files if not in prefs, then passphrase */
		ok = key_dialog();
		if (ok)
		{
			/* snapshot password (key passphrase) */
			plen = (unsigned char)prefs.password[0];
			if (plen > sizeof(s->scp_password) - 1)
				plen = sizeof(s->scp_password) - 1;
			memcpy(s->scp_password, prefs.password + 1, plen);
			s->scp_password[plen] = '\0';
				/* snapshot key paths */
				if (prefs.pubkey_path)
				{
					copy_cstr_trunc(s->scp_pubkey_path, sizeof(s->scp_pubkey_path), prefs.pubkey_path);
				}
				if (prefs.privkey_path)
				{
					copy_cstr_trunc(s->scp_privkey_path, sizeof(s->scp_privkey_path), prefs.privkey_path);
				}
			s->scp_use_key = 1;
		}
	}

	/* restore prefs state, freeing any new allocations from key_dialog */
	memcpy(prefs.password, saved_pw, sizeof(Str255));
	prefs.auth_type = saved_auth;
	if (prefs.pubkey_path != saved_pubkey)
	{
		free(prefs.pubkey_path);
		prefs.pubkey_path = saved_pubkey;
	}
	if (prefs.privkey_path != saved_privkey)
	{
		free(prefs.privkey_path);
		prefs.privkey_path = saved_privkey;
	}

	return ok;
}

static void cmd_scp(int idx, int argc, char** argv)
{
	struct session* s = &sessions[idx];
	ThreadID tid = kNoThreadID;
	OSErr err = noErr;
	int argi = 1;
	int no_progress = 0;
	char user[256], host[256], port[16], remote_path[512];
	int local_arg = -1;  /* which argv[] is the local file/name */

	if (argc < 2)
	{
		vt_write(idx, "usage: scp [-n] user@host:/path [local]\r\n");
		vt_write(idx, "       scp [-n] local user@host:/path\r\n");
		return;
	}

	/* parse -n flag */
	if (strcmp(argv[argi], "-n") == 0 || strcmp(argv[argi], "--no-progress") == 0)
	{
		no_progress = 1;
		argi++;
	}

	if (argi >= argc)
	{
		vt_write(idx, "usage: scp [-n] user@host:/path [local]\r\n");
		return;
	}

	/* guard: no worker already running */
	if (s->worker_mode != WORKER_NONE)
	{
		vt_write(idx, "scp: another worker is already active\r\n");
		return;
	}

	/* reap previous thread if DONE */
	if (s->thread_state == DONE && s->thread_id != kNoThreadID)
	{
		session_reap_thread(idx, 0);
		if (s->thread_id != kNoThreadID)
		{
			vt_write(idx, "scp: previous worker thread could not be reclaimed\r\n");
			return;
		}
	}

	if (local_shell_worker_active(s))
	{
		vt_write(idx, "scp: another local command is already running\r\n");
		return;
	}

	/* detect direction: which arg is the remote spec? */
	if (parse_scp_spec(argv[argi], user, 256, host, 256, port, 16, remote_path, 512))
	{
		/* first non-flag arg is remote -> download */
		s->scp_direction = 0;
		if (argi + 1 < argc)
			local_arg = argi + 1;
	}
	else if (argi + 1 < argc &&
	         parse_scp_spec(argv[argi + 1], user, 256, host, 256, port, 16, remote_path, 512))
	{
		/* second arg is remote -> upload */
		local_arg = argi;
		s->scp_direction = 1;
	}
	else
	{
		vt_write(idx, "scp: no valid user@host:/path argument found\r\n");
		return;
	}

	/* populate session SCP fields */
	copy_cstr_trunc(s->scp_user, sizeof(s->scp_user), user);
	copy_cstr_trunc(s->scp_host, sizeof(s->scp_host), host);
	copy_cstr_trunc(s->scp_port, sizeof(s->scp_port), port);
	copy_cstr_trunc(s->scp_remote_path, sizeof(s->scp_remote_path), remote_path);
	s->scp_no_progress = no_progress ? 1 : 0;

	if (s->scp_direction == 0)
	{
		/* download: set local filename */
		if (local_arg >= 0 &&
		    strcmp(argv[local_arg], ".") != 0 &&
		    strcmp(argv[local_arg], "./") != 0)
		{
			copy_cstr_trunc(s->scp_local_path, sizeof(s->scp_local_path), argv[local_arg]);
			/* truncate to 31 for HFS */
			if (strlen(s->scp_local_path) > 31)
				s->scp_local_path[31] = '\0';
		}
		else
		{
			/* no local name given, or "." — use basename of remote path */
			scp_basename(remote_path, s->scp_local_path, sizeof(s->scp_local_path));
		}
	}
	else if (is_glob(argv[local_arg]))
	{
		/* upload with glob: expand pattern, worker will iterate files */
		short gvRef;
		long gdID;
		const char* gpat;
		CInfoPBRec gpb;
		Str255 gname;
		short gi;
		int match_count = 0;

		if (glob_resolve_dir(idx, argv[local_arg], &gvRef, &gdID, &gpat) != 0)
		{
			printf_s(idx, "scp: invalid path: %s\r\n", argv[local_arg]);
			return;
		}

		/* count matches (files only) */
		for (gi = 1; ; gi++)
		{
			char nc[256];
			int nl;
			memset(&gpb, 0, sizeof(gpb));
			gpb.hFileInfo.ioNamePtr = gname;
			gpb.hFileInfo.ioVRefNum = gvRef;
			gpb.hFileInfo.ioDirID = gdID;
			gpb.hFileInfo.ioFDirIndex = gi;
			if (PBGetCatInfoSync(&gpb) != noErr) break;
			if (gpb.hFileInfo.ioFlAttrib & ioDirMask) continue;
			nl = gname[0];
			if (nl > 255) nl = 255;
			memcpy(nc, gname + 1, nl);
			nc[nl] = '\0';
			if (glob_match(gpat, nc)) match_count++;
		}

		if (match_count == 0)
		{
			printf_s(idx, "scp: no files matching '%s'\r\n", argv[local_arg]);
			return;
		}

		copy_cstr_trunc(s->scp_glob_pattern, sizeof(s->scp_glob_pattern), gpat);
		s->scp_glob_vRefNum = gvRef;
		s->scp_glob_dirID = gdID;

		printf_s(idx, "scp: %d file(s) matching '%s'\r\n", match_count, gpat);
	}
	else
	{
		/* upload: resolve single file */
		FSSpec spec;
		short ref;
		long eof_size;
		OSErr ferr;

		s->scp_glob_pattern[0] = '\0';

		if (resolve_path(idx, argv[local_arg], &spec) != noErr)
		{
			printf_s(idx, "scp: file not found: %s\r\n", argv[local_arg]);
			return;
		}

		ferr = FSpOpenDF(&spec, fsRdPerm, &ref);
		if (ferr != noErr)
		{
			printf_s(idx, "scp: cannot open file (err=%d)\r\n", (int)ferr);
			return;
		}
		GetEOF(ref, &eof_size);
		FSClose(ref);

		s->scp_local_spec = spec;
		s->scp_local_file_size = eof_size;

		/* If remote path looks like a directory, append local filename.
		   libssh2 uses basename(remote_path) for the SCP C header filename,
		   so "~" alone would create a file literally named "~". */
		{
			const char* rp = s->scp_remote_path;
			int rlen = strlen(rp);
			int is_dir = 0;

			if (rlen == 0 || rp[rlen - 1] == '/')
				is_dir = 1;
			else if (strcmp(rp, "~") == 0 || strcmp(rp, ".") == 0 || strcmp(rp, "..") == 0)
				is_dir = 1;
			else if (rp[0] == '~' && rp[1] == '/' && rp[rlen - 1] == '/')
				is_dir = 1;

			if (is_dir)
			{
				int space = sizeof(s->scp_remote_path) - rlen - 1;
				if (rlen > 0 && rp[rlen - 1] != '/')
				{
					s->scp_remote_path[rlen++] = '/';
					s->scp_remote_path[rlen] = '\0';
					space--;
				}
				if (space > 0)
				{
					copy_cstr_trunc(s->scp_remote_path + rlen, space,
					                argv[local_arg]);
				}
			}
		}
	}

	/* prompt for auth method + credentials (runs on main thread) */
	if (!scp_auth_prompt(s))
	{
		vt_write(idx, "scp: cancelled\r\n");
		return;
	}

	/* set up thread state */
	s->thread_command = READ;
	s->thread_state = OPEN;
	s->endpoint = kOTInvalidEndpointRef;

	err = NewThread(kCooperativeThread, scp_worker_thread,
	                (void*)(long)idx, 100000,
	                kCreateIfNeeded, NULL, &tid);
	if (err != noErr)
	{
		s->thread_command = WAIT;
		s->thread_state = DONE;
		s->thread_id = kNoThreadID;
		printf_s(idx, "scp: failed to create worker thread (err=%d)\r\n", (int)err);
		return;
	}

	s->thread_id = tid;
	s->worker_mode = WORKER_SCP;
}

/* ------------------------------------------------------------------ */
/* realpath - resolve to full absolute path                           */
/* ------------------------------------------------------------------ */

/* get full colon path for a file FSSpec */
static void fsspec_to_path(FSSpec* spec, char* out, int maxlen)
{
	char dir_path[512];
	char name[64];
	int nlen;

	get_full_path(spec->vRefNum, spec->parID, dir_path, sizeof(dir_path));

	nlen = spec->name[0];
	if (nlen > 63) nlen = 63;
	memcpy(name, spec->name + 1, nlen);
	name[nlen] = '\0';

	if (strlen(dir_path) + strlen(name) < (size_t)maxlen)
	{
		strcpy(out, dir_path);
		strcat(out, name);
	}
	else
	{
		strncpy(out, "???", maxlen);
	}
}

static void cmd_realpath(int idx, int argc, char** argv)
{
	FSSpec spec;
	OSErr e;
	char path[512];

	if (argc < 2)
	{
		vt_write(idx, "usage: realpath <path>\r\n");
		return;
	}

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "realpath: not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	fsspec_to_path(&spec, path, sizeof(path));
	vt_write(idx, path);
	vt_write(idx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* hostname - show Mac computer name from Sharing Setup               */
/* ------------------------------------------------------------------ */

static void cmd_hostname(int idx, int argc, char** argv)
{
	StringHandle h;

	(void)argc;
	(void)argv;

	h = GetString(-16413);
	if (h && *h && (*h)[0] > 0)
	{
		char name[256];
		int len = (*h)[0];
		memcpy(name, *h + 1, len);
		name[len] = '\0';
		vt_write(idx, name);
		vt_write(idx, "\r\n");
	}
	else
	{
		vt_write(idx, "(no name set)\r\n");
	}
}

/* ------------------------------------------------------------------ */
/* which - find an application by name                                */
/* ------------------------------------------------------------------ */

static void cmd_which(int idx, int argc, char** argv)
{
	FSSpec spec;
	FInfo finfo;
	OSErr e;
	char path[512];

	if (argc < 2)
	{
		vt_write(idx, "usage: which <command>\r\n");
		return;
	}

	/* try to resolve the argument as a path */
	e = resolve_path(idx, argv[1], &spec);
	if (e == noErr && FSpGetFInfo(&spec, &finfo) == noErr && finfo.fdType == 'APPL')
	{
		fsspec_to_path(&spec, path, sizeof(path));
		vt_write(idx, path);
		vt_write(idx, "\r\n");
		return;
	}

	vt_write(idx, argv[1]);
	vt_write(idx, ": not found\r\n");
}

/* ------------------------------------------------------------------ */
/* ln -s / readlink - Mac alias creation and resolution               */
/* ------------------------------------------------------------------ */

#include <Aliases.h>
#include <Resources.h>

static void cmd_ln(int idx, int argc, char** argv)
{
	int sym = 0;
	const char* target_path = NULL;
	const char* link_path = NULL;
	int argi;
	FSSpec target_spec, link_spec;
	AliasHandle alias;
	OSErr e;
	FInfo finfo;
	short res_ref;

	for (argi = 1; argi < argc; argi++)
	{
		if (strcmp(argv[argi], "-s") == 0)
			sym = 1;
		else if (!target_path)
			target_path = argv[argi];
		else
			link_path = argv[argi];
	}

	if (!target_path || !link_path)
	{
		vt_write(idx, "usage: ln -s <target> <alias>\r\n");
		return;
	}

	if (!sym)
	{
		vt_write(idx, "ln: only symbolic links (aliases) supported, use -s\r\n");
		return;
	}

	/* resolve target */
	e = resolve_path(idx, target_path, &target_spec);
	if (e != noErr)
	{
		vt_write(idx, "ln: target not found: ");
		vt_write(idx, target_path);
		vt_write(idx, "\r\n");
		return;
	}

	/* create alias record */
	e = NewAliasMinimal(&target_spec, &alias);
	if (e != noErr)
	{
		vt_write(idx, "ln: cannot create alias record\r\n");
		return;
	}

	/* create the alias file */
	{
		struct session* s = &sessions[idx];
		Str255 pname;
		int len = strlen(link_path);
		if (len > 31) len = 31;
		pname[0] = len;
		memcpy(pname + 1, link_path, len);

		/* try to resolve link_path; if it doesn't exist, create in cwd */
		e = resolve_path(idx, link_path, &link_spec);
		if (e == noErr)
		{
			/* file already exists */
			DisposeHandle((Handle)alias);
			vt_write(idx, "ln: file exists: ");
			vt_write(idx, link_path);
			vt_write(idx, "\r\n");
			return;
		}

		/* create the file */
		e = HCreate(s->shell_vRefNum, s->shell_dirID, pname, 'adrp', 'MACS');
		if (e != noErr)
		{
			DisposeHandle((Handle)alias);
			vt_write(idx, "ln: cannot create file\r\n");
			return;
		}

		e = FSMakeFSSpec(s->shell_vRefNum, s->shell_dirID, pname, &link_spec);
	}

	/* set kIsAlias flag */
	e = FSpGetFInfo(&link_spec, &finfo);
	if (e == noErr)
	{
		finfo.fdFlags |= kIsAlias;
		FSpSetFInfo(&link_spec, &finfo);
	}

	/* write alias record to resource fork */
	HCreateResFile(link_spec.vRefNum, link_spec.parID, link_spec.name);
	res_ref = FSpOpenResFile(&link_spec, fsRdWrPerm);
	if (res_ref != -1)
	{
		AddResource((Handle)alias, 'alis', 0, "\p");
		WriteResource((Handle)alias);
		CloseResFile(res_ref);
	}
	else
	{
		DisposeHandle((Handle)alias);
		vt_write(idx, "ln: cannot write alias resource\r\n");
	}
}

static void cmd_readlink(int idx, int argc, char** argv)
{
	FSSpec spec;
	OSErr e;
	Boolean isFolder, wasAlias;
	char path[512];

	if (argc < 2)
	{
		vt_write(idx, "usage: readlink <alias>\r\n");
		return;
	}

	e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "readlink: not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	/* check if it's actually an alias */
	{
		FInfo finfo;
		e = FSpGetFInfo(&spec, &finfo);
		if (e != noErr || !(finfo.fdFlags & kIsAlias))
		{
			vt_write(idx, "readlink: not an alias: ");
			vt_write(idx, argv[1]);
			vt_write(idx, "\r\n");
			return;
		}
	}

	e = ResolveAliasFile(&spec, true, &isFolder, &wasAlias);
	if (e != noErr)
	{
		vt_write(idx, "readlink: cannot resolve alias\r\n");
		return;
	}

	fsspec_to_path(&spec, path, sizeof(path));
	vt_write(idx, path);
	vt_write(idx, "\r\n");
}

static void cmd_help(int idx, int argc, char* argv[])
{
	static const char* help_lines[] = {
		"SevenTTY local shell - commands:",
		"",
		"  \033[1mFile operations:\033[0m",
		"    ls [-la] [path]    list directory",
		"    cd [path]          change directory",
		"    pwd                print working directory",
		"    cat <file>         show file contents",
		"    head [-n N] <file> show first N lines",
		"    tail [-n N] <file> show last N lines",
		"    grep [-ivnc] s f   search for string in file",
		"    hexdump <file>     hex + ASCII dump",
		"    strings [-n N] f   printable strings in file",
		"    xxd [-r] <file>    xxd-style dump (-r=reverse)",
		"    nl <file>          cat with line numbers",
		"    cut -d. -f1,3 f    extract delimited fields",
		"    fold [-w N] <file> wrap lines to N columns",
		"    rev <file>         reverse each line",
		"    cmp <f1> <f2>      compare two files",
		"    cp <src> <dst>     copy file (both forks)",
		"    mv <old> <new>     rename file",
		"    rm [-i] <file>     delete file (-i=confirm)",
		"    mkdir <name>       create directory",
		"    rmdir <dir>        remove empty directory",
		"    touch <file>       create or update timestamp",
		"    ln -s <tgt> <lnk>  create Mac alias",
		"    readlink <alias>   show alias target",
		"    realpath <path>    full absolute path",
		"    which <command>    find application path",
		"    wc [-lwc] <file>   line/word/byte count",
		"    rot13 <file>       ROT13 encode/decode",
		"    dos2unix <file>    CRLF to LF (in-place)",
		"    unix2dos <file>    LF to CRLF (in-place)",
		"    mac2unix <file>    CR to LF (in-place)",
		"    unix2mac <file>    LF to CR (in-place)",
		"",
		"  \033[1mChecksums:\033[0m",
		"    md5sum <file>      MD5 hash",
		"    sha1sum <file>     SHA-1 hash",
		"    sha256sum <file>   SHA-256 hash",
		"    sha512sum <file>   SHA-512 hash",
		"    crc32 <file>       CRC32 checksum",
		"",
		"  \033[1mMac-specific:\033[0m",
		"    getinfo <file>     show full file info",
		"    chown TYPE:CREA f  set type/creator codes",
		"    settype TYPE f     set file type",
		"    setcreator CREA f  set file creator",
		"    chmod +w|-w f      unlock/lock file",
		"    chattr [+-]li f    set lock/invisible",
		"    label <0-7> f      set Finder label color",
		"",
		"  \033[1mSystem:\033[0m",
		"    basename <path>    filename part of path",
		"    dirname <path>     directory part of path",
		"    seq [first] last   print number sequence",
		"    sleep <seconds>    wait N seconds",
		"    echo [text...]     print text",
		"    clear              clear screen",
		"    df [-m|-h]         show disk usage",
		"    date               show date/time",
		"    uptime             time since boot",
		"    cal                calendar for this month",
		"    uname              show system info",
		"    hostname           computer name",
		"    free [-m|-h]       show memory usage",
		"    ps                 list running processes",
		"    history            command history",
		"    open <path>        launch application",
		"    ssh [user@]h[:p]   open SSH tab",
		"    telnet <h> [port]  open telnet tab",
		"    wget [-n] <url>    HTTP download",
		"    scp [-n] u@h:/p [l]  SCP download",
		"    scp [-n] l u@h:/p    SCP upload",
		"    nc <host> <port>   raw TCP connection",
		"    host <hostname>    DNS lookup",
		"    ping <host> [port] TCP connect test",
		"    ifconfig           show network config",
		"    colors             display color test",
		"    help               this message",
		"    exit               close this tab",
		"",
		"  Paths: use / or : as separator, .. for parent",
		"  Run apps by path: ./SimpleText or /Apps/SimpleText",
		"  Tab completion works for commands and file names.",
		"  Scroll: Shift+PgUp/PgDn (page), Cmd+Up/Down (line)"
	};

	(void)argc;
	(void)argv;
	shell_paged_write(idx, help_lines, sizeof(help_lines) / sizeof(help_lines[0]));
}

/* ------------------------------------------------------------------ */
/* tab completion                                                     */
/* ------------------------------------------------------------------ */

/* unescape "\ " to " " in a path string */
static int path_unescape(const char* src, int src_len, char* dst, int dst_max)
{
	int di = 0;
	int si;
	for (si = 0; si < src_len && di < dst_max - 1; si++)
	{
		if (src[si] == '\\' && si + 1 < src_len && src[si + 1] == ' ')
		{
			dst[di++] = ' ';
			si++; /* skip the space */
		}
		else
		{
			dst[di++] = src[si];
		}
	}
	dst[di] = '\0';
	return di;
}

/* write a string to line buffer, escaping spaces with backslash */
static int escape_and_append(char* line, int pos, int max, const char* src, int src_len)
{
	int i, added = 0;
	for (i = 0; i < src_len && pos + added < max - 1; i++)
	{
		if (src[i] == ' ')
		{
			if (pos + added + 1 >= max - 1) break;
			line[pos + added] = '\\';
			added++;
			line[pos + added] = ' ';
			added++;
		}
		else
		{
			line[pos + added] = src[i];
			added++;
		}
	}
	return added;
}

static const char* shell_commands[] = {
	"basename", "cal", "cat", "cd", "chattr", "chmod", "chown", "clear",
	"cls", "cmp", "colors", "copy", "cp", "crc32", "cut", "date", "del",
	"delete", "df", "dir", "dirname", "dos2unix", "echo", "exit", "file",
	"fold", "free", "getinfo", "grep", "head", "help", "hexdump",
	"history", "host", "hostname", "ifconfig", "info", "label", "less",
	"ln", "ls", "mac2unix", "md", "md5sum", "mkdir", "more", "mv", "nc",
	"nl", "open", "ping", "ps", "pwd", "quit", "rd", "readlink",
	"realpath", "ren", "rename", "rev", "rm", "rmdir", "rot13", "scp", "seq",
	"setcreator", "settype", "sha1sum", "sha256sum", "sha512sum", "sleep",
	"ssh", "strings", "tail", "telnet", "touch", "type", "uname",
	"unix2dos", "unix2mac", "uptime", "wc", "wget", "which", "xxd"
};
#define NUM_SHELL_COMMANDS (sizeof(shell_commands) / sizeof(shell_commands[0]))

static void shell_complete(int idx)
{
	struct session* s = &sessions[idx];
	char* line = s->shell_line;
	int cpos = s->shell_cursor_pos; /* complete at cursor, not end of line */
	int len = s->shell_line_len;

	/* find the start of the word being completed, respecting quotes and escapes */
	int word_start = cpos;
	int in_quotes = 0;

	/* scan backwards: if we find an unmatched quote, the word starts after it */
	{
		int qi;
		int quote_pos = -1;
		int qcount = 0;
		for (qi = 0; qi < cpos; qi++)
		{
			if (line[qi] == '"' || line[qi] == '\'')
				qcount++;
		}
		if (qcount % 2 == 1)
		{
			/* odd number of quotes: we're inside a quoted string */
			for (qi = cpos - 1; qi >= 0; qi--)
			{
				if (line[qi] == '"' || line[qi] == '\'')
				{
					quote_pos = qi;
					break;
				}
			}
			if (quote_pos >= 0)
			{
				word_start = quote_pos + 1;
				in_quotes = 1;
			}
		}

		if (!in_quotes)
		{
			/* scan backward, treating "\ " as non-boundary */
			while (word_start > 0)
			{
				char prev = line[word_start - 1];
				if (prev == ' ' || prev == '\t')
				{
					/* check if this space is escaped */
					if (word_start >= 2 && line[word_start - 2] == '\\')
					{
						word_start -= 2; /* skip both \ and space */
						continue;
					}
					break; /* unescaped space: word boundary */
				}
				word_start--;
			}
		}
	}

	/* extract the raw word (may contain backslash-escapes) */
	char word_raw[256];
	int word_raw_len = cpos - word_start;
	if (word_raw_len > 255) return;
	memcpy(word_raw, line + word_start, word_raw_len);
	word_raw[word_raw_len] = '\0';

	/* unescape the word for path resolution */
	char word[256];
	int word_len = path_unescape(word_raw, word_raw_len, word, sizeof(word));

	/* check if we're completing the first word (command name) */
	{
		int is_first_word = 1;
		int fi;
		for (fi = 0; fi < word_start; fi++)
		{
			if (line[fi] != ' ' && line[fi] != '\t')
			{
				is_first_word = 0;
				break;
			}
		}

		if (is_first_word && word_raw_len > 0)
		{
			/* if word looks like a path, fall through to filesystem completion */
			int looks_like_path = (word[0] == '/' || word[0] == '.'
				|| memchr(word, '/', word_len) != NULL);

			if (!looks_like_path)
			{
			char cmd_match[64];
			int cmd_match_count = 0;
			int cmd_match_len = 0;
			int ci;

			for (ci = 0; ci < (int)NUM_SHELL_COMMANDS; ci++)
			{
				const char* cmd = shell_commands[ci];
				int pi;
				int matches = 1;
				for (pi = 0; pi < word_len && cmd[pi]; pi++)
				{
					if (tolower((unsigned char)word[pi]) != tolower((unsigned char)cmd[pi]))
					{
						matches = 0;
						break;
					}
				}
				if (!matches || pi < word_len) continue;

				cmd_match_count++;
				if (cmd_match_count == 1)
				{
					strncpy(cmd_match, cmd, sizeof(cmd_match) - 1);
					cmd_match[sizeof(cmd_match) - 1] = '\0';
					cmd_match_len = strlen(cmd_match);
				}
				else
				{
					/* find common prefix */
					int mi;
					for (mi = 0; mi < cmd_match_len && cmd[mi]; mi++)
					{
						if (tolower((unsigned char)cmd_match[mi]) != tolower((unsigned char)cmd[mi]))
						{
							cmd_match_len = mi;
							cmd_match[mi] = '\0';
							break;
						}
					}
					if (mi < cmd_match_len)
					{
						cmd_match_len = mi;
						cmd_match[mi] = '\0';
					}
				}
			}

			if (cmd_match_count == 0) return;

			/* insert completion suffix */
			if (cmd_match_len > word_len)
			{
				char* suffix = cmd_match + word_len;
				int suf_len = cmd_match_len - word_len;
				int tail_len = len - cpos;

				if (len + suf_len >= (int)sizeof(s->shell_line)) return;

				if (tail_len > 0)
					memmove(line + cpos + suf_len, line + cpos, tail_len);
				memcpy(line + cpos, suffix, suf_len);
				s->shell_line_len += suf_len;
				s->shell_cursor_pos = cpos + suf_len;
				line[s->shell_line_len] = '\0';

				vt_write_n(idx, line + cpos, suf_len + tail_len);
				if (tail_len > 0)
				{
					char esc[16];
					snprintf(esc, sizeof(esc), "\033[%dD", tail_len);
					vt_write(idx, esc);
				}
			}

			/* single match: append a space */
			if (cmd_match_count == 1)
			{
				int cur = s->shell_cursor_pos;
				int tail2 = s->shell_line_len - cur;
				if (s->shell_line_len < (int)sizeof(s->shell_line) - 1)
				{
					if (tail2 > 0)
						memmove(line + cur + 1, line + cur, tail2);
					line[cur] = ' ';
					s->shell_line_len++;
					s->shell_cursor_pos++;
					line[s->shell_line_len] = '\0';

					vt_write_n(idx, line + cur, 1 + tail2);
					if (tail2 > 0)
					{
						char esc[16];
						snprintf(esc, sizeof(esc), "\033[%dD", tail2);
						vt_write(idx, esc);
					}
				}
			}

			/* multiple matches, no more common prefix: show all */
			if (cmd_match_count > 1 && cmd_match_len <= word_len)
			{
				vt_write(idx, "\r\n");
				for (ci = 0; ci < (int)NUM_SHELL_COMMANDS; ci++)
				{
					const char* cmd = shell_commands[ci];
					int pi;
					int matches = 1;
					for (pi = 0; pi < word_len && cmd[pi]; pi++)
					{
						if (tolower((unsigned char)word[pi]) != tolower((unsigned char)cmd[pi]))
						{
							matches = 0;
							break;
						}
					}
					if (!matches || pi < word_len) continue;

					vt_write(idx, "  ");
					vt_write(idx, cmd);
					vt_write(idx, "\r\n");
				}

				shell_prompt(idx);
				vt_write(idx, line);
				if (s->shell_cursor_pos < s->shell_line_len)
				{
					char esc[16];
					snprintf(esc, sizeof(esc), "\033[%dD",
						s->shell_line_len - s->shell_cursor_pos);
					vt_write(idx, esc);
				}
			}

			return; /* command completion done, skip filesystem completion */
		} /* end if (!looks_like_path) */
		}
	}

	/* split word into directory part and filename prefix at last : or / */
	short comp_vRef = s->shell_vRefNum;
	long comp_dID = s->shell_dirID;
	char prefix[256];
	int prefix_len;
	int dir_part_len = 0; /* length of dir prefix in UNESCAPED word */

	{
		int last_sep = -1;
		int wi;
		for (wi = 0; wi < word_len; wi++)
		{
			if (word[wi] == ':' || word[wi] == '/')
				last_sep = wi;
		}

		if (last_sep >= 0)
		{
			/* resolve the directory portion */
			char dir_path[256];
			memcpy(dir_path, word, last_sep);
			dir_path[last_sep] = '\0';
			dir_part_len = last_sep + 1;

			FSSpec dir_spec;
			OSErr e = resolve_path(idx, dir_path, &dir_spec);
			if (e != noErr || !is_directory(&dir_spec)) return;
			comp_vRef = dir_spec.vRefNum;
			comp_dID = get_dir_id(&dir_spec);

			prefix_len = word_len - dir_part_len;
			memcpy(prefix, word + dir_part_len, prefix_len);
			prefix[prefix_len] = '\0';
		}
		else
		{
			prefix_len = word_len;
			memcpy(prefix, word, prefix_len);
			prefix[prefix_len] = '\0';
		}
	}

	/* enumerate directory looking for matches */
	CInfoPBRec pb;
	Str255 name;
	short index;
	char match[256];
	int match_count = 0;
	int match_len = 0;

	for (index = 1; ; index++)
	{
		memset(&pb, 0, sizeof(pb));
		name[0] = 0;
		pb.hFileInfo.ioNamePtr = name;
		pb.hFileInfo.ioVRefNum = comp_vRef;
		pb.hFileInfo.ioDirID = comp_dID;
		pb.hFileInfo.ioFDirIndex = index;

		if (PBGetCatInfoSync(&pb) != noErr) break;

		char name_c[256];
		{
			int nl = name[0];
			if (nl > 255) nl = 255;
			memcpy(name_c, name + 1, nl);
			name_c[nl] = '\0';
		}

		/* case-insensitive prefix match (using unescaped prefix) */
		if (prefix_len > 0)
		{
			int pi;
			int matches = 1;
			for (pi = 0; pi < prefix_len && name_c[pi]; pi++)
			{
				if (tolower((unsigned char)prefix[pi]) != tolower((unsigned char)name_c[pi]))
				{
					matches = 0;
					break;
				}
			}
			/* also reject if name is shorter than prefix */
			if (!matches || pi < prefix_len) continue;
		}

		match_count++;
		if (match_count == 1)
		{
			strcpy(match, name_c);
			match_len = strlen(match);
		}
		else
		{
			/* multiple matches: find common prefix */
			int ci;
			for (ci = 0; ci < match_len && match[ci] && name_c[ci]; ci++)
			{
				if (tolower((unsigned char)match[ci]) != tolower((unsigned char)name_c[ci]))
				{
					match_len = ci;
					match[ci] = '\0';
					break;
				}
			}
			if (ci < match_len)
			{
				match_len = ci;
				match[ci] = '\0';
			}
		}
	}

	if (match_count == 0) return;

	/* complete: insert the suffix (match chars beyond prefix) at cursor */
	if (match_len > prefix_len)
	{
		char* suffix = match + prefix_len;
		int suf_len = match_len - prefix_len;

		/* figure out how many escaped chars we'll insert */
		char esc_buf[256];
		int added = escape_and_append(esc_buf, 0,
			(int)sizeof(esc_buf), suffix, suf_len);

		int tail_len = len - cpos; /* chars after cursor */
		if (len + added >= (int)sizeof(s->shell_line)) return;

		/* shift tail right to make room */
		if (tail_len > 0)
			memmove(line + cpos + added, line + cpos, tail_len);

		/* insert escaped completion */
		memcpy(line + cpos, esc_buf, added);
		s->shell_line_len += added;
		s->shell_cursor_pos = cpos + added;
		line[s->shell_line_len] = '\0';

		/* redraw: echo inserted text + tail, then move cursor back over tail */
		vt_write_n(idx, line + cpos, added + tail_len);
		if (tail_len > 0)
		{
			char esc[16];
			snprintf(esc, sizeof(esc), "\033[%dD", tail_len);
			vt_write(idx, esc);
		}
	}

	/* if single match, check if it's a directory and add : separator */
	if (match_count == 1 && match_len >= prefix_len)
	{
		char full_match[512];
		if (dir_part_len > 0)
		{
			memcpy(full_match, word, dir_part_len);
			strcpy(full_match + dir_part_len, match);
		}
		else
		{
			strcpy(full_match, match);
		}

		FSSpec spec;
		OSErr e = resolve_path(idx, full_match, &spec);
		if (e == noErr && is_directory(&spec))
		{
			int cur = s->shell_cursor_pos;
			int tail2 = s->shell_line_len - cur;
			if (s->shell_line_len < (int)sizeof(s->shell_line) - 1)
			{
				if (tail2 > 0)
					memmove(line + cur + 1, line + cur, tail2);
				line[cur] = ':';
				s->shell_line_len++;
				s->shell_cursor_pos++;
				line[s->shell_line_len] = '\0';

				/* redraw : + tail, move cursor back */
				vt_write_n(idx, line + cur, 1 + tail2);
				if (tail2 > 0)
				{
					char esc[16];
					snprintf(esc, sizeof(esc), "\033[%dD", tail2);
					vt_write(idx, esc);
				}
			}
		}
	}

	if (match_count > 1 && match_len <= prefix_len)
	{
		/* show all matches */
		vt_write(idx, "\r\n");

		for (index = 1; ; index++)
		{
			memset(&pb, 0, sizeof(pb));
			name[0] = 0;
			pb.hFileInfo.ioNamePtr = name;
			pb.hFileInfo.ioVRefNum = comp_vRef;
			pb.hFileInfo.ioDirID = comp_dID;
			pb.hFileInfo.ioFDirIndex = index;

			if (PBGetCatInfoSync(&pb) != noErr) break;

			char name_c[256];
			{
				int nl = name[0];
				if (nl > 255) nl = 255;
				memcpy(name_c, name + 1, nl);
				name_c[nl] = '\0';
			}

			if (prefix_len > 0)
			{
				int pi;
				int matches = 1;
				for (pi = 0; pi < prefix_len && name_c[pi]; pi++)
				{
					if (tolower((unsigned char)prefix[pi]) != tolower((unsigned char)name_c[pi]))
					{
						matches = 0;
						break;
					}
				}
				if (!matches || pi < prefix_len) continue;
			}

			vt_write(idx, "  ");
			vt_write(idx, name_c);
			vt_write(idx, "\r\n");
		}

		/* reprint prompt and current line, restore cursor position */
		shell_prompt(idx);
		vt_write(idx, line);
		if (s->shell_cursor_pos < s->shell_line_len)
		{
			char esc[16];
			snprintf(esc, sizeof(esc), "\033[%dD", s->shell_line_len - s->shell_cursor_pos);
			vt_write(idx, esc);
		}
	}
}

/* ------------------------------------------------------------------ */
/* command dispatch                                                   */
/* ------------------------------------------------------------------ */

static void shell_execute(int idx, char* line)
{
	char line_copy[256];
	strncpy(line_copy, line, sizeof(line_copy) - 1);
	line_copy[255] = '\0';

	char* argv[MAX_ARGS];
	int argc = parse_args(line_copy, argv, MAX_ARGS);

	if (argc == 0) return;

	char* cmd = argv[0];

	if      (strcmp(cmd, "ls") == 0)         cmd_ls(idx, argc, argv);
	else if (strcmp(cmd, "dir") == 0)        cmd_ls(idx, argc, argv);
	else if (strcmp(cmd, "cd") == 0)         cmd_cd(idx, argc, argv);
	else if (strcmp(cmd, "pwd") == 0)        cmd_pwd(idx, argc, argv);
	else if (strcmp(cmd, "cat") == 0)        cmd_cat(idx, argc, argv);
	else if (strcmp(cmd, "more") == 0)       cmd_cat(idx, argc, argv);
	else if (strcmp(cmd, "less") == 0)       cmd_cat(idx, argc, argv);
	else if (strcmp(cmd, "type") == 0)       cmd_cat(idx, argc, argv);
	else if (strcmp(cmd, "cp") == 0)         cmd_cp(idx, argc, argv);
	else if (strcmp(cmd, "copy") == 0)       cmd_cp(idx, argc, argv);
	else if (strcmp(cmd, "mv") == 0)         cmd_mv(idx, argc, argv);
	else if (strcmp(cmd, "ren") == 0)        cmd_mv(idx, argc, argv);
	else if (strcmp(cmd, "rename") == 0)     cmd_mv(idx, argc, argv);
	else if (strcmp(cmd, "rm") == 0)         cmd_rm(idx, argc, argv);
	else if (strcmp(cmd, "del") == 0)        cmd_rm(idx, argc, argv);
	else if (strcmp(cmd, "delete") == 0)     cmd_rm(idx, argc, argv);
	else if (strcmp(cmd, "mkdir") == 0)       cmd_mkdir(idx, argc, argv);
	else if (strcmp(cmd, "md") == 0)         cmd_mkdir(idx, argc, argv);
	else if (strcmp(cmd, "rmdir") == 0)      cmd_rmdir(idx, argc, argv);
	else if (strcmp(cmd, "rd") == 0)         cmd_rmdir(idx, argc, argv);
	else if (strcmp(cmd, "touch") == 0)      cmd_touch(idx, argc, argv);
	else if (strcmp(cmd, "echo") == 0)       cmd_echo(idx, argc, argv);
	else if (strcmp(cmd, "clear") == 0)      cmd_clear(idx, argc, argv);
	else if (strcmp(cmd, "cls") == 0)        cmd_clear(idx, argc, argv);
	else if (strcmp(cmd, "getinfo") == 0)    cmd_getinfo(idx, argc, argv);
	else if (strcmp(cmd, "info") == 0)       cmd_getinfo(idx, argc, argv);
	else if (strcmp(cmd, "file") == 0)       cmd_getinfo(idx, argc, argv);
	else if (strcmp(cmd, "chown") == 0)      cmd_chown(idx, argc, argv);
	else if (strcmp(cmd, "settype") == 0)    cmd_settype(idx, argc, argv);
	else if (strcmp(cmd, "setcreator") == 0) cmd_setcreator(idx, argc, argv);
	else if (strcmp(cmd, "chmod") == 0)      cmd_chmod(idx, argc, argv);
	else if (strcmp(cmd, "chattr") == 0)     cmd_chattr(idx, argc, argv);
	else if (strcmp(cmd, "label") == 0)      cmd_label(idx, argc, argv);
	else if (strcmp(cmd, "df") == 0)         cmd_df(idx, argc, argv);
	else if (strcmp(cmd, "date") == 0)       cmd_date(idx, argc, argv);
	else if (strcmp(cmd, "uname") == 0)      cmd_uname(idx, argc, argv);
	else if (strcmp(cmd, "free") == 0)       cmd_free(idx, argc, argv);
	else if (strcmp(cmd, "ps") == 0)         cmd_ps(idx, argc, argv);
	else if (strcmp(cmd, "ssh") == 0)        cmd_ssh(idx, argc, argv);
	else if (strcmp(cmd, "telnet") == 0)    cmd_telnet(idx, argc, argv);
	else if (strcmp(cmd, "nc") == 0)        cmd_nc(idx, argc, argv);
	else if (strcmp(cmd, "host") == 0)      cmd_host(idx, argc, argv);
	else if (strcmp(cmd, "ifconfig") == 0)  cmd_ifconfig(idx, argc, argv);
	else if (strcmp(cmd, "ping") == 0)      cmd_ping(idx, argc, argv);
	else if (strcmp(cmd, "colors") == 0)    cmd_colors(idx, argc, argv);
	else if (strcmp(cmd, "open") == 0)      cmd_open(idx, argc, argv);
	else if (strcmp(cmd, "md5sum") == 0)    cmd_md5sum(idx, argc, argv);
	else if (strcmp(cmd, "sha1sum") == 0)   cmd_sha1sum(idx, argc, argv);
	else if (strcmp(cmd, "sha256sum") == 0) cmd_sha256sum(idx, argc, argv);
	else if (strcmp(cmd, "sha512sum") == 0) cmd_sha512sum(idx, argc, argv);
	else if (strcmp(cmd, "crc32") == 0)     cmd_crc32(idx, argc, argv);
	else if (strcmp(cmd, "wc") == 0)        cmd_wc(idx, argc, argv);
	else if (strcmp(cmd, "rot13") == 0)     cmd_rot13(idx, argc, argv);
	else if (strcmp(cmd, "grep") == 0)      cmd_grep(idx, argc, argv);
	else if (strcmp(cmd, "head") == 0)      cmd_head(idx, argc, argv);
	else if (strcmp(cmd, "tail") == 0)      cmd_tail(idx, argc, argv);
	else if (strcmp(cmd, "hexdump") == 0)   cmd_hexdump(idx, argc, argv);
	else if (strcmp(cmd, "strings") == 0)   cmd_strings(idx, argc, argv);
	else if (strcmp(cmd, "uptime") == 0)    cmd_uptime(idx, argc, argv);
	else if (strcmp(cmd, "cal") == 0)       cmd_cal(idx, argc, argv);
	else if (strcmp(cmd, "history") == 0)   cmd_history(idx, argc, argv);
	else if (strcmp(cmd, "basename") == 0) cmd_basename(idx, argc, argv);
	else if (strcmp(cmd, "dirname") == 0)  cmd_dirname(idx, argc, argv);
	else if (strcmp(cmd, "sleep") == 0)    cmd_sleep(idx, argc, argv);
	else if (strcmp(cmd, "nl") == 0)       cmd_nl(idx, argc, argv);
	else if (strcmp(cmd, "cmp") == 0)      cmd_cmp(idx, argc, argv);
	else if (strcmp(cmd, "seq") == 0)      cmd_seq(idx, argc, argv);
	else if (strcmp(cmd, "rev") == 0)      cmd_rev(idx, argc, argv);
	else if (strcmp(cmd, "dos2unix") == 0) cmd_dos2unix(idx, argc, argv);
	else if (strcmp(cmd, "unix2dos") == 0) cmd_unix2dos(idx, argc, argv);
	else if (strcmp(cmd, "mac2unix") == 0) cmd_mac2unix(idx, argc, argv);
	else if (strcmp(cmd, "unix2mac") == 0) cmd_unix2mac(idx, argc, argv);
	else if (strcmp(cmd, "fold") == 0)     cmd_fold(idx, argc, argv);
	else if (strcmp(cmd, "xxd") == 0)      cmd_xxd(idx, argc, argv);
	else if (strcmp(cmd, "cut") == 0)      cmd_cut(idx, argc, argv);
	else if (strcmp(cmd, "realpath") == 0) cmd_realpath(idx, argc, argv);
	else if (strcmp(cmd, "hostname") == 0) cmd_hostname(idx, argc, argv);
	else if (strcmp(cmd, "which") == 0)    cmd_which(idx, argc, argv);
	else if (strcmp(cmd, "ln") == 0)       cmd_ln(idx, argc, argv);
	else if (strcmp(cmd, "readlink") == 0) cmd_readlink(idx, argc, argv);
	else if (strcmp(cmd, "wget") == 0)     cmd_wget(idx, argc, argv);
	else if (strcmp(cmd, "scp") == 0)      cmd_scp(idx, argc, argv);
	else if (strcmp(cmd, "help") == 0)       cmd_help(idx, argc, argv);
	else if (strcmp(cmd, "?") == 0)          cmd_help(idx, argc, argv);
	else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0)
	{
		{
			struct window_context* wc = window_for_session(idx);
			if (wc && wc->num_sessions > 1)
				close_session(idx);
			else if (wc && num_windows > 1)
				close_window(wc - windows);
			else
				exit_requested = 1;
		}
		return;
	}
	else
	{
		/* try to resolve as application path */
		FSSpec spec;
		FInfo finfo;
		OSErr e = resolve_path(idx, cmd, &spec);
		if (e == noErr && FSpGetFInfo(&spec, &finfo) == noErr
			&& finfo.fdType == 'APPL')
		{
			char* open_argv[2];
			open_argv[0] = "open";
			open_argv[1] = cmd;
			cmd_open(idx, 2, open_argv);
		}
		else
		{
			vt_write(idx, cmd);
			vt_write(idx, ": command not found (type 'help')\r\n");
		}
	}
}

/* ------------------------------------------------------------------ */
/* prompt                                                             */
/* ------------------------------------------------------------------ */

void shell_prompt(int idx)
{
	struct session* s = &sessions[idx];
	char dirname[64];
	char esc[20];
	int c = prefs.prompt_color;

	get_dir_name(s->shell_vRefNum, s->shell_dirID, dirname, sizeof(dirname));

	/* color: 0-7 = SGR 30-37, 8-15 = SGR 90-97 */
	if (c >= 8)
		snprintf(esc, sizeof(esc), "\033[1;%dm", 90 + (c - 8));
	else
		snprintf(esc, sizeof(esc), "\033[1;%dm", 30 + c);
	vt_write(idx, esc);
	vt_write(idx, dirname);
	vt_write(idx, "\033[0m \033[1m$\033[0m ");
}

/* ------------------------------------------------------------------ */
/* line editing helpers                                               */
/* ------------------------------------------------------------------ */

/* erase the displayed line text (cursor-position aware) */
static void shell_erase_display(int idx)
{
	struct session* s = &sessions[idx];

	/* move cursor to end of line first */
	if (s->shell_cursor_pos < s->shell_line_len)
	{
		char esc[16];
		snprintf(esc, sizeof(esc), "\033[%dC", s->shell_line_len - s->shell_cursor_pos);
		vt_write(idx, esc);
	}

	/* backspace to erase all chars */
	while (s->shell_line_len > 0)
	{
		vt_write(idx, "\b \b");
		s->shell_line_len--;
	}
	s->shell_cursor_pos = 0;
}

/* redraw from cursor position to end of line, then restore cursor */
static void shell_redraw_from_cursor(int idx)
{
	struct session* s = &sessions[idx];

	/* write remaining chars + space to clear old trailing char */
	vt_write(idx, s->shell_line + s->shell_cursor_pos);
	vt_write(idx, " ");

	/* move cursor back to position */
	{
		int back = s->shell_line_len - s->shell_cursor_pos + 1;
		if (back > 0)
		{
			char esc[16];
			snprintf(esc, sizeof(esc), "\033[%dD", back);
			vt_write(idx, esc);
		}
	}
}

/* ------------------------------------------------------------------ */
/* public interface                                                   */
/* ------------------------------------------------------------------ */

void shell_init(int session_idx)
{
	struct session* s = &sessions[session_idx];
	short vRef;
	long dID;

	/* start in the system disk root, or wherever HGetVol says */
	if (HGetVol(NULL, &vRef, &dID) == noErr)
	{
		s->shell_vRefNum = vRef;
		s->shell_dirID = dID;
	}
	else
	{
		s->shell_vRefNum = 0;
		s->shell_dirID = fsRtDirID;
	}

	s->shell_line[0] = '\0';
	s->shell_line_len = 0;
	s->shell_cursor_pos = 0;
	s->shell_history_count = 0;
	s->shell_history_pos = -1;
	s->shell_saved_line[0] = '\0';
	s->shell_saved_len = 0;

	vt_write(session_idx, "\033[32mS\033[33me\033[31mv\033[35me\033[34mn\033[36mT\033[32mT\033[33mY\033[0m local shell\r\n");
	vt_write(session_idx, "type 'help' for commands\r\n\r\n");

	shell_prompt(session_idx);
}

void shell_input(int session_idx, unsigned char c, int modifiers, unsigned char vkeycode)
{
	struct session* s = &sessions[session_idx];

	/* nc inline mode: worker_mode is WORKER_NC when nc_inline_connect succeeded */
	if (s->worker_mode == WORKER_NC)
	{
		if (s->thread_state == DONE || s->thread_command == EXIT)
		{
			/* connection finished, failed, or disconnect in progress */
			nc_inline_disconnect(session_idx);
			if (s->thread_state != UNINITIALIZED ||
				s->recv_buffer != NULL ||
				s->send_buffer != NULL)
				return;
			vt_write(session_idx, "\r\n(connection closed)\r\n");
			shell_prompt(session_idx);
			return;
		}

		if (s->thread_state == OPEN && s->endpoint != kOTInvalidEndpointRef)
		{
			/* Ctrl+C or Ctrl+D: disconnect
			   right-Ctrl via QEMU sends charcode without controlKey modifier,
			   so also accept bare charcode 3/4 if not numpad Enter or End key */
			if ((c == 3 || c == 4) &&
				((modifiers & controlKey) || (vkeycode != 0x4C && vkeycode != 0x77)))
			{
				nc_inline_disconnect(session_idx);
				s->shell_line_len = 0;
				s->shell_line[0] = '\0';
				vt_write(session_idx, "\r\n(disconnected)\r\n");
				shell_prompt(session_idx);
				return;
			}
			/* Enter or numpad Enter (vkeycode 0x4C): send buffered line + LF */
			if (c == '\r' || vkeycode == 0x4C)
			{
				int i;
				vt_write(session_idx, "\r\n");
				for (i = 0; i < s->shell_line_len; i++)
					tcp_write_s(session_idx, &s->shell_line[i], 1);
				s->send_buffer[0] = '\n';
				tcp_write_s(session_idx, s->send_buffer, 1);
				s->shell_line_len = 0;
				s->shell_line[0] = '\0';
				return;
			}
			/* Backspace */
			if (c == kBackspaceCharCode)
			{
				if (s->shell_line_len > 0)
				{
					s->shell_line_len--;
					s->shell_line[s->shell_line_len] = '\0';
					vt_write(session_idx, "\b \b");
				}
				return;
			}
			/* buffer printable characters */
			if (s->shell_line_len < 254 && c >= 32)
			{
				s->shell_line[s->shell_line_len++] = c;
				s->shell_line[s->shell_line_len] = '\0';
				vt_char(session_idx, c);
			}
			return;
		}

		/* still connecting — Ctrl+C, Ctrl+D, or Escape cancels */
		if (c == 3 || c == 4 || c == '\033')
		{
			nc_inline_disconnect(session_idx);
			vt_write(session_idx, "\r\n(cancelled)\r\n");
			shell_prompt(session_idx);
			return;
		}
		return;
	}

	/* Reap completed local worker threads before handling new key input. */
	if (s->type == SESSION_LOCAL &&
		s->worker_mode != WORKER_NC &&
		s->thread_state == DONE &&
		s->thread_id != kNoThreadID)
	{
		session_reap_thread(session_idx, 0);
	}

	/* While a local worker command is active (e.g. wget), suppress line
	   editing and only allow Ctrl+C to request cancellation. */
	if (local_shell_worker_active(s))
	{
		if ((c == 3 && ((modifiers & controlKey) || vkeycode != 0x4C)) ||
			(modifiers & controlKey && c == 'c'))
		{
			s->thread_command = EXIT;
			if (s->endpoint != kOTInvalidEndpointRef)
				OTCancelSynchronousCalls(s->endpoint, kOTCanceledErr);
			vt_write(session_idx, "^C\r\n");
		}
		return;
	}

	/* ignore page up/down in local shell (handled by scrollback) */
	if (c == kPageUpCharCode || c == kPageDownCharCode)
		return;

	/* Ctrl+D: close tab if line is empty (End key is also charCode 4 but vkeycode 0x77) */
	if (c == 4 && ((modifiers & controlKey) || vkeycode != 0x77))
	{
		if (s->shell_line_len == 0)
		{
			struct window_context* wc = window_for_session(session_idx);
			vt_write(session_idx, "exit\r\n");
			if (wc && wc->num_sessions > 1)
				close_session(session_idx);
			else if (wc && num_windows > 1)
				close_window(wc - windows);
			else
				exit_requested = 1;
		}
		return;
	}

	/* Ctrl+C: cancel line (numpad Enter is also charCode 0x03 but vkeycode 0x4C) */
	if ((c == 3 && ((modifiers & controlKey) || vkeycode != 0x4C)) ||
		(modifiers & controlKey && c == 'c'))
	{
		vt_write(session_idx, "^C\r\n");
		s->shell_line_len = 0;
		s->shell_cursor_pos = 0;
		s->shell_line[0] = '\0';
		shell_prompt(session_idx);
		return;
	}

	/* Ctrl+L: clear screen, reprint prompt */
	if (c == 12)
	{
		vt_write(session_idx, "\033[2J\033[H");
		shell_prompt(session_idx);
		vt_write(session_idx, s->shell_line);
		/* move cursor back to position if not at end */
		if (s->shell_cursor_pos < s->shell_line_len)
		{
			char esc[16];
			snprintf(esc, sizeof(esc), "\033[%dD", s->shell_line_len - s->shell_cursor_pos);
			vt_write(session_idx, esc);
		}
		return;
	}

	/* Ctrl+U: clear line */
	if (c == 21)
	{
		shell_erase_display(session_idx);
		s->shell_line[0] = '\0';
		return;
	}

	/* Ctrl+A / Home: beginning of line */
	if (c == kHomeCharCode)
	{
		if (s->shell_cursor_pos > 0)
		{
			char esc[16];
			snprintf(esc, sizeof(esc), "\033[%dD", s->shell_cursor_pos);
			vt_write(session_idx, esc);
			s->shell_cursor_pos = 0;
		}
		return;
	}

	/* Ctrl+E / End: end of line */
	if (c == kEndCharCode)
	{
		if (s->shell_cursor_pos < s->shell_line_len)
		{
			char esc[16];
			snprintf(esc, sizeof(esc), "\033[%dC", s->shell_line_len - s->shell_cursor_pos);
			vt_write(session_idx, esc);
			s->shell_cursor_pos = s->shell_line_len;
		}
		return;
	}

	/* Tab: completion at cursor position */
	if (c == kTabCharCode)
	{
		shell_complete(session_idx);
		return;
	}

	/* Backspace: delete char before cursor */
	if (c == kBackspaceCharCode)
	{
		if (s->shell_cursor_pos > 0)
		{
			memmove(s->shell_line + s->shell_cursor_pos - 1,
					s->shell_line + s->shell_cursor_pos,
					s->shell_line_len - s->shell_cursor_pos);
			s->shell_line_len--;
			s->shell_cursor_pos--;
			s->shell_line[s->shell_line_len] = '\0';

			vt_write(session_idx, "\b");
			shell_redraw_from_cursor(session_idx);
		}
		return;
	}

	/* Forward delete: delete char after cursor */
	if (c == kDeleteCharCode)
	{
		if (s->shell_cursor_pos < s->shell_line_len)
		{
			memmove(s->shell_line + s->shell_cursor_pos,
					s->shell_line + s->shell_cursor_pos + 1,
					s->shell_line_len - s->shell_cursor_pos - 1);
			s->shell_line_len--;
			s->shell_line[s->shell_line_len] = '\0';

			shell_redraw_from_cursor(session_idx);
		}
		return;
	}

	/* Left arrow */
	if (c == kLeftArrowCharCode)
	{
		if (s->shell_cursor_pos > 0)
		{
			s->shell_cursor_pos--;
			vt_write(session_idx, "\033[D");
		}
		return;
	}

	/* Right arrow */
	if (c == kRightArrowCharCode)
	{
		if (s->shell_cursor_pos < s->shell_line_len)
		{
			s->shell_cursor_pos++;
			vt_write(session_idx, "\033[C");
		}
		return;
	}

	/* Up arrow: previous history entry */
	if (c == kUpArrowCharCode)
	{
		if (s->shell_history_count == 0) return;

		int next_pos;
		if (s->shell_history_pos < 0)
		{
			/* entering history: save current line */
			memcpy(s->shell_saved_line, s->shell_line, s->shell_line_len + 1);
			s->shell_saved_len = s->shell_line_len;
			next_pos = s->shell_history_count - 1;
		}
		else if (s->shell_history_pos > 0)
		{
			next_pos = s->shell_history_pos - 1;
		}
		else
		{
			return; /* already at oldest */
		}

		shell_erase_display(session_idx);

		{
			int hi = next_pos % SHELL_HISTORY_SIZE;
			strcpy(s->shell_line, s->shell_history[hi]);
			s->shell_line_len = strlen(s->shell_line);
			s->shell_cursor_pos = s->shell_line_len;
			s->shell_history_pos = next_pos;
		}
		vt_write(session_idx, s->shell_line);
		return;
	}

	/* Down arrow: next history entry */
	if (c == kDownArrowCharCode)
	{
		if (s->shell_history_pos < 0) return; /* not browsing */

		shell_erase_display(session_idx);

		if (s->shell_history_pos < s->shell_history_count - 1)
		{
			s->shell_history_pos++;
			{
				int hi = s->shell_history_pos % SHELL_HISTORY_SIZE;
				strcpy(s->shell_line, s->shell_history[hi]);
				s->shell_line_len = strlen(s->shell_line);
				s->shell_cursor_pos = s->shell_line_len;
			}
		}
		else
		{
			/* back to the saved line */
			memcpy(s->shell_line, s->shell_saved_line, s->shell_saved_len + 1);
			s->shell_line_len = s->shell_saved_len;
			s->shell_cursor_pos = s->shell_line_len;
			s->shell_history_pos = -1;
		}

		vt_write(session_idx, s->shell_line);
		return;
	}

	/* Enter / Return / numpad Enter (vkeycode 0x4C) */
	if (c == kReturnCharCode || vkeycode == 0x4C)
	{
		vt_write(session_idx, "\r\n");

		if (s->shell_line_len > 0)
		{
			s->shell_line[s->shell_line_len] = '\0';

			/* add to history */
			{
				int hi = s->shell_history_count % SHELL_HISTORY_SIZE;
				strncpy(s->shell_history[hi], s->shell_line, 255);
				s->shell_history[hi][255] = '\0';
				s->shell_history_count++;
			}

			shell_execute(session_idx, s->shell_line);
		}

		/* reset line buffer (session may have been closed by 'exit') */
		if (sessions[session_idx].in_use)
		{
			s->shell_line_len = 0;
			s->shell_cursor_pos = 0;
			s->shell_line[0] = '\0';
			s->shell_history_pos = -1;
			/* suppress prompt if a local worker thread is active */
			if (s->worker_mode != WORKER_NC &&
				!local_shell_worker_active(s))
				shell_prompt(session_idx);
		}
		return;
	}

	/* printable character */
	if (c >= 32 && c < 127)
	{
		if (s->shell_line_len < (int)sizeof(s->shell_line) - 1)
		{
			if (s->shell_cursor_pos < s->shell_line_len)
			{
				/* insert in middle: shift chars right */
				memmove(s->shell_line + s->shell_cursor_pos + 1,
						s->shell_line + s->shell_cursor_pos,
						s->shell_line_len - s->shell_cursor_pos);
			}
			s->shell_line[s->shell_cursor_pos] = c;
			s->shell_line_len++;
			s->shell_line[s->shell_line_len] = '\0';
			s->shell_cursor_pos++;

			if (s->shell_cursor_pos == s->shell_line_len)
			{
				/* at end of line: simple append */
				vt_char(session_idx, c);
			}
			else
			{
				/* in middle: redraw from inserted char onward */
				vt_write(session_idx, s->shell_line + s->shell_cursor_pos - 1);
				/* move cursor back to position */
				{
					int back = s->shell_line_len - s->shell_cursor_pos;
					if (back > 0)
					{
						char esc[16];
						snprintf(esc, sizeof(esc), "\033[%dD", back);
						vt_write(session_idx, esc);
					}
				}
			}
		}
	}
}
