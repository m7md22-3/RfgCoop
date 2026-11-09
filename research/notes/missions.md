# RfgCoop notes: missions, activities, cutscenes, game states, pause, co-op UI

Distilled 2026-10-07 from `research/findings/workflows/coop_missions.md` (the investigator report) and the skeptic
verdicts on claims C1-C8. Build: GOG rfg.exe, 32-bit, PDB image base 0x400000. All addresses are **RVAs** unless marked
VA (VA = RVA + 0x400000; the disassembly prints VAs). Tools: the PDB, disassembly, the symbol/xref tables and the
`rfg.asm` cache. The xref table columns are `target RVA, kind, site RVA, containing function`. Nothing here was run
in game.
Tags: **[V]** = read in code, PDB or data and re-checked by a skeptic. **[V1]** = read by the investigator only, not
contradicted, not independently re-checked. **[I]** = inferred, not read.

## TL;DR

- Campaign missions are data, not Lua: `missions.xtbl` rows + XML `.scriptx` event/condition/action groups + managed
  ZONE layers, over the synchronous C++ event bus `gem_post_event` (0x737800). Game Lua is vint UI only. Scripts use the
  shared RNG, random `<delay>` ranges and AI/dialogue/physics timing, so two machines cannot run the same mission in
  lockstep. **Run missions and activities on the host only and replicate results** (design choice, not an engine limit). [V]
- **The host must keep `Multiplayer` (0x2BEA588) = 0.** `player_start_mission` returns false, `activity_get_activity_data`
  returns NULL, `activity_is_active` returns false, and the SP death/reload flow is skipped whenever it is 1. [V]
- Every SP death-continue, fail-retry, mission cancel, fast travel, activity replay and warping story video ends in
  `world::set_pending_territory_load` (0x51C320), the only `gameseq_set_state(GS_LOAD=2)` in the exe. One detour = the
  session-reload choke point. It has silent no-op guards, and some side effects (saves, `vdir_restore`) happen before it. [V]
- The SP death loop runs for every `OT_SUB_HUMAN_PLAYER` object, not only the local `Player`. A player-subtype partner
  puppet on the host would fail the mission / autosave / open the death screen. Latch `+0xB8 & 8` skips it but is cleared
  by every `player_respawn`. Scripts' `player_*` checks only see the host's `Player`; 42 of 485 script callbacks are
  bound to it, and they are swappable `.data` pointers. [V]
- Story cutscenes are Bink videos in a non-transparent state with `game_pause`; the in-engine cutscene system is dead.
  Pause = state-stack transparency + per-push flags + `Game_paused_count`; alt-tab pause is one call site (0x4DDB58);
  the MP Esc menu already runs over a live world. [V]

## Facts

### Mission definition and scripts
- Lua is UI-only: `lua_load_script` 0x830090 is called only by `vint_lua_init` / `vint_rfg_system_init`;
  `lua_load_dynamic_script` 0x8314A0 and 0x831570 only by `vint_lua_document_init`; `lua_create_state` 0x8323A0 only by
  `vint_lua_init`; `lua_do_frame` 0x831650 only by `ui_process`. [V]
- `table.vpp_pc`: `missions.xtbl` (22 rows: 21 story + "Marauder Temple"), `mission_objectives.xtbl`, `objectives.xtbl`,
  `mission_briefing_text.xtbl`, `missions_text.xtbl`, `mission_unlock.xtbl`, `rfg_lines_scripted.xtbl`,
  `script_messages.xtbl`. Row fields: `script_groups`, optional `checkpoint_script_groups`, `managed_layer_info`,
  `resource_reservation`, `voice_persona_list`, `district`, `capstone`, `No_Autosave`, `mode`, `cutscene_played`,
  `final_mission`. `No_Autosave=True`: Tutorial, PartyTime, DFA, Ants Vs Mag, Final Mission, Marauder Temple. [V1]
- `mission_mode`: DEFAULT 0, PARTYTIME 1, WMR 2, ANTS_VS_MAG 3, TUTORIAL 4, FMC 5, SNIPER 6, HIGHWAY_TO_HELL 7,
  CUTSCENE 8 (Marauder Temple). [V1]
- `mission_info` (0x60), `All_mission_infos` RVA 0x01E45C18 (`farray<mission_info,26>`): +0x00 name, +0x10
  start_node_handle, +0x14 mis_mode, +0x18 reward_salvage, +0x23 no_autosave, +0x24 cutscene_played, +0x28
  managed_layer_set*, +0x38 `farray<int,2>` managed_script_list, +0x50 `int[2]` checkpoint_managed_script, +0x58
  mission_flags. Full table in the report F1. [V1]
- `missions.vpp_pc`: 21 `.str2_pc`, one per mission, holding managed layers `terr01_XX_YY_NNN.layer_pc` (ZONE v36). DFA
  layer `terr01_08_10_128` = 109 objects, mostly spawn/action/logic nodes. [V1]
