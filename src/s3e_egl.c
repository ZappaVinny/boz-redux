#include "s3e_host_internal.h"

#include <limits.h>

enum {
    SDL_INIT_VIDEO = 0x00000020u,
    SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
    SDL_WINDOW_OPENGL = 0x00000002u,
    SDL_WINDOW_SHOWN = 0x00000004u,
    SDL_WINDOWPOS_UNDEFINED = 0x1fff0000u,
    SDL_GL_RED_SIZE = 0,
    SDL_GL_GREEN_SIZE = 1,
    SDL_GL_BLUE_SIZE = 2,
    SDL_GL_ALPHA_SIZE = 3,
    SDL_GL_DOUBLEBUFFER = 5,
    SDL_GL_DEPTH_SIZE = 6,
    SDL_GL_STENCIL_SIZE = 7,
    SDL_GL_CONTEXT_MAJOR_VERSION = 17,
    SDL_GL_CONTEXT_MINOR_VERSION = 18,
    SDL_GL_CONTEXT_PROFILE_MASK = 21,
    SDL_GL_CONTEXT_PROFILE_ES = 0x0004,
    EGL_SUCCESS = 0x3000,
    EGL_NOT_INITIALIZED = 0x3001,
    EGL_BAD_ATTRIBUTE = 0x3004,
    EGL_BAD_CONFIG = 0x3005,
    EGL_BAD_CONTEXT = 0x3006,
    EGL_BAD_MATCH = 0x3009,
    EGL_BAD_NATIVE_WINDOW = 0x300b,
    EGL_BAD_PARAMETER = 0x300c,
    EGL_BAD_SURFACE = 0x300d,
    EGL_BUFFER_SIZE = 0x3020,
    EGL_ALPHA_SIZE = 0x3021,
    EGL_BLUE_SIZE = 0x3022,
    EGL_GREEN_SIZE = 0x3023,
    EGL_RED_SIZE = 0x3024,
    EGL_DEPTH_SIZE = 0x3025,
    EGL_STENCIL_SIZE = 0x3026,
    EGL_CONFIG_ID = 0x3028,
    EGL_SURFACE_TYPE = 0x3033,
    EGL_NONE = 0x3038,
    EGL_RENDERABLE_TYPE = 0x3040,
    EGL_OPENGL_ES_BIT = 0x0001,
    EGL_OPENGL_ES2_BIT = 0x0004,
    EGL_WINDOW_BIT = 0x0004,
    EGL_VENDOR = 0x3053,
    EGL_VERSION = 0x3054,
    EGL_EXTENSIONS = 0x3055,
    EGL_HEIGHT = 0x3056,
    EGL_WIDTH = 0x3057,
    EGL_CLIENT_APIS = 0x308d,
    EGL_CONTEXT_CLIENT_TYPE = 0x3097,
    EGL_CONTEXT_CLIENT_VERSION = 0x3098,
    EGL_OPENGL_ES_API = 0x30a0,
};

struct sdl_video_api {
    int (*InitSubSystem)(uint32_t flags);
    void (*QuitSubSystem)(uint32_t flags);
    int (*GL_SetAttribute)(int attr, int value);
    void *(*GL_GetProcAddress)(const char *name);
    void *(*CreateWindow)(const char *title, int x, int y, int width, int height, uint32_t flags);
    void *(*GL_CreateContext)(void *window);
    int (*GL_MakeCurrent)(void *window, void *context);
    void (*GL_SwapWindow)(void *window);
    int (*GL_SetSwapInterval)(int interval);
    int (*GetWindowDisplayMode)(void *window, void *mode);
    void (*GL_GetDrawableSize)(void *window, int *w, int *h);
    void (*GL_DeleteContext)(void *context);
    void (*DestroyWindow)(void *window);
    const char *(*GetCurrentVideoDriver)(void);
    const char *(*GetError)(void);
};

struct sdl_egl_state {
    struct sdl_video_api api;
    void *library;
    void *window;
    void *context;
    EGLDisplay display;
    EGLint error;
    int initialized;
    int active;
    int current;
    int surface_valid;
    int context_valid;
    int context_destroy_pending;
    int context_major;
    int red_size;
    int green_size;
    int blue_size;
    int alpha_size;
    int depth_size;
    int stencil_size;
    int renderable_type;
};

static struct sdl_egl_state g_sdl = {
    .error = EGL_SUCCESS,
    .context_major = 2,
    .red_size = 8,
    .green_size = 8,
    .blue_size = 8,
    .alpha_size = 8,
    .depth_size = 16,
    .renderable_type = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT,
};
static int g_display_token;
static int g_config_token;

static EGLDisplay fallback_display(void) {
    return (EGLDisplay)(void *)&g_display_token;
}

static EGLConfig fallback_config(void) {
    return (EGLConfig)(void *)&g_config_token;
}

static const char *sdl_error(void) {
    return g_sdl.api.GetError ? g_sdl.api.GetError() : "unknown SDL error";
}

static void set_error(EGLint error) {
    g_sdl.error = error;
}

static int load_symbol(void **target, const char *name) {
    *target = plat_lib_symbol(g_sdl.library, name);
    return *target != NULL;
}

