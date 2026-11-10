# ESP32-P4C5 歌词显示屏

基于 ESP32-P4C5（P4 + C5 ESP-Hosted）和 AXS15260 6.2 寸 1280x452 MIPI-DSI 屏的
纯歌词显示界面。电脑端运行网易云音乐和 `tools/music_agent/music_agent.py`，
板子通过局域网自动发现代理，按播放进度滚动显示歌词，并支持上一首/下一首/暂停。

视觉基准：`docs/MUSIC_UI.png`（516x214 设计稿，按 1280x452 等比放大）。

## 1. 已确认的硬件与软件事实

| 项目 | 结论 |
|---|---|
| 主控 | ESP32-P4C5（P4X，芯片 rev v3.x）+ ESP32-C5，ESP-Hosted-MCU 2.12.9 |
| 屏幕 | AXS15260D 6.2 寸，物理 452x1280，MIPI-DSI 2-lane 1000Mbps，RGB888，60Hz |
| 逻辑分辨率 | 1280x452，横屏（面板竖装，flush 时软件旋转 90 度） |
| 屏幕引脚 | LCD_RST=IO26，LCD_BL=IO30，触摸 SDA=IO28 / SCL=IO29 / INT=IO27 |
| 触摸 | AXS15260 内置触摸，I2C 地址 0x3B，INT 事件模式 |
| GUI | LVGL 9.2.x，RGB888，libc malloc（落 PSRAM） |
| 音频 | 板端不播放音频；网易云在电脑上播放，板端只显示歌词和远控 |
| 时间戳来源 | 电脑端代理用网易云窗口标题 + SMTC 播放状态 + 本地时钟推算进度 |
| 歌词格式 | 标准 LRC `[mm:ss.xx]`；代理可附带译文，用 `0x1F` 分隔，本界面单行只显示原文 |

说明：参考工程的 ES8389 只是 codec，用于钢琴发声，不能直接播放网易云。
本方案不实现板端音频解码；它只做“显示 + 遥控”，这样不需要 ESP-ADF、网络流媒体
和解码器，和电脑端的网易云保持同一播放时钟。

## 2. 功能与界面

- 左侧：320px 黑胶唱片 + 240px 当前歌曲封面 + 白色唱针；上一首/播放/下一首三个圆形按钮
  移到黑胶下方，播放键 64px 白色、两侧 52px 灰色。
- 右侧：4 行歌词，第 2 行为当前句，第 1 行为上一句并淡化到 `#4A4A4A`；当前句白色 `#FFFFFF`，
  后面的句子灰色 `#8A8A8A`；字号保持 44px，行高 88px，文字在右侧区域水平居中。
- 长句按像素宽度预切分：单行最大约 650px；44px 字体下
  `Trouble is a friend yeah trouble is` 约 631px，正好在这一句后换行。换行后的歌词
  占两个显示行，滚动时按实际占用行数（1–3 行）上移。
- 第一句歌词之前：第 2 行显示“歌名 - 歌手”（来自代理 /state），第一句开始后随滚动淡出；
  没有元信息时退回 `...`。
- 右下角：留空（已移除音量按钮）。
- 背景：从当前封面提取偏饱和主色，压暗后作为上下渐变（每首歌背景不同）；
  没有封面时使用默认深色渐变。参考 `wyy_desk.png`；不使用 emoji，日志也不带 emoji。

## 3. 目录结构

