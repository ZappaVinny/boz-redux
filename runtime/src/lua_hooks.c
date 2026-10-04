/* hook: Lua handlers on game functions. Each hooked address gets one emulator code hook (the
 * game's code is never patched); any number of handlers share it, in the order they were added.
 *
 * before(call) runs on entry and may read or change the arguments, or skip the function with a
 * result. after(call) runs when the function returns: on entry the return address is swapped for
 * a trap page, whose own code hook runs the after handlers and continues at the real caller.
 * Hooks fire on the game thread only (the thread the Lua state runs on). */
#include "lua_runtime_internal.h"

#include "arm_emu.h"
#include "mod_runtime.h"

#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

enum {
    MAX_SITES = 128,
    MAX_HANDLERS = 512,
    MAX_FRAMES = 512,
};


struct site {
    uint32_t address;  /* 0: free */
    int arm_hook;
    int handlers;
    int after_handlers;
};

struct handler {
    bool used, failed;
    int site;
    int before, after;  /* registry refs or LUA_NOREF */
    int mod;
};

struct frame {
    int site;
    uint32_t return_address, sp;
    uint32_t args[4];
};

struct call {
    struct arm_emu_regs *regs;  /* NULL once the handler returned */
    uint32_t address;
    uint32_t args[4];           /* after: the arguments the function was called with */
    uint32_t sp;
    bool after;
    bool skip;
    uint32_t skip_lo, skip_hi;
};

static const char CALL_TYPE[] = "boz.hook.call";

static lua_State *g_L;
static struct site g_sites[MAX_SITES];
static struct handler g_handlers[MAX_HANDLERS];
static struct frame g_frames[MAX_FRAMES];
static int g_frame_count;
static uint32_t g_trap;

static float bits_to_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t float_to_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* --- The call object --------------------------------------------------------------------------- */

static struct call *check_call(lua_State *L) {
    struct call *call = luaL_checkudata(L, 1, CALL_TYPE);
    if (!call->regs) {
        luaL_error(L, "this call object is only valid inside its hook handler");
    }
    return call;
}

static int check_index(lua_State *L, int arg) {
    lua_Integer index = luaL_checkinteger(L, arg);
    if (index < 1 || index > 16) {
        luaL_error(L, "argument index must be 1 to 16");
    }
    return (int)index;
}

static uint32_t read_arg(const struct call *call, int index) {
    if (index <= 4) {
        return call->after ? call->args[index - 1] : call->regs->r[index - 1];
    }
    return *(const volatile uint32_t *)(uintptr_t)(call->sp + (uint32_t)(index - 5) * 4u);
}

static void write_arg(lua_State *L, struct call *call, int index, uint32_t value) {
    if (call->after) {
        luaL_error(L, "arguments can only be changed in a before handler");
    }
    if (index <= 4) {
        call->regs->r[index - 1] = value;
    } else {
        *(volatile uint32_t *)(uintptr_t)(call->sp + (uint32_t)(index - 5) * 4u) = value;
    }
}

static uint32_t check_u32(lua_State *L, int arg) {
    if (lua_isboolean(L, arg)) {
        return lua_toboolean(L, arg) ? 1u : 0u;
    }
    if (lua_isnoneornil(L, arg)) {
        return 0;
    }
    return (uint32_t)luaL_checkinteger(L, arg);
}

static int call_arg(lua_State *L) {
    struct call *call = check_call(L);
    lua_pushinteger(L, read_arg(call, check_index(L, 2)));
    return 1;
}

static int call_arg_signed(lua_State *L) {
    struct call *call = check_call(L);
    lua_pushinteger(L, (int32_t)read_arg(call, check_index(L, 2)));
    return 1;
}

static int call_arg_float(lua_State *L) {
    struct call *call = check_call(L);
    lua_pushnumber(L, bits_to_float(read_arg(call, check_index(L, 2))));
    return 1;
}

static int call_set_arg(lua_State *L) {
    struct call *call = check_call(L);
    write_arg(L, call, check_index(L, 2), check_u32(L, 3));
    return 0;
}

static int call_set_arg_float(lua_State *L) {
    struct call *call = check_call(L);
    write_arg(L, call, check_index(L, 2), float_to_bits((float)luaL_checknumber(L, 3)));
    return 0;
}

static int call_skip(lua_State *L) {
    struct call *call = check_call(L);
    if (call->after) {
        return luaL_error(L, "skip is only possible in a before handler");
    }
    call->skip = true;
    call->skip_lo = check_u32(L, 2);
    call->skip_hi = 0;
    return 0;
}

