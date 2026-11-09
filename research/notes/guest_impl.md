# M5 guest fences and rules (src/guest.cpp)

Implementation notes for the guest side of M5 "Join the host's world" (RESEARCH.md §3.3, §3.9, §3.13, §4.3, §4.7, §4.8,
§8 Q12-Q15; notes/gaps.md GF-*). All addresses are RVAs of the GOG rfg.exe (VA = RVA + 0x400000). Every row was read in
the disassembly cache and the raw exe bytes this pass.

## Lifetime

- `Guest_Install` (DllMain) does nothing. Nothing is patched before `Guest_Arm`.
- `Guest_Arm` (session.cpp, sim thread, when this game links as a guest with no mission and no modal activity but a
  convoy running: `GuestBusy` refuses the link otherwise, before the save guard):
  1. calls `World_ArmSaveGuard("guest")` (the one 0x811278 chain, world.cpp; idempotent there);
  2. checks **every** site below byte for byte (abs32 operands compared as runtime addresses);
  3. writes nothing unless all of them match; then writes them all.
- Nothing is ever restored: the fences stay until the process exits.
- Failure (a byte mismatch, or the guard): log + HUD, `Guest_Armed()` stays false, and no retry (the bytes will not
  change). **The session must not join when `Guest_Armed()` is false after `Guest_Arm()`:** session.cpp `Link()` checks
  it right after `Guest_Arm()` and stops ("the guest fences could not be armed"), before READY can start the host's
  capture and pause; `GuestJoin` checks again before any blob load (backstop).
- Every hook runs on the sim thread (gameplay, the gameseq UI states, loads). The net thread never enters this file.

## Sites

| Fence | Site | Original bytes | Patch | ABI checked |
|---|---|---|---|---|
| Save guard | 0x811278 | `52 E8 rel32` (requestSave) | world.cpp `World_ArmSaveGuard` | world_impl.md |
| GF-SAVE | `world::save_and_continue` 0x51D040 | `56 8B F1 8A 86 5C 26 00 00` | `E9` → `OnSaveAndContinue` (clears `World+0x265C` bit 7, logs, returns) | thiscall (ecx = World), 3 stack args, `ret 0xC` at 0x51D0DD. No caller reads eax (its own early-out returns garbage al). Hook compiles to `and byte [ecx+0x265C],0x7F` … `ret 0xC` |
| GF-MIS | 0x39D69C in `mission_system_load` | `A3 <abs Mission_checkpoint_restore_index 0x1250510>` | `E8` → `MisStoreStub` (`mov [gc_guestMisRead],eax; ret`) | Next instruction is `call mission_info_load` (0x39D6A1) with `esi` already pushed; the stub keeps every register and flag. Same effect as 5 × NOP, plus the value for the E10 log |
| GF-ACT | `mission_activate(handle,bool)` 0x38E560 | `8B 44 24 08 50` | `E9` → `OnMissionActivate` (logs, returns) | `pop ecx; ret` (0x38E56E): caller-cleaned. Only caller `world::setup_territory` 0x540AB0 (`add esp,8`, result unused) |
| GF-ACT | `mission_activate(char*,bool)` 0x39D960 | `51 56 8B 74 24 0C` | `E9` → `OnMissionActivateByName` + `90` | `pop ecx; ret` (0x39D9BA). Callers 0x633FD3, 0x65B783, 0x458C7E, 0x45867F, 0x45825E: none reads eax |
| GF-SCR | 0x3EE1FB | `E8 60 DE 34 00` (→ `script_do_frame` 0x73C060) | 5 × `90` | void, no pushed args; next instruction `cmp [Multiplayer]` sets its own flags |
| GF-POP | 0x3EDF23 | `E8 48 B8 FC FF` (→ `gameplay_do_frame` 0x3B9770, only caller) | chain → `OnGameplayDoFrame`: `Activity_node_start_on_load` 0x1250244 = -1 (logged when it was not), then the original | void, no args; the original starts with a call (no register input) |
| GF-LOAD | `world::set_pending_territory_load` 0x51C320 | `56 8B F1 E8 98 32 FD FF` (call → `hud_ui_save_load_kill_msgbox` 0x4EF5C0) | entry detour → `OnSetPending`; trampoline = the 8 bytes with the call re-aimed, `jmp 0x51C328` | thiscall, 6 stack args, `ret 0x18` (0x51C334 / 0x51C3D3); hook compiles to `ret 0x18` |
| Q14 backstop | `activity_base_modal::activity_start` 0x3ACA10 | `57 8B F9 8B 07 8B 50 7C` | entry detour → `OnActivityStart<0>` | thiscall, no stack args, plain `ret`, bool. 9 callers (xrefs), incl. `activity_convoy_data::activity_start` 0x3B5990 |
| Q14 backstop | `activity_base_free::activity_start` 0x3ACA80 | `56 8B F1 8B 06 8B 50 18` | entry detour → `OnActivityStart<1>` | same; 7 callers (assassin, car bomb, cont attack ×2, killing spree, mission vrail, mayhem `jmp`) |
| Q14 / Q13 | 7 callback stores (table below) | `C7 40/46 14 <abs fn>` | imm32 → `OnUse<K>` | `bool __cdecl(player*, bool go)`: go 0 at 0x6DE0C4 (`add esp,8; test al,al`), go 1 at 0x2F717A / 0x2F72E8 (result ignored) |
| Q15 fast travel | 0x454469 in `fsm_warp_to_safehouse_cb` | `E8 32 70 21 00` (→ `safehouse_get_start_pos_orient` 0x66B4A0) | chain → `OnFastTravelPos` (returns false) | cdecl (vector*, matrix*, object*), `add esp,0xC`; prologue `push esi; mov esi,[esp+0x10]`. false → `je 0x454493` = the callback's own no-warp exit (wait for button ups, `Msgbox_handle` = -1, return 1) |
| Esc → Load | 0x49DF50 (`hud_ui_save_load_load_cb`), 0x5076C7 (`..._selection_made`), 0x4B67A6 (`..._no_space_cb`) | `E8` → `verify_gamesave_start` 0x3E76A0 | chain → `OnVerifyStart` (no-op) | cdecl (game_save_info*, bool): sets `Pending_save`, pushes GS 0x37. These are its only callers |
| Esc → Restart | 0x4AF634 in `ingame_options_handshake` | `E8 B7 BA D1 FF` (→ `vdir_restore` 0x1CB0F0) | chain → `OnRestartVdir` (no-op) | cdecl (int, bool), `add esp,8` |
| Esc → Restart | 0x4AF641 | `E9 9A CD 06 00` (tail → `world::set_pending_reload` 0x51C3E0, ecx = World) | chain → `OnRestartReload`: HUD, `gameseq_pop_state`, return 0 | thiscall, no stack args, plain `ret` (0x51C440). Pop = the handshake's default branch (`jmp gameseq_pop_state` 0x4AF626) |
| Esc → Cancel mission | 0x4AF62B | `E9 40 F0 ED FF` (tail → `mission_cancel` 0x38E670) | chain → `OnCancelMission`: HUD, `gameseq_pop_state` | `mission_cancel` takes no args and returns at once when `Mission_active_index == -1` (0x38E679), i.e. always on a guest |
| Death → Continue | `player_death_options_continue_cb` 0x492FF0 | `80 3D <abs Multiplayer> 00 75 46` | `E9` → `OnDeathContinue` + `90 90` (the original never runs) | cdecl, reads no args. Refs: call 0x516382 (after `ui_unload_document` of the death menu) and the menu item 0x50433E |

