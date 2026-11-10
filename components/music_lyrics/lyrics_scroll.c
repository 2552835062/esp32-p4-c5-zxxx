// -*- coding: utf-8 -*-
// 歌词滚动控制器（4 行可视版）。
//
// 右侧歌词区固定显示 4 个“显示行”，当前句固定在第二个显示行：
//   第 1 行：上一句（可以是上一句的最后一行，颜色淡化）
//   第 2 行：当前句的第一个显示行（白色）
//   第 3/4 行：当前句的续行或后面的句子（灰色）
//
// 一句歌词先按像素宽度预切成 1..3 个显示行（约 640px，44px 字体下
// "Trouble is a friend yeah trouble is" 正好一行），再进入显示行队列。
// 换句时滚动距离 = 当前句占用的显示行数，因此换行歌词会一次上移两行。
#include "lyrics_scroll.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "music_state.h"

#define SCROLL_VISIBLE 4
#define SCROLL_EXTRA 3
#define SCROLL_SLOTS (SCROLL_VISIBLE + SCROLL_EXTRA)
#define SCROLL_MAX_WRAP 3
#define SCROLL_DISP_MAX 1600
#define SCROLL_ANIM_MS 320
#define SCROLL_TEXT_MAX 2048

#define COL_CURRENT 0xFFFFFF      // 当前句（整句，包括换行后的续行）
#define COL_PREV    0x4A4A4A      // 上一句：淡化
#define COL_NEXT    0x8A8A8A      // 下一句
#define COL_HINT    0x9A9A9A

static const char *TAG = "lyrics_scroll";

typedef struct {
    const char *text;
    int entry;                 // -1 = 歌名/歌手占位，0.. = 歌词行号
} disp_line_t;

typedef struct {
    lyrics_scroll_t *sc;
    uint32_t gen;
} anim_ctx_t;

struct lyrics_scroll {
    lv_obj_t *root;             // 固定视口（4 行高）
    lv_obj_t *track;            // 移动容器（7 行高）
    lv_obj_t *rows[SCROLL_SLOTS];
    lv_obj_t *labels[SCROLL_SLOTS];
    lv_obj_t *empty_title;
    lv_obj_t *empty_hint;
    const lv_font_t *font;
    const music_lyric_t *lyr;
    int row_h;
    int max_text_w;

    int current;                // -2 无歌词，-1 第一句之前，0..count-1
    int target_disp;            // 当前句第一个显示行在 disp[] 里的下标
    int window_start;
    int pending_lyric;          // 动画目标
    int pending_disp;
    bool animating;
    uint32_t anim_gen;

    bool has_track;
    char title[MUSIC_TEXT_MAX];
    char artist[MUSIC_TEXT_MAX];
    char meta_buf[MUSIC_TEXT_MAX * 2 + 8];

    // 显示行队列：文本全部复制到 disp_buf，disp[].text 指向这里
    char disp_buf[MUSIC_LYRIC_MAX_BYTES + 4096];
    size_t disp_used;
    disp_line_t disp[SCROLL_DISP_MAX];
    int disp_count;
    int meta_lines;
    int entry_first[MUSIC_LYRIC_MAX_LINES];
    int entry_lines[MUSIC_LYRIC_MAX_LINES];
};

