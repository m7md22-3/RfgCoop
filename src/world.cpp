// World blob (E3, and the M5 join through proto.h "world.cpp") and the E8 handle dump, RESEARCH.md §6 / §4.8.
// The capture and load pieces are exported for session.cpp (join / re-join); the dev keys use the same ones.
// dev=1 only, keys read on the async high bit while the game window is focused and in gameplay (GS 1) with a Player:
//   F9   E3 capture: a silent world blob (free roam only), copied to our heap. No disk access.
//   F10  E3 load: the captured blob from memory (§4.8 step 3). Arms the save guard first.
//   F11  arm the save guard by hand (the "die with the guard on" check).
//   F12  E8: RfgCoop_objects_<tag>.txt next to the DLL, every object within 300 m, sorted by handle.
// F9 / F10 are refused while a co-op session runs or once this game has been a guest (its world is the host's).
// F1-F12 are unbound in the game's default keyboard map; RfgCoop uses F6 (ghost) and F7/F8 (session); keen::wndProc 0x80EA5A swallows SC_KEYMENU, so F10 opens no menu.
// Save guard: the only keen::SaveDataHandler::requestSave call (0x811278, in unit4::LocalGameSession::startSave) is
// chained to a refusal, once, until the process exits; the config.ini write that follows is cancelled there too.
// Capture: gamestate_gp_handshake's `call process_save_load` (0x3D9159) is chained on the first request, so the save
// bits are set right before process_save_load and no vanilla autosave of that frame is dropped. Every address, offset
// and ABI: research/notes/world_impl.md.
#include "features.h"
#include "game.h"
#include "proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace site {
// world (rva::World), PDB types `world`, `world_state_buf`, `base_array<object *>`
constexpr uintptr_t WObjects    = 0x64;   // all_objects.elt (object**), base_array at +0x60
constexpr uintptr_t WNumObjects = 0x6C;   // all_objects.num
constexpr uintptr_t WSaveFile   = 0x24D0; // cfile* save_file (open while a load parses the blob)
constexpr uintptr_t WDeser      = 0x24E0; // deserialize_state, 0 = DS_IDLE
constexpr uintptr_t WCurState   = 0x2578; // cur_world_state: 0 anchor, 1 checkpoint (index into stored_world_state)
constexpr uintptr_t WState0     = 0x257C; // stored_world_state[0], 0x6C each: +0x60 buf, +0x64 cur_size, +0x68 max_size
constexpr uintptr_t WFlags      = 0x265C; // save_load_info: 0x265C bit2 pending_save_game, bit5 performing_save_state;
                                          // 0x265D bit0 checkpoint, bit1 to_disk, bit4 saving_state_data
constexpr uintptr_t WDlc        = 0x2694; // int dlc_bundle_id
// game functions (prologue and a vanilla call site read for each)
constexpr uintptr_t GetStateData  = 0x0051C460; // world_state_buf* __thiscall world::get_state_data(): [cur] if cur_size > 0
constexpr uintptr_t GetLoadBuf    = 0x0051C970; // uint8_t* __thiscall world::get_load_buf(): stored_world_state[cur].buf
constexpr uintptr_t SetCurSize    = 0x0051C990; // void __thiscall world::set_cur_size(int), ret 4
constexpr uintptr_t PlatformLoad  = 0x0051C9B0; // void __thiscall world::platform_load(game_save_info*, bool), ret 8
constexpr uintptr_t SetPending    = 0x0051C320; // bool __thiscall world::set_pending_territory_load(char* name, bool new_game,
                                                //   bool single_zone, bool safehouse, int dlc, bool reset_destruction), ret 0x18
