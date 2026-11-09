# RfgCoop notes: AI / NPC authority, spawners and object identity

Distilled 2026-10-07 from `findings/workflows/coop_ai.md` (investigator report) and the skeptic verdicts on its claims C1-C8.
Target: GOG rfg.exe, 32-bit, ASLR. All addresses are **RVAs** (PDB base 0x400000; disassembly shows VA = RVA + 0x400000,
e.g. `Multiplayer` shows as 0x2fea588).

Tags: **[S]** = re-derived by an independent skeptic from the disassembly, data or exe bytes. **[R]** = marked VERIFIED by
the investigator; no skeptic re-checked it, but no verdict contradicts it. **INFERRED** = reasoned, not read.
Tool note: the xref table columns are `target_rva  kind  call_site_rva  containing_function`, so "callers of
X" = `grep ^<X_RVA>`. The column order in the workflow context text was wrong.

---

## TL;DR

1. **Puppet switch exists [S].** Per-NPC AI runs only from `npc_process_all` (0x6E6140), through `ai_system::do_frame_pre`
   (0x2B52C0) and `ai_system::do_frame` (0x2C35B0). Both return early when `ai_system`+0x451 bit 0x02 (`yield_next_frame`)
   is set. Movement and animation (`human_do_frame`) keep running. The switch is not clean, though: release the trigger
   and clear the path yourself, and note that global and vehicle AI keep running.
2. **There is no single spawn chokepoint [S].** `gameplay_do_frame` (call at 0x3EDF23) and `convoy_do_frame` (0x3EDF06)
   cover most spawners. Mission scripts (`script_do_frame`), Marauder ambushes (`ambush_region_do_frame`), streaming
   callbacks and `object_spawn_do_spawn` from territory setup or mission setup and cleanup all spawn NPCs outside them. To
   stop all NPC creation on a client, block the three `*_spawn_node::spawn_child_object` functions and the ambient
   spawners, not two call sites.
3. **Identity [S].** Objects loaded from files or saves keep their stored handle, so two machines with the same files and
   the same save agree on them. Runtime objects get `gid<<16|slot` from a next-fit per-zone allocator and diverge. Byte 2
   == 0xFF marks an allocator handle, but saves restore persisted 0xFF handles too, so byte 2 alone does not mean "created
   this session". The engine has no NPC replication (no npc/human `*_multi_registration`), so the mod needs its own net-ID map.
4. **The host's world centres on one local viewpoint [S].** Spawn regions, spawn-node evaluation, despawn distance and AI
   slow-down (40 m on foot, 80 m with the NPC in a vehicle) all use the local player. Streaming uses the local camera.
   `npc_process_all` destroys any NPC in an unstreamed zone. The v1 answer is a leash; v2 is per-system patches.
5. **No determinism [S].** Two independent global RNGs are both seeded from the microsecond timer: the Vlib MT pool and the
   CRT `rand()`, which is advanced once per frame. So run AI and spawning on the host only and mirror them on the client.
   The client's avatar on the host should be a real `player` object, because alert, allies and the multi-player distance
   helpers key off `OT_SUB_HUMAN_PLAYER`.

---

## Facts

### 1. Per-NPC AI and the yield bit (C1) [S]
- `gamestate_gp_simulate_frame` (0x3EDB60) → `human_process_all` (0x6E7440; call at 0x3EDF9E) → `npc_process_all`
  (0x6E6140), its only caller.
- **Loop 1** runs over `World.subtype_objects[OT_SUB_HUMAN_NPC]` (World+0x3E4 index array, +0x3EC count):
  1. Clears `npc+0xB8` bit0.
  2. Skips the NPC if it is already destroyed (`obj_flags` +0x54 bit 0x10).
  3. Calls `world::is_zone_streamed(&npc->pos)` at 0x6E6196; if that is false, calls `world::destroy_object(npc,0)` at
     0x6E61A7. `is_zone_streamed` (0x52E020) checks a 16x16 zone grid (`[World+0x2724+4*idx]+0x58 == 2`), not a distance.
  4. If `!(npc+0xB4 & 2) && npc+0xB0 && char_inst(+0x32C) && char_inst+0x34`, sets `+0xB8 |= 1` and calls
     `ai_system::do_frame_pre` at **0x6E61DD** (bytes `e8 de f0 bc ff`).
- **Loop 2** handles NPCs with `+0xB8` bit0 set:
  1. Calls `ai_system::do_frame` at **0x6E6224** (`e8 87 d3 bd ff`).
  2. Calls `npc::weapon_drift_process` (alive NPCs only) and the cover lean-out blend.
  3. Calls `human_do_frame(npc,1)` (0x6E5920; push at 0x6E6259). Its callees include `human_move_process` (→
     `human_move_pathfind`) and `human_animation_processing`, and no `ai_system::*`.
- Those two are the only call sites of the AI functions. There are no absolute pointers (exe scan for VA 0x6C35B0,
  0x6B52C0 and 0x6E6140 found nothing).
