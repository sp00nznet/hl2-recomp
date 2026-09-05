"""
Read an Xbox 'xCmp' container -- the .xz_ files on the Half-Life 2 disc.

The game reads .xzp; the disc ships .xz_, and default.xbe converts one to the
other while copying (LoaderMedia/install.txt is the manifest). The container is
Microsoft XCompress. The framing below is reversed from the file and verified
against zip0_xbox.xz_: 26,530 blocks producing exactly the 434,653,523 bytes
the header claims.

  Header, 24 bytes -- the length default.xbe's own check reads (sub_00011F10
  reads 0x18 and compares the magic and version):

      0   'xCmp'   0x78436D70 little-endian
      4   version  1
      8   uncompressed size
     12   window   0x00080000 (512 KB)
     16   field4   0x00390080  (unidentified)
     20   block    0x00004000 (16 KB)

  Records:  { uint16 length; payload[...] }, and the length is a
            discriminator as much as a size:

      0x0000  padding. The rest of this 512 KB chunk is zero; the next record
              begins at the next multiple of the window size.
      0xC000  a stored block. The payload is exactly `block` raw bytes, so the
              record is 2 + 16384. Not a length -- reading it as one walks off
              by 4 bytes per block and the framing never recovers.
      else    a compressed block of `length` bytes, opening with a six-byte
              sub-header { uint16 0x434A, uint32 output size }. The payload
              after it is LZX.

  Chunks:   the window size, 512 KB, zero-padded at the end. That is what
            makes each chunk independently decodable, and every resume point
            in the file is exactly 0x80000-aligned.

zip0_xbox.xz_ is 25,894 compressed blocks and 636 stored ones.

Decoding the LZX payload is the remaining piece; walk() hands out blocks so a
decoder can be dropped in behind it.
"""
import struct
import sys

XCMP_MAGIC = 0x78436D70
HEADER_SIZE = 24
PAD_MARK = 0x0000
STORED_MARK = 0xC000
BLOCK_MARK = 0x434A          # 'JC', opening a compressed block's sub-header


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


def walk(data):
    """Yield (stored, output_size, payload) for every block, in order.

    `stored` says whether the payload is already the output. Raises if the
    blocks do not account for exactly the header's uncompressed size, which is
    the check that makes the framing trustworthy rather than plausible.
    """
    hdr = Header(data)
    off = HEADER_SIZE
    produced = 0
    n = len(data)

    while off + 2 <= n and produced < hdr.uncompressed_size:
        (length,) = struct.unpack_from("<H", data, off)

        if length == PAD_MARK:
            nxt = ((off // hdr.window_size) + 1) * hdr.window_size
            if nxt <= off or nxt >= n:
                break
            off = nxt
            continue

        if length == STORED_MARK:
            body = off + 2
            yield True, hdr.block_size, data[body:body + hdr.block_size]
            produced += hdr.block_size
            off = body + hdr.block_size
            continue

        if length < 6:
            raise XCmpError("record at 0x%X is too short to hold a sub-header"
                            % off)
        mark, out_len = struct.unpack_from("<HI", data, off + 2)
        if mark != BLOCK_MARK:
            raise XCmpError("record at 0x%X has sub-header mark 0x%04X"
                            % (off, mark))
        body = off + 2 + 6
        yield False, out_len, data[body:off + 2 + length]
        produced += out_len
        off += 2 + length

    if produced != hdr.uncompressed_size:
        raise XCmpError("blocks total %d bytes, header says %d"
                        % (produced, hdr.uncompressed_size))


def _self_check():
    """Build a container by the spec above and walk it back.

    Covers what actually went wrong while reversing this: a stored record
    carries 0xC000, and reading that as a length rather than a flag walks 4
    bytes off per block and never recovers.
    """
    window, block = 0x80000, 0x4000

    payloads = [
        (False, b"JC" + struct.pack("<I", block) + b"compressed"),
        (True, bytes(block)),
        (True, bytes(block)),
        (False, b"JC" + struct.pack("<I", 999) + b"short block"),
    ]
    body = b""
    expect = []
    for stored, payload in payloads:
        if stored:
            body += struct.pack("<H", 0x8000 | block) + payload
            expect.append((True, block))
        else:
            body += struct.pack("<H", len(payload)) + payload
            expect.append((False, struct.unpack_from("<I", payload, 2)[0]))

    total = sum(size for _s, size in expect)
    data = (struct.pack("<6I", XCMP_MAGIC, 1, total, window, 0, block)
            + body)
    data += bytes(window - len(data) % window)      # pad out the chunk

    got = [(stored, size) for stored, size, _p in walk(data)]
    assert got == expect, (got, expect)

    # The 0xC000 records must advance by 2 + block, not by 0xC000. Mis-reading
    # them lands mid-record, so the walk stops accounting for the full size and
    # walk() raises rather than returning something plausible.
    bad = bytearray(data)
    struct.pack_into("<I", bad, 8, total + 1)
    try:
        list(walk(bytes(bad)))
    except XCmpError:
        pass
    else:
        raise AssertionError("a wrong total should not validate")

    # A container cut off inside its records is an error rather than a short
    # read. Cutting into the trailing padding is not: every block is still
    # there, which is exactly why chunks are padded.
    try:
        list(walk(data[:HEADER_SIZE + len(body) // 2]))
    except XCmpError:
        pass
    else:
        raise AssertionError("a truncated container should not validate")

    print("xcmp self-check: ok (%d blocks, %d bytes)" % (len(expect), total))
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--self-check":
        return _self_check()
    if len(argv) < 2:
        print(__doc__)
        return 2
    data = open(argv[1], "rb").read()
    hdr = Header(data)
    print("%s" % argv[1])
    print("  %s" % hdr)

    compressed = stored = 0
    out = 0
    for is_stored, out_len, _payload in walk(data):
        out += out_len
        if is_stored:
            stored += 1
        else:
            compressed += 1
    print("  %d blocks: %d compressed, %d stored"
          % (compressed + stored, compressed, stored))
    print("  %d bytes out, header agrees" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
