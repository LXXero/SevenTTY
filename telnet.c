/*
 * SevenTTY - Telnet and raw TCP (nc) support
 *
 * Shared TCP helpers, telnet IAC negotiation, threaded read loops.
 */

#include "app.h"
#include "telnet.h"
#include "console.h"
#include "debug.h"

#include <stdio.h>
#include <string.h>
#include <Threads.h>

/* ------------------------------------------------------------------ */
/* telnet protocol constants                                          */
/* ------------------------------------------------------------------ */

#define TEL_SE    240
#define TEL_NOP   241
#define TEL_BRK   243
#define TEL_SB    250
#define TEL_WILL  251
#define TEL_WONT  252
#define TEL_DO    253
#define TEL_DONT  254
#define TEL_IAC   255

#define TELOPT_ECHO    1
#define TELOPT_SGA     3
#define TELOPT_TTYPE  24
#define TELOPT_NAWS   31

/* IAC parser states */
#define TS_DATA    0
#define TS_IAC     1
#define TS_WILL    2
#define TS_WONT    3
#define TS_DO      4
#define TS_DONT    5
#define TS_SB      6
#define TS_SB_IAC  7

/* ------------------------------------------------------------------ */
/* raw TCP write                                                      */
/* ------------------------------------------------------------------ */

void tcp_write_s(int session_idx, char* buf, size_t len)
{
	struct session* s = &sessions[session_idx];

	if (s->thread_state == OPEN && s->thread_command != EXIT)
	{
		OTResult r = OTSnd(s->endpoint, buf, len, 0);

		if (r == kOTFlowErr)
			return;  /* flow control, not fatal */

		if (r == kOTLookErr)
		{
			/* pending event — let the read thread handle it */
			return;
		}

		if (r < 0)
		{
			printf_s(session_idx, "\r\nTCP send error %d, closing.\r\n", (int)r);
			s->thread_command = EXIT;
		}
	}
}

/* vterm output callback for telnet/nc sessions */
void tcp_output_callback(const char *s, size_t len, void *user)
{
	int idx = (int)(intptr_t)user;
	tcp_write_s(idx, (char*)s, len);
}

/* ------------------------------------------------------------------ */
/* TCP connection setup/teardown (no libssh2)                         */
/* ------------------------------------------------------------------ */

/* connect timeout: 30 seconds (1800 ticks).  set before OTConnect,
   cleared after.  the notifier cancels the blocking call if exceeded. */
#define TCP_CONNECT_TIMEOUT  1800
static unsigned long tcp_connect_deadline = 0;
static EndpointRef tcp_connect_ep = kOTInvalidEndpointRef;
static int tcp_connect_session_idx = -1;

/* OT notifier: yields to cooperative threads during blocking calls.
   Called at system task time with kOTSyncIdleEvent when
   OTUseSyncIdleEvents is true, keeping the machine responsive.
   Also cancels on timeout or when disconnect sets thread_command=EXIT. */
pascal void tcp_ot_notifier(void* context, OTEventCode event,
                                   OTResult result, void* cookie)
{
	(void)context;
	(void)result;
	(void)cookie;
	if (event == kOTSyncIdleEvent)
	{
		YieldToAnyThread();
		if (tcp_connect_ep != kOTInvalidEndpointRef)
		{
			int cancel = 0;

			/* timeout expired */
			if (tcp_connect_deadline > 0 &&
			    TickCount() > tcp_connect_deadline)
				cancel = 1;

			/* disconnect requested */
			if (tcp_connect_session_idx >= 0 &&
			    sessions[tcp_connect_session_idx].thread_command == EXIT)
				cancel = 1;

			if (cancel)
				OTCancelSynchronousCalls(tcp_connect_ep, kOTCanceledErr);
		}
	}
}

