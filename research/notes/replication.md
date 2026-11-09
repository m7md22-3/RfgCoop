# RfgCoop notes: engine MP object replication and its reuse in the campaign

Domain: the engine's MP object-replication layer (`multi_core_object_*`, the per-class registrations, the remote-player
"puppet" pipeline) and what a campaign co-op mod can reuse from it. Distilled 2026-10-07 from
`research/findings/workflows/coop_replication.md` (the investigator report) plus the skeptic verdicts on claims C1-C8.
Build: GOG rfg.exe, 32-bit, PDB image base 0x400000. All addresses are **RVAs** unless marked VA (VA = RVA + 0x400000;
the left column of `rfg.asm` is a VA). Tools: the PDB, disassembly and the symbol/xref tables; the
xref table columns are `target RVA, kind, site RVA, containing function`.
Tags: **[V]** = someone read the code, data or PDB. **[I]** = reasoned, not read. Nothing here has been run in game.

## TL;DR

- **Do not set `Multiplayer` (0x02BEA588) and do not call `multi_init`.** The flag has 1663 references in 1058 functions.
  Set globally, it skips AI, pathfinding, convoys, the game clock, time triggers and ambient spawning, and it replaces
  SP death handling with an MP respawn path. `multi_init` also takes at least 47.5 MiB from `Level_memmgr`. [V]
- **The MP replication layer cannot carry campaign co-op.** Only 5 of 12 types are wired (projectile, player, item,
  multi_mover, rfg_mover), and in the campaign even those are never registered. There is no NPC replication code at
  all, and vehicle, walker and turret replication is orphaned dead code. Missions, zones and world state have no packets. [V]
- **The reusable part is the receiver-side puppet code.** Every `player` has `mp_sim_state` at +0x1274, and
  `multi_game_player_process_state` 0x5E1C50 drives a full player object from it (movement, jump, crouch, ladder,
  ragdoll, weapons, vehicle enter/exit). It is NOT self-contained in SP. The mod must call it each frame, `reset` the
  buffer, feed it with `add_state`, advance `Clock_current_time`, and **pre-fill cached game handles** for vehicles and
  turrets. Without those handles the code divides by zero in an uninitialised MP hash table. [V code / I runtime]
- **Car puppets are partial.** `multi_game_object_vehicle_automobile_apply_sim_state` 0x59D9B0 replays steering,
  throttle and brake and soft-keyframes (or hard-snaps) the body. In SP it does NOT apply handbrake, horn, lights,
  flat tyres, hp, fire or components. The mod applies those itself. [V]
- **Recommended route:** use our own transport and protocol. The host owns the world, and each peer owns its own avatar
  and the car it drives. On the receiving side we call the engine's puppet and apply functions. Remote players are created
  with the Wrecking Crew recipe, and `Player` is never touched. Every runtime assumption here must first pass probes P1-P4. [I]

## Facts

### Registration layer [V unless marked]

- `multi_core_object_registration` (0x814 bytes): +0x00 vtable, +0x04 `registration_type`, +0x08 `update_rate` (100 in
  all five live registrations; its meaning is unknown), +0x0C name, +0x10 num_masks, +0x14 `mask_info masks[64]`.
  The vtable is 23 slots: +0x04 create, +0x08/+0x0C send/process_create_packet, +0x10/+0x14 destroy packet,
  +0x18 process_object_free, +0x28 `move(handle)` (per-frame apply on the client), +0x2C get_state,
  +0x34 get_compressed_state, +0x38 apply_compressed_state, +0x54 check_object_is_dirty, +0x58 check_full_update.
  The full table is in the report §1.1.
- `Object_type_registration_list` 0x02BEA34C is `registration*[12]` (48 bytes). It is zeroed in
  `multi_core_objects_init` 0x5B6000 (0x5B629D..0x5B62C5). `Remote_objects_by_type` is at 0x02BE4BAC.
