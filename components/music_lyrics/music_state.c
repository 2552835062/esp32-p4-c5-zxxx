// -*- coding: utf-8 -*-
// 播放状态快照与演示模式实现。
// 演示歌单与歌词均为本项目自写占位内容，不涉及任何歌曲版权。
#include "music_state.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *title;
    const char *artist;
    const char *album;
    uint32_t duration_ms;
    const char *lrc;
} demo_track_t;

static const char k_lrc_night[] =
    "[00:04.00]夜色落在安静的窗\n"
    "[00:09.00]风把旧故事轻轻吹响\n"
    "[00:18.00]我把心事写进星光\n"
    "[00:27.00]让它在夜里慢慢发亮\n"
    "[00:36.00]街灯沿着长路生长\n"
    "[00:45.00]影子陪我走过小巷\n"
    "[00:54.00]耳机里循环着过往\n"
    "[01:03.00]每一步都变得滚烫\n"
    "[01:12.00]如果明天还有方向\n"
    "[01:21.00]我想沿着梦去流浪\n"
    "[01:30.00]把没说完的话收藏\n"
    "[01:40.00]等一场温柔的晴朗\n"
    "[01:50.00]夜航星 请为我导航\n"
    "[02:00.00]穿过人海和迷惘\n"
    "[02:10.00]就算世界偶尔安静\n"
    "[02:20.00]也要唱到天亮\n"
    "[02:30.00]夜航星 请为我导航\n"
    "[02:40.00]把孤单都照亮\n"
    "[02:50.00]等风吹过来的时候\n"
    "[03:00.00]我会看见远方\n"
    "[03:12.00]所有的等待都有回响\n"
    "[03:24.00]所有的旅程都有光\n"
    "[03:36.00]把这首歌轻轻唱完\n"
    "[03:42.00]留给夜晚收藏\n";

static const char k_lrc_afternoon[] =
    "[00:04.00]午后的光停在桌角\n"
    "[00:08.50]一杯茶慢慢变凉\n"
    "[00:17.00]窗外的云走得很慢\n"
    "[00:25.50]像没写完的愿望\n"
    "[00:34.00]翻开旧书的那一页\n"
    "[00:42.50]夹着去年的花香\n"
    "[00:51.00]时间没有回答我\n"
    "[00:59.50]只把影子拉长\n"
    "[01:08.00]午后晴天 风很轻\n"
    "[01:17.00]把烦恼都吹散\n"
    "[01:26.00]让阳光落在肩上\n"
    "[01:35.00]陪我走一段\n"
    "[01:44.00]午后晴天 云很白\n"
    "[01:53.00]把心事都晾干\n"
    "[02:02.00]等下一场微风来\n"
    "[02:11.00]再轻轻说晚安\n"
    "[02:20.00]街角的树摇着叶子\n"
    "[02:29.00]像在说别慌张\n"
    "[02:38.00]所有的路都有尽头\n"
    "[02:47.00]也都有新的方向\n"
    "[02:56.00]午后晴天 风很轻\n"
    "[03:05.00]把烦恼都吹散\n"
    "[03:14.00]让阳光落在肩上\n"
    "[03:23.00]陪我走一段\n"
    "[03:32.00]等下一场微风来\n"
    "[03:41.00]再轻轻说晚安\n"
    "[03:50.00]再轻轻说晚安\n"
    "[04:00.00]午后的光还在桌角\n"
    "[04:10.00]茶香慢慢变淡\n";

