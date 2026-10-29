#include "s_skills.h"

/* Night Elf passive attack abilities: Moon Glaive (Amgl/Amgr) and Slow Poison (Aspo). */

/* Detonate (Adtn) has none of Kaboom's physical area-damage or death-triggered
 * behavior.  The point order controls approach; the caster's position when it
 * completes is the epicentre. DataA is mana loss, DataB hostile summon damage. */
BZ_ABILITY_PROC(CAbilityDetonate) {
    uint32_t const code = call && call->item ? call->item->code : 0;
    uint32_t level;
    float radius, mana_loss, summon_damage;
    vec2_t origin;
    if (msg != A_EXECUTE) {
        if (msg == A_VALIDATE)
            return ent && call && call->target && call->target->type == SPELL_TARGET_POINT &&
                S_SpellIsAliveTarget(ent);
        return CAbilitySimpleSpell(ent, msg, call);
    }
    if (!ent || !code || !S_SpellIsAliveTarget(ent) || !call || !call->target ||
        call->target->type != SPELL_TARGET_POINT) return false;

    level = S_SpellLevel(ent, code);
    radius = S_SpellNumber(code, ABILITY_NUMBER_AREA, level);
    mana_loss = MAX(0.0f, S_SpellData(code, level, 1));
    summon_damage = MAX(0.0f, S_SpellData(code, level, 2));
    origin = ent->s.origin2;
    if (radius >= 0.0f) {
        /* A completed cast works even if the target area is empty. The normal
         * point-approach thinker still cancels an interrupted Move order. */
        FILTER_EDICTS(target, target != ent && S_SpellIsAliveTarget(target) &&
                      Vector2_distance(&target->s.origin2, &origin) <= radius &&
                      S_SpellAllowsAreaTarget(code, ent, target)) {
            bool affected = S_DispelUnitMagic(target) != 0;
            /* Invulnerable units still lose magical buffs, but not mana or HP. */
            if (!G_UnitHasFlag(target, UNIT_FLAG_INVULNERABLE)) {
                if (mana_loss > 0.0f && target->mana.value > 0.0f) {
                    target->mana.value = MAX(0.0f, target->mana.value - mana_loss);
                    G_InvalidateUnitInfoPanel(target);
                    affected = true;
                }
                if (summon_damage > 0.0f && S_SpellIsEnemy(ent, target) &&
                    S_DispelIsSummoned(target) && !S_SummonIsDispelImmune(target))
                    affected |= S_SpellDamage(target, ent, (int)summon_damage);
            }
            if (affected && target->inuse)
                G_SpawnAbilityEffectTarget(code, WC3_EFFECT_TARGET, 0, target, NULL, true);
        }
        /* Loose terrain Blight (including Sacrificial Skull paint) can be
         * dispelled; do not erase currently maintained building-growth areas. */
        G_DispelBlightRadius(&origin, radius);
    }
    /* Die through the ordinary worker lifecycle (food, harvest, queues, death
     * events). Script explosion uses the unit's authored SpecialArt, not the
     * Sapper's damage effect. Death alone must never trigger Adtn. */
    ent->aiflags |= AI_SCRIPT_EXPLODED;
    G_SetHealth(ent, 0);
    if (ent->die) ent->die(ent, ent);
    else unit_die(ent, ent);
    return true;
}


#define ID_MOON_GLAIVE  MAKEFOURCC('A','m','g','l')
#define ID_MOON_GLAIVER MAKEFOURCC('A','m','g','r')
#define ID_SLOW_POISON  MAKEFOURCC('A','s','p','o')
#define ID_BARKSKIN     MAKEFOURCC('A','b','a','r')
#define BUFF_SLOW_POI   MAKEFOURCC('B','s','p','o')
#define BUFF_BARKSKIN   MAKEFOURCC('B','b','a','r')

cstring_t const barkskin_orders[] = { "barkskinon", "barkskinoff", NULL };

/* ---- Moon Glaive (Amgl / Amgr): passive attack bounce -------------------- */

BZ_ABILITY_PROC(CAbilityMoonGlaive) { return CAbilityPassive(ent, msg, call); }

/* Called from S_ResolveAttackHit after the primary hit lands. Stock Amgl DataA
 * and Area are 0; that means one extra bounce inside attack range, not "no bounce".
 * Authored DataA>0 is total targets. Damage is the already-mitigated primary hit. */
void S_MoonGlaiveAttack(edict_t *attacker, edict_t *primary, int damage) {
    uint32_t level = G_UnitAbilityLevel(attacker, ID_MOON_GLAIVE);
    float data_a, range;
    uint32_t total;
    edict_t *visited[8], *current;
    uint32_t nvisited = 0;
    if (!level) level = G_UnitAbilityLevel(attacker, ID_MOON_GLAIVER);
    if (!level || !primary) return;
    data_a = S_SpellData(ID_MOON_GLAIVE, level, 1);
    total = data_a > 0.0f ? (uint32_t)data_a : 2; /* stock 0 → primary + 1 bounce */
    range = S_SpellNumber(ID_MOON_GLAIVE, ABILITY_NUMBER_AREA, level);
    if (range <= 0.0f) range = attacker->unit_state->attack1.range;
    if (nvisited < 8) visited[nvisited++] = primary;
    current = primary;
    for (uint32_t i = 1; i < total; i++) {
        edict_t *next = NULL;
        FILTER_EDICTS(other, other != attacker && S_SpellIsAliveTarget(other) &&
                      S_AttackHitAllowed(attacker, primary, other) &&
                      S_SpellIsEnemy(attacker, other) &&
                      Vector2_distance(&other->s.origin2, &current->s.origin2) <= range) {
            bool seen = false;
            FOR_LOOP(j, nvisited) seen |= other == visited[j];
            if (!seen) { next = other; break; }
        }
        if (!next) break;
        if (nvisited < 8) visited[nvisited++] = next;
        T_Damage(next, attacker, damage);
        current = next;
    }
}

