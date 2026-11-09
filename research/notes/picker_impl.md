# Q17 picker: the guest's own hammer and backpack (src/picker.cpp, src/guest.cpp)

Rule Q17 (RESEARCH.md §8): before it connects, the guest picks any hammer and any backpack in the game (the lists
below hold the main campaign's; whether the DotB hammers count is open, see Lists).
The pick is applied to the guest's own player after every join and re-join and kept for the session. The host is not
affected. Nothing is saved (the guest's save guard stays on). Research: `findings/workflows/picker_overlay.md` (UI,
§C chosen) and `findings/workflows/picker_equip.md` (lists and apply). GOG rfg.exe, RVAs (the disassembly cache prints
VA = RVA + 0x400000). Every call below was re-read in the disassembly for this build. ver6.1 was tested in game (keys and pick
worked, the box layout broke); the ver6.2 box is not yet tested in game.

## ver6.2: one box (what changed)

The ver6.1 test (`vers/ver6.1/screens/1_picker_box.png`) showed the keys and the pick working, but the box broke: its
16 rows and 3-line body grew the box far past the 36 hexagon masks that stencil everything the box draws, so the
body, the first row and the CANCEL hint were clipped and the black showed as hexagons
(`findings/workflows/picker_layout.md` §2-§4). The judge chose the msgbox route: **one game message box,
HAMMER/BACKPACK char-list rows + CONNECT**, 3 rows and a 1-line body, which fits the stencil with margin at every
resolution (picker_layout.md §5, §6: box -140..144 vs the union's -161..171).

- `src/picker.cpp` only (plus comments in `features.h` and this note). `session.cpp`, `guest.cpp`, `core.cpp` and
  `build.cmd` are untouched; the interface (`Picker_Open`, `Picker_Tick`, `Guest_SetPick`) is the same.
- Deleted: box 2 (`OpenPackBox`, the `Step` enum), the bridge pause token (`game_pause` / `game_unpause` /
  `Game_paused_count` sites and log fields), and the localized body (`Name`, `localization_get_display_string_opt`):
  the body is now a fixed line.
- Added: three sites (below), `g_hKey[16]` / `g_pKey[11]` (the rows' value lists, static: the game keeps the pointer),
  `g_rowH` / `g_rowP`, `g_open`, and `Row()` (reads a row's value).

## What the player sees

1. The guest presses **F8** in gameplay, on its own save, not in a mission or activity, with `connect=` set (the
   same refusals as before, with the same HUD lines, the password length one included). Instead of dialling, one of
   the game's message boxes opens, dimmed over gameplay, and the game pauses.
2. **The box.** Title *CO-OP LOADOUT* on the top torn edge. Body: *"Up, then Left/Right: change. CONNECT: join."* (ver6.2: the box opens on CONNECT, where
   Left/Right does nothing, so the line names Up first; 43 characters, under the 45 limit)
   Rows:
   - **HAMMER** `<name>`: the 16 hammers by their in-game names, preselected to the hammer in slot 3;
   - **BACKPACK** `<name>`: NONE, then the 10 packs by their in-game names, preselected to the worn pack;
   - **CONNECT**.
   The cursor starts on CONNECT (ver6.2: the box's select icon always sits on the selected row, and Enter works only
   on CONNECT; Up reaches the value rows; Enter at once joins with the preselected loadout). Hint: CANCEL (adding a caption row clears the box's ACCEPT hint, as in ver6.1).
3. Keys are all the game's: Up / Down and the D-pad move between rows; Left / Right and the D-pad cycle a row's value
   with wrap; Enter / A on CONNECT accepts (on a value row it does nothing); Esc / B cancels. F8 while the box is up
   is logged and ignored. To change the pick: F8 (stop), F8 again (new picker).
4. **CONNECT**: the pick is stored and the session starts ("Co-op: connecting to ..."). **Cancel**: nothing starts,
   HUD "Co-op: not connecting (picker cancelled)".

The host presses F8 as before (no picker). `autoconnect=1` (a test option) starts without a picker: no pick is
applied unless one was confirmed earlier in the same process.

## Code and hooks

| Where | What |
|---|---|
| `src/picker.cpp` | `Picker_Open()`: builds both lists and the key arrays, opens the box, adds the 3 rows, preselects. `Picker_Tick()`: one step, reads the answer and hands the pick to `Guest_SetPick`. `OnPick`: the box callback, reads both rows' values. |
| `src/session.cpp` (ver6.1, unchanged) | `Start(bool pick = false)`: the `connect=` check, the host's salt and the password length check sit next to the other refusals; `if (pick)` (zero the password, `return Picker_Open();`) right after them, so the picker never opens when its confirm's `Start()` would refuse. `Net_Tick`: F8 calls `Start(g_ini.guest)`; one line `if (Picker_Tick() && g_phase == Off) Start();` |
| `src/guest.cpp` (ver6.1, unchanged) | `Guest_SetPick`, `PickTable`, `ApplyPick`; `g_pickDue` set in `Guest_Tick`'s existing `g_loaded` block; `ApplyPick` runs below it on the first frame on foot; `ApplyLoadout` calls `PickTable` right after its `memcpy`. |
| `src/features.h` | `Picker_Open`, `Picker_Tick`, `Guest_SetPick` |

No hooks or patched bytes, no core.cpp, XInput export or build.cmd change. Everything runs on the sim thread
(`Net_Tick` and the `ui_process` callback): no render-thread, WndProc or XInput code.

## UI: one game message box with char-list rows (picker_layout.md §8)

All `__cdecl`, caller cleans, sim thread.

| RVA | Function | Checked in the disassembly |
|---|---|---|
| 0x4F6C70 | `int ui_add_msgbox(int type, char* title, char* text, cb, bool, bool, char* accept, char* cancel, bool pause_audio)` | args: type ebx = [esp+0x44] after 3 pushes; title → `ui_menu_add` (0x4F6F85), copied into `ui_menu::m_title` char[192]; text copied 0x300 bytes into info+0x6C (0x4F6F51-0x4F6F62); cb → info+0x64 (0x4F6E01); pause: only box (pool count ≤ 1, 0x4F6E0A), not MP, GS 1 active → `game_pause(pause_audio)` 0x4F6E8D + `Paused_game` 0x2C071AD = 1. Returns the handle (`movsx byte [info]`, 0x4F7058) or -1 (no utility doc 0x4F6CCE; save/load box up and arg6 = 0, 0x4F6CDA; pool 0x4F6DB6; menu 0x4F706C). Called as `(1, "CO-OP LOADOUT", enc, OnPick, 0, 0, 0, 0, 0)` |
| 0x4CC780 | `int ui_msgbox_add_char_list_item(int box, char* caption, char** list, int count)` | **new.** box [esp+4] → msgbox info (0x4CC330, NULL → -1); caption/list/count [esp+8/C/10] (read at [esp+0x10/14/18] after 2 pushes, 0x4CC798-0x4CC7A0) → `ui_menu_add_char_list_item(info+4, caption, list, count, 0x4A1B40, 0, 1)` (0x4CC7B4, `add esp,0x1C`); returns its handle (edi) or -1, booked into info+0xC[info+0x14++] under the cap info+0x10 (0x4CC7C4-0x4CC7F2). `ret` (caller cleans). Down the chain: `ui_menu::add_char_list_item` 0x4B77C0 (16-item cap, item stored inline at menu+0x24[menu+0x20++], 0x4B7803) → item init 0x4A0DB0: +0xC = 1 (char list), +0x94 m_which_char = the initial index (0 here), +0x98 count, **+0x9C = the list pointer, kept, not copied**, +0xA4 change cb. Hence static `g_hKey` / `g_pKey` |
| 0x4B93D0 | `bool ui_menu_set_char_list_index(int menu, int item_index, int value)` | **new.** menu [esp+0xC] after 2 pushes (0x4B93D8) matched against `Menus[i]+0x10`; item_index [esp+0x14] and value [esp+0x18] after 3 pushes; item = `menu+0x24[item_index]` if 0 ≤ index < `menu+0x20`; writes `item+0x94` only if `item+0xC == 1` and 0 ≤ value < `item+0x98` (0x4B93FF-0x4B942A). Always returns 0 (`xor al, al`), so the result is ignored. Called right after the adds, before the first `process_populate_items` |
| 0x4B7730 | `ui_menu* ui_menu_get_via_handle(int menu)` | **new.** menu [esp+0xC] after 2 pushes; linear search of `Menus` (RVA 0x126669C, count RVA 0x12666A4) by `+0x10`; returns the menu or 0 (0x4B7743-0x4B775C) |
| 0x4F06E0 | `int ui_msgbox_add_caption_item(int box, char* text, bool, bool)` | → `ui_menu_add_function_item(menu, text, my_selection_cb 0x4F0300, 1, a3, a4)` (0x4F0717); for type 1 clears the accept hint (0x4F0782-0x4F0793). Used for CONNECT, the only type-0 row |
| 0x4CC800 | `int ui_msgbox_get_menu_handle(int box)` | info+4, -1 if no such box |
| 0x4B9490 | `void ui_menu_set_selected_item(int menu, int index)` | difficulty box 0x4F7930-0x4F7932 `push idx; push menu`. Writes `menu+0x1C` only if 0 ≤ index < `menu+0x20` (0x4B94BF-0x4B94CC), so a refused row leaves the default. Row 2 (CONNECT): the highlight's select icon (msgbox field 4, control 0x49 at 0x4DCD6C) follows the selected row whatever its type |
| 0x4CC4D0 | `bool ui_is_msgbox_active(int box)` | returns in `al` only (`mov al, bl`): read as `& 0xFF`. -1 → 0; no utility doc → 0 |
| 0x065250 | `void vint_encode_wide_text(char* out, unsigned size, wchar_t* in)` | 0x80, then 3 bytes per wchar, then `08 00`; stops at out+size-1. The body is 43 characters (one 412-unit line holds about 45) |
| 0x455D30 | `hud_message_add(char*, float, void*, void*, int*)` | as session.cpp / guest.cpp (cancel and failure lines) |

**Input** (`ui_menu::process_input` 0x4DBF80, per visible, enabled item; the selected one acts):
- Select (Enter / A) acts only on a type-0 row (0x4DC1F1-0x4DC28B: the item's vtable+8, for CONNECT `my_selection_cb`).
  A char-list row (type 1) takes the 0x4DC290 branch, which handles only Left / Right (`handle_input`, vtable+8,
  +1 / -1 with wrap, 0x4B7DC0). After the loop, the menu-level select callback (`menu+0x124`, 0x4DC3CA) runs only
  when no type-0 row exists ([esp+0x14], set at 0x4DC1DD); CONNECT is one, so Enter on a value row does nothing.

**Callback** `bool __cdecl OnPick(int box, int index, int choice)`:
- Enter / A on CONNECT: `my_selection_cb` 0x4F0300 → `cb(box, item, 0)`, then `ui_hide_msgbox(box, 0)`.
- Esc / B: the abort hint `my_selection_hint_cb` 0x4F03FD → `cb(box, selected, 1)`, then hide.
- `ui_hide_msgbox(-2, …)` (the game closing all boxes) → `ui_kill_msgbox` 0x4DC998 → `cb(box, 0, 2)`; false refuses
  the kill (0x4DC9A4). The callback runs before the menu handle goes to 0x4CBDE0 (0x4DC9EE-0x4DC9F2), so the rows are
  still readable.

OnPick reads both rows' `m_which_char` first (`Row()`: `ui_menu_get_via_handle(ui_msgbox_get_menu_handle(box))`,
then `menu+0x24[i]`, `+0xC == 1`, `+0x94`, the reads `ui_menu_set_char_list_index` makes), logs them, and answers
1 (CONNECT) only for choice 0 with both values in range, else -1. It always returns true.

**Pause.** One box, one token: `ui_add_msgbox` calls `game_pause` (only box in the pool) and `ui_kill_msgbox` calls
`game_unpause` when the last box goes (0x4DCA0E-0x4DCA46). No token of ours (ver6.1's bridge is gone). While paused,
`hud_process` still runs `ui_process` before the paused early-out (0x3EDE5B), so the box keeps taking input; the
player, the Esc menu (0x4DDA4E) and the map (0x44DE1A) do not. `pause_audio` = 0, as the game's in-gameplay boxes.

**Backstop.** If the box is no longer active and never answered (torn down with no callback), Picker_Tick treats it
as cancel.

## Lists (built at F8, sim thread)

- **Hammers.** `Hammer_upgrade_data` 0xF10058: 16 × `{int upgrade; char* inv_item_name; bool special}` (0xC each, ends
  at 0xF10118; `meta_data_get_hammer_upgrade_weapon_name` 0x611D10 reads +4). For each, `inv_item_get_info(name)`
  (0x612240, NULL → left out). Caption = `inv_item_info+8` display key (`WEAP_SLEDGE`, `WEAP_SLEDGE_UPG_1..14`,
  `MENU_HAMMER_UNLOCK`; PDB `inv_item_info`: +0 name, +8 display_name), else the name. Preselected = the info of
  `human_get_inventory_item(Player, 3)` (0x607820).
- **Backpacks.** "NONE" (`MENU_NONE`, en_us "NONE"; upgrade 0x3E) first, then ids 0..9 with a non-NULL
  `Backpack_info[id]` (0x2BE7D18, `multi_backpack_info*[16]`; `parse_backpacks_table` stores the id at +0x50
  (0x5AFDA3), the display key at +0x24 (`multi_game_backpack_get_display_name` 0x558F10)). Upgrade = 0 for id 0
  (jetpack), else 0x34 + id. An id with a NULL entry is never offered: `player::do_frame` would call
  `event_player_picked_up` on NULL (0x6D56C3-0x6D56D2). Preselected = `upgrade_get_backpack_type()` (0x375CE0).
- DLC items (DotB pickaxe, mar_dlc_sledge, gutter, WC packs, kaboom) are left out (picker_equip.md §7). **Open, for the
  user:** Q17 says "any hammer in the game", and DotB content loaded into the campaign brings `mar_dlc_sledge` (class 7
  melee_sledgehammer, caption WEAP_SLEDGE like the default) and `pickaxe` (class 6 melee) into the campaign's
  `Inv_item_info`. If they count: add them in Picker_Open after the table loop when `inv_item_get_info` finds them
  (own caption, e.g. "Marauder Sledgehammer"; g_hammers / g_hKey [18]), store the name and look it up again in ApplyPick, and
  note that the host's puppet shows it only when the host loads that DotB content too. If not: record "DotB items
  excluded" in RESEARCH.md Q17. The backpack side is settled: SP expresses only ids 0..9 (upgrades 0, 0x35..0x3D).
- The row values are these keys (`g_hKey` / `g_pKey`, pointers into `inv_item_info` / `Backpack_info` memory, stable
  while the box lives): the menu shows them localized, as ver6.1's caption rows did. `localization_get_display_string_opt`
  is no longer called (ver6.2's body is a fixed line).

## Apply (guest.cpp, sim thread; picker_equip.md §9)

`Guest_SetPick(inv_item_info* hammer, int pack)` keeps the pick for the process (the next confirmed pick replaces it).

**Once per load** (join, re-join, the warp video's in-RAM reload: every load an armed guest goes through passes
GS_LOAD, which sets `g_loaded`): the `g_loaded` block sets `g_pickDue`; below it, `ApplyPick(me)` runs on the first
frame with `human_in_vehicle(me)` (0x653870) NULL.

| Step | Calls |
|---|---|
| hammer | `old = human_get_inventory_item(me, 3)`; if `old+8 != pick`: `held = me+0xC90 == old` (equipped item, `human::get_equipped_inv_item` 0x607160); `human_delete_item_from_inventory(me, old, 0, 0)` (0x6C76A0; re-equips +0xA34 when it was equipped, 0x6C76FD-0x6C7718); `human_add_item_to_inventory(me, pick, 1, 99999, 3, 0, 0, 0)` (0x6B4E20); if held, `player_equip_item(me, item, NULL, 0)` (0x6C2770) |
| backpack table | `PickTable(me+0x2978)`: level byte at +0x18+8·u. Jetpack (u 0) = 2 if picked (levels 0/1/2 in upgrades.xtbl; 2 = with recharge), else 0; u 0x35..0x3D = 1 for the picked one, else 0 (the shop's writes, purchase_upgrade 0x6C3D9C, 0x6C3DF5-0x6C3E5A). `get_upgrade_value` 0x611760 reads exactly this byte |
| backpack object | attached = `me+0x1170`, its id = `[[+0x270]+0x50]`. Pack picked: `s_lastBackpackIndex` (0x1293490) = id if the attached one already is it, else -1; `player::do_frame` (0x6D5694-0x6D56D7) then creates it next frame and `event_player_picked_up` drops the old one. None: `event_player_dropped(true)` (0x5DF1C0, `__thiscall`, `ret 4`; detaches, `player+0x1170 = 0`, `world::destroy_object`), the call `unused_dcf_remove_backpack` 0x5F10A0 makes |

**After every LOADOUT** (Q13): `ApplyLoadout` copies the host's table, then `PickTable(meta)` in the same call, so
`do_frame` never sees the host's pack. The hammer is not touched there (LOADOUT has no inventory).

**Stop (F8)** keeps the applied pick on the current player; nothing is restored or saved.

Deviations from picker_equip.md §9, with the reason:
- Ammo 99999 instead of -1 in the add call: the exact arguments of puppet.cpp (meaningless for a melee item either way).
- The whole ApplyPick (backpack too) waits for "on foot": one gate, and a load always spawns on foot.
- `g_pickDue` is set only when a pick exists (an armed guest with no pick keeps the host's items, as ver6).

## Log lines (ver6.2)

```
picker: 16 hammers (carrying WEAP_SLEDGE_UPG_1), 10 backpacks + None (wearing type 3E)
picker: box 0 up, WEAP_SLEDGE_UPG_1 + MENU_NONE preselected
picker: answer choice 0 index 2 rows h/p           <- list indices
picker: WEAP_SLEDGE_UPG_5 + HM_BACKPACK_RHINO
guest: Q17 pick: hammer sledge_upgrade_5, backpack upgrade 3C (applied after every join and re-join)
session: HUD "Co-op: connecting to ..."
... join ...
guest: Q17 hammer <host's> -> sledge_upgrade_5 (in hand)
guest: Q17 backpack upgrade 3C: ...
```
Cancel: `picker: answer choice 1 index N rows a/b` + `picker: HUD "Co-op: not connecting (picker cancelled)"`.
A box gone without an answer: `picker: box N went without an answer`. Refused: `picker: ui_add_msgbox refused ...`;
a row refused: `picker: HAMMER row refused` (the box still cancels; CONNECT then answers cancel).

## In-game checks (ver6.2; B = guest, one PC)

1. F8 shows one box: a clean torn band with no hexagon outlines, the title CO-OP LOADOUT on the top edge, the body
   line fully readable, 3 rows, and a whole CANCEL hint.
2. HAMMER and BACKPACK open showing the current hammer and pack. Left / Right and the D-pad cycle all 16 hammers and
   NONE + 10 packs, with wrap, and the values are localized names (SLEDGEHAMMER ... OSTRICH, NONE, JETPACK ...), not
   keys.
3. Enter / A on CONNECT: the log shows both keys, then connecting. Enter on a value row does nothing. Esc / B: the
   cancel HUD line.
4. No key or pad press leaks into gameplay (Esc does not open the pause menu, A does not jump, B does not detonate),
   and the game unpauses after the box.
5. Same look at a second resolution (e.g. 1280x800 windowed and native).
6. As ver6.1 (unchanged code): after the join the picked hammer in slot 3 and the picked pack on the back; the pick
   survives a host LOADOUT and every re-join.

**Fallback, only if check 2 fails** (values do not cycle or show wrong): keep the ver6.1 two-box chain and page it
like the difficulty box: raw title "CO-OP HAMMER n/6" (packs "n/4"), empty body, 3 choices + a "MORE" caption that
reopens the next page (the last page wraps), with ver6.1's bridge `game_pause` / `game_unpause` token
(picker_layout.md §8.3).

## Open risks

- Char-list rows in a message box have no shipped caller (the options pages use them in other docs). The C++ side is
  generic (`ui_menu`); the msgbox Lua is read to show `item_value`, not seen in game. Fallback above.
- The value text renders at font scale 1.0 next to the 0.8 caption (cosmetic); the longest name ("GOLD BREAKER
  ALPHA", ~231 units) should stay clear of the caption.
- Preselect: `ui_menu_set_char_list_index` writes `+0x94` without marking the item dirty; it runs before the first
  populate, so the first push should carry it [inferred].
- Raw ASCII title and captions (CO-OP LOADOUT, HAMMER, BACKPACK, CONNECT): expected to render as typed, like the
  language box's raw caption [inferred for plain words]. If one shows wrong, use keys (`MENU_ACCEPT`, ...).
- Mouse: clicking CONNECT should accept; clicking a value row only selects it [not checked].
- The character-mesh hammers (sledge_upgrade_8 SKULLDIGGER, sledge_upgrade_14 OSTRICH) and the jetpack at level 2
  (recharge) are untested as guest items.
- Alt-tab before the session (quick pause not yet off) pushes GS_QUICK_PAUSE over the box; the box should wait, or a
  game teardown answers FORCED_EXIT (cancel).
