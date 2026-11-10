#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Shield-1.0.0
# Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
# Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
"""
Zero-dependency PNG decoder & 1-bit bitmap generator for beanzilla-black.png.
Generates esp32/include/beanzilla_logo.h containing:
  1. BEANZILLA_BOOT_BITMAP (256x168, 5,376 bytes PROGMEM) for the RLCD boot splash
  2. BEANZILLA_MINI_BITMAP (40x24, 120 bytes PROGMEM) for the live dashboard header/chart
"""

from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent.parent
PNG_PATH = ROOT / "beanzilla-black.png"
OUT_HEADER = Path(__file__).resolve().parent / "include" / "beanzilla_logo.h"


def decode_rgba_png(path: Path) -> tuple[int, int, list[list[float]]]:
    """Decodes an 8-bit RGBA non-interlaced PNG into a 2D luminance array [0.0=black .. 255.0=white]."""
    raw = path.read_bytes()
    pos = 8
    w = h = 0
    idat_chunks: list[bytes] = []

    while pos < len(raw):
        length = struct.unpack(">I", raw[pos : pos + 4])[0]
        ctype = raw[pos + 4 : pos + 8]
        cdata = raw[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            w, h, bd, ct, _, _, _ = struct.unpack(">IIBBBBB", cdata)
            assert bd == 8 and ct == 6, f"Expected 8-bit RGBA, got bd={bd} ct={ct}"
        elif ctype == b"IDAT":
            idat_chunks.append(cdata)

    decomp = zlib.decompress(b"".join(idat_chunks))
    stride = w * 4
    prev_row = bytearray(stride)
    lum_grid: list[list[float]] = []

    def paeth(a: int, b: int, c: int) -> int:
        p = a + b - c
        pa = abs(p - a)
        pb = abs(p - b)
        pc = abs(p - c)
        if pa <= pb and pa <= pc:
            return a
        if pb <= pc:
            return b
        return c

    offset = 0
    for _y in range(h):
        ftype = decomp[offset]
        offset += 1
        cur_row = bytearray(decomp[offset : offset + stride])
        offset += stride

        if ftype == 1:  # Sub
            for i in range(4, stride):
                cur_row[i] = (cur_row[i] + cur_row[i - 4]) & 0xFF
        elif ftype == 2:  # Up
            for i in range(stride):
                cur_row[i] = (cur_row[i] + prev_row[i]) & 0xFF
        elif ftype == 3:  # Average
            for i in range(stride):
                left = cur_row[i - 4] if i >= 4 else 0
                up = prev_row[i]
                cur_row[i] = (cur_row[i] + ((left + up) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                left = cur_row[i - 4] if i >= 4 else 0
                up = prev_row[i]
                up_left = prev_row[i - 4] if i >= 4 else 0
                cur_row[i] = (cur_row[i] + paeth(left, up, up_left)) & 0xFF

        prev_row = cur_row
        row_lum: list[float] = []
        for x in range(w):
            r = cur_row[x * 4 + 0]
            g = cur_row[x * 4 + 1]
            b = cur_row[x * 4 + 2]
            a = cur_row[x * 4 + 3] / 255.0
            # Composite onto white paper background (255, 255, 255)
            lum = (0.299 * r + 0.587 * g + 0.114 * b) * a + 255.0 * (1.0 - a)
            row_lum.append(lum)
        lum_grid.append(row_lum)

    return w, h, lum_grid


def autocrop(lum_grid: list[list[float]], threshold: float = 240.0) -> list[list[float]]:
    h = len(lum_grid)
    w = len(lum_grid[0])
    min_x, min_y = w, h
    max_x, max_y = 0, 0
    for y in range(h):
        for x in range(w):
            if lum_grid[y][x] < threshold:
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y
    pad = 4
    min_x = max(0, min_x - pad)
    min_y = max(0, min_y - pad)
    max_x = min(w - 1, max_x + pad)
    max_y = min(h - 1, max_y + pad)
    return [row[min_x : max_x + 1] for row in lum_grid[min_y : max_y + 1]]


def resize_and_pack_1bit(
    cropped: list[list[float]], target_w: int, target_h: int, dither_gray: bool = True
) -> list[int]:
    """
    Area-averages cropped luminance into (target_w x target_h) while preserving
    aspect ratio, and packs into row-major 1-bit bytes (MSB first, 1 = black pixel).
    Uses ordered 2x2 dithering on the mid-gray dorsal stripe so it stands out from
    the solid black body.
    """
    assert target_w % 8 == 0, "target_w must be a multiple of 8"
    src_h = len(cropped)
    src_w = len(cropped[0])

    scale = min(target_w / src_w, target_h / src_h)
    draw_w = int(round(src_w * scale))
    draw_h = int(round(src_h * scale))
    off_x = (target_w - draw_w) // 2
    off_y = (target_h - draw_h) // 2

    bayer2x2 = ((0, 2), (3, 1))
    out_bytes: list[int] = []

    for ty in range(target_h):
        for bx in range(0, target_w, 8):
            byte_val = 0
            for bit in range(8):
                tx = bx + bit
                lx = tx - off_x
                ly = ty - off_y
                if 0 <= lx < draw_w and 0 <= ly < draw_h:
                    # Bilinear / box sample from cropped source
                    sx0 = (lx * src_w) / draw_w
                    sy0 = (ly * src_h) / draw_h
                    sx1 = ((lx + 1) * src_w) / draw_w
                    sy1 = ((ly + 1) * src_h) / draw_h
                    ix0, iy0 = int(sx0), int(sy0)
                    ix1 = min(src_w - 1, int(sx1))
                    iy1 = min(src_h - 1, int(sy1))
                    total = 0.0
                    cnt = 0
                    for sy in range(iy0, iy1 + 1):
                        for sx in range(ix0, ix1 + 1):
                            total += cropped[sy][sx]
                            cnt += 1
                    lum = total / max(1, cnt)

                    is_black = False
                    if lum < 95.0:
                        is_black = True
                    elif dither_gray and lum < 195.0:
                        # Mid-gray dorsal stripe -> 50% checkerboard dither
                        is_black = bayer2x2[ty & 1][tx & 1] < 2
                    if is_black:
                        byte_val |= 1 << (7 - bit)
            out_bytes.append(byte_val)

    return out_bytes


def format_c_array(name: str, w: int, h: int, data: list[int]) -> list[str]:
    lines = [
        f"static constexpr uint16_t {name}_W = {w};",
        f"static constexpr uint16_t {name}_H = {h};",
        f"static constexpr size_t   {name}_LEN = {len(data)};",
        f"static const uint8_t {name}[{len(data)}] PROGMEM = {{",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    return lines


def main() -> None:
    w, h, lum = decode_rgba_png(PNG_PATH)
    cropped = autocrop(lum)
    print(f"Decoded {PNG_PATH.name}: {w}x{h} -> autocropped to {len(cropped[0])}x{len(cropped)}")

    boot_w, boot_h = 240, 156
    boot_bytes = resize_and_pack_1bit(cropped, boot_w, boot_h, dither_gray=True)

    mini_w, mini_h = 48, 30
    mini_bytes = resize_and_pack_1bit(cropped, mini_w, mini_h, dither_gray=False)

    header_lines = [
        "#pragma once",
        "// SPDX-License-Identifier: PolyForm-Shield-1.0.0",
        "// Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.",
        "// Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.",
        "// Auto-generated by esp32/gen_beanzilla_logo.py from beanzilla-black.png",
        "// Beanzilla Coffee Worldwide — 1-bit monochrome PROGMEM bitmaps (MSB first, 1 = Black)",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "#ifdef ARDUINO",
        "#include <pgmspace.h>",
        "#else",
        "#ifndef PROGMEM",
        "#define PROGMEM",
        "#endif",
        "#ifndef pgm_read_byte",
        "#define pgm_read_byte(addr) (*(const uint8_t*)(addr))",
        "#endif",
        "#endif",
        "",
        "namespace bulletspy {",
        "",
        *format_c_array("BEANZILLA_BOOT_BITMAP", boot_w, boot_h, boot_bytes),
        "",
        *format_c_array("BEANZILLA_MINI_BITMAP", mini_w, mini_h, mini_bytes),
        "",
        "} // namespace bulletspy",
        "",
    ]

    OUT_HEADER.write_text("\n".join(header_lines), encoding="utf-8")
    print(
        f"Wrote {OUT_HEADER} "
        f"(Boot: {boot_w}x{boot_h} = {len(boot_bytes)} B, Mini: {mini_w}x{mini_h} = {len(mini_bytes)} B)"
    )


if __name__ == "__main__":
    main()
