# RfgCoop notes: world streaming, world state, saves, join-in-progress

Distilled 2026-10-07 from `research/findings/workflows/coop_world_save.md` (the report) and the skeptic verdicts on
claims C1-C8. Static research only; nothing was run in game.

Conventions
- Addresses are **RVAs** (runtime = `GetModuleHandle(NULL) + RVA`; the disassembly prints VA = RVA + 0x400000).
- `World` = the static `world` object at RVA 0x02B97490; `World+X` is a field of it. `Multiplayer` = bool at 0x02BEA588.
- **VERIFIED** = read in disasm, PDB, data bytes or source. **INFERRED** = reasoned, not read. Everything in "Facts"
  is VERIFIED unless it says otherwise.

---

## TL;DR

- **One stream centre.** Streaming follows the camera (`World+0x2710`). Normal radii are 340 m (physics, cells) and
  510 m (zones), but the game **shrinks them at runtime** under stream-memory pressure (down to 102/168.3 m). No
  dual-point streaming, so co-op needs a leash (or puppets the host does not simulate).
- **Join = the engine's own reload-from-memory path.** The host serializes its world into `stored_world_state[0]`
  without touching disk; the client copies that blob into its load buffer and calls `platform_load` +
  `set_pending_territory_load("",...)`. No MD5 is checked, the client's save file is untouched.
- **The blob carries the host's player sections** (inventory, salvage, upgrades, difficulty, EDF mover restore). The
  client must re-apply its own afterwards.
- **SP death must be replaced.** The death branch fires for *any* player-subtype object whose death timer elapses
  (no local-player check), then autosaves to disk and reloads the territory (or fails the mission). Intercept it at
  `player_is_killed` / the start of the branch, and respawn in place.
- **Save guard needs more than `game_save_write`.** That hook blocks campaign saves and autosaves; import, delete,
  options and profile writes call `game_save_write_thread` / `startSave` directly. `startSave` (0x811240) is the
  single point every disk write passes.

---

## Facts

### 1. World streaming

**F1. Single centre.** `world::do_frame` (0x540950) writes `World.stream_pos` (+0x2710) =
`Stream_follows_player ? Player->pos : Rfg_camera+0x2C` (cmp at 0x5409A4). `Stream_follows_player` (0x28EB1EC) is 0
in the image and has only two reads (0x5409A4 do_frame, 0x540B54 stream_hard_wait), no writes. So streaming always
follows the camera. `world` has one `stream_pos`; `+0x270C` is the `stream_grid*`.
- do_frame does nothing while `Stream_pause` (0x28A0670) is set, while `World+0x24E0` deserialize_state != DS_IDLE,
  while a GS_LOAD is pending or while `performing_save_state` is set.
- Order per frame: `monitor_leaving_zones` (0x52E180), `monitor_grid` (0x534EF0), `monitor_entering_zones`
  (0x540530), `process_streaming_objects` (0x53F9E0), restore (`restore_do_frame_post` 0x5402D0),
  `process_deserialization` (0x53F690).
- `world::stream_hard_wait` (0x540B00) runs the same pipeline in a blocking loop until stream2 is idle; used by
  `cs_play_internal` and `world::post_load_stream`.

**F2. Layers.** `stream_grid` (0x24): `+0 stream_layer* layers` (2 layers), `+0x14 bool compact_super_emergency`,
`+0x18 compact_ts`, `+0x1C compact_wait_ts`. `stream_layer` (0x4C0): `+0x10 cell_load_distance`,
`+0x14 float stream_radius`, `+0x30/+0x3C stream_bmin/bmax`, `+0x48 farray<stream_grid_cell*,25> active_cells`
(count +0x54), `+0xBC masked_cells[50]`, `+0x4B4 cell_dim`. Layer 1's radius is therefore at grid-layers+0x4D4.
- `stream_layout_level_load` (0x537B40) writes 340.0 (const 0xED0248) to layer0+0x14 at 0x53813B and 510.0
  (0xEEBF40) to +0x4D4 at 0x538222.
- Max 25 active cells per layer (`mov ecx,0x19` at 0x530F2B in `stream_grid::do_frame` 0x530E50).
- `stream_grid::set_stream_radius` (0x51AD30) has no callers and no pointer references.

