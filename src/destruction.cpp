// M6 shared destruction, ver7.0: E4 on one game (research/notes/m6_plan.md §4.1; every site, ABI, record and log line:
// research/notes/destruction_impl.md). Installed at DLL load; every site is checked byte for byte first and nothing is
// written unless all match. Every hook passes through and every co-op byte is 0 (vanilla SP) unless the dev=1 F5 runner
// turns them on, or Guest_Arm turns GF-EDF on for good:
//   operands (L5 idiom, `cmp byte [Multiplayer],0` -> a co-op byte): gc_profile (Q10 MP destruction profile) at 0x339EA3
//            0x339FE9 0x33811A 0x3249AC 0x332772; gc_rfglGuest (rfgl_do_frame skipped: no local debris deleter) at
//            0x325051; gc_edf (GF-EDF: no district control change) at 0x3B0024. Never opened: the create_shard sites,
//            0x312670, 0x3383CF, 0x33830F, 0x33D261, RFG_load_balance 0x124F0FB
//   0x4102E0 postCollideCallback `call rfg_dyn_do_collide_post`: the step (record / replay)
//   0x3383FD `call rfgl_desired_values`: debris tuples (record) / the host's tuple, the own one logged as a shadow (replay)
//   0x324ACC / 0x324ADE rfgl_find_and_delete_object_mover's fade / destroy calls, 0x6A2657 / 0x7CD092 the explosion
//            hit's and do_debris_cleanup's fades: DESPAWN records (the last two skipped on a live mode-2 guest)
//   0x33A94C / 0x33A92E regrow begin / finish calls, 0x534B70 world::restore_object entry: the restore log
//   0x3249BF the shardify candidates' qsort comparator (abs32): on the replay, blob-loaded chunks sort as on the host
// F5 (dev=1, gameplay, free roam, no session, never on a guest): records a scenario from a silent capture, reloads that
// capture from memory and replays the record through the guest path e4_runs times, then logs the K3a / K3b verdict.
// The save guard stays on until the game is restarted.
// ver7.1 (m6_plan §4.2, destruction_impl.md §11), a linked session: the host records from each capture (Destr_Stamp) and
// streams the records in kDestr packets of its blob's epoch (the host's ini picks the mode, stamped into BLOB_BEGIN); the
// guest buffers them and replays them through the same code once that blob is loaded. Both run the census (kCensus at
// settle and a resync check at every JOINED; the guest answers kCensusRes).
#include "features.h"
#include "game.h"
#include "proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
uintptr_t gc_hierarchy, gc_restoreBack; // RestoreThunk: &Hierarchy and world::restore_object+6 (runtime addresses)
void __cdecl OnRestore(uintptr_t from, uint32_t handle);
}

