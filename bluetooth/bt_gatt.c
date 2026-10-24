/**************************************************************************
 * bt_gatt.c —— 把官方 HID profile 挂到 BLE 协议栈上
 *
 * 调用次序照 `HID_Keyboard\APP\hidkbd.c` 的 HidEmu_Init()（第 244/247 行）：
 *     Hid_AddService();                              注册 HID over GATT 本体
 *     HidDev_Register(&hidEmuCfg, &hidEmuHidCBs);     配 hiddev 的回调
 * 注意官方**没有**单独调 HidDev_RegisterReports()：报告映射由 hidkbdservice
 * 内部完成，不要自己塞一张表（我一开始就写错了，见文件末尾的备注）。
 *
 * 报文格式（重要）：官方 buf[HID_KEYBOARD_IN_RPT_LEN] = buf[8]，
 *     buf[0] 修饰键 / buf[1] 保留 / buf[2..7] 6 个键码
 * 这和我们 `bt_hid.c` 产出的 8 字节 boot 报文**逐字节一致**，
 * 所以这里**直接原样传**，不需要重排——重排反而是 bug。
 **************************************************************************/

#include "bt_gatt.h"
#include "CONFIG.h"
#include "hiddev.h"
#include "hidkbdservice.h"
#include "battservice.h"
#include "devinfoservice.h"
#include "scanparamservice.h"

#include <string.h>

/* 输入报告 READ 时回当前按键状态（8 字节 boot 报文） */
#include "bt_hid.h"

/*==========================================================================
 * 状态
 *========================================================================*/
static uint8_t  s_ready;
static uint16_t s_conn_handle;

/* 最近一次由应用推过来的真实电量（0~100），初值取满电量。
 * 也是 profile 量测回调 Batt_SetMeasureCB() 的返回值来源。 */
static uint8_t  s_batt_pct = 100u;

static uint8_t gatt_batt_measure(void)
{
    return s_batt_pct;
}

/*--------------------------------------------------------------------------
 * hiddev 事件回调。现在不额外做事，但必须非空：hiddev 状态机里有几处
 * 判空后会跳过分支，留空指针会让协议模式切换/boot 模式请求没人应答。
 *------------------------------------------------------------------------*/
static void gatt_hid_evt_cb(uint8_t evt)
{
    (void)evt;
}

/*--------------------------------------------------------------------------
 * 【修复 BLE-1】reportCB 必须非空 —— 这就是"手机一连就死机"的根因。
 *
 * 官方 hiddev.c 里有三处**不判空**直接调用 reportCB：
 *     行 471  HidDev_ReadAttrCB()  —— 主机读 Report / Boot 报告特征
 *     行 566  HidDev_WriteAttrCB() —— 主机写 LED 输出报告
 *     行 605  HidDev_WriteAttrCB() —— **主机写 CCCD 打开通知**
 * 第三处是手机/电脑连上后**必做**的一步（输入报告的 CCCD 权限是
 * GATT_PERMIT_ENCRYPT_WRITE，所以配对完成后主机立刻就会写它来订阅）。
 * 原来这里填 0，一旦执行就是 "jalr 0"：flash 地址 0 是
 *     _sinit: j handle_reset
 * 于是直接跳回复位入口 —— 表现就是"手机连接/配对到订阅那一步，设备死机"。
 *
 * 语义照官方 HID_Keyboard\APP\hidkbd.c 的 hidEmuRptCB()：
 *   READ    —— 主机读输入报告：回当前 8 字节 boot 报文（无按键时全 0）；
 *              LED/Feature 报告仍走服务层读。
 *   WRITE   —— 主机写 LED 输出报告（CapsLock/NumLock 等）：存进服务层。
 *   ENABLE / DISABLE —— 订阅开关；订阅状态已由 hiddev.c 的 CCCD 回调
 *              记在 HidDev_IsReportSubscribed() 里，这里不用再管。
 *------------------------------------------------------------------------*/
