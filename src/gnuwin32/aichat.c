/*
 *  R : A Computer Language for Statistical Data Analysis
 *  file aichat.c -- local AI assistant panel for RGui
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

/*
 *  DESIGN NOTES
 *
 *  The assistant is a GraphApp window (an MDI child when RGui runs in MDI
 *  mode) holding a read-only transcript, an input box and a row of buttons.
 *  It talks HTTP to a llama.cpp "llama-server" process that RGui starts on
 *  demand as a child process, on the loopback interface only.
 *
 *  Running the model out of process is deliberate:
 *
 *    - a crash, a hang or a missing model file cannot take RGui with it;
 *    - no C++ runtime, BLAS or model library has to be linked into R.dll;
 *    - the helper can be upgraded by replacing one .exe on the pendrive.
 *
 *  Threading.  All UI work happens on the R main thread, which is also the
 *  GUI thread.  One worker thread at a time performs the blocking network
 *  I/O.  The worker never touches the R API, GraphApp, or any window: it
 *  appends UTF-8 text to a buffer under a critical section and posts a
 *  message to a message-only window, exactly as the console reader thread
 *  in system.c does.  The main thread drains that buffer from its message
 *  handler, which runs inside doevent() and therefore inside
 *  R_ProcessEvents().  That is what lets R keep working while the model
 *  generates.
 *
 *  Consequently nothing in this file below the "worker thread" marker may
 *  call error(), Rprintf(), R_alloc() or allocate SEXPs.  Plain malloc and
 *  free only.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include "win-nls.h"

#ifdef Win32
#define USE_MDI 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN 1
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <winhttp.h>
#include <bcrypt.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#include "graphapp/ga.h"
#include "graphapp/graphapp.h"
#include "console.h"
#include "rui.h"
#include "aichat.h"

#define gettext GA_gettext

RECT *RgetMDIsize(void);        /* rui.c */

/* ------------------------------------------------------------------ */
/* small growable byte buffer                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *s;
    size_t n;     /* bytes used, not counting the NUL */
    size_t cap;
} dynbuf;

static void db_init(dynbuf *b)
{
    b->s = NULL; b->n = 0; b->cap = 0;
}

static int db_reserve(dynbuf *b, size_t extra)
{
    size_t need = b->n + extra + 1;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
	if (cap > (size_t)1 << 28) { cap = need; break; }
	cap *= 2;
    }
    char *p = (char *) realloc(b->s, cap);
    if (!p) return 0;
    b->s = p; b->cap = cap;
    return 1;
}

static int db_addn(dynbuf *b, const char *s, size_t n)
{
    if (!n) return 1;
    if (!db_reserve(b, n)) return 0;
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
    return 1;
}

static int db_add(dynbuf *b, const char *s)
{
    return s ? db_addn(b, s, strlen(s)) : 1;
}

static int db_addc(dynbuf *b, char c)
{
    return db_addn(b, &c, 1);
}

static void db_free(dynbuf *b)
{
    free(b->s);
    db_init(b);
}

/* Detach the buffer's storage; caller owns it and must free() it.
   Never returns NULL for an empty buffer -- returns an empty string. */
static char *db_release(dynbuf *b)
{
    char *p = b->s;
    if (!p) { p = (char *) malloc(1); if (p) p[0] = '\0'; }
    db_init(b);
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = (char *) malloc(strlen(s) + 1);
    if (p) strcpy(p, s);
    return p;
}

/* ------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16                                                    */
/* ------------------------------------------------------------------ */

/* Returns a malloc'd wide string, or NULL. */
static wchar_t *u8_to_wcs(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *) malloc(n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}

/* Returns a malloc'd UTF-8 string, or NULL. */
static char *wcs_to_u8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *) malloc(n);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
	free(s); return NULL;
    }
    return s;
}

/* Append one Unicode code point to a buffer as UTF-8. */
static void db_add_codepoint(dynbuf *b, unsigned int cp)
{
    if (cp < 0x80) {
	db_addc(b, (char) cp);
    } else if (cp < 0x800) {
	db_addc(b, (char) (0xC0 | (cp >> 6)));
	db_addc(b, (char) (0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
	db_addc(b, (char) (0xE0 | (cp >> 12)));
	db_addc(b, (char) (0x80 | ((cp >> 6) & 0x3F)));
	db_addc(b, (char) (0x80 | (cp & 0x3F)));
    } else {
	db_addc(b, (char) (0xF0 | (cp >> 18)));
	db_addc(b, (char) (0x80 | ((cp >> 12) & 0x3F)));
	db_addc(b, (char) (0x80 | ((cp >> 6) & 0x3F)));
	db_addc(b, (char) (0x80 | (cp & 0x3F)));
    }
}

/* ------------------------------------------------------------------ */
/* configuration (R_HOME/etc/Rai.conf)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int  loaded;
    int  enabled;
    int  autostart;
    int  thinking;          /* ask the model to emit reasoning */
    int  strip_think;       /* hide <think>...</think> from the transcript */
    int  port;
    int  ctx_size;
    int  n_predict;
    int  threads;           /* 0 = let the server decide */
    int  gpu_layers;
    int  keep_history;      /* how many past turns to resend */
    int  context_max_chars;
    int  startup_timeout;   /* seconds to wait for the model to load */
    int  request_timeout;   /* seconds of silence before giving up */
    int  font_points;
    int  hotkey;            /* accelerator letter, 0 for none */
    double temperature;
    double top_p;
    char host[64];
    char server_exe[MAX_PATH];
    char model[MAX_PATH];
    char system_prompt_file[MAX_PATH];
    char context_dir[MAX_PATH];
    char extra_args[512];
    char model_url[1024];   /* where to fetch the model on first use */
    char model_sha256[72];  /* its published SHA-256, lower-case hex */
    long long model_bytes;  /* its size, for the disk check and progress */
} aiconf;

static aiconf CFG;

static const char *ai_rhome(void)
{
    static char rhome[MAX_PATH] = "";
    if (!rhome[0]) {
	const char *p = getenv("R_HOME");
	if (p && *p) {
	    strncpy(rhome, p, MAX_PATH - 1);
	    rhome[MAX_PATH - 1] = '\0';
	} else {
	    /* Last resort: the directory two levels above R.dll. */
	    strcpy(rhome, ".");
	}
	/* R_HOME uses forward slashes; Win32 accepts both. */
    }
    return rhome;
}

/* Turn a possibly relative configured path into an absolute one under
   R_HOME.  Backslashes and forward slashes are both accepted. */
static void ai_resolve(const char *in, char *out, size_t outlen)
{
    out[0] = '\0';
    if (!in || !*in) return;
    int absolute = (in[1] == ':') || (in[0] == '/' && in[1] == '/')
	         || (in[0] == '\\' && in[1] == '\\');
    if (absolute)
	snprintf(out, outlen, "%s", in);
    else
	snprintf(out, outlen, "%s/%s", ai_rhome(), in);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}

static void ai_trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' ||
		 s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = '\0';
}

static int ai_yes(const char *v)
{
    return !strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
	   !strcasecmp(v, "on")  || !strcmp(v, "1");
}

static void ai_defaults(void)
{
    memset(&CFG, 0, sizeof CFG);
    CFG.enabled           = 1;
    CFG.autostart         = 1;
    CFG.thinking          = 0;
    CFG.strip_think       = 1;
    CFG.port              = 8713;
    CFG.ctx_size          = 8192;
    CFG.n_predict         = 1024;
    CFG.threads           = 0;
    CFG.gpu_layers        = 0;
    CFG.keep_history      = 12;
    CFG.context_max_chars = 12000;
    CFG.startup_timeout   = 240;
    CFG.request_timeout   = 120;
    CFG.font_points       = 0;      /* 0 = follow the console font */
    CFG.hotkey            = 'T';
    CFG.temperature       = 0.3;
    CFG.top_p             = 0.9;
    strcpy(CFG.host, "127.0.0.1");
    ai_resolve("ai/llama/llama-server.exe", CFG.server_exe, MAX_PATH);
    ai_resolve("ai/models/Qwen3.5-4B-Q4_K_M.gguf", CFG.model, MAX_PATH);
    snprintf(CFG.model_url, sizeof CFG.model_url, "%s",
	     "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/"
	     "Qwen3.5-4B-Q4_K_M.gguf");
    snprintf(CFG.model_sha256, sizeof CFG.model_sha256, "%s",
	     "00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4");
    CFG.model_bytes = 2740937888LL;
    ai_resolve("ai/system_prompt.txt", CFG.system_prompt_file, MAX_PATH);
    ai_resolve("ai/context",           CFG.context_dir,        MAX_PATH);
}

