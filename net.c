/*
 * SevenTTY (based on ssheven by cy384)
 *
 * Copyright (c) 2021 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#include "app.h"
#include "net.h"
#include "console.h"
#include "debug.h"

#include <errno.h>
#include <Script.h>
#include <Threads.h>

#include <mbedtls/base64.h>

void ssh_write_s(int session_idx, char* buf, size_t len)
{
	struct session* s = &sessions[session_idx];

	if (s->thread_state == OPEN && s->thread_command != EXIT)
	{
		int r = libssh2_channel_write(s->channel, buf, len);

		if (r < 1)
		{
			printf_s(session_idx, "Failed to write to channel, closing connection.\r\n");
			s->thread_command = EXIT;
		}
	}
}

void ssh_write(char* buf, size_t len)
{
	ssh_write_s(active_session_global(), buf, len);
}

// read from the channel and print to console
void ssh_read(int session_idx)
{
	struct session* s = &sessions[session_idx];
	ssize_t rc = libssh2_channel_read(s->channel, s->recv_buffer, SSH_BUFFER_SIZE);

	if (rc == 0) return;

	if (rc <= 0)
	{
		/* only report the error if we weren't told to shut down */
		if (s->thread_command != EXIT)
		{
			printf_s(session_idx, "channel read error: %s\r\n", libssh2_error_string(rc));
			s->thread_command = EXIT;
		}
	}

	while (rc > 0)
	{
		rc -= vterm_input_write(s->vterm, s->recv_buffer, rc);
	}
}

void end_connection(int session_idx)
{
	struct session* s = &sessions[session_idx];
	s->thread_state = CLEANUP;

	OSStatus err = noErr;

	if (s->ssh_session)
	{
		/* switch to non-blocking so cleanup calls don't hang */
		libssh2_session_set_blocking(s->ssh_session, 0);
	}

	if (s->channel)
	{
		libssh2_channel_send_eof(s->channel);
		libssh2_channel_close(s->channel);
		libssh2_channel_free(s->channel);
		s->channel = NULL;
	}

	if (s->ssh_session)
	{
		libssh2_session_disconnect(s->ssh_session, "Normal Shutdown, Thank you for playing");
		libssh2_session_free(s->ssh_session);
		s->ssh_session = NULL;
	}

	libssh2_exit();

	if (s->endpoint != kOTInvalidEndpointRef)
	{
		// request to close the TCP connection
		OTSndOrderlyDisconnect(s->endpoint);

		// discard remaining data so we can finish closing the connection
		// limit iterations to prevent infinite loop if endpoint is stuck
		{
			int rc = 1;
			int drain_count = 0;
			OTFlags ot_flags;
			while (rc != kOTLookErr && drain_count < 1000)
			{
				rc = OTRcv(s->endpoint, s->recv_buffer, 1, &ot_flags);
				drain_count++;
			}
		}

		// finish closing the TCP connection
		OSStatus result = OTLook(s->endpoint);

		switch (result)
		{
			case T_DISCONNECT:
				OTRcvDisconnect(s->endpoint, nil);
				break;

			case T_ORDREL:
				err = OTRcvOrderlyDisconnect(s->endpoint);
				if (err == noErr)
				{
					err = OTSndOrderlyDisconnect(s->endpoint);
				}
				break;

			default:
				break;
		}

		// release endpoint
		OTUnbind(s->endpoint);
		OTCloseProvider(s->endpoint);
		s->endpoint = kOTInvalidEndpointRef;
	}

	s->thread_state = DONE;
}


int check_network_events(int session_idx)
{
	struct session* s = &sessions[session_idx];
	int ok = 1;
	OSStatus err = noErr;

	// check if we have any new network events
	OTResult look_result = OTLook(s->endpoint);

	switch (look_result)
	{
		case T_DATA:
		case T_EXDATA:
			// got data
			// we always try to read, so ignore this event
			break;

		case T_RESET:
			// connection reset? close it/give up
			end_connection(session_idx);
			ok = 0;
			break;

		case T_DISCONNECT:
			// got disconnected
			OTRcvDisconnect(s->endpoint, nil);
			end_connection(session_idx);
			ok = 0;
			break;

		case T_ORDREL:
			// nice tcp disconnect requested by remote
			err = OTRcvOrderlyDisconnect(s->endpoint);
			if (err == noErr)
			{
				err = OTSndOrderlyDisconnect(s->endpoint);
			}
			end_connection(session_idx);
			ok = 0;
			break;

		default:
			// something weird or irrelevant: ignore it
			break;
	}

	return ok;
}

