// main/app_audio.h —— 警示音 PCM 播放:独立工作任务,UI 只发命令/轮询状态。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 创建播放任务(幂等;应用启动时调一次)。
esp_err_t app_audio_start(void);

// NVS 设置已加载(播放任务已启动)?UI 构建早于 init_deferred,开机需等
// 此标志做一次性状态同步(历史播放计数角标等)。
bool app_audio_started(void);

// 播放条目 index(打断当前播放;切换延迟 <150ms 由 1024 采样 chunk 保证)。
void app_audio_play(int index);

// 播放一次(Quiz 题面/答案回放):自然结束即停,连播开着也不接棒;其余同上。
void app_audio_play_once(int index);

// 场景序列播放(scenes.h 表):按表顺序播完自动停(表尾不循环),打断一切在播;
// 与 autoplay 开关正交。seq 指向静态存储(scenes.h),存续期须覆盖播放期。
void app_audio_play_scene(const int8_t *seq, int n);

// 单曲循环(闹钟响铃):自然结束重播当前条直到被打断(任何播放/停止命令)。
// 循环重播不计入播放统计(闹钟是功能音,不是聆听;spec §7 只计 Quiz/场景)。
void app_audio_play_loop(int index);

// 暂停/继续(无活跃条目时忽略)。
void app_audio_pause_resume(void);

// 停止播放并复位状态(连播链一并终止)。
void app_audio_stop(void);

// ---- UI 轮询的状态(命令生效后反映,最大滞后一个 chunk ≈64ms) ----
bool app_audio_active(void);        // 有活跃条目(含暂停)
int app_audio_current(void);        // 当前条目 index,无则 -1
bool app_audio_paused(void);
uint32_t app_audio_elapsed_ms(void); // 已播时长(暂停时冻结)
uint32_t app_audio_seq(void);        // 播放序号:每次开始播一个条目自增(UI 跟随用)

// ---- 设置弹窗项(NVS 持久化,namespace "gpws_app") ----
// 音量 0..100(钳制)。播放中(含暂停)即时生效,否则下次播放生效。默认 80。
void app_audio_set_volume(int percent);
int app_audio_volume(void);

// 连播:条目自然结束后自动接下一条(顺序回绕或随机,见下)。打断/停止/坏条目不接棒。
void app_audio_set_autoplay(bool on);
bool app_audio_autoplay(void);

// 随机连播:连播接棒改为随机选条(不与当前重复)。单曲播放无关,默认开。
void app_audio_set_random(bool on);
bool app_audio_random(void);

// 聆听模式:播放中闲置熄屏只灭背光不停播(仅保护播放中,暂停不受保护)。
// 仅随连播出现;关连播联动关闭(含持久化)。默认关。
void app_audio_set_listen(bool on);
bool app_audio_listening(void);

// 每条目播放计数(每次开始播放 +1,9999 封顶)。UI 角标数据源。
unsigned app_audio_play_count(int index);

// 每条目累计已播秒数(打断计已播,暂停冻结不计,65535 封顶)。统计弹窗数据源。
unsigned app_audio_play_secs(int index);

// 落盘播放计数(熄屏/深睡前调;弹窗关闭的 settings_save 不含计数)。
void app_audio_counts_save(void);

// 落盘脏设置(弹窗关闭时调;连调音量的快速步进合并为一次 NVS commit)。
void app_audio_settings_save(void);
