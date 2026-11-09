# RfgCoop notes: mod infrastructure, packaging, two-instance dev testing

Domain: hook points and the sim frame, packaging next to other proxy-DLL mods, running two game instances on one PC,
diagnostics, build. Distilled 2026-10-07 from `research/findings/workflows/coop_infra.md` (the investigator report) plus
the skeptic verdicts on claims C1-C8. Where they disagree, the verdicts win.
Build: GOG rfg.exe, 32-bit, PE stamp 0x5B9B713D, SizeOfImage 0x397B000, ASLR. The `<game copy B>` and `<GOG Galaxy folder>` exes are
byte-identical (same MD5/sha256). Their **sw_api.dll differs** (same size, different hash and date).
All addresses are **RVAs** (runtime = `GetModuleHandleW(nullptr) + RVA`; PDB/disassembly VA = RVA + 0x400000). File offsets:
`.text` = RVA - 0xC00, `.rdata` = RVA - 0xE3F000 + 0xE3E400, `.data` = RVA - 0x10F4000 + 0x10F3200.
The xref table columns are `target RVA, kind, site RVA, containing function` (the first column is the target).
Tags: **[V]** = someone read the code, data or logs. **[I]** = reasoned, not read. Nothing here was seen at runtime.

## TL;DR

- **Two instances on one PC should work, but nobody has run them side by side yet.** rfg.exe has no single-instance guard,
  sw_api moved to UDP 23023 when 23022 was held, and a failed Galaxy init is not fatal [V]. The only log case had the
  second instance boot next to a *hung* first one from the same folder. Probe P1 settles it.
- **Dev build = four small, base-relative writes:** quick-pause byte 0x4DDB5F `75`→`EB`; `s_allowBackgroundInput`
  (0x12EF120) = 0; a save-name patch in instance B (use the 2-byte `keen`→`kee2` version below, not the 1-byte `.sa2`
  one); optional `SetWindowPos`. **Never minimize an instance**: its sim thread blocks.
- **Packaging:** its own `xinput1_3.dll` proxy (ordinals 2/3 imported, not a KnownDLL, slot free). Do not touch 0x80F7C1
  or `State_info[GS_GAMEPLAY].process`, never set `Multiplayer`, and chain (never overwrite) the hooks other mods may
  already own.
- **Hook sites:** 0x1D24D9 (`multi_process` call, every SP sim frame), 0x3EE55C (wraps `gamestate_gp_simulate_frame`),
  0x3EDFEB / 0x3EE16F (pre/post Havok), 0x86FFF0 (endFrame prologue, render thread). The gameplay sites stop while paused.
- **Timing:** sim and render run in lockstep. dt is variable; at runtime it is capped at **1/300 s** and clamped at **0.1 s**
  (not 1/120 and 0.25). Network I/O, keepalives and timeouts go on our own thread with QPC. The engine's lag simulators
  are dead code: write a small shim.

## Facts

### F1. Sim frame order [V]
`rfg_do_frame` (0x1D2420; only caller `do_game_main_loop_frame` at 0x1D2642), sim thread:
```
game_check_for_gameplay_shutdown
os_foreground() → when unfocused only mouse_update_visibility + input_set_vibration_unwanted (0x1D2448..)
game_input_poll 0x3D9850                       (runs regardless of focus, 0x1D2499)
frametime_calculate(0, 0.0, 0.0) 0x1AD0F0       (0x1D24B1)
if (gameseq_get_state() != 4 && != 5) multi_process(game_booted_to_main_menu())   ← 0x1D24D9
game_save_process, gamelog_process
gameseq_process → State_info[top].process → gamestate_gp_process 0x3EE520 → gamestate_gp_simulate_frame 0x3EDB60
video_player_process_all, game_handshake_copy_gameseq(Sim_snapshot), mouse_flush
game_handshake_do_frame 0x3EE6E0 → keen::rfg::start_middle_of_frame 0x5A870 (waits for the render thread)
```
- `gamestate_gp_process` 0x3EE520: `if (!s_pauseGame) { if (s_speedScale != 1) Frametime *= s_speedScale;
  gamestate_gp_simulate_frame(); }`.