Callback stores (each written by its `player_is_near_*` check into `player+0xCE4` usable-object, +0x14 = callback):

| Store | Callback | Refused press says |
|---|---|---|
| 0x65B85D | `player_start_mission` 0x633F40 | Only the host can start missions |
| 0x65B9B8 | `player_start_upgrades` 0x633FE0 | Only the host can buy upgrades |
| 0x65BA83 | `player_start_offensive_raid` 0x6340A0 | Only the host can start activities |
| 0x65BED7 | `player_start_demolitions_master` 0x6340D0 | 〃 |
| 0x65C06F | `player_start_riding_shotgun` 0x634140 | 〃 |
| 0x683CCF | `player_start_area_defense` 0x6341B0 | 〃 |
| 0x65C1E7 | `player_start_house_arrest` 0x634220 | 〃 |

- xrefs: each callback has exactly one `mem` reference (the store).
- `player_start_upgrades` is also called directly with go 0 by `hud_ui_process_use_action_info` 0x4E9560 (prompt only).
- The upgrade shop's only other entry is `unused_dcf_show_upgrades` 0x44AFC0 (debug command).
- A cheat that pushes GS_INGAME_UPGRADES_HUD (0x2A) directly gets past this fence (a purchase
  there would be free: the salvage tracker puts the cost back; its armor prop / hammer swap would stay). v1 leaves it to
  user discipline (as with salvage-adding cheats). A code fence, if ever wanted, belongs at
  `player_meta_data::purchase_upgrade` 0x6C37B0 (every shop route; check its bytes and thiscall ABI first).

Game functions called (prologue and a vanilla call site read for each): `hud_message_add` 0x455D30 (as session.cpp),
`gameseq_pop_state` 0x3D8890 (cdecl, no args), `player_meta_data::add_salvage` 0x611700 (thiscall (int, bool), `ret 8`;
call site 0x3B2DFE), `add_unlocked_weapons_to_weapons_cabinet` 0x611630 (thiscall, no args; 0x65E0BE),
`base_array<ui_weapon_list_item>::add_unique` 0x4761A0 (thiscall (item*), `ret 4`; 0x4772C2),
`weapons_get_weapon_info_from_weapon_unique_id` 0x7D9AA0 (cdecl (int) → weapon_info* or NULL; 0x45A928),
`player_create_armor_prop` 0x6B2F60 (cdecl (int level, player*); 0x6C3EC9).