- **`ai_system::do_frame` (0x2C35B0), in order:**
  1. MP-client gate: `Multiplayer==1 && !multi_is_server()` → ret. `multi_is_server` (0x5431D0) = `mov al,[Server
     0x2BEA590]`. In SP this branch is never taken.
  2. Dead check (`[esi+0x650]` human → vtable+0x10) → `jmp do_death_func`. This comes **before** the yield test.
  3. At 0x2C35E8: `if (byte[+0x451] & 2) { clear bit; ret; }`.
  4. `update_stunned_state` (0x2C3601) and `update_on_fire` (0x2C3628).
  5. reduced_processing calculation (section 4).
  6. `ai_perception::do_frame` (0x2C3697).
  7. `ai_planner::do_frame` (0x2C3720; skipped while stunned, +0x450 bit 0x20).
  8. `ai_perception::do_attention_frame` (0x2C3739).
  9. `jmp ai_system::do_fire_frame` (0x2C3746).
- **`ai_system::do_frame_pre` (0x2B52C0):** same MP gate, then the null/dead check, then `test [esi+0x451],2 / jne ret` at
  0x2B52EA (it does not clear the bit), then a tail jump to `ai_planner::do_frame_pre` (0x2B535D).
- **PDB:** `ai_system`+0x450 is `ai_system_flags` (2 bytes). `yield_next_frame` = byte +1 bit1 (+0x451 mask 0x02).
  `reduced_processing` = +0x450 bit 0x10. Methods: `yield_next_frame` 0x1DF500, `clear_yield_next_frame` 0x1DC980 (no
  callers), `should_yield_frame`.
- **Engine writers of the bit:**
  - `ai_system::yield_next_frame` (0x1DF521)
  - `pedestrian_wander_action::do_action` (0x290FEE)
  - `wander_panicked_action::do_action` (0x2910BF)
  - `despawn_ped_network_action::do_action` (0x29EC20)
  - `do_frame_pre` itself at 0x2B534B (vehicle type 3, `vi_instance::find_human`, seat state 4)

  Only `do_frame` clears it. The bit therefore has to be set again **every frame**.
- **What yield skips:** planner pre-frame, stun and on-fire updates, perception, planner, attention and fire frame. NPC
  driving AI (`ai_vehicle_travel_to_a_point_do_frame` 0x28B110) is reached only from planner `do_action`s, so it stops too.
- **What yield does NOT stop:**
  - `human_do_frame` (movement along the current path, animation).
  - `npc::weapon_drift_process`.
  - Global AI: `ai_system_do_frame` 0x2BB6B0, called at 0x3EDF01. It runs the pathfind and replan rate limiters,
    `ai_search_object_manager`, `ai_defense_coordination_do_frame`, `ai_formation_do_frame`/`ai_formation_follow` and
    `ai_perception::process_movers`.
  - `ai_system_process_alert_level_change`, squads and convoys.
  - `vehicle_ai_do_frame` (0x7D0B70, from `automobile_/vehicle_do_frame_presim`), which handles u-turns, the dead-driver
    check and the intersection controller.
- **Trigger hazard:** `do_fire_frame` (0x1DEF20) pulls or releases the trigger every frame (selected by +0x450 bit 0x40).
  `human_weapons_firearm_release_trigger_common` (0x60ECF0) calls `weapon::end_looping_effects` and
  `weapon::set_trigger_pulled(0)`. Yielding mid-burst leaves `[weapon+0x18C]&2` set. `weapon::process_flame`,
  `process_arc_welder`, `process_repair` and `human_weapons_firearm_process_common` read that bit. Whether that causes
  continued fire or effects is INFERRED.

### 2. Spawn sources (C2) [S]
- `gamestate_gp_simulate_frame`:
  - 0x3EDEF3 `cmp Multiplayer,0; jne` skips `rfg_pf_do_frame` (0x3EDEFC), `ai_system_do_frame` (0x3EDF01) and
    `convoy_do_frame` (0x6B3870, call at **0x3EDF06**, `e8 65 59 2c 00`).
  - Unconditional `call gameplay_do_frame` at **0x3EDF23** (`e8 48 b8 fc ff`), the only caller.
  - Later: `trigger_region_do_frame` 0x3EE1E7, `ambush_region_do_frame` 0x3EE1EC, `safehouse_vip_do_frame` 0x3EE1F1
    (voice lines only), `mission_system_do_frame` 0x3EE1F6, `script_do_frame` 0x3EE1FB (0x73C060, tail-jumps to
    `script_run` 0x73BFC0).
