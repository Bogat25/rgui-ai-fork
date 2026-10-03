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

/* A copy of src scaled so that it fits in maxw x maxh (never enlarged),
   on white, in memory GDI+ owns.  Size in *pw, *ph. */
static GpBitmap *scaled(GpImage *src, int maxw, int maxh, UINT *pw, UINT *ph)
{
    UINT w = 0, h = 0;
    GdipGetImageWidth(src, &w);
    GdipGetImageHeight(src, &h);
    if (!w || !h) return NULL;
    double f = 1.0;
    if (maxw > 0 && w > (UINT) maxw) f = (double) maxw / w;
    if (maxh > 0 && h * f > maxh) f = (double) maxh / h;
    UINT nw = (UINT) (w * f + 0.5), nh = (UINT) (h * f + 0.5);
    if (!nw) nw = 1;
    if (!nh) nh = 1;
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
    *pw = nw;
    *ph = nh;
    return dst;
}

/* Scale src (longer side at most maxside) and encode it. */
static char *encode(GpImage *src, int maxside, int *pw, int *ph)
{
    UINT nw = 0, nh = 0;
    GpBitmap *dst = scaled(src, maxside, maxside, &nw, &nh);
    if (!dst) return NULL;
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

/* ------------------------------------------------------------------ */
/* showing pictures: decoding, the viewer, the strip of attachments    */
/* ------------------------------------------------------------------ */

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* The bytes inside a data URL, malloc'd. */
static unsigned char *url_bytes(const char *url, size_t *n)
{
    *n = 0;
    const char *b = url ? strchr(url, ',') : NULL;
    if (!b) return NULL;
    b++;
    unsigned char *out = (unsigned char *) malloc(strlen(b) * 3 / 4 + 3);
    if (!out) return NULL;
    unsigned acc = 0;
    int bits = 0;
    for (; *b && *b != '='; b++) {
	int v = b64val((unsigned char) *b);
	if (v < 0) continue;
	acc = (acc << 6) | (unsigned) v;
	bits += 6;
	if (bits >= 8) {
	    bits -= 8;
	    out[(*n)++] = (unsigned char) ((acc >> bits) & 0xFF);
	}
    }
    return out;
}

/* A data URL decoded into a bitmap GDI+ owns outright (one made from a
   stream keeps reading the stream, which would have to live as long). */
static GpBitmap *bitmap_from_url(const char *url)
{
    if (!gdip_up()) return NULL;
    size_t n = 0;
    unsigned char *bytes = url_bytes(url, &n);
    if (!bytes || !n) { free(bytes); return NULL; }
    GpBitmap *res = NULL;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, n);
    void *p = hg ? GlobalLock(hg) : NULL;
    if (p) {
	memcpy(p, bytes, n);
	GlobalUnlock(hg);
	IStream *st = NULL;
	if (CreateStreamOnHGlobal(hg, TRUE, &st) == S_OK) {
	    GpBitmap *b = NULL;
	    if (GdipCreateBitmapFromStream(st, &b) == Ok && b) {
		UINT w, h;
		res = scaled((GpImage *) b, 0, 0, &w, &h);    /* a full copy */
		GdipDisposeImage((GpImage *) b);
	    }
	    IStream_Release(st);                 /* frees hg too */
	} else
	    GlobalFree(hg);
    } else if (hg)
	GlobalFree(hg);
    free(bytes);
    return res;
}

unsigned char *aiimg_thumbnail_png(const char *url, int maxw, int maxh,
				   size_t *n, int *w, int *h)
{
    *n = 0;
    GpBitmap *b = bitmap_from_url(url);
    if (!b) return NULL;
    UINT tw = 0, th = 0;
    GpBitmap *t = scaled((GpImage *) b, maxw, maxh, &tw, &th);
    GdipDisposeImage((GpImage *) b);
    if (!t) return NULL;
    unsigned char *png = save((GpImage *) t, &PNG_ENC, NULL, n);
    GdipDisposeImage((GpImage *) t);
    if (png) { *w = (int) tw; *h = (int) th; }
    return png;
}

/* Draw b into r of a memory DC, fitted and centred, never enlarged
   beyond twice its size. */