## GF-SAVE and the warp bit

`world::save_and_warp` 0x51D120 and `world::save_and_continue_warp` 0x51D1B0 set the warp-pending bit
(`or byte [esi+0x265C],0x80` at 0x51D13E / 0x51D1CE, after their `0x24E0`, `0x265C & 0x24` and `mission_is_active`
checks), store the warp target (+0x26D8..+0x2707), then save: `save_and_warp` through `save_and_reload` 0x51D0E0
(`save_and_continue(0,0,1)` at 0x51D106, then `0x265D |= 4`, a reload that GF-LOAD refuses), `save_and_continue_warp`
directly (0x51D236, no reload). Only `save_state_finish` (0x53DD09 `and al,0x7f` / 0x53DD19, where the warp target is
applied) and boot (`world::system_init` 0x53BA13) clear the bit; no load does (`set_pending_territory_load` masks with
0xBC at 0x51C394). Under GF-SAVE the save never runs, so the bit would stay set until exit, and
`camera_shake_process` returns at once while it is set (0x2CCD44 `test byte [World+0x265C],0x80; jne` to its end):
no camera shake on the guest. `OnSaveAndContinue` clears it: both warp paths pass through it (the root cause is the
skipped save that would have consumed the request). Reached on an armed guest by `hud_ui_district_liberation_process`
0x456E24, `camera_stop_satellite_mode_internal` 0x2CCABD, `activity_warp_and_start` 0x37F7B8 (from
`activity_force_start_generic` 0x3A8835 / 0x3A88CA; GF-POP normally prevents it), a cheat's mission warp
(`mission_warp_and_start` 0x3AF652), and a warping video (`video_cutscene_do_frame` 0x3071DE, the only
`save_and_continue_warp` caller). Fast travel's 0x454484 is refused earlier; `mission_system_do_frame` 0x3B4D6F needs
a mission. The bit's other readers
(`save_state_start` 0x53C718, `save_state_finish` 0x53DCC1) never run on a guest, so nothing else changes.

## GF-LOAD policy (`OnSetPending`)

1. **The session's own load:** the return address lies inside this DLL and `Guest_AllowLoad()` was called within the
   last 10 s → the original runs. A true result uses up the pass. A false one (the game busy) keeps it for the
   session's retry.
2. **A warp-to-object video:** return address 0x306F1A in `video_cutscene_exit_state` with new_game 0 → let through,
   then a re-join request. That branch (0x306EFC-0x306F15, taken when `Video_cutscene_warp_to_object` 0x164AE8C is not
   -1) loads `""` with new_game 0, which reads the in-RAM world: on the guest the last host blob, because the guest never
   saves. A false result there would run `gameseq_set_state(GS_MAINMENU)` (0x306F26). Only scripts reach it
   (`Action_callback_video_cutscene_play` 0x722519, already off by GF-SCR), so it is a backstop.
   - **The zone-warp branch is refused** (0x306F2F-0x306F4D, return 0x306F52): `set_pending_territory_load(
     Video_cutscene_warp_to_zone 0x1E2A490, new_game 1, 0, 0, dlc, 0)` loads a fresh territory from the zone files, not
     the in-RAM world. The main menu's New Game takes it (`main_menu_process_delayed_selection` 0x4F7AC2:
     `video_cutscene_play("RFG_CINE_00A.bik", -1, "terr01_03_12.rfgzone_pc", ...)`), so an armed guest at the main menu
     would start a fenced new campaign. Refused, the game runs `gameseq_set_state(0, 1)` at 0x306F5A: back to the main
     menu. The other `video_cutscene_play` callers reach neither branch: handbook 0x46940A and upgrade shop 0x47FDA4
     pass -1 / 0, `dlc_process_delayed_selection` 0x508B92 passes object -1 (edi = -1, 0x5085C2) and zone 0.
3. **Anything else:** log the caller, clear `World+0x265D` bit 2, return false, and send a re-join request. guest.cpp
   only logs `guest: re-join wanted: <why>`; the one debounce is `Session_RejoinRequest`'s 5 s window, which holds a
   request inside it and sends it when the window opens (session_impl.md "REJOIN_REQ"). The host debounces too.
