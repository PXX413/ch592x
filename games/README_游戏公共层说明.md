# CH592X 小游戏公共层（game_port）+ 中文字库 说明

本层是"地基"：**输入 / 计时 / 随机数 / 绘图 / 横竖屏**。
后续的游戏（2048、跑酷、雷霆战机）都建在这一层上。

---

## 一、文件清单

| 文件 | 说明 |
|------|------|
| `game_port.h` | 公共层接口：按键事件、计时、随机数、绘图、屏幕方向 |
| `game_port.c` | 公共层实现 |
| `game_font16.h` / `game_font16.c` | 16x16 中文字模（**脚本生成，不要手改**），96 个字 = 3072 字节 |
| `game_text.h` / `game_text.c` | 中文字符串表（**脚本生成**），57 条常用界面文字 |
| `font_gen.ps1` | PC 端字模生成脚本（宋体 16x16 -> 上面 4 个文件） |
| `font_texts.txt` | 字表源数据（`GTXT_xxx=中文` 一行一条），**加字只改这个** |
| `font_preview.png` | 字模预览图（自查用，一眼看出有没有缺笔画） |
| `font_report.txt` | 每个字的编号 / Unicode / 点数统计 |

依赖（都在工程 `hardware\` 里，已存在）：

```
game_port
   |-- key_matrix.c/h    3x3 矩阵键盘
   |-- encoder.c/h       带按键旋转编码器
   |-- tft_port / tft_st7789 / tft_gfx     ST7789 135x240 屏
   `-- game_font16 / game_text             16x16 中文字模
```

**没有引入任何新的外设驱动**，也不需要动工程设置（`games\` 目录会自动参与编译）。

---

## 二、五分钟上手

```c
#include "game_port.h"
#include "key_matrix.h"
#include "encoder.h"

int main(void)
{
    GameKeyEvent_t ev;

    SetSysClock(CLK_SOURCE_PLL_60MHz);

    Game_Init(GAME_LANDSCAPE);          /* 横屏 240x135；2048 用 GAME_PORTRAIT */

    while (1)
    {
        Game_Task();                    /* 唯一的一处节拍：扫键 + 1ms 延时 */

        while (Game_PollEvent(&ev))     /* 取出所有按键事件 */
        {
            if (ev.key == GKEY_ENC_CW)  { /* 编码器右转 */ }
            if (ev.key == GKEY_ENC_CCW) { /* 编码器左转 */ }
            if (ev.key == GKEY_UP)      { /* K5 */ }
            if (ev.type == GAME_EV_PRESS) { /* 刚按下 */ }
        }

        Game_DrawText(4, 4, GTXT_SCORE, ST77XX_WHITE, ST77XX_BLACK);
        Game_DrawNumberRight(130, 4, Game_Millis() / 1000u, 2, ST77XX_GREEN, ST77XX_BLACK);
    }
}
```

要点：

1. **`Game_Task()` 自己带 1ms 延时**，主循环里不要再写 `DelayMs()`，否则节拍会变。
2. `Game_PollEvent()` 一次取一个事件，循环取干净为止（队列 8 个，满了丢新事件，不会卡死）。
3. 中文只能用 `GTXT_xxx` 编号 + `Game_DrawText()`，**不要在代码里写中文字符串**（原因见第五章）。

---

## 三、按键表

矩阵键盘按 **WASD 手感**摆放：

```
     K1   K2   K3              上 = K5
     K4   K5   K6      ==>     左 = K4     右 = K6
     K7   K8   K9              下 = K8
