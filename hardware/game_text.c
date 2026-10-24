/**************************************************************************
 * game_text.c - 游戏中文字符串表数据（由 font_gen.ps1 自动生成，不要手改）
 **************************************************************************/

#include "game_text.h"
#include "game_font16.h"

static const uint8_t s_text_0[2] = { GF_U8DD1, GF_U9177,};   /* 跑酷 */
static const uint8_t s_text_1[4] = { GF_U96F7, GF_U9706, GF_U6218, GF_U673A,};   /* 雷霆战机 */
static const uint8_t s_text_2[2] = { GF_U83DC, GF_U5355,};   /* 菜单 */
static const uint8_t s_text_3[4] = { GF_U9009, GF_U62E9, GF_U6E38, GF_U620F,};   /* 选择游戏 */
static const uint8_t s_text_4[2] = { GF_U96BE, GF_U5EA6,};   /* 难度 */
static const uint8_t s_text_5[2] = { GF_U7B80, GF_U5355,};   /* 简单 */
static const uint8_t s_text_6[2] = { GF_U666E, GF_U901A,};   /* 普通 */
static const uint8_t s_text_7[2] = { GF_U56F0, GF_U96BE,};   /* 困难 */
static const uint8_t s_text_8[2] = { GF_U5206, GF_U6570,};   /* 分数 */
static const uint8_t s_text_9[3] = { GF_U6700, GF_U9AD8, GF_U5206,};   /* 最高分 */
static const uint8_t s_text_10[2] = { GF_U7B49, GF_U7EA7,};   /* 等级 */
static const uint8_t s_text_11[2] = { GF_U65F6, GF_U95F4,};   /* 时间 */
static const uint8_t s_text_12[2] = { GF_U901F, GF_U5EA6,};   /* 速度 */
static const uint8_t s_text_13[2] = { GF_U7EAA, GF_U5F55,};   /* 纪录 */
static const uint8_t s_text_14[2] = { GF_U751F, GF_U547D,};   /* 生命 */
static const uint8_t s_text_15[4] = { GF_U6E38, GF_U620F, GF_U7ED3, GF_U675F,};   /* 游戏结束 */
static const uint8_t s_text_16[2] = { GF_U6682, GF_U505C,};   /* 暂停 */
static const uint8_t s_text_17[2] = { GF_U5F00, GF_U59CB,};   /* 开始 */
static const uint8_t s_text_18[4] = { GF_U91CD, GF_U65B0, GF_U5F00, GF_U59CB,};   /* 重新开始 */
static const uint8_t s_text_19[2] = { GF_U80DC, GF_U5229,};   /* 胜利 */
static const uint8_t s_text_20[2] = { GF_U7EE7, GF_U7EED,};   /* 继续 */
static const uint8_t s_text_21[2] = { GF_U9000, GF_U51FA,};   /* 退出 */
static const uint8_t s_text_22[2] = { GF_U8FD4, GF_U56DE,};   /* 返回 */
static const uint8_t s_text_23[2] = { GF_U9009, GF_U62E9,};   /* 选择 */
static const uint8_t s_text_24[3] = { GF_U65B0, GF_U7EAA, GF_U5F55,};   /* 新纪录 */
static const uint8_t s_text_25[4] = { GF_U6309, GF_U4EFB, GF_U610F, GF_U952E,};   /* 按任意键 */
static const uint8_t s_text_26[1] = { GF_U4E0A,};   /* 上 */
static const uint8_t s_text_27[1] = { GF_U4E0B,};   /* 下 */
static const uint8_t s_text_28[1] = { GF_U5DE6,};   /* 左 */
static const uint8_t s_text_29[1] = { GF_U53F3,};   /* 右 */
static const uint8_t s_text_30[2] = { GF_U8DF3, GF_U8DC3,};   /* 跳跃 */
static const uint8_t s_text_31[2] = { GF_U6ED1, GF_U94F2,};   /* 滑铲 */
static const uint8_t s_text_32[2] = { GF_U51B2, GF_U523A,};   /* 冲刺 */
static const uint8_t s_text_33[2] = { GF_U52A0, GF_U901F,};   /* 加速 */
static const uint8_t s_text_34[2] = { GF_U64A4, GF_U9500,};   /* 撤销 */
static const uint8_t s_text_35[2] = { GF_U786E, GF_U5B9A,};   /* 确定 */
static const uint8_t s_text_36[2] = { GF_U53D6, GF_U6D88,};   /* 取消 */
static const uint8_t s_text_37[1] = { GF_U662F,};   /* 是 */
static const uint8_t s_text_38[1] = { GF_U5426,};   /* 否 */
static const uint8_t s_text_39[2] = { GF_U8BBE, GF_U7F6E,};   /* 设置 */
static const uint8_t s_text_40[2] = { GF_U97F3, GF_U6548,};   /* 音效 */
static const uint8_t s_text_41[2] = { GF_U6309, GF_U952E,};   /* 按键 */
static const uint8_t s_text_42[1] = { GF_U5F00,};   /* 开 */
static const uint8_t s_text_43[1] = { GF_U5173,};   /* 关 */
static const uint8_t s_text_44[2] = { GF_U84DD, GF_U7259,};   /* 蓝牙 */
static const uint8_t s_text_45[4] = { GF_U84DD, GF_U7259, GF_U8BBE, GF_U5907,};   /* 蓝牙设备 */
static const uint8_t s_text_46[3] = { GF_U626B, GF_U63CF, GF_U4E2D,};   /* 扫描中 */
static const uint8_t s_text_47[2] = { GF_U8FDE, GF_U63A5,};   /* 连接 */
static const uint8_t s_text_48[3] = { GF_U5DF2, GF_U8FDE, GF_U63A5,};   /* 已连接 */
static const uint8_t s_text_49[2] = { GF_U65AD, GF_U5F00,};   /* 断开 */
static const uint8_t s_text_50[5] = { GF_U672A, GF_U627E, GF_U5230, GF_U8BBE, GF_U5907,};   /* 未找到设备 */
static const uint8_t s_text_51[2] = { GF_U914D, GF_U5BF9,};   /* 配对 */
static const uint8_t s_text_52[4] = { GF_U9009, GF_U62E9, GF_U8BBE, GF_U5907,};   /* 选择设备 */
static const uint8_t s_text_53[4] = { GF_U952E, GF_U76D8, GF_U6A21, GF_U5F0F,};   /* 键盘模式 */
static const uint8_t s_text_54[4] = { GF_U6E38, GF_U620F, GF_U6A21, GF_U5F0F,};   /* 游戏模式 */
static const uint8_t s_text_55[2] = { GF_U5BBF, GF_U4E3B,};   /* 宿主 */
static const uint8_t s_text_56[4] = { GF_U8BBE, GF_U4E3A, GF_U5BBF, GF_U4E3B,};   /* 设为宿主 */
static const uint8_t s_text_57[4] = { GF_U9065, GF_U63A7, GF_U6A21, GF_U5F0F,};   /* 遥控模式 */
static const uint8_t s_text_58[5] = { GF_U952E, GF_U76D8, GF_U706F, GF_U4EAE, GF_U5EA6,};   /* 键盘灯亮度 */
static const uint8_t s_text_59[4] = { GF_U4F11, GF_U7720, GF_U65F6, GF_U95F4,};   /* 休眠时间 */
static const uint8_t s_text_60[4] = { GF_U6062, GF_U590D, GF_U9ED8, GF_U8BA4,};   /* 恢复默认 */
static const uint8_t s_text_61[3] = { GF_U5DF2, GF_U8FDE, GF_U63A5,};   /* 已连接 */
static const uint8_t s_text_62[3] = { GF_U672A, GF_U8FDE, GF_U63A5,};   /* 未连接 */
static const uint8_t s_text_63[4] = { GF_U952E, GF_U503C, GF_U7F16, GF_U8F91,};   /* 键值编辑 */
static const uint8_t s_text_64[3] = { GF_U5F00, GF_U53D1, GF_U4E2D,};   /* 开发中 */
static const uint8_t s_text_65[5] = { GF_U952E, GF_U76D8, GF_U706F, GF_U5F00, GF_U5173,};   /* 键盘灯开关 */
static const uint8_t s_text_66[2] = { GF_U5206, GF_U949F,};   /* 分钟 */
static const uint8_t s_text_67[2] = { GF_U6C38, GF_U4E0D,};   /* 永不 */
static const uint8_t s_text_68[2] = { GF_U706F, GF_U6548,};   /* 灯效 */
static const uint8_t s_text_69[2] = { GF_U547C, GF_U5438,};   /* 呼吸 */
static const uint8_t s_text_70[2] = { GF_U8DD1, GF_U9A6C,};   /* 跑马 */
static const uint8_t s_text_71[2] = { GF_U6D9F, GF_U6F2A,};   /* 涟漪 */
static const uint8_t s_text_72[2] = { GF_U6CE2, GF_U6D6A,};   /* 波浪 */
static const uint8_t s_text_73[2] = { GF_U5F69, GF_U8679,};   /* 彩虹 */
static const uint8_t s_text_74[4] = { GF_U6309, GF_U952E, GF_U63D0, GF_U793A,};   /* 按键提示 */
static const uint8_t s_text_75[4] = { GF_U5C4F, GF_U5E55, GF_U4EAE, GF_U5EA6,};   /* 屏幕亮度 */

