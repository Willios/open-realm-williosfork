# Warcraft III entity and player state

## Optional travel and retained intent

`edict_t.movement` is a `movement_t *`. Its presence means a travel operation
owns routing scratch. `G_UnitIsMoving()` exposes that same check. A paused unit,
a unit turning before its next step, or a unit waiting for a route can still have
an active travel operation; physical displacement is measured from position samples.
Queries never allocate a record or return a shared writable zero object.

| Record | Owner and lifetime |
|---|---|
| `movement_t *movement` | Active travel: progress watermark, blocked decisions, heading, formation speed, displacement, resumable route and worker avoidance. `G_BeginMovement` attaches it; stationary `unit_setmove`, Stop, arrival and death release it. Steering entry points can begin travel for interaction approaches. |
| `unitOrders_t *orders` | A live unit's retained command intent: Follow/Patrol/Attack-Move waypoints, guard anchors, cargo/drop interaction identity, resource-return target, Wisp egress, Shift-order FIFO and pending item drop/give context. Unit spawn initializes it; it survives stationary phases and combat detours. Entity teardown releases it. Effects/destinations do not need it merely to participate in routing. |
| `routeCache_t *route` | Destination-owned flow cache, shared group destination and waypoint incarnation. Allocated when a destination participates in routing. It does not indicate that the destination is moving. Reusing a waypoint increments its serial and clears its field/shared destination. |
| `wander_t *wander` | Awan-owned idle deadline, random state and private waypoint identity. Initialized through ability lifecycle/idle dispatch; disable/death/removal releases the waypoint and record. It persists during the idle interval between trips. |

`umove_t.travel` explicitly declares a travel-capable behavior; animation names do
not determine ownership. All walk/approach behaviors declare it, including item
pickup/drop/give. A Follow order inside its stopping distance releases movement
scratch while retaining its target, then begins travel again when the target moves.
`unit_facegoal` turns stationary attacks and repair work without allocating a route.
Immobile steering attempts do not create travel state.

The audit also covered fields outside the old embedded movement struct:

- `wander_*` moved into `wander_t`.
- `heatmap2*` and `waypoint_generation` moved into `routeCache_t`.
- The per-edict `order_queue` moved into `orders->queue`; pending `item_drop`
  moved into `orders`, and spell-approach inventory origin moved into its already
  owned `channel_t` record with explicit entity fixups.
- The overloaded `secondarygoal` split into `orders->resource_goal` for worker
  resource-return identity and `route->shared_goal` for group destination sharing.
- `old_origin` remains an event sample: region/proximity events must detect
  teleports as well as ordinary steps.
- `goalentity` remains the current behavior target shared by attacks, spells,
  missiles and travel. `movetype` and `unitinfo.MoveSpeed` describe unit physics
  and configuration even while standing. `velocity` is used by missiles and spell
  thinkers, not as the unit travel-state flag.

On the local arm64 build, the combined refactor reduces `sizeof(edict_t)` from 5,800 to 4,232 bytes (1,568 bytes smaller), including 64 bytes removed by the Soul Trap extraction.
This is an edict-layout measurement, not a claim about total resident RAM: the
existing typed-pool implementation also reserves backing storage for optional roles.

## Review checklist

| Original review area | Completed result |
|---|---|
| Struct/enum purpose and consumers | Documented declarations in `g_local.h`; named player/HUD/entity-data records replace anonymous bundles. |
| Player JASS state | Named `playerScript_t`; retained because spawn, natives, HUD, XP and saves use it. |
| Camera redundancy | Named camera record and grouped pan; independent scalar, pan, controller and noise contracts remain explicit. |
| Optional movement, including fields outside Movement | Active scratch, retained orders/FIFO/waypoints, destination cache and Awan schedule have separate owned records. |
| Animation names | Immutable interned content replaces fixed per-edict arrays; saves store text and restore shared identity. |
| Cooldown duplication | One authoritative deadline; recovery derives remaining time. |
| Ability payload duplication | One related-entity slot plus typed message payload; registry class derives from the bound ability. |
| Event/message enums | Audited production phases; JASS numeric ABI slots remain stable. Unsupported events are not implemented by deleting constants. |
| SC2API placement | Remains a WC3-owned protocol adapter; no SC2 simulation dependency introduced. |

The remaining inline fields that movement also touches are general entity contracts,
not hidden travel scratch: the fixed server prefix (`s`, collision, bounds, area
links) describes every entity; `movetype`/`unitinfo` retain physics and configuration;
`goalentity`, `combatentity`, `currentmove`, callbacks and `wait` dispatch attacks,
spells, work and missiles as well as travel. `paused`, `stunned`, `no_pathing` and
`aiflags` are persistent simulation rules. `velocity` belongs to missiles/thinkers;
`old_origin` samples region/proximity events even for stationary teleports. None
should be used instead of `G_UnitIsMoving()` to infer active unit travel.

A second full-diff audit found and fixed rejected point/selection Move paths that
assumed `order_move` always attached scratch. Translation-disabled units now reject
before destination/formation work. It also restored authored Rooted Turning through
stationary facing and steering entry points without allocating travel. Regressions
failed first (null dereferences and a missing turn), then passed in classic/TFT.

## Attack timing

