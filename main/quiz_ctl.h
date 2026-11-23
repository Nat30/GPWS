// main/quiz_ctl.h —— 听声辨机出题/判定状态机(纯逻辑,host 可测)。
//
// 出题: 24 条环洗牌取正确条目(耗尽重洗,重洗首项不与上题重复);干扰项优先
// 同 en 家族(QUIZ_GROUP: PULL UP 系 3 条全同组,RETARD/STALL/TRAFFIC 系 2 条
// 1 同组 + 1 全库,无组 2 全库);三选项洗序。题型每 5 题第 5 题 = 场景题
// (2:8,不播音),其余听音题(题面自动播一次,播一次即停,不接棒)。
// 判定: 答对 streak+1、800ms 后下一题;答错 streak 归 0(归 0 前无尽成绩已
// 捕获)、回放正确答案音、1500ms 后下一题(挑战)或结算(无尽)。
// 计时推进在调用方(tick 心跳查 resume_us);rng 注入(host 用确定性假随机)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define QUIZ_ROUND_N 10  // 挑战轮题数
#define QUIZ_N 24        // 目录条目数(quiz 是应用专属逻辑,与目录锁定)

// 干扰项分组(同 en 家族): 1=PULL UP 系 2=RETARD 系 3=STALL 系 4=TRAFFIC 系,-1 无组。
static const int8_t QUIZ_GROUP[QUIZ_N] = {
    1, -1, -1, 3, -1, -1, 4, 2, -1, -1, -1, -1, -1, 2, -1, 3, 1, 4, -1, -1, -1, -1, 1, -1,
};

// 场景题文案(idx 对齐目录条目;spec §3.3 为正本, 此处为显示串)。
static const char *const QUIZ_SCENARIO[QUIZ_N] = {
    "起飞后不升反降，最急促的警告连成一片",
    "前方地形逼近，机载雷达反复念同一个词",
    "波音驾驶舱的告警大合辑",
    "机翼失速，连续急促的警告音",
    "EGPWS:「TERRAIN TERRAIN PULL UP」",
    "起飞爬升中雷达探测到风切变",
    "TCAS 提示有飞机正在靠近",
    "着陆拉平，减速提醒连成一串",
    "空客 AP1 断开的电气音",
    "离地不远，下降却太快",
    "主告诫灯亮起的那一声",
    "空客无线电高度喊话合辑",
    "地面通电自检，舱里传来「狗叫」",
    "着陆减速的单次提醒",
    "飞越信标台上空的音频提示",
    "失速警告的另一种风格",
    "波音版连续「PULL UP」",
    "空客版 TCAS 交通提示",
    "决断高度到了，机长喊出那个词",
    "无线电里传来 MAYDAY",
    "地形注意提示音响起",
    "乘务员呼叫铃「叮咚」",
    "最短促的连续拉升警告",
    "TCAS 下达爬升指令",
};

typedef enum { QUIZ_MODE_ROUND = 0, QUIZ_MODE_ENDLESS } quiz_mode_t;
typedef enum { QUIZ_IDLE = 0, QUIZ_ASK, QUIZ_WAIT, QUIZ_SUM } quiz_state_t;

typedef struct {
    quiz_state_t state;   // QUIZ_IDLE=未开(UI 据此路由输入)
    quiz_mode_t mode;
    int question;         // 当前题号(1 起)
    int options[3];       // 选项(条目 idx, 展示序;必含 correct 恰一次)
    int correct;          // 本题正确条目
    bool scenario;        // true=场景题(题面不播音)
    int picked;           // 已选项 0..2(-1 未答)
    bool last_right;
    int streak;           // 当前连对
    int best_streak;      // 本局最长连对
    int correct_cnt;      // 答对数(挑战成绩)
    int score;            // 结算成绩: 挑战=答对数, 无尽=断连时连对数
    // 内部
    int64_t resume_us;    // WAIT -> 推进时刻
    int prev;             // 上一题正确条目(不与上题重复)
    int ring[QUIZ_N];
    int ring_pos;
} quiz_ctl_t;

static inline int quiz_rn(uint32_t (*rng)(void), int n) {
    return (int)(rng() % (uint32_t)n);  // n 小,玩具级取模偏差可忽略
}