- `multi_game_init_remote_game_objects` 0x5F7CF0 calls exactly five inits. Each one stores its registration in its slot:

  | type | registration | init | slot store | vftable | state |
  |---|---|---|---|---|---|
  | 1 | projectile | 0x5E0870 | 0x5E0C63 (+0x4) | 0xF0AEDC | multi_projectile_state 0x6C |
  | 2 | player (`Player_mr` 0x02BE4AE0) | 0x5F50F0 | 0x5F57E0 (+0x8) | 0xF0AE7C | multi_player_state 0xAC |
  | 3 | item | 0x5DF440 | 0x5DF86C (+0xC) | 0xF0ADBC | multi_item_state 0x50 |
  | 6 | multi_mover | 0x5DF880 | 0x5E015C (+0x18) | 0xF0AE1C | multi_mover_state 0x50 |
  | 7 | rfg_mover | 0x5E0C70 | 0x5E15CE (+0x1C) | 0xF0AF3C | multi_mover_state (bundle 0x54) |

- **In the campaign no slot is filled, not even player.** `multi_game_level_init_pre` 0x5FD0B0 (push 1; call at
  0x5FD0CB) is skipped by `gamestate_gp_level_init_pre` at 0x3EFD75 when `Multiplayer==0`. The function's other
  callers are `multi_game_init` 0x5FCB50 (called only from `multi_init`, at 0x5FD6B6), `multi_client_reset_remote_object_system`
  and the migration path.
- **Orphaned types.** `multi_game_object_vehicle_automobile_init` 0x55E430, `..._walker_init` 0x55F1A0,
  `multi_game_object_turret_init` 0x55DD20 and `..._weapon_pickup_init` 0x55F1C0 are each a 1-byte `ret`.
  Automobile, walker and turret have no vftable symbol, no callers in xrefs, and no absolute pointer anywhere in
  rfg.exe. A raw VA scan for 17 functions found 0 hits; the control pointers were found, so the scan works.
  `multi_core_object_register_object_type` 0x547EE0 (writes any empty slot < 12) also has no callers.
- **The orphaned vehicle code is complete, not stubs.** Examples:
  - `automobile_multi_registration::apply_compressed_state` 0x5FA720 is 0x55D bytes.
  - `get_state` 0x5F60D0 reads the rigid body and `automobile_get_vcontrols`.
  - The state pools are `multi_pool<object_state_data<automobile_data_bundle>,800>` 0x5B0E40, walker (100) 0x5B0FE0
    and turret (1600) 0x5B0D50.
  - `multi_sim_state<multi_automobile_state>::interpolate_state` is 0x5693D0 and `extrapolate_state` is 0x5692E0.
  - Some glue is still called from the simulation. `automobile_do_frame_presim` calls the automobile
    `apply_sim_state` 0x59D9B0 (call site 0x79BE16) and `is_client_authoritative` 0x59D930. `turret::frame_process_pre`
    calls the turret `apply_sim_state` 0x5800C0 (0x6C5AD0).
- **NPC (type 5) has no engine code.** The symbol table contains no npc_multi or human_multi symbols, and no npc/human state
  bundle. The `object_state_data<>` bundles exist only for automobile, item, mover, player, projectile, rfg_mover,
  turret and walker. `npc::serialize` still pushes 5 before `multi_core_create_remote_object` (0x6DD79F/0x6DD7A1).
- Type ids pushed by serialize paths: player 2 (0x6DF1FE), automobile 4 (0x795966), npc 5 (0x6DD79F), turret 0xA
  (0x6AFE2B), walker 0xB (0x7941D5).

### Host-side remote objects and packets [V unless marked]

- `multi_core_create_remote_object(type, game_handle)` 0x5D97E0 returns at once unless `Multiplayer==1 && Server==1`.
  `remote_object` is 0x898 bytes with at most 16 connections. Its handle hashes (`Reserved_network_to_game_hash` etc.)
  are built only in `multi_core_objects_init` 0x5B6000, from `MP_allocators_mempool`. That function is reached only
  through `multi_io_init` (0x5FB1A4) <- `multi_init` (0x5FD58A).