static int tcp_init_connection(int session_idx, char* hostname)
{
	struct session* s = &sessions[session_idx];
	OSStatus err = noErr;
	TCall sndCall;
	DNSAddress hostDNSAddress;

	s->endpoint = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, nil, &err);
	if (err != noErr || s->endpoint == kOTInvalidEndpointRef)
	{
		printf_s(session_idx, "Failed to open TCP endpoint.\r\n");
		return 0;
	}

	OT_CHECK(OTSetSynchronous(s->endpoint));
	OT_CHECK(OTSetBlocking(s->endpoint));
	OT_CHECK(OTInstallNotifier(s->endpoint, tcp_ot_notifier, nil));
	OT_CHECK(OTUseSyncIdleEvents(s->endpoint, true));
	OT_CHECK(OTBind(s->endpoint, nil, nil));

	OTMemzero(&sndCall, sizeof(TCall));
	sndCall.addr.buf = (UInt8 *) &hostDNSAddress;
	sndCall.addr.len = OTInitDNSAddress(&hostDNSAddress, hostname);

	printf_s(session_idx, "Connecting to \"%s\"... ", hostname);
	YieldToAnyThread();

	/* OTConnect blocks during DNS + TCP handshake.
	   The idle notifier yields to other threads during the wait.
	   A 30-second timeout prevents hanging on unreachable hosts. */
	tcp_connect_ep = s->endpoint;
	tcp_connect_session_idx = session_idx;
	tcp_connect_deadline = TickCount() + TCP_CONNECT_TIMEOUT;

	err = OTConnect(s->endpoint, &sndCall, nil);

	tcp_connect_deadline = 0;
	tcp_connect_ep = kOTInvalidEndpointRef;
	tcp_connect_session_idx = -1;

	if (err == kOTCanceledErr)
	{
		if (s->thread_command == EXIT)
			printf_s(session_idx, "cancelled.\r\n");
		else
			printf_s(session_idx, "timed out.\r\n");
		OTUnbind(s->endpoint);
		OTCloseProvider(s->endpoint);
		s->endpoint = kOTInvalidEndpointRef;
		return 0;
	}

	if (err == kOTLookErr)
	{
		/* event arrived during connect (e.g. connection refused) */
		OTResult ev = OTLook(s->endpoint);
		if (ev == T_DISCONNECT)
		{
			OTRcvDisconnect(s->endpoint, nil);
			printf_s(session_idx, "connection refused.\r\n");
		}
		else
		{
			printf_s(session_idx, "failed (event=%d)\r\n", (int)ev);
		}
		OTUnbind(s->endpoint);
		OTCloseProvider(s->endpoint);
		s->endpoint = kOTInvalidEndpointRef;
		return 0;
	}

	if (err != noErr)
	{
		printf_s(session_idx, "failed (err=%d)\r\n", (int)err);
		OTUnbind(s->endpoint);
		OTCloseProvider(s->endpoint);
		s->endpoint = kOTInvalidEndpointRef;
		return 0;
	}

	printf_s(session_idx, "done.\r\n");
	YieldToAnyThread();

	/* switch to non-blocking for reads — disable idle events so OTRcv
	   returns kOTNoDataErr instead of blocking with idle callbacks */
	OTUseSyncIdleEvents(s->endpoint, false);
	OTSetNonBlocking(s->endpoint);

	return 1;
}

static void tcp_end_connection(int session_idx)
{
	struct session* s = &sessions[session_idx];
	s->thread_state = CLEANUP;

	if (s->endpoint != kOTInvalidEndpointRef)
	{
		OTSndOrderlyDisconnect(s->endpoint);

		/* drain remaining data */
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

		/* finish closing */
		{
			OSStatus result = OTLook(s->endpoint);
			OSStatus err;
			switch (result)
			{
				case T_DISCONNECT:
					OTRcvDisconnect(s->endpoint, nil);
					break;
				case T_ORDREL:
					err = OTRcvOrderlyDisconnect(s->endpoint);
					if (err == noErr)
						OTSndOrderlyDisconnect(s->endpoint);
					break;
				default:
					break;
			}
		}

		OTUnbind(s->endpoint);
		OTCloseProvider(s->endpoint);
		s->endpoint = kOTInvalidEndpointRef;
	}

	s->thread_state = DONE;
}

static int tcp_check_events(int session_idx)
{
	struct session* s = &sessions[session_idx];
	int ok = 1;
	OTResult look_result = OTLook(s->endpoint);

	switch (look_result)
	{
		case T_DATA:
		case T_EXDATA:
			break;

		case T_RESET:
			printf_s(session_idx, "\r\nConnection reset.\r\n");
			tcp_end_connection(session_idx);
			ok = 0;
			break;

		case T_DISCONNECT:
			OTRcvDisconnect(s->endpoint, nil);
			printf_s(session_idx, "\r\nConnection reset by peer.\r\n");
			tcp_end_connection(session_idx);
			ok = 0;
			break;

		case T_ORDREL:
		{
			OSStatus err = OTRcvOrderlyDisconnect(s->endpoint);
			if (err == noErr)
				OTSndOrderlyDisconnect(s->endpoint);
			printf_s(session_idx, "\r\nConnection closed by peer.\r\n");
			tcp_end_connection(session_idx);
			ok = 0;
			break;
		}

		default:
			break;
	}

	return ok;
}