namespace {

namespace site {
constexpr uintptr_t StepCall      = 0x004102E0; // postCollideCallback: E8 -> rfg_dyn_do_collide_post, then C2 08 00
constexpr uintptr_t CollidePost   = 0x00340710; //   void __cdecl(), plain ret
constexpr uintptr_t RfglCall      = 0x003383FD; // rfg_dyn_shardify_setup: 50 E8 -> rfgl_desired_values (its only caller)
constexpr uintptr_t RfglValues    = 0x0031D000; //   void __cdecl(int out[2] = {quality, delete}, int count, int flags)
constexpr uintptr_t FadeCall      = 0x00324ACC; // rfgl_find_and_delete_object_mover: 56 E8 -> object_mover_mark_for_fade
constexpr uintptr_t MarkForFade   = 0x006139A0; //   void __cdecl(object_mover*, int ms)
// object_mover_process_explosion_hit, SP: 52 56 E8 -> mark_for_fade, add esp,8. rfg_movers reach it only from a Duration
// explosion (explosion_info +0x28 & 0x8000, explosion_parse_entry 0x2E82AC); any other queues a step event and returns
// at 0x6A250A. Runs outside the step (explosion_do_frame, shape_cutter_do_frame).
constexpr uintptr_t ExplFadeCall    = 0x006A2657;
constexpr uintptr_t CleanupFadeCall = 0x007CD092; // do_debris_cleanup: 6A 00 51 E8 -> mark_for_fade (0 ms), add esp,8
constexpr uintptr_t DestroyCall   = 0x00324ADE; //   B9 <World> E8 -> world::destroy_object
constexpr uintptr_t DestroyObject = 0x0051CEF0; //   void __thiscall(object*, int), ret 8
constexpr uintptr_t IsVisible     = 0x00613960; // bool __cdecl object_mover_is_visible(object_mover*): the deleter's fade test
constexpr uintptr_t GmGate        = 0x0068A923; // general_mover::apply_damage: 80 3D <Multiplayer> 01 (GF-GM operand +2)
constexpr uintptr_t GmSlot        = 0x00F14064; // general_mover vftable slot 34 = apply_damage (relocated: g_base + GmApply)
constexpr uintptr_t GmApply       = 0x0068A920; // bool __thiscall(float, damaged_by_types, vector*, vector*, object*, unsigned, bool), ret 0x1C
constexpr uintptr_t SortPush      = 0x003249BF; // alloc_rfg_mover_process_shardify_list (qsort, only with gc_profile): 68 <abs32>
constexpr uintptr_t ListCompare   = 0x00314FF0; //   rfg_mover_list_compare, int __cdecl(entry*, entry*), entry {mover, handle, uid}
constexpr uintptr_t RegrowBeginCall  = 0x0033A94C; // rfg_dyn_do_frame_pre: 57 E8 -> rfg_mover_regrow_begin
constexpr uintptr_t RegrowBegin      = 0x00335DC0; //   bool __cdecl(rfg_mover*)
constexpr uintptr_t RegrowFinishCall = 0x0033A92E; //   57 E8 -> rfg_mover_regrow_finish
constexpr uintptr_t RegrowFinish     = 0x003360E0; //   void __cdecl(rfg_mover*)
constexpr uintptr_t RestoreObject = 0x00534B70; // world::restore_object: 8B 0D <Hierarchy> 56 8B 74 24 08, thiscall(handle), ret 4
constexpr uintptr_t Hierarchy     = 0x028CA67C; //   its first operand (ecx is reloaded there: the callers' ecx is unused)
// its per-machine callers (each game runs them itself, so they never make a resend stale): object_mover::stream at
// stream-in, public_address_kiosk_level_init_post at every load (C0: all ~40 restores per reload)
constexpr uintptr_t StreamRestore = 0x0068B9F8, KioskRestore = 0x00734D1E;
constexpr uintptr_t Profile[5]    = {0x00339EA3, 0x00339FE9, 0x0033811A, 0x003249AC, 0x00332772};
constexpr uintptr_t RfglLane      = 0x00325051; // rfgl_do_frame: 1, then multi_is_server = Server 0 -> jne 0x325209 (ret)
constexpr uintptr_t EdfGate       = 0x003B0024; // gfm_change_edf_control: 1 -> jne 0x3B035B (ret)
constexpr uintptr_t Queue     = 0x01CC12F8;     // RFG_dyn_collisions[1024], 0x110 each
constexpr uintptr_t Count     = 0x01AF7268;     // int Num_rfg_dyn_collisions (read 0x340787, 0 at 0x340A2B)
constexpr uintptr_t Lock      = 0x01B16350;     // Collision_event_wrapper (next_free_collision_event 0x313319)
constexpr uintptr_t LockEnter = 0x0018AC30, LockLeave = 0x0018AC40; // cs_wrapper::enter / leave, thiscall, no stack args
constexpr uintptr_t ObjFromHandle  = 0x0053BEE0; // object* __thiscall world::object_from_handle(unsigned)
constexpr uintptr_t RandRegenerate = 0x0017A290; // void __cdecl rand_regenerate(int seed): new MT seed, pool refilled
constexpr uintptr_t HudMessageAdd  = 0x00455D30;
constexpr uintptr_t PausedCount    = 0x02132880; // int Game_paused_count (session.cpp): no census while paused
} // namespace site
constexpr uintptr_t kImageSize = 0x397B000;     // core.cpp's build check
// rfg_collision_event (0x110): the 0xEC prefix is the input; +0x50 / +0xA8 object handles (base_a / base_b; only type 4
// reads +0xA8, 0x33A2A7), +0xB0 type, +0xC4 owner handle, +0xC8 u8 flags (bits 0/1 skip, 2 MP send, 3/4 apply inputs).
// The tail +0xEC..+0x10F is the apply's output and is never recorded.
constexpr int kEv = 0x110, kPrefix = 0xEC;
constexpr uint32_t kLoaded = ~0u, kNotMover = ~1u; // REvent uid tags: an rfg_mover with uid -1 (zone / blob) / no rfg_mover

// ---- the record stream (m6_plan §3.1: ver7.0's recorder format is ver7.1's wire) ----
#pragma pack(push, 1)
struct RStep { uint8_t kind, nRfgl; uint16_t nEvents; uint32_t seq, uidBefore, uidAfter; uint16_t nHash, pad; uint32_t hostMs, flags; };
struct REvent { uint32_t uidA, uidB; uint8_t owner, pad[3]; uint8_t prefix[kPrefix]; }; // owner 0 none, 1 host, 2 guest
struct RRfgl { uint16_t count, flags; int16_t quality, del; };                        // saturated to 16 bits
struct RHash { uint32_t handle, hStatic; uint16_t alive; uint8_t firstTouch, pad; };   // alive 0xFFFF = no shape
struct RDespawn { uint8_t kind, fade; uint16_t dt; uint32_t uid, handle, fadeMs; }; // dt: ms after the last STEP's hostMs
struct RRestore { uint8_t kind, how; uint16_t pad; uint32_t handle; }; // how 0 restore_object, 1 regrow begin, 2 finish,
                                                                     // 3 (7.2) our repair finished: the guest unmarks it
struct RUids { uint8_t kind, pad[3]; uint32_t n; };                    // + n x {handle, uid}: the host's chunks at the capture
#pragma pack(pop)
static_assert(sizeof(RStep) == 28 && sizeof(REvent) == 248 && sizeof(RRfgl) == 8 && sizeof(RHash) == 12 && sizeof(RDespawn) == 16 &&
                  sizeof(RRestore) == 8 && sizeof(RUids) == 8, "record sizes");
// 3 GM_DEAD (7.2) and 5 DISTRICT (M9) are reserved. kCensusRec never travels: the guest appends a received kCensus to its
// buffer as {kCensusRec, MsgCensus}, so it is evaluated in stream order.
enum : uint8_t { kStep = 1, kDespawn = 2, kGmDead = 3, kRestore = 4, kUids = 6, kCensusRec = 0x43 };
// 7.2: a general mover killed on the host (pad[0]: its damaged_by_types)
struct RGmDead { uint8_t kind, pad[3]; uint32_t handle; float pos[3]; uint32_t hostMs; };
// BLOB_BEGIN destr (m6_plan §2): bits 0-1 mode (0 parallel, 1 live holes, 2 full), 2 profile, 5 own-pack (Q26), 7 stale
// (6 autoresync with it: session.cpp's, for the guest's landing line)
constexpr uint8_t kModeMask = 3, kProfileBit = 4, kGmBit = 0x10, kOwnPack = 0x20, kStaleBit = 0x80;
constexpr uint32_t kPriv = 0x40000000;   // private uid base (own-pack and synthetic pending steps): never paired
constexpr size_t kLiveCap = 16u << 20;   // the host's stream backlog, reserved at the link (appended inside the step)

uint8_t gc_profile, gc_rfglGuest, gc_edf, gc_gm; // operand targets, 0 = vanilla. gc_gm (7.2 GF-GM): 1 = local damage to
                                                // general movers is swallowed on this guest (the host's GM_DEAD kills them)
enum Mode : uint8_t { kOff, kRecord, kReplay } g_mode;
void(__cdecl* g_stepNext)();
void(__cdecl* g_rfglNext)(int*, int, int);
void(__cdecl* g_fadeNext)(uint8_t*, int);
void(__thiscall* g_destroyNext)(void*, uint8_t*, int);
uint32_t(__cdecl* g_regrowNext[2])(uint8_t*);
bool g_installed, g_dev, g_guest, g_inStep, g_f5;
uint8_t g_mp;
int g_catchup, g_runs, g_types;
uint32_t g_seq;   // 0x4102E0 calls (a Havok step failure can fire the callback twice in one frame)
DWORD g_hookTid;

std::vector<uint8_t> g_rec;   // STEP / DESPAWN records since the capture (Destr_Stamp)
// Reserved at F5 #1 and never outgrown while recording: a failed reallocation inside the Havok callback would throw
// through the game's frames (no unwind info there: terminate). Full = the runner aborts the scenario.
constexpr size_t kRecCap = 32u << 20;
bool g_full;
bool Room(size_t n) { return !(g_full = g_full || g_rec.size() + n > g_rec.capacity()); }
std::vector<RRfgl> g_rfgl;    // record: this step's rfgl calls
// record: DESPAWNs from inside the step (Havok's cannotAllocate 0x43C1DD -> rfgl_emergency_cleanup 0x325210 ->
// 0x3249E0, gated only by hkpWorld+0x8C, which is 0 in the post-collide callback). Appended after that STEP, so every
// record stays contiguous and DESPAWN(S) still lands before STEP(S+1).
std::vector<RDespawn> g_stepDespawns;
struct { int steps, events, filtered, despawns, despawnsInStep, rfglCut, types[14], ptrN, ptr[14][kPrefix / 4];
         uint32_t ptrEx[14][kPrefix / 4], stepMs; double lastRec; } g_rs; // stepMs: the last recorded STEP's hostMs
// Destr_Stamp: handle -> uid of every chunk (runtime uid) in the captured world. The blob loads them with uid -1 and the
// same handle; OnCompare sorts them by this uid on the replay, as the host did.
std::unordered_map<uint32_t, uint32_t> g_preUid;
// restore log (any thread: object_mover::stream restores at stream-in)
volatile LONG g_notes[3], g_noteBudget[3]; // restores, regrow begins, regrow finishes; log lines left per kind (C0: regrow
                                           // begin repeats every frame for one mover, so a few lines, then counts only)
volatile uintptr_t g_noteFrom;          // call site of the last restore
LONG Notes() { return g_notes[0] + g_notes[1] + g_notes[2]; }
void Budget(LONG n) { for (auto& b : g_noteBudget) b = n; } // per E4 phase (recording, feed) or live session

struct Cen { bool ok, destroyed; uint32_t hs, hp; int alive, n, chunks, gone, orphan; float dmg; };
// Buildings: the FIXED rfg_movers the recording targeted (first-touch order, at most 64). Per run: r0 = at the feed start,
// preDiff = changed here before its first touch although it loaded as the host had it, skipped = a host event naming it
// was dropped (no verdict, like unstreamed). Live host: the newest 64 touched; ep = the epoch serial of its last first
// touch, ownEp = hit by an own-pack cutter in that epoch (no verdict), due = a census 10 s after `last` (its last event).
struct Bld { uint32_t h; float pos[3]; Cen ref; bool matched, preDiff; int lines, runs, staticDiff, unstreamed, skipped;
             LONG notesAt, skipAt, idleAt; RHash r0; uint32_t ep, ownEp; double last; bool due; };
std::vector<Bld> g_bld;
Bld* FindBld(uint32_t h) { for (Bld& b : g_bld) if (b.h == h) return &b; return nullptr; }

// replay
size_t g_rd;                                  // cursor into g_rec
bool g_feeding, g_uidLeftOk;
double g_feedT0;
uint32_t g_host0, g_lastApplied, g_uidLeft, g_uidHigh; // g_lastApplied: the hostMs of the last applied STEP; g_uidHigh:
                                                        // the highest uid counter seen here this run (stale chunks above it: none)
const RStep* g_cur;                           // the step being applied (its rfgl tuples)
int g_rfglIdx;
std::unordered_map<uint32_t, uint32_t> g_map; // host chunk uid -> local handle
struct Unpaired { uint32_t h; double at; };
std::vector<Unpaired> g_unpaired;             // chunks of a uid-mismatched step: faded after 30 s
LONG g_skipTotal, g_idleTotal;
struct { int uidEq, uidN, uidNa, step0Eq, step0N, rfglEq, rfglN, rfglExtra, rfglUnused, preMissing, unStatic, unChunk, unStream,
         byHandle, catchMax, idleUid, despawnMiss, unpairedFaded, restored, pended; uint32_t diffSeq; char diffTypes[40];
         double hookMax; } g_st;

// ---- live (ver7.1, destruction_impl.md §11). Sim thread, except g_noteQ (any thread, under g_noteLock).
bool g_live, g_host, g_synth;                 // linked (Destr_Link) / this game hosts / a local step runs (keeps its rfgl)
bool g_synthNa;                               // mode 2: a synthetic pending step ran since the last host step (its uid: n/a)
int g_sync, g_syncIni, g_profileIni, g_ownIni, g_check; // host ini: destr_sync (g_sync: this link's), destr_profile, destr_ownpack; guest: destr_check
// the stale rule (§4.2): non-empty steps and DESPAWNs recorded since the last level load (no Player while loading), and
// restore_object calls from any caller but the per-machine ones (any thread)
int g_sinceLoad;
volatile LONG g_restSinceLoad;
double g_lastEvent;                           // the last step with queued events (the crush window)
// host: the epoch being streamed (its blob id; 0 = a capture's records waiting for SendBlob), its BEGIN byte, a serial per
// epoch (first touches), kDestr packets sent and bytes of g_rec handed to ENet, census messages waiting for their offset
uint16_t g_hId;
uint8_t g_hDestr;
bool g_hFresh;                                // the current epoch's blob is a fresh capture (not a loaded buffer resent)
int g_badIdx;                                 // guest: host events dropped by Indices (a corrupt or hostile stream)
int g_gmIni = 1, g_gmApplied, g_gmMissed;     // destr_gm (7.2); guest: GM_DEAD applied / not found here
std::vector<RGmDead> g_stepGm;                 // host: GM_DEADs met inside a step (Record flushes them after it)
struct GmRetry { RGmDead k; double first; };
std::vector<GmRetry> g_gmRetry;                // guest: GM_DEADs whose mover is not here yet (120 s, 256 at most)
// 7.2 re-join log (host): every record streamed since our last level load (or since the link), and for each stored save
// buffer's bytes (Fnv) the log offset when they were written. A resend of a buffer with an offset gets the log from there,
// so the guest replays what that save lacks. ponytail: no trim below the oldest offset; past 16 MB the log stops (stale).
std::vector<uint8_t> g_log;
size_t g_logAt;                                // g_rec bytes already copied into g_log
struct Stamp { size_t off; std::vector<uint32_t> uids; }; // a save's log offset and its runtime chunks {handle, uid}
std::unordered_map<uint32_t, Stamp> g_stamps;  // save bytes hash -> its stamp (the first time those bytes were seen)
uint32_t g_bufHash[2];                         // stored_world_state[i] bytes last seen (only a change is a new save)
bool g_logOn, g_loadStamp;                     // the log is complete since its start / our load's buffer to stamp at 0
bool(__fastcall* g_gmNext)(uint8_t*, void*, float, int, void*, void*, void*, unsigned, bool); // the original slot 34
// Safehouse repair (7.2, user choice "repair on both"): the game never saves some buildings' damage (safehouses: rebuilt at
// every load), so a fresh capture loads them whole on the guest. A fresh epoch's resync check that finds one whole there
// and damaged here repairs ours (world::restore_object queues it; the regrow runs when it is off screen). Our own repairs
// are kept out of the RESTORE stream (the guest would skip that building's events) and out of the stale rule.
std::unordered_set<uint32_t> g_mine, g_repaired; // g_mine: under g_noteLock (Note runs off the sim thread too)
std::vector<uint32_t> g_repairDone;              // under g_noteLock: our regrows that finished (a census then)
uint32_t g_hSerial, g_pkt;
size_t g_sent;
struct CenOut { size_t at; MsgCensus m; };
std::vector<CenOut> g_cenOut;
std::unordered_set<uint64_t> g_restSent;      // RESTORE (how, handle) streamed this epoch: once each
SRWLOCK g_noteLock = SRWLOCK_INIT;
std::vector<RRestore> g_noteQ;
std::map<uint32_t, int> g_restoreFrom, g_restoreShown; // restore_object callers (rfg+RVA) -> count (both roles, 5 s line)
int g_hMatch, g_hDiff, g_hNv;
// guest: the epoch (0 = none: its packets are dropped), its byte, the last packet, the local time of the last applied STEP
uint16_t g_gEpoch;
uint16_t g_brokeEpoch;                        // its stream broke before it landed: the re-join is asked once it has
uint8_t g_gDestr;
uint32_t g_gPkt, g_priv = kPriv;
double g_lastAt;
int g_drain;                                  // STEPs waiting when this game resumed (C1 step 5): "drained" once applied
std::unordered_set<uint32_t> g_restored;      // movers the host restored or regrew: their events are skipped (M9 applies)
std::unordered_map<uint32_t, const char*> g_lost; // buildings that lost host events this epoch -> why (census: no verdict)
std::unordered_map<uint32_t, uint32_t> g_evSeq;   // building (host handle) -> seq of the last host STEP naming it
struct Pend { uint32_t h; double at; REvent ev; }; // a host event on a mover not streamed here (h): waits up to 60 s
std::vector<Pend> g_pend;
struct Retry { MsgCensus c; double first, next; }; // a live census of a building not streamed here: every 2 s for 60 s
std::vector<Retry> g_retry;
std::unordered_map<uint32_t, double> g_echo;  // mover -> when this guest's own hit on it was dropped (echo timing)
uint8_t g_own[64 * kEv];                      // own-pack: this guest's own type-6 cutters, applied apart (Q26)
struct { int match, diff, notLoaded, noVerdict, partEq; } g_rc; // the resync check being received
int g_gMatch, g_gDiff, g_gNv;
std::unordered_set<uint32_t> g_drift;         // live census: the buildings that read DIFF this epoch (the mode-0 hint)
bool g_sane, g_partOnce;                      // once per session: the sanity line, the first resync part line
// both: the 5 s window (destr: line)
struct { uint32_t pkts, census, restOut, ownPack, pendDropped, dropped, driving, staleIn, synth; uint64_t raw, est, estRaw;
         size_t maxStep; std::vector<float> lag, echo; } g_ls;
std::map<uint32_t, int> g_crush, g_crushShown; // dev: {victim, attacker, damage type} -> count while an event is < 10 s old
struct { bool on, wait; float from[3], dist; double end, echo; } g_charge[2]; // Q26: own / the partner puppet's Rhino charge
int GMode() { return g_gDestr & kModeMask; }
bool Full() { return !g_live || GMode() == 2; } // E4 and live mode 2: reseeded uids, host rfgl tuples, DESPAWNs
uint32_t Below(uint32_t u) { return u < kPriv ? u : 0; } // the private range never raises the stale-chunk walk

uint8_t* World() { return Rva<uint8_t>(rva::World); }
uint8_t* Me() { return *Rva<uint8_t*>(rva::Player); }
uint8_t* Obj(uint32_t h) { return h == ~0u ? nullptr : Fn<uint8_t* __thiscall(void*, unsigned)>(site::ObjFromHandle)(World(), h); }
bool Mover(const uint8_t* o) { return o && Field<int8_t>(o, 0x7E) == 2 && Field<int8_t>(o, 0x7F) == 1; } // rfg_mover
uint8_t* Shape(uint8_t* m) { return Field<uint8_t>(m, 0x154) == 1 ? Field<uint8_t*>(m, 0x14C) : nullptr; } // get_shape 0x1FB340
uint32_t Uid(const uint8_t* m) { return Field<uint32_t>(m, 0x134); } // destruction_uid: -1 for movers a zone or blob loaded
uint32_t Handle(const uint8_t* o) { return Field<uint32_t>(o, 0x6C); }
uint32_t TagOf(uint32_t h) { uint8_t* m = Obj(h); return Mover(m) ? Uid(m) : kNotMover; }
void Lock(bool on) { Fn<void __thiscall(void*)>(on ? site::LockEnter : site::LockLeave)(Rva(site::Lock)); }
void Hud(const char* t) { if (GameState() == 1 && Me()) Fn<int __cdecl(char*, float, void*, void*, int*)>(site::HudMessageAdd)((char*)t, 4.0f, nullptr, nullptr, nullptr); }
void Fade(uint8_t* m, int ms) { Fn<void __cdecl(void*, int)>(site::MarkForFade)(m, ms); }
template <class F> void ForMovers(F f) { // world all_objects (+0x64 elt, +0x6C num), world.cpp's F12 walk: every rfg_mover
    uint8_t** all = Field<uint8_t**>(World(), 0x64);
    for (int i = 0, n = Field<int>(World(), 0x6C); i < n; i++) if (Mover(all[i])) f(all[i]);
}
template <class T> void Put(const T& v) { g_rec.insert(g_rec.end(), (const uint8_t*)&v, (const uint8_t*)&v + sizeof v); }
size_t StepSize(const RStep& s) { return sizeof s + s.nEvents * sizeof(REvent) + s.nRfgl * sizeof(RRfgl) + s.nHash * sizeof(RHash); }
size_t RecSize(const uint8_t* r) { // the header must be there (Whole)
    switch (r[0]) {
    case kStep: return StepSize(*(const RStep*)r);
    case kDespawn: return sizeof(RDespawn);
    case kGmDead: return sizeof(RGmDead);
    case kRestore: return sizeof(RRestore);
    case kUids: return sizeof(RUids) + 8 * (size_t)((const RUids*)r)->n;
    default: return 1 + sizeof(MsgCensus); // kCensusRec
    }
}
size_t Next(size_t at) { return at + RecSize(&g_rec[at]); }
// Guest (received bytes, never trusted): 1 = a whole record at `at`, 0 = its bytes are not all here yet, -1 = corrupt
int Whole(size_t at) {
    const uint8_t* r = &g_rec[at];
    size_t left = g_rec.size() - at, head = r[0] == kStep ? sizeof(RStep) : r[0] == kUids ? sizeof(RUids) : 1;
    if (r[0] != kStep && r[0] != kDespawn && r[0] != kGmDead && r[0] != kRestore && r[0] != kUids && r[0] != kCensusRec) return -1;
    if (left < head) return 0;
    if (r[0] == kStep && (((const RStep*)r)->nEvents > 1024 || ((const RStep*)r)->nHash > 64)) return -1;
    if (r[0] == kUids && ((const RUids*)r)->n > (1u << 20)) return -1;
    return left >= RecSize(r);
}
const char* Tag() { return g_live ? "destr" : "e4"; } // log prefix of the shared replay lines
const REvent* Events(const RStep& s) { return (const REvent*)(&s + 1); }
const RRfgl* Rfgl(const RStep& s) { return (const RRfgl*)(Events(s) + s.nEvents); }
const RHash* Hashes(const RStep& s) { return (const RHash*)(Rfgl(s) + s.nRfgl); }
void TypesOf(const RStep& s, char (&out)[40]) { // the step's event types, for the divergence lines
    int n = snprintf(out, sizeof out, "%s", s.nEvents ? "" : "none");
    for (int k = 0; k < s.nEvents && n < (int)sizeof out - 4; k++)
        n += snprintf(out + n, sizeof out - n, k ? " %u" : "%u", Field<uint32_t>(Events(s)[k].prefix, 0xB0));
}

// ---- measurement (m6_plan §4.1.3): reads only --------------------------------------------------------------------------
// Fingerprint (labels false) or census of rfg_mover h. Subpiece i (0x24 at shape+0x24) is alive for shape S as
// rfg_destroyable_shape::get_shape_state 0x31AC20 tests it: flags +0x18 without 0x100000, owner offset +8 not 0 / -1,
// (sp+8)+[sp+8] == S. n = [[S+0x3C]+0xC] (num_objects; get_rfg_mover_subpiece_info 0x59870F). Census labels: STATIC
// 0xFFFE, GONE 0xFFFF, ORPHAN (the owner is no live, non-fading shape on this subpiece buffer) also 0xFFFF but counted
// apart, else the lowest index with that owner (owners compared, never dereferenced; chunk shapes share the building's
// buffer, rfg_shape_new 0x33CCF0). Fade-invariant: a fading chunk (mark_for_fade sets +0xE0 |= 0x800, 0x6139C8) hashes
// as it will once destroyed, so a census taken during a fade (the 60 s cap) matches one taken after it.
Cen Census(uint32_t h, bool labels) {
    Cen c{};
    uint8_t* m = Obj(h);
    uint8_t* S = Mover(m) ? Shape(m) : nullptr;
    uint8_t* base = S ? Field<uint8_t*>(S, 0x3C) : nullptr; // rfg_base_destroyable_shape
    uint8_t* objs = S ? Field<uint8_t*>(S, 0x24) : nullptr;
    int n = base ? Field<int>(base, 0xC) : -1;
    if (!objs || n < 0 || n > 0xFFF0) return c;
    std::vector<uint8_t> bits((n + 7) / 8);
    std::vector<uint16_t> lab(labels ? n : 0);
    std::vector<uintptr_t> fam;
    std::unordered_map<uintptr_t, uint16_t> first;
    if (labels)
        ForMovers([&](uint8_t* o) {
            if (uint8_t* s = Shape(o); s && s != S && !(Field<uint32_t>(o, 0xE0) & 0x800) && Field<uint8_t*>(s, 0x24) == objs) fam.push_back((uintptr_t)s);
        });
    for (int i = 0; i < n; i++) {
        uint8_t* sp = objs + 0x24 * i;
        int off = Field<int>(sp, 8);
        uintptr_t owner = (uintptr_t)sp + 8 + off;
        bool gone = (Field<uint32_t>(sp, 0x18) & 0x100000) || off == 0 || off == -1;
        if (!gone && owner == (uintptr_t)S) {
            bits[i >> 3] |= 1 << (i & 7), c.alive++;
            if (labels) lab[i] = 0xFFFE;
        } else if (labels) {
            if (gone) lab[i] = 0xFFFF, c.gone++;
            else if (std::find(fam.begin(), fam.end(), owner) == fam.end()) lab[i] = 0xFFFF, c.orphan++;
            else { auto [it, fresh] = first.try_emplace(owner, (uint16_t)i); lab[i] = it->second, c.chunks += fresh; }
        }
    }
    c.ok = true, c.n = n, c.hs = Fnv(bits.data(), (int)bits.size());
    if (labels) c.hp = Fnv((const uint8_t*)lab.data(), 2 * n), c.dmg = Field<float>(m, 0xFC), c.destroyed = Field<uint32_t>(m, 0x10C) & 0x10;
    return c;
}
RHash Hash(uint32_t h, bool first) { Cen c = Census(h, false); return {h, c.hs, (uint16_t)(c.ok ? c.alive : 0xFFFF), first, 0}; }

// ---- record (the host side, m6_plan §4.1.1) ------------------------------------------------------------------------
// Pointer probe: prefix dwords inside the exe image (the handles +0x50 / +0xA8 / +0xC4 are remapped, not counted). On
// one PC both games share the image base, so this is the only one-PC sign of a static pointer in a prefix.
void Probe(const uint8_t* p, uint32_t t) {
    for (int o = 0; o < kPrefix; o += 4) {
        uint32_t v = Field<uint32_t>(p, o);
        if (o == 0x50 || o == 0xA8 || o == 0xC4 || v - g_base >= kImageSize) continue;
        g_rs.ptrN++, g_rs.ptr[t][o / 4]++, g_rs.ptrEx[t][o / 4] = v;
    }
}

// dt = ms since the last recorded STEP's hostMs (empty steps are not recorded): the replay applies it that late again
void PutDespawn(RDespawn d) {
    d.dt = g_rs.steps ? (uint16_t)std::min<uint32_t>((uint32_t)(uint64_t)NowMs() - g_rs.stepMs, 65535) : 0;
    if (Room(sizeof d)) Put(d);
}
void PutGm(const RGmDead& k);
void LogOff(const char* why);
void FlushDespawns() {
    for (const RDespawn& d : g_stepDespawns) PutDespawn(d);
    for (const RGmDead& k : g_stepGm) PutGm(k);
    g_stepDespawns.clear(), g_stepGm.clear();
}

// Host: RESTORE records from the restore log (any thread), behind the steps before them. Once per (how, handle) and
// epoch: C0 showed one mover's regrow begin every frame (4880 in a scenario); the guest marks a mover once anyway.
void DrainNotes() {
    std::vector<RRestore> q;
    std::vector<uint32_t> done;
    AcquireSRWLockExclusive(&g_noteLock);
    q.swap(g_noteQ), done.swap(g_repairDone);
    ReleaseSRWLockExclusive(&g_noteLock);
    bool out = (g_hDestr & kModeMask) || g_logOn; // stale and mode-0 epochs too: the re-join log needs every restore
    for (uint32_t h : done) { // our repair is whole again: the guest unmarks it, then a census (its copy is whole)
        if (out && Room(sizeof(RRestore))) Put(RRestore{kRestore, 3, 0, h});
        if (Bld* b = FindBld(h)) b->due = true, b->last = NowMs() - 10000, Log("destr: %08X repaired here (regrow done): census", h);
    }
    for (const RRestore& r : q)
        if (out && g_restSent.insert((uint64_t)r.how << 32 | r.handle).second && Room(sizeof r)) Put(r), g_ls.restOut++;
}

// Live host: a building to track (the newest 64; past that the stalest one with no census due makes room)
Bld* AddBld(uint32_t h) {
    if (g_bld.size() >= 64) {
        auto it = std::min_element(g_bld.begin(), g_bld.end(), [](const Bld& a, const Bld& b) { return a.due < b.due || (a.due == b.due && a.last < b.last); });
        if (it->due) return nullptr; // 64 buildings with a census due: this one gets none
        g_bld.erase(it);
    }
    g_bld.push_back({h});
    memcpy(g_bld.back().pos, &Field<float>(Obj(h), 0x04), sizeof(float[3]));
    return &g_bld.back();
}

void Record() {
    if (g_live) DrainNotes();
    // one step at its largest (count clamped to 1024, nRfgl to 255, hashes 32 + 32): nothing below reallocates
    if (!Room(sizeof(RStep) + 1024 * sizeof(REvent) + 255 * sizeof(RRfgl) + 64 * sizeof(RHash))) return g_stepNext();
    uint8_t* q = Rva<uint8_t>(site::Queue);
    int count = std::clamp(*Rva<int>(site::Count), 0, 1024);
    size_t at = g_rec.size();
    RStep s{kStep};
    s.seq = g_seq, s.hostMs = (uint32_t)(uint64_t)NowMs(); // via 64 bits: wraps after 2^32 ms of uptime (a plain cast is UB)
    Put(s); // the header is written again below
    // owner 2: the host's puppet of the guest. Own-pack (Q26): its type-6 cutters (shape_cutter::do_cut 0x328657) break the
    // guest's copy there by themselves, so they stay off the stream and their building gets no verdict
    uint32_t pup = g_live ? Puppet_Handle() : ~0u, ownTg[32];
    bool own = (g_hDestr & kOwnPack) && pup != ~0u;
    int nOwn = 0;
    Lock(true);
    for (int i = 0; i < count; i++) {
        uint8_t* e = q + i * kEv;
        uint32_t t = Field<uint32_t>(e, 0xB0);
        if (e[0xC8] & 3) continue; // the apply skips it too (0x3407B0)
        // 8 (DET_MP_DESTROY: NULL mp_info at 0x31C19C), 9 (MP vehicle), 11 (+0xCC invulnerable handles): never; e4_types bisects
        if (t > 13 || t == 8 || t == 9 || t == 11 || !(g_types >> t & 1)) { g_rs.filtered++; continue; }
        if (own && t == 6 && Field<uint32_t>(e, 0xC4) == pup) {
            if (nOwn < 32) ownTg[nOwn++] = Field<uint32_t>(e, 0x50);
            g_ls.ownPack++;
            continue;
        }
        g_rec.resize(g_rec.size() + sizeof(REvent));
        uint8_t* p = &g_rec[g_rec.size() - kPrefix];
        memcpy(p, e, kPrefix);
        // types 0 and 3-6 never read the +0xCC union: stale type-1 {shapekey, damage} pairs there only vary the stream
        // (m6_ptrlike §4; type 13 keeps its axis there)
        if (t == 0 || (t >= 3 && t <= 6)) memset(p + 0xCC, 0, kPrefix - 0xCC);
        s.nEvents++;
    }
    Lock(false);
    uint32_t me = Me() ? Handle(Me()) : ~0u;
    std::vector<uint32_t> tg; // the FIXED rfg_movers (move_type +0xF8 == 0) the step targets, at most 32
    REvent* ev = (REvent*)(g_rec.data() + at + sizeof s);
    for (int k = 0; k < s.nEvents; k++) {
        uint8_t* p = ev[k].prefix;
        uint32_t t = Field<uint32_t>(p, 0xB0), o = Field<uint32_t>(p, 0xC4);
        ev[k].uidA = TagOf(Field<uint32_t>(p, 0x50)), ev[k].uidB = t == 4 ? TagOf(Field<uint32_t>(p, 0xA8)) : kNotMover;
        ev[k].owner = o == ~0u ? 0 : o == me ? 1 : o == pup ? 2 : 0;
        g_rs.types[t]++;
        Probe(p, t);
        for (int off : {0x50, 0xA8}) {
            uint8_t* m = off == 0x50 || t == 4 ? Obj(Field<uint32_t>(p, off)) : nullptr;
            if (Mover(m) && !Field<int>(m, 0xF8) && tg.size() < 32 && std::find(tg.begin(), tg.end(), Handle(m)) == tg.end())
                tg.push_back(Handle(m));
        }
    }
    uint32_t& uid = *Rva<uint32_t>(rva::DestrUid);
    s.uidBefore = uid;
    std::vector<RHash> hs;
    double now = NowMs();
    // first touch: the building before this step (the replay's step-0 check; live: once per epoch); censused later
    for (uint32_t h : tg) {
        Bld* b = FindBld(h);
        bool first = !b;
        if (!b && (g_live || g_bld.size() < 64)) b = g_live ? AddBld(h) : &g_bld.emplace_back(Bld{h});
        if (!b) continue;
        if (first && !g_live) memcpy(b->pos, &Field<float>(Obj(h), 0x04), sizeof(float[3]));
        if (g_live) first = b->ep != g_hSerial, b->ep = g_hSerial, b->last = now, b->due = true;
        if (first) hs.push_back(Hash(h, true));
    }
    for (int i = 0; i < nOwn; i++) // own-pack buildings: censused with "no verdict (own-pack)"
        if (uint8_t* m = Obj(ownTg[i]); Mover(m) && !Field<int>(m, 0xF8))
            if (Bld* b = FindBld(ownTg[i]) ? FindBld(ownTg[i]) : AddBld(ownTg[i])) b->ownEp = g_hSerial, b->last = now, b->due = true;
    // the cutters left out still run here, so this step's uids and rfgl tuples include theirs: flags bit 0 tells the guest
    // to leave all of its chunks unpaired and keep its own tuples, and every building it targets gets no verdict
    if (nOwn) {
        if (g_logOn) LogOff("the guest's own-pack cuts are not in the stream");
        s.flags |= 1;
        for (uint32_t h : tg) if (Bld* b = FindBld(h)) b->ownEp = g_hSerial;
    }
    g_rfgl.clear();
    g_inStep = true;
    g_stepNext(); // OnRfgl appends during it
    g_inStep = false;
    s.uidAfter = uid;
    for (uint32_t h : tg) hs.push_back(Hash(h, false));
    if (g_rfgl.size() > 255) g_rs.rfglCut += (int)g_rfgl.size() - 255;
    s.nRfgl = (uint8_t)std::min<size_t>(g_rfgl.size(), 255), s.nHash = (uint16_t)hs.size();
    if (!s.nEvents && !s.nRfgl && s.uidAfter == s.uidBefore) return g_rec.resize(at), FlushDespawns(); // empty steps cost nothing
    g_rec.insert(g_rec.end(), (const uint8_t*)g_rfgl.data(), (const uint8_t*)(g_rfgl.data() + s.nRfgl));
    g_rec.insert(g_rec.end(), (const uint8_t*)hs.data(), (const uint8_t*)(hs.data() + hs.size()));
    memcpy(&g_rec[at], &s, sizeof s);
    g_rs.steps++, g_rs.events += s.nEvents, g_rs.lastRec = NowMs(), g_rs.stepMs = s.hostMs, g_sinceLoad++;
    g_ls.maxStep = std::max(g_ls.maxStep, g_rec.size() - at);
    FlushDespawns(); // after stepMs: this step's own DESPAWNs get dt ~0
}

// ---- replay (the guest path, m6_plan §4.1.2) -----------------------------------------------------------------------
uint8_t* Mapped(uint32_t u) { // the host's chunk u here; nullptr once it died (a faded entry stays) or its handle was reused
    auto it = g_map.find(u);
    uint8_t* m = it != g_map.end() ? Obj(it->second) : nullptr;
    return Mover(m) && Uid(m) == u ? m : nullptr;
}

// +0x50 / +0xA8 of a written entry: the host's chunk uid through g_map, else (a chunk from before the capture, which the
// blob loads with uid -1) its handle; a loaded rfg_mover (uid -1 on both games) by its handle. false = skip the event;
// live: wait = the mover it waits for (not streamed here: the pending queue).
bool Target(uint8_t* e, int off, uint32_t u, uint32_t& wait) {
    uint32_t& h = Field<uint32_t>(e, off);
    if (u == kNotMover) return h = ~0u, true; // the host's apply ignored it as well; -1, so a recycled handle hits nothing
    if (g_live && GMode() == 1 && u != kLoaded) return g_st.unChunk++, false; // mode 1: chunks are local, counted only
    uint8_t* m = nullptr;
    if (u != kLoaded) {
        if (!(m = Mapped(u))) {
            if (!Mover(m = Obj(h)) || Uid(m) != kLoaded) return g_st.unChunk++, false;
            g_st.byHandle++;
        }
    } else if (!Mover(m = Obj(h)) || Uid(m) != kLoaded) return g_st.unStatic++, false;
    if (g_live && g_restored.count(Handle(m))) return g_st.restored++, false; // restored on the host: M9 applies it
    if (Field<uint8_t>(m, 0x154) != 1) { // not streamed in: E4, that building gets no verdict in this run; live, it waits
        if (g_live) return wait = Handle(m), false;
        if (Bld* b = FindBld(Handle(m))) b->unstreamed++;
        return g_st.unStream++, false;
    }
    h = Handle(m);
    return true;
}

// live: every loaded building a host event names, by its host handle (the same here); object_complex damages both sides
// of a type 4 (0x33A384 / 0x33A3D1)
template <class F> void ForLoaded(const REvent& r, F f) {
    for (int i = 0; i < (Field<uint32_t>(r.prefix, 0xB0) == 4 ? 2 : 1); i++)
        if ((i ? r.uidB : r.uidA) == kLoaded) f(Field<uint32_t>(r.prefix, i ? 0xA8 : 0x50));
}
bool Names(const REvent& r, uint32_t h) { bool n = false; ForLoaded(r, [&](uint32_t x) { n |= x == h; }); return n; }
// live: a host event dropped whole (skipped, or a pending one dropped): every loaded building it names lost host damage
// this epoch and its census gets no verdict (E4's rule, m6_plan §4.1.2 / §4.2)
void Lost(const REvent& r, const char* why = "host events skipped (a chunk not here, a static missing or restored)") {
    ForLoaded(r, [&](uint32_t h) { g_lost.try_emplace(h, why); });
}

// One host event into queue entry e, its handles remapped (owner 1: E4 this Player, live the guest's puppet of the host;
// 2: this Player). The tail +0xEC..+0x10F zeroed (+0xF8 0: the post pass skips it, 0x340968); bits 3/4 of +0xC8 are
// apply inputs (0x33A213). false = skipped (+0xC8 |= 3). Types 8, 9 and 11 are dropped on both ends (a code rule).
// wait already set (Apply: a building with older pending events): held, skipped here with no target looked up.
// 7.2 trust boundary (the host's bytes reach the game's own arrays; m6_ptrlike.md §1): the apply functions index subpieces
// (stride 0x24) and links with these fields unchecked: rfg_dyn_apply_stress_to_subpiece 0x332F90 (types 1, 2), the link /
// subpiece loop of rfg_dyn_apply_link_broken 0x333940 (12, writes flags at 0x333994), the slice axis 0x3345E4 (13). The
// +0xCC union holds 0x20 bytes before the real pointers at +0xEC, so a genuine event never has more entries than these caps.
// Counts against the target's rfg_base_destroyable_shape: num_objects +0xC, num_links +0x14 (PDB). 0xFFFF unpacks to -1,
// which both applies index unchecked (0x333820, 0x333A86): rejected.
bool Indices(const uint8_t* e, uint32_t t) {
    if (t != 1 && t != 2 && t != 12 && t != 13) return true;
    if (t == 13) return Field<uint32_t>(e, 0xCC) < 3;
    uint8_t* m = Obj(Field<uint32_t>(e, 0x50));
    uint8_t* S = Mover(m) ? Shape(m) : nullptr;
    uint8_t* base = S ? Field<uint8_t*>(S, 0x3C) : nullptr;
    int c = e[0xC9];
    if (c > (t == 2 ? 16 : 8)) return false;
    if (!base) return true; // no destroyable shape: each apply returns before any index (0x332F84, 0x333917)
    uint32_t n = (uint32_t)Field<int>(base, 0xC), links = (uint32_t)Field<int>(base, 0x14);
    for (int i = 0; i < c; i++) {
        uint32_t k = t == 2 ? Field<uint16_t>(e, 0xCC + 2 * i) : t == 1 ? Field<uint16_t>(e, 0xCC + 4 * i) : Field<uint32_t>(e, 0xCC + 4 * i);
        if (k >= (t == 12 && !(e[0xC8] & 0x20) ? links : n)) return false;
    }
    return true;
}

bool Write(uint8_t* e, const REvent& r, uint32_t host, uint32_t me, uint32_t& wait) {
    memcpy(e, r.prefix, kPrefix);
    memset(e + kPrefix, 0, kEv - kPrefix);
    e[0xC8] &= ~7;
    uint32_t t = Field<uint32_t>(e, 0xB0);
    // mode 1 drops every chunk-vs-building impact (chunks are local; object_complex needs both sides, 0x33A2D3): that
    // building lost host debris damage this epoch (B's own debris is dropped too, Replay), so its census has no verdict
    if (g_live && GMode() == 1 && t == 4 && (r.uidA < kNotMover || r.uidB < kNotMover)) Lost(r, "debris local (mode 1)");
    bool ok = wait == ~0u && t <= 13 && t != 8 && t != 9 && t != 11 && Target(e, 0x50, r.uidA, wait) &&
              (t != 4 || Target(e, 0xA8, r.uidB, wait));
    if (ok && !Indices(e, t)) { // never into the game: dropped and counted (a corrupt or hostile host)
        ok = false;
        if (++g_badIdx <= 3) Log("%s: a host event of type %u with an index outside its building dropped (#%d)", Tag(), t, g_badIdx);
    }
    Field<uint32_t>(e, 0xC4) = r.owner == 1 ? host : r.owner == 2 ? me : ~0u;
    if (!ok) e[0xC8] |= 3;
    return ok;
}

// The step's chunks: uids (lo, hi] minus `stale` (chunks that held those uids before the step). Paired when there are
// as many as the host made, else unpaired (faded later). true = paired.
bool MapUids(uint32_t lo, uint32_t hi, uint32_t host, const std::vector<uint32_t>& stale) {
    std::vector<uint8_t*> mine;
    ForMovers([&](uint8_t* o) {
        if (uint32_t u = Uid(o); u > lo && u <= hi && std::find(stale.begin(), stale.end(), Handle(o)) == stale.end()) mine.push_back(o);
    });
    bool pair = hi - lo == host && mine.size() == host;
    for (uint8_t* o : mine) {
        uint32_t h = Handle(o);
        if (!pair) { g_unpaired.push_back({h, NowMs()}); continue; }
        g_map[Uid(o)] = h;
        std::erase_if(g_unpaired, [&](const Unpaired& x) { return x.h == h; }); // a reused handle is not faded later
    }
    return pair;
}

void Compare(const RStep& s, const RHash& r) { // a targeted building after the step: the first divergence line
    Bld* b = FindBld(r.handle);
    if (!b) return;
    Cen c = Census(r.handle, false);
    if (c.ok && c.hs == r.hStatic && c.alive == r.alive) {
        b->matched = true, b->notesAt = Notes(), b->skipAt = g_skipTotal, b->idleAt = g_idleTotal;
    } else if (b->matched) {
        b->matched = false;
        char ty[40];
        TypesOf(s, ty);
        if (b->lines++ < 20)
            Log("%s step %u: %08X first divergence: hStatic %08X vs %08X, alive %u vs %d (host vs here), types %s; since the last "
                "match: restores/regrows %ld (last restore from rfg+%X), skipped events %ld, idle uid changes %ld", Tag(), s.seq,
                r.handle, r.hStatic, c.hs, r.alive, c.ok ? c.alive : -1, ty, Notes() - b->notesAt, (unsigned)g_noteFrom,
                g_skipTotal - b->skipAt, g_idleTotal - b->idleAt);
    }
}

void LiveStep0(const RStep& s, const RHash& r) { // live: the building as loaded here vs the host's at its first touch this epoch
    RHash l = Hash(r.handle, true);
    if (l.alive == 0xFFFF) return; // not streamed here: its events wait in the pending queue
    Bld* b = FindBld(r.handle);
    if (!b) {
        if (g_bld.size() >= 64) g_bld.erase(g_bld.begin()); // the oldest first-touched building makes room
        b = &g_bld.emplace_back(Bld{r.handle});
    }
    bool eq = l.alive == r.alive && l.hStatic == r.hStatic;
    g_st.step0N++, g_st.step0Eq += eq;
    b->matched = eq, b->notesAt = Notes(), b->skipAt = g_skipTotal, b->idleAt = g_idleTotal;
    if (!eq && b->lines++ < 20)
        Log("destr step %u: step0 %08X DIFF: hStatic %08X vs %08X, alive %u vs %u (host vs here): this building was not as the "
            "host's when its first event of this epoch came", s.seq, r.handle, r.hStatic, l.hStatic, r.alive, l.alive);
}

void Wait(uint32_t h, const REvent& ev) { // live: a host event on a mover not streamed here waits (m6_risks R8)
    if (g_pend.size() < 4096) g_pend.push_back({h, NowMs(), ev}), g_st.pended++;
    else g_ls.pendDropped++, Lost(ev, "host events dropped (pending queue full)"); // every building it names, not only h
}

void Echo(uint32_t h) { // a host step with owner 2 applied on mover h: the first one after this guest's own hit there
    auto it = g_echo.find(h);
    if (it == g_echo.end()) return;
    double ms = NowMs() - it->second;
    g_ls.echo.push_back((float)ms), g_echo.erase(it);
    if (g_charge[0].wait) g_charge[0].wait = false, g_charge[0].echo = ms; // Q26: the first echo of an own Rhino charge
}

void Apply() {
    const RStep& s = *(const RStep*)&g_rec[g_rd];
    const REvent* ev = Events(s);
    uint8_t* q = Rva<uint8_t>(site::Queue);
    uint32_t me = Me() ? Handle(Me()) : ~0u, host = g_live ? Puppet_Handle() : me; // E4: host and guest are this Player
    bool full = Full();
    LONG sk0 = g_skipTotal;
    std::unordered_set<uint32_t> named; // the buildings g_pend names: built once, at this step's first loaded side
    bool idx = false;
    Lock(true);
    for (int k = 0; k < s.nEvents; k++) { // from index 0: the 1024 cap (0x339C5A) never drops a host event
        uint8_t* e = q + k * kEv;
        uint32_t wait = ~0u;
        // applied or pended alike: a census taken before this step and judged after it (a retry) has no verdict. Per-building
        // order (§4.2): an event naming a building that older events still wait for (either side of a type 4) waits behind
        // them, even once the building has streamed in (RunPending runs them in order right after this step)
        if (g_live) ForLoaded(ev[k], [&](uint32_t h) {
            g_evSeq[h] = s.seq;
            if (g_pend.empty()) return;
            if (!idx) {
                idx = true;
                for (const Pend& p : g_pend) ForLoaded(p.ev, [&](uint32_t x) { named.insert(x); });
            }
            if (named.count(h)) wait = h;
        });
        if (Write(e, ev[k], host, me, wait)) {
            if (ev[k].owner == 2) Echo(Field<uint32_t>(e, 0x50));
            continue;
        }
        InterlockedIncrement(&g_skipTotal);
        if (wait != ~0u) {
            size_t n0 = g_pend.size();
            Wait(wait, ev[k]);
            if (idx && g_pend.size() > n0) ForLoaded(ev[k], [&](uint32_t x) { named.insert(x); }); // queued: later events wait too
        }
        if (g_live) { if (wait == ~0u) Lost(ev[k]); continue; } // waiting: Wait / the 60 s purge mark it if dropped
        // the whole event is dropped, and object_complex damages both sides (0x33A384 / 0x33A3D1): a building named on
        // either side (by its host handle, the same here) lost host damage and gets no verdict in this run
        for (int off : {0x50, 0xA8})
            if (off == 0x50 || Field<uint32_t>(e, 0xB0) == 4)
                if (Bld* b = FindBld(Field<uint32_t>(ev[k].prefix, off))) b->skipped++;
    }
    *Rva<int>(site::Count) = s.nEvents;
    Lock(false);
    // step-0 check (§4.1.3): the host's first-touch fingerprint vs this building right before the step. Not at run start:
    // a step can change a building it does not target (rfg_mover_delete_dynamic_link 0x32EA46 queues the linked shape
    // for shardify, 0x3247B6 splits its loose subpieces). A DIFF while the building was as the host's at the feed start
    // (r0) is a divergence before its first touch (a static DIFF in EndRun); any other DIFF is a blob or handle bug and
    // makes the run INVALID.
    for (int k = 0; k < s.nHash; k++) {
        const RHash& r = Hashes(s)[k];
        if (g_live) { // no reference here: each first touch of an epoch is checked against the host's (destr_check)
            if (r.firstTouch && r.alive != 0xFFFF && g_check) LiveStep0(s, r);
            continue;
        }
        Bld* b = FindBld(r.handle);
        if (!r.firstTouch || r.alive == 0xFFFF || !b || !b->ref.ok) continue;
        RHash l = Hash(r.handle, true);
        if (l.alive == 0xFFFF) { Log("e4 step %u: step0 %08X not loaded", s.seq, r.handle); continue; }
        bool eq = l.alive == r.alive && l.hStatic == r.hStatic;
        if (!eq && b->r0.alive == r.alive && b->r0.hStatic == r.hStatic) {
            b->preDiff = true, b->matched = false;
            if (b->lines++ < 20)
                Log("e4 step %u: %08X first divergence before its first touch: hStatic %08X vs %08X, alive %u vs %u (host vs here); "
                    "since the feed start: restores/regrows %ld (last restore from rfg+%X), skipped events %ld, idle uid changes %ld",
                    s.seq, r.handle, r.hStatic, l.hStatic, r.alive, l.alive, Notes() - b->notesAt, (unsigned)g_noteFrom,
                    g_skipTotal - b->skipAt, g_idleTotal - b->idleAt);
            continue;
        }
        g_st.step0N++, g_st.step0Eq += eq;
        b->matched = eq, b->notesAt = Notes(), b->skipAt = g_skipTotal, b->idleAt = g_idleTotal;
        if (!eq) Log("e4 step %u: step0 %08X DIFF: hStatic %08X vs %08X, alive %u vs %u (host vs here; 65535 = no shape)", s.seq,
                     r.handle, r.hStatic, l.hStatic, r.alive, l.alive);
    }
    uint32_t& uid = *Rva<uint32_t>(rva::DestrUid);
    // Chunks above the host's counter (a local > host step's, or the idle path's) hold uids the reseed hands out again:
    // they never pair. Walked only while the counter has been above the host's. Mode 1: chunks are local, no reseed.
    std::vector<uint32_t> stale;
    if (full) {
        g_uidHigh = std::max(g_uidHigh, Below(uid));
        if (g_uidHigh > s.uidBefore)
            ForMovers([&](uint8_t* o) { if (Uid(o) > s.uidBefore && Uid(o) <= g_uidHigh) stale.push_back(Handle(o)); });
        uid = s.uidBefore; // reseeded per step: a mismatch does not spread
    }
    int extra0 = g_st.rfglExtra;
    g_cur = &s, g_rfglIdx = 0, g_inStep = true, g_lastApplied = s.hostMs;
    g_stepNext();
    g_inStep = false, g_cur = nullptr;
    if (g_live) g_lastAt = NowMs(), g_ls.lag.push_back((float)(uint32_t)((uint32_t)(uint64_t)g_lastAt - s.hostMs)); // one QPC clock on one PC
    bool synth = std::exchange(g_synthNa, false);
    if (full) {
        // shardify candidates unlike the host's (host tuples left over, or local calls past them): with a chunk of the
        // capture missing here (preMissing), the tuples shifted: no uid verdict; otherwise it is a real divergence
        // own-pack (flags bit 0): the host's step ran this guest's cutters too, which run apart here: its chunks never pair
        // (MapUids with an impossible count), its tuples were not taken (OnRfgl), no uid verdict
        bool own = s.flags & 1;
        int unused = own ? 0 : std::max(0, s.nRfgl - g_rfglIdx);
        g_st.rfglUnused += unused;
        bool shifted = g_st.preMissing && (unused || g_st.rfglExtra != extra0);
        uint32_t local = uid - s.uidBefore, made = s.uidAfter - s.uidBefore;
        g_uidHigh = std::max(g_uidHigh, Below(uid));
        bool eq = local ? MapUids(s.uidBefore, uid, own ? ~0u : made, stale) : !made;
        if (local || made) {
            // own-pack, a skipped event (unstreamed, or a chunk never made here), or a synthetic step since the last host
            // step (it may have taken shardify candidates the host takes in this one): no uid verdict
            if (own || g_skipTotal != sk0 || shifted || synth) g_st.uidNa++;
            else if (g_st.uidN++, eq) g_st.uidEq++;
            else if (!g_st.diffSeq) g_st.diffSeq = s.seq, TypesOf(s, g_st.diffTypes);
        }
    }
    for (int k = 0; k < s.nHash && (!g_live || g_check); k++)
        if (!Hashes(s)[k].firstTouch && Hashes(s)[k].alive != 0xFFFF) Compare(s, Hashes(s)[k]);
    g_rd = Next(g_rd);
    if (g_drain && !--g_drain) Log("destr: backlog drained (every STEP waiting at the resume is applied)");
}

// One more call of the original inside this callback, outside the host's steps: this guest's own-pack cutters (Q26) or a
// synthetic step of pending events (§4.2). write fills the queue from index 0 and returns the count. Mode 2: its chunks
// take private uids (never paired, faded after 30 s) and its rfgl calls keep the local values; the next host step reseeds.
// true = the original ran (n > 0).
template <class W> bool LocalStep(W write) {
    uint32_t& uid = *Rva<uint32_t>(rva::DestrUid);
    Lock(true);
    int n = write(Rva<uint8_t>(site::Queue));
    *Rva<int>(site::Count) = n;
    Lock(false);
    if (!n) return false;
    bool full = GMode() == 2;
    uint32_t lo = g_priv;
    if (full) uid = g_priv;
    g_synth = g_inStep = true;
    g_stepNext();
    g_synth = g_inStep = false;
    if (!full || uid <= lo) return true;
    double now = NowMs();
    ForMovers([&](uint8_t* o) { if (Uid(o) > lo && Uid(o) <= uid) g_unpaired.push_back({Handle(o), now}); });
    g_priv = uid;
    return true;
}

// Movers that streamed in: their waiting events as one synthetic step (Replay: mode 2 right after a host step, else
// every hook call). true = the original ran.
bool RunPending() {
    if (g_pend.empty()) return false;
    uint32_t me = Me() ? Handle(Me()) : ~0u, host = Puppet_Handle();
    // per-building order (§4.2): an entry that still waits holds every later one naming its mover or one of its buildings
    std::vector<uint32_t> held;
    auto each = [](const Pend& p, auto f) { f(p.h), ForLoaded(p.ev, f); };
    bool ran = LocalStep([&](uint8_t* q) {
        int n = 0;
        std::erase_if(g_pend, [&](const Pend& p) {
            uint8_t* m = Obj(p.h);
            if (n >= 1024) return false;
            if (!Mover(m)) return Lost(p.ev), ++g_ls.pendDropped, true; // its mover is gone (despawned): it never runs
            uint32_t wait = ~0u;
            each(p, [&](uint32_t h) { if (std::find(held.begin(), held.end(), h) != held.end()) wait = h; });
            if (wait == ~0u && Shape(m) && Write(q + n * kEv, p.ev, host, me, wait)) return n++, g_ls.synth++, true;
            if (wait != ~0u || !Shape(m)) return each(p, [&](uint32_t h) { held.push_back(h); }), false; // it waits on
            return Lost(p.ev), true; // any other miss is counted (Target) and dropped
        });
        return n;
    });
    g_synthNa |= ran && GMode() == 2;
    return ran;
}

bool Due() { // a STEP at the head the hook may apply now
    if (g_rd >= g_rec.size() || g_rec[g_rd] != kStep) return false;
    if (g_live) return Whole(g_rd) == 1; // as soon as all of it is here (the network paces it)
    return (uint32_t)(((const RStep*)&g_rec[g_rd])->hostMs - g_host0) <= NowMs() - g_feedT0; // E4: live pacing
}

void ApplyDespawn(const RDespawn& d);
void Replay() {
    uint8_t* q = Rva<uint8_t>(site::Queue);
    int& count = *Rva<int>(site::Count);
    if (g_uidLeftOk && *Rva<uint32_t>(rva::DestrUid) != g_uidLeft) g_st.idleUid++, InterlockedIncrement(&g_idleTotal);
    // Every local event dropped, and no call while no host step is due (§0.3): the original's own SP end (count 0 at
    // 0x340A2B), so the queue never fills to its cap and no later post pass walks stale entries. Live: an own hit starts
    // its echo timer; own-pack keeps this guest's own type-6 cutters (Q26), applied apart below.
    uint32_t me = Me() ? Handle(Me()) : ~0u;
    bool own = g_live && (g_gDestr & kOwnPack), driving = g_live && Me() && Field<uint32_t>(Me(), 0xAC4) != ~0u;
    int kept = 0;
    Lock(true);
    for (int i = 0, n = std::clamp(count, 0, 1024); i < n; i++) {
        uint8_t* e = q + i * kEv;
        if (g_live && !(e[0xC8] & 3)) {
            bool mine = me != ~0u && Field<uint32_t>(e, 0xC4) == me;
            if (own && mine && Field<uint32_t>(e, 0xB0) == 6 && kept < 64) memcpy(g_own + kept++ * kEv, e, kEv);
            else {
                g_ls.dropped++, g_ls.driving += driving;
                if (mine) g_echo.try_emplace(Field<uint32_t>(e, 0x50), NowMs());
            }
        }
        e[0xC8] |= 3;
    }
    count = 0;
    Lock(false);
    if (kept) { // mode 2: like a synthetic step, it may take candidates the next host step shardifies (its uid: n/a)
        LocalStep([&](uint8_t* q2) { return memcpy(q2, g_own, kept * kEv), kept; });
        g_ls.ownPack += kept, g_synthNa |= GMode() == 2;
    }
    // the synthetic pending step. Mode 2: never while no host step is due (§0.3: the original shardifies deletion-fed
    // candidates on every call, which the host takes in its next step), but right after a host step, before the DESPAWNs
    // behind it; here only for an idle host (no STEP applied for 1 s, none due). Mode 1: here, before the host steps
    bool ran = g_live && g_feeding && (GMode() != 2 || (!Due() && NowMs() - g_lastAt >= 1000)) && RunPending();
    int applied = 0;
    while (g_feeding && applied < g_catchup) {
        if (Due()) {
            Apply(), applied++;
            if (!ran && g_live && GMode() == 2) ran = RunPending();
            continue;
        }
        // live: a DESPAWN / RESTORE between STEPs is taken here as well, in stream order (after its STEP, before the next),
        // or B applies one STEP per frame whatever destr_catchup is. kUids and census records stay GuestTick's
        if (!g_live || g_rd >= g_rec.size() || Whole(g_rd) != 1) break;
        const uint8_t* r = &g_rec[g_rd];
        if (r[0] == kDespawn) { if (Full()) ApplyDespawn(*(const RDespawn*)r); }
        else if (r[0] == kRestore) g_restored.insert(((const RRestore*)r)->handle);
        else break;
        g_rd = Next(g_rd);
    }
    // mode 1 idle: the original with count 0 (its own debris), once per callback (not again after a local step)
    if (g_live && !applied && !kept && !ran && GMode() == 1) g_stepNext();
    g_st.catchMax = std::max(g_st.catchMax, applied);
    g_uidLeft = *Rva<uint32_t>(rva::DestrUid), g_uidLeftOk = g_feeding;
}

// Always before the next STEP (m6_pipeline R2). E4 and live GuestTick: the tick, outside the Havok step; live catch-up
// (Replay): also between two STEPs in the post-collide callback, where the host's own deleter fades and destroys
// (despawnsInStep)
void ApplyDespawn(const RDespawn& d) {
    uint8_t* m = d.uid != kLoaded ? Mapped(d.uid) : nullptr;
    if (!m && (!Mover(m = Obj(d.handle)) || Uid(m) != kLoaded)) return (void)g_st.despawnMiss++;
    // A fade takes the host's ms whatever is visible here: a fading chunk still collides (0x32DD7E tests +0x54 only), so
    // it lives and stays mapped as long as on the host. Only a destroy goes by the deleter's own test here.
    if (d.fade) return Fade(m, (int)d.fadeMs);
    g_map.erase(d.uid);
    if (Fn<uint32_t __cdecl(void*)>(site::IsVisible)(m) & 0xFF) Fade(m, 1000);
    else Fn<void __thiscall(void*, void*, int)>(site::DestroyObject)(World(), m, 0);
}

void FadeUnpaired() { // the deleter is off here, so chunks no DESPAWN will name fade 30 s after they appeared
    double now = NowMs();
    std::erase_if(g_unpaired, [&](const Unpaired& u) {
        if (now - u.at < 30000) return false;
        if (uint8_t* m = Obj(u.h); Mover(m) && Uid(m) != kLoaded) Fade(m, 2000), g_st.unpairedFaded++;
        return true;
    });
}

// ---- hooks -----------------------------------------------------------------------------------------------------------
void __cdecl OnStep() {
    if (!g_hookTid) g_hookTid = GetCurrentThreadId();
    g_seq++;
    if (*Rva<int>(site::Count) > 0) g_lastEvent = NowMs(); // the crush window
    if (g_mode == kOff) return g_stepNext();
    double t = NowMs();
    g_mode == kRecord ? Record() : Replay();
    g_st.hookMax = std::max(g_st.hookMax, NowMs() - t);
}

void __cdecl OnRfgl(int* out, int count, int flags) {
    g_rfglNext(out, count, flags);
    if (!g_inStep || g_synth) return; // a local step (own-pack, pending) has no host tuple: it keeps the local values
    auto s16 = [](int v) { return (int16_t)std::clamp(v, -32768, 32767); };
    RRfgl own{(uint16_t)std::clamp(count, 0, 65535), (uint16_t)flags, s16(out[0]), s16(out[1])};
    if (g_mode == kRecord) return g_rfgl.push_back(own);
    if (!Full()) return; // live mode 1: chunks are local, so are the debris tuples
    if (g_cur && (g_cur->flags & 1)) return; // an own-pack step: its tuples include the cutters run apart here (as g_synth)
    // replay: never our own values (m6_pipeline R1); only a call the host did not make keeps them
    if (!g_cur || g_rfglIdx >= g_cur->nRfgl) return (void)g_st.rfglExtra++;
    const RRfgl& h = Rfgl(*g_cur)[g_rfglIdx++];
    g_st.rfglN++, g_st.rfglEq += !memcmp(&own, &h, sizeof h);
    out[0] = std::min<int>(h.quality, count), out[1] = std::min<int>(h.del, count);
}

// qsort's comparator for the shardify candidates: rfg_mover_list_compare 0x314FF0 (uid -1 first by handle, then by uid,
// int differences as there), but on the replay a blob-loaded chunk (uid -1 here) sorts by its uid on the recording game,
// so the candidates take their rfgl tuples and new uids in the host's order. Chunks made here carry the host's uids
// (reseed). Elsewhere it is the original.
int __cdecl OnCompare(const uint32_t* a, const uint32_t* b) {
    auto uid = [](const uint32_t* x) {
        auto it = g_mode == kReplay && x[2] == kLoaded ? g_preUid.find(x[1]) : g_preUid.end();
        return it != g_preUid.end() ? it->second : x[2];
    };
    uint32_t ua = uid(a), ub = uid(b);
    if (ua == kLoaded) return ub == kLoaded ? (int)(a[1] - b[1]) : -1;
    return ub == kLoaded ? 1 : (int)(ua - ub);
}

void AddDespawn(uint8_t* m, bool fade, int ms) { // rfg_movers only: the general-mover list's shards are local (uid -1, own fade)
    if (g_mode != kRecord || !Mover(m)) return;
    g_sinceLoad++; // a resend of the loaded buffer would lose it (the stale rule)
    RDespawn d{kDespawn, fade, 0, Uid(m), Handle(m), (uint32_t)ms};
    if (g_inStep) g_stepDespawns.push_back(d), g_rs.despawnsInStep++; // Record() appends it after this STEP
    else PutDespawn(d);
    g_rs.despawns++, g_rs.lastRec = NowMs();
}
void __cdecl OnFade(uint8_t* m, int ms) {
    // mode 2 guest: no local deleter (as gc_rfglGuest), so the explosion hit's and do_debris_cleanup's fades of rfg_movers
    // are skipped (their callers clean the stack, add esp,8, and read nothing back): only the host's DESPAWN, or the 30 s
    // unpaired fade, removes a chunk. General movers (+0x7F 0) still fade here: no DESPAWN carries them (local until 7.2). The deleter's own 0x324ACC also runs inside the step (emergency cleanup): kept
    uintptr_t ra = (uintptr_t)__builtin_return_address(0) - g_base;
    if (g_live && !g_host && GMode() == 2 && Mover(m) && (ra == site::ExplFadeCall + 5 || ra == site::CleanupFadeCall + 5)) return;
    AddDespawn(m, true, ms), g_fadeNext(m, ms);
}
void __thiscall OnDestroy(void* w, uint8_t* o, int f) { AddDespawn(o, false, 0), g_destroyNext(w, o, f); }

// 7.2 GM_DEAD, host: general movers (gas tanks, props) never use the destruction queue, so their deaths travel as records.
// Slot 34 swap (every caller is virtual: xrefs has no direct call): the original, then a 0 -> 1 edge of
// general_mover::is_killed (0x612D50: +0xF8 bit 8). thiscall via fastcall: ecx = this, edx unused, 7 stack args, ret 0x1C.
// The death block of apply_damage (hp <= 0, 0x68ABC6) ORs 2 (explode) or 4 (destroyed model) into gm_flags +0xF8
// (0x68ABDB-0x68ABE7); 0x100 (is_killed 0x612D50) only comes a frame later and never for an exploding tank, and 0x800
// follows the explosion (0x68A901-0x68A90F). So "dead" = any of 0x906, the death edge = 2 or 4 appearing.
bool Dead(const uint8_t* gm) { return Field<uint32_t>(gm, 0xF8) & 0x906; }
void PutGm(const RGmDead& k) { if (Room(sizeof k)) Put(k); }
bool __fastcall OnGmDamage(uint8_t* gm, void* edx, float d, int t, void* pos, void* dir, void* src, unsigned a6, bool force) {
    bool was = Dead(gm), r = g_gmNext(gm, edx, d, t, pos, dir, src, a6, force);
    // recorded whenever this host streams mode 2 (g_sync), not only in a mode-2 epoch: the re-join log must have it
    if (!was && (Field<uint32_t>(gm, 0xF8) & 6) && g_mode == kRecord && g_live && g_sync == 2 && g_gmIni) {
        RGmDead k{kGmDead, {(uint8_t)(t >= 1 && t <= 5 ? t : 2)}, Handle(gm), {}, (uint32_t)(uint64_t)NowMs()}; // pad[0]: its type
        memcpy(k.pos, &Field<float>(gm, 0x04), sizeof k.pos);
        if (g_inStep) g_stepGm.push_back(k); // never inside the STEP being built (Record flushes it after, as DESPAWNs)
        else PutGm(k);
        g_rs.lastRec = NowMs(), g_sinceLoad++; // a resend of the loaded buffer would lose it (the stale rule)
    }
    return r;
}

// 7.2 GM_DEAD, guest (GuestTick, outside the Havok step): the same handle, a general_mover (its slot 34 is our patched
// one: only general_mover's vftable 0xF13FDC holds apply_damage), killed through that slot with force = 1, which passes
// GF-GM's swallow (0x68A939-0x68A93F), with the host's damaged_by_types (level_object_inst_properties::apply_damage
// 0x636958 changes hp for types 1-5 only), as do_debris_cleanup 0x7CD01B calls it (NULL source is safe: 0x68A9CA, 0x68ABED).
// Within 50 m of the host's spot (props are pushed before they die). true = done (killed, already dead, not a mover).
bool ApplyGmDead(const RGmDead& k) {
    uint8_t* m = Obj(k.handle);
    if (!m) return false; // not streamed here (yet): retried
    if (Field<int8_t>(m, 0x7E) != 2 || Field<uintptr_t*>(m, 0)[34] != (uintptr_t)&OnGmDamage || !std::isfinite(k.pos[0]) ||
        !std::isfinite(k.pos[1]) || !std::isfinite(k.pos[2])) {
        if (++g_gmMissed <= 3) Log("destr: GM_DEAD %08X: not a general mover here (#%d)", k.handle, g_gmMissed);
        return true;
    }
    const float* p = &Field<float>(m, 0x04);
    float dx = p[0] - k.pos[0], dy = p[1] - k.pos[1], dz = p[2] - k.pos[2];
    if (dx * dx + dy * dy + dz * dz > 2500.0f) return false; // 50 m: a reused handle elsewhere; retried (it may be on its way)
    if (Dead(m)) return true;
    static const float zero[3] = {};
    auto fn = (bool(__fastcall*)(uint8_t*, void*, float, int, void*, void*, void*, unsigned, bool))Field<uintptr_t*>(m, 0)[34];
    fn(m, nullptr, 1.0e6f, k.pad[0] >= 1 && k.pad[0] <= 5 ? k.pad[0] : 2, (void*)&Field<float>(m, 0x04), (void*)zero, nullptr,
       ~0u, true); // 1e6: the 0xE0&4 branch scales it by 200 into an int (0x68A99C)
    if (++g_gmApplied <= 5) Log("destr: gm dead applied %08X, lag %u ms", k.handle, (uint32_t)(uint64_t)NowMs() - k.hostMs);
    return true;
}

// Counted always; the first 5 of each kind logged per phase (E4 recording, each feed; a live session), then counts only
// (the e4 reference / run lines, the 5 s destr: line). A live host also streams it as RESTORE (DrainNotes).
void Note(int kind, uint32_t h, uintptr_t from) {
    static const char* const what[3] = {"restore_object", "regrow begin", "regrow finish"};
    InterlockedIncrement(&g_notes[kind]);
    if (!kind) g_noteFrom = from;
    bool mine = false;
    if (g_live) {
        AcquireSRWLockExclusive(&g_noteLock);
        if ((mine = g_host && g_mine.count(h)) && kind == 2) g_mine.erase(h), g_repairDone.push_back(h);
        if (!kind && !mine) g_restoreFrom[(uint32_t)from]++;
        if (g_host && !mine && g_noteQ.size() < 4096) g_noteQ.push_back({kRestore, (uint8_t)kind, 0, h});
        ReleaseSRWLockExclusive(&g_noteLock);
    }
    // the stale rule: every regrow starts from a restore_object (mission start's restore_list_do_restore, scripts), and
    // the anchor a mid-mission resend sends is saved before them (mission_activate_post_save 0x39D6FE). Our repairs: not.
    if (!kind && !mine && from != site::StreamRestore && from != site::KioskRestore) InterlockedIncrement(&g_restSinceLoad);
    LONG left;
    if (g_noteBudget[kind] > 0 && (left = InterlockedDecrement(&g_noteBudget[kind])) >= 0)
        Log("destr: %s %08X at step %u, called from rfg+%X%s", what[kind], h, g_seq, (unsigned)from,
            left ? "" : " (more of this kind: counts only)");
}
// regrow begin: noted only when it ran. The caller tests al (0x33A954); false = +0xE0 & 0x1000000 clear (0x735DC8) or the
// mover on screen (0x7C2800, 0x735E00), both before the request bit is cleared (0x735E04): a no-op retried every frame
// (C0: 384 begins, 0 finishes), which must not mark a live building restored on the guest.
uint32_t __cdecl OnRegrowBegin(uint8_t* m) {
    uint32_t h = Handle(m), r = g_regrowNext[0](m);
    if (r & 0xFF) Note(1, h, site::RegrowBeginCall);
    return r;
}
uint32_t __cdecl OnRegrowFinish(uint8_t* m) { Note(2, Handle(m), site::RegrowFinishCall); return g_regrowNext[1](m); }

// world::restore_object entry (6 stolen bytes `mov ecx,[Hierarchy]`, no relative operand): OnRestore(return address,
// handle), the stolen instruction, then on at +6. eax / edx are free at entry and ecx is reloaded there.
__attribute__((naked)) void RestoreThunk() {
    asm volatile("pushl 4(%esp)\n\t" // handle
                 "pushl 4(%esp)\n\t" // return address
                 "call _OnRestore\n\t"
                 "addl $8, %esp\n\t"
                 "movl _gc_hierarchy, %ecx\n\t"
                 "movl (%ecx), %ecx\n\t"
                 "jmp *_gc_restoreBack");
}

// ---- install (DLL load: no game thread runs game code yet) -------------------------------------------------------
bool Bytes(uintptr_t at, const void* want, size_t n, const char* what) {
    if (!memcmp(Rva(at), want, n)) return true;
    Log("destr: %s at %X: unexpected bytes", what, (unsigned)at);
    return false;
}
bool Call(uintptr_t at, uintptr_t fn, const char* what) { // E8 rel32 -> the game's own function (no sibling chain there)
    uint8_t* s = Rva<uint8_t>(at);
    if (s[0] == 0xE8 && (uintptr_t)(s + 5 + *(int32_t*)(s + 1)) == g_base + fn) return true;
    Log("destr: %s at %X: not the expected call to %X", what, (unsigned)at, (unsigned)fn);
    return false;
}

bool Install() {
    uint32_t mp = (uint32_t)(g_base + rva::Multiplayer), hier = (uint32_t)(g_base + site::Hierarchy); // abs32: relocated
    uint8_t cmp[7] = {0x80, 0x3D, 0, 0, 0, 0, 0x00}, restore[11] = {0x8B, 0x0D, 0, 0, 0, 0, 0x56, 0x8B, 0x74, 0x24, 0x08},
            world[5] = {0xB9}, sort[5] = {0x68};
    memcpy(cmp + 2, &mp, 4), memcpy(restore + 2, &hier, 4);
    uint32_t w = (uint32_t)(g_base + rva::World), lc = (uint32_t)(g_base + site::ListCompare);
    memcpy(world + 1, &w, 4), memcpy(sort + 1, &lc, 4);
    static const uint8_t ret8[3] = {0xC2, 0x08, 0x00}, pushEax = 0x50, pushEsi = 0x56, pushEdi = 0x57;
    bool ok = Call(site::StepCall, site::CollidePost, "step call");
    ok &= Bytes(site::StepCall + 5, ret8, 3, "step ret 8");
    ok &= Bytes(site::RfglCall - 1, &pushEax, 1, "rfgl push");
    ok &= Call(site::RfglCall, site::RfglValues, "rfgl call");
    ok &= Bytes(site::FadeCall - 1, &pushEsi, 1, "deleter push");
    ok &= Call(site::FadeCall, site::MarkForFade, "deleter fade call");
    static const uint8_t explPush[2] = {0x52, 0x56}, cleanupPush[3] = {0x6A, 0x00, 0x51};
    ok &= Bytes(site::ExplFadeCall - 2, explPush, 2, "explosion hit push");
    ok &= Call(site::ExplFadeCall, site::MarkForFade, "explosion hit fade call");
    ok &= Bytes(site::CleanupFadeCall - 3, cleanupPush, 3, "debris cleanup push");
    ok &= Call(site::CleanupFadeCall, site::MarkForFade, "debris cleanup fade call");
    ok &= Bytes(site::DestroyCall - 5, world, 5, "deleter mov ecx,World");
    ok &= Call(site::DestroyCall, site::DestroyObject, "deleter destroy call");
    ok &= Bytes(site::RegrowBeginCall - 1, &pushEdi, 1, "regrow push");
    ok &= Call(site::RegrowBeginCall, site::RegrowBegin, "regrow begin call");
    ok &= Bytes(site::RegrowFinishCall - 1, &pushEdi, 1, "regrow push");
    ok &= Call(site::RegrowFinishCall, site::RegrowFinish, "regrow finish call");
    ok &= Bytes(site::RestoreObject, restore, 11, "world::restore_object prologue");
    ok &= Bytes(site::SortPush, sort, 5, "push rfg_mover_list_compare");
    const uintptr_t ops[7] = {site::Profile[0], site::Profile[1], site::Profile[2], site::Profile[3], site::Profile[4], site::RfglLane, site::EdfGate};
    for (uintptr_t o : ops) ok &= Bytes(o, cmp, 7, "cmp byte [Multiplayer],0");
    uint8_t cmpOne[7] = {0x80, 0x3D, 0, 0, 0, 0, 0x01};
    memcpy(cmpOne + 2, &mp, 4);
    uint32_t slot = (uint32_t)(g_base + site::GmApply); // relocated at load like every vftable entry
    ok &= Bytes(site::GmGate, cmpOne, 7, "GF-GM cmp byte [Multiplayer],1");
    ok &= Bytes(site::GmSlot, &slot, 4, "general_mover vftable slot 34");
    if (!ok) return false;
    uint8_t* to[7] = {&gc_profile, &gc_profile, &gc_profile, &gc_profile, &gc_profile, &gc_rfglGuest, &gc_edf};
    for (int i = 0; i < 7; i++) {
        uint32_t a = (uint32_t)(uintptr_t)to[i];
        WriteCode(Rva(ops[i] + 2), &a, 4);
    }
    uint32_t gm = (uint32_t)(uintptr_t)&gc_gm, hook = (uint32_t)(uintptr_t)&OnGmDamage;
    WriteCode(Rva(site::GmGate + 2), &gm, 4);
    g_gmNext = (bool(__fastcall*)(uint8_t*, void*, float, int, void*, void*, void*, unsigned, bool))(g_base + site::GmApply);
    WriteCode(Rva(site::GmSlot), &hook, 4); // pass-through unless a live mode-2 host records (OnGmDamage)
    uint32_t oc = (uint32_t)(uintptr_t)&OnCompare;
    WriteCode(Rva(site::SortPush + 1), &oc, 4);
    g_stepNext = (void(__cdecl*)())ChainRel32(site::StepCall, (void*)&OnStep);
    g_rfglNext = (void(__cdecl*)(int*, int, int))ChainRel32(site::RfglCall, (void*)&OnRfgl);
    g_fadeNext = (void(__cdecl*)(uint8_t*, int))ChainRel32(site::FadeCall, (void*)&OnFade);
    ChainRel32(site::ExplFadeCall, (void*)&OnFade), ChainRel32(site::CleanupFadeCall, (void*)&OnFade); // same original: one g_fadeNext
    g_destroyNext = (void(__thiscall*)(void*, uint8_t*, int))ChainRel32(site::DestroyCall, (void*)&OnDestroy);
    g_regrowNext[0] = (uint32_t(__cdecl*)(uint8_t*))ChainRel32(site::RegrowBeginCall, (void*)&OnRegrowBegin);
    g_regrowNext[1] = (uint32_t(__cdecl*)(uint8_t*))ChainRel32(site::RegrowFinishCall, (void*)&OnRegrowFinish);
    gc_hierarchy = g_base + site::Hierarchy, gc_restoreBack = g_base + site::RestoreObject + 6;
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)((uintptr_t)&RestoreThunk - (g_base + site::RestoreObject + 5));
    memcpy(jmp + 1, &rel, 4);
    WriteCode(Rva(site::RestoreObject), jmp, 6);
    return true;
}