static void draw_fitted(HDC dc, GpBitmap *b, RECT r)
{
    UINT w = 0, h = 0;
    GdipGetImageWidth((GpImage *) b, &w);
    GdipGetImageHeight((GpImage *) b, &h);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (!w || !h || rw <= 0 || rh <= 0) return;
    double f = (double) rw / w;
    if ((double) rh / h < f) f = (double) rh / h;
    if (f > 2.0) f = 2.0;
    int dw = (int) (w * f + 0.5), dh = (int) (h * f + 0.5);
    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(dc, &g) != Ok) return;
    GdipSetInterpolationMode(g, f < 1.0 ? InterpolationModeHighQualityBicubic
			     : InterpolationModeNearestNeighbor);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHighQuality);
    GdipDrawImageRectI(g, (GpImage *) b, r.left + (rw - dw) / 2, r.top + (rh - dh) / 2, dw, dh);
    GdipDeleteGraphics(g);
}

/* --- the viewer: one window per picture opened ---------------------- */

static LRESULT CALLBACK viewer_proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    GpBitmap *b = (GpBitmap *) GetWindowLongPtrW(hw, GWLP_USERDATA);
    switch (m) {
    case WM_ERASEBKGND:
	return 1;
    case WM_SIZE:
	InvalidateRect(hw, NULL, FALSE);
	return 0;
    case WM_PAINT: {
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hw, &ps);
	RECT rc;
	GetClientRect(hw, &rc);
	/* Through a memory bitmap, so that resizing does not flicker. */
	HDC mdc = CreateCompatibleDC(dc);
	HBITMAP mb = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
	HGDIOBJ old = SelectObject(mdc, mb);
	HBRUSH bg = CreateSolidBrush(RGB(0x30, 0x30, 0x30));
	FillRect(mdc, &rc, bg);
	DeleteObject(bg);
	if (b) {
	    RECT in = { 8, 8, rc.right - 8, rc.bottom - 8 };
	    draw_fitted(mdc, b, in);
	}
	BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
	SelectObject(mdc, old);
	DeleteObject(mb);
	DeleteDC(mdc);
	EndPaint(hw, &ps);
	return 0;
    }
    case WM_KEYDOWN:
	if (wp == VK_ESCAPE) { DestroyWindow(hw); return 0; }
	break;
    case WM_NCDESTROY:
	if (b) GdipDisposeImage((GpImage *) b);
	SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
	break;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

int aiimg_show(const char *url, const char *title_u8, HWND owner)
{
    static int registered = 0;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
	WNDCLASSW wc;
	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc = viewer_proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));   /* RGui's own, if any */
	wc.lpszClassName = L"RGuiAIPicture";
	registered = RegisterClassW(&wc) != 0;
	if (!registered) return 0;
    }
    GpBitmap *b = bitmap_from_url(url);
    if (!b) return 0;
    UINT w = 0, h = 0;
    GdipGetImageWidth((GpImage *) b, &w);
    GdipGetImageHeight((GpImage *) b, &h);

    /* Big enough for the picture, at most 85% of the screen RGui is on. */
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    RECT work = { 0, 0, 1024, 768 };
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi))
	work = mi.rcWork;
    int maxw = (work.right - work.left) * 85 / 100, maxh = (work.bottom - work.top) * 85 / 100;
    RECT r = { 0, 0, (LONG) w + 16, (LONG) h + 16 };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    if (ww > maxw || wh > maxh) {
	double f = (double) maxw / ww;
	if ((double) maxh / wh < f) f = (double) maxh / wh;
	ww = (int) (ww * f);
	wh = (int) (wh * f);
    }
    if (ww < 320) ww = 320;
    if (wh < 240) wh = 240;
    int x = work.left + ((work.right - work.left) - ww) / 2;
    int y = work.top + ((work.bottom - work.top) - wh) / 2;

    int tn = MultiByteToWideChar(CP_UTF8, 0, title_u8 ? title_u8 : "Picture", -1, NULL, 0);
    wchar_t *title = (wchar_t *) calloc((size_t) (tn > 0 ? tn : 1) + 16, sizeof(wchar_t));
    if (title) MultiByteToWideChar(CP_UTF8, 0, title_u8 ? title_u8 : "Picture", -1, title, tn);
    HWND hw = CreateWindowExW(0, L"RGuiAIPicture", title ? title : L"Picture", style,
			      x, y, ww, wh, owner, NULL, inst, NULL);
    free(title);
    if (!hw) { GdipDisposeImage((GpImage *) b); return 0; }
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR) b);
    ShowWindow(hw, SW_SHOWNORMAL);
    UpdateWindow(hw);
    return 1;
}