const uint8_t *const game_text_ptr[GTXT_COUNT] =
{
    0,                                  /* GTXT_NONE */
    s_text_0,                          /* GTXT_RUNNER */
    s_text_1,                          /* GTXT_THUNDER */
    s_text_2,                          /* GTXT_MENU */
    s_text_3,                          /* GTXT_SELECT_GAME */
    s_text_4,                          /* GTXT_DIFFICULTY */
    s_text_5,                          /* GTXT_EASY */
    s_text_6,                          /* GTXT_NORMAL */
    s_text_7,                          /* GTXT_HARD */
    s_text_8,                          /* GTXT_SCORE */
    s_text_9,                          /* GTXT_BEST */
    s_text_10,                          /* GTXT_LEVEL */
    s_text_11,                          /* GTXT_TIME */
    s_text_12,                          /* GTXT_SPEED */
    s_text_13,                          /* GTXT_RECORD */
    s_text_14,                          /* GTXT_LIFE */
    s_text_15,                          /* GTXT_GAME_OVER */
    s_text_16,                          /* GTXT_PAUSE */
    s_text_17,                          /* GTXT_START */
    s_text_18,                          /* GTXT_RESTART */
    s_text_19,                          /* GTXT_WIN */
    s_text_20,                          /* GTXT_CONTINUE */
    s_text_21,                          /* GTXT_EXIT */
    s_text_22,                          /* GTXT_BACK */
    s_text_23,                          /* GTXT_SELECT */
    s_text_24,                          /* GTXT_NEW_RECORD */
    s_text_25,                          /* GTXT_ANY_KEY */
    s_text_26,                          /* GTXT_UP */
    s_text_27,                          /* GTXT_DOWN */
    s_text_28,                          /* GTXT_LEFT */
    s_text_29,                          /* GTXT_RIGHT */
    s_text_30,                          /* GTXT_JUMP */
    s_text_31,                          /* GTXT_SLIDE */
    s_text_32,                          /* GTXT_SPRINT */
    s_text_33,                          /* GTXT_ACCEL */
    s_text_34,                          /* GTXT_UNDO */
    s_text_35,                          /* GTXT_CONFIRM */
    s_text_36,                          /* GTXT_CANCEL */
    s_text_37,                          /* GTXT_YES */
    s_text_38,                          /* GTXT_NO */
    s_text_39,                          /* GTXT_SETTINGS */
    s_text_40,                          /* GTXT_SOUND */
    s_text_41,                          /* GTXT_KEYS */
    s_text_42,                          /* GTXT_ON */
    s_text_43,                          /* GTXT_OFF */
    s_text_44,                          /* GTXT_BT */
    s_text_45,                          /* GTXT_BT_DEVICES */
    s_text_46,                          /* GTXT_BT_SCAN */
    s_text_47,                          /* GTXT_BT_CONNECT */
    s_text_48,                          /* GTXT_BT_CONNECTED */
    s_text_49,                          /* GTXT_BT_DISCONNECT */
    s_text_50,                          /* GTXT_BT_NODEV */
    s_text_51,                          /* GTXT_BT_PAIR */
    s_text_52,                          /* GTXT_BT_SELECT */
    s_text_53,                          /* GTXT_KB_MODE */
    s_text_54,                          /* GTXT_GAME_MODE */
    s_text_55,                          /* GTXT_BT_HOST */
    s_text_56,                          /* GTXT_BT_SET_HOST */
    s_text_57,                          /* GTXT_RC_MODE */
    s_text_58,                          /* GTXT_BRIGHT */
    s_text_59,                          /* GTXT_SLEEP */
    s_text_60,                          /* GTXT_RESET */
    s_text_61,                          /* GTXT_CONNECTED */
    s_text_62,                          /* GTXT_DISCONNECTED */
    s_text_63,                          /* GTXT_KEYEDIT */
    s_text_64,                          /* GTXT_WIP */
    s_text_65,                          /* GTXT_KBD_LED */
    s_text_66,                          /* GTXT_MIN */
    s_text_67,                          /* GTXT_NEVER */
    s_text_68,                          /* GTXT_LEDFX */
    s_text_69,                          /* GTXT_FX_BREATH */
    s_text_70,                          /* GTXT_FX_CHASE */
    s_text_71,                          /* GTXT_FX_RIPPLE */
    s_text_72,                          /* GTXT_FX_WAVE */
    s_text_73,                          /* GTXT_FX_RAINBOW */
    s_text_74,                          /* GTXT_KEY_HINT */
    s_text_75,                          /* GTXT_SCRBRIGHT */
};

