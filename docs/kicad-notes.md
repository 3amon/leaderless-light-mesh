# KiCad PCB-dev gotchas (raw notes)

Collected while building the carrier board in `hardware/` (July 2026,
KiCad 10.0.4, KiCAD-MCP-Server, freerouting, JLCPCB flow).

## MCP server / session state

- The MCP session pins to SWIG (file-based) or IPC (live GUI) **at
  `open_project` time** and never upgrades/downgrades. IPC only if the board
  is already open in the PCB editor. Check `_realtime` in every response.
- Tools that shell out (`sync_schematic_to_board`, `autoroute`) write the
  .kicad_pcb **on disk, behind the session's back** → the session then
  refuses auto-save ("disk changed externally") and your in-memory edits
  silently stop persisting. Recovery: `open_project` to reload, re-apply.
  Rule of thumb: reload after every tool that spawns an external process.
- Never mix external pcbnew scripts with an IPC-pinned session: the GUI's
  in-memory board diverges from disk, and a GUI save clobbers script work.
  Pick one authority per work phase; if scripts must run, have the human
  File → Revert afterward and never save from the GUI mid-run.
- `assign_net_to_class` isn't implemented in the SWIG backend — pass
  explicit `width` per routing call, or fix widths post-hoc via script.
- MCP tool calls time out at ~30 s but the underlying work may finish
  anyway (`download_jlcpcb_database` did). Re-check state before retrying —
  a second call may hit "already exists".

## pcbnew Python scripting (SWIG)

- **Segfault-prone under load.** One long script doing rip + move + load
  footprints + rebuild zones + route = SIGSEGV. Split phases into separate
  processes, `SaveBoard` between each. Small scripts are reliable.
- `board.FindFootprintByReference()` returns a raw SwigPyObject in KiCad 10
  (no methods). Iterate `board.GetFootprints()` and match `GetReference()`.
- `board.Remove(track)` spews "memory leak of type 'PCB_TRACK'" warnings —
  harmless, but they flood stdout; keep prints `flush=True` and filter.
- Zone refill via MCP `refill_zones` has a known segfault risk; do
  `pcbnew.ZONE_FILLER(board).Fill(board.Zones())` in a subprocess instead.
- `pcbnew.ExportSpecctraDSN(board, path)` and `ImportSpecctraSES(board,
  path)` exist and work — full headless freerouting without the MCP tool.
- **Never call `board.GetTracks()` twice after `board.Remove(track)`** —
  the second call throws "'SwigPyObject' object is not iterable". Snapshot
  `list(board.GetTracks())` ONCE at the top and work from that list.
- Zone rebuild + `ZONE_FILLER` in the same process after heavy Remove
  churn segfaults; save unfilled, then fill in a fresh process.
- Same-net zones that share an edge trip DRC `zones_intersect`; give each a
  distinct `SetAssignedPriority(i)`.
- kicad-cli is the reliable interface for renders/DRC/exports; the MCP
  `get_board_2d_view` wrapper breaks on KiCad ≥9 CLI syntax. Use
  `kicad-cli pcb export svg --exclude-drawing-sheet --fit-page-to-board`
  then `uvx cairosvg` when no rsvg/imagemagick exists.

## Freerouting

- Version ↔ Java coupling: freerouting ≥2.1 needs Java 25; v2.0.1 runs on
  Java 21. Grab the matching jar or it dies with
  UnsupportedClassVersionError before doing anything.
- CLI: `java -jar freerouting.jar -de in.dsn -do out.ses -mp 30` is fully
  headless and fast (<10 s for small boards).
- It routes at the **board default width at exactly minimum clearance**.
  Post-hoc widening of power nets WILL create clearance/short violations at
  pinch points (parallel runs, fuse pads, via throats). Loop: widen →
  kicad-cli DRC (json) → match offending tracks by (net, exact length) →
  revert those to 0.25 then 0.2 → refill → re-DRC until 0 errors.
  Better: set netclass widths BEFORE routing so freerouting spaces for them.
- It rips-and-reroutes anything not "fixed" in the DSN — pre-placed GND
  stitching vias may not survive; add stitching AFTER routing.
- SES import maps by net name; run it in the same process that exported if
  possible, else re-load the board fresh.

## Zones: the hard-won stuff

- **Edge-touching same-net zones do NOT merge** — fills pull back and leave
  a hairline gap → "zone unconnected to zone" on the same net. Make
  sections **overlap** (0.5–1 mm); distinct priorities make the overlap
  legal, and overlapped same-net fills merge properly.
