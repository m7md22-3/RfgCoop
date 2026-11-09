# RfgCoop notes: remote player representation (puppet) and animation

Source: `research/findings/workflows/coop_puppet.md` (investigator report) and the skeptic verdicts on claims C1–C8.
Build: GOG `rfg.exe` 32-bit. Every address is an **RVA**. The `rfg.asm` listing prints VA = RVA + 0x400000; at runtime
add the module base. Tags: **VERIFIED** means someone read the disasm, PDB or exe bytes. **INFERRED** means reasoned
only. Nothing here has been run in game yet.

---

## TL;DR

- **Represent each remote peer as a second `player` object (a "puppet player"), not an NPC.** The pool holds 16, and
  creating one never touches the `Player` global. A non-local player gets its own `player_do_frame` path. (VERIFIED code
  path; the campaign runtime behaviour is INFERRED and has never been exercised by the game.)
- **Drive it with the MP client apply call `multi_game_player_process_state(player*)` 0x5E1C50** (cdecl), fed by
  `multi_sim_state<multi_player_state>::add_state` 0x568B40 on `player+0x1274`. In SP nothing calls it, so the mod
  calls it every frame. It is **not** the whole MP apply step: weapon equip and zoom happen elsewhere and the mod must
  do them.
- **Three SP-specific traps in the apply path:**
  - Vehicle and turret network handles divide by zero in SP; pre-fill the cached game handle.
  - Melee and trigger-down rolling counters never converge in SP, so swings and trigger pulls re-fire on every apply.
    Write the counters back into `puppet+0xC4`.
  - `human_ragdoll_start`'s force argument is ignored in SP, so a puppet must **not** carry the
    disallow-ragdoll flag.
- **Two side effects must be patched:**
  - Call site 0x6E62D7 (`player::do_frame`): backpack theft, radiation, district HUD and radio.
  - Call site 0x6E6B36 (`player_process_object_interaction`): the puppet eats pickups.
  Also detour `human_damage` 0x6CE950 (8 clean prologue bytes). A puppet's death is **not** free of local
  consequences: `human_died` counts it in the local career stats.
- **Local AI sees and fights a puppet by team.** Hostile puppets are noticed every perception call, with no 1–2 s
  delay. The player attack limiter never protects a puppet, so the guest takes harsher fire than SP.

---

## Facts

