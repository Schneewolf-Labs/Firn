# Roadmap

Ranked by value: first what any image editor is unusable without, then by
how often the original's bundled scripts call each command (docs/COMMANDS.md),
then by effort. Items are checked off as they land; the top unchecked item is
what to work on next.

## Done

- [x] Document model, layers, undo/history, PNG/JPEG/BMP/TGA I/O
- [x] Paint brush, eraser, flood fill, dropper, pan, zoom
- [x] Selections: rectangle/ellipse, freehand, magic wand, modify, marching ants
- [x] Clipboard: cut, copy, clear, paste as new layer / new image
- [x] File browser dialog; save format by extension
- [x] Layer blend modes, properties, duplicate/arrange/merge/promote
- [x] Read the native container (.PspImage/.PspTube/.PspFrame)

## 1. Image geometry (essential)

- [x] Crop to selection and a Crop tool
- [x] Resize with nearest / bilinear / bicubic, by pixels or percent, aspect lock
- [x] Canvas size with anchor
- [x] Rotate 90/180/270 and free rotate

## 2. Native format writer (essential for layered work)

- [x] Save .PspImage (version 6.0, zlib) with layers, opacity, blend, visibility
- [x] Save/Save As default to the native format when a document has layers

## 3. Adjustments with live preview (the most-called script commands)

- [x] Preview framework: dialogs apply to the layer live, commit on OK, restore on Cancel
- [x] Colorize, Hue/Saturation/Lightness, Levels, Curves
- [x] Posterize, Solarize, Threshold, Channel Mixer
- [x] Auto contrast, histogram equalize/stretch
- [x] Brightness/Contrast and the blurs move onto the preview framework
- [x] Color Balance (shadows/midtones/highlights), Hue Map, Sepia Toning
- [x] Gamma per channel, Red-eye removal (tool), Fade Correction

## 4. Effects

- [x] Add Noise, Sharpen / Sharpen More / Unsharp Mask
- [x] Median, Motion Blur, Blur More, Soften
- [x] Find Edges, Enhance Edges, Emboss, Erode / Dilate
- [x] Drop Shadow, Mosaic (pixelate)
- [x] Inner Bevel, Cutout, Buttonize, Sepia, Wave, Pinch/Punch, Twirl
- [x] Outer Bevel, Chrome, Halftone, Ripple, Spherize, Lens Distortion
- [x] Kaleidoscope, Sunburst
- [x] Page Curl, Pattern, Lights, Fur, Weave (with the rest of the Effects menu)

## 5. Retouch and paint tools

- [x] Move tool (layer offset), Clone brush
- [x] Airbrush, Lighten/Darken, Saturation, Hue, Color Replacer
- [x] Smudge / Push, Dodge / Burn, Soften / Sharpen brushes
- [x] Round and square brush shapes
- [x] Picture Tube tool (.PspTube cells, scale, step, placement, selection modes)
- [x] Custom brush tips (.PspBrush and PNG tips, tip from selection)
- [x] Paper textures under strokes (.bmp/.png tiles, strength)

## 6. Text and shapes

- [x] Text tool (vendor stb_truetype; rasterized to a new layer)
- [x] Line and preset shapes drawn as raster
- [x] Text stroke/outline and rotation; rounded rectangle, polygon and star shapes
- [x] Text editing after placement (vector text objects: Objects > Edit Text, or the properties dialog)

## 7. Workspace

- [x] Multiple open documents (tabs), recent files, remember window and dialog state
- [x] Rulers, grid
- [x] Guides (drag from rulers), snap to guides and grid
- [x] Tools palette grouped by the original's categories
- [x] Windows build verified by CI (GitHub Actions, MSVC + vcpkg SDL2)

## 9. Performance (large photos)

- [x] Dirty-rect compositing and sub-texture uploads while painting
- [x] Integer fast path for Normal blending; row-parallel compositing (12 MP x 3 layers: ~50 ms full, ~1 ms per brush flush)
- [x] Live previews on large layers: visible region while dragging, downscaled proxy when the whole image is in view, exact result on release

## 10. Polish

