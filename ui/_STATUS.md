# CH592X_ui 进度说明（交接用，不是最终 README）

> 最终的用户文档是 `README_UI说明.md`（还没写，属第 5 步）。
> 这份文件只记**做到哪了、下一步接着做什么**，做完可以删。

---

## 一、已完成

### 起点模块（本轮之前就有，完整自洽，别重写）

| 文件 | 内容 | 测试 |
|---|---|---|
| `ui_enc.h/.c` | 编码器点击分类器：单击 / 双击 / 长按(0.5s) / 超长按(3s) | `test_ui_enc.c` 36 项 |
| `ui_page.h/.c` | 页面栈 + 路由 + `ui_page_t` 基类 + `UI_PostKey()` | 无独立测试 |
| `ui_list.h/.c` | 列表控件（23px 项高、可循环、文字滚动、黄框固定居中） | 无独立测试 |
| `ui_widget.h/.c` | 电量条 / 1px 分区线 / 3×3 井字格 | 无独立测试 |

### 本轮做的

| 文件 | 内容 | 测试 |
|---|---|---|
| `ui_store.h/.c` | 设置持久化到数据 Flash **0x0300**（magic + CRC16，16 字节记录） | `test_store.c` 54 项 |
| `ui_transport.h/.c` | 通信仲裁状态机 **USB 0.5s → 2.4G 1.5s（其中 1s 内要对上码）→ BLE 无限等** | `test_transport.c` 56 项 |
| `tests\run_tests.ps1` | 两轮跑法（UTF-8 副本 + 交付件直编） | — |
| `tests\stub\` | `CH59x_common.h` + `stub_flash.c`（数据 Flash 桩件） | — |

### 三款游戏按键重映射（文档「（三）游戏模式」）

| 游戏 | 新键位 |
|---|---|
| 2048 | 上=K2 下=K8 左=K4 右=K6；暂停=ENC_CLICK；退出=ENC_DBL；**撤销保留在 K7**（文档未定义）；结束后 K5/ENC_CLICK 重开 |
| 跑酷 | 跳=K5 滑铲=K8 冲刺=K7（新增，1.5 倍速）；暂停=ENC_CLICK；退出=ENC_DBL；重开=K5；**二段跳移到 K9**（文档未留键） |
| 雷霆战机 | 上=K1 下=K4 开火=K6 炸弹=K3；暂停=ENC_CLICK；退出=ENC_DBL；重开=K5 |

2048 原来**完全没有暂停功能**，本轮补上了 `s_paused` + `G2048_IsPaused()`。

---

## 二、没做（第 3~5 步）

### 第 3 步：10 个页面模块 —— **一个都还没写**

`ui_page.h` 已经声明了这 10 个单例，实现时**每个页面一个 .c**，导出同名 `const ui_page_t`：

```
UI_PAGE_HOME     主页面（4 项列表：键盘模式 / 游戏模式 / 遥控模式 / 设置）
UI_PAGE_KBD      键盘页（左 189px 按键显示 + 右下 50×50 井字格 + 右上 50×50 通道 + 右中 50×33 连接状态，1px 白线分区）
UI_PAGE_KBSET    键盘设置页（5 项：3 套默认映射 + 2 套自定义 + RGB 亮度）
UI_PAGE_KEYEDIT  键值编辑页（左 50px：上=按键名 下=井字格；右=190px HID 名称列表；1px 黄线分区）
UI_PAGE_GAME     游戏页（3 项：2048 / 跑酷 / 雷霆战机）
UI_PAGE_RC       遥控页（设备列表 → 选中后显示 9 键功能 → 按键发指令）
UI_PAGE_SETTING  设置页（4 项：灯开关 / 亮度 / 休眠时间 / 恢复默认）
UI_PAGE_BRIGHT   亮度页（中心显示当前值，步进 5，0~100，全灯白光）
UI_PAGE_SLEEP    休眠时间页（5/10/20/60min/永不）
UI_PAGE_RESET    恢复默认确认页（是/否）
```

页面怎么写：

```c
static ui_nav_t kbset_on_key(const GameKeyEvent_t *ev) { ... return UI_NAV_STAY / UI_NAV_POP / UI_NAV_HOME; }
static void     kbset_enter(void *arg) { ... }
static void     kbset_draw(void) { ... }
const ui_page_t UI_PAGE_KBSET = { "kbset", kbset_enter, 0, kbset_on_key, 0, kbset_draw };
```

跳转用 `UI_Push(&UI_PAGE_XXX, arg)` / `UI_Pop()` / `UI_GoHome()`。
绘图用 `game_port.h` 的 `Game_Clear / Game_DrawText(GTXT_xxx) / Game_DrawNumber / Game_DrawPanel` + `tft_gfx` 的 `DrawRoundRect/FillRect` + `ui_widget`/`ui_list`。

### 第 4 步：`CH592X_User\Main.c` —— 没动

现在还是旧的键盘+计数器 demo。要重写成页面机主循环：
时钟 → 屏 → 键盘矩阵 → 编码器（1ms 定时器里 `ENC_Scan`）→ 电池 → RGB → `UI_Init()` → 主循环 `UI_Task()`。
**不要**照抄 `games\main_example.txt` 那套游戏机主循环。

`UI_Task()` 需要的三个外部输入已经就绪：`ui_enc` 出编码器事件、`ui_store` 出设置、`ui_transport` 出通道。

### 第 5 步：字库 + README —— 没做

1. **中文词条**：`CH592X_hardware\font_texts.txt` 要加「键盘模式 / 游戏模式 / 遥控模式 / 设置 / 亮度 / 休眠时间 / 恢复默认 / 已连接 / 未连接 / 键盘设置 / 键值编辑」等，然后跑 `CH592X_hardware\font_gen.ps1`，**看 `font_preview.png` 确认无缺笔画**。
2. **编码**：`CH592X_ui\` 现在**全是 UTF-8**，工程整体是 GBK —— 收尾时要批量转 GBK（先写 UTF-8、测通后转，是本项目的既定流程）。
3. `README_UI说明.md`：页面树 / 每页布局坐标 / 编码器事件表 / 通信仲裁时序 / Flash 布局 / 资源占用 / 文档矛盾取舍。

---

## 三、踩过的坑（下一轮别再踩）

1. **`CH592X_games\*.c` 是 LF 换行**，`*.bat` 是 CRLF。按 CRLF 去 split 会得到一整块，函数定位会全失败。
2. **不要在 PowerShell 脚本里写中文再拿去做字符串匹配** —— .ps1 无 BOM 时 PowerShell 按 ANSI 读，中文会变成乱码，`Replace` 全部匹配不上。要么脚本纯 ASCII，要么把中文放进独立的数据文件用 UTF-8 读。
3. **`@(@(1,2,3))` 会被 PowerShell 拍平成 `@(1,2,3)`** —— 二维表用扁平字符串 `"file|line|from|to"` 再 split。
4. **别用行号 splice 改脚本文件**（上一轮就这么弄坏过 `run_tests.ps1`）。改 .c 的测试断言时可以按行号，但每行都要先校验再改，改完立刻重跑。
5. `@('-I'+$p, '-I'+$q)` 逗号优先级高于 `+`，会拼成一个字符串 —— 写 `@("-I$p","-I$q")`。

---

## 四、当前测试基线（改之前先跑一遍）

| 套件 | 每轮项数 | 位置 |
|---|---|---|
| games | 499 | `CH592X_games\tests\run_tests.ps1` |
| keyboard | 183 | `CH592X_keyboard\tests\run_tests.ps1` |
| rf24 | 308 | `CH592X_rf24\tests\run_tests.ps1` |
| ui | 73 | `CH592X_ui\tests\run_tests.ps1` |

**四套全绿 = 1063 项/轮。** 改游戏键位会动到 `test_2048 / test_runner / test_thunder` 的断言，那是预期的。

---

## 五、同步提醒

工作区这一轮的改动**还没同步进 MounRiver 工程**（按约定由父代理做）：
- `CH592X_games\` 三个游戏 + 三个测试改了键位
- `CH592X_ui\` 新增 4 个模块 + 测试
- 工程里 `keyboard\`、`ui\` 目录和 `.cproject` 的包含路径、`CH59x_usbdev.c` 的 exclude 都还没处理
