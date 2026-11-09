# RfgCoop notes: Geo-Mod destruction synchronization

Domain: how Geo-Mod destruction is produced, applied and persisted, how stock MP syncs it, and what campaign co-op should do.
Distilled 2026-10-07 from `research/findings/workflows/coop_destruction.md` (the investigator report) plus the skeptic
verdicts on claims C1-C8. Build: GOG rfg.exe, 32-bit, PDB image base 0x400000. All addresses are **RVAs** unless marked VA
(VA = RVA + 0x400000). Tools: the PDB, disassembly, the symbol/xref tables and the `rfg.asm` cache.
The xref table columns are `target RVA, kind, site RVA, containing function`. Nothing here was run in game.
Tags: **[V]** = read in code/PDB and re-checked by a skeptic. **[V1]** = read by the investigator only, not contradicted,
not independently re-checked. **[I]** = inferred, not read.

## TL;DR

- Live Geo-Mod destruction is (almost all) queued as `rfg_collision_event` entries in `RFG_dyn_collisions` (0x1CC12F8) and
  applied in one function, `rfg_dyn_do_collide_post` (0x340710). In SP its only caller is `postCollideCallback`
  (0x4102E0). That is the co-op hook point for both host capture and client replay. [V]
- Stock MP syncs destruction by forwarding events (packet 0x5D) and re-simulating on clients, and binds new chunk movers by
  creation order. **Every stage is gated on `Multiplayer` (0x2BEA588) and mostly on `Server`, so in the SP campaign none
  of it runs.** Co-op must rebuild capture, transport, client drop and chunk-identity mapping itself. [V]
- Do not set `Multiplayer=1`: salvage turns off, campaign movers have `mp_info == NULL` (only allocated at load time when
  the flag is already 1) and the host's MP paths would queue `DET_MP_DESTROY`, which dereferences it. [V]
- Not everything goes through the queue: saved/streamed shape state (`rfg_mover::stream` -> `decompress_state`),
  `world::destroy_object`, immediate slices. Gameplay consequences of "building destroyed" fire from several paths
  (`do_frame_report`, `general_mover::apply_damage`, nano dissolve, mission script actions), all locally on each machine. [V]
- Joining and zone streaming need state snapshots (host save, zone stored state), not event replay: MP's replay is capped
  at 400 frames, and chunk uids/handles are per-session and never saved. [V]

## Facts

### Event model
- **Types** (`destruction_event_type`): EXPLOSION 0, DIRECT_DAMAGE 1, SP_ARRAY 2, OBJECT_SIMPLE 3, OBJECT_COMPLEX 4,
  SHATTER 5, SHAPE_CUTTER 6, REPAIR 7, MP_DESTROY 8, MP_VEHICLE 9, SHARDIFY 10, DESTRUCTION_MODE 11, LINK_BROKEN 12,
  SLICE 13. [V1]
- **`rfg_collision_event`** (0x110) = `rfg_collision_event_base` (0xEC) + `rfg_collision_havok_base` (0x24 at +0xEC). [V1]
  | Off | Field |
  |---|---|
  | +0x00 | `base_a` (0x58): raw local point/normal +0x00/+0x0C; compressed form +0x18..+0x33 (u16 `cobj_network_handle` +0x30); `collision_point_local` +0x34; `collision_normal_local` +0x40; `shape_key` +0x4C; `obj_handle` +0x50; `misc` +0x54 |
  | +0x58 | `base_b`, same layout (contacts: `shape_key` +0xA4, `obj_handle` +0xA8) |
  | +0xB0 | `destruction_type` [V] |
  | +0xB4 | damage / radius; +0xB8 `exp_unique_id` / repair duration; +0xBC `exp_radius_multiplier`; +0xC4 `owner` |
  | +0xC8 | u8 flags: `&3` skip (dead) [V], `&4` do not send to MP frame [V] |
  | +0xC9 | `data_count`; +0xCC union: `u16 direct_sps[15]` / `link_indices[7]` / ... |
  | +0xEC.. | `affected_subpiece_a/b` raw pointers, count, `hkpDynamicsContactMgr*`, ids: machine-local, never send |
  Geometry is in the target mover's local space. [V1]
