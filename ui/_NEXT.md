# 接手续做说明（第 3、4 步）

> 这个文件是给"下一轮接手的人/会话"看的。**先读完再动手，别重复调研。**

## 一、当前状态（已核实）

四套 PC 自测**每轮 1063 项、UTF-8 与 GBK 两轮全绿、零告警**：

| 套件 | 每轮项数 |
|---|---|
| games | 499 |
| keyboard | 183 |
| rf24 | 308 |
| ui | 73 |

### 已完成

| 内容 | 位置 |
|---|---|
| 编码器分档（单击/双击/长按/>3s） | `CH592X_ui\ui_enc.c/.h` |
| 页面栈 + 路由 + 全局规则 | `CH592X_ui\ui_page.c/.h` |
| 列表控件（23px 项高、2px 黄框固定居中、文字滚动、循环、240/190 双宽度） | `CH592X_ui\ui_list.c/.h` |
| 电量条 / 1px 分区线 / 3×3 井字格 / 数值控件 | `CH592X_ui\ui_widget.c/.h` |
| 设置持久化（数据 Flash **0x0300**，16B 记录 + magic + CRC16） | `CH592X_ui\ui_store.c/.h` |
| 通信仲裁 USB 0.5s → 2.4G 1.5s → BLE 无限 | `CH592X_ui\ui_transport.c/.h` |
| `GKEY_ENC_CLICK/DBL/HOLD/HOLD3`（13~16，`GKEY_COUNT` 17） | `CH592X_games\game_port.h` |
| 三款游戏按键已按新文档重映射（含 2048 补上暂停、修掉"松开也触发移动"） | `CH592X_games\game_*.c` |
| 字库挪到共享层（`game_font16/game_text/font_texts.txt/font_gen.ps1`） | `CH592X_hardware\` |

### 未完成（**本轮要做的**）

1. **前置**：字库缺词条 —— 遥控模式、亮度、休眠时间、恢复默认、已连接、未连接、键值编辑 等
2. **10 个页面模块**（`ui_page.h` 里已声明 10 个单例，实现数 = 0）
3. **`CH592X_User\Main.c`** 重写（现在还是旧的键盘+计数器 demo）
4. 字库新词条 + `README_UI说明.md`
5. GBK 转码 + 同步 MRS 工程

## 二、动手前的三个前置

### 1. 字库缺词条（不做页面编译不过）

改 `CH592X_hardware\font_texts.txt` → 加 `GTXT_xxx` → 在该目录跑：
```
powershell -NoProfile -ExecutionPolicy Bypass -File font_gen.ps1
```
→ **打开 `font_preview.png` 确认没有缺笔画**（宋体 16x16，个别字可能缺横）。

### 2. `CH592X_ui\` 整个目录现在是 UTF-8，其余是 GBK
**故意留到最后一步统一转**，别中途转 —— 新写的文件还是 UTF-8，最后一起转才一致。

### 3. 工作区已改、MRS 工程**未同步**
三款游戏的键位在工程里还是旧的。同步放在最后做（避免每轮同步一次）。

## 三、要写的页面

```
主页面 ─┬─ 键盘页 ──(编码器长按1s)──→ 键盘设置页 ─→ 键值编辑页
        │                                      ├─→ RGB亮度页
        ├─ 游戏页 ─→ 2048 / 跑酷 / 雷霆战机
        ├─ 遥控页 ─→ 设备列表 ─→ 9键功能页
        └─ 设置页 ─┬─ 键盘灯开关
                   ├─ 亮度调节（步进 5，范围 0~100，全灯白光）
                   ├─ 休眠时间（5/10/20/60min / 永不）
                   └─ 恢复默认（是/否 确认）
```

### 硬性布局数字（照抄《键盘功能实现.txt》，别自己发挥）

| 位置 | 规格 |
|---|---|
| 列表项 | **23px 高**；主页面/键盘设置页宽 **240px**，键值编辑页 **190px**；**16px 字**；**2px 黄色圆角框固定居中不动**；文字滚动；可循环 |
| 顶部电量条 | 固定 **20px** 高，开机后每 **5 分钟**刷新 |
| 键盘页 | 左 **189px** 显示当前按下的按键；右下 **50×50** 井字格（按下变绿、支持多键）；右上 **50×50** 显示 USB/2.4G/蓝牙；右中 **50×33** 居中"已连接/未连接"；**四区域 1px 白线分割** |
| 键值编辑页 | 左 **50px**（下=井字格、上=当前按键名如 "key1"）；右=HID 键值名称列表；**三部分 1px 黄线分割** |
| 亮度页 | 中心显示当前亮度值 |

### `Main.c`
初始化：时钟 → 屏 → 键盘矩阵 → 编码器（1ms 定时器里 `ENC_Scan`）→ 电池 → RGB → UI。
主循环：1ms 节拍驱动 UI。
**不要**照抄 `CH592X_games\main_example.txt` 那套游戏机主循环 —— 现在是 UI 驱动，游戏只是子页面。

## 四、现成能直接用（别重写）

| 需要 | 在哪 |
|---|---|
| 圆角黄框 | `tft_gfx` 的 `DrawRoundRect` / `FillRoundRect` |
| 16px 中文 | `CH592X_hardware\game_font16.*` + `game_text.*`（`GTXT_xxx` + `Game_DrawText()`） |
| 3×3 井字格按键 | `key_matrix` 的 `KEY_Scan` / `KEY_IsPressed` / `KEY_GetEvent` |
| 3 套默认 + 2 套自定义键值映射 | `key_matrix` 的 `KEY_SetMapGroup` / `KEY_GetMapped` / `KEY_SetMapValue` / `KEY_GetMapGroupName` |
| RGB 亮度 | `rgb_led` |
| 电量 | `battery` 的 `BAT_ReadPercent()` |
| 列表页范例 | `CH592X_bluetooth\bt_screen.c` |
| 三种通道 | `CH592X_keyboard\usb_kbd.*`、`CH592X_rf24\proto_rc.*`、`CH592X_bluetooth\bt_*` |
| 输入抽象 | `CH592X_games\game_port.*`（按键事件队列 + 毫秒 + 绘图工具） |

## 五、已知的坑（三个子代理各踩过一次，别重复）

1. **改脚本别用行号 splice** —— 弄坏过 `run_tests.ps1` 两次（吞行、重复行）。多行文本用 `edit` 工具。
2. **PowerShell 逗号优先级**：`@('-I'+$p,'-I'+$q)` 会拼成一个字符串，写 `@("-I$p","-I$q")`。
3. **含中文的 here-string / `@"` 极易解析失败**（本 session 踩过 4 次），中文多行内容一律用 `edit` 工具写。
4. **`powershell` 脚本要 `-ExecutionPolicy Bypass -File`** 才能跑。
5. **LF/CRLF 混用**会让 `git diff`/脚本失效，注意统一。
6. `ENC_Scan()` 要在 1ms 定时器里调，别放主循环。

## 六、别弄坏的东西

`games` 499 / `keyboard` 183 / `rf24` 308 / `ui` 73（每轮）四套现在全绿。
每套的 `run_tests.ps1` 都会跑两轮（UTF-8 副本 + GBK 直编），要求**零告警**（`-std=gnu99 -Wall -Wextra -Wpedantic`）。

**别 malloc**，整机 RAM 只剩约 9KB（当前 17488 / 26624 B）。