static void ai_load_config(void)
{
    if (CFG.loaded) return;
    ai_defaults();
    CFG.loaded = 1;

    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s/etc/Rai.conf", ai_rhome());
    FILE *f = fopen(path, "r");
    if (!f) return;   /* defaults are fine; the panel will report what is missing */

    char line[1024];
    while (fgets(line, sizeof line, f)) {
	char *hash = strchr(line, '#');
	if (hash) *hash = '\0';
	char *eq = strchr(line, '=');
	if (!eq) continue;
	*eq = '\0';
	char *k = line, *v = eq + 1;
	ai_trim(k); ai_trim(v);
	if (!*k) continue;

	if      (!strcasecmp(k, "enabled"))        CFG.enabled = ai_yes(v);
	else if (!strcasecmp(k, "autostart"))      CFG.autostart = ai_yes(v);
	else if (!strcasecmp(k, "thinking"))       CFG.thinking = ai_yes(v);
	else if (!strcasecmp(k, "strip_think"))    CFG.strip_think = ai_yes(v);
	else if (!strcasecmp(k, "port"))           CFG.port = atoi(v);
	else if (!strcasecmp(k, "ctx_size"))       CFG.ctx_size = atoi(v);
	else if (!strcasecmp(k, "n_predict"))      CFG.n_predict = atoi(v);
	else if (!strcasecmp(k, "threads"))        CFG.threads = atoi(v);
	else if (!strcasecmp(k, "gpu_layers"))     CFG.gpu_layers = atoi(v);
	else if (!strcasecmp(k, "keep_history"))   CFG.keep_history = atoi(v);
	else if (!strcasecmp(k, "context_max_chars")) CFG.context_max_chars = atoi(v);
	else if (!strcasecmp(k, "startup_timeout"))CFG.startup_timeout = atoi(v);
	else if (!strcasecmp(k, "request_timeout"))CFG.request_timeout = atoi(v);
	else if (!strcasecmp(k, "font_points"))    CFG.font_points = atoi(v);
	else if (!strcasecmp(k, "temperature"))    CFG.temperature = atof(v);
	else if (!strcasecmp(k, "top_p"))          CFG.top_p = atof(v);
	else if (!strcasecmp(k, "hotkey"))         CFG.hotkey = toupper((unsigned char)v[0]);
	else if (!strcasecmp(k, "host")) {
	    strncpy(CFG.host, v, sizeof(CFG.host) - 1);
	    CFG.host[sizeof(CFG.host) - 1] = '\0';
	}
	else if (!strcasecmp(k, "model_url"))
	    snprintf(CFG.model_url, sizeof CFG.model_url, "%s", v);
	else if (!strcasecmp(k, "model_sha256"))
	    snprintf(CFG.model_sha256, sizeof CFG.model_sha256, "%s", v);
	else if (!strcasecmp(k, "model_bytes"))
	    CFG.model_bytes = strtoll(v, NULL, 10);
	else if (!strcasecmp(k, "extra_args")) {
	    strncpy(CFG.extra_args, v, sizeof(CFG.extra_args) - 1);
	    CFG.extra_args[sizeof(CFG.extra_args) - 1] = '\0';
	}
	else if (!strcasecmp(k, "server_exe"))  ai_resolve(v, CFG.server_exe, MAX_PATH);
	else if (!strcasecmp(k, "model"))       ai_resolve(v, CFG.model, MAX_PATH);
	else if (!strcasecmp(k, "system_prompt_file"))
	    ai_resolve(v, CFG.system_prompt_file, MAX_PATH);
	else if (!strcasecmp(k, "context_dir")) ai_resolve(v, CFG.context_dir, MAX_PATH);
    }
    fclose(f);

    if (CFG.port <= 0 || CFG.port > 65535) CFG.port = 8713;
    if (CFG.ctx_size < 512)  CFG.ctx_size = 512;
    if (CFG.n_predict < 16)  CFG.n_predict = 16;
    if (CFG.keep_history < 0) CFG.keep_history = 0;
    if (CFG.startup_timeout < 10) CFG.startup_timeout = 10;
    if (CFG.request_timeout < 10) CFG.request_timeout = 10;
    if (CFG.context_max_chars < 0) CFG.context_max_chars = 0;
}

int aichat_enabled(void)
{
    ai_load_config();
    return CFG.enabled;
}

int aichat_hotkey(void)
{
    ai_load_config();
    return CFG.hotkey;
}

/* ------------------------------------------------------------------ */
/* course material                                                     */
/* ------------------------------------------------------------------ */

static const char AI_FALLBACK_PROMPT[] =
    "You are an assistant built into RGui, helping a student with a "
    "statistics course that uses R.\n"
    "Rules:\n"
    "- Answer about R, statistics and the course material only.\n"
    "- Prefer base R and the packages the course already uses. Do not "
    "introduce a new package unless there is no reasonable base R way, "
    "and say so when you do.\n"
    "- Put every piece of runnable code in a fenced block that starts "
    "with ```r on its own line and ends with ``` on its own line, so the "
    "Copy code button can find it.\n"
    "- Keep code short and commented, and explain what the output means "
    "in plain language.\n"
    "- When reference material is supplied below, follow its notation, "
    "method and conventions even where another approach would also work.\n"
    "- If you are unsure, say so rather than inventing a function name.\n";

/* Read a whole file as UTF-8 text.  Returns malloc'd text, or NULL.
   A UTF-8 BOM is dropped; CRLF is normalised to LF. */
static char *ai_read_text_file(const char *path, size_t limit)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    if (limit && (size_t) sz > limit) sz = (long) limit;
    char *buf = (char *) malloc((size_t) sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t) sz, f);
    fclose(f);
    buf[got] = '\0';

    char *start = buf;
    if (got >= 3 && (unsigned char) buf[0] == 0xEF &&
	(unsigned char) buf[1] == 0xBB && (unsigned char) buf[2] == 0xBF)
	start = buf + 3;

    char *out = (char *) malloc(strlen(start) + 1);
    if (!out) { free(buf); return NULL; }
    char *d = out;
    for (char *s = start; *s; s++) if (*s != '\r') *d++ = *s;
    *d = '\0';
    free(buf);
    return out;
}

/* Count how many of the question's words of 4+ characters occur in text.
   Deliberately crude: it only has to order a handful of course files. */
static int ai_relevance(const char *text, const char *question)
{
    if (!text || !question) return 0;
    int score = 0;
    char word[64];
    const char *q = question;
    while (*q) {
	int n = 0;
	while (*q && (isalnum((unsigned char) *q) || *q == '.' || *q == '_')) {
	    if (n < (int) sizeof(word) - 1)
		word[n++] = (char) tolower((unsigned char) *q);
	    q++;
	}
	word[n] = '\0';
	if (!n) { if (!*q) break; q++; continue; }
	if (n < 4) continue;
	for (const char *t = text; *t; t++) {
	    if (tolower((unsigned char) *t) != word[0]) continue;
	    int i = 1;
	    while (word[i] && t[i] &&
		   tolower((unsigned char) t[i]) == word[i]) i++;
	    if (!word[i]) { score++; break; }
	}
    }
    return score;
}

typedef struct {
    char  path[2 * MAX_PATH];
    char  name[MAX_PATH];
    char *text;
    int   score;
} aidoc;

/* Build the system message: the course prompt plus as much reference
   material as the budget allows, most relevant file first. */
static char *ai_build_system_prompt(const char *question)
{
    dynbuf out;
    db_init(&out);

    char *prompt = ai_read_text_file(CFG.system_prompt_file, 64 * 1024);
    db_add(&out, prompt ? prompt : AI_FALLBACK_PROMPT);
    free(prompt);
    db_add(&out, "\n");

    if (CFG.context_max_chars <= 0 || !CFG.context_dir[0])
	return db_release(&out);

    char pattern[MAX_PATH + 8];
    snprintf(pattern, sizeof pattern, "%s\\*", CFG.context_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return db_release(&out);

    aidoc *docs = NULL;
    int ndocs = 0, cdocs = 0;
    do {
	if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
	const char *ext = strrchr(fd.cFileName, '.');
	if (!ext) continue;
	if (strcasecmp(ext, ".txt") && strcasecmp(ext, ".md") &&
	    strcasecmp(ext, ".r")   && strcasecmp(ext, ".rmd") &&
	    strcasecmp(ext, ".csv")) continue;
	if (ndocs == cdocs) {
	    int nc = cdocs ? cdocs * 2 : 8;
	    aidoc *nd = (aidoc *) realloc(docs, nc * sizeof(aidoc));
	    if (!nd) break;
	    docs = nd; cdocs = nc;
	}
	aidoc *d = &docs[ndocs];
	snprintf(d->path, sizeof d->path, "%s\\%s", CFG.context_dir, fd.cFileName);
	snprintf(d->name, sizeof d->name, "%s", fd.cFileName);
	d->text = ai_read_text_file(d->path, (size_t) CFG.context_max_chars);
	if (!d->text) continue;
	d->score = ai_relevance(d->text, question);
	ndocs++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    /* Highest score first, then by name, so the choice is reproducible. */
    for (int i = 1; i < ndocs; i++) {
	aidoc tmp = docs[i];
	int j = i - 1;
	while (j >= 0 && (docs[j].score < tmp.score ||
			  (docs[j].score == tmp.score &&
			   strcmp(docs[j].name, tmp.name) > 0))) {
	    docs[j+1] = docs[j]; j--;
	}
	docs[j+1] = tmp;
    }

    if (ndocs > 0) {
	db_add(&out,
	       "\n=== COURSE REFERENCE MATERIAL ===\n"
	       "The following is supplied by the student as the authority "
	       "for this course. Follow it in preference to your own "
	       "habits.\n");
	size_t budget = (size_t) CFG.context_max_chars;
	for (int i = 0; i < ndocs; i++) {
	    if (budget == 0) break;
	    size_t len = strlen(docs[i].text);
	    db_add(&out, "\n--- file: ");
	    db_add(&out, docs[i].name);
	    db_add(&out, " ---\n");
	    if (len > budget) {
		db_addn(&out, docs[i].text, budget);
		db_add(&out, "\n[...truncated...]\n");
		budget = 0;
	    } else {
		db_add(&out, docs[i].text);
		budget -= len;
	    }
	}
	db_add(&out, "\n=== END OF COURSE REFERENCE MATERIAL ===\n");
    }

    for (int i = 0; i < ndocs; i++) free(docs[i].text);
    free(docs);
    return db_release(&out);
}

/* ------------------------------------------------------------------ */
/* JSON: just enough of it                                             */
/* ------------------------------------------------------------------ */

static void json_escape(dynbuf *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
	switch (*p) {
	case '"':  db_add(b, "\\\""); break;
	case '\\': db_add(b, "\\\\"); break;
	case '\n': db_add(b, "\\n");  break;
	case '\r': db_add(b, "\\r");  break;
	case '\t': db_add(b, "\\t");  break;
	case '\b': db_add(b, "\\b");  break;
	case '\f': db_add(b, "\\f");  break;
	default:
	    if (*p < 0x20) {
		char tmp[8];
		snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned) *p);
		db_add(b, tmp);
	    } else
		db_addc(b, (char) *p);
	}
    }
}

/* Decode a JSON string body starting just after the opening quote.
   Returns a pointer to the closing quote, or NULL if malformed. */
static const char *json_unescape(const char *p, dynbuf *out)
{
    while (*p) {
	if (*p == '"') return p;
	if (*p != '\\') { db_addc(out, *p++); continue; }
	p++;
	switch (*p) {
	case '"':  db_addc(out, '"');  p++; break;
	case '\\': db_addc(out, '\\'); p++; break;
	case '/':  db_addc(out, '/');  p++; break;
	case 'n':  db_addc(out, '\n'); p++; break;
	case 'r':  db_addc(out, '\r'); p++; break;
	case 't':  db_addc(out, '\t'); p++; break;
	case 'b':  db_addc(out, '\b'); p++; break;
	case 'f':  db_addc(out, '\f'); p++; break;
	case 'u': {
	    unsigned int cp = 0;
	    for (int i = 1; i <= 4; i++) {
		char c = p[i];
		if (!isxdigit((unsigned char) c)) return NULL;
		cp = cp * 16 + (unsigned)
		    (c <= '9' ? c - '0' : (tolower((unsigned char) c) - 'a' + 10));
	    }
	    p += 5;
	    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
		unsigned int lo = 0;
		int ok = 1;
		for (int i = 2; i <= 5; i++) {
		    char c = p[i];
		    if (!isxdigit((unsigned char) c)) { ok = 0; break; }
		    lo = lo * 16 + (unsigned)
			(c <= '9' ? c - '0' : (tolower((unsigned char) c) - 'a' + 10));
		}
		if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
		    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
		    p += 6;
		}
	    }
	    db_add_codepoint(out, cp);
	    break;
	}
	default: return NULL;
	}
    }
    return NULL;
}

