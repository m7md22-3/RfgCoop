// Co-op session (M2 "Link", M4 "See each other"). Sim thread only: Net_Tick runs from the 0x1D24D9 tick, every frame
// in every game state; the network itself is net.cpp's thread.
//   F8 (game focused; starting needs gameplay with a loaded save, both roles) starts / stops the session from
//   RfgCoop.ini [coop] role: the host listens on `port`, the guest dials `connect` and redials every 3 s until linked.
//   A guest's F8 first opens the hammer / backpack picker (rule Q17, picker.cpp); accepting it starts the session.
//   autoconnect=1 starts it on the first gameplay frame. A guest at the main menu ends it (it cannot load from there).
//   Handshake on channel 0: the guest sends HELLO; the host compares it with its own values and answers REJECT with a
//   readable reason (HUD + log on both sides), else CHALLENGE (Nh + the host's per-session salt) / AUTH (AUTH echoes
//   Nh, so the guest's address is real; with a password the guest's HMAC proof), then WELCOME (with a password the
//   host's proof, only after the guest's checked out). Linked: LOADING / READY on every level change, PSTATE 20 times
//   a second both ways
//   (puppet.cpp captures you and drives the partner puppet), BYE with a reason when F8 (or the Tutorial rule) ends it.
//   The host hosts only in gameplay on a save that has finished the Tutorial (RESEARCH.md §8 Q12), re-checked after
//   every load; 5 wrong password proofs from one address = that address is refused for 60 s. A password is empty
//   (open session) or at least 10 characters.
//   F7 (gameplay) teleports you beside the partner, with a streaming wait. F3 (guest, M6) asks the host for a
//   fresh world (its free roam only). M6 (destruction.cpp): each blob id is a destruction epoch; BLOB_BEGIN carries its
//   mode, the stream follows on channel 2 once the guest landed, and a mid-mission re-join after changes is stale.
//   M5 (RESEARCH.md §4.7, §4.8, rules §8 Q13-Q15): once linked the guest arms the save guard and guest.cpp's fences for
//   good; on the guest's first READY (or a REJOIN_REQ before any world) the host makes a silent world blob in free roam
//   (or says "join after the mission"), pauses with its own token
//   and streams it on channel 2; the guest loads it from memory, lands beside the host, JOINED, the host unpauses. Every
//   host load re-sends the buffer that load used, as is; the guest's REJOIN_REQ gets the same (or a fresh blob in free
//   roam). The host's pause is mirrored (PAUSED), its loadout pushed (LOADOUT), the guest's salvage sent to it (SALVAGE).
//   Status in the window title (core's helper thread, never SetWindowText here), HUD lines in gameplay.
// Protocol, keys and every game call: research/notes/session_impl.md.
#include "features.h"
#include "game.h"
#include "proto.h"
#include <bcrypt.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- password proofs (BCrypt). Key = PBKDF2-HMAC-SHA256(UTF-8 password, the host's 16-byte salt, fresh at each host
// start, 100000 rounds), made by the net thread (host at start, guest from CHALLENGE). Proofs bind both nonces of this
// connection, so a recorded exchange is useless later; the salt makes a precomputed password list useless.
bool DeriveKey(const char* password, const uint8_t salt[16], uint8_t key[32]) {
    BCRYPT_ALG_HANDLE a;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return false;
    bool ok = !BCryptDeriveKeyPBKDF2(a, (PUCHAR)password, (ULONG)strlen(password), (PUCHAR)salt, 16, 100000, key, 32, 0);
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

static bool Random(uint8_t* p, ULONG n) { return !BCryptGenRandom(nullptr, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

// HMAC-SHA256(NetKey(), label NUL || a || b || the guest's HELLO as sent); "host" Ng Nh / "guest" Nh Ng
static bool Proof(const char* label, const uint8_t* a, const uint8_t* b, const MsgHello& hello, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return false;
    bool ok = !BCryptCreateHash(alg, &h, nullptr, 0, (PUCHAR)NetKey(), 32, 0) &&
              !BCryptHashData(h, (PUCHAR)label, (ULONG)strlen(label) + 1, 0) && !BCryptHashData(h, (PUCHAR)a, 16, 0) &&
              !BCryptHashData(h, (PUCHAR)b, 16, 0) && !BCryptHashData(h, (PUCHAR)&hello, sizeof hello, 0) &&
              !BCryptFinishHash(h, out, 32, 0);
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static bool Same(const uint8_t* a, const uint8_t* b, int n) { // constant time
    volatile uint8_t d = 0;
    for (int i = 0; i < n; i++) d = d | (a[i] ^ b[i]);
    return !d;
}

namespace {

namespace site {
constexpr uintptr_t HudMessageAdd = 0x00455D30;      // hud_message_handle __cdecl(char*, float, void*, void*, int*)
constexpr uintptr_t ObjFromHandle = 0x0053BEE0;      // object* __thiscall world::object_from_handle(unsigned)
// F7 teleport
constexpr uintptr_t HumanTeleportUnsafe = 0x0067C290; // void __cdecl human_teleport_unsafe(human*, vector, matrix)
constexpr uintptr_t HumanTeleport = 0x0069A3C0;      // void __cdecl human_teleport(human*, vector*, matrix*, float, bool)
constexpr uintptr_t TerrainHeight = 0x0043B440;      // bool __cdecl havok_terrain_height(float*, float, float, bool, vector*)
constexpr uintptr_t SetLinearVel = 0x00423870;       // void __cdecl havok_body_set_linear_velocity(unsigned, vector*)
constexpr uintptr_t SetAngularVel = 0x004239A0;      // void __cdecl havok_body_set_angular_velocity(unsigned, vector*)
constexpr uintptr_t HumanIsDead = 0x001DC0F0;        // bool __cdecl human_is_dead(human*)
constexpr uintptr_t RagdollStop = 0x006C1460;        // bool __cdecl human_ragdoll_stop(human*, bool)
constexpr uintptr_t Frametime = 0x01118568;          // float Frametime_uncompressed (real seconds)
// Q12: farray<mission_info,26> All_mission_infos: +0x4 data, +0xC count, stride 0x60 (mission_is_complete 0x374B20)
constexpr uintptr_t AllMissionInfos = 0x01E45C18;
constexpr uintptr_t MissionIsComplete = 0x00374B20;  // bool __cdecl mission_is_complete(int index): flags +0x58 bit 0
// M5 pause (§4.7). game_pause: `lock xadd` +1 on Game_paused_count, on 0->1 pauses timers and audio (its bool goes to
// audiolib_all_sounds_pause; video_cutscene_play_enter passes 1, 0x707004), caller cleans (0x4A8602 `add esp,4`).
// game_unpause: no stack args, -1, on 1->0 resumes; no floor at 0 (a stray call leaves -1 and the next pause is lost)
constexpr uintptr_t GamePause = 0x003C1620;          // void __cdecl game_pause(bool)
constexpr uintptr_t GameUnpause = 0x003C1650;        // void __cdecl game_unpause()
// int Game_paused_count; gamestate_gp_exit (0x3F0F71) and gamestate_gp_level_init_post (0x3EFE80) unpause it to 0
constexpr uintptr_t PausedCount = 0x02132880;
constexpr uintptr_t StateInfo = 0x0213ADA0;          // state_info[64], 0x24 each: +0x18 transparent, +0x19 pause_beneath
                                                     // (gameseq_do_state 0x3BFB1C / 0x3BFB29, gameseq_push_state 0x3D8875)
constexpr uintptr_t GameClock = 0x021044AC;          // Game_clock: +4 h, +5 m, +6 s bytes (game_clock_save 0x3C1680)
constexpr uintptr_t MissionActive = 0x0125050C;      // int Mission_active_index, -1 = none
// int Current_activity_mode: the running modal activity's type (+0x4), 0 = none (activity_base_modal::activity_start
// 0x3ACA73 sets it, activity_end_common 0x3698CE clears it). Every activity whose death skips the death screen is modal
constexpr uintptr_t ActivityMode = 0x01E28590;
// Q13 loadout. player_meta_data at Player+0x2978 (player_get_meta_data 0x610550): +0x4 salvage (add_salvage 0x611700),
// +0x18 upgrade_item[128], 8 bytes each (add_unlocked_weapons_to_weapons_cabinet 0x611630 steps 8, byte 0 = level).
// Weapon_list: farray<ui_weapon_list_item,64>, +0x4 data, +0xC count, 8 bytes each {u32 weapon handle, u8 flag}
// (hud_ui_weapon_cabinet_unlock_weapon 0x45B3F5-0x45B424; hud_ui_weapon_cabinet_load 0x477280 reads <= 0x40 from the save)
constexpr uintptr_t MetaData = 0x2978;
constexpr uintptr_t WeaponList = 0x01260ED0;
} // namespace site
// GS_MAINMENU, GS_GAMEPLAY, GS_LOAD, GS_PLAYER_DEATH_OPTIONS (PDB type game_state; missions.md "Game states"), the
// mission-complete / fail screen (pushed by hud_ui_mission_complete_do_frame 0x44AE40)
constexpr int kGsMainMenu = 0, kGsGameplay = 1, kGsLoad = 2, kGsDeathOptions = 16, kGsMissionComplete = 0x28;
constexpr int kConvoy = 13; // activity type: rule Q14's one exception (guest.cpp)

struct vector { float x, y, z; };
struct matrix { vector r, u, f; };

// ---- CRC32 (zlib's polynomial): handshake file and data-list hashes
struct CrcTable { uint32_t t[256]; };
constexpr CrcTable MakeCrc() {
    CrcTable c{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t k = i;
        for (int j = 0; j < 8; j++) k = k >> 1 ^ (0xEDB88320u & (0u - (k & 1)));
        c.t[i] = k;
    }
    return c;
}
constexpr CrcTable kCrc = MakeCrc();
template <class B> constexpr uint32_t Crc(uint32_t c, const B* p, size_t n) { // chainable: Crc(Crc(0,a),b) = Crc(0,ab)
    c = ~c;
    for (size_t i = 0; i < n; i++) c = kCrc.t[(c ^ (uint8_t)p[i]) & 0xFF] ^ c >> 8;
    return ~c;
}
static_assert(Crc(0, "123456789", 9) == 0xCBF43926u, "CRC32 check value");

struct {
    bool host, guest;
    char addr[128], name[24];
    uint16_t port, dialPort;
    int autoconnect, lag, jitter, loss;
    bool autoresync; // M6 Q21 (host): a stale guest re-syncs by itself when the mission ends
} g_ini;

enum Phase { Off, Waiting, Shaking, Linked } g_phase; // Waiting: host listening / guest dialling; Shaking: HELLO pending
MsgHello g_hello;      // ours, built on the first start
int g_state, g_sentReady = -1, g_nDrop, g_vppFiles; // g_sentReady: last LOADING (0) / READY (1) sent, -1 none
double g_phaseAt, g_lastSend, g_lastStat, g_lastTitle;
bool g_f7, g_f8, g_autoDone, g_byeSeen;
char g_partner[24], g_title[128], g_hudPending[128], g_byeWhy[120];
// this connection's handshake. Host: the guest's HELLO as received (the proofs cover it), its name, address and our Nh;
// challenged = CHALLENGE sent. Guest: Nh as received; challenged = AUTH sent. g_salt: the host's, per session start.
MsgHello g_peerHello;
uint8_t g_nonce[16], g_salt[16];
bool g_challenged;
char g_peerName[24], g_peerIp[24];
uint8_t* g_checked; // host: the Player whose save passed the Tutorial check (a new one = a load: check again)
uint16_t g_seqOut, g_seqIn;
uint32_t g_out, g_in, g_lost;

// ---- M5 join / re-join (session_impl.md "M5"). Host: g_hj. Guest: g_rx (arriving), g_pend (complete, not loaded yet),
// g_ga (loading it, then landing beside the host).
enum HostJoin { hjIdle, hjWant, hjCapture, hjResend, hjSent } g_hj; // want: a fresh silent blob; resend: the loaded buffer
uint8_t g_hjKind;           // kBlobJoin / kBlobHostLoad / kBlobAsked: the next blob's
uint16_t g_joinId;          // the last blob sent
bool g_blobSent;            // host: this guest has had a world from us (from then on a host load re-sends as is)
bool g_capMine;             // a capture we asked world.cpp for is in flight (polled even after the link ends)
double g_hjAt, g_hjSentAt;  // want: next try; sent: when
uint8_t* g_hjPlayer;        // the Player the capture started under
char g_waitWhy[96];         // the last reason the join waited
bool g_myPause;             // our pause token is held (host: while the guest joins; guest: while the host is paused)
bool g_sentPause, g_hostPaused, g_hostLoading; // host: last PAUSED sent; guest: the host's PAUSED / LOADING
struct Blob { MsgBlobBegin b; std::vector<uint8_t> data; double at, done; bool on; } g_rx, g_pend; // at: BEGIN, done: last chunk
enum GuestJoin { gaIdle, gaLoad, gaWait, gaArrive, gaTeleport } g_ga;
MsgBlobBegin g_gaHdr;       // the blob being loaded
double g_gaAt, g_rejoinAt;  // g_rejoinAt: host, last REJOIN_REQ taken; guest, last one sent
char g_rejoinHeld[64];      // guest: a re-join wanted inside the 5 s window, sent when it opens (Net_Tick)
bool g_gaLeft;
uint32_t g_gaTransfer, g_gaLoadMs;
MsgLoadout g_loadout;       // host: last sent; guest: last received
bool g_loadoutForce, g_loadoutIn; // host: send at the next poll even unchanged; guest: one to apply
double g_loadoutAt, g_salvAt;     // host: last LOADOUT poll; guest: last SALVAGE sent
int g_salvOut, g_salvIn;          // guest: collected, not sent yet; host: received, not added yet
uint32_t g_loadoutCrc;            // upgrades + arsenal of the last LOADOUT sent / applied (log on change only)
// M6 (destruction_impl.md §11). Guest: the blob whose world is loaded here (0 = none, or a load is replacing it): the
// destruction stream of that epoch applies only then. Host: the last blob's epoch is stale (mode 0) and a mission ran
uint16_t g_loadedId;
bool g_hStale, g_wasMission, g_f3;
uint32_t g_netDrops;              // NetDrops() last seen (a change: logged; the guest re-joins)

uint8_t* LocalPlayer() { return *Rva<uint8_t*>(rva::Player); }
uint8_t* ObjectFromHandle(unsigned h) { return Fn<uint8_t* __thiscall(void*, unsigned)>(site::ObjFromHandle)(Rva(rva::World), h); }

// HUD line now in gameplay, else on the next gameplay frame (the newest one wins); always logged.
__attribute__((format(printf, 1, 2))) void Hud(const char* fmt, ...) {
    char t[128];
    va_list a;
    va_start(a, fmt);
    vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    Log("session: HUD \"%s\"", t);
    if (g_state == 1 && LocalPlayer()) {
        *g_hudPending = 0; // an older deferred line would show after this one
        Fn<int __cdecl(char*, float, void*, void*, int*)>(site::HudMessageAdd)(t, 4.0f, nullptr, nullptr, nullptr);
    } else
        memcpy(g_hudPending, t, sizeof t);
}

void Clean(char* s, size_t n) { // received text: terminated, printable ASCII only
    s[n - 1] = 0;
    for (; *s; s++)
        if (*s < 0x20 || *s > 0x7E) *s = '?';
}

// ---- handshake values -------------------------------------------------------------------------------------------------
uint32_t FileCrc(const wchar_t* name) { // a file beside rfg.exe, 0 when absent
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls%ls", ModDir(), name);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static uint8_t buf[1 << 16];
    uint32_t c = 0;
    DWORD n;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) c = Crc(c, buf, n);
    CloseHandle(f);
    return c;
}

uint32_t VppHash(int& files) { // order-free: the sum of CRC32(name, size) over data\*.vpp_pc
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%lsdata\\*.vpp_pc", ModDir());
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(p, &d);
    uint32_t sum = 0;
    files = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        uint64_t size = (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
        sum += Crc(Crc(0, (const uint8_t*)d.cFileName, wcslen(d.cFileName) * 2), (const uint8_t*)&size, 8), files++;
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return sum;
}

void BuildHello() { // ~a few MB of file reads, once per process (the first F8)
    auto nt = (IMAGE_NT_HEADERS32*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    MsgHello& h = g_hello;
    h = {};
    h.type = kHello, h.proto = kProtocol;
    h.peStamp = nt->FileHeader.TimeDateStamp, h.imageSize = nt->OptionalHeader.SizeOfImage;
    h.crcXinput = FileCrc(L"xinput1_3.dll"), h.crcWinmm = FileCrc(L"winmm.dll"), h.crcDinput8 = FileCrc(L"dinput8.dll");
    h.vpp = VppHash(g_vppFiles);
    memcpy(h.name, g_ini.name, sizeof h.name);
    Log("session: handshake values: protocol %u, rfg.exe %08X/%08X, xinput1_3.dll %08X, winmm.dll %08X, dinput8.dll %08X, "
        "data %08X (%d vpp_pc), name %s",
        (unsigned)h.proto, (unsigned)h.peStamp, (unsigned)h.imageSize, (unsigned)h.crcXinput, (unsigned)h.crcWinmm,
        (unsigned)h.crcDinput8, (unsigned)h.vpp, g_vppFiles, g_ini.name);
}

bool Mismatch(const MsgHello& g, char* why, size_t n) { // host side: the guest's HELLO against ours
    const MsgHello& h = g_hello;
    if (g.peStamp != h.peStamp || g.imageSize != h.imageSize)
        snprintf(why, n, "different rfg.exe (host %08X/%08X, guest %08X/%08X)", (unsigned)h.peStamp, (unsigned)h.imageSize,
                 (unsigned)g.peStamp, (unsigned)g.imageSize);
    else if (g.crcXinput != h.crcXinput)
        snprintf(why, n, "different RfgCoop build (xinput1_3.dll host %08X, guest %08X)", (unsigned)h.crcXinput, (unsigned)g.crcXinput);
    else if (g.crcWinmm != h.crcWinmm)
        snprintf(why, n, "different winmm.dll mod (host %08X, guest %08X, 0 = none)", (unsigned)h.crcWinmm, (unsigned)g.crcWinmm);
    else if (g.crcDinput8 != h.crcDinput8)
        snprintf(why, n, "different dinput8.dll mod (host %08X, guest %08X, 0 = none)", (unsigned)h.crcDinput8, (unsigned)g.crcDinput8);
    else if (g.vpp != h.vpp)
        snprintf(why, n, "different game data (data\\*.vpp_pc list host %08X, guest %08X)", (unsigned)h.vpp, (unsigned)g.vpp);
    else if (!g.pw != !h.pw)
        snprintf(why, n, "a password is set on the %s only (password= in RfgCoop.ini)", h.pw ? "host" : "guest");
    else
        return false;
    return true;
}

// Host: wrong guest proofs (online password guesses) per remote IPv4 address; the 5th refuses that address for 60 s
// (kicked on connect), then it gets 5 more. A WELCOME clears it. Only an AUTH that echoes this connection's Nh counts:
// ENet does not check source addresses (a forged CONNECT completes blind), so a strike must come from an address that
// receives our packets, else anyone could ban the real guest. A malformed or unechoed AUTH, a timeout and leaving after
// CHALLENGE are refused or dropped but not counted: a blind sender can fake them, and they test no password (CHALLENGE
// holds nothing password-derived, so a wrong AUTH is the only way to test one). ponytail: 16 addresses, the stalest
// failure makes room, so more than 16 real attacking addresses can push a ban out early; a firewall rule is the upgrade.
struct Strike { char ip[24]; int fails; double at; } g_strikes[16]; // at: the last failure (a ban runs from the 5th)

Strike* FindStrike(const char* ip, bool make) {
    Strike* old = g_strikes;
    for (Strike& s : g_strikes) {
        if (!strcmp(s.ip, ip)) return &s;
        if (s.at < old->at) old = &s;
    }
    if (!make) return nullptr;
    *old = {};
    snprintf(old->ip, sizeof old->ip, "%s", ip);
    return old;
}

bool Banned(const char* ip) {
    Strike* s = FindStrike(ip, false);
    return s && s->fails >= 5 && NowMs() - s->at < 60000;
}

void AddStrike(const char* what) {
    Strike* s = FindStrike(g_peerIp, true);
    if (s->fails >= 5) s->fails = 0; // its ban ran out (a banned address never gets this far)
    s->fails++, s->at = NowMs();
    Log("session: failed handshake from %s (%s), %d of 5%s", g_peerIp, what, s->fails, s->fails == 5 ? ": refused for 60 s" : "");
}

// Q12: co-op needs a host save that has finished the Tutorial: the All_mission_infos entry with mis_mode (+0x14) 4
// (MISSION_MODE_TUTORIAL), complete per mission_is_complete(index). Gameplay only. A DLC level keeps the base list
// (dlc_missions_init appends after All_mission_infos_mark) and every level init clears +0x58 (mission_info_level_init_pre
// 0x3B0D20, before mission_system_load), so DotB finds the Tutorial with complete 0 and is refused. No mode-4 entry =
// not allowed (defensive only).
bool TutorialDone() {
    uint8_t* a = Rva<uint8_t>(site::AllMissionInfos);
    uint8_t* m = Field<uint8_t*>(a, 0x4);
    int n = Field<int>(a, 0xC);
    for (int i = 0; m && i < n && i < 26; i++, m += 0x60) {
        if (Field<int>(m, 0x14) != 4) continue;
        bool done = Fn<uint32_t __cdecl(int)>(site::MissionIsComplete)(i) & 0xFF;
        const char* name = Field<const char*>(m, 0x00);
        Log("session: Tutorial = mission %d \"%.40s\", flags %08X, complete %d", i, name ? name : "?", Field<unsigned>(m, 0x58), done);
        return done;
    }
    Log("session: no Tutorial mission among %d: co-op not allowed", n);
    return false;
}

// ---- session ------------------------------------------------------------------------------------------------------------
void SendByte(uint8_t type) { MsgByte m{type}; NetSend(kChControl, &m, sizeof m); }

// Our one pause token, balanced against the game's own pauses (§4.7). Every level exit and level init unpause the count
// to 0 (site::PausedCount), so a token held when the level goes is gone with it: never "released" again. Taken only in
// gameplay (state 1, after that level init); released only while the count is above 0.
void PauseToken(bool want) {
    uint8_t* me = LocalPlayer();
    if (!me) {
        if (g_myPause) Log("session: our pause token went with the level");
        g_myPause = false;
        return;
    }
    if (want && !g_myPause && g_state == kGsGameplay) {
        Fn<void __cdecl(bool)>(site::GamePause)(true);
        g_myPause = true;
        Log("session: co-op pause on (Game_paused_count %d)", *Rva<int>(site::PausedCount));
    } else if (!want && g_myPause) {
        if (*Rva<int>(site::PausedCount) > 0) Fn<void __cdecl()>(site::GameUnpause)();
        g_myPause = false;
        Log("session: co-op pause off (Game_paused_count %d)", *Rva<int>(site::PausedCount));
    }
}

void DropBlobs(const char* why) { // guest: a blob arriving or waiting to load is stale
    if (g_rx.on || g_pend.on)
        Log("session: dropped %s (%s)", g_pend.on ? "a received world blob not loaded yet" : "a world blob arriving", why);
    g_rx = {}, g_pend = {};
    if (!g_ini.host) Destr_GuestDrop(); // its destruction stream is stale too (M6 epoch rules)
}

void JoinReset() { // a link begins or ends: nothing of the last one carries over (the partner puppet: Puppet_Net)
    g_hj = hjIdle, g_blobSent = false, *g_waitWhy = 0;
    DropBlobs("link change");
    g_ga = gaIdle, *g_rejoinHeld = 0, g_loadedId = 0, g_hStale = g_wasMission = false;
    g_sentPause = g_hostPaused = g_hostLoading = false;
    g_loadout = {}, g_loadoutIn = false, g_loadoutForce = true, g_loadoutCrc = 0;
    g_salvOut = 0; // guest: Stop flushes it first. Host g_salvIn stays: it is already ours (Q13), added in gameplay (Net_Tick)
    PauseToken(false);
}

// Guest: a mission or modal activity of its own must not run under the fences. Its death there skips the death screen
// (gamestate_gp_simulate_frame: 0x3EDC0C mission_terminate -> the fail screen GS 0x28, whose Continue load GF-LOAD
// refuses at 0x44A8D8 after Mc_info = 1 switched the screen off; 0x3EDC19-0x3EDD2A activity_*_process_player_death),
// and Esc -> Cancel mission is fenced: stuck for good. Convoys (Q14) take the normal death screen. NULL = none.
const char* GuestBusy() {
    if (World_InMission()) return "finish or cancel your mission before joining";
    int act = *Rva<int>(site::ActivityMode);
    return act && act != kConvoy ? "finish or cancel your activity before joining" : nullptr;
}

// pick: a guest's F8 opens the Q17 hammer / backpack picker once the refusals below have passed; its confirm calls
// Start() again (Net_Tick), which checks everything anew
void Start(bool pick = false) {
    if (!g_ini.host && !g_ini.guest) return Hud("Co-op: set role=host or role=guest in RfgCoop.ini");
    // Both roles need a level: the guest arms its fences at the link for good, and an armed guest loads nothing of its
    // own (guest.cpp) while the blob needs a Player (GuestJoin), so a link from the main menu could never join. Outside
    // gameplay a HUD line would only show after the load, when it no longer applies: the title says it (Status)
    if (g_state != 1 || !LocalPlayer())
        return Log("session: F8 outside gameplay: %s", g_ini.host ? "the host needs a loaded save"
                                                                  : "the guest needs a loaded save, in free roam (the host's world replaces it)");
    if (g_ini.host) { // Q12; a host may start in a mission (the join waits: JOINWAIT)
        if (!TutorialDone()) return Hud("Co-op needs a save that has finished the Tutorial");
        g_checked = LocalPlayer();
    } else if (const char* busy = GuestBusy()) // early word, no dialling; Link checks again (a mission started meanwhile)
        return Hud("Co-op: %s", busy);
    if (!g_ini.host && !*g_ini.addr) return Hud("Co-op: set connect=<ip:port> in RfgCoop.ini");
    if (g_ini.host && !Random(g_salt, 16)) return Hud("Co-op: no BCrypt random numbers"); // above w: no return holds it
    wchar_t w[128]; // read at every start; only the net thread keeps it (the host until its key is made)
    IniStr(L"password", w, 128);
    if (size_t k = wcslen(w); k && k < 10) { // AUTH / WELCOME proofs can be guessed offline (session_impl.md)
        SecureZeroMemory(w, sizeof w);
        return Hud("Co-op password too short: use at least 10 characters (or none for an open session)");
    }
    if (pick) { // last: the picker never opens when its confirm's Start() would refuse (that Start reads the ini again)
        SecureZeroMemory(w, sizeof w);
        return Picker_Open();
    }
    if (!g_hello.type) BuildHello();
    NetConfig c{};
    c.host = g_ini.host, c.port = g_ini.host ? g_ini.port : g_ini.dialPort;
    c.lagMs = g_ini.lag, c.jitterMs = g_ini.jitter, c.lossPct = g_ini.loss;
    memcpy(c.addr, g_ini.addr, sizeof c.addr);
    memcpy(c.salt, g_salt, 16); // host: fresh at every start, sent in every CHALLENGE (the guest's key needs it)
    WideCharToMultiByte(CP_UTF8, 0, w, -1, c.password, sizeof c.password, nullptr, nullptr);
    SecureZeroMemory(w, sizeof w);
    g_hello.pw = *c.password != 0;
    bool ok = NetStart(c);
    SecureZeroMemory(c.password, sizeof c.password);
    if (!ok) return Hud("Co-op: the network thread did not start (RfgCoop.log)");
    g_phase = Waiting, g_phaseAt = NowMs();
    if (!g_ini.host) Guest_ResetLoadout(); // Q18: a new session starts from the host's weapons; a redial (Unlink) keeps ours
    QuickPauseOff(true); // §4.7: alt-tab never pauses while a session runs (it would pause the partner or stall a join)
    Log("session: password %s", g_hello.pw ? "set: the guest proves it in AUTH, then the host in WELCOME" : "none: open session");
    if (g_ini.host) Hud("Co-op: hosting on UDP port %u%s", (unsigned)c.port, g_hello.pw ? "" : ". No password: anyone who can reach your port can join");
    else Hud("Co-op: connecting to %s:%u%s", c.addr, (unsigned)c.port, g_hello.pw ? "" : " (no password)");
}

// F8, REJECT, a network failure, the Tutorial rule. why goes out before the disconnect: as BYE when linked, as REJECT
// to a guest in the host's handshake (it shows it and stops instead of redialling a host that is off)
void Stop(const char* why) {
    if (g_phase == Off) return;
    if (g_phase == Linked && !g_ini.host && g_salvOut) { // Q13: pickups not sent yet go before BYE (channel 0: in order)
        Log("session: %d salvage to the host before leaving", g_salvOut);
        while (g_salvOut) { // at most 34 (g_salvOut <= 1000000); the host takes SALVAGE with no rate limit
            MsgSalvage m{kSalvage, g_salvOut > 30000 ? 30000 : g_salvOut};
            g_salvOut -= m.amount;
            NetSend(kChControl, &m, sizeof m);
        }
    }
    if (g_phase == Linked || (g_ini.host && g_phase == Shaking)) {
        MsgReject b{g_phase == Linked ? kBye : kReject, {}};
        snprintf(b.reason, sizeof b.reason, "%s", why);
        NetSend(kChControl, &b, sizeof b);
    }
    if (g_phase == Linked) Puppet_Net(false), Destr_Link(false, g_ini.host);
    NetStop();
    g_phase = Off;
    QuickPauseOff(false); // alt-tab pauses again (not with dev=1)
    JoinReset();
    // a guest's fences and save guard stay until the process exits (it may hold the host's world)
    if (!g_ini.host && Guest_Armed()) Hud("Co-op: %s. Restart the game to continue your own campaign", why);
    else Hud("Co-op: %s", why);
}

void Link(const char* partner) {
    snprintf(g_partner, sizeof g_partner, "%s", partner);
    g_phase = Linked, g_phaseAt = NowMs(), g_sentReady = -1, g_byeSeen = false;
    g_seqOut = g_seqIn = 0, g_out = g_in = g_lost = 0;
    JoinReset(); // the host's join (§4.8 step 2) starts on the guest's first READY (OnMessage), not here
    if (!g_ini.host) { // §4.8 step 1: this game's world becomes the host's. Save guard and fences now, before any blob, for good
        if (const char* busy = GuestBusy()) return Stop(busy); // F8, autoconnect, or one started while dialling
        if (!World_ArmSaveGuard("co-op guest")) return Stop("the save guard could not be armed (RfgCoop.log): no join");
        Guest_Arm(); // no join without the fences: else READY would make the host capture, pause and send for nothing
        if (!Guest_Armed()) return Stop("the guest fences could not be armed (RfgCoop.log): no join");
    }
    bool puppet = Puppet_Net(true);
    Destr_Link(true, g_ini.host); // M6: the host records, the guest waits for its first epoch (BLOB_BEGIN)
    g_netDrops = NetDrops();
    Log("session: linked with %s", g_partner);
    Hud("Co-op: %s connected%s", g_partner, puppet ? "" : " (no puppet: RfgCoop.log)");
}

void Unlink() { // the peer is gone: the host listens again, the guest redials (net.cpp)
    if (g_phase == Linked) {
        Puppet_Net(false);
        Destr_Link(false, g_ini.host);
        if (!g_ini.host && Guest_Armed()) // §4.8 leave: the fences stay; why is in the log (BYE / disconnect line). The
            // session goes on (Q18 keeps the weapons) until F8: the redial's join is a re-join
            Hud("Co-op: %s gone, reconnecting (F8 stops; your own campaign needs a game restart)", g_partner);
        else if (g_byeSeen) Hud("Co-op: %s left: %s", g_partner, g_byeWhy);
        else Hud("Co-op: %s disconnected", g_partner);
        JoinReset();
    }
    if (g_phase != Off) g_phase = Waiting, g_phaseAt = NowMs();
}

__attribute__((format(printf, 1, 2))) void Refuse(const char* fmt, ...) { // host: REJECT with the reason, then drop the peer
    MsgReject r{kReject, {}};
    va_list a;
    va_start(a, fmt);
    vsnprintf(r.reason, sizeof r.reason, fmt, a);
    va_end(a);
    NetSend(kChControl, &r, sizeof r);
    NetKick();
    g_phase = Waiting, g_phaseAt = NowMs();
    Log("session: refused the guest: %s", r.reason);
    Hud("Co-op: guest refused: %s", r.reason);
}

void AuthFail(const char* what) { // host: the guest's proof is wrong (a password guess): both sides show it
    AddStrike(what);
    Refuse(Banned(g_peerIp) ? "wrong password (5 tries: wait 60 s)" : "wrong password"); // the 5th strike starts the ban
}

const char* Failed() { return g_hello.pw ? "wrong password" : "handshake failed"; } // any other bad CHALLENGE / AUTH step

void Welcome() { // host: the guest is in; with a password our proof goes only now, to a guest that has proved it
    MsgWelcome w{kWelcome, {}, {}};
    memcpy(w.name, g_ini.name, sizeof w.name);
    if (g_hello.pw && !Proof("host", g_peerHello.nonce, g_nonce, g_peerHello, w.proof)) return Refuse("no BCrypt on the host");
    NetSend(kChControl, &w, sizeof w);
    if (Strike* s = FindStrike(g_peerIp, false)) *s = {};
    if (!g_hello.pw) Log("session: open session (no password): anyone who can reach this port can join");
    Link(g_peerName);
}

bool Sane(const float* v) { // received position: finite and on the scale of the map
    for (int i = 0; i < 3; i++)
        if (!(fabsf(v[i]) <= 1e5f)) return false;
    return true;
}

// Guest, channel 2: BLOB_BEGIN / BLOB_CHUNK (the world) and LOADOUT. NULL = taken, else why it is dropped. The blob's
// own bytes are checked by World_BlobOk before the game sees them; here only sizes, order and the header's values.
const char* OnBlob(uint8_t t, const uint8_t* d, size_t n) {
    if (g_ini.host || g_phase != Linked) return "channel 2 is host -> guest, linked";
    if (t == kBlobBegin) {
        if (n != sizeof(MsgBlobBegin)) return "BLOB_BEGIN size";
        MsgBlobBegin b;
        memcpy(&b, d, n);
        // dlc: an honest host sends its World+0x2694 (base campaign; Q12 refuses DLC levels); a sanity bound only
        // destr: mode 3 does not exist; logPkts: no world log before ver7.2
        if (b.size < 8 || b.size > kBlobMax || b.kind > kBlobAsked || !Sane(b.pos) || b.meta.dlc < -1 || b.meta.dlc > 16 ||
            (b.destr & 3) == 3 || b.logPkts)
            return "BLOB_BEGIN values";
        if (g_rx.on) Log("session: blob %u abandoned at %u of %u bytes", g_rx.b.id, (unsigned)g_rx.data.size(), (unsigned)g_rx.b.size);
        g_rx = {}, *g_rejoinHeld = 0; // a world is coming: a re-join held back is moot
        g_rx.b = b, g_rx.data.reserve(b.size), g_rx.at = NowMs(), g_rx.on = true;
        Destr_GuestBegin(b.id, b.destr); // M6: the epoch starts here; the old world's stream is dropped
        Log("session: blob %u (%s) coming: %u bytes, dlc %d, host at %.1f %.1f %.1f, host clock %02u:%02u:%02u", b.id,
            b.kind == kBlobJoin ? "join" : b.kind == kBlobHostLoad ? "host load" : "asked", (unsigned)b.size, (int)b.meta.dlc,
            b.pos[0], b.pos[1], b.pos[2], b.clock[0], b.clock[1], b.clock[2]);
        Hud("Co-op: receiving %s's world (%.1f MB)", g_partner, b.size / 1048576.0);
        return nullptr;
    }
    if (t == kBlobChunk) {
        if (n < sizeof(MsgBlobChunk)) return "BLOB_CHUNK size";
        MsgBlobChunk c;
        memcpy(&c, d, sizeof c);
        if (!g_rx.on || c.id != g_rx.b.id) return nullptr; // a dropped blob's chunks (host LOADING, a newer BEGIN)
        size_t have = g_rx.data.size(), want = g_rx.b.size - have < kBlobChunkBytes ? g_rx.b.size - have : kBlobChunkBytes;
        if (c.index != have / kBlobChunkBytes || n != sizeof c + want) { // reliable ordered: only a cap (net.cpp) or a bad host
            Log("session: blob %u broken: chunk %u of %u bytes where chunk %u of %u bytes was due", c.id, c.index,
                (unsigned)(n - sizeof c), (unsigned)(have / kBlobChunkBytes), (unsigned)want);
            g_rx = {};
            Session_RejoinRequest("the world transfer broke");
            return nullptr;
        }
        g_rx.data.insert(g_rx.data.end(), d + sizeof c, d + n);
        if (g_rx.data.size() < g_rx.b.size) return nullptr;
        g_rx.done = NowMs(), g_rx.on = false;
        g_pend = std::move(g_rx), g_pend.on = true; // replaces an older one not loaded yet
        g_rx = {};
        Log("session: blob %u complete: %u bytes in %.0f ms", g_pend.b.id, (unsigned)g_pend.b.size, g_pend.done - g_pend.at);
        return nullptr;
    }
    if (t == kLoadout) {
        if (n != sizeof(MsgLoadout)) return "LOADOUT size";
        MsgLoadout m;
        memcpy(&m, d, n);
        if (m.arsenalBytes > sizeof m.arsenal || m.arsenalBytes % 8 || m.salvage < 0 || m.salvage > 1000000) return "LOADOUT values";
        g_loadout = m, g_loadoutIn = true; // applied in gameplay (Net_Tick)
        return nullptr;
    }
    if (t == kDestr || t == kCensus) return Destr_OnMessage(t, d, n); // M6: behind the blob, same channel (ordered)
    return "type off channel 2";
}

void OnMessage(uint8_t ch, const std::vector<uint8_t>& d) {
    size_t n = d.size();
    uint8_t t = n ? d[0] : 0;
    auto drop = [&](const char* why) {
        if (++g_nDrop <= 5) Log("session: dropped type %u (%u bytes, channel %u): %s (#%d)", t, (unsigned)n, ch, why, g_nDrop);
    };
    if (ch == kChBlob) {
        if (const char* why = OnBlob(t, d.data(), n)) drop(why);
        return;
    }
    if (t == kPState) {
        if (ch != kChState || n != sizeof(MsgPState)) return drop("size or channel");
        if (g_phase != Linked) return;
        MsgPState m;
        memcpy(&m, d.data(), n);
        if (g_in && (int16_t)(m.seq - g_seqIn) <= 0) return; // late or duplicate: as ENet's sequenced channel 1 (the jitter shim reorders)
        uint16_t gap = m.seq - g_seqIn; // a gap = states lost on the way (or dropped late just above)
        if (g_in && gap > 1 && gap < 0x8000) g_lost += gap - 1;
        g_seqIn = m.seq, g_in++;
        return Puppet_In(m);
    }
    if (ch != kChControl) return drop("control type off channel 0");
    switch (t) {
    case kHello: {
        if (!g_ini.host || g_phase != Shaking || g_challenged) return drop("unexpected HELLO");
        uint16_t proto = n >= 3 ? (uint16_t)(d[1] | d[2] << 8) : 0; // before the size: another build gets a reason
        if (proto != kProtocol) return Refuse("RfgCoop protocol %u on the host, %u on the guest", (unsigned)kProtocol, (unsigned)proto);
        if (n != sizeof(MsgHello)) return drop("HELLO size");
        MsgHello& h = g_peerHello; // kept as received: the proofs cover these bytes
        memcpy(&h, d.data(), n);
        memcpy(g_peerName, h.name, sizeof g_peerName);
        Clean(g_peerName, sizeof g_peerName);
        Log("session: HELLO from %s: rfg.exe %08X/%08X, xinput1_3.dll %08X, winmm.dll %08X, dinput8.dll %08X, data %08X, "
            "password %d", g_peerName, (unsigned)h.peStamp, (unsigned)h.imageSize, (unsigned)h.crcXinput, (unsigned)h.crcWinmm,
            (unsigned)h.crcDinput8, (unsigned)h.vpp, h.pw != 0);
        char why[100];
        if (Mismatch(h, why, sizeof why)) return Refuse("%s", why);
        // every session: AUTH must echo Nh, so a forged source address never links (the open session would else stream
        // PSTATE to it). Nothing password-derived: Nh and the salt; our proof waits for a good AUTH (WELCOME)
        MsgChallenge c{kChallenge, {}, {}};
        if (!Random(g_nonce, 16)) return Refuse("no BCrypt on the host");
        memcpy(c.nonce, g_nonce, 16), memcpy(c.salt, g_salt, 16);
        NetSend(kChControl, &c, sizeof c);
        g_challenged = true, g_phaseAt = NowMs(); // the handshake timeout restarts per step
        return Log("session: CHALLENGE sent to %s", g_peerName);
    }
    case kChallenge: { // guest: with a password we prove first; the net thread made the key from c.salt (net.cpp)
        if (g_ini.host || g_phase != Shaking) return drop("unexpected CHALLENGE");
        MsgChallenge c;
        MsgAuth a{kAuth, {}, {}}; // proof stays zero without a password
        if (g_challenged || n != sizeof c) return Log("session: bad CHALLENGE"), Stop(Failed());
        memcpy(&c, d.data(), n);
        memcpy(g_nonce, c.nonce, 16); // Nh: the host's proof in WELCOME covers it
        if (g_hello.pw && !Proof("guest", c.nonce, g_hello.nonce, g_hello, a.proof)) return Stop("no BCrypt");
        memcpy(a.nonce, c.nonce, 16); // Nh back: shows the host this AUTH comes from where its CHALLENGE went
        NetSend(kChControl, &a, sizeof a);
        g_challenged = true, g_phaseAt = NowMs(); // the handshake timeout restarts per step
        return Log("session: CHALLENGE, AUTH sent%s", g_hello.pw ? " with our proof" : " (open session)");
    }
    case kAuth: { // host. A strike only for a wrong proof with our Nh echoed (see g_strikes); the rest is refused
        if (!g_ini.host || g_phase != Shaking) return drop("unexpected AUTH");
        MsgAuth a;
        uint8_t want[32];
        if (!g_challenged || n != sizeof a) return Log("session: AUTH out of turn or of the wrong size (not counted)"), Refuse("%s", Failed());
        memcpy(&a, d.data(), n);
        if (!Same(a.nonce, g_nonce, 16)) return Log("session: AUTH without our nonce (not counted)"), Refuse("%s", Failed());
        if (g_hello.pw && (!Proof("guest", g_nonce, g_peerHello.nonce, g_peerHello, want) || !Same(want, a.proof, 32)))
            return AuthFail("wrong guest proof");
        Log("session: %s", g_hello.pw ? "the guest's proof checks out" : "the guest echoed our nonce (open session)");
        return Welcome();
    }
    case kWelcome: { // guest: with a password, the host's proof before we link
        if (g_ini.host || g_phase != Shaking || n != sizeof(MsgWelcome)) return drop("unexpected WELCOME");
        if (!g_challenged) return Log("session: WELCOME before our AUTH"), Stop(Failed());
        MsgWelcome w;
        uint8_t want[32];
        memcpy(&w, d.data(), n);
        if (g_hello.pw && (!Proof("host", g_hello.nonce, g_nonce, g_hello, want) || !Same(want, w.proof, 32))) {
            MsgReject b{kBye, "the host does not know the password"}; // it linked on our AUTH: BYE tells it why
            NetSend(kChControl, &b, sizeof b);
            return Log("session: the host's password proof is wrong"), Stop(b.reason);
        }
        Clean(w.name, sizeof w.name);
        return Link(w.name);
    }
    case kReject: {
        if (g_ini.host || g_phase != Shaking || n != sizeof(MsgReject)) return drop("unexpected REJECT");
        MsgReject r;
        memcpy(&r, d.data(), n);
        Clean(r.reason, sizeof r.reason);
        Log("session: the host refused us: %s", r.reason);
        char t2[140];
        snprintf(t2, sizeof t2, "refused by the host: %s", r.reason);
        return Stop(t2); // no redial: the same values would be refused again
    }
    case kBye: {
        if (n != sizeof(MsgReject) || g_phase != Linked) return drop("unexpected BYE");
        MsgReject b;
        memcpy(&b, d.data(), n);
        Clean(b.reason, sizeof b.reason);
        memcpy(g_byeWhy, b.reason, sizeof g_byeWhy);
        g_byeSeen = true;
        return Log("session: %s said BYE: %s", g_partner, g_byeWhy);
    }
    case kLoading:
    case kReady:
        if (n != 1 || g_phase != Linked) return drop("unexpected LOADING/READY");
        Log("session: %s is %s", g_partner, t == kLoading ? "LOADING" : "READY");
        Puppet_PartnerLoading(t == kLoading);
        if (g_ini.host) { // the guest's loads; during its join the host already shows "Partner joining..."
            // §4.8 step 2: the join starts on the guest's first READY, which it sends on channel 0 only once it took
            // WELCOME. ENet orders per channel only: a BEGIN sent at the link (channel 2) could beat WELCOME and be
            // dropped by a guest still in the handshake, leaving us paused for 120 s
            if (t == kReady && g_hj == hjIdle && !g_blobSent) g_hj = hjWant, g_hjKind = kBlobJoin, g_hjAt = 0;
            if (t == kLoading && g_hj != hjSent) Hud("Co-op: %s is loading", g_partner);
            return;
        }
        // §4.8 re-join: the host's world changes; whatever it sent before is stale, a new blob follows its load
        if ((g_hostLoading = t == kLoading)) DropBlobs("the host is loading"), g_hostPaused = false, Hud("Host loading...");
        return;
    case kJoinWait: { // guest: the host is in a mission (§4.8 step 2)
        if (g_ini.host || n != sizeof(MsgReject) || g_phase != Linked) return drop("unexpected JOINWAIT");
        MsgReject r;
        memcpy(&r, d.data(), n);
        Clean(r.reason, sizeof r.reason);
        Log("session: the join waits: %s", r.reason);
        if (!strncmp(r.reason, "F3: ", 4)) return Hud("Co-op: F3 resync refused: %s", r.reason + 4); // the host refused our F3
        return Hud("Co-op: you join after the host's mission (%s)", r.reason);
    }
    case kPaused: { // guest: §4.7, applied with our own token in Net_Tick
        if (g_ini.host || n != sizeof(MsgPaused) || g_phase != Linked) return drop("unexpected PAUSED");
        bool on = d[1] != 0;
        if (on == g_hostPaused) return;
        g_hostPaused = on;
        Log("session: the host %s", on ? "paused" : "resumed");
        if (on) Hud("Host paused");
        return;
    }
    case kJoined: { // host: the guest landed beside us
        if (!g_ini.host || n != sizeof(MsgJoined) || g_phase != Linked) return drop("unexpected JOINED");
        MsgJoined j;
        memcpy(&j, d.data(), n);
        if (j.id != g_joinId) return Log("session: JOINED for blob %u ignored (blob %u is the current one)", j.id, g_joinId);
        Log("session: %s joined with blob %u, %.1f s after it was sent: guest transfer %u ms, load %u ms, %.1f m from us "
            "after its teleport", g_partner, j.id, g_hj == hjSent ? (NowMs() - g_hjSentAt) / 1000 : -1.0, j.transferMs,
            j.loadMs, j.dist);
        if (g_hj == hjSent) g_hj = hjIdle; // the pause token goes in Net_Tick
        g_loadoutForce = true;             // §4.8 step 5 / Q13: the loadout once after every join and re-join
        Destr_HostJoined(j.id);            // M6: the resync check (one census per tracked building)
        return Hud("Co-op: %s joined", g_partner);
    }
    case kRejoinReq: { // host: the guest needs the world again (its death, a refused load, a broken transfer)
        if (!g_ini.host || n != sizeof(MsgReject) || g_phase != Linked) return drop("unexpected REJOIN_REQ");
        MsgReject r;
        memcpy(&r, d.data(), n);
        Clean(r.reason, sizeof r.reason);
        double now = NowMs();
        // ponytail: a flood bound only (one per 2.5 s, so a broken guest cannot keep us capturing; each still pauses us
        // while it loads). The real debounce is the guest's 5 s (Session_RejoinRequest); ours is shorter because each
        // side measures from its own moment, so network or lag-shim jitter would refuse an honest held request, and a
        // refusal sends nothing back
        // a waiting hjWant (no capture of ours in flight: a mission refused it) is not busy: merged below (a resend in a
        // mission, else hjWant again), so a dead guest is not left on its death screen until the host's mission ends
        bool busy = g_hj == hjCapture || g_hj == hjResend || (g_hj == hjWant && g_capMine);
        if (busy || now - g_rejoinAt < 2500)
            return Log("session: %s asks to re-join (%s): refused, a blob is on its way or one came < 2.5 s ago", g_partner, r.reason);
        g_rejoinAt = now;
        // No world from us yet (it linked outside gameplay, so no READY): this is its join, free roam only, from a fresh
        // blob (§4.8 step 2; HostJoin sends JOINWAIT in a mission). After that: in a mission the loaded buffer as is
        // (never a fresh blob mid-mission), else a fresh silent blob
        bool resend = g_blobSent && World_InMission();
        Log("session: %s asks to re-join (%s): %s", g_partner, r.reason,
            !g_blobSent ? "no world from us yet: its join (free roam only)" : resend ? "the loaded buffer as is" : "a fresh silent blob");
        g_hjKind = g_blobSent ? kBlobAsked : kBlobJoin, g_hj = resend ? hjResend : hjWant, g_hjAt = 0;
        return;
    }
    case kSalvage: { // host: rule Q13, the guest's salvage is ours. 1..30000 each (add_salvage's cap). No rate limit:
        // Drain counts them when we tick, not when they arrived, so a host stall bunches them, and a dropped one is lost
        // salvage; the wallet cap bounds a hostile guest anyway
        if (!g_ini.host || n != sizeof(MsgSalvage) || g_phase != Linked) return drop("unexpected SALVAGE");
        MsgSalvage m;
        memcpy(&m, d.data(), n);
        if (m.amount < 1 || m.amount > 30000) return drop("SALVAGE amount");
        g_salvIn = g_salvIn + m.amount > 30000 ? 30000 : g_salvIn + m.amount; // added in gameplay (Net_Tick)
        return;
    }
    case kResync: { // host: the guest's F3 (M6). Free roam: a fresh silent blob, as REJOIN_REQ. In a mission refused and
        // nothing changes (ver7.1; 7.2 makes it a re-join with the world log). JOINWAIT carries the reason back.
        if (!g_ini.host || n != sizeof(MsgByte) || g_phase != Linked) return drop("unexpected RESYNC");
        double now = NowMs();
        const char* why = !g_blobSent || g_hj != hjIdle ? "a world is already on its way"
                        : now - g_rejoinAt < 2500         ? "one came less than 2.5 s ago"
                        : World_InMission()               ? "resync after the host's mission" : nullptr;
        if (why) {
            Log("session: %s asks to re-sync (F3): refused, %s", g_partner, why);
            MsgReject m{kJoinWait, {}};
            snprintf(m.reason, sizeof m.reason, "F3: %s", why); // "F3: " = this refusal (any other JOINWAIT keeps ver6.4's text)
            return NetSend(kChControl, &m, sizeof m);
        }
        g_rejoinAt = now;
        Log("session: %s asks to re-sync (F3): a fresh silent blob", g_partner);
        g_hjKind = kBlobAsked, g_hj = hjWant, g_hjAt = 0;
        return;
    }
    case kCensusRes:
        if (!g_ini.host || g_phase != Linked) return drop("unexpected CENSUS_RES");
        if (const char* why = Destr_OnMessage(t, d.data(), n)) drop(why);
        return;
    default:
        return drop("unknown type");
    }
}

void Drain() {
    NetEvent e;
    while (NetPoll(e)) {
        char text[160];
        size_t k = e.data.size() < sizeof text - 1 ? e.data.size() : sizeof text - 1;
        if (e.kind != kNetData) memcpy(text, e.data.data(), k), text[k] = 0, Clean(text, sizeof text);
        if (g_phase == Off) continue; // left over from a stopped session
        switch (e.kind) {
        case kNetFailed:
            Log("session: network: %s", text);
            Stop(text);
            break;
        case kNetConnected:
            Log("session: connected to %s, handshake", text);
            g_challenged = false;
            if (g_ini.host) {
                snprintf(g_peerIp, sizeof g_peerIp, "%s", text);
                if (char* colon = strrchr(g_peerIp, ':')) *colon = 0;
                if (Banned(g_peerIp)) { // REJECT says why (the guest stops instead of redialling), stays Waiting, kicks
                    int left = (int)ceil((60000 - (NowMs() - FindStrike(g_peerIp, false)->at)) / 1000);
                    Refuse("too many wrong passwords: try again in %d s", left);
                    break;
                }
            } else if (!Random(g_hello.nonce, 16)) {
                Stop("no BCrypt random numbers");
                break;
            }
            g_phase = Shaking, g_phaseAt = NowMs();
            if (!g_ini.host) NetSend(kChControl, &g_hello, sizeof g_hello);
            break;
        case kNetDisconnected:
            Log("session: peer disconnected (%s)", g_phase == Linked ? (g_byeSeen ? "after BYE" : "timeout or drop") : "before the link");
            if (g_ini.host && g_phase == Shaking && g_challenged) // not a strike: a forged DISCONNECT looks the same
                Hud("Co-op: the guest left during the %s", g_hello.pw ? "password check" : "handshake");
            Unlink();
            break;
        case kNetData:
            OnMessage(e.channel, e.data);
            break;
        case kNetNote: // net.cpp: the guest's dial went unanswered 3 times
            Hud("Co-op: %s", text);
            break;
        }
    }
}

// LOADING when your level goes (Player NULL), READY on the first gameplay frame with a Player. A menu over a loaded
// level (pause, map) changes nothing: PSTATE then carries a still state (puppet.cpp Puppet_Out).
// Host (§4.8 re-join, one rule): LOADING drops a blob being made, due or awaited (its pause token went with the level);
// the first gameplay frame after the load re-sends the buffer that load used, as is (death-continue, mission retry or
// checkpoint, fast travel, warping video, activity replay, Esc -> Load).
void LevelState(int state, uint8_t* me) {
    int ready = !me ? 0 : state == 1 ? 1 : g_sentReady;
    if (ready < 0 || ready == g_sentReady) return;
    bool reload = ready == 1 && g_sentReady == 0; // the first gameplay frame after a level change (not the link's READY)
    SendByte(ready ? kReady : kLoading);
    g_sentReady = ready;
    Log("session: sent %s", ready ? "READY" : "LOADING");
    if (!ready) g_loadedId = 0; // guest: any load here (the warp video's too) ends the epoch's world: no stream until the next blob lands
    if (!g_ini.host) return;
    if (!ready && g_hj != hjIdle && g_hj != hjWant) {
        Log("session: our load drops the blob step in progress (%d)", (int)g_hj);
        g_hj = g_blobSent ? hjIdle : hjWant, g_hjAt = 0;
    } else if (reload && g_blobSent) {
        Log("session: host load done: re-sending the world it loaded to %s", g_partner);
        g_hjKind = kBlobHostLoad, g_hj = hjResend;
    }
}

bool HostPaused(int state);
void SendState() { // PSTATE at 20 Hz (the first fresh capture after 50 ms); paused: still + kFrozen (the partner's
    double now = NowMs();  // puppet ends your pack, ver6.3 test: the Esc menu kept Capture running and a Tremor going).
    MsgPState m;           // The host's join token counts (its sim is held while the guest lands); the guest's mirror not.
    // A pack activation's capture goes out at once (Puppet_Step: a low Thrust can start and end within 50 ms)
    if ((now - g_lastSend < 50 && !Puppet_Step()) || !Puppet_Out(m, HostPaused(GameState()) || (g_ini.host && g_myPause)))
        return;
    g_lastSend = now;
    m.seq = ++g_seqOut;
    NetSend(kChState, &m, sizeof m);
    g_out++;
}

void Status() { // window title once a second at most, RTT log every 5 s
    double now = NowMs();
    if (now - g_lastTitle < 1000) return;
    g_lastTitle = now;
    char s[128];
    const char* role = g_ini.host ? "host" : "guest";
    // Off: F8 needs gameplay on a loaded save (Start), so outside it the title says so. An armed guest without a level
    // (the main menu; not a load) loads nothing of its own (guest.cpp): only a restart gets it back into a level
    if (g_phase == Off)
        snprintf(s, sizeof s, "%s", !g_ini.host && !g_ini.guest ? ""
                                    : !g_ini.host && Guest_Armed() && !LocalPlayer() && g_state != kGsLoad
                                        ? "co-op off: restart the game (a co-op guest cannot load)"
                                    : g_state != 1 || !LocalPlayer() ? "co-op off (F8 in gameplay, on your save)"
                                                                     : "co-op off (F8)");
    else if (g_phase == Waiting && g_ini.host) snprintf(s, sizeof s, "host, listening on %u", (unsigned)g_ini.port);
    else if (g_phase == Waiting) snprintf(s, sizeof s, "guest, connecting to %s:%u", g_ini.addr, (unsigned)g_ini.dialPort);
    else if (g_phase == Shaking) snprintf(s, sizeof s, "%s, handshake", role);
    else {
        char j[40] = ""; // the M5 join step
        if (g_ini.host && g_hj != hjIdle) snprintf(j, sizeof j, ", %s", g_hj == hjWant && *g_waitWhy ? "join waits" : "partner joining");
        else if (!g_ini.host && g_rx.on) snprintf(j, sizeof j, ", receiving world %u%%", (unsigned)(g_rx.data.size() * 100 / g_rx.b.size));
        else if (!g_ini.host && (g_pend.on || g_ga != gaIdle)) snprintf(j, sizeof j, ", joining");
        else if (!g_ini.host && g_hostPaused) snprintf(j, sizeof j, ", host paused");
        snprintf(s, sizeof s, "%s, partner %s, RTT %u ms%s", role, g_partner, NetRtt(), j);
    }
    if (strcmp(s, g_title)) memcpy(g_title, s, sizeof s), TitleStatus(s);
    if (g_phase == Linked && now - g_lastStat >= 5000) {
        g_lastStat = now;
        Log("session: RTT %u ms, PSTATE out %u in %u lost %u, partner %s, net drops %u", NetRtt(), g_out, g_in, g_lost,
            g_partner, NetDrops());
    }
}

// ---- F7: teleport beside the partner (Vehicle, Place, Ground, the hold in the air) -----------------------------------
struct { bool active, back; vector dst, origin, held; float t; } g_tp;

uint8_t* Vehicle(uint8_t* p, bool& ok) { // ok = false on a handle that is not a live vehicle (a mounted turret)
    unsigned h = Field<unsigned>(p, 0xAC4);
    uint8_t* v = h == ~0u ? nullptr : ObjectFromHandle(h);
    ok = h == ~0u || (v && Field<int8_t>(v, 0x7E) == 3); // OT_VEHICLE
    return ok ? v : nullptr;
}

vector Here(uint8_t* p) {
    bool ok;
    uint8_t* v = Vehicle(p, ok);
    return Field<vector>(v ? v : p, 0x04);
}

bool Place(vector pos, bool snap) { // snap: human_teleport picks a free spot within 6 m (on foot)
    uint8_t* p = LocalPlayer();
    if (!p) return false;
    bool ok;
    uint8_t* v = Vehicle(p, ok);
    if (!ok || Fn<uint32_t __cdecl(void*)>(site::HumanIsDead)(p) & 0xFF) return false;
    auto unsafe = Fn<void __cdecl(void*, vector, matrix)>(site::HumanTeleportUnsafe);
    if (v) { // the vehicle moves, occupants follow; it keeps no momentum
        unsafe(p, pos, Field<matrix>(v, 0x10));
        vector zero{};
        Fn<void __cdecl(unsigned, vector*)>(site::SetLinearVel)(Field<unsigned>(v, 0x4C), &zero);
        Fn<void __cdecl(unsigned, vector*)>(site::SetAngularVel)(Field<unsigned>(v, 0x4C), &zero);
        return true;
    }
    if (Field<int>(p, 0x33C)) Fn<uint32_t __cdecl(void*, bool)>(site::RagdollStop)(p, false); // the ragdoll keeps the body
    matrix o = Field<matrix>(p, 0x10);
    if (snap) Fn<void __cdecl(void*, vector*, matrix*, float, bool)>(site::HumanTeleport)(p, &pos, &o, 6.0f, false);
    else unsafe(p, pos, o);
    return true;
}

bool Ground(float x, float z, float& y) { // terrain only; false while its collision is not streamed in
    return Fn<uint32_t __cdecl(float*, float, float, bool, vector*)>(site::TerrainHeight)(&y, x, z, false, nullptr) & 0xFF;
}

void TeleportTo(uint8_t* me, vector dst, const char* why) { // F7 and the join: beside dst, with the hold below
    // received data: the zone grid (world::is_zone_streamed 0x52E020, 16 cells of 511 m) ends near +-4100 (-4599 in x)
    if (!(fabsf(dst.x) <= 4600 && fabsf(dst.z) <= 4600 && fabsf(dst.y) <= 4600)) return Hud("%s: %s is off the map", why, g_partner);
    bool ok;
    Vehicle(me, ok);
    if (!ok) return Hud("%s: can't teleport right now", why);
    dst.x += 2.f; // beside the partner, not inside
    vector here = Here(me);
    g_tp = {true, false, dst, g_tp.active ? g_tp.origin : here, here, 0.f};
    Log("session: %s: teleport to %s at %.1f,%.1f,%.1f", why, g_partner, dst.x, dst.y, dst.z);
    Hud("Teleporting to %s", g_partner);
}

// F3 (guest, M6): ask the host for a fresh world. Refused here before the first join and while a world is on its way;
// the host refuses it in its mission (JOINWAIT with the reason; the epoch stays as it is).
void Resync() {
    if (g_phase != Linked) return Hud("F3: no co-op host");
    if (!g_loadedId || g_ga != gaIdle || g_rx.on || g_pend.on || g_hostLoading) return Hud("Co-op: F3 resync: not while joining or loading");
    double now = NowMs();
    if (now - g_rejoinAt < 5000) return Hud("Co-op: F3 resync: one went out less than 5 s ago");
    g_rejoinAt = now;
    SendByte(kResync);
    Log("session: F3: resync asked");
    Hud("Co-op: re-sync asked");
}

void Teleport(uint8_t* me) {
    vector dst;
    if (g_phase != Linked) return Hud("F7: no co-op partner");
    if (!Puppet_PartnerPos(&dst.x)) return Hud("F7: no position from %s yet", g_partner);
    TeleportTo(me, dst, "F7");
}

void StepTeleport(uint8_t* me) { // gameplay frames: held above the spot until its terrain streams in (15 s at most)
    if (!g_tp.active) return;
    vector at = Here(me);
    if (fabsf(at.x - g_tp.held.x) + fabsf(at.y - g_tp.held.y) + fabsf(at.z - g_tp.held.z) > 10.f) {
        g_tp.active = false; // a respawn, checkpoint or script moved you: drop it
        return;
    }
    float h, dt = *Rva<float>(site::Frametime);
    bool ground = Ground(g_tp.dst.x, g_tp.dst.z, h);
    if (!ground && (g_tp.t += dt < 0 ? 0 : dt > 0.1f ? 0.1f : dt) < 15.f) {
        g_tp.held = {g_tp.dst.x, g_tp.dst.y + 2.f, g_tp.dst.z};
        Place(g_tp.held, false);
        return;
    }
    if (!ground && !g_tp.back) { // dropping without terrain collision falls through the world
        Hud("F7: the partner's area did not load, going back");
        g_tp = {true, true, g_tp.origin, g_tp.origin, g_tp.held, 0.f};
        return;
    }
    g_tp.active = false;
    if (!ground) { Place(g_tp.dst, false); return; } // the origin did not load either: nothing better left
    bool ok, veh = Vehicle(me, ok) != nullptr;
    float y = h + (veh ? 2.f : 1.f);
    Place({g_tp.dst.x, g_tp.dst.y > y ? g_tp.dst.y : y, g_tp.dst.z}, !veh);
}

// ---- M5 host: join (§4.8 steps 2 and 5), re-join, pause mirror, loadout --------------------------------------------------
// The guest's join waits (a mission, a save running, streaming paused). It hears about missions only: the rest passes in
// a second or two and is retried each second.
void JoinWait(const char* why) {
    if (!strcmp(why, g_waitWhy)) return;
    snprintf(g_waitWhy, sizeof g_waitWhy, "%s", why);
    Log("session: %s's join waits: %s", g_partner, why);
    if (!World_InMission()) return;
    MsgReject m{kJoinWait, {}};
    snprintf(m.reason, sizeof m.reason, "%s", why);
    NetSend(kChControl, &m, sizeof m);
    Hud("Co-op: %s joins when you are in free roam (%s)", g_partner, why);
}

// BEGIN + chunks on channel 2 (reliable ordered: they arrive whole and in order), then we stay paused until JOINED.
// resend: the buffer the last load used (not a fresh capture). M6: the blob id is the destruction epoch; its mode goes in
// BEGIN, and a mid-mission re-join resend after non-empty steps since our load is stale (mode 0, §4.2 stale rule).
void SendBlob(const uint8_t* p, int n, uint8_t* me, bool resend) {
    MsgBlobBegin b{};
    b.type = kBlobBegin, b.kind = g_hjKind, b.id = ++g_joinId, b.size = (uint32_t)n;
    b.destr = Destr_HostEpoch(b.id, resend, resend && g_hjKind == kBlobAsked);
    if ((g_hStale = b.destr & 0x80)) // bit 6: this host re-syncs it when the mission ends (Q21), so the guest's HUD can say so
        b.destr |= g_ini.autoresync ? 0x40 : 0, Log("session: stale re-join: %d steps or restores since the load", Destr_SinceLoad());
    World_ReadMeta(b.meta);
    vector at = Here(me);
    memcpy(b.pos, &at, sizeof b.pos);
    memcpy(b.clock, Rva<uint8_t>(site::GameClock) + 4, 3);
    double t0 = NowMs();
    NetSend(kChBlob, &b, sizeof b);
    static uint8_t c[sizeof(MsgBlobChunk) + kBlobChunkBytes];
    unsigned chunks = 0;
    for (uint32_t off = 0; off < b.size; off += kBlobChunkBytes, chunks++) {
        MsgBlobChunk h{kBlobChunk, b.id, (uint16_t)chunks};
        uint32_t k = b.size - off < kBlobChunkBytes ? b.size - off : kBlobChunkBytes;
        memcpy(c, &h, sizeof h), memcpy(c + sizeof h, p + off, k);
        NetSend(kChBlob, c, sizeof h + k);
    }
    g_blobSent = true, g_hj = hjSent, g_hjSentAt = NowMs(), *g_waitWhy = 0;
    Log("session: blob %u (%s) to %s: %d bytes in %u chunks, queued in %.0f ms; dlc %d, we are at %.1f %.1f %.1f, clock "
        "%02u:%02u:%02u, destr %02X", b.id, b.kind == kBlobJoin ? "join" : b.kind == kBlobHostLoad ? "after our load" : "asked",
        g_partner, n, chunks, g_hjSentAt - t0, (int)b.meta.dlc, at.x, at.y, at.z, b.clock[0], b.clock[1], b.clock[2], b.destr);
    Hud(b.kind == kBlobJoin ? "Partner joining..." : "Partner re-joining...");
}

void HostJoin(int state, uint8_t* me) {
    double now = NowMs();
    if (g_hj == hjSent && now - g_hjSentAt > 120000) { // never leave the host paused for a guest that went quiet
        Log("session: no JOINED for blob %u in 120 s: unpaused", g_joinId);
        Hud("Co-op: %s did not finish joining", g_partner);
        g_hj = hjIdle;
    }
    if (g_hj == hjIdle || g_hj == hjSent || g_hj == hjCapture || state != kGsGameplay || !me) return;
    if (g_hj == hjResend) { // the buffer the last load used (stored_world_state[World+0x2578]), never a fresh one mid-mission
        int n;
        if (const uint8_t* p = World_CurrentState(n)) return SendBlob(p, n, me, true);
        Log("session: no loaded world buffer to re-send (a new game?): a fresh silent blob once in free roam");
        g_hj = hjWant, g_hjAt = 0;
    }
    if (now < g_hjAt || g_capMine) return;
    if (const char* why = World_CaptureStart()) { // §4.8 step 2: free roam only
        // M6: a capture that failed after its stamp (or the 16 MB end) left no live epoch to wait on: in a mission the
        // guest would follow a frozen one until it ends. The loaded buffer as is instead (REJOIN_REQ's path: stale, Q21);
        // none to re-send: the 1 s JoinWait below, as ver6.4 (never a per-frame hjResend <-> hjWant loop)
        int cur;
        if (g_blobSent && !Destr_Streaming() && World_InMission() && World_CurrentState(cur)) {
            Log("session: no live stream for %s and a mission started: the loaded buffer as is", g_partner);
            g_hjKind = kBlobAsked, g_hj = hjResend, g_hjAt = 0;
            return;
        }
        g_hjAt = now + 1000;
        return JoinWait(why);
    }
    g_capMine = true, g_hj = hjCapture, g_hjPlayer = me;
}

void PollCapture() { // every tick while ours is in flight, linked or not (world.cpp allows one capture at a time)
    if (!g_capMine) return;
    std::vector<uint8_t> blob;
    int r = World_CapturePoll(blob);
    if (r == 0) return;
    g_capMine = false;
    if (g_hj != hjCapture) return; // a load, a stop or the link's end made it moot
    uint8_t* me = LocalPlayer();
    if (r < 0 || me != g_hjPlayer || g_state != kGsGameplay) {
        Log("session: capture for %s's join failed: retrying in 3 s", g_partner);
        g_hj = hjWant, g_hjAt = NowMs() + 3000;
        return;
    }
    SendBlob(blob.data(), (int)blob.size(), me, false);
}

bool HostPaused(int state) { // §4.7: Esc, map, menus, videos, the death screen; not our own join token
    if (*Rva<int>(site::PausedCount) - (g_myPause ? 1 : 0) > 0) return true;
    if (state == kGsGameplay || state < 0 || state >= 64) return false;
    const uint8_t* si = Rva<uint8_t>(site::StateInfo) + state * 0x24;
    return !si[0x18] || si[0x19]; // a top state that hides gameplay, or one that pauses what is beneath
}

// Q13: upgrade table, Weapon_list arsenal and salvage, about once a second; LOADOUT when anything changed, and once
// after every JOINED (g_loadoutForce). Only once the guest has a world from us and is not loading another one.
void PollLoadout(int state, uint8_t* me) {
    double now = NowMs();
    if (!g_blobSent || g_hj != hjIdle || state != kGsGameplay || !me || now - g_loadoutAt < 1000) return;
    g_loadoutAt = now;
    MsgLoadout m{};
    m.type = kLoadout;
    const uint8_t* meta = me + site::MetaData;
    m.salvage = Field<int32_t>(meta, 0x4);
    memcpy(m.upgrades, meta + 0x18, sizeof m.upgrades);
    const uint8_t* wl = Rva<uint8_t>(site::WeaponList);
    const uint8_t* items = Field<const uint8_t*>(wl, 0x4);
    int count = Field<int>(wl, 0xC);
    if (items && count > 0 && count <= 64) m.arsenalBytes = (uint16_t)(count * 8), memcpy(m.arsenal, items, count * 8);
    if (!g_loadoutForce && !memcmp(&m, &g_loadout, sizeof m)) return;
    uint32_t crc = Crc(Crc(0, m.upgrades, sizeof m.upgrades), m.arsenal, m.arsenalBytes);
    if (g_loadoutForce || crc != g_loadoutCrc) // salvage-only changes are not logged
        Log("session: LOADOUT to %s: upgrades + arsenal %08X, %d arsenal items (Weapon_list count %d), salvage %d", g_partner,
            crc, m.arsenalBytes / 8, count, m.salvage);
    g_loadout = m, g_loadoutCrc = crc, g_loadoutForce = false;
    NetSend(kChBlob, &m, sizeof m);
}

// ---- M5 guest: load the host's world, land beside the host, JOINED (§4.8 steps 3 and 4) -------------------------------
void GuestJoin(int state, uint8_t* me) {
    double now = NowMs();
    // gameplay, or the death screen (the guest's own death becomes a re-join: guest.cpp keeps Continue from loading), or
    // the mission-fail screen (backstop: GuestBusy keeps missions out; vanilla loads from it too, humc_request_exit 0x44A8D8)
    bool canLoad = me && (state == kGsGameplay || state == kGsDeathOptions || state == kGsMissionComplete);
    // died while landing: arrive and the teleport hold run only in gameplay, and Session_RejoinRequest refuses while a
    // step runs, so the death screen would wait for ever. Idle: Continue's REJOIN_REQ (the host takes it while waiting
    // for JOINED) or a pending blob takes over; no JOINED
    if (state == kGsDeathOptions && (g_ga == gaArrive || g_ga == gaTeleport)) {
        Log("session: died while landing (blob %u): the re-join takes over", g_gaHdr.id);
        g_ga = gaIdle, g_tp.active = false;
    }
    switch (g_ga) {
    case gaIdle:
        if (!g_pend.on || !canLoad) return;
        if (!Guest_Armed()) return Stop("the guest fences are not on (RfgCoop.log): this game cannot take the host's world");
        g_gaHdr = g_pend.b, g_gaTransfer = (uint32_t)(g_pend.done - g_pend.at), g_ga = gaLoad, g_gaAt = now;
        Log("session: loading blob %u from memory", g_gaHdr.id);
        Hud("Co-op: loading %s's world", g_partner);
        [[fallthrough]];
    case gaLoad: {
        if (!g_pend.on) { g_ga = gaIdle; return; } // dropped meanwhile (the host is loading)
        if (!canLoad) { g_gaAt = now; return; }    // a menu: the timeout counts only while a load could start
        int r = World_Load(g_pend.data.data(), (int)g_pend.data.size(), g_pend.b.meta, true);
        if (r == 0) {
            if (now - g_gaAt > 15000) Stop("join failed: the world load did not start in 15 s (RfgCoop.log)");
            return;
        }
        // the blob actually handed over: a newer one may have replaced g_pend while we waited (OnBlob), and JOINED must
        // carry its id (the host ignores any other and stays paused)
        g_gaHdr = g_pend.b, g_gaTransfer = (uint32_t)(g_pend.done - g_pend.at);
        g_pend = {}; // the game has its copy
        if (r < 0) {
            g_ga = gaIdle;
            return Session_RejoinRequest("the host's world was refused here");
        }
        g_ga = gaWait, g_gaAt = now, g_gaLeft = false, g_loadedId = 0; // the world here is going: no stream applies to it
        return;
    }
    case gaWait: // every load destroys the players: Player NULL is the proof this one started
        if (!me) { g_gaLeft = true; return; }
        if (!g_gaLeft) {
            if (now - g_gaAt > 15000) {
                Log("session: the world load was accepted but never started");
                g_ga = gaIdle;
                Session_RejoinRequest("the world load did not start");
            }
            return;
        }
        if (state != kGsGameplay) return;
        {
            const uint8_t* clk = Rva<uint8_t>(site::GameClock) + 4;
            vector at = Here(me);
            g_gaLoadMs = (uint32_t)(now - g_gaAt), g_ga = gaArrive, g_gaAt = now, g_loadedId = g_gaHdr.id;
            Log("session: in the host's world after %u ms (transfer %u ms): at %.1f %.1f %.1f, Mission_active_index %d, clock "
                "%02u:%02u:%02u (the host's was %02u:%02u:%02u)", g_gaLoadMs, g_gaTransfer, at.x, at.y, at.z,
                *Rva<int>(site::MissionActive), clk[0], clk[1], clk[2], g_gaHdr.clock[0], g_gaHdr.clock[1], g_gaHdr.clock[2]);
        }
        return;
    case gaArrive:
        if (!me || state != kGsGameplay) return;
        if (g_pend.on || g_hostLoading) { // a newer world is coming: load that one instead
            g_ga = gaIdle;
            return Log("session: blob %u superseded: no teleport", g_gaHdr.id);
        }
        if (now - g_gaAt < 1000) return; // the load's own spawn (nearest safehouse, save_state_finish) settles first
        {
            vector dst;
            if (!Puppet_PartnerPos(&dst.x)) memcpy(&dst, g_gaHdr.pos, sizeof dst); // no fresh PSTATE: where it sent from
            TeleportTo(me, dst, "Join");
        }
        g_ga = gaTeleport;
        return;
    case gaTeleport: {
        if (!me || g_pend.on || g_hostLoading) { g_ga = gaIdle; return; } // StepTeleport drops the hold after a level change
        if (g_tp.active) return;
        vector here = Here(me), host;
        float dist = -1;
        if (Puppet_PartnerPos(&host.x))
            dist = sqrtf((here.x - host.x) * (here.x - host.x) + (here.y - host.y) * (here.y - host.y) + (here.z - host.z) * (here.z - host.z));
        MsgJoined j{kJoined, g_gaHdr.id, g_gaTransfer, g_gaLoadMs, dist};
        NetSend(kChControl, &j, sizeof j);
        g_ga = gaIdle;
        Log("session: joined (blob %u): transfer %u ms, load %u ms, %.1f m from the host after the teleport (-1 = unknown)",
            j.id, g_gaTransfer, g_gaLoadMs, dist);
        if (g_gaHdr.destr & 0x80) // M6 stale epoch: parallel destruction until the host's mission ends (Q21; bit 6 = autoresync)
            return Hud("Your world misses the host's changes since its mission began; %s",
                       g_gaHdr.destr & 0x40 ? "it re-syncs when the mission ends" : "press F3 after the host's mission");
        Hud(g_gaHdr.kind == kBlobJoin ? "Co-op: joined %s" : "Co-op: re-joined %s", g_partner);
        return;
    }
    }
}

} // namespace

// ---- features.h (guest.cpp calls these on the sim thread) ------------------------------------------------------------
// A load or a death the guest may not do itself: the host sends its world again. One request per 5 s, whoever asks
// (the only debounce on the guest; the host only bounds floods at 2.5 s); one inside the window is held and sent when
// it opens (Net_Tick), not dropped (a death's Continue there would otherwise do nothing, with no word on the death screen).
void Session_RejoinRequest(const char* why) {
    if (g_ini.host) return;
    if (!why) why = "?";
    double now = NowMs();
    if (now - g_rejoinAt < 5000) {
        if (strcmp(g_rejoinHeld, why)) Log("session: re-join wanted (%s): within 5 s of the last request, held until then", why);
        snprintf(g_rejoinHeld, sizeof g_rejoinHeld, "%s", why);
        return;
    }
    g_rejoinAt = now, *g_rejoinHeld = 0;
    if (g_phase != Linked) {
        Log("session: re-join wanted (%s) but no host is linked", why);
        return Hud("Co-op: no host. Restart the game to continue your own campaign");
    }
    if (g_rx.on || g_pend.on || g_ga != gaIdle) return Log("session: re-join wanted (%s): a world is already on its way", why);
    MsgReject m{kRejoinReq, {}};
    snprintf(m.reason, sizeof m.reason, "%s", why);
    NetSend(kChControl, &m, sizeof m);
    Log("session: REJOIN_REQ sent (%s)", m.reason);
    Hud("Co-op: re-joining %s", g_partner);
}

void Session_SendSalvage(int amount) { // rule Q13: summed here, sent at most 4 times a second (Net_Tick)
    if (amount <= 0 || g_ini.host) return;
    if (g_phase != Linked) return Log("session: %d salvage collected with no host linked: not sent", amount);
    g_salvOut = g_salvOut + amount > 1000000 ? 1000000 : g_salvOut + amount;
}

bool Session_Active() { return g_phase != Off; }

// M6 epoch gate (§4.2): applied only once that blob is loaded here and landed (7.2 adds gaReplay)
bool Session_EpochLoaded(uint32_t epoch) {
    return g_phase == Linked && !g_ini.host && epoch && g_loadedId == epoch && g_ga == gaIdle && !g_pend.on && !g_hostLoading;
}
// open while a blob is only wanted (an accepted F3 whose capture met a mission waits for free roam): the guest still
// follows the live epoch, whose id (g_hId) holds until a capture's stamp (Destr_Stamp, inside the capture). A capture
// that fails after its stamp leaves g_hId 0 (nothing goes out) until the next blob: the 3 s retry in free roam,
// HostJoin's resend of the loaded buffer in a mission
bool Session_StreamOpen() {
    return g_phase == Linked && g_ini.host && g_blobSent && (g_hj == hjIdle || (g_hj == hjWant && !g_capMine)) && g_sentReady == 1;
}
bool Session_Capturing() { return g_hj == hjCapture; }
void Session_AskBlob() { g_hjKind = kBlobAsked, g_hj = hjWant, g_hjAt = 0; } // as F3: a fresh silent blob (free roam)

void Session_TeleportTo(const float pos[3], const char* why) {
    if (uint8_t* me = LocalPlayer()) TeleportTo(me, {pos[0], pos[1], pos[2]}, why);
}
bool Session_Teleporting() { return g_tp.active; }

void Net_Install() { // ini only: no thread, no socket here (DllMain)
    wchar_t w[128];
    IniStr(L"role", w, 128);
    g_ini.host = !_wcsicmp(w, L"host"), g_ini.guest = !_wcsicmp(w, L"guest");
    int port = IniInt(L"port", 27777);
    g_ini.port = g_ini.dialPort = (uint16_t)(port > 0 && port < 65536 ? port : 27777);
    IniStr(L"connect", w, 128); // ip:port (or name:port); the port defaults to `port`
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_ini.addr, sizeof g_ini.addr, nullptr, nullptr);
    if (char* colon = strrchr(g_ini.addr, ':')) {
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536) g_ini.dialPort = (uint16_t)p;
        *colon = 0;
    }
    IniStr(L"name", w, 24);
    if (!*w) IniStr(L"tag", w, 24);
    WideCharToMultiByte(CP_ACP, 0, *w ? w : L"?", -1, g_ini.name, sizeof g_ini.name, nullptr, nullptr);
    g_ini.name[sizeof g_ini.name - 1] = 0;
    Clean(g_ini.name, sizeof g_ini.name);
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    g_ini.autoconnect = IniInt(L"autoconnect", 0);
    g_ini.lag = clamp(IniInt(L"lag_ms", 0), 0, 5000), g_ini.jitter = clamp(IniInt(L"jitter_ms", 0), 0, 5000);
    g_ini.loss = clamp(IniInt(L"loss_pct", 0), 0, 100);
    g_ini.autoresync = IniInt(L"destr_autoresync", 1) != 0;
    if (!g_ini.host && !g_ini.guest) return Log("session: no role in RfgCoop.ini: co-op off");
    Log("session: role %s, port %u, connect %s:%u, autoconnect %d, lag %d ms, jitter %d ms, loss %d%%, "
        "name %s; F8 starts/stops, F7 teleports to the partner, F3 (guest) re-syncs the world (the password is read at each "
        "start); destr_autoresync %d", g_ini.host ? "host" : "guest", (unsigned)g_ini.port, g_ini.addr, (unsigned)g_ini.dialPort,
        g_ini.autoconnect, g_ini.lag, g_ini.jitter, g_ini.loss, g_ini.name, g_ini.autoresync);
}

void Net_Tick(int state) {
    g_state = state;
    uint8_t* me = LocalPlayer();
    bool fg = *Rva<uint8_t>(rva::OsAppActive);
    bool f8 = fg && GetAsyncKeyState(VK_F8) < 0, f7 = fg && GetAsyncKeyState(VK_F7) < 0; // edges on the async high bit
    bool f3 = fg && g_ini.guest && GetAsyncKeyState(VK_F3) < 0;
    if (f8 && !g_f8) {
        if (g_phase == Off) Start(g_ini.guest); // a guest picks its hammer and backpack first (Q17, picker.cpp)
        else Stop("session stopped (F8)");
    }
    if (Picker_Tick() && g_phase == Off) Start();
    if (f7 && !g_f7 && state == 1 && me) Teleport(me);
    if (f3 && !g_f3 && state == 1 && me && g_phase != Off) Resync();
    g_f8 = f8, g_f7 = f7, g_f3 = f3;
    if (g_ini.autoconnect && !g_autoDone && state == 1 && me) {
        g_autoDone = true;
        if (g_phase == Off) Start();
    }
    Drain();
    // Q12 on the host, hosting or linked: the first gameplay frame with a new Player (after any load) checks again.
    // After Drain: a guest connected during the load is then in the handshake (or linked) and gets the reason (Stop)
    if (!me) g_checked = nullptr;
    if (g_ini.host && g_phase != Off && state == 1 && me && me != g_checked) {
        g_checked = me;
        if (!TutorialDone()) Stop("session stopped: the host's save has not finished the Tutorial");
    }
    // A guest at the main menu (Esc -> Quit, the death screen's Main menu; a load never passes state 0): armed, it can
    // load nothing of its own and the blob needs a Player, so it could never join again. End the session: the host gets
    // BYE instead of a 120 s join pause, and the title says to restart (Status)
    if (!g_ini.host && g_phase != Off && state == kGsMainMenu && !me) Stop("the guest quit to the main menu");
    // per handshake step (from the connect, CHALLENGE sent or AUTH sent to the next message): 10 s + the lag shim's
    // worst case both ways (lag_ms + jitter_ms, each up to 5000, on either game). ENet already confirmed the peer is
    // alive, so the longer wait costs nothing. Not a strike (see g_strikes).
    if (g_phase == Shaking && NowMs() - g_phaseAt > 10000 + 2 * (5000 + 5000)) {
        Log("session: handshake timed out");
        Hud("Co-op: handshake timed out");
        NetKick();
        g_phase = Waiting, g_phaseAt = NowMs();
    }
    PollCapture();
    if (g_phase == Linked) {
        LevelState(state, me);
        SendState();
        // M6 §3.3: a reliable item lost to a full inbox or the lag shim. The guest's world may miss a destruction step
        if (uint32_t d = NetDrops(); d != g_netDrops) {
            Log("net: drops %u (a reliable message was lost: inbox or lag shim full)", d);
            g_netDrops = d;
            if (!g_ini.host) Destr_NetDrop();
        }
        if (g_ini.host) {
            HostJoin(state, me);
            // Q21: a stale epoch (an in-mission re-join after changes) re-syncs when the mission ends with no load between
            // (a load re-sends a fresh epoch by itself). A blob already on its way decides instead
            if (state == kGsGameplay && me) {
                bool m = World_InMission();
                if (g_hStale && g_wasMission && !m && g_ini.autoresync) {
                    if (g_hj == hjIdle) {
                        Log("session: the mission is over: re-syncing %s (blob %u was stale)", g_partner, g_joinId);
                        g_hjKind = kBlobAsked, g_hj = hjWant, g_hjAt = 0, g_wasMission = false;
                    }
                } else
                    g_wasMission = m;
            }
            if (bool p = me && HostPaused(state); p != g_sentPause) { // §4.7: the guest pauses with its own token
                MsgPaused m{kPaused, p};
                NetSend(kChControl, &m, sizeof m);
                g_sentPause = p;
                Log("session: sent PAUSED %d (state %d, Game_paused_count %d)", p, state, *Rva<int>(site::PausedCount));
            }
            PollLoadout(state, me);
        } else {
            GuestJoin(state, me);
            if (*g_rejoinHeld && NowMs() - g_rejoinAt >= 5000) { // a request held by the 5 s window: its turn now
                char why[sizeof g_rejoinHeld];
                memcpy(why, g_rejoinHeld, sizeof why);
                Session_RejoinRequest(why);
            }
            if (g_salvOut && NowMs() - g_salvAt >= 250) {
                MsgSalvage m{kSalvage, g_salvOut > 30000 ? 30000 : g_salvOut};
                g_salvOut -= m.amount, g_salvAt = NowMs();
                NetSend(kChControl, &m, sizeof m);
                Log("session: sent %d salvage to the host", (int)m.amount);
            }
            if (g_loadoutIn && me && state == kGsGameplay && g_ga == gaIdle && !g_pend.on) { // not under a load
                g_loadoutIn = false;
                uint32_t crc = Crc(Crc(0, g_loadout.upgrades, sizeof g_loadout.upgrades), g_loadout.arsenal, g_loadout.arsenalBytes);
                if (crc != g_loadoutCrc)
                    Log("session: host loadout applied: upgrades + arsenal %08X, %d arsenal items, salvage %d", crc,
                        g_loadout.arsenalBytes / 8, (int)g_loadout.salvage);
                g_loadoutCrc = crc;
                Rules_ApplyHostLoadout(g_loadout.upgrades, sizeof g_loadout.upgrades, g_loadout.arsenal, g_loadout.arsenalBytes,
                                       g_loadout.salvage);
            }
        }
    }
    // Q13, host: the guest's salvage is ours once received, linked or not (a SALVAGE drained with its BYE and disconnect,
    // or one that waited out a menu, the map or a load); added in gameplay only
    if (g_ini.host && g_salvIn && me && state == kGsGameplay) {
        Log("session: +%d salvage from %s", g_salvIn, g_partner);
        Rules_AddHostSalvage(g_salvIn);
        g_salvIn = 0;
    }
    // host: while the guest joins; guest: while the host is paused, never while it loads or lands
    PauseToken(g_phase == Linked && (g_ini.host ? g_hj == hjSent : g_hostPaused && g_ga == gaIdle && !g_tp.active));
    // a hold in progress lands or goes back even if the link drops (dst and origin are in g_tp); only Teleport() needs a link
    if (state == 1 && me) StepTeleport(me);
    Status();
    if (*g_hudPending && state == 1 && me) {
        Fn<int __cdecl(char*, float, void*, void*, int*)>(site::HudMessageAdd)(g_hudPending, 4.0f, nullptr, nullptr, nullptr);
        *g_hudPending = 0;
    }
}
