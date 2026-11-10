set(srcs
    "main.c"
    "axs15260_lcd.c"
    "lvgl_port.c"
)

set(priv_requires
    esp_lcd
    esp_lcd_axs15260
    esp_mm
    esp_psram
    esp_hw_support
    esp_driver_gpio
    esp_driver_i2c
    esp_timer
    esp_netif
    esp_event
    esp_wifi
    esp_wifi_remote
    esp_hosted
    nvs_flash
    heap
    lvgl
    music_lyrics
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${priv_requires}
)
