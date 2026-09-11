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
- [ ] Let a stroke choose which assistant it follows (a Tool Options
      picker), which is what two-point perspective needs
- [ ] Layer style presets, and styles that scale when the image is resized
- [ ] Lighter PNG compression for large .ora saves (about a second for a
      3 MP project today, most of it the layer and merged PNGs)
- [ ] Filter layer masks in the classic format (only .ora keeps them)

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

## Dropped (not worth the effort for this port)

- Art Media layers and tools (oil brush, chalk, pastel, palette knife,
  smear): a separate paint simulation and layer type with no format sample
- Plugin filters (.8bf): Windows binaries
- Digimarc watermarking, TWAIN scanning, screen capture, batch processing,
  web tools (image slicer, image mapper, optimizer wizards)
- Browser palette, Print Layout (the Effect Browser landed in 0.2)
- Vector leftovers: Fit Text to Path, the original's text shape layout