static const char k_lrc_sea[] =
    "[00:04.00]海风吹过长长的堤岸\n"
    "[00:10.00]浪花把脚印填满\n"
    "[00:20.00]远处的船慢慢走远\n"
    "[00:30.00]带走了谁的期盼\n"
    "[00:40.00]我把名字写进沙滩\n"
    "[00:50.00]等潮水把它带还\n"
    "[01:00.00]那些说不出口的话\n"
    "[01:10.00]都交给海风保管\n"
    "[01:20.00]远方海风 吹过来\n"
    "[01:30.00]吹散所有阴霾\n"
    "[01:40.00]让心跟着浪花走\n"
    "[01:50.00]走到云朵之外\n"
    "[02:00.00]远方海风 吹过来\n"
    "[02:10.00]吹亮梦的色彩\n"
    "[02:20.00]就算前路还很远\n"
    "[02:30.00]也要勇敢去爱\n"
    "[02:40.00]海鸥飞过灰色的天\n"
    "[02:50.00]把自由写在风里面\n"
    "[03:00.00]我听见远方在呼唤\n"
    "[03:10.00]歌声越过海岸线\n"
    "[03:20.00]远方海风 吹过来\n"
    "[03:30.00]吹散所有阴霾\n"
    "[03:40.00]让心跟着浪花走\n"
    "[03:50.00]走到云朵之外\n"
    "[04:00.00]远方海风 吹过来\n"
    "[04:10.00]吹亮梦的色彩\n"
    "[04:20.00]就算前路还很远\n"
    "[04:30.00]也要勇敢去爱\n"
    "[04:40.00]海风还在轻轻吹\n"
    "[04:50.00]故事还没有写完\n"
    "[05:00.00]等下一次潮水来\n"
    "[05:10.00]我们再相见\n";

static const demo_track_t k_demo_tracks[] = {
    {"夜航星（演示）", "演示模式", "未连接电脑端", 224000, k_lrc_night},
    {"午后晴天（演示）", "演示模式", "未连接电脑端", 269000, k_lrc_afternoon},
    {"远方海风（演示）", "演示模式", "未连接电脑端", 326000, k_lrc_sea},
};
#define DEMO_TRACK_COUNT (int)(sizeof(k_demo_tracks) / sizeof(k_demo_tracks[0]))

static int s_demo_track;
static int64_t s_demo_pos_ms;
static bool s_demo_playing = true;

void music_state_format_time(uint32_t ms, char *out, int out_len) {
    if (out == NULL || out_len <= 0) return;
    uint32_t total = ms / 1000;
    snprintf(out, out_len, "%02u:%02u", (unsigned)(total / 60), (unsigned)(total % 60));
}

int music_state_progress_percent(uint32_t position_ms, uint32_t duration_ms) {
    if (duration_ms == 0) return 0;
    if (position_ms >= duration_ms) return 100;
    return (int)((uint64_t)position_ms * 100 / duration_ms);
}

static void demo_fill(music_state_t *state) {
    const demo_track_t *track = &k_demo_tracks[s_demo_track];
    memset(state, 0, sizeof(*state));
    state->agent_found = false;
    state->track_loaded = true;
    state->playing = s_demo_playing;
    state->demo = true;
    state->position_ms = (uint32_t)s_demo_pos_ms;
    state->duration_ms = track->duration_ms;
    snprintf(state->title, sizeof(state->title), "%s", track->title);
    snprintf(state->artist, sizeof(state->artist), "%s", track->artist);
    snprintf(state->album, sizeof(state->album), "%s", track->album);
    snprintf(state->app, sizeof(state->app), "%s", "演示模式");
    snprintf(state->track_hash, sizeof(state->track_hash), "demo:%d", s_demo_track);
    snprintf(state->lyric, sizeof(state->lyric), "%s", "演示模式：等待电脑端代理");
}

void music_state_demo_advance(music_state_t *state, int delta_ms) {
    if (delta_ms < 0) delta_ms = 0;
    if (s_demo_playing) {
        s_demo_pos_ms += delta_ms;
        if (s_demo_pos_ms >= (int64_t)k_demo_tracks[s_demo_track].duration_ms) {
            s_demo_track = (s_demo_track + 1) % DEMO_TRACK_COUNT;
            s_demo_pos_ms = 0;
        }
    }
    demo_fill(state);
}

void music_state_demo_toggle(music_state_t *state) {
    s_demo_playing = !s_demo_playing;
    demo_fill(state);
}

void music_state_demo_skip(music_state_t *state, int dir) {
    s_demo_track = ((s_demo_track + dir) % DEMO_TRACK_COUNT + DEMO_TRACK_COUNT) % DEMO_TRACK_COUNT;
    s_demo_pos_ms = 0;
    demo_fill(state);
}

const char *music_state_demo_lrc(void) {
    return k_demo_tracks[s_demo_track].lrc;
}
