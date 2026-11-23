// Host tests for the quiz question/judging state machine (quiz_ctl.h).
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "quiz_ctl.h"

// Deterministic LCG so failures reproduce.
static uint32_t s_rng = 12345u;
static uint32_t rng(void) { return s_rng = s_rng * 1664525u + 1013904223u; }

static int right_pick(const quiz_ctl_t *q) {
    for (int i = 0; i < 3; i++)
        if (q->options[i] == q->correct) return i;
    assert(false);
    return -1;
}

static int wrong_pick(const quiz_ctl_t *q) {
    for (int i = 0; i < 3; i++)
        if (q->options[i] != q->correct) return i;
    assert(false);
    return -1;
}

int main(void) {
    quiz_ctl_t q;

    // Scenario prompt table: one non-empty prompt per catalog entry.
    for (int i = 0; i < QUIZ_N; i++) assert(QUIZ_SCENARIO[i] && QUIZ_SCENARIO[i][0]);

    // ---- endless right streak: cadence, options, ring order, timing ----
    quiz_start(&q, QUIZ_MODE_ENDLESS, rng);
    assert(q.state == QUIZ_ASK && q.question == 1);
    bool seen[QUIZ_N] = {false};
    int prev_correct = -1;
    for (int n = 1; n <= 60; n++) {  // > 2x ring: covers reshuffles
        assert(q.state == QUIZ_ASK);
        assert(q.question == n);
        // Cadence: every 5th question is a scenario question (2:8).
        assert(q.scenario == (n % 5 == 0));
        // Options: 3 distinct in-range values, correct present exactly once.
        int correct_seen = 0;
        for (int i = 0; i < 3; i++) {
            assert(q.options[i] >= 0 && q.options[i] < QUIZ_N);
            for (int j = i + 1; j < 3; j++) assert(q.options[i] != q.options[j]);
            if (q.options[i] == q.correct) correct_seen++;
        }
        assert(correct_seen == 1);
        // Distractor families: 3-member family -> all options same family;
        // 2-member family -> exactly one same-family distractor.
        int g = QUIZ_GROUP[q.correct];
        if (g == 1) {
            for (int i = 0; i < 3; i++) assert(QUIZ_GROUP[q.options[i]] == g);
        } else if (g >= 0) {
            int same = 0;
            for (int i = 0; i < 3; i++)
                if (q.options[i] != q.correct && QUIZ_GROUP[q.options[i]] == g) same++;
            assert(same == 1);
        }
        // No consecutive repeat of the correct entry, even across reshuffles.
        assert(q.correct != prev_correct);
        prev_correct = q.correct;
        // Ring order is a permutation: each 24-question window covers all.
        if (n <= 48) {
            if (n == 25) memset(seen, 0, sizeof(seen));
            assert(!seen[q.correct]);
            seen[q.correct] = true;
            if (n == 24 || n == 48) {
                for (int i = 0; i < QUIZ_N; i++) assert(seen[i]);
            }
        }
        // Answer right: streak grows, resume 800 ms out.
        int64_t now = (int64_t)n * 1000000;
        assert(quiz_answer(&q, right_pick(&q), now));
        assert(q.state == QUIZ_WAIT && q.last_right);
        assert(q.resume_us == now + 800 * 1000);
        assert(q.streak == n && q.best_streak == n && q.correct_cnt == n);
        quiz_resume(&q, rng);
    }
    // Still running: endless never sums on right answers.
    assert(q.state == QUIZ_ASK && q.question == 61);

    // ---- round mode: 10 questions, one wrong; score = correct count ----
    quiz_start(&q, QUIZ_MODE_ROUND, rng);
    for (int n = 1; n <= QUIZ_ROUND_N; n++) {
        bool want_right = (n != 5);
        int64_t now = (int64_t)n * 1000000;
        bool got = quiz_answer(&q, want_right ? right_pick(&q) : wrong_pick(&q), now);
        assert(got == want_right);
        assert(q.resume_us == now + (want_right ? 800 : 1500) * 1000);
        quiz_resume(&q, rng);
        if (n < QUIZ_ROUND_N) {
            assert(q.state == QUIZ_ASK && q.question == n + 1);
        } else {
            assert(q.state == QUIZ_SUM);
        }
    }
    // 4 right, wrong (streak reset), 5 right -> best streak 5, score 9/10.
    assert(q.score == 9 && q.correct_cnt == 9 && q.best_streak == 5);
    // resume outside WAIT is a no-op.
    quiz_resume(&q, rng);
    assert(q.state == QUIZ_SUM);

    // ---- endless: wrong answer ends the run, score = streak at the break ----
    quiz_start(&q, QUIZ_MODE_ENDLESS, rng);
    int64_t now = 9000000;
    for (int i = 0; i < 3; i++) {
        assert(quiz_answer(&q, right_pick(&q), now));
        quiz_resume(&q, rng);
    }
    assert(q.streak == 3 && q.best_streak == 3);
    now += 1000000;
    assert(quiz_answer(&q, wrong_pick(&q), now) == false);
    assert(q.state == QUIZ_WAIT && q.streak == 0 && q.score == 3);
    assert(q.resume_us == now + 1500 * 1000);
    assert(q.picked >= 0 && q.options[q.picked] != q.correct);
    quiz_resume(&q, rng);
    assert(q.state == QUIZ_SUM && q.score == 3 && q.correct_cnt == 3);

    // ---- endless: wrong on the very first question -> score 0 ----
    quiz_start(&q, QUIZ_MODE_ENDLESS, rng);
    assert(quiz_answer(&q, wrong_pick(&q), 1000) == false);
    assert(q.score == 0);
    quiz_resume(&q, rng);
    assert(q.state == QUIZ_SUM && q.score == 0);

    puts("quiz state-machine tests: PASS");
    return 0;
}