- **`gameplay_do_frame` (0x3B9770, 0xCB bytes) is not flat:**
  - Calls `object_spawn_node_do_frame` (0x6AC5E0), `srm_do_frame` and `gsm_do_frame`.
  - Raids are gated on `Current_alert_level` (0x1E268B4), `Mission_active_index` and the `Raid_*` handles
    (0x3B977F-0x3B97BB). The do_frame_spawn_new_raid / spawn_selected_raid / cancel_current_raid branch structure is [R].
  - Calls `activity_system_do_frame`.
  - `if (!Multiplayer && !cutscene_is_playing())` (0x3B97CA/0x3B97D3) gates `sdc`, `reinforcement_loc`, `rfc`,
    `cont_support`, `Orders_manager`, `vehicle_drop_off`, `ssm` and `gfm_smash_building_coordinator` (0x3B97DC-0x3B980E).
  - Calls the mission start node, roadblock, morale, amb_prop and action_node do_frames, then `amb_spawn_do_frame`
    (0x75F410, call at 0x3B982C) and `amb_action_manager_do_frame`.
  - Ends with `jmp amb_edf_action_manager_do_frame` (0x251E70) at 0x3B9836, a tail jump.
- **Spawn primitives** and every caller found:
  - `object_npc_spawn_node::spawn_child_object` (0x693F60): `Action_callback_spawn_npc` 0x728D6A, `_spawn_saboteur`
    0x728BC0, `_spawn_marauder_lookout` 0x728A9E, `_spawn_jailed_npc` 0x72894B, `activity_raid_data::spawn_raid_objects`,
    `activity_house_arrest::do_spawn_hostage`.
  - `object_squad_spawn_node::spawn_child_object` (0x66D2B0): `Action_callback_spawn_squad` 0x728DCE,
    `_spawn_squad_with_icons` 0x728EAC, `_spawn_near_squad` 0x728FD8, `_orders_terrorize_spawn` 0x7295D3,
    `marauder_ambush_region::trigger_ambush` 0x6915F0 (from `ambush_region_do_frame` 0x691CAD), `safehouse_vip_notify`
    0x39095E (from `object_squad_spawn_node::stream` 0x69427D), `stream_notify_*`, `do_node_spawning` 0x679643.
  - `object_vehicle_spawn_node::spawn_child_object` (0x66CB60): `Action_callback_spawn_vehicle` 0x729482.
  - `object_spawn_node_do_frame` is also entered from `object_spawn_do_spawn` **0x6B42E0** (call at 0x6B42F8), whose
    callers are `world::setup_territory` 0x540AED, `gameplay_level_init_spawn` 0x35E570, `mission_setup_do_frame` 0x39D94D
    (called from `gamestate_gp_handshake` 0x3D916F) and `mission_terminate_cleanup` 0x3B06C7. This is not the same function
    as the NPC-list spawner `object_spawn_do_spawn(npc_spawn_param_list*)` **0x75D220**; the two share a
    name.
  - INFERRED: that `script_run` dispatches the `Action_callback_*` handlers (through the action table at 0x12B1658, not
    traced).
- **Bool switches:**
  - `Disable_reinforcement_spawn` 0x1E285CB and `Disable_guerrilla_reinforcement` 0x1E285CA are reset to 0 by
    `mission_terminate_cleanup` (0x3B04F8, 0x3B06E0). The same function calls `amb_set_ambient_vehicle_spawn_disabled(0)`
    (0x3B04F3) and `object_spawn_node_enable_mission_spawns` (0x3B04FF).
  - `Spawn_squad_disabled` 0x2C2E15F is written by `object_spawn_node_level_init` and
    `object_squad_spawn_node_set_disabled` (0x61CC50) and read by `object_squad_spawn_node::evaluate_spawn`. It is not
    touched by mission cleanup.
  - `Disable_ambient_edf` 0x1966DB6 has no code writer. Its only reference is a read in `ambient_edf_info::amb_do_spawn`
    (0x251DB0).
  - [R] `Disable_vehicle_drop_off` 0x1E28682, `Activity_spawn_disable` 0x1E28595, `Disable_ambient_vehicle_spawn`
    0x334146C, `Mission_mode` 0x334146E, `Allow_walker_spawn` 0x1293700.
- [R] `traffic_do_frame` (0x75FC20) is a 1-byte `ret`. Traffic belongs to ambient spawning.

### 3. Object identity (C3) [S]
- **Runtime objects:**
  - `world::create_object_internal` (0x53E550) → `world::alloc_object_stub` (0x53B2E0) → `handle_manager<7280>::
    allocate_handle` (0x51F160) on `zone+0xE0`.
  - The allocator is a next-fit bitmap: 0x38E bytes, byte cursor at +0x390, bit cursor at +0x394, wraps around. It returns
    `[+0x398] | (byte*8+bit)`.
  - `world::init_world_zones` (0x53A831..0x53A855) sets `gid = ((a&0xF)|b<<4)<<8|0xFF`, stores `zone+0x47C = gid` and
    `zone+0x478 = gid<<16` (= handle_manager+0x398).
  - So a runtime handle is `top-byte zone id | 0xFF<<16 | slot`. The slot depends on the allocation history, so runtime
    handles diverge between machines (INFERRED from the allocator, and consistent with C6).
  - `allocate_handle` is also called by `world::alloc_obj_handle` and `world_zone::alloc_obj_handle`.
  - [R] `world::create_object<npc>` is 0x624450. `spawn_npc` (0x747F40, params in EAX) is the common NPC creator.