static int call_skip_float(lua_State *L) {
    struct call *call = check_call(L);
    if (call->after) {
        return luaL_error(L, "skip is only possible in a before handler");
    }
    call->skip = true;
    call->skip_lo = float_to_bits((float)luaL_checknumber(L, 2));
    call->skip_hi = 0;
    return 0;
}

static void check_after(lua_State *L, const struct call *call) {
    if (!call->after) {
        luaL_error(L, "the result is only known in an after handler");
    }
}

static int call_result(lua_State *L) {
    struct call *call = check_call(L);
    check_after(L, call);
    lua_pushinteger(L, call->regs->r[0]);
    return 1;
}

static int call_result_signed(lua_State *L) {
    struct call *call = check_call(L);
    check_after(L, call);
    lua_pushinteger(L, (int32_t)call->regs->r[0]);
    return 1;
}

static int call_result_float(lua_State *L) {
    struct call *call = check_call(L);
    check_after(L, call);
    lua_pushnumber(L, bits_to_float(call->regs->r[0]));
    return 1;
}

static int call_set_result(lua_State *L) {
    struct call *call = check_call(L);
    check_after(L, call);
    call->regs->r[0] = check_u32(L, 2);
    return 0;
}

static int call_set_result_float(lua_State *L) {
    struct call *call = check_call(L);
    check_after(L, call);
    call->regs->r[0] = float_to_bits((float)luaL_checknumber(L, 2));
    return 0;
}

static int call_address(lua_State *L) {
    struct call *call = check_call(L);
    lua_pushinteger(L, call->address);
    return 1;
}

static void push_call(lua_State *L, struct arm_emu_regs *regs, uint32_t address, bool after,
                      const uint32_t *args, uint32_t sp) {
    struct call *call = lua_newuserdatauv(L, sizeof(*call), 0);
    memset(call, 0, sizeof(*call));
    call->regs = regs;
    call->address = address;
    call->after = after;
    call->sp = sp;
    memcpy(call->args, args, sizeof(call->args));
    luaL_setmetatable(L, CALL_TYPE);
}

/* --- Dispatch ---------------------------------------------------------------------------------- */

static void run_handlers(int site, bool after, struct call *call, int call_index) {
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        struct handler *h = &g_handlers[i];
        int ref = after ? h->after : h->before;
        if (!h->used || h->failed || h->site != site || ref == LUA_NOREF) {
            continue;
        }
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_pushvalue(g_L, call_index);
        if (!lua_runtime_pcall(h->mod, 1, 0)) {
            h->failed = true;
            lua_runtime_log(h->mod, "hook handler switched off after the error above");
        }
        if (!after && call->skip) {
            break;
        }
    }
}

static void on_trap(struct arm_emu_regs *regs, void *user);

static void on_site(struct arm_emu_regs *regs, void *user) {
    int site = (int)(intptr_t)user;
    int top = lua_gettop(g_L);
    push_call(g_L, regs, g_sites[site].address, false, regs->r, regs->r[13]);
    struct call *call = lua_touserdata(g_L, -1);
    run_handlers(site, false, call, lua_gettop(g_L));
    call->regs = NULL;
    lua_settop(g_L, top);

    if (call->skip) {
        regs->r[0] = call->skip_lo;
        regs->r[1] = call->skip_hi;
        regs->thumb = (regs->r[14] & 1u) != 0;
        regs->r[15] = regs->r[14] & ~1u;
        return;
    }
    if (g_sites[site].after_handlers > 0 && g_trap && g_frame_count < MAX_FRAMES) {
        struct frame *frame = &g_frames[g_frame_count++];
        frame->site = site;
        frame->return_address = regs->r[14];
        frame->sp = regs->r[13];
        memcpy(frame->args, regs->r, sizeof(frame->args));
        regs->r[14] = g_trap;
    }
}

static void on_trap(struct arm_emu_regs *regs, void *user) {
    (void)user;
    /* A function that returned normally left sp where it found it; frames below a matching one
     * belong to calls that never returned (longjmp) and are dropped. */
    while (g_frame_count > 0 && g_frames[g_frame_count - 1].sp != regs->r[13] &&
           g_frame_count > 1) {
        --g_frame_count;
    }
    if (g_frame_count == 0) {
        fprintf(stderr, "[hook] return trap without a pending call\n");
        return;
    }
    struct frame frame = g_frames[--g_frame_count];
    int top = lua_gettop(g_L);
    push_call(g_L, regs, g_sites[frame.site].address, true, frame.args, frame.sp);
    struct call *call = lua_touserdata(g_L, -1);
    run_handlers(frame.site, true, call, lua_gettop(g_L));
    call->regs = NULL;
    lua_settop(g_L, top);
    regs->thumb = (frame.return_address & 1u) != 0;
    regs->r[15] = frame.return_address & ~1u;
}

