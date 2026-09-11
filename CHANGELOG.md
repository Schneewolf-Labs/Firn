# Changelog

All notable changes to Firn. The format follows Keep a Changelog; versions
follow Semantic Versioning. `scripts/release.sh` turns the Unreleased
section into the next version.

## Unreleased

### Added
- A drawing API, so a script or a model can put marks on the canvas rather
  than only open, adjust and save. `draw.rectangle`, `draw.ellipse`,
  `draw.polygon` and `draw.path` place shapes as editable vector objects or
  rasterize them, honoring the selection, and `draw.stroke` paints one
  continuous brush stroke. `app.batch` applies a list of actions atomically:
  they land as one undo step, and if any of them is refused the document is
  left as it was. `samples/api-cat.json` is a worked example that draws a
  complete editable picture in one call, and `scripts/cli_tests.py` covers
  the API's transport and rollback behavior.
- Fixed retained native menu callbacks reading expired stack variables. ImGui
  menu actions now run after menu construction, so closing documents or merging
  layers cannot invalidate predicates still being rendered.
- Saved/autosaved history states have stable identities: undo-and-edit and
  history trimming no longer hide unsaved work. Reducing the undo limit preserves
  the dependencies of the remaining redo commands.
- Selection edits are finalized before switching, saving, closing or undoing;
  layer-mask state cannot leak into another document. Leaving an untouched
  selection edit preserves redo.
- Headless app-state regressions run under CTest alongside the core tests.
- Windows builds run straight from the build tree without vcpkg: point
  `SDL2_DIR` at SDL's own VC package and the build copies `SDL2.dll` beside
  `firn.exe` (docs/BUILDING.md, "Windows").
- The driver, `firn-cli` and the app test suites work on Windows. There
  `FIRN_DRIVE` names an address file for a loopback port, and clients must
  present the random token it holds before the program listens to them.
- Fixed a crash when flattening from a layer context menu. Menu and palette
  rendering no longer read the old layer stack after merges or deletion.
- Searchable tools with common tools first and collapsible specialist categories.
  Tool labels wrap, and the default Firn theme uses a 15 px system sans font.
- A start screen with New Image, Open Image, recent files and access to deferred
  recovery copies. Failed recoveries remain available to retry.
- Canvas context identifying the active layer, pixels/vector/mask target and
  selection, with Deselect and Finish Mask Editing controls.
- Clearer Materials labels, foreground hex and opacity controls, and a color
  wheel that fits the available panel space. Default panel widths follow UI scale.
- Tool Options fits its height when its contents change; right-click the pane
  background to disable this. Custom side docks and floating panes retain their
  sizes, and long option rows scroll horizontally.
- The project format is now lossless. An audit set every field of the
  document model to a non-default value and round-tripped it: twenty came
  back wrong. The active layer, the live selection, a group's expanded
  state, dash arrays, pattern and texture images, per-object visibility,
  styled line names, fractional point sizes, and every Adjustment field
  outside the active kind are all carried now, and `test_openraster_lossless`,
  `test_openraster_vectors` and `test_psp_vector_compat` keep them that way.
- Vector layers in a project use Firn's own object encoding
  (`core/src/io_vec.cpp`) rather than the original's shape layout, which had
  nowhere to put most of the above. Projects written before this carry the
  old blob and still open; new ones need this version or later. `.PspImage`
  is unchanged and still opens in the original, vector shapes and all.
- Metadata: Exif tags and text notes are read from JPEG and PNG files, kept
  through every edit, and written back on save, including into OpenRaster
  projects. Image > Image Information has a Metadata tab that lists and edits
  them, Remove Private drops GPS and serial numbers in one click, and the
  actions `image.metadata`, `image.set_metadata` and `image.strip_metadata`
  do the same from a script. Entries nothing touched keep their exact bytes.
- `file.save_as` takes a `quality`, so a script can write a JPEG without the
  dialog that the menu item raises.
