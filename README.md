# CH592X 多功能键盘固件

基于 WCH **CH592X**（RISC-V，BLE 5.x）的多功能键盘 / 桌面小终端固件。集成 3×3 矩阵键盘、旋转编码器、ST7789 彩屏 UI、BLE HID 键盘、2.4G 私有协议、小游戏、RGB 灯效与电池管理。

## 主要功能

- **输入**：3×3 矩阵键盘、旋转编码器，支持组合键与多组按键映射
- **显示**：ST7789 屏幕、分页 / 卡片式 UI、中英文点阵字库、图标与动画
- **连接**
  - BLE HID 键盘（WCH BLE 协议栈，支持多主机记录与切换）
  - 2.4G 私有协议（RF24 兼容收发、跳频、配对、遥控）
  - USB HID 键盘 / CDC 串口
- **扩展**：2048、Runner、Thunder 等小游戏；在线键位映射；RGB 键盘灯；电池电压 / 电量显示
- **低功耗**：空闲休眠、射频角色仲裁、BLE / 2.4G 共用射频

## 目录结构

| 目录 | 说明 |
|------|------|
| `User/` | 应用入口 `Main.c`，整合 UI、键盘、BLE、2.4G |
| `hardware/` | ST7789、图形库、矩阵键盘、编码器、电池、RGB 灯驱动 |
| `bluetooth/` | WCH BLE 协议栈适配、HID、设备扫描与连接管理 |
| `keyboard/` | USB HID、USB CDC、在线键位映射、宏 |
| `rf24/` | 2.4G 私有协议、跳频、TDMA、键位与遥控协议 |
| `ui/` | 页面、卡片、列表、图标、字库与主题 |
| `games/` | 游戏公共层与各小游戏 |
| `tools/` | 字库生成、键位映射网页、校验与构建脚本 |
| `Ld/ Startup/ RVMSIS/ StdPeriphDriver/` | WCH 官方启动文件、链接脚本与外设库 |

## 构建

### MounRiver Studio

直接导入仓库根目录的 `.project` / `.cproject`，选择 **CH592X** 目标编译，产物默认输出到 `obj/CH592X.hex`。

### 命令行（可选）

使用 MounRiver Studio 自带的 RISC-V GCC 工具链与 `mingw32-make`：

```bash
cd obj
mingw32-make --no-print-directory main-build
```

仓库内提供两份可直接烧录的固件：`CH592X_FW52.hex`、`CH592X_FIX_20261019.hex`；`官方参考hex/` 为 WCH 官方示例固件。

## 源码编码说明

本工程历史较长，**源码编码并不统一**：`User/`、`ui/`、`games/`、`rf24/`、`hardware/` 等目录以 **GBK** 为主，`keyboard/` 目录内 GBK 与 UTF-8 混用。直接以 UTF-8 保存中文注释会破坏原有编码。

修改 GBK 文件前请先确认编码，或使用 `tools/gbk_edit.py`（按原编码读写并自动生成 `.bak-时间戳` 备份）。文档中记录了各目录的实测编码情况，详见 [`CH592X_交接说明.md`](CH592X_交接说明.md)。

## 文档

- [`工程整理说明.md`](工程整理说明.md)：分层结构、模块 API、瘦身建议
- [`CH592X_交接说明.md`](CH592X_交接说明.md)：构建、编码、已修复问题与硬件参数
- [`CH592X_操作流程图与结果.md`](CH592X_操作流程图与结果.md)：交互流程与验证记录
- [`CH592X_Bug修复与优化方案.md`](CH592X_Bug修复与优化方案.md)：问题分析与优化方案
- 各子目录下的 `README_*.md`：蓝牙、USB 键盘、2.4G、UI、游戏模块说明

## 说明

仓库为个人硬件开发工程，包含开发过程中的分析文档与测试脚本。提交前已排除 `obj/`、`obj_cmdline/`、`.mrs/` 编译产物、旧副本 `ch592x/ch592x/` 以及编辑器备份文件。
