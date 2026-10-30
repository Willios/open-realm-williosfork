/*
 * api_reforged.h — natives newer than the 1.29 common.j (frames, keys, unit/effect setters).
 *
 * Covers the subset maps use to build their own UI from code: FRAME groups,
 * BACKDROP textures, TEXT labels and GLUETEXTBUTTON click areas, placed with
 * absolute points or relative to another frame's rectangle. Frames live in the
 * server pool owned by hud_script_frames.c, which draws them on their own
 * layer and turns clicks back into FRAMEEVENT triggers.
 */

static scriptFrame_t *JassFrame(jass_t *j, int index) {
    return G_ScriptFrameValid(jass_checkhandle(j, index, "framehandle"));
}

static int32_t JassEnum(jass_t *j, int index) {
    int32_t const *value = jass_checkhandle(j, index, "handle");
    return value ? *value : 0;
}

CONVERT_FUNC(OriginFrameType, originframetype);
CONVERT_FUNC(FramePointType, framepointtype);
CONVERT_FUNC(TextAlignType, textaligntype);
CONVERT_FUNC(FrameEventType, frameeventtype);

uint32_t BlzGetOriginFrame(jass_t *j) {
    return jass_pushlighthandle(j, G_ScriptFrameOrigin(JassEnum(j, 1)), "framehandle");
}

uint32_t BlzHideOriginFrames(jass_t *j) {
    G_ScriptFrameHideOrigin(jass_checkboolean(j, 1));
    return 0;
}

uint32_t BlzGetFrameByName(jass_t *j) {
    return jass_pushlighthandle(j, G_ScriptFrameByName(jass_checkstring(j, 1)), "framehandle");
}

uint32_t BlzCreateFrameByType(jass_t *j) {
    scriptFrame_t *frame = G_ScriptFrameCreate(jass_checkstring(j, 1), jass_checkstring(j, 2),
                                               JassFrame(j, 3), jass_checkstring(j, 4));
    return jass_pushlighthandle(j, frame, "framehandle");
}

uint32_t BlzCreateSimpleFrame(jass_t *j) {
    scriptFrame_t *frame = G_ScriptFrameCreate("FRAME", jass_checkstring(j, 1), JassFrame(j, 2), "");
    return jass_pushlighthandle(j, frame, "framehandle");
}

uint32_t BlzDestroyFrame(jass_t *j) {
    G_ScriptFrameDestroy(JassFrame(j, 1));
    return 0;
}

uint32_t BlzFrameClearAllPoints(jass_t *j) {
    G_ScriptFrameFill(JassFrame(j, 1), NULL);
    return 0;
}

uint32_t BlzFrameSetAbsPoint(jass_t *j) {
    G_ScriptFrameSetPoint(JassFrame(j, 1), JassEnum(j, 2), jass_checknumber(j, 3), jass_checknumber(j, 4));
    return 0;
}

uint32_t BlzFrameSetPoint(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    scriptFrame_t *relative = JassFrame(j, 3);
    vec2_t anchor = relative ? G_ScriptFrameAnchor(relative, JassEnum(j, 4)) : (vec2_t){ 0, 0 };
    G_ScriptFrameSetPoint(frame, JassEnum(j, 2), anchor.x + jass_checknumber(j, 5), anchor.y + jass_checknumber(j, 6));
    return 0;
}

uint32_t BlzFrameSetAllPoints(jass_t *j) {
    G_ScriptFrameFill(JassFrame(j, 1), JassFrame(j, 2));
    return 0;
}

uint32_t BlzFrameSetSize(jass_t *j) {
    G_ScriptFrameSetSize(JassFrame(j, 1), jass_checknumber(j, 2), jass_checknumber(j, 3));
    return 0;
}

uint32_t BlzFrameSetVisible(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    bool visible = jass_checkboolean(j, 2);
    if (frame && frame->visible != visible) { frame->visible = visible; G_ScriptFramesDirty(); }
    return 0;
}

uint32_t BlzFrameIsVisible(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    return jass_pushboolean(j, frame && frame->visible);
}

uint32_t BlzFrameSetEnable(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    bool enabled = jass_checkboolean(j, 2);
    if (frame && frame->enabled != enabled) { frame->enabled = enabled; G_ScriptFramesDirty(); }
    return 0;
}

