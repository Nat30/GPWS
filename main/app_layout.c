// main/app_layout.c —— 几何计算实现。规则见 app_layout.h,数值来自设计稿。
#include "app_layout.h"

#include <stdbool.h>
#include <stdint.h>

void app_layout_compute(const int *card_heights, int n, app_layout_t *out) {
    if (n > APP_MAX_ITEMS) n = APP_MAX_ITEMS;
    out->n = n;

    // B 列起始 y 即偏移 12(只错位一次,不是每张卡加 12)。
    int col_bottom[2] = {0, APP_COL_B_OFFSET}; // 每列下一张卡的 y
    for (int i = 0; i < n; i++) {
        const int col = i & 1;
        const int h = card_heights[i];
        out->card_h[i] = h;
        out->card_y[i] = col_bottom[col];
        col_bottom[col] += h + APP_CARD_GAP;
    }
    // 列净高 = 卡片总高 + gap*(张数-1);B 列另加顶部错位。
    int col_h[2] = {0, 0};
    int cnt[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        col_h[i & 1] += card_heights[i];
        cnt[i & 1]++;
    }
    for (int c = 0; c < 2; c++) {
        if (cnt[c] > 1) col_h[c] += APP_CARD_GAP * (cnt[c] - 1);
    }
    int b_total = APP_COL_B_OFFSET + col_h[1];
    out->content_h = col_h[0] > b_total ? col_h[0] : b_total;
    out->max_scroll = out->content_h - APP_VIEWPORT_H;
    if (out->max_scroll < 0) out->max_scroll = 0;
}

int app_layout_clamp_scroll(const app_layout_t *l, int top) {
    if (top < 0) return 0;
    if (top > l->max_scroll) return l->max_scroll;
    return top;
}

int app_layout_scroll_for_focus(const app_layout_t *l, int index, int top) {
    if (index < 0 || index >= l->n) return app_layout_clamp_scroll(l, top);
    const int t = l->card_y[index];
    const int b = t + l->card_h[index];
    if (t - top < APP_SCROLL_EDGE) top = t - APP_SCROLL_EDGE;
    if (b - top > APP_VIEWPORT_H - APP_SCROLL_EDGE) {
        top = b - APP_VIEWPORT_H + APP_SCROLL_EDGE;
    }
    return app_layout_clamp_scroll(l, top);
}

void app_layout_thumb(const app_layout_t *l, int top,
                      int *visible, int *thumb_h, int *thumb_y) {
    if (l->max_scroll <= 0 || l->content_h <= 0) {
        *visible = 0;
        *thumb_h = *thumb_y = 0;
        return;
    }
    *visible = 1;
    int h = APP_VIEWPORT_H * APP_VIEWPORT_H / l->content_h;
    if (h < APP_THUMB_MIN) h = APP_THUMB_MIN;
    if (h > APP_VIEWPORT_H) h = APP_VIEWPORT_H;
    *thumb_h = h;
    *thumb_y = (int)((int64_t)top * (APP_VIEWPORT_H - h) / l->max_scroll);
}

// 视觉顺序导航:数组顺序(偶A奇B交替)与两列累计高度解耦后,↓ 取「y 更大的卡片中
// y 最小者」(同 y 取左列),↑ 反之。保证焦点在屏幕上单调移动,永不回跳。
int app_layout_visual_next(const app_layout_t *l, int cur, int dir) {
    if (l->n <= 0) return cur;
    if (cur < 0 || cur >= l->n) cur = dir > 0 ? 0 : l->n - 1;
    int best = cur;
    int best_y = -1;
    int cur_y = l->card_y[cur];
    int cur_col = app_layout_card_col(cur);
    for (int j = 0; j < l->n; j++) {
        int y = l->card_y[j];
        if (dir > 0) {
            // 严格向下:同 y 时 A 列(0)在 B 列(1)之前
            bool after = y > cur_y || (y == cur_y && app_layout_card_col(j) > cur_col);
            bool better = best_y < 0 || y < best_y ||
                          (y == best_y && app_layout_card_col(j) < app_layout_card_col(best));
            if (after && better) { best = j; best_y = y; }
        } else {
            // 严格向上:同 y 时 B 列(1)在 A 列(0)之前
            bool after = y < cur_y || (y == cur_y && app_layout_card_col(j) < cur_col);
            bool better = best_y < 0 || y > best_y ||
                          (y == best_y && app_layout_card_col(j) > app_layout_card_col(best));
            if (after && better) { best = j; best_y = y; }
        }
    }
    return best;
}

// 视觉端点:沿 visual_next 一直走到停(步数上限 n,全序保证必停)。走同一条
// 导航序而非重写比较逻辑,端点与导航终点永不分叉;O(n^2) 且 n<=24,环绕按键
// 一次微秒级。
int app_layout_visual_end(const app_layout_t *l, int dir) {
    if (l->n <= 0) return -1;
    int cur = dir > 0 ? 0 : l->n - 1;
    for (int i = 0; i < l->n; i++) {
        int next = app_layout_visual_next(l, cur, dir);
        if (next == cur) return cur;
        cur = next;
    }
    return cur;  // 不可达(全序必停),防御性兜底
}
