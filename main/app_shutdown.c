// main/app_shutdown.c —— 关机动画:开机动画(app_boot)的镜像。
//   黑幕淡入(150ms) → 光晕(3 层同心圆) → 扫描环三段弧自旋一圈 →
//   飞机从开机收尾位降回光晕中心并坍缩 → 光点一闪而灭 → 背光 200ms 渐灭。
// 全程 ~1.05s;仅亮屏关机(低电强睡)前播,熄屏闲置深睡不播(见 app_sleep.c)。
//
// 与 app_boot 同构:全屏覆盖层 + 声明式 lv_anim + master timer,finish()
// 自清理(删覆盖层即自动清子树 anim)。区别:动画帧在 taskLVGL 上跑,而
// 调用方(input_task)不能持 LVGL 锁等待(会饿死 LVGL 任务)——持锁只做
// 构建与启动,立刻放锁,轮询完成标志(20ms 步进,1.5s 超时兜底:深睡的
// 正确性优先于动画的优雅,超时强制收尾后照常入睡)。
#include "app_shutdown.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

#include "app_catalog.h"
#include "app_ui.h"
#include "bsp_display.h"

// ---------- 调色板(与 app_boot 一致) ----------
#define COL_BOOT_BG    0x070A0E
#define COL_A1         0x4D8CFF
#define COL_A2         0x35C6E0
#define COL_INK        0xE6EDF6

// 布局常量(与 app_boot 同一画面坐标)。
#define CX         120          // 画面水平中心
#define CY_GLOW    147          // 光晕/飞机中心 y
#define RISE_PX    23           // 开机动画的飞机上移量(降落起点)

// 时序(ms):全部元素 ~750ms 退场;master 800ms 起背光渐灭,再一拍收尾,
// 总长 ~1.05s(与开机动画等长,开关机节奏对称)。
#define T_MASTER_MS    800
#define T_BL_FADE_MS   200
#define T_FINISH_MS    250

// 光晕 3 层同心圆:直径/峰值透明度(由外到内,与 app_boot 一致)。
static const int glow_diam[3]     = {160, 104, 56};
static const lv_opa_t glow_opa[3] = {22, 34, 51};

static bool s_active;
static volatile bool s_done;      // finish() 置位;input_task 轮询
static lv_obj_t *s_layer;         // 全屏覆盖层(删它即清全部子树动画)
static lv_timer_t *s_master;

// ---------- anim exec 回调 ----------
static void set_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
// 飞机降落:从开机收尾位(升顶 26px)降回光晕中心,同时渐远缩小(镜像)。
static void set_plane_fall_cb(void *obj, int32_t v) {
    lv_obj_t *o = obj;
    int size = 26 - 7 * v / 100;
    int cy = CY_GLOW - RISE_PX + RISE_PX * v / 100;
    lv_obj_set_size(o, size, size);
    lv_obj_set_pos(o, CX - size / 2, cy - size / 2);
}
// 飞机退场:在光晕中心坍缩到 0(开机是同位置的浮现放大)。
static void set_plane_collapse_cb(void *obj, int32_t v) {
    lv_obj_t *o = obj;
    lv_obj_set_size(o, v, v);
    lv_obj_set_pos(o, CX - v / 2, CY_GLOW - v / 2);
}
static void set_arc_rot_cb(void *obj, int32_t v) {
    lv_arc_set_rotation((lv_obj_t *)obj, v);
}

static void anim(lv_obj_t *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                 uint32_t delay, uint32_t dur, lv_anim_path_cb_t path) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

// ---------- 收尾 ----------
static void finish(void) {
    if (s_master) {
        lv_timer_delete(s_master);
        s_master = NULL;
    }
    if (s_layer) {
        lv_obj_delete(s_layer);  // 自动清掉子树上全部 anim
        s_layer = NULL;
    }
    s_active = false;
    s_done = true;
}

static void finish_tick_cb(lv_timer_t *t) {
    (void)t;
    finish();
}

// master timer(一次性):元素已全部退场,覆盖层纯黑——渐灭背光,再等一拍
// (给背光渐灭留出时间)收尾置位。
static void master_tick_cb(lv_timer_t *t) {
    (void)t;
    lv_timer_delete(t);
    s_master = lv_timer_create(finish_tick_cb, T_FINISH_MS, NULL);
    lv_timer_set_repeat_count(s_master, 1);
    bsp_display_backlight_fade(0, T_BL_FADE_MS);
}

