# -*- coding: utf-8 -*-
"""生成歌词页使用的 44px CJK 字体。

字体文件约 34MB，不放进 Git 仓库。首次克隆后运行：

    python tools/gen_font.py

依赖：
  - Node.js + npm（用于安装 lv_font_conv）
  - 一个含中文的 TTF，默认使用 Windows 的 C:/Windows/Fonts/Deng.ttf
    （等线；也可以用 --font 指定其他字体）

生成结果：components/music_lyrics/fonts/lyrics_font_44.c
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "components", "music_lyrics", "fonts", "lyrics_font_44.c")
BUILD = os.path.join(ROOT, "tools", ".font_build")
LV_FONT_CONV_VERSION = "1.5.3"


def gb2312_chars():
    chars = []
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if "\u4e00" <= ch <= "\u9fff":
                chars.append(ch)
    # 去重，保持稳定顺序
    return list(dict.fromkeys(chars))


def run(cmd):
    if os.name == "nt" and cmd[0].lower().endswith((".cmd", ".bat")):
        cmd = [os.environ.get("COMSPEC", "cmd.exe"), "/c"] + cmd
    print("+", " ".join(cmd[:4]), "...")
    subprocess.run(cmd, cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description="生成 lyrics_font_44.c")
    parser.add_argument("--font", default=r"C:\Windows\Fonts\Deng.ttf",
                        help="源 TTF 路径，默认 Windows 等线")
    args = parser.parse_args()

    if not os.path.isfile(args.font):
        sys.exit("找不到源字体：%s\n请用 --font 指定一个含中文字形的 TTF。" % args.font)

    node = shutil.which("node")
    npm = shutil.which("npm")
    if not node or not npm:
        sys.exit("找不到 Node.js/npm，请先安装 Node.js 后再运行。")

    os.makedirs(BUILD, exist_ok=True)
    module = os.path.join(BUILD, "node_modules", "lv_font_conv", "lv_font_conv.js")
    if not os.path.isfile(module):
        run([npm, "install", "--prefix", BUILD,
             "lv_font_conv@" + LV_FONT_CONV_VERSION])

    if not os.path.isfile(module):
        sys.exit("lv_font_conv 安装失败，请检查 npm 网络/代理后重试。")

    chars = "".join(gb2312_chars())
    print("GB2312 汉字：%d 个" % len(chars))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    run([
        node, module,
        "--font", args.font,
        "--size", "44",
        "--bpp", "4",
        "--format", "lvgl",
        "--lv-font-name", "lyrics_font_44",
        "--lv-include", "lvgl.h",
        "--no-compress",
        "--range", "0x20-0x7F",
        "--range", "0xA0-0xFF",
        "--range", "0x3000-0x303F",
        "--range", "0x3040-0x309F",
        "--range", "0x30A0-0x30FF",
        "--range", "0xFF00-0xFFEF",
        "--symbols", chars,
        "-o", OUT,
    ])
    print("已生成：%s（%.1f MB）" % (OUT, os.path.getsize(OUT) / 1024.0 / 1024.0))


if __name__ == "__main__":
    main()
