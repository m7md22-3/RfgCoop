# RFG Coop

Online co-op through the main Red Faction Guerrilla Re-Mars-tered campaign (Tharsis): two players in one world,
seeing each other, driving, fighting, destroying and doing missions together.

Status: **no longer developed by the author.** It is left to the community; forks are welcome. The last build is
ver7.2 (shared world, partner puppets, missions, destruction sync) and it was never tested in game. See MODLOG.md
and the known issues below.

- Target: the GOG build (32-bit rfg.exe + rfg.pdb).
- Build: `build.cmd` with llvm-mingw (set `CXX` to its i686 clang++). Install: copy `build\xinput1_3.dll` and
  `RfgCoop.ini` next to rfg.exe; the ini explains every key.
- `research/RESEARCH.md`: the main report (feasibility, architecture, roadmap).
- `research/notes/*.md`: verified facts per domain.
- Networking: [ENet](http://enet.bespin.org/) (MIT, `third_party/enet`).

## Known issues

- **Security: values from the partner that aren't range-checked.** Only a connected, password-authenticated partner can
  send them, and normal clients never do. Each issue has the details and a suggested fix:
  - [#1](https://github.com/m7md22-3/RfgCoop/issues/1): on replay the guest accepts negative debris values from the
    host's destruction stream (`OnRfgl`, `src/destruction.cpp`).
  - [#2](https://github.com/m7md22-3/RfgCoop/issues/2): the partner's weapon slot reaches the game's inventory calls
    unchecked (`Effects` / `GiveWeapon`, `src/puppet.cpp`).
- **Debris:** holes and destruction sync on both screens (ver7.1), but loose debris pieces still differ between the two
  games.
- **Untested build:** ver7.2 (safehouse repair on both, gas tanks, host re-join log) has never run in game. The last
  two-PC test was ver6, over a virtual LAN.
- **Official multiplayer:** "Find game" fails while RfgCoop is installed. Remove `xinput1_3.dll` to play online.
- **No prebuilt DLL:** build it with `build.cmd`.
