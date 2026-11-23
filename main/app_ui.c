// main/app_ui.c —— 「航空警示音」界面:还原设计稿三屏(默认浏览/焦点选中/播放中)。
//
// 布局几何全部来自 app_layout(host 已测);本文件只做 LVGL 对象映射与动效:
//  - 状态栏:时间(9:41 基准+真实分钟推进)/飞机标识/For AvGeeks/Wi-Fi/电量
//  - 页头:标题 + 副标题三态(焦点态/播放态复用同一行)
//  - 宫格瀑布流:两列错位 12,卡片 72/96 两档,焦点描边+发光,播放遮罩+7 段音波
//  - 底栏:浏览提示 <-> 播放进度条 两态切换
// 图标均为 draw_event 矢量绘制(viewBox 24 映射到控件尺寸),无位图资源。
#include "app_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nvs.h"

#include "app_audio.h"
#include "app_boot.h"
#include "app_catalog.h"
#include "app_layout.h"
#include "bootlog.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "key_guard.h"
#include "lowbatt_ctl.h"
#include "alarm_ctl.h"
#include "menu_ctl.h"
#include "modal_ctl.h"
#include "quiz_ctl.h"
#include "scenes.h"

LV_FONT_DECLARE(app_font_15_bold);
LV_FONT_DECLARE(app_font_10_bold);
LV_FONT_DECLARE(app_font_12);
LV_FONT_DECLARE(app_font_10);
LV_FONT_DECLARE(app_font_6_bold);

// ---------- 调色板(0xRRGGBB) ----------
#define COL_CANVAS     0x0D1218
#define COL_CARD       0x161E28
#define COL_CARD_FOCUS 0x1D2734
#define COL_LINE       0x232C39
#define COL_TRACK      0x232E3F
#define COL_INK        0xE6EDF6
#define COL_INK2       0x8492A6
#define COL_INK3       0x7C8DA3
#define COL_MASK_BG    0x0A0E14
#define COL_MASK_OPA   214   // 84%
#define COL_SB_TRACK   0x1B2433
#define COL_SB_THUMB   0x3B4A61
#define COL_BOTTOM_BG  0x0A0F15
#define COL_BOTTOM_LN  0x1A2330
#define COL_KEYCAP_BG  0x1D2734
#define COL_KEYCAP_LN  0x31405A
#define COL_PAUSE_BG   0x2A1B24
#define COL_PAUSE_LN   0x6E3550
#define COL_PAUSE_ICON 0xFF9DC6
#define COL_BATT_BODY  0x10161F
#define COL_BATT_FILL  0xADBCCB
#define COL_BATT_LOW   0xFF4D4F   // SOC < 20%: fill turns red
#define COL_MODAL_EDIT 0x35C6E0   // settings popup: editing-row border
#define COL_MEDAL_GOLD   0xF5C542  // stats leaderboard: gold medal
#define COL_MEDAL_SILVER 0xC8D2DC  // stats leaderboard: silver medal
#define COL_MEDAL_BRONZE 0xC98A5E  // stats leaderboard: bronze medal
#define COL_RIGHT        0x3FCF6E  // quiz: correct-option border
#define COL_RIGHT_BG     0x142A1E  // quiz: correct-option bg(答错行的红沿用 COL_PAUSE_BG)

// 播放遮罩 7 段音波:基础高度/相位差/周期(Demo .wave)。
static const int WAVE_BASE[7] = {11, 21, 33, 25, 17, 9, 14};
#define WAVE_PHASE_MS 95
#define WAVE_PERIOD_MS 900
// 96 档卡片底部迷你静态音波(Demo .mini)。
static const int MINI_H[9] = {5, 9, 14, 7, 11, 5, 9, 12, 6};

#define N_CARDS ((int)APP_CATALOG_COUNT)  // 条目数(编译期与 app_catalog 锁定)

// 低电保护阈值/去抖/佐证:已移入 lowbatt_ctl.h(纯决策,host 可测)。

// 满电钉死检测:插 USB 刷机/充电(电压被顶满,OCV 映射 100%)后 esp_restart
// 不断电,CW2017 无 RESTART 机会,拔电后读数钉死 100%(ACTIVE 态不自动重估;
// 深睡唤醒路径才有 RESTART,故"深睡一次就校准过来")。连续 FULL_STUCK_*
// 拍矛盾读数(报 ≥99% 却 <4100mV)→ RESTART 重估;冷却拍数防重估后首拍
// 仍矛盾时连环重启(重估 ~1s 出新值,冷却 60s 远超之)。
#define FULL_STUCK_CONFIRM_POLLS  5
#define FULL_STUCK_COOLDOWN_POLLS 30

// ---------- 状态 ----------
static lv_obj_t *s_cards[APP_CATALOG_COUNT];
static lv_obj_t *s_badges[APP_CATALOG_COUNT];  // 播放计数角标(0=隐藏)
static lv_obj_t *s_masks[APP_CATALOG_COUNT];
static lv_obj_t *s_mask_labels[APP_CATALOG_COUNT];
static lv_obj_t *s_wave_bars[APP_CATALOG_COUNT][7];
static lv_obj_t *s_content, *s_sb_track, *s_sb_thumb;
static lv_obj_t *s_sub, *s_batt_fill, *s_batt_label;
static lv_obj_t *s_vol_label;      // 标题行音量数字(变更才写)
static lv_obj_t *s_vol_icon;       // 标题行音量图标(随数字宽度动态摆位)
static int s_shown_vol = -1;       // 已呈现的音量(app_audio_volume;-1 保证首刷)
static lv_obj_t *s_browsebar, *s_playbar, *s_cur_label, *s_tot_label, *s_track, *s_fill;
static app_layout_t s_lay;
static int s_focus;
static int s_play_idx = -1;      // -1=未播放
static uint32_t s_seq_shown;     // 已呈现的播放序号(app_audio_seq;连播跟随用)
static bool s_paused;
static int s_scroll;
static lv_timer_t *s_repeat_timer;
static int s_repeat_dir;
static int s_shown_soc = 82;     // 已渲染的电量(Demo 初值 82)
static int s_tick_count;
static bool s_state_synced;      // 开机一次性状态同步完成(app_audio_started 后)
static int64_t s_last_ok_press_us;  // 应用层双击检测:两次 OK PRESS 的间隔
static int64_t s_screen_off_us;     // 关屏时刻:吞掉关屏手势的尾随事件
static int64_t s_swallow_until_us;  // 尾随 CLICK 吞键窗(响铃停止/低电提醒/唤醒手势用)
static int64_t s_last_active_us;    // 最近一次 tick 观测到播放活跃的时刻
static volatile bool s_sleep_due;   // 闲置深睡到期(input_task 轮询消费)

// 低电提醒遮罩(s_lowbatt_show_us 非零=弹出中):任意键继续,15s 自动关。
// 决策状态(streak/提醒闩)在 s_lowbatt_ctl(lowbatt_ctl.h),此处仅遮罩呈现。
static lowbatt_ctl_t s_lowbatt_ctl;   // 低电去抖/提醒闩(lowbatt_ctl.h 纯逻辑)
static int s_fullstuck_streak;      // 连续满电矛盾读数拍数(见 FULL_STUCK_*)
static int s_fullstuck_cooldown;    // 重估后冷却倒计时(拍)
static int64_t s_lowbatt_show_us;   // 提醒弹出时刻(0=未弹出)
static lv_obj_t *s_lowbatt_scrim;   // 提醒遮罩(懒构建一次)
static lv_obj_t *s_lowbatt_pct;     // SOC 百分比文本

// ---------- 关屏/设置弹窗 ----------
static bool s_screen_off;        // 背光熄灭(播放/渲染照常,仅不可见)
static key_guard_t s_wake_guard; // 唤醒守卫:吞掉唤醒键按住期间的事件(600ms 兜底)
static int64_t s_last_input_us;  // 最近一次按键事件(闲置计时基准;0=未开始计)
static modal_ctl_t s_modal;      // 设置弹窗导航状态(值存在 app_audio)
static lv_obj_t *s_modal_scrim;  // 弹窗懒构建一次,关=整层隐藏(子对象一起收)
static lv_obj_t *s_modal_panel;  // 弹窗面板(高度随可见行数自适应,modal_refresh)
static lv_obj_t *s_modal_rows[MODAL_ITEM_COUNT];
static lv_obj_t *s_modal_vol_segs[10];  // 音量 10 段条
static lv_obj_t *s_modal_ap_label;      // 连播值:「开」/「关」
static lv_obj_t *s_modal_rnd_label;     // 随机值:「开」/「关」(随连播显隐)
static lv_obj_t *s_modal_lst_label;     // 聆听值:「开」/「关」(随连播显隐)
static lv_obj_t *s_modal_hint;          // 底部操作提示(浏览/编辑两态文案)

// 主菜单(OK 长按入口, 状态机 menu_ctl.h; UI 段在下文统计弹窗段之后)。
static menu_ctl_t s_menu;
static lv_obj_t *s_menu_scrim, *s_menu_panel;
static lv_obj_t *s_menu_rows[MENU_ITEM_COUNT];

// 硬件诊断页(只读快照, stats 弹窗同款滚动列表; UI 段在下文主菜单段之后)。
static struct {
    bool open;
    int scroll;
} s_diag;
static lv_obj_t *s_diag_scrim, *s_diag_view, *s_diag_list;

// 听声辨机(quiz): quiz_ctl.h 状态机 + 模式选择页复用 menu_ctl(2 行)。
// 三个子页共用一套 scrim/panel(组容器隐藏切换): 模式页 / 答题页(ASK/WAIT) /
// 结算页;UI 段在下文硬件诊断页段之后。WAIT 到时推进挂在 tick。
static quiz_ctl_t s_quiz;
static menu_ctl_t s_quiz_mode;  // 模式选择页导航(复用菜单状态机, 2 行)
static bool s_quiz_open;        // 任一 quiz 子页可见(输入路由/tick 用)
static bool s_quiz_recs_loaded; // NVS 纪录懒读标志(首开 quiz 时读一次)
static uint16_t s_quiz_best_round, s_quiz_best_streak;  // 纪录缓存(结算对比)
static lv_obj_t *s_quiz_scrim, *s_quiz_panel;       // 答题/结算共用大面板
static lv_obj_t *s_quiz_mode_scrim, *s_quiz_mode_panel;  // 模式页独立小面板(2 行)
static lv_obj_t *s_quiz_mode_grp, *s_quiz_ask_grp, *s_quiz_sum_grp;
static lv_obj_t *s_quiz_mode_rows[2];
static lv_obj_t *s_quiz_qline;      // 题号行:「第 N 题 · 听音/场景」
static lv_obj_t *s_quiz_qtext;      // 场景句(听音题隐藏;两行折行)
static lv_obj_t *s_quiz_icon_listen, *s_quiz_icon_scn;  // 题型图标(二显一)
static lv_obj_t *s_quiz_opt_rows[3], *s_quiz_opt_labels[3];
static lv_obj_t *s_quiz_sum_big, *s_quiz_sum_streak, *s_quiz_sum_best,
    *s_quiz_sum_new;

// 场景连播列表页: menu_ctl 复用(count=APP_SCENE_COUNT, UP/DOWN 选行, OK 开始
// 播放并关页回宫格, OK 长按退出)。播放本身交给 app_audio_play_scene(序列源
// 在音频层, 视觉跟随复用宫格 seq 机制);UI 段在下文听声辨机段之后。
static menu_ctl_t s_scene_sel;
static bool s_scene_open;
static lv_obj_t *s_scene_scrim;
static lv_obj_t *s_scene_rows[APP_SCENE_COUNT];

// GPWS 闹钟: 设置页(alarm_ctl.h 状态机, 浏览/编辑两态) → 确认层「闹钟已设
// N 分钟」(1s) → input_task 闹钟深睡(timer+GPIO 双武装)。TIMER 唤醒后进
// 响铃页(单曲循环 + 波形呼吸 + 任意键停止)。闹钟值跨深睡由 NVS 持载
// (gpws_app: alarm_min/alarm_tune, main.c 唤醒时消费清除)。
// UI 段在下文场景连播段之后。
static alarm_ctl_t s_alarm;
static bool s_alarm_open;            // 设置页可见
static int64_t s_alarm_confirm_us;   // 确认层显示时刻(0=无)
static volatile bool s_alarm_due;    // 确认层满 1s: input_task 执行闹钟深睡
static bool s_alarm_ring;            // 响铃页可见(TIMER 唤醒后)
static lv_obj_t *s_alarm_scrim, *s_alarm_rows[ALARM_ROW_COUNT];
static lv_obj_t *s_alarm_val_labels[ALARM_ROW_COUNT], *s_alarm_hint;
static lv_obj_t *s_alarm_ok_scrim, *s_alarm_ok_label;  // 「闹钟已设 N 分钟」层
static lv_obj_t *s_ring_scrim, *s_ring_name, *s_ring_hint, *s_ring_bars[7];

// 播放统计弹窗:打开时快照排序(次数降序,同次数目录序),列表滚动=y 位移钳制。
static struct {
    bool open;
    int scroll;  // 列表滚动偏移(行数)
} s_stats;
static lv_obj_t *s_stats_scrim, *s_stats_view, *s_stats_list;
static lv_obj_t *s_stats_sub;  // 副标题:总次数/总时长(快照求和)
static lv_obj_t *s_stats_names[APP_CATALOG_COUNT], *s_stats_counts[APP_CATALOG_COUNT];
static lv_obj_t *s_stats_marks[APP_CATALOG_COUNT];  // 前三奖牌图标/第 4+ 序号
#define STATS_ROW_H   22  // 统计行高(含行距)
#define STATS_VIS_ROWS 10 // 统计列表可视行数(滚动上限 = N_CARDS - 此值)

// ---------- 绘制辅助 ----------
// 图标统一在 20×20 设计栅格上设计,按控件尺寸浮点缩放(不预取整),
// 消除奇偶尺寸下的 ±0.5px 中轴偏移与阶梯不对称;整数化只发生在 LVGL
// 光栅化层(弧自带边缘 AA,线段/三角形按子像素位置光栅化)。
#define RS(v) ((int)((v) + 0.5))

// 浮点精确点:设计栅格坐标 × 缩放因子,保留子像素。
static lv_point_precise_t ptd(double x, double y) {
    lv_point_precise_t p = {(lv_value_precise_t)x, (lv_value_precise_t)y};
    return p;
}

static void d_line(lv_layer_t *l, lv_point_precise_t p1, lv_point_precise_t p2,
                   lv_color_t col, int width) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = col;
    d.opa = LV_OPA_COVER;
    d.width = width;
    d.round_start = 1;
    d.round_end = 1;
    d.p1 = p1;
    d.p2 = p2;
    lv_draw_line(l, &d);
}

static void d_rect(lv_layer_t *l, const lv_area_t *a, lv_color_t col, lv_opa_t opa, int r) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa = opa;
    d.radius = r;
    lv_draw_rect(l, &d, a);
}

static void d_arc(lv_layer_t *l, lv_point_precise_t ctr, int r, int a0, int a1,
                  lv_color_t col, int width) {
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = col;
    d.opa = LV_OPA_COVER;
    d.center = lv_point_from_precise(&ctr);
    d.radius = r;
    d.start_angle = a0;
    d.end_angle = a1;
    d.width = width;
    lv_draw_arc(l, &d);
}

static void d_tri(lv_layer_t *l, lv_point_precise_t p0, lv_point_precise_t p1,
                  lv_point_precise_t p2, lv_color_t col, lv_opa_t opa) {
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0] = p0;
    d.p[1] = p1;
    d.p[2] = p2;
    d.color = col;
    d.opa = opa;
    lv_draw_triangle(l, &d);
}

// 栅格点/盒/弧:20 单位设计栅格 → 控件像素(浮点,子像素保留到光栅层)。
static lv_point_precise_t gpt(const lv_area_t *c, double sx, double sy, double gx, double gy) {
    return ptd(c->x1 + gx * sx, c->y1 + gy * sy);
}

static void gbox(lv_layer_t *l, const lv_area_t *c, double sx, double sy,
                 double gx0, double gy0, double gx1, double gy1,
                 lv_color_t col, lv_opa_t opa, int r) {
    lv_area_t a = {c->x1 + RS(gx0 * sx), c->y1 + RS(gy0 * sy),
                   c->x1 + RS(gx1 * sx), c->y1 + RS(gy1 * sy)};
    d_rect(l, &a, col, opa, r);
}

// 弧:半径按 x/y 缩放均值计算——非正方形控件上仍是正圆,不会椭圆化。
static void garc(lv_layer_t *l, const lv_area_t *c, double sx, double sy,
                 double gx, double gy, double gr, int a0, int a1,
                 lv_color_t col, int width) {
    d_arc(l, gpt(c, sx, sy, gx, gy), RS(gr * (sx + sy) / 2), a0, a1, col, width);
}

// ---------- 图标:draw_event 矢量绘制 ----------
// UI 自有图标(卡片图标直接复用 app_icon_t 的值)。
enum {
    UI_ICON_PLAY = 100,
    UI_ICON_PAUSE,
    UI_ICON_CHEV_UP,
    UI_ICON_CHEV_DOWN,
    UI_ICON_MEDAL_GOLD,
    UI_ICON_MEDAL_SILVER,
    UI_ICON_MEDAL_BRONZE,
    UI_ICON_VOLUME,
};

