"""Create enclosure v5 from the proven v4 ring-support geometry.

V5 addresses the three physical-print findings from v4:

* a narrow, bottom-open roof notch is completed by a raised base tongue, so
  the assembled opening exposes the USB-C receptacle instead of the module;
* that tongue continues inward as a shelf 0.4 mm below the shifted SuperMini,
  providing a mechanical catch under the overhanging module/connector;
* the tiny round plunger becomes a 5.5 mm square captured slider.  It prints
  head-down on its own small plate and is retained by a snap-on C collar.

The roof also carries a reinforced D-ring eye at the centre of its clear
short wall.
It is intended to be sliced with build-plate supports limited to that loop.
"""

import math
from pathlib import Path

import FreeCAD as App
import Import
import Mesh
import Part


ENCLOSURE_DIR = Path(__file__).resolve().parents[1]
OUT_DIR = ENCLOSURE_DIR / "out"
V4_DOC_PATH = ENCLOSURE_DIR / "thread-led-node-clamped-v4.FCStd"
DOC_PATH = ENCLOSURE_DIR / "thread-led-node-clamped-v5.FCStd"
OUT_DIR.mkdir(exist_ok=True)

if not V4_DOC_PATH.exists():
    raise SystemExit(f"missing {V4_DOC_PATH}; run create_clamped_v4.py first")


def feature(doc, group, name, label, shape):
    obj = doc.addObject("PartDesign::Feature", name)
    obj.Label = label
    obj.Shape = shape
    group.addObject(obj)
    return obj


def print_copy(shape, flip=False):
    result = shape.copy()
    if flip:
        result.rotate(App.Vector(0, 0, 0), App.Vector(1, 0, 0), 180)
    bbox = result.BoundBox
    result.translate(App.Vector(-bbox.XMin, -bbox.YMin, -bbox.ZMin))
    return result


doc = App.openDocument(str(V4_DOC_PATH))
doc.Label = "Thread LED Node Clamped V5"

assembly = doc.getObject("Assembly")
references = doc.getObject("References")
print_group = doc.getObject("PrintParts")
construction = doc.getObject("Construction")

v5_group = doc.addObject("App::DocumentObjectGroup", "V5Revision")
v5_group.Label = "V5 — close USB bezel, module cradle, captive button, D-ring"

params = doc.addObject("Spreadsheet::Sheet", "V5Parameters")
params.Label = "V5 design choices"
for cell, value, meaning in (
    ("A1", "Value", "Meaning"),
    ("A2", "14.8 mm", "USB connector opening at its narrow inner face"),
    ("A3", "8.0 mm", "Assembled USB opening height after plug fit test"),
    ("A4", "0.4 mm", "Cradle clearance below conservative module PCB"),
    ("A5", "5.5 mm", "Square USER-button actuator shaft"),
    ("A6", "0.30 mm/side", "Actuator running clearance in guide"),
    ("A7", "6.4 mm", "Short-side attachment-loop clear opening"),
    ("A8", "M2.5 x 35 mm", "Recommended four lid screws"),
):
    params.set(cell, value)
    params.set(f"B{cell[1:]}", meaning)
params.setColumnWidth("A", 150)
params.setColumnWidth("B", 390)
v5_group.addObject(params)

# ---------------- inherited primary dimensions ----------------
base_x, base_y = 94.0, 60.0
base_x0, base_y0 = -7.0, -53.0
wall, floor, base_h = 2.2, 2.0, 10.0
lid_clearance, lid_wall = 0.30, 2.0
lid_bottom_z, roof_t = 7.5, 2.4
roof_top_z, roof_bottom_z = 38.6, 36.2

lid_inner_x = base_x + 2 * lid_clearance
lid_inner_y = base_y + 2 * lid_clearance
lid_outer_x = lid_inner_x + 2 * lid_wall
lid_outer_y = lid_inner_y + 2 * lid_wall
lid_inner_x0 = base_x0 - lid_clearance
lid_inner_y0 = base_y0 - lid_clearance
lid_outer_x0 = lid_inner_x0 - lid_wall
lid_outer_y0 = lid_inner_y0 - lid_wall