**F3. Radii change at runtime (memory pressure).** `stream_grid::do_fragmentation_handling` (0x522000) is called
unconditionally at the end of every `stream_grid::do_frame` (0x531360). When `Stream_world_allocator` (0x1958D5C,
`stream2_dynamic_allocator`) `+0x10 frag_fail` is set:
- with `compact_super_emergency`: radii = 102.0 / 168.3 (writes 0x52231D / 0x5223FE);
- otherwise: 170.0 / 336.6 (0x522512 / 0x5225F3);
- arms `compact_ts` 6500 ms and `compact_wait_ts` 20000 ms; when `compact_ts` elapses it restores 340/510
  (0x52203B / 0x522122), releases cells with stream2 status 4 and clears `frag_fail` (0x5222CE).
- `frag_fail` is set by `stream2_dynamic_allocator::alloc` (0xAA020) at 0xAA03F (not enough free space or too
  fragmented) and 0xAA069 (alloc returned null).
- All monitors read the live radius fields every frame, so the shrink applies at once to zones and physics.
- Other writers of +0x4D4: dev commands `unused_dcf_stream_radius` (0x51D260) / `_terrain` (0x51D390), and
  `sl_build_minimal_grid` (0x530880, called from level load).

**F4. Zones.** `monitor_leaving_zones` / `monitor_entering_zones` test each zone bbox with `ix_box_box_aligned_xz`
against `stream_pos ± layers[1].stream_radius` in XZ (Y ±512, consts 0xF05BD0 / 0xED00AC).
- SP, leaving: state 2 → `world::deserialize_zone` (0x52DD10, queues it to be written into its
  `stored_zone_state`); state 1 → `world::destream_zone` (0x51D610). Both skipped when `Multiplayer` (0x52E26D,
  0x52E297) or `wrecking_crew_is_active()`.
- MP/WC, entering: if the box does not overlap, the zone is still loaded (0x540627), i.e. **every zone loads**.
- Zones lie on a 511 m grid, so at 510 m at most 3×3 zones are resident in SP
  (INFERRED from the grid, holds if zone bboxes match the grid).
- `world_zone` (0x480): `+0x18 name[64]`, `+0x58 state` (0 unloaded, 1 streaming, 2 loaded), `+0x68 stored_zone_state`
  (`world_state_buf`; buf +0xC8, cur_size +0xCC, max_size +0xD0), `+0xD8 srid`.
- `World+0x2724 world_zone* global_zone_grid[257]` (index YY*16+XX); `World+0x271C` / `+0x2720` = territory zone
  count / array.

**F5. Physics region.** `monitor_grid` (0x534EF0): SP (check at 0x534F0C) = cube `stream_pos ±
(layers[0].stream_radius − 0.01)` on x, y, z (double 0.01 at 0xECDCC0), i.e. ±339.99 m normally. MP/WC (0x5350AF) =
`bbox_expand` union of all zones with non-zero srid. Then `havok_bpo_region_update` (0x43BD30, call 0x53512C). After
that, on both paths and without any MP check, `terrain_deinstantiate_subzone` (0x535357) and
`stream_grid::do_frame(stream_pos)` (0x535394) run, so layer cells and terrain subzones still stream around the
single `stream_pos` even in MP.

**F6. Object destream.** `process_streaming_objects` (0x53F9E0) handles `havok_bpo_get_first_leaving` objects,
budget 2000 µs/frame, skipped in MP/WC. It tests `ix_box_cylinder_xz(bbox, stream_pos, layers[0].stream_radius)` and
`havok_bpo_is_forced_outside(obj+0x74)`, asks virtual +0x74 (can-destream), then calls virtual +0x1C(0) on objects
with `+0x55 & 0x10` and `stream2_release(obj+0x98)`. Meaning (INFERRED): an object outside the host's physics region
and 340 m cylinder loses its streamed resources.

