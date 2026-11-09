# RfgCoop research report: online co-op through the RFGR campaign

Date: 2026-10-07. Build: GOG `rfg.exe`, 32-bit, PE stamp 0x5B9B713D (sha256 7a82d2d0…), the same file in both installs.
All addresses are **RVAs** (runtime address = module base + RVA, because of ASLR). `rfg.asm` and the disassembler
print VA = RVA + 0x400000.

**Nothing in this report has run in game.** It is static research: disassembly, PDB, data files, community code, logs.

Tags:
- **[confirmed]**: read in code or data by a domain agent and upheld by the skeptic pass, or re-read by a judge or in
  this pass ("checked this pass").
- **[partially]**: the core holds but the original claim was narrowed or corrected. The correction is given.
- **[contested]**: the domains disagree, or the claim was refuted. See §9.
- **[inferred]**: design reasoning, not read in code.

---

## 1. Verdict

**Two-player co-op through the Tharsis campaign is feasible on this engine, but only as a custom sync layer over two
ordinary single-player games.** It is a very large mod: about 4,700 new lines in about 12 test builds for v1 [inferred
from the per-milestone estimates in §5; the gap round added about 700], with two make-or-break experiments at the start.

How it works:
- Both machines run the normal SP campaign. The `Multiplayer` flag (0x02BEA588) stays 0, always. Setting it switches off
  missions, activities, the AI frame drivers, ambient spawning, salvage and the game clock [confirmed].
- **The host's game is the world.** The host plays a vanilla campaign plus observing hooks, so guest bugs cannot corrupt
  the host's campaign or save.
- **The guest's game is a mirror.** It loads the host's world from memory, switches off its own spawning, saving,
  missions and scripts with one-site fences, and shows copies of the host's NPCs, vehicles and Geo-Mod events. The host's
  campaign consequences (district control, liberation, unlocks) never run on the guest.
- **Each player owns their own avatar.** The partner appears as a second real `player` object, driven by the engine's own
  MP client function `multi_game_player_process_state` (0x5E1C50), which still exists in every SP player object.
- **Transport is ENet over UDP** to a direct IP or a virtual LAN (Tailscale, ZeroTier, Radmin). The engine's own MP
  network stack cannot run with `Multiplayer` at 0 [confirmed].

A realistic v1:
- 2 players, GOG build only;
- the guest joins the host's world in free roam and re-joins automatically after every reload;
- see each other, shared Geo-Mod, shared enemies, shared vehicles (drive, ride, gun);
- downed/revive instead of death;
- the host leads missions and does the objective steps, the guest fights alongside;
- the guest uses the host's loadout and earns nothing; players stay within about 200 m of each other.

What makes it hard:
- The engine assumes one local player almost everywhere: streaming, spawning, AI, death handling, 42 mission-script
  callbacks [confirmed].
- Nothing is deterministic: two random number generators seeded from the microsecond timer, one of them also drawn
  from by the render thread; Havok steps once per frame with each machine's frame time; the SP debris load balancer
  reads each machine's camera and timing [confirmed]. So lockstep is out.
- Saints Row 2 (same engine family) had about 37% of its QA bugs from co-op, with source code and a studio [confirmed
  quote, GameSpot GDC 2010].
- No one has published or even publicly attempted campaign co-op for RFG or RFGR [partially: bounded negative; private
  experiments cannot be ruled out].

The project continues or stops on two cheap experiments: **E2** (does a second `player` object work in the campaign?) and
**E3** (can the guest load the host's world from memory without touching its own save?). E4 (destruction replay) and E5
(mirrored NPCs) then decide how much of v1 is possible. All four are single-instance builds and come before any real
network code. E2's first build must carry the stats-skip patch at 0x6E6B44, or the ghost crashes on its first frame
(gap round).

---

## Gap round (second critic round, 2026-10-07)

A completeness critic asked five questions the first round left open. Five investigators answered them statically and
skeptics checked every claim. The verified facts are in `notes/gaps.md`; the raw reports are
`findings/workflows/coop_gap_1.md` … `coop_gap_5.md`. What changed in this report:

1. **The guest mission fence moves to M5 and grows.** A host *checkpoint* blob carries the active mission and its running
   managed scripts; a guest that loads it starts the mission itself (`world::setup_territory` 0x540AB0) and runs the
   scripts. The fence is: NOP the restore-index store 0x39D69C, skip `script_do_frame` at 0x3EE1FB, `ret` at both
   `mission_activate` entries as a backstop, plus the old `player_start_mission` refusal. An activity-replay blob
   likewise force-starts its activity (`Activity_node_start_on_load` 0x1250244). (§3.3, §3.9, §4.3, §4.8)
2. **Retry from start never re-activates a mission**; only a checkpoint "continue" does. DFA has no checkpoint, so M9
   adds a checkpoint mission to its test. (§3.9, §5)
3. **Mission and activity starts regrow buildings** outside the Geo-Mod queue (`restore_list_do_restore` 0x375180). The
   guest needs a RESTORE event. (§3.6, §3.9, §4.5)
4. **The guest's mission content is "mirror everything, load nothing".** Mission layers hold logic only; objective
   buildings are base-zone objects; icons and ground markers sit on layer logic objects and go by position. The guest
   reserves the host's actual model set; it must never call `srm_mission_reservation`. (§3.9, §4.4)
5. **Population fence:** `gameplay_do_frame` (single call 0x3EDF23) holds almost every spawner, raids included
   (`activity_suspend_activities` does not stop raids). The guest replaces that call with a two-function whitelist;
   `spawn_npc` / `sp_spawn_vehicle` refusals become backstops. Ambient peds and traffic were traced into those
   refusals. Turrets and roadblock barricades bypass them and need mirroring. (§3.5)
6. **Replayed destruction runs campaign consequences on the guest** (district control, liberation, the Dust safehouse
   warp). `district::set_control` is not a live setter. The guest repoints the engine's own `cmp [Multiplayer]` early-outs
   to a guest byte and writes district control raw. (§2, §3.6, §3.7, §4.5)
7. **Geo-Mod determinism is redesigned.** RNG control is cheap but secondary. The per-machine inputs are the SP debris
   load balancer, immediate slices and uber explosions, the unsorted shardify list and the out-of-queue deleter. Copy
   stock MP's branches on both machines (4-5 operand repoints), take the host's balancer values, pair chunks by
   destruction uid, and add a chunk pose lane. K3 splits into K3a/b/c and E4 is redesigned. (§3.6, §5, §6, §7)
8. **The puppet has a known first-frame crash** (NULL `Mp_game_data` in MP stats, fix 0x6E6B44 `8B CE → EB 5E`), a local
   stick leak in mid-air physics, and camera sites that treat "a player" as "the local player". All have one-site fixes.
   The MP hash tables need a real zeroed bucket array. (§3.1, §4.3)
9. **The guest's load and save entries are classified.** The 0x51C320 detour is not side-effect free (Esc → Load runs
   `platform_load` first; a refused warping video drops to the main menu), so the UI paths are blocked before their own
   side effects. (§3.3, §4.8)

---

## In-game results so far (2026-10-07)

- **E1 passed** (ver4): two instances on one PC, each simulating at ~59 fps while unfocused, separate saves (`kee2`).
- **E2 passed** (ver3): a second simulated `player` works in the campaign (ghost). The first build crashed in the minimap
  (`cartographer::collect_humans` casts allied humans to npc); fixed, and an audit of every "not Player = npc" site found
  no other crash.
- **M2 + M4 passed** (ver5): ENet link with mutual password proofs, RTT 1 ms on loopback, 0 loss, partner puppets
  tracking within 0.1-0.4 m, hidden while driving / loading, F7, F8 stop and reconnect, wrong password refused.
- **E3 passed** (ver5): a silent world blob (1.31 MB) captured in free roam and loaded back from memory restores the world
  exactly as captured; the save guard blocked the death autosave. Correction: any load spawns the player at the nearest
  spawn safehouse (`save_state_finish` -> `safehouse_find_spawn_safehouse`), not at the captured spot; join/re-join
  teleports the guest beside the host anyway (§4.8).
- The two make-or-break experiments (E2, E3) have passed.

## 2. What the engine already gives us

| Piece | Address (RVA) | What it gives co-op | Tag |
|---|---|---|---|
| `Multiplayer` flag | 0x02BEA588 | must stay 0: 1,663 references in 1,058 functions; `activity_system_do_frame` and `mission_warp_and_start` return at once when it is 1; it skips AI, pathfinding, convoys and the clock in `gamestate_gp_simulate_frame` (0x3EDEF3, 0x3EDE7E) | [confirmed] |
| Several `player` objects without MP | pool `object_pool<player,16,1>`; recipe `obj_human_props` 0x6529A0 + `world::create_object_internal` 0x53E550 with `player::get_classname_hash` 0x610140 + `human_set_state` 0x607520 | up to 16 players; creating one never writes the `Player` global (0x02C22874), which only 3 functions write | [confirmed]. A second *simulated* player in the campaign has never run: Wrecking Crew hides and skips its extra players [partially] |
| Puppet driver | `player+0x1274` `multi_sim_state<multi_player_state>` (25 × 0xAC states); `reset` 0x568B20, `add_state` 0x568B40; `multi_game_player_process_state(player*)` 0x5E1C50, cdecl | interpolation, extrapolation, correction, locomotion, aim, jump, crouch, ladders, ragdoll, fire, reload, melee, throws, vehicle enter/exit, turrets | [partially]: five SP traps and some local-player leaks, see §3.1 |
| Puppet aim | `apply_movement_state` (inside process_state) writes `mp_camera_orient` +0x1238, `mp_camera_pos` +0x122C and `aim_pos` +0xD40 (stores at 0x5CF393 / 0x5CF3AD) | the puppet's real weapons point where the remote player aims: hitscan along `mp_camera`, other weapons at `aim_pos` | [confirmed by two judges] |
| MP clock | `Clock_current_time` 0x02BEA538 | frozen in SP (only `multi_core_clock_process` advances it, and `multi_process` returns before it); the mod writes it each frame | [confirmed] |
| never_die clamp | `human_damage` 0x6CE950; at 0x6CEF85 `test byte [player+0xDCC],0x10` then HP (+0x914) = 1.0 | any player-class victim with bit 0x10 never reaches 0 HP, so `human_died`, `player_is_killed` and the SP death branch never run | [confirmed, checked this pass] |
| Death timer arm | `player_is_killed` 0x634360, **one** call site 0x6E59E7, after `+0x7E==0` / `+0x7F==3` | single choke point to stop the 4 s death timer (+0x1118) for puppets | [confirmed, checked this pass] |
| Save choke point | `call keen::SaveDataHandler::requestSave` at 0x811278, its only caller, inside `unit4::LocalGameSession::startSave` 0x811240 | every save-image disk write (saves, autosaves, import, delete, options, profile) passes here; `config.ini` (options cache) is written by `game_save_ini_thread` after three of its callers bump `Save_system_current_ini_write`, which the E3 guard cancels too (notes/world_impl.md) | [confirmed] |
| World from memory | `world::get_load_buf` 0x51C970, `set_cur_size` 0x51C990, `platform_load` 0x51C9B0, `set_pending_territory_load` 0x51C320 | the guest loads the host's "RFGS" world blob (v52, ≤ 2.5 MB; user's mid-game slot 1.3 MB) without disk access; only the header is checked, no MD5 | [confirmed] |
| Silent blob | `World+0x265C \|= 4`, `World+0x265D &= ~3`; result in `stored_world_state` (World+0x257C, stride 0x6C, buf +0x60, size +0x64), read with `world::get_state_data` 0x51C460 | the host produces a fresh world blob without writing to disk | [partially]: only reliable in free roam, see §9 |
| Reload choke point | `world::set_pending_territory_load` 0x51C320 | every SP death-continue, mission fail/retry/cancel, checkpoint restore, fast travel, activity replay and warping video ends here | [confirmed] |
| Destruction queue | `RFG_dyn_collisions` 0x1CC12F8 (0x110-byte entries, max 0x400), count `Num_rfg_dyn_collisions` 0x1AF7268, lock `Collision_event_wrapper` 0x1B16350; applied by `rfg_dyn_do_collide_post` 0x340710, whose only SP caller is `postCollideCallback` 0x4102E0 (`call; ret 8`) | one hook captures (host) or replaces (guest) every live Geo-Mod event; the first 0xEC bytes of an entry hold no pointers | [confirmed] (saved shape state is restored outside the queue, on stream-in; so are restore-list regrows, §3.6). Stress breaks are queued too, before the Havok step, and no worker thread is involved [confirmed, gap round] |
| Stable handles for zone objects | `world::alloc_object_stub_preexisting` 0x53B3C0 | buildings and other file/save objects get the same 32-bit handle on both machines | [partially]: only with the same files and save, while the zone is loaded on both; chunks and spawns get local handles |
| AI off, body on | `ai_system+0x451` bit 0x02 (`yield_next_frame`), re-set every frame before `npc_process_all` 0x6E6140 | mirrored NPCs animate and move without thinking | [partially]: caveats in §3.5 |
| NPC spawning | `object_spawn_do_spawn` 0x75D220 | the guest creates NPC copies | [confirmed] |
| Vehicle input | `automobile_set_vcontrols` 0x795BD0; vehicle AI is skipped while a player-subtype human sits in seat 0 | a puppet driver plus injected inputs drives a car copy | [partially]: the puppet must really occupy seat 0 and have `+0xAC4` set |
| Vehicle MP replay | `multi_game_object_vehicle_automobile_apply_sim_state` 0x59D9B0, container pointer at `car+0x4E88` | soft-keyframe correction plus steering and throttle replay | [partially]: registration is orphaned, but the apply code is reachable (lane L2, §4.3) |
| Seats | `vehicle_interaction_request_enter` 0x7B7E10 with `vi_enter_data{seat,0,-1,teleport}` | MP's seat recipe works in SP | [confirmed]; a player-type entrant ejects **every** occupant, friends too (`mark_seat_reserved` 0x7A19B0) |
| Runtime world setters | `game_clock_set_time` 0x3C16F0, `human_teams_set_disposition` 0x60D070, `fog_of_war_update` 0x43F850, `gsm_set_alert_level` 0x3AA6B0; districts by a raw write of `district+0x9C` (clamped to `+0xA4`) | time, teams, alert, district control can be synced live, without a reload | [confirmed] for teams, time and fog of war. **`district::set_control` 0x3ABD90 is not a live setter** [confirmed, gap round]: unless `Loading_data` is set it runs `liberate_district`, the capstone unlock and the mission notify. Alert setter side effects still to be checked in M7a |
| Population umbrella | `gameplay_do_frame` 0x3B9770, single call at 0x3EDF23 | one call site holds spawn nodes, raids, activity start/spawn, reinforcements, drop-offs, roadblocks, morale, ambient peds, traffic and ambient EDF; the guest replaces it with a whitelist | [confirmed, gap round] |
| MP early-outs in consequence code | 7-byte `cmp byte [Multiplayer],imm8` at e.g. 0x3B0024 (`gfm_change_edf_control`), 0x68A923 (`general_mover::apply_damage`), 0x359DF0, 0x3AB64A, 0x3D4F33 | repoint the operand (site+2) to a guest byte: the guest behaves like an MP client in exactly those functions, without setting `Multiplayer` | [confirmed, gap round] |
| Restore lists | `restore_list_do_restore(key)` 0x375180 (cdecl), `All_restore_lists` 0x1257518, `world::restore_object` 0x534B70 | mission and activity starts regrow destroyed buildings; the guest repeats it from one key | [confirmed, gap round] |
| Model reservation | `srm_reserve_slot(type,user,info*)` 0x75D410 while `Mission_mode` 0x334146E == 0 | the guest pre-streams the host's exact mission models (SPAWN_SLOT_MISSION, max 24) | [partially, gap round]: can still fail on owner, cap or a full record pool |
| Script callbacks are data | e.g. `player_in_radius` 0x12B4648 | later, swapping one pointer lets either player trigger a mission step | [confirmed] |
| Pause | `game_pause` 0x3C1620 / `game_unpause` 0x3C1650, `Game_paused_count` 0x2132880; the only alt-tab pause push is at 0x4DDB6D, patch byte 0x4DDB5F `75→EB` | host pause can be mirrored; alt-tab no longer freezes a peer | [partially]: other pause sources exist (Esc, map, videos) |
| Hook sites free of other mods | 0x1D24D9 (`call multi_process`, every sim frame except GS 4/5), 0x3EE55C (`call gamestate_gp_simulate_frame`, skipped while `s_pauseGame`), 0x86FFF0 (`endFrame`) | network tick, apply/capture points, overlay | [partially]: exact firing conditions in §4.3 |
| UI | `hud_message_add` 0x455D30, `hud_icon_add` 0x476E60, `cartographer::add_icon` 0x479AF0, `add_objective` 0x464FF0 | HUD lines, name tag, map blip, mirrored objectives | [confirmed RVAs] |
| No single-instance lock | no CreateMutex/FindWindow imports; named kernel objects include the process id | two instances on one PC for testing | [confirmed static]; two *live* instances not yet observed [partially] |
| Toolchain fit | llvm-mingw ships i686 `libws2_32.a` / `libwinmm.a`; rfg.exe already imports WS2_32 and WINMM | ENet adds no new system DLL | [confirmed] |

