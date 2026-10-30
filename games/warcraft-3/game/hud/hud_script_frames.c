/*
 * Reforged script frames: the server pool behind BlzCreateFrameByType.
 *
 * Maps build whole interfaces from FRAME groups, BACKDROP textures, TEXT
 * labels and GLUETEXTBUTTON click areas. The pool keeps their tree, anchors,
 * visibility and styling; when anything changes the visible frames are
 * flattened into absolute rectangles and sent as one ordered layer
 * (UIFLAG_PAINT_ORDERED keeps tree order instead of per-type passes).
 * A click arrives as "scriptframe <slot> <serial>" and fires the triggers
 * registered with BlzTriggerRegisterFrameEvent.
 */
#include "hud_local.h"

#define SF_MAX_EVENTS 8192 // frame-event registrations; a survivor lobby registers three per click area
#define SF_EVENT_CONTROL_CLICK 1 // FRAMEEVENT_CONTROL_CLICK value in common_reforged declarations
#define SF_EVENT_MOUSE_ENTER 2
#define SF_EVENT_MOUSE_LEAVE 3
#define SF_ORIGIN_WORLD_FRAME 22 // ORIGIN_FRAME_WORLD_FRAME value
#define SF_DEFAULT_FONT "Fonts\\FRIZQT__.TTF"

typedef struct {
    trigger_t *trigger;
    int32_t frame;
    int32_t event;
} scriptFrameEvent_t;

static struct {
    scriptFrame_t frames[MAX_SCRIPT_FRAMES];
    scriptFrameEvent_t events[SF_MAX_EVENTS];
    uint32_t num_events;
    uint32_t serial;
    bool dirty;
    bool origin_hidden;
    bool world_full;
    int32_t trigger_frame;
    int32_t trigger_event;
    int32_t child[MAX_SCRIPT_FRAMES];
    int32_t sibling[MAX_SCRIPT_FRAMES];
} sf;

static void SF_ApplyFullWorld(void);

static int32_t SF_Index(scriptFrame_t const *frame) {
    return frame ? (int32_t)(frame - sf.frames) : -1;
}

scriptFrame_t *G_ScriptFrameValid(handle_t handle) {
    scriptFrame_t *frame = handle;
    if (!frame || frame < sf.frames || frame >= sf.frames + MAX_SCRIPT_FRAMES || !frame->used) return NULL;
    return frame;
}

void G_ScriptFramesDirty(void) {
    sf.dirty = true;
}

void G_ScriptFramesReset(void) {
    FOR_LOOP(i, MAX_SCRIPT_FRAMES) SAFE_DELETE(sf.frames[i].text, gi.MemFree);
    memset(&sf, 0, sizeof(sf));
    SF_ApplyFullWorld(); /* a new map starts with the world above the console again */
    sf.trigger_frame = -1;
    /* Origin frames cover the 0.8 x 0.6 canvas; maps reach them through BlzGetOriginFrame. */
    FOR_LOOP(i, SF_ORIGIN_COUNT) {
        scriptFrame_t *origin = &sf.frames[i];
        origin->used = origin->visible = origin->enabled = true;
        origin->kind = SF_ORIGIN;
        origin->parent = origin->fill = -1;
        origin->alpha = 255;
        origin->ax = 0.0f; origin->ay = UI_BASE_HEIGHT;
        origin->w = UI_BASE_WIDTH; origin->h = UI_BASE_HEIGHT;
        origin->serial = ++sf.serial;
    }
}

scriptFrame_t *G_ScriptFrameOrigin(int32_t type) {
    return type >= 0 && type < SF_ORIGIN_COUNT ? &sf.frames[type] : NULL;
}

scriptFrame_t *G_ScriptFrameByName(cstring_t name) {
    if (!name || !*name) return NULL;
    FOR_LOOP(i, MAX_SCRIPT_FRAMES) {
        scriptFrame_t *frame = &sf.frames[i];
        if (frame->used && frame->kind != SF_ORIGIN && !strcmp(frame->name, name)) return frame;
    }
    return NULL;
}

