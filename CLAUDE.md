# Firn

Native raster image editor in C++20 on Dear ImGui (docking) + SDL2 + OpenGL 3.
A Schneewolf Labs project. It recreates the workflow of the classic 2004 paint
program it grew out of; `docs/COMMANDS.md` inventories that program's commands
and maps its module layout onto this repo.

## Naming

- The app is **Firn**. Never put the original product's name in user-facing
  strings, window titles, or the README. In code comments refer to it as
  "the original". `docs/COMMANDS.md` is the one place it is named, because it
  documents that program.
- Identifiers: namespace `firn`, library `firncore`, executable `firn`,
  CMake options `FIRN_BUILD_APP` / `FIRN_BUILD_TESTS`, env var `FIRN_WINDOW`.
- `WindowsInstall/` is a gitignored backup of the original install, kept only
  as a reverse-engineering reference. Never build against it or copy from it.

## Build and test

```sh
cmake -S . -B build -G Ninja      # fetches Dear ImGui on first configure
cmake --build build
ctest --test-dir build --output-on-failure
./build/app/firn [image.png]
FIRN_WINDOW=1280x800 ./build/app/firn   # small window for test runs
```

Ubuntu deps: `libsdl2-dev libgl-dev`. Warnings are `-Wall -Wextra -Wpedantic`;
keep the build at zero warnings. Link legacy `libGL`, not GLVND `libOpenGL`
(the latter silently no-ops texture uploads on this machine; see
`app/CMakeLists.txt`).

## Layout

```
core/    libfirncore: image model, layers, document, commands, undo, raster ops, codecs.
         No ImGui, SDL, or GL includes here, ever.
app/     the desktop app. src/ui/ = canvas, menus, palettes; src/tools/ = canvas tools.
tests/   assert-based core tests (no framework), one ctest target.
scripts/ drive.py drives the running app with synthesized X11 input for screenshots.
docs/    notes on the original: command inventory, module mapping.
```

## Architecture rules

- **Every document mutation is a `Command`** (`core/include/firn/commands.h`)
  so undo/redo, the History palette, and future scripting see the same thing.
  UI code never edits pixels directly except through a tool gesture.
- **Tools paint live, then commit.** A tool snapshots the layer on press,
  edits `Layer::pixels` during the drag (calling `Document::touch()`), and on
  release records one `LayerSnapshotCommand` via `App::commit`. One gesture =
  one history entry. `App::run` is for commands that execute themselves.
- **`Document::revision()`** is how the UI knows to re-upload the canvas
  texture. Anything that changes pixels or layer state must `touch()`.
- **Straight-alpha RGBA8** everywhere in `Image`. Blur and resample in
  premultiplied space internally (see `raster::gaussian_blur`) so transparent
  pixels don't bleed colour.
- **Brush opacity is per stroke, not per stamp** (`raster::Stroke` keeps a
  max-coverage mask). Do not "fix" overlapping stamps by blending them.
- The **Background layer** (`Layer::background`) has no transparency: the
  eraser paints the background colour on it and clears alpha elsewhere.
- Index 0 is the bottom of the layer stack. Palettes list top first.
- New tools go in `app/src/tools/Tools.cpp` and register in
  `make_default_tools()`; give them a single-letter `shortcut()` matching the
  original where one exists (A pan, Z zoom, E dropper, B brush, X eraser, F fill).
- Add a test in `tests/test_core.cpp` for every new raster op or command.

## Checking UI changes

Unit tests cover the core. For the app, launch it small and drive it:

```sh
FIRN_WINDOW=1280x800 ./build/app/firn some.png &
WID=$(xwininfo -root -tree | grep '"Firn"' | awk '{print $1}')
python3 scripts/drive.py $WID key:b drag:400,300:700,500:1 shot:/tmp/out.png
```

Needs `python3-xlib`, `xwininfo`, and ImageMagick `import`. Coordinates are
window-relative. Stop the app with `pkill -x firn` (not `pkill -f`, which
matches your own shell). Do not launch the app maximized; the user's screen is
3440 px wide.