- **Queue** `RFG_dyn_collisions` 0x1CC12F8 (1024 x 0x110 = 278528 bytes), count `Num_rfg_dyn_collisions` 0x1AF7268,
  lock `Collision_event_wrapper` 0x1B16350 (`cs_wrapper`). Producers check the 0x400 cap (stress 0x3292EE, slice 0x339C5A). [V]
- **Producers** (all write the queue) [V unless noted]:
  - `rfg_dyn_add_pre_collide_contact` 0x32DD30 (Havok contacts, types 3/4); network variant
    `rfg_dyn_multi_add_pre_collide_contact` 0x32DB20 `bool(object*,object*,unsigned,vector*x4,unsigned shapekey_a,unsigned shapekey_b,bool)`. [V1 signature]
  - `rfg_dyn_add_pre_collide_explosion` 0x313700 `bool(rfg_mover*,object*,explosion_info*,vector*,vector*,float,bool from_network)`,
    called from `object_mover_process_explosion_hit` 0x6A2390 (pushes from_network=0 at VA 0xAA24EB).
  - `rfg_mover::apply_damage` 0x334D30: `next_free_collision_event` (0x313310) at 0x334F2C / 0x335195 / 0x3355B7 (bullets, melee; type 1).
  - `rfg_dyn_add_pre_collide_direct_damage` 0x313950; `_shatter` 0x313EE0 [V1]; other `_add_pre_collide_*`: link_broken
    0x313B30, sp_array 0x313D30, repair 0x314060, shardify 0x314350, slice 0x314630, shape_cutter 0x31C1C0, mp_vehicle
    0x3137E0, destruction_mode 0x314470, mp_destroy 0x3141F0. [V1]
  - Stress: `stress_evaluation_process_object` 0x328E20 writes a type-0xC event inline (`mov [esi+0xB0],0xC` at 0x329340)
    under the lock. Stress is a frame-sliced state machine gated by `gameplay_should_run_stress` 0x374650. [V; gate V1]
  - `shape_cutter::do_cut` writes type 6 at 0x328657. [V]
  - `rfg_dyn_slice_shape` 0x339960 `bool(rfg_mover*,rfg_destroyable_shape*,vector*,bool immediate)`: immediate=0 (from
    `rfg_dyn_do_frame_pre`, push at 0x33A8F6) queues type 0xD at 0x339C7D; plane from `fl_rand_range` (0x339B46/0x339B7F).
    immediate=1 (from `rfg_dyn_apply_single_object_collision`, push at 0x339FC1) applies at once via
    `rfg_dyn_add_collide_slice_immediate` + `rfg_dyn_apply_slice` (0x339BCD/0x339BD9), **no queue entry**, but only while a
    queued event is being applied. [V]
  - `rfg_mover::script_point_destroy` 0x327010 -> `get_sp_array_event` 0x313520 (lock + queue scan). [V]
- **Apply pass** `rfg_dyn_do_collide_post` 0x340710 (cdecl void) [V]:
  - SP: count = `Num_rfg_dyn_collisions` (read 0x340787); skip `flags&3` (0x3407B0); jump table at 0x3407CC (VA table
    0x740A4C) on types 0..0xD -> `rfg_dyn_apply_explosion` 0x333030, `_direct_damage` 0x333780, `_link_broken` 0x333870,
    `_sp_array` 0x3339E0, `_shatter` 0x333AD0, `_shape_cutter` 0x333BC0, `_repair` 0x333F60, `_object_simple` 0x33A190,
    `_object_complex` 0x33A260, `_mp_destroy` 0x31C170, `_mp_veh` 0x334380, `_destruction_mode` 0x3246A0, `_slice` 0x334570.
    Each top-level apply has this function as its only caller (except `_slice`). Then shardify (0x3408E2-0x340944) from a
    **separate** mover list (`alloc_rfg_mover_process_shardify_list`): `rfg_dyn_shardify_setup` 0x338040, `_work` 0x33CD70,
    `_cleanup` 0x33F660. Then count = 0 at 0x340A2B.
  - `postCollideCallback` 0x4102E0 is just `call rfg_dyn_do_collide_post; ret 8`.
  - MP-only extras: `rfg_dyn_mp_do_collide_post_setup` 0x33AA50 (call 0x340742) picks the count; server early-exit in
    pregame; server append at 0x340894; `rfg_dyn_do_mp_collide_post_finalize` 0x314D30 (call 0x340A04, skipped by
    `cmp Multiplayer,0` at 0x3409F3); a non-migrating MP server does not reset the count.
  - Four more callers, all MP-only: `multi_game_destruction_jip_sync_process` (0x5DE469),
    `process_migration_client_wait_for_destruction_applied` (0x5DEFBE), `process_migration_destruction` (jmp 0x555ABE),
    `process_migration_server_apply_destruction` (0x599A7E).
