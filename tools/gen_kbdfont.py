# -*- coding: gbk -*-
"""从 design_kbdfont.py 的字形数据生成 C 数据头 ui/ui_kbd_font_data.h"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import design_kbdfont as D

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ui', 'ui_kbd_font_data.h')

CHARS = sorted(D.G.keys(), key=lambda c: ord(c))
FIRST, LAST = 32, 126

strokes, off = [], {}
for ch in CHARS:
    off[ch] = len(strokes)
    adv, polys = D.G[ch]
    for p in polys:
        n = len(p) // 2
        strokes.append(n)
        for k in range(n):
            strokes.append(p[2*k]); strokes.append(p[2*k+1])
    strokes.append(0)

lut = [(CHARS.index(chr(c)) if chr(c) in D.G else 255) for c in range(FIRST, LAST+1)]

L = []
L.append('/**************************************************************************')
L.append(' * ui_kbd_font_data.h —— 键盘页大字号「笔画字体」数据（自动生成，不要手改）')
L.append(' *')
L.append(' * 生成器：tools/gen_kbdfont.py（字形设计在 tools/design_kbdfont.py）')
L.append(' *')
L.append(' * 坐标系：x 0..56，y 0(顶)..100(基线)，下延到 128；小写字身顶 = 34')
L.append(' *         大写高度 = 100 个单位，绘制时按 cap 缩放。')
L.append(' *')
L.append(' * 每个字形的数据 = 若干条折线，每条折线格式为：')
L.append(' *        点数 n, 然后 n 组 (x, y)')
L.append(' *     整个字形以一个 0 结尾。')
L.append(' **************************************************************************/')
L.append('')
L.append('#ifndef __UI_KBD_FONT_DATA_H__')
L.append('#define __UI_KBD_FONT_DATA_H__')
L.append('')
L.append('#include <stdint.h>')
L.append('')
L.append('/* 笔画坐标表 */')
L.append('static const uint8_t s_strokes[] =')
L.append('{')
row = []
for v in strokes:
    row.append('%d,' % v)
    if len(row) == 16:
        L.append('    ' + ' '.join(row)); row = []
if row: L.append('    ' + ' '.join(row))
L.append('};')
L.append('')
L.append('/* 字形表：数据偏移 + 前进宽度（单位同坐标系） */')
L.append('typedef struct { uint16_t off; uint8_t adv; } kbdf_glyph_t;')
L.append('')
L.append('static const kbdf_glyph_t s_glyphs[] =')
L.append('{')
for ch in CHARS:
    L.append('    { %5d, %3d },   /* %s */' % (off[ch], D.G[ch][0], ch if ch != ' ' else 'SP'))
L.append('};')
L.append('')
L.append('#define KBDF_NGLYPH    %du' % len(CHARS))
L.append('#define KBDF_FIRST     %du' % FIRST)
L.append('#define KBDF_LAST      %du' % LAST)
L.append('#define KBDF_NONE      0xFFu')
L.append('')
L.append('/* ASCII -> 字形下标；KBDF_NONE = 没收录 */')
L.append('static const uint8_t s_lut[KBDF_LAST - KBDF_FIRST + 1u] =')
L.append('{')
for i in range(0, len(lut), 16):
    L.append('    ' + ' '.join(('%3d,' % v) for v in lut[i:i+16]))
L.append('};')
L.append('')
L.append('#endif /* __UI_KBD_FONT_DATA_H__ */')
L.append('')

txt = '\n'.join(L)
# 工程约定：源文件用 GBK
open(OUT, 'wb').write(txt.encode('gbk'))
print('已生成 %s' % os.path.normpath(OUT))
print('  字形 %d 个，坐标数据 %d 字节，表 %d 项' % (len(CHARS), len(strokes), len(lut)))
