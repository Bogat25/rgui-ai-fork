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
