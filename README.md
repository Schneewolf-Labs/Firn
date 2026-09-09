# Firn

A native Linux (and Windows) raster image editor in C++, using Dear ImGui on
SDL2 + OpenGL 3 for the UI. A Schneewolf Labs project.

Firn recreates the workflow of the classic 2004 paint program it grew out of
(see `docs/COMMANDS.md` for the reverse-engineering notes). It is an
independent reimplementation and is not affiliated with the original's
publishers.

## Layout

```
core/             libfirncore: image model, layers, document, commands, undo, codecs
app/              firn: the ImGui desktop app (canvas, palettes, tools, menus)
tests/            core unit tests (ctest)
docs/             notes from studying the original, command inventory
third_party/      vendored single-header libs (stb)
WindowsInstall/   backup of the original program (gitignored, reference only)
```

The original's structure is mirrored on purpose. It was built as a command
layer (`JascCmd*.dll`, one per menu category) driving a document model, with
tools (`JascTool*.dll`) and palettes on top. See `docs/COMMANDS.md`.

## Build

```sh
sudo apt install libsdl2-dev libgl-dev   # Ubuntu
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/app/firn [image.png]
FIRN_WINDOW=1280x800 ./build/app/firn   # override the initial window size
```

## Tools

Single-key shortcuts as in the original: **A** pan, **Z** zoom, **S**
selection, **L** freehand selection, **W** magic wand, **E** dropper, **B**
paint brush, **X** eraser, **F** flood fill. `[` and `]` resize the brush.
Left button uses the foreground material, right button the background.
Space + drag or middle-drag pans with any tool; Escape cancels a stroke.

Selections: Shift adds, Ctrl subtracts, a plain click deselects. Ctrl+A all,
Ctrl+D none, Ctrl+Shift+I invert, Delete clears. Every pixel command and the
brush, eraser, and fill tools are confined to the selection. Cut/Copy go to an
internal clipboard; paste as a new layer (Ctrl+L) or a new image (Ctrl+V).

Dear ImGui (docking branch) is fetched by CMake on first configure.
