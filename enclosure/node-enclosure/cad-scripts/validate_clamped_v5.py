"""Headless geometry, fit, and A1 Mini envelope checks for enclosure v5."""

from pathlib import Path

import FreeCAD as App


ENCLOSURE_DIR = Path(__file__).resolve().parents[1]
DOC_PATH = ENCLOSURE_DIR / "thread-led-node-clamped-v5.FCStd"

doc = App.openDocument(str(DOC_PATH))
doc.recompute()

required = (
    "ClampedV5Base",
    "ClampedV5Roof",
    "V5UserButtonActuator",
    "V5UserButtonCollar",
    "ClampedV5BasePrint",
    "ClampedV5RoofPrint",
    "ClampedV5RoofSupportedPrint",
    "V5UserButtonActuatorPrint",
    "V5UserButtonCollarPrint",
    "ClampedV5EnclosurePlatePrint",
    "V5UserButtonKitPrint",
    "PCBAssemblyReference",
    "V4ActualSuperMiniReference",
    "V4ActualUSBConnectorReference",
    "V4PatternedCapReference",
    "V4LEDRingReference",
    "V5USBApproachReference",
    "V5USBSection",
    "V5ButtonSection",
)
objects = {name: doc.getObject(name) for name in required}
missing = [name for name, obj in objects.items() if obj is None]
if missing:
    raise SystemExit(f"missing FreeCAD objects: {', '.join(missing)}")

for name, obj in objects.items():
    if obj.Shape.isNull() or not obj.Shape.isValid():
        raise SystemExit(f"{name} is null or invalid")

for name in (
    "ClampedV5Base",
    "ClampedV5Roof",
    "V5UserButtonActuator",
    "V5UserButtonCollar",
    "ClampedV5BasePrint",
    "ClampedV5RoofPrint",
    "V5UserButtonActuatorPrint",
    "V5UserButtonCollarPrint",
):
    solids = len(objects[name].Shape.Solids)
    if solids != 1:
        raise SystemExit(f"{name} has {solids} solids, expected 1")

if len(objects["ClampedV5RoofSupportedPrint"].Shape.Solids) != 4:
    raise SystemExit("supported v5 roof does not contain roof + 3 support walls")
if len(objects["ClampedV5EnclosurePlatePrint"].Shape.Solids) != 5:
    raise SystemExit("v5 enclosure plate does not contain base + supported roof")
if len(objects["V5UserButtonKitPrint"].Shape.Solids) != 9:
    raise SystemExit("v5 USER-button kit does not contain 3 sliders + 6 collars")

base = objects["ClampedV5Base"].Shape
roof = objects["ClampedV5Roof"].Shape
pcb = objects["PCBAssemblyReference"].Shape
module = objects["V4ActualSuperMiniReference"].Shape
usb = objects["V4ActualUSBConnectorReference"].Shape
cap = objects["V4PatternedCapReference"].Shape
ring = objects["V4LEDRingReference"].Shape
actuator = objects["V5UserButtonActuator"].Shape
collar = objects["V5UserButtonCollar"].Shape

for label, a, b, allowance in (
    ("PCB/base", pcb, base, 0.05),
    ("shifted SuperMini/base", module, base, 0.05),
    ("shifted SuperMini/roof", module, roof, 0.05),
    ("USB receptacle/base", usb, base, 0.05),
    ("USB receptacle/roof", usb, roof, 0.05),
    ("cap/roof hard overlap", cap, roof, 0.05),
    ("ring/roof", ring, roof, 0.05),
    ("actuator/roof", actuator, roof, 0.05),
    ("actuator/collar", actuator, collar, 0.05),
    ("collar/roof", collar, roof, 0.05),
):
    overlap = a.common(b).Volume
    if overlap > allowance:
        raise SystemExit(f"{label} collision is {overlap:.3f} mm^3")

# The shelf is close enough to catch the shifted module without preloading it.
module_base_clearance = module.distToShape(base)[0]
usb_base_clearance = usb.distToShape(base)[0]
if not 0.30 <= module_base_clearance <= 0.55:
    raise SystemExit(
        f"module/cradle clearance is {module_base_clearance:.3f} mm"
    )
if usb_base_clearance < 0.30:
    raise SystemExit(
        f"USB receptacle/base clearance is only {usb_base_clearance:.3f} mm"
    )

