/*
 *  R : A Computer Language for Statistical Data Analysis
 *  file aiplot.c -- the current plot, for the AI assistant (aichat.c)
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

/*  An on-screen windows() device draws every plot into an off-screen
 *  bitmap and only copies it to its window.  Reading that bitmap, the way
 *  savePlot() does, gives the plot exactly as R drew it, whether or not
 *  other windows (the assistant's own panel, say) cover it -- a capture
 *  of the screen inside RGui's MDI frame would get whatever is on top.
 *  Only accessors are used: no evaluation, no allocation of R objects.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/GraphicsEngine.h>

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <stdlib.h>
#include "graphapp/ga.h"
#include "../library/grDevices/src/devWindows.h"
#include "aiimage.h"

/* R_MaxDevices, the size of R's device table; not in the public headers. */
#define AI_MAX_DEVICES 64

unsigned char *aiplot_pixels(HWND hwnd, int devnum, int *w, int *h, int *stride)
{
    /* The window title says "Device N"; R counts devices from 0. */
    if (!hwnd || devnum < 2 || devnum > AI_MAX_DEVICES) return NULL;
    pGEDevDesc gd = GEgetDevice(devnum - 1);
    if (!gd || !gd->dev) return NULL;
    gadesc *xd = (gadesc *) gd->dev->deviceSpecific;
    if (!xd || xd->kind != SCREEN || !xd->gawin || !xd->bm ||
	(HWND) getHandle(xd->gawin) != hwnd)
	return NULL;
    rect saved = ggetcliprect(xd->bm), full = getrect(xd->bm);
    gsetcliprect(xd->bm, full);
    unsigned char *data = NULL;
    getbitmapdata2(xd->bm, &data);       /* 32-bit BGRX, top row first */
    gsetcliprect(xd->bm, saved);
    if (!data) return NULL;
    *w = xd->windowWidth < full.width ? xd->windowWidth : full.width;
    *h = xd->windowHeight < full.height ? xd->windowHeight : full.height;
    *stride = 4 * full.width;
    if (*w <= 0 || *h <= 0) { free(data); return NULL; }
    return data;
}