- [x] Keyboard zoom (+ / -, Ctrl+0 fit, Ctrl+Alt+0 actual), Image Information dialog, JPEG quality prompt
- [x] Toolbar with common actions and materials; status bar with message, cursor, zoom and image facts
- [x] Preferences dialog (undo limit, JPEG quality, checkerboard, new-image size, view options, library folders)

## 8. Format completeness

- [x] Masks attached to groups and layers, kept editable (enable, invert, delete, from selection/image)
- [x] Layer groups in the model, palette (indent, collapse) and file round trip
- [x] Selections: load from / save to disk (.PspSelection or any image)
- [x] Alpha channels (saved selections) read, written, loaded and saved from the Selections menu
- [x] Painting directly on masks (Edit button / Layers > Mask > Edit Mask)
- [x] Mask overlay view (red tint over hidden areas while editing)

## 11. MVP: the rest of what people use (in order)

Parity target: every tool and command a regular user of the original reaches
for. Ranked by value per effort; work top to bottom, tick as they land.

- [x] Adjustment layers: Brightness/Contrast, Levels, Curves, HSL, Color
      Balance, Channel Mixer, Invert, Threshold, Posterize; live, editable from
      the palette and Layers > Properties, native read/write per the spec
- [x] Deform tool (move/scale/rotate/skew/perspective a layer), Straighten,
      Perspective Correction (all layers or one, optional crop)
- [x] Image menu odds and ends: Add Borders, Picture Frame (.PspFrame, inside
      or outside), Count Colors, Decrease Color Depth (2/16/256 with error
      diffusion, median cut), palettes (JASC-PAL load/save), Split/Combine
      Channel (RGB, HSL, CMYK), Arithmetic
- [x] Photo fixes: One Step Photo Fix, Automatic Color/Contrast/Saturation
      Enhancement, Clarify, Black and White Points, Histogram Adjustment,
      Salt and Pepper, JPEG Artifact Removal, Fill Flash, Backlighting,
      Chromatic Aberration Removal, Digital Camera Noise Removal (bilateral)
- [x] Warp Brush (push, expand, contract, twirl, noise, iron out) and Mesh Warp;
      Scratch Remover, Object Remover (selection + source rectangle, feathered)
- [x] Effects long tail: every entry of the original's Effects menu now has a
      live-preview dialog (distortion, geometric, reflection, image, artistic,
      illumination, texture, art media, User Defined Filter)
- [x] Materials palette parity: swatches (JASC-PAL, saved in ~/.config/firn),
      recent colors, Black/White reset, "all tools" lock