static uint8_t SF_KindFromType(cstring_t type) {
    if (!type) return SF_FRAME;
    if (!strcasecmp(type, "BACKDROP") || !strcasecmp(type, "SIMPLETEXTURE")) return SF_BACKDROP;
    if (!strcasecmp(type, "TEXT") || !strcasecmp(type, "SIMPLEFONTSTRING")) return SF_TEXT;
    if (!strcasecmp(type, "SPRITE") || !strcasecmp(type, "MODEL")) return SF_MODEL;
    if (strstr(type, "BUTTON") || strstr(type, "button")) return SF_BUTTON;
    return SF_FRAME;
}

/* Only BACKDROP templates carry over (EscMenuButtonBackdropTemplate,
 * QuestButtonBaseTemplate, ...): their background, edge file and corner size
 * give script panels the stock bordered look. Button templates are ignored;
 * script click areas draw their own look underneath. */
scriptFrame_t *G_ScriptFrameCreate(cstring_t type, cstring_t name, scriptFrame_t *parent, cstring_t inherits) {
    for (uint32_t i = SF_ORIGIN_COUNT; i < MAX_SCRIPT_FRAMES; i++) {
        scriptFrame_t *frame = &sf.frames[i];
        if (frame->used) continue;
        memset(frame, 0, sizeof(*frame));
        frame->used = frame->visible = frame->enabled = true;
        frame->kind = SF_KindFromType(type);
        frame->parent = parent ? SF_Index(parent) : 0;
        frame->fill = -1;
        frame->alpha = 255;
        frame->color = COLOR32_WHITE;
        frame->halign = 3; /* TEXT_JUSTIFY_LEFT */
        frame->valign = 0; /* TEXT_JUSTIFY_TOP */
        frame->serial = ++sf.serial;
        strlcpy(frame->name, name ? name : "", sizeof(frame->name));
        if (frame->kind == SF_BACKDROP && inherits && *inherits) {
            frame->templated = UI_BackdropTemplate(inherits, &frame->backdrop);
            if (!frame->templated) fprintf(stderr, "BlzCreateFrameByType: no BACKDROP template %s\n", inherits);
        }
        sf.dirty = true;
        return frame;
    }
    fprintf(stderr, "BlzCreateFrameByType: script frame pool exhausted (%u)\n", MAX_SCRIPT_FRAMES);
    return NULL;
}

void G_ScriptFrameDestroy(scriptFrame_t *frame) {
    int32_t index = SF_Index(frame);
    if (!frame || frame->kind == SF_ORIGIN) return;
    FOR_LOOP(i, MAX_SCRIPT_FRAMES) {
        if (sf.frames[i].used && sf.frames[i].parent == index) G_ScriptFrameDestroy(&sf.frames[i]);
    }
    SAFE_DELETE(frame->text, gi.MemFree);
    frame->used = false;
    sf.dirty = true;
}

/* Reforged frame points: 0 TOPLEFT .. 8 BOTTOMRIGHT, row-major from the top. */
static void SF_PointFraction(int32_t point, float *fx, float *fy) {
    point = MAX(0, MIN(point, 8));
    *fx = (float)(point % 3) * 0.5f;
    *fy = (float)(point / 3) * 0.5f;
}

/* Top-left corner and size in Reforged screen units. */
static void SF_Rect(scriptFrame_t const *frame, float *left, float *top, float *w, float *h, uint32_t depth) {
    float fx, fy;
    if (frame->fill >= 0 && depth < 16 && sf.frames[frame->fill].used) {
        SF_Rect(&sf.frames[frame->fill], left, top, w, h, depth + 1);
        return;
    }
    SF_PointFraction(frame->point, &fx, &fy);
    *w = frame->w;
    *h = frame->h;
    *left = frame->ax - fx * frame->w;
    *top = frame->ay + fy * frame->h;
}

vec2_t G_ScriptFrameAnchor(scriptFrame_t *frame, int32_t point) {
    float left, top, w, h, fx, fy;
    if (!frame) return (vec2_t){ 0, 0 };
    SF_Rect(frame, &left, &top, &w, &h, 0);
    SF_PointFraction(point, &fx, &fy);
    return (vec2_t){ left + fx * w, top - fy * h };
}

