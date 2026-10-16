#!/usr/bin/env python3
"""Losslessly index the pinned upstream's RGB565 stock UI images for LVGL 8."""
from pathlib import Path
import re
import struct
import sys


def encode(source: str) -> tuple[str, int]:
    fmt = re.search(r'\.header\.cf\s*=\s*(LV_IMG_CF_\w+)', source)
    if not fmt or fmt[1] not in ('LV_IMG_CF_TRUE_COLOR', 'LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED'):
        return source, 0
    match = re.search(r'#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0\s*(.*?)#endif', source, re.S)
    if not match:
        raise ValueError('Missing RGB565 source pixels')
    pixels = re.sub(r'/\*.*?\*/', '', match[1], flags=re.S)
    raw = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', pixels))
    width = int(re.search(r'\.header\.w\s*=\s*(\d+)', source)[1])
    height = int(re.search(r'\.header\.h\s*=\s*(\d+)', source)[1])
    if len(raw) != width * height * 2:
        raise ValueError('RGB565 dimensions do not match source data')
    colors = list(struct.unpack(f'<{width * height}H', raw))
    palette = list(dict.fromkeys(colors))
    if len(palette) > 256:
        return source, 0
    bits = next(bits for bits in (1, 2, 4, 8) if len(palette) <= 1 << bits)
    stride = (width * bits + 7) // 8
    data = bytearray((1 << bits) * 4 + stride * height)
    for i, color in enumerate(palette):
        # LVGL palettes contain BGRA8888. Shifting preserves every RGB565 bit.
        alpha = 0 if fmt[1].endswith('CHROMA_KEYED') and color == 0xf81f else 255
        data[i * 4:i * 4 + 4] = bytes(((color & 31) << 3, ((color >> 5) & 63) << 2,
                                     ((color >> 11) & 31) << 3, alpha))
    lookup = {color: i for i, color in enumerate(palette)}
    offset = (1 << bits) * 4
    for y in range(height):
        for x in range(width):
            shift = 8 - bits - (x * bits % 8)
            data[offset + y * stride + x * bits // 8] |= lookup[colors[y * width + x]] << shift
    # Check the encoded pixels and transparency against the original image.
    decoded = bytearray()
    for y in range(height):
        for x in range(width):
            index = (data[offset + y * stride + x * bits // 8] >> (8 - bits - x * bits % 8)) & ((1 << bits) - 1)
            b, g, r, alpha = data[index * 4:index * 4 + 4]
            color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            original = colors[y * width + x]
            expected_alpha = 0 if fmt[1].endswith('CHROMA_KEYED') and original == 0xf81f else 255
            if color != original or alpha != expected_alpha:
                raise ValueError('Indexed image failed lossless round trip')
            decoded.extend(struct.pack('<H', color))
    if bytes(decoded) != raw:
        raise ValueError('Pixel bytes changed')
    if len(data) >= len(raw):
        return source, 0
    name = re.search(r'const lv_img_dsc_t (\w+)\s*=', source)[1]
    lines = [', '.join(f'0x{value:02x}' for value in data[i:i + 20]) for i in range(0, len(data), 20)]
    encoded = ('/* Generated losslessly from ModRetro RGB565 source by optimize_images.py. */\n'
               '#include "lvgl.h"\n\n'
               f'_Alignas(4) static const uint8_t {name}_map[] = {{\n  ' + ',\n  '.join(lines) + '\n};\n\n'
               f'const lv_img_dsc_t {name} = {{\n'
               f'  .header.cf = LV_IMG_CF_INDEXED_{bits}BIT,\n'
               f'  .header.w = {width}, .header.h = {height},\n'
               f'  .data_size = sizeof {name}_map, .data = {name}_map,\n}};\n')
    return encoded, len(raw) - len(data)


def optimize(directory: Path) -> int:
    saved = 0
    for path in sorted(directory.glob('*.c')):
        result, reduction = encode(path.read_text())
        if reduction:
            path.write_text(result)
            saved += reduction
    print(f'Lossless stock image reduction: {saved:,} bytes')
    return saved


if __name__ == '__main__':
    optimize(Path(sys.argv[1]))
