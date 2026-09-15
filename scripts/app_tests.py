#!/usr/bin/env python3
"""Behaviour tests for the app, driven through the action API.

    xvfb-run -a python3 scripts/app_tests.py [build-dir]

The core has unit tests; this covers the part that lives in App, which the
restructuring work moves around: documents, the view, layers, selections,
materials, libraries, tools and preferences. Every case states what it
expects of the JSON the program reports, or of the pixels it writes, so a
refactor that changes behaviour fails here rather than in someone's hands.

Needs a display (a real one or Xvfb) and no other Firn using the socket.
"""
import json, os, re, struct, subprocess, sys, tempfile, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402

ROOT = drive.ROOT
# Each test owns its socket and configuration; leave open editors alone.
_SESSION = tempfile.mkdtemp(prefix="firn-apptest-")
drive.SOCK = os.path.join(_SESSION, "driver.sock")
BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build")
OUT = tempfile.mkdtemp(prefix="firn-apptest-")

passed, failures, current = 0, [], "startup"


def section(name):
    global current
    current = name
    print(f"\n-- {name}")


def check(cond, what):
    global passed
    if cond:
        passed += 1
        print("  ok   " + what)
    else:
        failures.append(f"{current}: {what}")
        print("  FAIL " + what)


class Firn:
    """One running program, spoken to over its driver socket."""

    def __init__(self, sock):
        self.s = sock

    def send(self, line, quiet=False):
        self.s.sendall((line + "\n").encode())
        reply = drive.recv_line(self.s)
        payload = reply[3:] if reply.startswith("ok ") else reply
        if payload.startswith("error"):
            if not quiet:                      # a test can expect the refusal
                failures.append(f"{current}: {line} -> {payload}")
                print(f"  FAIL {line} -> {payload}")
            return None
        return payload[7:] if payload.startswith("result ") else payload

    def do(self, action, **params):
        return self.send(f"do {action} {json.dumps(params, separators=(',', ':'))}")

    def refused(self, action, **params):
        """True when the program rejects the call, which some tests want."""
        return self.send(f"do {action} {json.dumps(params, separators=(',', ':'))}", quiet=True) is None

    def step(self, *steps):               # driver input steps, for the UI itself
        for st in steps:
            self.send(drive.to_command(st))

    def state(self):
        raw = self.send("state_json")
        try:
            return json.loads(raw)
        except (TypeError, ValueError):
            return {}

    def image(self):
        return self.state().get("image", {})

    def layers(self):
        return self.image().get("layers", [])


def png_size(path):
    """The pixel size of a PNG, from its header."""
    data = open(path, "rb").read(33)
    w, h = struct.unpack(">II", data[16:24])
    return w, h