/* ------------------------------------------------------------------ */
/* telnet IAC negotiation                                             */
/* ------------------------------------------------------------------ */

static void telnet_send_cmd(int session_idx, unsigned char cmd, unsigned char opt)
{
	struct session* s = &sessions[session_idx];
	unsigned char buf[3];
	buf[0] = TEL_IAC;
	buf[1] = cmd;
	buf[2] = opt;
	if (s->endpoint != kOTInvalidEndpointRef)
		OTSnd(s->endpoint, buf, 3, 0);
}

void telnet_send_naws(int session_idx)
{
	struct session* s = &sessions[session_idx];
	struct window_context* wc = window_for_session(session_idx);
	unsigned char buf[9];
	int cols, rows;

	if (s->thread_state != OPEN) return;
	if (wc == NULL) return;

	cols = wc->size_x;
	rows = wc->size_y;

	buf[0] = TEL_IAC;
	buf[1] = TEL_SB;
	buf[2] = TELOPT_NAWS;
	buf[3] = (cols >> 8) & 0xFF;
	buf[4] = cols & 0xFF;
	buf[5] = (rows >> 8) & 0xFF;
	buf[6] = rows & 0xFF;
	buf[7] = TEL_IAC;
	buf[8] = TEL_SE;

	if (s->endpoint != kOTInvalidEndpointRef)
		OTSnd(s->endpoint, buf, 9, 0);
}

static void telnet_handle_will(int session_idx, unsigned char opt)
{
	switch (opt)
	{
		case TELOPT_ECHO:
		case TELOPT_SGA:
			telnet_send_cmd(session_idx, TEL_DO, opt);
			break;
		default:
			telnet_send_cmd(session_idx, TEL_DONT, opt);
			break;
	}
}

static void telnet_handle_do(int session_idx, unsigned char opt)
{
	switch (opt)
	{
		case TELOPT_TTYPE:
		case TELOPT_SGA:
			telnet_send_cmd(session_idx, TEL_WILL, opt);
			break;
		case TELOPT_NAWS:
			telnet_send_cmd(session_idx, TEL_WILL, opt);
			telnet_send_naws(session_idx);
			break;
		default:
			telnet_send_cmd(session_idx, TEL_WONT, opt);
			break;
	}
}

static void telnet_handle_sb(int session_idx, unsigned char* buf, int len)
{
	/* terminal type subnegotiation: option=TTYPE, qualifier=SEND(1) */
	if (len >= 2 && buf[0] == TELOPT_TTYPE && buf[1] == 1)
	{
		struct session* s = &sessions[session_idx];
		const char* ttype = prefs.terminal_string;
		int tlen = strlen(ttype);
		unsigned char resp[64];
		int ri = 0;

		resp[ri++] = TEL_IAC;
		resp[ri++] = TEL_SB;
		resp[ri++] = TELOPT_TTYPE;
		resp[ri++] = 0; /* IS */
		if (tlen > 56) tlen = 56;
		memcpy(resp + ri, ttype, tlen);
		ri += tlen;
		resp[ri++] = TEL_IAC;
		resp[ri++] = TEL_SE;

		if (s->endpoint != kOTInvalidEndpointRef)
			OTSnd(s->endpoint, resp, ri, 0);
	}
}

/* process received data through telnet IAC state machine.
 * strips IAC sequences, converts bare LF to CRLF,
 * returns clean data length in out buffer. */
