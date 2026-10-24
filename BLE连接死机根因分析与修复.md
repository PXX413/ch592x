# BLE 连接即死机 —— 根因分析与修复报告

> 分析对象：`C:\Users\a\Desktop\单片机开发\ch592x`（CH592X 多功能键盘/遥控器工程）
> 对照参考：`C:\Users\a\Desktop\AI\sys\CH592EVT\EVT\EXAM\BLE\HID_Keyboard`（官方例程）
> 结论来源：源码比对 + 官方例程比对 + `libCH59xBLE.a` 反汇编 + 编译产物二进制校验
> 固件规模：FLASH 284832 B / 448 KB（62.09%），RAM 24896 B / 26 KB（93.51%）

---

## 一、结论（先看这里）

**根因是 `bluetooth/bt_gatt.c` 里的 HID 报告回调 `reportCB` 被填成了 `NULL`。**

手机连上设备后，配对一完成就会去写"输入报告"的 CCCD（订阅通知）——
这一步在官方 `hiddev.c` 里是**不判空**直接调用 `(*pHidDevCB->reportCB)(...)` 的。
`reportCB == 0` 时这条指令就是 `jalr 0`，而 **flash 地址 0 处是 `_sinit: j handle_reset`**，
CPU 直接跳回**复位入口** —— 表现就是"手机连接此设备时，设备死机"。

> 电话再重连一次，就再死一次 —— 所以看起来像"设备彻底挂了"。

修复后：`reportCB` 指向真实实现 `gatt_hid_rpt_cb`，三处调用点另加判空保护。

---

## 二、证据链

### 证据 1：官方 hiddev.c 有三处不判空地调用 reportCB

`bluetooth/hiddev.c`（官方文件，本工程未改过其调用逻辑）：

| 行号 | 所在函数 | 触发时机 | 是否判空 |
|---|---|---|---|
| 471 | `HidDev_ReadAttrCB()` | 主机**读** Report / Boot 报告特征 | × 只判 `pRpt`，不判 `reportCB` |
| 566 | `HidDev_WriteAttrCB()` | 主机**写** LED 输出报告 | × 同上 |
| 605 | `HidDev_WriteAttrCB()` | 主机**写 CCCD 订阅通知** | × 同上 |

对比另外两个回调：`pfnStateChange`（821 行）和 `passcodeCB`（897 行）**都有判空**——
只有 `reportCB` 这一路被漏掉了，所以"填 0 也行"的直觉在这里不成立。

### 证据 2：CCCD 写是手机连接后的必经动作（且必然发生在配对之后）

`bluetooth/hidkbdservice.c` 的输入报告 CCCD 权限：

```c
{ATT_BT_UUID_SIZE, clientCharCfgUUID},
GATT_PERMIT_READ | GATT_PERMIT_ENCRYPT_WRITE,     /* ← 要求加密 */
```

`hidRptMap[0].cccdHandle = hidAttrTbl[HID_REPORT_KEY_IN_CCCD_IDX].handle;`（`Hid_AddService()` 内回填），
所以主机写 CCCD 时 `hidDevRptByCccdHandle()` **一定能查到**（`p->cccdHandle == handle` 即命中），
接着就走 605 行 → `(*reportCB)(...)` → 崩。

时序上正好对应"连接 → 配对 → 订阅 → 死机"：因为 CCCD 要求加密写，主机必须先配对成功才能订阅，
所以死机点必然出现在**配对完成后的第一瞬间**。

### 证据 3：编译产物里确实是空指针，而地址 0 指向复位入口

从修复前的 `obj/CH592X.elf` 里直接读 `s_hid_cbs`（0x20002264 起，16 字节）：

```
20002264 00000000   ← reportCB    = NULL   ★
20002268 0000fdbc   ← evtCB       = gatt_hid_evt_cb（非空）
2000226c 00000000   ← passcodeCB  = NULL（hiddev 内部已判空，安全）
20002270 00000000   ← pfnStateChange = NULL（hiddev 内部已判空，安全）
```

地址 0 处的内容（`objdump -d`）：

```asm
00000000 <_sinit>:
   0:  20c0206f      j  220c <handle_reset>      /* ← 跳回复位入口 */
```

