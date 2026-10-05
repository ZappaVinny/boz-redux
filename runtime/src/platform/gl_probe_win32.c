/* Mesa driver selection on Windows.
 *
 * Mesa picks its Gallium driver (d3d12, llvmpipe, ...) once per process from GALLIUM_DRIVER, and
 * some drivers initialise but cannot present to a window (d3d12 in VMs without a GPU). Before the
 * game loads OpenGL, the loader starts a copy of itself per candidate driver; each copy opens a
 * window through Mesa's opengl32.dll exactly like the game does (WGL pixel format, then an
 * OpenGL ES 1.1 context from WGL_EXT_create_context_es_profile) and presents one frame. The
 * first driver that works is exported for the game process.
 */
#include "gl_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_ES_PROFILE_BIT_EXT 0x0004
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

enum { PROBE_TIMEOUT_MS = 15000 };

struct wgl_api {
    int(__stdcall *ChoosePixelFormat)(HDC, const PIXELFORMATDESCRIPTOR *);
    BOOL(__stdcall *SetPixelFormat)(HDC, int, const PIXELFORMATDESCRIPTOR *);
    HGLRC(__stdcall *CreateContext)(HDC);
    BOOL(__stdcall *MakeCurrent)(HDC, HGLRC);
    BOOL(__stdcall *DeleteContext)(HGLRC);
    BOOL(__stdcall *SwapBuffers)(HDC);
    PROC(__stdcall *GetProcAddress)(LPCSTR);
};

static int exe_directory(char *path, size_t size) {
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)size);
    if (!length || length >= size) {
        return 0;
    }
    char *slash = strrchr(path, '\\');
    if (!slash) {
        return 0;
    }
    slash[1] = '\0';
    return 1;
}

/* Mesa lives in mesa\ next to the executable; its DLLs load each other from there. */
static void use_mesa_directory(void) {
    char path[MAX_PATH];
    if (exe_directory(path, sizeof(path)) && strlen(path) + sizeof("mesa") <= sizeof(path)) {
        strcat(path, "mesa");
        SetDllDirectoryA(path);
    }
}

static int load_wgl(struct wgl_api *wgl, HMODULE *module_out) {
    char path[MAX_PATH];
    use_mesa_directory();
    if (!exe_directory(path, sizeof(path)) ||
        strlen(path) + sizeof("mesa\\opengl32.dll") > sizeof(path)) {
        return 0;
    }
    strcat(path, "mesa\\opengl32.dll");
    HMODULE module = LoadLibraryA(path);
    if (!module) {
        fprintf(stderr, "[gl-probe] cannot load %s (error %lu)\n", path, GetLastError());
        return 0;
    }
#define LOAD(field, name)                                                                          \
    if (!(*(FARPROC *)&wgl->field = GetProcAddress(module, name))) {                              \
        fprintf(stderr, "[gl-probe] opengl32.dll is missing %s\n", name);                         \
        return 0;                                                                                  \
    }
    LOAD(ChoosePixelFormat, "wglChoosePixelFormat");
    LOAD(SetPixelFormat, "wglSetPixelFormat");
    LOAD(CreateContext, "wglCreateContext");
    LOAD(MakeCurrent, "wglMakeCurrent");
    LOAD(DeleteContext, "wglDeleteContext");
    LOAD(SwapBuffers, "wglSwapBuffers");
    LOAD(GetProcAddress, "wglGetProcAddress");
#undef LOAD
    *module_out = module;
    return 1;
}

static HWND create_test_window(void) {
    WNDCLASSA window_class = {
        .style = CS_OWNDC,
        .lpfnWndProc = DefWindowProcA,
        .hInstance = GetModuleHandleA(NULL),
        .lpszClassName = "BozGlProbe",
    };
    RegisterClassA(&window_class);
    return CreateWindowExA(0, window_class.lpszClassName, "BOZ GL probe", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 320, 180, NULL, NULL,
                           window_class.hInstance, NULL);
}

static void *gl_proc(const struct wgl_api *wgl, HMODULE module, const char *name) {
    void *address = (void *)wgl->GetProcAddress(name);
    return address ? address : (void *)GetProcAddress(module, name);
}

