# Changelog

All notable changes to Firn. The format follows Keep a Changelog; versions
follow Semantic Versioning. `scripts/release.sh` turns the Unreleased
section into the next version.

## Unreleased

### Fixed
- Native format: a fully transparent layer is written with a 1 x 1 tile;
  the previous empty-layer encoding hung the original on "Reading".

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
