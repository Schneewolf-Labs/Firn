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
import json, os, struct, subprocess, sys, tempfile, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402

ROOT = drive.ROOT
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
    check(api.get("version") == 1, "describe reports a version")
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
    f.do("file.close")


def main():
    os.environ.setdefault("FIRN_WINDOW", "1280x800")
    firn = os.path.join(BUILD, "app", "firn")
    if not os.path.exists(firn):
        print("no app binary at " + firn)
        sys.exit(1)
    drive.kill()
    env = dict(os.environ, FIRN_DRIVE=drive.SOCK)
    log = open(os.path.join(OUT, "app.log"), "w")
    proc = subprocess.Popen([firn], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        sock = drive.connect(timeout=30.0)
        sock.sendall(b"wait 30\n")
        drive.recv_line(sock)
        f = Firn(sock)
        for case in (test_api_surface, test_documents, test_view, test_layers, test_selection,
                     test_painting_and_materials, test_edit_actions, test_tools_and_history, test_image_geometry, test_metadata):
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
