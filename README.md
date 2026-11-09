# RFG Coop

Online co-op through the main Red Faction Guerrilla Re-Mars-tered campaign (Tharsis). The goal: two players in one
world, seeing each other, driving, fighting, destroying and doing missions together.

Status: **no longer developed by the author.** It is left to the community; forks are welcome. The last build is
ver7.2 (shared world, partner puppets, destruction sync; missions don't sync yet) and it was never tested in game.
See MODLOG.md and the known issues below.

- Target: the GOG build (32-bit rfg.exe).
- Download: [Releases](https://github.com/m7md22-3/RfgCoop/releases) (prebuilt `xinput1_3.dll` plus host and guest inis). Install and play: see
  [How to use](#how-to-use). `RfgCoop.ini` explains every key.
- `research/RESEARCH.md`: the main report (feasibility, architecture, roadmap).
- `research/notes/*.md`: verified facts per domain.
- Networking: [ENet](http://enet.bespin.org/) (MIT, `third_party/enet`).
- License: MIT, see `LICENSE`.

## How to use

The join, re-joins and F3 last ran in game in ver7.1 (one PC). The guest's refusals and the only two-PC test were in
ver6. ver7.2 builds on that and is untested.

### What you need (both players)

- The GOG build of Re-Mars-tered (32-bit `rfg.exe`). With any other `rfg.exe` the mod switches itself off (the log
  says `rfg.exe build not recognised`) and the game runs as normal. You don't need `rfg.pdb`; the mod never reads it.
- The same game data on both PCs: the same `data\*.vpp_pc` files (same names and sizes). Also the same `winmm.dll` /
  `dinput8.dll` in the game folder: identical files on both, or none on both.
- The exact same `xinput1_3.dll` file on both PCs. Both games compare its bytes, and every build comes out different,
  so both players download the same release zip. If you build it yourself, send your file to your partner: two
  separate builds are refused (`different RfgCoop build`).
- Host: a main-campaign save that has finished the Tutorial (a DLC level fails this check).
- Guest: a save that loads into free roam. A new game starts inside the Tutorial mission, which the guest's F8 refuses,
  so play past it first. The host's world replaces yours. From the link until you restart, your game writes no saves
  and no option changes.
- A keyboard. F8, F7 and F3 have no gamepad binding.
- Between two PCs, the guest must reach the host's UDP port. A virtual LAN such as Tailscale, ZeroTier or Radmin works
  (the ver6 two-PC test used Radmin). A port forwarded on the host's router should also work, but that is untested.
  Game traffic is not encrypted, so play only with people you trust. Tailscale and ZeroTier encrypt the link.

### Install (both players)

1. Both players download the same `RfgCoop-ver7.2.zip` from [Releases](https://github.com/m7md22-3/RfgCoop/releases) and unzip it.
2. Host: copy `xinput1_3.dll` and `Host\RfgCoop.ini` into the folder that holds `rfg.exe`. Guest: copy
   `xinput1_3.dll` and `Guest\RfgCoop.ini` there, then set `connect=` to the host's address (see Guest setup).
3. Start the game. `RfgCoop.log` in that folder should say `build OK` and `session: role host` (or `guest`). In
   windowed mode, the window title gains `[RfgCoop <tag>]`. If there is no log at all, the DLL isn't loaded: check the
   name `xinput1_3.dll` and that it sits beside `rfg.exe`.

### Build from source (modders)

Only needed if you change the code. Players use the release.

1. Get llvm-mingw for Windows from [its releases](https://github.com/mstorsjo/llvm-mingw/releases): the
   `ucrt-x86_64` zip (it also contains the 32-bit compilers). Make a `tools` folder next to `build.cmd` and unzip it
   there, so you get `tools\llvm-mingw-<version>-ucrt-x86_64\bin\i686-w64-mingw32-clang++.exe`. There's no need to
   rename it. Or set `CXX` to the full path of that exe (no quotes) in the cmd window you build from.
2. Run `build.cmd` (double-clicking works: the window stays open at the end). It prints the compiler it found and
   ends with `Built build\xinput1_3.dll`. If it says `Compiler not found`, step 1 isn't done yet. If it stops at a
   Windows header with `file not found` (e.g. `joystickapi.h`), the llvm-mingw unzip is incomplete: extract it again.
3. Install `build\xinput1_3.dll` as in Install, with the inis from `vers\ver7.2\TwoPCs\Host` and `Guest` (the same as
   the release zip's). Send that same DLL to your partner: a build of their own won't match yours.

### Host setup

All keys live in `[coop]` and are read when the game starts, so restart the game after changing them. Only `password`
is read again at every F8.

```ini
[coop]
tag=Host
role=host
port=27777
; same on both games, at least 10 characters, or empty on both (then anyone who can reach the port can join)
password=
name=YourName
```

- `port`: the UDP port the host listens on (default 27777). Only the host needs it reachable. If Windows Firewall
  asks, allow the game.
- `name`: what the partner sees in its HUD lines and window title (empty = `tag`). Up to 23 plain ASCII characters;
  other characters show as `?`.
- Leave the `destr_*` keys at their defaults. For `destr_sync`, `destr_profile`, `destr_ownpack`, `destr_gm` and
  `destr_autoresync` only the host's values count.

### Guest setup

```ini
[coop]
tag=Guest
role=guest
; the host's address and port: its virtual-LAN address (shown in the host's Tailscale / ZeroTier / Radmin app) or a
; host name; 127.0.0.1:27777 for two games on one PC
connect=HOST-ADDRESS:27777
password=
name=YourName
```

The release zip's `Host` and `Guest` folders (also `vers/ver7.2/TwoPCs/`) hold ready-made inis for this. `autoconnect=1` tries to start
the session once, by itself, on the first gameplay frame. If that is refused, press F8 later. On a guest, autoconnect
skips the picker, so you keep the host's hammer and backpack.

### Playing

Play windowed or borderless on both PCs. Alt-tab never pauses a game while a session runs, but a minimized game stops
running and stalls its partner (from the author's notes; alt-tab from exclusive fullscreen probably minimizes).

1. Both players load a save and get into gameplay. F8 does nothing at the main menu (the title then says
   `co-op off (F8 in gameplay, on your save)`).
2. The host presses F8: `Co-op: hosting on UDP port 27777`. The host may start during a mission. The guest's join then
   waits until the host is in free roam (`Co-op: you join after the host's mission`).
3. The guest, in free roam (no mission, no activity except a Convoy), presses F8. The `CO-OP LOADOUT` box opens.
4. In the box, press Up to reach HAMMER and BACKPACK, and Left/Right to change them. The lists hold every base-game
   hammer and backpack, unlocked or not (DLC items are left out). Enter or A on CONNECT joins. The cursor starts on
   CONNECT, so Enter at once keeps your current hammer and backpack (if they are in the lists). Esc or B cancels.
5. The guest dials every 3 s (`Co-op: connecting to ...`). When linked, both see `Co-op: <name> connected`, and the
   guest sees `Save guard ON`.
6. The host's game pauses (`Partner joining...`) while it sends its world. The guest loads it, lands 2 m beside the
   host and sees `Co-op: joined <host>`. Then the host unpauses. The host pauses the same way (`Partner re-joining...`)
   at every re-join: the guest's death or F3, and every load of the host's own. If a join never lands, the host
   unpauses after 120 s (`<name> did not finish joining`).

The pick lasts until the game exits and comes back after every re-join. To change it, press F8 to stop, then F8
again. That starts a new session, so your slots 0-2 go back to the host's weapons. The window title shows the state,
e.g. `host, listening on 27777` or `guest, partner <name>, RTT 58 ms`.

| Key | Who | What |
|---|---|---|
| F8 | Both | Start the session (in gameplay) or stop it (any time). On the guest, starting opens the `CO-OP LOADOUT` box first. |
| F7 | Both, linked | Teleport 2 m beside the partner (your vehicle comes along). |
| F3 | Guest, linked | Reload the host's world, as at the join: the guest loads and the host pauses meanwhile (host in free roam only, once per 5 s). |

Keys work only while the game window has focus.

Other HUD lines you will see:
- `Destruction check: ...`: the two games compare their buildings.
- `World drifted ...`: the guest may press F3 while the host is in free roam.
- On the guest, `Save guard: disk write #N skipped`: an autosave was blocked.

The guest, from the link until the game exits:

- can't save, fast travel, use Esc -> Load / Restart / Cancel mission, buy upgrades, start missions, or start
  activities other than Convoys. Each refusal shows a HUD line, e.g. `Only the host can start missions`. Use F7
  instead of fast travel.
- collects salvage into the host's wallet. It sees the host's total and can't spend it.
- gets the host's upgrades and weapon lockers. Each session's first join also gives it the host's weapons. From then
  on, its weapons in slots 0-2 (with ammo) are kept across re-joins.
- re-joins the host's world on death -> Continue instead of reloading.
- needs a game restart to play its own campaign again, even after the session ends.

What syncs:

- Players, both ways: position, movement, aim, weapon, backpack and its effects. The partner shows as a second Mason
  with a green minimap star (`partner_icon=1`, the default). It neither takes nor deals damage to people, but its shots
  still break buildings, gas tanks and props.
- The world: the guest gets the host's world at the join and again after every host load (fast travel, death,
  retry, checkpoint, Esc -> Load). The guest sees `Host loading...`, then `Co-op: re-joined <host>`.
- Destruction: holes, chunks and debris clean-up follow the host. Loose debris pieces still differ.
- Pause: when the host pauses (Esc, map, menus, videos, death screen), the guest pauses too (`Host paused`). The
  guest's pause doesn't pause the host.

What doesn't sync:

- Missions. Only the host plays them, and only the host's save keeps progress. The guest runs no mission or script
  code, so it sees no objectives or scripted events. A guest that re-joins during a host mission may see
  `Your world misses the host's changes...`. It re-syncs when the mission ends.
- Vehicles: the partner's puppet hides while it drives, but the star still shows where it is.
- NPCs: each game runs its own.
- Time of day after the join, and district control.

Ending: F8 stops the session. If the guest stops, the host goes back to listening. If the host stops or drops, the
guest shows `gone, reconnecting` and keeps dialling until it presses F8. A guest that quits to the main menu ends the
session.

### Two games on one PC (for testing)

1. Give each game its own folder with its own `xinput1_3.dll` and `RfgCoop.ini` (the log is written next to the DLL).
   Start each game by running its own `rfg.exe`, the host first; GOG Galaxy isn't needed. Whether a plain copy of the
   game folder runs as a second game is unconfirmed: the author's second game was a separate install.
2. Use the inis in `vers/ver7.2/A` (host) and `vers/ver7.2/B` (guest). They set:
   - `dev=1` on both, so alt-tab doesn't pause the unfocused game and the gamepad drives only the focused window;
   - `connect=127.0.0.1:27777` and `save_tag=1` on B;
   - frame caps;
   - window positions: A at 0,0 and B at 1280,0, each 1280x800. Change `window=` to fit your screen, and set the game
     to windowed mode in its options;
   - `lag_ms=30` for a test card. Set it to 0 on both.
3. With `save_tag=1`, B saves to and lists only `kee2_savegame_*.sav`, so it starts with no save. Back up
   `keen_savegame_0_0.sav`, then copy it to `kee2_savegame_0_0.sav` once, in the save folder both games share. On a GOG
   Galaxy install that is `%LOCALAPPDATA%\GOG.com\Galaxy\Applications\51153410217180642\Storage\Shared\Files\autocloud\save\`.
4. `dev=1` also turns on experiment keys (F5, F6, F9-F12). Don't press them.

Two PCs never need `dev=1`.

### Troubleshooting

The session, guest-rule and picker HUD lines are also written to `RfgCoop.log` in the game folder. The log is emptied
at each game start; the previous run is kept as `RfgCoop.prev.log`. When a session first starts (on the guest: at
CONNECT in the box), each game logs `session: handshake values: ...`. Compare that line in both logs to find a
mismatch.

- `build.cmd` says `Compiler not found`: llvm-mingw isn't in `tools\` (see Build from source) and `CXX` isn't set.
- F8 does nothing: you are not in gameplay on a loaded save.
- `set role=host or role=guest` / `set connect=<ip:port>`: fix the ini and restart the game.
- `Co-op needs a save that has finished the Tutorial` (or `session stopped: the host's save has not finished the
  Tutorial`): the host loads a main-campaign save past the Tutorial.
- `finish or cancel your mission before joining` (or `activity`): the guest gets to free roam first.
- `password too short`: use 10 or more characters, or none on both. Press F8 again (no restart needed).
- `wrong password` / `a password is set on the host only` (or `guest only`): make both the same. The guest then
  presses F8 again. A host that changed its own password presses F8 twice (stop, start). After 5 wrong tries the host
  locks that address out for 60 s (`too many wrong passwords`).
- `no answer from <ip>:<port> yet`: check `connect=`, that the host shows `hosting`, the firewall or virtual LAN, and
  that both run the same `xinput1_3.dll`.
- `cannot resolve`: the address in `connect=` is wrong. Fix it and restart the game.
- `UDP port <n> is busy`: another program uses the port. Change `port=` on the host and the port in the guest's
  `connect=`, then restart.
- `different RfgCoop build`, `RfgCoop protocol ...`, `different winmm.dll mod`, `different dinput8.dll mod` (0 = none)
  or `different game data`: make those files match on both (or absent on both, for the two mod DLLs), then restart
  both games. The values are read once per run.
- `handshake timed out`: no answer for about 30 s. Either side drops that link: the host keeps listening and the
  guest redials.
- `F7: no position from <name> yet`: the partner is loading; try again. `F7: can't teleport right now`: leave the
  mounted turret.
- `F3 resync refused: resync after the host's mission`: try again once the host is in free roam. The other refusals,
  `a world is already on its way` and `one came less than 2.5 s ago`, clear by themselves.
- `the network thread did not start`: wait a moment after a stop and press F8 again.
- `the picker could not open`, `the save guard could not be armed`, `guest fences unavailable`,
  `Partner puppet failed`: a check on the game's code failed; the details are in `RfgCoop.log`. If the log says
  `unexpected bytes` or `a sibling chained it?`, another mod DLL in the game folder patched the same spot: try without
  it. The picker also fails while a save/load box is up: close it and press F8 again.
- No gamepad, and the log says `system XInput not found`: the mod forwards the pad to `XInput1_3.dll` in Windows'
  system folder (SysWOW64). The DirectX End-User Runtime (June 2010) should provide it (untested).

Uninstall: delete `xinput1_3.dll`, `RfgCoop.ini`, `RfgCoop.log`, `RfgCoop.prev.log` and any `RfgCoop_objects_*.txt`
from the game folder. The mod patches the game only in memory, so no game file needs restoring. `kee2_savegame_*.sav`
files from a `save_tag=1` copy stay behind.

## Known issues

- **Security: values from the partner that aren't range-checked.** Only a connected, password-authenticated partner can
  send them, and normal clients never do. Each issue has the details and a suggested fix:
  - [#1](https://github.com/m7md22-3/RfgCoop/issues/1): on replay the guest accepts negative debris values from the
    host's destruction stream (`OnRfgl`, `src/destruction.cpp`).
  - [#2](https://github.com/m7md22-3/RfgCoop/issues/2): the partner's weapon slot reaches the game's inventory calls
    unchecked (`Effects` / `GiveWeapon`, `src/puppet.cpp`).
- **Missions:** they don't sync yet.
- **Debris:** holes and destruction sync on both screens (ver7.1), but loose debris pieces still differ between the two
  games.
- **Untested build:** ver7.2 (safehouse repair on both, gas tanks, host re-join log) has never run in game. The last
  two-PC test was ver6, over a virtual LAN.
- **Official multiplayer:** "Find game" fails while RfgCoop is installed. Remove `xinput1_3.dll` to play online.
