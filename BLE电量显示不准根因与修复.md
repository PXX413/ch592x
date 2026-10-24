# 电脑上显示的电量不准 —— 根因分析与修复报告

> 分析对象：`C:\Users\a\Desktop\单片机开发\ch592x`
> 对照参考：`C:\Users\a\Desktop\AI\sys\CH592EVT\EVT\EXAM\BLE\HID_Keyboard`（官方例程）
> 结论来源：源码比对 + 编译产物反汇编验证
> 编译结果：FLASH 284956 B / 448 KB（62.12%），RAM 24896 B / 26 KB（93.51%）

---

## 一、结论（先看这里）

**电脑显示的电量不是电池的真实电量，而是一个写死的假值 —— 这是"从应用层到 BLE 电池服务"这条路整条断掉造成的。**

拆开是四个独立问题，缺一个都不会准（本工程四个全中）：

| # | 问题 | 位置 | 后果 |
|---|---|---|---|
| 1 | **官方量测函数是空壳**：`battMeasure()` 里 `adc = 300;` 写死，注释写着 "Configure ADC and perform a read"，**其实一行 ADC 都没读** | `bluetooth/battservice.c` | 按默认区间 [273,409] 换算 → **恒定 20%** |
| 2 | **"只降不升"**：读电量特征时会拿上面那个假值把 `battLevel` 往下拉：`if(level < battLevel) battLevel = level;` | `battservice.c: battReadAttrCB()` | 主机**读一次**电量特征（Windows 一连上就会读）→ battLevel 从 100 掉到 20，此后**充电也涨不回去** |
| 3 | **`Batt_SetParameter()` 根本没有 `BATT_PARAM_LEVEL` 分支**（它落进 `default` 返回 `INVALIDPARAMETER`） | `battservice.c` | `BtGatt_SetBatteryLevel()` 是个**空操作**，想推真实电量也推不进去 |
| 4 | **全工程从来没人调用** `BtGatt_SetBatteryLevel()` | 全工程 grep | 应用侧**已经标定过**的真实电量（`hardware/battery.c` → `BAT_GetLastPercent()`）与 BLE 服务完全脱节 |

> 所以屏上（应用 UI）显示的是**真实电量**（标定过的），电脑上显示的是 **20% 的假值** —— 两边对不上，就是"不准"的直接原因。

---

## 二、证据链

### 证据 1：官方量测空壳（battservice.c）

```c
static uint8_t battMeasure(void)
{
    uint16_t adc;
    uint8_t  percent;
    ...
    // Configure ADC and perform a read
    adc = 300;                 /* ← 写死，从不采样 */
    ...
}
```
默认区间（`Batt_Setup()` 从未被调用，所以是默认值）：
```c
static uint16_t battMinLevel = BATT_ADC_LEVEL_2V;   /* = 273 */
static uint16_t battMaxLevel = BATT_ADC_LEVEL_3V;   /* = 409 */
```
换算：`range = (409-273+1)>>2 = 34`，
`percent = ((300-273)*25 + (34-1)) / 34 = 708/34 = 20` → **恒定 20%**。

### 证据 2：读取特征时会把假值写进 battLevel（且只降不升）

```c
if(uuid == BATT_LEVEL_UUID)
{
    uint8_t level;
    level = battMeasure();        /* → 20 */
    if(level < battLevel)         /* 100 → 20 */
    {
        battLevel = level;
    }
    *pLen = 1;
    pValue[0] = battLevel;        /* 主机读到 20 */
}
```
主机（Windows / 手机）连接后会主动读一次 Battery Level 特征（0x2A19），
这一读就把值钉死在 20% —— 而且条件只有"小于"才更新，**之后充电升上去也不会刷新**。

### 证据 3：`BATT_PARAM_LEVEL` 分支不存在（编译产物反汇编）

修复前的 `Batt_SetParameter()`（0xFC?? 附近）只有 `BATT_PARAM_CRITICAL_LEVEL` 一个 case，
`param == 0`（= `BATT_PARAM_LEVEL`）直接落到 `default: ret = INVALIDPARAMETER`。

也就是说工程里这个"看起来对"的函数：

