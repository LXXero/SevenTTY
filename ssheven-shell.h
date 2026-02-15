/*
 * ssheven
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#pragma once

void shell_init(int session_idx);
void shell_input(int session_idx, unsigned char c, int modifiers);
void shell_prompt(int idx);