static lv_obj_t *make_layer(lv_obj_t *parent, int w, int h, int x, int y) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static size_t utf8_len(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static bool add_disp_line(lyrics_scroll_t *sc, const char *text, size_t len, int entry) {
    if (sc->disp_count >= SCROLL_DISP_MAX) return false;
    if (sc->disp_used + len + 1 > sizeof(sc->disp_buf)) return false;
    char *dst = sc->disp_buf + sc->disp_used;
    if (len > 0) memcpy(dst, text, len);
    dst[len] = '\0';
    sc->disp_used += len + 1;
    sc->disp[sc->disp_count].text = dst;
    sc->disp[sc->disp_count].entry = entry;
    ++sc->disp_count;
    return true;
}

// 把一段歌词按像素宽度切成最多 max_lines 个显示行。
// 拉丁文优先在空格处断行，中文按字符断行；\x1f 之后是译文，不显示。
static int split_text(lyrics_scroll_t *sc, const char *src, int entry, int max_lines) {
    char tmp[SCROLL_TEXT_MAX];
    size_t n = 0;
    if (src != NULL) {
        while (src[n] != '\0' && src[n] != '\x1f' && n + 1 < sizeof(tmp)) ++n;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;
        memcpy(tmp, src, n);
    }
    tmp[n] = '\0';

    int lines = 0;
    char *p = tmp;
    while (*p != '\0' && lines < max_lines) {
        while (*p == ' ') ++p;
        if (*p == '\0') break;

        char *start = p;
        char *fit = start;
        char *last_space = NULL;
        char *q = start;
        while (*q != '\0') {
            size_t step = utf8_len(q);
            char *next = q + step;
            int32_t w = lv_text_get_width(start, (uint32_t)(next - start),
                                          sc->font, 0);
            if (w <= sc->max_text_w) {
                fit = next;
                if (*q == ' ') last_space = q;
                q = next;
            } else {
                break;
            }
        }
        if (fit == start) {
            fit = start + utf8_len(start);   // 至少放一个字符，避免死循环
        }

        char *brk = fit;
        if (last_space != NULL && last_space > start) {
            brk = last_space;                // 在最后一个空格前断行
        }
        if (brk <= start) brk = fit;

        size_t len = (size_t)(brk - start);
        while (len > 0 && start[len - 1] == ' ') --len;
        add_disp_line(sc, start, len, entry);
        ++lines;

        p = brk;
        while (*p == ' ') ++p;
    }

    if (lines == 0) {
        add_disp_line(sc, "", 0, entry);
        lines = 1;
    }
    if (*p != '\0' && lines >= max_lines) {
        ESP_LOGW(TAG, "歌词行过长，已截断到 %d 个显示行", max_lines);
    }
    return lines;
}

static void build_display(lyrics_scroll_t *sc) {
    sc->disp_count = 0;
    sc->disp_used = 0;
    sc->meta_lines = 0;

    // 歌名/歌手作为“第 -1 句”，永远放在队列最前面；第一句之前中心显示它，
    // 第一句开始后它变成上一句，保留最后一行淡化显示。
    if (sc->has_track && sc->meta_buf[0] != '\0') {
        sc->meta_lines = split_text(sc, sc->meta_buf, -1, SCROLL_MAX_WRAP);
    } else {
        add_disp_line(sc, "...", 3, -1);
        sc->meta_lines = 1;
    }

    int count = sc->lyr != NULL ? sc->lyr->count : 0;
    for (int i = 0; i < count && i < MUSIC_LYRIC_MAX_LINES; ++i) {
        const char *text = music_lyric_text_at(sc->lyr, i);
        int first = sc->disp_count;
        int lines = split_text(sc, text, i, SCROLL_MAX_WRAP);
        sc->entry_first[i] = first;
        sc->entry_lines[i] = lines;
    }
    ESP_LOGI(TAG, "显示行队列：%d 个歌词行 -> %d 个显示行（歌名/歌手 %d 行）",
             count, sc->disp_count, sc->meta_lines);
}

static int target_for(lyrics_scroll_t *sc, int lyric_idx) {
    if (lyric_idx < 0 || sc->lyr == NULL) return 0;
    if (lyric_idx >= sc->lyr->count) lyric_idx = sc->lyr->count - 1;
    if (lyric_idx < 0) return 0;
    // entry_first[] 已经是包含歌名/歌手显示行的绝对下标，不能再加 meta_lines
    return sc->entry_first[lyric_idx];
}

static lv_color_t color_for(lyrics_scroll_t *sc, int disp_idx, int current_lyric) {
    if (disp_idx < 0 || disp_idx >= sc->disp_count) return lv_color_hex(COL_NEXT);
    int entry = sc->disp[disp_idx].entry;
    if (entry == current_lyric) return lv_color_hex(COL_CURRENT);
    if (entry < current_lyric) return lv_color_hex(COL_PREV);
    return lv_color_hex(COL_NEXT);
}

static void set_label(lyrics_scroll_t *sc, int slot, const char *text, lv_color_t color) {
    lv_label_set_text(sc->labels[slot], text != NULL ? text : "");
    lv_obj_set_style_text_color(sc->labels[slot], color, 0);
    if (text == NULL || text[0] == '\0') {
        lv_obj_add_flag(sc->rows[slot], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(sc->rows[slot], LV_OBJ_FLAG_HIDDEN);
    }
}

static void fill_labels(lyrics_scroll_t *sc, int target_disp, int color_lyric) {
    sc->window_start = target_disp - 1;
    for (int slot = 0; slot < SCROLL_SLOTS; ++slot) {
        int idx = sc->window_start + slot;
        const char *text = NULL;
        lv_color_t color = lv_color_hex(COL_NEXT);
        if (idx >= 0 && idx < sc->disp_count) {
            text = sc->disp[idx].text;
            color = color_for(sc, idx, color_lyric);
        }
        set_label(sc, slot, text, color);
    }
    lv_obj_set_y(sc->track, 0);
}

static void show_empty(lyrics_scroll_t *sc, bool empty) {
    if (empty) {
        lv_obj_add_flag(sc->track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sc->empty_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sc->empty_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(sc->track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sc->empty_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sc->empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void finish_anim(lyrics_scroll_t *sc) {
    sc->animating = false;
    sc->current = sc->pending_lyric;
    sc->target_disp = sc->pending_disp;
    ++sc->anim_gen;
    fill_labels(sc, sc->target_disp, sc->current);
}

static void on_anim_completed(lv_anim_t *a) {
    anim_ctx_t *ctx = lv_anim_get_user_data(a);
    if (ctx != NULL) {
        if (ctx->gen == ctx->sc->anim_gen) {
            finish_anim(ctx->sc);
        }
        lv_free(ctx);
        lv_anim_set_user_data(a, NULL);
    }
}

static void on_anim_deleted(lv_anim_t *a) {
    anim_ctx_t *ctx = lv_anim_get_user_data(a);
    if (ctx != NULL) {
        lv_free(ctx);
        lv_anim_set_user_data(a, NULL);
    }
}

static void cancel_anim(lyrics_scroll_t *sc) {
    if (!sc->animating) return;
    ++sc->anim_gen;
    lv_anim_delete(sc->track, NULL);
    sc->animating = false;
}

static void snap_to(lyrics_scroll_t *sc, int lyric_idx) {
    cancel_anim(sc);
    if (sc->lyr == NULL || sc->lyr->count <= 0) {
        sc->current = -2;
        return;
    }
    if (lyric_idx < 0) lyric_idx = -1;
    if (lyric_idx >= sc->lyr->count) lyric_idx = sc->lyr->count - 1;
    sc->current = lyric_idx;
    sc->target_disp = target_for(sc, lyric_idx);
    sc->pending_lyric = sc->current;
    sc->pending_disp = sc->target_disp;
    fill_labels(sc, sc->target_disp, sc->current);
}

static void start_anim(lyrics_scroll_t *sc, int new_lyric) {
    int new_target = target_for(sc, new_lyric);
    int delta = new_target - sc->target_disp;
    if (delta <= 0 || delta > SCROLL_EXTRA) {
        snap_to(sc, new_lyric);
        return;
    }
    sc->pending_lyric = new_lyric;
    sc->pending_disp = new_target;
    sc->animating = true;
    ++sc->anim_gen;

    // 保持旧窗口的文字，但颜色按新当前句刷新：
    // 新当前句（可能占 2-3 行）白色，旧当前句变成上一句淡化。
    fill_labels(sc, sc->target_disp, new_lyric);

    anim_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        snap_to(sc, new_lyric);
        return;
    }
    ctx->sc = sc;
    ctx->gen = sc->anim_gen;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sc->track);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, 0, -delta * sc->row_h);
    lv_anim_set_duration(&a, SCROLL_ANIM_MS + (uint32_t)(delta - 1) * 100);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, ctx);
    lv_anim_set_completed_cb(&a, on_anim_completed);
    lv_anim_set_deleted_cb(&a, on_anim_deleted);
    lv_anim_start(&a);
}

lyrics_scroll_t *lyrics_scroll_create(lv_obj_t *parent, const lv_font_t *font,
                                      int row_h, int center_y, int root_x,
                                      int root_w, int max_text_w) {
    if (parent == NULL || font == NULL || row_h <= 0 || root_w <= 0 ||
        max_text_w <= 0) {
        return NULL;
    }
    if (max_text_w > root_w) max_text_w = root_w;

    lyrics_scroll_t *sc = lv_malloc(sizeof(*sc));
    if (sc == NULL) {
        ESP_LOGE(TAG, "歌词滚动控制器内存分配失败");
        return NULL;
    }
    memset(sc, 0, sizeof(*sc));
    sc->font = font;
    sc->row_h = row_h;
    sc->max_text_w = max_text_w;
    sc->current = -2;
    sc->pending_lyric = -2;
    sc->target_disp = 0;
    sc->window_start = -1;

    const int root_h = row_h * SCROLL_VISIBLE;
    sc->root = make_layer(parent, root_w, root_h, root_x,
                          center_y - root_h * 3 / 8);
    sc->track = make_layer(sc->root, root_w, row_h * SCROLL_SLOTS, 0, 0);

    const int side_pad = (root_w - max_text_w) / 2;
    for (int slot = 0; slot < SCROLL_SLOTS; ++slot) {
        sc->rows[slot] = make_layer(sc->track, lv_pct(100), row_h, 0,
                                    slot * row_h);
        lv_obj_set_style_pad_left(sc->rows[slot], side_pad, 0);
        lv_obj_set_style_pad_right(sc->rows[slot], side_pad, 0);

        lv_obj_t *label = lv_label_create(sc->rows[slot]);
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COL_NEXT), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(label, 0, 0);
        lv_obj_set_style_pad_all(label, 0, 0);
        sc->labels[slot] = label;
    }

    sc->empty_title = lv_label_create(sc->root);
    lv_obj_set_width(sc->empty_title, lv_pct(100));
    lv_obj_set_style_text_align(sc->empty_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sc->empty_title, font, 0);
    lv_obj_set_style_text_color(sc->empty_title, lv_color_hex(COL_CURRENT), 0);
    lv_label_set_long_mode(sc->empty_title, LV_LABEL_LONG_WRAP);
    lv_obj_align(sc->empty_title, LV_ALIGN_CENTER, 0, -26);

    sc->empty_hint = lv_label_create(sc->root);
    lv_obj_set_width(sc->empty_hint, lv_pct(100));
    lv_obj_set_style_text_align(sc->empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sc->empty_hint, font, 0);
    lv_obj_set_style_text_color(sc->empty_hint, lv_color_hex(COL_HINT), 0);
    lv_label_set_long_mode(sc->empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(sc->empty_hint, LV_ALIGN_CENTER, 0, 30);

    lyrics_scroll_set_meta(sc, "", "", false, false);
    show_empty(sc, true);
    return sc;
}

void lyrics_scroll_set_meta(lyrics_scroll_t *sc, const char *title,
                            const char *artist, bool agent_found, bool has_track) {
    if (sc == NULL) return;
    sc->has_track = has_track;
    snprintf(sc->title, sizeof(sc->title), "%s", title != NULL ? title : "");
    snprintf(sc->artist, sizeof(sc->artist), "%s", artist != NULL ? artist : "");
    if (has_track && sc->title[0] != '\0') {
        if (sc->artist[0] != '\0') {
            snprintf(sc->meta_buf, sizeof(sc->meta_buf), "%s - %s",
                     sc->title, sc->artist);
        } else {
            snprintf(sc->meta_buf, sizeof(sc->meta_buf), "%s", sc->title);
        }
    } else {
        sc->meta_buf[0] = '\0';
    }

    char line[MUSIC_TEXT_MAX * 2 + 8];
    if (has_track) {
        if (sc->artist[0] != '\0') {
            snprintf(line, sizeof(line), "%s - %s",
                     sc->title[0] != '\0' ? sc->title : "未知曲目", sc->artist);
        } else {
            snprintf(line, sizeof(line), "%s",
                     sc->title[0] != '\0' ? sc->title : "未知曲目");
        }
        lv_label_set_text(sc->empty_title, line);
        lv_label_set_text(sc->empty_hint, "暂无歌词");
    } else if (agent_found) {
        lv_label_set_text(sc->empty_title, "暂无播放");
        lv_label_set_text(sc->empty_hint, "请在网易云音乐中播放歌曲");
    } else {
        lv_label_set_text(sc->empty_title, "等待电脑端代理");
        lv_label_set_text(sc->empty_hint, "请启动电脑端 music_agent.py");
    }

    // 歌名/歌手变化会改变显示行队列；只在有歌词时重建，避免打断空状态。
    if (sc->lyr != NULL && sc->lyr->count > 0) {
        cancel_anim(sc);
        build_display(sc);
        if (sc->current >= -1) {
            sc->target_disp = target_for(sc, sc->current);
            sc->pending_lyric = sc->current;
            sc->pending_disp = sc->target_disp;
            fill_labels(sc, sc->target_disp, sc->current);
        }
    }
}

void lyrics_scroll_set_lyrics(lyrics_scroll_t *sc, const music_lyric_t *lyr,
                              uint32_t position_ms) {
    if (sc == NULL) return;
    cancel_anim(sc);
    sc->lyr = lyr;
    if (lyr == NULL || lyr->count <= 0) {
        sc->current = -2;
        sc->target_disp = 0;
        show_empty(sc, true);
        return;
    }
    show_empty(sc, false);
    build_display(sc);
    int idx = music_lyric_index_at(lyr, position_ms);
    snap_to(sc, idx < 0 ? -1 : idx);
}

void lyrics_scroll_update(lyrics_scroll_t *sc, uint32_t position_ms) {
    if (sc == NULL || sc->lyr == NULL || sc->lyr->count <= 0 ||
        sc->current == -2) {
        return;
    }
    int idx = music_lyric_index_at(sc->lyr, position_ms);
    if (idx < 0) idx = -1;
    if (idx == sc->current) return;
    if (sc->animating) {
        if (idx == sc->pending_lyric) return;
        snap_to(sc, idx);
        return;
    }
    if (idx == sc->current + 1) {
        start_anim(sc, idx);
    } else {
        snap_to(sc, idx);
    }
}

void lyrics_scroll_destroy(lyrics_scroll_t *sc) {
    if (sc == NULL) return;
    cancel_anim(sc);
    lv_free(sc);
}
