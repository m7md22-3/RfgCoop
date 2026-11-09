# RfgCoop: M6 shared destruction, ver7.0 (E4 on one game) and ver7.1 (live stream, §11)

Code: `src/destruction.cpp` (new), plus small exports in `core.cpp`, `world.cpp`, `session.cpp`, `guest.cpp`, `features.h`,
`game.h`. Plan of record: `m6_plan.md` §4.1 (record rules §4.1.1, replay rules §4.1.2, measurement §4.1.3, runner
§4.1.4, card C0 §5.1, decision tree §5.3). Protocol 6 unchanged, no network code. Build: GOG `rfg.exe` 32-bit, ASLR, PDB
base 0x400000; **every address is an RVA** (`rfg.asm` and the disassembly print VA = RVA + 0x400000). Bytes below were read
from the exe on disk (`<game copy B>\...\rfg.exe`, read-only) and every ABI in the disassembly during the implementation. Not run in
game yet: card C0 is the first run.

## 1. What it does

- **At DLL load** (`Destr_Install`, after the build check, before any game thread runs game code) every site in §2 is
  checked byte for byte. One mismatch: nothing is written, each bad site is logged, and the line
  `destr: sites not as expected (above): nothing installed (no E4, no GF-EDF)` follows. Otherwise all sites are written
  at once and every co-op byte is 0.
