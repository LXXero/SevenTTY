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

#include <Files.h>
#include <Folders.h>
#include <Devices.h>
#include <Gestalt.h>
#include <DateTimeUtils.h>
#include <MacMemory.h>
#include <Processes.h>
#include <TextUtils.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

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

static void cmd_help(int idx, int argc, char* argv[])
{
	vt_write(idx, "SevenTTY local shell - commands:\r\n");
	vt_write(idx, "\r\n");
	vt_write(idx, "  \033[1mFile operations:\033[0m\r\n");
	vt_write(idx, "    ls [-la] [path]    list directory\r\n");
	vt_write(idx, "    cd [path]          change directory\r\n");
	vt_write(idx, "    pwd                print working directory\r\n");
	vt_write(idx, "    cat <file>         show file contents\r\n");
	vt_write(idx, "    cp <src> <dst>     copy file (both forks)\r\n");
	vt_write(idx, "    mv <old> <new>     rename file\r\n");
	vt_write(idx, "    rm <file>          delete file\r\n");
	vt_write(idx, "    mkdir <name>       create directory\r\n");
	vt_write(idx, "    rmdir <dir>        remove empty directory\r\n");
	vt_write(idx, "    touch <file>       create or update timestamp\r\n");
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
	vt_write(idx, "    echo [text...]     print text\r\n");
	vt_write(idx, "    clear              clear screen\r\n");
	vt_write(idx, "    df [-m|-h]         show disk usage\r\n");
	vt_write(idx, "    date               show date/time\r\n");
	vt_write(idx, "    uname              show system info\r\n");
	vt_write(idx, "    free [-m|-h]       show memory usage\r\n");
	vt_write(idx, "    ps                 list running processes\r\n");
	vt_write(idx, "    ssh [user@]h[:p]   open SSH tab\r\n");
	vt_write(idx, "    colors             display color test\r\n");
	vt_write(idx, "    help               this message\r\n");
	vt_write(idx, "    exit               close this tab\r\n");
	vt_write(idx, "\r\n");
	vt_write(idx, "  Paths: use / or : as separator, .. for parent\r\n");
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
	"cat", "cd", "chattr", "chmod", "chown", "clear", "cls", "colors",
	"copy", "cp", "date", "del", "delete", "df", "dir", "echo", "exit",
	"file", "free", "getinfo", "help", "info", "label", "less", "ls",
	"md", "mkdir", "more", "mv", "ps", "pwd", "quit", "rd", "ren",
	"rename", "rm", "rmdir", "setcreator", "settype", "ssh", "touch",
	"type", "uname"
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
	else if (strcmp(cmd, "colors") == 0)    cmd_colors(idx, argc, argv);
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
		vt_write(idx, cmd);
		vt_write(idx, ": command not found (type 'help')\r\n");
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

	vt_write(session_idx, "\033[2J\033[H");
	vt_write(session_idx, "SevenTTY local shell\r\n");
	vt_write(session_idx, "type 'help' for commands\r\n\r\n");

	shell_prompt(session_idx);
}

void shell_input(int session_idx, unsigned char c, int modifiers)
{
	struct session* s = &sessions[session_idx];

	/* ignore page up/down, home, end in local shell (handled by scrollback) */
	if (c == kPageUpCharCode || c == kPageDownCharCode ||
		c == kHomeCharCode || c == kEndCharCode)
		return;

	/* Ctrl+D: close tab if line is empty */
	if (c == 4)
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

	/* Ctrl+C: cancel line (numpad Enter is also charCode 0x03 but without controlKey) */
	if (modifiers & controlKey && (c == 3 || c == 'c'))
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

	/* Backspace / Delete */
	if (c == kBackspaceCharCode || c == kDeleteCharCode)
	{
		if (s->shell_cursor_pos > 0)
		{
			/* delete char before cursor */
			memmove(s->shell_line + s->shell_cursor_pos - 1,
					s->shell_line + s->shell_cursor_pos,
					s->shell_line_len - s->shell_cursor_pos);
			s->shell_line_len--;
			s->shell_cursor_pos--;
			s->shell_line[s->shell_line_len] = '\0';

			/* move back, redraw rest of line */
			vt_write(session_idx, "\b");
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

	/* Enter / Return */
	if (c == kReturnCharCode || c == kEnterCharCode)
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