- `gamestate_gp_simulate_frame` 0x3EDB60, in order: `interface_process`, AI (`ai_system_do_frame` 0x3EDF01),
  `player_process_all` 0x3EDF99, `vehicle_process_all_presim` 0x3EDFE1, `object_mover_do_frame_pre_sim` 0x3EDFE6,
  **`multi_game_pre_sim` 0x3EDFEB**, **`havok_simulate` 0x3EE088**, **`multi_game_post_sim` 0x3EE16F**,
  `rfg_destruction_event_do_frame` 0x3EE1B1, `mission_system_do_frame` 0x3EE1F6, `script_do_frame` 0x3EE1FB,
  `activity_system_do_frame` 0x3EE4DF, and more (full list in the report, F1).
- Early-outs inside it: `jg` at 0x3EDE5B jumps to 0x3EE4DD when `Game_paused_count > 0`, skipping both 0x3EDFEB and
  0x3EE16F. If `activity_is_active(0xB)` it exits at 0x3EDC25 → 0x3EDF58 (tail-jmp `demolitions_master_process_player_death`).

### F2. Hook sites (bytes read from the GOG GALAXY exe) [V]
| RVA | Bytes | What | When it runs |
|---|---|---|---|
| **0x1D24D9** | `E8 42 27 43 00` | `call multi_process` (0x604C20, `void __cdecl(bool)`) | every sim frame in every state except GS_MULTI_INIT/GS_MULTI_SHUTDOWN (4/5); no Multiplayer check. Before `gameseq_process` |
| 0x1D2642 | `E8 D9 FD FF FF` | `call rfg_do_frame` | every sim frame (alternative) |
| **0x3EE55C** | `E8 FF F5 FF FF` | `call gamestate_gp_simulate_frame` | gameplay, skipped while `s_pauseGame` is set |
| **0x3EDFEB** | `E8 60 AC 18 00` | `call multi_game_pre_sim` (0x578C50) | gameplay; skipped when `Game_paused_count > 0` and on the activity-0xB exit |
| **0x3EE16F** | `E8 EC AA 18 00` | `call multi_game_post_sim` (0x578C60) | same as above; also a `jle` target from 0x3EE0C4 (harmless for a rel32 retarget) |
| **0x86FFF0** | `56 8B 74 24 08` | `keen::graphics::endFrame` prologue: `push esi; mov esi,[esp+8]`, position-independent, nothing branches into +1..+4 | render thread. Callers: 0x80F7C1 (jmp, Internal::render), 0x80E087 (GameFramework::endFrame) and 0x81075F (GameFramework::initialize, once, maybe before a swapchain exists) |
| 0x80F7C1 | `E9 2A 08 06 00` | tail jmp to endFrame | **another mod may own it: do not touch** |
| 0x213ADD0 | ptr | `State_info[GS_GAMEPLAY].process` | **another mod may own it: do not touch** |

E8 sites: hook with `ChainRel32`, which accepts any existing E8/E9 target and returns
it so the wrapper calls through. endFrame: trampoline = the 5 original bytes + `jmp 0x86FFF5`. An endFrame detour runs
**after** the draw of any mod that calls 0x86FFF0 directly.

### F3. Frame time [V static]
`frametime_calculate` 0x1AD0F0: if `Frametime_real < Frametime_min` it sleeps `trunc((min-real)*1000)` ms and measures again
(0x1AD205..0x1AD232). Then `Frametime = min(real, Frametime_max)` (only `Frametime`, not `Frametime_real`), then
`/= Frametime_divisor` (0x1AD49B). It returns early while `Frametime_paused` is set. All three callers (rfg_do_frame,
game_boot_thread, mbox_tight_loop) pass 0.0/0.0, so the fixed-step branch is unused.

**Runtime values:** `rfg_init_stage_1_prepare` calls `frametime_init` (= `frametime_reset` + `Frametime_inited = 1`), then at
**0x1D93B0** `frametime_set_cap(0.0033333 [0xEC6BE8], 0.1 [0xEB9610])`. That stores min = 1/300 s and max = 0.1 s. Nothing writes
min/max again: `frametime_enable_cap`/`disable_cap` have no callers. So: **cap ≈ 300 fps without vsync, and any hitch over
100 ms loses sim time against the wall clock.**

| Global | RVA | Notes |
|---|---|---|
| `Frametime` | 0x1118560 | sim dt of this frame |
| `Frametime_real` | 0x1118564 | unclamped wall dt |
| `Frametime_uncompressed` | 0x1118568 | dt before the divisor |
| `Frames_per_second` | 0x111856C | rolling average |
| `Frametime_divisor` | 0x1118570 | 1.0. `frametime_set_compression` 0x18AD10 has no callers; only mods change it |
| `Frametime_min` / `_max` | 0x1118574 / 0x1118578 | .data 1/120 and 0.25, **overwritten at boot to 1/300 and 0.1** |
| `s_speedScale` | 0x125BBD4 | 1.0, its only reference is the read at 0x3EE529 |
| `Framecount` | 0x18E7710 | ++ per frametime_calculate |
| `Timestamp_ticker` | 0x1118708 | game ms |
| `Frametime_paused` | 0x18E7730 | |