/* Find  "key" : "..."  and return the decoded value (malloc'd), or NULL.
   String values are skipped over properly, so a key name appearing inside
   some other value cannot fool it. */
static char *json_find_string(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while (*p) {
	if (*p != '"') { p++; continue; }
	dynbuf name; db_init(&name);
	const char *end = json_unescape(p + 1, &name);
	if (!end) { db_free(&name); return NULL; }
	int match = (name.n == klen && name.s && !memcmp(name.s, key, klen));
	db_free(&name);
	p = end + 1;
	if (!match) continue;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (*p != ':') continue;      /* it was a value, not a key */
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (*p != '"') return NULL;   /* null / number / object: no string */
	dynbuf val; db_init(&val);
	if (!json_unescape(p + 1, &val)) { db_free(&val); return NULL; }
	return db_release(&val);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* HTTP over Winsock, loopback only                                    */
/* ------------------------------------------------------------------ */

/* Winsock rather than WinHTTP/WinINet on purpose: those consult the
   system proxy configuration, which on a managed machine can be set to
   something that breaks even loopback requests, and they cannot be
   unblocked from another thread as cleanly as shutdown() on a socket. */

static int  g_wsa_up = 0;
static CRITICAL_SECTION g_cs;
static int  g_cs_up = 0;
static SOCKET g_active_sock = INVALID_SOCKET;   /* guarded by g_cs */
static volatile LONG g_cancel = 0;

static void ai_lock(void)   { if (g_cs_up) EnterCriticalSection(&g_cs); }
static void ai_unlock(void) { if (g_cs_up) LeaveCriticalSection(&g_cs); }

static int ai_wsa_start(void)
{
    if (g_wsa_up) return 1;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    g_wsa_up = 1;
    return 1;
}

static SOCKET ai_connect(const char *host, int port, int timeout_s)
{
    if (!ai_wsa_start()) return INVALID_SOCKET;

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
	return INVALID_SOCKET;

    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
	s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (s == INVALID_SOCKET) continue;
	if (connect(s, ai->ai_addr, (int) ai->ai_addrlen) == 0) break;
	closesocket(s);
	s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    DWORD tv = (DWORD) timeout_s * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *) &tv, sizeof tv);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof one);
    return s;
}

static int ai_send_all(SOCKET s, const char *buf, size_t len)
{
    while (len) {
	int n = send(s, buf, (int) (len > 32768 ? 32768 : len), 0);
	if (n <= 0) return 0;
	buf += n; len -= (size_t) n;
    }
    return 1;
}

/* Buffered reader that also undoes chunked transfer encoding. */
typedef struct {
    SOCKET s;
    char   buf[16384];
    int    len, pos;
    int    chunked;
    long   remaining;    /* bytes left in this chunk, or in the body */
    int    have_length;  /* Content-Length was present */
    int    done;
} httpstream;

static void hs_init(httpstream *hs, SOCKET s)
{
    memset(hs, 0, sizeof *hs);
    hs->s = s;
    hs->remaining = -1;
}

/* Refill the raw buffer.  Returns bytes read, 0 on clean EOF, -1 on error. */
static int hs_fill(httpstream *hs)
{
    if (hs->pos < hs->len) return hs->len - hs->pos;
    hs->pos = hs->len = 0;
    int n = recv(hs->s, hs->buf, (int) sizeof hs->buf, 0);
    if (n <= 0) return n == 0 ? 0 : -1;
    hs->len = n;
    return n;
}

/* Read one raw CRLF- or LF-terminated line (terminator stripped).
   Returns 1 on success, 0 on EOF, -1 on error. */
static int hs_raw_line(httpstream *hs, dynbuf *out)
{
    out->n = 0;
    if (out->s) out->s[0] = '\0';
    for (;;) {
	if (hs->pos >= hs->len) {
	    int n = hs_fill(hs);
	    if (n <= 0) return out->n ? 1 : n;
	}
	char c = hs->buf[hs->pos++];
	if (c == '\n') {
	    if (out->n && out->s[out->n - 1] == '\r') {
		out->n--; out->s[out->n] = '\0';
	    }
	    return 1;
	}
	db_addc(out, c);
    }
}

/* Read up to max decoded body bytes.  Returns count, 0 at end of body,
   -1 on error. */
static int hs_body(httpstream *hs, char *out, int max)
{
    if (hs->done) return 0;

    if (hs->chunked && hs->remaining <= 0) {
	dynbuf line; db_init(&line);
	/* after the first chunk there is a bare CRLF to swallow */
	if (hs->remaining == 0) {
	    if (hs_raw_line(hs, &line) <= 0) { db_free(&line); return -1; }
	}
	if (hs_raw_line(hs, &line) <= 0) { db_free(&line); return -1; }
	long sz = strtol(line.s ? line.s : "0", NULL, 16);
	db_free(&line);
	if (sz <= 0) { hs->done = 1; return 0; }
	hs->remaining = sz;
    } else if (!hs->chunked && hs->have_length && hs->remaining <= 0) {
	hs->done = 1;
	return 0;
    }

    if (hs->pos >= hs->len) {
	int n = hs_fill(hs);
	if (n < 0) return -1;
	if (n == 0) { hs->done = 1; return 0; }
    }
    int avail = hs->len - hs->pos;
    if (avail > max) avail = max;
    if (hs->remaining > 0 && avail > hs->remaining) avail = (int) hs->remaining;
    memcpy(out, hs->buf + hs->pos, (size_t) avail);
    hs->pos += avail;
    if (hs->remaining > 0) hs->remaining -= avail;
    return avail;
}

/* Send a request and consume the status line and headers.
   Returns the HTTP status code, or -1.  On success the stream is
   positioned at the first body byte and knows its framing. */
static int hs_request(httpstream *hs, const char *method, const char *path,
		      const char *body, size_t bodylen)
{
    dynbuf req; db_init(&req);
    db_add(&req, method); db_add(&req, " "); db_add(&req, path);
    db_add(&req, " HTTP/1.1\r\nHost: ");
    db_add(&req, CFG.host);
    char hdr[128];
    snprintf(hdr, sizeof hdr, ":%d\r\n", CFG.port);
    db_add(&req, hdr);
    db_add(&req, "User-Agent: RGui-AI\r\nAccept: text/event-stream, application/json\r\n"
		 "Connection: close\r\n");
    if (body) {
	snprintf(hdr, sizeof hdr, "Content-Length: %lu\r\n", (unsigned long) bodylen);
	db_add(&req, hdr);
	db_add(&req, "Content-Type: application/json\r\n");
    }
    db_add(&req, "\r\n");
    if (body) db_addn(&req, body, bodylen);

    int ok = ai_send_all(hs->s, req.s, req.n);
    db_free(&req);
    if (!ok) return -1;

    dynbuf line; db_init(&line);
    if (hs_raw_line(hs, &line) <= 0) { db_free(&line); return -1; }
    int status = -1;
    if (line.s && !strncmp(line.s, "HTTP/", 5)) {
	const char *sp = strchr(line.s, ' ');
	if (sp) status = atoi(sp + 1);
    }
    for (;;) {
	if (hs_raw_line(hs, &line) <= 0) { db_free(&line); return -1; }
	if (!line.n) break;                    /* blank line: headers done */
	if (!strncasecmp(line.s, "Transfer-Encoding:", 18)) {
	    if (strstr(line.s, "chunked") || strstr(line.s, "Chunked"))
		hs->chunked = 1;
	} else if (!strncasecmp(line.s, "Content-Length:", 15)) {
	    hs->have_length = 1;
	    hs->remaining = atol(line.s + 15);
	}
    }
    db_free(&line);
    if (!hs->chunked && !hs->have_length) hs->remaining = -1;  /* read to EOF */
    return status;
}

/* One-shot request; the decoded body is returned in *out (may be NULL). */
static int ai_http(const char *method, const char *path,
		   const char *body, char **out, int timeout_s)
{
    if (out) *out = NULL;
    SOCKET s = ai_connect(CFG.host, CFG.port, timeout_s);
    if (s == INVALID_SOCKET) return -1;
    httpstream hs;
    hs_init(&hs, s);
    int status = hs_request(&hs, method, path, body, body ? strlen(body) : 0);
    if (status > 0 && out) {
	dynbuf b; db_init(&b);
	char tmp[4096];
	int n;
	while ((n = hs_body(&hs, tmp, (int) sizeof tmp)) > 0)
	    db_addn(&b, tmp, (size_t) n);
	*out = db_release(&b);
    }
    closesocket(s);
    return status;
}

/* ------------------------------------------------------------------ */
/* the llama-server helper process                                     */
/* ------------------------------------------------------------------ */

static HANDLE g_job      = NULL;
static HANDLE g_srv_proc = NULL;
static DWORD  g_srv_pid  = 0;
static wchar_t g_srv_log[MAX_PATH] = L"";   /* llama-server's stdout+stderr */

/* The warm-up thread started when the panel opens and the answer thread
   started by Send can both reach ai_server_start().  Without this lock
   they race and the loser leaves a second llama-server that fails to
   bind the port.  It is a separate lock from g_cs because starting the
   server blocks for seconds and g_cs is taken by the GUI thread. */
static CRITICAL_SECTION g_srv_cs;
static int g_srv_cs_up = 0;

static void ai_srv_lock(void)   { if (g_srv_cs_up) EnterCriticalSection(&g_srv_cs); }
static void ai_srv_unlock(void) { if (g_srv_cs_up) LeaveCriticalSection(&g_srv_cs); }

/* 200 means the model is loaded and ready; 503 means still loading. */
static int ai_server_health(void)
{
    char *body = NULL;
    int st = ai_http("GET", "/health", NULL, &body, 3);
    free(body);
    return st;
}