static int telnet_process(int session_idx, const unsigned char* in, int in_len,
                          unsigned char* out)
{
	struct session* s = &sessions[session_idx];
	int oi = 0;
	int i;
	int prev_cr = 0;

	for (i = 0; i < in_len; i++)
	{
		unsigned char c = in[i];

		switch (s->telnet_state)
		{
			case TS_DATA:
				if (c == TEL_IAC)
					s->telnet_state = TS_IAC;
				else if (c == '\n' && !prev_cr)
				{
					out[oi++] = '\r';
					out[oi++] = '\n';
				}
				else
					out[oi++] = c;
				prev_cr = (c == '\r');
				break;

			case TS_IAC:
				switch (c)
				{
					case TEL_IAC:
						out[oi++] = 0xFF;
						s->telnet_state = TS_DATA;
						break;
					case TEL_WILL:
						s->telnet_state = TS_WILL;
						break;
					case TEL_WONT:
						s->telnet_state = TS_WONT;
						break;
					case TEL_DO:
						s->telnet_state = TS_DO;
						break;
					case TEL_DONT:
						s->telnet_state = TS_DONT;
						break;
					case TEL_SB:
						s->telnet_state = TS_SB;
						s->telnet_sb_len = 0;
						break;
					default:
						/* NOP, BRK, etc. — ignore */
						s->telnet_state = TS_DATA;
						break;
				}
				break;

			case TS_WILL:
				telnet_handle_will(session_idx, c);
				s->telnet_state = TS_DATA;
				break;

			case TS_WONT:
				/* acknowledge, nothing to do */
				s->telnet_state = TS_DATA;
				break;

			case TS_DO:
				telnet_handle_do(session_idx, c);
				s->telnet_state = TS_DATA;
				break;

			case TS_DONT:
				s->telnet_state = TS_DATA;
				break;

			case TS_SB:
				if (c == TEL_IAC)
					s->telnet_state = TS_SB_IAC;
				else if (s->telnet_sb_len < (int)sizeof(s->telnet_sb_buf))
					s->telnet_sb_buf[s->telnet_sb_len++] = c;
				break;

			case TS_SB_IAC:
				if (c == TEL_SE)
				{
					telnet_handle_sb(session_idx, s->telnet_sb_buf,
					                 s->telnet_sb_len);
					s->telnet_state = TS_DATA;
				}
				else
				{
					/* not SE, put byte into SB buffer */
					if (s->telnet_sb_len < (int)sizeof(s->telnet_sb_buf))
						s->telnet_sb_buf[s->telnet_sb_len++] = c;
					s->telnet_state = TS_SB;
				}
				break;
		}
	}

	return oi;
}

/* ------------------------------------------------------------------ */
/* read functions                                                     */
/* ------------------------------------------------------------------ */

static void telnet_read(int session_idx)
{
	struct session* s = &sessions[session_idx];
	OTFlags ot_flags = 0;
	OTResult rc;
	unsigned char clean[SSH_BUFFER_SIZE];
	int clean_len;

	/* read half-buffer so CRLF expansion fits in clean[] */
	rc = OTRcv(s->endpoint, s->recv_buffer, SSH_BUFFER_SIZE / 2, &ot_flags);

	if (rc == kOTNoDataErr) return;

	if (rc == kOTLookErr)
	{
		tcp_check_events(session_idx);
		return;
	}

	if (rc <= 0)
	{
		if (s->thread_command != EXIT)
		{
			printf_s(session_idx, "\r\nConnection closed (rc=%d).\r\n", (int)rc);
			s->thread_command = EXIT;
		}
		return;
	}

	clean_len = telnet_process(session_idx, (unsigned char*)s->recv_buffer,
	                           (int)rc, clean);

	if (clean_len > 0)
		vterm_input_write(s->vterm, (char*)clean, clean_len);
}

/* convert bare LF to CRLF for vterm display */
static int lf_to_crlf(const char* in, int in_len, char* out)
{
	int oi = 0;
	int i;
	int prev_cr = 0;

	for (i = 0; i < in_len; i++)
	{
		if (in[i] == '\n' && !prev_cr)
			out[oi++] = '\r';
		out[oi++] = in[i];
		prev_cr = (in[i] == '\r');
	}

	return oi;
}

static void nc_raw_read(int session_idx)
{
	struct session* s = &sessions[session_idx];
	OTFlags ot_flags = 0;
	OTResult rc;
	char clean[SSH_BUFFER_SIZE];
	int clean_len;

	/* read half-buffer so CRLF expansion fits in clean[] */
	rc = OTRcv(s->endpoint, s->recv_buffer, SSH_BUFFER_SIZE / 2, &ot_flags);

	if (rc == kOTNoDataErr) return;

	if (rc == kOTLookErr)
	{
		tcp_check_events(session_idx);
		return;
	}

	if (rc <= 0)
	{
		if (s->thread_command != EXIT)
		{
			printf_s(session_idx, "\r\nConnection closed (rc=%d).\r\n", (int)rc);
			s->thread_command = EXIT;
		}
		return;
	}

	clean_len = lf_to_crlf(s->recv_buffer, (int)rc, clean);
	vterm_input_write(s->vterm, clean, clean_len);
}

