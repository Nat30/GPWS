// main/app_ui.h —— 「航空警示音」LVGL 界面:状态栏/页头/宫格瀑布流/滚动条/底栏。
//
// 所有接口在 LVGL 任务外调用时必须已持有 bsp_lvgl_lock()(main 的输入任务已持锁)。
// 纯几何计算在 app_layout(host 可测),本模块只做布局结果 -> LVGL 对象的映射。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_button.h"

struct _lv_obj_t;

// 构建 UI 并启动内部定时器(时钟/电量/播放进度/音波动画)。需持 LVGL 锁,调一次。
void app_ui_build(void);

// 矢量图标构造(draw_event 绘制,无位图资源)。icon_id: app_catalog.h 的
// APP_ICON_*。供 app_boot 开机动画复用同一套图标绘制。
struct _lv_obj_t *app_ui_icon_create(struct _lv_obj_t *parent, int icon_id,
                                     int x, int y, int w, int h, uint32_t color);

// 焦点上移/下移一格(到顶/到底停,触发滚动使焦点完整可见)。
void app_ui_move_focus(int dir);

// 确认键:无播放->播焦点条目;正在播焦点条目->暂停/继续;播其他条目->切换。
void app_ui_toggle_play(void);

// ↑/↓ 长按:启动 120ms/格连续滚动;松手(RELEASE)停止。
void app_ui_key_repeat_start(int dir);
void app_ui_key_repeat_stop(void);

// True once the idle deep-sleep deadline has been reached (screen off and no
// input for AUTO_SLEEP_US). Sticky until consumed; the caller clears it via
// app_ui_sleep_due_ack() before running the sleep sequence.
bool app_ui_sleep_due(void);
void app_ui_sleep_due_ack(void);

// ---- GPWS 闹钟 ----
// TIMER 唤醒后进入响铃态(单曲循环 + 波形呼吸 + 任意键停止)。需持 LVGL 锁。
void app_ui_alarm_ring(int tune);
// 闹钟确认层满 1s: input_task 轮询到即执行闹钟深睡(ack 清标志);
// 分钟数由 app_ui_alarm_minutes() 读(确认时定格)。
bool app_ui_alarm_due(void);
void app_ui_alarm_due_ack(void);
int app_ui_alarm_minutes(void);

// 亮屏状态(供关机动画判定:仅亮屏关机播动画,见 app_shutdown.h)。
bool app_ui_screen_off(void);

// 统一输入入口(main 持 LVGL 锁后先调,返回 true=已消费不再分发)。处理:
//   关屏唤醒(吞唤醒键尾随事件)/OK 双击关屏/设置弹窗(音量·连播)/OK 长按开弹窗。
bool app_ui_input(bsp_btn_t btn, bsp_btn_ev_t ev);
