// main/app_audio.c —— PCM 播放工作任务。仿 demo_audio.c 的命令模式,但任务是
// 应用级常驻:本应用没有"退出页面回收任务"的生命周期,活到断电,免去停止握手。
//
// 命令经 xTaskNotify 传值(value 高 16 位=命令,低 16 位=条目),播放循环每个
// chunk(1024 采样=64ms)之间非阻塞轮询新命令,保证切换延迟 <150ms。
// 暂停时不写也不推进 elapsed(冻结),每 30ms 醒一次轮询。
#include "app_audio.h"

#include <stddef.h>
#include <string.h>

#include "app_catalog.h"
#include "bsp_audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_audio";

#define AUDIO_HZ       16000
#define CHUNK_SAMPLES  1024
#define PAUSE_POLL_MS  30

#define CMD_NONE  (0u << 16)
#define CMD_PLAY  (1u << 16)
#define CMD_STOP  (2u << 16)
#define CMD_PAUSE (3u << 16)
#define CMD_PLAY_ONCE (4u << 16)  // 播一次:自然结束不接棒(连播开着也不接)
#define CMD_PLAY_SCENE (5u << 16) // 场景序列:按表播完自动停(不走 autoplay 接棒)
#define CMD_PLAY_LOOP (6u << 16)  // 单曲循环:自然结束重播当前条(闹钟响铃)
#define CMD_MASK  0xFFFF0000u

static TaskHandle_t s_task;
static volatile bool s_active;
static volatile int s_cur = -1;
static volatile bool s_paused;
static volatile uint32_t s_elapsed_ms;
static int s_vol = 80;              // 0..100, applied at every play_index()
static volatile bool s_autoplay;    // natural end -> play next entry (wrap)
static volatile bool s_random = true; // autoplay next pick: shuffle (avoid repeat)
static volatile bool s_listen;      // listen mode: keep playing with screen off
static volatile uint32_t s_seq;     // play sequence: ++ on every play_index start
static volatile bool s_once;        // one-shot play: natural end stops instead of chaining (quiz)
static volatile bool s_loop;        // loop play: natural end replays the same entry (alarm ring)
static volatile bool s_no_count;    // loop plays skip play statistics (alarm ring is not listening)
// 场景序列源: 播完按表接下一条, 表尾停(与 autoplay 正交, 不污染连播设置)。
// 表指针由调用方(UI)传入, 指向静态存储(scenes.h 表), 存活期=程序全程。
static const int8_t *s_scene_seq;
static volatile int s_scene_len, s_scene_pos;
static bool s_vol_dirty, s_ap_dirty, s_rnd_dirty;  // pending NVS writes
static bool s_lst_dirty;
static uint16_t s_counts[APP_CATALOG_COUNT];       // per-entry play counts (9999 cap)
static bool s_cnt_dirty;
static uint16_t s_play_secs[APP_CATALOG_COUNT];    // per-entry played seconds (65535 cap)
static bool s_sec_dirty;

// Settings persistence (volume / autoplay) in NVS namespace "gpws_app".
// Load once at task start; save is explicit (app_audio_settings_save) so the
// volume repeat timer's rapid steps coalesce into a single commit.
#define NVS_NS "gpws_app"

static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) s_vol = v;
    if (nvs_get_u8(h, "apm", &v) == ESP_OK) s_autoplay = (v != 0);
    if (nvs_get_u8(h, "rnd", &v) == ESP_OK) s_random = (v != 0);
    if (nvs_get_u8(h, "lst", &v) == ESP_OK) s_listen = (v != 0);
    // 播放计数 blob:大小不符(条目数变过)则复位重新累计。秒数 blob 同理。
    size_t len = sizeof(s_counts);
    if (nvs_get_blob(h, "cnt", s_counts, &len) != ESP_OK) {
        memset(s_counts, 0, sizeof(s_counts));
    }
    size_t slen = sizeof(s_play_secs);
    if (nvs_get_blob(h, "sec", s_play_secs, &slen) != ESP_OK) {
        memset(s_play_secs, 0, sizeof(s_play_secs));
    }
    nvs_close(h);
}

// 连播接棒条目:顺序回绕,或随机(不与当前重复;真随机即可,洗牌表是过度设计)。
static int next_index(int cur) {
    const int n = app_catalog_count();
    if (n < 2) return cur;
    if (!s_random) return (cur + 1) % n;
    // ponytail: rejection sampling, fine for n=24 (expected <2 draws)
    int r;
    do { r = (int)(esp_random() % (uint32_t)n); } while (r == cur);
    return r;
}