### F4. Lockstep threads; a minimized window blocks the sim [V code path, I runtime]
- `start_middle_of_frame` 0x5A870 always waits on app+0x648 (app = 0x1636A58, event at 0x16370A0) via
  `keen::Event::sleepUntilSignaled`, with no timeout. The only signaller is `keen::rfg::Application::render` (0x59970; at 0x599FE,
  0x59B1B, 0x59B35), which then waits on app+0x64C. Result: one sim frame per render frame.
- `GameFramework::Internal::render` 0x80F760 skips Application::render and sleeps 15 ms (0x80F7C9) if `renderingSuspended`
  (GameFrameworkSystem+0x301) is set or `beginFrame` (0x86D9D0) returns 0. beginFrame calls `GetClientRect` (IAT 0xE3F588;
  SetRect 100x100 only if that call fails) and then `resizeRenderSwapChain` 0x86A7F0, which returns false for width or height 0
  (0x86A7F6/0x86A802). A minimized window has a 0x0 client rect [I, Win32], so its sim thread stops.
- `IsIconic` is used only by the dead `os_handle_suspend`, so there is no special minimize handling.
- The engine's own `multi_process` runs **on the sim thread**.

### F5. Thread ownership [V threads, I ordering]
- Game objects, `World` (0x2B97490), `world::object_from_handle` (0x53BEE0) and the mempools belong to the sim thread and are
  not locked. The render and network threads must never dereference them.
- The render thread only sees `Render_snapshot` (0x125D828). An overlay reads **our own** struct, copied under a lock on the
  sim thread.
- [I] Write transforms/velocities before Havok (0x3EDFEB wrapper, or before the original at 0x3EE55C). Capture final
  state after the original at 0x3EE55C returns (0x3EE16F is post-physics but before missions, scripts and destruction events).

### F6. Loading [V calls, I durations]
`gs_load_process` is a per-frame state machine, and the 0x1D24D9 hook keeps firing during loads. Single calls
(`world::load_territory`) can still block the sim thread for seconds, so timeouts belong on the network thread.

### F7. Packaging: proxy slot [V]
- rfg.exe imports `XINPUT1_3.dll` by **ordinals 2 and 3 only** (IAT 0xE3F728 = `_imp__XInputGetState`). No game-folder DLL
  (sw_api, Galaxy, GalaxyPeer, GfeSDK, discord-rpc, binkw32) imports xinput. xinput1_3 is not in KnownDLLs.
- Taken by other proxy-DLL mods: `winmm.dll`, `dinput8.dll`. The GOG GALAXY folder has no xinput1_3.dll.
- KnownDLLs (cannot be proxied) include kernel32, advapi32, user32, ole32, OLEAUT32, SHELL32, SHLWAPI, WLDAP32, NORMALIZ,
  WS2_32. Fallback slot: `D3DCOMPILER_43.dll` (only D3DReflect, used at shader load). Avoid dbghelp, d3d11, CRYPT32, MSVCR100,
  and any shipped game-folder DLL. `version.dll` is not imported.
- On this PC `C:\Windows\SysWOW64\XInput1_3.dll` is 10.0.x, with `.bak` files for 1_1/1_2/1_3 and
  `System32\chxinputrouter.dll` present, which points to a third-party controller router [I]. The proxy forwards to whatever
  is installed by full system path.

### F8. Hooks other proxy-DLL mods may own [I]
- A `dinput8.dll` or `winmm.dll` mod beside rfg.exe may:
  - require the PE stamp and an E9 at 0x80F7C1 to 0x86FFF0;
  - swap the process slot only if it still equals 0x3EE520;
  - overwrite the XInputGetState IAT slot 0xE3F728;
  - chain the State_info exit slot and the gs_load_process exit calls 0x51305F/0x51308D/0x5130FA;
  - subclass the WndProc through `s_windowStates` (0x34BC540, +4 hWnd);
  - switch its features off while `Multiplayer` (0x2BEA588) is 1 (DotB content loaded into the campaign by a mod is
    skipped then).

