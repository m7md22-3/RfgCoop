# RfgCoop: ghost puppet implementation (experiment E2, milestone M3)

Code: `src/puppet.cpp` (all of it), `src/features.h` (comment), `RfgCoop.ini` (`ghost_offset`, `l5`, `pp_stats`). `core.cpp` is
unchanged: it already calls `Puppet_Install()` from DllMain and `Puppet_Tick(state)` from the 0x1D24D9 tick.
Build: GOG `rfg.exe` 32-bit, every address is an RVA (`rfg.asm` prints VA = RVA + 0x400000). Nothing here has run in
game yet; "verified" means read in the disassembly, the PDB or the exe bytes on disk, and for the DLL side in
`llvm-objdump` of the built `build/xinput1_3.dll`.

## What it does

- `dev=1` in `RfgCoop.ini`, then **F6** while the game window is focused (`Os_app_active` 0x1118591), in GS_GAMEPLAY
  (state 1) with `Player` (0x02C22874) set. F6 again removes the ghost. HUD lines through `hud_message_add`.
- Refused (HUD line) in multiplayer, during Wrecking Crew, or while you sit in a vehicle.
- The first F6 checks every site below. If one differs, nothing is written, the log names the site and the HUD says
  "Ghost unavailable". Before the first F6 the DLL patches nothing for the ghost.
- The ghost is a second `player` object built with your own `h_info` (+0xCC), guerrilla team, `no_save`, `never_die`.
  It stands `ghost_offset` metres (default 2) along world +X from where you were, so it never stands inside you.
- Every gameplay frame, after the simulation, your Player is captured into a `multi_player_state`. Each capture is
  delivered 300 ms later with `network_time = capture + 300 + 100`, through `add_state` into `puppet+0x1274`. Before
  every simulation `multi_game_player_process_state(puppet)` runs. So the ghost shows you about 0.4 s late, exactly as
  a remote player whose packets arrive 300 ms late would look.
- When the game destroys every player (level load, save load, death reload), the tick sees the handle die, restores the
  per-puppet patches and recreates the ghost once the new level's Player is streamed in, under the same refusals as F6
  (not in MP, not in Wrecking Crew, not while you sit in a vehicle: it waits until you are on foot).

## Hook and patch sites

All bytes were read from `<game copy B>\...\rfg.exe` and re-read here. `<MP>` is the ASLR-relocated absolute of
`Multiplayer` (`88 A5 FE 02` on disk = VA 0x02FEA588); at runtime the check compares against `g_base + 0x2BEA588`.

### Installed once, on the first F6 (each passes straight through when no puppet exists)

| Site | Original | New | Why | Verified |
|---|---|---|---|---|
| 0x3EE55C | `E8 FF F5 FF FF` call `gamestate_gp_simulate_frame` 0x3EDB60 (`void __cdecl()`) | call `OnSimFrame` | before: deliver due states, apply the effects of states Clock has reached, `process_state`; after: capture Player. Gameplay only, skipped while `s_pauseGame` (0x3EE520 `jne`) | target computed from bytes; PDB sig; caller `call; mov byte [Sim_snapshot+0x44],1; ret` |
| 0x2FC841 | `E8 5A 07 3A 00` call `player_move_set_anim_direct` 0x69CFA0 | call `OnAnimDirect` (records `side`, `fwd` when `p == Player`, then tail-jumps to the original) | SP never stores the local stick in `+0x117C/+0x1180` (the stores at 0x2FC5A8/0x2FC5B0 sit behind `cmp Multiplayer,0; je 0x2FC6D4`) | xrefs: only callers 0x2FC841 (local) and 0x5CF100 (puppet); both push `(p, sideways, forward, sprint, stopped)`; cdecl (`add esp,0x1C` at 0x5CF11A covers both calls); prologue `sub esp,8 ... mov esi,[esp+0x18]` |
| 0x6E62D7 | `E8 84 F3 FE FF` call `player::do_frame` 0x6D5660 (thiscall, `mov ecx,esi` before) | call `OnPlayerDoFrame` (`__thiscall`) | backpack theft (`s_lastBackpackIndex`), radiation, district HUD and radio treat any player as local | its only caller; return value unused (next insn `cmp Multiplayer,1`) |
| 0x6E6B36 | `E8 15 75 FF FF` call `player_process_object_interaction` 0x6DE050 | call `OnInteract` | the puppet would eat salvage and pickups (`object_item_player_touch`) | `push esi; call; push esi; call human_weapons_process_human; add esp,0x18` = cdecl; non-local path only |
| 0x6E59E7 | `E8 74 E9 F4 FF` call `player_is_killed` 0x634360 | call `OnKilled` | second guard after never_die: no 4 s death timer, no SP death branch | its only caller (`push esi; call; add esp,4`) |
| 0x6CE950 | `83 EC 0C 56 8B 74 24 14` (`human_damage` prologue) | `E9 rel32` + 3 NOP to `OnHumanDamage`; trampoline = the 8 bytes + `E9` back to 0x6CE958 | ghost rules: victim == puppet -> return 0.0, attacker == puppet -> 0.0, and a NULL attacker that the original would charge to the puppet, dmg >= 0 -> 0.0 (below; forced kills pass); else the original | 8 position-independent bytes; no xref lands in 0x6CE951..0x6CE957; float returned in st0 (objdump: `fldz` / trampoline result) |
| 0x653124 | `A0 <MP>` (`mov al,[Multiplayer]`), then `84 C0 74 41` | `E9` to `AirCave` | PP-AIR: an airborne player-subtype human is steered by the **local** stick in SP (0x65316E); the puppet must use its own `+0x1180/+0x117C` (0x65315C), which `apply_movement_state` fills | `[esp+0x11]` = is player (0x652C1F), `[esp+0x12]` = is local (0x652C33); flags after both targets are rewritten (`comiss` at 0x6531C3) before use |
| 0x67ED30 | `80 3D <MP> 00 74 2B` | `E9` to `MeleeCave` + 4 NOP | PP-MELEE: in SP every lunge heads along `Rfg_camera+0x68` (your view, 0x67ED64); the MP path uses the attacker's `+0x1250` (`mp_camera_orient.fvec`) | eax = acting human if subtype 3 else 0 (0x67ED18-0x67ED2E); edi = 0 (0x67EC6D); ecx overwritten first on both paths (0x67ED3D, 0x67ED69); nothing jumps into 0x67ED31..0x67ED38 |
| 0x630B07 | `E8 E4 A7 C9 FF` call `camera_free_set_submode` 0x2CB2F0 (crouch framing) | call `CrouchThunk` | PP-CROUCH: the puppet crouching switched **your** free camera to crouch framing | esi = the human (0x630AC4); cdecl `(int, float, float)`, void, reads `[esp+4]` (no register arg); caller `add esp,0xC` |
| 0x680783 | `E8 68 AB C4 FF` same, stand framing | call `CrouchThunk` | same, standing up | esi = the human (0x68074B); same ABI |
| 0x6C1CD4 (ver6.2) | `E8` rel32 call `upgrade_get_backpack_type` 0x375CE0 (`int __cdecl()`) in `player::stream` | call `PackTypeThunk` | `upgrade_get_backpack_type` reads **your** table (`player_get_meta_data` 0x610550 reads `Player`), and 0x6C1CF2-0x6C1D18 then make that pack for whoever streams in (the puppet: it wore your pack) and store the global `s_lastBackpackIndex`. Player not 0 and esi not Player: eax = 0x3E (none, 0x6C1CD9 skips to the armor); else tail-jump to the original. Section "Partner gear (ver6.2)" | esi = this (0x6C1C87), callee-saved; MP (0x6C1CBA) and WC (0x6C1CC7) branch away before the call; `ExpectCall` checks the target |
| 0x59AA78 (ver6.3, G5) | `E8` rel32 call `camera_shake_stop` 0x2C6F40 (`void __cdecl()`) in `multi_object_backpack::deactivate` | call `ShakeStopThunk` | the end of the puppet's pack (or a swap) cleared **your** current camera shakes. Pack wearer `[esi+0x274]` is the puppet: plain `ret`; else tail-jump to the original. Section "Backpack effects P1 (ver6.3)" | esi = the pack (0x59AA35, callee-saved); the callee touches only eax / ecx (0x6C6F40-0x6C6F5F) and its result is unused (0x59AA7D reloads ecx); `ExpectCall` checks the target; xrefs: the only call of 0x2C6F40 in `deactivate` |
| 0x5F18CA (ver6.4, G3) | `38 1D <MP>` `cmp byte [Multiplayer],bl`, then `0F 85 B6 03 00 00` `jne 0x5F1C8C` in `multi_object_backpack::use_ripple` | `E9` to `RippleCave` + 1 NOP (the `jne` stays) | the puppet's Shockwave ripple ragdolled and hurt every NPC and vehicle within 6 m on your machine (SP = server). Wearer `[ebp+0x274]` is the puppet and `gc_ppWorld` bit 2 clear: `jmp 0x5F1E7B` (object-mover impulses only, the MP client's path); else the replaced compare and back to the `jne`. Section "Backpack effects P2 (ver6.4)" | all 12 bytes checked at install; ebp = the pack (0x5F185E), ebx = 0 (0x5F1860, untouched since), esi = the position, edi = the ability set: the registers and stack depth the MP client brings to 0x5F1E7B (0x5F18D0 -> 0x5F1C8C -> 0x5F1C93); eax kept (push / pop; `pop` leaves the flags); only 0x5F1890 jumps to 0x5F18CA and nothing into 0x5F18CB..0x5F18D5 (rfg.asm grep) |
| 0x5F1942 (ver6.4, G4) | `E8 79 6F C0 FF` call `world::object_from_handle<human>` 0x1F88C0 (thiscall, ecx = World, 1 arg, `ret 4`, NULL if not a human) in the SP ripple loop | call `RippleHumanThunk` | your ripple floored the puppet (and from M7a the puppet's would floor you): the SP loop has no team test. The thunk calls the original; a result that is you or the puppet becomes the wearer `[ebp+0x274]`, which the loop's own `cmp esi,wearer / je 0x5F1A8A` (0x5F194F) skips. Always on while a puppet exists | `ExpectCall` checks the target; the loop takes eax (0x5F1947); ecx / edx are the callee's (it calls `world::object_from_handle`); only players wear packs (`event_player_picked_up(player*)`), so the wearer is you or the puppet |
| 0x5F3188 (ver6.4, G6) | imm32 of `C7 44 24 7C <imm32>` `mov [esp+0x7C], rhino_cast_filter_callback` (0x57BF50, relocated) in `multi_object_backpack::process` | `&RamFilter` | the puppet's Rhino and Thrust ram hit NPCs (300 / 225 + ragdoll), vehicles and buildings on your machine. `RamFilter` rejects for a puppet wearer: players always, NPCs without bit 2, vehicles without bit 4, movers (the in-charge cutters) without bit 1; else the game's filter | 8 bytes checked (imm = `g_base + 0x57BF50`); the only reference to 0x57BF50 (xrefs); the one spherecast 0x5F3199 feeds every hit (0x5F31A9-0x5F3712); caller `havok_game_filter::isCollisionEnabled` calls it cdecl `(object, data)` with object non-NULL and stores al (0x41C06E-0x41C081); data+0 = wearer (0x57BFA4, 0x57C027); a false return without writing data+4 = neither a hit nor a stop |
| 0x59ABA7 (ver6.4, G7) | `E8 A4 39 FA FF` call `world::create_object_internal` 0x53E550 (thiscall, 4 args, `ret 0x10`) in `deactivate` | call `EndCutterThunk` | the end-of-charge cutter (`Rhino_shape_cutter_explosion`, Rhino and Thrust, SP-or-server 0x59AABA) of the puppet's pack. Wearer `[esi+0x274]` is the puppet and `gc_ppWorld` bit 1 clear: eax = 0, `ret 0x10`; else tail-jump. Bit 1 is on now, so it only counts | `ExpectCall` checks the target; result unused (0x59ABAC destroys the props); esi = the pack (0x59AA35); eax free (the hash, pushed at 0x59ABA1) |

**Ragdoll damage the game credits to the puppet.** The detour runs before `human_damage`'s own attacker substitution
(0x6CE964-0x6CE9B1): with attacker NULL and the victim's `ragdoll_state` +0x33C not 0/2/3 (4 only with +0xBC bit 15),
the original makes `object_from_handle(victim+0x340 ragdoll_source_human_handle)` the attacker. `human_ragdoll_start`
stores its arg 3 there (0x6C97AB-0x6C97AF), and the puppet is that source for its explosions
(`human_combat_process_explosion_hit` 0x6DC366 / 0x6DC451 -> `apply_explosion_ragdoll_impulse` 0x6D4062 -> 0x6C97AF;
the knockdown does not depend on `human_damage`'s result) and its knockdown swings (`human_combat_unified_melee_hit`
0x6D1129-0x6D1130). The follow-up impact and get-up damage come with attacker NULL (`havok_collide_ragdoll` 0x4259E6,
`human_ragdoll_get_up` 0x6D4FBF). So `OnHumanDamage` mirrors the state test exactly and returns 0.0 when +0x340 is the
puppet's handle (log `ragdoll damage by it swallowed`). The bare +0x340 is not enough: it stays set after the ragdoll
ends. The knockdown itself still happens (it deals no damage). Victim NULL passes through to the original's own check
(0x6CE958). Only `dmg >= 0` is swallowed: a negative dmg is the game's own forced kill (all current HP: `comiss 0,dmg; ja`
at 0x6CED66 -> 0x6CEDDB loads +0x914), sent with attacker NULL and -1.0f (`[0xEB8824]`) by kill regions
(`trigger_region_do_frame` 0x6D64A1 / 0x6D6505), the below-world phantom (`game_aabb_phantom::addOverlappingCollidable`
0x43BF69, fires once on entry: swallowing it would leave you falling forever) and `maybe_kill_player` 0x37F8D2.
Those pass to the original, which credits the kill to the puppet through +0x340 (vanilla
attribution; the puppet is not `Player`, so the 0x6CEE03 branch is skipped and only +0xDD0 player flags are set,
0x6CEE17 / 0x6CEE26).