static void icon_draw_event_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    int w = (int)lv_area_get_width(&c);
    int h = (int)lv_area_get_height(&c);
    if (w < 2 || h < 2) return;
    lv_color_t col = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    const double sx = w / 20.0, sy = h / 20.0;
    const int sw = (w >= 26) ? 3 : (w >= 13) ? 2 : 1;  // 描边分档:大/中/小图标

    switch (id) {
    case APP_ICON_UP: {  // 拉升/爬升:侧视飞机上仰(填充机身+机翼+尾翼)+地面线
        // 机身:细长平行四边形,机头 (13,4) 右上;两个三角形拼成。
        d_tri(layer, gpt(&c, sx, sy, 2, 15), gpt(&c, sx, sy, 13, 4),
              gpt(&c, sx, sy, 15, 6), col, LV_OPA_COVER);
        d_tri(layer, gpt(&c, sx, sy, 2, 15), gpt(&c, sx, sy, 15, 6),
              gpt(&c, sx, sy, 4, 17), col, LV_OPA_COVER);
        // 机翼:垂直于机身中段的短杠。
        gbox(layer, &c, sx, sy, 7, 10, 9, 14, col, LV_OPA_COVER, 1);
        // 尾翼:机尾小三角。
        d_tri(layer, gpt(&c, sx, sy, 2, 15), gpt(&c, sx, sy, 4, 13),
              gpt(&c, sx, sy, 4, 16), col, LV_OPA_COVER);
        // 地面线:水平线,天然笔直。
        d_line(layer, gpt(&c, sx, sy, 1, 19), gpt(&c, sx, sy, 19, 19), col, sw);
        break;
    }
    case APP_ICON_DOWN: {  // 下沉率:侧视飞机下俯(镜像)+地面线
        d_tri(layer, gpt(&c, sx, sy, 17, 15), gpt(&c, sx, sy, 6, 4),
              gpt(&c, sx, sy, 4, 6), col, LV_OPA_COVER);
        d_tri(layer, gpt(&c, sx, sy, 17, 15), gpt(&c, sx, sy, 4, 6),
              gpt(&c, sx, sy, 15, 17), col, LV_OPA_COVER);
        gbox(layer, &c, sx, sy, 12, 10, 14, 14, col, LV_OPA_COVER, 1);
        d_tri(layer, gpt(&c, sx, sy, 17, 15), gpt(&c, sx, sy, 15, 13),
              gpt(&c, sx, sy, 15, 16), col, LV_OPA_COVER);
        d_line(layer, gpt(&c, sx, sy, 1, 19), gpt(&c, sx, sy, 19, 19), col, sw);
        break;
    }
    case APP_ICON_MTN:  // 近地/地形:双峰山形(主峰+左侧矮峰,填充)
        d_tri(layer, gpt(&c, sx, sy, 10, 3), gpt(&c, sx, sy, 2, 17),
              gpt(&c, sx, sy, 18, 17), col, LV_OPA_COVER);
        d_tri(layer, gpt(&c, sx, sy, 5, 9), gpt(&c, sx, sy, 1, 17),
              gpt(&c, sx, sy, 11, 17), col, LV_OPA_COVER);
        break;
    case APP_ICON_BELL: {  // 警报铃铛:填充钟形(圆顶+两侧斜边+底边)+铃锤
        // 钟形主体:圆顶弧(180..360,半径 6)+左右斜边到底边,整体填充。
        // 圆顶用上半弧描粗近似轮廓,主体用两三角形+矩形填满,消除拼接缝。
        d_tri(layer, gpt(&c, sx, sy, 10, 2), gpt(&c, sx, sy, 4, 11),
              gpt(&c, sx, sy, 16, 11), col, LV_OPA_COVER);   // 上锥体
        gbox(layer, &c, sx, sy, 4, 8, 16, 11, col, LV_OPA_COVER, 0);  // 主体矩形
        garc(layer, &c, sx, sy, 10, 8, 6, 180, 360, col, RS(3 * (sx + sy) / 2));  // 圆顶加粗
        gbox(layer, &c, sx, sy, 3, 11, 17, 12, col, LV_OPA_COVER, 0); // 底沿
        d_tri(layer, gpt(&c, sx, sy, 10, 12), gpt(&c, sx, sy, 12, 12),
              gpt(&c, sx, sy, 11, 16), col, LV_OPA_COVER);   // 铃锤
        break;
    }
    case APP_ICON_VOICE: {  // 呼啸:手握扩音器(口朝右上):锥体梯形+尾部弯柄+尾扣
        // 锥体:窄端(左下)向喇叭口(右上)展开,两个三角形填充。
        d_tri(layer, gpt(&c, sx, sy, 3.2, 11.9), gpt(&c, sx, sy, 13.6, 0.7),
              gpt(&c, sx, sy, 2.9, 14.2), col, LV_OPA_COVER);
        d_tri(layer, gpt(&c, sx, sy, 13.6, 0.7), gpt(&c, sx, sy, 19.6, 10.4),
              gpt(&c, sx, sy, 2.9, 14.2), col, LV_OPA_COVER);
        // 手柄:锥体尾部伸出的弯钩,两段圆头粗线衔接。
        d_line(layer, gpt(&c, sx, sy, 4.0, 15.0), gpt(&c, sx, sy, 9.4, 18.4),
               col, RS(4 * sx));
        d_line(layer, gpt(&c, sx, sy, 9.4, 18.4), gpt(&c, sx, sy, 11.2, 16.7),
               col, RS(3.5 * sx));
        // 尾扣:左下独立小短柄(挂绳扣)。
        d_line(layer, gpt(&c, sx, sy, 1.0, 15.4), gpt(&c, sx, sy, 3.0, 19.0),
               col, RS(2.5 * sx));
        break;
    }
    case APP_ICON_AP: {  // 自动驾驶姿态仪(外环+地平线+中心点)
        garc(layer, &c, sx, sy, 10, 10, 8, 0, 360, col, sw);
        d_line(layer, gpt(&c, sx, sy, 3, 10), gpt(&c, sx, sy, 17, 10), col, sw);
        gbox(layer, &c, sx, sy, 9, 9, 11, 11, col, LV_OPA_COVER, 1);
        break;
    }
    case APP_ICON_PACK:  // 合辑(四方格)
        gbox(layer, &c, sx, sy, 3, 3, 9, 9, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 11, 3, 17, 9, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 3, 11, 9, 17, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 11, 11, 17, 17, col, LV_OPA_COVER, 1);
        break;
    case APP_ICON_BEACON: {  // 信标灯(圆灯+放射光)
        garc(layer, &c, sx, sy, 10, 12, 4, 0, 360, col, sw);
        d_line(layer, gpt(&c, sx, sy, 10, 2), gpt(&c, sx, sy, 10, 6), col, sw);
        d_line(layer, gpt(&c, sx, sy, 4, 5), gpt(&c, sx, sy, 7, 8), col, sw);
        d_line(layer, gpt(&c, sx, sy, 16, 5), gpt(&c, sx, sy, 13, 8), col, sw);
        d_line(layer, gpt(&c, sx, sy, 2, 14), gpt(&c, sx, sy, 6, 13), col, sw);
        d_line(layer, gpt(&c, sx, sy, 18, 14), gpt(&c, sx, sy, 14, 13), col, sw);
        break;
    }
    case APP_ICON_GEAR:  // 齿轮(外齿+内环)
        garc(layer, &c, sx, sy, 10, 10, 7, 0, 360, col, sw);
        garc(layer, &c, sx, sy, 10, 10, 3, 0, 360, col, sw);
        d_line(layer, gpt(&c, sx, sy, 10, 1), gpt(&c, sx, sy, 10, 5), col, sw);
        d_line(layer, gpt(&c, sx, sy, 10, 15), gpt(&c, sx, sy, 10, 19), col, sw);
        d_line(layer, gpt(&c, sx, sy, 1, 10), gpt(&c, sx, sy, 5, 10), col, sw);
        d_line(layer, gpt(&c, sx, sy, 15, 10), gpt(&c, sx, sy, 19, 10), col, sw);
        d_line(layer, gpt(&c, sx, sy, 4, 4), gpt(&c, sx, sy, 7, 7), col, sw);
        d_line(layer, gpt(&c, sx, sy, 13, 13), gpt(&c, sx, sy, 16, 16), col, sw);
        d_line(layer, gpt(&c, sx, sy, 16, 4), gpt(&c, sx, sy, 13, 7), col, sw);
        d_line(layer, gpt(&c, sx, sy, 7, 13), gpt(&c, sx, sy, 4, 16), col, sw);
        break;
    case APP_ICON_ALT: {  // 高度表(竖刻度尺+指针)
        d_line(layer, gpt(&c, sx, sy, 5, 2), gpt(&c, sx, sy, 5, 18), col, sw);
        d_line(layer, gpt(&c, sx, sy, 5, 5), gpt(&c, sx, sy, 8, 5), col, sw);
        d_line(layer, gpt(&c, sx, sy, 5, 10), gpt(&c, sx, sy, 8, 10), col, sw);
        d_line(layer, gpt(&c, sx, sy, 5, 15), gpt(&c, sx, sy, 8, 15), col, sw);
        d_line(layer, gpt(&c, sx, sy, 7, 10), gpt(&c, sx, sy, 16, 6), col, sw);
        d_line(layer, gpt(&c, sx, sy, 7, 10), gpt(&c, sx, sy, 16, 14), col, sw);
        gbox(layer, &c, sx, sy, 6, 9, 8, 11, col, LV_OPA_COVER, 1);
        break;
    }
    case APP_ICON_CHIME: {  // 客舱提示音(双音符)
        gbox(layer, &c, sx, sy, 4, 13, 7, 15, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 12, 11, 15, 13, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 6, 4, 7, 14, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 14, 3, 15, 12, col, LV_OPA_COVER, 1);
        d_line(layer, gpt(&c, sx, sy, 7, 4), gpt(&c, sx, sy, 15, 3), col, sw);
        break;
    }
    case APP_ICON_RADIO: {  // 呼叫:对话气泡(圆角矩形+短尾)
        gbox(layer, &c, sx, sy, 2, 3, 18, 14, col, LV_OPA_COVER, 3);
        d_tri(layer, gpt(&c, sx, sy, 6, 13), gpt(&c, sx, sy, 10, 13),
              gpt(&c, sx, sy, 5, 18), col, LV_OPA_COVER);
        break;
    }
    case APP_ICON_PLANE:  // 失速/状态栏:飞机(机身+主翼+尾翼)
        gbox(layer, &c, sx, sy, 9, 2, 11, 17, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 2, 8, 18, 10, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 6, 14, 14, 15, col, LV_OPA_COVER, 1);
        break;
    case APP_ICON_GAUGE:  // 减速:速度表(表盘+指针+轴心)
        garc(layer, &c, sx, sy, 10, 10, 7, 0, 360, col, sw);
        d_line(layer, gpt(&c, sx, sy, 10, 10), gpt(&c, sx, sy, 15, 6), col, sw);
        gbox(layer, &c, sx, sy, 9, 9, 11, 11, col, LV_OPA_COVER, 1);
        break;
    case APP_ICON_WIND:  // 风切变:三段气流+上挑钩
        d_line(layer, gpt(&c, sx, sy, 3, 7), gpt(&c, sx, sy, 12, 7), col, sw);
        d_line(layer, gpt(&c, sx, sy, 12, 7), gpt(&c, sx, sy, 15, 4), col, sw);
        d_line(layer, gpt(&c, sx, sy, 3, 11), gpt(&c, sx, sy, 14, 11), col, sw);
        d_line(layer, gpt(&c, sx, sy, 14, 11), gpt(&c, sx, sy, 17, 8), col, sw);
        d_line(layer, gpt(&c, sx, sy, 3, 15), gpt(&c, sx, sy, 9, 15), col, sw);
        d_line(layer, gpt(&c, sx, sy, 9, 15), gpt(&c, sx, sy, 12, 12), col, sw);
        break;
    case APP_ICON_RADAR:  // 冲突告警:雷达(双环+扫描线)
        garc(layer, &c, sx, sy, 10, 10, 8, 0, 360, col, sw);
        garc(layer, &c, sx, sy, 10, 10, 3, 0, 360, col, sw);
        d_line(layer, gpt(&c, sx, sy, 10, 10), gpt(&c, sx, sy, 16, 5), col, sw);
        break;
    case UI_ICON_PLAY:  // 播放三角(等腰,视觉质心在中轴)
        d_tri(layer, gpt(&c, sx, sy, 6, 4), gpt(&c, sx, sy, 15, 10),
              gpt(&c, sx, sy, 6, 16), col, LV_OPA_COVER);
        break;
    case UI_ICON_PAUSE:  // 暂停双杠
        gbox(layer, &c, sx, sy, 6, 4, 9, 16, col, LV_OPA_COVER, 1);
        gbox(layer, &c, sx, sy, 11, 4, 14, 16, col, LV_OPA_COVER, 1);
        break;
    case UI_ICON_CHEV_UP:  // 上翻箭头
        d_line(layer, gpt(&c, sx, sy, 5, 13), gpt(&c, sx, sy, 10, 8), col, sw);
        d_line(layer, gpt(&c, sx, sy, 10, 8), gpt(&c, sx, sy, 15, 13), col, sw);
        break;
    case UI_ICON_CHEV_DOWN:
        d_line(layer, gpt(&c, sx, sy, 5, 7), gpt(&c, sx, sy, 10, 12), col, sw);
        d_line(layer, gpt(&c, sx, sy, 10, 12), gpt(&c, sx, sy, 15, 7), col, sw);
        break;
    case UI_ICON_MEDAL_GOLD:
    case UI_ICON_MEDAL_SILVER:
    case UI_ICON_MEDAL_BRONZE: {  // 统计排行奖牌:双圆环 + 中心名次数字
        // 外环细线 + 内环加粗,名次数字(1/2/3)经 lv_draw_character 画在
        // 图标正中;数字色取图标色,与环同色系。行内序号语义见 build_stats。
        garc(layer, &c, sx, sy, 10, 10, 8.5, 0, 360, col, sw);
        garc(layer, &c, sx, sy, 10, 10, 6.5, 0, 360, col, RS(2 * (sx + sy) / 2));
        uint32_t rank = id == UI_ICON_MEDAL_GOLD   ? (uint32_t)'1'
                        : id == UI_ICON_MEDAL_SILVER ? (uint32_t)'2'
                                                     : (uint32_t)'3';
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.font = &app_font_6_bold;
        ld.color = col;
        ld.opa = LV_OPA_COVER;
        // lv_draw_character 的 point = 字形盒左上角(高=line_height 9px)。
        // 以环中心 (10,10) 居中:左上 ≈ 中心 - 半宽 2 - 半高 4。
        lv_point_t p = {c.x1 + (lv_value_precise_t)(10 * sx) - 2,
                        c.y1 + (lv_value_precise_t)(10 * sy) - 4};
        lv_draw_character(layer, &ld, &p, rank);
        break;
    }
    case UI_ICON_VOLUME: {  // 音量:喇叭(方盒+锥体)+两道声波弧
        gbox(layer, &c, sx, sy, 4, 8, 8, 12, col, LV_OPA_COVER, 1);
        d_tri(layer, gpt(&c, sx, sy, 8, 8), gpt(&c, sx, sy, 13, 4),
              gpt(&c, sx, sy, 13, 16), col, LV_OPA_COVER);
        d_tri(layer, gpt(&c, sx, sy, 8, 8), gpt(&c, sx, sy, 13, 16),
              gpt(&c, sx, sy, 8, 12), col, LV_OPA_COVER);
        // 声波弧以 3 点钟为 0° 顺时针:-55° 会被 LVGL 角度掩码钳位成 0°
        // (只画上半段);而 305°..55° 的跨零弧走掩码通用路径(start/end 双线
        // 半平面组合),1px 线宽+小半径下端头像素会被定点舍入丢掉(右下缺
        // 一点)。拆成两段非跨零弧:305..360 同 bell 图标的 end 处理路径,
        // 0..55 走 start==0 特判路径,两段在 0° 射线处几何相接。
        garc(layer, &c, sx, sy, 13, 10, 3, 305, 360, col, sw);
        garc(layer, &c, sx, sy, 13, 10, 3, 0, 55, col, sw);
        garc(layer, &c, sx, sy, 13, 10, 5, 305, 360, col, sw);
        garc(layer, &c, sx, sy, 13, 10, 5, 0, 55, col, sw);
        break;
    }
    default:
        break;
    }
}


// ---------- 控件构造辅助 ----------
static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

// 矢量图标构造(draw_event 绘制,见 icon_draw_event_cb)。导出给 app_boot 开机
// 动画复用同一套图标绘制,避免复制 path 数据。icon_id: APP_ICON_* / UI_ICON_*。
lv_obj_t *app_ui_icon_create(lv_obj_t *parent, int icon_id, int x, int y, int w, int h,
                             uint32_t color) {
    lv_obj_t *o = mk_box(parent, x, y, w, h);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_add_event_cb(o, icon_draw_event_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)icon_id);
    return o;
}