- `zonescript_terr01.vpp_pc`: 144 `.scriptx` (extracted copy:
  `...\e162797c-...\scratchpad\verify_c1m\zs`). 76 have a `<managed>` group (missions/activities); 68 are unmanaged and
  always run (PA speakers, EDF target counters, tool tips, free-fire zone) and contain **no** `spawn_squad` /
  `spawn_vehicle`. [V1]
- scriptx = XML `<script>` with `<condition>` (`<event>`/`<function>` tree) + `<action>`s, `<delay><min><max>`, fixed
  object handles (e.g. `8A0004EF`). Counts: `patrol_squad_reached_node` 277, `dialogue_line_finished` 132 [V];
  81 of 693 `<delay>` pairs have min != max [V]; `player_in_radius` 249, `player_in_trigger_region` 150,
  `get_player_handle` 120, `player_in_car` 64, `random` 9, `video_cutscene_play` 5, `mission_save_checkpoint` 8 [V1].
- Script engine [V1]: `script_do_frame` 0x73C060, `script_run_script` 0x73BD30, `script_action_run` 0x737AD0,
  `script_event_evaluate` 0x7325F0, `script_activate_managed_group(int,bool)` 0x73F410,
  `script_terminate_managed_group(int,bool)` 0x73B700, `script_system_save(_managed)` 0x73C350/0x73C190 (script state is
  in the save).
- `Function_callback_random` 0x71FE90 calls `int_rand_range` (0x17A380): a 2048-entry table `Vlib_rand_instance`
  indexed by one global counter (+0x9C4) that every system advances, so sequences diverge across machines. [V]

### Local-player binding
- `player_get_local_player` 0x610540 = `mov eax,[Player]; ret`, no MP branch. `Player` = RVA 0x02C22874. 849 call sites
  in 750 functions; 301 memory refs to `Player` in 194 functions (5 are writes: VA 0xA108A2, 0xA9DD66, 0xA9DD72,
  0xAD593A, 0xAD5A83). 117-128 activity functions use the local player depending on how counted. [V]
- 42 of the 485 `Function_/Event_/Action_callback_*` functions reference the local player; no "any player" / remote
  variant exists. [V] Examples:
  - `Function_callback_get_player_handle` 0x720240 -> local player +0x6C (handle) or -1.
  - `Function_callback_player_in_radius` 0x720000 -> XZ distance from local player +0x4/+0xC.
  - `Event_callback_player_enter_trigger_region` 0x7209D0 -> needs event+0x8 == region **and** event+0x4 == local
    player +0x6C.
- `trigger_region_do_frame` 0x6D61E0 iterates the World+0x74 object list (count World+0x7C), not only `Player`, and posts
  gem event 0x14 with object + region handles (0x6D637D). So a puppet human fires the event; only the script-side
  filter rejects it. `ambush_region_do_frame` 0x691A40 also posts 0x14. [V]
- `critical_area_do_frame` 0x67A580 reads `Player` for the leave-mission-area warning/fail. [V1]
- Activity scoring: `activity_mayhem::register_building_damage` 0x398AD0 gates on
  `activity_register_damage_is_player_controlled` 0x385720 (false in MP; else looks up
  `Register_current_marked_list` / `Register_previous_marked_list`). [V1]

### Script definitions are swappable data
- Layouts (PDB) [V]:
  - `script_function_definition` 0x14: +0 callback `void(script_variant*, script_variant*, script_node**)`, +4 result,
    +8 args, +C desc, +10 group.
  - `script_event_definition` 0x10: +0 callback `event_result(event_data*, script_node**)`, +4 event_type, +8 args, +C desc.
  - `script_action_definition` 0x14: +0 callback `void(script_node**)`.
  - `script_function` and `script_event` hold the definition pointer at +0xC.
- `.data` starts at RVA 0x10F4000 (VirtualSize 0x26B801C); callbacks are static `.data` (dynamic initializers 0xC7DCD0 /
  0xC7FD80 write only desc/group). Dictionary entries point at the single global definition, so one write reaches every
  script use. [V]
  | Definition | RVA | +0 callback |
  |---|---|---|
  | `Function_definition_player_in_radius` | 0x012B4648 | 0x720000 [V] |
  | `Function_definition_player_in_trigger_region` | 0x012B3128 | 0x7265D0 [V] |
  | `Event_definition_player_enter_trigger_region` | 0x012B32B0 | 0x7209D0, type 0x14 [V] |
  | `Function_definition_get_player_handle` | 0x012B2EDC | [V1] |
  | `Function_definition_random` | 0x012B35CC | [V1] |
  | `Action_definition_video_cutscene_play` | 0x012B3348 | [V1] |
  | `Action_definition_mission_set_failed` | 0x012B3DF0 | [V1] |