// ---- the E4 runner (F5, m6_plan §4.1.4) ----------------------------------------------------------------------------
enum Phase { pIdle, pCapture, pRecord, pSettle, pLoad, pArrive, pTeleport, pShapes, pQuiet, pFeed, pEnd };
struct Cond { const char* name; int fps; bool away; };
constexpr Cond kCond[3] = {{"near60", 60, false}, {"near30", 30, false}, {"far60", 60, true}};
struct {
    Phase ph;
    int scen, run, invalid, bRuns, bStatic, bPart, partRuns, uidEq, uidN; // this scenario's counted runs
    std::vector<int> queue; // the condition of each run
    bool extended, left, retried;
    double t0, f5At;
    uint32_t seed, stampSeq;
    LONG notes0[3], notesRun[3]; // restore-log counts at the stamp / at this run's feed start
    float nearPos[3], centre[3], dir[2], spot[3], dist;
    std::vector<uint8_t> blob;
    WorldMeta meta;
} g_e4;

void Stop(const char* why) { // a scenario's end (done or aborted): every byte back to vanilla (a live session keeps its own)
    if (!g_live) {
        if (g_mode == kReplay) { uint32_t& u = *Rva<uint32_t>(rva::DestrUid); u = std::max(u, g_uidHigh); } // no leftover chunk
        g_mode = kOff, g_feeding = false;                                     // keeps a uid the next scenario hands out again
        Budget(0);
        gc_profile = gc_rfglGuest = 0, gc_edf = g_guest; // a guest armed during E4 (F8) keeps GF-EDF
    }
    Core_SetFpsCap(0);
    g_e4.ph = pIdle;
    if (why) Log("e4: scenario S%d aborted: %s", g_e4.scen, why), Hud("E4 aborted (RfgCoop.log)");
}

