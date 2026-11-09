// Shared game access for RfgCoop. Target: GOG rfg.exe (32-bit, PE timestamp 0x5B9B713D), addresses are PDB RVAs.
// Research and every address: research/RESEARCH.md (§2, §4.3) and research/notes/*.md.
#pragma once
#include <windows.h>
#include <cstdint>

extern uintptr_t g_base; // GetModuleHandleW(nullptr)

template <class T = void> inline T* Rva(uintptr_t rva) { return reinterpret_cast<T*>(g_base + rva); }
template <class T> inline T& Field(const void* obj, uintptr_t off) { return *reinterpret_cast<T*>((uintptr_t)obj + off); }
// Typed game function: Fn<void __cdecl(int)>(0x2F6920)(5);
template <class Sig> inline Sig* Fn(uintptr_t rva) { return reinterpret_cast<Sig*>(g_base + rva); }

namespace rva {
constexpr uintptr_t Player          = 0x02C22874; // player* (local player, NULL outside a level)
constexpr uintptr_t World           = 0x02B97490; // world (object itself)
constexpr uintptr_t Multiplayer     = 0x02BEA588; // bool: never set it (RESEARCH.md §1)
constexpr uintptr_t Server          = 0x02BEA590; // bool: expected 0 in SP (general_mover::apply_damage gate, §3.7)
constexpr uintptr_t GameseqGetState = 0x003BFC70; // int __cdecl(): top game state, 1 = GS_GAMEPLAY
constexpr uintptr_t Framecount      = 0x018E7710; // int, +1 per sim frame
constexpr uintptr_t FramesPerSecond = 0x0111856C; // float, rolling average
constexpr uintptr_t FrametimeMin    = 0x01118574; // float, 1/300 after boot (frame limiter)
constexpr uintptr_t OsAppActive     = 0x01118591; // bool, 0 while the window is unfocused
constexpr uintptr_t WindowStates    = 0x034BC540; // s_windowStates[4], stride 0x38: +4 HWND
constexpr uintptr_t DestrUid        = 0x01A3733C; // Rfg_mover_destruction_uid: +1 per chunk (0x33D298), 0 at level init
} // namespace rva

inline int GameState() { return Fn<int __cdecl()>(rva::GameseqGetState)(); }

void Log(const char* fmt, ...);                      // RfgCoop.log next to the dll, lines "[tag] qpc_ms ..."
void WriteCode(void* at, const void* src, size_t n); // overwrite game code or data (unprotect, copy, flush)
// Points the rel32 call/jmp at `at` to `to` and returns its previous target (the game's function or a sibling mod's
// wrapper). 0 = not a call/jmp there, nothing written.
uintptr_t ChainRel32(uintptr_t at, void* to);
// Writes `to` over `expect` at `at` if the bytes there are exactly `expect`; logs either way.
bool PatchBytes(uintptr_t at, const void* expect, const void* to, size_t n, const char* what);
void QuickPauseOff(bool off); // sim thread / DllMain: alt-tab quick pause off (true) or back on (false; never with dev=1)
void Core_SetFpsCap(int cap); // sim thread (E4 runner): frame cap; 0 = back to the ini's fps_cap
inline uint32_t Fnv(const uint8_t* p, int n) { uint32_t h = 2166136261u; while (n-- > 0) h = (h ^ *p++) * 16777619u; return h; } // FNV-1a

// RfgCoop.ini next to the dll, section [coop]
int IniInt(const wchar_t* key, int def);
void IniStr(const wchar_t* key, wchar_t* out, int n);

double NowMs();                       // absolute QPC ms, any thread
const wchar_t* ModDir();              // folder of this dll (= the game folder), with trailing backslash
void TitleStatus(const char* status); // any thread: window title = original + " [RfgCoop tag] " + status (core's helper thread)
