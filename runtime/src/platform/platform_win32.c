#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_last_error[256];
static LARGE_INTEGER g_counter_frequency;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static uint64_t counter_us(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / g_counter_frequency.QuadPart) * 1000000u +
           (uint64_t)(now.QuadPart % g_counter_frequency.QuadPart) * 1000000u /
               (uint64_t)g_counter_frequency.QuadPart;
}

void plat_sleep_us(uint64_t microseconds) {
    static __thread HANDLE timer;
    if (!g_counter_frequency.QuadPart) {
        QueryPerformanceFrequency(&g_counter_frequency);
    }
    uint64_t deadline = counter_us() + microseconds;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    if (timer && microseconds > 200) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((microseconds - 100) * 10u);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        }
    }
    while (counter_us() < deadline) {
        SwitchToThread();
    }
}

static LONG WINAPI report_crash(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const void *address = record->ExceptionAddress;
    char module_path[MAX_PATH] = "?";
    HMODULE module = NULL;
    uintptr_t offset = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module)) {
        GetModuleFileNameA(module, module_path, sizeof(module_path));
        offset = (uintptr_t)address - (uintptr_t)module;
    }
    fprintf(stderr, "[crash] exception 0x%08lx at %p (%s+0x%lx)\n",
            (unsigned long)record->ExceptionCode, address, module_path, (unsigned long)offset);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        fprintf(stderr, "[crash] %s address %p\n",
                record->ExceptionInformation[0] == 1   ? "write to"
                : record->ExceptionInformation[0] == 8 ? "execute at"
                                                       : "read from",
                (void *)record->ExceptionInformation[1]);
    }
    const CONTEXT *context = info->ContextRecord;
    fprintf(stderr, "[crash] eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx\n",
            context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi,
            context->Esp, context->Ebp);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* First-chance exceptions that usually end the process, logged before any other handler (SDL,
 * Mesa or the C runtime may replace the unhandled-exception filter above). Some may be handled
 * later, so only the first few are logged. */
static LONG WINAPI log_first_chance(EXCEPTION_POINTERS *info) {
    static volatile LONG logged;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != 0xc0000409u /* fail fast */ &&
        code != 0xc0000374u /* heap corruption */) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    /* Unicorn grows its code buffer by catching writes at the next 64 KB-aligned uncommitted
     * page (patches/unicorn-win32-codegen-commit-tail.patch); those are expected. */
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2 &&
        info->ExceptionRecord->ExceptionInformation[0] == 1 &&
        (info->ExceptionRecord->ExceptionInformation[1] & 0xffff) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&logged) <= 20) {
        fprintf(stderr, "[crash] first chance (thread %lu): ", (unsigned long)GetCurrentThreadId());
        report_crash(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* SDL or Mesa may install their own unhandled-exception filter; put ours back once they are up. */
void plat_reinstall_crash_handler(void) {
    SetUnhandledExceptionFilter(report_crash);
}

static void log_abort(int sig) {
    (void)sig;
    fprintf(stderr, "[crash] abort() called\n");
    fflush(stderr);
}

static void log_exit(void) {
    fprintf(stderr, "[platform] process exiting\n");
    fflush(stderr);
}

enum { PROFILE_SLOTS = 4096, PROFILE_TOP = 25 };

struct profile_slot {
    uintptr_t address;
    unsigned count;
};

static HANDLE g_profile_target;
static struct profile_slot g_profile[PROFILE_SLOTS];
static unsigned g_profile_samples;

static void profile_record(uintptr_t address) {
    uintptr_t key = address & ~(uintptr_t)0xf;
    unsigned index = (unsigned)((key >> 4) * 2654435761u) % PROFILE_SLOTS;
    for (unsigned probe = 0; probe < PROFILE_SLOTS; ++probe) {
        struct profile_slot *slot = &g_profile[(index + probe) % PROFILE_SLOTS];
        if (slot->address == key || slot->address == 0) {
            slot->address = key;
            slot->count++;
            return;
        }
    }
}

static int profile_compare(const void *a, const void *b) {
    const struct profile_slot *x = a, *y = b;
    return (int)y->count - (int)x->count;
}

static void profile_report(void) {
    static struct profile_slot sorted[PROFILE_SLOTS];
    memcpy(sorted, g_profile, sizeof(sorted));
    qsort(sorted, PROFILE_SLOTS, sizeof(sorted[0]), profile_compare);
    unsigned exe = 0, other = 0, jit = 0;
    for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
        if (!sorted[i].count) {
            continue;
        }
        HMODULE module = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)sorted[i].address, &module)) {
            if (module == GetModuleHandleA(NULL)) {
                exe += sorted[i].count;
            } else {
                other += sorted[i].count;
            }
        } else {
            jit += sorted[i].count;
        }
    }
    fprintf(stderr, "[profile] %u samples: exe %u%%, other modules %u%%, jit/anon %u%%\n",
            g_profile_samples, g_profile_samples ? exe * 100 / g_profile_samples : 0,
            g_profile_samples ? other * 100 / g_profile_samples : 0,
            g_profile_samples ? jit * 100 / g_profile_samples : 0);
    for (unsigned i = 0; i < PROFILE_TOP && sorted[i].count; ++i) {
        char path[MAX_PATH] = "jit/anon";
        HMODULE module = NULL;
        uintptr_t offset = sorted[i].address;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)sorted[i].address, &module)) {
            GetModuleFileNameA(module, path, sizeof(path));
            offset = sorted[i].address - (uintptr_t)module;
        }
        const char *name = strrchr(path, '\\');
        fprintf(stderr, "[profile]   %5u  %s+0x%lx\n", sorted[i].count, name ? name + 1 : path,
                (unsigned long)offset);
    }
    memset(g_profile, 0, sizeof(g_profile));
    g_profile_samples = 0;
}