**F7. NPC squads** (report A6; not re-checked by a skeptic, no verdict contradicts).
`MAX_SQUAD_LOAD_DISTANCE` 0x1256834 = 250.0, `MAX_SQUAD_UNLOAD_DISTANCE` 0xEDAD98 = 300.0,
`..._SQRD` 0x1256830 = 90000.0. `squad_member_manager_data::do_squad_simulation` (0x38D230) uses
`player_find_shortest_player_xz_dist_sqrd` (0x65CCD0), which uses only `Player` when `Multiplayer == 0` and loops over
`World.subtype_objects[3]` (elements World+0x3F4, count World+0x3FC) in MP. Siblings `player_find_nearest_player`
(0x65CAB0), `_xz` (0x65CB60), `player_find_shortest_player_dist_sqrd` (0x65CC10) presumably match (INFERRED).

**F8. Cutscenes.** `cs_play_internal` calls `world::stream_pause` and `stream_hard_wait`. A cutscene camera elsewhere
moves the host's stream centre (INFERRED from F1).

### 2. World state and how it is applied

**F9. Blob is parsed only at territory load.** `mission_system_load` (0x39D690), `activity_system_load` (0x369B50),
`script_system_load` (0x73C4C0), `human_teams_load` (0x60D230), `ui_system_load` (0x4C7800, which calls
`fog_of_war_load` 0x43FD10), `game_clock_load` (0x3C16C0), `progression_level_system_load` (0x373EB0) and
`world::restore_system_load` (0x534950) each have the single caller `world::init_world_zones` (0x53A6C0).
`activity_system_load` skips per-activity loads only when `Multiplayer` is set (0x369B70).

**F10. Runtime apply paths that do exist.**

| State | Live path |
|---|---|
| Zone object state | consumed on each zone stream-in: `monitor_entering_zones` → `process_streaming_zone` (0x5403F0) → `serialize_zone` (0x53ECE0, inflates +0xC8/+0xCC); also `restore_do_frame_post` (0x5402D0) → `restore_serialize_zone` (0x5400D0) from do_frame / stream_hard_wait. Swapping a zone's `stored_zone_state` before stream-in applies it. |
| Team dispositions | `human_teams_set_disposition(a,b,disp)` (0x60D070): writes both `Current_human_team_dispositions` entries, calls `ai_system_process_team_disposition_change`. Used by `Action_callback_set_team_dispositions` (0x7227C0). |
| Time of day | `game_clock_set_time(h,m,s)` (0x3C16F0), used by `Action_callback_set_time_of_day`, `multi_game_mode_set_time_of_day`; `game_clock_freeze_ingame_time` (0x3C1740). |
| Fog of war | `fog_of_war_update(vector pos)` (0x43F850), caller `cartographer::process`. |
| Missions, activities | `mission_activate`, `activity_base::activity_start` (0x3AC8F0), `activity_complete` (0x37F050), `mission_unlock_group::unlock_missions` (0x38E9B0). |
| Destroyed-object table | `world::object_add_to_destroyed_table` (0x51C830), `object_was_destroyed` (0x52CE90), `set_game_destroyed` (0x316DF0). |
| Districts | `district::set_control` 0x3ABD90, `set_morale` 0x35EC40, `gfm_set_edf_control` 0x3AFF40, `gfm_change_district_morale` 0x3AAFC0. |
| Tech level | `world::set_tech_level` 0x51CBF0, `set_tech_level_max` 0x51CCD0 (World+0x470/+0x474). |
| Alert | `gsm_set_alert_level` 0x3AA6B0, `gsm_set_alert_value` 0x3A4440. |

No live setter was found for scripts, progression (`Guerrilla_progression` 0x1E4BAE0), handbook, weapon cabinet or
tooltips (not exhaustively searched).

**F11. Not in the blob / drifts** (report B; unchecked claim C8, no verdict contradicts). Alert level
(`Current_alert_level` 0x1E268B4, `Current_alert_value` 0x1E248A0) is not saved: `save_state_start`/`finish` call no
`gsm_*`. Time is saved as h/m/s only (`game_clock_save` 0x3C1680; `Game_clock` 0x21044AC: +4 h, +5 m, +6 s,
+8 time_scale = 48.0) and each machine advances it locally, so it drifts. Weather is only random wind (local).

