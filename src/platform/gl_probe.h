#ifndef BOZ_GL_PROBE_H
#define BOZ_GL_PROBE_H

/* Windows only: open a window with the bundled Mesa WGL using one Gallium driver. 0 on success. */
int gl_probe_run(const char *driver);

/* Windows only: export GALLIUM_DRIVER for the first driver that can present to a window. */
void gl_probe_select_driver(void);

#endif