void Teleport(float dist) { // near: the F5 #2 spot; far: dist m from the buildings' centre, away from it
    auto& e = g_e4;
    e.spot[0] = dist ? e.centre[0] + e.dir[0] * dist : e.nearPos[0];
    e.spot[1] = e.nearPos[1]; // the hold lands on the terrain when it is higher
    e.spot[2] = dist ? e.centre[2] + e.dir[1] * dist : e.nearPos[2];
    Session_TeleportTo(e.spot, "E4");
    e.ph = pTeleport;
}

void StartRun() {
    auto& e = g_e4;
    const Cond& c = kCond[e.queue[e.run]];
    Core_SetFpsCap(c.fps);
    e.seed = GetTickCount();
    Fn<void __cdecl(int)>(site::RandRegenerate)((int)e.seed); // a fresh pool, as another machine has
    gc_profile = gc_rfglGuest = gc_edf = 1;
    g_mode = kReplay, g_feeding = false, g_map.clear(), g_unpaired.clear(), g_st = {}, g_uidHigh = 0;
    e.ph = pLoad, e.t0 = NowMs(), e.left = e.retried = false;
    char msg[64];
    snprintf(msg, sizeof msg, "E4 run %d/%d %s", e.run + 1, (int)e.queue.size(), c.name);
    Hud(msg);
}

