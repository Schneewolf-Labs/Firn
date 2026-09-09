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
ctest --test-dir build --output-on-failure   # core tests + native-format corpus
./build/app/firn [image.png]
./build/tools/firn-convert in.PspImage out.png
FIRN_WINDOW=1280x800 ./build/app/firn   # small window for test runs
```

Ubuntu deps: `libsdl2-dev libgl-dev`. Warnings are `-Wall -Wextra -Wpedantic`;
keep the build at zero warnings. Link legacy `libGL`, not GLVND `libOpenGL`
(the latter silently no-ops texture uploads on this machine; see
`app/CMakeLists.txt`).

## Layout

```
core/    libfirncore: image model, layers, document, commands, undo, raster ops, codecs
         (stb for PNG/JPEG/BMP/TGA, io_psp.cpp for the native container).
         No ImGui, SDL, or GL includes here, ever.
tools/   firn-convert: CLI that prints a file's layer stack and flattens it to PNG.
app/     the desktop app. src/ui/ = canvas, menus, palettes, file dialog; src/tools/ = canvas tools.
tests/   assert-based core tests (no framework), one ctest target.
scripts/ drive.py drives the running app with synthesized X11 input for screenshots.
docs/    notes on the original: command inventory, module mapping, FORMAT.md
         (the native container layout, verified against the sample files).
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
- **Selections are a `Mask` on the `Document`** (empty mask = none). Change
  it only through `SelectionCommand` (`App::set_selection`) so it is undoable.
  `LayerPixelCommand` clips its result to the selection automatically via
  `raster::apply_through_mask`; tools pass `&doc->selection()` as the clip to
  `raster::Stroke` / `raster::flood_fill`. New pixel commands get this for
  free; new tools must opt in.
- **Adjustment and effect dialogs** live in `app/src/ui/Adjust.cpp` and use
  `adjust_modal(app, title, body, op)`: `body` draws widgets and returns
  true on change, `op` applies the parameters to an `Image`. The preview
  session re-applies `op` to the active layer live (on slider release for
  layers over 1 MP), OK commits one `LayerSnapshotCommand`, Cancel restores.
  Add new pixel operations to `core` (`adjust.h` for colour, `raster.h` for
  spatial) with a test, then one `adjust_modal` call and a menu item.
  Instant menu items use `AdjustCommand(layer, name, fn)`.
- **Canvas-size changes** derive from `GeometryCommand`: implement
  `transform(in, out)` producing every layer at the new size plus the
  selection; undo restores a full `Document::State` snapshot.
- `docs/ROADMAP.md` is the value-ranked work list. Tick items off as they
  land and take the top unchecked item next.
- Index 0 is the bottom of the layer stack. Palettes list top first.
- New tools go in `app/src/tools/Tools.cpp` and register in
  `make_default_tools()`; give them a single-letter `shortcut()` matching the
  original where one exists (A pan, Z zoom, S selection, E dropper, B brush,
  X eraser, F fill). L freehand and W magic wand are ours; the original put
  those on the S flyout.
- **Native format reading** lives in `core/src/io_psp.cpp`; `docs/FORMAT.md`
  is the reference and must be updated when the reader learns a new block.
  `tests/test_psp_corpus.cpp` loads every sample under `WindowsInstall/`
  (skips when absent) and round-trips each through the writer; run it after
  any reader or writer change. Open files through `io::load_document` and
  save through `io::save_document`; both dispatch on extension.
- **The original runs under Wine** from `WindowsInstall/`.
  `scripts/original-open.sh file.pspimage shot.png` opens a file in it and
  captures the window: the definitive check for anything the writer emits,
  and a way to observe the original's behaviour when a port detail is unclear.
- File open/save go through `FileDialog` (`app/src/ui/FileDialog.*`), an
  ImGui modal, via `App::request_open` / `request_save_as`. No native dialogs
  or extra dependencies. `io::save` picks the format from the extension.
- Text uses `core/src/text.cpp` (stb_truetype, vendored in `third_party/stb`).
  Fonts are discovered by scanning the usual directories plus
  `FIRN_FONT_DIRS`; the Text dialog previews on a temporary layer that is
  never recorded, then commits through `PasteLayerCommand`.
- **Documents**: the current image's state lives in App's members (`doc`,
  `history`, `doc_path`, zoom/pan); other open images are parked in
  `App::docs` as `DocState` and swapped in by `activate_document`. Always
  create documents through `add_document` and close through
  `close_document`, which handles the unsaved-changes prompt.
- Settings persist through `Config` (`~/.config/firn/firn.cfg`; the ImGui
  layout is `layout.ini` beside it). Add new persisted fields there.
- CI (`.github/workflows/build.yml`) builds and tests on Linux and Windows.
  Keep the code portable: no GCC-only flags outside the `if(NOT MSVC)`
  blocks, NOMINMAX is defined project-wide, `main()` is plain (SDL's
  entry point is disabled).
- Add a test in `tests/test_core.cpp` for every new raster op or command.

## Checking UI changes

Unit tests cover the core. For the app, launch it small and drive it:

```sh
FIRN_WINDOW=1280x800 ./build/app/firn some.png &
WID=$(xwininfo -root -tree | grep '"Firn"' | awk '{print $1}')
python3 scripts/drive.py $WID key:b drag:400,300:700,500:1 shot:/tmp/out.png
python3 scripts/drive.py $WID ctrl:o type:grad.png key:Return   # dialogs too
```

Needs `python3-xlib`, `xwininfo`, and ImageMagick `import`. Coordinates are
window-relative. Stop the app with `pkill -x firn` (not `pkill -f`, which
matches your own shell). Do not launch the app maximized; the user's screen is
3440 px wide.