```text
esp32-p4-c5-zxxx/
├─ CMakeLists.txt                 # ESP-IDF 工程入口
├─ partitions.csv                 # 16MB flash，factory 10MB（字体 + 应用）
├─ sdkconfig.defaults             # P4X/C5/PSRAM/LVGL 默认配置
├─ main/
│  ├─ main.c                      # NVS -> 屏幕 -> LVGL -> C5/WiFi 启动流程
│  ├─ axs15260_lcd.c/.h           # AXS15260 MIPI-DSI 面板与背光初始化
│  ├─ lvgl_port.c/.h              # 1280x452、90 度旋转、触摸、LVGL 任务
│  ├─ Kconfig.projbuild           # WiFi SSID/密码
│  └─ idf_component.yml           # esp_hosted / esp_wifi_remote / lvgl / cjson
├─ components/
│  ├─ esp_lcd_axs15260/           # 面板 + 触摸驱动（复用已验证组件，Apache-2.0）
│  └─ music_lyrics/
│     ├─ include/
│     │  ├─ lyrics_ui.h           # 页面接口
│     │  ├─ lyrics_scroll.h       # 滚动控制器接口
│     │  ├─ music_lyric.h         # LRC 解析接口（纯 C，无 LVGL 依赖）
│     │  ├─ music_state.h         # 播放状态 + 演示模式
│     │  └─ music_remote.h        # 局域网远控接口
│     ├─ lyrics_ui.c              # 页面、按钮、图标、定时器
│     ├─ lyrics_scroll.c          # 当前句索引、4 行窗口、滚动动画
│     ├─ music_lyric.c            # LRC 解析（纯 C）
│     ├─ music_state.c            # 状态快照 + 内置演示歌单/LRC
│     ├─ music_remote.c           # UDP 发现 + HTTP /state /lyrics /cmd
│     └─ fonts/README.md          # 字体生成说明（34MB 字体不进仓库）
├─ tools/
│  ├─ gen_font.py                 # 生成 44px CJK 字体
│  └─ music_agent/                # Windows 网易云代理（MIT）
│     ├─ music_agent.py
│     ├─ requirements.txt
│     ├─ run_agent.bat
│     └─ README.md
├─ docs/                          # 设计图和实机参考图
└─ README.md
```

模块职责与需求逐项对应：

| 需求 | 文件 |
|---|---|
| 屏幕/驱动初始化 | `main/axs15260_lcd.c`、`components/esp_lcd_axs15260` |
| LVGL 显示与触摸接入 | `main/lvgl_port.c` |
| UI 构建 | `components/music_lyrics/lyrics_ui.c` |
| 歌词解析（与 UI 解耦） | `components/music_lyrics/music_lyric.c` |
| 滚动控制 | `components/music_lyrics/lyrics_scroll.c` |
| 按键事件 | `components/music_lyrics/lyrics_ui.c` 的按钮回调 |
| 数据源 | `components/music_lyrics/music_remote.c`、`music_state.c` |
| 电脑端网易云联动 | `tools/music_agent/music_agent.py` |

## 4. 编译与烧录

### 4.1 环境

- ESP-IDF v6.1（本工程在该版本完整编译通过；v5.5.3+ 的 API 兼容性需自行验证）；
- 目标芯片 `esp32p4`；
- 16MB flash。

```bash
cd esp32-p4-c5-zxxx
python tools/gen_font.py          # 首次克隆后生成 44px CJK 字体
idf.py set-target esp32p4
idf.py menuconfig
```

在 `歌词显示屏（ESP32-P4C5 + AXS15260）` 菜单里填写：

- `WiFi SSID`：电脑和板子所在的同一个 2.4GHz/5GHz 局域网；
- `WiFi 密码`。

C5 需要预先烧录 ESP-Hosted-MCU 2.12.9 固件；本仓库不包含 C5 固件和首刷工具。
如果 C5 是空片，可先使用参考工程 `aibox-p4xc5` 的 C5 首刷流程烧录一次。

编译并烧录：

```bash
idf.py build
idf.py -p COMx flash monitor
```

如果使用本机已经装好的 IDF v6.1，注意 Git Bash 下 `export.sh` 会拒绝 MSys 环境；
请用 ESP-IDF 的 PowerShell/CMD 环境，或直接调用安装目录里的
`tools\python\v6.1\venv\Scripts\python.exe <IDF_PATH>\tools\idf.py`。

### 4.2 分区与 Flash

`partitions.csv`：

```text
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  10M,
```

- 生成的 CJK 字体源文件约 34MB，编译进 flash 的未压缩位图数据约 5.6MB；
  仓库不提交字体文件，首次构建前运行 `python tools/gen_font.py`；