即：`jalr 0` → 复位初始化代码 → 固件重来一遍，用户看到的就是"死机 / 卡死"。

修复后的同一段内存（实测）：

```
20002264 d0fd0000   ← reportCB    = gatt_hid_rpt_cb（0xFDD0）★ 已修
20002268 cefd0000   ← evtCB       = gatt_hid_evt_cb
2000226c 00000000   ← passcodeCB  = 0（安全）
20002270 94000100   ← pfnStateChange = BT_WCH_OnGapState（0x10094）
```

---

## 三、第二个缺陷：就算不死机，BLE 键盘也发不出一个字

排查过程中同时确认了第二个结构性问题（属于"BLE 不能正常使用"的另一半）：

**`bt_wch.c` 自己调了 `GAPRole_PeripheralStartDevice()`，把 hiddev 的 GAPRole 回调顶掉了。**

反汇编 `libCH59xBLE.a` 的 `GAPRole_PeripheralStartDevice()`（0x2A6F6）可见：

```asm
2a702:  beqz a1, 2a70e            ; if (pBondCB) GAPBondMgr_PeripheralRegister(pBondCB)
2a708:  jal  GAPBondMgr_PeripheralRegister
2a718:  bnez a5, 2a754            ; if (gapRole_state != INIT) → return 0x11（什么都不注册）
2a71c:  sw   a2, pGapRolesPeri_AppCGs   ; 只在 INIT 时才记下 pAppCallbacks
```

而官方架构（`HID_Keyboard\APP\hidkbd_main.c`）是：

```
HidDev_Init()  →  tmos_set_event(START_DEVICE_EVT)
            ...等 TMOS 跑起来...
HidDev_ProcessEvent() → GAPRole_PeripheralStartDevice(hidDevTaskId, &hidDevBondCB, &hidDev_PeripheralCBs)
```

即**启动设备由 hiddev 自己完成**。本工程却在 `BT_WCH_Init()` 里**同步**抢着调了一次——
`GAPRole_PeripheralInit()` 刚把 `gapRole_state` 清成 INIT，所以这一枪**必然抢先**，
记下的是本文件的回调；hiddev 稍后再调就撞上 `state != INIT`，直接返回 0x11，什么都没注册。

后果（hiddev 的三个内部变量全部停在初始值）：

| 变量 | 后果 |
|---|---|
| `hidDevGapState` 恒为 `GAPROLE_INIT` | `HidDev_Report()` 永远返回 `bleNotReady` → **BLE 报文一条也发不出去** |
| `gapConnHandle` 恒为 0 | 就算能发，也会发到句柄 0 |
| `hidDevConnSecure` 恒为 FALSE | 同上，且 `/hidDev_Report` 还会误走"重新广播"分支 |

`HidDev_Report()` 源码（`hiddev.c`）：

```c
if(hidDevGapState == GAPROLE_CONNECTED) {
    if(hidDevConnSecure) { return hidDevSendReport(...); }
}
```

`hidDevGapState` 只在 `hidDevGapStateCB()` 里更新，而它只在 hiddev 持有 GAPRole 回调时才会被调用。

**修复前的反汇编佐证**：全固件只有 1 处调用 `GAPRole_PeripheralStartDevice`，修复后这唯一一处落在
`HidDev_ProcessEvent`（0x10648），且传入 `hidDevTaskId` / `&hidDevBondCB` / `&hidDev_PeripheralCBs`
—— 与官方例程完全一致。

---

## 四、修复清单

