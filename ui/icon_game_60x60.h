/* icon_game_60x60.h —— 60x60 RGB565 圆角 UI 游戏图标集 */
#ifndef ICON_GAME_60X60_H
#define ICON_GAME_60X60_H

#include <stdint.h>

#define GICON_SIZE_W      60u
#define GICON_SIZE_H      60u
#define GICON_PIXELS      3600u
#define GICON_COLOR_KEY   0xFFFFu   /* 圆角外透明键（白色） */

extern const uint16_t game_2048_60x60[GICON_PIXELS];
extern const uint16_t game_parkour_60x60[GICON_PIXELS];
extern const uint16_t game_jet_60x60[GICON_PIXELS];

/* 便于索引：与 .c 中定义顺序一致 */
typedef enum {
    GICON_IDX_GAME_2048 = 0,
    GICON_IDX_GAME_PARKOUR = 1,
    GICON_IDX_GAME_JET = 2,
    GICON_IDX_COUNT
} gicon_index_t;

extern const uint16_t *const icon_game_table[GICON_IDX_COUNT];

/* 可选：定义 GICON_USE_LVGL_DESC 后在 .c 中启用 LVGL v9 图像描述符 */
#ifdef GICON_USE_LVGL_DESC
struct _lv_image_dsc_t;
extern const struct _lv_image_dsc_t *const icon_game_lvgl_table[GICON_IDX_COUNT];
#endif

#endif /* ICON_GAME_60X60_H */