ring_x, ring_y = 40.0, -23.0
cap_support_z, ring_bottom_z = 26.0, 27.4

usb_center_x = 27.62
usb_inner_w = 14.8
usb_outer_w = 16.8
usb_floor_z = 14.3
# Physical v5 fit feedback: the metal receptacle cleared, but an ordinary
# moulded plug shoulder caught the upper edge and needed 2 mm hand trimming.
usb_inner_top_z = 22.3
usb_outer_top_z = 23.1
usb_inner_x0 = usb_center_x - usb_inner_w / 2
usb_outer_x0 = usb_center_x - usb_outer_w / 2

# ---------------- close USB opening and add module cradle ----------------
v4_base = doc.getObject("ClampedV4Base").Shape
v5_base = v4_base.copy()

# V4 removed a 22 mm portion of the base front wall.  Restore it before
# adding the much smaller raised tongue.
old_usb_x0 = usb_center_x - 11.0
base_wall_patch = Part.makeBox(
    22.0,
    wall,
    base_h - floor,
    App.Vector(old_usb_x0, base_y0 + base_y - wall, floor),
)
v5_base = v5_base.fuse(base_wall_patch)

# The tongue is 0.3 mm narrower per side than the roof notch.  It fills the
# bottom-open notch after assembly and leaves only the connector-height slot.
tongue_clearance = 0.30
tongue_x0 = usb_inner_x0 + tongue_clearance
tongue_w = usb_inner_w - 2 * tongue_clearance
tongue = Part.makeBox(
    tongue_w,
    wall,
    usb_floor_z - floor,
    App.Vector(tongue_x0, base_y0 + base_y - wall, floor),
)

# A short internal shelf catches the overhanging module if insertion force
# tries to bend it down.  Its top is 0.4 mm below the conservative PCB plane.
cradle_top_z = 14.7
cradle_t = 0.8
cradle = Part.makeBox(
    tongue_w,
    3.8,
    cradle_t,
    App.Vector(tongue_x0, 1.0, cradle_top_z - cradle_t),
)

# Two printable ribs carry the shelf back to the restored front wall.
rib_w = 1.4
left_rib = Part.makeBox(
    rib_w,
    3.8,
    cradle_top_z - base_h,
    App.Vector(tongue_x0, 1.0, base_h),
)
right_rib = Part.makeBox(
    rib_w,
    3.8,
    cradle_top_z - base_h,
    App.Vector(tongue_x0 + tongue_w - rib_w, 1.0, base_h),
)
v5_base = (
    v5_base.fuse(tongue)
    .fuse(cradle)
    .fuse(left_rib)
    .fuse(right_rib)
    .removeSplitter()
)

base_obj = feature(
    doc,
    assembly,
    "ClampedV5Base",
    "V5 BASE — USB tongue and shifted-module cradle",
    v5_base,
)

# ---------------- narrow roof port and square button guide ----------------
v4_roof = doc.getObject("ClampedV4Roof").Shape
v5_roof = v4_roof.copy()

# Restore the wall removed by v4's 22 x 20 mm cable opening.
roof_wall_patch = Part.makeBox(
    22.0,
    lid_wall,
    27.5 - lid_bottom_z,
    App.Vector(
        old_usb_x0,
        lid_inner_y0 + lid_inner_y,
        lid_bottom_z,
    ),
)
v5_roof = v5_roof.fuse(roof_wall_patch)