static int find_sdl_sibling_library(const char *const *names, char *path, size_t path_size) {
    void *anchor = plat_lib_symbol(g_sdl.library, "SDL_InitSubSystem");
    char library_path[PATH_MAX];
    if (!anchor || !plat_lib_path(anchor, library_path, sizeof(library_path))) {
        return 0;
    }

    const char *separator = strrchr(library_path, '/');
    const char *backslash = strrchr(library_path, '\\');
    if (backslash && (!separator || backslash > separator)) {
        separator = backslash;
    }
    if (!separator) {
        return 0;
    }

    size_t directory_length = (size_t)(separator - library_path);
    for (size_t i = 0; names[i]; ++i) {
        int length =
            snprintf(path, path_size, "%.*s/%s", (int)directory_length, library_path, names[i]);
        if (length > 0 && (size_t)length < path_size && access(path, R_OK) == 0) {
            return 1;
        }
    }
    return 0;
}

static void set_sdl_sibling_library(const char *variable, const char *const *names) {
    const char *configured = getenv(variable);
    if (configured && configured[0]) {
        return;
    }

    char path[PATH_MAX];
    if (find_sdl_sibling_library(names, path, sizeof(path))) {
        setenv(variable, path, 1);
    }
}

#if defined(_WIN32)
/* Windows renderers: ANGLE (libEGL.dll and libGLESv2.dll next to the game, OpenGL ES on
 * Direct3D 11) through SDL's EGL path, like Linux; or with BOZ_RENDERER=mesa the bundled Mesa in
 * mesa\ through WGL. */
static int win_use_mesa(void) {
    const char *renderer = getenv("BOZ_RENDERER");
    return renderer && !strcmp(renderer, "mesa");
}
#endif

static void configure_sdl_graphics_libraries(void) {
#if defined(_WIN32)
    if (win_use_mesa()) {
        /* Mesa's EGL cannot create window surfaces on Windows, so SDL uses Mesa's WGL driver and
         * ensure_context() creates the OpenGL ES context through WGL_EXT_create_context_es_profile. */
        const char *wgl_names[] = {"mesa/opengl32.dll", NULL};
        set_sdl_sibling_library("SDL_VIDEO_GL_DRIVER", wgl_names);
        return;
    }
    setenv("SDL_OPENGL_ES_DRIVER", "1", 1); /* SDL: OpenGL ES through EGL, not WGL */
#endif
    const char *egl_names[] = {BOZ_LIB_EGL, NULL};
    const char *gles_names[] = {BOZ_LIB_GLES2, NULL};
    set_sdl_sibling_library("SDL_VIDEO_EGL_DRIVER", egl_names);
    set_sdl_sibling_library("SDL_VIDEO_GL_DRIVER", gles_names);
}