Weapon cooldown retains only `attack_cooldown_end_time` in simulation milliseconds.
Active state and remaining seconds are derived when queried, including after a
replacement order. The former active flag and remaining-time mirror are gone.
`attack_backswing_end_time` is a different post-hit gate, and `wait` is the
move-local animation/damage/recovery clock; neither duplicates the weapon deadline.
No-op per-frame mirror-refresh calls were removed.

## Player records and camera

`client_s` contains named records for script-owned player state, camera, info-panel
cache, resource-bar cache, shortcuts, active messages and retained message history.
`playerScript_t` is live runtime state: spawn initializes it, JASS natives mutate it,
XP reads handicap, and HUD/save paths consume names and overrides. It is not test state.

Camera pan fields are grouped as `camera.pan.{active,time,start,destination,rate}`.
The captured start and timed endpoint differ from continuous strafe/forward velocity.
Scalar setup interpolation, target/orient controllers and noise have independent
contracts; the game samples their results into player state. The obsolete cached
portrait mana value was removed; the old overloaded HP cache is now explicitly a
selection `count`, while live health/mana use player-state bindings.

## Animation strings

Per-edict fixed request/queue/property arrays were replaced with interned immutable
strings. `G_InternAnimationName` copies script/stack text and shares identical content;
repeated Walk requests do not allocate per unit or frame. The 80-byte request limit
was removed. Normalized property tags still use the parser's bounded temporary
serialization buffer. `G_FreeAnimationNames` releases storage at game shutdown,
independently of model-cache refresh, because logical names can outlive model metadata.

A timer cannot identify which animation family to select. Retaining logical names
is necessary for deferred model resolution, property-driven reselection and queued
JASS destructable clips; interning removes the repeated inline storage.

## Ability procedure payload

The single `abilityProc_t` dispatcher remains. `ent` is its receiver and the common
`call->entity` identifies the related actor for commands, damage, projectile impacts,
Move starts and targeted orders. Immediate and targeted orders share `call->order`;
queue messages use `entity` for the producer and `queue_item` for the queued result.
`source_item` and its spawn incarnation remain separate because an asynchronous
inventory origin can coexist with another related entity.

The redundant init-only `classname` payload was removed: `S_AbilityClass(call)`
reads the registry key from `call->item->ability`. Concrete ability rawcodes remain
in `item->code`. Order names such as `smart`/`attack` and internal command keys such
as `CmdBuild` are not FOURCCs. Typed function/status/target payloads retain their
contracts without converting function pointers or floats through untyped integers.

Every `abilityMsg_t` has production references. Similar payloads do not make
query/notification, issuer/target, add/remove or refresh/remove phases duplicates.
JASS `EVENTTYPE` values remain numerically stable, including currently unsupported
subscriptions. `G_ExecuteEvent` explicitly retains empty cases such as
`EVENT_GAME_VICTORY`; removing enum rows would break the script ABI rather than
implement those events. Struct/enum declarations now document purpose and consumers,
and `AGENTS.md` requires those comments for future declarations.

## Save and network contracts

Save version 103 rejects prior layouts. Movement, orders, route and wander records
use the existing entity-owned typed-pool serialization. Entity references have
explicit fixups; route jobs, resumable steering and destination flow generations
are process-local caches cleared on load. Guard intent and waypoint incarnations
survive. Animation strings follow each raw edict as text and are interned on read;
process addresses are cleared from the raw record.

No entity/player network fields or wire layouts changed. The SC2API Raw endpoint
remains the optional Warcraft-owned adapter: it translates agent protocol values
into WC3 state/orders and does not belong to the StarCraft II game module.

## Verification

- `wc3_unit.movement_storage_is_optional`: optional pointer storage contract.
- `wc3_order_lifecycle.optional_movement_stop_pause_arrival_and_reuse`: real orders,
  scheduler pause/arrival, Stop, death, release and a fresh trip.
- `wc3_order_lifecycle.stationary_follow_retains_intent_without_travel_scratch`:
  waiting Follow releases scratch and resumes when the target moves.
- `wc3_unit.animation_names_are_shared_owned_and_not_truncated`: script/stack lifetime,
  shared identity and requests longer than the former 80-byte limit.
- `wc3_save.animation_strings_restore_after_intern_storage_is_released`: save/load
  after discarding all process-owned animation strings.
- `wc3_save.route_cache_rebuilds_after_load` and existing pool/save tests: destination
  cache reset, entity fixups, absent roles, invalid references and old-save rejection.
- Existing combat, movement, resource-worker, guard, cargo, waypoint and wander suites
  cover the changed production consumers in classic and TFT modes.

The local Homebrew `sdl2-compat` 2.32.72 crashes in an unchanged SDL text-input test.
Validation uses separately built upstream SDL2 2.32.10 without changing dependencies:

```sh
SDL_VIDEODRIVER=dummy SDL_DYNAMIC_API=/path/to/upstream/libSDL2-2.0.0.dylib make test
```

Network tests also require local UDP access outside the test sandbox.
See [pooled state](pooled-entity-state.md), [ability implementation](ability-implementation.md),
[animation properties](unit-animation-properties.md), [save/load](save-load.md),
[SC2API adapter](sc2api-agent-interface.md), and [camera sampling](../../architecture/client.md).