- 16MB flash 的 factory 分区为 10MB；如果换成 4MB flash 的板子，需要把字体改小
  （例如 28px/2bpp 或只保留常用 3500 字）。

### 4.3 首次启动日志

正常应看到：

```text
p4c5_lyrics: ESP32-P4C5 歌词显示屏启动
axs15260_lcd: AXS15260 MIPI LCD 初始化完成（452x1280, RGB888）
lvgl_port: AXS15260 触摸已按 INT 事件模式接入
lvgl_port: 歌词界面已启动：1280x452（面板 452x1280，软件旋转 90 度）
music_remote: 正在监听电脑端代理广播（UDP 9878）
p4c5_lyrics: 正在连接 WiFi：<SSID>
p4c5_lyrics: WiFi 已连接，IP：<ip>
music_remote: 已发现电脑端代理 <ip>:8777
music_remote: 歌词已就绪：<n> 字节
```

没配置 WiFi 或没启动代理时，界面会进入演示模式，用内置歌单和 LRC 正常滚动。

## 5. 电脑端代理

```powershell
cd tools\music_agent
pip install -r requirements.txt
python music_agent.py
```

也可以直接双击 `tools/music_agent/run_agent.bat`，它会打开一个可见的 CMD 窗口并运行代理；
关闭窗口或按 Ctrl+C 即可停止，不需要后台进程。

要求：

- Windows 10/11，网易云音乐 Windows 客户端已登录并正在运行；
- 电脑和板子在同一个局域网；
- 首次运行允许 Windows 防火墙放行 UDP 9878 和 TCP 8777；
- 代理启动后会向 `255.255.255.255:9878` 每 2 秒广播 `MUSICAGENT|<ip>|<port>`。

代理的详细行为、SMTC/窗口标题的坑和接口说明见
`tools/music_agent/README.md`。

## 6. 数据链路与协议

```text
Windows 电脑
  网易云音乐客户端
    ├─ 窗口标题 -> 歌名/歌手
    ├─ SMTC -> 播放状态
    └─ 本地时钟 -> 播放进度
  music_agent.py（HTTP 8777 + UDP 9878 广播）
        │  GET /state      每秒
        │  GET /lyrics?h=  换歌时一次
        │  POST /cmd       按钮
        ▼
ESP32-P4C5
  music_remote（独立任务）
    ├─ UDP 发现代理，8 秒无广播转入演示模式
    ├─ /state -> music_state_t 快照
    └─ /lyrics -> 16KB PSRAM 缓冲 + music_lyric 解析
  lyrics_ui（LVGL 任务）
    ├─ 40ms 读快照/进度，250ms 与代理校准一次
    ├─ 当前句索引 -> lyrics_scroll
    └─ 按钮 -> music_remote_send_cmd
```

`/state` 关键字段：`track`、`playing`、`position`、`duration`、`title`、`artist`、
`album`、`app`、`art_hash`、`lyric`。板端用 `art_hash` 作为曲目哈希；如果没有该字段，
退化为 `title|artist|duration`。

## 7. 歌词滚动算法

### 7.1 数据与索引

- `music_lyric_parse()` 把 LRC 原地切成 `(time_ms, text)` 数组，按时间升序，
  重复时间戳只保留第一条；支持 `[mm:ss]`、`[mm:ss.x]`、`[mm:ss.xxx]`、
  `[mm:ss:xx]`，识别一行多个时间标签。
- `music_lyric_index_at()` 二分查找最后一个 `time_ms <= position_ms` 的行；
  早于第一行返回 -1，晚于最后一行返回最后一行。
- 界面每 40ms 调一次 `lyrics_scroll_update(display_position_ms())`。
  `display_position_ms()` 在暂停时返回冻结位置，播放时按 `lv_tick_get()` 外推。
- 快照每 250ms 拉一次；播放中偏差超过 1.2 秒才重新校准，避免每秒一跳。

### 7.2 4 行显示窗口 + 可变行高动画