static int load_sdl_video(void) {
    if (g_sdl.library) {
        return 1;
    }

    const char *libraries[] = {BOZ_LIB_SDL2, NULL};
    g_sdl.library = open_first(libraries);
    if (!g_sdl.library) {
        return 0;
    }

    int ok = 1;
    ok &= load_symbol((void **)&g_sdl.api.InitSubSystem, "SDL_InitSubSystem");
    ok &= load_symbol((void **)&g_sdl.api.QuitSubSystem, "SDL_QuitSubSystem");
    ok &= load_symbol((void **)&g_sdl.api.GL_SetAttribute, "SDL_GL_SetAttribute");
    ok &= load_symbol((void **)&g_sdl.api.GL_GetProcAddress, "SDL_GL_GetProcAddress");
    ok &= load_symbol((void **)&g_sdl.api.CreateWindow, "SDL_CreateWindow");
    ok &= load_symbol((void **)&g_sdl.api.GL_CreateContext, "SDL_GL_CreateContext");
    ok &= load_symbol((void **)&g_sdl.api.GL_MakeCurrent, "SDL_GL_MakeCurrent");
    ok &= load_symbol((void **)&g_sdl.api.GL_SwapWindow, "SDL_GL_SwapWindow");
    load_symbol((void **)&g_sdl.api.GL_SetSwapInterval, "SDL_GL_SetSwapInterval");
    load_symbol((void **)&g_sdl.api.GetWindowDisplayMode, "SDL_GetWindowDisplayMode");
    load_symbol((void **)&g_sdl.api.GL_GetDrawableSize, "SDL_GL_GetDrawableSize");
    ok &= load_symbol((void **)&g_sdl.api.GL_DeleteContext, "SDL_GL_DeleteContext");
    ok &= load_symbol((void **)&g_sdl.api.DestroyWindow, "SDL_DestroyWindow");
    ok &= load_symbol((void **)&g_sdl.api.GetError, "SDL_GetError");
    load_symbol((void **)&g_sdl.api.GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
    if (!ok) {
        plat_lib_close(g_sdl.library);
        memset(&g_sdl.api, 0, sizeof(g_sdl.api));
        g_sdl.library = NULL;
    }
    return ok;
}

static void *try_graphics_library(const char *path, char *error, size_t error_size) {
    plat_lib_error();
    void *library = plat_lib_open(path);
    if (!library && !error[0]) {
        const char *reason = plat_lib_error();
        snprintf(error, error_size, "%s: %s", path, reason ? reason : "unknown error");
    }
    return library;
}

static void *open_graphics_library(const char *configured, const char *const *siblings,
                                   const char *const *fallbacks, char *error, size_t error_size) {
    error[0] = '\0';
    if (configured && configured[0]) {
        void *library = try_graphics_library(configured, error, error_size);
        if (library) {
            return library;
        }
    }

    char path[PATH_MAX];
    if (find_sdl_sibling_library(siblings, path, sizeof(path))) {
        void *library = try_graphics_library(path, error, error_size);
        if (library) {
            return library;
        }
    }
    for (size_t i = 0; fallbacks[i]; ++i) {
        void *library = try_graphics_library(fallbacks[i], error, error_size);
        if (library) {
            return library;
        }
    }
    return NULL;
}

bool egl_backend_load_libraries(void) {
#if defined(_WIN32)
    const char *angle_egl[] = {"libEGL.dll", NULL};
    const char *angle_gles1[] = {"libGLESv1_CM.dll", NULL};
    const char *angle_gles2[] = {"libGLESv2.dll", NULL};
    const char *mesa_egl[] = {"mesa/libEGL.dll", NULL};
    const char *mesa_gles1[] = {"mesa/libGLESv1_CM.dll", NULL};
    const char *mesa_gles2[] = {"mesa/libGLESv2.dll", NULL};
    const char *const *egl_names = win_use_mesa() ? mesa_egl : angle_egl;
    const char *const *gles1_names = win_use_mesa() ? mesa_gles1 : angle_gles1;
    const char *const *gles2_names = win_use_mesa() ? mesa_gles2 : angle_gles2;
    fprintf(stderr, "[egl] renderer: %s\n", win_use_mesa() ? "Mesa (WGL)" : "ANGLE (Direct3D 11)");
#else
    const char *egl_names[] = {"libEGL.so.1", "libEGL.so", "libmali.so", NULL};
    const char *gles1_names[] = {"libGLESv1_CM.so.1", "libGLESv1_CM.so", "libmali.so", NULL};
    const char *gles2_names[] = {"libGLESv2.so.2", "libGLESv2.so", "libmali.so", NULL};
#endif
    char egl_error[512];
    char gles1_error[512];
    char gles2_error[512];

    if (!load_sdl_video()) {
        fprintf(stderr, "[egl] SDL2 video library is unavailable\n");
        return false;
    }
    configure_sdl_graphics_libraries();

    g_egl = open_graphics_library(getenv("SDL_VIDEO_EGL_DRIVER"), egl_names, egl_names, egl_error,
                                  sizeof(egl_error));
    g_gles1 =
        open_graphics_library(NULL, gles1_names, gles1_names, gles1_error, sizeof(gles1_error));
#if defined(_WIN32)
    const char *gles2_configured = win_use_mesa() ? NULL : getenv("SDL_VIDEO_GL_DRIVER");
#else
    const char *gles2_configured = getenv("SDL_VIDEO_GL_DRIVER");
#endif
    g_gles2 = open_graphics_library(gles2_configured, gles2_names, gles2_names, gles2_error,
                                    sizeof(gles2_error));
    if (g_egl && g_gles2 && !g_gles1) {
        fprintf(stderr, "[egl] OpenGL ES 1 library unavailable, resolving through EGL: %s\n",
                gles1_error);
    }
#if defined(_WIN32)
    if (!g_egl || !g_gles2) {
        fprintf(stderr, "[egl] ANGLE not found next to the executable; using the system OpenGL ES "
                        "driver through SDL\n");
        return true;
    }
#endif
    if (!g_egl || !g_gles2) {
        if (!g_egl) {
            fprintf(stderr, "[egl] EGL load failed: %s\n", egl_error);
        }
        if (!g_gles2) {
            fprintf(stderr, "[egl] OpenGL ES 2 load failed: %s\n", gles2_error);
        }
        fprintf(stderr, "[egl] compatible EGL and OpenGL ES libraries are required\n");
        if (g_egl) {
            plat_lib_close(g_egl);
            g_egl = NULL;
        }
        if (g_gles1) {
            plat_lib_close(g_gles1);
            g_gles1 = NULL;
        }
        if (g_gles2) {
            plat_lib_close(g_gles2);
            g_gles2 = NULL;
        }
        egl_backend_shutdown();
        return false;
    }
    return true;
}

static int initialize_sdl(EGLDisplay display) {
    if (g_sdl.active) {
        return 1;
    }
    if (!load_sdl_video()) {
        set_error(EGL_NOT_INITIALIZED);
        return 0;
    }
    configure_sdl_graphics_libraries();
    /* The S3E loader owns fault handlers used by its compatibility boundary. */
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
    if (g_sdl.api.InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[egl] SDL video initialization failed: %s\n", sdl_error());
        set_error(EGL_NOT_INITIALIZED);
        return 0;
    }
    g_sdl.initialized = 1;
    g_sdl.display = display ? display : fallback_display();
    g_sdl.active = 1;
    fprintf(stderr, "[egl] using SDL video backend driver=%s\n",
            g_sdl.api.GetCurrentVideoDriver ? g_sdl.api.GetCurrentVideoDriver() : "unknown");
    return 1;
}

static int uses_sdl(EGLDisplay display) {
    return g_sdl.active && (!display || display == g_sdl.display || display == fallback_display());
}

static int sdl_owns_display(void) {
#if defined(_WIN32)
    /* There is no headless native EGL window on Windows; SDL always owns the window. */
    return 1;
#endif
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    const char *x11_display = getenv("DISPLAY");
    const char *video_driver = getenv("SDL_VIDEODRIVER");
    return (wayland_display && wayland_display[0]) || (x11_display && x11_display[0]) ||
           (video_driver && strcmp(video_driver, "kmsdrm") == 0);
}

static void apply_window_attributes(void) {
#if defined(_WIN32)
    if (win_use_mesa()) {
        /* SDL only offers WGL for desktop profiles; see create_wgl_es_context(). */
        g_sdl.api.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        g_sdl.api.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        g_sdl.api.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    } else
#endif
    {
        g_sdl.api.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        g_sdl.api.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, g_sdl.context_major);
        g_sdl.api.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    }
    g_sdl.api.GL_SetAttribute(SDL_GL_RED_SIZE, g_sdl.red_size);
    g_sdl.api.GL_SetAttribute(SDL_GL_GREEN_SIZE, g_sdl.green_size);
    g_sdl.api.GL_SetAttribute(SDL_GL_BLUE_SIZE, g_sdl.blue_size);
    g_sdl.api.GL_SetAttribute(SDL_GL_ALPHA_SIZE, g_sdl.alpha_size);
    g_sdl.api.GL_SetAttribute(SDL_GL_DEPTH_SIZE, g_sdl.depth_size);
    g_sdl.api.GL_SetAttribute(SDL_GL_STENCIL_SIZE, g_sdl.stencil_size);
    g_sdl.api.GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

static int ensure_window(void) {
    if (g_sdl.window) {
        return 1;
    }
    apply_window_attributes();
    g_sdl.window = g_sdl.api.CreateWindow(
        "Call of Duty: Black Ops Zombies", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        g_native_window.width, g_native_window.height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
            (getenv("BOZ_WINDOWED") && strcmp(getenv("BOZ_WINDOWED"), "0") != 0
                 ? 0x20u
                 : (uint32_t)SDL_WINDOW_FULLSCREEN_DESKTOP));
    if (!g_sdl.window) {
        static int failures;
        fprintf(stderr, "[egl] SDL window creation failed: %s\n", sdl_error());
        if (++failures >= 5) {
            fprintf(stderr, "[egl] unable to create an OpenGL ES window; check the graphics driver "
                            "or that the bundled Mesa DLLs are next to the executable\n");
            _Exit(1);
        }
        set_error(EGL_BAD_NATIVE_WINDOW);
        return 0;
    }
    fprintf(stderr, "[egl] SDL window size=%ux%u\n", g_native_window.width, g_native_window.height);
    return 1;
}

#if defined(_WIN32)
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_ES_PROFILE_BIT_EXT 0x0004

/* SDL refuses WGL for OpenGL ES 1 and would fall back to EGL, which Mesa cannot do on Windows.
 * SDL's desktop context gives the window its pixel format and makes the DC current; the ES
 * context is then created on that DC. SDL's WGL make-current, swap and delete take any HGLRC. */
static void *create_wgl_es_context(void) {
    void *bootstrap = g_sdl.api.GL_CreateContext(g_sdl.window);
    if (!bootstrap) {
        fprintf(stderr, "[egl] WGL bootstrap context failed: %s\n", sdl_error());
        return NULL;
    }
    void *(GL_APIENTRY *create_attribs)(void *, void *, const int *) =
        g_sdl.api.GL_GetProcAddress("wglCreateContextAttribsARB");
    void *(GL_APIENTRY *current_dc)(void) = g_sdl.api.GL_GetProcAddress("wglGetCurrentDC");
    void *dc = current_dc ? current_dc() : NULL;
    int minor = g_sdl.context_major == 1 ? 1 : 0;
    const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,  g_sdl.context_major,
                              WGL_CONTEXT_MINOR_VERSION_ARB,  minor,
                              WGL_CONTEXT_PROFILE_MASK_ARB,   WGL_CONTEXT_ES_PROFILE_BIT_EXT,
                              0};
    void *context = create_attribs && dc ? create_attribs(dc, NULL, attributes) : NULL;
    g_sdl.api.GL_MakeCurrent(g_sdl.window, NULL);
    g_sdl.api.GL_DeleteContext(bootstrap);
    if (!context) {
        fprintf(stderr, "[egl] WGL OpenGL ES %d.%d context failed (create=%p dc=%p)\n",
                g_sdl.context_major, minor, (void *)create_attribs, dc);
        return NULL;
    }
    if (g_sdl.api.GL_MakeCurrent(g_sdl.window, context) == 0) {
        const char *(GL_APIENTRY *get_string)(unsigned int) =
            g_sdl.api.GL_GetProcAddress("glGetString");
        const char *version = get_string ? get_string(0x1F02) : NULL;
        const char *renderer = get_string ? get_string(0x1F01) : NULL;
        fprintf(stderr, "[egl] WGL context: %s on %s\n", version ? version : "?",
                renderer ? renderer : "?");
        g_sdl.api.GL_MakeCurrent(g_sdl.window, NULL);
    }
    return context;
}
#endif

