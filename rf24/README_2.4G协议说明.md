# CH592X / CH570Q 2.4G 私有协议实现说明

依据：工作区里的《CH592X / CH570Q 2.4G 私有协议详细规范 v1.0》
（23 章 + 7 附录，两种模式、23 种帧）。

本目录就是规范第 16.1 节那张文件结构图的落地版：**一份可以编译、可以自测、
可以烧进 CH592X / CH570Q 的完整协议栈**。

---

## 一、文件清单

```
CH592X_rf24\
├── proto_common.h/.c      帧字典（23 种帧）、frame_ctrl 拆解、CRC-8/16、小端读写、地址拼装
├── hal_rf.h               ★ 射频硬件抽象层（规范 16.2）
├── hal_rf_ch592.c         真机后端：落到 CH59x BLE 库的 RF_Config/RF_Tx/RF_Rx/RF_Shut
├── link_hop.h/.c          跳频：3/8 频点表、5-bit LFSR、劣化频点屏蔽
├── link_keymap.h/.c       ★ 9 键名称元数据通道：TLV + 分片 + 重组 + 缺片重传 + 改名 + 校验
├── link_reliability.h/.c  序列号去重、丢帧统计、PER、链路自适应、报告缓存
├── link_tdma.h/.c         16 ms 超帧 / 20×0.8 ms 时隙调度、从机睡到自己的时隙
├── proto_kb.h/.c          键盘模式：主机 POLL 循环、从机应答、配对、6KRO/NKRO、补帧
├── proto_rc.h/.c          遥控模式：主机 5 态 / 从机 6 态状态机、枚举、REPORT、改名
├── rf_cfg.h/.c            配置持久化：数据 Flash 双 Bank + CRC-16
├── rc_json.h/.c           JSON 导出（规范 16.7 / 附录 F.2 格式）
├── README_2.4G协议说明.md   本文件
└── tests\                 PC 主机自测（虚拟空口 + 三套测试程序，不参与单片机编译）
```

---

## 二、怎么用

### 真机（MounRiver Studio）

1. 把 `CH592X_rf24\` 整个目录放进工程根目录（和 `hardware\` 同级 → 工程里叫 `rf24\`）；
2. 工程里按 **F5 刷新**，`.c` 自动参与编译；确保 `rf24` 在包含路径里；
3. 主程序里 `#include "hal_rf.h"`、`#include "proto_rc.h"`，照着 `tests\test_stack.c` 的
   主循环写：主机循环调 `Rc_HostSuperframe()`，从机在 SYNC 里醒来调 `Rc_DevServeSlot()`；
4. `hal_rf_ch592.c` 里 `HALRF_Micros()` 的 `HALRF_SYSTICK_HZ` 按主频改
   （60 MHz 时 SysTick 是 7.5 MHz，默认就是这个）。

### PC 自测

```
powershell -ExecutionPolicy Bypass -File "C:\Users\a\Desktop\sys\CH592X_rf24\tests\run_tests.ps1"
```

它会把源码复制成 UTF-8 副本编一遍，再对交付的 GBK 源码直编一遍，两轮都跑。

---

## 三、协议栈怎么分层的

```
        proto_kb.c            proto_rc.c          ← 应用层：状态机、超帧、命令
             \                   /
          link_keymap  link_hop  link_tdma  link_reliability   ← 协议层：纯逻辑，无硬件
                       \        |        /
                        proto_common.h                       ← 帧字典 / CRC / 字节序
                              |
                          hal_rf.h                            ← 硬件抽象
                        /          \
              hal_rf_ch592.c     tests\stub\hal_rf_sim.c     ← 真机后端 / PC 虚拟空口
```

**关键设计**：协议层和 `proto_*` 层只 include `hal_rf.h`，不认识任何 CH59x 寄存器。
所以 PC 上换成 `hal_rf_sim.c`（一块虚拟空口 + 虚拟时钟）以后，主机和从机可以
**在同一个进程里对拖**，把枚举、分片、改名、跳频、TDMA 全部跑通并逐条断言。

`test_stack.c` 就是这么干的：`Rc_HostSuperframe()` 跑到哪个时隙，虚拟空口就把帧
交给从机的 `Rc_DevServeSlot()`，从机的应答再原路回给主机。

---

## 四、★ 9 键名称通道（规范第 6 章）

9 个键各 ≤19 字节 ASCII，最坏 `9 × (1+1+19) = 198` 字节，远大于 MTU 32 字节，
所以走"分片 + TLV"。

### 线上 TLV

```
+--------+--------+--------+-------------------+
| key_id |  act   |  len   |  name[0..len-1]   |
|  1B    |  1B    |  1B    |   len 字节        |
+--------+--------+--------+-------------------+
```

