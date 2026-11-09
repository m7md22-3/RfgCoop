# RfgCoop notes: transport and session layer

Domain: the engine networking transport, the session layer, and which transport co-op should use. Distilled 2026-10-07 from
`research/findings/workflows/coop_transport.md` (the investigator report) plus the skeptic verdicts on claims T1-T8.
Build: GOG rfg.exe, 32-bit, PDB image base 0x400000. All addresses are **RVAs** unless marked VA (VA = RVA + 0x400000).
Tools: the PDB, disassembly and the symbol/xref tables. Despite what the context describes, the xref table
columns are `target RVA, kind, site RVA, containing function`.
Tags: **[V]** means someone read the code, data or logs. **[I]** means it was reasoned, not read.

## TL;DR

- The engine MP stack is complete: a reliability layer (`multi_connection_*`), sessions, a 128-message packet table, LAN raw
  UDP and an internet path through sw_api.dll. But all of its per-frame transport work sits behind `Multiplayer` (0x2BEA588)
  in `multi_process`, and that flag steers 1058 functions of single-player code. **Do not reuse it for campaign co-op.** [V]
- **Transport: vendor ENet 1.3.18** into the co-op DLL. Build it with llvm-mingw i686 `-static` and link
  `-lws2_32 -lwinmm`; the toolchain already has both libraries. ENet touches no engine state. [V facts / I choice]
- **Run ENet on its own network thread, which owns the `ENetHost` exclusively** (ENet is not thread-safe). Exchange data
  with the sim thread through locked queues, and init after DllMain returns. Never pump it from the GS_GAMEPLAY process
  hook, because that hook stops during pauses and loads. The sim thread itself keeps ticking in every state, and territory
  loads are async by default.
- **Connectivity is the user-facing problem.** Start with direct IP plus a UDP port forward, or a virtual LAN (ZeroTier,
  Tailscale, Radmin). NAT punch through sw_api's THQNO RakNet is an unproven option for later: the stock GOG DLL points at a
  dead facilitator, and the relay was never shown to carry traffic.
- Topology: host-authoritative client/server. Channel 0 reliable ordered (events), channel 1 unreliable sequenced
  (transforms, 20-30 Hz), channel 2 reliable bulk (join-in-progress). Use a handshake with a version, exe and content check
  plus a password, and validate every received message.

## Facts

### Layer map [V]
```
multi_game_* / multi_core_object_*   game messages (packet_def tables)
multi_session_* / multi_join_*       session, join, migration
multi_connection_* + multi_io_*      acks, resend, unreliable buffer, bandwidth
multi_core_platform_internal_send_packet 0x5420D0 / _receive_packet 0x58DE20
   -> lock g_gameLinkMutex (0x2BEA77C) -> Remote_game_session.m_pGameLinkInterface -> vtbl
unit4::RemoteGameSession  Remote_game_session 0x127E500 (size 0x12000)
   +0x08 GameLinkSessionParameters, +0x3C LocalGameSession*, +0x4C bool lanCreated,
   +0x50 GameLinkLan (connectionType 0), +0xB38 bool internetCreated, +0xB40 GameLinkInternet (type 1),
   +0x11FF0 GameLink* m_pGameLinkInterface (null without an MP session), +0x11FF4 bool m_isInCriticalErrorState
GameLinkLan      -> keen::network::sendDataTo 0x884DD0 / receiveDataFrom 0x884CE0 -> WS2_32 sendto/recvfrom
GameLinkInternet -> SteamNetworking() 0x8D13A0 -> sw_api.dll ISteamNetworking SendP2PPacket/ReadP2PPacket
```
- Send path: `getGameLinkSession` 0x810FA0 (`[ecx+0x11FF0]`), then vtbl+0x84 `getRemoteStationById`, then vtbl+0x5C
  `sendP2PPacketUnreliable`. Receive path: vtbl+0x60 `getNextP2PPacket`, then `connection_address::set` 0x543AE0.
