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
- [ ] Gamma per channel, Color Balance (shadows/midtones/highlights), Hue Map

## 4. Effects

- [x] Add Noise, Sharpen / Sharpen More / Unsharp Mask
- [x] Median, Motion Blur, Blur More, Soften
- [x] Find Edges, Enhance Edges, Emboss, Erode / Dilate
- [x] Drop Shadow, Mosaic (pixelate)
- [ ] Inner/Outer Bevel, Cutout, Buttonize, Sepia, Wave, Pinch/Punch, Twirl

## 5. Retouch and paint tools

- [x] Move tool (layer offset), Clone brush
- [x] Airbrush, Lighten/Darken, Saturation, Hue, Color Replacer
- [ ] Smudge / Push, Dodge / Burn, Soften / Sharpen brushes
- [ ] Brush shapes and stamp textures

## 6. Text and shapes

- [x] Text tool (vendor stb_truetype; rasterised to a new layer)
- [x] Line and preset shapes drawn as raster
- [ ] Text stroke/outline, rotation, and editing after placement; more preset shapes

## 7. Workspace

- [x] Multiple open documents (tabs), recent files, remember window and dialog state
- [x] Rulers, grid
- [ ] Guides (drag from rulers), snap to grid
- [ ] Windows build verified (CI workflow added; see Actions)

## 8. Format completeness

- [ ] Masks attached to groups and layers
- [ ] Selections and alpha channels in files
- [ ] Layer groups in the palette
