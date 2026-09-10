# Changelog

All notable changes to Firn. The format follows Keep a Changelog; versions
follow Semantic Versioning. `scripts/release.sh` turns the Unreleased
section into the next version.

## Unreleased

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
