# RfgCoop notes: gap round (mission fence, Geo-Mod determinism, puppet SP safety, guest fences, mission content)

Distilled 2026-10-07 from the five gap reports `research/findings/workflows/coop_gap_1.md` … `coop_gap_5.md` plus the
skeptic verdicts on their claims. Build: GOG rfg.exe, 32-bit, PDB image base 0x400000. All addresses are **RVAs** unless
marked VA (VA = RVA + 0x400000; at runtime add the module base, ASLR). Nothing here was run in game.

Tags:
- **[V]** = read in code, data or PDB by the investigator and upheld by the skeptic, or re-read in this integration pass
  (marked "re-read").
- **[V1]** = read by one investigator (or two independently) but not re-checked by a skeptic.
- **[P]** = the core holds, the claim was narrowed or corrected by the skeptic; the corrected form is what is written.
- **[I]** = inferred, not read.

Patch ids: the gap reports reused ids (two different "G1"s, a "T1" that clashes with RESEARCH's hit tap). This note
renames them: **GF-*** guest fences, **PP-*** puppet patches, **DP-*** destruction profile. The report's own id is given
in brackets.

## TL;DR

- **The guest's mission fence at `player_start_mission` was not enough.** A host *checkpoint* blob carries the active
  mission index and the mission's running managed scripts. Loading it makes the guest start the mission itself
  (`world::setup_territory` 0x540AB0) and run the scripts (`script_do_frame` has no mission gate when no mission is
  active). Fix on the guest: NOP the restore-index store 0x39D69C, skip `script_do_frame` at 0x3EE1FB, keep the
  `player_start_mission` refusal. An activity-replay blob likewise force-starts its activity (`Activity_node_start_on_load`).
- **Retry from start never restarts a mission.** Only a checkpoint "continue" re-activates one; 8 missions have
  checkpoints, DFA has none.
- **Mission starts regrow buildings** (`restore_list_do_restore`), outside the Geo-Mod event queue. The guest needs a
  RESTORE event.
- **Managed layers are logic, not scenery.** No building, mover or vehicle in any mission layer. Objective *buildings* are
  base-zone objects with identical handles; map icons and ground markers sit mostly on layer logic objects, so they go
  by position.
- **The guest must never run `srm_mission_reservation`** (wipes reservations, random variants, then `Mission_mode` blocks
  every further reservation). It reserves the host's actual model set with `srm_reserve_slot(SPAWN_SLOT_MISSION,…)`.
- **All runtime NPC and vehicle creation ends in `spawn_npc` / `create_new_vehicle`**, ambient peds and traffic included.
  But one call, 0x3EDF23 (`gameplay_do_frame`), holds almost every periodic driver, so it is the early fence. Raids are not
  stopped by `activity_suspend_activities`. Turrets and roadblock barricades bypass both refusals.
- **Replayed destruction on the guest runs campaign consequences**: district control, liberation (and the Dust safehouse
  warp), activity and stats hooks. Most of these functions have a `cmp [Multiplayer]` early-out whose operand can be
  repointed to a guest byte. `district::set_control` is not a live setter: it runs the liberation tail.
- **Geo-Mod: RNG control is cheap but not the main lever.** Per-machine inputs are the SP debris load balancer (camera
  distance, visibility, measured Havok time), immediate slices and uber explosions (frame count, live velocity), the
  unsorted shardify list and the out-of-queue debris deleter. Stock MP's branches remove them; 4-5 operand repoints copy
  that. Chunk *poses* need a pose lane in any case. The shared random pool is also drawn from on the render thread.
- **A puppet crashes SP on its first processed frame** (`player_do_frame` non-local tail → MP stats → NULL
  `Mp_game_data`). Fix: 0x6E6B44 `8B CE` → `EB 5E`. Also: the local stick drives every airborne player-class human, and
  several camera sites treat "is a player" as "is the local player".

---

## Facts

### 1. Mission activation paths (gaps 1, 4, 5)

- `mission_activate_pre_save` 0x38E410 is the only writer of a valid `Mission_active_index` (0x125050C). Its only callers
  are the two `mission_activate` wrappers (0x38E569, 0x39D9B2). **Register ABI**: handle in EAX, bool on the stack,
  caller cleans, despite the PDB's cdecl. Prologue `57 83 CF FF 39 3D <abs32>` (10 bytes, no relative branch, abs32
  relocated). [V]
- Wrappers [V, re-read]:
  - `mission_activate(mission_handle,bool)` 0x38E560: `mov eax,[esp+8]; push eax; mov eax,[esp+8]; call pre_save; pop ecx;
    ret`. Only caller: `world::setup_territory` 0x540AB0.
  - `mission_activate(char*,bool)` 0x39D960: `push ecx; push esi; …` writes `Mission_saved_checkpoint` (0x1250500) and
    0x1250508 = -1, looks up `Mission_hash_table`, calls pre_save, `pop ecx; ret`. Callers: `player_start_mission`
    0x633FD3, `player_is_near_mission_start` 0x65B783 (autostart, bool 1), `hud_ui_mission_briefing_start` 0x458C7E,
    `hud_ui_mission_briefing_process` 0x45867F, `humb_process_bink_video` 0x45825E (only from the briefing, 0x45898E).
  - Both are plain caller-cleaned functions, so a `C3` at either entry is a valid early return [I from the re-read].
- pre_save [V]: returns if a mission is active or handle == -1; returns if `Mission_checkpoint_restore_index` (0x1250510)
  != -1 and != handle (0x38E428-0x38E434); writes `Mission_active_index` at 0x38E478.
  - Fresh start (restore index -1): attempted flag, `mission_reclaim_charges_and_mines` (0x38E4BD),
    `world::save_and_continue(NULL,false,true)` at 0x38E4C8-0x38E4D1 (the pre-mission anchor, buffer 0, not a checkpoint;
    skipped for mode 8), and a despawn loop for squads with `+0x17D & 0x60` (0x38E50B-0x38E52D).
  - Checkpoint restore (restore index == handle): no save, no despawn.
- `mission_activate_post_save` 0x39D6B0 [V]: `managed_layer_set::start_layer_loading(mi+0x28)`; on a fresh start only,
  `restore_list_do_restore(mi+0x10 start_node_handle)` (0x39D6F1-0x39D6FE, cdecl); `activity_suspend_activities(1)`;
  `human_voice_load_set(mi+0x30)`; EDF progression lock; `release_vip_reservation`; `squad_pre_mission_clean_up`;
  `srm_mission_reservation(mi+0x2C)` (0x39D73A); `object_upgrade_node_disable_markers`; restore index = -1 (0x39D74C);
  `cartographer::remove_all_paths`; for the Tutorial (mode 4) `game_time_of_day_set_tutorial_tod` (0x39D7AD: Game_clock
  09:30, `Enable_tutorial_hacks`) on fresh and checkpoint starts; music; `local_player_fill_ammo`.
- `mission_setup_do_frame` 0x39D7D0 [V]: callers `gamestate_gp_handshake` 0x3D916F and `world::setup_territory` 0x540AB8.
  Returns unless a mission is active and not ready. Then post_save, restore wait, voices, layers, three
  `script_activate_managed_group` calls (0x39D8CF, 0x39D8EC, 0x39D911), `srm_all_reserved_slots_streamed`, ready, then
  `world::do_frame` and **`script_do_frame` at 0x39D93D**.
- `mission_info` (0x60, `All_mission_infos` 0x1E45C18): +0x10 start_node_handle, +0x14 mis_mode, +0x18 reward_salvage,
  +0x1C morale_multiplier, +0x28 managed_layer_set*, +0x2C srm_data, +0x30 voice personas, +0x38 managed_script_list,
  +0x50 checkpoint_managed_script[2], +0x58/+0x59 mission_flags (complete, layers_loaded, scripts_loaded,
  checkpoint_script_loaded, attempted, viewed, unlocked, unlock_notified / ready, checkpoint2_script_loaded). The two flag
  bytes are saved verbatim in the blob. [V1]

### 2. What a world blob carries into the guest (gaps 1, 4, 5)

- `mission_system_save` 0x38E3A0 writes `Mission_active_index` only when `World+0x2578` (cur_world_state) == 1 and
  `!Mission_succeeded`, else -1. `world::save_state_data` 0x541250 sets `World+0x2578` = 1 for checkpoint saves and 0 for
  all others (0x541261-0x541279) before saving. So **only checkpoint blobs** (buffer 1, written by
  `save_and_continue(NULL,true,true)` in `mission_system_do_frame` 0x3B4DD1) carry a mission; the anchor and every
  free-roam save carry -1. [V/P: "any blob saved during a mission" was too broad]
- Load order [V]: `world::load_territory` → `gamestate_gp_level_init_pre` (0x54132A) → `mission_system_level_init`
  (0x3EFAF6, writes restore index -1 at 0x3A5883 and active -1) → `init_territory` (0x541370) → `init_world_zones`
  (0x540015) → `mission_system_load` (0x53A993; **store at 0x39D69C**, bytes `A3 <abs32>`, re-read) →
  `script_system_load` (0x53AAD8) → `restore_list_system_load` (0x53AC0E) → `gs_load_process` loops
  `world::setup_territory` 0x540A80. `init_territory` loads `stored_world_state[World+0x2578]` (0x53FD32-0x53FD46).
- `world::setup_territory` [V]: returns at once when `Multiplayer` != 0; else `call mission_get_checkpoint_handle`
  0x540A9E (`E8 BD DF E1 FF`), `cmp eax,-1` 0x540AA3, `je` 0x540AA6, `call mission_activate(idx,0)` 0x540AB0, then
  `mission_setup_do_frame` 0x540AB8. Also `object_spawn_do_spawn()` 0x6B42E0 at 0x540AED (initial garrisons).