```

| 宏 | 键 | 常用含义 |
|----|----|---------|
| `GKEY_UP` / `GKEY_DOWN` | K5 / K8 | 上 / 下 |
| `GKEY_LEFT` / `GKEY_RIGHT` | K4 / K6 | 左 / 右 |
| `GKEY_OK` | K5 | 确定 |
| `GKEY_BACK` | K9 | 返回 / 退出 |
| `GKEY_PAUSE` | K7 | 暂停 |
| `GKEY_ENC_CW` / `GKEY_ENC_CCW` | 编码器旋转 | 转一格一个事件 |
| `GKEY_ENC_BTN` | 编码器按键 | 确定 / 冲刺 / 放炸弹 |

也可以直接用物理编号 `GKEY_K1` ~ `GKEY_K9`。

**事件类型**（`GameKeyEvent_t`）：

| 类型 | 什么时候产生 |
|------|-------------|
| `GAME_EV_PRESS` | 刚按下（一次） |
| `GAME_EV_REPEAT` | 按住超过 `GAME_REPEAT_DELAY_MS`(450ms) 后，每 `GAME_REPEAT_PERIOD_MS`(90ms) 一次 |
| `GAME_EV_RELEASE` | 松开（一次） |

另外可以随时查询"现在是否按着 / 按住多久"：`Game_KeyDown(key)`、`Game_KeyHeldMs(key)`。
**"按住持续动作"（跑酷滑铲、雷霆战机上飞/下飞/连发）用 `Game_KeyDown()`，
它反映的是按键的实时状态，就算漏掉一个事件也不会卡住。**

---

## 四、接口速查

| 接口 | 说明 |
|------|------|
| `Game_Init(orient)` | 初始化键盘、编码器、屏，清屏，设初始方向 |
| `Game_Task()` | 主循环里反复调用：扫描 + 1ms 节拍 + 产生事件 |
| `Game_PollEvent(&ev)` | 取一个按键事件，返回 `false` 表示没有 |
| `Game_FlushEvents()` | 清空未处理事件（切场景时用） |
| `Game_KeyDown(k)` / `Game_KeyHeldMs(k)` | 当前是否按着 / 按住多久 |
| `Game_Millis()` | 毫秒计数（从 `Game_Init` 起） |
| `Game_SetOrientation(o)` / `Game_GetOrientation()` | 切横竖屏 |
| `Game_Width()` / `Game_Height()` | 当前方向下的宽 / 高（横屏 240x135，竖屏 135x240） |
| `Game_Srand(seed)` / `Game_Rand()` / `Game_RandRange(n)` | 随机数（xorshift32，`RandRange(n)` 返回 0~n-1） |
| `Game_Clear(color)` | 整屏清成某色 |
| `Game_DrawGlyph(x, y, glyph, fg, bg)` | 画一个 16x16 汉字 |
| `Game_DrawText(x, y, GTXT_xxx, fg, bg)` | 画一串中文 |
| `Game_DrawTextCenter(y, GTXT_xxx, fg, bg)` | 水平居中画中文 |
| `Game_TextWidth(GTXT_xxx)` | 这串中文的像素宽（用来排版/居中） |
| `Game_NumberWidth(num, scale)` | 数字占多宽（5x7 字库，每个数字 `6*scale` px） |
| `Game_DrawNumber(x, y, num, scale, fg, bg)` | 画放大数字（scale = 1~4） |
| `Game_DrawNumberRight(x_right, y, ...)` | 右对齐画数字（分数用） |
| `Game_DrawPanel(x, y, w, h, fill, border)` | 面板/格子底 |
| `Game_DrawHintBar(GTXT_xxx, fg, bg)` | 底部按键提示条（高 18px） |
| `Game_Sound(freq, ms)` | **预留音效接口，现在是空函数**（雷霆战机已经在调它了） |

> 小技巧：想"擦掉"一串中文，可以用 `Game_DrawText(x, y, GTXT_xxx, bg, bg)`
> —— `fg == bg` 时 `Game_DrawGlyph()` 会跳过底色填充，只把字模按 `bg` 色重画一遍，等效于擦除。

---

## 五、中文字库怎么用、怎么加字

### 为什么不直接在代码里写中文
工程源码是 **GBK**，而编译器默认按 UTF-8 解析字符串字面量，
两者不一致时中文会变成乱码甚至报 `failure to convert` 错误。
所以本方案把中文全部变成**点阵数据 + 编号**：

```c
Game_DrawText(4, 4, GTXT_SCORE, ST77XX_WHITE, ST77XX_BLACK);   /* 画"分数" */
```

`GTXT_xxx` -> 一串字模编号（`GF_U5206` = 分为 U+5206）-> 点阵数据。
这样**完全不受源码编码影响**，也不会因为编译器换了个编码就乱码。

### 加字 / 改字（重要，以后加界面文字都走这条路）
1. 编辑 `font_texts.txt`，加一行：`GTXT_MY_TEXT=我的文字`（GBK 编码保存）；
2. 在 PC 上跑一次：
   ```
   powershell -ExecutionPolicy Bypass -File font_gen.ps1
   ```
3. 它会重新生成 `game_font16.h/.c`、`game_text.h/.c`、`font_preview.png`、`font_report.txt`；
4. 把 `game_font16.*`、`game_text.*` 覆盖回工程的 `games\` 目录，工程里按 F5 刷新、重新编译；
5. 打开 `font_preview.png` 看一眼，确认新字没缺笔画。

> 字模格式：16 行 x 2 字节，**高位在左**（MSB first），每字 32 字节，放在 Flash（`const`）。
> 当前 96 个字 = 3072 字节；再加字按"每个新字 32 字节"估算。
>
> **删字**同样只改 `font_texts.txt` 然后重跑脚本，但要注意：删掉字之后整个字模表会重新编号，
> 所以**所有 `.c` 都要重新编译一次**（代码里写的是 `GTXT_xxx` 名字，不是数字，所以不用改代码）。

### 已经有哪些中文
游戏名（跑酷 / 雷霆战机 / 菜单）、状态（分数 / 最高分 / 等级 / 时间 / 速度 / 纪录 / 生命）、
流程（游戏结束 / 暂停 / 开始 / 重新开始 / 胜利 / 继续 / 退出 / 返回 / 选择 / 新纪录 / 按任意键）、
难度（难度 / 简单 / 普通 / 困难）、操作（上下左右 / 跳跃 / 滑铲 / 冲刺 / 加速 / 撤销 / 确定 / 取消 / 是 / 否 / 设置 / 音效 / 按键 / 开 / 关）、
蓝牙（蓝牙 / 设备 / 扫描中 / 连接 / 已连接 / 断开 / 未找到设备 / 配对 / 选择设备 / 键盘模式 / 游戏模式 / 宿主 / 设为宿主）。
共 57 条、96 个不重复的汉字，明细见 `font_report.txt`。

> 字数变化过程：最早 108 字 -> 删掉贪吃蛇/俄罗斯方块后 90 字 -> 加雷霆战机后 96 字
> （雷霆、战机、等级 这 6 个字是新加的）。

---

## 六、其他说明

* **屏幕方向**：`Game_SetOrientation()` 内部调 `TFT_SetRotation()` 并同步宽高变量，
  游戏代码只按 `Game_Width()/Game_Height()` 排版，不用管旋转细节。
  横屏（跑酷、雷霆战机）用 `GAME_LANDSCAPE`，2048 用 `GAME_PORTRAIT`。
* **随机数**：上电种子取自系统 tick，每次开机序列不同；`Game_Srand()` 可固定种子做调试。
* **音效**：`Game_Sound()` 现在是空函数，以后接无源蜂鸣器只改这一个函数，
  所有游戏代码一行都不用动。
* **性能**：本层每次只做"读 10 个键 + 少量队列操作"，1ms 一次毫无压力；
  绘图全部交给 `tft_gfx`（局部重绘，不做整屏重画）。
* **裁剪要自己来**：`TFT_FillRect()` 不做边界裁剪，往屏幕外画会真的发 SPI 数据。
  需要"对象可能跑出屏幕"的游戏（雷霆战机的敌机从右边飞进来）要自己写一个
  clamp 到可见区的小函数，`game_runner.c` / `game_thunder.c` 里都有现成的例子。

---

## 七、代码自检情况（PC 上跑逻辑验证，非真机）

用 PC gcc（`-std=gnu99 -Wall -Wextra -Wpedantic`）+ 桩件（假的键盘/编码器/屏，记录所有绘图调用）
跑了 **3 组配置 x 75 项 = 225 项检查，全部通过，零告警**：

| 配置 | 覆盖内容 |
|------|---------|
| A：默认 | 初始化与方向切换、1ms 节拍、按下/松开/长按连发、双击键独立、编码器正反转与按键、队列满丢事件、随机性、字表完整性、绘图坐标与居中、右对齐数字、提示条、音效空实现 |
| B：`REPEAT_DELAY=120 / PERIOD=40` | 连发参数按宏计算，换参数不用改代码 |
| C：`GAME_EVENT_QUEUE=4` | 队列变小时行为仍然正确 |

另外对字库做了**逐字检查**：96 个字全部非空、字模位数统计正常，
并用"上"字校验了点阵的位序（高位在左）没有搞反
—— 它是竖笔偏左 + 底部一长横，如果每个字节内部按 LSB 先解，竖笔会跑到第 9 列去，一测就知道。

---

## 八、状态

公共层、菜单、纪录存储和三个游戏（2048 / 跑酷 / 雷霆战机）都已完成并通过 PC 自测，
明细和通过项数见 `README_小游戏说明.md`。

> 俄罗斯方块和贪吃蛇已按需求删除，公共层本身没受影响（它们只是这一层的使用者）。