- **File and save objects:**
  - `world::alloc_object_stub_preexisting` (0x53B3C0) → `world::alloc_obj_handle_preexisting` (0x53B210) uses the stored
    handle as is.
  - The zone is taken from handle bits 24..31 (`((h>>24)&0xF)<<4 + (h>>28)`, `[World+0x2724+4*idx]`), then the code does a
    hash find/insert at World+0x480 and writes `*slot=obj`. No bitmap reservation, no duplicate check (overwrite on a
    collision is INFERRED).
  - Callers: `world::visit_stub` (0x53B70E, on the `restore_serialize_zone` path) and `world::serialize_zone` (0x53F35C,
    0x53F591).
- **Byte 2 == 0xFF in serialized data:**
  - `serialize_zone(world_zone*, zone_file*, bool)`: when the bool is set (0x53F285), records with byte 2 == 0xFF
    (`cmp [rec+6],0xFF` at 0x53F28F) get their bitmap bit reserved (0x53F2DF).
  - `world::zone_visit_cb` passes true (0x53F876); it is the callback of `stream_layout_level_load` in
    `init_territory_streaming_resources` (0x53F8D8). `process_streaming_zone` and `post_load_stream` pass false.
  - `visit_stub` sets `obj+0x55 |= 0x40` (file-owned, `free_obj_handle` 0x53B270 leaves the bitmap alone) only when byte
    2 != 0xFF (0x53B719).
  - So persisted runtime objects come back from save or stream data with their 0xFF handle. They still match across
    machines that load the same save.
- **Shipped data sampled, no 0xFF handles in any of it:**
  - `p_terr01_04_06.rfgzone_pc`: 89 objects, `0x46xxxxxx`, byte 2 in {0,1,2,4..9,12}.
  - `p_terr01_06_09.rfgzone_pc`: 19 objects, `0x69xxxxxx`.
  - `terr01_04_06_140.layer_pc` (1 object) and `terr01_04_07_128.layer_pc` (97 objects, byte 2 up to 128).
  - 34 DLC rfgzone_pc files (byte 2 up to 16).
  - The main terr01 zone files inside `terr01_l0/l1` were **not** checked.
  - Format: RFGM.Formats-master `RFGM.Formats/Zones/RfgZoneFile.cs`, `RfgZoneObject.cs`.
- **Engine MP IDs:**
  - Network handles are 16-bit. `multi_core_network_handle_from_game_handle` (0x5B63F0) looks in
    `Reserved_game_to_network_hash` first, then the remote-object table, and returns a word.
  - `add_reserved_handles<rfg_mover>` (0x5D7110) sorts by game handle (`obj_handle_compare` 0x54E3F0), then calls
    `multi_core_add_reserved_network_handle` (0x5B5C40).
  - This table is filled only in MP code and is empty in SP.
  - Registrations: automobile, item, multi_mover, player, projectile, rfg_mover, turret, walker. **None for npc or human.**

### 4. Local-viewpoint centring on the host (C4) [S]
- `player_get_local_player` (0x610540) = `mov eax,[Player 0x2C22874]`. There is no other player list in SP.
- **Streaming:** `world::do_frame` (0x540950) writes `World+0x2710 stream_pos` from the player (0x5409AD..0x5409BD) only if
  `Stream_follows_player` (0x2CEB1EC) is set. Nothing ever writes that flag (two `cmp` refs only), so stream_pos comes from
  `Rfg_camera+0x2C` (0x5409C5). It has a single centre.
- **Ambient spawns:** `Player_spawn_region` (`player_bounding_region`, 0x33472A0) is updated by `update_region` (0x747770)
  from `player_get_local_player` (0x7477A5) and `Rfg_camera` (+0x2C, +0x50..+0x70). `can_spawn_ambient_vehicle`
  (0x74DD20) queries it too.
- **Spawn nodes:** `object_spawn_node_do_frame` +0xA4 (0x6AC684) does `mov eax,[Player]`, then calls
  `evaluate_node_spawning<object_npc_spawn_node>` (0x64DE30, cdecl `player*`) and `<object_vehicle_spawn_node>`
  (0x64DEF0). A node is evaluated only within 100 m of that player (10000.0 double at 0xEC1A80), or 200 m when node flag
  0x40 is set (40000.0 at 0xECA740). Out-of-range nodes get `node+0xAC` bit 0x4 cleared (0x64DE9D).
