// -*- coding: utf-8 -*-
// 网易云远控客户端实现（纯歌词版，不下载封面）。
//
// 数据通路：电脑端代理每 2 秒向 255.255.255.255:9878 广播
//   "MUSICAGENT|<ip>|<port>"，本任务收到后按 1 秒间隔拉取
//   GET /state（JSON 状态）与 GET /lyrics?h=<哈希>（换歌时拉一次完整 LRC），
//   控制指令 POST /cmd。
// 超过 8 秒没有广播视为代理离线，转入演示模式（music_state.c 的假歌单）。
//
// 生命周期：任务用"代号(generation)"区分。界面反复进出页面时，旧任务
// 拿到的是旧代号，醒来即退出，不会和新任务并存。
#include "music_remote.h"
#include "music_lyric.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define MUSIC_AGENT_UDP_PORT 9878
#define MUSIC_AGENT_TIMEOUT_MS 8000
#define MUSIC_POLL_AGENT_MS 1000
#define MUSIC_POLL_DEMO_MS 400
#define MUSIC_STATE_BUF 4096
#define MUSIC_LRC_MAX MUSIC_LYRIC_MAX_BYTES
#define MUSIC_LYRIC_RETRY_MS 5000

static const char *TAG = "music_remote";

typedef struct {
    char ip[24];
    int port;
    int64_t last_seen_ms;
} agent_info_t;

static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t wake;  // 指令/退出唤醒：让任务立刻结束等待
    music_state_t snap;
    char *state_buf;         // /state 的 JSON 下载缓冲
    char *lrc_buf;           // 歌词下载暂存
    char *lrc;               // 已发布的歌词文本
    size_t lrc_cap;
    size_t lrc_len;
    uint32_t lyric_seq;      // 歌词版本号，界面按它判断是否需要重新解析
    char lyric_hash[MUSIC_HASH_MAX];
    char lyric_try_hash[MUSIC_HASH_MAX];
    int64_t lyric_retry_ms;
    uint8_t *art[2];         // 240x240 RGB565 双缓冲
    int art_active;
    uint32_t art_seq;
    bool art_valid;
    char art_hash[MUSIC_HASH_MAX];
    char art_try_hash[MUSIC_HASH_MAX];
    int64_t art_retry_ms;
    const char *demo_lrc;    // 已发布的演示歌词，指针变化表示换曲
    agent_info_t agent;
    QueueHandle_t cmd_queue;
    volatile bool running;
    volatile uint32_t task_gen;
    volatile bool wifi_ready;
} s_m;

// ── HTTP ─────────────────────────────────────────────────────────

typedef struct {
    char *text;
    int text_max;
    int text_len;
} http_sink_t;

static esp_err_t http_event(esp_http_client_event_t *evt) {
    http_sink_t *sink = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || sink == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    int room = sink->text_max - sink->text_len - 1;
    int len = evt->data_len < room ? evt->data_len : room;
    if (len > 0) {
        memcpy(sink->text + sink->text_len, evt->data, (size_t)len);
        sink->text_len += len;
        sink->text[sink->text_len] = '\0';
    }
    return ESP_OK;
}

