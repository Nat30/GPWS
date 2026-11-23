// main/scenes.h —— 场景连播静态表(纯数据,host 可测)。
//
// 一个场景 = 一条有序的目录条目序列,按序播完自动停(序列尾不循环,
// 不走 autoplay 接棒,与连播设置正交)。故事线见 spec §6.1。
#pragma once

#include <stdint.h>

#define APP_SCENE_COUNT 6  // 场景数(单一编译期来源)

// 序列元素是目录条目 idx(0..23,host 测试断言合法域)。
typedef struct {
    const char *name;       // 场景名(菜单行)
    const int8_t *seq;      // 有序条目序列
    int n;                  // 序列长度
} app_scene_t;

static const int8_t SCENE_TAKEOFF[]  = {21, 23, 0, 20};           // 关门铃→爬升→拉升→地形间隔
static const int8_t SCENE_APPROACH[] = {21, 11, 9, 13, 7, 12};    // 关门铃→报高→下沉率→减速→拉平→狗叫
static const int8_t SCENE_WINDSHEAR[] = {5, 16, 3, 0};            // 风切变→拉升→失速边缘→复飞
static const int8_t SCENE_DESCENT[]  = {10, 1, 19};               // 主警告→近地→求救
static const int8_t SCENE_BOEING[]   = {2, 18, 16};               // 告警合辑→决断高度→连续拉升
static const int8_t SCENE_AIRBUS[]   = {8, 17, 15, 11};           // AP 断开→冲突→失速→报高

static const app_scene_t APP_SCENES[APP_SCENE_COUNT] = {
    {"完整起飞",   SCENE_TAKEOFF,  4},
    {"进近着陆",   SCENE_APPROACH, 6},
    {"风切变逃脱", SCENE_WINDSHEAR, 4},
    {"紧急下降",   SCENE_DESCENT,  3},
    {"波音全警告", SCENE_BOEING,   3},
    {"空客全家桶", SCENE_AIRBUS,   4},
};