**F12. Per-player state lives on the player object.** `player_get_meta_data` (0x610550) = `Player+0x2978`.
`player_meta_data` (0x420): +0x4 salvage, +0x8 mining_count, +0xC supply_crate_count, +0x10 district_hash,
+0x14 district_time, +0x18 `upgrade_item[128]`, +0x418 play_time, +0x41C last_death_time. `add_salvage` (0x611700)
caps at 30000; `load_meta_data` (0x65DFF0) writes salvage uncapped. Inventory list at `Player+0xA2C`.
- `player_level_init_post` (0x6D58B0): `reset_upgrades` (0x6D5A96); if `World+0x24D0` save cfile is open, deletes
  every inventory item, then `player::load_inventory` (0x6D5B2E) and `load_meta_data` (0x6D5BAC), each size-checked
  (cf_tell / cf_read_uint32); mismatch → cf_close, return false.
- `load_meta_data` also calls `add_unlocked_weapons_to_weapons_cabinet`, `difficulty_load` (global difficulty),
  `local_player_fill_ammo`, `restore_edf_movers`.
- Hammers, backpacks, jetpack are process globals (`s_hammers_unlocked`, `s_backpacks_unlocked`,
  `s_jetpack_upgrade_level`), set by `platform_load` from the caller's `game_save_info`: hammers = byte +0x38 | qword
  +0x3A/+0x3E, golden hammer +0x39, backpacks word +0x42, jetpack byte +0x44.

### 3. Saves

**F13. Save image.** `Save_system_save_data` (0x21328E4) → `keen::rfg::RfgSaveData`, 0x1E0589C bytes (= RFGM.Formats
`SaveFileSizeBytes` 31479964). 12 slots, stride 0x2804D4, at +0x188: in-use +0x188, `game_save_info` (0xBC) +0x18C,
size +0x248, blob +0x24C (max 0x280010). Options at +0x1E03B78 (0x564 bytes), profile data at +0x1E044DC (0xDC0).

**F14. Disk-write paths.**
- Campaign saves/autosaves: `process_save_load` (0x3C00E0) → `platform_save_to_disk` (0x52D610; writes padding + a
  16-byte `console_md5_mem` trailer in place; refuses padded size ≥ 0x280000) → `game_save_write` (0x3D11E0, only
  caller platform_save_to_disk; copies the info into `Directory_cache`) → `game_save_write_thread` (0x3D0E40, runs
  synchronously; copies into the slot, CRCs the whole image, calls `unit4::LocalGameSession::startSave` 0x811240 →
  `keen::SaveDataHandler::requestSave`).
- Bypassing `game_save_write`: `game_save_import_save_slot` (0x3E90D0, from three `hud_ui_save_load_*` functions) and
  `game_save_import_profile_data` (0x3D99F0) call `game_save_write_thread`; `game_save_delete` (0x3C3550),
  `game_save_write_options` (0x3D1310), `game_save_write_profile_data` (0x3D13A0) call `startSave` directly.
- Every write rewrites the whole image.

**F15. sl_flags** (`save_load_info` at World+0x265C). +0x265C: bit0 pending_new_game, bit1 reset_destruction,
bit2 pending_save_game, bit4 pending_save_state, bit5 performing_save_state, bit6 pending_single_zone, bit7
save_game_warp. +0x265D: bit0 game_save_checkpoint, bit1 game_save_to_disk, bit2 pending_load_game_from_memory,
bit3 auto_save_game, bit4 saving_state_data, bit5 player_start_at_safehouse. `World+0x2690` pending slot,
`World+0x2694` dlc_bundle_id, `World+0x2698` pending territory name, `World+0x2578` cur_world_state,
`World+0x257C` `stored_world_state[2]` (stride 0x6C; buf +0x60, cur_size +0x64, max_size +0x68, i.e. World+0x25DC /
+0x25E0 / +0x25E4 for buffer 0).

**F16. `world::save_and_continue(game_save_info*, bool checkpoint, bool autosave)`** (0x51D040): returns if
`sl_flags & 0xC`; sets pending_save_game, checkpoint, auto_save; `game_save_to_disk = !checkpoint &&
!cheats_are_enabled()` (0x51D0B3); stores the slot. `process_save_load` gates `platform_save_to_disk` on
`World+0x265D & 2` (0x3C0129), so cheats really do skip disk writes. Callers: `hud_ui_mission_complete_exit`
(0x44ACE8), `activity_system_do_frame` (0x3ACC58), `hud_ui_upgrades_exit` (0x44B432), `mission_activate_pre_save`
(0x38E4D1), `mission_system_do_frame` (0x3B4D93 disk; 0x3B4DD1 = in-memory checkpoint, args null,true,true),
`gamestate_gp_simulate_frame` (0x3EDCF0, death), `world::save_and_reload` (0x51D106), `save_and_continue_warp`
(0x51D236, from `video_cutscene_do_frame`), `hud_ui_save_load_*`, `sony_options_process`, `unused_dcf_autosave_now`.