// 返回 HTTP 状态码；失败返回 -1。
static int http_get(const agent_info_t *agent, const char *path,
                    char *text, int text_max) {
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s", agent->ip, agent->port, path);
    http_sink_t sink = {.text = text, .text_max = text_max, .text_len = 0};
    if (text != NULL && text_max > 0) text[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 2500,
        .event_handler = http_event,
        .user_data = &sink,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return -1;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return -1;
    return status;
}

typedef struct {
    uint8_t *buf;
    int max;
    int len;
} bin_sink_t;

static esp_err_t bin_event(esp_http_client_event_t *evt) {
    bin_sink_t *sink = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || sink == NULL ||
        sink->buf == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    int room = sink->max - sink->len;
    int len = evt->data_len < room ? evt->data_len : room;
    if (len > 0) {
        memcpy(sink->buf + sink->len, evt->data, (size_t)len);
        sink->len += len;
    }
    return ESP_OK;
}

// 下载二进制数据；返回 HTTP 状态码，实际长度写入 out_len。
static int http_get_bin(const agent_info_t *agent, const char *path,
                        uint8_t *buf, int max, int *out_len) {
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s", agent->ip, agent->port, path);
    bin_sink_t sink = {.buf = buf, .max = max, .len = 0};
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 4000,
        .event_handler = bin_event,
        .user_data = &sink,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return -1;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (out_len) *out_len = sink.len;
    if (err != ESP_OK) return -1;
    return status;
}

static bool http_post_cmd(const agent_info_t *agent, music_cmd_t cmd) {
    const char *action = cmd == MUSIC_CMD_NEXT ? "next" :
                         cmd == MUSIC_CMD_PREV ? "prev" : "play_pause";
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d/cmd", agent->ip, agent->port);
    char body[48];
    snprintf(body, sizeof(body), "{\"action\":\"%s\"}", action);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 2500,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return false;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "发送控制指令 %s 失败：err=%s status=%d", action,
                 esp_err_to_name(err), status);
        return false;
    }
    ESP_LOGI(TAG, "已发送控制指令：%s", action);
    return true;
}

// ── 状态解析与发布 ───────────────────────────────────────────────

static void publish_snapshot(const music_state_t *snap) {
    xSemaphoreTake(s_m.lock, portMAX_DELAY);
    s_m.snap = *snap;
    xSemaphoreGive(s_m.lock);
}

// 发布歌词文本（text 可为空串，表示"本曲确实没有歌词"）；版本号自增通知界面重解析。
static void publish_lyrics(const char *text) {
    xSemaphoreTake(s_m.lock, portMAX_DELAY);
    size_t len = text ? strlen(text) : 0;
    if (len >= s_m.lrc_cap) len = s_m.lrc_cap - 1;
    // 不截断 UTF-8 中间字节，否则界面解析出半个汉字
    while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) --len;
    if (len > 0) memcpy(s_m.lrc, text, len);
    s_m.lrc[len] = '\0';
    s_m.lrc_len = len;
    s_m.lyric_seq++;
    xSemaphoreGive(s_m.lock);
}

static void publish_art(const uint8_t *pixels) {
    xSemaphoreTake(s_m.lock, portMAX_DELAY);
    s_m.art_active = 1 - s_m.art_active;
    memcpy(s_m.art[s_m.art_active], pixels, MUSIC_ART_BYTES);
    s_m.art_seq++;
    s_m.art_valid = true;
    xSemaphoreGive(s_m.lock);
}

static void refresh_art(agent_info_t *agent, const char *hash, int64_t now_ms) {
    if (hash[0] == '\0' || strcmp(hash, s_m.art_hash) == 0) return;
    bool new_track = strcmp(hash, s_m.art_try_hash) != 0;
    if (!new_track && now_ms - s_m.art_retry_ms < MUSIC_LYRIC_RETRY_MS) return;
    snprintf(s_m.art_try_hash, sizeof(s_m.art_try_hash), "%s", hash);
    s_m.art_retry_ms = now_ms;

    char path[128];
    snprintf(path, sizeof(path), "/art_rgb565?h=%s", hash);
    int scratch = 1 - s_m.art_active;
    int len = 0;
    int status = http_get_bin(agent, path, s_m.art[scratch], MUSIC_ART_BYTES, &len);
    if (status == 200 && len == MUSIC_ART_BYTES) {
        publish_art(s_m.art[scratch]);
        snprintf(s_m.art_hash, sizeof(s_m.art_hash), "%s", hash);
        ESP_LOGI(TAG, "封面已就绪：%d 字节", len);
    } else {
        ESP_LOGW(TAG, "封面获取失败：status=%d len=%d", status, len);
    }
}