static int ai_file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Start llama-server as a child in a job object that kills it when RGui
   exits, so nothing is left holding the pendrive open.  Returns 1 if a
   process was started (or one was already running), 0 with a message in
   err otherwise. */
static int ai_server_start_locked(char *err, size_t errlen)
{
    if (g_srv_proc) {
	if (WaitForSingleObject(g_srv_proc, 0) == WAIT_TIMEOUT) return 1;
	CloseHandle(g_srv_proc);
	g_srv_proc = NULL;
    }
    /* Somebody else may already be serving on that port. */
    if (ai_server_health() > 0) return 1;

    if (!ai_file_exists(CFG.server_exe)) {
	snprintf(err, errlen,
		 "The model server was not found:\r\n  %s\r\n\r\n"
		 "Check server_exe in R_HOME\\etc\\Rai.conf.", CFG.server_exe);
	return 0;
    }
    if (!ai_file_exists(CFG.model)) {
	snprintf(err, errlen,
		 "The model file was not found:\r\n  %s\r\n\r\n"
		 "Check model in R_HOME\\etc\\Rai.conf.", CFG.model);
	return 0;
    }

    if (!g_job) {
	g_job = CreateJobObject(NULL, NULL);
	if (g_job) {
	    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
	    memset(&li, 0, sizeof li);
	    li.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation,
				    &li, sizeof li);
	}
    }

    dynbuf cmd; db_init(&cmd);
    char num[64];
    db_add(&cmd, "\""); db_add(&cmd, CFG.server_exe); db_add(&cmd, "\"");
    db_add(&cmd, " -m \""); db_add(&cmd, CFG.model); db_add(&cmd, "\"");
    db_add(&cmd, " --host "); db_add(&cmd, CFG.host);
    snprintf(num, sizeof num, " --port %d", CFG.port);          db_add(&cmd, num);
    snprintf(num, sizeof num, " -c %d", CFG.ctx_size);          db_add(&cmd, num);
    if (CFG.threads > 0) {
	snprintf(num, sizeof num, " -t %d", CFG.threads);       db_add(&cmd, num);
    }
    snprintf(num, sizeof num, " -ngl %d", CFG.gpu_layers);      db_add(&cmd, num);
    if (CFG.extra_args[0]) { db_add(&cmd, " "); db_add(&cmd, CFG.extra_args); }

    /* Run it from the server's own directory so that the DLLs shipped
       beside it (ggml, llama, the OpenMP runtime) are found without
       touching PATH. */
    char workdir[MAX_PATH];
    snprintf(workdir, MAX_PATH, "%s", CFG.server_exe);
    char *slash = strrchr(workdir, '\\');
    if (slash) *slash = '\0'; else strcpy(workdir, ".");

    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof pi);

    /* Keep the server's own output.  When it refuses to start -- an
       option renamed in a newer llama.cpp, a missing DLL, a truncated
       model -- the reason is in there, and ai_server_log_reason() puts
       it in the status line instead of a bare "exited". */
    HANDLE logh = INVALID_HANDLE_VALUE;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    BOOL inherit = FALSE;
    {
	wchar_t tmp[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, tmp);
	if (n > 0 && n < MAX_PATH - 32) {
	    wcscat(tmp, L"rgui-llama-server.log");
	    wcscpy(g_srv_log, tmp);
	    SECURITY_ATTRIBUTES sa;
	    sa.nLength = sizeof sa;
	    sa.lpSecurityDescriptor = NULL;
	    sa.bInheritHandle = TRUE;
	    logh = CreateFileW(tmp, GENERIC_WRITE,
			       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			       &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	}
    }
    if (logh != INVALID_HANDLE_VALUE) {
	SIZE_T sz = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
	attrs = (LPPROC_THREAD_ATTRIBUTE_LIST) malloc(sz);
	if (attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &sz)) {
	    if (UpdateProcThreadAttribute(attrs, 0,
					  PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
					  &logh, sizeof logh, NULL, NULL)) {
		si.lpAttributeList = attrs;
		si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput  = NULL;
		si.StartupInfo.hStdOutput = logh;
		si.StartupInfo.hStdError  = logh;
		inherit = TRUE;
	    } else {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
		attrs = NULL;
	    }
	} else {
	    free(attrs);
	    attrs = NULL;
	}
    }
    DWORD cflags = CREATE_NO_WINDOW | CREATE_SUSPENDED |
		   (attrs ? EXTENDED_STARTUPINFO_PRESENT : 0);
    /* cb has to match the flag: the extended size only with the flag. */
    si.StartupInfo.cb = attrs ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);

    wchar_t *wcmd = u8_to_wcs(cmd.s);
    wchar_t *wdir = u8_to_wcs(workdir);
    db_free(&cmd);
    BOOL ok = FALSE;
    if (wcmd && wdir) {
	ok = CreateProcessW(NULL, wcmd, NULL, NULL, inherit,
			    cflags | CREATE_BREAKAWAY_FROM_JOB,
			    NULL, wdir, &si.StartupInfo, &pi);
	if (!ok)
	    ok = CreateProcessW(NULL, wcmd, NULL, NULL, inherit, cflags,
				NULL, wdir, &si.StartupInfo, &pi);
    }
    DWORD lasterr = GetLastError();
    if (attrs) { DeleteProcThreadAttributeList(attrs); free(attrs); }
    if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);  /* child has its own */
    if (!wcmd || !wdir) {
	free(wcmd); free(wdir);
	snprintf(err, errlen, "Out of memory starting the model server.");
	return 0;
    }
    free(wcmd); free(wdir);
    SetLastError(lasterr);
    if (!ok) {
	snprintf(err, errlen,
		 "Could not start the model server (Windows error %lu):\r\n  %s",
		 (unsigned long) GetLastError(), CFG.server_exe);
	return 0;
    }

    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_srv_proc = pi.hProcess;
    g_srv_pid  = pi.dwProcessId;
    return 1;
}

static int ai_server_start(char *err, size_t errlen)
{
    ai_srv_lock();
    int ok = ai_server_start_locked(err, errlen);
    ai_srv_unlock();
    return ok;
}

/* The last line of llama-server's output that mentions an error, trimmed
   to fit a status line.  Returns 1 if one was found. */