4. **Two refused callers that never back out by themselves**, backed out after step 3 as their own siblings do:
   - **Esc → Load armed mid-way** (return 0x3F0359, `verify_gamesave_process`, GS 0x37). A guest still dialling is not
     armed, so its own Esc → Load passes `verify_gamesave_start`; WELCOME during the verify's multi-frame read arms the
     fences, and state 6 (0x3F0316) runs `difficulty_set_level_reload_overwrite(-1)` and `platform_load` (0x3F032E) and
     then the load at 0x3F0354, which is refused. 0x3F0359 sets `Verify_state` = -1, every later frame takes the `ja`
     exit (0x3F00DB) and only the load's `gameseq_set_state(2)` (0x51C3C7) would have left GS 0x37: a loading screen with
     a null input handler for good. The hook calls `gameseq_pop_state` 0x3D8890 (cdecl, no args, queues the pop), as
     the verify's own failure path does from the same process function (0x3F0257 `Verify_state = -1`, 0x3F0269 pop;
     0x3F0128 jumps there too). `verify_gamesave_exit` 0x3C93C0 then gives up the input id and removes the loading
     screen. The pop returns to the state that pushed GS 0x37 (save/load UI or the Esc menu); the blob the host sends
     for the step-3 REJOIN_REQ loads once the guest is back in gameplay. platform_load's side effects (World+0x2578 = 0,
     World+0x2694, cheats off, the guest save's hammer / backpack / jetpack globals) stay: the host's world load
     (WorldMeta) and LOADOUT replace them.
   - **The DLC intro video's load** (return 0x3BAF7A, `dlc_load_level`, whose call 0x3BAF75 ignores the result).
     After a session the armed guest may pick Demons of the Badlands at the main menu: `dlc_process_delayed_selection`
     plays the intro (`video_cutscene_play` 0x508B92 with the level as argument 8 -> `Video_cutscene_dlc_level`
     0x1E2A488; `game_pause` 0x508BBA) and `video_cutscene_enter_state` pushes / sets GS 0x31 (0x306E78 / 0x306E83).
     At its end `video_cutscene_exit_state` (from `video_cutscene_stop_callback` 0x306FD0 or the play handshake) takes
     the DLC branch, 0x306F6D `dlc_load_level`, then `ret` 0x306F75: no fallback, no pop, no `game_unpause` (0x306FA1
     skipped), so GS 0x31 stays on a freed video for good. When the caller is 0x3BAF7A and the top state is 0x31 the hook
     calls `gameseq_set_state(0, 1)` 0x3D8730 (cdecl (state, bool); `push 1; push 0` at 0x306F56-0x306F5A), what the
     two refused sibling branches of the same function do (0x306F26 / 0x306F5A; they skip `game_unpause` too). The
     direct path (no video, 0x508BC9) is not in GS 0x31 and is left alone; the third caller 0x3CB6E9 is a debug command.

Corrections found this pass:
- **The "retries every frame" claim is refuted.** RESEARCH.md §4.7 [inferred]: "process_save_load retries it every
  frame". In fact `process_save_load` clears bit 2 itself right after its call, whatever the result:
  `and byte [World+0x265D], ~4` at 0x3C0171. The detour clears it anyway.
- **Esc → Load is blocked earlier than 0x3F032E.** The verify state machine copies the chosen save into the world load
  buffer (0x3F0280, `get_load_buf` + `set_cur_size` + memcpy) before `platform_load` (0x3F032E). The fence therefore sits
  at `verify_gamesave_start`, and the guest's in-RAM world stays the host's.
- **Unreachable on the guest:**
  - `humc_request_exit` 0x44A8D8: the mission-complete screen opens only from `mission_terminate_internal` 0x3B4BCF,
    i.e. only with a mission. Correction: that held only for a guest that linked with no mission of its own; a mission
    still running at the link failed on the guest's death (0x3EDC0C) into GS 0x28 for good. session.cpp `GuestBusy`
    now refuses the link then (session_impl.md "Guest link"), and GuestJoin also loads a blob from GS 0x28 (backstop);
  - credits 0x512761: refused if it ever comes. `dlc_load_level` 0x3BAF75 is reachable after a session (DotB at the
    main menu): GF-LOAD step 4.
- **The save UI does not hang without a save.** `Wait_for_write` is cleared once `game_save_get_state() == 0`
  (0x5122C6), which is immediate when no save started. GF-SAVE shows "Saving is off on the guest" for its three
  save-UI callers (returns 0x49DFCB, 0x507767, 0x507EFD).

## Rule Q14: activities

- **Every start converges on one of two functions.**
  - Node presses go through the 5 activity callbacks above, then the specific starts (`activity_raid_attack_start`,
    `activity_house_arrest_start`, …).
  - Ready-state activities start from their own `activity_do_frame_ready`: Transporter (delivery truck), Assassination,
    Courier.
  - Replays go through `activity_force_start_generic` 0x3A8690.
  - All of these reach `activity_base_modal::activity_start` or `activity_base_free::activity_start`.