uint32_t BlzFrameGetEnable(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    return jass_pushboolean(j, frame && frame->enabled);
}

uint32_t BlzFrameSetText(jass_t *j) {
    G_ScriptFrameSetText(JassFrame(j, 1), jass_checkstring(j, 2));
    return 0;
}

uint32_t BlzFrameGetText(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    return jass_pushstring(j, frame && frame->text ? frame->text : "");
}

uint32_t BlzFrameSetTextAlignment(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    if (frame) {
        frame->valign = (uint8_t)JassEnum(j, 2);
        frame->halign = (uint8_t)JassEnum(j, 3);
        G_ScriptFramesDirty();
    }
    return 0;
}

uint32_t BlzFrameSetFont(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    if (frame) {
        strlcpy(frame->font, jass_checkstring(j, 2), sizeof(frame->font));
        frame->fontheight = jass_checknumber(j, 3);
        G_ScriptFramesDirty();
    }
    return 0;
}

uint32_t BlzFrameSetTexture(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    if (frame) {
        strlcpy(frame->texture, jass_checkstring(j, 2), sizeof(frame->texture));
        /* blend: the background keeps its alpha (a templated backdrop may be given a clear one). */
        if (frame->templated) frame->backdrop.Opaque = !jass_checkboolean(j, 4);
        G_ScriptFramesDirty();
    }
    return 0;
}

/* SPRITE/MODEL frames show the model inside their rectangle (cameraIndex is unused: the camera fits the model). */
uint32_t BlzFrameSetModel(jass_t *j) {
    G_ScriptFrameSetModel(JassFrame(j, 1), jass_checkstring(j, 2));
    return 0;
}

/* Engine-only: the player color (0 red, 1 blue, ...) of a model frame's team-colored parts. */
uint32_t EngineFrameSetTeamColor(jass_t *j) {
    G_ScriptFrameSetTeamColor(JassFrame(j, 1), jass_checkinteger(j, 2));
    return 0;
}

