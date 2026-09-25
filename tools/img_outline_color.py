"""截图「描边取色」分析 —— 判断画面里各目标的描边到底是什么颜色。

用途（docs/18 那一轮就是这么定位的）：用户报「两类东西看起来一个颜色」时，
先把截图里的彩色像素按**色相分桶 + 分块定位**，再对可疑区域取「最饱和的像素」，
用数字（而不是肉眼）判断是「同一个颜色」还是「两个相近颜色」。

用法：
    python tools/img_outline_color.py <截图.png>
    python tools/img_outline_color.py <截图.png> --rect 10,80,330,210 --rect 530,50,790,370

不带 --rect：打全图的色相分桶 + 每桶的 20x12 分块热力图（一眼看出颜色集中在哪里）。
带 --rect  ：额外对每个矩形统计「最饱和/最亮的前若干像素」（RGB + 色相角）。
              矩形格式 x0,y0,x1,y1（像素，左上角为原点）。
"""

import argparse
import colorsys
from collections import Counter

from PIL import Image

BINS = [
    ('红   0-20/340-360', lambda h: h < 20 or h >= 340),
    ('橙   20-45', lambda h: 20 <= h < 45),
    ('黄   45-70', lambda h: 45 <= h < 70),
    ('绿   70-150', lambda h: 70 <= h < 150),
    ('青绿 150-190', lambda h: 150 <= h < 190),
    ('青   190-210', lambda h: 190 <= h < 210),
    ('蓝   210-250', lambda h: 210 <= h < 250),
    ('紫   250-300', lambda h: 250 <= h < 300),
    ('品红 300-340', lambda h: 300 <= h < 340),
]
GX, GY = 20, 12


def hue_of(rgb):
    r, g, b = (c / 255 for c in rgb)
    return colorsys.rgb_to_hsv(r, g, b)[0] * 360


def is_colorful(rgb, min_v=90, min_sat=45):
    return max(rgb) >= min_v and (max(rgb) - min(rgb)) >= min_sat


def bin_of(rgb):
    h = hue_of(rgb)
    for name, f in BINS:
        if f(h):
            return name
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--rect', action='append', default=[], help='x0,y0,x1,y1（可重复）')
    a = ap.parse_args()

    im = Image.open(a.image).convert('RGB')
    w, h = im.size
    px = im.load()
    print(f'image: {a.image}  size={w}x{h}')

    buckets = {name: [] for name, _ in BINS}
    for y in range(h):
        for x in range(w):
            rgb = px[x, y]
            if not is_colorful(rgb):
                continue
            name = bin_of(rgb)
            if name:
                buckets[name].append((x, y, rgb))

    print('\n=== 全图彩色像素的色相分桶 + 分块热力图（20x12）===')
    bw, bh = w / GX, h / GY
    for name, _ in BINS:
        lst = buckets[name]
        if not lst:
            continue
        print(f'\n--- {name}: {len(lst)} px ---')
        top = sorted(lst, key=lambda t: -sum(t[2]))[:4]
        for x, y, rgb in top:
            print(f'    最亮 ({x:4d},{y:3d}) #{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X} hue={hue_of(rgb):5.1f}')
        grid = [[0] * GX for _ in range(GY)]
        for x, y, _rgb in lst:
            grid[min(GY - 1, int(y / bh))][min(GX - 1, int(x / bw))] += 1
        for gy, row in enumerate(grid):
            if sum(row):
                print('    ' + ' '.join(f'{v:4d}' if v else '   .' for v in row))

    for spec in a.rect:
        x0, y0, x1, y1 = (int(v) for v in spec.split(','))
        cnt = Counter()
        best = []
        for y in range(y0, y1):
            for x in range(x0, x1):
                rgb = px[x, y]
                cnt[rgb] += 1
                if is_colorful(rgb):
                    sat = max(rgb) - min(rgb)
                    best.append((sat, x, y, rgb))
        best.sort(reverse=True)
        print(f'\n=== rect {spec} ===')
        print('  出现最多的颜色:', ' '.join(f'#{r:02X}{g:02X}{b:02X}x{n}' for (r, g, b), n in cnt.most_common(5)))
        print('  最饱和的像素（描边就该出现在这里）:')
        seen = set()
        shown = 0
        for sat, x, y, rgb in best:
            if rgb in seen:
                continue
            seen.add(rgb)
            print(f'    ({x:4d},{y:3d}) #{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X} sat={sat:3d} hue={hue_of(rgb):5.1f}')
            shown += 1
            if shown >= 8:
                break


if __name__ == '__main__':
    main()
