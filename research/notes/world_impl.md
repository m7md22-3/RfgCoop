# RfgCoop: world experiments implementation (E3 blob round trip, E8 handle dump, save guard)

Code: `src/world.cpp` only (`World_Install` / `World_Tick`, already called by `core.cpp`). Build: GOG `rfg.exe` 32-bit, every
address is an RVA (`rfg.asm` prints VA = RVA + 0x400000). Nothing here has run in game yet. "Verified" means
read in the disassembly (`rfg.asm`), the PDB, the exe bytes on disk (`<game copy B>\...\rfg.exe`), and
for the DLL side in clang's `-O2 -S` output of `world.cpp` (scratch, not a build).

## What it does

`dev=1` only. Keys are edges on the `GetAsyncKeyState` high bit while `Os_app_active` (0x1118591) is set, handled only in
GS_GAMEPLAY (state 1) with `Player` (0x02C22874) set. HUD lines through `hud_message_add`, only in state 1.

| Key | Action |
|---|---|
| F9 | E3 capture: a silent world blob (free roam only), copied into our heap buffer. No disk access |
| F10 | E3 load: the captured blob from memory (RESEARCH.md §4.8 step 3). Arms the save guard first; refuses if it cannot |
| F11 | Arms the save guard by hand (for the "die with the guard on" check) |
| F12 | E8: `RfgCoop_objects_<tag>.txt` next to the DLL: every object within 300 m of you, sorted by handle |

**Keys checked free.** The game's default keyboard map (`Button_bindings_keyboard_mouse` 0xED14E0) binds no F key;
RfgCoop uses F6 (ghost) and plans F7/F8 (session).
F10 is a system key (WM_SYSKEYDOWN, then WM_SYSCOMMAND SC_KEYMENU from DefWindowProc): `keen::wndProc` 0x80E9F0 returns 0
for `wParam & 0xFFF0 == 0xF100` (0x80EA5A-0x80EA85), so no menu loop starts. A user rebinding F9-F12 in the game's options
would make them do both.

## Game functions called

| Function | RVA | ABI used | Verified |
|---|---|---|---|
| `world::get_state_data` | 0x51C460 | `world_state_buf* __thiscall(world*)`, no stack args, plain `ret` | body: `[ecx+0x2578]*0x6C`, returns `ecx+0x257C+cur*0x6C` if `cur_size > 0` else 0. Callers `process_save_load` 0x3C0101, `save_state_data` 0x5412BE (`mov ecx,World; call`) |
| `world::get_load_buf` | 0x51C970 | `uint8_t* __thiscall(world*)`, plain `ret` | body: `mov eax,[World+cur*0x6C+0x25DC]; ret`. Only caller `verify_gamesave_process` 0x3F0286 (`mov ecx,World; call`) |
| `world::set_cur_size` | 0x51C990 | `void __thiscall(world*, int)`, `ret 4` | body writes `+0x25E0` of `[cur]`. Caller 0x3F0298 (`push Verify_size; mov ecx,World; call`) |
| `world::platform_load` | 0x51C9B0 | `void __thiscall(world*, game_save_info*, bool)`, `ret 8` | prologue `sub esp,0xC; cmp byte [esp+0x14],0` (arg 2), `mov esi,[esp+0x18]` (arg 1 after 2 pushes). Caller 0x3F032E (`push 1; push Pending_save; mov ecx,World; call`) |
| `world::set_pending_territory_load` | 0x51C320 | `bool __thiscall(world*, char*, bool new_game, bool single_zone, bool safehouse, int dlc, bool reset)`, `ret 0x18`, result in `al` | body decoded: refuses (`al = 0`) if `+0x24E0 != 0`, `0x265C & 4`, `0x265C & 0x20` or `+0x24D0 != 0`; copies the name to `+0x2698`; packs new_game -> 0x265C bit0, reset -> bit1, single_zone -> bit6, safehouse -> 0x265D bit5; `+0x2694 = dlc`; `gameseq_set_state(2, 0)` (queues a request, 0x3D8730). Callers 0x3F0354 and 0x3C016C both push `("", 0, 0, 0, dlc, reset)` |
| `difficulty_set_level_reload_overwrite` | 0x35CEA0 | `void __cdecl(int)` | body: `Difficulty_level_overwrite (0x12503F0) = arg; ret`. Caller 0x3F0318 (`push -1; call; add esp,4`) |
| `str_hash_string` | 0x1936C0 | `unsigned __cdecl(char*)`, pure | body: lowercase, `rol 6; xor` per char, NULL -> -1. Caller `dynamic initializer for 'player_instantiator'` 0xC75409 (`push "player"; call; add esp,4`): this is how class hashes are made |
| `hud_message_add` (char) | 0x455D30 | `int __cdecl(char*, float, void*, void*, int*)` | same as `puppet.cpp` |

