"""
Verify a decompressed .xzp by checking it against itself.

An archive that came out of tools/xcmp.py plus the recompiled LZX decoder can
be wrong in a way that every cheap check misses. The first extraction of
zip0_xbox.xzp had the right magic, the right footer, the exact length its
header declares, and 19,842 plausible filenames -- and 165,468 of its bytes
were zeros where real bytes belonged, because forward "rep movs" was being
lowered to memcpy and every LZ run came out wrong. Length is preserved by that
bug, so nothing about the shape of the file gives it away.

What does give it away is that XZP stores many files twice. A file may have a
preload copy in the preload block as well as its ordinary copy, and when the
preload entry's length equals the file's length the two are byte-identical in
a good archive. That is a few hundred independent checks needing no reference
copy of anything.

    py -3 tools/xzp_verify.py saves/Cache/hl2/hl2x/zip0_xbox.xzp

Layout, all little-endian, offsets from the start of the file:

    0                       header, 36 bytes (see tools/xcmp.py)
    36                      directory entries   {crc, length, offset} x N
    36 + N*12               preload entries     {crc, length, offset} x P
    ... + P*12              mappings            uint16 x N, 0xFFFF = none
    ... + N*2               preload data
    header.itemOffset       items {crc, nameOffset, timeCreated} x N,
                            then the name blob; nameOffset is absolute
"""
import struct
import sys

XZP_MAGIC = 0x785A6970          # 'piZx'
NO_PRELOAD = 0xFFFF
MAX_COMPARE = 1 << 20           # skip absurd entries rather than read them


class XzpError(Exception):
    pass


class Archive:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(36)
        if len(head) < 36 or struct.unpack_from("<I", head, 0)[0] != XZP_MAGIC:
            raise XzpError("not an XZP archive")
        (self.version, self.preload_count, self.count, self.preload_bytes,
         self.header_len, self.item_count, self.item_offset,
         self.item_len) = struct.unpack_from("<8I", head, 4)

        de = self.header_len
        pe = de + self.count * 12
        mp = pe + self.preload_count * 12
        # From the start of the file: every offset below is absolute, so the
        # buffer has to be too.
        self.f.seek(0)
        table = self.f.read(mp + self.count * 2)

        self.entries = [struct.unpack_from("<3I", table, de + i * 12)
                        for i in range(self.count)]
        self.preload = [struct.unpack_from("<3I", table, pe + i * 12)
                        for i in range(self.preload_count)]
        self.mapping = struct.unpack_from("<%dH" % self.count, table, mp)

    def read_at(self, offset, length):
        self.f.seek(offset)
        return self.f.read(length)

    def duplicated(self):
        """Yield (index, length, preload_offset, file_offset) for each file
        stored whole in both places."""
        for i, (crc, length, offset) in enumerate(self.entries):
            pi = self.mapping[i]
            if pi == NO_PRELOAD or pi >= self.preload_count:
                continue
            _pcrc, plength, poffset = self.preload[pi]
            if plength != length or not 0 < length <= MAX_COMPARE:
                continue
            yield i, length, poffset, offset


def verify(path, block_size=0x4000):
    ar = Archive(path)
    compared = mismatched = differing = 0
    blocks = {}

    for _i, length, poffset, offset in ar.duplicated():
        a = ar.read_at(poffset, length)
        b = ar.read_at(offset, length)
        compared += 1
        if a == b:
            continue
        mismatched += 1
        for k in range(length):
            if a[k] != b[k]:
                differing += 1
                blocks[(poffset + k) // block_size] = 1
                blocks[(offset + k) // block_size] = 1

    return compared, mismatched, differing, sorted(blocks)


def _self_check():
    """The check must catch a single flipped byte, and pass a clean archive."""
    import io
    import os
    import tempfile

    count, preload_count = 2, 1
    body = b"the quick brown fox jumps over the lazy dog\n" * 4
    length = len(body)

    header_len = 36
    de = header_len
    pe = de + count * 12
    mp = pe + preload_count * 12
    data_at = mp + count * 2

    poffset = data_at
    offset = data_at + length

    head = struct.pack("<6I", XZP_MAGIC, 6, preload_count, count, 0,
                       header_len)
    head += struct.pack("<3I", count, 0, 0)
    entries = struct.pack("<3I", 0xAAAA, length, offset)
    entries += struct.pack("<3I", 0xBBBB, 0, 0)
    preload = struct.pack("<3I", 0xAAAA, length, poffset)
    mapping = struct.pack("<2H", 0, NO_PRELOAD)
    blob = head + entries + preload + mapping + body + body

    path = os.path.join(tempfile.gettempdir(), "xzp_selfcheck.bin")
    with open(path, "wb") as fh:
        fh.write(blob)
    compared, bad, diff, _ = verify(path)
    assert (compared, bad, diff) == (1, 0, 0), (compared, bad, diff)

    # Corrupt one byte of the preload copy the way the memcpy bug did: a zero
    # where a repeated byte belonged.
    broken = bytearray(blob)
    broken[poffset + 5] = 0
    with open(path, "wb") as fh:
        fh.write(bytes(broken))
    compared, bad, diff, _ = verify(path)
    assert (compared, bad, diff) == (1, 1, 1), (compared, bad, diff)

    os.unlink(path)
    print("xzp_verify self-check: ok")
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--self-check":
        return _self_check()
    if len(argv) < 2:
        print(__doc__)
        return 2

    rc = 0
    for path in argv[1:]:
        compared, bad, differing, blocks = verify(path)
        print("%s" % path)
        if compared == 0:
            print("  no file is stored twice -- nothing to check against")
            continue
        print("  compared %d duplicated files" % compared)
        if bad:
            print("  %d MISMATCHING, %d differing bytes, %d blocks implicated"
                  % (bad, differing, len(blocks)))
            print("  first blocks: %s" % ", ".join(str(b) for b in blocks[:8]))
            rc = 1
        else:
            print("  all identical -- decompression is byte-correct here")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