int ssh_setup_terminal(int session_idx)
{
	struct session* s = &sessions[session_idx];
	int rc = 0;

	struct window_context* wc = window_for_session(session_idx);
	int cols = wc ? wc->size_x : 80;
	int rows = wc ? wc->size_y : 24;
	SSH_CHECK(libssh2_channel_request_pty_ex(s->channel, prefs.terminal_string, (strlen(prefs.terminal_string)), NULL, 0, cols, rows, 0, 0));

	/* try to set COLORTERM — server may reject this (AcceptEnv), that's OK */
	libssh2_channel_setenv(s->channel, "COLORTERM", "truecolor");

	SSH_CHECK(libssh2_channel_shell(s->channel));

	s->thread_state = OPEN;

	return 1;
}

// returns base64 sha256 hash of key as a malloc'd pascal string
char* host_hash(int session_idx)
{
	struct session* s = &sessions[session_idx];
	size_t length = 0;
	char* human_readable = malloc(66);
	memset(human_readable, 0, 66);
	const char* host_key_hash = NULL;

	host_key_hash = libssh2_hostkey_hash(s->ssh_session, LIBSSH2_HOSTKEY_HASH_SHA256);
	mbedtls_base64_encode((unsigned char*)human_readable+1, 64, &length, (unsigned const char*)host_key_hash, 32);

	human_readable[0] = (unsigned char)length;

	return human_readable;
}

char* known_hosts_full_path(int* found)
{
	int ok = 1;
	short foundVRefNum = 0;
	long foundDirID = 0;
	FSSpec known_hosts_file;
	*found = 0;

	OSType pref_type = 'SH7p';
	OSType creator_type = 'SSH7';

	// find the preferences folder on the system disk, create folder if needed
	OSErr e = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &foundVRefNum, &foundDirID);
	if (e != noErr) ok = 0;

	// make an FSSpec for the new file we want to make
	if (ok)
	{
		e = FSMakeFSSpec(foundVRefNum, foundDirID, "\pknown_hosts", &known_hosts_file);

		// if the file exists, we found it else make an empty one
		if (e == noErr) *found = 1;
		else if (e == noErr) e = FSpCreate(&known_hosts_file, creator_type, pref_type, smSystemScript);

		if (e != noErr) ok = 0;
	}

	Handle full_path_handle;
	int path_length;

	FSpPathFromLocation(&known_hosts_file, &path_length, &full_path_handle);
	char* full_path = malloc(path_length+1);
	strncpy(full_path, (char*)(*full_path_handle), path_length+1);
	DisposeHandle(full_path_handle);

	return full_path;
}

