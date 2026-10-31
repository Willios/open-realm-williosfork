/*
 * g_lua.c — Lua map scripts (war3map.lua).
 *
 * Reforged maps saved in Lua mode carry war3map.lua instead of war3map.j.
 * The script runs in its own Lua state but drives the same JASS natives and
 * Blizzard.j functions: an unknown Lua global resolves to the JASS function or
 * global of that name, and calls convert arguments by the native's declared
 * signature. Lua functions passed as `code` become native jassFunc_t records,
 * so triggers, timers and filters run them like JASS callbacks.
 *
 * Handles are full userdata holding a pinned jassVar_t (a handle reference,
 * like a JASS global). One userdata per handle pointer keeps `==` and table
 * keys working the way Reforged scripts expect.
 */

#include "g_local.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define LUA_HANDLE_META "wc3handle"

typedef struct {
    jassVar_t *ref;
    edict_t *ent;          /* edict handles: the slot and incarnation the handle was made for */
    uint32_t spawn_time;
} luaHandle_t;

static lua_State *g_lua;
static jass_t *g_luajass; /* state natives run on: the calling coroutine, or level.vm */
static char g_luahandles_key;   /* registry: lightuserdata pointer -> handle userdata (weak) */
static char g_luacallbacks_key; /* registry: lightuserdata jassFunc_t -> Lua function */
static char g_luafuncs_key;     /* registry: Lua function -> lightuserdata jassFunc_t */
static uint32_t g_luacallback_count;

/* Reforged ships these helpers in its Lua runtime rather than in Blizzard.j.
 * This VM stores rawcodes in string byte order (VM_EvalFourCC reads the four
 * characters as a little-endian integer), so FourCC must match it, not retail. */
static cstring_t const g_luaprelude =
    "function FourCC(s) return string.unpack('<I4', s) end\n"
    "function __jarray(default)\n"
    "    return setmetatable({}, { __index = function() return default end })\n"
    "end\n";

static int G_LuaPushValue(lua_State *L, jass_t *j, int index);

static jass_t *G_LuaJass(void) {
    return g_luajass ? g_luajass : level.vm;
}

/* =========================================================================
 * Handles
 * ========================================================================= */

static int G_LuaHandleGC(lua_State *L) {
    luaHandle_t *h = luaL_checkudata(L, 1, LUA_HANDLE_META);
    if (h->ref && level.vm) jass_freeref(h->ref);
    h->ref = NULL;
    return 0;
}

static int G_LuaHandleToString(lua_State *L) {
    luaHandle_t *h = luaL_checkudata(L, 1, LUA_HANDLE_META);
    lua_pushfstring(L, "handle: %p", h->ref ? jass_refhandle(h->ref) : NULL);
    return 1;
}

/* Wrap the handle at the top of the JASS stack, reusing the userdata already bound to that pointer. */
static void G_LuaPushHandle(lua_State *L, jass_t *j, int index) {
    handle_t value = jass_tohandle(j, index);
    luaHandle_t *h;
    int32_t enumvalue;

    if (!value) {
        lua_pushnil(L);
        return;
    }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_luahandles_key);
    /* Enum handles are equal by type and value, so they share one userdata per pair.
     * Units, effects and items share the recycled edict pool: key them by slot and
     * spawn time so a new unit in a dead effect's slot gets a fresh handle. */
    if (jass_isvaluehandle(j, index, &enumvalue)) {
        lua_pushfstring(L, "%s:%d", jass_typename(j, index), (int)enumvalue);
    } else if ((edict_t *)value >= globals.edicts && (edict_t *)value < globals.edicts + globals.num_edicts) {
        edict_t const *ent = value;
        lua_pushfstring(L, "e%d:%d", (int)(ent - globals.edicts), (int)ent->spawn_time);
    } else {
        lua_pushlightuserdata(L, value);
    }
    lua_pushvalue(L, -1);
    if (lua_rawget(L, -3) == LUA_TUSERDATA) {
        lua_replace(L, -3);
        lua_pop(L, 1);
        return;
    }
    lua_pop(L, 1);
    h = lua_newuserdatauv(L, sizeof(*h), 0);
    h->ref = jass_newref(j, index);
    h->ent = NULL;
    h->spawn_time = 0;
    if ((edict_t *)value >= globals.edicts && (edict_t *)value < globals.edicts + globals.num_edicts) {
        h->ent = value;
        h->spawn_time = h->ent->spawn_time;
    }
    luaL_setmetatable(L, LUA_HANDLE_META);
    lua_insert(L, -2);          /* handles, ud, key */
    lua_pushvalue(L, -2);       /* handles, ud, key, ud */
    lua_rawset(L, -4);
    lua_replace(L, -2);
}

