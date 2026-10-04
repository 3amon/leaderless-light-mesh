# Hardware and design-file licence

Copyright (c) 2026 Eamon White <white.eamon@gmail.com>

The hardware design files in this repository are licensed under the
**Creative Commons Attribution 4.0 International** licence (CC BY 4.0):
<https://creativecommons.org/licenses/by/4.0/>

This covers:

- `hardware/` — the KiCad schematic, board, project file, gerbers, drill
  files, BOM, placement file, the generated module stand-in model and the
  board renders;
- `enclosure/` — the FreeCAD documents, STEP and STL files, diffuser-cap
  pattern SVGs, contact sheets, renders, the photograph, and the dimension
  and print notes.

You may share and adapt these files for any purpose, including commercially,
provided you give appropriate credit, link to the licence, and indicate if
changes were made.

Notes:

- The FreeCAD documents carry FreeCAD's default embedded licence string
  ("All rights reserved") in their document properties. This file, not that
  default string, states the licence.
- The KiCad board references footprints and 3D models from the stock KiCad
  libraries by path (`${KICAD10_3DMODEL_DIR}/...`); those libraries are not
  redistributed here and keep their own licence (CC BY-SA 4.0 with the KiCad
  library exception).
- `hardware/fab_export.py` and `enclosure/node-enclosure/cad-scripts/*.py`
  are programs and are covered by the MIT licence in `LICENSE`.
- Everything else in the repository (firmware, tests, tools, scripts,
  documentation) is under the MIT licence in `LICENSE`, except `patches/`,
  which are modifications to Zephyr and MCUboot and remain under those
  projects' Apache-2.0 licence.
- The hardware is provided as is. Rev A has known errata, including a
  battery connection that is not safe to use: read
  `docs/hardware-rev-a-errata.md` before building one.
