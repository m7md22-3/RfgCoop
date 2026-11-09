# RfgCoop notes: community code reuse

Distilled from `research/findings/workflows/coop_community.md` plus skeptic verdicts (claims C1-C8), 2026-10-07.
**[V]** = verified by reading code, disassembly or data, or by running a check. **[I]** = inferred.
RVAs are for the GOG build (image base 0x400000; VA = RVA + 0x400000). Community repos live in `..\..\` (`<RFG Modding>\`) and are read-only.
Related notes: `transport.md`, `replication.md`, `destruction.md`, `prior_art.md`.

## TL;DR

* **No community project ships a working co-op transport.** RfgNetworking's own backend is all stubs, SwApiNet only logs calls, and nobody has emulated THQNO. There are three possible routes: the engine's own Winsock LAN gamelink (`kaiko::gamelink::GameLinkLan`), a transport bundled in the mod (RakNet-master is on disk and passes a syntax check with llvm-mingw), or sw_api P2P through THQNO.
* **One offset table is enough.** The user's `rfg.exe` (sha256 `7a82d2d0…6af0e`) is the stock GOG exe, and the Terraform patch (applied by SyncFaction) gives every community MP player that same exe. Resolve everything by PDB name. Never take numbers from RSL (Steam offsets, and the deltas vary).
* **The save format is known and the CRC is plain CRC-32, but the save only bootstraps the world.** The game reads the `.sav` into RAM once, at the menu. It loads slots from that RAM buffer, and every in-game save rewrites the whole file. Inject into RAM instead of writing the file. The save holds persistent progress, not live NPCs, vehicles or a mission in progress.
* **Zone-file handles are stable IDs for intact level objects only.** Destruction creates shards with local, order-dependent handles, freed handles get reused, and the engine's MP reserved-handle scheme is MP-only and capped at 1000. Co-op needs its own host-authoritative mapping.
* **Check data parity at session start.** The user's two installs differ (the GOG GALAXY copy has the community-patched `sw_api.dll`). The Unification patch changes `terr01_l1` content while leaving the exe the same. Reconstructor changes memory pools and the inventory table. Peers must hash their files and run the same patch set.

## Facts

### Build identity and data parity
* Both installs' `rfg.exe` hash to `7a82d2d0f425af5e75d8ffbce12fac53eb5ca9cd812731ccf5a29697e906af0e`, which is the stock GOG hash (`SyncFaction-master/src/SyncFaction.Core/Hashes.cs:137`). The Steam exe is `0d52039e7f2d…a2df4` (`Hashes.cs:120`). [V]
* SyncFaction's docs say "the game will be converted to GOG version" (`SyncFaction-master/docs/usage.md:23`). The conversion itself is done by the Terraform patch, which SyncFaction applies. Its `.steam/rfg.xdelta` patches only the Steam exe (`docs/modding/version_specific.md:11-13,28`). The `mods/Steam Re-enabler` mod exists to revert that patch. The patched set in `docs/modding/verification.md:37-50` still lists rfg.exe as `7A82…`. [V]
* Reconstructor patches the game in memory, so the running code of a community-patched peer can differ from the exe on disk. [I]
* `sw_api.dll` differs between the user's installs [V]:
  * `<game copy B>` has the stock GOG file, `3d5b4130…`.
  * `<GOG Galaxy folder>` (the copy the user plays) has `67f9f8e9…`, modified 2026-09-26. That is the Unification/Terraform-patched sw_api.dll from `verification.md:54`.
  * Neither install has a `data/.syncfaction` folder (`Constants.cs:10` AppDirName `.syncfaction`).
* `terr01_l1.vpp_pc` is 4,448,220,211 bytes, dated 2023-11-27 10:24:37, in both installs. It was not hashed, so whether the Unification terr01 changes are present is unknown. [V size/date]
* Steam and GOG list different hashes for `terr01_precache`, `vehicles_r`, `terr01_l1`, `effects`, `effects_mp` and `chunks` (`Hashes.cs:118-131` vs `:133-147`). Steam and GOG also differ in rfg.exe, though. The real "same exe, different data" case is the Unification patch. [V]
* What the Unification patch changes (`verification.md:57-64`) [V]:
  * It replaces `sw_api.dll`, plus `0h_c1218.str2_pc`, `0h_c1618.str2_pc`, `0h_c1219.str2_pc` and `terr01_l1.asm_pc` inside `terr01_l1`.
  * The patched `terr01_l1` hash is `12CC145B…`. Stock GOG is `a89bfd54…` and Steam is `92dc6908…`.
* What the three `.str2_pc` containers hold (`src/tests/SyncFactionTests/hashes.json:62719-62740`) [V]:
  * `0h_c1218` and `0h_c1618`: mostly terrain mesh, ore-mineral chunks and howitzer effects.
  * `0h_c1219`: EDF building chunks (`edfwall_conc_a`, `edfpillbox_a`, `edfcheckpoint_a`, `edfgas_station_a`).
* `Hashes.cs:15-158` is a ready-made table of stock hashes (common, Steam-only, GOG-only and MP sets). It contains facts, not code, so it can be reused. [V/I]

### Reconstructor (MPL-2.0, MSVC/CMake, development on hold)
* Its offsets are GOG RVAs added to the module base (`Common/common/patching/FunHook.h:30`). The report checked 8 functions, 5 globals and 10 patch sites against the symbol table. All matched, for example `gameseq_get_state` 0x3BFC70, `human_teleport_unsafe` 0x67C290 and `player_do_frame` 0x6E6290. [V]
* It has no networking code. A grep for socket, winsock, enet, raknet, send(, recv( and ws2_32 found nothing. [V]
* MP-relevant patches (`hooks/MpHooks.h`, `hooks/MiscHooks.h`, `patching/Patches.cpp`, installed at `Reconstructor.cpp:92-101`) [V]:
  * `multi_game_process_remote_console_command` 0x54E7C0 is hooked to a no-op, which turns off the rcon handler.
  * `multi_game_memory_alloc` 0x54F320 gets two push patches: 0x54F359 raises the MP effect preload pool from 0x580000 to 11534336, and 0x54F3A2 raises the MP item preload pool from 0x4CFEC00 to 161470464.
  * The match countdown in `multi_game_mode_match_init` (imm32 of the mov at 0x5AAD87) drops from 10000 ms to 5000 ms.
  * `ApplyMultiplayerUIPatches` zeroes 0x12624B0 and 0x12624F0, which disables the matchmaking and spectator menu items. It also writes `GS_MULTIPLAYER_LIVE_FIND_SERVERS` at 0x4AD61D.
  * A `gameseq_push_state` hook at 0x3D87E0 drops `GS_MULTIPLAYER_SEARCH_MATCHMAKING` and `GS_MULTIPLAYER_CHANGE_MATCHMAKING`.
* General patches [V]:
  * `Inv_item_info` (0x2C6D550, 2560 bytes) moves to a 127-entry heap array. Code is patched at 0x61203A, 0x612013, 0x612251, 0x612381, 0x612399, 0x6C2FFB and 0x6C3050.
  * 37 push-pair patches cover 34 string pools: 32 doubled, `Riding_shotgun` quadrupled (512 to 2048), `Activity_data_name` left at 2560.
  * The Common_page, Permanent and Item_preload mempools are doubled.
  * Saving with cheats on is allowed (0x3EADED).
* NagDLL (`NagDLL/dinput8.c`) is a **dinput8.dll proxy**. It shows a message box and exits unless the command line contains `/RanWithReconstructor`. It clashes with any other dinput8 proxy. The Reconstructor plugin itself is injected by `Launcher.exe` / `Host.dll` (CreateRemoteThread LoadLibraryA). [V]
* The Host plugin ABI passes `std::vector`/`std::string` across DLLs, so it needs MSVC. An llvm-mingw plugin cannot safely load into it. [V code, I ABI clash]

### RSL (GPL-2.0, Steam offsets): names only
* The Steam offsets do not map to GOG by a constant delta [V]:

  | Function | RSL (Steam) | GOG | Delta |
  |---|---|---|---|
  | `mission_get_name` | 0x39DA30 | 0x39D9C0 | -0x70 |
  | `object_spawn_vehicle` | 0x757F40 | 0x757B50 | -0x3F0 |
  | `cutscene_spawn_vehicle` | 0x305D70 | 0x305CD0 | -0xA0 |

* Puppet primitives checked in the GOG PDB [V]:

  | Function | GOG RVA | Signature | Other overload |
  |---|---|---|---|
  | `havok_body_set_pos_orient` | 0x423790 | `void __cdecl(unsigned havok_handle, vector*, matrix*)` | `hkpRigidBody*` at 0x416EC0 |
  | `havok_body_get_pos_orient` | 0x4295A0 | | 0x4236A0 |
  | `havok_body_set_linear_velocity` | 0x423870 | `(unsigned, vector*)` | 0x417080 |
  | `havok_body_get_linear_velocity` | 0x423830 | | 0x417050 |
  | `havok_body_set_angular_velocity` | 0x4239A0 | | 0x4174E0 |
  | `havok_body_set_movable` | 0x423E30 | `(unsigned, bool)` | 0x423DD0 |
  | `havok_body_force_activate` | 0x4177E0 | | |
  | `vehicle_interaction_request_enter` | 0x7B7E10 | `bool(human*, vehicle*, vi_enter_data*)` | |
  | `vehicle_interaction_request_exit` | 0x7B5C80 | | |
  | `human_teleport` | 0x69A3C0 | `void(human*, vector*, matrix*, float, bool)` | |
  | `human_teleport_unsafe` | 0x67C290 | | |

  Notes:
  * `vehicle_interaction_request_enter` is a 5-byte `jmp` to `vehicle_interaction_request_enter_internal`.
  * RSL itself binds `human_teleport_unsafe`; its `human_teleport` binding is commented out.
  * The `unsigned` overloads take `object+0x4C` (`havok_handle`).
* Lua [V]:
  * RSL runs mod scripts in its own bundled LuaJIT 2.0.5 through sol2.
  * The game embeds **Lua 5.0**: the string "Lua 5.0", real functions `lua_open` 0x821690 and `lua_dobuffer` 0x82F9C0, and no `luaL_newstate` or `lua_getfield`.
  * The only Lua state comes from `lua_open`, called only by `lua_create_state`, called only by `vint_lua_init`. So the game's Lua drives only the Vint UI. Campaign missions are not Lua.
* RSL's Lua API has no networking, mission-control or save functions (`RSL/ApiTables.cpp`). `RSL/IpcManager.cpp` is a toy named pipe that teleports the player. [V]

### RfgNetworking (MIT, Beef; the Beef toolchain is not installed) and SwApiNet (no license)
* RfgNetworking's standalone `CommunityBackend` is all stubs [V]:
  * `CNetworking.SendP2PPacket`, `ReadP2PPacket` and the socket methods return false, 0 or NotConnected (`src/Backend/Community/CNetworking.bf:43-174`).
  * `SW_CCSys_InitCallbackFunc` treats its listed callback IDs as no-ops and calls `Runtime.FatalError` on any other ID (`CommunityBackend.bf:95-123`).
* Its default build is a logging pass-through: `src/Program.bf:9` `#define UseDebugBackend`, and `DebugBackend.bf:76` calls `LoadLibraryA("sw_api.original.dll")`. The repo has 33 files and no master server, even though `README.md` says it has one. [V]
* It documents the sw_api ABI the game links against [V]:
  * Exports (`src/Program.bf`) include `SW_CCSys_Init`, `SW_CCSys_DynamicInit(CallbackCounterAndContext*)`, `SW_CCSys_InitCallbackFunc`, `SW_CCSys_ProcessApiCb` (called every frame) and `SW_CCSys_Register/UnregisterCallResult`.
  * `CallbackCounterAndContext` = `{ void(*InitCallback)(CSteamAPIContext*); i32 Counter; CSteamAPIContext* Context; }`.
  * `CSteamAPIContext` holds 21 interface pointers, which matches the PDB (size 0x54).
  * The PDB already carries the full `ISteamNetworking` method list, so RfgNetworking's vtables are not needed.
