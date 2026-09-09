# Native container format (.PspImage / .PspTube / .PspFrame / .PspSelection)

What `core/src/io_psp.cpp` reads, verified against the 153 sample files in
the original install (versions 4.0, 5.0, 6.0 and 7.0; LZ77, RLE and
uncompressed). All integers are little-endian. Rects are `left, top, right,
bottom` as int32, half-open.

## File

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | Signature `"Paint Shop Pro Image File\n\x1a"` zero-padded |
| 32 | 2 | Major version (4 = PSP 6, 5 = PSP 7, 6 = PSP 8/9, 7 = PSP X) |
| 34 | 2 | Minor version |
| 36 | … | Blocks until end of file |

## Block header (version >= 4)

`"~BK\0"`, block id (u16), payload length (u32), payload. Versions before 4
add an "initial chunk length" u32 before the total; not supported.

Most payloads start with a *chunk*: a u32 length (including itself) followed by
fixed fields. Skip to `payload + chunk_len` to reach whatever follows the
chunk (sub-blocks, or raw data). Chunk lengths vary between writers, so
always use the stored length rather than a constant.

| Id | Block | Notes |
|---|---|---|
| 0 | General image attributes | one chunk, see below |
| 1 | Creator | `~FL` fields: title, author, copyright, dates |
| 2 | Color palette | chunk `{len, entry_count}` then `entry_count` x `B,G,R,0` |
| 3 | Layer bank | contains layer blocks directly, **no chunk** |
| 4 | Layer | info chunk + bitmap chunk + channel sub-blocks |
| 5 | Channel | chunk `{len, compressed_len, uncompressed_len, dib_type u16, channel_type u16}` then data |
| 6 | Selection | not read yet |
| 7 / 8 | Alpha bank / alpha channel | bank chunk `{6, count u16}`; each channel block: chunk `{len, name (u16 len + bytes), rect, saved rect}`, bitmap chunk `{8, 1, 1}`, one channel of DIB type 4 over the saved rect (relative to the rect). These are the original's saved selections; read into `Document::alpha_channels()` and written back |
| 27 | Brush | chunk `{len=12, u32 1, u32 step}`; the image itself (8-bit gray) is the tip: dark = coverage, white = nothing |
| 11 | Picture tube | chunk `{len=30, u16 0, step u32, columns u32, rows u32, total cells u32, placement u32, selection u32}`; placement 1 random / 2 continuous, selection 1 random / 2 incremental / 3 angular / 4 pressure / 5 velocity; cells are the image divided into columns x rows |
| 9 | Composite image | bitmap chunk `{len, bitmap_count u16, channel_count u16}` + channel blocks |
| 16 | Composite bank | chunk `{len, count}` then attribute blocks, then one data block each |
| 17 | Composite attributes | chunk `{len, width, height, depth u16, compression u16, planes u16, colors u32, type u16}`; type 0 full size, 1 thumbnail |
| 18 | JPEG | chunk `{len, compressed_len, uncompressed_len, image_type u16}` then a JFIF stream |
| 13 / 14 / 15 / 19 | Vector extension / shape / paint style / line style | see **Vector layers** below |
| 12 | Adjustment extension | see **Adjustment layers** below |
| 10 | extended data | skipped |

## General image attributes (id 0), chunk length 46

width i32, height i32, resolution f64, metric u8 (0 undefined, 1 inch, 2 cm),
compression u16 (0 none, 1 RLE, 2 LZ77/zlib, 3 JPEG), bit depth u16 (8, 24
or 48 seen), plane count u16, color count u32, grayscale u8, total image
size u32, active layer i32, layer count u16, graphic contents flags u32.

## Layer (id 4)

Info chunk: name (u16 length + ISO-8859-1 bytes), type u8, image rect,
saved image rect, opacity u8, blend mode u8, visible u8, transparency
protected u8, link group u8, mask rect, saved mask rect, mask linked u8,
mask disabled u8, invert-mask-on-blend u8, blend range count u16, five
blend ranges (8 bytes each), then version-dependent extras. Skip by chunk
length.

