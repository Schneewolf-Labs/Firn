# PSP9

A native Linux (and Windows) reimplementation of Jasc Paint Shop Pro 9 in C++,
using Dear ImGui on SDL2 + OpenGL 3 for the UI.

Personal reverse-engineering project. Not affiliated with Jasc or Corel.

## Layout

```
core/             libpsp9core: image model, layers, document, commands, undo, codecs
app/              psp9: the ImGui desktop app (canvas, palettes, tools, menus)
tests/            core unit tests (ctest)
docs/             notes from studying the original, command inventory
third_party/      vendored single-header libs (stb)
WindowsInstall/   backup of the original PSP9 install (gitignored, reference only)
```

The original's structure is mirrored on purpose. PSP9 was built as a command
layer (`JascCmd*.dll`, one per menu category) driving a document model, with
tools (`JascTool*.dll`) and palettes on top. See `docs/COMMANDS.md`.

## Build

```sh
sudo apt install libsdl2-dev libgl-dev   # Ubuntu
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/app/psp9 [image.png]
```

Dear ImGui (docking branch) is fetched by CMake on first configure.