* SwApiNet [V]:
  * It is a .NET 8 / DNNE logging wrapper. `Exports.cs:69` wraps calls as `DllLogWrapper(DllInterceptWrapper)`; the intercept layer hijacks vtables but forwards everything to `sw_api_original.dll`.
  * Its README TODO still says "Actually figure out how to hack netcode…".
  * `src/sw_api_spartacus_demo/` is a usable MSVC C proxy template.
* A grep for SendP2PPacket/ReadP2PPacket across the other repos finds only RakNet's generic Steam Lobby2 code. [V]

### Engine LAN transport (counter-evidence from C1)
* `kaiko::gamelink::GameLinkLan` is a Winsock UDP path that never touches sw_api or THQNO [V]:
  * Functions: ctor 0x8D5320, `create` 0x8D6A40, `sendPacketTo` 0x8D0820, `sendP2PPacketUnreliable` 0x8D33B0, `sendBroadcastPacket` 0x8D07D0, `getNextP2PPacket` 0x8D6D80, `handlePacket` 0x8D6E50, `update` 0x8D83A0, `startHostMigration` 0x8D5550.
  * `sendPacketTo` calls `keen::network::sendDataTo` (0x884DD0). That calls `[0x123F68C]`, the first WS2_32 IAT slot (IAT RVA 0xE3F68C, ordinal 20 = `sendto`).
  * Other `keen::network` functions: `createSocket` 0x8848E0, `bind` 0x8849C0, `createNetworkSystem` 0x887050.

