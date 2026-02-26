/*
 * SevenTTY (based on ssheven by cy384)
 *
 * Copyright (c) 2021 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#pragma once

void ssh_write(char* buf, size_t len);
void ssh_write_s(int session_idx, char* buf, size_t len);
void* read_thread(void* arg);

struct ssh_auth_params {
	const char* hostname;      /* C string "host:port" for OT DNS */
	const char* host_only;     /* C string "host" for known_hosts check */
	int port;                  /* numeric port for known_hosts keying */
	const char* username;      /* C string */
	const char* password;      /* C string (also key passphrase) */
	const char* pubkey_path;   /* C string, NULL/"" for password auth */
	const char* privkey_path;  /* C string, NULL/"" for password auth */
	int use_key;               /* 1 = pubkey auth, 0 = password auth */
};

int ssh_connect_and_auth(int session_idx, const struct ssh_auth_params* auth);
void ssh_request_pty_resize(int session_idx, int cols, int rows);
void end_connection(int session_idx);