- **The backstop refuses at the entry of those two.** Every type except 13 (`ACTIVITY_NAMES` 0x12502A0: 1 Vehicle_Rail,
  2 Mission_VRail, 3 Tanks_a_Lot, 4 Delivery, 5 Killing_Spree, 6 Courier, 7 Mayhem, 8 Raid Defend, 9 Raid Attack,
  10 House_Arrest, 11 Demolitions_Master, 12 Assassination, 13 Convoy, 14 Cont_Attack, 15 Car_bomb, 16 Area_Defense)
  gets false before any side effect of these two. False is the game's own refusal (`Current_activity_mode != 0`,
  0x3ACA22).
- **Some derived starts act before the modal or free start:**
  - DM: `do_startup_initialization` (its node press is refused first);
  - Mayhem: `add_time`, an audio cue and a tooltip (0x3ADB90-0x3ADBC6);
  - Convoy: `set_active_flags_*` (allowed anyway).
- **Convoys (13) pass:** `activity_convoy_data::activity_start` → modal start. Their reward goes through
  `apply_standard_rewards` → `add_salvage`, and the salvage tracker forwards it to the host.

## Rule Q13: the host's loadout

### Interface

`Rules_ApplyHostLoadout(upgrades, upgradeBytes, arsenal, arsenalBytes, salvage)` is called by session.cpp on the sim
thread. All of it is untrusted.

- `upgrades`: exactly 0x400 bytes, the host's `player_meta_data+0x18` `upgrade_item[128]` (8 bytes each: byte 0 level,
  word +2 unlocked bits, +4 notified bits, +6 new-to-store bits).
  - Each level must be 0 or below the level count of `Upgrade_data` 0x1E395F8 (stride 0x228, count byte +5, 0x3E
    entries). That is the `upgrade_definition_data::get_upgrade_level` 0x35F4C0 bound.
  - Entries 0x3E-0x7F must be 0.
  - Otherwise the whole table is kept as it was (logged).
- `arsenal`: the host's `Weapon_list` 0x1260ED0 items (`farray<ui_weapon_list_item,64>`: +4 data, +8 max, +0xC count).
  Each item is 8 bytes: u32 weapon unique id (`weapon_info+0x8`), u8 purchased, 3 bytes of padding.
  - At most 0x200 bytes, a multiple of 8. proto.h `MsgLoadout` uses the same layout.
- `salvage`: the host's total; negative values become 0.

### Applying it

Applied on the next gameplay frame with a `Player`. The weapon locker UI is a game state of its own, so the list is
never rewritten under it.

1. The upgrade table is copied into `Player+0x2978+0x18`. Right after, in the same call, the guest's Q17 backpack
   levels are written over it (`PickTable`, notes/picker_impl.md), so the guest keeps its picked pack.
2. Salvage is written to `Player+0x2978+0x4`, and the tracker's baseline is set to it.
3. The locker list is rebuilt the way a load builds it:
   - count = 0, as `hud_ui_weapon_cabinet_reset` 0x45B450 does;
   - `add_unique` for each item whose id `weapons_get_weapon_info_from_weapon_unique_id` resolves; unknown ids are
     dropped and counted;
   - then `add_unlocked_weapons_to_weapons_cabinet`, which `load_meta_data` 0x65E0BE runs after the cabinet load. It
     unlocks or adds every weapon whose type-2 upgrade has level ≥ 1.
4. Armor: if upgrade 0x1A's level rose, `player_create_armor_prop(level, Player)` runs, as `purchase_upgrade` does
   (0x6C3EAF-0x6C3EC9).

### Upgrade effects

Read live from the local meta data, so nothing to re-apply:

| Effect | Read by |
|---|---|
| Armor damage reduction | `human_damage` → `upgrade_get_player_armor` 0x376200 |
| Ammo capacity | `weapon::get_max_rounds` → `upgrade_weapon_has_extra_ammo` |
| Charges | `upgrade_get_max_charges` |
| Backpack type, jetpack | `player::do_frame` 0x6D5694 → `upgrade_get_backpack_type` 0x375CE0 (upgrade 0 jetpack > 0 → 0; else the first of 0x35..0x3D > 0; else 0x3E = no change), compared with `s_lastBackpackIndex` 0x1293490; a change calls `multi_game_backpack_create` 0x59AC50 itself (not in Multiplayer, Wrecking Crew or DLC) |
| Grinder, arc welder, rockets, mines, detector, radiation, nano, ore, salvage collector, safehouse warp, singularity | the `upgrade_has_*` getters |

Explicit side effects of `purchase_upgrade` 0x6C37B0:

