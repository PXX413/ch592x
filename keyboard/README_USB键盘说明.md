# CH592X USB 键盘模式 说明

数据线插上电脑，CH592X 就当一个**标准 HID 键盘**用（免驱，BIOS 里也能用）。
这是产品里**优先级最高的传输通道**：插着线就走 USB，没插线才去走 2.4G / BLE
（见 `键盘功能实现.txt` 的"键盘自动判断通信方式"）。

---

## 一、文件清单与分层

```
CH592X_keyboard\
├── usb_kbd_report.h/.c       按键状态 -> 8 字节 boot 报文（纯逻辑，零依赖，可 PC 单测）
├── usb_desc.h/.c             描述符表：设备/配置/接口/HID/端点/报表/字符串 + 自查
├── usb_kbd.h/.c              设备侧主逻辑：控制传输状态机、HID 类请求、上报节流
├── usb_kbd_port.h            硬件抽象层（USB 外设的接口）
├── usb_kbd_port_ch592.c      ★ 真机后端：CH59x USB 寄存器 + 中断
├── README_USB键盘说明.md      本文件
└── tests\                    PC 自测（虚拟 USB 主机 + 虚拟端点）
    ├── run_tests.ps1
    ├── test_report.c         报文层
    ├── test_desc.c           描述符
    ├── test_kbd.c            端到端：枚举 -> 打字 -> LED
    ├── stub\usb_kbd_port_sim.c   虚拟 USB 主机
    └── stub\usb_sim.h
```

依赖方向（下层不认识上层）：

```
        usb_kbd.c            ← 协议逻辑：谁也不需要认识寄存器
        /        \
  usb_desc.c   usb_kbd_report.c     ← 纯数据 / 纯逻辑
        \        /
        usb_kbd_port.h             ← 硬件抽象
        /              \
usb_kbd_port_ch592.c   tests\stub\usb_kbd_port_sim.c
   （真机寄存器）           （PC 虚拟主机）
```

**为什么要把端口层单独抽出来**：`usb_kbd_port_ch592.c` 要 include CH59x 的 SFR 头，
PC 上根本编不了。抽掉它之后，上面三层（报文 + 描述符 + 控制传输状态机）在 PC 上
可以完整跑一遍枚举过程 —— `test_kbd.c` 就是用一个虚拟 USB 主机把设备从
"刚上电"一路驱动到"把键码发出去"。

---

## 二、怎么用

### 真机（MounRiver Studio）

```c
#include "usb_kbd.h"

int main(void)
{
    SetSysClock(CLK_SOURCE_PLL_60MHz);

    UsbKbd_DeviceInit();          /* 里面会调 UsbPort_Init()：配端点 + 开 USB 中断 */

    while (1)
    {
        UsbKbd_DeviceTask();      /* 非阻塞：处理 USB 事件 + 有新状态就上报 */

        /* 按键扫描 -> 喂给报文层（和 BLE 那份用法一模一样） */
        if (KEY_IsPressed(1)) { UsbKbd_KeyEvent(USBKBD_K1, true); }
        else                  { UsbKbd_KeyEvent(USBKBD_K1, false); }

        /* 通道选择：USB 最优先 */
        if (UsbKbd_IsConnected())  { /* 走 USB，别的通道别发 */ }
        else if (UsbKbd_IsPlugged()) { /* 插着线但还没枚举完，先等着，别切 2.4G */ }
        else                       { /* 没插线 -> 2.4G / BLE */ }
    }
}
```

### 通道切换要点

| 接口 | 含义 | 用来干什么 |
|---|---|---|
| `UsbKbd_IsConnected()` | 主机已经 `SET_CONFIGURATION`，可以发 HID 报文 | 真正切到 USB 通道 |
| `UsbKbd_IsPlugged()` | 只要枚举开始过就算（比上面宽松） | **插了线还没枚举完的这段过渡期不要切走**，否则枚举会被打断 |

`UsbKbd_DeviceShutdown()` 会先 `ReleaseAll()` 把按键全松开（不然切走之后主机那边
会一直以为某个键按着），再关 USB、放掉 D+ 上拉（主机立刻看到"拔出"）。

### PC 自测

```
powershell -ExecutionPolicy Bypass -File "C:\Users\a\Desktop\sys\CH592X_keyboard\tests\run_tests.ps1"
```

---

## 三、启用 USB 时工程要改的地方