**F17. Producing a blob without disk.** Set `World+0x265C |= 4` and `World+0x265D &= ~3`; the next
`process_save_load` (from `gamestate_gp_handshake` each frame, or `world::setup_territory`) calls
`world::save_state_data` (0x541250):
- sets `cur_world_state` = 1 (checkpoint) or 0 and **leaves it set**;
- loops `world::do_frame` until DS_IDLE, sets pending_save_state + saving_state_data, loops again; returns 0 at once
  if saving_state_data is already set;
- `process_deserialization` → `save_state_start` (0x53C610): queues every zone with state 2 (loop 0x53C62D, zones stay
  loaded), zeroes `cur_size`, drops a held non-inventory weapon (`human_weapons_drop_equipped_weapon` 0x53C68C),
  `cf_open`s `stored_world_state[cur].buf` with max_size; writes WorldProperties (tech level etc.) inline;
- `save_state_finish` (0x53DA20) writes `cur_size` (0x53E227);
- `world::get_state_data` (0x51C460) returns `&stored_world_state[cur]` if cur_size > 0, else null;
- then `process_save_load` clears 0x265C bit2.
- Buffers come from `World_state_mempool` (0x195C5A8), half each, allocated in `world::system_init` (0x53BAB1..
  0x53BAEC), so they exist from boot (main menu too).

**F18. Blob format.** `"RFGS"` (0x53474652), version 0x34; start zone name; sections framed `u32 start, u32 end` in
this order: destroyed table, missions, script (zlib), progression, activities, restore list, raids, VIPs, spawn
nodes, world properties, audio, UI, restored objects, critical areas, PA kiosks, team dispositions, tracked stats,
time, per-zone {name, cur_size, max_size, zlib buf}, districts, player position, inventory, player meta data, roads
(RFGM.Formats `Saves/SaveWorld.cs` L46-L113 + the save_state_start/finish call lists). Disk blobs end with a pad to
512 and the 16-byte MD5 trailer.

**F19. Load pipeline.** UI → `verify_gamesave_start(info, reset_destruction)` (0x3E76A0; stores `Pending_save`
0x213294C, `Reset_destruction_on_load` 0x2132945, pushes GS_VERIFY_SAVEGAME 55) → `verify_gamesave_process`
(0x3F00D0, jump table 0x3F0368):
- state 3 (0x3F01F5): MD5 check (`console_md5_mem` 0x3F0226, `md5_value::operator!=` 0x3F0236);
- state 4 (0x3F0280): `memcpy(get_load_buf(), Verify_mem, Verify_size)`, `set_cur_size`;
- state 6 (0x3F0316): `difficulty_set_level_reload_overwrite(-1)`, `platform_load(Pending_save, 1)`,
  `set_pending_territory_load("", 0, 0, 0, Pending_save->+0x30, Reset_destruction_on_load)`.
- MD5 is only checked there (`console_md5_mem` callers: verify + platform_save_to_disk). `platform_load` and
  `get_load_buf` are called only by verify.

**F20. Loader functions.**
- `world::get_load_buf` (0x51C970) = `[World + cur*0x6C + 0x25DC]`; `set_cur_size` (0x51C990) writes +0x25E0.
- `world::platform_load(game_save_info*, bool)` (0x51C9B0): if arg2, `World+0x2578 = 0` (0x51C9C7). Non-null info:
  `World+0x2694 = info+0x30`, `stats_turn_off_all_cheats`, `gamelog_campaign_start_logged`, loading-screen district
  from +0xC/+0x10, hammer/backpack/jetpack globals (F12). It does **not** call `set_pending_territory_load`. NULL
  info: uses `game_save_get_autosave` (touches the save system) and on failure `gameseq_set_state(1)`.
