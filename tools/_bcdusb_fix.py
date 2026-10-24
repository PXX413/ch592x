#!/usr/bin/env python3
# -*- coding: gbk -*-
"""bcdUSB 0x0110 -> 0x0200（有 IAD 的复合设备必须是 USB2.0）。走 gbk_edit 保证编码安全。"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gbk_edit import Ed

JOBS = [
    # (文件, 旧行, 新行)   —— 逐字节字面量替换，命中必须唯一
    ('keyboard/usb_desc_cdc.c',
     '    0x10, 0x01,                 /*  2 bcdUSB            = 0x0110 (LE) USB1.1  */',
     '    0x00, 0x02,                 /*  2 bcdUSB            = 0x0200 (LE) USB2.0：有 IAD 的复合设备必须 >=0x0200 */'),
    ('keyboard/usb_desc.c',
     '    0x10, 0x01,                 /*  2 bcdUSB             = 0x0110 (LE) USB1.1  */',
     '    0x00, 0x02,                 /*  2 bcdUSB             = 0x0200 (LE) USB2.0 */'),
]

for path, old, new in JOBS:
    e = Ed(path)
    e.sub(old, new)          # 命中数 != 1 直接抛异常
    print(e.save())
    s = open(path, 'rb').read()
    enc = 'utf-8'
    try:
        s.decode('utf-8')
    except UnicodeDecodeError:
        enc = 'gbk'
    txt = s.decode(enc)
    assert '0x0200 (LE) USB2.0' in txt, path
    assert 'USB1.1' not in txt.split('bcdUSB')[1][:40], path
    print('  ok %s (enc=%s, bcdUSB 已改)' % (path, enc))