// 换歌时拉一次完整 LRC；界面自己按播放进度滚动显示。
// 代理要现搜网易云，第一次往往来不及（板端 HTTP 超时 2.5s），失败后 5 秒重试。
static void refresh_lyrics(agent_info_t *agent, const char *hash, int64_t now_ms) {
    if (hash[0] == '\0' || strcmp(hash, s_m.lyric_hash) == 0) return;
    bool new_track = strcmp(hash, s_m.lyric_try_hash) != 0;
    if (!new_track && now_ms - s_m.lyric_retry_ms < MUSIC_LYRIC_RETRY_MS) {
        return;
    }
    snprintf(s_m.lyric_try_hash, sizeof(s_m.lyric_try_hash), "%s", hash);
    s_m.lyric_retry_ms = now_ms;
    // 换了一首新歌才清掉上一首的歌词，重试期间保留已拿到的内容
    if (new_track) publish_lyrics("");

    char path[128];
    snprintf(path, sizeof(path), "/lyrics?h=%s", hash);
    s_m.lrc_buf[0] = '\0';
    int status = http_get(agent, path, s_m.lrc_buf, (int)s_m.lrc_cap - 1);
    if (status != 200 || s_m.lrc_buf[0] == '\0') {
        ESP_LOGW(TAG, "歌词获取失败：status=%d（纯音乐或代理还没匹配到原曲）", status);
        return;
    }
    snprintf(s_m.lyric_hash, sizeof(s_m.lyric_hash), "%s", hash);
    publish_lyrics(s_m.lrc_buf);
    ESP_LOGI(TAG, "歌词已就绪：%u 字节", (unsigned)strlen(s_m.lrc_buf));
}

static void fetch_agent_state(agent_info_t *agent, int64_t now_ms) {
    char *buf = s_m.state_buf;
    buf[0] = '\0';
    int status = http_get(agent, "/state", buf, MUSIC_STATE_BUF - 1);
    if (status != 200) {
        static int64_t last_warn_ms;
        if (now_ms - last_warn_ms > 5000) {
            last_warn_ms = now_ms;
            ESP_LOGW(TAG, "获取播放状态失败：status=%d（检查代理是否在运行）", status);
        }
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "状态 JSON 解析失败（%u 字节）", (unsigned)strlen(buf));
        return;
    }
    music_state_t snap = {0};
    snap.agent_found = true;
    snap.track_loaded = cJSON_IsTrue(cJSON_GetObjectItem(root, "track"));
    snap.playing = cJSON_IsTrue(cJSON_GetObjectItem(root, "playing"));
    cJSON *pos = cJSON_GetObjectItem(root, "position");
    cJSON *dur = cJSON_GetObjectItem(root, "duration");
    snap.position_ms = cJSON_IsNumber(pos) ? (uint32_t)(cJSON_GetNumberValue(pos) * 1000.0) : 0;
    snap.duration_ms = cJSON_IsNumber(dur) ? (uint32_t)(cJSON_GetNumberValue(dur) * 1000.0) : 0;

    const cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "title")) != NULL && cJSON_IsString(item)) {
        snprintf(snap.title, sizeof(snap.title), "%s", cJSON_GetStringValue(item));
    }
    if ((item = cJSON_GetObjectItem(root, "artist")) != NULL && cJSON_IsString(item)) {
        snprintf(snap.artist, sizeof(snap.artist), "%s", cJSON_GetStringValue(item));
    }
    if ((item = cJSON_GetObjectItem(root, "album")) != NULL && cJSON_IsString(item)) {
        snprintf(snap.album, sizeof(snap.album), "%s", cJSON_GetStringValue(item));
    }
    if ((item = cJSON_GetObjectItem(root, "app")) != NULL && cJSON_IsString(item)) {
        snprintf(snap.app, sizeof(snap.app), "%s", cJSON_GetStringValue(item));
    }
    if ((item = cJSON_GetObjectItem(root, "lyric")) != NULL && cJSON_IsString(item)) {
        const char *src = cJSON_GetStringValue(item);
        size_t n = strlen(src);
        if (n >= sizeof(snap.lyric)) {
            n = sizeof(snap.lyric) - 1;
            while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;
        }
        memcpy(snap.lyric, src, n);
        snap.lyric[n] = '\0';
    }

    const cJSON *hash = cJSON_GetObjectItem(root, "art_hash");
    const char *hash_str = cJSON_IsString(hash) ? cJSON_GetStringValue(hash) : "";
    if (hash_str[0] != '\0') {
        snprintf(snap.track_hash, sizeof(snap.track_hash), "%s", hash_str);
    } else {
        // 代理没给哈希时用标题/歌手/时长拼一个，保证换歌能被界面发现
        snprintf(snap.track_hash, sizeof(snap.track_hash), "%s|%s|%" PRIu32,
                 snap.title, snap.artist, snap.duration_ms);
    }

    publish_snapshot(&snap);

    if (snap.track_loaded && hash_str[0] != '\0') {
        static char last_title[MUSIC_TEXT_MAX];
        if (strcmp(snap.title, last_title) != 0) {
            snprintf(last_title, sizeof(last_title), "%s", snap.title);
            ESP_LOGI(TAG, "曲目已切换：《%s》", snap.title);
        }
        refresh_lyrics(agent, hash_str, now_ms);
        refresh_art(agent, hash_str, now_ms);
    }
    cJSON_Delete(root);
}

