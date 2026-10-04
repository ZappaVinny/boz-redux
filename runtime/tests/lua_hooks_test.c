/* hook module against real emulated code: Lua handlers change arguments, skip calls and change
 * results; several handlers share a function; failing handlers are switched off. */
#include "arm_emu.h"
#include "lua_runtime_internal.h"
#include "mod_runtime.h"

#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static lua_State *L;
static uint32_t *g_code;
static int g_failures;

/* The runtime pieces lua_hooks.c uses, reduced to what a test needs. */
uint32_t game_symbol_address(const char *name) {
    return strcmp(name, "AddOne") == 0 ? (uint32_t)(uintptr_t)&g_code[0] : 0;
}

int lua_runtime_current_mod(void) {
    return 0;
}

bool lua_runtime_pcall(int mod, int nargs, int nresults) {
    (void)mod;
    if (lua_pcall(L, nargs, nresults, 0) != LUA_OK) {
        fprintf(stderr, "(expected) handler error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void lua_runtime_log(int mod, const char *text) {
    (void)mod;
    fprintf(stderr, "(log) %s\n", text);
}

static void run(const char *chunk) {
    if (luaL_dostring(L, chunk) != LUA_OK) {
        fprintf(stderr, "FAIL lua: %s\n", lua_tostring(L, -1));
        g_failures++;
        lua_pop(L, 1);
    }
}

static void expect(const char *name, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u want %u\n", name, got, want);
        g_failures++;
    } else {
        fprintf(stderr, "ok   %s\n", name);
    }
}

static uint32_t call1(uint32_t fn, uint32_t arg) {
    return (uint32_t)arm_emu_call(fn, 1, &arg);
}

int main(void) {
    if (!arm_emu_init()) {
        return 1;
    }
    g_code = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t code[] = {
        0xe2800001u, 0xe12fff1eu, /* 0: AddOne: r0 + 1 */
        0xe0800001u, 0xe12fff1eu, /* 2: r0 + r1 */
        0xe2400001u, 0xe12fff1eu, /* 4: r0 - 1 */
    };
    memcpy(g_code, code, sizeof(code));
    uint32_t add_one = (uint32_t)(uintptr_t)&g_code[0];

    L = luaL_newstate();
    luaL_openlibs(L);
    lua_hooks_open(L);

    expect("unhooked", call1(add_one, 41), 42);

    run("h1 = hook.add('AddOne', {before = function(c) c:set_arg(1, c:arg(1) * 10) end})");
    expect("before changes an argument", call1(add_one, 4), 41);

    run("h2 = hook.add('AddOne', {after = function(c) c:set_result(c:result() + 1000 + c:arg(1)) end})");
    expect("after sees original args and changes the result", call1(add_one, 4), 41 + 1000 + 40);

    run("hook.remove(h1); hook.remove(h2)");
    expect("removed hooks", call1(add_one, 4), 5);

    run("h3 = hook.add('AddOne', {before = function(c) if c:arg(1) == 99 then c:skip(7) end end})");
    expect("skip with a result", call1(add_one, 99), 7);
    expect("no skip for other calls", call1(add_one, 1), 2);
    run("hook.remove(h3)");

    run("h4 = hook.add('AddOne', {before = function(c) error('boom') end})");
    expect("failing handler does not break the call", call1(add_one, 1), 2);
    run("hook.remove(h4)");

    run("seen = hook.bits_float(hook.float_bits(1.5))");
    lua_getglobal(L, "seen");
    expect("float bits round trip", (uint32_t)(lua_tonumber(L, -1) * 10), 15);
    lua_pop(L, 1);

    /* r0 + r1, hooked by address with float conversions on the result. */
    char chunk[256];
    snprintf(chunk, sizeof(chunk),
             "hook.add(%u, {after = function(c) c:set_result(c:result() * 2) end})",
             (unsigned)(uintptr_t)&g_code[2]);
    run(chunk);
    uint32_t args[2] = {20, 1};
    expect("hook by address", (uint32_t)arm_emu_call((uint32_t)(uintptr_t)&g_code[2], 2, args), 42);

    /* A handler hooks another function that already ran (cached): the flush is deferred until
     * the handler returns, and the new hook works on the next call. */
    uint32_t sub_one = (uint32_t)(uintptr_t)&g_code[4];
    expect("sub_one before", call1(sub_one, 10), 9);
    snprintf(chunk, sizeof(chunk),
             "h5 = hook.add('AddOne', {before = function(c) if not inner then "
             "inner = hook.add(%u, {after = function(c2) c2:set_result(500) end}) end end})",
             (unsigned)sub_one);
    run(chunk);
    expect("call that adds the hook still runs normally", call1(add_one, 1), 2);
    expect("hook added inside a handler works", call1(sub_one, 10), 500);
    run("hook.remove(h5); hook.remove(inner)");
    expect("both removed", call1(sub_one, 10), 9);

    run("ok, err = pcall(hook.add, 'Missing', {before = print})");
    lua_getglobal(L, "ok");
    expect("unknown function is an error", (uint32_t)lua_toboolean(L, -1), 0);
    lua_pop(L, 1);

    lua_close(L);
    return g_failures ? 1 : 0;
}