- [x] Scripting: App.Do commands over the driver socket (about 120 of the
      original's commands with its parameter names), scripts/firn-script.py runs
      .PspScript files unmodified with the original's JascUtils helpers,
      terminal prompts for GetString/GetNumber; Thumbnail_150, CenterLayer and
      SimpleCaption from the original run end to end
- [x] Plain printing: File > Print (Ctrl+P) writes a one-page PDF (paper, orientation,
      margins, fit or scale) and hands it to the system spooler, or saves the PDF
- [x] 16-bit channels: Image16 layer data beside the 8-bit display pixels, exact
      16-bit adjustments (levels, curves, brightness/contrast, gamma, HSL, colorize,
      color balance, channel mixer, threshold, posterize, invert, grayscale, blur,
      fill), geometry, 48-bit native and 16-bit PNG I/O; 8-bit-only operations
      reduce the layer undoably with a status note
- [x] Color management: ICC matrix/TRC profiles parsed from PNG (iCCP) and JPEG
      (APP2) and embedded on save, built-in sRGB / Adobe RGB / ProPhoto, Assign and
      Convert to Profile (8- and 16-bit), color managed display (composite converted
      to sRGB for the screen), profile shown in Image Information

## 12. User feedback (2026-09-09, first hands-on session)

- [x] Drag a file onto the window to open it
- [x] Save As: a file type list
- [x] Windowed view of all open images with independent zoom (Window >
      Tabbed Documents off; Cascade, Tile Horizontally, Tile Vertically)
- [x] Tool icons in the Tools palette
- [x] The full Material Properties dialog (color, gradient, pattern, texture,
      swatches, foreground/background, lock)
- [x] Thumbnails in the file dialog (lazy, two per frame, plus a preview pane)
- [x] Freehand Selection types: Point to Point, Smart Edge, Edge Seeker, smoothing;
      Selection tool shapes (square, rounded, circle, polygons, star, arrow)
- [x] Selections menu completed: From Mask, From Vector Object, Matting (remove
      black/white matte, defringe), Modify (inside/outside feather, unfeather,
      specks and holes, color range, select similar, shape-based anti-alias,
      smooth), Hide Marquee, Edit Selection, Promote Selection to Layer, Float, Defloat
- [x] Right-click menu on layers (the Layers menu for that layer, plus View > Current Only / All)
- [x] Help > About (version, commit, build facts) and themes: built-ins plus a theme
      editor (colors, shape, font, text size) with save, import and export
- [x] Windowed view polish: a Windows button on the tab bar, a Tabs / Cascade / Tile
      strip over the workspace, windows kept inside it, zoom in every title, and a
      right-click menu on tabs and window titles (Image Information, fit, save, close)

## 13. Borrowed from the open-source editors (2026-09-10)

Ideas taken from studying GIMP and Krita (reimplemented, not copied):

- [x] Heal Brush (seamless clone per stamp)
- [x] Brush smoothing: Basic, Weighted, Stabilizer
- [x] Color to Alpha
- [x] Symmetry painting: mirror axes, rotational, kaleidoscope
- [x] Foreground Select (matting from a rough scribble)
- [x] Unified transform gizmo (Deform: selection-aware, pivot, numeric entry, flips)
- [x] Color smudge brush (paint that mixes with what it passes over)
- [x] Perspective and vanishing-point assistants for the brushes
- [x] Non-destructive filter layers (Gaussian Blur, Average, Unsharp Mask)
- [x] Layer styles (drop shadow, glow, bevel, stroke) kept editable
- [x] OpenRaster (.ora) as the project format; the classic format kept for the original

## 14. Next (2026-09-10, after the format work)

- [ ] Check .ora against files written by GIMP and Krita on a machine that
      has them; ours is validated only against files we write and a
      hand-built foreign one
- [ ] Foreground Select: a real matting pass so hair and soft edges come
      out feathered rather than hard
- [x] Let a stroke choose which assistant it follows (Tool Options: Follow)
- [ ] Layer style presets, and styles that scale when the image is resized
- [ ] Lighter PNG compression for large .ora saves (about a second for a
      3 MP project today, most of it the layer and merged PNGs)
- [x] Filter layer masks in the classic format

## 15. Conveniences and compatibility (2026-09-10)

From an audit of our menus against the original's, and of `App.Do`
against the 115 commands its own bundled scripts use.

- [x] Edit: Copy Merged, Paste Into Selection, Repeat last effect
- [x] File > Revert, View > Zoom to Selection, rename a layer in place
- [x] Script commands: 69 of 115 implemented, now 96
- [x] Edge Preserving Smooth
- [ ] Export Picture Tube (we read tubes but cannot make one)
- [ ] Duplicate Window: two views of one image at different zooms. Needs
      shared document ownership; App and DocState each own theirs outright
- [ ] Vector node editing: Convert to Path, Add Path, node-level edits
      (4 script commands wait on this)
- [ ] The last script commands are runner plumbing (StartForeignWindow,
      GetString, EventNotify, the preferences and file-location queries)
      and two whose mapping is ambiguous (CombineRGB, MoveSelection)

## 16. Beyond the original (2026-09-11)

- [x] Content-Aware Fill (exemplar synthesis, `core/src/inpaint.cpp`)
- [ ] Run it off the UI thread: a few seconds of frozen window is the worst
      thing about it today
- [ ] Better structure in large holes: the fill is convincing over texture
      but can leave an edge where it has to invent shape. Onion-peel
      initialization and more search passes are the usual answers
- [x] Edge-directed upscaling (directional cubic convolution): better on
      graphic edges, a wash on photographs
- [ ] Neural upscaling or inpainting would mean shipping an inference
      runtime and weights; that is a dependency and licensing decision,
      not just a feature

- [x] Metadata: Exif and text notes read, edited and written back for JPEG,
      PNG and OpenRaster (`core/src/metadata.cpp`, Image > Image Information)
- [ ] IPTC and XMP: the other two metadata standards a photograph carries.
      XMP is RDF/XML in an APP1 segment, IPTC an IIM block inside a Photoshop
      resource. Reading both is a day's work; the editor UI is already there
- [ ] Keep the camera's embedded thumbnail rather than dropping it, by
      regenerating it from the edited picture on save
- [ ] Preferences switch for what leaves the machine: strip private metadata
      on every export, as a default rather than a per-image action
- [x] The project format is lossless: every field of the document model
      round-trips, audited field by field and pinned by three standing tests
      (`test_openraster_lossless`, `test_openraster_vectors`,
      `test_psp_vector_compat`). Vector layers use Firn's own encoding,
      `core/src/io_vec.cpp`
- [ ] Metadata does not ride in the native container yet. It would go in the
      Firn stash, which the original shows under image information and
      otherwise ignores
- [ ] The native container still loses what the original's shape layout
      cannot hold (dashes, pattern images, per-object visibility, fractional
      point sizes). That is the original's ceiling, not a bug; the stash
      could carry them for Firn's own re-reads if it ever matters

## 19. Borrowed from Photoshop (2026-09-11)

Ideas worth having, each written independently from the idea rather than
from any implementation, in the order they pay off.

- [x] Clipping masks (`Layer::clipped`, `Document::composite_clip_unit`)
- [x] Blend ranges, which turned out to be the original's feature too: its
      layer info has a slot for five of them, though no sample uses it, so
      Firn keeps its own in the stash (docs/FORMAT.md)
- [ ] Write blend ranges into the original's own slot, once the 8-byte
      layout is learned by setting one in the original under Wine and
      diffing the layer info chunk
- [ ] Pass-through group mode, so an adjustment layer inside a group can
      reach what is below the group
- [ ] Gradient map adjustment: map luminance through a gradient. The
      gradient model, its stops and the library already exist
- [ ] History brush: paint back from an earlier history state. The history
      snapshots already exist
- [ ] Smart objects: a layer that keeps its source and re-renders transforms
      and placed images from it. The other half of the non-destructive story
      that adjustment and filter layers started. A real project
- [ ] Content-aware scale (seam carving), the sibling of the fill
- [ ] Vector booleans: unite, subtract, intersect on objects
- [ ] On-canvas targeted adjustment: drag on the image to move the curve
      point for the tone under the cursor

## 17. Restructuring (2026-09-11)

- [x] Phase 1: per-dialog state off `App` (145 members, 11 owners). App.h
      733 lines / 379 member declarations -> 656 / 302
- [x] Phase 4: `scripts/app_tests.py`, 91 checks over the part that lives in
      `App`, which had 21 smoke checks over 14,800 lines
- [x] `describe` publishes the inherited command names too, generated from
      Script.cpp at build time so the list cannot drift
- [ ] Phase 2 (group the remaining members into Documents / View /
      Materials / Preferences): **measured and dropped.** About 2,000
      call-site edits, 1,200 of them for `doc` alone, for readability only:
      it reduces no coupling and no build time
- [ ] Trimming App.h's includes: **measured and dropped.** Removing any one
      saves 0.01 to 0.04 s, because they share transitive content App needs
- [ ] Phase 3 (the GUI calling the action layer rather than `App`): the only
      change that would move the 8.8 s rebuild, because that needs the 26
      files including App.h to stop. It wants a narrow interface for the UI
      to read state through, which is a redesign rather than a refactor.
      Worth doing when the UI surface stops growing, not before

Parsing App.h costs 0.59 s per translation unit: about 0.38 s of core
headers it genuinely needs and 0.2 s of its own declarations. That is the
budget any future attempt is working against.

## 18. macOS (2026-09-11, verified on hardware 2026-09-11)

Four real bugs were found and fixed by building and driving the app on
actual Apple Silicon hardware (not just CI). The previous note below was
wrong on the key point — worth recording why, so nobody re-derives it and
trusts it again:

- **The window never rendered.** `main.cpp` requested a 3.0 core GL context
  and compiled ImGui's shaders as `#version 130`. macOS never grants a 3.0
  core context: it silently promotes the request to its highest core
  profile (3.2+), which only accepts GLSL 150, so shader compilation failed
  at startup and the window stayed blank. Fixed with an `__APPLE__` branch
  requesting 3.2 core + `SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG` and
  `#version 150`, matching upstream ImGui's own SDL2+GL3 example.
- **Cmd shortcuts did nothing, for real users too.** The previous note
  claimed ImGui swaps Cmd and Ctrl itself via `ConfigMacOSXBehaviors`. It
  doesn't: that flag only changes widget-internal editing keys (word-jump,
  the Shortcut() helper, nav windowing). A physical Cmd press only ever
  sets `io.KeySuper`; `io.KeyCtrl` stays false. `App::handle_shortcuts`
  checked `io.KeyCtrl` directly, so Cmd+Z/Cmd+S/etc. were silently inert
  for every Mac user, not just the driver. Fixed in `App.cpp`:
  `ctrl = io.KeyCtrl || (io.ConfigMacOSXBehaviors && io.KeySuper)`. This is
  also what made the driver's one failing shortcut fail — the driver's
  Ctrl→Super translation (`app/src/Drive.cpp`) was already correct, it was
  faithfully reproducing the real bug.