constexpr uintptr_t DiffOverwrite = 0x0035CEA0; // void __cdecl difficulty_set_level_reload_overwrite(int)
constexpr uintptr_t HashString    = 0x001936C0; // unsigned __cdecl str_hash_string(char*): how class hashes are made (0xC75409)
constexpr uintptr_t HudMessageAdd = 0x00455D30; // hud_message_handle __cdecl(char*, float, void*, void*, int*)
constexpr uintptr_t RequestSaveCall = 0x00811278; // startSave: `52 E8 rel32` push edx / call requestSave (its only caller)
constexpr uintptr_t RequestSave   = 0x008CD880; //   bool __thiscall keen::SaveDataHandler::requestSave(unsigned user), ret 4
constexpr uintptr_t IniWrite      = 0x021328E8; // uint Save_system_current_ini_write (vs Save_system_last_ini_write 0x21328EC)
constexpr uintptr_t HandshakeSaveLoad = 0x003D9159; // gamestate_gp_handshake: `E8 rel32` -> process_save_load, its only
constexpr uintptr_t ProcessSaveLoad   = 0x003C00E0; //   gameplay caller; void __cdecl(), reads only globals, plain ret
// globals
constexpr uintptr_t MissionActive = 0x0125050C; // int Mission_active_index, -1 = none (mission_is_active 0x35E970)
constexpr uintptr_t StreamPause   = 0x028A0670; // bool Stream_pause
constexpr uintptr_t LoadDistrict  = 0x028531D4; // int[2] Loading_screen_district (loading_screen_set_district 0x489020)
constexpr uintptr_t Hammers       = 0x02C256B0; // uint64 s_hammers_unlocked (meta_data_get_hammers_unlocked 0x611CB0)
constexpr uintptr_t GoldenHammer  = 0x02C2E044; // bool Wants_to_start_game_with_golden_hammer
constexpr uintptr_t Backpacks     = 0x02C16C50; // uint16 s_backpacks_unlocked (0x611D40)
constexpr uintptr_t Jetpack       = 0x02C16C52; // uint8 s_jetpack_upgrade_level (0x611D80)
constexpr uintptr_t PlayerHash    = 0x0314F83C; // unsigned player_instantiator (= str_hash_string("player"), 0xC75410)
} // namespace site

// startSave's callers, by the return address on its frame (OnRequestSave)
constexpr uintptr_t kFromWrite = 0x3D0F47, kFromDelete = 0x3C3658, kFromOptions = 0x3D137E, kFromProfile = 0x3D140E;
constexpr uintptr_t kImageSize = 0x397B000; // core.cpp's build check
constexpr float kDumpRadius = 300.0f;
constexpr ULONGLONG kTimeoutMs = 10000;

enum Phase { kIdle, kCapture, kLoad, kArrive }; // the dev keys' own steps
Phase g_phase;
ULONGLONG g_t0;
bool g_dev, g_keys[4], g_left, g_guard;
std::vector<uint8_t> g_blob; // the F9 capture
WorldMeta g_meta;            // dlc id and load globals at the F9 capture
float g_capPos[3];           // Player position at the F9 capture
LONG g_skipped, g_skippedShown;
// the one capture in flight (F9 or the session's join): when it was asked, want = its save bits not set yet (the
// 0x3D9159 chain sets them), buffer 0's size and hash right before they were set
struct { bool busy, want; ULONGLONG t0; int oldSize; uint32_t oldHash; } g_cap;
void(__cdecl* g_saveLoad)(); // what 0x3D9159 called before us: process_save_load (checked)

uint8_t* World() { return Rva<uint8_t>(rva::World); }
uint8_t* LocalPlayer() { return *Rva<uint8_t*>(rva::Player); }
void Hud(const char* text) { // gameplay only: the capture and load phases also tick through menus and loads
    if (GameState() == 1) Fn<int __cdecl(char*, float, void*, void*, int*)>(site::HudMessageAdd)((char*)text, 4.0f, nullptr, nullptr, nullptr);
}