- **Not via the queue** [V]: `rfg_mover::stream` 0x340B70 -> `rfg_mover::decompress_state` 0x33C370 (call 0x340CF6, its only
  caller) edits shapes directly (`rfg_destroyable_shape::set_shape_state` x4, `remove_link`, `restore_link`,
  `create_world_anchor`); `world::destroy_object` (e.g. `rfg_mover_multi_registration::process_destroy_packet` 0x57F530);
  MP `rfg_mover_multi_registration::apply_compressed_state` 0x5FA1E0; immediate slices (above).

### Stock MP sync (all MP-gated)
- **Roles**: `box_acts_as_client_for_destruction` 0x3119C0 = MP && (!server || migrating); in SP it returns 0.
  `box_acts_as_server_for_destruction` 0x311A30 [V1]. `multi_is_server` 0x5431D0 = `mov al,[Server 0x2BEA590]`. [V]
- **Client drop gate**: `rfg_dyn_add_pre_collide_explosion` begins `Multiplayer? -> multi_is_server -> migrating ->
  cmp from_network,0 -> return 1 (dropped)`. With `Multiplayer==0` it jumps straight to the enqueue path (0x313729). Contacts:
  `box_acts_as_client_for_destruction` at 0x32DFA8 (except walker-arm / `Allow_mp_vehicle_mover_collision` 0x1AF7265). [V]
- **Server capture**: `multi_game_destruction_frame_server_add_destruction_event` 0x5F0620 (prepare frame -> add -> inc
  `Destruction_packet+0x2`). Call sites: 0x340894 in `rfg_dyn_do_collide_post` (skips `flags&4`) and 0x32E2FB (most likely
  `maybe_add_shardify_event` 0x32E250; one verdict placed it in `rfg_dyn_add_pre_collide_contact`). Both gated on MP &&
  server && !migrating. [V]
- **Packet**: `_destruction_packet` 0x20C, global `Destruction_packet` 0x2BE7E60 [V1]; `+0xA _packet_size`, `+0xC
  uint8_t[512] _packet_data`. Byte +0xC = 0x5D (`start_packet` 0x555020), length at +0xD/+0xE, payload from +0xF, so
  **≤ ~509 bytes of events**. `finalize_packet` 0x5C4400 runs `multi_game_destruction_process_packet` locally (0x5C45ED)
  and `multi_io_send_reliable` per connection (0x5C4636); its only caller is `rfg_dyn_do_mp_collide_post_finalize`. [V]
  Explosion encodes to 80 bits (0x552C00); link_broken 3+2n bytes (0x553E90). [V1]
- **Quantization**: `multi_game_destruction_set_*_working_data` (explosion 0x5C3470, called at 0x3137C6 at enqueue time)
  compress+decompress only when `Multiplayer && Server`; otherwise copy raw floats. `set_link_broken` 0x5C35F0 does a
  handle round trip instead. All 13 `multi_destruction_compress_*/decompress_*` test `Multiplayer` first and skip the
  network-handle conversion when it is 0 (e.g. compress_explosion 0x5BDC00: `je` at 0x5BDC09 over the call at 0x5BDC1A). [V]
  `multi_core_network_handle_from_game_handle` 0x5B63F0 returns 0xFFFF for objects not in the MP hash tables. [V]
- **Client replay**: `multi_game_destruction_frame_client_get_destruction_events` 0x5C4A00 -> `add_destruction_events_from_packet`
  0x5C4670 -> `process_destruction_event_explosion` 0x5C38A0 -> `rfg_dyn_add_pre_collide_explosion(..., from_network=1)`
  (push 1 at VA 0x9C3920). [V]
- **Server throttle**: `rfg_dyn_mp_do_collide_post_setup` sorts with `destruction_compare` 0x314C30 (qsort at 0x33AB39), lets
  ≤ 5 live events per Havok step through (`cmp ebp,5` 0x33ABAE), 360-step history window. [V1] Remaining events deferred:
  INFERRED from a memmove call at 0x314E46.