static lv_obj_t *mk_icon(lv_obj_t *parent, int icon_id, int x, int y, int w, int h,
                         uint32_t color) {
    return app_ui_icon_create(parent, icon_id, x, y, w, h, color);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                          const lv_font_t *font, uint32_t color,
                          lv_text_align_t align, int letter_space) {
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(o, align, 0);
    lv_obj_set_style_text_letter_space(o, letter_space, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    return o;
}

// ---------- 状态刷新 ----------
// 时长格式 M:SS(单位数分钟,资源最长 10s → 0:10;分钟钳制 9)。手写转换免 snprintf 告警。
static void fmt_dur(char *buf, size_t n, uint32_t ms) {
    if (n < 6) return;
    int s = (int)(ms / 1000);
    int m = s / 60;
    if (m > 9) m = 9;
    s %= 60;
    buf[0] = (char)('0' + m);
    buf[1] = ':';
    buf[2] = (char)('0' + s / 10);
    buf[3] = (char)('0' + s % 10);
    buf[4] = '\0';
}

// 页头副标题三态复用(默认态:进入即聚焦,不出现)。
static void update_sub(void) {
    const app_alert_t *a;
    char buf[64], dur[8];
    if (s_play_idx >= 0) {
        a = app_catalog(s_play_idx);
        snprintf(buf, sizeof(buf), "正在播放 · %s", a->name);
        lv_obj_set_style_text_color(s_sub, lv_color_hex(a->acc), 0);
    } else {
        a = app_catalog(s_focus);
        fmt_dur(dur, sizeof(dur), a->dur_ms);
        snprintf(buf, sizeof(buf), "%s · %s · %s", a->name, a->en, dur);
        lv_obj_set_style_text_color(s_sub, lv_color_hex(a->acc), 0);
    }
    lv_label_set_text(s_sub, buf);
}

static void card_set_focus(lv_obj_t *card, const app_alert_t *a, bool focused) {
    if (focused) {
        lv_obj_set_style_border_color(card, lv_color_hex(a->acc), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD_FOCUS), 0);
        lv_obj_set_style_shadow_width(card, 12, 0);
        lv_obj_set_style_shadow_color(card, lv_color_hex(a->acc), 0);
        lv_obj_set_style_shadow_opa(card, 71, 0);
    } else {
        lv_obj_set_style_border_color(card, lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
    }
}

static void scroll_anim_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void update_thumb(void) {
    int vis, th, ty;
    app_layout_thumb(&s_lay, s_scroll, &vis, &th, &ty);
    if (vis) {
        lv_obj_clear_flag(s_sb_track, LV_OBJ_FLAG_HIDDEN);  // 隐藏 flag 连子滑块一起收
        lv_obj_set_size(s_sb_thumb, 3, th);
        lv_obj_set_pos(s_sb_thumb, 0, ty);  // 滑块在轨道内,x 恒 0
    } else {
        lv_obj_add_flag(s_sb_track, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scroll_to(int top) {
    if (top == s_scroll) {
        update_thumb();
        return;
    }
    s_scroll = top;
    if (s_repeat_timer) {
        // 连滚中(宫格连发,120ms/步):直写 y,完全绕开 lv_anim 链——真机长按
        // 连滚曾出现列表停滞(焦点/副标题/滚动条照常,仅卡片列表不动,环绕步
        // 才恢复)的怪象,根因在动画执行层,直写对其免疫。每步 36-68px 的
        // 移动在 120ms 间隔下,100ms 缓动与直写观感几乎无差。先清残留:
        // 连滚第一步(repeat_start 立即步进时 timer 尚未创建)仍走动画,
        // 其轨迹若还在跑会把直写位置又拉回去。
        lv_anim_delete(s_content, scroll_anim_cb);
        lv_obj_set_y(s_content, -top);
    } else {
        // 单击导航:保留 100ms 平滑动画;start 失败(分配不出)回退直写——
        // 列表宁可瞬移,不可停滞(停滞在连滚中真实发生过,见上)。
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_content);
        lv_anim_set_values(&a, lv_obj_get_y(s_content), -top);
        lv_anim_set_duration(&a, 100);
        lv_anim_set_exec_cb(&a, scroll_anim_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        if (!lv_anim_start(&a)) lv_obj_set_y(s_content, -top);
    }
    update_thumb();
}

// 音波条高度变化时保持垂直居中(Demo scaleY 的近似:上下对称呼吸)。
static void wave_set_height(int idx, int k, int h) {
    lv_obj_t *b = s_wave_bars[idx][k];
    int wave_cy = (app_catalog(idx)->card_h == 96) ? 39 : 27;
    lv_obj_set_size(b, 4, h);
    lv_obj_set_y(b, wave_cy - h / 2);
}

// 播放遮罩复位(隐藏+标签归位+音波归静态)。
static void mask_reset(int idx) {
    lv_obj_add_flag(s_masks[idx], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_mask_labels[idx], "播放中");
    for (int i = 0; i < 7; i++) {
        wave_set_height(idx, i, WAVE_BASE[i]);
    }
}

static void mask_show(int idx) {
    const app_alert_t *a = app_catalog(idx);
    lv_obj_clear_flag(s_masks[idx], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_mask_labels[idx], "播放中");
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_bg_color(s_wave_bars[idx][i], lv_color_hex(a->acc), 0);
    }
}

// 纯视觉版 ui_play:不向音频任务发命令。音频任务自行接播(连播)后,tick 用它
// 把 UI 追赶到新条目;ui_play 也复用它。
static void ui_show_playing(int idx) {
    char dur[8];
    if (idx < 0) return;
    if (s_play_idx >= 0 && s_play_idx != idx) mask_reset(s_play_idx);
    s_play_idx = idx;
    s_paused = false;
    // 乐观更新活跃基准:播放命令经 notify 异步生效,若 tick 在音频任务置
    // s_active 前拍到「不活跃」,宽限期从这里起算,不会立刻误判自然结束。
    s_last_active_us = esp_timer_get_time();
    mask_show(idx);
    const app_alert_t *a = app_catalog(idx);
    fmt_dur(dur, sizeof(dur), a->dur_ms);
    lv_label_set_text(s_cur_label, "0:00");
    lv_label_set_text(s_tot_label, dur);
    lv_obj_set_style_text_color(s_cur_label, lv_color_hex(a->acc), 0);
    lv_obj_set_style_bg_color(s_fill, lv_color_hex(a->acc), 0);
    lv_obj_set_width(s_fill, 0);
    lv_obj_add_flag(s_browsebar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_playbar, LV_OBJ_FLAG_HIDDEN);
    update_sub();
}

static void ui_play(int idx) {
    app_audio_play(idx);
    ui_show_playing(idx);
}

static void ui_stop(void) {
    if (s_play_idx < 0) return;
    mask_reset(s_play_idx);
    s_play_idx = -1;
    s_paused = false;
    lv_obj_add_flag(s_playbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_browsebar, LV_OBJ_FLAG_HIDDEN);
    update_sub();
}

// 焦点跳到指定条目并滚动到可见(连播跟随;与用户导航共用同一套视觉)。
static void ui_focus_jump(int idx) {
    if (idx < 0 || idx == s_focus) return;
    card_set_focus(s_cards[s_focus], app_catalog(s_focus), false);
    s_focus = idx;
    card_set_focus(s_cards[s_focus], app_catalog(s_focus), true);
    scroll_to(app_layout_scroll_for_focus(&s_lay, s_focus, s_scroll));
    if (s_play_idx < 0) update_sub();
}

// ---------- 100ms 心跳:电量/播放进度/音波动画(时钟已按需求移除) ----------

static void lowbatt_poll(int soc, int mv);  // 定义在下文低电保护段

static void update_battery(void) {
    int soc = bsp_battery_soc();
    if (soc < 0) return;  // 读取失败保持上次显示与低电判定
    int mv = bsp_battery_mv();  // 一拍一读,full_stuck/低电佐证/0% 显示门共用
    // 满电钉死检测(每 2s 一拍):矛盾读数连续 FULL_STUCK_CONFIRM_POLLS 拍才
    // RESTART 重估(误判窗口内充电态电压 >4.15V 不满足矛盾条件);重估阻塞
    // tick ~31ms(一次性、低频);重估后 ~1s 新 SOC 出来,本拍仍显示旧值,
    // 下一两拍自愈;重估初期的假低读数由 lowbatt_poll 的低电去抖挡住。
    if (s_fullstuck_cooldown) {
        s_fullstuck_cooldown--;
    } else if (bsp_battery_full_stuck(soc, mv)) {
        if (++s_fullstuck_streak >= FULL_STUCK_CONFIRM_POLLS) {
            s_fullstuck_streak = 0;
            s_fullstuck_cooldown = FULL_STUCK_COOLDOWN_POLLS;
            bsp_battery_recalibrate();
        }
    } else {
        s_fullstuck_streak = 0;
    }
    lowbatt_poll(soc, mv);
    // 熄屏态不碰 UI:满电重估/充电爬升的 SOC 跳变会让标题行脏区重绘走进
    // 最深的弧线绘制链(音量图标+奖牌环,栈溢出高危路径),而屏本来就黑着
    // 刷新毫无收益。s_shown_soc 不推进,亮屏后下一拍(≤2s)自愈补写;
    // lowbatt_poll 已跑完,低电保护与唤醒点亮不受影响。
    if (s_screen_off) return;
    // 重算窗口的假 0%(电压健康)不进显示:保持上次读数,状态栏不再闪 0%;
    // 真 0% 电芯 ~3.3V < 佐证阈值,照常显示。mv<0 不拦(保守,罕见瞬态)。
    if (soc == 0 && mv >= LOW_BATT_MV_CORROBORATE) return;
    if (soc == s_shown_soc) return;
    s_shown_soc = soc;
    if (soc > 100) soc = 100;
    char buf[8];  // soc 0-100,手写转换免 format-truncation 告警
    int i = 0;
    if (soc >= 100) {
        buf[i++] = '1';
        buf[i++] = '0';
        buf[i++] = '0';
    } else if (soc >= 10) {
        buf[i++] = (char)('0' + soc / 10);
        buf[i++] = (char)('0' + soc % 10);
    } else {
        buf[i++] = (char)('0' + soc);
    }
    buf[i] = '\0';
    lv_label_set_text(s_batt_label, buf);
    lv_obj_set_width(s_batt_fill, 13 * soc / 100);  // 内宽 13 的百分比
    // 电量低时填充变红;百分比改浅色保证可读。
    lv_obj_set_style_bg_color(s_batt_fill,
                              lv_color_hex(soc < 20 ? COL_BATT_LOW : COL_BATT_FILL), 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(soc >= 45 ? COL_CANVAS : COL_BATT_FILL), 0);
}

// 标题行音量指示器同步:变更才写(幂等)。音量唯一运行期变更源是设置弹窗
// (modal_refresh 即时刷,含连调),开机 NVS 真值晚于 UI 构建由 tick 一次性
// 状态同步纠正——无需周期轮询。
static void update_volume_indicator(void) {
    int vol = app_audio_volume();
    if (vol == s_shown_vol) return;
    s_shown_vol = vol;
    char buf[5];  // "100" + NUL
    snprintf(buf, sizeof(buf), "%d", vol);
    lv_label_set_text(s_vol_label, buf);
    // 图标随数字实际宽度摆位:恒 4px 间距,整组右缘 228 与电池列对齐
    // (文字左缘 = 192+36-宽,图标 x = 文字左缘-4-12 = 212-宽)。
    lv_point_t ts;
    lv_text_get_size(&ts, buf, &app_font_10, 0, 0, LV_COORD_MAX,
                     LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_x(s_vol_icon, 212 - ts.x);
}

// ---------- 低电保护 ----------
// 低电提醒遮罩(懒构建一次):非交互,任意键继续,15s 自动关。
static void build_lowbatt(lv_obj_t *scr) {
    s_lowbatt_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_lowbatt_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_lowbatt_scrim, COL_MASK_OPA, 0);
    lv_obj_t *panel = mk_box(s_lowbatt_scrim, 16, 96, 208, 108);
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    mk_icon(panel, APP_ICON_BELL, 94, 8, 20, 20, COL_BATT_LOW);
    mk_label(panel, "电量不足", 8, 34, 192, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_CENTER, 0);
    s_lowbatt_pct = mk_label(panel, "10%", 8, 60, 192, 17, &app_font_12, COL_INK,
                             LV_TEXT_ALIGN_CENTER, 0);
    mk_label(panel, "任意键继续", 8, 86, 192, 17, &app_font_12, COL_INK3,
             LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_lowbatt_scrim, LV_OBJ_FLAG_HIDDEN);
}

// 弹低电提醒:熄屏态直接亮屏(无按键事件→无尾随吞键问题,不 arm 唤醒守卫,
// 也不清 s_sleep_due——闲置到期该睡照睡,提醒只管提醒)。
static void lowbatt_show(int soc) {
    if (!s_lowbatt_scrim) build_lowbatt(lv_screen_active());
    char buf[16];  // %d 最长 11 字节 + '%' + NUL(实际调用时 soc ≤ 10)
    snprintf(buf, sizeof(buf), "%d%%", soc > 100 ? 100 : soc);
    lv_label_set_text(s_lowbatt_pct, buf);
    lv_obj_clear_flag(s_lowbatt_scrim, LV_OBJ_FLAG_HIDDEN);
    s_lowbatt_show_us = esp_timer_get_time();
    // 提醒=会话基准:刷新闲置计时,否则熄屏闲置态(闲置早已 ≥60s)弹出的
    // 提醒会被下一拍 tick 的闲置熄屏立即回收(15s 遮罩活不到、会话闩已
    // 耗),亮屏闲置 55s 弹出的也只剩 5s 可见——提醒获得完整 15s+60s 窗口。
    s_last_input_us = s_lowbatt_show_us;
    if (s_screen_off) {
        s_screen_off = false;
        bsp_display_backlight_fade(100, 300);
    }
}

// 低电保护落地层:决策与去抖状态在 lowbatt_ctl.h(纯逻辑,host 可测),
// 本函数只做呈现(弹提醒遮罩)与执行(置深睡到期标志给 input_task)。
// 从未成功读取(SOC=-1)则整个保护不生效(update_battery 早退)。
static void lowbatt_poll(int soc, int mv) {
    bool was_warned = s_lowbatt_ctl.warned;
    lowbatt_action_t act = lowbatt_ctl_poll(&s_lowbatt_ctl, soc, mv,
                                            s_modal.open || s_stats.open);
    if (act == LOWBATT_SLEEP) {
        s_sleep_due = true;  // input_task 轮询执行入睡序列(计数落盘在内)
        return;              // 强睡在途,提醒无意义
    }
    // 回弹重置提醒闩:若提醒遮罩还挂着(15s 内回弹),一并收起。
    if (was_warned && !s_lowbatt_ctl.warned && s_lowbatt_show_us) {
        lv_obj_add_flag(s_lowbatt_scrim, LV_OBJ_FLAG_HIDDEN);
        s_lowbatt_show_us = 0;
    }
    if (act == LOWBATT_WARN) lowbatt_show(soc);
}

// ---------- 关屏/设置弹窗 ----------

static void stats_close(void);  // 定义在下文播放统计弹窗段
static void menu_close(void);   // 定义在下文主菜单段
static void diag_close(void);   // 定义在下文硬件诊断页段
static void diag_open(void);    // menu_enter(下文)先于定义引用
static void quiz_open(void);    // menu_enter(下文)先于定义引用
static void quiz_close(void);   // screen_off(下文)先于定义引用
static void scene_close(void);  // screen_off(下文)先于定义引用
static void scene_open(void);   // menu_enter(下文)先于定义引用
static void alarm_close(void);  // screen_off(下文)先于定义引用
static void alarm_open(void);   // menu_enter(下文)先于定义引用

// 熄屏:停连发、关弹窗、停播放(含连播链)、背光 300ms 渐灭。渲染照常(仅背光不可见)。
// 聆听保护:播放中且未暂停且聆听开 → 只灭背光不停播(s_play_idx 保留,
// tick 尾部刷新以合法下标继续;熄屏渲染节流只跳过视觉)。暂停/非播放
// 不受保护(暂停+常暗=电池洞,与 d7d9c30 暂停熄屏停播同语义)。
static void screen_off(void) {
    s_screen_off = true;
    s_screen_off_us = esp_timer_get_time();
    app_ui_key_repeat_stop();
    if (s_modal.open) {
        modal_ctl_close(&s_modal);
        lv_obj_add_flag(s_modal_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_stats.open) stats_close();
    if (s_menu.open) menu_close();
    if (s_diag.open) diag_close();
    if (s_quiz_open) quiz_close();
    if (s_scene_open) scene_close();
    if (s_alarm_open) alarm_close();
    if (s_lowbatt_show_us) {  // 低电提醒:熄屏即收(会话闩 s_lowbatt_shown 不动)
        lv_obj_add_flag(s_lowbatt_scrim, LV_OBJ_FLAG_HIDDEN);
        s_lowbatt_show_us = 0;
    }
    if (!app_audio_listening() || !app_audio_active() || app_audio_paused()) {
        app_audio_stop();
        if (s_play_idx >= 0) ui_stop();
    }
    app_audio_counts_save();  // 熄屏落盘播放计数(下一落盘点是深睡前)
    bsp_display_backlight_fade(0, 300);
}

static void badge_refresh(int i);  // 定义在下文(熄屏期间计数补刷要用)

static void screen_wake(void) {
    s_screen_off = false;
    // 唤醒=新会话:清掉可能刚好在唤醒前一拍置位的深睡到期标志,否则
    // input_task 下一次 500ms 轮询仍会消费旧标志,把刚亮起的屏幕又送进深睡。
    // 与 tick_cb(置位方)同受 bsp_lvgl_lock 保护,无竞态。
    s_sleep_due = false;
    bsp_display_backlight_fade(100, 300);
    // 熄屏期间连播的条目只保持了"当前"一张的视觉跟随(其余条目计数在音频
    // 任务里默默累计),唤醒时全量补一次角标。
    for (int i = 0; i < N_CARDS; i++) badge_refresh(i);
}

// 弹窗懒构建一次(首次打开时);关闭=整层隐藏,子对象一起收。
// panel 高度按全行数(4 行)预留,modal_refresh 按可见行数实时缩放;
// 行位置由 modal_refresh 按可见序动态堆叠。
static void build_modal(lv_obj_t *scr) {
    s_modal_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_modal_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_modal_scrim, COL_MASK_OPA, 0);

    s_modal_panel = mk_box(s_modal_scrim, 16, 51, 208, 217);
    lv_obj_t *panel = s_modal_panel;
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);

    mk_label(panel, "设置", 12, 10, 80, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);

    static const char *names[MODAL_ITEM_COUNT] = {"音量", "连播", "随机", "聆听"};
    for (int i = 0; i < MODAL_ITEM_COUNT; i++) {
        lv_obj_t *row = mk_box(panel, 8, 34 + i * 40, 192, 36);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        s_modal_rows[i] = row;
        mk_label(row, names[i], 12, 9, 48, 17, &app_font_12, COL_INK,
                 LV_TEXT_ALIGN_LEFT, 0);
    }

    // 音量值:10 段条(段 8×12,间距 3,右对齐)。
    for (int k = 0; k < 10; k++) {
        s_modal_vol_segs[k] = mk_box(s_modal_rows[MODAL_ITEM_VOLUME],
                                     73 + k * 11, 12, 8, 12);
        lv_obj_set_style_bg_opa(s_modal_vol_segs[k], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_modal_vol_segs[k], 2, 0);
    }
    // 连播值:「开」/「关」。
    s_modal_ap_label = mk_label(s_modal_rows[MODAL_ITEM_AUTOPLAY], "关",
                                160, 9, 20, 17, &app_font_12, COL_INK,
                                LV_TEXT_ALIGN_RIGHT, 0);
    // 随机值:「开」/「关」。行本体在 modal_refresh 里随连播显隐。
    s_modal_rnd_label = mk_label(s_modal_rows[MODAL_ITEM_RANDOM], "开",
                                 160, 9, 20, 17, &app_font_12, COL_INK,
                                 LV_TEXT_ALIGN_RIGHT, 0);
    // 聆听值:「开」/「关」。行本体在 modal_refresh 里随连播显隐。
    s_modal_lst_label = mk_label(s_modal_rows[MODAL_ITEM_LISTEN], "关",
                                 160, 9, 20, 17, &app_font_12, COL_INK,
                                 LV_TEXT_ALIGN_RIGHT, 0);

    s_modal_hint = mk_label(panel, "上下键移动 确认键选中 长按退出", 8, 236, 192, 17,
                            &app_font_12, COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_modal_scrim, LV_OBJ_FLAG_HIDDEN);
}

// 按 modal 状态刷选中/编辑高亮与值(值单一来源:app_audio)。
// 行位置按可见序动态堆叠(隐藏行收起不留空),hint 紧跟末行;随机/聆听行
// 随连播显隐,位掩码同步给状态机(导航跳过隐藏行)。
static void modal_refresh(void) {
    int y = 34;
    for (int i = 0; i < MODAL_ITEM_COUNT; i++) {
        bool hidden = (i == MODAL_ITEM_RANDOM || i == MODAL_ITEM_LISTEN) &&
                      !app_audio_autoplay();
        modal_ctl_hidden_set(&s_modal, i, hidden);
        if (hidden) {
            lv_obj_add_flag(s_modal_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_modal_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(s_modal_rows[i], y);
        y += 40;
        bool sel = s_modal.item == i;
        bool editing = sel && s_modal.editing;
        lv_obj_set_style_bg_color(s_modal_rows[i],
                                  lv_color_hex(sel ? COL_CARD_FOCUS : COL_CARD), 0);
        lv_obj_set_style_border_color(s_modal_rows[i],
                                      lv_color_hex(editing ? COL_MODAL_EDIT : COL_LINE), 0);
        lv_obj_set_style_border_opa(s_modal_rows[i],
                                    editing ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    lv_obj_set_y(s_modal_hint, y + 2);
    // panel 高度随可见行数自适应并垂直居中:标题区 34 + 行区 n*40 + hint
    // 17 + 2 + 底边 4 = n*40 + 57。n=3 → 177、n=5 → 257(满配),
    // 弹窗内切连播 = 行数变化,本函数即重算,
    // LVGL 延迟刷新下同帧原子缩放,无中间闪烁。
    int n = (y - 34) / 40;
    int h = n * 40 + 57;
    lv_obj_set_height(s_modal_panel, h);
    lv_obj_set_y(s_modal_panel, (APP_SCREEN_H - h) / 2);
    int filled = app_audio_volume() / 10;
    for (int k = 0; k < 10; k++) {
        lv_obj_set_style_bg_color(s_modal_vol_segs[k],
                                  lv_color_hex(k < filled ? COL_INK : COL_TRACK), 0);
    }
    lv_label_set_text(s_modal_ap_label, app_audio_autoplay() ? "开" : "关");
    if (!app_audio_autoplay()) {
        lv_label_set_text(s_modal_rnd_label, "关");
        lv_label_set_text(s_modal_lst_label, "关");
    } else {
        lv_label_set_text(s_modal_rnd_label, app_audio_random() ? "开" : "关");
        lv_label_set_text(s_modal_lst_label, app_audio_listening() ? "开" : "关");
    }
    lv_label_set_text(s_modal_hint, s_modal.editing ? "上下键调整 确认键完成 长按退出"
                                                     : "上下键移动 确认键选中 长按退出");
    update_volume_indicator();  // 音量编辑/连调即时反馈(其余项值不变,幂等早退)
}

static void modal_open(void) {
    // 停掉可能活着的宫格连发(按住 UP/DOWN 连滚中再长按 OK 开弹窗):repeat_cb
    // 每拍刷闲置基准,弹窗 15s 自动关/60s 熄屏/5min 深睡会全部失效;且松开
    // 方向键时 browse 态 RELEASE 返回 NONE 不停它,之后进音量编辑态会变成
    // 没有任何 RELEASE 能停的幻影连调。与 screen_off/stats_close 对称。
    app_ui_key_repeat_stop();
    if (!s_modal_scrim) build_modal(lv_screen_active());
    modal_ctl_open(&s_modal);
    lv_obj_clear_flag(s_modal_scrim, LV_OBJ_FLAG_HIDDEN);
    modal_refresh();
}

// 关弹窗:幂等收尾。OK 长按路径里 modal_ctl_key 已先把 open 置 false,这里
// 不能再看 open 早退(否则遮罩永不隐藏、连发不停,后续事件全部穿透)——
// 无条件清状态、停连发(编辑音量连调中关弹窗会跑焦点)、隐藏遮罩。
static void modal_close(void) {
    modal_ctl_close(&s_modal);
    app_ui_key_repeat_stop();
    app_audio_settings_save();  // 弹窗关闭时落盘音量/连播(脏则写,合并连调步进)
    if (s_modal_scrim) lv_obj_add_flag(s_modal_scrim, LV_OBJ_FLAG_HIDDEN);
}

// ---------- 播放统计弹窗 ----------
// 懒构建一次;打开时快照排序(播放期间计数变化不实时刷新,重开即新快照)。
// 排行榜版式:前三名奖牌图标,第 4+ 名数字序号(x 与标题绝对坐标对齐);
// 副标题快照求和 Σ次数/Σ秒数。
static void build_stats(lv_obj_t *scr) {
    s_stats_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_stats_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_stats_scrim, COL_MASK_OPA, 0);
    lv_obj_t *panel = mk_box(s_stats_scrim, 16, 8, 208, 304);
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    mk_label(panel, "播放统计", 12, 10, 100, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    s_stats_sub = mk_label(panel, "", 12, 34, 184, 17, &app_font_12, COL_INK3,
                           LV_TEXT_ALIGN_LEFT, 0);
    // 视口容器(透明纯裁剪,镜像宫格 viewport 模式):列表只画在「标题以下、
    // 提示以上」区域,滚动行结构性盖不到标题/hint(LVGL 裁剪=父坐标∩父
    // 裁剪区;没有这层,列表会裁剪到整个 panel)。
    s_stats_view = mk_box(panel, 8, 56, 192, STATS_VIS_ROWS * STATS_ROW_H);
    // 列表盒(高 = 全行数,挂 view 下,y 基准 0):滚动 = y 位移钳制为负偏移。
    s_stats_list = mk_box(s_stats_view, 0, 0, 192, N_CARDS * STATS_ROW_H);
    for (int i = 0; i < N_CARDS; i++) {
        if (i < 3) {
            // 前三名:奖牌图标(金/银/铜),绘制含中心名次数字。
            static const int medal_icon[3] = {
                UI_ICON_MEDAL_GOLD, UI_ICON_MEDAL_SILVER, UI_ICON_MEDAL_BRONZE};
            static const int medal_col[3] = {
                COL_MEDAL_GOLD, COL_MEDAL_SILVER, COL_MEDAL_BRONZE};
            s_stats_marks[i] = mk_icon(s_stats_list, medal_icon[i], 2,
                                       i * STATS_ROW_H + 4, 14, 14, medal_col[i]);
        } else {
            // 第 4+ 名:数字序号(font_6_bold,右对齐到奖牌列宽)。
            // x=4 与标题绝对坐标对齐(panel16+border2+view8+list4 = 标题 panel x12)。
            s_stats_marks[i] = mk_label(s_stats_list, "", 4, i * STATS_ROW_H + 6,
                                        14, 9, &app_font_6_bold, COL_INK3,
                                        LV_TEXT_ALIGN_LEFT, 0);
        }
        s_stats_names[i] = mk_label(s_stats_list, "", 22, i * STATS_ROW_H + 2,
                                    114, 17, &app_font_12, COL_INK,
                                    LV_TEXT_ALIGN_LEFT, 0);
        s_stats_counts[i] = mk_label(s_stats_list, "", 140, i * STATS_ROW_H + 5,
                                     44, 9, &app_font_6_bold, COL_INK3,
                                     LV_TEXT_ALIGN_RIGHT, 0);
    }
    mk_label(panel, "上下键滚动 确认键返回", 8, 274, 192, 17, &app_font_12,
             COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_stats_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void stats_open(void) {
    if (!s_stats_scrim) build_stats(lv_screen_active());
    // 排序快照:次数降序,同次数保持目录序(稳定插入排序,n=24 一次性)。
    int order[N_CARDS];
    for (int i = 0; i < N_CARDS; i++) order[i] = i;
    for (int i = 1; i < N_CARDS; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0 && app_audio_play_count(order[j]) < app_audio_play_count(v)) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    unsigned total_cnt = 0, total_sec = 0;
    // 副标题 buf:最坏「总次数 239976 · 总时长 26214分40秒」= 44B(UTF-8 3B/汉字),取 48。
    char buf[48];
    for (int i = 0; i < N_CARDS; i++) {
        total_cnt += app_audio_play_count(order[i]);
        total_sec += app_audio_play_secs(order[i]);
        if (i >= 3) {  // 前三名奖牌数字在图标内,第 4+ 名填序号文本
            snprintf(buf, sizeof(buf), "%d", i + 1);
            lv_label_set_text(s_stats_marks[i], buf);
        }
        lv_label_set_text(s_stats_names[i], app_catalog(order[i])->name);
        snprintf(buf, sizeof(buf), "%u", app_audio_play_count(order[i]));
        lv_label_set_text(s_stats_counts[i], buf);
    }
    // 副标题:<60s 整秒;≥60s「x分y秒」(y=0 也显示,如 2分0秒)。
    if (total_sec < 60) {
        snprintf(buf, sizeof(buf), "总次数 %u · 总时长 %u 秒", total_cnt, total_sec);
    } else {
        snprintf(buf, sizeof(buf), "总次数 %u · 总时长 %u分%u秒", total_cnt,
                 total_sec / 60, total_sec % 60);
    }
    lv_label_set_text(s_stats_sub, buf);
    s_stats.scroll = 0;
    lv_obj_set_y(s_stats_list, 0);
    s_stats.open = true;
    lv_obj_clear_flag(s_stats_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void stats_close(void) {
    s_stats.open = false;
    app_ui_key_repeat_stop();
    if (s_stats_scrim) lv_obj_add_flag(s_stats_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void stats_scroll_by(int d) {
    // 上限防御:目录数 < 可视行数的未来目录不会产生负上限(当前 24-10=14)。
    int max = N_CARDS - STATS_VIS_ROWS;
    if (max < 0) max = 0;
    int s = s_stats.scroll + d;
    if (s < 0) s = 0;
    if (s > max) s = max;
    if (s == s_stats.scroll) return;
    s_stats.scroll = s;
    lv_obj_set_y(s_stats_list, -s * STATS_ROW_H);  // 列表在 view 内负偏移
}

// ---------- 主菜单 ----------
// OK 长按弹出的功能入口(modal/scrim 同款版式)。行数随实现批次生长
// (MENU_ITEM_*), 行固定编译期, 无 hidden/editing 语义, 状态机在 menu_ctl.h
// (host 可测);状态与对象声明在文件头部(关屏段 screen_off 先于本段引用)。
static void build_menu(lv_obj_t *scr) {
    s_menu_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_menu_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_menu_scrim, COL_MASK_OPA, 0);

    // 高度公式与设置弹窗同源: 标题区 34 + 行区 n*40 + hint 17 + 2 + 底边 4。
    int h = MENU_ITEM_COUNT * 40 + 57;
    s_menu_panel = mk_box(s_menu_scrim, 16, (APP_SCREEN_H - h) / 2, 208, h);
    lv_obj_t *panel = s_menu_panel;
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);

    mk_label(panel, "菜单", 12, 10, 80, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    static const char *names[MENU_ITEM_COUNT] = {"听声辨机", "场景连播", "闹钟",
                                                 "播放统计", "设置", "硬件诊断"};
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        lv_obj_t *row = mk_box(panel, 8, 34 + i * 40, 192, 36);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        s_menu_rows[i] = row;
        mk_label(row, names[i], 12, 9, 120, 17, &app_font_12, COL_INK,
                 LV_TEXT_ALIGN_LEFT, 0);
    }
    mk_label(panel, "上下键移动 确认键选中 长按退出", 8,
             MENU_ITEM_COUNT * 40 + 36, 192, 17, &app_font_12, COL_INK3,
             LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_menu_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void menu_refresh(void) {
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        lv_obj_set_style_bg_color(s_menu_rows[i],
                                  lv_color_hex(i == s_menu.item ? COL_CARD_FOCUS
                                                                : COL_CARD), 0);
        lv_obj_set_style_border_color(s_menu_rows[i], lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_border_opa(s_menu_rows[i], LV_OPA_40, 0);
    }
}

static void menu_open(void) {
    // 停连发(同 modal_open 语义, 37b7ecd): 连滚中长按 OK 开菜单不得泄漏
    // repeat(闲置时间线失效 + 幻影连调同源)。
    app_ui_key_repeat_stop();
    if (!s_menu_scrim) build_menu(lv_screen_active());
    menu_ctl_open(&s_menu, MENU_ITEM_COUNT);
    lv_obj_clear_flag(s_menu_scrim, LV_OBJ_FLAG_HIDDEN);
    menu_refresh();
}

static void menu_close(void) {
    menu_ctl_close(&s_menu);
    app_ui_key_repeat_stop();
    if (s_menu_scrim) lv_obj_add_flag(s_menu_scrim, LV_OBJ_FLAG_HIDDEN);
}

// 行进入派发: 菜单先收(隐层/停连发), 再移交目标页(与原 MODAL_STATS_OPEN
// 同款交接: 关旧开新, 一次只开一个弹窗)。后续批次在此追加行。
static void menu_enter(int item) {
    switch (item) {
    case MENU_ITEM_QUIZ:
        menu_close();
        quiz_open();
        break;
    case MENU_ITEM_SCENES:
        menu_close();
        scene_open();
        break;
    case MENU_ITEM_ALARM:
        menu_close();
        alarm_open();
        break;
    case MENU_ITEM_STATS:
        menu_close();
        stats_open();
        break;
    case MENU_ITEM_SETTINGS:
        menu_close();
        modal_open();
        break;
    case MENU_ITEM_DIAG:
        menu_close();
        diag_open();
        break;
    default:
        break;
    }
}

// ---------- 硬件诊断页 ----------
// 只读滚动列表(stats 弹窗同款: 打开时快照, 不实时轮询; view 裁剪 + 负偏移滚动)。
// 三段: 电池(电压/SOC/CW2017 版本) / I2C(冷启动扫描缓存) / 运行记录(环形日志
// 新→旧, 含本次)。复位原因短名表在 bootlog.c(reset_reason_name, 与启动日志共用)。
// 运行记录行格式(含义见下方 wc/site 注释):
//   `[复位原因] 唤[源] [站N] [N分|断电][ · 本次]`
// 状态与对象声明在文件头部(screen_off 先引用)。
#define DIAG_ROW_H   22   // 行高(与 stats 同款)
#define DIAG_VIS_ROWS 10  // 可视行数

static void diag_build(lv_obj_t *scr) {
    s_diag_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_diag_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_diag_scrim, COL_MASK_OPA, 0);
    lv_obj_t *panel = mk_box(s_diag_scrim, 16, 8, 208, 304);
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    mk_label(panel, "硬件诊断", 12, 10, 100, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    mk_label(panel, "上下键滚动 确认键返回", 8, 274, 192, 17, &app_font_12,
             COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
    // 视口容器(纯裁剪, 与统计弹窗同款): 列表只画在标题以下、hint 以上。
    s_diag_view = mk_box(panel, 8, 56, 192, DIAG_VIS_ROWS * DIAG_ROW_H);
    s_diag_list = mk_box(s_diag_view, 0, 0, 192, 0);  // 高度在 open 时按行数定
}

static int diag_y;  // diag_row 追加 y 游标(0 起)

// 追加一行: label 文本 + 颜色(标题行用强调色, 数据行用主色/次色)。
static lv_obj_t *diag_row(const char *text, uint32_t col) {
    lv_obj_t *l = mk_label(s_diag_list, "", 2, diag_y, 188, 17, &app_font_12, col,
                           LV_TEXT_ALIGN_LEFT, 1);
    diag_y += DIAG_ROW_H;
    lv_label_set_text(l, text);
    return l;
}

static void diag_open(void) {
    if (!s_diag_scrim) diag_build(lv_screen_active());
    diag_y = 0;
    char buf[64];

    // ---- 电池段 ----
    int mv = bsp_battery_mv(), soc = bsp_battery_soc(), ver = bsp_battery_version();
    diag_row("电池", COL_INK);
    if (mv < 0 && soc < 0) {
        diag_row("  读取失败", COL_INK3);
    } else {
        snprintf(buf, sizeof(buf), "  电压 %d mV", mv < 0 ? 0 : mv);
        diag_row(buf, COL_INK2);
        snprintf(buf, sizeof(buf), "  电量 %d%%", soc < 0 ? 0 : soc);
        diag_row(buf, COL_INK2);
    }
    snprintf(buf, sizeof(buf), "  CW2017 版本 0x%02X", ver < 0 ? 0 : ver);
    diag_row(buf, COL_INK2);

    // ---- I2C 段 ----
    diag_row("I2C 总线", COL_INK);
    const uint8_t *addrs; int cnt; bool aborted, done;
    bsp_i2c_scan_cache(&addrs, &cnt, &aborted, &done);
    if (!done) {
        diag_row("  本次未扫描", COL_INK3);
    } else if (aborted) {
        diag_row("  扫描中止(疑似卡死)", COL_BATT_LOW);
    } else if (cnt == 0) {
        diag_row("  无设备", COL_INK3);
    } else {
        for (int i = 0; i < cnt; i++) {
            snprintf(buf, sizeof(buf), "  0x%02X%s", addrs[i],
                     addrs[i] == 0x18 ? " ES8311" : addrs[i] == 0x63 ? " CW2017" : "");
            diag_row(buf, COL_INK2);
        }
    }

    // ---- 运行记录段 ----
    // wc(唤醒源): 0=冷启动/软重启(无唤醒源, 显示 "-") / 4=TIMER(闹钟到时) /
    //   7=GPIO(按键)。
    // site(重启站点): 0=正常入睡或断电截断(省略, 由分钟字段区分) /
    //   1=GPIO 武装失败 2=按键交回失败 3=LVGL 锁失败 4=深睡意外返回 5=TIMER
    //   武装失败——非 0 即深睡序列中途 esp_restart 重启, 站点定位失败步骤。
    // run_min: 0 且无 touch = 无人收尾的断电截断(诚实显示「断电」)。
    diag_row("运行记录", COL_INK);
    const bootlog_t *log = bootlog_snapshot();
    bootlog_rec_t r;
    for (int n = 0; bootlog_get(log, n, &r); n++) {
        char wcbuf[4], sitebuf[8];
        if (r.wc == 0) strcpy(wcbuf, "-");
        else snprintf(wcbuf, sizeof(wcbuf), "%d", r.wc);
        if (r.site == 0) sitebuf[0] = '\0';
        else snprintf(sitebuf, sizeof(sitebuf), " 站%d", r.site);
        if (n == 0) {
            // 本次运行永不显示时长/断电: touch 只在结束时落账, 运行中的
            // 当前槽 run_min 恒 0, 显示「断电」是误导。
            snprintf(buf, sizeof(buf), "  %s 唤%s%s 运行中", reset_reason_name(r.rr),
                     wcbuf, sitebuf);
        } else if (r.run_min > 0) {
            snprintf(buf, sizeof(buf), "  %s 唤%s%s %u分", reset_reason_name(r.rr),
                     wcbuf, sitebuf, r.run_min);
        } else {
            snprintf(buf, sizeof(buf), "  %s 唤%s%s 断电", reset_reason_name(r.rr),
                     wcbuf, sitebuf);
        }
        diag_row(buf, n == 0 ? COL_INK : COL_INK2);
    }
    if (log->count == 0) diag_row("  无记录", COL_INK3);

    int rows = diag_y / DIAG_ROW_H;
    s_diag.scroll = 0;
    lv_obj_set_height(s_diag_list, rows * DIAG_ROW_H);
    lv_obj_set_y(s_diag_list, 0);
    s_diag.open = true;
    lv_obj_clear_flag(s_diag_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void diag_close(void) {
    s_diag.open = false;
    app_ui_key_repeat_stop();
    if (s_diag_scrim) lv_obj_add_flag(s_diag_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void diag_scroll_by(int d) {
    int rows = diag_y / DIAG_ROW_H;  // open 时快照的行数(diag_y 不再变)
    int max = rows - DIAG_VIS_ROWS;
    if (max < 0) max = 0;
    int s = s_diag.scroll + d;
    if (s < 0) s = 0;
    if (s > max) s = max;
    s_diag.scroll = s;
    lv_obj_set_y(s_diag_list, -s * DIAG_ROW_H);
}

// ---------- 听声辨机 ----------
// 三子页共用一套 scrim/panel(组容器隐藏切换): 模式选择(menu_ctl 复用, 2 行) /
// 答题(题面 + 三选项行, 三键直接 = 三选项一步确认, 按键物理位映射:
// 上键(顶)→上项/下键(中)→中项/确认键(底)→下项, 见 bsp_pins.h) /
// 结算(成绩 + 纪录)。出题/判定/计时在 quiz_ctl.h(host 已测);本段只做映射:
// 出题 → 渲染 + 听音题播一次(play_once, 连播开着也不接棒)/判定 → 行着色反馈
// (答对绿/答错所选红 + 正确项绿;静态着色,不做闪烁动画)+ 答错回放正确答案音;
// WAIT 到时由 tick 调 quiz_resume 推进(下一题或结算)。答对的题面音已播过,
// 反馈期不再加播(800ms 静默即反馈)。状态与对象声明在文件头部(screen_off/
// 输入路由/tick 先引用)。
#define QUIZ_NS "gpws_quiz"  // 纪录 NVS namespace(app_audio 的 gpws_app 之外独立)

// 出题随机源(设备侧);host 测试注入确定性 LCG(见 quiz_ctl.h 的 rng 参数)。
static uint32_t quiz_rng(void) { return esp_random(); }

static void quiz_show(lv_obj_t *grp) {
    lv_obj_add_flag(s_quiz_mode_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_quiz_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_quiz_mode_grp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_quiz_ask_grp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_quiz_sum_grp, LV_OBJ_FLAG_HIDDEN);
    if (!grp) return;
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_HIDDEN);
    // 模式组挂在小面板, 答题/结算组挂在大面板。
    lv_obj_clear_flag(grp == s_quiz_mode_grp ? s_quiz_mode_scrim : s_quiz_scrim,
                      LV_OBJ_FLAG_HIDDEN);
}

static void quiz_build(lv_obj_t *scr) {
    // ---- 模式选择页: 独立小面板(2 行, 菜单同款高度公式) ----
    s_quiz_mode_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_quiz_mode_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_quiz_mode_scrim, COL_MASK_OPA, 0);
    int mh = 2 * 40 + 57;  // 标题区 34 + 行区 2*40 + hint 17+2+4
    s_quiz_mode_panel = mk_box(s_quiz_mode_scrim, 16, (APP_SCREEN_H - mh) / 2, 208, mh);
    lv_obj_t *mp = s_quiz_mode_panel;
    lv_obj_set_style_radius(mp, 11, 0);
    lv_obj_set_style_bg_color(mp, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(mp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mp, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(mp, 2, 0);
    lv_obj_set_style_border_opa(mp, LV_OPA_COVER, 0);
    mk_label(mp, "听声辨机", 12, 10, 100, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    s_quiz_mode_grp = mk_box(mp, 0, 0, 208, mh);
    static const char *modes[2] = {"10 题挑战", "无尽模式"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = mk_box(s_quiz_mode_grp, 8, 34 + i * 40, 192, 36);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        s_quiz_mode_rows[i] = row;
        mk_label(row, modes[i], 12, 9, 168, 17, &app_font_12, COL_INK,
                 LV_TEXT_ALIGN_LEFT, 0);
    }
    mk_label(s_quiz_mode_grp, "上下键选择 确认开始 长按退出", 8, 2 * 40 + 36,
             192, 17, &app_font_12, COL_INK3, LV_TEXT_ALIGN_CENTER, 0);

    // ---- 答题/结算页: 共用大面板 ----
    s_quiz_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_quiz_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_quiz_scrim, COL_MASK_OPA, 0);
    s_quiz_panel = mk_box(s_quiz_scrim, 16, 8, 208, 304);
    lv_obj_t *panel = s_quiz_panel;
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    mk_label(panel, "听声辨机", 12, 10, 100, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);

    // ---- 答题组 ----
    s_quiz_ask_grp = mk_box(panel, 0, 0, 208, 304);
    s_quiz_icon_listen = mk_icon(s_quiz_ask_grp, APP_ICON_VOICE, 8, 41, 12, 12, COL_INK2);
    s_quiz_icon_scn = mk_icon(s_quiz_ask_grp, APP_ICON_RADAR, 8, 41, 12, 12, COL_INK2);
    s_quiz_qline = mk_label(s_quiz_ask_grp, "", 26, 40, 166, 17, &app_font_12,
                            COL_INK, LV_TEXT_ALIGN_LEFT, 0);
    s_quiz_qtext = mk_label(s_quiz_ask_grp, "", 8, 64, 192, 38, &app_font_12,
                            COL_INK, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_quiz_qtext, LV_LABEL_LONG_WRAP);  // 场景句按宽折行(至多两行)
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = mk_box(s_quiz_ask_grp, 8, 112 + i * 48, 192, 42);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        s_quiz_opt_rows[i] = row;
        s_quiz_opt_labels[i] = mk_label(row, "", 12, 12, 168, 17, &app_font_12,
                                        COL_INK, LV_TEXT_ALIGN_LEFT, 0);
    }
    mk_label(s_quiz_ask_grp, "上键 下键 确认键 进行选择", 8, 274, 192, 17,
             &app_font_12, COL_INK3, LV_TEXT_ALIGN_CENTER, 0);

    // ---- 结算组 ----
    s_quiz_sum_grp = mk_box(panel, 0, 0, 208, 304);
    s_quiz_sum_big = mk_label(s_quiz_sum_grp, "", 8, 96, 192, 22, &app_font_15_bold,
                              COL_INK, LV_TEXT_ALIGN_CENTER, 0);
    s_quiz_sum_streak = mk_label(s_quiz_sum_grp, "", 8, 130, 192, 17, &app_font_12,
                                 COL_INK2, LV_TEXT_ALIGN_CENTER, 0);
    s_quiz_sum_best = mk_label(s_quiz_sum_grp, "", 8, 168, 192, 17, &app_font_12,
                               COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
    s_quiz_sum_new = mk_label(s_quiz_sum_grp, "新纪录！", 8, 200, 192, 17,
                              &app_font_12, COL_MEDAL_GOLD, LV_TEXT_ALIGN_CENTER, 0);
    mk_label(s_quiz_sum_grp, "确认键返回", 8, 274, 192, 17, &app_font_12,
             COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
}

// ---- 纪录持久化(懒读一次;写只在破纪录的结算时刻, 一局至多一次) ----
static void quiz_records_load(void) {
    s_quiz_recs_loaded = true;
    nvs_handle_t h;
    if (nvs_open(QUIZ_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t v;
    if (nvs_get_u16(h, "quiz_best_round", &v) == ESP_OK) s_quiz_best_round = v;
    if (nvs_get_u16(h, "quiz_best_streak", &v) == ESP_OK) s_quiz_best_streak = v;
    nvs_close(h);
}

static void quiz_record_save(const char *key, uint16_t v) {
    nvs_handle_t h;
    if (nvs_open(QUIZ_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u16(h, key, v);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void quiz_mode_refresh(void) {
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(s_quiz_mode_rows[i],
                                  lv_color_hex(i == s_quiz_mode.item ? COL_CARD_FOCUS
                                                                     : COL_CARD), 0);
        lv_obj_set_style_border_color(s_quiz_mode_rows[i], lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_border_opa(s_quiz_mode_rows[i], LV_OPA_40, 0);
    }
}

// 选项行着色: kind 0=中性 / 1=对(绿) / 2=错(红)。
static void quiz_opt_color(int i, int kind) {
    lv_obj_set_style_bg_color(s_quiz_opt_rows[i],
                              lv_color_hex(kind == 1 ? COL_RIGHT_BG
                                            : kind == 2 ? COL_PAUSE_BG : COL_CARD), 0);
    lv_obj_set_style_border_color(s_quiz_opt_rows[i],
                                  lv_color_hex(kind == 1 ? COL_RIGHT
                                                : kind == 2 ? COL_BATT_LOW : COL_LINE), 0);
    lv_obj_set_style_border_opa(s_quiz_opt_rows[i], kind ? LV_OPA_COVER : LV_OPA_40, 0);
}

// 渲染当前题(ASK): 题面 + 三选项复位着色;听音题播一次题面音。
static void quiz_ask_render(void) {
    char buf[48];
    bool scn = s_quiz.scenario;
    snprintf(buf, sizeof(buf), "第 %d 题 · %s", s_quiz.question, scn ? "场景" : "听音");
    lv_label_set_text(s_quiz_qline, buf);
    lv_obj_add_flag(scn ? s_quiz_icon_listen : s_quiz_icon_scn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(scn ? s_quiz_icon_scn : s_quiz_icon_listen, LV_OBJ_FLAG_HIDDEN);
    if (scn) {
        lv_label_set_text(s_quiz_qtext, QUIZ_SCENARIO[s_quiz.correct]);
        lv_obj_clear_flag(s_quiz_qtext, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_quiz_qtext, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; i++) {
        lv_label_set_text(s_quiz_opt_labels[i], app_catalog(s_quiz.options[i])->name);
        quiz_opt_color(i, 0);
    }
    if (!scn) app_audio_play_once(s_quiz.correct);
}

// 判定(ASK 时三键之一): 状态机记分 + 行着色反馈;答错回放正确答案音。
static void quiz_answer_pick(int pick) {
    bool right = quiz_answer(&s_quiz, pick, esp_timer_get_time());
    int ci = 0;
    for (int i = 0; i < 3; i++) {
        if (s_quiz.options[i] == s_quiz.correct) ci = i;
    }
    if (right) {
        quiz_opt_color(pick, 1);
    } else {
        quiz_opt_color(pick, 2);
        quiz_opt_color(ci, 1);
        app_audio_play_once(s_quiz.correct);  // 正确答案回放(场景题也播, 强化记忆)
    }
}

// 结算页渲染: 成绩 + 纪录对比(破纪录即写 NVS 并标注「新纪录」)。
static void quiz_sum_render(void) {
    char buf[48];
    bool round = s_quiz.mode == QUIZ_MODE_ROUND;
    if (round) {
        snprintf(buf, sizeof(buf), "答对%d/10", s_quiz.score);
        lv_label_set_text(s_quiz_sum_big, buf);
        snprintf(buf, sizeof(buf), "最长连对 %d", s_quiz.best_streak);
        lv_label_set_text(s_quiz_sum_streak, buf);
        lv_obj_clear_flag(s_quiz_sum_streak, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "最佳%d/10", s_quiz_best_round);
        lv_label_set_text(s_quiz_sum_best, buf);
    } else {
        snprintf(buf, sizeof(buf), "连对%d", s_quiz.score);
        lv_label_set_text(s_quiz_sum_big, buf);
        lv_obj_add_flag(s_quiz_sum_streak, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "最佳连对 %d", s_quiz_best_streak);
        lv_label_set_text(s_quiz_sum_best, buf);
    }
    bool is_new = s_quiz.score > (round ? s_quiz_best_round : s_quiz_best_streak);
    if (is_new) {
        if (round) s_quiz_best_round = (uint16_t)s_quiz.score;
        else s_quiz_best_streak = (uint16_t)s_quiz.score;
        quiz_record_save(round ? "quiz_best_round" : "quiz_best_streak",
                         (uint16_t)s_quiz.score);
        lv_obj_clear_flag(s_quiz_sum_new, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_quiz_sum_new, LV_OBJ_FLAG_HIDDEN);
    }
    quiz_show(s_quiz_sum_grp);
}

// 开局(模式页 OK): 关模式页 → quiz_start 出第一题 → 答题页。
static void quiz_start_mode(void) {
    menu_ctl_close(&s_quiz_mode);
    quiz_start(&s_quiz, s_quiz_mode.item == 0 ? QUIZ_MODE_ROUND : QUIZ_MODE_ENDLESS,
               quiz_rng);
    quiz_show(s_quiz_ask_grp);
    quiz_ask_render();
}

static void quiz_open(void) {
    app_ui_key_repeat_stop();
    app_audio_stop();  // 互斥: 进 Quiz 即停场景/连播(spec §7), 题面音是唯一声源
    if (!s_quiz_scrim) quiz_build(lv_screen_active());
    if (!s_quiz_recs_loaded) quiz_records_load();
    menu_ctl_open(&s_quiz_mode, 2);
    s_quiz.state = QUIZ_IDLE;  // quiz_ctl 未开局(模式页;答题从 quiz_start_mode 起)
    s_quiz_open = true;
    quiz_show(s_quiz_mode_grp);
    quiz_mode_refresh();
}

static void quiz_close(void) {
    s_quiz_open = false;
    menu_ctl_close(&s_quiz_mode);
    s_quiz.state = QUIZ_IDLE;
    app_ui_key_repeat_stop();
    app_audio_stop();  // 半途退出: 题面音/答案回放随关(结算后音频本已自然结束)
    if (s_quiz_mode_scrim) lv_obj_add_flag(s_quiz_mode_scrim, LV_OBJ_FLAG_HIDDEN);
    if (s_quiz_scrim) lv_obj_add_flag(s_quiz_scrim, LV_OBJ_FLAG_HIDDEN);
}

// ---------- 场景连播 ----------
// 列表页(menu_ctl 复用, 6 行 = 6 场景)。行 = 场景名 + 条数;OK 开始 =
// app_audio_play_scene 按表播完自动停(打断一切在播——含 Quiz 音, 互斥由
// 弹窗互斥 + play_scene 打断双保险)并关页回宫格, seq 视觉跟随现成。
// 播放中交互全兼容(宫格态): OK 单击暂停/继续, 双击关屏聆听照旧。
static void scene_build(lv_obj_t *scr) {
    s_scene_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_scene_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_scene_scrim, COL_MASK_OPA, 0);
    lv_obj_t *panel = mk_box(s_scene_scrim, 16, 8, 208, 304);
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    mk_label(panel, "场景连播", 12, 10, 100, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    for (int i = 0; i < APP_SCENE_COUNT; i++) {
        lv_obj_t *row = mk_box(panel, 8, 44 + i * 40, 192, 36);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        s_scene_rows[i] = row;
        mk_label(row, APP_SCENES[i].name, 12, 9, 128, 17, &app_font_12, COL_INK,
                 LV_TEXT_ALIGN_LEFT, 0);
        char buf[12];
        snprintf(buf, sizeof(buf), "%d 条", APP_SCENES[i].n);
        mk_label(row, buf, 144, 9, 44, 17, &app_font_12, COL_INK3,
                 LV_TEXT_ALIGN_RIGHT, 0);
    }
    mk_label(panel, "上下键选择 确认播放 长按退出", 8, 280, 192, 17,
             &app_font_12, COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
}

static void scene_refresh(void) {
    for (int i = 0; i < APP_SCENE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_scene_rows[i],
                                  lv_color_hex(i == s_scene_sel.item ? COL_CARD_FOCUS
                                                                     : COL_CARD), 0);
        lv_obj_set_style_border_color(s_scene_rows[i], lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_border_opa(s_scene_rows[i], LV_OPA_40, 0);
    }
}

static void scene_open(void) {
    app_ui_key_repeat_stop();
    if (!s_scene_scrim) scene_build(lv_screen_active());
    menu_ctl_open(&s_scene_sel, APP_SCENE_COUNT);
    s_scene_open = true;
    scene_refresh();
    lv_obj_clear_flag(s_scene_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void scene_close(void) {
    s_scene_open = false;
    menu_ctl_close(&s_scene_sel);
    app_ui_key_repeat_stop();
    if (s_scene_scrim) lv_obj_add_flag(s_scene_scrim, LV_OBJ_FLAG_HIDDEN);
}

// ---------- GPWS 闹钟 ----------
// 设置页: alarm_ctl.h 状态机(浏览/编辑两态, 时长 ±1 钳 1..120 / 选曲环绕,
// 时长长按连调 ±5/拍复用 repeat timer)。OK 长按(浏览态) → 确认层 1s →
// input_task 深睡(timer+GPIO 双武装)。响铃页: TIMER 唤醒后由 main 调
// app_ui_alarm_ring 进入——单曲循环 + 波形呼吸 + 任意键停止回主界面。
#define ALARM_CONFIRM_US (1LL * 1000 * 1000)  // 确认层展示时长(此后深睡)

// 闹钟值 NVS 持载(跨深睡唯一载体; main.c 唤醒分支消费清除)。
// namespace 与 app_audio 设置共用 gpws_app, 键名不冲突。
static void alarm_nvs_save(int min, int tune) {
    nvs_handle_t h;
    if (nvs_open("gpws_app", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "alarm_min", (uint8_t)min);
    (void)nvs_set_u8(h, "alarm_tune", (uint8_t)tune);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void alarm_build(lv_obj_t *scr) {
    s_alarm_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_alarm_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_alarm_scrim, COL_MASK_OPA, 0);
    // 高度公式与菜单同源: 标题区 34 + 行区 2*40 + hint 17 + 2 + 底边 4。
    int h = ALARM_ROW_COUNT * 40 + 57;
    lv_obj_t *panel = mk_box(s_alarm_scrim, 16, (APP_SCREEN_H - h) / 2, 208, h);
    lv_obj_set_style_radius(panel, 11, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    mk_label(panel, "闹钟", 12, 10, 80, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    static const char *names[ALARM_ROW_COUNT] = {"时长", "选曲"};
    for (int i = 0; i < ALARM_ROW_COUNT; i++) {
        lv_obj_t *row = mk_box(panel, 8, 34 + i * 40, 192, 36);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        s_alarm_rows[i] = row;
        mk_label(row, names[i], 12, 9, 60, 17, &app_font_12, COL_INK,
                 LV_TEXT_ALIGN_LEFT, 0);
        s_alarm_val_labels[i] = mk_label(row, "", 70, 9, 118, 17, &app_font_12,
                                         COL_INK2, LV_TEXT_ALIGN_RIGHT, 0);
    }
    s_alarm_hint = mk_label(panel, "", 8, ALARM_ROW_COUNT * 40 + 36, 192, 17,
                            &app_font_12, COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_alarm_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void alarm_refresh(void) {
    for (int i = 0; i < ALARM_ROW_COUNT; i++) {
        lv_obj_set_style_bg_color(s_alarm_rows[i],
                                  lv_color_hex(i == s_alarm.row && !s_alarm.editing
                                                   ? COL_CARD_FOCUS : COL_CARD), 0);
        // 编辑行描边强调(设置弹窗同款)
        lv_obj_set_style_border_color(s_alarm_rows[i],
                                      lv_color_hex(s_alarm.editing && i == s_alarm.row
                                                       ? COL_MODAL_EDIT : COL_LINE), 0);
        lv_obj_set_style_border_opa(s_alarm_rows[i],
                                    s_alarm.editing && i == s_alarm.row
                                        ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%d 分钟", s_alarm.min);
    lv_label_set_text(s_alarm_val_labels[ALARM_ROW_MIN], buf);
    lv_label_set_text(s_alarm_val_labels[ALARM_ROW_TUNE],
                      app_catalog(s_alarm.tune)->name);
    lv_label_set_text(s_alarm_hint, s_alarm.editing ? "上下键调整 确认键完成"
                                                    : "上下键选择 确认键调整 长按确认");
}

static void alarm_open(void) {
    app_ui_key_repeat_stop();
    if (!s_alarm_scrim) alarm_build(lv_screen_active());
    if (s_alarm.min == 0) s_alarm.min = 5;  // 静态零值 → 默认 5(此后保留上次)
    alarm_ctl_open(&s_alarm);
    s_alarm_open = true;
    alarm_refresh();
    lv_obj_clear_flag(s_alarm_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void alarm_close(void) {
    s_alarm_open = false;
    alarm_ctl_close(&s_alarm);
    app_ui_key_repeat_stop();
    if (s_alarm_scrim) lv_obj_add_flag(s_alarm_scrim, LV_OBJ_FLAG_HIDDEN);
}

// 确认(浏览态 OK 长按): 写 NVS(跨深睡持载) → 收设置页 → 确认层上屏;
// 1s 后由 tick 置 s_alarm_due,input_task 执行闹钟深睡。
static void alarm_ok_build(lv_obj_t *scr) {
    s_alarm_ok_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_alarm_ok_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_alarm_ok_scrim, COL_MASK_OPA, 0);
    s_alarm_ok_label = mk_label(s_alarm_ok_scrim, "", 24, 152, 192, 22,
                                &app_font_15_bold, COL_INK, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_alarm_ok_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void alarm_confirm(void) {
    alarm_nvs_save(s_alarm.min, s_alarm.tune);
    alarm_close();
    if (!s_alarm_ok_scrim) alarm_ok_build(lv_screen_active());
    char buf[32];
    snprintf(buf, sizeof(buf), "闹钟已设%d分钟", s_alarm.min);
    lv_label_set_text(s_alarm_ok_label, buf);
    lv_obj_clear_flag(s_alarm_ok_scrim, LV_OBJ_FLAG_HIDDEN);
    s_alarm_confirm_us = esp_timer_get_time();
    s_last_input_us = s_alarm_confirm_us;  // 确认层视为活跃会话(1s 后即深睡)
}

// 响铃页(TIMER 唤醒后, main 持锁调 app_ui_alarm_ring): 曲名 + 7 段波形呼吸
// (tick 驱动) + 「任意键停止」。音频单曲循环(app_audio_play_loop)持续活跃,
// 闲置熄屏/深睡判定天然不触发(音频不静音不熄屏, 熄屏才计深睡)。
static void ring_build(lv_obj_t *scr) {
    s_ring_scrim = mk_box(scr, 0, 0, APP_SCREEN_W, APP_SCREEN_H);
    lv_obj_set_style_bg_color(s_ring_scrim, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(s_ring_scrim, LV_OPA_COVER, 0);
    mk_label(s_ring_scrim, "闹钟", 24, 108, 192, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_CENTER, 0);
    s_ring_name = mk_label(s_ring_scrim, "", 24, 136, 192, 22, &app_font_15_bold,
                           COL_INK, LV_TEXT_ALIGN_CENTER, 0);
    // 7 段波形(宫格播放遮罩同款基础高度, tick 呼吸)
    for (int i = 0; i < 7; i++) {
        s_ring_bars[i] = mk_box(s_ring_scrim, 67 + i * 14, 176, 10, 33);
        lv_obj_set_style_radius(s_ring_bars[i], 5, 0);
        lv_obj_set_style_bg_color(s_ring_bars[i], lv_color_hex(COL_BATT_FILL), 0);
        lv_obj_set_style_bg_opa(s_ring_bars[i], LV_OPA_COVER, 0);
    }
    s_ring_hint = mk_label(s_ring_scrim, "任意键停止", 24, 232, 192, 17,
                           &app_font_12, COL_INK3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_ring_scrim, LV_OBJ_FLAG_HIDDEN);
}

void app_ui_alarm_ring(int tune) {
    if (tune < 0 || tune >= N_CARDS) tune = 0;
    if (!s_ring_scrim) ring_build(lv_screen_active());
    lv_label_set_text(s_ring_name, app_catalog(tune)->name);
    s_alarm_ring = true;
    lv_obj_clear_flag(s_ring_scrim, LV_OBJ_FLAG_HIDDEN);
    app_audio_play_loop(tune);
}

// ---------- 播放计数角标 ----------
// 计数在音频任务 play_index 里自增;tick 的 seq 跳变钩子与构建时刷。
static void badge_refresh(int i) {
    if (i < 0 || i >= (int)APP_CATALOG_COUNT) return;
    unsigned c = app_audio_play_count(i);
    if (c == 0) {
        lv_obj_add_flag(s_badges[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char buf[11];  // %u 最长 10 字节(计数实际 ≤9999)
    snprintf(buf, sizeof(buf), "%u", c);
    lv_label_set_text(s_badges[i], buf);
    lv_obj_clear_flag(s_badges[i], LV_OBJ_FLAG_HIDDEN);
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    s_tick_count++;
    if (s_tick_count % 20 == 0) update_battery();

    // 开机一次性状态同步:app_ui_build 早于 init_deferred(app_audio_start 内
    // 才 settings_load),构建时读到的播放计数全是默认 0,NVS 历史角标丢失。
    // 音频任务就绪(设置已加载)后刷一次;之后本块退化为一个 bool 检查。
    if (!s_state_synced && app_audio_started()) {
        s_state_synced = true;
        for (int i = 0; i < N_CARDS; i++) badge_refresh(i);
        update_volume_indicator();  // 开机音量从默认 80 纠正到 NVS 真值
    }

    int64_t now = esp_timer_get_time();

    // 弹窗闲置 15s 自动关(不刷新 last_input:关后剩余闲置预算继续流向熄屏计时)。
    if (s_menu.open && s_last_input_us &&
        now - s_last_input_us >= MODAL_IDLE_CLOSE_US) {
        menu_close();
    }
    if (s_modal.open && s_last_input_us &&
        now - s_last_input_us >= MODAL_IDLE_CLOSE_US) {
        modal_close();  // 同时防御性停掉可能连调中的音量 repeat
    }
    if (s_stats.open && s_last_input_us &&
        now - s_last_input_us >= MODAL_IDLE_CLOSE_US) {
        stats_close();
    }
    if (s_diag.open && s_last_input_us &&
        now - s_last_input_us >= MODAL_IDLE_CLOSE_US) {
        diag_close();
    }

    // 听声辨机: WAIT 到时推进(下一题或结算, 恢复时刻在 quiz_ctl 内);页面闲置
    // 15s 自动关(与 stats/diag 同款——题面音是 play_once 一次性播放, 无人应答
    // 不会永续;auto-advance 不刷新闲置基准, 离开后自然关)。
    if (s_quiz_open) {
        if (s_quiz.state == QUIZ_WAIT && now >= s_quiz.resume_us) {
            quiz_resume(&s_quiz, quiz_rng);
            if (s_quiz.state == QUIZ_SUM) {
                quiz_sum_render();
            } else {
                quiz_show(s_quiz_ask_grp);
                quiz_ask_render();
            }
        }
        if (s_last_input_us && now - s_last_input_us >= MODAL_IDLE_CLOSE_US) {
            quiz_close();
        }
    }

    // 场景列表页闲置 15s 自动关(与 stats/diag 同款;只在选, 无播放中的页)。
    if (s_scene_open && s_last_input_us &&
        now - s_last_input_us >= MODAL_IDLE_CLOSE_US) {
        scene_close();
    }

    // 闹钟设置页闲置 15s 自动关(同款)。确认层 1s 到期 → 交 input_task 深睡
    // (闹钟深睡序列要拿 LVGL 锁, 不能在 tick 里直接调)。
    if (s_alarm_open && s_last_input_us &&
        now - s_last_input_us >= MODAL_IDLE_CLOSE_US) {
        alarm_close();
    }
    if (s_alarm_confirm_us && now - s_alarm_confirm_us >= ALARM_CONFIRM_US) {
        lv_obj_add_flag(s_alarm_ok_scrim, LV_OBJ_FLAG_HIDDEN);
        s_alarm_confirm_us = 0;
        s_alarm_due = true;  // input_task 轮询执行(app_ui_alarm_minutes)
    }

    // 响铃波形呼吸(宫格播放遮罩同款正弦+中心锚定: 高度变化时 y 随动,
    // 柱子上下居中而非顶对齐, 与主界面播放遮罩的波形一致; 闹钟页下音频
    // 单曲循环持续活跃, 闲置熄屏/深睡判定天然不触发, 响铃页视为活跃会话)。
    if (s_alarm_ring) {
        uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
        for (int i = 0; i < 7; i++) {
            double phase = (double)((ms + i * WAVE_PHASE_MS) % WAVE_PERIOD_MS) /
                           WAVE_PERIOD_MS;
            double scale = 0.42 + 0.29 * (1.0 - cos(6.283185307179586 * phase));
            int h = (int)(WAVE_BASE[i] * scale + 0.5);
            lv_obj_set_height(s_ring_bars[i], h);
            lv_obj_set_y(s_ring_bars[i], 192 - h / 2);  // 176+33/2 波形区中心
        }
    }

    // 低电提醒 15s 自动关(基准=弹出时刻:它弹出时没有按键事件,不能复用闲置
    // 计时,否则弹出即被"早已闲置"误关)。
    if (s_lowbatt_show_us && now - s_lowbatt_show_us >= MODAL_IDLE_CLOSE_US) {
        lv_obj_add_flag(s_lowbatt_scrim, LV_OBJ_FLAG_HIDDEN);
        s_lowbatt_show_us = 0;
    }

    // 静音判定加 250ms 宽限:连播条目交接(自然结束→下一条置位)audio 侧
    // s_active 有亚毫秒 false 窗口,「持续不活跃」才判静音(与下方自然结束
    // 判定同一宽限),播放中闲置撞入交接间隙不会误熄屏。从未播放过
    // (s_last_active_us==0)直接算静音。
    bool audio_quiet = !app_audio_active() &&
                       (s_last_active_us == 0 ||
                        now - s_last_active_us >= 250000);

    // 闲置 60s 且暂停或持续静音 → 自动熄屏;暂停同样算闲置(熄屏即停播,
    // 与手动双击关屏同语义——暂停态只存在于 RAM,深睡后本来就不保留)。
    // 不 arm 唤醒守卫:没有尾随事件,陈旧武装会吞掉下一次真实按键。
    if (!s_screen_off && s_last_input_us &&
        now - s_last_input_us >= SCREEN_IDLE_OFF_US &&
        (app_audio_paused() || audio_quiet)) {
        screen_off();
    }

    // 距最后输入累计 5min(熄屏后约 4min)→ 深睡到期标志(input_task 检查并执行入睡序列)。
    // 追加静音/暂停双保险:聆听态熄屏(播放中)不算到期——播放中永不全寝;
    // 连播链尾 250ms 静音后即并入时间线(闲置早已 ≥5min 则下一拍直接睡)。
    if (s_screen_off && !s_sleep_due && s_last_input_us &&
        now - s_last_input_us >= AUTO_SLEEP_US &&
        (app_audio_paused() || audio_quiet)) {
        s_sleep_due = true;
    }

    // 播放条目跟随:OK 切条命令生效或连播接棒都会让 seq 跳变,把遮罩/副标题
    // /进度/焦点全部追到当前条目(幂等刷新;焦点跟随播放走)。先记 seq 再读
    // cur:极端竞态下最多多刷一拍,下一拍自愈,不会卡在旧条目。
    // 熄屏(聆听)时整块跳过:s_seq_shown 不推进,黑屏下的换曲重绘纯属
    // 耗电(SPI 传输 + 卡片图标弧线绘制链,正是 update_battery 熄屏跳过的
    // 同类);唤醒后首拍 seq 差仍在,一次性补齐到当前条目。
    if (!s_screen_off && app_audio_active() && app_audio_seq() != s_seq_shown) {
        s_seq_shown = app_audio_seq();
        int cur = app_audio_current();
        if (cur >= 0) {
            ui_show_playing(cur);
            ui_focus_jump(cur);
            badge_refresh(cur);  // 计数在音频任务 play_index 里自增,seq 跳变后必新
        }
    }

    if (app_audio_active()) {
        s_last_active_us = now;
        // 撕裂窗口防御:screen_off() 可能刚在本 tick 内(自动熄屏)把 UI 停掉
        // (s_play_idx=-1)而音频任务尚未消费 STOP——单核+持锁执行下必现。
        // 窗口内后续的 paused 同步/进度/音波刷新全部以 s_play_idx 为下标,
        // =-1 时越界写坏相邻数组(s_mask_labels[-1]=s_masks 末项,对 box 写
        // label 字段→内存腐坏→panic 重启,现象即"熄屏后播开机动画")。
        if (s_play_idx < 0) return;
    } else if (s_play_idx >= 0) {
        // 自然结束判定加 250ms 宽限:播放命令 notify 后音频任务置 s_active 前、
        // 连播条目交接的间隙,都会短暂读到「不活跃」——持续不活跃才判定结束。
        if (s_last_active_us && now - s_last_active_us >= 250000) {
            ui_stop();
        }
        return;
    } else {
        return;
    }
    // 熄屏渲染节流:黑屏下跳过视觉刷新(暂停态标签/进度条/音波,均不可见,
    // 省 LVGL 重绘+SPI 传输;聆听态的主要能耗收益来源)。s_paused 状态可能
    // 陈旧一拍,唤醒后第一拍自愈(暂停/播放切换本就以拍为单位同步)。
    if (s_screen_off) return;
    bool paused = app_audio_paused();
    if (paused != s_paused) {
        s_paused = paused;
        lv_label_set_text(s_mask_labels[s_play_idx], paused ? "已暂停" : "播放中");
    }
    const app_alert_t *a = app_catalog(s_play_idx);
    uint32_t elapsed = app_audio_elapsed_ms();
    char buf[8];
    fmt_dur(buf, sizeof(buf), elapsed);
    lv_label_set_text(s_cur_label, buf);
    int w = 110 * (int)elapsed / (int)a->dur_ms;
    if (w > 110) w = 110;
    lv_obj_set_width(s_fill, w);

    if (!paused) {  // 音波呼吸(相位错开;暂停冻结当前形态)
        uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
        for (int i = 0; i < 7; i++) {
            double phase = (double)((ms + i * WAVE_PHASE_MS) % WAVE_PERIOD_MS) / WAVE_PERIOD_MS;
            double scale = 0.42 + 0.29 * (1.0 - cos(6.283185307179586 * phase));
            wave_set_height(s_play_idx, i, (int)(WAVE_BASE[i] * scale + 0.5));
        }
    }
}

// ---------- 长按连续滚动 / 弹窗连调音量(共用 120ms repeat timer) ----------
static void repeat_cb(lv_timer_t *t) {
    (void)t;
    // 连发不产生新按键事件,刷新闲置基准,长按连滚/连调不会被误判闲置熄屏。
    s_last_input_us = esp_timer_get_time();
    if (s_stats.open) {
        stats_scroll_by(s_repeat_dir);  // 统计列表连滚(浏览/编辑之外的第三态)
        return;
    }
    if (s_diag.open) {
        diag_scroll_by(s_repeat_dir);  // 诊断页连滚(此前漏分支落到宫格焦点)
        return;
    }
    if (s_modal.open) {
        // 编辑音量 = ±10/拍连调; 浏览态 = 行连移(跳隐藏行); 其余状态无 repeat。
        if (s_modal.editing && s_modal.item == MODAL_ITEM_VOLUME) {
            app_audio_set_volume(modal_ctl_volume_step(app_audio_volume(),
                                                       -s_repeat_dir * 10));
            modal_refresh();
        } else if (!s_modal.editing) {
            s_modal.item = modal_ctl_item_next(&s_modal, s_modal.item,
                                               s_repeat_dir);
            modal_refresh();
        }
        return;  // 弹窗开着不滚动宫格
    }
    if (s_alarm_open) {
        // 编辑态: 时长 ±5/拍(钳 1..120) / 选曲环绕连调; 浏览态: 行连移。
        if (s_alarm.editing) {
            if (s_alarm.row == ALARM_ROW_MIN)
                s_alarm.min = alarm_ctl_min_clamp(s_alarm.min - s_repeat_dir * 5);
            else
                s_alarm.tune = alarm_ctl_tune_step(s_alarm.tune, s_repeat_dir);
        } else {
            alarm_ctl_row_move(&s_alarm, s_repeat_dir);
        }
        alarm_refresh();
        return;
    }
    if (s_menu.open) {
        menu_ctl_move(&s_menu, s_repeat_dir);
        menu_refresh();
        return;
    }
    // 辨机模式页(答题页无 repeat: 三键=三选项, LONG 已被答题页吞掉)。
    if (s_quiz_open && s_quiz_mode.open) {
        menu_ctl_move(&s_quiz_mode, s_repeat_dir);
        quiz_mode_refresh();
        return;
    }
    if (s_scene_open) {
        menu_ctl_move(&s_scene_sel, s_repeat_dir);
        scene_refresh();
        return;
    }
    app_ui_move_focus(s_repeat_dir);
}

// 启动 repeat。step_now=false 供弹窗连调:LONG 已由 CLICK 语义处理过一步,
// 连调从 timer 第一拍开始,不再立即步进。
static void repeat_start(int dir, bool step_now) {
    s_repeat_dir = dir;
    if (step_now) app_ui_move_focus(dir);
    if (!s_repeat_timer) s_repeat_timer = lv_timer_create(repeat_cb, 120, NULL);
}

// ---------- 构建 ----------
static void build_status_bar(lv_obj_t *scr) {
    // 时钟已按需求移除:左侧仅飞机标识 + For AvGeeks。
    mk_icon(scr, APP_ICON_PLANE, 12, 6, 10, 10, COL_INK3);
    mk_label(scr, "For AVGeeks", 27, 2, 90, 17, &app_font_12, COL_INK3,
             LV_TEXT_ALIGN_LEFT, 1);

    // 电池:主体 15x8 + 触点 + 电量条 + 内嵌百分比。
    lv_obj_t *body = mk_box(scr, 212, 7, 15, 8);
    lv_obj_set_style_bg_color(body, lv_color_hex(COL_BATT_BODY), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(COL_INK3), 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_t *nub = mk_box(scr, 227, 9, 1, 4);
    lv_obj_set_style_bg_color(nub, lv_color_hex(COL_INK3), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nub, 1, 0);
    s_batt_fill = mk_box(body, 0, 0, 10, 6);  // 内容原点=边框内,即内缩 1px
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(COL_BATT_FILL), 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    s_batt_label = mk_label(body, "82", 0, 0, 15, 9, &app_font_6_bold, COL_CANVAS,
                             LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_batt_label);
}

static void build_header(lv_obj_t *scr) {
    mk_label(scr, "航空警示音", 12, 22, 150, 22, &app_font_15_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    s_sub = mk_label(scr, "", 12, 47, 216, 17, &app_font_12, COL_INK3,
                     LV_TEXT_ALIGN_LEFT, 0);
    // 音量指示器:标题行右侧,喇叭图标+数字,右缘与状态栏电池列对齐。
    // 开机时 app_audio 尚未加载 NVS(读默认 80),真值由 tick 的一次性
    // 状态同步纠正——同电量占位 82→真值模式。图标 x 由 update_volume_
    // indicator 按数字实际宽度动态摆位(恒 4px 间距)。
    s_vol_icon = mk_icon(scr, UI_ICON_VOLUME, 176, 27, 12, 12, COL_INK3);
    char vb[5];
    snprintf(vb, sizeof(vb), "%d", app_audio_volume());
    // y=25:数字字形(font_10)box_h=8、ofs_y=0、ascent=12,视觉中心=label 顶+8;
    // 25+8=33=图标中心(27+6),图标与数字上下居中对齐。
    s_vol_label = mk_label(scr, vb, 192, 25, 36, 14, &app_font_10, COL_INK3,
                           LV_TEXT_ALIGN_RIGHT, 0);
    update_volume_indicator();  // 初始摆位(s_shown_vol=-1 必刷;文本幂等)
}

static void build_card(lv_obj_t *content, int i) {
    const app_alert_t *a = app_catalog(i);
    int h = a->card_h;
    bool tall = h == 96;

    lv_obj_t *card = mk_box(content, app_layout_card_x(i), s_lay.card_y[i], APP_COL_W, h);
    lv_obj_set_style_radius(card, 11, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    s_cards[i] = card;

    // 子元素坐标相对内容区原点(border 2px 内缩);8px 视觉内边距 => 内容坐标 6。
    const int P = 6;              // 距卡片边缘 8px(含 border 2)
    const int CW = APP_COL_W - 2 * 2;  // 内容宽 100
    int chip = tall ? 28 : 24;
    int icon = tall ? 20 : 16;
    lv_obj_t *chip_o = mk_box(card, P, P, chip, chip);
    lv_obj_set_style_bg_color(chip_o, lv_color_hex(a->acc), 0);
    lv_obj_set_style_bg_opa(chip_o, 38, 0);  // 15% 视觉叠加
    lv_obj_set_style_radius(chip_o, tall ? 9 : 8, 0);
    mk_icon(card, a->icon, P + (chip - icon) / 2, P + (chip - icon) / 2, icon, icon, a->acc);
    char dur[8];
    fmt_dur(dur, sizeof(dur), a->dur_ms);
    mk_label(card, dur, P + chip + 4, P + (chip - 14) / 2, CW - P - chip - 4 - P, 14,
             &app_font_10, COL_INK3, LV_TEXT_ALIGN_RIGHT, 0);

    // 名称/英文术语。
    int y = P + chip + 3;
    mk_label(card, a->name, P, y, CW - 2 * P, 14, &app_font_10_bold, COL_INK,
             LV_TEXT_ALIGN_LEFT, 0);
    // 播放计数角标:英文术语行右端小数字(en 最长 9 字符 ~54px,右侧留白足够)。
    s_badges[i] = mk_label(card, "", CW - 26, y + 18, 20, 9, &app_font_6_bold,
                           COL_INK3, LV_TEXT_ALIGN_RIGHT, 0);
    mk_label(card, a->en, P, y + 17, CW - 2 * P, 14, &app_font_10, COL_INK3,
             LV_TEXT_ALIGN_LEFT, 1);

    // 96 档:底部迷你静态音波(底边距 6)。
    if (tall) {
        for (int k = 0; k < 9; k++) {
            lv_obj_t *b = mk_box(card, P + k * 5, 70 + (16 - MINI_H[k]) / 2, 3, MINI_H[k]);
            lv_obj_set_style_bg_color(b, lv_color_hex(a->acc), 0);
            lv_obj_set_style_bg_opa(b, 115, 0);  // 45%
            lv_obj_set_style_radius(b, 2, 0);
        }
    }

    // 播放遮罩:84% 深色,盖满整卡(内容坐标 -2 => 卡片 0,0 起,超出被裁到卡框)。
    // radius 与卡片一致(11):大于卡片圆角会在四角切出月牙缝。
    lv_obj_t *mask = mk_box(card, -2, -2, APP_COL_W + 4, h + 4);
    lv_obj_set_style_radius(mask, 11, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(COL_MASK_BG), 0);
    lv_obj_set_style_bg_opa(mask, COL_MASK_OPA, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN);
    s_masks[i] = mask;
    // 音波条绝对定位在遮罩内(遮罩无边框,子坐标=遮罩坐标);高度变化时垂直再居中。
    // 组中心 ≈ 遮罩中心(72 档 38 / 96 档 50),「播放中」随后(底边距 9/21)。
    int wave_cy = tall ? 39 : 27;  // 波形组中心 y(遮罩高 h+4)
    int label_y = tall ? 62 : 50;  // 「播放中」标签 y
    for (int k = 0; k < 7; k++) {
        lv_obj_t *b = mk_box(mask, 28 + k * 8, wave_cy - WAVE_BASE[k] / 2, 4, WAVE_BASE[k]);
        lv_obj_set_style_bg_color(b, lv_color_hex(a->acc), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 2, 0);
        s_wave_bars[i][k] = b;
    }
    lv_obj_t *mlabel = mk_label(mask, "播放中", 30, label_y, 48, 17, &app_font_12,
                                0xFFFFFF, LV_TEXT_ALIGN_CENTER, 2);
    lv_obj_set_style_text_opa(mlabel, 235, 0);
    s_mask_labels[i] = mlabel;
}

static void build_grid(lv_obj_t *scr) {
    int heights[N_CARDS];
    for (int i = 0; i < N_CARDS; i++) heights[i] = app_catalog(i)->card_h;
    app_layout_compute(heights, N_CARDS, &s_lay);

    lv_obj_t *viewport = mk_box(scr, 0, APP_STATUS_H + APP_HEADER_H, APP_SCREEN_W, APP_VIEWPORT_H);
    s_content = mk_box(viewport, APP_PAD_X, 0, APP_COL_W * 2 + APP_COL_GAP, s_lay.content_h);
    for (int i = 0; i < N_CARDS; i++) build_card(s_content, i);

    // 滚动条。
    s_sb_track = mk_box(viewport, 233, 0, 3, APP_VIEWPORT_H);
    lv_obj_set_style_bg_color(s_sb_track, lv_color_hex(COL_SB_TRACK), 0);
    lv_obj_set_style_bg_opa(s_sb_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sb_track, 2, 0);
    s_sb_thumb = mk_box(s_sb_track, 0, 0, 3, APP_VIEWPORT_H);
    lv_obj_set_style_bg_color(s_sb_thumb, lv_color_hex(COL_SB_THUMB), 0);
    lv_obj_set_style_bg_opa(s_sb_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sb_thumb, 2, 0);
}

static void build_bottom(lv_obj_t *scr) {
    lv_obj_t *bottom = mk_box(scr, 0, APP_SCREEN_H - APP_BOTTOM_H, APP_SCREEN_W, APP_BOTTOM_H);
    lv_obj_set_style_bg_color(bottom, lv_color_hex(COL_BOTTOM_BG), 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bottom, lv_color_hex(COL_BOTTOM_LN), 0);
    lv_obj_set_style_border_width(bottom, 1, 0);
    lv_obj_set_style_border_side(bottom, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_opa(bottom, LV_OPA_COVER, 0);

    // 浏览态:左(上下键+移动选择) 右(确认播放+键帽)。
    s_browsebar = mk_box(bottom, 0, 0, APP_SCREEN_W, APP_BOTTOM_H);
    mk_icon(s_browsebar, UI_ICON_CHEV_UP, 12, 8, 10, 10, COL_INK2);
    mk_icon(s_browsebar, UI_ICON_CHEV_DOWN, 24, 8, 10, 10, COL_INK3);
    mk_label(s_browsebar, "移动选择", 40, 3, 48, 17, &app_font_12, COL_INK2,
             LV_TEXT_ALIGN_LEFT, 0);
    mk_label(s_browsebar, "确认播放", 146, 3, 52, 17, &app_font_12, COL_INK3,
             LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *keycap = mk_box(s_browsebar, 204, 8, 13, 10);
    lv_obj_set_style_bg_color(keycap, lv_color_hex(COL_KEYCAP_BG), 0);
    lv_obj_set_style_bg_opa(keycap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(keycap, lv_color_hex(COL_KEYCAP_LN), 0);
    lv_obj_set_style_border_width(keycap, 1, 0);
    lv_obj_set_style_border_opa(keycap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(keycap, 3, 0);
    lv_obj_center(mk_icon(keycap, UI_ICON_PLAY, 0, 0, 9, 9, COL_INK2));

    // 播放态:进度组(已播 | 轨道 | 总长 | 暂停键帽)。
    s_playbar = mk_box(bottom, 0, 0, APP_SCREEN_W, APP_BOTTOM_H);
    s_cur_label = mk_label(s_playbar, "0:00", 12, 5, 28, 14, &app_font_10, COL_INK2,
                           LV_TEXT_ALIGN_LEFT, 0);
    s_track = mk_box(s_playbar, 44, 11, 110, 3);
    lv_obj_set_style_bg_color(s_track, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_track, 2, 0);
    s_fill = mk_box(s_track, 0, 0, 0, 3);
    lv_obj_set_style_bg_color(s_fill, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_fill, 2, 0);
    s_tot_label = mk_label(s_playbar, "0:00", 158, 5, 28, 14, &app_font_10, COL_INK3,
                           LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *pcap = mk_box(s_playbar, 204, 8, 13, 10);
    lv_obj_set_style_bg_color(pcap, lv_color_hex(COL_PAUSE_BG), 0);
    lv_obj_set_style_bg_opa(pcap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pcap, lv_color_hex(COL_PAUSE_LN), 0);
    lv_obj_set_style_border_width(pcap, 1, 0);
    lv_obj_set_style_border_opa(pcap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pcap, 3, 0);
    lv_obj_center(mk_icon(pcap, UI_ICON_PAUSE, 0, 0, 9, 9, COL_PAUSE_ICON));
    lv_obj_add_flag(s_playbar, LV_OBJ_FLAG_HIDDEN);
}

void app_ui_build(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_CANVAS), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(scr);
    build_header(scr);
    build_grid(scr);
    build_bottom(scr);

    // 初始:首卡聚焦,内容顶部对齐。
    s_focus = 0;
    s_scroll = 0;
    // 闲置计时从 UI 就绪起算(0 会被判"自上电起闲置"而立即熄屏)。
    s_last_input_us = esp_timer_get_time();
    card_set_focus(s_cards[0], app_catalog(0), true);
    update_thumb();
    update_sub();
    for (int i = 0; i < (int)APP_CATALOG_COUNT; i++) badge_refresh(i);  // NVS 计数角标

    lv_timer_create(tick_cb, 100, NULL);
}

// ---------- 公共接口 ----------
void app_ui_move_focus(int dir) {
    // 视觉顺序导航:按卡片 y 坐标取下一张,焦点在屏幕上单调移动(见 app_layout.h)。
    int next = app_layout_visual_next(&s_lay, s_focus, dir);
    if (next == s_focus) {
        // 首尾环绕:视觉顶按 ↑ 绕到视觉底、视觉底按 ↓ 绕到视觉顶(瞬移到
        // 另一端,滚动同 100ms 动画)。环绕目标取视觉端点而非数组端点,
        // 不引入视觉回跳。
        next = app_layout_visual_end(&s_lay, -dir);
        if (next == s_focus) return;  // 单卡:环无意义
    }
    card_set_focus(s_cards[s_focus], app_catalog(s_focus), false);
    s_focus = next;
    card_set_focus(s_cards[s_focus], app_catalog(s_focus), true);
    scroll_to(app_layout_scroll_for_focus(&s_lay, s_focus, s_scroll));
    if (s_play_idx < 0) update_sub();  // 播放中副标题保持原条目
}

void app_ui_toggle_play(void) {
    if (s_play_idx < 0) {
        ui_play(s_focus);
    } else if (s_play_idx == s_focus) {
        app_audio_pause_resume();  // 暂停/继续,tick 同步视觉
    } else {
        ui_play(s_focus);  // 切换条目(切换延迟 <150ms)
    }
}

void app_ui_key_repeat_start(int dir) {
    repeat_start(dir, true);  // 长按立即移一格,再 120ms/格连滚
}

void app_ui_key_repeat_stop(void) {
    if (s_repeat_timer) {
        lv_timer_delete(s_repeat_timer);
        s_repeat_timer = NULL;
    }
}

bool app_ui_sleep_due(void) { return s_sleep_due; }
void app_ui_sleep_due_ack(void) { s_sleep_due = false; }

bool app_ui_alarm_due(void) { return s_alarm_due; }
void app_ui_alarm_due_ack(void) { s_alarm_due = false; }
int app_ui_alarm_minutes(void) { return s_alarm.min; }

// 亮屏状态(供关机动画判定:仅亮屏关机播动画,见 app_shutdown.h)。
bool app_ui_screen_off(void) { return s_screen_off; }

// ---------- 统一输入入口(main 持 LVGL 锁后先调这里) ----------
// 返回 true=事件已消费,main 不再分发;false=交回 main 旧分发(浏览/播放)。
// 优先级:唤醒守卫 > 关屏唤醒 > OK 双击关屏 > 设置弹窗 > OK 长按开弹窗。
bool app_ui_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    int64_t now = esp_timer_get_time();
    s_last_input_us = now;
    // 方向键事件打断挂起的 OK 双击候选:双击关屏是连续两次 OK 按下,中间夹
    // UP/DOWN 的「OK→方向→OK」快速操作不应误判关屏。放在吞键路径之前——
    // 熄屏窗口内按过的方向键同样使候选失效。OK 自身的 RELEASE/CLICK 不清
    // (两次 PRESS 之间必有 RELEASE,清了双击永远判不出)。
    if (btn != BSP_BTN_OK) s_last_ok_press_us = 0;

    // 开机动画:首事件触发跳过并吞键(跳过后RELEASE落到旧分发亦无副作用)。
    if (app_boot_active()) {
        app_boot_skip();
        return true;
    }

    // 尾随 CLICK 吞键窗:一次性手势动作(响铃停止/低电提醒继续/熄屏唤醒)只吞
    // 当前事件不够——iot_button 的 CLICK 在抬键后 ~180ms(双击窗)才补发,那次
    // CLICK 会越过当页消费落进宫格(OK 停止=误播焦点卡/方向停止=焦点跳格)。
    // 手势处置处置 800ms 窗(关屏手势同款模式),窗内一切输入吞掉。
    if (s_swallow_until_us) {
        if (now < s_swallow_until_us) return true;
        s_swallow_until_us = 0;  // 到期即撤(下次手势重置)
    }

    // 唤醒守卫:吞掉唤醒键按住期间的尾随事件,直到该键 RELEASE 或 600ms
    // 兜底到期(到期那次也吞,见 key_guard.h)。仅唤醒路径武装;双击关屏的
    // 尾随 RELEASE 不归它管——由下方关屏 800ms 吞键窗口吞。
    if (key_guard_consume(&s_wake_guard, ev == BSP_BTN_RELEASE, now)) {
        return true;
    }

    // 关屏态:关屏手势的尾随事件(RELEASE/迟到的 CLICK/LONG)在 800ms 窗口内
    // 一并吞掉,防止它们立刻把屏幕唤醒;之后的按键才作为唤醒键(唤醒守卫挡按住)。
    if (s_screen_off) {
        if (now - s_screen_off_us < 800000) return true;
        screen_wake();
        key_guard_arm(&s_wake_guard, now, WAKE_GUARD_US);
        // 唤醒守卫只吞到唤醒键 RELEASE;尾随 CLICK(+180ms)由吞键窗兜住,
        // 否则熄屏轻点唤醒会误触宫格播放/焦点跳格。
        s_swallow_until_us = now + 800000;
        return true;
    }

    // 低电提醒遮罩:任意键继续(消费该事件,遮罩期间不响应其他语义)。
    if (s_lowbatt_show_us) {
        lv_obj_add_flag(s_lowbatt_scrim, LV_OBJ_FLAG_HIDDEN);
        s_lowbatt_show_us = 0;
        // 遮罩可能在长按连滚中弹出:本块消费掉 RELEASE 后 repeat 已无人可停
        // (无限滚动+闲置基准每拍刷新永不熄屏),这里必须显式停。
        app_ui_key_repeat_stop();
        s_swallow_until_us = now + 800000;  // 尾随 CLICK 同款吞(否则落宫格)
        return true;
    }

    // 闹钟确认层显示中(1s 后深睡):闹钟已确认,任何按键不构成语义动作,
    // 全吞(深睡序列自带按键电压预检,无需输入参与)。
    if (s_alarm_confirm_us) return true;

    // 响铃页(TIMER 唤醒后): 任意键动作停止响铃(松手事件不算动作)并回主界面。
    // 放在所有弹窗之前: 响铃中按键只做停止, 不进菜单/弹窗/关屏语义。
    // 停止动作置吞键窗: 尾随 CLICK(+180ms)不得落进宫格误触播放/焦点。
    if (s_alarm_ring) {
        if (ev != BSP_BTN_RELEASE) {
            s_alarm_ring = false;
            app_audio_stop();
            app_ui_key_repeat_stop();
            lv_obj_add_flag(s_ring_scrim, LV_OBJ_FLAG_HIDDEN);
            s_swallow_until_us = now + 800000;
        }
        return true;
    }

    // 统计弹窗开着:UP/DOWN 滚动(单击 1 行/双击 2 行/长连续滚),OK 关闭,
    // 完整消费一切事件(设置弹窗与统计互斥,不会叠加)。
    if (s_stats.open) {
        switch (ev) {
        case BSP_BTN_CLICK:
            if (btn == BSP_BTN_OK) stats_close();
            else stats_scroll_by(btn == BSP_BTN_UP ? -1 : 1);
            break;
        case BSP_BTN_DOUBLE:
            stats_scroll_by(btn == BSP_BTN_UP ? -2 : 2);
            break;
        case BSP_BTN_LONG:
            if (btn == BSP_BTN_OK) stats_close();
            else repeat_start(btn == BSP_BTN_UP ? -1 : 1, false);
            break;
        case BSP_BTN_RELEASE:
            app_ui_key_repeat_stop();  // 停掉可能连滚中的 repeat
            break;
        default:
            break;
        }
        return true;
    }

    // 硬件诊断页开着: 与统计弹窗同款交互(滚动/连滚/OK 关), 只读快照。
    if (s_diag.open) {
        switch (ev) {
        case BSP_BTN_CLICK:
            if (btn == BSP_BTN_OK) diag_close();
            else diag_scroll_by(btn == BSP_BTN_UP ? -1 : 1);
            break;
        case BSP_BTN_DOUBLE:
            diag_scroll_by(btn == BSP_BTN_UP ? -2 : 2);
            break;
        case BSP_BTN_LONG:
            if (btn == BSP_BTN_OK) diag_close();
            else repeat_start(btn == BSP_BTN_UP ? -1 : 1, false);
            break;
        case BSP_BTN_RELEASE:
            app_ui_key_repeat_stop();
            break;
        default:
            break;
        }
        return true;
    }

    // 听声辨机开着: 完整消费一切事件。三子页:
    //  模式页(menu_ctl 复用: UP/DOWN 选行 / OK 进入 / OK 长按退出);
    //  答题页 ASK(三键直接 = 三选项一步确认, 物理位映射: 上键=上项/下键=中项/
    //   确认键=下项, 见 bsp_pins.h; OK 长按退出, 其余事件全吞);
    //  WAIT(反馈展示中, 全吞) / 结算页(OK 关闭)。
    if (s_quiz_open) {
        if (s_quiz_mode.open) {
            menu_action_t mact = menu_ctl_key(&s_quiz_mode, btn, ev);
            switch (mact) {
            case MENU_UP:
            case MENU_DOWN:
                quiz_mode_refresh();
                break;
            case MENU_ENTER:
                quiz_start_mode();
                break;
            case MENU_CLOSE:
                quiz_close();
                break;
            case MENU_REPEAT_UP:
            case MENU_REPEAT_DOWN:
                repeat_start(mact == MENU_REPEAT_UP ? -1 : 1, false);
                break;
            case MENU_REPEAT_STOP:
                app_ui_key_repeat_stop();
                break;
            default:
                break;
            }
        } else if (s_quiz.state == QUIZ_ASK) {
            if (ev == BSP_BTN_CLICK) {
                // 按键【物理位】映射选项屏位(设备三键纵排 [上][下][确认],
                // 见 bsp_pins.h): 上键(顶)→顶项 / 下键(中)→中项 / 确认键
                // (底)→底项。曾按电气名映射(DOWN=底/OK=中), 与物理位错开
                // 一格, 真机表现为"下键/确认键反了"(两轮反馈)。
                int pick = btn == BSP_BTN_UP ? 0
                         : btn == BSP_BTN_DOWN ? 1
                         : btn == BSP_BTN_OK ? 2 : -1;
                if (pick >= 0) quiz_answer_pick(pick);
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
                quiz_close();
            }
        } else if (s_quiz.state == QUIZ_SUM) {
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) quiz_close();
        }
        return true;
    }

    // 场景连播列表页: menu_ctl 复用(UP/DOWN 选行 / OK 开始播放并关页回宫格 /
    // OK 长按退出), 其余全吞。
    if (s_scene_open) {
        menu_action_t mact = menu_ctl_key(&s_scene_sel, btn, ev);
        switch (mact) {
        case MENU_UP:
        case MENU_DOWN:
            scene_refresh();
            break;
        case MENU_ENTER:
            app_audio_play_scene(APP_SCENES[s_scene_sel.item].seq,
                                 APP_SCENES[s_scene_sel.item].n);
            scene_close();  // 关页回宫格: seq 跳变由 tick 视觉跟随接管
            break;
        case MENU_CLOSE:
            scene_close();
            break;
        case MENU_REPEAT_UP:
        case MENU_REPEAT_DOWN:
            repeat_start(mact == MENU_REPEAT_UP ? -1 : 1, false);
            break;
        case MENU_REPEAT_STOP:
            app_ui_key_repeat_stop();
            break;
        default:
            break;
        }
        return true;
    }

    // 闹钟设置页: alarm_ctl 矩阵(浏览/编辑两态), 时长长按连调复用 repeat,
    // 浏览态 OK 长按 = 确认(alarm_confirm)。完整消费一切事件。
    if (s_alarm_open) {
        alarm_action_t act = alarm_ctl_key(&s_alarm, btn, ev);
        switch (act) {
        case ALARM_ROW_MOVE:
        case ALARM_EDIT_ENTER:
        case ALARM_EDIT_EXIT:
        case ALARM_MIN_UP:
        case ALARM_MIN_DOWN:
        case ALARM_TUNE_UP:
        case ALARM_TUNE_DOWN:
            alarm_refresh();
            break;
        case ALARM_MIN_REPEAT_UP:
        case ALARM_MIN_REPEAT_DOWN:
        case ALARM_TUNE_REPEAT_UP:
        case ALARM_TUNE_REPEAT_DOWN:
        case ALARM_ROW_REPEAT_UP:
        case ALARM_ROW_REPEAT_DOWN:
            repeat_start(act == ALARM_MIN_REPEAT_UP || act == ALARM_TUNE_REPEAT_UP ||
                                 act == ALARM_ROW_REPEAT_UP
                             ? -1 : 1,
                         false);
            break;
        case ALARM_REPEAT_STOP:
            app_ui_key_repeat_stop();
            break;
        case ALARM_CONFIRM:
            alarm_confirm();
            break;
        default:
            break;
        }
        return true;
    }

    // 主菜单开着: UP/DOWN 选行, OK 进入(行派发在 menu_enter), OK 长按关闭,
    // 其余全吞(RELEASE 不停连调——菜单无连调语义, 连发在打开时已停)。
    // 放在双击检测之前: 菜单内 OK PRESS 被这里消费, 不会喂双击关屏候选。
    if (s_menu.open) {
        menu_action_t mact = menu_ctl_key(&s_menu, btn, ev);
        switch (mact) {
        case MENU_UP:
        case MENU_DOWN:
            menu_refresh();
            break;
        case MENU_ENTER:
            menu_enter(s_menu.item);
            break;
        case MENU_CLOSE:
            menu_close();
            break;
        case MENU_REPEAT_UP:
        case MENU_REPEAT_DOWN:
            repeat_start(mact == MENU_REPEAT_UP ? -1 : 1, false);
            break;
        case MENU_REPEAT_STOP:
            app_ui_key_repeat_stop();
            break;
        default:
            break;
        }
        return true;
    }

    // OK 双击(应用层检测):iot_button 的 DOUBLE 需要第二击在第一击抬键后
    // ~180ms(短按窗)内压下(还要先过 40ms 去抖确认),人手偏快节奏打不出
    // 来,改用两次 PRESS 间隔 <450ms 判定。注意聚合并不总兜底:抬键到二次
    // 按压间隔 >180ms 时组件已先行发出第一击的 SINGLE_CLICK(播放已开始),
    // 本分支只负责随后关屏——所以 280~450ms 的中速双击会先"噗"一小段声音
    // 再关屏,属已知取舍;组件自身的 DOUBLE(<180ms 段)才聚合吞掉 CLICK、
    // 无此串台。
    // 第二击按下即关屏,尾随事件由上面的关屏窗口吞掉。
    // 弹窗开着时不响应双击关屏:弹窗内快速按两次 OK 是确认编辑,
    // 不是关屏手势(统计弹窗同理)。
    if (btn == BSP_BTN_OK && ev == BSP_BTN_PRESS && !s_modal.open && !s_stats.open) {
        if (s_last_ok_press_us && now - s_last_ok_press_us < 450000) {
            s_last_ok_press_us = 0;
            modal_close();
            screen_off();
            return true;
        }
        s_last_ok_press_us = now;
        return false;  // 单次按下不消费:旧分发本就不处理 PRESS
    }

    // iot_button 的 DOUBLE 仍可能命中(极快轻点);幂等防二次关屏/二次武装。
    // 同上,弹窗开着时不作为关屏手势。
    if (btn == BSP_BTN_OK && ev == BSP_BTN_DOUBLE && !s_modal.open && !s_stats.open) {
        modal_close();
        screen_off();
        return true;
    }

    // 设置弹窗开着:完整消费一切事件,按 modal_ctl 矩阵出动作。
    if (s_modal.open) {
        modal_action_t act = modal_ctl_key(&s_modal, btn, ev);
        switch (act) {
        case MODAL_ITEM_PREV:
        case MODAL_ITEM_NEXT:
        case MODAL_EDIT_ENTER:
            modal_refresh();
            break;
        case MODAL_EDIT_EXIT:
            app_ui_key_repeat_stop();  // 编辑退出停掉可能连调中的 repeat
            modal_refresh();
            break;
        case MODAL_VOL_UP:
        case MODAL_VOL_DOWN:
            app_audio_set_volume(modal_ctl_volume_step(
                app_audio_volume(), act == MODAL_VOL_UP ? 10 : -10));
            modal_refresh();
            break;
        case MODAL_VOL_REPEAT_UP:
        case MODAL_VOL_REPEAT_DOWN:
            repeat_start(act == MODAL_VOL_REPEAT_UP ? -1 : 1, false);
            break;
        case MODAL_ITEM_REPEAT_UP:
        case MODAL_ITEM_REPEAT_DOWN:
            repeat_start(act == MODAL_ITEM_REPEAT_UP ? -1 : 1, false);
            break;
        case MODAL_REPEAT_STOP:
            app_ui_key_repeat_stop();
            break;
        case MODAL_CLOSE:
            modal_close();
            break;
        case MODAL_AUTOPLAY_TOGGLE:
            app_audio_set_autoplay(!app_audio_autoplay());
            modal_refresh();  // 显隐随机行 + 刷值
            break;
        case MODAL_RANDOM_TOGGLE:
            app_audio_set_random(!app_audio_random());
            modal_refresh();
            break;
        case MODAL_LISTEN_TOGGLE:
            app_audio_set_listen(!app_audio_listening());
            modal_refresh();
            break;
        case MODAL_NONE:
        default:
            break;
        }
        return true;
    }

    // OK 长按:开主菜单(设置/播放统计从主菜单进)。
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        menu_open();
        return true;
    }

    return false;  // 交回 main 旧分发(UP/DOWN 焦点滚动、OK 单击播放)
}
