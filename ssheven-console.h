/*
 * ssheven
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#pragma once

#include "MacTypes.h"

struct window_context;

void init_font_metrics(void);
void reset_console(struct window_context* wc, int session_idx);

void draw_screen(struct window_context* wc, Rect* r);
void draw_tab_bar(struct window_context* wc);

void printf_i(const char* c, ...);
void printf_s(int session_idx, const char* c, ...);

void check_cursor(struct window_context* wc);

void mouse_click(struct window_context* wc, Point p, int click);
int tab_bar_click(struct window_context* wc, Point p);

void update_console_colors(struct window_context* wc);

size_t get_selection(struct window_context* wc, char** selection);

void clear_selection(struct window_context* wc);

void font_size_change(struct window_context* wc);

void setup_session_vterm(struct window_context* wc, int session_idx);