- Update packet 0x12 is parsed by `multi_core_object_process_update_packet` 0x5E3A10. The receiver drops it unless
  `Client_state` 0x2BEA6A8 == 4 and `Migration_state` 0x2BEA6C4 == 0. The full wire layout is in the report §1.3.
- Packet tables (decoded from the exe): `Multi_core_packets` 0x127C848 (46 x 0x1C) and `Multi_game_packets` 0x127D710
  (82 entries). Direction values: 0 = S->C, 1 = C->S, 2 = both. `multi_io_process_packet` enforces the direction on
  receive (`multi_io_verify_packet_direction_receive` 0x572620, called at 0x58F949).
  - 16/17/18 remote_object_create/destroy/update: S->C.
  - 19 dc ack list: C->S.
  - 47 client_controls: C->S.
  - 76/77/78 bullet_hit/tracer/melee_hit: both, drop_on_cheating.
  - 80 collision_hit: C->S, drop_on_cheating.
  - 86 request damage: C->S.
  - 104 client object create: C->S.
  - No mission, zone or world-state packets were found (report §1.9; no skeptic checked this).

### Shipped authority model [V]

- **Each client owns its own player's movement.** `multi_game_cp_send_client_controls` 0x5BD290 packs:
  - the camera orientation (`mat_ph_delta_compress`, 0x5BD60D);
  - the position and velocity: object +0x4 and +0x270 (0x5BD94A/0x5BD95E), or `havok_ragdoll_get_pos/get_vel` when
    ragdolled;
  - the control flags. It packs with `vec_delta_compress` (0x5BD9AC/0x5BD9E9).
  The server's `multi_game_cp_process_client_controls` 0x5DD8D0 calls `add_state` at 0x5DDFA6.
- **Hits are detected by the shooter's client and validated by the server.**
  - Sender: `do_bullet_hit` calls `multi_gw_send_bullet_hit` (0x7E588B) when the shooter is the local player.
  - Receiver: `multi_gw_process_bullet_hit` runs only on the server (check at 0x5D3EFA). It calls
    `validate_attack_hit` (0x5D3F12) and `weapons_process_bullet_hit` (0x5D3F9B), then relays the hit (0x5D3FE5).
  - Melee works the same way.
- **The server owns the five registered types and broadcasts their snapshots.**
- **Driver-owned vehicles are not a shipped model** (see Contested). The co-op mod has to build that model itself.

### Remote player creation [V]

- `player_multi_registration::process_create_packet` 0x5E02E0 creates a full player object, not a light proxy:
  1. builds `obj_human_props` and sets the props `MULTI_PLAYER_HANDLE` (0x5E0679) and `MULTI_TEAM` (0x5E069C);
  2. creates the object with `world::create_object_internal(World, player::get_classname_hash(), props, 0, -1)`
     0x53E550 (0x5E06C8/0x5E06D3);
  3. calls `human_set_state`, `player_set_zoom_state` and visibility, and puts the player in a vehicle if needed.
  It also reads `Mp_game_data`, `Multi_gameplay_mode` and `Multi_match_info`, so do not call it directly. Copy the
  recipe instead.
- **Several player objects can exist with `Multiplayer==0`.** `wrecking_crew_spawn_players` 0x39F130 creates one per
  slot:
  - `obj_human_props::obj_human_props` 0x6529A0 (called at 0x39F1AB);
  - `player::get_classname_hash` 0x610140 (0x39F1B9);
  - `world::create_object_internal` (0x39F1C4);
  - `human_set_state` 0x607520 (0x39F1E3).
  It is called from `player_level_init_post`, on a path that does not depend on the flag. It immediately hides every
  player with `wrecking_crew_hide_player` 0x39ED50 (holster, delete inventory, invisible, `+0xB4 |= 2`, collision
  filter off). Wrecking Crew is hot-seat: one live player at a time.
- `playerpool` 0x02C6E4A0 is `object_pool<player,16,1>`: 16 x 0x2E50 plus a 0x90 header = 189840 bytes. That makes
  **16 players the maximum**.