# Keep the notch bottom-open for the upside-down roof print.  The base tongue
# closes it to z=14.3 in assembly.  A shallow outer flare accepts ordinary
# moulded USB-C plugs while the inner aperture stays connector-sized.
usb_inner_cut = Part.makeBox(
    usb_inner_w,
    lid_wall + 2.0,
    usb_inner_top_z - lid_bottom_z + 0.2,
    App.Vector(
        usb_inner_x0,
        lid_inner_y0 + lid_inner_y - 0.8,
        lid_bottom_z - 0.1,
    ),
)
usb_outer_cut = Part.makeBox(
    usb_outer_w,
    1.0,
    usb_outer_top_z - lid_bottom_z + 0.2,
    App.Vector(
        usb_outer_x0,
        lid_outer_y0 + lid_outer_y - 0.8,
        lid_bottom_z - 0.1,
    ),
)
v5_roof = v5_roof.cut(usb_inner_cut.fuse(usb_outer_cut))

# Delete the v4 round guide, heal its roof hole, then add a non-rotating
# square guide.  The larger 5.5 mm slider is much more stable as a tall print.
button_x, button_y = 11.0, -40.0
old_guide_cut = Part.makeCylinder(
    4.65, roof_bottom_z - 12.0, App.Vector(button_x, button_y, 12.0)
)
button_roof_patch = Part.makeCylinder(
    5.2, roof_t, App.Vector(button_x, button_y, roof_bottom_z)
)
v5_roof = v5_roof.cut(old_guide_cut).fuse(button_roof_patch)

guide_bottom_z = 13.2
guide_outer = 10.0
guide_inner = 6.1
guide_outer_box = Part.makeBox(
    guide_outer,
    guide_outer,
    roof_bottom_z - guide_bottom_z,
    App.Vector(
        button_x - guide_outer / 2,
        button_y - guide_outer / 2,
        guide_bottom_z,
    ),
)
guide_bore = Part.makeBox(
    guide_inner,
    guide_inner,
    roof_top_z - guide_bottom_z + 1.0,
    App.Vector(
        button_x - guide_inner / 2,
        button_y - guide_inner / 2,
        guide_bottom_z - 0.5,
    ),
)
v5_roof = v5_roof.fuse(guide_outer_box).cut(guide_bore)

# Short-side D-ring: 5 mm thick along Y with a 6.4 mm opening.  It sits on
# the right short wall, directly opposite the USER-button side.  The ring
# overlaps the wall by 1.8 mm, so pulling loads the wall rather than a point.
loop_center_x = 92.8
loop_center_y = -23.0
loop_center_z = 31.8
loop_outer_r = 6.0
loop_inner_r = 3.2
loop_t = 5.0
loop_outer = Part.makeCylinder(
    loop_outer_r,
    loop_t,
    App.Vector(loop_center_x, loop_center_y - loop_t / 2, loop_center_z),
    App.Vector(0, 1, 0),
)
loop_inner = Part.makeCylinder(
    loop_inner_r,
    loop_t + 1.0,
    App.Vector(loop_center_x, loop_center_y - loop_t / 2 - 0.5, loop_center_z),
    App.Vector(0, 1, 0),
)
attachment_loop = loop_outer.cut(loop_inner)

# Broad roots at both sides of the eye reduce stress concentration at the
# wall.  The slicer will add removable build-plate support beneath the arc.
root_left = Part.makeBox(
    3.8,
    1.2,
    8.0,
    App.Vector(87.0, loop_center_y - loop_t / 2 - 1.0, 27.8),
)
root_right = Part.makeBox(
    3.8,
    1.2,
    8.0,
    App.Vector(87.0, loop_center_y + loop_t / 2 - 0.2, 27.8),
)
attachment_loop = attachment_loop.fuse(root_left).fuse(root_right)
v5_roof = v5_roof.fuse(attachment_loop).removeSplitter()

roof_obj = feature(
    doc,
    assembly,
    "ClampedV5Roof",
    "V5 ROOF — close USB port, captured USER button, rear D-ring",
    v5_roof,
)

# ---------------- captive square USER-button actuator ----------------
button_model_top_z = 10.9
tip_bottom_z = 11.25
neck_bottom_z = 12.2
shaft_bottom_z = 13.2
head_bottom_z = roof_top_z + 0.6

tip_w = 3.2
neck_w = 4.7
shaft_w = 5.5
head_w = 10.0
head_t = 2.2