---

## 3. The hard problems, ranked, with the chosen solution

### 3.1 Showing the partner (biggest risk)
- **Problem.** A second `player` object that is *simulated* in the campaign has never existed. `player_process_all`
  runs `player_do_frame` for every player whose character is loaded. Its non-local path is SP code nobody exercised:
  district HUD and radio, backpack index, radiation, stats [confirmed code, runtime unknown].
- **Solution: puppet player** driven by `add_state` + `multi_game_player_process_state`. Known SP traps and their fixes:
  1. Vehicle and turret handles are resolved through `multi_core_game_handle_from_network_handle` 0x5B6380. Its hash
     table size is 0 in SP, so `uint_hash` divides by zero at 0x18FD00. Fix: write the local game handle into the
     state's cache dwords (vehicle +0x9C, turret +0x94) in **every** state (`interpolate_state` copies a whole state
     first), and prime `Reserved_network_to_game_hash` 0x1290C60 and `Network_handle_to_rop_hash` (buckets +0x40, size
     +0x44, i.e. 0x1290BC8 / 0x1290BCC) as size 1 **with buckets pointing at a real zeroed dword**. A NULL bucket array
     still crashes after the divide (0x5B63A5) [confirmed, gap round].
  2. Melee and trigger edge counters (`human+0xC4`) only advance under `Multiplayer`, so a swing would re-fire every frame.
     Fix: lane L5 (repoint the flag operand at 0x6B19C6 and 0x6CC4A1 to a co-op byte; both guarded blocks only bump
     counters, checked by two judges), or write the counters back after each `process_state` [confirmed].
  3. `mp_sim_state` is never reset in SP (`player::serialize` resets it only in MP and clears `+0xC4` bit 0). Call
     `reset` 0x568B20 once [confirmed].
  4. Weapon switch and zoom are not in `process_state`. On EQUIP, call `human_add_item_to_inventory` (three overloads,
     0x6B4E20 / 0x6B52A0 / 0x6B52C0: pick by PDB signature), then `player_equip_item` 0x6C2770 and
     `player_set_zoom_state` 0x6851A0 [confirmed; the inventory add is a judge graft].
  5. **First-frame crash** (gap round): the non-local tail of `player_do_frame` calls `multi_game_stats_record_weapon_time`
     (0x6E6B70) and `_backpack_time` (0x6E6B9C) with no `Multiplayer` test; both read `[Mp_game_data+0x610]` with
     `Mp_game_data` NULL in SP (0x5862E6). Fix: 0x6E6B44 `8B CE` → `EB 5E` (jmp 0x6E6BA4, as the local path does in SP)
     [confirmed].
- **Local-player leaks on the puppet path** (gap round; one-site fixes in `notes/gaps.md`):
  - `human_process_controllers_pre_integrate` scales every airborne player-class human's velocity by the **local**
    stick (0x653124 → 0x65316E). Cave at 0x653124: non-local players read their own +0x117C / +0x1180, which
    `apply_movement_state` fills [confirmed];
  - "is a player" used as "is the local player": crouch / stand flip your camera framing (0x630B07, 0x680783); the
    puppet's melee lunge follows your view in SP (0x67ED30); leaving a turret pops or retargets your camera (0x6C0A05);
    manning a flagged turret writes your camera (0x6B5F89); minor: steering seed 0x62D185, burning-car shake 0x6E696D,
    SP tooltips 0x6BC1AC, first-person spinebend (rule: no first-person camera mod during a session) [confirmed /
    partially];
  - the puppet uses the observer's upgrades (every `upgrade_*` getter reads the local meta data); harmless in v1, where
    both machines run the host's blob [checked by one investigator];
  - the static audit followed direct calls only (depth 7, 1,616 functions); vtable calls and global per-human passes
    outside `player_do_frame` / `human_do_frame` are not covered, so E2 logs camera and HUD writes in the puppet window.
- **Patches:** skip `player::do_frame` for puppets (call 0x6E62D7: backpack theft, district HUD); skip
  `player_process_object_interaction` for puppets (call 0x6E6B36: puppets eat pickups); puppets must never sit at index 0
  of `World.subtype_objects[3]` [confirmed].
- **Fallback:** an AI-yielded `npc` moved by `human_move_direct_travel_to` (weaker: no alert line of sight, no ally
  squads, no Player anim set).
- **Decided by:** E2 (ghost puppet).

### 3.2 Death and the reload flow
- **Problem.** In SP the death branch of `gamestate_gp_simulate_frame` runs for **every** player-subtype object, with no
  local-player check. Four seconds after death: in a mission `mission_terminate(0)` (mission fails); in some activities
  their own death handler; otherwise `world::save_and_continue(NULL,false,true)` (disk autosave) and the death screen.
  So a puppet dying on the host can fail the host's mission [confirmed].
- `player_respawn(player*, vector*, matrix*, bool)` 0x6DF5F0 works in SP but always calls `player_reequip`, which wipes
  the campaign inventory. The `(player*, int, bool)` overload 0x6DFAB0 does nothing in SP [confirmed].
- **Solution:**
  - never_die (`+0xDCC |= 0x10`, never 0x20, which mission cleanup clears) on both puppets always;
  - the 0x6E59E7 chain as a second guard;
  - the `human_damage` firewall swallows damage to puppets (so `human_died` never runs and local career stats stay clean);
  - for the players themselves, from M7b: never_die plus a co-op **downed** state. Revive = HP back to max plus
    `human_ragdoll_get_up` 0x6D4CB0, no `player_respawn`;
  - out-of-world teleport, because never_die also disables kill volumes [confirmed].
- **Decided by:** E6.

### 3.3 One world on two machines (join and re-join)
- **Problem.** Mission, activity, script, team and zone state load only during a full territory load. The guest needs the
  host's world, and its own save must survive.
- **Solution:**
  - **Join:** the host makes a silent blob in free roam; the guest copies it into `world::get_load_buf`, calls
    `platform_load` and `set_pending_territory_load("",0,0,0,dlc,0)`.
  - **Re-join:** after any host territory load, the host re-sends the buffer it just loaded,
    `stored_world_state[World+0x2578]`, unchanged.
  - **Guest fences, from connect until the guest process exits** (gap round): such a buffer can carry a running
    mission (after a checkpoint "continue") with its managed scripts, or an activity to force-start (after an activity
    replay). On the guest: NOP 0x39D69C (no mission from any blob), `C3` at the `mission_activate` entries 0x38E560 /
    0x39D960 (backstop), skip `script_do_frame` at 0x3EE1FB, chain 0x3EDF23 to clear `Activity_node_start_on_load`
    0x1250244 before `gameplay_do_frame` runs [confirmed paths; patch set inferred]. With these the host never has to
    sanitize a blob.
  - **Save guard:** skip the call at 0x811278 from connect until the guest process exits. Also skip
    `world::save_and_continue` 0x51D040 on the guest (`ret 0xC`), so no in-RAM save resets its state.
  - **Guest loads:** the 0x51C320 detour turns any load the session did not start into a re-join request. It is not
    side-effect free: Esc → Load runs `platform_load(Pending_save)` first (0x3F032E), and a refused warping video drops
    to the main menu (0x306F26). So the guest's Esc Load / Restart / Cancel mission, fast travel and death Continue are
    blocked in the UI, before their own side effects [confirmed paths].
- **v1 limit:** the guest plays with the host's inventory, salvage and upgrades (they live on the player object inside
  the blob) [confirmed].
- **Decided by:** E3 (blob round trip), E8 (handle agreement) and E10 (guest fences).

### 3.4 Everything follows one local player
- **Problem.** Every one of these centres on the host alone [confirmed]:
  - streaming centres on the host's camera (`World+0x2710`), with zones within ±510 m and physics within ±340 m. These
    radii shrink at runtime to 170/336.6 m, or 102/168.3 m, under stream-memory pressure;
  - NPC squads spawn within 250 m and despawn beyond 300 m of the local player;
  - AI perception runs at reduced rate beyond 40 m (80 m in a vehicle);
  - `npc_process_all` deletes NPCs in zones that are not streamed;
  - 42 script callbacks read the global `Player`.
- **Solution v1:** a leash (warn at 100 m, auto-teleport to the host at 200 m or at the live `layers[0].stream_radius`
  − 40 m, whichever is smaller). Host-only mission steps.
- **v2:** the local-player gates (lane L6 at 0x65CC13 / 0x65CCD3, spawn and despawn distances, script callback swaps).

### 3.5 NPC authority
- **Problem.** Lockstep is impossible, and the engine has no NPC replication at all [confirmed].
- **Solution: host-authoritative mirror.**
  - The guest's own population is off **at its drivers** (gap round) [confirmed]:
    - replace the call at 0x3EDF23 (`gameplay_do_frame`, its only caller) with a whitelist that runs only `srm_do_frame`
      0x75D300 and `object_mission_start_node_do_frame` 0x6B3F00. That one site removes spawn nodes, Marauder raids,
      activity start and spawn, reinforcements, drop-offs, roadblocks, morale, action-node props, ambient peds, traffic,
      ambient EDF and local alert computation;
    - skip the single calls to `convoy_do_frame` 0x3EDF06, `ambush_region_do_frame` 0x3EE1EC, `safehouse_vip_do_frame`
      0x3EE1F1, `mission_system_do_frame` 0x3EE1F6, `script_do_frame` 0x3EE1FB and `activity_system_do_frame(1)` 0x3EE4DF;
    - `activity_suspend_activities(5)` 0x356550 after each load stays, but it does **not** stop raids (they are not
      activities).
  - **Backstops:** refuse `spawn_npc` 0x747F40 (params in EAX) and the only `sp_spawn_vehicle` call 0x757BF7. Every
    runtime NPC and every automobile, flyer and walker is created at one site behind these two (0x748132, 0x748680), and
    ambient peds and traffic do reach them (traced in the gap round). Optionally refuse `spawn_squad` 0x3B0D60, which
    creates an empty `object_squad` before its members.
  - A 1 Hz **sweeper** destroys any NPC, vehicle or turret the host has not announced within 2 s. It is still needed for
    objects restored from zone or save state (`create_object_from_stub` bypasses both refusals), load-time and
    stream-notify spawns, and anything missed. Mirror copies are non-persistent (clear `obj+0x55` bit 5) so the guest's
    own zone state never brings them back with local handles [inferred].
  - **Turrets and roadblock barricades bypass both refusals** (`object_turret_spawn_node::spawn_child_object` 0x66D700,
    `create_barricade` 0x663020 → `object_mover_create_from_chunk`). The base zones alone hold 78 turret nodes at
    spawn_prob 1.0, spawned at load (`object_spawn_do_spawn()` from `setup_territory` 0x540AED). They are a third mirror
    class: bind node turrets by node handle, barricades by roadblock node handle + formation index [confirmed facts;
    rule inferred].
  - Copies use AI yield plus `human_move_direct_travel_to` 0x6309A0, with `human_teleport` 0x69A3C0 when more than 4 m off.
  - A replayed death runs `human_died` locally (morale, stats, civilian conversion, alert; and
    `vehicle_queue_damage(…,0x7FFFFFFF)` at 0x6C7E47 when the victim pilots a flyer). Alert is overwritten by the host;
    morale and stats are never saved on the guest; the flyer case needs the M8 vehicle firewall [confirmed].
- **Yield caveats** [partially]:
  - a dead NPC still runs `do_death_func`;
  - yield also skips stun and on-fire updates;
  - a trigger held at the moment of yielding stays pulled (release it yourself);
  - pathfinding keeps following its last path (clear it).
- **Decided by:** E5.