/* =========================================================================
 * Callbacks: Lua functions as JASS code
 * ========================================================================= */

static int G_LuaTraceback(lua_State *L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

static uint32_t G_LuaTrampoline(jass_t *j) {
    jassFunc_t const *self = jass_currentnative(j);
    jass_t *previous = g_luajass;
    lua_State *L = g_lua;
    uint32_t results = 0;
    int base;

    if (!L || !self) return 0;
    base = lua_gettop(L);
    lua_pushcfunction(L, G_LuaTraceback);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_luacallbacks_key);
    lua_rawgetp(L, -1, self);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        return 0;
    }
    g_luajass = j;
    if (lua_pcall(L, 0, 1, base + 1) != LUA_OK) {
        fprintf(stderr, "Lua error in %s: %s\n", jass_functionname(self), lua_tostring(L, -1));
    } else if (lua_isboolean(L, -1)) {
        results = jass_pushboolean(j, lua_toboolean(L, -1));
    } else if (lua_isinteger(L, -1)) {
        results = jass_pushinteger(j, (int32_t)lua_tointeger(L, -1));
    }
    g_luajass = previous;
    lua_settop(L, base);
    return results;
}

/* One jassFunc_t per Lua function value, so re-registering the same function reuses its record. */
static jassFunc_t const *G_LuaFunctionToJass(lua_State *L, int index, cstring_t name, bool global) {
    jassFunc_t *func;
    char generated[32];

    index = lua_absindex(L, index);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_luafuncs_key);
    lua_pushvalue(L, index);
    if (lua_rawget(L, -2) == LUA_TLIGHTUSERDATA && !global) {
        func = lua_touserdata(L, -1);
        lua_pop(L, 2);
        return func;
    }
    lua_pop(L, 1);
    if (!name) {
        snprintf(generated, sizeof(generated), "lua#%u", ++g_luacallback_count);
        name = generated;
    }
    func = jass_newnativefunction(level.vm, name, G_LuaTrampoline, global);
    lua_pushvalue(L, index);
    lua_pushlightuserdata(L, func);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_luacallbacks_key);
    lua_pushvalue(L, index);
    lua_rawsetp(L, -2, func);
    lua_pop(L, 1);
    return func;
}

/* =========================================================================
 * Calling natives and Blizzard.j functions
 * ========================================================================= */

static void G_LuaPushArgument(lua_State *L, jass_t *j, int index, cstring_t type) {
    switch (jass_typenamebase(j, type)) {
        case jasstype_integer:
            if (lua_isinteger(L, index)) jass_pushinteger(j, (int32_t)lua_tointeger(L, index));
            else jass_pushinteger(j, (int32_t)lua_tonumber(L, index));
            break;
        case jasstype_real:
            jass_pushnumber(j, (float)lua_tonumber(L, index));
            break;
        case jasstype_boolean:
            jass_pushboolean(j, lua_toboolean(L, index));
            break;
        case jasstype_string:
            if (lua_isnoneornil(L, index)) {
                jass_pushnull(j);
            } else {
                jass_pushstring(j, luaL_tolstring(L, index, NULL));
                lua_pop(L, 1);
            }
            break;
        case jasstype_code:
            if (lua_isfunction(L, index)) jass_pushfunction(j, G_LuaFunctionToJass(L, index, NULL, false));
            else jass_pushnull(j);
            break;
        default: {
            luaHandle_t *h = luaL_testudata(L, index, LUA_HANDLE_META);
            /* A dead effect's slot can hold a new unit: a stale handle must not
             * reach it (moving an old pickup effect dragged enemies along). */
            if (h && h->ref && (!h->ent || (h->ent->inuse && h->ent->spawn_time == h->spawn_time)))
                jass_pushref(j, h->ref);
            else jass_pushnullhandle(j, type);
            break;
        }
    }
}

