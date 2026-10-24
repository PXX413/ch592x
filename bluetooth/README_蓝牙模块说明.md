# CH592X 蓝牙模块（键盘模式 / 宿主白名单）

## 文件

| 文件 | 说明 |
|------|------|
| `bt_port.c/.h` | 蓝牙公共层：设备表（最多 8 个，自动去重刷新 RSSI）、扫描状态机、编码器选择、**宿主白名单**、事件队列、内置模拟后端 |
| `bt_hid.c/.h` | 标准 BLE HID 键盘的 8 字节 boot 报文：K1~K9 → 数字 1~9，编码器正/反转 → 音量 ±，编码器按下 → 回车；支持修饰键、最多 6 键同时 |
| `bt_screen.c/.h` | 键盘屏上的"附近蓝牙设备"界面：一屏 6 行（名字或地址 + 信号强度），编码器上下选，**K5 设为宿主**，K7 重扫，K9 返回 |
| `bt_wch.c/.h` | **【待写】** WCH BLE 协议栈后端（外设 HID 键盘 + 扫描），需要 EVT 包里的 `libCH59xBLE.a` |

## 行为（白名单模式）

* **键盘模式**：广播成 `CH592 Keyboard`，电脑/手机来连，按键通过 HID 报文发出去（标准 BLE 键盘，不用装 App）；
* **设备列表**只用来挑"允许连接的宿主"：选中某个设备按 K5 就把它记成宿主，**我们不主动连出去**；
* 只有白名单里的那个地址能连上我们，其它设备来连一律拒绝；
* 还没设宿主时谁都能连（开箱即用）；
* 状态区第一行：扫描中 / 已连接 xxx / 未找到设备；**第二行：宿主 + 设备名**。

## 依赖

* 上层：`game_port`（输入事件、1ms 计时、绘图、中文字库），在 `games\` 目录里；
* 真机后端：`CH592EVT\EVT\EXAM\BLE\LIB\` 里的 `libCH59xBLE.a`、`CH59xBLE_LIB.h`、`CH59xBLE_ROM.h`、`ble_task_scheduler.S`，以及 BLE 版链接脚本；
* 天线：CH592 的 ANT 是专用射频脚，软件不用配置；板子若带外部天线开关/PA，用
  `R16_PIN_ALTERNATE |= RB_RF_ANT_SW_EN`（0x8000）把控制输出到 PB16~PB21 再切换。

## 接线 / 按键

沿用现有硬件，不需要额外引脚：3x3 矩阵键盘 + 带按键旋转编码器 + ST7789 屏。

## 自测

PC 上用桩件 + **模拟后端**跑逻辑（不需要真蓝牙、不需要 BLE 库）：

```
powershell -ExecutionPolicy Bypass -File "C:\Users\a\Desktop\sys\CH592X_games\tests\run_tests.ps1"
```

`test_bt.c` 共 66 项：设备去重与表满、RSSI 刷新、选择循环、扫描超时、连接状态机、
宿主白名单（选中即放行、其它地址拒绝、清空后谁都能连）、HID 报文格式与修饰键、
超 6 键计数、Flush 时机、界面绘制与"没有画到屏幕外"。

## 还没做的部分

`bt_wch.c`（真正调用 WCH 协议栈）要等 BLE 库进工程后再写，用到的接口已经确认：

```
HAL_Init() / GAPRole_PeripheralInit()              当键盘（广播 + HID 服务）
GAPRole_CentralInit() + 扫描事件                   填设备表 -> BT_OnDeviceFound()
连接建立事件                                       BT_IsHostAllowed()? 否则断开 -> BT_OnConnected()
HID 订阅(CCCD)事件                                 BT_OnHidReady()
HidDev_Report(HID_RPT_ID_KEY_IN, ...)              发 8 字节键盘报文
TMOS_SystemProcess()                               挂在 BT_Task() 里
```
