# RfgCoop notes: prior art and lessons from other co-op mods

Domain: whether RFG/RFGR campaign co-op exists, what other fan co-op/MP mods teach us, and which network library to use.
Distilled 2026-10-07 from `research/findings/workflows/coop_prior_art.md` (the investigator report) and the skeptic
verdicts on claims C1-C8.
Build: GOG rfg.exe, 32-bit, PE timestamp 0x5B9B713D. All addresses are **RVAs** unless marked VA (VA = RVA + 0x400000).
Tags: **[V]** = someone read the page, source, imports or disassembly. **[V-snip]** = only a search-engine excerpt was
read (the page returned 403). **[I]** = reasoned, not read.

## TL;DR

- **No public campaign co-op mod for RFG/RFGR, and no public attempt at one.** The search covered the rfg-modding org,
  the local community code, Steam threads from 2015, 2018, 2019 and 2022, ModDB and YouTube. It is a bounded negative:
  private Discord experiments cannot be ruled out. All feasibility evidence has to come from our own probes. [V]
- **Keep both machines in normal SP mode (`Multiplayer` 0x02BEA588 = 0) and add our own sync layer.** The flag is read
  by 1,662 instructions in 1,058 functions, and campaign systems switch themselves off when it is 1
  (`activity_system_do_frame`, `mission_warp_and_start`). [V]
- **Engine MP destruction is one-way, from server to clients.** The server records events and clients replay them.
  There is no validation step (the validate functions are `ret` stubs) and no event rate limiter (the "throttle" caps
  debris movers). All of it is gated on `Multiplayer` and is dead in SP. What we can copy is the design. Running the
  code as-is is not an option. [V]
- **Transport: ENet over UDP to a direct IP**, reached through a port forward or a virtual LAN. GameNetworkingSockets,
  yojimbo and RakNet are rejected, mostly because they are heavy to build, not for the reasons the report first gave. [V facts / I choice]
- **Authority:** the host owns the world, missions and the save, and each player owns their own avatar (the KenshiCoop
  model). Fan projects do not agree on one ownership model for shared NPCs and vehicles: MTA uses a nearby/driver
  "syncer", Nitrox uses simulation ownership. Budget expectation: co-op caused about 37% of SR2's QA bugs. [V]

## Facts