/* Convert one JASS value by its runtime type (globals) or its declared type (returns). */
static int G_LuaPushValue(lua_State *L, jass_t *j, int index) {
    switch (jass_gettype(j, index)) {
        case jasstype_integer: lua_pushinteger(L, jass_checkinteger(j, index)); break;
        case jasstype_real: lua_pushnumber(L, jass_checknumber(j, index)); break;
        case jasstype_boolean: lua_pushboolean(L, jass_toboolean(j, index)); break;
        case jasstype_string: {
            cstring_t s = jass_checkstring(j, index);
            if (s) lua_pushstring(L, s); else lua_pushnil(L);
            break;
        }
        case jasstype_handle: G_LuaPushHandle(L, j, index); break;
        default: lua_pushnil(L); break;
    }
    return 1;
}

/* JASS selects the local player only inside GetLocalPlayer() branches; Lua has
 * no such branches, so a Lua call runs as the machine's human player, the way
 * Reforged runs a Lua map (GetLocalPlayer, camera and presentation natives). */
static player_t *G_LuaLocalPlayer(void) {
    FOR_LOOP(i, MIN((uint32_t)game.max_clients, (uint32_t)MAX_CLIENTS)) {
        if (game.clients[i].connected) return &game.clients[i].ps;
    }
    return game.clients ? &game.clients[0].ps : NULL;
}

static int G_LuaCallJass(lua_State *L) {
    extern player_t *currentplayer;
    jassFunc_t const *func = lua_touserdata(L, lua_upvalueindex(1));
    jass_t *j = G_LuaJass();
    uint32_t argc = jass_functionargcount(func);
    player_t *previous_player = currentplayer;
    uint32_t results;
    int pushed = 0;

    if (!currentplayer) currentplayer = G_LuaLocalPlayer();
    jass_pushfunction(j, func);
    FOR_LOOP(i, argc) G_LuaPushArgument(L, j, (int)i + 1, jass_functionargtype(func, i));
    results = jass_call(j, argc);
    if (results && strcmp(jass_functionreturntype(func), "nothing")) pushed = G_LuaPushValue(L, j, -1);
    if (results) jass_release(j, results);
    currentplayer = previous_player;
    return pushed;
}

/* Reforged natives newer than the 1.29 common.j. Until each gets a real
 * implementation it resolves to a no-op that reports itself once. */
static cstring_t const g_luareforged_natives[] = {
    "SetPlayerRaceSkin", "SetHDWaterParamsEx", "SetEnemyStartLocPrioCount",
    "SetEnemyStartLocPrio", "SetStartLocPrioCount", "SetStartLocPrio",
    NULL
};

static bool G_LuaIsReforgedNative(cstring_t name) {
    if (!strncmp(name, "Blz", 3)) return true;
    for (cstring_t const *it = g_luareforged_natives; *it; it++)
        if (!strcmp(*it, name)) return true;
    return false;
}

static int G_LuaMissingNative(lua_State *L) {
    if (!lua_toboolean(L, lua_upvalueindex(2))) {
        fprintf(stderr, "war3map.lua: native %s is not implemented (no-op)\n", lua_tostring(L, lua_upvalueindex(1)));
        lua_pushboolean(L, 1);
        lua_replace(L, lua_upvalueindex(2));
    }
    return 0;
}

/* _G fallback: JASS functions are cached as Lua closures; globals are read live. */
static int G_LuaGlobalIndex(lua_State *L) {
    cstring_t name = lua_tostring(L, 2);
    jass_t *j = G_LuaJass();
    jassFunc_t const *func;

    if (!name || !j) return 0;
    func = jass_functionbyname(j, name);
    if (func) {
        lua_pushlightuserdata(L, (void *)func);
        lua_pushcclosure(L, G_LuaCallJass, 1);
        lua_pushvalue(L, 2);
        lua_pushvalue(L, -2);
        lua_rawset(L, 1);
        return 1;
    }
    if (jass_pushglobal(j, name)) {
        G_LuaPushValue(L, j, -1);
        jass_release(j, 1);
        return 1;
    }
    if (G_LuaIsReforgedNative(name)) {
        lua_pushvalue(L, 2);
        lua_pushboolean(L, 0);
        lua_pushcclosure(L, G_LuaMissingNative, 2);
        lua_pushvalue(L, 2);
        lua_pushvalue(L, -2);
        lua_rawset(L, 1);
        return 1;
    }
    return 0;
}

static int G_LuaPrint(lua_State *L) {
    int n = lua_gettop(L);
    FOR_LOOP(i, n) {
        fprintf(stderr, "%s%s", i ? "\t" : "", luaL_tolstring(L, (int)i + 1, NULL));
        lua_pop(L, 1);
    }
    fprintf(stderr, "\n");
    return 0;
}

