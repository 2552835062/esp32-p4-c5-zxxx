// -*- coding: utf-8 -*-
// 歌词页：按 UI/MUSIC_UI.png 的 1280x452 布局构建。
// 所有函数必须在 LVGL 任务上下文调用。
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 在给定 screen（lv_obj_create(NULL) 创建的独立屏幕）上构建歌词页。
// 内部缓冲和远控任务在首次调用时创建，之后常驻。
void lyrics_page_create(lv_obj_t *screen);

// 页面显隐：隐藏时暂停歌词定时器并停止远控任务，恢复时重新拉起。
void lyrics_page_set_visible(bool visible);

#ifdef __cplusplus
}
#endif
