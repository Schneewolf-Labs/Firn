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

## File formats

Opens PNG, JPEG, BMP, TGA, GIF and PNM through stb, and the original
program's native container (`.PspImage`, plus `.PspTube` and `.PspFrame`,
which share it) with layers, positions, opacity, blend modes and visibility.
Vector, adjustment, mask and group layers are skipped with a warning; a file
with no raster layers falls back to its embedded flattened composite. See
`docs/FORMAT.md`. Saves the native container with layers, or PNG, JPEG, BMP
and TGA flattened. Files Firn writes open in the original program.
`firn-convert` does the same from the command line.

## Tools

Single-key shortcuts as in the original: **A** pan, **Z** zoom, **S**
selection, **L** freehand selection, **W** magic wand, **E** dropper, **M**
move, **R** crop, **B** paint brush, **P** airbrush, **X** eraser, **C**
clone (right-click sets the source), **N** lighten/darken, **U** smudge
(right button pushes), **Q** color replacer, **F** flood fill, **T** text,
**V** line, **I** preset shape. Dodge/Burn, Soften, Sharpen, Saturation and
Hue brushes have no key. `[` and `]` resize the brush.
Left button uses the foreground material, right button the background.
Space + drag or middle-drag pans with any tool; Escape cancels a stroke.

Brush tips: the brush tools take a custom tip from `.PspBrush` or PNG files
in `~/.config/firn/brushes` (or `FIRN_BRUSH_DIRS`), or from the current
selection, and a paper texture from `~/.config/firn/textures` (or
`FIRN_TEXTURE_DIRS`). Saving as JPEG asks for the quality.

Picture Tubes: the Picture Tube tool stamps cells from `.PspTube` files found
in `~/.config/firn/tubes` (or `FIRN_TUBE_DIRS`), with the original's random,
incremental and angular selection and random or continuous placement.

Layers can be grouped (Layers > New Layer Group) and carry masks (Layers >
New Mask Layer); press Edit next to a mask to paint on it in grayscale.

Selections: Shift adds, Ctrl subtracts, a plain click deselects. Ctrl+A all,
Ctrl+D none, Ctrl+Shift+I invert, Delete clears. Every pixel command and the
brush, eraser, and fill tools are confined to the selection. Cut/Copy go to an
internal clipboard; paste as a new layer (Ctrl+L) or a new image (Ctrl+V).

Dear ImGui (docking branch) is fetched by CMake on first configure.
