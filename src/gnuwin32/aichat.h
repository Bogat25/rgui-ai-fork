/*
 *  R : A Computer Language for Statistical Data Analysis
 *  file aichat.h -- local AI assistant panel for RGui
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  https://www.R-project.org/Licenses/
 */

#ifndef R_GNUWIN32_AICHAT_H
#define R_GNUWIN32_AICHAT_H

/* Show the assistant panel if hidden, hide it if visible.  The panel and
   the conversation survive hiding, so reopening restores the transcript.
   Safe to call even when the assistant is disabled or misconfigured: in
   that case it reports the problem and returns. */
void aichat_toggle(void);

/* Stop any generation in progress and terminate the helper process.
   Called from the RGui shutdown path.  Safe to call when nothing started. */
void aichat_shutdown(void);

/* Non-zero if the assistant shortcut is enabled in etc/Rai.conf. */
int aichat_enabled(void);

/* Shortcut letter, used with Ctrl, from etc/Rai.conf (default 'T').
   Returns 0 if no shortcut was requested. */
int aichat_hotkey(void);

#endif /* R_GNUWIN32_AICHAT_H */