// 光晕单层(与 app_boot 同构,时序压缩)。
static lv_obj_t *glow_disc(lv_obj_t *parent, int diam, lv_opa_t max_opa) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, CX - diam / 2, CY_GLOW - diam / 2);
    lv_obj_set_size(o, diam, diam);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_A1), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    anim(o, set_opa_cb, LV_OPA_TRANSP, max_opa, 0, 200, lv_anim_path_ease_in);
    return o;
}

void app_shutdown_play(void) {
    if (s_active) return;
    s_active = true;
    s_done = false;

    // 拿不到锁(LVGL 卡死)就跳过动画直接深睡——深睡优先于动画。
    if (!bsp_lvgl_lock(1000)) {
        s_active = false;
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    s_layer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_layer);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_size(s_layer, 240, 320);
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(COL_BOOT_BG), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_layer, LV_OPA_TRANSP, 0);

    // 黑幕淡入:主 UI 沉入黑暗(150ms 后覆盖层全黑,后续绘制与底层无关)。
    anim(s_layer, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 0, 150,
         lv_anim_path_ease_in);

    // 光晕:最先创建(z 序最底),600-750ms 淡出。
    lv_obj_t *glows[3];
    for (int i = 0; i < 3; i++) {
        glows[i] = glow_disc(s_layer, glow_diam[i], glow_opa[i]);
        anim(glows[i], set_opa_cb, glow_opa[i], LV_OPA_TRANSP, 600, 150,
             lv_anim_path_ease_in);
    }

    // 飞机:复用主 UI 矢量图标,从开机动画收尾位降落、坍缩。
    lv_obj_t *plane = app_ui_icon_create(s_layer, APP_ICON_PLANE,
                                         CX - 13, CY_GLOW - RISE_PX - 13,
                                         26, 26, COL_INK);
    anim(plane, set_plane_fall_cb, 0, 100, 100, 300, lv_anim_path_ease_in);
    anim(plane, set_plane_collapse_cb, 19, 0, 600, 150, lv_anim_path_ease_in);

    // 扫描环:三段弧快速自旋一圈后淡出(与 app_boot 同构,时序压缩)。
    static const int arc_span[3]  = {129, 105, 39};
    static const int arc_rot[3]   = {150, 265, 20};
    static const int arc_w[3]     = {3, 2, 2};
    static const lv_opa_t arc_opa[3] = {230, 128, 77};
    static const uint32_t arc_col[3] = {COL_A2, COL_A1, COL_A2};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *arc = lv_arc_create(s_layer);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_remove_style(arc, NULL, LV_PART_MAIN);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(arc, CX - 54, CY_GLOW - 54);
        lv_obj_set_size(arc, 108, 108);
        lv_arc_set_rotation(arc, arc_rot[i]);
        lv_arc_set_bg_angles(arc, 0, arc_span[i]);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 100);
        lv_obj_set_style_arc_color(arc, lv_color_hex(arc_col[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, arc_w[i], LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_opa(arc, LV_OPA_TRANSP, 0);
        anim(arc, set_opa_cb, LV_OPA_TRANSP, arc_opa[i], 0, 100,
             lv_anim_path_ease_in);
        anim(arc, set_opa_cb, arc_opa[i], LV_OPA_TRANSP, 500, 150,
             lv_anim_path_ease_out);
        anim(arc, set_arc_rot_cb, arc_rot[i], arc_rot[i] + 360, 60, 600,
             lv_anim_path_linear);
    }

    // 光点:降落末段一闪而灭。
    lv_obj_t *dot = lv_obj_create(s_layer);
    lv_obj_remove_style_all(dot);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dot, CX - 3, CY_GLOW - 3);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COL_A2), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(COL_A2), 0);
    lv_obj_set_style_shadow_width(dot, 8, 0);
    lv_obj_set_style_opa(dot, LV_OPA_TRANSP, 0);
    anim(dot, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 350, 70,
         lv_anim_path_ease_in);
    anim(dot, set_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 480, 70,
         lv_anim_path_ease_out);

    // master timer:800ms 起背光渐灭 + 收尾。
    s_master = lv_timer_create(master_tick_cb, T_MASTER_MS, NULL);
    lv_timer_set_repeat_count(s_master, 1);

    bsp_lvgl_unlock();  // 动画帧交给 taskLVGL,本任务放锁轮询

    // 轮询完成(20ms 步进,1.5s 兜底);超时强制收尾——深睡不等动画。
    for (int i = 0; i < 75 && !s_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_done && bsp_lvgl_lock(100)) {
        finish();
        bsp_lvgl_unlock();
    }
}