### F9. No single-instance guard [V]
- No `CreateMutex*`/`OpenMutex*`/`FindWindow*`/`CreateFileMapping` imports in rfg.exe, sw_api.dll, Galaxy.dll, GalaxyPeer.dll
  or discord-rpc.dll.
- `keen::Mutex::create` 0x807060 = `InitializeCriticalSection` (import 0xE3F210), e.g. "GameLinkMutexRFG" → `g_gameLinkMutex`
  0x2BEA77C. It is in-process only.
- Every *named* kernel object (`keen::Event::create` 0x806FC0, `keen::Semaphore::create` 0x807130, `NativeFileDevice::create`
  0x80D280) is named `"%s@%x:%x"` (0xF3CD1C) from name, this and **GetCurrentProcessId**, so it is unique per process. All
  other CreateEventW/CreateSemaphoreW sites pass NULL. No code checks GetLastError for an existing object, and there is no
  "already running" string.
- No self-relaunch: `process_spawn*` has no callers.

### F10. Startup checks: what is fatal [V]
- `keen::GameFramework::initialize` 0x80FE30: if config byte `[edi+0xA4]` (`useSteam`) is 0, it skips all of this
  (0x80FEE1..0x80FEEF). Otherwise 0x80FF08 calls `keen::createSteamworksSystem` (0x24D0), which calls
  `keen::SteamworksSystem::create` (0x2460). That returns:
  - **1** if `SW_CCSys_Init` (import 0xE3F7EC) returns false: "Failed to initialize steamworks system..." (VA 0x12A4B20).
  - **2** if `SW_CCSys_DynamicInit` (0xE3F7E8) → `[obj+0x18]->vtbl+0x18(appId)` returns false: "You don't own this steam
    app %u." (VA 0x12A4AFC).
  - Either code → `LoadStringW` (0x8100B0) + `MessageBoxW` (0x8100C5) + **`_exit(1)`** (0x8100D3). Both checks pass on this
    GOG install.
- `galaxy::api::Init Failed!` appears in every recent session of `<GOG Galaxy folder>\...\logs\steamwrapper.log` (77 lines, e.g.
  :17904). Each one is followed by `cSteamWrapper::Init return! ... BackendType '3'` (:17928) with a **new random OnlineId
  per run**, and the game ran. GOG Galaxy does not need to be running.
- Port: steamwrapper.log :12572 "[UPNP] init, port requested 23022" (09:52:52), then :12709 "port requested 23023"
  (09:55:03), then :12851 back to 23022. networking.log :3505 "RakHost STARTED ... 127.0.0.1|23023". The 23022 holder (the 09:52
  session) stopped its stats loop at 09:54:49 and never logged Terminate or `THQNClient::Shutdown` (client_api.log :417-431).
  It was **hung**, not live. Both sessions came from the GOG GALAXY folder, and `<game copy B>` has no `logs` folder (never run).
  The port-stepping code itself has not been read.
- Other UDP binds in rfg.exe (`keen::network::bind` 0x8849C0) come only from `GameLinkLan::beginSession` (MP LAN) and SMTP.
  They are irrelevant to the SP campaign.

### F11. Shared and per-folder resources [V unless marked]
| Resource | Where | Shared? |
|---|---|---|
| Save container | `%LOCALAPPDATA%\GOG.com\Galaxy\Applications\51153410217180642\Storage\Shared\Files\autocloud\save\keen_savegame_0_0.sav` (31,479,964 bytes; 33 FileWrite lines in the GOG log) | **YES** for the GOG GALAXY install. For `<game copy B>` [I]: the path depends on the product ID, not the folder, but that copy has never run and its sw_api differs |
| Options | `%APPDATA%\kaiko\rfg\config.ini` (`game_options_load_ini_file_into_cache` 0x3EE770, `keen::getKnownWindowsFolder` 0x80A6B0, RoamingAppData) | YES. Report says it is written only from option-menu callbacks (report-verified, **not** skeptic-checked: C8 unchecked) |
| Game log | `<exe dir>\Red Faction Guerrilla Re-Mars-tered.log` (`CreateFileW` GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS at 0x80AE8D..0x80AEA5) | per folder. Two instances in **one** folder would collide, so use two folders |
| sw_api logs, `sw_game_config.ini`, `thqnocfg*` | `<exe dir>` | per folder |
| GPU preference | Windows per-exe-path setting | [I] the `<game copy B>` exe needs its own "High performance" entry |

