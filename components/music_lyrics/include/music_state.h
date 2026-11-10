// -*- coding: utf-8 -*-
// 播放状态快照与演示模式。
// 本文件不依赖 ESP-IDF 与 LVGL，可在宿主机上单独编译测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_TEXT_MAX 96
#define MUSIC_LYRIC_MAX 192
#define MUSIC_HASH_MAX 208

// 播放状态快照：music_remote 任务写入，界面任务读取。
typedef struct {
    bool agent_found;      // 已发现电脑端代理且未超时
    bool track_loaded;     // 代理上有正在使用的播放会话
    bool playing;
    bool demo;             // true = 未发现代理，显示板内演示数据
    uint32_t position_ms;
    uint32_t duration_ms;
    char title[MUSIC_TEXT_MAX];
    char artist[MUSIC_TEXT_MAX];
    char album[MUSIC_TEXT_MAX];
    char app[32];          // 播放来源应用名，演示模式为"演示模式"
    char track_hash[MUSIC_HASH_MAX];  // 曲目哈希，换歌时变化
    char lyric[MUSIC_LYRIC_MAX];      // 当前句歌词（代理提供，界面可选使用）
} music_state_t;

// 时间格式化为 mm:ss。
void music_state_format_time(uint32_t ms, char *out, int out_len);

// 进度百分比 0..100。
int music_state_progress_percent(uint32_t position_ms, uint32_t duration_ms);

// ── 演示模式 ─────────────────────────────────────────────────────
// 未发现电脑端代理时，用内置假歌单和内置 LRC 驱动界面滚动，保证单独亮屏可用。

// 按 delta_ms 推进假进度；放完自动切下一首。输出填入 state。
void music_state_demo_advance(music_state_t *state, int delta_ms);

// 演示播放/暂停切换。
void music_state_demo_toggle(music_state_t *state);

// 演示切歌：dir = +1 下一首，-1 上一首。
void music_state_demo_skip(music_state_t *state, int dir);

// 当前演示曲目的完整 LRC 文本（静态字符串，不需要释放）。
const char *music_state_demo_lrc(void);

#ifdef __cplusplus
}
#endif
