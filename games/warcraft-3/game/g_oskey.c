/*
 * g_oskey.c — Reforged keyboard events (BlzTriggerRegisterPlayerKeyEvent).
 *
 * Registering a key sets PS_UIFLAG_FORWARD_KEYS for that player's client, which
 * then reports every gameplay key as "oskey <engine-key> <down> <mods>". Keys
 * are matched as Windows virtual-key codes, the values of Reforged's OSKEY_*.
 */
#include "g_local.h"

#define OSKEY_MAX_EVENTS 512 // registrations; the survivor script registers 92 (46 bindable keys, press and release)

/* Engine key codes from client/keys.h; the game module does not include client headers. */
#define OSKEY_ENGINE_F1 128
#define OSKEY_ENGINE_UP 140

typedef struct {
    trigger_t *trigger;
    uint32_t player;
    int32_t vk;
    bool down;
} osKeyEvent_t;

static struct {
    osKeyEvent_t events[OSKEY_MAX_EVENTS];
    uint32_t num_events;
    int32_t trigger_key;
    bool trigger_down;
    uint32_t trigger_mods;
} oskey;

void G_OsKeyReset(void) {
    memset(&oskey, 0, sizeof(oskey));
}

/* SDL-style engine key code -> Windows virtual-key code. */
static int32_t G_OsKeyFromEngine(int32_t key) {
    if (key >= 'a' && key <= 'z') return key - 'a' + 'A';
    if (key >= '0' && key <= '9') return key;
    if (key >= OSKEY_ENGINE_F1 && key < OSKEY_ENGINE_F1 + 12) return 0x70 + key - OSKEY_ENGINE_F1;
    switch (key) {
        case 8: return 0x08;   /* backspace */
        case 9: return 0x09;   /* tab */
        case 13: return 0x0D;  /* enter */
        case 27: return 0x1B;  /* escape */
        case 32: return 0x20;  /* space */
        case OSKEY_ENGINE_UP: return 0x26;
        case OSKEY_ENGINE_UP + 1: return 0x28; /* down */
        case OSKEY_ENGINE_UP + 2: return 0x25; /* left */
        case OSKEY_ENGINE_UP + 3: return 0x27; /* right */
        default: return 0;
    }
}

static void G_OsKeyForward(uint32_t player) {
    FOR_LOOP(i, MIN((uint32_t)game.max_clients, (uint32_t)MAX_CLIENTS)) {
        gameClient_t *client = &game.clients[i];
        if (client->connected && client->ps.number == player)
            client->ps.uiflags |= PS_UIFLAG_FORWARD_KEYS;
    }
}

void G_OsKeyRegister(trigger_t *trigger, uint32_t player, int32_t vk, bool down) {
    if (!trigger) return;
    if (oskey.num_events >= OSKEY_MAX_EVENTS) {
        fprintf(stderr, "BlzTriggerRegisterPlayerKeyEvent: registration table full (%u)\n", OSKEY_MAX_EVENTS);
        return;
    }
    oskey.events[oskey.num_events++] = (osKeyEvent_t){ trigger, player, vk, down };
    G_OsKeyForward(player);
}

/* A reconnecting client starts with clean uiflags; restore forwarding for its registrations. */
void G_OsKeyClientBegin(edict_t *ent) {
    if (!ent || !ent->client) return;
    FOR_LOOP(i, oskey.num_events) {
        if (oskey.events[i].player == ent->client->ps.number) {
            ent->client->ps.uiflags |= PS_UIFLAG_FORWARD_KEYS;
            return;
        }
    }
}

int32_t G_OsKeyTrigger(void) { return oskey.trigger_key; }
bool G_OsKeyTriggerDown(void) { return oskey.trigger_down; }
uint32_t G_OsKeyTriggerMods(void) { return oskey.trigger_mods; }

void CMD_OsKey(edict_t *ent, uint32_t argc, cstring_t argv[]) {
    uint32_t player, count = oskey.num_events;
    int32_t vk;
    bool down;

    if (!ent || !ent->client || argc < 3) return;
    player = ent->client->ps.number;
    vk = G_OsKeyFromEngine(atoi(argv[1]));
    down = atoi(argv[2]) != 0;
    if (!vk) return;
    oskey.trigger_key = vk;
    oskey.trigger_down = down;
    oskey.trigger_mods = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
    FOR_LOOP(i, count) {
        osKeyEvent_t const *reg = &oskey.events[i];
        if (reg->player != player || reg->vk != vk || reg->down != down) continue;
        if (jass_triggerdisabled(reg->trigger)) continue;
        jass_executetriggerforplayer(level.vm, reg->trigger, &ent->client->ps);
    }
}