- **Never hand-edit zone outline points** (`Outline().SetPoint`) on a
  filled zone — produced phantom clearance errors and self-split fills.
  Delete + recreate the zone cleanly instead.
- A zone can report "unconnected to itself": its fill fragments. Traces +
  through-hole pin columns fence the pour into pockets; a pocket
  containing a GND pad is "connected" (so island removal keeps it) yet has
  no path to the rest of the net. Diagnose with
  `zone.GetFilledPolysList(layer)` → per-outline bbox + check which
  fragments contain a GND via/PTH pad. Fix: drop a stitching via inside
  the starved pocket (collision-check first; gaps as tight as 1.4 mm can
  still fit a 0.6/0.3 via).
- A **solid zone connection on a boxed-in SMD pad** creates a tiny
  connected-but-isolated blob — trades `starved_thermal` for
  zone-unconnected. Only use solid connection when pour actually reaches
  the pad from a connected direction; otherwise via-rescue it.
- `SetIslandRemovalMode(AREA)` + `SetMinIslandArea` did NOT cull
  pad-bearing fragments — pads count as connections. Don't rely on it to
  clean up starved pockets.

## DRC / electrical gotchas found by tooling

- `starved_thermal`: an SMD GND pad surrounded by other-net copper can't
  get thermal spokes from the pour → effectively unconnected. Fix: short
  GND stub trace to a dedicated via. But CHECK THE STUB PATH — a naive
  straight stub crosses traces (instant `tracks_crossing`). Collision-check
  candidate directions before placing.
- Mounting-hole footprints have huge text bboxes — courtyard checkers
  report phantom overlaps/boundary violations from the ref text. Verify
  against the actual pad diameter before "fixing".
- NPTH mounting hole + nearby via/testpoint = courtyard overlap; keep
  ~3 mm center spacing from any copper feature.
- Zone islands: pours between two through-hole pin columns get sealed off
  by traces and culled as islands — harmless, but explains "missing" pour.

## Schematic authoring via MCP

- `batch_add_and_connect` places net labels EXACTLY on pins. Fast and
  electrically correct, but visually awful: labels overlap value text and
  symbol bodies. Plan positions generously (25 mm+ pitch) up front.
- Moving a component does NOT move its net labels or no-connect flags —
  they're independent objects at absolute coords. Bulk rearrangement =
  rewrite the .kicad_sch s-expression directly, translating each symbol
  together with labels/NCs whose coords sit on its pins (assign by nearest
  component origin). Validate with `kicad-cli sch erc` after.
- `autoplace_schematic_fields` (net-label aware) fixes most ref/value text
  collisions in one call — run it after any rearrangement.
- Remaining hotspots (3-pin devices: FETs, single-gate ICs): add short wire
  stubs from the pins and move the labels to the stub ends
  (`add_schematic_wire` snaps to pins + `move_schematic_net_label`).
- Integer-mm placements put pins off KiCad's 1.27 mm grid →
  `endpoint_off_grid` warnings (cosmetic; labels-on-pins still connect).
  Place on 1.27 multiples to avoid the noise.
- PWR_FLAG needed on every power-ish net (GND, rails) or ERC errors.
- Single-pin nets (e.g. an unused 3V3 brought to a label) warn "label
  connected to only one pin" — acceptable, documents intent.

## Silkscreen & final-check habits

- A footprint's reference text carries the FOOTPRINT's rotation. Move a
  rotated connector's ref text and it stays vertical/upside-down —
  `fp.Reference().SetTextAngle(EDA_ANGLE(0))` after repositioning.
- Iterating `board.GetDrawings()` after `board.Remove(drawing)` = same
  SWIG iterator crash as tracks. Snapshot the list first, always.
- Pre-order audit worth scripting every time: (1) dump every pad's net and
  diff against the intended pin→net map from the schematic build; (2)
  assert pin-1 positions/polarity geometrically (socket pin1 at the USB
  end, electrolytic + pad, LED cathode side, connector pin order); (3)
  dump every silk text's angle/mirror and eyeball a full-board silk
  render. Catches swapped rows and rotated parts that DRC never will.

## 3D models

- No community STEP for a module? **Generate a VRML stand-in** — VRML is
  plain text (colored Box nodes), and a slab + connector shell + shield +
  antenna block is plenty for enclosure-fit renders. KiCad .wrl units are
  0.1 inch: divide all mm dimensions by 2.54.