`act` 一个字节塞两样东西（正好凑成规范 6.1 的 `9×22=198`）：

* `bit[2:0]` = `key_action_e`（MOMENTARY / TOGGLE / LATCH / INCREMENT / DECREMENT / SCENE / MACRO）
* `bit[7:3]` = scene 参数（只有 `ACT_SCENE` 用得到）

`len = 0` 存在但没配名字（UI 显示 `KEYn`）；`len = 0xFF` 该键物理不存在（UI 隐藏）。

### 分片

```
0  frame_ctrl=0x30   1  dev_id        2  config_ver   3  frag_total
4  frag_index        5  payload_len   6..30 payload[25]   31 crc8
```

最坏 198 字节 → **8 片**（`frag_total` 是算出来的，不是写死的）。
每片占一个超帧（16 ms），0.2 秒内传完；主机 200 ms 收不齐就发 `RC_KEYMAP_MISS` 只补缺的那几片。

### 改名

`RC_RENAME_KEY`（32B，Host→Device）可以一次把名称、行为、scene 参数都改掉，
从机写完 Flash 后 `config_ver++`，主机发现版本变了会重新拉一遍名称表。

---

## 五、规范里自相矛盾的地方，以及本实现怎么取的舍

写代码的过程中发现规范有 **11 处**前后对不上。每一条都在对应 `.c/.h` 的注释里
写明了，这里汇总一份，方便回改文档：

| # | 规范位置 | 矛盾点 | 本实现的取舍 |
|---|---|---|---|
| 1 | 3.1 vs 附录 D.2 | 3.1 说 `frame_ctrl[4:0]` 是类型、bit5 是 AC；但 13.2 的 `RC_REPORT=0x20`、`RC_KEYMAP_FRAG=0x30` 都把 bit5 当类型用。`0x20 & 0x1F == 0`，会和 `KB_ACK` 撞车 | 按附录 D.2 的"最终约定"：**整个字节就是帧类型**，只掩掉 bit7。协议靠信道+地址+帧长区分 |
| 2 | 6.3 vs 6.1 | 帧布局给 payload 留的是 **25 字节**，6.1 却按 28 字节/片算出"7 片"。25×7=175 装不下 198 | 按帧布局的 25 字节，`frag_total` **动态计算**（最坏 8 片），字段本身支持 |
| 3 | 6.2 vs 6.4 | 6.2 要求"名称必须与行为成对上报"、6.1 的 198 字节预算也算进了行为字节，但 6.4 的 TLV 图里没有 action | TLV 里**加上 action**（并顺带塞进 scene 参数），正好对上 6.1 的 9×22 |
| 4 | 6.5 示例代码 | `memcpy(&k->tlv_buf[k->tlv_len], p, plen)` 是**按到达顺序**拼的 —— 片一乱序，重组出来的 TLV 就是错的 | 按 `frag_index × 25` 算固定偏移再落盘，乱序到达也不串（`test_keymap` T36/T37 专门验这条） |
| 5 | 6.9.1 vs D.2.4 | 改名帧：一处说 24B / name 18B，一处字段加起来 28B；而且 `frame_ctrl = 0x40\|dev_id` 会和 `0x41/0x44` 撞 | 改成 **32B、dev_id 单独一字节、name 放满 19 字节**，scene 参数并进 action 的高 5 位 |
| 6 | 6.6 vs 13.2 | 时序图里有 `ENUM_REQ` 帧，帧表里根本没有这个类型 | 用 **`SYNC.flags.bit0 = enum_open`** 当 ENUM_REQ（这颗位规范里本来就有） |
| 7 | 5.3.4 | `ENUM_ACK` 只有 `dev_id + tx_addr`，没有能标识"这帧给谁"的东西。CH592 只有一个 accessAddress、没有管道地址过滤，未枚举的从机全会收到 | `ENUM_ACK` 第 7 字节（规范里是填充）拿来放 **hw_id 低字节**，从机对一下才认领 |
| 8 | 5.3.4 | `key_mask` 只给 1 字节，可 9 个键要 9 位 | 内部用 `uint16_t`；线上第 9 位放在偏移 30 的 reserved 字节 bit0（偏移 29 是 rssi，不能占） |
| 9 | 2.4 vs CH592 PHY | 2.4 要求 16 个从机用 16 个不同的 RF 地址。CH592 换一次 accessAddress 要重写 `RF_Config`，16 个时隙就是 16 次 | 默认**单地址方案**（`RF24_RC_PER_DEVICE_ADDR=0`）：共用一个 accessAddress，靠 TDMA 时隙 + 帧里的 `dev_id` 区分。改成 1 就回到规范原方案 |
| 10 | 4.6 / 7.2 | `kb_hop_tbl[(seq >> 5) & 0x03]` —— 表只有 3 个元素，`& 0x03` 会算出下标 3，**数组越界**。跳频同理，`rc_hop_tbl[seed % 8]` 实测会出现 `7 7 7` / `0 0 0 0 0` 这种连着停在同一频点 | 键盘表改 `% 3`；遥控侧改成"**先按 `ceil(31/频点数)` 抽稀 LFSR，再等比映射到 0..N-1**"，并挡掉连续同频。实测最长同频段从 7 降到 1 |
| 11 | 2.1 vs 16.2 | CRC 一处写 `0xA001`（MODBUS 反射多项式）、一处写 init `0xFFFF` | 统一成 **CRC-16/CCITT-FALSE（0x1021 / 0xFFFF）**，和工程里 `game_score` 保持一致，避免一个工程两套 CRC |