static int ensure_context(void) {
    if (g_sdl.context) {
        return 1;
    }
    if (!ensure_window()) {
        return 0;
    }
    apply_window_attributes();
#if defined(_WIN32)
    g_sdl.context = win_use_mesa() ? create_wgl_es_context()
                                   : g_sdl.api.GL_CreateContext(g_sdl.window);
#else
    g_sdl.context = g_sdl.api.GL_CreateContext(g_sdl.window);
#endif
    if (!g_sdl.context) {
        fprintf(stderr, "[egl] SDL OpenGL ES %d context creation failed: %s\n", g_sdl.context_major,
                sdl_error());
        set_error(EGL_BAD_CONTEXT);
        return 0;
    }
    return 1;
}

static void destroy_pending_context(void) {
    if (!g_sdl.current && g_sdl.context_destroy_pending && g_sdl.context) {
        g_sdl.api.GL_DeleteContext(g_sdl.context);
        g_sdl.context = NULL;
        g_sdl.context_destroy_pending = 0;
    }
}

static EGLDisplay host_eglGetDisplay(void *native_display) {
    if (sdl_owns_display()) {
        return fallback_display();
    }
    EGLDisplay (GL_APIENTRY *native)(void *) = lookup_egl("eglGetDisplay");
    EGLDisplay display = native ? native(native_display) : NULL;
    return display ? display : fallback_display();
}