`lyrics_scroll` 维护一个“显示行队列”：每句歌词先按像素宽度预切成 1–3 个显示行
（`lv_text_get_width()`，最大约 650px），歌名/歌手作为第 -1 句放在队首。
视口固定显示 4 个显示行，当前句的第一个显示行固定在**第 2 行**：

1. 当前句可能占 1–3 个显示行（换行歌词），这些显示行全部白色；
2. 上一句的最后一个显示行在第 1 行，颜色淡化；后面的句子灰色；
3. 换句时用 `lv_anim` 把 `track` 向上移动 `当前句显示行数 × row_h`
   （320ms + 每多一行 100ms，ease-out）；
4. 动画结束后按新当前句重建 7 个槽位的窗口，`track.y` 归零。

因此一句歌词如果换行占了两行，切到下一句时会一次上移两行，下一句仍回到第 2 行。
只有索引变化才改文字；动画只移动一个容器，不逐帧改文本。

### 7.3 边界情况

| 情况 | 处理 |
|---|---|
| 第一句之前 | 第 2 行显示“歌名 - 歌手”（无元信息时 `...`），第一句在下方待入场；到时间后滚入并高亮 |
| 最后一句之后 | 最后一句保持居中高亮，不再滚动 |
| 歌词行数少于 4 | 可用显示行垂直排列，空行隐藏，不滚动 |
| 无歌词 | 显示歌名/歌手 + “暂无歌词” |
| 没发现代理 | 显示“等待电脑端代理 / 请启动电脑端 music_agent.py” |
| 代理在线但没播放 | 显示“暂无播放 / 请在网易云音乐中播放歌曲” |
| 切歌 | 立即清空旧歌词，等远控发布新歌词版本后重建；不会把上一首歌词短暂显示到新歌 |
| 暂停 | 代理用 Windows 音频会话峰值判断真实播放状态；暂停后位置冻结，索引不再变化 |
| 拖动进度/跳句 | 索引跨多行时直接重建窗口，不做长链动画 |

## 8. 内存与性能

- 显示：1280x452 RGB888。LVGL 绘制缓冲 1,735,680 字节 + 旋转暂存
  1,735,680 字节，共约 3.3MB PSRAM。
- 歌词：16KB LRC 文本 + 400 行索引 + 显示行队列（最多 1600 行，约 20KB 文本/15KB 索引）在 PSRAM；
- 封面：240x240 RGB565 双缓冲 230KB + LVGL 显示缓冲 115KB，在 PSRAM。
- 字体：44px、4bpp、未压缩位图、6763 个 GB2312 汉字 + ASCII/假名/标点，编译进 flash，
  不占 RAM；只对当前实际出现的字形做位图缓存。
- 刷新：LVGL `LV_DEF_REFR_PERIOD=10ms`，歌词索引 40ms 一次，动画 300ms；
  背景、按钮、胶囊都是静态对象，只有歌词容器移动时局部重绘。
- flush：只旋转脏矩形，不整屏重绘。LVGL v9 partial 模式的 flush 缓冲按
  脏区宽度排布，所以 `src_stride = 脏区宽度 * 3`；这一点在
  `main/lvgl_port.c` 里有注释，和参考工程的整屏 stride 不同。
- 触摸：INT 事件模式，只有中断到来才读 I2C，避免 AXS15260 空闲读寄存器
  导致内部指针漫游；LVGL 输入回调只读缓存坐标。
- 任务栈：LVGL 16KB，远控 8KB（PSRAM），触摸 4KB。

如果后续要省 PSRAM，可把绘制缓冲改成 1/4 屏局部缓冲（约 0.87MB 总计），
`flush_cb` 的脏区 stride 逻辑已经按脏区宽度写，不需要改旋转算法。

## 9. LVGL 版本与关键 API

本工程按 LVGL 9.2 编写。v8/v9 的主要差异：

