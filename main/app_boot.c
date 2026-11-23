// main/app_boot.c —— 开机动画:同心圆光晕/光点/飞机放大/扫描环自旋/标题,
// ~1s 后 160ms 交叉淡出露出主 UI。任意键跳过(由 app_ui_input 触发)。
//
// 全部动效为声明式 lv_anim,按关键帧时刻排布;动画期间渲染量小,taskLVGL
// 栈 12288 充裕。所有动画对象的 var 都在覆盖层子树内,删除覆盖层时 LVGL
// 自动删除绑定它们的 anim,skip/结束皆无需逐个清理(仅 master timer 需停)。
//
// 光晕用 3 层同心低透明圆近似径向渐变(LV_GRAD_DIR_RADIAL 在本板渲染异常,
// 真机表现为大蓝圆块);透明度为首猜值,真机观感可再调。
#include "app_boot.h"

#include "lvgl.h"

#include "app_catalog.h"
#include "app_ui.h"
#include "bsp_display.h"

LV_FONT_DECLARE(app_font_15_bold);
LV_FONT_DECLARE(app_font_10);

// ---------- 调色板(与 app_ui 一致) ----------
#define COL_BOOT_BG    0x070A0E
#define COL_GLOW       0x4D8CFF
#define COL_A1         0x4D8CFF
#define COL_A2         0x35C6E0
#define COL_INK        0xE6EDF6
#define COL_INK2       0x8492A6
#define COL_INK3       0x7C8DA3

// 布局常量(240x320 画布)。
#define CX         120          // 画面水平中心
#define CY_GLOW    147          // 光晕/飞机初始中心 y
#define RISE_PX    23           // 飞机上移量
#define TITLE_Y    170
#define SUB_Y      195
#define READY_Y    234

// 时序(ms):总长 ~1s,随后 160ms 交叉淡出。
#define T_TOTAL_MS     1000
#define T_XFADE_MS     160

// 光晕 3 层同心圆:直径/峰值透明度(由外到内)。
static const int glow_diam[3]     = {160, 104, 56};
static const lv_opa_t glow_opa[3] = {22, 34, 51};

static bool s_active;
static lv_obj_t *s_layer;    // 全屏覆盖层(删它即清全部子树动画)
static lv_timer_t *s_master;

// ---------- anim exec 回调 ----------
static void set_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
// 飞机图标:保持中心 (CX, CY_GLOW) 的方形缩放(draw_event 图标随控件尺寸缩放)。
static void set_plane_size_cb(void *obj, int32_t v) {
    lv_obj_t *o = obj;
    lv_obj_set_size(o, v, v);
    lv_obj_set_pos(o, CX - v / 2, CY_GLOW - v / 2);
}
// 飞机收尾:上移 + 缩小同时进行(v: 0..100 进度)。
static void set_plane_rise_cb(void *obj, int32_t v) {
    lv_obj_t *o = obj;
    int size = 32 - 6 * v / 100;
    int cy = CY_GLOW - RISE_PX * v / 100;
    lv_obj_set_size(o, size, size);
    lv_obj_set_pos(o, CX - size / 2, cy - size / 2);
}
// 飞机退场:在升顶位置(CY_GLOW-RISE_PX)中心坍缩到 0。
static void set_plane_collapse_cb(void *obj, int32_t v) {
    lv_obj_t *o = obj;
    lv_obj_set_size(o, v, v);
    lv_obj_set_pos(o, CX - v / 2, CY_GLOW - RISE_PX - v / 2);
}
static void set_arc_rot_cb(void *obj, int32_t v) {
    lv_arc_set_rotation((lv_obj_t *)obj, v);
}
static void set_lspace_cb(void *obj, int32_t v) {
    lv_obj_set_style_text_letter_space((lv_obj_t *)obj, v, 0);
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

// 无限自旋(删除 arc 时自动停)。
static void anim_spin(lv_obj_t *arc, int32_t from_rot, uint32_t delay) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, set_arc_rot_cb);
    lv_anim_set_values(&a, from_rot, from_rot + 360);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// 光晕单层:同心低透明圆,0-420ms 各自独立淡入到峰值。
static lv_obj_t *glow_disc(lv_obj_t *parent, int diam, lv_opa_t max_opa) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, CX - diam / 2, CY_GLOW - diam / 2);
    lv_obj_set_size(o, diam, diam);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_GLOW), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    anim(o, set_opa_cb, LV_OPA_TRANSP, max_opa, 0, 420, lv_anim_path_ease_in);
    return o;
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
}

static void xfade_ready_cb(lv_anim_t *a) {
    (void)a;
    finish();
}

// master timer(一次性):到点启动覆盖层交叉淡出(160ms),ready 回调里销毁。
// 此时子元素已全部退场,只剩纯黑背景,xfade 即主 UI 的交叉淡入。
static void master_tick_cb(lv_timer_t *t) {
    (void)t;
    lv_timer_delete(t);
    s_master = NULL;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_layer);
    lv_anim_set_exec_cb(&a, set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, T_XFADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, xfade_ready_cb);
    lv_anim_start(&a);
}