- Attach via pcbnew: `FP_3DMODEL()`, `m_Filename = '${KIPRJMOD}/models/x.wrl'`
  (project-relative works), `m_Offset` in mm, push onto `fp.Models()`.
  A module spanning two socket footprints: attach to one socket with an
  x/y offset to the module center and z = socket header height (8.5 mm for
  standard 2.54 mm female headers). 3D offset +y maps to board −y.
- `kicad-cli pcb render --rotate "-35,0,15" --perspective` gives a quick
  raytraced PNG — no GUI needed.

## Parts / library / fab flow

- JLCPCB parts DB (`download_jlcpcb_database`, cdfer source) = offline
  pricing/stock; ~350 Basic parts are the free-assembly set; each unique
  Extended part = $3 setup at assembly. Hand-solder small boards.
- LCSC search needs exact-ish terms: "330R"/"330 ohm" fail where the
  manufacturer part number (0603WAF3300T5E) hits. Diode/connector clones
  outrank genuine parts on price at same spec. Connector/header hunting:
  English category terms ("female header 2.54 1x12") often miss — MPN
  series prefixes work ("DS1023 1x12" → C7509522), and category names in
  the DB are sometimes Chinese (插件/排母).
- JLCPCB assembly files: BOM = Comment/Designator/Footprint/LCSC Part #
  (include a duplicate "JLCPCB Part #" column — their parser has used both
  names); CPL = Designator/Mid X/Mid Y/Layer/Rotation.
- **`kicad-cli pcb export pos` emits the footprint ORIGIN, not the
  centroid** — for pin-1-origin footprints (headers, JST, radial caps,
  buttons) JLC places the part's CENTER at that point → parts render
  offset by half their body. Compute Mid X/Y as the pad-bbox center in a
  script; don't use the raw pos file for CPL.
- JLC part-model rotations differ from KiCad zero-angle per package
  (PinSocket_1x needed +90). Keep a rotation-offset DB (footprint-name
  substring → degrees) in a project `fab_export.py` and grow it from
  placement-preview evidence. Same idea as the community JLC rotation DBs.
- In the JLC order flow, BOM/CPL do nothing until you click **"Process
  BOM & CPL"** (disabled until BOTH files are up) — "parts not adding" is
  usually this. Placement preview: select a part by clicking it in the 3D
  canvas (pointer events), then "independent rotation" spins it in place —
  but fix the CPL at the source instead so re-uploads stay correct.
- Enable "Confirm Parts Placement" (~$0.45): their engineer reviews
  polarity/rotation against silk before production — the safety net for
  LED/diode zero-angle flips you can't verify in the tiny preview.
- To verify polarity YOURSELF in the placement preview: switch to **2D
  mode** — wheel-zoom is cursor-anchored there (3D zooms on view center
  and synthetic pan events don't work), and 2D draws explicit +/−
  polarity overlays on LEDs and electrolytics plus a live mm coordinate
  readout. KiCad LED_0603 at rotation 0 imports with correct polarity
  (community rotation DBs carry no LED entry for a reason). The arrow
  buttons in the viewer MOVE THE SELECTED PART — they are not view pan.
- Real cost datapoint (this 15-line, 17-part board incl. THT connectors,
  headers, 5 boards): PCB $2 + assembly $28.52 = $30.52 + shipping.
- Stock KiCad libs lack many JLC-common SMD footprints (e.g. TS-1187A
  tactile) — substitute a footprint-compatible generic (6×6 THT button) or
  import the JLC lib rather than guessing pad geometry.
- Module-on-carrier designs: check what the module already provides before
  adding circuitry (SuperMini nRF52840's RAW pin is already USB/battery
  diode-OR'd — carrier diodes would just add a second drop).
- Chip-antenna modules: window ALL copper (both layers) under the antenna
  end; either overhang the antenna past the board edge or split pours
  around a keepout rect.
- USB-connector-on-module + socket: to make the port reachable through an
  enclosure wall, the module's USB edge must sit flush/proud of the carrier
  edge — pin-1-to-module-edge distance sets socket placement; measure the
  real module, don't trust clone drawings (~2.7 mm for nice!nano-likes).
- P-FET high-side switch on a 5 V-ish rail can't be driven off by a 3.3 V
  GPIO directly (V_GS never reaches 0) — needs an NMOS pre-driver + pullup.
- WS2812 chain: 0.7×VDD input threshold math decides whether you need a
  level shifter at all; TTL-input parts (74AHCT / LV1T) shift up cleanly;
  BSS138 I²C modules are too slow.
- JLCPCB accepts kicad-cli gerber output zipped as-is (gerbers + Excellon
  PTH/NPTH). `--fit-page-to-board` renders are the quickest sanity check.
