# RfgCoop: session, link and partner puppet (milestones M2 "Link", M4 "See each other", M5 "Join the host's world")

Code: `src/net.cpp` (ENet thread, the only file that includes `enet.h`), `src/session.cpp` (state machine, handshake,
keys, status, F7, the M5 join), `src/proto.h` (wire structs and the sim-thread API between the files), `src/puppet.cpp`
(the ghost generalised: a puppet is fed either by the local ghost ring or by network states), `src/world.cpp` (the world
blob capture and load, exported for the join), small helpers in `src/core.cpp` / `src/game.h` (`NowMs`, `ModDir`,
`TitleStatus`). Keys in `RfgCoop.ini`. The guest's rules and fences (M5) are `src/guest.cpp` (its own
notes). Build: GOG `rfg.exe` 32-bit, every address is an RVA (`rfg.asm` prints VA = RVA + 0x400000). M2 + M4 passed in
game (ver5); the M5 part (section "M5" below) has not run yet; "verified" means read in the disassembly or the PDB
or, for the calls marked "in game", used in game.

## What it does

- **F8** (game window focused; starting needs gameplay with a Player, both roles, M5) starts the session from `role`:
  the host listens on UDP `port`, the guest dials `connect` and redials every 3 s until it gets an answer. F8 again
  stops it in any game state (BYE when linked, REJECT to a guest in the host's handshake, then disconnect).
  `autoconnect=1` starts it on the first gameplay frame. No `role` = co-op off. `dev=1` is not needed.
- **Tutorial rule** (RESEARCH.md §8 Q12, below): the host's F8 / autoconnect starts hosting only in gameplay with a
  Player and a loaded save that has finished the Tutorial mission; a host load of a save without it ends the session.
  The guest's own save is not checked.
- Handshake (below): compatibility values, then CHALLENGE / AUTH / WELCOME (the guest echoes the host's nonce; with a
  password the guest proves it knows it first, the host second). On success both
  sides show "Co-op: <name> connected" and the window title reads `<original> [RfgCoop <tag>] host, partner <name>,
  RTT <n> ms`. A mismatch is refused with a reason on both sides; a wrong password shows "wrong password" on both.
  A password is empty on both sides (open session) or at least 10 characters.
- While linked, each side captures its own Player every gameplay frame (the ghost's `Capture`) and sends the newest
  capture as PSTATE 20 times a second. The receiver creates a **partner puppet** (the ghost's recipe and patch set,
  model = your own Player's `h_info`) at the partner's position when the first visible state arrives, and feeds every
  state through the ghost's ring, `Deliver` (add_state) and `Effects` (visibility, weapon, zoom, detonation).
- The partner puppet is hidden (standing still where it was last seen) while the partner sits in a vehicle, while the
  partner is loading, and while the partner's real position (or the received body position, or the two more than 10 m
  apart) is not in a zone streamed on your side. A green star on the minimap marks the partner's real position every
  gameplay frame (`partner_icon=1`): also while it drives, while it is far away and while no puppet exists.
- After any level load on your side the game destroys every player; the partner puppet is recreated in gameplay with
  the next visible state (the ghost's lifecycle), and LOADING / READY tell the other side.
- **F7** (gameplay) teleports you 2 m beside the partner's last reported position (its vehicle while it drives),
  holding you in the air until the terrain there streams in (at most 15 s, then back). A hold
  that has started finishes (or goes back) even if the link drops meanwhile. A partner position off the zone grid
  (|x| or |z| > 4600) is refused.
- Damage: the partner puppet takes no damage from humans and deals no damage to humans (the ghost's `human_damage`
  rules). Its bullets, explosions, remote charges and hammer still damage buildings, vehicles and props in **your**
  world (parallel worlds, as the ghost; RESEARCH.md §3.7: vehicle and building sinks come in M6/M8). That destruction is
  ordinary world state: the next autosave or manual save writes it into your own campaign save (the destroyed table is
  the first section of the blob, world_save.md F18). Play M4 sessions on a throwaway save if your campaign must not
  change.
- **F6** (the ghost) is refused while a session is linked; a ghost that exists when the link comes up is removed.

## Keys (`RfgCoop.ini`, section `[coop]`)

| Key | Default | Meaning |
|---|---|---|
| `role` | empty | `host` / `guest` / empty (no co-op) |
| `port` | 27777 | host: UDP listen port; guest: port when `connect` has none |
| `connect` | empty | guest: `ip:port` or `name:port` (resolved by ENet on the net thread) |
| `password` | empty | Same on both sides or empty on both (open session); a non-empty one needs at least 10 characters, else F8 refuses to start ("Co-op password too short: use at least 10 characters (or none for an open session)"). Read at every F8 start; never sent: the guest proves it knows it, then the host (below). A gate against strangers, not encryption. 5 wrong guest proofs from one address lock it out for 60 s (throttle below) |
| `name` | `tag` | shown in the partner's title / HUD (ASCII, 23 chars) |
| `autoconnect` | 0 | 1 = start on the first gameplay frame |
| `lag_ms`, `jitter_ms`, `loss_pct` | 0 | test shim on received packets (0-5000, 0-5000, 0-100) |
| `partner_icon` | 1 | minimap star at the partner's real position (0 = none) |

Hotkeys: F8 session on/off (a guest's F8 first opens the Q17 hammer / backpack picker: notes/picker_impl.md), F7
teleport to the partner, F6 ghost (dev=1, refused while linked). Edges on the async
high bit while `Os_app_active` (0x1118591) is set, read on the sim thread (the accepted dev-hotkey rule).

## Threads

- **Net thread** (`net.cpp`): created by every F8 / autoconnect start (`NetStart`), never in DllMain. It makes the
  password key (`DeriveKey`, PBKDF2, ~35 ms measured in game: too slow for the sim thread). The host's comes first,
  from this session's salt, and its copy of the password is wiped then, so the key exists before any event reaches the
  session. The guest's comes from the salt in each connection's first CHALLENGE, made before that CHALLENGE is queued
  (key, proofs below). It wipes the password and the key when it ends (`SecureZeroMemory`). Then it runs
  `enet_initialize` once per process (WSAStartup 1.1 + `timeBeginPeriod(1)`, never deinitialised), one
  `enet_host_create` (host 4 peer slots, guest 1; 3 channels) and an `enet_host_service(5 ms)` loop. It touches no game memory (Log only).
  `maximumPacketSize` 4096 and `maximumWaitingData` 256 KB (ENet's 32 MB defaults let any peer that connects, before
  HELLO or the password, force a 32 MB allocation from one fragment header; `enet_malloc` aborts on failure). M5: a
  packet received past the waiting-data cap is dropped unacked (peer.c:956, protocol.c `commandError`) and resent by the
  peer, which also throttles itself down; 256 KB stays above the sender's 64 KB reliable window, so a blob streams
  without that (it was 64 KB before M5). The host
  links one partner: a CONNECT event from another peer while linked gets `enet_peer_disconnect_now` (that guest sees a
  disconnect before the link and redials in 3 s). While unlinked, the host resets any slot that has sat in
  `ACKNOWLEDGING_CONNECT` for over 2 s: ENet gives a free slot to any bare CONNECT, even from a forged address
  (`enet_protocol_handle_connect`, protocol.c:310-341), and one that never acks VERIFY_CONNECT holds it 5-30 s and is
  then reset with no event, so with one slot a CONNECT every ~15 s kept the real guest out. A real guest acks within
  one RTT (a lost VERIFY_CONNECT is resent at ~0.5 s, then 1.5 s). Ceiling: a forged CONNECT flood faster than 4 per
  2 s still holds every slot; only a firewall stops that. The guest
  accepts a CONNECT only for the peer it dialled (ENet also answers incoming CONNECTs on the guest's port, and between
  dials the slot is free). Before each dial the guest resets its one slot if it is not free: with no peer of ours it can
  only hold a foreign half-open CONNECT (`enet_protocol_handle_connect` takes a free slot for a bare CONNECT; a sender
  that never acks keeps it 5-30 s with no event), which would make `enet_host_connect` return NULL. A NULL anyway is
  logged ("no free peer slot") and retried in 3 s, never a session end. One service pass handles at most 256 events,
  so a flooding peer cannot starve the lag-shim flush, the outbox or the F8 stop check.
  A kick (`NetKick`: REJECT, a ban, the handshake timeout) is `enet_peer_disconnect_later`, so a queued REJECT goes
  first; a peer that has not acked the DISCONNECT 2 s later (a client that never acks; a lost REJECT gets one resend
  first) is reset and the session gets DISCONNECTED, so it cannot hold the one partner slot for ENet's 15-30 s timeout.
  Stop: the outbox (BYE) is sent, `enet_peer_disconnect_later`, up to 1 s for the ack, `enet_host_destroy`, the thread
  ends; the next `NetStart` waits for it (3 s max).
- **Queues**: `g_out` (sim -> net: data, or a kick), `g_in` (net -> sim: CONNECTED / DISCONNECTED / DATA / FAILED), one
  SRWLOCK. A connection serial (+1 per accepted partner CONNECT, net thread) rides on every inbound item; `NetPoll`
  takes it from each CONNECTED the sim pops, and `NetSend` / `NetKick` stamp it on their items. The flush sends only
  items of the current connection: a peer's DISCONNECT and the next peer's CONNECT can land before the sim thread has
  answered the first one (one frame, or a load stall), and its REJECT / kick / CHALLENGE / WELCOME / PSTATE would
  otherwise go to the newcomer. Caps for a sim thread stalled for minutes (a minimised window): from 4096 queued items
  a new PSTATE evicts the oldest queued PSTATE (the newest states survive, and 20 Hz PSTATEs never crowd out the
  reliable LOADING / READY / BYE, which ENet has already acked and never resends); channel 0 stops at 8192 (a peer
  flooding it before HELLO);
  channel 2 (M5: blob chunks and LOADOUT, up to 4 KB each) stops at 1024 queued items (4 MB; a 2.5 MB blob is 656
  chunks; a dropped chunk breaks that blob's order and the guest asks again); connect / disconnect / failure events
  always go in. Channels 0 and 1 drop anything larger than PSTATE before it is queued; channel 2 takes up to
  `maximumPacketSize`.
- **Lag shim** (net thread, receive side): delay `lag_ms + rand(0..jitter_ms)`; a reliable packet's release time is at
  least that of every packet that arrived before it, on any channel, so channel 0 keeps its order and no PSTATE from
  before LOADING comes out after READY (it would re-seed the partner's ids from the old Player and replay one action);
  `loss_pct` drops channel-1 packets only. Jitter reorders PSTATEs; the session
  drops a PSTATE whose seq is not newer than the last one (as ENet's sequenced channel does on a real link), so it
  counts as lost. At most 4096 packets wait in the shim (a flood or a long `lag_ms`); past that they are dropped.
- **Sim thread**: `Net_Tick` (0x1D24D9 tick, every frame in every state, after `Puppet_Tick`) drains the inbox, runs the
  state machine, sends LOADING / READY and PSTATE, steps F7, updates the status. Every game call is here or in
  puppet.cpp's hooks.
- **Window thread** (`core.cpp WindowThread`): places the window once, then loops on an auto-reset event:
  `TitleStatus()` (any thread) stores the text under a lock and signals; only this thread calls `SetWindowTextW`.

## Protocol (`src/proto.h`, `kProtocol = 6`)

Protocol 4 = protocol 3 plus the M5 messages (section "M5" below); a protocol 3 build gets "RfgCoop protocol 4 on the
host, 3 on the guest". Protocol 5 (ver6.2) = protocol 4 with PSTATE's spare byte used as `pack`, the sender's worn
backpack (ghost_impl.md "Partner gear (ver6.2)"). The sizes do not change, and a protocol 4 build gets "RfgCoop protocol
5 on the host, 4 on the guest". Protocol 6 (ver6.3) = protocol 5 with 4 bytes appended to PSTATE (152 → 156): `bpAct`
and `bpFuel`, the activity of the sender's worn pack, plus a pad (ghost_impl.md "Backpack effects P1 (ver6.3)"). HELLO
did not change, so a protocol 5 build gets "RfgCoop protocol 6 on the host, 5 on the guest". Little-endian structs,
first byte = type. Channels: **0** reliable ordered (control), **1**
unreliable sequenced (PSTATE, ENet drops late ones), **2** reliable ordered (M5: world blob chunks and LOADOUT, each
packet at most 4096 bytes). Receive rules: exact size per
type, the right channel, the right phase and direction, else dropped and logged (first 5). Received text is cut at
its length and non-printable bytes become `?`. Nothing received is executed; handles, vehicle/turret ids and the
seat never come from the wire.

| Type | Id | Ch | Size | Direction / phase | Payload |
|---|---|---|---|---|---|
| HELLO | 1 | 0 | 72 | guest -> host, handshake | u16 protocol, u32 PE stamp, u32 SizeOfImage, u32 CRC32 of `xinput1_3.dll` / `winmm.dll` / `dinput8.dll` beside rfg.exe (0 = absent), i32 reserved (always 0), u32 data hash, u8 pw (1 = the guest has a password), u8 nonce[16] (Ng, fresh per connection), char name[24]. No password material |
| WELCOME | 2 | 0 | 57 | host -> guest, handshake | char name[24], u8 proof[32] = HMAC(key, "host\0" Ng Nh HELLO) (zero in an open session); only after a good AUTH (the guest refuses one before it sent AUTH, and one whose proof is wrong) |
| REJECT | 3 | 0 | 121 | host -> guest, handshake | char reason[120]; the host then kicks (`disconnect_later`, so the REJECT arrives). Also the host's stop (F8, the Tutorial rule) during the handshake |
| BYE | 4 | 0 | 121 | either, linked | char reason[120]: why it ended (F8, the Tutorial rule); the other side shows "<name> left: <reason>" instead of "disconnected". Also a guest refusing WELCOME's proof ("the host does not know the password"): the host has linked on its AUTH |
| LOADING | 5 | 0 | 1 | either, linked | your Player went NULL (level unload) |
| READY | 6 | 0 | 1 | either, linked | first gameplay frame (state 1) with a Player after that, and at link time if in gameplay |
| PEVENT | 7 | - | - | - | reserved, not sent in M4 (weapon and zoom ride in every PSTATE) |
| PSTATE | 8 | 1 | 156 | either, linked | see below |
| CHALLENGE | 9 | 0 | 33 | host -> guest, handshake, after HELLO, every session | u8 nonce[16] (Nh), u8 salt[16] (this host session's PBKDF2 salt). Nothing password-derived |
| AUTH | 10 | 0 | 49 | guest -> host, handshake, after CHALLENGE | u8 nonce[16] (Nh echoed: shows the host the sender receives its packets), u8 proof[32] = HMAC(key, "guest\0" Nh Ng HELLO) (zero in an open session) |

Handshake: the guest sends HELLO on CONNECT (a fresh Ng each connection). The host reads the protocol field first
(bytes 1-2): another protocol version is refused with "RfgCoop protocol x on the host, y on the guest" whatever the
size (a ver5 build, protocol 2, gets this from a protocol 3 host, and the other way round: HELLO's and REJECT's
layouts did not change). Then the exact size, then the comparison, first difference wins: rfg.exe, RfgCoop build (xinput1_3.dll), the
winmm.dll mod, the dinput8.dll mod, game data (`data\*.vpp_pc`: sum over files of
CRC32(UTF-16 name, CRC chained with the u64 size), order-free), a password on one side only ("a password is set on the
host only" / "guest only"). The reason is logged and shown on the host and sent in REJECT; the guest shows "refused by
the host: <reason>" and stops (no redial). Each side logs its own values once (first F8). `sw_api.dll` is not hashed
(the user's installs differ there on purpose). Then:
- **No password on either side** (open session): the host answers CHALLENGE {Nh, salt (unused)}, the guest AUTH {Nh
  echoed, zero proof}, the host checks the echo and sends WELCOME {name, zero proof}. No proofs, but the echo shows the host the guest
  receives its packets, so a forged source address never links (the host would else stream PSTATE to that address:
  threat model below). Start logs it and the host's HUD says "No password: anyone who can reach your port can join";
  the guest's says "(no password)". A bad step here reads "handshake failed" instead of "wrong password".
- **Password on both**: the guest proves first. The host answers CHALLENGE {Nh, this session's salt}: nothing
  password-derived. The guest's net thread makes the key from that salt before the session sees the CHALLENGE, and the
  guest sends AUTH {Nh echoed, guest proof}. The host checks the echo, then the proof (constant time). A wrong proof is
  a password guess: a strike (throttle below) and REJECT "wrong password", so both screens say it (host "Co-op: guest
  refused: wrong password", guest "Co-op: refused by the host: wrong password"; the guest stops, no redial). A malformed
  AUTH, an AUTH out of turn or with another nonce: REJECT "wrong password", not counted. Only after a good AUTH does
  the host send WELCOME {name, host proof} and link. The guest checks the host proof (constant time) before it links.
  A wrong one means a host that does not know the password and let any AUTH through (not a stock host): the guest sends
  BYE "the host does not know the password" (that host has linked, so it shows "<guest> left: the host does not know
  the password") and stops with "Co-op: the host does not know the password". A malformed or second CHALLENGE, or a
  WELCOME before our AUTH: the guest stops ("Co-op: wrong password").
No progress within 30 s of the connect or of the last handshake step (CHALLENGE sent / AUTH sent) (10 s plus the lag
shim's worst case both ways, 2 x (5000 + 5000) ms; every step waits on at most two shimmed trips): the side kicks the
peer (host listens again, guest redials).

### Password: key, proofs, throttle

- **Key** = PBKDF2-HMAC-SHA256(password as UTF-8, salt = 16 random bytes, 100000 rounds, 32 bytes),
  `BCryptDeriveKeyPBKDF2` (bcrypt.dll, linked with `-lbcrypt`; `DeriveKey` in session.cpp, run only by net.cpp's
  thread; `NetKey()` serves the key to the sim thread). The host makes a fresh salt at every F8 / autoconnect start,
  sends it in every CHALLENGE of that session and derives its key at start (`net: password key made in <n> ms`). The
  guest derives when a connection's first CHALLENGE arrives, from its salt, before the CHALLENGE is queued for the
  session (`net: password key made from the host's salt in <n> ms`; ~35 ms, so the net thread does it rather than
  stalling the sim thread; it only delays this connection's AUTH). The session reads the key only while it answers
  that CHALLENGE and checks the WELCOME after it; a stock host's next CHALLENGE needs the guest's next HELLO, which the
  session sends only once it is done with this connection, so the key never changes under it. One derive per
  connection: more CHALLENGEs cost no CPU (the session stops on the second). The password is read from the ini at each
  F8 start, refused when non-empty and shorter than 10 characters (UTF-16 units, so `é` counts once), converted to
  UTF-8 and handed to the net thread; the sim thread's copies are wiped right after `NetStart`. The host's net thread
  wipes its copy once its key is made; the guest's keeps it until the thread ends (a host restarted between two dials
  brings a new salt). The key is wiped when the net thread ends (F8 stop, a failure).
- **Nonces and salt**: 16 bytes each from `BCryptGenRandom(NULL, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG)`: Ng per
  connection (guest), Nh per HELLO (host), the salt per host start.
- **Proofs**: HMAC-SHA256(key, label with its NUL || first nonce || second nonce || the guest's HELLO as sent, 72
  bytes). Guest (AUTH): "guest" Nh Ng; host (WELCOME): "host" Ng Nh. The guest proves first and the host answers only
  a guest whose proof checked out, so a peer that has not proved receives nothing password-derived. The labels and
  nonce order differ, so neither proof can be reflected as the other; the HELLO bytes bind the compatibility values
  and names the host checked; both nonces are fresh, so a recorded exchange proves nothing on a later connection.
- **Throttle** (host): per remote IPv4 address (from the CONNECT text), wrong guest proofs are counted: an AUTH that
  echoes this connection's Nh but carries a wrong proof, i.e. an online password guess. The 5th refuses that address
  for 60 s: the 5th REJECT reads "wrong password (5 tries: wait 60 s)"; each CONNECT in that minute gets REJECT "too
  many wrong passwords: try again in N s" and a kick (no HELLO is read, no strike; cut off after 2 s if it does not ack:
  one reliable 121-byte packet, at most one resend), so a stock guest stops instead of redialling every 3 s and both
  screens say why; the log says "refused for 60 s"; afterwards it gets 5 more. A WELCOME clears the address. Only echoed AUTHs count because ENet does not check
  that a source address is real: a CONNECT that declares outgoing peer id 0xFFF skips the session-id check, and the
  ACK that completes it can be sent blind, so an off-path sender can complete a connection from another address (about
  4 packets) and send HELLO / AUTH / DISCONNECT from it. Strikes for anything it can fake (a malformed AUTH, one out of
  turn or with another nonce, the handshake timeout, leaving after CHALLENGE) would let it ban a known guest for 60 s
  over and over, or fill the table with forged addresses and push its own ban out; those are refused or dropped, not
  counted. They test no password either: CHALLENGE holds nothing password-derived, so a wrong AUTH is the only test.
  A stock guest with a wrong password earns one strike per try (it proves first): 5 mistyped tries in a row lock its
  address out for 60 s. Compatibility REJECTs do not count. The table holds 16 addresses; the one with the oldest failure makes room (ceiling: more than 16 real
  attacking addresses can push a ban out early; a firewall rule is the upgrade).

### Threat model

- **What the password protects**: who can link. Without it nobody completes the handshake, and the password itself
  never crosses the network (only HMACs bound to fresh nonces), so replaying a recorded handshake does not let anyone
  in; but anyone who can watch one handshake (same LAN / Wi-Fi, ISP, router, VPN exit) gets the guest's AUTH proof
  (and, if it linked, WELCOME's) with its salt and nonces, and can guess the password offline at PBKDF2 speed. The salt
  is fresh for each host start, so that is one target per recorded session, not one precomputed list for every host. A
  failed handshake's AUTH tests only the password that guest typed (often a near miss), not the host's. Tailscale /
  ZeroTier / Radmin encrypt the link, which closes this.
- **Guessing against a host** (protected since protocol 3): the guest proves first and the host proves only to a
  guest that did, so whoever reaches the host's port gets nothing password-derived (CHALLENGE is Nh and the salt) and
  can only guess online: one guess per HELLO / CHALLENGE / AUTH round, 5 per address per 60 s (throttle above). The
  salt is random per host start, so no list computed before a session fits it and no two sessions share a key.
  (Protocol 2 sent the host's proof in CHALLENGE with an app-wide fixed salt: one free HELLO gave an offline target,
  and one precomputed list served against every host.)
- **What a malicious host can still do to a guest that dials it** (an address typed wrong in `connect`, a name that
  resolves elsewhere, a machine on the path answering in the host's name): it receives the guest's AUTH proof and can
  guess the password offline at PBKDF2 speed (100000 rounds per guess). It picks the salt, so it can reuse one salt
  and one precomputed list against every guest that dials it; the nonces only stop replays, not that. It also sees
  the guest's HELLO (build CRCs, name). On its own it cannot link as the real host (it cannot make WELCOME's proof;
  the guest tells it why in BYE and stops). If it relays to the real host while the guest waits, the proofs pass
  through unchanged (they bind the nonces and HELLO, not an address) and the guest links through it (see 'Not
  encrypted' below). A passive watcher gets the same offline test for the sessions it records, but cannot
  choose the salt. The 10-character minimum keeps a random password out of reach (10 random lowercase
  letters are about 2^47 guesses, each a 100000-round PBKDF2); a word or a known phrase is not. Use a random
  password and dial addresses you trust (a Tailscale / ZeroTier / Radmin IP).
- **No PAKE**: a PAKE (SPAKE2, OPAQUE) would remove both offline targets (a malicious host, and anyone watching a handshake), but needs elliptic-curve group code
  that BCrypt does not provide as a PAKE (a new dependency or hand-rolled crypto) and a longer protocol. That is out of
  proportion here: two friends, the guest dials an address it chose, and the traffic after the handshake is not
  encrypted or authenticated anyway (next bullet), so an attacker in a position to impersonate the host already wins
  more than the password. The password is a gate against strangers who reach the host's port; guest-first closes that.
- **Not encrypted, not authenticated after the handshake**: game traffic (PSTATE, LOADING / READY, BYE, from M5 the
  world blob) is plain UDP. Anyone on the path can read it, and an on-path relay (a machine the guest dials by mistake
  that forwards to the real host, or a router between the two) can let the proofs pass through and then read or change
  everything that follows. Use a private network (Tailscale / ZeroTier / Radmin) between two PCs.
- **The host is trusted by the guest**: from M5 the guest parses the host's world blob (and later its NPC, vehicle and
  mission messages), large untrusted input into game code. Join only hosts you trust; the password proves the host
  knows it, not that the host is harmless.
- **The open session** (no password on either side) lets anyone who can reach the port link; the host's HUD says so.
  Linking still needs the AUTH that echoes Nh, so the partner's address is real.
- **Forged source addresses**: ENet does not verify them (throttle above). An off-path sender can complete a
  connection from another address and hold the host's one partner slot for the handshake timeout (30 s per connect,
  60 s if it also sends a blind HELLO), as a CONNECT flood can. It cannot link in any session (it never sees
  CHALLENGE's Nh, which AUTH must echo) and cannot spend another address's strikes. Without the echo an open-session
  host linked to a forged address would send it PSTATE (20 Hz, ~160 bytes) plus reliable resends for 15-60 s per
  blind link, a reflection flood at a third party; with it, that address only gets the few backed-off CHALLENGE /
  VERIFY_CONNECT resends. A forged address cannot earn the real guest's address a strike either (only an AUTH that
  echoes Nh counts), and a blind sender cannot make one. Hosting on Tailscale / ZeroTier / Radmin, or behind a firewall allowlist, prevents forged
  sources.
- Before the handshake: the size caps, queue caps and half-open slot reaping in "Threads" above; the compatibility
  REJECT (sent to anyone who sends a HELLO) shows the host's build CRCs, as before.

Timeouts: `enet_peer_timeout(peer, 32, 15000, 60000)` after the connect (a dial keeps ENet's defaults, so a host that
is not up yet is retried). The net thread is independent of the sim thread, so pauses, loads and minimised windows do
not stall acks; LOADING / READY are explicit.

**PSTATE** (156 bytes, naturally aligned, no padding): u8 type, u8 flags (bit 0 = on foot: the puppet shows), u16
seq, u32 time (the sender's `Clock_current_time` at capture), f32 where[3] (the sender's real position: its vehicle
while driving), then the `multi_player_state` fields pos[3] (+0x00), vel[3] (+0x0C), orient[9] (+0x24), cam[9]
(+0x4C, the aim), forward, sideways (+0x78/+0x7C), hp fraction (+0x80), u32 ladder handle (+0x84), i32 ragdoll
(+0x88), i32 knockdown ms (+0x8C), u16 ladderState (+0xA2), u8 det (detonation counters: low nibble remote charges,
high nibble in-air), u8 pack (protocol 5: the worn backpack `[[Player+0x1170]+0x270]+0x50` + 1, 0 = none; the
receiver keeps 0..10, anything else = none), u32 bits (+0xA4, layout in ghost_impl.md "The state"), u32 item (the
`inv_item_info` handle `+0x04` of the equipped item, ~0 = none), then (protocol 6) u8 bpAct (the worn pack's activity:
bits 0-3 an activation counter, bit 4 active `+0x254`, bit 5 Thrust up `+0x2AC`, bit 6 Thrust landed `+0x2AD`, bit 7
frozen: a still re-send, see Sender), u8
bpFuel (`+0x25C` × 255), u16 pad (0).

The activation counter steps when the pack's `activated_time` end tick (`+0x268`) changes to a value other than -1 on
the same pack object, so it also catches a Shockwave one-shot that is inactive again by the capture; another pack
object re-baselines without counting. Like the detonation counters, the receiver turns it into an edge and never acts
on the first state of a link or after a partner READY; level and fuel are re-sent at 20 Hz, so a lost packet loses no
activation unless 16 happen between two received states. A step that arrives after 5+ states lost in a row (by `seq`,
which is otherwise only for the loss statistics) is treated as stale, a resync, since it may be from anywhere in the
gap (ver6.4). The receiver uses only bits 0-7 and maps the fuel byte to
0..1; `pad` is ignored. What the puppet does with it: ghost_impl.md "Backpack effects P1 (ver6.3)".

Sender (`Puppet_Out`, called at most every 50 ms, or at once when the newest capture stepped the pack counter:
`Puppet_Step`, ver6.4, so a low down-Thrust that starts and ends within 50 ms still goes out with level 1; the next
PSTATE, unless frozen, repeats that level, so losing the step's state loses no Thrust): the newest capture since the last
PSTATE. While you are paused
(`HostPaused`: Game_paused_count minus our own token > 0, or a top state that hides or pauses gameplay; the Esc
menu, state 15, keeps Capture running, ver6.3 test; on the host also its join token, which holds its sim while the guest
lands), or with no capture for 150 ms (a hitch): the last one as a still state (no velocity, stick, sprint, detonation; trigger released), re-stamped
with the current Clock, so the partner sees you stand rather than extrapolate. Such a state also sets bpAct bit 7
(frozen): its pack level is stale, so the partner ends your pack until you capture again, then resyncs it
(otherwise your guest-side Esc or map would keep a Tremor quaking or a Heal healing on the host); a Jetpack still takes
the fuel, which a paused tank does not change. In a vehicle every capture is a still
state with flag 0 and `where` = the vehicle; it carries the current activation counter (not 0 after a link or load
while seated), so getting out is no edge. Detonation bits 11/13 are one-frame edges that 20 Hz sampling would lose,
so every edge also bumps a 4-bit counter that every PSTATE carries.

Receiver (`Puppet_In`), in this order:
1. Dropped while the partner is LOADING (until READY), or if any float is NaN/inf or out of range (where/pos 1e5,
   vel 1e3, matrices 1.5, stick/hp 10), `orient` or `cam` is not a rotation (rows of unit length and pairwise
   orthogonal within 0.1: `orient` reaches `obj_human_props` and `update_pos_and_orient` at creation; process_state
   0x5E1D06 runs `matrix::orthogonalize` 0x1A2670 on `cam`, which divides by the f row's length with no zero check,
   0x1A26C1, so a zero f would make the aim NaN) or ragdoll > 6
   (`NUM_HUMAN_RAGDOLL_STATES` = 7, PDB). The session already dropped a PSTATE that is not newer than the last one.
2. `network_time` = sender time + offset + 100 ms. Offset = arrival - sender time; its target is the smallest
   offset seen in the last 2 s window (the packet with the least queueing), and the offset used slews toward it by at
   most 1 ms per state, so jitter neither compresses nor reorders states. An offset more than 2 s **lower** re-syncs at
   once (logged); a higher one is queueing (your sim thread stalled and drains its inbox, a delay spike) and is not
   followed: within a link the sender's Clock never steps back, and a partner restart is a new link (`g_map` cleared).
   Those stale states land in the past and `Deliver` clamps them monotonic. The partner's `where` is stored for F7 and
   the minimap star.
3. Only in gameplay (`gameseq_get_state` == 1, Player, character streamed): world reads below are unsafe during loads.
4. Built into a `multi_player_state`: stick clamped to ±1.5, hp to 0-1.5, knockdown to 0-10 s; turret / vehicle net
   ids 0xFFFF and cache dwords -1, seat 0 (always: the SP divide-by-zero trap, ghost_impl.md); bits masked to what the
   capture writes (0x7FFF3FFF); the ladder handle kept only if it resolves locally to an object of type 0x11 (what
   `apply_movement_state` 0x5CF474-0x5CF497 mounts), else -1 with bit 6 cleared; detonation bits set from counter
   changes (never on the first state); the pack activation edge likewise from the bpAct counter, with its level,
   Thrust bits, frozen bit and fuel as sent (protocol 6); the weapon resolved from its handle in `Inv_item_info` (unknown =
   unarmed, logged).
5. Visible = flag bit 0, `world::is_zone_streamed(&where)`, `is_zone_streamed(&pos)` and `pos` within 10 m of `where`
   (an honest `pos` is `where` or the ragdoll body; in a vehicle they differ, but the flag is 0 then and the received
   `pos` is never used). A visible state is remembered as the last seen one; a
   hidden one is replaced by that last seen state made still (the puppet stands hidden where you last saw it, it is
   never moved into unstreamed ground). A puppet kept across a relink (the link dropped and came back while this side
   was out of gameplay, so `Puppet_Tick` did not destroy it) has no last seen state from the new link: its hidden
   states (and LOADING's) reuse the old link's last seen state with the new link's bits and weapon, so it is hidden
   too. Pushed into the ring with `due` = now (delivered on the next gameplay frame).
6. `Deliver`: the first delivered state, and every visible state that follows a hidden one, resets the sim buffer
   (`multi_sim_state::reset` 0x568B20 zeroes the count at +0) and re-seeds Player+0xC4 (melee, jump, trigger, throw
   ids) from that state. A hidden state carries the last visible one's ids, and a partner load means a new Player with
   new ids, so without the re-seed the first visible state after it would replay one jump, swing, shot or throw.

## Puppet changes (`src/puppet.cpp`)

- One puppet at a time, of the kind `g_net` says (`g_isPartner` records the kind of the one that exists). A puppet of
  the wrong kind (the ghost when a link comes up, the partner after it ends) is destroyed by `Puppet_Tick` in gameplay
  (state 1), or only forgotten when the game freed it.
- Ring entries carry `due` (Clock for `add_state`) and `st.time` (network_time, also when `Effects` runs). Ghost:
  due = capture + 300, time = capture + 400 (unchanged behaviour). Partner: due = arrival, time = mapped (above).
- `Create(me, pos, orient)`: ghost at your position + `ghost_offset` on X; partner at its last visible state. Partner
  creation needs a visible state younger than 1 s and stops after 3 failures until the next link.
- L5 (operands 0x6B19C8 / 0x6CC4A3 -> `g_coopOne`) is now `L5(bool)`, open while a puppet exists **or a session is
  linked**: the sender's own Player must advance its melee and SINGLE trigger ids even when no partner puppet exists
  on its side. Same bytes and checks as before.
- `Capture` resets its baselines when the Player changes (a new level), never adds `ghost_offset` for the network, and
  in network mode writes the PSTATE source instead of the ghost ring.
- Shared log lines now start with `puppet:` (hooks, PP-STATS, listener store, melee turn, L5, hash tables);
  ghost-only lines `ghost:`, partner lines `partner:`.
- The Clock (`Clock_current_time` 0x02BEA538) is written every tick once the hooks are installed (first F6 or first
  link), with or without dev=1.
- Minimap: no icon on the puppet object any more. While linked, `OnSimFrame` (after the simulation, gameplay frames
  only) adds a temporary green star at the partner's last `where` (`Puppet_PartnerPos`: not while the partner loads,
  not when its last PSTATE is older than 10 s), as the activities draw theirs every frame.

## New game calls and data (all on the sim thread)

| What | RVA | ABI used | Evidence |
|---|---|---|---|
| `world::is_zone_streamed(vector*)` | 0x52E020 | thiscall (ecx = &World), 1 stack arg, `ret 4`, bool in al | body: zone grid `World+0x2724`, `[zone+0x58] == 2`; call site 0x6E618D-0x6E6196 in `npc_process_all` (`lea eax,[esi+4]; push eax; mov ecx,World; call`) |
| `cartographer::add_temporary_icon(vector* pos, minimap_elements bitmap, bool stick, unsigned handle, color*, bool blink, unsigned activity_handle)` | 0x473FF0 | thiscall (ecx = &Cartographer 0x27B61F8), 7 stack args, `ret 0x1C`, void | PDB sig (the other overload is 0x4741D0); argument names from `map_icon_temp` (PDB: pos, bmin, bmax, bitmap_color, bitmap, stick, blink, handle, render_pos, activity_handle) and its ctor 0x450C00; call sites 0x369F37, 0x3820AF, 0x39AEEF push `activity handle or -1, 0, &Default_map_item_color, -1, stick, element, &pos; mov ecx,Cartographer`. The body copies x/z into the per-frame list `+0x11974` (capacity check `+0x11980 < +0x1197C`); gates Show_minimap, Usability_hud_enable, Hide_hud_ref_count, Debug_show_hud, `Add_temp_icons` 0x279CD4A. `cartographer::render` empties the list after drawing and re-arms the flag (0x483098-0x4830A5); `cartographer::process` (inside `gamestate_gp_simulate_frame`, 0x3EDE25) clears the flag while `game_is_paused` (0x47CF0B-0x47CF14), so nothing piles up in a pause. Element 2 = `MAP_ELEMENT_STAR_GREEN_ICON` (PDB). Our call was checked in clang's `-S` output: the same 7 pushes, ecx = base + 0x27B61F8, no stack fix-up after |
| `Default_map_item_color` | 0x12611A0 | data, `color*` arg | `FF FF FF FF` in the exe (.data), no writer in the disassembly |
| `Inv_item_info[64]`, `Num_inv_item_info` | 0x2C6D550, 0x2C2E0E4 | data: stride 0x28, `+0x04` handle | `inv_item_info_from_handle` 0x612370 (loop over Num, compare `[Inv_item_info+4+i*0x28]`), `inv_item_info_get_handle` 0x612360 (`[info+4]`); 2560 bytes in the symbol table = 64 entries |
| ladder object type | `+0x7E == 0x11` | data | `apply_movement_state` 0x5CF48D |
| `human_teleport_unsafe(human*, vector, matrix)` | 0x67C290 | cdecl, by value | in game |
| `human_teleport(human*, vector*, matrix*, float, bool)` | 0x69A3C0 | cdecl | in game |
| `havok_terrain_height(float*, float, float, bool, vector*)` | 0x43B440 | cdecl, al | in game |
| `havok_body_set_linear_velocity` / `_angular_velocity(unsigned, vector*)` | 0x423870 / 0x4239A0 | cdecl | in game (vehicle body handle `+0x4C`) |
| `human_is_dead(human*)` | 0x1DC0F0 | cdecl, al | in game |
| `human_ragdoll_stop(human*, bool)` | 0x6C1460 | cdecl | in game |
| `Frametime_uncompressed` | 0x1118568 | float, read | in game |
| `hud_message_add`, `world::object_from_handle`, `gameseq_get_state` | 0x455D30, 0x53BEE0, 0x3BFC70 | as before | ghost_impl.md |
| `All_mission_infos` (`farray<mission_info,26>`) | 0x1E45C18 | data: `+0x4` data pointer, `+0xC` count, stride 0x60; `mission_info` `+0x00` char* name, `+0x14` `mission_mode` mis_mode, `+0x58` mission_flags | `mission_is_complete` 0x374B20 and `mission_get_mode` 0x39DA20 / `mission_get_name` 0x39D9C0 bound the index with `[0x2245C24]` (= +0xC) and index `[0x2245C1C]` (= +0x4) with `lea eax,[eax+2*eax]; shl eax,5` (0x60); `mission_get_num_completed_missions` 0x374C80 walks +0x58 bit 0 in 0x60 steps; field offsets and `MISSION_MODE_TUTORIAL = 4` from the PDB (`mission_info` size 0x60, `mission_mode`); 2512 bytes in the symbol table = 0x10 + 26 x 0x60. Flags come from the save: `mission_system_load` 0x39D690 from `world::init_world_zones` 0x53A993 (territory load) |
| `mission_is_complete(int index)` | 0x374B20 | cdecl, 1 stack arg, plain `ret`, bool in al | body: -1 / negative / >= count give 0, else `al = [data + i*0x60 + 0x58] & 1` (the other overload 0x38EB00 takes a handle) |

No new code patch. The ghost's patch set (ghost_impl.md) is installed by the first F6 **or the first link**, with the
same expected-byte checks; if any fails, the session still links (M2) but nothing is captured and no puppet is made
("partner: game code differs" in the log, "(no puppet: RfgCoop.log)" on the HUD).

## Tutorial rule (RESEARCH.md §8 Q12)

`TutorialDone()` (session.cpp, sim thread, gameplay only): the first `All_mission_infos` entry (count capped at 26)
whose `mis_mode` is 4 (MISSION_MODE_TUTORIAL); allowed = `mission_is_complete(index)`. It logs the index, the name
(+0x00) and the flags (+0x58). The flags are cleared at every level init (`mission_info_level_init_pre` 0x3B0D20 sets
+0x10 = -1 and +0x58 = 0 for every entry, from `gameplay_level_init_pre` 0x3B46C0, before `mission_system_load`), so a
New Game or a save without the Tutorial reads 0, never the previous save's flags. A DLC level keeps the base list:
`dlc_missions_init` 0x3CB4F0 stores the count in `All_mission_infos_mark` and appends the DLC missions behind it
(`mission_info_load_table` 0x3B4FB0 resets the count only for -1, the base load), and `dlc_missions_shutdown` trims
back to the mark. So DotB finds the base Tutorial (index below 26) with complete 0 and is refused. No mode-4 entry =
not allowed (defensive only, not expected in the shipped game).
- **Start** (F8 or autoconnect, both roles): outside gameplay or without a Player nothing starts and the log says
  `session: F8 outside gameplay: the host needs a loaded save` (guest: `the guest needs a loaded save, in free roam (the
  host's world replaces it)`); the window title already reads "co-op off (F8 in gameplay, on your save)" there (a HUD line
  could only show after the load, when it no longer applies). The guest needs it because it arms its fences at the
  link for good and an armed guest can load nothing of its own (M5), so a link from the main menu could never join.
  Host only: Tutorial not done: "Co-op needs a save that has finished the Tutorial"; nothing starts (autoconnect does
  not retry; F8 later does). Guest only: a mission or a modal activity other than a convoy running (`GuestBusy`, M5
  "Guest link"): "Co-op: finish or cancel your mission before joining" / "... your activity ..."; nothing starts (a
  fresh save still in the Tutorial mission cannot be a guest). A host may start in a mission (its join waits:
  JOINWAIT). A HUD line shown at once clears an older deferred one, so the newest line wins.
- **Every load** while hosting or linked (any phase but off): the first gameplay frame with a Player other than the
  one last checked (Player goes NULL on every load) checks again, after the frame's inbox (so a guest that connected
  during the load is already in the handshake). Not done: `Stop("session stopped: the host's save has not finished
  the Tutorial")`; linked, that text goes in BYE, so the guest shows "<host> left: session stopped: the host's save
  has not finished the Tutorial" (and redials the stopped host every 3 s until F8); a guest in the handshake gets it as
  REJECT ("refused by the host: session stopped: ...") and stops.

## M5: join the host's world (session side; RESEARCH.md §4.7, §4.8, rules §8 Q12-Q15)

Not run in game yet. The guest's rules and fences (load fence at 0x51C320, mission / script / activity fences, no in-RAM
saves, UI blocks, applying the loadout, catching salvage) are `src/guest.cpp`'s; this is the session side. Interface:
`src/features.h` (Guest_* / Rules_* called here, Session_RejoinRequest / Session_SendSalvage / World_ArmSaveGuard
implemented here and in world.cpp); world blob pieces: `src/proto.h` "world.cpp".

### What happens

- **Guest link** (§4.8 step 1): first `GuestBusy()`: a mission (`World_InMission`) or a modal activity other than a
  convoy (`Current_activity_mode` 0x1E28590 = the running modal activity's type, set at 0x3ACA73, cleared at 0x3698CE)
  ends the session at the link, BYE "finish or cancel your mission (activity) before joining", nothing armed. Start
  already refuses it; the link checks again because a guest dials for minutes and may start one meanwhile. Why: under
  the fences a death in its own mission skips the death screen (`gamestate_gp_simulate_frame` 0x3EDC01 `mission_is_active`
  -> 0x3EDC0C `mission_terminate(0)`; `mission_terminate_internal` -> `hud_ui_mission_complete_start` 0x3B4BCF -> the fail
  screen GS 0x28, pushed at 0x44AE40). Its Continue / Abandon (`humc_request_exit`) sets `Mc_info = 1` (input and render
  off) and then loads at 0x44A8D8, which GF-LOAD refuses; GS 0x28 never closes, Esc -> Cancel mission is fenced, and the
  host would sit paused 120 s on the blob that refusal asked for. The modal activities raid, house arrest, riding
  shotgun, guerrilla express, demolitions master and area defense skip the death screen the same way (0x3EDC19-0x3EDD2A,
  `activity_*_process_player_death`); convoys (13, Q14) do not. Then `World_ArmSaveGuard("co-op guest")` (the one
  0x811278 guard in world.cpp; if it cannot
  arm, the session ends: "the save guard could not be armed") and `Guest_Arm()`, before any blob, for good: never
  disarmed, also not after F8, the host leaving or a disconnect. `Guest_Armed()` false right after `Guest_Arm()` ends
  the session at the link ("the guest fences could not be armed"), before the puppets, the "connected" line and READY,
  so the host never captures, pauses or sends for a join that cannot happen (`Guest_Arm` never retries: every F8 relink
  ends the same way). Each blob load still checks `Guest_Armed()` as a backstop ("the guest fences are not on").
- **Alt-tab** (§4.7): from Start to Stop, 0x4DDB5F `75 16` -> `EB 16` (core.cpp `QuickPauseOff`), so an unfocused game
  never pushes GS_QUICK_PAUSE (it would pause the guest under a pending blob, which loads only in gameplay or on the
  death screen, or make the host send PAUSED and skip its capture). Back to `75` at Stop unless dev=1, which keeps it
  off for the whole run; already in place is a no-op, so the two never clash.
- **Join, host** (§4.8 step 2): from the guest's first READY after the link (no world sent yet), not from the link
  itself: the guest sends READY on channel 0 only once it took WELCOME, while BEGIN goes on channel 2, and ENet orders
  per channel only (`enet_peer_dispatch_incoming_reliable_commands`, peer.c), so a BEGIN sent at the link could beat a
  lost or late WELCOME and be dropped by a guest still in the handshake (the host then sat paused for 120 s). A guest
  that links during a load sends LOADING, then READY. Then, in gameplay with a Player, once a second: `World_CaptureStart()` (the E3
  capture; free roam = `Mission_active_index == -1`, `Stream_pause` clear, `0x265D` bit 4 clear, `0x265C & 0x24`
  clear, checked at the request and again in the gameplay handshake right before `process_save_load`, world_impl.md;
  `World+0x2578` left at 1 after a checkpoint mission is no refusal: the silent save resets it to 0). Refused in a
  mission: JOINWAIT with the reason (once per
  reason), host HUD "Co-op: <guest> joins when you are in free roam (<reason>)", guest HUD "Co-op: you join after the
  host's mission (<reason>)". Other refusals (saving, streaming paused, a dev capture running) are logged once and
  retried. A finished capture (`World_CapturePoll`: synchronous `save_state_data` in the gameplay handshake, stale check
  by size + FNV) is sent; a failed or stale one is retried in 3 s.
- **Send**: BLOB_BEGIN {id, kind, size, WorldMeta, host position, host clock}, then the chunks (4000 bytes each, the last
  the rest) on channel 2, all queued at once (ENet's reliable window paces them). Then the host takes its co-op pause
  token (`game_pause(1)`), HUD "Partner joining..." ("Partner re-joining..." for a re-join), and waits for JOINED with that
  id. 120 s without: it unpauses, HUD "Co-op: <guest> did not finish joining".
- **Receive, guest**: BEGIN is checked (size 8..2.5 MB, kind, a finite position within 1e5, dlc -1..16) and opens a
  receive buffer. Each chunk must carry that id and the next index with exactly the size its index implies. A newer BEGIN
  abandons a partial blob; chunks of an abandoned blob are ignored quietly; a wrong index or size drops the transfer and
  sends REJOIN_REQ. Complete: the blob waits to be loaded (a newer complete one replaces it).
- **Load, guest** (§4.8 step 3), in gameplay, on the death screen (GS 16) or on the mission-fail screen (GS 0x28; a
  backstop, `GuestBusy` keeps missions out; vanilla `humc_request_exit` loads from it too, 0x44A8D8) with a Player. The
  blob is the one waiting at the moment `World_Load` accepts it: a newer complete blob can replace it while the load is
  "not now" or a menu is open, so the header (id for JOINED, kind, BEGIN position, transfer time) is taken again then,
  not only when the step began (else JOINED named the older id, which the host ignores: paused 120 s). `World_Load(blob, meta,
  session=true)`: the world-busy refusals of `set_pending_territory_load` first (else "not now", retried each frame),
  "RFGS" v52 and buffer 0's max_size (else refused: REJOIN_REQ), `World+0x2578 = 0`, memcpy into `get_load_buf`,
  `set_cur_size`, `difficulty_set_level_reload_overwrite(-1)`, `platform_load(info, 1)` with the **host's** WorldMeta
  (dlc, hammers, golden hammer, backpacks, jetpack: process globals platform_load sets; rule Q13), `Guest_AllowLoad()`,
  `set_pending_territory_load("", 0, 0, 0, dlc, 0)`. 15 s of "not now" in a loadable state ends the session ("join
  failed: the world load did not start in 15 s").
- **Arrive, guest** (§4.8 step 4): Player going NULL proves the load started (15 s without: REJOIN_REQ). On the first
  gameplay frame after it the log has the load time, position, `Mission_active_index` and the clock against the host's.
  1 s later (the load's own nearest-safehouse spawn settles) it teleports beside the host (`Puppet_PartnerPos`, else
  BEGIN's position) with F7's streaming hold (`TeleportTo`, at most 15 s, then back). When the hold ends: JOINED {id,
  transfer ms, load ms, distance to the host}, HUD "Co-op: joined <host>" / "Co-op: re-joined <host>". A newer blob or a
  host LOADING in between skips the teleport and JOINED (that next blob is loaded instead). A death during arrive or the
  hold (both run only in gameplay) goes back to idle on the death screen (`session: died while landing`): Continue's
  REJOIN_REQ, which the host takes while waiting for JOINED, or a blob already pending takes over.
- **Host on JOINED** (current id only): releases its pause token, logs the guest's numbers, forces a LOADOUT, HUD "Co-op:
  <guest> joined". Puppets come back by themselves on both sides (puppet.cpp, from PSTATE after READY).
- **Re-join after a host load** (§4.8, one rule): the host's LOADING (its Player NULL) drops any blob step in progress
  (its pause token went with the level). On its first gameplay frame after the load, if this guest already had a world
  from us, it sends `World_CurrentState()` = `world::get_state_data()` = `stored_world_state[World+0x2578]`, the buffer
  that load used, **as is**: death-continue, mission fail / continue, checkpoint, fast travel, warping video, activity
  replay, Esc -> Load (a disk load copies the save into that buffer first, `verify_gamesave_process` 0x3F0280). Never a
  fresh silent blob mid-mission. No valid buffer (a new game; Q12 ends the session there anyway): a fresh silent blob once
  in free roam. Before the guest's first world, a host load keeps the join waiting for free roam.
- **Guest on the host's LOADING**: drops any blob arriving or waiting, clears "host paused", HUD "Host loading...", hides
  the puppet (`Puppet_PartnerLoading`). The host's READY ends it.
- **REJOIN_REQ** (guest -> host; `Session_RejoinRequest` from guest.cpp on its death or a refused load, and from the
  session on a broken transfer, a refused blob or a load that never started): one per 5 s from the guest, never while
  a world is on its way to it. That 5 s window in `Session_RejoinRequest` is the guest's only debounce (guest.cpp
  logs `guest: re-join wanted: <why>` and leaves it to the session): a request inside it is held (`re-join wanted
  (why): within 5 s of the last request, held until then`) and sent when the window opens, unless a BEGIN arrived
  meanwhile; a death's Continue in that window is therefore not lost. The host takes it while idle or waiting for
  JOINED, with a 2.5 s flood bound only: each side measures from its own moment (the guest at send, the host at
  Drain), so a held request sent 5.0 s after the last could reach the host a few ms (or, with the dev `jitter_ms`, up
  to seconds) under 5 s, and a refusal sends nothing back. Before the guest's first world from us (it linked outside
  gameplay, so no READY yet: e.g. on its death screen) the request is its **join**: kind join, a fresh silent blob in
  free roam, JOINWAIT in a mission, never the loaded buffer (§4.8 step 2). After that: in a mission
  (`World_InMission`, `Mission_active_index != -1`) the loaded buffer as is, in free roam a fresh silent blob. A READY
  arriving later finds the host busy and starts nothing. Without a link the guest shows "Co-op: no host. Restart the
  game to continue your own campaign".
- **Pause** (§4.7): the host sends PAUSED 1 while it has a Player and (`Game_paused_count` minus our own token > 0, or its
  top game state is not gameplay and hides or pauses it: `State_info` +0x18 transparent 0 or +0x19 pause_beneath 1): Esc
  (`ingame_options_enter` pauses), map (37, not transparent), videos (49), the death screen (pauses), other menus.
  PAUSED 0 when that ends or the Player goes. The guest holds its own token (`game_pause(1)`) while PAUSED 1, in
  gameplay, with no join step and no teleport hold running; HUD "Host paused". The guest's own Esc pauses only the guest
  (vanilla, nothing sent). The host's join token is left out of PAUSED, so a join never pauses the guest.
- **Pause tokens** (both roles, `PauseToken`): one bool, taken only in gameplay, released with `game_unpause()` only
  while the count is above 0. Every level exit and level init unpause `Game_paused_count` to 0 (`gamestate_gp_exit` loop
  0x3F0F71, `gamestate_gp_level_init_post` loop 0x3EFE80), so a token held when the Player goes is dropped, never
  released again (an extra `game_unpause` would leave -1 and swallow the game's next pause).
- **Loadout** (rule Q13), host, once the guest has a world from us and no blob step is running, about once a second:
  `player_meta_data` (Player+0x2978) salvage +0x4 and `upgrade_item[128]` +0x18 (0x400 bytes), `Weapon_list` (0x1260ED0,
  count ≤ 64, 8 bytes per item) -> LOADOUT on channel 2 when any byte changed, and once after every JOINED. Guest: sizes
  checked (arsenal bytes ≤ 512 and a multiple of 8, salvage 0..1000000), then `Rules_ApplyHostLoadout(upgrades, 0x400,
  arsenal, arsenalBytes, salvage)` in gameplay with no join step running (the newest one wins). The weapon cabinet itself
  is derived from the upgrade table (`add_unlocked_weapons_to_weapons_cabinet` 0x611630 adds every type-2 upgrade with
  level ≥ 1); `Weapon_list` is sent too because the save keeps its own list (`hud_ui_weapon_cabinet_load`).
- **Salvage** (rule Q13): guest.cpp calls `Session_SendSalvage(amount)`; the session sums it and sends SALVAGE at most 4
  times a second (≤ 30000 each). Host: 1..30000 per message, else dropped and logged; summed (capped at 30000) and
  handed to `Rules_AddHostSalvage` in gameplay. No per-second limit: Drain counts messages when the host ticks, not when
  they arrived, so a host stall (load, save) bunches them and a limit would drop real salvage; `add_salvage` caps the
  wallet at 30000 (0x611725 `cmp eax,0x7530`), which bounds a hostile guest anyway. Guest side: guest.cpp sends a frame's
  gain before it applies a LOADOUT (which overwrites the wallet and the baseline), so a pickup in that frame is kept.
  **Nothing is lost when the link ends:** the guest's Stop (F8, quit to the main menu) sends what it has not sent yet
  (≤ 30000 per SALVAGE, up to 34 back to back) before BYE on the same reliable ordered channel 0; the host keeps what it
  received across Unlink, Stop and a new link (only `g_salvOut` is reset) and adds it on its next gameplay frame with a
  Player, linked or not, so a SALVAGE drained with the BYE and the disconnect (the lag shim posts its held packets
  just before the disconnect) or one that waited out a menu, the map or a load still reaches the wallet.
- **Leave** (§4.8): guest gone -> the host removes its puppet, releases its token, plays on. Host gone (BYE or a
  disconnect) -> the guest removes puppets, releases its token, keeps fences and guard, HUD "Co-op: <host> gone,
  reconnecting (F8 stops; your own campaign needs a game restart)" (the reason is in the log), and keeps redialling: a
  host that comes back sends its world again; the session goes on, so Q18 restores the guest's weapons over that join. The guest's own F8: "Co-op: session stopped (F8). Restart the game to continue your own campaign".
  A guest at the main menu (Esc -> Quit or the death screen's Main menu; state 0 with no Player, which no load passes
  through) ends the session in any phase, `Stop("the guest quit to the main menu")`: armed, it loads nothing of its own
  and the blob needs a Player, so it could never join again, and the host gets BYE instead of a 120 s join pause. Its
  title then reads "co-op off: restart the game (a co-op guest cannot load)" (any armed guest without a Player outside
  GS_LOAD); New Game is refused too (guest_impl.md GF-LOAD), so a restart is the way back.
- **Dev keys** (world.cpp): F9 / F10 are refused while a session runs or once the game armed as a guest ("World: F9 / F10
  are off during co-op"); F9 adds ". Earlier snapshot replaced" when it overwrote one; "Save guard: disk write #N
  skipped" shows without dev=1 too.
- **Title** while linked: ", partner joining" / ", join waits" (host); ", receiving world N%" / ", joining" / ", host
  paused" (guest).

### Protocol additions (kProtocol 4)

| Type | Id | Ch | Size | Direction | Payload |
|---|---|---|---|---|---|
| JOINWAIT | 11 | 0 | 121 | host -> guest | MsgReject layout: why the join waits (a mission) |
| BLOB_BEGIN | 12 | 2 | 39 | host -> guest | u8 kind (0 join, 1 after a host load, 2 asked), u16 id (+1 per blob), u32 size (8..2621440), WorldMeta {i32 dlc, u8 hammers[8], u8 golden, u8 backpacks[2], u8 jetpack}, f32 pos[3] (the host, its vehicle while driving), u8 clock[3] (h m s, log only) |
| BLOB_CHUNK | 13 | 2 | 5 + n | host -> guest | u16 id, u16 index, then n = min(4000, size - index × 4000) blob bytes |
| JOINED | 14 | 0 | 15 | guest -> host | u16 id, u32 transfer ms, u32 load ms, f32 distance to the host after the teleport (-1 unknown) |
| REJOIN_REQ | 15 | 0 | 121 | guest -> host | MsgReject layout: why |
| PAUSED | 16 | 0 | 2 | host -> guest | u8 on |
| LOADOUT | 17 | 2 | 1544 | host -> guest | u8 pad, u16 arsenal bytes, i32 salvage, u8 upgrades[0x400], u8 arsenal[0x200] |
| SALVAGE | 18 | 0 | 5 | guest -> host | i32 amount (1..30000) |

Each one is checked for its exact size (a chunk: exactly the size its index implies), channel, direction (role) and
the linked phase, else dropped and logged (first 5). The blob bytes reach the game only through `World_Load`, which checks
"RFGS" v52 and buffer 0's `max_size`; the game itself checks only magic and version (`init_territory`), so a malicious
host can feed the guest's world parser anything, as a crafted save file could. Channel 2 is reliable ordered: BEGIN,
its chunks and LOADOUT arrive in send order; channel 0 messages are not ordered against them (so the host starts the
join only on the guest's READY, which follows WELCOME on channel 0).

### State machines

Host (`g_hj`): link -> idle; the guest's first READY with no world sent yet -> **want** (each second in gameplay:
capture, or JOINWAIT) -> **capture** (`World_CapturePoll`;
failed or stale -> want in 3 s) -> **sent** (paused; JOINED -> **idle**; 120 s -> idle). Own LOADING: capture / resend /
sent -> idle (-> want if the guest never had a world). First gameplay frame after a load, guest has a world: ->
**resend** (as is; no buffer -> want). REJOIN_REQ while idle or sent (2.5 s apart at least): no world sent yet -> want
(kind join); else resend (mission) or want (free roam). A capture in flight is polled every tick even after the link
ends (world.cpp runs one at a time).

Guest: receive (`g_rx`) -> complete (`g_pend`) -> `g_ga` **idle** -> **load** (`World_Load` each frame) -> **wait**
(Player NULL, then gameplay) -> **arrive** (1 s) -> **teleport** (hold) -> JOINED -> idle. The host's LOADING drops
receive and complete; a newer complete blob, a host load or the guest's death (GS 16) during arrive / teleport returns
to idle without JOINED.

### Game calls and data (M5, sim thread)

| What | RVA | ABI / use | Evidence |
|---|---|---|---|
| `game_pause(bool)` | 0x3C1620 | cdecl, caller cleans; `lock xadd` +1 on `Game_paused_count`, on 0 -> 1 pauses timers, animation, audio (the bool goes to `audiolib_all_sounds_pause`); we pass 1 | body; call sites 0x4A85FB-0x4A8602 (`push 1; call; add esp,4`) and `video_cutscene_play_enter` 0x707004 (`push 1`) |
| `game_unpause()` | 0x3C1650 | cdecl, no stack args (`push ecx` is a local); -1, on 1 -> 0 resumes; no floor at 0 | body; call sites 0x306FA1, 0x468519, 0x484912 push nothing |
| `Game_paused_count` | 0x2132880 | int, read only | the two unpause-to-0 loops above; `game_is_paused` 0x3C1610 |
| `State_info` | 0x213ADA0 | `state_info[64]`, 0x24 each: +0x18 transparent, +0x19 pause_beneath; read for the top state (`gameseq_get_state` 0x3BFC70) | `gameseq_do_state` 0x3BFB1C / 0x3BFB29, `gameseq_push_state` 0x3D8875 / 0x3D887B |
| `Game_clock` | 0x21044AC | +4 h, +5 m, +6 s, log only | `game_clock_save` 0x3C1680 |
| `Mission_active_index` | 0x125050C | int, log only (the capture checks it in world.cpp; `World_InMission` is the guest's link gate) | `mission_is_active` 0x35E970 |
| `Current_activity_mode` | 0x1E28590 | int, read only: the running modal activity's type (`activity_base_modal+0x4`), 0 none; the guest's link gate (not 13, convoy) | set `activity_base_modal::activity_start` 0x3ACA73, cleared `activity_end_common` 0x3698CE, compared by `activity_base_modal::is_active` 0x355BF5; modal = the 9 `activity_start` callers of 0x3ACA10 (raid, house arrest, DM, area defense, delivery, courier, tanks, vehicle rail, convoy) |
| `player_meta_data` | Player+0x2978 | +0x4 salvage; +0x18 `upgrade_item[128]`, 8 bytes each (byte 0 = level) | `player_get_meta_data` 0x610550; `add_salvage` 0x611700 (+0x4, cap 0x7530); `add_unlocked_weapons_to_weapons_cabinet` 0x611630 (meta+0x18, step 8, level ≥ 1) |
| `Weapon_list` | 0x1260ED0 | `farray<ui_weapon_list_item,64>`: +0x4 data, +0xC count, 8-byte items {u32 weapon handle (weapon_info+8), u8 flag} | `hud_ui_weapon_cabinet_unlock_weapon` 0x45B3F5-0x45B424, `hud_ui_weapon_cabinet_load` 0x477280 (≤ 0x40 items), 528 bytes in the symbol table |
| `world::get_state_data()` | 0x51C460 | thiscall; `stored_world_state[World+0x2578]` if its cur_size (+0x64) > 0, else NULL | body |
| capture, load, `set_pending_territory_load` | world_impl.md | the E3 code, same order; `World_Load` adds `Guest_AllowLoad()` right before `set_pending_territory_load` for the session's loads | world_impl.md, `verify_gamesave_process` 0x3F0280-0x3F0354; the death screen's own Continue calls it from GS 16 (0x49303A) |

### Log lines (M5)

- guest link: `world: save guard armed at 811278 until the game exits (co-op guest)`, then guest.cpp's lines
- host: `world: capture requested (buffer 0 held N bytes)`, `world: captured N bytes (was M) in T ms, ...`, `session: blob
  1 (join) to B: N bytes in K chunks, queued in T ms; dlc D, we are at x y z, clock hh:mm:ss`, `session: co-op pause on
  (Game_paused_count 1)`; in a mission `session: B's join waits: a mission is active`
- guest: `session: blob 1 (join) coming: N bytes, dlc D, host at x y z, host clock hh:mm:ss`, `session: blob 1 complete: N
  bytes in T ms`, `session: loading blob 1 from memory`, `world: loading a blob from memory (N bytes, dlc D, mission M,
  co-op)`, `session: sent LOADING` / `READY`, `session: in the host's world after T ms (transfer T ms): at x y z,
  Mission_active_index -1, clock hh:mm:ss (the host's was hh:mm:ss)`, `session: Join: teleport to A at x,y,z`, `session:
  joined (blob 1): transfer T ms, load T ms, D m from the host after the teleport`
- host: `session: B joined with blob 1, S s after it was sent: guest transfer T ms, load T ms, D m from us after its
  teleport`, `session: co-op pause off (...)`, `session: LOADOUT to B: upgrades + arsenal <crc>, K arsenal items
  (Weapon_list count K), salvage S`; guest `session: host loadout applied: ...` (logged when upgrades or arsenal change)
- re-join: host `session: sent LOADING`, maybe `session: our load drops the blob step in progress (n)`, `session: sent
  READY`, `session: host load done: re-sending the world it loaded to B`, `session: blob 2 (after our load) ...`; guest
  `session: A is LOADING`, maybe `session: dropped ...`, HUD "Host loading..."
- pause: host `session: sent PAUSED 1 (state S, Game_paused_count N)`; guest `session: the host paused`, `session: co-op
  pause on ...`
- salvage: guest `session: sent N salvage to the host` (at Stop: `session: N salvage to the host before leaving`); host
  `session: +N salvage from B`
- refusals and failures: `session: blob N broken: chunk ...`, `session: REJOIN_REQ sent (why)`, host `session: B asks to
  re-join (why): the loaded buffer as is | a fresh silent blob | no world from us yet: its join (free roam only) |
  refused, ... < 2.5 s ago`, guest link `Co-op: the guest fences could not be armed (RfgCoop.log): no join`,
  `session: JOINED for blob N ignored ...`,
  `session: no JOINED for blob N in 120 s: unpaused`, `session: capture for B's join failed: retrying in 3 s`, `world:
  capture stale ...` / `timed out ...`, `world: load refused: ...`, `session: the world load was accepted but never
  started`, `session: dropped type ...` (first 5), guest `session: re-join wanted (why): within 5 s of the last request,
  held until then`, `session: died while landing (blob N): the re-join takes over`, `session: F8 outside gameplay: the
  guest needs a loaded save ...`, guest HUD `Co-op: finish or cancel your mission before joining` (Start, or BYE at the
  link), and at the main menu BYE / `Co-op: the guest quit to the main menu`

### Test steps (M5; one PC, A = host, B = guest, both dev=1)

Back up both saves first and note B's `kee2_savegame_*.sav` modified time.
1. A on its campaign save (Tutorial done), B on a **different** save. F8 in A, F8 in B. B: "Co-op: A connected", "Save
   guard ON...", "Co-op: receiving A's world (x MB)", a load, "Teleporting to A", "Co-op: joined A". A: "Partner
   joining..." and frozen until then, then "Co-op: B joined". Within ~30 s B stands beside A: compare liberated districts
   on the map, a building A destroyed earlier, the time of day.
2. A in a mission when B links: B "Co-op: you join after the host's mission (a mission is active)"; A "Co-op: B joins
   when you are in free roam (...)". Finish or cancel the mission: B joins.
3. A fast travels: B "Host loading...", then B loads and lands beside A.
4. A dies in free roam: B "Host paused" (A's death screen), "Host loading...", then the re-join; both continue.
5. B dies: guest.cpp turns Continue into a re-join (B's log `REJOIN_REQ sent`); A "Partner re-joining..."; B lands beside A.
6. A plays Guns of Tharsis or EBS past its checkpoint, fails, Continue: B re-joins; B's log `Mission_active_index -1`;
   on B no objectives, script spawns, video or reload for 10 min (guest.cpp's fences).
7. A replays an activity: B re-joins and nothing starts on B.
8. Esc / map / a video in A: B "Host paused" and freezes until A resumes. Esc in B pauses only B.
9. A buys an upgrade or a weapon: about 1 s later B's log `host loadout applied` with a new value; B's locker shows it. B
   picks up salvage: B `sent N salvage to the host`, A `+N salvage from B`, A's wallet grows.
10. F8 in B: A "Co-op: B left: session stopped (F8)" and plays on. Link again, then F8 in A: B "Co-op: A gone, reconnecting (F8
   stops; your own campaign needs a game restart)". Quit B: its `.sav` modified time is unchanged; restart B and load its own save: intact.
11. F9 / F10 in either during the session: "World: F9 / F10 are off during co-op".
12. B's Esc -> Load and fast travel (map): nothing loads or warps; B's log has `guest: HUD "Only the host can ..."` at
   the click, and the line itself shows on screen only once B closes the Esc menu / the map (HUD lines need gameplay).
   Esc -> Restart: the menu closes and the line shows at once.
13. F8 in B at the main menu: nothing starts (`F8 outside gameplay`). Linked, B Esc -> Quit to the main menu: A "Co-op: B
   left: the guest quit to the main menu" and plays on (no 2 min "Partner joining..." freeze); B's title "co-op off:
   restart the game (a co-op guest cannot load)"; New Game and Load do nothing on B.
14. Kill B within 15 s of "Teleporting to A" (the join's hold), Continue: B re-joins (log `died while landing`), A's
   "Partner re-joining..." pause ends then.
15. B starts a mission of its own save (or is still in its Tutorial), F8 in B: "Co-op: finish or cancel your mission
   before joining", nothing dials. Start the mission while B is still dialling (A not hosting yet), then host in A: A
   "Co-op: B left: finish or cancel your mission before joining", B's save guard never armed.

### Open risks (M5)

1. **Never run.** New on the host: a pause token held across the guest's whole load (transfer + load + 1 s + up to 15 s
   streaming hold; 120 s cap). New on the guest: a blob load started from our tick on the death screen (GS 16; vanilla
   does it from the same state, 0x49303A).
2. **Pause count resets**: the two unpause-to-0 loops (level exit, level init) are taken as the only resets. Another one
   while a token is held would make our later `game_unpause` take away a vanilla pause (we only call it while the count is
   above 0, so it never goes negative).
3. **Host pause detection** reads only the top game state; a transparent state on top of a hiding one would read as
   running. Briefings (transparent, not pausing) do not pause the guest (the Q16 notice is M9).
4. **As is after a disk load**: RESEARCH §4.8 asked for a fresh silent blob after a host Esc -> Load; this build sends
   the buffer that load used (the save, copied there by `verify_gamesave_process`), and falls back to a fresh blob only when
   no valid buffer exists. If B shows A's previous world after Esc -> Load on A, switch that case to a fresh blob.
5. **`World+0x2578` left at 1 in free roam** (after Save the Guerrilla Camp, §4.8 step 2): resolved. It is no longer a
   refusal nor "in a mission" (`World_InMission` = `Mission_active_index != -1` only): the join and a REJOIN_REQ make a
   fresh silent blob, whose save sets it back to 0 (`save_state_data` 0x541279), instead of waiting with a misleading
   "after the host's mission" or re-sending the finished mission's mid-mission checkpoint (stale world). A host load
   still re-sends the buffer that load used, as is.
6. **Slow links**: all chunks are queued at once; on a WAN ENet's 64 KB reliable window makes 1.3 MB take seconds, and
   PSTATE and control share the link meanwhile. Loopback should be well under a second.
7. **A REJOIN_REQ while the host waits for JOINED** restarts the send; the host unpauses for a frame or two in between.
8. **Teleport after arrival**: 1 s after the first gameplay frame; a script or spawn moving the guest later drops the hold
   (StepTeleport's 10 m rule) and the guest stays at the safehouse (JOINED still sent with the distance; F7 by hand).
9. **Hammer purchases are not live** (rule Q13 says live: needs the user's OK or the swap in guest_impl.md "Upgrade
   effects"). LOADOUT copies the upgrade table, which is all backpacks and the jetpack need (player::do_frame 0x6D5694
   recreates them from it; the process globals `s_backpacks_unlocked` / `s_jetpack_upgrade_level` / `s_hammers_unlocked`
   only feed shop prices, saves and level init, and WorldMeta sets them at each join). A hammer purchase also swaps the
   buyer's slot-3 inventory item, which nothing re-applies: the guest keeps its old hammer until its next re-join.
10. **Time of day**: the blob carries the clock and the host is paused during the guest's load, so both start equal; it
   drifts afterwards (live WORLD sync is M7a). An as-is re-join carries the time of the host's load.
11. **dlc id** from the wire is only bounded (-1..16); an honest host sends its base-campaign value (Q12 refuses DLC levels).
12. **A guest without a level**: the blob loads only with a Player (gameplay or the death screen), and once armed the
   guest can load nothing of its own (Load, New Game, DLC all refused). So the guest's F8 needs gameplay (load any save
   **before** F8, in free roam: no mission, no modal activity but a convoy), and a guest that reaches the main menu ends
   the session; after that only a restart brings it back.
13. **Arm order**: the save guard, then `Guest_Arm()`; a failed arm ends the session at the link ("the guest fences
   could not be armed"); the first-blob check ("the guest fences are not on") is a backstop only.

## Log lines to expect

- load: `session: role host, port 27777, connect :27777, autoconnect 0, lag 0 ms, ...` (or `no role ... co-op off`)
- first F8: host `session: Tutorial = mission <i> "<name>", flags <hex>, complete 1`; `session: handshake values:
  protocol 3, rfg.exe 5B9B713D/0397B000, xinput1_3.dll <crc>, winmm.dll 00000000, dinput8.dll 00000000, data
  <hash> (<n> vpp_pc), name A`; `session: password set: the guest proves it in AUTH, then the host in WELCOME` (or
  `none: open session`); host `net: password key made in <n> ms` (password only); `net: thread up, hosting, ...`
- guest: `net: connecting to 127.0.0.1:27777`, maybe `net: no answer ..., retrying in 3 s`
- both: `session: connected to <ip:port>, handshake`; host: `session: HELLO from B: ..., password 1`, `session:
  CHALLENGE sent to B`; guest: `net: password key made from the host's salt in <n> ms`, `session: CHALLENGE, AUTH
  sent with our proof`; host: `session: the guest's proof checks out` (open session: guest `session: CHALLENGE, AUTH
  sent (open session)`, host `session: the guest echoed
  our nonce (open session)`, `session: open session (no password): ...`); both: `session: linked with
  <name>`; `puppet: hooks installed (...)` (first link), `puppet: L5 melee open`, `partner: session linked: ...`,
  `session: sent READY`, `partner: clock offset set at <n> ms`
- partner puppet: `puppet: PP-STATS on`, `... listener store off`, `... hash table ... primed`, `partner: created
  puppet <p> handle <h> at x,y,z; ... puppet list index 1 ...`, `partner: puppet weapon -> slot N item <p>`
- every 5 s: `session: RTT n ms, PSTATE out N in N lost N, partner B` and `partner: puppet behind the partner's last
  PSTATE by X m, puppet states N, ring N, delivered N, visible 1, hp N, clock offset N ms; ...`
- loads: `session: sent LOADING` / `READY`, the other side `session: B is LOADING` / `READY`, `partner: puppet ... is
  gone (level unload?)`, later `partner: created puppet ...`
- refusals: host `session: refused the guest: <reason>`, guest `session: the host refused us: <reason>`
- wrong password: host `session: failed handshake from <ip> (wrong guest proof), n of 5`, `session: refused the
  guest: wrong password`, HUD "Co-op: guest refused: wrong password"; guest `session: the host refused us: wrong
  password`, HUD "Co-op: refused by the host: wrong password". The 5th: `...: refused for 60 s` and REJECT "wrong
  password (5 tries: wait 60 s)", then `session: refused the guest: too many wrong passwords: try again in N s` on its
  connects (guest: "refused by the host: too many ...", stops); an AUTH out of turn, of the wrong size or without our
  nonce: `session: AUTH ... (not counted)` and a REJECT
- a host proof that fails (not a stock host): guest `session: the host's password proof is wrong`, HUD "Co-op: the
  host does not know the password"; that host `session: B said BYE: the host does not know the password`
- too short: HUD "Co-op password too short: use at least 10 characters (or none for an open session)", nothing starts
- a kicked peer that does not ack: `net: the kicked peer did not leave, dropped`
- Tutorial: `session: Tutorial = mission <i> "<name>", flags <hex>, complete 0|1` at each check; outside gameplay
  `session: F8 outside gameplay: the host needs a loaded save`; `session: no Tutorial mission among <n>: co-op not
  allowed` is defensive only (not expected: DLC levels keep the base Tutorial); BYE `session: A said BYE: <reason>`
- disconnect: `session: peer disconnected (after BYE | timeout or drop | before the link)`, `partner: session down`,
  `partner: puppet destroyed`; `net: thread down` after F8.
- every HUD line is also logged as `session: HUD "..."`.

## Test steps (one PC, A = host, B = guest, both dev=1)

1. A's ini: `role=host`; B's ini: `role=guest`, `connect=127.0.0.1:27777`, a different `tag`/`name`. Same RfgCoop
   build in both folders. Load the same save in both (B's `kee2` copy).
2. F8 in A (title: "host, listening on 27777"), F8 in B: both HUDs "Co-op: <name> connected", titles show the RTT.
   Allow the Windows Firewall prompt on A if it appears (loopback works without it).
3. Each window shows the other's Mason where the other stands: walk, sprint, jump, crouch, shoot, swing, throw; the
   green star on the minimap. Drive in one: the other's Mason disappears, its green star follows the vehicle on the
   minimap, and the Mason reappears when you get out. Walk 1 km apart: the star stays on the map.
4. F7 in B: B lands beside A (also from 2 km away: B hangs in the air until the terrain loads).
5. Fast travel / save load / death in one: the other shows "<name> is loading"; afterwards the puppet comes back on
   both sides. Esc menu and alt-tab for minutes: the link stays (RTT keeps updating).
6. Refusals: a different `password` in B: B shows "Co-op: refused by the host: wrong password", A "Co-op: guest
   refused: wrong password" (A's log: `failed handshake ... 1 of 5`); fix B's ini and F8 again (the password is read at
   each start). `password=short1` in either: F8 shows "Co-op password too short: ..." and nothing starts. A password
   in A only: B shows "refused by
   the host: a password is set on the host only ...". B with a different RfgCoop dll (or a winmm.dll only in one
   folder): B shows "refused by the host: ...". The ver5 dll in B: "RfgCoop protocol 3 on the host, 2 on the guest".
   F8 in B stops, F8 again reconnects. `lag_ms=150` in both: the puppet lags but moves smoothly.
7. Throttle: 5 wrong-password tries from B in a row (F8 each time): each shows "wrong password" on both, A's log
   counts 1 to 5 of 5 and the 5th adds "refused for 60 s"; B's 5th REJECT reads "wrong password (5 tries: wait 60 s)".
   An F8 in B within the minute is refused before the handshake: B shows "refused by the host: too many wrong
   passwords: try again in N s" and stops (no redial), A shows "Co-op: guest refused: ..." (not a strike); with the
   right password B links once the minute is over.
8. Tutorial rule: F8 in A in the main menu: nothing starts, A's title reads "co-op off (F8 in gameplay, on your save)"
   and the log says `F8 outside gameplay`; on a new game (Tutorial not done): "Co-op needs a save that has finished the
   Tutorial". Linked, A loads such a save: B shows "A left: session stopped: the host's save has not finished the
   Tutorial". A DLC / DotB level in A: log `session: Tutorial = mission <i> "<name>", flags ..., complete 0`, HUD
   "Co-op needs a save that has finished the Tutorial"; linked, the host-save-not-finished stop message.

## Open risks for the in-game test

1. **Never run.** The partner puppet is the E2 ghost with a different feed; the ghost's open risks (ghost_impl.md)
   all apply. New: a puppet made at the partner's position instead of yours; states 50 ms apart instead of one per
   frame (interpolation spans two states inside the 100 ms delay).
2. **Hidden puppets keep a body** where they were last seen (as the ghost while you drive): AI may target it and it
   blocks that spot. If that spot itself stops being streamed (you walk away while the partner is out of range), the
   body has no ground under it and may fall; never_die and the hidden state cover it, and the next visible state
   resets the sim state (pop).
3. **Parallel worlds**: buildings and NPCs differ between the games; the puppet can walk "through" a wall that is
   intact on your side, and its weapons damage your buildings and vehicles (only human damage is firewalled); your
   next save keeps that damage (see "Damage" above).
4. **Minimap star**: `add_temporary_icon` from our code has never run; if the minimap misbehaves, `partner_icon=0`.
   The `stick` flag is passed as the squads pass it; what it does on the minimap edge is not read yet.
5. **ENet RTT** reads ~500 ms for the first seconds (ENet's default), then converges; on loopback expect ~5-10 ms
   because each net thread services every 5 ms.
6. **Failed dial timing**: a guest started before the host retries with ENet's default connect timeout (~15-30 s per
   attempt, then 3 s). Starting the host first links within a second.
7. **Clock**: both clocks start at 10000 at their own first install; the offset estimator absorbs the difference. A
   partner whose game hitches for seconds sends nothing meanwhile (no re-sync); the puppet extrapolates (the engine's
   `Max_extrap_time_ms` = 1000). A stall on your side drains the queued states on resume: they map into the past and
   only the newest moves the puppet.
8. **First-F8 hitch**: the handshake reads the three DLL files (a few MB at most) on the sim thread once.
9. **Alt-tab on two PCs**: fixed in M5. 0x4DDB5F is now patched while a session runs whatever `dev` says (core.cpp
   `QuickPauseOff`, from Start to Stop; dev=1 keeps it for the whole run), so an unfocused game keeps running. Esc, the
   map and videos still pause (on the host: PAUSED, §4.7).
10. **Name tag** (`hud_icon_add`) and PEVENT are not built; weapon and zoom ride in every PSTATE.
11. **Two guests**: the host links one partner; a second guest connects at the ENet level, gets a disconnect at once
   ("peer disconnected (before the link)") and redials every 3 s.
12. **Tutorial check timing**: read on the first gameplay frame with a new Player; the flags are loaded from the save
   during the territory load (`mission_system_load`), before that frame, by the static reading. If some load path set
   them later, a good save would be refused once (F8 again works). DotB / DLC levels keep the base list with the
   flags cleared at level init (Tutorial rule above), so they are refused through complete 0.
13. **Password costs**: PBKDF2 takes ~35 ms (34 ms in game with ver5) on the net thread: the host's at each start,
   the guest's at each connection's CHALLENGE (both logged); a slow CPU only delays the listen / the AUTH. The
   remaining password risk is an offline guess from an AUTH proof at PBKDF2 speed, by a malicious host the guest dials
   or by anyone who watches a handshake (threat model above). A stock guest's mistyped password now counts as a strike (5 = 60 s wait). A NAT that puts several players
   behind one address shares one throttle entry.
