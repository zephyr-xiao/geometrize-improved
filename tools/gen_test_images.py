# -*- coding: utf-8 -*-
# 程序合成测试图生成器:压各种光栅化路径的针对性图样
# 用法: python gen_test_images.py <输出目录>
import math
import os
import random
import shutil
import sys

from PIL import Image, ImageDraw


def gen_concentric_rings(size=512):
    """同心圆环:大半径圆密集出现,专压 Circle O(r^2) 光栅化"""
    img = Image.new("RGBA", (size, size), (240, 240, 245, 255))
    draw = ImageDraw.Draw(img)
    cx = cy = size // 2
    for i in range(40):
        r = 12 + i * 6
        color = (255 - i * 4 % 200, 60 + (i * 13) % 180, 120 + (i * 29) % 130, 255)
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=color, width=3)
    return img


def gen_diagonal_hatch(size=512):
    """斜线格:细扫描线密集,压 Line/Bresenham 与窄段 copy/memcpy 路径"""
    img = Image.new("RGBA", (size, size), (25, 30, 45, 255))
    draw = ImageDraw.Draw(img)
    for offset in range(-size, size * 2, 9):
        draw.line([(offset, 0), (offset + size, size)], fill=(220, 190, 90, 255), width=1)
    for offset in range(-size, size * 2, 11):
        draw.line([(0, offset), (size, offset + size)], fill=(80, 160, 210, 255), width=1)
    return img


def gen_gradient_noise(size=512, seed=42):
    """渐变+噪声:色彩连续无平坦区,压制 computeColor 早退机会并逼出全管线"""
    rng = random.Random(seed)
    img = Image.new("RGBA", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            r = int(255 * x / size) + rng.randint(-14, 14)
            g = int(255 * y / size) + rng.randint(-14, 14)
            b = int(128 + 127 * math.sin(x * 0.02) * math.cos(y * 0.02)) + rng.randint(-10, 10)
            px[x, y] = (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)), 255)
    return img


def gen_triangle_patchwork(size=512, seed=7):
    """三角马赛克:压 Triangle/polygon 光栅化路径"""
    rng = random.Random(seed)
    img = Image.new("RGBA", (size, size), (250, 248, 242, 255))
    draw = ImageDraw.Draw(img)
    cell = 48
    for gy in range(0, size, cell):
        for gx in range(0, size, cell):
            for _ in range(3):
                pts = [(gx + rng.randint(0, cell), gy + rng.randint(0, cell)) for _ in range(3)]
                color = (rng.randint(40, 255), rng.randint(40, 255), rng.randint(40, 255), 255)
                draw.polygon(pts, fill=color)
    return img


def gen_flat_color(size=512):
    """纯色图:高拒绝率场景,压 Model::step 的回滚路径与退化扫描线"""
    return Image.new("RGBA", (size, size), (128, 128, 132, 255))


def gen_large_gradient(size=2048):
    """2048 大图渐变:带宽型压测(B5/B6 拷贝税放大器)"""
    return gen_gradient_noise(size, seed=99)


def main():
    if len(sys.argv) != 2:
        print("用法: python gen_test_images.py <输出目录>")
        sys.exit(1)
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    images = {
        "rings_512.png": gen_concentric_rings(),
        "hatch_512.png": gen_diagonal_hatch(),
        "gradnoise_512.png": gen_gradient_noise(),
        "triangles_512.png": gen_triangle_patchwork(),
        "flat_512.png": gen_flat_color(),
        "gradnoise_1024.png": gen_gradient_noise(1024, seed=7),
        "gradnoise_2048.png": gen_large_gradient(),
    }
    # 小尺寸边界用例
    images["tiny_64.png"] = gen_gradient_noise(64, seed=3)
    images["wide_32x1024.png"] = gen_diagonal_hatch().crop((0, 0, 32, 1024))

    for name, img in images.items():
        path = os.path.join(out_dir, name)
        img.save(path)
        print(f"已生成 {path} ({img.width}x{img.height})")

    copy_upstream_assets(out_dir)


def copy_upstream_assets(out_dir):
    """tree_under_clouds.png 是上游截图资产(摄影图,无法程序合成),run_ab 多个用例依赖它。
    从 upstream 只读快照复制,保证再生后与 golden 对拍基线逐字节一致。"""
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    upstream_image = os.path.join(repo_root, "upstream", "geometrize-lib", "screenshots", "tree_under_clouds.png")
    if not os.path.isfile(upstream_image):
        print(f"警告: 未找到上游快照 {upstream_image},tree_under_clouds.png 未复制", file=sys.stderr)
        return
    dst = os.path.join(out_dir, "tree_under_clouds.png")
    shutil.copyfile(upstream_image, dst)
    print(f"已从上游快照复制 {dst}")


if __name__ == "__main__":
    main()