- `Player` 0x02C22874 is written at exactly five sites, and nothing takes its address:
  - `player_level_init_post`: 0x6D593A and 0x6D5A83;
  - `player_level_shutdown_all`: 0x69DD66 and 0x69DD72;
  - `player_set_next_active_wrecking_crew_player`: 0x6108A2.
  Creating more players does not change it.
- `player_process_all` 0x6E7330 iterates `World.subtype_objects[3]` (World+0x3F4 elements, +0x3FC count). It calls
  `player_do_frame` only when `[[obj+0x32C]+0x34] != 0`, i.e. the char_inst stream is loaded (check at
  0x6E7360..0x6E736A).
- `player_do_frame` 0x6E6290:
  - Returns early for any player that is not `Player` while Wrecking Crew is active (0x6E6294..0x6E62AF).
  - Otherwise has non-local branches (0x6E6333, 0x6E6393, 0x6E6975).
  - Calls `multi_game_player_process_state` only when `Multiplayer==1 && multi_is_server()` and the object is not
    `Player` (0x6E698F..0x6E69A2). A second player in SP is therefore an undriven puppet.
- **`player::do_frame` 0x6D5660 has an SP-path hazard.** It treats whatever player it processes as the local one:
  - writes `Local_player_in_protected_vehicle_postion` (0x6D5807);
  - compares and writes `Player_district` (0x6D5815) and calls `district_local_player`;
  - calls `hud_message_add_minor` and `play_district_liberated_radio_line` (0x6D570E);
  - runs the backpack fade and `multi_game_backpack_create`/`s_lastBackpackIndex` (0x6D56B2..0x6D56D7).
  Puppets need these gated per player.
- Local-player use sites: 301 `Player` references plus 849 call/jmp to `player_get_local_player` 0x610540, about 1150 in
  all. 70 functions iterate the player list (World+0x3F4/+0x3FC).

### The player puppet pipeline [V code, I runtime]

- Every player, in SP too, has `multi_sim_state<multi_player_state>` at **player+0x1274** (0x10D4 bytes):
  `int m_states_used` (+0), `multi_player_state m_states[25]` (+4, 0xAC each), `m_apply_correction_timestamp` (+0x10D0).
  Other MP fields that every player has: +0x1228 fired_this_frame, +0x122C/+0x1238 mp_camera_pos/orient,
  +0x1264 desired weapon slot, +0x1268 `mp_last_respawn_time`, +0x126C mp_data.
- `multi_player_state` (0xAC) is laid out as follows:
  - `multi_object_state`: pos, lin vel +0xC, ang vel, orient, `network_time` +0x48;
  - +0x4C camera matrix, +0x70 cam ang vel, +0x78 forward, +0x7C sideways, +0x80 hp pct;
  - +0x84 ladder handle, +0x88 ragdoll_state, +0x90 `turret_handle` and +0x98 `vehicle_handle`. Both handles are
    `network_to_game_handle` {u16 net, u32 game}, so the cached game handles sit at +0x94 and +0x9C;
  - +0xA0 seat, +0xA4 bitfield (weapon slot, sprint, 2-bit edge ids for jump, melee, trigger down/up, throw, crouch...);
  - +0xA8/+0xAA magazine/reserve ammo.
  The report has the full list. The edge-id semantics are [I].
- `multi_sim_state<multi_player_state>::reset` 0x568B20 sets `[this]=0` and `[this+0x10D0]=-1`. In SP nobody calls it:
  `player::player` 0x6DE880 never touches +0x1274, and `player::serialize` calls reset only under `Multiplayer`
  (0x6DEB5C).
- `add_state` 0x568B40 (thiscall, ecx = player+0x1274) drops a new state in two cases:
  - its `network_time` is strictly older than the newest stored state (unsigned `ja`, so equal times pass);
  - its `network_time` is non-zero and more than 5000 ms (0x1388) away from `Clock_current_time`. A time of 0 skips
    this window check.
  An invalid orientation is replaced with identity. When the buffer is full (25), the oldest entry is shifted out.
  In shipped code, only `player_multi_registration::apply_compressed_state` and `multi_game_cp_process_client_controls`
  call it.
