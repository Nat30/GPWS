// main/app_catalog.c —— 24 条警示音条目(音频源:aviation_cockpit_sounds_organized,
// 名称按文件名精简翻译;v1 名称备份见 tools/catalog_names_v1.txt)。
// PCM 经 CMake EMBED_FILES 嵌入,符号名为 _binary_<basename>_start/_end('.'→'_')。
#include "app_catalog.h"

// idf_component_register(EMBED_FILES ...) 生成的符号。
extern const int16_t _binary_gpws_whoop_pull_up_pcm_start[];
extern const int16_t _binary_gpws_whoop_pull_up_pcm_end[];
extern const int16_t _binary_gpws_terrain_pcm_start[];
extern const int16_t _binary_gpws_terrain_pcm_end[];
extern const int16_t _binary_boeing_777_alerts_pack_pcm_start[];
extern const int16_t _binary_boeing_777_alerts_pack_pcm_end[];
extern const int16_t _binary_airbus_stall_warning_pcm_start[];
extern const int16_t _binary_airbus_stall_warning_pcm_end[];
extern const int16_t _binary_gpws_windshear_pcm_start[];
extern const int16_t _binary_gpws_windshear_pcm_end[];
extern const int16_t _binary_tcas_traffic_pcm_start[];
extern const int16_t _binary_tcas_traffic_pcm_end[];
extern const int16_t _binary_egpws_terrain_pull_up_pcm_start[];
extern const int16_t _binary_egpws_terrain_pull_up_pcm_end[];
extern const int16_t _binary_gpws_sink_rate_pcm_start[];
extern const int16_t _binary_gpws_sink_rate_pcm_end[];
extern const int16_t _binary_airbus_callout_retard_loop_pcm_start[];
extern const int16_t _binary_airbus_callout_retard_loop_pcm_end[];
extern const int16_t _binary_avionics_master_caution_pcm_start[];
extern const int16_t _binary_avionics_master_caution_pcm_end[];
extern const int16_t _binary_airbus_ap1_disconnect_pcm_start[];
extern const int16_t _binary_airbus_ap1_disconnect_pcm_end[];
extern const int16_t _binary_airbus_callout_retard_pcm_start[];
extern const int16_t _binary_airbus_callout_retard_pcm_end[];
extern const int16_t _binary_airbus_gpws_pack_pcm_start[];
extern const int16_t _binary_airbus_gpws_pack_pcm_end[];
extern const int16_t _binary_airbus_marker_beacon_pcm_start[];
extern const int16_t _binary_airbus_marker_beacon_pcm_end[];
extern const int16_t _binary_airbus_ptu_bark_pcm_start[];
extern const int16_t _binary_airbus_ptu_bark_pcm_end[];
extern const int16_t _binary_airbus_tcas_traffic_pcm_start[];
extern const int16_t _binary_airbus_tcas_traffic_pcm_end[];
extern const int16_t _binary_avionics_stall_pcm_start[];
extern const int16_t _binary_avionics_stall_pcm_end[];
extern const int16_t _binary_boeing_approaching_minimums_pcm_start[];
extern const int16_t _binary_boeing_approaching_minimums_pcm_end[];
extern const int16_t _binary_boeing_gpws_pull_up_pcm_start[];
extern const int16_t _binary_boeing_gpws_pull_up_pcm_end[];
extern const int16_t _binary_cabin_chime_ding_dong_pcm_start[];
extern const int16_t _binary_cabin_chime_ding_dong_pcm_end[];
extern const int16_t _binary_radio_mayday_call_pcm_start[];
extern const int16_t _binary_radio_mayday_call_pcm_end[];
extern const int16_t _binary_gpws_pull_up_pcm_start[];
extern const int16_t _binary_gpws_pull_up_pcm_end[];
extern const int16_t _binary_gpws_terrain_alert_pcm_start[];
extern const int16_t _binary_gpws_terrain_alert_pcm_end[];
extern const int16_t _binary_tcas_ra_climb_pcm_start[];
extern const int16_t _binary_tcas_ra_climb_pcm_end[];

