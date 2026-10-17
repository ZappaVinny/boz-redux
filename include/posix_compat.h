#ifndef BOZ_POSIX_COMPAT_H
#define BOZ_POSIX_COMPAT_H

#include <fcntl.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)

#include <direct.h>
#include <io.h>

#define mkdir(path, mode) _mkdir(path)
#define fsync(fd) _commit(fd)

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

static inline int unsetenv(const char *name) {
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

static inline int fchmod(int fd, int mode) {
    (void)fd;
    (void)mode;
    return 0;
}

static inline struct tm *localtime_r(const time_t *time_value, struct tm *result) {
    return localtime_s(result, time_value) == 0 ? result : NULL;
}

static inline int set_binary_mode(int fd) {
    return _setmode(fd, _O_BINARY) < 0 ? -1 : 0;
}

#else

#ifndef O_BINARY
#define O_BINARY 0
#endif

static inline int set_binary_mode(int fd) {
    (void)fd;
    return 0;
}

#endif

#endif