- **Copying to the clipboard crashed (SIGSEGV).** `clipboard::write_image`
  called the shared `run()` helper with `nullptr` for both stdin and
  stdout, which fell into the stdout-capturing branch and dereferenced a
  null `output` vector. `~/Library/Logs/DiagnosticReports/*.ips` had the
  full backtrace once `DevToolsSecurity -enable` wasn't an option (no
  Developer Mode on the box) — a real macOS crash always leaves one there.
  Fixed with a genuine fire-and-forget branch in `run()`
  (`app/src/Clipboard.cpp`); verified the PNG actually lands on the real
  pasteboard afterward (`osascript -e 'clipboard info'`).
- **Retina screenshots (and the GL viewport generally) were wrong.**
  `main.cpp` set `glViewport` from `io.DisplaySize`, which is logical
  *points*, not the physical pixel size of the drawable — on a Retina
  display those differ by the HiDPI factor.
  `ImGui_ImplOpenGL3_RenderDrawData` uses the correct physical size
  internally for its own draw calls (so on-screen rendering was fine), but
  it saves/restores `GL_VIEWPORT` around itself, so after every frame the
  viewport was left at the wrong, too-small logical size. The driver's
  screenshot code trusted that leftover state (`glGetIntegerv(GL_VIEWPORT,
  ...)`) to size `glReadPixels`, so `shot:` only ever captured a
  logical-size crop of the real framebuffer — enlarged and cut off, not a
  clean downscale. Fixed both: `main.cpp` now sets the viewport from
  `SDL_GL_GetDrawableSize` every frame, and `Drive.cpp`'s
  `save_framebuffer` takes the `SDL_Window*` and reads the drawable size
  directly instead of trusting `GL_VIEWPORT`. Verified with a real
  screenshot at full 2x resolution and a click/drag selection landing on
  the exact image coordinates the ruler shows.

