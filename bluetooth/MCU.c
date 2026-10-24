/********************************** (C) COPYRIGHT *******************************
 * File Name          : MCU.c
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        : 硬件任务处理函数及BLE和硬件初始化
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
#include "HAL.h"
#include "CH59x_common.h"   /* __risc_v_disable_irq / __risc_v_enable_irq */

/* 【卡死定位】用主循环的阶段码显示 Flash 操作进度（20/21/22），
 * 由 User/Main.c 定义。SNV 写在蓝牙配对路径上，如果卡死发生在 ROM 的
 * 擦除/写入里，冻结的 S 字段会直接指出来。 */
extern volatile uint8_t g_loop_stage;
extern volatile uint16_t g_temp_calls;

tmosTaskID halTaskID;
uint32_t g_LLE_IRQLibHandlerLocation;
/*******************************************************************************
 * @fn      Lib_Calibration_LSI
 *
 * @brief   内部32k校准
 *
 * @param   None.
 *
 * @return  None.
 */
void Lib_Calibration_LSI(void)
{
    Calibration_LSI(Level_64);
}

#if(defined(BLE_SNV)) && (BLE_SNV == TRUE)
/*******************************************************************************
 * @fn      Lib_Read_Flash
 *
 * @brief   Callback function used for BLE lib.
 *
 * @param   addr - Read start address
 * @param   num - Number of units to read (unit: 4 bytes)
 * @param   pBuf - Buffer to store read data
 *
 * @return  None.
 */
uint32_t Lib_Read_Flash(uint32_t addr, uint32_t num, uint32_t *pBuf)
{
    EEPROM_READ(addr, pBuf, num * 4);
    return 0;
}
/*******************************************************************************
 * @fn      Lib_Write_Flash_592A
 *
 * @brief   Callback function used for BLE lib.
 *
 * @param   addr - Write start address
 * @param   num - Number of units to write (unit: 4 bytes)
 * @param   pBuf - Buffer with data to be written
 *
 * @return  None.
 */
/* 【2026-10-14】原来这个缓冲是**栈上**数组（FLASH_BUF[64] = 256 字节），
 * 实测 Lib_Write_Flash_592A 栈帧 288 字节。
 *
 * 它由 cfg.writeFlashCB 注册，被协议栈在**配对回调上下文**里同步调用 ——
 * 也就是 TMOS_SystemProcess() 调用链的中段，此时栈上已叠了协议栈若干层。
 * 栈上限只有 2048 字节（Ld/Link.ld: __stack_size = 2048），历史实测最深
 * 链路 688 字节，余量并不宽裕。256 字节放栈上没有必要：
 *
 *   * 本函数是**同步串行**的（协议栈一次只写一次 SNV），不存在重入，
 *     一个静态缓冲区完全够用；
 *   * 常驻 .bss 不占栈，深处调用时不再加剧栈压力；
 *   * 顺带消除"栈上大数组在中断/异常里可能被踩"的隐患。
 *
 * 代价是常驻 256 字节 RAM。Main.c 里有链接期堆余量护栏（heap_bytes()），
 * 多这 256 字节是安全的。 */
static __attribute__((aligned(4))) uint32_t s_snv_buf[(BLE_SNV_BLOCK*BLE_SNV_NUM) / 4];

void Lib_Write_Flash_592A(uint32_t addr, uint32_t num, uint32_t *pBuf)
{
    uint8_t   saved = g_loop_stage;
    uint32_t *FLASH_BUF = s_snv_buf;      /* 指向静态缓冲，不再占栈 */

    g_loop_stage = 20u;       /* SNV 读回整块 */
    EEPROM_READ(addr&0xFFFFF000, FLASH_BUF, BLE_SNV_BLOCK*BLE_SNV_NUM);
    tmos_memcpy(&FLASH_BUF[addr&0xFFF], pBuf, num * 4);
    g_loop_stage = 21u;       /* SNV 擦 4KB 扇区 */
    EEPROM_ERASE(addr&0xFFFFF000, ((BLE_SNV_BLOCK*BLE_SNV_NUM+EEPROM_BLOCK_SIZE-1)/EEPROM_BLOCK_SIZE)*EEPROM_BLOCK_SIZE);
    g_loop_stage = 22u;       /* SNV 写回 */
    EEPROM_WRITE(addr&0xFFFFF000, FLASH_BUF, BLE_SNV_BLOCK*BLE_SNV_NUM);
    g_loop_stage = saved;
}

