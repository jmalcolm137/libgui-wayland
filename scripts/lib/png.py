"""Minimal PNG reading for the test scripts (stdlib only: CI has no Pillow).

Handles what the headless compositor and LIBWM_DUMP write: 8-bit RGB/RGBA,
non-interlaced.
"""
import struct
import zlib

def read_png_rgba(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    w = h = color_type = 0
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos+4])[0]
        tag = data[pos+4:pos+8]
        chunk = data[pos+8:pos+8+length]
        pos += 12 + length
        if tag == b"IHDR":
            w, h, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", chunk)
            assert bit_depth == 8
        elif tag == b"IDAT":
            idat.extend(chunk)
    raw = zlib.decompress(bytes(idat))
    bpp = 4 if color_type == 6 else 3
    stride = w * bpp
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(h):
        ftype = raw[i]
        row = bytearray(raw[i+1:i+1+stride])
        i += 1 + stride
        if ftype == 1:
            for x in range(bpp, stride):
                row[x] = (row[x] + row[x - bpp]) & 0xff
        elif ftype == 2:
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 0xff
        elif ftype == 3:
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                b = prev[x]
                row[x] = (row[x] + ((a + b) >> 1)) & 0xff
        elif ftype == 4:
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[x] = (row[x] + pr) & 0xff
        prev = row
        rows.append((row, bpp))
    return w, h, rows

def pixel_rgb(rows, x, y):
    row, bpp = rows[y]
    off = x * bpp
    return (row[off], row[off+1], row[off+2])