tip = Part.makeBox(
    tip_w,
    tip_w,
    neck_bottom_z - tip_bottom_z,
    App.Vector(button_x - tip_w / 2, button_y - tip_w / 2, tip_bottom_z),
)
neck = Part.makeBox(
    neck_w,
    neck_w,
    shaft_bottom_z - neck_bottom_z,
    App.Vector(button_x - neck_w / 2, button_y - neck_w / 2, neck_bottom_z),
)
shaft = Part.makeBox(
    shaft_w,
    shaft_w,
    head_bottom_z - shaft_bottom_z,
    App.Vector(button_x - shaft_w / 2, button_y - shaft_w / 2, shaft_bottom_z),
)
head = Part.makeBox(
    head_w,
    head_w,
    head_t,
    App.Vector(button_x - head_w / 2, button_y - head_w / 2, head_bottom_z),
)
actuator = tip.fuse(neck).fuse(shaft).fuse(head).removeSplitter()

actuator_obj = feature(
    doc,
    assembly,
    "V5UserButtonActuator",
    "V5 USER button captive square actuator",
    actuator,
)

# A 1 mm C collar snaps sideways onto the 4.7 mm neck after the actuator is
# inserted from outside.  Its top rests against the guide bottom, while the
# tactile switch spring supplies the return force.
collar_outer_w = 9.0
collar_inner_w = 4.9
collar_t = 1.0
collar_outer = Part.makeBox(
    collar_outer_w,
    collar_outer_w,
    collar_t,
    App.Vector(
        button_x - collar_outer_w / 2,
        button_y - collar_outer_w / 2,
        neck_bottom_z,
    ),
)
collar_inner = Part.makeBox(
    collar_inner_w,
    collar_inner_w,
    collar_t + 0.4,
    App.Vector(
        button_x - collar_inner_w / 2,
        button_y - collar_inner_w / 2,
        neck_bottom_z - 0.2,
    ),
)
collar_split = Part.makeBox(
    collar_outer_w / 2 + 0.5,
    2.0,
    collar_t + 0.4,
    App.Vector(
        button_x + collar_inner_w / 2 - 0.2,
        button_y - 1.0,
        neck_bottom_z - 0.2,
    ),
)
collar = collar_outer.cut(collar_inner.fuse(collar_split)).removeSplitter()
collar_obj = feature(
    doc,
    assembly,
    "V5UserButtonCollar",
    "V5 USER button snap retaining collar",
    collar,
)

# ---------------- review references and cross-sections ----------------
actual_module = doc.getObject("V4ActualSuperMiniReference").Shape
actual_usb = doc.getObject("V4ActualUSBConnectorReference").Shape
pcb = doc.getObject("PCBAssemblyReference").Shape
cap_ref = doc.getObject("V4PatternedCapReference").Shape
ring_ref = doc.getObject("V4LEDRingReference").Shape

usb_approach = Part.makeBox(
    usb_inner_w,
    10.0,
    usb_inner_top_z - usb_floor_z,
    App.Vector(usb_inner_x0, 3.5, usb_floor_z),
)
usb_approach_obj = feature(
    doc,
    references,
    "V5USBApproachReference",
    "V5 USB-C plug nose approach",
    usb_approach,
)

usb_section_box = Part.makeBox(
    24.0,
    18.0,
    26.0,
    App.Vector(usb_center_x - 12.0, -2.0, 4.0),
)
usb_section = Part.makeCompound(
    (
        v5_base.common(usb_section_box),
        v5_roof.common(usb_section_box),
        actual_module.common(usb_section_box),
        actual_usb.common(usb_section_box),
        usb_approach.common(usb_section_box),
    )
)
usb_section_obj = feature(
    doc,
    construction,
    "V5USBSection",
    "V5 USB section — tongue closes notch and shelf catches module",
    usb_section,
)