/*******************************************************************************
 * @fn      Lib_Write_Flash
 *
 * @brief   Callback function used for BLE lib.
 *
 * @param   addr - Write start address
 * @param   num - Number of units to write (unit: 4 bytes)
 * @param   pBuf - Buffer with data to be written
 *
 * @return  None.
 */
uint32_t Lib_Write_Flash(uint32_t addr, uint32_t num, uint32_t *pBuf)
{
    if(((*(uint32_t*)ROM_CFG_VERISON)&0xFF) == DEF_CHIP_ID_CH592A)
    {
        Lib_Write_Flash_592A(addr, num, pBuf);
    }
    else
    {
        EEPROM_ERASE(addr, num * 4);
        EEPROM_WRITE(addr, pBuf, num * 4);
    }
    return 0;
}
#endif

/*******************************************************************************
 * @fn      CH59x_BLEInit
 *
 * @brief   BLE 库初始化
 *
 * @param   None.
 *
 * @return  None.
 */
void CH59x_BLEInit(void)
{
    uint8_t     i;
    bleConfig_t cfg;
    if(tmos_memcmp(VER_LIB, VER_FILE, strlen(VER_FILE)) == FALSE)
    {
        PRINT("head file error...\n");
        while(1);
    }

    SysTick_Config(SysTick_LOAD_RELOAD_Msk);// 配置SysTick并打开中断
    PFIC_DisableIRQ(SysTick_IRQn);

    g_LLE_IRQLibHandlerLocation = (uint32_t)LLE_IRQLibHandler;
    PFIC_SetPriority(BLEL_IRQn, 0xF0);
    tmos_memset(&cfg, 0, sizeof(bleConfig_t));
    cfg.MEMAddr = (uint32_t)MEM_BUF;
    cfg.MEMLen = (uint32_t)BLE_MEMHEAP_SIZE;
    cfg.BufMaxLen = (uint32_t)BLE_BUFF_MAX_LEN;
    cfg.BufNumber = (uint32_t)BLE_BUFF_NUM;
    cfg.TxNumEvent = (uint32_t)BLE_TX_NUM_EVENT;
    cfg.TxPower = (uint32_t)BLE_TX_POWER;
#if(defined(BLE_SNV)) && (BLE_SNV == TRUE)
    /* 上界 = DataFlash 容量：EEPROM_* 的地址是以 0x70000 为基址的偏移，
     * 0x78000 - 0x70000 = 0x8000 = DataFlash 大小。原值本来就正确，勿改。 */
    if((BLE_SNV_ADDR + BLE_SNV_BLOCK * BLE_SNV_NUM) > (0x78000 - FLASH_ROM_MAX_SIZE))
    {
        PRINT("SNV config error...\n");
        while(1);
    }
    cfg.SNVAddr = (uint32_t)BLE_SNV_ADDR;
    cfg.SNVBlock = (uint32_t)BLE_SNV_BLOCK;
    cfg.SNVNum = (uint32_t)BLE_SNV_NUM;
    cfg.readFlashCB = Lib_Read_Flash;
    cfg.writeFlashCB = Lib_Write_Flash;
#endif
    cfg.ConnectNumber = (PERIPHERAL_MAX_CONNECTION & 3) | (CENTRAL_MAX_CONNECTION << 2);
    cfg.srandCB = SYS_GetSysTickCnt;
#if(defined TEM_SAMPLE) && (TEM_SAMPLE == TRUE)
    cfg.tsCB = HAL_GetInterTempValue; // 根据温度变化校准RF和内部RC( 大于7摄氏度 )
  #if(CLK_OSC32K)
    cfg.rcCB = Lib_Calibration_LSI; // 内部32K时钟校准
  #endif
#endif
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    cfg.idleCB = CH59x_LowPower; // 启用睡眠
#endif
#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
    for(i = 0; i < 6; i++)
    {
        cfg.MacAddr[i] = MacAddr[5 - i];
    }
#else
    {
        uint8_t MacAddr[6];
        GetMACAddress(MacAddr);
        for(i = 0; i < 6; i++)
        {
            cfg.MacAddr[i] = MacAddr[i]; // 使用芯片mac地址
        }
    }
#endif
    if(!cfg.MEMAddr || cfg.MEMLen < 4 * 1024)
    {
        while(1);
    }
    if(cfg.TxPower & (1<<7))
    {
        sys_safe_access_enable();
        *( PUINT32V)(0x40001048) |= (6<<6);
        *( PUINT32V)(0x40001020) |= (2<<19);
        sys_safe_access_disable();

    }
    i = BLE_LibInit(&cfg);
    if(i)
    {
        PRINT("LIB init error code: %x ...\n", i);
        while(1);
    }
}

