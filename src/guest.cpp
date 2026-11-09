// Guest rules and fences (M5 "Join the host's world"). Off until session.cpp calls Guest_Arm (this game linked as a
// guest), then on until the process exits: nothing is ever restored. RESEARCH.md §3.3, §3.9, §3.13, §4.3, §4.7, §4.8,
// §8 Q12-Q15; notes/gaps.md patch list GF-*. Every site, its original bytes, the patch and the ABI check:
// research/notes/guest_impl.md. Each site is checked byte for byte first; nothing is written unless all of them match.
//   Saves    the 0x811278 save guard (world.cpp, World_ArmSaveGuard) + world::save_and_continue 0x51D040 -> logged no-op
//   Missions 0x39D69C store -> a stub that only records the value (GF-MIS); mission_activate 0x38E560 / 0x39D960 ->
//            logged no-op (GF-ACT); 0x3EE1FB script_do_frame call NOPed (GF-SCR); 0x3EDF23 gameplay_do_frame chained,
//            Activity_node_start_on_load = -1 first (GF-POP, M5 part)
//   Loads    world::set_pending_territory_load 0x51C320 detour: the session's own load (Guest_AllowLoad) passes, a
//            warp-to-object video's load ("", new_game 0: the in-RAM world, the last host blob) passes, anything else
//            is refused and asks the host for a re-join (GF-LOAD); so is the zone-warp video load (new_game 1, a fresh
//            territory: the main menu's New Game), whose refusal returns to the main menu. Two refusals the game does
//            not back out of are backed out here: an Esc -> Load verify armed mid-way (pop GS 0x37) and the DLC
//            video's load (back to the main menu)
//   UI       fast travel 0x454469, Esc Load 0x49DF50 / 0x5076C7 / 0x4B67A6, Esc Restart 0x4AF634 + 0x4AF641, Esc Cancel
//            mission 0x4AF62B: refused with a HUD line before their own side effects; death Continue 0x492FF0 -> re-join
//            request (GF-UI, rule Q15)
//   Q14      the "press to start" callbacks of mission, activity and upgrade nodes (7 stores) refuse the press; the
//            activity_base_modal / _free::activity_start entries 0x3ACA10 / 0x3ACA80 refuse every type but Convoy (13)
//   Q13      Rules_ApplyHostLoadout: the host's upgrade table, weapon locker list and salvage; the guest's own salvage
//            gains go to the host (Session_SendSalvage); Rules_AddHostSalvage adds them on the host
//   Q17      the guest's picked hammer and backpack (picker.cpp, Guest_SetPick): after every load on the first frame on
//            foot (ApplyPick), and its backpack levels again after every LOADOUT copy (PickTable)
//   Q18      the guest's own weapons (slots 0..2, ammo, held slot): recorded every frame on foot and alive in the host's
//            world, put back after every re-join right after ApplyPick (RestoreLoadout); each session's first join
//            (F8 / autoconnect Start, not a redial) keeps the host's
#include "features.h"
#include "game.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" IMAGE_DOS_HEADER __ImageBase; // this DLL: the session's own loads come from here
extern "C" {
int gc_guestMisRead; // GF-MIS stub: the Mission_checkpoint_restore_index value mission_system_load read (not stored)
}

