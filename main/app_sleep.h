// main/app_sleep.h —— 深睡(软关机):闲置深睡与 GPWS 闹钟深睡共用序列。
#pragma once

// 执行深睡序列,正常情况不返回(唤醒=重启)。任一致命失败 esp_restart() 恢复。
// 必须从非 LVGL 任务调用(input_task):序列内部要拿 bsp_lvgl_lock。
// timer_min=0: 闲置深睡(GPIO 唤醒武装,任意键唤醒=正常亮屏)。
// timer_min>0: 闹钟深睡(TIMER 定时唤醒 + GPIO 并存武装: 按键=取消闹钟,
//              TIMER 到时=响铃; 见 main.c 唤醒分支)。
void app_sleep_now(int timer_min);