All four were confirmed with the project's own suites on hardware: `ctest`,
`scripts/smoke.py` (all 19 checks, including the previously-failing `undo
removed the selection`), and `scripts/app_tests.py` (all 94 checks,
including content-aware fill, which is what surfaced the clipboard crash).
The macOS CI steps went back to blocking in `.github/workflows/build.yml`
once this was verified.

Also done on the same pass: menu shortcut labels now show Cmd instead of
Ctrl on macOS (`app/src/ui/Shortcut.h`, `SC()`) — except drag modifiers
like "Ctrl while selecting" that are checked as the literal physical key in
`Tools.cpp` and were never routed through the Cmd-accepting shortcut
handler, which stay written as Ctrl because that's what they actually are.
And settings now land in `~/Library/Application Support/Firn`, not
`~/.config/firn` (`Config::directory()`).

**A fifth bug, found after a user report on real hardware and fixed the
same day**: the UI and text were far too big on Retina. `main.cpp`'s HiDPI
detection fed the drawable/window pixel ratio (2.0 on Retina) straight into
`auto_ui_scale`, which multiplies both the ImGui style sizes and the
requested font point size — so a 13 px font came out as 26, and panels wide
enough for a non-Retina display ate most of the window. That ratio is pixel
*density* (present on any high-DPI display, there purely so the OS can
render more sharply at the same logical size), not a "make text and
widgets bigger" preference — those are two different things that happened
to share one number. Split them: `App::font_density` (new field) feeds
`ImFontConfig::RasterizerDensity` so the font atlas rasterizes sharp
without changing the logical (point) size; `auto_ui_scale` now comes only
from genuine DPI/toolkit-scale signals (X11 `Xft.dpi`, `GDK_SCALE`,
`QT_SCALE_FACTOR`), which don't exist on macOS and are a density signal on
Wayland too — so both leave it at 1.0 there unless `FIRN_UI_SCALE` or
Preferences > UI Scale asks for more. Verified: `ui_scale`/`font_size`
read back as 1/13 again, a screenshot shows the full tool list and
normal-proportioned panels instead of a cropped quarter of the UI, and all
106 app_tests.py + 19 smoke.py checks plus ctest still pass. This likely
means Wayland had the same "too big" bug even though nobody had reported
it there.

