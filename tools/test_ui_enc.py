#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
编码器点击分类器自测（PC 上跑，不用真机）

验证 "单击 / 双击 / 长按 0.5s / 长按 1s / 长按 3s" 五档的时间和优先级：

    * 单击要等 300ms 双击窗口过期才吐
    * 0.5s / 1s / 3s 三档各只吐一次（页面各接各的）
    * 吐过任何一档长按之后，松手不再补 CLICK
    * 事件 -> GameKey 编号对得上（HOLD1 = 17，GKEY_COUNT = 18）

本脚本直接编译 ui/ui_enc.c（真源码），game_port.h 用桩件：
桩里的 GKEY 编号从真实的 games/game_port.h 里抠出来，顺便校验没被改过。

跑法（工程根目录）：
    python tools/test_ui_enc.py
"""

import io
import os
import subprocess
import sys
import tempfile

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENC_C  = os.path.join(ROOT, 'ui', 'ui_enc.c')
PORT_H = os.path.join(ROOT, 'games', 'game_port.h')
LF     = chr(10)

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


# 期望的编号（改 game_port.h 就要同步这里，等于把 ABI 钉死）
EXPECT = [('GKEY_NONE', 0), ('GKEY_ENC_CLICK', 13), ('GKEY_ENC_DBL', 14),
          ('GKEY_ENC_HOLD', 15), ('GKEY_ENC_HOLD3', 16),
          ('GKEY_ENC_HOLD1', 17), ('GKEY_COUNT', 18)]


def parse_gkeys():
    """从真实的 game_port.h 里抠出枚举值（逐行找，不用正则转义）"""
    txt = io.open(PORT_H, encoding='gbk', errors='replace').read()
    vals = {}
    for name, _want in EXPECT:
        val = None
        for line in txt.split(LF):
            s = line.strip()
            if s.startswith(name):
                rest = s[len(name):]
                j = rest.find('=')
                if j < 0:
                    continue
                digits = ''
                for ch in rest[j + 1:]:
                    if ch.isdigit():
                        digits += ch
                    elif digits:
                        break
                if digits:
                    val = int(digits)
                break
        vals[name] = val
    return vals


STUB_H = '''#ifndef __GAME_PORT_H__
#define __GAME_PORT_H__
/* 桩件：只给 ui_enc.c 提供事件编号，值从真实 game_port.h 里抠出来 */
#define GKEY_NONE        %(GKEY_NONE)d
#define GKEY_ENC_CLICK   %(GKEY_ENC_CLICK)d
#define GKEY_ENC_DBL     %(GKEY_ENC_DBL)d
#define GKEY_ENC_HOLD    %(GKEY_ENC_HOLD)d
#define GKEY_ENC_HOLD3   %(GKEY_ENC_HOLD3)d
#define GKEY_ENC_HOLD1   %(GKEY_ENC_HOLD1)d
#define GKEY_COUNT       %(GKEY_COUNT)d
#endif
'''

MAIN_C = '''#include <stdio.h>
#include "ui_enc.h"
#include "game_port.h"   /* 桩件：只取 GKEY_ENC_* 编号 */

static ui_enc_t e;
static uint32_t now;
static int      fails;

typedef struct { uint32_t t; ui_enc_evt_t e; } rec_t;
static rec_t log_buf[16];
static int   log_n;

static void nl(void) { putchar(10); }

static void check(const char *name, int cond)
{
    if (cond) { printf("PASS %s", name); nl(); }
    else      { fails++; printf("FAIL %s", name); nl(); }
}

static ui_enc_evt_t ff(uint8_t pressed)
{
    now++;
    return UiEnc_Step(&e, pressed, now);
}

static void rec(ui_enc_evt_t ev)
{
    if ((ev != UI_ENC_NONE) && (log_n < 16)) { log_buf[log_n].t = now; log_buf[log_n].e = ev; log_n++; }
}

static void begin(void)
{
    log_n = 0;
    now   = 0;
    UiEnc_Reset(&e);
    (void)UiEnc_Step(&e, 0u, now);          /* 第一次只对齐电平 */
}

/* 按住 hold_ms 毫秒再松开 after_ms 毫秒；返回按下时刻 */
static uint32_t run(uint32_t hold_ms, uint32_t after_ms)
{
    uint32_t i, t0;

    begin();
    (void)ff(1u);                           /* 按下（ff 返回的是事件，不是时间） */
    t0 = now;                               /* 按下时刻 */
    for (i = 1u; i < hold_ms; i++) rec(ff(1u));
    for (i = 0u; i < after_ms; i++) rec(ff(0u));
    return t0;
}

int main(void)
{
    uint32_t i, t0;
    ui_enc_evt_t ev;

    /* ---------- 单击：松开后 300ms 才吐 ---------- */
    begin();
    (void)ff(1u);
    for (i = 1u; i < 100u; i++) (void)ff(1u);      /* 按住 100ms（不够 HOLD） */
    ev = ff(0u);                                   /* 松开边沿：开始等第二击 */
    check("单击：松手当拍不吐 CLICK", ev == UI_ENC_NONE);
    for (i = 0u; i < 299u; i++) ev = ff(0u);       /* 再等 299ms */
    check("单击：300ms 窗口没过不吐", ev == UI_ENC_NONE);
    ev = ff(0u);                                   /* 第 300ms */
    check("单击：300ms 后吐 CLICK", ev == UI_ENC_CLICK);

    /* ---------- 双击 ---------- */
    begin();
    for (i = 0u; i < 80u; i++) (void)ff(1u);       /* 第一击 80ms */
    for (i = 0u; i < 20u; i++) (void)ff(0u);       /* 松手 20ms（窗口内） */
    ev = ff(1u);                                   /* 第二击按下 */
    check("双击：300ms 内第二击吐 DOUBLE", ev == UI_ENC_DOUBLE);
    ev = UI_ENC_NONE;
    for (i = 0u; i < 400u; i++) ev = ff(0u);
    check("双击：松手后不再补 CLICK", ev == UI_ENC_NONE);

    /* ---------- 长按 0.7s：只有 HOLD ---------- */
    t0 = run(710u, 400u);
    check("长按 0.71s：只吐一次，且是 0.5s 的 HOLD",
          (log_n == 1) && (log_buf[0].e == UI_ENC_HOLD) &&
          (log_buf[0].t == t0 + 500u));
    check("长按 0.71s：松手不补 CLICK", log_n == 1);

    /* ---------- 长按 1.01s：HOLD + HOLD1 ---------- */
    t0 = run(1010u, 0u);
    check("长按 1.01s：0.5s 吐 HOLD",
          (log_n >= 1) && (log_buf[0].e == UI_ENC_HOLD) &&
          (log_buf[0].t == t0 + 500u));
    check("长按 1.01s：1.0s 吐 HOLD1",
          (log_n >= 2) && (log_buf[1].e == UI_ENC_HOLD1) &&
          (log_buf[1].t == t0 + 1000u));
    check("长按 1.01s：一共只吐两档（不是三档）", log_n == 2);

    /* ---------- 长按 3.01s：三档都吐 ---------- */
    t0 = run(3010u, 0u);
    check("长按 3.01s：0.5s / 1.0s / 3.0s 各一档",
          (log_n == 3) &&
          (log_buf[0].e == UI_ENC_HOLD)  && (log_buf[0].t == t0 + 500u) &&
          (log_buf[1].e == UI_ENC_HOLD1) && (log_buf[1].t == t0 + 1000u) &&
          (log_buf[2].e == UI_ENC_HOLD3) && (log_buf[2].t == t0 + 3000u));

    /* ---------- 事件 -> GameKey 编号 ---------- */
    check("HOLD1  -> GKEY_ENC_HOLD1",  UiEnc_ToGameKey(UI_ENC_HOLD1)  == (uint8_t)GKEY_ENC_HOLD1);
    check("HOLD   -> GKEY_ENC_HOLD",   UiEnc_ToGameKey(UI_ENC_HOLD)   == (uint8_t)GKEY_ENC_HOLD);
    check("HOLD3  -> GKEY_ENC_HOLD3",  UiEnc_ToGameKey(UI_ENC_HOLD3)  == (uint8_t)GKEY_ENC_HOLD3);
    check("CLICK  -> GKEY_ENC_CLICK",  UiEnc_ToGameKey(UI_ENC_CLICK)  == (uint8_t)GKEY_ENC_CLICK);
    check("DOUBLE -> GKEY_ENC_DBL",    UiEnc_ToGameKey(UI_ENC_DOUBLE) == (uint8_t)GKEY_ENC_DBL);
    check("NONE   -> GKEY_NONE",       UiEnc_ToGameKey(UI_ENC_NONE)   == (uint8_t)GKEY_NONE);

    printf("fails=%d", fails); nl();
    return fails != 0;
}
'''


def main():
    print('=' * 62)
    print(' 编码器点击分类器自测（编译 ui_enc.c 真源码）')
    print('=' * 62)

    if not os.path.isfile(ENC_C):
        print('  [FAIL] 找不到 ' + ENC_C)
        return 1

    vals = parse_gkeys()
    for name, want in EXPECT:
        got = vals.get(name)
        check(name + ' = ' + str(want), got == want, 'fw=' + str(got))

    tmp    = tempfile.mkdtemp(prefix='enctest_')
    stub   = os.path.join(tmp, 'game_port.h')
    mainc  = os.path.join(tmp, 'test_main.c')
    exe    = os.path.join(tmp, 'test_ui_enc' + ('.exe' if os.name == 'nt' else ''))
    io.open(stub, 'w', encoding='utf-8').write(STUB_H % vals)
    io.open(mainc, 'w', encoding='utf-8').write(MAIN_C)

    cc = os.environ.get('CC', 'gcc')
    try:
        r = subprocess.run([cc, '-std=gnu99', '-Wall', '-Wextra', '-o', exe,
                            ENC_C, mainc, '-I', tmp, '-I', os.path.join(ROOT, 'ui')],
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

    # ---------- 静态接线检查：这几处是这次一起修的问题，别再退化 ----------
    main_c = io.open(os.path.join(ROOT, 'User', 'Main.c'),
                     encoding='gbk', errors='replace').read()
    port_c = io.open(os.path.join(ROOT, 'games', 'game_port.c'),
                     encoding='gbk', errors='replace').read()
    kbd_c  = io.open(os.path.join(ROOT, 'ui', 'ui_kbd.c'),
                     encoding='gbk', errors='replace').read()

    check('编码器只在 TMR0 中断里扫（Main.c）', 'ENC_Scan();' in main_c)
    check('game_port.c 不再扫编码器（不再双扫）', 'ENC_Scan();' not in port_c)
    check('game_port 的毫秒走系统时基 g_ms_tick', 'g_ms_tick' in port_c)
    check('键盘页用 GKEY_ENC_HOLD1 进设置页', 'GKEY_ENC_HOLD1' in kbd_c)
    check('键盘页单击走 UiKbd_PulseExtra（额外键）', 'UiKbd_PulseExtra' in kbd_c)

    print('')
    print('=' * 62)
    print(' 结果：PASS=' + str(PASS) + '  FAIL=' + str(FAIL))
    print('=' * 62)
    return 0 if FAIL == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
