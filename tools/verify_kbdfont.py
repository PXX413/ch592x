# -*- coding: gbk -*-
"""按 ui_kbd_font.c 的算法复刻渲染，验证真实字号下的效果与宽度"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import design_kbdfont as D

CAPU, BOXU = 100, 128

def px(u, cap):  return (u * cap + CAPU // 2) // CAPU
def boxh(cap):   return (cap * BOXU + CAPU // 2) // CAPU
def thick(cap):  return max(2, min(6, (cap + 12) // 14))

def width(s, cap):
    return (sum(D.G[c][0] for c in s if c in D.G) * cap + CAPU // 2) // CAPU

def draw(bm, W, H, x, y, s, cap, fg=1):
    t = thick(cap)
    ox = t // 2
    oy = t - 1 - ox
    def fill(ax, ay, w, h):
        for j in range(max(0,ay), min(H, ay+h)):
            for i in range(max(0,ax), min(W, ax+w)):
                bm[j][i] = fg
    def line(ax, ay, bx, by):
        dx=abs(bx-ax); dy=-abs(by-ay)
        sx=1 if ax<bx else -1; sy=1 if ay<by else -1
        err=dx+dy
        while True:
            if 0<=ay<H and 0<=ax<W: bm[ay][ax]=fg
            if ax==bx and ay==by: break
            e2=2*err
            if e2>=dy: err+=dy; ax+=sx
            if e2<=dx: err+=dx; ay+=sy
    pen = x
    for ch in s:
        if ch not in D.G: continue
        adv, polys = D.G[ch]
        for p in polys:
            pts = [(pen + px(p[2*k], cap), y + px(p[2*k+1], cap)) for k in range(len(p)//2)]
            for k in range(len(pts)-1):
                ax,ay = pts[k]; bx,by = pts[k+1]
                dx=abs(bx-ax); dy=abs(by-ay)
                if dy <= 1:
                    x0=min(ax,bx); fill(x0-ox, ay-ox, dx+t, t)
                elif dx <= 1:
                    y0=min(ay,by); fill(ax-ox, y0-ox, t, dy+t)
                elif dx >= dy:
                    for i in range(t): line(ax, ay+i-oy, bx, by+i-oy)
                else:
                    for i in range(t): line(ax+i-oy, ay, bx+i-oy, by)
        pen += px(adv, cap)
    return pen

def show(title, s, cap, region_w=189):
    w = width(s, cap); h = boxh(cap)
    x = max(0, (region_w - w)//2)
    W = region_w; H = h + 4
    bm = [[0]*W for _ in range(H)]
    draw(bm, W, H, x, 2, s, cap)
    print('='*70)
    print('%s   文本="%s"  cap=%d  笔画粗=%d  整行高=%dpx  宽=%dpx  区域宽=%d -> %s'
          % (title, s, cap, thick(cap), h, w, region_w, '放得下' if w<=region_w else '放不下!'))
    print('='*70)
    for j in range(H):
        print(''.join('#' if v else '.' for v in bm[j]))

show('单键（目标整行高 64）', 'Enter', 50)
show('单键（目标整行高 64）', 'A', 50)
show('组合键每排（目标整行高 32）', 'LCtrl', 25)
