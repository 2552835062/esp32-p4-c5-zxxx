// -*- coding: utf-8 -*-
// MIPI-DSI 面板 framebuffer 数量。
// 歌词界面只需要 DPI 的 2 个帧缓冲；LVGL 自己的绘制缓冲在 lvgl_port.c 里分配。
#pragma once

#define PANEL_FRAMEBUFFERS 2