void app_boot_play(void) {
    if (s_active) return;
    s_active = true;

    lv_obj_t *scr = lv_screen_active();
    s_layer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_layer);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_size(s_layer, 240, 320);
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(COL_BOOT_BG), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);

    // 光晕:3 层同心圆,最先创建(z 序最底)。
    lv_obj_t *glows[3];
    for (int i = 0; i < 3; i++) glows[i] = glow_disc(s_layer, glow_diam[i], glow_opa[i]);

    // 光点:6px 圆 + 同色辉光。
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

    // 飞机:复用主 UI 矢量图标(随控件尺寸缩放),初始 19px。
    lv_obj_t *plane = app_ui_icon_create(s_layer, APP_ICON_PLANE,
                                         CX - 19 / 2, CY_GLOW - 19 / 2, 19, 19,
                                         COL_INK);

    // 扫描环:三段弧,不同 span/宽度/透明度,各自旋转错开。
    static const int arc_span[3]  = {129, 105, 39};
    static const int arc_rot[3]   = {150, 265, 20};
    static const int arc_w[3]     = {3, 2, 2};
    static const lv_opa_t arc_opa[3] = {230, 128, 77};
    static const uint32_t arc_col[3] = {COL_A2, COL_A1, COL_A2};
    lv_obj_t *rings[3];
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
        rings[i] = arc;
    }

    // 标题/副标/就绪:初始化与入场动画统一用对象 opa(text_opa 会与 opa 相乘,
    // 初始 text_opa=透明会让文案永久不可见——上一版的真机 bug)。
    lv_obj_t *title = lv_label_create(s_layer);
    lv_obj_remove_style_all(title);
    lv_obj_set_style_text_font(title, &app_font_15_bold, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_INK), 0);
    lv_label_set_text(title, "航空警示音");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    lv_obj_set_style_opa(title, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_letter_space(title, 8, 0);

    lv_obj_t *sub = lv_label_create(s_layer);
    lv_obj_remove_style_all(sub);
    lv_obj_set_style_text_font(sub, &app_font_10, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_label_set_text(sub, "FOR AVGEEKS");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, SUB_Y);
    lv_obj_set_style_opa(sub, LV_OPA_TRANSP, 0);

    lv_obj_t *ready = lv_label_create(s_layer);
    lv_obj_remove_style_all(ready);
    lv_obj_set_style_text_font(ready, &app_font_10, 0);
    lv_obj_set_style_text_color(ready, lv_color_hex(COL_INK3), 0);
    lv_obj_set_style_text_letter_space(ready, 2, 0);
    lv_label_set_text(ready, "SYSTEM READY");
    lv_obj_align(ready, LV_ALIGN_TOP_MID, 0, READY_Y);
    lv_obj_set_style_opa(ready, LV_OPA_TRANSP, 0);

    // ---------- 关键帧 ----------
    anim(dot, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 0, 120, lv_anim_path_ease_in);
    anim(dot, set_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 290, 90, lv_anim_path_ease_out);
    anim(plane, set_plane_size_cb, 19, 32, 110, 220, lv_anim_path_ease_out);
    for (int i = 0; i < 3; i++) {
        anim(rings[i], set_opa_cb, LV_OPA_TRANSP, arc_opa[i], 120, 240,
             lv_anim_path_ease_in);
        anim(rings[i], set_opa_cb, arc_opa[i], LV_OPA_TRANSP, 700, 150,
             lv_anim_path_ease_out);
        anim_spin(rings[i], arc_rot[i], 120);
    }
    anim(plane, set_plane_rise_cb, 0, 100, 350, 210, lv_anim_path_ease_out);
    anim(title, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 560, 240,
         lv_anim_path_ease_out);
    anim(title, set_lspace_cb, 8, 2, 560, 240, lv_anim_path_ease_out);
    anim(sub, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 640, 140,
         lv_anim_path_ease_out);
    anim(ready, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 750, 120,
         lv_anim_path_ease_out);
    // 结尾逐对象退场(圆环 700-850/光点 290-380 已自行淡出):
    // 光晕/文案 opa→0,飞机在升顶位置坍缩,随后覆盖层只剩纯黑背景。
    for (int i = 0; i < 3; i++) {
        anim(glows[i], set_opa_cb, glow_opa[i], LV_OPA_TRANSP, 880, 120,
             lv_anim_path_ease_in);
    }
    anim(title, set_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 880, 120,
         lv_anim_path_ease_in);
    anim(sub, set_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 880, 120,
         lv_anim_path_ease_in);
    anim(ready, set_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 880, 120,
         lv_anim_path_ease_in);
    anim(plane, set_plane_collapse_cb, 26, 0, 880, 120, lv_anim_path_ease_in);

    // 先同步刷出首帧(黑覆盖层)再渐亮背光,消除"背光已亮而首帧未到"的白屏窗口。
    lv_refr_now(NULL);
    bsp_display_backlight_fade(100, 300);

    // master timer:1s 后启动覆盖层交叉淡出。
    s_master = lv_timer_create(master_tick_cb, T_TOTAL_MS + 5, NULL);
    lv_timer_set_repeat_count(s_master, 1);
}

bool app_boot_active(void) { return s_active; }

void app_boot_skip(void) {
    if (!s_active) return;
    finish();
}
