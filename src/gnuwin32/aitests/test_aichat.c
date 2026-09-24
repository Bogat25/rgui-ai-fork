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
    t_str("ai_extract_code: no fence falls back to all", c, "no fences here");
    free(c);

    c = ai_extract_code("```r\nunterminated <- TRUE\n");
    t_str("ai_extract_code: unterminated fence", c, "unterminated <- TRUE");
    free(c);
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

    test_json();
    test_think_filter();
    test_extract_code();
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
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