namespace {

namespace site {
// fences (bytes, ABI and callers: notes/guest_impl.md)
constexpr uintptr_t SaveAndContinue = 0x0051D040; // void __thiscall world::save_and_continue(info*, bool, bool), ret 0xC
constexpr uintptr_t MisStore        = 0x0039D69C; // mission_system_load: A3 <Mission_checkpoint_restore_index>
constexpr uintptr_t MisRestoreIndex = 0x01250510;
constexpr uintptr_t MisActByHandle  = 0x0038E560; // void __cdecl mission_activate(mission_handle, bool), caller cleans
constexpr uintptr_t MisActByName    = 0x0039D960; // void __cdecl mission_activate(char*, bool), caller cleans
constexpr uintptr_t ScriptCall      = 0x003EE1FB; // gamestate_gp_simulate_frame: E8 -> script_do_frame 0x73C060
constexpr uintptr_t ScriptDoFrame   = 0x0073C060;
constexpr uintptr_t GameplayCall    = 0x003EDF23; // gamestate_gp_simulate_frame: E8 -> gameplay_do_frame (only caller)
constexpr uintptr_t GameplayDoFrame = 0x003B9770; //   void __cdecl()
constexpr uintptr_t NodeStartOnLoad = 0x01250244; // int Activity_node_start_on_load (consumed at 0x3ACB17)
constexpr uintptr_t SetPending      = 0x0051C320; // bool __thiscall world::set_pending_territory_load(char*, bool, bool,
                                                  //   bool, int dlc, bool), ret 0x18; prologue 56 8B F1 E8 rel32
constexpr uintptr_t KillMsgbox      = 0x004EF5C0; //   its first call: hud_ui_save_load_kill_msgbox
// video_cutscene_exit_state after its warp-to-object call (0x306EFC-0x306F15: "", new_game 0, the in-RAM world). Its
// zone-warp call (0x306F2F-0x306F4D: Video_cutscene_warp_to_zone, new_game 1, returns to 0x306F52) is refused like any
// other load. Either false -> gameseq_set_state(GS_MAINMENU) (0x306F26 / 0x306F5A)
constexpr uintptr_t VideoExitA      = 0x00306F1A;
// Refused loads with no fallback of their own (GF-LOAD backs them out as the game's siblings do):
constexpr uintptr_t VerifyLoadRet   = 0x003F0359; // verify_gamesave_process (GS 0x37) after its set_pending call 0x3F0354
constexpr uintptr_t DlcLoadRet      = 0x003BAF7A; // dlc_load_level after its set_pending call 0x3BAF75 (result ignored)
constexpr uintptr_t SetState        = 0x003D8730; // void __cdecl gameseq_set_state(int state, bool): push 1; push 0 at 0x306F56
constexpr uintptr_t ModalStart      = 0x003ACA10; // bool __thiscall activity_base_modal::activity_start(), plain ret
constexpr uintptr_t FreeStart       = 0x003ACA80; // bool __thiscall activity_base_free::activity_start(), plain ret
constexpr uintptr_t FastTravelCall  = 0x00454469; // fsm_warp_to_safehouse_cb: E8 -> safehouse_get_start_pos_orient;
constexpr uintptr_t SafehousePos    = 0x0066B4A0; //   bool __cdecl(vector*, matrix*, object*), false -> 0x454493 (no warp)
constexpr uintptr_t VerifyCalls[3]  = {0x0049DF50, 0x005076C7, 0x004B67A6}; // save/load UI: E8 -> verify_gamesave_start
constexpr uintptr_t VerifyStart     = 0x003E76A0; //   void __cdecl(game_save_info*, bool): Pending_save, push GS 0x37
constexpr uintptr_t RestartVdirCall = 0x004AF634; // ingame_options_handshake Restart: E8 -> vdir_restore(int, bool)
constexpr uintptr_t VdirRestore     = 0x001CB0F0;
constexpr uintptr_t RestartJmp      = 0x004AF641; //   then E9 -> world::set_pending_reload (bool __thiscall(), ecx = World)
constexpr uintptr_t SetPendingReload = 0x0051C3E0;
constexpr uintptr_t CancelJmp       = 0x004AF62B; // ingame_options_handshake Cancel mission: E9 -> mission_cancel
constexpr uintptr_t MissionCancel   = 0x0038E670; //   void __cdecl()
constexpr uintptr_t DeathContinue   = 0x00492FF0; // void __cdecl player_death_options_continue_cb(): 80 3D <MP> 00 75 46
constexpr uintptr_t PopState        = 0x003D8890; // void __cdecl gameseq_pop_state()
constexpr uintptr_t HudMessageAdd   = 0x00455D30; // hud_message_handle __cdecl(char*, float, void*, void*, int*)
constexpr uintptr_t SaveUiA = 0x0049DFCB, SaveUiB = 0x00507767, SaveUiC = 0x00507EFD; // save UI save_and_continue returns
// rules (Q13)
constexpr uintptr_t MetaData        = 0x2978;     // player_meta_data at Player+ (player_get_meta_data 0x610550): +0x4
                                                  // salvage, +0x18 upgrade_item[128] (8 bytes, byte 0 = level)
constexpr uintptr_t AddSalvage      = 0x00611700; // void __thiscall player_meta_data::add_salvage(int, bool tip), ret 8
constexpr uintptr_t AddUnlocked     = 0x00611630; // void __thiscall player_meta_data::add_unlocked_weapons_to_weapons_cabinet()
constexpr uintptr_t WeaponList      = 0x01260ED0; // farray<ui_weapon_list_item,64>: +0x4 data, +0x8 max, +0xC count
constexpr uintptr_t AddUnique       = 0x004761A0; // int __thiscall base_array<ui_weapon_list_item>::add_unique(item*), ret 4
constexpr uintptr_t WeaponByUid     = 0x007D9AA0; // weapon_info* __cdecl weapons_get_weapon_info_from_weapon_unique_id(int)
constexpr uintptr_t ArmorProp       = 0x006B2F60; // void __cdecl player_create_armor_prop(int level, player*)
constexpr uintptr_t UpgradeData     = 0x01E395F8; // upgrade_definition_data[0x3E], 0x228 each, +5 level count
// Q17 pick (notes/picker_impl.md; the calls puppet.cpp's GiveWeapon uses)
constexpr uintptr_t InVehicle       = 0x00653870; // vehicle* __cdecl human_in_vehicle(human*)
constexpr uintptr_t InvItem         = 0x00607820; // inventory_item* __cdecl human_get_inventory_item(human*, int slot): +8 info
constexpr uintptr_t DeleteItem      = 0x006C76A0; // void __cdecl human_delete_item_from_inventory(human*, item*, bool, bool)
constexpr uintptr_t AddItem         = 0x006B4E20; // inventory_item* __cdecl human_add_item_to_inventory(human*, inv_item_info*,
                                                  //   int count, int ammo, int slot, bool, bool, bool)
constexpr uintptr_t EquipItem       = 0x006C2770; // bool __cdecl player_equip_item(player*, inventory_item*, item**, bool)
constexpr uintptr_t BackpackDropped = 0x005DF1C0; // void __thiscall multi_object_backpack::event_player_dropped(bool), ret 4
constexpr uintptr_t LastBackpack    = 0x01293490; // int s_lastBackpackIndex (player::do_frame 0x6D56B2 / 0x6D56D7)
// Q18 loadout (guest_impl.md "Rule Q18")
constexpr uintptr_t InvInfo         = 0x00612240; // inv_item_info* __cdecl inv_item_get_info(char* name): NULL unknown
constexpr uintptr_t ObjFromHandle   = 0x0053BEE0; // object* __thiscall world::object_from_handle(unsigned), ret 4; -1 -> NULL
constexpr uintptr_t SetMagazine     = 0x001FB530; // void __thiscall weapon::set_rounds_in_magazine(int), ret 4: clamps 0..mag size
constexpr uintptr_t SetReserve      = 0x00369C90; // void __thiscall weapon::set_rounds_in_reserve(int), ret 4: clamps 0..max - mag
constexpr uintptr_t IsDead          = 0x001DC0F0; // bool __cdecl human_is_dead(human*): vtable +0x10, NULL -> true
// E10 log
constexpr uintptr_t MissionActive   = 0x0125050C; // int Mission_active_index
constexpr uintptr_t MissionMode     = 0x0334146E; // bool Mission_mode
constexpr uintptr_t WCurState       = 0x2578;     // World+: cur_world_state
constexpr uintptr_t WFlags          = 0x265C;     // World+: bit 7 = warp pending (save_and_warp 0x51D13E)
constexpr uintptr_t WFlags2         = 0x265D;     // World+: bit 2 = load from memory pending (process_save_load 0x3C0144)
} // namespace site

constexpr uintptr_t kImageSize = 0x397B000; // core.cpp's build check
constexpr int kGsGameplay = 1, kGsLoad = 2; // GS_LOAD: set_pending_territory_load's gameseq_set_state(2) (0x51C3C7)
constexpr int kGsVideo = 0x31;              // video_cutscene_enter_state 0x306E78 (push) / 0x306E83 (set)
constexpr int kConvoy = 13;                 // ACTIVITY_NAMES 0x12502A0 [13] "Convoy": rule Q14's one exception
constexpr int kUpgrades = 0x3E, kArmorUpgrade = 0x1A; // upgrade_get_data_by_index bound; purchase_upgrade armor 0x6C3EAF
constexpr int kNotRead = 0x7FFFFFFF;
constexpr ULONGLONG kAllowMs = 10000;       // a Guest_AllowLoad is good for the next session load within 10 s
constexpr const char* kActs = "Only the host can start activities";

// The "press to start" callbacks (bool __cdecl(player*, bool go): go 0 = may the prompt show, 1 = pressed; the use
// press at 0x2F717A / 0x2F72E8 ignores the result). Each is stored as `C7 4x 14 imm32` by its player_is_near_* check.
struct Use { uintptr_t store, fn; const char *what, *hud; };
constexpr Use kUse[] = {
    {0x0065B85D, 0x00633F40, "mission start", "Only the host can start missions"},     // player_start_mission
    {0x0065B9B8, 0x00633FE0, "upgrade shop", "Only the host can buy upgrades"},         // player_start_upgrades
    {0x0065BA83, 0x006340A0, "guerrilla raid start", kActs},                            // player_start_offensive_raid
    {0x0065BED7, 0x006340D0, "demolitions master start", kActs},                        // player_start_demolitions_master
    {0x0065C06F, 0x00634140, "riding shotgun start", kActs},                            // player_start_riding_shotgun
    {0x00683CCF, 0x006341B0, "area defense start", kActs},                              // player_start_area_defense
    {0x0065C1E7, 0x00634220, "house arrest start", kActs},                              // player_start_house_arrest
};

bool g_armed, g_failed, g_allowLoad, g_loaded, g_adopt, g_loadoutIn, g_upOk, g_arsenalOk;
ULONGLONG g_allowAt;
uintptr_t g_loadNext, g_actNext[2]; // trampolines (original prologue + jmp back)
char g_hudPending[128];
int g_salvage;                                  // the shared wallet as this guest last knew it
uint8_t g_up[0x400], g_arsenal[0x200];          // last LOADOUT, validated; applied on the next gameplay frame
int g_arsenalN, g_loSalvage;
int g_nSave, g_nMis, g_nLoad, g_nVideo, g_nStart, g_nUse, g_nUi;
uint8_t* g_pickHammer;                          // Q17: inv_item_info*, NULL = no pick (autoconnect, or never confirmed)
int g_pickPack = -1;                            // Q17: backpack upgrade (0 jetpack, 0x35..0x3D, 0x3E none), -1 = no pick
bool g_pickDue;                                 // Q17: a load happened, apply on the next frame on foot
// Q18: what player::save_inventory 0x634740 keeps per item (slot, item name, count, magazine + reserve), for the
// weapon slots 0..2 (3 is the Q17 hammer's), and the held slot. Names, not inv_item_info*, so a load cannot stale them.
struct LoSlot { char name[48]; int count, mag, res; }; // name "" = empty slot; mag / res -1 = no weapon object
struct Loadout { LoSlot s[3]; int held; bool have; }; // held: slot of the desired item (+0xA30), -1 none
Loadout g_lo;                                   // the last good frame (on foot, alive) in the host's world
bool g_loOn, g_loDue;                           // recording (from a session's first join on); restore on the next frame on foot
bool g_loStart;                                 // the session's own load passed (OnSetPending): its end turns recording on

uint8_t* LocalPlayer() { return *Rva<uint8_t*>(rva::Player); }

// Fence hit: the first 5 and every 100th go to the log, with the caller.
__attribute__((format(printf, 3, 4))) void Hit(int& n, void* from, const char* fmt, ...) {
    if (++n > 5 && n % 100) return;
    char t[160];
    va_list a;
    va_start(a, fmt);
    vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    uintptr_t x = (uintptr_t)from;
    bool in = x - g_base < kImageSize;
    Log("guest: %s (#%d, from %s%X)", t, n, in ? "rfg+" : "", (unsigned)(in ? x - g_base : x));
}

// HUD line now in gameplay, else on the next gameplay frame (newest wins). The same line within 5 s shows once: some
// refusals repeat every frame.
void Hud(const char* t) {
    static char last[128];
    static ULONGLONG lastAt;
    ULONGLONG now = GetTickCount64();
    if (!strcmp(t, last) && now - lastAt < 5000) return;
    snprintf(last, sizeof last, "%s", t), lastAt = now;
    Log("guest: HUD \"%s\"", t);
    if (GameState() == kGsGameplay && LocalPlayer()) {
        *g_hudPending = 0;
        Fn<int __cdecl(char*, float, void*, void*, int*)>(site::HudMessageAdd)((char*)t, 4.0f, nullptr, nullptr, nullptr);
    } else
        snprintf(g_hudPending, sizeof g_hudPending, "%s", t);
}

void Rejoin(const char* why) { // the one debounce is Session_RejoinRequest's (it logs whether the request went out)
    Log("guest: re-join wanted: %s", why);
    Session_RejoinRequest(why);
}

// ---- fences (installed only by Guest_Arm, so each hook below runs only on an armed guest) ---------------------------
// GF-SAVE: the entry jumps here, the original never runs (no in-RAM save: death 0x3EDCF0, upgrade-shop exit, save UI).
// w = World. save_and_warp 0x51D13E / save_and_continue_warp 0x51D1CE set World+0x265C bit 7 (warp pending) right
// before they reach this (via save_and_reload 0x51D106 / directly 0x51D236); only the skipped save would clear it
// (save_state_finish 0x53DD19), and no load does (0x51C394 keeps it). Left set, camera_shake_process returns at once
// (0x2CCD44) until exit. Its other readers, save_state_start 0x53C718 / save_state_finish 0x53DCC1, never run here.
void __thiscall OnSaveAndContinue(uint8_t* w, void*, int, int) {
    Field<uint8_t>(w, site::WFlags) &= 0x7F;
    void* from = __builtin_return_address(0);
    Hit(g_nSave, from, "world::save_and_continue skipped (no in-RAM save on the guest)");
    uintptr_t f = (uintptr_t)from - g_base;
    if (f == site::SaveUiA || f == site::SaveUiB || f == site::SaveUiC) Hud("Saving is off on the guest: only the host's game saves");
}

// GF-MIS: replaces the 5-byte store at 0x39D69C with a call here. The restore index keeps the -1 from
// mission_system_level_init (0x3A5883); eax (the blob's value) is only recorded for the log. Registers and flags are
// untouched; the call's return address sits below mission_info_load's pushed argument only while the stub runs.
__attribute__((naked)) void MisStoreStub() {
    asm volatile("movl %eax, _gc_guestMisRead\n\t"
                 "ret");
}

// GF-ACT: both mission_activate entries jump here; caller-cleaned and no caller reads the result (guest_impl.md).
void __cdecl OnMissionActivate(unsigned handle, int) {
    Hit(g_nMis, __builtin_return_address(0), "mission_activate(handle %X) refused", handle);
}
void __cdecl OnMissionActivateByName(const char* name, int) {
    Hit(g_nMis, __builtin_return_address(0), "mission_activate(\"%.40s\") refused", name ? name : "");
}

// GF-POP (M5 part): an activity-replay world carries the node to force-start; activity_system_do_frame(0) consumes it
// inside gameplay_do_frame (0x3ACB17).
void __cdecl OnGameplayDoFrame() {
    int& node = *Rva<int>(site::NodeStartOnLoad);
    if (node != -1) {
        Log("guest: GF-POP: Activity_node_start_on_load %08X cleared (an activity replay in the loaded world)", node);
        node = -1;
    }
    Fn<void __cdecl()>(site::GameplayDoFrame)();
}

// GF-LOAD backstop. The UI entries are refused earlier (below), so whatever reaches this is unexpected: log the caller.
using SetPendingFn = uint32_t(__thiscall*)(void*, const char*, int, int, int, int, int);
uint32_t __thiscall OnSetPending(uint8_t* w, const char* name, int newGame, int single, int safehouse, int dlc, int reset) {
    void* from = __builtin_return_address(0);
    auto next = (SetPendingFn)g_loadNext;
    uintptr_t me = (uintptr_t)&__ImageBase, x = (uintptr_t)from;
    bool ours = x - me < ((IMAGE_NT_HEADERS*)(me + __ImageBase.e_lfanew))->OptionalHeader.SizeOfImage;
    if (ours && g_allowLoad && GetTickCount64() - g_allowAt < kAllowMs) {
        uint32_t r = next(w, name, newGame, single, safehouse, dlc, reset);
        // a refusal by the game itself keeps the pass for the session's retry. Q18: only a load of the host's world
        // starts recording (not one the guest began before its link), and only at its end (not on the frames before)
        if (r & 0xFF) g_allowLoad = false, g_loStart = true;
        Log("guest: the session's load %s", r & 0xFF ? "started" : "was refused by the game (busy), kept for its retry");
        return r;
    }
    x -= g_base;
    if (x == site::VideoExitA && !(newGame & 0xFF)) { // the in-RAM world only (the zone-warp load is a fresh territory)
        Hit(g_nVideo, from, "warp-to-object video load let through (in-RAM world; a refusal would drop to the main menu), then re-join");
        Rejoin("a warping video loaded the guest");
        return next(w, name, newGame, single, safehouse, dlc, reset);
    }
    Hit(g_nLoad, from, "load refused: set_pending_territory_load(\"%.40s\", new %d, dlc %d)", name ? name : "", newGame & 0xFF, dlc);
    Field<uint8_t>(w, site::WFlags2) &= ~4; // no pending load_from_memory left behind (process_save_load clears it too, 0x3C0171)
    Rejoin("the guest's game tried to load");
    // Esc -> Load started before the fences (still dialling), refused at its last step: 0x3F0359 sets Verify_state -1
    // and nothing pops GS 0x37 (only the load's gameseq_set_state(2) 0x51C3C7 would have left it): a loading screen for
    // good. Pop as the verify's own failure path does (0x3F0257-0x3F0269); verify_gamesave_exit 0x3C93C0 then drops the
    // loading screen and its input. platform_load (0x3F032E) already set the guest save's World+0x2578 / +0x2694 and
    // hammer / backpack / jetpack globals: the host's world load (WorldMeta) and LOADOUT replace them.
    if (x == site::VerifyLoadRet) Fn<void __cdecl()>(site::PopState)();
    // video_cutscene_exit_state's DLC branch (0x306F6D -> dlc_load_level) has no fallback: still in GS 0x31 on a freed
    // video, paused. Do what its refused siblings do: gameseq_set_state(GS_MAINMENU, 1) (0x306F26 / 0x306F5A). The
    // direct path (dlc_process_delayed_selection 0x508BC9, no video) is at the main menu: untouched.
    if (x == site::DlcLoadRet && GameState() == kGsVideo) Fn<void __cdecl(int, int)>(site::SetState)(0, 1);
    return 0;
}

// Q14 backstop: every activity start ends in one of these two (xrefs: 9 modal, 7 free callers). Convoys stay allowed.
// false is the game's own refusal (another modal activity running, 0x3ACA22), returned before any side effect.
template <int K> uint32_t __thiscall OnActivityStart(uint8_t* act) {
    int type = Field<int>(act, 4);
    if (type == kConvoy) return ((uint32_t(__thiscall*)(void*))g_actNext[K])(act);
    Hit(g_nStart, __builtin_return_address(0), "activity start refused: type %d (%s)", type, K ? "free" : "modal");
    Hud(kActs);
    return 0;
}

// Q14 / Q13: a refused press does nothing; the prompt itself still shows (go = 0 runs the original).
template <int K> uint32_t __cdecl OnUse(uint8_t* p, int go) {
    if (!(go & 0xFF)) return Fn<uint32_t __cdecl(uint8_t*, int)>(kUse[K].fn)(p, go);
    Hit(g_nUse, __builtin_return_address(0), "%s refused", kUse[K].what);
    Hud(kUse[K].hud);
    return 0;
}
void* const kUseHook[] = {(void*)&OnUse<0>, (void*)&OnUse<1>, (void*)&OnUse<2>, (void*)&OnUse<3>,
                          (void*)&OnUse<4>, (void*)&OnUse<5>, (void*)&OnUse<6>};
static_assert(sizeof kUseHook / sizeof *kUseHook == sizeof kUse / sizeof *kUse, "one hook per use callback");

// Q15: fast travel. false = fsm_warp_to_safehouse_cb's own "no warp" path (0x454493: wait for button ups, close the
// message box), reached before save_and_warp, the instant fade-out and the map pop.
uint32_t __cdecl OnFastTravelPos(void*, void*, void*) {
    Hit(g_nUi, __builtin_return_address(0), "fast travel refused");
    Hud("Only the host can fast travel (F7 teleports to the host)");
    return 0;
}
// Esc -> Load: before Pending_save and the verify state (which copies the save into the world buffer, 0x3F0280).
void __cdecl OnVerifyStart(void*, int) {
    Hit(g_nUi, __builtin_return_address(0), "Esc load refused");
    Hud("Only the host can load a game (F7 teleports to the host)");
}
// Esc -> Restart: vdir_restore (its first side effect) is skipped, then the reload is refused and the menu closes as
// the handshake's default branch does (jmp gameseq_pop_state 0x4AF626).
void __cdecl OnRestartVdir(int, int) {}
uint32_t __thiscall OnRestartReload(void*) {
    Hit(g_nUi, __builtin_return_address(0), "Esc restart refused");
    Hud("Only the host can restart (F7 teleports to the host)");
    Fn<void __cdecl()>(site::PopState)();
    return 0;
}
// Esc -> Cancel mission (mission_cancel returns at once with no mission, which the guest never has; closed the same way)
void __cdecl OnCancelMission() {
    Hit(g_nUi, __builtin_return_address(0), "Esc cancel mission refused");
    Hud("Only the host can cancel missions (F7 teleports to the host)");
    Fn<void __cdecl()>(site::PopState)();
}
// Death -> Continue: no local reload; the host sends its world again (the session's load then leaves the death state).
void __cdecl OnDeathContinue() {
    Hit(g_nUi, __builtin_return_address(0), "death Continue -> re-join request");
    Rejoin("death");
}

// ---- install --------------------------------------------------------------------------------------------------------
bool Expect(uintptr_t at, const void* want, size_t n, const char* what) {
    if (!memcmp(Rva(at), want, n)) return true;
    Log("guest: %s at %X: unexpected bytes", what, (unsigned)at);
    return false;
}
bool ExpectRel(uintptr_t at, uint8_t op, uintptr_t fn, const char* what) { // E8/E9 rel32 to the game's own function
    uint8_t* s = Rva<uint8_t>(at);
    if (s[0] == op && (uintptr_t)(s + 5 + *(int32_t*)(s + 1)) == g_base + fn) return true;
    Log("guest: %s at %X: not the expected %s to %X (a sibling chained it?)", what, (unsigned)at, op == 0xE8 ? "call" : "jmp", (unsigned)fn);
    return false;
}
void Branch(uintptr_t at, uint8_t op, const void* to, size_t len) { // E8/E9 rel32, NOP-padded to len
    uint8_t code[16];
    memset(code, 0x90, sizeof code);
    code[0] = op;
    int32_t rel = (int32_t)((uintptr_t)to - (g_base + at + 5));
    memcpy(code + 1, &rel, 4);
    WriteCode(Rva(at), code, len);
}
// Entry detour: the n prologue bytes move to a 32-byte trampoline slot (an E8 rel32 at offset `call` is re-aimed) that
// jumps back to at+n; the entry becomes a jmp to hook. Returns the trampoline: calling it runs the original.
uint8_t* g_tramp;
int g_trampUsed;
uintptr_t Detour(uintptr_t at, size_t n, void* hook, int call = -1) {
    uint8_t *s = Rva<uint8_t>(at), *t = g_tramp + g_trampUsed;
    g_trampUsed += 32;
    memcpy(t, s, n);
    if (call >= 0) {
        int32_t rel = (int32_t)((uintptr_t)(s + call + 5) + *(int32_t*)(s + call + 1) - (uintptr_t)(t + call + 5));
        memcpy(t + call + 1, &rel, 4);
    }
    t[n] = 0xE9;
    int32_t back = (int32_t)((uintptr_t)(s + n) - (uintptr_t)(t + n + 5));
    memcpy(t + n + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    Branch(at, 0xE9, hook, n);
    return (uintptr_t)t;
}

// Checks every site, then writes them all. Sim thread.
bool Install() {
    uint32_t mp = (uint32_t)(g_base + rva::Multiplayer), restore = (uint32_t)(g_base + site::MisRestoreIndex);
    uint8_t misStore[5] = {0xA3}, death[9] = {0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x75, 0x46}; // abs32: runtime addresses
    memcpy(misStore + 1, &restore, 4), memcpy(death + 2, &mp, 4);
    static const uint8_t save[9] = {0x56, 0x8B, 0xF1, 0x8A, 0x86, 0x5C, 0x26, 0x00, 0x00}, actH[5] = {0x8B, 0x44, 0x24, 0x08, 0x50},
                         actN[6] = {0x51, 0x56, 0x8B, 0x74, 0x24, 0x0C}, pending[3] = {0x56, 0x8B, 0xF1},
                         modal[8] = {0x57, 0x8B, 0xF9, 0x8B, 0x07, 0x8B, 0x50, 0x7C}, freeS[8] = {0x56, 0x8B, 0xF1, 0x8B, 0x06, 0x8B, 0x50, 0x18};
    bool ok = Expect(site::SaveAndContinue, save, 9, "GF-SAVE save_and_continue prologue");
    ok &= Expect(site::MisStore, misStore, 5, "GF-MIS store");
    ok &= Expect(site::MisActByHandle, actH, 5, "GF-ACT mission_activate(handle)");
    ok &= Expect(site::MisActByName, actN, 6, "GF-ACT mission_activate(name)");
    ok &= ExpectRel(site::ScriptCall, 0xE8, site::ScriptDoFrame, "GF-SCR");
    ok &= ExpectRel(site::GameplayCall, 0xE8, site::GameplayDoFrame, "GF-POP");
    ok &= Expect(site::SetPending, pending, 3, "GF-LOAD prologue");
    ok &= ExpectRel(site::SetPending + 3, 0xE8, site::KillMsgbox, "GF-LOAD prologue call");
    ok &= Expect(site::ModalStart, modal, 8, "modal activity_start prologue");
    ok &= Expect(site::FreeStart, freeS, 8, "free activity_start prologue");
    for (const Use& u : kUse) {
        uint8_t* s = Rva<uint8_t>(u.store);
        if (s[0] != 0xC7 || (s[1] != 0x40 && s[1] != 0x46) || s[2] != 0x14 || *(uint32_t*)(s + 3) != g_base + u.fn)
            ok = false, Log("guest: %s callback store at %X: unexpected bytes", u.what, (unsigned)u.store);
    }
    ok &= ExpectRel(site::FastTravelCall, 0xE8, site::SafehousePos, "fast travel");
    for (uintptr_t c : site::VerifyCalls) ok &= ExpectRel(c, 0xE8, site::VerifyStart, "Esc load");
    ok &= ExpectRel(site::RestartVdirCall, 0xE8, site::VdirRestore, "Esc restart vdir");
    ok &= ExpectRel(site::RestartJmp, 0xE9, site::SetPendingReload, "Esc restart");
    ok &= ExpectRel(site::CancelJmp, 0xE9, site::MissionCancel, "Esc cancel mission");
    ok &= Expect(site::DeathContinue, death, 9, "death Continue prologue");
    if (!ok) return false;
    if (!(g_tramp = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        return Log("guest: VirtualAlloc for the trampolines failed"), false;

    Branch(site::SaveAndContinue, 0xE9, (void*)&OnSaveAndContinue, 5);
    Branch(site::MisStore, 0xE8, (void*)&MisStoreStub, 5);
    Branch(site::MisActByHandle, 0xE9, (void*)&OnMissionActivate, 5);
    Branch(site::MisActByName, 0xE9, (void*)&OnMissionActivateByName, 6);
    static const uint8_t nop5[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
    WriteCode(Rva(site::ScriptCall), nop5, 5);
    ChainRel32(site::GameplayCall, (void*)&OnGameplayDoFrame);
    g_loadNext = Detour(site::SetPending, 8, (void*)&OnSetPending, 3);
    g_actNext[0] = Detour(site::ModalStart, 8, (void*)&OnActivityStart<0>);
    g_actNext[1] = Detour(site::FreeStart, 8, (void*)&OnActivityStart<1>);
    for (size_t k = 0; k < sizeof kUse / sizeof *kUse; k++) {
        uint32_t to = (uint32_t)(uintptr_t)kUseHook[k];
        WriteCode(Rva(kUse[k].store + 3), &to, 4);
    }
    ChainRel32(site::FastTravelCall, (void*)&OnFastTravelPos);
    for (uintptr_t c : site::VerifyCalls) ChainRel32(c, (void*)&OnVerifyStart);
    ChainRel32(site::RestartVdirCall, (void*)&OnRestartVdir);
    ChainRel32(site::RestartJmp, (void*)&OnRestartReload);
    ChainRel32(site::CancelJmp, (void*)&OnCancelMission);
    Branch(site::DeathContinue, 0xE9, (void*)&OnDeathContinue, 7); // the original never runs on a guest
    return true;
}

// ---- Q13 loadout ----------------------------------------------------------------------------------------------------
bool UpgradesValid(const uint8_t* up) { // every level must name a real level (get_upgrade_level 0x35F4C0 bound)
    for (int i = 0; i < 128; i++) {
        int level = up[8 * i], count = i < kUpgrades ? Rva<uint8_t>(site::UpgradeData)[i * 0x228 + 5] : 0;
        if (level && level >= count) return Log("guest: LOADOUT upgrade %d level %d (levels %d): table refused", i, level, count), false;
    }
    return true;
}

// ---- Q17 pick (notes/picker_impl.md) ----------------------------------------------------------------------------------
// The backpack levels the shop writes (purchase_upgrade 0x6C3D9C, 0x6C3DF5-0x6C3E5A): the picked pack 1, the others 0;
// the jetpack (upgrade 0) 2 = with recharge when picked, else 0. upgrade_get_backpack_type 0x375CE0 reads only these
// bytes and player::do_frame 0x6D5694 makes the pack from it.
void PickTable(uint8_t* meta) {
    if (g_pickPack < 0) return;
    meta[0x18] = g_pickPack == 0 ? 2 : 0;
    for (int u = 0x35; u <= 0x3D; u++) meta[0x18 + 8 * u] = u == g_pickPack;
}

// Once per load (the host's blob brought the host's hammer and pack), on the first frame on foot.
// Hammer: purchase_upgrade's slot-3 swap (0x6C3B33-0x6C3D53) with the inventory delete / add / equip calls.
// Backpack: the table, then do_frame creates the picked pack next frame unless the attached one (+0x1170, info +0x270,
// id +0x50) already is it; its event_player_picked_up drops the old one. None drops it the way 0x5F10A0 does.
void ApplyPick(uint8_t* me) {
    uint8_t* old = Fn<uint8_t* __cdecl(void*, int)>(site::InvItem)(me, 3);
    uint8_t* had = old ? Field<uint8_t*>(old, 8) : nullptr;
    if (g_pickHammer && had != g_pickHammer) {
        bool held = old && Field<uint8_t*>(me, 0xC90) == old; // equipped inventory_item (human::get_equipped_inv_item)
        if (old) Fn<void __cdecl(void*, void*, bool, bool)>(site::DeleteItem)(me, old, false, false);
        uint8_t* it = Fn<uint8_t* __cdecl(void*, void*, int, int, int, bool, bool, bool)>(site::AddItem)(me, g_pickHammer, 1, 99999, 3, false, false, false);
        if (it && held) Fn<uint32_t __cdecl(void*, void*, void*, bool)>(site::EquipItem)(me, it, nullptr, false);
        Log("guest: Q17 hammer %s -> %s%s", had ? Field<const char*>(had, 0) : "none", Field<const char*>(g_pickHammer, 0),
            it ? (held ? " (in hand)" : "") : " FAILED (add_item 0)");
    }
    if (g_pickPack < 0) return;
    PickTable(me + site::MetaData);
    uint8_t* bp = Field<uint8_t*>(me, 0x1170);
    uint8_t* info = bp ? Field<uint8_t*>(bp, 0x270) : nullptr;
    int have = info ? Field<int>(info, 0x50) : -1, want = g_pickPack ? g_pickPack - 0x34 : 0;
    int& last = *Rva<int>(site::LastBackpack);
    if (g_pickPack == 0x3E) {
        if (bp) Fn<void __thiscall(void*, bool)>(site::BackpackDropped)(bp, true);
    } else
        last = have == want ? want : -1;
    Log("guest: Q17 backpack upgrade %02X: attached id %d, s_lastBackpackIndex %d%s", g_pickPack, have, last,
        g_pickPack == 0x3E ? (bp ? " (dropped)" : "") : have == want ? " (already worn)" : " (made next frame)");
}

// ---- Q18 loadout (guest_impl.md "Rule Q18") -------------------------------------------------------------------------
uint8_t* InvSlot(uint8_t* me, int slot) { return Fn<uint8_t* __cdecl(void*, int)>(site::InvItem)(me, slot); }
const char* ItemName(uint8_t* it) { // inventory_item +8 inv_item_info*, its +0 name
    uint8_t* info = it ? Field<uint8_t*>(it, 8) : nullptr;
    return info ? Field<const char*>(info, 0) : "-";
}
uint8_t* WeaponOf(uint8_t* it) { // the item's weapon object, checked as save_inventory does (0x6347C6-0x6347EC)
    uint8_t* o = Fn<uint8_t* __thiscall(void*, unsigned)>(site::ObjFromHandle)(Rva(rva::World), Field<unsigned>(it, 0x18));
    return o && o[0x7E] == 1 && o[0x7F] == 7 ? o : nullptr;
}
const char* LoText(const Loadout& lo) {
    static char t[320]; // at most 3 x 75 + 15 characters: never truncated, so n stays inside t
    int n = 0;
    for (const LoSlot& r : lo.s)
        n += snprintf(t + n, sizeof t - n, "%d %s %d/%d, ", (int)(&r - lo.s), *r.name ? r.name : "-", r.mag, r.res);
    snprintf(t + n, sizeof t - n, "held %d", lo.held);
    return t;
}

// Every gameplay frame on foot and alive, from the session's first join on: so g_lo is the last good state before any load.
void RecordLoadout(uint8_t* me) {
    Loadout lo{};
    for (int s = 0; s < 3; s++) {
        LoSlot& r = lo.s[s];
        r.mag = r.res = -1;
        uint8_t* it = InvSlot(me, s);
        if (!it) continue;
        snprintf(r.name, sizeof r.name, "%s", ItemName(it));
        r.count = Field<int>(it, 0xC);
        if (uint8_t* w = WeaponOf(it)) r.mag = Field<int>(w, 0x20C), r.res = Field<int>(w, 0x210); // get_rounds_in_*
    }
    uint8_t* held = Field<uint8_t*>(me, 0xA30); // desired_equipped_inv_item (what puppet.cpp's capture sends)
    // unequipped (turret 0x6B5F5D, held object 0x6BE9D9, holster): human_unequip_inventory_item put it in +0xA34
    // (0x653831-0x653846), the item the game re-equips (unholster 0x6CD54F); a delete keeps +0xA34 valid (0x6C7765)
    if (!held) held = Field<uint8_t*>(me, 0xA34);
    lo.held = held ? Field<int>(held, 0x10) : -1, lo.have = true;
    g_lo = lo;
}

// After a re-join, on the first frame on foot, right after ApplyPick (slot 3 is its). The blob put the host's items in.
// Pass 1 deletes what differs from ours; pass 2 adds ours. Two passes: human_add_item_to_inventory merges into an item
// with the same info wherever it sits and only adds ammo (0x6B4E50, 0x6B5169), so none of ours may sit in another slot.
// Ammo is set after the add: the add's own split fills the magazine first and scales by the backpack (0x6B5018-0x6B5078).
void RestoreLoadout(uint8_t* me) {
    uint8_t* want[3];
    bool skip[3];
    for (int s = 0; s < 3; s++) {
        const LoSlot& r = g_lo.s[s];
        uint8_t* cur = InvSlot(me, s);
        want[s] = *r.name ? Fn<uint8_t* __cdecl(const char*)>(site::InvInfo)(r.name) : nullptr;
        if ((skip[s] = *r.name && !want[s])) {
            Log("guest: Q18 slot %d: %s is unknown in this world, skipped (keeps %s)", s, r.name, ItemName(cur));
            continue;
        }
        if (!cur || Field<uint8_t*>(cur, 8) == want[s]) continue;
        Log("guest: Q18 slot %d: %s removed", s, ItemName(cur));
        Fn<void __cdecl(void*, void*, bool, bool)>(site::DeleteItem)(me, cur, false, false);
    }
    for (int s = 0; s < 3; s++) {
        const LoSlot& r = g_lo.s[s];
        if (skip[s] || !want[s]) continue;
        uint8_t* it = InvSlot(me, s);
        bool added = !it;
        if (added) it = Fn<uint8_t* __cdecl(void*, void*, int, int, int, bool, bool, bool)>(site::AddItem)(me, want[s], r.count, -1, s, false, false, false);
        if (!it || Field<int>(it, 0x10) != s) { // NULL: no room or no weapon; another slot: merged into a carried one
            Log("guest: Q18 slot %d: %s NOT added (%s)", s, r.name, it ? "already carried in another slot" : "add_item 0");
            continue;
        }
        uint8_t* w = r.mag >= 0 ? WeaponOf(it) : nullptr;
        if (w) {
            Fn<void __thiscall(void*, int)>(site::SetMagazine)(w, r.mag);
            Fn<void __thiscall(void*, int)>(site::SetReserve)(w, r.res);
        }
        Log("guest: Q18 slot %d: %s %s, ammo %d/%d", s, r.name, added ? "added" : "kept", w ? Field<int>(w, 0x20C) : -1,
            w ? Field<int>(w, 0x210) : -1);
    }
    uint8_t* held = g_lo.held >= 0 ? InvSlot(me, g_lo.held) : nullptr;
    if (held) Fn<uint32_t __cdecl(void*, void*, void*, bool)>(site::EquipItem)(me, held, nullptr, false);
    Log("guest: Q18 loadout restored, held slot %d (%s)", g_lo.held, held ? ItemName(held) : "not equipped: empty or none");
}

// As a load does it: the table (load_meta_data 0x65E09A), the locker list (hud_ui_weapon_cabinet_reset 0x45B450 count
// = 0, hud_ui_weapon_cabinet_load 0x4772C2 add_unique per item), the weapons the table unlocks (0x65E0BE). Every other
// upgrade effect reads the table live (guest_impl.md "Upgrade effects"; backpacks and the jetpack too: player::do_frame
// 0x6D5694 recreates them from it); armor's prop is made as purchase_upgrade does (0x6C3EAF-0x6C3EC9). Not live: a
// hammer purchase's inventory swap (the guest keeps its old hammer until the next re-join). Gameplay only (the locker
// UI is its own game state).
void ApplyLoadout(uint8_t* me) {
    g_loadoutIn = false;
    uint8_t* meta = me + site::MetaData;
    uint8_t armor = meta[0x18 + 8 * kArmorUpgrade];
    if (g_upOk) memcpy(meta + 0x18, g_up, sizeof g_up), PickTable(meta); // Q17: our pack, before do_frame sees the host's
    Field<int>(meta, 4) = g_salvage = g_loSalvage;
    g_adopt = false;
    int added = 0, unknown = 0;
    if (g_arsenalOk) {
        uint8_t* list = Rva<uint8_t>(site::WeaponList);
        Field<int>(list, 0xC) = 0;
        for (int i = 0; i < g_arsenalN; i++) {
            struct { uint32_t uid; uint8_t bought, pad[3]; } item = {*(uint32_t*)(g_arsenal + 8 * i), (uint8_t)(g_arsenal[8 * i + 4] != 0), {}};
            if (!Fn<void* __cdecl(int)>(site::WeaponByUid)((int)item.uid)) { unknown++; continue; } // untrusted id
            if (Fn<int __thiscall(void*, void*)>(site::AddUnique)(list, &item) >= 0) added++;
        }
    }
    Fn<void __thiscall(void*)>(site::AddUnlocked)(meta);
    uint8_t now = meta[0x18 + 8 * kArmorUpgrade];
    if (now > armor) Fn<void __cdecl(int, void*)>(site::ArmorProp)(now, me);
    Log("guest: host loadout applied: upgrades %s, lockers %d weapons (%d unknown ids dropped)%s, salvage %d, armor %d -> %d",
        g_upOk ? "copied" : "kept", added, unknown, g_arsenalOk ? "" : " [list kept]", g_salvage, armor, now);
}

// The guest's own salvage gains (pickups, crates, mining, a convoy's reward) belong to the host (rule Q13). Shown at
// once; the host's next LOADOUT replaces the number with the shared total. A drop is put back: the guest cannot
// spend the host's wallet. After a load the loaded value is the host's (the blob), not a gain.
void TrackSalvage(uint8_t* me) {
    int& s = Field<int>(me + site::MetaData, 4);
    if (g_adopt) {
        g_adopt = false, g_salvage = s;
        return;
    }
    if (s == g_salvage) return;
    if (s > g_salvage) {
        Log("guest: salvage +%d collected -> the host", s - g_salvage);
        Session_SendSalvage(s - g_salvage);
        g_salvage = s;
    } else {
        Log("guest: salvage %d -> %d put back (the wallet is the host's)", g_salvage, s);
        s = g_salvage;
    }
}

} // namespace

void Guest_Install() {} // nothing before Guest_Arm

// Q18, session.cpp Start (F8 / autoconnect, never a redial): the session's first join starts from the host's loadout.
// A redial after a dropped link keeps the record, so its join blob is restored over like any re-join
void Guest_ResetLoadout() { g_lo = {}, g_loOn = g_loDue = g_loStart = false; }

void Guest_Arm() {
    if (g_armed || g_failed) return;
    if (!World_ArmSaveGuard("guest") || !Install()) {
        g_failed = true; // the bytes will not change: no retry
        Log("guest: fences NOT armed (see above): this game must not join");
        return Hud("Co-op guest fences unavailable (RfgCoop.log)");
    }
    g_armed = g_adopt = true;
    gc_guestMisRead = kNotRead;
    Destr_GuestArm(); // M6: GF-EDF from ver7.0 (m6_plan §0.2)
    Log("guest: fences armed until the game exits: save guard 811278, GF-SAVE 51D040, GF-MIS 39D69C, GF-ACT 38E560 39D960, "
        "GF-SCR 3EE1FB, GF-POP 3EDF23, GF-LOAD 51C320, activity starts 3ACA10 3ACA80 (convoys allowed), node callbacks x%d, "
        "UI: fast travel 454469, Esc load x3, restart 4AF634/4AF641, cancel 4AF62B, death continue 492FF0",
        (int)(sizeof kUse / sizeof *kUse));
}

bool Guest_Armed() { return g_armed; }

void Guest_AllowLoad() {
    g_allowLoad = true, g_allowAt = GetTickCount64();
}

void Guest_Tick(int state) {
    if (state == kGsGameplay && *g_hudPending && LocalPlayer()) {
        Fn<int __cdecl(char*, float, void*, void*, int*)>(site::HudMessageAdd)(g_hudPending, 4.0f, nullptr, nullptr, nullptr);
        *g_hudPending = 0;
    }
    if (!g_armed) return;
    if (state != kGsGameplay) {
        if (state == kGsLoad && !g_loaded) {
            g_loaded = true;
            if (g_lo.have) Log("guest: Q18 loadout before the load: %s", LoText(g_lo));
        }
        return;
    }
    uint8_t* me = LocalPlayer();
    if (!me) return;
    if (g_loaded) { // E10: what the loaded world tried to bring along
        g_loaded = false, g_adopt = true;
        g_pickDue = g_pickHammer || g_pickPack >= 0; // Q17: the load brought the host's hammer and pack
        g_loDue = g_lo.have;                         // Q18: and the host's weapons. A session's first join has no record
        if (!g_lo.have)                              // nor has a load the guest began before its link (no g_loStart)
            Log("guest: Q18 nothing recorded this session: the loaded weapons stand, %s",
                g_loOn || g_loStart ? "recording from now" : "not recorded (a load from before the link)");
        g_loOn |= g_loStart, g_loStart = false;
        int read = gc_guestMisRead;
        gc_guestMisRead = kNotRead;
        Log("guest: in gameplay after a load: cur_world_state %d, mission_system_load read %d%s, Mission_active_index %d, "
            "Mission_mode %d; so far %d mission_activate, %d loads, %d saves, %d activity starts refused",
            Field<int>(Rva(rva::World), site::WCurState), read == kNotRead ? -1 : read, read == kNotRead ? " (no read)" : " (not stored)",
            *Rva<int>(site::MissionActive), *Rva<uint8_t>(site::MissionMode), g_nMis, g_nLoad, g_nSave, g_nStart);
    }
    bool onFoot = !Fn<void* __cdecl(void*)>(site::InVehicle)(me); // no inventory swap in a vehicle
    if (onFoot && g_pickDue) g_pickDue = false, ApplyPick(me);
    if (onFoot && g_loDue) g_loDue = false, RestoreLoadout(me); // Q18 after Q17: slot 3 is the pick's
    TrackSalvage(me);                  // this frame's gain goes out before a LOADOUT overwrites the wallet and its baseline (Q13)
    if (g_loadoutIn) ApplyLoadout(me);
    // Q18: on foot the restore above already ran, so the blob's items are never recorded; not while dying either (the
    // last alive frame is the one to keep)
    if (onFoot && g_loOn && !(Fn<uint32_t __cdecl(void*)>(site::IsDead)(me) & 0xFF)) RecordLoadout(me);
}

// picker.cpp, sim thread: kept for the process (the next confirmed pick replaces it); nothing is saved (save guard)
void Guest_SetPick(void* hammer, int pack) {
    g_pickHammer = (uint8_t*)hammer, g_pickPack = pack;
    Log("guest: Q17 pick: hammer %s, backpack upgrade %02X (applied after every join and re-join)",
        hammer ? Field<const char*>(hammer, 0) : "none", pack);
}

// session.cpp, sim thread, guest. Received bytes: sizes and every level checked here, weapon ids when applied.
void Rules_ApplyHostLoadout(const uint8_t* upgrades, int upgradeBytes, const uint8_t* arsenal, int arsenalBytes, int salvage) {
    if (!g_armed) return Log("guest: LOADOUT ignored (not an armed guest)");
    g_upOk = upgradeBytes == (int)sizeof g_up && UpgradesValid(upgrades);
    if (g_upOk) memcpy(g_up, upgrades, sizeof g_up);
    else Log("guest: LOADOUT upgrade table refused (%d bytes, expected %d)", upgradeBytes, (int)sizeof g_up);
    g_arsenalOk = arsenalBytes >= 0 && arsenalBytes <= (int)sizeof g_arsenal && arsenalBytes % 8 == 0;
    if (g_arsenalOk) memcpy(g_arsenal, arsenal, arsenalBytes), g_arsenalN = arsenalBytes / 8;
    else Log("guest: LOADOUT locker list refused (%d bytes)", arsenalBytes);
    g_loSalvage = salvage < 0 ? 0 : salvage;
    g_loadoutIn = true;
}

// session.cpp, sim thread, host: the partner's salvage into our own wallet, through the game's add (30000 cap).
void Rules_AddHostSalvage(int amount) {
    uint8_t* me = LocalPlayer();
    if (amount < 1 || amount > 30000 || !me) return Log("rules: partner salvage %d dropped (%s)", amount, me ? "amount" : "no Player");
    Fn<void __thiscall(void*, int, int)>(site::AddSalvage)(me + site::MetaData, amount, 0);
    Log("rules: +%d salvage from the partner, wallet now %d", amount, Field<int>(me + site::MetaData, 4));
}