| 文件 | 改动 | 说明 |
|---|---|---|
| `bluetooth/bt_gatt.c` | 新增 `gatt_hid_rpt_cb()` 实现 | READ：回当前 8 字节 boot 报文（源自 `BT_HID_Report()`，长度按对端上限钳位）；WRITE：`Hid_SetParameter()` 存 LED/Feature；ENABLE/DISABLE：交给已有订阅记录 |
| `bluetooth/bt_gatt.c` | `s_hid_cbs.reportCB = gatt_hid_rpt_cb` | **死机根因修复** |
| `bluetooth/bt_gatt.c` | `s_hid_cbs.pfnStateChange = BT_WCH_OnGapState` | 应用层仍能收到 GAP 状态（签名与原来 `gapRolesCBs_t.pfnStateChange` 一致） |
| `bluetooth/bt_gatt.c` | `#include "bt_hid.h"` | `BT_HID_Report()` |
| `bluetooth/bt_wch.c` | **删除** `BT_WCH_Init()` 里的 `GAPRole_PeripheralStartDevice(...)` | 交回 hiddev 持有，官方架构 |
| `bluetooth/bt_wch.c` | 新增 `BT_WCH_OnGapState()` | 转发给原 `wch_state_cb()`（应用层状态入口） |
| `bluetooth/bt_wch.c` | 删除已无用的 `s_role_cbs` / `s_bond_cbs` / `wch_rssi_cb` | 避免 `-Wunused` 警告 |
| `bluetooth/hiddev.c` | 471 / 566 / 605 三处 `reportCB` 调用加判空 | 加固，防止同类空指针再犯（判空后行为与官方一致） |

未改动：`RF_RoleInit()` 相关的射频交接逻辑（当前能搜到、能连上，不动它；见第六节风险）。

---

## 五、编译与验证结果

编译（MounRiver 工具链 `riscv32-wch-elf-gcc 15.2.0`，`obj/` 下 `make all`）：

```
Memory region         Used Size  Region Size  %age Used
           FLASH:      284832 B       448 KB     62.09%
             RAM:       24896 B        26 KB     93.51%
   text    data   bss    dec     hex   filename
 282768    2064 14120 298952  48fc8   CH592X.elf
```

> RAM 比修复前少 24 B（删掉的 `s_role_cbs`/`s_bond_cbs`），无新增警告。

二进制校验（`objdump -s -j .data`）：`s_hid_cbs.reportCB` = `0x0000FDD0`（`gatt_hid_rpt_cb`）√
反汇编校验：全固件 `GAPRole_PeripheralStartDevice` 仅 1 处调用，位于 `HidDev_ProcessEvent` √

---

## 六、上机验证步骤

**前提：必须拔掉 USB 线。** 传输仲裁在 USB 插入时会锁在 USB 档，BLE 按设计就不工作。

1. 烧 `obj/CH592X.hex`；
2. 拔 USB，进键盘页（`kbd` / `kbset` / `keyedit` 任意一个），等约 2 秒让仲裁走到 BLE 档；
3. 手机蓝牙搜索 `CH592X` → 连接 → **预期：不再死机**（配对时手机可能弹确认）；
4. 手机端应出现 HID 键盘（部分机型在"已配对的设备"里显示为输入设备）；
5. 按键 → 手机应能收到输入；
6. 断开再连一次，确认可重复。

**若仍异常**，按现象分流：

| 现象 | 下一步查 |
|---|---|
| 还是死机 | 看屏上是否有 `HARDFAULT` 页（PC / CA / ST / CN / SK）；`SK` 接近 1024 说明栈不够 |
| 能连上但按键无输入 | 在 `wch_state_cb` 的 `GAPROLE_CONNECTED` 分支加打点，确认 `s_connected` 变了；再看 `HidDev_IsReportSubscribed()` 是否为 1（CCCD 是否真的订阅上） |
| 搜不到设备 | 与本问题无关，回到射频仲裁链（`BT_WCH_RestorePhy()` / `want_bt_phy()`） |

---

## 七、遗留风险（本次未改，建议后续处理）

1. **`BT_WCH_RestorePhy()` 里运行时调 `RF_RoleInit()`**
   你自己在 2026-10-12 的实测记录里写过"运行期调它会破坏协议栈内部状态"。
   目前的时序恰好安全（它跑在"广播使能消息被 TMOS 处理"之前，之后广播还能起来，所以现在能搜到、能连），
   但它是在 2.4G ←→ BLE 每次切换时都会执行的初始化期 API，属于**结构性问题**。
   路线建议：让 2.4G 一侧在 BLE 拥有射频时**完全不写 LLE**（`HALRF_BtOwnsPhy(true)` 已挡住大部分路径），
   交还时只靠"重启广播"让协议栈自己重配射频，然后**单独做一轮实验**再决定是否删掉 `RF_RoleInit()`。