void Reference() { // 10 s quiet after F5 #2: the recording ends with the reference census
    auto& e = g_e4;
    g_mode = kOff;
    Budget(0);
    int nb = 0, nc = 0, ln = 0, tn = 0;
    char list[200] = "", types[120] = "";
    float c[3] = {};
    for (Bld& b : g_bld) {
        b.ref = Census(b.h, true), b.runs = b.staticDiff = 0;
        if (!b.ref.ok) continue;
        nb++;
        // the far spot's centre: only buildings within 100 m of the F5 #2 spot (one hit far away would pull it off)
        if (hypotf(b.pos[0] - e.nearPos[0], b.pos[2] - e.nearPos[2]) <= 100) { nc++; for (int k = 0; k < 3; k++) c[k] += b.pos[k]; }
        if (ln < 150) ln += snprintf(list + ln, sizeof list - ln, "%s%08X alive %d/%d chunks %d", ln ? "; " : "", b.h, b.ref.alive, b.ref.n, b.ref.chunks);
    }
    for (int t = 0; t < 14; t++)
        if (g_rs.types[t]) tn += snprintf(types + tn, sizeof types - tn, "%s%d:%d", tn ? " " : "", t, g_rs.types[t]);
    Log("e4: reference: %d buildings (%s), %d over 100 m from the F5 #2 spot (not in the far centre), steps %d, events %d "
        "(types %s), filtered %d, despawns %d (%d in a step), ptr-like %d, restores %ld regrows %ld, rfgl cut %d, %.2f MB, "
        "hook max %.1f ms", nb, list, nb - nc, g_rs.steps, g_rs.events, tn ? types : "none",
        g_rs.filtered, g_rs.despawns, g_rs.despawnsInStep, g_rs.ptrN, g_notes[0] - e.notes0[0], g_notes[1] + g_notes[2] - e.notes0[1] - e.notes0[2], g_rs.rfglCut,
        g_rec.size() / 1048576.0, g_st.hookMax);
    for (int t = 0, shown = 0; t < 14; t++)
        for (int o = 0; o < kPrefix / 4 && shown < 6; o++)
            if (g_rs.ptr[t][o]) shown++, Log("e4: ptr-like type %d +0x%02X: %d (e.g. %08X = rfg+%X)", t, 4 * o, g_rs.ptr[t][o], g_rs.ptrEx[t][o], g_rs.ptrEx[t][o] - (uint32_t)g_base);
    if (!nb || !g_rs.steps) return Stop("nothing recorded (no building was hit)");
    for (int k = 0; k < 3; k++) e.centre[k] = nc ? c[k] / nc : e.nearPos[k]; // none near: 150 m from the spot, +x
    float dx = e.centre[0] - e.nearPos[0], dz = e.centre[2] - e.nearPos[2], d = sqrtf(dx * dx + dz * dz);
    e.dir[0] = d > 0.1f ? dx / d : 1, e.dir[1] = d > 0.1f ? dz / d : 0;
    e.queue.clear();
    for (int i = 0; i < g_runs; i++) e.queue.push_back(i % 3);
    e.run = e.invalid = e.bRuns = e.bStatic = e.bPart = e.partRuns = e.uidEq = e.uidN = 0, e.extended = false;
    StartRun();
}

void Verdict() {
    auto& e = g_e4;
    int counted = (int)e.queue.size() - e.invalid, worst = 0;
    for (Bld& b : g_bld) worst = std::max(worst, b.staticDiff);
    double pct = e.uidN ? 100.0 * e.uidEq / e.uidN : 100.0;
    // a step-0 DIFF in any run is a census / handle bug (§5.3): INVALID before FAIL, so it never reads as a K3a kill
    const char* a = !counted || !e.bRuns || e.invalid ? "INVALID" : worst >= 2 ? "FAIL" : "PASS";
    const char* b = !counted || !e.bRuns ? "INVALID" : pct >= 95 && e.partRuns <= 1 ? "PASS" : "FAIL";
    Log("e4 verdict S%d: K3a %s (static %d/%d building-runs) K3b %s (uid %.1f%% of %d chunk steps, part %d/%d) invalid %d crash 0",
        e.scen, a, e.bStatic, e.bRuns, b, pct, e.uidN, e.bPart, e.bRuns, e.invalid);
    char msg[96];
    snprintf(msg, sizeof msg, "E4 done: K3a %s K3b %s (RfgCoop.log)", a, b);
    Stop(nullptr);
    Hud(msg);
    Log("e4: S%d done. F5 starts a new scenario from this world; restart the game at the end (the save guard stays on)", e.scen);
}

