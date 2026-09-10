# Changelog

All notable changes to Firn. The format follows Keep a Changelog; versions
follow Semantic Versioning. `scripts/release.sh` turns the Unreleased
section into the next version.

## Unreleased

### Added
- Apache License 2.0.
- Preferences: undo memory budget per image (default 1 GB).
- Text objects are saved as the native format's text shapes, rotation
  included: they reopen as editable text here and in the original.
- Effects > Effect Browser: every adjustment and effect previewed on the
  active layer with its current settings; click a tile to open its dialog.
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
