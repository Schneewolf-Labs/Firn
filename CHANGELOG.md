# Changelog

All notable changes to Firn. The format follows Keep a Changelog; versions
follow Semantic Versioning. `scripts/release.sh` turns the Unreleased
section into the next version.

## Unreleased

### Fixed
- The right-hand palette stays on whichever of Materials and Overview you
  left it on, across restarts and across opening an image. ImGui keeps the
  dock layout but not reliably this: with a saved workspace it gives the tab
  to whichever of the pair drew last, which is why it kept reverting to
  Overview however often you clicked Materials. Firn keeps the choice itself
  now and asks for it back on the way in.
- The workspace is written out when the program closes. ImGui only flushes
  its layout every few seconds on its own, so anything rearranged shortly
  before quitting was lost.

## 0.7.0 (2026-09-16)

### Fixed
- Opening the first image no longer drags the workspace off Materials and
  onto Overview. A docked panel that puts nothing at all on the screen
  counts as appearing the first time it does, and takes its tab with it, so
  the Overview panel now says "No image" instead of being blank -- which is
  what the original does too. The panel you leave selected is the one you
  come back to, and Materials is what a new workspace starts on.
- The crop rectangle can be adjusted instead of only redrawn: its corners
  and edges resize it, dragging the middle moves it, and a double-click
  inside crops, which is how the original ends the gesture. Before this,
  pressing anywhere threw the rectangle away and started another.
- Clicking text that is already there with the Text tool re-opens it for
  editing, rather than starting a second block on top of the first. Vector
  text only; once it is painted it is pixels like anything else.
- The pointer says what it is about to do. Resize arrows on the crop and
  vector handles, a move arrow inside them, the rotate knob and panning as a
  hand, a caret for the Text tool. Nothing in the program set a cursor
  before, so every handle looked like every other part of the canvas.
- The right button does its own job with the selection tools instead of
  repeating the left one. It ends a point to point selection where it
  stands, which is easier than hunting for the first vertex, and otherwise
  clears the selection -- but only when the click falls outside it, so
  working inside a selection cannot throw it away by accident. A right drag
  no longer draws a selection at all. Foreground Select keeps the right
  button for marking background, which is what it is for there.

### Added
- Edit > Generative Fill hands the selection to an image model and
  composites what comes back, as one undoable step. Firn is a client here,
  not a tool with opinions: the far side is any stable-diffusion.cpp server,
  on this machine or elsewhere, and the parameters are passed through
  untouched. Off unless you set an address in Preferences, which has a Test
  button that says what answered. There is no new dependency -- it shells
  out to curl or PowerShell, the way the update check does.
  Also the `generate.fill` action, with an optional seed so a script can pin
  a result that the interface deliberately varies.
- Edit > Generative Edit, for instruction models: "remove the dog" against
  the whole layer rather than a selection. It is a separate command because
  the picture reaches the model a different way -- as the reference it was
  trained to edit, not as noise to work back from. Hand an instruction model
  an init image and it stops editing and starts inventing, which is a silent
  wrong answer rather than an error. Also the `generate.edit` action.

## 0.6.0 (2026-09-13)

### Added
- TIFF, read and written. It is what print and archival photography run on
  and Firn could not touch it at all. Reading covers what cameras, scanners
  and other editors actually emit: LZW, Deflate and PackBits as well as
  uncompressed, strips or tiles, either byte order, 1 to 16 bits a channel,
  grey, palette, RGB and RGBA. Writing is Deflate with the horizontal
  predictor, which is lossless, and **at 16 bits a channel when the document
  is** -- the first way Firn hands full precision to another program. CCITT
  fax and JPEG-in-TIFF say so plainly instead of failing vaguely.
- Firn writes Photoshop files. It could already read them faithfully, which
  meant anyone handed a PSD could edit it and had no way to hand it back.
  The layer stack goes out with names, opacity, blend modes, visibility,
  masks and groups, plus the flattened composite every reader falls back on,
  and the ICC profile when the document carries one. Verified by round
  trip: a PSD written by GIMP, opened in Firn, saved again and reopened in
  GIMP with its layers, opacity and blend modes intact.

## 0.5.1 (2026-09-13)

### Fixed
- A photo's XMP is no longer thrown away when you save. XMP is where a photo
  manager keeps the title, caption, keywords, copyright, creator and rating,
  and Firn dropped the whole packet while Exif came through untouched, which
  is what made the loss easy to miss: an ordinary iPhone photo carries about
  3 KB of it. The packet now survives JPEG, PNG, OpenRaster and the native
  project format byte for byte.