```c
void BtGatt_SetBatteryLevel(uint8_t percent)
{
    ...
    (void)Batt_SetParameter(BATT_PARAM_LEVEL, sizeof(percent), &percent);   /* ← 无效 */
}
```

**从来就是空操作**，无论传什么值都写不进 `battLevel`。

### 证据 4：应用侧电量与 BLE 侧完全脱节

- 应用侧：`ui/ui_widget.c` 的 `UiW_BatInit()`（开机同步读一次）+ `UiW_BatTask()`（每 60 s 非阻塞采一次）→
  `s_bat_pct = BAT_GetLastPercent()`，值来自 `hardware/battery.c` 的 **ADC + 分压比 + 两点标定 + OCV 曲线**，屏上电量条用它。
- BLE 侧：`BtGatt_SetBatteryLevel()` 的调用者 **0 个**（全工程 grep 只有定义与声明）。

修复后新增的调用点（反汇编已确认落在 `main` 里）：

```
000112fa <main>:
   ...
   115d8:  jal  fece <BtGatt_SetBatteryLevel>
```

---

## 三、修复清单

| 文件 | 改动 | 说明 |
|---|---|---|
| `bluetooth/battservice.h` | 新增 `typedef uint8_t (*battMeasureCB_t)(void);` 与 `Batt_SetMeasureCB()` 声明 | 给 profile 开一个"应用层电量来源"入口 |
| `bluetooth/battservice.c` | 新增 `battMeasureCB` 静态变量 + `Batt_SetMeasureCB()` 实现 | 未注册时行为与官方完全一致（方便对照例程） |
| `bluetooth/battservice.c` | `battMeasure()` 开头改为：**注册了应用回调就直接返回它**，否则走官方空壳 | 从根上消灭"恒定 20%" |
| `bluetooth/battservice.c` | `Batt_SetParameter()` 补上 `BATT_PARAM_LEVEL` 分支：**写值 + 变化时 `battNotifyLevel()`** | `BtGatt_SetBatteryLevel()` 从此真正生效 |
| `bluetooth/bt_gatt.c` | 新增 `s_batt_pct` 缓存 + `gatt_batt_measure()`；`BtGatt_Init()` 里 `Batt_SetMeasureCB(gatt_batt_measure)` | 主机读特征时返回应用推上来的值 |
| `bluetooth/bt_gatt.c` | `BtGatt_SetBatteryLevel()` 加**去抖**（值没变直接返回），新增 `BtGatt_GetBatteryLevel()` | 可以每圈放心调用，不会刷爆 BLE 连接事件 |
| `bluetooth/bt_gatt.c` | `gatt_batt_cb()` 补注释：说明为何不使用 profile 自带的周期测量 | 避免后人重复踩坑 |
| `bluetooth/bt_gatt.h` | 新增 `BtGatt_GetBatteryLevel()` 声明 | 诊断用 |
| `User/Main.c` | `#include "bt_gatt.h"`；主循环电量块后加 `BtGatt_SetBatteryLevel(UiW_BatPercent());` | 应用侧标定电量 → BLE 电池服务，每圈推送（内部去抖） |

**数据流（修复后）**：

```
VBAT(PA7 分压) ─ ADC ─ hardware/battery.c（分压比×2、836/1000 增益、+1mV、OCV 曲线）
                          │  UiW_BatTask()  每 60s 非阻塞采样
                          ├──> 屏上电量条（原本就有）
                          └──> BtGatt_SetBatteryLevel() ──> battLevel
                                                            ├─ 主机读 0x2A19 → 返回真值
                                                            └─ 值变化 → 通知（CCCD 订阅后）
```

---

## 四、编译与二进制验证

编译（`riscv32-wch-elf-gcc 15.2.0`，无警告）：

```
Memory region         Used Size  Region Size  %age Used
           FLASH:      284956 B       448 KB     62.12%
             RAM:       24896 B        26 KB     93.51%
```
> 相比修复前 FLASH +124 B，RAM 不变。

反汇编验证（逐条对应上面的修复）：