- **Inert by default.** With all bytes 0 every operand site reads 0 as `Multiplayer` does in SP, and every hook calls the
  game's function unchanged (mode off). Only two things turn anything on:
  - the **dev=1 F5 runner** (E4, §6), which sets the bytes for its own phases and puts them back to 0 at its end (`gc_edf` stays 1
    on a guest armed meanwhile);
  - **`Guest_Arm`** (a game linked as a guest, armed for good): `gc_edf = 1` until the process exits (GF-EDF, plan §0.2).
    This is the one change a ver7.0 co-op session sees against ver6.4: the guest's own destruction changes no district
    control (no liberation or warp from its parallel world, today's R14).
- **Real multiplayer**: when `Multiplayer` changes, `Destr_Tick` copies it into `gc_profile` / `gc_rfglGuest` / `gc_edf`
  (the guest keeps `gc_edf = 1`), so MP reads its own flag through the repointed operands. The runner refuses MP.
- The restore / regrow hooks count every call on any thread; they log lines only while the runner records or feeds (50
  lines per phase).

## 2. Sites (all [R] in the disassembly, bytes from the exe on disk)

| RVA | Checked bytes (abs32 relocated at runtime) | What / ABI | Written |
|---|---|---|---|
| 0x4102E0 | `E8 2B 04 F3 FF` -> 0x340710, then `C2 08 00` | `postCollideCallback`: `call rfg_dyn_do_collide_post; ret 8`. 0x340710 is `void __cdecl()`, plain `ret` (0x340A48), reads the count at 0x340787, skips `+0xC8 & 3` at 0x3407B0, type `+0xB0` (`cmp eax,0xd` 0x3407C3), post pass `cmp [entry+0xF8],0` 0x340968, count = 0 at 0x340A2B (SP: `je 0x340A2B` at 0x3409FA) | rel32 -> `OnStep` |
| 0x3383FD | `50` + `E8 FE 4B FE FF` -> 0x31D000 | `rfgl_desired_values(int out[2], int count, int flags)`, cdecl (caller `add esp,0xC` 0x33840E); out[0] quality (0x31D0BC), out[1] delete (0x31D0A7/0x31D0B2); writes no global (0x31D000-0x31D100) | rel32 -> `OnRfgl` |
| 0x324ACC | `56` + `E8 CF EE 2E 00` -> 0x6139A0 | `object_mover_mark_for_fade(mover, int ms)`, cdecl (0x324AD1 `add esp,0x10` also pops int_rand_range's args): timestamp `+0xE8`, `+0xE0 \|= 0x800` | rel32 -> `OnFade` |
| 0x324ADE | `B9 <World>` + `E8 0D 84 1F 00` -> 0x51CEF0 | `world::destroy_object(object*, int)`, thiscall, `ret 8` (0x51CF9B) | rel32 -> `OnDestroy` |
| 0x6A2657 | `52 56` + `E8` -> 0x6139A0, then `add esp,8` | `object_mover_process_explosion_hit`, SP branch (`mov al,[Multiplayer]` / `jne` 0x6A263E): `mark_for_fade(mover, rand()%250)`, same ABI as 0x324ACC. Reached for an rfg_mover only by a **Duration** explosion (explosion_info `+0x28 & 0x8000` at 0x6A2483, set by `explosion_parse_entry` 0x2E82AC when the xtbl entry has `<Duration>`: Vehicle*, propane / gas / fuel / chem tanks, tank shells, thermo_rocket, Big / Medium Bang); any other explosion goes to `rfg_dyn_add_pre_collide_explosion` 0x313700 (`cmp [esi+0x7F],1` 0x6A2490), which in SP queues a step event and returns 1 (null event too, 0x3137CE) unless the mover is destroyed, so `jne 0x6A26AD` at 0x6A250A returns first. Needs `+0xE0 & 4` and an impulse over the threshold (`rfg_dyn_maybe_delete` 0x315210). Runs outside the step (`explosion_do_frame`, `shape_cutter_do_frame`) | rel32 -> `OnFade` |
| 0x7CD092 | `6A 00 51` + `E8` -> 0x6139A0, then `add esp,8` | `do_debris_cleanup` (an AI vehicle stuck on debris): `mark_for_fade(obj, 0)` on `+0x7E == 2` with `+0xE0 & 4`; outside the step | rel32 -> `OnFade` |
| 0x33A94C | `57` + `E8 6F B4 FF FF` -> 0x335DC0 | `rfg_mover_regrow_begin(mover)` bool cdecl (0x33A951 `add esp,4; test al,al`), taken when `+0x10C & 0x80000` (0x33A944) | rel32 -> `OnRegrowBegin` |
| 0x33A92E | `57` + `E8 AD B7 FF FF` -> 0x3360E0 | `rfg_mover_regrow_finish(mover)` cdecl (0x33A939 `add esp,4`), taken when `+0x10C & 0x100000` (0x33A926) | rel32 -> `OnRegrowFinish` |
| 0x534B70 | `8B 0D <Hierarchy 0x28CA67C> 56 8B 74 24 08` | `world::restore_object(handle)`, thiscall, `ret 4` (0x534B86); ecx is reloaded at entry (the callers' `mov ecx,World` is unused); eax / edx are not read before written. 10 callers (xref table): 0x317309, 0x33A985, 0x3751CF, 0x385D0F, 0x3A6D82, 0x3B5FB0, 0x68B9F8, 0x721EB6, 0x72E76E, 0x734D1E | `E9 rel32 90` -> `RestoreThunk` |
| 0x3249BF | `68 <0x314FF0>` | `alloc_rfg_mover_process_shardify_list`: `push rfg_mover_list_compare; push 0xC; push n; push list; call [qsort]` (0x3249BF-0x3249C8), reached only with the 0x3249AC operand on (`gc_profile`) and n > 1. Entries `{mover, +0x6C handle, +0x134 uid}` (0x32497A-0x32498B). 0x314FF0 is `int __cdecl(a, b)`: a.uid -1 -> (b.uid -1 ? a.h - b.h : -1); else b.uid -1 ? 1 : a.uid - b.uid; its only reference (xref table) | imm32 (site+1) -> `OnCompare` |
| 0x339EA3, 0x339FE9, 0x33811A, 0x3249AC, 0x332772 | `80 3D <Multiplayer> 00` each | MP destruction profile (immediate slice, uber explosion, camera/visibility flags, sorted shardify, shatter impulse; m6_pipeline §5) | operand (site+2) -> `&gc_profile` |
| 0x325051 | `80 3D <Multiplayer> 00` | `rfgl_do_frame` entry: 1, then `multi_is_server` 0x5431D0 (= `Server`, 0 in SP) `cmp al,1; jne 0x325209` (pop ecx; ret): the whole balancer and deleter skipped | operand -> `&gc_rfglGuest` |
| 0x3B0024 | `80 3D <Multiplayer> 00` | `gfm_change_edf_control`: 1 -> `jne 0x3B035B` at 0x3B0043 (cookie check, `ret`) | operand -> `&gc_edf` |

Never opened: the `create_shard` sites, 0x312670, 0x3383CF, 0x33830F, 0x33D261, `RFG_load_balance` 0x124F0FB (0 makes
`rfgl_desired_values` return {0,0}, 0x31D020 -> 0x31D0C1), G1/G2.

Data and functions called (not patched):

| RVA | What |
|---|---|
| 0x1CC12F8 | `RFG_dyn_collisions[1024]`, 0x110 each (0x3407A1 `mov edi,0x20c12f8`, stride 0x34089C) |
| 0x1AF7268 | `Num_rfg_dyn_collisions` |
| 0x1A3733C | `Rfg_mover_destruction_uid` (`rva::DestrUid`): `lock xadd` +1 per chunk at 0x33D298, 0 at level init 0x32D988 |
| 0x1B16350 | `Collision_event_wrapper` (next_free_collision_event 0x313319); `cs_wrapper::enter` 0x18AC30 = `push 0; call keen::Mutex::lock; ret`, `leave` 0x18AC40 = `jmp keen::Mutex::unlock`: thiscall, no stack args |
| 0x53BEE0 | `object* __thiscall world::object_from_handle(World, unsigned)` |
| 0x1FB340 | `rfg_mover::get_shape`: `[m+0x14C]` iff `byte [m+0x154] == 1` (inlined) |
| 0x31AC20 | `get_shape_state`: the alive test, inlined (§5) |
| 0x613960 | `bool __cdecl object_mover_is_visible(m)` = `word [m+0xA0] != 0`, the deleter's fade test (0x324A9C) |
| 0x17A290 | `void __cdecl rand_regenerate(int seed)`: `sgenrand(seed)` unless 0, index 0x123AC7C = 0, pool refilled |

Event fields used (`rfg_collision_event`, 0x110): `+0x50` base_a handle (every apply reads it), `+0xA8` base_b handle
(read only by type 4, `rfg_dyn_apply_object_complex` 0x33A2A7: a scan of all 13 apply functions for `+0xa8]`), `+0xB0`
type (dword), `+0xC4` owner handle (types 0-7 read it), `+0xC8` u8 flags (bits 0/1 skip, 2 MP send, 3/4 apply inputs
0x33A213). The prefix 0xEC is the input; the tail `+0xEC..+0x10F` is apply output and is never recorded.

Object fields: `+0x04` pos, `+0x6C` handle, `+0x7E` type (2 = mover), `+0x7F` subtype (1 = rfg_mover), rfg_mover
`+0xF8` move_type (0 = FIXED), `+0xFC` damage_pct, `+0x10C` flags (bit 4 = 0x10 game_destroyed), `+0x134`
destruction_uid (-1 for movers a zone or blob loaded), `+0x14C` shape, `+0x154` streamed (1).

## 3. Hooks

- **`OnStep`** (0x4102E0): counts calls (`seq`), notes the thread id once, then by mode: off = the original; record =
  `Record()` (§4); replay = `Replay()` (§5). Hook time per call goes to `hook max`.
- **`OnRfgl`** (0x3383FD): calls the original first. Inside a recorded step it appends `{count, flags, quality, delete}`;
  inside a replayed step the own result is compared with the host's tuple at that call index (`rfgl own==host`), then
  out[0..1] = the host's quality / delete clamped to the local count. A call past the host's tuples keeps the local
  values and counts `extra` (as plan §4.2 does for synthetic steps). It never falls through otherwise.
- **`OnCompare`** (0x3249BF, qsort's comparator for the shardify candidates): `rfg_mover_list_compare` with the same
  int differences, except on the replay an entry with uid -1 whose handle is in the stamp table (every chunk with a
  runtime uid in the captured world, handle -> uid, `Destr_Stamp`) sorts by that uid. The blob loads those chunks with
  uid -1 (`rfg_mover::serialize` 0x325E2D; the uid prop is set only at creation, 0x31B15A) and the same handle, and
  handles are not in creation order (`handle_manager::allocate_handle` 0x51F160, a wrapping cursor), so without it such
  a chunk would sort among the buildings here but after them on the host: the rfgl tuples (taken by call index) and
  the step's new uids would go to the wrong candidates. Chunks made here carry the host's uids (reseed). Off the replay
  it is the original.
- **`OnFade` / `OnDestroy`** (0x324ACC / 0x324ADE; `OnFade` also at 0x6A2657 and 0x7CD092, the same original 0x6139A0,
  one `g_fadeNext`): while recording, an rfg_mover (type 2/1) appends DESPAWN; then the original. The general-mover list's shards (subtype 0, uid -1, their own 2-5 s fade) are not recorded.
- **`OnRegrowBegin` / `OnRegrowFinish`** (0x33A94C / 0x33A92E) and **`RestoreThunk`** (0x534B70): `Note(kind, handle,
  call site)` then the original. The thunk pushes the handle and the return address, calls `OnRestore` (cdecl), then runs
  the stolen `mov ecx,[Hierarchy]` and jumps to 0x534B76. Compiled form checked in the DLL
  (`push [esp+4]; push [esp+4]; call _OnRestore; add esp,8; mov ecx,[gc_hierarchy]; mov ecx,[ecx]; jmp [gc_restoreBack]`).

## 4. The record stream (plan §3.1; ver7.0's in-memory format is ver7.1's wire)

Packed little-endian records, appended in host order:

- **STEP** 28 B `{kind=1 u8, nRfgl u8, nEvents u16, seq u32, uidBefore u32, uidAfter u32, nHash u16, pad u16, hostMs u32
  (NowMs through 64 bits: it wraps after 2^32 ms of uptime, and every use is a u32 difference), flags u32 (0; ver7.1
  bit 0 = own-pack cutters left out, §11.3)}`, then
  - `nEvents` x 248 B `{uidA u32, uidB u32, owner u8, pad[3], prefix[0xEC]}`. uidA / uidB tag the +0x50 / +0xA8 target:
    the chunk's uid, `0xFFFFFFFF` = an rfg_mover with uid -1 (zone or blob loaded), `0xFFFFFFFE` = no rfg_mover (or +0xA8
    of a type other than 4). Owner 1 = the host's Player, 0 = anything else (sent as -1); 2 (the host's puppet of the
    guest) comes with ver7.1's `Puppet_Handle`.
  - `nRfgl` x 8 B `{count u16, flags u16, quality i16, delete i16}` (saturated; quality can be negative, 0x31D0B6), at
    most 255 per step (more are counted as `rfgl cut`).
  - `nHash` x 12 B `{handle u32, hStatic u32, alive u16 (0xFFFF = no shape), firstTouch u8, pad}`: for each FIXED
    rfg_mover the step targets (at most 32) its fingerprint **after** the step (firstTouch 0); a building touched for the
    first time also gets an entry with its fingerprint **before** the step (firstTouch 1, the step-0 reference).
- **DESPAWN** 16 B `{kind=2, fade u8 (1 fade, 0 destroy), dt u16, uid u32, handle u32, fadeMs u32}`, from the deleter
  (0x324ACC / 0x324ADE), a Duration explosion's hit (0x6A2657) or `do_debris_cleanup` (0x7CD092). `dt` = ms from
  the last recorded STEP's `hostMs` to the moment the DESPAWN is written (capped at 65535; 0 before the first STEP): empty
  steps are not recorded, so a deleter fade in a quiet stretch sits right after the last STEP and needs its own time.
  One from inside a step (Havok `cannotAllocate` 0x43C1DD -> `rfgl_emergency_cleanup` 0x325210 -> 0x3249E0 at 0x325245
  / 0x325253; its only gate, `hkpWorld+0x8C`, is 0 in the post-collide callback) is held and appended right after that
  STEP (dt ~0; alone when the step was empty, with the real dt), so a STEP is always contiguous and DESPAWN(S) lands
  before STEP(S+1).
- A step is appended only when it has events, rfgl calls or a uid change.
- The record is reserved at F5 #1 (32 MB, once per process) and never reallocates while recording: a step that might
  not fit at its largest (`28 + 1024 x 248 + 255 x 8 + 64 x 12` B) runs unrecorded, and the runner aborts.

## 5. Record, replay and measurement as built

**Record** (inside 0x4102E0, before the original): snapshot the count; under the queue lock copy the prefixes of
entries `[0, count)` with `+0xC8 & 3` clear, dropping types 8, 9, 11 and any type outside `e4_types` (`filtered`). After
the lock: tag targets and owner, count types, run the pointer probe (prefix dwords inside the exe image, the three handle
offsets excluded; per type and offset with an example value), collect the FIXED targets. Note `uidBefore`, fingerprint
first-touch buildings (they join the building list, at most 64), call the original (rfgl tuples append; DESPAWNs are
held), note `uidAfter`, fingerprint the targets again, append the STEP, then the held DESPAWNs.

**Replay** (inside 0x4102E0; the live guest of ver7.1 runs the same code):
- every call, under the lock: bits 0|1 on entries `[0, count)` and count = 0 (every local event dropped). The original
  is **not** called unless a host step is applied (plan §0.3, mode 2 idle).
- while feeding, at most `destr_catchup` due STEPs per call (due: `hostMs - first hostMs <= now - feed start`): write the
  entries from index 0 with the tail zeroed and `+0xC8 &= ~7`; resolve +0x50 (and +0xA8 for type 4): chunk uid through
  the uid map, else the prefix handle if it is an rfg_mover with local uid -1 (`by handle`: a chunk from before the
  capture that the blob loaded); uid -1 tag: the handle must be an rfg_mover with local uid -1; tag "no rfg_mover": the
  field becomes -1. Unresolved -> `+0xC8 |= 3`, counted `static` / `chunk`; not streamed (`+0x154 != 1`) -> skipped,
  counted `unstreamed`, and that building gets no verdict in this run. A skipped event is dropped whole, and
  `object_complex` (type 4, 0x33A260) damages both sides (0x33A384 side A, 0x33A3D1 side B): so every tracked building
  the dropped event names (its +0x50, and +0xA8 for type 4, by the host handle, which is the same here) is marked
  `skipped` and gets no verdict in this run either (a chunk never made here must not turn into a K3a DIFF on the
  building it fell on). A uid mapping is used only while its mover is
  alive with that uid (a faded entry stays mapped, a handle can be reused). Owner: 1 or 2 -> the local Player, 0 -> -1.
  Set the count. **Step-0 check** for each building the step touches first: its fingerprint here vs the host's
  firstTouch entry. On a DIFF, if the building at the feed start (`r0`, taken before the feed) equals the host's entry,
  it loaded as the host had it and the replay changed it before its first touch: a real divergence, logged as
  `first divergence before its first touch` with the counters since the feed start, not counted in step0, and a static
  DIFF for that building in this run. Any other DIFF (`r0` differs too: the blob or a handle) makes the run INVALID.
  Set the uid counter to `uidBefore`, call the original. Map the new
  uids `(uidBefore, local counter]` from an `all_objects` walk, leaving out **stale** movers (those that already held a
  uid above `uidBefore` before the step: a local > host step's chunks or the idle path's, whose uids the reseed hands
  out again; that walk runs only while the local counter has been above the host's this run). The step pairs only when
  both deltas are equal and the walk found exactly that many chunks; otherwise its chunks are unpaired and it is a uid
  mismatch (`uid x/y` over steps where either side made chunks). A step with a skipped event (unstreamed, or a chunk
  never made here) gets no uid verdict (`n/a`). rfgl: host tuples the step left over are counted `unused`, local calls
  past them `extra`; when either happens in a step while a chunk of the capture is missing here (`capture chunks
  missing`: a stamp-table handle that is no uid -1 rfg_mover at the feed start), the candidate list was shorter or
  longer than the host's for a harness reason and the tuples shifted, so that step is `n/a` too; with none missing it
  stays a uid verdict (a real divergence). Then compare each after-step building fingerprint (first divergence
  line).
- a uid counter that moved between two hook calls while feeding counts as `idle uid`.
- DESPAWN records at the cursor are applied in `Destr_Tick` (outside the Havok step) once due (`hostMs` of the last
  applied STEP + `dt`, on the feed's host clock: a fade-end destroy feeds `add_to_process_shardify_list` in the same
  host step as on the host, m6_pipeline R2) on the mapped mover (uid map, else a uid -1 mover by handle). Fade: `mark_for_fade` with the host's ms whatever is visible here, and the mapping
  stays (a fading body still collides: `rfg_dyn_add_pre_collide_contact` tests only `+0x54 & 0x10`, 0x32DD7E, so host
  events can name it until its fade ends). Destroy: the mapping goes; fade 1000 ms if `object_mover_is_visible`, else
  `world::destroy_object(World, m, 0)`. The step hook stops at a DESPAWN, so DESPAWN(S) lands before STEP(S+1). A byte
  at the cursor that is neither STEP nor DESPAWN aborts the scenario (`the record is corrupt`) instead of stalling.
- unpaired chunks fade (`mark_for_fade` 2000 ms) 30 s after they appeared (`unpaired faded`); a chunk that pairs later
  (a reused handle) leaves that list.

**Fingerprint / census**: `S = get_shape`, `n = [[S+0x3C]+0xC]` (NULL base or n outside 0..0xFFF0 = not loaded). Alive
`i` = not flag 0x100000, owner offset not 0 / -1, `(sp+8)+[sp+8] == S`. `alive` = count, `hStatic` = FNV-1a of the
alive bitset. The census adds labels (STATIC 0xFFFE, GONE 0xFFFF, ORPHAN when the owner is no live, non-fading shape
sharing the subpiece buffer, else the lowest index with that owner; owners are compared, never dereferenced), `hPart` =
FNV of the u16 labels, `chunks`, `gone`, `orphan`, `damage_pct`, destroyed. **Fade-invariant**: a family mover with
`+0xE0 & 0x800` (fading, set by `object_mover_mark_for_fade` at 0x6139C8) is left out of the family, and ORPHAN hashes as
0xFFFF like GONE (still counted apart), so a fading chunk's pieces hash the same before and after its destroy: the
reference taken at the 60 s cap (a host fade of up to 2 s still running) compares with a replay census taken 10 s after
the feed. Static MATCH = alive and hStatic equal; part MATCH = hPart equal.

## 6. The runner (F5, dev=1)

F5 is read on the async high bit while the window is focused, in gameplay with a Player, in `Destr_Tick` (after
`Net_Tick`). Refused (logged `e4: F5 refused: <why>`, HUD "E4 refused (RfgCoop.log)") while a session runs, once this
game has been a guest, in MP, outside free roam (`World_NotFreeRoam`), without 32 MB for the record, without the save
guard, or when the capture cannot start.

1. **F5 #1**: reserve the record (32 MB, once per process; a failed allocation is caught there and refuses F5),
   `World_ArmSaveGuard("E4")`, `gc_profile = 1`, `World_CaptureStart()`. HUD "E4: recording (F5 when done)".
   `Destr_Stamp()` (world.cpp `OnProcessSaveLoad`, in the handshake that sets the capture's save bits, right before
   `process_save_load` runs `save_state_data`) clears the record, fills the stamp table (handle -> uid of every chunk
   with a runtime uid, for `OnCompare`) and starts recording: every step in the record is after the blob. `World_CapturePoll` done -> `e4: recording from step S (blob N B), profile on`. A failed or stale capture
   aborts.
2. **F5 #2**: the player's position is the near spot; HUD "E4: settling 10 s". **Recording goes on** until nothing was
   recorded (no STEP, no DESPAWN) for 10 s, or 60 s after F5 #2; then every building is censused (the reference) and
   recording stops. So everything that changed the world before the reference is in the record, and the replay's census
   (10 s after its last record, with no local destruction) is comparable (at the 60 s cap a host fade may still run:
   the census is fade-invariant, §5). A mission that starts during the recording aborts the scenario
   (`World_InMission`; its restores would not be in the record). The far spot's centre is the mean of the buildings
   within 100 m (XZ) of the F5 #2 spot (the F5 #2 spot itself when none): a building hit far away by NPC fire during
   the recording is still censused and judged but does not pull the far spot off the scenario.
3. **Runs**: `e4_runs` (default 3) cycling near60, near30, far60. Each: `Core_SetFpsCap(60|30)`;
   `rand_regenerate(GetTickCount())`; `gc_profile = gc_rfglGuest = gc_edf = 1`; mode replay (local events dropped from
   here on); `World_Load(blob, meta, false)` (the F10 path); on the first gameplay frame after the load
   `Session_TeleportTo` the near spot, or 150 m from the buildings' centre on the side away from the near spot (y = the
   near spot's; the hold lands on the terrain when it is higher); wait for the hold (`Session_Teleporting`), then until
   every building has a shape (20 s; at far, one retry at 100 m, logged); 3 s quiet; `r0` of each building and the
   count of capture chunks missing here; feed at live pacing (the step-0 check runs inside it, before each building's
   first-touch step; DESPAWNs when due); 10 s after the last record the census and the run line. A static DIFF in a counted run adds 2 runs of that condition, once per scenario (5 runs at most with
   e4_runs=3).
4. **Verdict** line and HUD "E4 done: K3a … K3b … (RfgCoop.log)"; bytes back to 0 (`gc_edf` stays 1 on an armed
   guest), the uid counter raised to the run's highest (`g_uidHigh`, also on an abort), mode off, fps cap back to the
   ini's. F5 then starts a new scenario from the current world. The save guard
   stays on: restart the game at the end.
- A Player gone (a load or a death) during a run or the recording, a mission started during the recording, a co-op
  session started (F8; checked every tick after the capture, in the frame `Net_Tick` starts it, before any join capture
  or load), a full record or a corrupt record aborts the scenario (`e4: scenario Sn aborted: ...`, HUD "E4 aborted (RfgCoop.log)").

- world.cpp's F9 / F10 are refused while E4 runs (HUD "World: F9 / F10 are off during E4"): F10 would load its own blob
  over the one E4 just asked for.

ini (`[coop]`): `e4_runs` (1..9, default 3), `e4_types` (bit mask, default 0x3FFF; 8, 9, 11 never), `destr_catchup`
(1..16, default 4).

## 7. Log lines

Install and guest:
- `destr: installed, all bytes 0 (vanilla): profile ..., step 4102E0, ..., sort 3249BF; dev D: F5 runs E4 (e4_runs N, e4_types X, destr_catchup C)`
- `destr: <site> at <RVA>: unexpected bytes` / `destr: <site> at <RVA>: not the expected call to <RVA>`, then
  `destr: sites not as expected (above): nothing installed (no E4, no GF-EDF)`
- `destr: GF-EDF on until the game exits (3B0024): ...` (guest) / `destr: GF-EDF unavailable (sites not installed)`
- `destr: restore_object|regrow begin|regrow finish <handle> at step S, called from rfg+<call site>` (recording and
  feeds, 50 per phase)
- core's dev stat line gains `uid <Rfg_mover_destruction_uid> chunks <rfg_movers with uid != -1>` (-1 outside gameplay)

Runner (A's `RfgCoop.log`):
```
e4: thread 0x4102E0 5120 = tick 5120                                  (once per process, dev=1)
e4: F5 refused: a co-op session runs
e4: recording from step 1043 (blob 1315236 B), profile on
e4: F5 #2 at step 1580: settling
e4: reference: 2 buildings (0A1B2C3D alive 812/1530 chunks 14; ...), 0 over 100 m from the F5 #2 spot (not in the far centre), steps 611, events 2930 (types 0:41 1:212 3:1880 4:700 5:63 12:30 13:4), filtered 0, despawns 37 (0 in a step), ptr-like 0, restores 0 regrows 0, rfgl cut 0, 1.40 MB, hook max 2.1 ms
e4: ptr-like type 1 +0xCC: 12 (e.g. 01A2B3C4 = rfg+1A2B3C4)          (only when ptr-like > 0, 6 lines at most)
e4 step 1102: step0 0A1B2C3D DIFF: hStatic ... vs ..., alive 812 vs 811 (host vs here; 65535 = no shape)   (before that building's first-touch step)
e4 step 1102: step0 0A1B2C3D not loaded
e4 step 1102: 0A1B2C3D first divergence before its first touch: hStatic ... vs ..., alive 812 vs 809 (host vs here); since the feed start: restores/regrows 0 (last restore from rfg+0), skipped events 0, idle uid changes 0   (step0 DIFF while r0 == host: a static DIFF, not INVALID)
e4 run 3: a building is not streamed 20 s after landing 150 m away: retrying at 100 m
e4 step 1311: 0A1B2C3D first divergence: hStatic ... vs ..., alive 640 vs 641 (host vs here), types 4 4 5; since the last match: restores/regrows 0 (last restore from rfg+0), skipped events 0, idle uid changes 0   (20 per building per run)
e4 run 2: 0A1B2C3D static DIFF[ (before its first touch)] alive 640 (ref 641) of 1530, part DIFF chunks 13 (ref 14) gone 210 (ref 210) orphan 0 (ref 0), dmg 0.410 (ref 0.410), destroyed 0 (ref 0)
e4 run 2: 0A1B2C3D no verdict (not loaded | events skipped while it was not streamed | host events skipped (a chunk never made here))
e4 run 2/3 near30 seed 81234: step0 2/2 MATCH, static 2/2 MATCH, part 2/2 MATCH, uid 248/249 (99.6%, n/a 0, first diff step 1311 types 4 4 5), rfgl own==host 31/40 (+0 extra, 0 unused, capture chunks missing 0), unmapped static 0 chunk 2 unstreamed 0, by handle 0, despawns unmapped 0, unpaired faded 0, idle uid 0, catch-up max 3, hook max 2.8 ms, 2 m from the spot
e4 verdict S1: K3a PASS (static 6/6 building-runs) K3b PASS (uid 99.4% of 747 chunk steps, part 6/6) invalid 0 crash 0
e4: S1 done. F5 starts a new scenario from this world; restart the game at the end (the save guard stays on)
e4: scenario S1 aborted: <the capture failed | nothing recorded (no building was hit) | a load or a death during a run | a mission started during the recording | a co-op session started | the recording reached 32 MB | the record is corrupt | ...>
```
Around them, existing lines: `world: save guard armed at 811278 until the game exits (E4)`, `world: capture requested`,
`world: captured N bytes ...`, `world: loading a blob from memory (N bytes, dlc D, mission -1, F10)` per run,
`session: E4: teleport to  at x,y,z` (TeleportTo's line; no partner name in E4).

Verdict rules (per scenario, counted runs = runs with step0 all MATCH; a step0 DIFF whose building did not load as the
host had it makes the run `INVALID`; one that did is a static DIFF):
- **K3a**: `INVALID` when any run was INVALID, no run counted or no building got a verdict (a step-0 DIFF is a census
  or handle bug, §5.3: never a kill); else `FAIL` when a building has a static DIFF in 2 or more counted runs; else
  `PASS`. K3a counts only buildings
  that received every host event naming them in that run (`no verdict` for not loaded, unstreamed or skipped host
  events: the run line's `unmapped ... chunk N` still shows that cascade).
- **K3b PASS**: uid deltas equal (and paired) in >= 95% of chunk-creating steps with no skipped event over the counted
  runs (100% when none; steps with a skipped event, or with shifted rfgl tuples while a capture chunk is missing, are
  `n/a`), and at most one counted run has a part DIFF.
- `crash 0` is printed only because a crash never reaches the line: a crash shows as `CRASH` lines instead.

## 8. Card C0 (plan §5.1; A only, dev=1, no role)

Setup: a free-roam save past the Tutorial; buildings that are not mission targets, not next to mining targets or supply
crates, not in a district near zero EDF control.
- **S1**: walk to a 2-3 storey building, F5, hammer it and detonate 2-3 remote charges (~1 min), step back 20 m, F5.
  Leave the game alone until "E4 done" (~5 min: "E4: settling 10 s", then 3 world reloads, 5 with an extension; you
  appear near or 150 m away and the holes come back by themselves).
- **S2**: another building: F5, ram it with a heavy vehicle 2-3 times, then bring a section down with charges, step
  back, F5, wait.
- **S3**: F5, Rhino through two walls, then Tremor beside it, step back, F5, wait.
- Restart the game (save guard), send `RfgCoop.log`.

Read: one `e4 verdict Sn:` line per scenario; PASS = `K3a PASS` and `K3b PASS` in each. Information: `rfgl own==host`
below 100% (the override is needed: on the replay `rfgl_do_frame` is skipped, so its own RFG_load is stale by design),
`unstreamed > 0` and `uid ... n/a > 0` at far60 (7.1's pending queue), `ptr-like > 0` (a type to remap before C1),
`despawns N (k in a step)` with k > 0 (a Havok emergency cleanup ran during the recording), `capture chunks missing`
and `unused` above 0 (the blob did not bring a chunk back: steps with shifted rfgl tuples are `n/a`), `no verdict (host
events skipped ...)` (a K3b-class chunk miss kept out of K3a), `restores` / `regrows` and the first-divergence lines,
including `before its first touch` (§5.3: read them before blaming K3a). Then §5.3's table picks the next build.

## 9. Deviations from the plan (each with its reason)

1. **Recording starts at the stamp** (`Destr_Stamp` clears the record and switches to record) instead of recording from
   F5 #1 and replaying only steps after the stamp: same steps, no pre-stamp bookkeeping.
2. **Recording continues through the settle** and stops at the reference census (10 s with nothing recorded, 60 s at
   most), and the census timer is global, not per building: the plan's "recording stops at F5 #2, then census at settle"
   would leave the settle's steps (falling debris, despawns) in the reference but not in the record.
3. **Session exports**: `Session_TeleportTo(pos, why)` and `Session_Teleporting()` instead of exporting `StepTeleport`:
   `Net_Tick` already calls `StepTeleport` every gameplay frame, session or not (session.cpp `if (state == 1 && me)
   StepTeleport(me)`), so a second caller would step the hold twice.
4. **First touch** is an extra hash entry (firstTouch 1, before the step) next to the after-step entry: one entry per
   target cannot carry both the step-0 reference and the per-step check.
5. **uid tags** gain `0xFFFFFFFE` (no rfg_mover): such a target is written as -1 on the replay, so a recycled runtime
   handle hits nothing (the host's apply ignored it). +0xA8 is resolved only for type 4, the only reader (0x33A2A7).
6. **rfgl quality / delete are i16** (saturated): quality is `min(count - delete, Max - trunc(Recent))` and can be
   negative (0x31D0B6-0x31D0BC). A call beyond the host's tuples keeps the local values (`extra`).
7. **Unstreamed buildings get no verdict** in that run, and a building not loaded at step 0 is logged as `not loaded`
   rather than DIFF: otherwise far60's streaming gap (7.1's pending queue, plan §8 risk 6) would fail K3a or invalidate
   runs. The same holds for a building named by any dropped event (`host events skipped (a chunk never made here)`):
   a type 4 event between an unpaired chunk and the building is dropped whole, so the building misses host damage
   although every event aimed at it alone was replayed; counting it would turn one K3b-class chunk miss (S2's debris on
   the rest) into a K3a FAIL. K3a therefore measures buildings that received every host event naming them.
8. **DESPAWN of a mover the host destroyed** fades in 1000 ms when visible here (the record has no fade ms for it). A
   fade DESPAWN always fades with the host's ms and keeps the mapping until the mover dies: the plan's "fade if the
   deleter's test allows it, otherwise destroy" would destroy at once at far60 a chunk that still collides for up to
   2 s on the host (fade is `int_rand_range(0, 2000)`, 0x324ABE-0x324ACC; 0x32DD7E ignores the fade bit `+0xE0 & 0x800`),
   dropping the host's events on it.
9. **MP mirror** (§1): not in the plan; keeps real multiplayer identical to vanilla with the operands repointed at load.
10. **Owner 2** is not produced in 7.0 (no session in E4; `Puppet_Handle` is 7.1's export). The ini key `destr_check` is
    not read (E4 needs the checks).
11. **Step-0 check** runs inside the feed, right before each building's first-touch step (as §4.1.3 says), not at run
    start. A step can change a building it does not target: `rfg_mover_delete_dynamic_link` 0x32E8C0 (from the apply
    and shardify code inside the step, e.g. `apply_damage_to_subpiece` 0x332A41) sets `[shape+0x58] |= 1` and queues
    the linked rfg_mover's shape (0x32EA46 / 0x32EA4A); `rfg_dyn_do_collide_post` then runs `rfg_mover_should_shardify`
    0x324730 on it (0x3408E2-0x340924), which queues its loose subpieces (0x3247B6) before the FIXED test, so a building
    with saved dynamic links (blob-saved, 0x32E923) loses pieces in a step that targets only a linked chunk. A run-start
    check would mark a correct replay INVALID. A step-0 DIFF is split by `r0` (each building at the feed start): `r0`
    equal to the host's first-touch entry means the building loaded as the host had it and the replay changed it before
    its first touch, a real divergence (logged `first divergence before its first touch` with the counters since the
    feed start, a static DIFF in that run, not counted in step0); otherwise the run is INVALID (blob or handle). K3a
    reads INVALID (never FAIL or PASS) when any run was INVALID. Blind spot: the host changed the building before its first
    touch and the replay did not (`r0` != host, here != host) still reads INVALID; telling that apart would need a
    host capture-time hash per building, which the record does not have.
12. **DESPAWNs from inside a step** (Havok emergency cleanup) are held and appended after that STEP: the plan assumed
    the deleter runs only in `rfgl_do_frame`, outside the step; appending them in place would split the STEP record.
13. **uid `n/a`**: a step with a skipped event gets no uid verdict, as deviation 7 does for the census (the host's
    chunks from an unstreamed building, and its later chunk events, are never made here).
14. **Stale chunks never pair** (§5): the per-step reseed hands a local > host step's (or the idle path's) uids out
    again, so a plain `(uidBefore, uidAfter]` walk could map a host uid to a leftover chunk. The walk runs only while the
    local counter has been above the host's; a step pairs only when the walk finds exactly the host's count.
15. **A co-op session aborts E4** (checked every tick after the capture) and `Stop` leaves `gc_edf = 1` on an armed
    guest: F5 is refused in a session, but F8 during the unattended runs would otherwise mix E4 with a join, and Stop
    would clear a guest's permanent GF-EDF.
16. **The record is reserved (32 MB)** at F5 #1 instead of a 64 MB check after growth: a reallocation inside the Havok
    callback (up to 192 MB alive at once at 64 MB) that fails would throw through game frames with no unwind info.
17. **The shardify candidates' comparator** (0x3249BF -> `OnCompare`, §3) is a site the plan did not list: with the MP
    profile on, the candidates are sorted and the rfgl tuples and new uids follow that order, and a chunk from before
    the capture sorts differently after the blob load (uid -1). The record's rfgl tuple has no candidate identity
    (format unchanged, `RRfgl` 8 B); a capture chunk the blob did not bring back is caught by the `unused` / `extra`
    counters and makes the step `n/a` (only while `capture chunks missing > 0`, so a real candidate divergence still
    counts against K3b).
18. **DESPAWN `dt`** (the plan's `pad`): a DESPAWN is applied `dt` ms after the last applied STEP's `hostMs`, not in the
    next tick: a fade from a quiet stretch would otherwise start seconds early, and its fade-end shardify would land in
    another host step (m6_pipeline R2), inflating K3b failures that live 7.1 (real-time stream) would not have.
19. **Fade-invariant census** (§5): the plan's ORPHAN keeps its count but hashes as GONE, and fading movers are not
    family, so the 60 s cap's reference (taken right after a record) matches the replay's census 10 s after its feed.
20. **A mission start during the recording aborts** the scenario (`World_InMission`): its restores (`restore_list_do_restore`
    0x3751CF) are not recorded, so every replay would read a static DIFF. Activities have no index here and stay
    covered by `restores N` on the reference line.
21. **The far centre** uses only buildings within 100 m of the F5 #2 spot (all buildings are still censused): one
    building hit far away during the recording would move the plan's "buildings' centre" by up to 150 m.

22. **Two more fade sites** (0x6A2657, 0x7CD092, §2) besides the deleter's: a Duration explosion's hit and
    `do_debris_cleanup` fade an rfg_mover outside the step; unrecorded, that chunk would live on the replay until the
    census and read as a part DIFF.
23. **`Stop` raises the uid counter** to the run's highest (`g_uidHigh`, every Apply and step): the per-step reseed can
    leave it below a surviving unpaired or idle-path chunk, and the next scenario on the same world would hand that uid
    out again (a uid tie in the map and the comparator).
24. **world.cpp's F9 / F10 are off while E4 runs** (`Destr_Busy`): an F10 accepted in the frame E4's load is accepted
    (or while E4 retries a busy game) would load its own blob instead.

## 10. Open risks

- Calling `rfg_dyn_do_collide_post` up to `destr_catchup` times in one callback is [I] (plan §8 risk 3); near30 exercises
  it; `destr_catchup=1` is the knob.
- The census and fingerprint read engine internals (owner offset, flag 0x100000, `num_objects`); the step-0 check and
  the reference line's `alive a/n` validate them (an untouched part of a building must count as alive).
- Same process: the pointer probe is the only sign of a static pointer in a prefix (types 1, 12 and 13 read `+0xCC`).
- Teleport to the far spot uses the near spot's height: the hold puts the player on the terrain if it is higher, but a
  far spot more than a few metres lower means a short fall.
- EDF patrols or a death during the unattended runs abort the scenario (logged); pick a quiet area.
- A restore or regrow during the recording (activity starts, stream-in restores) is counted and logged, not replayed
  (M9); stay in free roam. A mission start aborts the scenario.

## 11. ver7.1: the live stream and the resync layer (protocol 7)

Implemented 2026-10-09 (plan `m6_plan.md` §4.2, §6; C0 passed, so the full build, §5.3). Code: `src/destruction.cpp`
(the live section and the shared replay), `src/session.cpp` (epochs, stale rule, F3, autoresync), `src/net.cpp` (caps,
drop counter, range-coded estimate, dial HUD), `src/proto.h` (protocol 7), `src/puppet.cpp` (`Puppet_Handle`,
`Puppet_SetWorldBit`, the crush counter call), `src/features.h`, `RfgCoop.ini` (template). Builds warning-free. Not run in
game yet: card C1 (§11.8) is the first run. No new game site: every hook, byte and ABI is ver7.0's (§2); the new reads
are fields puppet.cpp / session.cpp already use (Player `+0x1170` worn pack, pack `+0x270` info, info `+0x50` id, pack
`+0x254` active; Player `+0xAC4` vehicle handle; `Game_paused_count` 0x2132880; object `+0x7E` type).

### 11.1 Protocol 7 and the messages

`kProtocol` 6 -> 7. HELLO compares the protocol before its size, so a ver7.1 game and a ver6.x / ver7.0 game refuse each
other with a readable REJECT on both screens ("RfgCoop protocol 7 on the host, 6 on the guest"; the RfgCoop CRC check
would refuse them next anyway). No compressor is ever enabled (plan §0.3).

| Message | Dir, channel | Layout (packed, exact size checked) |
|---|---|---|
| BLOB_BEGIN (12) | host -> guest, 2 | + `u8 destr` (bits 0-1 mode, 2 profile, 5 own-pack, 7 stale, 6 = the host's `destr_autoresync` on a stale epoch; 3 lane / 4 gm stay 0 until 7.3 / 7.2) + `u32 logPkts` (0 until 7.2): 39 -> 44 B. The guest refuses mode 3 and `logPkts != 0`. |
| `kDestr` (19) | host -> guest, 2 | `{type, u32 epoch, u32 pkt, u16 len}` 11 B + `len` (1..4000) bytes of the record stream; `pkt` 1, 2, ... per epoch; cut at 4000 B and at a waiting census's offset, otherwise record boundaries ignored |
| `kCensus` (20) | host -> guest, 2 | 58 B: `{type, kind (bit 0 resync check, bit 1 own-pack), u16 index, u16 count, u32 epoch, u32 seq, u32 handle, float pos[3], u16 n, u16 alive, u32 hStatic, u32 hPart, u16 chunks, u16 gone, u16 orphan, u8 destroyed, pad, float dmg, i32 chunksAll}` |
| `kCensusRes` (21) | guest -> host, 0 | 18 B: `{type, verdict (bit 0 identity, 1 static, 2 part, 3 no verdict, 4 resync check), u32 epoch, u32 handle, i32 chunks (the guest's runtime chunks), float poseMax (0 until 7.3)}` |
| `kResync` (22) | guest -> host, 0 | MsgByte: the guest's F3 |

The record stream (plan §3.1) is ver7.0's STEP (28 B + events 248 B + rfgl 8 B + hashes 12 B) and DESPAWN (16 B), plus:
- **RESTORE** 8 B `{kind 4, how (0 restore_object, 1 regrow begin, 2 regrow finish), u16 pad, u32 handle}`;
- **UIDS** 8 B + 8 B per chunk: `{kind 6, pad[3], u32 n}` + `n x {handle, uid}` (deviation 1, §11.9);
- kinds 3 (GM_DEAD, 7.2) and 5 (DISTRICT, M9) are reserved. The guest also stores a received kCensus in its buffer as
  `{0x43, MsgCensus}`, so it is evaluated in stream order; that kind never travels.

### 11.2 Epochs and modes

- **The host's ini decides** (`destr_sync` 0/1/2, `destr_profile`, `destr_ownpack`; read once at DLL load). An epoch is
  one blob id. `SendBlob` calls `Destr_HostEpoch(id, resend, rejoin)`, which returns BEGIN's `destr` byte; a mode never
  changes inside an epoch.
- **Host recording**: from the link the host's step hook records (`kRecord`) every step. In mode 0 / stale epochs the bytes
  are dropped each tick (only the building timers and the census are used); in live epochs they are the stream.
  `Destr_Stamp` (world.cpp's capture chain: the session's capture) starts a **pending epoch**: the buffer is cleared, a
  UIDS record of every runtime chunk goes first, and the steps between the capture and `SendBlob` (one frame) stay in it
  for the new epoch. A **resend** (the loaded buffer) clears the buffer instead and writes the UIDS record again: its
  stream starts at `SendBlob` (the host is paused until JOINED, 0x3EDE51). A live re-join resend's buffer holds the
  host's current runtime chunks (not stale = nothing changed since it was written, and chunks made before the link or in
  an unlink gap never count as changes); after a host load there are none (n 0). A host level load (no Player) ends the
  epoch.
- **Host sending** only while `Session_StreamOpen()`: linked, the guest has a world from us (`g_blobSent`), `g_hj ==
  hjIdle` (no blob on its way, not waiting for JOINED) or `hjWant` with no capture in flight (a wanted blob that has not
  started: deviation 21), READY sent (not loading); and only for a live epoch with an id. At
  most 16 `kDestr` packets (64 KB) per tick, and none while ENet holds 256 KB of channel 2 (queued or unacked,
  `NetQueued`: ENet's send queue has no cap, so the backlog stays in the stream buffer, where the 16 MB end sees it,
  deviation 10); a census goes out once every stream byte recorded before it has gone, and no
  packet runs past a waiting census's offset (the guest splices a census in at the end of what it has, so it must land
  on the record boundary it was taken at; a backlog over 64 KB, a shut gate or a resync check queued before the next
  send would otherwise put it inside a later record).
- **Guest epochs**: `Destr_GuestBegin` at BLOB_BEGIN sets the epoch and its mode, clears the buffer, uid map, pending
  queue, restored set and census retries, and sets `gc_profile` (mode >= 1 and the profile bit), `gc_rfglGuest` (mode 2),
  the hook mode (replay in modes 1 and 2, off in mode 0) and the puppet's world bit (§11.4). `DropBlobs` (the host's
  LOADING, a link change) calls `Destr_GuestDrop`: epoch 0. `kDestr` / `kCensus` of any other epoch are dropped and
  counted (`stale-epoch drops`).
- **The apply gate** (`Session_EpochLoaded`): `g_loadedId == epoch && g_ga == gaIdle && !g_pend.on && !g_hostLoading`.
  `g_loadedId` is set when the load of that blob is seen done (gaWait -> gaArrive) and cleared when a new load is accepted,
  so a refused load (`World_Load` < 0: `g_ga` back to idle with the new header) never opens the gate on the old world.
  Any guest load (LevelState's LOADING edge, e.g. the warp-to-object video's in-RAM reload) clears it too: the reloaded
  world gets no stream until the next blob lands.
- **Broken stream**: a `pkt` gap, a corrupt record (an unknown kind, more than 1024 events or 64 hashes in a STEP, a UIDS
  over 2^20 entries) or over 16 MB waiting (checked before the insert, so the buffer never doubles past ~32 MB): `destr:
  the stream of blob N broke (...)`, epoch 0, and `Session_RejoinRequest("destruction stream broke")`. Any net drop
  (net.cpp's counter) does the same on the guest (`Destr_NetDrop`, `broke (a net drop)`; with no epoch only the re-join):
  a `kCensus` has no packet number, so one behind a lost `kDestr` would otherwise land inside that packet's record. The guest drops its applied bytes when the buffer drains, or once
  they pass 1 MB (a host streaming every frame never drains it).
- **Modes on the guest**: mode 2 is ver7.0's replay (uid reseed and pairing, the host's rfgl tuples, DESPAWNs, no call of
  the original while no host step is due, unpaired chunks faded after 30 s). Mode 1 writes the host's events but skips
  every chunk-target event (counted as `unmapped ... chunk`), keeps its own rfgl values, uid counter and deleter, ignores
  DESPAWNs, and calls the original with count 0 when no host step is due. A dropped chunk-vs-building impact (type 4)
  marks its building: host debris damage never reaches it here (B's own debris is dropped too), so its census says "no
  verdict (debris local (mode 1))" for the rest of the epoch. Mode 0 is ver6.4 plus GF-EDF (the hook is off).
- **Owners**: 1 (the host's Player) -> the guest's puppet of the host (`Puppet_Handle()`), 2 (the host's puppet of the
  guest) -> the guest's Player, 0 -> -1.

### 11.3 The resync layer

- **Stale rule**: destruction.cpp counts, since the Player reappeared after the last level load (`Destr_SinceLoad`), the
  plan's non-empty steps (Record's own test: events, rfgl calls or a uid change; a step with count 0 still shardifies
  deletion-fed candidates), every DESPAWN the host records (deleter fades and destroys need no event), and every
  `world::restore_object` call except the two per-machine callers (object_mover::stream 0x68B9F8 at stream-in;
  public_address_kiosk_level_init_post 0x734D1E at every load, C0's ~40 per reload): a mission start's
  restore_list_do_restore (0x3751CF) runs after its anchor is saved (mission_activate_post_save 0x39D6FE), so a resend
  of that anchor lacks the regrow, and the RESTORE marks of the earlier epoch do not carry over. An in-mission
  REJOIN_REQ resend (`kBlobAsked` + `hjResend`) with a count > 0 gets a stale epoch: mode 0 + bit 7, and bit 6 when the
  host's `destr_autoresync` is on. The host logs `session: stale re-join: S steps or restores since the load`; the
  guest's landing HUD says "Your world misses the host's changes since its mission began; it re-syncs when the mission
  ends" (bit 6) or "...; press F3 after the host's mission" (no bit 6). A host-load resend is never stale (the plan's
  rule is the REJOIN_REQ path).
- **Autoresync (Q21, `destr_autoresync=1`, the host's)**: while the last blob was stale, the host's mission ending (in
  gameplay, `World_InMission()` 1 -> 0, no load between: a load re-sends a fresh epoch by itself) sets `hjWant` /
  `kBlobAsked`: `session: the mission is over: re-syncing <guest> (blob N was stale)`. With a blob already on its way it
  waits; the next blob decides.
- **F3 (guest)**: refused locally before the first join, while a world is on its way, loading or landing, and within 5 s
  of the last request; else `kResync`. The host refuses it when no world was sent yet, a blob is on its way, one went
  under 2.5 s ago, or in its mission, with `kJoinWait` and the reason prefixed `F3: ` (the guest shows "Co-op: F3 resync
  refused: <reason>" only for that prefix, so the text follows what the host decided; any other JOINWAIT, e.g. a
  REJOIN_REQ, autoresync or accepted F3 whose capture met a mission, keeps ver6.4's "you join after the host's mission";
  the epoch stays as it was). In free roam it makes a fresh silent blob (`hjWant`, `kBlobAsked`).
- **Census on the host**: it tracks the newest 64 FIXED rfg_movers its steps touched (an own-pack hit tracks its building
  too). 10 s after a building's last event, in gameplay and unpaused, once the epoch has an id, the host censuses it
  (skipped when it is not streamed on the host) and queues a `kCensus` behind the stream bytes recorded so far. At every
  JOINED for the current blob (`Destr_HostJoined`) it queues one resync-check census per tracked building (index /
  count; one empty census with count 0 when it tracks none).
- **Census on the guest**: evaluated when it reaches the head of the buffer (after every STEP and DESPAWN before it).
  `identity` = its object at the handle within 0.1 m of the host's position; `static` = alive and hStatic equal; `part` =
  hPart equal. The verdict is identity + static; part is printed beside it. Own-pack (kind bit 1) and restored movers
  get "no verdict", and so does a building that lost host events this epoch (`host events dropped (not streamed for 60
  s | pending queue full)`, `debris local (mode 1)`, `host events skipped (a chunk not here, a static missing or
  restored)`: a host event dropped whole because one side is an unmapped or unpaired chunk, a zone mover missing here
  or a restored mover marks every loaded building it names, both sides of a type 4, as E4's `skipped` did; marked until
  the next epoch, since it stays unlike the host's). A
  census of a building not streamed here, or named (either side of a type 4) by an event still in the pending queue, is
  retried every 2 s (judged after they run) and gets "no verdict (not streamed for 60 s | not loaded for 60 s | events
  pending)" at 60 s (unpaused time: B's pause edge shifts these clocks, the pending queue's and the 30 s unpaired fade's
  at the resume, since a pause cannot stream anything in). A retry judged after a host STEP newer than the census named the building (its `seq` above the
  census's; applied or pended alike) gets "no verdict (newer host events)" at once: its snapshot is older than the
  building here (a live census gets the newer census 10 s later; a resync check counts it as no verdict). The resync
  check waits the same way (the landing waits for terrain only, so its shapes can stream in later), and its summary
  line follows once every building of it has an answer (they come out of order); a newer census replaces a waiting one
  of the same kind only, so a live census never drops a resync check's.
  Every verdict goes back as `kCensusRes`; the HUD shows "Destruction check: N MATCH, M DIFF, K no verdict" (live
  censuses, per epoch, a "no verdict" answer too) on both games, or on B in mode 0 with a DIFF "World drifted (k
  buildings): F3 re-syncs in the host's free roam" (F3 is refused in the host's mission). A logs every answer but
  counts and shows only live ones (one for an older epoch, sent before A's capture stamp, its 16 MB end or its load,
  is logged as "for an older epoch N (not counted)", so the tallies can differ around an F3): B shows the resync check as its one summary line ("Destruction check: N MATCH, M
  DIFF").
- **Pending queue** (modes 1 and 2): a host event whose target is not streamed here (`+0x154 != 1`) waits in a list keyed
  by that mover (4096 events, 60 s at most). The events of every mover now streamed run as one synthetic step
  (`LocalStep`; mode 1's idle count-0 call is skipped in a callback that ran it). Mode 1: at every hook call while the
  stream applies, before any host step. Mode 2: right after a host step in the hook's catch-up (before the DESPAWNs
  behind it), never while no host step is due (plan §0.3: the original shardifies deletion-fed candidates on every
  call, and the host takes those in its next step); an idle host (no STEP applied for 1 s and none due) lets it run at
  the hook call (an idle host records no steps, so the queue cannot wait for one). Mode 2 sets the uid counter to the
  private range (0x40000000 and up), marks its chunks unpaired (faded after 30 s) and keeps the local rfgl values; the
  next host step reseeds and gets uid `n/a` (the synthetic step may have taken candidates the host shardifies in it).
  An event on a mover that still waits joins the same list behind the older ones, so per-building order holds: Target
  sends every event on a still unstreamed mover there, Apply also holds (uid `n/a`) every host event naming a building
  that older events still wait for (either side of a type 4), even once it has streamed in (a set of the buildings the
  queue names, built once per step at its first loaded side and grown as events join: no queue scan per event), and the synthetic step
  keeps every later entry naming the mover or a building of an entry that still waits (e.g. a type 4 waiting on its
  other side). Events dropped (60 s, the 4096 cap, or at once when the mover
  they wait on is gone, e.g. a despawned chunk) mark every loaded building they name, both sides of a type 4 (no
  verdict, above), not only the mover they waited on.
- **RESTORE**: every restore / regrow note on a live host becomes a RESTORE record, once per (how, handle) and epoch (a
  regrow begin is noted only when it ran: `rfg_mover_regrow_begin` 0x335DC0 returns false with no change while the
  mover is on screen or +0xE0 & 0x1000000 is clear, and its caller retries it every frame); the
  guest adds the mover to its restored set, skips later events on it (`restored`) and its census says "no verdict
  (restored)". Notes queued before an epoch starts (the host's own load, before a capture) are dropped.
- **Own-pack (Q26, `destr_ownpack=1`, stamp bit 5)**: the host leaves type-6 events whose owner is its puppet of the
  guest out of the stream and tracks their building with "no verdict (own-pack)"; the guest keeps its own type-6 events
  (owner = its Player, up to 64 per callback) and runs them right away as a local step (private uids, unpaired; in mode
  2 the next host step gets uid `n/a`, as after a synthetic step: it may take candidates that step shardifies). The
  host still applies the left-out cutters in that step, so its uid range and rfgl tuples include theirs: the STEP gets
  `flags` bit 0, and every FIXED building it targets is own-pack too. The guest leaves all of that step's chunks unpaired
  (faded after 30 s; later host events naming them are skipped), keeps its local rfgl values (as a local step) and gives
  it no uid verdict (`n/a`).

### 11.4 The gc_ppWorld flip (plan §6)

The host keeps `kPolicyHost` = 1. On the guest, `Puppet_Net(true)` still sets `kPolicyGuest` = 1 at the link, and every
BLOB_BEGIN sets the world bit from the epoch's mode (`Puppet_SetWorldBit(mode == 0)`): off in modes 1 and 2, on in mode
0 or a stale epoch. Off is `gc_ppWorld` bit 8 ("live epoch"), not bit 0: only G6 reads it (the host's puppet passes
rfg_movers that are still whole here for ~RTT; general movers still stop it and break locally on each game until 7.2).
G7 stays on bit 0 (cleared only by the quiet ends, `PackEffects` / `Destroy`), so the host's puppet's end cutter is
still made on B: its holes are type-6 queue events (do_cut 0x328657), which Replay drops (not B's own), its mode-2
rfg_mover fades are skipped by OnFade, and its general-mover, vehicle, debris and item effects play locally, as B's own
cutter's do; the holes arrive in the stream. G1 / G2 stay uninstalled. Log: `partner: pack gate policy P (world bit
on|off: ...)` (P = 9 in a live epoch).

### 11.5 net.cpp

- Inbox caps (`Post`): channel 2 admits while it holds < 8192 items; every other data item while the rest of the inbox
  holds < 8192; channel 1's eviction counts only the items outside channel 2 (4096).
- Channel 2 is host -> guest only: the host's net thread drops every channel-2 packet on receive, so the 8192-item cap
  and the lag-shim exemption hold data only on a guest (from the host it dialled), never an unauthenticated peer's 32 MB
  on the host.
- The lag shim no longer drops channel 2, and counts its channel-2 items apart (as `Post` does), so a stream backlog
  never fills the 4096 cap of channels 0 and 1 (a long `lag_ms` with a saturated stream would drop JOINED, READY, BYE or
  CENSUS_RES). Every reliable drop, in
  `Post` or the shim, goes into one atomic counter (`NetDrops`). The session logs a change (`net: drops N ...`) and the
  guest ends its epoch's stream and asks to re-join (`Destr_NetDrop`, §11.2); the 5 s session line ends with `net drops N`.
- `NetRangeCoded`: the sim thread's own range-coder context; the host adds each packet's coded size to `est`.
- `NetQueued`: channel-2 bytes in packets ENet still holds (an atomic, + at `Send`, - in the packet's `freeCallback` on
  the net thread: the last fragment's ack, a failed send or a peer reset, so it is back at 0 after every relink). The
  host's stream pacing reads it (§11.2, deviation 10).
- Dial HUD: after 3 unanswered dials the guest's HUD says "Co-op: no answer from <addr>:<port> yet: check the address,
  and that both games run the same RfgCoop version" (a `kNetNote` event; once per run of unanswered dials).

### 11.6 Logging (capped)

Restore / regrow lines (`destr: restore_object|regrow begin|regrow finish <h> at step S, called from rfg+<RVA>`) are logged
5 per kind per phase (E4 recording, each E4 feed, a live session); the 5th ends with "(more of this kind: counts only)".
C0 had ~200 regrow begins per scenario (no-op retries; since the C1 fixes only a begin that ran is noted) and ~40
restores per reload. The counts are in the E4 reference line, the E4 run
line (`restores N regrows N`, new) and the 5 s `destr:` line, which also prints `restore_object callers rfg+<RVA> xN`
when they changed.

### 11.7 Every ver7.1 log line

Both:
- `destr: installed, ... co-op: destr_sync S, destr_profile P, destr_ownpack O (as host), destr_check C`
- `destr: live host: mode M (destr_sync), profile P, own-pack O; it records from the next capture` /
  `destr: live guest: waits for the host's first blob (its mode), destr_catchup C, destr_check C` /
  `destr: live off: every co-op byte back to vanilla[ (GF-EDF stays on this guest)]`
- `charge: own X m[, echo N ms | , no echo yet]` (the echo on the guest only) / `charge: puppet of B|A X m`
- `net: drops N (a reliable message was lost: inbox or lag shim full)`; `session: RTT ..., net drops N`
- the 5 s line ends with `, crush (victim<-attacker type: count) Player<-mover t5: 3 ...` when those counters changed
  (dev=1: `OnHumanDamage` calls while a step with queued events is under 10 s old; victim Player / puppet / NPC, attacker
  NULL / mover (`+0x7E` 2) / puppet / other) and `, restore_object callers rfg+68B9F8 x12 ...` when those changed.

Host (A):
- `destr: capture at step S: the next epoch records from here (mode M), N chunks with a runtime uid noted for the guest`
- `session: blob N (...) to <guest>: ..., destr XX` and `destr: epoch N (a fresh capture|the loaded buffer): mode M[ (stale)],
  profile on|off, own-pack on|off; B bytes recorded since the capture`
- `session: stale re-join: S steps or restores since the load`; `session: the mission is over: re-syncing <guest> (blob N was stale)`
- `session: no live stream for <guest> and a mission started: the loaded buffer as is` (deviation 21)
- `session: <guest> asks to re-sync (F3): refused, <why>` / `... (F3): a fresh silent blob`
- `destr: resync check for blob N: K buildings`
- `destr: B checked <h>[ (resync check)]: identity OK|BAD static MATCH|DIFF (part MATCH|DIFF), B chunks y` (B's own
  line has the `chunks A x vs B y` pair from the census's `chunksAll`) /
  `destr: B checked <h>[ (resync check)]: no verdict` / `destr: B checked <h>[ (resync check)] for an older epoch N (not
  counted)`
- `destr: resend: N chunks with a runtime uid noted for the guest` (before the epoch line of a resend)
- `destr: our level load ends epoch N`; `destr: the stream backlog reached 16 MB: epoch N stops streaming (a fresh blob
  re-syncs the guest | the blob on its way re-syncs)`; `destr: no 16 MB for the stream: this host runs mode 0 (parallel
  worlds) this session` (until the next link, which takes `destr_sync` from the ini again)
- every 5 s with traffic: `destr: out P pkts, bytes raw R est E (the tick's first packet range-coded, scaled), max/step M, backlog B, ENet queue Q KB, census sent C,
  own-pack echoes left out O, RESTORE R, restores a regrows b/c, uid U chunks K, net drops D[, crush ...][, restore_object
  callers ...]` (`backlog` = stream bytes not yet handed to ENet; `ENet queue` = channel-2 bytes ENet still holds)
- HUD: "Destruction check: N MATCH, M DIFF, K no verdict" (B's live census answers)

Guest (B):
- `destr: mode M from blob N (profile on|off, pack world bit off|on[, own-pack][, stale])`
- `destr: blob N is loaded here: applying its stream (B bytes waiting)`
- `destr: the host's capture had N chunks with a runtime uid, M of them not here (as blob-loaded chunks)`
- `destr: the stream of blob N is dropped (the host is loading or the link changed)`
- `destr: the stream of blob N broke (packet X where Y was due | a corrupt record | over 16 MB waiting | a net drop): re-joining`
  (broken before blob N landed: its REJOIN_REQ goes out once it has, `session: REJOIN_REQ sent (destruction stream broke)`)
- `destr step S: step0 <h> DIFF: hStatic ... vs ..., alive a vs b (host vs here): ...` (destr_check=1, 20 per building)
- `destr step S: <h> first divergence: ...` (ver7.0's line, with the `destr` prefix in a session)
- `census: <h> seq S[ (resync check)]: identity OK|BAD static MATCH|DIFF (part MATCH|DIFF) chunks A x vs B y; alive a
  (host b) of n, chunks c (host d), gone g (host h), dmg x (host y)`
- `census: <h> seq S[ (resync check)]: no verdict (own-pack | restored | host events dropped (not streamed for 60 s |
  pending queue full) | host events skipped (a chunk not here, a static missing or restored) | debris local (mode 1) |
  newer host events | not loaded for 60 s | not streamed for 60 s | events pending)`
- `census: sanity: <h> undamaged, alive a of n (OK|WRONG)` (once per session)
- `census: resync check (blob N, fresh|stale): K buildings, a MATCH, b DIFF, c not loaded, d no verdict, part e/f`, and
  once per session `census: resync part MATCH e/f (information only)`
- `destr: game paused|resumed (backlog B bytes, K STEPs)` (this game's own pause edge) and `destr: backlog drained (every
  STEP waiting at the resume is applied)`
- `session: F3: resync asked`; `session: the join waits: F3: <reason>` (an F3 refusal)
- every 5 s with traffic or a count of its own (pending applied or dropped, own-pack, stale-epoch drops: an idle host's
  synthetic step or a broken epoch's drops print in their own window): `destr: in P pkts, bytes raw R, backlog B, lag p50 a p95 b ms, uid equal x/y (n/a z), unmapped
  static a chunk b, skipped s, restored r (movers m), pending k (dropped d: 60 s, the 4096 cap or its mover gone; applied p), unpaired faded f, local
  events dropped L (driving k), own-pack o, net drops D, rfgl own==host x/y, step0 a/b, echo p50 a p95 b ms, stale-epoch
  drops s, restores a regrows b/c[, crush ...][, restore_object callers ...]` (`lag` = the guest's apply time minus the
  step's `hostMs`: one QPC clock, so one PC only; `echo` = this guest's own hit on mover H, dropped here, to the first
  applied host step with owner 2 on H; percentiles are nearest rank, so p95 of 2 samples is the larger; `skipped` is
  cumulative per process)
- `destr: blob N ends: uid equal x/y (n/a z), step0 a/b, rfgl own==host c/d, unmapped static e chunk f, despawn misses
  g` (the epoch's totals, at each new BEGIN, broken or dropped stream, or disconnect)
- HUD: "Destruction check: N MATCH, M DIFF, K no verdict" (live censuses), "Destruction check: N MATCH, M DIFF" (the
  resync check's summary), "World drifted (k buildings): F3 re-syncs in the host's free roam" (k = the distinct buildings with a DIFF this
  epoch, not the DIFF answers), "Co-op: re-sync asked",
  "Co-op: F3 resync: not while joining or loading", "Co-op: F3 resync: one went out less than 5 s ago",
  "Co-op: F3 resync refused: <reason>", the stale landing line (§11.3), the dial line (§11.5)

### 11.8 Card C1 (plan §5.1; A host + B guest on one PC, both dev=1, ~10 min)

Setup: both inis `lag_ms=30` (about 60 ms RTT) and `dev=1`; A `fps_cap=60`, **B `fps_cap=30`** (the slower guest). A:
`role=host`; B: `role=guest`, `connect=127.0.0.1:27777`. A keeps the defaults (`destr_sync=2`, `destr_profile=1`,
`destr_autoresync=1`, `destr_ownpack=0`).
1. Start A (a free-roam save) and B. F8 on A, F8 on B, CONNECT. B joins. Stand together near 4 small buildings that are
   not mission targets.
2. **A** hammers building 1 through a wall and detonates 2 charges. Wait for the HUD line on both (~10-15 s).
3. **B** hammers building 2 and detonates 2 charges. B's own hits land a moment later (expected). Wait for the HUD line.
4. **A** (Rhino) charges through a wall of building 3, then Tremor or Shockwave beside it. Wait for the HUD line.
5. **B** opens Esc and stays there 30 s while **A** hammers building 4, then B closes Esc. Wait for the HUD line.
6. **B** walks ~150 m away facing away; **A** charges building 1 again; B walks back (F7). Wait for the HUD line (its
   `no verdict` count may be the one that rises: when B's 5 s line shows `applied p > 0`, building 1 is expected to read
   "no verdict (host events skipped ...)", and this step's evidence is `applied p > 0` with no crash).
7. **B** Rhino-charges a wall of building 2 twice, then Thrusts into one. Did B's charge stop at the wall before the hole
   appeared? Wait for the HUD line on both.
8. **B** presses **F3** (resync) and lands beside A.

Restart both games and send both `RfgCoop.log` files. Report: the same holes in both windows? Anything doubled, jumping
or popping? How late did B's own hits feel? Did B's Rhino stop at walls? Crash?

Pass (read in the logs):
- B: `destr: mode 2 from blob N (profile on, pack world bit off)` after the join and after F3;
- every `census: <h> seq S: identity OK static MATCH` (restored and own-pack movers and buildings that lost host events,
  §11.3, are "no verdict"); uid >= 95% in each
  `destr: blob N ends` line (cumulative per epoch; the 5 s lines show progress); part is the K3b reading: among B's judged
  live `census: <h> seq S:` lines (not resync check, not no verdict), at most one says `part DIFF` (2 or more = K3b,
  plan §5.3); A shows the same as `destr: B checked <h>: ...`
  and the same HUD tally;
- after step 8: `census: resync check (blob N, fresh): ... 0 DIFF, 0 not loaded` (static); `census: resync part MATCH
  x/y` is information;
- step 5: B's `destr: backlog drained` within 5 s of its `destr: game resumed`, no `broke` line, `net drops 0`;
- `echo p95` < 500 ms;
- no `CRASH`, and B's `.sav` time unchanged (standing rule).

Information: `charge: own X m` on B (with its echo) vs `charge: puppet of B X m` on A (step 7, Q26); the crush counters;
`restored` and `restore_object callers` (which code restored movers on A: 0x68B9F8 is object_mover::stream, per
machine); `pending` / `applied` (step 6); the first-divergence lines. Then plan §5.3 picks the next build.

### 11.9 Deviations from plan §4.2 (each with its reason)

1. **UIDS record** (kind 6, at each capture and each resend): the replay sorts shardify candidates by the host's uid for chunks the blob
   loads with uid -1 (`OnCompare` at 0x3249BF, ver7.0 deviation 17). In E4 that table came from the same process; on a
   live guest the capture is on the host, so its table travels as the epoch's first record. Without it a pre-capture chunk
   hit after a join would take another rfgl tuple and uid here.
2. **RESTORE once per (how, handle) and epoch**, and none from before the epoch: C0 S1 logged 4880 `regrow begin` for 3
   movers (`rfg_dyn_do_frame_pre` retries every frame), and every load restores ~40 movers at stream-in on both games.
   Those retries were no-ops (0 regrow finishes): a regrow begin is now noted only when it returns true.
3. **Capped restore / regrow lines** (§11.6), with per-caller counts so C1 can still name the callers.
4. The host skips a census of a building not streamed on the host. (The resync check retries like a live census, plan
   §4.2, so its summary line can come up to 60 s after JOINED when a building streams in late.)
5. **Live step-0** compares each epoch's first touch of a building with the guest's building (`step0 a/b`); ver7.0's
   reference / INVALID logic stays E4's.
6. **Mode 1**: the host streams the same records; the guest skips chunk-target events, DESPAWNs, the rfgl override and the
   uid reseed (plan §2's table, done on the guest side so the stream has one format).
7. **Crush window** = a step with queued events in the last 10 s on that game (the guest has no host census timers);
   attacker "mover" = object type `+0x7E` 2.
8. **Charge log**: each game logs both its own and the partner puppet's Rhino charges; B's own adds its first echo, 1 s
   after the charge ends.
9. **`destr_check=0`** skips the guest's per-step checks (step-0 and first divergence) only; the census is the verdict and
   always runs. `destr_gm` / `destr_lane` are not read before 7.2 / 7.3.
10. **Backlog caps**: the host reserves 16 MB at the link (it appends inside the Havok callback), and hands ENet nothing
    while it holds 256 KB of channel 2 (`NetQueued`, from each packet's free callback: ENet's send queue has no cap, so
    a link slower than the host's destruction would otherwise grow it without bound and empty the stream buffer every
    tick). A full backlog (a step then runs unrecorded: the link, or 16 packets a tick, ~3.8 MB/s at 60 fps, fell
    behind for seconds) ends the epoch whatever the gate: nothing more of it goes out (`g_hId` 0, no census either, so
    none can land inside a half-sent record). With the gate shut the blob on its way, or PollCapture's retry after a
    failed capture, starts the next epoch; with it open the host asks itself for a fresh silent blob
    (`Session_AskBlob`, as F3; in a mission HostJoin sends the loaded buffer as is instead, deviation 21), whose BEGIN
    tells the guest the new mode. Never a silent mode switch inside an epoch. The guest re-joins past 16 MB waiting
    (the host's cap, checked before the insert) and compacts applied bytes past 1 MB.
11. **Net drops**: logged on both games; only the guest ends its epoch's stream and asks to re-join (the host has nothing
    to re-sync from it).
12. **E4 and a session**: a link aborts E4 past its capture phase; during its capture the runner polls world.cpp's
    capture to its end, then stops (one capture at a time). E4's stop leaves a live session's bytes alone.
13. **Stale count** kept in destruction.cpp (§11.3: recorded non-empty steps, DESPAWNs and host-only restores since the
    Player reappeared after a load), not reset in LevelState, so the count is right in the tick of the reload's resend.
    Steps and DESPAWNs count only while recording (from the link): every resend buffer is saved after the link (the
    first blob is always a fresh free-roam capture), so earlier changes are in it.
14. **The m6_ptrlike §4 hygiene line** is in `Record()` (types 0 and 3-6: `+0xCC..+0xEB` zeroed). It also applies to E4
    recordings; nothing reads those bytes for those types, and the pointer probe goes quiet for them.
15. The host sets its MP profile at the capture (a pending epoch), so the steps between the capture and `SendBlob` are
    recorded under the epoch's profile.
16. **Live DESPAWNs apply on arrival** at the head (behind the step before them), not `dt` after it in local time: the
    host sends each tick, so they arrive `dt` late anyway, and a wait would cost every `dt` again while B catches up
    (plan §4.1.2: `dt` paces E4's feed only). The hook's catch-up also takes a DESPAWN or RESTORE between two STEPs (in
    stream order, inside the post-collide callback, where the host's own deleter fades and destroys too), or a host
    stream like STEP, DESPAWN, STEP, ... would apply one STEP per frame whatever `destr_catchup` is. UIDS and census
    records stay GuestTick's.
17. **Buildings that lost host events** (pending drops, mode 1's dropped debris impacts, and host events skipped because
    a side is an unpaired or unmapped chunk, a zone mover missing here or a restored mover) get no census verdict for
    the rest of the epoch, as E4's `skipped` did for a run. Every path marks every loaded building the event names
    (`Lost`), so a type 4 waiting on its chunk side still marks the building on the other side.
18. **The synthetic pending step in mode 2** runs right after a host step, not before it: plan §4.2 asked for it "at the
    next hook call, before any host step", which is the idle call §0.3 rules out for mode 2 (it would shardify
    deletion-fed candidates, e.g. links `rfg_mover_delete_dynamic_link` 0x32EA46 queued for movers a DESPAWN destroyed,
    under private uids and local tuples, so the host's next step makes more chunks than this one and none of them
    pair). An idle host (no STEP applied for 1 s, none due) still lets it run at the hook call; the next host step after
    any synthetic step has uid `n/a`. Mode 1 keeps the plan's place.
19. **A census retry after newer host events** (`seq` of a host STEP naming the building above the census's) gets "no
    verdict (newer host events)": the retry would otherwise compare the building after those events with a snapshot
    from before them (a false DIFF; a resync-check retry is never superseded by a live census).
20. **`g_map` sweep** (host uid -> local handle): every 5 s the guest drops the entries `Mapped` already reads as absent
    (the chunk died or its handle was reused). Fades and in-step re-splits send no destroy DESPAWN, so without it the
    map grew by every chunk paired in an epoch (thousands per collapsed building) until the next blob.
21. **The stream gate** (`Session_StreamOpen`) is also open while a blob is only wanted and no capture is in flight
    (`hjWant && !g_capMine`): an accepted F3 whose capture met a mission waits for free roam with `hjWant` for the whole
    mission, and the guest still follows the live epoch meanwhile (`g_hId` is still it: only the session's own capture
    stamps, `Destr_Stamp` checks `Session_Capturing`, so a capture our load made moot never ends the live epoch). Plan
    §4.2's `g_hj == hjIdle` alone would stop the stream, and B's holes from both players, until the mission ends. A
    capture that fails after its stamp (a stale or empty buffer 0, or the host out of gameplay at the poll), or the 16 MB
    end (deviation 10), leaves no epoch id: in free roam the 3 s retry sends a fresh blob; once a mission has started,
    HostJoin sends the loaded buffer as is (REJOIN_REQ's path: stale after changes, Q21 re-syncs at the mission's end)
    instead of a JOINWAIT that would leave B following a frozen epoch for the whole mission (not with `destr_sync=0`,
    whose epochs never freeze B: ver6.4's wait; nor with no loaded buffer to re-send: ver6.4's 1 s JoinWait). A
    REJOIN_REQ meeting that waiting `hjWant` (no capture of ours in flight) is accepted, not refused as busy: in a
    mission it becomes the resend, so a dead guest does not wait on its death screen for the mission's end.
22. **A stream broken before its world landed** (a `pkt` gap, the 16 MB cap or no memory while the blob of that epoch
    still waits or lands, e.g. after the host's 120 s JOINED timeout opened its gate): `Session_RejoinRequest` goes
    nowhere then ("a world is already on its way", and that world is the broken epoch), so the guest remembers the epoch
    and asks again once it has landed (held by the 5 s window if needed). A newer BLOB_BEGIN, the host's load or a link
    change makes it moot.
23. **A mode-2 guest skips two local fades of rfg_movers**: the Duration explosion hit's (0x6A2657, B-local vehicles
    and gas tanks) and do_debris_cleanup's (0x7CD092, B's own stuck AI traffic) `mark_for_fade` calls return at once in
    `OnFade` (by return address, for an rfg_mover only; both callers clean the stack and read nothing back), so a paired
    chunk never dies on B alone and only the host's DESPAWN (or the 30 s unpaired fade) removes it, as plan §2 says for
    mode 2. General movers (+0x7F 0, which both sites also fade) still fade locally on B: no DESPAWN or unpaired fade
    carries them. The deleter's own 0x324ACC (also the in-step emergency cleanup) still runs. B's AI vehicle stuck on
    rfg_mover debris stays stuck until the host's DESPAWN.

### 11.10 Open risks

- Never run in game; C1 is the first test of the cross-process replay, the epoch gate and the census.
- `object_mover::stream` restores (0x68B9F8) on the host at stream-in mark buildings "restored" on the guest, so their
  events are skipped and their census says no verdict. If C1 shows many, M9 decides (plan §4.2) whether that caller
  should travel at all; the 5 s line names the callers.
- The guest runs the original up to `destr_catchup` + 2 times per callback (catch-up, own-pack, a synthetic step) [I];
  `destr_catchup=1` is the knob. Its catch-up also fades / destroys DESPAWN movers between two of those calls [I] (the
  host's emergency cleanup does the same inside the original).
- Mode 2 freezes the guest's own destruction while no live epoch applies (between BLOB_BEGIN and landing, after a broken
  stream until the re-join's blob): its own hits make no holes then.
- Mode 1 and own-pack are untested ini fallbacks (defaults 2 and 0).
- Mode 2's synthetic step right after host step N can still take candidates queued by step N's own shardify stages,
  which the host takes only in step N+1 (the disassembly does not show whether that list is empty after a step); step
  N+1's uid result is therefore `n/a`, and its chunks may stay unpaired. Only while pending events run (C1 step 6).
- The lag line is meaningless across two PCs (two clocks); the two-PC run reads RTT, B's backlog and A's `backlog` /
  `ENet queue` instead.

## 12. ver7.2: safehouse repair, trust boundary, gas tanks and props, re-join log (written inline, no workflow; protocol 8)

### 12.1 Safehouse repair ("repair on both", user decision 2026-10-12)
The game never saves some buildings' damage (safehouses are rebuilt at every load), so a fresh capture loads them whole
on the guest (C1 epochs 3-4: 4702009C). The guest's census reply sets verdict bit 0x20 when its copy is whole
(`alive == n`). On the host, a resync check of an epoch whose blob holds all our damage (`g_hFresh`: a fresh capture, or a
resend whose re-join log went first, §12.4) that gets identity OK + static DIFF + whole there, for a building we track
(`FindBld`, a fixed rfg_mover: the guest names the handle), repairs our copy once per building and epoch: `Repair()` calls
`world::restore_object` 0x534B70 (thiscall(handle), ret 4: it only adds the handle to Pending_restorations 0x1669B68;
the regrow runs when the mover is off screen). Our own repair stays out of the RESTORE note stream and the stale counter
(`g_mine` under `g_noteLock`, cleared at our load). Instead `Repair` puts one RESTORE (how 0) so B skips the building's
events until our regrow is done (hits in between would break only B's copy), and the regrow finish puts how 3 (B unmarks
it) and queues a census. Log: `destr: <h> whole on B, damaged here after a fresh capture ...: repairing ours`, then
`destr: <h> repaired here (regrow done): census`. HUD on A: "a building the game does not save was repaired".

### 12.2 Trust boundary on received events (security review of 4fafb29)
`Indices()` (guest, in `Write`, after the target handles are mapped): the apply functions index subpieces (stride 0x24)
and links with event fields unchecked: `rfg_dyn_apply_stress_to_subpiece` 0x332F90 (types 1, 2: reads and writes
`[objs + 36*k + 0x18]`), `rfg_dyn_apply_link_broken` 0x333940 (12: link records and two subpiece flag writes), the slice
axis 0x3345E4 (13). Caps: count `+0xC9` <= 8 (types 1, 12: 4-byte entries; the game makes at most 7, 0x3131C1, 0x31325B)
or 16 (type 2: u16; at most 16, 0x313582), because the `+0xCC` union has 0x20 bytes before the real pointers at `+0xEC`;
indices < `num_objects` `[[S+0x3C]+0xC]` (chunk shapes share their building's base, 0x336782) or, for a type-12 link
entry (`+0xC8 & 0x20` clear), < `num_links` `[[S+0x3C]+0x14]`; the slice axis < 3. 0xFFFF is rejected too:
`shapekey_unpack16` makes it -1 and both 0x333820 and 0x333A86 index `objs[-1]` with it. A failing event is skipped like
an unmapped one and logged (first 3: `a host event of type T with an index outside its building dropped`).

### 12.3 GM_DEAD: gas tanks and props (the host's ini `destr_gm`, default 1, BLOB_BEGIN destr bit 0x10)
- Host: general_mover vftable slot 34 at 0xF14064 (the only vftable entry holding 0x68A920; byte-checked) ->
  `OnGmDamage` (fastcall for thiscall: ecx = this, 7 stack args, callee pops 0x1C). After the original, the death edge is
  `+0xF8 & 6` appearing (the death block 0x68ABDB-0x68ABE7 ORs 2 explode or 4 destroyed model; is_killed's 0x100 comes a
  frame later and never for an exploding tank; 0x800 follows the explosion, so "dead" = 0x906). Recorded whenever this
  host streams mode 2 (`g_sync`), not only in a mode-2 epoch (the re-join log needs it): `RGmDead {kind 3,
  damaged_by_types, handle, pos, hostMs}`; inside a step it waits in `g_stepGm` and goes after the STEP (`FlushDespawns`).
- Guest: GF-GM operand repoint at 0x68A925 (`80 3D <Multiplayer> 01` at 0x68A923) -> `gc_gm` (1 in a mode-2 epoch whose
  destr byte has 0x10): `multi_is_server` (0x5431D0, `Server`, 0 in SP) then swallows local damage unless the last bool
  is set. `ApplyGmDead` (GuestTick): the same handle, a type-2 object whose slot 34 is our `OnGmDamage` (= a
  general_mover), within 50 m of the host's spot, not dead (0x906) -> its own slot 34 with 1e6 (the `+0xE0 & 4` branch
  multiplies by 200 into an int, 0x68A99C), the host's damaged_by_types (hp changes for types 1-5 only, 0x636958; else 2),
  a zero vector, NULL source, -1, force 1, as `do_debris_cleanup` 0x7CD01B calls it (NULL source safe: 0x68A9CA,
  0x68ABED). Not here yet or farther than 50 m: retried each tick for 120 s (256 waiting at most). Logs: `destr: gm dead
  applied <h>, lag X ms` (first 5), `GM_DEAD <h>: ...` (first 3 misses).

### 12.4 Re-join log (simplified from plan §4.3: no gaReplay phase)
- Host: `g_log` holds every record streamed since our last level load (or since the link): `LogSync()` copies g_rec's
  new bytes in before any clear (HostReset, the "all out" clear, mode 0) and each tick; the epoch's UIDS record is never
  copied (`g_logAt` moves past it). RESTOREs go to g_rec in every epoch while the log is on, and HostReset drains the
  queued ones first. `LogOff` (the log has a hole: until our next load every resend is stale as in 7.1): past 16 MB,
  out of memory (the insert is caught: never a throw into the game's frames), any record that did not fit (`g_full`),
  or an own-pack step (the guest's own cuts are not in the stream).
- Stamps: world.cpp's 0x3D9159 chain (installed at the host's link, `World_HookSaves`) calls `Destr_Saved()` after
  `process_save_load` when a save was pending or a stored buffer changed size. Each `stored_world_state` buffer whose FNV
  changed since last seen (`g_bufHash`, taken at the link and our load) gets a stamp {log size, our runtime chunks
  {handle, uid}}; bytes seen before keep their first stamp (a save that changed nothing). After our own level load the
  buffer that load used is stamped at 0 with no chunks.
- Resend (`Destr_HostEpoch(resend)`: a death REJOIN_REQ in a mission, our load's resend): the sent buffer's hash looked
  up; a hit of at most 8 MB that fits writes the stamp's chunks as the UIDS record, then `g_log[offset..]`, and the epoch
  is not stale (and its resync check may repair, §12.1). The guest replays those records after landing at
  `destr_catchup` steps per physics step, before the live ones; the resync check at JOINED sits behind them. A miss: the
  7.1 stale rule plus `destr: no log base for the sent buffer (hash X[, log off | log too long]): stale`.

### 12.5 Card C2 (one PC, A host + B guest, both dev=1)
1. Joined. A starts an early story mission and blows up a non-target building in it. B jumps from a height and dies,
   then continues (re-joins mid-mission). Is the building broken on B too? (A: `destr: re-join log: N bytes ... go
   first`; B: census MATCH for it after landing.)
2. A shoots a gas tank next to a building; B watches. Then B shoots another one. (B: `gm dead applied`, both explode once.)
3. Back in free roam, A damages the safehouse building, then B presses F8 twice (leave, join). (A: `whole on B ...
   repairing ours`, then `repaired here (regrow done)` once A looks away from it.)
4. A demolishes something, then fast travels; B follows. Holes the same on both?
- Pass: the census lines MATCH; no `destr: no log base` unless the session linked after that save; every tank once on
  both; no liberation or district message on B; no crash; `index outside its building` never appears with a genuine host.

### 12.6 Review (2026-10-12: two single reviewer agents, no workflow) and known limits
- Fixed from the reviews: the GM_DEAD death edge (0x100 never set by apply_damage) and its replay type (0 = DBT_NONE
  changes no hp); GM_DEAD inside a STEP; the log insert throwing into game frames; stamps of unchanged buffers moving
  later; log holes kept on (`g_full`, own-pack, RESTOREs and GM_DEADs outside live epochs, restores dropped at the reset);
  the hit resend's UIDS (now the save's chunks); a prepend that set `g_full` or passed what the guest holds; `destr_gm`
  per side (now the host's bit); lost GM_DEADs (retry); hits on B during our repair window; the repair trusting any
  handle; `g_mine` kept across our load; 0xFFFF let through.
- Known limits: log events on buildings B has not loaded wait in the pending queue (60 s, 4096) like live ones, so a
  long log far from where B lands can leave those buildings whole there ("no verdict"); a repair already in the log
  (repair, then a mid-mission re-join from a save older than it) leaves B with the pre-repair damage; damage from before
  the link is never in the log (stale, as 7.1).