### Added
- XMP properties show up in Image > Image Information beside the Exif tags
  and text notes, and can be edited there or through `image.set_metadata`
  with `group: XMP`. A value with "; " in it is a list, which is how
  keywords are held. Firn does not pretend to understand RDF: a structured
  property such as a face-region list stays in the packet and out of the
  table rather than being shown as one unreadable line.
- Remove Private now reaches into the XMP as well. Stripping only the Exif
  side would have left the location and the owner's name in the file, since
  XMP says the same things in its own vocabulary.

## 0.5.0 (2026-09-12)

### Fixed
- Layer styles scale when the image is resized. Halving a picture used to
  leave a full-size drop shadow on it, twice as heavy against the artwork
  as the one that was drawn. Offsets follow each axis and the radii the
  average of the two; colours, opacities, the bevel's depth and its light
  angle are not lengths and do not change. A stroke never scales away to
  nothing.

### Added
- Saved JPEGs and PNGs carry an Exif thumbnail again, so a photo edited in
  Firn shows a preview in file browsers and on cameras. It is generated
  from the picture as saved rather than carried over from the file that was
  opened: a camera's own thumbnail survives a crop and goes on showing what
  was cut away, which is a known way for detail to leak out of a photo.
- File > Export > Picture Tube writes one. Firn has read tubes since it
  had a tube tool and could never make one; now an image divided into a
  grid of equal cells becomes a .psptube, with the placement and selection
  modes the tool honors. The dialog refuses a grid the image does not
  divide by, because uneven cells would make the tool stamp slivers of
  their neighbors. Also the `file.export_tube` action and the original's
  `ExportTube` script command, which is what its AutoTuber script calls.
- Vector node editing is finished. A path can be broken at a node, joined
  back to another open path, reversed, and closed or opened, from the Pen
  tool's Edit Nodes mode or Objects > Edit Node. Objects > Convert to Path
  turns text into an editable outline.
- Nine `object.*` actions read and edit a vector layer through the API:
  `object.list` reports every object with its paths and nodes, and
  `object.node_select` then `object.node_break` / `node_join` /
  `path_reverse` / `path_closed` / `add_path` edit them. Three more of the
  original's script commands run as a result -- `ConvertToPath`,
  `ReturnVectorObjectProperties` and `NodeEditAddPath`, which together are
  what its VectorMergeSelected script is made of.
- The native project format keeps a picture's metadata. It was the last
  thing Firn held that saving to that format dropped: open a photo, save it
  as a `.pspimage`, and the camera, the exposure and the place all survived
  to the reopen. Exif rides in the Firn stash as a base64 TIFF directory and
  the text notes as key/value pairs; the original ignores both, as it
  already does the rest of the stash.
- Help > Check for Updates asks GitHub whether a newer release exists and
  reports it in Help > About. It only ever tells you; it never downloads or
  installs anything. Preferences has an opt-in daily check, off by default
  because a check tells a server someone here is running Firn. Failures are
  silent, and it shells out to curl or PowerShell rather than adding an HTTP
  dependency.

## 0.4.0 (2026-09-12)

### Fixed
- A use-after-free reading the native format. The stored-composite reader
  kept a pointer to a block inside a temporary that the loop it came from
  had already destroyed, so it read freed memory on every file that has a
  composite bank. AddressSanitizer found it; the format corpus had been
  passing over it for weeks.
- A corrupted file could exhaust memory. The channel reader took its image
  size straight from the file without a bound, so one flipped byte in a
  20 KB file had it asking for 2.3 GB. Sizes are now checked the way the
  full reader already checked them.
- Firn asks for eight bits a colour channel now. It never did: SDL's
  defaults are three, three and two bits, which most drivers quietly exceed
  and a software renderer does not. On such a setup every colour was
  quantised to four bits, so 32 came back as 34 and 128 as 136, and the
  canvas showed visible banding.
- A data race when saving a project: the PNG encoder takes its compression
  level from one global, and the writer sets it per image while encoding a
  document's layers on several threads at once. The level is now chosen once.
  Found with ThreadSanitizer, which the core tests now run clean under.

### Changed
- The 16-bit operations run across cores as well. On a 24 megapixel layer
  a Gaussian blur went from 5.9 seconds to 1.6, and the colour adjustments
  from around 300 to 600 milliseconds down to a few tens. Same results, to
  the byte.
