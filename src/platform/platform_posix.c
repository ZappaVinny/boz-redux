#include "platform.h"

#include <dlfcn.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

#if defined(__i386__)
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

enum { PROFILE_SLOTS = 4096, PROFILE_TOP = 25 };
static volatile uintptr_t g_profile_address[PROFILE_SLOTS];
static volatile unsigned g_profile_count[PROFILE_SLOTS];
static volatile unsigned g_profile_samples;
static volatile uint32_t g_guest_address[PROFILE_SLOTS];
static volatile unsigned g_guest_count[PROFILE_SLOTS];

static void profile_signal(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    uintptr_t key = (uintptr_t)((ucontext_t *)context)->uc_mcontext.gregs[REG_EIP] & ~(uintptr_t)0xf;
    unsigned index = (unsigned)((key >> 4) * 2654435761u) % PROFILE_SLOTS;
    for (unsigned probe = 0; probe < 64; ++probe) {
        unsigned slot = (index + probe) % PROFILE_SLOTS;
        if (g_profile_address[slot] == key || g_profile_address[slot] == 0) {
            g_profile_address[slot] = key;
            g_profile_count[slot]++;
            break;
        }
    }
    g_profile_samples++;
    uint32_t guest = plat_profile_guest_pc ? plat_profile_guest_pc() & ~(uint32_t)0x3f : 0;
    if (guest) {
        unsigned slot = (unsigned)((guest >> 6) * 2654435761u) % PROFILE_SLOTS;
        for (unsigned probe = 0; probe < 64; ++probe, slot = (slot + 1) % PROFILE_SLOTS) {
            if (g_guest_address[slot] == guest || g_guest_address[slot] == 0) {
                g_guest_address[slot] = guest;
                g_guest_count[slot]++;
                break;
            }
        }
    }
}

static void *profile_report_thread(void *unused) {
    (void)unused;
    for (;;) {
        sleep(5);
        unsigned samples = g_profile_samples;
        fprintf(stderr, "[profile] %u samples\n", samples);
        /* BOZ_PROFILE=all: every sampled address as module+offset, for offline symbolising. */
        if (!strcmp(getenv("BOZ_PROFILE"), "all")) {
            for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
                if (!g_profile_count[i]) {
                    continue;
                }
                Dl_info dl;
                uintptr_t address = g_profile_address[i];
                if (dladdr((void *)address, &dl) && dl.dli_fname) {
                    fprintf(stderr, "[profile-all] %u %s+0x%lx\n", g_profile_count[i],
                            dl.dli_fname, (unsigned long)(address - (uintptr_t)dl.dli_fbase));
                } else {
                    fprintf(stderr, "[profile-all] %u anonymous+0x%lx\n", g_profile_count[i],
                            (unsigned long)address);
                }
                g_profile_count[i] = 0;
            }
            for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
                if (g_guest_count[i]) {
                    fprintf(stderr, "[profile-guest] %u 0x%08x\n", g_guest_count[i],
                            g_guest_address[i]);
                    g_guest_count[i] = 0;
                    g_guest_address[i] = 0;
                }
            }
        }
        for (unsigned rank = 0; rank < PROFILE_TOP; ++rank) {
            unsigned best = 0, best_slot = 0;
            for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
                if (g_profile_count[i] > best) {
                    best = g_profile_count[i];
                    best_slot = i;
                }
            }
            if (!best) {
                break;
            }
            Dl_info dl;
            const char *name = "?";
            if (dladdr((void *)g_profile_address[best_slot], &dl) && dl.dli_sname) {
                name = dl.dli_sname;
            } else if (dladdr((void *)g_profile_address[best_slot], &dl) && dl.dli_fname) {
                name = strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname;
            }
            fprintf(stderr, "[profile]   %5u  %s (%p)\n", best, name,
                    (void *)g_profile_address[best_slot]);
            g_profile_count[best_slot] = 0;
        }
        for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
            g_profile_count[i] = 0;
            g_profile_address[i] = 0;
        }
        g_profile_samples = 0;
    }
    return NULL;
}
#endif

uint32_t (*plat_profile_guest_pc)(void);

void plat_reinstall_crash_handler(void) {}

bool plat_init(void) {
#if defined(__i386__)
    if (getenv("BOZ_PROFILE")) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = profile_signal;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &action, NULL);
        struct itimerval timer = {{0, 1000}, {0, 1000}};
        setitimer(ITIMER_PROF, &timer, NULL);
        pthread_t thread;
        pthread_create(&thread, NULL, profile_report_thread, NULL);
        pthread_detach(thread);
    }
#endif
    return true;
}

void plat_sleep_us(uint64_t microseconds) {
    struct timespec request = {
        .tv_sec = (time_t)(microseconds / 1000000u),
        .tv_nsec = (long)(microseconds % 1000000u) * 1000L,
    };
    while (nanosleep(&request, &request) != 0 && errno == EINTR) {
    }
}

