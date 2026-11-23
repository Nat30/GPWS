// main/main.c —— 「航空警示音」入口:初始化外设与 UI,分发三键输入到 app_ui。//
// 输入路径与上游 demo 一致(回调只入队,工作线程持 LVGL 锁处理):
//   bsp_button 回调(esp_timer 任务) -> input queue -> input_task(本文件)
//   -> app_ui_move_focus / toggle_play / stop / key_repeat。
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "app_audio.h"
#include "app_boot.h"
#include "app_sleep.h"
#include "app_ui.h"
#include "bootlog.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "lvgl.h"

static const char *TAG = "app";

// 唤醒防误触窗口上限:兜底卡死的唤醒键(状态机在 key_guard.h)。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
    int mv;  // 触发本次事件的共享采样均值(判窗依据;quiz 按键判读调查用)
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;  // UI 构建完成前丢弃输入

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user_data) {
    (void)user_data;
    // 深睡唤醒后的防误触窗口(见 key_guard.h)。
    key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time());
    if (!s_input_ready) return;
    input_event_t ie = {.btn = btn, .ev = ev, .mv = bsp_button_last_mv()};
    xQueueSend(s_input_queue, &ie, 0);  // 只入队,绝不阻塞回调
}

// 事件名(日志用;按键调查:PRESS 行的 mv = 判窗依据,CLICK 行 = 抬键电压)。
static const char *BTN_NAME[BSP_BTN_COUNT] = {"UP", "DOWN", "OK"};
static const char *EV_NAME[] = {"press", "click", "double", "long", "release"};

static void process_input(void) {
    input_event_t ie;
    // 500ms 超时轮询:闲置/闹钟深睡到期检查(不落在 LVGL 任务,序列内部要拿锁)。
    if (xQueueReceive(s_input_queue, &ie, pdMS_TO_TICKS(500)) != pdTRUE) {
        // 闹钟确认流程到期优先(确认层刚满 1s,深睡路径不含闲置前置条件)。
        if (app_ui_alarm_due()) {
            app_ui_alarm_due_ack();
            app_sleep_now(app_ui_alarm_minutes());  // 按住键时返回,下轮重计
            return;
        }
        if (app_ui_sleep_due()) {
            app_ui_sleep_due_ack();
            app_sleep_now(0);  // 按键被按住时返回,下一轮重计
        }
        return;
    }

    if (!bsp_lvgl_lock(500)) return;

    // 逐事件日志(quiz 按键判读调查):PRESS 行的 mv 就是解码器判窗依据,
    // 换键/双事件序列在此一目了然。常驻:USB 不接时无开销,接上即是证据。
    ESP_LOGI(TAG, "input: %s %s mv=%d",
             BTN_NAME[ie.btn], EV_NAME[ie.ev], ie.mv);

    // 统一入口先走 app_ui(关屏/唤醒/设置弹窗);未消费才落到浏览/播放分发。
    if (app_ui_input(ie.btn, ie.ev)) {
        bsp_lvgl_unlock();
        return;
    }

    switch (ie.btn) {
    case BSP_BTN_UP:
    case BSP_BTN_DOWN: {
        int dir = (ie.btn == BSP_BTN_UP) ? -1 : 1;
        if (ie.ev == BSP_BTN_CLICK) {
            app_ui_move_focus(dir);
        } else if (ie.ev == BSP_BTN_DOUBLE) {
            // iot_button 双击聚合吞掉了第一击的 SINGLE_CLICK,补齐两格。
            app_ui_move_focus(dir);
            app_ui_move_focus(dir);
        } else if (ie.ev == BSP_BTN_LONG) {
            app_ui_key_repeat_start(dir);  // 长按连续滚动 120ms/格
        } else if (ie.ev == BSP_BTN_RELEASE) {
            app_ui_key_repeat_stop();
        }
        break;
    }
    case BSP_BTN_OK:
        if (ie.ev == BSP_BTN_CLICK) {
            app_ui_toggle_play();  // 播放/暂停/继续/切换条目
        }
        // OK 长按已移至 app_ui_input(开设置弹窗)。
        break;
    default:
        break;
    }

    bsp_lvgl_unlock();
}