static int ai_server_log_reason(char *out, size_t outlen)
{
    out[0] = '\0';
    if (!g_srv_log[0]) return 0;
    HANDLE h = CreateFileW(g_srv_log, GENERIC_READ,
			   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[16384];
    DWORD got = 0;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > (LONGLONG) sizeof buf - 1) {
	LARGE_INTEGER off;
	off.QuadPart = size.QuadPart - (LONGLONG) (sizeof buf - 1);
	SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    }
    ReadFile(h, buf, (DWORD) sizeof buf - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = '\0';

    const char *best = NULL;
    size_t bestlen = 0;
    for (char *line = buf; *line; ) {
	char *end = strchr(line, '\n');
	size_t len = end ? (size_t) (end - line) : strlen(line);
	for (size_t i = 0; i + 5 <= len; i++) {
	    if (!strncasecmp(line + i, "error", 5) ||
		(i + 6 <= len && !strncasecmp(line + i, "failed", 6))) {
		best = line; bestlen = len;
		break;
	    }
	}
	if (!end) break;
	line = end + 1;
    }
    if (!best) return 0;
    while (bestlen && (best[bestlen-1] == '\r' || best[bestlen-1] == ' '))
	bestlen--;
    if (bestlen >= outlen) bestlen = outlen - 1;
    memcpy(out, best, bestlen);
    out[bestlen] = '\0';
    return 1;
}

static void ai_server_stop(void)
{
    if (g_srv_proc) {
	TerminateProcess(g_srv_proc, 0);
	CloseHandle(g_srv_proc);
	g_srv_proc = NULL;
	g_srv_pid  = 0;
    }
    if (g_job) { CloseHandle(g_job); g_job = NULL; }
}

/* ------------------------------------------------------------------ */
/* conversation store (main thread only)                               */
/* ------------------------------------------------------------------ */

typedef struct { char *role; char *content; } aimsg;

static aimsg *g_conv  = NULL;
static int    g_nconv = 0, g_cconv = 0;

static void conv_add(const char *role, const char *content)
{
    if (g_nconv == g_cconv) {
	int nc = g_cconv ? g_cconv * 2 : 16;
	aimsg *n = (aimsg *) realloc(g_conv, nc * sizeof(aimsg));
	if (!n) return;
	g_conv = n; g_cconv = nc;
    }
    g_conv[g_nconv].role    = xstrdup(role);
    g_conv[g_nconv].content = xstrdup(content);
    if (g_conv[g_nconv].role && g_conv[g_nconv].content) g_nconv++;
}

static void conv_clear(void)
{
    for (int i = 0; i < g_nconv; i++) {
	free(g_conv[i].role);
	free(g_conv[i].content);
    }
    g_nconv = 0;
}

/* Build the whole /v1/chat/completions body.  Runs on the main thread;
   the worker only ever sees the finished string. */
static char *ai_build_request(const char *question)
{
    char *sys = ai_build_system_prompt(question);
    dynbuf b; db_init(&b);
    char num[96];

    db_add(&b, "{\"messages\":[{\"role\":\"system\",\"content\":\"");
    json_escape(&b, sys ? sys : "");
    db_add(&b, "\"}");
    free(sys);

    int first = g_nconv - CFG.keep_history;
    if (first < 0) first = 0;
    for (int i = first; i < g_nconv; i++) {
	db_add(&b, ",{\"role\":\"");
	json_escape(&b, g_conv[i].role);
	db_add(&b, "\",\"content\":\"");
	json_escape(&b, g_conv[i].content);
	db_add(&b, "\"}");
    }
    db_add(&b, ",{\"role\":\"user\",\"content\":\"");
    json_escape(&b, question);
    db_add(&b, "\"}]");

    snprintf(num, sizeof num, ",\"temperature\":%.3f", CFG.temperature);
    db_add(&b, num);
    snprintf(num, sizeof num, ",\"top_p\":%.3f", CFG.top_p);
    db_add(&b, num);
    snprintf(num, sizeof num, ",\"max_tokens\":%d", CFG.n_predict);
    db_add(&b, num);
    db_add(&b, ",\"stream\":true");
    /* Qwen3-style models expose reasoning through this switch; servers
       that do not know the field ignore it. */
    db_add(&b, CFG.thinking
	   ? ",\"chat_template_kwargs\":{\"enable_thinking\":true}"
	   : ",\"chat_template_kwargs\":{\"enable_thinking\":false}");
    db_add(&b, "}");
    return db_release(&b);
}

/* ================================================================== */
/* worker thread  -- no R API, no GraphApp, no window calls below here */
/* ================================================================== */

#define WM_AI_DATA    (WM_USER + 21)   /* text waiting in g_pending      */
#define WM_AI_STATUS  (WM_USER + 22)   /* lParam: malloc'd char*, we free */
#define WM_AI_DONE    (WM_USER + 23)   /* wParam: 1 ok / 0 failed        */
#define WM_AI_DLDONE  (WM_USER + 24)   /* model download ended, wParam ok */

static HWND   g_msgwin = NULL;
static dynbuf g_pending;                /* guarded by g_cs */
static volatile LONG g_post_pending = 0;
static volatile LONG g_busy = 0;
static HANDLE g_worker = NULL;

static void w_emit(const char *s, size_t n)
{
    if (!n) return;
    ai_lock();
    db_addn(&g_pending, s, n);
    ai_unlock();
    if (InterlockedCompareExchange(&g_post_pending, 1, 0) == 0)
	PostMessage(g_msgwin, WM_AI_DATA, 0, 0);
}

static void w_status(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char *copy = xstrdup(buf);
    if (copy && !PostMessage(g_msgwin, WM_AI_STATUS, 0, (LPARAM) copy))
	free(copy);
}

/* Emit everything in raw[*emitted..] that lies outside <think> blocks,
   holding back any trailing fragment that might be the start of a tag. */
static void w_emit_filtered(dynbuf *raw, size_t *emitted, int *in_think)
{
    static const char OPEN[]  = "<think>";
    static const char CLOSE[] = "</think>";
    const size_t olen = sizeof OPEN - 1, clen = sizeof CLOSE - 1;

    for (;;) {
	if (*emitted >= raw->n) return;
	const char *base = raw->s + *emitted;
	size_t avail = raw->n - *emitted;
	const char *tag = *in_think ? CLOSE : OPEN;
	size_t tlen = *in_think ? clen : olen;

	const char *hit = NULL;
	for (size_t i = 0; i + tlen <= avail; i++)
	    if (base[i] == tag[0] && !memcmp(base + i, tag, tlen)) {
		hit = base + i; break;
	    }

	if (hit) {
	    if (!*in_think) w_emit(base, (size_t) (hit - base));
	    *emitted += (size_t) (hit - base) + tlen;
	    *in_think = !*in_think;
	    continue;
	}
	/* No complete tag.  Keep back tlen-1 bytes in case one is arriving. */
	size_t hold = tlen - 1;
	if (avail <= hold) return;
	size_t safe = avail - hold;
	if (!*in_think) w_emit(base, safe);
	*emitted += safe;
	return;
    }
}

/* Wait for the server to answer /health with 200, or give up.
   Returns 1 when ready, 0 on timeout or cancellation. */
static int w_wait_ready(void)
{
    int waited = 0, announced = 0;
    while (waited < CFG.startup_timeout) {
	if (g_cancel) return 0;
	int st = ai_server_health();
	if (st == 200) return 1;
	if (g_srv_proc &&
	    WaitForSingleObject(g_srv_proc, 0) == WAIT_OBJECT_0) {
	    char why[300];
	    if (ai_server_log_reason(why, sizeof why))
		w_status("The model server stopped while starting: %s", why);
	    else
		w_status("The model server exited unexpectedly. "
			 "Run it once from a command prompt to see why.");
	    return 0;
	}
	if (!announced) {
	    w_status("Loading the model, this takes a while the first time...");
	    announced = 1;
	} else if (waited % 10 == 0)
	    w_status("Loading the model... (%d s)", waited);
	Sleep(1000);
	waited++;
    }
    w_status("The model did not become ready within %d s. "
	     "Increase startup_timeout in etc\\Rai.conf if the machine is slow.",
	     CFG.startup_timeout);
    return 0;
}

static unsigned __stdcall ai_worker(void *arg)
{
    char *request = (char *) arg;
    int ok = 0;
    char err[1024];
    err[0] = '\0';

    if (ai_server_health() != 200) {
	w_status("Starting the local model...");
	if (!ai_server_start(err, sizeof err)) {
	    w_status("%s", err);
	    goto finish;
	}
	if (!w_wait_ready()) goto finish;
    }
    if (g_cancel) goto finish;

    SOCKET s = ai_connect(CFG.host, CFG.port, CFG.request_timeout);
    if (s == INVALID_SOCKET) {
	w_status("Could not connect to the model server on %s:%d.",
		 CFG.host, CFG.port);
	goto finish;
    }
    ai_lock(); g_active_sock = s; ai_unlock();

    httpstream hs;
    hs_init(&hs, s);
    w_status("Thinking...");
    int status = hs_request(&hs, "POST", "/v1/chat/completions",
			    request, strlen(request));

    if (status != 200) {
	dynbuf body; db_init(&body);
	char tmp[2048];
	int n;
	while ((n = hs_body(&hs, tmp, (int) sizeof tmp)) > 0 && body.n < 4000)
	    db_addn(&body, tmp, (size_t) n);
	char *msg = body.s ? json_find_string(body.s, "message") : NULL;
	w_status("The model server refused the request (HTTP %d)%s%s",
		 status, msg ? ": " : ".", msg ? msg : "");
	free(msg);
	db_free(&body);
	goto closeup;
    }

    {
	dynbuf line, raw;
	db_init(&line); db_init(&raw);
	size_t emitted = 0;
	int in_think = 0, sawdata = 0, finished = 0;
	char tmp[4096];
	int n;

	while (!finished && !g_cancel &&
	       (n = hs_body(&hs, tmp, (int) sizeof tmp)) > 0) {
	    for (int i = 0; i < n; i++) {
		char c = tmp[i];
		if (c != '\n') { if (c != '\r') db_addc(&line, c); continue; }
		if (line.n > 6 && !strncmp(line.s, "data: ", 6)) {
		    const char *payload = line.s + 6;
		    if (!strcmp(payload, "[DONE]")) { finished = 1; }
		    else {
			char *piece = json_find_string(payload, "content");
			if (piece) {
			    if (*piece) {
				sawdata = 1;
				db_add(&raw, piece);
				w_emit_filtered(&raw, &emitted, &in_think);
			    }
			    free(piece);
			} else {
			    char *e = json_find_string(payload, "message");
			    if (e) { w_status("Model error: %s", e); free(e); }
			}
		    }
		}
		line.n = 0;
		if (line.s) line.s[0] = '\0';
	    }
	}
	/* Flush whatever was held back in case it was the start of a tag. */
	if (!in_think && emitted < raw.n)
	    w_emit(raw.s + emitted, raw.n - emitted);
	ok = sawdata || finished;
	if (!ok && !g_cancel)
	    w_status("The model returned nothing. Check the server window "
		     "or try a shorter question.");
	db_free(&line);
	db_free(&raw);
    }

closeup:
    ai_lock();
    g_active_sock = INVALID_SOCKET;
    ai_unlock();
    closesocket(s);

finish:
    free(request);
    /* g_busy is cleared by the main thread in ai_finish_turn(), so that
       the whole turn is owned by one thread and Send cannot slip in
       between the last token and the end of the turn. */
    PostMessage(g_msgwin, WM_AI_DONE, (WPARAM) (ok ? 1 : 0), 0);
    return 0;
}


/* ------------------------------------------------------------------ */
/* first-run model download                                            */
/* ------------------------------------------------------------------ */

/* The model is too big for the installer (and for a GitHub release
   asset), so it is fetched the first time the assistant is used.
   WinHTTP rather than raw sockets here: this goes to the internet over
   TLS, and it honours the machine's proxy settings, which a school
   network may require.  The file grows as <model>.part, so an
   interrupted download continues where it stopped, and it only gets its
   real name once its SHA-256 matches the published one. */

static volatile LONG g_downloading = 0;

static long long ai_file_size_w(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return -1;
    return ((long long) a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

/* SHA-256 of a file as 64 lower-case hex digits.  Honours g_cancel. */
static int ai_sha256_file(const wchar_t *path, char out[65])
{
    int ok = 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    unsigned char *buf = NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
			   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    buf = (unsigned char *) malloc(1 << 20);
    if (buf &&
	BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
	BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) >= 0) {
	DWORD got = 0;
	ok = 1;
	for (;;) {
	    if (g_cancel) { ok = 0; break; }
	    if (!ReadFile(f, buf, 1 << 20, &got, NULL)) { ok = 0; break; }
	    if (got == 0) break;
	    if (BCryptHashData(hh, buf, got, 0) < 0) { ok = 0; break; }
	}
	unsigned char dig[32];
	if (ok && BCryptFinishHash(hh, dig, sizeof dig, 0) >= 0) {
	    for (int i = 0; i < 32; i++)
		snprintf(out + 2 * i, 3, "%02x", dig[i]);
	} else
	    ok = 0;
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(buf);
    CloseHandle(f);
    return ok;
}

/* Fetch url into dest (UTF-8), resuming dest.part if it exists.  When
   sha_hex is non-empty the result must match it.  expect is the size in
   bytes, or 0 if unknown.  Returns 1 when dest is complete and checked;
   otherwise 0 with a sentence for the status line in err.  Runs on a
   worker thread: w_status only, no R API, no windows. */
static int ai_download_file(const char *url, const char *dest,
			    const char *sha_hex, long long expect,
			    char *err, size_t errlen)
{
    int ok = 0;
    wchar_t *wurl = u8_to_wcs(url), *wdest = u8_to_wcs(dest);
    wchar_t *wpart = NULL, *host = NULL;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    HANDLE out = INVALID_HANDLE_VALUE;
    unsigned char *buf = NULL;
    long long have = 0, total = expect;
    DWORD status = 0;
    const double GB = 1e9;

    err[0] = '\0';
    if (!wurl || !wdest) { snprintf(err, errlen, "Out of memory."); goto done; }
    wpart = (wchar_t *) malloc((wcslen(wdest) + 6) * sizeof(wchar_t));
    if (!wpart) { snprintf(err, errlen, "Out of memory."); goto done; }
    wcscpy(wpart, wdest);
    wcscat(wpart, L".part");

    have = ai_file_size_w(wpart);
    if (have < 0) have = 0;
    if (expect > 0 && have > expect) { DeleteFileW(wpart); have = 0; }

    if (!(expect > 0 && have == expect)) {
	/* Room for what is still to come, plus a margin. */
	wchar_t dir[MAX_PATH];
	wcsncpy(dir, wdest, MAX_PATH - 1);
	dir[MAX_PATH - 1] = L'\0';
	wchar_t *sl = wcsrchr(dir, L'\\');
	if (sl) *sl = L'\0';
	ULARGE_INTEGER freeb;
	if (expect > 0 && GetDiskFreeSpaceExW(dir, &freeb, NULL, NULL) &&
	    (long long) freeb.QuadPart < expect - have + (64LL << 20)) {
	    snprintf(err, errlen,
		     "Not enough free space for the AI model: it needs %.1f GB "
		     "more, the drive has %.1f GB free.",
		     (expect - have) / GB, (double) freeb.QuadPart / GB);
	    goto done;
	}

	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof uc);
	uc.dwStructSize = sizeof uc;
	uc.dwSchemeLength = (DWORD) -1;
	uc.dwHostNameLength = (DWORD) -1;
	uc.dwUrlPathLength = (DWORD) -1;
	uc.dwExtraInfoLength = (DWORD) -1;
	if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !uc.dwHostNameLength) {
	    snprintf(err, errlen, "model_url in Rai.conf is not a valid URL.");
	    goto done;
	}
	host = (wchar_t *) malloc((uc.dwHostNameLength + 1) * sizeof(wchar_t));
	if (!host) { snprintf(err, errlen, "Out of memory."); goto done; }
	wcsncpy(host, uc.lpszHostName, uc.dwHostNameLength);
	host[uc.dwHostNameLength] = L'\0';

	ses = WinHttpOpen(L"RGui-AI/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses)
	    ses = WinHttpOpen(L"RGui-AI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (ses) WinHttpSetTimeouts(ses, 30000, 30000, 30000, 60000);
	if (ses) con = WinHttpConnect(ses, host, uc.nPort, 0);
	/* lpszUrlPath runs on to the end of the URL, query included. */
	if (con)
	    req = WinHttpOpenRequest(con, L"GET", uc.lpszUrlPath, NULL,
				     WINHTTP_NO_REFERER,
				     WINHTTP_DEFAULT_ACCEPT_TYPES,
				     uc.nScheme == INTERNET_SCHEME_HTTPS ?
				     WINHTTP_FLAG_SECURE : 0);
	if (!req) {
	    snprintf(err, errlen, "Could not prepare the download (Windows error %lu).",
		     (unsigned long) GetLastError());
	    goto done;
	}
	if (have > 0) {
	    char range[64];
	    snprintf(range, sizeof range, "Range: bytes=%lld-", have);
	    wchar_t *wr = u8_to_wcs(range);
	    if (wr) {
		WinHttpAddRequestHeaders(req, wr, (DWORD) -1, WINHTTP_ADDREQ_FLAG_ADD);
		free(wr);
	    }
	}
	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(req, NULL)) {
	    snprintf(err, errlen,
		     "Could not reach the download server (Windows error %lu). "
		     "Check the internet connection and open the assistant again.",
		     (unsigned long) GetLastError());
	    goto done;
	}
	DWORD sl2 = sizeof status;
	WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl2,
			    WINHTTP_NO_HEADER_INDEX);
	if (status == 200)
	    have = 0;                    /* no resume on offer: start over */
	else if (status == 416) {
	    DeleteFileW(wpart);          /* our partial file makes no sense */
	    snprintf(err, errlen, "The download could not be resumed and was "
		     "reset. Open the assistant again to start it afresh.");
	    goto done;
	} else if (status != 206) {
	    snprintf(err, errlen, "The download server answered HTTP %lu.",
		     (unsigned long) status);
	    goto done;
	}
	wchar_t cl[32];
	DWORD cls = sizeof cl;
	if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
				WINHTTP_HEADER_NAME_BY_INDEX, cl, &cls,
				WINHTTP_NO_HEADER_INDEX)) {
	    long long n = _wcstoi64(cl, NULL, 10);
	    if (n > 0) total = have + n;
	}

	out = CreateFileW(wpart, GENERIC_WRITE, FILE_SHARE_READ, NULL,
			  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (out == INVALID_HANDLE_VALUE) {
	    snprintf(err, errlen, "Cannot write the AI model file (Windows error "
		     "%lu). Is the folder read-only?", (unsigned long) GetLastError());
	    goto done;
	}
	LARGE_INTEGER pos;
	pos.QuadPart = have;
	SetFilePointerEx(out, pos, NULL, FILE_BEGIN);
	SetEndOfFile(out);

	buf = (unsigned char *) malloc(256 * 1024);
	if (!buf) { snprintf(err, errlen, "Out of memory."); goto done; }
	DWORD last = GetTickCount() - 2000;
	for (;;) {
	    if (g_cancel) {
		snprintf(err, errlen, "Download paused at %.2f of %.2f GB. Open "
			 "the assistant again to continue.", have / GB, total / GB);
		goto done;
	    }
	    DWORD got = 0;
	    if (!WinHttpReadData(req, buf, 256 * 1024, &got)) {
		snprintf(err, errlen, "The download was interrupted (Windows error "
			 "%lu). Open the assistant again to continue it.",
			 (unsigned long) GetLastError());
		goto done;
	    }
	    if (got == 0) break;
	    DWORD wr = 0;
	    if (!WriteFile(out, buf, got, &wr, NULL) || wr != got) {
		snprintf(err, errlen, "Writing the AI model failed (Windows error "
			 "%lu). Is the drive full?", (unsigned long) GetLastError());
		goto done;
	    }
	    have += got;
	    if (GetTickCount() - last >= 1000) {
		last = GetTickCount();
		if (total > 0)
		    w_status("Downloading the AI model: %.2f of %.2f GB (%d%%). "
			     "R stays usable; Stop pauses.", have / GB, total / GB,
			     (int) (100.0 * (double) have / (double) total));
		else
		    w_status("Downloading the AI model: %.2f GB. Stop pauses.",
			     have / GB);
	    }
	}
	CloseHandle(out);
	out = INVALID_HANDLE_VALUE;
	if (expect > 0 && have != expect) {
	    snprintf(err, errlen, "The download ended early (%.2f of %.2f GB). "
		     "Open the assistant again to continue it.", have / GB, expect / GB);
	    goto done;
	}
    }

    if (sha_hex && *sha_hex) {
	w_status("Checking the downloaded model...");
	char sum[65];
	if (!ai_sha256_file(wpart, sum)) {
	    snprintf(err, errlen, g_cancel ? "The check was cancelled."
		     : "Could not read the downloaded model to check it.");
	    goto done;
	}
	if (strcasecmp(sum, sha_hex) != 0) {
	    DeleteFileW(wpart);
	    snprintf(err, errlen, "The download was damaged (checksum mismatch) "
		     "and has been deleted. Open the assistant again to retry.");
	    goto done;
	}
    }
    if (!MoveFileExW(wpart, wdest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
	snprintf(err, errlen, "Could not put the downloaded model in place "
		 "(Windows error %lu).", (unsigned long) GetLastError());
	goto done;
    }
    ok = 1;

done:
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(buf);
    free(host);
    free(wpart);
    free(wurl);
    free(wdest);
    return ok;
}

static unsigned __stdcall ai_download_worker(void *unused)
{
    char err[512];
    int ok = ai_download_file(CFG.model_url, CFG.model, CFG.model_sha256,
			      CFG.model_bytes, err, sizeof err);
    if (ok)
	w_status("The AI model is downloaded.");
    else
	w_status("%s", err);
    PostMessage(g_msgwin, WM_AI_DLDONE, (WPARAM) ok, 0);
    return 0;
}

/* ================================================================== */
/* user interface -- main (R) thread only                              */
/* ================================================================== */

static window  g_panel   = NULL;
static control g_hist    = NULL, g_input = NULL, g_status = NULL;
static control g_bsend   = NULL, g_bstop = NULL, g_bcopy = NULL;
static control g_beditor = NULL, g_bclear = NULL;
static HWND    g_hhist   = NULL, g_hinput = NULL;
static dynbuf  g_reply;                  /* assistant text of this turn */
static int     g_reply_open = 0;

/* --- Win32 EDIT helpers; UTF-8 in and out whatever the window type -- */

static void edit_send_text(HWND h, UINT msg, WPARAM wp, const char *u8)
{
    wchar_t *w = u8_to_wcs(u8);
    if (!w) return;
    if (IsWindowUnicode(h))
	SendMessageW(h, msg, wp, (LPARAM) w);
    else {
	int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
	char *a = n > 0 ? (char *) malloc((size_t) n) : NULL;
	if (a && WideCharToMultiByte(CP_ACP, 0, w, -1, a, n, NULL, NULL) > 0)
	    SendMessageA(h, msg, wp, (LPARAM) a);
	free(a);
    }
    free(w);
}

static void edit_append_u8(HWND h, const char *u8)
{
    if (!h || !u8 || !*u8) return;
    dynbuf t; db_init(&t);
    for (const char *p = u8; *p; p++) {
	if (*p == '\r') continue;
	if (*p == '\n') db_add(&t, "\r\n"); else db_addc(&t, *p);
    }
    LONG style = GetWindowLong(h, GWL_STYLE);
    int ro = (style & ES_READONLY) != 0;

    /* EM_REPLACESEL inserts at the caret, so the caret has to be put at
       the end first.  EM_SETSEL(-1, -1) does not do that -- it only
       drops the selection and leaves the caret where the user last
       clicked, which spliced streamed text into the middle of the
       transcript.  An ANSI control reports its length in bytes, which
       can only overshoot the end, and the control clamps that. */
    DWORD sel0 = 0, sel1 = 0;
    SendMessage(h, EM_GETSEL, (WPARAM) &sel0, (LPARAM) &sel1);
    int user_selection = sel0 != sel1;
    LRESULT end = IsWindowUnicode(h) ? GetWindowTextLengthW(h)
				     : GetWindowTextLengthA(h);

    if (ro) SendMessage(h, EM_SETREADONLY, FALSE, 0);
    SendMessage(h, EM_SETSEL, (WPARAM) end, (LPARAM) end);
    edit_send_text(h, EM_REPLACESEL, FALSE, t.s ? t.s : "");
    if (ro) SendMessage(h, EM_SETREADONLY, TRUE, 0);

    if (user_selection)
	/* The user is selecting text, perhaps to copy it: leave that
	   alone and do not scroll away from it. */
	SendMessage(h, EM_SETSEL, (WPARAM) sel0, (LPARAM) sel1);
    else
	SendMessage(h, EM_SCROLLCARET, 0, 0);
    db_free(&t);
}

/* Returns malloc'd UTF-8 with CRLF folded to LF, or NULL. */
static char *edit_get_u8(HWND h)
{
    if (!h) return NULL;
    int len = GetWindowTextLengthW(h);
    wchar_t *w = (wchar_t *) malloc(((size_t) len + 2) * sizeof(wchar_t));
    if (!w) return NULL;
    GetWindowTextW(h, w, len + 1);
    char *u8 = wcs_to_u8(w);
    free(w);
    if (!u8) return NULL;
    char *d = u8;
    for (char *s = u8; *s; s++) if (*s != '\r') *d++ = *s;
    *d = '\0';
    return u8;
}

static void edit_clear(HWND h)
{
    if (!h) return;
    LONG style = GetWindowLong(h, GWL_STYLE);
    int ro = (style & ES_READONLY) != 0;
    if (ro) SendMessage(h, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(h, L"");
    if (ro) SendMessage(h, EM_SETREADONLY, TRUE, 0);
}

static void ai_set_status(const char *s)
{
    if (g_status) settext(g_status, s);
}

/* --- clipboard and code extraction -------------------------------- */

static int ai_clipboard_put(const char *u8)
{
    wchar_t *w = u8_to_wcs(u8);
    if (!w) return 0;
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) { free(w); return 0; }
    void *p = GlobalLock(h);
    if (!p) { GlobalFree(h); free(w); return 0; }
    memcpy(p, w, bytes);
    GlobalUnlock(h);
    free(w);
    /* Own the clipboard with a real window: with a NULL owner,
       EmptyClipboard leaves no owner and SetClipboardData may fail.  And
       another program (a clipboard manager, say) may hold the clipboard
       open for a moment, so try for up to half a second. */
    HWND owner = g_panel ? (HWND) getHandle(g_panel) : NULL;
    int opened = 0;
    for (int i = 0; i < 20 && !opened; i++) {
	opened = OpenClipboard(owner);
	if (!opened) Sleep(25);
    }
    if (!opened) { GlobalFree(h); return 0; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
	CloseClipboard(); GlobalFree(h); return 0;
    }
    CloseClipboard();
    return 1;
}

/* Concatenate every fenced code block in text.  Falls back to the whole
   text when it contains no fences.  Returns malloc'd UTF-8, or NULL. */
static char *ai_extract_code(const char *text)
{
    if (!text) return NULL;
    dynbuf out; db_init(&out);
    const char *p = text;
    int found = 0;
    while ((p = strstr(p, "```")) != NULL) {
	p += 3;
	while (*p && *p != '\n') p++;     /* skip the language tag */
	if (*p == '\n') p++;
	const char *end = strstr(p, "```");
	if (!end) end = p + strlen(p);
	if (found) db_add(&out, "\n");
	db_addn(&out, p, (size_t) (end - p));
	found = 1;
	p = (*end) ? end + 3 : end;
    }
    if (!found) { db_free(&out); return xstrdup(text); }
    /* drop a trailing newline so pasting does not execute a blank line */
    while (out.n && (out.s[out.n-1] == '\n' || out.s[out.n-1] == ' '))
	out.s[--out.n] = '\0';
    return db_release(&out);
}

/* The most recent assistant turn, or NULL. */
static const char *ai_last_reply(void)
{
    for (int i = g_nconv - 1; i >= 0; i--)
	if (!strcmp(g_conv[i].role, "assistant")) return g_conv[i].content;
    return NULL;
}

/* --- generation control ------------------------------------------- */

static void ai_set_busy(int busy)
{
    if (g_bsend) { if (busy) disable(g_bsend); else enable(g_bsend); }
    if (g_bstop) { if (busy) enable(g_bstop);  else disable(g_bstop); }
}

static void ai_stop_generation(void)
{
    InterlockedExchange(&g_cancel, 1);
    ai_lock();
    if (g_active_sock != INVALID_SOCKET) shutdown(g_active_sock, SD_BOTH);
    ai_unlock();
}

static void ai_finish_turn(int ok)
{
    if (g_reply_open) {
	if (g_reply.n) conv_add("assistant", g_reply.s);
	edit_append_u8(g_hhist, "\n\n");
	db_free(&g_reply);
	g_reply_open = 0;
    }
    ai_set_busy(0);
    if (g_cancel)   ai_set_status("Stopped.");
    else if (ok)    ai_set_status("Ready.");
    /* on failure the worker has already put the reason in the status line */
    InterlockedExchange(&g_cancel, 0);
    InterlockedExchange(&g_busy, 0);
}

static void ai_drain_pending(void)
{
    InterlockedExchange(&g_post_pending, 0);
    ai_lock();
    char *text = g_pending.n ? db_release(&g_pending) : NULL;
    ai_unlock();
    if (!text) return;
    edit_append_u8(g_hhist, text);
    db_add(&g_reply, text);
    free(text);
}

/* Window procedure of the message-only window.  Runs on the R main
   thread, from inside doevent(), i.e. inside R_ProcessEvents(). */
static unsigned __stdcall ai_warmup(void *unused);   /* below */

static void ai_start_warmup(void)
{
    unsigned tid;
    HANDLE th = (HANDLE) _beginthreadex(NULL, 0, ai_warmup, NULL, 0, &tid);
    if (th) CloseHandle(th);
}

static LRESULT CALLBACK ai_msgproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_AI_DATA:
	ai_drain_pending();
	return 0;
    case WM_AI_STATUS: {
	char *s = (char *) lp;
	if (s) { ai_set_status(s); free(s); }
	return 0;
    }
    case WM_AI_DONE:
	ai_drain_pending();
	ai_finish_turn((int) wp);
	return 0;
    case WM_AI_DLDONE:
	InterlockedExchange(&g_downloading, 0);
	InterlockedExchange(&g_cancel, 0);
	InterlockedExchange(&g_busy, 0);
	ai_set_busy(0);
	if (wp && CFG.autostart) ai_start_warmup();
	return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static int ai_make_msgwin(void)
{
    if (g_msgwin) return 1;
    static const char CLS[] = "RGuiAIChatMsgWindow";
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = ai_msgproc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = CLS;
    RegisterClassA(&wc);      /* a second call just fails harmlessly */
    g_msgwin = CreateWindowA(CLS, "", 0, 0, 0, 0, 0,
			     HWND_MESSAGE, NULL, wc.hInstance, NULL);
    return g_msgwin != NULL;
}

/* --- buttons ------------------------------------------------------- */

extern font consolefn;                       /* from console.c */

/* editor.c.  enc is a cetype_t; 1 is CE_UTF8.  Declared here rather than
   including Rinternals.h, which would drag the whole R API into a file
   that deliberately does not use it. */
int Rgui_Edit(const char *filename, int enc, const char *title, int modal);
#define AI_CE_UTF8 1

static int ai_model_missing(void)
{
    return CFG.model[0] && !ai_file_exists(CFG.model);
}

/* If the model is missing and there is somewhere to fetch it from, ask
   whether to download it now and start the download if so.  Returns 1
   when the model is missing (asked or not), 0 when there is nothing to
   do here. */
static int ai_offer_download(void)
{
    if (!ai_model_missing() || !CFG.model_url[0]) return 0;
    if (g_downloading) {
	ai_set_status("The AI model is still downloading. Stop pauses it.");
	return 1;
    }
    if (g_busy) return 1;

    char part[MAX_PATH + 8], q[800];
    snprintf(part, sizeof part, "%s.part", CFG.model);
    wchar_t *wpart = u8_to_wcs(part);
    long long have = wpart ? ai_file_size_w(wpart) : -1;
    free(wpart);
    if (have > 0 && CFG.model_bytes > 0)
	snprintf(q, sizeof q,
		 "Part of the AI model is already downloaded (%.1f of %.1f GB).\n\n"
		 "Continue the download now?", have / 1e9, CFG.model_bytes / 1e9);
    else if (CFG.model_bytes > 0)
	snprintf(q, sizeof q,
		 "The AI model is not on this computer yet.\n\n"
		 "Download it now? It is %.1f GB and is needed only once. "
		 "The download runs in the background, R stays usable, and an "
		 "interrupted download continues where it stopped.",
		 CFG.model_bytes / 1e9);
    else
	snprintf(q, sizeof q,
		 "The AI model is not on this computer yet.\n\nDownload it now?");
    if (askyesno(q) != YES) {
	ai_set_status("The AI model is not downloaded. Send a question to be asked again.");
	return 1;
    }

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return 1;
    InterlockedExchange(&g_cancel, 0);
    InterlockedExchange(&g_downloading, 1);
    ai_set_busy(1);
    ai_set_status("Starting the download...");
    unsigned tid;
    HANDLE th = (HANDLE) _beginthreadex(NULL, 0, ai_download_worker, NULL, 0, &tid);
    if (!th) {
	InterlockedExchange(&g_downloading, 0);
	InterlockedExchange(&g_busy, 0);
	ai_set_busy(0);
	ai_set_status("Could not start the download.");
	return 1;
    }
    if (g_worker) CloseHandle(g_worker);
    g_worker = th;
    return 1;
}

static void ai_do_send(control c)
{
    if (!g_hinput) return;
    /* No model yet: offer the download; the question stays in the box. */
    if (ai_offer_download()) return;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
	ai_set_status("Still answering the previous question.");
	return;
    }
    char *q = edit_get_u8(g_hinput);
    if (q) { ai_trim(q); }
    if (!q || !*q) {
	free(q);
	InterlockedExchange(&g_busy, 0);
	return;
    }

    char *request = ai_build_request(q);
    if (!request) {
	free(q);
	InterlockedExchange(&g_busy, 0);
	ai_set_status("Out of memory building the request.");
	return;
    }
    conv_add("user", q);

    edit_append_u8(g_hhist, "You:\n");
    edit_append_u8(g_hhist, q);
    edit_append_u8(g_hhist, "\n\nR assistant:\n");
    free(q);
    edit_clear(g_hinput);

    db_free(&g_reply);
    db_init(&g_reply);
    g_reply_open = 1;
    InterlockedExchange(&g_cancel, 0);
    ai_set_busy(1);
    ai_set_status("Contacting the local model...");

    unsigned tid;
    HANDLE th = (HANDLE) _beginthreadex(NULL, 0, ai_worker, request, 0, &tid);
    if (!th) {
	free(request);
	InterlockedExchange(&g_busy, 0);
	g_reply_open = 0;
	ai_set_busy(0);
	ai_set_status("Could not start the background thread.");
	return;
    }
    if (g_worker) CloseHandle(g_worker);
    g_worker = th;
}

