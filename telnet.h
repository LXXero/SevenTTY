/*
 * SevenTTY - Telnet and raw TCP (nc) support
 */

#pragma once

#include <stddef.h>

void tcp_write_s(int session_idx, char* buf, size_t len);
pascal void tcp_ot_notifier(void* context, OTEventCode event,
                            OTResult result, void* cookie);
int telnet_connect(int session_idx);
void telnet_disconnect(int session_idx);
int nc_inline_connect(int session_idx);
void nc_inline_disconnect(int session_idx);
void telnet_send_naws(int session_idx);
void tcp_output_callback(const char *s, size_t len, void *user);
