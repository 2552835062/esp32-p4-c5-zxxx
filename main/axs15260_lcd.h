// -*- coding: utf-8 -*-
// AXS15260D 6.2 寸 452x1280 MIPI-DSI 屏幕驱动。
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_axs15260.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 MIPI DSI 总线、AXS15260D 面板和背光。
// 返回底层 DPI 面板句柄，供 USB 副屏和摄像头直刷路径使用。
esp_err_t axs15260_lcd_init(esp_lcd_panel_handle_t *out_panel,
                            esp_lcd_panel_io_handle_t *out_io);

// 背光开关（true=亮）。
void axs15260_lcd_set_backlight(bool on);

#ifdef __cplusplus
}
#endif