- `obtain_state` 0x568C20 copies a whole state (`rep movsd` 0x2B dwords, so the cached handle fields come with it).
  It picks the newest state whose time is <= t:
  - between two states: `interpolate_state` 0x563B50;
  - past the newest state: holds it, and flags it extrapolated after `Max_extrap_time_ms` 0x127E4C4 (default 1000).
  The position extrapolation itself happens in `correct_player_movement`.
- `multi_game_player_process_state(player*)` 0x5E1C50 is cdecl. Its callers are `player_do_frame` (0x6E69A2, MP-gated)
  and `player_multi_registration::move` (0x5F49B7). It does nothing when:
  - `[obj+0x304]` movement_mode == 2;
  - `mp_last_respawn_time + 500 >= state time`;
  - `[obj+0x2F4]` cur_state == 4;
  - the object is the local player (`object_is_local_player`).
  It runs these steps:
  1. `obtain_state(Clock_current_time)` (0x5E1CCB).
  2. `apply_movement_state` 0x5CF130. **Real ABI: the state goes in EAX and the player is pushed.** It writes the
     camera, jump, ladder, crouch and ragdoll, then calls `do_frame_movement` 0x5CEEA0 ->
     `multi_game_player_correct_player_movement` 0x5CE2C0. The correction tunables are `No_sync_position_threshold`,
     `No_sync_velocity_threshold`, `Max_velocity_correction_rate` and `Mp_player_edge_correct`.
  3. `apply_weapon_state` 0x582250 (`player*, state*, bool, bool`): equip, reload, melee, trigger pull/release, throw.
  4. `multi_game_player_apply_vehicle_state` 0x5E1B60 (0x5E1DF7).
  5. Turret man/abandon through `network_to_game_handle::get_game_handle` 0x5D2A80 (0x5E1E0E).
- **The movement and weapon path** reads only `Clock_current_time`, `World`, `Rfg_camera`, `Frametime`, the tunables,
  `Player`, and the `Multiplayer`/`Server` flags.
- **The vehicle and turret paths reach the MP hash.**
  - A vehicle needs a lookup when its cached handle (state+0x9C) is -1 (check at 0x5E1BB7). A turret needs one when
    its cached handle (+4 of its `network_to_game_handle`, 0x5D2A95) is -1. In both cases the code calls
    `multi_core_game_handle_from_network_handle` 0x5B6380.
  - That function reads `Reserved_network_to_game_hash` +0x44 (size) and +0x40 (buckets) and calls `uint_hash`
    0x18FD00, which does `div` by the size. In SP the size is 0, so this is a **divide-by-zero**. The crash was read
    from the code, not seen in game.
  - Avoid it by pre-filling the cached game handles, or by sending 0xFFFF for the turret handle.
  - Net vehicle handle 0xFFFF while the player is in a vehicle (+0xAC4 != -1) calls `vehicle_interaction_request_exit`
    (0x5E1B6D..0x5E1B95).
- **Branches that do nothing in SP:**
  - Magazine and reserve ammo are applied only when `Multiplayer && Server==1` (`apply_weapon_state` 0x582298).
  - The jetpack fuel fudge runs only on an MP client, `Multiplayer && !Server` (0x5E1D56..0x5E1DEA).

### MP clock in SP [V]

- `multi_process` 0x604C20 runs every frame (called from `rfg_do_frame` at 0x1D24D9). It returns at 0x604D39
  (`je 0x604F62`) when `!Multiplayer`. That skips `multi_core_clock_process` (its only caller, at 0x604DE9), incoming
  packets, latencies, sessions and sends.
- Before that gate it still runs presence, `multi_core_platform_internal_system_process` and invite handling, so the MP
  stack is not fully idle.
