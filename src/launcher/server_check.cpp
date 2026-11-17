#include "server_check.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <algorithm>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
#define BAD_SOCKET INVALID_SOCKET
static void close_socket(Socket s) { closesocket(s); }
#else
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
typedef int Socket;
#define BAD_SOCKET (-1)
static void close_socket(Socket s) { close(s); }
#endif

// A host that accepts and then drops the connection must not kill the launcher with SIGPIPE.
#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

namespace {

const char *const LOBBY_PORT = "3074";
const char *const STUN_PORT = "3478";
const int TIMEOUT_MS = 4000;

#if defined(_WIN32)
struct Winsock {
    bool ok;
    Winsock() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Winsock() {
        if (ok) {
            WSACleanup();
        }
    }
};
#endif

void set_blocking(Socket s, bool blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

// Waits until the socket can be read (or written); false on a timeout.
bool wait_for(Socket s, bool write, int ms) {
#if defined(_WIN32)
    WSAPOLLFD fd = {s, (SHORT)(write ? POLLWRNORM : POLLRDNORM), 0};
    return WSAPoll(&fd, 1, ms) > 0;
#else
    pollfd fd = {s, (short)(write ? POLLOUT : POLLIN), 0};
    return poll(&fd, 1, ms) > 0;
#endif
}

int milliseconds_left(std::chrono::steady_clock::time_point deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return left.count() > 0 ? (int)left.count() : 0;
}

addrinfo *resolve(const std::string &host, const char *port, int type) {
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), port, &hints, &found) != 0) {
        return nullptr;
    }
    return found;
}

enum class Lobby { MISSING, NOT_BOZ, BOZ };

// TCP 3074: ask for an account's lobby ticket (AccountForMMP, title 18436: Black Ops Zombies) for
// an account that does not exist. A BOZ server only looks it up and answers with an
// AccountForMMP reply (00 0b ...); nothing is created. The request is cod-boz-online's own test
// vector (auth/account_for_mmp_test.go): IV seed 0x12345678, account hash 0x0123456789abcdef.
Lobby check_lobby(addrinfo *addresses) {
    static const unsigned char request[24] = {
        20,   0,    0,    0,                                          // frame: body length
        0x00, 0x0a, 0x11, 0x9e, 0x15, 0x8d, 0x04, 0x22, 0x40, 0x02,  // cleartext, AccountForMMP
        0x00, 0x78, 0x6f, 0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0x09, 0x00};
    bool connected_any = false;
    for (addrinfo *a = addresses; a; a = a->ai_next) {
        Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCKET) {
            continue;
        }
        set_blocking(s, false);
#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TIMEOUT_MS);
        int result = connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool connected = result == 0;
        if (!connected && wait_for(s, true, milliseconds_left(deadline))) {
            int error = 0;
            socklen_t size = sizeof(error);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &size);
            connected = error == 0;
        }
        if (!connected) {
            close_socket(s);
            continue;
        }
        connected_any = true;
        send(s, (const char *)request, sizeof(request), SEND_FLAGS);
        unsigned char reply[6];   // frame length, then the body's first two bytes
        size_t got = 0;
        while (got < sizeof(reply) && wait_for(s, false, milliseconds_left(deadline))) {
            int n = (int)recv(s, (char *)reply + got, (int)(sizeof(reply) - got), 0);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        close_socket(s);
        uint32_t length = got >= 4 ? (uint32_t)reply[0] | (uint32_t)reply[1] << 8 |
                                         (uint32_t)reply[2] << 16 | (uint32_t)reply[3] << 24
                                   : 0;
        if (got == sizeof(reply) && length >= 2 && length < 4096 && reply[4] == 0x00 && reply[5] == 0x0b) {
            return Lobby::BOZ;
        }
        return Lobby::NOT_BOZ;  // it answered (or kept quiet), but not like a BOZ server
    }
    return connected_any ? Lobby::NOT_BOZ : Lobby::MISSING;
}

// UDP 3478: a STUN binding request as the game sends it (RFC 3489, CHANGE-REQUEST 0).
bool check_stun(addrinfo *addresses) {
    unsigned char request[28] = {0x00, 0x01, 0x00, 0x08};
    std::random_device random;
    for (int i = 4; i < 20; ++i) {
        request[i] = (unsigned char)(random() & 0xff);
    }
    request[20] = 0x00;  // CHANGE-REQUEST, length 4, flags 0
    request[21] = 0x03;
    request[22] = 0x00;
    request[23] = 0x04;
    for (addrinfo *a = addresses; a; a = a->ai_next) {
        Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCKET) {
            continue;
        }
        bool answered = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TIMEOUT_MS);
        for (int attempt = 0; attempt < 3 && !answered && milliseconds_left(deadline) > 0; ++attempt) {
            sendto(s, (const char *)request, sizeof(request), SEND_FLAGS, a->ai_addr, (int)a->ai_addrlen);
            int wait = std::min(milliseconds_left(deadline), 1300);
            if (!wait_for(s, false, wait)) {
                continue;
            }
            unsigned char reply[512];
            int n = (int)recv(s, (char *)reply, sizeof(reply), 0);
            answered = n >= 20 && reply[0] == 0x01 && reply[1] == 0x01 &&
                       std::memcmp(reply + 4, request + 4, 16) == 0;
        }
        close_socket(s);
        if (answered) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool valid_server_host(const std::string &host) {
    if (host.empty() || host.size() >= 128 || !std::isalnum((unsigned char)host[0])) {
        return false;
    }
    for (char c : host) {
        if (!std::isalnum((unsigned char)c) && c != '.' && c != '-' && c != ':') {
            return false;
        }
    }
    return true;
}

ServerCheck check_boz_server(const std::string &host) {
    ServerCheck result;
    if (!valid_server_host(host)) {
        result.message = "Enter a server's name or IP address (letters, digits, dots, dashes).";
        return result;
    }
#if defined(_WIN32)
    Winsock winsock;
    if (!winsock.ok) {
        result.message = "Could not start the network check.";
        return result;
    }
#endif
    addrinfo *tcp = resolve(host, LOBBY_PORT, SOCK_STREAM);
    if (!tcp) {
        result.message = "No server by that name was found. Check the spelling.";
        return result;
    }
    Lobby lobby = check_lobby(tcp);
    freeaddrinfo(tcp);
    if (lobby == Lobby::MISSING) {
        result.message = "Nothing answers on port 3074 there. Is the server running and reachable?";
        return result;
    }
    if (lobby == Lobby::NOT_BOZ) {
        result.message = "That isn't a BOZ server: something answers on port 3074, but not the "
                         "way a Black Ops Zombies online server does.";
        return result;
    }
    addrinfo *udp = resolve(host, STUN_PORT, SOCK_DGRAM);
    bool stun = udp && check_stun(udp);
    if (udp) {
        freeaddrinfo(udp);
    }
    if (!stun) {
        result.message = "It looks like a BOZ server, but its UDP port 3478 doesn't answer, so "
                         "matches can't start. Ask the server's owner to open it.";
        return result;
    }
    result.ok = true;
    result.message = "BOZ server found.";
    return result;
}