- `world::set_pending_territory_load(name, new_game, single_zone, start_at_safehouse, dlc_id, reset_destruction)`
  (0x51C320): returns false if `World+0x24E0 != 0`, `sl_flags` bit 0x4 or 0x20, or `World+0x24D0` (save_file) open;
  otherwise copies the name to +0x2698, packs flags, `+0x2694 = dlc_id`, `gameseq_set_state(GS_LOAD=2, 0)`.
  No Multiplayer/state check; called in gameplay by `mission_cancel`, `humc_request_exit`,
  `player_death_options_continue_cb` and others.
- `world::load_territory` (0x5412D0) → `init_territory(World+0x2698, new_game, single_zone)` (call 0x541370).
- `world::init_territory` (0x53FCE0): if not new game and the extension is not `"rfgs_pc"` (string 0xF06F60), needs
  buf != 0 and cur_size > 0, then `cf_open(buf, cur_size, "rb", 0)` (0x1B5BB0); checks magic (0x53FD83) and version
  0x34 (0x53FDCC), else msgbox and fail. Its Multiplayer checks (0x53FE6B, 0x53FEDE, 0x53FF44) only affect dlc id,
  WC and script preload. Nothing checks cur_size against max_size.

**F21. SP death.** `human_do_frame` (call at 0x6E59E7) calls `player_is_killed` (0x634360) for any human with HP
(+0x914) ≤ 0, type 0 / subtype 3, state not 4/0xB, with no local/MP check. `player_is_killed` arms
`player+0x1118 dead_timestamp` (4000 ms, 0x63438A) and calls `local_player_is_killed` only if obj == Player.
`gamestate_gp_simulate_frame` (0x3EDB60) loops `World.subtype_objects[3]` (World+0x3F4 / +0x3FC, via `all_objects`
+0x64), skips destroyed (`+0x54 & 0x10`), and when the timestamp elapses sets `+0xB8 |= 8` once. If
`Multiplayer == 0` and not WC (0x3EDBE7, 0x3EDBF4) — **no object_is_local_player check**:
- mission active → `mission_terminate(0)` (no save);
- activity 0xB, 8, 9, 0xA, 1, 4 or 0x10 active → that activity's `*_process_player_death` (incl.
  `demolitions_master_process_player_death` 0x3EDF58);
- else `save_and_continue(null, false, true)` (0x3EDCF0, disk autosave unless cheats) and
  `gameseq_push_state(GS_PLAYER_DEATH_OPTIONS=16, 1, 0)` (0x3EDD05).
- `player_death_options_continue_cb` (0x492FF0): returns in MP; `Reload_level_on_death` (0x2132874, BSS, 2 reads,
  never written) → dead branch; `mission_get_mode()==4` → `set_pending_territory_load`; else
  `world::set_pending_load_from_memory` (0x51C450: `World+0x265D |= 4`) → full territory reload from the blob.
- MP only (0x3EDD2F..0x3EDD52): `multi_is_server`, `multi_timestamp +0x234C`, `gamestate_respawn_player` (0x3BFEF0,
  sole caller) → `player_respawn` (0x6DFAB0).
- In-place respawn precedents with Multiplayer false: `player_respawn` 0x6DF5F0 from
  `activity_demolitions_master::warp_player_on_end` (0x397B65); 0x6DFAB0 from `wrecking_crew_respawn_player`
  (0x39F2E1).

**F22. Mission state depends on `cur_world_state`.** Readers of World+0x2578: `mission_system_save` (0x38E3A0; saves
the active mission index only when it is 1, else -1), `mission_cancel` (0x38E68E, resets 1 → 0),
`mission_setup_do_frame` (0x39D8A7), `process_mission_information`, `hud_ui_mission_complete_process`,
`humc_request_exit`, `player_is_near_mission_start`.

---

## Approach for co-op

1. **Topology.** Host-authoritative world; only the host saves. The client never writes its save during a session.
2. **Client save guard.** Hook `unit4::LocalGameSession::startSave` (0x811240) to no-op while a session is active
   (covers every disk write, F14). Hook `game_save_write` too if you want to keep the in-memory slots untouched.
   Then test that the save system does not hang waiting for completion (probe P5).