// ---- save guard ----------------------------------------------------------------------------------------------------
// Replaces the call at 0x811278. Returning 0 is requestSave's own refusal path (0x8CD8A3 `xor al,al; ret 4`): no request is
// counted (+0x14), so SaveDataHandler::update never starts a write flow (0x8CFC2C) and isSaveInProgress stays false. No
// startSave caller reads the result. The in-RAM save image is still updated (game_save_write_thread copies the slot first).
// config.ini: the write, options and profile callers then run `inc Save_system_current_ini_write` (at their return
// address) and signal game_save_ini_thread, which writes config.ini (game_options_write_ini_file_from_cache 0x3EE840, its
// only call 0x3EE98B) only while current > last (0x3EE96A-0x3EE975, unsigned jbe). Decremented here, after the Log, the
// caller's `inc` nets it to 0 and nothing is written. ponytail: the gap between our decrement and the caller's inc is a
// few instructions (startSave's `pop esi; ret 4`, 0x81127D). Two guarded saves on different threads overlapping there,
// with config.ini never written this run (last == 0), let one config.ini write through (the ini thread reads
// 0xFFFFFFFF); last = 0xFFFFFFFF then stops all later ones. Options only.
uint32_t __thiscall OnRequestSave(void*, unsigned user) {
    // frame: [ebp+4] = 0x81127D in startSave, [ebp+8] = user, [ebp+0xC] = startSave's saved esi, [ebp+0x10] = its caller
    uintptr_t from = (uintptr_t)((void**)__builtin_frame_address(0))[4] - g_base;
    const char* what = from == kFromWrite ? "save/autosave" : from == kFromDelete ? "save delete"
                     : from == kFromOptions ? "options" : from == kFromProfile ? "profile data" : "unknown";
    Log("world: save guard skipped disk write #%ld: %s (startSave from rfg+%X, user %u)", InterlockedIncrement(&g_skipped),
        what, (unsigned)from, user);
    // last, so the caller's `inc` follows within a few instructions, not after Log's file I/O
    if (from == kFromWrite || from == kFromOptions || from == kFromProfile) InterlockedDecrement(Rva<LONG>(site::IniWrite));
    return 0;
}

bool ArmGuard(const char* why) { // once per process; World_ArmSaveGuard is the only other way in (never chain twice)
    if (g_guard) return true;
    uint8_t* s = Rva<uint8_t>(site::RequestSaveCall);
    if (s[-1] != 0x52 || s[0] != 0xE8 || (uintptr_t)(s + 5 + *(int32_t*)(s + 1)) != g_base + site::RequestSave) {
        Log("world: save guard (%s): 0x811278 is not `push edx; call requestSave` (a sibling chained it?), not armed", why);
        return false;
    }
    ChainRel32(site::RequestSaveCall, (void*)&OnRequestSave);
    g_guard = true;
    Log("world: save guard armed at 811278 until the game exits (%s)", why);
    Hud("Save guard ON: this game writes no saves until you restart it");
    return true;
}

// ---- capture (F9, join) ------------------------------------------------------------------------------------------
// World+0x2578 (cur_world_state) is not checked: it can stay 1 in free roam after a checkpoint mission that skips its
// result-screen save (Save the Guerrilla Camp; mission_terminate_internal 0x3B4B76-0x3B4BE3), and the silent save
// itself sets it back to 0 (save_state_data 0x541279, 0x265D bit 0 clear), as RESEARCH.md §4.8 step 2 intends.
const char* NotFreeRoam(uint8_t* w) {
    if (*Rva<int>(site::MissionActive) != -1) return "a mission is active";
    if (*Rva<uint8_t>(site::StreamPause)) return "streaming is paused";
    if (Field<uint8_t>(w, site::WFlags + 1) & 0x10) return "the game is saving";
    // a pending vanilla save: clearing its to_disk bit would turn it into a silent one (it goes first, we follow)
    if (Field<uint8_t>(w, site::WFlags) & 0x24) return "a save is pending";
    return nullptr;
}