// 播放一个条目。返回 0=自然结束,非 0=打断它的命令(交回主循环继续处理)。
// 资源缺失/set_format 失败返回 CMD_STOP 自止:连播开着也不接棒,避免坏条目
// 让接播链无限快速循环撞 I2C。
static uint32_t play_index(int index) {
    const app_alert_t *a = app_catalog(index);
    if (!a || !a->pcm) return CMD_STOP;
    if (bsp_audio_set_format(AUDIO_HZ, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "set format failed, cannot play '%s'", a->en);
        return CMD_STOP;
    }
    bsp_audio_set_volume((uint8_t)s_vol);

    if (!s_no_count) {
        if (s_counts[index] < 9999) s_counts[index]++;  // 播放计数(9999 封顶)
        s_cnt_dirty = true;
    }
    s_cur = index;
    s_seq++;  // UI polls this: a jump means the audio task switched entries itself
    s_paused = false;
    s_elapsed_ms = 0;
    s_active = true;

    const size_t total = app_alert_pcm_bytes(a) / sizeof(int16_t); // 采样数
    size_t pos = 0;
    uint32_t interrupt = 0;
    while (pos < total) {
        uint32_t v;
        if (xTaskNotifyWait(0, UINT32_MAX, &v, 0) == pdTRUE) {
            if ((v & CMD_MASK) == CMD_PAUSE) {
                s_paused = !s_paused;
            } else {
                interrupt = v; // STOP / 新 PLAY:立刻交回主循环
                break;
            }
        }
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(PAUSE_POLL_MS));
            continue;
        }
        size_t n = total - pos;
        if (n > CHUNK_SAMPLES) n = CHUNK_SAMPLES;
        if (bsp_audio_write(a->pcm + pos, n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "PCM write failed at sample %u of '%s'",
                     (unsigned)pos, a->en);
            break;
        }
        pos += n;
        s_elapsed_ms = (uint32_t)(pos * 1000u / AUDIO_HZ);
    }
    // 已播秒数累计(打断计已播部分;暂停冻结不计):时长统计的口径是
    // 「实际出声的秒数」,与计数(每次开始播 +1)互补。循环(闹钟)不计。
    if (s_elapsed_ms && !s_no_count) {
        uint32_t add = s_elapsed_ms / 1000u;
        if (add > 65535u) add = 65535u;
        uint32_t t = (uint32_t)s_play_secs[index] + add;
        s_play_secs[index] = (uint16_t)(t > 65535u ? 65535u : t);
        s_sec_dirty = true;
    }
    if (!interrupt) {
        s_active = false;
        s_cur = -1;
        s_paused = false;
    }
    return interrupt;
}

static void audio_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t v = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &v, portMAX_DELAY) != pdTRUE) continue;
        for (;;) { // 打断命令链:PLAY 可能被新 PLAY/STOP 打断,继续处理
            const uint32_t cmd = v & CMD_MASK;
            if (cmd == CMD_PLAY || cmd == CMD_PLAY_ONCE || cmd == CMD_PLAY_LOOP) {
                s_once = (cmd == CMD_PLAY_ONCE);
                s_loop = (cmd == CMD_PLAY_LOOP);
                s_no_count = s_loop;  // 循环(闹钟响铃)不计入播放统计
                s_scene_len = 0;  // 单曲/单次/循环命令打断场景序列
                int idx = (int)(v & 0xFFFF);
                for (;;) {
                    v = play_index(idx);
                    if (v) break;           // 打断:命令交回外层处理
                    if (s_loop) continue;   // 循环:重播当前条(自然结束即重来)
                    if (!s_autoplay || s_once) break; // 自然结束:未开连播/单次不接棒
                    idx = next_index(idx);  // 连播:接下一条(顺序/随机)
                }
                if (v) continue;
                break;
            }
            if (cmd == CMD_PLAY_SCENE) {
                s_once = false;
                s_loop = false;
                s_no_count = false;
                int idx = s_scene_seq[s_scene_pos];
                for (;;) {
                    v = play_index(idx);
                    if (v) break;                       // 打断:命令交回外层处理
                    if (++s_scene_pos >= s_scene_len) break; // 序列尾:自动停
                    idx = s_scene_seq[s_scene_pos];     // 场景:按表接下一条
                }
                if (v) continue;
                break;
            }
            if (cmd == CMD_STOP) {
                s_active = false;
                s_cur = -1;
                s_paused = false;
                s_loop = false;
                break;
            }
            if (cmd == CMD_PAUSE) {
                if (s_cur >= 0) s_paused = !s_paused;
                break;
            }
            break;
        }
    }
}