- **Despawn:**
  - `ai_system::can_despawn` (0x20A4C0) calls `player_get_local_player` (0x20A5A7) and returns true if there is no player
    or `npc::despawn_overdue` is set. Otherwise it compares distance squared against `MIN_TELEPORT_DIST_SQRD` 400 (20 m)
    offscreen, `MIN_VEH_ACTIVE_DESPAWN_DIST_SQ` 10000 (100 m), or 2500 (50 m) on foot.
  - `object_squad::should_attempt_despawn` (0x670620) does `mov ebp,[Player]` (0x670690) and then `vec_dist_squared`.
- **AI slow-down:** `ai_system::do_frame` clears +0x450 bit 0x10 (0x2C362D), then computes
  `vec_xz_dist_squared(npc, local player)` against `AI_SLOWDOWN_VEHICLE_PLAYER_DIST_SQ` = 6400.0 (0xEC7390, 80 m) when
  **the NPC** is in a vehicle (`human_in_vehicle(npc)`), else `MAX_PED_FOLLOW_PLAYER_DIST_SQ` = 1600.0 (0xEC750C, 40 m).
  If the NPC is farther, the bit is set (0x2C368B). `ai_perception::do_frame` (0x2C2ED0) tests it at 0x2C2EDC. [R] It
  doubles perception intervals and is read by `ai_planner::do_replan`, `require_high_priority_replan` and `notice_humans`.
  It slows the AI but does not stop it.
- **Consequence:** without a leash, NPCs near a distant client are slowed, despawned, never spawned (nodes beyond 100 or
  200 m, no ambient region), or deleted once the client is outside the host camera's streamed zones.

### 5. Targeting, alert and allies (C5) [S]
- **`ai_perception::notice_humans` (0x2C2870)** loops over all humans (World+0x74 = `type_objects[OT_HUMAN]`), switches on
  `get_effective_disposition(target,1)` (0x265A40) and dispatches to `notice_ally` (4), `notice_friendly` (3),
  `notice_unfriendly` (1) and `notice_enemy` (0). It is **not** fully generic. Two stack flags gate non-local humans:
  - `[esp+0x13]` (allies and friends) is 1 only when the `notice_friendlies_time` timer (perception+0x10/+0x14) has
    elapsed (0x2C28CD).
  - `[esp+0x12]` (unfriendlies and enemies) is 1 when `perception+0x218` (belief bin 1) is empty, or the current action is
    `terrorize_action`. For a non-combatant it is 1 when it has no flee-to-safety belief (0x2C29EC..0x2C2A33).
  - Otherwise `cmp edi,[esp+0x14]` (the local player, 0x2C2ADD/0x2C2B0D) skips every other human. INFERRED: an NPC that
    is already engaged will not newly notice a second player through this path.
- `get_effective_disposition` and `can_engage_target_reason` (0x268BE0) contain `object_is_local_player` (0x614BD0)
  logic. [R] The local-player-only friendly-fire rule (`Player_friendly_fire_threshold`) and the undercover team at
  +0xC9C. [R] `object_is_local_player` has 151 callers (target_in_fov, register_hit, notice_projectile, combat hit
  processing, ...). [R] `ai_system::is_engaging_player` (0x20ABD0) and `should_ignore_player` (0x1EEB10, `Ai_ignore_player`
  0x1966DCD) are local-player only.
- **Alert inputs:** `has_los_to_enemy_player` (0x38B560) and `get_hostile_target_count` (0x38B4B0) loop over all NPCs
  (World+0x3E4/+0x3EC) and keep those with `human_teams_get_disposition(npc,1) >= 3`. They then call
  `ai_perception::any_enemy_player_in_los` (0x283820), which scans belief bin perception+0x238 with
  `belief_obj_iterator<player>::seek_obj` (0x24B710: `obj_type(+0x7E)==0 && sub_type(+0x7F)==3`). That accepts **any**
  player-subtype object, with no LOS test of its own. Then they check `human_combat_get_target` and
  `can_engage_target_reason`.
- [R] `gsm_do_frame` (0x3AA770) ticks once per second and returns in MP or WC. Its distance decay uses
  `get_dist_delta()` (0x373350, local player → `Last_hostile_action_pos`). Its restricted-area check and its tank/walker
  check also use the local player. The alert value is one global (`Current_alert_level` 0x1E268B4, `Current_alert_value`
  0x1E248A0), which suits co-op. The alert table (`alert_level.xtbl`) gives Yellow 250-300, Orange 1000-1100, Red
  3000-3100, ContactGrowRate 75.
- **Allies:** `player`+0x24C4 `ally_squad_handle`, +0x24C8 `escort_squad_handle`. `object_squad::get_player` (0x66E810;
  squad+0x134 bit 0x200) loops over all players (World+0x3F4/+0x3FC = `subtype_objects[OT_SUB_HUMAN_PLAYER]`). [R]
  `player::ally_squad_add` (0x684540) does not read the local player.
- **Precedent:** `wrecking_crew_spawn_players` (0x39F130) creates `Wrecking_crew_options+0x4` player objects through
  `create_object_internal(player::get_classname_hash())` (0x39F1C4) and hides the inactive ones.
