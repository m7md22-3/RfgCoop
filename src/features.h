// RfgCoop features, called from core.cpp.
#pragma once
#include <cstdint>

// Ghost puppet (E2/M3, src/puppet.cpp): a second player object that replays your own movement 300 ms late, through
// the same code path networked play will use. dev=1 + F6 toggles it.
void Puppet_Install();         // DllMain, after the build check (the exe is mapped and relocated)
void Puppet_Tick(int state);   // sim thread, every frame in every SP state (0x1D24D9 tick); state = top game state

// Session and link (M2/M4, src/net.cpp + src/session.cpp): ENet on its own thread, handshake, partner puppet.
void Net_Install();            // DllMain, after the build check: read the ini only (no threads, no WSAStartup here)
void Net_Tick(int state);      // sim thread, every frame in every SP state

// World experiments (E3 blob round trip, E8 handle dump, src/world.cpp), dev=1 hotkeys.
void World_Install();          // DllMain, after the build check
void World_Tick(int state);    // sim thread, every frame in every SP state

// ---- M5: the guest joins the host's world. Interface between the session (A) and the guest rules (B). ----
// Guest rules and fences (src/guest.cpp): on from the moment this game links as a guest until the process exits
// (RESEARCH.md §3.3, §4.8, notes/gaps.md patch list GF-*, user rules §8 Q12-Q15).
void Guest_Install();          // DllMain, after the build check
void Guest_Tick(int state);    // sim thread, every frame in every SP state
void Guest_Arm();              // session.cpp, sim thread: this game linked as a guest. Idempotent; never disarmed
void Guest_ResetLoadout();     // session.cpp Start, guest: drops the Q18 weapon record (the session's first join is the host's)
bool Guest_Armed();
void Guest_AllowLoad();        // world.cpp World_Load(session=true), for session.cpp: the next world::set_pending_territory_load
                               // is the session's own (join/re-join)
// The host's loadout on the guest (rule Q13): raw copies of the host's player_meta_data upgrade_item[128] table, its
// unlocked arsenal and its salvage total, applied on the sim thread.
void Rules_ApplyHostLoadout(const uint8_t* upgrades, int upgradeBytes, const uint8_t* arsenal, int arsenalBytes, int salvage);
void Rules_AddHostSalvage(int amount); // host: salvage the guest collected (rule Q13), added to the host's Player
// Rule Q17: the guest's own hammer and backpack, picked before it connects (src/picker.cpp, one game message box:
// HAMMER/BACKPACK char-list rows + CONNECT; notes/picker_impl.md) and applied by guest.cpp after every join and
// re-join. Sim thread.
void Picker_Open();            // session.cpp Start(pick): guest F8 past Start's refusals; ignored while the box is up
bool Picker_Tick();            // session.cpp Net_Tick, every tick: true once CONNECT was accepted (-> Start())
void Guest_SetPick(void* hammer, int pack); // inv_item_info*, backpack upgrade (0 jetpack, 0x35..0x3D, 0x3E none)
// Implemented by session.cpp:
void Session_RejoinRequest(const char* why); // guest: a load / death the guest may not do itself -> ask the host
void Session_SendSalvage(int amount);        // guest: salvage collected -> host
// Implemented by world.cpp:
bool World_ArmSaveGuard(const char* why);    // arms the 0x811278 save guard once (never chain that site twice)
// Implemented by session.cpp for the E4 runner (sim thread): F7's teleport with its streaming hold (StepTeleport runs
// from Net_Tick every gameplay frame, session or not)
void Session_TeleportTo(const float pos[3], const char* why);
bool Session_Teleporting();                  // the hold is still running

// ---- M6 shared destruction (src/destruction.cpp; research/notes/destruction_impl.md). ver7.0: E4 on one game, dev=1 F5.
void Destr_Install();          // DllMain, after the build check: byte-checks and installs every site, all bytes 0 (vanilla)
void Destr_Tick(int state);    // sim thread, every frame in every SP state, after Net_Tick
void Destr_Stamp();            // world.cpp 0x3D9159 chain: a capture's save_state_data runs right after it
void Destr_Saved();            // world.cpp 0x3D9159 chain: process_save_load rewrote a stored buffer (7.2 re-join log stamps)
bool World_HookSaves();        // world.cpp: the 0x3D9159 chain installed (idempotent); false = a sibling owns that call
const uint8_t* World_StateBuf(int i, int& n); // stored_world_state[i] bytes now, NULL / n 0 when empty
void Destr_GuestArm();         // guest.cpp Guest_Arm: GF-EDF on until the process exits
bool Destr_Busy();             // world.cpp OnKey: the E4 runner owns the world (F9 / F10 off)
int Destr_Chunks();            // core.cpp dev stat line: rfg_movers with a runtime uid, -1 outside gameplay
// ver7.1, the live stream (destruction_impl.md §11). Sim thread. The host's ini decides the mode of each epoch (= blob id).
void Destr_Link(bool on, bool host);              // session Link / Unlink / Stop: the live roles on, or everything off
// host SendBlob: epoch id starts; returns BLOB_BEGIN's destr byte. resend = the loaded buffer (the stream restarts there,
// else it holds the steps since the capture); rejoin = an in-mission REJOIN_REQ resend: stale (mode 0, bit 7) after a
// non-empty step, a DESPAWN or a host-only restore since our last load
uint8_t Destr_HostEpoch(uint16_t id, bool resend, bool rejoin);
int Destr_SinceLoad();                            // those steps, DESPAWNs and restores since this game's last level load
bool Destr_Streaming();                           // host: the live epoch still has its id (false after a capture's stamp,
                                                  // the 16 MB end or our load, until the next blob; always true in mode 0)
void Destr_HostJoined(uint16_t id);               // host: JOINED for blob id: the resync check census
void Destr_GuestBegin(uint16_t id, uint8_t destr); // guest BLOB_BEGIN: the new epoch and its mode
void Destr_GuestDrop();                           // guest DropBlobs: the epoch ends (0), its stream is dropped
void Destr_NetDrop(); // guest: a reliable item was lost (NetDrops): end the epoch's stream and re-join
const char* Destr_OnMessage(uint8_t t, const uint8_t* d, size_t n); // kDestr / kCensus (guest), kCensusRes (host); NULL = taken
void Destr_Crush(uint8_t* victim, uint8_t* attacker, int type); // puppet.cpp OnHumanDamage (dev=1 counters for M7a)
// Implemented by session.cpp for destruction.cpp:
bool Session_EpochLoaded(uint32_t epoch);         // guest: that blob is loaded here, landed, nothing newer coming
bool Session_StreamOpen();                        // host: the guest has our world and no capture / blob is in flight; not loading
bool Session_Capturing();                         // host: the session's own capture is in flight (Destr_Stamp stamps only it)
void Session_AskBlob();                           // host: the stream broke (16 MB backlog): a fresh blob starts the next epoch