void EndRun() { // 10 s after the last record: census, compare with the reference, next run
    auto& e = g_e4;
    const Cond& c = kCond[e.queue[e.run]];
    bool invalid = g_st.step0Eq != g_st.step0N;
    int n = 0, st = 0, pt = 0;
    for (Bld& b : g_bld) {
        if (!b.ref.ok) continue;
        Cen x = Census(b.h, true);
        if (!x.ok || b.unstreamed || b.skipped) {
            Log("e4 run %d: %08X no verdict (%s)", e.run + 1, b.h, !x.ok ? "not loaded" : b.unstreamed ? "events skipped while it was not streamed"
                                                                   : "host events skipped (a chunk never made here)");
            continue;
        }
        // static: a divergence before its first touch counts even if the building ends up as the reference
        bool sEq = !b.preDiff && x.alive == b.ref.alive && x.hs == b.ref.hs, pEq = x.hp == b.ref.hp;
        n++, st += sEq, pt += pEq;
        if (!invalid) b.runs++, b.staticDiff += !sEq;
        if (!sEq || !pEq)
            Log("e4 run %d: %08X static %s%s alive %d (ref %d) of %d, part %s chunks %d (ref %d) gone %d (ref %d) orphan %d (ref %d), "
                "dmg %.3f (ref %.3f), destroyed %d (ref %d)", e.run + 1, b.h, sEq ? "MATCH" : "DIFF", b.preDiff ? " (before its first touch)" : "",
                x.alive, b.ref.alive, x.n,
                pEq ? "MATCH" : "DIFF", x.chunks, b.ref.chunks, x.gone, b.ref.gone, x.orphan, b.ref.orphan, x.dmg, b.ref.dmg,
                x.destroyed, b.ref.destroyed);
    }
    char diff[80] = "";
    if (g_st.diffSeq) snprintf(diff, sizeof diff, ", first diff step %u types %s", g_st.diffSeq, g_st.diffTypes);
    Log("e4 run %d/%d %s seed %u: step0 %d/%d %s, static %d/%d %s, part %d/%d %s, uid %d/%d (%.1f%%, n/a %d%s), rfgl own==host "
        "%d/%d (+%d extra, %d unused, capture chunks missing %d), unmapped static %d chunk %d unstreamed %d, by handle %d, "
        "despawns unmapped %d, unpaired faded %d, idle uid %d, catch-up max %d, hook max %.1f ms, %.0f m from the spot%s, "
        "restores %ld regrows %ld", e.run + 1, (int)e.queue.size(), c.name, e.seed,
        g_st.step0Eq, g_st.step0N, invalid ? "DIFF (INVALID run)" : "MATCH", st, n, st == n ? "MATCH" : "DIFF", pt, n,
        pt == n ? "MATCH" : "DIFF", g_st.uidEq, g_st.uidN, g_st.uidN ? 100.0 * g_st.uidEq / g_st.uidN : 100.0, g_st.uidNa, diff,
        g_st.rfglEq, g_st.rfglN, g_st.rfglExtra, g_st.rfglUnused, g_st.preMissing,
        g_st.unStatic, g_st.unChunk, g_st.unStream, g_st.byHandle, g_st.despawnMiss, g_st.unpairedFaded, g_st.idleUid,
        g_st.catchMax, g_st.hookMax, e.dist, e.retried ? " (retried at 100 m)" : "", g_notes[0] - e.notesRun[0],
        g_notes[1] + g_notes[2] - e.notesRun[1] - e.notesRun[2]);
    if (invalid) e.invalid++;
    else {
        e.bRuns += n, e.bStatic += st, e.bPart += pt, e.partRuns += pt != n, e.uidEq += g_st.uidEq, e.uidN += g_st.uidN;
        if (st != n && !e.extended) { // a static DIFF: 2 more runs of this condition (5 at most with e4_runs=3)
            int cond = e.queue[e.run];
            e.extended = true, e.queue.insert(e.queue.end(), 2, cond);
        }
    }
    if (++e.run < (int)e.queue.size()) return StartRun();
    Verdict();
}

void OnF5(uint8_t* me) {
    auto& e = g_e4;
    if (e.ph == pRecord) { // F5 #2: the near spot; recording goes on until the world is quiet
        memcpy(e.nearPos, &Field<float>(me, 0x04), sizeof e.nearPos);
        e.ph = pSettle, e.f5At = NowMs();
        Log("e4: F5 #2 at step %u: settling", g_seq);
        return Hud("E4: settling 10 s");
    }
    if (e.ph != pIdle) return Hud("E4: busy, wait for E4 done");
    const char* why = Session_Active() ? "a co-op session runs" : Guest_Armed() ? "this game has been a co-op guest"
                    : *Rva<uint8_t>(rva::Multiplayer) ? "multiplayer" : World_NotFreeRoam();
    if (!why) try { g_rec.reserve(kRecCap); } catch (...) { why = "no 32 MB for the record"; } // once per process
    if (!why && !World_ArmSaveGuard("E4")) why = "the save guard is unavailable";
    if (!why) why = World_CaptureStart();
    if (why) return Log("e4: F5 refused: %s", why), Hud("E4 refused (RfgCoop.log)");
    e.scen++, e.ph = pCapture;
    gc_profile = 1; // the host records with the MP profile (Q10)
    Hud("E4: recording (F5 when done)");
}

void Runner(int state, uint8_t* me) {
    auto& e = g_e4;
    double now = NowMs();
    // F8 during E4: abort in the frame the session starts (Net_Tick runs first), before any join capture or load. Not in
    // pCapture: world.cpp allows one capture at a time, so ours is polled to its end first.
    if (e.ph > pCapture && (Session_Active() || Guest_Armed())) return Stop("a co-op session started");
    if (e.ph >= pTeleport && !me) return Stop("a load or a death during a run");
    switch (e.ph) {
    case pIdle: return;
    case pCapture: {
        int r = World_CapturePoll(e.blob);
        if (r == 0) return;
        if (g_live) return Stop("a co-op session started"); // polled to its end first: world.cpp has one capture at a time
        if (r < 0 || g_mode != kRecord) return Stop("the capture failed");
        World_ReadMeta(e.meta);
        Log("e4: recording from step %u (blob %d B), profile on", e.stampSeq, (int)e.blob.size());
        e.ph = pRecord;
        return;
    }
    case pRecord:
    case pSettle: // census when nothing was recorded for 10 s (60 s at most after F5 #2)
        if (!me) return Stop("a load or a death during the recording");
        if (World_InMission()) return Stop("a mission started during the recording"); // its restores are not in the record
        if (g_full) return Stop("the recording reached 32 MB");
        if (e.ph == pSettle && state == 1 && (now - std::max(g_rs.lastRec, e.f5At) >= 10000 || now - e.f5At >= 60000)) Reference();
        return;
    case pLoad: {
        int r = state == 1 ? World_Load(e.blob.data(), (int)e.blob.size(), e.meta, false) : 0;
        if (r < 0) return Stop("the world load was refused");
        if (r > 0) e.ph = pArrive, e.t0 = now;
        else if (now - e.t0 > 10000) Stop("the world load did not start");
        return;
    }
    case pArrive: // the first gameplay frame after the load (world.cpp's kArrive test)
        if (state != 1 || !me) e.left = true;
        else if (e.left) return Teleport(kCond[e.queue[e.run]].away ? 150 : 0);
        if (now - e.t0 > 120000) Stop("no gameplay 120 s after the load");
        return;
    case pTeleport: // session.cpp's StepTeleport holds us until the terrain streams in
        if (!Session_Teleporting()) e.ph = pShapes, e.t0 = now;
        return;
    case pShapes: {
        bool all = true;
        for (Bld& b : g_bld)
            if (uint8_t* m = Obj(b.h); b.ref.ok) all &= Mover(m) && Shape(m);
        if (!all && now - e.t0 < 20000) return;
        if (!all && kCond[e.queue[e.run]].away && !e.retried) {
            e.retried = true;
            Log("e4 run %d: a building is not streamed 20 s after landing 150 m away: retrying at 100 m", e.run + 1);
            return Teleport(100);
        }
        float* p = &Field<float>(me, 0x04);
        e.dist = hypotf(p[0] - e.spot[0], p[2] - e.spot[2]);
        for (Bld& b : g_bld) b.matched = b.preDiff = false, b.lines = b.unstreamed = b.skipped = 0; // Apply's step-0 check sets matched
        e.ph = pQuiet, e.t0 = now;
        return;
    }
    case pQuiet:
        if (now - e.t0 < 3000) return;
        for (size_t a = 0; a < g_rec.size(); a = Next(a))
            if (g_rec[a] == kStep) { g_host0 = ((const RStep*)&g_rec[a])->hostMs; break; }
        // r0: each building as loaded here, and the counters the before-first-touch divergence line starts from
        for (Bld& b : g_bld)
            if (b.ref.ok) b.r0 = Hash(b.h, true), b.notesAt = Notes(), b.skipAt = g_skipTotal, b.idleAt = g_idleTotal;
        for (auto [h, u] : g_preUid) // a chunk of the capture the load did not bring back: shardify candidates differ
            if (uint8_t* m = Obj(h); !Mover(m) || Uid(m) != kLoaded) g_st.preMissing++;
        g_rd = 0, g_feedT0 = now, g_feeding = true, g_uidLeftOk = false, g_lastApplied = g_host0;
        Budget(5);
        for (int k = 0; k < 3; k++) e.notesRun[k] = g_notes[k];
        e.ph = pFeed;
        return;
    case pFeed: // DESPAWNs at the head go here once due (dt after the last applied STEP, host time); the step hook stops at them
        while (g_rd < g_rec.size() && g_rec[g_rd] == kDespawn &&
               g_lastApplied - g_host0 + ((const RDespawn*)&g_rec[g_rd])->dt <= NowMs() - g_feedT0)
            ApplyDespawn(*(const RDespawn*)&g_rec[g_rd]), g_rd += sizeof(RDespawn);
        if (g_rd < g_rec.size() && g_rec[g_rd] != kStep && g_rec[g_rd] != kDespawn) return Stop("the record is corrupt"); // never stall without a line
        FadeUnpaired();
        if (g_rd >= g_rec.size()) g_feeding = false, e.ph = pEnd, e.t0 = now;
        return;
    case pEnd:
        FadeUnpaired();
        if (state == 1 && now - e.t0 >= 10000) EndRun();
        return;
    }
}

// ---- live (ver7.1: m6_plan §4.2, destruction_impl.md §11) ---------------------------------------------------------------
uint8_t HostByte(bool stale) { // BLOB_BEGIN destr from the host's ini (§2); a stale epoch is mode 0 + bit 7
    if (stale) return kStaleBit;
    return g_sync ? (uint8_t)(g_sync | (g_profileIni ? kProfileBit : 0) | (g_gmIni ? kGmBit : 0) | (g_ownIni ? kOwnPack : 0)) : 0;
}

void DropNotes() { // restores from before an epoch's start (a load's own stream-in restores, pre-capture ones): not its
    AcquireSRWLockExclusive(&g_noteLock);
    g_noteQ.clear();
    ReleaseSRWLockExclusive(&g_noteLock);
}

void LogOff(const char* why) { // the log has a hole from here: no resend can use it until our next load
    Log("destr: re-join log off (%s): mid-mission re-joins are stale until our next load", why);
    g_log.clear(), g_log.shrink_to_fit(), g_stamps.clear(), g_logOn = false, g_logAt = g_rec.size();
}

void LogSync() { // host: g_rec's new records into the log (UIDS never: PutUids moves g_logAt past it)
    if (!g_logOn || g_rec.size() <= g_logAt) return void(g_logAt = std::min(g_logAt, g_rec.size()));
    if (g_log.size() + g_rec.size() - g_logAt > kLiveCap) return LogOff("it passed 16 MB");
    try { // up to 16 MB beside g_rec's 16 in a 32-bit process: never a throw into the game's frames
        g_log.insert(g_log.end(), g_rec.begin() + g_logAt, g_rec.end());
    } catch (...) { return LogOff("out of memory"); }
    g_logAt = g_rec.size();
}

std::vector<uint32_t> UidSnap() { // our runtime chunks as {handle, uid} pairs (the blob load gives them uid -1 on the guest)
    std::vector<uint32_t> v;
    ForMovers([&](uint8_t* o) { if (Uid(o) != kLoaded) v.push_back(Handle(o)), v.push_back(Uid(o)); });
    return v;
}

void BufHashes() { // the stored buffers as they are: only a later change of them is a save the log can start from
    for (int i = 0; i < 2; i++) {
        int n;
        const uint8_t* b = World_StateBuf(i, n);
        g_bufHash[i] = b ? Fnv(b, n) : 0;
    }
}

void HostReset() { // the stream of the next epoch starts empty
    if (g_logOn) DrainNotes(); // restores queued before it belong to the log (DropNotes below would lose them)
    LogSync();
    g_logAt = 0,
    g_rec.clear(), g_sent = 0, g_cenOut.clear(), g_restSent.clear(), g_stepDespawns.clear(), g_stepGm.clear(), g_rs = {},
    g_full = false;
    g_hSerial++, g_pkt = 0, g_hMatch = g_hDiff = g_hNv = 0;
    for (Bld& b : g_bld) b.due = false; // no old epoch's census in the next one (the resync check covers them)
    DropNotes();
}

// The epoch's first record: our runtime chunks (the guest's blob load gives them uid -1, and its shardify sort needs these
// uids, OnCompare). A capture and every resend: a live re-join resend's buffer holds the same runtime chunks (not stale =
// nothing changed since it was written); after our load there are none (n 0).
// 7.2: a resend with a log base sends the chunks of the save it resends (its stamp's), not ours now.
uint32_t PutUids(const std::vector<uint32_t>& v) {
    size_t at = g_rec.size();
    Put(RUids{kUids, {}, 0});
    uint32_t n = 0;
    for (size_t i = 0; i + 1 < v.size() && Room(8); i += 2) Put(v[i]), Put(v[i + 1]), n++;
    return ((RUids*)&g_rec[at])->n = n;
}

void HostStamp() { // the session's capture: its blob holds every step before this; the next epoch streams from here
    HostReset();
    g_hId = 0, g_hDestr = HostByte(false);
    gc_profile = (g_hDestr & kModeMask) && (g_hDestr & kProfileBit);
    uint32_t n = PutUids(UidSnap());
    g_logAt = g_rec.size(); // the UIDS record is this epoch's, not history
    Log("destr: capture at step %u: the next epoch records from here (mode %d), %u chunks with a runtime uid noted for the "
        "guest", g_seq, g_hDestr & kModeMask, n);
}

MsgCensus MakeCensus(const Bld& b, const Cen& c, uint8_t kind, int all) { // kind bit 0 resync check; bit 1 own-pack
    MsgCensus m{};
    m.type = kCensus, m.kind = kind | (b.ownEp == g_hSerial ? 2 : 0), m.epoch = g_hId, m.seq = g_seq, m.handle = b.h;
    memcpy(m.pos, &Field<float>(Obj(b.h), 0x04), sizeof m.pos);
    m.n = (uint16_t)c.n, m.alive = (uint16_t)c.alive, m.hStatic = c.hs, m.hPart = c.hp, m.chunks = (uint16_t)c.chunks;
    m.gone = (uint16_t)c.gone, m.orphan = (uint16_t)c.orphan, m.destroyed = c.destroyed, m.dmg = c.dmg, m.chunksAll = all;
    return m;
}

void HostTick(int state) {
    if (!Me()) { // our level load: that world is gone; the load's resend starts the next epoch
        if (g_hId || g_hDestr) Log("destr: our level load ends epoch %u", g_hId);
        if (g_hId || g_hDestr || !g_rec.empty()) HostReset(), g_hId = 0, g_hDestr = 0, gc_profile = 0;
        if (!g_log.empty() || !g_stamps.empty() || !g_loadStamp) { // the log restarts with the load (0x32D988 resets the uid)
            g_log.clear(), g_stamps.clear(), g_logAt = 0, g_loadStamp = g_logOn = g_sync != 0 && World_HookSaves();
            AcquireSRWLockExclusive(&g_noteLock); // a repair cut short by the load: its handle is not ours any more
            g_mine.clear(), g_repairDone.clear();
            ReleaseSRWLockExclusive(&g_noteLock);
        }
        return DropNotes();
    }
    if (g_loadStamp) { // the first frame after our load: the buffer that load used is the log's offset 0
        g_loadStamp = false;
        int n;
        BufHashes();
        if (const uint8_t* b = World_CurrentState(n)) // no runtime chunks right after a load
            g_stamps[Fnv(b, n)] = {}, Log("destr: re-join log restarts with our load (base %u KB)", (unsigned)(n >> 10));
    }
    DrainNotes();
    LogSync();
    // The backlog reached 16 MB (a step went unrecorded; the link or 16 packets a tick fell behind): this epoch cannot be
    // followed, never a silent mode switch the guest is not told of. Either way the epoch ends here (g_hId 0: nothing more
    // of it goes out). Gate shut: the blob on its way, or PollCapture's hjWant retry after a failed capture, starts the
    // next epoch. Gate open: ask for a fresh blob as F3 does; its BEGIN tells the guest its mode.
    if (g_full) {
        if (g_logOn) LogOff("a record did not fit in the stream"); // a dropped step: the log has a hole
        if (g_hDestr & kModeMask) {
            bool open = Session_StreamOpen();
            Log("destr: the stream backlog reached %u MB: epoch %u stops streaming (%s)", (unsigned)(kLiveCap >> 20), g_hId,
                open ? "a fresh blob re-syncs the guest" : "the blob on its way re-syncs");
            if (open) Session_AskBlob();
            HostReset(), g_hId = 0, g_hDestr = kStaleBit;
        }
        g_full = false;
    }
    if (!(g_hDestr & kModeMask)) { // mode 0 or stale: the census only (a drift monitor)
        LogSync();
        g_rec.clear(), g_sent = 0, g_logAt = 0;
        for (CenOut& c : g_cenOut) c.at = 0;
    }
    double now = NowMs();
    // live census: 10 s after a building's last event (a new event restarts it), not while paused, once a guest has the world
    if (g_hId && state == 1 && !*Rva<int>(site::PausedCount)) {
        int all = -2;
        for (Bld& b : g_bld)
            if (b.due && now - b.last >= 10000) {
                b.due = false;
                Cen c = Census(b.h, true);
                if (!c.ok) continue; // not streamed here any more: no census
                if (all == -2) all = Destr_Chunks();
                g_cenOut.push_back({g_rec.size(), MakeCensus(b, c, 0, all)});
            }
    }
    if (!g_hId || !Session_StreamOpen()) return;
    static uint8_t pkt[sizeof(MsgDestr) + kDestrBytes];
    // 16 packets (64 KB) per tick at most, and none while ENet holds 256 KB of channel 2 (queued or unacked: its send queue
    // has no cap): the rest waits in g_rec, so a link slower than our destruction fills it and the 16 MB end bounds it (§3.3)
    for (int k = 0;;) {
        while (!g_cenOut.empty() && g_cenOut.front().at <= g_sent) // behind the stream bytes it covers
            NetSend(kChBlob, &g_cenOut.front().m, sizeof(MsgCensus)), g_cenOut.erase(g_cenOut.begin()), g_ls.census++;
        if (g_sent >= g_rec.size() || k++ == 16 || NetQueued() >= (256u << 10)) break;
        // never cross a waiting census's offset: the guest splices it in at the end of what it has, so it must land on the
        // record boundary it was taken at (the while above sent every at <= g_sent, so end > g_sent here)
        size_t end = g_cenOut.empty() ? g_rec.size() : g_cenOut.front().at;
        MsgDestr h{kDestr, g_hId, ++g_pkt, (uint16_t)std::min<size_t>(kDestrBytes, end - g_sent)};
        uint16_t len = h.len;
        memcpy(pkt, &h, sizeof h), memcpy(pkt + sizeof h, &g_rec[g_sent], len);
        NetSend(kChBlob, pkt, sizeof h + len);
        g_ls.pkts++, g_ls.raw += len; // est: the tick's first packet range-coded, scaled (75-190 us per 4 KB on the sim thread)
        if (k == 1) g_ls.estRaw += len, g_ls.est += NetRangeCoded(&g_rec[g_sent], len);
        g_sent += len;
    }
    if (g_sent && g_sent == g_rec.size()) { // all out: start over (keeps the reserved capacity)
        LogSync();
        g_rec.clear(), g_sent = 0, g_logAt = 0;
        for (CenOut& c : g_cenOut) c.at = 0;
    }
}

