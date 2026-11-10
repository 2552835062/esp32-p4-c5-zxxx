// -*- coding: utf-8 -*-
// 歌词页实现（UI_new 布局）：
//   左侧：黑胶唱片 + 当前歌曲封面 + 唱针，三个控制按钮在黑胶下方；
//   右侧：4 行歌词，第 2 行为当前句，第 1 行淡化，超长句按像素宽度换行。
// 视觉参考：docs/UI_new.png、docs/wyy_desk.png、docs/demo.png。
// 所有 LVGL 调用都在 LVGL 任务上下文（lvgl_port 的 lv_task）执行。
#include "lyrics_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lyrics_scroll.h"
#include "music_lyric.h"
#include "music_remote.h"
#include "music_state.h"

#include "lvgl.h"

LV_FONT_DECLARE(lyrics_font_44);

static const char *TAG = "lyrics_ui";

// ── 布局常量（1280x452） ─────────────────────────────────────────
#define SCREEN_W 1280
#define SCREEN_H 452

// 右侧歌词区
#define LYRIC_X 420
#define LYRIC_W 840
#define LYRIC_ROW_H 88
#define LYRIC_CURRENT_CY 162
#define LYRIC_TEXT_W 650

// 左侧黑胶区
#define VINYL_CX 210
#define VINYL_CY 180
#define VINYL_D 320
#define COVER_D 240
#define VINYL_X (VINYL_CX - VINYL_D / 2)
#define VINYL_Y (VINYL_CY - VINYL_D / 2)

// 黑胶下方控制按钮
#define CTRL_CY 400
#define CTRL_LEFT_CX 140
#define CTRL_PLAY_CX 210
#define CTRL_RIGHT_CX 280
#define PLAY_D 64
#define NAV_D 52

#define COL_BG_TOP 0x1A0B2E
#define COL_BG_BOTTOM 0x05020A
#define COL_NAV 0x8B8B8B
#define COL_PLAY 0xFFFFFF
#define COL_ICON_DARK 0x090909
#define COL_VINYL 0x050505
#define COL_GROOVE 0x1A1A1A
#define COL_ARM 0xD8D8D8
#define COL_COVER_BG 0x141414

#define UI_TICK_MS 40
#define SNAP_POLL_MS 250
#define POS_DRIFT_RESYNC_MS 1200
#define TOGGLE_COOLDOWN_MS 1500

// ── 状态 ─────────────────────────────────────────────────────────
static lv_obj_t *s_screen;
static lyrics_scroll_t *s_scroll;
static lv_obj_t *s_play_label;
static lv_timer_t *s_ui_timer;
static bool s_visible;

static music_lyric_t s_lyric;
static char *s_lyric_text;
static music_lyric_line_t *s_lyric_lines;
static uint32_t s_lyric_seq = UINT32_MAX;

static music_state_t s_snap;
static uint32_t s_pos_ms;
static int64_t s_pos_sample_ms;
static bool s_playing;
static int64_t s_toggle_until;
static bool s_meta_valid;

static int64_t s_last_snap_ms;

// 封面：代理下发 240x240 RGB565，直接作为 LVGL 图像源
static uint8_t *s_art_buf;
static lv_image_dsc_t s_art_dsc;
static lv_obj_t *s_cover_img;
static lv_obj_t *s_cover_placeholder;
static uint32_t s_art_seq = UINT32_MAX;
static bool s_art_visible;
static int32_t s_cover_angle;
static lv_obj_t *s_bg_obj;

// ── 小工具 ───────────────────────────────────────────────────────
static lv_obj_t *make_plain(lv_obj_t *parent, int w, int h, int x, int y) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_circle_button(lv_obj_t *parent, int d, uint32_t color,
                                    int center_x, int center_y) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, d, d);
    lv_obj_set_pos(btn, center_x - d / 2, center_y - d / 2);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static lv_obj_t *add_icon(lv_obj_t *parent, const char *symbol,
                          const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_center(label);
    return label;
}