static EGLBoolean host_eglInitialize(EGLDisplay display, EGLint *major, EGLint *minor) {
    EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLint *, EGLint *) = lookup_egl("eglInitialize");
    int native_display = display != fallback_display();
    if (native_display && native && native(display, major, minor)) {
        return 1;
    }
    EGLint (GL_APIENTRY *get_error)(void) = lookup_egl("eglGetError");
    EGLint native_error = native_display && get_error ? get_error() : EGL_NOT_INITIALIZED;
    if (!initialize_sdl(display)) {
        if (native_display) {
            fprintf(stderr, "[egl] native EGL failed (0x%04x) and SDL fallback is unavailable\n",
                    native_error);
        } else {
            fprintf(stderr, "[egl] SDL video was requested but is unavailable\n");
        }
        return 0;
    }
    if (major) {
        *major = 1;
    }
    if (minor) {
        *minor = 4;
    }
    if (native_display) {
        fprintf(stderr, "[egl] native EGL failed (0x%04x); SDL owns the display surface\n",
                native_error);
    } else {
        fprintf(stderr, "[egl] using an SDL window surface\n");
    }
    return 1;
}

static EGLBoolean host_eglGetConfigs(EGLDisplay display, EGLConfig *configs, EGLint config_size,
                                     EGLint *count) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLConfig *, EGLint, EGLint *) =
            lookup_egl("eglGetConfigs");
        return native ? native(display, configs, config_size, count) : 0;
    }
    if (!count || config_size < 0) {
        set_error(EGL_BAD_PARAMETER);
        return 0;
    }
    *count = 1;
    if (configs && config_size > 0) {
        configs[0] = fallback_config();
    }
    return 1;
}

static EGLBoolean host_eglGetConfigAttrib(EGLDisplay display, EGLConfig config, EGLint attribute,
                                          EGLint *value) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLConfig, EGLint, EGLint *) =
            lookup_egl("eglGetConfigAttrib");
        return native ? native(display, config, attribute, value) : 0;
    }
    if (config != fallback_config() || !value) {
        set_error(config != fallback_config() ? EGL_BAD_CONFIG : EGL_BAD_PARAMETER);
        return 0;
    }
    switch (attribute) {
    case EGL_RED_SIZE:
        *value = g_sdl.red_size;
        break;
    case EGL_GREEN_SIZE:
        *value = g_sdl.green_size;
        break;
    case EGL_BLUE_SIZE:
        *value = g_sdl.blue_size;
        break;
    case EGL_ALPHA_SIZE:
        *value = g_sdl.alpha_size;
        break;
    case EGL_BUFFER_SIZE:
        *value = g_sdl.red_size + g_sdl.green_size + g_sdl.blue_size + g_sdl.alpha_size;
        break;
    case EGL_DEPTH_SIZE:
        *value = g_sdl.depth_size;
        break;
    case EGL_STENCIL_SIZE:
        *value = g_sdl.stencil_size;
        break;
    case EGL_CONFIG_ID:
        *value = 1;
        break;
    case EGL_SURFACE_TYPE:
        *value = EGL_WINDOW_BIT;
        break;
    case EGL_RENDERABLE_TYPE:
        *value = g_sdl.renderable_type;
        break;
    default:
        set_error(EGL_BAD_ATTRIBUTE);
        return 0;
    }
    return 1;
}