# Existing cap and LED-ring clamp/support contacts remain unchanged from v4.
for label, a, b in (
    ("cap/base", cap, base),
    ("LED ring/base shelf", ring, base),
    ("cap/roof clamp", cap, roof),
):
    distance = a.distToShape(b)[0]
    overlap = a.common(b).Volume
    if distance > 0.01:
        raise SystemExit(f"{label} contact has {distance:.3f} mm gap")
    if overlap > 0.05:
        raise SystemExit(f"{label} hard overlap is {overlap:.3f} mm^3")

# Plug-clear port: the centre and the added 2 mm upper region are open, while
# points just outside its inner width and above the revised top remain wall.
usb_center_x = 27.62
front_wall_y = 8.0
if roof.isInside(App.Vector(usb_center_x, front_wall_y, 17.2), 0.01, True):
    raise SystemExit("USB-C port centre is blocked")
if roof.isInside(App.Vector(usb_center_x, front_wall_y, 22.1), 0.01, True):
    raise SystemExit("USB-C port is missing its 2 mm upper plug clearance")
if not roof.isInside(App.Vector(19.9, front_wall_y, 17.2), 0.01, True):
    raise SystemExit("USB-C port is wider than its specified inner aperture")
if not roof.isInside(App.Vector(usb_center_x, front_wall_y, 22.5), 0.01, True):
    raise SystemExit("USB-C port is taller than specified")

# The assembled base tongue sits inside the roof notch with 0.3 mm side and
# fore/aft clearance; no base/roof collision is permitted.
base_roof_overlap = base.common(roof).Volume
if base_roof_overlap > 0.05:
    raise SystemExit(f"base/roof collision is {base_roof_overlap:.3f} mm^3")

# Slider rests 0.35 mm above the modeled switch and has 0.6 mm head travel.
switch_top_z = 10.9
tip_z = actuator.BoundBox.ZMin
head_gap = actuator.BoundBox.ZMax - 2.2 - roof.BoundBox.ZMax
if abs((tip_z - switch_top_z) - 0.35) > 0.01:
    raise SystemExit("USER actuator rest gap is not 0.35 mm")
if abs(head_gap - 0.60) > 0.01:
    raise SystemExit("USER actuator head travel is not 0.60 mm")

# The short-side D-ring hole is genuinely open along its Y axis.
for y in (-25.0, -23.0, -21.0):
    if roof.isInside(App.Vector(92.8, y, 31.8), 0.01, True):
        raise SystemExit("short-side attachment-loop hole is blocked")

for name in (
    "ClampedV5BasePrint",
    "ClampedV5RoofPrint",
    "ClampedV5RoofSupportedPrint",
    "V5UserButtonActuatorPrint",
    "V5UserButtonCollarPrint",
    "ClampedV5EnclosurePlatePrint",
    "V5UserButtonKitPrint",
):
    bbox = objects[name].Shape.BoundBox
    if abs(bbox.ZMin) > 0.001:
        raise SystemExit(f"{name} is not seated on Z=0")

plate_box = objects["ClampedV5EnclosurePlatePrint"].Shape.BoundBox
if plate_box.XLength + 20 > 180 or plate_box.YLength + 20 > 180:
    raise SystemExit(
        "v5 enclosure plate plus 10 mm brim does not fit A1 Mini: "
        f"{plate_box.XLength + 20:.1f} x {plate_box.YLength + 20:.1f}"
    )

kit_box = objects["V5UserButtonKitPrint"].Shape.BoundBox
if kit_box.XLength + 20 > 180 or kit_box.YLength + 20 > 180:
    raise SystemExit("v5 USER-button kit does not fit A1 Mini")

print("FreeCAD clamped-v5 checks: OK")
print(f"module/cradle clearance {module_base_clearance:.3f} mm")
print(f"USB receptacle/base clearance {usb_base_clearance:.3f} mm")
print(f"base/roof overlap {base_roof_overlap:.6f} mm^3")
print(f"USER actuator rest gap {tip_z - switch_top_z:.3f} mm")
print(f"USER actuator head travel {head_gap:.3f} mm")
print(
    f"enclosure plate {plate_box.XLength:.1f} x {plate_box.YLength:.1f} x "
    f"{plate_box.ZLength:.1f} mm before brim"
)
print(
    f"button kit {kit_box.XLength:.1f} x {kit_box.YLength:.1f} x "
    f"{kit_box.ZLength:.1f} mm before brim"
)

App.closeDocument(doc.Name)