const uint8_t game_text_len[GTXT_COUNT] =
{
    0,                                  /* GTXT_NONE */
    2,                                  /* GTXT_RUNNER */
    4,                                  /* GTXT_THUNDER */
    2,                                  /* GTXT_MENU */
    4,                                  /* GTXT_SELECT_GAME */
    2,                                  /* GTXT_DIFFICULTY */
    2,                                  /* GTXT_EASY */
    2,                                  /* GTXT_NORMAL */
    2,                                  /* GTXT_HARD */
    2,                                  /* GTXT_SCORE */
    3,                                  /* GTXT_BEST */
    2,                                  /* GTXT_LEVEL */
    2,                                  /* GTXT_TIME */
    2,                                  /* GTXT_SPEED */
    2,                                  /* GTXT_RECORD */
    2,                                  /* GTXT_LIFE */
    4,                                  /* GTXT_GAME_OVER */
    2,                                  /* GTXT_PAUSE */
    2,                                  /* GTXT_START */
    4,                                  /* GTXT_RESTART */
    2,                                  /* GTXT_WIN */
    2,                                  /* GTXT_CONTINUE */
    2,                                  /* GTXT_EXIT */
    2,                                  /* GTXT_BACK */
    2,                                  /* GTXT_SELECT */
    3,                                  /* GTXT_NEW_RECORD */
    4,                                  /* GTXT_ANY_KEY */
    1,                                  /* GTXT_UP */
    1,                                  /* GTXT_DOWN */
    1,                                  /* GTXT_LEFT */
    1,                                  /* GTXT_RIGHT */
    2,                                  /* GTXT_JUMP */
    2,                                  /* GTXT_SLIDE */
    2,                                  /* GTXT_SPRINT */
    2,                                  /* GTXT_ACCEL */
    2,                                  /* GTXT_UNDO */
    2,                                  /* GTXT_CONFIRM */
    2,                                  /* GTXT_CANCEL */
    1,                                  /* GTXT_YES */
    1,                                  /* GTXT_NO */
    2,                                  /* GTXT_SETTINGS */
    2,                                  /* GTXT_SOUND */
    2,                                  /* GTXT_KEYS */
    1,                                  /* GTXT_ON */
    1,                                  /* GTXT_OFF */
    2,                                  /* GTXT_BT */
    4,                                  /* GTXT_BT_DEVICES */
    3,                                  /* GTXT_BT_SCAN */
    2,                                  /* GTXT_BT_CONNECT */
    3,                                  /* GTXT_BT_CONNECTED */
    2,                                  /* GTXT_BT_DISCONNECT */
    5,                                  /* GTXT_BT_NODEV */
    2,                                  /* GTXT_BT_PAIR */
    4,                                  /* GTXT_BT_SELECT */
    4,                                  /* GTXT_KB_MODE */
    4,                                  /* GTXT_GAME_MODE */
    2,                                  /* GTXT_BT_HOST */
    4,                                  /* GTXT_BT_SET_HOST */
    4,                                  /* GTXT_RC_MODE */
    5,                                  /* GTXT_BRIGHT */
    4,                                  /* GTXT_SLEEP */
    4,                                  /* GTXT_RESET */
    3,                                  /* GTXT_CONNECTED */
    3,                                  /* GTXT_DISCONNECTED */
    4,                                  /* GTXT_KEYEDIT */
    3,                                  /* GTXT_WIP */
    5,                                  /* GTXT_KBD_LED */
    2,                                  /* GTXT_MIN */
    2,                                  /* GTXT_NEVER */
    2,                                  /* GTXT_LEDFX */
    2,                                  /* GTXT_FX_BREATH */
    2,                                  /* GTXT_FX_CHASE */
    2,                                  /* GTXT_FX_RIPPLE */
    2,                                  /* GTXT_FX_WAVE */
    2,                                  /* GTXT_FX_RAINBOW */
    4,                                  /* GTXT_KEY_HINT */
    4,                                  /* GTXT_SCRBRIGHT */
};

