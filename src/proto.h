// RfgCoop wire protocol (M2 "Link", M4 "See each other", M5 "Join the host's world") and the sim-thread API between
// net.cpp, session.cpp, puppet.cpp and world.cpp. Messages are packed little-endian structs whose first byte is the type;
// the receiver checks the exact size and channel of every type and drops anything else. Nothing received is ever
// executed. research/notes/session_impl.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// 2: password challenge-response, BYE with a reason; 3: the guest proves first, per-session salt; 4: M5 join;
// 5: PSTATE carries the sender's worn backpack (research/findings/workflows/puppet_backpack.md); 6: PSTATE carries its
// activity (bpAct, bpFuel; research/notes/backpack_sync.md §3); 7: M6 live destruction (kDestr, kCensus, kCensusRes,
// kResync; BLOB_BEGIN + destr + logPkts; research/notes/destruction_impl.md §11); 8: ver7.2 stream records GM_DEAD
// (kind 3) and RESTORE how 3, destr bit 4 = gm (destruction_impl.md §12)
constexpr uint16_t kProtocol = 8;
// reliable ordered / unreliable sequenced / reliable ordered for the big ones (world blob chunks, LOADOUT; each packet
// at most ENet's maximumPacketSize 4096, net.cpp). Channels 0 and 1 carry nothing bigger than PSTATE.
enum : uint8_t { kChControl = 0, kChState = 1, kChBlob = 2, kChannels = 3 };
enum : uint8_t { kHello = 1, kWelcome, kReject, kBye, kLoading, kReady, kPEvent, kPState, kChallenge, kAuth, // kPEvent: reserved
                 kJoinWait, kBlobBegin, kBlobChunk, kJoined, kRejoinReq, kPaused, kLoadout, kSalvage,     // 11-18: M5
                 kDestr, kCensus, kCensusRes, kResync };                                                 // 19-22: M6

