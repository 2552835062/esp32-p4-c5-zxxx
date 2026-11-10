# 歌词字体

`lyrics_font_44.c` 是 44px、4bpp、未压缩位图的 CJK 字体（GB2312 6763 字 +
ASCII/假名/标点），约 34MB，不放入 Git 仓库。

首次克隆工程后生成：

```bash
python tools/gen_font.py
```

默认使用 Windows 的 `C:/Windows/Fonts/Deng.ttf`；也可以指定其他中文 TTF：

```bash
python tools/gen_font.py --font D:/fonts/YourFont.ttf
```

要求：

- Node.js/npm（脚本会自动安装 `lv_font_conv@1.5.3` 到 `tools/.font_build/`）；
- 一个包含中文字形的 TTF；
- 生成的 `lyrics_font_44.c` 必须存在，否则 `music_lyrics` 组件会在 CMake 阶段报错。

字体生成参数：

- 44px、4bpp、`--no-compress`（压缩位图在部分 LVGL 版本上会出现花字，因此不用压缩）；
- GB2312 一二级 6763 字 + ASCII + Latin-1 + 平假名/片假名 + CJK 标点 + 全角字符。

字体来源是系统里的等线（DengXian），生成的位图仅用于本工程显示；分发前请自行确认字体授权。
