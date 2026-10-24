/*==========================================================================
 * 栈大小
 *
 * 【P2 修复】640 -> 1024。与 rf24/proto_rc.h 的
 *     RC_HOST_MAX_DEV   6 -> 4
 *     RC_KEYMAP_RX_SLOTS 2 -> 1
 * 三者必须一起改，单独改任何一个都会坏（见下方账）。
 *
 * 为什么必须加（objdump 实测，不是 -fstack-usage）：
 *   CH592X 的 __INTERRUPT 并没有传说中那么贵 —— 全工程 0 条 fsd 指令，
 *   中断入口实测：TMR0/USB/HardFault 都是 0 B，LLE_IRQHandler 128 B。
 *   也没有任何 handler 写 mstatus，所以中断**不嵌套**，不用为嵌套预留。
 *   真正的开销在**主线程调用链**：
 *       main(80) -> radio_task(16) -> BT_WCH_Init(48) -> CH59x_BLEInit(96)
 *       -> iprintf(64) -> _vfiprintf_r(176) -> __swsetup_r(16)
 *       -> __smakebuf_r(48) -> __swhatbuf_r(128) -> _fstat_r(16)  = 688 B
 *   也就是说每次开机走到 CH59x_BLEInit() 里的 puts/iprintf 就要 688 B，
 *   原 640 B 栈从开机第一秒起就溢出 48 B（溢出方向是往下踩堆）。
 *   全工程最大单帧 288 B（Lib_Write_Flash_592A）。
 *   最坏情况 = 主线程 688 + LLE 中断 128 = 816 B -> 取 1024，余 208 B。
 *
 * 代价与平衡：RAM 顶端是**栈和堆共用**的
 *     可用 = 0x20006800 - _ebss
 *     堆   = 可用 - __stack_size（_heap_end = RAM 顶端 - __stack_size）
 *
 * 堆里唯一的大块是遥控主机结构体 rc_host_t（进遥控页才 _sbrk 申请）。
 *
 * ★★ 2026-10-14：栈 1024 -> 2048，把遥控省下来的堆让给栈 ★★
 *
 * 为什么动：上面那行"最坏 816 B -> 取 1024，余 208 B"是按**单帧**估的，
 *   没算上"配对 + SNV 写 Flash + 射频中断嵌套"同时发生的情形。
 *   而**栈就在堆的上面（往下长）** —— 栈一踩穿，先冲掉的就是堆里的数据，
 *   表现是各种莫名其妙的偶发故障（蓝牙连不稳、遥控设备表错乱）。
 *   208 B 的余量对这种叠加场景太薄了。
 *
 * 堆为什么能让：协议瘦身之后 rc_host_t 从 1596 B 掉到约 540 B
 *   （键名不再从从机上传，主机不再为键名驻留 RAM，详见《遥控模式开发文档》），
 *   进遥控页的堆需求从 1628 B 降到约 570 B —— 省出来一千多字节。
 *
 * 账（改后实测，obj/CH592X.map + User/Main.c 静态断言双重把关）：
 *     _ebss = 0x20005a28
 *     可用（栈+堆）= 0x20006800 - 0x20005a28 = 3544 B
 *     栈   = 2048 B   （比原来翻倍，余量从 208 B 涨到 1232 B）
 *     堆   = 1496 B
 *     需求 = sizeof(rc_host_t) ~540 + 32 = 约 570 B
 *     余量 = 约 926 B  （原来只有 68 B）
 *     （cmdq/last_rpt_ms 仍按 RF_RC_MAX_DEV 定尺：它们按协议 dev_id 索引，
 *      dev_id 空间是 1..6，不能跟着 RC_HOST_MAX_DEV 一起缩）
 *
 * 想再加大栈：继续往下吃堆即可，但 1496 B 的堆要留着给 rc_host_t（约 570 B）
 *   和其它 malloc（网页配置、JSON 生成等）。栈超过 2560 就要重新算这笔账。
 *========================================================================*/
/* 【回归修复】上一轮重写本注释块时误删了 ENTRY(_start)。
 * 芯片实际从 Flash 0x0 的 _start 启动，删掉它不影响运行，
 * 但 ELF 头的入口点会从 0x0 漂到 0x221c（ld 的兜底值），
 * 调试器 / wchisp 按 ELF 入口复位时会跳错地方。必须加回来。 */
/*==========================================================================
 * 【2026-10-14 二次调整 FW39】__stack_size 2048 -> 1536。
 *
 * 起因：游戏列表页"能左右切、按下去没反应、也不卡死"。根因是堆不够：
 *     _ebss = 0x20005d10，__stack_size 2048 => _heap_end = 0x20006000
 *     => 堆只剩 752 B，而游戏状态池 game_pool_t（雷霆 46 个对象 x 20 B）
 *     要 920 B（games/game_state.c 的 GSTATE_POOL_BYTES）。
 *     malloc() -> _sbrk() 一看越界直接返回 0，于是
 *     Game_PoolAcquire() 返回 false，而 ui/ui_pages.c 的 game_start()
 *     第一句就是 if (!Game_PoolAcquire()) { return; }  —— 纯静默。
 *     所以表现成"进不去、不报错、也不卡死"。FW37/FW38 上就已经是这样。
 *
 * 为什么敢从 2048 退到 1536（而不是回到 1024）：
 *   上面那句"最坏 816 B"的账里，最大的一段是 CH59x_BLEInit() 里的
 *   iprintf -> _vfiprintf_r -> __swsetup_r -> __smakebuf_r -> _fstat_r
 *   那条 newlib stdio 链（688 B）。**FW37 删掉 -DDEBUG 之后整条链已经不
 *   在镜像里了**（nm 查 iprintf/_vfiprintf_r/__swsetup_r/__smakebuf_r/
 *   _fstat_r 全为空），所以今天主线程的真实最坏栈深远小于 688 B。
 *   另外 rf24/rf_cfg.c 的 RfCfg_RcSave / RfCfg_KbSave / RfCfg_RcLoad
 *   三个大局部（784 / 528 / 528 B）在 2026-10-12 已搬去 static，
 *   不再吃栈。
 *   1536 B 相对今天的实际用量仍有很大余量，而堆 752 -> 1264 B，
 *   一次解决"游戏进不去"，顺带给将来 WS2812 的 DMA 帧缓冲（864 B）
 *   留出了位置。
 *
 * 上机验证办法：开机 "BT step n FWxx SK<数字>" 里的 SK 就是
 *   User/Main.c 的栈高水位 g_stack_hw（= _eusrstack - 用过的最深处）。
 *   SK 明显小于 1536 就安全；若接近，把 __stack_size 调回来即可。
 *========================================================================*/