/* EngineCvar(name) reads a console variable; EngineCvar(name, value) sets it.
 * Engine-only (a Reforged map never sees it): cvars outlive a level change, so
 * a script can hand a choice to the next map it loads with ChangeLevel. */
static int G_LuaEngineCvar(lua_State *L) {
    cstring_t name = luaL_checkstring(L, 1);
    if (!lua_isnoneornil(L, 2) && gi.CvarSet) gi.CvarSet(name, luaL_tolstring(L, 2, NULL));
    lua_pushstring(L, gi.CvarString ? gi.CvarString(name, "") : "");
    return 1;
}

/* EngineQuit() closes the program the way the main menu's Quit does (cvars are
 * archived on the way out). Engine-only, like EngineCvar. */
static int G_LuaEngineQuit(lua_State *L) {
    (void)L;
    if (gi.MenuAction) gi.MenuAction("quit", NULL);
    return 0;
}

/* EngineOpenURL(url) opens an http(s) link outside the game (the system
 * browser; in the browser build, the page downloads or follows it).
 * Engine-only, like EngineCvar. */
static int G_LuaEngineOpenURL(lua_State *L) {
    if (gi.MenuAction) gi.MenuAction("openurl", luaL_checkstring(L, 1));
    return 0;
}

/* EngineUnitModel(unitId) returns the model file of a unit type (UnitUI file),
 * for BlzFrameSetModel. Engine-only, like EngineCvar. */
static int G_LuaEngineUnitModel(lua_State *L) {
    UnitUI_t const *ui = G_UnitUI((uint32_t)luaL_checkinteger(L, 1));
    PATHSTR path = { 0 };
    if (ui && ui->modelFile) G_NormalizeModelFilename(ui->modelFile, path, sizeof(path));
    lua_pushstring(L, path);
    return 1;
}

/* The live unit a Lua handle points at, or NULL (freed slot, other type). */
static edict_t *G_LuaCheckUnit(lua_State *L, int index) {
    luaHandle_t *h = luaL_testudata(L, index, LUA_HANDLE_META);
    if (!h || !h->ent || !h->ent->inuse || h->ent->spawn_time != h->spawn_time) return NULL;
    return h->ent;
}

/* EngineUnitAnimation(unit) returns the sequence the unit plays, its frame,
 * and its upper-body sequence ("" when none). Engine-only test aid. */
static int G_LuaEngineUnitAnimation(lua_State *L) {
    edict_t *ent = G_LuaCheckUnit(L, 1);
    lua_pushstring(L, ent && ent->animation ? ent->animation->name : "");
    lua_pushinteger(L, ent ? (lua_Integer)ent->s.frame : 0);
    lua_pushstring(L, ent && ent->upper_animation ? ent->upper_animation->name : "");
    return 3;
}

/* EngineUnitUpperAnimation(unit, name[, speed]) plays a one-shot sequence
 * (attack, spell, ...) on the unit's upper body while its legs keep the base
 * animation (walk), like World of Warcraft. The bones from the chest up take
 * the overlay; a model without a chest bone plays it on the whole body.
 * Returns false when the model has no such sequence. Engine-only. */
static int G_LuaEngineUnitUpperAnimation(lua_State *L) {
    edict_t *ent = G_LuaCheckUnit(L, 1);
    cstring_t name = luaL_checkstring(L, 2);
    float speed = (float)luaL_optnumber(L, 3, 1.0);
    lua_pushboolean(L, ent && G_SetUnitUpperAnimation(ent, name, speed));
    return 1;
}

/* EngineUnitScriptOrders(unit, on): the script moves this unit itself. Its
 * move, attack and stop orders (right-clicks included) raise their events but
 * start no native behavior, so they never reset its animation. Engine-only. */
static int G_LuaEngineUnitScriptOrders(lua_State *L) {
    edict_t *ent = G_LuaCheckUnit(L, 1);
    if (ent) G_SetUnitFlag(ent, UNIT_FLAG_SCRIPT_ORDERS, lua_toboolean(L, 2));
    return 0;
}