- Dispatch: `script_function::operator bool` 0x725080 `mov eax,[ecx+0xC]; mov eax,[eax]; call eax`. Events:
  `script_event::load` 0x7385A0 stores the definition at +0xC and registers `script_event_evaluate` 0x7325F0 via
  `gem_register_for_event` with `def->type`; `script_event_evaluate` calls `[[esi+0xC]]` at 0x732634. [V]
  Action call site not traced. [V1 layout only]

### Event bus `gem`
- `gem_post_event(event_data*)` 0x737800: enters `Gem_cs`, copies the `notify_info` list for `event->type` from
  `Gem_notifications` (RVA 0x032BE350, `object_pool<notify_info,4096,62>`), calls each callback synchronously
  (`call eax` 0x7378E8); a callback returning 1 is unregistered. [V]
- `event_type` 0..61, `NUM_EVENT_TYPES` = 62. 87 direct call sites, no inlined posters (only gem_*, `script_event::reset`,
  `public_address_kiosk_level_init_pre` touch `Gem_notifications`/`Gem_cs`). All 75 `Event_callback_*` are reached only
  through gem registrations. [V]
- Useful types [V1]: 2 BUILDING_DESTROYED, 8 ACTIVITY_STATE_CHANGE, 12 VEHICLE_DESTROYED, 13 HUMAN_DEAD, 14
  DIALOGUE_FINISHED, 18/19 PLAYER_ENTER/EXIT_TRIGGER_REGION, 20/21 OBJECT_ENTER/EXIT_TRIGGER_REGION, 25 MISSION_SUCCESS,
  26 MISSION_FAILURE, 27 TIMER_ELAPSED, 29 MISSION_OBJECTIVE_COMPLETE, 33 SQUAD_KILLED, 36 PLAYER_KILLS_HUMAN, 41/42
  PLAYER_ENTERS/EXITS_VEHICLE, 49 PATROL_SQUAD_REACHED_NODE, 61 USER_DEFINED. (The skeptic-verified handler reads
  type 0x14 = 20 as the trigger-region post.)
- **Not on the bus** [V]:
  - Player death during a mission -> `mission_terminate(0)` (flag only); no MISSION_FAILURE posted.
  - `mission_cancel` (menu restart/quit) posts nothing.
  - Completion work (`mission_info::mission_completed`, salvage, HUD) is in `mission_terminate_internal`.
  - BUILDING_DESTROYED is deferred: `object_mover_maybe_add_to_destroyed_event_queue` ORs 0x800000 into mover+0xE0;
    `object_mover_maybe_post_destroyed_event` 0x6132C0 posts once, later.
  - `mission_failure()` 0x35E9B0 posts 0x1A but has **no callers** (dead).
  - `Action_callback_mission_success` 0x738090 posts 0x19 only if a mission is active and the local player's vfunc
    +0x10 returns false.

### Mission lifecycle [V1 unless marked]
- Start: `player_is_near_mission_start` 0x65B5D0 -> `player_start_mission(player*,bool)` 0x633F40, which returns false
  when `Multiplayer` != 0 [V], a mission is active, during vehicle interaction or with the map open. Then
  `hud_ui_mission_briefing_start` 0x458A00 (GS 38) or `mission_activate` 0x39D960.
- `mission_activate(handle,bool)` 0x38E560 -> `mission_activate_pre_save` 0x38E410: sets `Mission_active_index`, clears
  termination flags, `world::save_and_continue` (pre-mission save = restart anchor), despawns squads.
- `mission_setup_do_frame` 0x39D7D0 (from `gamestate_gp_handshake`) -> `mission_activate_post_save` 0x39D6B0: managed
  layers, checkpoint restore, `activity_suspend_activities`, `human_voice_load_set`, `srm_mission_reservation`, then
  `script_activate_managed_group` for `managed_script_list` or the saved checkpoint group.
- Run: `mission_system_do_frame` 0x3B4C10 (simulate 0x3EE1F6, before `script_do_frame` at 0x3EE1FB): critical area,
  unlock flow, checkpoint save, `Mission_pending_termination` -> tail-jump to `mission_terminate_internal` 0x3B4970.
- `mission_terminate(bool by_script)` 0x35E9F0 only sets `Mission_pending_termination` / `Mission_terminated_by_script`. [V]
- `mission_terminate_internal`: success rewards, `hud_ui_mission_complete_start(bool, mission_info*)` 0x44AE70 (not for
  CUTSCENE mode), fade, `mission_terminate_cleanup` 0x3B0380 (layers unload, `activity_reinstate_activities`,
  `human_teleport` of the player, clear objectives, `Mission_active_index = -1`), `Mission_terminated = 1`.