Layer types: 1 raster, 2 floating selection, 3 vector, 4 adjustment,
5 group, 6 mask, 7 art media. Only raster layers carry a bitmap chunk and
channels directly after the info chunk; the others are followed by their
extension sub-block first.

- **Group (5)**: group extension block (id 25) `{chunk_len, child_count u32, u8}`
  followed by an *empty* bitmap chunk `{8, 0, 0}`. The original refuses to
  finish reading a file whose group blocks lack that chunk.
  The children are the next `child_count` layer blocks in the bank (nested
  groups count as one child each and bring their own children). A hidden
  group hides its children; the reader folds group opacity into each child
  and composites children individually (a group blend mode is reported).
- **Mask (6)**: mask extension block (id 26) `{chunk_len u32, outside u32,
  u8}`, then a bitmap chunk `{8, 1, 1}` and one channel block of DIB type 2
  (user mask), covering the *saved mask rect* relative to the *mask rect*
  from the layer info (0 = hidden, 255 = shown). The `outside` value (255 in
  every sample) is taken as the mask value beyond the saved rect; this is
  inferred, not documented. A mask inside a group becomes the group's mask;
  a top-level mask becomes the mask of the layer directly beneath it. The
  original represents "a layer with a mask" as a group holding that layer
  and its mask; the reader collapses such two-member groups back to a masked
  raster layer, and the writer expands them again.

Blend modes 0–16 match `firn::BlendMode` in order (Normal, Darken, Lighten,
Hue, Saturation, Color, Luminance, Multiply, Screen, Dissolve, Overlay,
Hard Light, Soft Light, Difference, Dodge, Burn, Exclusion); 17–20 are the
"true" hue/saturation/color/lightness variants, 255 is adjustment.

Then the bitmap chunk `{len u32, bitmap_count u16, channel_count u16}` and
`channel_count` channel blocks.

### Pixel placement

Channel data covers the **saved image rect**, whose coordinates are relative
to the image rect's origin. Canvas position of the data is therefore
`image_rect.left + saved.left, image_rect.top + saved.top`. Rows are not
padded. The `uncompressed_len` field is nominal (it assumes 4-byte padded
rows and, for color channels, all three planes) and must not be used to size
buffers; use `saved_width * saved_height * bytes_per_sample`.

A saved rect of zero size with `compressed_len` 0 is an empty layer.

### Channels

DIB type 0 (image) with channel type 1/2/3 = R/G/B planes for 24/48-bit;
channel type 0 = palette index or gray level for 8-bit. DIB type 1
(transparency mask), channel 0 = alpha. A layer with no transparency mask is
opaque; if it also fills the canvas and is the bottom layer it is the
Background. Thumbnails use DIB types 5/6 and composites 8/9 with the same
meaning. 48-bit samples are u16; the high byte is kept.

RLE: read a count byte `n`; if `n > 128` repeat the next byte `n - 128`
times, else copy `n` literal bytes.

## Adjustment layers (type 4)

Per the official format specification (see References): the layer info
chunk is followed by an adjustment extension block (id 12) holding an info
chunk `{6, type u16}` and one definition chunk, then a bitmap chunk
`{8, 1, 1}` and a channel block of DIB type 7 (an 8-bit mask that limits
where the adjustment applies; all 255 = everywhere). That bitmap takes the
user mask's place, so its extent goes in the info chunk's **mask rect and
saved mask rect** fields with the image rects zero; the original hangs on
a file that puts it in the image rects. The header's contents flags gain
0x4. Types: 1 Levels, 2 Curves, 3
Brightness/Contrast, 4 Color Balance, 5 HSL, 6 Channel Mixer, 7 Invert,
8 Threshold, 9 Posterize. Definition chunks (each starts with its u32
length):