/* ------------------------------------------------------------------ */
/* thread functions                                                   */
/* ------------------------------------------------------------------ */

/* free buffers owned by this thread (used when disconnect timed out) */
static void tcp_free_buffers(struct session* s)
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

static void* telnet_read_thread(void* arg)
{
	int session_idx = (int)(intptr_t)arg;
	struct session* s = &sessions[session_idx];
	char hostport[280];

	while (s->thread_command == WAIT) YieldToAnyThread();
	if (s->thread_command == EXIT) { s->thread_state = DONE; return 0; }

	/* mark thread active so disconnect can cancel OTConnect */
	s->thread_state = CLEANUP;

	snprintf(hostport, sizeof(hostport), "%s:%d", s->telnet_host,
	         (int)s->telnet_port);

	if (!tcp_init_connection(session_idx, hostport))
	{
		/* if disconnect timed out and gave up, we own the buffers */
		if (s->thread_command == EXIT)
			tcp_free_buffers(s);
		s->thread_state = DONE;
		return 0;
	}

	/* disconnect requested while we were connecting? */
	if (s->thread_command == EXIT)
	{
		tcp_end_connection(session_idx);
		tcp_free_buffers(s);
		s->thread_state = DONE;
		return 0;
	}

	s->thread_state = OPEN;

	/* enable paste */
	{
		void* menu = GetMenuHandle(MENU_EDIT);
		EnableItem(menu, 5);
	}

	while (s->thread_command == READ && s->thread_state == OPEN)
	{
		telnet_read(session_idx);
		YieldToAnyThread();
	}

	if (s->thread_state != DONE)
		tcp_end_connection(session_idx);

	/* if disconnect gave up waiting for us, we own the buffers */
	if (s->thread_command == EXIT)
		tcp_free_buffers(s);

	{
		void* menu = GetMenuHandle(MENU_EDIT);
		DisableItem(menu, 5);
	}

	s->thread_state = DONE;
	return 0;
}

static void* nc_read_thread(void* arg)
{
	int session_idx = (int)(intptr_t)arg;
	struct session* s = &sessions[session_idx];
	char hostport[280];

	while (s->thread_command == WAIT) YieldToAnyThread();
	if (s->thread_command == EXIT) { s->thread_state = DONE; return 0; }

	/* mark thread active so disconnect can cancel OTConnect */
	s->thread_state = CLEANUP;

	snprintf(hostport, sizeof(hostport), "%s:%d", s->telnet_host,
	         (int)s->telnet_port);

	if (!tcp_init_connection(session_idx, hostport))
	{
		if (s->thread_command == EXIT)
			tcp_free_buffers(s);
		s->thread_state = DONE;
		return 0;
	}

	if (s->thread_command == EXIT)
	{
		tcp_end_connection(session_idx);
		tcp_free_buffers(s);
		s->thread_state = DONE;
		return 0;
	}

	s->thread_state = OPEN;

	while (s->thread_command == READ && s->thread_state == OPEN)
	{
		nc_raw_read(session_idx);
		YieldToAnyThread();
	}

	if (s->thread_state != DONE)
		tcp_end_connection(session_idx);

	/* if disconnect gave up waiting for us, we own the buffers */
	if (s->thread_command == EXIT)
		tcp_free_buffers(s);

	s->thread_state = DONE;
	return 0;
}

/* ------------------------------------------------------------------ */
/* connect / disconnect entry points                                  */
/* ------------------------------------------------------------------ */