// Chained at 0x3D9159 (gamestate_gp_handshake's `call process_save_load`). A requested capture sets its save bits here,
// right before process_save_load reads them, not from the tick at the top of rfg_do_frame (0x1D24D9): the frame's
// simulation runs in between (gameseq_process 0x1D24FF, handshake 0x1D2533), and a vanilla save_and_continue there
// sees bit 2 and returns (0x51D049) while its caller clears its own request, so that autosave was gone (free-roam death
// 0x3EDCF0, activity 0x3ACC58, mission 0x3B4D93 / 0x38E4D1). NotFreeRoam again, as a mission, Stream_pause or a vanilla
// save may have started this frame: then the request waits for the next frame (a vanilla save's own bit 2 goes first).
void __cdecl OnProcessSaveLoad() {
    if (g_cap.want && !NotFreeRoam(World())) {
        uint8_t *w = World(), *s0 = w + site::WState0;
        g_cap.oldSize = Field<int>(s0, 0x64);
        g_cap.oldHash = g_cap.oldSize > 0 ? Fnv(Field<uint8_t*>(s0, 0x60), g_cap.oldSize) : 0;
        Field<uint8_t>(w, site::WFlags + 1) &= ~3; // not a checkpoint (cur_world_state 0), not to disk
        Field<uint8_t>(w, site::WFlags) |= 4;      // pending_save_game: process_save_load runs save_state_data now
        g_cap.want = false;
        Destr_Stamp(); // E4: the blob holds every destruction step before this one
    }
    // 7.2 re-join log: a save rewrote a stored buffer (a vanilla save, a checkpoint, our capture): its bytes get a log offset
    uint8_t *w = World(), *s0 = w + site::WState0, *s1 = s0 + 0x6C;
    bool saving = Field<uint8_t>(w, site::WFlags) & 4;
    int n0 = Field<int>(s0, 0x64), n1 = Field<int>(s1, 0x64);
    g_saveLoad();
    if (saving || n0 != Field<int>(s0, 0x64) || n1 != Field<int>(s1, 0x64)) Destr_Saved();
}

bool HookSaveLoad() { // once per process: the first capture request, or a host's link (7.2 re-join log stamps)
    if (g_saveLoad) return true;
    uint8_t* s = Rva<uint8_t>(site::HandshakeSaveLoad);
    if (s[0] != 0xE8 || (uintptr_t)(s + 5 + *(int32_t*)(s + 1)) != g_base + site::ProcessSaveLoad) {
        Log("world: 3D9159 is not `call process_save_load` (a sibling chained it?): no capture");
        return false;
    }
    g_saveLoad = (void(__cdecl*)())ChainRel32(site::HandshakeSaveLoad, (void*)&OnProcessSaveLoad);
    Log("world: capture hook at 3D9159 (gameplay handshake, process_save_load)");
    return true;
}

// ---- E8 (F12) ------------------------------------------------------------------------------------------------------
// object::get_classname is vtable slot 3 (+0xC, object::`vftable' 0xED611C, automobile 0xF3817C). All 60 overrides are
// `B8 imm32 C3` (mov eax, string; ret): read the string instead of calling into the object. NULL if the shape differs.
const char* ClassName(uint8_t* o) {
    uintptr_t lo = g_base, hi = g_base + kImageSize, vt = Field<uintptr_t>(o, 0);
    if (vt < lo || vt + 0x10 > hi) return nullptr;
    uint8_t* code = *(uint8_t**)(vt + 0xC);
    if ((uintptr_t)code < lo || (uintptr_t)code + 6 > hi || code[0] != 0xB8 || code[5] != 0xC3) return nullptr;
    uintptr_t s = *(uintptr_t*)(code + 1); // relocated by the loader like any code operand
    return s >= lo && s < hi ? (const char*)s : nullptr;
}