### 3.6 Geo-Mod destruction
- **Problem.** SP and MP share one event queue. MP syncs by host event replay plus chunk pairing in creation order, but
  every stage of that is gated on `Multiplayer` [confirmed]. The gap round found what makes a plain SP replay diverge
  [confirmed]:
  - the **SP debris load balancer** (`rfgl_desired_values` 0x31D000, one caller) decides how many broken pieces stay as
    chunks and how many are deleted, from each machine's camera distance (75 m) and visibility, the measured time of the
    last Havok step, decaying history and the Havok memory status;
  - **immediate slices and uber-heavy explosions** inside the apply depend on the local `Framecount` and the live Havok
    velocity (the uber flag also changes stress);
  - the **shardify list is unsorted** in SP;
  - an **out-of-queue deleter** (`rfgl_maybe_delete_objects` 0x324C10) fades or destroys debris by camera and
    visibility;
  - **chunk physics** steps once per frame with each machine's `Frametime`;
  - **RNG** (secondary): every Vlib draw only reads a boot-filled 2048-entry pool by one global index (+0x9C4); the
    render thread draws from the same index (`rl_gpu_vis_get_state`, live by default); the CRT `rand()` used in the apply
    is per thread. The geometry-relevant draws are only the SP immediate-slice ones. The `int_rand_range` calls in
    `rfg_dyn_create_shard` are fade lifetimes, not shard counts.
  - Stock MP removes the first three with plain `cmp [Multiplayer]` branches that touch no MP state.
- **Solution:** chain the call inside `postCollideCallback` (0x4102E0), on the sim thread (no worker threads exist).
  - Host: copy the step's queue entries (0xEC-byte prefix only; stress breaks are queued too), the step's
    `rfgl_desired_values` results and the range of chunk destruction uids it created.
  - Guest: mark its own entries skipped (flags +0xC8 bits 0/1), append the host's (handles remapped; the owner must map to
    a `player` object because of `player_damage_only`), call the original once per host step, then pair new chunks **by
    destruction uid** (copy `Rfg_mover_destruction_uid` 0x1A3733C at join; check the uid delta per step; a mismatch marks
    the step unpaired).
  - **MP determinism profile on both machines** for the session: repoint the `Multiplayer` operand (site+2) of the cmps at
    0x339EA3 (immediate slice), 0x339FE9 (uber explosion), 0x33811A (camera/visibility flags), 0x3249AC (sorted list),
    optionally 0x332772 (constant shatter impulse) to a co-op byte = 1. Never open the `create_shard` sites,
    `rfg_dyn_do_mode_specific_processing` or the 0x3383CF client path (MP state, NULL in SP).
  - Hook `rfgl_desired_values`: the host logs, the guest applies the host's quality/delete counts when count and flags
    match. On the guest clear `RFG_load_balance` 0x124F0FB (no local deletions); the host's deletions arrive as despawns
    by uid.
  - **Chunk pose lane** (new, before M6 sign-off): the host sends {uid, pos, orient, lin/ang vel} for the top ~10 active
    and ~20 settling chunks at 10 Hz; the guest eases them with `havok_body_set_pos_orient` 0x423790 /
    `havok_body_set_linear_velocity` 0x423870. The MP pose path (`apply_compressed_state`) cannot run in SP.
  - Optional RNG sync: a private pool and index (plus the CRT `_holdrand` via msvcr100 `_getptd`) swapped in around the
    apply on both machines. Setting the global index alone is not enough because of render-thread draws.
  - Guest-only suppressions inside the apply must keep the draw count (delete a salvage item; do not skip the roll).
  - **Restore-list regrows** (mission and activity starts) are a third path outside the queue: RESTORE event (§3.9).
  - On the guest, the destruction consequences that change campaign state are switched off by operand repoints:
    `gfm_change_edf_control` (0x3B0026) and `general_mover::apply_damage` (0x68A925) (§3.7). Without them a replayed
    collapse can change district control and even liberate a district on the guest.
  - Never inject `DET_MP_DESTROY`: `rfg_dyn_apply_mp_destroy` dereferences the NULL `mp_info` at +0x110 in SP.
  - Fallback: re-inject through `rfg_dyn_add_pre_collide_*`.
- **Prediction** [inferred]: the static remainder of a building (what is gone) replays identically from the full event
  stream; the chunk set matches only with the profile and the host's balancer values; poses match only with the lane.
- **Drift counter:** compare `object_mover+0xB0` checksums every 2 s. v1 repairs drift only at the next re-join.
- **Cost on the host** [inferred small]: no slicing of moving chunks, no random uber-heavy flourishes, no view-based
  debris reduction (bounded at 20 pieces per break-up and 10 quality chunks per frame). The user decides (§8, Q10).
- **Decided by:** E4 (redesigned, §6).

### 3.7 Damage authority and double counting
- **Problem.**
  - A puppet's weapons are real in the observer's world, so its fire would damage things twice.
  - Victim-side modifiers (difficulty, armour, vehicle columns) key on the local `Player` [confirmed].
- **Solution:** one firewall in a `human_damage` prologue detour (8 position-independent bytes `83 EC 0C 56 8B 74 24 14`),
  and in M8 also `vehicle_apply_damage` 0x7BF870. Rules in §4.6. In v1 the guest's puppet on the host fires **real**
  weapons from the replicated aim and trigger, so the guest acts on the host's world by remote control:
  - no hit protocol is needed;
  - the cost is about one RTT plus interpolation of lag on the guest's shots [confirmed mechanics, lag inferred].
- **General movers** (gas tanks, EDF props) take damage through `general_mover::apply_damage` 0x68A920, which neither
  firewall covers and which reaches `drop_control` (district control). On the guest, repoint the operand of its MP-client
  gate (0x68A925, compared with 1) to a guest byte = 1: local damage is swallowed, damage passed with the last bool true
  still applies [confirmed; relies on `Server` 0x2BEA590 being 0 in SP, a one-line M0 log check].
- **M10+:** shooter-side hit capture at the engine's MP send sites (T1, T2 in §4.3).

### 3.8 Vehicles
- **Problem.** Vehicle physics is not deterministic; the hijack rule ejects friends; with no resolved driver, vehicle AI
  zeroes injected inputs every frame [confirmed].
- **Solution:**
  - The driver's machine owns movement. The host arbitrates seats and owns HP and wrecks.
  - The copy carries the driver's puppet really seated in seat 0 (MP enter recipe), with the puppet's `+0xAC4` set so
    `player_occupied_vehicle` keeps the car alive in unstreamed zones.
  - The copy is driven by `automobile_set_vcontrols` plus Havok velocity easing, or by lane L2.
  - Wrecks use `vehicle_mp_force_blow_up` 0x7B71F0. Aircraft are moved by state only (random hover noise).
  - Before M8: the puppet turret camera fixes at 0x6C0A05 and 0x6B5F89 (§3.1); otherwise the partner manning or leaving a
    turret moves your camera. On the guest, a replayed death of a flyer pilot copy and kill regions
    (`trigger_region_do_frame` → `vehicle_blow_up`) still damage mirrored vehicles locally; the vehicle firewall must
    swallow those [confirmed paths].
- **Decided by:** E7.

### 3.9 Missions and activities
- **Problem.** Scripts use the RNG and react to AI, physics and audio timing. They read the single `Player`. Mission end
  bypasses `gem_post_event` [confirmed].
- **Solution: the guest has no mission object at all** (gap round):
  - missions and activities run on the host only; on the guest `Mission_active_index` is always -1 and no script runs;
  - **fence, from M5** (re-join needs it, not only M9): NOP the store at 0x39D69C, `C3` at the `mission_activate` entries
    0x38E560 / 0x39D960, skip `script_do_frame` at 0x3EE1FB, and keep refusing `player_start_mission` 0x633F40 (for the UI:
    no briefing). Why: a host checkpoint blob sets `Mission_checkpoint_restore_index`, and `world::setup_territory` then
    calls `mission_activate` itself (0x540AB0); the blob's script section also restores the mission's managed action
    scripts, which `script_do_frame` runs whenever no mission is active [confirmed]. The other entries (autostart at
    0x65B783, briefing paths) are dead after the tutorial on a guest that refuses `player_start_mission` [confirmed by
    one investigator each];
  - **MISSION_START** teleports the guest to the host and carries the start node, whether the start was fresh, and the
    host's reserved model set. On a fresh start the guest calls `restore_list_do_restore(start_node)` 0x375180 so the
    buildings the host regrows also regrow on the guest. The guest pre-streams the models with
    `srm_reserve_slot(SPAWN_SLOT_MISSION, 0, info)` and must **never** call `srm_mission_reservation` (it wipes the
    ambient reservations, picks variants at random, and then `Mission_mode` blocks every further reservation) or play an
    engine cutscene (`cs_play_internal` sets `Mission_mode` too). The same-team model fallback stays only as a safety
    net for a full 17-record budget or slow streaming [confirmed];
  - objectives are mirrored by table index (`add_objective` 0x464FF0, `complete_objective` 0x475D70, end from
    `mission_terminate_internal` 0x3B4970), plus a **full objective snapshot after every re-join**: at a checkpoint
    restore the host's objectives are added during its own load, before the guest is back [inferred];
  - **icons and markers go by position**: mission layers hold only logic objects (2,786: navpoints, spawn nodes,
    dummies, triggers; no building, mover, vehicle or NPC), and the guest does not load them. 72 of 79
    `add_mission_objective_icon` targets and all 40 ground markers are layer objects; about 24% of icon and highlight
    handles overall. Objective *buildings* (restore, destroy, count) are base-zone objects with identical handles while
    their zone is loaded. ICON / MARKER events carry `{element, world pos, base-zone handle or 0}` [confirmed];
  - never call `hud_ui_mission_complete_start` (its exit reloads), `mission_terminate_cleanup` or `mission_cancel` on the
    guest; show the result as a HUD line;
  - **retry**: after a fail without a checkpoint, an abandon or Esc → Cancel mission, the host reloads the pre-mission
    anchor (mission index -1) and nothing re-activates the mission: the host restarts it at the start node. Only
    "continue" after a checkpoint re-activates a mission. Exactly 8 missions have checkpoints (WMR, PartyTime, H2H, GoT,
    STGC, EBS, Assault the EDF CC, Final); DFA has none [confirmed];
  - host story videos show as a pause screen on the guest in v1. Videos on the guest are only ever played with warp -1,
    zone 0, DLC 0 and no credits: then they never load or save [confirmed];
  - a mission completion can liberate a district through `mission_completed` → `unlock_new_missions` →
    `liberate_district` (0x3ABB4D); "no mission on the guest" covers it;
  - co-op starts after the tutorial.

### 3.10 Reload storms
- **Problem.** Every host fail, retry, fast travel and warping video means a reload for both machines.
- **v1:** re-join after each one, about 10–30 s each [inferred]. never_die on players removes the most common cause,
  deaths. The re-join blob may carry a running mission or an activity to start; the guest fences (§3.3) make that
  harmless. A district liberation reloads only for Dust (the safehouse warp after the liberation screen) [confirmed].
- **Later:** anchor-blob forwarding (§4.8) so both machines load at the same time.

### 3.11 Pause, focus and time
- **Problem** [confirmed]:
  - alt-tab pushes GS_QUICK_PAUSE;
  - a minimized window blocks the sim thread (render handshake);
  - `Frametime` is clamped at 0.1 s at runtime;
  - a slow-mo cheat changes `Frametime_divisor`.
- **Solution:**
  - patch 0x4DDB5F while a session runs;
  - never minimize (borderless window);
  - all network timing on the wall clock (QPC) in the net thread;
  - host pause sent to the guest, applied with its own balanced pause token;
  - no slow-mo during a session.

