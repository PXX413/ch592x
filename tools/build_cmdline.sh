#!/usr/bin/env bash
#==============================================================================
# tools/build_cmdline.sh —— 不打开 MounRiver Studio 也能命令行编译本工程
#
# 为什么要有这个：
#   MRS 是 GUI，构建一次要点好几下；而这个工程改一行就要验一次。
#   脚本用工程里已有的 .cproject 参数（-march=rv32imac -mabi=ilp32 -Os ...）
#   直接调 riscv-none-embed-gcc，产物写到 obj_cmdline/，**不碰 obj/**，
#   所以不会和 IDE 的增量构建互相干扰。
#
# 用法：
#   bash tools/build_cmdline.sh                     # 用默认工具链路径
#   TC=/path/to/bin bash tools/build_cmdline.sh     # 指定工具链 bin 目录
#   OUT=/tmp/mybuild bash tools/build_cmdline.sh    # 指定输出目录
#
# 工具链从哪来：
#   MRS2 里自带的压缩包
#     <MRS>/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC.7z
#   用同一目录下的 7za.exe 解开，bin/ 里就是 riscv-none-embed-gcc。
#==============================================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
TC="${TC:-/c/Users/a/AppData/Local/Temp/wchtc/bin}"
OUT="${OUT:-$PROJ/obj_cmdline}"

CC="$TC/riscv-none-embed-gcc"
OC="$TC/riscv-none-embed-objcopy"
SZ="$TC/riscv-none-embed-size"

if [ ! -x "$CC" ]; then
    echo "找不到编译器：$CC"
    echo "用 TC=<工具链 bin 目录> 指定，或先解压 MRS 自带的 RISC-V Embedded GCC.7z"
    exit 1
fi

ARCH="-march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore"
INCS="-I$PROJ/StdPeriphDriver/inc -I$PROJ/RVMSIS -I$PROJ/hardware -I$PROJ/games \
-I$PROJ/rf24 -I$PROJ/keyboard -I$PROJ/ui -I$PROJ/bluetooth -I$PROJ/User"

# 和 .cproject 的 release 配置保持一致（注意有 -DDEBUG=1，PRINT 才会被编译进去）
CFLAGS="$ARCH -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections \
-fdata-sections -fno-common -Wunused -Wuninitialized -g -gdwarf-4 $INCS -std=gnu99 -DDEBUG=1"
ASFLAGS="$ARCH -g -gdwarf-4"

LDFLAGS="$ARCH -Os -fsigned-char -ffunction-sections -fdata-sections -fno-common \
-g -gdwarf-4 -T $PROJ/Ld/Link.ld -nostartfiles -Xlinker --gc-sections \
-L$PROJ -L$PROJ/StdPeriphDriver -L$PROJ/bluetooth -Xlinker --print-memory-usage \
-Wl,-Map,$OUT/CH592X.map --specs=nano.specs --specs=nosys.specs"

mkdir -p "$OUT"
cd "$PROJ" || exit 1

OBJS=()
FAILED=0
mapfile -t SRC < <(find RVMSIS Startup StdPeriphDriver User bluetooth games hardware \
                        keyboard rf24 ui -maxdepth 1 -type f \
                        \( -name '*.c' -o -name '*.S' \) | sort)

for src in "${SRC[@]}"; do
    obj="$OUT/${src//\//_}.o"
    case "$src" in
        *.c) $CC $CFLAGS   -c "$src" -o "$obj" || { echo "FAIL: $src"; FAILED=1; } ;;
        *.S) $CC $ASFLAGS  -c "$src" -o "$obj" || { echo "FAIL: $src"; FAILED=1; } ;;
    esac
    OBJS+=("$obj")
done

if [ "$FAILED" -ne 0 ]; then
    echo "=== 有编译错误，未链接 ==="
    exit 1
fi

$CC $LDFLAGS -o "$OUT/CH592X.elf" "${OBJS[@]}" -lISP592 -lCH59xBLE || exit 1
$OC -O binary "$OUT/CH592X.elf" "$OUT/CH592X.bin"
$OC -O ihex   "$OUT/CH592X.elf" "$OUT/CH592X.hex"
$SZ --format=berkeley "$OUT/CH592X.elf"
echo "=== OK -> $OUT/CH592X.hex ==="