### F12. Save file name [V static]
- `.rdata` 0xF44204 = `"%skeen_savegame_%u_%u.sav"` (`k` at 0xF44206, `n` at 0xF44209, `v` at 0xF4421C), formatted with
  `s_steamAutoCloudSaveDirectory` 0xF44080 `"autocloud/save/"`. Exactly six functions reference it, all in
  `SaveDataProviderWin32Steam`: `generateFileName` 0x8CD120, `writeSaveData` 0x8CE390, `updateReadSaveData`,
  `updateEraseSaveData`, `updateEraseSaveDataSet`, `getUnusedSaveDataId`.
- **Enumeration does not use it.** `enumerateSaveData` 0x8CDCC0 lists every remote-storage file (vtbl +0x48/+0x4C on
  [this+0x110]), and `getSaveDataInfoFromFileName` 0x8CD150 parses each name: it splits on `_` (0xEC2444), requires the tokens
  `"keen"` (0xF4422C) and `"savegame"` (0xF44220), and reads the last two with `readUint32FromDecimalString`. That parser stops
  at the first non-digit and still succeeds, so **the extension is never checked**. Size is then checked via vtbl +0x3C.
- `"keen"` at 0xF4422C and `"savegame"` at 0xF44220 are referenced only by `getSaveDataInfoFromFileName` (xrefs, checked during
  this distillation).

