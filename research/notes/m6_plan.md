# M6 plan: Shared Geo-Mod (ver7.0 to ver7.3)

Plan of record for the implementers, 2026-10-08. Synthesis of the M6 design workflow wf_bf3597a1-a65: 4 readers
(`findings/workflows/m6_pipeline.md`, `m6_code.md`, `m6_measure.md`, `m6_risks.md`), 3 designs (`m6_design_e4first.md`,
`m6_design_live.md`, `m6_design_staged.md`), 3 judges. Design only: no code was written, nothing was run.
Revised the same day with a critic's 17 gaps (testing per build, epoch and replay phase, restore paths, net.cpp caps,
channel 3, log base, verdicts readable from the log). New sites were re-read in `rfg.asm` for this revision.

Build: GOG `rfg.exe`, 32-bit, ASLR, PDB base 0x400000. **All addresses are RVAs**; the `rfg.asm` cache prints
VA = RVA + 0x400000. Tags: **[R]** re-read in `rfg.asm` / the symbol table during this synthesis (the cache line is quoted),
**[N]** verified by a reader or designer (named), **[S]** read in `src/` (file:line), **[I]** inferred, not verified.

---

## 0. Decisions

### 0.1 The approach

**E4-first** (`m6_design_e4first.md`) is the skeleton. Judges 1 and 2 picked it (30 and 32 points); judge 3 picked staged-safe
(30). The point totals were E4-first 86, staged 88 and live-first 78. The skeleton follows the majority pick, and most of
staged's resync layer is grafted on (§0.2), so the plan has the strengths of both:

- **ver7.0**: a one-game E4 experiment. It decides K3a/K3b with the same record and replay code the live guest will use.
  It adds no network code.
- **ver7.1**: the live stream (protocol 7) with the resync layer: the host's mode per epoch, the stale-epoch rule, the
  resync key, the resync check and the census on both games. (Implemented 2026-10-09, §4.2; card C1 next.)
- **ver7.2**: re-join with holes (a world-message log) and gas tanks/props (GM_DEAD).
- **ver7.3**: the chunk pose lane and the M6 sign-off.

Mandatory path: 7.0 to 7.2. ver7.3 decides K3c. Each build is followed by its one-PC card before the next build starts
(§1). Estimate about 1,200 new lines (RESEARCH §5 guessed 600; the difference is the instruments, the despawns, GM_DEAD,
the re-join log and the restore records, all found after that estimate).

### 0.2 Grafts taken

| From | Graft | Where |
|---|---|---|
| staged, live (all judges) | The **host decides the mode** and stamps it into BLOB_BEGIN for each epoch (+1 byte, plus `logPkts` u32 for 7.2: 39 -> 44 B). Only A's ini matters; B's ini cannot disagree. | 7.1 |
| staged (all judges) | The **Q19 world bit follows the mode**: the guest's world bit is 0 only in a live epoch (`gc_ppWorld` bit 8 set: G6 only, §6); it is 1 in mode 0 or a stale epoch; the host always keeps 1. This replaces an unconditional `kPolicyGuest = 0`. | 7.1 |
| staged (J1, J2) | **Stale-epoch rule**. The host counts non-empty steps since its last load. A mid-mission as-is resend (`session.cpp:732` [S]) with a count > 0 marks the epoch stale, and it runs in mode 0. When the mission ends there is an automatic fresh resync (ini toggle, Q21). ver7.2's log replaces the rule while the log is under its cap. | 7.1 |
| staged (J1, J2) | A **resync check** census after every fresh join, expected 0 static DIFF (the verdict is identity + static; part is logged as information, §4.2). It is a cross-process self-test that costs the user nothing. | 7.1 |
| staged (J1) | A free-roam **guest resync key** through the existing `kBlobAsked` path. It is **F3**, because F5 belongs to the dev E4 runner (J2). | 7.1 |
| staged (J2) | **GF-EDF** (operand 0x3B0026) is **permanent on the guest from ver7.0**. It fixes today's R14 (the guest's own destruction can change district control, liberate or warp) and is the base of the K3a fallback. | 7.0 |
| live (all judges) | Event types **8, 9 and 11 are filtered on both ends** as a code rule. Type 11 carries `invulnerable_mover_handles` at `+0xCC`, and only MP or siege code produces it. | 7.0 |
| live (J2) | Corrected addresses: `world::object_from_handle` **0x53BEE0**, `cs_wrapper::enter/leave` **0x18AC30 / 0x18AC40**. | all |
| live (all judges) | **Census identity check**: the guest's object at the handle must lie within 0.1 m of the host's building position (the first cross-process test of E8). Plus a **sanity line**: an undamaged building has alive == n. | 7.1 |
| live (J2, J3) | The guest calls `rfgl_desired_values` as a **shadow** and logs own == host. This has no side effects: the function writes no global (0x31D000-0x31D100) [N live N6]. The guest always returns the host's values. | 7.0 (runner), 7.1 |
| live (J1, J2) | A **restore/regrow log** on both games: chains at 0x33A94C (the only caller of `rfg_mover_regrow_begin`) and 0x33A92E (`rfg_mover_regrow_finish`), and an entry detour on `world::restore_object` 0x534B70 (10 callers, among them 0x33A985). Per-step fingerprints with a **first-divergence line** name any preceding restore, regrow, skipped event, idle-local shardify or net drop. From 7.1 the host streams RESTORE records (§3.1). | 7.0 |
| live (J2, J3) | **net.cpp channel-2 inbox cap 1024 -> 8192, counted apart from the other channels** (which keep their own 8192), the lag shim no longer drops channel 2, and every drop goes into one atomic counter. Any drop leads to a re-join. | 7.1 |
| staged (J2) | **Raw byte counts plus an offline range-coded estimate**, so the final two-PC run reports bandwidth by itself. The range coder on the wire is declined (§0.3). | 7.1 |
| live (J1) | The **GM_DEAD host site**: `general_mover` vftable slot 34 at 0xF14064 (expected `g_base+0x68A920`) and the `is_killed` 0x612D50 0 -> 1 edge. | 7.2 |
| live (J1) | Each event carries **both** the chunk uid and the raw handle. The guest maps by uid first, then by handle (blob-loaded chunks have uid -1 on both sides). | 7.0 |
| live, staged (J1, J2) | **Test table per build**: each build's one-PC card gates the next build (§5.3), and a failed card picks an ini fallback or the reduced next build (§5). | §5 |

### 0.3 Grafts declined (and why)

- **Live's private uid range (0x40000000+) and staged's "call the original while idle"** (offered by J3) are declined
  for mode 2; J1 says so explicitly. An idle call shardifies deletion-fed candidates early and breaks chunk identity. In
  mode 2 the guest **does not call** 0x340710 while no host step is due (it only marks and resets the local queue, §4.1.2). What an idle skip delays is only the shardify
  stages: `alloc_rfg_mover_process_shardify_list` 0x324930 (0x3408E2), `rfg_dyn_shardify_setup` 0x338040 (0x340914),
  `rfg_dyn_shardify_work` 0x33CD70 (0x340924), `rfg_dyn_shardify_cleanup` 0x33F660 (0x34092D), `maybe_add_shardify_event`
  0x32E250 (0x34093C) [R, the call list of 0x340710]. In **mode 1** (chunks local) the idle call is made, with count 0,
  because no pairing is at stake there.
- **Staged's DEBRIS_DEL by 0.5 s polling** is declined (J1, J3). The host instead chains 0x324ACC and 0x324ADE (every
  caller of 0x3249E0 passes through them), and the guest applies DESPAWN(S) before STEP(S+1). Polling breaks that order
  and would send host stream-outs as deletions [I].
- **Live's "everything in 7.0"** is declined: E4's single-game runner decides K3a before the network layer adds its own
  failure modes.
- **G1/G2 stay uninstalled** (all designs, `m6_risks` R15). They would remove the blast effect of the host's
  Tremor/Shockwave on B, and the queue drop already removes the holes.
- **DP-RNG, `RFG_load_balance = 0`, the `object_mover+0xB0` drift counter** are not used. With the profile on, no
  geometry-relevant draw is left. Setting the byte to 0 makes `rfgl_desired_values` return {0,0} (0x31D020). The `+0xB0`
  field is a zone-file property that never changes.
- **The ENet range coder on the wire** is declined (YAGNI on one PC). `enet_host_compress_with_range_coder` on only one
  side makes the other side drop every datagram, so a stale ver6 folder would turn HELLO's readable REJECT into a silent
  dial loop. Enabling it after WELCOME on both sides races (the host's next datagram can arrive before the guest's
  session thread has seen WELCOME) [I]. Bandwidth comes from raw bytes plus an offline estimate (§3.3).

---

## 1. Testing policy

The user's relayed words were "That is not possible now. We will test once we finish everything." They answer the
two-PC question: RESUME.md:112 records "User: NO two-PC test until everything is finished; all tests one PC (A host, B
guest)", and MODLOG's ver6.4 row ends "The user defers every two-PC test until everything is finished". What waits for
the end is the **two-PC** test, not every test:

- **One one-PC card after each build**, by hand, short, every verdict a log line (§5.1): **C0 after 7.0, C1 after 7.1,
  C2 after 7.2, C3 after 7.3**. Each build is one implement-and-review workflow, and only one workflow runs at a time.
- **§5.3 is the gate** for what gets built next. A C0 K3a FAIL (after its one diagnosis round) means only F0's resync
  layer is built as 7.1 (mode 0, the stale rule, F3, the census as a drift monitor); the live stream, the log and the
  lane are not written.
- A failed later card selects its fallback by an ini value with no rebuild, or one fix round, as §5.3 says.
- **Only the two-PC run (§5.2) waits for the end.** It repeats C1 and is the only check of cross-CPU float and real
  bandwidth.

Every risky part keeps an ini switch, and the default of the final binary is full sync (`destr_sync=2`, `destr_lane=1`,
`destr_gm=1`) unless a card moved it.

---

## 2. Modes and ini