#pragma pack(push, 1)
struct MsgHello {                                // guest -> host, channel 0
    uint8_t type;                                // kHello
    uint16_t proto;                              // kProtocol: checked before the size, so any other build gets a readable REJECT
    uint32_t peStamp, imageSize;                 // rfg.exe PE TimeDateStamp, SizeOfImage
    uint32_t crcXinput, crcWinmm, crcDinput8;    // CRC32 of xinput1_3.dll / winmm.dll / dinput8.dll beside rfg.exe, 0 = absent
    int32_t reserved;                            // always 0 (keeps the message layout)
    uint32_t vpp;                                // hash of the data\*.vpp_pc name+size list
    uint8_t pw;                                  // 1 = the guest has a password (AUTH / WELCOME carry proofs), 0 = none
    uint8_t nonce[16];                           // Ng, fresh random per connection; no password material
    char name[24];
};
// Key = PBKDF2(password, the host's per-session salt). Proofs: HMAC-SHA256(key, label NUL || first nonce || second
// nonce || the guest's HELLO as sent), session.cpp Proof; all zero in an open session (no password), where CHALLENGE /
// AUTH still run for the Nh echo. The guest proves first: nothing password-derived reaches a peer that has not.
struct MsgChallenge { uint8_t type; uint8_t nonce[16], salt[16]; }; // host -> guest: Nh, this host session's salt
// guest -> host: Nh echoed (the sender receives the host's packets: no forged address links, and the host counts a
// wrong proof only with it), proof "guest" Nh Ng
struct MsgAuth { uint8_t type; uint8_t nonce[16], proof[32]; };
struct MsgWelcome { uint8_t type; char name[24]; uint8_t proof[32]; }; // host -> guest after a good AUTH: proof "host" Ng Nh
// kReject: host -> guest, then the host drops the peer. kBye: either, linked (also a guest refusing WELCOME's proof)
struct MsgReject { uint8_t type; char reason[120]; };
struct MsgByte { uint8_t type; };                // kLoading / kReady, channel 0
// ---- M5 (session_impl.md "M5: join"). kJoinWait (host -> guest: why the join waits) and kRejoinReq (guest -> host:
// why it asks) use MsgReject's layout, channel 0.
struct WorldMeta {                               // what platform_load takes from a save's game_save_info: the host's
    int32_t dlc;                                 // World+0x2694 dlc_bundle_id (info +0x30)
    uint8_t hammers[8];                          // s_hammers_unlocked 0x2C256B0 (info +0x3A)
    uint8_t golden;                              // Wants_to_start_game_with_golden_hammer 0x2C2E044 (info +0x39)
    uint8_t backpacks[2];                        // s_backpacks_unlocked 0x2C16C50 (info +0x42)
    uint8_t jetpack;                             // s_jetpack_upgrade_level 0x2C16C52 (info +0x44)
};
enum : uint8_t { kBlobJoin, kBlobHostLoad, kBlobAsked }; // MsgBlobBegin::kind: join / after a host load / REJOIN_REQ
struct MsgBlobBegin {                            // host -> guest, channel 2, then the chunks
    uint8_t type, kind;
    uint16_t id;                                 // +1 per blob the host sends; chunks and JOINED carry it
    uint32_t size;                               // 8 .. kBlobMax
    WorldMeta meta;
    float pos[3];                                // the host's position (its vehicle's while driving) when it sent this
    uint8_t clock[3];                            // the host's Game_clock h m s (log only: the blob carries the time)
    uint8_t destr;                               // M6: this epoch's destruction mode, the host's ini (bits 0-1 mode, 2
                                                 // profile, 3 lane, 4 gm, 5 own-pack, 6 autoresync (with 7), 7 stale;
                                                 // destruction_impl.md §11)
    uint32_t logPkts;                            // M6 ver7.2: world-log packets right after the blob (0 until then)
};
struct MsgBlobChunk { uint8_t type; uint16_t id, index; }; // + exactly min(kBlobChunkBytes, size - index * kBlobChunkBytes) bytes
constexpr uint32_t kBlobChunkBytes = 4000, kBlobMax = 2621440; // 2.5 MB (RESEARCH.md §2: user's mid-game slot 1.3 MB)
struct MsgJoined { uint8_t type; uint16_t id; uint32_t transferMs, loadMs; float dist; }; // guest -> host, ch 0: landed
struct MsgPaused { uint8_t type, on; };          // host -> guest, ch 0: the host's game is paused (1) or runs (0)
struct MsgSalvage { uint8_t type; int32_t amount; }; // guest -> host, ch 0: salvage the guest collected, 1..30000
struct MsgLoadout {                              // host -> guest, ch 2: the host's loadout (rule Q13)
    uint8_t type, pad;
    uint16_t arsenalBytes;                       // 8 per Weapon_list item, at most 64 items
    int32_t salvage;                             // player_meta_data +0x4
    uint8_t upgrades[0x400];                     // player_meta_data +0x18 upgrade_item[128] (8 bytes each)
    uint8_t arsenal[0x200];                      // Weapon_list items {u32 weapon handle, u8 purchased, 3 pad}
};
// ---- M6 (destruction_impl.md §11). Epoch = the blob id the stream belongs to; the guest drops any other.
struct MsgDestr { uint8_t type; uint32_t epoch, pkt; uint16_t len; }; // host -> guest, ch 2, + len (1..kDestrBytes) bytes
constexpr uint32_t kDestrBytes = 4000;           // of the record stream; pkt 1, 2, ... per epoch (a gap = re-join)
struct MsgCensus {                               // host -> guest, ch 2, behind the stream bytes it covers
    uint8_t type, kind;                          // kind bit 0 resync check (at JOINED), bit 1 own-pack (no verdict)
    uint16_t index, count;                       // resync check: i of n
    uint32_t epoch, seq, handle;                 // seq: the host's step counter when censused (vs STEP seq: newer events)
    float pos[3];                                // the building's position on the host (identity check)
    uint16_t n, alive;                           // subpieces, alive ones
    uint32_t hStatic, hPart;                     // FNV-1a of the alive bitset / of the label array
    uint16_t chunks, gone, orphan;
    uint8_t destroyed, pad;
    float dmg;
    int32_t chunksAll;                           // the host's runtime chunks (uid != -1) in the world
};
// guest -> host, ch 0: verdict bit 0 identity OK, 1 static MATCH, 2 part MATCH, 3 no verdict, 4 resync check; chunks:
// the guest's runtime chunks; poseMax: 0 until 7.3. kResync (guest -> host, ch 0, MsgByte): the guest's F3.
struct MsgCensusRes { uint8_t type, verdict; uint32_t epoch, handle; int32_t chunks; float poseMax; };
#pragma pack(pop)
// Naturally aligned with no padding (the size check proves it), so its float arrays can be read in place.
struct MsgPState {                               // both ways, channel 1, 20 Hz: the sender's own Player
    uint8_t type;                                // kPState
    uint8_t flags;                               // bit 0: on foot (the puppet shows); 0 = in a vehicle (hidden)
    uint16_t seq;                                // +1 per PSTATE (loss statistics; Puppet_In: a pack step after a loss burst is stale)
    uint32_t time;                               // sender's Clock_current_time at capture (a still state: when sent), ms
    float where[3];                              // sender's real position (its vehicle while driving): F7, streamed check
    float pos[3], vel[3], orient[9], cam[9];     // multi_player_state +0x00 +0x0C +0x24 +0x4C (cam = the aim)
    float forward, sideways, hp;                 // +0x78 +0x7C +0x80 (hp: fraction of max)
    uint32_t ladder;                             // +0x84 ladder handle (zone object: same on both machines) or ~0
    int32_t ragdoll, knockdown;                  // +0x88 human_ragdoll_state, +0x8C ms until the knockdown ends
    uint16_t ladderState;                        // +0xA2 mode | rung << 8
    uint8_t det;                                 // detonation counters: low nibble remote charges, high nibble in air
    uint8_t pack;                                // worn backpack: Backpack_info id + 1 (1..10 = ids 0..9), 0 = none
    uint32_t bits;                               // +0xA4 engine bitfield (notes/ghost_impl.md "The state")
    uint32_t item;                               // inv_item_info handle (+0x04) of the equipped item, ~0 = none
    uint8_t bpAct;                               // worn pack: bits 0-3 activation counter, 4 active (+0x254), 5 Thrust
                                                 // up (+0x2AC), 6 landed (+0x2AD), 7 frozen (a still state: the sender
                                                 // is paused, or nothing captured for 150 ms; level and fuel stale; the
                                                 // receiver ends the pack)
    uint8_t bpFuel;                              // its fuel +0x25C * 255
    uint16_t pad;                                // 0
};
static_assert(sizeof(MsgHello) == 72 && sizeof(MsgChallenge) == 33 && sizeof(MsgAuth) == 49 && sizeof(MsgWelcome) == 57 &&
                  sizeof(MsgReject) == 121 && sizeof(MsgPState) == 156 && sizeof(WorldMeta) == 16 &&
                  sizeof(MsgBlobBegin) == 44 && sizeof(MsgBlobChunk) == 5 && sizeof(MsgJoined) == 15 &&
                  sizeof(MsgPaused) == 2 && sizeof(MsgSalvage) == 5 && sizeof(MsgLoadout) == 1544 &&
                  sizeof(MsgBlobChunk) + kBlobChunkBytes <= 4096 && kBlobMax / kBlobChunkBytes < 0xFFFF &&
                  sizeof(MsgDestr) == 11 && sizeof(MsgDestr) + kDestrBytes <= 4096 && sizeof(MsgCensus) == 58 &&
                  sizeof(MsgCensusRes) == 18 && sizeof(MsgCensusRes) <= sizeof(MsgPState),
              "wire sizes");

