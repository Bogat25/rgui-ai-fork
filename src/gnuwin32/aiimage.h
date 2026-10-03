/*
 *  R : A Computer Language for Statistical Data Analysis
 *  file aiimage.h -- pictures for the AI assistant (aichat.c)
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

#ifndef RGUI_AIIMAGE_H
#define RGUI_AIIMAGE_H

/* Each returns a malloc'd data URL ("data:image/png;base64,...") of the
   picture scaled so that its longer side is at most maxside pixels, and
   its size after scaling in *w and *h; or NULL if there is no picture
   or it cannot be read.  Main (GUI) thread only. */
char *aiimg_from_file(const wchar_t *path, int maxside, int *w, int *h);
char *aiimg_from_hbitmap(HBITMAP hb, int maxside, int *w, int *h);
/* 32-bit BGRX pixels, rows top first, stride bytes apart. */
char *aiimg_from_pixels(const unsigned char *bgrx, int pw, int ph, int stride,
			int maxside, int *w, int *h);
/* aiplot.c: the plot of windows() device devnum ("Device N" in the
   title of window hwnd) as savePlot() reads it; malloc'd, or NULL when
   that is not an on-screen windows() device. */
unsigned char *aiplot_pixels(HWND hwnd, int devnum, int *w, int *h, int *stride);
/* The client area of a window, as it is on screen. */
char *aiimg_from_window(HWND hwnd, int maxside, int *w, int *h);
/* A bitmap on the clipboard (a screenshot, a copied picture). */
char *aiimg_from_clipboard(int maxside, int *w, int *h);

/* Files copied in Explorer: a malloc'd list of paths, each ending in a
   NUL, the list in a second NUL; NULL if there are none. */
wchar_t *aiimg_clipboard_files(void);

/* Whether the clipboard holds a picture, or picture files copied in
   Explorer, and no text (text is pasted as text). */
int aiimg_clipboard_has_picture(void);

void aiimg_shutdown(void);

#endif