- Globals: `Mission_active_index` 0x0125050C, `Mission_saved_checkpoint` 0x01250500, `Mission_checkpoint_start_handle`
  0x01250508, `Mission_active_data` 0x01E28578, `Mission_pre_terminated` 0x01E28664, `Mission_pending_termination`
  0x01E28665 [V], `Mission_terminated` 0x01E28666, `Mission_terminated_by_script` 0x01E28667, `Mission_succeeded`
  0x01E28668, `Mission_checkpoint_pending` 0x01E28669. Also `mission_is_active` 0x35E970, `mission_cancel` 0x38E670,
  `mission_skip_tutorial` 0x35EB80, `mission_set_player_never_die` 0x35EA20.
- Checkpoint: action `mission_save_checkpoint` -> `mission_checkpoint_save` 0x35EA10 sets `Mission_checkpoint_pending` ->
  `world::save_and_continue`.

### Activities [V1 unless marked]
- C++ singletons: `All_activities` RVA 0x01250248 = `activity_base*[17]`, one per mode (1 Riding Shotgun, 4 Guerrilla
  Express, 7 Mayhem, 8/9 Raid defend/attack, 10 House Arrest, 11 Demolitions Master, 12 Assassin, 13 Convoy, 16 Area
  Defense, ...). `activity_base` 0x444: +0x424 suspend_flags, +0x428 state (WAIT 0 .. SHUTDOWN 5), +0x42C interface_state.
- `activity_system_do_frame(bool)` 0x3ACB00 runs from simulate (0x3EE4DF, **after** the pause label) and
  `gameplay_do_frame` (also runs `do_frame_spawn_new_raid`, `do_frame_spawn_selected_raid`).
- `activity_suspend_activities(reason)` 0x356550 / `activity_reinstate_activities` 0x356640.
- `activity_get_activity_data` 0x354FE0 / 0x355000 return NULL when `Multiplayer` != 0 [V]; `activity_is_active`
  0x355F90 false under MP.
- `activity.xtbl`: 115 `base_activity` rows; `activities.vpp_pc`: 89 `je_*.str2_pc` layer containers.

### Reload choke point `world::set_pending_territory_load` [V]
- `bool __thiscall world::set_pending_territory_load(char*, bool, bool, bool, int, bool)` 0x51C320. Copies the name to
  World+0x2698, sets sl_flags, calls `gameseq_set_state(2=GS_LOAD)` at 0x51C3C7 (the only GS_LOAD entry in the exe).
  `gs_load_process` 0x512CE0 then runs `gamestate_gp_exit`, `world::load_territory`, `world::setup_territory`.
- **Silent no-op** (`xor al,al; ret 0x18`) when World+0x24E0 (deserialize_state) != 0, sl_flags byte0 bit2
  (pending_save_game) or bit5 (performing_save_state) set, or World+0x24D0 (save_file) != NULL.
  `save_and_reload` / `save_and_warp` have the same guards; `save_and_warp` also does nothing while `mission_is_active`.
- `World` = RVA 0x2B97490; `sl_flags` (`save_load_info`) at World+0x265C; byte1 (World+0x265D) bit2 =
  pending_load_game_from_memory. Full bit table in the report F6. [V1 for the full table]
- Indirect routes:
  - `world::set_pending_load_from_memory` 0x51C450 = `or [ecx+0x265D],4` only.
  - `world::save_and_reload` 0x51D0E0 = `save_and_continue` (only if no mission active) + that bit.
  - `world::save_and_warp` 0x51D120 = warp pos/orient + `save_and_reload`.
  - `process_save_load` 0x3C00E0 (from `gamestate_gp_handshake` 0x3D9159, `sony_options_handshake`,
    `world::setup_territory`): runs `world::save_state_data` / `platform_save_to_disk` first, then if the bit is set calls
    set_pending_territory_load at 0x3C016C and clears it.
  - `world::set_pending_reload` 0x51C3E0 -> set_pending_territory_load(World+0x1C, new_game=1) at 0x51C438.
- Direct callers: `video_cutscene_exit_state` (x2), `mission_cancel` 0x38E75D, `humc_request_exit` 0x44A8D8,
  `player_death_options_continue_cb`, `process_save_load`, `world::set_pending_reload`, `dlc_load_level` 0x3BAF75,
  `load_level_internal`, `verify_gamesave_process`, `hud_ui_mission_briefing_process`, `credits_process` 0x512761,
  `wrecking_crew_match_begin`, `multi_game_lobby_server/client_start_game`.
- `save_and_warp` callers: `fsm_warp_to_safehouse_cb` 0x454484 (fast travel), `mission_warp_and_start`,
  `activity_warp_and_start`, `mission_system_do_frame`, `hud_ui_district_liberation_process`,
  `camera_stop_satellite_mode_internal`.