### 3.12 Transport and NAT
- **Problem.** The engine stack is unusable at `Multiplayer` 0. sw_api P2P depends on THQ Nordic servers: the stock NAT
  server is dead, and the user's patched copy reaches `nat2.thqonline.net` but its relay was never shown to work
  [confirmed from the user's logs].
- **Solution:** ENet 1.3.18 with a direct IP and a port forward, or any virtual LAN. A NAT punch or relay comes later, if
  ever.

### 3.13 Save safety
- **Problem.** The whole 31.5 MB save image is rewritten from RAM on every disk save. A guest death during a session puts
  the host's world into the guest's autosave slot **in RAM** [confirmed].
- **Solution:**
  - the guard at 0x811278 stays on until the guest process exits;
  - from M5 the guest also skips `world::save_and_continue` 0x51D040 (no in-RAM save at all: death, upgrade-shop exit,
    save UI) [gap round];
  - puppets carry `no_save` (`+0x54 |= 0x20`) and are recreated after every load; mirror copies are non-persistent
    (`obj+0x55` bit 5 clear) so the guest's zone state never keeps them;
  - back up `keen_savegame_0_0.sav` before every test.
- **Hard rule:** any test that damages a save stops all feature work.

---

## 4. Recommended architecture

**Base design: "MVP first" (host-authoritative mirror with player puppets)**, from
`findings/workflows/coop_design_mvp_first.md`. Two of three judges picked it (7.5 and 7.5). The third picked the
puppet design (8.0) for player experience.

Why MVP-first:
- the smallest hook surface (about 12 call-site chains, one prologue detour);
- the host stays vanilla;
- small, bisectable builds with LOC estimates;
- one reload rule;
- cheap make-or-break experiments first.

**Grafted in from the judges** (source in brackets):
1. never_die is the **primary** death guard for puppets, and for the players from M7b. The 0x6E59E7 chain is the
   backstop. Downed/revive restores HP instead of calling `player_respawn`. Out-of-world teleport. [puppet]
2. On the guest, mirrored NPCs, vehicles and the host's puppet swallow **all** local damage. Their deaths and wrecks
   arrive only as host events. This closes MVP's ghost-NPC gap. [puppet, engine-MP]
3. A hash of the `data\*.vpp_pc` name+size list goes into the handshake. [puppet, engine-MP]
4. Before the puppet's `player_equip_item`, add the weapon with `human_add_item_to_inventory`. Real puppet fire depends
   on it. [judges]
5. Lane L5 replaces the hand-written melee and trigger counter writeback. The writeback stays as an ini fallback.
   [engine-MP]
6. Prime the four MP handle hash tables as empty but valid (a crash-to-desktop class removed). [engine-MP]
7. Guest population: `spawn_npc` / `sp_spawn_vehicle` refusal plus a 2 s sweeper. [puppet]
8. Lane L2 is tried against `set_vcontrols` plus easing in E7. [engine-MP]
9. Measure the longest gap between ticks (E9) before fixing ENet timeouts. [puppet X9]
10. Split the big "shared enemies" build into M7a (mirror) and M7b (danger). [judge 2]
11. AI noticing of the guest is tested in M7b with the gate patches ready behind a toggle, not deferred to M10+.
    [judge 2]. This pass narrowed it: an on-foot hostile puppet is already noticed every call (§9).
12. M10+: guest-side hit detection through the MP send taps T1 and T2, with the `multi_game_stats_record_hit` call at
    0x7E58AA neutralised as well. [engine-MP, judges]
13. Anchor-blob forwarding is the first reload-cost upgrade, kept out of v1. [puppet]

### 4.1 Processes and authority

Two copies of the GOG `rfg.exe`, each with RfgCoop as `xinput1_3.dll` (other mods may sit beside it as `dinput8.dll`
or `winmm.dll`). Exactly two players. Messages carry a peer id, but 3–4 players are not built.

| State | Owner | The other machine shows |
|---|---|---|
| Each avatar: movement, aim, animation, weapon events | that player | puppet `player` driven by snapshots |
| Guest's attacks on the world | host re-simulates them: the guest's puppet fires real weapons (buildings from M6, NPCs from M7a) | guest sees its own shot at once (cosmetic) and the real result when host events arrive |
| Damage to the guest | host detects the hit on the guest's puppet; the guest finalizes it (difficulty, armour, downed) | - |
| Damage to the host | host (vanilla) | - |
| NPCs, squads, spawning, AI, alert, morale, districts, time | host | AI-off copies, live setters |
| Unoccupied and NPC-driven vehicles; vehicle HP and wrecks | host | copies driven by state |
| A vehicle's movement while a player sits in seat 0 | that driver's machine | copy driven by the driver's state |
| Seats | host (arbiter) | - |
| Geo-Mod event order and chunk creation | host | guest drops its own events and replays the host's |
| Missions, activities, scripts, saves, territory loads | host | guest has no mission object and runs no script; mirrors objectives, icons by position, HUD, restore-list regrows and the mission model set |
| District control, liberation, unlocks, morale, stats | host | guest writes district control raw; its own consequence code is switched off (§3.6, §3.7) |
| Turrets and roadblock barricades | host | bound by spawn-node handle (+ formation index) |
| Inventory, salvage, upgrades | v1: the guest has the host's (from the blob); not synced | - |
| Chunk set, chunk deletions, large-chunk poses | host (from M6) | uid pairing, host balancer values, despawns by uid, pose lane |
| Pickups, collectibles, effects, decals | local per machine | not synced |

### 4.2 Transport and protocol
- **ENet 1.3.18** vendored in `RfgCoop/third_party/enet` (C, MIT, 9 files). Built with the existing llvm-mingw i686
  `-static`, linked `-lws2_32 -lwinmm`.
  - Every `enet_*` call lives in `src/net.cpp` on one net thread, created on the first tick (never in DllMain: WSAStartup
    under the loader lock is unsafe).
  - The net thread never touches game memory. Two SRWLOCK-guarded queues connect it to the sim thread.
  - Note: our WINMM imports resolve to a proxy `winmm.dll` if another mod installs one beside rfg.exe. `enet_initialize`
    calls `timeBeginPeriod(1)` process-wide [confirmed].
- **Connect:** UDP port from `RfgCoop.ini` (default 27777, clear of 1005 syslink and 23022/23023 sw_api). Direct IP plus a
  port forward, or Tailscale / ZeroTier / Radmin. Windows Firewall prompts once on the host.
- **Timeouts:** about `enet_peer_timeout(peer, 32, 15000, 60000)`, final values from E9. LOADING, PAUSED and JOINING are
  sent explicitly, so a long load never looks like a drop.
- **Channels:** 0 reliable ordered (control, events, spawns, destruction, missions); 1 unreliable sequenced (snapshots);
  2 reliable (world blob, 1.3 MB typical, ENet fragments it).
- **Rates:** avatar 20 Hz (about 60 B); NPC and vehicle states 10 Hz (≤ 100 NPCs × 24 B ≈ 24 KB/s, no interest
  management in v1 because the leash keeps everything near); destruction as produced.
- **Framing:** packed little-endian structs with a type byte. Exact size and id range are checked on receive, unknown
  types are dropped, nothing received is ever executed, and the session password is proven by a mutual HMAC
  challenge-response, never sent (notes/session_impl.md "Password"). No compression until
  measured.
- **Handshake** (HELLO / WELCOME / REJECT, the reason shown on screen):
  - protocol version;
  - rfg.exe PE stamp and SizeOfImage;
  - CRC32 of `xinput1_3.dll`, `winmm.dll` and `dinput8.dll`;
  - hash of the `data\*.vpp_pc` name+size list;
  - the avatar's `Master_human_info_list` index;
  - password: not in HELLO; CHALLENGE carries a per-session salt, then the guest proves it in AUTH and the host in
    WELCOME (kProtocol 3).
  - Do **not** hash `sw_api.dll`. RfgCoop never uses it, and the user's two installs differ there on purpose (the
    GOG GALAXY copy is community-patched).
  - Checked this pass: the `data\` listings of both installs are identical in name, size and date. `terr01_l1.vpp_pc` is
    4,448,220,211 bytes, dated 2023-11-27, in both. So the Unification patch's `terr01_l1` change is probably in neither
    [inferred from the dates; contents not hashed].
- **Lag/loss shim** in `net.cpp` (`lag_ms`, `jitter_ms`, `loss_pct`). The engine's own simulators are dead code
  [confirmed].

### 4.3 Threads and hook points

| Site (RVA) | Original | Use | Fires when | From |
|---|---|---|---|---|
| **0x1D24D9** | `call multi_process` 0x604C20 | session tick: write `Clock_current_time` (QPC ms, start at 10,000), drain inbox, session state machine, load and pause detection, title and HUD status | every sim frame in every SP state except GS 4/5 [partially] | M0 |
| **0x3EE55C** | `call gamestate_gp_simulate_frame` 0x3EDB60 | before: apply puppets, NPC/vehicle copies, AI yield bits; after: capture own avatar and (host) world snapshots | gameplay only; skipped while `s_pauseGame` | M3 |
| 0x6E62D7 | `call player::do_frame` 0x6D5660 | skip for puppets | per player | M3 |
| 0x6E6B36 | `call player_process_object_interaction` 0x6DE050 | skip for puppets (pickups) | per non-local player | M3 |
| **0x6E59E7** | `call player_is_killed` 0x634360 (only caller) | puppets never arm the death timer; from M7b, also a downed guard for local players | per dead player-subtype human | M3 |
| 0x6CE950 prologue | `human_damage` (8 clean bytes) | damage firewall (§4.6), downed detection | every human damage | M3 |
| 0x6B19C6, 0x6CC4A1 | `cmp byte [Multiplayer],0` operands (lane L5) | repoint at a co-op byte: melee and trigger counters advance as in MP. Toggle; writeback as fallback. NPC counters tick too [inferred harmless, checked in E2] | per swing / trigger pull | M3 |
| MP handle tables | `Reserved_network_to_game_hash` 0x1290C60, `Network_handle_to_rop_hash` (+0x40/+0x44 at 0x1290BC8/0x1290BCC) | size 1, buckets → a zeroed dword, during a session; restore after | - | M3 |
| **0x6E6B44** | `mov ecx,esi` (`8B CE`) before the non-local MP stats calls | `EB 5E` (jmp 0x6E6BA4): puppets skip `multi_game_stats_record_*` (NULL `Mp_game_data` crash) | per non-local player | M3 (first E2 build) |
| 0x653124 | `mov al,[Multiplayer]` (`A0 abs32`) in `human_process_controllers_pre_integrate` | cave: non-local players take the MP read of +0x117C/+0x1180 instead of the local stick | per airborne player | M3 |
| 0x630B07, 0x680783, 0x67ED30 | crouch/stand `camera_free_set_submode` calls; SP camera-forward copy in `human_combat_play_combat_move` | thunk / cave: only for `Player` | puppet crouch, melee | M3 |
| 0x6C0A05, 0x6B5F89 | turret abandon / man camera writes | cave: camera writes only for `Player` | puppet on a turret | M8 |
| **0x4102E0** | `call rfg_dyn_do_collide_post` in `postCollideCallback` | host capture / guest drop and inject; uid range, rfgl records, optional private RNG | each Havok step | M6 |
| operands of 0x339EA3, 0x339FE9, 0x33811A, 0x3249AC (opt. 0x332772) | `cmp byte [Multiplayer],0` in the destruction apply | co-op byte = 1 on both machines: the MP determinism profile | per break-up | M6 |
| 0x31D000 | `rfgl_desired_values` (one caller 0x3383FD) | host logs, guest applies the host's quality/delete counts | per shardify | M6 |
| `RFG_load_balance` 0x124F0FB | data byte | 0 on the guest (no local debris deletions) | - | M6 |
| **0x811278** | `call requestSave` (only caller) | guest save guard, connect → process exit | any disk save | M5 |
| 0x51D040 prologue | `world::save_and_continue` (9 clean bytes) | guest: `ret 0xC` (no in-RAM save) | any save | M5 |
| 0x51C320 prologue | `world::set_pending_territory_load` (`push esi; mov esi,ecx; call rel32`) | guest: allow only the session's own load, else log the caller, return false, send REJOIN_REQ; UI entries are blocked earlier (§3.3) | any load | M5 |
| **0x39D69C** | `mov [Mission_checkpoint_restore_index],eax` (`A3 abs32`) in `mission_system_load` | guest: 5 × NOP (no mission from any blob) | every load | M5 |
| 0x38E560, 0x39D960 | `mission_activate` entries | guest: `C3` (backstop for every activation path) | mission start | M5 |
| 0x3EE1FB | `call script_do_frame` | guest: skip (the second caller 0x39D93D is dead while no mission is active) | every frame | M5 |
| **0x3EDF23** | `call gameplay_do_frame` (only caller) | guest, M5: clear `Activity_node_start_on_load` then call; M7a: whitelist `srm_do_frame` + `object_mission_start_node_do_frame` | every frame | M5 / M7a |
| 0x633F40 | `player_start_mission` | guest: refuse (UI half of the fence) | mission start | M5 |
| 0x4DDB5F | `jne` after `os_foreground` | `75→EB` while a session runs (restore after; dev=1 keeps it the whole run) | focus loss | M1 / M5 |
| 0x3D9159 | `call process_save_load` 0x3C00E0 in `gamestate_gp_handshake` | chain: a requested silent capture sets its save bits right before it (re-checked free roam), so no vanilla autosave of that frame is refused | each gameplay handshake | M5 |
| operands 0x3B0026, 0x68A925 | `cmp [Multiplayer]` in `gfm_change_edf_control`, `general_mover::apply_damage` | guest byte: no district control change, no local general-mover damage | per consequence | M6 |
| 0x3EDF06, 0x3EE1EC, 0x3EE1F1, 0x3EE1F6, 0x3EE4DF | convoy, ambush, VIP, mission system, `activity_system_do_frame(1)` calls | guest: skip | every frame | M7a |
| 0x747F40 prologue, call 0x757BF7 (opt. 0x3B0D60) | `spawn_npc` (params in EAX), the only `sp_spawn_vehicle` call (opt. `spawn_squad`) | guest: refuse unless the mirror is spawning (backstops) | spawns | M7a |
| optional operands 0x359DF2, 0x359D82, 0x359D32, 0x3AB64C, 0x3D4F35, 0x3D4DC3, 0x611906, 0x611A76 | MP early-outs in activity, morale, stats and meta hooks | guest byte (guest-local drift only) | per consequence | M7a |
| 0x7BF870 prologue | `vehicle_apply_damage` (6 clean bytes `sub esp,0x170`) | vehicle damage firewall | vehicle damage | M8 |
| 0x79BDD2 (lane L2, optional) | 18-byte MP gate in `automobile_do_frame_presim` | engine replays remote-driven cars from a container at `car+0x4E88` (size 0x1E1C, reset 0x5691D0); re-check and clear before the car is freed | per car | M8 (E7 decides) |
| 0x38E560 + 0x39D960, 0x3B4970, 0x464FF0, 0x475D70, 0x30A4A0, 0x375180, 0x73A380 | both `mission_activate` overloads (fresh starts come through 0x39D960, checkpoint restores through 0x38E560), `mission_terminate_internal`, `add_objective`, `complete_objective`, `video_cutscene_play`, `restore_list_do_restore`, `objective_highlight_add` | host: emit MISSION / OBJECTIVE / ICON / VIDEO / RESTORE events | mission flow | M9 |
| M10+ only | T1 0x7E585D gate / 0x7E588B call (bullet hit; also neutralise 0x7E58AA `multi_game_stats_record_hit`); T2 0x6D14AE / 0x6D14F2 (melee hit); L6 0x65CC13 / 0x65CCD3; script callback pointers (0x12B4648 …) | guest-side hit detection; host helpers see both players; either player triggers steps | - | M10+ |

Threads:
- **Net thread:** ENet only.
- **Sim thread:** every game call, from the hooks above.
- **Render thread:** nothing in v1. Status goes to the window title (`SetWindowTextW`, for example " [HOST] RTT 34 ms")
  and HUD lines through `hud_message_add`. An ImGui overlay is out of v1 (it would run through the 0x86FFF0
  detour).

Register-ABI trap: `apply_movement_state`, `do_frame_movement`, `apply_weapon_state`, `correct_player_movement`,
`do_bullet_hit`, `spawn_npc` (params in EAX), `set_game_destroyed_common` (mover in EDI) and `mission_activate_pre_save`
(index in EAX) take arguments in registers (LTCG), whatever the PDB says. Only call `multi_game_player_process_state`,
`add_state`, `reset` and the `*_apply_sim_state` functions directly [confirmed].

Every `cmp byte [Multiplayer]` operand and every `[Player]` read in a cave is an ASLR-relocated absolute: expected-byte
checks compare against the runtime address (`A0 88 A5 FE 02` on disk is `A0 <base+0x2BEA588>` in memory).

### 4.4 Object identity
- **One rule:** the net id is the host's 32-bit game handle. Only the host creates world objects. The guest keeps a map
  `hostHandle → localHandle`. On every lookup it re-checks that the object exists and has the same classname hash and
  type bytes (+0x7E/+0x7F), because handles are recycled [confirmed].
- **Zone and saved objects** have identical handles after the guest loads the host's blob [partially: only while the
  zone is loaded on both, which the leash ensures]. Events whose target is missing locally are skipped and counted.
  Of the 663 distinct movers that managed scripts target, 423 live only in streamed `terr01_XX_YY` zones [confirmed, gap
  round].
- **Managed-layer objects** keep their file handle on every machine that loads the layer
  (`(XX<<4|YY)<<24 | NNN<<16 | index`, byte 2 = 0x80-0x8F) [confirmed], but the guest loads no layers in v1, so anything
  placed on them (icons, ground markers) is sent as a position.
- **Turrets and roadblock objects** get runtime handles on both machines: node turrets are bound by their spawn node's
  handle, barricades by the roadblock node's handle plus formation index [inferred rule; probe G7].
- **Host runtime objects** (NPCs, vehicles): SPAWN carries the host handle. The guest adopts a local object with the same
  handle and class (persisted vehicles), otherwise it creates a copy. DESPAWN goes on the same reliable ordered channel.
- **Destruction chunks:** paired by **destruction uid**, not by order (gap round). The guest copies
  `Rfg_mover_destruction_uid` 0x1A3733C at join; with the MP profile and the host's balancer values, each step creates the
  same uids on both machines. The host sends the step's uid range; a different uid delta on the guest marks the step
  "unpaired", and later events on those chunks are skipped. Plain SP order pairing fails by construction (camera,
  visibility and timing decide the chunk count) [confirmed causes; uid scheme inferred].
- **Byte 2 == 0xFF does not prove** a handle was created this session: saves restore such handles [confirmed].
- The map is cleared on every join and re-join.

### 4.5 Per-system sync table

| System | Owner | Sent | Rate | Applied with |
|---|---|---|---|---|
| Avatar | its player | PSTATE: pos, vel, yaw, camera fwd, move input (captured at `player_move_set_anim_direct` 0x69CFA0), the engine's `+0xA4` bitfield (jump and throw ids from `Player+0xC4`, melee and trigger ids via L5), vehicle id, seat, flags (ragdoll, downed, hidden) | 20 Hz unreliable | build a `multi_player_state` (0xAC), fill the cache handles, `network_time` = local arrival + 100 ms, `add_state`, `process_state` each frame |
| Weapon switch, zoom | its player | PEVENT | on change | `human_add_item_to_inventory` → `player_equip_item` → `player_set_zoom_state` |
| Guest's attacks | host | nothing extra | - | the host puppet fires real weapons; charges detonate on the state bit |
| Damage to the guest | host → guest | DAMAGE (amount, type, weapon hash, attacker id, hit pos) | per hit | guest calls `human_damage(Player, mapped attacker, …)` with the "from net" flag set |
| Downed / revived | its player | PEVENT | on change | puppet shows the ragdoll |
| Destruction | host | DESTR: step seq, n × 0xEC-byte event prefixes (handles at +0x50, +0xA8, owner +0xC4), the step's `rfgl` records, the new chunk uid range (optional: RNG index); DEBRIS_DEL: uids the host's balancer faded or deleted | per Havok step, reliable | §3.6 |
| Chunk poses | host | CHUNKS: uid, pos, orient, lin/ang vel for ~10 active + ~20 settling chunks | 10 Hz unreliable | `havok_body_set_pos_orient` / `_set_linear_velocity`, eased, snapped at rest |
| Building regrow | host | RESTORE: restore-list key (fallback: the handle list) | on mission or activity start, or a single restore action | `restore_list_do_restore(key)` or `world::restore_object(h)`; hold destruction events on listed handles until the regrow is done |
| Turrets, barricades | host | TURRET_BIND {node, host handle}; barricade SPAWN {roadblock node, formation index, chunk, transform} | reliable | adopt the guest's own node child or create a copy |
| NPCs | host | SPAWN / DESPAWN / DIE; STATES (pos, yaw, flags crouch/firing/ragdoll/in-vehicle, hp) | reliable; 10 Hz | §3.5; DIE = `human_damage(copy, killer, -1, …)` inside the "from net" flag |
| Vehicles | host, or the seat-0 driver | SPAWN / DESPAWN / STATES (pos, quat, lin/ang vel, accel, steer, bits, hp) / SEAT_REQ / SEAT_SET | reliable; 10 Hz (driver 20 Hz) | §3.8 |
| Alert, time | host | WORLD | 1 Hz | `gsm_set_alert_level`, `game_clock_set_time` when drift > 30 game seconds |
| Districts, destroyed table | host | DISTRICT (handle, control) | on change, 1 Hz max | raw write of `district+0x9C` clamped to `+0xA4`, plus `hud_ui_change_control` for the HUD; **never** `district::set_control` or `gfm_*` on the guest. The liberated flag, progression and unlocks arrive with the next re-join (v1), with a HUD line. `world::object_add_to_destroyed_table` 0x51C830 is pure and stays on |
| Pause | host | STATE paused | on change | `game_pause` / `game_unpause` under one co-op token, always balanced |
| Missions | host | MISSION START (index, start node, fresh flag, host pos, model set) / OBJ_ADD / OBJ_DONE / ICON and MARKER (element, pos, base handle or 0) / END / VIDEO; OBJ_SNAPSHOT after each re-join | reliable | `add_objective` / `complete_objective` by table index, position markers, HUD line, teleport to the host at START, `restore_list_do_restore` on a fresh START, `srm_reserve_slot(SPAWN_SLOT_MISSION)` for the model set |
| Activities | host | RESTORE on start | - | guest: no activity logic (0x3EDF23 whitelist, 0x3EE4DF skip); `activity_suspend_activities(5)` after every load stays as a belt |
| World blob | host | BLOB | join and re-join | §4.8 |

### 4.6 Damage firewall (`human_damage` detour; `vehicle_apply_damage` gets the same rules in M8)

P = any puppet, L = this machine's `Player`, "from net" = a flag set while applying a received event.

| Machine | Victim | Attacker | Action |
|---|---|---|---|
| host | guest's puppet | anyone | return 0; from M7b send DAMAGE to the guest |
| host | L | guest's puppet | return 0 (no friendly fire in v1) |
| host | NPC, vehicle | guest's puppet | run it (it is the guest's real gun), from M7a. Before M7a, return 0 |
| host | anything else | anything else | run it (vanilla) |
| guest | host's puppet, mirrored NPC, mirrored vehicle | any | return 0 unless "from net" (death replay) |
| guest | L | NULL (falls, fire, environment) or L | run it (local) |
| guest | L | anything else | return 0 unless "from net" (copies fire for show; real hits arrive as DAMAGE) |
| guest | general mover (gas tank, prop) | any | swallowed by the 0x68A925 operand repoint unless passed with the last bool true (from M6) |

**Downed detection:** after the original runs on L, if HP is pinned at the never_die value (1.0) by a hit that would
have killed, L is downed.

### 4.7 Death, downed, pause
- **Puppets:** never die (never_die, plus the 0x6E59E7 chain, plus damage swallowed). They show "downed" through the
  ragdoll bit. They are never at index 0 of the player list, carry `no_save`, and are recreated after every load
  (`player_level_shutdown_all` destroys every player at unload).
- **Guest, M3–M4:** vanilla death on the guest (it reloads from its own memory).
- **Guest, M5–M7a:** the death screen's Continue is blocked in the UI and becomes a re-join request (the guest's own
  in-RAM save and load are fenced, §3.3). A refused pending load must also clear its pending bit (`World+0x265D` bit 2),
  or `process_save_load` retries it every frame [inferred].
- **Guest from M7b, host from M7b if the user agrees (§8, Q3):**
  - never_die plus downed: input blocked by the ragdoll, HUD "Downed (10)";
  - revive when the partner holds Use within 3 m, or when the timer runs out: HP to max, `human_ragdoll_get_up`,
    optionally `human_teleport` beside the partner.
- **Team wipe during a mission:** the host clears its own 0x10 bit and dies vanilla (mission fail), then both re-join.
- **Out of world:** never_die disables kill volumes, so a player 50 m under the terrain or in a kill region is teleported
  to the partner.
- Cheats that clear bit 0x10 (suicide) or change `Frametime_divisor` (slow-mo): do not use either during a session.
- **Pause:**
  - host Esc, map, menus or video → PAUSED → the guest pauses with its own token and shows "Host paused";
  - the guest's Esc pauses only the guest;
  - alt-tab never pauses (0x4DDB5F);
  - never minimize;
  - destruction batches that arrive during a guest pause are queued; above 8 MB, force a re-join.

### 4.8 Join, re-join, leave
1. **Guest:** load any save (its own campaign is fine), press F8 (or `autoconnect=1`). HELLO/WELCOME. The save guard and
   the guest fences (§3.3: mission store NOP, `mission_activate` backstop, script skip, 0x3EDF23 chain, no in-RAM saves,
   load detour, UI blocks) turn on now and stay on until the process exits.
2. **Host:** only in free roam. Check: no mission (`Mission_active_index == -1`), `Stream_pause` clear,
   `saving_state_data` (`0x265D` bit 4) clear, no save pending (`0x265C & 0x24`). `World+0x2578` is not checked: it may
   stay 1 after a mission that succeeds without its result-screen save (mode 4/8, or zero reward and morale, e.g. Save
   the Guerrilla Camp, which has a checkpoint), and the silent blob resets it to 0 [confirmed code; probe X8].
   - Set the silent-blob flags and wait for `process_save_load` to fill `stored_world_state[0]`. Check that the size
     changed.
   - `game_pause` with its own token, "Partner joining…".
   - Send BLOB with the host's dlc id (`World+0x2694`), position and time.
   - During a mission, answer "join after the mission".
3. **Guest:**
   - set `World+0x2578 = 0`;
   - `memcpy` into `get_load_buf`, then `set_cur_size`;
   - `platform_load(&info, true)` with a `game_save_info` carrying the host's dlc id;
   - `set_pending_territory_load("",0,0,0,dlc,0)`, retried while it returns false.
4. **Guest, first gameplay frame:** suspend activities, spawn suppression already on (from M7a), copy the host's
   `Rfg_mover_destruction_uid` (M6+), teleport 3 m beside the host (with a streaming wait), create the host
   puppet, send READY.
5. **Host:** create the guest puppet, send the full NPC, vehicle and turret SPAWN/BIND list (M7a+), the district
   controls and the objective snapshot (M9), unpause.

**Re-join (one rule):** the tick watches `gameseq_get_state` 0x3BFC70 for GS_LOAD.
- Host load (death-continue, retry, fast travel, warping video, load game): send LOADING. The guest drops all mirrored
  state and shows "Host loading…".
  - After the host's first gameplay frame, the host re-sends `stored_world_state[World+0x2578]`, the buffer it just
    loaded, **as is**. It never makes a fresh silent blob mid-mission, because that resets `cur_world_state` and changes
    the host's mission restart and cancel behaviour [confirmed caveat]. After a checkpoint "continue" that buffer carries
    the running mission and its scripts, after an activity replay the activity to force-start: the guest fences neutralize
    both, so the blob is never edited [confirmed, gap round].
  - Then steps 3–5 run again.
  - If the host loaded a disk save or started a new game, it makes a silent blob once back in free roam.
- Guest load on its own (M3–M4 death screen): re-join request on its first gameplay frame. From M5 the guest cannot
  start a load: the UI entries are blocked and the 0x51C320 detour turns anything else into REJOIN_REQ.
- The guest's own fast travel, Esc → Load / Restart / Cancel mission during a session show "only the host can do this;
  F7 teleports to the host" (blocked before `platform_load` or any other side effect).
- **Upgrade (M10+, anchor-blob forwarding):** the host forwards each `stored_world_state` it produces (falling edge of
  `saving_state_data`) and announces RELOAD from a 0x51C320 detour. The guest then loads the same bytes at the same time,
  which roughly halves the reload cost. The detour must relocate the E8 inside the prologue
  (`push esi; mov esi,ecx; call rel32`).

**Leave:**
- Guest leaves: the host destroys the puppet and plays on.
- Host leaves: the guest destroys puppets, keeps the save guard on, and shows "Session ended, restart the game to
  continue your own campaign".

### 4.9 Packaging, coexistence, dev setup

```
RfgCoop/
  build.cmd          llvm-mingw i686 build plus the ENet .c files, -lws2_32 -lwinmm
  RfgCoop.ini        role=host|guest, connect=ip:port, port, password, name, hotkeys, dev options, lag shim, toggles
  src/core.cpp       DllMain (build stamp + expected bytes at every site), ChainRel32 + small detour helper, log,
                     VEH crash logger, xinput forwarding, RfgCoop_Api export
  src/xinput1_3.def  XInputGetState @2, XInputSetState @3 (the only imports rfg.exe uses) + named forwards
  src/game.h         Rva/Field/Fn helpers and addresses
  src/proto.h        message structs + size table
  src/net.cpp        ENet thread, queues, lag shim (the only file that includes enet.h)
  src/session.cpp    state machine, handshake, join/re-join, pause, save guard, status
  src/puppet.cpp     M3-M4    src/destruction.cpp  M6    src/mirror.cpp  M7a-M8    src/missions.cpp  M9
  third_party/enet/  ENet 1.3.18