// ── 代理发现（UDP 广播） ─────────────────────────────────────────

static int udp_listen_open(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MUSIC_AGENT_UDP_PORT);
    addr.sin_addr.s_addr = IPADDR_ANY;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    ESP_LOGI(TAG, "正在监听电脑端代理广播（UDP %d）", MUSIC_AGENT_UDP_PORT);
    return sock;
}

// 收到广播则更新代理地址；返回代理是否仍然新鲜。
static bool udp_poll_agent(int sock, int64_t now_ms) {
    if (sock < 0) return false;
    uint8_t buf[128];
    for (int reads = 0; reads < 4; ++reads) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                           (struct sockaddr *)&src, &src_len);
        if (len <= 0) break;
        buf[len] = '\0';
        char magic[16] = {0};
        char ip[24] = {0};
        int port = 0;
        if (sscanf((char *)buf, "%15[^|]|%23[^|]|%d", magic, ip, &port) != 3 ||
            strcmp(magic, "MUSICAGENT") != 0 || port <= 0 || port > 65535) {
            continue;   // 无关报文
        }
        // 广播报文的源地址就是代理，优先用源地址，报文里的 ip 仅作兜底
        char src_ip[24] = {0};
        inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
        if (src_ip[0] && strcmp(src_ip, "0.0.0.0") != 0) {
            snprintf(s_m.agent.ip, sizeof(s_m.agent.ip), "%s", src_ip);
        } else {
            snprintf(s_m.agent.ip, sizeof(s_m.agent.ip), "%s", ip);
        }
        s_m.agent.port = port;
        s_m.agent.last_seen_ms = now_ms;
    }
    return s_m.agent.ip[0] && now_ms - s_m.agent.last_seen_ms < MUSIC_AGENT_TIMEOUT_MS;
}

// ── 指令处理 ─────────────────────────────────────────────────────

// 演示模式：发布当前假歌单曲目的完整 LRC（指针变化表示换曲）。
static void publish_demo_lrc_if_needed(void) {
    const char *lrc = music_state_demo_lrc();
    if (lrc != s_m.demo_lrc) {
        s_m.demo_lrc = lrc;
        publish_lyrics(lrc);
    }
}

