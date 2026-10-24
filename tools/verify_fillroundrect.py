# -*- coding: gbk -*-
# 按 tft_gfx.c 的算法逐行模拟，验证圆角填充
W = H = 76
R = 15
bm = [[0]*W for _ in range(H)]

def fill_rect(x, y, w, h):
    # 对应 TFT_FillRect（带裁剪）
    if w <= 0 or h <= 0: return
    if x >= W or y >= H: return
    if x < 0: w += x; x = 0
    if y < 0: h += y; y = 0
    if x + w > W: w = W - x
    if y + h > H: h = H - y
    if w <= 0 or h <= 0: return
    for j in range(y, y+h):
        for i in range(x, x+w):
            bm[j][i] = 1

def fast_vline(x, y, h):
    # 对应 TFT_DrawFastVLine（带裁剪）
    if x < 0 or x >= W or h <= 0: return
    if y < 0: h += y; y = 0
    if y + h > H: h = H - y
    if h <= 0: return
    for j in range(y, y+h):
        bm[j][x] = 1

def fill_circle_helper(x0, y0, r, cornername, delta):
    # 完全照抄 tft_gfx.c：只有 0x1 / 0x2 两个分支
    f = 1 - r; ddF_x = 1; ddF_y = -2*r; x = 0; y = r
    while x < y:
        if f >= 0:
            y -= 1; ddF_y += 2; f += ddF_y
        x += 1; ddF_x += 2; f += ddF_x
        if cornername & 0x1:
            fast_vline(x0 + x, y0 - y, 2*y + 1 + delta)
            fast_vline(x0 + y, y0 - x, 2*x + 1 + delta)
        if cornername & 0x2:
            fast_vline(x0 - x, y0 - y, 2*y + 1 + delta)
            fast_vline(x0 - y, y0 - x, 2*x + 1 + delta)

def round_old(x, y, w, h, r):
    fill_rect(x + r, y, w - 2*r, h)
    fill_rect(x, y + r, w, h - 2*r)
    fill_circle_helper(x + r, y + r, r, 1, 0)
    fill_circle_helper(x + w - r - 1, y + r, r, 2, 0)
    fill_circle_helper(x + w - r - 1, y + h - r - 1, r, 4, 0)
    fill_circle_helper(x + r, y + h - r - 1, r, 8, 0)

def round_new(x, y, w, h, r):
    fill_rect(x + r, y, w - 2*r, h)
    fill_rect(x, y + r, w, h - 2*r)
    delta = h - 2*r - 1
    fill_circle_helper(x + r, y + r, r, 2, delta)          # 左
    fill_circle_helper(x + w - r - 1, y + r, r, 1, delta)  # 右

def show(title, fn):
    for j in range(H): 
        for i in range(W): bm[j][i] = 0
    fn(0, 0, W, H, R)
    print('='*40)
    print(title)
    print('='*40)
    for j in range(0, H, 2):          # 隔行打印，省点篇幅
        print(''.join('#' if bm[j][i] else '.' for i in range(0, W, 1)))
    # 统计四个角 r x r 区域里被填的比例
    def corner(ox, oy):
        n = sum(bm[oy+j][ox+i] for j in range(R) for i in range(R))
        return n
    print()
    print('四个角 %dx%d 区域填充像素数(应约为内切圆面积 %d):' % (R, R, int(3.14159*(R-1)**2/4)))
    print('  左上=%d  右上=%d  左下=%d  右下=%d' % (
        corner(0,0), corner(W-R,0), corner(0,H-R), corner(W-R,H-R)))

show('【修复前】round_old', round_old)
show('【修复后】round_new', round_new)