static void ai_do_stop(control c)
{
    if (!g_busy) return;
    ai_set_status(g_downloading ? "Pausing the download..." : "Stopping...");
    ai_stop_generation();
}

static void ai_do_copy(control c)
{
    const char *src = (g_reply_open && g_reply.n) ? g_reply.s : ai_last_reply();
    if (!src) { ai_set_status("Nothing to copy yet."); return; }
    char *code = ai_extract_code(src);
    if (code && *code && ai_clipboard_put(code))
	ai_set_status("Code copied to the clipboard.");
    else
	ai_set_status("Nothing to copy yet.");
    free(code);
}

/* Open the code of the last answer in an ordinary RGui script editor
   window.  Nothing is executed: the user reads it, edits it and runs it
   with Ctrl+R in the usual way. */
static void ai_do_editor(control c)
{
    static int seq = 0;
    const char *src = (g_reply_open && g_reply.n) ? g_reply.s : ai_last_reply();
    if (!src) { ai_set_status("Nothing to open yet."); return; }
    char *code = ai_extract_code(src);
    if (!code || !*code) {
	free(code);
	ai_set_status("The last answer contained no code.");
	return;
    }

    char dir[MAX_PATH], path[MAX_PATH + 64], title[64];
    if (!GetTempPathA(MAX_PATH, dir)) strcpy(dir, ".\\");
    seq++;
    snprintf(path, sizeof path, "%sRGui-ai-%d.R", dir, seq);

    FILE *f = fopen(path, "wb");
    if (!f) {
	free(code);
	ai_set_status("Could not write the temporary script file.");
	return;
    }
    fwrite(code, 1, strlen(code), f);
    fputc('\n', f);
    fclose(f);
    free(code);

    snprintf(title, sizeof title, "AI answer %d", seq);
    if (Rgui_Edit(path, AI_CE_UTF8, title, 0) != 0)
	ai_set_status("Could not open a script editor window.");
    else
	ai_set_status("Code opened in a script editor. Run it with Ctrl+R.");
}