### F13. Focus loss and the quick pause [V]
- WM_KILLFOCUS/WM_SETFOCUS → `keen::generateInputEventForWin32Message` 0x87C840 (jump table 0x87CC04) → input event
  **0xE**/**0xF** → `keen::rfg::Application::processInputEvent` 0x58CF0 (0x58D09..0x58D39): 0xE → `audiolib_mute_sound(1)`,
  `Os_app_active` (0x1118591) = 0; 0xF → unmute, = 1. Both are skipped if `g_ignoreFocusChange` (0x1636840) is nonzero; it is
  only read (0x58D0D, 0x58D2A) and never written.
- Readers of `Os_app_active`: `os_foreground` 0x1AD960, `mouse_generic_update_visibility`, `mouse_generic_poll`,
  `mouse_update_visibility`, plus the dead `os_handle_suspend`/`win32_message_handler` (registrar `os_create_window` has no
  callers).
- Callers of `os_foreground`: `rfg_do_frame` (mouse visibility/vibration only), `control_info::read_controls` 0x2FE4F5 (mouse
  deltas), `gamestate_quick_pause_process` 0x3D91A0 (pops the state on refocus), `interface_process`.
- `interface_process` 0x4DD9D0, bytes at 0x4DDB4F (identical in both exes):
  `80 3D 88 A5 FE 02 00 75 1F E8 .. 84 C0 [75] 16 38 05 B4 71 C0 02 75 0E 6A 00 6A 00 6A 3E E8 ..`
  = `if (!Multiplayer && !os_foreground() && !Cursor_visible) gameseq_push_state(GS_QUICK_PAUSE = 62, 0, 0)` (call at 0x4DDB6F).
- **It is the only quick-pause push**: of 91 `gameseq_push_state` calls only this one has `push 0x3e`, 43 `gameseq_set_state`
  calls have none, and the tail-jmps (game_push_multi/dlc/wc_init) never pass 62. GS_QUICK_PAUSE registers with
  State_info+0x18 = 0 (`gamestate_quick_pause_init` 0x3E9000), and `gameseq_do_state` only recurses into lower states when +0x18
  == 1, so **the gameplay sim really freezes** underneath it.
- **Patch: 0x4DDB5F `75`→`EB`** (jmp +0x16 to 0x4DDB77). It removes only that push. Muting, `Os_app_active` and mouse-delta
  gating keep working. (`g_ignoreFocusChange = 1` would also stop the pause, but it loses muting and mouse gating.)
- **Other local pauses that freeze the sim the same way** and are *not* fixed by this patch: Esc/start pause (state 0xF
  pushed at 0x4DDB00..0x4DDB06) and `game_pause` 0x3C1620 callers: `fullscreen_map_enter` 0x476B4F,
  `video_cutscene_play_enter` 0x307006, `interface_open_mbox` 0x4A85FD, `ui_add_msgbox` 0x4F6E8D, `save_game_callback`
  0x5011F4, options menus.

### F14. Input to an unfocused instance [V code, I runtime]
- `keen::input::createPlatformEvents` 0x87E500: focus computed into `s_windowStates[i]+0x14` (from
  GetFocus/GetParent/GetForegroundWindow), then at 0x87E601 `cmp [s_allowBackgroundInput],0 / jne / cmp hasFocus,0 / je`.
  The gated block holds all pad event generation (XInput, DInput, Steam: `fillInGenericPadInput`, `generateGenericPadEvents`
  0x87E673..0x87E787, and they have no other callers).
- **`s_allowBackgroundInput` (0x12EF120, 1 byte) = 01 in .data** in both exes, with one reference (0x87E601). Once the quick
  pause is gone, an unfocused instance follows the pad. **Fix: write 0** (writable .data).
- Keyboard: `addKeyEvent` 0x87B0A0 is called only from `generateInputEventForWin32Message`. There is no
  GetAsyncKeyState/GetKeyboardState/RegisterRawInputDevices, so the keyboard is focus-only [I, Win32 semantics]. DirectInput8 only
  enumerates game controllers (class 4, 0x87E579).
- Mouse: `read_controls` skips `mouse_get_deltas` when not foreground (0x2FE4F5). `keen::centerMouseCursor` 0x875100 and the
  clip in `setMouseCursorVisible` 0x874FF0 act only on focused windows.
- **Exception:** `keen::win32MsgProc` WM_SIZE (msg 5) at 0x87CE1C calls `ClipCursor(client rect)` with **no focus check** when
  the cursor is hidden and the clip flag is set. A resize of an unfocused instance (e.g. our `SetWindowPos`) can grab the global
  cursor clip.

### F15. Window [V report, partly unchecked]
- User config.ini: `window_mode=0` = `keen::graphics::WindowMode_Windowed` (0 Windowed, 1 Borderless, 2 Fullscreen),
  2560x1600 (report, C8 unchecked).
- beginFrame reads `GetClientRect` every frame and resizes the swapchain (confirmed under C3). So `SetWindowPos` can lay out the
  windows [I that the HUD and internal resolution follow: probe P9].
- HWND: `s_windowStates` 0x34BC540, stride 0x38, 4 entries: +0 pOldMsgProc, +4 hWnd, +0x14 hasFocus.

### F16. Diagnostics [V]
- Built-in lag/loss simulators are **dead**: `net_lag_init` 0x1AC6F0, `net_lag_recvfrom` 0x1AC710, `net_lag_get_random_lag`
  0x1AC830, `net_lag_should_be_lost` 0x1AC940 and `keen::network::ErrorSimulation::*` (createSocket 0x887B80, sendTo 0x882AF0,
  update 0x887E00, setSimulationSettings 0x880AC0, ...) have no callers and 0 absolute pointers in the file. The
  ErrorSimulationSocket has no vtable. `net_lagloss_dcf` 0x1BCA40 is called only from `unused_dcf_mp_*`, which are themselves
  unreferenced. `Net_lag_inited` 0x18E76FC is read only inside that cluster.
- The engine debug print (`keen::debug::nativePrintArgs` 0x8071D0) works in retail and goes to `<exe dir>\Red Faction Guerrilla
  Re-Mars-tered.log`. sw_api logs: `<exe dir>\logs\{steamwrapper,networking,matchmaking,client_api,user,Callbacks}.log`.

### F17. Build [V]
llvm-mingw: clang 23.1.2, i686-w64-windows-gnu, ships libws2_32.a, libiphlpapi.a, libmswsock.a and libc++
with threads (`std::thread`/`std::mutex` work). A proxy linked `-shared -static` imports only system DLLs + UCRT.
rfg.exe is LARGE_ADDRESS_AWARE (Characteristics 0x122), so each instance can use 4 GB.

## Approach for co-op

**Packaging**
- `RfgCoop/` with `src/`, `build.cmd`, `xinput1_3.def` → `build\xinput1_3.dll` next to rfg.exe. Exports: XInputGetState @2,
  XInputSetState @3, @4-@8 named, 100-103 NONAME, each forwarding to the system DLL (full SysWOW64 path, loaded lazily on the
  first call, never in DllMain), plus `RfgCoop_Api`. Check with `llvm-readobj --coff-exports`.
- DllMain: check the build (stamp, size, bytes at every patch site), install the ChainRel32 hooks + endFrame detour + VEH
  logger, read `RfgCoop.ini`. No threads and no WSAStartup there: start them lazily on the first tick.
- Rules: never touch 0x80F7C1 or the process slot; **never set `Multiplayer`** (it can silently switch off other mods and DotB
  content loaded into the campaign by a mod, and flips 1000+ SP branches); chain the IAT 0xE3F728, the exit calls and the
  exit slot if needed; every wrapper calls what was there before. The xinput IAT may already be another mod's: with our
  proxy, that mod ends up wrapping our export, which is fine as long as we forward correctly.

**Threads and tick**
- Network thread: sockets (ENet per `transport.md`), keepalive/timeouts, lag/loss shim (delay queue + drop/dup/reorder), fixed
  20-30 Hz send by QPC. It never touches game memory. Two SRWLOCK-guarded buffers connect it to the sim thread.
- Sim thread: the 0x1D24D9 wrapper (call the original, drain inbound, run the session state machine, publish overlay stats).
  It is the only hook that runs during menus, pauses and loads. The 0x3EE55C wrapper applies remote input/events before the
  original and captures state after it. 0x3EDFEB/0x3EE16F are for pre/post-Havok puppet work. **The gameplay hooks fall silent
  during any pause**, so session liveness must come from 0x1D24D9 and the network thread.
- Render thread: the endFrame detour draws the overlay from our snapshot only.
- Use QPC timestamps for snapshots and interpolation, never summed `Frametime`. Host-controlled or locked during a session:
  `Frametime_divisor`, `s_speedScale`, the game clock.

**Session robustness**
- Alt-tab: apply 0x4DDB5F while a session is active and restore it after. On its own it is not enough: Esc pause, map,
  cutscenes, message boxes and save prompts also freeze the local sim (F13). A frozen *host* freezes the shared world, so
  these need a policy (sync the pause, or neutralise it) chosen by the replication/world domains.
- Minimize stalls a peer (F4): treat a silent peer as "lagging" up to a generous timeout and keep the keepalives on the
  network thread. Tell players not to minimize. Exclusive fullscreen alt-tab probably minimizes (unverified).
- Version handshake: protocol + build id; rfg.exe stamp/size; size+CRC32 of dinput8/winmm/xinput1_3 in the exe folder;
  a hash of the `data\*.vpp_pc` name+size list. Refuse on mismatch (DotB content must match on both sides).

**Dev setup on one PC**
1. Back up `keen_savegame_0_0.sav` (F11) and `config.ini`.
2. A = `<GOG Galaxy folder>\...` (host), B = `<game copy B>\...` (client), each with the same mod DLLs (an install step the user
   approves) and its own `RfgCoop.ini` (`role`, `bind`/`connect=127.0.0.1:port`, `window=x,y,w,h`, `save_tag`,
   `lag_ms/jitter_ms/loss_pct`).
3. Both instances: 0x4DDB5F patch, `s_allowBackgroundInput = 0`, `SetWindowPos` (watch the WM_SIZE ClipCursor exception in F14),
   `SetWindowTextW` " [HOST]"/" [CLIENT]".
4. **B only: save isolation, applied in DllMain before any save read.** Recommended: patch the two `n`s, 0xF44209 and
   0xF4422F, to `2`, which turns the prefix `keen` into `kee2` in both the format string and the parser token. B then
   writes `kee2_savegame_*.sav` and only enumerates `kee2_*`, and A ignores those files (static [V], runtime [I], probe P5).
   Copy `.sav` → `kee2_savegame_0_0.sav` once to start B from the same progress. `.rdata` is read-only, so use VirtualProtect.
5. Launch both exes directly with their own working directory. Galaxy is not needed. Never minimize. Loopback 127.0.0.1
   avoids the firewall prompt; for LAN tests add rules for both exe paths. Mark both exe paths "High performance" GPU.
6. Per-folder `RfgCoop.log` lines prefixed with role + QPC ms, so the two logs can be merged. The overlay shows state, RTT,
   jitter, loss, kbps, entity count and `Frames_per_second`.
7. Light load: cap both instances (vsync or `frametime_set_cap`, e.g. 1/60) during tests; this PC crashed once under load.

**Build:** `i686-w64-mingw32-clang++ -O2 -std=c++20 -shared -static src\*.cpp <imgui objs> src\xinput1_3.def -ld3dcompiler
-lws2_32 -lwinmm -o build\xinput1_3.dll`.

## Contested / refuted (do not rely on these)

- **"Sim timestep clamped to [1/120 s, 0.25 s]; sim at most 120 Hz"** (report F3, Verdict, Implications 3): wrong. Those are the
  .data/`frametime_reset` defaults, which are overwritten at boot by `frametime_set_cap(1/300, 0.1)` at 0x1D93B0.
- **"Only SW_CCSys_Init returning false is fatal"** (C1): wrong. The ownership check (status 2) is also fatal. Both pass on GOG.
- **"Two instances proven to run"**: the logs only show a second instance booting next to a **hung** first instance from the
  **same** folder. Two live instances, and anything at all from `<game copy B>`, are unproven. The port-stepping logic was inferred
  from the logs, not read.
- **"One byte `v`→`2` redirects every save read, write, erase and enumeration"** (C5, report F14): enumeration is *not*
  redirected. `getSaveDataInfoFromFileName` ignores the extension, so each instance still sees the other's file as slot (0,0),
  slots can be listed twice, and a size check can pass under one name while the read uses another. Use the `keen`→`kee2`
  patch (Approach step 4) instead.
- **"Both installs share the save"** for `<game copy B>`: inferred only (never run, different sw_api.dll).
- **"0x1D24D9 runs every frame in every state"**: not in GS_MULTI_INIT/SHUTDOWN (4/5). Fine for SP.
- **"0x3EDFEB/0x3EE16F run every gameplay frame"**: skipped when `Game_paused_count > 0` and on the activity-0xB exit.
- **"Avoiding 0x80F7C1 and the process slot is enough to avoid conflicts"**: also never set `Multiplayer`, and chain the
  XInput IAT, exit calls and exit slot.
- **"Cursor clip is fully focus-gated"**: no, WM_SIZE clips without a focus check (F14).
- **"An alt-tab freezes a peer"**: only true when the frozen instance is the host/authority or the netcode waits for it.
  And the quick-pause patch alone does not cover the other pauses (F13).
- **"Frametime_divisor / s_speedScale vary in vanilla"**: they don't. Only mods write them.
- **config.ini claims and "the user is windowed"** (C8): never checked by a skeptic. Treat as likely, not proven.
- **DLL init order winmm → xinput1_3 → dinput8** (report F9): inferred only. It doesn't matter as long as 0x80F7C1 stays
  untouched.

## Unknowns and the probes that settle them
Each probe is a few lines in a test build, logged to RfgCoop.log with QPC ms.

| # | Unknown | Probe |
|---|---|---|
| P1 | Do two *live* instances run, one from each install? Does `<game copy B>` (different sw_api) pass both startup checks and use the same GOG storage? | Start A, then B. Expect both at the main menu, ports 23022/23023 in each `logs\networking.log`, no Steamworks MessageBox, and B's steamwrapper.log listing `autocloud/save/keen_savegame_0_0.sav` |
| P1b | Does `SW_CCSys_Init` return true with no internet (THQNO timeout / "Pure nodrm backend!")? If not, offline LAN co-op exits at boot | Boot with the network disconnected |
| P2 | Does the unfocused instance simulate with the patch? | Log `Framecount` (0x18E7710) and `gameseq_get_state()` at 1 Hz from 0x1D24D9 on both, with and without 0x4DDB5F. Expect state 62 on top without the patch |
| P3 | Does minimize stall the sim? Does exclusive-fullscreen alt-tab minimize? | Minimize B: `Framecount` stops while its network thread still logs, and resumes on restore |
| P4 | Pad crosstalk | One pad, focus A. With `s_allowBackgroundInput` = 1, B's player moves too; with 0 it doesn't |
| P5 | Does sw_api GOG storage accept a `kee2_` (or `.sa2`) name? Is the enumeration clean? | In B with the patch, save manually. Expect `kee2_savegame_0_0.sav` to appear, the `keen_` mtime to stay unchanged, no "FileWrite exception", and each instance to list exactly one slot |
| P6 | Loopback baseline | 30 Hz UDP echo between the instances: RTT p50/p99 and the jitter of the 0x1D24D9 interval |
| P7 | Longest sim-thread stall | Maximum gap between 0x1D24D9 calls across a territory load, a save load and death/respawn. Sets the session timeout |
| P8 | Which menu states freeze the world | Dump `State_info[s]` (0x213ADA0 + s*0x24) +0x18 / +0x19 for s = 15, 16, 37, 38-46, 50, 62 |
| P9 | Does `SetWindowPos(1280x800)` give a correct picture and HUD? Does it trigger the WM_SIZE ClipCursor grab? | Resize at the first endFrame; read `rl_screen_info` width; check whether the cursor gets trapped |
| P10 | Does the <game copy B> exe use the NVIDIA GPU? | Both game logs should name "NVIDIA dGPU" |
| P11 | DLL init order | Each DllMain logs QPC at entry |
| P12 | Coexistence | With other proxy-DLL mods installed: their hooks still work and the RfgCoop overlay draws. Also check the 0x81075F startup call of the endFrame detour is harmless |