### Toggled while a puppet exists (on at creation, back to the original when it is destroyed or the game frees it)

| Site | Original | Patched | Why | Verified |
|---|---|---|---|---|
| 0x6E6B44 PP-STATS | `8B CE` (`mov ecx,esi`) | `EB 5E` (`jmp 0x6E6BA4`) | the non-local tail calls `multi_game_stats_record_weapon_time` / `_backpack_time`, which read `[Mp_game_data+0x610]` with `Mp_game_data` NULL in SP: the first puppet frame would crash at 0x5862E6. `pp_stats=0` (E2 control run, below) leaves it unpatched | 0x6E6BA4 = `56 E8 ..` (`push esi; call player_cover_do_frame`); both skipped calls are balanced by their own `add esp,0xC` (0x6E6B83, 0x6E6BA1) |
| 0x3EDDC6 listener store | `89 6C 24 10` (`mov [esp+0x10],ebp`) | `90 90 90 90` | `gamestate_gp_simulate_frame` seeds `[esp+0x10]` with `subtype_objects[3][0]` (0x3EDB88), but after its per-player loop (ebp advances at 0x3EDDA6-0x3EDDBC) stores the LAST entry there, reloads it at 0x3EE16B and reads its +0x2FC stance at 0x3EE47A for the audio listener height (the listener position comes from `player_get_local_player`, 0x3EE3A9). The ghost is appended last, so your listener height would follow its stance. NOPed, entry 0 (you; `Create` refuses index 0) is kept | the only `[esp+0x10]` accesses between are this store and the reload (0x3EDE43 writes `[esp+0x18]` one push deep = base +0x14); ebp is not used again after 0x3EE16B except 0x3EE47A and the epilogue pop; nothing jumps into 0x3EDDC7..C9 (xrefs); count 0 jumps from 0x3EDB96 to 0x3EDDCA, past the store; with one player both values are the same |
| 0x6B19C8 (L5, operand of 0x6B19C6) | `<MP>` | `&g_coopOne` (a byte = 1) | `human_melee_attack`: the guarded block is only the 4-way jump table that adds 1 to the 2-bit melee ids in `+0xC4` bits 1-8 (0x6B19DB-0x6B1A42), then rejoins 0x6B1A48 | re-read: every case ends in a `+0xC4` write and `jmp 0x6B1A48` |
| 0x6CC4A3 (L5, operand of 0x6CC4A1) | `<MP>` | `&g_coopOne` | `human_weapons_firearm_pull_trigger_common`: the block only bumps the trigger-down id (bits 14-15) and copies it to the up id (16-17), 0x6CC4AA-0x6CC4D6; runs only for SINGLE/CHARGE weapons that are not linked (0x6CC48B, 0x6CC498) | re-read |
| `Reserved_network_to_game_hash` 0x1290C60 +0x40/+0x44, `Network_handle_to_rop_hash` 0x1290B88 +0x40/+0x44 (= 0x1290BC8/0x1290BCC) | size 0, buckets 0 | buckets = `&g_zeroBucket` (a zeroed dword), size 1 | a stray network-handle lookup (`multi_core_game_handle_from_network_handle` 0x5B6380 -> `uint_hash` divides by the size at 0x18FD00, then reads `buckets[h]` with no NULL check, 0x5B63A5) now finds nothing | `uint_hash` returns `h % size` (0x18FD00-0x18FD04); primed only if the size is 0 (else MP initialised it: left alone, logged); restored only if still ours |
| `Reserved_game_to_network_hash` 0x1290C18 +0x40/+0x44 (= 0x1290C58/0x1290C5C), `Game_handle_to_rop_hash` 0x1290B40 +0x40/+0x44 (= 0x1290B80/0x1290B84) (ver6.1, after the two-PC crash `C0000094` at rfg+18FD00) | size 0, buckets 0 | the same shared `g_zeroBucket`, size 1 | the game -> network direction: a puppet's movement-correction raycast -> Havok filter -> `collide_obstructions` 0x5CE126 -> `multi_core_network_handle_from_game_handle` 0x5B63F0 (reads 0x1690C5C/0x1690C58 VA at 0x5B6401/0x5B640D), miss -> `multi_core_get_remote_object_from_game_handle` 0x5B6340 (0x5B6340/0x5B6351), miss -> 0xFFFF (0x5B63FA) | same prime / restore rules. Checked 2026-10-08: from `multi_game_player_process_state`, `multi_game_player_correct_player_movement`, `collide_obstructions`, `player_do_frame` (direct call graph, the xref tables) every MP-table access is one of these four; `Debug_remote_object_hash` 0x1290BD0 is never reached; the other reachable `uint_hash` callers use SP tables (World+0x4C0/0x4C4, `dynamic_hash_table`, vfx) or a constant size (`activity_register_damage_is_player_controlled`, 0x1F4). The only writer, `add_remote_object_to_hash_tables` 0x5B5D90, is reached only through `multi_core_object_create_remote_object` 0x5D97E0, which returns at entry unless Multiplayer and Server (0x5D97F4 / 0x5D9811), so the shared dword stays 0. Teardown (`Destroy`: Detach restores, then `player_level_shutdown`) reaches no MP table in SP (`player::free_this` MP branch 0x6CD02C; `multi_core_object_process_object_free` 0x5D9FD0 is Multiplayer-gated). Virtual / indirect calls are not in the static graph |
| `Bm_bitmaps[h]+4` of the six hammer 1-3 low-res placeholders (ver6.2) | `Bogus_static_bitmap_peg_entry` (white) | the correctly named items-peg entry's +4 | the puppet's hammer 1-3 rendered white; section "Partner gear (ver6.2)" | only a real placeholder, target single-frame; restored from the saved value |

Not needed in practice: every state sent carries vehicle and turret net id 0xFFFF with cache dwords -1, and
`network_to_game_handle::get_game_handle` 0x5D2A80 / `apply_vehicle_state` 0x5E1B6D return before any lookup for
0xFFFF. The priming is the belt the research asked for.

**E2 control run, `pp_stats=0`** (RESEARCH.md §6 row E2: the stats skip "behind a toggle"). The first F6 still checks
`8B CE` at 0x6E6B44, but `Lifetime` writes nothing there in either direction (so no spurious mismatch on restore). The
load logs `PP-STATS off (control run)`. Expected on the first puppet frame, from the crash logger: an access violation
at rfg+0x5862E6 reading 0x00000610, in `multi_game_stats_are_finalizing` (0x5862E0 `mov edx,[Mp_game_data]` = NULL,
0x5862E6 `cmp byte [esi+edx+0x610],0` with esi = 0), called from `multi_game_stats_record_weapon_time` 0x5A3080, called
from `player_do_frame` 0x6E6B70. That crash with the key at 0 and none at 1 (the default) proves PP-STATS is the fix.

### Data written every tick (0x1D24D9, after the first F6, only while `Multiplayer` == 0)

- `Clock_current_time` 0x02BEA538 = 10000 + QPC ms since the first F6 (unsigned, monotonic). Its readers are all MP
  code, `add_state`, `obtain_state`, `apply_movement_state` and `multi_timestamp` (xrefs). Written after the F6 check,
  so the capture of the enabling frame already carries a valid stamp.

### Cave assembly (AT&T, `src/puppet.cpp`; `gc_*` are C globals holding runtime addresses)

```
AirCave:                         ; entered by jmp from 0x653124, esp unchanged
  push ecx / mov ecx,[gc_mp] / mov al,[ecx] / pop ecx      ; the replaced `mov al,[Multiplayer]`
  test al,al / jne back                                      ; MP: original flow
  cmp esi,[gc_puppet] / jne back                             ; not the puppet: original (SP: stick)
  inc [gc_airHits] / jmp [gc_airMp]                          ; 0x65315C: its own +0x1180/+0x117C
back: jmp [gc_airBack]                                       ; 0x653129: test al,al / je 0x65316E

MeleeCave:                       ; entered by jmp from 0x67ED30
  mov ecx,[gc_mp] / cmp byte [ecx],0 / jne mp                ; MP: original MP path
  test eax,eax / je sp                                       ; not a player
  cmp eax,[gc_puppet] / jne sp                               ; you (and anyone else): camera path
  inc [gc_meleeHits]
mp: jmp [gc_meleeMp]                                         ; 0x67ED39 (cmp eax,edi / ... +0x1250)
sp: jmp [gc_meleeSp]                                         ; 0x67ED64 (Rfg_camera+0x68)

CrouchThunk:                     ; called from 0x630B07 / 0x680783 instead of camera_free_set_submode
  push eax / mov eax,[gc_player] / cmp esi,[eax] / pop eax   ; esi = the crouching human; gc_player = &Player
  jne skip / jmp [gc_submode]                                ; yours: the original, args untouched
skip: inc [gc_crouchSkips] / ret                             ; anyone else: your camera keeps its framing
```