int known_hosts(int session_idx)
{
	struct session* s = &sessions[session_idx];
	int safe_to_connect = 1;
	int recognized_key = 0;
	int known_hosts_file_exists = 0;

	char* known_hosts_file_path = known_hosts_full_path(&known_hosts_file_exists);
	char* hash_string = NULL;

	size_t key_len = 0;
	int key_type = 0;
	const char* host_key = libssh2_session_hostkey(s->ssh_session, &key_len, &key_type);

	LIBSSH2_KNOWNHOSTS* kh = libssh2_knownhost_init(s->ssh_session);

	if (known_hosts_file_exists)
	{
		// load known hosts file

		int e = libssh2_knownhost_readfile(kh, known_hosts_file_path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
		if (e < 0)
		{
			printf_s(session_idx, "Failed to load known hosts file: %s\r\n", libssh2_error_string(e));
		}
	}
	else
	{
		printf_s(session_idx, "No known hosts file found.\r\n");
	}

	if (safe_to_connect)
	{
		// hostnames need to be either plain or of the format "[host]:port"
		prefs.hostname[prefs.hostname[0]+1] = '\0';
		int e = libssh2_knownhost_check(kh, prefs.hostname+1, host_key, key_len, LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW, NULL);
		prefs.hostname[prefs.hostname[0]+1] = ':';

		switch (e)
		{
			case LIBSSH2_KNOWNHOST_CHECK_FAILURE:
				printf_s(session_idx, "Failed to check known hosts against server public key!\r\n");
				safe_to_connect = 0;
				break;
			case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
				printf_s(session_idx, "No matching host found.\r\n");
				break;
			case LIBSSH2_KNOWNHOST_CHECK_MATCH:
				//printf_s(session_idx, "Host and key match found in known hosts.\r\n");
				recognized_key = 1;
				break;
			case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
				printf_s(session_idx, "WARNING! Host found in known hosts but key doesn't match!\r\n");
				safe_to_connect = 0;
				break;
			default:
				printf_s(session_idx, "Unexpected error while checking known-hosts: %d\r\n", e);
				safe_to_connect = 0;
				break;
		}
	}

	hash_string = host_hash(session_idx);
	//printf_s(session_idx, "Host key hash (SHA256): %s\r\n", hash_string+1);

	// ask the user to confirm if we're seeing a new host+key combo
	if (safe_to_connect && !recognized_key)
	{
		// ask the user if the key is OK
		DialogPtr dlg = GetNewDialog(DLOG_NEW_HOST, 0, (WindowPtr)-1);

		DialogItemType type;
		Handle itemH;
		Rect box;

		// draw default button indicator around the connect button
		GetDialogItem(dlg, 2, &type, &itemH, &box);
		SetDialogItem(dlg, 2, type, (Handle)NewUserItemUPP(&ButtonFrameProc), &box);

		// write the hash string into the dialog window
		ControlHandle hash_text_box;
		GetDialogItem(dlg, 4, &type, &itemH, &box);
		hash_text_box = (ControlHandle)itemH;
		SetDialogItemText((Handle)hash_text_box, (ConstStr255Param)hash_string);

		// let the modalmanager do everything
		// stop on reject or accept
		short item;
		do {
			ModalDialog(NULL, &item);
		} while(item != 1 && item != 5);

		// reject if user hit reject
		if (item == 1)
		{
			safe_to_connect = 0;
		}

		DisposeDialog(dlg);
		FlushEvents(everyEvent, -1);

		printf_s(session_idx, "Saving host and key... ");

		int save_type = 0;
		save_type |= LIBSSH2_KNOWNHOST_TYPE_PLAIN;
		save_type |= LIBSSH2_KNOWNHOST_KEYENC_RAW;

		switch (key_type)
		{
			default:
				__attribute__ ((fallthrough));
			case LIBSSH2_HOSTKEY_TYPE_UNKNOWN:
				save_type |= LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
				break;
			case LIBSSH2_HOSTKEY_TYPE_RSA:
				save_type |= LIBSSH2_KNOWNHOST_KEY_SSHRSA;
				break;
			case LIBSSH2_HOSTKEY_TYPE_DSS:
				save_type |= LIBSSH2_KNOWNHOST_KEY_SSHDSS;
				break;
			case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
				save_type |= LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
				break;
			case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
				save_type |= LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
				break;
			case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
				save_type |= LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
				break;
			case LIBSSH2_HOSTKEY_TYPE_ED25519:
				save_type |= LIBSSH2_KNOWNHOST_KEY_ED25519;
				break;
		}

		// hostnames need to be either plain or of the format "[host]:port"
		prefs.hostname[prefs.hostname[0]+1] = '\0';
		int e = libssh2_knownhost_addc(kh, prefs.hostname+1, NULL, host_key, key_len, NULL, 0, save_type, NULL);
		prefs.hostname[prefs.hostname[0]+1] = ':';

		if (e != 0) printf_s(session_idx, "failed to add to known hosts: %s\r\n", libssh2_error_string(e));
		else
		{
			e = libssh2_knownhost_writefile(kh, known_hosts_file_path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
			if (e != 0) printf_s(session_idx, "failed to save known hosts file: %s\r\n", libssh2_error_string(e));
			else printf_s(session_idx, "done.\r\n");
		}
	}

	free(hash_string);

	libssh2_knownhost_free(kh);

	free(known_hosts_file_path);

	return safe_to_connect;
}


ssize_t network_recv_callback(libssh2_socket_t sock, void *buffer,
               size_t length, int flags, void **abstract)
{
	int idx = (int)(intptr_t)*abstract;
	struct session* s = &sessions[idx];
	OTResult ret = kOTNoDataErr;
	OTFlags ot_flags = 0;

	if (length == 0) return 0;

	// in non-blocking mode, returns instantly always
	ret = OTRcv(s->endpoint, buffer, length, &ot_flags);

	// if we got bytes, return them
	if (ret >= 0) return ret;

	// if no data, let other threads run, then tell caller to call again
	if (ret == kOTNoDataErr && s->thread_command != EXIT)
	{
		YieldToAnyThread();
		return -EAGAIN;
	}

	// if we got anything other than data or nothing, return an error
	if (ret != kOTNoDataErr) return -1;

	return -1;
}

ssize_t network_send_callback(libssh2_socket_t sock, const void *buffer,
               size_t length, int flags, void **abstract)
{
	int idx = (int)(intptr_t)*abstract;
	struct session* s = &sessions[idx];
    int ret = -1;

    ret = OTSnd(s->endpoint, (void*) buffer, length, 0);

    // TODO FIXME handle cases better, i.e. translate error cases
    if (ret == kOTLookErr)
    {
        OTResult lookresult = OTLook(s->endpoint);
        //printf("kOTLookErr, reason: %ld\n", lookresult);

        switch (lookresult)
        {
            default:
               //printf("what?\n");
               ret = -1;
               break;
        }
    }

    return (ssize_t) ret;
}

void ssh_end_msg_callback(LIBSSH2_SESSION* session, int reason, const char *message,
           int message_len, const char *language, int language_len,
           void **abstract)
{
	printf_i("got a disconnect msg\r\n");
}

int init_connection(int session_idx, char* hostname)
{
	struct session* s = &sessions[session_idx];
	int rc;

	// OT vars
	OSStatus err = noErr;
	TCall sndCall;
	DNSAddress hostDNSAddress;


	// open TCP endpoint
	s->endpoint = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, nil, &err);

	if (err != noErr || s->endpoint == kOTInvalidEndpointRef)
	{
		printf_s(session_idx, "Failed to open Open Transport TCP endpoint.\r\n");
		return 0;
	}

	OT_CHECK(OTSetSynchronous(s->endpoint));
	OT_CHECK(OTSetBlocking(s->endpoint));
	OT_CHECK(OTUseSyncIdleEvents(s->endpoint, false));

	OT_CHECK(OTBind(s->endpoint, nil, nil));

	OT_CHECK(OTSetNonBlocking(s->endpoint));

	// set up address struct, do the DNS lookup, and connect
	OTMemzero(&sndCall, sizeof(TCall));

	sndCall.addr.buf = (UInt8 *) &hostDNSAddress;
	sndCall.addr.len = OTInitDNSAddress(&hostDNSAddress, (char *) hostname);

	printf_s(session_idx, "Connecting to endpoint \"%s\"... ", hostname); YieldToAnyThread();
	err = OTConnect(s->endpoint, &sndCall, nil);
	if (err != noErr)
	{
		printf_s(session_idx, "OTConnect failed (err=%d)\r\n", (int)err);
		return 0;
	}
	printf_s(session_idx, "done.\r\n"); YieldToAnyThread();

	// init libssh2
	SSH_CHECK(libssh2_init(0));
	YieldToAnyThread();

	s->ssh_session = libssh2_session_init();
	if (s->ssh_session == 0)
	{
		printf_s(session_idx, "Failed to initialize SSH session.\r\n");
		return 0;
	}
	YieldToAnyThread();

	// store session index in libssh2 abstract pointer for use in callbacks
	*libssh2_session_abstract(s->ssh_session) = (void*)(intptr_t)session_idx;

	// register callbacks
	libssh2_session_callback_set(s->ssh_session, LIBSSH2_CALLBACK_SEND, network_send_callback);
	libssh2_session_callback_set(s->ssh_session, LIBSSH2_CALLBACK_RECV, network_recv_callback);
	libssh2_session_callback_set(s->ssh_session, LIBSSH2_CALLBACK_DISCONNECT, ssh_end_msg_callback);

	long st = TickCount();
	printf_s(session_idx, "Beginning SSH session handshake... "); YieldToAnyThread();
	SSH_CHECK(libssh2_session_handshake(s->ssh_session, 0));

	printf_s(session_idx, "done. (%d ticks)\r\n", TickCount() - st); YieldToAnyThread();

	//const char* banner = libssh2_session_banner_get(s->ssh_session);
	//if (banner) printf_s(session_idx, "Server banner: %s\r\n", banner);

	return 1;
}

void* read_thread(void* arg)
{
	int session_idx = (int)(intptr_t)arg;
	struct session* s = &sessions[session_idx];
	int ok = 1;
	int rc = LIBSSH2_ERROR_NONE;

	// yield until we're given a command
	while (s->thread_command == WAIT) YieldToAnyThread();

	if (s->thread_command == EXIT)
	{
		return 0;
	}

	// connect
	printf_s(session_idx, "Connecting to: \"%s\"\r\n", prefs.hostname+1);
	ok = init_connection(session_idx, prefs.hostname+1);

	if (!ok)
	{
		s->thread_state = DONE;
		return 0;
	}

	YieldToAnyThread();

	// check the server pub key vs. known hosts
	if (ok)
	{
		ok = known_hosts(session_idx);
		if (!ok) printf_s(session_idx, "Rejected server public key!\r\n");
	}

	// actually log in
	if (ok)
	{
		printf_s(session_idx, "Authenticating... "); YieldToAnyThread();

		if (prefs.auth_type == USE_PASSWORD)
		{
			rc = libssh2_userauth_password(s->ssh_session, prefs.username+1, prefs.password+1);
		}
		else
		{
			rc = libssh2_userauth_publickey_fromfile_ex(s->ssh_session,
				prefs.username+1,
				prefs.username[0],
				prefs.pubkey_path,
				prefs.privkey_path,
				prefs.password+1);
		}

		if (rc == LIBSSH2_ERROR_NONE)
		{
			printf_s(session_idx, "done.\r\n");
		}
		else
		{
			printf_s(session_idx, "failed!\r\n");
			if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED && prefs.auth_type == USE_PASSWORD) StopAlert(ALRT_PW_FAIL, nil);
			else if (rc == LIBSSH2_ERROR_FILE) StopAlert(ALRT_FILE_FAIL, nil);
			else if (rc == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED)
			{
				printf_s(session_idx, "Username/public key combination invalid!\r\n"); // TODO: have an alert for this
				if (prefs.pubkey_path[0] != '\0' || prefs.privkey_path[0] != '\0')
				{
					prefs.pubkey_path[0] = '\0';
					prefs.privkey_path[0] = '\0';
				}
			}
			else printf_s(session_idx, "unexpected failure: %s\r\n", libssh2_error_string(rc));
			ok = 0;
		}
	}

	save_prefs();

	// if we logged in, open and set up the tty
	if (ok)
	{
		s->channel = libssh2_channel_open_session(s->ssh_session);
		if (s->channel)
		{
			libssh2_channel_handle_extended_data2(s->channel, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE);
			ok = ssh_setup_terminal(session_idx);
		}
		else
		{
			printf_s(session_idx, "Failed to open channel.\r\n");
			ok = 0;
		}
		YieldToAnyThread();
	}

	// if we failed, close everything and exit
	if (!ok || (s->thread_state != OPEN))
	{
		end_connection(session_idx);
		return 0;
	}

	// if we connected, allow pasting
	void* menu = GetMenuHandle(MENU_EDIT);
	EnableItem(menu, 5);

	// read until failure, command to EXIT, or remote EOF
	while (s->thread_command == READ && s->thread_state == OPEN && libssh2_channel_eof(s->channel) == 0)
	{
		if (check_network_events(session_idx)) ssh_read(session_idx);
		YieldToAnyThread();
	}

	if (s->channel && libssh2_channel_eof(s->channel))
	{
		printf_s(session_idx, "(disconnected by server)");
	}

	// if we still have a connection, close it
	if (s->thread_state != DONE) end_connection(session_idx);

	// disallow pasting after connection is closed
	DisableItem(menu, 5);

	// Don't call ssh_disconnect here — close_session or the menu handler
	// will call it from the main thread.  Calling it from the read thread
	// causes a race condition with close_session freeing the vterm.

	return 0;
}
