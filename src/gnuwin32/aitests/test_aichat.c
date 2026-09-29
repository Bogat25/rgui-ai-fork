/* Harness for the non-GUI half of aichat.c.
 *
 * aichat.c is included verbatim so that its static functions are
 * reachable.  Everything GraphApp or R provides is stubbed out; the
 * stubs are enough because the code under test never draws anything.
 */

#define WIN32_LEAN_AND_MEAN 1
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "graphapp/ga.h"
#include "graphapp/graphapp.h"

#include <stdio.h>
#define GA_EXTERN
#include "graphapp/internal.h"
#include "stubs.h"

#include "aichat.c"

/* Stubs that need types from rui.h / console.h, which aichat.c includes
   and which have no include guards: hence after it. */
window RConsole = NULL;
int RguiMDI = 0;
/* Declared by the stand-in graphapp/stdimg.h next to this file. */
image open_image, copy_image, paste_image, stop_image, console_image;
control GA_newtoolbar(int h) { (void)h; return STUB; }
button  GA_newtoolbutton(image i, rect r, actionfn f) { (void)i; (void)r; (void)f; return STUB; }
int     GA_addtooltip(control c, const char *t) { (void)c; (void)t; return 1; }
void menueditornew(control m) { (void)m; }
void menueditoropen(control m) { (void)m; }
void menuconfig(control m) { (void)m; }
int  RguiPackageMenu(PkgMenuItems p) { (void)p; return 0; }
void pkgmenuact(PkgMenuItems p) { (void)p; }
int  RguiCommonHelp(menu m, HelpMenuItems h) { (void)m; (void)h; return 0; }
void helpmenuact(HelpMenuItems h) { (void)h; }
char *consoletailtext(console c, int n) { (void)c; (void)n; return NULL; }
char *editor_top_text(char *t, size_t n) { (void)t; (void)n; return NULL; }
int   editor_insert_top(const char *x, char *t, size_t n) { (void)x; (void)t; (void)n; return 0; }
textbox GA_newrichtextarea(const char *t, rect r) { (void)t; (void)r; return STUB; }
int pointsize = 10;

/* ---- assertions -------------------------------------------------- */
static int failures = 0;

static void t_ok(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static void t_str(const char *what, const char *got, const char *want)
{
    int ok = got && want && !strcmp(got, want);
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        failures++;
        printf("     want [%s]\n     got  [%s]\n", want ? want : "(null)",
               got ? got : "(null)");
    }
}

/* ---- unit tests -------------------------------------------------- */

static void test_json(void)
{
    char *v;

    v = json_find_string("{\"a\":\"one\",\"b\":\"two\"}", "b");
    t_str("json_find_string: plain key", v, "two"); free(v);

    /* the key name also appears as a value; must not be matched there */
    v = json_find_string("{\"x\":\"content\",\"content\":\"real\"}", "content");
    t_str("json_find_string: key name inside a value", v, "real"); free(v);

    v = json_find_string("{\"content\":\"a\\nb\\t\\\"q\\\"\\\\z\"}", "content");
    t_str("json_find_string: escapes", v, "a\nb\t\"q\"\\z"); free(v);

    /* é and a surrogate pair for U+1F600 */
    v = json_find_string("{\"content\":\"\\u00e9\\ud83d\\ude00\"}", "content");
    t_str("json_find_string: \\u and surrogate pair",
              v, "\xc3\xa9\xf0\x9f\x98\x80"); free(v);

    /* content: null is not a string, so nothing is produced */
    v = json_find_string("{\"delta\":{\"role\":\"assistant\",\"content\":null}}",
                         "content");
    t_ok("json_find_string: null value yields nothing", v == NULL); free(v);

    dynbuf b; db_init(&b);
    json_escape(&b, "a\"b\\c\nd\te");
    t_str("json_escape", b.s, "a\\\"b\\\\c\\nd\\te");
    db_free(&b);
}