static EGLSurface host_eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                              EGLNativeWindowType native_window,
                                              const EGLint *attributes) {
    if (!uses_sdl(display)) {
        EGLSurface (GL_APIENTRY *native)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *) =
            lookup_egl("eglCreateWindowSurface");
        return native ? native(display, config, native_window, attributes) : NULL;
    }
    (void)native_window;
    (void)attributes;
    if (config != fallback_config()) {
        set_error(EGL_BAD_CONFIG);
        return NULL;
    }
    if (!ensure_window()) {
        return NULL;
    }
    g_sdl.surface_valid = 1;
    return (EGLSurface)g_sdl.window;
}

static EGLContext host_eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share,
                                        const EGLint *attributes) {
    if (!uses_sdl(display)) {
        EGLContext (GL_APIENTRY *native)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) =
            lookup_egl("eglCreateContext");
        return native ? native(display, config, share, attributes) : NULL;
    }
    if (config != fallback_config() || share) {
        set_error(config != fallback_config() ? EGL_BAD_CONFIG : EGL_BAD_MATCH);
        return NULL;
    }
    if (attributes) {
        for (size_t i = 0; i < 16 && attributes[i * 2] != EGL_NONE; ++i) {
            if (attributes[i * 2] == EGL_CONTEXT_CLIENT_VERSION) {
                g_sdl.context_major = attributes[i * 2 + 1];
            }
        }
    }
    if (!ensure_context()) {
        return NULL;
    }
    g_sdl.context_valid = 1;
    return (EGLContext)g_sdl.context;
}

static int g_vsync_active;
static int g_swap_interval_applied;

/* BOZ_VSYNC: 1 (default) waits for vertical blank, -1 allows late frames to tear, 0 disables. */
static void apply_swap_interval(void) {
    if (g_swap_interval_applied || !g_sdl.api.GL_SetSwapInterval) {
        return;
    }
    g_swap_interval_applied = 1;
    const char *setting = getenv("BOZ_VSYNC");
    int interval = setting && setting[0] ? atoi(setting) : 1;
    if (interval == -1 && g_sdl.api.GL_SetSwapInterval(-1) != 0) {
        interval = 1;
    }
    if (interval != -1 && g_sdl.api.GL_SetSwapInterval(interval ? 1 : 0) != 0) {
        fprintf(stderr, "[egl] vsync unavailable: %s\n", sdl_error());
        g_vsync_active = 0;
        return;
    }
    g_vsync_active = interval != 0;
    fprintf(stderr, "[egl] vsync %s\n", interval == -1 ? "adaptive" : interval ? "on" : "off");
}

static EGLBoolean host_eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read,
                                      EGLContext context) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) =
            lookup_egl("eglMakeCurrent");
        return native ? native(display, draw, read, context) : 0;
    }
    if (!context) {
        if (draw || read) {
            set_error(EGL_BAD_MATCH);
            return 0;
        }
        if (g_sdl.window && g_sdl.api.GL_MakeCurrent(g_sdl.window, NULL) != 0) {
            fprintf(stderr, "[egl] SDL release-current failed: %s\n", sdl_error());
            set_error(EGL_BAD_CONTEXT);
            return 0;
        }
        g_sdl.current = 0;
        destroy_pending_context();
        return 1;
    }
    if (context != (EGLContext)g_sdl.context || !g_sdl.context_valid) {
        set_error(EGL_BAD_CONTEXT);
        return 0;
    }
    if (draw != (EGLSurface)g_sdl.window || read != (EGLSurface)g_sdl.window ||
        !g_sdl.surface_valid) {
        set_error(EGL_BAD_SURFACE);
        return 0;
    }
    if (g_sdl.api.GL_MakeCurrent(g_sdl.window, g_sdl.context) != 0) {
        fprintf(stderr, "[egl] SDL make-current failed: %s\n", sdl_error());
        set_error(EGL_BAD_CONTEXT);
        return 0;
    }
    g_sdl.current = 1;
    apply_swap_interval();
    return 1;
}

static EGLBoolean host_eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute,
                                       EGLint *value) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLSurface, EGLint, EGLint *) =
            lookup_egl("eglQuerySurface");
        EGLBoolean result = native ? native(display, surface, attribute, value) : 0;
        if (result && value && attribute == EGL_WIDTH) {
            *value = g_surface.width;
        } else if (result && value && attribute == EGL_HEIGHT) {
            *value = g_surface.height;
        }
        return result;
    }
    if (surface != (EGLSurface)g_sdl.window || !g_sdl.surface_valid || !value) {
        set_error(surface != (EGLSurface)g_sdl.window || !g_sdl.surface_valid ? EGL_BAD_SURFACE
                                                                              : EGL_BAD_PARAMETER);
        return 0;
    }
    if (attribute == EGL_WIDTH) {
        *value = g_surface.width;
    } else if (attribute == EGL_HEIGHT) {
        *value = g_surface.height;
    } else {
        set_error(EGL_BAD_ATTRIBUTE);
        return 0;
    }
    return 1;
}