- Levels: `f64 gamma x4, i32 input ceiling x4, i32 input floor x4, i32
  output ceiling x4, i32 output floor x4` (master, red, green, blue).
- Curves: four chunks (RGB, red, green, blue) of `{len, freehand u8, point
  count u16, 18 x (input u8, output u8), 256-byte table}`.
- Brightness/Contrast: `i32 brightness, i32 contrast`.
- Color Balance: `u8 preserve luminance, i32 highlight x3, midtone x3,
  shadow x3`.
- HSL: `u8 colorize, i32 master hue/saturation/lightness, i32 colorize
  hue/saturation/lightness, then red, yellow, green, cyan, blue, magenta
  ranges of 7 i32 each (hue, saturation, lightness, four range degrees)`.
- Channel Mixer: `u8 monochrome, i32 blue row (red, green, blue, constant),
  green row, red row`.
- Invert: nothing. Threshold: `i32`. Posterize: `i32`.

Firn applies an adjustment layer to everything composited below it within
its group, blended by opacity and its mask; alpha is untouched.

## Vector layers (type 3)

The layer info chunk is followed by a vector extension block (id 13) whose
chunk is `{8, shape count u32}`, then that many shape blocks (id 14), then
an empty bitmap chunk `{8, 0, 0}`. Shapes are stored bottom-first: the last
shape in the block is drawn on top (the palette lists them in reverse).

A shape block is a sequence of chunks and sub-blocks:

1. Info chunk `{len, name (u16 len + bytes), type u16, u32 5, shape id u32,
   u32 0}`. Type 2 is a path, type 5 a group. A group is followed only by a
   chunk `{8, member count}`; its members are the next shape blocks.
2. Attribute chunk, length 60 (payload 56): `u8 stroke on, u8 fill on,
   u8 antialias, f64 stroke width, {u8, u8, f64, f64} x2, u8, f64 miter
   limit`. The two records read as the first and last line caps `{type,
   ?, width, height}`: the default styled line (+Solid) has the same 7.21
   cap sizes the balloon sample stores here. The second byte of each and
   the lone byte are carried through unchanged. Defaults in the original's
   own preset shapes: `01 00 1.0 1.0`, `01 00 1.0 1.0`, `00`, miter 10.
   Dash patterns have no known slot in a shape and are not written.
3. Paint style block (id 15) for the stroke, then one for the fill. The
   first chunk is `{6, kind u16}`: 0 none, 1 solid, 2 gradient, 3 pattern.
   Solid: chunk `{12, r, g, b, 0, ffffffff}`. Gradient: chunk `{35, style
   u16, ffffffff, invert u8, center x u32 %, center y u32 %, angle f64,
   repeats u32, color stop count u16, opacity stop count u16}` followed by
   color stops `{12, r, g, b, 0, position u16 %, midpoint u16 %}` and
   opacity stops `{9, opacity u8 %, position u16 %, midpoint u16 %}`.
   Style 0 is linear; 1 rectangular, 2 sunburst, 3 radial are assumed from
   the dialog order. A disabled stroke or fill still has its style block
   (kind 0) and its attribute flag clear.
4. Line style block (id 19), chunk length 45: `u16 cap, f64 width, f64
   height, u16 cap, f64 width, f64 height, u8, 4 bytes` (the segment caps;
   note the u16 framing, unlike the styled-line files). Every sample has
   caps 0 and sizes 1.0 (or all zeros in preset shapes). The original hangs
   while reading a file whose sizes sit at the wrong offsets.
5. One chunk `{8, node count}` then 55-byte node chunks: `f64 x, y, in x,
   in y, out x, out y` (handles are absolute image coordinates) and three
   flag bytes: byte 0 bit 0 starts a subpath (a shape with several
   contours, such as text, keeps them all in this one list; the original
   reads only the first list, so never write one per path), byte 1 bit 7
   closes the subpath at that node, bits 0x40/0x43/0xc0 in byte 1 encode
   the node type (corner, smooth, symmetric; only 0x40 is relied on).