// ---- net.cpp: the ENet thread. Sim thread side; never blocks except NetStart waiting for a previous thread to end.
// password: UTF-8, "" = none; salt: the host's for this session (the guest takes the host's from CHALLENGE)
struct NetConfig { bool host; char addr[128]; uint16_t port; int lagMs, jitterMs, lossPct; char password[384]; uint8_t salt[16]; };
enum : uint8_t { kNetConnected = 1, kNetDisconnected, kNetData, kNetFailed, kNetNote }; // NetEvent::kind (Note: a HUD line)
struct NetEvent { uint8_t kind, channel; std::vector<uint8_t> data; }; // Connected/Failed/Note: data = text (no NUL)
bool NetStart(const NetConfig& c);               // starts the net thread (enet_initialize on its first start)
void NetStop();                                  // queued packets go out, then disconnect; the thread ends by itself
void NetSend(uint8_t channel, const void* data, size_t n); // dropped while no peer is connected
void NetKick();                                  // disconnect the peer after what is queued (REJECT); the host keeps listening
bool NetPoll(NetEvent& e);                       // next event from the net thread (after the lag shim)
uint32_t NetRtt();                               // ENet's mean round trip time, ms
uint32_t NetDrops();                             // received reliable items (channels 0 and 2) dropped by a cap, any thread
size_t NetQueued();                              // channel-2 bytes ENet holds to send (queued or unacked), any thread
size_t NetRangeCoded(const void* p, size_t n);   // sim thread: what ENet's range coder would make of n <= 4096 bytes
                                                 // (a bandwidth estimate for the log; never used on the wire)