int telnet_connect(int session_idx)
{
	struct session* s = &sessions[session_idx];
	struct window_context* wc = window_for_session(session_idx);
	OSStatus err = noErr;
	ThreadID tid = 0;

	if (InitOpenTransport() != noErr)
	{
		printf_s(session_idx, "Failed to initialize Open Transport.\r\n");
		return 0;
	}

	s->recv_buffer = OTAllocMem(SSH_BUFFER_SIZE);
	s->send_buffer = OTAllocMem(SSH_BUFFER_SIZE);
	if (s->recv_buffer == NULL || s->send_buffer == NULL)
	{
		printf_s(session_idx, "Failed to allocate buffers.\r\n");
		return 0;
	}

	s->telnet_state = TS_DATA;
	s->telnet_sb_len = 0;
	s->thread_command = WAIT;

	err = NewThread(kCooperativeThread, telnet_read_thread,
	                (void*)(intptr_t)session_idx, 100000,
	                kCreateIfNeeded, NULL, &tid);
	if (err != noErr)
	{
		printf_s(session_idx, "Failed to create read thread.\r\n");
		return 0;
	}

	s->thread_command = READ;
	s->type = SESSION_TELNET;
	{
		char lbl[64];
		snprintf(lbl, sizeof(lbl), "telnet %s", s->telnet_host);
		memcpy(s->tab_label, lbl, sizeof(s->tab_label));
	}

	if (wc != NULL && wc->num_sessions > 1)
	{
		SetPort(wc->win);
		draw_tab_bar(wc);
	}

	{
		void* menu = GetMenuHandle(MENU_FILE);
		DisableItem(menu, FMENU_CONNECT);
		EnableItem(menu, FMENU_DISCONNECT);
	}

	return 1;
}

void telnet_disconnect(int session_idx)
{
	struct session* s = &sessions[session_idx];
	struct window_context* wc = window_for_session(session_idx);

	s->thread_command = EXIT;

	if (s->thread_state != UNINITIALIZED && s->thread_state != DONE)
	{
		/* force the connect timeout notifier to cancel immediately */
		if (tcp_connect_ep == s->endpoint)
			tcp_connect_deadline = 1;

		if (s->endpoint != kOTInvalidEndpointRef)
			OTCancelSynchronousCalls(s->endpoint, kOTCanceledErr);

		/* give thread a few chances to notice EXIT and clean up,
		   but don't block for seconds waiting */
		{
			int tries;
			for (tries = 0; tries < 5 && s->thread_state != DONE; tries++)
				YieldToAnyThread();
		}
	}

	/* only free buffers if thread is done; otherwise thread owns them */
	if (s->thread_state == DONE || s->thread_state == UNINITIALIZED)
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

	snprintf(s->tab_label, sizeof(s->tab_label), "disconnected");

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

/* ------------------------------------------------------------------ */
/* nc (inline) — runs in the current local shell session              */
/* ------------------------------------------------------------------ */

int nc_inline_connect(int session_idx)
{
	struct session* s = &sessions[session_idx];
	OSStatus err = noErr;
	ThreadID tid = 0;

	if (InitOpenTransport() != noErr)
	{
		printf_s(session_idx, "Failed to initialize Open Transport.\r\n");
		return 0;
	}

	s->recv_buffer = OTAllocMem(SSH_BUFFER_SIZE);
	s->send_buffer = OTAllocMem(SSH_BUFFER_SIZE);
	if (s->recv_buffer == NULL || s->send_buffer == NULL)
	{
		printf_s(session_idx, "Failed to allocate buffers.\r\n");
		return 0;
	}

	s->thread_command = WAIT;

	err = NewThread(kCooperativeThread, nc_read_thread,
	                (void*)(intptr_t)session_idx, 100000,
	                kCreateIfNeeded, NULL, &tid);
	if (err != noErr)
	{
		printf_s(session_idx, "Failed to create read thread.\r\n");
		return 0;
	}

	s->thread_command = READ;
	return 1;
}

void nc_inline_disconnect(int session_idx)
{
	struct session* s = &sessions[session_idx];

	s->thread_command = EXIT;

	if (s->thread_state != UNINITIALIZED && s->thread_state != DONE)
	{
		/* force the connect timeout notifier to cancel immediately */
		if (tcp_connect_ep == s->endpoint)
			tcp_connect_deadline = 1;

		if (s->endpoint != kOTInvalidEndpointRef)
			OTCancelSynchronousCalls(s->endpoint, kOTCanceledErr);

		/* give thread a few chances to notice EXIT and clean up,
		   but don't block for seconds waiting */
		{
			int tries;
			for (tries = 0; tries < 5 && s->thread_state != DONE; tries++)
				YieldToAnyThread();
		}
	}

	/* only free buffers if thread is done; otherwise thread owns them.
	   If thread is still running, DON'T reset state — let the thread
	   finish and set DONE, then shell_input's DONE check will clean up. */
	if (s->thread_state == DONE || s->thread_state == UNINITIALIZED)
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
		s->thread_state = UNINITIALIZED;
		s->thread_command = WAIT;
	}
}