- **World layout (PDB):** +0x60 `all_objects`, +0x70 `type_objects[53]`, +0x3C0 `subtype_objects[11]`. `base_array` is 0x10
  bytes (elt at +4, num at +0xC). `OT_SUB_HUMAN_PLAYER` = 3.

### 6. RNG (C6) [S]
- **RNG 1, the Vlib MT pool:** `Vlib_rand_instance` at 0x123A2B8, with a pool of 0x800 dwords at +0x9C8 and an index at
  +0x9C4. `vlib_system_init` (0x1CFCA3) fills it from the default state. It is reseeded only by
  `rand_regenerate(timer_get(1000000))` at 0x1D5771 (in `rfg_init_stage_2_done`), the sole caller of `rand_regenerate`
  0x17A290.
  - Readers: `fl_rand` 0x17A300 (140 calls), `int_rand_range` (290), `fl_rand_range` (181), `timestamp::set_rand` (252),
    `vrand` (96), `vector::rand_sphere` (23), `int_rand_chance` (18), `fl_rand_chance` (12), `bool_rand` (5) and
    `net_lag_*`.
- **RNG 2, the CRT `rand`/`srand`:** imports at [0x123F3F8]/[0x123F3FC]. `srand(timer_get(1000000))` runs at 0x1D23B7..0x1D23C8
  (`rfg_init_stage_2_done_2`). `do_game_main_loop_frame` calls `rand()` once per frame (0x1D263C). `create_cells` (0x532EF9)
  reseeds it locally.
  - About 30 call sites use it, including `pick_model`, `find_spawn_point_road`, `activity_get_player_start`, destruction
    (`rfg_dyn_slice_shape`, `object_mover_process_explosion_hit`, `get_rand_explosion_size`, ...) and Lua `math_random`.
- Divergence of spawns between machines: INFERRED, strongly supported.

### 7. Multi-player distance helpers (C7) [S]
- Helpers:
  - `player_find_nearest_player` 0x65CAB0
  - `player_find_nearest_player_xz` 0x65CB60
  - `player_find_shortest_player_dist_sqrd` 0x65CC10
  - `player_find_shortest_player_xz_dist_sqrd` 0x65CCD0
- Each tests `Multiplayer` (at 0x65CAB8, 0x65CB68, 0x65CC13 and 0x65CCD3). SP uses only `Player`. MP loops over
  World+0x3F4/+0x3FC through `all_objects` (+0x64).
- **The two `nearest_player` helpers are dead code:** no call sites and no pointers. Only the `*_dist_sqrd` helpers
  matter.
- **Callers:**
  - `xz_dist_sqrd`: `is_near_a_player` (0x35D400, called by `alm_report_damage`), `find_local_spawn_region`,
    `alm_report_death`, `squad_member_manager_data::do_squad_simulation`, `activity_house_arrest::activity_do_frame_ready`,
    `activity_killing_spree_data::register_death`, `activity_courier_chase_data::check_for_end`.
  - `dist_sqrd`: `find_spawn_region`, `examine_corpse_goal::get_fitness`, `activity_courier_chase_data::check_for_start`
    and `::check_for_end`.

### 8. Ambient population and caps (C8: no skeptic vote; investigator-verified) [R]
- **`amb_spawn_do_frame` (0x75F410):**
  - Returns if `Multiplayer==1` or Wrecking Crew is active.
  - Updates `Player_spawn_region`.
  - `update_desired_guerrillas` (0x75E630) picks spawn groups for the local player's zone.
  - `Disable_ambient_vehicle_spawn` or `Mission_mode` skips slot allocation.
  - Alternates `do_frame_spawn_npc` (0x75F250) and `do_frame_spawn_vehicle` (0x75F0A0).
- **`do_frame_spawn_npc` caps:** spawning slows when the NPC count (World+0x3EC) is at least 75 (0x4B) and stops at 90
  (0x5A). `activity_disable_ambient_ped_spawn` (0x356990) also stops it. The NPC pool is 100 (`npc::pool_remaining`
  0x633010).
- **Data** (`misc.vpp_pc`):
  - `ambient_spawn_info.xtbl` maps each terr01 zone to an `npc_spawn_group` (e.g. "Guerilla Dust") and a
    `vehicle_spawn_group` (e.g. "Amb_Dust").
  - The human groups hold civilian models (`spawn_group_human.xtbl`).
  - The vehicle groups hold `Min_*`/`Col_*` civilian vehicles (`spawn_group_vehicle.xtbl`).
  - So ambient peds and traffic are civilians.
- **Density locks:** `amb_spawn_lock_ped_levels` 0x742910, `amb_spawn_lock_veh_levels` 0x742940,
  `amb_spawn_unlock_ambient_levels` 0x742970. Density levels are NONE 0, LOW 1, MEDIUM 2, HIGH 3.
- **Ambient EDF:** `amb_edf_action_manager_do_frame` (0x251E70) returns in MP, WC or while a mission is active. Its spawn
  kinds (drive-by, fly-by, harass, dump civ) read the local player.
