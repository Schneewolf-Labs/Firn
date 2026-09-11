# The tools

Every tool in the Tools palette, what it does, and the files it can load.
Single-key shortcuts match the original program's where one exists.

## Shortcuts

| Key | Tool | Key | Tool |
| --- | --- | --- | --- |
| A | Pan | F | Flood Fill |
| Z | Zoom | T | Text |
| S | Selection | V | Line |
| L | Freehand Selection | I | Preset Shape |
| W | Magic Wand | O | Object Selector |
| E | Dropper | D | Pen |
| M | Move | K | Deform |
| R | Crop | C | Clone |
| B | Paint Brush | N | Lighten/Darken |
| P | Airbrush | U | Smudge |
| X | Eraser | Q | Color Replacer |

Dodge/Burn, Soften, Sharpen, Saturation and Hue brushes have no key.
`[` and `]` resize the brush. The left button paints with the foreground
material and the right button with the background. Space with a drag, or a
middle-drag, pans with any tool. Escape cancels a stroke.

## Painting

The brush tools take a custom tip from `.PspBrush` or PNG files, or from the
current selection, and a paper texture that modulates coverage. Brush opacity
applies once per stroke rather than per stamp, so overlapping stamps inside
one drag do not darken each other.

Pressure from a pen tablet changes the brush size, the opacity, or both; the
eraser end of the pen picks the Eraser tool. Pressure is read from XInput2 on
X11 (build with `libxi-dev` present), pointer messages on Windows and tablet
events on macOS. Wayland sessions do not report pressure yet, and Help >
About says which backend is active.

The Picture Tube tool stamps cells from `.PspTube` files with the original's
random, incremental and angular cell selection and random or continuous
placement.

## Retouching

Clone (right-click sets the source), Heal, Scratch Remover, Object Remover,
Smudge, Soften, Sharpen, Dodge/Burn, Lighten/Darken, Saturation, Hue,
Red-eye Removal and Color Replacer all work as brushes, so they respect the
selection, the pressure response and the mask edit mode.

Edit > Content-Aware Fill rebuilds whatever is selected from the rest of the
picture by exemplar synthesis, for removing an object outright.

## Selections

Shift adds to a selection, Ctrl subtracts, and a plain click deselects.
Ctrl+A selects all, Ctrl+D drops the selection, Ctrl+Shift+I inverts it and
Delete clears it. The Selections menu feathers, expands, contracts, smooths,
selects similar pixels and edits the selection as a grayscale mask.

Every pixel command and every painting tool is confined to the current
selection. Selections can be saved into the file and loaded back.

## Layers

Layers can be grouped (Layers > New Layer Group) and carry masks (Layers >
New Mask Layer); press Edit next to a mask to paint on it in grayscale with
any brush. Adjustment layers transform what is composited below them, filter
layers do the same for spatial effects, and layer styles add drop shadows,
glows, strokes and bevels that re-render as the layer changes.

Image > Increase Color Depth > 16 Bits per Channel keeps every raster layer
at 16 bits beside its 8-bit display pixels. Levels, curves,
brightness/contrast, gamma, HSL, colorize, color balance, channel mixer,
threshold, posterize, invert, grayscale, Gaussian blur, fills and all
geometry run at full precision. Other operations and the painting tools work
at 8 bits and reduce the layer, undoably, with a note in the status bar.

## Vector objects

Layers > New Vector Layer, or tick "Create as vector" on the Preset Shape,
Line and Text tools, and the shapes stay editable. The Object Selector moves,
scales and rotates them; double-click for the Vector Properties dialog
(stroke, fill, width, line style, gradient, pattern, and the text of a text
object). The Pen draws point-to-point or freehand paths and edits nodes. The
Objects menu aligns, distributes, sizes, arranges, groups and converts text
to curves. Layers > Convert to Raster Layer flattens one.

## Materials

The Materials palette switches the foreground and background between a solid
color, a gradient and a pattern, optionally modulated by a texture. The flood
fill, shape, line and text tools all paint through whichever is set.

## Resizing

Image > Resize offers nearest neighbor, bilinear, bicubic, Mitchell-Netravali,
Lanczos3 and an edge-directed method that keeps diagonal edges clean when
enlarging past a doubling. Smart picks among them from the scale factor.

## Metadata

Image > Image Information has a Metadata tab listing every Exif tag and text
note the picture carries. Click a value to edit it. Remove All clears
everything, and Remove Private drops what identifies the photographer and the
place: GPS coordinates, camera and lens serial numbers, the owner's name and
the manufacturer's private note.

Metadata is read from JPEG and PNG files, kept through every edit, and written
back when the image is saved as JPEG, PNG or an OpenRaster project. Entries
Firn did not change keep their exact original bytes. The embedded thumbnail a
camera writes is not carried over, since it would no longer match the picture.

## Color management

Embedded ICC profiles (PNG iCCP, JPEG APP2) are read, kept and written back.
Image > Color Management assigns or converts to sRGB, Adobe RGB, ProPhoto RGB
or a profile file, and the color-managed display shows tagged images converted
to sRGB. RGB matrix/TRC profiles are handled; CMYK profiles are recognized but
not converted.

## Printing

File > Print (Ctrl+P) lays the image out on Letter, A4 or Legal paper
(orientation, margins, fit to page or a scale at a chosen DPI), writes a
one-page PDF and sends it to the default or a named printer through the
system spooler. "Save as PDF" keeps the file instead.

## Themes

File > Preferences picks the theme (Firn, Dark, Light, Classic, Slate, or
your own). Edit Themes opens the editor for every color, rounding and
padding, the font and the text size. Themes are saved as
`~/.config/firn/themes/*.firntheme` and can be exported and imported as
single files.

## Where Firn looks for content

Each library is scanned from the matching folder under `~/.config/firn`, from
the folders set in Preferences, and from the directories named by an
environment variable.

| Content | Folder | Variable |
| --- | --- | --- |
| Picture tubes (`.PspTube`) | `tubes` | `FIRN_TUBE_DIRS` |
| Brush tips (`.PspBrush`, PNG) | `brushes` | `FIRN_BRUSH_DIRS` |
| Paper textures | `textures` | `FIRN_TEXTURE_DIRS` |
| Preset shapes (`.PspShape`) | `shapes` | `FIRN_SHAPE_DIRS` |
| Gradients (`.PspGradient`) | `gradients` | `FIRN_GRADIENT_DIRS` |
| Styled lines (`.PspStyledLine`) | `lines` | `FIRN_LINE_DIRS` |
| Patterns (images) | `patterns` | `FIRN_PATTERN_DIRS` |
| Fonts | the usual system font folders | `FIRN_FONT_DIRS` |

Settings live in `~/.config/firn/firn.cfg` and the window layout in
`layout.ini` beside it.

## Other environment variables

| Variable | Effect |
| --- | --- |
| `FIRN_WINDOW=1280x800` | initial window size |
| `FIRN_UI_SCALE=1.5` | force the interface scale instead of following the display |
| `FIRN_DRIVE=/path/to.sock` | listen on a socket for automation (see [API.md](API.md)) |