static float G_LuaFieldNumber(lua_State *L, int table, cstring_t key, float fallback) {
    float value;
    lua_getfield(L, table, key);
    value = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

/* EngineTelegraph{shape=, x=, y=, ...} draws a World of Warcraft style ground
 * warning and returns its id. shape: 'circle' (radius), 'ring' (radius, inner:
 * the safe hole), 'cone' (radius, arc in degrees), 'line' (length, width);
 * angle in degrees faces a cone or line. duration (seconds) is the cast: the
 * fill reaches the edge when the hit lands, then the shape flashes and fades
 * (flash = false: it only fades). r, g, b, a (0-255) tint it, red by default.
 * Engine-only. */
static int G_LuaEngineTelegraph(lua_State *L) {
    static cstring_t const shapes[] = { "circle", "ring", "cone", "line", NULL };
    groundTelegraph_t t = { 0 };
    float const deg = 3.14159265f / 180.0f;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "shape");
    t.shape = (telegraphShape_t)luaL_checkoption(L, -1, "circle", shapes);
    lua_pop(L, 1);
    t.origin.x = G_LuaFieldNumber(L, 1, "x", 0.0f);
    t.origin.y = G_LuaFieldNumber(L, 1, "y", 0.0f);
    t.angle = G_LuaFieldNumber(L, 1, "angle", 0.0f) * deg;
    switch (t.shape) {
        case TELEGRAPH_LINE:
            t.radius = G_LuaFieldNumber(L, 1, "length", 600.0f);
            t.inner = G_LuaFieldNumber(L, 1, "width", 150.0f);
            break;
        case TELEGRAPH_CONE:
            t.radius = G_LuaFieldNumber(L, 1, "radius", 500.0f);
            t.inner = G_LuaFieldNumber(L, 1, "arc", 60.0f) * deg;
            break;
        default:
            t.radius = G_LuaFieldNumber(L, 1, "radius", 250.0f);
            t.inner = G_LuaFieldNumber(L, 1, "inner", 0.0f);
            break;
    }
    t.duration_ms = (uint32_t)MAX(0.0f, G_LuaFieldNumber(L, 1, "duration", 1.5f) * 1000.0f);
    t.color = (color32_t){
        BZ_CLAMP_U8(G_LuaFieldNumber(L, 1, "r", 255.0f)),
        BZ_CLAMP_U8(G_LuaFieldNumber(L, 1, "g", 48.0f)),
        BZ_CLAMP_U8(G_LuaFieldNumber(L, 1, "b", 24.0f)),
        BZ_CLAMP_U8(G_LuaFieldNumber(L, 1, "a", 255.0f)),
    };
    lua_getfield(L, 1, "flash");
    if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) t.flags |= TELEGRAPH_NO_FLASH;
    lua_pop(L, 1);
    if (t.radius <= 0.0f) return lua_pushinteger(L, 0), 1;
    lua_pushinteger(L, (lua_Integer)G_SendGroundTelegraph(&t));
    return 1;
}

/* EngineTelegraphStop(id[, fade]) fades a telegraph out over fade seconds
 * (default 0.2), without the impact flash: the cast was interrupted. */