static void test_think_filter(void)
{
    dynbuf raw; db_init(&raw);
    size_t emitted = 0;
    int in_think = 0;

    /* Feed it one character at a time: the worst case for tag splitting. */
    const char *src = "A<think>hidden</think>B<think>more</think>C";
    for (const char *p = src; *p; p++) {
        db_addn(&raw, p, 1);
        w_emit_filtered(&raw, &emitted, &in_think);
    }
    if (!in_think && emitted < raw.n)
        w_emit(raw.s + emitted, raw.n - emitted);

    ai_lock();
    char *got = g_pending.n ? db_release(&g_pending) : NULL;
    ai_unlock();
    t_str("think filter: tags split across every boundary", got, "ABC");
    free(got);
    db_free(&raw);
}

static void test_extract_code(void)
{
    char *c = ai_extract_code("blah\n```r\nx <- 1\n```\ntext\n```\ny <- 2\n```\n");
    t_str("ai_extract_code: two fenced blocks", c, "x <- 1\n\ny <- 2");
    free(c);

    c = ai_extract_code("no fences here");
    t_ok("ai_extract_code: no fenced block gives nothing", c == NULL);
    free(c);

    c = ai_extract_code("```r\nunterminated <- TRUE\n");
    t_str("ai_extract_code: unterminated fence", c, "unterminated <- TRUE");
    free(c);
}

/* ---- the transcript, on a real RichEdit control ------------------- */

/* Face name (UTF-8) and effects of the character at pos. */
static const char *face_at(HWND h, LONG pos, DWORD *effects)
{
    static char u8[LF_FACESIZE * 3];
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    re_select(h, pos, pos + 1);
    SendMessage(h, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM) &cf);
    WideCharToMultiByte(CP_UTF8, 0, cf.szFaceName, -1, u8, sizeof u8, NULL, NULL);
    if (effects) *effects = cf.dwEffects;
    return u8;
}

/* Position of the first occurrence of u8 in the control's text. */
static LONG pos_of(HWND h, const char *u8)
{
    wchar_t *w = u8_to_wcs(u8);
    LONG n = re_length(h);
    wchar_t *all = (wchar_t *) calloc((size_t) n + 2, sizeof(wchar_t));
    GETTEXTEX gt;
    memset(&gt, 0, sizeof gt);
    gt.cb = (DWORD) (((size_t) n + 1) * sizeof(wchar_t));
    gt.flags = GT_DEFAULT;             /* paragraphs as \r, one position each */
    gt.codepage = AI_CP_UTF16;
    SendMessage(h, EM_GETTEXTEX, (WPARAM) &gt, (LPARAM) all);
    wchar_t *at = w ? wcsstr(all, w) : NULL;
    LONG r = at ? (LONG) (at - all) : -1;
    free(all);
    free(w);
    return r;
}

