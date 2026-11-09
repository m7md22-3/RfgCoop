# RfgCoop: backpack effects and animations on the partner puppet (plan)

User report after the ver6.2 test (2026-10-08): "All worked. However, backpacks effects and animations are not synced.
For example, Firepower ...". ver6.2 makes the puppet wear the partner's real pack (`ghost_impl.md` "Partner gear").
This note is the plan for the rest: every pack's effects, sounds, animations and gameplay on the puppet.

Sources (each ends with an adversarial "Verification" section; its corrections are applied here):
- `findings/workflows/bp_framework.md`: class layout, activate / deactivate, effects machinery, MP path, transport;
- `bp_anim.md`: animations;
- `bp_movement.md`: Jetpack, Thrust, Fleetfoot;
- `bp_perception.md`: Stealth, Vision;
- `bp_offense.md`: Commando (Firepower), Shockwave (Concussion), Tremor;
- `bp_heal_rhino.md`: Heal, Rhino;
- gap round (§9): `bp_gap_1.md` (R1 visibility gate), `bp_gap_2.md` (pack blasts vs humans, G8), `bp_gap_3.md` (R2
  sound and asset residency), `bp_gap_4.md` (pack-mesh animations). Their corrections are applied below too.

Everything was read in the GOG rfg.exe disassembly and the game's tables (`misc.vpp_pc`, `effects.vpp_pc`, read only).
Addresses are RVAs (the disassembly cache prints VA = RVA + 0x400000). "Inferred" = not read in code. **Nothing here
has been run in game.**

## 0. Answer in short

- **Why nothing plays.** Every activation visual, sound, animation and ability starts in
  `multi_object_backpack::activate` 0x5F2030 and stops in `deactivate` 0x59AA30. In SP only the local input path
  reaches them (`control_gameplay_process` → `player_toggle_backpack_activation` 0x5F8520 → ramp → `activate`). The
  puppet's pack is processed every frame (`multi_game_backpack_process` 0x5F3ED0, called ungated at 0x3EDE79) but
  never activated, so it shows only the "charged" fuel glow.
- **The Jetpack is the exception.** Flight is not an activation. It rides PSTATE state bit 8 into
  `player_update_jetpack` (0x5CF3FA → 0x5CF115), the same path MP uses, so flames, sounds, lift and spine bend should
  already show. One gap: the puppet burns its **own** simulated tank and can run dry before the partner.
- **The fix is what an MP client does.** An MP client receiving the partner's toggle calls the full `activate()` /
  `deactivate()` on the remote player's pack (`toggle_activation` 0x5F8271 / 0x5F81EA). Everything visible to others
  then plays on that remote player. Do the same on the puppet, driven by 4 new PSTATE bytes (protocol 6), applied in
  `Effects` at the state's replay time.
- **Animations.** Only the **Rhino** has its own character animation: state 0xF1 "mp rhino" (`male_run_rno.animx`)
  plus the "override default animation state" flag, set in `activate` and cleared in `deactivate`. Fleetfoot and Rhino
  change max speed, which drives the run blend. Thrust uses the generic jump and airborne states. The Jetpack's spine
  bend already works. The other six packs have **no animation at all**. The pack-mesh animations in `backpacks.xtbl`
  (`rhino_activate`, `rhino_rd`, `shckwve_activate`, `shckwve_idle`) are never played by the game, in SP or MP, for
  any wearer (**confirmed**, `bp_gap_4.md`). The only pack-mesh animation that plays is `rhino_idle` ("stand"), looped
  on Rhino and Shockwave packs by generic item code, so the puppet's pack already has it. So the animation part
  comes entirely from `activate()`, with no separate animation sync.
- **The catch: in SP every machine is the "server".** `activate()` on the puppet's pack also runs the pack's gameplay
  on the observer's machine: Tremor quakes, the Shockwave blast and 6 m ripple, the Rhino and Thrust ram with its
  holes, the Commando hammer blast, and the Heal. Small gates (§4.3) give each piece its milestone. Only the Shockwave
  ripple can floor a co-op player (G4). Pack blasts and cutters **cannot** knock down or hurt a player in SP, in either
  direction (`bp_gap_2.md`), so G8 is optional and cosmetic.
- **Firepower checked** (§7): right in kind, wrong in the numbers. It lasts 6.7 s (not 8). Damage is ×1.25 (not half
  the ammo). The glow is on the pack and the gun, not the body. Tracers turn red only on guns that have tracers. The
  impact gets an extra fiery puff, not bigger sparks. The hammer blast is real and far from small (20000 structural).
  A player hit by Firepower bullets still shows blood in SP (seen in game, ver6.3.1; the cause is not verified, §2.5).
- **Sounds and assets are all there** (`bp_gap_3.md`): every pack sound, local and remote, loads at boot in SP. No new
  asset loading is needed.

## 1. Shared facts used below

| Item | Value |
|---|---|
| Worn pack | `player+0x1170` → `multi_object_backpack` (0x694 bytes) |
| Pack fields | `+0x254` activated; `+0x25C` fuel 0..1; `+0x268` `activated_time` (first dword: end tick; −1 after a deactivate); `+0x270` info; `+0x274` wearer; `+0x2AC` Thrust direction up; `+0x2AD` Thrust landed |
| Info fields | `+0x50` id; `+0x58` passive / `+0x150` active `abilities` (0xF8 each); `+0x248` jetpack; `+0x6EE` activatable; `+0x6EF` deactivatable (absent = one-shot) |
| Ability choice | every consumer reads the active block while `+0x254` = 1, else the passive one, live at the moment of use |
| `activate` 0x5F2030 | thiscall, no args, plain `ret` (0x5F23C0; the WC path tail-jumps) |
| `deactivate` 0x59AA30 | thiscall, no args, `ret` (0x59AC4E). **No "is active" guard**: callers must test `+0x254` |
| Local-only parts (exclude the puppet by themselves) | ramp-up effects and ramp shake (`start_ramp_up` 0x57CEA9); cover exit and zoom off (0x5F203C); activation shake (`object_is_local_player` 0x5F20E6); Vision X-ray (0x57C356) and screen blur/noise (`+0x30C`); Fleetfoot FOV (0x5F3766); HUD, reason text, refuel and no-fuel foley (0x5F857A, 0x5F3C82); Rhino forced-forward input and camera lock (`control_gameplay_process` path) |
| Built for remote wearers | `effects_alr_attack/loop/release` (0x57C8A0 / 0x599D30 / 0x59A7C0): a non-local wearer gets the `_NPC` sound at the pack. Material effects (`start_material_effects` 0x599B60, weapon in `process` 0x5F3D7F): no local gate |
| Visibility gate (R1, closed) | Only `effects_alr_attack` tests it: wearer `+0x56 & 0x80` (0x57C8BF, skip to 0x57CADA). The bit is the object's visible flag (`object::get_is_visible` 0x2C38A0 = `[ecx+0x56] >> 7`). The puppet has it set by `object::serialize` at creation (0x6A2B8F) and by puppet.cpp `SetVisible` (`object_set_visibility_plus_children` 0x68C08B), and keeps it while visible. Loop effects (`effects_alr_loop` 0x599D30, `effects_loop` 0x57CAF0), release, materials and the jetpack flames (`effects_play` 0x69E3E9) have **no** visibility test: a failed gate would lose only the activation glares and the attack sound |
| Sound and asset residency (R2, closed) | Every `MP_BACKPACK_*` cue, local and `_MONO` remote, is in the `mp_backpacks` sound and wave banks, loaded at boot in every mode (`gameaudio_system_init_stage_2` 0x3E8EF0; tables 0xA21CD, 0xA253C; no MP test). `mov_jet_pack` likewise. Thrust, Rhino, Shockwave sounds come from effect `<sound>` fields and reach a remote wearer as the 3D `_MONO` cross-linked cue (0x3BDEDC-0x3BDF13). Every pack visual is in `effects.vpp_pc`, preloaded at boot (0x345476-0x3454B9). Only `mp_extreme_blood_hit_a` (and the unused `mp_kaboom_pack_exp_a`) live in `effects_mp.vpp_pc`, built only by `mp_preload_visual_effects` 0x3455A0 (MP only); a missing effect is skipped safely (0x34D0E4-0x34D0F4, 0x57C8C9, 0xA04F6) |
| SP-or-server gameplay | heal 0x5F2925; rhino ram 0x5F2DA0; Thrust landing end 0x5F38F5; fuel-out end 0x5F3AFA; kaboom explosion 0x5582ED; shockwave (melee) explosion 0x558253; ripple hits 0x5F18CA; rhino end cutter 0x59AABA |
| SP fuel | drain `fuel_use_per_second × Frametime_real`, fuel-out → `deactivate` (0x5F3B12). Idle regen is `Jetpack_charge_rate` 0.1/s × the **observer's** jetpack upgrade (×2 at level 2), not the table's regen; MP-only overrides do not apply |
| Common to every activatable pack | ramp-up 300 ms + ramp shake: local only, never synced (MP parity). Activation shake: local only. Fuel glow `Backpack_fuel_effect_charging/charged/active` + pack shader "active" (0x5F3D94-0x5F3EA7, `update_render` 0x57D06D): visual, synced by `activate` + wire fuel. End: `deactivate` → `camera_shake_stop()` 0x2C6F40 clears **all of the observer's** current shakes (0x59AA78, deactivatable packs only) → gate G5. Also: `activate` (0x5F20DA), `event_player_dropped` (0x5DF205), `event_player_died` (0x5F25BF), `reset` (0x59A914) and `human_ragdoll_start` (0x6C9721) call `stop_ramp_up` 0x57CF80 → `camera_shake_stop(def)` 0x2C6ED0 (0x57CFB0), which clears **your** ramp shake when the puppet wears the same pack type. Cosmetic, MP parity, not gated (one site covers all callers if it ever matters: `ExpectCall(0x57CFB0, 0x2C6ED0)`, skip when `[esi+0x274]` is the puppet) |