/*******************************************************************************
 * @fn      HAL_ProcessEvent
 *
 * @brief   硬件层事务处理
 *
 * @param   task_id - The TMOS assigned task ID.
 * @param   events  - events to process.  This is a bit map and can
 *                      contain more than one event.
 *
 * @return  events.
 */
tmosEvents HAL_ProcessEvent(tmosTaskID task_id, tmosEvents events)
{
    uint8_t *msgPtr;

    if(events & SYS_EVENT_MSG)
    { // 处理HAL层消息，调用tmos_msg_receive读取消息，处理完成后删除消息。
        msgPtr = tmos_msg_receive(task_id);
        if(msgPtr)
        {
            /* De-allocate */
            tmos_msg_deallocate(msgPtr);
        }
        return events ^ SYS_EVENT_MSG;
    }
    if(events & LED_BLINK_EVENT)
    {
#if(defined HAL_LED) && (HAL_LED == TRUE)
        HalLedUpdate();
#endif // HAL_LED
        return events ^ LED_BLINK_EVENT;
    }
    if(events & HAL_KEY_EVENT)
    {
#if(defined HAL_KEY) && (HAL_KEY == TRUE)
        HAL_KeyPoll(); /* Check for keys */
        tmos_start_task(halTaskID, HAL_KEY_EVENT, MS1_TO_SYSTEM_TIME(100));
#endif
        return events ^ HAL_KEY_EVENT;
    }
    if(events & HAL_REG_INIT_EVENT)
    {
#if(defined BLE_CALIBRATION_ENABLE) && (BLE_CALIBRATION_ENABLE == TRUE) && (CLK_OSC32K == 0)
        /* 只有"外部 32K 晶振"档才会用到这个局部变量；
         * 内部 RC（CLK_OSC32K=1，本工程默认）时它是死代码，会报未使用告警。 */
        uint8_t x32Kpw;
#endif
#if(defined BLE_CALIBRATION_ENABLE) && (BLE_CALIBRATION_ENABLE == TRUE) // 校准任务，单次校准耗时小于10ms
        BLE_RegInit();                                                  // 校准RF
#if(CLK_OSC32K)
        Lib_Calibration_LSI(); // 校准内部RC
#else
        x32Kpw = (R8_XT32K_TUNE & 0xfc) | 0x01;
        sys_safe_access_enable();
        R8_XT32K_TUNE = x32Kpw; // LSE驱动电流降低到额定电流
        sys_safe_access_disable();
#endif
        tmos_start_task(halTaskID, HAL_REG_INIT_EVENT, MS1_TO_SYSTEM_TIME(BLE_CALIBRATION_PERIOD));
        return events ^ HAL_REG_INIT_EVENT;
#endif
    }
    if(events & HAL_TEST_EVENT)
    {
        PRINT("* \n");
        tmos_start_task(halTaskID, HAL_TEST_EVENT, MS1_TO_SYSTEM_TIME(1000));
        return events ^ HAL_TEST_EVENT;
    }
    return 0;
}