**? 工程里 `CH59x_usbdev.c` 目前是被排除编译的**，必须放开它，否则
`USB_DeviceInit()` / `DevEP1_IN_Deal()` / `pEP0_RAM_Addr` 全是未定义符号。

`.cproject` 第 290 行（同步后行号可能变，搜 `excluding=` 就是它）：

```xml
<entry excluding="StdPeriphDriver/CH59x_usbhostClass.c|StdPeriphDriver/CH59x_usbhostBase.c|StdPeriphDriver/CH59x_usbdev.c|StdPeriphDriver/CH59x_uart3.c|…" flags="VALUE_WORKSPACE_PATH|RESOLVED" kind="sourcePath" name=""/>
```

把里面 `StdPeriphDriver/CH59x_usbdev.c|` 这一段**删掉**（只删这一个，别把整条 entry 删了）。

更省事的做法：在 MRS 里右键 `StdPeriphDriver/CH59x_usbdev.c` → Properties →
C/C++ Build → 取消勾选 **Exclude resource from build**。这样 `.cproject` 由 IDE 自己改，
不容易手抖。

另外要在包含路径里加上 `rf24` 那样的一条 `${ProjName}/keyboard`（或者把
`CH592X_keyboard\` 同步成工程里的 `keyboard\`）。

---

## 四、描述符说明

### 4.1 设备描述符（18 字节）

| 偏移 | 字段 | 值 | 说明 |
|---|---|---|---|
| 0 | `bLength` | 18 | |
| 1 | `bDescriptorType` | 0x01 | DEVICE |
| 2-3 | `bcdUSB` | 0x0110 | USB 1.1（够用；HID 键盘不需要 2.0 高速） |
| 4 | `bDeviceClass` | **0** | 类定义在接口里（HID 规范这么要求的） |
| 5-6 | SubClass / Protocol | 0 / 0 | 设备级不定义 |
| 7 | `bMaxPacketSize0` | **64** | 控制端点 0 单包 64 字节 |
| 8-9 | `idVendor` | **0x3D41** | WCH 的 VID（和官方例程一致） |
| 10-11 | `idProduct` | **0x0721** | 和官方 HID 例程同款；想区分产品就换一个 |
| 12-13 | `bcdDevice` | 0x0100 | 固件版本 1.00 |
| 14-16 | `iManufacturer/Product/Serial` | 1 / 2 / 3 | 指向字符串描述符 |
| 17 | `bNumConfigurations` | 1 | |

> `idVendor` 用的是 WCH 的 0x3D41。自己量产要换成自己的 VID，否则和别家设备撞了
> 驱动不会出问题、但设备管理器里会显示成别家厂商。

### 4.2 配置描述符集合（41 字节）

| 段 | 偏移 | 关键字段 |
|---|---|---|
| 配置 | 0 | `wTotalLength=41`、`bNumInterfaces=1`、`bmAttributes=0x80`（**bit7 必须为 1**）、`bMaxPower=100mA` |
| 接口 | 9 | `bNumEndpoints=2`、`bInterfaceClass=3`(HID)、`bInterfaceSubClass=1`(Boot)、`bInterfaceProtocol=1`(Keyboard) |
| HID | 18 | `bcdHID=0x0111`、`bCountryCode=0`、`bNumDescriptors=1`、`bDescriptorType=0x22`、`wDescriptorLength=63` |
| 端点 IN | 27 | `0x81`、中断、`wMaxPacketSize=8`、`bInterval=1` |
| 端点 OUT | 34 | `0x01`、中断、`wMaxPacketSize=8`、`bInterval=1` |

> **为什么多了一个 EP1 OUT**：需求里既写了"Endpoint Descriptor：EP1 IN"，又写了
> "EP1 OUT 收 LED 报文"。标准 boot 键盘其实是用**控制端点**下发 LED 的
> （`SET_REPORT`，`bmRequestType=0x21`），但确实有一部分主机/固件走中断 OUT 端点。
> 所以这里**两条路都做**：描述符里声明 EP1 OUT，代码里
> `SET_REPORT`（走 EP0）和 EP1 OUT 都能收。如果确定不需要 OUT 端点，
> 把接口的 `bNumEndpoints` 改回 1、删掉偏移 34 那段 7 字节、`wTotalLength` 改成 34。

### 4.3 HID 报表描述符（63 字节）

标准 boot 键盘，`wDescriptorLength` 和数组长度是**同一个宏**（`USBDESC_REPORT_LEN`），
改一处就够，`UsbDesc_IsValid()` 会盯着。

| 项 | 内容 |
|---|---|
| 修饰键 | Usage Page `0x07`、Usage Min `0xE0`、Max `0xE7`、Report Size 1 × Count 8、`Input(Data,Var,Abs)` |
| 保留字节 | Report Size 8 × Count 1、`Input(Const)` |
| LED 输出 | Usage Page `0x08`、Usage Min `0x01`、Max `0x05`、Report Size 1 × Count 5、`Output(Data,Var,Abs)` + 3 位补齐 |
| 键码数组 | Report Size 8 × Count 6、Logical 0..101、Usage Min 0、Max `0x65`、`Input(Data,Array)` |

### 4.4 字符串描述符

格式 `[bLength][0x03][UTF-16LE …]`：

| 序号 | 内容 | 字节 |
|---|---|---|
| 0 | 语言 ID `0x0409`（美式英语） | 4 |
| 1 | `"WCH"` | 8 |
| 2 | `"CH592X Keyboard"` | 32 |
| 3 | `"0001"`（占位，**建议改成芯片 UID**） | 10 |

改产品名：编辑 `usb_desc.c` 里的 `UsbDesc_StrProduct[]`，
**注意同时改首字节的长度**（= 2 + 字符数×2），`UsbDesc_ProductName[]` 也一起改。

---

## 五、按键 -> HID 键码映射

和 `CH592X_bluetooth\bt_hid.c` 的默认表**逐字节相同**（`test_report.c` 的 T5 专门钉这件事）：

| 按键 | HID Usage | 电脑上打出来 |
|---|---|---|
| K1 ~ K9 | `0x1E` ~ `0x26` | 数字 `1` ~ `9` |
| 编码器正转 | `0x80` | 音量 + |
| 编码器反转 | `0x81` | 音量 ? |
| 编码器按下 | `0x28` | 回车 |

想改键位：`UsbKbd_SetUsage(key, usage)`，或者用
`UsbKbd_SetModifierBit(key, USBKBD_MOD_LCTRL)` 给某个键挂修饰位
（`usage` 传 0 就是"纯修饰键"，不占 6 个键码槽位）。

**注意：上面这张默认表只是开机初值。** 3x3 键盘页用的是 `key_matrix.c` 里
那套可改的键值映射（设备端键值编辑页 / 网页在线改映射都能改），
`ui_kbd_ch.c` 的 `ch_usb_sync_map()` 会在按键之前把当前映射同步进来：

* 映射值低字节 → `UsbKbd_SetUsage()`
* 映射值高字节（组合键）→ `UsbKbd_SetModifierBit()`
* 低字节是 `0xE0~0xE7`（单独一个 Ctrl/Shift/Alt/GUI）→ usage 置 0、
  修饰位单独给，所以它不占 6 个键码槽位

不接这个同步的话，USB 通道会一直按默认的 `K1~K9 = 1~9` 发，改了映射也不生效。

---

## 六、资源占用

### 端点 RAM（静态，`__attribute__((aligned(4)))`，必须常驻）

| 数组 | 大小 | 用途 |
|---|---|---|
| `s_ep0_buf` | **192 B** | EP0(64) + EP4 OUT(64) + EP4 IN(64) |
| `s_ep1_buf` | **128 B** | EP1 OUT(64) + EP1 IN(64) |
| `s_ep23_buf` | **64 B** | EP2/EP3 本次不用，但 `USB_DeviceInit()` 会读 `pEP2/pEP3_RAM_Addr` 写 DMA 寄存器，不能给 NULL |
| **合计** | **384 B** | |

> 官方例程给 EP2、EP3 各分了 128 字节（一共 576 B）。本实现把 EP2/EP3 指向**同一个
> 64 字节缓冲**（它们从头到尾没被使能，只是个合法落地地址），省下 192 字节。
> 如果以后要开 EP2/EP3，必须给它们各自独立缓冲。

### 其它

| 项 | 大小 | 位置 |
|---|---|---|
| 描述符表 | **176 B** | Flash（`const`）：设备 18 + 配置 41 + 报表 63 + 字符串 54 |
| `usb_kbd.c` 状态 | ~40 B | RAM（`.bss`） |
| `usb_kbd_report.c` 状态 | 32 B | RAM（8B 报文 + 12B 状态 + 12B 位图/修饰） |
| 端口层状态 | ~32 B | RAM |
| **本模块 RAM 合计** | **约 450 B** | 其中 384 B 是 USB 端点缓冲 |

USB 外设自己的 RAM（CH59x 内部 FIFO）另算，不在上面。

---

## 七、PC 自测

```
############ pass 1: UTF-8 sources ############
  test_report  warnings=0   ==== result: 58 passed, 0 failed ====
  test_desc    warnings=0   ==== result: 68 passed, 0 failed ====
  test_kbd     warnings=0   ==== result: 57 passed, 0 failed ====