- `save_and_reload` callers: `activity_area_defense/house_arrest/raid(x3)/riding_shotgun/guerrilla_express_replay`,
  `gamestate_respawn_player`, `check_for_user_input_complete` (x2). Guerrilla Express replay has an early return at
  0x3B5EFD. No replay function exists for Demolitions Master or Mayhem; `activity_demolitions_master::warp_player_on_end`
  0x397AE0 uses `player_respawn`, no reload.
- `humc_request_exit` 0x44A820 reloads only when `Mc_info+0x8 != 1`; which value means "failed" is [I].

### Death [V]
- `human_do_frame` (VA 0xAE59B4-0xAE59E7) calls `player_is_killed` 0x634360 for any human with hp (+0x914) <= 0, state
  (+0x2F4) not 4/0xB, +0x7E == 0, +0x7F == 3. `player_is_killed` arms `+0x1118` = 4000 ms for **any** player object;
  only `local_player_is_killed` is gated on `Player`.
- `gamestate_gp_simulate_frame` 0x3EDB60 loops `World.subtype_objects[3]` (data World+0x3F4, count World+0x3FC). Per
  object: skip if +0x54 & 0x10; when +0x1118 elapsed and `!(+0xB8 & 8)` it sets `+0xB8 |= 8` (0x3EDBD0-0x3EDBE1), then
  if `!Multiplayer && !wrecking_crew_is_active`:
  - mission active -> `mission_terminate(0)` (0x3EDC0C);
  - else activity handler by mode (11 demolitions tail-jump at 0x3EDF69, 8/9 raid, 10 house arrest, 1 riding shotgun, 4
    guerrilla express, 16 area defense). Handlers take **no player argument**;
  - else `world::save_and_continue(0,0,1)` (autosave) and `gameseq_push_state(16,1,0)` at 0x3EDD05
    (GS_PLAYER_DEATH_OPTIONS; `player_death_options_enter` 0x50FB30 calls `game_pause` in SP).
- `+0xB8 & 8` = `human_flags.death_func_done` (human_flags at +0xB4, dword +4 bit 3). Also read by
  `ai_system::do_death_func` and `despawn_corpses` (which walks the NPC list World+0x3E4, not players).
- `player_death_options_continue_cb` 0x492FF0: returns with no reload if `Multiplayer`. SP: `Reload_level_on_death`
  (RVA 0x2132874) -> `vdir_restore` + `set_pending_reload`; `mission_get_mode()==4` -> set_pending_territory_load
  (0x49303A); else `set_pending_load_from_memory`. `gamestate_respawn_player` 0x3BFEF0 has the same SP shape; its MP
  branch uses `multi_game_respawn_get_best_spawn_point` + `player_respawn` int overload (0x3BFF5A).
- `player_respawn(player*, vector*, matrix*, bool)` 0x6DF5F0: no MP early-out; SP path `human_teleport` (0x6DF7AF),
  hp refill from +0xAE0/+0x910, ragdoll stop, +0x2F4 = 0, clears `+0xB8` bit 8 (0x6DF9EA), camera/hud reset only if
  `Player`, `player_reequip`, `+0x1118 = -1` (0x6DFA86). Direct callers: `activity_demolitions_master::warp_player_on_end`
  (0x397B65, a living player) and the overload.
- `player_respawn(player*, int, bool)` 0x6DFAB0 returns immediately unless `Multiplayer || wrecking_crew_is_active()`.
  `wrecking_crew_respawn_player` uses this one, so **call 0x6DF5F0 directly**.

### Cutscenes [V]
- Story videos `data/rfg_cine_0Xa.bik_xbox2`; subtitles `table.vpp_pc/rfg_cine_*.xtbl`. [V1 for files]
- Script use (`video_cutscene_play(bik, warp_obj)`):
  | Script (line) | Video | Warp object |
  |---|---|---|
  | terr01_tutorial.scriptx:1277 | RFG_CINE_01A | 4A0201B9 |
  | terr01_dfa.scriptx:3528 | 02A | FFFFFFFF |
  | terr01_partytime.scriptx:2844 | 03A | 7904000B |
  | terr01_guerrillacamp.scriptx:2871 | 04A | FFFFFFFF |
  | terr01_martemple.scriptx:42 | 05A | CA010008 |
  | terr01_final.scriptx:5236 | 06A (`video_cutscene_play_with_credits`) | 56020003 |
- `Action_callback_video_cutscene_play` 0x7224F0 -> `video_cutscene_play(char*, unsigned warp, char*, bool, bool,
  mempool_base*, mempool_base*, dlc_level_info*)` 0x30A4A0 [V1] -> `video_cutscene_enter_state` 0x306E30: pauses sounds,
  `gameseq_push_state(49,0,0)` (or `gameseq_set_state(49,0)` if `Video_cutscene_set_state`).