void Dump(uint8_t* me) {
    struct Obj { unsigned handle, hash; int type, sub; float x, y, z; const char* name; };
    uint8_t* w = World();
    uint8_t** all = Field<uint8_t**>(w, site::WObjects);
    int n = Field<int>(w, site::WNumObjects);
    float* at = &Field<float>(me, 0x04);
    auto strHash = Fn<unsigned __cdecl(const char*)>(site::HashString);
    std::vector<Obj> v; // read-only pass on the sim thread; the only game call is the pure string hash
    for (int i = 0; i < n; i++) {
        uint8_t* o = all[i];
        if (!o) continue;
        float* p = &Field<float>(o, 0x04);
        float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
        if (dx * dx + dy * dy + dz * dz > kDumpRadius * kDumpRadius) continue;
        const char* name = ClassName(o);
        v.push_back({Field<unsigned>(o, 0x6C), name ? strHash(name) : 0, Field<int8_t>(o, 0x7E), Field<int8_t>(o, 0x7F), p[0], p[1],
                     p[2], name ? name : "?"});
    }
    std::sort(v.begin(), v.end(), [](const Obj& a, const Obj& b) { return a.handle < b.handle; });
    wchar_t path[MAX_PATH], tag[16];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Dump, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    IniStr(L"tag", tag, 16);
    wchar_t* file = wcsrchr(path, L'\\') + 1;
    swprintf(file, MAX_PATH - (file - path), L"RfgCoop_objects_%ls.txt", *tag ? tag : L"A");
    FILE* f = _wfopen(path, L"w");
    if (!f) return Log("world: E8 could not write %ls", path), Hud("Object dump failed (RfgCoop.log)");
    fprintf(f, "# RfgCoop E8: %d of %d objects within %.0f m of %.1f %.1f %.1f, dlc %d\n"
               "# handle   classhash type sub          x          y          z class\n",
            (int)v.size(), n, kDumpRadius, at[0], at[1], at[2], Field<int>(w, site::WDlc));
    for (const Obj& o : v)
        fprintf(f, "%08X %08X %4d %3d %10.2f %10.2f %10.2f %s\n", o.handle, o.hash, o.type, o.sub, o.x, o.y, o.z, o.name);
    fclose(f);
    const char* mine = ClassName(me); // method self-check: must equal the game's own player class hash
    Log("world: E8 wrote %d objects to %ls; self-check: hash(\"%s\") %08X, player_instantiator %08X", (int)v.size(), path,
        mine ? mine : "?", mine ? strHash(mine) : 0, *Rva<unsigned>(site::PlayerHash));
    char msg[96];
    snprintf(msg, sizeof msg, "Objects dumped: %d within 300 m", (int)v.size());
    Hud(msg);
}

void OnKey(int k, uint8_t* me) {
    if (g_phase == kCapture || g_phase == kLoad) return Hud("World: busy, wait a moment");
    // the session owns the world blob while it runs, and a guest's world is the host's until the game restarts
    if (k <= 1 && (Session_Active() || Guest_Armed())) return Hud("World: F9 / F10 are off during co-op");
    if (k <= 1 && Destr_Busy()) return Hud("World: F9 / F10 are off during E4"); // F10 would swap the world E4 loads
    char msg[120];
    switch (k) {
    case 0:
        if (const char* why = World_CaptureStart()) {
            snprintf(msg, sizeof msg, "World capture refused: %s", why);
            return Log("world: %s", msg), Hud(msg);
        }
        memcpy(g_capPos, &Field<float>(me, 0x04), sizeof g_capPos);
        g_phase = kCapture;
        return;
    case 1:
        if (g_blob.empty()) return Hud("World: press F9 first to capture");
        if (!ArmGuard("F10")) return Hud("Save guard unavailable (RfgCoop.log): world load refused");
        g_phase = kLoad, g_t0 = GetTickCount64();
        return;
    case 2:
        if (g_guard) return Hud("Save guard is already ON");
        if (!ArmGuard("F11")) Hud("Save guard unavailable (RfgCoop.log)");
        return;
    case 3: return Dump(me);
    }
}

} // namespace