/* The trap is a word of the emulator's code page: always mapped executable, never run (its code
 * hook moves on first). Ordinary memory would not do: the emulator may have mapped it as data. */
static bool ensure_trap(void) {
    if (g_trap) {
        return true;
    }
    uint32_t trap = arm_emu_trap_address();
    if (!trap || arm_emu_hook_add(trap, on_trap, NULL) < 0) {
        return false;
    }
    g_trap = trap;
    return true;
}

/* --- Lua: hook.add / hook.remove ---------------------------------------------------------------- */

static uint32_t check_target(lua_State *L, int arg) {
    if (lua_type(L, arg) == LUA_TSTRING) {
        uint32_t address = game_symbol_address(lua_tostring(L, arg));
        if (!address) {
            luaL_error(L, "unknown game function %s", lua_tostring(L, arg));
        }
        return address;
    }
    lua_Integer value = luaL_checkinteger(L, arg);
    if (value < 0x10000 || value > 0xffffffffLL) {
        luaL_error(L, "bad game address");
    }
    return (uint32_t)value;
}

static int ref_field(lua_State *L, int table, const char *name) {
    lua_getfield(L, table, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    luaL_checktype(L, -1, LUA_TFUNCTION);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static int api_hook_add(lua_State *L) {
    uint32_t address = check_target(L, 1) & ~1u;
    luaL_checktype(L, 2, LUA_TTABLE);
    int before = ref_field(L, 2, "before");
    int after = ref_field(L, 2, "after");
    if (before == LUA_NOREF && after == LUA_NOREF) {
        return luaL_error(L, "hook.add needs a before and/or an after function");
    }
    if (after != LUA_NOREF && !ensure_trap()) {
        return luaL_error(L, "cannot set up after hooks");
    }
    int site = -1, free_site = -1;
    for (int i = 0; i < MAX_SITES; ++i) {
        if (g_sites[i].address == address) {
            site = i;
            break;
        }
        if (!g_sites[i].address && free_site < 0) {
            free_site = i;
        }
    }
    if (site < 0) {
        if (free_site < 0) {
            return luaL_error(L, "too many hooked functions");
        }
        site = free_site;
        int arm_hook = arm_emu_hook_add(address, on_site, (void *)(intptr_t)site);
        if (arm_hook < 0) {
            return luaL_error(L, "cannot hook 0x%x", (unsigned)address);
        }
        g_sites[site] = (struct site){address, arm_hook, 0, 0};
    }
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        struct handler *h = &g_handlers[i];
        if (h->used) {
            continue;
        }
        *h = (struct handler){true, false, site, before, after, lua_runtime_current_mod()};
        g_sites[site].handlers++;
        if (after != LUA_NOREF) {
            g_sites[site].after_handlers++;
        }
        lua_pushinteger(L, i + 1);
        return 1;
    }
    return luaL_error(L, "too many hooks");
}

static int api_hook_remove(lua_State *L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    if (handle < 1 || handle > MAX_HANDLERS || !g_handlers[handle - 1].used) {
        return 0;
    }
    struct handler *h = &g_handlers[handle - 1];
    struct site *site = &g_sites[h->site];
    luaL_unref(L, LUA_REGISTRYINDEX, h->before);
    luaL_unref(L, LUA_REGISTRYINDEX, h->after);
    if (h->after != LUA_NOREF) {
        site->after_handlers--;
    }
    h->used = false;
    if (--site->handlers == 0) {
        arm_emu_hook_remove(site->arm_hook);
        site->address = 0;
    }
    return 0;
}

static int api_float_bits(lua_State *L) {
    lua_pushinteger(L, float_to_bits((float)luaL_checknumber(L, 1)));
    return 1;
}

static int api_bits_float(lua_State *L) {
    lua_pushnumber(L, bits_to_float((uint32_t)luaL_checkinteger(L, 1)));
    return 1;
}

void lua_hooks_open(lua_State *L) {
    g_L = L;
    static const luaL_Reg methods[] = {
        {"arg", call_arg},
        {"arg_signed", call_arg_signed},
        {"arg_float", call_arg_float},
        {"set_arg", call_set_arg},
        {"set_arg_float", call_set_arg_float},
        {"skip", call_skip},
        {"skip_float", call_skip_float},
        {"result", call_result},
        {"result_signed", call_result_signed},
        {"result_float", call_result_float},
        {"set_result", call_set_result},
        {"set_result_float", call_set_result_float},
        {"address", call_address},
        {NULL, NULL},
    };
    luaL_newmetatable(L, CALL_TYPE);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg functions[] = {
        {"add", api_hook_add},
        {"remove", api_hook_remove},
        {"float_bits", api_float_bits},
        {"bits_float", api_bits_float},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    lua_setglobal(L, "hook");
}
