#!/usr/bin/env python3
"""Drive a running Firn window with synthesized X11 input, for screenshots.

    python3 scripts/drive.py <window-id> step [step ...]

Steps (coordinates are window-relative pixels):
    mv:X:Y            move the pointer
    click:X:Y[:B]     click button B (1 left, 3 right) at X,Y
    drag:X,Y:X,Y...[:B]  press, move through the points, release
    key:K             tap a key by X keysym name (b, f, Escape, ...)
    type:TEXT         type text (letters, digits, . / - _ space)
    dbl:X:Y           double-click
    ctrl:K            tap Ctrl+K
    shot:FILE         capture the window to FILE with ImageMagick import
    sleep:SECONDS

Find the window id with: xwininfo -root -tree | grep '"Firn"'
`drive.py --kill` stops every running instance (use it before launching and
after finishing; an instance parked on the unsaved-changes prompt will not
exit on its own).
Requires python3-xlib, xwininfo, and ImageMagick.
"""
import time, sys, subprocess, re
from Xlib import display, X, XK
from Xlib.ext import xtest
d = display.Display()
wid = sys.argv[1]
if wid == "--kill":
    # Housekeeping: stop every running Firn instance (including ones parked on a dialog).
    # SIGKILL: SDL turns SIGTERM into a normal quit, which parks a modified
    # document on the unsaved-changes prompt instead of exiting.
    subprocess.run(["pkill", "-9", "-x", "firn"])
    sys.exit(0)
info = subprocess.run(["xwininfo","-id",wid],capture_output=True,text=True).stdout
ox = int(re.search(r"Absolute upper-left X:\s+(\d+)",info).group(1))
oy = int(re.search(r"Absolute upper-left Y:\s+(\d+)",info).group(1))
def mv(x, y): xtest.fake_input(d, X.MotionNotify, x=ox+x, y=oy+y); d.sync()
def press(b): xtest.fake_input(d, X.ButtonPress, b); d.sync()
def rel(b): xtest.fake_input(d, X.ButtonRelease, b); d.sync()
def key(ch):
    kc = d.keysym_to_keycode(XK.string_to_keysym(ch))
    xtest.fake_input(d, X.KeyPress, kc); d.sync(); time.sleep(0.08)
    xtest.fake_input(d, X.KeyRelease, kc); d.sync(); time.sleep(0.15)
def click(x, y, b=1):
    mv(x, y); time.sleep(0.1); press(b); time.sleep(0.1); rel(b); time.sleep(0.25)
def drag(pts, b=1):
    mv(*pts[0]); time.sleep(0.1); press(b); time.sleep(0.1)
    for (x0,y0),(x1,y1) in zip(pts, pts[1:]):
        for i in range(1, 21):
            mv(int(x0+(x1-x0)*i/20), int(y0+(y1-y0)*i/20)); time.sleep(0.012)
    time.sleep(0.1); rel(b); time.sleep(0.25)
def shot(name): subprocess.run(["import","-window",wid,name])

steps = sys.argv[2:]
try:
  for s in steps:
      op, *args = s.split(":")
      if op == "mv": mv(int(args[0]), int(args[1])); time.sleep(0.3)
      elif op == "click": click(int(args[0]), int(args[1]), int(args[2]) if len(args) > 2 else 1)
      elif op == "key": key(args[0])
      elif op == "drag":
          pts = [tuple(map(int, p.split(","))) for p in args[:-1]] if args[-1] in ("1","3") else [tuple(map(int, p.split(","))) for p in args]
          b = int(args[-1]) if args[-1] in ("1","3") else 1
          drag(pts, b)
      elif op == "type":
          names = {'.': 'period', '/': 'slash', '-': 'minus', '_': 'underscore', ' ': 'space'}
          for ch in args[0]:
              key(names.get(ch, ch))
      elif op == "dbl":
          mv(int(args[0]), int(args[1])); time.sleep(0.1)
          for _ in range(2):
              press(1); time.sleep(0.03); rel(1); time.sleep(0.06)
          time.sleep(0.3)
      elif op == "ctrl":
          ctl = d.keysym_to_keycode(XK.string_to_keysym("Control_L"))
          xtest.fake_input(d, X.KeyPress, ctl); d.sync(); time.sleep(0.05)
          key(args[0])
          xtest.fake_input(d, X.KeyRelease, ctl); d.sync(); time.sleep(0.2)
      elif op == "shot": shot(args[0])
      elif op == "sleep": time.sleep(float(args[0]))

except Exception as e:
    print("drive.py step failed:", e, file=sys.stderr)
    subprocess.run(["pkill", "-9", "-x", "firn"])
    sys.exit(1)