- **Scripts are in the blob** [V]: `script_system_save` 0x73C350 saves every `action_script`, the
  `Loaded_managed_script_groups` table (skipping groups whose data byte has bit 0), variables and timers, then unmanaged
  (`+0x40 == -1`) and managed scripts. `script_system_load` 0x73C4C0 (only caller `init_world_zones` 0x53AAD8) restores
  them with no mission or `Multiplayer` gate.
- `script_do_frame` 0x73C060 [V]: returns when `World+0x265C & 0x28` or `World+0x265D & 0x10`; is skipped only while a
  mission is active **and not ready**; otherwise runs the evaluate/run loop over every action script (no group filter)
  and tail-jumps (0x73C184) to `script_run` 0x73C045. Actions run only through `script_run_script` 0x73BD30 (from
  0x73C156 and `script_run`). Two callers: `gamestate_gp_simulate_frame` 0x3EE1FB (`E8 60 DE 34 00`) and
  `mission_setup_do_frame` 0x39D93D (`E8 1E E7 39 00`); both re-read.
- `script_event_evaluate` 0x7325F0 (the gem callback) only bumps `action_script+0x38` and resets nodes [V1].
  `gem_post_event` dispatches registrants synchronously (`call eax` at 0x7378E8); whether any handler runs actions is
  unread.
- `script_activate_managed_group` 0x73F410: callers `mission_setup_do_frame` ×3 and
  `activity_base::load_managed_script_group` 0x37F169. `script_terminate_managed_group` 0x73B700: `mission_system_do_frame`
  ×3 and the activity unload. [V1]
- **Activity replay** [V]: `Activity_node_start_on_load` (0x1250244) is saved/loaded (`activity_system_save` 0x369AF0 /
  `_load` 0x369B5F), set by the `activity_*_replay` functions before `save_and_reload`, and consumed at 0x3ACB17 in
  `activity_system_do_frame(0)`: unless `World+0x265D & 4`, `activity_force_start_generic(node,0)` 0x3A8690 starts the
  activity in place (raid start, convoy start, delivery vehicle, `start_activity_spawn`, `activity_kill_all_except`).
  That consumer runs only inside `gameplay_do_frame`.
- **Autostart** [V1, two investigators]: `player_is_near_mission_start` 0x65B5D0 skips its scan when a mission is
  active, the restore index != -1, an activity runs, or the node's mission is complete (0x65B676) or locked; it calls
  `mission_activate(name,1)` at 0x65B783 (`6A 01 50 E8 …` at 0x65B780) when node+0xA1 autostart is set and
  `World+0x2578 == 0`, with the passed (local) player. Of the 22 start nodes in `zonescript_terr01`, only Tutorial
  (3C000029) has autostart = 1.
- Briefing paths [V1]: `hud_ui_district_liberation_exit` 0x46888A passes "Parker Liberation" (VA 0x12FB1D4), which is
  not a `missions.xtbl` row, so the lookup misses (return at 0x39D99C). The briefing's own reload (0x4585FB) needs
  `Mission_parker_liberation_warp` 0x279CCFF, written only by a debug command. `mission_warp_and_start` 0x3AF5A0 is
  reached only from `unused_dcf_mission_start`.

### 3. Fail, retry, cancel, checkpoints (gap 1) [V]

- Fail: `mission_terminate(0)` (death branch 0x3EDC0C or `Action_callback_mission_terminate` 0x721EE1) →
  `mission_terminate_internal` → `hud_ui_mission_complete_start(false,…)` 0x3B4BCF. `Mc_info` 0x2794698: +3
  restart_mission (means **abandon**), +8 result.
- "Continue" → `humc_request_exit` 0x44A820 → `set_pending_territory_load` 0x44A8D8: reloads the current buffer (the
  checkpoint if cur = 1, else the anchor). The abandon button (0xB) is offered only after a fail with `World+0x2578 == 1`
  (0x459601-0x45960F) and reloads the anchor. Esc → Cancel mission (`mission_cancel` 0x38E670 → 0x38E75D) reloads the
  anchor.
- **After the anchor (buffer 0, mission -1) loads, nothing re-activates the mission**; the player restarts it at the start
  node. Only a checkpoint "continue" re-activates one.
- Checkpoint save: `Mission_checkpoint_pending` is set only by `Action_callback_mission_save_checkpoint` 0x7226D0 (checks
  `mission_is_active`); `mission_system_do_frame` 0x3B4DB2-0x3B4E82 increments `Mission_saved_checkpoint` (RAM only) and
  saves buffer 1 with no disk write.
- 8 missions have checkpoints: Walker Martian Ranger, PartyTime, Highway To Hell, Guns of Tharsis, Save the Guerrilla
  Camp, Emergency Broadcast System, Assault the EDF Central Command, Final Mission. **Death From Above has none.**
- On success, `mission_terminate_internal` skips the result screen and its save when mode is 4 or 8 or reward_salvage and
  morale_multiplier are both 0 (0x3B4B76-0x3B4BE3). Save the Guerrilla Camp is reward 0 with a checkpoint, so
  `World+0x2578` may stay 1 in free roam afterwards (open).

### 4. Restore lists: mission and activity starts regrow buildings (gaps 1, 5) [V/P]

- `restore_list_do_restore` 0x375180 (`cdecl(unsigned key)`, prologue `8B 15 <abs32>`, 6 bytes) finds the
  `All_restore_lists` entry (0x1257518, `farray<restore_list,128>`; `{+0 key, +4 count, +8 unsigned* list, +0xC
  restoring}`) with that key, sets `restoring` and calls `world::restore_object` 0x534B70 (thiscall) per handle.
- `restore_object` queues into `Pending_restorations` (0x1269B68) **only if `Hierarchy` holds the handle** (the object was
  destroyed). The regrow is per zone, not per object: `world::restore_do_frame_pre` puts each pending handle's zone in
  `Zone_table` when that zone is loaded (state 2); `world::restore_do_frame_post` re-serializes those whole zones from file
  (`world::restore_serialize_zone` at 0x540359 / 0x5403B3). Both are skipped while `World+0x265C` bit 0 is set. Handles in
  unloaded zones stay pending until the zone loads.
- `restore_list_restore_complete` 0x3751E0 (waited on by `mission_setup_do_frame`) checks only immediate entries (loaded
  zone, or hierarchy-node flag 0x1) and returns true at once when no list matches.
- Missions: only on a fresh start (not a checkpoint restore). Activities: `activity_base::start_object_restoration`
  0x37EE30 (call 0x37EE3F) with `activity+0x430`; `activity.xtbl` restoration_time At Init 92 / At Start 23.
  `Action_callback_restore_object` (call 0x721EB6) restores single objects.
- Registration: `register_restorable_object_list` 0x731710 via `Action_callback_mission_restore_object` 0x731820 and
  `Action_callback_activity_restore_object` 0x731850, from always-on (unmanaged) script groups.
- Counts: 18 `mission_restore_object` actions (17 missions plus `terr01_ambush`) registering 171 handles (e.g. Start Your
  Engines 37, Highway To Hell 29, Refugee Truck 20); 67 `activity_restore_object` actions (63 `je_*` + 4 `ls_ad_*`), not 39.
  DFA: key 7A050002 (its start node), 10 buildings in base zone terr01_08_10, e.g. 8A0004EF = `0101house_corp_exec_A`
  (`terr01_dfa.scriptx` lines 7-50); "dozens" was wrong. Restore targets are base-zone `rfg_mover` / `general_mover`.
- `restore_list_system_save` / `_load` 0x3750E0 / 0x38FB30 put the lists in the blob; whether the load restores every list
  with its handles is open.

### 5. Managed layers and what mission scripts point at (gaps 1, 5)

- `missions.vpp_pc` (318,686 B): 21 `.str2_pc` + `mission_containers.asm_pc`, together **59 `.layer_pc` and nothing
  else** (no meshes, textures, chunks or scripts). `activities.vpp_pc` (811,924 B): **88** `je_*.str2_pc` + asm, 204
  layers (incl. 7 `ls_ad` containers). Every layer is ZONE v36, flags 0x4, all parsed exactly to EOF. [V]
- Mission layers: 2,786 objects. Navpoint 992, squad spawn node 483, dummy 325, trigger_region 220 (no kill regions),
  guard node 179, spawn region 140, patrol 122, vehicle spawn node 114, bounding box 79, npc spawn node 45, action node 32,
  turret spawn node 30, **object_effect 10**, player_start 8, **Item 4**, restricted area 3. **No rfg_mover, building,
  vehicle or human.** [V]
  - The visible ones: 9 `env_building_smoke` plumes in Save the Guerrilla Camp (0xC8830064-0xC883006D), the PartyTime
    `gp_nano_pedestal` 0xAA800035 and Nano Forge pickup 0xAA800013, an Intro 2 ammo box 0x5A82003D, 2 Tutorial salvage
    pickups. [V]
- Activity layers: 2,555 objects; the visible ones are all Demolitions Master: 22 explosive-tank `general_mover`s
  (je_dm_04/14), 26 `propane_tank` Weapons (je_dm_05/10/15), 5 ammo boxes (je_dm_07/12/16). No script references the DM
  tanks. [V]
- **Layer handle = file handle** [V]: `(XX<<4|YY)<<24 | NNN<<16 | index`, NNN = the layer number 128-143; base-zone
  objects have byte 2 < 0x80 (`object_is_from_managed_layer` 0x61BDC0 tests `obj+0x6E >= 0x80`).
  `serialize_layer_objects` → `world::serialize_layer` 0x53EB10 → `world::visit_stub` 0x53B6F0 →
  `alloc_object_stub_preexisting` (0x53B70E) keeps it. But a layer object exists only on a machine that loads the layer.