The built DLL was disassembled: the three stubs assemble as above, the `gc_*` operands resolve to the right .data
slots, `.reloc` is present and the DLL is DYNAMIC_BASE (it shares the 0x10000000 preferred base with other
proxy DLLs). `OnPlayerDoFrame` keeps ecx and tail-jumps; `OnAnimDirect` tail-jumps with the caller's stack untouched;
`OnHumanDamage` saves ebx/esi/edi/ebp and returns in st0.

## Puppet creation and teardown

Recipe copied from `player_multi_registration::process_create_packet` 0x5E0657-0x5E07A3 (and
`wrecking_crew_spawn_players` 0x39F1A2-0x39F1FB), minus the MP-only `MULTI_PLAYER_HANDLE` / `MULTI_TEAM` props:

1. `obj_human_props::obj_human_props(props, Player->h_info, &pos, &orient, NULL)` 0x6529A0
2. `world::create_object_internal(&World, player::get_classname_hash(), props, NULL, -1)` 0x53E550 / 0x610140
3. `human_set_state(p, HS_STANDARD, HMS_IDLE, -1.0f)` 0x607520 (the float is `[0xEB8824]` = -1.0f)
4. vtbl+0x30 `update_pos_and_orient(&pos, &orient, true)`
5. `human_set_state` again, `player_set_zoom_state(p, 0, false)` 0x6851A0, `object_set_visibility_plus_children(p, true)`
   0x68C070, `rfg_prop_block::~rfg_prop_block` 0x5217D0
6. vtbl+0x88 `human::set_team(HUMAN_TEAM_GUERILLA)` (MP calls it the same way at 0x5E07A3)
7. `+0x54 |= 0x20` (object_flags.no_save, PDB), `+0xDCC |= 0x10` (player_flags.never_die, never 0x20),
   `multi_sim_state::reset` on `+0x1274`, `+0x1268 = 0`
8. list index in `World.subtype_objects[3]` (`World+0x3F4` = uint16 indices into `World+0x64`, count `+0x3FC`, layout as
   read in `player_level_shutdown_all` 0x69DD01-0x69DD13). Index 0 -> destroyed again and refused (log): with the
   0x3EDDC6 store NOPed, entry 0 is the one whose stance sets the audio listener height, and it must be you.

Teardown: `player_level_shutdown(player*)` 0x69DCA0 = ragdoll unbind, `player::disband_ally_and_escort_squads`,
`world::destroy_object(p, false)`: the game's own per-player teardown, used by MP `multi_game_remove_player`
(0x5BC531). It never writes `Player` (only `player_level_shutdown_all` does).

Existence: the handle is re-resolved with `world::object_from_handle` every tick and every gameplay frame; the puppet
counts as alive only if it resolves to the same pointer, is not `Player`, is type 0 / subtype 3 and not flagged
destroyed. Otherwise the per-puppet patches are restored, the ring, `g_last` and `g_driving` are cleared and the tick
recreates it when `Player` exists, the byte `[[Player+0x32C]+0x34] != 0` (`character_instance.rdd.m_loaded`, a bool:
`player_process_all` 0x6E7366 `cmp byte ptr [eax+0x34],0`; `set_loaded` 0x3FA520 / `is_loaded` 0x1F5960 touch only that
byte, +0x35..+0x37 is padding) and F6's refusals all pass (`Refused`: MP, `wrecking_crew_is_active`, `+0xAC4 != -1`).
In WC `wrecking_crew_process` (0x3A6BF0-0x3A6C1C) would hide and teleport every non-turn player each frame; in a vehicle
the new ghost would never hide.

## The state: capture (sender) and delivery (receiver)

Built as `player_multi_registration::get_state` 0x5F49C0 builds it for a local player (read in full), with two SP
substitutions: the aim and the stick.

