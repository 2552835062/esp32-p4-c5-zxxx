// -*- coding: utf-8 -*-
// 1280x452 LVGL 移植层实现。
//
// 面板原生 452x1280 竖屏，逻辑界面 1280x452 横屏。flush 时只把脏矩形
// 逆时针旋转 90 度写入面板，不整屏重绘：
//   逻辑 (x,y) -> 面板 (x'=y, y'=1279-x)
// LVGL v9 partial 模式下，flush 缓冲是"按脏区宽度排布"的，因此源步长必须用
// 脏区宽度而不是屏幕宽度；否则局部刷新会出现斜切花屏。
//
// 触摸使用 INT 事件模式：只有中断到来才读 I2C，避免 AXS15260 空闲读寄存器
// 导致内部指针漫游；LVGL 输入回调只读取缓存坐标。
#include "lvgl_port.h"

#include <string.h>

#include "axs15260_lcd.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_axs15260.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lyrics_ui.h"
#include "lvgl.h"

static const char *TAG = "lvgl_port";

// AXS15260 物理分辨率与逻辑分辨率
#define PANEL_H_RES 452
#define PANEL_V_RES 1280
#define PANEL_BYTES_PER_PX 3

#define LOGIC_W PANEL_V_RES   // 横屏宽 1280
#define LOGIC_H PANEL_H_RES   // 横屏高 452
#define LOGIC_BUF_BYTES ((size_t)LOGIC_W * LOGIC_H * PANEL_BYTES_PER_PX)

#define MIPI_TOUCH_I2C_PORT I2C_NUM_1
#define TOUCH_SDA_GPIO 28
#define TOUCH_SCL_GPIO 29
#define TOUCH_INT_GPIO 27

static esp_lcd_panel_handle_t s_panel;
static axs15260_touch_handle_t s_touch;
static uint8_t *s_draw_buf;
static uint8_t *s_rot_buf;

// 触摸缓存：中断任务写，LVGL 输入回调读
typedef struct {
    lv_coord_t x;
    lv_coord_t y;
    bool pressed;
} touch_snapshot_t;

static touch_snapshot_t s_pt;
static portMUX_TYPE s_pt_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_touch_task;

static uint32_t lv_tick_get_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int w = (int)(area->x2 - area->x1 + 1);
    const int h = (int)(area->y2 - area->y1 + 1);
    if (w <= 0 || h <= 0) {
        lv_display_flush_ready(disp);
        return;
    }
    // LVGL v9 partial 模式的缓冲按脏区宽度排布，且行首可能对齐到 4 字节；
    // 必须用 LVGL 的实际 stride，不能直接用 w * 3，否则奇数宽度会斜切。
    const size_t src_stride = lv_draw_buf_width_to_stride((uint32_t)w,
                                                          LV_COLOR_FORMAT_RGB888);

    // 逐像素逆时针旋转 90 度，旋转后缓冲宽 h、高 w
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = px_map + (size_t)y * src_stride;
        for (int x = 0; x < w; ++x) {
            uint8_t *dst = s_rot_buf + ((size_t)(w - 1 - x) * h + y) * PANEL_BYTES_PER_PX;
            dst[0] = row[x * PANEL_BYTES_PER_PX + 0];
            dst[1] = row[x * PANEL_BYTES_PER_PX + 1];
            dst[2] = row[x * PANEL_BYTES_PER_PX + 2];
        }
    }

    // 脏区映射到面板坐标；IDF 的 x_end/y_end 为开区间
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->y1, (PANEL_V_RES - 1) - area->x2,
                              area->y2 + 1, PANEL_V_RES - area->x1,
                              s_rot_buf);
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    touch_snapshot_t pt;
    portENTER_CRITICAL(&s_pt_lock);
    pt = s_pt;
    portEXIT_CRITICAL(&s_pt_lock);
    data->point.x = pt.x;
    data->point.y = pt.y;
    data->state = pt.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void IRAM_ATTR touch_isr_cb(axs15260_touch_handle_t handle, void *user_data) {
    (void)handle;
    (void)user_data;
    if (s_touch_task != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_touch_task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void touch_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "触摸中断任务已启动");
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        do {
            axs15260_touch_data_t raw;
            memset(&raw, 0, sizeof(raw));
            if (axs15260_touch_read(s_touch, &raw) != ESP_OK) {
                continue;
            }
            touch_snapshot_t pt;
            portENTER_CRITICAL(&s_pt_lock);
            pt = s_pt;
            portEXIT_CRITICAL(&s_pt_lock);

            if (raw.point_num > 0) {
                const axs15260_touch_point_t *p = &raw.points[0];
                // 面板物理坐标 -> 逻辑横屏坐标
                pt.x = (lv_coord_t)((PANEL_V_RES - 1) - p->y);
                pt.y = (lv_coord_t)p->x;
                pt.pressed = (p->event != AXS15260_TOUCH_EVT_UP);
            } else {
                pt.pressed = false;
            }

            portENTER_CRITICAL(&s_pt_lock);
            s_pt = pt;
            portEXIT_CRITICAL(&s_pt_lock);
        } while (ulTaskNotifyTake(pdFALSE, 0) > 0);
    }
}

