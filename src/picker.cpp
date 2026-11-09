// Q17 picker (RESEARCH.md §8 Q17): a guest's F8 opens one game message box: HAMMER/BACKPACK char-list rows + CONNECT
// (ver6.2; ver6.1's two 16/11-row boxes outgrew the box's hexagon stencil, findings/workflows/picker_layout.md).
// Left/Right cycles a row's value, Enter / A on CONNECT hands both to guest.cpp (Guest_SetPick) and session.cpp starts
// the connection; Esc / B starts nothing. The box brings the game's own pause token, UI input lock, keyboard / pad /
// mouse navigation and wait-for-button-ups. Sim thread only (Net_Tick). Sites, ABIs and the in-game checks:
// research/notes/picker_impl.md.
#include "features.h"
#include "game.h"

namespace {

namespace site {
constexpr uintptr_t AddMsgbox     = 0x004F6C70; // int __cdecl ui_add_msgbox(msgbox_type, char* title, char* text, cb, bool,
                                                //   bool, char* accept, char* cancel, bool pause_audio): handle, -1 none
constexpr uintptr_t AddCaption    = 0x004F06E0; // int __cdecl ui_msgbox_add_caption_item(int box, char* text, bool, bool)
constexpr uintptr_t AddCharList   = 0x004CC780; // int __cdecl ui_msgbox_add_char_list_item(int box, char* caption,
                                                //   char** list, int count): item handle, -1 none. Keeps list (item+0x9C)
constexpr uintptr_t SetCharList   = 0x004B93D0; // bool __cdecl ui_menu_set_char_list_index(int menu, int item, int value):
                                                //   item+0x94 = value if a char list and value in range; returns 0
constexpr uintptr_t MenuPtr       = 0x004B7730; // ui_menu* __cdecl ui_menu_get_via_handle(int menu), NULL none
constexpr uintptr_t BoxMenu       = 0x004CC800; // int __cdecl ui_msgbox_get_menu_handle(int box)
constexpr uintptr_t SetSelected   = 0x004B9490; // void __cdecl ui_menu_set_selected_item(int menu, int index)
constexpr uintptr_t BoxActive     = 0x004CC4D0; // bool __cdecl ui_is_msgbox_active(int box)
constexpr uintptr_t EncodeWide    = 0x00065250; // void __cdecl vint_encode_wide_text(char* out, unsigned size, wchar_t* in)
constexpr uintptr_t HudMessageAdd = 0x00455D30; // hud_message_handle __cdecl(char*, float, void*, void*, int*)
constexpr uintptr_t InvInfo       = 0x00612240; // inv_item_info* __cdecl inv_item_get_info(char*): +0 name, +8 display key
constexpr uintptr_t InvItem       = 0x00607820; // inventory_item* __cdecl human_get_inventory_item(human*, int slot): +8 info
constexpr uintptr_t BackpackType  = 0x00375CE0; // int __cdecl upgrade_get_backpack_type(): 0 jetpack, 0x35..0x3D, 0x3E none
constexpr uintptr_t Hammers       = 0x00F10058; // Hammer_upgrade_data[16], 0xC each: +4 char* inv item name
constexpr uintptr_t Backpacks     = 0x02BE7D18; // Backpack_info: multi_backpack_info*[16] by id, +0x24 display key
} // namespace site

using BoxCb = bool(__cdecl*)(int box, int index, int choice); // msgbox_choice: 0 OK, 1 ABORT, 2 FORCED_EXIT
using AddList = int __cdecl(int, const char*, const char* const*, int);
constexpr int kNone = 0x3E;                                   // upgrade_get_backpack_type: no backpack

struct Item { const char* key; uintptr_t pick; }; // key: a localization key (shown localized); pick: what it picks
Item g_hammers[16], g_packs[11];                  // pick: inv_item_info* / backpack upgrade (kNone, 0, 0x35..0x3D)
const char* g_hKey[16], *g_pKey[11];              // the rows' value lists: the game keeps these pointers while the box lives
int g_nHammers, g_nPacks, g_packSel;              // g_packSel: the pack worn at F8
int g_rowH, g_rowP;                               // the two rows' values, read in OnPick
bool g_open;                                      // the box is up (or its answer not yet taken)
int g_box = -1, g_result = -2;                    // the box; its answer: -2 waiting, -1 cancelled, 1 CONNECT

uint8_t* LocalPlayer() { return *Rva<uint8_t*>(rva::Player); }
const char* Key(uint8_t* inv) { const char* k = Field<const char*>(inv, 8); return k && *k ? k : Field<const char*>(inv, 0); }

void Say(const char* t) { // HUD line (shows once the box is gone and the game runs again)
    Log("picker: HUD \"%s\"", t);
    if (GameState() == 1 && LocalPlayer())
        Fn<int __cdecl(const char*, float, void*, void*, int*)>(site::HudMessageAdd)(t, 4.0f, nullptr, nullptr, nullptr);
}

// m_which_char of row i of the box's ui_menu, read the way ui_menu_set_char_list_index does (0x4B93FF-0x4B942A):
// items inline at menu+0x24, count menu+0x20, item+0xC == 1 a char list. -1: not a char list (or no menu)
int Row(int menu, int i) {
    uint8_t* m = Fn<uint8_t* __cdecl(int)>(site::MenuPtr)(menu);
    uint8_t* it = m && i < Field<int>(m, 0x20) ? Field<uint8_t*>(m, 0x24 + 4 * i) : nullptr;
    return it && Field<int>(it, 0xC) == 1 ? Field<int>(it, 0x94) : -1;
}

// Inside ui_process (sim thread): CONNECT (the only type-0 row, my_selection_cb 0x4F0300) = OK, Esc / B (the abort
// hint 0x4F03FD) = ABORT, ui_kill_msgbox 0x4DC998 = FORCED_EXIT (called before the menu is freed, 0x4DC9F2). Enter / A
// on a char-list row calls nothing (ui_menu::process_input 0x4DC1F1 only selects type-0 rows). The rows are read here,
// before my_selection_cb hides the box.
bool __cdecl OnPick(int box, int index, int choice) {
    if (box != g_box) return true; // always true: false would keep a forced kill from closing the box (0x4DC9A4)
    int menu = Fn<int __cdecl(int)>(site::BoxMenu)(box);
    g_rowH = Row(menu, 0), g_rowP = Row(menu, 1);
    bool ok = choice == 0 && g_rowH >= 0 && g_rowH < g_nHammers && g_rowP >= 0 && g_rowP < g_nPacks;
    Log("picker: answer choice %d index %d rows %d/%d", choice, index, g_rowH, g_rowP);
    g_result = ok ? 1 : -1, g_box = -1;
    return true;
}

} // namespace

