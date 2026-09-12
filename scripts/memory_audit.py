#!/usr/bin/env python3
"""Does Firn give memory back?

LeakSanitizer only sees memory that became unreachable. An editor's real
failure is memory that stays reachable and grows for as long as the session
does: a history that is never trimmed, documents that are closed but not
released. This drives the running program through those cycles and watches
its resident size.

    python3 scripts/memory_audit.py [build-dir]

It is slow and the numbers depend on the machine's allocator, so it is a
tool to run when something looks wrong, not a CI test. What matters is the
shape: repeated cycles should be flat, and the history should plateau at
the configured limit rather than climb forever. A few megabytes of creep
across rounds is glibc holding freed pages, not a leak; confirm any
suspicion with the sanitizer build, which names real leaks:

    cmake -S . -B build-asan -G Ninja -DCMAKE_CXX_FLAGS=-fsanitize=address \\
          -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
    ASAN_OPTIONS=detect_leaks=1 ./build-asan/app/firn      # then quit cleanly
"""
import json
import os
import subprocess
import sys
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
CLI = os.path.join(BUILD, "tools", "firn-cli")
SOCK = "/tmp/firn-memory-audit.sock"


def cli(*args):
    return subprocess.run([CLI, "--socket", SOCK] + list(args),
                          capture_output=True, text=True, timeout=300).stdout.strip()


def do(action, **params):
    return cli("do", action, json.dumps(params))


def rss_mb(pid):
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) // 1024
    return -1


def start():
    env = dict(os.environ, FIRN_DRIVE=SOCK, FIRN_WINDOW="900x650")
    app = subprocess.Popen(["xvfb-run", "-a", "--server-args=-screen 0 1000x700x24",
                            os.path.join(BUILD, "app", "firn")],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(80):
        if cli("state").startswith("{"):
            break
        time.sleep(0.5)
    else:
        sys.exit("the app never answered on " + SOCK)
    pid = subprocess.run(["pgrep", "-n", "-x", "firn"], capture_output=True, text=True).stdout.strip()
    return app, int(pid)


def repeated_cycles(pid):
    cycles = {
        "open and close a file": lambda: (do("file.open", path="samples/luca.jpg"), do("file.close")),
        "new, draw, undo, close": lambda: (do("file.new", width=900, height=700, color="#405060"),
                                           do("draw.rectangle", x=10, y=10, width=600, height=400,
                                              fill="#ff8800", target="raster"),
                                           do("edit.undo"), do("file.close")),
        "save and reopen a project": lambda: (do("file.new", width=600, height=400, color="#204080"),
                                              do("file.save_as", path="/tmp/firn-memory-audit.ora"),
                                              do("file.open", path="/tmp/firn-memory-audit.ora"),
                                              do("file.close"), do("file.close")),
        "resize and undo": lambda: (do("file.new", width=800, height=600, color="#808080"),
                                    do("image.resize", width=400, height=300, filter="lanczos"),
                                    do("edit.undo"), do("file.close")),
    }
    print(f'{"cycle":<28} {"start":>9} {"after 30":>10} {"growth":>8}')
    worst = 0
    for name, body in cycles.items():
        body()          # the first run fills caches; measure from the second
        body()
        time.sleep(0.3)
        before = rss_mb(pid)
        for _ in range(30):
            body()
        time.sleep(0.5)
        after = rss_mb(pid)
        worst = max(worst, after - before)
        print(f"{name:<28} {before:>6} MB {after:>7} MB {after - before:>+7} MB")
    return worst


def history_is_bounded(pid):
    print("\nhistory on a 12 megapixel image (the limit is undo_memory_mb, 1024 by default)")
    do("file.new", width=4000, height=3000, color="#334455")
    base = rss_mb(pid)
    marks = []
    for i in range(1, 61):
        do("draw.rectangle", x=i * 10, y=i * 8, width=1500, height=1200,
           fill="#%02x4080" % (i * 3 % 256), target="raster")
        if i % 20 == 0:
            marks.append((i, rss_mb(pid)))
            print(f"  after {i:>2} full-layer edits: {marks[-1][1]} MB")
    do("file.close")
    time.sleep(0.6)
    after_close = rss_mb(pid)
    print(f"  after closing the document: {after_close} MB (one document open was {base} MB)")
    # The last stretch of edits should add far less than the first: that is
    # the history trimming itself rather than growing without end.
    grew_late = marks[-1][1] - marks[-2][1]
    grew_early = marks[1][1] - marks[0][1] if len(marks) > 1 else 0
    print(f"  growth over the last 20 edits: {grew_late:+} MB "
          f"({'plateaued' if grew_late <= max(16, grew_early) else 'STILL CLIMBING'})")
    return grew_late, after_close


def main():
    if not os.path.exists(CLI):
        sys.exit("no firn-cli at " + CLI)
    app, pid = start()
    try:
        worst = repeated_cycles(pid)
        grew_late, after_close = history_is_bounded(pid)
        print("\nwhat to look for: the repeated cycles flat (a few MB is the allocator,")
        print("not a leak), and the history plateauing rather than climbing.")
        print(f"worst cycle growth: {worst:+} MB; history growth once full: {grew_late:+} MB")
    finally:
        cli("quit")
        try:
            app.wait(timeout=60)
        except subprocess.TimeoutExpired:
            app.kill()


main()