- State 49 registered transparent=0, pause_beneath=0 (`video_cutscene_system_init` 0x30FD90).
  `video_cutscene_play_enter` 0x306FE0: `hud_hide(1)`, `game_pause(1)`.
- `video_cutscene_exit_state` 0x306E90: warp obj != -1 (`Video_cutscene_warp_to_object` RVA 0x124AE8C) ->
  set_pending_territory_load (0x306F15); warp zone -> 0x306F4D; DLC level -> `dlc_load_level`. **Roll-credits branch
  (06A)** calls `credits_show(1, warp_obj)` instead, and the reload comes later from `credits_process`.
- In-engine cutscenes are dead: `cutscene_play` 0x305940 (only GS 47 entry) is called only by
  `unused_dcf_cutscene_play`. Only scripted camera actions are `satellite_camera_*` (gameplay mode). Other
  `video_cutscene_play` callers: handbook, upgrades, main menu, DLC UI.
- `interface_process` will not open Esc while `cutscene_is_playing()` / `video_cutscene_pending_or_playing()` 0x307140. [V1]

### Game states and pause [V]
- `State_info` = `state_info[64]` at RVA 0x0213ADA0, stride 0x24: +0xC process, +0x10 handshake, +0x14 render,
  +0x18 transparent, +0x19 pause_beneath.
- `gameseq_push_state(game_state, bool transparent, bool pause_beneath)` 0x3D87E0 rewrites +0x18/+0x19 on every push
  (0x3D8875 / 0x3D887B), except when the request queue rejects the push.
- `gameseq_do_state` 0x3BFAE0 recurses into the state below only if the top is transparent; pause_beneath (or inherited
  paused) skips process (mode 0) and handshake (mode 1) but still renders (mode 2). Non-transparent top = gameplay
  neither processed nor rendered.
- `gamestate_gp_process` 0x3EE520 calls `gamestate_gp_simulate_frame` at 0x3EE55C. In it, `Game_paused_count` (RVA
  0x2132880) > 0 jumps 0x3EDE5B -> 0x3EE4DD, skipping the world sim (missions, scripts, AI, physics...). Earlier work
  (interface_process, HUD, camera, cartographer, dialogue, music [V1 list]) and, after the label, `gem_do_frame` and
  `activity_system_do_frame` [V1] still run.
- `game_pause` 0x3C1620: atomic `Game_paused_count++`; on 0->1 pauses timestamps, animation timer, audio.
  `game_unpause` 0x3C1650, `game_is_paused` 0x3C1610. [V; unpause/is_paused addresses V1]
- Pushes (enum: GS_INGAME_OPTIONS 15, GS_PLAYER_DEATH_OPTIONS 16, GS_INGAME_FULLSCREEN_MAP 37, GS_VIDEO_CUTSCENE_PLAY
  49, GS_QUICK_PAUSE 62):
  - Esc 15 = (1,0) at 0x4DDB00-0x4DDB06 in SP and MP; `ingame_options_enter` 0x50F400 calls `game_pause` only if
    `!Multiplayer` (0x50F48C). **So MP Esc runs over a live world.**
  - Map 37 = (0,0) at 0x44DE4D-0x44DE53; video 49 = (0,0); quick pause 62 = (0,0); death 16 = (1,0).
  - Other in-game HUD states 38-50 are mostly (1,0) [V1, table in report F8].
- Alt-tab: the only GS_QUICK_PAUSE push is in `interface_process` 0x4DDB4F-0x4DDB6F, gated by `!Multiplayer`,
  `!os_foreground()` (call at **0x4DDB58**) and `!Cursor_visible`. `os_foreground` 0x1AD960 has other callers
  (`rfg_do_frame`, `control_info::read_controls`, `gamestate_quick_pause_process`), so patch the call site, not the
  function.
- Other `game_pause` callers: `ui_set_screen_blur`, `ui_add_msgbox`, `tool_tips_queue_process`,
  `ui_fade_to_black_fade_out`, `save_game_callback`, `gs_load_enter`, ... [V1]

### Co-op UI building blocks (signatures [V1]; claim C8 not re-checked; SP behaviour unknown)
- Objectives: `add_objective(int)` 0x464FF0 (0x194-byte table entries, text at +0x123) -> `hud_ui_missions_add_objective(char*)`
  0x496F80; `complete_objective(int,bool)` 0x475D70; `hud_ui_missions_complete_objective` 0x495B20;
  `hud_ui_missions_remove_objective` 0x496FB0.
- Highlights: `objective_highlight_add(unsigned, highlight_purpose, highlight_style)` 0x73A380, `_remove` 0x73A430,
  `_remove_all` 0x73A4A0, `objective_highlight_ground_marker_add` 0x72DDE0.
