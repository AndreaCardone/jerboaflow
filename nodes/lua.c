/* nodes/lua.c -- run arbitrary Lua 5.4 on each input packet.
 *
 * args: "<script_path>"
 *
 * Script contract: define a global function
 *
 *     function process(payload, in_idx)
 *         -- payload: string (raw bytes)
 *         -- in_idx:  integer, input port
 *         -- return a string to emit a packet, or nil to emit nothing.
 *     end
 *
 * Per-node state is preserved across calls (Lua globals stick around),
 * so users can write stateful filters with no extra ceremony. The runtime
 * guarantees per-node sequential execution, so this lua_State is touched
 * by at most one worker at a time -- no internal locking needed. */

#include "nodes.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bundled rxi/json.lua (MIT). Exposed to user scripts as global `json`
 * with json.decode / json.encode -- so users don't need patterns to
 * pull fields out of JSON payloads. See nodes/embed/json.lua. */
#include "embed/json_lua.h"

typedef struct {
    lua_State *L;
    char       path[256];
    size_t     mem_used;
    size_t     mem_cap;   /* hard cap on Lua heap; 0 = unlimited */
} Ctx;

/* Custom allocator: refuses growth past mem_cap. Lua handles NULL from
 * the allocator gracefully (treats it as out-of-memory). */
static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    Ctx *c = ud;
    if (nsize == 0) {
        if (ptr) { c->mem_used -= osize; free(ptr); }
        return NULL;
    }
    /* Growing or new allocation: check the cap. */
    size_t old = ptr ? osize : 0;
    if (c->mem_cap && nsize > old && (c->mem_used + (nsize - old)) > c->mem_cap)
        return NULL;
    void *q = realloc(ptr, nsize);
    if (!q) return NULL;
    c->mem_used += nsize - old;
    return q;
}

/* Sandboxed openlibs: deliberately omits io, os, package, debug, loadfile,
 * dofile, require, load, loadstring. A malicious or buggy script cannot
 * shell out, touch the filesystem, or load native code. */
static void open_sandbox_libs(lua_State *L) {
    static const luaL_Reg libs[] = {
        { LUA_GNAME,      luaopen_base    },
        { LUA_TABLIBNAME, luaopen_table   },
        { LUA_STRLIBNAME, luaopen_string  },
        { LUA_MATHLIBNAME,luaopen_math    },
        { LUA_UTF8LIBNAME,luaopen_utf8    },
        { NULL, NULL }
    };
    for (const luaL_Reg *l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    /* Remove the most dangerous base entries that luaopen_base installs:
     * dofile / loadfile / load / loadstring / require open arbitrary
     * code or files. The user-supplied script is still loaded via
     * luaL_loadfile in init(), so removing these is safe. */
    static const char *banned[] = {
        "dofile", "loadfile", "load", "loadstring", "require",
        "collectgarbage",  /* lets script defeat the memory cap */
        NULL
    };
    for (const char **b = banned; *b; b++) {
        lua_pushnil(L);
        lua_setglobal(L, *b);
    }
}

static Packet *process(Node *self, size_t idx, Packet *in) {
    if (!in) return NULL;
    Ctx *c = self->ctx;
    lua_State *L = c->L;

    lua_getglobal(L, "process");
    lua_pushlstring(L, in->data, in->len);
    lua_pushinteger(L, (lua_Integer)idx);

    Packet *out = NULL;
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        fprintf(stderr, "lua/%s: %s\n", self->name, lua_tostring(L, -1));
        lua_pop(L, 1);
    } else if (lua_isstring(L, -1)) {
        size_t n;
        const char *s = lua_tolstring(L, -1, &n);
        out = packet_new(s, n);
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }

    packet_release(in);
    return out;
}

static void ctx_free(void *p) {
    Ctx *c = p;
    if (c->L) lua_close(c->L);
    free(c);
}

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: lua node requires a script path\n");
        return -1;
    }
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;

    /* args: "<path> [mem_kb]". Default cap 4 MB; 0 = unlimited. */
    c->mem_cap = 4 * 1024 * 1024;
    {
        char buf[320];
        size_t i = 0;
        for (; args[i] && i < sizeof(buf) - 1; i++) buf[i] = args[i];
        buf[i] = '\0';
        char *sp = strchr(buf, ' ');
        if (sp) {
            *sp = '\0';
            long kb = strtol(sp + 1, NULL, 10);
            if (kb >= 0) c->mem_cap = (size_t)kb * 1024;
        }
        size_t j = 0;
        for (; buf[j] && j < sizeof(c->path) - 1; j++) c->path[j] = buf[j];
        c->path[j] = '\0';
    }

    c->L = lua_newstate(lua_alloc, c);
    if (!c->L) { free(c); return -1; }
    open_sandbox_libs(c->L);

    /* Preload bundled json.lua and bind it to global `json`. */
    if (luaL_loadbufferx(c->L, (const char *)json_lua, json_lua_len,
                        "json.lua", "t") != LUA_OK ||
        lua_pcall(c->L, 0, 1, 0) != LUA_OK) {
        fprintf(stderr, "lua/%s: bundled json.lua failed: %s\n",
                n->name, lua_tostring(c->L, -1));
        lua_close(c->L);
        free(c);
        return -1;
    }
    lua_setglobal(c->L, "json");

    /* expose the node name as jerboa.node_name */
    lua_newtable(c->L);
    lua_pushstring(c->L, n->name);
    lua_setfield(c->L, -2, "node_name");
    lua_setglobal(c->L, "jerboa");

    if (luaL_dofile(c->L, c->path) != LUA_OK) {
        fprintf(stderr, "lua/%s: load failed: %s\n",
                n->name, lua_tostring(c->L, -1));
        lua_close(c->L);
        free(c);
        return -1;
    }
    lua_getglobal(c->L, "process");
    int ok = lua_isfunction(c->L, -1);
    lua_pop(c->L, 1);
    if (!ok) {
        fprintf(stderr, "lua/%s: script defines no global process()\n", n->name);
        lua_close(c->L);
        free(c);
        return -1;
    }

    n->ctx = c;
    n->ctx_free = ctx_free;
    n->process = process;
    return 0;
}

const NodeType ndt_lua = { "lua", init };
