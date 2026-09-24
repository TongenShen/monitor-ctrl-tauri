#!/usr/bin/env python3
"""
生成 src-tauri/icons/icon.ico

为什么要写脚本而不是直接丢一个 .ico 进去:
  * 图标是纯几何图形, 用代码描述比二进制文件更容易 review 和微调。
  * 多尺寸 (16..256) 一次生成, 每个尺寸独立超采样, 小尺寸下也清晰。
  * 不依赖 Pillow —— 只用标准库 zlib/struct。

运行:  python scripts/make-icon.py
"""

import os
import struct

# ---------------------------------------------------------------- 调色板
BG_TOP = (0x1F, 0x2A, 0x38)      # 圆角底板渐变起点
BG_BOT = (0x0D, 0x11, 0x17)      # 圆角底板渐变终点
ACCENT = (0x4A, 0x9E, 0xFF)      # 显示器轮廓
SCREEN = (0x10, 0x1A, 0x26)      # 屏幕内部 (比底板稍暗)

SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4                            # 每轴超采样倍数 -> 4x4 = 16 样本/像素


# ---------------------------------------------------------------- 几何
def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def round_rect_sdf(x, y, x0, y0, x1, y1, r):
    """圆角矩形有符号距离: 负数=内部, 正数=外部。"""
    r = min(r, (x1 - x0) * 0.5, (y1 - y0) * 0.5)
    cx = _clamp(x, x0 + r, x1 - r)
    cy = _clamp(y, y0 + r, y1 - r)
    dx = x - cx
    dy = y - cy
    return (dx * dx + dy * dy) ** 0.5 - r


def lerp(a, b, t):
    return a + (b - a) * t


# ---------------------------------------------------------------- 绘制
def sample(u, v, px):
    """
    u, v ∈ [0,1] 归一化坐标, px = 1/边长 (用于把线宽换算成归一化单位)。
    返回 (r, g, b, a), 分量 0..255。
    """
    # --- 底板: 圆角矩形 + 垂直渐变 ---
    d_bg = round_rect_sdf(u, v, 0.0, 0.0, 1.0, 1.0, 0.215)
    if d_bg > 0.0:
        return (0, 0, 0, 0)

    t = _clamp(v, 0.0, 1.0)
    r = lerp(BG_TOP[0], BG_BOT[0], t)
    g = lerp(BG_TOP[1], BG_BOT[1], t)
    b = lerp(BG_TOP[2], BG_BOT[2], t)

    # 底板外缘一圈极淡的高光, 让图标在深色任务栏上不至于糊成一团
    if d_bg > -0.012:
        k = 1.0 - (d_bg + 0.012) / 0.012
        r = lerp(r, 0x3A, k * 0.35)
        g = lerp(g, 0x46, k * 0.35)
        b = lerp(b, 0x57, k * 0.35)

    stroke = 0.036                       # 轮廓线宽 (归一化)

    # --- 屏幕内部填充 ---
    d_scr = round_rect_sdf(u, v, 0.185, 0.205, 0.815, 0.615, 0.055)
    if d_scr < 0.0:
        r, g, b = SCREEN

    # --- 屏幕轮廓 (描边) ---
    if abs(d_scr) < stroke * 0.5:
        r, g, b = ACCENT

    # --- 支架立柱 ---
    d_neck = round_rect_sdf(u, v, 0.455, 0.615, 0.545, 0.765, 0.028)
    if d_neck < 0.0:
        r, g, b = ACCENT

    # --- 底座 ---
    d_base = round_rect_sdf(u, v, 0.325, 0.755, 0.675, 0.815, 0.030)
    if d_base < 0.0:
        r, g, b = ACCENT

    # --- 屏幕里画一条"信号"横线, 让 16x16 下也能看出是显示器 ---
    d_wave = round_rect_sdf(u, v, 0.275, 0.375, 0.725, 0.425, 0.024)
    if d_wave < 0.0:
        r, g, b = ACCENT

    return (int(r + 0.5), int(g + 0.5), int(b + 0.5), 255)


def render(size):
    """超采样渲染一张 size×size 的 BGRA (bottom-up) 像素表。"""
    inv = 1.0 / size
    off = inv / (SS * 2.0)
    step = inv / SS
    px = inv

    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            ar = ag = ab = aa = 0
            for sy in range(SS):
                v = y * inv + off + sy * step
                for sx in range(SS):
                    u = x * inv + off + sx * step
                    cr, cg, cb, ca = sample(u, v, px)
                    # 预乘 alpha 再累加, 避免边缘出现黑边
                    ar += cr * ca
                    ag += cg * ca
                    ab += cb * ca
                    aa += ca
            n = SS * SS
            if aa == 0:
                row.append((0, 0, 0, 0))
            else:
                a = aa / n
                row.append((
                    int(ar / aa + 0.5),
                    int(ag / aa + 0.5),
                    int(ab / aa + 0.5),
                    int(a + 0.5),
                ))
        rows.append(row)
    rows.reverse()                       # BMP 是 bottom-up
    return rows


# ---------------------------------------------------------------- ICO 封装
def bmp_image(rows, size):
    """构造 ICO 里的 BMP 段: BITMAPINFOHEADER + BGRA 像素 + AND 掩码。"""
    header = struct.pack(
        '<IiiHHIIiiII',
        40,          # biSize
        size,        # biWidth
        size * 2,    # biHeight (XOR + AND)
        1,           # biPlanes
        32,          # biBitCount
        0,           # biCompression = BI_RGB
        0, 0, 0, 0, 0,
    )

    xor = bytearray()
    for row in rows:
        for (r, g, b, a) in row:
            xor += bytes((b, g, r, a))

    # AND 掩码: 每行按 4 字节对齐, 1 = 透明
    stride = ((size + 31) // 32) * 4
    and_mask = bytearray()
    for row in rows:
        line = bytearray(stride)
        for x, (_r, _g, _b, a) in enumerate(row):
            if a < 128:
                line[x >> 3] |= 0x80 >> (x & 7)
        and_mask += line

    return bytes(header) + bytes(xor) + bytes(and_mask)


def build_ico(path):
    images = [(s, bmp_image(render(s), s)) for s in SIZES]

    out = bytearray(struct.pack('<HHH', 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack(
            '<BBBBHHII',
            0 if size >= 256 else size,   # 256 用 0 表示
            0 if size >= 256 else size,
            0, 0, 1, 32,
            len(data), offset,
        )
        offset += len(data)
    for _size, data in images:
        out += data

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(out)
    return len(out)


if __name__ == '__main__':
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(root, 'src-tauri', 'icons', 'icon.ico')
    n = build_ico(target)
    print(f'[icon] 已生成 {target}')
    print(f'[icon] 尺寸: {", ".join(str(s) for s in SIZES)}  ({n} 字节)')
