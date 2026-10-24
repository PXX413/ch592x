# 在线改按键映射 —— 固件接入说明

> 目标：键盘插上电脑，用网页直接改按键映射，**不用重新烧录固件**。

---

## 一、这次加了什么

固件从「纯 HID 键盘」升级成「HID 键盘 + CDC 虚拟串口」的**复合设备**。
那个虚拟串口就是给网页用的通道：网页发命令 → 固件改映射 → 应答返回。
改完可以存进 Flash，下次开机还是你改过的样子。

```
┌──────────┐    USB 一根线     ┌──────────────────────────┐
│  网页     │ ───────────────> │  CH592X 键盘              │
│ Web Serial│                   │  ├ HID 键盘（正常打字）    │
│          │ <─────────────── │  └ CDC 虚拟串口（改映射）  │
└──────────┘                   └──────────────────────────┘
```

---

## 二、新增文件清单

全部在 `keyboard/` 目录下，**这些是新增的，不用改原来的文件**：

| 文件 | 作用 |
|---|---|
| `usb_desc_cdc.h` / `.c` | 复合设备描述符（HID + CDC，94 字节配置集合） |
| `usb_cdc.h` / `.c` | CDC 虚拟串口传输层（收发字节、发送队列） |
| `kmap_proto.h` / `.c` | 配置协议（字节流 ←→ 帧，帧头 `A5 5A` + CRC8） |
| `kmap_server.h` / `.c` | 协议业务层（每个命令具体干什么） |
| `kmap_store.h` / `.c` | 映射的 Flash 持久化（偏移 `0x0400`，占一块 256B） |

## 三、改动过的文件（4 个）

| 文件 | 改了什么 | 可逆性 |
|---|---|---|
| `keyboard/usb_kbd.h` | 加 `USBKBD_ENABLE_CDC` 开关宏 | — |
| `keyboard/usb_kbd.c` | 描述符分支、CDC 类请求、EP2/3/4 事件、发送泵 | 开关设 0 即退回原样 |
| `keyboard/usb_kbd_port.h` | 加 EP2/3/4 号与 `UsbPort_Ep2Recv/Ep3Send/Ep4Send` 声明 | — |
| `keyboard/usb_kbd_port_ch592.c` | EP2/3/4 缓冲区 + 中断处理 + 接口实现 | — |
| `hardware/key_matrix.h` / `.c` | 加 `KEY_MapGroupNameHook()` 弱符号（组名可改） | 默认返回 0，行为不变 |
| `User/Main.c` | 加两个 `#include`、`KmapStore_Init()` | — |
| `CH592X.wvproj` | include 路径补 `keyboard` / `ui` / `rf24` | 已备份 `.bak-kmap` |

### 想退回原来的纯 HID 键盘？

在 `keyboard/usb_kbd.h` 里把：

```c
#define USBKBD_ENABLE_CDC   1
```

改成 `0`，重新编译。USB 描述符、CDC 代码全部不参与编译，
行为跟改之前**一模一样**。

---

## 四、Flash 分区（重要，别撞车）

```
偏移          长度    用途
0x0000        256     game_score
0x0100 / 0x0200  各256  rf_cfg（A/B 双 Bank）
0x0300        256     ui_store（UI 设置）
0x0400        256     ★ 按键映射（本次新增）★
```

擦除粒度是 **256 字节**，每个模块各占一整块，互不干扰。
如果你以后要加新东西，从 `0x0500` 往后排。

---

## 五、协议帧格式

所有多字节字段都是**小端**。

```
┌──────┬──────┬─────┬─────┬─────────┬─────────┬──────┐
│ 0xA5 │ 0x5A │ CMD │ SEQ │ LEN(2B) │ PAYLOAD │ CRC8 │
└──────┴──────┴─────┴─────┴─────────┴─────────┴──────┘
   帧头     命令   序号   负载长度    负载      校验
```

