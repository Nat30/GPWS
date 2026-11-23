// 瀑布流宫格几何计算的 host 测试:分列、错位、滚动 clamp、焦点可见性、滑块。
#include <assert.h>
#include <stdio.h>

#include "app_layout.h"

int main(void) {
    // 目录定稿的 10 张卡(72/96 交替,见 app_catalog.c)。
    static const int heights[10] = {72, 96, 96, 72, 96, 96, 72, 96, 72, 96};
    app_layout_t l;
    app_layout_compute(heights, 10, &l);

    // A 列(偶数索引)列内 y 与 B 列(奇数索引)全局 y(列内 y + 12 错位)。
    // 高度序列 {72,96,96,72,96,96,72,96,72,96}:B 列(idx1/3/5/7/9)= 96,72,96,96,96。
    assert(l.card_y[0] == 0);
    assert(l.card_y[1] == 12);
    assert(l.card_y[2] == 80);
    assert(l.card_y[3] == 116);
    assert(l.card_y[4] == 184);
    assert(l.card_y[5] == 196);
    assert(l.card_y[6] == 288);
    assert(l.card_y[7] == 300);
    assert(l.card_y[8] == 368);
    assert(l.card_y[9] == 404);

    // A 列净高 408+32=440,B 列 12+456+32=500,取高 500;可滚动 272。
    assert(l.content_h == 500);
    assert(l.max_scroll == 272);

    // clamp。
    assert(app_layout_clamp_scroll(&l, -5) == 0);
    assert(app_layout_clamp_scroll(&l, 300) == 272);
    assert(app_layout_clamp_scroll(&l, 100) == 100);

    // 焦点可见性:首卡在顶部,滚不动。
    assert(app_layout_scroll_for_focus(&l, 0, 0) == 0);
    // 末卡(index 9,y=404 h=96,b=500):底部越界,滚到 b-228+8=280,clamp 272。
    assert(app_layout_scroll_for_focus(&l, 9, 0) == 272);
    // 中部卡(index 5,y=196 h=96):从 0 滚到 72;从 272 回滚到 188。
    assert(app_layout_scroll_for_focus(&l, 5, 0) == 72);
    assert(app_layout_scroll_for_focus(&l, 5, 272) == 188);
    // 越界索引:原样 clamp。
    assert(app_layout_scroll_for_focus(&l, -1, 999) == 272);
    assert(app_layout_scroll_for_focus(&l, 10, 999) == 272);

    // 性质:任意索引+任意起点,结果使卡片完整可见(或到达滚动边界)。
    for (int i = 0; i < 10; i++) {
        for (int top = -20; top <= 300; top += 7) {
            int t = app_layout_scroll_for_focus(&l, i, top);
            assert(t >= 0 && t <= l.max_scroll);
            assert(l.card_y[i] - t >= 0 || t == 0);
            assert(l.card_y[i] + l.card_h[i] - t <= APP_VIEWPORT_H || t == l.max_scroll);
        }
    }

    // 滑块:可见,高度 228*228/500=103,中点位置 61。
    int visible, th, ty;
    app_layout_thumb(&l, 0, &visible, &th, &ty);
    assert(visible && th == 103 && ty == 0);
    app_layout_thumb(&l, 134, &visible, &th, &ty);
    assert(visible && ty == 61);
    app_layout_thumb(&l, 272, &visible, &th, &ty);
    assert(visible && ty == 125);

    // 内容不满一屏:不可滚、滑块隐藏。
    static const int two[2] = {72, 72};
    app_layout_t s;
    app_layout_compute(two, 2, &s);
    assert(s.content_h == 84);
    assert(s.max_scroll == 0);
    assert(app_layout_clamp_scroll(&s, 40) == 0);
    app_layout_thumb(&s, 0, &visible, &th, &ty);
    assert(!visible);
    // 焦点可见性在不可滚时也应返回 0。
    assert(app_layout_scroll_for_focus(&s, 1, 0) == 0);

    // ---- 视觉顺序导航(24 条真实卡高,偶A奇B交替) ----
    static const int h24[24] = {
        72, 96, 96, 72, 96, 96, 72, 96, 72, 96, 72, 96,
        72, 96, 72, 96, 72, 96, 72, 96, 72, 96, 72, 96,
    };
    app_layout_t v;
    app_layout_compute(h24, 24, &v);
    // 旧数组序导航在 13→14 处视觉回跳(y 612→608);视觉导航必须严格向下。
    int step = app_layout_visual_next(&v, 13, 1);
    assert(step != 13 && v.card_y[step] > 612);
    // 性质:视觉序是 (y,列) 严格全序。从任意卡 ↓ 走到底、↑ 走到顶:
    // 每步新卡(无环)、y 单调不回跳;两段路径并集恰为全部 24 卡。
    for (int start = 0; start < 24; start++) {
        int seen[APP_MAX_ITEMS] = {0};
        seen[start] = 1;
        int count = 1;
        // ↓ 直到停在自身(到底)。
        int cur = start, prev_y = v.card_y[start];
        for (int k = 0; k < 24; k++) {
            int next = app_layout_visual_next(&v, cur, 1);
            assert(next >= 0 && next < 24);
            if (next == cur) break;                    // 到底
            assert(v.card_y[next] >= prev_y);          // 永不视觉回跳
            assert(!seen[next]);                       // 无环,新卡
            seen[next] = 1;
            count++;
            prev_y = v.card_y[next];
            cur = next;
        }
        assert(cur == app_layout_visual_next(&v, cur, 1));  // 确已到底
        // ↑ 从起点直到到顶。
        cur = start;
        prev_y = v.card_y[start];
        for (int k = 0; k < 24; k++) {
            int next = app_layout_visual_next(&v, cur, -1);
            if (next == cur) break;                    // 到顶
            assert(v.card_y[next] <= prev_y);
            assert(!seen[next]);
            seen[next] = 1;
            count++;
            prev_y = v.card_y[next];
            cur = next;
        }
        assert(cur == app_layout_visual_next(&v, cur, -1));  // 确已到顶
        assert(count == 24);                           // 两段并集=全部
    }

    // ---- 视觉端点与首尾环绕 ----
    // 顶恒为卡 0(A 列 y=0 起步,无卡在其前);底 = 走到底停的那张(此处卡 23)。
    assert(app_layout_visual_end(&v, -1) == 0);
    int bottom = app_layout_visual_end(&v, 1);
    assert(bottom >= 0 && bottom < 24);
    assert(bottom == app_layout_visual_next(&v, bottom, 1));  // 端点无后继
    // 端点是两个方向的停点:从端点出发再走必停在自身。
    for (int start = 0; start < 24; start++) {
        int cur = start;
        for (int k = 0; k < 24; k++) {
            int next = app_layout_visual_next(&v, cur, 1);
            if (next == cur) break;
            cur = next;
        }
        assert(cur == bottom);  // 任意起点 ↓ 必停视觉底
        cur = start;
        for (int k = 0; k < 24; k++) {
            int next = app_layout_visual_next(&v, cur, -1);
            if (next == cur) break;
            cur = next;
        }
        assert(cur == 0);       // 任意起点 ↑ 必停视觉顶
    }
    // 环绕语义:底按 ↓(停)→ 绕到顶;顶按 ↑(停)→ 绕到底。
    assert(app_layout_visual_end(&v, -1) == 0);
    assert(app_layout_visual_end(&v, 1) == bottom);
    // 单卡:两端点同为该卡,环绕 no-op(调用方以 next==cur 守卫)。
    static const int one[1] = {72};
    app_layout_t u;
    app_layout_compute(one, 1, &u);
    assert(app_layout_visual_end(&u, 1) == 0);
    assert(app_layout_visual_end(&u, -1) == 0);
    assert(app_layout_visual_next(&u, 0, 1) == 0);  // 单卡自停
    // 空目录防御。
    app_layout_t e;
    app_layout_compute(one, 0, &e);
    assert(app_layout_visual_end(&e, 1) == -1);

    puts("app_layout geometry tests: PASS");
    return 0;
}
