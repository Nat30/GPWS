// components/bsp/src/bsp_i2c.c
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_bus;

// 冷启动扫描缓存(诊断页展示):最多记 8 个应答地址 + 是否中止/已扫。
#define BSP_I2C_SCAN_MAX 8
static uint8_t s_scan_addrs[BSP_I2C_SCAN_MAX];
static int s_scan_cnt;
static bool s_scan_aborted, s_scan_done;

esp_err_t bsp_i2c_init(void) {
    if (s_bus) return ESP_OK;                 // 幂等
    i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&cfg, &s_bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线创建失败 (%s) —— 检查 SDA=GPIO%d / SCL=GPIO%d 是否被别的外设占用",
                 esp_err_to_name(e), BSP_I2C_SDA, BSP_I2C_SCL);
        s_bus = NULL;
        return e;
    }
    ESP_LOGI(TAG, "I2C 就绪 SDA=GPIO%d SCL=GPIO%d", BSP_I2C_SDA, BSP_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_bus; }

esp_err_t bsp_i2c_scan(void) {
    // ⚠ 千万别为了"隔离 NACK"另开一条临时总线扫描 —— 那会把整条 I2C 打死:
    //   同一个 port 二次 i2c_new_master_bus() 必然失败,但 i2c_common.c 里
    //   "已被 acquire" 的分支仍会把【正式总线】的 bus 对象从 *i2c_new_bus 交出去;
    //   紧接着 err 清理路径 i2c_master_bus_destroy() 拿着它调 i2c_common_deinit_pins(),
    //   把 SDA/SCL 从 I2C 外设上解绑。此后每次事务都卡在 "clear bus failed",
    //   ES8311 与 CW2017 一起失联。
    //   (IDF v5.5.3 实测;症状是满屏 reset hardware failed,极难反查到扫描这一步。)
    //
    // 正式总线上直接扫是安全的:i2c_master_probe() 收尾会把 status 复位成
    // I2C_STATUS_DONE("in order not influence next time transaction"),NACK 不留残留;
    // 它临时设的 100kHz 时序也会被下一次设备事务按各自的 scl_speed_hz 覆盖。
    if (!s_bus) {
        ESP_LOGE(TAG, "请先成功调用 bsp_i2c_init()");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "I2C 扫描开始:");
    int found = 0, stuck_timeouts = 0;
    s_scan_cnt = 0;            // 扫描缓存清零(诊断页展示用)
    s_scan_aborted = false;
    s_scan_done = true;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t e = i2c_master_probe(s_bus, addr, 50);
        if (e == ESP_OK) {
            const char *who = (addr == BSP_I2C_ES8311_ADDR) ? "  <- ES8311 音频 codec"
                            : (addr == BSP_I2C_CW2017_ADDR) ? "  <- CW2017 电量计"
                            : "";
            ESP_LOGI(TAG, "  发现设备 @ 0x%02X%s", addr, who);
            if (s_scan_cnt < BSP_I2C_SCAN_MAX) s_scan_addrs[s_scan_cnt++] = addr;
            found++;
            stuck_timeouts = 0;
        } else if (e == ESP_ERR_TIMEOUT) {
            // 总线卡死(SDA 被拉低)时每个地址都吃满 50ms 超时,全表扫完要
            // 5.6s——这里在冷启动首帧之前。NACK(未应答)不算,只有连续
            // 超时才判卡死。
            if (++stuck_timeouts >= 8) {
                ESP_LOGE(TAG, "I2C 总线疑似卡死(连续 %d 次探测超时),中止扫描",
                         stuck_timeouts);
                s_scan_aborted = true;
                return ESP_ERR_TIMEOUT;
            }
        } else {
            stuck_timeouts = 0;
        }
    }
    if (found == 0) ESP_LOGW(TAG, "  未发现任何 I2C 设备 —— 检查接线、上拉电阻与供电");
    else            ESP_LOGI(TAG, "I2C 扫描完成,共 %d 个设备", found);
    return ESP_OK;
}

void bsp_i2c_scan_cache(const uint8_t **addrs, int *count, bool *aborted,
                        bool *done) {
    *addrs = s_scan_addrs;
    *count = s_scan_cnt;
    *aborted = s_scan_aborted;
    *done = s_scan_done;
}

esp_err_t bsp_i2c_prepare_deep_sleep(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BSP_I2C_SDA) | (1ULL << BSP_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C SDA/SCL 高阻配置失败: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "I2C SDA/SCL 已切换为高阻；外部上拉功耗仍由硬件决定");
    return ESP_OK;
}