void G_ScriptFrameSetPoint(scriptFrame_t *frame, int32_t point, float x, float y) {
    if (!frame || frame->kind == SF_ORIGIN) return;
    frame->point = (uint8_t)MAX(0, MIN(point, 8));
    frame->ax = x;
    frame->ay = y;
    frame->fill = -1;
    sf.dirty = true;
}

static void SF_ApplyFullWorld(void) {
    if (!game.clients) return;
    FOR_LOOP(i, MIN((uint32_t)game.max_clients, (uint32_t)MAX_CLIENTS)) {
        gameClient_t *client = &game.clients[i];
        if (!client->connected) continue;
        if (sf.world_full) client->ps.uiflags |= PS_UIFLAG_FULL_WORLD;
        else client->ps.uiflags &= ~PS_UIFLAG_FULL_WORLD;
    }
}

/* SetAllPoints copies another frame's rectangle live; NULL clears it (ClearAllPoints).
 * Origin frames keep their stock place, except the world frame: stretched over
 * the game UI it lets the 3D view fill the window instead of stopping above
 * the console (maps do this after BlzHideOriginFrames). */
void G_ScriptFrameFill(scriptFrame_t *frame, scriptFrame_t *relative) {
    if (frame && SF_Index(frame) == SF_ORIGIN_WORLD_FRAME) {
        sf.world_full = relative != NULL && relative->kind == SF_ORIGIN;
        SF_ApplyFullWorld();
        return;
    }
    if (!frame || frame->kind == SF_ORIGIN) return;
    frame->fill = relative && relative != frame ? SF_Index(relative) : -1;
    sf.dirty = true;
}

void G_ScriptFrameSetSize(scriptFrame_t *frame, float w, float h) {
    if (!frame || frame->kind == SF_ORIGIN) return;
    frame->w = w;
    frame->h = h;
    sf.dirty = true;
}

void G_ScriptFrameSetText(scriptFrame_t *frame, cstring_t text) {
    size_t len;
    if (!frame) return;
    text = text ? text : "";
    if (frame->text && !strcmp(frame->text, text)) return;
    SAFE_DELETE(frame->text, gi.MemFree);
    len = strlen(text);
    frame->text = gi.MemAlloc((long)len + 1);
    memcpy(frame->text, text, len + 1);
    sf.dirty = true;
}

/* BlzFrameSetModel on a SPRITE/MODEL frame: the model plays Stand inside the
 * frame, seen from the front and a little to its right, with a camera that
 * fits its bind-pose mesh (the origin is at the feet; models face +X). The
 * Stand sequence extents are only a fallback: many models pad them far beyond
 * the mesh, which left those heroes small in the middle of the frame. */
void G_ScriptFrameSetTeamColor(scriptFrame_t *frame, int32_t color) {
    if (!frame || frame->kind != SF_MODEL) return;
    frame->team = (uint8_t)MAX(0, MIN(color, MAX_PLAYERS - 1));
    sf.dirty = true;
}