3. **Join (host side).** Only in free roam: `mission_is_active()` false, no modal activity, `cur_world_state == 0`,
   `Stream_pause` clear, `saving_state_data` clear. Set `World+0x265C |= 4`, `World+0x265D &= ~3`, let
   `process_save_load` run, read `get_state_data()` (buf +0x60, size +0x64). Expect a held object to drop and a hitch.
   Never use checkpoint mode (writes buffer 1, the mission restart). Alternative: hook `game_save_write` on the host
   to capture a real autosave blob (includes MD5).
4. **Join (client side).** Snapshot own inventory/meta sections first (slice from its own slot, F13/F18). Then:
   `World+0x2578 = 0` → `memcpy(get_load_buf(), blob, n)` → `set_cur_size(n)` →
   `difficulty_set_level_reload_overwrite(-1)` (as vanilla) → `platform_load(&own_info, true)` with a real non-null
   `game_save_info` carrying the host's dlc id at +0x30 and the client's own hammer/backpack/jetpack bytes →
   `set_pending_territory_load("", 0, 0, 0, host_dlc_id, 0)`; **check the return value** (false = busy, retry).
   `n` must fit `max_size` (World+0x25E4); same build on both sides.
5. **After load (client).** On the first gameplay frame re-apply its own inventory and meta (and difficulty), then
   teleport next to the host, then signal ready. Keep the host paused during the client load.
6. **Live sync, host → client.** Time (`game_clock_freeze_ingame_time` + `game_clock_set_time`), alert value/level,
   district control/morale, tech level, team dispositions (`human_teams_set_disposition`), fog-of-war reveals,
   destroyed-object table entries. Missions/activities via events (other domains).
7. **Resync.** For state with no setter (scripts, progression, handbook, cabinet, tooltips) resend the host blob at
   host autosave points (any `save_and_continue` caller in F16 that runs in free roam) and reload on the client.
8. **Leash.** No dual-point streaming. Leash well inside the *current* physics radius: read
   `layers[0].stream_radius` live each frame and leash against it (normally ~250 m warn / ~300 m hard), because a
   fragmentation shrink can drop it to 170 or 102 m for seconds. Outside the region, freeze/hide the host-side
   puppet. Optional: bias the host's `World+0x2710` toward the midpoint (hook at `monitor_leaving_zones` entry).
   Optional: patch the `Multiplayer` test in `player_find_shortest_player_xz_dist_sqrd` (F7) so squads spawn around
   the remote player.
9. **Death.** Intercept on both machines before the SP branch: either keep puppets from ever arming
   `dead_timestamp` (hook `player_is_killed` 0x634360 / make puppets non-player objects) or patch
   `gamestate_gp_simulate_frame`'s branch. Respawn in place with `player_respawn` (SP precedents in F21). Mission
   death on the host can stay vanilla (`mission_terminate`) followed by a resync.
10. **Zone substitution (optional).** Send the host's `world_zone+0xC8/+0xCC` when a zone unloads; the client swaps
    its stored buffer before `process_streaming_zone` consumes it (F10).
11. **Content parity.** Both players need the same build, mods (DotB content in the campaign) and data, or zone deltas
    reference missing handles (INFERRED).

---

## Contested / refuted (do not rely on)

- **"Radii are fixed at 340/510 after level load"** (report A2, claim C1): false. `do_fragmentation_handling` shrinks
  them at runtime (F3). A fixed 330 m leash is not safe.
- **"In MP/WC nothing unloads"**: only whole zones stay resident; layer cells and terrain subzones still stream (F5).
- **"MP-style all-zones-resident is impossible for terr01"** (report A8): INFERRED only; the memory cost is untested.
  Do not cite it as fact either way.
- **"Hooking `game_save_write` alone protects the client's campaign"** (report C1/implication 5, claim C5): false;
  import, delete, options and profile writes bypass it (F14). Use `startSave`.
- **"There is no runtime apply API; missions/activities/scripts/teams/time/zones only via territory load"** (report
  B intro, implication 3, claim C3): false for zones, teams, time, fog of war, missions/activities (F10). Only the
  blob *parsing* is load-only. Report table B's "none found" for dispositions is wrong.
- **"The host autosaves only at mission complete, activity complete, upgrade shop exit"**: there are more callers
  (F16).
