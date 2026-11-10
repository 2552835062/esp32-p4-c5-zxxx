// -*- coding: utf-8 -*-
// ESP32-P4C5 歌词显示屏启动流程：
//   NVS -> 网络栈 -> AXS15260 面板 -> LVGL 歌词页 -> C5 Hosted WiFi -> 电脑端代理
//
// 屏幕和歌词页先启动：即使 WiFi 没配置或 C5 没起来，界面也会用内置演示歌单
// 正常滚动，方便单独验收屏幕。WiFi 就绪后远控任务会自动发现电脑端代理。
#include <string.h>

#include "sdkconfig.h"

#include "axs15260_lcd.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "music_remote.h"
#include "nvs_flash.h"

static const char *TAG = "p4c5_lyrics";
static esp_timer_handle_t s_reconnect_timer;

static esp_err_t init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重新初始化：%s", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "擦除 NVS 失败");
        err = nvs_flash_init();
    }
    return err;
}

static void reconnect_timer_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "重新连接 WiFi");
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 已断开，3 秒后重连");
        if (s_reconnect_timer != NULL) {
            esp_timer_start_once(s_reconnect_timer, 3000000);
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    if (event != NULL) {
        ESP_LOGI(TAG, "WiFi 已连接，IP：" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void start_wifi(void) {
    if (CONFIG_P4_WIFI_STA_SSID[0] == '\0') {
        ESP_LOGI(TAG, "未配置 WiFi SSID，歌词页以演示模式运行");
        return;
    }

    // C5 需要预先烧录 ESP-Hosted-MCU 2.12.9；本仓库不包含 C5 固件和首刷工具。
    esp_err_t err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted 初始化失败：%s；歌词页继续以演示模式运行",
                 esp_err_to_name(err));
        return;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "创建默认 WiFi STA 网络接口失败");
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_remote_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败：%s", esp_err_to_name(err));
        return;
    }
    // WIFI_EVENT 事件基在 esp_wifi_init 之后才存在，所以断线重连处理器在这里注册
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL, NULL));
    err = esp_wifi_remote_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 WiFi STA 模式失败：%s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_remote_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 WiFi 失败：%s", esp_err_to_name(err));
        return;
    }
    music_remote_set_wifi_ready(true);

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, CONFIG_P4_WIFI_STA_SSID,
            sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, CONFIG_P4_WIFI_STA_PASSWORD,
            sizeof(sta_cfg.sta.password));

    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 WiFi 配置失败：%s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "正在连接 WiFi：%s", CONFIG_P4_WIFI_STA_SSID);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "发起 WiFi 连接失败：%s", esp_err_to_name(err));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "ESP32-P4C5 歌词显示屏启动");

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    // 1. 屏幕：AXS15260 452x1280 MIPI-DSI，DPI 面板 + 背光
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t err = axs15260_lcd_init(&panel, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "屏幕初始化失败：%s", esp_err_to_name(err));
        return;
    }
    axs15260_lcd_set_backlight(true);

    // 2. LVGL 歌词页（先于 WiFi 启动，保证没有网络也能亮屏滚动）
    err = lvgl_port_start(panel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL 歌词页启动失败：%s", esp_err_to_name(err));
        return;
    }

    // 3. C5 Hosted WiFi + 电脑端代理
    start_wifi();

    ESP_LOGI(TAG, "启动流程完成。歌词页演示模式/代理模式会自动切换。");
}