| Side effect | What M5 does |
|---|---|
| Weapon unlocks → cabinet | the rebuild above |
| Armor prop | above |
| Hammer inventory swap (0x6C3AAA-0x6C3D4E) | **not live**: the guest keeps its old hammer until its next re-join |
| Jetpack / backpack object (purchase_upgrade's own create 0x6C3E6B-0x6C3E92, jetpack only) | live: `player::do_frame` makes the same object from the copied table on the next frame |
| `s_backpacks_unlocked` 0x2C16C50, `s_jetpack_upgrade_level` 0x2C16C52, `s_hammers_unlocked` 0x2C256B0 | not needed: read only by shop prices (`upgrade_level::get_cost`, `get_next_upgrade_level`), saves, `player::serialize`, `select_jetpack` (level init) and load/reset; the guest has no shop or saves, and WorldMeta sets them at every join and re-join (world.cpp) |

- **Hammers are the one gap in rule Q13's "live".** `purchase_upgrade`'s hammer branch zeroes the other
  `Hammer_upgrade_data` (0xF10058, stride 0xC: upgrade index, inventory item name) levels, removes the buyer's slot-3 item
  (`human_get_inventory_item(Player, 3)` 0x607820, then an inlined removal 0x6C3B33-0x6C3D2A), adds the new hammer with
  `human_add_item_to_inventory` 0x6B4E20 (cdecl, 8 args: player, info, -1, -1, 3, 1, 0, 1; `add esp,0x20` at 0x6C3D42)
  and, if the held weapon class is 7, `human_equip_inventory_item` 0x6BF410 (cdecl (human*, item*)). ApplyLoadout copies
  only the table, and nothing rebuilds the hammer from it (the cabinet skips class 7; the shop is fenced). The new
  hammer reaches the guest with the next re-join's world (`player::stream`).
- **Open (needs the user's call):** accept "next re-join" for hammers, or make it live: in ApplyLoadout, when the
  `Hammer_upgrade_data` entry with level ≥ 1 changes, delete the old slot-3 item with `human_delete_item_from_inventory`
  0x6C76A0 (cdecl, 4 args as `player_do_weapon_swap` 0x6CD7AD-0x6CD7B8: human, item, 0, 0) and add / equip the new one as
  above; the item info comes from the entry's +4 (`inv_item_get_info` 0x612240, as 0x6C3B0A). Untested; the hammer may be in hand.

### Salvage

`add_salvage` 0x611700 has only two callers: `apply_standard_rewards` 0x3B2DFE (activity rewards) and
`mission_terminate_internal` 0x3B4AFC. Pickups, crates and mining write the meta elsewhere, so the guest uses a
**per-frame delta**:

- **A rise** of `meta+0x4` is sent with `Session_SendSalvage(gain)` and stays on screen until the next LOADOUT replaces
  it with the shared total.
- **A drop** is put back: the guest cannot spend the host's wallet.
- **After a load** (GS_LOAD seen), the loaded value is adopted, not sent: it is the host's value from the blob.
- **Order in `Guest_Tick`:** the delta first, then a pending LOADOUT. ApplyLoadout writes the host's total into
  `meta+0x4` and the baseline, so a gain from the frame before would otherwise be overwritten unseen and lost. The host's
  total may lag that gain for a second on screen (the next LOADOUT has it).

`Rules_AddHostSalvage(amount)` (host): `add_salvage(meta, amount, false)` on the host's own `Player`. It takes 1..30000
(the game caps the wallet at 30000) and is dropped without a `Player`.

## Rule Q18: the guest keeps its own weapons across re-joins

Every re-join loads a host blob, and `player_level_init_post` → `player::load_inventory` 0x6C2F80 gives the guest the
host's items. Q18 puts the guest's own back. Code: guest.cpp `RecordLoadout`, `RestoreLoadout`, the `g_lo*` state, a few
lines in `Guest_Tick`, one in `OnSetPending` (GF-LOAD) and `Guest_ResetLoadout`, which session.cpp `Start` calls on a
guest. No new hook, nothing patched, nothing saved.

### What is recorded

What `player::save_inventory` 0x634740 keeps per item, for selection slots 0..2:

| Field | Read from | Checked |
|---|---|---|
| item | `human_get_inventory_item(me, slot)` 0x607820 → `inventory_item+8` `inv_item_info*` → `+0` name, copied (48 chars) | PDB `inventory_item` (0x1C): +0 next, +4 prev, +8 info, +0xC count, +0x10 selection_slot, +0x14 attachment_prop, +0x18 weapon_handle. The name, not the pointer: `load_inventory` itself resolves names (v_stricmp over `Inv_item_info`, 0x6C2FFB) |
| count | `inventory_item+0xC` | saved by 0x6347BC |
| magazine / reserve | weapon = `world::object_from_handle(+0x18)` 0x53BEE0 (thiscall, `ret 4`, -1 → NULL), kept only if `+0x7E == 1 && +0x7F == 7` (as 0x6347E2-0x6347EC); `+0x20C` / `+0x210` (`weapon::get_rounds_in_magazine` 0x1DC6B0 / `_reserve` 0x1DC6C0) | -1 / -1 when there is no weapon object |
| held slot | `player+0xA30` desired_equipped_inv_item → `+0x10` (what puppet.cpp's capture sends); when it is NULL, `player+0xA34` (the unequipped item); -1 = neither | `human_unequip_inventory_item` 0x6537F0 moves +0xC90 to +0xA34 and zeroes +0xA30 (0x653831-0x653846); callers: turret 0x6B5F5D, held object 0x6BE9D9, holster (`player_equip_item(NULL)` 0x6C2AB4 ← 0x6CD544). Unholster re-equips +0xA34 (0x6CD54F); `human_delete_item_from_inventory` replaces it when that item goes (0x6C7765-0x6C7779), so it never points at a freed item. After a re-join the turret / held object is gone, so the weapon goes back in hand |

- Slot 3 is the hammer and belongs to Q17 (`ApplyPick`): never recorded or touched here. The selection slots are -1..3
  (`human_find_open_inventory_selection_slot` 0x606AB0 marks a 4-byte array; the weapon locker scans 0..2); slot -1
  items are left alone.
- Thrown weapons and charges are ordinary items in these slots (`human_add_item_to_inventory` only adds items with a
  `weapons_get_weapon_info`, 0x6B4E63-0x6B4E73), so they are recorded the same way.
- **When:** every gameplay frame with a Player that is on foot (`human_in_vehicle` NULL) and alive (`human_is_dead`
  0x1DC0F0, cdecl (human*), vtable +0x10; call site 0x237AD3), from the session's first join on (`g_loOn`). So `g_lo` is
  the last good state before the player goes away at a load: a death stops it at the last alive frame, a vehicle at
  the last frame on foot, a locker UI or menu (not gameplay) at the frame before it.
- On the first GS_LOAD frame of each load the record is logged (`Q18 loadout before the load: ...`).

### First join and re-join

- The boundary is the **session**: `Guest_ResetLoadout` (record, `g_loOn`, `g_loDue`, `g_loStart` cleared) runs in
  session.cpp `Start` on a guest (F8 confirm or autoconnect, once `NetStart` succeeded), never at a link.
- Recording starts only at the end of a load the session let through: GF-LOAD's `ours` branch sets `g_loStart` when
  the session's `set_pending_territory_load` starts; the `g_loaded` block in `Guest_Tick` then does `g_loOn |=
  g_loStart` and clears it. So a load the guest began itself while still dialling (unarmed: death → Continue, Esc →
  Load, fast travel), which a WELCOME landing mid-load would otherwise count, ends with recording off and nothing to
  restore (log `... not recorded (a load from before the link)`). A separate flag, not `g_loOn` set in `OnSetPending`:
  gameplay frames can run between `set_pending` and GS 2 and would record the guest's own inventory.
- The session's first join (host: `g_blobSent` false, so `kBlobJoin`: `OnMessage` READY, or REJOIN_REQ before any
  world) finds no record: the host's loadout stands (log `Q18 nothing recorded this session: ... recording from now`).
  Nothing from the guest's own world is ever recorded: before the first `Guest_Arm` `Guest_Tick` returns early, and
  afterwards only `g_loStart` turns recording on.
- Every later load (host load re-send, the guest's REJOIN_REQ after its death or a refused load, the warp video's
  in-RAM reload) sets `g_loDue` in `Guest_Tick`'s `g_loaded` block, next to Q17's `g_pickDue`.
- A redial after a dropped link (`Unlink`: phase Waiting, the session goes on until F8 / REJECT / failure) keeps the
  record. The host's `JoinReset` makes its next blob a `kBlobJoin`, but on the guest it is a re-join: the record is
  restored over it. The HUD says "Co-op: <host> gone, reconnecting (F8 stops; ...)". Leftover risk: a host that stops
  and starts its own session (F8 twice, or another save) while the guest keeps redialling gets the guest's weapons
  restored into that world; unknown names are skipped, but a weapon that save has not unlocked (Q13) can stay.

### Restore (`RestoreLoadout`)

On the first gameplay frame on foot after the load, in the same block and right after `ApplyPick` (so slot 3 is
already decided), before the recording of that frame:

1. **Pass 1, per slot 0..2:** the recorded name → `inv_item_get_info` 0x612240 (cdecl, NULL unknown). An unknown name
   is skipped and logged; the slot keeps what the blob put there. Otherwise the blob's item is deleted with
   `human_delete_item_from_inventory(me, item, 0, 0)` 0x6C76A0 when it differs (also when our slot was empty). The
   delete fixes +0xA30/+0xA34/+0xA38 and re-equips the last item if the deleted one was held (0x6C76EF-0x6C774B), then
   `human_validate_inventory` (pointer checks only).
2. **Pass 2, per slot:** a slot already holding our item is kept; else `human_add_item_to_inventory(me, info, count,
   -1, slot, 0, 0, 0)` 0x6B4E20 (cdecl, 8 args, plain `ret`; `load_inventory` 0x6C3091-0x6C30A4 pushes the same
   order). Two passes because the add merges into any carried item with the same info, wherever it sits, and only adds
   ammo to it (0x6B4E50, 0x6B5169-0x6B5239); in the player's SP path the slot must be free
   (`human_inventory_slot_is_available` 0x6B4EE9) or it returns NULL. A NULL result, or an item in another slot
   (merged into a slot 3 / -1 item), is logged as not added.
3. **Ammo:** set after the add, exactly: `weapon::set_rounds_in_magazine` 0x1FB530 (thiscall (int), `ret 4`, clamps to
   0..`get_magazine_size`) then `weapon::set_rounds_in_reserve` 0x369C90 (thiscall (int), `ret 4`, clamps to
   0..`get_max_rounds` - magazine size). The add's own ammo argument is -1 (nothing set): its split fills the magazine
   first and multiplies by the backpack's ammo factor (0x6B5018-0x6B5078).
4. **Held slot:** `player_equip_item(me, item in that slot, NULL, 0)` 0x6C2770 (as ApplyPick; already held → 1). The
   recorded held slot may be 3 (the Q17 hammer).

Not changed: the host's LOADOUT (Q13) never touches the inventory, so it needs no Q18 step.

**Ammo vs vanilla.** Vanilla refills every weapon at every load: `load_meta_data` 0x65E0F6 → `local_player_fill_ammo`
0x634470 (`weapon::fill_ammo` 0x7E6960 per item with ammo). The blob's items arrive full; Q18 puts back the recorded
magazine and reserve, so after a re-join the guest has the ammo it had, not a refill (the host, loading normally, is
refilled). If the user wants vanilla's refill instead: call `weapon::fill_ammo` (thiscall, no args) in place of the two
setters. Open for the user.

### Log lines

```
guest: Q18 nothing recorded this session: the loaded weapons stand, recording from now
guest: Q18 nothing recorded this session: the loaded weapons stand, not recorded (a load from before the link)
guest: Q18 loadout before the load: 0 assault_rifle 30/180, 1 remote_charge 1/4, 2 - -1/-1, held 0
guest: Q18 slot 0: smg removed
guest: Q18 slot 2: rocket_launcher removed
guest: Q18 slot 0: assault_rifle added, ammo 30/180
guest: Q18 slot 1: remote_charge kept, ammo 1/4
guest: Q18 slot 1: xyz is unknown in this world, skipped (keeps remote_charge)
guest: Q18 loadout restored, held slot 0 (assault_rifle)
```

### In-game checks (guest = B)

1. First join: B has the host's weapons, log `nothing recorded this session ... recording from now`.
2. B takes other weapons at a locker (and picks one up from the ground), fires some rounds. Host fast travels: after
   the re-join B has its own three weapons with the same magazine / reserve, holding the same slot; the Q17 hammer and
   pack too.
3. Same after the host's death, a mission retry, an Esc load on the host, and B's own death → Continue.
4. B empties a slot (if the game allows it) → after a re-join the slot is empty.
5. B in a vehicle when the host loads: the restore waits until B is on foot (log lines appear then).
6. The weapon shows in hand after the restore (the add attaches it to the hand tag when the character instance is
   loaded, 0x6B4FA6-0x6B5007).
7. F8 stop / F8 start (a new session): the first join starts from the host's weapons again.
8. One PC: B dials a host that is not up, fast travels, and the host starts during B's loading screen: after B's own
   world `... not recorded (a load from before the link)`, after the join `... recording from now`, no `Q18 loadout
   restored` line.
9. B on a turret (or carrying an object) when the host loads: after the re-join B holds the weapon it had before.
10. Link drop over 15 s (or host F8 off / on) while B keeps its picks: after the redial's join B has its own weapons
    (`Q18 loadout restored`).

## Logs (the E10 check from the log)

`guest:` lines in RfgCoop.log:
- every fence hit with its caller (`from rfg+RVA`; first 5, then every 100th);
- every HUD line, at the moment it is asked for. The line itself shows only in gameplay (else on the first gameplay
  frame after): Esc → Load is refused inside the save/load UI (GS 0x32 over the Esc menu or the death screen; it closes
  back to that state) and fast travel inside the map (the no-warp exit 0x454493 leaves it open), so their "host only"
  line appears once the guest closes the Esc menu / the map. Restart and Cancel mission pop the menu themselves;
- every re-join request and the session's own load;
- on the first gameplay frame after each load: `cur_world_state`, the restore index `mission_system_load` read (not
  stored), `Mission_active_index`, `Mission_mode`, and the refusal counts so far;
- GF-POP logs each non -1 `Activity_node_start_on_load` it clears.

Not built (add when E10 runs as a single-instance experiment):
- an ini mask to switch single fences off;
- a count of managed `Action_scripts`.