- `RemoteGameSession::beginGameLinkSession` 0x811C60 picks +0x50 or +0xB40 and calls vtbl+0x10 `beginSession`. Its only
  caller is `multi_core_platform_internal_session_start` 0x5A7E40.
- Both GameLinks are created at boot: `multi_core_platform_internal_system_init` 0x541CC0 calls `RemoteGameSession::create`
  0x811990, which calls `GameLinkLan::create` 0x8D6A40 and `GameLinkInternet::create` 0x8D3EA0. Both are updated every frame
  by `RemoteGameSession::update` 0x811A40, **in single-player too**.

### GameLink layer [V]
- `kaiko::gamelink::GameLink` (0x18 bytes):
  - +0x04 `m_state` (0 Offline, 1 Online, 2 InLobby), +0x08 `m_error`, +0x10 `m_gameplayRunning`.
  - vtbl: +0x10 beginSession, +0x30 update, +0x5C sendP2PPacketUnreliable, +0x60 getNextP2PPacket, +0x74 lobby member
    count, +0x84 getRemoteStationById.
  - Only an **unreliable** send exists at this layer. Reliability comes from `multi_connection` above it.
- `GameLinkSessionParameters` (0x30): +0 connectionType (Lan 0, Internet 1, Wireless 2), union at +4.
  - Lan (0x2C): +0 u16 port, +4 gameUniqueIdentifier, timing fields +8..+0x24, +0x28 hostMigrationEnabled.
  - `multi_init` fills it with port = `Broadcast_port` (0x127C81C, static 1005, which only multi_init reads), id
    0x12345678, 1000 ms search, 200 ms resends, 100 ms keep-alive, 5000 ms timeouts.
- `GameLinkLan` (0xAE8): +0x1C params, +0x4C `m_socketBroadcast`, +0xAC8 lobby members.
  - `beginSession` 0x8D3000 makes the **only** `keen::network::createSocket` call in the exe (site 0x8D305B) and binds it
    to `0.0.0.0:[this+0x20]` (params+4 = 1005). All LAN traffic uses that one UDP socket.
  - LAN packet header (12 bytes): `{u32 uid, u32 size, u32 type}`, with type 1 UserDefined, 2-6 lobby/join, 7 KeepAlive.
- Peer address: `connection_address` (10 bytes) = `RemoteUserId {char providerData[8]; u8 guestIndex; bool isValid}`. For LAN
  it holds ip4 + port. For Internet it holds a 64-bit id, which is the RakNet GUID.

### Bytes on the wire [V]
- **WS2_32 imports.** The IAT starts at 0xE3F68C, and slot 0 is ordinal 20 `sendto`. `sendDataTo` 0x884DD0 calls `htons`,
  then `[IAT 0xE3F68C]` (site 0x884E39).
  - Its callers: `GameLinkLan::sendBroadcastPacket`, `sendPacketTo`, `sendPacketToHost`, `sendP2PPacketUnreliable` 0x8D33B0
    and `update`.
  - The other sendto/socket users in the exe are not game netcode: libcurl (tftp/ftp), Havok `hkBsdSocket`, and the
    `createAsyncNetworkSocket` that SMTP uses.
- **sw_api imports.** The IAT starts at 0xE3F7D8; slot 4 (0xE3F7E8) is `SW_CCSys_DynamicInit`. `SteamNetworking()` 0x8D13A0
  is `push ctx(RVA 0x10F4008); call DynamicInit; mov eax,[eax+0x20]`. `SW_CCSys_ProcessApiCb` is called from
  `updateSystemServicesPlatform` 0x8136F0.
- `GameLinkInternet::sendP2PPacketUnreliable` 0x8D4530:
  - It requires `m_state==2`, a valid id, and a **payload of at most 0x4A8 (1192) bytes**.
  - It adds an 8-byte header `{1, len}` (1200 bytes on the wire) and calls `SendP2PPacket(id, buf, len+8, type 0
    Unreliable, channel 0)`.
  - The other Steam send sites use type 1 (UnreliableNoDelay) on channel 0, with no visible length check:
    `sendP2PPacketToSteamID` 0x8D2A40, `sendPacketToAllOthersInternal` 0x8D29C0, `updateAuth`, and
    `handleValidateAuthTicketResponse`.
  - **No reliable send type (2 or 3) is used anywhere.** The enum and vtable order match
    `RfgNetworking-master/src/API/SteamAPI.bf:943-949, 408-431`.