static void test_transcript(void)
{
    LoadLibraryA("riched20.dll");
    HWND h = CreateWindowExW(0, L"RichEdit20W", L"", WS_POPUP | ES_MULTILINE,
                             0, 0, 400, 300, NULL, NULL, GetModuleHandle(NULL), NULL);
    t_ok("transcript: RichEdit control created", h != NULL);
    if (!h) return;
    re_setup(h);
    SendMessage(h, EM_SETREADONLY, TRUE, 0);

    const char *emoji = "\xf0\x9f\x98\x80";            /* U+1F600 */
    const char *question = "\xc3\x81rv\xc3\xadzt\xc5\xb1r\xc5\x91 \xe2\x89\xa4 "
                           "\xe4\xb8\xad\xe6\x96\x87 \xe2\x80\x93 \xf0\x9f\x98\x80";

    /* What ai_do_send and the stream do, in order. */
    tr_block(h, "Welcome.\n\n", TF_NOTE);
    tr_begin(h);
    tr_add(h, "You\n", TF_YOU);
    tr_add(h, question, TF_TEXT);
    tr_add(h, "\n\n", TF_TEXT);
    tr_add(h, "R assistant\n", TF_BOT);
    tr_end(h);
    md_begin(h);
    const char *answer[] = {
        "Use **t.te", "st()** with `var.equal", " = TRUE`:\n\n```r\nt.test(len ~ supp",
        ", data = ToothGrowth)\n```\n\n- first\n- sec", "ond *point*\n\nAnd 2 * 3 is 6, ",
        "and ", emoji, " done.", NULL };
    for (int i = 0; answer[i]; i++) md_feed(h, answer[i], 0);
    md_feed(h, "", 1);

    char *text = re_get_u8(h);
    dynbuf want;
    db_init(&want);
    db_add(&want, "Welcome.\n\nYou\n");
    db_add(&want, question);
    db_add(&want, "\n\nR assistant\nUse t.test() with var.equal = TRUE:\n\n"
                  "t.test(len ~ supp, data = ToothGrowth)\n\n"
                  "\xe2\x80\xa2 first\n\xe2\x80\xa2 second point\n\n"
                  "And 2 * 3 is 6, and ");
    db_add(&want, emoji);
    db_add(&want, " done.\n");
    t_str("transcript: Markdown shown without its markup", text, want.s);
    db_free(&want);
    free(text);

    DWORD fx = 0;
    LONG p = pos_of(h, "t.test()");
    t_str("transcript: **bold** is bold", face_at(h, p, &fx), "Segoe UI");
    t_ok("transcript: **bold** is bold (effect)", (fx & CFE_BOLD) != 0);
    p = pos_of(h, "var.equal");
    t_str("transcript: `inline code` in Consolas", face_at(h, p, NULL), "Consolas");
    p = pos_of(h, "t.test(len");
    t_str("transcript: code block in Consolas", face_at(h, p, NULL), "Consolas");
    p = pos_of(h, "point");
    face_at(h, p, &fx);
    t_ok("transcript: *italic* is italic", (fx & CFE_ITALIC) != 0);
    p = pos_of(h, "2 * 3");
    face_at(h, p, &fx);
    t_ok("transcript: 2 * 3 is not italic", (fx & CFE_ITALIC) == 0);

    p = pos_of(h, question);
    wchar_t *wq = u8_to_wcs(question);
    LONG qe = p + (LONG) wcslen(wq) - 2;               /* the emoji's first half */
    free(wq);
    t_str("transcript: question text in Segoe UI", face_at(h, p, NULL), "Segoe UI");
    t_str("transcript: emoji in the question has an emoji font",
          face_at(h, qe, NULL), "Segoe UI Emoji");
    /* RichEdit's font binding would put these in SimSun after Chinese. */
    t_str("transcript: text after Chinese stays in Segoe UI",
          face_at(h, qe - 1, NULL), "Segoe UI");
    t_str("transcript: emoji in the answer has an emoji font",
          face_at(h, pos_of(h, " done.") - 2, NULL), "Segoe UI Emoji");
    t_str("transcript: text after it is back in Segoe UI",
          face_at(h, pos_of(h, " done.") + 1, NULL), "Segoe UI");

    /* Consolas has no Chinese and no check mark: code borrows Segoe UI. */
    tr_add(h, "y <- 2 # \xe4\xb8\xad\xe6\x96\x87 \xe2\x9c\x93\n", TF_CODE);
    t_str("transcript: code stays in Consolas",
          face_at(h, pos_of(h, "y <- 2"), NULL), "Consolas");
    t_str("transcript: Chinese in code falls back to Segoe UI",
          face_at(h, pos_of(h, "\xe6\x96\x87"), NULL), "Segoe UI");
    t_str("transcript: a check mark in code falls back too",
          face_at(h, pos_of(h, "\xe2\x9c\x93"), NULL), "Segoe UI");
    t_str("transcript: the space between them stays Consolas",
          face_at(h, pos_of(h, "\xe2\x9c\x93") - 1, NULL), "Consolas");

    DestroyWindow(h);
}