The **host's** ini decides. At each blob, the host stamps the epoch's mode into BLOB_BEGIN `destr` (1 byte): bits 0-1 =
mode, bit 2 = profile, bit 3 = lane, bit 4 = gm, bit 5 = own-pack (Q26), bit 7 = stale, bit 6 = autoresync (the host's
`destr_autoresync` on a stale epoch: the guest's landing line says what follows). The guest uses the stamp, never
its own ini, except for the guest-local knobs. **A mode never changes inside an epoch.** If one ever must, it gets a
`kMode` message on channel 2 carrying the epoch and the `destr` byte (reserved, not built).

| | mode 0, parallel (also any stale epoch) | mode 1, live holes (K3b fallback) | mode 2, full |
|---|---|---|---|
| Guest applies its own queue events | yes (ver6.4) | no: count = 0 under the lock | no |
| Host streams | CENSUS only | DESTR, zone targets, RESTORE; chunk-target events are counted, not applied | DESTR, all targets, uid reseed per step, rfgl tuples, DESPAWN, GM_DEAD, RESTORE |
| Guest idle (no host step due) | original | original with count 0 | **original not called**; under the lock count = 0 and bit 0 on the entries |
| MP profile, both games (`gc_profile`) | 0 | `destr_profile` | `destr_profile` |
| Guest `rfgl_do_frame` (0x325051 lane) | runs | runs (own deleter) | skipped (host DESPAWN; so are the explosion-hit 0x6A2657 and do_debris_cleanup 0x7CD092 fades) |
| Guest rfgl values | own | own | host's, never falls through (own value logged as a shadow) |
| GF-EDF on the guest | on | on | on |
| GF-GM on the guest (7.2) | off | off | on when `destr_gm` |
| Guest `gc_ppWorld` world bit (Q19) | 1 | 0 | 0 |
| Census | drift monitor | static K3a (only for buildings no host debris hit: chunk-vs-building impacts are dropped, so those get "no verdict (debris local)") | static (part logged; pose from 7.3) |
| Guest chunks with no host counterpart | own deleter | own deleter | marked unpaired, faded after 30 s (§4.1.2) |

```
; ---- M6 shared destruction. Only the HOST's values count: they travel with the world; the guest follows them.
destr_sync=2       ; 0 parallel worlds (ver6.4 + district fence), 1 live holes (chunks local), 2 full
destr_profile=1    ; MP destruction profile on both games during a live session (Q10)
destr_gm=1         ; (7.2) gas tanks / props break on the guest only when the host's break
destr_lane=1       ; (7.3) chunk pose lane
destr_autoresync=1 ; host: re-sync a stale guest when the mission ends (Q21; 0 = only on the guest's F3)
destr_ownpack=0    ; (Q26) 1 = B applies its own Rhino/Thrust cutters locally; the host drops their echo to B
; guest-local
destr_catchup=4    ; host steps applied per local physics step, at most
destr_check=1      ; per-step fingerprints + census (log, HUD). 0 saves CPU
; dev (dev=1 only)
e4_runs=3          ; E4 runner runs per scenario (near60, near30, far60)
e4_types=0x3FFF    ; E4/stream event-type mask (bit = type) for bisecting a crash; types 8, 9, 11 are always dropped
```

---

## 3. Wire format and protocol 7

### 3.1 The record stream (ver7.0's recorder format = ver7.1's wire)

The stream is a byte sequence of records, appended in host order:

- **STEP** header 28 B: `kind=1 u8, nRfgl u8, nEvents u16, seq u32 (hook-call counter), uidBefore u32, uidAfter u32,
  nHash u16, pad u16, hostMs u32 (NowMs), flags u32`. It is followed by:
  - `nEvents` × 248 B: `{uidA u32, uidB u32 (chunk uid of the +0x50 / +0xA8 target, ~0 = not a chunk), owner u8
    (0 none, 1 host, 2 guest), pad[3], prefix[0xEC]}`. The tail `+0xEC..+0x10F` is never sent;
  - `nRfgl` × 8 B: `{count u16, flags u16, quality u16, delete u16}`, one per `rfgl_desired_values` call, in call order;
  - `nHash` × 12 B: `{handle u32, hStatic u32, alive u16, firstTouch u8, pad}`, one per FIXED rfg_mover the step targeted
    (max 32).
- **DESPAWN** 16 B: `kind=2, fade u8 (0 destroy / 1 fade), dt u16 (ms after the last STEP's hostMs, capped at 65535),
  uid u32, handle u32, fadeMs u32`.
- **GM_DEAD** 12 B (7.2): `kind=3, pad[3], handle u32, hostMs u32`.
- **RESTORE** 8 B (7.1 records it; M9 applies it): `kind=4, how u8 (0 restore_object, 1 regrow begin, 2 regrow finish),
  pad u16, handle u32` plus the caller's return address in the log only (§4.1 item 5).
- **DISTRICT**: kind 5, **reserved** for M9 (district control changes share the stream's order, m6_risks R10).
- A step is recorded only when it has events, rfgl calls or a uid change. Empty steps cost nothing.

### 3.2 Messages (`kProtocol` 6 -> 7; HELLO refuses a mixed pair readably, because no compressor is ever on, §0.3)

| Message | Dir / channel | Content | Build |
|---|---|---|---|
| BLOB_BEGIN | host -> guest, ch 2 | + `u8 destr` (mode bits, §2) + `u32 logPkts` (log packets that follow the blob; 0 until 7.2), 39 -> 44 B | 7.1 |
| `kDestr` (19) | host -> guest, ch 2 | `{type, epoch u32 (= blob id), pkt u32 (1.. per epoch), len u16}` + ≤ 4000 B of the stream (record boundaries ignored, except that a packet never runs past a waiting census's offset; the guest reassembles) | 7.1 |
| `kCensus` (20) | host -> guest, ch 2 (ordered behind the steps it covers, exactly at the record boundary it was taken at: the host cuts the `kDestr` packet there; the guest puts it into the replay FIFO and evaluates it when it reaches the head, after the DESPAWNs before it) | `{epoch, afterSeq, kind (live / resync), index, count, handle, pos[3], n, alive, hStatic, hPart, chunks, gone, orphan, dmg, destroyed, chunksAll (the host's total runtime chunks)}`; 7.3 adds `top[5] {label u16, count u16, pos[3]}` | 7.1 |
| `kCensusRes` (21) | guest -> host, ch 0 (≤ 156 B) | `{epoch, handle, verdict bits (identity, static, part), chunks, poseMax}` | 7.1 |
| `kResync` (22) | guest -> host, ch 0, MsgByte | the guest's F3 | 7.1 |
| `kChunks` (23) | host -> guest, **ch 3** (new, unreliable, ≤ 1392 B) | `{epoch, hostMs, n, n × {uid, handle, pos[3], quat[4], lin[3]}}` (48 B per chunk, 10 max); the guest maps by uid, then by handle, as events do (blob-loaded chunks have uid -1 here and a runtime uid on the host) | 7.3 |

Exact-size checks apply as for every message, and nothing received is ever executed. All world messages ride channel 2
with the epoch. That keeps the order rule independent of the one-PC lag shim, which orders reliable packets across
channels (`net.cpp:181` [N m6_code]).

### 3.3 net.cpp

7.1:
- **Separate inbox caps** (`net.cpp:49` [S]: today `g_in.size() < 8192 && (!blob || g_inBlob < 1024)`, so a channel-2
  burst of 8192 would fill the whole inbox and silently drop channel-0 control messages: LOADING/READY/BYE, `kResync`,
  `kCensusRes`). New rule: channel 2 admits while `g_inBlob < 8192`; every other data item while `g_in.size() - g_inBlob
  < 8192`. Channel 1 keeps its eviction at 4096 (`net.cpp:44-47`).
- **Lag shim** (`net.cpp:177-179` [S]: `delayed.size() < 4096`, "reliable data past it is lost"): channel 2 is exempt
  from that cap and counted apart (as Post's `g_inBlob`), so a stream backlog never fills the cap of channels 0 and 1.
  Every drop, in Post or in the shim, goes into **one atomic counter**, which the guest's session turns into a broken
  stream (the epoch ends, so no later `kCensus` lands in a record the lost packet left half) and
  `Session_RejoinRequest("destruction stream broke")`. Channel 2 is host -> guest only, so the host's net thread drops
  it on receive (an unauthenticated peer cannot park 32 MB of it on the host).
- **Host pacing**: at most 16 `kDestr` packets (64 KB) per tick are handed to ENet, so ENet's reliable window bounds what
  is in flight [I: 64 KB is ENet's maximum window with bandwidth 0], and none while ENet still holds 256 KB of channel 2
  (`NetQueued`: `enet_peer_send` queues without a cap, so the backlog stays in the host's stream buffer). A 16 MB 7.2
  log then takes about 4 s on loopback instead of arriving inside one 30 ms lag slot. A 7.1 host whose 16 MB stream
  backlog fills (a link slower than its destruction once ENet holds 256 KB, or above ~3.8 MB/s) ends that epoch and
  makes a fresh silent blob (as F3; destruction_impl.md §11.9 #10), never a silent mode switch.
- **No range coder** (§0.3). The 5 s stat line gains raw bytes and an offline estimate: the host runs
  `enet_range_coder_compress` on the first `kDestr` payload of each tick into a scratch buffer and scales the window's
  raw bytes by that sample's ratio (sim thread, ~75-190 µs per 4 KB measured, so one packet per tick, not all 16).
- **Dial HUD**: `net.cpp:158` only logs "no answer ... retrying in 3 s" [S]. After 3 unanswered dials the guest's HUD
  says "Co-op: no answer from <addr> yet: check the address, and that both games run the same RfgCoop version".

7.3, channel 3 (`kChannels` 3 -> 4, `proto.h:16` [S]) needs **four** net.cpp edits:
1. **Send** (`net.cpp:68` [S], `it.channel == kChState ? 0 : ENET_PACKET_FLAG_RELIABLE`): channel 3 is sent unreliable
   (flag 0) like channel 1.
2. **Receive limit** (`net.cpp:163` [S], `channelID != kChBlob && dataLength > sizeof(MsgPState)`): channel 3 gets its own
   1392 B limit; the 156 B rule stays for channels 0 and 1.
3. **Lag shim ordering** (`net.cpp:181` [S], `channelID != kChState` waits for everything before it): channel 3 is left
   out of the reliable ordering, and `loss_pct` applies to it as to channel 1 (`net.cpp:164`).
4. **Post** (`net.cpp:44-49` [S]): channel 3 is evicted oldest-first past 64 items, as channel 1 is past 4096, and is
   admitted outside the global cap, so 10 Hz lane packets cannot crowd out channel 0 during a guest stall.

---

## 4. Builds

### 4.1 ver7.0: E4, one game (~420 lines, protocol 6 unchanged)

**Status: implemented 2026-10-08** (`src/destruction.cpp`, builds warning-free; not run in game yet: card C0 is next).
Review fixes the same day (`destruction_impl.md` §9, 12-16): in-step DESPAWNs after their STEP, the step-0 check per
step, stale chunks never pair, a fade DESPAWN keeps its mapping, uid `n/a` for steps with a skipped event, a co-op
session aborts E4 (a guest keeps GF-EDF), a reserved 32 MB record. Second review (`destruction_impl.md` §9, 7, 11,
17-21): a building named by a dropped event gets no verdict, the step-0 DIFF split by the feed-start fingerprint (and
K3a INVALID with an INVALID run), the shardify comparator for blob-loaded chunks (0x3249BF) with rfgl `unused`
counting, a fade-invariant census, a mission start aborts the recording, the far centre from buildings within 100 m,
DESPAWN `dt`. Third review (`destruction_impl.md` §9, 22-24, and §7): K3a reads INVALID before FAIL when any run was
INVALID (§5.3), the explosion hit's and `do_debris_cleanup`'s fades (0x6A2657, 0x7CD092) also record DESPAWNs, `hostMs`
wraps instead of saturating after 2^32 ms of uptime, `Stop` raises the uid counter to the run's highest, world.cpp's
F9 / F10 are off while E4 runs.
Sites with their checked bytes, ABIs, the record format, the runner flow, every log line and the deviations from this
section (each with its reason): `notes/destruction_impl.md`.

**Scope**: a new `src/destruction.cpp` (build.cmd already compiles `src\*.cpp` [N m6_code]), plus small exports.

1. **Runtime bytes**, installed at DLL load with the L5 operand-repoint idiom. Each install is byte-checked, and nothing
   is written unless all expected bytes match. Every byte starts at 0, which is vanilla.
   - `gc_profile`: the operands at site+2 of 0x339EA3, 0x339FE9, 0x33811A, 0x3249AC and 0x332772.
   - `gc_rfglGuest`: the operand 0x325053 (site 0x325051).
   - `gc_edf`: the operand 0x3B0026. `Guest_Arm` sets it to 1 permanently, so **GF-EDF is live on the guest from
     ver7.0**. The E4 runner also sets it during its runs.
   - Never open the `create_shard` sites, 0x312670, 0x3383CF, 0x33830F or 0x33D261 [N m6_pipeline].
2. **Step hook**: chain the call at 0x4102E0 (`ChainRel32` at 0x4102E1 after checking `E8 rel32 -> 0x340710` and
   `C2 08 00`). Modes: off (call the original), record, replay.
3. **rfgl hook**: chain the call at 0x3383FD (`cdecl(out*, count, flags)`).
   - record: append `{count, flags, quality, delete}`;
   - replay: the original runs first (its result is logged as the shadow own == host), then it is overwritten with the
     host's tuple at that call index, clamped to the local count. It **never falls through** (m6_pipeline R1).
4. **Despawn hooks** (record side): chains at 0x324ACC (`object_mover_mark_for_fade`, args mover and ms) and 0x324ADE
   (`world::destroy_object`, ecx = World, push 0, push mover). They append DESPAWN with `{uid or handle, fade ms |
   destroy}`. The same fade chain at 0x6A2657 (a Duration explosion's hit, SP) and 0x7CD092 (`do_debris_cleanup`):
   both fade an rfg_mover outside the step (`destruction_impl.md` §2).
5. **Restore/regrow log** on both games (logs the handle, the current seq and, for 0x534B70, the return address, which
   names the caller):
   - chain at 0x33A94C (`push edi; call rfg_mover_regrow_begin` 0x335DC0, cdecl(mover), taken when `+0x10C & 0x80000`);
   - chain at 0x33A92E (`push edi; call rfg_mover_regrow_finish` 0x3360E0, cdecl(mover), taken when `+0x10C & 0x100000`);
   - entry detour on `world::restore_object` 0x534B70 (one stack arg, the handle; `ret 0x4`; ecx is reloaded at entry,
     so the callers' `mov ecx,World` is unused). Byte-check `8B 0D <abs32> 56 8B 74 24 08` with abs32 == `g_base +
     0x28CA67C` (the operand is relocated under ASLR); the trampoline re-issues `mov ecx,[g_base+0x28CA67C]` and jumps to
     0x534B76. It sees all 10 call sites in the xref table: `rfg_dyn_do_frame_pre` 0x33A985 (`+0x10C & 0x400000`, cleared
     first), `object_mover::stream` 0x68B9F8 (per machine at stream-in, after `test [edi+0xE0],0x10000000` and a 0x355F90
     check), `restore_list_do_restore` 0x3751CF (mission starts, host only), `Action_callback_restore_object` 0x721EB6
     (scripts, host only), `rfg_mover_rebuild` 0x317309, `wrecking_crew_process` 0x3A6D82, the activity cleanups
     0x385D0F / 0x3B5FB0 and the kiosk restores 0x72E76E / 0x734D1E.
   - From 7.1 the host also appends RESTORE {how, handle} to the stream (§3.1, §4.2).
6. **Fingerprint and census** (§4.1.3), reads only.
7. **Runner** on F5 (dev=1 only, refused in a session or once this game has been a guest; §4.1.4).
8. **Exports**: session.cpp `TeleportTo` / `StepTeleport` (anonymous today); core.cpp `Core_SetFpsCap(int)`; world.cpp
   one line in `OnProcessSaveLoad` (the 0x3D9159 chain) calling `Destr_Stamp()`; features.h `Destr_Install` /
   `Destr_Tick`; two lines in core.cpp after `Net_Tick`.

#### 4.1.1 Record rules (host side, inside the 0x4102E0 hook, before the original)

- Snapshot the count `[0x1AF7268]`. Under `Collision_event_wrapper` (0x1B16350, `cs_wrapper::enter/leave`
  0x18AC30/0x18AC40), copy entries `[0, count)` whose `+0xC8 & 3` is clear. Skip types 8, 9 and 11 always, plus any type
  outside `e4_types`.
- Targets `+0x50` / `+0xA8`: run `object_from_handle` 0x53BEE0. For a type 2 / subtype 1 mover with `+0x134 != -1`
  (a runtime chunk: uid written at 0x33D298, -1 for loaded movers at 0x325DDE/0x325E2D [N]), put the uid in uidA/uidB.
  The raw handle always stays in the prefix.
- Owner `+0xC4`: the host's `Player+0x6C` -> 1, the host's puppet of the guest (`Puppet_Handle()`) -> 2, anything else
  -> 0 (sent as -1, which is also what the host's own `player_damage_only` sees for NPC owners [N]).
- Note `uidBefore`; compute `firstTouch` for FIXED targets not seen before. Call the original (the rfgl hook appends
  during it). Then read `uidAfter` and the per-target `hStatic`, and append the STEP.
- A DESPAWN can come from **inside** the step: Havok's `cannotAllocate` 0x43C170 calls `rfgl_emergency_cleanup`
  0x325210 (0x43C1DD), which calls 0x3249E0 twice (0x325245, 0x325253); its only gate is `hkpWorld+0x8C`
  (`m_criticalOperationsLockCount`), which is 0 in the post-collide callback on both Havok paths [R]. Such DESPAWNs are
  held while the step runs and appended right after its STEP (or alone for an empty step), so every record stays
  contiguous and DESPAWN(S) still lands before STEP(S+1); counted as `despawns N (k in a step)`.
- Every DESPAWN carries `dt` = ms since the last recorded STEP's `hostMs` (empty steps are not recorded, so a deleter
  fade in a quiet stretch would otherwise have no time of its own; R2).
- **Pointer probe**: count the dwords of each prefix that fall inside the exe image range and log them per type and
  offset (~6 lines). On one PC both instances share the image base, so this is the only one-PC sign of a static pointer
  in a prefix. Expected 0: the explosion prefix holds handles and a u8 id only (0x313779 `mov [esi+0xc4],eax`, 0x313796
  `mov [esi+0xb8],ecx` [R]).

#### 4.1.2 Replay rules (the guest path; E4's replaying instance and the live guest run the same code)

- Records wait in a FIFO. In each 0x4102E0 call, under the lock:
  - set the count to 0 (every local event dropped; this also avoids the 1024 cap, m6_pipeline R4);
  - while the FIFO head is a due STEP and fewer than `destr_catchup` steps were applied in this call: write its entries
    **from index 0** with the tail `+0xEC..+0x10F` zeroed (so the post pass skips them at 0x340968). Clear bits 0-2 of
    `+0xC8` and keep bits 3/4 (apply parameters, 0x33A213 [N]). Remap the handles: uid -> local handle through the uid
    map, else the prefix handle if it resolves to a type 2/1 mover; owner 1 -> the local `Player` (E4) or the guest's
    puppet of the host (live), 2 -> the guest's `Player`, 0 -> -1. An unresolved target, a wrong type, or `+0x154 != 1`
    (not streamed) gets `+0xC8 |= 3` and is counted per class: `static`, `chunk`, `blob-loaded`, `unstreamed`.
  - set the count; set `[0x1A3733C] = uidBefore`; call the original; compare the uid delta, the per-target `hStatic`
    and the rfgl shadow.
  - after a step with `uidAfter > uidBefore`, walk `all_objects` once (world.cpp's F12 walk) and map uid -> handle for
    type 2/1 movers with `+0x134` in `(uidBefore, uidAfter]`. Movers that already held a uid above `uidBefore` before
    the step (a local > host step's chunks, or the idle path's; the reseed hands their uids out again) never pair; that
    walk runs only while the local counter has been above the host's. The step pairs only when the remaining chunks
    are exactly the host's count; otherwise it counts as a uid mismatch and its chunks are unpaired.
  - the uid statistic leaves out a step with a skipped event (unstreamed, or a chunk never made here), as the census
    does for an unstreamed building: counted `n/a`. A skipped event is dropped whole and type 4 damages both sides, so
    every building it names gets no census verdict in that run either.
  - the shardify candidates are sorted with the MP profile (qsort at 0x3249BF); on the replay a blob-loaded chunk (uid
    -1 here) sorts by its uid on the recording game (a handle -> uid table taken at the capture), so tuples and new uids
    follow the host's order. Host tuples left over (`unused`) or local calls past them (`extra`) make the step `n/a`
    only while a chunk of the capture is missing here.
  - mode 2 with no step due: **do not call the original** (§0.3), but under the lock set bit 0 (with bit 1) on the
    entries `[0, count)` and set the count to 0, which is what the original does at its end in SP (`740a09: mov al,[0x2fea588]`,
    `740a13: je 0x740a2b`, `740a2b: mov dword ptr [0x1ef7268],0x0` [R]). Otherwise `Num_rfg_dyn_collisions` 0x1AF7268 fills to the 1024 cap (0x339C5A), producers fail
    locally for as long as the guest is idle (what each does on a full queue is [I]), and the post pass later walks stale
    contact entries from earlier frames (m6_risks F2). Mode 1: call it with count 0.
  - **Unpaired chunks** (mode 2; the guest's own deleter is off, so only the host's DESPAWN deletes): after a step whose
    uid delta differs from the host's, the local chunks it created (`+0x134` in the local range) get no uid mapping and
    are marked unpaired; so are chunks created by a pending synthetic step (§4.2). `Destr_Tick` fades each unpaired chunk
    with `object_mover_mark_for_fade` 0x6139A0 30 s after it appeared, counted as `unpaired faded`.
- A DESPAWN or GM_DEAD at the FIFO head is applied in `Destr_Tick` (sim thread, outside the Havok step), a DESPAWN once
  due (`dt` after the last applied STEP's `hostMs`, on the feed's host clock). That rule paces E4's feed only: a live
  DESPAWN is applied as soon as it is whole at the head (the host sends each tick, so it arrives `dt` after its step,
  and a local wait would cost every `dt` again while the guest catches up):
  - DESPAWN (fade): fade the mapped mover with the host's ms, whatever is visible here, and keep its uid mapping: a
    fading body still collides (`rfg_dyn_add_pre_collide_contact` tests only `+0x54 & 0x10`, 0x32DD7E [R]), so host
    events can name it until the fade ends. A mapping is used only while its mover is alive with that uid.
  - DESPAWN (destroy): drop the mapping; fade in 1000 ms if the deleter's own test allows it (0x324A9C `call 0x613960`
    [R]), otherwise `world::destroy_object(World, mover, 0)` 0x51CEF0.
  - The step hook stops at a non-step record, so DESPAWN(S) always lands before STEP(S+1) (m6_pipeline R2).
- A first-divergence line (max 20 per building): seq, handle, both `hStatic`/alive values, the step's event types, and
  whether a restore or regrow (with its caller), a skipped event, an idle-local uid change or a net drop came since
  the last match.

#### 4.1.3 Measurement

- **Fingerprint(mover)**:
  - `S = get_shape` (`[m+0x14C]` iff `byte [m+0x154] == 1`, 0x1FB340 [R]), `n = [[S+0x3C]+0xC]` [N m6_measure].
  - Per subpiece, the 0x31AC20 alive test inline [R]: `test [sp+0x18],0x100000`; owner offset `[sp+8]` not 0 or -1; and
    `(sp+8)+[sp+8] == S`.
  - The result is `alive` and `hStatic` = FNV-1a (world.cpp `Fnv`) of the alive bitset.
- **Census(building)** at settle (10 s after the building's last event; a new event restarts the timer; not while
  paused):
  - the fingerprint;
  - `hPart` = FNV of the label array: STATIC; GONE; ORPHAN when the owner is not the shape of a live, non-fading
    (`+0xE0 & 0x800` clear) family mover, hashed as GONE but counted apart (fade-invariant: a census during a host fade
    matches one after it); else the lowest index sharing that owner (compared, never dereferenced). Chunk shapes share
    the building's subpiece buffer (m6_measure §3.1), so labels need no uid;
  - `chunks`, `gone`, `orphan`, `damage_pct` (+0xFC), destroyed (+0x10C bit 4), and the building position (+0x04);
  - 7.3 adds the top-5 chunks.
- **Step-0 check**: the host's `firstTouch` hash (the building at its first touch) must equal the replay side's
  fingerprint before it applies that step. A DIFF there means a handle or census bug, not non-determinism, and the run
  is INVALID, not FAIL; except when the building at the feed start already equalled the host's hash: then the replay
  changed it before its first touch (a real divergence, a static DIFF in that run). K3a reads INVALID (never FAIL or
  PASS) with an INVALID run (§5.3).
- **Once per process**: the thread id of the first 0x4102E0 call vs the tick's; the max ms in the hook per step.
- The dev=1 stat line gains `uid <[0x1A3733C]> chunks <type 2/1 movers with +0x134 != -1>`.

#### 4.1.4 Runner (F5, dev=1, free roam, no session)

1. **F5 #1**: `World_ArmSaveGuard("E4")`, `gc_profile = 1`, start recording, `World_CaptureStart()`. `Destr_Stamp()` in
   the 0x3D9159 chain marks the capture's seq; only later steps are replayed. HUD "E4: recording (F5 when done)".
2. The user demolishes, steps back ~20 m and presses **F5 #2**. Recording stops, the spot is noted, the HUD shows
   "E4: settling 10 s", and every touched building is censused (the reference). A mission start during the recording
   aborts the scenario.
3. **Runs** `near60`, `near30`, `far60`. Each run:
   - `Core_SetFpsCap(60|30)`;
   - `rand_regenerate(GetTickCount())` (0x17A290 [R]; a fresh pool, like another machine);
   - `gc_profile = gc_rfglGuest = gc_edf = 1`, mode replay;
   - `World_Load(blob, meta, false)`;
   - on the first gameplay frame, teleport near, or far = 150 m from the buildings' centre (the buildings within 100 m of
     the near spot) on the side away from the near spot, then `StepTeleport` until the terrain streams;
   - wait until every reference handle has a non-NULL shape (20 s max; at far, retry once at 100 m and log it);
   - 3 s of quiet (the step-0 check runs inside the feed, right before each building's first-touch step, §4.1.3: a
     step can change a building it does not target, `rfg_mover_delete_dynamic_link` 0x32EA46/0x32EA4A);
   - feed the records at live pacing (`hostMs`);
   - 10 s after the last record, census and compare.
   - A static DIFF adds 2 more runs of that condition (5 max).
4. Verdict lines and HUD "E4 done: K3a … K3b … (RfgCoop.log)". The bytes go back to 0. F5 starts a new scenario from the
   current world. The game must be restarted at the end (save guard).

**Log lines that decide** (A):
```
e4: recording from step 1043 (blob 1315236 B), profile on
e4: reference: 2 buildings (...), steps 611, events 2930 (types ...), despawns 37, ptr-like 0, restores 0 regrows 0
e4: thread 0x4102E0 5120 = tick 5120
e4 run 2/3 near30 seed 81234: step0 2/2 MATCH, static 2/2 MATCH, part 2/2 MATCH, uid 248/249 (99.6%, n/a 0, first diff step 1311 types 4 4 5), rfgl own==host 31/40, unmapped static 0 chunk 2 blob 0 unstreamed 0, catch-up max 3, hook max 2.8 ms
e4 verdict S1: K3a PASS (static 6/6 building-runs) K3b PASS (uid 99.4%, part 6/6) invalid 0 crash 0
```

### 4.2 ver7.1: live stream and resync layer (~400 lines, protocol 7)

**Status: implemented 2026-10-09**, the full build (C0 passed K3a and K3b, §5.3): `src/destruction.cpp` (the live
section on the shared record / replay code), `session.cpp` (epochs, the stale rule, F3, autoresync), `net.cpp` (caps, the
drop counter, the range-coded estimate, the dial HUD), `proto.h` (protocol 7), `puppet.cpp` (`Puppet_Handle`, the §6 flip,
the crush counter call), `RfgCoop.ini` keys. Builds warning-free; not run in game yet: card C1 is next. Messages, epochs,
the resync layer, every log line, card C1 and the 23 deviations from this section (each with its reason; the main ones: a
UIDS record per capture and resend so the guest sorts blob-loaded chunks as the host, RESTORE once per mover and epoch, capped
restore / regrow lines with per-caller counts): `notes/destruction_impl.md` §11.

If C0 ended in K3a (§5.3), 7.1 is built reduced: only F0's resync layer (mode 0, BLOB_BEGIN's byte, the stale rule, F3,
the census as a drift monitor, the net.cpp caps). Everything about `kDestr` below is then left out.

**Scope**
- **proto.h**: §3.2 (`kDestr`, `kCensus`, `kCensusRes`, `kResync`), BLOB_BEGIN + `u8 destr` + `u32 logPkts` (0 until
  7.2, so 7.2 needs no protocol bump), 39 -> 44 B, and the `static_assert` update.
- **net.cpp**: §3.3 (separate caps, shim exemption, one drop counter, pacing, byte counts, dial HUD).
- **destruction.cpp** by role:
  - host = record -> pack into `kDestr` (only while `g_blobSent && (g_hj == hjIdle || (g_hj == hjWant && !g_capMine))`:
    a blob only wanted, e.g. an accepted F3 waiting out a mission, keeps the live epoch streaming until its capture
    starts; the epoch is not stale, mode ≥ 1, not loading), at most 16 packets per tick (§3.3);
  - guest = receive -> the same replay FIFO as ver7.0.
  - `gc_profile` is 1 on both while linked with mode ≥ 1 and profile bit set; `gc_rfglGuest` is 1 on the guest in mode 2.
- **RESTORE**: every entry of the §4.1 item 5 log on the host also appends RESTORE {how, handle} to the stream. In live
  modes the guest marks that mover "restored on host": its later events are skipped (counted as `restored`) and its
  census says "no verdict (restored)". M9 turns RESTORE into an apply. The guest's own restores are logged as in 7.0.
  `object_mover::stream`'s restore (0x68B9F8) runs per machine at stream-in, so it is tagged by `how` and the return
  address in the log; if C1 shows it often, M9 decides whether it needs the record at all [I].
- **session.cpp**:
  - **The host's mode** goes into BLOB_BEGIN; a stale epoch forces mode 0 and bit 7. **A mode never changes inside an
    epoch** (§2).
  - **Epoch rules**: `g_gaHdr` is set only when the load starts (`session.cpp:1075`, again at 1089) [S]. While the blob
    is received (`g_rx`, line 528) and until the load starts (`g_pend`, line 551), `g_gaHdr.id` still holds the old
    epoch, and the 7.2 log packets arrive in exactly that window. So:
    - the guest keeps **`g_destrEpoch`**, set to `b.id` where BLOB_BEGIN is received (`session.cpp:527-528`, where `g_rx`
      is reset; the FIFO and the uid map are cleared there too). `DropBlobs` (`session.cpp:350`: host LOADING, link
      change) and `JoinReset` set it to 0;
    - `kDestr` and `kCensus` are buffered only with `epoch == g_destrEpoch`; other epochs are dropped and counted;
    - they are applied only after that blob has loaded: `g_gaHdr.id == g_destrEpoch && g_ga == gaIdle && !g_pend.on &&
      !g_hostLoading` (7.2 widens it to `g_ga == gaIdle || g_ga == gaReplay`, §4.3); any guest load (its LOADING
      edge, e.g. the warp-to-object video's in-RAM reload) closes it until the next blob lands;
    - a `pkt` gap or any net drop (§3.3) leads to `Session_RejoinRequest("destruction stream broke")`.
    - The host is paused from SendBlob to JOINED (0x3EDE51 / 0x3EDE5B [R]), so no step falls between the blob and the
      stream, and the per-step `uidBefore` replaces any counter field in BLOB_BEGIN.
  - **Stale rule**: the host counts non-empty steps (§3.1's test), its DESPAWNs and its `world::restore_object` calls
    (except the per-machine object_mover::stream 0x68B9F8 and kiosk level-init 0x734D1E callers: a mission start's
    restores run after its anchor is saved) since its last level load. On an in-mission REJOIN_REQ (`session.cpp:732`
    [S]) with a count > 0, the new epoch is stale (mode 0, bit 7; bit 6 with `destr_autoresync`). The host logs "stale
    re-join: S steps or restores since the load", and the guest's HUD says "Your world misses the host's changes since
    its mission began; it re-syncs when the mission ends" (bit 6) or "...; press F3 after the host's mission".
    - When `World_InMission()` falls with no load in between and `destr_autoresync=1`, the host makes a fresh blob
      (`hjWant`, `kBlobAsked`).
    - A host load clears the stale flag.
  - **F3 on the guest** (next to F7/F8, `session.cpp:1219` [S]) sends `kResync`. In free roam the host answers through
    `hjWant` / `kBlobAsked`. **In a mission F3 is refused and changes nothing** (the epoch stays live): the host answers
    `kJoinWait` with the reason "resync after the host's mission". Today `kJoinWait` always shows "you join after the
    host's mission" (`session.cpp:687-693` [S]); the host's F3 refusal carries the prefix `F3: ` in its reason, and only
    that one shows "Co-op: F3 resync refused: <reason>"; any other JOINWAIT (a REJOIN_REQ, autoresync or accepted-F3
    capture that met a mission) keeps ver6.4's text. F3 is refused locally while loading or landing and before the first
    join. (7.2 makes F3 in a mission a re-join with the log, §4.3.)
  - **Stat line every 5 s**:
    - `destr in/out, bytes raw/est, max/step, backlog, lag p50/p95` (one QPC clock on one PC; `est` = the offline
      range-coded size, §3.3);
    - `uid equal x/y`, `unmapped`, `skipped`, `restored`, `pending k (dropped d)`, `unpaired faded`, `local
      events dropped (driving k)`, `net drops`, `rfgl own==host`;
    - `echo p50/p95`: the guest's own hammer, bullet or detonation on mover H, to the first applied host step on H with
      owner 2.
  - **Charge log** (for Q26): when a Rhino charge (pack id 8, ver6.4's pack on/off) ends, B logs `charge: own X m`
    and A logs `charge: puppet of B X m`, plus the echo ms of the first host step on the wall it hit.
  - **Crush log for M7a** (dev=1, both games): `OnHumanDamage` (`puppet.cpp:306` [S]) counts `{victim kind (Player,
    puppet, NPC), attacker kind (NULL, mover, puppet, other), damage type}` while any building has a census timer
    running (an event in the last 10 s). The 5 s line prints the counters when they changed. M7a needs this data
    (m6_risks R14), and only a log can give it.
- **Census on both games**:
  - **live**: 10 s after a building's last event the host sends `kCensus`. The guest puts it into the replay FIFO and
    evaluates it when it reaches the head (after the steps and DESPAWNs before it), and checks:
    - **identity**: its object at that handle lies within 0.1 m of the host's position;
    - **static**: alive and `hStatic` equal;
    - **part**: `hPart` equal.
    - The verdict is identity + static. Part is printed beside it and read only for K3b (C1, §5.3). It logs, shows
      the HUD line "Destruction check: N MATCH, M DIFF, K no verdict" (a "no verdict" answer shows it too) and sends
      `kCensusRes`, so A shows it too (A's tally and HUD
      count live censuses only; resync-check answers are logged). B's line carries `chunks A x vs B y` (from
      `chunksAll`); A's carries B's count.
    - A mover marked restored gets "no verdict (restored)"; a building hit by an own-pack cutter (Q26) gets "no verdict
      (own-pack)".
  - **resync check**: at every JOINED the host sends one `kCensus` per tracked building (the newest 64). The verdict is
    **identity + static only**: chunks rebuilt by a blob load are not the host's runtime chunks (uid -1 on the guest;
    movers above 0.999 damage may not be saved at all, OSM_NONE, m6_code [N]; the loader's chunk ownership is [I]), so
    `hPart` can differ after a perfect load. The guest logs `census: resync check (blob N, fresh|stale): k buildings, k
    MATCH, 0 DIFF, 0 not loaded, j no verdict, part k/m`.
  - **Sanity**, once per session: the first census of an undamaged building must say alive == n; the first resync check
    adds `resync part MATCH x/y`, which shows whether part can ever be used across a load.
  - A building that is not streamed is retried every 2 s for 60 s (unpaused: a pause cannot stream it in, so the
    resume shifts this clock and the pending queue's), then gets "no verdict" (the resync check too; its
    summary line follows its last answer). A building that lost host events this epoch (pending events dropped, mode
    1's dropped debris impacts, host events skipped because a side is an unpaired or unmapped chunk, a zone mover
    missing here or a restored mover: every loaded building the event names, both sides of a type 4, as §4.1.2's E4
    rule) gets "no verdict" until the next epoch.
  - In mode 0 a DIFF shows the hint "World drifted (k buildings): F3 re-syncs in the host's free roam" (k = distinct
    buildings with a DIFF this epoch).
- **puppet.cpp**:
  - export `Puppet_Handle()`;
  - the **Q19 flip** (§6);
  - **G1/G2 stay uninstalled**.
- **Pending queue (+25)**: events on a target that is not streamed (`+0x154 != 1`) wait in a per-mover list (m6_risks
  R8). C1 step 6 needs it; C0's far60 `unstreamed` count says how much. The rule:
  - when the shape streams in, its pending events run as a **synthetic STEP inside the 0x4102E0 hook** (never outside
    the Havok callback). Mode 2 runs it **right after a host step** (before the DESPAWNs behind it), never while no
    host step is due (§0.3: an idle call shardifies deletion-fed candidates early); only an idle host (no STEP applied
    for 1 s, none due) lets it run at the next hook call, since an idle host records no steps to carry it. The host
    step after a synthetic step gets uid `n/a`. Mode 1 runs it at the next hook call, before any host step, and skips
    its idle count-0 call in that callback (the original runs once);
  - a live census of a building with pending events (any event naming it, either side of a type 4) is retried (every
    2 s) until they ran, "no verdict (events pending)" at 60 s, so it never judges a building whose host events are
    still queued; a retry judged after a host step newer than the census named the building gets "no verdict (newer
    host events)";
  - the synthetic step runs with the uid counter set to a private base (0x40000000 + a running count) and its uid range
    is marked unpaired (no pairing attempt); the next host step reseeds as usual. Its rfgl calls keep the local value (no
    host tuple exists for it). Uids are only identities here [I];
  - a newer live event on a building that still has pending events joins that building's list behind them, so
    per-building order holds;
  - after 60 s of unpaused time (or the 4096 cap, or at once when the mover they wait on is gone) pending events are dropped and
    counted, and every loaded building they name gets no census verdict this epoch.
- **Own-pack option (Q26, `destr_ownpack`, default 0)**: with 1 (stamp bit 5), B keeps its own type-6 cutter events (`shape_cutter::do_cut` writes type 6 at 0x328657 [N destruction.md:57])
  (owner = B's `Player`) in the replay hook instead of dropping them, the host leaves their owner-2 echo out of `kDestr`,
  and those buildings are excluded from the census (`own-pack`). Their chunks are unpaired on both sides. The host still
  applies those cutters in its step, so that STEP's uids and rfgl tuples include theirs: it carries `flags` bit 0 (the
  unused STEP field, no protocol change), every FIXED building it targets is own-pack, and the guest leaves all of that
  step's chunks unpaired, keeps its own rfgl values and gives it uid `n/a`. In mode 2 the host step after B's own local
  step gets uid `n/a` too (as after a synthetic step).
- **Packages**: both inis get `lag_ms=30` (about 60 ms RTT, like Radmin), `fps_cap=60` and `dev=1`.

**Log lines that decide** (B unless marked):
- `destr: mode 2 from blob N (profile on, pack world bit off)`;
- `census: <h> seq S: identity OK static MATCH (part MATCH) chunks A x vs B y`;
- A: `destr: B checked <h>: ...`;
- `census: resync check ... 0 DIFF, part k/m` and once `resync part MATCH x/y`;
- the 5 s `destr:` line (incl. `restored`, `pending`, `unpaired faded`, the crush counters);
- A: `session: stale re-join: S steps since the load`;
- `net: drops 0`;
- no `CRASH`.

### 4.3 ver7.2: re-join with holes and general movers (~230 lines)

**Implemented 2026-10-12 inline (no workflow), simplified and extended: see destruction_impl.md §12** (safehouse repair,
trust boundary, GM_DEAD, re-join log without a gaReplay phase; protocol 8).

**Scope**
- **World-message log on the host** (m6_risks §3.1):
  - every record byte (STEP, DESPAWN, GM_DEAD, RESTORE) since the host's last level load, where the uid also restarts
    (0x32D988 [R]);
  - **`Destr_Stamp()`** around the original in the 0x3D9159 chain (`OnProcessSaveLoad`, `world.cpp:146` [S]). That chain
    is installed today only on the first capture request (`world.cpp:165`), so 7.2 installs it at link. The buffers are
    `stored_world_state[2]` at `World+0x257C`, stride 0x6C (`+0x60` buf, `+0x64` cur_size), and `cur_world_state` is
    `World+0x2578` [S world.cpp:34, world_save.md:171-173]. The stamp:
    - before the original, notes both buffers' `cur_size` and the `WFlags` save/load bits (bit 2 = pending_save_game,
      `world.cpp:152` [S]; the load bits are [I]);
    - after it, if a bit was set or a size changed, hashes both buffers with `Fnv`, as world.cpp already does for
      buffer 0 (`g_cap.oldSize` / `oldHash`, `world.cpp:149-150` [S]). A buffer whose size or hash changed gets the
      entry `{hash -> current log offset}`. A level load restarts the log at offset 0 and stamps both buffers at its
      first handshake;
    - so the stamp never guesses which save rewrote which buffer: it sees the bytes change.
  - on an in-mission resend (a death REJOIN_REQ, or **F3 in a mission**, which now takes the same path) the host hashes
    the buffer actually sent (`World_CurrentState`, `world.cpp:305` [S]) and looks it up. A hit: after the blob and
    under the new epoch it sends the log from that offset (pkt 1..N, `logPkts = N` in BLOB_BEGIN), paced at 16 packets
    per tick (§3.3); the live stream continues at N+1. A miss: the 7.1 stale rule plus `destr: no log base for the sent
    buffer (hash X): stale`, never a guessed base;
  - **trim**: log bytes below the oldest offset still referenced by a buffer whose current hash matches its entry are
    dropped, so a long free-roam session keeps only what a resend could need;
  - over a 16 MB cap after the trim: log a line and fall back to the stale rule for that re-join;
  - the step-0 check runs after every re-join and turns a wrong base into a DIFF line, not silent drift.
- **Guest, `gaReplay`**: a new phase in `Guest_Tick` between the end of the teleport hold and JOINED. After a load the
  guest stands at the load's own spawn (the nearest safehouse, `session.cpp` gaArrive [S]); only gaTeleport /
  StepTeleport brings it to the host and streams that area (`session.cpp:1118-1145` [S]). So:
  - gaTeleport, when `g_tp.active` is false, computes the distance as today and goes to **gaReplay** instead of sending
    JOINED;
  - in gaReplay the step hook drains the log at `destr_catchup` steps per physics step. A step whose targets have no
    shape yet waits, as the E4 runner waits (each target up to 20 s, then it goes to the pending queue);
  - JOINED goes out when all `logPkts` packets are applied, or after 60 s in gaReplay (the rest continues in order under
    the normal gate). The host stays paused meanwhile; its 120 s fallback is unchanged [N m6_code];
  - the apply gate becomes `g_ga == gaIdle || g_ga == gaReplay`. The guest is not paused by its token in gaReplay
    (`session.cpp:1298` pauses only at `g_ga == gaIdle` [S]). gaReplay leaves to gaIdle like gaTeleport when Player is
    NULL, a newer blob waits or the host loads, and the death check at `session.cpp:1067` includes it;
  - log line `destr: replayed N records (base load|save|capture) in X ms, waited Y ms for shapes`.
- **Mid-mission epochs are live, not stale**, while the log has a base for the sent buffer and is under the cap.
- **GF-GM + GM_DEAD** (`destr_gm`, mode 2):
  - guest: operand 0x68A925 (site 0x68A923 `cmp byte ptr [Multiplayer],0x1` [R]) -> `gc_gm`. Local damage to general
    movers is then swallowed (`mov al,1` at 0x68A942 [R]) unless the last bool is set; this relies on `Server` = 0,
    which is logged since M0;
  - host: swap `general_mover` vftable slot 34 at 0xF14064 (check that it holds `g_base+0x68A920` before writing [N live
    N8, on-disk read]). After the original returns, a 0 -> 1 edge of `general_mover::is_killed` 0x612D50 appends GM_DEAD
    {handle, hostMs} to the stream;
  - guest (tick): `general_mover::apply_damage` 0x68A920 (thiscall, 7 args) with lethal damage and the last bool = 1,
    logged as `gm dead applied <h> lag X ms` (NowMs minus hostMs, one QPC clock on one PC). The `damaged_by_types`
    value and the meaning of arg 6 are [I]; the 7.2 workflow reads them.

### 4.4 ver7.3: chunk pose lane and M6 sign-off (~150 lines)

- **Host, 10 Hz**: the 10 largest paired chunks (by subpiece count) that moved in the last second, `{uid, handle, pos,
  quat, lin}`, on channel 3 with the four net.cpp edits of §3.3.
- **Guest**, from puppet.cpp's existing `OnSimFrame` chain at 0x3EE55C (one call into `Destr_BeforeSim`, no second chain;
  outside the Havok step, m6_pipeline R9):
  - map each entry by uid, then by handle (so chunks loaded from a blob after a mid-collapse join are corrected too);
  - an error above 0.5 m -> `havok_body_set_pos_orient` 0x423790 (`cdecl(int havok_handle, vector*, matrix*)` [N
    m6_pipeline]);
  - a smaller error -> velocity toward the target, via `havok_body_set_linear_velocity` 0x423870 (signature [I]);
  - the Havok handle is `object+0x4C` (0x339F3A [N]).
- **Census** adds the top-5 chunks `{label, count, pos}`, so the guest logs the per-label rest error, `lane corrections >
  0.5 m: N` and one verdict line `lane: k/m buildings with 4/5 top chunks ≤ 1 m`.
- **ini** `destr_lane` 0/1.

---

## 5. Test cards (one PC, A = host `<GOG Galaxy folder>`, B = guest `<game copy B>`)

### 5.1 The cards, one after each build (C0 after 7.0, C1 after 7.1, C2 after 7.2, C3 after 7.3)

Both games run with `dev=1` and the build's defaults unless a card says otherwise. After each card the user restarts
both games and sends both `RfgCoop.log` files. Each card's result gates the next build (§5.3).

**C0: E4, one game (A only, ~15-20 min, ~4 min hands-on)**
- **Setup**: A alone, no `role`, any free-roam save past the Tutorial. Pick buildings that are not mission targets, not
  next to mining targets or supply crates, and not in a district near zero EDF control.
- **S1**: walk to a 2-3 storey building, press **F5**, hammer it and detonate 2-3 remote charges (~1 min), step back
  20 m, press **F5**. Leave the game alone until "E4 done" (~5 min). The world reloads 3-5 times, you appear near or
  150 m away, and the holes appear by themselves.
- **S2**: another building. **F5**, drive a heavy vehicle into it 2-3 times, then bring a section down with charges so
  debris falls on the rest, step back, **F5**, wait.
- **S3**: **F5**, Rhino through two walls, then Tremor next to it, step back, **F5**, wait.
- Restart the game. **Report**: did the holes come back the same in each run? Any crash?
- **Pass**: for each scenario, `K3a PASS` (no building has a static DIFF in ≥ 2 of its runs, no crash, step-0 MATCH in
  every run) and `K3b PASS` (uid ≥ 95% of chunk-creating steps with no skipped event, part MATCH in all but at
  most one run). K3a counts only buildings that received every host event naming them in that run: one named by a
  dropped event (a chunk never made here, or unstreamed) is `no verdict`; the run line's `unmapped ... chunk N` still
  shows that cascade.
- **Information only**:
  - `rfgl own==host` below 100% confirms that the host-value override is needed;
  - `unstreamed > 0` (and `uid ... n/a > 0`) at far60 confirms the pending queue;
  - `ptr-like > 0` names a type that needs a remap before C1 means anything.

**C1: live holes and chunks (A + B, ~10 min)**
- **Setup**: B's ini `fps_cap=30` (the slower-guest case).
- **Steps**:
  1. Start A (free-roam save) and B. F8 on A, F8 on B, CONNECT. B joins. Stand together near 4 small buildings that are
     not mission targets.
  2. **A** hammers building 1 through a wall and detonates 2 charges. Wait for the HUD line on both (~10-15 s).
  3. **B** hammers building 2 and detonates 2 charges. B's own hits land a moment later (expected). Wait for the HUD
     line.
  4. **A** (Rhino) charges through a wall of building 3, then Tremor or Shockwave beside it. Wait for the HUD line.
  5. **B** opens Esc and stays there 30 s while **A** hammers building 4, then B closes Esc. Wait for the HUD line.
  6. **B** walks ~150 m away facing away; **A** charges building 1 again; B walks back (F7). Wait for the HUD line (when
     B's 5 s line shows `applied p > 0`, building 1 is expected to read "no verdict (host events skipped ...)", so the
     line's `no verdict` count rises; this step's evidence is `applied p > 0` with no crash).
  7. **B** Rhino-charges a wall of building 2 twice, then Thrusts into one. Did B's charge stop at the wall before the
     hole appeared? Wait for the HUD line on both.
  8. **B** presses **F3** (resync) and lands beside A.
- **Report**: the same holes in both windows? Anything doubled, jumping or popping? How late did B's own hits feel?
  Did B's Rhino stop at walls? Crash?
- **Pass**:
  - every census `identity OK` and `static MATCH` (restored and own-pack movers and buildings that lost host events,
    §4.2, are "no verdict"), uid ≥ 95% in each `destr: blob N ends` line; part is the K3b reading: among B's judged
    live `census: <h> seq S:` lines (not resync check, not no verdict), at most one says `part DIFF` (2 or more = K3b,
    §5.3);
  - `resync check ... 0 DIFF` after step 8 (static); `resync part MATCH x/y` is information;
  - `destr: backlog drained` within 5 s of B's `destr: game resumed`, 0 gaps, `net: drops 0`;
  - echo p95 < 0.5 s;
  - no crash, and B's `.sav` time unchanged (standing rule).
- **Information**: `charge: own X m` on B vs `charge: puppet of B X m` on A (step 7) answers Q26; the crush counters;
  `restored` (which callers, from the log's return addresses).

**C2: re-join with holes, gas tanks (A + B, ~10 min)**
1. Joined. A starts an early story mission and blows up a non-target building in it. B jumps from a height and dies,
   then re-joins (mid-mission). Look: is the building broken on B too?
2. A shoots a gas tank next to a building; B watches. Then B shoots another one (its puppet on A does it).
3. Back in free roam, A brings down a tall building; while it is still falling, B presses F8 twice (leave, join).
4. A demolishes something, then fast travels; B follows.
5. In a mission, B presses F3 (same path as step 1's death re-join).
- **Pass**:
  - B `destr: replayed N records (base ...)`, then step-0 MATCH and census static MATCH for the buildings broken before
    each re-join (read `restored` first: a mission start restores movers, §5.3);
  - `gm dead applied <h> lag X ms` for every tank, p95 < 500 ms (the 5 s line prints `gm lag p95`);
  - no `destr: no log base` line unless the log also names the miss;
  - no liberation or district message on B (`GF-EDF` count);
  - no crash.

**C3: pose lane and M6 sign-off (A + B, ~15 min)**
1. Joined, both 30 m from a tall structure (smokestack, tower, 3-storey building). A brings it down with charges. Do the
   big pieces fall and rest in the same places? Does anything jump?
2. B walks through the gap A walked through.
3. 10 min of free play: each player demolishes 2-3 buildings with any tools (no guest vehicle ram: that needs M8).
4. Rate the drift (none / small / bad).
- **Pass (M6 acceptance, shortened for one PC)**:
  - static MATCH everywhere, uid ≥ 95%;
  - the verdict line `lane: k/m buildings with 4/5 top chunks ≤ 1 m` with k/m ≥ 80%;
  - `chunks A x vs B y` on the last census: B ≤ A + 10% (unpaired chunks do not pile up);
  - no popping seen;
  - 15 min without a crash.

**F0: fallback card (only if C0 or C1 sends M6 to mode 0)**
- **Setup**: A's ini `destr_sync=0`.
- **Steps**:
  1. Join. Each player hammers a building.
  2. B presses F3.
  3. A starts a mission; B dies and re-joins.
  4. A finishes or cancels the mission.
- **Pass**: `resync check 0 DIFF` (static) after steps 2 and 4; step 3 logs `stale` with mode 0; no district message on
  B.

### 5.2 Final two-PC run

C1 steps 1-4 over Radmin/Tailscale. The same lines are read (identity, static, part, uid %, raw/est KB/s). The lag line
means nothing across two clocks; use RTT plus B's queue delay and A's `backlog` / `ENet queue` (the host's 5 s line).
This is the only check of cross-CPU float (m6_risks R19)
and of different image bases.

### 5.3 Decision tree (the first failing verdict sets the shipped default)

| Result | Consequence |
|---|---|
| C0 K3a PASS, K3b PASS | continue with C1 at `destr_sync=2` |
| C0 K3a PASS, K3b FAIL | A's ini `destr_sync=1`, `destr_lane=0`; run C1 (part/uid are information only; static holds only for buildings no host debris hit, the rest read "no verdict (debris local)", §2), C2 without the GM part |
| C0 K3a FAIL with one clear cause (the first divergence always comes after one event type, `ptr-like > 0`, or a regrow or skip named in the line) | **one** diagnosis build ver7.0.1 (type filter/remap, `e4_types` bisect, or DP-RNG around the apply); repeat C0 |
| C0 K3a FAIL otherwise, or again after 7.0.1 | **K3a**: 7.1 is built reduced (F0's resync layer only, §4.2), `destr_sync=0` is the shipped default; run F0 after it; M6 ends there; go to M7a |
| C0 K3a INVALID (a step-0 DIFF in any run, or no run counted) | not a kill: a census or handle bug; fix it before reading K3 |
| C1 static DIFF with C0 passed | a cross-process cause (handle identity, pointer field): one diagnosis round, else K3a |
| C1 crash inside the replay | `destr_catchup=1` first; still crashing -> one fix round, else K3a |
| C1 part DIFF in ≥ 2 or uid < 95% | **K3b**: `destr_sync=1`, `destr_lane=0` (mode 1: buildings hit by host debris get "no verdict (debris local)", §2) |
| C2 re-join DIFF | first check `restored` and the census "no verdict (restored)" count, and whether the DIFF mover had a RESTORE in the log: then it is the known restore gap (M9), not a base error. Otherwise the log base is wrong: `destr_autoresync=1` with the stale rule covers it until a fix |
| C2 tank mismatch or GM crash | `destr_gm=0` (tanks stay parallel) |
| C3 rest error > 1 m or popping | **K3c**: `destr_lane=0` (settled drift until the next resync) |

The table is read after each card, before the next build starts. Before blaming K3a for any static DIFF (C1 or C2),
check the `restored` count and the first-divergence line's restore/regrow note.

---

## 6. The gc_ppWorld flip (Q19)

- The host always keeps 1 (`kPolicyHost`, `puppet.cpp:213` [S]). Its world shows the guest's pack holes through the
  guest's puppet, and the 0x4102E0 capture streams them.
- On the guest the world bit follows the epoch's mode. It is set at landing and at every new BLOB_BEGIN, in
  `Puppet_Net` / `Puppet_SetWorldBit(mode)` instead of the constant at `puppet.cpp:1248` [S]:

| Guest epoch | world bit | Effect on B |
|---|---|---|
| mode 0 (parallel) or stale | **1** (Q19 as today) | the host's puppet's Rhino/Thrust, quake and blasts break B's copy, like its guns |
| mode 1 or 2 (live) | **0** (`gc_ppWorld` bit 8 set; bit 0 stays 1) | G6 lets the host's puppet pass through walls (rfg_movers only: general movers still stop it and break locally until 7.2) that are still whole on B for ~RTT; G7 still makes the end cutter (its holes are queue events, dropped). The holes arrive from A's stream; the blast effects stay (G1/G2 not installed) |

The pack's queue events on B are dropped in live modes anyway. The bit stops only G6's rfg_mover pass-through, the one
non-queue path to a building [N m6_pipeline R11]. The end cutter (G7) reaches buildings only through the queue
(shape_cutter::do_cut 0x328380: rfg_movers as type-6 collision events under the lock, 0x3285F8-0x3286E6; other movers an
impulse, 0x32876B; apply_explosion 0x32841D: movers, vehicles, debris, items, humans) [S], so it stays on bit 0, which
only the quiet ends clear: in a live epoch the host's puppet's end blast hits general movers, vehicles, debris and items
on B as A's own does on A, and Replay drops its holes (OnFade skips its mode-2 rfg_mover fades).

---

## 7. Sites

All were re-read this pass in `rfg.asm` (VA shown) or the symbol table unless the status column says otherwise.

| RVA | Cache line / symbol | Build | Use | Status |
|---|---|---|---|---|
| 0x4102E0 | `8102e0: call 0x740710`, `8102e5: ret 0x8`; `game_post_collide_listener::postCollideCallback` | 7.0 | step chain (record / replay) | [R] |
| 0x340710 | `rfg_dyn_do_collide_post`; `740787: mov edx,[0x1ef7268]`, `740793: mov [0x1ef726c],ebx`, `7407b0: test byte [edi+0xc8],0x3`, `740968: cmp dword [esi+0x48],0`, `74096e: test byte [esi+0x18],0x1`, `740a2b: mov [0x1ef7268],0` | 7.0 | count, skip bits, post pass, reset | [R] |
| 0x3408E2-0x340944 | calls 0x324930, 0x315030, 0x338040, 0x33CD70, 0x33F660, 0x32E250, 0x315070 (shardify stages) | 7.0 | what the mode-2 idle skip defers | [R] |
| 0x31331E | `call 0x58ac30` (= `cs_wrapper::enter` 0x18AC30; leave 0x18AC40), ecx = `Collision_event_wrapper` 0x1B16350 | 7.0 | queue lock | [R] |
| 0x339C5A | `cmp eax,0x400` | - | queue cap 1024 (why the guest writes from index 0) | [R] |
| 0x3383FD | `7383fd: call 0x71d000` (`rfgl_desired_values`, only caller) | 7.0 | rfgl chain | [R] |
| 0x31D000 | writes no global (0x31D000-0x31D100); `RFG_load_balance` 0 -> {0,0} at 0x31D020 | 7.0 | shadow call is safe; never clear the byte | [N live N6, m6_risks F4] |
| 0x339EA3, 0x339FE9, 0x33811A, 0x3249AC, 0x332772 | each `cmp byte ptr [0x2fea588],0x0` | 7.0 | `gc_profile` (operand at site+2) | [R] |
| 0x325051 | `725051: cmp byte ptr [0x2fea588],0x0` … `725061: jne 0x725209`; `multi_is_server` 0x5431D0 = `9431d0: mov al,[0x2fea590]` | 7.0 | `gc_rfglGuest` (operand 0x325053) | [R] |
| 0x3B0024 | `7b0024: cmp byte ptr [0x2fea588],0x0`, `7b0043: jne 0x7b035b` | 7.0 | GF-EDF, `gc_edf` (operand 0x3B0026) | [R] |
| 0x3249BF | `7249bf: push 0x714ff0` (`rfg_mover_list_compare` 0x314FF0, its only reference), `7249c8: call [qsort]`; reached only with the 0x3249AC operand on | 7.0 | replay comparator: blob-loaded chunks sort by their host uid (`destruction_impl.md` §3) | [R] |
| 0x324ACC | `724acc: call 0xa139a0` (`object_mover_mark_for_fade` 0x6139A0) | 7.0 | host DESPAWN (fade, ms) | [R] |
| 0x324ADE | `724ade: call 0x91cef0` (`world::destroy_object` 0x51CEF0) | 7.0 | host DESPAWN (destroy) | [R] |
| 0x6A2657 | `aa2655: push edx`, `aa2656: push esi`, `aa2657: call 0xa139a0`, `aa265c: add esp,0x8` (`object_mover_process_explosion_hit`, SP; rfg_movers only from a Duration explosion, `+0x28 & 0x8000`) | 7.0 | host DESPAWN (fade, ms) | [R] |
| 0x7CD092 | `bcd08f: push 0x0`, `bcd091: push ecx`, `bcd092: call 0xa139a0`, `bcd097: add esp,0x8` (`do_debris_cleanup`) | 7.0 | host DESPAWN (fade, 0 ms) | [R] |
| 0x3249E0 callers | `724c7c`, `724ca2`, `725245`, `725253`: `call 0x7249e0` | - | both despawn chains see every caller (incl. emergency cleanup) | [R] |
| 0x324A9C | `724a9c: call 0xa13960` | 7.0 | the guest's "can fade" test | [R] |
| 0x33A94C | `73a944: test eax,0x80000`, `73a94b: push edi`, `73a94c: call 0x735dc0` (`rfg_mover_regrow_begin` 0x335DC0) | 7.0 | regrow log, RESTORE how 1 | [R]; only caller [N live N11] |
| 0x33A92E | `73a926: test eax,0x100000`, `73a92d: push edi`, `73a92e: call 0x7360e0`, `73a939: add esp,0x4` (`rfg_mover_regrow_finish` 0x3360E0, cdecl(mover)) | 7.0 | regrow log, RESTORE how 2 | [R] |
| 0x534B70 | `934b70: mov ecx,dword ptr [0x2cca67c]`, `934b76: push esi`, `934b77: mov esi,dword ptr [esp+0x8]`, `934b86: ret 0x4` (`world::restore_object`) | 7.0 | entry detour (6 stolen bytes, operand relocated), RESTORE how 0 | [R] |
| 0x33A985 | `73a96a: test eax,0x400000`, `73a971: and eax,0xffbfffff`, `73a97c: mov eax,[edi+0x6c]`, `73a97f: push eax`, `73a985: call 0x934b70` | 7.0 | the frame-pre caller (handle = mover+0x6C) | [R] |
| 0x68B9F8 | `a8b9cb: test dword ptr [edi+0xe0],0x10000000`, `a8b9d9: call 0x755f90`, `a8b9e8: and dword ptr [edi+0xe0],0xefffffff`, `a8b9f8: call 0x934b70` (`object_mover::stream`) | 7.0 | per-machine restore at stream-in (logged by caller) | [R] |
| 0x3751CF / 0x721EB6 | `7751cf: call 0x934b70` (`restore_list_do_restore`), `b21eb6: call 0x934b70` (`Action_callback_restore_object`) | 7.0 | mission-start and script restores (host only) | [R]; the other callers 0x317309, 0x385D0F, 0x3A6D82, 0x3B5FB0, 0x72E76E, 0x734D1E from the xref table, call lines [R] |
| 0x33D298 / 0x32D988 | `73d298: lock` (xadd on 0x1A3733C); `72d988: mov dword ptr [0x1e3733c],0x0` | 7.0 | uid counter, reseed per step; reset at level init | [R] |
| 0x33D2C8 | `73d2c8: call 0x93e550` (`world::create_object_internal`) | - | not used (the uid walk replaces a chain; upgrade path if the walk is slow) | [R] |
| 0x325DDE / 0x325E2D | uid -1 for loaded movers | 7.0 | chunk vs zone target | [N e4first, live N12] |
| 0x53BEE0 | `world::object_from_handle` | 7.0 | target lookup | [R symbols] |
| 0x1FB340 | `5fb340: cmp byte ptr [ecx+0x154],0x1` (`get_shape`) | 7.0 | fingerprint, streamed test | [R] |
| 0x31AC20 | `71ac24: mov edx,[ecx+0x24]`, `71ac27: lea eax,[eax+8*eax]`, `71ac2a: test [edx+4*eax+0x18],0x100000`, `71ac37: mov eax,[edx+0x8]`, `71ac3a: cmp eax,-0x1` | 7.0 | alive test (inline copy) | [R] |
| shape +0x3C -> +0xC | `num_objects` | 7.0 | subpiece count | [N m6_measure, PDB] |
| 0x313779 / 0x313796 | `mov [esi+0xc4],eax` (owner handle or -1) / `mov [esi+0xb8],ecx` (explosion id) | 7.0 | the explosion prefix holds no pointer | [R] |
| 0x17A290 | `rand_regenerate` (`57a290: mov eax,[esp+0x4]`) | 7.0 | runner: a fresh pool per run | [R] |
| 0x3D9159 | `7d9159: call 0x7c00e0` in `gamestate_gp_handshake` 0x3D9140 (world.cpp's chain) | 7.0 | `Destr_Stamp` | [R] |
| 0x3EDE51 / 0x3EDE5B | `mov eax,[0x2532880]` / `jg 0x7ee4dd` | 7.1 | the host is paused while a blob is out: a clean stream cut | [R] |
| 0xF14064 | `general_mover` vftable slot 34 = VA 0xA8A920 on disk | 7.2 | host GM_DEAD hook (byte-checked) | [N live N8] |
| 0x68A923 | `a8a923: cmp byte ptr [0x2fea588],0x1`, `a8a942: mov al,0x1` | 7.2 | GF-GM, `gc_gm` (operand 0x68A925) | [R] |
| 0x68A920 / 0x612D50 | `general_mover::apply_damage` / `general_mover::is_killed` | 7.2 | GM_DEAD edge and apply | [R symbols]; arg values [I] |
| 0x3EE55C | `7ee55c: call 0x7edb60` (puppet.cpp's `OnSimFrame` chain) | 7.3 | lane apply, outside Havok | [R] |
| 0x423790 / 0x423870 | `havok_body_set_pos_orient` / `_linear_velocity` (`823790: push ebp`, `823870: push ebp`) | 7.3 | pose lane | [R symbols]; signature of 0x423790 [N], 0x423870 [I] |

Never touched: the `create_shard` MP sites, 0x312670, 0x3383CF, 0x33830F, 0x33D261, `RFG_load_balance` 0x124F0FB,
G1/G2 (0x558323 / 0x558281). **RESEARCH.md's site row for 0x124F0FB (§ sites, line 560: "0 on the guest (no local
debris deletions)") and its §4 text at line 316 ("On the guest clear `RFG_load_balance` 0x124F0FB") are superseded**:
clearing the byte makes `rfgl_desired_values` return {0,0} (0x31D020 -> 0x31D0C1, m6_pipeline R1). The correct row
reads: "0x325051 operand lane on the guest (`gc_rfglGuest`, skips its `rfgl_do_frame`); never clear 0x124F0FB".

---

## 8. Risks

1. **What only the two-PC run can show** (cross-CPU float, m6_risks R19; real bandwidth and RTT; different image
   bases). Every other verdict comes from the one-PC card after its build (§1). Mitigations:
   - every site is byte-checked, with no write unless all match;
   - `destr_sync=0` restores ver6.4 (plus GF-EDF) with the same binary;
   - raw bytes plus the offline range-coded estimate, and the census, run in the two-PC session by themselves.
2. **Same process hides cross-process faults** (C0): heap or image pointers in prefixes, handle equality. Mitigations:
   the pointer probe (C0), the identity check and the resync check (C1), and the two-PC run (§5.2).
3. **Repeated calls of the original in one callback** (catch-up) are [I]. near30 and C1's B at 30 fps exercise them, and
   `destr_catchup=1` is the knob. MP JIP calls 0x340710 from the tick (0x5DE469 [N]).
4. **Mode-2 idle skip** defers local shardify candidates to the next host step [I: no other per-step bookkeeping found
   in 0x340710's call list, §7]. The uid delta and first-divergence lines show any effect.
5. **Deletion timing** (m6_pipeline R2): fade-end destroys and dynamic-link shardify can fall in different steps. The
   per-step uid reseed stops a mismatch from spreading; K3b is judged at 95%. DESPAWN `dt` (§3.1) keeps a quiet-stretch
   fade from starting early in E4.
6. **Far camera** (shape not streamed): events are skipped, giving a DIFF. Covered by the pending queue (7.1) and
   measured by C0 far60 and C1 step 6. In C0 such a building gets no verdict and such steps are uid `n/a`, so the gap
   does not fail K3a or K3b.
7. **The census reads engine internals** (owner offset, dead flag, `num_objects`). The alive test is [R] at 0x31AC20;
   the step-0 check, the sanity line and the resync check validate the rest before any K3 verdict counts.
8. **Stale mid-mission world**: mode 0 plus auto-resync (7.1), then the log (7.2). The log base comes from the bytes
   of the buffer actually sent (§4.3 hash lookup), not from a guess about which save rewrote it; a miss falls back to
   stale with a log line, and the step-0 check catches anything else.
9. **Guest actions the host never sees** make no holes in live modes until M8: vehicle rams, seat or turret shots,
   charge-trigger weapons, actions while the puppet is hidden. The cards exclude them; Q25.
10. **Host consequences during C0's recording** (district control, liberation) are real on A. Pick buildings away from
    near-zero districts; the save guard keeps nothing.
11. **`player_damage_only`** (mining targets, supply crates, 20 m from the local Player) diverges [N m6_pipeline R3].
    Accepted (rare, salvage only) and visible as a census DIFF on such movers.
12. **Restores and regrows** (`world::restore_object` from mission starts, scripts, activity cleanups and stream-in;
    regrow begin/finish) are recorded as RESTORE from 7.1 and applied only from M9 (a regrow begin only when it ran:
    0x335DC0 returns false with no change for a mover on screen, retried every frame; C0 had 384 begins, 0 finishes). Until then the guest skips events on
    a restored mover and the census says "no verdict (restored)", so host events never land on a guest building with a
    different shape [I: crash risk if they did]. A resync repairs them.
13. **One-PC numbers**: optimistic for bandwidth and lag (loopback), pessimistic for the host's balancer (shared CPU).
    The census and stats stay on, so the two-PC run reports by itself.
14. **Salvage double pay** (Q20): a replayed shardify spawns salvage on both games. Default: accept.
15. **B's own Rhino/Thrust in live modes** (m6_risks R9): B's cutter events are dropped, and B's own player keeps Havok
    collision with the wall that is still whole on B [I], so B's charge may stop until the host's hole arrives (RTT + ~100
    ms), while A's puppet of B goes through. C1 step 7 measures it; Q26.
16. **District control frozen on B**: GF-EDF (from 7.0) stops B's own changes, and nothing mirrors the host's until a
    re-join or M9's DISTRICT record (kind 5 reserved). B's district control stays at the blob's value in between.
17. **Unpaired chunks in mode 2**: chunks from a uid-mismatched step or a pending synthetic step have no host DESPAWN;
    they are faded after 30 s (§4.1.2), and C3 checks `chunks A vs B`.

## 9. Not built (each with its trigger)

| Item | Build it only if |
|---|---|
| DP-RNG (a private random stream) | C0's first divergences sit in steps whose types reach the profile-removed branches |
| the `create_object_internal` chain at 0x33D2C8 for pairing | the `all_objects` walk per chunk-creating step shows in `hook max ms` |
| a single-building live resync (probe P7) | K3a fails and whole-blob resyncs are too disruptive |
| staged's deletion poll | only as a census-time count of host chunks removed by other paths, if C1's DIFFs need it |
| a fix for `player_damage_only` (detour at 0x316AD0) | the user reports salvage or crate mismatches |

---

## 10. Questions only the user can answer (a default is used if there is no answer)

- **Q20 (salvage)**: a building can pay salvage on both screens, and B's pickups go to A's wallet (cap 30,000).
  **Default: accept** (zero code). The alternative deletes the guest's destruction salvage after the roll.
- **Q21 (stale resync)**: when the guest re-joined mid-mission and the host's mission ends, re-sync automatically (about
  a 10 s pause for both), or only when the guest presses F3? **Default: automatic** (`destr_autoresync=1`).
- **Q22 (testing)**: each build gets one short one-PC card before the next build (C0 after 7.0, which is one game; C1
  after 7.1; C2 after 7.2; C3 after 7.3), and only the two-PC run waits until everything is finished. **Default: yes**
  (this is how RESUME.md and MODLOG record your rule). The ini fallbacks stay either way.
- **Q25 (live-mode limits)**: with shared destruction on, the guest's car rams, turret or seat shots and charge-trigger
  weapons make no holes on either screen until M8 (today they break the guest's own copy). Accept that until M8, or keep
  parallel worlds (`destr_sync=0`) until then? **Default: accept** (shared holes).
- **Q26 (B's own Rhino/Thrust)**: with shared destruction on, B's own Rhino charge may stop at a wall for about RTT +
  0.1 s until A's hole arrives (A's view of B goes through). Accept that, or set `destr_ownpack=1` (B breaks walls with
  its own pack locally; those buildings are left out of the destruction check and may differ slightly between screens)?
  **Default: accept** (`destr_ownpack=0`); C1 step 7 shows how it feels.
- **Info, no answer needed**: district control on the guest can no longer change from ver7.0 (GF-EDF). Today the guest's
  own collapses can drop control, liberate or warp it (R14). The host's district changes reach B only at a re-join (or
  F3) until M9. The guest's resync key is **F3** (refused during the host's mission in 7.1; a re-join with holes from
  7.2).