clang's output for `TryLoad` was read: `ecx = base+0x2B97490` before every thiscall, the stack re-adjusted after each
callee-pop (`sub esp,4/8/24`), `test al,al` after `set_pending_territory_load` (the declared return is `uint32_t & 0xFF`, as in
`puppet.cpp`, because the game sets only `al`).

## Data read or written

### `world` (static object `World`, RVA 0x02B97490; PDB type `world`)

| Offset | Field | Use |
|---|---|---|
| +0x60 / +0x64 / +0x6C | `base_array<object*> all_objects` (+4 `elt`, +0xC `num`; PDB type `base_array<object *>`) | E8 iterates `elt[0..num)`, NULL entries skipped |
| +0x24D0 | `cfile* save_file` | must be 0 before a load (set_pending refusal) |
| +0x24E0 | `deserialize_state` | must be 0 before a load |
| +0x2578 | `cur_world_state` | not checked by the capture (the silent save sets it to 0, 0x541279); F10 writes 0 before `get_load_buf` |
| +0x257C | `world_state_buf stored_world_state[2]`, 0x6C each (PDB type `world_state_buf`): +0x00 `player_start_pos`, +0x0C orient, +0x30/+0x3C safehouse pos/orient, +0x60 `buf`, +0x64 `cur_size`, +0x68 `max_size` | capture reads `[0]`; load checks `[0].buf != 0` and `size <= [0].max_size` |
| +0x265C | `save_load_info` bits: bit2 pending_save_game, bit5 performing_save_state, bit7 warp pending | the capture sets bit2 (in the 0x3D9159 chain); capture and F10 require bits 2 and 5 clear; guest.cpp's GF-SAVE clears bit7 (guest_impl.md) |
| +0x265D | bit0 checkpoint, bit1 to_disk, bit4 saving_state_data | the capture clears bits 0-1 (same place) and requires bit4 clear |
| +0x2694 | `int dlc_bundle_id` | saved at capture, passed to `platform_load` (info+0x30) and `set_pending_territory_load` |

### Globals

| Global | RVA | Use | Verified |
|---|---|---|---|
| `Mission_active_index` | 0x0125050C | F9 requires -1; `World_InMission()` (!= -1) is also the co-op guest's link gate (session.cpp `GuestBusy`, session_impl.md "Guest link") | `mission_is_active` 0x35E970: `cmp [it],-1; setne al` |
| `Stream_pause` | 0x028A0670 | F9 requires 0 | symbol table; readers `world::stream_pause` / `do_frame` 0x540964 / `stream_hard_wait` |
| `Loading_screen_district` | 0x028531D4 (int[2]) | copied to info+0xC/+0x10 so the loading screen is unchanged | `loading_screen_set_district` 0x489020 stores arg1/arg2 there; `platform_load` passes info+0xC, info+0x10 |
| `s_hammers_unlocked` | 0x02C256B0 (u64) | info+0x3A | `meta_data_get_hammers_unlocked` 0x611CB0 = `edx:eax = [it]` |
| `Wants_to_start_game_with_golden_hammer` | 0x02C2E044 (u8) | info+0x39 | `platform_load` 0x51CA83, `platform_save_to_disk` 0x52D809 |
| `s_backpacks_unlocked` | 0x02C16C50 (u16) | info+0x42 | getter 0x611D40 |
| `s_jetpack_upgrade_level` | 0x02C16C52 (u8) | info+0x44 | getter 0x611D80 |
| `player_instantiator` | 0x0314F83C | E8 self-check only (read) | `player::get_classname_hash` 0x610140 returns it; set at 0xC75410 to `str_hash_string("player")` |

### `game_save_info` (0xBC bytes, zeroed) for `platform_load`