### RFG prior art [V]
- rfg-modding org: 22 repos, none about co-op (https://github.com/orgs/rfg-modding/repositories). A grep for
  `co-?op(erative)?` over the local RSL, RSL-Docs, Reconstructor, RfgNetworking, SyncFaction, Nanoforge and RFGR_Types
  found 0 files. The symbol table has 0 matches for `co_?op|coop|split_?screen|second_player|player2|num_local`.
- The only "Red Faction Co-op" mod (svencoop.com/fansite/rfcoop, (c) 2000) is for Red Faction 1.
- Steam, 2018-07-10, user *peterthierolf* (https://steamcommunity.com/app/667720/discussions/0/1726450077696987897):
  "Lived with the code of the game for one and a half year" and "This game has fully separate setups and *a lot* of
  separate code between single and multiplayer." This is ONE unverified person; nothing confirms he worked at Kaiko.
- Sean Kennedy (Volition), Co-Optimus 2009-07-20 [V-snip], https://www.co-optimus.com/article/2218: co-op "was just not
  technically possible", because it meant duplicating the whole SP world plus destruction. That is a statement about the
  2009 game's scope. It says nothing about how SP and MP code is organised.
- The engine comes from the Saints Row family (Wikipedia [V-snip]), but rfg.exe has no co-op symbols.
- Community networking covers versus MP only. RfgNetworking's `CNetworking` P2P methods are `return false` stubs
  (`RfgNetworking-master/src/Backend/Community/CNetworking.bf:44-59`). SwApiNet's README lists netcode and encryption
  hacking as a TODO.
- Patch #4 (2018-08-12, https://www.gog.com/forum/red_faction_series/changelog/post7): "a new multiplayer library which
  makes it possible for all versions of the game (Steam, GOG etc.) to play multiplayer together", plus best-host
  selection, Nerio invites and Discord join.

### SP and MP share one codebase that branches on the flag [V]
- `Multiplayer` byte, RVA 0x02BEA588 (VA 0x2FEA588). `grep "0x2fea588\]"` on rfg.asm finds 1,662 instructions in 1,058
  functions; the list is in the skeptic's scratch `c1/funcs.txt`. Note that the xref table has no data xrefs to it.
- `activity_system_do_frame` 0x3ACB00: `cmp [Multiplayer],0 / jne exit` at 0x3ACB00, and a second check before
  `All_activities` at 0x3ACB9F.
- `mission_warp_and_start` 0x3AF5A0: jumps to its epilogue when the flag is set and skips `world::save_and_warp` (call at 0x3AF652).
- Other SP readers of the flag: `activity_system_save/load`, `amb_spawn_do_frame`, `ai_system::do_frame`,
  `gfm_change_district_morale`, `object_mission_start_node::serialize`, `player::serialize` (6 refs),
  `local_player_is_killed`.
- MP helpers behave differently with the flag at 0:
  - `multi_destruction_compress_explosion` 0x5BDC00 skips only an MP step (`je +0x26` at 0x5BDC09). It also calls
    `multi_core_network_handle_from_game_handle`.
  - `box_acts_as_server_for_destruction` 0x311A30 returns 0.
  - `multi_core_object_create_remote_object` 0x5D97E0 maps handles only when `Multiplayer==1 && Server(0x2BEA590)==1`.

### Engine MP destruction sync [V]
- **Server side.** `server_add_destruction_event` 0x314FC0 checks `Multiplayer` (VA 0x2FEA588), then `multi_is_server`
  (0x5431D0), then `!multi_game_is_migrating` (0x556F80), and then jumps to
  `multi_game_destruction_frame_server_add_destruction_event` 0x5F0620. That function calls `prepare_destruction_frame`
  and `add_destruction_event_to_destruction_frame` 0x5C4300. Its callers are `rfg_dyn_do_collide_post` (site VA
  0x740894), `maybe_add_shardify_event` and `server_add_destruction_event`. The packet is opened by
  `multi_game_destruction_frame_server_start_packet` 0x554FF0 and finished in `rfg_dyn_do_mp_collide_post_finalize`
  through `multi_game_destruction_frame_server_finalize_packet` 0x5C4400.
- **Client side.** `rfg_dyn_mp_do_collide_post_setup` 0x33AA50 sets bl=1 only when `Multiplayer==1 && !multi_is_server`
  (VA 0x73AA66-0x73AAC5). It then calls `multi_game_destruction_frame_client_get_destruction_events` 0x5C4A00, which calls
  `add_destruction_events_from_packet` 0x5C4670 (its only caller). The events are applied by `process_destruction_event_*`.
- **Clients send to the server only during host migration.** `multi_game_process_destruct_update_server` 0x57B5E0
  requires `Migration_state==3` and is reached only from `multi_game_process_migrate_df_update`.
  `multi_client_send_destruction_applied` 0x5C5A70 sends an empty reliable packet of type 0x62 and is called only from
  `process_migration_client_wait_for_destruction_applied`.
- **Event kinds (compress/decompress):** explosion 0x5BDC00, object_simple 0x5BE240, object_complex 0x5BE840, direct
  0x5BF410, link_broken 0x5BF9B0/0x5BF9D0, sp_array 0x5BF9F0, shatter 0x5BFA30, shape_cutter 0x5C0030, slice 0x5C03B0,
  repair 0x5C0840, mp_destroy 0x5C0C00, mp_veh 0x5C0C50. The `multi_game_destruction_set_*_working_data` functions call
  them (explosion at 0x5C348B, shape_cutter at 0x5C371B). link_broken (28 bytes) only converts between a game handle
  (+0x50) and a network handle (+0x30).
- **No validation.** `init_destruction_validate_sync` 0x552A50 is a 1-byte `ret` with no callers.
  `multi_game_destruction_validate_rfg_mover_initial_state` 0x5559A0,
  `multi_game_destruction_send_validate_destruction_frame` 0x57B5C0 and
  `multi_game_destruction_process_validate_destruction_frame` 0x57B5D0 are all `ret`.
- **No event rate limiter.** `mp_destruction_sync_throttle` 0x324CB0 caps synced debris movers
  (`Max_mp_throttle_active_sync_movers` 0x124F34C, `Max_mp_throttle_inactive_sync_movers` 0x124F350, plus the
  Too_small_shard length/volume/mass limits). It fades the rest with `object_mover_mark_for_fade`. Its only caller is
  `rfgl_do_frame` at VA 0x72511E, behind `cmp [Multiplayer],0` at VA 0x72510D.
- `MP_destruction_frame_pool` 0x2BEA564 is written only by `multi_game_init`, `multi_game_destruction_init`,
  `level_init_pre` and the shutdown functions. With the flag at 0, none of this path runs.
- Other MP object machinery: master/remote objects with delta compression keyed by ack IDs
  (`multi_core_dc_create_master_object` 0x5D7F50, `multi_core_dc_store_state_for_object` 0x547250,
  `multi_core_object_apply_compressed_state` 0x5DA050, `multi_core_get_object_updates_for_connection` 0x5FB1B0), and host
  migration (`Multi_game_has_migrated` 0x2BEA6C8, `multi_core_migrate_fixup_network_handles` 0x5D9700).

### Engine MP transport path [V]
- `multi_connection_flush_current_packet_no_sync` 0x58EEC0 (call at 0x58F20C) and `multi_core_platform_send_packet`
  (a tail jump) both lead to `multi_core_platform_internal_send_packet` 0x5420D0. That function:
  1. locks `g_gameLinkMutex` (VA 0x2FEA77C);
  2. calls `unit4::RemoteGameSession::getGameLinkSession` 0x810FA0, which returns `[this+0x11FF0]`;
  3. makes two virtual calls: vtable +0x84 `getRemoteStationById`, then +0x5C `sendP2PPacketUnreliable`.
- `GameLinkInternet`, vftable VA 0xF44B04: +0x5C = 0x8D4530, +0x84 = 0x8D18A0. 0x8D4530 calls the sw_api import
  `SW_CCSys_DynamicInit` and then a Steam-shaped interface at ctx+0x20 (`ISteamNetworking::SendP2PPacket` style, using CSteamID).
- `GameLinkLan`, vftable VA 0xF44C74: +0x5C = 0x8D33B0, +0x84 = 0x8D3540. It sends raw UDP through
  `keen::network::sendDataTo`, which calls WS2_32 ordinal 20 (`sendto`). Related functions: `sendBroadcastPacket`
  0x8D07D0, `sendPacketToHost` 0x8D0860, `startHostMigration` 0x8D5550, `getNextHost` 0x8D3700.
- `keen::network::ErrorSimulation::sendTo` 0x882AF0 is a built-in lag/loss simulator.
- sw_api.dll is THQ Nordic Online's Steam-API emulation layer. llvm-strings shows `UseGOGNetworking`,
  `UseTHQNONetworking`, `UseGOGMatchmaking`, `SetTHQNOState`, an embedded RakNet (`RakNetSocket2.cpp`,
  `UDPProxyClient.cpp`), and imports of WS2_32 and `GalaxyFactory::{CreateInstance,GetInstance,ResetInstance}`. The
  backend is chosen by the encrypted `thqnocfg_gog.dat` (528 bytes). Galaxy.dll is version 1.114.9.0 in both installs.

### Process and build facts [V]
- **No single-instance lock.** rfg.exe neither imports nor resolves by string `CreateMutex*`, `OpenMutex*`,
  `FindWindow*` or `CreateFileMapping*`. `keen::Event::create` 0x806FC0 and `keen::Semaphore::create` 0x807130 name
  their objects with `"%s@%x:%x"` (object pointer + `GetCurrentProcessId`), so the names are unique per process. The
  other CreateEventW callers pass NULL or a stack name. All 5 `GetLastError==0xB7` checks follow `CreateDirectoryA/W`.
  sw_api, Galaxy and GalaxyPeer import no mutex or FindWindow functions. The two local rfg.exe copies are byte-identical.
- rfg.exe already imports `WS2_32.dll` and `WINMM.dll`, so ENet adds no new system DLL.
- GOG rfg.exe sha256 is `7a82d2d0f425af5e75d8ffbce12fac53eb5ca9cd812731ccf5a29697e906af0e`. RSL's Steam offsets do
  not match it. Example: RSL reads InMultiplayer at
  +0x2CA210 (`RSL-master/RSL/Bootstrapper.cpp:436`), but on GOG that RVA is code among the `rfg_camera` methods, and the
  GOG flag is at 0x2BEA588. RSL already uses signature scanning (`Globals::FindPattern`, `Bootstrapper.cpp:76-77`).
  RSL-Docs `Installation.rst:9`: GOG support is "planned for the future".

### Other projects [V unless tagged]
| Project | Transport | Authority | NPCs | Missions / progress | Saves |
|---|---|---|---|---|---|
| Saints Row 2 (official) | GameSpy, later Steam | [I] host | [I] host | whole campaign, untethered | per player |
| Saints Row 2022 (official) | platform | host | host | "if one player enters a mission, the other warps to them"; progress **per player**, carried back into each SP save | per player |
| Elden Ring Seamless Co-op | Steam lobbies + native netcode [V-snip] | native host world | native | shared; world resets on grace rest | separate co-op save ([I] `.co2`) |
| Dark Souls 3 Seamless Co-op (LukeYui) | native netcode [V-snip] | native | native | whole campaign | separate save extension |
| KenshiCoop | ENet/UDP + optional Steam P2P tunnel | **player-host owns the world; each client owns its squad** | replicated from host | n/a (sandbox) | one shared save streamed to both |
| Skyrim Together Reborn | GameNetworkingSockets, standalone server exe | server assigns per-actor ownership (`RequestOwnership`, `OnOwnershipTransfer`, `DeclineOwnership(serverId, epoch)`) | owner client simulates | party leader advances quests; every member's quests advance too [V-snip]; main+side sync, misc does not | each player's own |
| MTA:SA | RakNet | server + syncer: nearby player within 100 u (peds) / 140 u (vehicles); last occupant syncs a vehicle | syncer | scripted | server |
| SA-MP / open.mp | RakNet | client sends own state every 30 ms (`onfoot_rate`/`incar_rate`); server relays; `stream_distance` 200 | none (SP AI removed) | none | server |
| JC2-MP | undisclosed | server | AI and story removed [V-snip] | none | server |
| Nitrox | LiteNetLib UDP [V-snip] | server + simulation ownership [V-snip] | owner simulates | partial | server |

- KenshiCoop README (https://github.com/nhoral/KenshiCoop): "drives all game mutation on the main thread". It is the only
  project for which main-thread-only mutation is verified.
- Skyrim Together: `QuestService.h` says "This service is currently not in use", and dropped items are not synced
  (wiki.tiltedphoques.com llms-full.txt).
- Saints Row 2: about 37% of QA bugs were co-op related, and "a lot of pain and suffering" (Bowring GDC 2010, GameSpot
  report [V-snip]; https://www.gdcvault.com/play/1012213). James Tsai, Co-Optimus 2008-09-22 [V-snip]: the SR1 engine
  "was not co-op friendly" and needed a lot of "adding support and rewriting stuff".
- MattG (Volition), 2013-06-27, https://www.saintsrowmods.com/forum/threads/more-than-2-player-co-op.2697/, explaining
  why SR has no **split-screen**: untethered roaming would make one machine load, simulate and stream two city areas.

### Network libraries [V]
- **ENet** (https://github.com/lsalzman/enet): MIT, plain C. Its CMake does `target_link_libraries(enet winmm ws2_32)`
  on WIN32. It offers reliable, unreliable, sequenced and unsequenced sends, with no encryption.
  `enet_initialize` calls `timeBeginPeriod(1)` (win32.c:28), which sets the timer resolution for the whole process.
- **GameNetworkingSockets:** C++17, protobuf (version 22+ pulls in Abseil), and ONE crypto backend (OpenSSL 1.1.1+,
  libsodium or bcrypt). A signaling service is needed only for the optional P2P/WebRTC mode (`USE_STEAMWEBRTC`).
  `examples/example_chat.cpp` connects directly with `ConnectByIPAddress`. BUILDING.md covers only Visual Studio and
  vcpkg on Windows and has no MinGW instructions.
- **yojimbo:** client/server for dedicated servers, BSD-3, needs libsodium and netcode.
- **RakNet** (facebookarchive) was archived 2022-01-13.
- **GOG Galaxy INetworking** docs (as read by the investigator, not re-checked): P2P only with users in the same lobby,
  a `CONNECTION_TYPE_PROXY` relay, and a recommended maximum of 1200 bytes per send.
- **Virtual LANs:** Tailscale has no broadcast (staff, 2020). ZeroTier is layer 2 with broadcast [V-snip]. Radmin VPN
  is free with no player limit [V-snip]. Broadcast matters only for in-game LAN discovery; a mod that dials an IP works on all of them.

## Approach for co-op

- **Mode:** both machines stay in SP freeroam with `Multiplayer` = 0. Never set the flag globally. [V reason / I plan]
- **Transport:** vendor ENet, build it with llvm-mingw i686, and use direct IP:port. Players connect through a port
  forward or a virtual LAN. Do not write NAT code in v1. If the DLL is the `winmm.dll` proxy, ENet's winmm imports
  resolve to the proxy itself, which must forward `timeGetTime`/`timeBeginPeriod`. The simpler fix is to put ENet in a
  dinput8-proxy DLL or a separate DLL. Thread rules and channel layout are in `transport.md`. [I]
- **Authority** (KenshiCoop model, which suits 2 players):
  - The host owns missions, activities, AI, zone/persistent state, the "building destroyed" outcome and saving.
  - Each player owns their avatar's transform, animation, aim, fire events and health.
  - A vehicle is owned by its driver (MTA last-occupant rule), and goes back to the host when empty. This is a design
    choice with precedents, not a consensus. [I]
- **Destruction:** host-authoritative event replay, as in the engine design. The host simulates the destruction and
  broadcasts compact events. The guest's hits go to the host as requests ("I fired / I hit X"), the host applies them and
  then broadcasts the result. Replay on the guest by calling `process_destruction_event_*` or the original cause (an
  explosion), once a probe proves these work in SP. Reconcile the persistent outcome from the host. [I]
- **Missions:** only the host's mission state counts. Partners are pulled in, either warped on mission start (SR 2022)
  or synced the way Skyrim Together syncs the leader's quests. The guest should not lose credit: both precedents give
  partners progress. How the guest gets credit (mirror the host's state, or a separate co-op save) is an open design
  choice. Never write the guest's own SP save mid-session. [I]
- **Remote player:** a puppet NPC with its AI cleared (the Skyrim Together / SA-MP pattern). A second real `player`
  object is an experiment only, because `Player` is a singleton. [I]
- **Distance:** a soft tether (sync radius plus a warp on mission start) avoids most ownership hand-offs. Each machine
  streams around its own player, so the SR "two areas" memory cost does not apply directly. [I]
- **Builds:** v1 is GOG-only. That is a scope choice. Later support for Steam players means resolving addresses on each
  player's build (a second RVA table or signature scanning), with a protocol and content check at handshake. [I]

## Contested / refuted (do not rely on these)

- **"Insiders say SP and MP are separate code."** The source is one unverified Steam user. The binary shows ONE codebase
  that branches on a flag. The "don't use MP mode" conclusion stands, but for the binary reason, not the quote.
- **The Volition 2009 quote as proof of separate code.** It is a statement about project scope only.
- **"Engine MP replays destruction both ways with a rate limiter and validation."** Wrong on all three counts: it is one
  way (server to client), the "throttle" caps debris, and validation is compiled out. `multi_client_send_destruction_applied`
  is a migration ack, not part of the normal sync.
- **Reusing the native destruction/object sync path unchanged.** It is gated on `Multiplayer`/`multi_is_server`, and its
  frame pool is created only by MP init. Only individual helpers might be reusable, each after checking its behaviour
  with the flag at 0.
- **"Every fan co-op mod uses host-controls-world + proximity/driver ownership."** This is overgeneralized from about
  six projects. Most of them use a separate server, and proximity/driver ownership is documented only for MTA. MTA,
  SA-MP, JC2-MP and MafiaHub are freeroam frameworks that drop the campaign. Native-netcode co-op is a family (Seamless
  Co-op for both Elden Ring and DS3), not a single exception.
- **"Only the host progresses" (SR / Skyrim Together).** False. SR 2022 tracks progress per player, and in Skyrim
  Together the leader advances quests for everyone. The warp in SR 2022 pulls players to whoever starts the mission,
  not to the host.
- **"Roaming apart doubles streamed areas" for online co-op.** MattG's quote is about split-screen on one machine.
- **The SR2 "massive engine rewrites" wording.** The source says "adding support and rewriting stuff", and the n4g page
  cited for it has no such quote.
- **Rejecting GNS for "OpenSSL + signaling server".** Both reasons are wrong for direct IP: bcrypt works on Windows, and
  signaling is needed only for P2P. GNS is rejected for its C++/protobuf/Abseil weight and its lack of a MinGW build path.
- **"GOG MP = sw_api over Galaxy P2P", and "a tunnel must use our own Galaxy lobby and the 1.114 vtables".** sw_api may
  use THQNO's RakNet instead; the choice sits in the encrypted config. A mod could instead call the game's own gamelink
  vtable or sw_api's Steam-shaped interface, or skip both and use its own UDP.
- **"Both players need the identical GOG build".** Addresses are resolved per process. What has to match across players
  is each player's supported build, the wire protocol and the game data.
- **The Skyrim Together party-leader rule cited from xgamingserver, and the GNS "MinGW/MSYS2" build claim.** Neither
  cited page contains the text.

## Unknowns and the probes that settle them

| Unknown | Probe |
|---|---|
| Do two instances run on one PC (Galaxy/sw_api double sign-in, save/log file share modes)? | **P1:** start the GOG exe twice with a test DLL and a read-only save copy; pass = both reach freeroam. |
| Is a puppet NPC viable (animation blending vs sliding, collision, ragdoll, whether EDF target it)? | **P2:** with no network, drive an allied, AI-cleared human from a 200 ms-delayed trace of the player. |
| ENet in-process: RTT, frame cost, queue thread safety, the winmm proxy forwarding issue | **P3:** loopback, two instances on UDP 7777, 30 Hz transforms. **P7:** the same between two PCs over a VLAN (pass: RTT < 100 ms, loss < 1%). |
| How far does destruction diverge under event replay? | **P4:** replay a logged explosion on B, then after 10 s compare `world::object_was_destroyed` 0x52CE90 on the same building. |
| Do `multi_destruction_compress_*` / `process_destruction_event_*` work with the flag at 0, without MP handle tables or `Active_destruction_frame_data`? | **P5:** call `multi_destruction_compress_explosion` 0x5BDC00 into a scratch buffer in SP; pass = no crash and deterministic bytes. |
| Does the client suppress its own local destruction simulation in MP? | Static: read `rfg_dyn_mp_do_collide_post_setup` 0x33AA50 and its callers past the bl flag. |
| Which sw_api backend runs on GOG (Galaxy vs THQNO RakNet), and is Galaxy initialised? | **P6:** log `GalaxyFactory::GetInstance()` after boot. Hook sw_api config reads to see `UseGOGNetworking` / `UseTHQNONetworking`. |
| What exactly breaks when `Multiplayer`=1 in SP? (Statically it is known that activities and mission warps stop.) | **P8**, optional and sacrificial: set the flag to 1 for one second in freeroam, restore it, and log what changes. |
| Can a second `player` object exist in the SP world (Wrecking Crew swaps `Player`)? | A separate experiment after P2. |
| Do Steam and GOG builds have the same struct layouts? | Needs a Steam exe; none is installed. |