- Map: `Cartographer` RVA 0x027B61F8; `cartographer::add_icon(unsigned, minimap_elements, bool, bool, bool)` 0x479AF0
  (ally squads use STAR_BLUE 1); `remove_icon` 0x4743A0; `add_player_marker` 0x47DFC0. Elements: STAR_GREEN 2, PLAYER 51,
  MISSION_OBJECTIVE 40.
- Tag: `hud_icon_add(unsigned)` 0x476E60 -> `hud_icon*` (0x134): +0x6 `wchar_t text[3][32]`, +0xE0 display_distance,
  +0xE1 snap_to_edge, +0x108 remove_when_destroyed; `hud_icon_remove` 0x476EA0; rendered by `hud_icon_render` 0x47BCC0
  from `hud_render` 0x47F48F.
- HUD text `hud_message_add` 0x455D30, `ui_add_secondary_message` 0x4D8270. Dialogue
  `dialogue_manager_start_dialogue` 0x739E20, `dialogue_manager_add_line` 0x732EA0, `human_voice_load_set` 0x659340.
  Timers `script_timer_display` 0x722BD0.
- Static handles: zone runtime handle == zone file handle (earlier verified). Script target
  `8A0004EF` matches zone (8,10) / layer `terr01_08_10_128`. Same handles in managed layers is [I]. Script-spawned NPCs
  and vehicles get runtime handles and need the replication handle map.

## Approach for co-op

All of this is design [I] built on the facts above.

1. **Ownership.** Host runs mission system, scripts, activities, GFM, alert and saves, with `Multiplayer` = 0. Guest runs
   the same territory but never a managed group:
   - Detour `player_start_mission` 0x633F40 on the guest -> false (optionally send a start request to the host).
   - Guest calls `activity_suspend_activities(5)` 0x356550 after every load (raid spawns: probe P14).
   - Detour `world::set_pending_territory_load` 0x51C320 on the guest; allow only session-protocol calls; log the rest.