static DWORD WINAPI profile_thread(LPVOID unused) {
    (void)unused;
    DWORD last_report = GetTickCount();
    for (;;) {
        Sleep(1);
        if (SuspendThread(g_profile_target) == (DWORD)-1) {
            continue;
        }
        CONTEXT context;
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_profile_target, &context)) {
            profile_record(context.Eip);
            g_profile_samples++;
        }
        ResumeThread(g_profile_target);
        if (GetTickCount() - last_report >= 5000) {
            profile_report();
            last_report = GetTickCount();
        }
    }
    return 0;
}

bool plat_init(void) {
    SetUnhandledExceptionFilter(report_crash);
    AddVectoredExceptionHandler(1, log_first_chance);
    signal(SIGABRT, log_abort);
    atexit(log_exit);
    if (getenv("BOZ_PROFILE") &&
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &g_profile_target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
        CreateThread(NULL, 0, profile_thread, NULL, 0, NULL);
    }
    fprintf(stderr, "[platform] loader base %p\n", (void *)GetModuleHandleA(NULL));
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    timeBeginPeriod(1);
    return true;
}

void *plat_lib_open(const char *name) {
    HMODULE module = LoadLibraryA(name);
    if (!module) {
        DWORD code = GetLastError();
        int length = snprintf(g_last_error, sizeof(g_last_error), "%s: ", name);
        if (length < 0 || (size_t)length >= sizeof(g_last_error)) {
            length = 0;
        }
        if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                            g_last_error + length, (DWORD)(sizeof(g_last_error) - (size_t)length),
                            NULL)) {
            snprintf(g_last_error + length, sizeof(g_last_error) - (size_t)length, "error %lu",
                     (unsigned long)code);
        }
    }
    return (void *)module;
}

void *plat_lib_symbol(void *library, const char *name) {
    return library ? (void *)(uintptr_t)GetProcAddress((HMODULE)library, name) : NULL;
}

int plat_lib_close(void *library) {
    return library && FreeLibrary((HMODULE)library) ? 0 : -1;
}

const char *plat_lib_error(void) {
    if (!g_last_error[0]) {
        return NULL;
    }
    static char reported[sizeof(g_last_error)];
    memcpy(reported, g_last_error, sizeof(reported));
    g_last_error[0] = 0;
    return reported;
}

static HMODULE module_at(const void *address) {
    HMODULE module = NULL;
    if (!address ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &module)) {
        return NULL;
    }
    return module;
}