void G_ScriptFrameSetModel(scriptFrame_t *frame, cstring_t model) {
    PATHSTR path;
    char *dot;
    animation_t const *stand;
    vec3_t lo = { -50, -50, 0 }, hi = { 50, 50, 180 }, center;
    float height, width, depth, aspect, half, distance, yaw = DEG2RAD(-20.0f);
    if (!frame || frame->kind != SF_MODEL) return;
    frame->model = 0;
    sf.dirty = true;
    if (!model || !*model) return;
    G_NormalizeModelFilename(model, path, sizeof(path));
    dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".mdl")) strcpy(dot, ".mdx"); /* game data ships the binary models */
    frame->model = G_RegisterModel(path);
    if (!frame->model) return;
    stand = G_GetAnimation(frame->model, "Stand");
    if (!G_GetModelBindBounds(frame->model, &lo, &hi) || hi.z - lo.z <= 1.0f) {
        lo = (vec3_t){ -50, -50, 0 };
        hi = (vec3_t){ 50, 50, 180 };
        if (stand && stand->max.z - stand->min.z > 1.0f) {
            lo = stand->min;
            hi = stand->max;
        }
    }
    height = hi.z - lo.z;
    /* Aimed a little above the middle: the head keeps room, the glow at the feet may crop. */
    center = (vec3_t){ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f + height * 0.1f };
    /* Width across the view and depth along it, for a camera at this yaw. */
    width = fabsf(cosf(yaw)) * (hi.y - lo.y) + fabsf(sinf(yaw)) * (hi.x - lo.x);
    depth = fabsf(cosf(yaw)) * (hi.x - lo.x) + fabsf(sinf(yaw)) * (hi.y - lo.y);
    aspect = frame->h > 0.0f ? frame->w / frame->h : 1.0f;
    frame->camera.fov = 30.0f;
    half = tanf(DEG2RAD(frame->camera.fov) * 0.5f);
    /* Fit the height with headroom (Stand lifts floating heroes above their
     * bind pose); wide props such as lances and wings may crop at the sides
     * rather than shrink the hero. */
    distance = MAX(height * 0.5f / half * 1.2f, width * 0.5f / (half * MAX(aspect, 0.1f)) * 0.8f) + depth * 0.35f;
    frame->camera.target = center;
    frame->camera.eye = (vec3_t){ center.x + cosf(yaw) * distance, center.y + sinf(yaw) * distance,
                                  center.z + height * 0.08f };
    frame->camera.pos = (vec3_t){ 0, 0, 0 };
    frame->camera.scale = (vec3_t){ 1, 1, 1 };
    frame->camera.znear = MAX(1.0f, distance - depth * 2.0f);
    frame->camera.zfar = distance + depth * 2.0f + 1000.0f;
    frame->camera.aspect = 1.0f;
    frame->camera.projection = UI_MODEL_PERSPECTIVE;
}

/* BlzHideOriginFrames hides the stock console layers; the world view stays. */
void G_ScriptFrameHideOrigin(bool hide) {
    sf.origin_hidden = hide;
    FOR_LOOP(i, MIN((uint32_t)game.max_clients, (uint32_t)MAX_CLIENTS)) {
        gameClient_t *client = &game.clients[i];
        uint32_t bits = (1u << LAYER_PORTRAIT) | (1u << LAYER_CONSOLE) | (1u << LAYER_COMMANDBAR) |
                        (1u << LAYER_INFOPANEL) | (1u << LAYER_INVENTORY) | (1u << LAYER_UNIT_SHORTCUTS);
        if (!client->connected) continue;
        if (hide) client->ps.uiflags |= bits;
        else client->ps.uiflags &= ~bits;
    }
}

void G_ScriptFrameRegisterEvent(trigger_t *trigger, scriptFrame_t *frame, int32_t event) {
    if (!trigger || !frame) return;
    if (sf.num_events >= SF_MAX_EVENTS) {
        fprintf(stderr, "BlzTriggerRegisterFrameEvent: registration table full (%u)\n", SF_MAX_EVENTS);
        return;
    }
    sf.events[sf.num_events++] = (scriptFrameEvent_t){ trigger, SF_Index(frame), event };
    if (event == SF_EVENT_CONTROL_CLICK) sf.dirty = true; /* the frame becomes clickable */
}

scriptFrame_t *G_ScriptFrameTrigger(void) {
    return sf.trigger_frame >= 0 ? &sf.frames[sf.trigger_frame] : NULL;
}

int32_t G_ScriptFrameTriggerEvent(void) {
    return sf.trigger_event;
}

static bool SF_HasClickEvent(int32_t index) {
    FOR_LOOP(i, sf.num_events) {
        if (sf.events[i].frame == index && sf.events[i].event == SF_EVENT_CONTROL_CLICK) return true;
    }
    return false;
}

/* Runs the registered triggers synchronously so BlzGetTriggerFrame stays valid. */
static void SF_Fire(int32_t index, int32_t event) {
    int32_t previous_frame = sf.trigger_frame, previous_event = sf.trigger_event;
    uint32_t count = sf.num_events;
    sf.trigger_frame = index;
    sf.trigger_event = event;
    FOR_LOOP(i, count) {
        scriptFrameEvent_t const *reg = &sf.events[i];
        if (reg->frame != index || reg->event != event || !reg->trigger) continue;
        if (jass_triggerdisabled(reg->trigger)) continue;
        if (!jass_evaluatetrigger(level.vm, reg->trigger, NULL)) continue;
        jass_executetrigger(level.vm, reg->trigger, NULL);
    }
    sf.trigger_frame = previous_frame;
    sf.trigger_event = previous_event;
}