2. **Host -> guest reliable messages** (detours on the host):
   - MISSION_START: `mission_activate` / `Mission_active_index` -1 -> n; teleport a far guest with `human_teleport`.
   - OBJECTIVE_ADD/COMPLETE/CLEAR: `add_objective`, `complete_objective`, `hud_ui_missions_remove_objective`.
   - HIGHLIGHT_ADD/REMOVE: `objective_highlight_add/remove`, static handles only in v1.
   - MISSION_END(success): `mission_terminate_internal` 0x3B4970 (not gem; death/cancel bypass the bus).
   - VIDEO(bik): `video_cutscene_play` 0x30A4A0; guest plays it with warp 0xFFFFFFFF; barrier on both done; for 06A hold
     the guest through credits until the host's `credits_process` reload.
   - RELOAD_BEGIN/DONE: `set_pending_territory_load` on the host; check its bool return; guest loads host-provided state
     (a reload from memory restores each machine's **own** state, so state transfer is required; world_save domain).
   - Optional observation of everything else: hook `gem_post_event` 0x737800.
3. **Death.**
   - Partner puppet on the host: prefer an NPC-subtype human. If it must be subtype 3, keep `+0xB8 |= 8` set (re-set
     after every respawn) or `+0x1118` invalid, or hook `player_is_killed`.
   - Guest's own death on the guest: set `+0xB8 |= 8` before +0x1118 elapses, then `player_respawn` 0x6DF5F0 near the
     host. Host death outside missions: same path (avoids reload storms). In missions: keep SP fail + synced reload.
     Gated on probe P7.
4. **Pause v1.** Patch the `os_foreground` call at 0x4DDB58 to return 1 on both. Host pause (count 0->n or
   non-transparent top) -> guest `game_pause`; keep a co-op pause token separate from the engine counter so it always
   balances. Guest menus freeze only the guest; host may make the guest avatar invulnerable meanwhile. v2: MP-style live
   Esc/map (skip `game_pause` from `ingame_options_enter`; set `State_info[37]` +0x18=1, +0x19=0 at RVA 0x213B2EC /
   0x213B2ED after the map push).
5. **Either player triggers steps (v2).** Swap the `.data` callbacks of the 42 local-player script callbacks that matter
   (start with `player_in_radius` 0x12B4648, `player_in_trigger_region` 0x12B3128, `player_enter_trigger_region` event
   0x12B32B0, then `player_in_car`, `player_in_specific_car`) with wrappers that also test the puppet. Leave
   `get_player_handle` (feeds AI targeting) alone. Native C++ checks (`critical_area_do_frame`, activity code) need
   separate hooks.
6. **UI.** Partner blip `cartographer::add_icon(h, STAR_GREEN, 1,0,0)`, tag `hud_icon_add(h)` with text, HUD feed via
   `hud_message_add`, objective text mirrored. All pending probes P9-P11.
7. Start co-op after the tutorial (mode 4 death reloads directly; scripted input locks). `mission_skip_tutorial` 0x35EB80.

## Contested / refuted (do not rely on)

- **"Missions can only run on the host" / "hard-bound".** Host-only is the right design but a choice: the binding is 42
  swappable callbacks plus `player_get_local_player`. And host-only alone is not co-op: guest actions do not advance
  `player_*` steps until those callbacks are widened.
- **"Host-authoritative works regardless of MP mode."** Wrong if the host enables stock `multi_init`: 144
  mission/activity/trigger functions read `Multiplayer`, and missions, activities and the SP death flow switch off.
- **Counts in the report.** "301 direct reads" = 301 references incl. 5 writes. "123 activity functions" not
  reproducible (117-128). "849 callers" = 849 sites in 750 functions.
- **"gem_post_event carries every mission-relevant event."** False: mission death-fail, `mission_cancel` and completion
  work bypass it; BUILDING_DESTROYED is deferred and posted once; `mission_failure()` is dead code. Hook
  `mission_terminate` / `mission_cancel` / `mission_terminate_internal` too.
- **"wrecking_crew_respawn_player shows player_respawn works in SP."** It calls the int overload 0x6DFAB0, which no-ops in
  plain SP. Revive-after-death via 0x6DF5F0 in SP is plausible (same body MP uses) but **not demonstrated** (P7).
- **"+0xB8 & 8 is a guard bit."** It is the engine's own one-shot `death_func_done` latch, cleared by `player_respawn`.
- **"Hooking set_pending_territory_load stops the guest's reload side effects."** No: `save_and_continue`,
  `save_state_data` / `platform_save_to_disk` and `vdir_restore` run before it. Blocking it on death also leaves the
  guest dead unless a respawn path replaces it.
- **"Video exit always reloads immediately."** The 06A credits path defers the reload to `credits_process`.
- **The report's F7/F8 "humc_request_exit failure branch"** is a code branch (`Mc_info+0x8 != 1`); its meaning is [I].
- **Anything about co-op UI rendering in SP** (claim C8) is unchecked; treat only signatures as known.

## Unknowns and in-game probes

Run on the sim thread via the gp_process hook, in a test build.

| # | Unknown | Probe | Settles |
|---|---|---|---|
| P1 | Runtime state/pause flags | Log `State_stack`, top `State_info` flags, `Game_paused_count` while opening Esc, map, upgrades, handbook, briefing, death screen, msgbox | Confirms the pause table |
| P2 | Live Esc menu in SP | Skip `game_pause` when called from `ingame_options_enter`; open Esc mid-fight | World runs behind menu, count balanced |
| P3 | Live map | After push 37 set `State_info[37]` +0x18=1/+0x19=0, skip its pause | Sim under map, input leak, FPS |
| P4 | Alt-tab off | Patch call 0x4DDB58 to return 1 | Game keeps running, no background input |
| P5 | Event volume / divergence | Log `gem_post_event` type+payload in DFA, twice | Channel size, non-determinism |
| P6 | Either-player trigger | Swap `player_in_radius` callback to accept an NPC; move the NPC into a "go here" radius | Step completes |
| P7 | Reload-free respawn | Local `Player` dies: set `+0xB8 |= 8`, then `player_respawn` 0x6DF5F0 after 3 s | HUD, weapons, camera OK, no death screen |
| P8 | Guest video path | Free roam: `video_cutscene_play("RFG_CINE_02A.bik", -1, 0,0,0,0,0,0)` | Returns to gameplay without reload |
| P9 | Objectives without a mission | `add_objective(i)`, `complete_objective(i,1)` | HUD shows and completes |
| P10 | Overhead tag in SP | `hud_icon_add(npc)`, set text, display_distance, snap_to_edge | Renders and tracks |
| P11 | Partner blip | `cartographer::add_icon(npc, STAR_GREEN, 1,0,0)`, then `remove_icon` | Blip follows, cleans up |
| P12 | Choke point coverage | Log return address + args of `set_pending_territory_load` for death, retry, cancel, fast travel, replay, tutorial video | One hook covers all |
| P13 | Puppet-as-player danger | Spawn a second subtype-3 player, kill it in and out of a mission | Mission fail / autosave / death screen on host |
| P14 | Guest activity fence | `activity_suspend_activities(5)`, roam past activities and raid triggers | Whether `do_frame_spawn_new_raid` still spawns |
| P15 | Mission-start warp | Host starts DFA with guest ~1 km away; guest `human_teleport`s | Streaming OK, no area warning |

Still open without a probe yet: what visible content managed layers hold (guest may need "load layers, don't run
scripts"); `Reload_level_on_death` default; meaning of `Mc_info+0x8`; whether the death autosave hits disk each time;
guest rewards/unlocks; per-mission soft-locks when only the guest acts (audit `player_in_*` uses in the 76 managed
scripts); `srm_mission_reservation` vs. guest puppets; video skip/completion signal for the barrier.