void GuestReset() { // a new epoch, or none: nothing of the last world's stream carries over
    if (g_gEpoch && (g_st.uidN || g_st.uidNa || g_st.step0N || g_st.rfglN)) // the epoch's totals (the 5 s line misses its tail)
        Log("destr: blob %u ends: uid equal %d/%d (n/a %d), step0 %d/%d, rfgl own==host %d/%d, unmapped static %d chunk %d, "
            "despawn misses %d", g_gEpoch, g_st.uidEq, g_st.uidN, g_st.uidNa, g_st.step0Eq, g_st.step0N, g_st.rfglEq, g_st.rfglN,
            g_st.unStatic, g_st.unChunk, g_st.despawnMiss);
    g_rec.clear(), g_rd = 0, g_feeding = g_uidLeftOk = false, g_map.clear(), g_unpaired.clear(), g_preUid.clear(), g_bld.clear();
    g_pend.clear(), g_retry.clear(), g_gmRetry.clear(), g_restored.clear(), g_lost.clear(), g_evSeq.clear(), g_echo.clear(), g_st = {}, g_uidHigh = 0;
    g_priv = kPriv, g_synthNa = false, g_lastAt = 0, g_drain = 0, g_rc = {}, g_gMatch = g_gDiff = g_gNv = 0, g_gPkt = 0, g_brokeEpoch = 0;
    g_drift.clear();
}

// guest: this epoch's stream cannot be followed: drop it and ask for the world again. Broken before that world landed
// (its blob still waits or lands, so the request goes nowhere: "a world is already on its way"): asked again once it has
void Broken(const char* why) {
    Log("destr: the stream of blob %u broke (%s): re-joining", g_gEpoch, why);
    uint16_t e = g_gEpoch && !Session_EpochLoaded(g_gEpoch) ? g_gEpoch : 0;
    GuestReset(), g_gEpoch = 0, g_brokeEpoch = e;
    Session_RejoinRequest("destruction stream broke");
}

void LoadUids(const uint8_t* r) { // the host's runtime chunks at its capture: loaded here with uid -1 (OnCompare sorts by these)
    uint32_t n = ((const RUids*)r)->n;
    const uint32_t* p = (const uint32_t*)(r + sizeof(RUids));
    g_preUid.clear(), g_st.preMissing = 0;
    for (uint32_t i = 0; i < n; i++) {
        g_preUid[p[2 * i]] = p[2 * i + 1];
        if (uint8_t* m = Obj(p[2 * i]); !Mover(m) || Uid(m) != kLoaded) g_st.preMissing++;
    }
    Log("destr: the host's capture had %u chunks with a runtime uid, %d of them not here (as blob-loaded chunks)", n, g_st.preMissing);
}

void ResyncLine(const MsgCensus& c) {
    auto& r = g_rc;
    Log("census: resync check (blob %u, %s): %u buildings, %d MATCH, %d DIFF, %d not loaded, %d no verdict, part %d/%d",
        g_gEpoch, g_gDestr & kStaleBit ? "stale" : "fresh", (unsigned)c.count, r.match, r.diff, r.notLoaded, r.noVerdict, r.partEq,
        r.match + r.diff);
    if (!g_partOnce && r.match + r.diff) // once per session: can part ever match across a load (chunks rebuilt by the loader)?
        g_partOnce = true, Log("census: resync part MATCH %d/%d (information only)", r.partEq, r.match + r.diff);
    char t[64];
    snprintf(t, sizeof t, "Destruction check: %d MATCH, %d DIFF", r.match, r.diff);
    Hud(t);
    g_rc = {};
}

// Guest: a host census at the head of the stream (after the steps and DESPAWNs before it). Verdict = identity + static;
// part is printed for K3b. false = not judged yet: the building is not streamed here, or its waiting host events have
// not run; retried every 2 s, last = the 60 s try ("no verdict"). A resync check waits the same way (its shapes can
// stream in after the landing, which waits for terrain only), and its summary line follows its last answer.
bool EvalCensus(const MsgCensus& c, bool last) {
    bool rs = c.kind & 1;
    if (rs && !c.count) return ResyncLine(c), true; // the host tracks no building
    auto lost = g_lost.find(c.handle);
    auto es = g_evSeq.find(c.handle); // a retry after host steps newer than the census: its snapshot is older than here
    const char* nv = c.kind & 2 ? "own-pack" : g_restored.count(c.handle) ? "restored" : lost != g_lost.end() ? lost->second
                   : es != g_evSeq.end() && es->second > c.seq ? "newer host events" : nullptr;
    bool absent = false;
    Cen x = nv ? Cen{} : Census(c.handle, true);
    if (!nv && !x.ok) {
        if (!last) return false;
        nv = rs ? "not loaded for 60 s" : "not streamed for 60 s", absent = true;
    }
    if (!nv && std::any_of(g_pend.begin(), g_pend.end(), [&](const Pend& p) { return Names(p.ev, c.handle); })) {
        if (!last) return false; // an event naming it still waits (RunPending runs it once its mover streams in)
        nv = "events pending";
    }
    uint8_t v = rs ? 16 : 0;
    int all = Destr_Chunks();
    if (nv) {
        v |= 8;
        if (rs) (absent ? g_rc.notLoaded : g_rc.noVerdict)++;
        Log("census: %08X seq %u%s: no verdict (%s)", c.handle, c.seq, rs ? " (resync check)" : "", nv);
    } else {
        const float* p = &Field<float>(Obj(c.handle), 0x04);
        float dx = p[0] - c.pos[0], dy = p[1] - c.pos[1], dz = p[2] - c.pos[2];
        bool id = dx * dx + dy * dy + dz * dz <= 0.01f, st = x.alive == c.alive && x.hs == c.hStatic, pt = x.hp == c.hPart;
        v |= id | st << 1 | pt << 2 | (x.alive == x.n) << 5; // bit 5: whole here (a safehouse the capture could not carry, 7.2)
        if (rs) (id && st ? g_rc.match : g_rc.diff)++, g_rc.partEq += pt;
        else if (id && st) g_gMatch++;
        else g_gDiff++, g_drift.insert(c.handle);
        Log("census: %08X seq %u%s: identity %s static %s (part %s) chunks A %d vs B %d; alive %d (host %u) of %d, chunks %d (host "
            "%u), gone %d (host %u), dmg %.3f (host %.3f)", c.handle, c.seq, rs ? " (resync check)" : "", id ? "OK" : "BAD",
            st ? "MATCH" : "DIFF", pt ? "MATCH" : "DIFF", c.chunksAll, all, x.alive, c.alive, x.n, x.chunks, c.chunks, x.gone,
            c.gone, x.dmg, c.dmg);
        if (!g_sane && x.dmg == 0) // once per session: an undamaged building must count every subpiece alive
            g_sane = true, Log("census: sanity: %08X undamaged, alive %d of %d (%s)", c.handle, x.alive, x.n, x.alive == x.n ? "OK" : "WRONG");
    }
    MsgCensusRes res{kCensusRes, v, c.epoch, c.handle, all, 0.0f};
    NetSend(kChControl, &res, sizeof res);
    if (rs) {
        if (g_rc.match + g_rc.diff + g_rc.notLoaded + g_rc.noVerdict >= c.count) ResyncLine(c); // answers come out of order
        return true;
    }
    g_gNv += !!nv; // shown too: a building that lost host events (C1 step 6) must still give the tester a line
    char t[96]; // F3 is refused in the host's mission (and this guest cannot tell): never promise it right away
    if (!GMode() && g_gDiff) snprintf(t, sizeof t, "World drifted (%d buildings): F3 re-syncs in the host's free roam", (int)g_drift.size());
    else snprintf(t, sizeof t, "Destruction check: %d MATCH, %d DIFF, %d no verdict", g_gMatch, g_gDiff, g_gNv);
    Hud(t);
    return true;
}

void GuestTick(int state) {
    // C1 step 5: this game's own pause edge (Esc, map, the host's pause token: all raise Game_paused_count). At the resume
    // the whole STEPs waiting are counted; Apply logs "drained" when the last of them is applied (later arrivals aside)
    static bool paused;
    static double pausedAt;
    if (bool q = *Rva<int>(site::PausedCount) > 0; q != paused) {
        paused = q, g_drain = 0;
        if (q) pausedAt = NowMs();
        else { // the 60 s (pending, retries) and 30 s (unpaired) budgets are for streaming, which a pause cannot do
            double d = NowMs() - pausedAt;
            for (Pend& p : g_pend) p.at += d;
            for (Retry& x : g_retry) x.first += d, x.next += d;
            for (Unpaired& u : g_unpaired) u.at += d;
        }
        for (size_t a = g_rd; !q && a < g_rec.size() && Whole(a) == 1; a = Next(a)) g_drain += g_rec[a] == kStep;
        Log("destr: game %s (backlog %u bytes, %d STEPs)", q ? "paused" : "resumed", (unsigned)(g_rec.size() - g_rd), g_drain);
    }
    if (g_brokeEpoch && Session_EpochLoaded(g_brokeEpoch)) g_brokeEpoch = 0, Session_RejoinRequest("destruction stream broke");
    bool open = g_gEpoch && Session_EpochLoaded(g_gEpoch);
    if (open && !g_feeding)
        Log("destr: blob %u is loaded here: applying its stream (%u bytes waiting)", g_gEpoch, (unsigned)(g_rec.size() - g_rd));
    g_feeding = open;
    if (!open) return;
    double now = NowMs();
    // the records at the head that are not STEPs, here outside the Havok step; a STEP is the hook's (Replay)
    while (g_rd < g_rec.size()) {
        int w = Whole(g_rd);
        if (w < 0) return Broken("a corrupt record");
        if (!w || g_rec[g_rd] == kStep) break;
        const uint8_t* r = &g_rec[g_rd];
        if (r[0] == kDespawn) {
            const RDespawn& d = *(const RDespawn*)r;
            // live: the network paces it (the host sends each tick, so it arrives dt after its step); FIFO order keeps it
            // after STEP(S). Mode 1: chunks are local, so is their deleter
            if (Full()) ApplyDespawn(d);
        } else if (r[0] == kGmDead) {
            if (Full() && !ApplyGmDead(*(const RGmDead*)r)) {
                if (g_gmRetry.size() < 256) g_gmRetry.push_back({*(const RGmDead*)r, now});
                else if (++g_gmMissed <= 3) Log("destr: GM_DEAD %08X: 256 waiting, dropped (#%d)", ((const RGmDead*)r)->handle, g_gmMissed);
            }
        } else if (r[0] == kRestore) {
            const RRestore& x = *(const RRestore*)r;
            if (x.how == 3) g_restored.erase(x.handle); // the host's repair is done: its events count again
            else g_restored.insert(x.handle);
        }
        else if (r[0] == kUids)
            LoadUids(r);
        else {
            MsgCensus c;
            memcpy(&c, r + 1, sizeof c);
            // a newer census of the same kind supersedes a waiting one (a live census never drops a resync check's)
            std::erase_if(g_retry, [&](const Retry& x) { return x.c.handle == c.handle && (x.c.kind & 1) == (c.kind & 1); });
            if (!EvalCensus(c, false)) g_retry.push_back({c, now, now + 2000});
        }
        g_rd = Next(g_rd);
    }
    if (Full() && !g_gmRetry.empty()) // a GM_DEAD whose mover streams in (or comes near) later: 120 s
        std::erase_if(g_gmRetry, [&](const GmRetry& g) {
            if (ApplyGmDead(g.k)) return true;
            if (now - g.first < 120000) return false;
            if (++g_gmMissed <= 3) Log("destr: GM_DEAD %08X: no such general mover within 50 m here for 120 s (#%d)", g.k.handle, g_gmMissed);
            return true;
        });
    if (g_rd && g_rd == g_rec.size()) g_rec.clear(), g_rd = 0;
    else if (g_rd > (1u << 20)) g_rec.erase(g_rec.begin(), g_rec.begin() + g_rd), g_rd = 0; // ponytail: applied bytes past 1 MB go
                                                                                          // (a host streaming every frame never drains it to empty)
    for (size_t i = 0; i < g_retry.size();) { // not streamed at the head: every 2 s, the 60 s try gives "no verdict"
        Retry& x = g_retry[i];
        if (now < x.next) i++;
        else if (EvalCensus(x.c, now - x.first >= 60000)) g_retry.erase(g_retry.begin() + i);
        else x.next = now + 2000, i++;
    }
    std::erase_if(g_pend, [&](const Pend& p) {
        return now - p.at >= 60000 && (++g_ls.pendDropped, Lost(p.ev, "host events dropped (not streamed for 60 s)"), true);
    });
    std::erase_if(g_echo, [&](const auto& e) { return now - e.second > 10000; });
    if (Full()) FadeUnpaired();
    // fades and in-step re-splits never send a destroy DESPAWN, so their entries would stay until the next blob; this
    // drops exactly those Mapped already reads as absent (a fading chunk is still alive, so it stays mapped)
    static double sweepAt;
    if (now - sweepAt >= 5000)
        sweepAt = now, std::erase_if(g_map, [](const auto& p) { uint8_t* m = Obj(p.second); return !Mover(m) || Uid(m) != p.first; });
}

// ---- 5 s lines, the charge log (Q26), the crush counters (M7a) ------------------------------------------------------------
float Pct(std::vector<float>& v, double p) { // -1 = no sample
    if (v.empty()) return -1;
    size_t i = std::min(v.size() - 1, (size_t)std::ceil(p * v.size()) - 1); // nearest rank: p95 of 2 = the larger
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

void LiveStats() {
    static double last;
    double now = NowMs();
    if (now - last < 5000) return;
    last = now;
    char crush[240] = "";
    if (g_crush != g_crushShown) {
        static const char* const vk[3] = {"Player", "puppet", "NPC"}, *const ak[4] = {"NULL", "mover", "puppet", "other"};
        int n = snprintf(crush, sizeof crush, ", crush (victim<-attacker type: count)");
        for (auto [k, c] : g_crush)
            if (n < (int)sizeof crush - 40) n += snprintf(crush + n, sizeof crush - n, " %s<-%s t%u: %d", vk[k >> 16], ak[k >> 8 & 0xFF], k & 0xFF, c);
        g_crushShown = g_crush;
    }
    char from[160] = ""; // which code restored movers (C1 reads it: object_mover::stream's 0x68B9F8 is per machine)
    AcquireSRWLockExclusive(&g_noteLock);
    if (g_restoreFrom != g_restoreShown) {
        int n = snprintf(from, sizeof from, ", restore_object callers");
        for (auto [f, c] : g_restoreFrom)
            if (n < (int)sizeof from - 24) n += snprintf(from + n, sizeof from - n, " rfg+%X x%d", f, c);
        g_restoreShown = g_restoreFrom;
    }
    ReleaseSRWLockExclusive(&g_noteLock);
    strncat(crush, from, sizeof crush - strlen(crush) - 1);
    auto& l = g_ls;
    size_t backlog = g_host ? g_rec.size() - g_sent : g_rec.size() - g_rd;
    if (!l.pkts && !l.census && !l.dropped && !l.synth && !l.pendDropped && !l.staleIn && !l.ownPack && !backlog && l.lag.empty() &&
        !*crush) return; // a quiet window: no line
    if (g_host)
        Log("destr: out %u pkts, bytes raw %llu est %llu, max/step %u, backlog %u, ENet queue %u KB, census sent %u, own-pack "
            "echoes left out %u, RESTORE %u, restores %ld regrows %ld/%ld, uid %u chunks %d, net drops %u%s", l.pkts,
            (unsigned long long)l.raw, (unsigned long long)(l.estRaw ? l.est * l.raw / l.estRaw : 0), (unsigned)l.maxStep, (unsigned)backlog,
            (unsigned)(NetQueued() >> 10), l.census, l.ownPack, l.restOut, g_notes[0],
            g_notes[1], g_notes[2], *Rva<uint32_t>(rva::DestrUid), Destr_Chunks(), NetDrops(), crush);
    else
        Log("destr: in %u pkts, bytes raw %llu, backlog %u, lag p50 %.0f p95 %.0f ms, uid equal %d/%d (n/a %d), unmapped static %d "
            "chunk %d, skipped %ld, restored %d (movers %u), pending %u (dropped %u, applied %u), unpaired faded %d, "
            "local events dropped %u (driving %u), own-pack %u, net drops %u, rfgl own==host %d/%d, step0 %d/%d, echo p50 %.0f "
            "p95 %.0f ms, stale-epoch drops %u, restores %ld regrows %ld/%ld%s", l.pkts, (unsigned long long)l.raw, (unsigned)backlog,
            Pct(l.lag, .5), Pct(l.lag, .95), g_st.uidEq, g_st.uidN, g_st.uidNa, g_st.unStatic, g_st.unChunk, g_skipTotal,
            g_st.restored, (unsigned)g_restored.size(), (unsigned)g_pend.size(), l.pendDropped, l.synth, g_st.unpairedFaded,
            l.dropped, l.driving, l.ownPack, NetDrops(), g_st.rfglEq, g_st.rfglN, g_st.step0Eq, g_st.step0N, Pct(l.echo, .5),
            Pct(l.echo, .95), l.staleIn, g_notes[0], g_notes[1], g_notes[2], crush);
    l = {};
}

void ChargeTick() { // Q26: a Rhino charge (pack id 8 on, +0x254) by this player or the partner's puppet, metres covered
    uint8_t* who[2] = {Me(), Obj(Puppet_Handle())};
    double now = NowMs();
    for (int i = 0; i < 2; i++) {
        uint8_t *p = who[i], *bp = p ? Field<uint8_t*>(p, 0x1170) : nullptr, *bi = bp ? Field<uint8_t*>(bp, 0x270) : nullptr;
        bool on = bi && Field<int>(bi, 0x50) == 8 && Field<uint8_t>(bp, 0x254);
        auto& c = g_charge[i];
        if (on && !c.on) memcpy(c.from, &Field<float>(p, 0x04), sizeof c.from), c.end = 0, c.echo = -1, c.wait = !i && !g_host;
        else if (!on && c.on && p) c.dist = hypotf(hypotf(Field<float>(p, 0x04) - c.from[0], Field<float>(p, 0x08) - c.from[1]),
                                                   Field<float>(p, 0x0C) - c.from[2]), c.end = now;
        c.on = on;
        if (c.end && now - c.end >= 1000) { // a second for the echo of B's own charge (the first host step on what it hit)
            char echo[40] = "";
            if (!i && !g_host) snprintf(echo, sizeof echo, c.echo >= 0 ? ", echo %.0f ms" : ", no echo yet", c.echo);
            Log("charge: %s %.1f m%s", i ? (g_host ? "puppet of B" : "puppet of A") : "own", c.dist, echo);
            c.end = 0, c.wait = false;
        }
    }
}

} // namespace

extern "C" void __cdecl OnRestore(uintptr_t from, uint32_t handle) { Note(0, handle, from - g_base - 5); }

