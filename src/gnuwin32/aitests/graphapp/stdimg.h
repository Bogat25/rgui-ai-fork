/* Test-harness stand-in for graphapp/stdimg.h.  The real header declares
   these images dllimport (they live in Rgraphapp.dll); the harness defines
   them itself, so here they are plain declarations.  Found before the
   real one because the harness is compiled with -I. first. */
extern image open_image, copy_image, paste_image, stop_image, console_image;