/* --- the strip of attached pictures --------------------------------- */

#define STRIP_TH    56          /* thumbnail height */
#define STRIP_ITEM  104         /* width of one item */
#define STRIP_PAD   6
#define STRIP_X     9           /* radius of the remove button */

typedef struct {
    int n;
    GpBitmap *thumb[16];
    wchar_t *label[16];
    RECT item[16], close[16];
    int hot_close;              /* index under the mouse on an x, or -1 */
    aiimg_strip_fn fn;
    HFONT font;
} strip;

int aiimg_strip_height(void) { return STRIP_PAD + STRIP_TH + 18 + STRIP_PAD; }

static void strip_clear(strip *s)
{
    for (int i = 0; i < s->n; i++) {
	if (s->thumb[i]) GdipDisposeImage((GpImage *) s->thumb[i]);
	free(s->label[i]);
    }
    s->n = 0;
}

static void strip_layout(strip *s)
{
    for (int i = 0; i < s->n; i++) {
	RECT r = { STRIP_PAD + i * (STRIP_ITEM + STRIP_PAD), STRIP_PAD, 0, 0 };
	r.right = r.left + STRIP_ITEM;
	r.bottom = r.top + STRIP_TH + 18;
	s->item[i] = r;
	RECT c = { r.right - 2 * STRIP_X - 2, r.top + 2, r.right - 2, r.top + 2 + 2 * STRIP_X };
	s->close[i] = c;
    }
}

static int strip_hit(strip *s, int x, int y, int *onclose)
{
    POINT p = { x, y };
    for (int i = 0; i < s->n; i++) {
	if (PtInRect(&s->close[i], p)) { *onclose = 1; return i; }
	if (PtInRect(&s->item[i], p))  { *onclose = 0; return i; }
    }
    return -1;
}

