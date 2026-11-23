// main/bootlog.h —— 重启历史环形日志(纯逻辑,host 可测)。
//
// 每次启动追加一条运行记录{复位原因, 唤醒源, esp_restart 站点, 运行分钟},
// 环形 8 条, 满后覆盖最老。运行中通过 touch 更新当前槽(esp_restart 前写
// 站点+准确时长; 深睡前写终时长大落账);断电截断的运行无 touch, 时长留 0
// (呈现层显示「断电」——诚实区分「知道怎么结束的」与「断电截断」)。
// NVS 持久化在 bootlog.c(host 可测部分在本头文件, 无 IDF 依赖)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BOOTLOG_CAP 8  // 环形容量(新盖旧)

typedef struct {
    uint8_t rr;       // esp_reset_reason_t(复位原因)
    uint8_t wc;       // esp_sleep_wakeup_cause_t(唤醒源; 0=UNDEFINED)
    uint8_t site;     // esp_restart 站点(app_sleep: 0=无/1-5 序列步, 5=TIMER 武装失败)
    uint8_t run_min;  // 本运行存活分钟(0=断电截断或 <1min)
} bootlog_rec_t;

typedef struct {
    bootlog_rec_t rec[BOOTLOG_CAP];
    int count;  // 已有条数(count <= BOOTLOG_CAP; 追加后 head 指向最新)
    int head;   // 最新记录下标(空日志 head 任意; 追加时先推进再写)
} bootlog_t;

// 追加一条记录(新 head);满容量覆盖最老(count 封顶 CAP)。
static inline void bootlog_append(bootlog_t *b, uint8_t rr, uint8_t wc,
                                  uint8_t site, uint8_t run_min) {
    b->head = (b->head + 1) % BOOTLOG_CAP;
    b->rec[b->head] = (bootlog_rec_t){rr, wc, site, run_min};
    if (b->count < BOOTLOG_CAP) b->count++;
}

// 运行中更新当前槽(head):esp_restart 站点与准确时长双落账点用。
static inline void bootlog_touch(bootlog_t *b, uint8_t site, uint8_t run_min) {
    b->rec[b->head].site = site;
    b->rec[b->head].run_min = run_min;
}

// 第 n 新记录(0=本次运行, 1=上次, ...);n 越界返回 false。
// 新→旧序遍历: for (n=0; bootlog_get(b, n, &r); n++)
static inline bool bootlog_get(const bootlog_t *b, int n, bootlog_rec_t *out) {
    if (n < 0 || n >= b->count) return false;
    int i = (b->head - n + BOOTLOG_CAP * (n / BOOTLOG_CAP + 1)) % BOOTLOG_CAP;
    // head - n 可能负; + CAP*(n/CAP+1) 保证非负后再模(上式一次展开即此)。
    if (i < 0) i += BOOTLOG_CAP;
    *out = b->rec[i];
    return true;
}

// ---- NVS 持久化壳(bootlog.c) ----
// 启动初始化: load(或旧散键迁移 seed)→ append 本次 → save。
void bootlog_init(uint8_t rr, uint8_t wc);
// 更新当前槽的站点+运行分钟(esp_restart 前/深睡前落账)并落盘。
void bootlog_touch_site(uint8_t site);
// 常驻快照指针(单写者, 只读遍历安全)。保存失败只打日志, 调用方无需处理。
const bootlog_t *bootlog_snapshot(void);
// esp_reset_reason_t 短名(启动日志与诊断页共用显示; 越界 "?")。
// 参数 int: 本头文件不得 include IDF 头(host 纪律), 调用方传枚举即可。
const char *reset_reason_name(int rr);