// ---- exports (features.h) --------------------------------------------------------------------------------------------
void Destr_Install() {
    g_dev = IniInt(L"dev", 0) == 1;
    g_catchup = std::clamp(IniInt(L"destr_catchup", 4), 1, 16);
    g_runs = std::clamp(IniInt(L"e4_runs", 3), 1, 9);
    g_types = IniInt(L"e4_types", 0x3FFF);
    g_gmIni = IniInt(L"destr_gm", 1); // 7.2: gas tanks / props die by the host's GM_DEAD on a mode-2 guest (0 = local, as 7.1)
    // ver7.1: only the host's values count (they travel in BLOB_BEGIN); destr_check is the guest's own
    g_syncIni = g_sync = std::clamp(IniInt(L"destr_sync", 2), 0, 2), g_profileIni = IniInt(L"destr_profile", 1) != 0;
    g_ownIni = IniInt(L"destr_ownpack", 0) != 0, g_check = IniInt(L"destr_check", 1) != 0;
    if (!(g_installed = Install())) return Log("destr: sites not as expected (above): nothing installed (no E4, no GF-EDF, no shared destruction)");
    Log("destr: installed, all bytes 0 (vanilla): profile 339EA3 339FE9 33811A 3249AC 332772, rfgl lane 325051, GF-EDF 3B0024, "
        "step 4102E0, rfgl 3383FD, despawns 324ACC 324ADE 6A2657 7CD092, regrow 33A94C 33A92E, restore 534B70, sort 3249BF; dev %d: F5 runs E4 (e4_runs %d, "
        "e4_types %X, destr_catchup %d); co-op: destr_sync %d, destr_profile %d, destr_ownpack %d (as host), destr_check %d",
        g_dev, g_runs, g_types, g_catchup, g_sync, g_profileIni, g_ownIni, g_check);
}

void Destr_Tick(int state) {
    if (!g_installed) return;
    if (uint8_t mp = *Rva<uint8_t>(rva::Multiplayer); mp != g_mp) // real multiplayer reads its own flag through our bytes
        g_mp = mp, gc_profile = gc_rfglGuest = gc_gm = mp, gc_edf = g_guest ? 1 : mp;
    if (!Me()) g_sinceLoad = 0, InterlockedExchange(&g_restSinceLoad, 0); // the stale rule counts from the last level load
    if (g_live) {
        g_host ? HostTick(state) : GuestTick(state);
        ChargeTick();
        LiveStats();
    }
    if (!g_dev) return;
    static bool tid;
    if (!tid && g_hookTid)
        tid = true, Log("e4: thread 0x4102E0 %lu %s tick %lu", g_hookTid, g_hookTid == GetCurrentThreadId() ? "=" : "!=", GetCurrentThreadId());
    uint8_t* me = Me();
    bool f5 = *Rva<uint8_t>(rva::OsAppActive) && GetAsyncKeyState(VK_F5) < 0;
    if (f5 && !g_f5 && state == 1 && me) OnF5(me);
    g_f5 = f5;
    Runner(state, me);
}

void Destr_Stamp() { // world.cpp, the capture's handshake: save_state_data runs right after this, so later steps are new
    if (g_live && g_host) { // a capture the session no longer wants (our load made it moot) leaves the live epoch alone
        if (Session_Capturing()) HostStamp();
        return;
    }
    if (g_e4.ph != pCapture || g_mode != kOff) return;
    g_rec.clear(), g_bld.clear(), g_stepDespawns.clear(), g_rs = {}, g_st = {}, g_full = false; // clear keeps the capacity
    for (int k = 0; k < 3; k++) g_e4.notes0[k] = g_notes[k];
    g_preUid.clear();
    ForMovers([](uint8_t* o) { if (Uid(o) != kLoaded) g_preUid[Handle(o)] = Uid(o); });
    g_mode = kRecord, g_e4.stampSeq = g_seq;
    Budget(5);
}

void Destr_Saved() { // world.cpp, after process_save_load rewrote a stored buffer: its bytes hold the log up to here
    if (!g_live || !g_host || !g_logOn) return;
    LogSync();
    if (!g_logOn) return;
    try {
        std::vector<uint32_t> uids;
        bool snap = false;
        for (int i = 0; i < 2; i++) {
            int n;
            const uint8_t* b = World_StateBuf(i, n);
            uint32_t h = b ? Fnv(b, n) : 0;
            if (!b || h == g_bufHash[i]) continue; // not rewritten by this save (a stale capture, the other buffer)
            g_bufHash[i] = h;
            if (!snap) uids = UidSnap(), snap = true;
            g_stamps.try_emplace(h, Stamp{g_log.size(), uids}); // bytes seen before keep their first offset
        }
    } catch (...) { LogOff("out of memory"); }
}

void Destr_GuestArm() {
    g_guest = true;
    if (g_installed) gc_edf = 1;
    Log(g_installed ? "destr: GF-EDF on until the game exits (3B0024): this game's destruction changes no district control"
                    : "destr: GF-EDF unavailable (sites not installed)");
}

bool Destr_Busy() { return g_e4.ph != pIdle; } // world.cpp: F9 / F10 are off while E4 runs

int Destr_Chunks() { // type 2/1 movers with a runtime uid; -1 outside gameplay
    if (GameState() != 1 || !Me()) return -1;
    int n = 0;
    ForMovers([&](uint8_t* o) { n += Uid(o) != kLoaded; });
    return n;
}

// ---- ver7.1 live (destruction_impl.md §11) ---------------------------------------------------------------------------------
void Destr_Link(bool on, bool host) {
    if (!g_installed || on == g_live) return;
    if (on && g_e4.ph > pCapture) Stop("a co-op session started"); // pCapture: the runner polls world.cpp's capture out first
    g_live = on, g_host = host, g_mode = on && host ? kRecord : kOff;
    gc_profile = gc_rfglGuest = gc_gm = 0, gc_edf = g_guest;
    GuestReset(), HostReset(), g_bld.clear(), g_ls = {}, g_crush.clear(), g_crushShown.clear();
    g_log.clear(), g_log.shrink_to_fit(), g_stamps.clear(), g_logOn = false; // 7.2: a link's log ends with it
    AcquireSRWLockExclusive(&g_noteLock);
    g_restoreFrom.clear(), g_restoreShown.clear(), g_mine.clear(), g_repairDone.clear();
    ReleaseSRWLockExclusive(&g_noteLock);
    g_hId = g_gEpoch = 0, g_hDestr = g_gDestr = 0, g_sane = g_partOnce = g_hFresh = false, g_repaired.clear();
    for (auto& c : g_charge) c = {};
    Budget(on ? 5 : 0);
    if (!on) return Log("destr: live off: every co-op byte back to vanilla%s", g_guest ? " (GF-EDF stays on this guest)" : "");
    g_bld.reserve(65); // g_noteQ: not here (no lock, and DrainNotes swaps it away); Note caps it at 4096
    g_sync = g_syncIni; // a failed reserve below lasts this link only
    g_log.clear(), g_stamps.clear(), g_logAt = 0, g_loadStamp = g_logOn = false; // 7.2: the log starts at the link
    if (host) try { // Record appends inside the Havok callback: never reallocate there
        g_rec.reserve(kLiveCap);
    } catch (...) {
        g_sync = 0;
        Log("destr: no %u MB for the stream: this host runs mode 0 (parallel worlds) this session", (unsigned)(kLiveCap >> 20));
    }
    g_logOn = host && g_sync != 0 && World_HookSaves(); // after the reserve: no stream, no log (an older save: stale)
    if (g_logOn) BufHashes();
    Log(host ? "destr: live host: mode %d (destr_sync), profile %d, own-pack %d; it records from the next capture"
             : "destr: live guest: waits for the host's first blob (its mode), destr_catchup %d, destr_check %d",
        host ? g_sync : g_catchup, host ? g_profileIni : g_check, g_ownIni);
}

uint8_t Destr_HostEpoch(uint16_t id, bool resend, bool rejoin) {
    if (!g_live || !g_host) return 0;
    size_t since = g_rec.size();
    bool hit = false;
    if (resend) { // the buffer the last load used: the stream starts now (paused until JOINED), with its runtime chunks
        HostReset(), since = 0;
        // 7.2: that buffer's bytes have a log offset: the records since then go first, so the guest replays what it lacks,
        // with the chunks that save holds. ponytail: 8 MB at most (the guest breaks past 16 MB waiting), else stale
        int n;
        const uint8_t* b = World_CurrentState(n);
        auto st = b && g_logOn ? g_stamps.find(Fnv(b, n)) : g_stamps.end();
        size_t len = st != g_stamps.end() && st->second.off <= g_log.size() ? g_log.size() - st->second.off : SIZE_MAX;
        hit = len <= kLiveCap / 2 && g_rec.size() + sizeof(RUids) + st->second.uids.size() * 4 + len <= g_rec.capacity();
        Log("destr: resend: %u chunks with a runtime uid noted for the guest", PutUids(hit ? st->second.uids : UidSnap()));
        g_logAt = g_rec.size();
        if (hit) {
            g_rec.insert(g_rec.end(), g_log.begin() + st->second.off, g_log.end()), g_logAt = g_rec.size();
            Log("destr: re-join log: %u bytes since the sent buffer (offset %u of %u) go first", (unsigned)len,
                (unsigned)st->second.off, (unsigned)g_log.size());
        } else if (rejoin && Destr_SinceLoad() > 0)
            Log("destr: no log base for the sent buffer (hash %08X%s): stale", b ? Fnv(b, n) : 0,
                !g_logOn ? ", log off" : len != SIZE_MAX ? ", log too long" : "");
    }
    g_hId = id, g_hDestr = HostByte(rejoin && !hit && Destr_SinceLoad() > 0), g_pkt = 0, g_hMatch = g_hDiff = g_hNv = 0;
    // fresh capture, or a resend plus its log: B's copy holds all our damage but what the game never saves (repairable)
    g_hFresh = !resend || hit, g_repaired.clear();
    gc_profile = (g_hDestr & kModeMask) && (g_hDestr & kProfileBit);
    Log("destr: epoch %u (%s): mode %d%s, profile %s, own-pack %s; %u bytes recorded since the capture", id,
        resend ? "the loaded buffer" : "a fresh capture", g_hDestr & kModeMask, g_hDestr & kStaleBit ? " (stale)" : "",
        gc_profile ? "on" : "off", g_hDestr & kOwnPack ? "on" : "off", (unsigned)since);
    return g_hDestr;
}

int Destr_SinceLoad() { return g_sinceLoad + g_restSinceLoad; }

// false only between a capture's stamp, the 16 MB end or our load and the next blob, in a live session (mode 0 never
// freezes the guest's destruction: ver6.4's wait)
bool Destr_Streaming() { return !g_live || !g_sync || g_hId; }

void Destr_HostJoined(uint16_t id) { // the resync check: one census per tracked building as it is here now
    if (!g_live || !g_host || id != g_hId) return;
    std::vector<MsgCensus> v;
    int all = Destr_Chunks();
    for (Bld& b : g_bld)
        if (Cen c = Census(b.h, true); c.ok) v.push_back(MakeCensus(b, c, 1, all));
    size_t n = v.size();
    if (!n) { // no building tracked: one empty census, so the guest still logs its line
        MsgCensus m{};
        m.type = kCensus, m.kind = 1, m.epoch = g_hId, m.seq = g_seq, m.handle = ~0u, m.chunksAll = all;
        v.push_back(m);
    }
    for (size_t i = 0; i < v.size(); i++) v[i].index = (uint16_t)i, v[i].count = (uint16_t)n, g_cenOut.push_back({g_rec.size(), v[i]});
    Log("destr: resync check for blob %u: %u buildings", id, (unsigned)n);
}

void Destr_GuestBegin(uint16_t id, uint8_t destr) {
    if (!g_live || g_host) return;
    GuestReset();
    g_gEpoch = id, g_gDestr = destr;
    int mode = destr & kModeMask;
    g_mode = mode ? kReplay : kOff;
    gc_profile = mode && (destr & kProfileBit), gc_rfglGuest = mode == 2; // mode 2: no local debris deleter (host DESPAWNs)
    gc_gm = mode == 2 && (destr & kGmBit);                                 // 7.2 GF-GM: general movers die by the host's GM_DEAD
    Puppet_SetWorldBit(!mode);                                              // §6: the world bit follows the epoch's mode
    Log("destr: mode %d from blob %u (profile %s, pack world bit %s%s%s)", mode, id, gc_profile ? "on" : "off", mode ? "off" : "on",
        destr & kOwnPack ? ", own-pack" : "", destr & kStaleBit ? ", stale" : "");
}

void Destr_GuestDrop() {
    if (!g_live || g_host) return;
    g_brokeEpoch = 0; // the host's load or a link change: a fresh world comes, so a re-join after a broken stream is moot
    if (!g_gEpoch) return;
    Log("destr: the stream of blob %u is dropped (the host is loading or the link changed)", g_gEpoch);
    GuestReset(), g_gEpoch = 0;
}

void Repair(uint32_t h) { // host, sim thread: world::restore_object(h) only queues it (Pending_restorations, 0x534B8F)
    AcquireSRWLockExclusive(&g_noteLock);
    g_mine.insert(h); // before the call: our entry detour notes it right away (OnRestore)
    ReleaseSRWLockExclusive(&g_noteLock);
    bool q = Fn<uint8_t __thiscall(void*, uint32_t)>(site::RestoreObject)(Rva(rva::World), h) & 1;
    if (!q) { // not in the hierarchy any more: nothing to repair
        AcquireSRWLockExclusive(&g_noteLock);
        g_mine.erase(h);
        ReleaseSRWLockExclusive(&g_noteLock);
    }
    // until our regrow (off screen, later) B skips its events: hits in between would break B's copy only (how 3 unmarks)
    if (q && ((g_hDestr & kModeMask) || g_logOn) && Room(sizeof(RRestore))) Put(RRestore{kRestore, 0, 0, h});
    Log("destr: %08X whole on B, damaged here after a fresh capture (damage the game does not save, e.g. a safehouse): %s", h,
        q ? "repairing ours (regrows when off screen)" : "not repaired (no such object here)");
    if (q) Hud("Co-op: a building the game does not save was repaired to match your partner");
}

void Destr_NetDrop() { if (g_live && !g_host && g_gEpoch) Broken("a net drop"); else Session_RejoinRequest("destruction stream broke"); }

const char* Destr_OnMessage(uint8_t t, const uint8_t* d, size_t n) {
    if (t == kCensusRes) { // host: the guest's verdict on one census
        if (!g_live || !g_host) return "CENSUS_RES off its role";
        if (n != sizeof(MsgCensusRes)) return "CENSUS_RES size";
        MsgCensusRes r;
        memcpy(&r, d, n);
        if (!r.epoch || r.epoch != g_hId) // an older epoch's (sent before our capture's stamp, the 16 MB end or our load)
            return Log("destr: B checked %08X%s for an older epoch %u (not counted)", r.handle, r.verdict & 16 ? " (resync check)" : "",
                       r.epoch), nullptr;
        bool ok = (r.verdict & 3) == 3, rs = r.verdict & 16, nv = r.verdict & 8;
        if (nv) Log("destr: B checked %08X%s: no verdict", r.handle, rs ? " (resync check)" : "");
        else Log("destr: B checked %08X%s: identity %s static %s (part %s), B chunks %d", r.handle, rs ? " (resync check)" : "",
                 r.verdict & 1 ? "OK" : "BAD", r.verdict & 2 ? "MATCH" : "DIFF", r.verdict & 4 ? "MATCH" : "DIFF", (int)r.chunks);
        // a fresh capture's resync check: same building in place, whole there, damaged here = damage the game never saves
        // (safehouses): repair ours as the game would at our next load. Once per building and epoch
        // (only a building we track: a guest names any handle here)
        uint8_t* m = rs && g_hFresh && !nv && (r.verdict & 0x23) == 0x21 && FindBld(r.handle) ? Obj(r.handle) : nullptr;
        if (Mover(m) && !Field<int>(m, 0xF8) && g_repaired.insert(r.handle).second) Repair(r.handle);
        if (rs) return nullptr; // B shows the resync check as one summary line (ResyncLine); A only logs it, apart from its tally
        (nv ? g_hNv : ok ? g_hMatch : g_hDiff)++;
        char h[96];
        snprintf(h, sizeof h, "Destruction check: %d MATCH, %d DIFF, %d no verdict", g_hMatch, g_hDiff, g_hNv);
        Hud(h);
        return nullptr;
    }
    if (!g_live || g_host) return "destruction stream off its role";
    if (t == kDestr) {
        MsgDestr h;
        if (n < sizeof h) return "DESTR size";
        memcpy(&h, d, sizeof h);
        if (h.len < 1 || h.len > kDestrBytes || n != sizeof h + h.len) return "DESTR size";
        if (!g_gEpoch || h.epoch != g_gEpoch) return g_ls.staleIn++, nullptr; // another epoch's: dropped, counted
        if (h.pkt != g_gPkt + 1) {
            char why[64];
            snprintf(why, sizeof why, "packet %u where %u was due", h.pkt, g_gPkt + 1);
            return Broken(why), nullptr;
        }
        g_gPkt = h.pkt;
        // before the insert, the host's cap: the buffer never doubles past ~32 MB in this 32-bit process
        if (g_rec.size() - g_rd + h.len > kLiveCap) return Broken("over 16 MB waiting"), nullptr; // a guest that never lands or applies
        try { // called from game frames (OnMultiProcess): a failed reallocation must never throw there
            g_rec.insert(g_rec.end(), d + sizeof h, d + n);
        } catch (...) {
            return Broken("no memory for the stream"), nullptr; // an append: g_rec is unchanged, Broken clears it
        }
        g_ls.pkts++, g_ls.raw += h.len;
        return nullptr;
    }
    if (t == kCensus) {
        if (n != sizeof(MsgCensus)) return "CENSUS size";
        MsgCensus c;
        memcpy(&c, d, n);
        if (!g_gEpoch || c.epoch != g_gEpoch) return g_ls.staleIn++, nullptr;
        if (!std::isfinite(c.pos[0]) || !std::isfinite(c.pos[1]) || !std::isfinite(c.pos[2]) || (c.count && c.index >= c.count))
            return "CENSUS values";
        try { // evaluated in stream order (GuestTick)
            g_rec.push_back(kCensusRec), g_rec.insert(g_rec.end(), d, d + n);
        } catch (...) {
            return Broken("no memory for the stream"), nullptr;
        }
        g_ls.census++;
        return nullptr;
    }
    return "type off channel 2";
}

void Destr_Crush(uint8_t* victim, uint8_t* attacker, int type) { // M7a data: who hurts whom while a building comes down
    if (!g_dev || !g_live || !victim || NowMs() - g_lastEvent >= 10000) return;
    uint32_t pup = Puppet_Handle();
    int v = victim == Me() ? 0 : Handle(victim) == pup ? 1 : 2;
    int a = !attacker ? 0 : Field<int8_t>(attacker, 0x7E) == 2 ? 1 : Handle(attacker) == pup ? 2 : 3;
    g_crush[(uint32_t)v << 16 | (uint32_t)a << 8 | (uint8_t)type]++;
}