```asm
;; 1) battMeasure() 先看应用回调（非空就直接尾调用返回）—— 空壳(li a0,300)只在没注册时用
0000fafc <battMeasure>:
    fafc:  lw   a5,-772(gp)        # battMeasureCB
    fb00:  beqz a5,fb04
    fb02:  jr   a5                 # ← 应用层电量
    fb04:  ...                     # 官方空壳路径
    fb1c:  li   a0,300             # （仅作后备）

;; 2) Batt_SetParameter() 新增 param==0（BATT_PARAM_LEVEL）分支：写值 + 通知
0000fce4 <Batt_SetParameter>:
    fce8:  beqz a0,fcf8            # param == 0 → BATT_PARAM_LEVEL
    fcea:  li   a5,1; beq a0,a5,fd16   # param == 1 → CRITICAL_LEVEL
    fcf0:  li   a0,2; ret          # 其它 → INVALIDPARAMETER
   fcf8:  lbu  a5,0(a2)            # level = *value
    fcfc:  lbu  a4,battLevel
    fd00:  beq  a4,a5,fd26         # 值没变就跳过
    fd04:  sb   a5,battLevel       # battLevel = level
    fd10:  jal  linkDB_PerformFunc # battNotifyLevel()

;; 3) 注册点 & 推送点都在
    febc:  jal  fcde <Batt_SetMeasureCB>        （BtGatt_Init 内）
   115d8:  jal  fece <BtGatt_SetBatteryLevel>   （main 内，电量块之后）
```

---

## 五、上机验证步骤

1. 烧 `obj/CH592X.hex`；
2. 拔 USB，进键盘页，等约 2 秒；
3. 电脑/手机连接 `CH592X`；
4. **预期**：电脑"蓝牙和其他设备"里该设备的电量 = 屏上电量条的百分比（不再是固定的 20%）；
5. 等一次电量刷新（≤60 s）或换一块电压不同的电池再上电，电脑上的数字应跟着变（说明通知也通了）。

**判读表**：

| 现象 | 说明 |
|---|---|
| 电脑显示 = 屏上电量 | √ 修好了 |
| 电脑显示 100% 不变 | 说明 `battLevel` 没被推过 —— 确认固件是最新编译的（`BtGatt_GetBatteryLevel()` 可打点看） |
| 电脑显示 20% | 说明还在跑官方空壳 —— 检查 `Batt_SetMeasureCB()` 是否被调用、`BtGatt_Init()` 有没有执行 |
| 电脑显示 0% | 查 `hardware/battery.c` 的 ADC 标定（`UIW_BAT_SHOW_RAW=1` 在屏上看原始值） |

---

## 六、附带说明（想把电量做得更准可以看）

1. **本工程的"真实电量"精度取决于 `hardware/battery.c` 的标定**（分压比 `BAT_VBAT_MUL_NUM/DEN`、
   参考电压 `BAT_ADC_VREF_MV`、增益 `BAT_CAL_GAIN_NUM/DEN`、偏移 `BAT_CAL_OFFSET_MV`、OCV 表）。
   单节锂电的开路电压 → 电量曲线本来就非线性，`battery_ocv_table` 越贴合实际电芯越准；
2. **刷新节奏**：应用侧 60 s 一次（`UIW_BAT_REFRESH_MS`），且 `BAT_EN_SETTLE_MS = 500 ms` 才读数。
   所以插拔/换电池后电脑上最多滞后 1 分钟 —— 这是设计取舍，不是 bug；
3. **充电时的读数**：若在充电中测（VBAT 带充电电压），OCV 映射会偏高（显示虚高），
   这是单点电压法的固有限制，需要的话可加"检测到 USB 插入就不更新/单独映射"的策略；
4. **通知不必等 60 s**：`BtGatt_SetBatteryLevel()` 只在百分比变化时才发通知，
   若想更灵敏，把 `UIW_BAT_REFRESH_MS` 调小即可（代价是更频繁地打开分压使能采样）。

---

## 七、一句话总结

**电脑上显示的不是假数据源就是没接上：官方 `battMeasure()` 写死 20%、`Batt_SetParameter()` 少了 `BATT_PARAM_LEVEL` 分支、
应用层标定好的真实电量一次都没推给 BLE。**
三处已全部修好（新增应用电量回调 + 补齐参数分支 + 主循环每圈去抖推送），编译与反汇编均已验证，待上机确认。