- `Clock_current_time` 0x02BEA538 has six writers: clock_system_init 0x543DC2, clock_process 0x543DF5, clock_set_time
  0x543E24, process_clock_packet 0x571D18, join_accept 0x5FC2F2 and multi_init 0x5FD578. **Nothing advances it in SP.**
- `multi_core_clock_process` 0x543DE0 does this:
  `t = timer_get(1000); Clock_current_time += t - Prev_frametime; Prev_frametime = t`, where `Prev_frametime` is
  0x02BE5D00. If the mod uses it, set `Prev_frametime` to the current time first. Otherwise the first call adds one large
  jump. Writing `Clock_current_time` directly works as well.

### Car puppet [V]

- `automobile+0x4E88`, `walker+0x7068` and `turret+0xC4` are `multi_sim_state<...>*`. `automobile::serialize` writes
  NULL to the car's pointer, so it is NULL in SP. The pointer is accessed in 12 places: the orphaned registration methods,
  `get_steering_correction`, `is_client_authoritative` (0x59D94A), `automobile::serialize` and
  `automobile_do_frame_presim`.
- Container sizes: automobile 0x1E1C, walker 0x1EE4, turret 0x9CC. For automobile, walker and turret respectively,
  `add_state` is at 0x5691F0 / 0x5696B0 / 0x569B70 and `obtain_state` is thiscall at 0x5895E0 / 0x589740 / 0x5898A0.
  `obtain_state` returns 0 when `m_states_used == 0`.
- `multi_automobile_state` (0x134) contains:
  - `multi_object_state`;
  - +0x4C `vehicle_controls`: bit0 brake_reverse, bit1 handbrake, bit2 honk, +4 accelerate, +8 steering;
  - +0x58 hp, +0x60 component_count;
  - bool[66] arrays at +0x64/+0xA6/+0xE8;
  - +0x12A flat_tire, +0x12B lights_bitmask, +0x132 on_fire/smoking.
- `multi_game_object_vehicle_automobile_apply_sim_state(multi_automobile_state*, automobile*)` 0x59D9B0 is __cdecl:
  - It skips physics and controls when the local player is in seat 0 (0x59DA22..0x59DA36).
  - It **hard-snaps** the position and velocity when the error is large, or when `multi_game_player_can_view_object`
    is false and `havok_collect_objects_fast` finds the space clear (0x59DB3A..0x59DD1F). Otherwise it uses
    `hkpKeyFrameUtility::applySoftKeyFrame` (0x59E225).
  - It replays steering (`get_steering_correction` + steering, then `automobile_steer`, 0x59E264), the throttle
    (`clear_accelerator`, then `set_forward_or_reverse_accelerator`) and the brake pedal (needs bit0).
  - It only *releases* the handbrake, with `automobile_set_handbrake(car,false)` when the speed is below a constant
    (0x59E322).
  - Its hp, smoking/fire, component unlatch/detach/shatter and horn part (which also needs `+0x350C==1`) is behind
    `Multiplayer==1 && Server==0` (0x59E32D). In SP all of that is skipped.
  - It never reads lights_bitmask or flat_tire.
- The engine's own caller, `automobile_do_frame_presim` (0x79BDD2..0x79BE16), needs `Multiplayer==1`, server and
  `is_client_authoritative`. `is_client_authoritative` checks for `+0x7E==3`, `+0x7F==4`, a non-NULL +0x4E88 and that
  the seat-0 passenger is not the local player. **In SP the mod must call `obtain_state` + `apply_sim_state` itself.**

### Standalone compression helpers [V, report only]

These are pure functions that work without MP init:
- `compress_float` 0x5448E0 / `decompress_float` 0x544B40;
- `vec_delta_compress` 0x545670 / `vec_delta_decompress` 0x5457E0;
- `mat_ph_delta_compress` 0x545B30 / `mat_ph_delta_decompress` 0x545DA0;
- `bit_packer` (ctor 0x544040, `set_next_bits` 0x5440F0, `get_next_bits` 0x544440).
The registration-member helpers (`buffer_delta_compressed_*`) need a registration with masks, so they are unusable in SP.

