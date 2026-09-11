#!/usr/bin/env python3
"""End-to-end smoke test: drives the real app through its socket driver.

    xvfb-run -a python3 scripts/smoke.py [build-dir]

Starts the app, makes an image, paints, adds a raster layer and a vector
shape, blurs, selects, saves the native file and a PNG, reopens the native
file and checks the layer stack, then converts it with firn-convert.
Exit status is non-zero on any failed step or check. Needs a display
(a real one or Xvfb) and no other Firn instance using the driver socket.
"""
import os, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402

ROOT = drive.ROOT
# Each test owns its socket and configuration; leave open editors alone.
_SESSION = tempfile.mkdtemp(prefix="firn-smoke-")
drive.SOCK = os.path.join(_SESSION, "driver.sock")
BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build")
OUT = tempfile.mkdtemp(prefix="firn-smoke-")
PSP = os.path.join(OUT, "smoke.pspimage")
ORA = os.path.join(OUT, "smoke.ora")
PNG = os.path.join(OUT, "smoke.png")
CONVERTED = os.path.join(OUT, "converted.png")

failures = []

def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)

def field(state, name):
    m = re.search(r'(?<![A-Za-z_])' + name + r'=("([^"]*)"|(\S+))', state)
    if not m:
        return None
    return m.group(2) if m.group(2) is not None else m.group(3)

def run(sock, steps):
    """Sends steps; returns the last state line; records rejected steps."""
    last = ""
    for step in steps:
        sock.sendall((drive.to_command(step) + "\n").encode())
        last = drive.recv_line(sock)
        if last.startswith("ok error"):
            failures.append(f"{step}: {last[3:]}")
            print("FAIL " + step + ": " + last[3:])
    return last[3:] if last.startswith("ok ") else last

def main():
    os.environ.setdefault("FIRN_WINDOW", "1280x800")
    firn = drive.binary(BUILD, "app", "firn")
    check(os.path.exists(firn), "app binary present at " + firn)
    if failures:
        sys.exit(1)
    drive.kill()
    env = dict(os.environ, FIRN_DRIVE=drive.SOCK, XDG_CONFIG_HOME=_SESSION)
    log = open(os.path.join(OUT, "app.log"), "w")
    proc = subprocess.Popen([firn], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        s = drive.connect(timeout=30.0)
        s.sendall(b"wait 30\n")
        drive.recv_line(s)

        st = run(s, ['do NewFile {"Width":320,"Height":200,"Transparent":false,"FillMaterial":{"Color":[200,220,240]}}', "state"])
        check(field(st, "size") == "320x200", "new image is 320x200")
        check(field(st, "layers") == "1", "new image has one layer")

        st = run(s, ["tool:Paint Brush", "drag_img:20,20:200,150", "wait:2", "state"])
        check(field(st, "last") == "Paint Brush", "brush stroke recorded in history")

        st = run(s, ["do NewRasterLayer {}", "tool:Preset Shape", "set:create_as_vector:1", "drag_img:40,40:160,160", "wait:2", "state"])
        check(field(st, "layers") == "3", "raster layer plus vector layer added")
        check(field(st, "objects") not in (None, "0"), "vector layer holds an object")

        st = run(s, ["layer:1", 'do GaussianBlur {"Radius":2}', "state"])
        check(field(st, "last") == "Gaussian Blur", "Gaussian Blur applied through the script API")

        st = run(s, ["tool:Selection", "drag_img:50,50:250,150", "wait:2", "state"])
        check(field(st, "selection") == "1", "rectangle selection made")

        st = run(s, ["ctrl:z", "state"])
        check(field(st, "selection") == "0", "undo removed the selection")

        st = run(s, ["save:" + PSP, "save:" + PNG, "state"])
        check(os.path.exists(PSP) and os.path.getsize(PSP) > 1000, "native file written")
        check(os.path.exists(PNG) and os.path.getsize(PNG) > 100, "PNG written")

        st = run(s, ["open:" + PSP, "state"])
        check(field(st, "docs") == "2", "native file reopened as a second document")
        check(field(st, "layers") == "3" and field(st, "size") == "320x200", "reopened file keeps three layers at 320x200")

        st = run(s, ["save:" + ORA, "open:" + ORA, "state"])
        check(os.path.exists(ORA) and os.path.getsize(ORA) > 1000, "OpenRaster file written")
        check(field(st, "docs") == "3" and field(st, "layers") == "3" and field(st, "size") == "320x200", "OpenRaster file reopened with three layers at 320x200")

        st = run(s, ['do IncreaseColorsTo16Bit {}', "state"])
        check(field(st, "depth") == "16", "16 bits per channel")

        st = run(s, ['do app.describe {}'])
        check('"name":"image.resize"' in st and '"tools"' in st, "the action API describes itself")
        st = run(s, ['do layer.new {}', 'do layer.properties {"name":"FromTheAPI","opacity":50}', "state"])
        check(field(st, "active_name") == "FromTheAPI", "an action renamed the layer")

        s.sendall(b"quit\n")  # the app exits without answering; wait for it to close the socket
        s.settimeout(10.0)
        try:
            while s.recv(4096):
                pass
        except OSError:
            pass
        s.close()
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            failures.append("app did not exit on quit")
        log.close()

    conv = drive.binary(BUILD, "tools", "firn-convert")
    r = subprocess.run([conv, PSP, CONVERTED], capture_output=True, text=True)
    check(r.returncode == 0 and os.path.exists(CONVERTED), "firn-convert flattened the native file")
    if r.returncode != 0:
        print(r.stdout, r.stderr)

    if failures:
        print("\n%d check(s) failed:" % len(failures))
        for f in failures:
            print("  " + f)
        print("app log: " + os.path.join(OUT, "app.log"))
        sys.exit(1)
    print("\nsmoke test passed (%s)" % OUT)

if __name__ == "__main__":
    main()