esp_err_t app_audio_start(void) {
    if (s_task) return ESP_OK;
    settings_load();
    if (xTaskCreate(audio_task, "app_audio", 4096, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_audio_play(int index) {
    if (!s_task || !app_catalog(index)) return;
    xTaskNotify(s_task, CMD_PLAY | (uint32_t)(index & 0xFFFF),
                eSetValueWithOverwrite);
}

// 播一次(Quiz 题面音/答案回放):自然结束即停,连播开着也不接棒。
void app_audio_play_once(int index) {
    if (!s_task || !app_catalog(index)) return;
    xTaskNotify(s_task, CMD_PLAY_ONCE | (uint32_t)(index & 0xFFFF),
                eSetValueWithOverwrite);
}

// 场景序列播放(scenes.h 表):按表顺序播完自动停;打断一切在播(含场景/Quiz),
// 与 autoplay 开关无关(独立序列源)。seq 指向静态存储,存续期=程序全程。
void app_audio_play_scene(const int8_t *seq, int n) {
    if (!s_task || !seq || n <= 0) return;
    s_scene_seq = seq;
    s_scene_len = n;
    s_scene_pos = 0;
    xTaskNotify(s_task, CMD_PLAY_SCENE, eSetValueWithOverwrite);
}

// 单曲循环(闹钟响铃):自然结束重播当前条直到被打断;打断一切在播。
void app_audio_play_loop(int index) {
    if (!s_task || !app_catalog(index)) return;
    xTaskNotify(s_task, CMD_PLAY_LOOP | (uint32_t)(index & 0xFFFF),
                eSetValueWithOverwrite);
}

void app_audio_pause_resume(void) {
    if (!s_task) return;
    xTaskNotify(s_task, CMD_PAUSE, eSetValueWithOverwrite);
}

void app_audio_stop(void) {
    if (!s_task) return;
    xTaskNotify(s_task, CMD_STOP, eSetValueWithOverwrite);
}

// Flush pending volume/autoplay changes to NVS (call on settings-popup close).
void app_audio_settings_save(void) {
    if (!s_vol_dirty && !s_ap_dirty && !s_rnd_dirty && !s_lst_dirty) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, settings not saved");
        return;
    }
    if (s_vol_dirty) {
        if (nvs_set_u8(h, "vol", (uint8_t)s_vol) == ESP_OK) s_vol_dirty = false;
    }
    if (s_ap_dirty) {
        if (nvs_set_u8(h, "apm", s_autoplay ? 1u : 0u) == ESP_OK) s_ap_dirty = false;
    }
    if (s_rnd_dirty) {
        if (nvs_set_u8(h, "rnd", s_random ? 1u : 0u) == ESP_OK) s_rnd_dirty = false;
    }
    if (s_lst_dirty) {
        if (nvs_set_u8(h, "lst", s_listen ? 1u : 0u) == ESP_OK) s_lst_dirty = false;
    }
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "NVS commit failed: %s", esp_err_to_name(e));
}

void app_audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_vol = percent;
    s_vol_dirty = true;
    // Live update while an entry is playing (or paused); otherwise play_index()
    // applies s_vol at the next play. bsp clamps internally too, this is the
    // app-side guard for the int -> uint8_t conversion.
    if (s_active) bsp_audio_set_volume((uint8_t)percent);
}

int app_audio_volume(void) { return s_vol; }

// 关连播联动关聆听:聆听项只随连播出现,关掉连播后不能留着不可见却仍
// 生效的幽灵状态(NVS 一并清零)。
void app_audio_set_autoplay(bool on) {
    s_autoplay = on;
    s_ap_dirty = true;
    if (!on && s_listen) {
        s_listen = false;
        s_lst_dirty = true;
    }
}

bool app_audio_autoplay(void) { return s_autoplay; }

// 随机连播:仅连播接棒时生效(单曲播放与此无关)。默认开。
void app_audio_set_random(bool on) { s_random = on; s_rnd_dirty = true; }

bool app_audio_random(void) { return s_random; }

// 聆听模式:播放中闲置熄屏只灭背光不停播。默认关。
void app_audio_set_listen(bool on) { s_listen = on; s_lst_dirty = true; }

bool app_audio_listening(void) { return s_listen; }

// 播放计数(角标数据源)。落盘时机:熄屏/深睡前(app_audio_counts_save)。
unsigned app_audio_play_count(int index) {
    if (index < 0 || index >= (int)APP_CATALOG_COUNT) return 0;
    return s_counts[index];
}

// 每条目累计已播秒数(统计弹窗数据源;打断计已播,暂停冻结不计)。
unsigned app_audio_play_secs(int index) {
    if (index < 0 || index >= (int)APP_CATALOG_COUNT) return 0;
    return s_play_secs[index];
}

// Flush pending play counts to NVS (call on screen-off and before deep sleep).
void app_audio_counts_save(void) {
    if (!s_cnt_dirty && !s_sec_dirty) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, play counts not saved");
        return;
    }
    if (s_cnt_dirty) {
        if (nvs_set_blob(h, "cnt", s_counts, sizeof(s_counts)) == ESP_OK) {
            s_cnt_dirty = false;
        }
    }
    if (s_sec_dirty) {
        if (nvs_set_blob(h, "sec", s_play_secs, sizeof(s_play_secs)) == ESP_OK) {
            s_sec_dirty = false;
        }
    }
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "NVS commit failed: %s", esp_err_to_name(e));
}

bool app_audio_active(void) { return s_active; }
int app_audio_current(void) { return s_cur; }
bool app_audio_paused(void) { return s_paused; }
uint32_t app_audio_elapsed_ms(void) { return s_elapsed_ms; }
uint32_t app_audio_seq(void) { return s_seq; }

// NVS settings loaded (task started)? UI polls this once to sync widgets
// that were built before init_deferred ran (badges, volume indicator).
bool app_audio_started(void) { return s_task != NULL; }