- **"SP death autosaves and reloads"** without qualification: only in free roam with no special activity and cheats
  off; mission deaths call `mission_terminate(0)` and do not save (F21).
- **"`gamestate_respawn_player` does the same in SP"** (report C7): false; its only caller is the MP-server branch.
- **"`platform_load` then calls `set_pending_territory_load`"** (report C4 step 6): false; it is a separate step
  (F20).
- **"The loaded blob replaces only inventory, salvage and upgrades"**: it also sets global difficulty, the weapons
  cabinet unlocks and EDF mover restore (F12).
- **`set_pending_territory_load(..., -1, ...)`**: vanilla passes the save's dlc id (`info+0x30`). -1 is only right if
  that id is -1 for the base campaign (INFERRED). Pass the host's `World+0x2694`.
- **A host blob saved in default mode during a mission** overwrites `cur_world_state` to 0, so `mission_system_save`
  records mission -1 and later restart/cancel logic changes (F22; consequences INFERRED). Restrict joins/resyncs to
  free roam.
- **`get_state_data()` freshness**: if `Stream_pause` is set when the save is requested, do_frame returns early, the
  save is deferred and `get_state_data()` may return the previous blob. Check that `cur_size` changed or that
  `saving_state_data` cleared.

---

## Unknowns and probes that settle them

| Unknown | Probe (sim-thread test build, log to file) |
|---|---|
| Blob size (bandwidth, join time); `max_size` at World+0x25E4 ≥ 0x280010? | Hook `game_save_write`, dump (buf, size, info) on an upgrade-shop exit; log `World+0x25E4`. Check RFGS/0x34 and diff against the slot parsed with RFGR.SaveEditor. |
| Silent blob works and costs what | Set the flags (F17), read `get_state_data()`; confirm save-file mtime unchanged; measure the hitch. |
| Memory injection works in game and from the main menu | Capture a blob, walk elsewhere, inject (Approach 4). Repeat from GS_MAINMENU (profile, session, `dlc_set_current_id` may be needed). |
| Cross-campaign load / per-player re-apply | Inject blob A into campaign B; check districts, buildings, missions, time; `Player+0x2978` salvage = A's; then re-apply own sections. Does `load_inventory` read its own start/end framing (caller reads two u32 at 0x6D5B04/0x6D5B17)? What is its bool? Which upgrade effects need re-applying? |
| Client spawn position after load | Log where `player_level_init_post` places the player (host's saved pos vs safehouse). |
| `startSave` no-op is safe | With the hook active, die in free roam / leave the upgrade shop; save file unchanged, no hang on `game_save_op_completed` / `Save_system_*`. |
| How often fragmentation shrinks fire | Log `layers[0/1].stream_radius`, `Stream_world_allocator+0x10`, `active_cells` count and zone states every second while driving fast, with and without extra co-op entities. Confirms the 3×3 zone claim too. |
| Out-of-range behaviour | Spawn an NPC/vehicle, move the camera away with slew cam; record the distance where it freezes, destreams, falls (300 / 340 / 510 m) per class (virtual +0x74). |
| Long teleport cost | Teleport 2 km; frames until `havok_terrain_height` (0x43B440) succeeds and `world::streaming_is_idle` (0x51C4C0); compare `stream_hard_wait`. |
| Midpoint stream bias | Rewrite `World+0x2710` 300 m away in a `monitor_leaving_zones` entry hook; check physics/streaming at the point and LOD loss at the camera. |
| In-place respawn | Call `player_respawn` (0x6DF5F0) on a dead SP player instead of death options; check health, camera, input, HUD, AI. |
| Zone-state substitution | Destroy a building in zone Z on save A, copy Z's `+0xC8/+0xCC` into a fresh load of B before stream-in; building destroyed on B? |
| Puppets as non-player objects | Can the remote puppet be an `npc` (not subtype 3) so the death loop and `Player`-assuming code ignore it? (puppet domain) |
| `console_md5_mem` = standard MD5? | Only for the verify-based fallback; call the game's function instead. |
| C8 facts (alert not saved, squad distances) | Not skeptic-checked: re-read `save_state_start`/`finish` call lists and `player_find_shortest_player_xz_dist_sqrd` before relying on them. |