// ---- exports (proto.h "world.cpp"; features.h World_ArmSaveGuard) ------------------------------------------------------
bool World_ArmSaveGuard(const char* why) { return ArmGuard(why); }

const char* World_NotFreeRoam() { return NotFreeRoam(World()); }

bool World_InMission() { return *Rva<int>(site::MissionActive) != -1; } // not 0x2578: see NotFreeRoam

// The next free-roam gameplay handshake sets the save bits (OnProcessSaveLoad); process_save_load then runs
// world::save_state_data synchronously: cur_world_state = 0 (not a checkpoint), the blob lands in stored_world_state[0],
// and no platform_save_to_disk (to_disk clear, 0x3C00F3)
const char* World_CaptureStart() {
    if (g_cap.busy) return "a capture is running";
    if (const char* why = NotFreeRoam(World())) return why;
    if (!HookSaveLoad()) return "the capture hook is unavailable (RfgCoop.log)";
    g_cap.busy = g_cap.want = true, g_cap.t0 = GetTickCount64();
    Log("world: capture requested");
    return nullptr;
}

int World_CapturePoll(std::vector<uint8_t>& out) {
    if (!g_cap.busy) return -1;
    uint8_t* w = World();
    // pending: no free-roam handshake yet, or (defensive: process_save_load clears it in the same call, 0x3C013D) set
    // and not consumed
    if (g_cap.want || (Field<uint8_t>(w, site::WFlags) & 4)) {
        if (GetTickCount64() - g_cap.t0 < kTimeoutMs) return 0;
        // set by us: save_and_continue refuses while it is set (0x51D049), so nobody else set it
        if (!g_cap.want) Field<uint8_t>(w, site::WFlags) &= ~4;
        Log("world: capture timed out (%s), request withdrawn",
            g_cap.want ? "no gameplay handshake in free roam" : "the save bits were not consumed");
        g_cap.busy = g_cap.want = false;
        return -1;
    }
    g_cap.busy = false;
    uint8_t* ws = Fn<uint8_t* __thiscall(void*)>(site::GetStateData)(w);
    if (!ws || ws != w + site::WState0) return Log("world: capture failed: no blob in buffer 0"), -1;
    uint8_t* buf = Field<uint8_t*>(ws, 0x60);
    int n = Field<int>(ws, 0x64);
    uint32_t h = Fnv(buf, n);
    if (n == g_cap.oldSize && h == g_cap.oldHash) // save_state_data returns the old buffer when do_frame skipped the save
        return Log("world: capture stale: buffer 0 unchanged (%d bytes)", n), -1;
    out.assign(buf, buf + n);
    char head[40] = {}; // "RFGS", version 0x34, then the start zone string (world::init_territory 0x53FD83 / 0x53FDCC)
    for (int i = 0; i < 32 && 8 + i < n; i++) head[i] = buf[8 + i] >= 32 && buf[8 + i] < 127 ? buf[8 + i] : '.';
    Log("world: captured %d bytes (was %d) in %llu ms, max %d, magic %08X (RFGS = 53474652), version %u, dlc %d, fnv %08X, "
        "then '%s'", n, g_cap.oldSize, GetTickCount64() - g_cap.t0, Field<int>(ws, 0x68), n >= 4 ? *(uint32_t*)buf : 0,
        n >= 8 ? *(uint32_t*)(buf + 4) : 0, Field<int>(w, site::WDlc), h, head);
    return 1;
}

// world::get_state_data 0x51C460: stored_world_state[World+0x2578] when its cur_size > 0. After a load it holds what
// init_territory loaded (0x53FD32; a disk load copied the save there first, 0x3F0280), unless a save wrote it since
bool World_HookSaves() { return HookSaveLoad(); }
const uint8_t* World_StateBuf(int i, int& n) { // stored_world_state[i] (0 anchor, 1 checkpoint) as it is now
    uint8_t* s = World() + site::WState0 + 0x6C * i;
    n = Field<int>(s, 0x64);
    uint8_t* buf = Field<uint8_t*>(s, 0x60);
    return buf && n > 0 && n <= Field<int>(s, 0x68) ? buf : (n = 0, nullptr);
}