`platform_load` with non-NULL info (0x51CA4C-0x51CA98) does exactly: `World+0x2694 = +0x30`; `stats_turn_off_all_cheats()`;
`gamelog_campaign_start_logged(0)`; `loading_screen_set_district(+0xC, +0x10)`;
`meta_data_set_hammers_unlocked((u64)byte +0x38 | qword +0x3A)`; golden hammer = `+0x39`;
`meta_data_set_backpacks_unlocked(word +0x42)`; `meta_data_set_jetpack_upgrade_level(byte +0x44)`. The setters are plain
stores (0x611CC0, 0x611D50, 0x611D90). `platform_save_to_disk` fills the same fields from the same globals
(0x52D7F8-0x52D822, `+0x38 = 0`). So the info is built from the **current** globals and only the dlc id comes from the
capture: on one instance `platform_load` changes nothing but turning the game's own cheats off (as every vanilla load).
A zeroed info would have wiped hammers, backpacks and jetpack.

### `object` (PDB type `object`, size 0x9C)

`+0x04 pos`, `+0x6C handle`, `+0x7E int8 obj_type`, `+0x7F int8 sub_type`. There is no class-hash field: the hash is
`str_hash_string(get_classname())`. `get_classname` is vtable slot 3 (+0xC): read from `object::`vftable'` 0xED611C (slot 3
= `_purecall`) and `automobile::`vftable'` 0xF3817C (slot 3 = `automobile::get_classname` 0x7A6480). All 60
`*::get_classname` overrides in the symbol table are 6 bytes, `B8 imm32 C3` (`mov eax, string; ret`). E8 therefore never calls
the virtual: it checks the vtable and slot target lie inside the image, that the code is `B8 imm32 C3`, and reads the
string at imm32 (relocated by the loader like any code operand). Anything else prints class `?`, hash 0. The log line
"self-check" compares `str_hash_string(ClassName(Player))` with `player_instantiator`: they must be equal.

## The save guard (patch)

| Site | Original | New | Why |
|---|---|---|---|
| 0x811278 | `E8 03 C6 0B 00` call `keen::SaveDataHandler::requestSave` 0x8CD880, preceded by `52` (push edx) at 0x811277 | call `OnRequestSave` (`ChainRel32`) | every disk write of the save image goes through `unit4::LocalGameSession::startSave` 0x811240, whose only `requestSave` call this is |

- Armed on the first F10 or on F11, after checking `52 E8` and that the call target is the game's own 0x8CD880 (another mod's
  chain there would refuse it, RESEARCH.md §4.9). Never restored: it stays until the process
  exits.
- **Balanced.** `requestSave(this, user)` (`ret 4`) only does `if (user connected && state == 2) { ++user.requested
  (+0x14); return true; } return false;` (0x8CD880-0x8CD8A5). `SaveDataHandler::update` starts a write flow when
  `requested > completed (+0x24)` (0x8CFC2C-0x8CFC3A); `isSaveInProgress` (0x8CD8B0) is `requested > completed` per user.
  `OnRequestSave` returns 0 without counting a request: the same as the vanilla refusal path, so no completion is ever
  awaited. `startSave` callers ignore its result: `game_save_write_thread` 0x3D0F47 (`inc Save_system_current_ini_write;
  Event::signal`), `game_save_delete` 0x3C3658 (`ui_show_save_icon`), `game_save_write_options` 0x3D137E and
  `game_save_write_profile_data` 0x3D140E (both `inc ...ini_write; signal; ui_show_save_icon`). The save icon
  (`ui_process_save_data` 0x4B9713) shows while `isSaveInProgress` or the ini writer runs or for a minimum duration, so it
  just shows briefly. `game_save_op_completed` 0x3C36D0 / `game_save_get_state` 0x3C36F0 depend only on
  `Save_system_save_data` and the ini **reader** flag.
- **Logging.** Every skip logs `save guard skipped disk write #n: <kind> (startSave from rfg+X)`. The kind comes from
  `startSave`'s return address on its frame: in `OnRequestSave` (frame pointer forced by `__builtin_frame_address`, checked
  in the `-S` output: `push ebp; mov ebp,esp`, the caller read from `[ebp+16]`, `ret 4`), `[ebp+4]` = 0x81127D,
  `[ebp+8]` = user, `[ebp+0xC]` = startSave's pushed esi, `[ebp+0x10]` = startSave's caller. The tick shows
  "Save guard: disk write #n skipped" in gameplay.