- **Struct anchors (PDB):**
  - `npc` (0xD90): +0xD00 npc_flags, +0xD38 spawn_node_handle, +0xD40 creation_system (SQUAD 0, AMBIENT 1, SPAWN_NODE 2,
    CUT_SCENE 3, ...), +0xD80 `ai_system*`.
  - `ai_system` (0x6A8): +0x18 perception, +0x300 planner, +0x454 desired_target, +0x650 npc, +0x658 squad_handle.
  - `human`: +0x264 velocity, +0x2F4 cur_state, +0x2F8 movement state, +0x2FC stance, +0x304 movement_mode, +0xAE8
    look_at_pos, +0xC98 current_team, +0xC9C undercover_team.

---

## Approach for co-op

1. **Authority.** The host runs all AI, spawning, alert and squads. The client mirrors them. Lockstep is ruled out by
   C6.
2. **Do not set `Multiplayer`.** It flips hundreds of branches and disables reinforcements, convoys, ambient spawning and
   `ai_system_do_frame`. Reproduce the engine's per-NPC client gate instead.
3. **Client puppets:**
   - Create puppets through `object_spawn_do_spawn(npc_spawn_param_list*)` 0x75D220 with manager `NPC_MANAGER_CUT_SCENE`
     (3) and an SRM mission slot.
   - Every frame before `npc_process_all`, set `ai+0x451 |= 2` (`ai` = `npc+0xD80`). Alternatively, redirect the calls at 0x6E61DD and 0x6E6224
     to wrappers that skip puppets; that needs no per-frame writes, and puppets then also skip `do_death_func`.
   - When engaging the switch, release the trigger (`human_weapons_firearm_release_trigger_common`) and clear or override
     the current path.
   - Drive puppets with `human_move_direct_travel_to` (0x6309A0) and use `human_teleport` (0x69A3C0) when the error is
     large. Make puppets invulnerable and forward hits to the host.
   - Remember that `vehicle_ai_do_frame` and global AI still act on puppets and puppet vehicles.
4. **Client spawn suppression:** hook the three `*_spawn_node::spawn_child_object` functions (0x693F60, 0x66D2B0,
   0x66CB60) to refuse non-puppet spawns, gate `amb_spawn_do_frame` and the ambient EDF, and skip `convoy_do_frame` (call at
   0x3EDF06). Keep `script_do_frame` running if the client needs mission UI, since missions are a separate question. The
   bool switches only help partially (section 2): re-assert the ones that get reset every frame and do not rely on them.
5. **Net IDs:** use an `unordered_map<netId, handle>` in both directions.
   - Objects loaded from files or the save can use their handle as the net ID, but only once both machines have that
     zone or layer streamed in the same state.
   - Everything the host creates after the session starts gets a host-assigned ID from a disjoint space (for example
     0x80000000|counter). Do not classify by byte 2 alone.
   - Check the classname hash and object type on every lookup, because runtime handles get recycled.
   - Despawn messages must be reliable.
6. **Host sees the client as a player:** the client's avatar on the host should be a real `player` object (WC precedent).
   That gives it alert LOS, allies (`get_player`) and the multi-player helpers. Patch the SP branch in
   `player_find_shortest_player(_xz)_dist_sqrd` (0x65CC13, 0x65CCD3). Also patch the local-player gates in
   `notice_humans` (0x2C2ADD, 0x2C2B0D), or EDF already in combat will not pick up the client.
7. **Leash (v1):** keep both players within roughly 40 m at first, then try 100-150 m, so that host streaming (camera
   based), spawn regions, node range (100 or 200 m) and despawn distances also cover the client.
   - v2 patches: `ai_system::can_despawn`, `object_squad::should_attempt_despawn`, the reduced_processing distance
     (0x2C3634..0x2C368B), `get_dist_delta`, and a second spawn-node pass.
   - A second `evaluate_node_spawning(clientPlayer)` call clears node bit 0x4 for nodes out of range of each player in
     turn, so it needs its own flag handling.
8. **Replicate by scanning:** each tick, walk the World NPC list and diff it by handle to produce spawn and despawn
   messages. Send state at 10-20 Hz inside an interest radius. Keep the client's NPC count well under 90 (pool 100).
9. **v2 option:** civilians stay client-local (they come from ambient groups, section 8). Lock density low, turn off
   `Allow_spontaneous_conversion` (0x12500DB, behaviour INFERRED), and exclude `creation_system == AMBIENT` from
   replication.

---

## Contested / refuted (do not rely on)

- **"Every NPC source except convoys is reached through `gameplay_do_frame`"**: refuted. Script actions, ambushes, stream
  callbacks and `object_spawn_do_spawn` (0x6B42E0) from territory setup or mission setup and cleanup spawn NPCs outside
  it. Patching 0x3EDF23 and 0x3EDF06 alone is not enough.