const uint8_t* World_CurrentState(int& n) {
    uint8_t* ws = Fn<uint8_t* __thiscall(void*)>(site::GetStateData)(World());
    n = ws ? Field<int>(ws, 0x64) : 0;
    uint8_t* buf = ws ? Field<uint8_t*>(ws, 0x60) : nullptr;
    return buf && n <= Field<int>(ws, 0x68) && World_BlobOk(buf, n) ? buf : nullptr;
}

void World_ReadMeta(WorldMeta& m) {
    m.dlc = Field<int>(World(), site::WDlc);
    memcpy(m.hammers, Rva(site::Hammers), 8);
    m.golden = *Rva<uint8_t>(site::GoldenHammer);
    memcpy(m.backpacks, Rva(site::Backpacks), 2);
    m.jetpack = *Rva<uint8_t>(site::Jetpack);
}

bool World_BlobOk(const uint8_t* p, int n) { // what init_territory checks (0x53FD83 / 0x53FDCC), plus our size bound
    return p && n >= 8 && (uint32_t)n <= kBlobMax && *(const uint32_t*)p == 0x53474652 && *(const uint32_t*)(p + 4) == 52;
}

int World_Load(const uint8_t* p, int n, const WorldMeta& m, bool session) {
    uint8_t* w = World();
    // set_pending_territory_load's own refusals (0x51C328-0x51C34C): checked first, so it cannot refuse after platform_load
    if (Field<int>(w, site::WDeser) || (Field<uint8_t>(w, site::WFlags) & 0x24) || Field<void*>(w, site::WSaveFile))
        return 0;
    int max = Field<int>(w, site::WState0 + 0x68); // buffer 0, the one we load into
    if (!World_BlobOk(p, n) || !Field<uint8_t*>(w, site::WState0 + 0x60) || n > max) {
        Log("world: load refused: buffer 0 %p, blob %d bytes, max %d, header %s", Field<void*>(w, site::WState0 + 0x60), n,
            max, World_BlobOk(p, n) ? "RFGS v52" : "not RFGS v52");
        return -1;
    }
    Field<int>(w, site::WCurState) = 0; // get_load_buf, set_cur_size and init_territory (0x53FD32) all index by it
    memcpy(Fn<uint8_t* __thiscall(void*)>(site::GetLoadBuf)(w), p, n);
    Fn<void __thiscall(void*, int)>(site::SetCurSize)(w, n);
    // game_save_info (0xBC) with what platform_load reads (0x51CA4C-0x51CA98), laid out as platform_save_to_disk fills it
    // (0x52D7F8-0x52D822): the loading-screen district is this game's, the rest is m (on a guest the host's, rule Q13:
    // hammers, backpacks and jetpack are process globals platform_load sets from here)
    uint8_t info[0xBC] = {};
    memcpy(info + 0x0C, Rva(site::LoadDistrict), 8); // +0xC territory, +0x10 district: loading screen
    memcpy(info + 0x30, &m.dlc, 4);                   // dlc id -> World+0x2694
    memcpy(info + 0x3A, m.hammers, 8);                // +0x38 = 0 | qword +0x3A
    info[0x39] = m.golden;
    memcpy(info + 0x42, m.backpacks, 2);
    info[0x44] = m.jetpack;
    Fn<void __cdecl(int)>(site::DiffOverwrite)(-1);     // the blob's difficulty, as every vanilla load (0x3F0318)
    Fn<void __thiscall(void*, void*, int)>(site::PlatformLoad)(w, info, 1); // 1: cur_world_state = 0
    if (session) Guest_AllowLoad(); // guest.cpp's 0x51C320 fence lets exactly the next call through
    if (!(Fn<uint32_t __thiscall(void*, const char*, int, int, int, int, int)>(site::SetPending)(w, "", 0, 0, 0, m.dlc, 0) & 0xFF)) {
        Log("world: set_pending_territory_load refused, retrying");
        return 0;
    }
    Log("world: loading a blob from memory (%d bytes, dlc %d, mission %d, %s)", n, m.dlc, *Rva<int>(site::MissionActive),
        session ? "co-op" : "F10");
    return 1;
}

