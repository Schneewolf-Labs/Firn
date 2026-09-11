# Building Firn from source

## Dependencies

Firn needs a C++20 compiler, CMake 3.22 or newer, SDL2 and OpenGL. Dear ImGui
(docking branch) and libwebp are fetched by CMake on the first configure; the
stb single-header libraries are vendored in `third_party/`.

```sh
sudo apt install libsdl2-dev libgl-dev libxi-dev   # Ubuntu and Debian
brew install sdl2                                  # macOS
```

`libxi-dev` is optional and enables pen pressure on X11.

## Build and run

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/app/firn samples/luca.jpg
```

Warnings are errors in spirit: the build runs with `-Wall -Wextra -Wpedantic`
(`/W3` on MSVC) and is expected to stay silent. The Windows CI job enforces
it with `/WX`; pass `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` to do the same
locally.

### Windows

Use Visual Studio 2022 (the CMake it bundles is new enough) and either
vcpkg (`vcpkg install sdl2:x64-windows`, then pass its toolchain file as CI
does) or SDL's own `SDL2-devel-<version>-VC.zip` from the SDL releases page,
unpacked anywhere; `build-deps/` is gitignored:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSDL2_DIR=build-deps/SDL2-2.32.10/cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\app\Release\firn.exe samples\luca.jpg
```

The build copies `SDL2.dll` next to `firn.exe`. The test suites below run
here too (`python scripts\app_tests.py build`); they find the programs under
`Release\`. On Windows `FIRN_DRIVE` names a small address file rather than a
Unix socket, because Python there cannot open one: the app listens on a
loopback port and writes that port and a random token into the file, and
clients present the token first (`app/src/DriveAddress.h`). `drive.py` and
`firn-cli` handle this for you.

## Install

```sh
cmake --install build --prefix ~/.local   # per user: ~/.local/bin/firn
sudo cmake --install build                # system wide: /usr/local
```

This installs the app, `firn-convert`, `firn-cli`, a desktop entry and the
icon.

## Repository layout

```
core/             libfirncore: image model, layers, document, commands, undo, codecs
app/              firn: the ImGui desktop app (canvas, palettes, tools, menus)
tools/            firn-convert and firn-cli
tests/            core unit tests (ctest)
scripts/          the socket driver, the test suites, doc generation, release
docs/             formats, the action API reference, the roadmap, notes on the original
third_party/      vendored single-header libraries (stb)
```

The module split mirrors the original program on purpose: a command layer per
menu category driving a document model, with tools and palettes on top. See
[COMMANDS.md](COMMANDS.md).

## Tests

| Suite | What it covers |
| --- | --- |
| `ctest --test-dir build` | core unit tests and the native-format corpus |
| `python3 scripts/smoke.py` | the real app through its socket driver, end to end |
| `python3 scripts/cli_tests.py` | CLI JSON files, launch/reuse, and socket isolation |
| `python3 scripts/app_tests.py` | the action API, one assertion per behavior |
| `python3 scripts/gen_api_docs.py --check` | the generated API manual is current |

CI runs these suites on Linux, Windows (under Mesa's software OpenGL) and
macOS; the manual check runs on Linux. Every
push to `main` leaves Linux and Windows packages as workflow artifacts.

## Checking interface changes

The app can drive itself. With `FIRN_DRIVE` set it listens on a Unix socket,
moves a virtual cursor with synthetic events, and writes screenshots from its
own framebuffer, so a change can be verified without touching the real mouse.

```sh
python3 scripts/drive.py --launch some.png
python3 scripts/drive.py "tool:Paint Brush" drag_img:20,20:140,140 shot:/tmp/out.png
python3 scripts/drive.py --kill
```

## Releasing

```sh
scripts/release.sh 0.3.0   # bumps the version, dates CHANGELOG.md, commits, tags, pushes
```

The tag triggers the release workflow, which builds the AppImage, the
tarballs and the Windows zip, checksums them, and publishes a GitHub release
whose notes are that version's CHANGELOG section.

### macOS computer-use checks

The build creates both `build/app/firn` and `build/app/Firn.app`. The bundle
uses the same executable; open it for accessibility-driven UI testing.
Cocoa pointer events retain their event-local coordinates and button order,
including when an automation client posts an entire drag at once. Verify
New/OK clicks, a continuous brush drag, undo/redo, and filename text editing.
Native menu shortcuts defer to text fields and modal dialogs so Cmd+A/C/V
edit the field rather than the document. Physical mouse, tablet and
multi-monitor behavior need manual checks on the relevant hardware.