/* The question box, with the panel's window procedure on it. */
static void test_question_box(void)
{
    HWND h = CreateWindowExW(0, L"RichEdit20W", L"", WS_POPUP | ES_MULTILINE,
                             0, 0, 400, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    t_ok("question box: RichEdit control created", h != NULL);
    if (!h) return;
    in_setup(h);
    g_hinput = h;
    g_re_proc = (WNDPROC) GetClassLongPtrW(h, GCLP_WNDPROC);
    g_prev_input = g_re_proc;
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR) ai_box_proc);

    /* set from outside, as Windows would pass it: UTF-16 */
    SendMessageW(h, WM_SETTEXT, 0, (LPARAM) L"ab \xD83D\xDE00 \x4E2D");
    char *t = re_get_u8(h);
    t_str("question box: set text arrives whole", t,
          "ab \xf0\x9f\x98\x80 \xe4\xb8\xad");
    free(t);
    t_str("question box: set text in Segoe UI", face_at(h, 0, NULL), "Segoe UI");
    t_str("question box: set emoji in the emoji font", face_at(h, 3, NULL), "Segoe UI Emoji");
    t_str("question box: text after it back in Segoe UI", face_at(h, 6, NULL), "Segoe UI");

    /* typed: a key, then the characters it produced, as TranslateMessage
       leaves them in the queue */
    LONG n = re_length(h);
    re_select(h, n, n);
    const wchar_t typed[] = L" x\xD83D\xDE00y\x0151";
    PostMessageW(h, WM_KEYDOWN, VK_F24, 1);
    for (const wchar_t *q = typed; *q; q++) PostMessageW(h, WM_CHAR, *q, 1);
    MSG m;
    while (PeekMessageW(&m, h, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    t = re_get_u8(h);
    t_str("question box: typed characters arrive whole", t,
          "ab \xf0\x9f\x98\x80 \xe4\xb8\xad x\xf0\x9f\x98\x80y\xc5\x91");
    free(t);
    t_str("question box: typed letter in Segoe UI", face_at(h, n + 1, NULL), "Segoe UI");
    t_str("question box: typed emoji in the emoji font", face_at(h, n + 2, NULL), "Segoe UI Emoji");
    t_str("question box: letter typed after it in Segoe UI", face_at(h, n + 4, NULL), "Segoe UI");
    t_ok("question box: typing can be undone", SendMessage(h, EM_CANUNDO, 0, 0) != 0);

    g_hinput = NULL;
    DestroyWindow(h);
}

static void test_find_error(void)
{
    const char *con =
        "> x <- 1\n"
        "> y <- log(-1)\n"
        "Warning message:\n"
        "In log(-1) : NaNs produced\n"
        "> lm(y ~ z, data = d)\n"
        "Error in eval(predvars, data, env) : object 'z' not found\n"
        "> summary(x)\n"
        "   Min. \n"
        "> ";
    char *e = ai_find_last_error(con);
    t_str("attach: last error with its command", e,
          "> lm(y ~ z, data = d)\n"
          "Error in eval(predvars, data, env) : object 'z' not found\n");
    free(e);

    e = ai_find_last_error("> f(\n+ 1)\nError in f(1) : boom\nCalls: f -> g\n> ");
    t_str("attach: multi-line command and Calls line", e,
          "> f(\n+ 1)\nError in f(1) : boom\nCalls: f -> g\n");
    free(e);

    e = ai_find_last_error("> 1 + 1\n[1] 2\n> ");
    t_ok("attach: no error in the console gives nothing", e == NULL);
    free(e);
}

static void test_relevance(void)
{
    const char *doc = "This chapter covers the two-sample t-test and variance.";
    t_ok("ai_relevance: matching words score",
          ai_relevance(doc, "how do I run a t-test on variance?") >= 2);
    t_ok("ai_relevance: unrelated question scores 0",
          ai_relevance(doc, "plot a histogram") == 0);
}

static void test_request_shape(void)
{
    conv_clear();
    conv_add("user", "earlier question");
    conv_add("assistant", "earlier answer");
    char *r = ai_build_request("what now?");
    t_ok("ai_build_request: has stream", strstr(r, "\"stream\":true") != NULL);
    t_ok("ai_build_request: system role first",
          strstr(r, "{\"role\":\"system\"") == strstr(r, "{\"role\""));
    t_ok("ai_build_request: keeps history",
          strstr(r, "earlier answer") != NULL);
    t_ok("ai_build_request: question last",
          strstr(r, "what now?") != NULL);
    t_ok("ai_build_request: thinking disabled by default",
          strstr(r, "\"enable_thinking\":false") != NULL);
    free(r);
    conv_clear();
}

/* ---- end-to-end against the fake server -------------------------- */

static void test_stream(int port, const char *expected)
{
    CFG.port = port;
    strcpy(CFG.host, "127.0.0.1");
    CFG.request_timeout = 15;
    CFG.startup_timeout = 15;

    t_ok("health check sees the server", ai_server_health() == 200);

    char *req = ai_build_request("please answer");
    ai_worker(req);          /* runs inline here, not on a thread */

    ai_lock();
    char *got = g_pending.n ? db_release(&g_pending) : NULL;
    ai_unlock();
    t_str("streamed answer, think block removed", got, expected);
    free(got);
}


/* A question containing every character json_escape has to deal with.
   The fake server does json.loads() on the body, so if the escaping is
   wrong the request is rejected and the answer never arrives. */
static void test_awkward_question(int port)
{
    CFG.port = port;
    conv_clear();
    char *req = ai_build_request(
        "line one\nline two\twith a tab\n"
        "a \"quoted\" word and a backslash \\ and a brace }\n"
        "accents: \xc3\xa9\xc3\xa1 emoji: \xf0\x9f\x98\x80");
    ai_worker(req);
    ai_lock();
    char *got = g_pending.n ? db_release(&g_pending) : NULL;
    ai_unlock();
    t_ok("awkward question survives escaping", got != NULL && *got);
    free(got);
    conv_clear();
}

/* Nothing listening: the worker must fail cleanly, not hang or crash. */
static void test_no_server(void)
{
    int saved = CFG.port;
    CFG.port = 45999;                 /* nothing is bound here */
    CFG.startup_timeout = 2;
    strcpy(CFG.server_exe, "Z:\\does\\not\\exist\\llama-server.exe");
    char *req = ai_build_request("hello");
    DWORD t0 = GetTickCount();
    ai_worker(req);
    DWORD dt = GetTickCount() - t0;
    t_ok("no server: fails instead of hanging", dt < 20000);
    ai_lock();
    char *got = g_pending.n ? db_release(&g_pending) : NULL;
    ai_unlock();
    t_ok("no server: produced no text", got == NULL);
    free(got);
    CFG.port = saved;
}

/* Stop pressed while the answer is streaming. */
static unsigned __stdcall cancel_after(void *ms)
{
    Sleep((DWORD) (uintptr_t) ms);
    ai_stop_generation();
    return 0;
}

static void test_cancel(int port)
{
    CFG.port = port;
    CFG.startup_timeout = 15;
    conv_clear();
    InterlockedExchange(&g_cancel, 0);
    unsigned tid;
    HANDLE th = (HANDLE) _beginthreadex(NULL, 0, cancel_after,
                                        (void *) (uintptr_t) 30, 0, &tid);
    char *req = ai_build_request("please answer");
    DWORD t0 = GetTickCount();
    ai_worker(req);
    DWORD dt = GetTickCount() - t0;
    if (th) { WaitForSingleObject(th, 2000); CloseHandle(th); }
    t_ok("stop: worker returns promptly", dt < 10000);
    t_ok("stop: cancel flag was observed", g_cancel != 0);
    ai_lock();
    char *got = g_pending.n ? db_release(&g_pending) : NULL;
    ai_unlock();
    free(got);
    InterlockedExchange(&g_cancel, 0);
    conv_clear();
}


/* ---- first-run model download against the fake server ------------- */

static char *read_small(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char b[256];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = '\0';
    return xstrdup(b);
}

static void write_small(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static long long file_len(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return -1;
    return ((long long) a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static void test_download(int port)
{
    char cwd[MAX_PATH], dest[MAX_PATH + 32], part[MAX_PATH + 40];
    char url[128], slow[128], missing[128], err[512];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    snprintf(dest, sizeof dest, "%s\\dl-test.gguf", cwd);
    snprintf(part, sizeof part, "%s.part", dest);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/file/model.gguf", port);
    snprintf(slow, sizeof slow, "http://127.0.0.1:%d/file/model.gguf?slow", port);
    snprintf(missing, sizeof missing, "http://127.0.0.1:%d/nothing-here", port);

    char *sha = read_small("download.sha256");
    char *sz = read_small("download.size");
    long long size = sz ? atoll(sz) : 0;
    t_ok("download: fixture from the fake server", sha && size > 0);
    if (!sha || size <= 0) { free(sha); free(sz); return; }

    DeleteFileA(dest); DeleteFileA(part);
    InterlockedExchange(&g_cancel, 0);
    int ok = ai_download_file(url, dest, sha, size, err, sizeof err);
    char *rng = read_small("last_range.txt");
    t_ok("download: fresh file arrives complete", ok && file_len(dest) == size);
    t_ok("download: .part renamed to the final name", file_len(part) < 0);
    t_str("download: fresh request sends no Range", rng, "none");
    free(rng);

    /* Half a file left from an earlier attempt. */
    {
        FILE *in = fopen(dest, "rb"), *o = fopen(part, "wb");
        char *b = (char *) malloc((size_t) (size / 2));
        if (in && o && b) {
            fread(b, 1, (size_t) (size / 2), in);
            fwrite(b, 1, (size_t) (size / 2), o);
        }
        if (in) fclose(in);
        if (o) fclose(o);
        free(b);
    }
    DeleteFileA(dest);
    ok = ai_download_file(url, dest, sha, size, err, sizeof err);
    char want[64];
    snprintf(want, sizeof want, "bytes=%lld-", size / 2);
    rng = read_small("last_range.txt");
    t_str("download: resume asks only for the rest", rng, want);
    t_ok("download: resumed file complete and verified", ok && file_len(dest) == size);
    free(rng);

    DeleteFileA(dest);
    ok = ai_download_file(url, dest,
        "0000000000000000000000000000000000000000000000000000000000000000",
        size, err, sizeof err);
    t_ok("download: checksum mismatch refused", !ok && file_len(dest) < 0);
    t_ok("download: damaged file deleted", file_len(part) < 0);
    t_ok("download: mismatch explained", strstr(err, "checksum") != NULL);

    ok = ai_download_file(missing, dest, sha, size, err, sizeof err);
    t_ok("download: HTTP error reported", !ok && strstr(err, "404"));

    ok = ai_download_file("http://127.0.0.1:45999/file/x", dest, sha, size,
                          err, sizeof err);
    t_ok("download: unreachable server reported", !ok && strstr(err, "reach"));

    /* Stop pauses; the next attempt carries on from the part file. */
    unsigned tid;
    HANDLE th = (HANDLE) _beginthreadex(NULL, 0, cancel_after,
                                        (void *) (uintptr_t) 600, 0, &tid);
    ok = ai_download_file(slow, dest, sha, size, err, sizeof err);
    if (th) { WaitForSingleObject(th, 3000); CloseHandle(th); }
    long long partial = file_len(part);
    t_ok("download: Stop pauses it", !ok && strstr(err, "paused"));
    t_ok("download: a paused download keeps its part",
         partial > 0 && partial < size);
    InterlockedExchange(&g_cancel, 0);
    ok = ai_download_file(url, dest, sha, size, err, sizeof err);
    t_ok("download: a paused download continues", ok && file_len(dest) == size);

    /* All bytes arrived but the rename never happened (power cut). */
    MoveFileExA(dest, part, MOVEFILE_REPLACE_EXISTING);
    write_small("last_range.txt", "untouched");
    ok = ai_download_file(url, dest, sha, size, err, sizeof err);
    rng = read_small("last_range.txt");
    t_ok("download: complete part is checked, not fetched again",
         ok && rng && !strcmp(rng, "untouched") && file_len(dest) == size);
    free(rng);

    DeleteFileA(dest);
    DeleteFileA(part);
    free(sha);
    free(sz);
}

/* ---- against the real model -------------------------------------- */

/* Drives the production path end to end: ai_worker() finds no server,
   starts llama-server itself with the same command line Rgui uses,
   waits for the model to load, streams an answer and stops the server.
   Usage: test_aichat --real <llama-server.exe> <model.gguf> <port> */
static int test_real(const char *exe, const char *model, int port,
                     const char *extra)
{
    snprintf(CFG.server_exe, sizeof CFG.server_exe, "%s", exe);
    snprintf(CFG.model, sizeof CFG.model, "%s", model);
    CFG.port = port;
    CFG.startup_timeout = 600;
    CFG.request_timeout = 600;
    CFG.n_predict = 256;
    snprintf(CFG.extra_args, sizeof CFG.extra_args, "%s", extra);  /* default: empty */

    char ai[MAX_PATH];
    snprintf(ai, sizeof ai, "%s", exe);
    for (int up = 0; up < 2; up++) {
        char *sl = strrchr(ai, '\\');
        if (sl) *sl = '\0';
    }
    snprintf(CFG.system_prompt_file, sizeof CFG.system_prompt_file,
             "%s\\system_prompt.txt", ai);
    snprintf(CFG.context_dir, sizeof CFG.context_dir, "%s\\context", ai);
    printf("prompt  %s\ncontext %s\n", CFG.system_prompt_file, CFG.context_dir);

    {
        char tmp[MAX_PATH], dest[MAX_PATH + 32], err[512];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(dest, sizeof dest, "%srgui-https-test.zip", tmp);
        DeleteFileA(dest);
        int ok = ai_download_file(
            "https://github.com/ggml-org/llama.cpp/releases/download/b11153/"
            "llama-b11153-bin-win-cpu-x64.zip", dest,
            "569d19826f3fb00a3fc2df7bd68ab9ad33e5c0d6d69ce022b24372700cee7931",
            18559858LL, err, sizeof err);
        t_ok("real: HTTPS download, redirect, checksum", ok);
        if (!ok) printf("      %s\n", err);
        DeleteFileA(dest);
    }

    conv_clear();
    DWORD t0 = GetTickCount();
    char *req = ai_build_request(
        "Write one line of R code that computes the mean of c(2, 4, 9). "
        "Answer with only a fenced r code block.");
    ai_worker(req);
    double secs = (GetTickCount() - t0) / 1000.0;

    ai_lock();
    char *got = g_pending.n ? db_release(&g_pending) : NULL;
    ai_unlock();

    /* Show what the status line would have said.  Only now, after the
       answer has been taken out of g_pending: pumping also delivers
       WM_AI_DATA, which would move the text somewhere else. */
    g_status = (control) STUB;
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&m);

    printf("---- model answer (%.1f s including model load) ----\n%s\n"
           "-----------------------------------------------------\n",
           secs, got ? got : "(nothing)");

    char *code = got ? ai_extract_code(got) : NULL;
    t_ok("real model: server started by aichat.c", g_srv_proc != NULL);
    t_ok("real model: produced an answer", got != NULL && *got);
    t_ok("real model: answer mentions mean()", got && strstr(got, "mean"));
    t_ok("real model: code is in a fenced block", got && strstr(got, "```"));
    t_ok("real model: no <think> text leaked", got && !strstr(got, "<think>"));
    t_ok("real model: Copy code finds code", code && *code);
    free(code);
    free(got);

    ai_server_stop();
    t_ok("real model: server stopped", g_srv_proc == NULL);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    ai_defaults();
    CFG.loaded = 1;
    CFG.context_dir[0] = '\0';        /* no course material in the test */
    CFG.system_prompt_file[0] = '\0';
    InitializeCriticalSection(&g_cs);   g_cs_up = 1;
    InitializeCriticalSection(&g_srv_cs); g_srv_cs_up = 1;
    db_init(&g_pending);
    ai_make_msgwin();

    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "--real"))
        return test_real(argv[2], argv[3], atoi(argv[4]), argc == 6 ? argv[5] : "");

    test_json();
    test_think_filter();
    test_extract_code();
    test_find_error();
    test_relevance();
    test_request_shape();
    test_transcript();
    test_question_box();

    if (argc > 2) {
        /* argv[1] = port, argv[2] = file holding the expected answer */
        FILE *f = fopen(argv[2], "rb");
        char exp[4096];
        size_t n = f ? fread(exp, 1, sizeof exp - 1, f) : 0;
        exp[n] = '\0';
        if (f) fclose(f);
        int port = atoi(argv[1]);
        test_stream(port, exp);
        test_awkward_question(port);
        test_cancel(port);
        test_no_server();
        test_download(port);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