// 条目顺序 = 焦点移动顺序;偶数索引在 A 列,奇数在 B 列(app_layout 固定该规则)。
// ★ 顺序约束:card_h 槽位按 (72,96),(96,72) 对角交替——A/B 两列累计高度均衡
// (11×72+13×96 无法完美对分,按此序两列差 24px,已是理论最优)。新增条目时
// 保持"填入使两列累计高度最接近对方"的槽位,勿把同高卡片连续堆到一列。
static const app_alert_t ALERTS[APP_CATALOG_COUNT] = {
    { .name = "呼啸拉升", .en = "WHOOP PULL UP", .dur_ms = 3376, .acc = 0xFF4D4F,
      .icon = APP_ICON_VOICE, .card_h = 72,
      .pcm = _binary_gpws_whoop_pull_up_pcm_start,
      .pcm_end = _binary_gpws_whoop_pull_up_pcm_end },
    { .name = "近地警告", .en = "TERRAIN", .dur_ms = 3000, .acc = 0xFF9A3D,
      .icon = APP_ICON_MTN, .card_h = 96,
      .pcm = _binary_gpws_terrain_pcm_start,
      .pcm_end = _binary_gpws_terrain_pcm_end },
    { .name = "波音警告", .en = "B777 ALERTS", .dur_ms = 3000, .acc = 0xFF6BA8,
      .icon = APP_ICON_BELL, .card_h = 96,
      .pcm = _binary_boeing_777_alerts_pack_pcm_start,
      .pcm_end = _binary_boeing_777_alerts_pack_pcm_end },
    { .name = "失速警告", .en = "STALL", .dur_ms = 7900, .acc = 0xFFCE3D,
      .icon = APP_ICON_PLANE, .card_h = 72,
      .pcm = _binary_airbus_stall_warning_pcm_start,
      .pcm_end = _binary_airbus_stall_warning_pcm_end },
    { .name = "地形拉起", .en = "TERRAIN PULL UP", .dur_ms = 1700, .acc = 0xFF9A3D,
      .icon = APP_ICON_MTN, .card_h = 72,
      .pcm = _binary_egpws_terrain_pull_up_pcm_start,
      .pcm_end = _binary_egpws_terrain_pull_up_pcm_end },
    { .name = "风切变", .en = "WINDSHEAR", .dur_ms = 2500, .acc = 0x35C6E0,
      .icon = APP_ICON_WIND, .card_h = 96,
      .pcm = _binary_gpws_windshear_pcm_start,
      .pcm_end = _binary_gpws_windshear_pcm_end },
    { .name = "冲突警告", .en = "TRAFFIC", .dur_ms = 3100, .acc = 0xFF4D4F,
      .icon = APP_ICON_RADAR, .card_h = 96,
      .pcm = _binary_tcas_traffic_pcm_start,
      .pcm_end = _binary_tcas_traffic_pcm_end },
    { .name = "减速连呼", .en = "RETARD LOOP", .dur_ms = 5400, .acc = 0x35D0A5,
      .icon = APP_ICON_GAUGE, .card_h = 72,
      .pcm = _binary_airbus_callout_retard_loop_pcm_start,
      .pcm_end = _binary_airbus_callout_retard_loop_pcm_end },
    { .name = "自动驾驶断开", .en = "AP DISCONNECT", .dur_ms = 5100, .acc = 0x4D8CFF,
      .icon = APP_ICON_AP, .card_h = 72,
      .pcm = _binary_airbus_ap1_disconnect_pcm_start,
      .pcm_end = _binary_airbus_ap1_disconnect_pcm_end },
    { .name = "下沉率警告", .en = "SINK RATE", .dur_ms = 1400, .acc = 0xFF9A3D,
      .icon = APP_ICON_DOWN, .card_h = 96,
      .pcm = _binary_gpws_sink_rate_pcm_start,
      .pcm_end = _binary_gpws_sink_rate_pcm_end },
    { .name = "主警告", .en = "MASTER CAUTION", .dur_ms = 4088, .acc = 0x9B7BFF,
      .icon = APP_ICON_BELL, .card_h = 96,
      .pcm = _binary_avionics_master_caution_pcm_start,
      .pcm_end = _binary_avionics_master_caution_pcm_end },
    { .name = "空客报高", .en = "AIRBUS GPWS", .dur_ms = 9870, .acc = 0xFF9A3D,
      .icon = APP_ICON_PACK, .card_h = 72,
      .pcm = _binary_airbus_gpws_pack_pcm_start,
      .pcm_end = _binary_airbus_gpws_pack_pcm_end },
    { .name = "液压狗叫", .en = "PTU BARK", .dur_ms = 4990, .acc = 0xFFCE3D,
      .icon = APP_ICON_GEAR, .card_h = 72,
      .pcm = _binary_airbus_ptu_bark_pcm_start,
      .pcm_end = _binary_airbus_ptu_bark_pcm_end },
    { .name = "减速提醒", .en = "RETARD", .dur_ms = 2200, .acc = 0x35D0A5,
      .icon = APP_ICON_GAUGE, .card_h = 96,
      .pcm = _binary_airbus_callout_retard_pcm_start,
      .pcm_end = _binary_airbus_callout_retard_pcm_end },
    { .name = "指点信标", .en = "MARKER", .dur_ms = 2200, .acc = 0x35C6E0,
      .icon = APP_ICON_BEACON, .card_h = 96,
      .pcm = _binary_airbus_marker_beacon_pcm_start,
      .pcm_end = _binary_airbus_marker_beacon_pcm_end },
    { .name = "空客失速", .en = "STALL", .dur_ms = 10000, .acc = 0xFFCE3D,
      .icon = APP_ICON_PLANE, .card_h = 72,
      .pcm = _binary_avionics_stall_pcm_start,
      .pcm_end = _binary_avionics_stall_pcm_end },
    { .name = "波音拉升", .en = "PULL UP", .dur_ms = 5014, .acc = 0xFF4D4F,
      .icon = APP_ICON_UP, .card_h = 72,
      .pcm = _binary_boeing_gpws_pull_up_pcm_start,
      .pcm_end = _binary_boeing_gpws_pull_up_pcm_end },
    { .name = "空客冲突", .en = "TRAFFIC", .dur_ms = 9760, .acc = 0xFF4D4F,
      .icon = APP_ICON_RADAR, .card_h = 96,
      .pcm = _binary_airbus_tcas_traffic_pcm_start,
      .pcm_end = _binary_airbus_tcas_traffic_pcm_end },
    { .name = "波音决断高度", .en = "MINIMUMS", .dur_ms = 2451, .acc = 0x9B7BFF,
      .icon = APP_ICON_ALT, .card_h = 96,
      .pcm = _binary_boeing_approaching_minimums_pcm_start,
      .pcm_end = _binary_boeing_approaching_minimums_pcm_end },
    { .name = "求救呼叫", .en = "MAYDAY", .dur_ms = 1642, .acc = 0xFF6BA8,
      .icon = APP_ICON_RADIO, .card_h = 72,
      .pcm = _binary_radio_mayday_call_pcm_start,
      .pcm_end = _binary_radio_mayday_call_pcm_end },
    { .name = "地形注意", .en = "TERRAIN ALERT", .dur_ms = 5042, .acc = 0xFF9A3D,
      .icon = APP_ICON_MTN, .card_h = 72,
      .pcm = _binary_gpws_terrain_alert_pcm_start,
      .pcm_end = _binary_gpws_terrain_alert_pcm_end },
    { .name = "客舱服务", .en = "DING DONG", .dur_ms = 2437, .acc = 0x35D0A5,
      .icon = APP_ICON_CHIME, .card_h = 96,
      .pcm = _binary_cabin_chime_ding_dong_pcm_start,
      .pcm_end = _binary_cabin_chime_ding_dong_pcm_end },
    { .name = "拉升警告", .en = "PULL UP", .dur_ms = 1200, .acc = 0xFF4D4F,
      .icon = APP_ICON_UP, .card_h = 96,
      .pcm = _binary_gpws_pull_up_pcm_start,
      .pcm_end = _binary_gpws_pull_up_pcm_end },
    { .name = "爬升指令", .en = "CLIMB", .dur_ms = 2200, .acc = 0x4D8CFF,
      .icon = APP_ICON_UP, .card_h = 96,
      .pcm = _binary_tcas_ra_climb_pcm_start,
      .pcm_end = _binary_tcas_ra_climb_pcm_end },
};

const app_alert_t *app_catalog(int index) {
    if (index < 0 || index >= app_catalog_count()) return NULL;
    return &ALERTS[index];
}

int app_catalog_count(void) {
    return (int)APP_CATALOG_COUNT;
}