void *plat_lib_open(const char *name) {
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

void *plat_lib_symbol(void *library, const char *name) {
    return dlsym(library, name);
}

int plat_lib_close(void *library) {
    return dlclose(library);
}

const char *plat_lib_error(void) {
    return dlerror();
}

bool plat_lib_path(const void *address, char *out, size_t out_size) {
    Dl_info info;
    if (!address || dladdr(address, &info) == 0 || !info.dli_fname) {
        return false;
    }
    snprintf(out, out_size, "%s", info.dli_fname);
    return true;
}

bool plat_address_in_module(const void *address) {
    Dl_info info;
    return dladdr(address, &info) != 0 && info.dli_fbase != NULL;
}

const char *plat_symbol_name(const void *address) {
    Dl_info info;
    return dladdr(address, &info) != 0 ? info.dli_sname : NULL;
}

#if UINTPTR_MAX > 0xffffffffu && defined(__linux__)
/* 64-bit: memory the game sees has to sit below 4 GiB. Takes the highest gap below 4 GiB that
 * fits, from the process's mappings, so blocks stay clear of the game image's fixed address
 * (0x4a000000) whatever the order of allocations; MAP_FIXED_NOREPLACE claims it (if another
 * thread took it first, the next gap is tried). MAP_32BIT only reaches the low 2 GiB and does not
 * exist on AArch64. */
static void *alloc_low(size_t size, int prot) {
    const uint64_t floor = 0x10000000u, ceiling = (uint64_t)1 << 32, align = 0x10000u;
    enum { MAX_GAPS = 4096 };
    static uint64_t gap_lo[MAX_GAPS], gap_hi[MAX_GAPS];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        pthread_mutex_unlock(&lock);
        return MAP_FAILED;
    }
    int gaps = 0;
    uint64_t gap_start = floor;
    char line[512];
    while (gaps < MAX_GAPS && gap_start < ceiling) {
        unsigned long long start = ceiling, end = ceiling;
        if (!fgets(line, sizeof(line), maps) || sscanf(line, "%llx-%llx", &start, &end) != 2) {
            start = end = ceiling;
        }
        uint64_t hi = start < ceiling ? start : ceiling;
        if (hi > gap_start) {
            gap_lo[gaps] = gap_start;
            gap_hi[gaps] = hi;
            gaps++;
        }
        if (end > gap_start) {
            gap_start = end;
        }
    }
    fclose(maps);
    void *address = MAP_FAILED;
    for (int i = gaps - 1; i >= 0 && address == MAP_FAILED; --i) {
        if (gap_hi[i] - gap_lo[i] < size + align) {
            continue;
        }
        uint64_t at = (gap_hi[i] - size) & ~(align - 1);
        void *got = mmap((void *)(uintptr_t)at, size, prot,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (got != MAP_FAILED && got != (void *)(uintptr_t)at) {
            munmap(got, size); /* an old kernel ignored the flag and moved it */
        } else {
            address = got;
        }
    }
    pthread_mutex_unlock(&lock);
    return address;
}
#endif

void *plat_alloc(void *want, size_t size, bool executable) {
    int prot = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
    void *address = MAP_FAILED;
    if (want) {
        address = mmap(want, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (address != MAP_FAILED && address != want) {
            munmap(address, size);
            address = MAP_FAILED;
        }
    }
    if (address == MAP_FAILED) {
#if UINTPTR_MAX > 0xffffffffu && defined(__linux__)
        address = alloc_low(size, prot);
#if defined(MAP_32BIT)
        if (address == MAP_FAILED) {
            address = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        }
#endif
#elif UINTPTR_MAX > 0xffffffffu && defined(MAP_32BIT)
        /* 64-bit: the game sees this memory, so it has to sit below 4 GiB. */
        address = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
        address = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    }
    return address == MAP_FAILED ? NULL : address;
}

void plat_free(void *address, size_t size) {
    if (address) {
        munmap(address, size);
    }
}

void plat_discard(void *address, size_t size) {
    if (address && size) {
        madvise(address, size, MADV_DONTNEED); /* private anonymous memory: zero-filled again */
    }
}

size_t plat_page_size(void) {
    long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? (size_t)page : 4096;
}

bool plat_region(uintptr_t address, uint64_t *start, uint64_t *end) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        return false;
    }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && address >= lo && address < hi) {
            *start = lo;
            *end = hi;
            found = true;
            break;
        }
    }
    fclose(maps);
    return found;
}

FILE *plat_memfile(const void *buffer, size_t size) {
    return fmemopen((void *)buffer, size, "rb");
}

bool plat_random_bytes(void *buffer, size_t size) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    uint8_t *out = buffer;
    size_t done = 0;
    while (done < size) {
        ssize_t got = read(fd, out + done, size - done);
        if (got <= 0) {
            close(fd);
            return false;
        }
        done += (size_t)got;
    }
    close(fd);
    return true;
}

int plat_rename_replace(const char *from, const char *to) {
    return rename(from, to);
}

bool plat_data_window(uintptr_t address, uint64_t *start, uint64_t *end) {
    if (!getenv("BOZ_WINDOW_TEST")) {
        return plat_region(address, start, end);
    }
    uint64_t window_lo = (uint64_t)address & ~(uint64_t)(64u * 1024u * 1024u - 1);
    uint64_t window_hi = window_lo + 64u * 1024u * 1024u;
    if (window_lo < 0x10000u) {
        window_lo = 0x10000u;
    }
    uint64_t lo = window_lo, hi = window_hi;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long a, b;
        char perms[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perms) != 3 || perms[2] != 'x') {
            continue;
        }
        if (b <= address && b > lo) {
            lo = b;
        }
        if (a > address && a < hi) {
            hi = a;
        }
    }
    fclose(maps);
    *start = lo;
    *end = hi;
    return hi > lo;
}
