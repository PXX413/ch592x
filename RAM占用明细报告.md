# CH592X 工程 RAM 占用明细（按模块）

> 数据来源：`obj/CH592X.map`（链接产物），由 `obj/_stk/ram.py` 解析。
> 重新生成：`python obj/_stk/ram.py obj/CH592X.map`
>
> RAM 区 = `[0x20000000, 0x20006800)`，26 KB = 26624 B。
> 只有四个输出段落在 RAM（见 `Ld/Link.ld`）：
> `.highcode`（中断向量 + `__HIGH_CODE` 函数，运行期必须在 RAM）、
> `.data`、`.bss`、`.stack`。`.rodata` / `.text` 全在 Flash，不计入。

==============================================================================
一、RAM 段总量    RAM 区 [0x20000000, 0x20006800) = 26 KB = 26624 B
==============================================================================
  段          VMA               字节   含义
  --------------------------------------------------------------------------
  .highcode  0x20000000      8712   中断向量表 + __HIGH_CODE 函数（运行期必须在 RAM）
  .data      0x20002208      2080   已初始化的全局/静态变量
  .bss       0x20002a28     13104   未初始化 + .sbss + COMMON
  --------------------------------------------------------------------------
                            23896   已分配小计
  .stack     0x20006400      1024   __stack_size（Link.ld 设定）
  heap                       1704   堆预留 [_ebss=0x20005D58, _heap_end=0x20006400)
  --------------------------------------------------------------------------
  总计                        24920   合计（占用率 93.60%）
  注：堆这 1704 B 目前是"预留未分配"，只有 malloc 才吃掉；
      rc_host_t 需 1628 B，装得下（余 76 B）。

==============================================================================
二、按模块归集（.highcode + .data + .bss，不含栈/堆）
==============================================================================
  模块                                 highcode    .data     .bss        合计      占比
  --------------------------------------------------------------------------
  bluetooth BLE 应用层                       262     1179     6608      8049   33.7%
  libCH59xBLE.a WCH BLE 协议栈              6138      271     1428      7837   32.8%
  keyboard 键盘/USB/按键映射                     12       13     2382      2407   10.1%
  libISP592.a WCH 下载/ISP 库               1062        0        0      1062    4.4%
  ui 界面                                     0       59      804       863    3.6%
  StdPeriphDriver 标准外设                    788        4       16       808    3.4%
  hardware 硬件驱动                             0      206      470       676    2.8%
  rf24 2.4G 协议 + 射频 HAL                   202       12      381       595    2.5%
  games 小游戏                                 0       27      480       507    2.1%
  newlib-nano C 运行时                         0       92      328       420    1.8%
  User 主程序                                 94        4       62       160    0.7%
  Startup 启动/中断向量                         146        0        0       146    0.6%
  --------------------------------------------------------------------------
  模块合计                                   8704     1867    12959     23530
  段头声明                                 8712     2080    13104     23896
  差异                                        8      213      145       366   ← 段内 4/8 字节对齐填充 *fill*

==============================================================================
三、单文件 RAM 占用 top 25
==============================================================================
  目标文件                                                      段       字节
  --------------------------------------------------------------------------
  ./bluetooth/bt_wch.o                                    d+b     6225
  libCH59xBLE.a(ll_connect.o)                          hc+d+b     1604
  libCH59xBLE.a(tmos.o)                                hc+d+b     1428
  libISP592.a(ISP583.o)                                    hc     1062
  libCH59xBLE.a(ip.o)                                  hc+d+b     1050
  ./keyboard/kmap_store.o                                 d+b      855
  libCH59xBLE.a(ll_hop.o)                                  hc      784
  ./StdPeriphDriver/CH59x_sys.o                            hc      764
  ./keyboard/usb_cdc.o                                    d+b      764
  ./keyboard/usb_kbd_port_ch592.o                      hc+d+b      628
  ./bluetooth/hidkbdservice.o                             d+b      572
  libCH59xBLE.a(memory.o)                              hc+d+b      518
  ./bluetooth/devinfoservice.o                            d+b      368
  libCH59xBLE.a(rf_fh.o)                               hc+d+b      346
  ./rf24/hal_rf_ch592.o                                hc+d+b      328
  libg_nano.a(libc_a-findfp.o)                            d+b      328
  libCH59xBLE.a(ll.o)                                     d+b      296
  ./hardware/tft_st7789.o                                 d+b      288
  libCH59xBLE.a(bb.o)                                  hc+d+b      272
  ./rf24/rf_cfg.o                                         d+b      256
  ./games/game_port.o                                     d+b      253
  ./bluetooth/bt_port.o                                   d+b      247
  ./ui/ui_pages2.o                                        d+b      234
  ./bluetooth/ble_task_scheduler.o                         hc      212
  libCH59xBLE.a(gap.o)                                    d+b      204

==============================================================================
四、单个符号 RAM 占用 top 20（只统计 .highcode/.data/.bss）
==============================================================================
  符号                           段                字节  目标文件
  --------------------------------------------------------------------------
  MEM_BUF                      .bss           6144  bt_wch.o
  FLASH_EEPROM_CMD             .highcode       874  libISP592.a(ISP583.o)
  ll_get_connect_anchorPoint   .highcode       752  libCH59xBLE.a(ll_connect.o)
  LL_HopGetChannel2            .highcode       618  libCH59xBLE.a(ll_hop.o)
  TMOS_SystemProcess           .highcode       518  libCH59xBLE.a(tmos.o)
  SetSysClock                  .highcode       472  CH59x_sys.o
  ll_tx_wait_finish            .highcode       392  libCH59xBLE.a(ip.o)
  __sf                         .bss            312  libg_nano.a(libc_a-findfp.o)
  ll_connect_send_data         .highcode       306  libCH59xBLE.a(ll_connect.o)
  gBleLlPara                   .bss            296  libCH59xBLE.a(ll.o)
  tmos_memory_free             .highcode       264  libCH59xBLE.a(memory.o)
  BB_IRQLibHandler             .highcode       262  libCH59xBLE.a(bb.o)
  tmos_memory_allocate         .highcode       242  libCH59xBLE.a(memory.o)
  SYS_ResetExecute             .highcode       234  CH59x_sys.o
  ll_connect_recv_data         .highcode       224  libCH59xBLE.a(ll_connect.o)
  ll_connect_event_continue    .highcode       202  libCH59xBLE.a(ll_connect.o)
  RF_FrequencyHoppingGet       .highcode       168  libCH59xBLE.a(rf_fh.o)
  LL_HopGetChannel             .highcode       166  libCH59xBLE.a(ll_hop.o)
  ble_ll_chkcrc                .highcode       146  libCH59xBLE.a(ip.o)
  LLE_IRQHandler               .highcode       134  ble_task_scheduler.o