- **Chunk binding by order**: `multi_game_destruction_frame_client_process_new_rfg_mover_remote_objects` 0x5DE1D0
  `void(rfg_mover** created, int count)` compares frame+0x4 with the local count; on mismatch only
  `keen::debug::nativePrintArgs` (0x5DE22F), then loops over the **local** count pairing `created[i]` with the i-th 3-byte
  server entry (`remote_object_allocate`, `multi_core_dc_create_master_object`). Skips registration if
  `Object_type_registration_list+0x1C` is null. Caller: `rfg_dyn_do_mp_collide_post_finalize` 0x314F68-0x314F74 with
  `Mover_destruction_results` (0x1A83090, 416 bytes) +0x60 / +0x4. [V]
- **Created list**: `add_rfg_mover_to_created_list` 0x312600 (returns if `Multiplayer==0`, cap 0x14) has **no callers**; the
  live writer is an inlined copy in `rfg_dyn_shardify_cleanup` at 0x33F751-0x33F76B with the same gate and cap. In SP the
  list is never filled. [V]
- **JIP**: pool of 500 frames (`Num_free_destruction_frames=0x1F4`, `multi_game_destruction_level_init_pre` 0x57B400).
  `multi_game_destruction_sync_jip` 0x5DE600 (called with range 0..Active-1) -> `multi_game_migration_send_client_destruction_update`
  0x5C56D0 (packet 0x61 per frame) + `multi_game_object_mover_send_destroyed_movers_list` 0x5C8640 (packet 0x55, list of
  destroyed-mover network handles). No geometry snapshot. `prepare_destruction_frame` 0x5F0580: at active frame ≥ 0x190
  clears `jip_destruction_sync_possible`, disconnects (reason 0x11) connections with `[conn+0x58]` (sync pending) set.
  The flag gates `multi_allow_join_in_progress` 0x571610, `multi_game_player_may_join` 0x57B9C0, `does_game_state_allow_joining`.
  Same cutoff in `multi_game_destruction_update_jip_status` 0x5DE580 (callers: round over, `rfg_dyn_apply_repair`). [V]
- **Vehicles**: `multi_game_auto_predict_collision` 0x59E480 (server lookahead using client ping, queues DET_MP_VEHICLE);
  tunables 0x127E218/0x127E21C/0x127E220. [V1]
- **MP mover info**: `multi_rfg_mover_info` (0x88; +0x84 `destroy_causes`, +0x86 `last_destruction_frame`) at
  `rfg_mover+0x110`, allocated only in `multi_game_object_rfg_mover_created` 0x59D090 (only code ref to
  `Rfg_mover_info_allocator` 0x16920B0; stores before NULL test at 0x59D0A1), whose only caller is `rfg_mover::serialize`
  at 0x3261DF behind `cmp Multiplayer,1` (0x3261D1). [V]
- Debug validation (`multi_send_validate_destruction` 0x5C4E40, packet 0x63) is reachable only from a debug command;
  `object_mover+0xB0 destruction_checksum` exists. [V1]

### Why Multiplayer=1 is not an option [V]
- `salvage_should_spawn` 0x61F0D0: `cmp Multiplayer,1 -> return 0`; callers `explosion_create_internal`, `rfg_dyn_shardify_work`.
- `rfg_dyn_apply_mp_destroy` 0x31C170: for type +0x7E==2 / subtype +0x7F==1, `mov eax,[esi+0x110]` (0x31C19C) then
  `or byte [eax+0x84],2` (0x31C1A6) with no NULL test, then `world::destroy_object`.
- `rfg_dyn_add_pre_collide_mp_destroy` 0x3141F0 has SP-reachable callers gated on MP && server:
  `object_mover_process_explosion_hit` (0x6A267E), `object_mover::handle_fading` (0x6A2737),
  `automobile::check_wheel_contact_with_ground` (0x7885AA), `deferred_delete_list_post_process` (0x578B0D). Flipping the
  flag at runtime on the host leaves all loaded campaign movers with `mp_info==NULL` -> crash. As a non-server, the gates
  drop all local destruction.
