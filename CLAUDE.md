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
- `reference/` holds gitignored shallow clones of GIMP and Krita (GPL) for
  studying designs. Never copy code from them: Firn is Apache-2.0, so every
  feature inspired by them is written independently from the idea.

## Build and test

```sh
cmake -S . -B build -G Ninja      # fetches Dear ImGui and libwebp on first configure
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
scripts/ drive.py drives the running app over its FIRN_DRIVE socket (virtual cursor, screenshots, state).
assets/  the icon (icon.png full size; icon-128.png is embedded as the window icon at build time, firn.ico/.rc for the Windows exe).
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
  texture. Anything that changes pixels or layer state must `touch()`; live
  tools pass the changed rect (`touch(rect)` / `App::paint_touched(layer,
  &rect)`) so only that area is recomposited and uploaded.
- **Straight-alpha RGBA8** everywhere in `Image`. Blur and resample in
  premultiplied space internally (see `raster::gaussian_blur`) so transparent
  pixels don't bleed color.
- **Brush opacity is per stroke, not per stamp** (`raster::Stroke` keeps a
  max-coverage mask). Do not "fix" overlapping stamps by blending them.
- The **Background layer** (`Layer::background`) has no transparency: the
  eraser paints the background color on it and clears alpha elsewhere.
- **Selections are a `Mask` on the `Document`** (empty mask = none). Change
  it only through `SelectionCommand` (`App::set_selection`) so it is undoable.
  The Selections menu, its Modify dialogs, Edit Selection mode and
  float/defloat live in `app/src/ui/SelectionMenu.cpp`; the mask operations
  behind them are in `core/include/firn/mask.h`.
  `LayerPixelCommand` clips its result to the selection automatically via
  `raster::apply_through_mask`; tools pass `&doc->selection()` as the clip to
  `raster::Stroke` / `raster::flood_fill`. New pixel commands get this for
  free; new tools must opt in.
- **Dialog parameters do not belong on `App`.** The Adjust and Effects
  dialogs keep theirs in `AdjustState` (`app/src/ui/AdjustState.h`), which
  only `Adjust.cpp` and `App.cpp` include; `App` holds it by pointer.
  Reach it as `adjust_state->x`, never through a local reference: the
  Effect Browser and Edit > Repeat store these lambdas and would outlive
  one. Put new single-dialog state there rather than in `App.h`, which 25
  translation units include.
- **Adjustment and effect dialogs** live in `app/src/ui/Adjust.cpp` and use
  `adjust_modal(app, title, body, op)`: `body` draws widgets and returns
  true on change, `op` applies the parameters to an `Image`. The preview
  session re-applies `op` to the active layer live (on slider release for
  layers over 1 MP), OK commits one `LayerSnapshotCommand`, Cancel restores.
  Add new pixel operations to `core` (`adjust.h` for color, `raster.h` for
  spatial) with a test, then one `adjust_modal` call and a menu item.
  Instant menu items use `AdjustCommand(layer, name, fn)`.
- **Canvas-size changes** derive from `GeometryCommand`: implement
  `transform(in, out)` producing every layer at the new size plus the
  selection; undo restores a full `Document::State` snapshot.
- `docs/ROADMAP.md` is the value-ranked work list. Tick items off as they
  land and take the top unchecked item next.
- Index 0 is the bottom of the layer stack. Palettes list top first.
  Double-clicking a row's name renames it in place (`App::rename_layer`).
  Dragging a palette row onto another runs `MoveLayerCommand` through
  `App::layer_move_onto`: the moved block lands directly above the target
  at the target's depth (a group target is stepped over whole).
- **Groups** are `LayerType::Group` layers with `depth`; their members are the
  run of layers immediately above them with a greater depth
  (`Document::group_end`). Commands that restructure layers snapshot the
  whole stack (`Document::State`). A layer's optional `mask` multiplies its
  alpha (or the group's composite) during compositing. Tools and pixel
  commands must check `Layer::is_raster()` / `App::active_is_raster()`.
- **Lock transparency** (`Layer::lock_alpha`) is the original's
  "transparency protected" and uses that format's own byte, not the stash.
  `LayerPixelCommand` puts back every pixel that was clear before the
  operation (`raster::restore_clear_pixels`), and `App::paint_clip` narrows
  a tool's clip to the pixels that already exist.
- **Firn-only adjustments** are kinds >= 50 (`Adjustment::is_firn_only()`);
  filters are the subset >= 100. The native container writes any of them as
  an empty placeholder layer plus the stash, which carries the whole
  `Adjustment::to_json()`, because the original's adjustment blocks cannot
  express them. Gradient Map (50) is the first non-filter one.
- **Pass-through groups** (`Layer::pass_through`, `composite_pass_through`):
  the members draw onto a copy of the backdrop so an adjustment or filter
  inside the group reaches what is below it, and the group's opacity and
  mask then mix that copy back over the original. Its blend mode and style
  do not apply: there is no isolated shape to apply them to. Filters inside
  need `CompositeOpts::filters_from_out` so they work on the buffer rather
  than re-compositing from the group's first member.
- **Blend ranges** (`Layer::ranges`, `BlendRanges` in `blend.h`) limit a
  layer to a range of its own tones or of the tones below it. `blend_rows`
  turns them into a coverage factor that multiplies the source alpha, the
  same way the mask does, so the two compose. The original has a slot for
  these in its layer info that no sample exercises, so Firn does not write
  it; see docs/FORMAT.md before trying.
- **Clipping masks** (`Layer::clipped`): a run of clipped layers above a
  layer is one unit (`Document::clip_end`, `composite_clip_unit`). They are
  drawn onto the base's pixels, held to its alpha and mask, and the unit
  blends with the base's own opacity, blend mode and style. Compositing the
  unit's members passes `top_clips = false` so their own flag is not
  re-read, and a clipped filter layer takes its source from the unit rather
  than re-compositing what is below (`apply_filter_layer`'s
  `source_is_out`). It is Firn's own, so it rides in `firn:clipped` and the
  native format's stash.
- **Mask edit mode**: tools never touch `layer.pixels` directly; they use
  `App::paint_pixels(layer)` (the pixels, or a grayscale proxy of the mask
  while editing it), `App::paint_touched(layer)` after live edits, and
  `App::commit_pixels(...)` to record the gesture. That is what makes every
  painting tool work on masks for free.
- New tools go in `app/src/tools/Tools.cpp` and register in
  `make_default_tools()`; give them a single-letter `shortcut()` matching the
  original where one exists (A pan, Z zoom, S selection, E dropper, B brush,
  X eraser, F fill). L freehand and W magic wand are ours; the original put
  those on the S flyout.
- **Guides and painting assistants** live on the `Document`
  (`guides_h()`, `guides_v()`, `assistants()`), so they travel with the
  image and the project format saves them; `App::guides_h()` and friends
  forward to the current document. They are not part of the undo state.
- **OpenRaster (.ora)** is the project format (`core/src/io_ora.cpp`,
  zip in `core/src/zip.cpp`) and it is **lossless**: everything the
  Document holds round-trips exactly, down to the active layer, the live
  selection, every field of an Adjustment (not just the active kind's) and
  the whole vector object model. Firn-only data goes in `firn:` attributes
  (docs/FORMAT.md, "OpenRaster"). Vector layers use Firn's own encoding,
  `core/src/io_vec.cpp` (`io::encode_objects` / `decode_objects`), which is
  the only thing that can hold dash arrays, pattern and texture images,
  per-object visibility and fractional point sizes.
  **Adding a field to the document model means adding it there too**, and
  extending `test_openraster_lossless` / `test_openraster_vectors`, which
  set every field to a non-default value and would otherwise pass on a
  default that happens to match.
  The classic native format stays fully supported for the original, at the
  original's fidelity: `test_psp_vector_compat` pins what it carries and
  what it cannot, and `scripts/original-open.sh` is the real check. Add new
  Firn-only state to the .ora writer and, where the original can tolerate
  it, the Firn stash.
- **Native format reading** lives in `core/src/io_psp.cpp`; `docs/FORMAT.md`
  is the reference and must be updated when the reader learns a new block.
  `tests/test_psp_corpus.cpp` loads every sample under `WindowsInstall/`
  (skips when absent) and round-trips each through the writer; run it after
  any reader or writer change. Open files through `io::load_document` and
  save through `io::save_document`; both dispatch on extension.
- **The original runs under Wine** from `WindowsInstall/`.
  `scripts/original-open.sh file.pspimage shot.png` opens a file in it and
  captures the window: the definitive check for anything the writer emits,
  and a way to observe the original's behavior when a port detail is unclear.
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
- `cmake/version.cmake` regenerates `Version.cpp` (version, commit, build
  date) on every build for Help > About (`app/src/ui/About.cpp`).
- **Themes** (`app/src/ui/Theme.h`): a `Theme` is every ImGui color, the
  shape values, and the font path and size; built-ins come from
  `Theme::builtins()`, user themes are key=value `.firntheme` files under
  the config folder. `App::apply_theme` sets the style at once and queues a
  font rebuild that `App::apply_pending_font` performs between frames
  (never inside a frame). The editor is `app/src/ui/ThemeEditor.cpp`.
- Settings persist through `Config` (`~/.config/firn/firn.cfg`; the ImGui
  layout is `layout.ini` beside it). Add new persisted fields there, expose
  them in File > Preferences, and push them into live state in
  `App::apply_config`.
- **Libraries** (picture tubes, brush tips, paper textures, preset shapes,
  gradients, styled lines, patterns) are scanned lazily by `App::ensure_*`
  from `~/.config/firn/{tubes,brushes,textures,shapes,gradients,lines,patterns}`,
  the `FIRN_*_DIRS` env vars, the Preferences folders, and the gitignored
  sample folders under `WindowsInstall/` in a development build. Tubes are
  the one kind Firn also writes (`io::save_psp_tube`, File > Export >
  Picture Tube).
- **Vector layers** (`LayerType::Vector`) hold `vec::Object`s
  (`core/include/firn/vector.h`) and a rendered pixel cache. Every edit goes
  through `VectorEditCommand` (before/after object lists); tools mutate
  `Layer::objects` live, call `Document::rasterize_vector_layer`, and commit
  with `App::objects_changed`. Selection is `Object::selected` (never
  saved); groups are a group object followed by its members
  (`vec::group_end`). Node editing is `vec::break_path` / `join_paths` /
  `reverse_path` in core, driven by `App::node_break` and its neighbours;
  the node the Pen tool has picked lives on `App` (`node_object`,
  `node_path`, `node_index`) so the `object.*` actions in
  `app/src/VectorActions.cpp` act on the same one.
  Shape, line and text tools build objects in both
  modes; "Create as vector" keeps them editable, otherwise
  `vec::rasterize` paints them through the selection. Text objects keep
  their `TextInfo` (text, font, size, insert point) and are written as the
  original's text shapes (`docs/FORMAT.md`, "Text shapes", rotation in the
  deformation matrix), so they stay
  editable after a reload in both programs; `vec::text_outline_paths`
  lays them out and `Object::transform` keeps the insert point in step.
- **Filter layers** are adjustment layers with `Adjustment::is_filter()`
  (kinds >= 100, ours only): spatial ops with a `reach()`, composited by
  `Document::apply_filter_layer` over a padded rect; `touch(rect)` grows
  the dirty rect by `filter_reach()`. The native writer emits them as empty
  placeholder layers plus the Firn stash in the creator description
  (docs/FORMAT.md, "Firn stash"); put other Firn-only data there too.
- **Layer styles** (`Layer::style`, `core/include/firn/layerstyle.h`) are
  rendered by `render_layer_style` during compositing over the padded
  rect; edit through `SetLayerStyleCommand`; the dialog is
  `app/src/ui/LayerStyles.cpp`. Saved through the Firn stash.
- **Adjustment layers** (`LayerType::Adjustment`, `Layer::adjustment`,
  `core/include/firn/adjustment.h`) transform what is composited below them
  (within their group) through their mask and opacity. Create with
  `AddAdjustmentLayerCommand`, edit with `SetAdjustmentCommand`; the dialog
  in `app/src/ui/AdjustmentLayer.cpp` edits the layer live and commits on OK.
- **The API manual is generated.** After adding or changing an action, run
  `python3 scripts/gen_api_docs.py`, which rewrites `docs/API-reference.md`
  and `docs/api.json` from the running program; CI fails when they drift.
  Give every parameter a summary, and mark it required, or give it choices
  and a fallback, because that metadata is what the manual and the JSON
  Schema are made of.
- **Actions** (`app/src/Actions.h`): every menu item and tool option worth
  automating is registered with a dotted name, a summary and typed
  parameters, and calls the same `App` method the menu does. `describe`
  reports the table, `firn-cli` (`tools/cli.cpp`) and the driver socket both
  dispatch through it, and `App::do_command` falls through to it after the
  original's command names. Add new UI here as well as in the menu, or the
  API falls behind. See `docs/API.md`.
- **The menu bar is one shared body, two renderers.** `App::draw_menu`
  (`app/src/ui/Menu.cpp`, plus `draw_selections_menu` and
  `draw_layer_menu_items`) describes the whole tree — File through Help —
  by calling a `MenuBuilder&` (`app/src/ui/MenuBuilder.h`) instead of
  `ImGui::` directly, so it never has two copies to keep in sync.
  `ImGuiMenuBuilder` draws it as the in-window bar (every platform, and
  every context menu — the Layers palette's right-click menu always uses
  this one). `NativeMenuBuilder` (`app/src/NativeMenu_mac.mm`, macOS only)
  reconciles a real `NSMenu` tree by position against what the same call
  sequence describes each frame; `main.cpp` skips the in-window bar there
  entirely. A disabled `begin_menu` must return `false` and not be entered,
  exactly like `ImGui::BeginMenu` — bodies dereference `doc`/`layer`
  unconditionally past their enabled check. New menu items go through `m.`,
  never `ImGui::`, in these three functions.
- **Scripting** (`app/src/Script.cpp`): `App::do_command(name, json)`
  implements the original's `App.Do` commands with its parameter names
  (the command API reference is linked from `docs/FORMAT.md`). The driver
  socket carries them (`do <Command> <json>`) and `scripts/firn-script.py`
  is the Python side that runs `.PspScript` files. Add a command by
  reading its parameter page and mapping it onto existing App functions;
  never invent parameter names.
- **The official format spec** (versions 7 and 8) and the scripting command
  API are linked from `docs/FORMAT.md` (References); consult them before
  reverse-engineering a block from samples.
- **16 bits per channel**: `Layer::deep` (`Image16`, shared between
  snapshots, so replace rather than mutate: `Layer::set_deep`) beside the
  8-bit display pixels. `LayerPixelCommand::apply16` runs a command at 16
  bits; commands without it drop the layer to 8 bits undoably. Adjust
  dialogs pass a 16-bit op to `adjust_modal` when they have one; tools
  paint at 8 bits (`LayerSnapshotCommand::capture_deep`).
- **Color management** (`core/include/firn/icc.h`): matrix/TRC profiles
  only. `Document::icc()` holds the embedded bytes (part of the undo
  state); `App::display_needs_transform` converts the composite to sRGB
  for the canvas texture; `io::read_icc` / `io::embed_icc` handle PNG and
  JPEG.
- **Update check** (`app/src/UpdateCheck.h`) reports a newer GitHub release
  and never downloads one: an unsigned binary that replaces itself can break
  an installation, and Windows cannot overwrite a running executable anyway.
  It shells out to curl or PowerShell, the way printing uses `lp`, so there
  is no HTTP dependency. Opt-in (`Config::check_updates`, off by default),
  once a day, and silent on every failure.
- **Pen input** (`app/src/Tablet.cpp`, `Tablet_mac.mm`): per-platform
  backends fill `App::pen` (pressure, tilt, eraser); the canvas copies
  pressure into `ToolInput` and `raster::Stroke` scales stamps per
  `set_pressure_response`. The driver simulates it (`set:pen_pressure`).
- **Materials**: `App::material_style(fg)` turns the color plus
  `App::Material` (gradient/pattern, optional texture, transparent switch)
  into a `vec::PaintStyle`; the flood fill, shape, line and text tools all
  paint through it. The Material Properties dialog and the palette's
  material boxes and pickers live in `app/src/ui/MaterialDialog.cpp`.
- **Metadata** (`core/include/firn/metadata.h`): Exif directories and PNG
  text chunks parse into `meta::Entry` values that keep their TIFF type and
  bytes, so an untouched entry round trips exactly. `Document::metadata()`
  holds them (part of the undo state); `io::read_metadata` / `embed_metadata`
  do the files, and `MetadataCommand` is the undoable edit. The native
  container has nowhere of its own for metadata, so it rides in the Firn
  stash (docs/FORMAT.md). Saving a JPEG or PNG writes a
  fresh Exif thumbnail (`io::exif_thumbnail`, IFD1) from the composite; the
  one the opened file carried is deliberately not reused, since after a crop
  it would still show what was cut away. The Metadata tab of
  Image > Image Information is in `app/src/ui/Menu.cpp`.
- **Saved selections** live in `Document::alpha_channels()` and round-trip
  through the native format; the current selection itself is not stored.
- The toolbar and status bar (`app/src/ui/Toolbar.cpp`) sit outside the
  dock space; the canvas reports cursor facts to the status bar.
- CI (`.github/workflows/build.yml`) builds and tests on Linux, Windows and
  macOS, runs `scripts/smoke.py` (the app under Xvfb through the driver)
  on Linux, and uploads packages built with CPack. Tags `vX.Y.Z` run
  `.github/workflows/release.yml` (AppImage via linuxdeploy, tarballs, zip,
  checksums, GitHub release with the CHANGELOG section as notes);
  `scripts/release.sh` makes the tag. Keep CHANGELOG.md's Unreleased
  section current.
  Keep the code portable: no GCC-only flags outside the `if(NOT MSVC)`
  blocks, NOMINMAX is defined project-wide, `main()` is plain (SDL's
  entry point is disabled).
- **Slow work off the interface thread**: `App::job` (`BackgroundJob.h`) is
  one operation on a worker, behind a modal that shows progress and offers
  Cancel. The modal is load-bearing: the document cannot change while the
  worker reads it, so there is no stale-result case to handle. The worker
  writes into its own `result` image, which the main thread touches only
  once the future is ready, and hands it to a command that copies rather
  than recomputes. Core operations take the cancel as an `on_progress`
  callback returning false. Actions stay synchronous
  (`content_aware_fill(false)`) because a script expects the work finished
  when the call returns, and `App::do_command` refuses every action while a
  job is running so nothing can change the document under the worker.
  Interactive saves and opens take the same route (`save_document_async`,
  `open_document_async`, used by File > Save, Save As, Open, the recent
  lists and drag-and-drop); the close prompt, the command line, the driver
  and scripts stay on the synchronous `save_document` / `open_document`
  because they act on the result immediately. `after_saved` is the
  bookkeeping both save paths share. Opening is the safe one: the worker
  builds a document of its own and touches nothing the main thread owns.
- **Blur fidelity**: `raster::gaussian_blur` is a true Gaussian and stays
  one. Substituting three box passes is much faster and is deliberately
  rejected: it changes the result, and with it layer styles, filter layers
  and unsharp mask (docs/ROADMAP.md, "Blur fidelity"). `sigma = radius` is
  verified against the original: an impulse blurred at Radius 10 in both
  gives profiles within 2 levels of each other.
  `mask::feather` uses `sigma = radius / 2` and that is verified too: the
  two words mean different things and Firn matches the original on both,
  so do not "fix" the feather to agree with the blur.
- **Measuring the original**: when a port detail is in doubt, run the
  original under Wine on a private `Xvfb :99` and drive it with
  python-xlib's XTest. No input reaches the real session, so this does not
  break the rule about synthetic input, which exists so the program never
  fights the user for their own pointer. `docs/ROADMAP.md` section 20 has a
  worked example: an impulse image, a known radius, and the kernel read
  back off the canvas.
- **Content-aware fill** (`core/include/firn/inpaint.h`) synthesizes a
  region from the rest of the image by PatchMatch. Two things it is easy to
  get wrong and that its test pins down: the patch distance must weight
  known pixels far above ones still being synthesized, or a flat seed
  matches flat regions and the fill stays flat; and a pyramid level whose
  hole leaves no whole patch of known image must be dropped, not treated as
  a failure of the whole fill.
- **Pixel loops run across cores** through `firn::parallel::rows`
  (`core/include/firn/parallel.h`), which runs small jobs inline so short
  operations pay nothing. Most effects go through three shared passes in
  `core/src/effects_util.h` (`rgb_pass`, `remap`, `remap_edges`) which are
  banded by row, so **the function passed to them must not keep state
  across pixels**: the median filter used to hold one window buffer outside
  its lambda, which becomes a race the moment the pass is threaded. When
  changing any of this, prove the output is unchanged rather than assuming
  it, by hashing each operation's result before and after. The 16-bit side
  goes through `raster16::map_rgb` and has the same rule.
  **Check it with ThreadSanitizer**, which needs address randomisation off
  on this kernel:
  `cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_FLAGS=-fsanitize=thread
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -DFIRN_BUILD_APP=OFF` then
  `setarch $(uname -m) -R ./build-tsan/tests/test_core`. It found the one
  race that was there: `encode_png` set stb's global compression level per
  image while the project writer encoded layers in parallel.
- **Run the suites under sanitizers when touching anything that parses a
  file or spawns a thread.** Both builds are worth keeping around:
  `-fsanitize=address,undefined` for the readers, and
  `-fsanitize=thread` (with `setarch $(uname -m) -R`) for the workers.
  Between them they have found a use-after-free in the native reader, an
  unbounded allocation from a corrupted size field, and a data race in the
  PNG writer, none of which any test noticed on its own.
  `scripts/memory_audit.py` drives the running program through repeated
  open/close, edit/undo and save/reopen cycles and watches its resident
  size, because LeakSanitizer only sees memory that became *unreachable*
  and an editor's real failure is memory that stays reachable and grows.
  Audited 2026-09-12: cycles flat, history plateaus at `undo_memory_mb`
  (1024 per document), documents fully released. A few MB of creep across
  many rounds is glibc holding freed pages, not a leak; the sanitizer
  reported the same fixed 256 bytes from a graphics driver whether 1 or 36
  documents had been opened.
  `test_parsers_survive_broken_files` feeds every byte-level parser
  truncations, corrupted headers and noise; it is cheap and belongs in the
  ordinary run, but it only catches memory errors when built with the
  sanitizer.
- Add a test in `tests/test_core.cpp` for every new raster op or command.

## macOS

ImGui swaps Cmd and Ctrl on Apple (`ConfigMacOSXBehaviors`, set from
`__APPLE__`), so `io.KeyCtrl` is what a Mac user pressing **Cmd** produces
and the app's shortcuts are already right there. The driver has to send
Super for a script's `ctrl` so ImGui swaps it back, which
`app/src/Drive.cpp` does; without it every scripted shortcut silently does
nothing on that platform. The smoke and behaviour suites run on the macOS
CI job for exactly this reason: building there proves nothing about
whether the program works.

## Checking UI changes

`scripts/app_tests.py` is the app's test suite: 106 checks over documents,
the view, layers, selections, painting and materials, the edit actions,
tools and history, image geometry, metadata and the action API itself,
asserting on the JSON the program reports and on pixels it writes. It runs
in CI beside `scripts/smoke.py`. Anything living in `App` is covered by this and nothing
else, so add a case here when you change behaviour there, and run it before
and after any restructuring:

```sh
xvfb-run -a python3 scripts/app_tests.py build
python scripts\app_tests.py build          # Windows: no Xvfb, a small window appears
```


Unit tests cover the core. For the app, use the in-app driver
(`app/src/Drive.cpp`): with `FIRN_DRIVE=<socket>` set, the app listens on a
Unix socket (on Windows a token-guarded loopback port named by an address
file at that path, `app/src/DriveAddress.h`, because Python there has no
`AF_UNIX`; `drive.open_socket` hides the difference), moves a **virtual cursor** with synthetic ImGui events (the
real pointer is never touched and real mouse events are ignored while
driving), writes screenshots from its own framebuffer, and answers every
command with a state line once its frames have run, so scripts never sleep
or guess:

```sh
python3 scripts/drive.py --launch some.png          # kills old instances, starts small, waits for the socket
python3 scripts/drive.py "tool:Preset Shape" set:create_as_vector:1 drag_img:20,20:140,140 state
python3 scripts/drive.py ctrl:z shot:/tmp/out.png save:/tmp/out.pspimage
python3 scripts/drive.py --kill                      # when done
```

`*_img` steps take image pixel coordinates (the state line reports `origin`
and `zoom` for the rest); `tool:NAME` and `set:OPTION:VALUE` replace hunting
for widgets; `state` reports the tool, active layer, object selection,
history, open popups and status. **Always `--launch` (or `--kill`) before a
run and `--kill` when done** (SIGKILL: SDL used to turn SIGTERM into a quit
request that parked a modified image on the unsaved-changes prompt). Never
launch the app maximized; the user's screen is 3440 px wide. The old
XTest-based driving is gone: it fought the user for the mouse.