ENTRY( _start )

__stack_size = 1536;

PROVIDE( _stack_size = __stack_size );

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 448K
	RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 26K
}

SECTIONS
{
	.init :
	{
		_sinit = .;
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.init)))
		. = ALIGN(4);
		_einit = .;
	} >FLASH AT>FLASH
	
 /*   .vector :
    {
        *(.vector);
    } >FLASH AT>FLASH */
    
    .highcodelalign : 
    {       
        . = ALIGN(4);
        PROVIDE(_highcode_lma = .); 
    } >FLASH AT>FLASH 
    
    .highcode : 
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        *(.vector);
		KEEP(*(SORT_NONE(.vector_handler)))
        *(.highcode);
        *(.highcode.*);
		. = ALIGN(4); 
        PROVIDE(_highcode_vma_end = .);
    } >RAM AT>FLASH
     
	.text :
	{
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.handle_reset)))
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata*)
  *(.sdata2.*)
		*(.glue_7)
		*(.glue_7t)
		*(.gnu.linkonce.t.*)
		. = ALIGN(4);
	} >FLASH AT>FLASH 

	.fini :
	{
		KEEP(*(SORT_NONE(.fini)))
		. = ALIGN(4);
	} >FLASH AT>FLASH

	PROVIDE( _etext = . );
	PROVIDE( _eitcm = . );	

	.preinit_array  :
	{
	  PROVIDE_HIDDEN (__preinit_array_start = .);
	  KEEP (*(.preinit_array))
	  PROVIDE_HIDDEN (__preinit_array_end = .);
	} >FLASH AT>FLASH 
	
	.init_array     :
	{
	  PROVIDE_HIDDEN (__init_array_start = .);
	  KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
	  KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
	  PROVIDE_HIDDEN (__init_array_end = .);
	} >FLASH AT>FLASH 
	
	.fini_array     :
	{
	  PROVIDE_HIDDEN (__fini_array_start = .);
	  KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
	  KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
	  PROVIDE_HIDDEN (__fini_array_end = .);
	} >FLASH AT>FLASH 
	
	.ctors          :
	{
	  /* gcc uses crtbegin.o to find the start of
	     the constructors, so we make sure it is
	     first.  Because this is a wildcard, it
	     doesn't matter if the user does not
	     actually link against crtbegin.o; the
	     linker won't look for a file to match a
	     wildcard.  The wildcard also means that it
	     doesn't matter which directory crtbegin.o
	     is in.  */
	  KEEP (*crtbegin.o(.ctors))
	  KEEP (*crtbegin?.o(.ctors))
	  /* We don't want to include the .ctor section from
	     the crtend.o file until after the sorted ctors.
	     The .ctor section from the crtend file contains the
	     end of ctors marker and it must be last */
	  KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
	  KEEP (*(SORT(.ctors.*)))
	  KEEP (*(.ctors))
	} >FLASH AT>FLASH 
	
	.dtors          :
	{
	  KEEP (*crtbegin.o(.dtors))
	  KEEP (*crtbegin?.o(.dtors))
	  KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
	  KEEP (*(SORT(.dtors.*)))
	  KEEP (*(.dtors))
	} >FLASH AT>FLASH 

	.dalign    : 
	{
		. = ORIGIN(RAM) + SIZEOF(.highcode);
	} >RAM AT>FLASH	

	.dlalign :
	{
		. = ALIGN(4); 
		PROVIDE(_data_lma = .);
	} >FLASH AT>FLASH

	.data :
	{
		. = ALIGN(4); 
		PROVIDE(_data_vma = .);
    	*(.gnu.linkonce.r.*)
    	*(.data .data.*)
    	*(.gnu.linkonce.d.*)
		. = ALIGN(8);
    	PROVIDE( __global_pointer$ = . + 0x800 );
    	*(.sdata .sdata.*)
    	*(.gnu.linkonce.s.*)
    	. = ALIGN(8);
    	*(.srodata.cst16)
    	*(.srodata.cst8)
    	*(.srodata.cst4)
    	*(.srodata.cst2)
    	*(.srodata .srodata.*)
    	. = ALIGN(4);
		PROVIDE( _edata = .);
	} >RAM AT>FLASH

	.bss :
	{
		. = ALIGN(4);
		PROVIDE( _sbss = .);
  	    *(.sbss*)
        *(.gnu.linkonce.sb.*)
		*(.bss*)
     	*(.gnu.linkonce.b.*)		
		*(COMMON*)
		. = ALIGN(4);
		PROVIDE( _ebss = .);
	} >RAM AT>FLASH

	PROVIDE( _end = _ebss);
	PROVIDE( end = . );
	
    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        PROVIDE( _heap_end = . );   
        . = ALIGN(4);
        PROVIDE(_susrstack = . );
        . = . + __stack_size;
        PROVIDE( _eusrstack = .);
    } >RAM 
}