- `rfg_mover::stream` sets regrow-on-stream (bit19) only when `!Multiplayer` (0x340FF1-0x34100C).

### Campaign building data and consequences
- `rfg_mover` (0x16C) over `object_mover` (0xF8): +0xF8 `move_type`, +0xFC `damage_pct`, +0x104 `state_data`, +0x108
  `game_destroyed_pct`, +0x10C `rfg_mover_flags`, +0x110 `mp_info`, +0x130 `salvage_num_pieces`, +0x134 `destruction_uid`.
  `object_mover`: +0xB0 `destruction_checksum`, +0xCC team, +0xD0 control, +0xE0 `mover_flags`. [V for the offsets used
  above; rest V1]
- `rfg_mover_flags`: 3 invulnerable, 4 game_destroyed [V1], 17 mining, 18 supply_crate, 19 regrow_on_stream_begin,
  20 regrow_on_stream_finish, 22 rebuild, 23 propaganda [V].
- **Destroy triggers in `rfg_mover::do_frame_report` 0x31F2D0** [V]:
  - Path A: `damage_pct (+0xFC) >= game_destroyed_pct (+0x108)` -> `set_game_destroyed` (0x31F523) + `object_mover::drop_control` (0x31F52D).
  - Path B: `fl_greater_than_equal_robust(+0xFC, const @0x12D6B58)` -> `set_game_destroyed` (0x31F4E7), no drop_control.
  - 0x31F5DA: `set_game_destroyed_common(..., 0)` when +0x108 > +0xFC (reverse / not-destroyed direction).
- `set_game_destroyed` 0x316DF0: HUD (`hud_ui_mission_set_building_destroyed`), `gfm_propaganda_morale_change`,
  `world::object_add_to_destroyed_table` 0x51C830, `stats_increment_by_one`, mining / supply-crate meta. Mutually calls
  `set_game_destroyed_common` 0x316CA0: `object_mover_maybe_add_to_destroyed_event_queue` 0x65F6F0,
  `activity_register_building_destroyed` 0x359DF0 (its only caller), `mission_is_active` + `is_objective_handle`, effects. [V]
- `object_mover::drop_control` 0x6139E0: destroyed table + `gfm_change_edf_control` x2. [V]
- **Other consequence paths (not do_frame_report)** [V]: `general_mover::apply_damage` 0x68A920 (destroyed queue 0x68AC5B,
  `activity_register_death`, `drop_control` 0x68AC6C, HUD 0x68ACA1; MP branch 0x68AB84); `maybe_start_nano_dissolve`
  0x6A2090 (destroyed queue 0x6A2110); script actions `Action_callback_lower_edf_control` 0x721B90,
  `_lower_edf_control_at_object` (0x721C12), `Action_callback_building_destroyed_message` 0x721980.
  `gfm_change_edf_control` 0x3B0010 has 8 callers (also `apply_standard_rewards`, tanks-a-lot, `monitor_bureaucrats`).

### Persistence [V]
- Destroyed table: `world+0x4C8` `hash_table<unsigned,...,1024,unsigned short>`; `world::destroyed_object_table_save`
  0x52CEA0 [V1]; `world::object_was_destroyed` 0x52CE90.
- `rfg_mover::serialize` 0x325D60 `bool(obj_prop_block*, bool load)`. Save branch: `compress_state` 0x31E930 (0x326212),
  WORLD_ANCHORS, DYNAMIC_LINKS, SHAPE_STATE, OM_PROP_DECAL_LIST, FLAGS, MOVE_TYPE, DAMAGE_PERCENT, SALVAGE_NUM_PIECES,
  GAME_DESTROYED_PCT, STREAM_OUT_PLAY_TIME. **Never DESTRUCT_UID.** Load branch only reads into `state_data` (+0x104); the
  shape is applied later by `rfg_mover::stream` -> `decompress_state`.
- `rfg_mover::serialize_mode` 0x316880 -> OSM_NONE (0) when not physically in original zone; or non-FIXED, byte +0x6E==0xFF
  (runtime handle) and damage_pct > 0.999 (double at 0xED6430); or mover_flags bit2 + script point effect. OSM_RESTORE (1)
  when flags bit7 and FIXED; else OSM_FULL (2).