static void touch_init(i2c_master_bus_handle_t touch_i2c) {
    if (touch_i2c == NULL) {
        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port = MIPI_TOUCH_I2C_PORT,
            .sda_io_num = (gpio_num_t)TOUCH_SDA_GPIO,
            .scl_io_num = (gpio_num_t)TOUCH_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&bus_cfg, &touch_i2c) != ESP_OK) {
            ESP_LOGW(TAG, "触摸 I2C 总线创建失败，界面将以无触摸运行");
            return;
        }
    }

    const axs15260_touch_config_t cfg = {
        .i2c_bus = touch_i2c,
        .i2c_sda = GPIO_NUM_NC,
        .i2c_scl = GPIO_NUM_NC,
        // 触摸复位与 LCD 共用 GPIO26，面板初始化已经复位过，这里绝不能再脉冲
        .rst_gpio = GPIO_NUM_NC,
        .int_gpio = (gpio_num_t)TOUCH_INT_GPIO,
        .i2c_port = MIPI_TOUCH_I2C_PORT,
        .i2c_freq_hz = AXS15260_TOUCH_I2C_FREQ_HZ,
        .x_max = AXS15260_TOUCH_H_RES,
        .y_max = AXS15260_TOUCH_V_RES,
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    if (axs15260_touch_new(&cfg, &s_touch) != ESP_OK) {
        s_touch = NULL;
        ESP_LOGW(TAG, "触摸驱动创建失败，界面将以无触摸运行");
        return;
    }

    if (xTaskCreatePinnedToCore(touch_task, "lyrics_touch", 4096, NULL, 5,
                                &s_touch_task, 1) != pdPASS) {
        ESP_LOGW(TAG, "触摸任务创建失败");
        axs15260_touch_del(s_touch);
        s_touch = NULL;
        return;
    }

    if (axs15260_touch_register_cb(s_touch, touch_isr_cb, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "触摸中断回调注册失败");
    }
    ESP_LOGI(TAG, "AXS15260 触摸已按 INT 事件模式接入");
}

static void lv_task(void *arg) {
    (void)arg;

    lv_init();
    lv_tick_set_cb(lv_tick_get_ms);

    lv_display_t *disp = lv_display_create(LOGIC_W, LOGIC_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_draw_buf, NULL, (uint32_t)LOGIC_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);

    lv_obj_t *screen = lv_obj_create(NULL);
    lyrics_page_create(screen);
    lv_screen_load(screen);
    lyrics_page_set_visible(true);

    ESP_LOGI(TAG, "歌词界面已启动：%dx%d（面板 %dx%d，软件旋转 90 度）",
             LOGIC_W, LOGIC_H, PANEL_H_RES, PANEL_V_RES);

    while (true) {
        lv_timer_handler_run_in_period(5);
    }
}

esp_err_t lvgl_port_start(esp_lcd_panel_handle_t panel,
                          i2c_master_bus_handle_t touch_i2c) {
    ESP_RETURN_ON_FALSE(panel != NULL, ESP_ERR_INVALID_ARG, TAG, "面板句柄为空");
    s_panel = panel;

    s_draw_buf = heap_caps_aligned_alloc(16, LOGIC_BUF_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rot_buf = heap_caps_aligned_alloc(16, LOGIC_BUF_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_draw_buf == NULL || s_rot_buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 帧缓冲分配失败：每块 %u 字节",
                 (unsigned)LOGIC_BUF_BYTES);
        heap_caps_free(s_draw_buf);
        heap_caps_free(s_rot_buf);
        s_draw_buf = s_rot_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    touch_init(touch_i2c);

    if (xTaskCreatePinnedToCore(lv_task, "lyrics_lvgl", 16384, NULL, 5,
                                NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "LVGL 任务创建失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}
