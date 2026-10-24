# -*- coding: gbk -*-
"""用字形数据生成 show_widget 用的 SVG（键盘页左侧显示区效果）"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import design_kbdfont as D

CAPU = 100
def px(u, cap):  return (u*cap + CAPU//2)//CAPU
def width(s, cap): return (sum(D.G[c][0] for c in s if c in D.G)*cap + CAPU//2)//CAPU
def boxh(cap): return (cap*128 + CAPU//2)//CAPU
def thick(cap): return max(2, min(6, (cap+12)//14))

def polys(s, cap, col):
    out, pen = [], 0
    for ch in s:
        if ch not in D.G:
            continue
        adv, ps = D.G[ch]
        for p in ps:
            pts = [(px(pen+p[2*k], cap), px(p[2*k+1], cap)) for k in range(len(p)//2)]
            d = ' '.join('%d,%d' % q for q in pts)
            out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%d" '
                       'stroke-linecap="round" stroke-linejoin="round"/>' % (d, col, thick(cap)))
        pen += adv
    return ''.join(out)

S = 1.5                    # 区域缩放
PW, PH = 189*S, 115*S
BG = '#0B1A2B'

parts = []
# ---- 面板 A：单键 Enter（cap=50，整行高 64）----
ax, ay = 30, 52
tx = (189 - width('Enter', 50))//2
ty = (115 - boxh(50))//2
parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" stroke="#888780" stroke-width="0.5"/>'
             % (ax, ay, PW, PH, BG))
parts.append('<g transform="translate(%.1f,%.1f) scale(%.2f)">%s</g>'
             % (ax + tx*S, ay + ty*S, S, polys('Enter', 50, '#FFFFFF')))

# ---- 面板 B：三排组合键（cap=25，每排整行高 32）----
bx, by = 30 + PW + 34, 52
parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" stroke="#888780" stroke-width="0.5"/>'
             % (bx, by, PW, PH, BG))
rows = ['LCtrl', 'LAlt', 'Del']
rh = boxh(25); total = len(rows)*rh + (len(rows)-1)*2
y0 = (115 - total)//2
for i, r in enumerate(rows):
    tx = (189 - width(r, 25))//2
    ty = y0 + i*(rh+2)
    parts.append('<g transform="translate(%.1f,%.1f) scale(%.2f)">%s</g>'
                 % (bx + tx*S, by + ty*S, S, polys(r, 25, '#FFFFFF')))

svg = []
svg.append('<svg viewBox="0 0 680 300" width="100%%" role="img" xmlns="http://www.w3.org/2000/svg">')
svg.append('<title>键盘页左侧键值显示效果</title>')
svg.append('<desc>左：单个按键 Enter 用 64px 大字；右：三个组合键分三排 32px。</desc>')
svg.append('<text x="30" y="26" font-family="sans-serif" font-size="14" font-weight="500" fill="#2C2C2A">'
           '键盘页左侧键值区（189 × 115）— 专用笔画字体</text>')
svg.append('<text x="%.1f" y="44" font-family="sans-serif" font-size="12" fill="#5F5E5A">'
           '单个键 · 整行高 64px（cap=50，笔画 4px）</text>' % ax)
svg.append('<text x="%.1f" y="44" font-family="sans-serif" font-size="12" fill="#5F5E5A">'
           '组合键 · 三排 · 每排 32px（cap=25，笔画 2px）</text>' % bx)
svg.extend(parts)
svg.append('<text x="30" y="%.1f" font-family="sans-serif" font-size="13" fill="#2C2C2A">'
           '① 矢量笔画字体：每个字形是折线，同一份数据在 64px / 32px 两档都是等宽笔画，不是把小点阵放大</text>' % (by + PH + 34))
svg.append('<text x="30" y="%.1f" font-family="sans-serif" font-size="13" fill="#2C2C2A">'
           '② 字形集 60 个（A-Z a-w 0-9 + - ?），覆盖 UiKbd_UsageName() 全部键名；数据 1068 字节</text>' % (by + PH + 58))
svg.append('<text x="30" y="%.1f" font-family="sans-serif" font-size="13" fill="#2C2C2A">'
           '③ 横向直接 TFT_FillRect（一次开窗刷一行），只有斜笔画才逐像素；过宽时按像素宽度自动压 cap</text>' % (by + PH + 82))
svg.append('<text x="30" y="%.1f" font-family="sans-serif" font-size="13" fill="#5F5E5A">'
           '④ 字形设计器 tools/design_kbdfont.py，数据生成 tools/gen_kbdfont.py（改字形后重跑即可）</text>' % (by + PH + 106))
svg.append('</svg>')

print('\n'.join(svg))