static int G_LuaEngineTelegraphStop(lua_State *L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    float fade = (float)luaL_optnumber(L, 2, 0.2);
    G_StopGroundTelegraph((uint32_t)id, (uint32_t)MAX(0.0f, fade * 1000.0f));
    return 0;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

static void G_LuaOpen(void) {
    lua_State *L = luaL_newstate();
    g_lua = L;
    g_luacallback_count = 0;
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_settop(L, 0);

    if (luaL_newmetatable(L, LUA_HANDLE_META)) {
        lua_pushcfunction(L, G_LuaHandleGC);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, G_LuaHandleToString);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);

    lua_newtable(L);                 /* handles, weak values */
    lua_newtable(L);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_luahandles_key);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_luacallbacks_key);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_luafuncs_key);

    lua_pushcfunction(L, G_LuaPrint);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, G_LuaEngineCvar);
    lua_setglobal(L, "EngineCvar");
    lua_pushcfunction(L, G_LuaEngineQuit);
    lua_setglobal(L, "EngineQuit");
    lua_pushcfunction(L, G_LuaEngineOpenURL);
    lua_setglobal(L, "EngineOpenURL");
    lua_pushcfunction(L, G_LuaEngineUnitModel);
    lua_setglobal(L, "EngineUnitModel");
    lua_pushcfunction(L, G_LuaEngineUnitAnimation);
    lua_setglobal(L, "EngineUnitAnimation");
    lua_pushcfunction(L, G_LuaEngineUnitUpperAnimation);
    lua_setglobal(L, "EngineUnitUpperAnimation");
    lua_pushcfunction(L, G_LuaEngineUnitScriptOrders);
    lua_setglobal(L, "EngineUnitScriptOrders");
    lua_pushcfunction(L, G_LuaEngineTelegraph);
    lua_setglobal(L, "EngineTelegraph");
    lua_pushcfunction(L, G_LuaEngineTelegraphStop);
    lua_setglobal(L, "EngineTelegraphStop");
    lua_pushglobaltable(L);
    lua_newtable(L);
    lua_pushcfunction(L, G_LuaGlobalIndex);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);

    if (luaL_dostring(L, g_luaprelude) != LUA_OK) {
        fprintf(stderr, "Lua prelude: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void G_LuaShutdown(void) {
    if (!g_lua) return;
    lua_close(g_lua); /* runs handle __gc, which releases VM references: close before the VM */
    g_lua = NULL;
    g_luajass = NULL;
}

/* Expose a Lua global function to the VM under the same name (main, config). */
static void G_LuaExportEntry(lua_State *L, cstring_t name) {
    if (lua_getglobal(L, name) == LUA_TFUNCTION) G_LuaFunctionToJass(L, -1, name, true);
    lua_pop(L, 1);
}

bool G_LuaRunMapScript(cstring_t source) {
    lua_State *L;
    bool ok;

    G_LuaShutdown();
    G_LuaOpen();
    L = g_lua;
    lua_pushcfunction(L, G_LuaTraceback);
    ok = luaL_loadbuffer(L, source, strlen(source), "=war3map.lua") == LUA_OK &&
         lua_pcall(L, 0, 0, -2) == LUA_OK;
    if (!ok) fprintf(stderr, "war3map.lua: %s\n", lua_tostring(L, -1));
    lua_settop(L, 0);
    G_LuaExportEntry(L, "config");
    G_LuaExportEntry(L, "main");
    return ok;
}

/* wc3_lua_script: comma-separated Lua files (game data paths) run on top of any
 * map, after its own script. A Lua main/config replaces the map's JASS one, so
 * an official map serves as the arena while the game logic stays in Lua. */
void G_LuaRunScriptOverlay(void) {
    cstring_t list = gi.CvarString ? gi.CvarString("wc3_lua_script", "") : "";
    char path[MAX_PATHLEN];
    lua_State *L;

    if (!list || !*list) return;
    if (!g_lua) G_LuaOpen();
    L = g_lua;
    while (*list) {
        cstring_t end = strchr(list, ',');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        uint32_t size = 0;
        handle_t data;
        snprintf(path, sizeof(path), "%.*s", (int)MIN(len, sizeof(path) - 1), list);
        list += len + (end ? 1 : 0);
        if (!path[0]) continue;
        data = gi.ReadFile(path, &size);
        if (!data) {
            fprintf(stderr, "wc3_lua_script: cannot read %s\n", path);
            continue;
        }
        lua_pushcfunction(L, G_LuaTraceback);
        if (luaL_loadbuffer(L, data, size, path) != LUA_OK || lua_pcall(L, 0, 0, -2) != LUA_OK)
            fprintf(stderr, "%s: %s\n", path, lua_tostring(L, -1));
        lua_settop(L, 0);
        gi.MemFree(data);
    }
    G_LuaExportEntry(L, "config");
    G_LuaExportEntry(L, "main");
}

/* Preloader runs the Lua block of a Reforged-style preload file (the
 * //!beginusercode section FileIO libraries write) in the map's state. */
bool G_LuaRunChunk(cstring_t source, size_t size, cstring_t name) {
    lua_State *L = g_lua;
    bool ok;
    if (!L || !source) return false;
    lua_pushcfunction(L, G_LuaTraceback);
    ok = luaL_loadbuffer(L, source, size, name) == LUA_OK && lua_pcall(L, 0, 0, -2) == LUA_OK;
    if (!ok) fprintf(stderr, "%s: %s\n", name, lua_tostring(L, -1));
    lua_pop(L, ok ? 1 : 2);
    return ok;
}

bool G_LuaHasFunction(cstring_t name) {
    bool found;
    if (!g_lua) return false;
    found = lua_getglobal(g_lua, name) == LUA_TFUNCTION;
    lua_pop(g_lua, 1);
    return found;
}
