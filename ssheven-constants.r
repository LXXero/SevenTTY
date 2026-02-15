/* used as both a C and resource file include */

#ifndef __SSHEVEN_CONSTANTS_R__
#define __SSHEVEN_CONSTANTS_R__

/* so many versions */
#define SSHEVEN_VERSION             "1.0.0"
#define SSHEVEN_LONG_VERSION        "1.0.0, SevenTTY fork by LXXero"
#define SSHEVEN_DESCRIPTION         "SevenTTY 1.0.0 - SSH & shell for classic Mac OS"
#define SSHEVEN_VERSION_MAJOR       0x01
#define SSHEVEN_VERSION_MINOR       0x00
#define SSHEVEN_VERSION_PRERELEASE  0x00

/* options: development, alpha, beta, release */
#define SSHEVEN_RELEASE_TYPE        release
#define SSHEVEN_RELEASE_REGION      verUS

/* requested number of bytes for RAM, used in SIZE resource */
#define SSHEVEN_MINIMUM_PARTITION   2*1024*1024
#define SSHEVEN_REQUIRED_PARTITION  SSHEVEN_MINIMUM_PARTITION

/* size in bytes for recv and send thread buffers */
/* making this too large is bad for responsiveness on 68k machines */
#define SSHEVEN_BUFFER_SIZE 4*1024

/* default terminal string */
#define SSHEVEN_DEFAULT_TERM_STRING "xterm-16color"

/* name for the preferences file (pascal string) */
#define PREFERENCES_FILENAME "\pSevenTTY Preferences"

/* application icon set */
#define SSHEVEN_APPLICATION_ICON 128

/* preferences/other files icon set */
#define SSHEVEN_FILE_ICON 129

/* dialog for getting connection info */
#define DLOG_CONNECT 128
#define DITL_CONNECT 128

/* alert for failure to find OT */
#define ALRT_OT 129
#define DITL_OT 129

/* alert for failure to find thread manager */
#define ALRT_TM 130
#define DITL_TM 130

/* alert for slow CPU detected */
#define ALRT_CPU_SLOW 131
#define DITL_CPU_SLOW 131

/* alert for pre-68020 detected */
#define ALRT_CPU_BAD 132
#define DITL_CPU_BAD 132

/* about info window */
#define DLOG_ABOUT 133
#define DITL_ABOUT 133
#define PICT_ABOUT 133

/* password entry window */
#define DLOG_PASSWORD 134
#define DITL_PASSWORD 134

/* alert for password authentication failure */
#define ALRT_PW_FAIL 135
#define DITL_PW_FAIL 135

/* alert for requesting public key */
#define ALRT_PUBKEY 136
#define DITL_PUBKEY 136

/* alert for requesting private key */
#define ALRT_PRIVKEY 137
#define DITL_PRIVKEY 137

/* alert for requesting key decryption password */
#define DLOG_KEY_PASSWORD 138
#define DITL_KEY_PASSWORD 138

/* alert for key file read failure */
#define ALRT_FILE_FAIL 139
#define DITL_FILE_FAIL 139

/* dialog for preferences */
#define DLOG_PREFERENCES 140
#define DITL_PREFERENCES 140

/* dialog for known host check */
#define DLOG_NEW_HOST 141
#define DITL_NEW_HOST 141

/* controls for preferences dialog */
#define CNTL_PREF_FG_COLOR  128
#define CNTL_PREF_BG_COLOR  129
#define CNTL_PREF_TERM_TYPE 130
#define CNTL_PREF_FONT_SIZE 131

/* menus */
#define MBAR_SSHEVEN   128

#define MENU_APPLE     128
#define MENU_FILE      129
#define MENU_EDIT      130
#define MENU_COLOR     131
#define MENU_TERM_TYPE 132
#define MENU_FONT_SIZE 133

/* File menu item indices */
#define FMENU_CONNECT     1
#define FMENU_DISCONNECT  2
/* separator = 3 */
#define FMENU_NEW_WINDOW  4
#define FMENU_NEW_LOCAL   5
#define FMENU_NEW_SSH     6
#define FMENU_CLOSE_TAB   7
/* separator = 8 */
#define FMENU_PREFS       9
/* separator = 10 */
#define FMENU_QUIT        11

#endif