static void ai_do_clear(control c)
{
    if (g_busy) { ai_set_status("Stop the answer first."); return; }
    conv_clear();
    db_free(&g_reply);
    g_reply_open = 0;
    edit_clear(g_hhist);
    ai_set_status("New conversation.");
}

/* --- layout and construction --------------------------------------- */

#define AI_PAD       8
#define AI_STATUS_H 18
#define AI_BTN_H    26
#define AI_BTN_W    96
#define AI_INPUT_H  84

static void ai_layout(window w, rect r)
{
    if (r.width < 260) r.width = 260;
    if (r.height < 240) r.height = 240;

    int x = AI_PAD, ww = r.width - 2 * AI_PAD;
    int y = AI_PAD;

    if (g_status) resize(g_status, rect(x, y, ww, AI_STATUS_H));
    y += AI_STATUS_H + 4;

    int bottom = r.height - AI_PAD - AI_BTN_H;
    int inputy = bottom - 4 - AI_INPUT_H;
    int histh  = inputy - 4 - y;
    if (histh < 60) histh = 60;

    if (g_hist)  resize(g_hist,  rect(x, y, ww, histh));
    if (g_input) resize(g_input, rect(x, inputy, ww, AI_INPUT_H));

    int bx = x;
    control bs[5] = { g_bsend, g_bstop, g_bcopy, g_beditor, g_bclear };
    for (int i = 0; i < 5; i++) {
	if (!bs[i]) continue;
	resize(bs[i], rect(bx, bottom, AI_BTN_W, AI_BTN_H));
	bx += AI_BTN_W + 4;
    }
}