// session.cpp Start(pick), guest, after Start's own refusals (gameplay with a Player, no mission or activity, connect= set,
// password length).
// Lists: the 16 Hammer_upgrade_data items and the backpack ids 0..9 that have a Backpack_info (an id without one would
// crash player::do_frame's create, 0x6D56C3-0x6D56D2), plus None; DLC items are left out (picker_equip.md §7).
void Picker_Open() {
    if (g_open) return Log("picker: F8 ignored, the box is up (Esc / B cancels)");
    uint8_t* me = LocalPlayer();
    uint8_t* slot3 = Fn<uint8_t* __cdecl(void*, int)>(site::InvItem)(me, 3);
    uint8_t* cur = slot3 ? Field<uint8_t*>(slot3, 8) : nullptr;
    int selH = 0;
    g_nHammers = 0;
    for (int i = 0; i < 16; i++) {
        const char* name = Field<const char*>(Rva(site::Hammers), 0xC * i + 4);
        uint8_t* inv = Fn<uint8_t* __cdecl(const char*)>(site::InvInfo)(name);
        if (!inv) continue;
        if (inv == cur) selH = g_nHammers;
        g_hammers[g_nHammers++] = {Key(inv), (uintptr_t)inv};
    }
    int type = Fn<int __cdecl()>(site::BackpackType)();
    g_packs[0] = {"MENU_NONE", (uintptr_t)kNone}; // localized "NONE"
    g_nPacks = 1, g_packSel = 0;
    for (int id = 0; id < 10; id++)
        if (uint8_t* info = Rva<uint8_t*>(site::Backpacks)[id]) {
            int up = id ? 0x34 + id : 0;
            if (up == type) g_packSel = g_nPacks;
            g_packs[g_nPacks++] = {(const char*)info + 0x24, (uintptr_t)up};
        }
    for (int i = 0; i < g_nHammers; i++) g_hKey[i] = g_hammers[i].key;
    for (int i = 0; i < g_nPacks; i++) g_pKey[i] = g_packs[i].key;
    Log("picker: %d hammers (carrying %s), %d backpacks + None (wearing type %02X)", g_nHammers, cur ? Key(cur) : "none",
        g_nPacks - 1, type);
    if (!g_nHammers) return Say("Co-op: the picker could not open (RfgCoop.log)");

    static char enc[0x300]; // ui_add_msgbox copies 0x300 bytes of it (0x4F6F51-0x4F6F62)
    // One line (<= 45 characters): more body or rows pokes out of the box's hexagon stencil (picker_layout.md §6)
    Fn<void __cdecl(char*, unsigned, const wchar_t*)>(site::EncodeWide)(enc, sizeof enc,
                                                                        L"Up, then Left/Right: change. CONNECT: join.");
    int box = Fn<int __cdecl(int, const char*, const char*, BoxCb, bool, bool, const char*, const char*, bool)>(site::AddMsgbox)(
        1 /*MBT_ACCEPT_CANCEL*/, "CO-OP LOADOUT", enc, OnPick, false, false, nullptr, nullptr, false); // pause_audio 0
    if (box < 0) {
        Log("picker: ui_add_msgbox refused (-1: a save/load box up, no UI doc, or pool full)");
        return Say("Co-op: the picker could not open (RfgCoop.log)");
    }
    // Missing rows still leave a box that cancels (Esc / B); OnPick refuses an accept without both values
    if (Fn<AddList>(site::AddCharList)(box, "HAMMER", g_hKey, g_nHammers) < 0) Log("picker: HAMMER row refused");
    if (Fn<AddList>(site::AddCharList)(box, "BACKPACK", g_pKey, g_nPacks) < 0) Log("picker: BACKPACK row refused");
    if (Fn<int __cdecl(int, const char*, bool, bool)>(site::AddCaption)(box, "CONNECT", false, false) < 0)
        Log("picker: CONNECT row refused");
    int menu = Fn<int __cdecl(int)>(site::BoxMenu)(box);
    Fn<bool __cdecl(int, int, int)>(site::SetCharList)(menu, 0, selH); // before the first populate pushes the values
    Fn<bool __cdecl(int, int, int)>(site::SetCharList)(menu, 1, g_packSel);
    Fn<void __cdecl(int, int)>(site::SetSelected)(menu, 2); // CONNECT: the select icon sits on the selected row and
                                                            // Enter works only there; Up reaches the two value rows
    g_box = box, g_result = -2, g_open = true;
    Log("picker: box %d up, %s + %s preselected", box, g_hKey[selH], g_pKey[g_packSel]);
}

// session.cpp Net_Tick, every tick. true once, on CONNECT: the pick is with guest.cpp and the caller starts the connection.
bool Picker_Tick() {
    if (!g_open) return false;
    if (g_result == -2) {
        if (Fn<uint32_t __cdecl(int)>(site::BoxActive)(g_box) & 0xFF) return false; // bool in al only
        Log("picker: box %d went without an answer", g_box); // torn down with no callback: as cancel
        g_result = -1;
    }
    g_open = false, g_box = -1;
    if (g_result < 0) {
        Say("Co-op: not connecting (picker cancelled)");
        return false;
    }
    Log("picker: %s + %s", g_hammers[g_rowH].key, g_packs[g_rowP].key);
    Guest_SetPick((void*)g_hammers[g_rowH].pick, (int)g_packs[g_rowP].pick);
    return true;
}