// The password's 32-byte key, made by the net thread (DeriveKey). Host: from NetConfig::salt before it posts any event,
// so valid from kNetConnected to NetStop; its password copy is wiped then. Guest: from the salt of the connection's
// first CHALLENGE, before that CHALLENGE is queued, so valid while the session answers it and checks WELCOME; it keeps
// the password until it ends (a restarted host brings a new salt). The key is wiped when the thread ends. Zero without
// a password.
const uint8_t* NetKey();

// ---- session.cpp, any thread (no game memory)
bool DeriveKey(const char* password, const uint8_t salt[16], uint8_t key[32]); // PBKDF2-HMAC-SHA256, 100000 rounds: ~35 ms, net thread

// ---- puppet.cpp: the partner puppet (same recipe, patch set and Deliver / Effects path as the ghost)
bool Puppet_Net(bool on);                        // link up: hooks in, ghost off, capture for PSTATE; down: partner puppet goes
bool Puppet_Out(MsgPState& m, bool paused);      // your latest capture as a PSTATE (seq left 0); false = nothing to send.
                                                 // paused: your game is paused, so the state goes out still + kFrozen
bool Puppet_Step();                              // the capture to send stepped the pack counter: send it now, no 50 ms gap
void Puppet_In(const MsgPState& m);              // a partner state (validated and mapped into the local Clock there)
void Puppet_PartnerLoading(bool loading);        // LOADING / READY from the partner
bool Puppet_PartnerPos(float pos[3]);            // the partner's last real position (F7); false = none or loading
uint32_t Puppet_Handle();                        // the partner puppet's object handle, ~0 = none
void Puppet_SetWorldBit(bool on);                // guest, per epoch (M6 Q19): the partner puppet's pack may break the world

// ---- world.cpp: the world blob (E3's capture and load, shared by the dev keys and the M5 join). Sim thread only.
const char* World_NotFreeRoam();                 // why no silent blob can be made now; NULL = free roam, it can
bool World_InMission();                          // Mission_active_index != -1 (World+0x2578 left at 1 in free roam is
                                                 // not a mission: a silent blob sets it back to 0)
const char* World_CaptureStart();                // asks for a silent blob (set at the next free-roam gameplay handshake);
                                                 // NULL = asked, else why not (one at a time)
int World_CapturePoll(std::vector<uint8_t>& out); // 0 pending, 1 done (out = the blob), -1 failed or none (logged)
const uint8_t* World_CurrentState(int& n);       // stored_world_state[World+0x2578] (what the last load used), NULL = none
void World_ReadMeta(WorldMeta& m);               // this game's dlc id and load globals
bool World_BlobOk(const uint8_t* p, int n);      // "RFGS" version 52, 8 .. kBlobMax bytes
// 0 = not now (call again next frame), 1 = load started, -1 = refused (logged). session: Guest_AllowLoad() right
// before set_pending_territory_load, so only this call passes the guest's load fence
int World_Load(const uint8_t* p, int n, const WorldMeta& m, bool session);

// ---- session.cpp
bool Session_Active();                           // a session runs (any phase but off): world.cpp keeps F9 / F10 off
