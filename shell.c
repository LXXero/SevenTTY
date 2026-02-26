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

static void cmd_rm(int idx, int argc, char* argv[])
{
	if (argc < 2)
	{
		vt_write(idx, "usage: rm <file>\r\n");
		return;
	}

	FSSpec spec;
	OSErr e = resolve_path(idx, argv[1], &spec);
	if (e != noErr)
	{
		vt_write(idx, "rm: file not found: ");
		vt_write(idx, argv[1]);
		vt_write(idx, "\r\n");
		return;
	}

	e = FSpDelete(&spec);
	if (e == fLckdErr)
		vt_write(idx, "rm: file is locked\r\n");
	else if (e == fBsyErr)
		vt_write(idx, "rm: file is busy\r\n");
	else if (e != noErr)
		vt_write(idx, "rm: delete failed\r\n");
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

			/* end of line? (\r, \n, or \r\n) */
			if (c == '\r' || c == '\n')
			{
				const char* found;
				int matched;

				/* skip \n after \r */
				if (c == '\r' && i + 1 < count && buf[i + 1] == '\n')
					i++;

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

	/* seek to the right line and output */
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

static void cmd_cal(int idx, int argc, char** argv)
{
	unsigned long secs;
	DateTimeRec dt;
	int year, month;
	int dow_first, days_in_month;
	int day;
	char buf[32];
	static const char* month_names[] = {
		"", "January", "February", "March", "April",
		"May", "June", "July", "August", "September",
		"October", "November", "December"
	};
	static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};

	(void)argc;
	(void)argv;

	GetDateTime(&secs);
	SecondsToDate(secs, &dt);
	year = dt.year;
	month = dt.month;

	/* days in this month */
	days_in_month = mdays[month];
	if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
		days_in_month = 29;

	/* day of week for the 1st (Zeller's congruence) */
	{
		int y = year, m = month;
		if (m < 3) { m += 12; y--; }
		dow_first = (1 + (13 * (m + 1)) / 5 + y + y / 4 - y / 100 + y / 400) % 7;
		/* Zeller: 0=Sat, 1=Sun, ..., 6=Fri -> convert to 0=Sun */
		dow_first = (dow_first + 6) % 7;
	}

	/* header */
	snprintf(buf, sizeof(buf), "   %s %d", month_names[month], year);
	vt_write(idx, buf);
	vt_write(idx, "\r\n");
	vt_write(idx, "Su Mo Tu We Th Fr Sa\r\n");

	/* leading spaces */
	{
		int s;
		for (s = 0; s < dow_first; s++)
			vt_write(idx, "   ");
	}

	for (day = 1; day <= days_in_month; day++)
	{
		if (day == dt.day)
		{
			snprintf(buf, sizeof(buf), "\033[7m%2d\033[0m", day);
			vt_write(idx, buf);
		}
		else
		{
			snprintf(buf, sizeof(buf), "%2d", day);
			vt_write(idx, buf);
		}

		if ((dow_first + day) % 7 == 0)
			vt_write(idx, "\r\n");
		else
			vt_write(idx, " ");
	}

	/* trailing newline if row didn't end on Saturday */
	if ((dow_first + days_in_month) % 7 != 0)
		vt_write(idx, "\r\n");
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
								int hi, lo;
								while (*p == ' ') p++;
								if (*p == '\0' || *p == ' ') break;
								/* stop at ASCII column (two spaces before it) */
								if (p[0] == ' ' && p[1] == ' ') break;

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
	vt_write(idx, "SevenTTY local shell - commands:\r\n");
	vt_write(idx, "\r\n");
	vt_write(idx, "  \033[1mFile operations:\033[0m\r\n");
	vt_write(idx, "    ls [-la] [path]    list directory\r\n");
	vt_write(idx, "    cd [path]          change directory\r\n");
	vt_write(idx, "    pwd                print working directory\r\n");
	vt_write(idx, "    cat <file>         show file contents\r\n");
	vt_write(idx, "    head [-n N] <file> show first N lines\r\n");
	vt_write(idx, "    tail [-n N] <file> show last N lines\r\n");
	vt_write(idx, "    grep [-ivnc] s f   search for string in file\r\n");
	vt_write(idx, "    hexdump <file>     hex + ASCII dump\r\n");
	vt_write(idx, "    strings [-n N] f   printable strings in file\r\n");
	vt_write(idx, "    hexdump and xxd are different formats:\r\n");
	vt_write(idx, "    xxd [-r] <file>    xxd-style dump (-r=reverse)\r\n");
	vt_write(idx, "    nl <file>          cat with line numbers\r\n");
	vt_write(idx, "    cut -d. -f1,3 f    extract delimited fields\r\n");
	vt_write(idx, "    fold [-w N] <file> wrap lines to N columns\r\n");
	vt_write(idx, "    rev <file>         reverse each line\r\n");
	vt_write(idx, "    cmp <f1> <f2>      compare two files\r\n");
	vt_write(idx, "    cp <src> <dst>     copy file (both forks)\r\n");
	vt_write(idx, "    mv <old> <new>     rename file\r\n");
	vt_write(idx, "    rm <file>          delete file\r\n");
	vt_write(idx, "    mkdir <name>       create directory\r\n");
	vt_write(idx, "    rmdir <dir>        remove empty directory\r\n");
	vt_write(idx, "    touch <file>       create or update timestamp\r\n");
	vt_write(idx, "    ln -s <tgt> <lnk>  create Mac alias\r\n");
	vt_write(idx, "    readlink <alias>   show alias target\r\n");
	vt_write(idx, "    realpath <path>    full absolute path\r\n");
	vt_write(idx, "    which <command>    find application path\r\n");
	vt_write(idx, "    wc [-lwc] <file>   line/word/byte count\r\n");
	vt_write(idx, "    rot13 <file>       ROT13 encode/decode\r\n");
	vt_write(idx, "    dos2unix <file>    CRLF to LF (in-place)\r\n");
	vt_write(idx, "    unix2dos <file>    LF to CRLF (in-place)\r\n");
	vt_write(idx, "    mac2unix <file>    CR to LF (in-place)\r\n");
	vt_write(idx, "    unix2mac <file>    LF to CR (in-place)\r\n");
	vt_write(idx, "\r\n");
	vt_write(idx, "  \033[1mChecksums:\033[0m\r\n");
	vt_write(idx, "    md5sum <file>      MD5 hash\r\n");
	vt_write(idx, "    sha1sum <file>     SHA-1 hash\r\n");
	vt_write(idx, "    sha256sum <file>   SHA-256 hash\r\n");
	vt_write(idx, "    sha512sum <file>   SHA-512 hash\r\n");
	vt_write(idx, "    crc32 <file>       CRC32 checksum\r\n");
	vt_write(idx, "\r\n");
	vt_write(idx, "  \033[1mMac-specific:\033[0m\r\n");
	vt_write(idx, "    getinfo <file>     show full file info\r\n");
	vt_write(idx, "    chown TYPE:CREA f  set type/creator codes\r\n");
	vt_write(idx, "    settype TYPE f     set file type\r\n");
	vt_write(idx, "    setcreator CREA f  set file creator\r\n");
	vt_write(idx, "    chmod +w|-w f      unlock/lock file\r\n");
	vt_write(idx, "    chattr [+-]li f    set lock/invisible\r\n");
	vt_write(idx, "    label <0-7> f      set Finder label color\r\n");
	vt_write(idx, "\r\n");
	vt_write(idx, "  \033[1mSystem:\033[0m\r\n");
	vt_write(idx, "    basename <path>    filename part of path\r\n");
	vt_write(idx, "    dirname <path>     directory part of path\r\n");
	vt_write(idx, "    seq [first] last   print number sequence\r\n");
	vt_write(idx, "    sleep <seconds>    wait N seconds\r\n");
	vt_write(idx, "    echo [text...]     print text\r\n");
	vt_write(idx, "    clear              clear screen\r\n");
	vt_write(idx, "    df [-m|-h]         show disk usage\r\n");
	vt_write(idx, "    date               show date/time\r\n");
	vt_write(idx, "    uptime             time since boot\r\n");
	vt_write(idx, "    cal                calendar for this month\r\n");
	vt_write(idx, "    uname              show system info\r\n");
	vt_write(idx, "    hostname           computer name\r\n");
	vt_write(idx, "    free [-m|-h]       show memory usage\r\n");
	vt_write(idx, "    ps                 list running processes\r\n");
	vt_write(idx, "    history            command history\r\n");
	vt_write(idx, "    open <path>        launch application\r\n");
	vt_write(idx, "    ssh [user@]h[:p]   open SSH tab\r\n");
	vt_write(idx, "    telnet <h> [port]  open telnet tab\r\n");
	vt_write(idx, "    nc <host> <port>   raw TCP connection\r\n");
	vt_write(idx, "    host <hostname>    DNS lookup\r\n");
	vt_write(idx, "    ping <host> [port] TCP connect test\r\n");
	vt_write(idx, "    ifconfig           show network config\r\n");
	vt_write(idx, "    colors             display color test\r\n");
	vt_write(idx, "    help               this message\r\n");
	vt_write(idx, "    exit               close this tab\r\n");
	vt_write(idx, "\r\n");
	vt_write(idx, "  Paths: use / or : as separator, .. for parent\r\n");
	vt_write(idx, "  Run apps by path: ./SimpleText or /Apps/SimpleText\r\n");
	vt_write(idx, "  Tab completion works for commands and file names.\r\n");
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
	"realpath", "ren", "rename", "rev", "rm", "rmdir", "rot13", "seq",
	"setcreator", "settype", "sha1sum", "sha256sum", "sha512sum", "sleep",
	"ssh", "strings", "tail", "telnet", "touch", "type", "uname",
	"unix2dos", "unix2mac", "uptime", "wc", "which", "xxd"
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

	/* nc inline mode: recv_buffer is non-NULL when nc_inline_connect was called */
	if (s->recv_buffer != NULL && s->type == SESSION_LOCAL)
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
			/* suppress prompt if nc inline is active (thread running) */
			if (!(s->recv_buffer != NULL && s->type == SESSION_LOCAL))
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