### 1. Several players in an SP world (VERIFIED)
- `player` 0x2E50 : `human` 0xCE0 : `object` 0x9C.
- Pool: `playerpool` 0x02C6E4A0 (189840 B). In `` `dynamic initializer for 'player_instantiator'' `` 0xC753C0:
  - pool_usage 0x2E590 at 0xC7548B;
  - **max_objects 0x10** at 0xC75496;
  - alloc `alloc_player` 0x65C3B0.
- **`Player` global 0x02C22874.** Only 5 instructions in 3 functions write it:
  - `player_set_next_active_wrecking_crew_player` 0x6108A2;
  - `player_level_shutdown_all` 0x69DD66 and 0x69DD72;
  - `player_level_init_post` 0x6D593A and 0x6D5A83.
  These write it **after** creation, as callers. Nothing takes its address. So `world::create_object_internal` on a
  player never changes `Player`.
- **`player_process_all` 0x6E7330** walks `World.subtype_objects[3]` (World+0x3F4 / +0x3FC). It calls
  `player_do_frame` only when `[[p+0x32C]+0x34] != 0` (the char_inst is ready).
- **`player_do_frame` 0x6E6290** returns at once for any non-`Player` while `wrecking_crew_is_active()`
  (0x6E6294–0x6E62AF). Wrecking Crew therefore proves that extra players can *exist* with Multiplayer=0, not that they
  get *processed*.
  - `wrecking_crew_spawn_players` 0x39F130 is called only from `player_level_init_post` 0x6D592E. It also hides the
    extra players (`wrecking_crew_hide_player` at 0x39F1F2).
  - In the campaign (WC off) the puppet runs the full non-local path. This is INFERRED from the code; the game has never
    done it.
- **`gamestate_gp_simulate_frame` 0x3EDB60 reads one player-specific value from `World.subtype_objects[3]`.** Its
  per-player loop (from 0x3EDB85) writes +0xB8 and +0x1118 for every entry. After the loop it stores the LAST entry
  in `[esp+0x10]` (0x3EDDC6), reloads it at 0x3EE16B and reads its +0x2FC stance at 0x3EE47A for the audio listener
  height (the listener position comes from `player_get_local_player`, 0x3EE3A9). An appended puppet is that last
  entry: a cosmetic listener offset. The ghost NOPs the 0x3EDDC6 store, so `[esp+0x10]` keeps entry 0 (0x3EDB88), and
  then **a puppet must never sit at list index 0.**
- 69 functions read the player count at World+0x3FC. Among them: `player_find_nearest_player*`,
  `any_player_visible_by_camera`, `activity_raid_data::player_in_raid_region`, `object_squad::get_player` and
  `cartographer::collect_humans`. AI, activities and the map will see the puppet. The exact effects are INFERRED.
- `player_level_shutdown_all` 0x69DCF0 destroys every player and sets `Player = 0`, so puppets must be recreated after
  every load.

### 2. Creation recipe (VERIFIED, from `player_multi_registration::process_create_packet` 0x5E02E0, 0x5E0657–0x5E0764)

| step | call |
|---|---|
| 1 | `obj_human_props::obj_human_props(human_info*, vector* pos, matrix* orient, char* name=0)` 0x6529A0, thiscall, 0xC-byte object |
| 2 | `world::create_object_internal(&World 0x02B97490, player::get_classname_hash() 0x610140, props, zone=0, parent=-1)` 0x53E550, thiscall, `ret 0x10` |
| 3 | `human_set_state(p, HS_STANDARD 0, HMS_IDLE 0, float)` 0x607520, cdecl |
| 4 | vtbl+0x30 `human::update_pos_and_orient(vector*, matrix*, bool update_controller)` 0x69AB90 |
| 5 | `player_set_zoom_state(p,0,0)` 0x6851A0; `object_set_visibility_plus_children(p,true)` 0x68C070 |
| end | `rfg_prop_block::~rfg_prop_block` 0x5217D0 |

The MP-only step `rfg_prop_block::set_internal(MULTI_PLAYER_HANDLE, MULTI_TEAM)` is skipped in co-op.

- **Model.** `player_local_get_human_info_for_creation` 0x633930 picks `rfg_PC_DLC` in SP and falls back to `rfg_PC`.
  Using `Player->h_info` (+0xCC) guarantees the model is resident.
- **Team.** `human::set_team` 0x607BA0 (vtbl+0x88) writes `+0xC98`. Use guerrilla = 0.
- **Objects from the player pool are zero-initialised** (INFERRED). So `mp_last_respawn_time` (+0x1268) = 0 and the
  apply path's respawn gate passes.

### 3. Non-local `player_do_frame` path in SP (VERIFIED)
For a player that is not `Player` (handle compare at 0x6E6975–0x6E6989), the steps run in this order:
1. `player::do_frame` 0x6D5660 (thiscall). Its **only** caller is 0x6E62D7.
2. `player::check_squads` (0x6E632E).
3. Turret, opacity and vehicle checks. Camera and HUD calls are gated on `esi == Player`, for example at 0x6E6333.
4. **`multi_game_player_process_state` at 0x6E69A2 runs only if `Multiplayer==1 && multi_is_server()`.** In SP nothing
   moves the puppet unless the mod calls it.
5. Look-at from `mp_camera_orient` (fvec at +0x1250), then `human_set_look_at_target` 0x6083C0 (call at 0x6E6B30).
   There is no Multiplayer test in 0x6E69CC–0x6E6B30. The direction choice depends on the *observer's* `Rfg_camera`
   mode (0x6E6A01) and the combat timer (+0x9D4).
6. `player_process_object_interaction` 0x6DE050 at **0x6E6B36**. Only the non-local path reaches it; the local path
   uses 0x6E6E72. It is followed by weapons processing, cover and `human_do_frame(p,1)`.

**`player::do_frame` hazards.** These calls are ungated at entry, for any player:
- Backpack: Multiplayer==0, WC off, `dlc_get_current_id()==-1` and `upgrade_get_backpack_type()` != 0x3E.
  - That function takes no player argument; it reads the local player's upgrade.
  - It computes an index. If the index ≠ global `s_lastBackpackIndex` 0x01293490, it calls
    `multi_game_backpack_create`, then `event_player_picked_up(this)`, then stores the index (0x6D5674–0x6D56D7).
  - So whichever player runs first after the index changes gets the backpack.
- Also ungated: `do_frame_radiation`, `do_frame_blow_undercover`, `play_district_liberated_radio_line`,
  `hud_message_add_minor` and `player_meta_data::set_current_district`.
- `player_do_frame` also calls `player_record_single_player_stats` and `player_check_if_stuck`. Only some of these
  steps compare against `Player`.

**Item hazard.** `player_process_object_interaction` reaches `object_item_player_touch` 0x690DD0 by two routes:
- on foot through `player_process_usable_items`, call at 0x6DDBF2; it uses +0x122C / +0x1238 for non-local players;
- in a vehicle with the salvage collector upgrade, call at 0x6DE300.

`object_item_player_touch` bails out only when Multiplayer==1 && Multi_match_info!=2. Otherwise it calls
`[item_info+0xC8](player, item, …)` and then consumes the item: hidden with a respawn timestamp (0x690E79), destroyed
(0x690EBD) or detached (0x690ECC). NOP-ing 0x6E6B36 covers both routes.

### 4. Apply call: `multi_game_player_process_state(player*)` 0x5E1C50 (VERIFIED)
- **ABI.** Prologue `sub esp,0xB8; push esi; mov esi,[esp+0xC0]`; the caller pops, so it is cdecl.
- **Callers.** Only `player_multi_registration::move` 0x5F4990 (it checks type bytes +0x7E==0 and +0x7F==3) and
  `player_do_frame` 0x6E69A2 (MP server only).
- **The argument must be a real `player`**: it uses +0x1268, +0x1274 and +0xDD0, all past the 0xD90-byte `npc`.
- **Flow:**
  1. Save and clear the pflags word at +0xDD0.
  2. Return if `movement_mode` (+0x304) == HMM_PATHFINDING (2), at 0x5E1C84.
  3. `obtain_state(Clock_current_time)` on `p+0x1274` (0x568C20, thiscall). It `rep movsd`s the whole 0xAC-byte state.
  4. Return unless `mp_last_respawn_time (+0x1268) + 500 < st.network_time (+0x48)`, at 0x5E1CEC.
  5. **Stale state** (newest state older than `Max_extrap_time_ms`) and not local: only `apply_movement_state` and
     `apply_weapon_state` run, at 0x5E1D0B.
  6. Otherwise:
     - HP sync only if `Multiplayer==1 && Server==0` (0x5E1D56). It writes +0x910 / +0x914. Skipped in SP.
     - `multi_game_player_apply_vehicle_state` 0x5E1B60.
     - Turret through `network_to_game_handle::get_game_handle` (0x5E1E0E), then `human_combat_man_turret` 0x6B5E30
       or `human_combat_abandon_turret` 0x6C0730.
     - Return if `cur_state` (+0x2F4) == HS_DEAD (4).
     - Skip if `object_is_local_player` (0x614BB0 / 0x614BD0 compares only against the `Player` global).
     - Then `apply_movement_state` 0x5CF130 and `apply_weapon_state` 0x582250.
- **`add_state` 0x568B40** (thiscall, `ret 4`):
  - rejects a state when newest.time > new.time (`ja`; an equal time is accepted);
  - rejects one with `|time - Clock_current_time| > 5000 ms`, but this window check is skipped when time == 0;
  - replaces an invalid orientation matrix with identity;
  - keeps a ring of 25 (0x19) entries of 0xAC bytes.
- **`multi_sim_state<multi_player_state>`** (+0x1274, 0x10D4 B) has +0 m_states_used, +4 states[25] and +0x10D0
  m_apply_correction_timestamp. `player::player` 0x6DE880 builds it inline, unconditionally.
- **Reset** with `multi_sim_state::reset` 0x568B20. The time domain is `Clock_current_time` 0x02BEA538 (unsigned ms).
- **Tuning globals.** These are static .data defaults; only reads exist, no writes:
  - `Max_extrap_time_ms` 0x0127E4C4 = 1000
  - `Max_velocity_correction_rate` 0x0127E28C = 5.0
  - `No_sync_velocity_threshold` 0x0127E290 = 0.1
  - `No_sync_position_threshold` 0x0127E294 = 0.35
- **Not done by process_state.** `player_multi_registration::apply_compressed_state` 0x5F8DF0 does these before
  `add_state` (0x5F9935):
  - equips the weapon slot (`inv_item_get_item_in_slot` 0x5F95B6 + `player_equip_item` 0x5F95C4);
  - sets zoom (`player_set_zoom_state` 0x5F9601);
  - calls `apply_vehicle_state` at once (0x5F9433).
  The puppet driver must do the weapon switch and zoom itself.
- **SP divide-by-zero in vehicle and turret apply.**
  - `multi_game_player_apply_vehicle_state` calls `multi_core_game_handle_from_network_handle` 0x5B6380 when the
    cached game handle at state+0x9C is -1. That function reads `Reserved_network_to_game_hash` (0x1290C60: +0x44
    size, +0x40 buckets). Both are 0 in the exe and are only set by `multi_core_objects_init` 0x5B6000 (called from
    `multi_io_init`, called from `multi_init`).
  - `uint_hash` then runs `div` by 0 at 0x18FD00 (the #DE crash is INFERRED; the code path is VERIFIED).
  - process_state presets +0x94 / +0x9C to -1, but `obtain_state` overwrites them with the stored state.
  - **Fix:** in every state passed to `add_state`, write the local game handle into the cache dwords: state+0x9C for
    the vehicle and state+0x94 for the turret. Give net id 0xFFFF only for "no vehicle".
- **Other Multiplayer reads in the call tree.** These change behaviour but do not crash:
  - `apply_weapon_state` ammo (0x582298): the SP branch forces 1 round in the magazine (+0x20C) and calls
    `set_rounds_in_reserve(1)`, so the puppet never runs dry;
  - `should_pop_towards_state` 0x582205: SP pops by distance only, with no visibility check;
  - `player_update_jetpack` (0x69E0E8 / 0x69E13A / 0x69E41D): remote infinite fuel is MP-only;
  - `player_process_normal_movement` 0x685C13: rubberbanding acceleration is MP-only.
  - Deeper callees (`human_combat_man_turret`, `vehicle_interaction_request_*`, `human_jump` …) were not exhaustively
    scanned.

### 5. `apply_movement_state` 0x5CF130 (VERIFIED)
- **ABI: EAX = state, stack = player** (the PDB's cdecl is wrong). Never call it directly.
- **Aim.** Builds `mp_camera_orient` (+0x1238) from `st.camera_orient` (+0x4C) plus angular velocity (+0x70) × age. Sets
  `mp_camera_pos` (+0x122C) from the head position. One `weapons_test_bullet` ray writes `aim_target` +0xD24 and
  `aim_pos` +0xD40. The range depends on `Rfg_camera+0x800`, read at 0x5CF25E.
- **Movement input.** `forward` / `sideways` (+0x78 / +0x7C) go to `mp_current_command` (+0x117C / +0x1180).
- **Jump** (only when `!object_is_local_player`, 0x5CF408). When state bits 4–5 differ from `+0xC4` bits 9–10, it calls
  `human_jump(p,0,0,0)` 0x6C72A0 (0x5CF418–0x5CF433).
- **Ladder.** Bit 6 plus `ladder_handle` +0x84 drive `human_mount_ladder` / `ladder_process_human` /
  `human_dismount_ladder`.
- **Crouch.** Bit 30 calls `human_move_start_crouching` 0x630A80 (call at 0x5CF4F1) or `_stop_` 0x680610
  (call at 0x5CF50E).
- **Ragdoll.** `push -1; push 1; push ebp; call human_ragdoll_start` at 0x5CF546. The signature is `bool (human*, bool,
  unsigned)`. The reverse case is `human_ragdoll_get_up` (call at 0x5CF6CD).
- **When not in a vehicle (+0xAC4 == -1): `do_frame_movement` 0x5CEEA0** (ESI = player, EDI = state). It calls:
  1. `multi_game_player_correct_player_movement` at 0x5CEEC1;
  2. `player_process_normal_movement` 0x685940 at 0x5CF0CA;
  3. **`player_move_set_anim_direct(p, sideways +0x7C, forward +0x78, sprint bit3, stopped)` 0x69CFA0** at 0x5CF100.
     Its `Player` compares (0x69D122 / 0x69D168) gate only opacity, HUD and camera.

### 6. Position correction (VERIFIED, `multi_game_player_correct_player_movement` 0x5CE2C0)
- It has no direct Multiplayer read.
- It runs extrapolation and edge correction (`Mp_player_edge_correct` 0x0127E10F) and compares against the `No_sync_*`
  thresholds.
- **`human_controller_set_vel` 0x67B140 is called only in the snap branch** (0x5CE571), after `should_pop_towards_state`
  returns true. The position is then set through vtable+0x30 (0x5CE633).
- The rate-limited steering with `Max_velocity_correction_rate` runs **only for a ragdolled body** (`+0x33C == 1`,
  0x5CECB1), ending in `havok_ragdoll_set_avg_velocity` 0x5CEE54.
- Below the snap threshold, a standing human gets only position nudges: edge / block raycast and `player_correct_block`.
  Locomotion itself is input-driven through `player_move_set_anim_direct`.

### 7. `apply_weapon_state` 0x582250 (VERIFIED)
- **ABI: ESI = player, stack = (state*, bool, bool).** It writes player-only pflags at +0xDD0.
- **Reload.** A state bit 7 edge (`[st+0xA4] & 0x80`), latched in `+0xDD0 & 0x200`, calls
  `human_weapons_firearm_reload(p,1,0)` at 0x582357. It is not a +0xC4 counter.
- **Melee.** 2-bit counters (state bits 16–23) are compared with `+0xC4` bits 1–8 (0x5823AC–0x58240E). A change calls
  `human_melee_attack(p, type 0..3, 1, running)` 0x6B1760 (call at 0x58246E). It needs a melee weapon, or type 3.
- **Pull trigger.** `(st>>0xA) ^ C4 & 0xC000` (0x5824D7–0x5824E8) calls `human_weapons_firearm_pull_trigger` 0x6CC870
  at 0x582645. That function is gated by `gameplay_can_player_attack` (0x6CC896), which reads local SP state.
- **Release trigger.** It compares the up id with the down id **inside the same state** (`((st>>2)^st) & 0x3000000`,
  0x5824EB–0x582500), then calls `human_weapons_firearm_release_trigger` 0x65A690 at 0x58268E.
- **Throw.** For weapon class 2, `(st>>0xA) ^ C4 & 0xC0000` calls `human_weapons_start_throw(p, weapon, 0, 0)` 0x6CB100
  at 0x5826B2.
- **Its own +0xC4 writes** are only at:
  - 0x582393 (bit 22);
  - 0x582585 (bits 14–15, class-1 branch only);
  - 0x5826D5 (bits 20–21);
  - 0x582732 (bit 24);
  - 0x58274C (bit 23).
  It never writes the melee ids.
- **Who advances the receiver's +0xC4 counters:**

  | event | advanced in SP? | where |
  |---|---|---|
  | jump | yes, unconditionally | `human_jump` 0x6C75DA–0x6C75FE |
  | throw | yes, unconditionally | `human_weapons_start_throw` 0x6CBC41–0x6CBC63 and 0x6CBCDE–0x6CBCFE |
  | melee ids | **MP only** | `human_melee_attack` 0x6B19DB–0x6B1A42, behind `cmp Multiplayer,0` at 0x6B19C6 |
  | trigger-down bits 14–17 | **MP only** | `human_weapons_firearm_pull_trigger_common`, 0x6CC4AA–0x6CC4D6, behind 0x6CC4A1 |
  | trigger release | no +0xC4 write at all | `human_weapons_firearm_release_trigger` |

  **So in SP, melee and trigger-pull re-fire on every apply until something writes the counters.**
- **Melee swing direction in SP.** It comes from the observer's `Rfg_camera+0x8C` (0x6B1AC6), not from the puppet's
  `mp_camera_orient`; that path is MP-only (0x6B1A89).
- **Ragdoll immunity in SP.** `human_ragdoll_start` 0x6C96E0 skips its force check when Multiplayer==0 (0x6C9726) and
  always consults `human_is_immune_to_ragdoll` 0x62E6B0. That function returns immune for:
  - a backpack ability (0x62E705);
  - `cur_state == 0xD` (0x62E71C);
  - `+0xC0 & 0x20000000` in SP with WC off (0x62E725–0x62E741).
- **Weapons fire real shots.** `weapon::fire` uses the owner player's `aim_pos` (+0xD40) and fires real bullets and
  projectiles (report F7). A puppet's attacks damage the observer's world.
- **Charge detonation.** `detonate_charges(unsigned player_handle)` 0x6C51E0 (cdecl); mid-air:
  `detonate_in_air(unsigned)` 0x6E4DE0 (report F7, not contested).

### 8. Vehicles and turrets (VERIFIED)
- `multi_game_player_apply_vehicle_state` 0x5E1B60 reads `network_to_game_handle` at state+0x98 (+0 u16 net id,
  +4 cached game handle).
  - Net id == 0xFFFF and the player is in a vehicle: `vehicle_interaction_request_exit` at 0x5E1B95.
  - Otherwise it uses the cache (+0x9C). If the cache is -1 it looks the handle up (crashes in SP, §4).
- **Enter.** Requires a changed vehicle, the player not dead, `[obj+0x7E]==3` (vehicle) and `[obj+0x350C]` (loaded).
  Then it calls `vehicle_interaction_request_enter` (0x7B7E10, a jmp thunk to `_internal` 0x7B7880, which has no
  Multiplayer test) with `vi_enter_data{seat=st.vehicle_seat +0xA0, method=-1, dest=0, flags=1}`. The `vi_enter_data`
  layout is +0 seat, +4 method, +8 dest, +0xC flags (0x10 bytes).
- **Turret.** `st.turret_handle` +0x90 (cache at +0x94) feeds man / abandon turret.

### 9. Wire-relevant layouts (VERIFIED, PDB)
**`multi_player_state` (0xAC):**
- +0 position, +0xC linear_velocity, +0x18 angular_velocity, +0x24 orientation, +0x48 network_time;
- +0x4C camera_orient, +0x70 camera_angular_velocity (vector2);
- +0x78 forward, +0x7C sideways, +0x80 hit_points_pct;
- +0x84 ladder_handle, +0x88 ragdoll_state, +0x8C knockdown_timestamp;
- +0x90 turret handle (8 B), +0x98 vehicle handle (8 B), +0xA0 vehicle_seat u8, +0xA2 ladder_state;
- +0xA8 / +0xAA magazine / reserve ammo (u16).

**+0xA4 bitfield:**

| bits | field | bits | field |
|---|---|---|---|
| 0–2 | equip_weapon_slot | 11 | in_air_detonate |
| 3 | do_sprint | 12 | running_attack |
| 4–5 | jump_id | 13 | detonated_charges |
| 6 | is_on_ladder | 14–15 | charge_weapon |
| 7 | reload | 16–23 | melee primary / secondary / tertiary / firearm ids |
| 8 | jetpack | 24–25 | trigger_down_id |
| 9 | is_zoomed | 26–27 | trigger_up_id |
| 10 | dont_deform_bones | 28–29 | throw_attack_id |
| | | 30 | is_crouched |

**`human_mp_flags` (human+0xC4):**

| bits | field | bits | field |
|---|---|---|---|
| 0 | is_multiplayer | 18–19 | throw |
| 1–8 | melee ids | 20–21 | charge |
| 9–10 | jump_id | 22 | running_attack |
| 11 | vehicle | 23 | in_air_detonate |
| 12 | turret | 24 | detonated |
| 13 | swap_weapon | 25 | jetpack |
| 14–17 | trigger ids | | |

**`player` fields:**
- +0x1178 `mp_current_command`, +0x122C `mp_camera_pos`, +0x1238 `mp_camera_orient` (fvec at +0x1250),
  +0x125C cam angular velocity;
- +0x1264 `mp_desired_weapon_slot`, +0x1268 `mp_last_respawn_time`, +0x1274 `mp_sim_state`;
- +0xDCC / +0xDD0 pflags, +0x23A0 ladder mount timestamp.

**`human` fields:**
- +0xB4 / +0xB8 / +0xC0 hflags, +0xCC h_info, +0x270 actual_velocity;
- +0x2F4 cur_state, +0x304 movement_mode, +0x32C char_inst, +0x33C ragdoll_state;
- +0x910 / +0x914 max / current HP, +0xAC4 vehicle handle, +0xACC seat, +0xC98 team, +0xC9C disguise team.

**`object` fields:** +0x54 object_flags (bit 4 destroyed, bit 5 no_save), +0x6C handle, +0x7E type, +0x7F subtype
(3 = player).

### 10. Damage and death (VERIFIED)
- **`human_damage` 0x6CE950.** Prologue `83 EC 0C 56 8B 74 24 14` (sub esp,0xC; push esi; mov esi,[esp+0x14]): 8 bytes
  with no relative operands, so it can be detoured cleanly.
- **Difficulty scaling.**
  - Victim is local: `difficulty_get_value(7,5)` plus `upgrade_get_player_armor`.
  - A local Player exists and the victim is not friendly to it (`local_player_is_friendly`): `difficulty_get_value(0,5)`.
  - The branch is at 0x6CEAC3–0x6CEB1C. A guerrilla puppet gets no scaling.
- **`damaged_by_player` (+0xB4 |= 0x40000, at 0x6CEB37)** is set only when the attacker is a human of subtype 3
  (`+0x7E==0 && +0x7F==3`) **and** the damage is above a threshold constant. A player attacking from a vehicle does not
  count.
- **`player_is_killed` 0x634360** (only caller: `human_do_frame` 0x6E59E7).
  - It sets a 4000 ms timestamp on +0x1118 for any player.
  - It calls `local_player_is_killed` only when `p == Player` (0x634393). So there is no death UI for puppets.
- **`human_died` 0x6C7890 runs for every human** (called from `human_damage` 0x6CF078). Side effects on a player-class
  victim:
  - `stats_register_human_death` 0x3D4F30 uses `object_is_player` (subtype only), so it increments local stats
    0x50–0x53 / 0x56 / 0x57 and may call `LocalGameSession::updateHeroStatAdd`. **The puppet's deaths count in the
    local career stats.**
  - `player_update_jetpack` at 0x6C7B0C.
  - `gem_post_event` (0x6C8059) when the killer is player-class.
- `gfm_control_morale_human_killed` skips player-class victims (0x3AB658). `alm_report_human_death` and
  `activity_register_death` fire only for non-player victims. So a player-class puppet avoids morale and activity hits
  but not the stats hit.

### 11. Local AI versus a puppet (VERIFIED for the code; runtime INFERRED)
- **Noticing.** `ai_perception::notice_humans` 0x2C2870 loops over every human (World+0x74 / +0x7C) on **every call**.
  - HOSTILE (0) targets, or the target whose handle equals the observer's +0x974, go straight to `notice_enemy`
    (0x2C2ACC → 0x2C2B33), with no timer.
  - The 1–2 s full-scan timer (+0x14) or the is-local-Player gate (0x2C2ADD–0x2C2AE8) holds back only ALLIED, FRIENDLY
    and UNFRIENDLY targets. UNFRIENDLY also passes a second gate at 0x2C2B0D.
- **Disposition.** `get_effective_disposition` 0x265A40 always applies `human_teams_get_disposition(me,target)` 0x60D1D0
  (team +0xC98 → `Current_human_team_dispositions` 0x02C22820). Only one case is local-only: the friendly-fire rule
  that turns ALLIED into HOSTILE (`object_is_local_player` at 0x265A49). These apply to **every** target, puppets
  included:
  - HOSTILE becomes UNFRIENDLY (0x265AB4–0x265AE6);
  - the disguise team +0xC9C / `treat_as_undercover` (0x265AF7).
  - Enum values: HOSTILE 0, UNFRIENDLY 1, NONE 2, FRIENDLY 3, ALLIED 4. Teams: GUERILLA 0, EDF 1, CIVILIAN 2,
    MARAUDER 3.
- **Attack limiter.** `can_engage_target_reason` (thiscall) calls `player_attack_limiter_request` 0x240220 at 0x268C4C
  only when all of these hold:
  - Multiplayer==0 (0x268BE3);
  - the attacker is ALLIED to EDF (0x268BFC–0x268C0E);
  - the target is the local `Player` (0x268C11);
  - the `vrail_get_patrol_vehicle` exception does not apply.
  **A puppet is never limited.**

### 12. NPC puppet comparison (VERIFIED parts)
- `npc` is 0xD90 bytes with `+0xD80 ai_system*`. Running the MP apply code on an NPC writes past the struct: +0xDD0
  (0x5E1C66 / 0x5E1C93 / 0x5E1DE1), +0x1274, +0x122C, +0x1238 and +0x23A0. That corrupts memory.
- `npc_process_all` 0x6E6140 calls `world::destroy_object` on any live NPC whose position fails
  `world::is_zone_streamed` (0x6E618D–0x6E61AC). Only zone state 2 counts as streamed.
- **The AI can be turned off without stopping the body.** Set `ai_system_flags.yield_next_frame` (ai+0x451 bit 1)
  before `npc_process_all` every frame:
  - `ai_system::do_frame_pre` 0x2B52C0 skips its work (test at 0x2B52EA);
  - `ai_system::do_frame` 0x2C35B0 clears the bit and returns (0x2C35E8–0x2C35FE);
  - `human_do_frame` still runs, because hflags+0xB8 bit 0 is set first (0x6E61D6).
- NPCs cannot use charges, mines or propane, and launchers animate properly only on player anim sets.
- **Conclusion: a player-class puppet still wins.** It keeps the Player anim set, charges and the MP apply path.

---

## Approach for co-op

1. **Create the puppet** on the sim thread after `Player` exists, using the §2 recipe with `Player->h_info`.
   - Set team 0 and `object_flags.no_save` (+0x54 |= 0x20).
   - Reset the sim state (0x568B20) and set `+0x1268 = 0`.
   - Verify the puppet is not `World.subtype_objects[3][0]`.
   - Do **not** set `+0xC0` bit 29: in SP it would block replicated ragdolls.
   - Equip it with `human_add_item_to_inventory` 0x6B52A0 and `human_equip_inventory_item` 0x6BF410. These come from
     the report and were not checked by a skeptic.
   - Recreate the puppet after every level load and owner respawn, and around saves.
2. **Patch three places**, with byte checks:
   - **0x6E62D7** `E8 rel32` → a thunk that skips `player::do_frame` (thiscall, ECX = player) for puppets;
   - **0x6E6B36**: NOP the 5-byte call (only non-local players reach it);
   - **`human_damage` detour** with an 8-byte trampoline.
3. **Receive.**
   - Expand the snapshot into a `multi_player_state`, with `network_time = Clock_current_time + interp delay`.
   - Write the local vehicle and turret game handles into the cache dwords (+0x9C / +0x94). Use net id 0xFFFF for
     "none".
   - Call `add_state(p+0x1274, &st)`.
   - Weapon switch and zoom arrive as reliable events, applied with `player_equip_item` and `player_set_zoom_state`
     as `apply_compressed_state` does, but when Clock reaches their state's `network_time`, not on receive. Applied
     on receive they lead the replayed movement and action ids by the interp delay (`process_state` applies a state
     only once Clock reaches its time). Retry a switch `player_equip_item` refused (it returns false while the
     puppet is HS_LADDER, 0x6C2A23). The ghost does this (ghost_impl.md, "Delivery").
4. **Drive.** Every sim frame, before the original `gamestate_gp_process` (MP runs `multi_process` before
   `gameseq_process`):
   - call `multi_game_player_process_state(p)`;
   - then copy the latest state's melee ids (bits 16–23) and trigger-down id (bits 24–25) into `p+0xC4` bits 1–8 and
     14–15.
   The alternative is patching the Multiplayer tests at 0x6B19C6 and 0x6CC4A1 for puppets only. Seed `p+0xC4` from the
   first snapshot to avoid a spurious jump or swing.
5. **Streaming gate.** When the network position fails `world::is_zone_streamed` (0x52E020, thiscall on `&World`),
   hide the puppet, skip `process_state` and pin its position. This comes from the report; `player_process_all` has no
   streamed check of its own.
6. **Damage.**
   - Victim is the puppet: swallow the damage (return 0) and forward it reliably to the owner. The owner then calls
     `human_damage(Player, …)` so its armour, difficulty and never_die apply.
   - Attacker is the puppet on a non-authoritative peer: return 0.
   - Rate-limit forwarded damage, because the attack limiter does not cover puppets.
7. **Death.**
   - Play it with `human_damage(p, NULL, -1, …)` (report R2). **But** the detour must keep `human_died`'s stats call
     off puppets: filter `stats_register_human_death` for victim == puppet. The alternative, ragdoll plus set_state
     without `human_died`, is not researched.
   - HP for markers: write `+0x914 = pct * +0x910` directly.
8. **Sender.**
   - Read `Player+0xC4` jump_id and throw bits directly; both are bumped unconditionally.
   - Count melee swings in a `human_melee_attack` detour filtered to `Player`.
   - Poll `human_weapons_is_trigger_pulled` 0x65A5F0 and `human_weapons_is_reloading` 0x65A380 for edges.
   - Capture locomotion from `player_move_set_anim_direct`.
   - Take the aim from the `Rfg_camera` real_orient (+0x50).
   - These come from the report; only the jump, throw and melee bump facts were skeptic-verified.

---

## Contested / refuted (do not rely on these)

- **"process_state is the whole MP apply step."** False. Weapon equip and zoom live in `apply_compressed_state`.
- **"Nothing in the apply path needs Multiplayer except HP and ammo."** It holds for direct flag reads only. Vehicle and
  turret handle resolution divides by zero in SP unless the cache dwords are filled (§4).
- **"All weapon events are rolling counters versus +0xC4."** False for reload (a bit edge latched in +0xDD0) and for
  trigger release (up id versus down id inside the state).
- **"Events converge by themselves."** False in SP for melee and trigger-down. Without writeback they re-fire on every
  apply.
- **"Velocity steering through `human_controller_set_vel` at up to `Max_velocity_correction_rate`."** False. `set_vel`
  is snap-only, and rate-limited steering is ragdoll-only.
- **"force=1 bypasses ragdoll immunity, so keep `disallow_flinches_and_ragdolls` set on puppets"** (report F13 / R1).
  False in SP. The force argument is checked only on the MP path, so setting that bit blocks replicated ragdolls.
- **"Melee swings face the puppet's aim."** In SP they face the observer's camera (`Rfg_camera+0x8C`).
- **"A remote player's death has no local consequences."** False. `human_died` adds to the local career stats
  (`stats_register_human_death`) and calls jetpack and gem events. Only the death UI is local-only.
- **"Any player-class attacker sets `damaged_by_player`."** It needs a human subtype-3 attacker (not one in a vehicle)
  and damage above a threshold.
- **"Puppets are noticed only every 1–2 s"** (report F14). Hostile puppets are noticed on every call; the timer applies
  only to non-hostile dispositions.
- **"Undercover special cases are local-player only."** HOSTILE→UNFRIENDLY and the disguise team apply to every target.
- **"Wrecking Crew proves extra players get processed in SP."** It proves only that they exist. WC skips non-`Player`
  frames.
- **"NPC AI cannot be disabled without stopping animation and physics."** False: `yield_next_frame` (ai+0x451 & 2) does
  it.
- **"The engine cannot spawn Mason as an NPC."** Unproven.
- **"Sender capture at 0x2FC841, ~50 B snapshot, ~1.6 KB/s at 20 Hz"** (claim C8). This was not checked by a skeptic.
  The call site, the cdecl ABI and the bandwidth are the investigator's reading and arithmetic. Treat them as INFERRED.
- Zero-padded addresses in the report, such as `0x00C75496`, are RVAs. Read as VAs they would land in unrelated code
  (`keen::input::fillPlatformControllerInfo`).

---

## Unknowns and the probes that settle them

| # | unknown | probe |
|---|---|---|
| P1 | Does a campaign-time player creation (outside WC and MP) run any init that assumes `Player` (HUD, minimap, `Stream_player_allocator` sizing)? Is the puppet at list index > 0? | Spawn a puppet 3 m ahead with the §2 recipe. Check: it renders as Mason; `Player` is unchanged; `World+0x3FC == 2`; the puppet's list index; 10 min of play including mission and activity start and end. |
| P2 | Does process_state animate a puppet in SP (walk / run / sprint blend, look-at, no sliding)? What does it cost per frame? | Feed a synthetic 10 m circle through `add_state` + `process_state`. |
| P3 | Full fidelity of the ghost loopback: jump, crouch, ladder, jetpack, fire / reload, hammer (with +0xC4 writeback), throw plus detonate, vehicle enter / exit with the cached handle, turret, ragdoll | Delay the local snapshot by 150 ms and feed it to a puppet. Log mismatches, and confirm that without writeback melee re-fires. |
| P4 | Does the vehicle cache fill fully avoid the hash lookup? Does `obtain_state` interpolation keep the cache dwords? | Enter a vehicle with net id 0 and the cache set; break on 0x5B6380. |
| P5 | AI behaviour: engagement latency and attackers per second on a puppet versus `Player`. Do guerrillas assist? | Puppet near an EDF patrol with the local player ignored by AI. |
| P6 | Do the patches stop item theft and the backpack transfer? | Walk the puppet over salvage and buy a backpack upgrade, with and without the 0x6E6B36 / 0x6E62D7 patches. Log `+0x1170`. |
| P7 | Streaming: does an out-of-range puppet fall or jitter? Does the hide-and-pin gate work? | Move the puppet 1 km away; log `is_zone_streamed`, Z and cost. |
| P8 | Does the world save serialise non-`Player` players (`player::serialize` vtbl+0x14 0x6DEAD0)? Is `no_save` enough? | Save with a puppet (no_save set), reload and count players. Repeat with destroy-around-save. |
| P9 | Puppet death: does the stats filter work? Any mission fail, morale change or voice line? | `human_damage(p, NULL, -1, …)` with the filter on; diff the stats 0x50–0x57. |
| P10 | Which `+0xC4` writeback is correct for weapon classes other than 1 (trigger bits 14–15 versus 14–17)? Does `gameplay_can_player_attack` ever block puppet fire? | Fire every weapon class through the loopback. |
| P11 | Do side effects of `human_do_frame` for a non-local player-class human (fall damage, fire, radiation VFX, voice) fire locally? | Observe during P3 and P7. |
| P12 | Do mission scripts and activities (`player_find_nearest_player` 0x65CAB0 users, raid regions, squads) react to a second player? Does a puppet's `damaged_by_player` raise alert or morale? | Mission domain; run an activity with a puppet inside the region. |
| P13 | The meaning of `apply_weapon_state`'s two bools (fired / charged this frame); SP keeps them 0 | Covered by P3. |
| P14 | DotB characters in the campaign (rfg_PC_DLC / Samanya): the h_info must travel in the join handshake | P1 with each character. |
