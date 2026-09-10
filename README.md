<img src="assets/icon.png" alt="Firn" width="160" align="right">

# Firn

[![build](https://github.com/Schneewolf-Labs/Firn/actions/workflows/build.yml/badge.svg)](https://github.com/Schneewolf-Labs/Firn/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/Schneewolf-Labs/Firn?include_prereleases)](https://github.com/Schneewolf-Labs/Firn/releases)

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

## Download

Packages for every tagged version are on the
[Releases](https://github.com/Schneewolf-Labs/Firn/releases) page:

- **Linux:** `Firn-<version>-x86_64.AppImage` (make it executable and run
  it; SDL2 is bundled) or `Firn-<version>-Linux-x86_64.tar.gz` (needs the
  system's SDL2; unpack and run `bin/firn`).
- **Windows:** `Firn-<version>-Windows-AMD64.zip` with `firn.exe`,
  `firn-convert.exe` and `SDL2.dll` in `bin/`.
- **macOS:** `Firn-<version>-Darwin-arm64.tar.gz` (needs `brew install sdl2`).

`SHA256SUMS.txt` lists the checksums. Every push to `main` also leaves
Linux and Windows packages as workflow artifacts on the Actions page for
trying the latest changes.

## Build

```sh
sudo apt install libsdl2-dev libgl-dev   # Ubuntu
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/app/firn [image.png]
FIRN_WINDOW=1280x800 ./build/app/firn   # override the initial window size
FIRN_UI_SCALE=1.5 ./build/app/firn      # force the UI scale (otherwise it follows the display)
```

Help > About shows the version, git commit and build facts (the build
records them each time it runs). File > Preferences picks the theme
(Firn, Dark, Light, Classic, Slate, or your own) and Edit Themes... opens
the theme editor: every color, rounding and padding, the font and text
size; themes save to `~/.config/firn/themes/*.firntheme` and can be
exported and imported as single files.

Install it (the app, `firn-convert`, a desktop entry and the icon):

```sh
cmake --install build --prefix ~/.local      # per user: ~/.local/bin/firn
sudo cmake --install build                   # system wide: /usr/local
```

## Pen tablets

Pressure changes the brush size, the opacity, or both (Tool Options of any
brush), and the eraser end of the pen picks the Eraser tool. Pressure is read
straight from the platform: XInput2 on X11 (build with `libxi-dev` installed),
pointer messages on Windows, tablet events on macOS. Wayland sessions do
not report pressure yet. Help > About says which backend is active.

## Testing and releasing

`ctest` runs the core tests and the native-format corpus. `scripts/smoke.py`
drives the real app through its socket driver (make an image, paint, add
layers and a vector shape, blur, select, save, reopen, convert) and is what
CI runs under Xvfb on every push. To cut a release:

```sh
scripts/release.sh 0.2.0    # bumps the version, dates CHANGELOG.md, commits, tags v0.2.0, pushes
```

The tag triggers `.github/workflows/release.yml`, which builds the
AppImage, tarballs and Windows zip, checks them, and publishes a GitHub
release whose notes are that version's CHANGELOG section.


## File formats

Opens PNG, JPEG, BMP, TGA, GIF and PNM through stb, and the original
program's native container (`.PspImage`, plus `.PspTube` and `.PspFrame`,
which share it) with layers, positions, opacity, blend modes and visibility.
Vector layers are read with their shapes, gradients and groups (the balloon
sample renders within 0.8/255 of the original's own composite); adjustment
layers are skipped with a warning. See `docs/FORMAT.md`. Saves the native container with layers, or PNG, JPEG, BMP
and TGA flattened. Files Firn writes open in the original program.
`firn-convert` does the same from the command line.

## Tools

Single-key shortcuts as in the original: **A** pan, **Z** zoom, **S**
selection, **L** freehand selection, **W** magic wand, **E** dropper, **M**
move, **R** crop, **B** paint brush, **P** airbrush, **X** eraser, **C**
clone (right-click sets the source), **N** lighten/darken, **U** smudge
(right button pushes), **Q** color replacer, **F** flood fill, **T** text,
**V** line, **I** preset shape, **O** object selector, **D** pen, **K** deform
(move, scale, rotate, skew and perspective a layer with handles; Straighten
and Perspective Correction sit beside it). Dodge/Burn, Soften, Sharpen,
Saturation and Hue brushes have no key. `[` and `]` resize the brush.
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

Vector objects: Layers > New Vector Layer, or tick "Create as vector" on the
Preset Shape, Line and Text tools, and the shapes stay editable. The Object
Selector moves, scales and rotates them (double-click for the Vector
Properties dialog: stroke, fill, width, line style, gradient, pattern, and
the text of a text object); the Pen draws point-to-point or freehand paths
and edits nodes. The Objects menu aligns, distributes, sizes, arranges,
groups and converts text to curves. Preset shapes come from `.PspShape`
files (`~/.config/firn/shapes` or `FIRN_SHAPE_DIRS`), gradients from
`.PspGradient` (`gradients`, `FIRN_GRADIENT_DIRS`), styled lines from
`.PspStyledLine` (`lines`, `FIRN_LINE_DIRS`) and patterns from images
(`patterns`, `FIRN_PATTERN_DIRS`). The Materials palette switches the
foreground and background between a color, a gradient and a pattern, which
the flood fill and the shape tools honor. Vector layers save to the native
container and open in the original; Layers > Convert to Raster Layer
flattens one.

Selections: Shift adds, Ctrl subtracts, a plain click deselects. Ctrl+A all,
Ctrl+D none, Ctrl+Shift+I invert, Delete clears. Every pixel command and the
brush, eraser, and fill tools are confined to the selection. Cut/Copy go to an
internal clipboard; paste as a new layer (Ctrl+L) or a new image (Ctrl+V).

Dear ImGui (docking branch) is fetched by CMake on first configure.

## 16 bits per channel and color management

Image > Increase Color Depth > 16 Bits per Channel keeps every raster layer
at 16 bits beside its 8-bit display pixels. Levels, curves,
brightness/contrast, gamma, HSL, colorize, color balance, channel mixer,
threshold, posterize, invert, grayscale, Gaussian blur, fills, and all
geometry run at full precision; other operations and the painting tools
work at 8 bits and reduce the layer (undoably, with a note in the status
bar). 48-bit native files and 16-bit PNGs read and write at full depth.

Embedded ICC profiles (PNG iCCP, JPEG APP2) are read, kept, and written
back. Image > Color Management assigns or converts to sRGB, Adobe RGB,
ProPhoto RGB or a profile file, and the color managed display shows tagged
images converted to sRGB (RGB matrix/TRC profiles; CMYK profiles are
recognized but not converted).

## Printing

File > Print (Ctrl+P) lays the image out on Letter, A4 or Legal paper
(orientation, margins, fit to page or a scale at a chosen DPI), writes a
one-page PDF and sends it to the default or a named printer through the
system spooler (`lp`); "Save as PDF" keeps the file instead.

## Scripting

Firn runs the original's Python scripts. Start the app with its driver
socket (`python3 scripts/drive.py --launch image.png`) and run a script
against it:

```sh
python3 scripts/firn-script.py "Thumbnail_150.PspScript"
python3 scripts/firn-script.py -c "App.Do(Environment, 'GaussianBlur', {'Radius': 4.0})"
```

`App.Do(Environment, 'Command', {...})` calls are sent as JSON to the app,
which implements them with the original's parameter names (see
`app/src/Script.cpp` for the list, about 110 commands covering files,
layers, selections, adjustments, effects, and materials); results such as
`ReturnImageInfo` come back as dicts. `App.Constants.X.Y` evaluates to the
value's name. Windows-only modules used by a few bundled scripts are not
available.

## License

Firn is released under the Apache License 2.0; see `LICENSE`. It bundles
Dear ImGui (MIT) and the stb libraries (public domain / MIT), and links
SDL2 (zlib). It is an independent reimplementation and is not affiliated
with the original program's publishers.