############ pass 2: delivered GBK sources (-finput-charset=GBK) ############
  （同上一轮）
```

**183 项 x 2 轮**，`-std=gnu99 -Wall -Wextra -Wpedantic` **零告警**。

覆盖内容：

* `test_report`：默认键位表（含"和 bt_hid 一致"那条）、8 字节报文组装、
  reserved 字节恒 0、6 键上限与溢出计数、修饰位、`usage=0` 纯修饰键、
  脏标志、`ReleaseAll`、NKRO 位图、LED 解析与保留位屏蔽；
* `test_desc`：三个描述符 + 4 条字符串的**逐字段**校验、`wTotalLength` 自洽、
  HID 的 `wDescriptorLength` = 报表长度、端点的地址/类型/包长/间隔、
  小端字节序、`UsbDesc_Lookup()` 边界、`UsbDesc_IsValid()`；
* `test_kbd`：**完整枚举**（8 字节探测 -> 18 字节设备描述符 -> SET_ADDRESS ->
  配置集合 -> 字符串 -> 报表描述符 -> SET_CONFIGURATION）、
  打字的报文内容、EP1 流量控制（上一包没取走不能覆盖）、
  `SET_REPORT` 和 `EP1 OUT` 两条 LED 路径、`SET_IDLE`/`SET_PROTOCOL`/`GET_REPORT`、
  不支持请求被 STALL、总线复位后重新枚举、未配置时不上报。

---

## 八、SDK / 例程里的坑（这些是实际踩出来的）

### 1. 例程用的是 **Vendor-Defined** 描述符，不是键盘

`HID_CompliantDev\src\Main.c` 里的报表描述符是：

```c
0x06, 0x00, 0xff,   // Usage Page (Vendor Defined 0xFF00)
0x09, 0x01,         // Usage (Vendor Usage 1)
0xa1, 0x01,         // Collection (Application)
… 0x40 字节的 Input + 0x40 字节的 Output …
```

它是给"自定义 64 字节双向透传"用的，**电脑上根本不认成键盘**。
本实现按标准重写了报表描述符（Generic Desktop 0x01 / Keyboard 0x06），
并配了 Boot 子类 + Keyboard 协议，所以 BIOS 也能直接用。

### 2. `CH59x_usbdev.c` 默认被排除编译

见第三章。这是最容易卡住的一步 —— 编译报 `USB_DeviceInit` 未定义时先来看这里。

### 3. `pEP2_RAM_Addr` / `pEP3_RAM_Addr` 不能留 NULL

`USB_DeviceInit()` 里有：

```c
R16_UEP2_DMA = (uint16_t)(uint32_t)pEP2_RAM_Addr;
R16_UEP3_DMA = (uint16_t)(uint32_t)pEP3_RAM_Addr;
```

不给这两个指针赋值，DMA 地址就是 0。本实现给它们指了一块共用的 64 字节缓冲。

### 4. `RB_UIS_SUSPEND_ACT` 这个宏**不存在**

我在挂起判定里先写的是它，编译报未定义。正确的是：

```c
if ((R8_USB_MIS_ST & RB_UMS_SUSPEND) != 0u)   /* 杂项状态寄存器，不是 INT_ST */
```

### 5. `RB_UIF_*` 是"写 1 清"的，而且要写在最后

`R8_USB_INT_FG = RB_UIF_TRANSFER;` 必须在处理完这一类事件之后执行；
三个事件（TRANSFER / BUS_RST / SUSPEND）互斥判断，别用并列的 `if` 去重复清标志。

### 6. 总线复位要把所有端点控制寄存器都写回去

例程里是：

```c
R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
```

本实现照做了。

### 7. 例程把发送逻辑写在 IN 令牌中断里

例程的 `case UIS_TOKEN_IN:` 里才 `memcpy` 描述符进 `pEP0_DataBuf`，
也就是"中断里决定发什么"。这样没法分层、也没法在 PC 上测。

本实现改成**预装**：SETUP 处理完立刻把第一包装进去并解锁（`T_RES=ACK`），
之后每次 IN 完成给主循环一个 `USBK_EV_EP0_IN` 事件，主循环再装下一包。
中间主机抢先发的 IN 会收到 NAK（USB 协议允许，主机会自动重试），功能完全等价。
代价是偶发几次重试，换来的是整个控制传输状态机可以在 PC 上单测。

### 8. EP0 的 DATA0/DATA1 要自己翻

`USB_DeviceInit()` 里 `R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;`，
**没有**给 EP0 开 `RB_UEP_AUTO_TOG`（EP1 开了）。所以每次解锁前都要
`R8_UEP0_CTRL ^= RB_UEP_T_TOG;`。漏了会导致主机认为数据包重复而丢弃。

### 9. HID 描述符和报表描述符的索引取的是 `wIndex`，不是 `wValue` 低字节

`GET_DESCRIPTOR(0x22)` 是**接口请求**（`bmRequestType=0x81`），
接口号在 `wIndex` 里；`wValue` 的低字节是"描述符索引"，键盘只有一个（0），
但这俩搞混了就会出现"接口 0 要报表描述符却按字符串索引 0 去查"的怪事。

### 10. `bConfigurationValue` 和 `bmAttributes` 的两个坑

* `bmAttributes` 的 **bit7 必须置 1**，否则主机认为这个配置非法，直接不认；
* `SET_CONFIGURATION` 的 `wValue` 是**配置值**（这里是 1），不是配置索引；
  收到 0 表示"取消配置"，要退回 ADDRESSED 状态。

### 11. 只声明输入报文的话，Windows 认，但某些主机不认

LED 输出报文在报表描述符里必须有，否则 `SET_REPORT` 会被主机认为不支持。
另外 `bInterfaceSubClass=1`（Boot）能保证 BIOS / GRUB 阶段也能打字；
如果只写 Report 协议（SubClass=0），进 BIOS 就失效了。

---

## 九、同步时要注意什么

1. **整个 `CH592X_keyboard\` 目录**一起同步到工程的 `keyboard\`（和 `hardware\`、`games\`、`rf24\` 同级）。
   `tests\` 不用进工程（`.txt`/`tests` 目录不参与编译）。
2. 要一起覆盖的成对文件：`usb_desc.h` + `usb_desc.c`（`USBDESC_REPORT_LEN` 两边都有）、
   `usb_kbd_port.h` 和它的两个后端。
3. **`.cproject` 必须改两处**：
   * 第 290 行那个 `excluding=` 里删掉 `StdPeriphDriver/CH59x_usbdev.c|`；
   * 包含路径里加一条 `${ProjName}/keyboard`。
4. `usb_kbd_port_ch592.c` **只有真机能编**，PC 自测会自动把它排除
   （`run_tests.ps1` 里按文件名过滤），所以它在工程里参与编译、在测试里不参与。
5. 我**没有**动 `CH592X_rf24`、`CH592X_bluetooth` 和任何工程文件。

---

## 十、已知的取舍

1. **EP1 OUT 端点**：需求里"描述符最少要有 EP1 IN"和"EP1 OUT 收 LED"其实有点冲突
   （标准键盘用控制端点收 LED）。我两个都做了，代价是多 7 字节描述符 + 一个端点。
   不想要就按 4.2 的说明删。
2. **NKRO**：报文层实现了位图生成（`UsbKbd_BuildNkroBitmap`，有测试），
   但**USB 描述符声明的仍然是 boot 6KRO 报表**。真要在 USB 上用 NKRO，
   得换一份 NKRO 报表描述符（28 字节位图 + `bInterfaceSubClass=0`），
   那会失去 BIOS 兼容性 —— 建议 NKRO 只走 BLE 侧。
3. **序列号**是固定的 `"0001"`。量产应该用芯片 UID 生成，`usb_desc.c` 里
   `UsbDesc_StrSerial[]` 改一下即可（长度字段也要跟着改）。
4. **`idProduct` 沿用 0x0721**（WCH HID 例程的值）。要区分自己的产品就换成自己的。
5. **低功耗**没做：挂起事件（`USBK_EV_SUSPEND`）只记了状态，没进 `__WFI()`。
   USB 挂起要进低功耗还涉及 GPIO/RF 的联合处理，留给系统级休眠那一层统一做。
