// main/app_catalog.h —— 警示音条目表:数据驱动,新增条目只改这里(和音频资源)。
#pragma once

#include <stddef.h>
#include <stdint.h>

// Entry count: single compile-time source shared by app_catalog.c (array size)
// and app_ui.c (static object arrays). Adding an entry without bumping this
// fails to compile (initializer overflow) instead of overflowing UI arrays.
#define APP_CATALOG_COUNT 24u

typedef enum {
    APP_ICON_UP = 0,
    APP_ICON_DOWN,
    APP_ICON_MTN,
    APP_ICON_PLANE,
    APP_ICON_GAUGE,
    APP_ICON_WIND,
    APP_ICON_RADAR,
    APP_ICON_BELL,
    APP_ICON_VOICE,
    APP_ICON_AP,
    APP_ICON_PACK,
    APP_ICON_BEACON,
    APP_ICON_GEAR,
    APP_ICON_ALT,
    APP_ICON_CHIME,
    APP_ICON_RADIO,
} app_icon_t;

// PCM 固定 16 kHz / 16 bit / mono 小端。
typedef struct {
    const char *name;   // 中文名称
    const char *en;     // 英文术语(显示时全大写)
    uint16_t dur_ms;    // 音频时长,进度条总长
    uint32_t acc;       // 强调色 0xRRGGBB
    app_icon_t icon;
    uint8_t card_h;     // 卡片高度档:72 / 96
    const int16_t *pcm;     // 16k/16bit/mono 小端数据起点(NULL=资源缺失)
    const int16_t *pcm_end; // 数据终点(字节数 = 差值,静态初始化器无法存地址差)
} app_alert_t;

const app_alert_t *app_catalog(int index);
int app_catalog_count(void);

// 音频数据字节数。
static inline size_t app_alert_pcm_bytes(const app_alert_t *a) {
    return (size_t)(a->pcm_end - a->pcm) * sizeof(int16_t);
}