- **CRC8**：poly `0x07`，初值 0，覆盖「帧头 → 负载末尾」
- **SEQ** 由网页递增，固件原样回填，用来配对请求与应答
- **应答** = `CMD | 0x80`（PING 的应答单独是 `0x81` PONG）
- **错误** = `0x7F`，负载第 1 字节是错误码

### 命令一览

| 命令 | 值 | 请求负载 | 应答负载 |
|---|---|---|---|
| PING | `0x01` | — | 4B 标识（用于确认对面是键盘） |
| GET_INFO | `0x02` | — | 10B（版本/组数/键数/当前组/有无记录） |
| GET_MAP | `0x10` | 1B 组号 | 18B（9 × uint16 映射值） |
| SET_KEY | `0x11` | 4B（组, 键, 值LO, 值HI） | 4B（回读实际值） |
| SET_GROUP | `0x12` | 19B（组 + 9×uint16） | — |
| GET_NAME | `0x13` | 1B 组号 | 17B 组名 |
| SET_NAME | `0x14` | 1B 组号 + 名字 | 17B（回读） |
| SET_GROUP_IDX | `0x20` | 1B 组号 | 1B（当前组） |
| SAVE_FLASH | `0x30` | — | — |
| LOAD_FLASH | `0x31` | — | 1B（是否读到有效记录） |
| RESET_DEFAULT | `0x32` | — | — |

### 映射值编码（16 位）

```
 高 8 位 = 修饰键位图       低 8 位 = 主键 HID Usage ID
┌────┬────┬────┬────┬────┬────┬────┬────┬─────────────┐
│RGUI│RALT│RSHF│RCTL│LGUI│LALT│LSHF│LCTL│  主键 Usage │
└────┴────┴────┴────┴────┴────┴────┴────┴─────────────┘
  0x80 0x40 0x20 0x10 0x08 0x04 0x02 0x01
```

举例：

| 想映射成 | 值 |
|---|---|
| 单个 `A` | `0x0004` |
| `Ctrl + C` | `0x0106` |
| `Shift + A` | `0x0204` |
| `Alt + F4` | `0x043D` |
| `Ctrl + Shift + Esc` | `0x0329` |
| 音量 + | `0x0080` |

**单独一个修饰键**有两种写法，固件都认：

| 想映射成 | 值 | 说明 |
|---|---|---|
| 单 `LCtrl` | `0x00E0` | HID usage 写法，设备端键值列表里选 `LCtrl` 就是它 |
| 单 `LCtrl` | `0x0100` | 只有修饰位、没有主键 |
| 单 `RShft` | `0x00E5` / `0x2000` | 左右是分开的 |

> **本版固件已经内置组合键 / 修饰键支持**，不用再打补丁：
> `ui_kbd.c` 的 `UiKbd_MapValueOf()` / `UiKbd_ModsOf()` / `UiKbd_BuildHid6()`
> 负责把 16 位值拆成"修饰字节 + 键数组"，USB / 2.4G / BLE 三条通道共用同一套
> 拆法；`ui_kbd_ch.c` 的 `ch_usb_sync_map()` 再把当前映射灌进 USB 报表层。
> 也就是说：**网页上怎么改，插 USB 就直接怎么发**，改完不用重新烧录。

设备端 UI 的 HID 键值列表（`ui_pages2.c` 的 `s_hid_usage[]`）也补上了
修饰键 `0xE0~0xE7` 和 Tab / Caps / F1~F12 / 导航键，一共 83 项；
网页 `tools/keymap_editor.html` 的列表与它逐项一致
（`tools/test_web_fw_consistency.py` 的 T9/T10 做交叉校验）。

---

## 六、编译步骤（MounRiver Studio）

1. 打开 `CH592X.wvproj`
2. **右键工程 → Refresh**（让它发现 `keyboard/` 下的新 `.c` 文件）
3. `obj/keyboard/subdir.mk` 是自动生成的，Refresh 后会自动把
   `usb_cdc.c`、`kmap_proto.c`、`kmap_server.c`、`kmap_store.c`、
   `usb_desc_cdc.c` 加进去
   - 如果没自动加：手动在工程树的 `keyboard` 目录上右键 → 「Add Files」