static uint8_t gatt_hid_rpt_cb(uint8_t id, uint8_t type, uint16_t uuid,
                               uint8_t oper, uint16_t *pLen, uint8_t *pData)
{
    uint8_t status = SUCCESS;

    switch (oper)
    {
        case HID_DEV_OPER_READ:
            if ((type == HID_REPORT_TYPE_INPUT) &&
                ((uuid == REPORT_UUID) || (uuid == BOOT_KEY_INPUT_UUID)))
            {
                const uint8_t *cur = BT_HID_Report();
                uint16_t       n   = *pLen;      /* 进入时 = 对端允许的长度上限 */
                uint16_t       i;

                if (n > (uint16_t)HID_KEYBOARD_IN_RPT_LEN)
                {
                    n = (uint16_t)HID_KEYBOARD_IN_RPT_LEN;
                }
                for (i = 0u; i < n; i++)
                {
                    pData[i] = (cur != 0) ? cur[i] : 0u;
                }
                *pLen = n;
            }
            else
            {
                status = Hid_GetParameter(id, type, uuid, pLen, pData);
            }
            break;

        case HID_DEV_OPER_WRITE:
            status = Hid_SetParameter(id, type, uuid, (uint8_t)(*pLen), pData);
            break;

        case HID_DEV_OPER_ENABLE:
        case HID_DEV_OPER_DISABLE:
            break;

        default:
            break;
    }

    return status;
}

/* GAP 状态回调的转发入口（实现在 bt_wch.c）。
 * 这里用前置声明，是为了不让 bt_wch.h 为了一个函数原型把整个 BLE 协议栈
 * 头文件传染给所有 include 它的模块。 */
extern void BT_WCH_OnGapState(gapRole_States_t newState, gapRoleEvent_t *pEvent);

/*--------------------------------------------------------------------------
 * 【修复 BLE-2】pfnStateChange 交给 hiddev 链路
 *
 * 原来应用层自己调 GAPRole_PeripheralStartDevice()（bt_wch.c），把 hiddev
 * 注册的 GAPRole 回调顶掉了 —— hiddev 的 hidDevGapStateCB() 于是永远不会
 * 被调用，它内部的 hidDevGapState / gapConnHandle / hidDevConnSecure 一直
 * 停在初始值，HidDev_Report() 永远只返回 bleNotReady（**BLE 报文一条都发不
 * 出去**），就算能发也会发到句柄 0。
 *
 * 现在按官方 HID_Keyboard 的架构：GAPRole 回调由 hiddev 持有，应用层通过
 * pfnStateChange 收状态（签名与原来的 gapRolesCBs_t.pfnStateChange 一致）。
 * passcodeCB 留 0 = 用默认配对码，hiddev 内部已判空。
 *------------------------------------------------------------------------*/
static hidDevCB_t s_hid_cbs =
{
    .reportCB       = gatt_hid_rpt_cb,
    .evtCB          = gatt_hid_evt_cb,
    .passcodeCB     = 0,
    .pfnStateChange = BT_WCH_OnGapState
};

static hidDevCfg_t s_hid_cfg =
{
    .idleTimeout = 0u,
    .hidFlags    = HID_FEATURE_FLAGS
};

/*--------------------------------------------------------------------------
 * 电池服务回调
 *------------------------------------------------------------------------*/
static void gatt_batt_cb(uint8_t event)
{
    /* 本工程**不使用** profile 自己的周期测量：
     *   hiddev 的 hidDevBattCB() 会在 BATT_LEVEL_NOTI_ENABLED 时启动
     *   BATT_PERIODIC_EVT -> hidDevBattPeriodicTask() -> Batt_MeasLevel()。
     * 我们的电量来自 hardware/battery.c（已标定，带 OCV 曲线，
     * 且在 UI 上每 60s 采一次），由 User/Main.c 每圈
     * BtGatt_SetBatteryLevel() 推进来，所以这里不需要再起一条周期任务。
     * 将来若想改回 profile 自测：在这里处理 BATT_LEVEL_NOTI_ENABLED 启动
     * BATT_PERIODIC_EVT 即可 —— battMeasure() 已经接到应用层电量，
     * 不会再出现"官方空壳恒 20%"的问题。 */
    (void)event;
}

/*********************************************************************
 * @fn      BtGatt_Init
 *********************************************************************/