static void strip_paint(HWND hw, strip *s, HDC dc)
{
    RECT rc;
    GetClientRect(hw, &rc);
    HDC mdc = CreateCompatibleDC(dc);
    HBITMAP mb = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mdc, mb);
    FillRect(mdc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    HGDIOBJ oldf = SelectObject(mdc, s->font ? (HGDIOBJ) s->font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(mdc, TRANSPARENT);
    for (int i = 0; i < s->n; i++) {
	RECT it = s->item[i];
	RECT box = { it.left, it.top, it.right, it.top + STRIP_TH };
	FillRect(mdc, &box, (HBRUSH) GetStockObject(WHITE_BRUSH));
	FrameRect(mdc, &box, GetSysColorBrush(COLOR_BTNSHADOW));
	if (s->thumb[i]) {
	    RECT in = { box.left + 2, box.top + 2, box.right - 2, box.bottom - 2 };
	    draw_fitted(mdc, s->thumb[i], in);
	}
	/* the remove button: a dark disc with a white x, red under the mouse */
	RECT c = s->close[i];
	HBRUSH disc = CreateSolidBrush(s->hot_close == i ? RGB(0xc4, 0x2b, 0x1c) : RGB(0x50, 0x50, 0x50));
	HGDIOBJ ob = SelectObject(mdc, disc);
	HGDIOBJ op = SelectObject(mdc, GetStockObject(NULL_PEN));
	Ellipse(mdc, c.left, c.top, c.right + 1, c.bottom + 1);
	SelectObject(mdc, op);
	SelectObject(mdc, ob);
	DeleteObject(disc);
	HPEN pen = CreatePen(PS_SOLID, 2, RGB(0xff, 0xff, 0xff));
	op = SelectObject(mdc, pen);
	int cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, d = STRIP_X / 2;
	MoveToEx(mdc, cx - d, cy - d, NULL); LineTo(mdc, cx + d + 1, cy + d + 1);
	MoveToEx(mdc, cx + d, cy - d, NULL); LineTo(mdc, cx - d - 1, cy + d + 1);
	SelectObject(mdc, op);
	DeleteObject(pen);
	RECT lr = { it.left, box.bottom + 2, it.right, it.bottom };
	SetTextColor(mdc, GetSysColor(COLOR_BTNTEXT));
	if (s->label[i])
	    DrawTextW(mdc, s->label[i], -1, &lr, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (s->n) {
	RECT hint = { s->item[s->n - 1].right + 2 * STRIP_PAD, STRIP_PAD, rc.right - STRIP_PAD, STRIP_PAD + STRIP_TH };
	SetTextColor(mdc, GetSysColor(COLOR_GRAYTEXT));
	DrawTextW(mdc, L"Goes with your next question.\nClick a picture to open it; x removes it.",
		  -1, &hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(mdc, oldf);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(mb);
    DeleteDC(mdc);
}

static LRESULT CALLBACK strip_proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    strip *s = (strip *) GetWindowLongPtrW(hw, GWLP_USERDATA);
    switch (m) {
    case WM_ERASEBKGND:
	return 1;
    case WM_PAINT: {
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hw, &ps);
	if (s) strip_paint(hw, s, dc);
	EndPaint(hw, &ps);
	return 0;
    }
    case WM_SIZE:
	InvalidateRect(hw, NULL, FALSE);
	return 0;
    case WM_SETCURSOR:
	if (s && LOWORD(lp) == HTCLIENT) {
	    POINT p;
	    GetCursorPos(&p);
	    ScreenToClient(hw, &p);
	    int onclose = 0;
	    SetCursor(LoadCursor(NULL, strip_hit(s, p.x, p.y, &onclose) >= 0 ? IDC_HAND : IDC_ARROW));
	    return TRUE;
	}
	break;
    case WM_MOUSEMOVE:
	if (s) {
	    int onclose = 0;
	    int i = strip_hit(s, (short) LOWORD(lp), (short) HIWORD(lp), &onclose);
	    int hot = (i >= 0 && onclose) ? i : -1;
	    if (hot != s->hot_close) {
		s->hot_close = hot;
		InvalidateRect(hw, NULL, FALSE);
	    }
	    TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, hw, 0 };
	    TrackMouseEvent(&t);
	}
	return 0;
    case WM_MOUSELEAVE:
	if (s && s->hot_close >= 0) {
	    s->hot_close = -1;
	    InvalidateRect(hw, NULL, FALSE);
	}
	return 0;
    case WM_LBUTTONUP:
	if (s && s->fn) {
	    int onclose = 0;
	    int i = strip_hit(s, (short) LOWORD(lp), (short) HIWORD(lp), &onclose);
	    /* The callback may change the strip: nothing of s after it. */
	    if (i >= 0) s->fn(i, onclose);
	}
	return 0;
    case WM_NCDESTROY:
	if (s) {
	    strip_clear(s);
	    if (s->font) DeleteObject(s->font);
	    free(s);
	    SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
	}
	break;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

HWND aiimg_strip_new(HWND parent, aiimg_strip_fn fn)
{
    static int registered = 0;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
	WNDCLASSW wc;
	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc = strip_proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"RGuiAIStrip";
	registered = RegisterClassW(&wc) != 0;
	if (!registered) return NULL;
    }
    strip *s = (strip *) calloc(1, sizeof(strip));
    if (!s) return NULL;
    s->fn = fn;
    s->hot_close = -1;
    s->font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
			  CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND hw = CreateWindowExW(0, L"RGuiAIStrip", L"", WS_CHILD | WS_CLIPSIBLINGS,
			      0, 0, 10, aiimg_strip_height(), parent, NULL, inst, NULL);
    if (!hw) { if (s->font) DeleteObject(s->font); free(s); return NULL; }
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR) s);
    return hw;
}

void aiimg_strip_set(HWND hw, char *const *urls, const char *const *labels, int n)
{
    strip *s = hw ? (strip *) GetWindowLongPtrW(hw, GWLP_USERDATA) : NULL;
    if (!s) return;
    strip_clear(s);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
	GpBitmap *b = bitmap_from_url(urls[i]);
	UINT tw, th;
	s->thumb[i] = b ? scaled((GpImage *) b, 2 * STRIP_ITEM, 2 * STRIP_TH, &tw, &th) : NULL;
	if (b) GdipDisposeImage((GpImage *) b);
	const char *l = labels[i] ? labels[i] : "";
	int wn = MultiByteToWideChar(CP_UTF8, 0, l, -1, NULL, 0);
	s->label[i] = (wchar_t *) calloc((size_t) (wn > 0 ? wn : 1), sizeof(wchar_t));
	if (s->label[i]) MultiByteToWideChar(CP_UTF8, 0, l, -1, s->label[i], wn);
    }
    s->n = n;
    s->hot_close = -1;
    strip_layout(s);
    InvalidateRect(hw, NULL, FALSE);
}
