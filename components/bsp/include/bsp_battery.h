// components/bsp/include/bsp_battery.h
// CellWise CW2017 电量计:I2C 0x63,与 ES8311 共用总线。
// BSP 写入自定义电池 profile,芯片直接给 SOC%,无需外部分压电阻与查表。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 初始化。内部会调 bsp_i2c_init()(幂等)。
// 芯片不应答时返回 ESP_ERR_NOT_FOUND —— 上层可据此在 UI 上标记该项不可用。
esp_err_t bsp_battery_init(void);

// 将 CW2017 CONFIG 写为睡眠值并回读确认，失败时重试一次。这是 deep
// sleep 前的终端操作；芯片未初始化时视为无需处理并返回成功。
esp_err_t bsp_battery_sleep(void);

// 剩余电量百分比 0..100;读失败返回 -1。
int bsp_battery_soc(void);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);

// CW2017 版本寄存器值(0x00, 上电应答即代表芯片在位);读失败返回 -1。
// 诊断页展示用——不参与任何控制逻辑。
int bsp_battery_version(void);

// 满电矛盾检测:报 ≥99% 却 <4100 mV(真满电拔电静置 ≥4.15V,充电中端电压
// >4.15V)——CW2017 的 OCV 映射自相矛盾,读数已冻结在 100%(典型场景:
// 插 USB 刷机/充电把电压顶满后拔电,ACTIVE 态芯片不会自动重估)。
bool bsp_battery_full_stuck(int soc, int mv);

// 满电钉死恢复:按芯片规定时序 RESTART→ACTIVE 触发 CW2017 以当前电压
// 重新 OCV 估值(与深睡唤醒路径同款操作)。~31ms 阻塞,低频调用。
esp_err_t bsp_battery_recalibrate(void);
