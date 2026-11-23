// main/app_layout.h —— 瀑布流宫格的纯几何计算(不依赖 LVGL/ESP-IDF,host 可测)。
//
// 240x320 画布四段:状态栏 22 / 页头 44 / 宫格视口 228 / 底栏 26。
// 两列等宽 104,列间距 8,列内卡片间距 8;B 列顶部整体下移 12 形成瀑布流错位。
// 偶数索引进 A 列,奇数进 B 列(与卡片阅读顺序一致)。
#pragma once

#define APP_SCREEN_W     240
#define APP_SCREEN_H     320
#define APP_STATUS_H     22
#define APP_HEADER_H     44
#define APP_VIEWPORT_H   228
#define APP_BOTTOM_H     26
#define APP_PAD_X        12   // 屏幕左右内边距
#define APP_COL_W        104
#define APP_COL_GAP      8
#define APP_CARD_GAP     8
#define APP_COL_B_OFFSET 12   // B 列顶部错位
#define APP_COL_B_X_IN_CONTENT (APP_COL_W + APP_COL_GAP)  // B 列在内容盒内 x=112
#define APP_SCROLL_EDGE  8    // 焦点进入视口边缘 8px 内触发滚动
#define APP_THUMB_MIN    24   // 滚动条滑块最小高度

#define APP_MAX_ITEMS 32

typedef struct {
    int card_y[APP_MAX_ITEMS]; // 卡片在内容坐标系中的 y(含 B 列 12px 偏移)
    int card_h[APP_MAX_ITEMS]; // 卡片高度副本
    int n;
    int content_h;   // 内容总高(两列取高,含 B 列偏移)
    int max_scroll;  // content_h - 视口高,<0 记 0
} app_layout_t;

// card_heights[i] = 第 i 张卡的高度(72/96)。
void app_layout_compute(const int *card_heights, int n, app_layout_t *out);

int app_layout_clamp_scroll(const app_layout_t *l, int top);

// 使卡片 index 完整可见(焦点移动时卡片进入视口边缘 APP_SCROLL_EDGE 内则滚动)。
int app_layout_scroll_for_focus(const app_layout_t *l, int index, int top);

// 视觉顺序导航(↓/↑):按卡片 y 坐标(而非数组序号)取下一张,焦点在屏幕上单调
// 移动。dir>0 向下,dir<0 向上;到顶/到底返回 cur。
int app_layout_visual_next(const app_layout_t *l, int cur, int dir);

// 视觉端点(dir>0 = 视觉底,dir<0 = 视觉顶):沿 visual_next 走到停的那张卡。
// 供首尾环绕导航跳到另一端(实现走同一条导航序,端点永不分叉)。n<=0 返回 -1。
int app_layout_visual_end(const app_layout_t *l, int dir);

// 滚动条滑块:内容不满一屏时 *visible=0。
void app_layout_thumb(const app_layout_t *l, int top,
                      int *visible, int *thumb_h, int *thumb_y);

// 卡片所在列(0=A,1=B)与列内 x 坐标(内容盒坐标系:内容盒已在屏幕 x=APP_PAD_X,
// A 列 0,B 列 APP_COL_B_X_IN_CONTENT —— 勿再叠加屏幕内边距,否则右列被裁)。
static inline int app_layout_card_col(int index) { return index & 1; }
static inline int app_layout_card_x(int index) {
    return (index & 1) ? APP_COL_B_X_IN_CONTENT : 0;
}