4. 编译，烧录

### 编译期自检

代码里有两个开机自检函数，描述符写错会第一时间暴露：

- `UsbDescCdc_IsValid()` —— 检查 94 字节描述符的各段长度、接口号、
  端点地址是否自洽
- `UsbDesc_IsValid()` —— 原来的纯 HID 描述符自检

两个都在 `UsbKbd_DeviceInit()` 里被调用了（返回值可忽略，只为自检）。

---

## 七、验证是否成功

1. 烧录后，把键盘插到电脑
2. **Windows**：设备管理器里应该出现
   - 「人体学输入设备」下的键盘
   - 「端口 (COM 和 LPT)」下的 **USB 串行设备 (COMx)**
   
   两个都在 = 复合设备枚举成功

3. **用串口助手试一下**（可选）：
   - 打开那个 COM 口（波特率随便填，虚拟串口不看这个）
   - 发送 `A5 5A 01 00 00 00 <CRC8>`
     （PING 命令，CRC 是前面 6 个字节的 CRC8）
   - 应该收到 `A5 5A 81 00 04 00 <4字节标识> <CRC8>`

4. **用网页试**（推荐）：见下一节

---

## 八、网页端使用

打开 `tools/keymap_editor.html`（双击即可，不用装任何东西）：

1. 点「**连接设备**」→ 浏览器弹出串口选择框
2. 选那个 **USB 串行设备**（不是蓝牙串口）
3. 连上后点「**读取设备映射**」—— 界面会自动显示设备里当前的映射
4. 随便改几个键 → 点「**下发单键**」或「**下发整组**」
5. 点「**保存到设备**」—— 写进 Flash，拔了再插也还在

> 浏览器要求：Chrome / Edge 89+ （需要 Web Serial 支持）。
> 用 `file://` 直接打开 HTML 就能用，不需要起服务器。

---

## 九、故障排查

| 现象 | 原因 / 处理 |
|---|---|
| 插上后没有 COM 口 | 描述符没枚举成功。确认 `USBKBD_ENABLE_CDC=1`；调 `UsbDescCdc_IsValid()` 看断言 |
| 插上后键盘也不能打字了 | 描述符破坏了。先设 `USBKBD_ENABLE_CDC=0` 编译回退确认键盘正常，再查描述符 |
| 网页选不到串口 | 浏览器不支持 Web Serial（Firefox/Safari 不支持），换 Chrome/Edge |
| 连上了但读不出映射 | ① 确认选对了串口；② 点「PING 测试」看有没有应答；③ 查 CRC 算法是否一致 |
| 改了但重启又变回去 | 忘了点「保存到设备」。改完必须 SAVE_FLASH 才进 Flash |
| 编译报找不到 `usb_cdc.h` | 工程 include 路径少了 `keyboard`，见第三节 wvproj 那条 |
| 编译报重复定义 | MRS 的 `subdir.mk` 没刷新，Refresh 一下工程 |
| 保存时报 Flash 错误 | `0x0400` 那块被别的东西占了。查一下有没有别的模块也用 `0x04xx` |

---

## 十、代码结构（维护时看这里）

```
网页
  │  USB 批量传输
  ▼
usb_kbd_port_ch592.c   ← 寄存器层：EP2 OUT 收字节 / EP3 IN 发字节
  │  USBK_EV_EP2_OUT 事件
  ▼
usb_kbd.c              ← USB 应用层：取数据、调 CDC 层、跑发送泵
  │  UsbCdc_OnDataOut()
  ▼
usb_cdc.c              ← 传输层：字节流 → 帧，应答 → 发送队列
  │  Kmap_ParserPush() / KmapServer_HandleFrame()
  ▼
kmap_proto.c           ← 协议层：切帧、CRC 校验
kmap_server.c          ← 业务层：每个命令干什么
  │  KEY_SetMapValue() / KmapStore_Save()
  ▼
key_matrix.c           ← 数据层：真正的映射表（RAM）
kmap_store.c           ← 持久化：写 Flash（偏移 0x0400）
```

