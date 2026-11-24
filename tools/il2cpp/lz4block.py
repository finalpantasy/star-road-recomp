"""LZ4 block decoder (own implementation of the public LZ4 block format).

NSO segments are stored as raw LZ4 blocks with no frame header.
"""


def decompress(src, expected_size):
    src = memoryview(src)
    n = len(src)
    out = bytearray()
    i = 0
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if lit:
            out += src[i:i + lit]
            i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0 or off > len(out):
            raise ValueError("bad LZ4 offset %d at src %d" % (off, i))
        mlen = token & 15
        if mlen == 15:
            while True:
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(out) - off
        if off >= mlen:
            out += out[start:start + mlen]
        else:
            # overlapping copy: repeat the pattern
            pat = bytes(out[start:])
            reps = mlen // off + 1
            out += (pat * reps)[:mlen]
    if len(out) != expected_size:
        raise ValueError("LZ4 size mismatch: got %d expected %d" % (len(out), expected_size))
    return bytes(out)