static void drain_cmds(bool have_agent) {
    music_cmd_t cmd;
    while (xQueueReceive(s_m.cmd_queue, &cmd, 0) == pdPASS) {
        if (have_agent) {
            http_post_cmd(&s_m.agent, cmd);
        } else {
            music_state_t snap = {0};
            if (cmd == MUSIC_CMD_PLAY_PAUSE) {
                music_state_demo_toggle(&snap);
            } else {
                music_state_demo_skip(&snap, cmd == MUSIC_CMD_NEXT ? 1 : -1);
            }
            publish_snapshot(&snap);
            publish_demo_lrc_if_needed();
        }
    }
}

// ── 任务主体 ─────────────────────────────────────────────────────

static void wifi_ps_none(void) {
    if (!s_m.wifi_ready) return;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

static void wifi_ps_restore(void) {
    if (!s_m.wifi_ready) return;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
}

static bool task_alive(uint32_t gen) {
    return s_m.running && gen == s_m.task_gen;
}

static void music_task(void *arg) {
    const uint32_t gen = (uint32_t)(intptr_t)arg;
    int sock = -1;
    int64_t last_tick_ms = esp_timer_get_time() / 1000;
    bool agent_logged = false;
    int ps_assert_tick = 0;
    ESP_LOGI(TAG, "歌词远控任务已启动（第 %" PRIu32 " 代）", gen);
    // UDP 广播对省电非常敏感，进入页面先关省电，之后周期性重申。
    wifi_ps_none();

    while (task_alive(gen)) {
        if (sock < 0) sock = udp_listen_open();
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool have_agent = udp_poll_agent(sock, now_ms);

        if (++ps_assert_tick >= 10) {
            ps_assert_tick = 0;
            wifi_ps_none();
        }

        // 先处理用户指令再轮询状态：按下按钮后几十毫秒就能发出去
        drain_cmds(have_agent);

        if (have_agent) {
            if (!agent_logged) {
                ESP_LOGI(TAG, "已发现电脑端代理 %s:%d", s_m.agent.ip, s_m.agent.port);
                agent_logged = true;
            }
            s_m.demo_lrc = NULL;   // 代理重新上线后，若再离线要重新发布演示歌词
            fetch_agent_state(&s_m.agent, now_ms);
            last_tick_ms = now_ms;
        } else {
            if (agent_logged) {
                ESP_LOGI(TAG, "电脑端代理已离线，转入演示模式");
                agent_logged = false;
            }
            music_state_t snap;
            music_state_demo_advance(&snap, (int)(now_ms - last_tick_ms));
            publish_demo_lrc_if_needed();
            publish_snapshot(&snap);
            last_tick_ms = now_ms;
        }
        // 用信号量等待而不是纯延时：界面按下按钮时立即醒来发指令
        xSemaphoreTake(s_m.wake, pdMS_TO_TICKS(have_agent ? MUSIC_POLL_AGENT_MS
                                                          : MUSIC_POLL_DEMO_MS));
    }

    if (sock >= 0) close(sock);
    if (gen == s_m.task_gen) {
        wifi_ps_restore();
    }
    ESP_LOGI(TAG, "歌词远控任务已退出（第 %" PRIu32 " 代）", gen);
    vTaskDeleteWithCaps(NULL);
}

// ── 对外接口 ─────────────────────────────────────────────────────

static void alloc_buffers(void) {
    if (s_m.lock != NULL) return;
    s_m.lock = xSemaphoreCreateMutex();
    s_m.wake = xSemaphoreCreateBinary();
    s_m.state_buf = heap_caps_malloc(MUSIC_STATE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_m.lrc_cap = MUSIC_LRC_MAX;
    s_m.lrc_buf = heap_caps_malloc(s_m.lrc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_m.lrc = heap_caps_malloc(s_m.lrc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (int i = 0; i < 2; ++i) {
        s_m.art[i] = heap_caps_malloc(MUSIC_ART_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_m.cmd_queue = xQueueCreate(4, sizeof(music_cmd_t));
    if (s_m.lock == NULL || s_m.wake == NULL || s_m.state_buf == NULL ||
        s_m.lrc_buf == NULL || s_m.lrc == NULL || s_m.art[0] == NULL ||
        s_m.art[1] == NULL || s_m.cmd_queue == NULL) {
        ESP_LOGE(TAG, "歌词远控缓冲分配失败");
    }
    if (s_m.lrc != NULL) s_m.lrc[0] = '\0';
}

esp_err_t music_remote_start(void) {
    alloc_buffers();
    if (s_m.lock == NULL || s_m.cmd_queue == NULL || s_m.wake == NULL ||
        s_m.state_buf == NULL || s_m.lrc_buf == NULL || s_m.lrc == NULL ||
        s_m.art[0] == NULL || s_m.art[1] == NULL) {
        return ESP_FAIL;
    }
    if (s_m.running) return ESP_OK;
    s_m.running = true;
    ++s_m.task_gen;
    // 丢掉上一轮残留的唤醒信号，避免新任务刚起来就空转一圈
    while (xSemaphoreTake(s_m.wake, 0) == pdTRUE) {}
    // 栈放 PSRAM：HTTP + cJSON 的峰值用量较大
    BaseType_t ok = xTaskCreateWithCaps(music_task, "music_remote", 8192,
                                        (void *)(intptr_t)s_m.task_gen,
                                        4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_m.running = false;
        ESP_LOGE(TAG, "歌词远控任务创建失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void music_remote_stop(void) {
    if (!s_m.running) return;
    s_m.running = false;                 // 旧代任务醒来发现代号不符即退出
    if (s_m.wake) xSemaphoreGive(s_m.wake);
}

void music_remote_get_snapshot(music_state_t *out) {
    if (out == NULL) return;
    static const music_state_t empty = {0};
    if (s_m.lock == NULL) {
        *out = empty;
        return;
    }
    xSemaphoreTake(s_m.lock, portMAX_DELAY);
    *out = s_m.snap;
    xSemaphoreGive(s_m.lock);
}

bool music_remote_copy_art(void *dst, size_t dst_size, uint32_t *seq) {
    if (dst == NULL || dst_size < MUSIC_ART_BYTES || s_m.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_m.lock, portMAX_DELAY);
    bool changed = s_m.art_valid && (seq == NULL || *seq != s_m.art_seq);
    if (changed) {
        memcpy(dst, s_m.art[s_m.art_active], MUSIC_ART_BYTES);
        if (seq != NULL) *seq = s_m.art_seq;
    }
    xSemaphoreGive(s_m.lock);
    return changed;
}

bool music_remote_copy_lyrics(char *dst, size_t dst_size, uint32_t *seq) {
    if (dst == NULL || dst_size == 0 || s_m.lock == NULL) return false;
    xSemaphoreTake(s_m.lock, portMAX_DELAY);
    bool changed = (seq == NULL || *seq != s_m.lyric_seq);
    if (!changed) {
        xSemaphoreGive(s_m.lock);
        return false;
    }
    size_t n = s_m.lrc_len;
    if (n >= dst_size) {
        n = dst_size - 1;
        while (n > 0 && ((unsigned char)s_m.lrc[n] & 0xC0) == 0x80) --n;
    }
    if (n > 0) memcpy(dst, s_m.lrc, n);
    dst[n] = '\0';
    if (seq != NULL) *seq = s_m.lyric_seq;
    xSemaphoreGive(s_m.lock);
    return true;
}

void music_remote_set_wifi_ready(bool ready) {
    s_m.wifi_ready = ready;
}

void music_remote_send_cmd(music_cmd_t cmd) {
    if (s_m.cmd_queue == NULL) return;
    if (xQueueSend(s_m.cmd_queue, &cmd, 0) != pdPASS) {
        ESP_LOGW(TAG, "控制指令队列已满，丢弃一条");
        return;
    }
    // 立刻叫醒任务，别让用户等下一次轮询
    if (s_m.wake) xSemaphoreGive(s_m.wake);
}