Linear gradient parameter, verified against the stored composite of
`Vector balloon.PspImage` (mean error 0.8/255): with `(dx, dy)` the pixel
offset from the object bounds' center and `a` the angle in degrees,

    t = (dx sin a - dy cos a) / (|w sin a| + |h cos a|) + 0.5

so angle 0 runs bottom-to-top, 90 left-to-right, 180 top-to-bottom.
Colors interpolate between stops with the midpoint skewing the blend; the
opacity stops interpolate the same way. `repeats` folds `t` that many extra
times; `invert` flips it.

Library files reuse this format: `.PspShape` / `.pspshape` (preset shapes)
are complete images with one vector layer; `.PspGradient` files are
Photoshop `.grd` version 3 (big-endian `8BGR`, u16 version, u16 count;
each gradient: Pascal name padded to an even 1+len bytes, u16 color stop
count, 20-byte color stops `{location/4096 u32, midpoint u32, model u16,
4 x u16 components (>> 8), u16}`, u16 opacity stop count, 10-byte opacity
stops `{location, midpoint, opacity u16}`).

`.PspStyledLine` is a raw little-endian struct, decoded from all 25
shipped files: optional magic `01 51 45 57`, first cap u32, last cap u32,
first cap width and height f64, last cap width and height f64, miter
limit f64, segment count u32 and that many u32 segment lengths
(alternating dash and gap, in multiples of the stroke width), two u32
flags, segment start cap `{type u32, width f64, height f64}`, segment end
cap likewise, and a u32 that is 1 whenever the segment caps are set. Cap
types seen: 0 none, 1 round, 2 square, 3 narrow arrow, 4 wide arrow, 7
fleur-de-lis, 12 ball. The shape's line style block (id 19) holds the
segment-cap part of the same struct. `+Solid` (the default) has no magic
and cap sizes of 7.21, the same numbers the balloon sample carries in its
attribute records.

## Composite bank (id 16)

Attribute blocks (id 17) are listed first, then one data block per attribute
in the same order: a JPEG block for JPEG-compressed entries, otherwise a
composite image block (id 9). Files whose layers are all adjustment layers
still carry a full-size (type 0) composite, which the reader uses as a
fallback Background layer; vector layers are rendered from their shapes.

## Fidelity check

`tests/test_psp_corpus.cpp` compares our `Document::composite()` with the
full-size composite stored in each sample file (only when that composite
is channel data; JPEG composites are lossy). The mean channel error over a
white background is required to stay under 2/255. The multi-layer sample
with Overlay layers comes out at about 0.3.

## What the writer emits

`io::save_psp` writes version 6.0: image attributes (LZ77, 24-bit), a
creator block (dates, application id 1, version 8.0.0.1), a composite bank
with a JPEG thumbnail and a full-size zlib composite, then the layer bank.
Every non-Background layer gets a transparency channel even when opaque,
because the original does the same and the reader uses "no transparency
channel" to recognize the Background. Groups are written as group blocks
(child count in the extension) followed by their members; a group's mask is
a mask layer block after the members; a masked raster layer is wrapped in a
group with its mask, as the original does. Layer info chunks end with the 43-byte
tail found in every sample. The original (under Wine) opens the result with
all layers intact; `scripts/original-open.sh` automates that check.

## Not read

The current selection block (id 6), adjustment layer contents, vector
text shapes (no sample carries one), color profiles, and the 16-bit path
beyond truncation to 8 bits.

## References

Corel published the format specification for versions 7 and 8 (the same
container this app reads; version 8 adds art media and text shapes):
`ftp.corel.com/pub/documentation/PSP/` mirrored at
`http://ftpmirror.your.org/pub/misc/ftp.corel.com/pub/documentation/PSP/`,
alongside the scripting command API reference. Everything above was
first derived from the sample files and later checked against the spec.