### Level object identity [V report; no skeptic checked it (C8)]

- `multi_game_setup_reserved_remote_objects` 0x5DBAF0 calls `add_reserved_handles<T>` for rfg_mover, general_mover,
  marker, obj_zone, ladder and flag. For example, `add_reserved_handles<rfg_mover>` 0x5D7110 collects the handles
  (capped at 1000, `cmp edi,0x3E8`), sorts them with `obj_handle_compare` and reserves network handles in that order.
- So MP assumes that **static level-object game handles are identical on every machine that loads the same map**.
  Whether that holds for campaign territory zones is unknown (P6).

## Approach for co-op

1. Never set `Multiplayer`, never call `multi_init`, and never use the registration or remote-object framework. Use our
   own transport (see transport.md).
2. **Authority:** copy the shipped split. Each peer owns its own avatar movement and detects its own hits; the host
   validates and applies them. Unlike shipped MP, the driver also owns the car (the mod has to build this). The host owns
   NPCs, unoccupied vehicles, missions and destruction.
3. **Spawn each remote player once** with the Wrecking Crew recipe: `obj_human_props` 0x6529A0 +
   `player::get_classname_hash` 0x610140 + `world::create_object_internal` 0x53E550 + `human_set_state` 0x607520. Do not
   call `wrecking_crew_hide_player`, and leave `Player` alone. Then call `multi_sim_state::reset` 0x568B20 on
   `puppet+0x1274`.
4. **Sender:** build a `multi_player_state` from the same fields `send_client_controls` uses: pos +0x4 (or ragdoll),
   vel +0x270, orient, camera, the bitfield edge ids, ladder, crouch, and the vehicle handle + seat. Ship it at 15-30 Hz.
   A plain struct is fine to start. The standalone compressors are there if bandwidth needs them.
5. **Receiver, each sim frame:**
   - advance `Clock_current_time` (seed `Prev_frametime` first, or write the clock directly);
   - map the sender's timestamp into the local clock (within ±5000 ms and monotonic);
   - **fill the cached game handles** in `vehicle_handle`/`turret_handle` (net 0xFFFF only for "no turret");
   - call `add_state`, then `multi_game_player_process_state(puppet)`.
   Either call it from our sim hook, or patch the gate at 0x6E698F to admit puppets only.
6. **Patch or gate the single-player-only side effects** of `player::do_frame` for puppets (district, HUD, radio,
   backpack, protected-vehicle global). Apply ammo ourselves, because that branch is MP-gated.
7. **Remote-driven car:**
   - allocate a zeroed 0x1E1C `multi_sim_state<multi_automobile_state>` and store it at `car+0x4E88`;
   - each frame: `add_state` 0x5691F0, then `obtain_state` 0x5895E0 + `apply_sim_state` 0x59D9B0;
   - apply handbrake, horn, lights, hp, fire and components ourselves;
   - clear the pointer before the car is freed or streamed out.
8. **NPCs and world state:** write our own replication (other domains). The engine offers nothing here.
9. Skip on purpose: delta compression with acks, priority/bandwidth scheduling and generic item/mover replication. Add
   them only if measurements demand it.

## Contested / refuted (do not rely on)

- **"The puppet pipeline reads no MP session/pool globals" (report §1.5, TL;DR): REFUTED.** The vehicle and turret
  handle paths call `multi_core_game_handle_from_network_handle`, and that function divides by zero in SP. The report's
  grep covered only the direct globals of each function.
- **"Server-only branches" in process_state: WRONG.** The `Multiplayer && !Server` block is a client-only fuel fudge.
  Ammo is server-only.
- **"Only `Clock_current_time` advancing is needed" (report TL;DR, §3): INCOMPLETE.** The mod must also call it each frame,
  `reset` the buffer, feed it, pre-fill handles and gate the `player::do_frame` side effects.
- **"apply_sim_state replays handbrake and horn" (report §1.6): REFUTED.** In SP the handbrake is only ever released,
  the horn is skipped by the MP gate, and lights and flat tyres are never applied.