| 功能 | v9（本工程） | v8 |
|---|---|---|
| 显示 | `lv_display_create`、`lv_display_set_buffers`、`lv_display_set_flush_cb` | `lv_disp_drv_init/register` |
| 输入 | `lv_indev_create`、`lv_indev_set_read_cb` | `lv_indev_drv_init/register` |
| 屏幕加载 | `lv_screen_load` | `lv_scr_load` |
| 移除标志 | `lv_obj_remove_flag` | `lv_obj_clear_flag` |
| 图片旋转 | `lv_image_set_rotation` | `lv_img_set_angle` |
| 标签长模式 | `LV_LABEL_LONG_WRAP` | 同名，v9.5 改为 `LV_LABEL_LONG_MODE_WRAP` |
| 动画 | `lv_anim_set_duration`、`lv_anim_set_values`、`lv_anim_start` | 同名，duration 旧名 `lv_anim_set_time` |

如果必须用 LVGL v8，需要把 `lyrics_ui.c` / `lyrics_scroll.c` 的上述 API
换回 v8 名称，`main/lvgl_port.c` 的显示/输入注册也要改；LRC 解析和远控逻辑
不依赖 LVGL，可以原样复用。

## 10. 字体

`components/music_lyrics/fonts/lyrics_font_44.c` 由 `lv_font_conv` 生成：

- 源字体：Windows `Deng.ttf`（等线），44px，4bpp，未压缩位图（--no-compress）；
- 覆盖：ASCII、Latin-1、GB2312 一二级 6763 字、日文平假名/片假名、
  CJK 标点、全角字符；
- 生成命令（在装有 Node.js 的机器上）：

```bash
npx lv_font_conv --font C:/Windows/Fonts/Deng.ttf \
  --size 44 --bpp 4 --format lvgl --lv-font-name lyrics_font_44 --no-compress \
  --range 0x20-0x7F --range 0xA0-0xFF --range 0x3000-0x303F \
  --range 0x3040-0x309F --range 0x30A0-0x30FF --range 0xFF00-0xFFEF \
  --symbols "<GB2312 汉字字符串>" \
  -o lyrics_font_44.c
```

- 字体需要 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`（6763 字超过普通描述符范围）。
  当前字体用 `--no-compress` 生成，不依赖 `LV_USE_FONT_COMPRESSED`；工程里仍保留该选项以兼容其他字体。

## 11. 已知限制

- 播放进度由电脑端代理用本地时钟推算；在电脑上拖动进度条时，板端不会立即跟随，
  偏差超过 1.2 秒才重新校准。
- 曲目识别依赖网易云窗口标题；网易云完全退出时没有曲目。
- 代理的切歌/暂停使用 Windows 系统媒体键（`VK_MEDIA_*`），不模拟点击网易云窗口。
  播放状态优先用 Windows 音频会话峰值（`pycaw`）判断，直接在网易云界面里暂停/恢复也会同步到板端；
  读不到音频会话时才退回最近一次命令的假定状态。
- 系统媒体键不需要管理员权限；如果网易云以管理员身份运行而代理不是，Windows UIPI
  可能拦截合成输入，可让两者权限一致。
- 板端不播放音频；如果后续要在板上出声，需要增加 ESP-ADF/解码器和网络流，
  这是另一个量级的工程。
- C5 必须预先烧录 ESP-Hosted-MCU 2.12.9 固件；仓库不提交 C5 四段固件，也不包含
  SDIO 首刷工具。参考工程 `aibox-p4xc5` 里有完整的 C5 首刷实现。
- 本工程按 ESP32-P4X rev v3.x + 1-bit SDIO 的 P4X 板配置；旧 v1.x 板或 4-bit
  SDIO 板需要改 `sdkconfig.defaults` 里的 `CONFIG_ESP32P4_*` 和 Hosted 引脚/位宽。

## 12. 许可与致谢

- 本工程代码：按项目需要自由使用。
- `tools/music_agent/music_agent.py` 来源于
  `esp32-p4-music-page`（MIT，Copyright (c) 2026 exder145），保留原 LICENSE。
- AXS15260 驱动组件来自参考工程，Apache-2.0。
- 字体由 Microsoft DengXian 生成，仅用于本项目显示；分发字体位图前请确认
  字体授权。
"# esp32-p4-c5-zxxx" 
