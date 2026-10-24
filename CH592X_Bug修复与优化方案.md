# CH592X 复合键盘工程 — Bug 修复与优化实施方案

> **文档版本** v1.0 · 2026-10-07  
> **目标工程** `C:\Users\a\Desktop\ch592x` · WCH CH592X (RV32IMAC @ 60MHz) · MounRiver Studio  
> **比对基准** `CH592X引脚定义.txt`、`键盘功能实现.txt`（本次未修改这两个文件）  
> **代码基线** `obj/CH592X.elf` @ 2026-10-06 21:20

---

## 目录

- [0. 结论速览](#0-结论速览)
- [1. 资源基线实测数据](#1-资源基线实测数据)
- [2. 修复总路线](#2-修复总路线)
- [3. P0 致命问题修复](#3-p0-致命问题修复)
- [4. P1 逻辑错误修复](#4-p1-逻辑错误修复)
- [5. P2 健壮性修复](#5-p2-健壮性修复)
- [6. 架构级优化](#6-架构级优化)
- [7. 验证与回归清单](#7-验证与回归清单)
- [8. 附录：本次修正的两处误判](#8-附录本次修正的两处误判)

---

## 0. 结论速览

引脚定义与需求文档的映射 **23 项全部正确**，架构分层清晰、注释质量很高，`battery.h` / `key_matrix.h` 的编译期强校验尤其扎实。

真正的风险 **不在引脚，而在架构层**，共 6 个 P0：

| 编号   | 问题                       | 后果                             |
| ---- | ------------------------ | ------------------------------ |
| P0-1 | 2.4G 与 BLE 抢占同一射频 PHY    | 需求(二).1.c 的 BLE 兜底 **100% 失效** |
| P0-2 | 跳频表频点越界（>41）             | 键盘 33% / 遥控 37.5% 的帧发到非法频点     |
| P0-3 | TDMA 时隙预算超支 3 倍          | 遥控模式不可用，主循环被阻塞 16.4ms          |
| P0-4 | **时间基准被破坏**              | 去抖/连击/UI 帧周期/手势判定**全部失真**      |
| P0-5 | 电池采样阻塞 500ms + 5 分钟刷新未实现 | 开机卡死 0.5s，电量永远停在开机读数           |
| P0-6 | WS2812 刷屏关中断 0.3ms       | 1ms 时基丢拍                       |

另有 9 个 P1（其中 4 个是游戏按键与需求不符）、7 个 P2。

**最值得优先处理的其实是 P0-4** —— 它一处污染按键、UI、通信三层，修复收益远大于其余各项之和。

---

## 1. 资源基线实测数据

以下数据从 `obj/CH592X.map` 与 `Ld/Link.ld` 实测得出，是判断优化可行性的基础。

### 1.1 Flash

| 项目         | 实测值                      | 占比        |
| ---------- | ------------------------ | --------- |
| 总容量 (ROM)  | 448 KB                   | 100%      |
| `.text` 已用 | 198 732 B ≈ **194.1 KB** | **43.3%** |
| **剩余**     | 260 020 B ≈ **253.9 KB** | 56.7%     |

> **结论：Flash 余量非常充裕（还有 254KB）。** 之前担心的"OCV 表 404B 会不会太占空间"不成立——它只占剩余量的 0.16%。**这个表可以保留完整精度，不必为了省空间而简化。**（见 §8.1）

### 1.2 RAM（26 KB）

```
0x20000000  ┌─────────────────────────┐
            │ .text 高地址代码段       │  已用
0x200021A4  ├─────────────────────────┤  _edata
            │ .data          1 984 B   │
0x20002968  ├─────────────────────────┤
            │ .bss         10 480 B   │
0x20005258  ├─────────────────────────┤  _ebss = _end
            │                         │
            │   堆（malloc 可用）      │  ← 实测 5 032 B
            │                         │
0x20006600  ├─────────────────────────┤  _heap_end
            │   栈  __stack_size       │  ← 仅 512 B ※
0x20006800  └─────────────────────────┘  RAM 顶
```

| 项目                            | 实测值         | 评价          |
| ----------------------------- | ----------- | ----------- |
| 静态已用 (`.data`+`.bss`)         | 12 464 B    | —           |
| **堆可用**（`_ebss`..`_heap_end`） | **5 032 B** | 够用，但需精算嵌套   |
| **栈 `__stack_size`**          | **512 B**   | ※ **偏小，见下** |

### 1.3 三个必须注意的资源事实

**① 堆实际有 5032 B，比代码注释里写的宽裕**

`Main.c:88-92` 的注释说"堆 2680 B / 约 2960 B"。实测为 **5 032 B**。这意味着 `Main.c:101` 那句

```c
typedef char rc_host_must_fit_in_heap[((sizeof(rc_host_t) + 64u) < 4096u) ? 1 : -1];
```

虽然通过，但实际约束（4096）远松于真实上限（5032）。**遥控模式 `rc_host_t`（约 2.8KB）与游戏 `game_pool` 不能同时占用**——这正是 `Main.c` 用"进入遥控页时把堆让给 rc_host、退出时还回"的原因，该设计是正确且必要的，务必保留。

**② 栈只有 512 B，是隐藏的溢出风险**

`Ld/Link.ld:3` → `__stack_size = 512;`。而工程中存在这些深调用链：

```
Game_Task → game_scan → key_track                    (3 层)
UI_Task    → draw → Game_DrawText → Game_DrawGlyph    (4 层)
                       → TFT_Printf → vsnprintf        (printf 内部栈开销大)
BAT_ReadMv → bat_read_adc                             (含 8 次 ADC 采样局部数组)
UiKbd_BuildPressedText (局部 buf[32]) → UsageName
rf24 收发  → HALRF_TxRx → halrf_wait_done             (rfConfig_t 结构体可能很大)
```

`TFT_Printf` 走的是 `vsnprintf`，典型局部帧 100~200 B；`HALRF_TxRx` 若在栈上定义 `rfConfig_t` 更容易超限。**512 B 属于"能用但没有余量"的水平**，任何新增的局部数组都可能踩雷。

**建议：`__stack_size` 从 512 提到 1024。** 代价是堆从 5032 降到 4528 B ——  
但由于 §1.3① 的结论（堆本就宽裕且靠让位设计保护），这个交换是划算的。

> 若担心堆，则反向操作：把 P0-1 的 PHY 隔离改好后，遥控与键盘不再并发，堆压力会进一步下降。

**③ 编译优化等级是 `-Os`（size），不是 `-O2`**

`.cproject` 中 `optimization.level = ilg.gnumcueclipse.managedbuild.cross.riscv.option.optimization.level.size`，  
对应 `-Os`：**优先压缩代码体积**。

这解释了为什么 Flash 只用了 43%（`-Os` 功劳）**同时**代码跑得比预期慢（`-Os` 牺牲性能）。  
在还有 254 KB 余量的前提下，**改用 `-O2` 是"免费"的性能提升**——见 §6.4。

---

## 2. 修复总路线

```
阶段一 · 打通功能          阶段二 · 修复时基          阶段三 · 对齐需求
─────────────────────      ─────────────────────      ─────────────────────
P0-1 射频/BLE 隔离          P0-4 统一时间基准          P1-1 雷霆战机键位
P0-2 跳频表合法化           P0-3 遥控时隙重排           P1-2 跑酷持续按住
P2-4 Flash 页重分配         P0-5 电池非阻塞            P1-4 编码器手势统一
P2-7 背光接法确认           P0-6 WS2812 时基           P1-5 恢复默认补全
                                                P1-6 映射组统一为 5
                                                P1-7 键盘页恢复 50px
                                                P1-3/1-9 解耦与清屏
```

**阶段优先级说明：**

- **阶段一**解决"功能根本跑不起来"，做完才能验证后续
- **阶段二**收益最大：P0-4 一处修好，去抖/连击/手势/UI 帧周期**同时恢复正常**
- **阶段三**是对齐需求文档，可并行穿插进行

> ※ **不要在阶段二完成前验证阶段三的功能表现** —— 时基错误会让所有时序相关的功能验证结果失真。

---

## 3. P0 致命问题修复

### P0-1　2.4G 与 BLE 抢占同一射频前端

**位置** `ui/ui_kbd_ch.c:115-134` · `rf24/hal_rf_ch592.c:245-248` · `User/Main.c:232-233`

#### 问题

`UiKbdCh_SetRadioMode()` 的注释声称会复位射频，实际 `else` 分支只清了两个软件标志：

```c
// ui_kbd_ch.c:128-133  —— 注释说会恢复，实际什么都没做
else
{
    /* 遥控模式由本文件负责切地址 proto_rc.c 里做 */
    s_rf24_alive = 0u;      // 仅清了两个软件标志
    s_rf_hb_ms   = 0u;
}
```

而 `HALRF_Init()` 会调 `RF_Config()` 覆盖全局 PHY：

```c
// hal_rf_ch592.c:245-248
s_cfg.accessAddress = Rf_GetU32LE(cfg->tx_addr);   // 全局 PHY 地址
st = RF_Config(&s_cfg);                // 覆盖 BLE 协议栈已配置好的 PHY
```

CH592 只有**一个**射频前端，`rfConfig_t.accessAddress` 是全局寄存器。`Main.c` 的初始化顺序致命：

```c
BT_WCH_Init();      // Main.c:232  BLE 配置 PHY（accessAddress = BLE 地址）
UiKbdCh_Init();     // Main.c:233  → HALRF_Init() → RF_Config(2.4G 地址)  ← 覆盖
```

#### 影响

开机后 BLE 的 PHY 地址被 2.4G 键盘地址覆盖。需求 **(二).1.c**「否则进入 BLE 广播等待连接（时长不限）」**必然失效**，不是概率问题。且 `HALRF_Shut/FlushTx/FlushRx`（`hal_rf_ch592.c:498-517`）全部直接 `RF_Shut()`，与 BLE 收发完全并发，互相打断。

#### 修复

**方案 A（推荐）：补齐状态保存/恢复**

```c
/* ---- hal_rf_ch592.c 顶部新增 ---- */
static uint32_t s_saved_addr;       /* 进 2.4G 前的 BLE accessAddress */
static uint8_t  s_saved_channel;
static uint8_t  s_ble_owned;        /* 1 = PHY 当前归 2.4G 使用 */

void HALRF_SaveBleState(void)
{
    if (s_ble_owned) { return; }      /* 已在 2.4G 模式，不重复保存 */
    RF_QueryStatus();                 /* 让 SDK 把当前 PHY 状态回填 s_cfg */
    s_saved_addr    = s_cfg.accessAddress;
    s_saved_channel = s_cfg.Channel;
    s_ble_owned     = 1u;
}

void HALRF_RestoreBleState(void)
{
    if (!s_ble_owned) { return; }
    s_cfg.accessAddress = s_saved_addr;
    s_cfg.Channel       = s_saved_channel;
    (void)RF_Config(&s_cfg);          /* 恢复 BLE 的 PHY */
    s_ble_owned = 0u;
}
```

在 `HALRF_Init()` **之前**必须先调 `HALRF_SaveBleState()`：

```c
// hal_rf_ch592.c  HALRF_Init() 开头
HALRF_SaveBleState();          // ← 新增：先备份 BLE 的 PHY
s_cfg.accessAddress = Rf_GetU32LE(cfg->tx_addr);
st = RF_Config(&s_cfg);
```

```c
/* ---- ui_kbd_ch.c:UiKbdCh_SetRadioMode() ---- */
void UiKbdCh_SetRadioMode(bool keyboard_on)
{
    if (keyboard_on)
    {
        rf_cfg_kb_t kcfg;
        (void)RfCfg_KbLoad(&kcfg);
        HALRF_SetLink(kcfg.addr, kcfg.channel);
        s_rf24_alive = 0u;
        s_rf_hb_ms   = 0u;
    }
    else
    {
        /* 新增：把 PHY 还给 BLE 协议栈 */
        HALRF_RestoreBleState();       // ← 真正做隔离
        s_rf24_alive = 0u;
        s_rf_hb_ms   = 0u;
    }
}
```

**方案 B（更彻底）：不改 accessAddress**

CH592 的 `RF_SetFrequency()` 只改频率不改地址。可以让 2.4G 与 BLE 共用同一 `accessAddress`，仅用信道区分；若必须用不同地址，则用时隙化方案把两协议分到不同超帧。

**验证要点**：进入键盘页等 3 秒（超时 1.5s+1.5s），确认 BLE 能被手机扫描到并成功配对连接。

---

### P0-2　跳频表频点越界

**位置** `rf24/link_hop.c:12-13` · `rf24/proto_common.h:58` · `rf24/hal_rf_ch592.c:51-54`

#### 问题

```c
// link_hop.c:12-13
const uint8_t hop_kb_table[HOP_KB_COUNT] = { 20u, 35u, 50u };
const uint8_t hop_rc_table[HOP_RC_COUNT] = { 5u, 12u, 19u, 26u, 33u, 40u, 47u, 54u };

// hal_rf_ch592.c:51-54
static uint32_t ch_to_freq(uint8_t ch) { return 2400000u + ((uint32_t)ch * 2000u); }
```

SDK `CH59xBLE_ROM.h:2498` 标注合法范围 `2400000kHz-2483500kHz`，即**信道上限 41**。实际越界情况：

| 信道 | 换算频率          | 判定     |
| -- | ------------- | ------ |
| 50 | 2 500 000 kHz | **越界** |
| 47 | 2 494 000 kHz | **越界** |
| 54 | 2 508 000 kHz | **越界** |

而 `proto_common.h:58` 定义 `RF_MAX_CHANNEL 84`，使 `hal_rf_ch592.c:295` 的守卫  
`if (ch > RF_MAX_CHANNEL) return;` **形同虚设** —— `ch_to_freq(84) = 2 568 000 kHz`（超频 85 MHz）。

#### 影响

- 键盘模式按 `(seq>>5) % 3` 轮询 → **每 3 帧有 1 帧（33%）工作在 2500MHz 非法频点**，主机收不到
- 遥控模式 8 个频点中 3 个非法 → **链路成功率上限 62.5%**
- `proto_rc.c:1163` `Rc_DevOnSync()` 收到 SYNC 后会跟到非法频点，**从机也一起跑飞**
- `proto_kb.c:225-231` `Kb_ParsePair()` 用同一错误常量校验对码帧 channel，脏 Flash 数据可写入非法频点

#### 修复

```c
/* proto_common.h:58 —— 按 SDK 实际范围修正 */
#define RF_MAX_CHANNEL          41u   /* (2483500-2400000)/2000 */

/* link_hop.c:12-13 —— 全部落在 2400~2483.5MHz */
const uint8_t hop_kb_table[HOP_KB_COUNT] = { 20u, 35u, 40u };
const uint8_t hop_rc_table[HOP_RC_COUNT] = { 5u, 12u, 19u, 26u, 31u, 36u, 40u, 41u };
```

并在 `ch_to_freq()` 加兜底断言：

```c
// hal_rf_ch592.c
static uint32_t ch_to_freq(uint8_t ch)
{
    uint32_t f = 2400000u + ((uint32_t)ch * 2000u);
    if (f > 2483500u) { f = 2483500u; }   /* 硬钳位，杜绝超频 */
    return f;
}
```

> **注意**：修改跳频表后，接收端固件**必须同步更新**同一张表，否则键盘与接收器无法对频。建议在两个工程共用同一份 `link_hop.h`，从源头避免不一致。

---

### P0-3　TDMA 时隙预算超支 3 倍

**位置** `rf24/link_tdma.h:36-37` · `rf24/proto_rc.c:688,725` · `rf24/hal_rf_ch592.c:472`

#### 问题

时隙预算 800μs，实际忙等远超此数：

```c
// link_tdma.h:36-37
#define RC_SLOT_US      800u
#define RC_SLOT_COUNT   20u      // 超帧 = 16ms
```

| 阶段        | 调用                                              | 预算    | 实际           | 超出       |
| --------- | ----------------------------------------------- | ----- | ------------ | -------- |
| Slot 0 同步 | `HALRF_TxWait(...,2000u)`                       | 800μs | 2000μs       | **2.5×** |
| 每设备时隙     | `HALRF_TxRx(...)` → `halrf_wait_done(400+2000)` | 800μs | 2400μs       | **3×**   |
| 6 设备满配    | 2000 + 6×2400                                   | 16 ms | **16.4 ms+** | **溢出**   |

#### 影响

- `link_tdma.c:81-87` 溢出计数每帧饱和 → `Tdma_HadOverflow()` 永久为真
- `proto_rc.c:470` 的 48ms 离线判定持续误触发 → **设备列表反复被清空**
- `hal_rf_ch592.c:365-390` `halrf_wait_done()` 是**纯忙等**，而 `Main.c:276` 在主循环调 `Rc_HostSuperframe()` → **主循环阻塞 16.4ms** → `BT_WCH_Process()` 饿死、`UI_Task()` 界面卡死

#### 修复

**第一步：把固定 2000μs 改为时隙内可用余量**

```c
/* proto_rc.c 顶部 */
#define RC_SLOT_GUARD_US   (RC_SLOT_US / 2u)   /* 400us，留一半给收发 */

if (HALRF_TxWait(sync, RC_SYNC_LEN, RC_SLOT_GUARD_US) != HALRF_OK) { ... }
```

**第二步：收紧 HALRF 内部等待**

```c
/* hal_rf_ch592.c:472 */
- halrf_wait_done(rx_window_us + 2000u);
+ halrf_wait_done(rx_window_us + 200u);
```

**第三步（关键）：忙等改非阻塞**

纯忙等即使不超时也会吃满 CPU。应让 `HALRF_TxRx` 返回"进行中"，主循环轮询完成：

```c
/* hal_rf.h 新增 */
typedef enum { HALRF_BUSY = 0, HALRF_DONE, HALRF_ERR } halrf_state_t;
halrf_state_t HALRF_Poll(void);      /* 非阻塞查状态 */

/* hal_rf_ch592.c 改造：把 while 忙等换成状态机 */
int HALRF_TxRx(const uint8_t *tx, uint8_t txlen, uint8_t *rx, uint8_t *rxlen, uint32_t window_us)
{
    g_halrf.state = HALRF_BUSY;
    g_halrf.deadline = SYS_GetSysTickCnt() + ...;
    /* 只启动射频，不等待 */
    return HALRF_OK;
}
```


主循环里：
```c
/* Main.c */
if (HALRF_Poll() == HALRF_DONE) { rc_on_frame_done(); }
```

> 若时间紧张，**至少先做完第一步 + 第二步**，可把超支从 3× 降到 1.3× 以内，已能消除溢出计数。

---

### P0-4　时间基准被破坏（影响面最广）

**位置** `games/game_port.c:141-142,171-175` · `User/Main.c:55-63` · `hardware/key_matrix.c:202`

> **这是本工程最值得优先修复的问题** —— 一处污染按键、UI、通信三层。

#### 问题 (a)：编码器被扫描两次

```c
TMR0_IRQHandler()  → ENC_Scan()      // Main.c:60，每 1ms
game_scan()        → ENC_Scan()      // game_port.c:142，每 1ms（主循环）
```

导致 `encoder.c` 的 `s_steps` / `s_acc` 被**双倍累加** —— 旋转一格可能上报 2 步。
且中断与主循环**并发读写同一组静态变量**，无临界区、无 `volatile`（`encoder.c:33-40`）。

#### 问题 (b)：去抖时间失真

```c
// key_matrix.h:120  声明每 5ms 扫一次
#define KEY_SCAN_PERIOD_MS      5
// key_matrix.c:202  但无论真实间隔多久，每次调用都假定过了 5ms
s_tick_ms = (uint16_t)(s_tick_ms + KEY_SCAN_PERIOD_MS);
```

而 `KEY_Scan()` 实际在主循环里被以 ~1ms 频率调用（`game_port.c:141`）。结果：

| 参数 | 设定值 | 实际等效值 | 后果 |
|---|---|---|---|
| `KEY_DEBOUNCE_MS` | 15 ms | **≈ 3 ms** | 去抖不足 → **误触发** |
| `KEY_COMBO_WINDOW_MS` | 40 ms | **≈ 8 ms** | 组合键窗口过窄 → 组合判失败 |

**两个方向都失真**：去抖被压缩易误触发，组合窗口被压缩导致组合键难以识别。

#### 问题 (c)：游戏时间与真实时间脱节

```c
// game_port.c:144   s_ms 是"循环计数"，不是真实时间
s_ms += GAME_SCAN_MS;         // 恒定 +1，与真实耗时无关
// game_port.c:174
DelayMs(GAME_SCAN_MS);        // 忙等 1ms
```

主循环还要跑 `UsbKbd_DeviceTask` / `BT_WCH_Process` / `UiKbdCh_PollRf24` / `UI_Task`，真实周期 **> 1ms**（全屏重绘约 20~40ms，RF 忙等 2.4ms）。

于是 `Game_Millis()` 返回的是"循环次数"而非真实毫秒，**所有依赖它的时序判定全部失真**：
- `ui_page.c:229` `UiEnc_Step(&s_enc, s_enc_down, now)` → 双击 300ms 窗口
- `ui_page.c:237` `now - s_last_frame_ms < UI_FRAME_MS` → UI 帧周期
- `game_port.c:112-115` 按键长按重复（450ms/90ms）

**症状**：长按 3 秒退主页面，实际可能需要十几秒真实时间。

> **关键发现**：`Main.c:53` 的 `g_ms_tick` 自增后**从未被任何代码读取** ——
> 而它（TMR0 1ms 中断累加）恰恰就是修复所需的真实时基。

#### 修复

**核心原则：TMR0 中断是唯一时间基准；中断只做采样，主循环只做协议，永不 `DelayMs` 忙等。**

```c
/* ---- Main.c ---- */
static volatile uint32_t g_ms_tick;          // 保留，作为唯一时基

void TMR0_IRQHandler(void)
{
    if (TMR0_GetITFlag(TMR0_3_IT_CYC_END) != 0u)
    {
        TMR0_ClearITFlag(TMR0_3_IT_CYC_END);
        ENC_Scan();                          // 编码器只在中断里扫
        g_ms_tick++;
    }
}
```

```c
/* ---- game_port.c ---- */
static uint32_t s_last_scan_ms;

static void game_scan(void)
{
    /* 注意：ENC_Scan() 已移到中断，这里不再调用，避免双扫 */
    KEY_Scan();
    s_ms = g_ms_tick - s_base_ms;             // 真实毫秒
    /* ... 按键跟踪不变 ... */
}

void Game_Task(void)
{
    uint32_t now = g_ms_tick;

    if (now == s_last_scan_ms) { return; }    // 真实 1ms 节拍
    s_last_scan_ms = now;
    game_scan();
    /* 删掉 DelayMs(GAME_SCAN_MS); */
}

void Game_Init(GameOrient_t orient)
{
    s_base_ms      = g_ms_tick;
    s_last_scan_ms = g_ms_tick;
    s_ms           = 0u;
    /* ... */
}

uint32_t Game_Millis(void) { return s_ms; }
```

```c
/* ---- key_matrix.c：用真实时间差累加 ---- */
void KEY_Scan(void)
{
    static uint32_t s_last_ms;
    uint32_t now = g_ms_tick;
    uint16_t dt  = (uint16_t)(now - s_last_ms);
    uint16_t raw, need_cnt;

    if (dt == 0u) { return; }
    s_last_ms = now;
    if (dt > 50u) { dt = 50u; }               /* 异常保护 */

    raw = key_read_raw();
    s_tick_ms = (uint16_t)(s_tick_ms + dt);   // ← 按真实 dt 累加
    /* 后续去抖逻辑不变，need_cnt 计算依然正确 */
```

> **需在 `game_port.h` 暴露时基**：
> ```c
> extern volatile uint32_t g_ms_tick;
> ```

**验证要点**：修完后，长按编码器 3 秒必须**真实 3 秒**退出主页面；双击两次按键的间隔 300ms 应可靠识别。

---

### P0-5　电池采样阻塞 + 5 分钟刷新未实现

**位置** `hardware/battery.c:242` · `ui/ui_widget.c:19-29` · `User/Main.c:237`

#### 问题

```c
// battery.c:239-242   采样一次要等 500ms
BAT_Enable(1);
DelayMs(BAT_EN_SETTLE_MS);        // 500u → 阻塞 500ms
```

而需求 **(一).2** 要求「开机后**每 5 分钟**读取并刷新」。实际只在开机读一次：

```c
// ui_widget.c:26-29   只有 Init，没有 Task
void UiW_BatInit(void) { bat_sample_once(); }

// ui_widget.c:14 的注释坦承：
/* 原本设计是每 5 分钟刷新（UiW_BatTask()），现在**根本没有任何地方调用它** */
```

#### 影响

1. 开机瞬间主循环卡死 **500ms**（TFT/BLE 尚未就绪，表现为白屏延迟）
2. **电量显示永远停留在开机读数** —— 需求 5 分钟刷新未实现
3. 若照注释补上 `UiW_BatTask()`，每 5 分钟会有一次 500ms 卡顿，遥控/游戏期间表现为"整机卡住半秒"

#### 修复

**改为非阻塞状态机**

```c
/* ---- battery.h 新增 ---- */
typedef enum { BAT_IDLE = 0, BAT_SETTLING, BAT_SAMPLING, BAT_DONE } bat_state_t;

extern void     BAT_TaskStart(void);              /* 启动一次采样 */
extern bat_state_t BAT_TaskPoll(void);            /* 非阻塞轮询 */
extern uint8_t  BAT_TaskGetPercent(void);
extern uint16_t BAT_TaskGetMv(void);

/* 保留原同步接口供兼容 */
extern uint16_t BAT_ReadMv(void);
```

```c
/* ---- battery.c ---- */
static bat_state_t s_state;
static uint32_t    s_t_start;

void BAT_TaskStart(void)
{
    BAT_Enable(1);
    s_t_start = g_ms_tick;
    s_state   = BAT_SETTLING;
}

bat_state_t BAT_TaskPoll(void)
{
    switch (s_state)
    {
        case BAT_SETTLING:
            if ((uint32_t)(g_ms_tick - s_t_start) >= BAT_EN_SETTLE_MS)
            {
                (void)bat_read_adc();
                s_state = BAT_DONE;
            }
            break;
        case BAT_DONE:
            /* 换算并落缓存 */
            s_last_mv      = (uint16_t)(/* pin_mv * MUL / DEN * GAIN / DEN + OFFSET */);
            s_last_percent = (uint8_t)BAT_MvToPercent(s_last_mv);
#if (BAT_EN_LEAVE_ON == 0)
            BAT_Enable(0);
#endif
            s_state = BAT_IDLE;
            break;
        default:
            break;
    }
    return s_state;
}
```

```c
/* ---- Main.c 主循环 ---- */
static uint32_t s_bat_last;
static uint8_t  s_bat_pct;

if ((uint32_t)(g_ms_tick - s_bat_last) >= 300000u)   /* 5 分钟 */
{
    s_bat_last = g_ms_tick;
    BAT_TaskStart();
}
if (BAT_TaskPoll() == BAT_IDLE && /* 本轮刚完成 */)
{
    s_bat_pct = BAT_TaskGetPercent();
    UI_Dirty();                       /* 刷新顶部电量条 */
}
```

**顺带标定 `BAT_EN_SETTLE_MS`**：500ms 对电阻分压网络通常过大，**10~50ms 即足够**。建议用示波器实测：使能 PB14 后，PA7 上的电压稳定时间是多少，取其 2~3 倍作为阈值即可。这一项能显著缩短开机延迟。

---

### P0-6　WS2812 刷屏关中断 0.3ms

**位置** `hardware/rgb_led.c:219-243,174-190`

#### 问题

```c
// rgb_led.c:221-238   一帧 9 灯 × 24bit × 1.25us ≈ 270us，全程关中断
uint32_t intlev = __risc_v_disable_irq();
   ... 9 × 3 × 8 = 216 bit ...
__risc_v_enable_irq(intlev);
DelayUs(RGB_RESET_US);        // 又额外阻塞 300us
```

且忙等依赖硬件 PWM 周期结束标志：
```c
// rgb_led.c:178-181   纯自旋，且有 guard 兜底
R8_PWM_INT_CTRL |= RB_PWM_IF_CYC;
while (((R8_PWM_INT_CTRL & RB_PWM_IF_CYC) == 0u) && (--guard)) { }
```

#### 影响

- 每帧关中断 0.3ms + 阻塞 300μs → **TMR0 的 1ms 时基丢拍**
- `guard = 100000` 兜底意味着：若 PWM 标志位因配置问题不置位，代码会**静默自旋到超时**并输出错误时序的 WS2812 信号（灯珠乱色），**不会有任何报错**

#### 修复

**① 复位等待移出中断上下文**
```c
// rgb_led.c:242
- DelayUs(RGB_RESET_US);
+ /* 复位低电平交给下一次刷帧的起始空闲期，见 RGB_IdleReset() */
```

**② 强制校验 PWM 配置**
```c
// rgb_led.c:224  刷帧前自检
if (RGB_BIT_TICKS == 0u || (PWMX_QueryCycleCfg() != RGB_BIT_TICKS))
{
    /* 配置错误 → 明确标志，便于定位，而不是静默输出乱码 */
    s_pwm_cfg_err = 1u;
    return;
}
```

**③ 建议先用逻辑分析仪确认波形**

在改代码之前，先用示波器/逻辑分析仪量 PB7：
- 位周期应为 **1.25μs ± 150ns**
- `'0'` 高电平 0.4μs / `'1'` 高电平 0.8μs

`RGB_BIT_TICKS=75`（1.25μs @60MHz）、`RGB_T0H_TICKS=24`（0.4μs）、`RGB_T1H_TICKS=48`（0.8μs）**计算上是对的**。若实测波形正确，说明问题只在时基干扰；若波形本身就不对，则需调整 `PWMX_CLKCfg` 实际分频比（注意 `PWMX_16bit_CycleCfg(75)` 的输入是否被 SDK 内部再次分频）。

**④ 长期方案：改用 SPI 或 DMA 发送 WS2812**

彻底摆脱 bit-bang 与关中断。见 §6.3。

---

## 4. P1 逻辑错误修复

### P1-1　雷霆战机按键持续按住逻辑与需求矛盾

**位置** `games/game_thunder.c:674-676`

**事件处理部分是正确的**（`game_thunder.c:1437-1451`，与需求 (三).3 完全一致）：
| 动作 | 需求 | 事件处理 | √ |
|---|---|---|---|
| 上 | key1 | `GKEY_K1` | √ |
| 下 | key4 | `GKEY_K4` | √ |
| 开火 | key6 | `GKEY_K6` | √ |
| 炸弹 | key3 | `GKEY_K3` | √ |

**但每帧轮询用的是另一套宏**：

```c
// game_thunder.c:674-676
bool up   = (s_ev_up   || Game_KeyDown(GKEY_UP))   ? true : false;  // GKEY_UP   = GKEY_K5
bool down = (s_ev_down || Game_KeyDown(GKEY_DOWN)) ? true : false;  // GKEY_DOWN = GKEY_K8
bool fire = (s_ev_fire || Game_KeyDown(GKEY_LEFT)) ? true : false;  // GKEY_LEFT = GKEY_K4 ← 撞了"下"！
```

**影响**：按 K4（需求里的"下"）会**同时触发 fire**，飞机一边下移一边开火；K1/K3 长按完全无效（轮询查的是 K5/K8）。**只有点按正常，长按行为全乱。**

```c
/* game_thunder.c:674-676 —— 改为与事件处理一致 */
bool up   = (s_ev_up   || Game_KeyDown(GKEY_K1)) ? true : false;
bool down = (s_ev_down || Game_KeyDown(GKEY_K4)) ? true : false;
bool fire = (s_ev_fire || Game_KeyDown(GKEY_K6)) ? true : false;
```

---

### P1-2　跑酷 key7 冲刺不支持长按

**位置** `games/game_runner.c:334` vs 372

需求 **(三).2** 要求"加速/冲刺 → key7"。但 K7 只在 `GAME_EV_PRESS` 瞬发一次，没有持续状态：

```c
// game_runner.c:334  仅按下瞬发
case GKEY_K7:
    if (press) { /* 冲刺触发 */ }

// game_runner.c:372  但下蹲却用了持续判断（风格不一致）
s_sliding = Game_KeyDown(GKEY_K8);
```

```c
/* game_runner.c —— 为 K7 增加持续状态，与 K8 风格统一 */
s_dashing = Game_KeyDown(GKEY_K7);
```

---

### P1-3　恢复默认设置漏清自定义键值映射

**位置** `ui/ui_pages.c:459-472`

需求 **(五).4.b** 明确要求：「恢复默认设置后，所有的设置都恢复到默认值（包括键盘灯亮度，休眠时间，**所有新建的键盘映射**）」。

```c
// ui_pages.c:459-472
UiStore_Default();  UiStore_Save();
RGB_Switch(c->led_on);  RGB_SetBrightness(c->led_bright);
KEY_SetMapGroup(0u);          // ← 只切回第 0 组
// 缺失：KEY_ResetMap()  没有调用！
```

`KEY_SetMapGroup(0)` 只是切换**当前组**，用户在自定义组改过的键值原封不动留在 RAM，下次切回去还是改过的值。

```c
/* ui_pages.c:471 —— 补一行 */
KEY_SetMapGroup(0u);
KEY_ResetMap();               // ← 新增：key_matrix.c:380 已存在此函数
```

> `KEY_ResetMap()` 的实现已验证正确（`key_matrix.c:380-392`，全量恢复 `s_map_default`），直接调用即可。

---

### P1-4　编码器手势体系混乱

**位置** `ui/ui_kbd.c:327-330` · `ui/ui_enc.h:50-55` · `ui/ui_pages2.c:238,428`

需求定义：
- **(二).3**：编码器**长按 1 秒**后进入键盘设置页面
- **(六).3**：**双击**返回上一级
- **(六).4**：长按 0.5~2.5s 执行当前可执行的操作

**实现的三处冲突**：

```c
// ui_kbd.c:327-330   单击和长按都进设置页
if ((ev->key == GKEY_ENC_HOLD) || (ev->key == GKEY_ENC_CLICK)) {
    UI_Push(&UI_PAGE_KBSET, 0);
}
```

```c
// ui_enc.h:51   HOLD_MS = 500ms，而需求要 1s
#define UI_ENC_HOLD_MS    500u
```

```c
// ui_pages2.c:238,428   用"长按"做返回，与需求"双击返回"冲突
if (ev->key == GKEY_ENC_HOLD) { return UI_NAV_POP; }
```

**影响**：用户在键盘页想用编码器单击做别的事时，会被直接弹进设置页（只能 HOLD3 返回）；两套返回手势并存，用户体验混乱。

**修复**：
```c
/* ui_enc.h —— 新增长按 1s 档位 */
#define UI_ENC_HOLD_MS      500u     /* 0.5~2.5s：执行当前操作 */
#define UI_ENC_HOLD1_MS    1000u     /* 1s：进键盘设置页（需求二.3） */
#define UI_ENC_HOLD3_MS    3000u     /* >3s：退主页面 */

/* ui_enc.c —— 在 HOLD3 之后、HOLD 之前插入 HOLD1 判定 */
if ((e->hold1_fired == 0u) && (held >= (uint32_t)UI_ENC_HOLD1_MS)) { ... }

/* ui_kbd.c:327 —— 键盘页只用长按 1s 进设置 */
if (ev->key == GKEY_ENC_HOLD1) { UI_Push(&UI_PAGE_KBSET, 0); }

/* ui_pages2.c:238,428 —— 返回统一改用双击，删掉长按返回 */
- if (ev->key == GKEY_ENC_HOLD) { return UI_NAV_POP; }
+ if (ev->key == GKEY_ENC_DBL)  { return UI_NAV_POP; }
```

---

### P1-5　映射组数量不一致：4 vs 5

**位置** `hardware/key_matrix.h:201` vs `ui/ui_store.h:40`


需求 **(二).3** 要求「默认的 **3 套**键值映射 + 自定义键值映射 **2 套**」= 共 5 组。

```c
ui_store.h:40      #define UI_MAPGROUP_COUNT  5   // 3 默认 + 2 自定义
key_matrix.h:201   #define KEY_MAP_GROUPS      4   // 实际只有 4 组
```

`ui_pages2.c:41-42` 的注释已承认此矛盾：
```c
#define KBSET_ITEMS  4u   /* 3 套默认 + 1 个"自定义（键值编辑页）" */
                      /* 实际硬件 KEY_MAP_GROUPS = 4 */
```

**影响**：
- `KEY_SetMapGroup(4)` 被 `key_matrix.c:418` 边界检查重置为 0 → **第 5 组永远无法选中**
- `ui_store.c:128` 的 `map_group >= 5` 检查失去意义
- 用户在"自定义"里改的键值实际写进第 3 组（**覆盖掉 F1-F9 那套默认组**）

**修复**：
```c
/* key_matrix.h:201 */
#define KEY_MAP_GROUPS      5       /* 3 默认 + 2 自定义 */

/* key_matrix.c —— 补第 5 组默认值（与第 4 组相同作为初始值） */
static const uint16_t s_map_default[KEY_MAP_GROUPS][KEY_MAP_KEYS] =
{
    { 0x1Eu, 0x1Fu, 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u },  /* 组0 数字 123 */
    { 0x29u, 0x52u, 0x2Cu, 0x50u, 0x28u, 0x4Fu, 0x80u, 0x51u, 0x81u },  /* 组1 方向 */
    { 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu, 0x40u, 0x41u, 0x42u },  /* 组2 F1-F9 */
    { 0x59u, 0x5Au, 0x5Bu, 0x5Cu, 0x5Du, 0x5Eu, 0x5Fu, 0x60u, 0x61u },  /* 组3 小键盘 */
    { 0x1Eu, 0x1Fu, 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u },  /* 组4 自定义1 */
};

/* key_matrix.c:371 —— 补第 5 组名字 */
static const char *const s_map_name[KEY_MAP_GROUPS] = { "123", "ARROW", "F1-F9", "NUM", "CUS1" };
```

`ui_pages2.c` 的 `KBSET_ITEMS` 改为 5 或 6（4 具名 + 2 自定义入口）。

> ※ **需同步考虑 Flash 存贮**：`KEY_SetMapValue()` 改的值目前只在 RAM，改 `kbset_key()` 与 `ke_key()` 中的 `UiStore_Save()` 落盘（见 §6.5）。

---

### P1-6　键盘页右侧面板被压缩到 43px

**位置** `ui/ui_kbd.h:36-46`

需求 **(二).2.a**：左侧 **189px** 显示按下按键；右上角 **50×50px** 状态；右侧中间 **50×33px** 连接状态；右下角 **50×50** 3×3 网格。

```c
// ui_kbd.h:38-43   注释里已经算过这道题，结论是"塞不下"
/* 需求右侧每个面板 = 240 - 197 = 43px（需求要 50x50 但塞不下，硬把 189px 压缩成 43px） */
#define UIK_PANEL_X     197
#define UIK_PANEL_W     43
#define UIK_GRID_SIZE   41      // 3*13 + 2*1
```

**但这个算错了** —— 三个面板是**垂直堆叠**的，不是水平并排：
```
189 (左) + 1 (分隔) + 50 (右) = 240  ← 正好满宽！
```

```c
/* ui_kbd.h —— 恢复需求尺寸 */
#define UIK_LEFT_W      188     /* 需求 189，减去 1px 分隔线 */
#define UIK_PANEL_X     189     /* 188 + 1px 分隔 */
#define UIK_PANEL_W     50      /* 需求 50x50 */
#define UIK_GRID_SIZE   50      /* UiW_GridCellW(50) = (50-2)/3 = 16，3格+2分隔 = 50 √ */
```

同时 `ui_kbd.c:297` 的分隔线位置需同步调整：
```c
// ui_kbd.c:295-298
UiW_VLine(UIK_LEFT_W, UIK_REGION_Y, UIK_REGION_H, UIW_LINE_WHITE);
```

---

### P1-7　主页面 5 分钟刷新与电池条绘制分离

详见 P0-5。补充一点：`ui_widget.c:50` 的电量条填充
```c
TFT_FillRect(6, 7, (int16_t)((uint16_t)22u * s_bat_pct / 100u), 6, col);
```
在 `s_bat_pct == 0` 时宽度为 0，`TFT_FillRect` 会因 `w <= 0` 直接 return —— **安全**，无需修改。

---

### P1-8　游戏退出后残留画面

**位置** `ui/ui_pages.c:195-202,225-242`

```c
// ui_pages.c:225-242
static void game_draw(void)
{
    if ((s_play == GP_NONE) || (!Game_PoolReady()))
    {
        page_bg();                        // ← 只有列表分支才清屏
        /* ... 画选择列表 ... */
        return;
    }
    switch (s_play) { case GP_2048: G2048_Draw(); break; /* ... */ }
}
```

游戏运行中的画面由各游戏自己绘制，退出后若游戏未清全屏，**上一次的 HUD 像素会残留在列表页背景上**（`page_bg()` 只在列表分支调用，而退出分支走的是 `UiList_Init` + `UI_Dirty`）。

```c
/* ui_pages.c:195-202 —— if (leave) 分支补清屏 */
if (leave)
{
    Game_PoolYield();
    s_play = GP_NONE;
    TFT_FillScreen(UI_COL_BG);        // ← 新增：确保无残留
    UiList_Init(&s_game, UI_SCREEN_W, game_draw_item, 0);
    UiList_SetCount(&s_game, 3u);
    UI_Dirty();
}
```

---

### P1-9　矩阵键导航与编码器导航耦合

**位置** `ui/ui_pages.c:42-49`

```c
static bool is_up(const GameKeyEvent_t *ev) {
    return (ev->key == GKEY_ENC_CCW) || (ev->key == GKEY_UP) || (ev->key == GKEY_LEFT);
}
static bool is_ok(const GameKeyEvent_t *ev) {
    return (ev->key == GKEY_ENC_CLICK) || (ev->key == GKEY_OK) || (ev->key == GKEY_ENC_BTN);
}
```

`GKEY_OK == GKEY_K5`，`GKEY_UP == GKEY_K5` —— **K5 同时命中 `is_up` 与 `is_ok`**。虽然 `is_up` 先 return 短路，但结果是在所有列表页按 K5 **只能上移、无法确认**。

需求 **(六).5** 明确：「旋转：列表类页面正转向下选中，反转向上选中」—— **只提旋转，没提矩阵键导航**。

**修复**：列表页只响应编码器旋转
```c
static bool is_up(const GameKeyEvent_t *ev) {
    return (ev->key == GKEY_ENC_CCW) != 0;      /* 只认旋转 */
}
static bool is_down(const GameKeyEvent_t *ev) {
    return (ev->key == GKEY_ENC_CW) != 0;
}
static bool is_ok(const GameKeyEvent_t *ev) {
    return (ev->key == GKEY_ENC_CLICK) || (ev->key == GKEY_OK);  /* K5 保留确认 */
}
```

---

## 5. P2 健壮性修复

### P2-1　射频接收缓冲区越界 + 原地重叠拷贝

**位置** `rf24/hal_rf_ch592.c:248,412,116,154`

```c
// hal_rf_ch592.c:248
s_cfg.RxMaxlen = (uint8_t)(RF_PLOAD_MAX + CH592_RX_HDR_LEN + 4u);   // = 38
// 但 g_halrf.buf 只有 RF_PLOAD_MAX = 32 字节（hal_rf.h:180）

// hal_rf_ch592.c:412  —— 目标缓冲就是 buf 本身
if (RF_Rx(g_halrf.buf, (uint8_t)RF_PLOAD_MAX, 0xFFu, 0xFFu) != SUCCESS)

// hal_rf_ch592.c:116 / :154  —— 带 2 字节偏移的原地拷贝
g_halrf.buf[i] = rxBuf[i + CH592_RX_HDR_LEN];   // i 最大 31 → 读 rxBuf[33]，越界
```

**修复**：头部长度钳位需扣除头部
```c
/* hal_rf.h 新增 */
#define RX_PAYLOAD_MAX   (RF_PLOAD_MAX - CH592_RX_HDR_LEN)   /* 30 */

/* hal_rf_ch592.c:248 */
s_cfg.RxMaxlen = (uint8_t)sizeof(((halrf_ctx_t *)0)->buf);

/* hal_rf_ch592.c:116,154 */
uint8_t n = rxBuf[1];
if (n > (uint8_t)RX_PAYLOAD_MAX) { n = (uint8_t)RX_PAYLOAD_MAX; }
```

更彻底的做法：给 SDK 一个独立的 `rx_raw[RF_PLOAD_MAX + CH592_RX_HDR_LEN]`，再拷进 `g_halrf.buf`，消除原地重叠依赖。

---

### P2-2　ISR 共享状态缺 volatile、无重入保护

**位置** `rf24/hal_rf.h:178-186`

```c
typedef struct {
    uint8_t  buf[RF_PLOAD_MAX];   // ← 非 volatile，回调(中断)写、主循环读
    uint8_t  len;                 // ← 非 volatile
    int8_t   rssi;                // ← 非 volatile
    volatile uint8_t busy;        // 只有这三个加了 volatile
    volatile uint8_t done;
    volatile uint8_t ok;
} halrf_ctx_t;
```

**影响**：`-Os` 下编译器可将缓冲读取提升到循环外或把 `len` 缓存进寄存器 → **偶发**读到上一帧残留（键盘丢键、遥控错帧）。且 `g_halrf.busy` 从未被读写（`HALRF_Tx`/`RxStart` 都没设），**无任何重入保护** —— `LLE_MODE_AUTO` 下上一帧的迟到回调会污染下一帧的 `ok`，导致 `HALRF_TxWait` 返回假的 `HALRF_OK`。

**修复**：
```c
typedef struct {
    volatile uint8_t  buf[RF_PLOAD_MAX];
    volatile uint8_t  len;
    volatile int8_t   rssi;
    volatile uint8_t  busy;
    volatile uint8_t  done;
    volatile uint8_t  ok;
    volatile uint8_t  seq;        /* 新增：帧序号，防迟到回调污染 */
} halrf_ctx_t;
```
并在 `HALRF_Tx`/`RxStart` 递增 `seq`，回调中比对，不匹配则丢弃。

---

### P2-3　POLL 帧不校验长度 → 幽灵按键

**位置** `rf24/proto_kb.c:619-648,254-272` · `ui/ui_kbd_ch.c:148-153`

```c
// ui_kbd_ch.c:148-153   收到了帧，但 inlen 没往下传
if (HALRF_RxWait(in, &inlen, RF_KB_RX_WINDOW_US) != HALRF_OK) { return false; }
if (!Kb_DevOnPoll(&s_kbdev, in, out, &outlen)) { return false; }   // ← inlen 丢失

// proto_kb.c:268   无条件读 in5[4]
if (flags != 0u) { *flags = in5[4]; }
```

**影响**：收到 1~4 字节的射频噪声帧时，`in[1..4]` 是**未初始化栈数据**。若 `in[0]` 恰为 `0x01`（KB_POLL 类型），就会用垃圾 `seq` 回一帧 HID 应答 → **主机收到随机按键（幽灵输入）**。2.4G 环境下噪声帧 `in[0]==0x01` 概率不低。

**修复**：
```c
/* proto_kb.h */
uint8_t Kb_ParsePoll(const uint8_t *in5, uint8_t len, uint8_t *seq,
                     uint8_t *host_ts_l, uint8_t *link_quality, uint8_t *flags);

/* proto_kb.c:254 —— 入口先判长度 */
uint8_t Kb_ParsePoll(const uint8_t *in5, uint8_t len, ...)
{
    if (len < (uint8_t)KB_POLL_LEN) { return RC_E_LEN; }   /* 必须先判 */
    if (FRAME_TYPE(in5[0]) != (uint8_t)KB_POLL) { return RC_E_PARAM; }
    /* ... */
}

/* ui_kbd_ch.c —— 传 inlen 下去 */
if (!Kb_DevOnPoll(&s_kbdev, in, inlen, out, &outlen)) { return false; }
```

**同类问题**：`proto_rc.c:1243` `Rc_DevServeSlot` 中 `Keymap_HandleRename(rxBuf, ...)` 会读 `rxBuf[5..23]`，但**未校验 `rxLen >= 32`**（`:1263` 只保护了 `rxBuf[1]` 的 dev_id）。

---

### P2-4　RF 配置 Flash 页与 KB 对码记录重叠

**位置** `rf24/rf_cfg.c:270-271,412,10` · `rf24/rf_cfg.h:43-45`

```c
// rf_cfg.h:43-45
#define RF_CFG_PAGE_A_OFF   0x0100u
#define RF_CFG_PAGE_B_OFF   0x0200u
#define RF_CFG_PAGE_LEN     256u

// rf_cfg.c:270-271   RC 存 A/B 双 bank
RfCfg_PageWrite(RF_CFG_PAGE_A_OFF, buf, RC_REC_LEN);    // 0x0100
RfCfg_PageWrite(RF_CFG_PAGE_B_OFF, buf, RC_REC_LEN);    // 0x0200

// rf_cfg.c:412      KB 存盘也写 0x0100，长度 256！
RfCfg_PageWrite(RF_CFG_PAGE_A_OFF, buf, RF_CFG_PAGE_LEN);

// rf_cfg.c:10        擦除是整页 256B
#define RF_CFG_PAGE_ERASE(off)  FLASH_EEPROM_CMD(CMD_EEPROM_ERASE, (off), 0, RF_CFG_PAGE_LEN)
```

**当前 Flash 分区状态**：

| 区间 | 用途 | 冲突 |
|---|---|---|
| `0x0000-0x00FF` | game_score | — |
| `0x0100-0x01FF` | **RC bank A** + **KB 记录** | ※ **互相摧毁** |
| `0x0200-0x02FF` | RC bank B | — |
| `0x0300-0x03FF` | ui_store | — |
| `0x0400+` | **空闲** | — |

与 `game_score` / `ui_store` 不冲突，但 **RC bank A 与 KB 记录完全重叠**。`RfCfg_KbSave()` 先擦整页 → **RC 的 256 字节记录被彻底摧毁**。当前 Main.c 只用 KB 故为潜伏 bug，但 `RfCfg_RcSave/Load` 已编入固件。

**修复**：Flash 还有 254KB 空闲，直接给 KB 分配独立页
```c
/* rf_cfg.h */
#define RF_CFG_PAGE_KB_OFF   0x0400u     /* 新增：KB 独立页 */

/* rf_cfg.c:412 —— RfCfg_KbSave 改用新页，写长度收到记录长度 */
RfCfg_PageWrite((uint16_t)RF_CFG_PAGE_KB_OFF, buf, (uint16_t)KB_REC_LEN);
```
两个 pack/unpack 各自校验 magic，做到"擦除按页、写按记录"，避免无谓地把 `0x0120-0x01FF` 写成 `0xFF`。

---

### P2-5　遥控重命名接口 key_id 越界写 5.6KB

**位置** `rf24/proto_rc.c:609-630`

```c
rc = Keymap_BuildRename(dev_id, key_id, action, scene_param, name, name_len, frame);
if (rc == RC_E_NAME_INVALID) { return rc; }   // 只捕获这一种错误！
if (!Rc_HostSendFrame(...)) { return RC_E_PARAM; }
d->km.key[key_id - 1u].act = RC_ACT_BYTE(action, scene_param);   // key_id 未校验
```

`Keymap_BuildRename`（`link_keymap.c:612-615`）**确实**校验了 `key_id`，但返回 `RC_E_PARAM` 时被穿透。`key_id=0` → `key_id-1u` 因 uint8_t 回绕成 255 → 对 `d->km.key[255]` 写入，**越界约 5.6KB**（`rc_key_t` 22 字节 × 255）。

**修复**：
```c
/* proto_rc.c:609 —— 入口自行校验，不依赖被调方 */
if ((key_id < 1u) || (key_id > (uint8_t)RC_KEY_COUNT)) { return RC_E_PARAM; }
rc = Keymap_BuildRename(...);
if (rc != RC_OK && rc != RC_E_LEN) { return rc; }   /* RC_E_PARAM 一律拒绝 */
```

---

### P2-6　USB EP2/EP3 共享 64 字节缓冲

**位置** `keyboard/usb_kbd_port_ch592.c:50,270-271`

```c
__attribute__((aligned(4))) static uint8_t s_ep23_buf[64];   // 只有 64 字节
pEP2_RAM_Addr = s_ep23_buf;
pEP3_RAM_Addr = s_ep23_buf;      // ← 两者指向同一块
```

SDK `CH59x_usbdev.h:30-40` 要求：
```c
extern uint8_t *pEP2_RAM_Addr;   // ep2_out(64) + ep2_in(64)
#define pEP2_IN_DataBuf  (pEP2_RAM_Addr + 64)
```

于是 `pEP2_IN_DataBuf` 和 `pEP3_IN_DataBuf` 都指向 `s_ep23_buf + 64` —— **缓冲区末尾之外**。

**当前 EP2/EP3 全 NAK 未触发**（`usb_kbd_port_ch592.c:210-214`），一旦启用就是越界 DMA。

```c
/* usb_kbd_port_ch592.c:50 —— 改成两块独立缓冲 */
__attribute__((aligned(4))) static uint8_t s_ep2_buf[64 + 64];
__attribute__((aligned(4))) static uint8_t s_ep3_buf[64 + 64];

/* :270-271 */
pEP2_RAM_Addr = s_ep2_buf;
pEP3_RAM_Addr = s_ep3_buf;
```

---


### P2-7　背光引脚功能被编译关闭（需确认硬件接法）

**位置** `hardware/tft_port.h:113-123` · `User/Main.c:196`

```c
// tft_port.h:114   引脚定义正确（PB4，与引脚文档一致）
#define TFT_PIN_BL   GPIO_Pin_4
// tft_port.h:122   但功能被关掉了
#define TFT_USE_BL   0

// Main.c:196       这行是空操作！
TPORT_Bl(1);       // → tft_port.c:165 (void)level; 什么都不做
```

引脚定义文档写「BLK→PB4（**高电平关闭**）」。当前：
- `TPORT_InitPins()` 不配置 PB4 为输出（`tft_port.c:95-98` 被 `#if (TFT_USE_BL)` 跳过）
- `TPORT_Bl()` 退化为空函数
- → **PB4 悬空**

**两个隐患**：
1. 若硬件靠外部上拉，屏幕背光将不受软件控制
2. **极性是"高电平关闭"，而 `TPORT_Bl(1)` 语义是"开"** —— 即使打开开关也是**反的**

**修复**：
```c
/* 第一步：确认硬件实际接法（万用表量 PB4 静态电平） */
#define TFT_USE_BL   1

/* 第二步：因极性为"高电平关闭"，TPORT_Bl 内部需取反 */
/* tft_port.c:158 —— 重命名为语义明确的 TPORT_BlOn */
void TPORT_BlOn(uint8_t on)
{
#if (TFT_USE_BL)
    /* BLK 高电平关闭 → on=1 时输出低 */
    if (on) { TFT_GPIO_CLR(TFT_PIN_BL_PORT, TFT_PIN_BL); }
    else    { TFT_GPIO_SET(TFT_PIN_BL_PORT, TFT_PIN_BL); }
#else
    (void)on;
#endif
}
```

> **注意**：若实测发现屏幕当前是亮的（靠外部电路），则**保持 `TFT_USE_BL=0` 也可**，但建议至少配置为输出并给出确定电平，避免悬空带来的功耗与干扰。

---

### P2-8　BLE 发送只判连接，不判加密/CCCD

**位置** `bluetooth/bt_wch.c:295-302`

```c
if ((report == 0) || (len < 8u) || (s_connected == false)) { return false; }
return BtGatt_SendReport(report);
```

`s_connected` 在 `GAPROLE_CONNECTED`（`:156`）就置位，此时**还没配对/加密**，CCCD 也可能未使能。不会崩（HidDev_Report 内部返回错误），但会让上层把发送失败误判为"通道故障"。另外 `len < 8u` 应为 `len != 8u`。

```c
/* bt_wch.c:295 */
if ((report == 0) || (len != 8u) || (s_connected == false)) { return false; }
if (!HidDev_GetReportCccd() || !link_is_encrypted()) { return false; }   /* 补充判断 */
```

---

### P2-9　死代码清理

| 位置 | 问题 |
|---|---|
| `bluetooth/bt_port.c` | 整层从未被调用（Main.c 只调 `BT_WCH_Init()`），`BT_SendHid()` 永远返回 false；`wch_state_cb`（`bt_wch.c:164`）传的 MAC 是全 0，导致 `BT_IsConnected()` 与 `BT_GetConnectedDevice()` 结论矛盾 → **建议删除整层** |
| `User/Main.c:53` | `g_ms_tick` 自增后从未被读取 —— 而它正是 P0-4 修复所需的时基（修复后会被使用） |
| `ui/ui_list.h:76,79` | 注释写 `UiList_WrapTo`，实际函数名 `UiList_OnEncoder` → 统一命名 |
| 9 个 `.cproject.bak-*` | 备份文件混在工程根目录 → 移出源码树（建议 `.bak/` 子目录或版本控制） |
| `hardware/lcd_test.c` | 独立的 LCD 测试程序，与主工程无调用关系 → 移出或加编译开关 |

---

## 6. 架构级优化

### 6.1 用 TMR0 中断统一驱动（与 P0-4 同一改造）

目标形态：

```c
/* ============ 中断：只做采样与置标志，不做协议处理 ============ */
void TMR0_IRQHandler(void)
{
    if (TMR0_GetITFlag(TMR0_3_IT_CYC_END) != 0u)
    {
        TMR0_ClearITFlag(TMR0_3_IT_CYC_END);
        ENC_Scan();                      // 1ms 采样
        g_ms_tick++;
        s_flags |= FLAG_1MS;             // 通知主循环
    }
}

/* ============ 主循环：只做协议与绘制，永不 Delay ============ */
int main(void)
{
    /* ... 初始化 ... */
    for (;;)
    {
        if (s_flags & FLAG_1MS) { s_flags &= ~FLAG_1MS; Game_1ms(); }

        BT_WCH_Process();                // BLE 协议栈有内部节拍要求
        UsbKbd_DeviceTask();
        UiKbdCh_Task();
        UiRc_Task();
        UI_Task();

        __WFI();                         // 空闲休眠，等中断唤醒
    }
}
```

**收益**：
- 时间精确（不再依赖"主循环其他部分够快"）
- **功耗下降**（`__WFI` 让 MCU 空闲时自动进入低功耗）
- **BLE 不再被饿死**（不再有 16.4ms 的 RF 忙等）
- UI 刷新率稳定

> **前提**：P0-3 的 `halrf_wait_done` 必须先改成非阻塞（§3 P0-3 第三步），否则 `Rc_HostSuperframe` 仍在主循环里阻塞 16ms。

---

### 6.2 UI 全屏重绘优化

#### 现状测算

| 项目 | 实测/推算 |
|---|---|
| 帧缓冲大小 | 240 × 135 × 2 B = **64 800 B** |
| SPI 分频 | `TFT_HW_SPI_DIV = 4` → 60/4 = **15 MHz** |
| 单帧传输时间 | 64800 × 8 / 15e6 ≈ **34.6 ms** |
| UI 帧周期设定 | `UI_FRAME_MS = 20` ms |
| **结论** | **理论上永远追不上**，实际按键时界面明显滞后 |

#### 优化一：提高 SPI 频率（最简单，收益最大）

```c
/* tft_port.h:50 —— ST7789 允许 62MHz，60MHz 系统下 DIV=2 即 30MHz，留足裕量 */
#define TFT_HW_SPI_DIV    2      // 30MHz（原 4 → 15MHz）
```

> ※ `CH59x_spi0.c:42-53` `SPI0_CLKCfg()`：当 `c == 2` 时会额外置 `RB_SPI_MST_DLY_EN`（自动插入延时）。**改后必须实测确认 ST7789 能稳定接收**；若出现花屏，退回 DIV=3。

**收益**：帧时间 34.6ms → **17.3ms**，直接满足 20ms 帧周期。

#### 优化二：引入脏矩形

当前每个页面 `draw()` 开头都是全屏填充：
```c
TFT_FillRect(0, 0, 240, 135, UI_COL_BG);      // 每帧 35ms
```

应改为按区域独立脏标记。以键盘页为例，它有 4 个独立变化的区域：

```c
/* ui_kbd.c —— 拆分独立脏标记 */
typedef struct {
    uint8_t map;         /* 3x3 网格 + 左侧按键文本 */
    uint8_t channel;     /* 右上角传输方式 */
    uint8_t conn;        /* 右侧中间连接状态 */
    uint8_t bat;         /* 顶部电量条 */
} kbd_dirty_t;

static void kbd_update(uint16_t dt_ms)
{
    kbd_dirty_t *d = &s_kbd_dirty;

    if (KEY_GetMap()       != s_shown_map)  { s_shown_map  = KEY_GetMap();        d->map     = 1u; }
    if (UiKbd_Channel()    != s_shown_ch)   { s_shown_ch   = UiKbd_Channel();     d->channel = 1u; }
    if (UiKbd_Connected()  != s_shown_conn) { s_shown_conn = UiKbd_Connected() ? 1u : 0u; d->conn = 1u; }
    if (UiW_BatPercent()   != s_shown_bat)  { s_shown_bat  = UiW_BatPercent();    d->bat     = 1u; }

    if (d->map | d->channel | d->conn | d->bat) { UI_Dirty(); }
}

/* kbd_draw —— 只重绘变化区域 */
static void kbd_draw(void)
{
    kbd_dirty_t *d = &s_kbd_dirty;

    if (d->bat)     { UiW_DrawBatBar(); }                             /* 30×20 ≈ 0.15ms */
    if (d->channel) { kbd_draw_center_y(45, UiKbd_ChannelText(), ...); } /* 50×50 ≈ 0.13ms */
    if (d->conn)    { kbd_draw_center_y(76, UiKbd_StateText(), ...); }
    if (d->map)     { UiW_DrawKeyGrid(...); kbd_draw_left_y(77, ...); }
}
```

**收益**：稳态（无变化）**零重绘**；单区域更新时从 35ms 降到 0.15ms 以下。

#### 优化三：字库绘制优化

```c
// game_port.c:313-325   Game_DrawGlyph 逐像素绘制 16x16
TFT_DrawBitmap(x, y, game_font16_data[g], GAME_GLYPH_W, GAME_GLYPH_H, fg);
```

列表项每行都画 16px 高的中文字形，每个字形要写 256 个像素的 2 字节。ASCII 部分已有 5×7 缩放（`Game_DrawAscii`），**列表项若能用 ASCII 就优先用**。

---

### 6.3 WS2812 改用 DMA 或 SPI

当前 bit-bang + 关中断方案，即使修好 P0-6 仍有两处硬伤：
- 占用 `PWM9`（PB7）+ 216 次忙等
- 必须关中断 0.3ms

**改进路径**（按投入递增）：

1. **最小改动**：用 TMR 定时器替代忙等，把 `RGB_Show()` 放进定时器中断
2. **中等改动**：配置 SPI 移位输出（CH592 的 SPI 可连到 WS2812 的 DIN），用 DMA 发送 9×3=27 字节
3. **彻底方案**：WS2812 数据寄存器双缓冲（若驱动支持 DMA + 双缓冲，可完全零 CPU）

---

### 6.4 编译优化等级：`-Os` → `-O2`

**这是当前性价比最高的一项优化。**

`.cproject` 实测：
```
optimization.level = ...option.optimization.level.size   →  -Os
```

`-Os` 优先压缩体积。这解释了为什么：
- Flash 只用了 43%（`-Os` 的功劳）
- **但代码跑得比预期慢**（`-Os` 牺牲了性能）

而 **Flash 还有 253.9KB 空闲** —— 完全有空间换性能。

```xml
<!-- .cproject -->
<!-- 改前 -->
<option ...optimization.level... value="...optimization.level.size"/>
<!-- 改后 -->
<option ...optimization.level... value="...optimization.level.2"/>
```

**预期收益**：纯计算代码（游戏逻辑、协议解析、CRC）性能提升 20~50%。

**代价**：`.text` 增大 10~20%（约 +20~40KB），**仍在 448KB 内**。

> ※ **改动后必须重新测量** `obj/CH592X.map` 确认 Flash 占用，并回归验证 P0-4（`-O2` 暴露 `volatile` 缺失的效果会更明显，见 P2-2）。

---

### 6.5 配置持久化统一化

当前配置**分散在三个模块的独立 RAM**：

| 模块 | 变量 | 持久化方式 |
|---|---|---|
| `ui_store.c` | `s_cfg` | √ Flash `0x0300` |
| `key_matrix.c` | `s_map[4][9]` | × **仅 RAM，重启丢失** |
| `rgb_led.c` | `s_bright` | × 仅 RAM（从 `s_cfg.led_bright` 恢复） |

**问题**：`KEY_SetMapValue()`（`ui_pages2.c:258`）改的键值**只存在 RAM**，断电即丢。需求 **(五).4.b** 提到"所有新建的键盘映射"，隐含这些映射应当持久。

**建议**：定义统一的配置事务

```c
/* 新增 ui/ui_config.h */
void App_ConfigLoad(void);       /* 启动时：Flash → RAM，应用到各模块 */
uint8_t App_ConfigSave(void);    /* 修改时：RAM → Flash（去抖后写） */
void App_ApplyDefaults(void);    /* 恢复默认：重置三个模块的 RAM + 落盘 */
```

```c
/* ui_config.c */
void App_ConfigLoad(void)
{
    (void)UiStore_Load();
    ui_cfg_t *c = UiStore_Cfg();

    RGB_Switch(c->led_on);
    RGB_SetBrightness(c->led_bright);
    KEY_LoadMap();                  /* 新增：从 Flash 恢复自定义映射 */
    KEY_SetMapGroup(c->map_group);
}

void App_ApplyDefaults(void)       /* 修 P1-3 的根本做法 */
{
    UiStore_Default();
    KEY_ResetMap();                 /* 别忘了这个 */
    RGB_SetBrightness(60u);

    ui_cfg_t *c = UiStore_Cfg();
    RGB_Switch(c->led_on);
    (void)UiStore_Save();
}
```

**同时解决三处隐患**：
- P1-3（恢复默认漏 `KEY_ResetMap`）—— 从"记得调用"变成"框架保证"
- 自定义映射不持久
- 配置项散落各处易漏改

> **另需加写保护**：亮度调节页每次旋转都调 `UiStore_Save()`（`ui_pages.c:342`），会高频擦写 Flash。建议加 500ms 去抖，或改为"退出页面时保存"。

---

### 6.6 栈容量调整

见 §1.3②。实测 `__stack_size = 512` 偏小，而工程存在 `TFT_Printf → vsnprintf` 等深调用。

```ld
/* Ld/Link.ld:3 */
-__stack_size = 512;
+__stack_size = 1024;      /* 512 → 1024，代价是堆从 5032 降到 4528 */
```

**代价评估**：堆减少 512B。由于 §1.3① 已确认堆实际有 5032B 且有让位设计保护，这个交换是划算的。

> 调整后应实测最坏情况的栈深度（可在 `Game_Task` 入口读栈指针做水位监测）。

---

### 6.7 死代码与工程整理

| 项目 | 建议 |
|---|---|
| `bluetooth/bt_port.c`（12KB） | 删除整层（见 P2-9） |
| `hardware/lcd_test.c/.h` | 移出主编译目标 |
| `games/main_example.txt` | 归档到 `doc/` |
| 9 个 `.cproject.bak-*` + `CH592X.wvproj.bak-*` | 移入 `.bak/` 或纳入版本控制 |
| `obj/` 编译产物 | 加入 `.gitignore`（若用 Git） |
| `ui/_NEXT.md` / `_STATUS.md` | 更新或标注完成状态，避免误导后续维护 |

---

## 7. 验证与回归清单

### 7.1 每阶段必做验证

**阶段一完成后**
- [ ] 进入键盘页，**等待 3 秒以上**（超 0.5s+1.5s 门槛），手机能扫描并连接 BLE
- [ ] 2.4G 键盘连续发送 100 次按键，**无丢键**（用接收端统计）
- [ ] 核对跳频表：确认发送端与接收端 `link_hop.c` 完全一致
- [ ] 用 WCH-Link 保存并回读 Flash 0x0100 / 0x0200 / 0x0400 段，确认 KB 存盘不破坏 RC 记录
- [ ] 万用表量 PB4，确认背光电平符合预期

**阶段二完成后（最关键）**
- [ ] **长按编码器，实测 3 秒是否真的 3 秒退出主页面**（用手机秒表对照）
- [ ] 单击编码器，300ms 窗口内可靠识别
- [ ] 快速连按矩阵键，**无重复触发、无丢键**（去抖验证）
- [ ] 同时按 K1+K2，组合键能可靠识别
- [ ] 观察 TMR0 是否丢拍（可在 ISR 里累加计数与主循环读数比对）
- [ ] 遥控模式持续运行 5 分钟，**设备列表不反复清空**
- [ ] 进入遥控页时操作游戏页面，界面**不卡死**
- [ ] 电量显示每 5 分钟更新，**刷新时界面无卡顿**
- [ ] 测量 `obj/CH592X.map` 确认 `.text` 增长在预算内

**阶段三完成后**
- [ ] 雷霆战机：按住 K4 只下移、**不开火**；按住 K6 只开火
- [ ] 跑酷：按住 K7 持续冲刺
- [ ] 恢复默认设置后，自定义映射**确实回到初始值**
- [ ] 映射组共 5 组，**第 5 组可正常选中并编辑**
- [ ] 键盘页右侧面板为 50px（用尺子量或截图核对）
- [ ] 列表页按 K5 能否正常确认
- [ ] 退出游戏后无残留画面
- [ ] 编码器单击/双击/长按/长按1s/长按3s 五种手势行为正确

### 7.2 建议补充的自动化检查

工程已有 `GAME_STATE_TEST` 条件编译（`game_state.c:137` `Game_PoolCheck()`），可扩展为开机自检：

```c
/* 开机自检：内存池边界 + Flash 配置有效性 */
static void App_SelfTest(void)
{
#ifdef GAME_STATE_TEST
    if (!Game_PoolCheck())
    {
        /* 内存池越界 → 停机或降级，不进入正常流程 */
        UiRc_SetDeviceCount(0);
    }
#endif
    /* 校验 Flash 各段 magic，不合法则重置为默认值 */
    (void)RfCfg_KbLoad(&s_kbcfg);
    (void)RfCfg_RcLoad(&s_rccfg);
}
```

### 7.3 调试建议

| 工具 | 用途 |
|---|---|
| **逻辑分析仪** | 量 PB7（WS2812 时序）、PB13（SPI SCK，验证 DIV=2 后是否稳定） |
| **示波器** | 量 PB14 使能后 PA7 的分压稳定时间（标定 `BAT_EN_SETTLE_MS`） |
| **WCH-Link + RISC-V 调试器** | 打断点验证 `Game_Millis()` 与真实时间是否一致 |
| `obj/CH592X.map` | 每次改动后对比 `.text` / `.bss` / `_ebss` 变化 |

---

## 8. 附录：本次修正的两处误判

为避免后续维护时误用，此处记录两处**与初版分析不同**的实测结论。

### 8.1 Flash 余量修正：不是"紧张"，而是"充裕"

| 项目 | 初版推测 | **实测** |
|---|---|---|
| Flash 已用 | 接近上限 | **194.1 KB / 448 KB = 43.3%** |
| Flash 剩余 | 需谨慎 | **253.9 KB** |
| OCV 表 404B | 占用可观，建议压缩 | **仅占剩余量 0.16%，可完整保留** |
| 优化等级假设 | 未知 | **`-Os`（size），非 `-O2`** |
| 建议 | 省空间 | **改 `-O2` 换性能，空间足够** |

**结论**：Flash 完全不是瓶颈，`-O2` 是"免费"的性能提升（§6.4）。**OCV 表可以保留完整 101 项，不必为了省空间而降低精度。**

### 8.2 堆容量修正：实际 5032B，比注释宽裕

`Main.c:88-92` 注释称堆约 2680B / 2960B，**实测为 5032B**（`_ebss`=0x20005258 至 `_heap_end`=0x20006600）。

**含义**：
- `Main.c:101` 的 `sizeof(rc_host_t)+64 < 4096` 约束比真实上限宽松，**不是真正的护栏**
- 遥控（~2.8KB）与游戏池**不能同时占用** → `Main.c` 的"进遥控页让出堆、退出时还回"设计**是正确且必要的**，务必保留
- 调整栈到 1024 后堆仍有 4528B，交换划算（§6.6）

---

## 变更记录

| 版本 | 日期 | 内容 |
|---|---|---|
| v1.0 | 2026-10-07 | 初版：6×P0 + 9×P1 + 7×P2，含实测资源基线与三阶段修复路线 |

> **本次仅新增本文档，未修改工程内任何源文件；`CH592X引脚定义.txt` 与 `键盘功能实现.txt` 保持原样。**