- **"Vehicles are owned by their driver" as the shipped model (report §1.4, C5): REFUTED.** Automobile, walker and turret
  have no registration, so no driver state ever reaches the server. The presim gate never fires, because `obtain_state`
  returns 0 on an empty buffer. Shipped MP vehicles most likely travel only through the driver's control flags in
  client_controls [I].
- **"Wrecking Crew proves several players are simulated together": REFUTED.** Wrecking Crew proves several player
  objects can coexist. It hides all but one and skips non-current players in `player_do_frame`.
- **"Multiplayer=1 blocks NPC spawning": TOO BROAD.** Only ambient spawning stops (`amb_spawn_do_frame` 0x75F410). Squad
  spawn nodes (`object_squad_spawn_node::evaluate_spawn` 0x66CFE9) and `ai_system::do_frame` 0x2C35B3 still run
  on a server. AI is off anyway, because `gamestate_gp_simulate_frame` skips `ai_system_do_frame` at 0x3EDEF3.
  The clock is only partly stopped: `game_time_of_day_do_frame` and `game_hudtimer_update` still run (0x3EDEDC/0x3EDEE4).
- **"~47 MiB" for multi_init:** 0x2F00200 + 0x80000 is about 47.5 MiB, and it is a lower bound because
  `multi_game_memory_alloc` (0x5FD41D) allocates more. Not viable whatever the exact figure.
- **"MP stack fully idle in SP": WRONG.** The presence, platform and invite code before the gate still runs.
- **Minor numbers:** the pool is 16 x 0x2E50 **+ 0x90** (not exactly 16 x 0x2E50). 70 functions iterate the player list,
  not 69. `player_do_frame` runs only for players whose char_inst stream is loaded, not for every player.
- Global `Multiplayer=1` is ruled out, but patching individual flag checks was not evaluated. The streaming, mission and
  destruction reads of the flag were only sampled, not traced per branch [I].

## Unknowns and the probes that settle them

| # | Unknown | Probe (test build, campaign save) |
|---|---|---|
| P1 | Does a second player object live safely in the campaign (spawn, idle animation, `player_process_all` runs it, save/load, zone change, local death, cutscenes)? Which `player::do_frame` side effects fire for it? | Spawn one puppet with the Wrecking Crew recipe next to the player and leave `Player` untouched. Log `World+0x3FC`, `Player_district` and HUD messages. |
| P2 | Does `multi_game_player_process_state` work with `Multiplayer==0` (no hidden MP reads, does not freeze)? Is `obtain_state` smoothing enough at real latency? | Echo puppet: reset the buffer, advance the clock, feed the local player's state delayed by N ms, then call process_state. Then add 10% loss and 200 ms jitter. |
| P3 | Do the weapon edge ids fire, reload, melee and throw? Do the puppet's bullets damage NPCs? Ammo must be set by us. | Flip the edge ids in the echoed state. |
| P4 | Car puppet: does steering replay work, does soft keyframe vs hard snap behave, does enter/exit work with pre-filled handles (no div-by-zero)? | Puppet in a parked car (cached game handle set). A 0x1E1C sim state at +0x4E88 is fed from the car the local player drives. Exit with 0xFFFF. |
| P5 | Side effects of advancing `Clock_current_time` in SP (135 references, believed to be all MP code [I]). | Log the clock and run a mission plus a save/load with clock advancing on. |
| P6 | Are campaign level-object handles deterministic across machines and runs? | Dump `(handle, srid, type, pos)` for one zone on two PCs or runs from the same save, then diff. |
| P7 | Is `Level_memmgr` headroom in the campaign enough for `multi_init`? This is academic now that the route is rejected. | Log the free space. |
| Q | Meaning of `update_rate = 100`. How to team the puppet (`MULTI_TEAM` is MP-only; human team +0xC98). Which `object_is_local_player` callers (AI belief 0x23D7A1, vehicle despawn, fire effects) matter for puppets. | Read the code when those domains need it. |