- Reset: `world::serialize_zone` 0x53ECE0 tests `[World+0x265C]&2` (`save_load_info.reset_destruction`) at 0x53F31C and
  0x53F519 -> `world::should_restore_object_if_resetting_destruction` 0x52D980 ->
  `rfg_mover::serialize_restore_if_resetting_destruction` 0x316950 (false if `flags&0x860000` or invulnerable). The flag
  comes from the save/load UI option: `Save_load_reset_destruction` 0x280712C is only touched by `hud_ui_save_load_*`.
- Regrow/rebuild in `rfg_dyn_do_frame_pre` 0x33A4D0: bit20 -> `rfg_mover_regrow_finish` (0x33A92E), bit19 ->
  `rfg_mover_regrow_begin` (0x33A94C), bit22 -> `world::restore_object` (0x33A985). Bit19 set in `rfg_mover::stream` only if
  `mover_flags&1 && !Multiplayer && !wrecking_crew_is_active`. Runs locally per machine.

### Handles and identity [V]
- Zone objects: `world::serialize_zone` -> `world::alloc_object_stub_preexisting` 0x53B3C0 (call 0x53F35C) with the zone
  file's handle: same on every machine.
- Chunk movers: `rfg_dyn_shardify_work` -> `world::create_object_internal(class, props, zone=0, -1)` (0x33D2B9-0x33D2C8);
  the -1 is the **parent** handle (obj+0x70). Own handle from `world::alloc_object_stub` 0x53B2E0 ->
  `handle_manager<7280>::allocate_handle` 0x51F160 (rotating bitmap cursor): allocation-order dependent, differs across machines.
- `destruction_uid = ++Rfg_mover_destruction_uid` (0x1A3733C, lock xadd at 0x33D28E) -> `obj_rfgm_props` ctor 0x31B020 ->
  `rfg_mover+0x134`. Counter reset to 0 in `rfg_dyn_level_init_pre` (0x32D988, with `General_mover_destruction_uid`
  0x1A37330), never saved; per-chunk uid never saved either. Only 3 code refs to the counter.
- MP looks chunks up by uid: `rfg_mover_find_by_network_handle_and_destruction_uid` 0x5CC1F0 (`cmp [obj+0x134],uid`, falls
  back to network handle).

## Approach for co-op