void CMD_ScriptFrameEvent(edict_t *ent, uint32_t argc, cstring_t argv[]) {
    int32_t index;
    uint32_t serial;
    scriptFrame_t *frame;
    if (!ent || !ent->client || argc != 3) return;
    index = atoi(argv[1]);
    serial = (uint32_t)strtoul(argv[2], NULL, 10);
    if (index < SF_ORIGIN_COUNT || index >= MAX_SCRIPT_FRAMES) return;
    frame = &sf.frames[index];
    /* The serial rejects clicks aimed at a slot that has since been reused. */
    if (!frame->used || frame->serial != serial || !frame->enabled) return;
    SF_Fire(index, SF_EVENT_CONTROL_CLICK);
}

/* "scriptframehover <slot> <serial> <1|0>": the client's pointer entered or left a click area. */
void CMD_ScriptFrameHover(edict_t *ent, uint32_t argc, cstring_t argv[]) {
    int32_t index;
    scriptFrame_t *frame;
    if (!ent || !ent->client || argc != 4) return;
    index = atoi(argv[1]);
    if (index < SF_ORIGIN_COUNT || index >= MAX_SCRIPT_FRAMES) return;
    frame = &sf.frames[index];
    if (!frame->used || frame->serial != (uint32_t)strtoul(argv[2], NULL, 10)) return;
    SF_Fire(index, atoi(argv[3]) ? SF_EVENT_MOUSE_ENTER : SF_EVENT_MOUSE_LEAVE);
}

/* =========================================================================
 * Layer authoring
 * ========================================================================= */

static bool SF_Before(scriptFrame_t const *a, scriptFrame_t const *b) {
    return a->level != b->level ? a->level < b->level : a->serial < b->serial;
}

/* Children lists sorted by (level, creation order): later and higher frames paint above. */
static void SF_BuildTree(void) {
    FOR_LOOP(i, MAX_SCRIPT_FRAMES) sf.child[i] = sf.sibling[i] = -1;
    FOR_LOOP(i, MAX_SCRIPT_FRAMES) {
        scriptFrame_t const *frame = &sf.frames[i];
        int32_t *link;
        if (!frame->used || frame->parent < 0 || frame->parent >= MAX_SCRIPT_FRAMES) continue;
        link = &sf.child[frame->parent];
        while (*link >= 0 && SF_Before(&sf.frames[*link], frame)) link = &sf.sibling[*link];
        sf.sibling[i] = *link;
        *link = (int32_t)i;
    }
}

static uiFontJustificationH_t SF_AlignH(uint8_t align) {
    switch (align) {
        case 4: return FONT_JUSTIFYCENTER;
        case 5: return FONT_JUSTIFYRIGHT;
        default: return FONT_JUSTIFYLEFT;
    }
}

static uiFontJustificationV_t SF_AlignV(uint8_t align) {
    switch (align) {
        case 1: return FONT_JUSTIFYMIDDLE;
        case 2: return FONT_JUSTIFYBOTTOM;
        default: return FONT_JUSTIFYTOP;
    }
}