int gl_probe_run(const char *driver) {
    const char *label = driver && driver[0] ? driver : "default";
    struct wgl_api wgl;
    HMODULE module;
    if (!load_wgl(&wgl, &module)) {
        return 2;
    }
    HWND window = create_test_window();
    HDC dc = window ? GetDC(window) : NULL;
    if (!dc) {
        fprintf(stderr, "[gl-probe] %s: CreateWindow failed (%lu)\n", label, GetLastError());
        return 3;
    }
    PIXELFORMATDESCRIPTOR format = {
        .nSize = sizeof(format),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 24,
        .cAlphaBits = 8,
        .cDepthBits = 16,
        .iLayerType = PFD_MAIN_PLANE,
    };
    int index = wgl.ChoosePixelFormat(dc, &format);
    if (!index || !wgl.SetPixelFormat(dc, index, &format)) {
        fprintf(stderr, "[gl-probe] %s: pixel format failed (index=%d error=%lu)\n", label, index,
                GetLastError());
        return 4;
    }
    HGLRC bootstrap = wgl.CreateContext(dc);
    if (!bootstrap || !wgl.MakeCurrent(dc, bootstrap)) {
        fprintf(stderr, "[gl-probe] %s: desktop context failed (%lu)\n", label, GetLastError());
        return 5;
    }
    HGLRC(__stdcall * create_attribs)(HDC, HGLRC, const int *) =
        (void *)wgl.GetProcAddress("wglCreateContextAttribsARB");
    static const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 1,
                                     WGL_CONTEXT_MINOR_VERSION_ARB, 1,
                                     WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_ES_PROFILE_BIT_EXT,
                                     0};
    HGLRC context = create_attribs ? create_attribs(dc, NULL, attributes) : NULL;
    wgl.MakeCurrent(dc, NULL);
    wgl.DeleteContext(bootstrap);
    if (!context || !wgl.MakeCurrent(dc, context)) {
        fprintf(stderr, "[gl-probe] %s: OpenGL ES 1.1 context failed (%lu)\n", label,
                GetLastError());
        return 6;
    }
    void(__stdcall * clear_color)(float, float, float, float) =
        gl_proc(&wgl, module, "glClearColor");
    void(__stdcall * clear)(unsigned int) = gl_proc(&wgl, module, "glClear");
    const char *(__stdcall * get_string)(unsigned int) = gl_proc(&wgl, module, "glGetString");
    if (clear_color && clear) {
        clear_color(0.0f, 0.0f, 0.0f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
    }
    int status = 0;
    if (!wgl.SwapBuffers(dc)) {
        fprintf(stderr, "[gl-probe] %s: SwapBuffers failed (%lu)\n", label, GetLastError());
        status = 7;
    } else {
        const char *version = get_string ? get_string(GL_VERSION) : NULL;
        const char *renderer = get_string ? get_string(GL_RENDERER) : NULL;
        fprintf(stderr, "[gl-probe] %s: OK, %s on %s\n", label, version ? version : "?",
                renderer ? renderer : "?");
    }
    wgl.MakeCurrent(dc, NULL);
    wgl.DeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return status;
}

static int probe_in_child(const char *driver) {
    char exe[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (!length || length >= sizeof(exe)) {
        return 0;
    }
    char command[MAX_PATH + 64];
    snprintf(command, sizeof(command), "\"%s\" --gl-probe %s", exe, driver);

    /* The child inherits the environment, so set the driver around CreateProcess. */
    char previous[64] = "";
    DWORD had_previous = GetEnvironmentVariableA("GALLIUM_DRIVER", previous, sizeof(previous));
    SetEnvironmentVariableA("GALLIUM_DRIVER", driver);

    STARTUPINFOA startup = {.cb = sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process;
    fflush(stdout);
    fflush(stderr);
    BOOL started =
        CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    SetEnvironmentVariableA("GALLIUM_DRIVER",
                            had_previous && had_previous < sizeof(previous) ? previous : NULL);
    if (!started) {
        fprintf(stderr, "[gl-probe] cannot start probe for %s (%lu)\n", driver, GetLastError());
        return 0;
    }
    DWORD exit_code = 1;
    if (WaitForSingleObject(process.hProcess, PROBE_TIMEOUT_MS) == WAIT_TIMEOUT) {
        fprintf(stderr, "[gl-probe] %s: timed out\n", driver);
        TerminateProcess(process.hProcess, 1);
    } else {
        GetExitCodeProcess(process.hProcess, &exit_code);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

void gl_probe_select_driver(void) {
    const char *renderer = getenv("BOZ_RENDERER");
    if (!renderer || strcmp(renderer, "mesa") != 0) {
        return; /* ANGLE: no driver to pick */
    }
    use_mesa_directory();
    const char *configured = getenv("GALLIUM_DRIVER");
    if (configured && configured[0]) {
        fprintf(stderr, "[gl-probe] GALLIUM_DRIVER=%s set by the user\n", configured);
        return;
    }
    static const char *const candidates[] = {"d3d12", "llvmpipe", "softpipe"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (probe_in_child(candidates[i])) {
            _putenv_s("GALLIUM_DRIVER", candidates[i]);
            SetEnvironmentVariableA("GALLIUM_DRIVER", candidates[i]);
            fprintf(stderr, "[gl-probe] using Mesa driver %s\n", candidates[i]);
            return;
        }
    }
    fprintf(stderr, "[gl-probe] no Mesa driver could open a window; continuing with defaults\n");
}