Host-sequenced event replication (MP's model, rebuilt without `Multiplayer`), plus snapshots and host-only consequences.
All of this is INFERRED design built on the facts above.

1. **Host capture**: hook `rfg_dyn_do_collide_post`. Before calling the original, copy `RFG_dyn_collisions[0..Num)` under
   `Collision_event_wrapper`, skipping `flags&3`. Send per step: type, target handle(s), shape keys, local point/normal as
   raw floats (SP does not quantize, so send what the host applies), damage/radius, `exp_unique_id`, radius multiplier,
   owner (remapped), `data_count` + data union. Never send +0xEC.. (pointers). Read the `Rfg_mover_destruction_uid` delta
   across the call to list the chunk movers this step created (and their host handles). Reliable, ordered, step sequence
   number, zlib when large. Optional MP-style cap (sort with `destruction_compare`, ≤ N per step).
2. **Client replay**: hook the same function. Under the lock, discard locally queued events, inject the next host batch
   (via `rfg_dyn_add_pre_collide_*`, which accept any caller when `Multiplayer==0`, or by writing queue entries directly),
   call the original, then pair new chunks (uid delta, in order) with the host's list in a `host_handle <-> client_handle`
   map. Skip events whose target is not loaded and mark the zone dirty. Never inject DET_MP_DESTROY; use
   `world::destroy_object` replication for whole-object removal. Consider disabling client stress
   (`gameplay_should_run_stress` -> false) once probes show it is safe.
3. **Consequences on host only**: on the client suppress `set_game_destroyed(_common)` side effects, `drop_control`,
   `general_mover::apply_damage` consequences, nano-dissolve destroyed events and script EDF-control actions (or suppress at
   `gfm_change_edf_control`, `activity_register_building_destroyed`, `object_mover_maybe_add_to_destroyed_event_queue`,
   `world::object_add_to_destroyed_table`). Replicate from host: destroyed-table delta, `game_destroyed` per handle,
   district control, morale, stats. Effects stay local.
4. **Snapshots**: join = host save loaded on the client. Zone hand-off = host sends `stored_zone_state` for zones it streams
   out that the client lacks (shape state is only re-applied via `rfg_mover::stream`). Drift = periodic per-mover
   shape-state hashes, resync by stream-cycling (needs P7). Make sure neither side loads with "reset destruction".
5. **v1 constraint**: keep players within one streaming radius. Vehicle ramming on the client needs prediction or a port
   of MP's host lookahead.

## Contested / refuted (do not rely on)

- "**All** destruction goes through the queue, single caller": false in general. Stream-in `decompress_state`,
  `world::destroy_object`, immediate slices and the separate shardify list bypass it; four MP-only callers exist. (C1)
- "MP's sync gives identical quantized values on both ends" in co-op: false. With `Multiplayer==0` nothing is captured,
  dropped or quantized. "≤ 512-byte payload" is really ≤ ~509. (C2)
- "The compressors need MP network handles": wrong as stated. They skip handle conversion when `Multiplayer==0` (they then
  write no portable +0x30 handle). They are still not useful: co-op needs its own identity mapping. (C4; report §2.2/F11)
- "`add_rfg_mover_to_created_list` 0x312600 fills the list": it has no callers; the live code is inlined in
  `rfg_dyn_shardify_cleanup`. Conclusion (empty in SP) unchanged. (C3)
- "Every consequence comes from `do_frame_report` when damage_pct ≥ game_destroyed_pct": two triggers there, and several
  other paths (C5). Suppressing only `do_frame_report` paths is not enough.
- "MP JIP has no state snapshot": it also sends a destroyed-mover handle list (packet 0x55). Still no geometry. (C6)
- "Each chunk's uid is saved" (report F22, Risks): **refuted**, the save branch never writes DESTRUCT_UID. The report's
  "-1 = fresh handle request" is also wrong: -1 is the parent handle. (C8)
- "reset_destruction is probably load game / checkpoint" (report F21): it is a save/load UI option. (C7 verdict)
- "Every randomized decision is made before queuing" (report F4): verified only for queued slices. The apply path calls
  `fl_rand_range`, `int_rand_chance`, `timestamp::set_rand` (INFERRED from C1 verdict) and shardify, so replay may not be
  deterministic.
- 0x32E2FB's containing function: `maybe_add_shardify_event` vs `rfg_dyn_add_pre_collide_contact` (verdicts disagree;
  symbol order favours the former).
- All bandwidth figures in the report are guesses.

## Unknowns and the probes that settle them

- **P1 SP injection**: call `rfg_dyn_add_pre_collide_explosion(building, player, &info, &local_pt, &local_dir, 1.0f, true)`
  on a looked-at building with `Multiplayer==0`; a hole must appear. Static analysis says it works; untested.
- **P2 rate/size logger**: hook `rfg_dyn_do_collide_post`, log per step `Num_rfg_dyn_collisions`, type histogram,
  `data_count` sum, uid delta, during normal play, a vehicle ram and a full building demolition. Gives real bandwidth.
- **P3 determinism (biggest unknown)**: quicksave, record pre-apply events while destroying a building, reload, drop local
  events and inject the log; compare per-mover shape hashes, created-chunk counts per step, `damage_pct`/`game_destroyed`.
  Repeat at different FPS caps, stress on/off, then across two PCs. Settles whether rand in the apply path and shardify
  pool budgets break replay and uid-order pairing.
- **P4 blob sizes**: log `world_zone+0x68 stored_zone_state` sizes (+0xC8 buffer, +0xCC size) and save size after demolition.
- **P5 chunk lifetime**: count chunks by uid for 60 s after a collapse (are they worth pairing?).
- **P6**: read `rfg_mover+0x110` on campaign buildings; expect NULL.
- **P7 live resync**: on a damaged static building call `rfg_mover::stream(false)` then `stream(true)`; then try swapping
  the SHAPE_STATE buffer between the calls.
- **P8**: confirm `World+0x265C` bit1 only follows the save/load UI option; check checkpoint restore and mission fail.
- Open: are events past MP's 5/step cap deferred or dropped (only relevant if we copy the cap); does client stress update
  bookkeeping later steps read; salvage dedupe (items domain); does any mission read per-subpiece state; whether the
  gem `building_destroyed` event is posted from `Mover_destroyed_event_queue` (INFERRED, not traced).
