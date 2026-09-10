#!/usr/bin/env python3
"""Drive Firn through its in-app driver (no real mouse involved).

    python3 scripts/drive.py --launch [IMAGE]      start the app (FIRN_DRIVE socket, small window), wait until it answers
    python3 scripts/drive.py step [step ...]       send steps; each is acknowledged when its frames have run
    python3 scripts/drive.py --kill                stop every running instance

The app listens on the Unix socket named by FIRN_DRIVE (default
/tmp/firn-drive.sock) and moves a virtual cursor: the real pointer is never
touched, and real mouse events are ignored while driving. Steps
(window-relative pixels; *_img variants take image pixel coordinates):

    mv:X:Y  mv_img:X:Y            move the virtual cursor
    click:X:Y[:B]  click_img:...   click button B (1 left, 3 right, 2 middle)
    dbl:X:Y  dbl_img:X:Y           double-click
    drag:X,Y:X,Y...[:B]  drag_img  press, move through the points, release
    down[:B]  up[:B]               press / release without moving
    key:NAME[:ctrl][:shift][:alt]  tap a key by ImGui name (z, Escape, Enter, Delete, ...)
    ctrl:NAME                      same as key:NAME:ctrl
    type:TEXT                      type text into the focused field
    wheel:D                        mouse wheel (positive = up)
    tool:NAME                      select a tool by its palette name ("Object Selector")
    layer:N                        make layer N active
    set:NAME:VALUE                 set a tool option (create_as_vector, shape_kind, shape_library, line_width,
                                   line_style, pen_mode, pen_close, sel_type, sel_shape, sel_range, sel_smoothing,
                                   sel_dialog, selection_edit, material_dialog, material_kind, material_gradient,
                                   material_texture, material_transparent, image_windows, arrange,
                                   brush_size, smooth_mode, smooth_amount, symmetry_mode, symmetry_count,
                                   symmetry_x, symmetry_y, fgsel_size, csmudge_rate, csmudge_length,
                                   csmudge_mode, assistant_kind, assistant_snap, filter_layer, ...)
    wait:N                         let N frames run
    shot:FILE                      save the framebuffer to FILE (PNG)
    save:PATH  open:PATH            save the current image / open a file (no file dialog)
    drop:PATH                       open a file the way a drag-and-drop onto the window does
    adjust:TITLE                   open an Adjust/Effects dialog by its title (then key:Enter applies it)
    profile:assign|convert|remove:sRGB|AdobeRGB|ProPhoto   color management actions
    state                          print the app state (tool, layers, selection, zoom, origin, history, status)
    quit                           ask the app to exit

Every step returns the state line; the last one is printed. Exit status is
1 when a step is rejected. Requires nothing beyond python3.
"""
import os, socket, subprocess, sys, time

SOCK = os.environ.get("FIRN_DRIVE", "/tmp/firn-drive.sock")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def kill():
    subprocess.run(["pkill", "-9", "-x", "firn"])
    # Wait for the processes to be gone so a new instance never races an old
    # one for the socket.
    for _ in range(50):
        if subprocess.run(["pgrep", "-x", "firn"], capture_output=True).returncode != 0:
            break
        time.sleep(0.05)
    try:
        os.unlink(SOCK)
    except OSError:
        pass

def connect(timeout=15.0):
    deadline = time.time() + timeout
    while True:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(SOCK)
            return s
        except OSError:
            s.close()
            if time.time() > deadline:
                sys.exit(f"drive.py: no app listening on {SOCK} (start one with --launch)")
            time.sleep(0.1)

def launch(image=None):
    kill()
    env = dict(os.environ, FIRN_DRIVE=SOCK)
    env.setdefault("FIRN_WINDOW", "1400x900")
    cmd = [os.path.join(ROOT, "build", "app", "firn")] + ([image] if image else [])
    log = open(os.environ.get("FIRN_LOG", "/tmp/firn-drive.log"), "w")
    subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    s = connect()
    s.sendall(b"wait 30\n")
    print(recv_line(s))
    s.close()

def recv_line(s):
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(4096)
        if not chunk:
            sys.exit("drive.py: app closed the connection")
        buf += chunk
    return buf.decode("utf-8", "replace").rstrip("\n")

def to_command(step):
    if step.startswith("do "):
        return step  # JSON parameters keep their colons
    op, *args = step.split(":")
    if op in ("save", "open", "drop"):
        return op + " " + ":".join(args)
    if op in ("key",):
        return " ".join([op] + args)
    if op == "ctrl":
        return f"key {args[0]} ctrl"
    if op == "type":
        return "type " + ":".join(args)
    if op == "shot":
        return "shot " + ":".join(args)
    if op in ("tool", "adjust"):
        return op + " " + ":".join(args)
    return " ".join([op] + args)

def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    if sys.argv[1] == "--kill":
        kill(); return
    if sys.argv[1] == "--launch":
        launch(sys.argv[2] if len(sys.argv) > 2 else None); return
    s = connect(timeout=3.0)
    last = ""
    failed = False
    for step in sys.argv[1:]:
        s.sendall((to_command(step) + "\n").encode())
        last = recv_line(s)
        if last.startswith("ok error"):
            print(f"{step}: {last[3:]}", file=sys.stderr)
            failed = True
            break
        if step == "state":
            print(last[3:])
    s.close()
    if not failed and sys.argv[-1] != "state":
        print(last[3:])
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