void BtGatt_Init(void)
{
    if (s_ready != 0u)
    {
        return;
    }

    s_conn_handle = INVALID_CONNHANDLE;

    /* ---- 服务注册顺序（严格不可调换）----
     *
     * HidDev_Init() 内部注册：GGS(GAP) / GATTServApp / DevInfo / Battery / ScanParam
     *
     * Hid_AddService() **必须放在 HidDev_Init() 之后**，因为它内部有两处
     * 强依赖电池服务已经注册：
     *     Batt_GetParameter(BATT_PARAM_SERVICE_HANDLE, &GATT_INCLUDED_HANDLE(...))
     *         -> 返回 battAttrTbl 的 handle，电池服务必须先注册才有效
     *     Batt_GetParameter(BATT_PARAM_BATT_LEVEL_IN_REPORT, &hidRptMap[5])
     *         -> 同样读 battAttrTbl 的 handle / cccdHandle
     * 若顺序颠倒，hidRptMap[5] 会被填入未初始化的 handle，
     * 主机访问 HID 描述符 / 订阅 CCCD 时命中垃圾句柄 -> 死机。
     *
     * 反之，DevInfo / Battery / ScanParam 三个服务**绝不能**在这里再注册一遍
     * （HidDev_Init 内部已有），GATTServApp_RegisterService 是纯追加语义，
     * 重复注册会撑爆 GATT 属性表、导致属性句柄错位。
     */
    HidDev_Init();          /* 1) 先注册 GAP / GATT / DevInfo / Battery / ScanParam */

    /* 电池事件回调也必须在 HidDev_Init() 之后：
     * HidDev_Init() 内部有 Batt_Register(hidDevBattCB)，会覆盖此前的注册。 */
    Batt_Register(gatt_batt_cb);

    (void)Hid_AddService(); /* 2) HID 必须最后，内部要读 Batt 的 service handle */

    HidDev_Register(&s_hid_cfg, &s_hid_cbs);

    /* 【新增】把 profile 的电量量测入口接到应用层：
     * 主机读电量特征时 battMeasure() 直接返回应用推上来的值，
     * 而不是官方那个写死 adc=300（恒定 20%）的空壳。 */
    Batt_SetMeasureCB(gatt_batt_measure);

    s_ready = 1u;
}

/*********************************************************************
 * @fn      BtGatt_SetBatteryLevel
 * @brief   把应用层（hardware/battery.c 标定过）的真实电量推给 BLE 电池服务
 *
 * 可以在主循环里每圈调用：内部去抖，只有百分比真的变了才写值 + 发通知，
 * 所以既不会漏推，也不会把 BLE 连接事件刷爆。
 *
 * 为什么必须由应用来推（原来这条路整个是断的）：
 *   1) 官方 battservice.c 的 battMeasure() 是空壳：里面写死 `adc = 300`，
 *      一行 ADC 都没读 —— 按默认区间换算是**恒定 20%**；
 *   2) battReadAttrCB() 读特征时会用这个假值把 battLevel 往下拉
 *      （只降不升），主机读一次就永久显示 20%；
 *   3) Batt_SetParameter() 原本**没有 BATT_PARAM_LEVEL 分支**，
 *      所以就算调用本函数也只是返回 INVALIDPARAMETER，什么都写不进去；
 *   4) 全工程从来没有调用过本函数 —— 应用侧标定好的电量与 BLE 服务脱节。
 * 前三条已在 battservice.c 里修好，第 4 条由 User/Main.c 每圈推送。
 *********************************************************************/
void BtGatt_SetBatteryLevel(uint8_t percent)
{
    if (s_ready == 0u)
    {
        return;
    }

    if (percent > 100u)
    {
        percent = 100u;
    }

    if (percent == s_batt_pct)
    {
        return;                             /* 没变化：不写、不通知 */
    }

    s_batt_pct = percent;

    (void)Batt_SetParameter(BATT_PARAM_LEVEL, sizeof(percent), &percent);
}

uint8_t BtGatt_GetBatteryLevel(void)
{
    return s_batt_pct;
}