static void input_task(void *arg) {
    (void)arg;
    for (;;) process_input();
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(input_task, "input", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static esp_err_t init_all(bool woke_from_deep_sleep) {
    // I2C 是 codec/电量计的共享总线;单项失败只记日志(与上游 demo 一致)。
    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C 初始化失败: %s", esp_err_to_name(e));
    } else if (!woke_from_deep_sleep) {
        bsp_i2c_scan();  // 诊断日志,仅冷启动跑(唤醒路径省扫描时间)
    }

    // 屏幕是 UI 唯一载体,失败就没有应用可言。
    // 注意 bsp_lvgl_init 返回 display 指针(非 NULL 即成功),不是 esp_err_t。
    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return ESP_FAIL;
    }
    // 背光不再这里直开:冷启动由开机动画在首帧上屏后渐亮(app_boot_play),
    // 深睡唤醒在 app_main 里直开——避免"背光已亮而首帧未到"的白屏窗口。

    // 其余外设单项失败不阻塞:屏幕必须出来,哪里坏了哪里记日志。
    if (input_dispatch_init() != ESP_OK) {
        ESP_LOGE(TAG, "输入分发任务创建失败");
    } else if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    } else {
        // 唤醒后按键驱动就绪:若物理键已松手则同步解除防误触窗口。
        key_guard_sync_released(&s_key_guard,
                                bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    }
    return ESP_OK;
}

// 音频与电量计对首帧无贡献(电量读失败时 UI 保持占位后自愈,CW2017 唤醒
// 重算 SOC 最坏可等数秒),放到首帧上屏之后、接受输入之前初始化,
// 缩短深睡唤醒到主 UI 出现的关键路径。
static void init_deferred(void) {
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败,播放将不可用");
    } else if (app_audio_start() != ESP_OK) {
        ESP_LOGE(TAG, "播放任务创建失败");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGE(TAG, "电量计初始化失败,电量显示保持占位");
    }
}

// 复位原因可读名在 bootlog.c(reset_reason_name, 与诊断页共用):
// 上电即打日志,自动重启复现时串口首行即可判别 PANIC(栈溢出/异常)/
// SW(深睡序列 esp_restart)/ BROWNOUT / DEEPSLEEP(正常唤醒),
// 不再靠有无开机动画反推。

// ---------- 启动诊断:重启历史环形日志 ----------
// 电池供电下发生的重启(无 USB 附件)抓不到启动首行——每次启动把复位原因/
// 唤醒源/站点/时长记入环形日志(bootlog, NVS blob `hist`),诊断页可查;
// 这里仍保留 4s 重复启动行(USB 枚举延迟 ~1.5s 吃掉首行),来源改环形日志。
#define DIAG_REPEAT_MS 500
#define DIAG_REPEAT_N  8   // 重复 4s:覆盖 USB 枚举延迟(~1.5s 吃掉首行)

static esp_timer_handle_t s_diag_timer;
static const char *s_diag_line;
static int s_diag_left;

static void diag_repeat_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "%s", s_diag_line);
    if (--s_diag_left > 0) return;
    esp_timer_stop(s_diag_timer);
    esp_timer_delete(s_diag_timer);
    s_diag_timer = NULL;
}

// NVS 就绪后调用;失败只影响诊断,静默降级(bootlog 空日志运行)。
static void boot_diag_init(esp_sleep_wakeup_cause_t cause) {
    esp_reset_reason_t rr = esp_reset_reason();
    // 追加本次记录(含旧散键 rr/wc/ps/rs → blob 的迁移, 保证据, 见 bootlog.c)。
    bootlog_init((uint8_t)rr, (uint8_t)cause);

    bootlog_rec_t prev;
    static char buf[128];
    if (bootlog_get(bootlog_snapshot(), 1, &prev)) {
        snprintf(buf, sizeof(buf),
                 "启动诊断: 本次=%s 唤醒源=%d | 上次=%s 唤醒源=%d 站点=%d", // glyph-check: ignore — log-only
                 reset_reason_name(rr), (int)cause,
                 reset_reason_name((esp_reset_reason_t)prev.rr), // glyph-check: ignore
                 (int)prev.wc, (int)prev.site);
    } else {
        snprintf(buf, sizeof(buf),
                 "启动诊断: 本次=%s 唤醒源=%d | 上次=无记录", // glyph-check: ignore — log-only
                 reset_reason_name(rr), (int)cause);
    }
    s_diag_line = buf;
    ESP_LOGI(TAG, "%s", s_diag_line);
    const esp_timer_create_args_t t = {
        .callback = diag_repeat_cb, .name = "bootdiag",
    };
    s_diag_left = DIAG_REPEAT_N;
    if (esp_timer_create(&t, &s_diag_timer) == ESP_OK) {
        esp_timer_start_periodic(s_diag_timer, DIAG_REPEAT_MS * 1000);
    }
}

