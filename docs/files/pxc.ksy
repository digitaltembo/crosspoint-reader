doc: >
  Pixel cache for a decoded EPUB image, written by PixelCache (lib/Epub/Epub/converters/PixelCache.h)
  next to the extracted image with its extension replaced by .pxc. Pixels are already scaled and
  dithered to the rendered size. A cache is reused only when width/height are within 1px of the
  expected render size and the file holds at least 4 + bytes_per_row * height bytes.

meta:
  id: pxc
  file-extension: pxc
  endian: le
  bit-endian: be

seq:
  - id: width
    type: u2
    doc: Image width in pixels
  - id: height
    type: u2
    doc: Image height in pixels
  - id: rows
    type: row
    repeat: expr
    repeat-expr: height
    doc: Rows in top-to-bottom order

instances:
  bytes_per_row:
    value: (width + 3) / 4
    doc: 2 bits per pixel, 4 pixels per byte; rows are padded to a whole byte

types:
  row:
    seq:
      - id: pixels
        type: b2
        enum: gray_level
        repeat: expr
        repeat-expr: _parent.width
        doc: Leftmost pixel in the most significant bits of each byte
      - id: padding
        type: b2
        repeat: expr
        repeat-expr: _parent.bytes_per_row * 4 - _parent.width
        doc: Zero padding to the end of the final byte

enums:
  gray_level:
    0: black
    1: dark_gray
    2: light_gray
    3: white