## 2. Per pack

Columns: **Kind** = visual, audio, animation, movement, self (the wearer only), world (Geo-Mod, objects), enemies
(NPCs), partner (the other co-op player), AI, local (wearer's own screen, input or HUD). **Today** = what the puppet
shows in ver6.2. **When**: P1 / P2 = the phases of §6 (both before M6); M6 / M7a / M7b / M8 = that milestone; never
= local only, as in MP. Rows common to all packs are in §1.

### 2.0 Jetpack (id 0, not activatable; flight while holding Jump in the air)

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Lift toward 5 m/s, spine bend, flight controller shape | movement, animation | `player_update_jetpack` 0x69E4B8-0x69E567; 0x6B60CA; 0x4153EE | works (code path; **unconfirmed in game**, Q1) | none | done |
| Flame `char_jetpack_a` on the pack; Start / Loop / Release `_NPC` 3D sounds | visual, audio | 0x69E3E9; 0x69E29A-0x69E67A | works (inferred) | none | done |
| Flame size and fuel glow follow the fuel | visual | expression var 5 (0x69E6CF); 0x5F3D94 | follow the puppet's own tank | write `bpFuel` into puppet `+0xDBC` and pack `+0x25C` | P1 |
| Fuel model | self | SP burn 0.35/s (0x69E45B); MP forces remote fuel to 1.0 (0x69E0E8, MP only) | own tank: can run dry before the partner's, so flames and lift stop early | same fuel write | P1 |
| NPC "he's flying" bark and reinforcement report | AI | `record_los_result_enemy` 0x2B98FB, local Player only, disposition ≥ 3 | not for the puppet | M7b AI-notice gate list | M7b |
| NPC turns hostile on seeing a jetpacking player | AI | `should_become_hostile` 0x288F9B, any player | already applies to the puppet | host: wanted; guest: mirror AI is off | M7a |
| `jump_pack` camera shake, HUD | local | camera-target gate 0x69E56C | - | none | never |

### 2.1 Thrust (id 1): 1.25 s, full tank, cannot be switched off, ends on landing

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Launch jump (on the ground) | animation, movement | `activate` → `human_jump` 0x5F22AC | shows via the replayed jump id (0x5CF433) | `activate` jumps too: check for a double jump (risk R4) | P2 |
| Boost: vy = max(vy, 2) + 10 (down-thrust: vy − 10), horizontal × 0, airborne state | movement, animation | `apply_jump_boost` 0x557100, 180 ms after activation (`process` 0x5F38C6) | none: the puppet pops up by position correction (inferred) | `activate` with `+0x2AC` from wire bit 5 (only the local wearer fills it, 0x558C3B) | P2 |
| Loop `mp_thrust_loop` (down: `mp_thrust_down`), attack sound `MP_BACKPACK_THRUST_ATTACK` | visual, audio | `effects_alr_*`; `+0x321` derived from `+0x2AC` (0x5F22C6) | none | `activate` | P2 |
| Landing dust `mp_thrust_dust` | visual | `deactivate` 0x59ABB5-0x59ABFE, only when `+0x2AD` = 1 | none | the observer's own landing end (0x5F38F5, SP = server), or wire bit 6 → `+0x2AD` before `deactivate` | P2 |
| Damage taken ×0, steel sparks when shot, no fall scream | self, visual | `human_damage` 0x6CEA51; victim `+0xE8` 0x7DBB5E; 0x69B718 | blood instead of sparks | `activate` | P2 |
| Ram: shape cutters on buildings hit (0x5F3296-0x5F32D4) and one at the end (0x59AACF-0x59ABA7) | world | SP-or-server gates 0x5F2DA0 / 0x59AABA | partner's machine only | G6 (movers) and G7 (end cutter) by policy | P2, M6 |
| Ram on NPCs: 300 damage + ragdoll launch, any team | enemies | `rhino_process_npc_hit` 0x557220 (ragdoll source −1, so its impact damage passes the firewall) | none | G6 rejects until M7a; then host only | M7a |
| Ram on vehicles | world | `rhino_process_vehicle_hit` 0x57C100 | none | G6 rejects until M8 | M8 |
| Ram on players | partner | filter needs a valid MP id `+0x126C` (0x57C00A): never in SP | none | G6 rejects anyway | never |
| Ramp burst, shakes, cover exit, zoom, control permissions | local | local gates | - | none | never |

### 2.2 Fleetfoot (id 2): toggle, ≤ 5.3 s

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Contrails on both forearms, both feet and the pack, intensity = speed | visual | `effects_alr_*`, expression var 0 (0x57CC0E) | none | `activate` | P1 |
| `MP_BACKPACK_FLEETFOOT_NPC` loop | audio | 0x57C8CE-0x57C8F2 | none | `activate` | P1 |
| Max walk / run speed ×1.45, so also the run blend | movement, animation | `player_get_max_movement_speeds` 0x6855AD; anim lookup 0x6BB0E3 | runs at 1.0×, falls behind and is snapped forward (inferred) | `activate` | P1 |
| FOV ×1.25, zoom off, shakes | local | 0x5F3766 | - | none | never |

No enemy, world or partner effect.

### 2.3 Stealth (id 3): toggle, ≤ 12 s

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Fade to a refraction over 1.0 s (body, pack, held and carried weapons); back over 0.75 s after it ends | visual | `stealth_player` 0x5588E0 → `set_stealth_plus_child_items` 0x654390 (`+0xAC0`, a one-frame diffraction latch re-sent every frame by `process` 0x5F282C-0x5F2913); `unstealth_player` 0x5589B0 | fully visible | `activate` / `deactivate`; the fade-out finishes by itself | P1 |
| `stealth_pack` stencil-mask material on pack, body and weapon | visual | 0x599B60, 0x5F3D7F | none | `activate` | P1 |
| `MP_BACKPACK_STEALTH_NPC` loop | audio | `effects_alr_*` | none | `activate` | P1 |
| EDF and Marauders drop the stealthed player as a combat target (SP campaign, not DLC or WC) | AI | `human_combat_get_target` 0x62F5DC → `human_is_player_in_stealth` 0x62F560, which tests `== Player` only | the observer's soldiers keep shooting the invisible puppet (harmless: swallowed) | G9 at 0x62F5DD: required on the host from M7a; optional earlier (Q5) | M7a |
| The observer's aim assist and crosshair skip a fully stealthed puppet | observer | 0x65DE76, 0x4B2FC4 | - | comes with `activate`; harmless between allies | P1 |
| Firing breaks stealth | gameplay | MP server only (`event_player_attacked` 0x5DF165) | - | keep the SP rule on both machines: it does not | never |

Guide claims to ignore: the 0.05 / 0.25 transparency fields have no reader; the top-level `damage_taken_factor 0.8` is
never parsed (all active factors are 1).

### 2.4 Vision (id 4): toggle, ≤ 7.5 s

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Additive `vison` glow on the pack and the weapon | visual | 0x599B60, 0x5F3D7F | none | `activate` | P1 |
| `MP_BACKPACK_VISION_NPC` loop | audio | `effects_alr_*` | none | `activate` | P1 |
| X-ray: every human (friend or foe) through walls in one `xray_vision` material; blur and noise ramp | local | 0x57C356 / 0x57C522; `process` 0x5F3984 | the partner's own X-ray already works on his screen | none | never |

No gameplay at all (every factor 1, no AI reader).

### 2.5 Commando / Firepower (id 5): 6.67 s, full tank, cannot be switched off

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Additive `firepower` glow on the **pack and the held gun** (not the body) | visual | 0x599C20-0x599CA6; gun 0x5F3D7F → `weapon_start_material_effect` | none | `activate` | P1 |
| Red glare loops `mp_commando_activation_a` @`Fuel_Level`, `_b` @`tail_bone-prop` | visual | `effects_alr_attack` 0x5F2385 | none | `activate` | P1 |
| `MP_BACKPACK_COMMANDO_NPC` loop | audio | 0x57C8CE-0x57C8F2 | none | `activate` | P1 |
| Red tracers `mp_commando_tracer_a` (rail driver: `Wep_rail_driver_trail_b`) on guns with `Tracer_Frequency` > 0 | visual | `weapon_create_tracer_base` 0x7DC402 / curved 0x7DC2B5 read the **shooter's live pack** | standard tracers on the puppet's replayed shots | `activate`; nothing else (shooter = puppet, from weapon `+0x13C`) | P1 |
| Extra fiery puff `wep_bullet_impact_exp` at every bullet hit | visual | `weapons_create_bullet_hit_effects` 0x7DBA72-0x7DBB54 | normal impacts | `activate` | P1 |
| Player victims: in MP the normal impact is switched off (`xor bl,bl` 0x7DBB54, skips 0x7DBCE6) for `mp_extreme_blood_hit_a`, which SP never loads (`effects_mp.vpp_pc`). Blood still shows in SP (seen in game, ver6.3.1); cause not verified: `+0xE0` may be 0 in SP, so `je` at 0x7DBB13 skips 0x7DBB54; or the material impact at 0x7DBC51-0x7DBC9C still runs when `bl == 0`; or the gore comes from the human damage path | visual | same; `bp_gap_3.md` | normal blood | none | - |
| Direct damage ×1.25 on humans (bullets, melee; non-explosive types) | enemies | `human_damage` 0x6CEDA0-0x6CEDD5 | puppet hits on humans are swallowed today | from M7a the host applies the guest puppet's hits; `activate` on the host's copy gives ×1.25 automatically (Q4) | M7a |
| Explosion reach ×1.25 (radius, flinch and knockdown radii; not peak damage) | enemies, world | `explosion_apply_damage` 0x2EF7F0; 0x2EC8A9; 0x6DBC93 | - | comes with `activate` | P1 / M7a |
| Hammer blast `mp_commando_melee` on every melee **hit**, ≤ 1 per 0.5 s: radius 2.5 m, falloff edge 3.75 m, humans 50-400, vehicles 50-800, **structural 20000**. Knocks down NPCs over almost all of the 3.75 m (impulse 15000 × damage / 400 > 100). Never knocks down or hurts a player (below) | world, enemies | `human_combat_unified_melee_hit` 0x6D147E → `attempt_shockwave` 0x558BB0 → `explosion_create` 0x558281 | none (the puppet's replayed swings hit locally, but the passive set has no blast) | `activate`; G2 by policy | P1, M6 |

### 2.6 Shockwave / Concussion (id 6): one-shot, costs the full tank

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Ring `mp_shockwave_active_a` at the feet + `MP_BACKPACK_SHOCK_ACTIVE` | visual, audio | `use_ripple` → `effects_play` 0x5F18C2, ungated | none | `activate` (edge counter: `+0x254` is back to 0 before any capture) | P2 |
| Radial impulse 4000 on loose objects within 6 m | world (physics) | 0x5F1E7B-0x5F2002, ungated (MP clients run it too) | none | keep in every policy | P2 |
| Blast `mp_shockwave_blast`: structural 15000; knocks down NPCs only within its 2.5 m `Radius` × the area factor (not 5.5 m: the knockdown radius is only the damage falloff edge, `bp_gap_2.md`); nothing to players | world, enemies | `use_kaboom` → `explosion_create` 0x558323 | partner's machine only | G1 by policy | P2, M6 |
| SP ripple: every human within 6 m except the wearer is ragdolled + 130 damage, **no team test** (NPCs, your Player, the puppet) | enemies, partner | 0x5F18CA-0x5F1AB7 → `ripple_process_npcs_hit` 0x557340 | your own ripple already floors the puppet on your screen | G4 always (no co-op knockdown, MP's teammate rule); G3 off for NPCs until M7a host | P2, M7a |
| SP ripple on vehicles: 520 damage, velocity × 0.5 | world | `ripple_process_vehicles_hit` 0x557420 | none | G3 (same switch as NPCs) | M7a / M8 |

Note: the SP ripple branch needs `Current_bundle_id == -1` (0x5F18E3). If loading DotB content into the campaign ever
sets a bundle id there, the ripple takes the player branch instead (risk R7).

### 2.7 Heal (id 7): ~1.04 s burst, full tank, cannot be switched off

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Blue ring `mp_backpack_heal_a` on the body, additive `heal` material on the **body**, `MP_BACKPACK_HEAL_NPC` loop | visual, audio | `effects_alr_*`; 0x599CEF | none | `activate` | P1 |
| +450 HP/s (≈ 470 HP total) to every living `player` within 5.8 m, wearer included | partner, self | `process` 0x5F2918-0x5F2A73, SP or server; `collect_objects<player>` (never NPCs) | the partner's heal never reaches you; his own HP shows via PSTATE `hp` | comes with `activate`: your machine heals **your** Player near the puppet (right authority: each side owns its HP). Default yes (Q2) | P1 |
| A heal raises a downed player (HP 1 under never_die) | partner | same | - | M7b revive rule (Q6) | M7b |

### 2.8 Rhino (id 8): 0.77 s charge, full tank, cannot be switched off

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| **Charge pose**: state 0xF1 "mp rhino" (`male_run_rno.animx`), blend 0.3 s, override flag; cleared in `deactivate` | animation | `activate` 0x5F2188-0x5F21CE; `deactivate` 0x59AA84-0x59AAA6 | normal run or sprint | `activate` (§5) | P2 |
| Speed 8.2 m/s (`+0x28C` every frame; max speed = `charge_top_speed`) | movement, animation | `process` 0x5F2C88-0x5F2D8B; 0x685813 | passive max speed: correction drags it (inferred) | `activate` | P2 |
| Hex shield `mp_rhino_shield`, engine flames `mp_rhino_engine_blast`, arc `mp_rhino_arc`, additive `invulnerability` on the body, `mp_rhino_active_sound` | visual, audio | `effects_alr_*`; 0x599CEF | none | `activate` | P2 |
| Damage taken ×0, steel sparks when shot | self, visual | 0x6CEA51; 0x7DBB5E | blood | `activate` | P2 |
| Cancels the wearer's melee, reload and triggers | self | 0x5F21D6-0x5F2209 | - | harmless (the partner cannot attack while charging) | P2 |
| Cutters `mp_rhino_cutter` (radius 2.5, structural 20000) at every building hit, plus one at the end of every charge | world | 0x5F31E9-0x5F32D4; 0x59ABA7 | partner's machine only: the puppet runs into an intact wall (inferred) | G6 (movers) + G7 by policy | P2, M6 |
| NPCs hit: 225 damage + ragdoll launch, any team | enemies | 0x557220 | none | G6 rejects until M7a host | M7a |
| Vehicles hit: 900 damage + impulse | world | 0x57C100 | none | G6 rejects until M8 host | M8 |
| Cutters (0 human damage) only make unarmored NPCs within 2.5 m flinch: no knockdown, no push, nothing to players (corrects `bp_heal_rhino.md` "Missed 1" and its "NPC firewall does not hold for cutter knockdowns") | enemies | `shape_cutter::apply_explosion` → `explosion_process_humans` 0x2EC850 → `human_combat_process_explosion_hit` 0x6DB710 (flinch at 0 damage 0x6DC221-0x6DC2B4) | - | none | - |
| Forced forward, camera lock, control permissions, cover exit, zoom off | local | 0x2FC450, 0x2FD624, 0x2FBD88 | - | none (the partner's input already makes the motion) | never |

### 2.9 Tremor (id 9): 4 s, full tank, cannot be switched off

| Effect | Kind | Mechanism | Today | Sync plan | When |
|---|---|---|---|---|---|
| Heat-haze loop `mp_backpack_tremor_a` on the body; `MP_BACKPACK_TREMOR_NPC` loop | visual, audio | `effects_alr_*` | none | `activate` | P1 |
| Quake `mp_quake` at the pack at activation and every 300 ms (≈ 14): crumble 9 m, structural 2500, no human damage and nothing else to humans (flinch radius 0.1 m), AI sound radius 150 | world, AI | `use_kaboom` (0x5F2183, `process` 0x5F395C-0x5F397F) → `explosion_create` 0x558323 | partner's machine only | G1 by policy; AI on the host from M7a | P1, M6, M7a |
| Quake camera shake `explosion_quake` on **every** machine: 1.0 × (1 − d / 60 m) for others, 0.4 for the wearer, even with shakes disabled | visual (camera) | `use_kaboom` 0x558366-0x5584D9, runs after the explosion gate | none | comes with `activate`, even when G1 blocks the quake (MP clients get it too) | P1 |

## 3. Protocol

### 3.1 PSTATE protocol 6 (`kProtocol` 6, `sizeof(MsgPState)` 152 → 156)

Append, after `item`:

```cpp
uint8_t bpAct;   // bits 0-3 activation counter, bit 4 active (pack+0x254), bit 5 Thrust up (+0x2AC), bit 6 landed (+0x2AD),
                 // bit 7 frozen (a still re-send: level stale, fuel too except for the Jetpack; ver6.3 fix, §3.3)
uint8_t bpFuel;  // pack+0x25C * 255 (Jetpack: process mirrors +0xDBC into it while flying, 0x5F394A)
uint16_t pad;    // keeps the struct naturally aligned
```

Why PSTATE and not a reliable control message:
- The pack event must land on the **replay** timeline, where the puppet actually is (a Rhino charge, a Thrust launch, a
  Shockwave ring). PSTATE is applied in `Effects` at the state's `network_time`, as the `det` counters already are.
- A reliable message on channel 0 is not ordered with channel 1 and arrives about `kInterpMs` (100 ms) plus jitter
  before the replayed movement. It would need its own clock-mapped queue: more code for no gain.
- Loss: the 4-bit counter is in every packet, so a lost packet loses no edge (it would take 16 activations between
  two received packets to wrap it). The level and the fuel are re-sent at 20 Hz.

### 3.2 Sender (`Capture`, read only, no hook)

- Read the worn pack (already done for `s.pack`). The counter steps when the first dword of `pack+0x268` changes to a
  value other than −1 while the **same pack object** is worn. Its only writers are the constructor, pool init,
  `reset`, `activate` (0x5F2337, a new tick each time) and `deactivate` (0x59AC27, −1). This catches the Shockwave
  one-shot, whose `+0x254` is already 0 again when `Capture` runs.
- On a pack object change (shop, Q17 pick, load), re-baseline without counting.
- Level = `+0x254`; bits 5 / 6 = `+0x2AC` / `+0x2AD`; fuel = `+0x25C`.

### 3.3 Receiver (`Puppet_In` → `Snap` → `Effects`)

- `Puppet_In` turns the counter into an edge exactly like `det`: `edge = g_bpHave && ((m.bpAct ^ g_bpSeen) & 0xF)`, then
  `g_bpSeen = m.bpAct, g_bpHave = true`. Reset `g_bpHave` everywhere `g_detHave` is reset (link, partner READY after a
  load, line 1073). The `Snap` gets `uint8_t bp` (edge, level, up, landed) and `uint8_t fuel`.
- Each packet is its own ring entry, and `Effects` runs per entry, so the 5 ms thinning in `Deliver` loses nothing.
- **Late join, re-join, relink, pack change, back from hidden:** set `g_bpResync`. On the next visible state, a level-1
  pack that is not active locally is activated with the wire fuel (a Stealth already running shows stealthed). One-shots
  missed while away are simply lost. This is the only time a level without an edge activates.
- **Fuel-out flicker (perception verification #12):** the puppet's local `process` can end the pack one state before
  the partner's level-0 arrives. A stale level 1 must not re-activate it, hence "activate on edge or resync only".
- **Hidden** (partner in a vehicle, out of your stream, loading): `deactivate` an active puppet pack at once, so no
  quake or heal runs at a hidden puppet, and set `g_bpResync`.
- **Partner paused** (ver6.3 fix): `Puppet_Out`'s still state (while the sender is paused per `HostPaused` or, on the host, holds its join token while the guest lands, or after
  150 ms without a capture; the ver6.3 test showed the Esc menu keeps Capture running) carries the last level and
  fuel; rewritten every state, they kept the puppet's pack on for the whole pause (the guest's own Esc or map pauses
  only the guest: endless Tremor quakes or Heal on the host). Such a state sets bpAct bit 7 (frozen) and the receiver
  treats it as hidden for every pack except the Jetpack, which still gets its fuel (a paused tank does not change, and
  `Still` keeps state bit 8, so the puppet hovers and would otherwise burn its own `+0xDBC` dry and sag); the first
  live state resyncs. A capture hitch ≥ 150 ms costs one off/on, and so does a host pause with a pack on, on the guest
  (its co-op token keeps it in state 1, so it takes the frozen states; `on id N (resync)` at resume replays the glare
  and sound). Suppressing that would need a guest token ack: not worth it.
- **Hidden / frozen ends make no cutter** (ver6.4 fix): hidden, partner paused, away (your own menu or map: the first
  state back has Snap bit `kAway`, and its level 0 ends the pack with no dust) and session down (`Destroy` drops the
  pack) make no end cutter (`gc_cutterSkips` counts them). `deactivate` builds the Rhino / Thrust end cutter (0x59AAAE-0x59ABA7) for any end,
  and the partner's charge goes on, so `PackEffects` and `Destroy` clear the world bit for those calls only; the partner's level
  0 (or the puppet's own landing or fuel-out) still cuts the one real end hole. A Thrust forced off mid-flight loses its
  landing hole and dust on the observer (it is never resynced): accepted, parallel worlds until M6. No stale dust:
  `activate` clears `+0x2AD` (0x5F2270).
- **Stale edge** (ver6.3 fix, ver6.4): a counter step first seen after states skipped in your own menu, map or load
  (`Puppet_In` returns before the counter), in a state 250 ms+ late, or any due state after 250 ms without a puppet
  frame (your own sim stall, a hitch or a minimised window, drains every queued state in one frame at the puppet's old
  place, the last 150 ms of them only just due; such a state's level 0 is a quiet `kAway` end), or
  after a loss burst (5+ PSTATEs lost in a row by `seq`, about 250 ms; a sender's own hitch skips no `seq` and maps on
  time), is `kStale`: a resync, so a Shockwave fired in the gap is lost rather than fired
  late at the puppet's current place, and a Thrust is not launched late. An edge whose level is already 0 never
  activates: a real activation of a deactivatable pack is still on at the capture that counts it, and the sender sends
  that capture at once (`Puppet_Step` skips the 50 ms gap: a low down-Thrust can start and end within one gap, so the
  latest capture alone would carry the step with level 0). That one PSTATE can be lost (channel 1 is unreliable, late
  ones are dropped), so the step's level is repeated in one more state (`Puppet_Out` `g_bpRepeat`): if the step's
  state is lost, the repeat carries the step with level 1 (activate about 50 ms late, then the level 0 with the landed
  bit ends it with dust and end hole; a repeat with the landed bit on a puppet already on the ground is dropped, as
  before the repeat: `activate` would jump it, 0x5F22AC); if not, the repeat is a level-1 state without a step (fuel only). Only a
  one-shot (`info+0x6EF` clear, Shockwave) has a real level-0 edge, and it is not repeated.
- **Both directions** are the same code: each side sends its own Player's pack and drives its puppet. Only the gate
  policy differs by role (§4.3).

## 4. Code design: driving the puppet's pack

### 4.1 Where and on which thread

On the **sim thread**, in `Effects` (from `BeforeSim`, before `simulate_frame`, hence before this frame's
`multi_game_backpack_process`), right after the `SetPack` line, and only when `g_pack == s.pack`, the pack's wearer
`+0x274` is the puppet, and the puppet is streamed (`+0x55 & 0x10`). The net thread never touches game memory (as
today).

**Order is a requirement** (`bp_gap_1.md`): the pack block must stay **after** `SetVisible(p, s.visible)` in `Effects`
(puppet.cpp line 767 today) and act only while `s.visible`, and the unhide resync must re-run `activate()`. The
activation glares and the attack sound are one-shot and pass the visibility gate (0x57C8BF) only while the puppet is
visible, so an activation applied to a hidden puppet loses them for that activation. ver6.2's `SetPack` already obeys
this order.

```cpp
// ponytail: an allowlist per phase (P1: ids 0, 2, 3, 4, 5, 7, 9; P2 adds 1, 6, 8) instead of per-pack code
uint8_t* bp = Field<uint8_t*>(p, 0x1170), *bi = bp ? Field<uint8_t*>(bp, 0x270) : nullptr;
if (!bi || g_pack != s.pack || Field<uint8_t*>(bp, 0x274) != p) return;
float fuel = s.fuel / 255.0f;
bool on = Field<uint8_t>(bp, 0x254), level = s.bp & kLevel;
if (!s.visible) { if (on) Deactivate(bp); g_bpResync = true; return; }
if (Field<uint8_t>(bi, 0x248)) { Field<float>(bp, 0x25C) = Field<float>(p, 0xDBC) = fuel; return; } // Jetpack: fuel only
if (!Field<uint8_t>(bi, 0x6EE) || !Allowed(s.pack - 1)) return;
// resync never re-launches a Thrust mid-flight: info+0x198 = active jump_boost (the key MP's type bytes use, 0x558C70)
if ((s.bp & kEdge) || (g_bpResync && level && !on && !Field<uint8_t>(bi, 0x198))) {
    if (on) Deactivate(bp);                                   // deactivate is unguarded: test +0x254 first
    Field<uint8_t>(bp, 0x2AC) = (s.bp & kUp) != 0;            // Thrust direction before activate (0x5F22C6)
    Activate(bp);                                             // thiscall 0x5F2030, as an MP client (0x5F8271)
    Field<float>(bp, 0x25C) = fuel;                           // after activate: it subtracts the activation cost
} else if (!level && on) {
    Field<uint8_t>(bp, 0x2AD) = (s.bp & kLanded) != 0;        // Thrust landing dust (0x59ABBA)
    Deactivate(bp);                                           // thiscall 0x59AA30
} else Field<float>(bp, 0x25C) = fuel;                        // glow; local fuel-out within one state of the partner's
g_bpResync = false;
```

Never call `player_toggle_backpack_activation`, `toggle_activation` or `start_ramp_up` on the puppet: in SP they go
through `can_activate` (a local fuel check that can refuse) and a 300 ms ramp whose effects are local-only anyway.

Never call `mp_preload_visual_effects` 0x3455A0 or `multi_game_do_mp_preload` to get MP-only assets: they release the
SP-only effects (0x345662-0x34569D), which include the Tremor haze `mp_backpack_tremor_a` (`bp_gap_3.md`).

### 4.2 What stays off by itself, what `activate` touches

Stays off for the puppet, with no code (local gates, §1): camera shakes of the ramp and the activation, cover exit,
zoom, FOV, X-ray and the Vision screen effect, HUD, reason text and foley, the Rhino input and camera lock. Stats: the
MP stats on the puppet are already skipped (0x6E6B44); the heal XP event is MP-server only (0x5F2A7B). Your own pack:
nothing writes `Player+0x1170` or `s_lastBackpackIndex`; the regen only reads your upgrade level.

Side effects of `activate` / `deactivate` on the puppet (all harmless today, to keep in mind):
- `player_stop_material_effect(puppet)` (0x5F2349) wipes every material effect on the puppet, so any material the mod
  puts on it later is lost at activation;
- `player_set_player_invulnerable(puppet, false)` (0x5F235B) does nothing while `+0xB4` bit 5 is clear (the mod never
  sets it);
- `deactivate` zeroes the puppet's `+0xDBC` (0x59AC0F): the next state's fuel write restores it;
- a pack swap while active deactivates through `event_player_dropped` (0x5DF1F4), which also cuts your shakes (G5
  covers it);
- `activate`, `event_player_dropped`, `event_player_died`, `reset` and `human_ragdoll_start` call `stop_ramp_up`, which
  can end your own same-type ramp shake early (§1, MP parity, not gated).

### 4.3 Gates: what must not run on the observer's machine

One policy value per role, `g_ppWorld` bits: 1 = world (pack explosions and cutters), 2 = NPCs (ripple and ram hits),
4 = vehicles (ram hits). Each gate acts only when the pack, or the explosion owner, belongs to the puppet. Every site
was read in the disassembly; check the bytes at install, like the other `ExpectCall` sites.

| Gate | Site | Patch | Covers | Phase |
|---|---|---|---|---|
| G1 | 0x558323 `call explosion_create` in `use_kaboom` (cdecl, 8 args, caller `add esp,0x20`, result unused) | thunk: **owner = arg 3** (`[esp+0xC]` at entry; arg 2 is the ignore object) is the puppet and world bit off → return | Tremor quakes, Shockwave blast. The kaboom camera shake after it still plays (MP-faithful) | M6 |
| G2 | 0x558281 `call explosion_create` in `use_shockwave` | same thunk | Commando hammer blast | M6 |
| G3 | 0x5F18CA `cmp byte [Multiplayer],bl` + `jne` (6 + 6 bytes) in `use_ripple` | cave: ebp (the pack) is the puppet's and NPC bit off → `jmp 0x5F1E7B` (object impulses only, what an MP client runs) | Shockwave ripple on NPCs and vehicles | P2 |
| G4 | 0x5F1942 `call world::object_from_handle<human>` in the SP ripple loop | after the call: eax is `Player` or the puppet → eax = wearer `[ebp+0x274]`, so 0x5F194F skips it | no co-op knockdown by any ripple, both directions. **Always on** | P2 |
| G5 | 0x59AA78 `call camera_shake_stop` (RVA 0x2C6F40) in `deactivate` | skip when `[esi+0x274]` is the puppet | the partner's pack ending (or a swap) no longer cuts your shakes. Vanilla MP clients have this glitch; fixing it is cheap | P1 |
| G6 | imm32 at **0x5F3188** (`c7 44 24 7c 50 bf 97 00` at 0x5F3184: the spherecast filter 0x57BF50, relocated) | point it at a wrapper: for a puppet wearer, reject players always, NPCs unless the NPC bit, vehicles unless the vehicle bit, movers unless the world bit; else call 0x57BF50 | every Rhino and Thrust ram hit and in-charge cutter, one 4-byte write | P2 |
| G7 | 0x59ABA7 `call world::create_object_internal` (`e8 a4 39 fa ff`) in `deactivate` | skip when `[esi+0x274]` is the puppet and world bit off | the end-of-charge cutter (Rhino, Thrust) | P2 / M6 |
| G8 (optional, cosmetic) | `explosion_process_humans` 0x2EC9F7, 9 bytes `80 3D <Multiplayer> 01 75 23` (MP's friendly skip), checked like PP-MELEE | jump to a naked cave: owner (ebx) = puppet and victim (esi) = `Player`, or owner = `Player` and victim = puppet → `jmp 0x2ECAB0` (keeps the AI block); else `Multiplayer` → 0x2ECA00, SP → 0x2ECA23. eax, ecx, edx free; same stack depth. Design in `bp_gap_2.md` §5 | hides, for every puppet explosion in both directions: the HUD hit marker (0x6DC02A), script event 0x2F (0x6DC160), the vehicle-entry/exit ragdoll (0x6DC398-0x6DC451) and a puppet blast using up a mission-armed ragdoll flag. **Not** needed against knockdown (see below) | none required; if Q3 = yes, P1 with the hammer blast |
| G9 | 0x62F5DD `call human_is_player_in_stealth` (cdecl, 1 arg, `add esp,4`) | `ChainRel32`: for the puppet, return `is_stealth_active` 0x558B20 on its pack | enemies lose a stealthed partner | M7a host (optional earlier, Q5) |

Policy by milestone:

| Piece | Now (P1, P2) | M6 host (guest's puppet) | M6 guest (host's puppet) | M7a host | M7a guest | M8 |
|---|---|---|---|---|---|---|
| Pack explosions G1 / G2, ram cutters G6 movers, end cutter G7 | **on** (parallel worlds, like the puppet's charges today; Q7) | on: captured and sent like any destruction | **off**: the host's destruction stream carries them (MP-client view) | on | off | as M7a |
| Ripple on NPCs and vehicles (G3) | off | off | off | **on** | off | on host |
| Ram on NPCs (G6) | off | off | off | **on** | off | on host |
| Ram on vehicles (G6) | off | off | off | off | off | **on host**, through the vehicle firewall |
| Co-op knockdown by ripple (G4) | never | never | never | never | never | never |
| Your shakes cut (G5) | never | never | never | never | never | never |
| Stealth vs enemies (G9) | optional (Q5) | - | - | **on** | not needed (mirror AI off) | on host |

Why "off" for NPC hits now: the ram and ripple ragdoll an NPC with source −1, so the firewall (`OnHumanDamage`, which
swallows damage *by* the puppet) misses the impact and get-up damage: they can kill the observer's NPCs (inferred).

Why no explosion floors a co-op player in SP (`bp_gap_2.md`, verified): `human_combat_process_explosion_hit` 0x6DB710
ragdolls a player victim only if it is `Player` **and** `Player+0xDCC` bit 29 is set (0x6DC2B9-0x6DC366). Only the
mission action `player_ragdoll_on_next_explosion` (0x7226B0-0x7226C3) sets it, and the hit clears it (0x6DC2FB). Flinch
needs damage > 0, and `OnHumanDamage` swallows damage by the puppet. This covers the hammer, the Shockwave blast,
cutters, quakes and also the partner's grenades and remote charges. The puppet as victim fails at 0x6DC2E4
(`object_is_local_player` 0x614BD0 is `== Player`). Exceptions: a player entering or leaving a vehicle when the blast
lands (0x6DC398-0x6DC451, no flag needed), and physics knockdown by flung debris (not traced). A local ragdoll on the
puppet (your ripple, vehicles, physics, impaling weapons) does not stick: the next state with ragdoll = 0 makes
`apply_movement_state` call `human_ragdoll_get_up(puppet, 1)` (0x5CF6A8-0x5CF6CD), so you see a flop and a get-up
while the partner runs on (answers `bp_offense.md` open question 3; G4 removes that flop for the ripple).

## 5. Animation design

- **Rhino:** `activate` itself makes the two calls: `character_instance::set_state(puppet+0x32C, {0xF1,-1}, {-1,-1},
  0.3f, false)` (0x402110, ret 0x18) and `human_override_default_animation_state(puppet, 1)` (0x6082C0). `deactivate`
  clears the flag and `+0x28C`. The flag holds the pose through human states 0, 6, 7 and 15. A jump (state 1) replaces
  it, and after landing the current state stays until `deactivate`, exactly as in vanilla and MP. The run blend input is
  `+0x28C` / max speed, both set by the active `charge`. Every MP character uses `Player.xtbl` / `Player_Female.xtbl`,
  so the puppet has the state.
- **Fleetfoot:** no animation; the ×1.45 max speed brings the run blend in line with the replayed speed.
- **Thrust:** no animation of its own. `human_jump` + `apply_jump_boost` → the generic airborne / fall states. Risk of
  a double launch with the replayed jump id (R4).
- **Jetpack:** done (spine bend on `+0xDCC` 0x100).
- **Everyone else:** nothing exists to sync.
- **Pack-mesh animations** (`BackPacks` set: `rhino_activate`, `rhino_rd`, `shckwve_activate`, `shckwve_idle`):
  **confirmed never played**, SP or MP, any wearer (`bp_gap_4.md`: all 282 vanim call sites sorted by target; the ten
  that drive an item never pass "stand ready" 0x48, "stand fire" 0x199 or "grenade throw" 0x27B to a pack; action ids
  are tag index + 0x142 from `vanim_lookup_action` 0x3F6F00, which corrects the 0x57 / 0x139 ids used in
  `bp_anim.md` §3.1 and closes its §8 jump-id item). The old reason "the Rhino item inherits the wearer's bones" was
  wrong: `animlib_set_parent_rig` 0x8946D0 binds only same-name bones (spine, spine1, spine2), so the fins and wheels
  would animate if anything triggered them. `rhino_idle` ("stand") is set by `item::stream` 0x6A80B2 on every Rhino
  and Shockwave pack and looped by `item::update_animlib` while visible: generic, so the puppet's pack has it. No sync.
- **Fallback if `activate()` is ever ruled out for the Rhino:** the two calls above on the edges, alone (pose only,
  no glow, no speed factor). About 6 lines (`bp_anim.md` §6.2).

## 6. Phases

Each phase is a build the user tests; A = host, B = guest, and every check is run **both ways** (B's pack seen on A,
then A's pack seen on B).

### P1 (ver6.3): the transport and the seven "quiet" packs

Scope: protocol 6 (§3), the receiver (§4.1) with the allowlist Jetpack (fuel), Fleetfoot, Stealth, Vision, Commando,
Heal, Tremor; G5. Explosions follow the "now" policy (on), so no G1 / G2 yet. Log lines: `partner: puppet pack on id
N` / `off id N` (`resync` when it comes from a resync) and `jetpack fuel written` once per flight.

**Status: implemented (ver6.3, 2026-10-08), confirmed in game (ver6.3.1, 2026-10-08).** Built warning-free. Code in `src/puppet.cpp`
(`Capture`, `Puppet_Out`, `Puppet_In`, `PackEffects`, G5 `ShakeStopThunk` at 0x59AA78 with `ExpectCall`) and
`src/proto.h` (protocol 6, 156 bytes). Sites, ABIs, log lines and the checks below: `ghost_impl.md` "Backpack effects P1
(ver6.3)"; protocol: `session_impl.md`. Deviations from §4.1, both for safety, with no new game call:
- hidden, **unstreamed** and "new pack not worn yet" (`g_pack != s.pack`) all deactivate an active puppet pack and arm
  the resync (the §4.1 code returned before its hidden test when `g_pack != s.pack`);
- the partner puppet only: the ghost's pack stays as in ver6.2 (its ring carries no edge).
- an edge acts only with level 1 or on a one-shot (`info+0x6EF` clear), and a frozen state (bpAct bit 7) counts as
  hidden for every pack except the Jetpack, which still gets its fuel (§3.3, review fixes);
- `Capture`'s vehicle branch sends the current counter (`g_bpCount`), not the reset `g_last`'s 0, so a link or load
  while seated gives no false edge on exit (a P2 Shockwave would fire on it).
The `off` line also says `(hidden)` when the puppet hid, `(partner paused)` on a frozen state.

Checks:
1. B flies until the jetpack is empty: on A the flames shrink and stop when B's do, never earlier (Q1 asks whether
   ver6.2 already showed the flames).
2. B Fleetfoot: limb and pack contrails, the loop sound, and the puppet keeps pace without snapping. B toggles it off:
   the contrails stop.
3. B Stealth: B fades to a shimmer in about 1 s and back in about 0.75 s when it ends (toggle or 12 s). B fires while
   stealthed: still stealthed on both screens.
4. B Vision: glow on B's pack and gun. A's screen gets no X-ray, blur or noise.
5. B Firepower (§7 lists what to expect): glow on pack and gun, red glares, red tracers from rifle, SMG and pistol (none
   from the shotgun), a fiery puff at each impact, hammer hits that blast (at most twice a second) and break A's world
   as B's charges do. It ends on both screens about 6.7 s after the start. A standing within 3.75 m of B's hammer hit
   is not knocked down and loses no health (a red hit marker may show, Q3).
6. B Heal next to a hurt A: ring and body shimmer on B, and A's health rises (if Q2 = yes).
7. B Tremor near A: haze, sound, A's camera shakes every 0.3 s (less with distance), the ground crumbles on A as on B.
8. A is in an explosion shake when B's Fleetfoot or Firepower ends: A's shake continues (G5).
9. B enters a vehicle, drives out of range or loads during Stealth or Tremor: the effects stop on A at once. B comes
   back mid-Stealth: shown stealthed.
10. Throughout, A's own pack, fuel HUD, camera, aim and controls stay unaffected by B's packs. 20 min, no crash.
11. If the glow shows but the activation glares or the attack sound are missing: R1 and R2 are closed in code, so look
    at whether `activate` ran (the `puppet pack on` log line) and whether the puppet was hidden at that moment (the
    one-shots are then lost until the unhide resync). The `Activate` log line also prints `p[0x56] & 0x80` (expect
    set). Missing loops or materials as well means `activate` never ran.
12. A screenshot of your own Firepower settles the colours (§7).
13. A turns on Firepower and shoots B's puppet: the fiery puff shows, plus normal blood (confirmed in game, ver6.3.1;
    §2.5).
14. B starts Tremor or Heal, then presses Esc or opens the map: it stops on A at once; B closes it while the pack still
    runs: it comes back (`resync`). The reverse (A pauses with a pack on) gives B one `off id N (partner paused)`,
    then `on id N (resync)` at resume: accepted.
15. A opens the map while B runs a whole Heal or Tremor and closes it after: no quake or heal blip on A.

### P2 (ver6.4): the ram and ripple packs

Scope: add Thrust, Shockwave and Rhino to the allowlist; G3, G4, G6, G7 with the "now" policy. G8 is not part of any
phase (optional, §4.3).

**Status: implemented (ver6.4, 2026-10-08), not yet tested in game.** Built warning-free. Code in `src/puppet.cpp`, plus
`Puppet_Step` in `session.cpp` `SendState` (`proto.h`) (allowlist ids 1-9; `RippleCave` G3 at 0x5F18CA, `RippleHumanThunk` G4 at 0x5F1942, `RamFilter` G6 at imm32 0x5F3188,
`EndCutterThunk` G7 at 0x59ABA7, every site byte-checked at install; policy `gc_ppWorld` from `kPolicyHost` /
`kPolicyGuest`, both 1 now, set by `Puppet_Net` from the role). Protocol stays 6 (no field missing). Sites, ABIs, log
lines and the checks below: `ghost_impl.md` "Backpack effects P2 (ver6.4)". Deviations, with evidence:
- G3 overwrites only the 6-byte `cmp` (`E9` + NOP); the `jne` stays and the cave re-does the compare (all 12 bytes are
  checked);
- R4 is fixed from the start, not after the test: `human_jump` bumps the jump id only when it jumps (early returns
  0x6C731D, 0x6C7379 cooldown, 0x6C74E1), so a disagreement between the machines makes the replay jump the puppet again
  (up to three times). After a Thrust `activate`, the puppet's `+0xC4` jump id is set to the state's;
- a counter step seen first after your own menu, map or load (`Puppet_In` skipped the states) is a resync, not an edge
  (Snap bit `kStale`): a Shockwave fired meanwhile is lost, as §3.3 says of one-shots missed while away (before, it
  fired late at the puppet's current place), and a Thrust still in flight is not launched late. Review fixes: the same
  after a loss burst of 5+ states (`seq`); the sender sends a pack step's capture at once (`Puppet_Step`, so a low
  down-Thrust keeps its level 1) and repeats that level in the next state (`g_bpRepeat`, so losing that one PSTATE
  loses no Thrust, §3.1; a late step with the landed bit on a grounded puppet is dropped); a state 250 ms+ late,
  or any due state after 250 ms without a puppet frame (your own stall), counts as away; a hide, a partner pause, your own menu or map (`kAway`) or a session drop that ends the puppet's Rhino or Thrust
  cuts no end hole (§3.3);
- R7 closed by code read: `Current_bundle_id` is written only by `dlc_set_current_id` 0x3B9E80, which the
  setup of a mod that loads DotB into the campaign calls with 1 and back with -1 in one function; the DLC branch tests teams (0x5F1CF8), and G3 precedes it.

Checks (the sounds of these three packs come from effect `<sound>` fields and must be **positional** at the puppet,
the `_MONO` cross-linked cue; no new code):
1. B Rhino: the `mp rhino` pose, shield, flames, arc and positional sound; the puppet charges at full speed without
   snapping; a wall B charges through shows the hole on A (and the end-of-charge hole); NPCs in B's path on A are
   **not** thrown (G6); A standing in the path is never knocked down (G6 rejects players; the cutters cannot floor
   anyone). Do not expect moving fins on the pack: vanilla has none (§5).
2. B Thrust: one launch (no double jump), loop effects, positional sound, the boost height matches B's, dust on
   landing; the down-thrust (Jump held at LB) shows the down effect.
3. B Shockwave next to A and some NPCs: ring and positional sound; loose objects fly; the blast breaks what it breaks
   on B. A is never knocked down (G3 keeps the ripple off; the blast cannot floor a player; no G4 line here, that is
   check 4). A's NPCs more than about 2.5 m from B are not ragdolled (G3 line: its ripple moved objects only).
   Unarmored NPCs within about 2.5 m are knocked down by the blast `mp_shockwave_blast` (impulse 6000 × damage, damage
   1 → 0 over the 2.5 m Radius; decided from the computed damage at 0x6DC221-0x6DC2B4, not from `human_damage`'s
   result), without damage: allowed until G1 at M6, like the hammer blast, and not a G3 failure.
4. A's own Shockwave next to B's puppet no longer floors the puppet (G4).
5. A's own Rhino, Thrust and Shockwave still hit A's NPCs as before.
6. B presses Esc mid-Rhino (or mid-Thrust): no hole on A at the freeze point (`off id 8 (partner paused)`, a
   `G7: its end-of-charge cutter skipped` stats line). Rhino: after resume `on id 8 (resync)` and one end hole where
   B's charge ends. Thrust: no landing hole or dust on A for that one (never resynced mid-flight; accepted).
7. B down-thrusts from about half a metre: the down effect and the landing dust show on A.
8. A (the guest) opens the map or Esc just as B starts a Rhino and closes it after the charge: no hole or dust on A
   at the puppet's pre-menu spot (`off id 8 (away)`, a `G7: its end-of-charge cutter skipped` stats line).
9. B presses F8 mid-Rhino: no end hole on A where the puppet stood (`puppet backpack -> id -1`, `puppet destroyed`).

### P3 (M6): role policy

The guest turns the world bit off for the host's puppet; the host keeps it on for the guest's puppet. Checked inside
M6's own acceptance test: a Rhino, Tremor or Firepower-hammer hole made by either player is the same on both screens,
and the guest never makes it twice.

### P4 (M7a): enemies

Host: NPC bit on for the guest's puppet (G3, G6), G9 installed. Checks: the guest's Shockwave and Rhino knock down the
host's soldiers and the guest's mirror shows it; the guest's Firepower kills in about 20 % fewer hits on the host (Q4);
EDF stop targeting a stealthed guest and resume when it ends.

### P5 (M7b) and P6 (M8)

M7b: the Heal-revive rule (Q6); optionally drop host DAMAGE to the guest while the guest's Rhino is active (a
≤ 150 ms leak at the charge's end); the jetpack bark and reinforcement report in the AI-notice list. M8: vehicle bit on
the host, through the vehicle firewall.

### Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | **Closed** (`bp_gap_1.md`): the gate bit is the visible flag, set on the puppet at creation and by `SetVisible`; it passes whenever the puppet is visible | keep the §4.1 order; P1 check 11 logs the bit once. Inferred only: that no other game code clears a visible puppet's bit (the clearers found target other objects) |
| R2 | **Closed** (`bp_gap_3.md`): every pack sound and every pack visual but `mp_extreme_blood_hit_a` is resident in SP | nothing visible is lost (blood still shows, §2.5); never MP-preload (§4.2) |
| R3 | `deactivate` is unguarded and always stops shakes and dual abilities | test `+0x254` first (§4.1); G5 |
| R4 | Thrust: `activate`'s `human_jump` plus the replayed jump id could double-launch; the boost fights position correction briefly | P2 check 2; if it doubles, skip the replayed jump id while a Thrust edge is pending. **ver6.4:** shown possible in code (`human_jump` bumps the id only when it jumps), so the puppet's jump id is set to the state's after every Thrust `activate` |
| R5 | The puppet's local fuel model differs (the observer's upgrade, SP regen) | wire fuel every state |
| R6 | **Corrected** (`bp_gap_2.md`): pack explosions and cutters cannot floor or hurt a player in SP; only a mission-armed flag or a vehicle entry or exit lets an explosion ragdoll the Player. Debris flung by a blast could still knock you down through physics (not traced) | none needed; G8 optional (Q3) |
| R7 | The SP ripple needs `Current_bundle_id == -1`; a DotB bundle id in the campaign changes the branch | check once in game with DotB content loaded into the campaign. **ver6.4: closed by code read** (a mod that loads DotB into the campaign sets 1 and back to -1 in one function of its setup; the DLC branch has a team test; G3 precedes both) |
| R8 | Calling `activate` on a pack that is not attached to the puppet | the `+0x274 == puppet` and `g_pack == s.pack` test in §4.1 |
| R9 | From M6, a guest-side pack explosion of the host's puppet would make the destruction twice | the M6 policy (world bit off on the guest) |

## 7. Firepower: the user's description, corrected

| The description says | The game does | Evidence |
|---|---|---|
| Left Bumper turns it on; the effects start immediately | LB (action 5). It first ramps for **0.3 s** (a charge sound and a small shake, for the wearer only), then activates. It needs a **full** tank and **cannot be switched off** once on | 0x300FC5 / 0x30103E; `start_ramp_up(300)` 0x5F8281; table: minimum 1.0, `player_deactivatable` False |
| The character flashes bright red and yellow | The additive `firepower` material glows on the **pack and the held gun**, not on the body (`on_human` False). Its colours are HDR red-orange (100, 25, 0) and red (100, 0, 0): the bright core clips to **yellow-white** and the edges stay red, so "red and yellow" is a fair description of the look. Red glare flares sit on the pack. Others see all of it; the wearer also gets a camera shake | `matfx.xtbl` → `vfx_firepower_matfx.matx`; 0x599C20-0x599CA6, 0x5F3D7F; colour channel order inferred |
| Roughly 8 seconds | **6.7 s** (1.0 / 0.15 per second), in SP and MP. Recharge from empty: 10 s in SP (5 s with the jetpack charge upgrade). 8 s is the **MP** recharge time | drain 0x5F3A95-0x5F3B12; regen 0x5F3BAC |
| All bullet and energy weapons deal much higher damage, about half the ammo per kill | **×1.25** direct damage on humans (about 20 % fewer hits, not 50 %), for any non-explosive hit including melee (the arc welder inferred). Refire, reload, magazine and ammo are unchanged. Vehicles are not boosted (inferred). Explosions reach 25 % farther, with the same peak damage. The ×1.4 / ×1.3 / ammo ×1.5 numbers in the raw table sit outside the parsed blocks and are never read: probably the source of guides' claims | `human_damage` 0x6CEDA0-0x6CEDD5; `explosion_apply_damage` 0x2EF7F0 |
| Every sledgehammer swing triggers a small explosion | **True on every hit** (a miss does nothing), at most once per 0.5 s, for any melee hit by the wearer. Not small: radius 2.5 m, knockdown 3.75 m, 50-400 to humans, 50-800 to vehicles, **20000 structural** (a remote charge does 22500) | 0x6D147E → `attempt_shockwave` 0x558BB0 → `explosion_create` 0x558281; `mp_commando_melee` |
| Bright red tracer trails replace the faint yellow ones on every round | Red tracers (`mp_commando_tracer_a`, colour keys ≈ (1.0, 0.05, 0.05)) replace the standard white-orange ones, but only on guns that make tracers: every round for pistol, assault rifle, SMG, sniper and the like; the **shotgun and autoblaster never** make tracers. The rail driver gets its own `Wep_rail_driver_trail_b` | 0x7DC3DB-0x7DC439; `Tracer_Frequency` 0x7DC494 |
| Larger, brighter sparks and dust on impact | The normal impact is unchanged; every bullet hit **adds** a small fiery "explosive round" puff (`wep_bullet_impact_exp`: flash, fireball, sparks, smoke), visual only. A player hit shows extreme blood in an MP game; in SP (and so in co-op) blood still shows (seen in game, ver6.3.1; cause not verified: `+0xE0` may be 0 in SP, so `je` at 0x7DBB13 skips 0x7DBB54; or the material impact at 0x7DBC51-0x7DBC9C still runs when `bl == 0`; or the gore comes from the human damage path) | 0x7DBA72-0x7DBB54; `bp_gap_3.md` |
| (not mentioned) | No animation of any kind | `bp_anim.md` §3.4 |

## 8. Questions for the user

**Answered by the user (2026-10-08):** Q1 yes (the jetpack showed flames and lift in ver6.2), Q2 yes (Heal heals you),
Q7 yes (break your copy of the world until M6). Q3, Q4, Q5, Q6 keep their defaults (no G8; yes at M7a; M7a only; yes
at M7b). RESEARCH.md §8 Q19.

| # | Question | Default |
|---|---|---|
| Q1 | In the ver6.2 test, did the partner's **jetpack** show flames, sound and lift on your screen? | expected yes; a "no" changes P1 |
| Q2 | Should the partner's **Heal** heal you when you stand within 5.8 m (it is what the pack is for)? | yes |
| Q3 | The partner's hammer blast, Shockwave blast, Rhino holes and charges can never knock you down or hurt you (the game itself blocks it in SP), and his Shockwave ripple never will (G4). When his hammer hits near you, you may see a red hit marker (no damage). Hide it (G8)? | no |
| Q4 | Once enemies are shared (M7a), should Firepower's ×1.25 apply to the partner's hits on the host's soldiers? | yes (as in MP) |
| Q5 | Stealth: from M7a the host's EDF lose a stealthed guest, as they lose you. Before that, should your own soldiers also stop shooting at the stealthed partner (one small hook)? | M7a only |
| Q6 | (M7b) Does a partner's Heal revive you when you are downed? | yes |
| Q7 | Until M6, should the partner's quakes, hammer blasts, Shockwave blast and Rhino holes break **your** copy of the world (as his guns and charges already do), or only be shown? | break it |

## 9. Gap round (2026-10-08): what it closed and what stays open

| Gap | Verdict | Report |
|---|---|---|
| R1 visibility gate (`effects_alr_attack`, 0x57C8BF) | **closed**: the bit is the visible flag; the puppet passes whenever it is visible. Only the activation one-shots depend on it; loops, materials and jetpack flames do not. Q1 is independent of it | `bp_gap_1.md` |
| Pack blasts vs players, G8 | **no knockdown or damage to players in SP** in either direction; G8 optional and cosmetic (hit marker, event 0x2F, vehicle-entry ragdoll). Several knockdown radii corrected (§2.5, §2.6, §2.8, §2.9) | `bp_gap_2.md` |
| R2 sound and asset residency | **closed**: all pack sounds and visuals resident in SP except `mp_extreme_blood_hit_a` (Firepower player hits still show blood in SP, seen in game; nothing to fix) | `bp_gap_3.md` |
| Pack-mesh animations | **confirmed never played**; `rhino_idle` loops for every wearer; no sync | `bp_gap_4.md` |

Net effect on the build plan: no new PSTATE bit, gate, transport or asset loading. G8 left the phases. The §4.1 order
(pack block after `SetVisible`, only while visible, resync on unhide) is now a requirement.

Still open (none blocks P1 or P2):
- which missions arm `player_ragdoll_on_next_explosion` (a puppet blast during such a moment would use the flag up);
- who listens to script event 0x2F (registered at runtime by mission scripts, inferred);
- physics knockdowns from debris flung by a puppet blast (not traced);
- whether `rhino_idle` actually moves the fins (animation content not parsed), and whether the pack sits in the item
  list `object_item_process_postsim` 0x664C70 ticks (parity holds either way);
- that vtable slot +0x14 at creation is `player::serialize` (inferred; the explicit `SetVisible` sets the bit anyway);
- callers of the virtual `set_is_visible` were not listed.