- **"gameplay_do_frame is a flat list of calls"**: false. It has raid conditions, an `!Multiplayer && !cutscene` block,
  and ends in a tail jump.
- **"Disable_ambient_edf / Spawn_squad_disabled are reset by mission cleanup"**: false. Only `Disable_reinforcement_spawn`
  and `Disable_guerrilla_reinforcement` are reset (cleanup also re-enables ambient vehicle spawns and mission spawns).
  `Disable_ambient_edf` has no code writer at all.
- **"Yield = clean puppet switch"**: only partly true. The death path runs first; stun and on-fire updates are also
  skipped; a held trigger stays pulled; the last path keeps being followed (INFERRED); global, squad, formation and
  vehicle AI keep running; and the engine writes the same bit itself.
- **"File handles never have byte 2 == 0xFF"**: false for save and stream-layout data. It holds only for the shipped files
  sampled. The report also said byte 2 is in 0..9; it reaches 12 in zones and 128 in layers.
- **"Streaming follows the local player"**: it follows the local camera (`Stream_follows_player` is never set).
- **"80 m in a vehicle"** means the NPC is in a vehicle, and the distance is XZ only.
- **"AI targeting is generic, NPCs fight any hostile human"**: overstated. `notice_humans` only considers non-local humans
  on the friendly timer tick, or when the NPC has an empty belief bin 1, is terrorizing, or is a non-combatant with no flee
  belief.
- **"Flip one branch in each of the four `player_find_*` helpers"**: `player_find_nearest_player` and `_xz` have no
  callers. Only the two `*_dist_sqrd` helpers matter.
- **"One shared RNG pool, seeded once"**: false. There is a second CRT RNG, advanced every frame and reseeded by
  `create_cells`.
- **Unverified counts**: "944 functions reference Player", "81 AI", "~195 spawn" (report F13). The skeptic did not
  recount them, and the xref table may record Player data refs only partially. Treat the numbers as rough.
- **`object_spawn_do_spawn` is two different functions**: 0x6B42E0 (spawn-node pass) and 0x75D220 (NPC param list,
  the one used for puppets).

---

## Unknowns and the probes that settle them

| # | Unknown | Probe (SP test build) |
|---|---|---|
| 1 | Does a yielded NPC stand still or keep walking its last path? Does a held trigger keep firing? | Spawn EDF; set `+0x451|=2` each sim frame while it is idle, walking and firing. Watch movement, the flame/loop effects, and `[weapon+0x18C]&2`. Then drive it with `human_move_direct_travel_to` and count stuck/teleport events. |
| 2 | The full set of client spawn paths | Hook the three `spawn_child_object` functions plus `amb_spawn_do_frame`, ambient EDF and convoy. Play a mission, an ambush area and red alert for 10 min. Log World+0x3EC and any NPC whose `creation_system` is not CUT_SCENE. Expect 0. Check that mission start markers still work. |
| 3 | Handle agreement across machines (file vs 0xFF) | Two runs on the same save, standing still: dump `(handle, classhash, obj_type, pos)` from `all_objects` (+0x60). Expect file handles to match. Check whether restored 0xFF handles also match. |
| 4 | Can a second `player` object live in the campaign? | WC-style `create_object_internal(player::get_classname_hash())`, team guerrilla. Check `player_process_all`, camera, HUD, save; whether EDF engage it (fresh NPCs and already-engaged NPCs); whether alert rises (`has_los_to_enemy_player`); whether `ally_squad_add` makes allies follow it. |
| 5 | notice_humans patch | Same probe with the jumps at 0x2C2ADD/0x2C2B0D patched. Do engaged EDF switch to the second player? |
| 6 | Distance effects | Put a squad and the probe player 50, 100 and 200 m from the host. Log +0x450 bit 0x10, `can_despawn`, and destruction by `npc_process_all` when the zone is unstreamed. |
| 7 | Helper flip | Patch the jumps at 0x65CC13 and 0x65CCD3. Confirm that `alm_report_damage` and `find_spawn_region` react near the probe player. |
| 8 | Second node pass | Call `evaluate_node_spawning<npc>(probePlayer)` each frame with the probe player in another garrison. Do nodes there spawn, and does bit 0x4 flapping cause re-spawn spam? |
| 9 | Puppet capacity | Spawn N distinct `human_info` puppets through mission SRM slots. Record successes and streaming time. |
| 10 | `npc+0xB0` / `+0xB4 bit 2` meaning (could be a cleaner per-NPC inactive switch) | Toggle each on a test NPC and watch whether the AI and `human_do_frame` stop. |
| 11 | Does `script_run` dispatch `Action_callback_*`? | Breakpoint or log hook on `Action_callback_spawn_squad` during a mission (or trace the table at 0x12B1658). |
| 12 | Kill credit and morale for the client (`gfm_control_morale_human_killed` reads the local player) | Kill an EDF NPC with the probe player as killer and diff the morale and control values. |