- The Effects menu and the per-pixel adjustments run across cores now. None
  of them did: three shared pixel passes drive most of the effects, and all
  three walked the image on one thread. On a 24 megapixel layer the median
  filter went from 6.7 seconds to 0.8, motion blur from 2.5 to 0.3, and
  twirl, ripple, spherize, emboss, sharpen and erode all landed between five
  and eight times faster. Every one of them produces the same bytes as
  before, checked effect by effect.
- Windows renders with per-monitor DPI awareness instead of OS bitmap
  stretching, follows DPI changes while running, and rebuilds fonts at the
  requested size. Theme changes no longer compound widget spacing. Startup
  uses Segoe UI without scanning every font, and full canvas refreshes reuse
  the GPU texture when the image dimensions have not changed.
- Windows builds have a PowerShell build/test/package command that finds a
  compatible CMake even when PATH points to an older version. ZIP packages
  include the MSVC runtime; command-line-only installs include both tools,
  and runtime DLL copying also handles static builds.
- Box blur is between 6 and 84 times faster, and its cost no longer grows
  with the radius: it keeps a running sum instead of re-adding the whole
  window at every pixel, and spreads across cores like the other spatial
  operations. The output is identical to the byte, so Average filter layers
  look exactly as they did. At 1.9 megapixels a radius of 24 went from 317
  milliseconds to 4.
- Closing the program while a file is being read or written now waits for
  that to finish rather than racing it, and File > Exit says so instead of
  stacking prompts over the progress dialog.
- Opening a file no longer freezes the window. A 24 megapixel project took
  over two seconds to read; it now happens on a worker. The command line,
  scripts and the driver still open synchronously.
- Saving from File > Save or Save As no longer freezes the window either.
  A 24 megapixel project with a few layers took about three seconds on the
  interface thread; the writing now happens on a worker while the program
  keeps drawing. It is deliberately not cancellable. Scripts, the driver
  and the close-without-saving prompt still save synchronously, because
  they act on the result straight away, and the action layer refuses to run
  anything while a worker is reading the document.
- Content-Aware Fill no longer freezes the window. It runs on a worker
  thread behind a progress dialog with a Cancel, so the program keeps
  drawing and responding while it works, and cancelling leaves the picture
  exactly as it was. Scripts are unaffected: `edit.content_aware_fill`
  still finishes before it returns, because a script expects the work done
  when the call comes back.

### Added
- Lock transparency, in Layer Properties. The clear parts of a layer stay
  clear, so painting and fills only touch pixels that are already there and
  a stroke stays inside the shape the layer covers. This is the original's
  "transparency protected" and Firn now writes and reads that format's own
  field for it, so it round trips rather than riding in an extension.
- Layers > New Adjustment Layer > Gradient Map recolors everything below it
  by lightness, the darkest tones taking the start of a gradient and the
  brightest the end. Any gradient from the library works and Reverse flips
  it. Firn-only adjustments like this one are written to the native
  container as placeholder layers plus the stash, which now carries the
  whole adjustment so a gradient survives.
- Pass-through groups. A group normally composites its members among
  themselves, so an adjustment or filter layer inside it changes only its
  siblings. Turning on Pass through in the group's properties lets them act
  on the whole image below the group, which is how one group holds a stack
  of corrections that apply to everything. The group's opacity and mask
  still control how much of the change lands; its blend mode and layer
  style do not apply, having no shape of their own to act on.
  `layer.properties` takes `pass_through`, and both formats keep it.
- Blend ranges (Layer Properties > Blend Ranges). A layer can be limited to
  a range of tones instead of a shape: its own tones, or the tones of what
  is under it. Four stops per range give a hard or a soft edge, and the
  stops can read lightness or one channel. Dropping a warm wash on a
  photograph and showing it only in the highlights needs no mask at all.
  `layer.blend_ranges` does the same from a script. Both formats keep them.
  The original has a slot for blend ranges that no sample file uses, so
  Firn keeps its own in the stash rather than guess that layout
  (docs/FORMAT.md).
- Clipping masks. A layer can be clipped to the one below it (Layers >
  Create Clipping Mask, Ctrl+Alt+G) and then shows only where that layer
  does, so a texture, gradient or tonal correction stays inside one shape
  without painting a mask for it. Several layers can clip to the same one,
  the unit blends with the bottom layer's opacity, blend mode and mask, and
  a clipped adjustment or filter layer affects only what it is clipped to.
  `layer.properties` takes `clipped`, so it is scriptable like everything
  else. Projects keep it; the native container keeps it in the Firn stash,
  where the original ignores it.

## 0.3.0 (2026-09-11)

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
