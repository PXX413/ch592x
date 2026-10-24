/* icon_mode_60x60.h —— 60x60 RGB565 圆角 UI 图标集 */
#ifndef ICON_MODE_60X60_H
#define ICON_MODE_60X60_H

#include <stdint.h>

#define ICON_SIZE_W      60u
#define ICON_SIZE_H      60u
#define ICON_PIXELS      3600u
#define ICON_COLOR_KEY   0xFFFFu   /* 圆角外透明键（白色） */

extern const uint16_t keyboard_mode_60x60[ICON_PIXELS];
extern const uint16_t settings_mode_60x60[ICON_PIXELS];
extern const uint16_t game_mode_60x60[ICON_PIXELS];
extern const uint16_t remote_mode_60x60[ICON_PIXELS];

/* 便于索引：与 .c 中定义顺序一致 */
typedef enum {
    ICON_IDX_KEYBOARD_MODE = 0,
    ICON_IDX_SETTINGS_MODE = 1,
    ICON_IDX_GAME_MODE = 2,
    ICON_IDX_REMOTE_MODE = 3,
    ICON_IDX_COUNT
} icon_index_t;

extern const uint16_t *const icon_mode_table[ICON_IDX_COUNT];

/* 可选：定义 ICON_USE_LVGL_DESC 后在 .c 中启用 LVGL v9 图像描述符 */
#ifdef ICON_USE_LVGL_DESC
struct _lv_image_dsc_t;
extern const struct _lv_image_dsc_t *const icon_mode_lvgl_table[ICON_IDX_COUNT];
#endif

#endif /* ICON_MODE_60X60_H */