- **config.ini** (correction to RESEARCH.md §2 "every disk write passes here"). The save image (saves, autosaves,
  delete, the options and profile copies inside it) passes 0x811278. But `game_save_write_thread` / `_write_options` /
  `_write_profile_data` then also run `inc Save_system_current_ini_write` (0x21328E8; the `inc` is the very instruction
  at each one's startSave return address 0x3D0F47 / 0x3D137E / 0x3D140E, the only three writers in the disassembly) and
  signal `Save_system_ini_event` 0x2134DBC. `game_save_ini_thread` 0x3EE930 sleeps on that event (`sleepUntilSignaled`
  0x3EE965) and only while `current > last` (`Save_system_last_ini_write` 0x21328EC, unsigned `jbe` at 0x3EE975) stores
  `last = current` and calls `game_options_write_ini_file_from_cache` 0x3EE840 (its only call, 0x3EE98B), which writes
  `<known folder 2>\kaiko\rfg\config.ini` (strings 0xEEFD44 "kaiko", 0xEB6480 "rfg", 0xEEFD38 "config.ini").
  `OnRequestSave` therefore also does `InterlockedDecrement(current)` for those three callers (not for delete, which
  does not bump it, nor for an unknown caller), as its last step after the Log (whose file I/O would otherwise widen the
  gap to milliseconds): the caller's `inc` nets it to 0, the woken thread sees `current == last` and writes nothing.
  Checked in clang's `-S` output: `lock decl` at base + 0x21328E8. Edge: the gap between our decrement and the caller's
  `inc` is a few instructions (startSave's `pop esi; ret 4` at 0x81127D). Two guarded saves on different threads
  overlapping there (a save on `game_save_write_thread` plus an options / profile write), with config.ini never written
  this run (`last == 0`), let the ini thread read 0xFFFFFFFF and write config.ini once; `last` = 0xFFFFFFFF then stops
  all later writes. Options only, no campaign data. (Only the three writers wake the ini thread after boot:
  `game_save_process` 0x3C36C2 signals once, on the first fetch of the save data.) Not taken: `InterlockedIncrement(last)`
  instead, because the ini thread's plain `mov [last],eax` at 0x3EE980 can overwrite it while a write from before the
  guard is still pending.
- The in-RAM save image still changes (`game_save_write_thread` copies the slot and the CRC before `startSave`), exactly
  as RESEARCH.md §3.13 says. Restarting the game re-reads the untouched file.

## Capture hook (patch, M5 fix)

| Site | Original | New | Why |
|---|---|---|---|
| 0x3D9159 in `gamestate_gp_handshake` 0x3D9140 | `E8 82 6F FE FF` call `process_save_load` 0x3C00E0 (checked on disk and at runtime: E8, target = base + 0x3C00E0) | call `OnProcessSaveLoad` (`ChainRel32`), which then calls the original | the save bits go in right before `process_save_load` reads them |

- Installed once, on the first capture request (`World_CaptureStart`, sim thread, from the tick: never while the
  handshake runs). A byte mismatch refuses the capture ("the capture hook is unavailable"). Never restored (it does
  nothing without a request).
- ABI: `process_save_load` is `void __cdecl()`: no stack args (the caller pushes nothing, `push 0` for the next call
  follows at 0x3D915E), no register input (it starts with `mov al,[World+0x265C]`), plain `ret` (0x3C0178), and the
  caller reads no register after it. Its only gameplay caller is this one; the others are `world::setup_territory`
  0x540ABD (during a load) and `sony_options_handshake` (a `jmp`). The wrapper compiles to push/pop of ebx/edi/esi and a
  tail `jmp` to the original (clang `-S`).
- **Why (the bug it fixes):** the tick (0x1D24D9 `multi_process`) runs at the top of `rfg_do_frame`; the frame's
  simulation (`gameseq_process` at 0x1D24FF) runs before the handshake (`game_handshake_do_frame` 0x1D2533). Bits set
  from the tick made every vanilla `world::save_and_continue` in that simulation return at once (`test al,0xC; jne`
  0x51D049 -> `ret 0xC`) while its caller cleared its own request: the free-roam death autosave (0x3EDCF0, once per
  death), the activity-complete autosave (0x3ACC58, flag cleared at 0x3ACC5D), the mission after-unlocks autosave
  (0x3B4D93, flag cleared at 0x3B4D9E) and the mission pre-save (0x38E4D1) were lost, not delayed. M5 captures in normal
  play (the join, every free-roam REJOIN_REQ, a retry each second while the join waits), so this mattered beyond F9.

## E3 capture (F9; the session's join and re-join use the same pieces)

1. At the request (`World_CaptureStart`): refused (HUD + log) unless `Mission_active_index == -1`, `Stream_pause == 0`,
   `0x265D` bit4 clear and `0x265C & 0x24 == 0`. The last one is ours (not in the plan): a vanilla save pending would
   otherwise lose its to_disk bit and become a silent save. `World+0x2578` is **not** checked: it can stay 1 in free
   roam after a checkpoint mission that skips its result-screen save (Save the Guerrilla Camp; RESEARCH.md §4.8 step 2,
   probe X8), and this save sets it back to 0. The request is only a flag (`want`).
2. In the 0x3D9159 chain, every gameplay handshake while `want`: the same checks again (a mission, `Stream_pause` or a
   vanilla save may have started during this frame's simulation; a vanilla save's own bit2 then goes first and we try
   the next frame). Passed: remember buffer 0's `cur_size` and an FNV-1a hash of its bytes, `0x265D &= ~3`,
   `0x265C |= 4`, clear `want`, call the original.
3. `process_save_load` 0x3C00E0 sees bit2, calls `world::save_state_data` 0x541250, which runs **synchronously** (loops
   `world::do_frame` until `deserialize_state` is idle, twice; 0x541283-0x5412B3), sets `cur_world_state = 0` because
   bit0 is clear (0x541279), then skips `platform_save_to_disk` because `0x265D & 2` is clear (0x3C00F3) and clears
   bit2 (0x3C013D) in the same call. Nothing runs between our bits and this, so no vanilla save can be refused.
4. The tick sees `want` clear and bit2 clear: `get_state_data()` must return `World+0x257C` (buffer 0). If size and hash
   equal the remembered ones, the capture is "stale" (`save_state_data` returns the old buffer when `do_frame` skipped
   the save) and nothing is kept. Otherwise the bytes are copied to a `std::vector` and the log shows size, previous
   size, `max_size`, magic (`RFGS` = 0x53474652), version (0x34), dlc, hash and 32 printable bytes after the version
   (the start-zone string `init_territory` reads with `cf_read_string`, 0x53FDEB). F9 also keeps the player position.
5. 10 s without a free-roam gameplay handshake (a menu on top, a mission started): `want` is withdrawn ("timed out (no
   gameplay handshake in free roam)") and the HUD says "failed". A bit2 of ours still set then (defensive only: step 3
   clears it in the same call) is withdrawn too: `save_and_continue` returns early while `sl_flags & 0xC`, so no vanilla
   code can have set it meanwhile.

Side effects the game itself causes (RESEARCH.md §9 "A silent blob is always safe"): a held non-inventory object drops
(`save_state_start` 0x53C68C), a hitch (expect a "tick gap" line), and buffer 0 is now the captured world (it is also
the death-continue reload source; it is the current world, so nothing changes).

## E3 load (F10)

Arms the guard (refuses the load if it cannot), then each gameplay frame until it succeeds or 10 s pass:
1. Wait while any `set_pending_territory_load` refusal holds (`+0x24E0`, `0x265C & 0x24`, `+0x24D0`). Checked first, so
   it cannot refuse after `platform_load` ran (same thread, same frame).
2. Refuse for good if buffer 0 has no `buf` or the blob exceeds `max_size` (cannot happen with our own capture).
3. `World+0x2578 = 0`; `memcpy(get_load_buf(), blob, n)`; `set_cur_size(n)` (verify_gamesave_process 0x3F0280-0x3F02AC
   does the same, but without zeroing `cur` first: if `cur` were 1 it would copy into buffer 1 and then load buffer 0).
4. `difficulty_set_level_reload_overwrite(-1)` and `platform_load(&info, true)` as 0x3F0316-0x3F032E.
5. `set_pending_territory_load("", 0, 0, 0, dlc, 0)`; false -> retried next frame (logged).
6. GS_LOAD: `world::load_territory` -> `init_territory` opens `stored_world_state[cur].buf/cur_size` for name "" (not
   `.rfgs_pc`, 0x53FD32-0x53FD59) and checks only magic and version. The player start position and orientation are read
   **from the blob** into `stored_world_state[cur]` (`world::load_territory` 0x5414B5-0x541518, written at save time by
   `save_state_finish` 0x53E127) and used by `world::post_load_stream` 0x540D99. **Correction (in game, 2026-10-07):
   the player does NOT stand at the captured spot.** `save_state_finish` takes the local player's position (0x53DB46),
   then `safehouse_find_spawn_safehouse` (0x53DBB1) and, when one is found, writes that safehouse's start pos/orient
   (`safehouse_get_start_pos_orient` 0x53DBCA) as the player start; only without a spawn safehouse does it fall back to
   `player_start` objects / the player's position. So every load, vanilla or ours, spawns at the nearest spawn safehouse
   (the user's two F10 loads both landed at -1723.9 39.9 437.9, 20 m and 112 m from the F9 spots). Co-op join/re-join
   already teleports the guest beside the host after the load (RESEARCH.md §4.8 step 4), so this needs no fix.

## E8 (F12)

`RfgCoop_objects_<tag>.txt` beside the DLL (`<tag>` from the ini, default A), overwritten each time. Header: count, total
objects, player position, dlc. Then one line per object within 300 m (3D distance from `Player+0x04`), sorted by handle:
`handle classhash type sub x y z class`. One read-only pass over `all_objects` on the sim thread (nothing can create or free
objects meanwhile; the only game call is the pure `str_hash_string`). Diff the A and B files after both load the same
save and stand at the same spot (e.g. `fc /n RfgCoop_objects_A.txt RfgCoop_objects_B.txt` or any diff tool). Expected:
zone and saved objects identical in handle, class and type, positions equal for static ones; runtime spawns differ.

## Test plan (single instance, dev=1; back up the save first)

1. Free roam: F9 -> "World captured: x MB". Log: `world: captured N bytes ... magic 53474652 ... version 52`.
2. Demolish something, walk ~500 m. F10 -> "Save guard ON..." then a load screen. Log: `loading the captured blob`, then
   `back in gameplay after the load: ... X m from the capture spot` (expect a few metres at most). The building stands
   again, you are where you pressed F9.
3. Die once (free roam). Log: `save guard skipped disk write #1: save/autosave`. The death screen and Continue work, no
   hang. After quitting: `keen_savegame_0_0.sav` modified time unchanged.
4. Esc -> Save in a slot: the save UI finishes, log shows another skip, the file is unchanged after a restart.
5. F12 in A and B on the same save at the same spot, then diff the two files.

## Open risks

- Nothing ran in game. The whole E3 path is the engine's own death-continue / load-a-save code, but this combination
  (F9 handshake timing, load from the 0x1D24D9 tick) is new.
- `config.ini`: cancelled through the ini counter (above); only that few-instruction race (two guarded saves overlapping,
  config.ini never written this run) can still let one write through.
- Closed (M5 fix, "Capture hook" above): a vanilla save requested between the capture request and the handshake used
  to be dropped for every capture (F9, the join, every re-join and the join's retries): the free-roam death autosave
  0x3EDCF0, the mission autosaves 0x3B4D93 / 0x38E4D1 and activity completion 0x3ACC58 (not the upgrade-shop exit:
  HostJoin captures only in GS_GAMEPLAY). The bits are now set in the same call that consumes them.
- `Stream_pause` turning on between the request and the handshake: the chain re-checks it and waits (it used to leave
  `pending_save_state` set and report "stale").
- A request pending across a load: the bits are set only in the gameplay handshake (`setup_territory`'s
  `process_save_load` 0x540ABD is not chained), so the first free-roam handshake after the load takes it, or it times
  out; the session drops such a result anyway (its LOADING moves `g_hj` off capture).
- M5 will need the same 0x811278 guard from `session.cpp`; it must call into this one (or move it), not chain the site a
  second time, or `ArmGuard`'s byte check refuses.
- The hammers / backpacks / jetpack bytes are this instance's own for F9 / F10. M5's session loads pass the host's
  (BLOB_BEGIN's WorldMeta, rule Q13). These globals only feed shop prices, saves and level init; the live backpack /
  jetpack comes from the upgrade table (`player::do_frame` 0x6D5694), and a mid-session hammer purchase reaches the
  guest at its next re-join (guest_impl.md "Upgrade effects").