- Layer API, separable from scripts [V]: `managed_layer_set` (0x98: +0 `farray<managed_layer_info,16>` with
  `{file_name, layer_grid_id}`, +0x90 srid, +0x94 flags). `start_layer_loading` 0x39D4E0 (`stream2_request`, cap 3 small /
  1 large via `get_slot_count` 0x38D5E0), `layer_resources_loaded` 0x35E5F0, `serialize_layer_objects` 0x38D900 (adds to
  `All_manager_layer_objects` 0x1256838, max 512), `start_layer_unloading` 0x38D840 (`stream2_release`, destroys the
  set's objects). None reads a `Mission_*` global. Activity sets: `activity_base+0x440 → base_activity_data+0x28`.
- Script targets (76 managed `.scriptx`, 7,790 object references) [P]:
  - restore, `building_destroyed`, `num_buildings_destroyed`, `delete_chunk`: base-zone `rfg_mover`/`general_mover` only;
  - `objective_highlight_add_list`: 140 base objects + 19 layer `object_roadblock_node`s (je_rs_03, je_rs_06);
  - `add_mission_objective_icon`: 72 of 79 targets are layer objects (trigger 36, dummy 26, spawn/action nodes,
    navpoints), 7 base;
  - `mission_add_ground_marker_offset`: 40 of 40 are layer trigger regions;
  - PartyTime `remove_effect` / `effect_spawn_at_script_point`: layer 0xAA800035 / 0xAA800013;
  - kill objectives: runtime squads, NPCs and vehicles spawned from layer nodes.
  - Gap 1's count over icon and highlight actions: 296 handles = 163 world objects, 114 layer objects, 19 neither. 42 of
    the 114 sit inside `get_squad/vehicle/npc_from_spawn_object`, so the real target is a runtime spawn the mirror
    carries. **72 of 296 (about 24%, not 39%) are layer objects highlighted directly**: 36 trigger regions, 26 dummies,
    4 squad spawn nodes, 2 vehicle spawn nodes, 2 action nodes, 2 navpoints.
- Zone residency [P]: of 663 distinct mover targets, 423 are in streamed-only `terr01_XX_YY` zones; only 240 also appear
  in the `p_terr01` persistent copies. A base-zone handle exists on the guest only while that zone is loaded there.
- **Spawn nodes spawn on their own** [V]: `object_spawn_node_do_frame` 0x6AC5E0 (first call of `gameplay_do_frame`)
  evaluates every npc/vehicle/squad/turret node, layer nodes included; `object_squad_spawn_node::evaluate_spawn` 0x66CFB0
  rolls `fl_rand` against `spawn_prob` (+0x9C) / `night_spawn_prob` (+0xA0); gated by `Spawn_squad_disabled` 0x2C2E15F.
  Data at spawn_prob 1.0: mission layers 136/483 squad, 73/114 vehicle (19 at 0.5), 28/30 turret; activity layers
  161/169 squad, 28/28 turret, 44/51 vehicle; **base zones 78 turret nodes and 305 vehicle nodes**.

### 6. Model reservation and `Mission_mode` (gap 5) [V/P]

- `Mission_mode` 0x334146E has three writers: `srm_level_init_pre` 0x752688 (=0), `srm_mission_reservation` 0x75E06E (=1),
  `srm_mission_release` 0x75E090 (=0). `srm_mission_reservation` 0x75E020 callers: `mission_activate_post_save` 0x39D73A
  and `cs_play_internal` 0x30FFEA (engine cutscenes, also via `cutscene_load_process` / `cs_play_job`). Release:
  `mission_terminate_cleanup` 0x3B04C3, `cutscene_stop` 0x30C6F1.
- `resource_slot_list::reserve_slot` 0x7520D0 accepts only types 1..16, and only while `Mission_mode` is 0 (check at
  0x7520F1); types 0 and ≥ 17 are always rejected. With `Mission_mode` = 1, `reserve_slot_player` and `unreserve_slot`
  are no-ops too.
- `srm_reserve_slot(type, user, human_info*)` 0x75D410 → `local_reserve_slot<human_info>` 0x758D70 can still fail: owner
  (+0xC) must be 0 or the user id; `slot_max` (+8; MISSION = 24) via `count_used_slots`; `human_info+0x30` must index
  `Human_records_list` (stride 0x18); `free_low_priority_slots(1)` may free nothing; `other_variant_loaded` can reject.
  `resource_slot_limit` table 0x12B7B00 (0x14 × 17) is filled at runtime. Vehicles: `srm_reserve_slot(…, vehicle_info*)`
  0x75D480, `Vehicle_records_list` 0x12B7AD0, capacity unread. `srm_reserved_human_info_list` 0x752A00 lists reservations.
- `srm_mission_reservation` first runs `clear_all_reservations_except(3 humans / 10 vehicles)` (wipes ambient
  reservations), then `_human` 0x75D730 / `_vehicle` 0x75DB80. The random pick (`int_rand_range` at 0x75DA28) applies only
  to groups with more members than reserved, preferring already-loaded records; single-member named groups are
  deterministic. Faction population goes through `squad_progression_reserve_members`.
- Every named mission character (Baker, May, Samanya, Dan, Roth, Kyle, Carmen, Camie, the DBC VIPs, the SH sniper, the
  pilots) is a row of the 146-row `character.xtbl` [V1: not re-checked by the skeptic].
- `human_voice_load_set` 0x659340 (`bool cdecl(int)`) on `mi+0x30`, polled with `human_voice_set_streamed` 0x631E60, is
  self-contained [V1].

### 7. Runtime creators and the population drivers (gap 4) [V]

- Every runtime `npc` is created at one site, `spawn_npc` 0x747F40 (create at 0x748132). **Params in EAX**, list on the
  stack; prologue `83 EC 30 55 8B 6C 24 38` then `56 8B F0`. Its 14 callers all root at `sp_do_spawn` 0x75C670, whose only
  caller is `object_spawn_do_spawn` 0x75D220 (0x75D2C0).
- Every automobile/flyer/walker is created in `create_new_vehicle` 0x748570 (create at 0x748680), reached only through
  `sp_spawn_vehicle` 0x756030, whose only caller is 0x757BF7 (`E8 34 E4 FF FF`) in `object_spawn_vehicle` 0x757B50.
- Exceptions: dead `create_object<npc/automobile/flyer/walker>` wrappers, the debug `unused_dcf_add_passenger`, Wrecking
  Crew, MP packet handlers, and **zone/save restore** (`serialize_zone` → `create_object_from_stub` 0x53E4B0;
  `npc::serialize`, `vehicle::serialize` exist). Persistence bit: `obj+0x55` bit 5.
- Ambient peds reach `spawn_npc` (`amb_spawn_do_frame` 0x75F410 → … → `object_spawn_do_spawn` → `spawn_npc`); traffic
  reaches `sp_spawn_vehicle` (`spawn_single_vehicle` 0x75EA70 → `object_spawn_vehicle`). Initial ambient population at
  load: `world::post_load_stream` 0x54121F → `gameplay_level_init_spawn` 0x35E570.
- Squads: `squad_spawn_parameters::spawn_squad` 0x3B0D60 (thiscall, prologue `56 8B F1 8B 46 78`, 21 call sites) creates
  the `object_squad` (via `create_squad` 0x375550, create at 0x375710) **before** its members. With only `spawn_npc`
  refused, every squad source leaves an empty `object_squad`.
- **`gameplay_do_frame` 0x3B9770 has one caller, 0x3EDF23** (`E8 48 B8 FC FF`, re-read). In order: spawn nodes
  (`object_spawn_node_do_frame`), `srm_do_frame` 0x75D300, `gsm_do_frame` (alert), raids, `activity_system_do_frame(0)`,
  a `!Multiplayer && !cutscene` block (sdc, reinforcements, rfc, continuous support, gfm orders, drop-offs, ssm, smash
  coordinator), `object_mission_start_node_do_frame` 0x6B3F00, roadblocks, morale, ambient props, action-node objects,
  ambient peds and traffic, ambient actions and ambient EDF.
- Drivers outside it in `gamestate_gp_simulate_frame`: `convoy_do_frame` 0x3EDF06 (`E8 65 59 2C 00`),
  `safehouse_do_frame_postsim` 0x3EE19D (respawns safehouse vehicles through `object_spawn_vehicle`),
  `trigger_region_do_frame` 0x3EE1E7, `ambush_region_do_frame` 0x3EE1EC, `safehouse_vip_do_frame` 0x3EE1F1,
  `mission_system_do_frame` 0x3EE1F6, `script_do_frame` 0x3EE1FB, `public_address_kiosk_do_frame` 0x3EE209,
  `activity_system_do_frame(1)` at 0x3EE4DD (`6A 01 E8 1C E6 FB FF`).
- Load-time and stream spawns: `object_spawn_do_spawn()` 0x6B42E0 from `setup_territory` 0x540AED;
  `safehouse_vip_level_init_post` 0x3906B0; `player_level_init_post` 0x6D5C9D → `create_player_squads`;
  `object_squad_spawn_node::stream` 0x69427D; `stream_notify_spawn_squad` 0x35F310 / `stream_notify_load` 0x3904F0 (no code
  refs; called through a data table). All end in `spawn_squad` / `spawn_npc`.
- **Raids are not activities** [V]: the gate 0x3B977F-0x3B97BB (`Current_alert_level` 0x1E268B4 ≤ -1 or 0..2 and no
  mission) runs `do_frame_spawn_new_raid` 0x38DF00 (night only, `fl_rand() < Marauder_raid_prob`, sets
  `Raid_patrol_handle` 0x12504F0) or `do_frame_spawn_selected_raid` 0x3B4790 (→ `spawn_squad`, `Raid_squad_handle`
  0x12504F4). `activity_suspend_activities` 0x356550 only ORs `1<<reason` into `All_activities[i]+0x424` and calls vfunc
  +0x40; nothing in the raid chain reads it.

### 8. Turrets and roadblocks (gaps 4, 5) [V/P]

- Turret creators: `vehicle_build_turrets` 0x7A87E0 and `object_turret_spawn_node::spawn_child_object` 0x66D700 (create via
  `turret_instantiator` at 0x66D85B). Neither `spawn_npc` nor `sp_spawn_vehicle` is on the path, so the refusals and the
  NPC/vehicle sweeper do not cover turrets. Base zones alone have 78 turret nodes at spawn_prob 1.0.
- Roadblocks: `object_roadblock_node::place_turrets` 0x63DC00 creates `object_turret_spawn_node` objects (create at
  0x63DD44), not turrets; `place_barricades` 0x6B3F70 creates `object_dummy`s; the destructible piece comes from
  `create_barricade` 0x663020 (`cdecl(object_dummy*, c_chunk_base*)`) → `object_mover_create_from_chunk`, plus more
  `create_object_internal` calls (0x663347 / 0x66346E). Layout from `roadblock_formation.xtbl` (barricade_pos_info ×17,
  turret_pos_info ×10).
- Triggers: `Action_callback_spawn_roadblock` 0x721520 (30 uses) → `roadblock_spawn` 0x6BA5B0 →
  `spawn_roadblock_dynamic` 0x6BA1A0; `object_roadblock_node::start` 0x6BA590 (dynamic when `+0x164` bit 0 is clear);
  `object_roadblock_node::stream` 0x6B3F30 → `spawn_roadblock_objects` 0x6AB040 (bit set) → `create_dummy_object` 0x6A6BD0 →
  `create_barricade`; `activity_house_arrest::do_spawn_roadblock/do_spawn_post/do_spawn_barricade/do_spawn_pre`;
  `activity_raid_data::spawn_raid_objects` / `spawn_raid_object_dummy`.
- The roadblock node is itself a layer object; all 30 `spawn_roadblock` uses and the 19 highlights point at it. The
  created objects appear to get it as parent (`[node+0x30]+0x6C` passed to `create_object_internal`) [I]. So "node
  handle + formation index" is a workable identity rule.

### 9. Consequences of replayed destruction and kills on the guest (gap 4) [V/P]

- `rfg_mover::do_frame_report` 0x31F2D0 is called from itself (0x31F32A), `rfg_mover::create_subpieces` 0x33DDD9 and
  `rfg_dyn_shardify_cleanup` 0x33FE78, so the guest's replay runs it. It calls `drop_control` when damage `+0xFC` ≥ the
  threshold `+0x108` and `+0x10C` bits 0x20000 / 0x10 are clear, plus `set_game_destroyed` 0x316DF0,
  `set_game_destroyed_common` 0x316CA0 (**mover in EDI**), activity and sdc hooks.
- `object_mover::drop_control` 0x6139E0 (thiscall; also from `general_mover::apply_damage` 0x68AC6C): needs `+0xE0` bit 25
  clear and team `+0xCC == 1` (EDF); adds to the destroyed table when `+0xD0 > 0` or `+0xC8`'s `+0x8 > 1`; only when
  `+0xD0 > 0` calls `gfm_change_edf_control(-control, district, …)`.
- `gfm_change_edf_control` 0x3B0010 (8 callers: `drop_control` ×2, `apply_standard_rewards`,
  `activity_assassin_data::monitor_bureaucrats`, `activity_tanks_a_lot`, `Action_callback_lower_edf_control(_at_object)`,
  `unused_dcf_edf_control_change`): returns when `Multiplayer` (cmp at 0x3B0024, `jne 0x3B0043` → epilogue 0x3B035B) or
  `Wrecking_crew_state != -1`. Otherwise `gfm_set_edf_control` 0x3AFF40 and more.
- `gfm_set_edf_control` and `district::set_control` 0x3ABD90 have the same body: unless `Loading_data` (0x1E28678), return
  if control is already 0; clamp to [0, `+0xA4`], store `+0x9C`; then unless `Loading_data`:
  `district::liberate_district`, `mission_unlock_capstone_in_district`, `mission_notify_new`. **`set_control` is not a
  live setter.** Its only caller is `object_district_load_districts` 0x3AC061 (under `Loading_data`).
- `district::liberate_district` 0x3A5C70: nothing if `+0xC8` is set or `Loading_data`; liberates only when control == 0
  and `missions_left_in_district(h,1) == 0` (on the guest's own mission state). Then progression and tech level,
  `object_mover::check_liberation`, `upgrade_unlock_from_liberation`, `mission_unlock_from_liberation`, stats, PA,
  riding shotgun.
- `upgrade_unlock_from_liberation` → `hud_ui_district_liberation_show` (0x390E91; sets `Push_state`, `District_handle`,
  `Liberation_screen_type`). `hud_ui_district_liberation_process` 0x456D80 calls `world::save_and_warp` (0x456E24) **only**
  when the player dismisses the screen, the district's name is "Dust" (VA 0x12EEF88) and
  `safehouse_find_safehouse_by_grid_id(0xC800)` finds the safehouse; otherwise it pops the state. `save_and_warp` does
  nothing with a mission active, `World+0x24E0 != 0` or `World+0x265C & 0x24`; else `save_and_reload`.
- A second liberation path that bypasses `gfm_change_edf_control`: `mission_info::mission_completed` (0x3ABB9D) →
  `unlock_new_missions` → tail jump to `district::liberate_district` (0x3ABB4D). Covered by "no mission on the guest".
- `world::object_add_to_destroyed_table` 0x51C830 is pure (thiscall, `World+0x4C8` insert); callers
  `set_game_destroyed` 0x316F0A and `drop_control` 0x613A44.
- `general_mover::apply_damage` 0x68A920 (vfunc, `bool thiscall(float, damaged_by_types, vector*, vector*, object*,
  unsigned, bool)`): gate `cmp [Multiplayer],1` at 0x68A923 → `multi_is_server` 0x5431D0 (`mov al,[Server 0x2BEA590]`) →
  last bool `[esp+0x3C]`; otherwise returns 1 (`ret 0x1C`). Past the gate: destroyed-event queue 0x68AC5B,
  `activity_register_death` 0x68AC62, `drop_control` 0x68AC6C, `hud_ui_mission_set_building_destroyed` 0x68ACA1, damaged
  queue 0x68ACC8. Not covered by the `human_damage` / `vehicle_apply_damage` firewalls.
- **MP early-outs** usable as guest gates, each `80 3D <Multiplayer abs32> imm8` (7 bytes, operand at site+2):
  `gfm_change_edf_control` 0x3B0024; `activity_register_building_destroyed` 0x359DF0; `activity_register_building_damage`
  0x359D80; `activity_register_death` 0x359D30; `gfm_control_morale_human_killed` 0x3AB64A; `stats_register_human_death`
  0x3D4F33; `stats_register_building_damage` 0x3D4DC1; `player_meta_data::mining_destroyed` 0x611904;
  `supply_crate_destroyed` 0x611A74 (jne 0x611A86); `general_mover::apply_damage` 0x68A923 (compares with 1). Re-read: the
  0x3B0024 and 0x68A923 forms.
- No early-out in `alm_report_human_death` 0x39C5E0, `gfm_propaganda_morale_change` 0x3AB3E0,
  `morale_maybe_convert_civilians_spontaneously_around_event` 0x3B3DA0, `sdc_report_new_damage` 0x373990 or the event
  queues.
- `human_died` 0x6C7890 (reached by the planned DIE replay) runs morale, stats, civilian conversion, alert, activity
  death, turret abandon, `cartographer::remove_icon`, `gem_post_event`, and **`vehicle_queue_damage(v,…,0x7FFFFFFF)` at
  0x6C7E47** when the victim pilots a flyer (seat 0 of type 3 / subtype 5).
- Salvage: `salvage_should_spawn` 0x61F0D0 is false in MP, WC, Demolitions Master (0xB), Area Defense (0x10), satellite
  camera (mode 0xC), Riding Shotgun, or DLC-disabled; called in `rfg_dyn_shardify_work` (0x33CE4C, items at 0x33CFE1) and
  `explosion_create_internal`. A guest with no activities spawns salvage where the host may not.

### 10. Loads, saves and videos reachable on the guest (gap 4) [V/P]

- `world::set_pending_territory_load` 0x51C320 (prologue `56 8B F1 E8 rel32`) is the only `gameseq_set_state(GS_LOAD)`.
  Guest-reachable callers: Esc → Load (`verify_gamesave_process` 0x3F0354), Esc → Restart (`set_pending_reload` 0x51C438
  ← `ingame_options_handshake` 0x4AF641), Esc → Cancel mission (`mission_cancel`, only with a mission active), death →
  Continue (`set_pending_load_from_memory` sets `World+0x265D |= 4` → `process_save_load` 0x3C016C; mode 4 calls 0x51C320
  directly at 0x49303A), fast travel (`fsm_warp_to_safehouse_cb` 0x454484 → `save_and_warp` → `save_and_reload`), and a
  warping video (`video_cutscene_exit_state` 0x306F15 / 0x306F4D).
- **A detour there is not a side-effect-free backstop**:
  - Esc → Load first calls `difficulty_set_level_reload_overwrite(-1)` and `world::platform_load(Pending_save,1)` at
    0x3F032E (clears `World+0x2578`, writes `World+0x2694`, `stats_turn_off_all_cheats`, sets hammers, backpacks and
    jetpack from the save; a missing save goes straight to GS_MAINMENU) before reaching 0x51C320 at 0x3F0354;
  - in `video_cutscene_exit_state`, a false return sends the game to `GS_MAINMENU` (0x306F26, 0x306F5A);
  - `mission_cancel` and `ingame_options_handshake` run their own side effects first.
- `world::save_and_continue` 0x51D040 (thiscall `ret 0xC`; first 9 bytes `56 8B F1 8A 86 5C 26 00 00` clean) reachable on
  the guest from the free-roam death branch 0x3EDCF0, `hud_ui_upgrades_exit` 0x44B432 and the save/load UI; the
  mission, activity and video callers need local missions, activities or a warping video.
- `video_cutscene_play` 0x30A4A0 callers: handbook 0x46940A and upgrade shop 0x47FDA4 (both warp -1), menus, and script
  actions. `video_cutscene_do_frame` 0x307170 saves (`save_and_continue_warp` 0x51D236) and loads only with a warp object,
  a warp zone, a DLC level or credits. **A video with warp -1, zone 0, DLC 0 and no credits never loads or saves.**
- `gamestate_respawn_player` 0x3BFEF0 is called only at 0x3EDD52 under `Multiplayer==1 && multi_is_server`.

### 11. Geo-Mod determinism: the random number generators (gap 2) [V/P]

- `Vlib_rand_instance` 0x123A2B8 (10,696 B): `Mt[624]` +0, `Mti` +0x9C0, **`m_next_rand` +0x9C4**, `m_rand_array[2048]`
  +0x9C8. A draw reads the index, reads the pool entry, increments and stores the index (wrap at 0x800). The Mersenne
  Twister is never touched by a draw. The read/increment is **not atomic**.
- Readers of the index: `vrand` 0x17A2D0, `fl_rand` 0x17A300, `int_rand_range` 0x17A380, `fl_rand_range` 0x17A3C0,
  `int_rand_chance` 0x1A5C30, `timestamp::set_rand` 0x18E5A0, plus `bool_rand`, `fl_rand_chance`,
  `vector`/`vector4::rand_sphere`, `rand_around_z`, `timestamp_realtime::set_rand`, `net_lag_get_random_lag`,
  `net_lag_should_be_lost`.
- The pool is filled three times at boot: the static initializer (0xC5FD90), `vlib_system_init` (inline at 0x1CFCA3), and
  `rand_regenerate(timer_get(1000000))` (0x17A290, `cdecl(seed)`) from `rfg_init_stage_2_done` at 0x1D5771. `rand_init`
  0x17A250 has no callers. Final contents are clock-seeded and differ between machines.
- **The render thread draws from the same index**: `rl_gpu_vis_get_state` 0x10F9F0 calls `int_rand_range(0,
  RL_gpu_vis_delay_rand)` for occlusion queries, live by default (`RL_gpu_vis_delay` = 3, `RL_gpu_vis_delay_rand` = 2),
  from `rl_scene_renderer::render` via `perform_visibility_pass`. VFX particle code (`vfx_effect::update_render`,
  `vfx_renderable_filter_instance::update`) and `rl_meteor_shower` also draw. So "same pool + the host's index per step"
  does not hold at runtime: the guest needs a **private stream** for the apply (swap pool and index around it, or detour
  the helpers for the apply window).
- CRT `rand`/`srand` come from MSVCR100 (import slots 0xE3F3F8 / 0xE3F3FC): a per-thread LCG at `_getptd()+0x14`
  (`_getptd` exported, ordinal 662). `srand(timer_get(1000000))` at 0x1D23C8 runs on the main/render thread
  (`rfg_init_stage_2_done_2` from `Application::render`) and does not seed the sim thread. On the sim thread
  `do_game_main_loop_frame` calls `rand()` every frame (0x1D263C), `create_cells` 0x532EF9 reseeds per cell, and Lua
  `math_randomseed` 0x82A500 (srand at 0x82A532) lets scripts reseed. `crypthead` (0xD8E6) also calls srand.
- CRT draws in the apply: slice axis `rand()&1` 0x339AE1 (`rfg_dyn_slice_shape`), uber explosion type `rand()%100`
  0x33A0C3, stress siblings 0x331201, and audio variation (0x9F638, 0x89F57) reached from `camera_shake_start_from_impact`
  at 0x339E38 (camera-distance dependent).
- What the pool draws decide [V]: the three `int_rand_range` calls in `rfg_dyn_create_shard` (0x337CE5, 0x337DDB,
  0x337DF1) are **fade lifetimes** (1000-5000 ms, `object_mover_mark_for_fade`), not shard counts; also slice cooldowns,
  shatter impulse scale (0x33280B, SP only), shard torque (0x312A04), salvage roll (0x33CFB9) and amount (0x641590),
  dust trails, stress scheduling. Only the SP immediate-slice draws touch geometry.

### 12. Geo-Mod determinism: per-machine inputs and the MP "profile" (gap 2) [V/P]

- **SP debris load balancer** (`rfg_dyn_shardify_setup` 0x338040 → `rfgl_desired_values` 0x31D000, one caller 0x3383FD,
  `cdecl(rfgl_balance_values*, int count, int flags)` returning {quality +0, delete +4}):
  - gated by `RFG_load_balance` (0x124F0FB, default 1);
  - `havok_get_status()` non-zero → quality 0, delete all;
  - `delete = max(trunc(Recent_history_objects × 0.015 × L² × count), count − 20)`;
    `quality = min(count − delete, Max_quality_objects_per_frame (10) − trunc(Recent_quality_objects))`;
  - L = 1.5 when flags & 0x61, else `RFG_load` = last Havok sim time / 26250 + `Recent_history_objects` / 100, clamped
    (1.0 when the Havok status is bad), written in `rfgl_do_frame` 0x325114 from `Gameplay_last_havok_simulate` (a timer
    around `havok_simulate`, 0x3EE07C-0x3EE099); the history values decay 0.95 per frame;
  - flags 0x20 (distance² from `Rfg_camera+0x2C` > 5625, i.e. 75 m, 0x338142) and 0x40 (`!game_render_is_object_visible`,
    0x33817B) are set in SP only (`cmp [Multiplayer]` at 0x33811A).
  - It decides only how many already-produced small pieces are deleted or kept as quality debris (which ones: deterministic
    striding). The piece count itself comes from the fracture. With `Recent_history_objects` = 0 (a quiet scene),
    `delete = max(0, count − 20)` does not depend on timing. Whether it or the RNG is the *main* divergence is not shown.
  - Move type: SP `(flags & 0x861) ? 4 : 3` when the piece flag 0x10 is clear and its size < 10, else 1 or 2; MP 4
    (`get_move_type_for_new_shard` 0x312500).
  - MP: the **server still runs** `rfgl_desired_values` in an MP mode (mask 0x1, quality forced 0, delete < 5 → 0, capped
    0xFFF) and records counts in `Mover_destruction_results+0x100`; only clients use
    `multi_game_destruction_get_client_balance_delete_count` 0x5550B0 (at 0x338442, reads MP-only
    `Active_destruction_frame_data`). Unreachable with `Multiplayer` = 0.
- **Immediate slice** in `rfg_dyn_apply_single_object_collision` 0x339D70 (SP only): `Slice_min_damage` (0x124F2E8),
  subpiece count > 100, `rfg_dyn_can_slice` 0x312C50 (`Framecount` delay ≥ 10), `havok_body_get_linear_velocity` at
  0x339F43, `fl_rand_range` 0x339EF5; then `rfg_dyn_slice_shape` (rand, 2× `fl_rand_range`, `set_rand`) and an immediate
  apply (0x339BCD / 0x339BD9) with no queue entry.
- **Uber-heavy explosion** (SP only): `Explode_min_damage` (0x124F2F0), `Framecount` gate (0x33A022-0x33A033),
  `int_rand_chance(RFG_uber_heavy_destruction_explosion_chance 0x124F324)`, `rand()%100`. Its result flag also feeds
  `rfg_dyn_apply_stress_to_subpiece`, so it changes stress (not only an effect).
- Multiplayer skips both: 0x339EA3 → 0x339FE9 → 0x33A124 (plain stress apply). The writable `.data` tunables above are an
  alternative switch.
- A third, timer-driven slice in `rfg_dyn_do_frame_pre` (0x33A8FF; `Timestamp_ticker` + 2 `fl_rand_range`) fires only
  after an earlier slice armed `rfg_mover+0x120`; with 0x339EA3 patched nothing arms it in SP [I].
- **Shardify list order**: `alloc_rfg_mover_process_shardify_list` 0x324930 keeps insertion order in SP; MP qsorts at
  0x3249AC-0x3249C8 with `rfg_mover_list_compare` 0x314FF0 (uid `+0x134`, then `+0x6C` when uid == -1; `+0x6C` may differ
  between machines for non-zone movers).
- **Out-of-queue deleter**: `rfgl_do_frame` 0x325050 (tail-called from `rfg_do_frame_post`; MP non-servers return at
  0x325061 after the cmp at 0x325051) runs `rfgl_maybe_delete_objects` 0x324C10 when `RFG_load_delete_stamp` elapses and
  `RFG_load_balance` is set. Candidates: general movers (`World+0x3CC`), rfg movers (`+0x3DC`), `object_debris` (`+0xCC`);
  choice by `rfgl_should_delete` 0x3154A0 (camera distance, active body, fade and script-effect flags, visibility).
  Visible → `mark_for_fade` (an `int_rand_range(0,2000)` fade length), invisible → `world::destroy_object`. MP servers run
  `mp_destruction_sync_throttle` 0x324CB0 instead (caps `Max_mp_throttle_active_sync_movers` 10 / inactive 20 at
  0x124F34C / 0x124F350).
- Shatter: MP uses impulse × 1.0 (0x332772) instead of `fl_rand_range(0.75,1.25)`.
- **MP-profile gates** (every one is a 7-byte `cmp byte [Multiplayer],0`, operand at site+2; forms re-read): 0x339EA3
  immediate slice, 0x339FE9 uber explosion, 0x33811A camera/visibility flags, 0x3249AC sorted list, 0x332772 shatter
  impulse, 0x325051 `rfgl_do_frame`. The four profile branches read and write no MP-only state. **Never open**: the
  `create_shard` sites (0x337CBF / 0x337D98 / 0x337EE5), `rfg_dyn_do_mode_specific_processing` 0x312670 (dereferences
  `mp_info`), or the client path at 0x3383CF (reads NULL `Active_destruction_frame_data`).
- Explosions: `rfg_dyn_apply_explosion` 0x333030 reads radius, impulse and structural damage from the static
  `Explosion_infos` table (`explosion_lookup_by_unique_id` 0x2E20C0); no RNG [V1]. Owner gate:
  `rfg_mover::player_damage_only` (0x3330BE, 0x33A1D5) needs the owner to resolve to a player object.
- **Stress is queued** [V]: the state machine is inlined in `rfg_dyn_do_frame_pre` (0x33A9B6-0x33AA0F, gated by
  `gameplay_should_run_stress` 0x374650); it writes type 0xC (0x329340, 0x3314AD, 0x3319D3) and type 5 SHATTER (0x331130)
  under the queue lock. `rfg_do_frame_pre` (0x3EDF0B) runs before `havok_simulate` (0x3EE088), so the host capture at
  0x4102E0 sees stress. MP: `multi_game_destruction_should_run_stress` 0x555970 returns true; queue writes are gated by
  `multi_is_server`.
- The apply snapshots the event count (`Num_events_to_process_this_havok_step`, 0x340787-0x340793) and resets
  `Num_rfg_dyn_collisions` at 0x340A2B; events appended during the apply are dropped. [V1]
- **No worker threads** [V]: `process_setup` 0x33ADF0, `process_work` 0x33D490, `swc_process_all` 0x340650,
  `process_cleanup` 0x340630 have no code refs and their VAs appear nowhere as dwords; the queues are never written.
  `postCollideCallback` (vftable 0xEF8C48) fires from `hkpWorld::stepDeltaTime`, called only by `havok_simulate`
  (0x43F372) on the sim thread; `stepMultithreaded` has no callers.
- Chunk uid: `Rfg_mover_destruction_uid` 0x1A3733C, incremented (lock xadd) in `rfg_dyn_shardify_work`, reset per level,
  never saved. [V1]

### 13. Chunk physics and poses (gap 2) [P]

- `havok_simulate` 0x43F2D0: with `Havok_use_single_step` (0x125EB5B) = 1 (the shipped value; only writer
  `unused_dcf_Havok_use_single_step`, which also writes the step time into `Havok_world+0x78 → +0x20F4`) it takes one step
  with dt = `Frametime`. With 0 it takes floor(accumulator / (1/60)) steps of 1/60 s and carries the rest. So variable dt is
  a default, not a law.
- `Frametime` comes from `frametime_calculate(false,0,0)` in `rfg_do_frame`; `frametime_set_cap(1/300, 0.1)` in
  `rfg_init_stage_1_prepare` overwrites the `.data` 1/120 and 0.25. The minimum is a frame limiter (sleep/spin), the
  maximum a clamp. `frametime_calculate` also takes a fixed-fps argument.
- Even with a fixed dt, chunk poses diverge: each machine runs its own world with different per-frame inputs (players,
  vehicles, AI forces, step counts, RNG). A host-authoritative **pose lane** is needed [I, consistent with stock MP].
- `rfg_mover_multi_registration::apply_compressed_state` 0x5FA1E0 (thiscall `ret 0x14`, only via the vftable at
  0xF0AF74) cannot run in SP: its `dc_state_list` pool is set up only by `multi_core_dc_init` ← `multi_init`
  (0x5FD5CC); `rfg_mover+0x110` (`mp_info`, allocated only when `Multiplayer` == 1) is dereferenced with no NULL test
  (0x5FA4B5-0x5FA4C3); and it only queues a state through `multi_sim_state::add_state`.
- Usable setters (cdecl, handle-based): `havok_body_set_pos_orient` 0x423790, `havok_body_set_linear_velocity` 0x423870,
  `havok_body_set_angular_velocity` 0x4239A0. `compute_object_mover_throttle_score` 0x31CDB0 is a scoring model.

### 14. Puppet SP safety audit (gap 3) [V/P]

Method: a CFG walker over direct calls from `multi_game_player_process_state`, `player_process_all`, `player_do_frame`
and `human_do_frame` with SP constants (depth 7: 1,616 functions in puppet mode, 2,013 in plain SP mode; same MP-pointer
set). Under-approximation: 193 indirect calls up to depth 3 are not followed (e.g. `weapons_test_bullet`,
`world::destroy_object`, `human_ragdoll_*`), nothing past depth 7, pruned functions not scanned.

- **Crash** [V, re-read]: for a non-local player, `player_do_frame` falls through at 0x6E6975-0x6E6989, skips MP
  `process_state` at 0x6E698F, then after `human_weapons_process_human` calls `multi_game_stats_record_weapon_time`
  (0x6E6B70) and `multi_game_stats_record_backpack_time` (0x6E6B9C) with no `Multiplayer` test. Both start with
  `multi_game_stats_are_finalizing` 0x5862D0, which reads `[Mp_game_data + 0x610]` (0x5862E0 / 0x5862E6) with no NULL
  check. `Mp_game_data` (0x2BEA698) is written only by MP init/shutdown, so it is NULL in the campaign. The local path
  gates the same calls at 0x6E6E80. **Fix PP-STATS: 0x6E6B44 `8B CE` → `EB 5E`** (jmp 0x6E6BA4; stack balanced by the
  `add esp,0xC`s; no absolute operand). Wrecking Crew never hit this because `player_do_frame` returns for its extras at
  0x6E62AF.
- Other MP reads on the path [P]: `Mp_player_stats` in the record functions (not dereferenced while `mp_data` = -1, dead
  after PP-STATS); the handle hashes `Reserved_network_to_game_hash` (0x1290C60) and `Network_handle_to_rop_hash`
  (+0x40 / +0x44 at 0x1290BC8 / 0x1290BCC); `multi_object_backpack_instantiator` in `multi_game_backpack_create` 0x59ACC8
  (only when `Backpack_info[i]` != 0, behind `player::do_frame`); `multi_game_data_get` 0x552180 (NULL-checks at
  0x552186); `Lobby_session` in `gamelog_get_player_index` (0x3506D9 NULL check); **one `Server` read**,
  `multi_object_backpack::event_player_picked_up` 0x5F23FA (via `player::do_frame` 0x6D56D2; a bool, no crash).
- The critic's six "unchecked" sites are gated [P]: `multi_game_player_request_damage` (callers gated), the
  `multi_game_xp_trigger_event` overload 0x5E2A20 (it **is** called from SP code: `human_damage` 0x6CEC67,
  `process_bullet_hit`, `human_restore_health`, `impaling_collide`, `fire_penetrating_bullet`, `process_arc_welder`; it
  returns at 0x5E2A4F when `mp_data` == -1, at 0x5E2A93 with `Multiplayer` 0, and its `Multi_packets` read sits behind
  `Multiplayer && Server`), `event_player_attacked` (self-gated 0x5DF160), rubberbanding (`fld1` in SP), `human_damage`'s
  `Multi_match_info` (gated, .data struct), `multi_core_object_create_remote_object` (self-gated).
- **Handle tables** [V]: `network_to_game_handle::get_game_handle` 0x5D2A80 returns -1 for net id 0xFFFF and the cached
  dword when it is not -1; otherwise `multi_core_game_handle_from_network_handle` divides by the size and reads
  `buckets[hash]` with no NULL check (0x5B63A5-0x5B63AB), then `…_get_remote_object_…` (0x5B630D-0x5B6318). Priming
  means size = 1 **and buckets pointing at a real zeroed dword**, for both tables. `interpolate_state` 0x563B50 copies a
  whole state first (`rep movsd` 0x563BA5), so every state given to `add_state` must carry the cache dwords.
- `mp_current_command` (+0x1178; `forward` +0x117C, `sideways` +0x1180) [V]: the pad writers all resolve
  `player_get_local_player`; for a puppet only `apply_movement_state` writes them (0x5CF3C4 / 0x5CF3CD).
- **Local stick leak** [V]: `human_process_controllers_pre_integrate` 0x652B00 (all humans) scales an airborne
  player-class human's drive velocity by the local `Stick_h` / `Stick_v` (0x1A24764 / 0x1A2476C) in SP (0x653124 →
  0x65316E); the MP client path reads +0x1180 / +0x117C (0x65315C). `[esp+0x11]` = is player subtype, `[esp+0x12]` = is
  local. **Fix PP-AIR**: replace `A0 <abs32>` at 0x653124 with `jmp cave`: local → 0x65316E, else → 0x65315C.
- **"Is a player" used as "is the local player"** [V code / P details]:
  - crouch / stand: `camera_free_set_submode(4)` at 0x630B07 and `(5)` at 0x680783 (only when `Rfg_camera+0` == 0 and the
    current submode is 5 / 4);
  - `human_combat_abandon_turret` 0x6C0730: `camera_stop_turret_mode(1)` at 0x6C0A0F does something only when the observer
    is also in turret mode; then **one** of two branches by arg2: turret angles into `Rfg_camera+0x22C` / `+0x220`
    (0x6C0A46 / 0x6C0A54), or `camera_free_set_target(vehicle)` at 0x6C0A8E. `camera_set_view_player` there is gated on
    `esi == Player` (0x6C07F9);
  - `human_combat_man_turret` 0x6B5E30: `camera_start_turret_mode` is gated (0x6B5F73), but 0x6B5F89-0x6B5FD2 write
    `Rfg_camera+0x22C` and `matrix::set(Rfg_camera+0x50)` for any human whose turret info has `+0xA0 & 0x20` [V1];
  - `human_combat_play_combat_move`: in SP (0x67ED30 `cmp [Multiplayer],0`) 0x67ED64-0x67ED82 copy the flattened
    `Rfg_camera+0x68` into the acting human's heading (`+0x2D8` / `+0x2E0`) with no subtype test; MP uses the player's
    `+0x1250`. A puppet's melee lunge follows the observer's view;
  - minor: `human_set_steering_heading_offset` 0x62D185 (camera orientation as heading seed), first-person spinebend
    0x6B6826 (camera mode 8 only), burning-vehicle shake at 0x6E696D.
- Pad reads on the puppet path [V1]: gyro reset 0x6E62EE, cover button in `player_cover_do_frame` 0x69E90F, empty-weapon
  click 0x6CC184, multi-rocket trigger 0x7EBDA7.
- Upgrades and tooltips [V1]: every reached `upgrade_*` getter reads `player_get_meta_data()` (the local player's), so a
  puppet has the observer's upgrades; `human_do_equip_unequip` 0x6BC1AC shows SP tooltips (`ui_tool_tips_add_tip`
  0x6BC212) for propane, hydrogen, thermobaric and multi-rocket equips by any human.
- Puppet init values from `player::serialize` 0x6DEAD0 (init branch) [V1]: `mp_data` +0x126C = -1 (0x6DEB00), backpack
  +0x1170 = 0, +0x1174 = -1, +0x1268 = 0, +0xC4 bit 0 cleared in SP (0x6DEB78); `mp_sim_state::reset` only in MP. That the
  creation recipe reaches `serialize(…,true)` is [I].

---

## Patch list from this round

All on the named role only, saved and restored with expected-byte checks. "abs32" operands are relocated by ASLR: compare
against the runtime address. Guest fences go on at HELLO and stay on until the process exits (like the save guard).

| Id (report id) | Site | Bytes now | Patch | Role, milestone | Tag |
|---|---|---|---|---|---|
| GF-MIS (gap1 G1) | 0x39D69C | `A3 <abs32>` | 5 × NOP: the restore index keeps the -1 from `mission_system_level_init` | guest, M5 | [V] |
| GF-ACT | 0x38E560, 0x39D960 | `8B 44 24 08 50`, `51 56 8B 74 24 0C` | `C3` at both entries (backstop for every activation path) | guest, M5 | [I] (gap 5 proposed full detours here) |
| GF-SCR (gap1 S1, gap4 G3) | 0x3EE1FB | `E8 60 DE 34 00` | skip `script_do_frame`. The second caller 0x39D93D is dead while no mission is active; hook the entry 0x73C060 instead if GF-MIS/GF-ACT are ever relaxed | guest, M5 | [V] |
| GF-POP (gap4 G1) | 0x3EDF23 | `E8 48 B8 FC FF` | M5: write `Activity_node_start_on_load` = -1, then the original. M7a: call only `srm_do_frame` 0x75D300 and `object_mission_start_node_do_frame` 0x6B3F00 | guest, M5 / M7a | [V] |
| GF-CNV, GF-AMB, GF-VIP, GF-MSY, GF-ACT1 | 0x3EDF06, 0x3EE1EC, 0x3EE1F1, 0x3EE1F6, 0x3EE4DF | `E8 …` | skip | guest, M7a | [V] |
| GF-NPC, GF-VEH (gap4 G5/G6) | 0x747F40 prologue (EAX live), 0x757BF7 call | see §7 | refuse unless the mirror is spawning | guest, M7a (backstop) | [V] |
| GF-SQD (gap4 G7) | 0x3B0D60 prologue | `56 8B F1 8B 46 78` | return NULL; optional, measure first | guest, M7a | [V] |
| GF-EDF (gap4 G10) | operand 0x3B0026 | abs32 | → guest byte: no district control change from replay, general movers or scripts | guest, M6 | [V] |
| GF-GM (gap4 G11) | operand 0x68A925 | abs32 (cmp with 1) | → guest byte (= 1): local general-mover damage swallowed; damage with the last bool true still applies | guest, M6 | [P] (assumes `Server` = 0, probe) |
| GF-CONS (gap4 G12) | operands 0x359DF2, 0x359D82, 0x359D32, 0x3AB64C, 0x3D4F35, 0x3D4DC3, 0x611906, 0x611A76 | abs32 | → guest byte; optional (guest-local drift, never saved) | guest, M7a | [V] |
| GF-LOAD (gap4 G13) | 0x51C320 prologue | `56 8B F1 E8 rel32` | allow only the session's own load; else log the caller, return false, send REJOIN_REQ | guest, M5 | [P] (not side-effect-free, §10) |
| GF-SAVE (gap4 G14) | 0x51D040 prologue | `56 8B F1 8A 86 5C 26 00 00` | `ret 0xC` | guest, M5 | [V1] |
| GF-UI (gap4 G15) | UI | - | block Esc → Load / Restart / Cancel mission, fast travel and death Continue **before** their own side effects, with a message | guest, M5 | [I] |
| PP-STATS (gap3 G1) | 0x6E6B44 | `8B CE` | `EB 5E` | both, M3 (first E2 build) | [V] |
| PP-AIR (gap3 G2) | 0x653124 | `A0 <abs32>` | `E9 cave` | both, M3 | [V] |
| PP-CROUCH (C1, C2) | 0x630B07, 0x680783 | `E8 E4 A7 C9 FF`, `E8 68 AB C4 FF` | call thunk: only for `esi == Player` | both, M3 | [V] |
| PP-MELEE (C5) | 0x67ED30 | `80 3D <abs32> 00 74 2B` | cave: non-local players use their own `+0x1250` path | both, M3 | [V] |
| PP-TURX (C3) | 0x6C0A05 | `85 DB 0F 84 96 00 00 00` | cave: puppets keep the equip and combat-ready calls, skip the camera writes | both, before M8 | [P] |
| PP-TURM (C4) | 0x6B5F89 | `8B 96 9C 00 00 00` | cave: camera writes only for `Player` | both, before M8 | [V1] |
| PP-SHAKE, PP-STEER, PP-TIP (C8, C6, gap3 T1) | 0x6E696D, 0x62D185, 0x6BC1AC | see report | optional | both | [V1] |
| PP-HASH | `Reserved_network_to_game_hash` 0x1290C60, `Network_handle_to_rop_hash` | data | size 1 + buckets → a zeroed dword | both, M3 | [V] |
| DP-SLICE, DP-UBER, DP-LOD, DP-SORT (DP-SHAT opt.) | operands of 0x339EA3, 0x339FE9, 0x33811A, 0x3249AC (0x332772) | abs32 | → co-op byte = 1 | both, M6 | [V] |
| DP-RFGL | `rfgl_desired_values` 0x31D000 | - | host logs {count, flags, quality, delete}; guest applies the host's values when count and flags match | both, M6 | [V1] |
| DP-BAL | `RFG_load_balance` 0x124F0FB (or the lane at 0x325051) | data | 0 on the guest: no local debris deletions; the host's deletions arrive as despawns by uid | guest, M6 | [V] |
| DP-UID | `Rfg_mover_destruction_uid` 0x1A3733C | data | copy the host's value at join | guest, M6 | [V1] |
| DP-RNG | around the 0x4102E0 apply | - | private pool + index (and CRT `_holdrand` via `_getptd`) for the apply on both machines | both, M6 (optional) | [P] |
| DP-POSE | - | - | host sends {uid, pos, orient, lin/ang vel} of ~10 active + ~20 settling chunks at 10 Hz; guest eases with `havok_body_set_*` | both, M6 | [I] |

Alternatives that were also proposed and are **not** needed with the list above: a chain at 0x540A9E that writes and
returns -1 (gap4 G8, equivalent to GF-MIS); a `mission_activate_pre_save` detour (EAX ABI); repointing 0x653124 and
0x65314A plus setting puppet `+0xC4` bit 0 (that bit has other readers).

---

## Approach for co-op (design, INFERRED, built on the facts above)

1. **Guest = no mission object, no scripts, no population drivers.** GF-MIS + GF-ACT + GF-SCR + `player_start_mission`
   refusal + GF-POP. `Mission_active_index` stays -1 on the guest, so no critical-area fail, no `Mission_mode`, no local
   result screen, no local reload. Everything mission-related comes from host messages.
2. **Re-join keeps "send the buffer as is".** The fences make a checkpoint blob or an activity-replay blob harmless, so
   the host never sanitizes blobs or falls back to the anchor (which would lose the buildings destroyed since mission
   start).
3. **MISSION_START** carries: mission index, start node handle, whether the start was fresh (the host's restore index ==
   -1, or a hook on the call at 0x39D6FE), the host position, and the model set (records with MISSION bits in
   `Human_records_list` / `Vehicle_records_list`, read after `srm_all_reserved_slots_streamed`). The guest teleports,
   pre-streams the models with `srm_reserve_slot(SPAWN_SLOT_MISSION,0,info)`, and, if fresh, calls
   `restore_list_do_restore(start_node)`.
4. **RESTORE event** for activity starts and `Action_callback_restore_object`: send the key (hook 0x375180) or the
   handles; fallback: send the expanded list and call `world::restore_object` per handle. The guest delays destruction
   events on a listed handle until its regrow is done.
5. **Objectives**: OBJ_ADD / OBJ_DONE by table index; ICON / MARKER as `{element, world pos, base-zone handle or 0}`; a
   full objective **snapshot** after every re-join (objectives added during the host's own load precede the guest).
   Never call `hud_ui_mission_complete_start`, `mission_terminate_cleanup` or `mission_cancel` on the guest.
6. **Consequences are host-only.** GF-EDF and GF-GM from M6; districts arrive as a raw write of `district+0x9C` (clamped
   to `+0xA4`, plus `hud_ui_change_control` for the HUD); never `district::set_control` or `gfm_*` on the guest. The
   liberated flag, progression and unlocks arrive with the next re-join (v1); show a HUD line.
7. **Turrets and barricades**: TURRET_BIND `{node handle, host turret handle}` for node turrets (the guest adopts its own
   node's child or creates a copy); barricades by roadblock node + formation index. Turrets join the sweeper's watch list.
   Mirror copies are non-persistent (clear `obj+0x55` bit 5).
8. **Destruction**: capture and inject as planned, plus the MP profile on both machines, DP-RFGL, DP-BAL, DP-UID, uid
   pairing (instead of order pairing), the pose lane, and optionally DP-RNG. Suppress guest-only results without
   changing the draw count (delete a salvage item rather than skipping `salvage_should_spawn`).
9. **E4 axes**: plain SP / profile / profile + rfgl / profile + rfgl + RNG, at 30 and 60 fps and with the camera moved
   away; measure a static-remainder hash, uid deltas, chunks alive at +10 s, pose error of the 5 largest chunks.
   Predicted: the static hash matches in every row, uid deltas from row 3, poses never without the lane.

## Contested / refuted (do not rely on)

- "The guest never runs missions, so the mission section in the blob does not matter": **refuted**. Mission section and
  script section both matter (§2).
- "Forcing the restore index to -1 is enough": **refuted**; the blob's managed scripts still run under `script_do_frame`.
- "Skipping 0x3EE1FB stops every script run": **partially**; `mission_setup_do_frame` also calls `script_do_frame`
  (0x39D93D), dead only while no mission is active.
- "Any blob saved during a mission starts it": **partially**; only checkpoint blobs (`World+0x2578 == 1`, not succeeded).
- "Retry from start restarts the mission": **refuted**; only a checkpoint continue re-activates one.
- "39% of highlight targets are layer objects": **corrected** to about 24% of icon/highlight handles; but 72 of 79 icons
  and 40 of 40 ground markers sit on layer objects.
- "Restore targets are 39 activity actions" / "DFA restores dozens of buildings": 67 activity actions; DFA restores 10.
- "Mission layers contain buildings or visible movers": **refuted** (§5).
- "`srm_mission_reservation` on the guest lets the mirror stream mission models": **refuted**; it wipes ambient
  reservations and then `Mission_mode` blocks every reservation.
- "`activity_suspend_activities` stops raids": **refuted** (§7).
- "`district::set_control` is a live setter": **refuted**; it runs the liberation tail (§9).
- "Liberation always reloads": **partially**; only Dust warps to its safehouse, and only after the screen is dismissed.
- "Repointing `gfm_change_edf_control` blocks every liberation": **partially**; `mission_completed` reaches
  `liberate_district` directly (needs the mission fence).
- "The `set_pending_territory_load` detour is a side-effect-free backstop": **partially** (§10).
- "Every RNG draw uses the MT" / "swap the MT seeded from the step number": **refuted**; draws only read a pool by index,
  the CRT `rand` is separate and per thread, and the render thread draws from the same index.
- "`rfg_dyn_create_shard`'s `int_rand_range` ×3 set shard counts": **refuted**; fade lifetimes.
- "Shardify may run on worker threads": **refuted**; the job functions are dead code.
- "The load balancer is the main cause of divergence, ahead of the RNG": **not shown**; both act, at different stages.
- "MP replaces all of the balancer": **partially**; the MP server still runs it in an MP mode.
- "The uber explosion is cosmetic": **refuted**; its flag changes stress.
- "Chunk physics can never match": **partially**; a fixed-substep mode exists, but different per-machine inputs still
  make a pose lane necessary.
- "Frametime is clamped to [1/120, 0.25]": **refuted** at runtime (limiter 1/300 s, clamp 0.1 s).
- "Prime the MP hash tables with a NULL bucket": **refuted**; buckets must point at a zeroed dword.
- "No `Server` read is reachable from the puppet roots": **refuted**; one harmless bool read at 0x5F23FA.
- "`multi_game_xp_trigger_event` is called only from MP code": **refuted**; SP callers exist, but it is self-gated.
- "The MP pose path can be a correction lane in SP": **refuted** (§13).

## Unknowns and the probes that settle them

| Probe | What | Settles |
|---|---|---|
| X1 (gap 1) | Log on every load `World+0x2578`, the u32 read at 0x39D69C, and the managed `Action_scripts` count; GoT checkpoint → die → continue; cancel; DFA fail → continue | which loads carry missions and scripts |
| X2/X3 (gap 1) | GoT checkpoint continue with GF-MIS + GF-SCR on; then each alone | both fences needed and sufficient |
| X4 (gap 1) | 15 min free roam with GF-SCR (PA zones, free-fire zones) | the cost of skipping ambient scripts |
| X5 (gap 1) | Hook `objective_highlight_add` 0x73A380 on the host in DFA and GoT; log handle, class, position | highlight protocol; whether squad-node highlights pass the node or its members |
| X6 / G3 | `restore_list_do_restore(0x7A050002)` on a guest after a destroyed DFA building; dump `All_restore_lists` on both machines | RESTORE event; list parity after a blob load |
| X8 (gap 1) | Finish Save the Guerrilla Camp past its checkpoint; log `World+0x2578` in free roam | the host's join precondition |
| G1 (gap 5) | Load DFA's layers directly; check `All_manager_layer_objects` and spawn-node activity | layers-only mode |
| G4/G5 (gap 5) | Model census at `srm_all_reserved_slots_streamed`; guest-side `srm_reserve_slot(1,0,info)` for named characters | MODELS payload, budgets, fallback need |
| G6/G7 (gap 5) | Histogram `create_object_internal` classes in DFA, FMC, a raid; compare node-turret handles on two instances | mirror classes; turret pairing rule |
| X1/X2 (gap 4) | Guest mode for 15 min with GF-POP and the skips; log every call reaching `spawn_npc` / `sp_spawn_vehicle` / `spawn_squad` by caller | fence completeness, refusal cost |
| X4 (gap 4) | Load an activity-replay blob with GF-POP on and off | `Activity_node_start_on_load` |
| X5/X6 (gap 4) | Destroy an EDF building near 0 control with GF-EDF; raw-write `+0x9C` | liberation chain, safe district sync |
| X7 (gap 4) | Guest shoots gas tanks with GF-GM; call `apply_damage(…,true)` | the general-mover lane; `Server` = 0 |
| X8 (gap 4) | Guest Esc → Load / Restart, death Continue, upgrade-shop exit, fast travel with GF-LOAD/GF-SAVE/GF-UI | no load, no save, no UI hang |
| X9/X10 (gap 4) | Copies streamed out and back; `gem_post_event` logger | non-persistent copies; synchronous script handlers |
| P1-P8 (gap 2) | Thread id at 0x4102E0; per-step index and `_holdrand` deltas; `vfx_effect::update_render` thread; rfgl logger near and far; the E4 matrix; immediate-slice counter; guest balancer off 10 min; pose snap test | the E4 design and DP-* set |
| gap 3 §6 | PP-STATS on/off (expect AV at rfg+0x5862E6 reading 0x610 when off); air control; crouch camera; turret camera; melee heading; upgrades; tooltips; pad leaks; a 10-min camera/HUD write log filtered to the puppet window | the M3 patch list in game |

Open questions:
- `managed_script_group_data` bit 0 (skipped by `script_system_save`): which groups are left out of blobs?
- Does `script_terminate_managed_group(id,bool)` fire on-terminate actions? (Only for a finer alternative to GF-SCR.)
- Does `restore_list_system_load` restore every list with its handles?
- Does any `gem_post_event` registrant run script actions synchronously on the guest?
- Is `Server` (0x2BEA590) 0 at runtime in SP? (GF-GM depends on it.)
- Does `activity_force_start_generic(…,0)` honour the `+0x424` suspend bits? (Moot while GF-POP is on.)
- Does `activity_suspend_activities(5)` stop "At Init" activity layer loads and restorations?
- Is `cs_play_internal` reachable in the campaign (it would set `Mission_mode`)?
- Do NPC-manned turrets ever have `turret_info+0xA0 & 0x20`? What is `turret+0xD4` bit 0?
- Does `player_cover_do_frame` make a puppet enter cover by itself?
- Global per-human passes outside the four puppet roots (post-integrate, AI perception, `cartographer::collect_humans`,
  the 69 readers of `World+0x3FC`) are not walked.
- Do render-thread pool draws overlap the apply in practice? Do virtual calls inside the apply draw per machine?
- `Vehicle_records_list` capacity; DLC (DotB) mission containers and `dlc01_spawn_resource` were not analysed.