bool plat_lib_path(const void *address, char *out, size_t out_size) {
    HMODULE module = module_at(address);
    if (!module || out_size == 0) {
        return false;
    }
    DWORD length = GetModuleFileNameA(module, out, (DWORD)out_size);
    return length > 0 && length < out_size;
}

bool plat_address_in_module(const void *address) {
    return module_at(address) != NULL;
}

const char *plat_symbol_name(const void *address) {
    (void)address;
    return NULL;
}

void *plat_alloc(void *want, size_t size, bool executable) {
    DWORD protect = executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    void *address = NULL;
    if (want) {
        address = VirtualAlloc(want, size, MEM_RESERVE | MEM_COMMIT, protect);
    }
    if (!address) {
        address = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, protect);
    }
    return address;
}

void plat_free(void *address, size_t size) {
    (void)size;
    if (address) {
        VirtualFree(address, 0, MEM_RELEASE);
    }
}

size_t plat_page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize ? info.dwPageSize : 4096;
}

bool plat_region(uintptr_t address, uint64_t *start, uint64_t *end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((LPCVOID)address, &info, sizeof(info)) || info.State != MEM_COMMIT) {
        return false;
    }
    uint64_t lo = (uintptr_t)info.BaseAddress;
    uint64_t hi = lo + info.RegionSize;
    *start = lo;
    *end = hi ? hi : (uint64_t)1 << 32;
    return true;
}

FILE *plat_memfile(const void *buffer, size_t size) {
    char directory[MAX_PATH];
    char path[MAX_PATH];
    if (!GetTempPathA(sizeof(directory), directory) ||
        !GetTempFileNameA(directory, "boz", 0, path)) {
        return NULL;
    }
    HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DeleteFileA(path);
        return NULL;
    }
    const uint8_t *data = buffer;
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done);
        DWORD written = 0;
        if (!WriteFile(handle, data + done, chunk, &written, NULL) || written == 0) {
            CloseHandle(handle);
            return NULL;
        }
        done += written;
    }
    SetFilePointer(handle, 0, NULL, FILE_BEGIN);
    int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return NULL;
    }
    FILE *file = _fdopen(fd, "rb");
    if (!file) {
        _close(fd);
    }
    return file;
}

bool plat_random_bytes(void *buffer, size_t size) {
    return BCryptGenRandom(NULL, buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

int plat_rename_replace(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

enum { DATA_WINDOW_SIZE = 64u * 1024u * 1024u };

bool plat_data_window(uintptr_t address, uint64_t *start, uint64_t *end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((LPCVOID)address, &info, sizeof(info))) {
        return false;
    }
    if (info.Type == MEM_IMAGE) {
        return plat_region(address, start, end);
    }
    uint64_t window_lo = (uint64_t)address & ~(uint64_t)(DATA_WINDOW_SIZE - 1);
    uint64_t window_hi = window_lo + DATA_WINDOW_SIZE;
    if (window_lo < 0x10000u) {
        window_lo = 0x10000u;
    }
    uint64_t lo = (uintptr_t)info.BaseAddress;
    uint64_t hi = lo + info.RegionSize;
    if (lo < window_lo) {
        lo = window_lo;
    }
    if (hi > window_hi) {
        hi = window_hi;
    }
    while (lo > window_lo) {
        MEMORY_BASIC_INFORMATION before;
        if (!VirtualQuery((LPCVOID)(uintptr_t)(lo - 1), &before, sizeof(before)) ||
            before.Type == MEM_IMAGE) {
            break;
        }
        uint64_t base = (uintptr_t)before.BaseAddress;
        lo = base > window_lo ? base : window_lo;
    }
    while (hi < window_hi) {
        MEMORY_BASIC_INFORMATION after;
        if (!VirtualQuery((LPCVOID)(uintptr_t)hi, &after, sizeof(after)) || after.Type == MEM_IMAGE) {
            break;
        }
        uint64_t next = (uintptr_t)after.BaseAddress + after.RegionSize;
        hi = next < window_hi ? next : window_hi;
    }
    *start = lo;
    *end = hi;
    return hi > lo;
}