**A native macOS menu bar**, requested the same day once the app was
usable enough to look wrong for not having one. `App::draw_menu` and its
Layers/Selections helpers now take a `MenuBuilder&` (see CLAUDE.md, "the
menu bar is one shared body, two renderers") instead of calling `ImGui::`
directly, so the ~200-item tree (mostly the Effects/Adjust long tail) has
exactly one source of truth for both the in-window ImGui bar (every
platform) and a real `NSMenu` tree (`NativeMenu_mac.mm`, reconciled by
position every frame) that macOS now shows instead. Two real bugs surfaced
building this on hardware, both fixed: `NativeMenuBuilder::begin_menu` has
to return `false` (and not be entered) for a disabled menu, exactly like
`ImGui::BeginMenu` — the menu bodies rely on that to dereference
`doc`/`layer` unconditionally once past their own enabled check, and
returning `true` unconditionally crashed immediately on Image > Color
Management with no document open; and the per-frame Objective-C objects
need an explicit `@autoreleasepool` since SDL's main loop is a plain
`while()`, not `[NSApp run]` — nothing else was draining one, so heap RSS
grew unbounded over a few hundred frames until that was added (`leaks`
confirmed it was never a true leak — 0 attributable bytes throughout —
just an ever-growing pool). Verified on hardware with the real
`NativeMenuBuilder` path (106 + 19 + ctest, RSS flat over 500+ frames) and,
forcing the `#else` branch locally since there is only one Mac here, with
`ImGuiMenuBuilder` too (untestable any other way here; CI covers it for
real on Linux/Windows).

Still not verified on hardware, no tablet or Developer Mode available to
check it:

- [ ] The tablet backend `app/src/Tablet_mac.mm`

Not attempted, and a real project of its own if picked up: there is no
`.app` bundle on macOS at all (`app/src/main.cpp` builds a plain
executable; `CMakeLists.txt` has no `MACOSX_BUNDLE`, no `Info.plist`, no
`.icns`, and `CPack` has no macOS generator configured). That means no dock
icon unless launched from a terminal, no double-click-a-file association,
and no notarization story. Worth scoping separately; it needs an icon set
in addition to the CMake/CPack work.

## Dropped (not worth the effort for this port)

- Art Media layers and tools (oil brush, chalk, pastel, palette knife,
  smear): a separate paint simulation and layer type with no format sample
- Plugin filters (.8bf): Windows binaries
- Digimarc watermarking, TWAIN scanning, screen capture, batch processing,
  web tools (image slicer, image mapper, optimizer wizards)
- Browser palette, Print Layout (the Effect Browser landed in 0.2)
- Vector leftovers: Fit Text to Path, the original's text shape layout