// ── 播放位置与图标 ───────────────────────────────────────────────
static uint32_t display_position_ms(void) {
    if (!s_playing || s_snap.duration_ms == 0) return s_pos_ms;
    int64_t elapsed = (int64_t)lv_tick_get() - s_pos_sample_ms;
    if (elapsed < 0) elapsed = 0;
    uint32_t pos = s_pos_ms + (uint32_t)elapsed;
    return pos > s_snap.duration_ms ? s_snap.duration_ms : pos;
}

static void set_playing_icon(bool playing) {
    if (s_play_label != NULL) {
        lv_label_set_text(s_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

// ── 歌词重载 ─────────────────────────────────────────────────────
// 远控任务按版本号发布歌词：换歌/解析完成都会自增。这里只在版本变化时重解析。
// 换歌时不清版本号，等远控把上一首歌词清空或新歌词写入（版本号必然变化），
// 避免把上一首的歌词短暂显示到新歌上。
static void reload_lyrics(void) {
    if (s_scroll == NULL || s_lyric_text == NULL) return;
    uint32_t seq = s_lyric_seq;
    if (!music_remote_copy_lyrics(s_lyric_text, MUSIC_LYRIC_MAX_BYTES, &seq)) {
        return;
    }
    s_lyric_seq = seq;
    if (s_lyric_text[0] == '\0') {
        music_lyric_clear(&s_lyric);
        lyrics_scroll_set_lyrics(s_scroll, NULL, 0);
        return;
    }
    int count = music_lyric_parse(&s_lyric, s_lyric_text);
    lyrics_scroll_set_lyrics(s_scroll, count > 0 ? &s_lyric : NULL,
                             display_position_ms());
    ESP_LOGI(TAG, "歌词已更新：%d 行", count);
}

static void clear_cover_visual(void);

// ── 快照应用 ─────────────────────────────────────────────────────
static void apply_snapshot(const music_state_t *snap) {
    bool track_changed = !s_meta_valid || strcmp(snap->track_hash, s_snap.track_hash) != 0;
    bool meta_changed = !s_meta_valid ||
                        strcmp(snap->title, s_snap.title) != 0 ||
                        strcmp(snap->artist, s_snap.artist) != 0 ||
                        snap->agent_found != s_snap.agent_found ||
                        snap->track_loaded != s_snap.track_loaded;

    if (track_changed) {
        s_pos_ms = snap->position_ms;
        s_pos_sample_ms = lv_tick_get();
        s_playing = snap->playing;
        s_toggle_until = 0;
        set_playing_icon(s_playing);
        // 先清空界面，等远控发布新歌词版本；不把旧歌歌词留在屏幕上
        music_lyric_clear(&s_lyric);
        if (s_scroll != NULL) lyrics_scroll_set_lyrics(s_scroll, NULL, 0);
        clear_cover_visual();
    } else {
        // 刚按过暂停/播放的 1.5 秒内不采信快照（指令还在路上）
        if ((int64_t)lv_tick_get() >= s_toggle_until && snap->playing != s_playing) {
            s_pos_ms = snap->position_ms;
            s_pos_sample_ms = lv_tick_get();
            s_playing = snap->playing;
            set_playing_icon(s_playing);
        }
        // 播放中不逐帧对齐快照（否则一秒一跳），偏差超过 1.2 秒才重新校准
        int64_t drift = (int64_t)snap->position_ms - (int64_t)display_position_ms();
        if (!s_playing || drift > POS_DRIFT_RESYNC_MS || drift < -POS_DRIFT_RESYNC_MS) {
            s_pos_ms = snap->position_ms;
            s_pos_sample_ms = lv_tick_get();
        }
    }

    if (meta_changed) {
        if (s_scroll != NULL) {
            lyrics_scroll_set_meta(s_scroll, snap->title, snap->artist,
                                   snap->agent_found, snap->track_loaded);
        }
    }

    s_snap = *snap;
    s_meta_valid = true;
    reload_lyrics();
}

// ── 封面与背景 ──────────────────────────────────────────────────
static void set_default_background(void) {
    if (s_bg_obj == NULL) return;
    lv_obj_set_style_bg_color(s_bg_obj, lv_color_hex(COL_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(s_bg_obj, lv_color_hex(COL_BG_BOTTOM), 0);
    lv_obj_invalidate(s_bg_obj);
}

// 从 240x240 封面里提取偏饱和的主色，压暗后作为上下渐变背景。
// 每首歌封面不同，背景颜色就不同；没有封面时回到默认暗色。
static void update_background_from_cover(void) {
    if (s_bg_obj == NULL || s_art_buf == NULL) return;
    const uint16_t *px = (const uint16_t *)s_art_buf;
    uint32_t sr = 0, sg = 0, sb = 0, wsum = 0;
    for (int y = 2; y < MUSIC_ART_H; y += 6) {
        for (int x = 2; x < MUSIC_ART_W; x += 6) {
            uint16_t v = px[y * MUSIC_ART_W + x];
            uint32_t r = ((v >> 11) & 0x1F) * 255 / 31;
            uint32_t g = ((v >> 5) & 0x3F) * 255 / 63;
            uint32_t b = (v & 0x1F) * 255 / 31;
            uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            uint32_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            if (mx < 20) continue;                  // 跳过近黑像素
            uint32_t sat = mx - mn;
            uint32_t w = 8 + sat;                   // 偏饱和的颜色权重更高
            sr += r * w;
            sg += g * w;
            sb += b * w;
            wsum += w;
        }
    }
    if (wsum == 0) {
        set_default_background();
        return;
    }
    uint32_t r = sr / wsum, g = sg / wsum, b = sb / wsum;
    uint32_t top = ((r * 35 / 100 + 4) << 16) |
                   ((g * 35 / 100 + 3) << 8) |
                   (b * 35 / 100 + 12);
    uint32_t bottom = ((r * 8 / 100) << 16) |
                      ((g * 8 / 100) << 8) |
                      (b * 10 / 100 + 4);
    lv_obj_set_style_bg_color(s_bg_obj, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(s_bg_obj, lv_color_hex(bottom), 0);
    lv_obj_invalidate(s_bg_obj);
}

static void clear_cover_visual(void) {
    s_art_visible = false;
    s_cover_angle = 0;
    if (s_cover_img != NULL) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_rotation(s_cover_img, 0);
    }
    if (s_cover_placeholder != NULL) {
        lv_obj_remove_flag(s_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    set_default_background();
}

static void reload_art(void) {
    if (s_art_buf == NULL || s_cover_img == NULL) return;
    if (!music_remote_copy_art(s_art_buf, MUSIC_ART_BYTES, &s_art_seq)) return;
    if (!s_art_visible) {
        s_art_visible = true;
        lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        if (s_cover_placeholder != NULL) {
            lv_obj_add_flag(s_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_invalidate(s_cover_img);
    update_background_from_cover();
    ESP_LOGI(TAG, "封面已更新");
}

// ── 定时器 ───────────────────────────────────────────────────────
static void ui_tick(lv_timer_t *timer) {
    (void)timer;
    if (!s_visible) return;

    int64_t now = (int64_t)lv_tick_get();
    if (now - s_last_snap_ms >= SNAP_POLL_MS) {
        s_last_snap_ms = now;
        music_state_t snap;
        music_remote_get_snapshot(&snap);
        apply_snapshot(&snap);
        reload_art();
    }
    if (s_scroll != NULL) {
        lyrics_scroll_update(s_scroll, display_position_ms());
    }
    if (s_playing && s_art_visible && s_cover_img != NULL) {
        s_cover_angle = (s_cover_angle + 4) % 3600;   // 0.1 度单位
        lv_image_set_rotation(s_cover_img, s_cover_angle);
    }
}

// ── 按钮事件 ─────────────────────────────────────────────────────
static void on_prev_click(lv_event_t *e) {
    (void)e;
    music_remote_send_cmd(MUSIC_CMD_PREV);
    ESP_LOGI(TAG, "按钮：上一首");
}

static void on_next_click(lv_event_t *e) {
    (void)e;
    music_remote_send_cmd(MUSIC_CMD_NEXT);
    ESP_LOGI(TAG, "按钮：下一首");
}

static void on_play_click(lv_event_t *e) {
    (void)e;
    s_pos_ms = display_position_ms();
    s_pos_sample_ms = lv_tick_get();
    s_playing = !s_playing;
    s_toggle_until = (int64_t)lv_tick_get() + TOGGLE_COOLDOWN_MS;
    set_playing_icon(s_playing);
    music_remote_send_cmd(MUSIC_CMD_PLAY_PAUSE);
    ESP_LOGI(TAG, "按钮：%s", s_playing ? "播放" : "暂停");
}

// ── 页面构建 ─────────────────────────────────────────────────────
static void build_background(lv_obj_t *screen) {
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG_BOTTOM), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bg = make_plain(screen, SCREEN_W, SCREEN_H, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(COL_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(bg, lv_color_hex(COL_BG_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(bg, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_stop(bg, 160, 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    s_bg_obj = bg;
}

static void build_vinyl(lv_obj_t *screen) {
    lv_obj_t *vinyl = make_plain(screen, VINYL_D, VINYL_D, VINYL_X, VINYL_Y);
    lv_obj_set_style_radius(vinyl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vinyl, lv_color_hex(COL_VINYL), 0);
    lv_obj_set_style_bg_opa(vinyl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl, 1, 0);
    lv_obj_set_style_border_color(vinyl, lv_color_hex(COL_GROOVE), 0);
    lv_obj_remove_flag(vinyl, LV_OBJ_FLAG_CLICKABLE);

    // 几圈唱片槽纹
    for (int i = 0; i < 4; ++i) {
        int d = VINYL_D - 20 - i * 18;
        lv_obj_t *ring = make_plain(vinyl, d, d, (VINYL_D - d) / 2,
                                    (VINYL_D - d) / 2);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(i % 2 ? 0x222222 : 0x141414), 0);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }

    // 圆形封面容器：旋转方形封面时只露出圆形部分
    lv_obj_t *clip = make_plain(screen, COVER_D, COVER_D,
                                VINYL_CX - COVER_D / 2, VINYL_CY - COVER_D / 2);
    lv_obj_set_style_radius(clip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(clip, true, 0);
    lv_obj_set_style_bg_color(clip, lv_color_hex(COL_COVER_BG), 0);
    lv_obj_set_style_bg_opa(clip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(clip, 0, 0);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE);

    s_cover_placeholder = lv_label_create(clip);
    lv_label_set_text(s_cover_placeholder, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(s_cover_placeholder, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_cover_placeholder, lv_color_hex(0x4A4A4A), 0);
    lv_obj_center(s_cover_placeholder);

    if (s_art_buf != NULL) {
        s_cover_img = lv_image_create(clip);
        lv_image_set_src(s_cover_img, &s_art_dsc);
        lv_obj_set_size(s_cover_img, COVER_D, COVER_D);
        lv_obj_set_pos(s_cover_img, 0, 0);
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    }

    // 唱针：一条白色折线 + 转轴 + 针尖
    static lv_point_precise_t arm_pts[4] = {
        {350, 20}, {340, 60}, {320, 105}, {300, 145}
    };
    lv_obj_t *arm = lv_line_create(screen);
    lv_line_set_points(arm, arm_pts, 4);
    lv_obj_set_style_line_width(arm, 4, 0);
    lv_obj_set_style_line_color(arm, lv_color_hex(COL_ARM), 0);
    lv_obj_set_style_line_rounded(arm, true, 0);
    lv_obj_remove_flag(arm, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *pivot = make_plain(screen, 20, 20, 340, 10);
    lv_obj_set_style_radius(pivot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pivot, lv_color_hex(COL_ARM), 0);
    lv_obj_set_style_bg_opa(pivot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(pivot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *needle = make_plain(screen, 10, 10, 295, 140);
    lv_obj_set_style_radius(needle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(needle, lv_color_hex(COL_ARM), 0);
    lv_obj_set_style_bg_opa(needle, LV_OPA_COVER, 0);
    lv_obj_remove_flag(needle, LV_OBJ_FLAG_CLICKABLE);
}

static void build_left_panel(lv_obj_t *screen) {
    build_vinyl(screen);

    lv_obj_t *btn_prev = make_circle_button(screen, NAV_D, COL_NAV,
                                            CTRL_LEFT_CX, CTRL_CY);
    add_icon(btn_prev, LV_SYMBOL_PREV, &lv_font_montserrat_32, COL_ICON_DARK);
    lv_obj_add_event_cb(btn_prev, on_prev_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_play = make_circle_button(screen, PLAY_D, COL_PLAY,
                                            CTRL_PLAY_CX, CTRL_CY);
    s_play_label = add_icon(btn_play, LV_SYMBOL_PLAY, &lv_font_montserrat_40,
                            COL_ICON_DARK);
    lv_obj_add_event_cb(btn_play, on_play_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_next = make_circle_button(screen, NAV_D, COL_NAV,
                                            CTRL_RIGHT_CX, CTRL_CY);
    add_icon(btn_next, LV_SYMBOL_NEXT, &lv_font_montserrat_32, COL_ICON_DARK);
    lv_obj_add_event_cb(btn_next, on_next_click, LV_EVENT_CLICKED, NULL);
}

void lyrics_page_create(lv_obj_t *screen) {
    if (screen == NULL || s_screen != NULL) return;
    s_screen = screen;
    s_visible = true;

    if (s_lyric_text == NULL) {
        s_lyric_text = heap_caps_malloc(MUSIC_LYRIC_MAX_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_lyric_lines = heap_caps_malloc(sizeof(music_lyric_line_t) * MUSIC_LYRIC_MAX_LINES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_lyric_text == NULL || s_lyric_lines == NULL) {
        ESP_LOGE(TAG, "歌词缓冲分配失败");
        return;
    }
    music_lyric_init(&s_lyric, s_lyric_text, MUSIC_LYRIC_MAX_BYTES,
                     s_lyric_lines, MUSIC_LYRIC_MAX_LINES);

    if (s_art_buf == NULL) {
        s_art_buf = heap_caps_malloc(MUSIC_ART_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_art_buf != NULL) {
        memset(&s_art_dsc, 0, sizeof(s_art_dsc));
        s_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_art_dsc.header.w = MUSIC_ART_W;
        s_art_dsc.header.h = MUSIC_ART_H;
        s_art_dsc.header.stride = MUSIC_ART_W * 2;
        s_art_dsc.data_size = MUSIC_ART_BYTES;
        s_art_dsc.data = s_art_buf;
    } else {
        ESP_LOGW(TAG, "封面缓冲分配失败，黑胶将显示占位图标");
    }

    build_background(screen);
    build_left_panel(screen);
    s_scroll = lyrics_scroll_create(screen, &lyrics_font_44, LYRIC_ROW_H,
                                    LYRIC_CURRENT_CY, LYRIC_X, LYRIC_W,
                                    LYRIC_TEXT_W);

    if (s_scroll == NULL) {
        ESP_LOGE(TAG, "歌词滚动控制器创建失败");
        return;
    }

    set_playing_icon(false);
    lyrics_scroll_set_meta(s_scroll, "", "", false, false);
    lyrics_scroll_set_lyrics(s_scroll, NULL, 0);

    s_ui_timer = lv_timer_create(ui_tick, UI_TICK_MS, NULL);
    if (s_ui_timer == NULL) {
        ESP_LOGE(TAG, "歌词界面定时器创建失败");
        return;
    }

    esp_err_t err = music_remote_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "远控任务启动失败，界面以空状态运行");
    }
    ESP_LOGI(TAG, "歌词页已创建：1280x452，左侧黑胶，右侧 4 行（当前句 y=%d）",
             LYRIC_CURRENT_CY);
}

void lyrics_page_set_visible(bool visible) {
    if (s_screen == NULL) return;
    s_visible = visible;
    if (visible) {
        music_remote_start();
        if (s_ui_timer != NULL) lv_timer_resume(s_ui_timer);
    } else {
        if (s_ui_timer != NULL) lv_timer_pause(s_ui_timer);
        music_remote_stop();
    }
}