```

- **Proxy slot:** `xinput1_3.dll`. rfg.exe imports only ordinals 2 and 3, and the DLL is not a KnownDLL. It forwards to
  `%SystemRoot%\SysWOW64\XInput1_3.dll` by full path, loaded on first call. Fallback slot: `D3DCOMPILER_43.dll`.
  Uninstall = delete one file.
- **Hard coexistence rules** [confirmed]:
  - never touch 0x80F7C1 or `State_info[GS_GAMEPLAY].process`;
  - never set `Multiplayer`;
  - chain, never overwrite, the XInputGetState IAT slot 0xE3F728, the gs_load_process exit calls
    0x51305F / 0x51308D / 0x5130FA and the State_info exit slot.
- **Hotkeys:** F8 session on/off, F7 teleport to the partner, F6 dev ghost. Read edge-triggered on the sim thread (async
  state plus an auto-repeat gate).
- **Dev setup on one PC:**
  - instance A = `<GOG Galaxy folder>\…` (host), B = `<game copy B>\…` (guest). The **user** copies the mod DLLs into B; Claude never
    writes to game folders.
  - `dev=1` turns on:
    - the 0x4DDB5F patch for the whole run (without dev=1 it is on only while a session runs);
    - `s_allowBackgroundInput` (0x12EF120) = 0, so the gamepad drives only the focused window;
    - `SetWindowPos` placement and a title tag.
  - In B only, the save-name patch `keen→kee2`, applied in DllMain before any save read: both `n` bytes, at 0xF44209
    (file name) and 0xF4422F (the name `getSaveDataInfoFromFileName` compares). Changing only the extension does not
    isolate saves (§9). Seed B once by copying the save to `kee2_savegame_0_0.sav`.
  - Cap both at 60 fps, set both exes to the High-performance GPU, and run nothing heavy at the same time: this PC crashed
    once under load.
  - One log per folder, `[H|G] qpc_ms` lines, merged with `sort`.

---

## 5. Roadmap (each milestone is a build the user tests in game)

LOC = rough new-code estimate. Each milestone ships behind ini toggles, so the previous acceptance test still passes.

| ID | Name | Goal | Acceptance test (in game) | Retires |
|---|---|---|---|---|
| **M0** (~250) | Loader | `xinput1_3.dll` loads beside any other proxy-DLL mods, checks the build stamp and the expected bytes at every planned site, installs only the 0x1D24D9 tick, logs the longest gap between ticks (E9) and the `Server` byte 0x2BEA590 (expected 0), shows "RfgCoop idle" in the title | Any other proxy-DLL mods installed, load a save, play 5 min with the pad; they keep working; the log has "build OK", "xinput forwarded", `Server = 0` and the longest tick gap during a fast travel, a save, a load and a story video; delete the DLL → the game is unchanged | loader slot, E9 |
| **M1** (~80) | Two windows (dev) | two live instances on one PC | A and B both in gameplay; clicking between them leaves the unfocused one running (NPCs walk, clock ticks); the pad drives only the focused window; a save in B creates `kee2_savegame_0_0.sav` and leaves `keen_savegame_0_0.sav`'s modified time unchanged; 15 min, no crash | E1 |
| **M2** (~300 + ENet) | Link | ENet thread, handshake, RTT, lag shim | Loopback: connect, RTT in the title, "Partner connected" HUD line; a wrong password, a different DLL or different data is refused with a reason; the link survives a fast travel, a save load, the Esc menu and alt-tab for 10 min; reconnect works | ENet with llvm-mingw, timeouts, firewall prompt |
| **M3** (~460) | Ghost | single instance: a puppet of yourself follows 300 ms behind, through the same code path as networked play (puppet recipe, `no_save`, never_die, 0x6E62D7 / 0x6E6B36 / 0x6E59E7 chains, **0x6E6B44 stats skip**, `human_damage` puppet rules, L5, cache handles in every state, MP tables primed with real bucket arrays, clock, the 0x653124 air-control cave, the crouch and melee camera fixes 0x630B07 / 0x680783 / 0x67ED30) | The ghost mirrors walk, sprint, jump, crouch, ladders, every weapon class (one shot per shot, visible tracers, no damage), exactly one hammer swing per swing, grenade and charge throws; its jumps follow its own input while you stand still; your camera framing never changes when only the ghost crouches; its melee lunge goes where it faces, not where you look; hides while you drive (never tries to enter your car); never picks up salvage; your backpack stays on you; EDF can shoot it and it never dies, no death screen; save → reload leaves exactly one player; an activity starts and finishes with the ghost on; 15 min, no crash; a 10-min log of camera/HUD writes inside the ghost's processing window is empty | **E2: the biggest risk** |
| **M4** (~250) | See each other | PSTATE exchange in parallel worlds (both load the same save); F7 teleport; map blip (`cartographer::add_icon`) and name tag (`hud_icon_add`); the puppet hides when its zone is not streamed locally | Both run, jump and hammer around Tharsis seeing each other (lag judged < 0.5 s); the partner shows on the minimap; F7 works across 2 km; a driving partner shows only as a map blip; 20 min, no crash. **First fun**: exploring, races on foot | - |
| **M5** (~430) | Join the host's world | blob join and re-join (§4.8), save guard at 0x811278, activity suspension, **guest fences** (0x39D69C NOP, `mission_activate` backstop, 0x3EE1FB script skip, 0x3EDF23 chain clearing `Activity_node_start_on_load`, `player_start_mission` refusal, no in-RAM saves 0x51D040, 0x51C320 detour → REJOIN_REQ, UI blocks) | The guest starts from a *different* save and within ~30 s stands next to the host with the same liberated districts, destroyed buildings and time of day; host fast travel → the guest follows; host dies → both continue; guest dies → guest re-joins; the host fails Guns of Tharsis or EBS **past its checkpoint** and picks continue → the guest re-joins with `Mission_active_index == -1`, no objectives, no script spawns, no video, no reload for 10 min; the host replays an activity → nothing starts on the guest; the guest's Esc Load / Restart / fast travel show the "host only" message and change nothing (Esc Load and fast travel: the line shows once the Esc menu / map is closed, HUD lines need gameplay; the log has `guest: HUD` at the click); afterwards the guest's `.sav` modified time is unchanged and its own campaign loads intact | E3, E8, E10 |
| **M6** (~1,060, ver7.0-7.3) | Shared Geo-Mod | Plan of record: **notes/m6_plan.md** (2026-10-08): one-game E4 runner first (ver7.0), then the live destruction stream with a resync layer (7.1), re-join with holes and GM_DEAD (7.2), chunk pose lane (7.3); modes chosen by the host's ini (`destr_sync` 0/1/2), protocol 7, Q19 world bit bound to the mode, every site with its status | Cards C0-C3 (and fallback F0) in notes/m6_plan.md §5, one PC, run at the end; the final two-PC run repeats C1. **Second fun**: co-op Geo-Mod sandbox | E4 (K3a/b/c) |
| **M7a** (~650) | Shared enemies: the mirror | guest population off at its drivers (0x3EDF23 whitelist; convoy, ambush, VIP, mission-system and `activity_system_do_frame(1)` skips) with `spawn_npc` / 0x757BF7 refusals and the sweeper (NPCs, vehicles, turrets) as backstops, NPC mirror (spawn, despawn, die, 10 Hz states), turret binding by node handle, non-persistent copies, AI-off copies with cosmetic fire, guest firewall (copies invulnerable), the guest's puppet kills NPCs for real on the host, WORLD (alert, time), DISTRICT (raw control write) | Fight an EDF patrol together: same soldiers in the same places; a soldier killed by either player falls on both screens within 0.5 s; alert matches; 20 min roam including a convoy, a night raid and a manned base turret: no NPC, vehicle or turret on the guest that the host lacks for more than 2 s (log count, plus a caller log of every refused spawn), guest fps ≥ 45, no crash | E5 |
| **M7b** (~400) | Shared enemies: real danger | DAMAGE to the guest, never_die + downed/revive (guest; host if Q3 = yes), out-of-world teleport, leash, AI-notice check with the gate patches behind a toggle | The guest takes damage from host soldiers on foot and in a car, gets downed, is revived by the partner or after 10 s with its loadout, no reload, no death screen; a team wipe in a mission fails it on both and both re-join; jumping into a kill volume teleports to the partner; EDF engage the guest even when already fighting the host | E6, AI noticing |
| **M8** (~600) | Shared rides | vehicle mirror, host seat arbitration, driver-owned movement (`set_vcontrols` + easing or lane L2), gunner turret aim with the puppet turret-camera fixes (0x6C0A05, 0x6B5F89), `vehicle_apply_damage` firewall (also against local kill regions and pilot-death replays), owner-decided wrecks; walkers via `walker_steer`; aircraft state-only | The host drives with the guest riding shotgun, then the guest takes the turret and shoots; the guest drives the host 5 min across a district and the car rests within 2 m of the same spot on both screens; a ram into a building replicates the destruction; a blown-up car is a wreck on both; nobody is ever ejected by a hijack | E7 |
| **M9** (~650) | Missions together | MISSION START (warp, restore-list regrow on fresh starts, host model set pre-streamed with `srm_reserve_slot(SPAWN_SLOT_MISSION)`), RESTORE for activity starts and single restores, objective mirror with icons and markers by position and a snapshot after each re-join, roadblock barricade mirror, host videos as a pause on the guest, fail/retry → re-join; same-team model fallback only as a logged safety net; co-op starts after the tutorial (the fence itself is in M5) | 3 story missions (e.g. the first ones after the tutorial, incl. "Death From Above" with its 10 restored buildings), **one checkpoint mission (Guns of Tharsis or EBS) failed past its checkpoint and continued**, and 2 activities (a raid, a house arrest) with the guest fighting along; the guest sees every objective, icon and ground marker; mission NPCs show their real models (fallback count logged); a deliberate fail reloads both into the retry; the host's save shows progress, the guest's save is untouched. **v1 done** | missions probes |
| **M10+** | Widening (one build each) | in order: (1) either player completes steps (callback swaps from 0x12B4648); (2) wider leash (L6, spawn/despawn/AI distances); (3) guest-side hit detection (T1/T2); (4) [dropped: the user decided the guest always uses the host's loadout, §8 Q13]; (5) concurrent reloads (anchor-blob forwarding); (6) videos on both machines; (7) DotB characters as puppets; (8) client-local civilians; (9) NAT punch or relay; (10) Steam address table; (11) 3–4 players | written when scheduled | - |

Order: M0 → M1 → M2 → M3 → M4 → M5 → M6 → M7a → M7b → M8 → M9. M3 is single-instance and can be built in parallel with
M2. From M4 on, test each milestone at least once between two real PCs over a virtual LAN.

### Backpack effects and animations (added 2026-10-08, after the ver6.2 test)

Plan: `notes/backpack_sync.md` (raw reports `findings/workflows/bp_*.md`). The puppet wears the partner's pack since
ver6.2, but nothing activates it. Fix, as an MP client does: PSTATE protocol 6 (activation counter, level, Thrust bits,
fuel) → `activate()` 0x5F2030 / `deactivate()` 0x59AA30 on the puppet's pack in `Effects`. Phases: **P1 ver6.3**
(transport + Jetpack fuel, Fleetfoot, Stealth, Vision, Firepower, Heal, Tremor), **P2 ver6.4** (Thrust, Shockwave,
Rhino with the ram and ripple gates), then role policies inside M6 (pack destruction), M7a (pack hits on NPCs, stealth
vs AI), M7b (Heal revive) and M8 (ram on vehicles). Open user questions Q1-Q7 are in that note's §8. Gap round
(`bp_gap_1..4.md`, note §9): the visibility gate and sound residency are closed, pack blasts cannot floor a player in
SP (G8 optional), pack-mesh animations are confirmed never played; no new transport, gate or asset loading.

---

## 6. First experiments (cheap, single builds, biggest unknowns first)

Each is a dev-only ini flag or hotkey with a clear pass/fail, logged to `RfgCoop.log`.

| # | Experiment | Pass if | Decides |
|---|---|---|---|
| **E1** | Two live instances (A from `<GOG Galaxy folder>`, B from `<game copy B>`) with the dev patches: quick pause off, background input off, `kee2` save name in B | both in gameplay at once, the unfocused one simulates (log `Framecount` 0x18E7710 at 1 Hz), separate save files | dev loop on one PC, else two PCs only |
| **E2** | Ghost puppet: create a `player` with the WC/MP recipe using `Player->h_info`, `reset` its sim state, feed the local state 300 ms late through `add_state` + `process_state`, with the 0x6E62D7 / 0x6E6B36 / 0x6E59E7 patches, **the 0x6E6B44 stats skip** (behind a toggle: off must AV at rfg+0x5862E6 reading 0x610), L5 open, cache handles in every state and tables primed with real bucket arrays. Log `World+0x3FC`, the puppet's list index, `Player`, the puppet's `mp_data` (+0x126C, expect -1), and every `Rfg_camera` write / `camera_*` / `hud_*` call inside the puppet's processing window | animates correctly, no crash, `Player` unchanged, list index ≥ 1, no stolen pickups or backpack, clean save/reload, no camera or HUD write from the puppet window after the M3 fixes | the whole puppet approach (K1) |
| **E3** | Blob round trip: hotkey 1 = silent blob; demolish something and walk 500 m; hotkey 2 = load the blob from memory. Arm the 0x811278 guard and die once | the world returns to the captured state with the player at the captured spot; the save file's modified time is unchanged; the save UI does not hang | join and re-join (K2) |
| **E10** | Guest-fence self-test (one instance, "guest" flag on): log on every load `World+0x2578`, the value `mission_system_load` read and the count of managed `Action_scripts`. Play Guns of Tharsis to its checkpoint, die, continue, capture `stored_world_state[World+0x2578]`, then load it from memory with the fences on, and with each one off in turn; repeat with an activity-replay buffer | fences on: `Mission_active_index == -1`, `Mission_mode == 0`, no objectives, no script spawns, no video, no reload for 10 min, nothing force-starts; with only the mission NOP: scripts act; with only the script skip: the mission activates | the M5 fence set |
| **E8** | Handle diff: both instances load the same save; each dumps (handle, classname hash, type, pos) of every object within 300 m; diff the two logs | static objects identical; runtime 0xFF handles listed | the identity rule |
| **E9** | Longest gap between 0x1D24D9 calls over a territory load, a save, a fast travel and a story video (in M0) | a number | ENet timeouts and the LOADING protocol |
| **E4** | Destruction replay (redesigned in the gap round): record each step's 0xEC event prefixes, `rfgl_desired_values` results, chunk uid delta, Vlib index and `_holdrand` while hammering and charging a building; load the E3 blob; inject the recording while dropping local events. Matrix: plain SP / MP profile / profile + host rfgl values / profile + rfgl + private RNG; each at 30 and 60 fps and once with the camera 150 m away facing away; 5 runs per cell. Measure a static-remainder hash (subpiece alive bits of every touched fixed mover), uid deltas per step, chunks alive at +10 s, rest-pose error of the 5 largest chunks (with and without a pose-lane snap). Also log the thread id at 0x4102E0 and of `vfx_effect::update_render` | K3a: static hash equal in ≥ 4 of 5 runs of every cell (predicted). K3b: uid deltas equal from the "profile + rfgl" cell on. K3c: rest poses within 1 m only with the pose lane (predicted: never without it) | shared Geo-Mod (K3a/b/c) |
| **E6** | Downed/revive: never_die on `Player`; take lethal hits in free roam, in a mission and in an activity; revive by HP write + `human_ragdoll_get_up` | standing, HP full, weapons kept, camera and HUD fine, no death screen, no mission failure, no autosave | death without a reload |
| **E5** | NPC copy: spawn an EDF soldier through `object_spawn_do_spawn`, set the yield bit every frame, walk it along a recorded path with `human_move_direct_travel_to`, toggle cosmetic firing, kill it by event; plus the 0x3EDF23 whitelist and the single-call skips, with the `spawn_npc` / 0x757BF7 refusals and the sweeper as backstops, for 15 min of play at night, in a red-alert district, on a convoy route and past an ambush region. Log every call that still reaches `spawn_npc` / `sp_spawn_vehicle` / `spawn_squad` with its caller, and the turret count | follows the path without sliding or sticking, releases the trigger, dies cleanly; after the load population, no driver reaches the refusals and every logged caller is a load-time, stream-notify, VIP, safehouse or player-squad one; fps up versus vanilla | shared enemies (K4) |
| **E7** | Puppet driver: seat a puppet in seat 0 with the MP enter recipe, set `+0xAC4`, call `automobile_set_vcontrols(accel 0.6, steer 0.3)` each frame; then repeat with lane L2 and a zeroed 0x1E1C container at `car+0x4E88` replaying a recorded drive | the car drives the circle and `vehicle_ai_do_frame` never runs for it (log); L2 tracks the recorded drive smoothly or not | vehicle approach for M8 |

Order: **E1 → E2 → E3 → E8** (E9 comes free with M0) **→ E10 → E4 → E6 → E5 → E7**. E2 and E3 alone decide whether
the project continues. The smaller gap-round probes (restore-list parity, model census, turret pairing, liberation with
the district repoint, guest UI paths, highlight census) are listed in `notes/gaps.md` and run inside the milestone that
needs them.

---

## 7. Risks and kill criteria

### Top risks
1. **Second player object in the campaign.** The code path has never run (non-local `player_do_frame`, squads, minimap,
   cover, stats, audio). The gap round's static audit found one certain crash (fixed at 0x6E6B44) and several camera and
   input leaks (fixed in M3), but it followed direct calls only. → E2 first with those fixes and a camera/HUD write log;
   each new finding becomes a one-site chain.
2. **Destruction replay drift.** The SP debris load balancer (camera, visibility, measured Havok time), immediate slices
   and uber explosions (frame count, live velocity), the unsorted shardify list, the out-of-queue deleter and per-machine
   frame-time physics make chunk sets and poses diverge; the shared random pool is also drawn by the render thread. →
   MP determinism profile, host balancer values, uid pairing, pose lane, optional private RNG; E4 matrix; drift counter;
   re-join resync, or drop live sync.
3. **NPC copies failing on the guest.** Model streaming (17 stream records, 24 mission slots), the NPC pool of 100 with
   ambient spawning stopping at 90, the cost of 60–100 copies. → E5, logging, the host's model set pre-streamed with
   `srm_reserve_slot`, same-team model fallback as a safety net. Turrets and roadblock barricades need their own mirror
   class.
4. **Reload storms.** Every host fail, fast travel and warping video costs both players a load. → never_die for players,
   anchor-blob forwarding next.
5. **Save safety.** Whole-image rewrites, the guest's death autosave, a puppet saved into a save. → guard until exit,
   `no_save` puppets, backups, the hard rule.
6. **One streaming centre.** The world exists only around the host's camera, and the radius shrinks under memory
   pressure. → leash with a live radius read; v2 patches.
7. **Felt lag.** The guest's shots are re-simulated on the host (RTT/2 + 100 ms behind NPCs that are themselves
   interpolated), and spread is rolled again on the host. → acceptable for friends at ≤ 100 ms RTT [inferred]; T1/T2 hit
   taps in M10+.
8. **never_die side effects.** Kill volumes stop killing; scripted "player must die" moments may hang. → out-of-world
   teleport; drop never_die for any such moment found in M9.
9. **Missions bound to the host's `Player`.** The guest cannot advance trigger steps. → the host leads in v1; callback
   swaps in M10+.
10. **Machine load and connectivity.** Two RFGR instances on a PC that crashed once; CGNAT. → 60 fps caps, two PCs
    from M4, virtual LAN as the default.
11. **Host logic leaking into the guest through the blob** (gap round). A checkpoint blob carries a running mission and
    its scripts; an activity-replay blob an activity to force-start. Unfenced, the guest runs the host's mission
    locally, with its own fail screen and reload. → the M5 guest fences, verified by E10.
12. **Campaign consequences on the guest** (gap round). Replayed destruction and kills run district control, liberation
    (Dust also warps), morale, stats and activity hooks on the guest; `district::set_control` runs the liberation tail. →
    operand repoints of the engine's own MP early-outs, raw district writes, never `set_control` or `gfm_*` on the guest.
13. **Skipping all guest scripts** also drops ambient territory scripts (PA lines, tool tips, free-fire zones); assumed
    cosmetic on a host-authoritative guest, unproven. Script handlers that `gem_post_event` calls synchronously are not
    covered by the skip (unread). → a 15-min roam with the skip on and a `gem_post_event` logger.
14. **Restore-list and regrow timing** (gap round). `world::restore_object` only queues; the zone is re-serialized when
    it is loaded, so host and guest can regrow at different moments, and a list registered after the guest's last load
    is missing there. → hold destruction events on listed handles until the regrow is done; fall back to sending the
    handle list.
15. **Guest UI states.** Refused loads can leave menus or the death screen hanging, or retry every frame. → block the
    UI entries before their side effects; clear the pending-load bit on refusal; probe each path.

### Kill criteria (the user decides from test results)

| # | Trigger | Consequence |
|---|---|---|
| K0 | E1 fails **and** there is no second PC or partner for regular testing | pause the project: every milestone needs two games |
| K1 | E2 cannot produce a stable, animated puppet `player` after 3 fix rounds | try the NPC puppet (yield bit + `human_move_direct_travel_to`); if that also fails, **stop**: there is no way to show the partner |
| K2 | E3's memory load fails, no disk-copy variant works, **and** E8 shows different static handles on identical saves | stop at M4 ("see each other" in parallel worlds) |
| K3a | E4's static-remainder hash differs in ≥ 2 of 5 runs even with the MP profile, or the replay crashes | drop live destruction sync; M6 becomes "destruction resyncs at re-join"; continue with M7a |
| K3b | static geometry matches but chunk uid deltas still differ with the profile and the host's balancer values | keep live holes; drop chunk identity (chunks stay local and cosmetic, host events on chunks are skipped) |
| K3c | chunk sets match but the pose lane cannot keep large fallen sections within ~1 m without visible popping | keep the lane for the ~10 largest chunks only, or accept settled-pose drift until the next re-join |
| K4 | in M7a, more than ~20% of the host's combat NPCs never appear on the guest, or the guest's frame rate halves | stop at M6 (co-op sandbox) unless a cheap fix shows up |
| K5 | after one tuning round over a real link (RTT ≤ 120 ms) the user judges combat or driving unplayable | try the T1/T2 hit taps once; otherwise freeze at the last fun milestone |
| K6 | a milestone takes more than 3× its estimated builds without passing | stop and re-plan before writing more code |
| Hard rule | any test damages a save (host or guest) | all feature work stops until it is fixed |

---

## 8. Decisions for the user

| # | Question | Recommended default |
|---|---|---|
| Q1 | Accept the v1 rules: the host starts missions and does the objective steps, the guest fights; players stay within ~200 m; only the host's progress is saved; join in free roam only? | Yes. Each one is lifted later by a M10+ item. |
| Q2 | Guest loadout in v1: borrow the host's (zero code) or splice the guest's own inventory and meta sections into the blob (unverified section rewriting on the riskiest path)? | **Decided by the user (2026-10-07): the host's, always (see Q13).** The guest never keeps its own loadout; M10+ item 4 is dropped. |
| Q3 | Host death: vanilla (mission fail or death screen, then both reload) or the same downed/revive as the guest (vanilla only on a team wipe)? | Downed/revive for both from M7b, with an ini toggle. Fewer reloads; the cost is the same code the guest needs anyway. |
| Q4 | May the user copy the mod DLLs and `RfgCoop.ini` into `<game copy B>` as dev instance B? (Claude never writes to game folders.) | Yes, from M1. Back up `keen_savegame_0_0.sav` before every test session. |
| Q5 | Connectivity for v1: direct IP + port forward or a virtual LAN only, no relay server? | Yes. Tailscale or ZeroTier are the easy default. |
| Q8 | GOG build only for v1? | Yes. Steam owners can use SyncFaction's GOG exe; a Steam address table is M10+. |
| Q9 | When does a second PC or a friend join testing? | From M4: each milestone from M4 on is tested once over a real link. |
| Q10 | During a session, run the host with the MP destruction profile: no slicing of moving chunks, no random uber-heavy flourishes, no view-based debris reduction? | Yes from M6, with an ini toggle. Shared chunks are impossible without it; the cost is a little SP flavour and more debris kept for far collapses. |
| Q11 | On the guest, skip every script, including ambient territory scripts (PA lines, tool tips)? | Yes in v1. Finer group-level termination only if the user misses them. |
| Q12 | When is co-op available? | **Decided by the user (2026-10-07): only when the host's loaded save has completed the Tutorial mission.** Check: the `All_mission_infos` entry (0x1E45C18: count +0xC, data +0x4, stride 0x60) with `mis_mode` (+0x14) == 4 (TUTORIAL), completed per `mission_is_complete(index)` 0x374B20 (cdecl; flags +0x58 bit 0, restored from the save). Host: F8 refused with a HUD reason; a host load of a save without it ends the session. The guest's own save does not matter (from M5 it plays in the host's world). |
| Q13 | What upgrades and weapons does the guest have? | **Decided by the user (2026-10-07): only what the host acquired in that save.** Lockers on the guest show the host's unlocked arsenal; every upgrade the host bought applies to the guest (armor included). Mechanism: the join blob carries the host's meta data (`Player+0x2978`: `upgrade_item[128]` at +0x18) and arsenal (`Weapon_list` 0x1260ED0, refilled by `add_unlocked_weapons_to_weapons_cabinet` in `load_meta_data`); from M5 the host also pushes its upgrade table and arsenal when they change during a session (UPGRADES, reliable), and the guest re-applies them (which upgrade effects need an explicit re-apply is open: notes/world_save.md probes). **Also decided (2026-10-07): the guest cannot access the upgrade shop, and every bit of salvage the guest collects goes to the host** (one shared wallet, the host's). Mechanism (M5): block the guest's shop entry with a HUD reason; on the guest, catch the local player's salvage gains (`player_meta_data` +0x4; `add_salvage` 0x611700 caps at 30000) and send SALVAGE(amount) to the host, which adds it to its own `Player` meta; the host pushes its salvage total with UPGRADES so the guest's HUD shows the shared wallet. Design point for M6: replayed destruction spawns salvage drops on both machines, so the same building could pay twice; decide then (host-owned salvage pickups, or guest-side drops suppressed). |
| Q14 | Can the guest start missions? | **Decided by the user (2026-10-07): the guest cannot start any type of mission (story missions, activities / guerrilla actions); the one exception is Convoys, which are not started by the host.** Mechanism: the guest has no mission system (M5 fences: `player_start_mission` 0x633F40 refused with a HUD reason, `mission_activate` backstops, activity starts blocked); start nodes on the host react only to the host's own `Player`. Convoys are activity type 13 (`activity_convoy_data`), started when `convoy_was_attacked` 0x36B5A0 sees a convoy-squad npc with `ai_perception::has_player_enemy_belief` 0x283860, which seeks a belief about ANY `player` object (`belief_obj_iterator<player>::seek_obj`), so the guest's puppet attacking a convoy in the host's world should start the convoy on the host by itself [read in code this pass; runtime unproven]. To check in M7a/M9: that it does, that no other activity can start from the guest's puppet alone, and that convoy objectives and rewards mirror to the guest (rewards go to the host, Q13). |
| Q15 | Can the guest fast travel? | **Decided by the user (2026-10-07): no.** On the guest, fast travel is refused with a HUD reason before any side effect: the safehouse warp callback `fsm_warp_to_safehouse_cb` 0x454484 (-> `save_and_warp` -> `save_and_reload`, notes/gaps.md §10) is blocked at the UI, and the 0x51C320 load detour stays the backstop (M5 guest fences, GF-UI). The host may fast travel: the guest re-joins at the new place (§4.8 re-join). The guest's F7 teleport goes only to the host and stays. |
| Q16 | Briefings and activity setups when the host starts a mission or activity | **Decided by the user (2026-10-07):** (a) while the host is on a briefing, the guest gets a notice (name + briefing text from its own tables, sent by index) and keeps playing; it pauses only through the normal host-pause rule. Static facts: the mission briefing is `gameseq_push_state(38, transparent 1, pause_beneath 0)` at 0x458C88 (from `hud_ui_mission_briefing_start` 0x458A00; its enter calls only `ui_set_max_frametime`), and activity briefings go through `hud_ui_activity_briefing_start` 0x445A30 (sets `Need_to_push_state`); whether the host's world keeps running under them is to be confirmed in game (missions.md probe P1). (b) Demolitions Master: the guest gets the same fixed activity loadout as the host for the duration, and the host's normal loadout back afterwards (the host's end path is `activity_demolitions_master::warp_player_on_end` 0x397AE0 -> `player_respawn` -> `player_reequip`). (c) Transporter: only the host can take the driver's seat of the delivery vehicle (host seat arbitration); the guest rides as passenger/gunner or escorts. (d) On accept the guest is always teleported next to the host (after the host's reload when the start involves one). |
| Q17 | Guest hammer and backpack | **Decided by the user (2026-10-08): the guest picks ANY hammer and ANY backpack in the game (not limited to any save's unlocks; an exception to Q13 for these two items) in an RfgCoop picker shown when the guest presses F8, before it connects (so the host is not held paused). The choice is applied after every join and re-join and kept for the session; changing it = leave and join again.** Replaces the "hammers/backpacks/jetpack arrive at the next re-join" gap of ver6. |
| Q18 | Does the guest keep its weapon loadout across re-joins? | **Requested by the user (2026-10-08, after the ver6 test): yes.** The weapons the guest picked at lockers (which show the host's unlocked arsenal, Q13) must survive every re-join (host fast travel, death, retry, load, warping video, and the guest's own death -> re-join); today each re-join loads the host's blob, whose player section carries the host's inventory. Plan (ver6.1): the guest records its own inventory per slot (weapon, ammo) and the equipped slot while in gameplay, and restores it after each re-join at the same point the Q17 hammer/backpack choice is applied; the first join still starts from the host's loadout. |
| Q19 | Partner backpack effects (plan: notes/backpack_sync.md) | **Decided by the user (2026-10-08):** the partner's jetpack already showed flames and lift in ver6.2 (yes); the partner's **Heal heals you** when you stand within 5.8 m (yes); **until M6 the partner's pack quakes, hammer blasts, Shockwave blast and Rhino/Thrust holes break your copy of the world**, as their guns and charges already do (yes). Defaults kept for the later ones: no G8 (a red hit marker may show), Firepower x1.25 on shared enemies from M7a, stealth vs your enemies from M7a, Heal revives at M7b. |

---

## 9. Contested and refuted claims (do not rely on these)

| Claim | Status | What is true |
|---|---|---|
| "Insiders say SP and MP are separate code" | [contested] | One anonymous Steam user (2018), identity unconfirmed. The binary shows one shared codebase that branches on `Multiplayer` in 1,058 functions. Same conclusion (keep the flag at 0), different reason. |
| "Engine MP destruction sync works both ways / has validation / is rate-limited" | [contested: refuted] | One-way, server to clients. The validation functions are `ret` stubs. `mp_destruction_sync_throttle` caps synced debris movers, not packets. |
| "The MP transport can't be brought up without MP mode" | [partially] | Stock `multi_init` forces MP mode, but its sub-inits don't check the flag. Irrelevant: RfgCoop uses ENet. |
| "GOG P2P runs over Galaxy" | [contested] | sw_api.dll is THQ Nordic's SteamWrapper over RakNet with NAT punch and relay; Galaxy is used for auth. Which backend runs is set by an encrypted config. Unverified either way. |
| "GameNetworkingSockets needs OpenSSL and a signaling server" | [contested: refuted] | bcrypt works on Windows; direct IP needs no signaling. The real objections are protobuf/Abseil and the MinGW build. |
| "A net thread is needed because the sim thread stops during pause" | [contested: refuted] | `rfg_do_frame` runs every frame in every state; only the GS_GAMEPLAY process hook stops. A net thread is still the right design (hitches, ENet not thread-safe). |
| "Engine vehicle replay crashes in SP (NULL `+0x4E88`)" | [contested: refuted] | The reads are NULL-guarded; the path is just never taken because the registrations are orphaned. `apply_sim_state` is callable (lane L2). |
| "Wrecking Crew simulates several players at once" | [contested: refuted] | Only the active player is processed; the others are hidden. N players *exist* without MP; N *simulated* players in the campaign are unproven (E2). |
| "`process_state` is self-contained in SP" | [partially] | Four traps (§3.1): divide-by-zero handle lookup, counters that don't converge, no reset, weapon switch outside it. |
| "The puppet swings its hammer in the observer's look direction" | [partially] | Only for a melee started while exiting cover (flag set at 0x6B192E, used at 0x6B1A82–0x6B1AE4). Normal swings are fine. |
| "`human_controller_set_vel` is continuous velocity correction"; "reload is a rolling counter" | [contested: refuted] | `set_vel` runs only in the snap path. Reload is state bit 7, latched in `+0xDD0`. |
| "The puppet must write `aim_pos` itself" | [contested: refuted] | `apply_movement_state` already writes it (0x5CF393 / 0x5CF3AD). |
| "An NPC's AI can't be switched off without stopping its body" | [contested: refuted] | The yield bit `ai+0x451 & 2` does it, with the caveats in §3.5. |
| "Every NPC source except convoys goes through `gameplay_do_frame` (0x3EDF23)" | [contested: refuted; refined in the gap round] | `gameplay_do_frame` holds almost every *periodic* driver, but convoys, ambushes, VIPs, the mission system, scripts and `activity_system_do_frame(1)` are separate single calls, and load-time and stream-notify spawns exist. All runtime NPC and vehicle creation still ends in `spawn_npc` / `create_new_vehicle`; turrets and barricades do not. Hence driver skips first, refusals and the sweeper as backstops. |
| "One global RNG" | [partially] | Two: the `Vlib_rand_instance` pool and the CRT `rand()`, both seeded from the microsecond timer. A Vlib draw only reads a boot-filled 2048-entry pool by one non-atomic global index, which the render thread also draws from; CRT `rand()` is per thread. Lockstep is still out. |
| "The guest never runs missions, so the mission section in the blob does not matter" | [contested: refuted, gap round] | A checkpoint blob makes the guest start the mission (`setup_territory` 0x540AB0), and its script section restores the mission's managed scripts, which run whenever no mission is active. Fence: §3.9. |
| "Retry from start restarts the mission" | [contested: refuted, gap round] | The anchor reload carries mission -1; nothing re-activates it. Only a checkpoint "continue" does (8 missions; DFA has none). |
| "The guest can call `srm_mission_reservation` so the mirror streams mission models" | [contested: refuted, gap round] | It wipes ambient reservations, picks variants at random and sets `Mission_mode`, after which every reservation fails. Use `srm_reserve_slot(SPAWN_SLOT_MISSION)` with the host's set. |
| "Mission layers contain mission buildings or vehicles" | [contested: refuted, gap round] | 59 mission layers, 2,786 logic objects; the only visible ones are 10 effects and 4 items. Buildings are base-zone objects. |
| "`activity_suspend_activities` stops Marauder raids" | [contested: refuted, gap round] | Raids are not activities; their chain in `gameplay_do_frame` reads only alert, the active mission and the `Raid_*` handles. |
| "District liberation always reloads the game" | [partially, gap round] | Only Dust: the liberation screen warps to its safehouse after it is dismissed. Liberation can also come from `mission_completed` directly. |
| "The 0x51C320 detour is a complete, side-effect-free load backstop" | [partially, gap round] | Complete, but Esc → Load runs `platform_load` first and a refused warping video drops to the main menu. Block the UI entries earlier. |
| "RNG control makes Geo-Mod replay deterministic" | [contested: refuted, gap round] | The load balancer, immediate slices, uber explosions, list order and frame-time physics diverge per machine; the RNG is secondary. `rfg_dyn_create_shard`'s random calls are fade lifetimes, not shard counts. |
| "Shardify may run on worker threads" | [contested: refuted, gap round] | The threaded job functions are dead code; the apply runs on the sim thread. |
| "Chunk physics can never match" | [partially, gap round] | A fixed 1/60 s substep mode exists (`Havok_use_single_step` = 0), but different per-machine inputs still diverge poses: a pose lane is needed. |
| "Prime the MP handle tables with a NULL bucket" | [contested: refuted, gap round] | A NULL bucket array still crashes after the divide; point the buckets at a zeroed dword. |
| "39% of highlight targets are mission-layer objects" | [partially, gap round] | About 24% of icon and highlight handles are layer objects highlighted directly (42 more resolve to runtime spawns), but 72 of 79 objective icons and 40 of 40 ground markers are. |
| "Byte 2 == 0xFF means a runtime object" | [partially] | Saves restore 0xFF handles too. Never decide identity by that byte alone. |
| "AI only newly notices the local player once engaged" (ai notes) vs "a hostile puppet is noticed every call" (puppet notes) | [contested; resolved this pass in favour of the puppet notes] | Checked this pass in `ai_perception::notice_humans`: disposition 0 (HOSTILE) jumps at 0x2C2ACE straight to `notice_enemy` (0x2BD840) with no local-player gate. The gates at 0x2C2ADD / 0x2C2B0D hold back only ALLIED, FRIENDLY and UNFRIENDLY targets. But `get_effective_disposition` (0x265A40) lowers HOSTILE to UNFRIENDLY for a player target whose `+0xC0` bit 0x40 is set and `+0xAC4 != -1` (0x265AB4–0x265AE6, a player in a vehicle). An engaged NPC may then ignore a guest in a car. Tested in M7b with the gate patch ready. |
| "Death always autosaves to disk" | [partially] | Only in free roam with cheats off. In a mission: `mission_terminate(0)`; in some activities: their own handler. `Reload_level_on_death` is never written (dead branch). |
| "`player_respawn` is the SP respawn" | [partially] | The 4-argument version 0x6DF5F0 works in SP but wipes the inventory through `player_reequip`; the int overload 0x6DFAB0 does nothing in SP. Hence revive by HP + `human_ragdoll_get_up`. |
| "`game_save_write` is the only way a save reaches disk" | [contested: refuted] | Import, delete, options and profile writes bypass it. The real choke point is the `requestSave` call at 0x811278. |
| "Patch the `.sav` extension (`.sa2`) to isolate dev saves" | [contested: refuted] | Enumeration parses every file name and ignores the extension. Patch both `n` bytes (`keen→kee2`, 0xF44209 and 0xF4422F). |
| "The save file can be edited on disk while the game runs" | [contested: refuted] | The game reads the whole file into RAM once and rewrites it from RAM; an outside edit is lost and can be cloud-synced. Transfer in RAM. |
| "No runtime apply API for blob systems" | [contested: refuted] | Setters exist for teams, time, fog of war and alert; district control is a raw write of `district+0x9C` (`district::set_control` is **not** a live setter: it runs the liberation tail, gap round); zone stored state applies on stream-in. Only scripts, progression, liberation flags and handbook need a blob reload. |
| "A silent blob is always safe" | [partially] | Only in free roam: it resets `cur_world_state` (mission restart/cancel change), returns an old buffer while `Stream_pause` is set, and does nothing while a save runs. |
| "Stream radii are fixed at 340/510 m" | [contested: refuted] | They shrink at runtime under stream-memory pressure to 170/336.6 or 102/168.3 m, and are restored on a timer. The leash must read the live value. |
| "Frame time is capped at 1/120 s and clamped at 0.25 s" | [partially] | Those are the .data defaults; `frametime_set_cap(1/300, 0.1)` overwrites them at init. The minimum is a frame limiter (sleep/spin), the maximum a clamp. |
| "Only `SW_CCSys_Init` failing is fatal at boot"; "a second instance's sw_api bind fails" | [partially] | Two fatal checks (init and ownership); both pass on this GOG install. The logs show a second instance bound port 23023 next to a *hung* first one; two live instances are unproven (E1). |
| "Hijack only ejects unfriendly occupants" | [contested: refuted] | `mark_seat_reserved` 0x7A19B0 ejects every occupant for a player-type entrant, with no team check. The host must arbitrate seats. |
| "The MP bullet receiver re-evaluates the victim's damage" | [contested: refuted] | It uses the shooter's number. Victim-side modifiers must be recomputed on the owner. |
| "`do_bullet_hit` is `cdecl(human*, weapon_fire_info*)`" | [contested: refuted] | LTCG passes `weapon_fire_info` in ESI (checked this pass: `test [esi+0x50],0x20` at 0x7E5877). A detour or call built from the PDB crashes. |
| "Tap T1 is a clean send-site redirect" | [partially] | Opening its gate also reaches `multi_game_stats_record_hit` at 0x7E58AA, which indexes `Mp_player_stats` (NULL in SP) by `player+0x126C`. It must be neutralised too. |
| "`gem_post_event` sees every mission event" | [partially] | Mission end, cancel and death termination bypass it. Hook `mission_terminate_internal`, `mission_cancel`. |
| "Saints Row co-op: only the host progresses" | [contested: refuted] | SR 2022 tracks progress per player. Only "one player drives the mission, the partner is pulled along" is shared with Skyrim Together. |
| "The user's two installs are identical" | [partially] | `rfg.exe` and the `data\` listing (name, size, date) are identical (checked this pass). `sw_api.dll` differs (GOG GALAXY is community-patched, 2026-09-26). RfgCoop does not use sw_api. |
| "The engine MP compressors fail in SP" | [partially] | They run with the flag at 0 but write no portable handle. Not needed: SP applies raw floats, so raw fields replay exactly. |

---

## 10. Index of notes and raw reports

All under `<RFG Modding>\RfgCoop\research\`.

| Domain | Verified notes | Raw report |
|---|---|---|
| Prior art (other co-op mods, quotes) | `notes/prior_art.md` | `findings/workflows/coop_prior_art.md` |
| Transport (engine stack, sw_api, ENet) | `notes/transport.md` | `findings/workflows/coop_transport.md` |
| Engine MP replication, puppet pipeline | `notes/replication.md` | `findings/workflows/coop_replication.md` |
| Geo-Mod destruction | `notes/destruction.md` | `findings/workflows/coop_destruction.md` |
| Puppet player and animation | `notes/puppet.md` | `findings/workflows/coop_puppet.md` |
| AI, spawning, object identity | `notes/ai.md` | `findings/workflows/coop_ai.md` |
| Streaming, world blob, saves, join | `notes/world_save.md` | `findings/workflows/coop_world_save.md` |
| Missions, activities, videos, pause, UI | `notes/missions.md` | `findings/workflows/coop_missions.md` |
| Vehicles, combat, death | `notes/vehicles_combat.md` | `findings/workflows/coop_vehicles_combat.md` |
| Infrastructure, two-instance dev setup | `notes/infra.md` | `findings/workflows/coop_infra.md` |
| Community code reuse | `notes/community.md` | `findings/workflows/coop_community.md` |
| Gap round: mission fence, Geo-Mod determinism, puppet SP audit, guest fences, mission content | `notes/gaps.md` (all five, with a unified patch list) | `findings/workflows/coop_gap_1.md` (mission fence, retry, restore lists), `coop_gap_2.md` (RNG and determinism), `coop_gap_3.md` (puppet SP safety), `coop_gap_4.md` (guest creation, consequence, load and save paths), `coop_gap_5.md` (layers, SRM, turrets) |

Designs judged:
- `findings/workflows/coop_design_mvp_first.md`: the **base design** (two of three judges).
- `findings/workflows/coop_design_puppet.md`: the custom-sync-layer design (one judge). Source of the never_die,
  sweeper, guest firewall and anchor-blob grafts.
- `findings/workflows/coop_design_engine_mp.md`: the MP-lanes design. Source of lanes L2, L5, L6, the T1/T2 taps and MP
  table priming. Its 18-byte gate dumps are in §2 of that file.

Checked in this pass (the GOG `rfg.exe` disassembly cache and the symbol table):
- the never_die clamp at 0x6CEF85;
- the single `player_is_killed` call at 0x6E59E7 with its subtype checks;
- the `requestSave` call at 0x811278;
- the T1 gate 0x7E585D and the ESI use in `do_bullet_hit`;
- the `notice_humans` dispatch 0x2C2ACC–0x2C2B45 and `get_effective_disposition` 0x265A40–0x265AE6;
- the `data\` folder listings of both installs.

Re-read while integrating the gap round:
- the `mission_activate` wrappers 0x38E560 and 0x39D960 (caller-cleaned, `pop ecx; ret`), so `C3` at their entries is a
  valid early return;
- the store at 0x39D69C, the `script_do_frame` calls at 0x3EE1FB and 0x39D93D, the `gameplay_do_frame` call at 0x3EDF23;
- 0x6E6B44 `mov ecx,esi` before the non-local stats calls (jmp target 0x6E6BA4);
- the 7-byte `cmp byte [Multiplayer],imm8` form at 0x339EA3, 0x339FE9, 0x33811A, 0x3249AC, 0x332772, 0x325051, 0x3B0024
  and 0x68A923 (compares with 1).

Research tools: the PDB, disassembly, the symbol/xref tables and the vpp_pc reader; the full disassembly cache path is
in the workflow instructions.
