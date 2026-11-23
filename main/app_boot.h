// main/app_boot.h —— 冷启动开机动画(约 1s + 160ms 交叉淡出,可按键跳过)。
//
// 仅冷启动/软重启播放;深睡唤醒不播(main.c 按 wakeup cause 决定)。
// 播放期间 app_ui_input() 吞掉一切按键;首个按键事件即触发跳过。
// 动画对象全部挂在覆盖层子树上:删除覆盖层会自动清掉绑定其上的全部
// lv_anim,无需逐个追踪。需持 LVGL 锁调用。
#pragma once

#include <stdbool.h>

// 在当前活动屏之上建覆盖层,同步刷出首帧(黑)后再渐亮背光,启动关键帧动画。
// 重复调用(动画未结束)忽略。
void app_boot_play(void);

// 动画进行中(含交叉淡出段)返回 true。
bool app_boot_active(void);

// 立即结束动画并删除覆盖层(任意键触发,由 app_ui_input 调用)。
void app_boot_skip(void);
