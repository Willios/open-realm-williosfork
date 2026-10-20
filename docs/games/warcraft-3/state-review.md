# Warcraft III game-state review

## Attack cooldown

`skills/s_attack.c` owns `attack_cooldown_end_time`, a simulation-millisecond deadline.
The former `attack_cooldown_active` and seconds-valued `attack_cooldown_remaining`
were derived mirrors. History (`487e1abfd`) already made the deadline authoritative
so replacement Move orders could not freeze weapon cooldown. Consumers now calculate
remaining seconds only when installing recovery waits. `wait` still serves the
move-local damage-point/recovery clock; `attack_backswing_end_time` is a separate
gate after a committed hit, not a duplicate weapon deadline.

Save version 101 removes the mirrors. Older saves are rejected, with no migration.
`wc3_combat.canceled_windup_cooldown_advances_after_attack_order_is_replaced` checks a
replaced windup, reissue one millisecond before expiry, and the next swing at expiry.
Existing combat tests cover chase, animation completion, backswing and save/load;
`wc3_save.rejects_prior_save_versions` covers version rejection.

## Retained state and boundaries

| Question | Finding and decision |
|---|---|
| Is `client_s.jass` test-only? | No. `g_spawn.c` initializes it; JASS player natives mutate it, `m_unit.c` reads XP handicap, `hud_allies.c` reads names, and `g_save.c` restores player state. It belongs to the game-owned player record. |
| Can camera fields collapse into one interpolation? | Scalar setup interpolation, timed pan, continuous pan rate, target/orient controllers and camera noise have independent contracts. `g_main.c` and JASS camera natives consume them. `pan_start` is the captured origin and `pan_destination` the endpoint. Wrappers alone would add indirection without reducing state. |
| Can movement storage be absent whenever a unit stands? | No. Guard anchors, retained Follow/Patrol, queued orders and critter idle schedules outlive active translation. Optional allocation is possible by behavior ownership, following the existing pooled-state architecture, but needs allocation/release/save coverage for every consumer. Standing is not a valid allocation predicate. No pool conversion is included here. |
| Can animation names become timers? | A timer answers when, not which model sequence to select. `animation_request` retains a logical name for property-driven reselection; `queued_animation` retains the next script-requested clip. Names must survive JASS caller storage and model resolution. Interning would require explicit lifetime and save identity; it is a separate representation change. |
| Why SC2API under Warcraft III? | It is the optional Warcraft game adapter for agents speaking the SC2API Raw protocol. Its race/unit/order translation depends on WC3 state, not the StarCraft II simulation. Moving it into that game would invert ownership. |
| Are all JASS events implemented? | No. The event enum preserves JASS ABI numbers, including events with no current producer. Lack of a producer does not make an ABI constant removable. |
| Are ability messages duplicates? | Query/notification, issuer/target, item add/remove and status refresh/remove have different phase and return-value contracts. Their per-message comments describe these differences; merging by similar payload shape would erase semantics. |

## Ability procedure payload proposal

Keep the single `abilityProc_t` dispatcher. Its `abilityCall_t` payload already uses
an anonymous union: `client`, `projectile`, `attacker` and `move_target` overlap in
storage. Giving that slot a generic name would shorten the declaration but remove
useful role information without saving memory. The target-order pair also carries
an order string. WC3 order names such as `smart` and `attack` are not four-byte
rawcodes. `classname` is the registry class name passed by `InitAbilities`;
`item->code` carries the concrete ability rawcode.

A Win32-style pair of untyped parameters would require casts for function pointers,
entity pointers, strings, floats and composite payloads. The recommended direction
is to keep named typed union members and document the message-to-member contract.
A future naming change can use one entity slot plus one typed order payload, but
should be agreed separately and migrated across producers and consumers together.
This review does not change the ability-call ABI.

## Documentation discipline

`AGENTS.md` now requires purpose, ownership and consumer comments for enums and
structs. This change adds focused comments at the reviewed declarations rather than
mechanically labelling every existing type without investigating its contract.

See also [pooled state](pooled-entity-state.md), [ability implementation](ability-implementation.md),
[animation properties](unit-animation-properties.md), [save/load](save-load.md), and
[SC2API adapter](sc2api-agent-interface.md).

## Validation environment

The local Homebrew `sdl2-compat` 2.32.72 crashed in `SDL_PushEvent_REAL` when
`client_input.menu_sdl_input_is_exclusive_with_world_presentation` pushed its
`SDL_TEXTINPUT` fixture. The unchanged test passed with upstream SDL2 2.32.10
built separately under `/tmp`; no client/test workaround is part of this change.
The runtime override used for validation is:

```sh
SDL_VIDEODRIVER=dummy SDL_DYNAMIC_API=/path/to/upstream/libSDL2-2.0.0.dylib make test
```

`SDL_DYNAMIC_API` selects the temporary library without modifying the installed
Homebrew package or repository dependency configuration. Network tests also need
local UDP socket access outside the filesystem/network sandbox.
