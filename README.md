# RFG Coop

Online co-op through the main Red Faction Guerrilla Re-Mars-tered campaign (Tharsis): two players in one world,
seeing each other, driving, fighting, destroying and doing missions together.

Status: in development (ver7.2: shared world, partner puppets, missions, destruction sync). See MODLOG.md.

- Target: the GOG build (32-bit rfg.exe + rfg.pdb).
- Build: `build.cmd` with llvm-mingw (set `CXX` to its i686 clang++). Install: copy `build\xinput1_3.dll` and
  `RfgCoop.ini` next to rfg.exe; the ini explains every key.
- `research/RESEARCH.md`: the main report (feasibility, architecture, roadmap).
- `research/notes/*.md`: verified facts per domain.
- Networking: [ENet](http://enet.bespin.org/) (MIT, `third_party/enet`).
