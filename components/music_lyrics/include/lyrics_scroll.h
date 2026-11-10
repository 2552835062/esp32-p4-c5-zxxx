// -*- coding: utf-8 -*-
// 歌词滚动控制器：3 行可视、当前句固定居中、换句时整块平滑上/下移一行。
// 只依赖 LVGL 与 music_lyric，不关心数据来自网络还是演示模式。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "music_lyric.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lyrics_scroll lyrics_scroll_t;

// 在 parent 上创建歌词视口：root_x/root_w 是右侧歌词区的位置和宽度，
// center_y 是当前句（4 行中的第 2 行）中心在 parent 坐标系里的 y，
// max_text_w 是单行歌词允许的最大像素宽度（超过则预切成多个显示行）。
lyrics_scroll_t *lyrics_scroll_create(lv_obj_t *parent, const lv_font_t *font,
                                      int row_h, int center_y, int root_x,
                                      int root_w, int max_text_w);

// 设置整首歌词；position_ms 用于决定初始高亮行。lyr 由调用方持有，
// 在下次调用 set_lyrics 前必须保持有效。lyr == NULL 或 count == 0 时显示空状态。
void lyrics_scroll_set_lyrics(lyrics_scroll_t *sc, const music_lyric_t *lyr,
                              uint32_t position_ms);

// 更新空状态文案（无歌词时显示歌名/歌手、暂无播放或等待代理）。
void lyrics_scroll_set_meta(lyrics_scroll_t *sc, const char *title,
                            const char *artist, bool agent_found, bool has_track);

// 按播放进度推进；暂停时调用方应传同一个冻结位置，界面就不会滚动。
void lyrics_scroll_update(lyrics_scroll_t *sc, uint32_t position_ms);

// 释放控制器（LVGL 对象由父对象统一删除，这里只释放本结构体）。
void lyrics_scroll_destroy(lyrics_scroll_t *sc);

#ifdef __cplusplus
}
#endif
