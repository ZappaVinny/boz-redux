#ifndef BOZ_SOCKET_COMPAT_H
#define BOZ_SOCKET_COMPAT_H

#include <errno.h>

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#undef CreateWindow
#undef ERROR
#undef DELETE

typedef ULONG nfds_t;

#define poll(fds, count, timeout) WSAPoll((fds), (ULONG)(count), (timeout))
#define setsockopt(fd, level, name, value, length)                                                 \
    setsockopt((SOCKET)(fd), (level), (name), (const char *)(value), (int)(length))
#define getsockopt(fd, level, name, value, length)                                                 \
    getsockopt((SOCKET)(fd), (level), (name), (char *)(value), (length))
#define send(fd, buffer, length, flags)                                                            \
    send((SOCKET)(fd), (const char *)(buffer), (int)(length), (flags))
#define recv(fd, buffer, length, flags) recv((SOCKET)(fd), (char *)(buffer), (int)(length), (flags))
#define sendto(fd, buffer, length, flags, target, target_length)                                   \
    sendto((SOCKET)(fd), (const char *)(buffer), (int)(length), (flags), (target), (target_length))
#define recvfrom(fd, buffer, length, flags, source, source_length)                                 \
    recvfrom((SOCKET)(fd), (char *)(buffer), (int)(length), (flags), (source), (source_length))

static inline int socket_translate_error(int error) {
    switch (error) {
    case WSAEWOULDBLOCK:
        return EWOULDBLOCK;
    case WSAEINPROGRESS:
        return EINPROGRESS;
    case WSAEALREADY:
        return EALREADY;
    case WSAENOTSOCK:
        return ENOTSOCK;
    case WSAEMSGSIZE:
        return EMSGSIZE;
    case WSAEAFNOSUPPORT:
        return EAFNOSUPPORT;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case WSAENETDOWN:
        return ENETDOWN;
    case WSAENETUNREACH:
        return ENETUNREACH;
    case WSAECONNABORTED:
        return ECONNABORTED;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAEISCONN:
        return EISCONN;
    case WSAENOTCONN:
        return ENOTCONN;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAEHOSTUNREACH:
        return EHOSTUNREACH;
    case WSAEMFILE:
        return EMFILE;
    case WSAEACCES:
        return EACCES;
    case WSAEINVAL:
        return EINVAL;
    case WSAEINTR:
        return EINTR;
    default:
        return error ? EINVAL : 0;
    }
}

static inline int socket_errno(void) {
    return socket_translate_error(WSAGetLastError());
}

static inline int socket_close(int fd) {
    return closesocket((SOCKET)fd);
}

static inline int socket_set_nonblocking(int fd, int enabled) {
    u_long mode = enabled ? 1u : 0u;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
}

static inline int socket_set_cloexec(int fd) {
    (void)fd;
    return 0;
}

#else

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static inline int socket_translate_error(int error) {
    return error;
}

static inline int socket_errno(void) {
    return errno;
}

static inline int socket_close(int fd) {
    return close(fd);
}

static inline int socket_set_nonblocking(int fd, int enabled) {
    return ioctl(fd, FIONBIO, &enabled);
}

static inline int socket_set_cloexec(int fd) {
    return ioctl(fd, FIOCLEX);
}

#endif

#endif