- `getNextP2PPacket` 0x8D5A80 reads channel 0 into a 0x4B0 buffer.
- Galaxy: rfg.exe neither imports Galaxy.dll nor contains a "galaxy" string. Only sw_api.dll imports it.

### sw_api.dll = THQ Nordic "SteamWrapper" [V, strings + `<GOG Galaxy folder>\...\logs`]
- **What it is.** RakNet with `NatPunchthroughClient` and `UDPProxyClient`. The build path is
  `c:\work\thqonline_clean\...\steamwrapper\3rdparty\raknet\`. It uses a THQNO lobby/backend at `176.9.91.194:38888`
  (client_api.log). Its RakNet host binds the fixed UDP port 23022.
- **The two copies.**

  | Copy | SHA-256 | Facilitator |
  |---|---|---|
  | Stock, `<game copy B>\...` | 3d5b4130… (SyncFaction `Hashes.cs:138`, Gog dictionary) | `94.130.198.244:61111`, **dead**: "Failed error 17" nine times on 2026-09-26 19:50-19:53 |
  | The user's, `<GOG Galaxy folder>\...` | 67f9f8e9… ("GOG Unification Patch", SyncFaction `docs/modding/verification.md:49`) | 19 bytes at file offset **0x295968** patched to `nat2.thqonline.net` (5.75.181.203) |

- **What the logs show.**
  - With the patch, the facilitator connected on every launch through 2026-10-07.
  - NAT punch-through **succeeded only on 2026-10-01** (10 × `OnNatPunchthroughSucceeded`, with valid pings).
  - On 2026-09-26 a punch failed and a relay was allocated (`OnForwardingNotification` 5.75.181.203:64968), but **all 54
    pings through the relay were -1**. No relayed traffic has been shown.
  - The peer id is a RakNet GUID that is new on every launch (60 sessions, 60 distinct GUIDs).
  - `galaxy::api::Init` failed on 77 of 84 launches, but succeeded on every 2026-10-01 launch.
  - `num lobbies: '0'`: the public MP population is empty.
- **No community replacement of the P2P layer exists.** RfgNetworking-master's `CNetworking.bf` is stubs, and its default
  build is a logging pass-through to the original DLL.

### multi_connection and the packet table [V]
- `multi_connection` (0x3BF8 bytes):
  - +0x00 addr, +0x24 owner session, +0x34 status (0 PENDING, 1 DISCONNECTED, 2 CONNECTED, 3 CONNECTED_GAMEPLAY),
    +0x38 disconnect reason.
  - +0x5C packet tracking (800 packets), +0x3330 64 ordered channels.
  - +0x345A `unreliable_buffer[1200]`, +0x390A its size, +0x3AB8 pings, +0x3AF0 available_bandwidth.
  - The pool holds 48 connections (`multi_connection_init` 0x5D7AE0), all from `MP_allocators_mempool`.
- Datagram layout, from `multi_connection_flush_current_packet_no_sync` 0x58EEC0:
  1. Address.
  2. u16 BE tracked id.
  3. Reliable blocks `{u16 BE id, u8 tag, payload}`. The resend deadline is now+0x1F4 (**500 ms**, site 0x58F027).
  4. 0xFFFF terminator.
  5. Unreliable messages.
  6. Optional "bull_taco" probe (id 0x27).

  The datagram is **capped at 0x400 bytes**. Sending runs under `Multi_thread_mutex` (0x2BEC1B8).
- Message framing: `[u8 id][u16 BE len][payload]`. `multi_io_process_packet` 0x58F8C0 does `cmp al,0x80; ja reject`, so it
  **accepts 0..0x80**. That is an off-by-one: id 0x80 reads one entry past the table. It then checks direction, connection
  state and cheating before calling the cdecl `receive_func(payload, sender)`.
- `packet_def` (0x1C): +0 id, +4 receive_func, +8 name, +0xC short_name, +0x10 direction, +0x14 connection_status,
  +0x18 connection_required, +0x19 drop_on_cheating, +0x1A group_id.
  - The tables are `Multi_core_packets` 0x127C848 (46 entries, ids 0-45) and `Multi_game_packets` 0x127D710 (82 entries,
    ids 46-127), copied into the heap block `Multi_packets` (pointer at 0x2BEA58C, 0xE00 bytes).
  - **The 128-entry table is full**, but the u8 ids 0x81-0xFF are unused on the wire.
- Tuning defaults (all writable `.data`):
  - `Reliable_packet_disconnect_ms` 0x127C824 = 20000.
  - `Force_send_rate_ms` 0x127CD60 = 100, `Max_rate_sec` 0x127CD64 = 10, `Base_bandwidth_kbits` 0x127CD68 = 64 (8192 B),
    `Max_data_per_packet` 0x127CD70 = 9.
  - These change at runtime. `multi_io_get_packet_rate_per_second` 0x58FAD0 sets the server to 10/s at 100 ms and the
    client to 15/s at 66 ms. `update_bandwidth` 0x5AA050 calls `multi_io_set_bandwidth_in_bits_sec` 0x58FB40.

### multi_process gate [V]
- `multi_process` 0x604C20 has a single caller, `rfg_do_frame` (site 0x1D24D9). That call is skipped in gameseq states 4
  and 5, and its argument is `game_booted_to_main_menu()`.
- **Before the gate** (runs in single-player):
  - gamelink update under `g_gameLinkMutex`, `process_presence_state`, `multi_core_platform_internal_system_process`
    (0x604CCE), and invites.
  - The invite path can call `game_boot_to_main_menu(0xD)` at 0x604D23 even with `Multiplayer=0`.
- **The gate** is at 0x604D32: `cmp byte [Multiplayer],0; je 0x604F62 (ret)`. A second exit is at 0x604D46 when booted to
  the main menu.
- **What it skips:** `multi_core_clock_process`, `multi_io_process_incoming` (0x5D9420), `multi_connection_update_latencies`,
  `multi_session_system_process` (0x5FC5D0), the server/client/migration dispatch, QoS, voice, party, `multi_game_process`
  (0x5DBB10), acks, and `multi_io_send_process` (0x5FD740, called inline at 0x604F73 or as the job scheduled at 0x604F29).
  **None of these has any other caller.**
- **With `Multiplayer=1`**, `multi_process` tail-calls `game_boot_to_main_menu(0xE)`:
  - at 0x604D80 if `Remote_game_session+0x11FF4` is set or `LocalGameSession::didLocalPlayerExit` (0x812050) is true;
  - at 0x604DE4 if gamelink state > 0x18 and a Lobby_session condition holds.
- Server/client dispatch also requires `multi_session_get_by_type(2)` to be non-null (0x604E1C).

### multi_init 0x5FD3C0 (`void __cdecl(bool syslink)`) [V]
- **Early-outs:** it returns at once if `Initd` (0x2BEA592) == 1 or `Multi_state` == 1 (0x5FD3E5 to 0x5FD3FD).
- **Then, in order:**
  1. `Multi_syslink` (0x2BEA589).
  2. `multi_core_memory_alloc` 0x543000 takes **0x2F00200 + 0x80000 bytes from `Level_memmgr`** and builds
     `MP_allocators_mempool` (cpu part) plus `MP_multi_purpose_mempool`.
  3. `multi_game_memory_alloc`.
  4. `multi_core_platform_internal_session_start` (0x5FD49D).
  5. **`Multiplayer=1` (0x5FD4C4), `Multi_state=1` (0x5FD4CB)**.
  6. `multi_io_init` (0x5FD58A), `multi_core_dc_init`, and `Multi_packets` (0xE00).
  7. `Current_game_type_context=1`, `Current_game_mode_context=2`.
  8. `multi_voice_init`, `multi_game_init`, the MP sound bank, and the scoreboard init.
- **Callers and shutdown:** the only caller is `multi_init_job` (site 0x4AB92F). `multi_shutdown` 0x600900 clears the flag
  (site 0x600991).
- **The "5 MB VRAM" part has a NULL base on PC.** `MP_alloc_vram_buffer` (0x2BEA598) and `Mp_mp_vram_buffer` (0x2BEA5A0) are
  only ever written with 0, in `multi_core_memory_release`.
- **Not gated on the flag:** `multi_core_memory_alloc`, `multi_io_init` and `multi_connection_init` contain no Multiplayer
  check. `multi_core_memory_alloc` has a second caller, `multi_memory_alloc` 0x571650.

### Scope of `Multiplayer` (0x2BEA588) [V counts]
- The flag is referenced at 1663 sites in 1058 functions. 1660 are reads, 2 are writes (multi_init, multi_shutdown), and 1 is
  an address compare in `player_process_cover_movement` 0x688324.
- The reading functions include:
  - 15 `world::` functions (`load_territory`, `serialize_zone`, `process_streaming_objects`, `monitor_entering_zones`…)
  - `save_game_callback`, and 86 functions with "activit" in the name
  - mission start (`player_start_mission`, `mission_warp_and_start`)
  - `ai_system::do_frame`, 4 `gfm_*` functions, 38 `rfg_dyn_*` functions, 8 `shape_cutter` functions, and
    `interface_process`
- About 233 of the 1058 are multi_/mp_ functions. The rest are real branches over single-player work; six sample sites were
  checked, for example `save_game_callback` VA 0x9010C4 and `world::serialize_zone` VA 0x93F536.

### Threads, misc [V]
- `g_gameLinkMutex` (0x2BEA77C) wraps every GameLink call. `Multi_thread_mutex` (0x2BEC1B8) wraps incoming IO and the
  platform send. sw_api.dll runs its own threads, and its callbacks reach the game in `SW_CCSys_ProcessApiCb`.
- Sim thread: `rfg_do_frame` 0x1D2420 runs every frame in every game state. The GS_GAMEPLAY process hook only runs
  while GS_GAMEPLAY is on top or sits under a non-pause_beneath state.
- Territory load is async by default:
  - `gs_load_process` 0x512CE0 checks `Async_level_load` (0x1262022; its initial byte is 1, and no writer was found).
  - It schedules `world_load_job` 0x4AAEE0 (`world::load_territory`, then sets `Gs_load_finished` 0x28024C4).
  - It then returns each frame after `os_sleep(10)`. The synchronous branch (VA 0x913074) is not taken by default.
- Lag sim: `Net_lag_*` (0x1118544..) and `keen::network::ErrorSimulation` are debug-only and unused in the shipping path. A
  lag shim in our own transport or the clumsy tool is easier.
- Discord: rfg.exe imports discord-rpc. `multi_core_platform_discord_join_game` 0x572910 parses a hex u64 join secret and
  passes it to `kaiko::gamelink::setInvitationData` 0x8D14D0.
- Second instance: rfg.exe imports no `CreateMutex`/`OpenMutex`/`FindWindow`. Whether a second instance runs is [I]. sw_api's
  fixed port 23022 and the shared save folder may collide.

### Third-party [V]
- **ENet 1.3.18** (upstream lsalzman/enet, MIT).
  - Sources: `callbacks compress host list packet peer protocol unix win32 .c`. CMake links `winmm ws2_32`.
  - Constants: `DEFAULT_MTU 1392`, `PEER_TIMEOUT_LIMIT 32 / MINIMUM 5000 / MAXIMUM 30000`, `PING_INTERVAL 500`.
  - The timeout only fires while a reliable command is unacked (protocol.c 1372-1379). `enet_peer_timeout()` (enet.h:585)
    changes it.
  - `enet_initialize` = `WSAStartup(1.1)` + `timeBeginPeriod(1)`. rfg.exe already imports both DLLs.
  - It has no NAT traversal and is IPv4 only.
- **Toolchain:** llvm-mingw is clang 23.1.2, with the `i686-w64-mingw32` target. `i686-w64-mingw32/lib` has
  `libws2_32.a`, `libwinmm.a` and `libiphlpapi.a`.
- **RakNet-master** is 4.081: unmaintained, 268 files, and has NAT punch, proxy and ReplicaManager3. It ships no mingw build
  files. That it would build with llvm-mingw is [I].

## Approach for co-op

1. **Leave the engine transport alone.** Do not call `multi_init`, set `Multiplayer`, write `Remote_game_session`, register
   anything in `Multi_packets`, or call the Galaxy SDK. Borrow ideas from the engine, not code paths:
   - the `packet_def` shape as a template for our message ids;
   - 500 ms resend and 20 s disconnect as tolerance references;
   - `multi_destruction_compress` for the destruction domain.
2. **ENet 1.3.18** in `RfgCoop/third_party/enet`, compiled as C with an llvm-mingw i686 `-static` build plus
   `-lws2_32 -lwinmm`. Keep every transport call in one source file. Add no backend abstraction until a second backend
   exists.
3. **Threading:**
   - A dedicated network thread, created after DllMain returns, runs `enet_initialize`, `enet_host_create` and an
     `enet_host_service(host,&ev,5-10 ms)` loop. It is the only thread that calls `enet_*`.
   - Locked inbox/outbox queues connect it to the sim thread. The sim side (a hook in the game-state process, or an
     `rfg_do_frame` hook if traffic must flow while paused or loading) applies the inbox and produces snapshots.
   - The net thread never touches game objects.
   - Raise the timeouts, for example `enet_peer_timeout(peer, 32, 15000, 60000)`, and send an explicit "host loading/paused"
     state.
4. **Topology:** host-authoritative; the host's save runs the world. Clients send input and intents; the host sends state and
   events. Target 2 players and keep the count configurable.
5. **Channels:**
   - 0 reliable ordered: handshake, mission/activity events, spawns, destruction events, chat.
   - 1 unreliable sequenced: transforms and animation, 20-30 Hz.
   - 2 reliable bulk: join-in-progress state; ENet fragments it.
   - Keep unreliable messages under ~1200 bytes. A ~100-byte snapshot at 30 Hz is about 3 KB/s per peer [I].
6. **Handshake:** protocol version, exe check (PE timestamp 0x5B9B713D, SizeOfImage 0x397B000, from infra.md),
   mod/DLC content hash, and session password. Reject on any mismatch. **Bounds-check every length and id**: this is a
   trust boundary, and nothing received is ever executed.
7. **Port:** one configurable UDP port in `RfgCoop.ini`, clear of 1005 (syslink) and 23022 (sw_api). Expect a Windows
   Firewall prompt on the first listen. Only one DLL (whichever the infra domain picks) may own
   the host.
8. **Connectivity order:**
   1. Direct IP plus a UDP forward; show the host's IP and port in game.
   2. A virtual LAN (zero code, and the CGNAT answer).
   3. Later, only if needed: an sw_api P2P backend (after probe 4) or a tiny self-hosted rendezvous/relay.

## Contested / refuted (do not rely on)

- **"multi_init unconditionally sets Multiplayer"** (T2): wrong. It has `Initd` and `Multi_state` early-outs.
- **"The transport cannot exist without MP mode"** (T2): not proven. The allocation, `multi_io_init` and
  `multi_connection_init` have no flag check. What is proven:
  - stock `multi_init` forces the flag;
  - the per-frame IO in `multi_process` is behind it.

  Driving the engine transport by hand is still rejected: it needs ~47.5 MB from `Level_memmgr` (free space on terr01 is
  unmeasured), it shares no state with the campaign, and the session/join flow is tied to the MP lobby.
- **"5 MB VRAM pool"** (T2): its base pointer is always NULL on PC. Do not count on it.
- **"Ids 0-127 / id space full"** (T6): the table is full, but the dispatcher accepts 0..0x80 (off-by-one) and u8 ids
  0x81-0xFF are free. Moot, because co-op does not use the engine packet table.
- **"Tuning too small for co-op"** (T6): the values are defaults (server 10/s, client 15/s, adaptive bandwidth), and all are
  writable globals. Calling them too small is [I], not a hard limit.
- **"Every flag reference is a read"** (T3): 1660 reads, plus 2 writes and 1 address compare. The conclusion stands.
- **sw_api claims in the report's F3** (T5):
  - The patch offset is 0x295968, not 0x2959E9.
  - The stock hash is at `Hashes.cs:138`, not :29.
  - The dead facilitator failed nine times, not three.
  - "Punch-through and relay both work today" is overstated: punch-through worked only on 2026-10-01, and the relay never
    carried a successful ping.
  - "Galaxy Init always fails" is wrong: it fails on most launches, not all.
- **"The sim thread stalls during loads and pauses, so ENet must be on its own thread"** (T7): the premise is wrong.
  - Pauses do not stop `rfg_do_frame`, and loads are async by default.
  - The own-thread design stays, for different reasons: ENet is not thread-safe, the GS_GAMEPLAY hook does stop, and
    hitches during level init, saves and cutscenes are unmeasured.
  - The report's "sim thread blocks during loads, saves, pauses" (Implications §3) is not supported by infra.md.
- **T8 (sw_api P2P outside a lobby), unchecked by any skeptic.** The report reads `handleP2PSessionRequest` 0x8D2BE0 as
  calling `AcceptP2PSessionWithUser` (vtbl+0xC) only for `m_lobbyMembers` (+0x1A8) or `m_partyMembers` (+0x1B4) [report-V].
  The game's reader is channel 0 and runs only in MP [V via the gate]. Everything about a private-channel mod tunnel with
  its own Accept call is **untested**: whether a punch fires without a lobby, and whether sw_api maps reliable send types.

## Unknowns and probes

Each probe goes in a test build of the co-op DLL and logs to `RfgCoop.log`.

1. **ENet smoke test.** Host on port P from the net thread; a second PC or instance connects. Exchange a 30 Hz unreliable
   ping and a 1 Hz reliable counter, and log RTT, loss and disconnects. Run it through a territory load, fast travel, a save,
   the Esc menu and alt-tab. Pass: no disconnects.
2. **Sim-thread gap measurement.** Timestamp `rfg_do_frame` and the GS_GAMEPLAY process hook. Log the worst gap during a new
   game, a load, a save, fast travel, death/respawn and cutscenes. This sizes the ENet timeouts and the "loading" state.
3. **Two instances on one PC.** Does a second rfg.exe start? Does sw_api fail to bind 23022 (networking.log)? Do saves
   collide? This decides whether one PC is enough for dev testing.
4. **sw_api P2P without a lobby** (only if NAT punch is wanted). On the sim thread, under `g_gameLinkMutex`:
   - get `ISteamNetworking*` via rfg+0x8D13A0 (cdecl);
   - `AcceptP2PSessionWithUser`, then `SendP2PPacket(peer, data, n, 1, channel 7)`;
   - poll `IsP2PPacketAvailable`/`ReadP2PPacket` on channel 7;
   - watch for `OpenNATToHost` in networking.log;
   - also test send types 2 and 3 and the relay path.

   Pass: packets arrive with no lobby.
5. **Level_memmgr headroom** (only if anyone proposes engine MP): read the free bytes of `split_memmgr` `Level_memmgr` on
   terr01 and compare them with 0x2F00200 + 0x80000.
6. **Bandwidth soak.** Synthetic snapshots for 4 players plus NPCs, vehicles and a destruction burst, with clumsy adding
   100 ms latency and 2% loss. Target: at most ~20 KB/s up for the host.
7. **Virtual LAN.** Repeat probe 1 over ZeroTier or Tailscale between two homes.

Open decisions, not probes:
- Whether to support more than 2 players.
- Whether a self-hosted relay is acceptable to the user.