void World_Install() {
    g_dev = IniInt(L"dev", 0) == 1;
    if (g_dev) Log("world: F9 capture, F10 load from memory (arms the save guard), F11 save guard, F12 object dump");
}

void World_Tick(int state) {
    if (state == 1 && g_skipped != g_skippedShown) { // with or without dev=1: a co-op guest arms the guard too
        g_skippedShown = g_skipped;
        char msg[96];
        snprintf(msg, sizeof msg, "Save guard: disk write #%ld skipped", g_skippedShown);
        Hud(msg);
    }
    if (!g_dev || *Rva<uint8_t>(rva::Multiplayer)) return;
    uint8_t* me = LocalPlayer();
    bool focus = *Rva<uint8_t>(rva::OsAppActive);
    static const int vk[4] = {VK_F9, VK_F10, VK_F11, VK_F12};
    for (int k = 0; k < 4; k++) {
        bool down = focus && GetAsyncKeyState(vk[k]) < 0;
        if (down && !g_keys[k] && state == 1 && me) OnKey(k, me);
        g_keys[k] = down;
    }
    if (g_phase == kCapture) {
        std::vector<uint8_t> blob;
        int r = World_CapturePoll(blob);
        if (r == 0) return;
        g_phase = kIdle;
        if (r < 0) return Hud("World capture failed (RfgCoop.log)");
        bool replaced = !g_blob.empty();
        g_blob.swap(blob);
        World_ReadMeta(g_meta);
        char msg[120];
        snprintf(msg, sizeof msg, "World captured: %.2f MB (F10 loads it back)%s", g_blob.size() / 1048576.0,
                 replaced ? ". Earlier snapshot replaced" : "");
        Hud(msg);
    } else if (g_phase == kLoad) { // retried each gameplay frame while the game is busy loading or saving
        int r = state == 1 ? World_Load(g_blob.data(), (int)g_blob.size(), g_meta, false) : 0;
        if (r > 0) g_phase = kArrive, g_left = false, g_t0 = GetTickCount64();
        else if (r < 0) g_phase = kIdle, Hud("World load refused (RfgCoop.log)");
        else if (GetTickCount64() - g_t0 > kTimeoutMs) {
            g_phase = kIdle;
            Log("world: load not started in %llu ms (the game stayed busy)", kTimeoutMs);
            Hud("World load not started (RfgCoop.log)");
        }
    } else if (g_phase == kArrive) { // E3 verdict aid: where the player stands on the first gameplay frame after the load
        if (state != 1) g_left = true;
        else if (g_left && me) {
            g_phase = kIdle;
            float* p = &Field<float>(me, 0x04);
            float dx = p[0] - g_capPos[0], dy = p[1] - g_capPos[1], dz = p[2] - g_capPos[2];
            Log("world: back in gameplay after the load: player at %.1f %.1f %.1f, %.1f m from the capture spot, mission %d, "
                "cur_world_state %d", p[0], p[1], p[2], sqrtf(dx * dx + dy * dy + dz * dz), *Rva<int>(site::MissionActive),
                Field<int>(World(), site::WCurState));
            Hud("World loaded from memory");
        } else if (!g_left && GetTickCount64() - g_t0 > kTimeoutMs) {
            g_phase = kIdle;
            Log("world: the load request was accepted but no load started");
        }
    }
}
