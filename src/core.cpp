// Loader, tick and dev mode. rfg.exe imports XINPUT1_3.dll (ordinals 2/3), so this DLL sits in the game folder as
// xinput1_3.dll (other proxy-DLL mods such as dinput8.dll or winmm.dll can sit beside it) and forwards every XInput call to the
// system DLL. Sites (RESEARCH.md §4.3, notes/infra.md):
//   - 0x1D24D9 `call multi_process`: the session tick, every sim frame in every SP state (chained, nobody else uses it)
//   - 0x4DDB5F quick-pause jne->jmp (alt-tab no longer freezes the sim): while a co-op session runs, all run with dev=1
//   - dev=1: s_allowBackgroundInput 0x12EF120 = 0 (the pad drives only the focused window), save_tag=1: "keen"->"kee2"
//     save names (second instance on one PC)
#include "features.h"
#include "game.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

uintptr_t g_base;

namespace {

namespace site {
constexpr DWORD TimeDateStamp = 0x5B9B713D, SizeOfImage = 0x397B000;
constexpr uintptr_t MultiProcessCall = 0x001D24D9; // rfg_do_frame: E8 -> multi_process 0x604C20, void __cdecl(bool)
constexpr uintptr_t QuickPauseJne    = 0x004DDB5F; // interface_process: `75 16` after call os_foreground
constexpr uintptr_t AllowBgInput     = 0x012EF120; // keen::input s_allowBackgroundInput (bool, 1 in .data)
constexpr uintptr_t SaveFmtN         = 0x00F44209; // "%skeen_savegame_%u_%u.sav": the n of keen
constexpr uintptr_t SaveTokenN       = 0x00F4422F; // "keen" token getSaveDataInfoFromFileName compares
} // namespace site

wchar_t g_dir[MAX_PATH]; // folder of this dll, with trailing backslash
wchar_t g_ini[MAX_PATH];
char g_tag[16] = "A";    // log and title tag (ini: tag)
double g_qpcMs;          // QPC ticks per ms
// Title status (TitleStatus): text after the tag, handed to WindowThread, which owns every SetWindowTextW
SRWLOCK g_titleLock = SRWLOCK_INIT;
char g_titleStatus[128];
HANDLE g_titleEvent;     // auto-reset, created on the sim thread right before WindowThread

bool BuildMatches() {
    auto nt = (IMAGE_NT_HEADERS32*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    return nt->FileHeader.TimeDateStamp == site::TimeDateStamp && nt->OptionalHeader.SizeOfImage == site::SizeOfImage;
}

// First-chance crash logger: the crash RVA plus rfg.exe addresses on the stack.
// ponytail: logs the first 3 faults only and scans the raw stack, no unwinding.
LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    static LONG count;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&count) > 3) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t end = g_base + ((IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew))->OptionalHeader.SizeOfImage;
    auto rel = [&](uintptr_t a) { return a >= g_base && a < end ? a - g_base : 0; };
    CONTEXT* c = e->ContextRecord;
    Log("CRASH %08lX at %p (rfg+%X) addr %p eax %08lX ecx %08lX edx %08lX esi %08lX edi %08lX", code,
        e->ExceptionRecord->ExceptionAddress, (unsigned)rel((uintptr_t)e->ExceptionRecord->ExceptionAddress),
        (void*)e->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi);
    char line[1024];
    int len = 0, found = 0;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    for (uintptr_t* p = (uintptr_t*)c->Esp; p < (uintptr_t*)tib->StackBase && found < 40; p++)
        if (uintptr_t r = rel(*p)) len += snprintf(line + len, sizeof line - len, " %X", (unsigned)r), found++;
    Log("  stack rfg RVAs:%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- XInput forwarding: the system DLL by full path, loaded on the first call (never in DllMain)
const char* const kXNames[] = {"XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputEnable",
                               "XInputGetDSoundAudioDeviceGuids", "XInputGetBatteryInformation", "XInputGetKeystroke",
                               MAKEINTRESOURCEA(100)};
void* g_x[8];
std::atomic<bool> g_xLoaded;

void* RealX(int i) {
    if (!g_xLoaded.load(std::memory_order_acquire)) { // ponytail: a racing first call loads twice, harmless
        wchar_t p[MAX_PATH];
        UINT n = GetSystemDirectoryW(p, MAX_PATH); // SysWOW64 for this 32-bit process
        wcscpy(p + n, L"\\XInput1_3.dll");
        HMODULE m = LoadLibraryW(p); // a full path: the loader keeps it apart from this xinput1_3.dll
        for (int k = 0; k < 8; k++) g_x[k] = m ? (void*)GetProcAddress(m, kXNames[k]) : nullptr;
        Log(m ? "xinput forwarded to %ls" : "system XInput not found: %ls (no gamepad)", p);
        g_xLoaded.store(true, std::memory_order_release);
    }
    return g_x[i];
}
template <class F> F X(int i) { return (F)RealX(i); }

// ---- tick (0x1D24D9)
using MultiProcessFn = void(__cdecl*)(bool);
MultiProcessFn g_mpNext;
struct { int dev, fpsCap; } g_cfg;

// SetWindowText/Pos send messages to the window's thread: never from the sim thread. Places the window once, then
// rewrites the title (original + " [RfgCoop tag] status") whenever TitleStatus signals; lives until the process exits.
DWORD WINAPI WindowThread(void* hwnd) {
    HWND h = (HWND)hwnd;
    wchar_t base[200], title[400], tag[16], rect[64], status[128];
    MultiByteToWideChar(CP_ACP, 0, g_tag, -1, tag, 16);
    GetWindowTextW(h, base, 200);
    IniStr(L"window", rect, 64);
    int x, y, w, ht;
    if (swscanf(rect, L"%d,%d,%d,%d", &x, &y, &w, &ht) == 4) {
        SetWindowPos(h, nullptr, x, y, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
        Log("window placed at %d,%d %dx%d", x, y, w, ht);
    }
    for (;;) {
        AcquireSRWLockShared(&g_titleLock);
        MultiByteToWideChar(CP_ACP, 0, g_titleStatus, -1, status, 128);
        ReleaseSRWLockShared(&g_titleLock);
        swprintf(title, 400, L"%ls [RfgCoop %ls]%ls%ls", base, tag, *status ? L" " : L"", status);
        SetWindowTextW(h, title);
        WaitForSingleObject(g_titleEvent, INFINITE);
    }
}

void LogSiblings() {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%lsdinput8.dll", g_dir);
    bool di8 = GetModuleHandleW(p);
    swprintf(p, MAX_PATH, L"%lswinmm.dll", g_dir);
    bool wmm = GetModuleHandleW(p);
    Log("other mods: dinput8.dll %s, winmm.dll %s", di8 ? "loaded" : "absent", wmm ? "loaded" : "absent");
}

void Tick() {
    static double last, lastStat;
    static int state = -2;
    static bool once, serverLogged, windowDone;
    double now = NowMs();
    int s = GameState();
    if (!once) { once = true; LogSiblings(); }
    if (last && now - last > 250) Log("tick gap %.0f ms (state %d -> %d)", now - last, state, s); // E9
    last = now;
    if (s != state) {
        Log("state %d -> %d", state, s);
        state = s;
        if (s == 1 && !serverLogged) { // expected 0/0: the guest's general-mover gate relies on Server == 0 (§3.7)
            serverLogged = true;
            Log("in gameplay: Server = %d, Multiplayer = %d", *Rva<uint8_t>(rva::Server), *Rva<uint8_t>(rva::Multiplayer));
        }
    }
    if (g_cfg.fpsCap > 0) *Rva<float>(rva::FrametimeMin) = 1.0f / g_cfg.fpsCap; // boot sets 1/300 (0x1D93B0)
    if (g_cfg.dev && now - lastStat >= 5000) { // E1: an unfocused instance keeps counting frames
        lastStat = now;
        Log("frame %d fps %.0f focused %d state %d uid %u chunks %d", *Rva<int>(rva::Framecount), *Rva<float>(rva::FramesPerSecond),
            *Rva<uint8_t>(rva::OsAppActive), s, *Rva<uint32_t>(rva::DestrUid), Destr_Chunks());
    }
    if (!windowDone) {
        if (HWND h = Field<HWND>(Rva(rva::WindowStates), 4)) {
            windowDone = true;
            g_titleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (HANDLE t = CreateThread(nullptr, 0, WindowThread, h, 0, nullptr)) CloseHandle(t);
        }
    }
    Puppet_Tick(s);
    Net_Tick(s);
    Destr_Tick(s);
    World_Tick(s);
    Guest_Tick(s);
}

void __cdecl OnMultiProcess(bool booted) {
    g_mpNext(booted);
    Tick();
}

void InstallDev(bool saveTag) {
    static const uint8_t on = 1, off = 0, n = 'n', two = '2';
    if (g_cfg.dev) {
        QuickPauseOff(true);
        PatchBytes(site::AllowBgInput, &on, &off, 1, "dev: pad drives only the focused window");
    }
    if (saveTag) { // before any save read: the image is mapped and relocated before import DLLs initialize
        PatchBytes(site::SaveFmtN, &n, &two, 1, "save_tag: file name kee2_savegame_*.sav");
        PatchBytes(site::SaveTokenN, &n, &two, 1, "save_tag: save list reads only kee2_*");
    }
}

} // namespace

// ---- exports (xinput1_3.def): rfg.exe imports ordinals 2 and 3
extern "C" {
DWORD WINAPI XInputGetState(DWORD i, void* s) {
    auto f = X<DWORD(WINAPI*)(DWORD, void*)>(0);
    return f ? f(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputSetState(DWORD i, void* v) {
    auto f = X<DWORD(WINAPI*)(DWORD, void*)>(1);
    return f ? f(i, v) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD flags, void* c) {
    auto f = X<DWORD(WINAPI*)(DWORD, DWORD, void*)>(2);
    return f ? f(i, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}
void WINAPI XInputEnable(BOOL e) {
    if (auto f = X<void(WINAPI*)(BOOL)>(3)) f(e);
}
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, GUID* r, GUID* c) {
    auto f = X<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>(4);
    return f ? f(i, r, c) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE type, void* b) {
    auto f = X<DWORD(WINAPI*)(DWORD, BYTE, void*)>(5);
    return f ? f(i, type, b) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD r, void* k) {
    auto f = X<DWORD(WINAPI*)(DWORD, DWORD, void*)>(6);
    return f ? f(i, r, k) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputGetStateEx(DWORD i, void* s) { // ordinal 100, NONAME
    auto f = X<DWORD(WINAPI*)(DWORD, void*)>(7);
    return f ? f(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}
}

void WriteCode(void* at, const void* src, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, src, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

uintptr_t ChainRel32(uintptr_t at, void* to) {
    uint8_t* s = Rva<uint8_t>(at);
    if (s[0] != 0xE8 && s[0] != 0xE9) return 0;
    uintptr_t prev = (uintptr_t)(s + 5 + *(int32_t*)(s + 1));
    int32_t rel = (int32_t)((uintptr_t)to - (uintptr_t)(s + 5));
    WriteCode(s + 1, &rel, 4);
    return prev;
}

bool PatchBytes(uintptr_t at, const void* expect, const void* to, size_t n, const char* what) {
    if (memcmp(Rva(at), expect, n)) {
        Log("%s: unexpected bytes at %X, not patched", what, (unsigned)at);
        return false;
    }
    WriteCode(Rva(at), to, n);
    Log("%s (%X)", what, (unsigned)at);
    return true;
}

// 0x4DDB5F interface_process `75 16` (jne after call os_foreground; falling through pushes GS_QUICK_PAUSE 0x3E at
// 0x4DDB6D, the only alt-tab pause) -> `EB 16`. Off for the whole run with dev=1 (which then owns the byte), else only
// while a co-op session runs (session.cpp Start / Stop, RESEARCH.md §4.7). Already as wanted = nothing to do.
void QuickPauseOff(bool off) {
    static const uint8_t jne = 0x75, jmp = 0xEB;
    if ((!off && g_cfg.dev) || *Rva<uint8_t>(site::QuickPauseJne) == (off ? jmp : jne)) return;
    PatchBytes(site::QuickPauseJne, off ? &jne : &jmp, off ? &jmp : &jne, 1, off ? "alt-tab quick pause off" : "alt-tab quick pause on again");
}

void Core_SetFpsCap(int cap) { // 0: the ini's again; none there: the value before the first call (boot's 1/300, 0x1D93B0)
    static float before = *Rva<float>(rva::FrametimeMin);
    g_cfg.fpsCap = cap ? cap : IniInt(L"fps_cap", 0);
    if (g_cfg.fpsCap <= 0) *Rva<float>(rva::FrametimeMin) = before;
}

int IniInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"coop", key, def, g_ini); }
void IniStr(const wchar_t* key, wchar_t* out, int n) { GetPrivateProfileStringW(L"coop", key, L"", out, n, g_ini); }

double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_qpcMs; // absolute QPC ms: logs of two instances merge with `sort`
}

const wchar_t* ModDir() { return g_dir; }

void TitleStatus(const char* status) {
    AcquireSRWLockExclusive(&g_titleLock);
    snprintf(g_titleStatus, sizeof g_titleStatus, "%s", status);
    ReleaseSRWLockExclusive(&g_titleLock);
    if (g_titleEvent) SetEvent(g_titleEvent); // before the window exists: shown when WindowThread starts
}

void Log(const char* fmt, ...) {
    static SRWLOCK lock = SRWLOCK_INIT;
    char buf[1024];
    int len = snprintf(buf, sizeof buf, "[%s] %.0f ", g_tag, NowMs());
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf + len, sizeof buf - len, fmt, a);
    va_end(a);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsRfgCoop.log", g_dir);
    AcquireSRWLockExclusive(&lock);
    if (FILE* f = _wfopen(path, L"a")) { fprintf(f, "%s\n", buf); fclose(f); }
    ReleaseSRWLockExclusive(&lock);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcMs = f.QuadPart / 1000.0;
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    *(wcsrchr(g_dir, L'\\') + 1) = 0;
    swprintf(g_ini, MAX_PATH, L"%lsRfgCoop.ini", g_dir);
    wchar_t tag[16];
    IniStr(L"tag", tag, 16);
    if (*tag) WideCharToMultiByte(CP_ACP, 0, tag, -1, g_tag, sizeof g_tag, nullptr, nullptr);
    wchar_t path[MAX_PATH], prev[MAX_PATH]; // keep the last run's log: a crash test is often followed by a relaunch
    swprintf(path, MAX_PATH, L"%lsRfgCoop.log", g_dir);
    swprintf(prev, MAX_PATH, L"%lsRfgCoop.prev.log", g_dir);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
    if (FILE* fl = _wfopen(path, L"w")) fclose(fl);
    if (!BuildMatches()) {
        Log("rfg.exe build not recognised (expected GOG Re-Mars-tered, PE stamp 5B9B713D); RfgCoop disabled");
        return TRUE; // still a working xinput1_3.dll
    }
    Log("build OK, folder %ls", g_dir);
    g_cfg.dev = IniInt(L"dev", 0);
    g_cfg.fpsCap = IniInt(L"fps_cap", 0);
    InstallDev(IniInt(L"save_tag", 0));
    if (!(g_mpNext = (MultiProcessFn)ChainRel32(site::MultiProcessCall, (void*)OnMultiProcess)))
        Log("multi_process call at %X not patched: no tick", (unsigned)site::MultiProcessCall);
    AddVectoredExceptionHandler(1, OnException);
    Puppet_Install();
    Net_Install();
    World_Install();
    Guest_Install();
    Destr_Install();
    Log("RfgCoop loaded (M0/M1), base %p, dev %d, fps_cap %d", (void*)g_base, g_cfg.dev, g_cfg.fpsCap);
    return TRUE;
}
