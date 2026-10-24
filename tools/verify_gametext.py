# -*- coding: gbk -*-
"""
game_text 回读校验：把生成好的字库反解回字符串，逐条和 font_texts.txt 比对。

【为什么需要它】
font_gen.ps1 是按"字符首次出现顺序"给字形编号的。所以只要 font_texts.txt 里
任何一条文本变了（哪怕只是把一个词挪了位置），后面的字形索引就可能整体位移
—— game_font16.c / game_text.c 必须重新生成，而且很容易串位。

这个脚本就是防串位的：把 game_text.c 里的索引序列译回汉字，和源文本逐条比对。
改完 font_texts.txt 并重跑 font_gen.ps1 之后跑一下它。

用法：python tools/verify_gametext.py
退出码 0 = 全部一致；1 = 有差异（逐条打印期望/实得）。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P_TEXT = os.path.join(ROOT, 'hardware', 'font_texts.txt')
P_GEN = os.path.join(ROOT, 'hardware', 'game_text.c')

RE_MAP = r'(s_text_\d+)\s*,\s*/\*\s*(GTXT_\w+)\s*\*/'
RE_ARR = r'static const uint8_t (s_text_\d+)\[\d+\]\s*=\s*{([^}]*)}'
RE_CP = r'GF_U([0-9A-Fa-f]{4})'


def main():
    gen = open(P_GEN, 'rb').read().decode('gbk')
    name2arr = dict((m.group(2), m.group(1)) for m in re.finditer(RE_MAP, gen))
    arr2cp = {}
    for m in re.finditer(RE_ARR, gen):
        arr2cp[m.group(1)] = [int(x, 16) for x in re.findall(RE_CP, m.group(2))]

    src = {}
    for line in open(P_TEXT, 'rb').read().decode('gbk').splitlines():
        line = line.strip()
        if line and not line.startswith('#') and '=' in line:
            k, v = line.split('=', 1)
            src[k.strip()] = v.strip()

    if not name2arr or not arr2cp:
        print('!! 解析失败：映射 %d 条、数组 %d 个 —— 生成文件格式变了？'
              % (len(name2arr), len(arr2cp)))
        return 1

    bad = 0
    for name in sorted(name2arr, key=lambda n: int(name2arr[n][7:])):
        cps = arr2cp.get(name2arr[name])
        if cps is None:
            print('  !! %s 找不到数组 %s' % (name, name2arr[name]))
            bad += 1
            continue
        got = ''.join(chr(x) for x in cps)
        exp = src.get(name, '<font_texts.txt 里没有>')
        if got != exp:
            bad += 1
            print('  xx %-18s 期望[%s] 实得[%s]' % (name, exp, got))

    print('回读校验: %d 条，错 %d 条 %s'
          % (len(name2arr), bad, 'OK 全部一致' if bad == 0 else '** 有差异 **'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