static EGLBoolean host_eglQueryContext(EGLDisplay display, EGLContext context, EGLint attribute,
                                       EGLint *value) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLContext, EGLint, EGLint *) =
            lookup_egl("eglQueryContext");
        return native ? native(display, context, attribute, value) : 0;
    }
    if (context != (EGLContext)g_sdl.context || !g_sdl.context_valid || !value) {
        set_error(context != (EGLContext)g_sdl.context || !g_sdl.context_valid ? EGL_BAD_CONTEXT
                                                                               : EGL_BAD_PARAMETER);
        return 0;
    }
    if (attribute == EGL_CONTEXT_CLIENT_TYPE) {
        *value = EGL_OPENGL_ES_API;
    } else if (attribute == EGL_CONTEXT_CLIENT_VERSION) {
        *value = g_sdl.context_major;
    } else if (attribute == EGL_CONFIG_ID) {
        *value = 1;
    } else {
        set_error(EGL_BAD_ATTRIBUTE);
        return 0;
    }
    return 1;
}

static EGLDisplay host_eglGetCurrentDisplay(void) {
    if (g_sdl.active) {
        return g_sdl.current ? g_sdl.display : NULL;
    }
    EGLDisplay (GL_APIENTRY *native)(void) = lookup_egl("eglGetCurrentDisplay");
    return native ? native() : NULL;
}

static EGLContext host_eglGetCurrentContext(void) {
    if (g_sdl.active) {
        return g_sdl.current ? (EGLContext)g_sdl.context : NULL;
    }
    EGLContext (GL_APIENTRY *native)(void) = lookup_egl("eglGetCurrentContext");
    return native ? native() : NULL;
}

static EGLSurface host_eglGetCurrentSurface(EGLint readdraw) {
    if (g_sdl.active) {
        (void)readdraw;
        return g_sdl.current ? (EGLSurface)g_sdl.window : NULL;
    }
    EGLSurface (GL_APIENTRY *native)(EGLint) = lookup_egl("eglGetCurrentSurface");
    return native ? native(readdraw) : NULL;
}

static const char *host_eglQueryString(EGLDisplay display, EGLint name) {
    if (!uses_sdl(display)) {
        const char *(GL_APIENTRY *native)(EGLDisplay, EGLint) = lookup_egl("eglQueryString");
        return native ? native(display, name) : NULL;
    }
    switch (name) {
    case EGL_VENDOR:
        return "CODBOZ SDL";
    case EGL_VERSION:
        return "1.4";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    case EGL_EXTENSIONS:
        return "";
    default:
        set_error(EGL_BAD_PARAMETER);
        return NULL;
    }
}

static EGLBoolean host_eglBindAPI(GLenum api) {
    if (!g_sdl.active) {
        EGLBoolean (GL_APIENTRY *native)(GLenum) = lookup_egl("eglBindAPI");
        return native ? native(api) : 0;
    }
    if (api != EGL_OPENGL_ES_API) {
        set_error(EGL_BAD_PARAMETER);
        return 0;
    }
    return 1;
}

static EGLBoolean host_eglDestroyContext(EGLDisplay display, EGLContext context) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLContext) = lookup_egl("eglDestroyContext");
        return native ? native(display, context) : 0;
    }
    if (context != (EGLContext)g_sdl.context || !g_sdl.context_valid) {
        set_error(EGL_BAD_CONTEXT);
        return 0;
    }
    g_sdl.context_valid = 0;
    if (g_sdl.current) {
        g_sdl.context_destroy_pending = 1;
    } else {
        g_sdl.api.GL_DeleteContext(g_sdl.context);
        g_sdl.context = NULL;
        g_sdl.context_destroy_pending = 0;
    }
    return 1;
}

static EGLBoolean host_eglDestroySurface(EGLDisplay display, EGLSurface surface) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLSurface) = lookup_egl("eglDestroySurface");
        return native ? native(display, surface) : 0;
    }
    if (surface != (EGLSurface)g_sdl.window || !g_sdl.surface_valid) {
        set_error(EGL_BAD_SURFACE);
        return 0;
    }
    g_sdl.surface_valid = 0;
    return 1;
}

static EGLBoolean host_eglTerminate(EGLDisplay display) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay) = lookup_egl("eglTerminate");
        return native ? native(display) : 0;
    }
    egl_backend_shutdown();
    return 1;
}

static EGLSurface host_eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
                                               const EGLint *attributes) {
    if (!uses_sdl(display)) {
        EGLSurface (GL_APIENTRY *native)(EGLDisplay, EGLConfig, const EGLint *) =
            lookup_egl("eglCreatePbufferSurface");
        return native ? native(display, config, attributes) : NULL;
    }
    (void)config;
    (void)attributes;
    set_error(EGL_BAD_MATCH);
    return NULL;
}

static EGLBoolean host_eglBindTexImage(EGLDisplay display, EGLSurface surface, EGLint buffer) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLSurface, EGLint) = lookup_egl("eglBindTexImage");
        return native ? native(display, surface, buffer) : 0;
    }
    (void)surface;
    (void)buffer;
    set_error(EGL_BAD_MATCH);
    return 0;
}

static EGLBoolean host_eglReleaseTexImage(EGLDisplay display, EGLSurface surface, EGLint buffer) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLSurface, EGLint) = lookup_egl("eglReleaseTexImage");
        return native ? native(display, surface, buffer) : 0;
    }
    (void)surface;
    (void)buffer;
    set_error(EGL_BAD_MATCH);
    return 0;
}