另外 `frame_ctrl` 的"协议位"（3.1 的 bit[7:6]）和附录 D.2 的"高 4 位不参与判别"
也是互相打架的，一并按附录 D.2 处理。

---

## 六、PC 自测

`tests\run_tests.ps1` 会把 `hal_rf_sim.c`（虚拟空口）和协议栈一起编起来，
**主机和从机在同一个进程里对拖**。通过项：

| 测试程序 | 覆盖内容 | 通过项 |
|---|---|---|
| `test_link` | 帧字典、CRC 标准向量、地址编码、LFSR 周期 31、跳频不连续同频、劣化屏蔽、超帧/时隙网格与超时对齐、时隙分配、序列号去重（含 255→0 回绕）、PER、链路自适应 | 106 |
| `test_keymap` | 默认配置、TLV 编解码、**乱序片重组**（规范 6.5 的 bug）、缺片补传、片 CRC、坏 TLV、改名（含截断/非法字符/CRC 错/key_id 越界）、名称校验、Flash 名称区往返 | 105 |
| `test_stack` | 键盘模式 POLL→HID、6KRO/NKRO 切换、**丢 4 帧后 RETRIEVE 补帧恢复**、遥控模式**枚举→9 键名称分片上传→REPORT 按键边沿→改名→版本失效重拉**、跳频跟随、离线判定、Flash 双 Bank（含"Bank A 被擦掉还能从 B 读回"）、JSON 导出与截断安全 | 97 |
| | **合计** | **308** |

两轮都是 **零告警**（`-std=gnu99 -Wall -Wextra -Wpedantic`）。

---

## 七、资源占用与注意事项

* 主机侧设备表 `RC_HOST_MAX_DEV` 默认 **6**：一个设备约 260 字节（含 9×19 字节名称），
  6 个约 1.6 KB。规范写 1:16，但 16 个要 ~4 KB，和 BLE 键盘协议栈一起放不下，
  而且 135px 高的屏幕一屏也只显示得下 6 行。要跑满 16 路就调大这个宏（协议本身支持 16）。
* 名称重组槽 `RC_KEYMAP_RX_SLOTS` 默认 2（每个约 210 字节），槽位是共享复用的。
* 从机低功耗：拿 `Tdma_DevSleepUs()` 算出睡多久，用 RTC 唤醒 + `__WFI()`，
  唤醒后 `HALRF_DelayUs(50)` 再开收（规范 9.2 / FAQ 里点名的坑）。
  这部分要接板子的 RTC，所以没写进 `hal_rf_ch592.c`，见 README 说明。
* 中间还有一步没做：`RF_Config` 每次换 accessAddress 都要重写寄存器，
  实测代价不小。单地址方案下只有开机和枚举窗口会调，正常运行时只走
  `RF_SetFrequency()`（换频点），开销可以忽略。

---

## 八、和工程里其它模块的关系

* **和蓝牙键盘（`bluetooth\`）**：两者抢同一个 2.4G 射频。规范 3.1 建议
  **频道带隔离**——BLE 走 0~39 数据信道，本协议键盘模式走 `{20,35,50}`、
  遥控模式走 `{5,12,19,26,33,40,47,54}`，遥控枚举走 CH20。
  真正同场部署时建议**分时**：键盘模式用 2.4G 私有协议 + USB，遥控模式再切过去。
* **和游戏（`games\`）**：互不依赖。`gt_rc` 这类"遥控器控制"的玩法，
  只要在 `Rc_HostSetKeyCallback()` 的回调里把按键事件喂给游戏输入层即可。
* **数据 Flash 分区**：`game_score` 占 `0x0000~0x00FF`，本模块用
  `RF_CFG_PAGE_A_OFF=0x0100` / `0x0200` 两个 Bank，不冲突。
