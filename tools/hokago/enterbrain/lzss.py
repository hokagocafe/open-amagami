"""Decompress and compress the game's LZSS streams (used by AMAGAMI.IMG).

Usage:
  python -m hokago lzss unpack <in> <out>
  python -m hokago lzss pack   <in> <out>
  python -m hokago lzss verify <compressed file>     (unpack + pack, compare)

Format: a little-endian u32 with the decompressed size, then a classic LZSS
stream with a 2048-byte window and matches of 3 to 34 bytes. See
docs/technical/scf-format.md.
"""
import struct
import sys

N = 2048          # window size
F = 34            # longest match
THRESHOLD = 2     # matches must be longer than this
NIL = N


def unpack(data):
    size = struct.unpack_from("<I", data, 0)[0]
    window = bytearray(N)
    r = N - F
    out = bytearray()
    i = 4
    flags = 0
    while len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            flags = data[i] | 0xFF00
            i += 1
        if flags & 1:
            c = data[i]
            i += 1
            out.append(c)
            window[r] = c
            r = (r + 1) & (N - 1)
        else:
            pos = data[i] | ((data[i + 1] & 0xE0) << 3)
            length = (data[i + 1] & 0x1F) + THRESHOLD + 1
            i += 2
            for k in range(min(length, size - len(out))):
                c = window[(pos + k) & (N - 1)]
                out.append(c)
                window[r] = c
                r = (r + 1) & (N - 1)
    return bytes(out)


class _Tree:
    """Binary search trees over window positions, as in Okumura's LZSS.C."""

    def __init__(self, text):
        self.text = text
        self.lson = [NIL] * (N + 1)
        self.rson = [NIL] * (N + 257)
        self.dad = [NIL] * (N + 1)
        self.match_position = 0
        self.match_length = 0

    def insert(self, r):
        text, lson, rson, dad = self.text, self.lson, self.rson, self.dad
        cmp = 1
        p = N + 1 + text[r]
        rson[r] = lson[r] = NIL
        self.match_length = 0
        while True:
            if cmp >= 0:
                if rson[p] != NIL:
                    p = rson[p]
                else:
                    rson[p] = r
                    dad[r] = p
                    return
            else:
                if lson[p] != NIL:
                    p = lson[p]
                else:
                    lson[p] = r
                    dad[r] = p
                    return
            i = 1
            while i < F:
                cmp = text[r + i] - text[p + i]
                if cmp != 0:
                    break
                i += 1
            if i > self.match_length:
                self.match_position = p
                self.match_length = i
                if i >= F:
                    break
        dad[r] = dad[p]
        lson[r] = lson[p]
        rson[r] = rson[p]
        dad[lson[p]] = r
        dad[rson[p]] = r
        if rson[dad[p]] == p:
            rson[dad[p]] = r
        else:
            lson[dad[p]] = r
        dad[p] = NIL

    def delete(self, p):
        lson, rson, dad = self.lson, self.rson, self.dad
        if dad[p] == NIL:
            return
        if rson[p] == NIL:
            q = lson[p]
        elif lson[p] == NIL:
            q = rson[p]
        else:
            q = lson[p]
            if rson[q] != NIL:
                while rson[q] != NIL:
                    q = rson[q]
                rson[dad[q]] = lson[q]
                dad[lson[q]] = dad[q]
                lson[q] = lson[p]
                dad[lson[p]] = q
            rson[q] = rson[p]
            dad[rson[p]] = q
        dad[q] = dad[p]
        if rson[dad[p]] == p:
            rson[dad[p]] = q
        else:
            lson[dad[p]] = q
        dad[p] = NIL


def pack(data):
    out = bytearray(struct.pack("<I", len(data)))
    if not data:
        return bytes(out)
    text = bytearray(N + F - 1)
    tree = _Tree(text)
    code = bytearray(1)
    mask = 1
    s = 0
    r = N - F
    src = 0
    length = min(F, len(data))
    text[r:r + length] = data[:length]
    src = length
    for i in range(1, F + 1):
        tree.insert(r - i)
    tree.insert(r)
    while length > 0:
        if tree.match_length > length:
            tree.match_length = length
        if tree.match_length <= THRESHOLD:
            tree.match_length = 1
            code[0] |= mask
            code.append(text[r])
        else:
            code.append(tree.match_position & 0xFF)
            code.append(((tree.match_position >> 3) & 0xE0) | (tree.match_length - (THRESHOLD + 1)))
        mask = (mask << 1) & 0xFF
        if mask == 0:
            out += code
            code = bytearray(1)
            mask = 1
        last = tree.match_length
        i = 0
        while i < last and src < len(data):
            tree.delete(s)
            c = data[src]
            src += 1
            text[s] = c
            if s < F - 1:
                text[s + N] = c
            s = (s + 1) & (N - 1)
            r = (r + 1) & (N - 1)
            tree.insert(r)
            i += 1
        while i < last:
            i += 1
            tree.delete(s)
            s = (s + 1) & (N - 1)
            r = (r + 1) & (N - 1)
            length -= 1
            if length:
                tree.insert(r)
    if len(code) > 1:
        out += code
    return bytes(out)


def main():
    cmd = sys.argv[1]
    with open(sys.argv[2], "rb") as f:
        data = f.read()
    if cmd == "unpack":
        result = unpack(data)
    elif cmd == "pack":
        result = pack(data)
    elif cmd == "verify":
        again = pack(unpack(data))
        if again == data:
            print(f"OK: {sys.argv[2]} round-trips identically")
            return
        diff = next((i for i, (a, b) in enumerate(zip(data, again)) if a != b), min(len(data), len(again)))
        raise SystemExit(f"MISMATCH: first differs at {diff:#x} (sizes {len(data)} / {len(again)})")
    else:
        raise SystemExit(__doc__)
    with open(sys.argv[3], "wb") as f:
        f.write(result)
    print(f"{len(data)} -> {len(result)} bytes")


if __name__ == "__main__":
    main()