static EGLint host_eglGetError(void) {
    if (!g_sdl.active) {
        EGLint (GL_APIENTRY *native)(void) = lookup_egl("eglGetError");
        return native ? native() : EGL_SUCCESS;
    }
    EGLint error = g_sdl.error;
    g_sdl.error = EGL_SUCCESS;
    return error;
}

EGLBoolean egl_backend_swap_buffers(EGLDisplay display, EGLSurface surface) {
    if (!uses_sdl(display)) {
        EGLBoolean (GL_APIENTRY *native)(EGLDisplay, EGLSurface) = lookup_egl("eglSwapBuffers");
        return native ? native(display, surface) : 0;
    }
    if (surface != (EGLSurface)g_sdl.window || !g_sdl.surface_valid) {
        set_error(EGL_BAD_SURFACE);
        return 0;
    }
    g_sdl.api.GL_SwapWindow(g_sdl.window);
    return 1;
}

uint64_t egl_backend_frame_interval_us(int *vsync) {
    static uint64_t cached_interval;
    static uint64_t next_query_us;
    if (vsync) {
        *vsync = g_vsync_active;
    }
    if (!g_sdl.window || !g_sdl.api.GetWindowDisplayMode) {
        return 0;
    }
    /* The window can move between monitors, so re-read the mode now and then. */
    uint64_t now = monotonic_us();
    if (now >= next_query_us) {
        struct {
            uint32_t format;
            int w;
            int h;
            int refresh_rate;
            void *driverdata;
        } mode = {0};
        next_query_us = now + 1000000u;
        if (g_sdl.api.GetWindowDisplayMode(g_sdl.window, &mode) == 0 && mode.refresh_rate > 0) {
            cached_interval = 1000000u / (uint64_t)mode.refresh_rate;
        }
    }
    return cached_interval;
}

int egl_backend_drawable_size(int *width, int *height) {
    if (!g_sdl.window || !g_sdl.api.GL_GetDrawableSize) {
        return 0;
    }
    g_sdl.api.GL_GetDrawableSize(g_sdl.window, width, height);
    return *width > 0 && *height > 0;
}

void *egl_backend_get_proc_address(const char *name) {
    if (g_sdl.active && g_sdl.api.GL_GetProcAddress) {
        return g_sdl.api.GL_GetProcAddress(name);
    }
    void *(GL_APIENTRY *native)(const char *) = lookup_egl("eglGetProcAddress");
    return native ? native(name) : NULL;
}

void *egl_backend_get_gl_proc(const char *name) {
    if (!g_sdl.active || !g_sdl.api.GL_GetProcAddress) {
        return NULL;
    }
    return g_sdl.api.GL_GetProcAddress(name);
}

struct egl_symbol {
    const char *name;
    void *function;
};

#define EGL_SYMBOL(name) {#name, (void *)(uintptr_t)&host_##name}

void *egl_backend_resolve(const char *name) {
    static const struct egl_symbol symbols[] = {
        EGL_SYMBOL(eglBindAPI),
        EGL_SYMBOL(eglBindTexImage),
        EGL_SYMBOL(eglCreateContext),
        EGL_SYMBOL(eglCreatePbufferSurface),
        EGL_SYMBOL(eglCreateWindowSurface),
        EGL_SYMBOL(eglDestroyContext),
        EGL_SYMBOL(eglDestroySurface),
        EGL_SYMBOL(eglGetConfigAttrib),
        EGL_SYMBOL(eglGetConfigs),
        EGL_SYMBOL(eglGetCurrentContext),
        EGL_SYMBOL(eglGetCurrentDisplay),
        EGL_SYMBOL(eglGetCurrentSurface),
        EGL_SYMBOL(eglGetDisplay),
        EGL_SYMBOL(eglGetError),
        EGL_SYMBOL(eglInitialize),
        EGL_SYMBOL(eglMakeCurrent),
        EGL_SYMBOL(eglQueryContext),
        EGL_SYMBOL(eglQueryString),
        EGL_SYMBOL(eglQuerySurface),
        EGL_SYMBOL(eglReleaseTexImage),
        EGL_SYMBOL(eglTerminate),
    };
    for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
        if (strcmp(name, symbols[i].name) == 0) {
            return symbols[i].function;
        }
    }
    return NULL;
}

void egl_backend_shutdown(void) {
    if (g_sdl.context && g_sdl.api.GL_DeleteContext) {
        g_sdl.api.GL_DeleteContext(g_sdl.context);
    }
    if (g_sdl.window && g_sdl.api.DestroyWindow) {
        g_sdl.api.DestroyWindow(g_sdl.window);
    }
    if (g_sdl.initialized && g_sdl.api.QuitSubSystem) {
        g_sdl.api.QuitSubSystem(SDL_INIT_VIDEO);
    }
    if (g_sdl.library) {
        plat_lib_close(g_sdl.library);
    }
    memset(&g_sdl, 0, sizeof(g_sdl));
    g_sdl.error = EGL_SUCCESS;
    g_sdl.context_major = 2;
    g_sdl.red_size = 8;
    g_sdl.green_size = 8;
    g_sdl.blue_size = 8;
    g_sdl.alpha_size = 8;
    g_sdl.depth_size = 16;
    g_sdl.renderable_type = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT;
}