- A photograph to try things on: `samples/luca.jpg`.
- The API documents itself: every action publishes JSON Schema for its
  parameters, including which are required, what values they accept and
  what is used when they are left out. `docs/API-reference.md` is the
  manual and `docs/api.json` the machine-readable form, both generated from
  the running program by `scripts/gen_api_docs.py`, with CI failing if they
  fall behind.
- `describe` publishes the inherited command names beside the actions, so
  one call reports everything that can be invoked. The list is generated
  from the source at build time and cannot fall behind.
- Everything the menus and tool options do is now an action with a name and
  typed parameters, registered beside the menu item it shares an App method
  with, so the program can be driven from outside without a mouse.
  `firn-cli` is the command line client: `firn-cli describe` reports the
  whole API as JSON, `firn-cli do image.resize '{"width":1600}'` runs one,
  `firn-cli state` says what is open, down to the layer list. The driver
  socket and .PspScript files reach the same surface. See docs/API.md.
- Image > Resize offers Lanczos and Mitchell alongside the existing
  filters, and a Smart size option that chooses for you the way the
  original's default does: Lanczos when reducing or enlarging a little
  (33.6 dB against bicubic's 32.1 on a photo doubled), edge directed past
  a doubling. Smart size is the new default and what the ResampleType
  script parameter maps to.
- Image > Resize offers Edge directed resampling, which interpolates along
  an edge instead of averaging across it. On hard-edged artwork enlarged
  several times it keeps curves and diagonals clean where bicubic
  staircases them; on a photograph it is close to bicubic and about ten
  times slower, so bicubic stays the default. Shrinking always uses the
  area average.
- Assistants: a stroke can follow a chosen assistant instead of the nearest
  one, which is what two-point perspective needs, since "nearest" means
  nothing once two vanishing points both cover the picture.
- Edit > Content-Aware Fill rebuilds the selection from the rest of the
  picture, so an unwanted object can be selected and removed. Exemplar
  synthesis over a resolution pyramid (the PatchMatch approach): patches
  inside the hole repeatedly look for the most similar patch of untouched
  image and the hole is rebuilt from those matches. No model or training
  data, and no new dependency. It reproduces a regular texture exactly; on
  a photograph it is convincing over texture and can leave a visible edge
  where it has to invent structure. A 360 x 300 hole in a 3 MP photo takes
  about two seconds, during which the window does not respond.

### Fixed
- Effects > Texture shows the chosen library texture. Its coverage (0 to 1)
  was written into the bump map as bytes, so nearly every pixel came out 0
  and the effect rendered flat. An MSVC conversion warning gave it away; the
  MSVC build is now warning-free too.
- Shift with a letter no longer changes tools as well as doing what the
  shortcut asks, so Shift+I opens Image Information and leaves the tool alone.
- `edit.content_aware_fill` refuses when nothing is selected instead of
  reporting success and doing nothing.

### Compatibility
- Projects (`.ora`) written by this version need this version or later to
  open, because vector layers moved to Firn's own object encoding. Projects
  written by 0.2.0 still open here. `.PspImage` is unchanged in both
  directions and still opens in the program Firn grew out of, vector shapes
  included.


## 0.2.0 (2026-09-11)

### Added
- Apache License 2.0.
- Preferences: undo memory budget per image (default 1 GB).
- Text objects are saved as the native format's text shapes, rotation
  included: they reopen as editable text here and in the original.
- Effects > Effect Browser: every adjustment and effect previewed on the
  active layer with its current settings; click a tile to open its dialog.
- HiDPI: the UI follows the display's scale factor (drawable ratio on
  macOS and Wayland, DPI on Windows, GDK_SCALE / QT_SCALE_FACTOR / Xft.dpi
  on X11), with a UI scale setting in Preferences and FIRN_UI_SCALE to force it.
- Heal Brush: a clone whose texture is blended into the target's colors
  (seamless clone per stamp), next to the Clone Brush.
- Brush smoothing in Tool Options: Basic (averaged points), Weighted
  (inertia) and Stabilizer (the brush trails the cursor on a string).
