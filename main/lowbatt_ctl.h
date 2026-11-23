// main/lowbatt_ctl.h —— 低电保护决策状态机(纯逻辑,host 可测)。
//
// 输入是每 2s 一拍的 (soc, mv) 读数与"设置/统计弹窗是否开着",输出是本拍
// 动作(提醒/强睡)+ 就地推进的去抖与提醒闩状态;何时弹提醒遮罩、何时置
// 深睡到期标志由 app_ui 落地。读数失败(soc<0)由调用方先拦(整个保护不
// 生效),mv<0(电压读失败)保守按真低电计——真低电叠加读坏仍要保护。
#pragma once

#include <stdbool.h>

// 低电保护阈值(SOC %):≤10 提醒(每次放电会话一次)、≤5 强制深睡
// (电芯保护优先,即使正在播放/弹窗开着)、≥15 回弹重置提醒。
#define LOW_BATT_WARN_SOC  10
#define LOW_BATT_SLEEP_SOC 5
#define LOW_BATT_REARM_SOC 15
// 低电动作去抖拍数(tick 每 2s 一拍):CW2017 深睡唤醒 RESTART 后 SOC 从零
// 重算,插着 USB 唤醒时可能超过 bsp 的 5s 等待窗(见 bsp_battery.c 软降级
// 注释)——连续 4 拍(8s)低读数才确认,覆盖已知重算窗口;真低电的动作
// (提醒/强睡)最多延后 6s,电量 5% 下无实质风险。
#define LOW_BATT_CONFIRM_POLLS 4
// 低读数电压佐证阈值(mV):真 SOC≤10% 的电芯电压 ≤~3.65V(0% 截止 ~3.3V),
// 而 RESTART 重算窗口的假 0% 电压健康(3.7-4.2V)。soc 低 + 电压健康 =
// 自相矛盾伪像(与满电钉死「soc 高 + 电压低」对称),不计数也不显示。
#define LOW_BATT_MV_CORROBORATE 3700

typedef enum {
    LOWBATT_NONE = 0,
    LOWBATT_WARN,   // 弹低电提醒(每次放电会话一次,弹窗开着则延迟)
    LOWBATT_SLEEP,  // 置深睡到期(强睡:不受弹窗/播放状态门限)
} lowbatt_action_t;

typedef struct {
    int  low_streak;  // 连续真低读数拍数(去抖,见 LOW_BATT_CONFIRM_POLLS)
    bool warned;      // 本放电会话已提醒(回弹 ≥15 前不重复)
} lowbatt_ctl_t;

// 喂一拍读数,返回本拍动作并推进状态。modal_busy=设置/统计弹窗开着:
// 只延迟提醒(下一拍复查),从不延迟强睡。
static inline lowbatt_action_t lowbatt_ctl_poll(lowbatt_ctl_t *m, int soc, int mv,
                                                bool modal_busy) {
    if (soc <= LOW_BATT_WARN_SOC) {
        // 伪像拍必须整体跳过(不累加/不清零 streak):重算窗口的假 0%
        // (电压健康)若掉出本分支,会 fall-through 直达强睡,去抖被整个
        // 绕过——插 USB 深睡唤醒后循环自动深睡即此回归(见 ff58464)。
        if (mv >= 0 && mv >= LOW_BATT_MV_CORROBORATE) return LOWBATT_NONE;
        if (m->low_streak < LOW_BATT_CONFIRM_POLLS) {
            m->low_streak++;
            if (m->low_streak < LOW_BATT_CONFIRM_POLLS) return LOWBATT_NONE;
        }
    } else {
        m->low_streak = 0;
    }
    if (soc <= LOW_BATT_SLEEP_SOC) return LOWBATT_SLEEP;  // 强睡在途,提醒无意义
    if (m->warned) {
        if (soc >= LOW_BATT_REARM_SOC) m->warned = false;
        return LOWBATT_NONE;
    }
    if (soc <= LOW_BATT_WARN_SOC && !modal_busy) {
        m->warned = true;
        return LOWBATT_WARN;
    }
    return LOWBATT_NONE;
}
