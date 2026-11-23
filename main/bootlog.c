// main/bootlog.c —— 重启历史环形日志的 NVS 持久化壳。
//
// blob `hist` = {count, head, rec[8]}(bootlog_t 直接落盘, 35B);启动时
// load(不存在则空)→ 迁移旧键(保证据)→ append 本次。旧散键 rr/wc/ps/rs
// (4e35ca0 协议)在 blob 不存在时 seed 一条, 否则弃读——升级首启不丢
// 「上次重启原因」这条正在等的电池调查证据。save 只写 blob, 旧键不回写。
#include "bootlog.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "bootlog";

#define BOOTLOG_NS  "bootdiag"
#define BOOTLOG_KEY "hist"
// 旧协议键(见 main.c boot_diag_init 旧实现/4e35ca0): 迁移 seed 用。
#define LEGACY_RR "rr"

static bootlog_t s_log;

static void bootlog_save(void);  // 定义在下文(touch_site 先用)

const bootlog_t *bootlog_snapshot(void) { return &s_log; }

// esp_reset_reason_t 短名(启动日志与诊断页共用; 越界 "?")。
const char *reset_reason_name(int r) {
    switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                return "?";
    }
}

// 运行起点(分钟, app_main 记录);touch 换算运行时长用。
static int64_t s_boot_us;

void bootlog_touch_site(uint8_t site) {
    // 断电截断的运行无人 touch;触点处按运行起点换算分钟(封顶 255)。
    int64_t min = (esp_timer_get_time() - s_boot_us) / (60LL * 1000 * 1000);
    bootlog_touch(&s_log, site,
                  min > 255 ? 255 : (min < 0 ? 0 : (uint8_t)min));
    bootlog_save();
}

static void bootlog_save(void) {
    nvs_handle_t h;
    if (nvs_open(BOOTLOG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_blob(h, BOOTLOG_KEY, &s_log, sizeof(s_log));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "bootlog 保存失败: %s", esp_err_to_name(e));
}

// 启动初始化:load(或迁移旧键 seed)→ 追加本次 → save。NVS 未就绪/读写失败
// 只影响历史(空日志运行), 不阻塞启动。
void bootlog_init(uint8_t rr, uint8_t wc) {
    s_boot_us = esp_timer_get_time();

    nvs_handle_t h;
    if (nvs_open(BOOTLOG_NS, NVS_READONLY, &h) == ESP_OK) {
        bootlog_t stored;
        size_t len = sizeof(stored);
        if (nvs_get_blob(h, BOOTLOG_KEY, &stored, &len) == ESP_OK &&
            len == sizeof(stored)) {
            s_log = stored;
        } else {
            // 迁移: blob 不存在而旧 rr 键在 → 用旧协议 seed 1 条(上次运行)。
            // ps(上上次站点)语义已并入环形, 弃之。
            uint8_t lrr = 0;
            if (nvs_get_u8(h, LEGACY_RR, &lrr) == ESP_OK) {
                uint8_t lwc = 0, lsite = 0;
                (void)nvs_get_u8(h, "wc", &lwc);
                (void)nvs_get_u8(h, "rs", &lsite);
                memset(&s_log, 0, sizeof(s_log));
                bootlog_append(&s_log, lrr, lwc, lsite, 0);  // 时长未知: 0=断电
            }
        }
        nvs_close(h);
    } else {
        memset(&s_log, 0, sizeof(s_log));
    }

    bootlog_append(&s_log, rr, wc, 0, 0);  // 本次运行(站点/时长待 touch)
    bootlog_save();
}