def png_pixel(path, x, y):
    """One pixel out of a PNG, so a test can assert on what was drawn."""
    data = open(path, "rb").read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 6
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", data[pos + 8:pos + 18])
        elif kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = 4 if ctype == 6 else 3
    stride = w * ch + 1
    prev = bytearray(w * ch)
    row = bytearray()
    for r in range(h):
        f = raw[r * stride]
        row = bytearray(raw[r * stride + 1:(r + 1) * stride])
        for i in range(len(row)):
            a = row[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1: row[i] = (row[i] + a) & 255
            elif f == 2: row[i] = (row[i] + b) & 255
            elif f == 3: row[i] = (row[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if (pa <= pb and pa <= pc) else b if pb <= pc else c)) & 255
        if r == y:
            return tuple(row[x * ch:(x + 1) * ch])
        prev = row
    return tuple(row[x * ch:(x + 1) * ch])


def test_documents(f):
    section("documents")
    f.do("file.new", width=320, height=240, color="#204080")
    img = f.image()
    check((img.get("width"), img.get("height")) == (320, 240), "file.new makes the size asked for")
    check(len(f.layers()) == 1, "a new image has one layer")
    check(f.state()["documents"] == 1, "one document is open")
    f.do("file.new", width=100, height=100)
    check(f.state()["documents"] == 2, "a second image opens beside the first")
    check(f.image()["width"] == 100, "the newest image is the current one")
    f.do("file.close")
    check(f.state()["documents"] == 1, "closing leaves the first")
    check(f.image()["width"] == 320, "and makes it current again")

    path = os.path.join(OUT, "doc.ora")
    f.do("file.save_as", path=path)
    check(os.path.exists(path), "file.save_as writes the file")
    check(f.image()["modified"] is False, "saving clears the modified flag")
    f.do("layer.new")
    check(f.image()["modified"] is True, "an edit marks it modified again")
    f.do("file.revert")
    check(f.image()["modified"] is False and len(f.layers()) == 1, "revert goes back to the saved file")
    f.do("file.open", path=path)
    check(f.state()["documents"] == 2, "file.open adds a document")
    f.do("file.close")


def test_view(f):
    section("view")
    f.do("view.zoom", mode="actual")
    check(abs(f.image()["zoom"] - 1.0) < 1e-6, "actual size is 1:1")
    f.do("view.zoom", zoom=4)
    check(abs(f.image()["zoom"] - 4.0) < 1e-6, "a zoom can be set outright")
    f.do("view.zoom", mode="fit")
    check(f.image()["zoom"] != 4.0, "fit picks its own zoom")
    f.do("select.rect", x0=10, y0=10, x1=40, y1=40)
    f.do("view.zoom_to_selection")
    check(f.image()["zoom"] > 4.0, "zoom to selection fills the window with it")
    f.do("select.none")
    for what in ("rulers", "grid", "guides", "assistants"):
        check(f.do("view.toggle", what=what) is not None, f"view.toggle knows {what}")
    check(f.refused("view.toggle", what="nonsense"), "and refuses one it does not")


def test_layers(f):
    section("layers")
    f.do("file.new", width=200, height=150, color="#ffffff")
    f.do("layer.new")
    f.do("layer.new")
    check(len(f.layers()) == 3, "layers can be added")
    check(f.image()["active_layer"] == 2, "the newest layer is active")
    f.do("layer.properties", name="Middle", opacity=40, blend="Multiply", visible=False)
    top = f.layers()[2]
    check(top["name"] == "Middle", "a layer can be renamed")
    check(abs(top["opacity"] - 40) < 0.5, "its opacity can be set")
    check(top["blend"] == "Multiply", "and its blend mode")
    check(top["visible"] is False, "and its visibility")
    f.do("layer.select", index=0)
    check(f.image()["active_layer"] == 0, "a layer can be made active by index")
    check(f.refused("layer.select", index=99), "an index past the end is refused")

    f.do("layer.select", index=2)
    f.do("layer.arrange", steps=-1)
    check(f.layers()[1]["name"] == "Middle", "arrange moves a layer down the stack")
    f.do("layer.duplicate")
    check(len(f.layers()) == 4, "a layer can be duplicated")
    f.do("layer.delete")
    check(len(f.layers()) == 3, "and deleted")
    f.do("edit.undo")
    check(len(f.layers()) == 4, "undo brings it back")
    f.do("edit.redo")
    check(len(f.layers()) == 3, "redo takes it away again")

    before = len(f.layers())
    f.do("layer.new_group")
    check(any(l["type"] == "group" for l in f.layers()), "a group layer appears")
    check(len(f.layers()) == before + 1, "grouping adds exactly the group")


def test_selection(f):
    section("selection")
    f.do("file.new", width=200, height=150, color="#000000")
    check(f.image()["selection"] is False, "a new image has no selection")
    f.do("select.all")
    check(f.image()["selection"] is True, "select all makes one")
    f.do("select.none")
    check(f.image()["selection"] is False, "and it can be dropped")
    f.do("select.rect", x0=20, y0=20, x1=80, y1=60)
    check(f.image()["selection"] is True, "a rectangle can be selected")
    f.do("select.invert")
    check(f.image()["selection"] is True, "and inverted")
    f.do("select.ellipse", x0=20, y0=20, x1=80, y1=60, feather=4)
    check(f.image()["selection"] is True, "a feathered ellipse can be selected")
    f.do("edit.undo")
    f.do("select.none")


def test_painting_and_materials(f):
    section("painting and materials")
    f.do("file.new", width=120, height=90, color="#ffffff")
    f.do("tool.color", color="#ff0000")
    f.do("tool.select", name="Flood Fill")
    check(f.refused("tool.select", name="No Such Tool"), "an unknown tool is refused")
    f.step("click_img:60:45", "wait:2")
    png = os.path.join(OUT, "fill.png")
    f.do("file.save_as", path=png)
    check(png_pixel(png, 60, 45)[:3] == (255, 0, 0), "the flood fill used the foreground color")

    # The gradient a user picks is the one that paints: this regressed once.
    f.do("file.new", width=120, height=90, color="#ffffff")
    f.step("set:material_kind:1", "set:material_gradient:0", "wait:1")
    f.do("tool.select", name="Flood Fill")
    f.step("click_img:60:45", "wait:2")
    grad = os.path.join(OUT, "grad.png")
    f.do("file.save_as", path=grad)
    top, bottom = png_pixel(grad, 60, 5), png_pixel(grad, 60, 85)
    check(top[:3] != bottom[:3], "a gradient fill varies down the image")

    f.do("tool.brush_size", size=24)
    f.do("tool.select", name="Paint Brush")
    f.step("drag_img:20,20:100,70", "wait:2")
    check(f.image()["last"] == "Paint Brush", "a brush stroke lands in the history")
    check(f.refused("tool.color", color="not a color"), "a bad color is refused")


def test_edit_actions(f):
    section("edit")
    f.do("file.new", width=100, height=80, color="#3366cc")
    f.do("layer.new")
    f.do("tool.color", color="#ffff00")
    f.do("select.rect", x0=20, y0=20, x1=60, y1=60)
    f.do("tool.select", name="Flood Fill")
    f.step("click_img:40:40", "wait:2")
    f.do("select.none")

    f.do("edit.copy_merged")
    f.do("edit.paste_as_image")
    check(f.state()["documents"] >= 2, "paste as image opens a document")
    merged = os.path.join(OUT, "merged.png")
    f.do("file.save_as", path=merged)
    check(png_pixel(merged, 5, 5)[:3] == (51, 102, 204), "copy merged keeps the layer underneath")
    f.do("file.close")

    f.do("select.rect", x0=10, y0=10, x1=50, y1=50)
    f.do("edit.content_aware_fill")
    check(f.image()["last"] == "Content-Aware Fill", "content-aware fill records one history entry")
    f.do("edit.undo")
    f.do("select.none")


def test_tools_and_history(f):
    section("tools and history")
    f.do("file.new", width=100, height=80, color="#ffffff")
    names = json.loads(f.send("do app.describe {}"))["tools"]
    check(len(names) > 20, "the program reports its tools")
    for name in ("Pan", "Zoom", "Selection", "Paint Brush", "Crop", "Deform", "Text"):
        check(name in names, f"{name} is among them")
        f.do("tool.select", name=name)
        check(f.state()["tool"] == name, f"{name} can be selected")
    f.do("tool.select", name="Paint Brush")

    depth = f.image()["history"]
    f.do("layer.new")
    check(f.image()["history"] == depth + 1, "an action adds a history entry")
    f.do("edit.undo")
    check(f.image()["history_cursor"] == depth, "undo steps the cursor back")


def test_api_surface(f):
    section("the api itself")
    api = json.loads(f.send("do app.describe {}"))
    check(api.get("version") == 2, "describe reports a version")
    names = [a["name"] for a in api["actions"]]
    check(len(names) == len(set(names)), "action names are unique")
    for required in ("file.new", "file.save_as", "layer.new", "select.rect", "image.resize", "view.zoom"):
        check(required in names, f"{required} is published")
    check(all(a["summary"] for a in api["actions"]), "every action has a summary")
    check(all("summary" in a and "input_schema" in a for a in api["actions"]), "every action publishes a summary and a schema")
    for a in api["actions"]:
        sch = a["input_schema"]
        check_once = sch.get("type") == "object" and "properties" in sch and "required" in sch
        if not check_once:
            check(False, f"{a['name']} has a well formed schema")
            break
    else:
        check(True, "every schema is a well formed JSON Schema object")
    resize = next(a for a in api["actions"] if a["name"] == "image.resize")
    props = resize["input_schema"]["properties"]
    check({"width", "height", "filter"} <= set(props), "image.resize documents its parameters")
    check("smart" in props["filter"]["enum"], "and the values its filter accepts")
    opened = next(a for a in api["actions"] if a["name"] == "file.open")
    check(opened["input_schema"]["required"] == ["path"], "a required parameter is marked required")
    check(f.refused("no.such.action"), "an unknown action is refused")
    cmds = api.get("commands", [])
    check(len(cmds) > 120, "describe also publishes the inherited command names")
    for required in ("GaussianBlur", "SelectAll", "NewRasterLayer", "Clarify"):
        check(required in cmds, f"{required} is published")
    check(any(c.endswith("*") for c in cmds), "prefix-matched commands are marked")
    f.do("file.new", width=60, height=40, color="#808080")
    check(f.do("GaussianBlur", Radius=2) is not None, "an inherited command runs alongside the actions")
    f.do("file.close")


def test_image_geometry(f):
    section("image geometry")
    f.do("file.new", width=100, height=60, color="#112233")
    f.do("image.resize", width=200, height=120, filter="lanczos")
    check((f.image()["width"], f.image()["height"]) == (200, 120), "the image can be resized")
    f.do("edit.undo")
    check(f.image()["width"] == 100, "and the resize undone")
    f.do("image.rotate", degrees=90)
    check((f.image()["width"], f.image()["height"]) == (60, 100), "rotating by 90 swaps the sides")
    f.do("edit.undo")
    f.do("select.rect", x0=10, y0=10, x1=60, y1=40)
    f.do("image.crop_to_selection")
    check((f.image()["width"], f.image()["height"]) == (50, 30), "cropping takes the selection's size")
    check(f.refused("image.resize", width=0), "a nonsense size is refused")


def test_clipping_masks(f):
    section("clipping masks")
    out = os.path.join(OUT, "clip.png")
    f.do("file.new", width=40, height=20, color="#00000000")
    f.do("layer.properties", name="Base")
    check(f.refused("layer.properties", clipped=True), "the bottom layer has nothing to clip to")
    f.do("layer.new")
    f.do("layer.properties", name="Top", clipped=True)
    check(f.layers()[1]["clipped"] is True, "a layer can be clipped to the one below")
    f.do("edit.undo")
    check(f.layers()[1].get("clipped") is False, "and the clip undone")
    f.do("edit.redo")
    check(f.layers()[1]["clipped"] is True, "and redone")
    f.do("layer.properties", clipped=False)
    check(f.layers()[1].get("clipped") is False, "and released again")
    f.do("file.close")


def test_background_work(f):
    section("background work")
    out = os.path.join(OUT, "bg.ora")
    f.do("file.new", width=64, height=48, color="#336699")
    # Actions save synchronously, so the file is there when the call returns.
    f.do("file.save_as", path=out)
    check(os.path.exists(out), "a scripted save finishes before it returns")
    size = os.path.getsize(out)
    check(size > 0, "and wrote something")
    # The same for the fill: synchronous through the API, so the pixels have
    # changed by the time the next call runs.
    f.do("select.rect", x0=20, y0=15, x1=44, y1=33)
    f.do("edit.content_aware_fill")
    check(f.image()["last"] == "Content-Aware Fill", "a scripted fill finishes before it returns")
    f.do("select.none")
    f.do("file.close")


def test_lock_transparency(f):
    section("lock transparency")
    f.do("file.new", width=40, height=20, color="#ffffff")
    f.do("layer.new")
    f.do("draw.rectangle", x=10, y=5, width=20, height=10, fill="#2244aa", target="raster")
    check(f.layers()[1]["lock_alpha"] is False, "a layer starts unlocked")
    f.do("layer.properties", lock_alpha=True)
    check(f.layers()[1]["lock_alpha"] is True, "and can be locked")
    # The same stroke, with the lock on and off: it must reach the clear part
    # of the layer only when the lock is off.
    f.do("tool.color", which="foreground", color="#ff8800")
    f.do("tool.select", name="Paint Brush")
    f.do("tool.brush_size", size=30)
    locked = os.path.join(OUT, "locked.png")
    f.step("drag_img:2,10:6,10")
    f.do("file.save_as", path=locked)
    check(png_pixel(locked, 3, 10)[:3] == (255, 255, 255), "paint stays out of the clear parts")
    f.do("edit.undo")
    f.do("layer.properties", lock_alpha=False)
    check(f.layers()[1]["lock_alpha"] is False, "and it can be unlocked again")
    unlocked = os.path.join(OUT, "unlocked.png")
    f.step("drag_img:2,10:6,10")
    f.do("file.save_as", path=unlocked)
    check(png_pixel(unlocked, 3, 10)[:3] == (255, 136, 0), "and the same stroke paints there once unlocked")
    f.do("file.close")


def test_pass_through_groups(f):
    section("pass-through groups")
    f.do("file.new", width=16, height=8, color="#c8c8c8")
    f.do("layer.new")
    f.do("layer.new_group")
    groups = [l for l in f.layers() if l["type"] == "group"]
    check(len(groups) == 1, "a group can be made")
    check(groups[0].get("pass_through") is False, "and starts isolated")
    # The group layer has to be the active one to toggle it.
    idx = next(i for i, l in enumerate(f.layers()) if l["type"] == "group")
    f.do("layer.select", index=idx)
    f.do("layer.properties", pass_through=True)
    check(f.layers()[idx]["pass_through"] is True, "a group can pass through")
    f.do("edit.undo")
    check(f.layers()[idx]["pass_through"] is False, "and the change undone")
    f.do("edit.redo")
    check(f.layers()[idx]["pass_through"] is True, "and redone")
    # Only groups have it.
    other = next(i for i, l in enumerate(f.layers()) if l["type"] != "group")
    f.do("layer.select", index=other)
    check(f.refused("layer.properties", pass_through=True), "an ordinary layer refuses it")
    f.do("file.close")


def test_blend_ranges(f):
    section("blend ranges")
    f.do("file.new", width=32, height=16, color="#404040")
    f.do("layer.new")
    check(f.layers()[1].get("blend_ranges") is None, "a new layer has no ranges")
    f.do("layer.blend_ranges", underlying="64 64 255 255")
    r = f.layers()[1].get("blend_ranges")
    check(r is not None and r["under"] == [64, 64, 255, 255], "a range can be set from a script")
    check(r["source"] == [0, 0, 255, 255], "and leaves the other one alone")
    f.do("layer.blend_ranges", channel="blue", this_layer="10 20 200 250")
    r = f.layers()[1]["blend_ranges"]
    check(r["channel"] == 3, "the channel can be chosen")
    check(r["source"] == [10, 20, 200, 250], "and the layer's own range set")
    check(r["under"] == [64, 64, 255, 255], "without disturbing the first")
    # Stops out of order are sorted rather than accepted as an inside-out range.
    f.do("layer.blend_ranges", this_layer="200 10 30 20")
    check(f.layers()[1]["blend_ranges"]["source"] == [200, 200, 200, 200], "stops are kept in order")
    check(f.refused("layer.blend_ranges", this_layer="10 20"), "a range needs four numbers")
    f.do("edit.undo")
    f.do("layer.blend_ranges", reset=True)
    check(f.layers()[1].get("blend_ranges") is None, "and they can be reset")
    f.do("file.close")


def test_metadata(f):
    section("metadata")
    out = os.path.join(OUT, "meta.png")
    f.do("file.new", width=40, height=30, color="#334455")
    check(json.loads(f.do("image.metadata"))["metadata"] == [], "a new image carries no metadata")
    f.do("image.set_metadata", name="Artist", value="A Person")
    f.do("image.set_metadata", group="Exif", name="UserComment", value="a note")
    f.do("image.set_metadata", group="Text", name="Source", value="the test suite")
    entries = {(m["group"], m["name"]): m["value"] for m in json.loads(f.do("image.metadata"))["metadata"]}
    check(entries.get(("Image", "Artist")) == "A Person", "an Exif tag can be set by name")
    check(entries.get(("Exif", "UserComment")) == "a note", "so can one in the Exif directory")
    check(entries.get(("Text", "Source")) == "the test suite", "and a text note")
    check(f.image()["last"] == "Metadata", "the edit is one history entry")
    f.do("edit.undo")
    check(len(json.loads(f.do("image.metadata"))["metadata"]) == 2, "undo takes the last one back")
    f.do("edit.redo")
    check(f.refused("image.set_metadata", name="NotATag", value="x"), "an unknown tag is refused")

    # Out to a file and back in.
    f.do("file.save_as", path=out)
    f.do("file.close")
    f.do("file.open", path=out)
    reopened = {(m["group"], m["name"]): m["value"] for m in json.loads(f.do("image.metadata"))["metadata"]}
    check(reopened.get(("Image", "Artist")) == "A Person", "it survives a PNG round trip")
    check(reopened.get(("Text", "Source")) == "the test suite", "text notes survive too")

    # Stripping.
    f.do("image.set_metadata", group="GPS", name="GPSLatitudeRef", value="N")
    f.do("image.strip_metadata", what="private")
    after = {(m["group"], m["name"]) for m in json.loads(f.do("image.metadata"))["metadata"]}
    check(("GPS", "GPSLatitudeRef") not in after, "stripping private data drops GPS")
    check(("Image", "Artist") in after, "and leaves the rest alone")
    f.do("image.strip_metadata")
    check(json.loads(f.do("image.metadata"))["metadata"] == [], "stripping everything empties it")

    # XMP: where a photo manager keeps the title, keywords and rating.
    f.do("image.set_metadata", group="XMP", name="dc:title", value="A dog in the grass")
    f.do("image.set_metadata", group="XMP", name="dc:subject", value="dog; summer")
    f.do("image.set_metadata", group="XMP", name="xmp:Rating", value="4")
    xmp = {m["name"]: m["value"] for m in json.loads(f.do("image.metadata"))["metadata"] if m["group"] == "XMP"}
    check(xmp.get("dc:title") == "A dog in the grass", "an XMP property can be set")
    check(xmp.get("dc:subject") == "dog; summer", "and one holding a list")
    for ext in ("png", "jpg", "ora", "pspimage", "tif"):
        path = os.path.join(OUT, "meta_xmp." + ext)
        f.do("file.save_as", path=path)
        f.do("file.close")
        f.do("file.open", path=path)
        back = {m["name"]: m["value"] for m in json.loads(f.do("image.metadata"))["metadata"] if m["group"] == "XMP"}
        check(back.get("dc:title") == "A dog in the grass", "XMP survives a " + ext + " round trip")
        check(back.get("dc:subject") == "dog; summer", "including its lists, in " + ext)
    f.do("image.set_metadata", group="XMP", name="photoshop:City", value="Reykjavik")
    f.do("image.strip_metadata", what="private")
    left = {m["name"] for m in json.loads(f.do("image.metadata"))["metadata"] if m["group"] == "XMP"}
    check("photoshop:City" not in left, "stripping private data reaches into the XMP packet")
    check("dc:title" in left, "and leaves the rest of it alone")
    f.do("file.close")


def test_drawing_api(f):
    section("typed drawing API")
    out = os.path.join(OUT, "drawing.png")
    def pixel(x,y):
        f.do("file.save_as",path=out)
        return png_pixel(out,x,y)[:3]
    def cursor(): return f.image()["history_cursor"]
    f.do("file.new",width=120,height=120,color="#ffffff")
    f.do("draw.rectangle",x=10,y=10,width=30,height=30,fill="#ff0000")
    check(pixel(20,20)==(255,0,0) and pixel(50,50)==(255,255,255), "rectangle paints the requested bounds")
    f.do("draw.ellipse",x=50,y=10,width=40,height=40,fill="#00ff00")
    check(pixel(70,30)==(0,255,0) and pixel(51,11)==(255,255,255), "ellipse has a filled center and clipped corners")
    f.do("draw.polygon",points=[[10,100],[30,50],[50,100]],fill="#0000ff")
    check(pixel(30,80)==(0,0,255), "polygon paints a triangle without scanline commands")
    before=cursor()
    f.do("draw.stroke",points=[[60,70],[100,70],[60,70]],color="#000000",size=8,opacity=0.5)
    check(all(115<=c<=140 for c in pixel(80,70)), "stroke opacity applies once despite retracing")
    check(cursor()==before+1,"a multi-point stroke is one undo entry")
    f.do("edit.undo")
    check(pixel(80,70)==(255,255,255),"undo removes the entire stroke")
    f.do("edit.redo")
    check(all(115<=c<=140 for c in pixel(80,70)),"redo restores the stroke")
    f.do("draw.stroke",points=[[100,100]],color="#000000",size=8)
    check(pixel(100,100)==(0,0,0),"a single stroke point paints a dot")
    f.do("file.new",width=120,height=120,color="#ffffff")
    f.do("draw.path",nodes=[{"x":10,"y":60,"out":[10,10]},{"x":100,"y":60,"in":[100,10]}],fill="none",stroke="#000000",stroke_width=8)
    check(pixel(55,23)==(0,0,0) and pixel(55,60)==(255,255,255),"Bezier control points bend the rendered path")
    f.do("select.rect",x0=20,y0=20,x1=40,y1=40)
    f.do("draw.rectangle",x=0,y=0,width=120,height=120,fill="#ff0000")
    check(pixel(25,30)==(255,0,0) and pixel(90,90)==(255,255,255),"raster drawing respects the active selection")
    f.do("edit.fill",color="#00ff00")
    check(pixel(25,30)==(0,255,0) and pixel(90,90)==(255,255,255),"typed fill also respects the selection")
    f.do("select.none")
    f.do("layer.new_vector")
    answer=json.loads(f.do("draw.ellipse",x=65,y=65,width=40,height=40,fill="#8844ff",target="vector"))
    check(answer["target"]=="vector" and answer["object"]==0,"vector draw returns its layer and object index")
    check(f.layers()[-1]["type"]=="vector","vector target stays editable")
    ora=os.path.join(OUT,"drawing.ora")
    f.do("file.save_as",path=ora);f.do("file.close");f.do("file.open",path=ora)
    check(f.layers()[-1]["type"]=="vector" and pixel(85,85)==(136,68,255),"vector shapes survive an OpenRaster round trip")
    f.do("select.rect",x0=0,y0=0,x1=20,y1=20)
    check(f.refused("draw.rectangle",x=0,y=0,width=10,height=10,target="vector"),"vector drawing refuses silently ignoring a raster selection")
    f.do("select.none")
    check(f.refused("draw.stroke",points=[[5,5]],color="#ffffff"),"stroke rejects a vector layer")
    f.do("file.new",width=50,height=50,color="#ffffff")
    for params in [dict(x=0,y=0,width=-1,height=10),dict(x=0,y=0,width=0,height=10),dict(x="0",y=0,width=10,height=10),dict(x=0,y=0,width=10,height=10,fill="#oops"),dict(x=0,y=0,width=10,height=10,colour="#ff0000"),dict(x=0,y=0,width=10),dict(x=0,y=0,width=10,height=10,fill="none",stroke="none")]:
        before=cursor()
        check(f.refused("draw.rectangle",**params) and cursor()==before,"invalid rectangle input leaves history unchanged: "+str(params))
    check(f.refused("draw.polygon",points=[[0,0],[1,1]]),"polygon requires at least three vertices")
    check(f.refused("draw.polygon",points=[[0,0],[1,1],[2,"bad"]]),"nested coordinates are validated")
    check(f.refused("draw.path",nodes=[{"x":0,"y":0,"out":[1]},{"x":10,"y":10}],fill="none",stroke="#000000"),"Bezier handle shape is validated")
    check(f.refused("draw.stroke",points=[],color="#000000"),"empty strokes are refused")
    check(f.refused("draw.stroke",points=[[0,0]],color="#000000",size=501),"brush size limits are enforced")
    check(f.refused("file.open"),"required parameters are enforced for existing typed actions")
    check(f.refused("view.zoom",mode="typo"),"existing enums are enforced")
    f.do("file.close")


def test_node_editing(f):
    section("vector node editing")
    f.do("file.new",width=200,height=150,color="#ffffff")
    f.do("layer.new_vector")
    f.do("draw.ellipse",x=30,y=30,width=100,height=80,fill="#eeaa75",target="vector")
    objs=json.loads(f.do("object.list"))["objects"]
    check(len(objs)==1 and len(objs[0]["paths"])==1 and objs[0]["paths"][0]["closed"],"object.list reports the drawn ellipse as one closed path")
    check(len(objs[0]["paths"][0]["nodes"])==4,"an ellipse is four Bezier nodes")
    check("in" in objs[0]["paths"][0]["nodes"][0],"curve nodes report their control points")

    check(f.refused("object.node_break"),"node editing refuses before a node is picked")
    f.do("object.node_select",object=0,node=1)
    f.do("object.node_break")
    paths=json.loads(f.do("object.list"))["objects"][0]["paths"]
    check(len(paths)==1 and not paths[0]["closed"] and len(paths[0]["nodes"])==5,"breaking a closed path opens it and repeats the break node")
    f.do("object.node_select",object=0,node=2)
    f.do("object.node_break")
    paths=json.loads(f.do("object.list"))["objects"][0]["paths"]
    check(len(paths)==2,"breaking an open path splits it in two")
    f.do("object.node_join")
    paths=json.loads(f.do("object.list"))["objects"][0]["paths"]
    check(len(paths)==1 and len(paths[0]["nodes"])==5,"joining puts the two halves back")

    first=paths[0]["nodes"][0]
    f.do("object.path_reverse")
    back=json.loads(f.do("object.list"))["objects"][0]["paths"][0]["nodes"]
    check(back[-1]["x"]==first["x"] and back[-1]["y"]==first["y"],"reversing turns the path around")
    f.do("object.path_closed",closed=True)
    check(json.loads(f.do("object.list"))["objects"][0]["paths"][0]["closed"],"a path can be closed again")

    f.do("object.add_path",nodes=[{"x":150,"y":20},{"x":190,"y":20},{"x":190,"y":60}],closed=True)
    obj=json.loads(f.do("object.list"))["objects"][0]
    check(len(obj["paths"])==2,"add_path appends a contour to the selected object")
    out=os.path.join(OUT,"nodes.png")
    f.do("file.save_as",path=out)
    check(png_pixel(out,175,35)[:3]==(238,170,117),"the appended contour is painted with the object's fill")
    f.do("edit.undo")
    check(len(json.loads(f.do("object.list"))["objects"][0]["paths"])==1,"every node edit is one undo step")

    # The original's own merge script runs these three in order.
    f.do("file.new",width=120,height=60,color="#ffffff")
    f.do("layer.new_vector")
    f.do("draw.rectangle",x=10,y=10,width=40,height=40,fill="#3366cc",target="vector")
    f.do("object.select",mode="all")
    props=json.loads(f.send('do ReturnVectorObjectProperties {}'))
    check(len(props["ListOfObjects"])==1,"ReturnVectorObjectProperties reports the layer's objects")
    path=props["ListOfObjects"][0]["paths"][0]
    f.send('do ConvertToPath {}')
    f.send('do NodeEditAddPath ' + json.dumps({"Path":{"nodes":[{"x":70,"y":10},{"x":110,"y":10},{"x":110,"y":50}],"closed":True}},separators=(',',':')))
    check(len(json.loads(f.do("object.list"))["objects"][0]["paths"])==2,"NodeEditAddPath adds the path a script hands it")
    f.send('do NodeEditAddPath ' + json.dumps({"Path":path},separators=(',',':')))
    check(len(json.loads(f.do("object.list"))["objects"][0]["paths"])==3,"a path read back out can be added straight to another object")
    check(f.refused("object.node_select",object=9),"node selection validates its indexes")
    f.do("file.close")
    f.do("file.close")


def test_generate_action(f):
    section("image model action")
    # No server is configured in CI, so what is checked here is that the
    # feature refuses cleanly rather than hanging or half-editing.
    f.do("file.new", width=64, height=48, color="#ffffff")
    check(f.refused("generate.fill", prompt="anything"), "generative fill refuses with no model configured")
    check(f.image()["history_cursor"] == 0, "and leaves the history alone")
    f.do("select.rect", x0=8, y0=8, x1=40, y1=32)
    check(f.refused("generate.fill", prompt="anything"), "still refused with a selection")
    api = json.loads(f.do("app.describe", name="generate.fill"))["actions"][0]
    names = set(api["input_schema"]["properties"])
    check(names == {"prompt", "strength", "seed"}, "the action takes a prompt, a strength and a seed")
    check("seed" not in api["input_schema"].get("required", []), "the seed is optional, so each call differs")
    # Instruction editing is a separate action because the picture goes to
    # the model a different way: as a reference, not as noise to work back
    # from, and with no selection involved.
    check(f.refused("generate.edit", prompt="remove the dog"), "generative edit refuses with no model configured")
    check(f.refused("generate.edit"), "and refuses without an instruction")
    edit = json.loads(f.do("app.describe", name="generate.edit"))["actions"][0]
    check(set(edit["input_schema"]["properties"]) == {"prompt", "seed"}, "editing takes an instruction and a seed, no strength")
    check("prompt" in edit["input_schema"].get("required", []), "the instruction is required")
    f.do("file.close")


def test_right_button_selection(f):
    section("the right button on a selection tool")
    def sel(): return f.image()["selection"]
    f.do("file.new", width=200, height=150, color="#ffffff")
    f.step("tool:Selection")
    f.do("select.rect", x0=40, y0=30, x1=160, y1=120)
    check(sel(), "there is a selection to work with")
    # Inside it the right button does nothing, so working within a selection
    # cannot throw it away by accident.
    f.send("click_img 100 75 right")
    check(sel(), "right-clicking inside the selection keeps it")
    # Outside it, the right button clears it, as in the original.
    f.send("click_img 10 10 right")
    check(not sel(), "right-clicking outside the selection clears it")
    # And a right drag never draws one, which is what made the two buttons
    # feel identical before.
    f.do("select.rect", x0=40, y0=30, x1=160, y1=120)
    f.send("drag_img 60,50 140,110 right")
    check(sel(), "a right drag inside leaves the selection as it was")
    f.do("select.none")
    f.send("drag_img 20,20 90,90 right")
    check(not sel(), "and a right drag never draws a new one")

    # Point to point: the right button ends the selection where it stands.
    f.step("tool:Freehand Selection", "set:sel_type:1")
    for pt in ((20, 20), (120, 30), (90, 110)):
        f.send("click_img %d %d" % pt)
    check(not sel(), "a polygon still being drawn is not a selection yet")
    f.send("click_img 60 90 right")
    check(sel(), "right-clicking closes the polygon")

    # The magic wand follows the same rule.
    f.step("tool:Magic Wand")
    f.send("click_img 10 10 right")
    check(not sel(), "and the wand clears a selection the same way")
    f.do("file.close")


def test_crop_and_text_gestures(f):
    section("crop handles and editing text in place")
    def size():
        i = f.image()
        return (i["width"], i["height"])

    # The crop rectangle can be adjusted rather than only redrawn, and a
    # double-click inside it crops, which is how the original ends the
    # gesture.
    f.do("file.new", width=200, height=150, color="#ffffff")
    f.step("tool:Crop")
    f.send("drag_img 40,30 160,120")
    check(size() == (200, 150), "drawing the rectangle does not crop on its own")
    f.send("drag_img 160,120 120,90")          # pull the bottom-right corner in
    f.send("dbl_img 80 60")                    # double-click inside applies
    w, h = size()
    check(abs(w - 80) <= 6 and abs(h - 60) <= 6, "a corner handle resizes the rectangle, a double-click crops to it")

    # Grabbing the middle moves the whole rectangle.
    f.do("file.new", width=200, height=150, color="#ffffff")
    f.send("drag_img 20,20 80,80")
    f.send("drag_img 50,50 90,90")
    f.send("dbl_img 100 100")
    w, h = size()
    check(abs(w - 60) <= 6 and abs(h - 60) <= 6, "dragging the middle moves it without changing its size")
    f.do("file.close")
    f.do("file.close")

    # Text already on the page is re-opened by clicking it, rather than
    # having a second block started on top.
    f.do("file.new", width=300, height=200, color="#ffffff")
    f.do("layer.new_vector")
    f.step("tool:Text", "set:create_as_vector:1")
    f.send("click_img 30 60")
    f.send("type Hello")
    f.send("key Enter")
    objs = json.loads(f.do("object.list"))["objects"]
    check(len(objs) == 1 and objs[0]["kind"] == "text", "the text tool makes a text object")
    f.send("click_img 60 40")                  # on the text itself
    f.send("key Enter")
    check(len(json.loads(f.do("object.list"))["objects"]) == 1,
          "clicking text already there edits it instead of stacking another")
    f.do("file.close")


def test_interchange_formats(f):
    section("Photoshop and TIFF")
    f.do("file.new", width=80, height=60, color="#ffffff")
    f.do("draw.rectangle", x=10, y=10, width=30, height=25, fill="#cc3322")
    f.do("layer.new")
    f.do("draw.ellipse", x=30, y=20, width=40, height=30, fill="#2266cc")
    f.do("layer.properties", name="Ellipse", blend="Multiply", opacity=60)
    for ext in ("psd", "tif"):
        path = os.path.join(OUT, "interchange." + ext)
        f.do("file.save_as", path=path)
        check(os.path.exists(path), "Firn writes a ." + ext)
    f.do("file.close")
    f.do("file.open", path=os.path.join(OUT, "interchange.psd"))
    layers = f.layers()
    check(len(layers) == 2, "a PSD round trip keeps both layers")
    top = layers[-1]
    check(top["name"] == "Ellipse", "and their names")
    check(top["blend"] == "Multiply", "and their blend modes")
    check(abs(top["opacity"] - 60) < 1, "and their opacity")
    f.do("file.close")
    # TIFF is a flat format, so it comes back as one layer, losslessly.
    f.do("file.open", path=os.path.join(OUT, "interchange.tif"))
    check(len(f.layers()) == 1, "a TIFF opens as a single flattened layer")
    out = os.path.join(OUT, "interchange_tif.png")
    f.do("file.save_as", path=out)
    check(png_size(out) == (80, 60), "at the size it was written")
    check(png_pixel(out, 15, 15)[:3] == (204, 51, 34), "with its pixels intact")
    f.do("file.close")


def test_tube_export(f):
    section("picture tube export")
    tube=os.path.join(OUT,"exported.psptube")
    f.do("file.new",width=120,height=80,color="#ffffff")
    f.do("draw.rectangle",x=5,y=5,width=20,height=20,fill="#cc4422")
    check(f.refused("file.export_tube",path=tube,columns=7,rows=2),"a grid the image does not divide by is refused")
    check(not os.path.exists(tube),"a refused export writes nothing")
    f.do("file.export_tube",path=tube,columns=3,rows=2,cells=5,step=30,placement="continuous",selection="incremental")
    check(os.path.exists(tube),"the tube file is written")
    f.do("file.close")
    f.do("file.open",path=tube)
    back=os.path.join(OUT,"tube_back.png")
    f.do("file.save_as",path=back)
    check(png_size(back)==(120,80),"the tube reopens at its own size")
    check(png_pixel(back,10,10)[:3]==(204,68,34),"the tube's artwork survives the round trip")
    # The original's AutoTuber script writes a tube this way.
    f.do("file.new",width=60,height=60,color="#ffffff")
    f.do("draw.rectangle",x=0,y=0,width=30,height=30,fill="#2288dd")
    reply=f.send('do ExportTube ' + json.dumps({"FileName":"firn_test_tube","NumberOfCellsAcross":2,"NumberOfCellsDown":2,
                                                "TotalNumberOfCells":4,"StepSize":30,"PlacementMode":"Random","SelectionMode":"Incremental"},separators=(',',':')))
    written=json.loads(reply).get("Path","")
    check(written.endswith("firn_test_tube.psptube") and os.path.exists(written),"ExportTube writes into the tube folder Firn scans")
    if os.path.exists(written):
        os.remove(written)
    f.do("file.close")
    f.do("file.close")


def test_atomic_batches(f):
    section("atomic batches")
    out=os.path.join(OUT,"batch.png")
    def pixel(x,y):
        f.do("file.save_as",path=out);return png_pixel(out,x,y)[:3]
    def call(action,**params):return {"action":action,"params":params}
    f.do("file.new",width=80,height=80,color="#ffffff")
    r=json.loads(f.do("app.batch",name="A tiny face",actions=[call("layer.new"),call("layer.properties",name="Face"),call("draw.ellipse",x=10,y=10,width=60,height=60,fill="#ff8800"),call("draw.polygon",points=[[20,30],[40,15],[60,30]],fill="#000000")]))
    check(r["count"]==4 and len(r["results"])==4,"batch returns every child result")
    check(f.image()["history_cursor"]==1 and f.image()["last"]=="A tiny face","batch becomes one named undo step")
    check(len(f.layers())==2 and pixel(40,50)==(255,136,0),"batch edits the intended layer")
    f.do("edit.undo")
    check(len(f.layers())==1 and pixel(40,50)==(255,255,255),"one undo restores the entire document")
    # Failure after an actual mutation must preserve this redo tail too.
    before=f.image()
    refused=f.refused("app.batch",actions=[call("edit.fill",color="#ff0000"),call("draw.rectangle",x=0,y=0,width=20,height=20,target="vector")])
    after=f.image()
    check(refused and after["history"]==before["history"] and after["history_cursor"]==before["history_cursor"],"runtime failure preserves the undo and redo history")
    check(pixel(40,50)==(255,255,255),"runtime failure rolls back earlier pixel edits")
    f.do("edit.redo")
    check(len(f.layers())==2 and pixel(40,50)==(255,136,0),"redo still works after a failed batch")
    before=f.image()
    check(f.refused("app.batch",actions=[call("layer.new"),call("select.rect",x0=0,y0=0,x1=20,y1=20),call("layer.select",index=999)]),"a late invalid layer index aborts a batch")
    after=f.image()
    check(len(after["layers"])==len(before["layers"]) and after["active_layer"]==before["active_layer"] and after["selection"]==before["selection"],"rollback restores layer structure, active layer and selection")
    sentinel=os.path.join(OUT,"must-not-exist.png")
    for action in [call("file.save_as",path=sentinel),call("edit.undo"),call("edit.copy"),call("tool.color",color="#ff0000"),call("Fill"),call("app.batch",actions=[call("layer.new")])]:
        check(f.refused("app.batch",actions=[call("edit.fill",color="#ff0000"),action]),"batch excludes non-transactional action "+action["action"])
    check(not os.path.exists(sentinel),"forbidden file actions never write a file")
    check(pixel(40,50)==(255,136,0),"prevalidation failures do not partially edit the image")
    check(f.refused("app.batch",actions=[]),"empty batch is refused")
    check(f.refused("app.batch",actions=[call("layer.new")]*257),"oversized batch is refused")
    check(f.refused("app.batch",actions=[{"action":"layer.new","typo":True}]),"unknown nested batch fields are refused")
    f.do("file.new",width=30,height=30,color="#ffffff")
    f.do("app.batch",actions=[call("edit.fill",color="#ff0000")])
    f.do("file.save_as",path=out)
    f.do("edit.undo")
    f.do("app.batch",actions=[call("edit.fill",color="#00ff00")])
    check(f.image()["modified"],"replacing a saved redo branch remains modified even at the same cursor")
    while f.state()["documents"]:
        f.do("file.close")
    check(f.refused("app.batch",actions=[call("layer.new")]),"batch requires an open document")


def test_discovery_v2(f):
    section("API discovery v2")
    api=json.loads(f.do("app.describe"))
    actions={a["name"]:a for a in api["actions"]}
    check(api["version"]==2,"discovery advertises schema version 2")
    for name in ["draw.rectangle","draw.ellipse","draw.polygon","draw.path","draw.stroke","edit.fill","app.batch"]:
        check(name in actions and bool(actions[name].get("examples")),name+" has a schema and runnable examples")
    check(actions["draw.stroke"]["input_schema"]["properties"]["size"]["default"]==16,"numeric defaults are JSON numbers")
    check(actions["draw.ellipse"]["input_schema"]["properties"]["antialias"]["default"] is True,"boolean defaults are JSON booleans")
    check("default" not in actions["file.save_as"]["input_schema"]["properties"]["quality"],"dynamic defaults are not falsely typed as numbers")
    check(actions["draw.path"]["input_schema"]["properties"]["nodes"]["items"]["required"]==["x","y"],"discovery includes nested path schemas")
    check(actions["layer.new"]["batch_safe"] and not actions["file.save_as"]["batch_safe"],"transaction eligibility is discoverable")
    single=json.loads(f.do("app.describe",name="draw.stroke"))
    check(len(single["actions"])==1 and single["actions"][0]["name"]=="draw.stroke","clients can request a single action schema")
    check(f.refused("app.describe",name="does.not.exist"),"unknown schema lookup is refused")
    check(api["legacy_replacements"]["Fill"]=="edit.fill","legacy callers can discover typed replacements")
    for name in ["draw.rectangle","draw.ellipse","draw.polygon","draw.path","draw.stroke","edit.fill","app.batch"]:
        for example in actions[name]["examples"]:
            f.do("file.new",width=140,height=140,color="#ffffff")
            check(f.do(name,**example) is not None,"published example runs: "+name)
            f.do("file.close")


def test_ui_scaling(f):
    section("UI scaling and framebuffer rendering")
    def value(state, name):
        return float(re.search(r'(?:^| )' + name + r'=([^ ]+)', state).group(1))
    f.do("file.new", width=96, height=96, color="#204080")
    f.step("set:ui_scale:1", "wait:3", "mv:0:0")
    base_font = value(f.send("state"), "font_size")
    for scale in (1, 1.25, 1.5, 2, 1):
        f.step(f"set:ui_scale:{scale}", "wait:3")
        f.do("view.zoom", mode="actual")
        state = f.send("state")
        check(abs(value(state, "ui_scale") - scale) < 0.01, f"UI uses {scale * 100:g}% scale")
        check(abs(value(state, "font_size") - base_font * scale) < 0.1, "font is rebuilt at the requested size")
        origin = re.search(r' origin=([^, ]+),([^ ]+)', state)
        x, y = map(float, origin.groups())
        density = value(state, "font_density")
        shot = os.path.join(OUT, f"scale-{scale}.png")
        f.step("shot:" + shot)
        px, py = int((x + 48) * density), int((y + 48) * density)
        got = png_pixel(shot, px, py)[:3]
        if got != (32, 64, 128):
            # Say what was actually there. This check assumes the screenshot
            # is the logical canvas times font_density, which is the part
            # most likely to be wrong on a platform whose DPI handling
            # differs, and a bare pass/fail gives nobody anything to go on.
            w, h = png_size(shot)
            print(f"       scale={scale} origin=({x},{y}) density={density} "
                  f"point=({px},{py}) shot={w}x{h} got={got}")
            for probe in (0.5, 1.0, 2.0):
                qx, qy = int((x + 48) * probe), int((y + 48) * probe)
                if 0 <= qx < w and 0 <= qy < h:
                    print(f"       at density {probe}: ({qx},{qy}) = {png_pixel(shot, qx, qy)[:3]}")
        check(got == (32, 64, 128),
              "canvas framebuffer contains the expected pixels after scaling")
    f.step("set:ui_scale:0", "wait:3")
    f.do("file.close")


def main():
    os.environ.setdefault("FIRN_WINDOW", "1280x800")
    firn = drive.binary(BUILD, "app", "firn")
    if not os.path.exists(firn):
        print("no app binary at " + firn)
        sys.exit(1)
    drive.kill()
    env = dict(os.environ, FIRN_DRIVE=drive.SOCK, XDG_CONFIG_HOME=_SESSION)
    log = open(os.path.join(OUT, "app.log"), "w")
    proc = subprocess.Popen([firn], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        sock = drive.connect(timeout=30.0)
        sock.sendall(b"wait 30\n")
        drive.recv_line(sock)
        f = Firn(sock)
        for case in (test_ui_scaling, test_api_surface, test_documents, test_view, test_layers, test_selection,
                     test_painting_and_materials, test_edit_actions, test_tools_and_history, test_image_geometry, test_clipping_masks, test_background_work, test_lock_transparency, test_pass_through_groups, test_blend_ranges, test_metadata, test_drawing_api, test_node_editing, test_generate_action, test_right_button_selection, test_crop_and_text_gestures, test_interchange_formats, test_tube_export, test_atomic_batches, test_discovery_v2):
            case(f)
        sock.sendall(b"quit\n")
        sock.settimeout(10.0)
        try:
            sock.recv(4096)
        except OSError:
            pass
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        drive.kill()

    print(f"\n{passed} checks passed, {len(failures)} failed")
    if failures:
        for why in failures:
            print("  " + why)
        print("app log: " + os.path.join(OUT, "app.log"))
        sys.exit(1)
    print("app tests passed (" + OUT + ")")


if __name__ == "__main__":
    main()