每一层都能单独测。交付时带了 4 套自测脚本，全部在 PC 上用 Python / Node 跑通，
**不需要真硬件**就能确认协议这一层是对的：

| 脚本 | 覆盖内容 | 结果 |
|------|----------|------|
| `tools/test_usb_cdc.py` | 传输层：跨包分片、队列积压丢最旧、半包/粘包、脏数据抗性、随机分片 | 29 / 29 |
| `tools/test_kmap_server.py` | 业务层：11 条命令的正常路径 + 错误路径，全部往返校验 | 52 / 52 |
| `tools/test_web_proto.js` | **网页组帧 ←→ 固件解析** 全链路对拍（从 HTML 里抠真实协议代码来跑） | 58 / 58 |
| `tools/test_web_fw_consistency.py` | **网页常量 ←→ 固件头文件** 逐项交叉比对（帧头/命令码/应答码/错误码/魔数/尺寸） | 40 / 40 |

另外 9 个固件文件在 `USBKBD_ENABLE_CDC=1` 和 `=0` 两种开关下都通过了 host gcc
语法检查（`-Wall`，无 error）。

跑法（在工程根目录）：

```bash
python tools/test_usb_cdc.py
python tools/test_kmap_server.py
python tools/test_web_fw_consistency.py
node   tools/test_web_proto.js
```

---

*最后更新：随本次在线改映射功能一并交付*

---

## 七、键盘宏（2026-10-15 新增）

一个键触发一串按键：网页上把"步骤"编好、绑到某个物理键，按一下那个键，
键盘自己按顺序把这一串敲出来（每步仍是一次正常的按下+松开报文）。

### 规格

| 项 | 值 |
|---|---|
| 宏个数 | 4（宏1~宏4） |
| 每个宏最多 | 8 步 |
| 一步 | `uint16_t`，和按键映射值同格式：高字节 = HID 修饰位图，低字节 = HID Usage |
| 步间隔 | 45ms |
| 绑定 | 每组 9 个键各自可绑 0（不绑）或 1~4，**独立于映射值**，不影响组合键编码 |

### Flash

```
0x1600 ~ 0x16FF   宏记录（120 字节有效，独立一块，和 0x0400 的映射记录互不影响）
```

记录格式：magic `0x414D` + 版本 + 绑定表(45B) + 各宏步数(4B) + 步内容(64B) + CRC8。

### 协议（在原有帧格式上增加 5 条命令）

| 命令 | 码 | 负载 | 应答 |
|---|---|---|---|
| MACRO_GET_BIND | 0x50 | 组号 1B | 9B 绑定表 |
| MACRO_SET_BIND | 0x51 | 组号 + 9B 绑定 | ACK |
| MACRO_GET | 0x52 | 宏号 1B | 1B 步数 + 8×2B 步内容 |
| MACRO_SET | 0x53 | 宏号 + 步数 + 8×2B 步内容 | ACK |
| MACRO_SAVE | 0x54 | 无 | ACK（写 Flash） |

`GET_INFO` 的协议次版本（第 4 字节）现在是 **2**：≥2 表示这台设备支持宏。

### 实现位置

- `keyboard/macro.c` / `macro.h` —— 存储 + 播放（播放复用 ui_kbd.c 的"额外键脉冲"）
- `keyboard/kmap_proto.h` 增加了 0x50~0x54；`keyboard/kmap_server.c` 增加处理函数
- `ui/ui_kbd.c` 的 `UiKbd_Task()` 里每 1ms 调一次 `Macro_Task()`
- 网页：`tools/keymap_editor.html` 的「键盘宏」面板（编步骤 / 录制 / 绑定 / 读 / 写 / 保存）

### 注意

- 宏播放时新按下的任何键都会先**停掉**正在播放的宏（防止宏卡住不放）。
- 网页「录制」是按**电脑键盘**录入步骤（和键盘自己的按键无关），录完必须
  点「写入设备并保存」才会进设备 Flash。
