#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
HID 6KRO 报文自测（PC 上跑，不用真机）

"某个物理键映射成 LCtrl / Ctrl+C 之后，发出去的 8 字节报文到底长什么样"
是这次改动里最容易出错的地方：修饰位必须进第 0 字节、不能塞进键数组，
6 个键码槽位满了也不能把修饰位挤掉。

这个脚本把 ui/ui_kbd.c 里的这几个函数**原样抠出来**，配桩用 gcc 编译后跑断言：

    UiKbd_MapValueOf()   UiKbd_UsageModBit()   UiKbd_UsageOf()
    UiKbd_ModsOf()       UiKbd_BuildHid6()

抠的是真源码而不是另抄一份，所以 ui_kbd.c 以后改了，这里会跟着验。
需要 PATH 里有 gcc（MinGW / Linux 都行）；没有就跳过并给出提示。

跑法（工程根目录）：
    python tools/test_hid_report.py
"""

import io
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, 'ui', 'ui_kbd.c')
LF   = chr(10)

PASS = 0
FAIL = 0


def check(name, cond, extra=''):
    global PASS, FAIL
    if cond:
        PASS += 1
        print('  [PASS] ' + name)
    else:
        FAIL += 1
        print('  [FAIL] ' + name + (('  ' + extra) if extra else ''))


def grab(src, sig):
    """按签名抠出一个函数（花括号配对）"""
    i = src.index(sig)
    j = src.index('{', i)
    depth = 0
    k = j
    while True:
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                return src[i:k + 1]
        k += 1


# ---------------------------------------------------------------------------
# 用例表：(说明, {键号: 映射值}, 按下的键位图, 期望的 8 字节报文)
#   修饰键 = HID usage 0xE0~0xE7（设备端键值列表里就是 LCtrl 这些）
#   组合键 = 16 位映射值，高字节 = 修饰位图，低字节 = 主键 usage
# ---------------------------------------------------------------------------
CASES = [
    ('单键 LCtrl 只进 modifiers',      {1: 0x00E0}, 0x0001, [0x01, 0, 0, 0, 0, 0, 0, 0]),
    ('单键 LShft',                     {1: 0x00E1}, 0x0001, [0x02, 0, 0, 0, 0, 0, 0, 0]),
    ('单键 LAlt',                      {1: 0x00E2}, 0x0001, [0x04, 0, 0, 0, 0, 0, 0, 0]),
    ('单键 LGUI',                      {1: 0x00E3}, 0x0001, [0x08, 0, 0, 0, 0, 0, 0, 0]),
    ('单键 RCtrl',                     {1: 0x00E4}, 0x0001, [0x10, 0, 0, 0, 0, 0, 0, 0]),
    ('单键 RShft',                     {1: 0x00E5}, 0x0001, [0x20, 0, 0, 0, 0, 0, 0, 0]),
    ('单键 RAlt',                      {1: 0x00E6}, 0x0001, [0x40, 0, 0, 0, 0, 0, 0, 0]),
    ('单键 RGUI',                      {1: 0x00E7}, 0x0001, [0x80, 0, 0, 0, 0, 0, 0, 0]),
    ('普通键 A 不进 modifiers',        {1: 0x0004}, 0x0001, [0x00, 0, 0x04, 0, 0, 0, 0, 0]),
    ('映射为 0 的键不发',              {1: 0x0000}, 0x0001, [0x00, 0, 0, 0, 0, 0, 0, 0]),
    ('按住 LCtrl 再按 C（两个物理键）', {1: 0x00E0, 2: 0x0006}, 0x0003,
     [0x01, 0, 0x06, 0, 0, 0, 0, 0]),
    ('组合键写法 0x0106 = Ctrl+C',     {1: 0x0106}, 0x0001, [0x01, 0, 0x06, 0, 0, 0, 0, 0]),
    ('只有修饰位 0x0100 = 单 LCtrl',   {1: 0x0100}, 0x0001, [0x01, 0, 0, 0, 0, 0, 0, 0]),
    ('0x0329 = Ctrl+Shift+Esc',        {1: 0x0329}, 0x0001, [0x03, 0, 0x29, 0, 0, 0, 0, 0]),
    ('0x0204 = Shift+A',               {1: 0x0204}, 0x0001, [0x02, 0, 0x04, 0, 0, 0, 0, 0]),
    ('6 键 + 第 7 键 LShft（修饰位不能丢）',
     {1: 0x0004, 2: 0x0005, 3: 0x0006, 4: 0x0007, 5: 0x0008, 6: 0x0009, 7: 0x00E1},
     0x007F, [0x02, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09]),
    ('7 个普通键只报前 6 个（Phantom Key 保护）',
     {1: 0x0004, 2: 0x0005, 3: 0x0006, 4: 0x0007, 5: 0x0008, 6: 0x0009, 7: 0x000A},
     0x007F, [0x00, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09]),
    ('没按任何键 = 全 0 报文',         {}, 0x0000, [0, 0, 0, 0, 0, 0, 0, 0]),
]

C_HEAD = '''#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define UIK_HID_REPORT_LEN 8u

static uint16_t g_val[10];          /* 桩：KEY_GetMapped() 的表 */
uint16_t KEY_GetMapped(uint8_t key)
{
    if (key < 1u || key > 9u) return 0u;
    return g_val[key];
}

static uint8_t s_mods;              /* 桩：UiKbd_SetModifier() 的口子 */
void UiKbd_SetModifier(uint8_t m) { s_mods = m; }

'''

C_TAIL = '''
static int fails;

static void run_case(const char *name, uint16_t keymap, const uint8_t *want)
{
    uint8_t got[8];
    int i;
    UiKbd_BuildHid6(got, keymap);
    if (memcmp(got, want, 8u) != 0)
    {
        fails++;
        printf("FAIL %s got:", name);
        for (i = 0; i < 8; i++) printf(" %02X", got[i]);
        printf(" want:");
        for (i = 0; i < 8; i++) printf(" %02X", want[i]);
        puts("");
    }
    else
    {
        printf("PASS %s", name);
        puts("");
    }
}

int main(void)
{
    uint8_t want[8];

    /* ---------- 用例（本文件顶部 CASES 表生成） ---------- */
<CASES>
    /* ---------- 映射值拆两半：UiKbd_UsageOf / UiKbd_ModsOf ---------- */
    memset(g_val, 0, sizeof(g_val));
    g_val[1] = 0x0106u;                                  /* Ctrl+C */
    if (UiKbd_UsageOf(1) != 0x06u) { fails++; puts("FAIL 0x0106 低字节 != 0x06"); }
    else puts("PASS 0x0106 低字节 = 0x06（主键）");
    if (UiKbd_ModsOf(1) != 0x01u)  { fails++; puts("FAIL 0x0106 高字节 != 0x01"); }
    else puts("PASS 0x0106 高字节 = 0x01（修饰位）");

    memset(g_val, 0, sizeof(g_val));
    g_val[1] = 0x00E4u;                                  /* RCtrl */
    if (UiKbd_UsageOf(1) != 0xE4u) { fails++; puts("FAIL UsageOf(0xE4)"); }
    else puts("PASS UsageOf(0xE4) = 0xE4（屏上显示名用）");
    if (UiKbd_ModsOf(1) != 0x10u)  { fails++; puts("FAIL ModsOf(0xE4) != 0x10"); }
    else puts("PASS ModsOf(0xE4) = 0x10");

    /* ---------- 全局 s_mods 叠加（Main.c 那条口子） ---------- */
    memset(g_val, 0, sizeof(g_val));
    g_val[1] = 0x0006u;                                  /* C */
    UiKbd_SetModifier(0x02u);                            /* 全局 Shift */
    memset(want, 0, 8); want[0] = 0x02; want[2] = 0x06;
    run_case("全局 s_mods 叠加：Shift + C", 0x0001u, want);
    UiKbd_SetModifier(0u);

    printf("fails=%d", fails);
    puts("");
    return fails != 0;
}
'''


def gen_cases():
    out = []
    for n, (name, mapping, keymap, want) in enumerate(CASES):
        out.append('    /* ' + str(n + 1) + '. ' + name + ' */')
        out.append('    memset(g_val, 0, sizeof(g_val));')
        for k in sorted(mapping):
            out.append('    g_val[%du] = 0x%04Xu;' % (k, mapping[k]))
        out.append('    memset(want, 0, 8);')
        for i, b in enumerate(want):
            if b:
                out.append('    want[%du] = 0x%02Xu;' % (i, b))
        out.append('    run_case("%s", 0x%04Xu, want);' % (name, keymap))
        out.append('')
    return LF.join(out)


def main():
    print('=' * 62)
    print(' HID 6KRO 报文自测（从 ui_kbd.c 抠真代码编译）')
    print('=' * 62)

    if not os.path.isfile(SRC):
        print('  [FAIL] 找不到 ' + SRC)
        return 1

    src = io.open(SRC, encoding='gbk', errors='replace').read()
    sigs = ['uint16_t UiKbd_MapValueOf(uint8_t key)',
            'uint8_t UiKbd_UsageModBit(uint8_t usage)',
            'uint8_t UiKbd_UsageOf(uint8_t key)',
            'uint8_t UiKbd_ModsOf(uint8_t key)',
            'uint8_t UiKbd_BuildHid6(uint8_t *out8, uint16_t keymap)']
    funcs = []
    for s in sigs:
        try:
            funcs.append(grab(src, s))
        except ValueError:
            print('  [FAIL] ui_kbd.c 里找不到函数：' + s)
            return 1

    c_src = C_HEAD + (LF + LF).join(funcs) + C_TAIL.replace('<CASES>', gen_cases())

    tmpdir = tempfile.mkdtemp(prefix='hidtest_')
    cpath = os.path.join(tmpdir, 'test_hid6.c')
    exe = os.path.join(tmpdir, 'test_hid6' + ('.exe' if os.name == 'nt' else ''))
    io.open(cpath, 'w', encoding='utf-8').write(c_src)

    cc = os.environ.get('CC', 'gcc')
    try:
        r = subprocess.run([cc, '-std=gnu99', '-Wall', '-Wextra', '-o', exe, cpath],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding='utf-8', errors='replace')
    except OSError:
        print('  [SKIP] 没找到 ' + cc + '，跳过（装个 MinGW，或在 Linux 上跑）')
        return 0

    if r.returncode != 0:
        print('  [FAIL] 编译不过：')
        print(r.stdout)
        return 1

    run = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding='utf-8', errors='replace')
    for line in (run.stdout or '').split(LF):
        line = line.strip()
        if line.startswith('PASS '):
            check(line[5:], True)
        elif line.startswith('FAIL '):
            check(line[5:], False)

    print('')
    print('=' * 62)
    print(' 结果：PASS=' + str(PASS) + '  FAIL=' + str(FAIL))
    print('=' * 62)
    return 0 if FAIL == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