- OpenRaster (.ora) is Firn's project format: read and written with every
  layer kind, masks, vector objects, adjustment and filter layers, layer
  styles, 16-bit layers, color profiles, saved selections, ruler guides and
  painting assistants. Layers are stored cropped to their content, so a
  few brush strokes on a large canvas cost kilobytes. Readable
  by GIMP, Krita and MyPaint. Save As suggests .ora for layered images
  and autosave uses it; the classic format stays supported for files
  meant for the original.
- Adjust > Add/Remove Noise > Edge Preserving Smooth: averages within flat
  areas while leaving edges alone, the last of the original's photo filters
  we were missing.
- Scripting: 27 more of the original's commands run, taking it from 69 to
  96 of the 115 names its own bundled scripts use. Selections (smooth,
  save and load alpha channels, float), tools (select by name, previous
  tool), channel splitting, masks, paste into selection, palettes, grid
  and guide visibility, and the effects Glowing Edges, Colored Edges,
  Brush Strokes, Inner Bevel, Average, Salt and Pepper, JPEG Artifact
  Removal, Digital Camera Noise Removal, Curves, Hue Map and Histogram
  Adjustment.
- Material Properties: the Color tab gained red/green/blue and
  hue/saturation/lightness entry on the original's 0..255 scale and an HTML
  field; the Gradient tab lists the library as strips rather than names and
  previews the gradient as it will paint, with its style, angle, centre,
  repeats and invert applied.
- Layers palette: double-clicking a layer's name renames it in place
  (its other settings stay behind Properties in the right-click menu).
- File > Revert loads the saved file again and drops every change, asking
  first when there is anything to lose.
- View > Zoom to Selection fills the window with the selection.
- Edit menu: Copy Merged (Ctrl+Shift+C) copies the composite rather than
  the active layer; Paste Into Selection (Ctrl+Shift+L) scales the
  clipboard to the selection and paints it through its shape; Repeat
  (Ctrl+Shift+Y) re-applies the last adjustment or effect, with the
  settings it was given, to the active layer.
- Layers palette: drag a layer onto another to restack it, into and out of
  groups; a group moves with its members and cannot be dropped into itself.
