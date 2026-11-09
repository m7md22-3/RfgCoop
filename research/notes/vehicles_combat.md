# RfgCoop notes: vehicles, weapons, damage, death and respawn

Domain: what campaign co-op needs for vehicles (seats, remote drivers, turrets, aircraft, vehicle damage), combat (weapon
fire, hits, explosions, damage application, friendly fire) and player death/respawn. Distilled 2026-10-07 from
`research/findings/workflows/coop_vehicles_combat.md` (the investigator report) plus the skeptic verdicts on claims C1-C8.
Build: GOG rfg.exe, 32-bit, PDB image base 0x400000. All addresses are **RVAs** unless marked VA (VA = RVA + 0x400000;
the left column of `rfg.asm` is a VA). Tools: the PDB, disassembly and the symbol/xref tables; the xref table
columns are `target RVA, kind, site RVA, containing function`.
Tags: **[V]** = someone read the code, data or PDB. **[V-skeptic]** = also re-derived by an independent skeptic.
**[V-report]** = read by the investigator only (no skeptic pass, no contradiction found). **[I]** = reasoned, not read.
Nothing here has been run in game.

## TL;DR

- **The SP death flow kills the session and fires for any player-subtype object.** When a player's 4 s dead timer
  elapses, `gamestate_gp_simulate_frame` 0x3EDB60 terminates the mission, runs an activity death handler, or saves and
  pushes `GS_PLAYER_DEATH_OPTIONS` (Continue = world reload). It has no local-player check, so a dying sub_type-3 puppet
  triggers it on the host. Co-op must keep every player from reaching HP 0. [V-skeptic]
- **The engine has the pieces for a no-reload death:** `pflags.never_die` (+0xDCC bit 0x10) clamps HP to 1.0 in
  `human_damage` for any player-class victim, before the only SP `human_died` call. `player_respawn(player*, vector*,
  matrix*, bool)` 0x6DF5F0 respawns in place without a reload. Caveats: it wipes the inventory to the character's default
  loadout (`player_reequip`), and with never_die set kill volumes no longer kill. [V-skeptic]
- **Make the remote avatar a real `player` object (sub_type 3), properly seated.** Vehicle AI is skipped only for a
  resolved driver that is a player. With no resolved driver (puppet merely teleported or attached) the AI runs, and
  `check_for_dead_driver` resets inputs every frame. A seated player-puppet driver keeps injected inputs:
  `automobile_set_vcontrols` 0x795BD0 plus velocity correction. [V-skeptic code / I runtime]