uint32_t BlzFrameSetVertexColor(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    uint32_t argb = (uint32_t)jass_checkinteger(j, 2);
    if (frame) {
        frame->color = (color32_t){ (argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, (argb >> 24) & 0xff };
        G_ScriptFramesDirty();
    }
    return 0;
}

uint32_t BlzFrameSetAlpha(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    if (frame) { frame->alpha = (uint8_t)MAX(0, MIN(jass_checkinteger(j, 2), 255)); G_ScriptFramesDirty(); }
    return 0;
}

uint32_t BlzFrameGetAlpha(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    return jass_pushinteger(j, frame ? frame->alpha : 0);
}

uint32_t BlzFrameSetLevel(jass_t *j) {
    scriptFrame_t *frame = JassFrame(j, 1);
    if (frame) { frame->level = (int16_t)jass_checkinteger(j, 2); G_ScriptFramesDirty(); }
    return 0;
}

uint32_t BlzFrameSetScale(jass_t *j) {
    (void)j; /* scale changes text and children uniformly; the script UI keeps 1.0 */
    return 0;
}

uint32_t BlzConvertColor(jass_t *j) {
    uint32_t a = (uint32_t)jass_checkinteger(j, 1) & 0xff;
    uint32_t r = (uint32_t)jass_checkinteger(j, 2) & 0xff;
    uint32_t g = (uint32_t)jass_checkinteger(j, 3) & 0xff;
    uint32_t b = (uint32_t)jass_checkinteger(j, 4) & 0xff;
    return jass_pushinteger(j, (int32_t)((a << 24) | (r << 16) | (g << 8) | b));
}

uint32_t BlzTriggerRegisterFrameEvent(jass_t *j) {
    trigger_t *trigger = jass_checkhandle(j, 1, "trigger");
    G_ScriptFrameRegisterEvent(trigger, JassFrame(j, 2), JassEnum(j, 3));
    return jass_pushnullhandle(j, "event");
}

uint32_t BlzGetTriggerFrame(jass_t *j) {
    return jass_pushlighthandle(j, G_ScriptFrameTrigger(), "framehandle");
}

uint32_t BlzGetTriggerFrameEvent(jass_t *j) {
    return JassPushFrameEventTypeHandle(j, G_ScriptFrameTriggerEvent());
}

/* Keyboard events (g_oskey.c) */
CONVERT_FUNC(OsKeyType, oskeytype);
CONVERT_FUNC(UnitWeaponRealField, unitweaponrealfield);
CONVERT_FUNC(UnitWeaponStringField, unitweaponstringfield);
CONVERT_FUNC(UnitWeaponIntegerField, unitweaponintegerfield);

uint32_t BlzTriggerRegisterPlayerKeyEvent(jass_t *j) {
    trigger_t *trigger = jass_checkhandle(j, 1, "trigger");
    player_t *player = jass_checkhandle(j, 2, "player");
    if (trigger && player) G_OsKeyRegister(trigger, PLAYER_NUM(player), JassEnum(j, 3), jass_checkboolean(j, 5));
    return jass_pushnullhandle(j, "event");
}

uint32_t BlzGetTriggerPlayerKey(jass_t *j) {
    return JassPushOsKeyTypeHandle(j, G_OsKeyTrigger());
}

uint32_t BlzGetTriggerPlayerIsKeyDown(jass_t *j) {
    return jass_pushboolean(j, G_OsKeyTriggerDown());
}

uint32_t BlzGetTriggerPlayerMetaKey(jass_t *j) {
    return jass_pushinteger(j, (int32_t)G_OsKeyTriggerMods());
}

/* Units */

static unitAttack_t *JassUnitWeapon(edict_t *unit, int32_t index) {
    if (!unit || !unit->unit_state) return NULL;
    return index == 1 ? &unit->unit_state->attack2 : &unit->unit_state->attack1;
}

uint32_t BlzGetUnitMaxHP(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    return jass_pushinteger(j, unit ? (int32_t)unit->health.max_value : 0);
}

uint32_t BlzSetUnitMaxHP(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    int32_t hp = jass_checkinteger(j, 2);
    if (unit && hp > 0) {
        unit->health.max_value = (float)hp;
        if (unit->health.value > unit->health.max_value) unit->health.value = unit->health.max_value;
    }
    return 0;
}

uint32_t BlzSetUnitArmor(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    float armor = jass_checknumber(j, 2);
    if (unit && unit->unit_state) G_ApplyPermanentArmorBonus(unit, armor - unit->unit_state->armor_value);
    return 0;
}

uint32_t BlzSetUnitBaseDamage(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    unitAttack_t *weapon = JassUnitWeapon(unit, jass_checkinteger(j, 3));
    if (weapon) weapon->damageBase = (uint32_t)MAX(0, jass_checkinteger(j, 2));
    return 0;
}

uint32_t BlzGetUnitAttackCooldown(jass_t *j) {
    unitAttack_t *weapon = JassUnitWeapon(jass_checkhandle(j, 1, "unit"), jass_checkinteger(j, 2));
    return jass_pushnumber(j, weapon ? weapon->cooldown : 0.0f);
}

uint32_t BlzPauseUnitEx(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    if (unit) G_SetUnitFlag(unit, UNIT_FLAG_PAUSED, jass_checkboolean(j, 2));
    return 0;
}

uint32_t BlzSetUnitFacingEx(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    if (unit) unit->s.angle = DEG2RAD(jass_checknumber(j, 2));
    return 0;
}

uint32_t BlzGetUnitZ(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    return jass_pushnumber(j, unit ? unit->s.origin.z : 0.0f);
}

uint32_t BlzSetUnitName(jass_t *j) {
    (void)j; /* names come from unit data; per-unit overrides are not stored yet */
    return 0;
}

/* Reforged weapon fields, only the ones custom attack code reads. */
#define BZ_UNIT_WEAPON_RF_ATTACK_RANGE MAKEFOURCC('u','a','1','r')
#define BZ_UNIT_WEAPON_RF_ATTACK_PROJECTILE_SPEED MAKEFOURCC('u','a','1','z')
#define BZ_UNIT_WEAPON_SF_ATTACK_PROJECTILE_ART MAKEFOURCC('u','a','1','m')
#define BZ_UNIT_WEAPON_IF_ATTACK_WEAPON_SOUND MAKEFOURCC('u','c','s','1')

/* Weapon sound (ucs1) as its weapontype value, as Reforged returns it. */
uint32_t BlzGetUnitWeaponIntegerField(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    int32_t field = JassEnum(j, 2);
    int32_t index = jass_checkinteger(j, 3);
    int32_t value = 0;
    if (unit && unit->data.UnitWeapons && field == (int32_t)BZ_UNIT_WEAPON_IF_ATTACK_WEAPON_SOUND)
        value = G_WeaponSoundType(index == 1 ? unit->data.UnitWeapons->attack2.weaponSound
                                             : unit->data.UnitWeapons->attack1.weaponSound);
    return jass_pushinteger(j, value);
}

uint32_t BlzGetUnitWeaponRealField(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    int32_t field = JassEnum(j, 2);
    unitAttack_t *weapon = JassUnitWeapon(unit, jass_checkinteger(j, 3));
    float value = 0.0f;
    if (weapon && field == (int32_t)BZ_UNIT_WEAPON_RF_ATTACK_RANGE) value = weapon->range;
    if (weapon && field == (int32_t)BZ_UNIT_WEAPON_RF_ATTACK_PROJECTILE_SPEED) value = weapon->projectile.speed;
    return jass_pushnumber(j, value);
}

uint32_t BlzGetUnitWeaponStringField(jass_t *j) {
    edict_t *unit = jass_checkhandle(j, 1, "unit");
    int32_t field = JassEnum(j, 2);
    int32_t index = jass_checkinteger(j, 3);
    cstring_t art = NULL;
    if (unit && field == (int32_t)BZ_UNIT_WEAPON_SF_ATTACK_PROJECTILE_ART)
        art = G_UnitProfile(unit->class_id)->attack[index == 1 ? 1 : 0].art;
    return jass_pushstring(j, art ? art : "");
}

/* Damage triggers run while the hit resolves (G_RunDamageTriggers), so the
 * amount set here is the damage the target takes. */
uint32_t BlzGetEventDamageTarget(jass_t *j) {
    return jass_pushlighthandle(j, jass_getcontext(j)->unit, "unit");
}

uint32_t BlzSetEventDamage(jass_t *j) {
    G_SetDamageEventAmount(jass_checknumber(j, 1));
    return 0;
}

/* Abilities */

static cstring_t JassAbilityField(int32_t code, cstring_t field) {
    char classname[5];
    memcpy(classname, &code, 4);
    classname[4] = '\0';
    return FindConfigValue(classname, field);
}

uint32_t BlzGetAbilityIcon(jass_t *j) {
    cstring_t art = JassAbilityField(jass_checkinteger(j, 1), "Art");
    return jass_pushstring(j, art ? art : "");
}

uint32_t BlzGetAbilityTooltip(jass_t *j) {
    int32_t code = jass_checkinteger(j, 1);
    cstring_t tip = G_AbilityTooltipOverride(code, jass_checkinteger(j, 2));
    if (!tip) tip = JassAbilityField(code, "Tip");
    return jass_pushstring(j, tip ? tip : "");
}

/* Kept per level and read back by BlzGetAbilityTooltip (FileIO loads data this way). */
uint32_t BlzSetAbilityTooltip(jass_t *j) {
    G_SetAbilityTooltipOverride(jass_checkinteger(j, 1), jass_checkinteger(j, 3), jass_checkstring(j, 2));
    return 0;
}

/* Special effects are edicts: these move and restyle the edict directly. */
uint32_t BlzSetSpecialEffectPosition(jass_t *j) {
    edict_t *effect = jass_checkhandle(j, 1, "effect");
    if (effect) {
        effect->s.origin = (vec3_t){ jass_checknumber(j, 2), jass_checknumber(j, 3), jass_checknumber(j, 4) };
        gi.LinkEntity(effect);
    }
    return 0;
}

uint32_t BlzSetSpecialEffectZ(jass_t *j) {
    edict_t *effect = jass_checkhandle(j, 1, "effect");
    if (effect) effect->s.origin.z = jass_checknumber(j, 2);
    return 0;
}

uint32_t BlzSetSpecialEffectScale(jass_t *j) {
    edict_t *effect = jass_checkhandle(j, 1, "effect");
    if (effect) effect->s.scale = jass_checknumber(j, 2);
    return 0;
}

uint32_t BlzSetSpecialEffectYaw(jass_t *j) {
    edict_t *effect = jass_checkhandle(j, 1, "effect");
    if (effect) effect->s.angle = jass_checknumber(j, 2);
    return 0;
}

uint32_t BlzSetSpecialEffectColor(jass_t *j) {
    edict_t *effect = jass_checkhandle(j, 1, "effect");
    if (effect) {
        uint8_t alpha = effect->vertex_color.a ? effect->vertex_color.a : 255;
        effect->vertex_color = MAKE(color32_t, BZ_CLAMP_U8(jass_checkinteger(j, 2)),
            BZ_CLAMP_U8(jass_checkinteger(j, 3)), BZ_CLAMP_U8(jass_checkinteger(j, 4)), alpha);
        G_SetUnitFlag(effect, UNIT_FLAG_VERTEX_COLOR_SET, true);
        G_SetUnitFlag(effect, UNIT_FLAG_VERTEX_COLOR_OVERRIDE_SET, true);
    }
    return 0;
}

/* Declarations for natives newer than the 1.29 common.j. Loaded after common.j
 * so Reforged maps (JASS or Lua) bind them like any other native. */
static cstring_t const jass_reforged_declarations =
    "type framehandle extends handle\n"
    "type originframetype extends handle\n"
    "type framepointtype extends handle\n"
    "type textaligntype extends handle\n"
    "type frameeventtype extends handle\n"
    "type oskeytype extends handle\n"
    "type unitweaponrealfield extends handle\n"
    "type unitweaponstringfield extends handle\n"
    "type unitweaponintegerfield extends handle\n"
    "constant native ConvertUnitWeaponRealField takes integer i returns unitweaponrealfield\n"
    "constant native ConvertUnitWeaponStringField takes integer i returns unitweaponstringfield\n"
    "constant native ConvertUnitWeaponIntegerField takes integer i returns unitweaponintegerfield\n"
    "constant native ConvertOsKeyType takes integer i returns oskeytype\n"
    "constant native ConvertOriginFrameType takes integer i returns originframetype\n"
    "constant native ConvertFramePointType takes integer i returns framepointtype\n"
    "constant native ConvertTextAlignType takes integer i returns textaligntype\n"
    "constant native ConvertFrameEventType takes integer i returns frameeventtype\n"
    "globals\n"
    "    constant originframetype ORIGIN_FRAME_GAME_UI = ConvertOriginFrameType(0)\n"
    "    constant originframetype ORIGIN_FRAME_COMMAND_BUTTON = ConvertOriginFrameType(1)\n"
    "    constant originframetype ORIGIN_FRAME_HERO_BAR = ConvertOriginFrameType(2)\n"
    "    constant originframetype ORIGIN_FRAME_HERO_BUTTON = ConvertOriginFrameType(3)\n"
    "    constant originframetype ORIGIN_FRAME_HERO_HP_BAR = ConvertOriginFrameType(4)\n"
    "    constant originframetype ORIGIN_FRAME_HERO_MANA_BAR = ConvertOriginFrameType(5)\n"
    "    constant originframetype ORIGIN_FRAME_HERO_BUTTON_INDICATOR = ConvertOriginFrameType(6)\n"
    "    constant originframetype ORIGIN_FRAME_ITEM_BUTTON = ConvertOriginFrameType(7)\n"
    "    constant originframetype ORIGIN_FRAME_COMMAND_BUTTON_COOLDOWN_INDICATOR = ConvertOriginFrameType(8)\n"
    "    constant originframetype ORIGIN_FRAME_SYSTEM_BUTTON = ConvertOriginFrameType(9)\n"
    "    constant originframetype ORIGIN_FRAME_PORTRAIT = ConvertOriginFrameType(10)\n"
    "    constant originframetype ORIGIN_FRAME_MINIMAP = ConvertOriginFrameType(11)\n"
    "    constant originframetype ORIGIN_FRAME_MINIMAP_BUTTON = ConvertOriginFrameType(12)\n"
    "    constant originframetype ORIGIN_FRAME_TOOLTIP = ConvertOriginFrameType(13)\n"
    "    constant originframetype ORIGIN_FRAME_UBERTOOLTIP = ConvertOriginFrameType(14)\n"
    "    constant originframetype ORIGIN_FRAME_CHAT_MSG = ConvertOriginFrameType(15)\n"
    "    constant originframetype ORIGIN_FRAME_UNIT_MSG = ConvertOriginFrameType(16)\n"
    "    constant originframetype ORIGIN_FRAME_TOP_MSG = ConvertOriginFrameType(17)\n"
    "    constant originframetype ORIGIN_FRAME_PORTRAIT_HP_TEXT = ConvertOriginFrameType(18)\n"
    "    constant originframetype ORIGIN_FRAME_PORTRAIT_MANA_TEXT = ConvertOriginFrameType(19)\n"
    "    constant originframetype ORIGIN_FRAME_UNIT_PANEL_BUFF_BAR = ConvertOriginFrameType(20)\n"
    "    constant originframetype ORIGIN_FRAME_UNIT_PANEL_BUFF_BAR_LABEL = ConvertOriginFrameType(21)\n"
    "    constant originframetype ORIGIN_FRAME_WORLD_FRAME = ConvertOriginFrameType(22)\n"
    "    constant framepointtype FRAMEPOINT_TOPLEFT = ConvertFramePointType(0)\n"
    "    constant framepointtype FRAMEPOINT_TOP = ConvertFramePointType(1)\n"
    "    constant framepointtype FRAMEPOINT_TOPRIGHT = ConvertFramePointType(2)\n"
    "    constant framepointtype FRAMEPOINT_LEFT = ConvertFramePointType(3)\n"
    "    constant framepointtype FRAMEPOINT_CENTER = ConvertFramePointType(4)\n"
    "    constant framepointtype FRAMEPOINT_RIGHT = ConvertFramePointType(5)\n"
    "    constant framepointtype FRAMEPOINT_BOTTOMLEFT = ConvertFramePointType(6)\n"
    "    constant framepointtype FRAMEPOINT_BOTTOM = ConvertFramePointType(7)\n"
    "    constant framepointtype FRAMEPOINT_BOTTOMRIGHT = ConvertFramePointType(8)\n"
    "    constant textaligntype TEXT_JUSTIFY_TOP = ConvertTextAlignType(0)\n"
    "    constant textaligntype TEXT_JUSTIFY_MIDDLE = ConvertTextAlignType(1)\n"
    "    constant textaligntype TEXT_JUSTIFY_BOTTOM = ConvertTextAlignType(2)\n"
    "    constant textaligntype TEXT_JUSTIFY_LEFT = ConvertTextAlignType(3)\n"
    "    constant textaligntype TEXT_JUSTIFY_CENTER = ConvertTextAlignType(4)\n"
    "    constant textaligntype TEXT_JUSTIFY_RIGHT = ConvertTextAlignType(5)\n"
    "    constant frameeventtype FRAMEEVENT_CONTROL_CLICK = ConvertFrameEventType(1)\n"
    "    constant frameeventtype FRAMEEVENT_MOUSE_ENTER = ConvertFrameEventType(2)\n"
    "    constant frameeventtype FRAMEEVENT_MOUSE_LEAVE = ConvertFrameEventType(3)\n"
    "    constant frameeventtype FRAMEEVENT_MOUSE_UP = ConvertFrameEventType(4)\n"
    "    constant frameeventtype FRAMEEVENT_MOUSE_DOWN = ConvertFrameEventType(5)\n"
    "    constant frameeventtype FRAMEEVENT_MOUSE_WHEEL = ConvertFrameEventType(6)\n"
    "    constant playerunitevent EVENT_PLAYER_UNIT_DAMAGED = ConvertPlayerUnitEvent(308)\n"
    "    constant unitweaponrealfield UNIT_WEAPON_RF_ATTACK_RANGE = ConvertUnitWeaponRealField('ua1r')\n"
    "    constant unitweaponrealfield UNIT_WEAPON_RF_ATTACK_PROJECTILE_SPEED = ConvertUnitWeaponRealField('ua1z')\n"
    "    constant unitweaponstringfield UNIT_WEAPON_SF_ATTACK_PROJECTILE_ART = ConvertUnitWeaponStringField('ua1m')\n"
    "    constant unitweaponintegerfield UNIT_WEAPON_IF_ATTACK_WEAPON_SOUND = ConvertUnitWeaponIntegerField('ucs1')\n"
    "    constant oskeytype OSKEY_BACKSPACE = ConvertOsKeyType(8)\n"
    "    constant oskeytype OSKEY_TAB = ConvertOsKeyType(9)\n"
    "    constant oskeytype OSKEY_RETURN = ConvertOsKeyType(13)\n"
    "    constant oskeytype OSKEY_ESCAPE = ConvertOsKeyType(27)\n"
    "    constant oskeytype OSKEY_SPACE = ConvertOsKeyType(32)\n"
    "    constant oskeytype OSKEY_LEFT = ConvertOsKeyType(37)\n"
    "    constant oskeytype OSKEY_UP = ConvertOsKeyType(38)\n"
    "    constant oskeytype OSKEY_RIGHT = ConvertOsKeyType(39)\n"
    "    constant oskeytype OSKEY_DOWN = ConvertOsKeyType(40)\n"
    "    constant oskeytype OSKEY_0 = ConvertOsKeyType(48)\n"
    "    constant oskeytype OSKEY_1 = ConvertOsKeyType(49)\n"
    "    constant oskeytype OSKEY_2 = ConvertOsKeyType(50)\n"
    "    constant oskeytype OSKEY_3 = ConvertOsKeyType(51)\n"
    "    constant oskeytype OSKEY_4 = ConvertOsKeyType(52)\n"
    "    constant oskeytype OSKEY_5 = ConvertOsKeyType(53)\n"
    "    constant oskeytype OSKEY_6 = ConvertOsKeyType(54)\n"
    "    constant oskeytype OSKEY_7 = ConvertOsKeyType(55)\n"
    "    constant oskeytype OSKEY_8 = ConvertOsKeyType(56)\n"
    "    constant oskeytype OSKEY_9 = ConvertOsKeyType(57)\n"
    "    constant oskeytype OSKEY_A = ConvertOsKeyType(65)\n"
    "    constant oskeytype OSKEY_B = ConvertOsKeyType(66)\n"
    "    constant oskeytype OSKEY_C = ConvertOsKeyType(67)\n"
    "    constant oskeytype OSKEY_D = ConvertOsKeyType(68)\n"
    "    constant oskeytype OSKEY_E = ConvertOsKeyType(69)\n"
    "    constant oskeytype OSKEY_F = ConvertOsKeyType(70)\n"
    "    constant oskeytype OSKEY_G = ConvertOsKeyType(71)\n"
    "    constant oskeytype OSKEY_H = ConvertOsKeyType(72)\n"
    "    constant oskeytype OSKEY_I = ConvertOsKeyType(73)\n"
    "    constant oskeytype OSKEY_J = ConvertOsKeyType(74)\n"
    "    constant oskeytype OSKEY_K = ConvertOsKeyType(75)\n"
    "    constant oskeytype OSKEY_L = ConvertOsKeyType(76)\n"
    "    constant oskeytype OSKEY_M = ConvertOsKeyType(77)\n"
    "    constant oskeytype OSKEY_N = ConvertOsKeyType(78)\n"
    "    constant oskeytype OSKEY_O = ConvertOsKeyType(79)\n"
    "    constant oskeytype OSKEY_P = ConvertOsKeyType(80)\n"
    "    constant oskeytype OSKEY_Q = ConvertOsKeyType(81)\n"
    "    constant oskeytype OSKEY_R = ConvertOsKeyType(82)\n"
    "    constant oskeytype OSKEY_S = ConvertOsKeyType(83)\n"
    "    constant oskeytype OSKEY_T = ConvertOsKeyType(84)\n"
    "    constant oskeytype OSKEY_U = ConvertOsKeyType(85)\n"
    "    constant oskeytype OSKEY_V = ConvertOsKeyType(86)\n"
    "    constant oskeytype OSKEY_W = ConvertOsKeyType(87)\n"
    "    constant oskeytype OSKEY_X = ConvertOsKeyType(88)\n"
    "    constant oskeytype OSKEY_Y = ConvertOsKeyType(89)\n"
    "    constant oskeytype OSKEY_Z = ConvertOsKeyType(90)\n"
    "    constant oskeytype OSKEY_F1 = ConvertOsKeyType(112)\n"
    "    constant oskeytype OSKEY_F2 = ConvertOsKeyType(113)\n"
    "    constant oskeytype OSKEY_F3 = ConvertOsKeyType(114)\n"
    "    constant oskeytype OSKEY_F4 = ConvertOsKeyType(115)\n"
    "endglobals\n"
    "native BlzGetOriginFrame takes originframetype frameType, integer index returns framehandle\n"
    "native BlzHideOriginFrames takes boolean enable returns nothing\n"
    "native BlzGetFrameByName takes string name, integer createContext returns framehandle\n"
    "native BlzCreateFrameByType takes string typeName, string name, framehandle owner, string inherits, integer createContext returns framehandle\n"
    "native BlzCreateSimpleFrame takes string name, framehandle owner, integer createContext returns framehandle\n"
    "native BlzDestroyFrame takes framehandle frame returns nothing\n"
    "native BlzFrameClearAllPoints takes framehandle frame returns nothing\n"
    "native BlzFrameSetAbsPoint takes framehandle frame, framepointtype point, real x, real y returns nothing\n"
    "native BlzFrameSetPoint takes framehandle frame, framepointtype point, framehandle relative, framepointtype relativePoint, real x, real y returns nothing\n"
    "native BlzFrameSetAllPoints takes framehandle frame, framehandle relative returns nothing\n"
    "native BlzFrameSetSize takes framehandle frame, real width, real height returns nothing\n"
    "native BlzFrameSetVisible takes framehandle frame, boolean visible returns nothing\n"
    "native BlzFrameIsVisible takes framehandle frame returns boolean\n"
    "native BlzFrameSetEnable takes framehandle frame, boolean enabled returns nothing\n"
    "native BlzFrameGetEnable takes framehandle frame returns boolean\n"
    "native BlzFrameSetText takes framehandle frame, string text returns nothing\n"
    "native BlzFrameGetText takes framehandle frame returns string\n"
    "native BlzFrameSetTextAlignment takes framehandle frame, textaligntype vert, textaligntype horz returns nothing\n"
    "native BlzFrameSetFont takes framehandle frame, string fileName, real height, integer flags returns nothing\n"
    "native BlzFrameSetTexture takes framehandle frame, string texFile, integer flag, boolean blend returns nothing\n"
    "native BlzFrameSetVertexColor takes framehandle frame, integer color returns nothing\n"
    "native BlzFrameSetModel takes framehandle frame, string modelFile, integer cameraIndex returns nothing\n"
    "native EngineFrameSetTeamColor takes framehandle frame, integer color returns nothing\n"
    "native BlzFrameSetAlpha takes framehandle frame, integer alpha returns nothing\n"
    "native BlzFrameGetAlpha takes framehandle frame returns integer\n"
    "native BlzFrameSetLevel takes framehandle frame, integer level returns nothing\n"
    "native BlzFrameSetScale takes framehandle frame, real scale returns nothing\n"
    "native BlzConvertColor takes integer a, integer r, integer g, integer b returns integer\n"
    "native BlzTriggerRegisterFrameEvent takes trigger whichTrigger, framehandle frame, frameeventtype eventId returns event\n"
    "native BlzGetTriggerFrame takes nothing returns framehandle\n"
    "native BlzGetTriggerFrameEvent takes nothing returns frameeventtype\n"
    "native BlzTriggerRegisterPlayerKeyEvent takes trigger whichTrigger, player whichPlayer, oskeytype key, integer metaKey, boolean keyDown returns event\n"
    "native BlzGetTriggerPlayerKey takes nothing returns oskeytype\n"
    "native BlzGetTriggerPlayerIsKeyDown takes nothing returns boolean\n"
    "native BlzGetTriggerPlayerMetaKey takes nothing returns integer\n"
    "native BlzPauseUnitEx takes unit whichUnit, boolean flag returns nothing\n"
    "native BlzSetUnitFacingEx takes unit whichUnit, real facingAngle returns nothing\n"
    "native BlzGetUnitZ takes unit whichUnit returns real\n"
    "native BlzGetUnitWeaponRealField takes unit whichUnit, unitweaponrealfield whichField, integer index returns real\n"
    "native BlzGetUnitWeaponStringField takes unit whichUnit, unitweaponstringfield whichField, integer index returns string\n"
    "native BlzGetUnitWeaponIntegerField takes unit whichUnit, unitweaponintegerfield whichField, integer index returns integer\n"
    "native BlzGetEventDamageTarget takes nothing returns unit\n";

void G_LoadReforgedDeclarations(jass_t *j) {
    size_t len = strlen(jass_reforged_declarations);
    string_t buffer = gi.MemAlloc((long)len + 1);
    memcpy(buffer, jass_reforged_declarations, len + 1);
    jass_dobuffer(j, buffer);
    gi.MemFree(buffer);
}