static void SF_WriteFrame(int32_t index, float alpha) {
    scriptFrame_t const *frame = &sf.frames[index];
    uiFrame_t out = { 0 };
    uiLabel_t label = { 0 };
    float left, top, w, h;
    char command[48];

    SF_Rect(frame, &left, &top, &w, &h, 0);
    out.flagsvalue |= UIFLAG_PAINT_ORDERED;
    UI_SetFrameRect(&out, left, UI_BASE_HEIGHT - top, w, h);
    switch (frame->kind) {
        case SF_BACKDROP: {
            uint8_t a = (uint8_t)(frame->color.a * alpha);
            if (frame->templated && a) {
                uiBackdrop_t backdrop = frame->backdrop;
                /* BlzFrameSetTexture on a templated backdrop replaces its background only. */
                if (frame->texture[0]) backdrop.Background = gi.ImageIndex(frame->texture);
                out.flags.type = FT_BACKDROP;
                out.color = frame->color;
                out.color.a = a;
                UI_WriteProxyFrame(&out, &backdrop, sizeof(backdrop));
                break;
            }
            if (!frame->texture[0] || !a) return;
            out.flags.type = FT_TEXTURE;
            out.tex.index = gi.ImageIndex(frame->texture);
            out.color = frame->color;
            out.color.a = a;
            UI_WriteProxyFrame(&out, NULL, 0);
            break;
        }
        case SF_TEXT: {
            uint8_t a = (uint8_t)(255 * alpha);
            if (!frame->text || !*frame->text || !a) return;
            out.flags.type = FT_STRING;
            out.text = frame->text;
            out.color = (color32_t){ 255, 255, 255, a };
            label.font = gi.FontIndex(frame->font[0] ? frame->font : SF_DEFAULT_FONT,
                                      (uint32_t)MAX(1.0f, frame->fontheight * UI_FONT_COORD_SCALE + 0.5f));
            label.textalignx = SF_AlignH(frame->halign);
            label.textaligny = SF_AlignV(frame->valign);
            UI_WriteProxyFrame(&out, &label, sizeof(label));
            break;
        }
        case SF_BUTTON:
            if (!SF_HasClickEvent(index)) return;
            snprintf(command, sizeof(command), "scriptframe %d %u", index, frame->serial);
            out.flags.type = FT_STRING;
            out.text = alpha > 0.0f && frame->text ? frame->text : "";
            out.onclick = command;
            if (!frame->enabled) out.flagsvalue |= UIFRAME_DISABLED;
            label.font = gi.FontIndex(SF_DEFAULT_FONT, HUD_FONT_SIZE);
            label.textalignx = FONT_JUSTIFYCENTER;
            label.textaligny = FONT_JUSTIFYMIDDLE;
            UI_WriteProxyFrame(&out, &label, sizeof(label));
            break;
        case SF_MODEL: {
            uiModel_t camera = frame->camera;
            if (!frame->model || alpha <= 0.0f) return;
            out.flags.type = FT_MODEL;
            out.tex.index = (uint32_t)frame->model;
            out.stat = frame->team; /* player color of the team-colored parts */
            out.text = "Stand";
            UI_WriteProxyFrame(&out, &camera, sizeof(camera));
            break;
        }
        default:
            break;
    }
}

static void SF_WriteTree(int32_t index, float alpha, uint32_t depth) {
    if (depth > 64) return;
    for (int32_t child = sf.child[index]; child >= 0; child = sf.sibling[child]) {
        scriptFrame_t const *frame = &sf.frames[child];
        float child_alpha;
        if (!frame->visible) continue;
        child_alpha = alpha * (frame->alpha / 255.0f);
        SF_WriteFrame(child, child_alpha);
        SF_WriteTree(child, child_alpha, depth + 1);
    }
}

static void UI_WriteScriptFrames(edict_t *ent) {
    UI_WriteStart(WC3_LAYER_SCRIPTFRAMES);
    FOR_LOOP(i, SF_ORIGIN_COUNT) {
        if (sf.frames[i].visible) SF_WriteTree((int32_t)i, 1.0f, 0);
    }
    UI_WriteEnd(ent);
}

static bool SF_Any(void) {
    for (uint32_t i = SF_ORIGIN_COUNT; i < MAX_SCRIPT_FRAMES; i++) if (sf.frames[i].used) return true;
    return false;
}

void G_UpdateScriptFrames(void) {
    if (!sf.dirty) return;
    sf.dirty = false;
    if (!SF_Any()) return;
    SF_BuildTree();
    FOR_LOOP(i, MIN((uint32_t)game.max_clients, (uint32_t)MAX_CLIENTS)) {
        edict_t *ent;
        if (!game.clients[i].connected) continue;
        ent = G_GetPlayerEntityByNumber(game.clients[i].ps.number);
        if (ent && ent->client) UI_WriteScriptFrames(ent);
    }
}

/* A client that joins after the map built its UI needs the current layer and console state. */
void G_ScriptFramesClientBegin(edict_t *ent) {
    (void)ent;
    if (sf.origin_hidden) G_ScriptFrameHideOrigin(true);
    if (sf.world_full) SF_ApplyFullWorld();
    sf.dirty = true;
}
