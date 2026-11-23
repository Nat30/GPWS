// main/app_sleep.c —— 闲置自动深睡(软关机):序列平移自 demo_low_power.c 的
// SLEEP_COMMAND_DEEP 分支(去掉 demo UI 耦合),顺序与失败策略保持一致:
//   按键电压预检 → [亮屏时关机动画] → RTC 域全关 + GPIO 低电平唤醒武装
//   → LVGL 停刷 → CW2017 → ES8311 → I2S → 共享 I2C → ST7789 → esp_deep_sleep_start
// 唤醒 = 芯片重启,走 app_main 的 ESP_SLEEP_WAKEUP_GPIO 分支(key_guard
// 防误触 + 直开背光,无开机动画)。任一步非致命失败记日志继续;武装唤醒源
// 失败或按键交回失败则 esp_restart() —— 按键已不可用时留在运行态没有意义。
#include "app_sleep.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "app_audio.h"
#include "app_shutdown.h"
#include "app_ui.h"
#include "bootlog.h"

static const char *TAG = "app_sleep";

static void log_step(const char *step, esp_err_t e) {
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep 继续:%s 失败: %s", step, esp_err_to_name(e));
    }
}

// esp_restart 前把序列站点写入环形日志(bootlog 读回诊断):电池上的
// 序列失败重启无 USB 附件,靠下一次启动的"站点"定位失败步骤。NVS 不可用
// 则跳过(重启照常)。站点:1=GPIO 唤醒武装 2=按键交回 3=LVGL 锁 4=深睡意外
// 返回 5=TIMER 唤醒武装(闹钟)。
// esp_restart 前的站点标记:写入环形日志当前槽(bootlog)。旧的 NVS 散键
// 协议(rs 键)已并入 blob。
static void mark_restart_site(uint8_t site) {
    bootlog_touch_site(site);
}

void app_sleep_now(int timer_min) {
    // 三键共用 GPIO0(深睡唤醒脚):按住时低电平唤醒条件入睡即成立,会立刻
    // 醒回来——按键还活着时先按电压挡一次。
    int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGW(TAG, "按键被按住(%d mV),放弃本轮入睡", mv);
        return;  // 调用方继续轮询,下一轮计时由该按键的输入事件刷新
    }
    // 关机动画(仅亮屏关机,~1.05s):熄屏闲置深睡不播——黑屏 4min 无人看,
    // 多亮 1s 纯耗电;低电强睡时提醒已点亮屏幕,给可见的关机一个过渡。
    // 闹钟深睡(确认层刚在屏上显示 1s)走亮屏路径,播动画。
    // 放在按键预检之后:按住键不入睡,不白播;低电保护因此最多延后 ~1s
    // (4 拍 8s 去抖已接受,5% 电量下无实质风险)。
    if (!app_ui_screen_off()) app_shutdown_play();
    app_audio_counts_save();    // 深睡前落盘播放计数(NVS 在电源域下电前可用)
    // 强睡不走 screen_off/modal_close,弹窗里未落盘的音量/连播编辑在这里补
    // 一次落盘(设置值在 app_audio 全局里已生效,这里只管持久化)。
    app_audio_settings_save();

    // 闹钟:TIMER 定时唤醒武装(与下方 GPIO 武装并存——按键=取消,到时=响铃)。
    // 失败则重启:武装不了任何唤醒源的深睡等于假关机(与 GPIO 武装同语义)。
    if (timer_min > 0) {
        esp_err_t te = esp_sleep_enable_timer_wakeup(
            (uint64_t)timer_min * 60LL * 1000LL * 1000LL);
        if (te != ESP_OK) {
            ESP_LOGE(TAG, "TIMER 唤醒武装失败,重启恢复: %s",
                     esp_err_to_name(te));
            mark_restart_site(5);
            esp_restart();
        }
    }

    // GPIO 低电平唤醒(位掩码,非引号号)。失败则重启:武装不了唤醒源的
    // 深睡等于假关机,按键交回后也无法唤醒。
    esp_err_t e = esp_deep_sleep_enable_gpio_wakeup(
        1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "GPIO 唤醒武装失败,重启恢复: %s", esp_err_to_name(e));
        mark_restart_site(1);
        esp_restart();
    }

    // 软关机:关掉无需保留的电源域,电流降到本硬件可达下限(~μA)。GPIO
    // 唤醒所需的 RTC IO 域由驱动自动保留;C3 无 RTC slow mem,其余域按
    // SOC_PM_SUPPORT_* 能力自动下电,这里只显式关时钟源(失败非致命:
    // 域本身仍会按能力自动下电,记日志继续,与其余步骤一致)。
    log_step("XTAL power domain off",
             esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF));
    log_step("RC_FAST power domain off",
             esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_OFF));

    // 按键脚交回普通数字输入 + 上拉:ADC 接管时该脚数字电平读回 0,不恢复
    // 则唤醒条件入睡瞬间成立。交回是终端式操作,失败只能重启。
    int level = 0;
    e = bsp_button_prepare_deep_sleep(&level);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "按键交回失败,重启恢复外设: %s", esp_err_to_name(e));
        mark_restart_site(2);
        esp_restart();
    }
    if (level != 1) {
        // 电压预检后 250ms 竞态窗口内又按下:继续睡,立即醒回来等价于没睡。
        ESP_LOGW(TAG, "入睡前按键仍被按住,本次深睡可能立即返回");
    }

    // 先拿 LVGL 锁,再动任何 I2C 设备:taskLVGL 的电量 tick 与本序列共用
    // I2C 读 CW2017,若先睡电量计/解绑 SDA/SCL,tick 里每次读都退化成
    // 100ms 总线超时(重估链可叠到 ~230ms),原本 1s 的锁预算被吃穿就误
    // esp_restart()。持锁冻结 LVGL 后,I2C 上只剩本任务。持锁跨序列内的
    // 短 delay 无碍(mutex 阻塞让渡);锁不再释放——深睡不返回,失败路径
    // 直接重启。调用于 input_task 队列超时分支,不持 LVGL 锁,无死锁。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止 LVGL 刷屏,重启恢复外设");
        mark_restart_site(3);
        esp_restart();
    }

    // CW2017 与 ES8311 共用 I2C,电量计必须先完成写入/回读。
    log_step("CW2017 suspend", bsp_battery_sleep());
    log_step("ES8311 suspend", bsp_audio_sleep());
    // codec 寄存器失败也继续:停时钟、释放引脚照做。
    log_step("I2S pin release", bsp_audio_prepare_deep_sleep());
    log_step("shared I2C pin release", bsp_i2c_prepare_deep_sleep());
    log_step("ST7789 suspend", bsp_display_prepare_deep_sleep());

    // 深睡前大落账: 当前槽 site=0(深睡收尾) + 准确运行时长——深睡重启后
    // 历史行可区分「正常入睡」与「断电截断」。
    bootlog_touch_site(0);
    ESP_LOGI(TAG, "%s(软关机):%s",
             timer_min > 0 ? "闹钟深睡" : "闲置深睡",
             timer_min > 0 ? "按键取消闹钟,到时响铃" : "任意按键唤醒");
    esp_deep_sleep_start();
    // prepare 接口返回后总线在本次运行中不可恢复,意外返回只能重启。
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回,重启恢复外设");
    mark_restart_site(4);
    esp_restart();
}
