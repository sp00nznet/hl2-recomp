"""
Read an Xbox 'xCmp' container -- the .xz_ files on the Half-Life 2 disc.

The game reads .xzp; the disc ships .xz_, and default.xbe converts one to the
other while copying (LoaderMedia/install.txt is the manifest). The container
is Microsoft XCompress. What is established so far, all verified against
zip0_xbox.xz_ (253 MB on disc, 434,653,523 bytes out):

  Header, 24 bytes -- the length default.xbe's own check reads (sub_00011F10
  reads 0x18 and compares the magic and version):

      0   'xCmp'   0x78436D70 little-endian
      4   version  1
      8   uncompressed size
     12   window   0x00080000 (512 KB)
     16   field4   0x00390080  (unidentified)
     20   block    0x00004000 (16 KB)

  Records:  { uint16 length; payload[length] }, next record at +2+length.
  Verified by walking: the chain holds for every record in a chunk.

  Chunks:   records pack until the next would cross a 512 KB boundary, then
            zero padding up to it. A length of 0 means padding, so skip to
            the next multiple of the window size. 483 chunks in zip0_xbox,
            and the resume point is always exactly 0x80000-aligned.

  Most payloads open with 'JC' (0x434A) followed by a uint32 output size of
  0x4000, which reads as a per-block sub-header. 12,241 of them do; 4,354 do
  not, and those include 47 records of exactly 0xC000 bytes. Summing only the
  'JC' ones lands 14,289 blocks short of the header's total, so the sizing of
  the remainder is not yet understood -- see docs/boot.md.

This module walks and validates the framing. It does not decode: the payload
is LZX, and that is the next piece.
"""
import struct
import sys

XCMP_MAGIC = 0x78436D70
HEADER_SIZE = 24


class XCmpError(Exception):
    pass


class Header:
    __slots__ = ("uncompressed_size", "window_size", "field4", "block_size")

    def __init__(self, data):
        if len(data) < HEADER_SIZE:
            raise XCmpError("short header")
        magic, version, usize, window, field4, block = struct.unpack_from(
            "<6I", data, 0)
        if magic != XCMP_MAGIC:
            raise XCmpError("not an xCmp container (magic %08X)" % magic)
        if version != 1:
            raise XCmpError("unsupported xCmp version %d" % version)
        self.uncompressed_size = usize
        self.window_size = window
        self.field4 = field4
        self.block_size = block

    def __str__(self):
        return ("xCmp v1: %d bytes uncompressed, %d KB window, %d byte blocks"
                % (self.uncompressed_size, self.window_size // 1024,
                   self.block_size))


def probe(data, limit=8):
    """Establish the framing on the first few blocks.

    Finds each record by looking for the next block-size field, which is sound
    only while no compressed byte happens to spell it. That is true early and
    false later, so this is a format-discovery tool: it answers "is the record
    {checksum, size} followed by payload" and stops.
    """
    hdr = Header(data)
    target = struct.pack("<I", hdr.block_size)
    out = []
    off = HEADER_SIZE
    for _ in range(limit):
        if off + 8 > len(data):
            break
        checksum, out_len = struct.unpack_from("<2I", data, off)
        if out_len != hdr.block_size:
            break
        nxt = data.find(target, off + 8)
        if nxt < 0:
            break
        nxt -= 4                      # the size field sits at record+4
        out.append((checksum, out_len, nxt - (off + 8)))
        off = nxt
    return hdr, out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    data = open(argv[1], "rb").read()
    hdr, blocks = probe(data)
    print("%s" % argv[1])
    print("  %s" % hdr)
    print("  expected blocks: %d"
          % ((hdr.uncompressed_size + hdr.block_size - 1) // hdr.block_size))
    for i, (checksum, out_len, in_len) in enumerate(blocks):
        print("  block %d: checksum %08X, %d compressed -> %d uncompressed"
              % (i + 1, checksum, in_len, out_len))
    if blocks:
        avg = sum(b[2] for b in blocks) / len(blocks)
        print("  mean %.0f compressed per %d uncompressed (%.2fx)"
              % (avg, hdr.block_size, hdr.block_size / avg))
    print()
    print("  Decoding the LZX payload is what turns this into an extractor;")
    print("  see docs/boot.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
