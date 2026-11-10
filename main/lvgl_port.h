// -*- coding: utf-8 -*-
// 1280x452 LVGL 移植层：AXS15260 竖屏面板 + 90 度软件旋转 + 触摸 + LVGL 任务。
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动歌词界面。须在 axs15260_lcd_init() 成功后调用。
// touch_i2c 传 NULL 时按板级固定引脚（SDA=IO28 / SCL=IO29）自建 I2C 总线。
esp_err_t lvgl_port_start(esp_lcd_panel_handle_t panel,
                          i2c_master_bus_handle_t touch_i2c);

#ifdef __cplusplus
}
#endif