// 环洗牌(Fisher-Yates);重洗后首项与上题重复则换位(不与上题连重)。
static inline void quiz_shuffle(quiz_ctl_t *q, uint32_t (*rng)(void)) {
    for (int i = QUIZ_N - 1; i > 0; i--) {
        int j = quiz_rn(rng, i + 1);
        int t = q->ring[i]; q->ring[i] = q->ring[j]; q->ring[j] = t;
    }
    if (q->ring[0] == q->prev && QUIZ_N > 1) {
        int t = q->ring[0]; q->ring[0] = q->ring[1]; q->ring[1] = t;
    }
}

// 生成下一题(正确条目 + 干扰项 + 洗序 + 题型),置 ASK。
static inline void quiz_gen(quiz_ctl_t *q, uint32_t (*rng)(void)) {
    if (q->ring_pos >= QUIZ_N) {
        quiz_shuffle(q, rng);
        q->ring_pos = 0;
    }
    q->correct = q->ring[q->ring_pos++];
    q->prev = q->correct;

    int g = QUIZ_GROUP[q->correct];
    int same[QUIZ_N], ns = 0;
    if (g >= 0) {
        for (int i = 0; i < QUIZ_N; i++) {
            if (i != q->correct && QUIZ_GROUP[i] == g) same[ns++] = i;
        }
    }
    int d[2];
    if (ns >= 2) {
        d[0] = same[quiz_rn(rng, ns)];
        do { d[1] = same[quiz_rn(rng, ns)]; } while (d[1] == d[0]);
    } else {
        if (ns == 1) d[0] = same[0];
        else do { d[0] = quiz_rn(rng, QUIZ_N); } while (d[0] == q->correct);
        do { d[1] = quiz_rn(rng, QUIZ_N); } while (d[1] == q->correct || d[1] == d[0]);
    }

    int pool[3] = {q->correct, d[0], d[1]};
    for (int i = 2; i > 0; i--) {
        int j = quiz_rn(rng, i + 1);
        int t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    for (int i = 0; i < 3; i++) q->options[i] = pool[i];

    q->scenario = (q->question % 5 == 0);  // 每 5 题第 5 题 = 场景(2:8)
    q->picked = -1;
    q->state = QUIZ_ASK;
}

static inline void quiz_start(quiz_ctl_t *q, quiz_mode_t mode, uint32_t (*rng)(void)) {
    q->mode = mode;
    q->question = 1;
    q->streak = q->best_streak = q->correct_cnt = q->score = 0;
    q->last_right = false;
    q->prev = -1;
    q->ring_pos = QUIZ_N;  // 强制首题洗牌
    for (int i = 0; i < QUIZ_N; i++) q->ring[i] = i;
    quiz_gen(q, rng);
}

// 判定(ASK 时调用): 记录对错、推进 streak,置 WAIT 与恢复时刻。
// 无尽成绩 = 断连时的连对数(归 0 前捕获)。
static inline bool quiz_answer(quiz_ctl_t *q, int pick, int64_t now_us) {
    q->picked = pick;
    q->last_right = (q->options[pick] == q->correct);
    if (q->last_right) {
        q->streak++;
        q->correct_cnt++;
        if (q->streak > q->best_streak) q->best_streak = q->streak;
    } else {
        q->score = q->streak;
        q->streak = 0;
    }
    q->state = QUIZ_WAIT;
    q->resume_us = now_us + (q->last_right ? 800 : 1500) * 1000;
    return q->last_right;
}

// WAIT 到时推进: 挑战答完 QUIZ_ROUND_N 题结算(成绩=答对数);无尽答错即结算;
// 否则下一题。非 WAIT 调用无操作。
static inline void quiz_resume(quiz_ctl_t *q, uint32_t (*rng)(void)) {
    if (q->state != QUIZ_WAIT) return;
    if (q->mode == QUIZ_MODE_ROUND) {
        if (q->question >= QUIZ_ROUND_N) {
            q->score = q->correct_cnt;
            q->state = QUIZ_SUM;
            return;
        }
    } else if (!q->last_right) {
        q->state = QUIZ_SUM;
        return;
    }
    q->question++;
    quiz_gen(q, rng);
}