- **Damage must be finalized on the machine whose local player is the victim (or sits in the victim vehicle)**, with
  the shooter's machine supplying attacker-side multipliers. Several modifiers key off the global `Player`; the MP bullet
  receiver does NOT recompute victim-side damage (it uses the shooter's number). [V-skeptic]
- **Puppet fire follows `mp_camera_*` only for on-foot hitscan bullets.** Projectile/thrown/melee/flame/etc. aim at
  `player::aim_pos` (+0xD40). Seat hijack ejects friendly occupants too, so seat claims need one arbiter (the host).
  [V-skeptic]

## Facts

### Death flow in the campaign [V-skeptic unless marked]

- `human_do_frame` 0x6E59B4..0x6E59E7: if `hp (+0x914) <= 0` and state (`human+0x2F4`) is not 4 (dead) or 0xB →
  `human_switch_to_dead_state`; then **only if `+0x7E == 0` and `+0x7F == 3` (OT_SUB_HUMAN_PLAYER)** →
  `player_is_killed`. An NPC-subtype (2) human never enters this path.
- `player_is_killed` 0x634360: skips if `dead_timestamp` (+0x1118, `timestamp_percent`) is already valid or
  `gameseq_get_pending_state() == GS_LOAD (2)`; otherwise sets the timer to 0xFA0 (4000 ms) for any player. Only when
  the object is the global `Player` (VA 0x3022874) does it call `local_player_is_killed` 0x634290, which tail-jumps
  (0x634359) to `mission_player_died` 0x35E9D0 → sets `Mission_pre_terminated` (RVA 0x1E28664) if
  `Mission_active_index != -1`. `local_player_is_killed` also does death cam, district morale penalty etc. [V-report]
- `gamestate_gp_simulate_frame` 0x3EDB60 loop (0x3EDB63..0x3EDDC0): count `World+0x3FC`, handles `World+0x3F4`
  (= `world::subtype_objects[3]`, the OT_SUB_HUMAN_PLAYER list; `subtype_objects` at +0x3C0, `base_array<u16>[11]`),
  resolved via `World+0x64`. Per object:
  - skip if `obj+0x54 & 0x10` (object_flags.destroyed) (0x3EDBA0);
  - needs `+0x1118` valid and elapsed (0x3EDBAA..0x3EDBCA); once-latch `+0xB8 bit 0x8` (0x3EDBD0..0x3EDBE1);
  - `Multiplayer` check 0x3EDBE7, `wrecking_crew_is_active` 0x3EDBF4; SP branch then picks ONE outcome:
    - `mission_is_active()` → `mission_terminate(0)` (0x3EDC01..0x3EDC0C). `mission_terminate` 0x35E9F0 only sets
      `Mission_pending_termination = 1`, `Mission_terminated_by_script = arg`; actual failure is later mission
      processing [I].
    - activity active → reset `+0x1118 = -1` and run its handler: 0xB demolitions master (tail-jump at 0x3EDF58/5A, ends
      the frame early; `begin_ending_sequence`), 8/9 raid (0x3EDD2A; `declare_winner`), 0xA house arrest (0x3EDD19),
      1 riding shotgun (0x3EDC9D), 4 guerrilla express (0x3EDCBF; `do_end_processing`), 0x10 area defense (0x3EDCDE).
      These fail/end the activity; no mission failure, no reload.
    - otherwise `world::save_and_continue(World, NULL, false, true)` + `gameseq_push_state(GS_PLAYER_DEATH_OPTIONS = 16)`
      (0x3EDCE5..0x3EDD05).
  - **No `object_is_local_player` inside the SP branch**; the only one is at 0x3EDD65 in the Multiplayer==1 block.
- `player_death_options_continue_cb` 0x492FF0: Multiplayer → return. `Reload_level_on_death` (RVA 0x2132874) →
  `vdir_restore` + `world::set_pending_reload`. `mission_get_mode() == 4` (TUTORIAL) → `world::set_pending_territory_load`.
  Otherwise → `world::set_pending_load_from_memory` (0x493040). All three are world reloads.
  `Reload_level_on_death` is only read (0x3BFEF9, 0x492FF9), never written.
- `gamestate_respawn_player` 0x3BFEF0: SP path is also a reload (`set_pending_reload` / `save_and_reload` /
  `set_pending_load_from_memory`); its call at 0x3EDD52 is behind `Multiplayer == 1`. Not usable in SP.
- `human_died` 0x6C7890 has exactly two callers: `human_damage` and `multi_game_player_process_player_death` (MP). It
  does ragdoll/turret abandon, morale, stats, ALM, activity death, and `vehicle_queue_damage(v, …, 0x7FFFFFFF …)` at
  0x6C7E47 if the victim drove; it does not start the death screen. [V-report for the body]

### never_die clamp and in-place respawn [V-skeptic]

- `human_damage(human* victim, human* attacker, float dmg, damaged_by_types, weapon_info*, object*, bool, bool)` 0x6CE950,
  float in st0. `damaged_by_types`: 0 NONE, 1 CAR, 2 EXPLOSION, 3 BULLET, 4 LEVEL_OBJECT, 5 MELEE, 6 WORLD, 7 RADIATION.
  - 0x6CE9B7..0x6CE9CA: `edi` = victim only if `+0x7E == 0 && +0x7F == 3` (any player, not just local).
  - 0x6CED75 `should_kill_when_damaged` instant-kill path also flows through the subtraction at 0x6CEE50.
  - 0x6CEF85: `test byte [edi+0xDCC], 0x10` (player_flags bit4 never_die; bit5 = mission_never_die); if HP <= 0, store
    `[0x12B70E8]` (= 1.0f in .rdata) to +0x914 (0x6CEF9B). This is before the HP<=0 → `human_died` call
    (0x6CF066/0x6CF078).
  - All 26 writes to `[reg+0x914]` in the exe were mapped: outside `human_damage` the only HP decreases are MP code,
    `human_restore_health`, `npc::clear` and script setters that never call `human_died`. In SP the clamp is the only
    player death gate.
- never_die is shared with game logic. `mission_set_player_never_die`, `mission_success/failure_event_happened`,
  `activity_demolitions_master::do_activity_end`, `activity_area_defense_data::record_victory` set 0x30 only if 0x10 is
  clear; `mission_clean_invlunerable_flags`, `activity_area_defense_data::activity_end`,
  `activity_demolitions_master::warp_player_on_end`, `maybe_kill_player` clear 0x30 only if 0x20 is set. **Set 0x10 only,
  never 0x20**, and the mod's bit survives.
- never_die side effects: `trigger_region_do_frame` (VA 0xAD64EE) skips the SP kill-region `human_damage` call;
  `maybe_kill_player` 0x37F7D0 (area defense) also gates on it. Falling into void / kill volumes no longer kills.
  The clamp does not suppress knockdown/ragdoll at low HP (0x6CEEDA..0x6CEF33).
- `player_respawn(player*, vector* pos, matrix* orient, bool teleport)` 0x6DF5F0 (len 0x4BB): no world-reload call.
  `vehicle_interaction_request_exit` 0x6DF7A0; if the bool is true: `human_teleport` in SP (0x6DF7D2) /
  `human_teleport_unsafe` in MP (0x6DF805); HP = min(+0xAE0 health_restore_hit_points + max (+0x910), max), stored at
  0x6DF864; `human_ragdoll_stop` 0x6DF887; `camera_level_init_post` + `hud_reset_damage_indicators` only if it is
  `Player` (0x6DF9F1..0x6DF9FE); MP-only `multi_object_backpack::event_player_respawned` skipped in SP;
  `player_reequip` 0x6DFA6D; `+0x1118 = 0xFFFFFFFF` at 0x6DFA86.
  - Callers: only `activity_demolitions_master::warp_player_on_end` (0x397B65, proves SP use) and the wrapper
    `player_respawn(player*, int, …)` 0x6DFAB0, which **returns early unless Multiplayer or Wrecking Crew**.
  - `player_reequip` 0x6DF450: holsters, deletes every item in the inventory list +0xA2C
    (`human_delete_item_from_inventory`, 0x6DF470..0x6DF493), then `human_set_default_inventory` 0x6DAAB0 (character
    defaults; MP-only branch for MP loadouts). **The campaign loadout is lost.**
  - Shipped SP code only calls it on a living player; reviving after `human_died` is untested.
- `human_teleport(human*, vector*, matrix*, float, bool)` 0x69A3C0 [V-report signature].

### Driving a vehicle remotely [V-skeptic unless marked]

- `automobile_do_frame_presim` 0x79BDA0: whole block gated by `+0x4198 == 1` (`a_rdd.m_loaded`) at 0x79BE1E.
  - Driver (0x79BFB2..0x79BFE5) = `object_from_handle(most_recent_driver +0x3F40)` if `most_recent_driver_exit_time`
    (+0x3F48) is not valid and the object is a human (+0x7E == 0), else `vehicle_passenger_entering_seat(v, 0)` 0x7777F0.
  - 0x79C227 `test edi,edi; je` → **driver NULL runs `vehicle_ai_do_frame` 0x7D0B70**; otherwise AI is skipped if
    `object_is_local_player` 0x614BB0 or `object_is_player` 0x614B60 (`[obj+0x7F] == 3`) (0x79C22F..0x79C24C).
  - `vehicle_ai_do_frame` = `vehicle_ai_process_u_turn`, `check_for_dead_driver` 0x7C8EA0, `vehicle_ai_check_extra_mass`,
    `intersection_controller_process_vehicle`. Only `check_for_dead_driver` writes inputs: it calls
    `vehicle_set_controls_when_abandoned` 0x78D0A0 (0x7C8EF8) **every frame** when `vehicle_passenger_in_seat(v,0)` is
    NULL or the occupant's vfunc+0x10 is true (probably "is dead" [I]).
  - MP block 0x79BDBA..0x79BE16 is skipped when Multiplayer == 0.
  - Flyers (sub_type 5) call `vehicle_ai_do_frame` unconditionally from `vehicle_do_frame_presim` (0x7BB531).
- Input writers. `hk_custom_vehicle_instance::set_forward_or_reverse_accelerator` 0x789A90 callers:
  `set_inputs_due_to_crash`, `vehicle_freeze`, `automobile_blow_up`, `automobile_clear_accelerator` / `set_accelerator` /
  `set_brake_reverse` / `set_forward_or_reverse_accelerator` / `set_vcontrols`, `automobile::stream`.
  `set_steering_wheel_input` 0x7742F0 callers: `automobile_set_steering`, `automobile_set_controls_when_abandoned` (x3),
  `set_inputs_due_to_crash`, `automobile_steer`, `automobile::stream`.
  `vehicle_set_controls_when_abandoned` callers: ~28 AI actions, `check_for_dead_driver`, and
  **`activity_convoy_data::target_vehicle_delivered`** (0x36BC34, from `activity_do_frame_active_capture`).
  Higher-level setters (`automobile_set_steering`, `_set_forward_or_reverse_accelerator` 0x795B30,
  `_set_brake_pedal_input` 0x7727F0) are called by AI actions / `vehicle_ai_*`, `control_gameplay_process`
  (0x301EDD/0x302008) and MP `apply_sim_state` (0x59E2A4). No hidden per-frame reset of steering field +0xCC (checked;
  accelerator fields +0xC4/+0xC8 not checked).
- `set_steering_wheel_input` 0x7742F0..0x774326 is gated: a timestamp at +0xD4 (crash window) skips the store, a bit
  in +0xE0 forces 0. Steering stored at +0xCC.
- `control_gameplay_process` 0x300B90 starts with `player_get_local_player` (0x300BBA) and drives only the local
  player's vehicle (`player+0xAC4`); `automobile_steer` 0x301D03, `flyer_steer` 0x301D0F, `walker_steer` 0x301D3C.
  [V-report for the masks `+0x24A0` / `+0x24AC`]
- `automobile_set_vcontrols(unsigned handle, vehicle_controls*)` 0x795BD0..0x795C8A: checks obj_type 3, sub_type 4,
  `+0x4198 == 1`; bit2 → `automobile_honk_horn`; bit1 → `set_handbrake_on`; accelerate `[c+4]` negated if bit0 →
  `set_forward_or_reverse_accelerator`; `[c+8]` → `automobile_steer` 0x7959C0. No callers in the exe.
  `vehicle_controls` (0xC): +0 bits (brake_reverse 0, handbrake 1, honk 2), +4 float accelerate, +8 float steering.
- `automobile_set_vcontrols`, `set_ai_controlled` 0x7637C0 and `vehicle_set_driver_inputs_due_to_crash` have no code
  callers.
- Despawn: `vehicle_do_frame_presim` 0x7BB190 (0x7BB1C5..0x7BB1F5) calls `world::destroy_object` on a vehicle in an
  unstreamed zone unless `player_occupied_vehicle` 0x65CA50 (loops the World+0x3F4 player list comparing
  `player+0xAC4` with the vehicle handle) or `vehicle_should_avoid_despawning`. **The puppet's +0xAC4 must be set.**
- Other helpers [V-report signatures]: `automobile_steer(automobile*, float h, float v)` 0x7959C0,
  `automobile_set_handbrake` 0x772760, `automobile_get_steering_info(unsigned, float*, float*)` 0x772950,
  `walker_steer(walker*, float, float)` 0x785440, `havok_body_set_pos_orient` 0x423790 /
  `_set_linear_velocity` 0x423870 / `_set_angular_velocity` 0x4239A0 (by havok handle, vehicle +0x4C).

### The engine's MP vehicle replay [V-skeptic]

- `multi_game_object_vehicle_automobile_apply_sim_state(multi_automobile_state*, automobile*)` 0x59D9B0, cdecl. Soft
  keyframe via `hkpKeyFrameUtility::applySoftKeyFrame` (limits `Mp_max_lv/av/acc`, `Mp_vehicle_factor`), then replays
  steering correction / accelerator / brake / handbrake. Its MP-named helper `multi_game_player_can_view_object`
  0x55FA50 uses only `player_get_local_player` and `Rfg_camera`.
- It runs from `automobile_do_frame_presim` only when `Multiplayer == 1`, `multi_is_server()`, and
  `multi_game_object_vehicle_automobile_is_client_authoritative` (tests `automobile+0x4E88 mp_sim_state != 0`, VA
  0x99D94A). `get_steering_correction` 0x59D320 NULL-guards +0x4E88 (`test ecx,ecx; je` at 0x59D342/0x59D344).
  **No NULL dereference; the path is just never taken.**
- `+0x4E88` is only allocated by `automobile_multi_registration::create` (0x59D49E) / `process_create_packet`
  (0x59D8DA). Automobile, walker and turret registrations are orphaned: no absolute pointer to create/move/
  get_compressed_state/apply_compressed_state/send_create/process_create in the exe, no call/jmp xref, no vftable, no
  `init<>`. Item, player, projectile, rfg_mover have vftables (0xF0ADBC/0xF0AE7C/0xF0AEDC/0xF0AF3C).
- Walker/turret equivalents: `multi_game_object_vehicle_walker_apply_sim_state` 0x580B20,
  `multi_game_object_turret_apply_sim_state` 0x5800C0 (writes +0x10C, calls `weapon::fire` on `turret::get_weapon_inst`
  0x641C20). [V-report]

### Seats, hijack, turrets [V-skeptic unless marked]

- MP seat recipe, `multi_game_player_apply_vehicle_state` 0x5E1B60: enter via `vehicle_interaction_request_enter`
  (0x5E1C3B) with `vi_enter_data {specified_seat = state+0xA0, seat_selection_method = 0 SPECIFIED, destination_seat = -1,
  flags = 0x01 teleport}`, only when the target is obj_type 3, `veh+0x350C` (rdd.m_loaded) != 0 and `human+0x2F4 != 4`.
  Exit: `vehicle_interaction_request_exit` (0x5E1B95) with `vi_exit_data` flags = 1 (teleport).
  `vi_enter_data` flags: bit0 teleport, walk, bit2 minimal_processing, bit3 reserve_seat_only.
- `vehicle_interaction_request_enter_internal` 0x7B7880: `vi_seat_reserve` 0x7A1D90 at 0x7B7AD5; on failure it retries
  with `destination_seat` if != -1, else `vi_request_exit` + return false (0x7B7AE1..0x7B7AFF). Writes `human+0xAC4`
  at 0x7B7B69 [V-report]. The `human_teams_are_unfriendly` → `vi_request_exit` loop (0x7B7BBA..0x7B7BF8) only targets
  humans still entering/reserving (logic state 1 or 2, or anim state 1); skipped with minimal_processing (0x7B7B6F).
- **Real hijack**: `mark_seat_reserved(vi_instance*, unsigned, vehicle_seat_index, vvm_waypoint_blocked_type)` 0x7A19B0.
  If the requester passes `object_is_player` (0x7A1A31), every other human in the vehicle is marked `being_hijacked`
  (`vi_human_manager+0x65` bit 0x40) and ejected (`vi_request_npc_exit` 0x7A1B7E or forced dive), sparing only NPCs for
  which `npc_is_defending_player` (0x7A1B5A). **No team check: a player-puppet can eject a friendly player.**
- `vi_human_manager` (0x68): handle +0, interaction +4, state (`vi_state_of_logic`) +0x18, state_of_anim +0x40, seat
  +0x4C, enter_data +0x54, exit_data +0x64, being_hijacked +0x65 bit6. Seat indices: 0 driver, 1..10 passengers,
  254 best, 255 invalid. [V-report for the full layout]
- `set_enter_complete` 0x7B4E20: the turret to man comes from `vehicle_get_seat_turret_mount` (0x7B4FBB);
  `vehicle_get_seat_turret` at 0x7B5163 only sets a per-seat state to 5. Driver seat (0) mans only a turret with no
  operator; other seats make the current operator abandon it (0x7B512E..0x7B5149). Requires `turret::is_functional`
  (0x7B51A3) → `human_combat_man_turret` 0x6B5E30 (0x7B51B2). Driver path gated by `Player_vehicle_turret_override`
  (RVA 0x12B9510) uses the turret list at +0x3D58.
- `multi_game_player_process_state` 0x5E1E07..0x5E1E3B also mans/abandons from the network turret handle.
- `turret::aim_turret(vector*)` 0x672760 (thiscall, ret 4): NPC operator (+0xA0) → always writes `target_pos` +0x10C
  (0x672791). Player operator or none → writes (0x6727C7) only if `turret::should_autoaim` 0x672240 is false.
  `should_autoaim` true for `t_info+0xA0 & 0x80` and for a player in seat 0 of a non-tank vehicle (driver guns).
- `camera_aim_turret` 0x2C8BF0 starts with `player_get_local_player` → local only.

### Weapon fire and aim [V-skeptic unless marked]

- `weapon::get_adjusted_muzzle_transform(vector*, vector*, vector*)` 0x7EA730 (callers: `weapon::fire` 0x7EB1D6,
  `weapon::process_repair`). Owner (`item+0x13C`) counts only if `+0x7E == 0 && +0x7F == 3`; NPC owners → raw muzzle
  (0x7EAE71).
  - Camera path needs: `Fire_bullets_from_camera` (RVA 0x12EAC7A, initialised 0x01, only read), blindfire timer
    `human+0x9DC` elapsed/invalid, `w_info (+0x148)->+0x108 ammo_type == 0` (BULLET), `weapon+0x14C turret_handle == -1`.
  - Then: local player → `Rfg_camera+0x2C` / `+0x68`; other players → `player+0x122C mp_camera_pos` and
    `copy_normalize_safe(player+0x1250)` (= `mp_camera_orient` (+0x1238) fvec). Origin is the camera pos pushed forward
    to the shooter's horizontal distance (0x7EA840..0x7EA98D), not the muzzle.
  - Non-bullet ammo, no turret (0x7EA9F0): direction = `player+0xD40 aim_pos` − muzzle, for every player.
  - Turret with non-local shooter → raw muzzle (0x7EAE86).
- `mp_camera_*` writers: `process_player_normal/cover/driving/ladder_movement` (gated on Multiplayer, e.g. VA 0x6FC595),
  `control_spectator_process`, `player::serialize`. In SP nothing overwrites values the mod writes.
- `weapon::fire(vector)` 0x7EAEB0 (thiscall, ret 0xC) uses `fl_rand_range` / `rand_around_dir` for spread.
  Trigger API used by MP `apply_weapon_state` 0x582250: `human_weapons_firearm_pull_trigger(human*)` 0x6CC870,
  `_release_trigger` 0x65A690, `human_weapons_start_throw(human*, weapon*, vector*, bool)` 0x6CB100,
  `human_melee_attack(human*, types, bool, bool)` 0x6B1760. [V-report]

### Damage pipeline [V-skeptic unless marked]

- `human_damage` modifiers (SP, not Wrecking Crew, gate 0x6CEA7F/0x6CEA8C): victim is local player → `*=
  difficulty_get_value(7,5)` (0x6CEAC4/0x6CEAD4) then `upgrade_get_player_armor` (0x6CEB13); else if `Player != 0` and
  `!local_player_is_friendly(victim)` → `*= difficulty_get_value(0,5)` (0x6CEAF0..0x6CEB18). Friendly humans are not
  scaled.
- `human_in_players_vehicle` 0x62E640 (victim+0xAC4 == Player+0xAC4) → zero damage (0x6CED16..0x6CED26, unless dbt 7).
- Also in `human_damage` [V-report]: `hflags.invulnerable` (+0xB4 & 0x20) blocks positive damage; story NPCs immune
  outside missions; `mission_get_mode() == 8` → none; kill-credit fields in `human_combat` at human+0x928.
- `weapons_get_bullet_damage(weapon_info*, object*, float)` 0x7E1720: column `info+0x14C+4*idx`. Vehicle target
  (+0x7E == 3) → idx 2/3 via `vehicle_player_in_vehicle` 0x764D70 (= local player in that vehicle); else idx 0/1 via
  `object_is_player` (any player). Callers: only `do_bullet_hit` and `projectile::collision_damage`.
- `vehicle_apply_damage(vehicle*, object*, weapon_info*, dbt, vector*, vector*, float, int, vector*, bool)` 0x7BF870:
  early exit only on MP client (0x7BF895/0x7BF89E). 0x7BFA15 `player_get_local_player`: if the local player is not in
  this vehicle (and attacker absent or not NPC) → `*= Damage_multiplier_npc_vehicle`. Local player in vehicle →
  `Vehicle_player_damage_factor` (0x7BFBA8), `difficulty_get_value(0xC,5)` (0x7BFBF2), `stats_cheat_enabled(2)`
  (0x7BFC0B). Occupant god mode uses `human::is_invulnerable` of the local player (0x7BFF5F..0x7BFF88).
  `m_invulnerable` = +0x3F24 & 0x800000 [V-report].
- `do_bullet_hit(human* shooter, weapon_fire_info*)` 0x7E5610: computes damage on the **shooter**
  (`weapons_get_bullet_damage` 0x7E5744), SP-only `Sequential_damage_factor` using victim +0xDB4 (0x7E575B..0x7E5818),
  then `weapons_create_bullet_hit_effects` 0x7DB9F0 (0x7E583F) and `weapons_process_bullet_hit` 0x7DD240 (0x7E5855); in MP
  a local shooter sends `multi_gw_send_bullet_hit` (0x7E588B).
- `weapons_process_bullet_hit` 0x7DD240: if `object_is_player(attacker)` → `*= upgrade_get_nano_enhancer`
  (0x7DD2EE.., 0x7DD3F2..), which reads the **local** `player_get_meta_data` upgrades. Then victim vtable+0x68
  `process_bullet_hit` (0x7DD53C). Multiplayer checks only skip MP branches.
- `human::process_bullet_hit` 0x6D3390: both humans sub_type 2 → `Damage_multiplier_npc_onscreen` / `_offscreen` chosen
  by a vtable+0x84 visibility test from the local camera (0x6D339C..0x6D3433). [V-report]: then
  `Damage_multiplier_npc_reduced`; SP friendly fire → `Damage_multiplier_player_damage_friendly` (0x1293330, 0.30) if
  attacker is a player, else `Damage_multiplier_friendly_fire` (0x129332C, 0.50); a player victim seated in a vehicle
  returns before damage (bullets hit the vehicle).
- `human_combat_unified_melee_hit(object*, human*, vector*, vector*, vector*, weapon_info*, human_melee_combat_move_data*,
  unsigned, bool, bool)` 0x6D0F90: `human_combat_get_melee_damage` 0x6D1193, local `stats_cheat_enabled` →
  `Super_hammer_*` (0x6D11E2..0x6D1241), victim vtable+0x6C (0x6D13AF). Multiplayer checks only gate the MP send.
- MP receivers (all SP-callable internals): `multi_gw_process_bullet_hit` 0x5D3A80 (`validate_attack_hit` 0x5D3F12 only
  under Multiplayer, effects 0x5D3F7F, process 0x5D3F9B, relay 0x5D3FE5; **never calls `weapons_get_bullet_damage`**);
  `multi_gw_process_melee_hit` 0x5D52B0 (`human_combat_move_data_from_index` 0x5D57B9 → melee hit 0x5D57EB);
  `multi_gw_process_explosion_create` 0x2F01F0 → `explosion_create` (0x2F03D6); `multi_gw_process_explosion_hit` 0x5D5D40
  → `human_combat_process_explosion_hit` (0x5D6084); `multi_game_player_process_request_damage` 0x5D1330 → `human_damage`
  (0x5D13D2) but starts with MP-only `multi_game_data_get_player_instance` → **call `human_damage` directly**.
- `explosion_create(explosion_info*, object*, object*, vector*, matrix*, vector*, weapon_info*, bool)` 0x2EC640: deferred
  via a pool (`explosion_do_frame` 0x2F0080); the last bool matters only on an MP client. Damage sinks
  `explosion_apply_damage` 0x2EF770, `explosion_process_humans` 0x2EC850, `vehicle_process_explosion_hit` 0x7C26E0,
  `explosion_get_damage_and_impulse` 0x2E7600 (uses `vehicle_player_in_vehicle`). [V-report]
- `projectile::detonate(vector*, vector*, object*)` 0x6D77D0 (thiscall) calls `human_damage`, `explosion_create`,
  `world::destroy_object`. Projectile: `mp_create_id` +0x14C, `mp_sim_state` +0x150. [V-report]
- `gfm_control_morale_human_killed` 0x3AB630 credits kills by `object_is_player(killer)` (any player).
  `gameplay_register_player_attack` 0x374710 is a no-op in SP. [V-report]
- Unregistered non-MP callbacks `bullet_hit_event_process_callback` 0x586ED0 / `melee_hit_event_process_callback`
  0x587000 call the same replay functions (no registration xref found).

### Vehicle death and damage queue [V-report only: claim C8 got no skeptic pass]

- `vehicle_blow_up(vehicle*, human* killer, bool, weapon_info*, bool)` 0x7B6520 uses `fl_rand_range`,
  `rand_around_dir`, `havok_body_apply_point_impulse`, `explosion_create`; kills passengers via
  `vehicle_kill_passengers` 0x7B3330 (skips invulnerable humans).
- `vehicle_mp_force_blow_up(vehicle*)` 0x7B71F0 = `vehicle_blow_up(v, obj(m_killer_handle +0x3F3C), m_flags(+0x3F24)
  bit26, m_killer_weapon +0x3F50, true)`.
- `vehicle_queue_damage` 0x765170: same signature as `vehicle_apply_damage`; locks `damage_events_cs` (+0x3EB8),
  appends a `vehicle_damage_event` (0x3C, `from_server` +0x38) to `damage_events[5]` (+0x3D88, count +0x3EB4).
  Meaning of the `int` and final `bool`, and the drain site, are unknown.

## Approach for co-op

1. **Avatar**: remote players are real `player` objects (Wrecking Crew recipe, see `replication.md`), never touching
   `Player`. Reasons: vehicle-AI skip, the never_die clamp, player bullet-damage column, `mp_camera_*` aim and morale
   credit all key on sub_type 3. Cost: the SP death flow and hijack logic also treat it as a player (handled below).
2. **Ownership**: each peer owns its avatar's movement, health and death. Vehicle movement + health owner = machine of
   the seat-0 player, else host. NPCs/world/destruction = host. Seat changes are requests to the host (hijack has no
   team check).
3. **Seats**: `vehicle_interaction_request_enter(puppet, v, &{seat, 0, -1, 0x01})`, exit with flags 1. Refuse enter
   when `vehicle_info+0x44` is NULL (crash guard). Confirm the puppet resolves as driver
   (`+0x3F40` or entering-seat) and that `puppet+0xAC4` is set.
4. **Remote driving (non-owner copy)**, every frame: `automobile_set_vcontrols(h, &controls)` from the owner's inputs,
   plus velocity correction toward the owner's interpolated transform with `havok_body_set_linear/angular_velocity`,
   snap above a threshold. Optionally try calling `multi_game_object_vehicle_automobile_apply_sim_state` directly with a
   hand-built `multi_automobile_state` (probe P11). Aircraft: state-driven velocity easing only (random hover noise).
5. **Damage**: shooter-detected hits are forwarded with attacker-side multipliers (nano enhancer, super hammer)
   normalised or included. On the victim's owner, recompute with `do_bullet_hit(puppet, &wfi)` (bullets) or call
   `human_damage` / `human_combat_unified_melee_hit` directly. Hook `human_damage` 0x6CE950 for puppet victims: forward
   and return 0. Hook `vehicle_apply_damage` / `vehicle_queue_damage` on non-owners: forward, the owner re-injects.
   Cosmetic copies of puppet fire must not deal damage (hook `do_bullet_hit`, `projectile::detonate`, melee hit).
6. **Puppet aim**: write `+0x122C` / `+0x1238` (bullets on foot) **and** `+0xD40 aim_pos` (everything else) each frame;
   drive triggers with `human_weapons_firearm_pull/release_trigger`. Remote gunner: `turret::aim_turret` every frame
   (not effective on autoaim turrets); visible turret fire via `weapon::fire` on `turret::get_weapon_inst`, damage
   suppressed.
7. **Death**: keep `pflags |= 0x10` (never 0x20) on both local players and all puppets for the whole session. Post-hook
   `human_damage` on the local player to detect "HP clamped to 1" → co-op downed state (ragdoll, input block).
   Revive = `human_ragdoll_stop` + heal. Respawn = snapshot inventory, `player_respawn(p, pos, orient, true)`, restore
   inventory. Handle out-of-world yourself (kill volumes no longer kill). Total failure (both down / host chooses fail):
   host clears 0x10 and lets the SP flow run, client follows with the same reload (session domain).
8. **Puppet vehicle wreck**: outcome from the vehicle owner as an event (random impulses); non-owners set killer
   fields and call `vehicle_mp_force_blow_up`, then snap the wreck (pending C8 verification).

## Contested / refuted (do not rely on)

- **"Hijack only ejects unfriendly occupants"** (report §1.4): REFUTED. That loop only hits competing entrants;
  seated riders are ejected by `mark_seat_reserved` for any player requester, with no team check.
- **"Entering a seat with a turret always mans it"**: conditional (`is_functional`, existing operator, seat 0 rule);
  turret comes from `vehicle_get_seat_turret_mount`, not `vehicle_get_seat_turret`.
- **"MP vehicle replay dereferences NULL +0x4E88"**: wrong; it is NULL-guarded and gated. "Not callable in SP" is
  unproven; direct call looks possible [I].
- **"Abandon reset is reached only from AI actions" / "inputs persist"**: incomplete. `check_for_dead_driver` (every
  frame if no valid seat-0 occupant), convoy delivery, `automobile::stream`, crash/freeze/blow-up all write inputs;
  `set_steering_wheel_input` itself can ignore or zero the value.
- **"Driver gate = `if (driver && !player)`"**: inverted for NULL; a NULL driver runs the AI.
- **"A remote player must be a player object"**: a design choice, not forced by the code; an NPC puppet would work if
  its human AI actions are suppressed. Still the recommended choice.
- **"Puppet weapon fire aims along mp_camera"**: only on-foot hitscan bullets; others use `aim_pos` +0xD40.
- **"Difficulty scale only for the local player"**: non-friendly NPC victims are scaled too (`difficulty_get_value(0,5)`).
- **"`vehicle_apply_damage` 0x7BFA15 is an invulnerability check"**: no, it is the `Damage_multiplier_npc_vehicle`
  branch; occupant god mode is at 0x7BFF5F..0x7BFF88.
- **"Replay remote bullets through the MP receiver"**: that path trusts the shooter's damage; recompute on the owner.
- **`multi_game_player_process_request_damage` as a replay entry**: MP-only data; use `human_damage`.
- **"Continue always = `set_pending_load_from_memory`"**: three branches (all reloads).
- **"Any player death fails the mission"**: only when a mission is active; activities end/fail the activity; outside
  both, the death screen + reload.
- **Report §3.4 `player_respawn(p, pos, orient, false)`**: false means no teleport; pass true to move the player.
  `player_respawn` is not "free": it resets the inventory.
- **"never_die covers killzones"** (report §4 risk 1): it disables them instead; the mod must handle out-of-world.
- **`Reload_level_on_death` default 0**: the value was not verified by the skeptic.

## Unknowns and the probes that settle them

All single-machine probes in a test DLL (`player_do_frame` hook), second `player` object via the WC
recipe.

- **P1 puppet driver**: seat a puppet in seat 0 with the MP recipe, `automobile_set_vcontrols({accel 0.6, steer 0.3})`
  each frame; log `vehicle_ai_do_frame` / `check_for_dead_driver` hits for that vehicle (expect none) and whether
  `+0x3F40` resolves to the puppet. P1b walker with `walker_steer`; P1c Raven with velocity easing (`vehicle_is_usable`
  is not called by `request_enter_internal` [V-report], so no flyer patch should be needed).
- **P2 local gunner in a puppet-driven car**: does `control_gameplay_process` leave the vehicle's steering alone when
  the local player is not in seat 0 (masks +0x24A0/+0x24AC not traced)? Does `player_switch_seat` work?
- **P3 puppet gunner**: is the turret auto-manned (`turret+0xA0 == puppet`)? Do `aim_turret` + `weapon::fire` move the
  barrel and show shots?
- **P4 puppet protection**: puppet with 0x10 (and 0x20 in `hflags +0xB4`, 0x4 at +0xBC): shoot it, blow up its car,
  during a mission and outside; expect HP >= 1, `+0x1118 == -1`, no `mission_terminate`. Does invulnerability change AI
  targeting?
- **P5 death interception**: never_die on the local player, lethal hit → HP 1, no death cam; then snapshot inventory,
  `player_respawn(p, pos, orient, true)`, restore; check camera, weapons, no `GS_PLAYER_DEATH_OPTIONS`. Variant: real
  death then `player_respawn` within 4 s, logging `Mission_pre_terminated`.
- **P6 hit replay**: `do_bullet_hit(puppet, &wfi)` against the local player (difficulty scaling applied?) and
  `weapons_create_bullet_hit_effects` + `weapons_process_bullet_hit` against an NPC (damage, retarget, kill credit).
- **P7 explosion replay**: `explosion_create(rocket_info, puppet, nullptr, pos, orient, dir, w_info, false)` near a
  building and a car: damage, destruction, credit.
- **P8 cosmetic suppression**: hooks on `do_bullet_hit` / `projectile::detonate` for puppet shooters → zero damage.
- **P9 vehicle damage queue (settles C8)**: `vehicle_queue_damage(v, …, 500, ?, nullptr, true)` then
  `vehicle_mp_force_blow_up(v)`; find the drain site, the meaning of the `int` and final `bool`.
- **P10 correction tuning**: record a drive at 30 Hz, replay on a puppet copy with 100-200 ms delay; compare input-only,
  velocity-only and combined. Also read `Mp_max_lv/av/acc`, `Mp_vehicle_factor` values.
- **P11 direct MP replay**: call `multi_game_object_vehicle_automobile_apply_sim_state` with a hand-built
  `multi_automobile_state` and compare with P10.
- Also open: runtime values of `Damage_multiplier_*` (tweak-table overrides?) and `Reload_level_on_death`; meaning of
  `check_for_dead_driver`'s vfunc+0x10; accelerator fields +0xC4/+0xC8 writers; who evaluates vehicle-vs-NPC ramming
  (`human_collide_vehicle` 0x6CF4F0); projectile identity mapping across machines (`mp_create_id` +0x14C); how
  activity-specific death handlers should behave in co-op.