/*******************************************************************************
 * @fn      HAL_Init
 *
 * @brief   硬件初始化
 *
 * @param   None.
 *
 * @return  None.
 */
void HAL_Init()
{
    halTaskID = TMOS_ProcessEventRegister(HAL_ProcessEvent);
    HAL_TimeInit();
#if(defined HAL_SLEEP) && (HAL_SLEEP == TRUE)
    HAL_SleepInit();
#endif
#if(defined HAL_LED) && (HAL_LED == TRUE)
    HAL_LedInit();
#endif
#if(defined HAL_KEY) && (HAL_KEY == TRUE)
    HAL_KeyInit();
#endif
#if(defined BLE_CALIBRATION_ENABLE) && (BLE_CALIBRATION_ENABLE == TRUE)
    tmos_start_task(halTaskID, HAL_REG_INIT_EVENT, 800); // 添加校准任务，500ms启动，单次校准耗时小于10ms
#endif
//    tmos_start_task( halTaskID, HAL_TEST_EVENT, 1600 );    // 添加一个测试任务
}

/*******************************************************************************
 * @fn      HAL_GetInterTempValue
 *
 * @brief   获取内部温感采样值，如果使用了ADC中断采样，需在此函数中暂时屏蔽中断.
 *
 * @return  内部温感采样值.
 */
uint16_t HAL_GetInterTempValue(void)
{
    uint8_t  sensor, channel, config, tkey_cfg;
    uint16_t adc_data;
    uint32_t intlev;
    uint8_t  saved_stage;

    g_temp_calls++;
    saved_stage  = g_loop_stage;
    g_loop_stage = 30u;                 /* FW35 诊断：进入温感采样 */

    /* ===== FW35 卡死修复 第 4 层：整段 ADC 操作关中断 =====
     * 本函数被 BLE 协议栈当温感回调调用（bluetooth/MCU.c:172  cfg.tsCB，注释写"大于7秒触发一次"），
     * 而主循环的电量采样（hardware/battery.c 的 bat_read_adc）用的是**同一个 ADC**，
     * 两边原先既没有互斥、也没有超时。谁插进对方的"置 START / 等 START 清零"之间，
     * 都会把正在进行的转换搞坏，START 位可能再也不自动清零 -> 无限循环 -> 整机卡死。
     *
     * 官方注释（本文件 :312）本来就写着：「如果使用了ADC中断采样，需在此函数中暂时屏蔽中断」——
     * 这里照做。配合 bat_read_adc() 那侧也关中断，这条竞争才算从两头都关上。
     * ADC 转换只有几十微秒，关中断不会影响 BLE 时序。 */
    intlev = __risc_v_disable_irq();

    tkey_cfg = R8_TKEY_CFG;
    sensor = R8_TEM_SENSOR;
    channel = R8_ADC_CHANNEL;
    config = R8_ADC_CFG;
    ADC_InterTSSampInit();
    R8_ADC_CONVERT |= RB_ADC_START;
    g_loop_stage = 31u;                 /* FW35 诊断：等待转换完成 */
    /* 【2026-10-14 卡死修复】这里原来是无超时死等，而本函数是被 BLE
     * 协议栈当温感回调调的（bluetooth/MCU.c:172 cfg.tsCB），同时主循环的
     * 电量采样也在用同一个 ADC（hardware/battery.c 的 bat_read_adc），
     * 两边都不做互斥 —— START 位一旦被交错破坏就永远不清零，整机卡死。
     * 这里同样限死等待次数：超时就把上一次的读数返回去，不死机。 */
    {
        uint32_t _n = 20000u;
        while ((R8_ADC_CONVERT & RB_ADC_START) != 0u)
        {
            if (_n == 0u) { break; }
            _n--;
        }
    }
    adc_data = R16_ADC_DATA;
    R8_TEM_SENSOR = sensor;
    R8_ADC_CHANNEL = channel;
    R8_ADC_CFG = config;
    R8_TKEY_CFG = tkey_cfg;
    __risc_v_enable_irq(intlev);

    g_loop_stage = saved_stage;
    return (adc_data);
}

/******************************** endfile @ mcu ******************************/