| Field | Source |
|---|---|
| position, orientation | Player +0x04, +0x10; ragdoll ACTIVE (+0x33C == 1): `havok_ragdoll_get_pos(+0x344, &pos, 1)` as 0x5F4BA8; then + `ghost_offset` on X |
| linear velocity | +0x270 `actual_velocity`; ragdoll: `havok_ragdoll_get_vel` as 0x5F4BEA |
| angular velocity, camera angular velocity | 0 (as get_state 0x5F4BF5 for the body) |
| camera_orient | `Rfg_camera` 0x19E3B50 +0x50 `real_orient` (what MP's local control copies into +0x1238 at 0x2FC5B8, MP-only) |
| forward / sideways | args 3 / 2 of the local `player_move_set_anim_direct` call (0x2FC841) this frame, else 0 (ladder, cover) |
| hit_points_pct | +0x914 / +0x910 |
| ladder_handle, ladder_state | +0xB18; when +0xB8 bit 15: synthesized as mode 1 (+0xA2) and `ladder_compute_current_rung(Player, +0xB18)` (+0xA3), what MP's sender writes on mount and during a normal climb (0x6B894E, 0x6C513C). Not +0x239C: every store to it is behind `Multiplayer` (0x6123B0, 0x6B8945, 0x6B8D0C, 0x6C484B, 0x6C4F18), so it never changes in SP. The receiver's climb stick comes only from these two bytes (`compute_ladder_stick_value` 0x55F940: mode 1 = +1 below target-2, -1 above target+2, else 0) |
| ragdoll_state, knockdown_timestamp | +0x33C; `timestamp::time_until(+0x920)` 0x18E640 |
| turret / vehicle handles | net 0xFFFF, cache dword -1, in **every** state; seat 0 (the ghost never enters a vehicle) |
| bits 0-2 equip slot | `[+0xA30 desired_equipped_inv_item]+0x10`, 7 = none |
| bit 3 sprint | +0xDCC bit 6 (`do_sprint`) |
| bits 4-5 jump id | +0xC4 bits 9-10 (bumped unconditionally by `human_jump`) |
| bit 6 ladder | +0xB8 bit 15 |
| bit 7 reload | `human_weapons_is_reloading(Player, false)`; the receiver turns the level into one reload (edge latched in +0xDD0 bit 9, 0x58232D-0x582367) |
| bit 8 jetpack | +0xDCC bit 8 |
| bit 9 zoom | `player_is_zoomed(Player)` |
| bits 10, 12 | +0xB4 bit 24, +0xC4 bit 22 (as get_state) |
| bits 11, 13 detonate in air / detonated | +0xC4 bits 23 / 24 (set at 0x301913 / 0x301089, cleared in SP only by MP code). The capture clears them on your Player after reading, so each detonation is a one-frame edge; `Effects` (when Clock reaches the state's network_time, capture + 400 ms) then calls `detonate_in_air` / `detonate_charges(puppet handle)` itself, as MP's `read_control_flags` does for a remote player (0x5DD713 / 0x5DD6AF). The receiver's copy into the ghost's +0xC4 only feeds `gameplay_register_player_attack` (WC-only) |
| bits 16-23 melee ids | +0xC4 bits 1-8 (advance in SP only through L5) |
| bits 24-25 / 26-27 trigger down / up ids | synthesised, see below |
| bits 28-29 throw id | +0xC4 bits 18-19 (bumped unconditionally by `human_weapons_start_throw`) |
| bit 30 crouch | `human_move_is_crouching(Player)` |

**Trigger rule** (`apply_weapon_state` 0x5824AD-0x58268E, for ammo types bullet/projectile/electricity/repair/blanks/
flame): `semi` = trigger_type SINGLE or class GRINDER, `auto` = trigger_type AUTOMATIC, `changed` = state down id !=
puppet +0xC4 down id, `held` = state up id != state down id. Fire when `semi && changed` or `auto && (changed || held)`,
unless `semi` and the weapon is already pulled; otherwise release if pulled. An auto weapon that is already pulled
syncs +0xC4 down = state down (0x582585). The puppet's own pull bumps its +0xC4 down id only for SINGLE/CHARGE (L5).
So the capture keeps its own counter: SINGLE/CHARGE add the delta of Player +0xC4 down id (one per shot through L5);
AUTOMATIC adds one per press of `human_weapons_is_trigger_pulled` (weapon+0x18C bit 1), because MP's auto bump at
0x6BDFF9 (`human_weapons_firearm_process_common`) is another `Multiplayer` gate that L5 does not open. Up = down - 1
while an AUTOMATIC trigger is held, else up = down. MP itself releases the up id one frame after the last trigger frame
(`human_weapons_process_weapons_post` 0x6CC740-0x6CC774).
While you man a turret (`human_combat_get_manned_turret(Player)` non-NULL) nothing is counted: `get_weapon`,
`is_trigger_pulled` and `is_reloading` read the turret's gun (0x65A126), L5 bumps your +0xC4 for a SINGLE turret
(0x6CC4AA), and the ghost holds nothing (manning unequips, +0xA30 = 0 at 0x653846, so slot 7 and `apply_weapon_state`
skips all trigger logic). The previous values are re-baselined every frame, and in Capture's vehicle branch too, so no
burst fires when you step off or get out.

**Delivery** happens in two steps over the same ring, before each simulation. A cursor `g_sent` counts the entries
from the head that are already in the sim buffer.

*Arrival* (`Deliver`, for every entry whose capture time is >= 300 ms old):
- the first delivered state seeds puppet +0xC4 melee, jump, trigger and throw ids from the state (no spurious action;
  nothing is applied before Clock reaches its `network_time`, see the gate below);
- the first visible state after hidden ones (back from a drive) resets the sim state first, so the ghost pops to you
  instead of interpolating across the drive. This stays at arrival: done later it would wipe the states queued since;
- `network_time = capture + 300 + 100`, clamped to never go backwards;
- thinning: a visible state is not added while the sim buffer is non-empty and its time is < the last added time + 5 ms
  (it stays in the ring for its Effects). The engine buffer keeps 25 states (`add_state` drops the oldest at 0x568B8C),
  so they then span >= 120 ms, more than the 100 ms interpolation delay, at any frame rate. The ids are cumulative 2-bit
  counters the next added state carries. The hidden stop state is never thinned: without it the hidden ghost would
  extrapolate your last move and keep a held AUTOMATIC trigger firing;
- then `add_state`.

*Effects* (`Effects`, once the entry is >= 400 ms old, i.e. Clock has reached its `network_time` and `process_state`
applies it: `obtain_state` 0x5E1CBC-0x5E1CDF takes the newest state with time <= Clock, which the gate below
guarantees exists, and `interpolate_state`
0x563B9B copies that older state's discrete bits). Running these at arrival would put them 100 ms (`kInterpMs`) ahead of
the replayed movement and ids: a shot fired just before a switch would replay with the new weapon, a pending id would
fire later with the wrong one, and the ladder re-equip below would be refused. Then the entry is popped:
- visibility follows the entry: entering a vehicle pushes one stop state (zero input and velocity, no sprint, trigger
  released) marked hidden, and no states while you drive; the first state after you get out shows the ghost again;
- weapon switch when the item or slot changes: find the `inv_item_info*` in the puppet's inventory (+0xA2C circular
  list), else `inv_item_get_item_in_slot` + `human_delete_item_from_inventory` + `human_add_item_to_inventory(p, info,
  1, 99999, slot, false, false, false)`, then `player_equip_item(p, item_or_NULL, NULL, true)` as MP does (0x5F95AB-0x5F95C4).
  The switch is recorded only when `player_equip_item` returns true. It returns false only while the puppet is HS_LADDER
  (`+0x2F4 == 5`, 0x6C2A23-0x6C2A2E; already equipped and NULL return true), so a refused switch is retried with the
  next state, as MP re-equips on every packet. The retry finds the item already in the inventory. This covers the
  ladder: your dismount restores your weapon (`human_dismount_ladder` 0x6C4027) while the ghost may still be climbing;
- zoom as MP (0x5F95CC-0x5F9601): `player_set_zoom_state(p, PZS_FINE_AIM, false)` or `(p, PZS_OFF, false)`;
- bit 13: `detonate_charges(puppet handle)`, bit 11: `detonate_in_air(puppet handle)` (the stop state masks both).

Captures are in Clock order, so anything old enough for its effects was delivered by the arrival loop first. A full
ring (512 entries; 400 ms is ~120 at the 300 fps limiter) drops the oldest and moves `g_sent` with it.

Then `multi_game_player_process_state(puppet)`, but only when the oldest buffered state (sim state +0 count, states
from +4, time at +0x48: `obtain_state` 0x568C4E) is <= Clock. With none, `obtain_state` falls through to 0x568C5F and
copies the NEWEST state, a future one (~Clock + 100): the replay would run 100 ms ahead, then step back and re-fire the
ids the newer state already put into +0xC4 (a second burst, jump or throw). Without the gate that happened for 100 ms
after creation, after every vehicle exit (sim reset) and continuously above ~240 fps. Skipping equals the empty-buffer
case (`obtain_state` false, 0x5E1CE6) except that the +0xDD0 bit 3 / bit 7 latches (set only by `apply_weapon_state`,
0x58264D / 0x582705, consumed at 0x5E1C66-0x5E1C93) wait for the next apply. With `l5=0` it is followed by
`obtain_state(Clock)` and a copy of that state's melee ids into +0xC4 bits 1-8 and its trigger-down id into bits 14-15
(the research's writeback fallback), behind the same gate.

## Game calls and their checked ABI

| Function | RVA | ABI used | Evidence |
|---|---|---|---|
| `multi_game_player_process_state` | 0x5E1C50 | cdecl `(player*)` | prologue `sub esp,0xB8; push esi; mov esi,[esp+0xC0]`, plain `ret`; task says verified |
| `multi_sim_state<multi_player_state>::reset` | 0x568B20 | thiscall | body `mov [ecx],0; mov [ecx+0x10D0],-1; ret` |
| `::add_state` | 0x568B40 | thiscall `(state*)`, `ret 4` | body read: rejects `newest.time > new.time` and `|time - Clock| > 5000` (time 0 skips), shifts at 25 |
| `::obtain_state` | 0x568C20 | thiscall `(unsigned, state*, bool*)`, `ret 0xC` | body read; call site 0x5E1CB3-0x5E1CDF; with no state <= t it copies the newest |
| `obj_human_props::obj_human_props` | 0x6529A0 | thiscall, 4 args, `ret 0x10` | body; call sites 0x5E0670, 0x39F1AB |
| `player::get_classname_hash` | 0x610140 | cdecl `()` | call sites 0x5E06C8, 0x39F1B9 |
| `world::create_object_internal` | 0x53E550 | thiscall `(&World, hash, props, zone, parent)`, `ret 0x10` | `ret 0x10` x3; call sites 0x5E06D3, 0x39F1C4 |
| `human_set_state` | 0x607520 | cdecl `(human*, int, int, float)` | call sites 0x5E06ED, 0x39F1E3 (`add esp,0x10`) |
| vtbl+0x30 `human::update_pos_and_orient` | 0x69AB90 | thiscall `(vector*, matrix*, bool)` | call site 0x5E0708; PDB |
| vtbl+0x88 `human::set_team` | 0x607BA0 | thiscall `(int)` | call site 0x5E07A3; PDB |
| `player_set_zoom_state` | 0x6851A0 | cdecl `(player*, int, bool)` | call sites 0x5E0727, 0x5F9601; HUD and camera parts gated on `ebx == Player` (0x6852B5, 0x6853AF) |
| `object_set_visibility_plus_children` | 0x68C070 | cdecl `(object*, bool)` | call site 0x5E0744 |
| `rfg_prop_block::~rfg_prop_block` | 0x5217D0 | thiscall | call sites 0x5E076B, 0x39F1FB |
| `player_level_shutdown` | 0x69DCA0 | cdecl `(player*)` | body (`mov esi,[esp+8]` after `push esi`, plain `ret`); call site 0x5BC530 |
| `world::object_from_handle` | 0x53BEE0 | thiscall `(&World, unsigned)` | used in game; many call sites |
| `hud_message_add` | 0x455D30 | cdecl `(char*, float, void*, void*, int*)` | used in game |
| `wrecking_crew_is_active` | 0x3603B0 | cdecl `()` -> al | call site 0x6E6294 |
| `human_weapons_get_weapon` | 0x65A120 | cdecl `(human*)` | prologue `push esi; mov esi,[esp+8]`; call site 0x6CC87F |
| `human_weapons_is_trigger_pulled` | 0x65A5F0 | cdecl `(human*)` -> al | body (weapon+0x18C bit 1); call site 0x582681 |
| `human_weapons_is_reloading` | 0x65A380 | cdecl `(human*, bool)` | prologue; call sites 0x5F4D6C, 0x582628 |
| `player_is_zoomed` | 0x6106C0 | cdecl `(player*)` | body (`+0xD58` not OFF/SPRINT) |
| `human_move_is_crouching` | 0x60B040 | cdecl `(human*)` | body (`+0x2FC == 1`) |
| `havok_ragdoll_get_pos` / `_get_vel` | 0x41CBA0 / 0x41CC20 | cdecl `(int, vector*, int)` / `(int, vector*)` | PDB; call sites 0x5F4BA8 / 0x5F4BEA |
| `timestamp::time_until` | 0x18E640 | thiscall `()` -> int | body (no stack args, plain `ret`); call site 0x5F5029 |
| `human_combat_get_manned_turret` | 0x655320 | cdecl `(human*)` -> turret* or NULL | prologue `push edi; mov edi,[esp+8]`, plain `ret`; call site 0x65A126 (`push esi; call; add esp,4`) |
| `detonate_charges` | 0x6C51E0 | cdecl `(unsigned owner_handle)` -> al | prologue `sub esp,8; push ebx; mov ebx,[esp+0x10]`; call sites 0x30107D, 0x5DD6AF, 0x6E06F2 (`push handle; call; add esp,4`); used in game |
| `detonate_in_air` | 0x6E4DE0 | cdecl `(unsigned owner_handle)` -> al | body: 4 pushes, `mov ebp,[esp+0x14]`, plain `ret`; detonates projectiles with info flag 0x10000 and +0x13C == handle; only call site 0x5DD713 (`push eax; call; add esp,4`) |
| `inv_item_get_item_in_slot` | 0x612330 | cdecl `(human*, int)` | prologue; call site 0x5F95B6 |
| `human_add_item_to_inventory` (inv_item_info* overload) | 0x6B4E20 | cdecl `(human*, inv_item_info*, int, int, int, bool, bool, bool)` | PDB picks this overload (0x6B52A0 takes `char*`, 0x6B52C0 takes `item*`); used in game |
| `human_delete_item_from_inventory` | 0x6C76A0 | cdecl `(human*, inventory_item*, bool, bool)` | used in game |
| `player_equip_item` | 0x6C2770 | cdecl `(player*, inventory_item*, item**, bool)` -> al | used in game; MP call site 0x5F95C4 (`item_or_NULL, 0, 1`); body: 1 when already equipped (0x6C277D), 0 only at HS_LADDER (0x6C2A23), else 1; the SP path before that check only calls `gamelog_maybe_log_weapon_equipped`, which is local-player only |
| `ladder_compute_current_rung` (handle overload) | 0x636880 | cdecl `(human*, unsigned ladder_handle)` -> int | PDB (the other overload 0x6366E0 takes `ladder*`); body reads `[esp+4]`, `[esp+8]`, handles -1 / NULL, returns `max(0, 6 - k)` in eax, plain `ret`; only caller 0x55F946 (`push handle; push human; call; add esp,8`) |
| original `gamestate_gp_simulate_frame`, `player::do_frame`, `player_process_object_interaction`, `player_is_killed`, `player_move_set_anim_direct`, `camera_free_set_submode` | see the table above | called through the previous rel32 target | |

Not called directly (register ABI, LTCG): `apply_movement_state`, `do_frame_movement`, `apply_weapon_state`,
`correct_player_movement`, `human_weapons_firearm_pull_trigger_common` (ecx = human, eax = weapon). They run only
inside `process_state` and the game's own frame.

## Log lines to expect (`RfgCoop.log`, prefix `ghost:`)

- load: `F6 toggles the ghost (l5 1, offset 2 m); nothing is patched before the first F6`; with `pp_stats=0` also
  `PP-STATS off (control run): expect AV at rfg+5862E6 reading 0x610 on the first puppet frame`
- first F6: any `unexpected bytes` / `not the expected call` line means it refused; else `hooks installed (...)`
- creation: `PP-STATS on (6E6B44)`, `listener store off (3EDDC6)`, `L5 melee open (6B19C8)`, `L5 trigger open (6CC4A3)`, `hash table 1290C60 primed`,
  `hash table 1290B88 primed`, then `created puppet <p> handle <h>; Player <P>; players 2; puppet list index 1;
  mp_data +0x126C = FFFFFFFF (expect FFFFFFFF); team 0; offset 2.0 m`
- first frames: `player::do_frame skipped for the puppet (#1..3)`, `player_process_object_interaction skipped (#1..3)`,
  `puppet weapon -> slot N item <p>`
- every 5 s: `player-puppet distance X m (offset removed), puppet states N, ring N, delivered N, visible 1, hp N; air N
  melee N crouch-skips N`. While you walk steadily the distance is about your speed x 0.4 s; standing still it goes to
  ~0. `puppet states` should sit near 25 (the engine's buffer).
- first hits: `PP-AIR: airborne puppet read its own command`, `PP-MELEE: puppet lunge along its own heading`,
  `PP-CROUCH: your camera framing kept`; `damage to it swallowed` when EDF shoot it; `ragdoll damage by it swallowed`
  after its explosion or knockdown swing floors someone.
- `puppet weapon -> ...` appears only when a switch took; a switch refused on a ladder is retried silently.
- after a load: `puppet ... is gone (level unload?)`, the restore lines, then a new `created puppet` line.

## Open risks for the in-game test

1. **Never run before.** A second simulated player in the campaign is new territory (E2 is the experiment). The first
   crash, if any, is most likely in `player_do_frame`'s non-local path past PP-STATS, or in `human_do_frame` for a
   non-local player-class human. The crash logger in core.cpp prints the RVA and stack.
2. **Creation from the tick.** The puppet is created from the 0x1D24D9 tick (the position where MP creates remote
   players inside `multi_process`). If something there expects to run inside gameplay processing, move `Create` into
   `BeforeSim`.
3. **Hiding while you drive.** `object_set_visibility_plus_children(false)` may be undone by the game's own opacity
   or visibility passes for players; the hidden ghost still has a body that AI can target and that blocks the spot
   where you got in.
4. **Charge-trigger weapons never fire on the puppet**: `apply_weapon_state` fires only SINGLE (or GRINDER) and
   AUTOMATIC trigger types; trigger_type CHARGE (2) only releases. Grinder, charge-up and multi-rocket upgrades
   (0x6CC13C, `upgrade_get_multi_rockets` reads your meta data) may behave oddly.
5. **Upgrades are yours**: every `upgrade_*` getter reads the local meta data, so the ghost has your upgrades; harmless
   here.
6. **Ghost weapons are real against the world**: bullets, explosions and the hammer damage buildings, vehicles and
   general movers (only `human_damage` is firewalled; `vehicle_apply_damage` is M8). AI can still be alerted by its
   shots (`damaged_by_player` needs damage > 0, so not from its hits on humans).
7. **Hammer swings while exiting cover** still use your camera (the 0x6B1A82-0x6B1AE4 path in `human_melee_attack`,
   research §9); normal swings and lunges are fixed by PP-MELEE.
8. **Turrets**: PP-TURX / PP-TURM (0x6C0A05, 0x6B5F89) are not in this build; the ghost never gets a turret handle
   (net id 0xFFFF), so it never mans one, but if it stands at a mounted turret nothing guards your camera. Shots you fire
   from a turret or a vehicle seat are not replicated; the ghost's trigger ids stay re-baselined while you are on one.
9. **Minor local-player leaks not patched**: steering seed from your camera (0x62D185), burning-car shake (0x6E696D),
   SP equip tooltips for propane/thermobaric/multi-rocket (0x6BC1AC), first-person spinebend.
   The E2 table's "log of every camera / HUD write inside the puppet window" is
   not implemented (too costly for this build); judge it by eye.
10. **Ladders**: the climb follows the synthesized mode-1 state (your current rung). A slide-down (mode 2, 0x6C5063)
    replays as a normal climb down that trails by up to 2 rungs. `ghost_offset` shifts only positions: the ghost mounts
    the same ladder 2 m away and the position correction may fight it; `ghost_offset=0` removes that fight (the ghost
    then walks into you when you stop) but is not needed for the climb itself.
11. **Trigger capture**: AUTOMATIC weapons rely on `weapon+0x18C` bit 1 staying set while you hold the trigger; a tap
    shorter than one frame cannot be seen. SINGLE weapons rely on L5 (`l5=0` falls back to counting presses; melee
    swings are then not replayed because your own melee ids never advance without L5).
12. **L5 side effect**: while the ghost exists, NPC melee and trigger ids also advance; only MP apply code reads them
    (two research judges); watch NPC melee and shooting for anything odd.
13. **High frame rates**: states are thinned to one per 5 ms (Arrival above), so above 200 fps the ghost interpolates
    between states 5 ms apart instead of one per frame; the 25-state buffer still spans >= 120 ms. A one-frame edge that
    lands only in a thinned state reaches the sim buffer up to 5 ms later with the next state (ids are cumulative);
    visibility, weapon, zoom and detonation are unaffected (per ring entry). `puppet states` in the 5 s line should
    sit at 25; `delivered` then grows by at most 200 per second.
14. **Puppet list index**: `gamestate_gp_simulate_frame` does not treat `subtype_objects[3][0]` as the local player
    (its per-player loop processes every entry); it keeps the LAST entry for the audio listener height (0x3EDDC6 ->
    0x3EE16B -> 0x3EE47A). With that store NOPed it keeps entry 0, so the ghost must never be entry 0: checked and
    logged at every creation, refused if it happens. If the 0x3EDDC6 byte check ever fails the ghost is refused.
15. **Saves**: the ghost carries `no_save`; a save with the ghost on, then a load, must leave exactly one player (the
    log shows `players 2` after recreation, 1 right after the load).
16. **Remote charges**: detonated when Clock reaches the state's `network_time`, with the replayed throw, not ahead of
    it. A detonation made from a vehicle reaches the ghost only on your next on-foot capture (the capture does not run
    while you drive). After a frame hitch several states' effects run before one `process_state`, which applies only
    the newest: a shot pending in an earlier state can then fire with the weapon of a later one (MP has the same limit).

## Partner gear (ver6.2)

The ver6.1 test showed two bugs in how the partner puppet looks. Research:
`research/findings/workflows/puppet_item_textures.md` and `puppet_backpack.md`. Both fixes are in `src/puppet.cpp`, plus
the PSTATE byte in `src/proto.h`. They cover the ghost too.

### Hammer: the partner's hammer 1-3 rendered white

**Cause.** This is a vanilla data bug.
- The low-res meshes of `sledge_upgrade_1..3` name `sledge_up_N_low_d.tga` / `_low_s.tga`.
- The always-loaded items peg (`items_containers.cpeg_pc`) spells them `sledge_up_N_d_low.tga` / `_s_low.tga`.
- `bm_load` finds neither name, so it makes a placeholder that shows the bogus static bitmap.
- Only your own held weapon streams its `_high` textures (the local branch of `player_do_frame`, 0x6E6989), and while
  loaded they override the shared mesh for every instance. The puppet takes the non-local path and renders low-res.
- So the white appears exactly when nobody local holds that hammer, for example right after B's Q17 pick dropped it.
- Every gun's low-res is complete. Hammers 4-7 and the golden one lack only the specular map (barely visible): left alone.

**Fix.** `Lifetime(true)` points each of the six placeholders at the correctly named peg entry, and `Lifetime(false)`
puts the saved value back. That covers creation, F8/F6 `Destroy`, a failed create and the game freeing the puppet at
level unload (`Puppet()` then `Detach`).

| Site | RVA | ABI / layout | Verified |
|---|---|---|---|
| `bm_load` | 0x1CA450 | `int __cdecl(const char*, bool dbg)`: looks the name up in `Bm_filename_hash_table` 0x1118C00, then in `Extra_bm_filename_hash_table` 0x1118CE0, else `bm_add_bitmap` (a placeholder, or -1 when the table is full) | body 0x5CA450-0x5CA4A5 (VA), plain `ret` |
| `Bm_bitmaps` | 0x18D194C | pointer to 12-byte entries, `+4` = peg entry. A peg entry's `+0x10` word = frame count and `+0x28` = device texture | `bm_get_entry` 0x163C10 (`lea [h*3]`, `[ebx+4*eax+4]`, frames `[ecx+0x10]`) |
| `Bogus_static_bitmap_peg_entry` | 0x18E7654 | pointer variable = `Bogus_static_bitmap_entry->+4`. Every placeholder gets this `+4` | `bm_init` 0x1D07A4-0x1D07AF, `bm_add_bitmap` 0x1C9B83-0x1C9B9C, `remove_from_entry_list` 0x1C707D |
| draw | `get_dev_texture` 0xF1DB0 | per bind: `bm_get_entry(h, 1)`, then `+4`, then `+0x28` | body 0x4F1DF8-0x4F1E17 (VA) |

**Guards.** An entry is touched only while its `+4` is still the bogus entry (a future data fix makes this a no-op), and
only if the target is a single-frame entry (`bm_get_entry` 0x163C3A animates multi-frame entries from the index). Each
write is one aligned dword, and both values are valid peg entries, so the render thread cannot read a torn pointer.

**Risk.** If the items peg were unloaded while the alias is on, the alias would point at freed memory.
`dlc_items_init` 0x3B9F30 unloads it at DotB start, but that runs from the front end, after the level unload has freed
the puppet and `Detach` has restored the entries. Never re-register `items_containers` while a puppet exists.

### Backpack: each side's puppet wore the local player's pack

**Cause.** `player::stream(true)` runs for every player the world creates, the puppet included. In SP outside WC it
asks `upgrade_get_backpack_type()`, which reads **your** table, creates that pack and attaches it to `this` (the
puppet). It also writes the global `s_lastBackpackIndex`, which could steal your own pending pack. Nothing changed the
puppet's pack after that: `player::do_frame` is skipped for it, and PSTATE had no pack field.

**Fix, in three parts:**
1. **The stream gives only you a pack.** 0x6C1CD4 calls `PackTypeThunk` (hook table above). Any other player gets 0x3E
   (none) while you exist, so there is no pack and no `s_lastBackpackIndex` write.
2. **PSTATE carries the sender's worn pack (protocol 5).** The spare byte after `det` becomes `pack`: the
   `Backpack_info` id + 1, 0 = none. The size stays 152.
   - `Capture` reads what you actually wear: `[[me+0x1170]+0x270]+0x50`. That covers the shop, every load, the Q17 pick,
     LOADOUT and None, with no change detection.
   - `Puppet_Out` sends it. `Puppet_In` accepts 0..10 and anything else becomes 0.
3. **The puppet wears exactly that pack.** `SetPack(p, id)` creates it with `multi_game_backpack_create(id, p+4,
   p+0x10)` and attaches it with `event_player_picked_up(pack, puppet, true, false)`, which drops the old pack first.
   - With id -1, or if the create fails, it drops any worn pack with `event_player_dropped(true)`.
   - If `picked_up` refused the pack (it attaches only a streamed one, `test [edi+0x55],0x10` at 0x5F23D3, before the
     drop at 0x5F2409, so the old pack stays worn), the pack is destroyed (`world::destroy_object(pack, false)`) rather
     than left lying in the world, and `SetPack` returns false. Every other outcome returns true (a create 0 is final).
   - Called from `Effects` once per change, on visible states of a **streamed** puppet only:
     `s.visible && s.pack != g_pack && (p+0x55 & 0x10) && SetPack(...)`, and `g_pack` is recorded only on true.
     Why the gate (ver6.2): visible only needs a loaded zone (`Puppet_In`, zones load to about ±510 m), but a new
     object streams in only inside the layer-0 cylinder (340 m, 170 m when fragmented): `create_object_from_stub`
     sets `+0x55 |= 0x10` at 0x53E479 only if `process_streaming_object` (0x52D2D0) passes. A pack made at an
     unstreamed puppet is unstreamed too and refused. The puppet streams in later through
     `process_streaming_objects`, and the first visible state after that applies the pending pack. Without the gate a
     far partner's pack (a respawn or a shop far away) was recorded as worn and never retried. In the rare corner of
     the ±340 m physics cube outside the cylinder the refusal repeats per ring entry until it attaches; it logs once
     per id.
   - Called from `Destroy` with -1 before `Detach` / `player_level_shutdown`, as MP does for a leaving player
     (0x5BA14B-0x5BA17D).
   - `Detach` resets `g_pack` to 0. A puppet kept across a relink keeps it.

| Function | RVA | ABI used | Evidence |
|---|---|---|---|
| `upgrade_get_backpack_type` | 0x375CE0 | `int __cdecl()`: 0 jetpack, 0x35..0x3D, 0x3E none | call 0x6C1CD4, result compared at 0x6C1CD9 |
| `multi_game_backpack_create` | 0x59AC50 | `multi_object_backpack* __cdecl(int id, vector*, matrix*)`. Returns 0 for id -1 or > 15 and for a NULL `Backpack_info[id]` (0x2BE7D18) | body 0x99AC50-0x99AC73 (VA), caller `add esp,0xC` (0x6C1D09) |
| `multi_object_backpack::event_player_picked_up` | 0x5F23D0 | `void __thiscall(player*, bool send, bool skip_attach)`, `ret 0xC` | body (arg 3 != 0 skips the attach, 0x5F23EA; HUD only when the player is local, 0x5F249C); call 0x6C1D0C-0x6C1D13 |
| `multi_object_backpack::event_player_dropped` | 0x5DF1C0 | `void __thiscall(bool)`, `ret 4`. Wearer `+0x274` NULL = nothing; else detach, `wearer+0x1170 = 0`, `+0x274 = 0`, destroy | body 0x9DF1C0-0x9DF2BE (VA) |
| `world::destroy_object` | 0x51CEF0 | `bool __thiscall(&World, object*, bool)`, `ret 8` | PDB; call 0x5DF2B0-0x5DF2B8 `push 0 / push esi / mov ecx,World` |

Offsets: worn pack `player+0x1170`, its info `+0x270` (`get_backpack_info` 0x2F69D0), and the id `info+0x50`
(`parse_backpacks_table` 0x5AFDA3). guest.cpp `ApplyPick` reads the same chain.

**Visual only** (ver6.2). The puppet's pack is never activated: activation comes only from the local control path.
From ver6.3 `PackEffects` activates it (section "Backpack effects P1 (ver6.3)"). Its pickup
HUD, backpack fade and xray camera effects are local-only. With the partner's real jetpack worn, the puppet shows
jetpack flames when the partner flies.

**Out of scope.** `player::stream` also gives the puppet **your** armor prop (0x6C1D1E). The levels match because the
guest gets the host's table (Q13). If they ever differ, use the same thunk pattern on that call.

### Log lines

- install: `hooks installed (... 6C1CD4, ...)`. A refusal names `player::stream backpack type call`.
- each creation: `puppet: 6 hammer low-res textures aliased (sledge_up_N_low_d/_s -> _d_low/_s_low)`. At F8 stop, F6
  off, partner leave or level unload: `puppet: 6 hammer low-res textures restored`. Fewer than 6 means a handle check
  failed: report the line.
- `partner: puppet backpack -> id N` (ghost: `ghost: ...`) at each change, and `-> id -1` at every `Destroy`.
  `FAILED (create 0): none` means a bad id (final). `FAILED (not attached), retrying` means `picked_up` refused it: the
  puppet keeps its old pack and the same id is retried silently until it attaches.

### In-game checks (A host, B guest)

1. Both "aliased" and "restored" lines appear, 6 each.
2. The reported case: A holds sledge_upgrade_2 and B picks sledge_upgrade_5. On B, A's puppet's hammer shows dark metal
   (low-res, softer than your own) instead of white.
3. B re-picks sledge_upgrade_1, then sledge_upgrade_3: on A, B's puppet's hammer is textured each time.
4. Optional proof on ver6.1: B picks _1 or _3, and A sees white although A never owned it.
5. Your own weapons show hi-res detail within about a second after the pick, the loadout restore and weapon swaps.
6. The partner's guns look as before (NPC quality).
7. B picks Rhino: A sees B's puppet with Rhino (`-> id 8`), B sees A's puppet with A's pack, and each side's own pack
   still works (B's Rhino charge, A's jetpack).
8. B picks None: no pack on B's puppet. A buys a pack at a safehouse: B's view of A updates.
9. After every load and re-join, your own pack is still on your back after the puppet appears. Check this right after
   B's Q17 apply.
10. The partner gets into a vehicle: the puppet and its pack vanish together, with no floating pack. On foot: both show.
11. F8 stop, F6 off, host quit, death and reload, then quit to the menu: no crash and no pack left lying where the puppet
    stood.

## Backpack effects P1 (ver6.3)

The ver6.2 test: "backpacks effects and animations are not synced". The plan is `backpack_sync.md` (evidence
`research/findings/workflows/bp_*.md`). P1 makes the puppet's pack do what the partner's pack does, for the Jetpack
(fuel only) and the six "quiet" packs: Fleetfoot, Stealth, Vision, Commando (Firepower), Heal, Tremor. Thrust,
Shockwave and Rhino are **not** activated on the puppet yet (P2). Code: `src/puppet.cpp` (`Capture`, `Puppet_Out`,
`Puppet_In`, `PackEffects`, `ShakeStopThunk`) and `src/proto.h` (protocol 6). Partner puppet only: the ghost's pack
stays as in ver6.2.

**How.** An MP client that receives a remote player's toggle calls the full `activate()` / `deactivate()` on that
player's pack (`toggle_activation` 0x5F8271 / 0x5F81EA), and everything built for a remote wearer then plays there. The
puppet's pack gets the same two calls, driven by PSTATE, at the state's replay time (`Effects`). The local-only parts
(ramp, activation shake, cover exit, zoom, X-ray and the Vision screen effect, Fleetfoot FOV, HUD, foley) test
`Player` and skip the puppet by themselves. User decisions (RESEARCH.md §8 Q19): the partner's Heal heals you within
5.8 m; until M6 his quakes and hammer blasts break your copy of the world (no G1 / G2); no G8; G9 only at M7a.

### Sites and ABIs

| What | RVA | ABI used | Verified |
|---|---|---|---|
| `multi_object_backpack::activate` | 0x5F2030 | `void __thiscall()`: `push ecx` ... `pop ecx; ret` (0x5F23C0); the WC path tail-jumps (0x5F23B7) | body read 0x5F2030-0x5F23C0: local gates `player_get_local_player` 0x5F203C and `object_is_local_player` 0x5F20E6; fuel -= cost only with uses_fuel (0x5F209D-0x5F20C3); `+0x254 = 1` (0x5F20CD); one-shot clear (0x5F2390-0x5F23A1); `+0x268` set (0x5F2337); `+0x2AC` read only in the jump_boost branch (0x5F22C6). clang `-S`: `ecx` = pack, no stack args |
| `multi_object_backpack::deactivate` | 0x59AA30 | `void __thiscall()`, plain `ret` (0x59AC4E). **No "is active" guard** (0x59AA46 only picks the ability block), so it is called only for a pack whose `+0x254` is set | body read 0x59AA30-0x59AC4E: release 0x59AA60; deactivatable branch 0x59AA6B (shake stop 0x59AA78, dual abilities, charge clear, Rhino cutter SP-or-server, landing dust on `+0x2AD`, puppet `+0xDBC = 0` 0x59AC0F, `+0x268 = -1` 0x59AC27); `+0x254 = 0` (0x59AC42) |
| `camera_shake_stop` | 0x2C6F40 | `void __cdecl()`: zeroes `Cur_camera_shake[0..4]` (0x1DE3554 VA); touches eax, ecx only | body 0x6C6F40-0x6C6F5F (VA) |
| G5 call site | 0x59AA78 | `E8` rel32 → 0x2C6F40, esi = the pack | `ExpectCall` at install, then `ChainRel32` (hook table above) |
| `event_player_dropped` → `deactivate` | 0x5DF1FE | deactivates (if `+0x254`) **before** it clears `+0x274` | 0x5DF1F4-0x5DF1FE: the swap of an active puppet pack is caught by G5 too |

| Offset | Meaning | Evidence |
|---|---|---|
| pack `+0x254` | activated (bool) | 0x5F20CD, 0x59AC42 |
| pack `+0x25C` | fuel 0..1 | 0x5F20A5-0x5F20C3; `process` mirrors the wearer's `+0xDBC` into it while the wearer jetpacks (0x5F3942-0x5F3950) |
| pack `+0x268` | `activated_time`, first dword = end tick | -1 from the constructor (0x57CD97), pool init (0x589F3E), `reset` (0x59A937) and `deactivate` (0x59AC27); a new tick only from `activate` (0x5F2337) |
| pack `+0x270` / `+0x274` | info / wearer | 0x5F204A, 0x5F2036 |
| pack `+0x2AC` / `+0x2AD` | Thrust direction up / landed | read at 0x5F22C6 / 0x59ABBA, jump_boost branches only |
| info `+0x248` | jetpack | `player_update_jetpack` 0x69E123 |
| info `+0x6EE` / `+0x6EF` | activatable / deactivatable | 0x5F2390 / 0x5F2398, 0x59AA6B |
| info `+0x198` | active jump_boost (`+0x150` active block + `+0x48`) | 0x5F224F |
| player `+0xDBC` | jetpack tank | burn 0x69E45B, idle refill from `+0x25C` 0x69E5A4 |
| player `+0x56` bit 7 | visible flag (the `effects_alr_attack` gate 0x57C8BF) | `bp_gap_1.md` |

### Protocol 6

`MsgPState` grows from 152 to 156 bytes:
- `uint8_t bpAct`: bits 0-3 activation counter, bit 4 active `+0x254`, bit 5 `+0x2AC`, bit 6 `+0x2AD`, bit 7 frozen
  (`kFrozen`: a still re-send, below);
- `uint8_t bpFuel`: `+0x25C` × 255, rounded (NaN = 0);
- `uint16_t pad`: 0.

Details in `session_impl.md`.

**Sender** (`Capture`, read only, every on-foot capture). The counter steps when the worn pack's `+0x268` end tick
changes to a value other than -1 on the **same** pack object. That catches every activation, including the Shockwave
one-shot, whose `+0x254` is back to 0 by the capture. Another pack object (shop, Q17 pick, load) re-baselines without
counting. `Puppet_Out` copies both bytes. Its still state (sent while you are paused per session.cpp `HostPaused`:
Esc, map, menus, death screen, all while Capture may still run; on the host also its join token, held while the guest
lands; or after 150 ms without a capture: a hitch) also sets bit 7: the level is then stale (the fuel too, except for the Jetpack: a paused tank does
not change). The vehicle branch sends the last capture with the current counter, so a link or load while seated (which
resets the last capture) gives no false edge when you get out.

**Receiver** (`Puppet_In`). The counter becomes an edge exactly as `det` does: `edge = have && (bpAct ^ seen) & 0xF`,
and the first state after a link or a partner READY only sets the base. `Snap::bp` = edge (bit 0) | level, up, landed,
frozen (bits 4-7 as sent), and `Snap::fuel` = bpFuel. Each packet is its own ring entry and `Effects` runs per entry, so the
5 ms thinning in `Deliver` loses nothing.

### `PackEffects` (sim thread, from `Effects`, after `SetVisible` and `SetPack`)

It acts only for the partner puppet, and only on a worn pack whose wearer `+0x274` is the puppet:
1. **Hidden** (in a vehicle, out of your zones, loading), **unstreamed** (`+0x55 & 0x10` clear), or the partner's new
   pack not worn yet (`g_pack != s.pack`, SetPack waits): an active pack is deactivated at once and a resync is armed.
   No quake or heal runs at a puppet you do not see. A **frozen** state (bit 7, the partner paused) does the same after the
   Jetpack step (2): otherwise its stale level, rewritten every state, would keep the pack on for the whole pause (a Tremor
   quaking every 0.3 s, a Heal healing you). The first live state resyncs a pack still on. A capture hitch of
   150 ms or more therefore turns the pack off and on once (glare and sound again): accepted. So does any pause of yours
   with a pack on, on the partner's screen (its co-op token keeps it in state 1, so it still takes your frozen states).
   - Deviation from the plan's code: it returned before the hidden test when `g_pack != s.pack`. Here those cases
     deactivate too, so an old pack never keeps running while the new one waits.
2. **Jetpack** (`info+0x248`): `+0x25C` and the puppet's `+0xDBC` = wire fuel on every state. Flight itself still rides
   state bit 8, but its flames now shrink and stop with the partner's tank. Nothing is activated. Frozen states too:
   `Still` keeps bit 8, so a puppet paused mid-flight hovers, and without the write it would burn its own `+0xDBC`
   (0.35/s, 0x69E45B) dry and sag while the partner's paused tank still has fuel.
3. Not activatable (`info+0x6EE`) or not in the P1 allowlist (ids 2, 3, 4, 5, 7, 9): nothing.
4. **Activate** on an edge, or on a resync while the partner's level is 1 and the pack is off (never a Thrust in
   mid-flight: `info+0x198`).
   - An edge whose level is already 0 is stale (states skipped in your menu, map or load, or a loss burst longer than
     an activation): no activation, so no late quake or heal blip. A real activation of a deactivatable pack is still
     on at the capture that counts it, and since ver6.4 that capture is sent at once (`Puppet_Step`, P2 below), so
     the PSTATE carries level 1 too; one-shots (`info+0x6EF` clear, Shockwave in P2) keep their level-0 edge. ver6.4
     also makes a step after your menu, in a state 250 ms+ late (or due after 250 ms without a puppet frame) or after a 5+ state loss burst `kStale` (P2 below).
   - An active pack is deactivated first.
   - `+0x2AC` = wire bit 5 before the call; fuel = wire after it (activate subtracts its cost).
5. **Deactivate** when the level is 0 and the pack is on (`+0x2AD` = wire bit 6 first).
6. Otherwise fuel = wire. This drives the glow, and the puppet's own drain between states ends the pack within one state
   of the partner. A level 1 without an edge never re-activates, because the local fuel-out may end the puppet's pack
   one state early.

A resync is armed by a new puppet (`Detach`), a link (`Puppet_Net`), a partner READY, a pack change (`SetPack` success)
and every hidden state. It is cleared after the first visible state of an allowed pack. One-shots missed while away
are lost by design.

**Side effects of `activate` / `deactivate` on the puppet** (`backpack_sync.md` §4.2):
- `player_stop_material_effect` (0x5F2349) wipes any material effect on the puppet;
- `player_set_player_invulnerable(puppet, false)` does nothing;
- `deactivate` zeroes the puppet's `+0xDBC`, and the next state restores it;
- the Heal heals **your** Player within 5.8 m (`collect_objects<player>`, SP-or-server 0x5F2925; each side owns its
  own HP);
- Tremor quakes (`use_kaboom`) and the Commando hammer blast break your copy of the world (the "now" policy until M6);
- Tremor's `explosion_quake` camera shake reaches you, as it reaches MP clients;
- damage by the puppet stays swallowed (`OnHumanDamage`);
- `activate` (0x5F20DA), `event_player_dropped` (0x5DF205), `event_player_died` (0x5F25BF), `reset` (0x59A914) and
  `human_ragdoll_start` (0x6C9721, SP-or-server) call `stop_ramp_up` 0x57CF80 on the puppet's pack, whose
  `camera_shake_stop(shake)` 0x2C6ED0 (call at 0x57CFB0) clears every slot holding that pack type's ramp shake: when
  you wear the same pack type, your own 300 ms ramp shake can end early. Cosmetic and MP parity (an MP client's
  `toggle_activation` does the same), not gated. If it ever matters: `ExpectCall(0x57CFB0, 0x2C6ED0)` and a thunk that
  skips the call when `[esi+0x274]` (esi = the pack, 0x57CF81) is the puppet (plain `ret`: the caller pops the argument
  at 0x57CFB5), one site for every caller.

### Log lines

- install: `hooks installed (... 6C1CD4 59AA78, ...)`. A refusal names `G5 backpack deactivate camera_shake_stop call`.
- `partner: puppet pack on id N, visible bit B` per activation, and `on id N (resync), visible bit B` when it came from
  a resync (late join, relink, re-join, pack change, unhide). B is `+0x56 >> 7`: expect 1.
- `partner: puppet pack off id N` when the partner's level drops, `off id N (hidden)` when the puppet hid, and
  `off id N (partner paused)` on a frozen state.
- `partner: jetpack fuel written (F)` once per flight (rising edge of state bit 8). F is the partner's tank then.
- `partner: G5: your camera shakes kept as its pack ended (hit N)`: the first three hits of the thunk, in the 5 s
  stats.

### In-game checks (backpack_sync.md §6 P1; A host, B guest, every check both ways)

1. B flies until the jetpack is empty: on A the flames shrink and stop when B's do, never earlier.
2. B Fleetfoot: limb and pack contrails, the loop sound, and the puppet keeps pace without snapping. B toggles it off:
   the contrails stop.
3. B Stealth: B fades to a shimmer in about 1 s and back in about 0.75 s when it ends (toggle or 12 s). B fires while
   stealthed: still stealthed on both screens.
4. B Vision: glow on B's pack and gun. A's screen gets no X-ray, blur or noise.
5. B Firepower: glow on pack and gun, red glares, red tracers from rifle, SMG and pistol (none from the shotgun), a
   fiery puff at each impact, hammer hits that blast (at most twice a second) and break A's world as B's charges do. It
   ends on both screens about 6.7 s after the start. A standing within 3.75 m of B's hammer hit is not knocked down and
   loses no health (a red hit marker may show, Q3).
6. B Heal next to a hurt A: ring and body shimmer on B, and A's health rises.
7. B Tremor near A: haze, sound, A's camera shakes every 0.3 s (less with distance), the ground crumbles on A as on B.
8. A is in an explosion shake when B's Fleetfoot or Firepower ends: A's shake continues (G5; its log line shows).
9. B enters a vehicle, drives out of range or loads during Stealth or Tremor: the effects stop on A at once (`off id N
   (hidden)`). B comes back mid-Stealth: shown stealthed (`on id 3 (resync)`).
10. Throughout, A's own pack, fuel HUD, camera, aim and controls stay unaffected by B's packs. 20 min, no crash.
11. If the glow shows but the activation glares or the attack sound are missing, look for the `puppet pack on` line and
    its visible bit (expect 1), and check whether the puppet was hidden at that moment (the one-shots are then lost
    until the unhide resync). Missing loops or materials as well means `activate` never ran.
12. A screenshot of your own Firepower settles the colours (`backpack_sync.md` §7).
13. A turns on Firepower and shoots B's puppet: the fiery puff shows, plus normal blood (confirmed in game, ver6.3.1).
14. B uses Thrust, Shockwave or Rhino: nothing new plays on A yet (P2), and there is no `puppet pack on` line for ids
    1, 6 or 8.
15. B starts Tremor or Heal, then presses Esc or opens the map: on A it stops at once (`off id N (partner paused)`).
    B closes it while the pack still runs: it comes back (`on id N (resync)`) and ends with B's. The reverse (A pauses
    with a pack on) gives B one `off id N (partner paused)`, then `on id N (resync)` at resume: accepted.
16. A opens the map while B runs a whole Heal or Tremor and closes it after B's ended: no quake or heal blip on A and
    no `puppet pack on` line.

## Backpack effects P2 (ver6.4)

P2 of `backpack_sync.md` (§6): the puppet's pack now also activates for **Thrust** (id 1), **Shockwave** (6) and
**Rhino** (8), so every pack the partner uses plays on the puppet. These three run gameplay on your machine (SP is the
server), so four gates come with them. Code: `src/puppet.cpp` (`kPackAllow`, `RippleCave`, `RippleHumanThunk`,
`RamFilter`, `EndCutterThunk`, `Install`, `PackEffects`, `Puppet_Net`, `Puppet_In`, `Puppet_Out`, `Puppet_Step`,
`Stats`), plus `session.cpp` `SendState` and `proto.h` for `Puppet_Step`. Protocol stays 6: every
field P2 needs (counter, level, Thrust up and landed bits, fuel) is already in `bpAct` / `bpFuel`.

**What `activate` brings now** (no new code; `backpack_sync.md` §2.1, §2.6, §2.8, §5):
- Thrust: the launch (`human_jump` on the ground, 0x5F22AC), the boost 180 ms later (`apply_jump_boost` 0x557100:
  vy = max(vy, 2) + 10, down-thrust vy - 10), `mp_thrust_loop` / `mp_thrust_down` from wire bit 5, the attack sound,
  steel sparks and no damage taken; landing dust from the puppet's own landing end (0x5F38F5) or wire bit 6.
- Shockwave: the ring `mp_shockwave_active_a` and its sound, the radial impulse on loose objects (6 m), the blast
  `mp_shockwave_blast` (breaks your copy of the world: no G1 before M6).
- Rhino: the `mp rhino` pose (state 0xF1 + override flag, set by `activate`, cleared by `deactivate`), 8.2 m/s, the
  shield, engine flames, arc, `invulnerability` material and sound, steel sparks.
- The three sounds come from effect `<sound>` fields: positional `_MONO` cues at the puppet (no new code).

### Gates (sites, bytes and ABIs: hook table above)

| Gate | Site | Acts when | Effect |
|---|---|---|---|
| G3 | 0x5F18CA `use_ripple` | wearer `[ebp+0x274]` = puppet, `gc_ppWorld` bit 2 clear | the ripple goes to 0x5F1E7B: loose objects only, no NPC or vehicle ragdoll or damage (an MP client's path) |
| G4 | 0x5F1942 SP ripple loop | a puppet exists and the victim is you or the puppet | the victim reads as the wearer, so the loop skips it: no co-op knockdown by any ripple, both directions. Always on |
| G6 | imm32 0x5F3188 ram cast filter | cast data+0 (wearer) = puppet, the object is not the wearer | players: rejected always; NPCs: bit 2; vehicles: bit 4; movers (the in-charge cutters): bit 1; else the game's filter |
| G7 | 0x59ABA7 `deactivate` end cutter | wearer `[esi+0x274]` = puppet, bit 1 clear | no end-of-charge cutter (Rhino and Thrust). Hidden/frozen/away deactivations and the session-down drop make no end cutter (`gc_cutterSkips` counts them): `PackEffects` and `Destroy` clear bit 1 for those calls only, since the partner's charge goes on (`deactivate` builds it for any end, 0x59AAAE-0x59ABA7) |

`RippleCave`, `RippleHumanThunk` and `EndCutterThunk` are naked (AT&T in `puppet.cpp`). The built DLL was
disassembled: they assemble as written and the `gc_*` operands resolve to `.data`. `RamFilter` is a C++ `__cdecl`
returning bool (the caller reads al) and calls the game's filter as `uint32_t & 0xFF`. All four sites are checked at
`Install` with the others: one mismatch and nothing is installed.

### Policy (`gc_ppWorld`, one value per role)

Bits: 1 world (G6 movers, G7; G1 / G2 at M6), 2 NPCs (G3, G6), 4 vehicles (G6; the ripple's vehicles follow bit 2,
because the SP ripple branch does both, §2.6). `Puppet_Net(true)` sets it from `kPolicyHost` / `kPolicyGuest` (role:
`Guest_Armed()`, which session.cpp `Link` arms on the guest before that call). Now both are **1** (Q7 / Q19: the
partner's Rhino and Thrust holes break your copy of the world; his ram and ripple never touch your NPCs or vehicles).
M6: guest 0 (the host's destruction stream carries the holes); M7a: host 3; M8: host 7. The ghost (dev F6) keeps the
default 1 and never activates its pack.

### Thrust: no double launch (R4)

`activate` jumps a puppet on the ground, and the partner's own launch is also in the same state's jump id, which
`apply_movement_state` replays (`human_jump` 0x5CF433) while it differs from `+0xC4` bits 9-10 (0x5CF418-0x5CF42A).
When both machines agree, `activate`'s `human_jump` bumps the puppet's id to the state's and nothing replays. But
`human_jump` bumps the id **only when it jumps** (0x6C75DA-0x6C75FE): in the air it only sets `+0xDCC` 0x200
(0x6C731D); within the player's 1.1 s cooldown (`+0x24C0`, 0x6C7379) or outside human state 0 / 5 (0x6C74E1) it returns
early. So where the two machines disagree (the puppet still in the air or already landed, a cooldown edge) the ids
differ and the replay calls `human_jump` every state until it succeeds: a second launch after the Thrust lands, or up to
three (+1 per jump, mod 4) when the puppet's own launch was the extra one. Hence, right after `activate` on a pack with
an active jump_boost (`info+0x198`), `PackEffects` sets the puppet's jump id to this state's: one launch, plus the
boost. The plan said "if it doubles in the test"; the code read shows it can, so the fix is in from the start
(deviation).

### Stale step after your menu, a late state or a loss burst

`Puppet_In` drops states while you are in a menu, the map or a load (`GameState() != 1`), so the first state back can
carry a counter step from meanwhile. Taken as an edge, a Shockwave fired meanwhile (one-shot, level 0) would blast late
at the puppet's current place, and a Thrust still in flight would launch the puppet late. Now such a step (first seen
with `g_bpAway` set) goes into the ring entry as `kStale` (Snap::bp bit 1) instead of `kEdge`, and `PackEffects` treats
it as a resync: a pack the partner still has on is activated (`on id N (resync)`), never a Thrust mid-flight, and a
one-shot is lost, as one-shots missed while hidden are (`backpack_sync.md` §3.3). It rides the entry, not
`g_bpResync`: older entries still in the ring would clear that global before this one replays.

The same after a **loss burst**: a step in the first state after 5 or more PSTATEs lost in a row (by `seq`, about
250 ms; the session already dropped late or duplicate ones) is `kStale`, since the activation may be from anywhere in
the gap. `seq`, not the capture time: a sender's own hitch skips no `seq`, and its first state after the hitch is on time.

The same for a **late state** (review fix): `now - time > 150` (250 ms past the fastest packet) sets `g_bpAway` for that
state only. Your own sim stall in state 1 (a destruction or save hitch, a minimised window) skips no `seq` and no menu,
but `Drain` then passes every queued PSTATE in one frame; `MapTime` maps them into the past, so `PackEffects` would run
each one's step and end at once at the puppet's pre-stall place (late Shockwaves, Rhino / Thrust end holes the partner
never made). Now a late step is `kStale` (a Shockwave is lost, a Thrust not launched) and a late level 0 is a quiet
`kAway` end; the on-time newest states of the batch drive the pack. Lateness alone misses the batch's last 150 ms (due,
not 150 ms late, yet still before the puppet moves: `Effects` runs before `ProcessState`), so any due state (`now -
time >= 0`) also counts as away once 250 ms passed without a puppet frame (`g_simAt`, set in `BeforeSim` right before
`ProcessState`); a state not yet due plays after the puppet caught up. A sender hitch maps on time and is not flagged; a
large lag-shim `jitter_ms` makes some ordinary states resyncs too (as a loss burst).

### Pack step sent at once (`Puppet_Step`)

A down-Thrust started in the air sets its boost timer to 0 (0x5F2285 → 0x5F22C1) and ends on the first grounded frame
(0x5F38E6-0x5F3922), so a low one lives 2-3 frames. `SendState` sends only the latest capture, every 50 ms, so the
first PSTATE with the step could carry level 0 and be dropped as a stale edge (no down effect, no dust). Now `SendState`
skips the 50 ms gap when `Puppet_Step()` (`g_outFresh` and the capture's counter differs from `g_sentBp`, the last
sent `bpAct`, set in `Puppet_Out`): the activating capture goes out at once with level 1 and its in-air pose, and the
next one carries level 0 and the landed bit. At most one extra PSTATE per activation. `g_sentBp` is not reset per
level or link: a stale value costs one early send at most.

PSTATE is unreliable (channel 1) and the session drops late ones (session.cpp `seq` test), so that one state can be
lost; the next would then carry the step with level 0 and the landed bit and activate nothing (no down effect, no
dust, no end hole, while the partner's own landing cut it in his world). Hence the step's level is repeated in one
more state (review fix): `Puppet_Out` sets `g_bpRepeat` when the PSTATE it sends steps the counter (`(s.bp ^
g_sentBp) & 0xF`, before `g_sentBp` is overwritten) with level 1 and is not frozen, and ORs `kLevel` into the very
next PSTATE unless that one is frozen (the flag is cleared on every send, on link and when the level goes). Step received: the repeat has no step and
level 1, so `PackEffects` only writes the fuel and the following level 0 ends the pack (or it already ended locally).
Step lost: the repeat carries the step with level 1, so the Thrust activates about 50 ms late, and the following level
0 with the landed bit sets `+0x2AD` and deactivates: dust and end hole. A repeat that already has the landed bit
(the sender landed: a real step's capture follows `activate`, which clears `+0x2AD` at 0x5F2270) on a puppet that
`human_is_in_air` (0x653A20) reads as grounded is dropped, as before the repeat: `activate` would `human_jump` it
(0x5F22AC) and arm a full boost at the landing point. A puppet still in the air gets the timer-0 Thrust, which its own
landing ends. No extra
packet, protocol stays 6. One-shots (step with level 0) are not repeated; they need no level.

### Forced ends cut no hole (G7)

`PackEffects` turns the puppet's pack off itself when it is hidden or unstreamed (or the new pack is not worn yet) and
on a frozen state. For a Rhino or Thrust, `deactivate` then builds the end cutter (0x59AAAE `cmp [edi+0xBC],1`, SP path
0x59AABA, `create_object_internal` 0x59ABA7) at the puppet's current place while the partner's charge goes on. So those
two calls clear `gc_ppWorld` bit 1 around `deactivate` (sim thread; only `EndCutterThunk` reads it in there), and
`EndCutterThunk` skips the cutter and counts it. Two more ends are ours (review fixes):
- **session down**: `Destroy` (the link stopped or dropped; `Puppet_Tick` after `Puppet_Net(false)`) drops the pack
  while `gc_puppet` is still set (`SetPack(-1)` → `PackDropped` 0x5DF1F4 → `deactivate` on an active pack), so it clears
  bit 1 around `SetPack` too and restores it (`Puppet_Net(true)` resets the policy anyway);
- **away**: your own menu, map or load (`Puppet_In` drops every state, `g_bpAway`) leaves the puppet where it stood
  before it; the first state back (or any state 250 ms+ late, or due after 250 ms without a puppet frame) carries Snap bit `kAway` (4, receiver only), and a level 0 there (or a level 0 with
  `kStale`) ends the pack quietly with no landing dust either (`+0x2AD` = 0, read at 0x59ABBA): `off id N (away)`.
  The partner's charge ended meanwhile somewhere else. Mostly the guest's own menu: a host menu pauses the guest too
  (PAUSED token), so the guest's charge itself waits while the host is away. The guest's states stay live, without
  `kFrozen`: session.cpp does not count the guest's mirror token as paused. Not covered: the puppet's own fuel-out between your resume and that first state
  (the game's `deactivate` 0x5F3B12); add a world-bit clear while away if the test shows it. The partner's level 0, the re-edge `deactivate` and the puppet's own
landing or fuel-out keep their cutter. A Rhino resynced after the pause makes one end hole where it really ends; a
Thrust forced off mid-flight is never resynced, so its landing hole and dust are lost on the observer (accepted until
M6). No stale dust either way: `activate` clears `+0x2AD` (0x5F2270).

### R7: the ripple branch

The SP ripple branch needs `dlc_get_current_id() == -1` (0x5F18E3-0x5F18EB). Its variable `Current_bundle_id`
(0x125BB04) is written only by `dlc_set_current_id` 0x3B9E80 (xrefs; every other reference reads it). DotB
content loaded into the campaign by another mod sets it to 1 and back to -1 inside one function (no return in
between), so the campaign keeps -1. In real DLC mode the player branch runs instead: it tests `human_teams_are_enemies`
(0x5F1CF8: no co-op hit) and has no NPCs; G3 sits before both branches.

### Log lines

- install: `hooks installed (... pack gates G3 5F18CA G4 5F1942 G6 5F3188 G7 59ABA7)`. A refusal names
  `G3 use_ripple Multiplayer test`, `G4 ripple object_from_handle<human> call`, `G6 ram spherecast filter` or
  `G7 backpack deactivate cutter create_object_internal call`.
- link: `partner: pack gate policy 1 as host` (or `as guest`).
- `partner: puppet pack on id 1 / 6 / 8, visible bit 1` (as P1); `off id 1 / 8` when the partner's level drops first.
  The Shockwave has no `off` (one-shot), and a local end (the puppet's own Thrust landing, Rhino fuel-out) logs nothing:
  the partner's level 0 then finds the pack off.
- `partner: Thrust: puppet jump id A -> B (the state's): no second launch` when R4 acted.
- `partner: pack step after N lost states: a resync, not an edge` when a loss burst made a step stale.
- `partner: puppet pack off id 1 / 8 (hidden)`, `(partner paused)` or `(away)` (own menu/map during the partner's
  charge, or a state 250 ms+ late or due after 250 ms without a puppet frame: the end is quiet, `kAway`): no end cutter for it (the G7 stats line counts it). A session-down `Destroy`
  drop is quiet too (its `puppet backpack -> id -1` line).
- `partner: G6: its ram passes through a player / an NPC / a vehicle / a building (#n)`: the first five rejections.
- 5 s stats (first three hits each): `G3: its ripple moved objects only (no NPC or vehicle hit)`,
  `G4: a ripple spared a co-op player`, `G7: its end-of-charge cutter skipped` (until M6 clears bit 1: only when the
  mod itself quietly ended its Rhino or Thrust, i.e. a hide, a partner pause, an `(away)` end (your own menu or map,
  a late state, or a late step after a loss burst) or a session-down `Destroy` drop).

### In-game checks (backpack_sync.md §6 P2; A host, B guest, every check both ways)

The sounds of these three packs must be positional at the puppet.
1. B Rhino: the `mp rhino` pose, shield, flames, arc and positional sound; the puppet charges at full speed without
   snapping; a wall B charges through shows the hole on A (and the end-of-charge hole); NPCs in B's path on A are
   **not** thrown (G6 line `passes through an NPC`); A standing in the path is never knocked down. No moving fins on
   the pack (vanilla has none).
2. B Thrust: one launch (no double jump; an R4 line, if any, says one was caught), loop effects, positional sound, the
   boost height matches B's, dust on landing; the down-thrust (Jump held at LB) shows the down effect (ver6.4 test: the landing dust shows only after a down-thrust, on your own Thrust too, so a normal Thrust landing has none in vanilla).
3. B Shockwave next to A and some NPCs: ring and positional sound; loose objects fly; the blast breaks what it breaks
   on B. A is never knocked down (G3 keeps the ripple off; the blast cannot floor a player; no G4 line here, that is
   check 4). A's NPCs more than about 2.5 m from B are not ragdolled (G3 line: its ripple moved objects only).
   Unarmored NPCs within about 2.5 m are knocked down by the blast `mp_shockwave_blast` (impulse 6000 × damage,
   damage 1 → 0 over the 2.5 m Radius; decided from the computed damage at 0x6DC221-0x6DC2B4, not from
   `human_damage`'s result), without damage: allowed until G1 at M6, like the hammer blast, and not a G3 failure.
4. A's own Shockwave next to B's puppet no longer floors the puppet (G4 line).
5. A's own Rhino, Thrust and Shockwave still hit A's NPCs as before (no G3 or G6 line for them).
6. A opens the map, B fires a Shockwave, A closes the map: no late ring or blast on A and no `pack on id 6` line.
   A opens the map, B starts Tremor, A closes it while B's still runs: `on id 9 (resync)` and the quakes resume.
7. P1 checks 1-16 still hold, except P1 check 14 ("nothing new for Thrust, Shockwave or Rhino"), replaced by 1-3.
8. B presses Esc mid-Rhino (or mid-Thrust): no hole on A at the freeze point (`off id 8 (partner paused)`, a
   `G7: its end-of-charge cutter skipped` stats line). Rhino: after resume `on id 8 (resync)` and one end hole where
   B's charge ends. Thrust: no landing hole or dust on A for that one (accepted).
9. B down-thrusts from about half a metre: the down effect and the landing dust show on A (`puppet pack on id 1`).
10. A (the guest) opens the map or Esc just as B starts a Rhino and closes it after the charge: no hole or dust on A
   at the puppet's pre-menu spot (`off id 8 (away)`, a `G7: its end-of-charge cutter skipped` stats line).
11. B presses F8 mid-Rhino: no end hole on A where the puppet stood (`puppet backpack -> id -1`, `puppet destroyed`).