### RakNet-master (BSD-2 plus patent grant)
* Version 4.081, dated 2014-05-28 (`Source/RakNetVersion.h:11-15`). The license is 2-clause BSD (Oculus VR 2014), and `PATENTS` adds a patent grant. [V]
* sw_api.dll embeds RakNet 4.x [V]:
  * RTTI includes `RakPeer`, `RakNetSocket2`, `RNS2_Windows`, `RNS2_Berkley`, `NatPunchthroughClient` and `UDPProxyClient`.
  * The build path is `c:\work\thqonline_clean\…\steamwrapper\3rdparty\raknet\source\`.
  * It has no version string, so the exact revision is unknown.
* `i686-w64-mingw32-clang++ -fsyntax-only -w -I Source` passes on `RakPeer.cpp`, `ReliabilityLayer.cpp`, `NatPunchthroughClient.cpp` and `RakNetSocket2.cpp`. [V]
* The shipped CMake applies MSVC flags under `WIN32` (`Lib/LibStatic/CMakeLists.txt:11-13,20-23`). A full build and link with `-lws2_32` is untested. [V]
* Useful modules: `ReliabilityLayer`, `NatPunchthrough*`, `UDPProxy*`, `FullyConnectedMesh2`, `ReplicaManager3`. `DependentExtensions/` (old openssl and others) is not needed. [V file list]

### Save format (RFGM.Formats, MIT) and the game's save path
* The file is `%LOCALAPPDATA%\GOG.com\Galaxy\Applications\51153410217180642\Storage\Shared\Files\autocloud\save\keen_savegame_0_0.sav`, a fixed 31,479,964 bytes (`SaveFile.cs:8`). [V]
* Layout [V]:
  * Header: 392 bytes. `u32 version 0x01000000`, `u32 crc`, `char[256]`, 128 bytes padding.
  * 12 slots, stride 2,622,676 bytes (0x2804D4). Each slot is: `u32 exists`, a 188-byte `game_save_info` (PDB 0xBC: territory_index +0xC, dlc_id +0x30), `i32 SizeUsed`, a world buffer of at most 2,621,456 bytes with magic `RFGS`, then 1,024 bytes of padding.
  * Options and profile data follow the slots.
* The checksum is standard CRC-32 (zlib) over the whole file with bytes 4..7 zeroed (`Hash.cs:72-82`). On the user's file this gives `0x440292d7`, equal to the stored value. [V]
* The user's slots [V]:

  | Slot | Content | SizeUsed |
  |---|---|---|
  | 0 | terr01, 18 missions, 97 activities, 5 districts, start zone `terr01_05_04` | 1,316,864 |
  | 1 | terr01 | 663,552 |
  | 2 | terr01 | 1,314,816 |
  | 3 | dlc01 | 153,088 |

  Slots 4-11 are empty.
* World sections (`SaveWorld.cs:48-126`) are mostly typed [V]:
  * Destroyed handles, mission flags, activities, restorable/restored objects, per-zone stored state buffers, districts, player position, inventory, and more.
  * **Not fully mapped:** `ScriptSystemState` (`:24`) and `TrackedStatisticsState` (`:38`) are raw TODO blobs, `ActivitiesData.cs:7` reads 16 of 17 entries, and `PlayerStatData.cs:5,45` and `ScriptData.cs:3` are unimplemented. The raw blobs still round-trip byte for byte.
* Runtime behaviour (disassembly) [V]:
  * `game_save_process` 0x3C3690 fills `Save_system_save_data` (0x021328E4) once, while that pointer is null, through `unit4::LocalGameSession::getSaveData` 0x8111E0 and `keen::SaveDataHandler::getSaveGameData`.
  * The pointer is cleared or refetched only from `main_menu_start_auto_catch` and `main_menu_process`.
  * `game_save_load_thread` 0x3EFF10 (called by `game_save_load` 0x3EFF90) memcpys from `[Save_system_save_data] + slot*0x2804D4`: `+0x248` size, `+0x24C` world. It never touches disk.
  * `game_save_write_thread` 0x3D0E40 writes `+0x188` exists, `+0x18C` the 188-byte info, `+0x248` size and `+0x24C` world. It then zeroes `+4`, calls `keen::getCrc32Value(buf, 0x1E0589C)`, stores the result at `+4` and calls `LocalGameSession::startSave`. So the whole RAM image goes back to disk on every save.
  * None of `game_save_load`, `game_save_load_thread` or `game_save_process` checks `Multiplayer`.
  * `game_save_import_save_slot` 0x3E90D0 exists but was not traced.

### Zone handles
* Zone file format [V]:
  * Signature 0x454E4F5A ("ZONE"), version 36 (`RfgZoneFile.cs:8-9`).
  * Each object has a 56-byte header: class hash, handle, 24-byte bbox, u16 flags @+0x20, u16 block size @+0x22, parent @+0x24, sibling, child, num, u16 prop count @+0x34, u16 prop bytes @+0x36 (`RfgZoneObject.cs:38-50`).
  * Nanoforge pairs objects with their `p_` twins by (Handle, Num) (`ZoneImporter.cs:110-126`).
* How the engine loads them [V]:
  * `world::serialize_zone` 0x53ECE0 walks the same records (`add esi,0x38`).
  * It calls `alloc_object_stub_preexisting(class=[esi], handle=[esi+4], parent=[esi+0x24])` at 0x53F35C and 0x53F591.
  * `world::alloc_obj_handle_preexisting` 0x53B210 stores the handle verbatim (`mov [edi+0x6c], esi` at 0x53B255) and inserts it into the World+0x480 hash. The top byte of the handle selects the zone slot.
  * This happens in single player too. The MP branch at 0x53F536 only filters records by gameplay mode.
* Records that are skipped, so the live set depends on the save [V]:
  * Handles already present in the hash (0x53F305).
  * Records with `flags & 8` (0x53F343 / 0x53F578).
  * Records rejected by `should_restore_object_if_not_in_safe_file` or `…_if_resetting_destruction` (0x53F50C / 0x53F529).
* Spawned objects get local handles [V]:
  * `world::alloc_object_stub` 0x53B2E0 (from `create_object_internal` 0x53E654) calls `handle_manager<7280>::allocate_handle` 0x51F160.
  * That function scans a per-zone bitmap from a rotating cursor (+0x390/+0x394) and ORs in the zone bits from +0x398.
* Destruction creates new movers [V]:
  * `rfg_dyn_shardify_work` (0x33D2C8, an `rfg_mover`)
  * `rfg_dyn_create_shard` (0x337E9D, a `general_mover`)
  * `rfgm_create_from_chunk` 0x65F625
* Freed handles are reused [V]:
  * `world::free_obj_handle` 0x53B270 clears the bitmap bit (0x53B2D3) unless `obj+0x55 & 0x40` or `obj+0x56 & 1` is set.
  * `serialize_zone` reserves file handles in the bitmap only for records with byte +6 == 0xFF (0x53F2DF). It sets `+0x55 |= 0x40` otherwise (0x53F5A4).
* The engine's MP reserved scheme [V]:
  * `gamestate_gp_level_init_post` calls `multi_game_setup_reserved_remote_objects` 0x5DBAF0 only when `Multiplayer != 0` (0x3EFDD5).
  * `add_reserved_handles<rfg_mover>` 0x5D7110 snapshots at most 1000 (0x3E8) mover handles, qsorts them, and gives sequential network IDs through `multi_core_add_reserved_network_handle` 0x5B5C40.
  * `add_rfg_mover_to_created_list` 0x312600 returns early unless `Multiplayer` is set.
* Object fields that matter for co-op (the PDB, and they match `RFGR_Types Object.h`; `sizeof(object)` = 0x9C) [V]:
  * `+0x04` pos, `+0x10` orient, `+0x4C` havok_handle, `+0x54` object_flags
  * `+0x58` remote_object_flags: bit0 remote, bit1 client_sync, bit2 client_destroy_permission, bit3 no_destroy_packet, bit4 common_create
  * `+0x59` mp_dcmo_index, `+0x6C` handle, `+0x70` parent, `+0x7E` obj_type, `+0x7F` sub_type, `+0x98` srid

### Other repos, licenses, DLL slots
* RFGR_Types (no license) [V]:
  * Its size asserts match the PDB: `object` 156, `world` 11192 (0x2BB8).
  * It has no vehicle header, and its player/human/npc headers are unchecked.
  * It adds nothing over the PDB.
* SyncFaction (no license) [V]:
  * Mods go in `<game>/data/.syncfaction/<mod>` and may ship DLLs to the game root (`docs/modding/intro.md:11-33`).
  * It supports `.xdelta`, `ModInfo.xml` and `.mod.hashes.json`.
  * It cannot delete files.
  * The community MP baseline is Terraform + Reconstructor (`docs/usage.md:32`).
  * The Steam Re-enabler ships a neutered NagDLL dinput8.
* `rfg.exe` imports DINPUT8, WINMM, XINPUT1_3, discord-rpc, sw_api, WS2_32 and others (`llvm-readobj --coff-imports`). [V]
  * Taken slots: `dinput8.dll` (used by other proxy-DLL mods, the Reconstructor NagDLL and the RSL loader) and `winmm.dll` (other proxy-DLL mods).
* Licenses [V]:

  | License | Repos |
  |---|---|
  | MIT | RFGM.Formats, RFGR.SaveEditor, RfgNetworking, Nanoforge, RfgTools, RfgToolsPlusPlus |
  | BSD | RakNet, subhook |
  | MPL-2.0 (file-level copyleft) | Reconstructor |
  | GPL-2.0 | RSL |
  | None (reference only) | RFGR_Types, SyncFaction, SwApiNet |

## Approach for co-op

1. **Source of truth: `rfg.pdb`.** Use the PDB and the symbol table. Reconstructor and RFGR_Types are for cross-checks only. Never use RSL numbers, and do not copy GPL or unlicensed code.
2. **Infrastructure: an llvm-mingw hook and ImGui stack.** Load `RfgCoop.dll` from a winmm proxy or through an `XINPUT1_3.dll` proxy. Avoid dinput8 (the slot is contested) and the sw_api wrapper slot (SyncFaction hash-verifies it). Do not use the Reconstructor plugin ABI (MSVC only).
3. **Transport.** Phase 1: direct-IP UDP inside the DLL. Users can port-forward or use Tailscale or ZeroTier. Options in order:
   * Raw Winsock with a small reliability layer.
   * RakNet core (BSD), if its reliability layer or NAT punch is worth the build effort.
   * The engine's `GameLinkLan` / `keen::network`, worth probing because it is already in the exe.
   Phase 2 (optional): sw_api `SteamNetworking()` P2P on a private channel through THQNO, for free NAT traversal.
4. **World bootstrap.** Make sure both peers start from the host's world state.
   * Ship the host's slot (at most 2.5 MB; 1.3 MB mid-game), with the opaque blobs passed through unchanged.
   * Inject it into RAM on the client at `Save_system_save_data + slot*0x2804D4` (`+0x188` exists, `+0x18C` info, `+0x248` size, `+0x24C` world), then call the normal `game_save_load` path. RAM needs no CRC.
   * Fallback: edit the file offline only while the game is closed, on a backup, then fix the CRC. Watch for GOG cloud sync.
   * After the bootstrap, live NPCs, vehicles and mission state must be replicated separately.
5. **Identity.** Host-authoritative network IDs.
   * Intact zone objects can be keyed by game handle, checked against class hash and obj_type, only after both peers load the same save.
   * Shards, debris, spawned NPCs and vehicles go in a host-to-client mapping table.
   * Do not reuse `add_reserved_handles`, which is MP-only, capped at 1000 and a snapshot at level init.
6. **Parity handshake.** Exchange SHA-256 of `rfg.exe`, `sw_api.dll`, the six store-specific vpp files and any modded files, using `Hashes.cs` as the reference. Cache the result, because `terr01_l1` is 4.4 GB. Also exchange "Reconstructor present: yes/no". Refuse or warn on mismatch.
7. **Puppets.** Remote vehicles and bodies use the `havok_body_set_*` handle overloads plus `vehicle_interaction_request_enter/exit`. Remote humans use `human_teleport`. Keep remote-command paths closed, as Reconstructor does with rcon.

## Contested / refuted (do not rely on)

* **"The save world sections are fully mapped."** Refuted: `ScriptSystemState` and `TrackedStatisticsState` are opaque, Activities is 16/17, and PlayerStat and Script data are unimplemented. Whether the opaque blobs hold mission-script state that a mid-mission join needs is unknown.
* **"Write the save file and the client loads it."** Wrong while the game is running. The game reads the file once into RAM, and the next in-game save overwrites external edits.
* **"Level objects have the same handle on every machine that loads identical data."** Holds only for intact zone-instantiated objects with identical save and destroyed state. Shards are local, handles get recycled, and serialize_zone skips objects depending on save state.
* **"The engine MP reserved-handle scheme applies."** It does not run in single player and does not fit streaming terr01 (more than 1000 movers, snapshot at level init).
* **"The game's Lua is 5.1."** It is 5.0, and UI only.
* **"Only two transport routes (own or THQNO)."** `GameLinkLan` is a third route. Whether it can run while `Multiplayer` is false is unverified.
* **"The user's installs are identical and unpatched (F2)."** Wrong. The GOG GALAXY copy has the patched `sw_api.dll` (67f9f8e9…). Whether terr01_l1 is stock is unknown (not hashed).
* **"0h_c1218/0h_c1618/0h_c1219 are building chunk containers."** Only `0h_c1219` holds building chunks. The other two hold mostly terrain, ore and effects.
* **"Steam vs GOG is a same-exe data mismatch."** No, the exes differ too. The same-exe case is Unification versus stock GOG.
* **"About 35 string pools doubled; the rcon, mempool and inventory patches are Reconstructor's MP patches."** The list is incomplete. It also has the countdown change, the MP UI patches and the gameseq hook, and the pool count is 34 (32 doubled, one ×4, one unchanged).
* **RakNet "usable":** only a 4-file syntax check passed. Linking is unproven, and wire compatibility with sw_api's RakNet is unknown.
* **sw_api P2P through THQNO:** depends on third-party servers with unknown operators and lifetime (`nat2.thqonline.net`, see `transport.md`). It was not tested in this domain.

## Unknowns and the probes that settle them

| Unknown | Probe |
|---|---|
| Do zone-object handles match across machines after loading the same save? How fast do shard handles diverge? | Dump `(handle, obj_type, sub_type, class hash, pos)` for all rfg_mover/general_mover within 200 m on two PCs (or twice on one PC) and diff. Destroy one building on each and diff the shard handles. |
| Does RAM injection plus `game_save_load` load a host world on the client? | In a test build, memcpy slot 0 into slot 11 of `Save_system_save_data` (set `+0x188`/`+0x18C`/`+0x248`/`+0x24C`) and load slot 11. Do not save afterwards, or the whole RAM image is written to disk. |
| Does a main-menu refetch (`game_save_start_get_save_data`) re-read disk? | Breakpoint or log `game_save_process` after returning to the main menu. Check whether `getSaveData` re-runs and whether it hits the file. |
| What do the opaque `ScriptSystemState` and `TrackedStatistics` hold? | Diff the blobs between autosaves before and after a mission start or checkpoint. |
| Can `GameLinkLan` / `keen::network` be created and used while `Multiplayer` is false? | Trace the real caller of `GameLinkLan::create` 0x8D6A40 (xrefs show folded thunks; check rfg.asm call sites). Then call `keen::network::createSocket`/`bind`/`sendDataTo` from the DLL in the campaign. |
| Does sw_api `SendP2PPacket` work outside a THQNO lobby, and are non-zero channels left alone? | Call `SteamNetworking()` 0x8D13A0 after load, then `IsP2PPacketAvailable(&n,7)` and `SendP2PPacket(peer, buf, 16, 2, 7)` between two PCs, with and without a lobby. |
| Can the havok puppet drive smoothly? | Each frame, `havok_body_set_pos_orient` + `set_linear_velocity` on a spawned vehicle from a recorded track. Compare with `set_movable(false)`. |
| Does RakNet link under llvm-mingw? | Compile `Source/*.cpp` with `-O2 -c`, link a test DLL with `-lws2_32`, and send reliable-ordered messages over loopback. |
| Is `terr01_l1` stock or Unification-patched on each peer? | Hash it once with an offline script (4.4 GB; run it alone, given machine load) and compare with `a89bfd54…` and `12CC145B…`. |
| Do Reconstructor and RfgCoop coexist? (only if a partner uses SyncFaction MP) | Launch via Reconstructor `Launcher.exe` with RfgCoop loaded from winmm or xinput. Confirm both load and the dinput8 nag does not fire. |
| Can Nanoforge v1.3.0 open terr01 and show handles? | Open it offline. Otherwise port `RfgZoneFile.Read` (about 60 lines) next to the vpp_pc reader. |
