/**************************************************************************
 * game_text.h - 游戏中文字符串表（由 font_gen.ps1 自动生成，不要手改）
 *
 *   代码里不要出现中文字符串字面量（工程是 GBK，编译器按 UTF-8 解会乱），
 *   统一用 GameTextId_t 这个编号 + Game_DrawText() 来显示中文。
 **************************************************************************/

#ifndef __GAME_TEXT_H__
#define __GAME_TEXT_H__

#include <stdint.h>

typedef enum
{
    GTXT_NONE = 0,
    GTXT_RUNNER,        /* 跑酷 */
    GTXT_THUNDER,        /* 雷霆战机 */
    GTXT_MENU,        /* 菜单 */
    GTXT_SELECT_GAME,        /* 选择游戏 */
    GTXT_DIFFICULTY,        /* 难度 */
    GTXT_EASY,        /* 简单 */
    GTXT_NORMAL,        /* 普通 */
    GTXT_HARD,        /* 困难 */
    GTXT_SCORE,        /* 分数 */
    GTXT_BEST,        /* 最高分 */
    GTXT_LEVEL,        /* 等级 */
    GTXT_TIME,        /* 时间 */
    GTXT_SPEED,        /* 速度 */
    GTXT_RECORD,        /* 纪录 */
    GTXT_LIFE,        /* 生命 */
    GTXT_GAME_OVER,        /* 游戏结束 */
    GTXT_PAUSE,        /* 暂停 */
    GTXT_START,        /* 开始 */
    GTXT_RESTART,        /* 重新开始 */
    GTXT_WIN,        /* 胜利 */
    GTXT_CONTINUE,        /* 继续 */
    GTXT_EXIT,        /* 退出 */
    GTXT_BACK,        /* 返回 */
    GTXT_SELECT,        /* 选择 */
    GTXT_NEW_RECORD,        /* 新纪录 */
    GTXT_ANY_KEY,        /* 按任意键 */
    GTXT_UP,        /* 上 */
    GTXT_DOWN,        /* 下 */
    GTXT_LEFT,        /* 左 */
    GTXT_RIGHT,        /* 右 */
    GTXT_JUMP,        /* 跳跃 */
    GTXT_SLIDE,        /* 滑铲 */
    GTXT_SPRINT,        /* 冲刺 */
    GTXT_ACCEL,        /* 加速 */
    GTXT_UNDO,        /* 撤销 */
    GTXT_CONFIRM,        /* 确定 */
    GTXT_CANCEL,        /* 取消 */
    GTXT_YES,        /* 是 */
    GTXT_NO,        /* 否 */
    GTXT_SETTINGS,        /* 设置 */
    GTXT_SOUND,        /* 音效 */
    GTXT_KEYS,        /* 按键 */
    GTXT_ON,        /* 开 */
    GTXT_OFF,        /* 关 */
    GTXT_BT,        /* 蓝牙 */
    GTXT_BT_DEVICES,        /* 蓝牙设备 */
    GTXT_BT_SCAN,        /* 扫描中 */
    GTXT_BT_CONNECT,        /* 连接 */
    GTXT_BT_CONNECTED,        /* 已连接 */
    GTXT_BT_DISCONNECT,        /* 断开 */
    GTXT_BT_NODEV,        /* 未找到设备 */
    GTXT_BT_PAIR,        /* 配对 */
    GTXT_BT_SELECT,        /* 选择设备 */
    GTXT_KB_MODE,        /* 键盘模式 */
    GTXT_GAME_MODE,        /* 游戏模式 */
    GTXT_BT_HOST,        /* 宿主 */
    GTXT_BT_SET_HOST,        /* 设为宿主 */
    GTXT_RC_MODE,        /* 遥控模式 */
    GTXT_BRIGHT,        /* 键盘灯亮度 */
    GTXT_SLEEP,        /* 休眠时间 */
    GTXT_RESET,        /* 恢复默认 */
    GTXT_CONNECTED,        /* 已连接 */
    GTXT_DISCONNECTED,        /* 未连接 */
    GTXT_KEYEDIT,        /* 键值编辑 */
    GTXT_WIP,        /* 开发中 */
    GTXT_KBD_LED,        /* 键盘灯开关 */
    GTXT_MIN,        /* 分钟 */
    GTXT_NEVER,        /* 永不 */
    GTXT_LEDFX,        /* 灯效 */
    GTXT_FX_BREATH,        /* 呼吸 */
    GTXT_FX_CHASE,        /* 跑马 */
    GTXT_FX_RIPPLE,        /* 涟漪 */
    GTXT_FX_WAVE,        /* 波浪 */
    GTXT_FX_RAINBOW,        /* 彩虹 */
    GTXT_KEY_HINT,        /* 按键提示 */
    GTXT_SCRBRIGHT,        /* 屏幕亮度 */
    GTXT_COUNT
} GameTextId_t;

/* 每个文本：字模编号序列 + 字数 */
extern const uint8_t *const game_text_ptr[GTXT_COUNT];
extern const uint8_t        game_text_len[GTXT_COUNT];

#endif /* __GAME_TEXT_H__ */