2. **`hidDevConnSecure` 门槛**：官方要求"已连接 **且** 已加密"才允许发报文。
   因为本工程的 CCCD 是 `ENCRYPT_WRITE`，主机必然先配对，所以正常。但若将来把权限放开成非加密订阅，
   这个门槛会让报文静默发不出去（此时需要放宽 `HidDev_Report()` 的判断）。

3. **栈余量仍然偏紧**：`.stack = 1024 B`，而"配对 → 写绑定信息到 Flash"这条链的静态栈深估算已接近 800 B
   （`main` 96 → `BT_WCH_Process` 16 → `TMOS_SystemProcess` 16 → `GAPBondMgr_ProcessEvent` 48 →
   `GAPBondMgr_ProcessGAPMsg` 96 → `gapBondMgrAddBond` 96 → `tmos_snv_write` 32 →
   `Lib_Write_Flash` 32 → `Lib_Write_Flash_592A` 288 → `FLASH_EEPROM_CMD` 64 → `FLASH_ROM_WAIT` 32），
   且这段代码**不关中断**，期间 BLE 中断（`LLE_IRQHandler` 光保存现场就要 128 B）可以随时嵌套。
   好消息是栈溢出首先踩的是 1704 B 的堆区（键盘/遥控模式下不用 `malloc`），到不了 `.bss`，
   所以表现不会太诡异；但建议把 `__stack_size` 提到 1280~1536 再做长期观察。

4. **`keyboard/bt_hid.c` 的 `BT_HID_Flush()` 目前是死路**：
   它内部走 `BT_SendHid()`（`bt_port.c` 的后端），而全工程从未调用 `BT_Init()`，`s_be` 恒为 0 ——
   所以 `BT_HID_Flush()` **永远返回 false，`s_dirty` 永远清不掉**。
   实际发报文的路径是 `ui_kbd_ch.c: ch_ble_send()` 直接调 `BT_WCH_SendHid()`，功能上没问题，
   但 `BT_HID_IsDirty()` 恒为真，将来谁依赖它就会踩坑。建议要么注册后端（`BT_Init()`），
   要么把 `BT_HID_Flush()` 的返回值与 `s_dirty` 语义改成"只算报文、不管发送"。

---

## 八、顺带纠正一个错误结论

`CH592X_交接说明.md` 里"★★★ 蓝牙排查的结局"第 4 条写着：

> `BLE_SNV_ADDR` 算错，落在程序区里：`0x77000-FLASH_ROM_MAX_SIZE` = `0x7000` = 28KB —— 在代码段里，
> 存配对信息时会擦掉代码。

**这条是误判，不用担心。** 依据 `StdPeriphDriver/inc/ISP592.h`：

```c
#define EEPROM_MAX_SIZE   0x8000     // Data-Flash maximum size, 32KB
#define EEPROM_BLOCK_SIZE 4096
```

`EEPROM_*` 系列（`MCU.c` 里的读写回调用的就是它）操作的是**独立的 Data-Flash 地址空间**，
与代码 Flash（`FLASH_ROM_*`，0x00000~0x6FFFF）不是同一片区域。
`BLE_SNV_ADDR = 0x7000` 正是 32 KB Data-Flash 的最后一个 4 KB 块，官方 SDK 自己的越界检查
（`MCU.c`：`BLE_SNV_ADDR + BLOCK*NUM > (0x78000 - FLASH_ROM_MAX_SIZE)` 即 `> 0x8000` 报警）也印证了这个范围。
工程自己的数据区只用到 `0x0000~0x05FF`（game_score / rf_cfg / ui_store / kmap），**不冲突**。

---

## 九、一句话总结

**"手机一连就死机"= `reportCB` 空指针被官方 HID profile 无条件调用，跳到了地址 0 的复位入口；
"BLE 键盘打不出字"= 应用层抢了 `GAPRole_PeripheralStartDevice()`，把 hiddev 的状态机冻在 INIT。**
两处都已按官方 `HID_Keyboard` 例程的架构修好，编译通过，待上机确认。