static void ai_resize(window w, rect r)
{
    ai_layout(w, r);
}

static void ai_keydown(control c, int ch)
{
    /* Ctrl+Enter sends.  Plain Enter inserts a newline, so multi-line
       questions and pasted code still work. */
    if (ch == 10 ||
	(ch == 13 && (GetKeyState(VK_CONTROL) & 0x8000)))
	ai_do_send(c);
}

static void ai_menu_noop(control m) { }

static void ai_hide_panel(control c)
{
    if (g_panel) hide(g_panel);
}

static void ai_menu_close(control m) { ai_hide_panel(NULL); }

static const char AI_WELCOME[] =
    "Local R assistant.\r\n"
    "Type a question below and press Send (or Ctrl+Enter).\r\n"
    "Copy code puts the code from the last answer on the clipboard; "
    "To editor opens it in a script window. Nothing is run for you.\r\n"
    "Nothing leaves this computer.\r\n"
    "\r\n";

static int ai_create(void)
{
    int w = 620, h = 560, x = 40, y = 40;

#ifdef USE_MDI
    if (ismdi()) {
	RECT *pR = RgetMDIsize();
	if (pR->right  > w + 60) x = pR->right - w - 20;
	if (pR->bottom > h + 60) y = 20;
	else if (pR->bottom > 320) h = pR->bottom - 40;
    } else
#endif
    {
	x = devicewidth(NULL) - w - 60;
	if (x < 20) x = 20;
	y = 40;
    }

    long flags = StandardWindow | Menubar;
#ifdef USE_MDI
    if (ismdi()) flags |= Document;
#endif
    g_panel = newwindow("R AI assistant", rect(x, y, w, h), flags);
    if (!g_panel) return 0;

    addto(g_panel);
    gsetcursor(g_panel, ArrowCursor);

    g_status = newlabel("Ready.", rect(0, 0, 10, AI_STATUS_H), AlignLeft);
    g_hist   = newtextarea("", rect(0, 0, 10, 10));
    g_input  = newtextbox("",  rect(0, 0, 10, 10));
    g_bsend    = newbutton("Send",       rect(0, 0, AI_BTN_W, AI_BTN_H), ai_do_send);
    g_bstop    = newbutton("Stop",       rect(0, 0, AI_BTN_W, AI_BTN_H), ai_do_stop);
    g_bcopy    = newbutton("Copy code",  rect(0, 0, AI_BTN_W, AI_BTN_H), ai_do_copy);
    g_beditor  = newbutton("To editor",  rect(0, 0, AI_BTN_W, AI_BTN_H), ai_do_editor);
    g_bclear   = newbutton("New chat",   rect(0, 0, AI_BTN_W, AI_BTN_H), ai_do_clear);

    if (!g_status || !g_hist || !g_input || !g_bsend || !g_bstop ||
	!g_bcopy || !g_beditor || !g_bclear) {
	del(g_panel);
	g_panel = NULL;
	return 0;
    }

    g_hhist  = (HWND) getHandle(g_hist);
    g_hinput = (HWND) getHandle(g_input);

    font f = consolefn ? consolefn : FixedFont;
    settextfont(g_hist, f);
    settextfont(g_input, f);

    /* A multiline EDIT defaults to a 32 kB limit; 0 removes it. */
    if (g_hhist) {
	SendMessage(g_hhist, EM_SETLIMITTEXT, 0, 0);
	SendMessage(g_hhist, EM_SETREADONLY, TRUE, 0);
    }
    if (g_hinput) SendMessage(g_hinput, EM_SETLIMITTEXT, 0, 0);

    setkeydown(g_input, ai_keydown);

    addto(g_panel);
    newmenubar(ai_menu_noop);
    newmenu(G_("File"));
    newmenuitem(G_("Hide assistant"), 0, ai_menu_close);
    newmenu(G_("Edit"));
    newmenuitem(G_("Copy code"), 0, ai_do_copy);
    newmenuitem(G_("Open code in script editor"), 0, ai_do_editor);
    newmenuitem("-", 0, NULL);
    newmenuitem(G_("New chat"), 0, ai_do_clear);
#ifdef USE_MDI
    if (ismdi()) newmdimenu();
#endif

    setresize(g_panel, ai_resize);
    /* Closing hides: the transcript has to survive so that reopening
       restores the conversation. */
    setclose(g_panel, ai_hide_panel);

    ai_layout(g_panel, getrect(g_panel));
    edit_append_u8(g_hhist, AI_WELCOME);
    ai_set_busy(0);
    return 1;
}

/* Warm the model up in the background so the first question is not the
   one that pays for loading it. */
static unsigned __stdcall ai_warmup(void *unused)
{
    char err[1024];
    err[0] = '\0';
    if (ai_server_health() == 200) {
	w_status("Ready.");
	return 0;
    }
    w_status("Starting the local model...");
    if (!ai_server_start(err, sizeof err)) { w_status("%s", err); return 0; }
    if (w_wait_ready()) w_status("Ready.");
    return 0;
}

void aichat_toggle(void)
{
    ai_load_config();
    if (!CFG.enabled) {
	R_ShowMessage("The AI assistant is disabled in R_HOME\\etc\\Rai.conf.");
	return;
    }
    if (!g_cs_up) {
	InitializeCriticalSection(&g_cs);
	g_cs_up = 1;
	db_init(&g_pending);
    }
    if (!g_srv_cs_up) {
	InitializeCriticalSection(&g_srv_cs);
	g_srv_cs_up = 1;
    }
    if (!ai_make_msgwin()) {
	R_ShowMessage("Could not create the assistant's message window.");
	return;
    }

    if (!g_panel) {
	if (!ai_create()) {
	    R_ShowMessage("Could not create the assistant window.");
	    return;
	}
	show(g_panel);
	if (g_input) { addto(g_panel); show(g_input); }
	if (!ai_offer_download() && CFG.autostart)
	    ai_start_warmup();
	return;
    }

    HWND hw = (HWND) getHandle(g_panel);
    if (hw && IsWindowVisible(hw))
	hide(g_panel);
    else {
	show(g_panel);
	ai_layout(g_panel, getrect(g_panel));
    }
}

void aichat_shutdown(void)
{
    if (g_busy) ai_stop_generation();
    if (g_worker) {
	WaitForSingleObject(g_worker, 3000);
	CloseHandle(g_worker);
	g_worker = NULL;
    }
    ai_server_stop();
    if (g_wsa_up) { WSACleanup(); g_wsa_up = 0; }
}
