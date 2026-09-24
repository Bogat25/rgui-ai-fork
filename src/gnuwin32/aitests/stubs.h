/* GraphApp / R stubs for the aichat.c test harness.
   Signatures copied from graphapp.h and ga.h. */

static struct objinfo stub_obj;
#define STUB (&stub_obj)

font consolefn = NULL;

window   GA_newwindow(const char *n, rect r, long f)
{ (void)n; (void)r; (void)f; return STUB; }
label    GA_newlabel(const char *t, rect r, int a)
{ (void)t; (void)r; (void)a; return STUB; }
textbox  GA_newtextarea(const char *t, rect r) { (void)t; (void)r; return STUB; }
textbox  GA_newtextbox(const char *t, rect r)  { (void)t; (void)r; return STUB; }
button   GA_newbutton(const char *t, rect r, actionfn f)
{ (void)t; (void)r; (void)f; return STUB; }
menubar  GA_newmenubar(actionfn f) { (void)f; return STUB; }
menu     GA_newmenu(const char *n) { (void)n; return STUB; }
menuitem GA_newmenuitem(const char *n, int k, menufn f)
{ (void)n; (void)k; (void)f; return STUB; }
menu     GA_newmdimenu(void) { return STUB; }

void  GA_addto(control c) { (void)c; }
void  GA_delobj(objptr o) { (void)o; }
void  GA_show(control c) { (void)c; }
void  GA_hide(control c) { (void)c; }
void  GA_enable(control c) { (void)c; }
void  GA_disable(control c) { (void)c; }
void  GA_resize(control c, rect r) { (void)c; (void)r; }
void  GA_settext(control c, const char *t)
{ (void)c; if (t) fprintf(stderr, "    [status] %s\n", t); }
void  GA_settextfont(control c, font f) { (void)c; (void)f; }
void  GA_setclose(control c, actionfn f) { (void)c; (void)f; }
void  GA_setresize(control c, drawfn f) { (void)c; (void)f; }
void  GA_setkeydown(control c, keyfn f) { (void)c; (void)f; }
void  GA_gsetcursor(drawing d, cursor c) { (void)d; (void)c; }
void *GA_getHandle(window w) { (void)w; return NULL; }
int   GA_ismdi(void) { return 0; }
int   GA_devicewidth(drawing d) { (void)d; return 1280; }

rect  GA_newrect(int a, int b, int c, int d)
{ rect r; r.x = a; r.y = b; r.width = c; r.height = d; return r; }
rect  GA_objrect(objptr o) { (void)o; return GA_newrect(0, 0, 600, 500); }

void  R_ShowMessage(const char *s) { fprintf(stderr, "    [msgbox] %s\n", s); }
RECT *RgetMDIsize(void) { static RECT r = {0, 0, 1280, 800}; return &r; }
int   Rgui_Edit(const char *f, int e, const char *t, int m)
{ (void)e; (void)m; fprintf(stderr, "    [editor] %s (%s)\n", t, f); return 0; }

char *libintl_dgettext(const char *d, const char *m)
{ (void)d; return (char *) m; }
