/* The `network` module: the online server the game uses for Play Online (chosen in the launcher's
 * Settings > Online, client.ini [network] server), and server checks for mods. The game's online
 * service names (*.demonware.net) go to that server (s3e_multiplayer_resolve_hostname); "" keeps
 * the game offline, the default. A mod may only check servers its mod.toml lists under
 * [network] connects ("*": any the player enters). */
#include "lua_runtime_internal.h"
#include "mods.h"
#include "s3e_host_internal.h"

#include "lauxlib.h"

enum { CHECK_TIMEOUT_MS = 6000, DEFAULT_PORT = 3074 };

/* A server check: runs on a thread of its own, its result is handed to Lua on the next frame. */
struct check {
    char host[128];
    int port;
    int mod;
    int callback;   /* registry reference */
    bool done, ok;
    char message[160];
    struct check *next;
};

static pthread_mutex_t g_checks_lock = PTHREAD_MUTEX_INITIALIZER;
static struct check *g_checks;

/* A host name or address: letters, digits, '.', '-', ':' (IPv6), at most 127 characters. */
static bool valid_host(const char *host) {
    size_t length = strlen(host);
    if (length == 0 || length >= 128 || host[0] == '-' || host[0] == '.') {
        return false;
    }
    for (const char *c = host; *c; ++c) {
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-' && *c != ':') {
            return false;
        }
    }
    return true;
}

static const struct mod_info *calling_mod(lua_State *state) {
    int mod = lua_runtime_current_mod();
    if (mod < 0) {
        luaL_error(state, "network can only be used by a mod's code");
    }
    return mods_enabled_at(mod);
}

static void check_allowed(lua_State *state, const struct mod_info *mod, const char *host) {
    if (!valid_host(host)) {
        luaL_error(state, "not a server name or address: %s", host);
    }
    if (!mods_may_connect(mod, host)) {
        luaL_error(state, "mod %s may not connect to %s: list it in mod.toml [network] connects",
                   mod->id, host);
    }
}

/* network.server(): the online server the game uses, or "" when it stays offline. */
static int api_server(lua_State *state) {
    lua_pushstring(state, s3e_multiplayer_server());
    return 1;
}

/* network.set_server(host): retired. The launcher's Settings > Online chooses the server (it
 * checks that it is a BOZ server); a mod's choice is ignored so the two never disagree. */
static int api_set_server(lua_State *state) {
    const struct mod_info *mod = calling_mod(state);
    const char *host = luaL_optstring(state, 1, "");
    static bool told;
    if (!told) {
        told = true;
        fprintf(stderr, "[network] mod %s asked for online server %s: ignored, the launcher's "
                        "Online settings choose it\n", mod->id, host[0] ? host : "off");
    }
    return 0;
}

static void finish(struct check *check, bool ok, const char *message) {
    pthread_mutex_lock(&g_checks_lock);
    check->ok = ok;
    snprintf(check->message, sizeof(check->message), "%s", message);
    check->done = true;
    pthread_mutex_unlock(&g_checks_lock);
}

static void *check_thread(void *user) {
    struct check *check = user;
    char port[16];
    snprintf(port, sizeof(port), "%d", check->port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(check->host, port, &hints, &addresses) != 0 || !addresses) {
        finish(check, false, "The server's name was not found.");
        return NULL;
    }
    bool ok = false;
    const char *message = "The server did not answer.";
    for (struct addrinfo *address = addresses; address && !ok; address = address->ai_next) {
        int fd = (int)socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        socket_set_nonblocking(fd, 1);
        if (connect(fd, address->ai_addr, (socklen_t)address->ai_addrlen) == 0) {
            ok = true;
        } else {
            int error = socket_errno();
            if (error == EINPROGRESS || error == EWOULDBLOCK) {
                struct pollfd poll_fd = {.fd = fd, .events = POLLOUT};
                if (poll(&poll_fd, 1, CHECK_TIMEOUT_MS) > 0) {
                    int code = 0;
                    socklen_t size = sizeof(code);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &size);
                    ok = code == 0;
                    if (!ok) {
                        message = "The server refused the connection (is it running?).";
                    }
                }
            } else if (error == ECONNREFUSED) {
                message = "The server refused the connection (is it running?).";
            }
        }
        socket_close(fd);
    }
    freeaddrinfo(addresses);
    finish(check, ok, ok ? "The server answered." : message);
    return NULL;
}

/* network.check(host, fn) or check(host, port, fn): tries to reach the server's login port (TCP
 * 3074) in the background; fn(ok, message) runs on a later frame. */
static int api_check(lua_State *state) {
    const struct mod_info *mod = calling_mod(state);
    const char *host = luaL_checkstring(state, 1);
    int port = DEFAULT_PORT;
    int callback_index = 2;
    if (lua_isinteger(state, 2)) {
        port = (int)lua_tointeger(state, 2);
        callback_index = 3;
        luaL_argcheck(state, port > 0 && port < 65536, 2, "port 1-65535");
    }
    luaL_checktype(state, callback_index, LUA_TFUNCTION);
    check_allowed(state, mod, host);
    struct check *check = calloc(1, sizeof(*check));
    if (!check) {
        return luaL_error(state, "out of memory");
    }
    snprintf(check->host, sizeof(check->host), "%s", host);
    check->port = port;
    check->mod = lua_runtime_current_mod();
    lua_pushvalue(state, callback_index);
    check->callback = luaL_ref(state, LUA_REGISTRYINDEX);
    pthread_mutex_lock(&g_checks_lock);
    check->next = g_checks;
    g_checks = check;
    pthread_mutex_unlock(&g_checks_lock);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, check_thread, check) != 0) {
        finish(check, false, "Could not start the check.");
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void lua_network_poll(lua_State *state) {
    for (;;) {
        struct check *found = NULL;
        pthread_mutex_lock(&g_checks_lock);
        for (struct check **link = &g_checks; *link; link = &(*link)->next) {
            if ((*link)->done) {
                found = *link;
                *link = found->next;
                break;
            }
        }
        pthread_mutex_unlock(&g_checks_lock);
        if (!found) {
            return;
        }
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->callback);
        luaL_unref(state, LUA_REGISTRYINDEX, found->callback);
        lua_pushboolean(state, found->ok);
        lua_pushstring(state, found->message);
        lua_runtime_pcall(found->mod, 2, 0);
        free(found);
    }
}

void lua_network_open(lua_State *state) {
    static const luaL_Reg functions[] = {{"server", api_server},
                                         {"set_server", api_set_server},
                                         {"check", api_check},
                                         {NULL, NULL}};
    lua_newtable(state);
    luaL_setfuncs(state, functions, 0);
    lua_setglobal(state, "network");
}