/* ---- Slow Poison (Aspo): passive poison on hit --------------------------- */

BZ_ABILITY_PROC(CAbilitySlowPoison) { return CAbilityPassive(ent, msg, call); }

/* Called from S_ResolveAttackHit after a hit lands on an enemy.  Applies the
 * Bspo buff which the movement and attack-speed hooks read each frame. */
void S_SlowPoisonOnHit(edict_t *attacker, edict_t *target) {
    uint32_t level = G_UnitAbilityLevel(attacker, ID_SLOW_POISON);
    heroabilitystatus_t *status;
    if (!level || !target || !S_SpellAllowsTarget(ID_SLOW_POISON, attacker, target)) return;
    /* DataA DPS / BuffID Bssd are leftover; this slice only applies Bspo slow. */
    status = S_SpellApplyTimedStatusFromAbility(target, ID_SLOW_POISON, level, "Bspo",
                                               S_SpellResistantDuration(ID_SLOW_POISON, level, target));
    if (status) unit_status_enabletargetart(target, status, "origin");
}

/* DataB/DataC are fractions (stock 0.5 / 0.25), same %>% convention as Bloodlust. */
float S_SlowPoisonMoveReduction(edict_t const *unit) {
    uint32_t level = G_UnitStatusLevel(unit, BUFF_SLOW_POI);
    if (!level) return 0.0f;
    return S_SpellData(ID_SLOW_POISON, level, 2);
}

float S_SlowPoisonAttackReduction(edict_t const *unit) {
    uint32_t level = G_UnitStatusLevel(unit, BUFF_SLOW_POI);
    if (!level) return 0.0f;
    return S_SpellData(ID_SLOW_POISON, level, 3);
}

/* ---- Barkskin (Abar): modal autocast of a timed friendly armor buff ------- */

static bool barkskin_validate(edict_t *caster, spellTarget_t st, abilityitem_t const *spell) {
    uint32_t level;
    cstring_t buff;
    wc3_status_apply_check_t result;
    if (!spell || !st.entity || !S_SpellIsAliveTarget(st.entity) || !S_SpellIsFriend(caster, st.entity) ||
        !S_SpellAllowsTarget(spell->code, caster, st.entity)) return false;
    level = S_SpellLevel(caster, spell->code);
    buff = S_SpellBuffId(spell->code, level);
    result = unit_status_checkapplication(st.entity, &(status_application_t){ .buff = buff, .level = level });
    return result == WC3_STATUS_APPLY_FREE_SLOT || result == WC3_STATUS_APPLY_REUSE;
}

static void barkskin_execute(edict_t *caster, spellTarget_t st, abilityitem_t const *spell) {
    uint32_t level = S_SpellLevel(caster, spell->code);
    cstring_t buff = S_SpellBuffId(spell->code, level);
    if (!st.entity || !buff || strlen(buff) < 4) return;
    S_SpellApplyTimedTargetStatus(st.entity, spell->code, level, buff,
                                  S_SpellResistantDuration(spell->code, level, st.entity));
}

static bool barkskin_acquire(edict_t *caster, uint32_t code) {
    edict_t *best = NULL;
    float range = S_SpellRange(code, S_SpellLevel(caster, code)), dist = FLT_MAX;
    FILTER_EDICTS(target, S_SpellIsAliveTarget(target) && S_SpellIsFriend(caster, target)) {
        float cur;
        if (G_UnitStatusLevel(target, BUFF_BARKSKIN) || !S_SpellAllowsTarget(code, caster, target)) continue;
        cur = Vector2_distance(&target->s.origin2, &caster->s.origin2);
        if ((range <= 0.0f || cur <= range) && cur < dist) { best = target; dist = cur; }
    }
    return best && S_CastUnitTargetSpell(caster, code, best);
}

BZ_ABILITY_PROC(CAbilityBarkskin) {
    spellTarget_t target = (msg == A_VALIDATE || msg == A_EXECUTE) && call && call->target ?
        *call->target : MAKE(spellTarget_t, .type = SPELL_TARGET_NONE);
    uint32_t code = call && call->item && call->item->code ? call->item->code : ID_BARKSKIN;
    switch (msg) {
    case A_VALIDATE: return barkskin_validate(ent, target, call ? call->item : NULL);
    case A_EXECUTE: barkskin_execute(ent, target, call ? call->item : NULL); return true;
    case A_AUTOCAST_ACQUIRE: return barkskin_acquire(ent, code);
    case A_ORDER:
        if (call && call->order && !strcmp(call->order, "barkskinon")) return G_SetUnitAutocast(ent, code, true);
        if (call && call->order && !strcmp(call->order, "barkskinoff")) return G_SetUnitAutocast(ent, code, false);
        return false;
    default: return CAbilityModalSpell(ent, msg, call);
    }
}

/* Bbar reads Abar DataA so expiry removes the armor without mutating base unit state. */
float S_BarkskinArmorBonus(edict_t const *unit) {
    uint32_t level = G_UnitStatusLevel(unit, BUFF_BARKSKIN);
    return level ? S_SpellData(ID_BARKSKIN, level, 1) : 0.0f;
}