- Layer Styles (Layers > Layer Styles, also in the layer's right-click
  menu): drop shadow, outer glow, inner glow, stroke and bevel rendered
  from the layer's shape at composite time and kept editable, on raster
  and vector layers and on groups (where the style follows the group's
  combined shape, not its members'); the palette tags styled layers [fx]. Kept in the native format through the Firn
  stash; the original shows the layer without them.
- Filter layers (Layers > New Filter Layer): Gaussian Blur, Average and
  Unsharp Mask applied live to everything below, with a mask and opacity
  like adjustment layers; edits underneath recomposite only the touched
  area plus the filter's reach. Saved in the native format as empty
  placeholder layers plus a stash the original ignores (docs/FORMAT.md).
- Painting assistants (Assistant tool in the View group): vanishing points,
  parallel rulers and rulers placed on the image; the brushes follow the
  nearest one while View > Snap to Assistants is on. Per image, like guides.
- Color Smudge brush (Paint group): paints the foreground color while
  dragging along what it passes over. Length sets how far the carried
  color goes, Color rate how much paint each stamp adds, and Dulling mode
  carries one averaged color instead of the patch.
- Deform tool as a unified transform: with a selection it transforms only
  the selected pixels (the selection follows, one history entry), rotation
  happens about a draggable pivot from anywhere outside the box, Alt scales
  from the center, and Tool Options gain numeric X / Y / W / H / Angle
  fields plus Flip H, Flip V, 90 CCW and 90 CW buttons.
- Foreground Select tool: scribble on the object (left button) and on the
  background (right button) and the selection is computed from color
  models of the marks regularized by geodesic distance; a selection made
  first serves as the rough outline. Solved at about 1.5 MP, so it is
  quick on large photos.
- Symmetry painting in Tool Options for every brush: Horizontal, Vertical,
  Both, Rotational (2 to 32 copies) and Kaleidoscope, about the image
  center or a point placed by clicking; the axes are drawn on the canvas.
- Adjust > Color to Alpha: a chosen color becomes transparency.
- Pen tablets: pressure drives the brush size and/or opacity (Tool
  Options), the eraser end switches to the Eraser tool and back, tilt is
  read where reported. XInput2 on X11, pointer messages on Windows, tablet
  events on macOS; Wayland sessions get no pressure yet.
- WebP read and write (lossless or lossy with a quality choice), bundled
  through libwebp at build time.
- Photoshop PSD/PSB import: layers with opacity, blend modes, visibility,
  masks and groups (8 and 16 bit RGB or grayscale; 16-bit files read at 8 bits).
- Autosave: modified images are written in the background every few
  minutes (Preferences, default 5) to the config folder; after a crash
  the next start offers to recover them.
- System clipboard: Copy puts the selection on the OS clipboard as an
  image, and Paste As New Image / Layer take images copied in other
  programs (Windows clipboard, xclip on X11, wl-clipboard on Wayland,
  osascript on macOS).
- Help > Keyboard Shortcuts; Ctrl+F / Ctrl+Shift+F float and defloat,
  Ctrl+Shift+M hides the marquee. Floating selections are tracked by a
  layer flag (shown as "(floating)") instead of their name.

### Changed
- Large images: Gaussian blur, resampling and native-file compression run
  across all cores (20 MP: blur 2.0 s to 0.55 s, native save 9.8 s to 3.3 s);
  undo entries for painting keep only the changed rectangle; big PNGs use
  a lighter compression level.

### Fixed
- macOS: the Text tool found no fonts, because the search knew the Linux and
  Windows directories but none of Apple's. Scripted keyboard shortcuts did
  nothing there either, since ImGui swaps Cmd and Ctrl on that platform and
  the driver was sending the one that becomes Super.
- A filter layer's mask is kept by the classic format as well, wrapped the
  way a masked raster layer is; only .ora had it before.
- One Step Photo Fix blocked up the shadows and drained color instead of
  improving the photo. Three causes: the automatic contrast stretch mapped
  everything below its clip point onto pure black (and its luma multiplier
  then took the pixel's color with it), Clarify scaled channels by a ratio
  that reached zero for a dark pixel in a bright neighborhood, and the
  pipeline ran gray-world cast removal and skin-tone damping that the
  original's own factory presets leave switched off. The contrast stretch
  now keeps a toe and shoulder, Clarify adds its local contrast rather than
  multiplying it and approaches the room left at each end, and the pipeline
  follows the original's documented defaults. On the bundled City photo,
  pure black went from 4.5% of the image to 0.01% and saturation now rises
  rather than falls.
- Automatic Color Balance gained the original's RemoveColorCast option (off
  by default) and Automatic Saturation Enhancement its Skintones option.
- Gradients picked from the library, or edited through Edit stops, painted
  as the plain foreground-to-background gradient instead of themselves.
- Native format: a fully transparent layer is written with a 1 x 1 tile;
  the previous empty-layer encoding hung the original on "Reading".

## 0.1.0 (2026-09-10)

### Added
- Raster editing: layers, groups, masks, adjustment layers, selections
  (shapes, freehand, point to point, smart edge, edge seeker, magic wand,
  color range, matting, the Modify submenu), the full Adjust and Effects
  menus with live preview, retouch and paint tools, picture tubes, text,
  vector shapes and lines with an object editor, deform and warp tools.
- Native file format read and write (layers, groups, masks, vectors,
  adjustment layers, saved selections, 48-bit), PNG including 16-bit,
  JPEG, BMP, TGA, GIF, PNM; ICC color management; printing to PDF.
- Scripting compatible with the original's `App.Do` command API, a Python
  runner for `.PspScript` files, and a socket driver for automation.
- 16 bits per channel editing.
- Material Properties: colors, gradients with a stop editor, patterns,
  textures, transparency; the Materials palette with Frame, Rainbow and
  Swatches pickers.
- Windowed or tabbed image views, tool icons, file dialog thumbnails,
  drag and drop, Save As type list, themes with an editor, Help > About.