button_section_box = Part.makeBox(
    12.0,
    12.0,
    roof_top_z + 5.0,
    App.Vector(button_x - 6.0, button_y - 6.0, 0.0),
)
button_section = Part.makeCompound(
    (
        v5_roof.common(button_section_box),
        actuator.common(button_section_box),
        collar.common(button_section_box),
        pcb.common(button_section_box),
    )
)
button_section_obj = feature(
    doc,
    construction,
    "V5ButtonSection",
    "V5 USER button section — guided slider and snap collar",
    button_section,
)

# ---------------- print copies and plate arrangements ----------------
base_print_shape = print_copy(v5_base)
roof_print_shape = print_copy(v5_roof, flip=True)
actuator_print_shape = print_copy(actuator, flip=True)
collar_print_shape = print_copy(collar)

# Three narrow sacrificial walls grow from the build plate to within one
# 0.2 mm layer of the loop's downward-facing arc.  They localize support to
# the requested eye instead of asking the slicer to fill the enclosure.
loop_supports = []
support_half_y = 0.35
support_gap = 0.22
for radial_offset in (0.7, 2.7, 4.7):
    closest_radius = radial_offset - support_half_y
    loop_surface_z = loop_center_z + math.sqrt(
        loop_outer_r**2 - closest_radius**2
    )
    support_start_z = loop_surface_z + support_gap
    support = Part.makeBox(
        2 * support_half_y,
        4.4,
        roof_top_z - support_start_z,
        App.Vector(
            loop_center_x + radial_offset - support_half_y,
            loop_center_y - 2.2,
            support_start_z,
        ),
    )
    loop_supports.append(support)

supported_roof_assembly = Part.makeCompound((v5_roof, *loop_supports))
roof_supported_print_shape = print_copy(supported_roof_assembly, flip=True)

base_print = feature(
    doc,
    print_group,
    "ClampedV5BasePrint",
    "PRINT — v5 base floor-down",
    base_print_shape,
)
roof_print = feature(
    doc,
    print_group,
    "ClampedV5RoofPrint",
    "PRINT — v5 roof top-face-down; support rear D-ring only",
    roof_print_shape,
)
roof_supported_print = feature(
    doc,
    print_group,
    "ClampedV5RoofSupportedPrint",
    "PRINT — v5 roof with three breakaway D-ring supports",
    roof_supported_print_shape,
)
actuator_print = feature(
    doc,
    print_group,
    "V5UserButtonActuatorPrint",
    "PRINT — v5 USER actuator head-down",
    actuator_print_shape,
)
collar_print = feature(
    doc,
    print_group,
    "V5UserButtonCollarPrint",
    "PRINT — v5 USER snap collar flat",
    collar_print_shape,
)

plate_base = base_print_shape.copy()
plate_roof = roof_supported_print_shape.copy()
# Keep only 4 mm between the physical parts. Their anti-warp brims may merge,
# and the compact layout leaves a safe bed-edge margin with a 20 mm brim.
plate_roof.translate(App.Vector(0.0, 64.0, 0.0))
enclosure_plate_shape = Part.makeCompound((plate_base, plate_roof))
enclosure_plate = feature(
    doc,
    print_group,
    "ClampedV5EnclosurePlatePrint",
    "PRINT — v5 base and supported-loop roof",
    enclosure_plate_shape,
)

# Three actuator candidates and six collars are intentionally isolated from
# the large enclosure plate.  The broad 10 mm heads and short travel moves
# avoid repeating the rough v4 tower print.
actuator_parts = []
for index in range(3):
    copy = actuator_print_shape.copy()
    copy.translate(App.Vector(index * 18.0, 0.0, 0.0))
    actuator_parts.append(copy)
for row in range(2):
    for col in range(3):
        copy = collar_print_shape.copy()
        copy.translate(App.Vector(col * 14.0 + 2.0, 16.0 + row * 12.0, 0.0))
        actuator_parts.append(copy)
actuator_plate_shape = Part.makeCompound(tuple(actuator_parts))
actuator_plate = feature(
    doc,
    print_group,
    "V5UserButtonKitPrint",
    "PRINT — three v5 actuators and six retaining collars",
    actuator_plate_shape,
)

doc.recompute()