/*********************************************************************
 * @fn      BtGatt_ConnChange
 * @note    changeType 用 LINKDB_STATUS_UPDATE_* ；这里只区分"断开了"和"其它"
 *********************************************************************/
void BtGatt_ConnChange(uint16_t conn_handle, uint8_t change_type)
{
    if (s_ready == 0u)
    {
        return;
    }

    Batt_HandleConnStatusCB(conn_handle, change_type);
    ScanParam_HandleConnStatusCB(conn_handle, change_type);

    if (change_type == LINKDB_STATUS_UPDATE_REMOVED)
    {
        s_conn_handle = INVALID_CONNHANDLE;

        /* 【修复：BLE 反复连接/断开】这里**不能**再调 HidDev_Close()。
         *
         * 原因：hiddev 的 hidDevGapStateCB() 是在**回调返回之后**才执行
         *       hidDevGapState = newState;（hiddev.c 末尾）。而本函数是通过
         *       hidDevCB_t.pfnStateChange 从那个回调里被调用的 —— 也就是说
         *       此刻 hidDevGapState 还停在 GAPROLE_CONNECTED。
         *       于是 HidDev_Close() 走进它的第一个分支：
         *           GAPRole_TerminateLink(gapConnHandle);
         *       在链路已经断开/正在断开的时候再去主动 Terminate，轻则返回错误，
         *       重则把"刚建立起来的下一条链路"一起带走 —— 主机侧的观感就是
         *       "刚连上就掉、掉了又连"的死循环。
         *
         * 清理工作 hiddev 自己已经做全了：hidDevGapStateCB() 在
         * hidDevGapState == GAPROLE_CONNECTED && newState != CONNECTED 时
         * 会调 hidDevDisconnected()，里面逐个通知
         *     Batt_HandleConnStatusCB / ScanParam_HandleConnStatusCB /
         *     hidDevHandleConnStatusCB
         * 并按需重新开广播。本函数只需要维护自己的连接句柄。 */
    }
    else
    {
        s_conn_handle = conn_handle;
    }
}

/*********************************************************************
 * @fn      BtGatt_SendReport
 * @note    rep8 的布局和官方的 buf[8] 一致，直接原样传。
 *********************************************************************/
bool BtGatt_SendReport(const uint8_t *rep8)
{
    uint8_t st;

    if ((s_ready == 0u) || (rep8 == 0))
    {
        return false;
    }

    /* 返回值是 bStatus_t 的窄化形式；SUCCESS == 0。
     * 对端没订阅通知时这里会返回非 0 —— 那是正常现象，不是 bug。 */
    st = HidDev_Report(HID_RPT_ID_KEY_IN, HID_REPORT_TYPE_INPUT,
                       HID_KEYBOARD_IN_RPT_LEN, (uint8_t *)rep8);

    return (st == (uint8_t)SUCCESS);
}

bool BtGatt_IsReady(void)
{
    return (s_ready != 0u);
}

/******************* (C) CH592X BLE GATT 接线 *****************/

/*==========================================================================
 * 走过的弯路（留着免得下次再犯）
 *
 * 1. `hidRptMap_t` 没有 `state` 字段，也不是给调用方填的：
 *    { handle, cccdHandle, id, type, mode }，句柄由服务内部回填。
 *    正确做法是不碰它，让 Hid_AddService() 自己建表。
 * 2. `hidDevCB_t` 的字段名是 reportCB / evtCB / passcodeCB / pfnStateChange，
 *    不是 pfnHidRptCB 之类 —— 猜名字会得到一个字段全零的回调表。
 * 3. `evtCB` 的签名是 `void (*)(uint8_t evt)`，不是 hidDevEvt_e。
 * 4. `HID_RPT_ID_KEY_IN` 是 **0**（不是 1），在 hidkbdservice.h 里。
 * 5. 报告长度宏叫 `HID_KEYBOARD_IN_RPT_LEN`（= 8），不在 hiddev.h/hidkbdservice.h
 *    里，而在例程的 APP 层 —— 所以我们这里用 bt_gatt.h 自己的定义，
 *    并在 bt_gatt.h 里写明它必须等于 UIK_HID_REPORT_LEN。
 *========================================================================*/
