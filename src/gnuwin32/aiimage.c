/*
 *  R : A Computer Language for Statistical Data Analysis
 *  file aiimage.c -- pictures for the AI assistant (aichat.c)
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

/*  A picture for the model -- a plot, a screenshot, a photo of an
 *  exercise -- goes to llama-server inside the JSON request as a data
 *  URL.  GDI+, which every Windows has, reads PNG, JPEG, BMP, GIF and
 *  TIFF, scales the picture down, and writes PNG (JPEG for big photos).
 *  This lives apart from aichat.c because the GDI+ headers and GraphApp
 *  would fight over names.
 */

#define WIN32_LEAN_AND_MEAN 1
#define COBJMACROS 1
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "aiimage.h"

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x1
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x2
#endif

static ULONG_PTR g_token = 0;
static int g_state = 0;                  /* 0 not started, 1 up, -1 failed */

static int gdip_up(void)
{
    if (!g_state) {
	GdiplusStartupInput in;
	memset(&in, 0, sizeof in);
	in.GdiplusVersion = 1;
	g_state = GdiplusStartup(&g_token, &in, NULL) == Ok ? 1 : -1;
    }
    return g_state == 1;
}

void aiimg_shutdown(void)
{
    if (g_state == 1) GdiplusShutdown(g_token);
    g_state = 0;
}

/* GDI+'s built-in encoders. */
static const CLSID PNG_ENC =
    { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const CLSID JPEG_ENC =
    { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID QUALITY =
    { 0x1d5be4b5, 0xfa4a, 0x452d, { 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb } };

/* A PNG above this is a photo, not a plot: JPEG makes it a fraction of
   the size, and the model does not see the difference. */
#define PNG_LIMIT (1200 * 1024)

static char *data_url(const char *mime, const unsigned char *p, size_t n)
{
    static const char T[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t head = strlen("data:;base64,") + strlen(mime);
    char *out = (char *) malloc(head + 4 * ((n + 2) / 3) + 1);
    if (!out) return NULL;
    char *o = out + sprintf(out, "data:%s;base64,", mime);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
	unsigned v = (unsigned) p[i] << 16 | (unsigned) p[i + 1] << 8 | p[i + 2];
	*o++ = T[v >> 18]; *o++ = T[(v >> 12) & 63];
	*o++ = T[(v >> 6) & 63]; *o++ = T[v & 63];
    }
    if (i < n) {
	unsigned v = (unsigned) p[i] << 16 | (i + 1 < n ? (unsigned) p[i + 1] << 8 : 0);
	*o++ = T[v >> 18]; *o++ = T[(v >> 12) & 63];
	*o++ = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
	*o++ = '=';
    }
    *o = '\0';
    return out;
}

/* Encode img with the given encoder into memory; malloc'd bytes, or NULL. */
static unsigned char *save(GpImage *img, const CLSID *enc,
			   const EncoderParameters *params, size_t *n)
{
    IStream *st = NULL;
    unsigned char *res = NULL;
    *n = 0;
    if (CreateStreamOnHGlobal(NULL, TRUE, &st) != S_OK) return NULL;
    if (GdipSaveImageToStream(img, st, enc, params) == Ok) {
	STATSTG ss;
	HGLOBAL hg = NULL;
	if (IStream_Stat(st, &ss, STATFLAG_NONAME) == S_OK &&
	    GetHGlobalFromStream(st, &hg) == S_OK) {
	    size_t len = (size_t) ss.cbSize.QuadPart;
	    void *p = GlobalLock(hg);
	    if (p && len && (res = (unsigned char *) malloc(len))) {
		memcpy(res, p, len);
		*n = len;
	    }
	    if (p) GlobalUnlock(hg);
	}
    }
    IStream_Release(st);
    return res;
}

/* Phone photos are stored sideways with an EXIF note saying so. */
static void upright(GpImage *img)
{
    UINT size = 0;
    if (GdipGetPropertyItemSize(img, 0x0112, &size) != Ok || size < sizeof(PropertyItem))
	return;
    PropertyItem *pi = (PropertyItem *) malloc(size);
    if (!pi) return;
    if (GdipGetPropertyItem(img, 0x0112, size, pi) == Ok && pi->value && pi->length >= 2) {
	RotateFlipType r = RotateNoneFlipNone;
	switch (*(unsigned short *) pi->value) {
	case 2: r = RotateNoneFlipX; break;
	case 3: r = Rotate180FlipNone; break;
	case 4: r = Rotate180FlipX; break;
	case 5: r = Rotate90FlipX; break;
	case 6: r = Rotate90FlipNone; break;
	case 7: r = Rotate270FlipX; break;
	case 8: r = Rotate270FlipNone; break;
	}
	if (r != RotateNoneFlipNone) GdipImageRotateFlip(img, r);
    }
    free(pi);
}

/* Scale src (longer side at most maxside) onto white, and encode it. */
static char *encode(GpImage *src, int maxside, int *pw, int *ph)
{
    UINT w = 0, h = 0;
    GdipGetImageWidth(src, &w);
    GdipGetImageHeight(src, &h);
    if (!w || !h) return NULL;
    UINT nw = w, nh = h;
    if (maxside > 0 && (w > (UINT) maxside || h > (UINT) maxside)) {
	if (w >= h) { nw = (UINT) maxside; nh = (UINT) ((double) h * maxside / w + 0.5); }
	else        { nh = (UINT) maxside; nw = (UINT) ((double) w * maxside / h + 0.5); }
	if (!nw) nw = 1;
	if (!nh) nh = 1;
    }
    GpBitmap *dst = NULL;
    if (GdipCreateBitmapFromScan0((INT) nw, (INT) nh, 0, PixelFormat24bppRGB, NULL, &dst) != Ok)
	return NULL;
    GpGraphics *g = NULL;
    if (GdipGetImageGraphicsContext((GpImage *) dst, &g) == Ok) {
	/* White behind transparent pixels, as the picture would look on a page. */
	GdipGraphicsClear(g, 0xFFFFFFFF);
	GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
	GdipSetPixelOffsetMode(g, PixelOffsetModeHighQuality);
	GdipDrawImageRectI(g, src, 0, 0, (INT) nw, (INT) nh);
	GdipDeleteGraphics(g);
    }
    char *url = NULL;
    size_t n = 0;
    unsigned char *bytes = save((GpImage *) dst, &PNG_ENC, NULL, &n);
    if (bytes && n > PNG_LIMIT) {
	EncoderParameters ep;
	ULONG q = 88;
	ep.Count = 1;
	ep.Parameter[0].Guid = QUALITY;
	ep.Parameter[0].Type = EncoderParameterValueTypeLong;
	ep.Parameter[0].NumberOfValues = 1;
	ep.Parameter[0].Value = &q;
	size_t jn = 0;
	unsigned char *jpg = save((GpImage *) dst, &JPEG_ENC, &ep, &jn);
	if (jpg && jn < n) url = data_url("image/jpeg", jpg, jn);
	free(jpg);
    }
    if (!url && bytes) url = data_url("image/png", bytes, n);
    free(bytes);
    GdipDisposeImage((GpImage *) dst);
    if (url) {
	if (pw) *pw = (int) nw;
	if (ph) *ph = (int) nh;
    }
    return url;
}

char *aiimg_from_file(const wchar_t *path, int maxside, int *w, int *h)
{
    if (!path || !gdip_up()) return NULL;
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromFile(path, &bmp) != Ok || !bmp) return NULL;
    upright((GpImage *) bmp);
    char *url = encode((GpImage *) bmp, maxside, w, h);
    GdipDisposeImage((GpImage *) bmp);
    return url;
}

char *aiimg_from_hbitmap(HBITMAP hb, int maxside, int *w, int *h)
{
    if (!hb || !gdip_up()) return NULL;
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromHBITMAP(hb, NULL, &bmp) != Ok || !bmp) return NULL;
    char *url = encode((GpImage *) bmp, maxside, w, h);
    GdipDisposeImage((GpImage *) bmp);
    return url;
}