if App.GuiUp:
    for old_name in (
        "ClampedV4Base",
        "ClampedV4Roof",
        "V4UserButtonPlunger",
        "ClampedV4BasePrint",
        "ClampedV4RoofPrint",
        "V4UserButtonPlungerPrint",
        "ClampedV4PlatePrint",
        "V4CenterCrossSection",
        "V4USBSection",
        "V4USBCableKeepoutReference",
    ):
        old_obj = doc.getObject(old_name)
        if old_obj:
            old_obj.ViewObject.Visibility = False
    base_obj.ViewObject.ShapeColor = (0.72, 0.74, 0.78)
    roof_obj.ViewObject.ShapeColor = (0.88, 0.88, 0.90)
    roof_obj.ViewObject.Transparency = 65
    actuator_obj.ViewObject.ShapeColor = (0.85, 0.25, 0.20)
    collar_obj.ViewObject.ShapeColor = (0.95, 0.55, 0.15)
    usb_approach_obj.ViewObject.ShapeColor = (0.20, 0.45, 0.90)
    usb_approach_obj.ViewObject.Transparency = 70
    usb_section_obj.ViewObject.Visibility = False
    button_section_obj.ViewObject.Visibility = False
    base_print.ViewObject.Visibility = False
    roof_print.ViewObject.Visibility = False
    roof_supported_print.ViewObject.Visibility = False
    actuator_print.ViewObject.Visibility = False
    collar_print.ViewObject.Visibility = False
    enclosure_plate.ViewObject.Visibility = False
    actuator_plate.ViewObject.Visibility = False

doc.recompute()
doc.saveAs(str(DOC_PATH))

Mesh.export([base_print], str(OUT_DIR / "thread-led-node-clamped-v5-base.stl"))
Mesh.export(
    [roof_print], str(OUT_DIR / "thread-led-node-clamped-v5-roof-clean.stl")
)
Mesh.export(
    [roof_supported_print],
    str(OUT_DIR / "thread-led-node-clamped-v5-roof.stl"),
)
Mesh.export(
    [actuator_print], str(OUT_DIR / "thread-led-node-v5-user-actuator.stl")
)
Mesh.export(
    [collar_print], str(OUT_DIR / "thread-led-node-v5-user-collar.stl")
)
Mesh.export(
    [enclosure_plate],
    str(OUT_DIR / "thread-led-node-clamped-v5-enclosure-plate.stl"),
)
Mesh.export(
    [actuator_plate],
    str(OUT_DIR / "thread-led-node-v5-user-button-kit.stl"),
)
Import.export(
    [
        base_obj,
        roof_obj,
        actuator_obj,
        collar_obj,
        doc.getObject("V4PatternedCapReference"),
        doc.getObject("V4LEDRingReference"),
        doc.getObject("V4ActualSuperMiniReference"),
        doc.getObject("V4ActualUSBConnectorReference"),
        usb_approach_obj,
    ],
    str(OUT_DIR / "thread-led-node-clamped-v5.step"),
)
doc.save()

print(f"saved {DOC_PATH}")
for obj in (
    base_obj,
    roof_obj,
    actuator_obj,
    collar_obj,
    base_print,
    roof_print,
    roof_supported_print,
    enclosure_plate,
    actuator_plate,
):
    bbox = obj.Shape.BoundBox
    print(
        f"{obj.Name}: {bbox.XLength:.2f} x {bbox.YLength:.2f} x "
        f"{bbox.ZLength:.2f} mm; solids={len(obj.Shape.Solids)}; "
        f"valid={obj.Shape.isValid()}"
    )
print(
    "USB visible opening: "
    f"{usb_inner_w:.1f} x {usb_inner_top_z - usb_floor_z:.1f} mm"
)
print(
    "module/cradle vertical clearance: "
    f"{actual_module.BoundBox.ZMin - cradle_top_z:.2f} mm"
)
print(
    "USER actuator rest gap: "
    f"{tip_bottom_z - button_model_top_z:.2f} mm"
)
