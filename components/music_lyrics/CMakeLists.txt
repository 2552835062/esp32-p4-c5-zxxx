# music_lyrics：歌词解析 + 滚动控制 + 网易云远控 + 1280x452 歌词页。
# 字体 lyrics_font_44.c 由 lv_font_conv 从 Deng.ttf 生成（GB2312 6763 字 + ASCII/
# 假名/标点），编译进 flash，不占 RAM。字体约 34MB，不在 Git 仓库中；
# 首次克隆后先运行 python tools/gen_font.py。
set(LYRIC_FONT_FILE "${CMAKE_CURRENT_LIST_DIR}/fonts/lyrics_font_44.c")
if(NOT EXISTS "${LYRIC_FONT_FILE}")
    message(FATAL_ERROR
        "缺少歌词字体：${LYRIC_FONT_FILE}\n"
        "请先在工程根目录运行：python tools/gen_font.py")
endif()

# Wi-Fi 头文件来源：ESP32-P4 无射频，走 esp_wifi_remote shim；其余芯片用 esp_wifi。
if(CONFIG_IDF_TARGET_ESP32P4)
    set(MUSIC_WIFI_REQUIRES esp_wifi_remote)
else()
    set(MUSIC_WIFI_REQUIRES esp_wifi)
endif()

idf_component_register(
    SRCS "music_lyric.c"
         "music_state.c"
         "music_remote.c"
         "lyrics_scroll.c"
         "lyrics_ui.c"
         "fonts/lyrics_font_44.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
             espressif__cjson
             esp_http_client
             lwip
             esp_timer
             heap
             ${MUSIC_WIFI_REQUIRES}
)