// ---------- GPWS 闹钟唤醒(NVS: namespace gpws_app, UI 确认时写入) ----------
// 闹钟值跨深睡的唯一载体是 NVS(深睡=RAM 失忆)。UI 确认时写 alarm_min/
// alarm_tune;唤醒分支读回并清除——闹钟只在确认后有效一次,GPIO 唤醒(按键=
// 取消)与 TIMER 唤醒(响铃)都不保留。
#define ALARM_NS       "gpws_app"
#define ALARM_KEY_MIN  "alarm_min"
#define ALARM_KEY_TUNE "alarm_tune"

// 唤醒时消费闹钟武装值(读回并清除)。tune==NULL 只清不读(GPIO=取消)。
// 返回 true=TIMER 唤醒且有有效武装值(min>0 且 tune 在域)→ 该响铃。
static bool alarm_wakeup_consume(uint8_t *tune) {
    nvs_handle_t h;
    if (nvs_open(ALARM_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t min = 0, t = 0;
    bool have = nvs_get_u8(h, ALARM_KEY_MIN, &min) == ESP_OK && min > 0 &&
                nvs_get_u8(h, ALARM_KEY_TUNE, &t) == ESP_OK && t < 24;
    nvs_erase_key(h, ALARM_KEY_MIN);
    nvs_commit(h);
    nvs_close(h);
    if (have && tune) *tune = t;
    return have;
}

void app_main(void) {
    // GPIO 唤醒(非上电复位)时启用防误触窗口。TIMER 唤醒(闹钟到时=响铃)
    // 无按键按住,不 arm;响铃态的任意键停止在 app_ui_input 的响铃块处理。
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "复位原因=%s 唤醒源=%d",
             reset_reason_name(esp_reset_reason()), (int)cause);
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // NVS 先于 app_audio_start(音量/连播从 NVS 加载;失败只影响持久化)。
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_err = nvs_flash_erase();
        if (nvs_err == ESP_OK) nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败,设置不持久化: %s",
                 esp_err_to_name(nvs_err));
    }
    boot_diag_init(cause);  // 上次复位原因读回+本次存档(电池重启事后可查)

    // 闹钟唤醒分流(须在 NVS 就绪后):TIMER=读回武装值(有效则响铃,无效走
    // 正常亮屏);GPIO=取消(清残留武装值,正常唤醒路径)。
    bool alarm_ring = false;
    uint8_t alarm_tune = 0;
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        alarm_ring = alarm_wakeup_consume(&alarm_tune);
    } else if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        (void)alarm_wakeup_consume(NULL);  // 按键=取消闹钟
    }

    bool woke = (cause == ESP_SLEEP_WAKEUP_GPIO ||
                 cause == ESP_SLEEP_WAKEUP_TIMER);
    if (init_all(woke) != ESP_OK) return;

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock timeout");
        return;
    }
    app_ui_build();
    // 冷启动/软重启:开机动画(内部先刷首帧再渐亮背光);深睡唤醒(GPIO 闲置
    // 唤醒 / TIMER 闹钟响铃):直开不播。
    if (woke) {
        lv_refr_now(NULL);  // render first frame before backlight on: no white flash
        bsp_display_backlight(100);
    } else {
        app_boot_play();
    }
    bsp_lvgl_unlock();

    // 首帧已上屏:再初始化音频/电量计(CW2017 唤醒重算 SOC 最坏数秒),
    // 完成后才接受输入——期间按键在 on_key 里被 s_input_ready 门直接丢弃
    // (并不入队),含开机动画的"任意键跳过":deferred init 慢时(电量计
    // 软降级最长 ~5s)该跳过会静默失效。冷启动 SOC 通常很快 ready,实际
    // 影响很小;改"先入队、就绪后再处理"收益场景多为理论值,故不为此
    // 加复杂度。
    init_deferred();

    // TIMER 唤醒且武装值有效:进入响铃态(单曲循环 + 任意键停止)。
    // 音频在 init_deferred 就绪后才可响铃;需持 LVGL 锁调 UI。
    if (alarm_ring) {
        if (bsp_lvgl_lock(1000)) {
            app_ui_alarm_ring(alarm_tune);
            bsp_lvgl_unlock();
        }
    }

    s_input_ready = true;
}
