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
| 2 | Colour palette | chunk `{len, entry_count}` then `entry_count` x `B,G,R,0` |
| 3 | Layer bank | contains layer blocks directly, **no chunk** |
| 4 | Layer | info chunk + bitmap chunk + channel sub-blocks |
| 5 | Channel | chunk `{len, compressed_len, uncompressed_len, dib_type u16, channel_type u16}` then data |
| 6 | Selection | not read yet |
| 9 | Composite image | bitmap chunk `{len, bitmap_count u16, channel_count u16}` + channel blocks |
| 16 | Composite bank | chunk `{len, count}` then attribute blocks, then one data block each |
| 17 | Composite attributes | chunk `{len, width, height, depth u16, compression u16, planes u16, colours u32, type u16}`; type 0 full size, 1 thumbnail |
| 18 | JPEG | chunk `{len, compressed_len, uncompressed_len, image_type u16}` then a JFIF stream |
| 10, 25, 26, 12, 13, 14 | extended data, group / mask / adjustment / vector extensions, shapes | skipped |

## General image attributes (id 0), chunk length 46

width i32, height i32, resolution f64, metric u8 (0 undefined, 1 inch, 2 cm),
compression u16 (0 none, 1 RLE, 2 LZ77/zlib, 3 JPEG), bit depth u16 (8, 24
or 48 seen), plane count u16, colour count u32, greyscale u8, total image
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

- **Group (5)**: group extension block (id 25) `{chunk_len, child_count u32}`.
  The children are the next `child_count` layer blocks in the bank (nested
  groups count as one child each and bring their own children). A hidden
  group hides its children; the reader folds group opacity into each child
  and composites children individually (a group blend mode is reported).
- **Mask (6)**: mask extension block (id 26) `{chunk_len u32, outside u32,
  u8}`, then a bitmap chunk `{8, 1, 1}` and one channel block of DIB type 2
  (user mask), covering the *saved mask rect* relative to the *mask rect*
  from the layer info (0 = hidden, 255 = shown). The `outside` value (255 in
  every sample) is taken as the mask value beyond the saved rect; this is
  inferred, not documented. A visible, enabled mask applies to the layers
  below it in its group (or to every layer below it at top level); the reader
  bakes it into those layers' alpha and reports it.

Blend modes 0–16 match `firn::BlendMode` in order (Normal, Darken, Lighten,
Hue, Saturation, Color, Luminance, Multiply, Screen, Dissolve, Overlay,
Hard Light, Soft Light, Difference, Dodge, Burn, Exclusion); 17–20 are the
"true" hue/saturation/colour/lightness variants, 255 is adjustment.

Then the bitmap chunk `{len u32, bitmap_count u16, channel_count u16}` and
`channel_count` channel blocks.

### Pixel placement

Channel data covers the **saved image rect**, whose coordinates are relative
to the image rect's origin. Canvas position of the data is therefore
`image_rect.left + saved.left, image_rect.top + saved.top`. Rows are not
padded. The `uncompressed_len` field is nominal (it assumes 4-byte padded
rows and, for colour channels, all three planes) and must not be used to size
buffers; use `saved_width * saved_height * bytes_per_sample`.

A saved rect of zero size with `compressed_len` 0 is an empty layer.

### Channels

DIB type 0 (image) with channel type 1/2/3 = R/G/B planes for 24/48-bit;
channel type 0 = palette index or grey level for 8-bit. DIB type 1
(transparency mask), channel 0 = alpha. A layer with no transparency mask is
opaque; if it also fills the canvas and is the bottom layer it is the
Background. Thumbnails use DIB types 5/6 and composites 8/9 with the same
meaning. 48-bit samples are u16; the high byte is kept.

RLE: read a count byte `n`; if `n > 128` repeat the next byte `n - 128`
times, else copy `n` literal bytes.

## Composite bank (id 16)

Attribute blocks (id 17) are listed first, then one data block per attribute
in the same order: a JPEG block for JPEG-compressed entries, otherwise a
composite image block (id 9). Files whose layers are all vector or adjustment
still carry a full-size (type 0) composite, which the reader uses as a
fallback Background layer.

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
channel" to recognise the Background. Layer info chunks end with the 43-byte
tail found in every sample. The original (under Wine) opens the result with
all layers intact; `scripts/original-open.sh` automates that check.

## Not read

Selections (id 6) and alpha channel banks (7/8), masks attached to groups or
layers (26), vector and adjustment layer contents, colour profiles, and the
16-bit path beyond truncation to 8 bits.