char *aiimg_from_pixels(const unsigned char *bgrx, int pw, int ph, int stride,
			int maxside, int *w, int *h)
{
    if (!bgrx || pw <= 0 || ph <= 0 || !gdip_up()) return NULL;
    GpBitmap *bmp = NULL;
    /* GDI+ only reads the pixels; it does not take them over. */
    if (GdipCreateBitmapFromScan0(pw, ph, stride, PixelFormat32bppRGB,
				  (BYTE *) bgrx, &bmp) != Ok || !bmp)
	return NULL;
    char *url = encode((GpImage *) bmp, maxside, w, h);
    GdipDisposeImage((GpImage *) bmp);
    return url;
}

char *aiimg_from_window(HWND hwnd, int maxside, int *w, int *h)
{
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
	return NULL;
    HDC wdc = GetDC(hwnd);
    if (!wdc) return NULL;
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP hb = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    char *url = NULL;
    if (mdc && hb) {
	HGDIOBJ old = SelectObject(mdc, hb);
	/* PrintWindow draws it even when other windows cover it. */
	if (!PrintWindow(hwnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
	    BitBlt(mdc, 0, 0, rc.right, rc.bottom, wdc, 0, 0, SRCCOPY);
	SelectObject(mdc, old);
	url = aiimg_from_hbitmap(hb, maxside, w, h);
    }
    if (hb) DeleteObject(hb);
    if (mdc) DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return url;
}

static int open_clipboard(void)
{
    for (int i = 0; i < 10; i++) {
	if (OpenClipboard(NULL)) return 1;
	Sleep(30);
    }
    return 0;
}

int aiimg_clipboard_has_picture(void)
{
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) return 0;
    return IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB) ||
	IsClipboardFormatAvailable(CF_HDROP);
}

wchar_t *aiimg_clipboard_files(void)
{
    if (!IsClipboardFormatAvailable(CF_HDROP) || !open_clipboard()) return NULL;
    wchar_t *list = NULL;
    HDROP hd = (HDROP) GetClipboardData(CF_HDROP);
    UINT n = hd ? DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0) : 0;
    size_t total = 1;
    for (UINT i = 0; i < n; i++) total += DragQueryFileW(hd, i, NULL, 0) + 1;
    if (n && (list = (wchar_t *) calloc(total, sizeof(wchar_t)))) {
	wchar_t *p = list;
	for (UINT i = 0; i < n; i++) {
	    UINT len = DragQueryFileW(hd, i, NULL, 0);
	    DragQueryFileW(hd, i, p, len + 1);
	    p += len + 1;
	}
    }
    CloseClipboard();
    return list;
}

char *aiimg_from_clipboard(int maxside, int *w, int *h)
{
    char *url = NULL;
    if (!open_clipboard()) return NULL;
    if (IsClipboardFormatAvailable(CF_BITMAP)) {
	/* Owned by the clipboard: not deleted here. */
	HBITMAP hb = (HBITMAP) GetClipboardData(CF_BITMAP);
	if (hb) url = aiimg_from_hbitmap(hb, maxside, w, h);
    }
    CloseClipboard();
    return url;
}
